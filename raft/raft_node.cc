#include "raft_node.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "batch_system_bridge.h"
#include "raft.h"
#include "rcf_rpc.h"
#include "rsm.h"
#include "storage.h"
#include "util.h"
#include "raft_unified_rpc_server.h"

namespace raft {

RaftNode::RaftNode(const NodeConfig &node_config)
    : node_id_me_(node_config.node_id_me),
      servers_(node_config.servers),
      raft_state_(nullptr),
      rsm_(node_config.rsm),
      N_physical_nodes_(node_config.N_physical_nodes),
      dynamic_k_(node_config.dynamic_k),
      encoding_mode_(node_config.encoding_mode) {
  if (node_config.storage_filename != "") {
    storage_ = FileStorage::Open(node_config.storage_filename);
  } else {
    storage_ = nullptr;
  }
}

void RaftNode::Init() {
  // [MULTI-RAFT FIX] 如果已设置了 unified_rpc_service_，说明在 Multi-Raft 模式下运行，
  // 不再创建私有 rcf_server_（避免与 RaftUnifiedRpcServer 端口冲突）。
  // RaftState 的初始化改到 RaftNode::Start() 中调用。
  if (unified_rpc_service_ != nullptr) {
  // Multi-Raft 路径：跳过 server 创建，只创建 RPC clients 和 RaftState
  for (const auto &[id, addr] : servers_) {
    if (id != node_id_me_) {
      rcf_clients_.insert({id, new rpc::RCFRpcClient(addr, id)});
    }
  }

  Rsm* rsm_to_use = nullptr;

  RaftConfig config = RaftConfig{
      .id = node_id_me_, .group_id = 0, .rpc_clients = rcf_clients_, .storage = storage_,
      .electionTimeMin = config::kElectionTimeoutMin,
      .electionTimeMax = config::kElectionTimeoutMax, .rsm = rsm_to_use,
      .N_physical_nodes = N_physical_nodes_,
      .batch_system_bridge = batch_system_bridge_.get(),
      .bridge_group_id = batch_system_group_id_,
      .bridge_node_id = batch_system_node_id_,
      .dynamic_k = dynamic_k_,
      .encoding_mode = encoding_mode_};
  RaftState* rs_raw = RaftState::NewRaftState(config);
  raft_state_owner_.reset(rs_raw, [](RaftState* p) { delete p; });
  raft_state_ = rs_raw;

    for (auto &[_, client] : rcf_clients_) {
      client->setState(raft_state_);
      client->setRaftStateOwnerPtr(raft_state_owner_);
    }
  } else {
    // 原有单 Raft 路径：创建私有 server + clients + RaftState
    rcf_server_ = new rpc::RCFRpcServer(servers_[node_id_me_]);
    rcf_server_->Start();

    for (const auto &[id, addr] : servers_) {
      if (id != node_id_me_) {
        rcf_clients_.insert({id, new rpc::RCFRpcClient(addr, id)});
      }
    }

    RaftConfig config = RaftConfig{
        .id = node_id_me_, .group_id = 0, .rpc_clients = rcf_clients_, .storage = storage_,
        .electionTimeMin = config::kElectionTimeoutMin,
        .electionTimeMax = config::kElectionTimeoutMax, .rsm = rsm_,
        .N_physical_nodes = N_physical_nodes_,
        .dynamic_k = dynamic_k_,
        .encoding_mode = encoding_mode_};
    RaftState* rs_raw = RaftState::NewRaftState(config);
    raft_state_owner_.reset(rs_raw, [](RaftState* p) { delete p; });
    raft_state_ = rs_raw;

    rcf_server_->setState(raft_state_);
    for (auto &[_, client] : rcf_clients_) {
      client->setState(raft_state_);
      client->setRaftStateOwnerPtr(raft_state_owner_);
    }
  }

  exit_.store(false);
  destroyed_.store(false);
  disconnected_.store(false);
}

void RaftNode::Start() {
  if (started_.load(std::memory_order_acquire)) {
    return;
  }
  LOG(util::kRaft, "S%d Starts", node_id_me_);
  printf("[FlexibleK Raft Starts Running]\n");
  fflush(stdout);
  exit_.store(false);
  if (raft_state_ != nullptr) {
    raft_state_->Init();
  }
  startTickerThread();
  startApplierThread();
  startSyncThread();
  started_.store(true, std::memory_order_release);
}

void RaftNode::Exit() {
  // First ensures ticker thread and applier thread exits, in case they access
  // raft_state field after we release it
  this->exit_.store(true);
  // Signal destroyed_ FIRST, then wait for threads to see it
  this->destroyed_.store(true);

  // Join the ticker thread if it's joinable
  if (ticker_thread_.joinable()) {
    ticker_thread_.join();
  }
  if (applier_thread_.joinable()) {
    applier_thread_.join();
  }
  if (sync_thread_.joinable()) {
    sync_thread_.join();
  }

  // raft_state_owner_ shared_ptr will release when all async callbacks are done.
  // raft_state_ raw pointer is no longer safe to access after this point.

  // TODO: Release storage or state machine if it's necessary
  // Multi-Raft: unified server is shared, not owned by this RaftNode
  if (rcf_server_) {
    rcf_server_->Stop();
    delete rcf_server_;
    rcf_server_ = nullptr;
  }
  delete storage_;
  storage_ = nullptr;
  raft_state_ = nullptr;
}

RaftNode::~RaftNode() {
  // If Exit() was not called, join threads here too
  exit_.store(true);
  destroyed_.store(true);
  if (ticker_thread_.joinable()) {
    ticker_thread_.join();
  }
  if (applier_thread_.joinable()) {
    applier_thread_.join();
  }
  if (sync_thread_.joinable()) {
    sync_thread_.join();
  }
  // Signal all RPC clients to stop before deleting them.
  // This sets destroyed_=true on each RCFRpcClient so any pending async
  // callbacks bail out immediately upon invocation.
  for (auto &[_, client] : rcf_clients_) {
    client->stop();
    delete client;
  }
  rcf_clients_.clear();
  // raft_state_owner_ deleter will run when last async callback releases its copy.
  // After this destructor completes and all callbacks finish, raft_state_ will be deleted.
  raft_state_ = nullptr;
}

void RaftNode::startTickerThread() {
  // Capture the shared_ptr to keep RaftState alive while the ticker thread runs.
  // Even after ~RaftNode sets destroyed_=true, the shared_ptr copy in this thread
  // keeps the object alive until the thread exits.
  auto ticker = [sp = raft_state_owner_, this]() noexcept(false) {
    printf("[RAFT-NODE-N%d] Ticker thread started\n", node_id_me_);
    try {
      while (true) {
        // Check destroyed_ flag first (set by ~RaftNode / Exit)
        if (destroyed_.load(std::memory_order_acquire)) break;
        RaftState* rs = raft_state_;
        if (!rs) break;
        rs->Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(config::kRaftTickBaseInterval));
      }
    } catch (const std::exception& e) {
      fprintf(stderr, "[RAFT-NODE-N%d] Ticker thread caught std::exception: %s\n",
              node_id_me_, e.what());
    } catch (...) {
      fprintf(stderr, "[RAFT-NODE-N%d] Ticker thread caught unknown exception\n",
              node_id_me_);
    }
    printf("[RAFT-NODE-N%d] Ticker thread exiting\n", node_id_me_);
  };
  ticker_thread_ = std::thread(ticker);
}

