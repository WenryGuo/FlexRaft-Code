// test_encoding_compare.cc
//
// 三种 multi-raft 编码方案对比测试（与 --encoding=RS_F|RS_3F|LRC 对齐）：
//
//   - RS_F  : RS(F+1, F),       total = 2F+1 = N,   1 frag/node
//   - RS_3F : RS(F+1, 3F+1),    total = 4F+2 = 2N,  2 random frag/node
//   - LRC   : LRC(F+1, 2, r=2N-k-l)
//                              total = 2N,         2 frag/node 正交放置
//
// 覆盖：
//   Section 1 — 参数 sanity（含 LrcParams::FromCapacity 各 N 的输出）
//   Section 2 — 编码正确性：encode → 取最少需要的 frag → DecodeSlice → memcmp
//   Section 3 — 编码耗时：每 (N, size, mode) 跑 T 轮，统计 mean/p50/p95/p99/吞吐
//   Section 4 — 放置算法：RS_F 1/node、RS_3F 2/node 随机、LRC 2/node 正交
//
// 运行：./build/multi-raft/test_encoding_compare
// 失败立即 exit(1)。

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "multi-raft/encoding_mode.h"
#include "multi-raft/lrc_encoder.h"
#include "multi-raft/lrc_placement.h"
#include "multi-raft/message.h"
#include "raft/encoder.h"
#include "raft/log_entry.h"

using namespace multiraft;
using Clock = std::chrono::high_resolution_clock;

// ----------------------------------------------------------------------------
//  小工具
// ----------------------------------------------------------------------------
static void Banner(const char* title) {
  printf("\n========================================================\n");
  printf("[ENC-CMP] %s\n", title);
  printf("========================================================\n");
}

#define REQUIRE(cond, msg)                                                     \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "[ENC-CMP] FAIL: %s  (at %s:%d)\n",                      \
              msg, __FILE__, __LINE__);                                        \
      std::exit(1);                                                            \
    }                                                                          \
  } while (0)

// 生成大小为 size 的伪随机数据（确定性 seed）
static raft::Slice MakeData(size_t size, unsigned seed) {
  auto* buf = new char[size];
  unsigned x = seed | 0x1u;
  for (size_t i = 0; i < size; ++i) {
    x = x * 1103515245u + 12345u;
    buf[i] = static_cast<char>((x >> 16) & 0xFF);
  }
  return raft::Slice(buf, size);
}

// 简单百分位（输入会被排序）
static double Percentile(std::vector<double>* v, double pct) {
  if (v->empty()) return 0.0;
  std::sort(v->begin(), v->end());
  size_t idx = static_cast<size_t>(pct * (v->size() - 1));
  return (*v)[idx];
}

// ============================================================================
//  Section 1: 参数 sanity（FromCapacity 表）
// ============================================================================
static void TestParams() {
  Banner("Section 1: LrcParams::FromCapacity (LRC mode params)");

  struct Case { int N; int F; int k; int l; int r; };
  Case cases[] = {
      {7,  3, 4, 2, 8},   // 4 + 2 + 8 = 14 = 2N
      {11, 5, 6, 2, 14},  // 6 + 2 +14 = 22 = 2N
      {15, 7, 8, 2, 20},  // 8 + 2 +20 = 30 = 2N
  };
  for (const auto& c : cases) {
    LrcParams p = LrcParams::FromCapacity(c.N, /*l=*/2);
    printf("[ENC-CMP]   N=%d  F=%d  -> %s  total=%d\n",
           c.N, c.F, p.ToString().c_str(), p.total_shards());
    REQUIRE(p.k == c.k, "LRC k mismatch");
    REQUIRE(p.l == c.l, "LRC l mismatch");
    REQUIRE(p.r == c.r, "LRC r mismatch");
    REQUIRE(p.total_shards() == 2 * c.N, "LRC total != 2N");
  }
}

// ============================================================================
//  Section 2: 编码正确性
// ============================================================================
static void VerifyRsF(int N, size_t size) {
  int F = N / 2;
  int k = F + 1;
  int m = F;
  raft::Slice orig = MakeData(size, /*seed=*/0xA1A1u + N);
  raft::Encoder enc;
  raft::Encoder::EncodingResults frags;
  REQUIRE(enc.EncodeSlice(orig, k, m, &frags), "RS_F EncodeSlice failed");
  REQUIRE(static_cast<int>(frags.size()) == k + m, "RS_F output count");

  raft::Slice recovered;
  REQUIRE(enc.DecodeSlice(frags, k, m, &recovered),
          "RS_F DecodeSlice (all frags) failed");
  REQUIRE(std::memcmp(recovered.data(), orig.data(), orig.size()) == 0,
          "RS_F decode bytes mismatch");
  delete[] recovered.data();
  delete[] orig.data();
}

