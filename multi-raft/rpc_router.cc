// rpc_router.cc — Multi-Raft 统一 RPC 消息路由器实现

#include "rpc_router.h"

#include <cassert>

#include "log_entry.h"
#include "raft_type.h"

namespace multiraft {

RpcRouter::RpcRouter(int node_id) : node_id_(node_id) {
  printf("[ROUTER-N%d] RpcRouter created\n", node_id_);
}

RpcRouter::~RpcRouter() {
  printf("[ROUTER-N%d] RpcRouter destroyed\n", node_id_);
}

void RpcRouter::RegisterPeerMailbox(GroupId group_id, raft::raft_node_id_t local_id,
                                    Mailbox<PeerMsg>* mb) {
  std::lock_guard<std::mutex> lk(mu_);

  uint64_t key = MakeKey(group_id, local_id);
  peers_[key] = mb;

  // 更新 group_peers_ 索引
  auto it = group_peers_.find(group_id);
  if (it == group_peers_.end()) {
    group_peers_[group_id] = std::vector<uint64_t>();
  }
  group_peers_[group_id].push_back(key);

  printf("[ROUTER-N%d] Registered PeerFsm Mailbox: group=%u node=%u mb=%p\n",
         node_id_, group_id, local_id, (void*)mb);
  printf("[ROUTER-N%d] Total registered peers: %zu groups: %zu\n",
         node_id_, peers_.size(), group_peers_.size());
}

void RpcRouter::UnregisterPeerMailbox(GroupId group_id, raft::raft_node_id_t local_id) {
  std::lock_guard<std::mutex> lk(mu_);

  uint64_t key = MakeKey(group_id, local_id);
  peers_.erase(key);

  // 更新 group_peers_ 索引
  auto it = group_peers_.find(group_id);
  if (it != group_peers_.end()) {
    auto& vec = it->second;
    vec.erase(std::remove(vec.begin(), vec.end(), key), vec.end());
    if (vec.empty()) {
      group_peers_.erase(it);
    }
  }

  printf("[ROUTER-N%d] Unregistered PeerFsm Mailbox: group=%u node=%u\n",
         node_id_, group_id, local_id);
}

Mailbox<PeerMsg>* RpcRouter::GetGroupMailbox(GroupId to_group) {
  std::lock_guard<std::mutex> lk(mu_);

  auto it = group_peers_.find(to_group);
  if (it != group_peers_.end() && !it->second.empty()) {
    uint64_t key = it->second[0];
    auto peer_it = peers_.find(key);
    if (peer_it != peers_.end()) {
      return peer_it->second;
    }
  }
  return nullptr;
}

Mailbox<PeerMsg>* RpcRouter::GetPeerMailbox(GroupId group_id, raft::raft_node_id_t node_id) {
  std::lock_guard<std::mutex> lk(mu_);

  uint64_t key = MakeKey(group_id, node_id);
  auto it = peers_.find(key);
  if (it != peers_.end()) {
    return it->second;
  }
  return nullptr;
}

int RpcRouter::GetRegisteredPeerCount() const {
  std::lock_guard<std::mutex> lk(mu_);
  return static_cast<int>(peers_.size());
}

int RpcRouter::GetRegisteredGroupCount() const {
  std::lock_guard<std::mutex> lk(mu_);
  return static_cast<int>(group_peers_.size());
}

bool RpcRouter::IsGroupRegistered(GroupId group_id) const {
  std::lock_guard<std::mutex> lk(mu_);
  return group_peers_.find(group_id) != group_peers_.end();
}

std::vector<Mailbox<PeerMsg>*> RpcRouter::GetGroupPeers(GroupId group_id) {
  std::lock_guard<std::mutex> lk(mu_);

  std::vector<Mailbox<PeerMsg>*> result;
  auto it = group_peers_.find(group_id);
  if (it != group_peers_.end()) {
    for (uint64_t key : it->second) {
      auto peer_it = peers_.find(key);
      if (peer_it != peers_.end()) {
        result.push_back(peer_it->second);
      }
    }
  }
  return result;
}

}  // namespace multiraft