void RaftNode::Disconnect() {
  LOG(util::kRaft, "S%d stop running raft node", node_id_me_);
  // Multi-Raft: unified server is shared, not owned by this RaftNode
  if (rcf_server_) {
    rcf_server_->Stop();
  }
  for (auto [_, client_ptr] : rcf_clients_) {
    client_ptr->stop();
  }
  disconnected_.store(true);
}

void RaftNode::Reconnect() {
  // Multi-Raft: unified server is shared, not owned by this RaftNode
  if (rcf_server_) {
    rcf_server_->Start();
  }
  for (auto [_, client_ptr] : rcf_clients_) {
    client_ptr->recover();
  }
  disconnected_.store(false);
}

void RaftNode::SetUnifiedRpcService(void* unified_svc) {
  unified_rpc_service_ = unified_svc;
}

void RaftNode::PostInit(raft_group_id_t group_id,
                         void* unified_rpc_service,
                         void* batch_transport) {
  // Register this RaftNode's RaftState with the unified RPC service
  if (unified_rpc_service && raft_state_) {
    auto* svc = static_cast<raft::rpc::RaftUnifiedRPCService*>(unified_rpc_service);
    svc->RegisterRaftState(group_id, raft_state_);
    printf("[RAFT-NODE-N%d] Registered with unified RPC service for group=%u\n",
           node_id_me_, group_id);
  }
  // Set group_id on RaftState for logging purposes
  if (raft_state_) {
    raft_state_->SetGroupId(group_id);
  }
  // Wire batch transport into RaftState so it can send via BatchTransport.
  // The transport handler (RaftState itself) will be registered by AttachToRaftState().
  if (batch_transport && raft_state_) {
    raft_state_->SetBatchTransport(batch_transport);
    printf("[RAFT-NODE-N%d] BatchTransport wired for group=%u\n",
           node_id_me_, group_id);
  }
  (void)batch_transport;
}

