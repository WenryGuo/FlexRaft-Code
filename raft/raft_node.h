#pragma once
#include <atomic>
#include <cstdio>
#include <memory>
#include <unordered_map>

#include "raft.h"
#include "raft_type.h"
#include "rcf_rpc.h"
#include "rpc.h"
#include "rsm.h"
#include "storage.h"
#include "batch_system_bridge.h"
namespace raft {

// A raft node is the collection of raft state runtime, i.e. the raft node is
// responsible for maintaining the RaftState instance, creating RPC calls,
// persisting log entries, creating timer thread and so on.

namespace config {
static const int kRaftTickBaseInterval = 20;
}
class RaftNode : public std::enable_shared_from_this<RaftNode> {
 public:
  struct NodeConfig {
    raft_node_id_t node_id_me;
    std::unordered_map<raft_node_id_t, rpc::NetAddress> servers;
    // The storage file_name, not used for now
    std::string storage_filename;
    // TODO: Add state machine into this config
    Rsm *rsm;
    int N_physical_nodes = 0;
    // For Multi-Raft: if non-null, use BatchSystemBridge with this BatchSystem pointer
    void* batch_system = nullptr;
    void* raft_store = nullptr;  // pointer to multiraft::RaftStore
    // Dynamic encoding parameter: 0 = auto (k = live_servers - F),
    // non-zero = fixed k for comparison experiments (e.g., k=3, k=5).
    raft_encoding_param_t dynamic_k = 0;

    // Multi-raft encoding mode (mirrors multiraft::EncodingMode as int):
    //   0=RS_F, 1=RS_3F, 2=LRC. Controls commit threshold inside RaftState.
    // Plain int here to avoid pulling multiraft/ headers into raft/.
    int encoding_mode = 0;
  };

  // Constructor
  RaftNode(const NodeConfig &node_config);
  ~RaftNode();

  // Start running this raft node
  void Start();

  // Do all necessary initialization work before starting running this node
  // server
  void Init();

  // Calling exit to stop running this raft node, and release all resources
  void Exit();

  // Check if current node has exited
  bool Exited() { return exit_.load(); }

  // To stop and continue current node. This is typically an interface for
  // testing Raft cluster robustness under unstable network condition. We call
  // Pause() on this node to make an illusion that this node is separate from
  // the cluster void Pause(); void Continue();

  // Disconnect this node from cluster, i.e. This node can not receive inbound
  // RPC and is not able to send outbound RPC, however, the ticker thread may
  // also works. This function is basically for testing
  void Disconnect();
  void Reconnect();
  bool IsDisconnected() const { return disconnected_.load(); }

  // Optional injection of the unified RPC service for Multi-Raft.
  // When set, RaftNode will NOT create its own private RCFRpcServer,
  // and instead will be registered with the unified RaftUnifiedRpcServer.
  void SetUnifiedRpcService(void* unified_svc);
  void PostInit(raft_group_id_t group_id,
                void* unified_rpc_service,
                void* batch_transport);

  // For Multi-Raft: register an ApplyFsm mailbox so committed entries
  // flow from RaftState -> BatchSystemBridge -> ApplyFsm mailbox -> ApplyFsm -> RocksDB.
  // Call this after creating the RaftNode but before Start().
  void SetRsmAndApplyMailbox(raft_group_id_t group_id,
                             raft_node_id_t node_id,
                             void* batch_system,
                             void* raft_store);

  // Step 2 of mailbox setup: called AFTER batch_system_.RegisterApply() returns the mailbox.
  // This registers the ApplyFsm mailbox with the bridge.
  void RegisterApplyMailbox(void* mailbox_ptr);

  // Dynamic encoding parameter API: set fixed k for comparison experiments.
  // 0 = auto mode (k = live_servers - F), non-zero = fixed k.
  void SetDynamicK(raft_encoding_param_t k);
  raft_encoding_param_t GetDynamicK() const;
  int ClusterServerNum() const;

  // Set the LRC complementary grouper for latency-aware orthogonal placement
  void SetLrcGrouper(void* grouper);

  // NOTE: This method should only be used in test or debug mode
  RaftState *getRaftState() const;
  // Returns weak_ptr to raft_state_owner_ so async callbacks can safely extend lifetime
  std::weak_ptr<RaftState> getRaftStateOwnerWeakPtr() const;
  Rsm *getRsm();

  ProposeResult Propose(const CommandData &cmd);
  uint64_t CommitLatency(raft_index_t raft_index);
  raft::raft_index_t LastIndex() const;
  bool IsLeader();

 private:
  void startTickerThread();
  void startApplierThread();
  void startSyncThread();

 private:
  raft_node_id_t node_id_me_;
  std::unordered_map<raft_node_id_t, rpc::NetAddress> servers_;
  // Raw pointer for fast access in header methods.
  // thread-safe access via destroyed_ flag: any accessor must check destroyed_
  // before using this pointer.
  RaftState *raft_state_ = nullptr;

  // RPC related struct
  // In Multi-Raft mode (SetUnifiedRpcService called), this remains nullptr
  // and the shared RaftUnifiedRpcServer handles all RPC.
  rpc::RpcServer *rcf_server_ = nullptr;
  std::unordered_map<raft_node_id_t, rpc::RpcClient *> rcf_clients_;
  void* unified_rpc_service_ = nullptr;

  // Multi-Raft: bridge from RaftState::tryApplyLogEntries to ApplyFsm mailbox
  std::unique_ptr<BatchSystemBridge> batch_system_bridge_;
  uint32_t batch_system_group_id_ = 0;
  uint32_t batch_system_node_id_ = 0;
  void* batch_system_ptr_ = nullptr;
  void* raft_store_ptr_ = nullptr;

  // Indicating if this server has exited, i.e. Stop running, this is important
  // so that the ticker thread and applier thread can also exit normally
  std::atomic<bool> exit_;
  // Set to true AFTER Start() completes. Prevents double-start deadlock when
  // KvServer::Start() calls raft_->Start() after RaftNode was already started
  // by RaftStore::StartRaftInstances().
  std::atomic<bool> started_;
  // Set to true BEFORE destroying raft_state_. All async callbacks and ticker thread
  // must check this flag before accessing raft_state_.
  std::atomic<bool> destroyed_;
  std::atomic<bool> disconnected_;
  std::thread ticker_thread_;
  std::thread applier_thread_;
  std::thread sync_thread_;
  Rsm *rsm_;
  Storage *storage_;
  int N_physical_nodes_ = 0;
  raft_encoding_param_t dynamic_k_ = 0;  // 0 = auto, non-zero = fixed k
  int encoding_mode_ = 0;                 // 0=RS_F, 1=RS_3F, 2=LRC

  // shared_ptr owner for raft_state_: lives in .cc so callbacks can capture copies.
  // This ensures raft_state_ is not deleted while async callbacks are in-flight.
  std::shared_ptr<RaftState> raft_state_owner_;
};
}  // namespace raft