static void VerifyRs3F(int N, size_t size) {
  int F = N / 2;
  int k = F + 1;
  int m = 3 * F + 1;
  raft::Slice orig = MakeData(size, /*seed=*/0xB2B2u + N);
  raft::Encoder enc;
  raft::Encoder::EncodingResults frags;
  REQUIRE(enc.EncodeSlice(orig, k, m, &frags), "RS_3F EncodeSlice failed");
  REQUIRE(static_cast<int>(frags.size()) == k + m, "RS_3F output count");
  REQUIRE(static_cast<int>(frags.size()) == 2 * N, "RS_3F total != 2N");

  // 只用前 k 个 frag (data shards) → DecodeSlice
  raft::Encoder::EncodingResults subset;
  for (int i = 0; i < k; ++i) subset[i] = frags[i];

  raft::Slice recovered;
  REQUIRE(enc.DecodeSlice(subset, k, m, &recovered),
          "RS_3F DecodeSlice (k data frags) failed");
  REQUIRE(std::memcmp(recovered.data(), orig.data(), orig.size()) == 0,
          "RS_3F decode bytes mismatch");
  delete[] recovered.data();
  delete[] orig.data();
}

static void VerifyLrc(int N, size_t size) {
  raft::Slice orig = MakeData(size, /*seed=*/0xC3C3u + N);

  LrcParams p = LrcParams::FromCapacity(N, /*l=*/2);
  LrcEncoder enc(p);
  LrcStripe stripe;
  REQUIRE(enc.EncodeStripe(orig, &stripe), "LRC EncodeStripe failed");

  REQUIRE(static_cast<int>(stripe.data_shards.size())     == p.k, "data count");
  REQUIRE(static_cast<int>(stripe.local_parities.size())  == p.l, "local count");
  REQUIRE(static_cast<int>(stripe.global_parities.size()) == p.r, "global count");
  REQUIRE(stripe.total_frags() == 2 * N, "LRC total != 2N");

  // 用前 k 个 data shard 通过 RS(k, r) 全局解码 → 原始数据
  raft::Encoder::EncodingResults subset;
  for (int i = 0; i < p.k; ++i) subset[i] = stripe.data_shards[i];

  raft::Encoder dec;
  raft::Slice recovered;
  REQUIRE(dec.DecodeSlice(subset, p.k, p.r, &recovered),
          "LRC global DecodeSlice failed");
  REQUIRE(std::memcmp(recovered.data(), orig.data(), orig.size()) == 0,
          "LRC decoded bytes mismatch");
  delete[] recovered.data();

  // 单独跑 LocalEncode 比对 local parity 字节一致
  int lk = p.local_k();
  for (int g = 0; g < p.l; ++g) {
    size_t buf_size = static_cast<size_t>(lk) * stripe.frag_size;
    auto* buf = new char[buf_size];
    for (int i = 0; i < lk; ++i) {
      std::memcpy(buf + i * stripe.frag_size,
                  stripe.data_shards[g * lk + i].data(), stripe.frag_size);
    }
    raft::Slice group_data(buf, buf_size);
    LocalEncodeResult local;
    REQUIRE(enc.LocalEncode(group_data, g, &local), "LRC LocalEncode failed");
    REQUIRE(local.local_parity.size() == stripe.frag_size,
            "local parity size mismatch");
    REQUIRE(std::memcmp(local.local_parity.data(),
                        stripe.local_parities[g].data(),
                        stripe.frag_size) == 0,
            "LRC local parity byte mismatch");
    local.FreeMemory();
    delete[] buf;
  }

  stripe.FreeMemory();
  delete[] orig.data();
}

static void TestCorrectness() {
  Banner("Section 2: encoding correctness (encode -> decode -> memcmp)");
  int Ns[]      = {7, 11, 15};
  size_t sizes[] = {1024, 64 * 1024, 1024 * 1024};
  for (int N : Ns) {
    for (size_t sz : sizes) {
      printf("[ENC-CMP]   N=%d size=%zu  RS_F  ...", N, sz); fflush(stdout);
      VerifyRsF(N, sz);
      printf(" PASS\n");
      printf("[ENC-CMP]   N=%d size=%zu  RS_3F ...", N, sz); fflush(stdout);
      VerifyRs3F(N, sz);
      printf(" PASS\n");
      printf("[ENC-CMP]   N=%d size=%zu  LRC   ...", N, sz); fflush(stdout);
      VerifyLrc(N, sz);
      printf(" PASS\n");
    }
  }
}

// ============================================================================
//  Section 3: 编码耗时
// ============================================================================
struct TimingStat {
  double mean_us;
  double p50_us;
  double p95_us;
  double p99_us;
  double throughput_mbps;  // MB/s
};

