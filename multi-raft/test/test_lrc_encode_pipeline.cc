// test_lrc_encode_pipeline.cc
//
// 验证 LRC 写入路径的"编码 + 正交放置"逻辑：
//   1) LrcParams::FromBandwidth(N) 参数计算
//   2) LrcEncoder::EncodeStripe 一次性产出 k+l+r 个分片，
//      且仅用前 k 个 data shard 即可还原原始数据
//   3) 单独跑 LocalEncode 得到的 local parity，与 EncodeStripe 结果位级一致
//   4) OrthogonalPlacer 把 k+l+r 个 fragment 正交放置到 l=2 个互补互斥分区
//
// 该测试是 bare-main 程序，运行后用 echo $? 判断是否 PASS。失败时返回非零。
//
// 用法：
//   ./build/multi-raft/test_lrc_encode_pipeline

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "multi-raft/lrc_encoder.h"
#include "multi-raft/lrc_placement.h"
#include "multi-raft/message.h"
#include "raft/encoder.h"
#include "raft/log_entry.h"

using namespace multiraft;

// ----------------------------------------------------------------------------
//  小工具：彩色横幅
// ----------------------------------------------------------------------------
static void Banner(const char* title) {
  printf("\n========================================================\n");
  printf("[LRC-TEST] %s\n", title);
  printf("========================================================\n");
}

// 简单断言：失败立刻 exit(1)
#define REQUIRE(cond, msg)                                                     \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "[LRC-TEST] FAIL: %s  (at %s:%d)\n",                     \
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

// ----------------------------------------------------------------------------
//  Section 1: 参数计算
// ----------------------------------------------------------------------------
static void TestParams() {
  Banner("Section 1: LrcParams::FromBandwidth(N)");

  struct Case { int N; int F; int k; int l; int r; };
  Case cases[] = {
      {7,  3, 4, 2, 4},   // 3F+1 = 10
      {11, 5, 6, 2, 8},   // 3F+1 = 16
      {15, 7, 8, 2, 12},  // 3F+1 = 22
  };

  for (const auto& c : cases) {
    LrcParams p = LrcParams::FromBandwidth(c.N);
    printf("[LRC-TEST]   N=%d  F=%d  -> %s\n",
           c.N, c.F, p.ToString().c_str());
    REQUIRE(p.k == c.k, "k mismatch");
    REQUIRE(p.l == c.l, "l mismatch");
    REQUIRE(p.r == c.r, "r mismatch");
    REQUIRE(p.k % p.l == 0, "k%l must be 0");
    REQUIRE(p.total_shards() == 3 * c.F + 1, "total = 3F+1");
  }

  // 奇数 k 必须抛异常
  bool threw = false;
  try { LrcParams::FromBandwidth(9); } catch (const std::exception&) { threw = true; }
  REQUIRE(threw, "N=9 should throw (k=F+1=5 is odd)");
  printf("[LRC-TEST]   N=9 odd-k: correctly throws\n");
}

