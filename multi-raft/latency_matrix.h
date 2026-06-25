#pragma once
// latency_matrix.h — 延迟矩阵管理
//
// 功能：
//   1. 存储 N×N 延迟矩阵（单位：毫秒）
//   2. 生成随机延迟矩阵（30ms~300ms）
//   3. 提供延迟查询接口
//   4. 按延迟排序的节点查询（用于延迟感知的 LRC 分组）
//
// 使用场景：
//   - 系统初始化时生成模拟延迟矩阵
//   - LrcComplementaryGrouper 使用延迟矩阵进行互补互斥分组

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <numeric>
#include <random>
#include <vector>

namespace multiraft {

// ============================================================================
//  LatencyMatrix — N×N 延迟矩阵
// ============================================================================
class LatencyMatrix {
 public:
  // 构造函数：指定节点数 N
  explicit LatencyMatrix(int N)
      : N_(N),
        matrix_(N, std::vector<int>(N, 0)),
        sorted_peers_(N) {
    assert(N >= 2 && "LatencyMatrix: N must be >= 2");
  }

  // 默认构造函数（用于声明）
  LatencyMatrix() : N_(0) {}

  // ------------------------------------------------------------------------
  // GenerateRandomMatrix: 生成随机延迟矩阵
  //
  // 对角线元素为 0（自身延迟）
  // 其他元素在 [min_ms, max_ms] 范围内随机
  // 矩阵是对称的：matrix[i][j] == matrix[j][i]
  // ------------------------------------------------------------------------
  void GenerateRandomMatrix(int min_ms = 30, int max_ms = 300,
                            std::uint64_t seed = 0) {
    if (seed == 0) {
      seed = static_cast<std::uint64_t>(std::time(nullptr));
    }
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(min_ms, max_ms);

    N_ = static_cast<int>(matrix_.size());
    for (int i = 0; i < N_; ++i) {
      matrix_[i][i] = 0;  // 自身延迟为 0
      for (int j = i + 1; j < N_; ++j) {
        int latency = dist(rng);
        matrix_[i][j] = latency;
        matrix_[j][i] = latency;  // 对称
      }
    }

    // 更新排序后的节点列表
    UpdateSortedPeers();
  }

  // ------------------------------------------------------------------------
  // GetLatency: 获取两点间的延迟
  // ------------------------------------------------------------------------
  int GetLatency(int src, int dst) const {
    assert(src >= 0 && src < N_ && "LatencyMatrix: src out of range");
    assert(dst >= 0 && dst < N_ && "LatencyMatrix: dst out of range");
    return matrix_[src][dst];
  }

  // ------------------------------------------------------------------------
  // GetAvgLatency: 获取某节点的平均延迟（到所有其他节点）
  // ------------------------------------------------------------------------
  float GetAvgLatency(int node_id) const {
    assert(node_id >= 0 && node_id < N_ && "LatencyMatrix: node_id out of range");
    int64_t sum = 0;
    for (int j = 0; j < N_; ++j) {
      if (j != node_id) {
        sum += matrix_[node_id][j];
      }
    }
    return static_cast<float>(sum) / static_cast<float>(N_ - 1);
  }

  // ------------------------------------------------------------------------
  // GetSortedPeersByLatency: 获取按延迟排序的节点列表
  //
  // 返回值：pair<int, float> = {节点ID, 平均延迟}
  // ------------------------------------------------------------------------
  const std::vector<std::pair<int, float>>& GetSortedPeersByLatency(
      int node_id) const {
    assert(node_id >= 0 && node_id < N_ && "LatencyMatrix: node_id out of range");
    return sorted_peers_[node_id];
  }

  // ------------------------------------------------------------------------
  // GetLatencyScore: 计算节点间的延迟亲和度分数
  //
  // 分数 = 1.0 / (1.0 + latency)
  // 用于选择延迟较低的节点组成 LRC 组
  // ------------------------------------------------------------------------
  float GetLatencyScore(int src, int dst) const {
    return 1.0f / (1.0f + static_cast<float>(GetLatency(src, dst)));
  }

  // ------------------------------------------------------------------------
  // GetAvgLatencyBetween: 计算两节点间延迟占总平均延迟的比例
  //
  // 返回值：(latency_ij / avg_i) - 1.0
  // 负值表示低于平均，正值表示高于平均
  // ------------------------------------------------------------------------
  float GetLatencyRatio(int src, int dst) const {
    float avg = GetAvgLatency(src);
    if (avg < 0.001f) return 0.0f;  // 避免除零
    return static_cast<float>(GetLatency(src, dst)) / avg - 1.0f;
  }

  // ------------------------------------------------------------------------
  // GetN: 获取节点数
  // ------------------------------------------------------------------------
  int GetN() const { return N_; }

  // ------------------------------------------------------------------------
  // IsInitialized: 检查矩阵是否已初始化
  // ------------------------------------------------------------------------
  bool IsInitialized() const { return N_ > 0; }

  // ------------------------------------------------------------------------
  // PrintMatrix: 打印延迟矩阵（用于调试）
  // ------------------------------------------------------------------------
  void PrintMatrix() const {
    printf("\n[LATENCY-MATRIX] N=%d\n", N_);
    printf("[LATENCY-MATRIX]     ");
    for (int j = 0; j < N_; ++j) {
      printf("%5d", j);
    }
    printf("\n[LATENCY-MATRIX]     ");
    for (int j = 0; j < N_; ++j) {
      printf("------");
    }
    printf("\n");
    for (int i = 0; i < N_; ++i) {
      printf("[LATENCY-MATRIX] %3d |", i);
      for (int j = 0; j < N_; ++j) {
        printf("%5d", matrix_[i][j]);
      }
      printf(" | avg=%.1f\n", GetAvgLatency(i));
    }

    // 打印排序后的邻居
    printf("\n[LATENCY-MATRIX] Sorted neighbors by latency:\n");
    for (int i = 0; i < N_; ++i) {
      printf("[LATENCY-MATRIX] Node %d: ", i);
      const auto& peers = GetSortedPeersByLatency(i);
      for (size_t k = 0; k < std::min(static_cast<size_t>(5), peers.size()); ++k) {
        printf("(%d,%.1f) ", peers[k].first, peers[k].second);
      }
      if (peers.size() > 5) {
        printf("... (+%zu more)", peers.size() - 5);
      }
      printf("\n");
    }
  }

 private:
  // ------------------------------------------------------------------------
  // UpdateSortedPeers: 更新每个节点的延迟排序列表
  // ------------------------------------------------------------------------
  void UpdateSortedPeers() {
    for (int i = 0; i < N_; ++i) {
      sorted_peers_[i].clear();
      sorted_peers_[i].reserve(N_ - 1);

      for (int j = 0; j < N_; ++j) {
        if (j != i) {
          sorted_peers_[i].emplace_back(j, static_cast<float>(matrix_[i][j]));
        }
      }

      // 按延迟升序排序
      std::sort(sorted_peers_[i].begin(), sorted_peers_[i].end(),
                [](const auto& a, const auto& b) { return a.second < b.second; });
    }
  }

  int N_;  // 节点数
  std::vector<std::vector<int>> matrix_;  // N×N 延迟矩阵（毫秒）
  // 每个节点的邻居按延迟排序：{节点ID, 延迟值}
  std::vector<std::vector<std::pair<int, float>>> sorted_peers_;
};

}  // namespace multiraft
