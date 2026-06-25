#pragma once
// lrc_placement.h — LRC 分片正交放置（Orthogonal Placement）
//
// [DEPRECATED] LRC 模式请使用 lrc_complementary_grouper.h 中的 LrcComplementaryGrouper。
// RS_3F 模式仍使用 RsRandomPlacement 和 BuildRsRandomPlacement。
//
// 一条 LRC 条带共 k + l + r 个 fragment：
//   [0,     k        )  : data shards
//   [k,     k + l    )  : local parity (frag_id k+g 对应局部组 g)
//   [k+l,   k + l + r)  : global parity
//
// 正交放置规则：
//   1. 局部组 g 的 local_k 个 data + 1 个 local parity 全部落在
//      "互补互斥分区 g" 的节点上（轮询分配）。
//   2. r 个 global parity 按 frag_id % l 交替落到 l 个互补分区中
//      （即奇/偶分组），保证全局校验也是正交的。
//   3. 若某分区内 fragment 数 > 节点数，则继续轮询，同一物理节点
//      持有多个 fragment（这是 N < 3F+1 时的正常情况，例如 N=7,
//      LRC(4,2,4) 总分片 10）。
//
// 调用方使用：
//   LrcParams       p = LrcParams::FromBandwidth(N);
//   LrcGroupBuilder gb(N, seed);
//   auto partitions   = gb.BuildComplementaryGroup(matix);
//   OrthogonalPlacer  placer(p, partitions);
//   auto              placement = placer.Place();  // size = p.k + p.l + p.r

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "lrc_encoder.h"
#include "message.h"  // FragmentPlacement

namespace multiraft {

// ============================================================================
//  LocalGroup — 单个 Local Group 的成员信息
// ============================================================================
struct LocalGroup {
  int group_id;                       // Local Group ID [0, l-1]
  std::vector<int> member_nodes;      // 该 Local Group 包含的物理节点 ID
  bool is_large;                      // 是否是大分组 (g_large)

  std::string ToString() const {
    std::string s = "LG" + std::to_string(group_id) + "(" +
                    (is_large ? "large" : "small") + "): {";
    for (size_t i = 0; i < member_nodes.size(); ++i) {
      s += std::to_string(member_nodes[i]);
      if (i < member_nodes.size() - 1) s += ",";
    }
    s += "}";
    return s;
  }
};

// ============================================================================
//  BuildTwoComplementaryPartitions
//
//  当我们使用 LrcParams::FromBandwidth（l 固定为 2）时，需要的是恰好 2 个
//  互补互斥分区。LrcGroupBuilder 会按"k 的最大因子"内部计算 l，可能 != 2
//  （例如 N=11 时 l=3）。这里提供一个简单可复用的 2-分区构造器：
//
//  规则：用 seed 打乱 [0..N-1]，前 floor(N/2) 个进入 LG0（小分区），
//        其余 ceil(N/2) 个进入 LG1（大分区）。
//
//  返回的 LocalGroup 数组长度恒为 2。
// ============================================================================
inline std::vector<LocalGroup> BuildTwoComplementaryPartitions(
    int N, std::uint64_t seed) {
  if (N < 2) {
    throw std::invalid_argument(
        "BuildTwoComplementaryPartitions: N must be >= 2");
  }
  std::vector<int> pool;
  pool.reserve(N);
  for (int i = 0; i < N; ++i) pool.push_back(i);

  std::mt19937 rng(seed == 0 ? 1u : static_cast<unsigned>(seed));
  std::shuffle(pool.begin(), pool.end(), rng);

  int small_size = N / 2;
  std::vector<LocalGroup> out(2);
  out[0].group_id = 0;
  out[0].is_large = false;
  out[0].member_nodes.assign(pool.begin(), pool.begin() + small_size);

  out[1].group_id = 1;
  out[1].is_large = true;
  out[1].member_nodes.assign(pool.begin() + small_size, pool.end());

  return out;
}

// ============================================================================
//  OrthogonalPlacer — 把 k+l+r 个 fragment 正交放置到 l 个互补互斥分区上
//
//  [DEPRECATED] LRC 模式请使用 LrcComplementaryGrouper::GetNodePlacementsVector()
//  此类的 Place() 和 Place2N() 方法不再用于 LRC 编码。
//  RS_F 模式仍使用 RsRandomPlacement。
// ============================================================================
class OrthogonalPlacer {
 public:
  // partitions 必须正好是 l 个 LocalGroup，且节点集合互补互斥地覆盖 N 个节点
  OrthogonalPlacer(const LrcParams& p,
                   const std::vector<LocalGroup>& partitions)
      : p_(p), partitions_(partitions) {
    if (static_cast<int>(partitions_.size()) != p_.l) {
      char err_buf[128];
      snprintf(err_buf, sizeof(err_buf),
               "OrthogonalPlacer: partitions=%zu, expected l=%d",
               partitions_.size(), p_.l);
      throw std::invalid_argument(err_buf);
    }
    for (int g = 0; g < p_.l; ++g) {
      if (partitions_[g].member_nodes.empty()) {
        throw std::invalid_argument(
            "OrthogonalPlacer: empty partition (no nodes)");
      }
    }
  }

