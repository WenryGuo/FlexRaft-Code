#pragma once
// lrc_complementary_grouper.h — 多锚点贪心聚类 LRC 分组器
//
// 功能：
//   1. 基于延迟矩阵构建 l 个全局互补互斥分组
//   2. 多锚点贪心聚类算法（Phase1: Max-Min选锚点, Phase2: 就近吸附, Phase3: 交换优化）
//   3. 为每个节点提供正交放置映射（每个节点存储 2 个 fragment）
//
// 核心概念：
//   - 只需生成 l 个全局分组（所有 Raft 实例共享）
//   - l 个 Local Group 互补互斥，完整覆盖所有 N 个节点
//   - 组内节点两两延迟较低（拓扑相近）
//
// 正交放置策略：
//   - 每个物理节点存储恰好 2 个编码块
//   - 对于 LRC(k, l, r)，总 fragment 数 = k + l + r
//   - 采用 FromCapacity 模式：k + l + r = 2N（每节点 2 块）
//
// 多锚点贪心聚类算法：
//   Phase 1: 选取 l 个正交锚点（Max-Min 距离）
//   Phase 2: 轮询就近吸附（Round-Robin Nearest Assignment）
//   Phase 3: 交换优化（Swap-based Optimization, 10次无改善终止）
//
// 使用场景：
//   - 系统初始化时，根据延迟矩阵构建分组
//   - EncodeRaftEntry 使用分组信息进行正交放置
//   - sendAppendEntries 使用分组信息确定每个 peer 应接收的 fragments

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <numeric>
#include <queue>
#include <random>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lrc_encoder.h"
#include "lrc_placement.h"
#include "latency_matrix.h"
#include "message.h"  // FragmentPlacement

namespace multiraft {

// ============================================================================
//  NodePlacementInfo — 节点放置信息
// ============================================================================
struct NodePlacementInfo {
  int node_id;                  // 物理节点 ID
  std::vector<int> frag_ids;    // 该节点存储的 fragment IDs（通常为 2 个）
  int lrc_group_id;             // 该节点所属的 LRC group ID
};

// ============================================================================
//  LrcComplementaryGrouper — 多锚点贪心聚类 LRC 分组器
// ============================================================================
class LrcComplementaryGrouper {
 public:
  // ------------------------------------------------------------------------
  // 构造函数
  // ------------------------------------------------------------------------
  explicit LrcComplementaryGrouper(int N, std::uint64_t seed = 0)
      : N_(N),
        seed_(seed == 0 ? static_cast<std::uint64_t>(std::time(nullptr)) : seed) {
    lrc_params_ = LrcParams::FromCapacity(N_, 2);  // l=2 for simplicity

    printf("[LRC-COMPL-GROUP] N=%d, seed=%lu, %s\n",
           N_, seed_, lrc_params_.ToString().c_str());
  }

  // ------------------------------------------------------------------------
  // BuildComplementaryGroups: 多锚点贪心聚类算法
  //
  // Phase 1: 选取 l 个正交锚点（Max-Min 距离）
  //   1. 找最大延迟对作为 Anchor 0 和 Anchor 1
  //   2. 对剩余锚点，每次选 Max-Min 距离最大的节点
  //
  // Phase 2: 轮询就近吸附
  //   1. 初始化 groups[i] = {anchor[i]}
  //   2. 轮询各组，选平均延迟最低的节点加入
  //
  // Phase 3: 交换优化（10次无改善终止）
  //   1. 遍历所有跨组节点对
  //   2. 评估交换收益，执行有益交换
  // ------------------------------------------------------------------------
  void BuildComplementaryGroups(const LatencyMatrix& latency_matrix) {
    latency_matrix_ = &latency_matrix;
    groups_.clear();
    groups_.resize(lrc_params_.l);  // 只需 l 个全局分组

    printf("\n[LRC-COMPL-GROUP] ===== Multi-Anchor Greedy Clustering =====\n");
    printf("[LRC-COMPL-GROUP] N=%d, l=%d\n", N_, lrc_params_.l);

    // Phase 1: 选择 l 个正交锚点
    std::vector<int> anchors = Phase1_SelectOrthogonalAnchors();
    printf("[LRC-COMPL-GROUP] Phase 1: Anchors selected: {");
    for (int a : anchors) printf("%d ", a);
    printf("}\n");

    // Phase 2: 轮询就近吸附
    Phase2_RoundRobinAssignment(anchors);
    printf("[LRC-COMPL-GROUP] Phase 2: Round-robin assignment done\n");

    // Phase 3: 交换优化
    Phase3_SwapOptimization();
    printf("[LRC-COMPL-GROUP] Phase 3: Swap optimization done\n");

    // 构建节点到 fragments 的映射
    BuildNodeToFragmentsMapping();

    // 验证
    VerifyGroups();

    printf("[LRC-COMPL-GROUP] ===== Multi-Anchor Clustering Done =====\n");
  }

