// bench_server_multiraft.cc
//
// Multi-Raft Actor 架构服务器入口 + 客户端测试
//
// 7-Phase 启动流程：
//   Phase 1: 解析配置文件
//   Phase 2: 创建统一 Raft RPC 服务器（绑定 raft_addr）
//   Phase 3: 建立到其他所有物理节点的连接池
//   Phase 4: 本地构建均匀分组（每个节点创建 N 个大小为 N 的 Raft group，无 RPC 通信）
//   Phase 5: 根据分组信息在每个节点创建 Raft 实例
//   Phase 6: 统一启动选举
//   Phase 7: 创建 KV RPC 服务器（一个物理节点一个）
//
// 用法：
//   ./bench_server_multiraft --conf cluster.conf --id <global_node_id>
//
// 配置文件格式：
//   - 第一行：N（集群节点总数）
//   - 节点行：node_id  raft_addr  kv_addr  raft_log  kv_db
//
// 参数：
//   --test             : 启动后自动执行测试写入
//   --data_size        : 测试写入的数据大小（字节）
//   --num_writes       : 测试写入次数
//   --group_per_node   : 每个物理节点参与的分组数量（默认=N）

#include <gflags/gflags.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "encoding_mode.h"
#include "lrc_encoder.h"
#include "raft_store.h"
#include "raft_struct.h"
#include "rcf_rpc.h"
#include "util.h"

#include "RCF/RCF.hpp"

DEFINE_string(conf,           "",  "multi-raft config file path");
DEFINE_int32 (id,            -1,  "global node id (unique across all groups)");
DEFINE_int32 (peer_threads,   2,  "number of Poll threads for PeerFsm");
DEFINE_int32 (apply_threads,  2,  "number of Apply threads for ApplyFsm");
DEFINE_int32 (batch_size,    16,  "max messages per batch per FSM");

// 测试参数
DEFINE_bool  (test,          false, "run test write after startup");
DEFINE_int64 (data_size,     4096, "test write data size in bytes");
DEFINE_int32 (num_writes,    3,    "number of test writes");
DEFINE_int32 (write_interval_ms, 1000, "interval between test writes in ms");
DEFINE_int32 (concurrent_writes, 1, "number of concurrent writes");
DEFINE_int32 (write_group,   0,    "which group to write to (default: 0)");
DEFINE_int32 (group_per_node, 0,  "number of groups per physical node (default: cluster_size N)");
DEFINE_string(encoding, "RS_F",
              "Multi-raft encoding mode: RS_F (RS(F+1,F), commit=2F+1) | "
              "RS_3F (RS(F+1,3F+1), commit=ceil((3F+1)/2)) | "
              "LRC (LRC(F+1,2,2N-k-2), commit=ceil((3F+1)/2))");

// ---------------------------------------------------------------------------
//  Config file format
// ---------------------------------------------------------------------------
// 第一行（集群规模）：
//   N  (整数，表示集群中物理节点总数)
//
// 后续 N 行（节点配置）：
//   node_id  raft_addr  kv_addr  raft_log  kv_db
//
//   字段说明：
//   - node_id       : 物理节点 ID (0 ~ N-1)
//   - raft_addr     : Raft RPC 通信地址 (格式 "IP:Port")，用于节点间 Raft 协议消息
//   - kv_addr       : KV 服务 RPC 地址 (格式 "IP:Port")，用于客户端读写请求
//   - raft_log      : Raft 共识日志文件路径（持久化存储 Raft log entries）
//   - kv_db         : RocksDB 数据库路径，存储已提交的 KV 状态机数据
// ---------------------------------------------------------------------------

struct RawNodeLine {
  raft::raft_node_id_t local_node_id;  // 该行就是物理节点 id
  std::string          raft_addr;
  std::string          kv_addr;
  std::string          raft_log;
  std::string          kv_db;
};

struct ParsedConfig {
  int                               cluster_size = 0;    // N，集群节点总数
  multiraft::LrcParams              lrc{4, 1, 2};
  int                               rs_k = 0;
  int                               rs_r = 0;
  std::vector<RawNodeLine>          nodes;              // 所有物理节点（按 local_node_id 索引）
};

