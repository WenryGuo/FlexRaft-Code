#pragma once
// lrc_encoder.h  —  LRC(k, l, r) 两级编码器
//
// 修改说明（2026-03-28）：
//   1. 修复 LocalEncode 的生命周期问题：不再依赖外部传入的 group_data_frags，
//      直接接收连续内存 Slice，避免 use-after-return
//   2. 移除 const_cast，直接使用 const Slice
//   3. 添加完整的日志输出，便于调试
//
// LRC 编码流程：
//   GlobalEncode: RS(k, r) - Leader 对原始数据执行全局编码，产生 k+r 个块
//   LocalEncode:  RS(k/l, 1) - 每个 group 对分配到的 k/l 个数据块执行局部编码
//
// FlexRaft 原有编码器接口（来自 encoder.cc）：
//   bool Encoder::EncodeSlice(const Slice&, int k, int m, EncodingResults*)
//   bool Encoder::DecodeSlice(const EncodingResults&, int k, int m, Slice*)

#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>

#include "encoder.h"    // raft::Encoder, raft::EncodingResults
#include "raft_type.h"  // raft::Slice, raft::raft_frag_id_t

namespace multiraft {

// ==========================================================================
//  LrcParams
// ==========================================================================
struct LrcParams {
  int k;  // 数据分片总数（全局编码参数）
  int l;  // Raft group 数（局部组数）
  int r;  // 全局校验分片数
  // 约束: k % l == 0

  int local_k() const { return k / l; }  // 每个局部组的数据块数

  // 总物理分片数 = k 数据 + l 局部校验 + r 全局校验
  int total_shards() const { return k + l + r; }

  // 存储开销倍数（相比原始数据）
  double storage_overhead() const {
    return static_cast<double>(total_shards()) / k;
  }

  // ------------------------------------------------------------------------
  //  AutoFromClusterSize: 根据集群规模 N 自动推导 LRC 参数
  //
  //  用户需求：
  //    k = floor(N/2) + 1
  //    l >= 2，最好取能被 k 整除的最大值
  //    r = N - k - l
  //
  //  示例：
  //    NormalClusterSize(7)  → F=3  k=4  → l=2 (4的最大因数,2<=3)  r=1 → LRC(4,2,1)
  //    NormalClusterSize(9) → F=4  k=5  → l=2 (5的最大因数,2<=4)  r=2 → LRC(5,2,2) k为奇数情况暂不考虑
  //    NormalClusterSize(11) → F=5  k=6  → l=3 (6的最大因数,3<=5)  r=2 → LRC(6,3,2)
  //    NormalClusterSize(15) → F=7  k=8  → l=4 (8的最大因数,4<=7)  r=3 → LRC(8,4,3)
  // ------------------------------------------------------------------------
  static LrcParams NormalClusterSize(int N) {
    if (N < 3)
      throw std::invalid_argument("NormalClusterSize: N must be >= 3");
    int F = N / 2;           // 容错性：N = 2F + 1（向下取整）
    int k = F + 1;           // 数据分片数

    // 找满足 l >= 2 且 l <= F 且 k % l == 0 的最大 l
    int best_l = 1;
    for (int cand = F; cand >= 2; --cand) {
      if (k % cand == 0) {
        best_l = cand;
        break;  // 从大到小找，找到第一个就退出
      }
    }
    int l = best_l;
    int r = F - l;
    return LrcParams{k, l, r};
  }

  // ------------------------------------------------------------------------
  //  FromBandwidth: 按"带宽-平衡"约束计算 LRC 参数
  //
  //  约束（与论文带宽公式一致）：
  //    N  = 2F + 1
  //    k  = F + 1                  （数据分片数）
  //    l  = 2                      （统一为 2 个局部组）
  //    Bw = 3F + 1                 （编码后总分片数 = 物理带宽预算）
  //    r  = Bw - k - l = 2F - 2    （全局校验分片数）
  //
  //  约束：k 必须能被 l=2 整除，即 F+1 为偶数。奇数 k 的情况暂不支持。
  //
  //  示例：
  //    N=7,  F=3 → LRC(4, 2, 4),  total=10 = 3F+1
  //    N=11, F=5 → LRC(6, 2, 8),  total=16 = 3F+1
  //    N=15, F=7 → LRC(8, 2, 12), total=22 = 3F+1
  // ------------------------------------------------------------------------
  static LrcParams FromBandwidth(int N) {
    if (N < 5)
      throw std::invalid_argument("FromBandwidth: N must be >= 5");
    int F = N / 2;
    int k = F + 1;
    int l = 2;
    if (k % l != 0) {
      throw std::invalid_argument(
          "FromBandwidth: k=F+1 must be even (odd k not supported)");
    }
    int r = (3 * F + 1) - k - l;  // = 2F - 2
    if (r < 0) {
      throw std::invalid_argument("FromBandwidth: derived r < 0");
    }
    return LrcParams{k, l, r};
  }