// ----------------------------------------------------------------------------
//  Section 2: EncodeStripe + 仅用前 k 个 data shard 解码
// ----------------------------------------------------------------------------
static void TestEncodeStripe() {
  Banner("Section 2: EncodeStripe (N=7 -> LRC(4,2,4))");

  const size_t kOrigSize = 1024;
  raft::Slice orig = MakeData(kOrigSize, /*seed=*/0xDEADBEEFu);
  printf("[LRC-TEST]   orig data size=%zu\n", orig.size());

  LrcParams p = LrcParams::FromBandwidth(7);
  LrcEncoder enc(p);
  LrcStripe stripe;

  bool ok = enc.EncodeStripe(orig, &stripe);
  REQUIRE(ok, "EncodeStripe failed");

  REQUIRE(static_cast<int>(stripe.data_shards.size())     == p.k,
          "data_shards.size != k");
  REQUIRE(static_cast<int>(stripe.local_parities.size())  == p.l,
          "local_parities.size != l");
  REQUIRE(static_cast<int>(stripe.global_parities.size()) == p.r,
          "global_parities.size != r");
  REQUIRE(stripe.total_frags() == p.k + p.l + p.r,
          "total_frags != k+l+r");
  REQUIRE(stripe.frag_size > 0, "frag_size must be > 0");

  // 所有 fragment 大小一致
  for (const auto& s : stripe.data_shards)
    REQUIRE(s.size() == stripe.frag_size, "data shard size mismatch");
  for (const auto& s : stripe.local_parities)
    REQUIRE(s.size() == stripe.frag_size, "local parity size mismatch");
  for (const auto& s : stripe.global_parities)
    REQUIRE(s.size() == stripe.frag_size, "global parity size mismatch");

  printf("[LRC-TEST]   counts: data=%zu local=%zu global=%zu frag_size=%zu\n",
         stripe.data_shards.size(), stripe.local_parities.size(),
         stripe.global_parities.size(), stripe.frag_size);

  // 用前 k 个 data shard 通过 RS(k, r) 还原原始数据
  raft::Encoder::EncodingResults frags;
  for (int i = 0; i < p.k; ++i) frags[i] = stripe.data_shards[i];

  raft::Encoder dec;
  raft::Slice recovered;
  bool dec_ok = dec.DecodeSlice(frags, p.k, p.r, &recovered);
  REQUIRE(dec_ok, "DecodeSlice from k data shards failed");
  REQUIRE(recovered.size() >= orig.size(),
          "recovered size shorter than orig");
  int cmp = std::memcmp(recovered.data(), orig.data(), orig.size());
  REQUIRE(cmp == 0,
          "recovered bytes do not match original (RS systematic check)");
  delete[] recovered.data();

  printf("[LRC-TEST]   RS-systematic check: PASS (first %zu bytes match)\n",
         orig.size());

  stripe.FreeMemory();
  delete[] orig.data();
}

// ----------------------------------------------------------------------------
//  Section 3: LocalEncode 一致性
// ----------------------------------------------------------------------------
static void TestLocalEncodeConsistency() {
  Banner("Section 3: LocalEncode vs EncodeStripe local_parities");

  const size_t kOrigSize = 4096;
  raft::Slice orig = MakeData(kOrigSize, /*seed=*/0xC0FFEEu);

  LrcParams p = LrcParams::FromBandwidth(7);
  LrcEncoder enc(p);
  LrcStripe stripe;
  REQUIRE(enc.EncodeStripe(orig, &stripe), "EncodeStripe failed");

  int lk = p.local_k();
  int mismatches = 0;
  for (int g = 0; g < p.l; ++g) {
    // 拼接该 group 的 lk 个数据分片 -> 连续内存 -> 独立 LocalEncode
    size_t buf_size = static_cast<size_t>(lk) * stripe.frag_size;
    auto* buf = new char[buf_size];
    for (int i = 0; i < lk; ++i) {
      std::memcpy(buf + i * stripe.frag_size,
                  stripe.data_shards[g * lk + i].data(), stripe.frag_size);
    }
    raft::Slice group_data(buf, buf_size);

    LocalEncodeResult local;
    REQUIRE(enc.LocalEncode(group_data, g, &local), "LocalEncode failed");
    REQUIRE(local.local_parity.size() == stripe.frag_size,
            "local parity size mismatch");
    int cmp = std::memcmp(local.local_parity.data(),
                          stripe.local_parities[g].data(),
                          stripe.frag_size);
    if (cmp != 0) {
      mismatches++;
      printf("[LRC-TEST]   group=%d local_parity BYTE MISMATCH\n", g);
    } else {
      printf("[LRC-TEST]   group=%d local_parity byte-identical: PASS\n", g);
    }
    local.FreeMemory();
    delete[] buf;
  }
  REQUIRE(mismatches == 0,
          "LocalEncode result differs from EncodeStripe local_parities");

  stripe.FreeMemory();
  delete[] orig.data();
}