template <typename EncodeFn>
static TimingStat MeasureOne(EncodeFn fn, size_t size, int rounds) {
  std::vector<double> samples;
  samples.reserve(rounds);

  // Warm-up
  for (int i = 0; i < 3; ++i) fn();

  for (int i = 0; i < rounds; ++i) {
    auto t0 = Clock::now();
    fn();
    auto t1 = Clock::now();
    double us =
        std::chrono::duration<double, std::micro>(t1 - t0).count();
    samples.push_back(us);
  }

  double sum = 0.0;
  for (double s : samples) sum += s;
  double mean = sum / samples.size();
  double p50 = Percentile(&samples, 0.50);
  double p95 = Percentile(&samples, 0.95);
  double p99 = Percentile(&samples, 0.99);
  double throughput_mbps = (size / 1024.0 / 1024.0) / (mean / 1e6);

  return TimingStat{mean, p50, p95, p99, throughput_mbps};
}

static void TestTiming() {
  Banner("Section 3: encoding latency (T=50 rounds, mean/p50/p95/p99 us)");
  int Ns[]      = {7, 11, 15};
  size_t sizes[] = {1024, 64 * 1024, 1024 * 1024};
  const int rounds = 50;

  printf("[ENC-CMP] %-3s %-8s %-6s %10s %10s %10s %10s %10s\n",
         "N", "size", "mode", "mean_us", "p50_us", "p95_us", "p99_us",
         "MB/s");

  for (int N : Ns) {
    int F = N / 2;
    int k_rs_f  = F + 1, m_rs_f  = F;
    int k_rs_3f = F + 1, m_rs_3f = 3 * F + 1;
    LrcParams lp = LrcParams::FromCapacity(N, 2);

    for (size_t sz : sizes) {
      raft::Slice orig = MakeData(sz, /*seed=*/0xD4D4u + N + (unsigned)sz);

      // RS_F
      TimingStat s_rsf = MeasureOne([&] {
        raft::Encoder enc;
        raft::Encoder::EncodingResults out;
        enc.EncodeSlice(orig, k_rs_f, m_rs_f, &out);
        // Encoder takes ownership of result memory until next call; let it
        // be reused on next iteration via the local enc going out of scope.
      }, sz, rounds);
      printf("[ENC-CMP] %-3d %-8zu %-6s %10.1f %10.1f %10.1f %10.1f %10.2f\n",
             N, sz, "RS_F",
             s_rsf.mean_us, s_rsf.p50_us, s_rsf.p95_us, s_rsf.p99_us,
             s_rsf.throughput_mbps);

      // RS_3F
      TimingStat s_rs3f = MeasureOne([&] {
        raft::Encoder enc;
        raft::Encoder::EncodingResults out;
        enc.EncodeSlice(orig, k_rs_3f, m_rs_3f, &out);
      }, sz, rounds);
      printf("[ENC-CMP] %-3d %-8zu %-6s %10.1f %10.1f %10.1f %10.1f %10.2f\n",
             N, sz, "RS_3F",
             s_rs3f.mean_us, s_rs3f.p50_us, s_rs3f.p95_us, s_rs3f.p99_us,
             s_rs3f.throughput_mbps);

      // LRC
      TimingStat s_lrc = MeasureOne([&] {
        LrcEncoder enc(lp);
        LrcStripe stripe;
        enc.EncodeStripe(orig, &stripe);
        stripe.FreeMemory();
      }, sz, rounds);
      printf("[ENC-CMP] %-3d %-8zu %-6s %10.1f %10.1f %10.1f %10.1f %10.2f\n",
             N, sz, "LRC",
             s_lrc.mean_us, s_lrc.p50_us, s_lrc.p95_us, s_lrc.p99_us,
             s_lrc.throughput_mbps);

      printf("\n");
      delete[] orig.data();
    }
  }
}

// ============================================================================
//  Section 4: 放置算法
// ============================================================================
static void TestPlacementRsF(int N) {
  // RS_F: total = N, 1 frag per node. 简单地由 frag_id 直接索引 node。
  printf("[ENC-CMP]   RS_F placement (trivial 1 frag/node) for N=%d\n", N);
  std::vector<int> node_count(N, 0);
  for (int fid = 0; fid < N; ++fid) {
    int node = fid;
    ++node_count[node];
    printf("[ENC-CMP]     frag=%d -> node=%d\n", fid, node);
  }
  for (int n = 0; n < N; ++n) {
    REQUIRE(node_count[n] == 1, "RS_F: each node must hold exactly 1 frag");
  }
}