  // ------------------------------------------------------------------------
  //  FromCapacity: 按"每节点 2 块"容量预算计算 LRC 参数
  //
  //  约束（N = 2F + 1）：
  //    k = F + 1                  （数据分片数）
  //    l 由调用方给定（默认 2）   （局部组数）
  //    Cap = 2 * N = 4F + 2       （总分片预算 = 每节点 2 块 × N 节点）
  //    r   = Cap - k - l = 2N - k - l
  //
  //  约束：k 必须能被 l 整除。
  //
  //  示例：
  //    N=7,  F=3,  l=2 → LRC(4, 2, 8),   total=14 = 2N
  //    N=11, F=5,  l=2 → LRC(6, 2, 14),  total=22 = 2N
  //    N=15, F=7,  l=2 → LRC(8, 2, 20),  total=30 = 2N
  // ------------------------------------------------------------------------
  static LrcParams FromCapacity(int N, int l = 2) {
    if (N < 5)
      throw std::invalid_argument("FromCapacity: N must be >= 5");
    if (l < 1)
      throw std::invalid_argument("FromCapacity: l must be >= 1");
    int F = N / 2;
    int k = F + 1;
    if (k % l != 0) {
      throw std::invalid_argument(
          "FromCapacity: k=F+1 must be divisible by l");
    }
    int r = 2 * N - k - l;  // = 3F + 1 - l
    if (r < 0) {
      throw std::invalid_argument("FromCapacity: derived r < 0");
    }
    return LrcParams{k, l, r};
  }

  std::string ToString() const {
    return "LRC(" + std::to_string(k) + "," +
                    std::to_string(l) + "," +
                    std::to_string(r) + ")" +
           " local_k=" + std::to_string(local_k()) +
           " overhead=" + std::to_string(storage_overhead()).substr(0, 4) + "x";
  }
};

// ==========================================================================
//  LocalEncodeResult
//  存储局部编码结果，包含数据分片和局部校验分片
//  注意：data_shards 和 local_parity 中的 Slice 指向 new[] 分配的内存，
//        需要通过 FreeMemory() 释放
// ==========================================================================
struct LocalEncodeResult {
  std::vector<raft::Slice> data_shards;   // local_k 个数据分片
  raft::Slice              local_parity;    // 1 个局部校验分片
  size_t                   original_size;   // 原始数据大小

  // 释放所有 Slice 指向的内存
  void FreeMemory() {
    for (auto& shard : data_shards) {
      delete[] shard.data();
    }
    data_shards.clear();
    delete[] local_parity.data();
  }
};

// ==========================================================================
//  LrcStripe
//  存储一次完整 LRC 编码后的所有分片
//
//  frag_id 编号约定（按写入路径与 OrthogonalPlacer 一致）：
//    [0,         k          ) → data_shards
//    [k,         k + l      ) → local_parities  （第 g 个对应局部组 g）
//    [k + l,     k + l + r  ) → global_parities
//
//  所有 Slice 都指向 LrcStripe 私有 new[] 分配的内存，调用方需用
//  FreeMemory() 释放（或依赖 LrcStripe 析构）。
// ==========================================================================
struct LrcStripe {
  std::vector<raft::Slice> data_shards;        // k 个数据分片
  std::vector<raft::Slice> local_parities;     // l 个局部校验分片
  std::vector<raft::Slice> global_parities;    // r 个全局校验分片
  size_t                   original_size = 0;  // 原始数据字节数
  size_t                   frag_size = 0;      // 每个 fragment 的字节数

  // 总分片数 = k + l + r
  int total_frags() const {
    return static_cast<int>(
        data_shards.size() + local_parities.size() + global_parities.size());
  }