// 解析 key=value 风格的 token，返回整数值；找不到时返回 -1
static int ParseKeyInt(const std::string& tok, char eq) {
  auto pos = tok.find(eq);
  if (pos == std::string::npos) return -1;
  return std::stoi(tok.substr(pos + 1));
}

// ---------------------------------------------------------------------------
//  ParseConfig: 读取配置文件
// ---------------------------------------------------------------------------
static ParsedConfig ParseConfig(const std::string& path) {
  ParsedConfig cfg;
  std::ifstream fin(path);
  if (!fin.is_open())
    throw std::runtime_error("cannot open config file: " + path);

  std::string line;
  int line_no = 0;
  bool first_line_done = false;

  while (std::getline(fin, line)) {
    ++line_no;
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line);
    std::string first;
    ss >> first;

    if (!first_line_done) {
      cfg.cluster_size = std::stoi(first);
      if (cfg.cluster_size <= 0)
        throw std::runtime_error("first line must be positive integer N");
      first_line_done = true;
      continue;
    }

    RawNodeLine n;
    n.local_node_id = static_cast<raft::raft_node_id_t>(std::stoi(first));
    ss >> n.raft_addr >> n.kv_addr >> n.raft_log >> n.kv_db;
    cfg.nodes.push_back(n);
  }

  return cfg;
}

// ---------------------------------------------------------------------------
//  Helper: 打印分组信息
// ---------------------------------------------------------------------------
static void PrintGroupInfo(const multiraft::GroupMembership& m, int global_id) {
  printf("[CONFIG]   Group %u (local_id=%u): ", m.group_id, m.local_node_id);
  for (const auto& [node_id, cfg] : m.group_cluster_cfg) {
    printf("(%u,%u) ", node_id, cfg.raft_rpc_addr.port);
  }
  printf("\n");
}

// ---------------------------------------------------------------------------
//  Helper: 解析地址字符串 "IP:Port"
// ---------------------------------------------------------------------------
static bool ParseAddressStr(const std::string& addr, std::string& ip, int& port) {
  size_t pos = addr.find(':');
  if (pos == std::string::npos) return false;
  ip = addr.substr(0, pos);
  port = std::stoi(addr.substr(pos + 1));
  return true;
}

// ---------------------------------------------------------------------------
//  Signal handler
// ---------------------------------------------------------------------------
static multiraft::RaftStore* g_store = nullptr;
static std::atomic<int> g_sigusr1_count{0};
static std::atomic<int> g_sigusr2_flag{0};
static std::atomic<bool> g_shutdown_requested{false};

static void SigHandler(int sig) {
  if (sig == SIGINT || sig == SIGTERM) {
    g_shutdown_requested.store(true, std::memory_order_relaxed);
  }
}

// ---------------------------------------------------------------------------
//  TestWrite: 执行一次测试写入
// ---------------------------------------------------------------------------
static void TestWrite(multiraft::RaftStore* store, int write_id, size_t data_size,
                     multiraft::GroupId group_id, const std::string& prefix) {
  static std::atomic<int> success_count{0};
  static std::atomic<int> fail_count{0};
  static std::mutex print_mutex;

  std::string test_data;
  test_data.reserve(data_size);
  for (size_t i = 0; i < data_size; ++i) {
    test_data.push_back('A' + (i % 26));
  }

  store->LocalProposeToGroup(group_id, raft::Slice(test_data),
      [write_id, data_size, group_id, prefix, &success_count, &fail_count](bool ok, multiraft::EntryId eid) {
    std::lock_guard<std::mutex> lock(print_mutex);
    if (ok) {
      success_count++;
      fprintf(stderr, "\n[%s] Write %d (Group %u) SUCCEEDED! entry_id=%lu\n", prefix.c_str(), write_id, group_id, eid);
    } else {
      fail_count++;
      fprintf(stderr, "\n[%s] Write %d (Group %u) FAILED!\n", prefix.c_str(), write_id, group_id);
    }
    fprintf(stderr, "[%s] Total: success=%d failed=%d\n",
           prefix.c_str(), success_count.load(), fail_count.load());
  });
}

