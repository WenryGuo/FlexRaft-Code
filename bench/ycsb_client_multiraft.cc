// ycsb_client_multiraft.cc
//
// Multi-Raft YCSB 客户端 —— 支持 100% 读、100% 写、混合工作负载
//
// 用法：
//   ./ycsb_client_multiraft --conf ../conf/multi-raft-7-lrc.conf --id 99 \
//                           --client_num 10 --size 4K --op_count 10000 \
//                           --type YCSB_WRITE
//
// Workload types:
//   YCSB_WRITE  : 100% Put operations
//   YCSB_READ   : 100% Get operations
//   YCSB_MIXED  : 50% Put + 50% Get
//
// 核心路径：
//   RoutePut(key, val) → hash(key)%num_groups → DetectLeaderForGroup(group_id)
//                      → 发 kPut RPC
//   RouteGet(key, &val) → hash(key)%num_groups → DetectLeaderForGroup(group_id)
//                       → 发 kGet RPC

#include <gflags/gflags.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "client.h"
#include "config.h"
#include "util.h"
#include "../raft/util.h"

DEFINE_string(conf,       "",  "Path to the Multi-Raft cluster configuration file");
DEFINE_int32 (id,        -1,  "The client Id (base client ID)");
DEFINE_string(size,       "",  "Size of values, e.g. 512, 4K, 1M");
DEFINE_int32 (op_count,   0,   "Number of operations to execute");
DEFINE_int32 (client_num, 1,   "Number of concurrent client threads");
DEFINE_string(type, "YCSB_WRITE",
              "Workload type: YCSB_WRITE (100%% put) | YCSB_READ (100%% get) | YCSB_MIXED (50%% put)");
DEFINE_int32 (warmup_ops, 50, "Number of warmup/pre-write ops for any workload (default: 50)");
DEFINE_bool  (verbose,    false, "Print per-operation details");
DEFINE_bool  (skip_warmup, false, "Skip warmup phase for YCSB_READ (data must already exist in DB)");

using KvPair = std::pair<std::string, std::string>;
const int kVerboseInterval = 1000;

// ---------------------------------------------------------------------------
//  YCSB 类型定义
// ---------------------------------------------------------------------------
enum YCSBOpType {
  kPut = 0,
  kGet = 1,
};

enum YCSBBenchType {
  YCSB_WRITE = 0,
  YCSB_READ  = 1,
  YCSB_MIXED = 2,
};

static const char* YCSBTypeToString(YCSBOpType type) {
  switch (type) {
    case kPut: return "Put";
    case kGet: return "Get";
    default:   return "Unknown";
  }
}

static const char* YCSBBenchToString(YCSBBenchType type) {
  switch (type) {
    case YCSB_WRITE: return "YCSB_WRITE (100% Put)";
    case YCSB_READ:  return "YCSB_READ (100% Get)";
    case YCSB_MIXED: return "YCSB_MIXED (50% Put + 50% Get)";
    default:          return "Unknown";
  }
}

// ---------------------------------------------------------------------------
//  数据结构
// ---------------------------------------------------------------------------
struct YCSBOperation {
  YCSBOpType type;
  std::string key;
  std::string value;
};

struct BenchConfiguration {
  std::string key_prefix;
  std::string value_prefix;
  int         bench_op_cnt;
  int         bench_value_size;
  YCSBBenchType bench_type;
};

struct OperationStat {
  YCSBOpType type;
  uint64_t   op_latency = 0;
  uint64_t   commit_latency = 0;
  uint64_t   apply_latency = 0;
  int        group_id = -1;

  std::string ToString(int val_size) const {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "[Type=%s][Size=%dK][OpLat=%.2f ms][CommitLat=%.2f ms][ApplyLat=%.2f ms] group=%d",
             YCSBTypeToString(type),
             val_size / 1024,
             (double)op_latency / 1000.0,
             (double)commit_latency / 1000.0,
             (double)apply_latency / 1000.0,
             group_id);
    return std::string(buf);
  }
};

struct LatencyPercentiles {
  uint64_t avg = 0;
  uint64_t p50 = 0;
  uint64_t p95 = 0;
  uint64_t p99 = 0;
  uint64_t max = 0;
};

