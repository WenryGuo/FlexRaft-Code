// multi_raft_client.cc
//
// Multi-Raft 专用客户端 —— 写入 + 读取正确性验证
//
// 用法：
//   ./multi_raft_client --conf ../conf/multi-raft-7-lrc.conf --id 99 \
//                       --size 1024 --write_num 100
//
// 核心路径：
//   RoutePut(key, val) → hash(key)%num_groups → DetectLeaderForGroup(group_id)
//                      → 发 kPut RPC → RegisterKeyRoute(key, group_id)
//   RouteGet(key, &val) → 查 key_to_group_ → DetectLeaderForGroup(group_id)
//                       → 发 kGet RPC → 值比对

#include <gflags/gflags.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "client.h"
#include "config.h"
#include "util.h"
#include "../raft/util.h"

DEFINE_string(conf,      "",  "Path to the Multi-Raft cluster configuration file");
DEFINE_int32 (id,       -1,  "The client Id");
DEFINE_string(size,     "",  "The size of the values (e.g. 128, 1K, 4K)");
DEFINE_int32 (write_num, 0,  "The number of write operations to execute");
DEFINE_int32 (retry,     0,  "Number of retries for leader detection (0 = unlimited)");
DEFINE_bool  (verbose,   false, "Print per-key write/read details");
DEFINE_int32 (concurrent_clients, 1, "Number of concurrent client threads");
DEFINE_int32 (concurrent_write_rounds, 1, "Number of rounds; each round writes all keys concurrently");

using KvPair = std::pair<std::string, std::string>;
const int kVerboseInterval = 100;

// ---------------------------------------------------------------------------
//  数据结构 — 复用 bench_client 的风格
// ---------------------------------------------------------------------------
struct BenchConfiguration {
  std::string key_prefix;
  std::string value_prefix;
  int         bench_put_cnt;
  int         bench_put_size;
};

struct OperationStat {
  uint64_t op_latency = 0;
  uint64_t commit_latency = 0;
  uint64_t apply_latency = 0;
  std::string key;
  int group_id = -1;

  OperationStat() = default;

  OperationStat(uint64_t op,
                uint64_t commit,
                uint64_t apply,
                std::string k,
                int gid)
      : op_latency(op),
        commit_latency(commit),
        apply_latency(apply),
        key(std::move(k)),
        group_id(gid) {}

  std::string ToString() const {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "[OpLatency=%llu us][CommitLatency=%llu us][ApplyLatency=%llu us]"
             " key=%s group=%d%s",
             (unsigned long long)op_latency,
             (unsigned long long)commit_latency,
             (unsigned long long)apply_latency,
             key.c_str(),
             group_id,
             (commit_latency == 0 && apply_latency == 0) ? " (NO_RAFT)" : "");
    return std::string(buf);
  }
};

struct LatencyPercentiles {
  uint64_t avg;
  uint64_t p50;
  uint64_t p95;
  uint64_t p99;
  uint64_t max;
};

static LatencyPercentiles ComputePercentiles(std::vector<uint64_t> latencies) {
  if (latencies.empty()) return {0, 0, 0, 0, 0};
  std::sort(latencies.begin(), latencies.end());
  size_t n = latencies.size();
  auto pick = [&](double q) -> uint64_t {
    if (n == 0) return 0;
    size_t idx = static_cast<size_t>(n * q);
    if (idx >= n) idx = n - 1;
    return latencies[idx];
  };
  uint64_t sum = std::accumulate(latencies.begin(), latencies.end(), 0ULL);
  return {sum / n, pick(0.50), pick(0.95), pick(0.99), latencies.back()};
}

struct LatencyAnalysis {
  LatencyPercentiles op;
  LatencyPercentiles commit;
  LatencyPercentiles apply;
};

