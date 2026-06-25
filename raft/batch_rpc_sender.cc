// batch_rpc_sender.cc — Per-Node 异步连接池实现

#include "batch_rpc_sender.h"

#include <cstdio>
#include <memory>

#include "rcf_rpc.h"
#include "raft_struct.h"
#include "serializer.h"

namespace raft {

// =======================================================================
//  AsyncBatchRpcSenderImpl 实现
// =======================================================================

AsyncBatchRpcSenderImpl::AsyncBatchRpcSenderImpl() : self_id_(0) {
  stats_ = {0, 0, 0, 0};
}

AsyncBatchRpcSenderImpl::~AsyncBatchRpcSenderImpl() {}

void AsyncBatchRpcSenderImpl::Init(
    raft_node_id_t self_id,
    const std::unordered_map<raft_node_id_t, rpc::NetAddress>& peers) {
  self_id_ = self_id;

  std::lock_guard<std::mutex> lock(pools_mutex_);

  for (const auto& [id, addr] : peers) {
    if (id == self_id) {
      continue;
    }

    auto pool = std::make_unique<NodePool>();
    for (size_t i = 0; i < kPoolSize; ++i) {
      pool->conns[i].client = std::make_unique<rpc::RcfClient<rpc::I_RaftRPCService>>(
          RCF::TcpEndpoint(addr.ip, addr.port));
      auto* stub = &pool->conns[i].client->getClientStub();
      stub->getTransport().setMaxOutgoingMessageLength(rpc::config::kMaxMessageLength);
      stub->getTransport().setMaxIncomingMessageLength(rpc::config::kMaxMessageLength);
      // Default timeout for batch sender (AppendEntries uses 500ms)
      stub->setRemoteCallTimeoutMs(rpc::config::kAppendEntriesTimeout);
    }

    addr_map_[id] = addr;
    node_pools_[id] = std::move(pool);

    printf("[ASYNC-SENDER] Created pool (%zu conns) to node %u at %s:%d\n",
           kPoolSize, id, addr.ip.c_str(), addr.port);
  }
}

AsyncBatchRpcSenderImpl::PooledConn* AsyncBatchRpcSenderImpl::GetPooledConn(raft_node_id_t to) {
  std::lock_guard<std::mutex> lock(pools_mutex_);
  auto it = node_pools_.find(to);
  if (it == node_pools_.end()) {
    printf("[ASYNC-SENDER] ERROR: No pool found for node %u\n", to);
    return nullptr;
  }
  // 全局 round-robin: 所有 peer 共享同一个计数器，确保每个 peer 的 4 个连接都能被均匀使用
  size_t idx = global_pool_idx_.fetch_add(1) % kPoolSize;
  return &it->second->conns[idx];
}

void AsyncBatchRpcSenderImpl::SendBatch(
    raft_node_id_t to,
    const std::vector<RaftRpcMessage>& messages) {
  if (messages.empty()) {
    return;
  }

  printf("[ASYNC-SENDER] SendBatch: %zu messages to node %u\n", messages.size(), to);
  fflush(stdout);

  // 预先增加 in_flight 计数器（每条消息一次）
  rpcs_in_flight_.fetch_add(messages.size());

  // 检查目标节点是否存在
  {
    std::lock_guard<std::mutex> lock(pools_mutex_);
    if (addr_map_.find(to) == addr_map_.end()) {
      // 回退计数器
      rpcs_in_flight_.fetch_sub(messages.size());
      return;
    }
  }

  // 异步发送：每条消息走独立连接（全局 round-robin），不等待响应
  // RPC 完成后的处理在回调中进行（回调会正确路由响应）
  for (const auto& msg : messages) {
    PooledConn* conn = GetPooledConn(to);
    if (!conn) {
      rpcs_in_flight_.fetch_sub(1);
      continue;
    }

    // 转换为内部 BatchRpcMessage
    BatchRpcMessage batch_msg;
    batch_msg.type = msg.type;
    batch_msg.group_id = msg.group_id;
    batch_msg.serialized_data = msg.serialized_data;

    // 获取该连接的 mutex，在锁内发送（RCF 要求同一连接不能并发调用）
    printf("[ASYNC-SENDER] SendBatch: dispatching msg_type=%d to node %u\n",
           static_cast<int>(msg.type), to);
    fflush(stdout);
    std::lock_guard<std::mutex> lk(conn->mtx);
    DispatchRpcAsync(conn, batch_msg);
  }

  // 更新统计
  {
    std::lock_guard<std::mutex> lock(stats_mtx_);
    stats_.total_batches_sent++;
    stats_.total_messages_sent += messages.size();
    stats_.total_async_sends += messages.size();
  }
}

size_t AsyncBatchRpcSenderImpl::SendBatchAndTrack(
    raft_node_id_t to,
    const std::vector<RaftRpcMessage>& messages) {
  if (messages.empty()) {
    return rpcs_in_flight_.load();
  }
  size_t baseline = rpcs_in_flight_.load();
  SendBatch(to, messages);
  return baseline;
}

void AsyncBatchRpcSenderImpl::WaitUntilInflightBelow(size_t target) {
  std::unique_lock<std::mutex> lk(inflight_mtx_);
  inflight_cv_.wait(lk, [this, target]() {
    return rpcs_in_flight_.load() <= target;
  });
}

void AsyncBatchRpcSenderImpl::DecrementInflightAndNotify() {
  size_t old = rpcs_in_flight_.fetch_sub(1);
  if (old <= 1) {
    std::lock_guard<std::mutex> lk(inflight_mtx_);
    inflight_cv_.notify_all();
  }
}

void AsyncBatchRpcSenderImpl::DispatchRpcAsync(
    PooledConn* conn,
    const BatchRpcMessage& msg) {

  // 预分配 ByteBuffer 并拷贝序列化数据（RCF 要求可写 buffer）
  size_t size = msg.serialized_data.size();
  RCF::ByteBuffer arg_buf(size);
  if (size > 0) {
    std::copy(msg.serialized_data.begin(), msg.serialized_data.end(), arg_buf.getPtr());
  }

  auto* client = conn->client.get();

  switch (msg.type) {
    case RaftRpcType::kRequestVote: {
      RCF::Future<RCF::ByteBuffer> ret;
      auto cb = [=]() {
        OnRequestVoteComplete(ret, msg.group_id, this);
      };
      ret = client->RequestVote(RCF::AsyncTwoway(cb), arg_buf);
      break;
    }
    case RaftRpcType::kAppendEntries: {
      RCF::Future<RCF::ByteBuffer> ret;
      auto cb = [=]() {
        OnAppendEntriesComplete(ret, msg.group_id, this);
      };
      ret = client->AppendEntries(RCF::AsyncTwoway(cb), arg_buf);
      break;
    }
    case RaftRpcType::kRequestFragments: {
      RCF::Future<RCF::ByteBuffer> ret;
      auto cb = [=]() {
        OnRequestFragmentsComplete(ret, msg.group_id, this);
      };
      ret = client->RequestFragments(RCF::AsyncTwoway(cb), arg_buf);
      break;
    }
    default:
      printf("[ASYNC-SENDER] WARNING: Unknown RPC type %d\n",
             static_cast<int>(msg.type));
      break;
  }
}

void AsyncBatchRpcSenderImpl::OnRequestVoteComplete(
    RCF::Future<RCF::ByteBuffer> ret,
    raft_group_id_t group_id,
    AsyncBatchRpcSenderImpl* sender) {
  auto e = ret.getAsyncException();
  if (e.get()) {
    printf("[DEBUG-CB] RequestVote RPC Error: %s\n", e->getErrorString().c_str());
    std::lock_guard<std::mutex> lock(sender->stats_mtx_);
    sender->stats_.total_errors++;
  } else {
    RCF::ByteBuffer ret_buf = *ret;
    RequestVoteReply reply;
    Serializer::NewSerializer().Deserialize(&ret_buf, &reply);
    sender->HandleRequestVoteReply(reply);
  }
  sender->rpcs_in_flight_.fetch_sub(1);
}

void AsyncBatchRpcSenderImpl::OnAppendEntriesComplete(
    RCF::Future<RCF::ByteBuffer> ret,
    raft_group_id_t group_id,
    AsyncBatchRpcSenderImpl* sender) {
  auto e = ret.getAsyncException();
  if (e.get()) {
    printf("[DEBUG-CB] AppendEntries RPC Error: %s\n", e->getErrorString().c_str());
    std::lock_guard<std::mutex> lock(sender->stats_mtx_);
    sender->stats_.total_errors++;
  } else {
    RCF::ByteBuffer ret_buf = *ret;
    AppendEntriesReply reply;
    Serializer::NewSerializer().Deserialize(&ret_buf, &reply);
    sender->HandleAppendEntriesReply(reply);
  }
  sender->rpcs_in_flight_.fetch_sub(1);
}

void AsyncBatchRpcSenderImpl::OnRequestFragmentsComplete(
    RCF::Future<RCF::ByteBuffer> ret,
    raft_group_id_t group_id,
    AsyncBatchRpcSenderImpl* sender) {
  auto e = ret.getAsyncException();
  if (e.get()) {
    printf("[DEBUG-CB] RequestFragments RPC Error: %s\n", e->getErrorString().c_str());
    std::lock_guard<std::mutex> lock(sender->stats_mtx_);
    sender->stats_.total_errors++;
  } else {
    RCF::ByteBuffer ret_buf = *ret;
    RequestFragmentsReply reply;
    Serializer::NewSerializer().Deserialize(&ret_buf, &reply);
    sender->HandleRequestFragmentsReply(reply);
  }
  sender->rpcs_in_flight_.fetch_sub(1);
}

AsyncBatchRpcSenderImpl::Stats AsyncBatchRpcSenderImpl::GetStats() const {
  std::lock_guard<std::mutex> lock(stats_mtx_);
  return stats_;
}

void AsyncBatchRpcSenderImpl::SetTransportHandler(raft_group_id_t group_id,
                                                 RaftTransportHandler* handler) {
  std::lock_guard<std::mutex> lock(handler_mtx_);
  transport_handlers_[group_id] = handler;
  printf("[ASYNC-SENDER] Registered transport handler for group=%u (total: %zu)\n",
         group_id, transport_handlers_.size());
}

void AsyncBatchRpcSenderImpl::RemoveTransportHandler(raft_group_id_t group_id) {
  std::lock_guard<std::mutex> lock(handler_mtx_);
  transport_handlers_.erase(group_id);
  printf("[ASYNC-SENDER] Removed transport handler for group=%u (remaining: %zu)\n",
         group_id, transport_handlers_.size());
}

// ================================================================
//  RaftTransportHandler interface implementation
// ================================================================

void AsyncBatchRpcSenderImpl::HandleRequestVoteReply(const RequestVoteReply& reply) {
  std::lock_guard<std::mutex> lock(handler_mtx_);
  auto it = transport_handlers_.find(reply.group_id);
  if (it != transport_handlers_.end() && it->second) {
    it->second->HandleRequestVoteReply(reply);
  }
}

void AsyncBatchRpcSenderImpl::HandleAppendEntriesReply(const AppendEntriesReply& reply) {
  std::lock_guard<std::mutex> lock(handler_mtx_);
  auto it = transport_handlers_.find(reply.group_id);
  if (it != transport_handlers_.end() && it->second) {
    it->second->HandleAppendEntriesReply(reply);
  }
}

void AsyncBatchRpcSenderImpl::HandleRequestFragmentsReply(const RequestFragmentsReply& reply) {
  std::lock_guard<std::mutex> lock(handler_mtx_);
  for (auto& [gid, handler] : transport_handlers_) {
    if (handler) {
      handler->HandleRequestFragmentsReply(reply);
    }
  }
}

// =======================================================================
//  RaftBatchTransportManager 实现
// =======================================================================

RaftBatchTransportManager::RaftBatchTransportManager() : self_id_(0) {}

RaftBatchTransportManager::~RaftBatchTransportManager() {
  Stop();
}

void RaftBatchTransportManager::Init(
    raft_node_id_t self_id,
    const std::unordered_map<raft_node_id_t, rpc::NetAddress>& peers) {
  self_id_ = self_id;

  BatchTransport::Config config;
  config.batch_size = 32;
  config.flush_interval = std::chrono::milliseconds(1);
  config.max_buffer_size = 1024;
  config.enable_local_delivery = true;

  transport_ = std::make_unique<BatchTransport>(config);
  transport_->SetSelfId(self_id);

  rpc_sender_ = std::make_unique<AsyncBatchRpcSenderImpl>();
  rpc_sender_->Init(self_id, peers);

  transport_->SetBatchRpcSender(rpc_sender_.get());

  printf("[ASYNC-MANAGER] Initialized for node %u with %zu peers (async mode)\n",
         self_id, peers.size());
}

void RaftBatchTransportManager::AttachToRaftState(RaftState* raft_state) {
  (void)raft_state;
  if (transport_) {
    printf("[ASYNC-MANAGER] Attached BatchTransport (group managed via PostInit)\n");
  }
}

void RaftBatchTransportManager::SetTransportHandler(raft_group_id_t group_id,
                                                  RaftTransportHandler* handler) {
  if (rpc_sender_) {
    rpc_sender_->SetTransportHandler(group_id, handler);
    printf("[ASYNC-MANAGER] Set transport handler for group=%u\n", group_id);
  }
}

void RaftBatchTransportManager::RemoveTransportHandler(raft_group_id_t group_id) {
  if (rpc_sender_) {
    rpc_sender_->RemoveTransportHandler(group_id);
    printf("[ASYNC-MANAGER] Removed transport handler for group=%u\n", group_id);
  }
}

void RaftBatchTransportManager::Start() {
  if (transport_) {
    transport_->Start();
  }
}

void RaftBatchTransportManager::Stop() {
  if (transport_) {
    transport_->Stop();
    auto stats = transport_->GetStats();
    auto sender_stats = rpc_sender_->GetStats();
    printf("[ASYNC-MANAGER] Final stats:\n");
    printf("  Transport: sent=%lu, batches=%lu, local=%lu, flushes=%lu\n",
           stats.total_sent_messages, stats.total_sent_batches,
           stats.total_local_deliveries, stats.total_queue_flushes);
    printf("  AsyncSender: batches=%lu, msgs=%lu, async=%lu, errors=%lu\n",
           sender_stats.total_batches_sent, sender_stats.total_messages_sent,
           sender_stats.total_async_sends, sender_stats.total_errors);
  }
}

}  // namespace raft
