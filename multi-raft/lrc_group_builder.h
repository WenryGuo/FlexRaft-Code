#pragma once
// lrc_group_builder.h — LRC(k,l,r) Multi-Raft 分组算法
//
// [DEPRECATED] 此文件已废弃，仅供测试参考使用。
//
// 生产路径请使用 lrc_complementary_grouper.h 中的 LrcComplementaryGrouper 类。
// LrcComplementaryGrouper 提供延迟感知的分组算法，功能更完整。
//
// 核心概念：
//   - N 个物理节点组成一个 LRC 编码集群
//   - 每个 Raft Group 包含 l 个 Local Group（互补互斥）
//   - l 个 Local Group 组成一个能够存放一个条带的节点集合
//
// LRC 参数计算 (1)：
//   - k = floor(N/2) + 1        （数据分片数）
//   - l >= 2，取能被 k 整除的最大值
//   - r = N - k - l              （全局校验数）
//
// Group 成员计算 (2)：
//   - local_k = k / l
//   - g_small = floor(local_k) + 1 + floor(r/l)
//   - g_large = ceil(local_k) + 1 + ceil(r/l)
//   - 先随机选择 g_small 个节点作为 g_small 组
//   - 其余节点分配到 g_large 组
//
// 互补互斥分组：
//   - l 个 Local Group 互补互斥，完整覆盖所有 N 个节点
//   - 每个 Local Group 的节点数要么是 g_small 要么是 g_large
//   - 总节点数 = g_small * x + g_large * y = N
//     其中 x + y = l
//
// 互补分组关联记录（路由表设计）：
//   - 每个发起节点产生的 l 个分组通过 initiator_id 关联
//   - 路由条目记录 stripe_id -> [group_id_0, group_id_1, ...]
//   - group_id 结构：initiator_id * N + local_group_index
//
// 示例 (N=7)：
//   - k=4, l=2, r=1
//   - local_k=2, g_small=3, g_large=4
//   - 分配：g_small=3 个，g_large=4 个，3+4=7
//   - Local Group 0: 节点 {0,1,2}
//   - Local Group 1: 节点 {3,4,5,6}
//
// 节点实例数推导（关键）：
//   - 节点 i 发起的分组 0 (g=0) 包含节点 i
//   - 节点 i 发起的分组 1 (g=1) 不包含节点 i
//   - 节点 i 参与由自己发起的分组中包含自己的那个 → 1个
//   - 节点 i 参与其他 N-1 个节点发起的、包含自己的分组 → N-1 个
//   - 总计：1 + (N-1) = N 个 Raft 实例

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lrc_encoder.h"
#include "lrc_placement.h"  // LocalGroup

namespace multiraft {

// ============================================================================
//  LrcGroupParams — LRC 分组参数
// ============================================================================
struct LrcGroupParams {
  int N;          // 物理节点总数
  int k;          // 数据分片数 (k = floor(N/2) + 1)
  int l;          // Local Group 数 (能被 k 整除的最大值, l >= 2)
  int r;          // 全局校验数 (r = N - k - l)
  int local_k;    // 每个 Local Group 的数据块数 (k/l)
  int g_small;    // 小分组节点数 (floor(local_k) + 1 + floor(r/l))
  int g_large;    // 大分组节点数 (ceil(local_k) + 1 + ceil(r/l))

  // 校验参数合法性
  bool IsValid() const {
    return N >= 3 && k >= 2 && l >= 2 && r >= 0 &&
           k % l == 0 &&                    // k 能被 l 整除
           (g_small + g_large) == (local_k + 1 + 1);  // 分组节点数正确
  }

  // 转为可读字符串
  std::string ToString() const {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "LRC(N=%d,k=%d,l=%d,r=%d) local_k=%d g_small=%d g_large=%d",
             N, k, l, r, local_k, g_small, g_large);
    return buf;
  }
};

// ============================================================================
//  RaftGroup — 一个完整 Raft Group（包含 l 个 Local Group）
// ============================================================================
struct RaftGroup {
  int raft_group_id;                   // Raft Group ID [0, k-1] 或其他
  std::vector<LocalGroup> local_groups; // l 个 Local Group
  std::vector<int> all_nodes;          // 该 Raft Group 涉及的所有物理节点

  std::string ToString() const {
    std::string s = "RaftGroup" + std::to_string(raft_group_id) + ": ";
    for (const auto& lg : local_groups) {
      s += lg.ToString() + " ";
    }
    return s;
  }
};

// ============================================================================
//  以下类和方法已废弃，不再使用
// ============================================================================
//   - LrcGroupBuilder 类
//   - RoutingEntry 结构
//   - LrcMembershipResolver 类
//   - LrcGroupMembership 结构
//
// 请使用 LrcComplementaryGrouper 类替代。
// ============================================================================

}  // namespace multiraft