// ----------------------------------------------------------------------------
//  Section 4: OrthogonalPlacer
// ----------------------------------------------------------------------------
static void TestPlacement() {
  Banner("Section 4: OrthogonalPlacer (N=7, LRC(4,2,4))");

  const int N = 7;
  LrcParams p = LrcParams::FromBandwidth(N);
  REQUIRE(p.k == 4 && p.l == 2 && p.r == 4, "expected LRC(4,2,4)");

  auto partitions = BuildTwoComplementaryPartitions(N, /*seed=*/42);
  REQUIRE(static_cast<int>(partitions.size()) == p.l,
          "partitions count != l");
  // 互补互斥：所有节点恰好出现一次
  std::set<int> all;
  for (const auto& part : partitions) {
    printf("[LRC-TEST]   %s (size=%zu)\n",
           part.ToString().c_str(), part.member_nodes.size());
    for (int n : part.member_nodes) {
      REQUIRE(all.insert(n).second, "node appears in multiple partitions");
    }
  }
  REQUIRE(static_cast<int>(all.size()) == N,
          "partitions do not cover all N nodes");

  OrthogonalPlacer placer(p, partitions);
  auto placement = placer.Place();
  REQUIRE(static_cast<int>(placement.size()) == p.k + p.l + p.r,
          "placement size != k+l+r");

  // 打印 placement 表
  OrthogonalPlacer::Dump(placement, "[LRC-TEST]");

  // 每个 frag_id 在 [0, k+l+r) 内且唯一
  std::set<int> fids;
  for (const auto& fp : placement) {
    REQUIRE(fp.frag_id >= 0 && fp.frag_id < p.total_shards(),
            "frag_id out of range");
    REQUIRE(fids.insert(fp.frag_id).second, "duplicate frag_id");
  }
  REQUIRE(static_cast<int>(fids.size()) == p.total_shards(),
          "frag_id set incomplete");

  // 数据分片 + 局部校验：必须落在自身分区的节点上
  auto in_partition = [&](int g, int node) {
    const auto& mems = partitions[g].member_nodes;
    return std::find(mems.begin(), mems.end(), node) != mems.end();
  };
  int data_count = 0, local_par_count = 0, global_par_count = 0;
  for (const auto& fp : placement) {
    if (fp.kind == FragmentPlacement::kData) {
      data_count++;
      REQUIRE(in_partition(fp.local_group, fp.node_id),
              "data shard placed outside its local-group partition");
    } else if (fp.kind == FragmentPlacement::kLocalParity) {
      local_par_count++;
      REQUIRE(in_partition(fp.local_group, fp.node_id),
              "local parity placed outside its local-group partition");
    } else {
      global_par_count++;
      REQUIRE(fp.local_group >= 0 && fp.local_group < p.l,
              "global parity local_group out of range");
      REQUIRE(in_partition(fp.local_group, fp.node_id),
              "global parity placed outside its assigned partition");
    }
  }
  REQUIRE(data_count       == p.k, "data fragment count");
  REQUIRE(local_par_count  == p.l, "local parity count");
  REQUIRE(global_par_count == p.r, "global parity count");

  // 数据分片 frag_id 编号：[0, k) ，按 (g, i) 顺序 → g*lk + i
  int lk = p.local_k();
  for (int g = 0; g < p.l; ++g) {
    for (int i = 0; i < lk; ++i) {
      int fid = g * lk + i;
      const auto& fp = placement[fid];
      REQUIRE(fp.frag_id == fid, "data frag_id order");
      REQUIRE(fp.kind == FragmentPlacement::kData, "data kind");
      REQUIRE(fp.local_group == g, "data local_group");
    }
  }
  // 局部校验 frag_id：[k, k+l)
  for (int g = 0; g < p.l; ++g) {
    const auto& fp = placement[p.k + g];
    REQUIRE(fp.kind == FragmentPlacement::kLocalParity, "local parity kind");
    REQUIRE(fp.local_group == g, "local parity local_group");
  }
  // 全局校验 frag_id：[k+l, k+l+r), local_group = i % l
  for (int i = 0; i < p.r; ++i) {
    const auto& fp = placement[p.k + p.l + i];
    REQUIRE(fp.kind == FragmentPlacement::kGlobalParity,
            "global parity kind");
    REQUIRE(fp.local_group == i % p.l,
            "global parity local_group rotates by i%l");
  }

  printf("[LRC-TEST]   counts: data=%d local_parity=%d global_parity=%d\n",
         data_count, local_par_count, global_par_count);
  printf("[LRC-TEST]   PASS: every fragment placed within its complementary "
         "partition\n");
}

// ----------------------------------------------------------------------------
//  main
// ----------------------------------------------------------------------------
int main() {
  printf("\n");
  printf("========================================================\n");
  printf("   FlexRaft LRC Encode + Placement Pipeline Test\n");
  printf("   ENCODING=LRC unit test (bare main)\n");
  printf("========================================================\n");

  TestParams();
  TestEncodeStripe();
  TestLocalEncodeConsistency();
  TestPlacement();

  printf("\n========================================================\n");
  printf("[LRC-TEST] ALL SECTIONS PASSED\n");
  printf("========================================================\n");
  return 0;
}