struct LatencyAnalysis {
  LatencyPercentiles op;
  LatencyPercentiles commit;
  LatencyPercentiles apply;
};

struct WorkerResult {
  std::vector<OperationStat> stats;
  int put_count = 0;
  int get_count = 0;
  int fail_count = 0;
};

// ---------------------------------------------------------------------------
//  辅助函数
// ---------------------------------------------------------------------------
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

static LatencyAnalysis AnalyzeLatency(const std::vector<OperationStat>& data,
                                      YCSBOpType type) {
  std::vector<uint64_t> op_lat, commit_lat, apply_lat;
  for (const auto& s : data) {
    if (s.type == type) {
      op_lat.push_back(s.op_latency);
      commit_lat.push_back(s.commit_latency);
      apply_lat.push_back(s.apply_latency);
    }
  }
  return {ComputePercentiles(std::move(op_lat)),
          ComputePercentiles(std::move(commit_lat)),
          ComputePercentiles(std::move(apply_lat))};
}

// 按操作类型聚合统计数据，写入一条汇总行（不打印每条明细）
static void DumpAggregated(const WorkerResult& bench_result,
                           std::ofstream& ofs,
                           int val_size,
                           int64_t wall_elapsed_ms) {
  // --- 收集 Put 统计 ---
  std::vector<uint64_t> put_op, put_commit, put_apply;
  std::map<int, int> put_group_cnt;
  int put_total = 0;
  for (const auto& s : bench_result.stats) {
    if (s.type == kPut) {
      put_op.push_back(s.op_latency);
      put_commit.push_back(s.commit_latency);
      put_apply.push_back(s.apply_latency);
      put_group_cnt[s.group_id]++;
      put_total++;
    }
  }

  // --- 收集 Get 统计 ---
  std::vector<uint64_t> get_op;
  std::map<int, int> get_group_cnt;
  int get_total = 0;
  for (const auto& s : bench_result.stats) {
    if (s.type == kGet) {
      get_op.push_back(s.op_latency);
      get_group_cnt[s.group_id]++;
      get_total++;
    }
  }

  // --- 计算 IOPS 和 带宽（使用 benchmark 纯墙上时间） ---
  double throughput_iops = bench_result.stats.size() * 1000.0 / wall_elapsed_ms;
  double bandwidth_mbps = 0.0;
  if (wall_elapsed_ms > 0) {
    bandwidth_mbps = (double)val_size * bench_result.stats.size() * 8.0
                     / (wall_elapsed_ms / 1000.0) / 1e6;
  }

  // --- 输出 Put 汇总行 ---
  if (put_total > 0) {
    auto put_lat = ComputePercentiles(put_op);
    auto put_commit_avg = std::accumulate(put_commit.begin(), put_commit.end(), 0ULL)
                          / put_commit.size();
    auto put_apply_lat = ComputePercentiles(put_apply);

    char buf[1024];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "[Type=Put][Size=%dK]"
                    "[AvgOpLat=%.2f ms][AvgCommitLat=%.2f ms][AvgApplyLat=%.2f ms]"
                    "[P50OpLat=%.2f ms][P95OpLat=%.2f ms][P99OpLat=%.2f ms][MaxOpLat=%.2f ms]",
                    val_size / 1024,
                    (double)put_lat.avg / 1000.0,
                    (double)put_commit_avg / 1000.0,
                    (double)put_apply_lat.avg / 1000.0,
                    (double)put_lat.p50 / 1000.0,
                    (double)put_lat.p95 / 1000.0,
                    (double)put_lat.p99 / 1000.0,
                    (double)put_lat.max / 1000.0);

    std::vector<std::pair<int, int>> sorted_groups(put_group_cnt.begin(), put_group_cnt.end());
    std::sort(sorted_groups.begin(), sorted_groups.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& p : sorted_groups) {
      double pct = 100.0 * p.second / put_total;
      pos += snprintf(buf + pos, sizeof(buf) - pos, " group%d=%.1f%%", p.first, pct);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, " [TotalOps=%d]", put_total);
    ofs << buf << "\n";
  }

  // --- 输出 Get 汇总行 ---
  if (get_total > 0) {
    auto get_lat = ComputePercentiles(get_op);

    char buf[1024];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "[Type=Get][Size=%dK]"
                    "[AvgOpLat=%.2f ms]"
                    "[P50OpLat=%.2f ms][P95OpLat=%.2f ms][P99OpLat=%.2f ms][MaxOpLat=%.2f ms]",
                    val_size / 1024,
                    (double)get_lat.avg / 1000.0,
                    (double)get_lat.p50 / 1000.0,
                    (double)get_lat.p95 / 1000.0,
                    (double)get_lat.p99 / 1000.0,
                    (double)get_lat.max / 1000.0);

    std::vector<std::pair<int, int>> sorted_groups(get_group_cnt.begin(), get_group_cnt.end());
    std::sort(sorted_groups.begin(), sorted_groups.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& p : sorted_groups) {
      double pct = 100.0 * p.second / get_total;
      pos += snprintf(buf + pos, sizeof(buf) - pos, " group%d=%.1f%%", p.first, pct);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, " [TotalOps=%d]", get_total);
    ofs << buf << "\n";
  }

  // --- 输出汇总行（带宽和 IOPS） ---
  ofs << "[Throughput=" << throughput_iops << " ops/sec]"
      << "[TotalWallTime=" << wall_elapsed_ms << " ms]"
      << "[Bandwidth=" << bandwidth_mbps << " Mbps]"
      << "[ClientNum=" << FLAGS_client_num << "][ValueSize=" << val_size / 1024 << "K]\n";
}