  // ------------------------------------------------------------------------
  // GetGroups: 获取全局 l 个分组（所有 RaftNode 共享）
  // ------------------------------------------------------------------------
  const std::vector<LocalGroup>& GetGroups() const { return groups_; }

  // ------------------------------------------------------------------------
  // GetGroupById: 获取指定 group_id 的分组
  // ------------------------------------------------------------------------
  const LocalGroup& GetGroupById(int group_id) const {
    static LocalGroup empty;
    if (group_id < 0 || group_id >= static_cast<int>(groups_.size())) return empty;
    return groups_[group_id];
  }

  // ------------------------------------------------------------------------
  // GetFragmentsForNode: 获取指定节点应该存储的 fragment IDs
  //
  // 返回值：通常为 2 个 fragment IDs
  // ------------------------------------------------------------------------
  const std::vector<int>& GetFragmentsForNode(int node_id) const {
    static std::vector<int> empty;
    if (node_id < 0 || node_id >= N_) return empty;
    return node_to_frags_[node_id];
  }

  // ------------------------------------------------------------------------
  // GetNodePlacement: 获取指定节点的完整放置信息
  // ------------------------------------------------------------------------
  const NodePlacementInfo* GetNodePlacement(int node_id) const {
    if (node_id < 0 || node_id >= N_) return nullptr;
    auto it = node_placement_.find(node_id);
    if (it == node_placement_.end()) return nullptr;
    return &it->second;
  }

  // ------------------------------------------------------------------------
  // GetAllNodePlacements: 获取所有节点的放置信息
  // ------------------------------------------------------------------------
  const std::unordered_map<int, NodePlacementInfo>& GetAllNodePlacements() const {
    return node_placement_;
  }

  // ------------------------------------------------------------------------
  // GetLrcParams: 获取 LRC 参数
  // ------------------------------------------------------------------------
  const LrcParams& GetLrcParams() const { return lrc_params_; }

  // ------------------------------------------------------------------------
  // GetN: 获取节点数
  // ------------------------------------------------------------------------
  int GetN() const { return N_; }

  // ------------------------------------------------------------------------
  // GetL: 获取 Local Group 数
  // ------------------------------------------------------------------------
  int GetL() const { return lrc_params_.l; }

