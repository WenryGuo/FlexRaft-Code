// test_lrc_params.cc — 独立测试 LRC 参数计算

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <random>
#include <cstring>

// ==========================================================================
//  LrcParams — 从 lrc_encoder.h 复制
// ==========================================================================
struct LrcParams {
  int k;  // 数据分片总数
  int l;  // Local Group 数
  int r;  // 全局校验分片数

  int local_k() const { return k / l; }
  int total_shards() const { return k + l + r; }
  double storage_overhead() const {
    return static_cast<double>(total_shards()) / k;
  }

  // 根据集群规模自动推导参数
  static LrcParams AutoFromClusterSize(int N) {
    if (N < 3)
      throw std::invalid_argument("N must be >= 3");
    int F = N / 2;           // 容错性：N = 2F + 1
    int k = F + 1;           // 数据分片数

    // 找满足 l >= 2 且 l <= F 且 k % l == 0 的最大 l
    int best_l = 1;
    for (int cand = F; cand >= 2; --cand) {
      if (k % cand == 0) {
        best_l = cand;
        break;
      }
    }
    int l = best_l;
    int r = F - l;
    return LrcParams{k, l, r};
  }

  std::string ToString() const {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "LRC(k=%d, l=%d, r=%d) local_k=%d overhead=%.2fx",
             k, l, r, local_k(), storage_overhead());
    return buf;
  }
};

// ==========================================================================
//  LocalGroup — 单个 Local Group
// ==========================================================================
struct LocalGroup {
  int group_id;
  std::vector<int> member_nodes;
  bool is_large;

  std::string ToString() const {
    char buf[256];
    snprintf(buf, sizeof(buf), "LG%d(%s): {", group_id, is_large ? "L" : "S");
    for (size_t i = 0; i < member_nodes.size(); ++i) {
      if (i > 0) snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ",");
      snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%d", member_nodes[i]);
    }
    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "}");
    return std::string(buf);
  }
};

// ==========================================================================
//  LrcGroupBuilder — LRC 分组算法
// ==========================================================================
class LrcGroupBuilder {
 public:
  explicit LrcGroupBuilder(int N, int k, int l, int r)
      : N_(N), k_(k), l_(l), r_(r) {
    local_k_ = k_ / l_;
    // g_small = floor(local_k) + 1 + floor(r/l)
    // g_large = ceil(local_k) + 1 + ceil(r/l)
    int floor_r_div_l = r_ / l_;
    int ceil_r_div_l = (r_ + l_ - 1) / l_;
    g_small_ = local_k_ + 1 + floor_r_div_l;
    g_large_ = local_k_ + 1 + ceil_r_div_l;
  }

  std::vector<LocalGroup> BuildLocalGroups(std::uint64_t seed = 0) {
    if (seed == 0) seed = time(nullptr);
    printf("[INFO] Seed: %lu\n", seed);

    // 计算分配方案
    int g_small_count, g_large_count;
    if (g_small_ == g_large_) {
      g_small_count = l_;
      g_large_count = 0;
    } else {
      int denom = g_large_ - g_small_;
      int numer = N_ - g_small_ * l_;
      g_large_count = (numer >= 0) ? (numer / denom) : 0;
      g_small_count = l_ - g_large_count;
    }

    printf("[INFO] Distribution: g_small=%d (x%d) + g_large=%d (x%d) = %d nodes\n",
           g_small_, g_small_count, g_large_, g_large_count,
           g_small_ * g_small_count + g_large_ * g_large_count);

    // 创建 l 个 Local Group
    std::vector<LocalGroup> groups(l_);
    for (int i = 0; i < l_; ++i) {
      groups[i].group_id = i;
      groups[i].is_large = (i < g_large_count);
    }

    // 打乱节点
    std::vector<int> pool;
    for (int i = 0; i < N_; ++i) pool.push_back(i);
    std::mt19937 rng(seed);
    std::shuffle(pool.begin(), pool.end(), rng);

    // 分配节点
    int idx = 0;
    for (int i = 0; i < l_; ++i) {
      int size = groups[i].is_large ? g_large_ : g_small_;
      for (int j = 0; j < size && idx < N_; ++j) {
        groups[i].member_nodes.push_back(pool[idx++]);
      }
    }

    return groups;
  }

 private:
  int N_, k_, l_, r_, local_k_, g_small_, g_large_;
};

// ==========================================================================
//  主程序
// ==========================================================================
int main() {
  printf("================================================================\n");
  printf("         LRC(k,l,r) Parameter Calculator & Group Builder\n");
  printf("================================================================\n\n");

  // 测试不同规模集群的 LRC 参数
  int cluster_sizes[] = {3, 5, 7, 9, 11, 13, 15, 17, 19, 21};

  for (int N : cluster_sizes) {
    printf("\n>>> N = %2d nodes <<<\n", N);
    LrcParams p = LrcParams::AutoFromClusterSize(N);
    printf("    %s\n", p.ToString().c_str());

    // 验证公式: N = 2F + 1, F = N/2, k = F+1, l = best, r = F - l
    int F = N / 2;
    printf("    N = 2F + 1 = 2*%d + 1 = %d ✓\n", F, N);
    printf("    k = F + 1 = %d + 1 = %d\n", F, p.k);
    printf("    l = max divisor of k where l <= F: %d\n", p.l);
    printf("    r = F - l = %d - %d = %d\n", F, p.l, p.r);
    printf("    total_shards = k + l + r = %d + %d + %d = %d\n",
           p.k, p.l, p.r, p.total_shards());

    // 构建 Local Groups (使用固定 seed 以便复现)
    LrcGroupBuilder builder(N, p.k, p.l, p.r);
    auto groups = builder.BuildLocalGroups(42);

    printf("    Local Groups:\n");
    for (const auto& g : groups) {
      printf("      %s\n", g.ToString().c_str());
    }

    // 验证互补互斥
    std::vector<int> seen(N, 0);
    for (const auto& g : groups) {
      for (int node : g.member_nodes) {
        if (seen[node]++) {
          printf("    [ERROR] Node %d appears in multiple groups!\n", node);
        }
      }
    }
    int total_assigned = 0;
    for (const auto& g : groups) total_assigned += g.member_nodes.size();
    if (total_assigned == N) {
      printf("    [OK] All %d nodes assigned exactly once\n", N);
    } else {
      printf("    [ERROR] Only %d/%d nodes assigned!\n", total_assigned, N);
    }
  }

  printf("\n================================================================\n");
  printf("                      Test Complete\n");
  printf("================================================================\n");
  return 0;
}
