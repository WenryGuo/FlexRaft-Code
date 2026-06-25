#pragma once
// group_coordinator.h — Multi-Raft 分组协调器
//
// 职责：
//   1. 管理 RPC 客户端连接池
//   2. 提供 RPC 发送能力
//
// 注意：分组功能已迁移到 RaftStore::BuildLrcGroups 和 LrcComplementaryGrouper

#include <functional>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <memory>

#include "type.h"              // kv::rpc::NetAddress
#include "rpc.h"               // raft::rpc::NetAddress
#include "rcf_rpc.h"           // raft::rpc::RCFRpcClient
#include "raft_struct.h"       // raft::GroupInfo, raft::GroupNotificationArgs

namespace multiraft {

class GroupCoordinator {
 public:
  using RpcClientPtr = std::shared_ptr<raft::rpc::RCFRpcClient>;

  GroupCoordinator(
      int node_id,
      int cluster_size,
      const std::vector<kv::rpc::NetAddress>& peer_addrs,
      std::function<void(const raft::GroupNotificationArgs&)> on_group_notification);

  ~GroupCoordinator();

  // 执行分组算法并发送 RPC 通知（已废弃，使用 LrcComplementaryGrouper）
  void ExecuteGroupingAndNotify(int groups_per_node, int l, int k, int r);

  // 等待分组创建完成
  void WaitForGroupsReady(int expected_groups);

  // 获取本地节点 ID
  int GetNodeId() const { return node_id_; }

  // 获取本地已创建的分组数
  int GetLocalGroupCount() const;

  // 获取 RPC 客户端
  RpcClientPtr GetRpcClient(int target_node_id);

  // 停止协调器
  void Stop();

 private:
  // 互斥锁保护
  mutable std::mutex mu_;
  int local_group_count_ = 0;
  int notified_group_count_ = 0;
  std::condition_variable cv_;
  bool groups_ready_ = false;

  // RPC 客户端池
  std::unordered_map<int, RpcClientPtr> rpc_clients_;
  std::mutex client_mutex_;

  int node_id_;
  int cluster_size_;
  std::vector<kv::rpc::NetAddress> peer_addrs_;
  std::function<void(const raft::GroupNotificationArgs&)> on_group_notification_;
};

}  // namespace multiraft
