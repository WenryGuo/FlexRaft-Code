// ycsb_server_multiraft.cc
//
// Multi-Raft YCSB 服务器 —— 复用 bench_server_multiraft.cc 的 7-Phase 启动流程
//
// 用法：
//   ./ycsb_server_multiraft --conf cluster.conf --id <global_node_id>
//
// 7-Phase 启动流程：
//   Phase 1: 解析配置文件
//   Phase 2: 创建统一 Raft RPC 服务器（绑定 raft_addr）
//   Phase 3: 建立到其他所有物理节点的连接池
//   Phase 4: 本地构建均匀分组（每个节点创建 N 个大小为 N 的 Raft group）
//   Phase 5: 在每个节点创建 Raft 实例
//   Phase 6: 统一启动选举
//   Phase 7: 创建 KV RPC 服务器（一个物理节点一个）
//
// 配置文件格式：
//   - 第一行：N（集群节点总数）
//   - 节点行：node_id  raft_addr  kv_addr  raft_log  kv_db

#include <gflags/gflags.h>

#include <cassert>
#include <chrono>
#include <csignal>
#include <ctime>
#include <unistd.h>
#include <execinfo.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "encoding_mode.h"
#include "raft_store.h"
#include "raft_struct.h"
#include "rcf_rpc.h"
#include "util.h"

#include "RCF/RCF.hpp"

DEFINE_string(conf,           "",  "multi-raft config file path");
DEFINE_int32 (id,            -1,  "global node id (unique across all groups)");
DEFINE_int32 (peer_threads,   2,  "number of Poll threads for PeerFsm");
DEFINE_int32 (apply_threads,  2,  "number of Apply threads for ApplyFsm");
DEFINE_int32 (batch_size,    64,  "max messages per batch per FSM");
DEFINE_string(encoding, "LRC",
              "Multi-raft encoding mode: RS_F | RS_3F | LRC");

// ---------------------------------------------------------------------------
//  Config file format
// ---------------------------------------------------------------------------
// 第一行（集群规模）：
//   N  (整数，表示集群中物理节点总数)
//
// 后续 N 行（节点配置）：
//   node_id  raft_addr  kv_addr  raft_log  kv_db
// ---------------------------------------------------------------------------

struct RawNodeLine {
  raft::raft_node_id_t local_node_id;
  std::string          raft_addr;
  std::string          kv_addr;
  std::string          raft_log;
  std::string          kv_db;
};

struct ParsedConfig {
  int                               cluster_size = 0;
  multiraft::LrcParams              lrc{4, 1, 2};
  int                               rs_k = 0;
  int                               rs_r = 0;
  std::vector<RawNodeLine>          nodes;
};

static int ParseKeyInt(const std::string& tok, char eq) {
  auto pos = tok.find(eq);
  if (pos == std::string::npos) return -1;
  return std::stoi(tok.substr(pos + 1));
}

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

static bool ParseAddressStr(const std::string& addr, std::string& ip, int& port) {
  size_t pos = addr.find(':');
  if (pos == std::string::npos) return false;
  ip = addr.substr(0, pos);
  port = std::stoi(addr.substr(pos + 1));
  return true;
}

static multiraft::RaftStore* g_store = nullptr;
static std::atomic<bool> g_shutdown_requested{false};

static void SigHandler(int sig) {
  if (sig == SIGINT || sig == SIGTERM) {
    g_shutdown_requested.store(true, std::memory_order_relaxed);
  }
}