static LatencyAnalysis AnalyzeLatency(const std::vector<OperationStat>& data) {
  if (data.empty()) return {{0,0,0,0,0}, {0,0,0,0,0}, {0,0,0,0,0}};
  std::vector<uint64_t> op_lat, commit_lat, apply_lat;
  op_lat.reserve(data.size());
  commit_lat.reserve(data.size());
  apply_lat.reserve(data.size());
  for (const auto& s : data) {
    op_lat.push_back(s.op_latency);
    commit_lat.push_back(s.commit_latency);
    apply_lat.push_back(s.apply_latency);
  }
  return {ComputePercentiles(std::move(op_lat)),
          ComputePercentiles(std::move(commit_lat)),
          ComputePercentiles(std::move(apply_lat))};
}

// ---------------------------------------------------------------------------
//  辅助函数
// ---------------------------------------------------------------------------
static void BuildBench(const BenchConfiguration& cfg,
                       std::vector<KvPair>* bench) {
  const std::string value_suffix(cfg.bench_put_size, '\0');
  for (int i = 1; i <= cfg.bench_put_cnt; ++i) {
    std::string key = cfg.key_prefix + std::to_string(i);
    std::string val = cfg.value_prefix + std::to_string(i) + value_suffix;
    bench->push_back({key, val});
  }
}

static void Dump(const std::vector<OperationStat>& stats,
                 std::ofstream& ofs) {
  for (const auto& s : stats) {
    ofs << s.ToString() << "\n";
  }
}

// ---------------------------------------------------------------------------
//  Concurrent execution helpers
// ---------------------------------------------------------------------------

static std::mutex g_print_mutex;

struct WorkerWriteResult {
  std::vector<OperationStat> stats;
  std::string thread_name;
};

struct WorkerReadResult {
  std::vector<OperationStat> stats;
  int match_count = 0;
  int mismatch_count = 0;
  int fail_count = 0;

  WorkerReadResult() = default;
};

static WorkerWriteResult RunWorkerWrite(
    const kv::KvClusterConfig& cluster_cfg,
    int base_client_id,
    int num_groups,
    const std::vector<KvPair>& slice,
    int worker_idx) {
  int client_id = base_client_id * 100 + worker_idx;
  auto client = new kv::KvServiceClient(cluster_cfg, client_id);
  client->SetNumGroups(num_groups);

  char prefix[16];
  snprintf(prefix, sizeof(prefix), "W%d", worker_idx);

  WorkerWriteResult result;
  result.thread_name = prefix;

  for (size_t i = 0; i < slice.size(); ++i) {
    const auto& p = slice[i];

    int gid = kv::KvServiceClient::GetGroupForKey(p.first, num_groups);
    if (FLAGS_verbose) {
      std::lock_guard<std::mutex> lock(g_print_mutex);
      printf("  [%s WRITE %zu] key=%s → group=%d\n",
             prefix, i + 1, p.first.c_str(), gid);
      fflush(stdout);
    }

    auto t0 = raft::util::NowTime();
    kv::OperationResults stat = client->RoutePut(p.first, p.second);
    auto dura = raft::util::DurationToMicros(t0, raft::util::NowTime());

    if (stat.err == kv::kOk) {
      result.stats.emplace_back(
          static_cast<uint64_t>(dura),
          stat.commit_elapse_time,
          stat.apply_elapse_time,
          p.first, gid);
      if (FLAGS_verbose) {
        std::lock_guard<std::mutex> lock(g_print_mutex);
        printf("  [%s WRITE %zu] SUCCESS (group=%d, %llu us)\n",
               prefix, i + 1, gid, (unsigned long long)dura);
      }
    } else {
      std::lock_guard<std::mutex> lock(g_print_mutex);
      printf("[%s WRITE %zu] FAILED (err=%d, key=%s)\n",
             prefix, i + 1, stat.err, p.first.c_str());
    }

    size_t done_cnt = i + 1;
    if (done_cnt % kVerboseInterval == 0) {
      std::lock_guard<std::mutex> lock(g_print_mutex);
      printf("\r[%s Write Progress] %zu / %zu", prefix, done_cnt, slice.size());
      fflush(stdout);
    }
  }

  delete client;
  return result;
}