  // ------------------------------------------------------------------------
  // GetNodePlacementsVector: 获取所有节点的放置向量（用于 EncodeRaftEntry）
  //
  // Frag ID 分配规则：
  //   frag_id = 0 ~ k-1      : 数据块 (k 个)
  //   frag_id = k ~ k+l-1    : 局部校验块 (l 个)
  //   frag_id = k+l ~ k+l+r-1 : 全局校验块 (r 个)
  //
  // 返回值：每个 fragment 的放置信息
  // ------------------------------------------------------------------------
  std::vector<FragmentPlacement> GetNodePlacementsVector() const {
    std::vector<FragmentPlacement> placements;
    placements.reserve(lrc_params_.total_shards());

    int k = lrc_params_.k;
    int l = lrc_params_.l;
    int r = lrc_params_.r;
    int local_k = lrc_params_.local_k();  // k / l

    // Phase 1: 数据块 (frag_id = 0 ~ k-1)
    // 每个局部组 g (0 ≤ g < l) 分配 local_k 个数据块
    // 规则：数据块分配到组内前 (group_size-1) 个节点，跳过最后节点（parity 节点）
    for (int g = 0; g < l; ++g) {
      const auto& group_nodes = groups_[g].member_nodes;
      int group_size = static_cast<int>(group_nodes.size());
      for (int i = 0; i < local_k; ++i) {
        int frag_id = g * local_k + i;
        FragmentPlacement fp;
        fp.frag_id = frag_id;
        fp.local_group = g;
        fp.node_id = group_nodes[i % (group_size - 1)];  // 修复：跳过 parity 节点
        fp.kind = FragmentPlacement::Kind::kData;
        placements.push_back(fp);
      }
    }

    // Phase 2: 局部校验块 (frag_id = k ~ k+l-1)
    // 每个 LRC Group 分配 1 个局部校验，分配给组内最后一个节点
    for (int g = 0; g < l; ++g) {
      FragmentPlacement fp;
      fp.frag_id = k + g;
      fp.local_group = g;
      fp.node_id = groups_[g].member_nodes.back();
      fp.kind = FragmentPlacement::Kind::kLocalParity;
      placements.push_back(fp);
    }

    // Phase 3: 全局校验块 (frag_id = k+l ~ k+l+r-1)
    // Derive from precomputed node_to_frags_ table to ensure consistency
    // with BuildNodeToFragmentsMapping() / GetFragmentsForNode()
    for (int node_id = 0; node_id < N_; ++node_id) {
      for (int frag_id : node_to_frags_[node_id]) {
        // Skip data (frag_id < k) and local parity (k <= frag_id < k+l)
        if (frag_id >= k + l) {
          FragmentPlacement fp;
          fp.frag_id = frag_id;
          fp.local_group = -1;
          fp.node_id = node_id;
          fp.kind = FragmentPlacement::Kind::kGlobalParity;
          placements.push_back(fp);
        }
      }
    }

    return placements;
  }

  // ------------------------------------------------------------------------
  // GetFragmentsForPeer: 获取指定 peer 应该接收的 fragment IDs
  // ------------------------------------------------------------------------
  std::vector<int> GetFragmentsForPeer(int peer_node_id) const {
    return GetFragmentsForNode(peer_node_id);
  }

  // ------------------------------------------------------------------------
  // GetLrcGroupIdForStripe: 获取指定条带应该使用的 LRC group ID
  // ------------------------------------------------------------------------
  int GetLrcGroupIdForNode(int node_id) const {
    auto* info = GetNodePlacement(node_id);
    if (info == nullptr) return -1;
    return info->lrc_group_id;
  }

  // ------------------------------------------------------------------------
  // PrintGroups: 打印所有分组（用于调试）
  // ------------------------------------------------------------------------
  void PrintGroups() const {
    printf("\n[LRC-COMPL-GROUP] ===== All Groups (l=%d) =====\n", lrc_params_.l);
    for (int g = 0; g < lrc_params_.l; ++g) {
      printf("[LRC-COMPL-GROUP] Group %d (size=%zu): {", g, groups_[g].member_nodes.size());
      for (int n : groups_[g].member_nodes) {
        printf("%d ", n);
      }
      printf("}\n");
    }

    // 计算并打印组内平均延迟
    printf("\n[LRC-COMPL-GROUP] Group intra-latency:\n");
    for (int g = 0; g < lrc_params_.l; ++g) {
      double avg_lat = CalculateGroupIntraLatency(g);
      printf("[LRC-COMPL-GROUP]   Group %d: avg=%.1f ms\n", g, avg_lat);
    }

    printf("\n[LRC-COMPL-GROUP] ===== Node -> Fragments Mapping =====\n");
    for (int n = 0; n < N_; ++n) {
      printf("[LRC-COMPL-GROUP] Node %d: frags={", n);
      for (int f : node_to_frags_[n]) {
        printf("%d ", f);
      }
      printf("}\n");
    }
  }