int main(int argc, char* argv[]) {
  // CRITICAL: Ignore SIGHUP FIRST, before any other signal setup
  // This prevents the server from dying when the terminal session changes
  ::signal(SIGHUP, SIG_IGN);

  struct sigaction sa {};
  sa.sa_handler = [](int sig) {
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "\n[FATAL] Signal=%d (%s) PID=%d\n",
        sig, strsignal(sig), (int)getpid());
    write(STDERR_FILENO, buf, len);

    void* buffer[64];
    int n = backtrace(buffer, 64);
    backtrace_symbols_fd(buffer, n, STDERR_FILENO);
    fflush(stderr);
    _exit(1);
  };
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESETHAND;
  sigaction(SIGFPE,  &sa, nullptr);
  sigaction(SIGSEGV, &sa, nullptr);
  // NOTE: Do NOT catch SIGABRT here - it masks the real SIGSEGV crash site
  // when backtrace() itself triggers a secondary crash during stack unwinding.
  // SIGHUP is already ignored at the start of main() via signal(SIGHUP, SIG_IGN)

  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_conf.empty() || FLAGS_id < 0) {
    std::cerr << "Usage: " << argv[0]
              << " --conf <file> --id <global_node_id>\n"
              << "Config file format:\n"
              << "  Line 1: N (cluster size, integer)\n"
              << "  Lines 2..N+1: local_id  raft_addr  kv_addr  raft_log  kv_db\n"
              << "\nServer options:\n"
              << "  --peer_threads N   : number of Poll threads (default: 2)\n"
              << "  --apply_threads N : number of Apply threads (default: 2)\n"
              << "  --batch_size N    : max messages per batch (default: 64)\n"
              << "  --encoding MODE    : RS_F | RS_3F | LRC (default: LRC)\n";
    return 1;
  }

  printf("\n");
  printf("========================================\n");
  printf("   YCSB Multi-Raft Server\n");
  printf("   7-Phase Startup\n");
  printf("========================================\n");
  printf("Node ID: %d\n", FLAGS_id);
  printf("Config: %s\n", FLAGS_conf.c_str());
  printf("========================================\n\n");

  try {
  // =======================================================================
  // PHASE 1: 解析配置文件
  // =======================================================================
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
  printf("[PHASE-1] Cluster size: %d nodes\n", raw.cluster_size);
  printf("################################################################\n\n");

  // =======================================================================
  // PHASE 2: 创建统一 Raft RPC 服务器
  // =======================================================================
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
  printf("################################################################\n\n");

  // =======================================================================
  // PHASE 3: 建立到其他所有物理节点的连接池
  // =======================================================================
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
  }
  printf("[PHASE-3] Connection pool established for %zu peers\n", peer_raft_addrs.size());
  printf("################################################################\n\n");

  // =======================================================================
  // PHASE 4: 本地构建均匀分组
  // =======================================================================
  printf("################################################################\n");
  printf("#  PHASE 4: Building Uniform Group Memberships\n");
  printf("################################################################\n");

  printf("[PHASE-4] Node %d: building %d uniform groups (size %d each)\n",
         FLAGS_id, raw.cluster_size, raw.cluster_size);

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
  printf("################################################################\n\n");

  // =======================================================================
  // PHASE 5: 创建 Raft 实例
  // =======================================================================
  printf("################################################################\n");
  printf("#  PHASE 5: Creating Raft Instances\n");
  printf("################################################################\n");

  multiraft::RaftStore store(
      FLAGS_id,
      my_raft_addr,
      my_kv_addr,
      raw.cluster_size,
      0,
      FLAGS_peer_threads,
      FLAGS_apply_threads,
      FLAGS_batch_size);

  g_store = &store;

  std::signal(SIGINT, SigHandler);
  std::signal(SIGTERM, SigHandler);

  store.GetUnifiedRpcServer()->Start();
  store.GetBatchTransportManager()->Start();
  printf("[PHASE-5] Raft RPC server started\n");

  store.SetPeerAddrs(peer_raft_addrs, peer_kv_addrs, raw.cluster_size);
  store.SetPhysicalClusterSize(raw.cluster_size);

  // 初始化延迟感知 以及进行LRC分组（在 CreateRaftInstances 之前）
  store.InitLatencyAware(raw.cluster_size);

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

  store.CreateRaftInstances(memberships);
  printf("[PHASE-5] Raft instances created: %d\n", store.GetRaftInstanceCount());
  printf("################################################################\n\n");

  // =======================================================================
  // PHASE 6: 统一启动选举
  // =======================================================================
  printf("################################################################\n");
  printf("#  PHASE 6: Unified Election Start\n");
  printf("################################################################\n");

  store.InitRaftInstances();
  store.StartRaftInstances();

  // 在 RaftState 创建完成后，分发 lrc_grouper_
  // （DistributeLrcGrouperToRaftNodes 在 CreateRaftInstances 末尾被调用时，raft_state_ 尚不存在）
  store.DistributeLrcGrouperToRaftNodes();

  printf("[PHASE-6] Elections started\n");
  printf("################################################################\n\n");

  // =======================================================================
  // PHASE 7: 创建 KV RPC 服务器
  // =======================================================================
  printf("################################################################\n");
  printf("#  PHASE 7: Starting KV RPC Server\n");
  printf("################################################################\n");

  store.StartKVRpcServer();

  printf("[PHASE-7] KV RPC server started at %s:%d\n",
         my_kv_addr.ip.c_str(), my_kv_addr.port);
  printf("################################################################\n\n");

  // =======================================================================
  // 打印启动摘要
  // =======================================================================
  printf("\n");
  printf("========================================\n");
  printf("         SERVER INFO SUMMARY\n");
  printf("========================================\n");
  printf("# Node ID:              %d\n", FLAGS_id);
  printf("# Total Raft Groups:   %d\n", store.GetRaftInstanceCount());
  printf("# Groups Per Node:      %zu\n", memberships.size());
  printf("# Peer Threads:         %d\n", FLAGS_peer_threads);
  printf("# Apply Threads:        %d\n", FLAGS_apply_threads);
  printf("# Batch Size:          %d\n", FLAGS_batch_size);
  printf("# Cluster Size:        %d\n", raw.cluster_size);
  printf("# Encoding Mode:       %s\n", FLAGS_encoding.c_str());
  printf("========================================\n\n");

  // =======================================================================
  // 主循环
  // =======================================================================
  printf("========================================\n");
  printf("   SERVER RUNNING - READY FOR YCSB BENCHMARK\n");
  printf("========================================\n");
  printf("# PID: %d\n", getpid());
  printf("#\n");
  printf("# Commands:\n");
  printf("#   kill -USR2 %d  : Print Leader status\n", getpid());
  printf("#   kill -INT  %d  : Graceful shutdown\n", getpid());
  printf("#\n");
  printf("# Waiting for clients...\n");
  printf("========================================\n\n");

  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(10));

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