// ---------------------------------------------------------------------------
//  ConcurrentWriteTest: 执行并发写入测试
// ---------------------------------------------------------------------------
static void ConcurrentWriteTest(multiraft::RaftStore* store, int num_writes,
                               size_t data_size, int concurrent_writes,
                               multiraft::GroupId base_group_id, int total_groups) {
  printf("\n[CONCURRENT] Starting concurrent write test: %d writes, %d workers, %zu bytes\n",
         num_writes, concurrent_writes, data_size);

  std::atomic<int> write_counter{0};
  std::atomic<int> complete_counter{0};
  std::mutex complete_mutex;

  auto start_time = std::chrono::steady_clock::now();

  std::vector<std::thread> threads;
  for (int t = 0; t < concurrent_writes; ++t) {
    threads.emplace_back([store, num_writes, data_size, concurrent_writes, t,
                          base_group_id, total_groups, &write_counter, &complete_counter,
                          &complete_mutex, start_time]() {
      int writes_per_thread = num_writes / concurrent_writes;
      if (t < num_writes % concurrent_writes) writes_per_thread++;

      char prefix[32];
      snprintf(prefix, sizeof(prefix), "T%d", t);

      for (int i = 0; i < writes_per_thread; ++i) {
        int write_id = write_counter.fetch_add(1);
        if (write_id >= num_writes) break;

        multiraft::GroupId gid = (base_group_id + write_id) % total_groups;
        TestWrite(store, write_id, data_size, gid, prefix);

        if (concurrent_writes > 1) {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
      }

      {
        std::lock_guard<std::mutex> lock(complete_mutex);
        complete_counter++;
      }
    });
  }

  for (auto& t : threads) {
    if (t.joinable()) t.join();
  }

  auto end_time = std::chrono::steady_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

  printf("\n[CONCURRENT] Test complete: %d writes in %ld ms\n", num_writes, duration.count());
  if (duration.count() > 0) {
    printf("[CONCURRENT] Throughput: %.2f writes/sec\n", num_writes * 1000.0 / duration.count());
  }
}

// ---------------------------------------------------------------------------
//  PrintRoutingTable: 打印路由表
// ---------------------------------------------------------------------------
static void PrintRoutingTable(multiraft::RaftStore* store) {
  printf("\n========================================\n");
  printf("         ROUTING TABLE (%zu entries)\n",
         store->GetRoutingTableAll().size());
  printf("========================================\n");

  auto routes = store->GetRoutingTableAll();
  for (const auto& [stripe_id, entry] : routes) {
    printf("stripe_id=%lu -> entry_id=%lu\n", stripe_id, entry.entry_id);
    printf("  groups: [");
    for (size_t i = 0; i < entry.data_groups.size(); ++i) {
      printf("%u%s", entry.data_groups[i], i < entry.data_groups.size() - 1 ? ", " : "");
    }
    printf("]\n");
  }
  printf("========================================\n\n");
}

// ---------------------------------------------------------------------------
//  PrintRaftStatus: 打印各 Raft 实例的状态（包括 Leader）
// ---------------------------------------------------------------------------
static void PrintRaftStatus(multiraft::RaftStore* store) {
  printf("\n========================================\n");
  printf("         RAFT STATUS SUMMARY\n");
  printf("========================================\n");
  printf("Node ID: %d\n", store->GetNodeId());
  printf("Raft Instances: %d\n", store->GetRaftInstanceCount());
  printf("Routing Table Entries: %zu\n", store->GetRoutingTableAll().size());

  printf("\n----- Group Leader Status -----\n");
  printf("NOTE: Leader status may change during election\n");
  printf("      Run 'kill -USR2 <pid>' again after election completes\n\n");
  store->PrintAllGroupLeaderStatus();

  printf("========================================\n\n");
}

// ---------------------------------------------------------------------------
//  Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  // CRITICAL: Ignore SIGHUP FIRST, before any other signal setup
  // This prevents the server from dying when the terminal session changes
  ::signal(SIGHUP, SIG_IGN);

  struct sigaction sa {};
  sa.sa_handler = [](int sig) {
    const char s1[] = "\n[FATAL] Caught signal ";
    const char s2[] = ", exiting immediately.\n";
    write(STDERR_FILENO, s1, sizeof(s1) - 1);
    char buf[2] = {(char)('0' + sig % 10), 0};
    write(STDERR_FILENO, buf, 1);
    write(STDERR_FILENO, s2, sizeof(s2) - 1);
    _exit(1);  // async-signal-safe, never returns
  };
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESETHAND;  // restore default handler after one shot
  sigaction(SIGFPE,  &sa, nullptr);
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGABRT, &sa, nullptr);
  // SIGHUP is already ignored at the start of main() via signal(SIGHUP, SIG_IGN)

  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_conf.empty() || FLAGS_id < 0) {
    std::cerr << "Usage: " << argv[0]
              << " --conf <file> --id <global_node_id>\n"
              << "Config file format:\n"
              << "  Line 1: N (cluster size, integer)\n"
              << "  Lines 2..N+1: local_id  raft_addr  kv_addr  raft_log  kv_db\n"
              << "  LRC parameters are auto-calculated from cluster size\n\n"
              << "Test options:\n"
              << "  --test          : run test write after startup\n"
              << "  --data_size N   : test write data size (default: 4096)\n"
              << "  --num_writes N  : number of test writes (default: 3)\n"
              << "  --write_interval_ms N : interval between writes (default: 1000)\n";
    return 1;
  }

  printf("\n");
  printf("========================================\n");
  printf("   FlexRaft Multi-Raft Server\n");
  printf("   7-Phase Startup\n");
  printf("========================================\n");
  printf("Node ID: %d\n", FLAGS_id);
  printf("Config: %s\n", FLAGS_conf.c_str());
  printf("Test mode: %s\n", FLAGS_test ? "ON" : "OFF");
  if (FLAGS_test) {
    printf("  - Data size: %ld bytes\n", FLAGS_data_size);
    printf("  - Num writes: %d\n", FLAGS_num_writes);
    printf("  - Interval: %d ms\n", FLAGS_write_interval_ms);
  }
  printf("========================================\n\n");

  // Catch-all to prevent unhandled exceptions from crashing silently
  try {
  // =======================================================================
  // PHASE 1: 解析配置文件
  // =======================================================================
  printf("\n");
  printf("################################################################\n");
  printf("#  PHASE 1: Parsing Configuration File\n");
  printf("################################################################\n");

  ParsedConfig raw;
  try {
    raw = ParseConfig(FLAGS_conf);
  } catch (const std::exception& e) {
    std::cerr << "Config parse error: " << e.what() << "\n";
    return 1;
  }

  if (static_cast<int>(raw.nodes.size()) != raw.cluster_size) {
    std::cerr << "ERROR: node count (" << raw.nodes.size()
              << ") != declared cluster_size (" << raw.cluster_size << ")\n";
    return 1;
  }

  printf("[PHASE-1] Config parsed successfully\n");
  printf("[PHASE-1] Cluster size: %d nodes, %zu nodes in config\n", raw.cluster_size, raw.nodes.size());
  // for (const auto& n : raw.nodes) {
  //   printf("[PHASE-1]   Node %u: raft=%s kv=%s\n",
  //          n.local_node_id, n.raft_addr.c_str(), n.kv_addr.c_str());
  // }
  printf("################################################################\n\n");

  // =======================================================================
  // PHASE 2: 创建统一 Raft RPC 服务器（绑定 raft_addr）
  // =======================================================================
  printf("\n");
  printf("################################################################\n");
  printf("#  PHASE 2: Creating Unified Raft RPC Server\n");
  printf("################################################################\n");

  kv::rpc::NetAddress my_kv_addr{raw.nodes[FLAGS_id].kv_addr, 0};
  raft::rpc::NetAddress my_raft_addr{raw.nodes[FLAGS_id].raft_addr, 0};

  {
    std::string ip;
    int port;
    if (ParseAddressStr(raw.nodes[FLAGS_id].raft_addr, ip, port)) {
      my_raft_addr.ip = ip;
      my_raft_addr.port = static_cast<uint16_t>(port);
    }
    if (ParseAddressStr(raw.nodes[FLAGS_id].kv_addr, ip, port)) {
      my_kv_addr.ip = ip;
      my_kv_addr.port = static_cast<uint16_t>(port);
    }
  }

  printf("[PHASE-2] My raft address: %s:%d\n",
         my_raft_addr.ip.c_str(), my_raft_addr.port);
  printf("[PHASE-2] My KV address: %s:%d\n",
         my_kv_addr.ip.c_str(), my_kv_addr.port);
  printf("[PHASE-2] Unified Raft RPC server will be started in Phase 4.5\n");
  printf("################################################################\n\n");

  // =======================================================================
  // PHASE 3: 建立到其他所有物理节点的连接池
  // =======================================================================
  printf("\n");
  printf("################################################################\n");
  printf("#  PHASE 3: Establishing Connection Pool to All Peers\n");
  printf("################################################################\n");

  std::vector<kv::rpc::NetAddress> peer_raft_addrs;
  std::vector<kv::rpc::NetAddress> peer_kv_addrs;
  peer_raft_addrs.reserve(raw.cluster_size);
  peer_kv_addrs.reserve(raw.cluster_size);

  for (const auto& n : raw.nodes) {
    std::string ip;
    int port;
    kv::rpc::NetAddress r_addr, k_addr;

    if (ParseAddressStr(n.raft_addr, ip, port)) {
      r_addr = kv::rpc::NetAddress{ip, static_cast<uint16_t>(port)};
    }
    if (ParseAddressStr(n.kv_addr, ip, port)) {
      k_addr = kv::rpc::NetAddress{ip, static_cast<uint16_t>(port)};
    }

    peer_raft_addrs.push_back(r_addr);
    peer_kv_addrs.push_back(k_addr);
    // printf("[PHASE-3]   Node %u: raft=%s:%d kv=%s:%d\n",
    //        n.local_node_id,
    //        r_addr.ip.c_str(), r_addr.port,
    //        k_addr.ip.c_str(), k_addr.port);
  }
  printf("[PHASE-3] Connection pool established for %zu peers\n", peer_raft_addrs.size());
  printf("################################################################\n\n");

  // =======================================================================
  // PHASE 4: 本地构建均匀分组（每个节点创建 N 个大小为 N 的 Raft group）
  // 无需 RPC 通信，所有节点独立计算出完全相同的 N 个 membership
  // =======================================================================
  printf("\n");
  printf("################################################################\n");
  printf("#  PHASE 4: Building Uniform Group Memberships\n");
  printf("################################################################\n");

  printf("[PHASE-4] Node %d: building %d uniform groups (size %d each)\n",
         FLAGS_id, raw.cluster_size, raw.cluster_size);

  // Build N uniform memberships locally: each group contains all N nodes.
  // group_id = 0..N-1, matching node IDs. No RPC needed.
  std::vector<multiraft::GroupMembership> memberships;
  for (int g = 0; g < raw.cluster_size; ++g) {
    kv::KvClusterConfig group_cfg;
    for (const auto& node : raw.nodes) {
      std::string ip;
      int port;
      kv::KvServiceNodeConfig nc;
      nc.id = static_cast<raft::raft_node_id_t>(node.local_node_id);
      nc.group_id = static_cast<multiraft::GroupId>(g);

      if (ParseAddressStr(node.raft_addr, ip, port)) {
        nc.raft_rpc_addr = raft::rpc::NetAddress{ip, static_cast<uint16_t>(port)};
      }
      if (ParseAddressStr(node.kv_addr, ip, port)) {
        nc.kv_rpc_addr = kv::rpc::NetAddress{ip, static_cast<uint16_t>(port)};
      }
      nc.raft_log_filename = node.raft_log + ".g" + std::to_string(g);
      nc.kv_dbname = node.kv_db + ".g" + std::to_string(g);
      group_cfg[nc.id] = nc;
    }

    multiraft::GroupMembership m;
    m.group_id = static_cast<multiraft::GroupId>(g);
    m.local_node_id = static_cast<raft::raft_node_id_t>(FLAGS_id);
    m.group_cluster_cfg = group_cfg;
    memberships.push_back(m);
  }

  printf("[PHASE-4] Node %d has %zu uniform groups\n", FLAGS_id, memberships.size());
  // for (size_t i = 0; i < memberships.size(); ++i) {
  //   const auto& g = memberships[i];
  //   printf("[PHASE-4]   [%zu] Group %u members: {", i, g.group_id);
  //   for (const auto& [nid, _] : g.group_cluster_cfg) {
  //     printf("%u ", nid);
  //   }
  //   printf("}\n");
  // }
  printf("[PHASE-4] Uniform group building complete (no RPC required)\n");
  printf("################################################################\n\n");

  // =======================================================================
  // PHASE 5: 创建 Raft 实例
  // memberships 已在 Phase 4 中本地计算完毕，无需等待 RPC 同步
  // =======================================================================
  printf("\n");
  printf("################################################################\n");
  printf("#  PHASE 5: Creating Raft Instances\n");
  printf("################################################################\n");

  // 创建 RaftStore
  multiraft::RaftStore store(
      FLAGS_id,
      my_raft_addr,
      my_kv_addr,
      raw.cluster_size,  // k = N (full cluster)
      0,                  // r = 0 (no global parity in uniform groups)
      FLAGS_peer_threads,
      FLAGS_apply_threads,
      FLAGS_batch_size);

  g_store = &store;

  // 设置信号处理
  std::signal(SIGINT, SigHandler);
  std::signal(SIGTERM, SigHandler);

  // 启动 Raft RPC 服务器和 batch transport
  store.GetUnifiedRpcServer()->Start();
  store.GetBatchTransportManager()->Start();
  printf("[PHASE-5] Raft RPC server started\n");

  // 设置 peer 地址
  store.SetPeerAddrs(peer_raft_addrs, peer_kv_addrs, raw.cluster_size);
  store.SetPhysicalClusterSize(raw.cluster_size);

  // 初始化延迟感知 LRC（在 CreateRaftInstances 之前）
  // 内部依次调用 InitLatencyMatrix + BuildLrcGroups
  store.InitLatencyAware(raw.cluster_size);

  // Parse and apply encoding mode (must be before CreateRaftInstances so
  // every RaftNode/RaftState picks up the corresponding commit threshold).
  {
    multiraft::EncodingMode em;
    if (!multiraft::ParseEncodingMode(FLAGS_encoding, &em)) {
      fprintf(stderr,
              "[FATAL] invalid --encoding=%s (expected RS_F | RS_3F | LRC)\n",
              FLAGS_encoding.c_str());
      return 1;
    }
    store.SetEncodingMode(em);
    printf("[PHASE-5] encoding mode: %s\n", multiraft::EncodingModeName(em));
  }

  printf("[PHASE-5] Total memberships to create: %zu\n", memberships.size());
  // printf("[PHASE-5] ===== ALL MEMBERSHIPS =====\n");
  // for (size_t mi = 0; mi < memberships.size(); ++mi) {
  //   const auto& m = memberships[mi];
  //   printf("[PHASE-5]   [%zu] group=%u local_id=%u cluster_cfg={", mi, m.group_id, m.local_node_id);
  //   for (const auto& [k, v] : m.group_cluster_cfg) {
  //     printf("%u ", k);
  //   }
  //   printf("}\n");
  // }
  // printf("[PHASE-5] =============================\n");

  // 创建数据库目录
  std::set<std::string> db_paths;
  for (const auto& m : memberships) {
    auto local_nid = static_cast<raft::raft_node_id_t>(FLAGS_id);
    if (m.group_cluster_cfg.count(local_nid) > 0) {
      db_paths.insert(m.group_cluster_cfg.at(local_nid).kv_dbname);
    }
  }
  for (const auto& p : db_paths) {
    std::filesystem::create_directories(p);
  }
  printf("[PHASE-5] Ensured %zu database directories exist\n", db_paths.size());

  // 批量创建所有 Raft 实例
  store.CreateRaftInstances(memberships);

  printf("[PHASE-5] Raft instances created: %d\n", store.GetRaftInstanceCount());
  printf("################################################################\n\n");

  // =======================================================================
  // PHASE 6: 统一启动选举
  // =======================================================================
  printf("\n");
  printf("################################################################\n");
  printf("#  PHASE 6: Unified Election Start\n");
  printf("################################################################\n");

  store.InitRaftInstances();

  printf("\n");
  printf("################################################################\n");
  printf("#  ELECTION START\n");
  printf("################################################################\n");

  store.StartRaftInstances();

  // 在 RaftState 创建完成后，分发 lrc_grouper_
  store.DistributeLrcGrouperToRaftNodes();

  printf("\n");
  printf("################################################################\n");
  printf("#  PHASE 6 Complete: Elections started\n");
  printf("#  Raft Instances: %d\n", store.GetRaftInstanceCount());
  printf("#  Batch threads: %d peer + %d apply\n", FLAGS_peer_threads, FLAGS_apply_threads);
  printf("################################################################\n");
  fflush(stdout);

  // =======================================================================
  // PHASE 7: 创建 KV RPC 服务器（一个物理节点一个）
  // =======================================================================
  printf("\n");
  printf("################################################################\n");
  printf("#  PHASE 7: Starting KV RPC Server\n");
  printf("################################################################\n");
  fflush(stdout);

  store.StartKVRpcServer();

  printf("[PHASE-7] KV RPC server started at %s:%d\n",
         my_kv_addr.ip.c_str(), my_kv_addr.port);
  printf("################################################################\n");
  fflush(stdout);

  // =======================================================================
  // 打印启动摘要
  // =======================================================================
  printf("\n");
  printf("################################################################\n");
  printf("#                    SERVER INFO SUMMARY                       #\n");
  printf("################################################################\n");
  printf("# Node ID:              %d\n", FLAGS_id);
  printf("# Total Raft Groups:   %d\n", store.GetRaftInstanceCount());
  printf("# Groups Per Node:      %zu\n", memberships.size());
  printf("# Peer Threads:         %d\n", FLAGS_peer_threads);
  printf("# Apply Threads:       %d\n", FLAGS_apply_threads);
  printf("# Batch Size:          %d\n", FLAGS_batch_size);
  printf("# Cluster Size:        %d (uniform groups, each of size N)\n", raw.cluster_size);
  printf("# Test Mode:           %s\n", FLAGS_test ? "ENABLED" : "DISABLED");
  if (FLAGS_test) {
    printf("# Test Data Size:      %ld bytes\n", (long)FLAGS_data_size);
    printf("# Test Write Count:    %d\n", FLAGS_num_writes);
    printf("# Concurrent Writes:   %d\n", FLAGS_concurrent_writes);
    printf("# Write Group:         %d\n", FLAGS_write_group);
  }
  printf("################################################################\n\n");

  // =======================================================================
  // 执行测试写入
  // =======================================================================
  if (FLAGS_test) {
    printf("\n");
    printf("################################################################\n");
    printf("#                    TEST PHASE                               #\n");
    printf("################################################################\n");
    printf("[TEST] Waiting 3 seconds for Raft leader election...\n");
    std::this_thread::sleep_for(std::chrono::seconds(3));

    int total_groups = store.GetRaftInstanceCount();
    multiraft::GroupId test_group_id = static_cast<multiraft::GroupId>(FLAGS_write_group);

    // 边界检查
    if (test_group_id >= static_cast<multiraft::GroupId>(total_groups)) {
      printf("[TEST] WARNING: write_group=%d >= total_groups=%d, using group 0\n",
             FLAGS_write_group, total_groups);
      test_group_id = 0;
    }

    if (FLAGS_concurrent_writes > 1) {
      ConcurrentWriteTest(&store, FLAGS_num_writes, FLAGS_data_size,
                         FLAGS_concurrent_writes, test_group_id, total_groups);
    } else {
      for (int i = 0; i < FLAGS_num_writes; ++i) {
        TestWrite(&store, i + 1, FLAGS_data_size, test_group_id, "MAIN");

        if (i < FLAGS_num_writes - 1) {
          printf("[TEST] Waiting %d ms before next write...\n", FLAGS_write_interval_ms);
          std::this_thread::sleep_for(std::chrono::milliseconds(FLAGS_write_interval_ms));
        }
      }
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));
    PrintRoutingTable(&store);
    PrintRaftStatus(&store);

    printf("\n");
    printf("################################################################\n");
    printf("#                    TEST PHASE COMPLETE                       #\n");
    printf("################################################################\n");
  }

  // =======================================================================
  // 主循环
  // =======================================================================
  printf("\n");
  printf("################################################################\n");
  printf("#                    SERVER RUNNING                            #\n");
  printf("################################################################\n");
  printf("# PID: %d\n", getpid());
  printf("#\n");
  printf("# Commands:\n");
  printf("#   kill -USR1 %d  : Trigger a test write\n", getpid());
  printf("#   kill -USR2 %d  : Print routing table + Leader status\n", getpid());
  printf("#   kill -INT  %d  : Graceful shutdown\n", getpid());
  printf("#\n");
  printf("# Press Ctrl+C to stop.\n");
  printf("################################################################\n\n");

  auto old_sigusr1 = std::signal(SIGUSR1, [](int) {
    g_sigusr1_count.fetch_add(1, std::memory_order_relaxed);
  });

  auto old_sigusr2 = std::signal(SIGUSR2, [](int) {
    g_sigusr2_flag.store(1, std::memory_order_relaxed);
  });

  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(10));

    // 每 10 秒自动打印 Leader 状态
    PrintRaftStatus(&store);

    if (g_sigusr2_flag.exchange(0, std::memory_order_relaxed)) {
      PrintRoutingTable(&store);
      PrintRaftStatus(&store);
    }

    int sig1_count = g_sigusr1_count.exchange(0, std::memory_order_relaxed);
    for (int i = 0; i < sig1_count; ++i) {
      static int sig1_writes = 0;
      sig1_writes++;
      TestWrite(&store, sig1_writes, FLAGS_data_size,
                static_cast<multiraft::GroupId>(FLAGS_write_group), "SIGUSR1");
    }

    // printf("[MAIN] ========================================\n");
    // printf("[MAIN] Heartbeat: server still running...\n");
    // printf("[MAIN]   Node ID: %d\n", store.GetNodeId());
    // printf("[MAIN]   Raft Instances: %d\n", store.GetRaftInstanceCount());
    // printf("[MAIN]   Routing Table Entries: %zu\n",
    //        store.GetRoutingTableAll().size());
    // printf("[MAIN] ========================================\n\n");

    if (g_shutdown_requested.load(std::memory_order_relaxed)) {
      printf("\n[MAIN] Shutdown signal received, stopping server...\n");
      break;
    }
  }

  printf("\n[MAIN] Initiating graceful shutdown...\n");
  store.Stop();
  printf("[MAIN] Server stopped cleanly.\n");
  } catch (const std::exception& e) {
    fprintf(stderr, "\n[MAIN] UNHANDLED EXCEPTION: %s\n", e.what());
    if (g_store) { try { g_store->Stop(); } catch (...) {} }
    return 1;
  } catch (...) {
    fprintf(stderr, "\n[MAIN] UNHANDLED UNKNOWN EXCEPTION\n");
    if (g_store) { try { g_store->Stop(); } catch (...) {} }
    return 1;
  }
  return 0;
}