  // 释放所有 Slice 指向的 new[] 内存
  void FreeMemory() {
    for (auto& s : data_shards)     delete[] s.data();
    for (auto& s : local_parities)  delete[] s.data();
    for (auto& s : global_parities) delete[] s.data();
    data_shards.clear();
    local_parities.clear();
    global_parities.clear();
  }
};

// ==========================================================================
//  LrcEncoder
// ==========================================================================
class LrcEncoder {
 public:
  explicit LrcEncoder(const LrcParams& p) : p_(p) {
    assert(p_.k > 0 && p_.l > 0 && p_.r >= 0);
    assert(p_.k % p_.l == 0 && "k must be divisible by l");
    global_enc_ = new raft::Encoder();
    
    printf("[LRC] Initialized with %s\n", p_.ToString().c_str());
  }

  ~LrcEncoder() { delete global_enc_; }

  LrcEncoder(const LrcEncoder&) = delete;
  LrcEncoder& operator=(const LrcEncoder&) = delete;

  const LrcParams& params() const { return p_; }

  // ------------------------------------------------------------------------
  //  GlobalEncode: RS(k, r) 对整块数据编码
  //
  //  输入 : slice  — 原始数据（生命周期须 >= out 的使用周期）
  //  输出 : out    — EncodingResults
  //           key 0..k-1   : 数据分片（零拷贝，指向 slice 内存）
  //           key k..k+r-1 : 全局校验分片（Encoder 内部 new[]）
  //  返回 : true 成功，false 失败
  // ------------------------------------------------------------------------
  bool GlobalEncode(const raft::Slice& slice,
                    raft::Encoder::EncodingResults* out) const {
    printf("[LRC] GlobalEncode: RS(%d, %d) on %zu bytes\n", p_.k, p_.r, slice.size());
    assert(out != nullptr);
    out->clear();
    bool ok = global_enc_->EncodeSlice(slice, p_.k, p_.r, out);
    if (ok) {
      printf("[LRC] GlobalEncode: Generated %d data shards + %d global parity shards\n",
             p_.k, p_.r);
    } else {
      printf("[LRC] GlobalEncode: FAILED\n");
    }
    return ok;
  }

  // ------------------------------------------------------------------------
  //  LocalEncode: RS(local_k, 1) 对指定 group 的数据块做局部编码
  //
  //  输入 : group_data  — 该 group 对应的 local_k 个连续数据块
  //           （从 GlobalEncode 结果中提取，内存必须连续）
  //         group_id    — group 编号（用于日志）
  //
  //  输出 : out  — LocalEncodeResult
  //           data_shards : local_k 个数据分片
  //           local_parity: 1 个局部校验分片
  //
  //  返回 : true 成功，false 失败
  //
  //  说明：直接接收连续内存 Slice，避免生命周期问题
  // ------------------------------------------------------------------------
  bool LocalEncode(const raft::Slice& group_data,
                   int group_id,
                   LocalEncodeResult* out) const {
    int lk = p_.local_k();
    printf("[LRC] LocalEncode: Group %d RS(%d, 1) on %zu bytes\n",
           group_id, lk, group_data.size());
    assert(out != nullptr);
    
    // 校验数据大小
    size_t expected_size = static_cast<size_t>(lk) * (group_data.size() / lk);
    if (group_data.size() < expected_size && group_data.size() % lk != 0) {
      printf("[LRC] LocalEncode: WARNING - size %zu may not be aligned for k=%d\n",
             group_data.size(), lk);
    }

    // 构造输入：local_k 个连续数据块
    raft::Encoder::EncodingResults input;
    size_t frag_size = group_data.size() / lk;
    for (int i = 0; i < lk; ++i) {
      const char* ptr = group_data.data() + i * frag_size;
      input[i] = raft::Slice(const_cast<char*>(ptr), frag_size);
    }

    // RS(local_k, 1) 编码
    raft::Encoder::EncodingResults results;
    raft::Encoder local_enc;
    bool ok = local_enc.EncodeSlice(group_data, lk, 1, &results);
    if (!ok) {
      printf("[LRC] LocalEncode: Group %d FAILED\n", group_id);
      return false;
    }

    // 提取结果
    // 注意：results.at(i) 对于 i < lk 可能指向 group_data 内存
    // 需要拷贝数据以避免 use-after-free
    out->data_shards.clear();
    out->data_shards.reserve(static_cast<size_t>(lk));
    for (int i = 0; i < lk; ++i) {
      const raft::Slice& src = results.at(i);
      char* copy = new char[src.size()];
      memcpy(copy, src.data(), src.size());
      out->data_shards.push_back(raft::Slice(copy, src.size()));
    }
    
    // 局部校验块由 local_enc 分配，需要拷贝（因为 local_enc 会销毁）
    const raft::Slice& parity_src = results.at(lk);
    char* parity_copy = new char[parity_src.size()];
    memcpy(parity_copy, parity_src.data(), parity_src.size());
    out->local_parity = raft::Slice(parity_copy, parity_src.size());
    
    out->original_size = group_data.size();

    printf("[LRC] LocalEncode: Group %d SUCCESS - %d data shards + 1 local parity\n",
           group_id, lk);
    return true;
  }

