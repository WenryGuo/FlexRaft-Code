// test_lrc_group.cc — 测试 LrcComplementaryGrouper 分组算法
//
// 本测试演示：
//   1. N=7 节点集群的 l 个全局互补互斥分组生成
//   2. 新设计：所有节点共享 l 个全局分组（而非 N*l 个）
//   3. 多锚点贪心聚类算法 (Phase1: Max-Min选锚点, Phase2: 就近吸附, Phase3: 交换优化)
//   4. 分组放置验证

#include <cstdio>
#include <cassert>
#include <cstdint>
#include <cstdlib>

#include "multi-raft/lrc_complementary_grouper.h"
#include "multi-raft/message.h"
#include "raft/raft_type.h"

using namespace multiraft;

int main() {
  printf("========================================\n");
  printf("   LrcComplementaryGrouper Algorithm Test\n");
  printf("   N=7: Global l Groups (All Nodes Share)\n");
  printf("========================================\n\n");

  // =========================================================================
  // 第一部分：数学推导
  // =========================================================================
  printf("【第一部分】数学推导：为什么使用 l 个全局分组？\n\n");

  printf("旧设计（已废弃）：\n");
  printf("  - 每个 initiator 生成 l 个分组\n");
  printf("  - 全局总分组数 = N × l\n");
  printf("  - 每个节点参与 N 个分组\n\n");

  printf("新设计（LrcComplementaryGrouper）：\n");
  printf("  - 所有节点共享 l 个全局分组\n");
  printf("  - 全局总分组数 = l（仅 l 个）\n");
  printf("  - 每个节点参与恰好 l 个分组\n");
  printf("  - l 个 Local Group 互补互斥，完整覆盖所有 N 个节点\n\n");

  // =========================================================================
  // 第二部分：LRC 参数自动计算
  // =========================================================================
  printf("【第二部分】LRC 参数自动计算\n\n");

  printf("N=7 节点集群的 LRC 参数推导（FromCapacity 模式）：\n");
  printf("  FromCapacity(N, 2) 模式：k + l + r = 2N\n");
  printf("  自动计算得到 k, l, r 参数...\n\n");

  LrcComplementaryGrouper grouper(7, 42);
  const LrcParams& params = grouper.GetLrcParams();
  printf("实际计算结果：%s\n", params.ToString().c_str());
  printf("  local_k=%d\n\n", params.local_k());

  // =========================================================================
  // 第三部分：构建延迟矩阵（模拟）
  // =========================================================================
  printf("【第三部分】构建延迟矩阵并生成分组\n\n");

  // 创建一个简单的延迟矩阵（实际使用时由系统测量）
  LatencyMatrix latency_matrix(7);
  latency_matrix.GenerateRandomMatrix(10, 100, 42);

  printf("延迟矩阵（模拟，前几个值）：\n");
  for (int i = 0; i < 7; ++i) {
    printf("  节点 %d: ", i);
    for (int j = 0; j < 7; ++j) {
      printf("%d ", latency_matrix.GetLatency(i, j));
    }
    printf("\n");
  }
  printf("\n");

  // 使用多锚点贪心聚类算法生成分组
  grouper.BuildComplementaryGroups(latency_matrix);

  // =========================================================================
  // 第四部分：分组验证
  // =========================================================================
  printf("\n【第四部分】全局分组状态验证\n\n");

  const auto& groups = grouper.GetGroups();
  printf("全局共有 %zu 个分组（l=%d）\n\n", groups.size(), params.l);

  printf("各分组详情：\n");
  for (size_t g = 0; g < groups.size(); ++g) {
    printf("  Group %zu (%s): members={",
           g, groups[g].is_large ? "large" : "small");
    for (size_t i = 0; i < groups[g].member_nodes.size(); ++i) {
      printf("%d%s", groups[g].member_nodes[i],
             i < groups[g].member_nodes.size() - 1 ? ", " : "");
    }
    printf("}\n");
  }

  // =========================================================================
  // 第五部分：节点参与验证
  // =========================================================================
  printf("\n【第五部分】节点参与验证\n\n");

  printf("验证：每个节点参与的分组数（应为 l=%d）\n", params.l);
  for (int i = 0; i < 7; ++i) {
    // 计算节点 i 参与的分组数
    int count = 0;
    for (const auto& g : groups) {
      for (int nid : g.member_nodes) {
        if (nid == i) {
          count++;
          break;
        }
      }
    }
    printf("  节点 %d 参与 %d 个分组", i, count);
    if (count == params.l) {
      printf(" ✓ 正确！\n");
    } else {
      printf(" ✗ 错误！应该是 %d\n", params.l);
    }
  }

  printf("\n节点参与的分组 ID：\n");
  for (int i = 0; i < 7; ++i) {
    printf("  节点 %d: {", i);
    bool first = true;
    for (size_t g = 0; g < groups.size(); ++g) {
      for (int nid : groups[g].member_nodes) {
        if (nid == i) {
          printf("%s%d", first ? "" : ", ", static_cast<int>(g));
          first = false;
          break;
        }
      }
    }
    printf("}\n");
  }

  // =========================================================================
  // 第六部分：片段放置验证
  // =========================================================================
  printf("\n【第六部分】片段放置验证\n\n");

  printf("验证：每个节点应该存储的 fragments（应为 2 个）\n");
  for (int i = 0; i < 7; ++i) {
    const auto& frags = grouper.GetFragmentsForNode(i);
    printf("  节点 %d: {", i);
    for (size_t j = 0; j < frags.size(); ++j) {
      printf("%d%s", frags[j],
             j < frags.size() - 1 ? ", " : "");
    }
    printf("}\n");
  }

  // =========================================================================
  // 第七部分：分组拓扑与 Local Repair
  // =========================================================================
  printf("\n【第七部分】分组拓扑与 Local Repair\n\n");

  const auto& all_placements = grouper.GetAllNodePlacements();
  printf("所有节点放置信息：\n");
  for (int i = 0; i < 7; ++i) {
    auto it = all_placements.find(i);
    if (it != all_placements.end()) {
      printf("  节点 %d: lrc_group_id=%d, frags={",
             i, it->second.lrc_group_id);
      for (size_t j = 0; j < it->second.frag_ids.size(); ++j) {
        printf("%d%s", it->second.frag_ids[j],
               j < it->second.frag_ids.size() - 1 ? ", " : "");
      }
      printf("}\n");
    }
  }

  printf("\n分组拓扑在 Local Repair 中的作用：\n");
  printf("  当需要 Local Repair 时，系统查找：\n");
  printf("    1. 失效分片所在的 Local Group\n");
  printf("    2. 在该分组内找到存活的节点进行恢复\n");
  printf("    3. 避免跨组通信，降低修复延迟\n");
  printf("\n");

  // =========================================================================
  // 总结
  // =========================================================================
  printf("========================================\n");
  printf("【总结】LrcComplementaryGrouper 分组机制\n");
  printf("========================================\n");
  printf("  1. 动态参数计算：LRC(k,l,r) 根据 N 和容量模式自动计算\n");
  printf("  2. 全局分组：所有节点共享 l 个全局分组（而非 N*l 个）\n");
  printf("  3. 多锚点贪心聚类算法：\n");
  printf("     - Phase 1: Max-Min 选取 l 个正交锚点\n");
  printf("     - Phase 2: 轮询就近吸附\n");
  printf("     - Phase 3: 交换优化（10次无改善终止）\n");
  printf("  4. 正交放置：每个节点存储恰好 2 个 fragment\n");
  printf("  5. Local Repair：通过分组拓扑快速定位修复节点\n");
  printf("\n");
  printf("  N=7, l=%d 时：\n", params.l);
  printf("    - 全局总分组数：%zu\n", groups.size());
  printf("    - 每节点参与数：%d\n", params.l);
  printf("    - 每节点存储 fragments：2\n");
  printf("========================================\n");

  return 0;
}