  // 返回 k + l + r 个 FragmentPlacement，按 frag_id 升序
  std::vector<FragmentPlacement> Place() const {
    std::vector<FragmentPlacement> out;
    out.reserve(static_cast<size_t>(p_.k + p_.l + p_.r));

    int lk = p_.local_k();

    // 为每个互补分区维护一个轮询游标
    std::vector<int> cursor(p_.l, 0);

    // ---- 1) 数据分片：data[g*lk + i] → 分区 g ----
    for (int g = 0; g < p_.l; ++g) {
      const auto& nodes = partitions_[g].member_nodes;
      for (int i = 0; i < lk; ++i) {
        FragmentPlacement fp;
        fp.frag_id     = g * lk + i;
        fp.local_group = g;
        fp.node_id     = nodes[cursor[g] % nodes.size()];
        fp.kind        = FragmentPlacement::kData;
        ++cursor[g];
        out.push_back(fp);
      }
    }

    // ---- 2) 局部校验：local_parity[g] → 分区 g ----
    for (int g = 0; g < p_.l; ++g) {
      const auto& nodes = partitions_[g].member_nodes;
      FragmentPlacement fp;
      fp.frag_id     = p_.k + g;
      fp.local_group = g;
      fp.node_id     = nodes[cursor[g] % nodes.size()];
      fp.kind        = FragmentPlacement::kLocalParity;
      ++cursor[g];
      out.push_back(fp);
    }

    // ---- 3) 全局校验：按 i % l 交替分到 l 个分区 ----
    for (int i = 0; i < p_.r; ++i) {
      int g = i % p_.l;
      const auto& nodes = partitions_[g].member_nodes;
      FragmentPlacement fp;
      fp.frag_id     = p_.k + p_.l + i;
      fp.local_group = g;
      fp.node_id     = nodes[cursor[g] % nodes.size()];
      fp.kind        = FragmentPlacement::kGlobalParity;
      ++cursor[g];
      out.push_back(fp);
    }

    // 排序保证按 frag_id 升序（其实已经是）
    // std::sort(out.begin(), out.end(),
    //           [](const FragmentPlacement& a, const FragmentPlacement& b) {
    //             return a.frag_id < b.frag_id;
    //           });
    return out;
  }