  // ------------------------------------------------------------------------
  //  EncodeStripe: 完整 LRC 编码 pipeline，一次性产出全部 k + l + r 个分片
  //
  //  流程：
  //    1) GlobalEncode RS(k, r)        → k 数据 + r 全局校验
  //    2) 对 l 个局部组分别做 LocalEncode RS(local_k, 1) → l 个局部校验
  //    3) 将所有分片 deep-copy 到 LrcStripe 独立 new[] 内存中
  //
  //  返回值: true 成功，false 失败
  //  内存: 由调用方负责调用 out->FreeMemory()
  // ------------------------------------------------------------------------
  bool EncodeStripe(const raft::Slice& data, LrcStripe* out) const {
    assert(out != nullptr);
    out->FreeMemory();
    out->original_size = data.size();

    printf("[LRC-ENC] EncodeStripe begin: %s on %zu bytes\n",
           p_.ToString().c_str(), data.size());

    // ---- 1) Global RS(k, r) ----
    raft::Encoder::EncodingResults global_result;
    if (!global_enc_->EncodeSlice(data, p_.k, p_.r, &global_result)) {
      printf("[LRC-ENC] GlobalEncode FAILED\n");
      return false;
    }
    if (static_cast<int>(global_result.size()) != p_.k + p_.r) {
      printf("[LRC-ENC] GlobalEncode produced %zu frags, expected %d\n",
             global_result.size(), p_.k + p_.r);
      return false;
    }
    size_t frag_size = global_result.begin()->second.size();
    out->frag_size = frag_size;
    printf("[LRC-ENC] step=GLOBAL frags=%d frag_size=%zu\n",
           p_.k + p_.r, frag_size);

    // 把 k 个数据分片 deep-copy 到 out->data_shards
    out->data_shards.reserve(static_cast<size_t>(p_.k));
    for (int i = 0; i < p_.k; ++i) {
      const raft::Slice& src = global_result.at(i);
      char* buf = new char[src.size()];
      std::memcpy(buf, src.data(), src.size());
      out->data_shards.emplace_back(buf, src.size());
      printf("[LRC-ENC]   data    frag_id=%d size=%zu\n", i, src.size());
    }
    // 把 r 个全局校验分片 deep-copy 到 out->global_parities
    out->global_parities.reserve(static_cast<size_t>(p_.r));
    for (int i = 0; i < p_.r; ++i) {
      const raft::Slice& src = global_result.at(p_.k + i);
      char* buf = new char[src.size()];
      std::memcpy(buf, src.data(), src.size());
      out->global_parities.emplace_back(buf, src.size());
      printf("[LRC-ENC]   global  frag_id=%d size=%zu (offset=%d in stripe)\n",
             i, src.size(), p_.k + p_.l + i);
    }

    // ---- 2) Local RS(local_k, 1) for each group ----
    int lk = p_.local_k();
    out->local_parities.reserve(static_cast<size_t>(p_.l));
    for (int g = 0; g < p_.l; ++g) {
      // 拼接该 group 的 local_k 个数据分片到连续内存
      size_t buf_size = static_cast<size_t>(lk) * frag_size;
      std::unique_ptr<char[]> buf(new char[buf_size]);
      for (int i = 0; i < lk; ++i) {
        std::memcpy(buf.get() + i * frag_size,
                    out->data_shards[g * lk + i].data(), frag_size);
      }
      raft::Slice group_data(buf.get(), buf_size);

      LocalEncodeResult local;
      if (!LocalEncode(group_data, g, &local)) {
        printf("[LRC-ENC] LocalEncode group=%d FAILED\n", g);
        return false;
      }

      // 拷贝 local parity 到 LrcStripe 独立内存
      char* parity_buf = new char[local.local_parity.size()];
      std::memcpy(parity_buf, local.local_parity.data(),
                  local.local_parity.size());
      out->local_parities.emplace_back(parity_buf, local.local_parity.size());
      printf("[LRC-ENC]   local   group=%d size=%zu (offset=%d in stripe)\n",
             g, local.local_parity.size(), p_.k + g);

      // 释放 LocalEncode 内部 new[] 内存（data_shards + local_parity）
      local.FreeMemory();
    }

    printf("[LRC-ENC] EncodeStripe done: total_frags=%d (k=%d + l=%d + r=%d)\n",
           out->total_frags(), p_.k, p_.l, p_.r);
    return true;
  }

