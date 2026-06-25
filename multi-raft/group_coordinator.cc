// group_coordinator.cc — 分组协调器实现
//
// 负责管理 RPC 客户端连接池
//
// 注意：ExecuteGroupingAndNotify 已简化，不再使用旧的 LrcGroupBuilder API
// 分组功能已迁移到 RaftStore::BuildLrcGroups 和 LrcComplementaryGrouper

#include "group_coordinator.h"

#include <algorithm>
#include <random>
#include <chrono>

#include "serializer.h"
#include "rcf_rpc.h"

namespace multiraft {

// ============================================================================
//  GroupCoordinator 实现
// ============================================================================

GroupCoordinator::GroupCoordinator(
    int node_id,
    int cluster_size,
    const std::vector<kv::rpc::NetAddress>& peer_addrs,
    std::function<void(const raft::GroupNotificationArgs&)> on_group_notification)
    : node_id_(node_id),
      cluster_size_(cluster_size),
      peer_addrs_(peer_addrs),
      on_group_notification_(std::move(on_group_notification)) {

  printf("[COORDINATOR-N%d] GroupCoordinator initialized\n", node_id_);
  printf("[COORDINATOR-N%d] Cluster size: %d, Peer addresses: %zu\n",
         node_id_, cluster_size_, peer_addrs_.size());
}

GroupCoordinator::~GroupCoordinator() {
  Stop();
}

void GroupCoordinator::Stop() {
  printf("[COORDINATOR-N%d] Stopping GroupCoordinator\n", node_id_);
  std::lock_guard<std::mutex> lock(client_mutex_);
  rpc_clients_.clear();
}

std::shared_ptr<raft::rpc::RCFRpcClient> GroupCoordinator::GetRpcClient(int target_node_id) {
  std::lock_guard<std::mutex> lock(client_mutex_);

  if (target_node_id == node_id_) {
    return nullptr;  // 不能给自己创建 RPC 客户端
  }

  auto it = rpc_clients_.find(target_node_id);
  if (it != rpc_clients_.end()) {
    return it->second;
  }

  // 创建新的 RPC 客户端
  if (target_node_id >= 0 && target_node_id < static_cast<int>(peer_addrs_.size())) {
    const auto& addr = peer_addrs_[target_node_id];
    printf("[COORDINATOR-N%d] Creating RPC client to node %d at %s:%d\n",
           node_id_, target_node_id, addr.ip.c_str(), addr.port);

    auto client = std::make_shared<raft::rpc::RCFRpcClient>(
        raft::rpc::NetAddress{addr.ip, addr.port},
        static_cast<raft::raft_node_id_t>(target_node_id));
    client->Init();
    rpc_clients_[target_node_id] = client;
    return client;
  }

  return nullptr;
}

// 已废弃，分组功能已迁移到 LrcComplementaryGrouper
void GroupCoordinator::ExecuteGroupingAndNotify(int groups_per_node, int l, int k, int r) {
  printf("[COORDINATOR-N%d] WARNING: ExecuteGroupingAndNotify is deprecated.\n", node_id_);
  printf("[COORDINATOR-N%d] Grouping is now handled by RaftStore::BuildLrcGroups().\n", node_id_);
  printf("[COORDINATOR-N%d] Use LrcComplementaryGrouper for grouping instead.\n", node_id_);
}

int GroupCoordinator::GetLocalGroupCount() const {
  std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mu_));
  return local_group_count_;
}

void GroupCoordinator::WaitForGroupsReady(int expected_groups) {
  printf("\n[COORDINATOR-N%d] Waiting for all groups to be ready (expected: %d)...\n",
         node_id_, expected_groups);

  std::unique_lock<std::mutex> lock(mu_);
  cv_.wait(lock, [this, expected_groups]() {
    return groups_ready_ || notified_group_count_ >= expected_groups;
  });

  printf("[COORDINATOR-N%d] Groups are ready! (notified: %d)\n",
         node_id_, notified_group_count_);
}

}  // namespace multiraft
