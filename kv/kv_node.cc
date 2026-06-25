#include "kv_node.h"

#include "kv_server.h"
namespace kv {
KvServiceNode *KvServiceNode::NewKvServiceNode(const KvClusterConfig &config,
                                               raft::raft_node_id_t id) {
  auto ret = new KvServiceNode(config, id);
  return ret;
}

KvServiceNode::KvServiceNode(const KvClusterConfig &config, raft::raft_node_id_t id)
    : config_(config), id_(id) {
  auto raft_cluster_config = ConstructRaftClusterConfig(config);
  auto kv_node_config = config.at(id);
  auto raft_config = raft::RaftNode::NodeConfig{id, raft_cluster_config,
                                                kv_node_config.raft_log_filename, nullptr};
  kv_server_ = KvServer::NewKvServer(config, id);
  rpc_server_ = new rpc::KvServerRPCServer(kv_node_config.kv_rpc_addr, id);
  rpc_server_->SetServiceContext(kv_server_);
}

KvServiceNode *KvServiceNode::NewKvServiceNodeWithExternalRaftNode(
    const KvClusterConfig &config, raft::raft_node_id_t id, raft::RaftNode *external_raft_node) {
  return new KvServiceNode(config, id, external_raft_node);
}

KvServiceNode::KvServiceNode(const KvClusterConfig &config, raft::raft_node_id_t id,
                             raft::RaftNode *external_raft_node)
    : config_(config), id_(id) {
  assert(config.count(id) > 0);
  auto kv_node_config = config.at(id);
  kv_server_ = KvServer::NewKvServerWithExternalRaftNode(config, id, external_raft_node);
  rpc_server_ = nullptr;  // KV RPC server will be shared across all groups in Multi-Raft
}

KvServiceNode *KvServiceNode::NewKvServiceNodeWithoutRPC(const KvClusterConfig &config,
                                                         raft::raft_node_id_t id) {
  return new KvServiceNode(config, id, nullptr);
}

KvServiceNode::~KvServiceNode() {
  if (!kv_server_->Exited()) {
    kv_server_->Exit();
  }
  if (rpc_server_) {
    rpc_server_->Stop();
  }
  delete kv_server_;
  delete rpc_server_;
}

void KvServiceNode::InitServiceNodeState() { kv_server_->Init(); }

void KvServiceNode::StartServiceNode() {
  if (kv_server_) {
    kv_server_->Start();
  }
  if (rpc_server_) {
    rpc_server_->Start();
  }
  printf("[KVNode Start Running]:\n[Storage Engine]: %s\n", kv_server_->DB()->EngineName().c_str());
}

void KvServiceNode::StopServiceNode() {
  if (kv_server_ && !kv_server_->Exited()) {
    kv_server_->Exit();
  }
  if (rpc_server_) {
    rpc_server_->Stop();
  }
}

};  // namespace kv