 private:
  // =========================================================================
  // Phase 1: 选择 l 个正交锚点（Max-Min 距离）
  // =========================================================================
  std::vector<int> Phase1_SelectOrthogonalAnchors() {
    std::vector<int> anchors;
    anchors.reserve(lrc_params_.l);

    // Step 1: 找最大延迟对作为前两个锚点
    int max_lat = -1;
    int anchor_a = -1, anchor_b = -1;
    for (int i = 0; i < N_; ++i) {
      for (int j = i + 1; j < N_; ++j) {
        int lat = latency_matrix_->GetLatency(i, j);
        if (lat > max_lat) {
          max_lat = lat;
          anchor_a = i;
          anchor_b = j;
        }
      }
    }
    anchors.push_back(anchor_a);
    anchors.push_back(anchor_b);
    printf("[LRC-PHASE1] Initial pair: (%d, %d) with latency=%d ms\n",
           anchor_a, anchor_b, max_lat);

    // Step 2: 对剩余锚点，使用 Max-Min 贪心选择
    // 维护每个未选节点到所有已选锚点的最小延迟
    std::vector<int> min_dist(N_, INT_MAX);  // min_dist[v] = min(dist(v, all anchors))
    for (int v = 0; v < N_; ++v) {
      min_dist[v] = latency_matrix_->GetLatency(v, anchor_a);
      min_dist[v] = std::min(min_dist[v], latency_matrix_->GetLatency(v, anchor_b));
    }
    min_dist[anchor_a] = -1;  // 标记为已选
    min_dist[anchor_b] = -1;

    while (static_cast<int>(anchors.size()) < lrc_params_.l) {
      // 找 max-min 距离最大的节点
      int best_node = -1;
      int best_min_dist = -1;
      for (int v = 0; v < N_; ++v) {
        if (min_dist[v] > best_min_dist) {
          best_min_dist = min_dist[v];
          best_node = v;
        }
      }

      if (best_node == -1) break;
      anchors.push_back(best_node);
      printf("[LRC-PHASE1] Selected anchor %d with min_dist=%d ms\n", best_node, best_min_dist);

      // 更新 min_dist
      min_dist[best_node] = -1;
      for (int v = 0; v < N_; ++v) {
        if (min_dist[v] >= 0) {
          int dist_to_new = latency_matrix_->GetLatency(v, best_node);
          min_dist[v] = std::min(min_dist[v], dist_to_new);
        }
      }
    }

    return anchors;
  }