void RaftNode::startApplierThread() {
  // Multi-Raft with BatchSystemBridge: entries are pushed to ApplyFsm mailbox
  // by BatchSystemBridge::ApplyEntry, polled by ApplyFsm threads.
  // We don't need a separate applier thread here.
}

void RaftNode::startSyncThread() {
  // Background thread: periodically calls storage_->Sync() to persist raft state.
  // This decouples disk I/O from the Propose() critical path — Propose() only
  // does AppendEntry (buffered write) and returns immediately; this thread
  // periodically flushes to disk via fsync().
  if (storage_ == nullptr) {
    return;  // no storage, nothing to sync
  }
  sync_thread_ = std::thread([sp = raft_state_owner_, this]() noexcept(false) {
    printf("[RAFT-NODE-N%d] Sync thread started\n", node_id_me_);
    while (true) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (destroyed_.load(std::memory_order_acquire)) break;
      RaftState* rs = raft_state_;
      if (!rs) break;
      Storage* st = rs->storage_;
      if (st != nullptr) {
        st->Sync();  // periodic fsync — not on every propose
      }
    }
    printf("[RAFT-NODE-N%d] Sync thread exiting\n", node_id_me_);
  });
}

void RaftNode::SetRsmAndApplyMailbox(raft_group_id_t group_id,
                                      raft_node_id_t node_id,
                                      void* batch_system,
                                      void* raft_store) {
  batch_system_group_id_ = group_id;
  batch_system_node_id_ = node_id;
  batch_system_ptr_ = batch_system;
  raft_store_ptr_ = raft_store;

  // Create the bridge object (owned by RaftNode)
  batch_system_bridge_ = std::make_unique<BatchSystemBridge>();
  printf("[RAFT-NODE-N%d] BatchSystemBridge created for group=%u\n",
         node_id_me_, group_id);
}

void RaftNode::RegisterApplyMailbox(void* mailbox_ptr) {
  if (batch_system_bridge_) {
    BatchSystemBridge::RegisterMailbox(
        batch_system_group_id_, batch_system_node_id_, mailbox_ptr);
    printf("[RAFT-NODE-N%d] ApplyFsm mailbox registered for group=%u\n",
           node_id_me_, batch_system_group_id_);
  }
}

void RaftNode::SetDynamicK(raft_encoding_param_t k) {
  if (!destroyed_.load(std::memory_order_acquire) && raft_state_) {
    raft_state_->SetDynamicK(k);
  }
}

void RaftNode::SetLrcGrouper(void* grouper) {
  if (!destroyed_.load(std::memory_order_acquire) && raft_state_) {
    raft_state_->SetLrcGrouper(grouper);
  }
}

raft_encoding_param_t RaftNode::GetDynamicK() const {
  if (!destroyed_.load(std::memory_order_acquire) && raft_state_) {
    return raft_state_->GetDynamicK();
  }
  return 0;
}
int RaftNode::ClusterServerNum() const {
  if (!destroyed_.load(std::memory_order_acquire) && raft_state_) {
    return raft_state_->GetClusterServerNumber();
  }
  return 0;
}
RaftState* RaftNode::getRaftState() const {
  return raft_state_;
}

std::weak_ptr<RaftState> RaftNode::getRaftStateOwnerWeakPtr() const {
  return raft_state_owner_;
}

Rsm* RaftNode::getRsm() {
  return rsm_;
}
ProposeResult RaftNode::Propose(const CommandData &cmd) {
  if (!destroyed_.load(std::memory_order_acquire) && raft_state_) {
    return raft_state_->Propose(cmd);
  }
  return {0, 0, false};
}
uint64_t RaftNode::CommitLatency(raft_index_t raft_index) {
  if (!destroyed_.load(std::memory_order_acquire) && raft_state_) {
    return raft_state_->CommitLatency(raft_index);
  }
  return 0;
}
raft::raft_index_t RaftNode::LastIndex() const {
  if (!destroyed_.load(std::memory_order_acquire) && raft_state_) {
    return raft_state_->LastIndex();
  }
  return 0;
}
bool RaftNode::IsLeader() {
  if (!destroyed_.load(std::memory_order_acquire) && raft_state_) {
    return raft_state_->Role() == kLeader;
  }
  return false;
}

}  // namespace raft