  // ------------------------------------------------------------------------
  //  ExtractGroupDataFrags: 从 GlobalEncode 结果中提取 group g 的数据分片
  //  提取后组装成连续内存的 Slice 返回
  //
  //  输入 : global_result — GlobalEncode 的输出
  //         group_id     — group 编号
  //
  //  返回 : 连续内存的 Slice，包含该 group 的 local_k 个数据块
  // ------------------------------------------------------------------------
  raft::Slice ExtractGroupDataFrags(const raft::Encoder::EncodingResults& global_result,
                                    int group_id) const {
    int lk    = p_.local_k();
    int begin = group_id * lk;
    int end   = begin + lk;

    printf("[LRC] ExtractGroupDataFrags: Group %d needs frags [%d, %d)\n",
           group_id, begin, end);

    // 验证所有需要的分片都存在
    for (int gfid = begin; gfid < end; ++gfid) {
      auto it = global_result.find(gfid);
      assert(it != global_result.end() && "frag missing from GlobalEncode result");
    }

    // 计算总大小
    size_t frag_size = global_result.at(begin).size();
    size_t total_size = static_cast<size_t>(lk) * frag_size;

    // 分配连续内存（由调用方负责生命周期）
    char* contiguous = new char[total_size];
    for (int i = 0; i < lk; ++i) {
      const raft::Slice& src = global_result.at(begin + i);
      memcpy(contiguous + i * frag_size, src.data(), frag_size);
    }

    printf("[LRC] ExtractGroupDataFrags: Group %d extracted %d frags, total %zu bytes\n",
           group_id, lk, total_size);
    
    return raft::Slice(contiguous, total_size);
  }

  // ------------------------------------------------------------------------
  //  ExtractGlobalParities: 从 GlobalEncode 结果中提取全局校验分片
  // ------------------------------------------------------------------------
  raft::Encoder::EncodingResults ExtractGlobalParities(
      const raft::Encoder::EncodingResults& global_result) const {
    raft::Encoder::EncodingResults parities;
    for (int fid = p_.k; fid < p_.k + p_.r; ++fid) {
      auto it = global_result.find(fid);
      assert(it != global_result.end());
      parities[fid] = it->second;
    }
    printf("[LRC] ExtractGlobalParities: Extracted %d global parity shards\n", p_.r);
    return parities;
  }

  // ------------------------------------------------------------------------
  //  LocalRepair: 仅在 group 内联系 local_k 个节点恢复单个失效分片
  // ------------------------------------------------------------------------
  bool LocalRepair(const raft::Encoder::EncodingResults& available_frags,
                   raft::Slice* out,
                   int group_id) const {
    int lk = p_.local_k();
    printf("[LRC] LocalRepair: Group %d RS(%d, 1) recovery\n", group_id, lk);
    raft::Encoder local_enc;
    return local_enc.DecodeSlice(available_frags, lk, 1, out);
  }

  // ------------------------------------------------------------------------
  //  GlobalRepair: 跨 group 全局恢复 RS(k, r)
  // ------------------------------------------------------------------------
  bool GlobalRepair(const raft::Encoder::EncodingResults& available_frags,
                    raft::Slice* out) const {
    printf("[LRC] GlobalRepair: RS(%d, %d) recovery\n", p_.k, p_.r);
    return global_enc_->DecodeSlice(available_frags, p_.k, p_.r, out);
  }

  // ------------------------------------------------------------------------
  //  辅助：全局数据分片 id → group id
  // ------------------------------------------------------------------------
  int GlobalFragToGroup(int global_frag_id) const {
    assert(global_frag_id >= 0 && global_frag_id < p_.k);
    return global_frag_id / p_.local_k();
  }

  // ------------------------------------------------------------------------
  //  辅助：全局数据分片 id → group 内局部 id
  // ------------------------------------------------------------------------
  int GlobalFragToLocalId(int global_frag_id) const {
    return global_frag_id % p_.local_k();
  }

 private:
  LrcParams      p_;
  raft::Encoder* global_enc_;  // RS(k, r) 全局编码器
};

}  // namespace multiraft