  // =========================================================================
  // Phase 2: 轮询就近吸附
  // =========================================================================
  void Phase2_RoundRobinAssignment(const std::vector<int>& anchors) {
    // 初始化各组
    for (int g = 0; g < lrc_params_.l; ++g) {
      groups_[g].member_nodes.clear();
      groups_[g].member_nodes.push_back(anchors[g]);
      groups_[g].group_id = g;
      groups_[g].is_large = false;
    }

    // 未分配节点集合
    std::set<int> unassigned;
    for (int i = 0; i < N_; ++i) {
      bool is_anchor = false;
      for (int a : anchors) {
        if (i == a) { is_anchor = true; break; }
      }
      if (!is_anchor) unassigned.insert(i);
    }

    // 计算目标组大小
    int nodes_per_group = N_ / lrc_params_.l;
    int remainder = N_ % lrc_params_.l;

    // 分配锚点后的各组目标大小
    std::vector<int> target_sizes(lrc_params_.l, nodes_per_group);
    for (int i = 0; i < remainder; ++i) {
      target_sizes[i]++;  // 前 remainder 个组多大一个
    }

    // 轮询分配
    int group_idx = 0;
    int round = 0;
    while (!unassigned.empty()) {
      // 找到下一个未满的组
      int attempts = 0;
      while (static_cast<int>(groups_[group_idx].member_nodes.size()) >=
             static_cast<size_t>(target_sizes[group_idx])) {
        group_idx = (group_idx + 1) % lrc_params_.l;
        attempts++;
        if (attempts >= lrc_params_.l) break;  // 所有组都满了
      }
      if (attempts >= lrc_params_.l) break;

      // 在未分配节点中找平均延迟最低的节点
      int best_node = -1;
      int64_t best_avg_lat = INT64_MAX;
      int group_size = static_cast<int>(groups_[group_idx].member_nodes.size());

      for (int v : unassigned) {
        int64_t sum_lat = 0;
        for (int u : groups_[group_idx].member_nodes) {
          sum_lat += latency_matrix_->GetLatency(v, u);
        }
        int64_t avg_lat = (group_size > 0) ? (sum_lat / group_size) : 0;

        if (avg_lat < best_avg_lat) {
          best_avg_lat = avg_lat;
          best_node = v;
        }
      }

      if (best_node != -1) {
        groups_[group_idx].member_nodes.push_back(best_node);
        unassigned.erase(best_node);
      }

      group_idx = (group_idx + 1) % lrc_params_.l;
      round++;
    }
  }

  // =========================================================================
  // Phase 3: 交换优化（10次无改善终止）
  // =========================================================================
  void Phase3_SwapOptimization() {
    // 预计算：节点到各组的延迟之和
    // node_to_group_sum[node][gid] = 节点到组 gid 内所有节点延迟之和
    std::vector<std::vector<int64_t>> node_to_group_sum(N_,
        std::vector<int64_t>(lrc_params_.l, 0));

    for (int gid = 0; gid < lrc_params_.l; ++gid) {
      for (int node = 0; node < N_; ++node) {
        int64_t sum = 0;
        for (int u : groups_[gid].member_nodes) {
          sum += latency_matrix_->GetLatency(node, u);
        }
        node_to_group_sum[node][gid] = sum;
      }
    }

    // 预计算：各组内延迟之和
    std::vector<int64_t> group_latency_sum(lrc_params_.l, 0);
    for (int gid = 0; gid < lrc_params_.l; ++gid) {
      int64_t sum = 0;
      for (size_t i = 0; i < groups_[gid].member_nodes.size(); ++i) {
        for (size_t j = i + 1; j < groups_[gid].member_nodes.size(); ++j) {
          int u = groups_[gid].member_nodes[i];
          int v = groups_[gid].member_nodes[j];
          sum += latency_matrix_->GetLatency(u, v);
        }
      }
      group_latency_sum[gid] = sum;
    }

    int no_improve_count = 0;
    int iteration = 0;

    while (no_improve_count < 10) {
      int improvements_this_round = 0;

      // 遍历所有跨组节点对
      for (int gid_a = 0; gid_a < lrc_params_.l; ++gid_a) {
        for (int gid_b = gid_a + 1; gid_b < lrc_params_.l; ++gid_b) {
          // 遍历 A 组中的每个节点
          for (size_t i = 0; i < groups_[gid_a].member_nodes.size(); ++i) {
            int node_u = groups_[gid_a].member_nodes[i];

            // 遍历 B 组中的每个节点
            for (size_t j = 0; j < groups_[gid_b].member_nodes.size(); ++j) {
              int node_v = groups_[gid_b].member_nodes[j];

              // 计算交换收益
              // ΔTotal = lat_sum(v, A) + lat_sum(u, B) - lat_sum(u, A) - lat_sum(v, B)
              int64_t delta = node_to_group_sum[node_v][gid_a]
                            + node_to_group_sum[node_u][gid_b]
                            - node_to_group_sum[node_u][gid_a]
                            - node_to_group_sum[node_v][gid_b];

              if (delta < 0) {
                // 执行交换
                groups_[gid_a].member_nodes[i] = node_v;
                groups_[gid_b].member_nodes[j] = node_u;

                // 更新预计算数据
                node_to_group_sum[node_u][gid_a] = node_to_group_sum[node_v][gid_a];
                node_to_group_sum[node_v][gid_b] = node_to_group_sum[node_u][gid_b];

                // 交换延迟和
                int64_t temp = group_latency_sum[gid_a];
                group_latency_sum[gid_a] += delta;
                group_latency_sum[gid_b] -= delta;

                improvements_this_round++;
              }
            }
          }
        }
      }

      iteration++;
      if (improvements_this_round == 0) {
        no_improve_count++;
      } else {
        no_improve_count = 0;
      }

      printf("[LRC-PHASE3] Iteration %d: improvements=%d, no_improve=%d\n",
             iteration, improvements_this_round, no_improve_count);
    }

    printf("[LRC-PHASE3] Optimization finished after %d iterations\n", iteration);
  }