// ---------------------------------------------------------------------------
//  YCSB Workload 生成
// ---------------------------------------------------------------------------

static void BuildYCSBBench(const BenchConfiguration& cfg,
                           std::vector<YCSBOperation>* bench,
                           int key_offset = 0) {
  for (int i = 0; i < cfg.bench_op_cnt; ++i) {
    auto key_id = key_offset + i;  // 顺序: key_offset, key_offset+1, ...
    auto key = raft::util::MakeKey(key_id, 64);
    YCSBOperation op;

    switch (cfg.bench_type) {
      case YCSB_WRITE:
        op.type = kPut;
        op.key = key;
        op.value = raft::util::MakeValue(key_id, cfg.bench_value_size);
        break;

      case YCSB_READ:
        op.type = kGet;
        op.key = key;
        break;

      case YCSB_MIXED: {
        int r = rand() % 100;
        if (r < 50) {
          op.type = kPut;
          op.key = key;
          op.value = raft::util::MakeValue(key_id, cfg.bench_value_size);
        } else {
          op.type = kGet;
          op.key = key;
        }
        break;
      }
    }

    bench->push_back(std::move(op));
  }
}

// ---------------------------------------------------------------------------
//  并发执行 helpers
// ---------------------------------------------------------------------------
static std::mutex g_print_mutex;

