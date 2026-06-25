#pragma once
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "RCF/Future.hpp"
#include "config.h"
#include "kv_node.h"
#include "raft_type.h"
#include "rpc.h"
#include "type.h"
namespace kv {

struct OperationResults {
  ErrorType err = kOk;
  uint64_t apply_elapse_time = 0;
  uint64_t commit_elapse_time = 0;
};
class KvServiceClient {
  // If a KV Request is not done within 10 seconds
  static const int kKVRequestTimesoutCnt = 10;

 public:
  KvServiceClient(const KvClusterConfig &config, uint32_t client_id, int pool_size = 2);
  ~KvServiceClient();

// struct GatherValueTask {
//   std::string key;
//   raft::raft_index_t read_index;
//   raft::raft_node_id_t replied_id;
//   raft::Encoder::EncodingResults *decode_input;
//   int k, m;
//   raft::raft_group_id_t group_id = static_cast<raft::raft_group_id_t>(-1);
// };
//
// struct GatherValueTaskResults {
//   std::string *value;
//   ErrorType err;
// };

 public:
  //废弃单Raft API，使用Multi-Raft路由API替代
  // OperationResults Put(const std::string &key, const std::string &value);
  // OperationResults Get(const std::string &, std::string *value);
  // OperationResults Delete(const std::string &key);
  // OperationResults Abort();

  // void DoGatherValueTask(const GatherValueTask *task, GatherValueTaskResults *res);

  // static void OnGetValueRpcComplete(RCF::Future<GetValueResponse> ret, KvServiceClient *client);

  raft::raft_node_id_t LeaderId() const { return curr_leader_; }

  rpc::KvServerRPCClient *GetRPCStub(raft::raft_node_id_t id) { return servers_[id]; }

  struct DecodedString {
    int k, m;
    raft::raft_frag_id_t frag_id;
    raft::Slice frag;

    std::string ToString() const {
      char buf[256];
      sprintf(buf, "DecodedString{k=%d, m=%d, frag_id=%d}", k, m, frag_id);
      return std::string(buf);
    }
  };

  static DecodedString DecodeString(std::string *str) {
    auto bytes = str->c_str();

    int k = *reinterpret_cast<const int *>(bytes);
    bytes += sizeof(int);

    int m = *reinterpret_cast<const int *>(bytes);
    bytes += sizeof(int);

    auto frag_id = *reinterpret_cast<const raft::raft_frag_id_t *>(bytes);
    bytes += sizeof(raft::raft_frag_id_t);

    auto remaining_size = str->size() - sizeof(int) * 2 - sizeof(raft::raft_frag_id_t);
    return DecodedString{k, m, frag_id, raft::Slice(const_cast<char *>(bytes), remaining_size)};
  }

  uint32_t ClientId() const { return client_id_; }

 private:
  //废弃单Raft API相关方法
  // raft::raft_node_id_t DetectCurrentLeader();
  // Response WaitUntilRequestDone(const Request &request);
  void sleepMs(int cnt) { std::this_thread::sleep_for(std::chrono::milliseconds(cnt)); }

 private:
  std::unordered_map<raft::raft_node_id_t, rpc::KvServerRPCClient *> servers_;
  raft::raft_node_id_t curr_leader_;
  // Use kv::kNoDetectLeader from type.h (sentinel value for "no leader detected")
  static constexpr raft::raft_node_id_t kNoDetectLeader = kv::kNoDetectLeader;

  raft::raft_term_t curr_leader_term_;

  uint32_t client_id_;

  // =======================================================================
  //  Multi-Raft 客户端路由（确定性哈希）
  // =======================================================================
 public:
  // 设置 group 总数（外部调用或构造函数自动推导）
  void SetNumGroups(int n) { num_groups_ = n; }

  // 确定性哈希：key → group_id（纯函数，跨进程一致）
  static raft::raft_node_id_t GetGroupForKey(const std::string& key, int num_groups);

  // 按路由写入（自动选择 group → 发 RPC）
  OperationResults RoutePut(const std::string& key, const std::string& value);

  // 按路由读取（计算 group → 找 leader → 发 Get RPC）
  OperationResults RouteGet(const std::string& key, std::string* value);

 private:
  // 探测指定 group 的 leader
  raft::raft_node_id_t DetectLeaderForGroup(raft::raft_node_id_t group_id);

  // Probe a specific node to verify if it's the leader for the given group
  bool ProbeLeader(raft::raft_node_id_t node_id, raft::raft_group_id_t group_id);

  // Thread-safe cache update methods (use unique_lock internally)
  void UpdateLeaderCache(raft::raft_node_id_t group_id, raft::raft_node_id_t leader_id);
  void InvalidateLeaderCache(raft::raft_node_id_t group_id);

  // Get peers for a group (假设环境中已存在该方法实现)
  std::vector<raft::raft_node_id_t> GetPeersForGroup(raft::raft_node_id_t group_id);

  // group_id → leader_node_id 缓存（per-instance，避免频繁 RPC）
  mutable std::shared_mutex routing_mutex_;
  std::unordered_map<raft::raft_node_id_t, raft::raft_node_id_t> group_to_leader_;
  // 总 group 数（哈希的模数）
  int num_groups_ = 0;
  int pool_size_ = 2;
};
}  // namespace kv