  // =========================================================================
  // CalculateGroupIntraLatency: 计算组内平均节点对延迟
  // =========================================================================
  double CalculateGroupIntraLatency(int gid) const {
    if (groups_[gid].member_nodes.size() < 2) return 0.0;

    int64_t sum = 0;
    int pair_count = 0;
    for (size_t i = 0; i < groups_[gid].member_nodes.size(); ++i) {
      for (size_t j = i + 1; j < groups_[gid].member_nodes.size(); ++j) {
        int u = groups_[gid].member_nodes[i];
        int v = groups_[gid].member_nodes[j];
        sum += latency_matrix_->GetLatency(u, v);
        pair_count++;
      }
    }

    return (pair_count > 0) ? (static_cast<double>(sum) / pair_count) : 0.0;
  }

  // =========================================================================
  // BuildNodeToFragmentsMapping: 构建节点到 fragments 的映射
  //
  // Frag ID 分配规则：
  //   frag_id = 0 ~ k-1      : 数据块 (k 个)
  //   frag_id = k ~ k+l-1    : 局部校验块 (l 个)
  //   frag_id = k+l ~ k+l+r-1 : 全局校验块 (r 个)
  //
  // 两步走算法：
  //   Step 1: 分配数据块 + 局部校验块
  //   Step 2: 遍历每个组，补足全局校验块到每节点恰好 2 个
  // =========================================================================
  void BuildNodeToFragmentsMapping() {
    node_to_frags_.clear();
    node_to_frags_.resize(N_);
    node_placement_.clear();

    int k = lrc_params_.k;
    int l = lrc_params_.l;
    int r = lrc_params_.r;
    int local_k = lrc_params_.local_k();  // k / l

    // ========== Step 1: 分配数据块 [0, k) ==========
    // 规则：数据块分配到组内前 (group_size-1) 个节点
    //       局部校验块分配到组内最后一个节点
    //       确保数据块和局部校验块不在同一节点
    int frag_id = 0;
    for (int g = 0; g < l; ++g) {
      const auto& group_nodes = groups_[g].member_nodes;
      int group_size = static_cast<int>(group_nodes.size());
      // 数据块只分配到前 (group_size-1) 个节点，跳过最后节点（parity 节点）
      for (int i = 0; i < local_k && frag_id < k; ++i) {
        int node_id = group_nodes[i % (group_size - 1)];
        node_to_frags_[node_id].push_back(frag_id);
        ++frag_id;
      }
    }

    // ========== Step 2: 分配局部校验块 [k, k+l) ==========
    // frag_id = k+g 对应 LRC Group g，分配给组内最后一个节点
    for (int g = 0; g < l; ++g) {
      const auto& group_nodes = groups_[g].member_nodes;
      int node_id = group_nodes.back();
      node_to_frags_[node_id].push_back(k + g);
    }

    // ========== Step 3: 全局校验块填充 [k+l, k+l+r) ==========
    // 遍历每个组的成员，如果节点块数 < 2，分配全局校验块
    int global_frag_id = k + l;

    // 第一轮：遍历每个组的成员
    for (int g = 0; g < l; ++g) {
      for (int node_id : groups_[g].member_nodes) {
        if (static_cast<int>(node_to_frags_[node_id].size()) < 2) {
          node_to_frags_[node_id].push_back(global_frag_id);
          ++global_frag_id;
        }
      }
    }

    // 如果还有剩余全局校验块，继续遍历直到分完
    while (global_frag_id < k + l + r) {
      for (int n = 0; n < N_ && global_frag_id < k + l + r; ++n) {
        if (static_cast<int>(node_to_frags_[n].size()) < 2) {
          node_to_frags_[n].push_back(global_frag_id);
          ++global_frag_id;
        }
      }
    }

    // 构建 NodePlacementInfo
    for (int n = 0; n < N_; ++n) {
      NodePlacementInfo info;
      info.node_id = n;
      info.frag_ids = node_to_frags_[n];
      info.lrc_group_id = -1;

      // 确定所属 LRC group
      // 优先：通过数据块所在组确定 (frag_id < k)
      // 回退：如果只有局部校验块 (k <= frag_id < k+l)，frag_id - k 就是 group_id
      // 最终：检查 groups_ 中节点属于哪个 group
      for (int fid : info.frag_ids) {
        if (fid < k) {
          // 数据块：frag_id / local_k = group_id (每个组有 local_k 个数据块)
          info.lrc_group_id = fid / local_k;
          break;
        } else if (fid < k + l && info.lrc_group_id < 0) {
          // 局部校验块：frag_id - k = group_id (frag_id = k + g)
          info.lrc_group_id = fid - k;
        }
      }

      // 最终回退：如果仍无法确定，查找节点属于哪个 group
      if (info.lrc_group_id < 0) {
        for (int g = 0; g < l; ++g) {
          const auto& group_nodes = groups_[g].member_nodes;
          for (int node_in_group : group_nodes) {
            if (node_in_group == n) {
              info.lrc_group_id = g;
              break;
            }
          }
          if (info.lrc_group_id >= 0) break;
        }
      }

      // 验证：确保 lrc_group_id 有效
      if (info.lrc_group_id < 0 || info.lrc_group_id >= l) {
        printf("[LRC-COMPL-GROUP] WARNING: Node %d lrc_group_id=%d invalid (k=%d l=%d)\n",
               n, info.lrc_group_id, k, l);
      }

      node_placement_[n] = info;
    }

    // 验证：每节点恰好 2 个 fragment
    bool all_ok = true;
    for (int n = 0; n < N_; ++n) {
      if (static_cast<int>(node_to_frags_[n].size()) != 2) {
        printf("[LRC-COMPL-GROUP] WARNING: Node %d has %zu fragments (expected 2)\n",
               n, node_to_frags_[n].size());
        all_ok = false;
      }
    }
    if (all_ok) {
      printf("[LRC-COMPL-GROUP] BuildNodeToFragmentsMapping: all %d nodes have exactly 2 fragments\n",
             N_);
    }
  }