static WorkerResult RunWorker(
    const kv::KvClusterConfig& cluster_cfg,
    int base_client_id,
    int num_groups,
    const std::vector<YCSBOperation>& slice,
    int worker_idx,
    int num_workers) {
  int client_id = base_client_id * 100 + worker_idx;
  auto client = new kv::KvServiceClient(cluster_cfg, client_id, num_workers);
  client->SetNumGroups(num_groups);

  char prefix[16];
  snprintf(prefix, sizeof(prefix), "W%d", worker_idx);

  WorkerResult result;

  for (size_t i = 0; i < slice.size(); ++i) {
    const auto& op = slice[i];

    int gid = kv::KvServiceClient::GetGroupForKey(op.key, num_groups);
    auto t0 = raft::util::NowTime();

    if (op.type == kPut) {
      kv::OperationResults stat = client->RoutePut(op.key, op.value);
      auto dura = raft::util::DurationToMicros(t0, raft::util::NowTime());

      if (stat.err == kv::kOk) {
        result.stats.emplace_back(
            OperationStat{kPut, static_cast<uint64_t>(dura),
                         stat.commit_elapse_time, stat.apply_elapse_time, gid});
        result.put_count++;
      } else {
        result.fail_count++;
        if (FLAGS_verbose) {
          std::lock_guard<std::mutex> lock(g_print_mutex);
          printf("[%s PUT %zu] FAILED (err=%d)\n",
                 prefix, i + 1, stat.err);
        }
      }
    } else {
      std::string get_val;
      kv::OperationResults stat = client->RouteGet(op.key, &get_val);
      auto dura = raft::util::DurationToMicros(t0, raft::util::NowTime());

      if (stat.err == kv::kOk) {
        result.stats.emplace_back(
            OperationStat{kGet, static_cast<uint64_t>(dura), 0, 0, gid});
        result.get_count++;
      } else {
        result.fail_count++;
        std::lock_guard<std::mutex> lock(g_print_mutex);
        printf("[%s GET %zu] FAILED err=%d key='%s' group=%d\n",
               prefix, i + 1, stat.err, op.key.c_str(), gid);
        fflush(stdout);
      }
    }

    if ((i + 1) % kVerboseInterval == 0) {
      std::lock_guard<std::mutex> lock(g_print_mutex);
      printf("\r[%s Progress] %zu / %zu", prefix, i + 1, slice.size());
      fflush(stdout);
    }
  }

  delete client;
  return result;
}