static std::vector<OperationStat> ConcurrentWritePhase(
    const kv::KvClusterConfig& cluster_cfg,
    const std::vector<KvPair>& bench,
    int num_workers,
    int base_client_id,
    int num_groups) {
  std::vector<std::vector<KvPair>> slices(num_workers);
  for (size_t i = 0; i < bench.size(); ++i) {
    slices[i % num_workers].push_back(bench[i]);
  }

  std::vector<WorkerWriteResult> worker_results(num_workers);
  std::vector<std::thread> threads;
  threads.reserve(num_workers);

  for (int w = 0; w < num_workers; ++w) {
    threads.emplace_back([&worker_results, &cluster_cfg, &slices, w,
                         base_client_id, num_groups]() {
      worker_results[w] = RunWorkerWrite(cluster_cfg, base_client_id, num_groups,
                                         slices[w], w);
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  std::vector<OperationStat> combined;
  size_t total_succeeded = 0;
  for (const auto& r : worker_results) {
    total_succeeded += r.stats.size();
    combined.insert(combined.end(), r.stats.begin(), r.stats.end());
  }
  printf("\n[ConcurrentWrite] %zu ops, %zu succeeded across %d workers\n",
         bench.size(), total_succeeded, num_workers);
  fflush(stdout);

  return combined;
}

static WorkerReadResult RunWorkerRead(
    const kv::KvClusterConfig& cluster_cfg,
    int base_client_id,
    int num_groups,
    const std::vector<KvPair>& slice,
    int worker_idx) {
  int client_id = base_client_id * 100 + 1000 + worker_idx;
  auto client = new kv::KvServiceClient(cluster_cfg, client_id);
  client->SetNumGroups(num_groups);

  char prefix[16];
  snprintf(prefix, sizeof(prefix), "R%d", worker_idx);

  WorkerReadResult result{};
  for (const auto& p : slice) {
    auto t0 = raft::util::NowTime();
    std::string get_val;
    kv::OperationResults stat = client->RouteGet(p.first, &get_val);
    auto dura = raft::util::DurationToMicros(t0, raft::util::NowTime());

    if (stat.err == kv::kOk && get_val == p.second) {
      result.match_count++;
      result.stats.emplace_back(
          static_cast<uint64_t>(dura), 0, 0, p.first, -1);
    } else if (stat.err == kv::kOk) {
      result.mismatch_count++;
      result.stats.emplace_back(
          static_cast<uint64_t>(dura), 0, 0, p.first, -1);
    } else {
      result.fail_count++;
    }
  }

  delete client;
  return result;
}

static WorkerReadResult ConcurrentReadPhase(
    const kv::KvClusterConfig& cluster_cfg,
    const std::vector<KvPair>& bench,
    int num_workers,
    int base_client_id,
    int num_groups) {
  std::vector<std::vector<KvPair>> slices(num_workers);
  for (size_t i = 0; i < bench.size(); ++i) {
    slices[i % num_workers].push_back(bench[i]);
  }

  std::vector<WorkerReadResult> worker_results(num_workers);
  std::vector<std::thread> threads;
  threads.reserve(num_workers);

  for (int w = 0; w < num_workers; ++w) {
    threads.emplace_back([&worker_results, &cluster_cfg, &slices, w,
                         base_client_id, num_groups]() {
      worker_results[w] = RunWorkerRead(cluster_cfg, base_client_id, num_groups,
                                        slices[w], w);
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  WorkerReadResult combined;
  {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    for (const auto& r : worker_results) {
      combined.match_count += r.match_count;
      combined.mismatch_count += r.mismatch_count;
      combined.fail_count += r.fail_count;
      combined.stats.insert(combined.stats.end(), r.stats.begin(), r.stats.end());
    }
  }

  int total = combined.match_count + combined.mismatch_count + combined.fail_count;
  printf("[ConcurrentRead] %d matched, %d mismatched, %d failed / %zu total\n",
         combined.match_count, combined.mismatch_count, combined.fail_count, bench.size());
  fflush(stdout);

  return combined;
}

// ---------------------------------------------------------------------------
//  ExecuteMultiRaftBench: routes to serial or concurrent based on flags
// ---------------------------------------------------------------------------

static void ExecuteMultiRaftBench(kv::KvServiceClient* client,
                                  const kv::KvClusterConfig& cluster_cfg,
                                  const std::vector<KvPair>& bench) {
  int workers = FLAGS_concurrent_clients;
  int rounds  = FLAGS_concurrent_write_rounds;

  printf("[Execution Process] mode=MULTI-RAFT-ROUTING\n");
  printf("[Bench] total_writes=%zu, value_size=%zu\n",
         bench.size(),
         bench.empty() ? 0 : bench[0].second.size());
  printf("[Concurrency] workers=%d, write_rounds=%d\n", workers, rounds);
  fflush(stdout);

  int base_client_id = client->ClientId();
  int num_groups     = static_cast<int>(cluster_cfg.size());

  std::vector<OperationStat> all_stats;
  std::vector<OperationStat> read_stats;
  int last_read_match = 0;

  auto write_start = std::chrono::steady_clock::now();

  for (int round = 0; round < rounds; ++round) {
    if (rounds > 1) {
      printf("\n===== ROUND %d / %d =====\n", round + 1, rounds);
      fflush(stdout);
    }

    std::vector<OperationStat> round_stats;
    if (workers <= 1) {
      // Serial fallback: inline single-client logic
      round_stats.clear();
      for (size_t i = 0; i < bench.size(); ++i) {
        const auto& p = bench[i];
        int gid = kv::KvServiceClient::GetGroupForKey(p.first, num_groups);
        if (FLAGS_verbose) {
          printf("  [WRITE %zu] key=%s → group=%d\n", i + 1, p.first.c_str(), gid);
          fflush(stdout);
        }
        auto t0 = raft::util::NowTime();
        kv::OperationResults stat = client->RoutePut(p.first, p.second);
        auto dura = raft::util::DurationToMicros(t0, raft::util::NowTime());
        if (stat.err == kv::kOk) {
          round_stats.emplace_back(
              static_cast<uint64_t>(dura),
              stat.commit_elapse_time,
              stat.apply_elapse_time,
              p.first, gid);
          if (FLAGS_verbose) {
            printf("  [WRITE %zu] SUCCESS (group=%d, %llu us)\n",
                   i + 1, gid, (unsigned long long)dura);
          }
        } else {
          printf("[WRITE %zu] FAILED (err=%d, key=%s)\n", i + 1, stat.err, p.first.c_str());
        }
        if ((i + 1) % kVerboseInterval == 0) {
          printf("\r[Write Progress] %zu / %zu", i + 1, bench.size());
          fflush(stdout);
        }
      }
      printf("\r[Write Complete] %zu ops, %zu succeeded\n", bench.size(), round_stats.size());
      fflush(stdout);
      // Serial read phase
      printf("\n[Read Verification] checking %zu keys...\n", bench.size());
      std::vector<OperationStat> read_stats;
      int succ_cnt = 0;
      for (const auto& p : bench) {
        auto t0 = raft::util::NowTime();
        std::string get_val;
        kv::OperationResults stat = client->RouteGet(p.first, &get_val);
        auto dura = raft::util::DurationToMicros(t0, raft::util::NowTime());
        if (stat.err == kv::kOk && get_val == p.second) {
          ++succ_cnt;
          read_stats.emplace_back(
              static_cast<uint64_t>(dura), 0, 0, p.first, -1);
        } else if (stat.err == kv::kOk) {
          printf("  [READ]  key=%s MISMATCH\n", p.first.c_str());
          read_stats.emplace_back(
              static_cast<uint64_t>(dura), 0, 0, p.first, -1);
        } else {
          printf("  [READ]  key=%s FAILED (err=%d)\n", p.first.c_str(), stat.err);
          read_stats.emplace_back(
              static_cast<uint64_t>(dura), 0, 0, p.first, -1);
        }
      }
      printf("[Read Verification] %d / %zu matched\n", succ_cnt, bench.size());

      size_t payload_size = bench.empty() ? 0 : bench[0].second.size();
      auto write_lat = AnalyzeLatency(round_stats);
      auto read_lat = AnalyzeLatency(read_stats);
      uint64_t sum_op_latency = 0;
      for (const auto& s : round_stats) sum_op_latency += s.op_latency;

      printf("\n========================================\n");
      printf("         MULTI-RAFT BENCH RESULTS\n");
      printf("========================================\n");
      printf("Client ID:          %d\n", base_client_id);
      printf("Data Payload Size:  %zu bytes\n", payload_size);
      printf("Concurrent Workers: %d\n", workers);
      printf("Write Ops:         %zu\n", round_stats.size());
      printf("Read Match:         %d / %zu\n", succ_cnt, bench.size());
      printf("\n[Write Latency]\n");
      printf("Avg Op Latency:     %llu us  |  P50: %llu  P95: %llu  P99: %llu  Max: %llu\n",
             (unsigned long long)write_lat.op.avg,
             (unsigned long long)write_lat.op.p50,
             (unsigned long long)write_lat.op.p95,
             (unsigned long long)write_lat.op.p99,
             (unsigned long long)write_lat.op.max);
      printf("Avg Commit Lat:     %.2f ms\n",  (double)write_lat.commit.avg / 1000.0);
      printf("Avg Apply Lat:      %.2f ms  |  P50: %.2f  P95: %.2f  P99: %.2f  Max: %.2f\n",
             (double)write_lat.apply.avg / 1000.0,
             (double)write_lat.apply.p50 / 1000.0,
             (double)write_lat.apply.p95 / 1000.0,
             (double)write_lat.apply.p99 / 1000.0,
             (double)write_lat.apply.max / 1000.0);
      printf("\n[Read Latency]\n");
      printf("Avg Read Latency:   %llu us  |  P50: %llu  P95: %llu  P99: %llu  Max: %llu\n",
             (unsigned long long)read_lat.op.avg,
             (unsigned long long)read_lat.op.p50,
             (unsigned long long)read_lat.op.p95,
             (unsigned long long)read_lat.op.p99,
             (unsigned long long)read_lat.op.max);
      printf("========================================\n");
      fflush(stdout);
      std::ofstream ofs("multi_raft_results");
      Dump(round_stats, ofs);
      printf("[Log] detailed results saved to multi_raft_results\n");
    } else {
      round_stats = ConcurrentWritePhase(
          cluster_cfg, bench, workers, base_client_id, num_groups);

      if (round == rounds - 1) {
        printf("\n[Read Verification] checking %zu keys with %d workers...\n",
               bench.size(), workers);
        WorkerReadResult read_result = ConcurrentReadPhase(
            cluster_cfg, bench, workers, base_client_id, num_groups);
        last_read_match = read_result.match_count;
        read_stats = std::move(read_result.stats);
      }
    }

    for (const auto& s : round_stats) {
      all_stats.push_back(s);
    }
  }

  auto write_end = std::chrono::steady_clock::now();
  auto total_duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
      write_end - write_start).count();

  if (workers > 1 && !all_stats.empty()) {
    size_t payload_size = bench.empty() ? 0 : bench[0].second.size();
    auto write_lat = AnalyzeLatency(all_stats);
    auto read_lat = AnalyzeLatency(read_stats);
    uint64_t sum_op_latency = 0;
    for (const auto& s : all_stats) sum_op_latency += s.op_latency;
    double throughput = (all_stats.size() * 1000000.0) / (total_duration_us ?: 1);

    printf("\n========================================\n");
    printf("      MULTI-RAFT BENCH RESULTS\n");
    printf("========================================\n");
    printf("Client ID:          %d\n", base_client_id);
    printf("Data Payload Size:  %zu bytes\n", payload_size);
    printf("Concurrent Workers: %d\n", workers);
    printf("Write Rounds:       %d\n", rounds);
    printf("Write Ops (total):  %zu\n", all_stats.size());
    printf("Read Match:         %d / %zu\n", last_read_match, bench.size());
    printf("\n[Write Latency]\n");
    printf("Avg Op Latency:     %llu us  |  P50: %llu  P95: %llu  P99: %llu  Max: %llu\n",
           (unsigned long long)write_lat.op.avg,
           (unsigned long long)write_lat.op.p50,
           (unsigned long long)write_lat.op.p95,
           (unsigned long long)write_lat.op.p99,
           (unsigned long long)write_lat.op.max);
    printf("Avg Commit Lat:     %.2f ms\n",  (double)write_lat.commit.avg / 1000.0);
    printf("Avg Apply Lat:      %.2f ms  |  P50: %.2f  P95: %.2f  P99: %.2f  Max: %.2f\n",
           (double)write_lat.apply.avg / 1000.0,
           (double)write_lat.apply.p50 / 1000.0,
           (double)write_lat.apply.p95 / 1000.0,
           (double)write_lat.apply.p99 / 1000.0,
           (double)write_lat.apply.max / 1000.0);
    printf("\n[Read Latency]\n");
    printf("Avg Read Latency:   %llu us  |  P50: %llu  P95: %llu  P99: %llu  Max: %llu\n",
           (unsigned long long)read_lat.op.avg,
           (unsigned long long)read_lat.op.p50,
           (unsigned long long)read_lat.op.p95,
           (unsigned long long)read_lat.op.p99,
           (unsigned long long)read_lat.op.max);
    printf("\n[Throughput]\n");
    printf("Sum Op Latency:     %llu us  (cumulative per-op latency)\n",
           (unsigned long long)sum_op_latency);
    printf("Total Duration:     %lld ms  (wall-clock time)\n",
           (long long)(total_duration_us / 1000));
    printf("Throughput:         %.2f ops/sec\n", throughput);
    printf("========================================\n");
    fflush(stdout);

    std::ofstream ofs("multi_raft_results");
    Dump(all_stats, ofs);
    printf("[Log] detailed results saved to multi_raft_results\n");
  }
}

// ===========================================================================
//  Main
// ===========================================================================
int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_conf.empty() || FLAGS_id < 0) {
    std::cerr << "Usage: " << argv[0] << "\n"
              << "  --conf <file>                Multi-Raft config file\n"
              << "  --id <client_id>             Unique client ID\n"
              << "  --size <val_size>            Value size (e.g. 128, 1K, 4K)\n"
              << "  --write_num <N>              Number of writes\n"
              << "  --concurrent_clients <N>     Concurrent client threads (default: 1)\n"
              << "  --concurrent_write_rounds <R> Write rounds; each writes all keys (default: 1)\n"
              << "  --verbose                    Print per-key details\n";
    return 1;
  }

  // ---- 解析配置 ----
  auto cluster_cfg = ParseConfigurationFile(FLAGS_conf);
  if (cluster_cfg.empty()) {
    std::cerr << "ERROR: empty cluster config from " << FLAGS_conf << "\n";
    return 1;
  }

  int client_id  = FLAGS_id;
  int val_size   = ParseCommandSize(FLAGS_size);
  int put_cnt    = FLAGS_write_num;
  int num_groups = static_cast<int>(cluster_cfg.size());

  printf("========================================\n");
  printf("   Multi-Raft Client\n");
  printf("========================================\n");
  printf("Config:               %s\n", FLAGS_conf.c_str());
  printf("Client ID:           %d\n", client_id);
  printf("Value size:           %d bytes\n", val_size);
  printf("Write count:          %d\n", put_cnt);
  printf("Num groups:           %d\n", num_groups);
  printf("Concurrent clients:   %d\n", FLAGS_concurrent_clients);
  printf("Write rounds:         %d\n", FLAGS_concurrent_write_rounds);
  printf("========================================\n\n");

  // ---- 构建测试数据 ----
  auto key_prefix   = "mr-key-" + std::to_string(client_id);
  auto value_prefix = "mr-value-" + std::to_string(client_id) + "-";
  BenchConfiguration bench_cfg{key_prefix, value_prefix, put_cnt, val_size};

  std::vector<KvPair> bench;
  BuildBench(bench_cfg, &bench);

  // ---- 创建 Multi-Raft 客户端 ----
  auto client = new kv::KvServiceClient(cluster_cfg, client_id);
  client->SetNumGroups(num_groups);

  printf("[Client] Initialized: %zu servers, %d groups\n",
         cluster_cfg.size(), num_groups);

  // ---- 执行写入 + 读取验证 ----
  ExecuteMultiRaftBench(client, cluster_cfg, bench);

  delete client;
  return 0;
}