  // =========================================================================
  // VerifyGroups: 验证分组的正确性
  // =========================================================================
  void VerifyGroups() const {
    printf("\n[LRC-COMPL-GROUP] ===== Verification =====\n");

    // 验证1：恰好 l 个组
    if (static_cast<int>(groups_.size()) != lrc_params_.l) {
      printf("[LRC-COMPL-GROUP] ERROR: %zu groups (expected %d)\n",
             groups_.size(), lrc_params_.l);
    }

    // 验证2：所有节点都被分配
    std::set<int> all_assigned;
    for (int gid = 0; gid < lrc_params_.l; ++gid) {
      for (int n : groups_[gid].member_nodes) {
        if (all_assigned.count(n)) {
          printf("[LRC-COMPL-GROUP] ERROR: Node %d assigned to multiple groups\n", n);
        }
        all_assigned.insert(n);
      }
    }
    if (static_cast<int>(all_assigned.size()) != N_) {
      printf("[LRC-COMPL-GROUP] ERROR: Only %zu nodes assigned (expected %d)\n",
             all_assigned.size(), N_);
    }

    // 验证3：组大小合理（差距不超过 1）
    size_t min_size = SIZE_MAX, max_size = 0;
    for (int gid = 0; gid < lrc_params_.l; ++gid) {
      min_size = std::min(min_size, groups_[gid].member_nodes.size());
      max_size = std::max(max_size, groups_[gid].member_nodes.size());
    }
    if (max_size - min_size > 1) {
      printf("[LRC-COMPL-GROUP] WARNING: Group size imbalance: min=%zu, max=%zu\n",
             min_size, max_size);
    }

    printf("[LRC-COMPL-GROUP] Verification complete: %d nodes in %d groups\n",
           N_, lrc_params_.l);

    // 调用约束验证
    ValidatePlacementConstraints();
  }