static void TestPlacementRs3F(int N) {
  int total = 2 * N;
  printf("[ENC-CMP]   RS_3F placement (random 2 frag/node) for N=%d total=%d\n",
         N, total);
  auto plan = BuildRsRandomPlacement(N, total, /*seed=*/42);
  REQUIRE(static_cast<int>(plan.size()) == total, "RS_3F plan size");

  std::set<int> fids;
  std::vector<int> per_node(N, 0);
  for (const auto& rp : plan) {
    REQUIRE(fids.insert(rp.frag_id).second, "RS_3F duplicate frag_id");
    REQUIRE(rp.frag_id >= 0 && rp.frag_id < total, "RS_3F frag_id range");
    REQUIRE(rp.node_id >= 0 && rp.node_id < N,    "RS_3F node_id range");
    ++per_node[rp.node_id];
    printf("[ENC-CMP]     %s\n", rp.ToString().c_str());
  }
  REQUIRE(static_cast<int>(fids.size()) == total, "RS_3F frag_id complete");
  for (int n = 0; n < N; ++n) {
    REQUIRE(per_node[n] == 2,
            "RS_3F: each node must hold exactly 2 frags");
  }
}

static void TestPlacementLrc(int N) {
  LrcParams p = LrcParams::FromCapacity(N, 2);
  int total = p.total_shards();
  printf("[ENC-CMP]   LRC placement (orthogonal 2 frag/node) for N=%d %s\n",
         N, p.ToString().c_str());
  REQUIRE(total == 2 * N, "LRC total != 2N");

  auto partitions = BuildTwoComplementaryPartitions(N, /*seed=*/42);
  REQUIRE(static_cast<int>(partitions.size()) == 2, "LRC partitions count");
  // 互补互斥
  std::set<int> all;
  for (const auto& part : partitions) {
    printf("[ENC-CMP]     %s (size=%zu)\n",
           part.ToString().c_str(), part.member_nodes.size());
    for (int n : part.member_nodes) {
      REQUIRE(all.insert(n).second, "LRC node in multiple partitions");
    }
  }
  REQUIRE(static_cast<int>(all.size()) == N, "LRC partitions cover N");

  OrthogonalPlacer placer(p, partitions);
  auto placement = placer.Place2N();
  REQUIRE(static_cast<int>(placement.size()) == total,
          "LRC Place2N size != total");
  OrthogonalPlacer::Dump(placement, "[ENC-CMP]");

  // 验证 1: frag_id 互异 + 范围正确
  std::set<int> fids;
  std::vector<int> per_node(N, 0);
  for (const auto& fp : placement) {
    REQUIRE(fids.insert(fp.frag_id).second, "LRC duplicate frag_id");
    REQUIRE(fp.frag_id >= 0 && fp.frag_id < total, "LRC frag_id range");
    REQUIRE(fp.node_id >= 0 && fp.node_id < N,     "LRC node_id range");
    ++per_node[fp.node_id];
  }
  REQUIRE(static_cast<int>(fids.size()) == total, "LRC frag_id complete");

  // 验证 2: 每个 node 恰好 2 frag
  for (int n = 0; n < N; ++n) {
    REQUIRE(per_node[n] == 2,
            "LRC: each node must hold exactly 2 frags (2/node budget)");
  }

  // 验证 3: 局部组 g 的 data + local_parity 全部落在分区 g 的节点上
  auto in_partition = [&](int g, int node) {
    const auto& mems = partitions[g].member_nodes;
    return std::find(mems.begin(), mems.end(), node) != mems.end();
  };
  for (const auto& fp : placement) {
    if (fp.kind == FragmentPlacement::kData ||
        fp.kind == FragmentPlacement::kLocalParity) {
      REQUIRE(in_partition(fp.local_group, fp.node_id),
              "LRC: data/local_parity placed outside its partition");
    } else {
      REQUIRE(in_partition(fp.local_group, fp.node_id),
              "LRC: global_parity assigned partition mismatch with node");
    }
  }

  // 验证 4: 每分区 fragment 数 = 2 * |partition|
  std::vector<int> per_partition(2, 0);
  for (const auto& fp : placement) ++per_partition[fp.local_group];
  for (int g = 0; g < 2; ++g) {
    int want = 2 * static_cast<int>(partitions[g].member_nodes.size());
    REQUIRE(per_partition[g] == want,
            "LRC: partition fragment count != 2*|partition|");
  }
}

static void TestPlacement() {
  Banner("Section 4: placement algorithms (RS_F / RS_3F / LRC, N=7)");
  TestPlacementRsF(7);
  printf("\n");
  TestPlacementRs3F(7);
  printf("\n");
  TestPlacementLrc(7);
}

// ============================================================================
//  main
// ============================================================================
int main() {
  printf("\n");
  printf("========================================================\n");
  printf("   FlexRaft Encoding Comparison Test\n");
  printf("   (RS_F vs RS_3F vs LRC: correctness + timing + placement)\n");
  printf("========================================================\n");

  TestParams();
  TestCorrectness();
  TestTiming();
  TestPlacement();

  printf("\n========================================================\n");
  printf("[ENC-CMP] ALL SECTIONS PASSED\n");
  printf("========================================================\n");
  return 0;
}
