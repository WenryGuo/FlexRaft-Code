#pragma once
#include <atomic>
#include <mutex>
#include <unordered_map>

#include "channel.h"
#include "concurrent_queue.h"
#include "config.h"
#include "kv_format.h"
#include "raft.h"
#include "raft_node.h"
#include "raft_type.h"
#include "storage_engine.h"

namespace kv {

struct KvServerConfig {
  raft::RaftNode::NodeConfig raft_node_config;
  std::string storage_engine_name;
};

class KvServer {
 public:
  struct KvRequestApplyResult {
    raft::raft_term_t raft_term;
    ErrorType err;
    std::string value;
    uint64_t elapse_time;  // Time used to apply this entry, in us
  };

  struct ValueGatheringTask {
    std::string key;
    raft::raft_index_t read_index;
    raft::raft_node_id_t replied_id;
    raft::Encoder::EncodingResults *decode_input;
    int k, m;
    raft::raft_group_id_t group_id = static_cast<raft::raft_group_id_t>(-1);
  };

  struct ValueGatheringTaskResults {
    std::string *value;
    ErrorType err;
  };

 public:
  KvServer() = default;
  ~KvServer() {
    delete channel_;
    delete db_;
    if (raft_owned_) {
      delete raft_;
    }
  }

 public:
  static KvServer *NewKvServer(const KvServerConfig &kv_server_config);
  static KvServer *NewKvServer(const KvClusterConfig &kv_cluster_config,
                               raft::raft_node_id_t node_id);
  // Create KvServer with an externally-provided RaftNode (for Multi-Raft)
  static KvServer *NewKvServerWithExternalRaftNode(const KvClusterConfig &kv_cluster_config,
                                                  raft::raft_node_id_t node_id,
                                                  raft::RaftNode *external_raft_node);
  // Create KvServer without its own RaftNode (for Multi-Raft, where RaftNode is managed externally)
  static KvServer *NewKvServerWithoutRaft(const KvClusterConfig &kv_cluster_config,
                                          raft::raft_node_id_t node_id);

 public:
  void DoValueGatheringTask(ValueGatheringTask *task, ValueGatheringTaskResults *res);
  void DealWithRequest(const Request *request, Response *resp);
  // Disable this server

  // Start running this kv server
  void Start();

  // Do necessary initialize work, for example, apply existed Snapshot to
  // storage engine(Not implemented yet), currently just initialize raft
  // state. In Multi-Raft mode, raft_ may be nullptr (injected later),
  // so we skip raft initialization in that case.
  void Init() {
    if (raft_) {
      raft_->Init();
    }
  }

  auto Id() const { return id_; }

  bool IsLeader() const { return raft_->IsLeader(); }

  raft::raft_index_t LastApplyIndex() const { return applied_index_.load(std::memory_order_acquire); }

  // Called by ApplyFsm after committing entries in Multi-Raft mode.
  // Ensures ExecuteGetOperation can spin-wait on the correct applied_index.
  void UpdateAppliedIndex(raft::raft_index_t idx) {
    auto prev = applied_index_.load(std::memory_order_relaxed);
    if (idx > prev) {
      applied_index_.store(idx, std::memory_order_release);
    }
  }

 private:
  // Check if a log entry has been committed yet
  bool CheckEntryCommitted(const raft::ProposeResult &pr, KvRequestApplyResult *apply);

 public:
  // A thread that periodically apply committed raft log entries to KVRsm
  static void ApplyRequestCommandThread(KvServer *server);

  void startApplyKvRequestCommandsThread() {
    std::thread t(ApplyRequestCommandThread, this);
    t.detach();
  }

 public:
  // For test and debug
  void Exit() {
    exit_.store(true);
    raft_->Exit();
    db_->Close();
  };

  bool Exited() const { return exit_.load(); }

  void Disconnect() { raft_->Disconnect(); }

  bool IsDisconnected() const { return raft_->IsDisconnected(); }

  void Reconnect() { raft_->Reconnect(); }

  StorageEngine *DB() { return db_; }

  raft::RaftNode* GetRaftNode() { return raft_; }

  // Get the RSM channel (used to connect committed entries to ApplyRequestCommandThread)
  Channel* GetChannel() { return channel_; }

  // Set an externally-managed RaftNode (for Multi-Raft).
  // The externally-provided RaftNode is OWNED by the caller (RaftStore),
  // so KvServer will NOT delete it in its destructor.
  // Also connects the KvServer's RSM channel so that committed entries
  // flow through BOTH the BatchSystemBridge (ApplyFsm) and the
  // KvServer's channel_ (ApplyRequestCommandThread → applied_cmds_).
  void SetRaftNode(raft::RaftNode* raft_node, bool owned = false) {
    raft_ = raft_node;
    raft_owned_ = owned;
    // Multi-Raft fix: connect KvServer's RSM channel to the RaftNode's RaftState.
    // This ensures committed entries flow to BOTH the BatchSystemBridge (ApplyFsm)
    // AND the KvServer's channel_ (ApplyRequestCommandThread → applied_cmds_).
    // Without this, CheckEntryCommitted always fails because applied_cmds_ is
    // never populated.
    if (raft_node && channel_) {
      auto* rs = raft_node->getRaftState();
      if (rs) {
        rs->SetRsm(channel_);
      }
    }
  }

  bool IsRaftOwned() const { return raft_owned_; }

  void ExecuteGetOperation(const Request *request, Response *resp);

  int ClusterServerNum() const { return raft_->ClusterServerNum(); }

  auto GetKVPeerServerStub(raft::raft_node_id_t node_id) -> void * {
    if (kv_peers_.count(node_id) == 0) {
      return nullptr;
    }
    return kv_peers_[node_id];
  }

  void AddKVPeer(raft::raft_node_id_t node_id, void *stub) {
    kv_peers_[node_id] = stub;
  }

 private:
  raft::RaftNode *raft_;
  bool raft_owned_ = true;  // true = KvServer owns raft_, false = external owner deletes

  // channel is used to interact with lower level raft library
  Channel *channel_;
  StorageEngine *db_;

  // We need a channel that receives apply messages from raft
  // We need a concurrent map to record which entries have been applied,
  // associated with their term and value (if it is a Get operation)
  std::unordered_map<raft::raft_index_t, KvRequestApplyResult> applied_cmds_;
  std::mutex map_mutex_;

  // Check if this server has exited
  std::atomic<bool> exit_;

  raft::raft_node_id_t id_;

  // The raft index of last applied entry
  std::atomic<raft::raft_index_t> applied_index_{0};

  // Record the RPC stub for each peer kv server. We can only store void* here
  // to avoid circle reference problem
  std::unordered_map<raft::raft_node_id_t, void *> kv_peers_;
};
}  // namespace kv