  // =========================================================================
  //  ValidatePlacementConstraints: 验证 LRC 放置约束
  // =========================================================================
  bool ValidatePlacementConstraints() const {
    printf("\n[LRC-COMPL-GROUP] ===== Validating Placement Constraints =====\n");

    int k = lrc_params_.k;
    int l = lrc_params_.l;
    int local_k = lrc_params_.local_k();
    bool all_ok = true;

    // 约束1：每节点恰好 2 个 fragment
    for (int n = 0; n < N_; ++n) {
      int frag_count = static_cast<int>(node_to_frags_[n].size());
      if (frag_count != 2) {
        printf("[LRC-VALIDATE] ERROR: Node %d has %d fragments (expected 2)\n", n, frag_count);
        all_ok = false;
      }
    }

    // 约束2：数据块和局部校验块不在同一节点
    for (int g = 0; g < l; ++g) {
      const auto& group_nodes = groups_[g].member_nodes;
      int parity_node = group_nodes.back();

      // 找出该组数据块的分配节点
      for (int i = 0; i < local_k; ++i) {
        int frag_id = g * local_k + i;
        for (int n = 0; n < N_; ++n) {
          for (int fid : node_to_frags_[n]) {
            if (fid == frag_id) {
              if (n == parity_node) {
                printf("[LRC-VALIDATE] ERROR: Data frag %d and local parity %d both on node %d\n",
                       frag_id, k + g, n);
                all_ok = false;
              }
              break;
            }
          }
        }
      }
    }

    // 约束3：组内节点数 >= local_k + 1
    for (int g = 0; g < l; ++g) {
      int group_size = static_cast<int>(groups_[g].member_nodes.size());
      if (group_size < local_k + 1) {
        printf("[LRC-VALIDATE] ERROR: Group %d has %d nodes, need at least %d\n",
               g, group_size, local_k + 1);
        all_ok = false;
      }
    }

    if (all_ok) {
      printf("[LRC-VALIDATE] All constraints satisfied!\n");
    }

    printf("[LRC-COMPL-GROUP] ===== Validation Complete =====\n\n");
    return all_ok;
  }

  // =========================================================================
  // 成员变量
  // =========================================================================
  int N_;                          // 物理节点总数
  std::uint64_t seed_;             // 随机种子
  LrcParams lrc_params_;           // LRC 参数
  const LatencyMatrix* latency_matrix_ = nullptr;  // 延迟矩阵
  std::vector<LocalGroup> groups_; // 全局 l 个分组
  std::vector<std::vector<int>> node_to_frags_;  // 节点 -> fragments 映射
  std::unordered_map<int, NodePlacementInfo> node_placement_;  // 节点放置信息
};

}  // namespace multiraft