  // ------------------------------------------------------------------------
  //  Place2N: "2 frag/node" 正交放置（容量 = 2N，对应 LrcParams::FromCapacity）
  //
  //  与 Place() 的区别：
  //    Place()   将 r 个 global parity 按 frag_id % l 在两分区间均分。
  //    Place2N() 让每个 node 恰好持有 2 个 fragment：
  //              分区 g 的总容量 = 2 * |partition g| 槽，先填数据+局部校验，
  //              剩余槽位再装全局校验；分区之间按"剩余容量优先大者"贪心分配
  //              全局校验，确保完全填满。
  //
  //  前置：sum(|partition_i|) == N 且 k + l + r == 2 * N。
  // ------------------------------------------------------------------------
  std::vector<FragmentPlacement> Place2N() const {
    int total = p_.k + p_.l + p_.r;

    // 检查容量预算（2 frag/node）
    int N = 0;
    for (const auto& part : partitions_) {
      N += static_cast<int>(part.member_nodes.size());
    }
    if (total != 2 * N) {
      char err_buf[160];
      snprintf(err_buf, sizeof(err_buf),
               "Place2N: total=%d != 2*N=%d (k=%d l=%d r=%d N=%d)",
               total, 2 * N, p_.k, p_.l, p_.r, N);
      throw std::invalid_argument(err_buf);
    }

    std::vector<FragmentPlacement> out;
    out.reserve(static_cast<size_t>(total));

    int lk = p_.local_k();
    std::vector<int> cursor(p_.l, 0);

    // 每分区容量 = 2 * |partition|
    std::vector<int> partition_capacity(p_.l, 0);
    for (int g = 0; g < p_.l; ++g) {
      partition_capacity[g] =
          2 * static_cast<int>(partitions_[g].member_nodes.size());
    }

    auto assign_one = [&](int g, int frag_id, FragmentPlacement::Kind kind) {
      const auto& nodes = partitions_[g].member_nodes;
      FragmentPlacement fp;
      fp.frag_id     = frag_id;
      fp.local_group = g;
      fp.node_id     = nodes[cursor[g] % nodes.size()];
      fp.kind        = kind;
      ++cursor[g];
      --partition_capacity[g];
      out.push_back(fp);
    };

    // ---- 1) 数据分片：data[g*lk + i] → 分区 g ----
    for (int g = 0; g < p_.l; ++g) {
      for (int i = 0; i < lk; ++i) {
        assign_one(g, g * lk + i, FragmentPlacement::kData);
      }
    }
    // ---- 2) 局部校验：local_parity[g] → 分区 g ----
    for (int g = 0; g < p_.l; ++g) {
      assign_one(g, p_.k + g, FragmentPlacement::kLocalParity);
    }
    // ---- 3) 全局校验：按 partition_capacity 贪心分配（剩余容量大者优先）----
    for (int i = 0; i < p_.r; ++i) {
      int best_g = -1;
      int best_remain = -1;
      for (int g = 0; g < p_.l; ++g) {
        if (partition_capacity[g] > best_remain) {
          best_remain = partition_capacity[g];
          best_g = g;
        }
      }
      if (best_g < 0 || best_remain <= 0) {
        throw std::runtime_error("Place2N: ran out of partition capacity");
      }
      assign_one(best_g, p_.k + p_.l + i, FragmentPlacement::kGlobalParity);
    }
    return out;
  }

  // 调试辅助：打印整张 placement 表
  static void Dump(const std::vector<FragmentPlacement>& placement,
                   const char* prefix = "[LRC-PLACE]") {
    printf("%s ===== Fragment Placement (%zu entries) =====\n",
           prefix, placement.size());
    for (const auto& fp : placement) {
      printf("%s   %s\n", prefix, fp.ToString().c_str());
    }
  }

 private:
  LrcParams                p_;
  std::vector<LocalGroup>  partitions_;
};

// ============================================================================
//  RsRandomPlacement — RS_3F 路径用的"每节点随机 2 块"放置记录
// ============================================================================
struct RsRandomPlacement {
  int frag_id = 0;   // [0, total_frags)
  int node_id = 0;   // [0, N)

  std::string ToString() const {
    char buf[64];
    snprintf(buf, sizeof(buf), "frag=%d node=%d", frag_id, node_id);
    return buf;
  }
};

// ----------------------------------------------------------------------------
//  BuildRsRandomPlacement — 随机洗牌 [0, total_frags) 后两两分配给 N 个 node
//
//  约束：total_frags == 2 * N。每个 node 恰好持有 2 个 frag。
//  用 seed 控制确定性，便于在测试中复现。
// ----------------------------------------------------------------------------
inline std::vector<RsRandomPlacement> BuildRsRandomPlacement(
    int N, int total_frags, std::uint64_t seed) {
  if (total_frags != 2 * N) {
    char err_buf[128];
    snprintf(err_buf, sizeof(err_buf),
             "BuildRsRandomPlacement: total_frags=%d != 2*N=%d",
             total_frags, 2 * N);
    throw std::invalid_argument(err_buf);
  }
  std::vector<int> pool;
  pool.reserve(total_frags);
  for (int i = 0; i < total_frags; ++i) pool.push_back(i);

  std::mt19937 rng(seed == 0 ? 1u : static_cast<unsigned>(seed));
  std::shuffle(pool.begin(), pool.end(), rng);

  std::vector<RsRandomPlacement> out;
  out.reserve(static_cast<size_t>(total_frags));
  for (int i = 0; i < total_frags; ++i) {
    RsRandomPlacement rp;
    rp.frag_id = pool[i];
    rp.node_id = i / 2;  // node 0 takes positions 0,1; node 1 takes 2,3; ...
    out.push_back(rp);
  }
  return out;
}

}  // namespace multiraft
