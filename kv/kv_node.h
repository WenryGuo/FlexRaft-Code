#pragma once
#include "config.h"
#include "kv_server.h"
#include "raft_type.h"
#include "rpc.h"
#include "util.h"
namespace kv {

// A KvServiceNode is basically an adapter that combines the KvServer and
// RPC server
class KvServiceNode {
 public:
  static KvServiceNode *NewKvServiceNode(const KvClusterConfig &config, raft::raft_node_id_t id);
  // Create with an externally-provided RaftNode (for Multi-Raft)
  static KvServiceNode *NewKvServiceNodeWithExternalRaftNode(const KvClusterConfig &config,
                                                           raft::raft_node_id_t id,
                                                           raft::RaftNode *external_raft_node);
  KvServiceNode() = default;
  ~KvServiceNode();

  KvServiceNode(const KvClusterConfig &config, raft::raft_node_id_t id);
  // Constructor with external RaftNode (for Multi-Raft)
  KvServiceNode(const KvClusterConfig &config, raft::raft_node_id_t id, raft::RaftNode *external_raft_node);

  // Create without any RaftNode or RPC server (for Multi-Raft where they're managed externally)
  static KvServiceNode *NewKvServiceNodeWithoutRPC(const KvClusterConfig &config, raft::raft_node_id_t id);
  void InitServiceNodeState();
  void StartServiceNode();
  void StopServiceNode();

  // This is for debug and test
  void Disconnect() {
    LOG(raft::util::kRaft, "S%d Disconnect", id_);
    rpc_server_->Stop();
    kv_server_->Disconnect();
  }

  void Reconnect() {
    LOG(raft::util::kRaft, "S%d Reconnect", id_);
    kv_server_->Reconnect();
    rpc_server_->Start();
  }

  bool IsDisconnected() const { return kv_server_->IsDisconnected(); }

  bool IsLeader() const { return kv_server_->IsLeader(); }

  KvServer* GetKvServer() { return kv_server_; }
  raft::RaftNode* GetRaftNode() { return kv_server_ ? kv_server_->GetRaftNode() : nullptr; }

  // Set an externally-managed RaftNode (convenience wrapper over KvServer::SetRaftNode)
  void SetRaftNode(raft::RaftNode* raft_node, bool owned = false) {
    if (kv_server_) kv_server_->SetRaftNode(raft_node, owned);
  }

  // Set KV peer stubs for cross-node GetValue RPCs (used by stripe_read)
  void SetKVPeerServers(const std::vector<kv::rpc::NetAddress>& peer_kv_addrs,
                       const std::vector<raft::raft_node_id_t>& peer_ids) {
    if (!kv_server_) return;
    for (size_t i = 0; i < peer_ids.size() && i < peer_kv_addrs.size(); ++i) {
      kv_server_->AddKVPeer(peer_ids[i], new rpc::KvServerRPCClient(peer_kv_addrs[i], peer_ids[i]));
    }
  }

  // Called by ApplyFsm after applying committed entries (Multi-Raft path).
  void UpdateAppliedIndex(raft::raft_index_t idx) {
    if (kv_server_) kv_server_->UpdateAppliedIndex(idx);
  }

 private:
  KvClusterConfig config_;
  raft::raft_node_id_t id_;
  KvServer *kv_server_;
  rpc::KvServerRPCServer *rpc_server_;
};
}  // namespace kv