static WorkerResult ConcurrentExecute(
    const kv::KvClusterConfig& cluster_cfg,
    const std::vector<YCSBOperation>& bench,
    int num_workers,
    int base_client_id,
    int num_groups) {
  std::vector<std::vector<YCSBOperation>> slices(num_workers);
  for (size_t i = 0; i < bench.size(); ++i) {
    slices[i % num_workers].push_back(bench[i]);
  }

  std::vector<WorkerResult> worker_results(num_workers);
  std::vector<std::thread> threads;
  threads.reserve(num_workers);

  auto start_time = std::chrono::steady_clock::now();

  for (int w = 0; w < num_workers; ++w) {
    threads.emplace_back([&worker_results, &cluster_cfg, &slices, w,
                         base_client_id, num_groups, num_workers]() {
      worker_results[w] = RunWorker(cluster_cfg, base_client_id, num_groups,
                                    slices[w], w, num_workers);
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  auto end_time = std::chrono::steady_clock::now();

  WorkerResult combined;
  combined.stats.reserve(bench.size());

  int total_put = 0, total_get = 0, total_fail = 0;
  for (const auto& r : worker_results) {
    total_put += r.put_count;
    total_get += r.get_count;
    total_fail += r.fail_count;
    combined.stats.insert(combined.stats.end(), r.stats.begin(), r.stats.end());
  }
  combined.put_count = total_put;
  combined.get_count = total_get;
  combined.fail_count = total_fail;

  auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      end_time - start_time).count();

  printf("\n");
  printf("[ConcurrentExecute] Total ops: %zu, Put: %d, Get: %d, Failed: %d\n",
         combined.stats.size(), total_put, total_get, total_fail);
  printf("[ConcurrentExecute] Duration: %lld ms\n", (long long)duration_ms);
  if (duration_ms > 0) {
    printf("[ConcurrentExecute] Throughput: %.2f ops/sec\n",
           combined.stats.size() * 1000.0 / duration_ms);
  }
  fflush(stdout);

  return combined;
}

// ---------------------------------------------------------------------------
//  预热阶段
// ---------------------------------------------------------------------------
static void Warmup(const kv::KvClusterConfig& cluster_cfg,
                   int num_groups,
                   int warmup_ops,
                   int value_size) {
  printf("\n========== WARMUP PHASE ==========\n");
  printf("[Warmup] Starting warmup with %d operations...\n", warmup_ops);

  auto client = new kv::KvServiceClient(cluster_cfg, 0);
  client->SetNumGroups(num_groups);

  int success = 0;
  for (int i = 0; i < warmup_ops; ++i) {
    int key_id = i;  // 顺序: 0, 1, 2, ..., warmup_ops-1
    auto key = raft::util::MakeKey(key_id, 64);
    auto value = raft::util::MakeValue(key_id, value_size);

    kv::OperationResults stat = client->RoutePut(key, value);
    if (stat.err == kv::kOk) {
      success++;
    }

    if ((i + 1) % (warmup_ops/10) == 0) {
      printf("\r[Warmup] Progress: %d / %d", i + 1, warmup_ops);
      fflush(stdout);
    }
  }

  delete client;
  printf("\n[Warmup] Completed: %d / %d successful\n", success, warmup_ops);
  printf("=================================\n\n");
  fflush(stdout);
}

// ---------------------------------------------------------------------------
//  结果打印
// ---------------------------------------------------------------------------
static void PrintResults(const WorkerResult& result,
                         const std::chrono::milliseconds& duration,
                         YCSBBenchType bench_type) {
  auto put_lat = AnalyzeLatency(result.stats, kPut);
  auto get_lat = AnalyzeLatency(result.stats, kGet);

  printf("\n");
  printf("========================================\n");
  printf("      YCSB MULTI-RAFT BENCH RESULTS\n");
  printf("========================================\n");
  printf("Workload Type:     %s\n", YCSBBenchToString(bench_type));
  printf("Total Ops:         %zu\n", result.stats.size());
  printf("Put Ops:           %d\n", result.put_count);
  printf("Get Ops:           %d\n", result.get_count);
  printf("Failed Ops:        %d\n", result.fail_count);
  printf("Duration:          %lld ms\n", (long long)duration.count());
  if (duration.count() > 0) {
    printf("Throughput:        %.2f ops/sec\n",
           result.stats.size() * 1000.0 / duration.count());
  }

  if (result.put_count > 0) {
    printf("\n[Put Latency]\n");
    printf("  Avg:     %.2f ms  |  P50: %.2f  P95: %.2f  P99: %.2f  Max: %.2f\n",
           (double)put_lat.op.avg / 1000.0,
           (double)put_lat.op.p50 / 1000.0,
           (double)put_lat.op.p95 / 1000.0,
           (double)put_lat.op.p99 / 1000.0,
           (double)put_lat.op.max / 1000.0);
    printf("  Commit:  %.2f ms (avg)\n", (double)put_lat.commit.avg / 1000.0);
    printf("  Apply:   %.2f ms (avg)  |  P50: %.2f  P95: %.2f  P99: %.2f  Max: %.2f\n",
           (double)put_lat.apply.avg / 1000.0,
           (double)put_lat.apply.p50 / 1000.0,
           (double)put_lat.apply.p95 / 1000.0,
           (double)put_lat.apply.p99 / 1000.0,
           (double)put_lat.apply.max / 1000.0);
  }

  if (result.get_count > 0) {
    printf("\n[Get Latency]\n");
    printf("  Avg:     %.2f ms  |  P50: %.2f  P95: %.2f  P99: %.2f  Max: %.2f\n",
           (double)get_lat.op.avg / 1000.0,
           (double)get_lat.op.p50 / 1000.0,
           (double)get_lat.op.p95 / 1000.0,
           (double)get_lat.op.p99 / 1000.0,
           (double)get_lat.op.max / 1000.0);
  }

  printf("========================================\n");
  fflush(stdout);
}

// ---------------------------------------------------------------------------
//  Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_conf.empty() || FLAGS_id < 0) {
    std::cerr << "Usage: " << argv[0] << "\n"
              << "  --conf <file>         Multi-Raft config file\n"
              << "  --id <client_id>      Base client ID\n"
              << "  --size <val_size>     Value size (e.g. 128, 4K)\n"
              << "  --op_count <N>        Number of operations\n"
              << "  --client_num <N>      Number of concurrent client threads\n"
              << "  --type <workload>     YCSB_WRITE | YCSB_READ | YCSB_MIXED\n"
              << "  --warmup_ops <N>      Warmup/pre-write ops (default: 50)\n"
              << "  --skip_warmup         Skip warmup for YCSB_READ (data must already exist)\n"
              << "  --verbose             Print per-operation details\n";
    return 1;
  }

  // Parse workload type
  YCSBBenchType bench_type = YCSB_WRITE;
  if (FLAGS_type == "YCSB_READ") {
    bench_type = YCSB_READ;
  } else if (FLAGS_type == "YCSB_MIXED") {
    bench_type = YCSB_MIXED;
  }

  // Parse configuration
  auto cluster_cfg = ParseConfigurationFile(FLAGS_conf);
  if (cluster_cfg.empty()) {
    std::cerr << "ERROR: empty cluster config from " << FLAGS_conf << "\n";
    return 1;
  }

  int client_id   = FLAGS_id;
  int val_size    = ParseCommandSize(FLAGS_size);
  int op_cnt      = FLAGS_op_count;
  int num_workers = FLAGS_client_num;
  int num_groups  = static_cast<int>(cluster_cfg.size());

  printf("========================================\n");
  printf("   YCSB Multi-Raft Client\n");
  printf("========================================\n");
  printf("Config:               %s\n", FLAGS_conf.c_str());
  printf("Client ID:            %d\n", client_id);
  printf("Value size:           %d bytes\n", val_size);
  printf("Operation count:      %d\n", op_cnt);
  printf("Concurrent clients:   %d\n", num_workers);
  printf("Workload type:        %s\n", YCSBBenchToString(bench_type));
  printf("Num groups:           %d\n", num_groups);
  printf("Warmup ops:           %d\n", FLAGS_warmup_ops);
  printf("Skip warmup:          %s\n", FLAGS_skip_warmup ? "true" : "false");
  printf("========================================\n\n");
  fflush(stdout);

  // Create client for warmup and to set num_groups
  auto client = new kv::KvServiceClient(cluster_cfg, client_id, num_workers);
  client->SetNumGroups(num_groups);

  // For YCSB_READ: warmup must write the EXACT same keys that the benchmark
  // will read. Strategy:
  //   1. Generate benchmark ops FIRST (know which keys are needed)
  //   2. Extract unique key_ids from benchmark ops
  //   3. Warmup writes those exact keys using MakeKey/MakeValue
  //   4. Benchmark reads those same keys (from the ops generated in step 1)
  // This guarantees 100% read hit rate.
  auto key_prefix = "ycsb-key-" + std::to_string(client_id);
  auto value_prefix = "ycsb-value-" + std::to_string(client_id) + "-";
  BenchConfiguration bench_cfg{key_prefix, value_prefix, op_cnt, val_size, bench_type};

  if (bench_type == YCSB_READ) {
    // Two modes:
    //   --skip_warmup=true : Data is assumed to already exist (from prior YCSB_WRITE run).
    //                        Skip pre-writing and the 120s settle wait, go straight to reading.
    //   --skip_warmup=false: (default) Pre-write keys first, then read (original behavior).
    if (FLAGS_skip_warmup) {
      printf("\n========== YCSB_READ (skip_warmup=true) ==========\n");
      printf("[Workload] Skipping warmup -- assuming data already exists in DB.\n");
      printf("[Workload] Generating %d read operations (key [0, warmup_ops))...\n", FLAGS_warmup_ops);
      fflush(stdout);

      // Read exactly warmup_ops keys, matching what YCSB_WRITE wrote
      BenchConfiguration read_cfg{key_prefix, value_prefix, FLAGS_warmup_ops, val_size, bench_type};
      std::vector<YCSBOperation> bench;
      BuildYCSBBench(read_cfg, &bench);
      printf("[Workload] Generated %zu operations\n", bench.size());
      fflush(stdout);

      auto bench_start = std::chrono::steady_clock::now();
      WorkerResult result = ConcurrentExecute(cluster_cfg, bench, num_workers, client_id, num_groups);
      auto bench_end = std::chrono::steady_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(bench_end - bench_start);

      PrintResults(result, duration, bench_type);

      auto wall_end = std::chrono::steady_clock::now();
      int64_t wall_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          wall_end - bench_start).count();

      std::ofstream ofs("ycsb_multiraft_results", std::ios::app);
      DumpAggregated(result, ofs, val_size, wall_elapsed_ms);
      printf("\n[Log] Aggregated results appended to ycsb_multiraft_results\n");
      fflush(stdout);

      delete client;
      return 0;
    }

    // Default: pre-write warmup_ops keys, benchmark reads exactly those keys.
    auto warmup_client = std::make_unique<kv::KvServiceClient>(cluster_cfg, client_id, num_workers);
    warmup_client->SetNumGroups(num_groups);

    printf("\n========== WARMUP PHASE ==========\n");
    printf("[Warmup] Pre-writing %d keys for READ benchmark...\n", FLAGS_warmup_ops);
    fflush(stdout);

    int warmup_ok = 0;
    int progress_step = std::max(1, FLAGS_warmup_ops / 5);
    for (int i = 0; i < FLAGS_warmup_ops; ++i) {
      int key_id = i;  // 顺序: 0, 1, 2, ..., warmup_ops-1
      auto key = raft::util::MakeKey(key_id, 64);
      auto value = raft::util::MakeValue(key_id, val_size);
      kv::OperationResults stat = warmup_client->RoutePut(key, value);
      if (stat.err == kv::kOk) warmup_ok++;
      if ((i + 1) % progress_step == 0) {
        printf("\r[Warmup] Progress: %d / %d", i + 1, FLAGS_warmup_ops);
        fflush(stdout);
      }
    }
    printf("\n[Warmup] Completed: %d / %d successful\n", warmup_ok, FLAGS_warmup_ops);
    printf("=================================\n\n");
    fflush(stdout);

    // Build benchmark: read exactly warmup_ops keys (key [0, warmup_ops))
    printf("[Workload] Generating operations...\n");
    BenchConfiguration read_cfg{key_prefix, value_prefix, FLAGS_warmup_ops, val_size, bench_type};
    std::vector<YCSBOperation> bench;
    BuildYCSBBench(read_cfg, &bench);
    printf("[Workload] Generated %zu operations\n", bench.size());
    fflush(stdout);

    // Wait for all warmup entries to be applied.
    printf("[Warmup] Waiting 120s for Raft replication to settle...\n");
    fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(FLAGS_warmup_ops/10));

    // Execute benchmark.
    auto bench_start = std::chrono::steady_clock::now();
    WorkerResult result = ConcurrentExecute(cluster_cfg, bench, num_workers, client_id, num_groups);
    auto bench_end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(bench_end - bench_start);

    PrintResults(result, duration, bench_type);

    // All workers have joined -- capture wall time BEFORE any disk I/O
    auto wall_end = std::chrono::steady_clock::now();
    int64_t wall_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        wall_end - bench_start).count();

    std::ofstream ofs("ycsb_multiraft_results", std::ios::app);
    DumpAggregated(result, ofs, val_size, wall_elapsed_ms);
    printf("\n[Log] Aggregated results appended to ycsb_multiraft_results\n");
    fflush(stdout);

    delete client;
    return 0;
  }
  // For：WRITE or MIXED
  if (FLAGS_warmup_ops > 0) {
    Warmup(cluster_cfg, num_groups, FLAGS_warmup_ops, val_size);
  }

  std::vector<YCSBOperation> bench;
  BuildYCSBBench(bench_cfg, &bench, FLAGS_warmup_ops);  // benchmark writes from key warmup_ops

  printf("[Workload] Generated %zu operations\n", bench.size());
  fflush(stdout);

  // Execute benchmark:WRITE or MIXED
  auto bench_start = std::chrono::steady_clock::now();
  WorkerResult result = ConcurrentExecute(
      cluster_cfg, bench, num_workers, client_id, num_groups);
  auto bench_end = std::chrono::steady_clock::now();

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      bench_end - bench_start);

  // Print results
  PrintResults(result, duration, bench_type);

  // All workers have joined -- capture wall time BEFORE any disk I/O
  auto wall_end = std::chrono::steady_clock::now();
  int64_t wall_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      wall_end - bench_start).count();

  std::ofstream ofs("ycsb_multiraft_results", std::ios::app);
  DumpAggregated(result, ofs, val_size, wall_elapsed_ms);

  printf("\n[Log] Aggregated results appended to ycsb_multiraft_results\n");
  fflush(stdout);

  delete client;
  return 0;
}
