// batch_transport.cc — 批量传输层实现

#include "batch_transport.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>

#include "RCF/ByteBuffer.hpp"
#include "serializer.h"
#include "util.h"

namespace raft {

// =======================================================================
//  RaftRpcMessage 实现
// =======================================================================

RaftRpcMessage RaftRpcMessage::CreateRequestVote(raft_group_id_t group_id,
                                                 raft_node_id_t from,
                                                 raft_node_id_t to,
                                                 const RequestVoteArgs& args) {
  RaftRpcMessage msg;
  msg.type = RaftRpcType::kRequestVote;
  msg.group_id = group_id;
  msg.from = from;
  msg.to = to;

  // 序列化 RequestVoteArgs
  Serializer serializer;
  size_t size = serializer.getSerializeSize(args);
  RCF::ByteBuffer buffer(size);
  serializer.Serialize(&args, &buffer);
  
  // 从 ByteBuffer 复制到 vector<char>
  msg.serialized_data.resize(size);
  std::copy(buffer.getPtr(), buffer.getPtr() + size, msg.serialized_data.begin());

  return msg;
}

RaftRpcMessage RaftRpcMessage::CreateAppendEntries(raft_group_id_t group_id,
                                                   raft_node_id_t from,
                                                   raft_node_id_t to,
                                                   const AppendEntriesArgs& args) {
  RaftRpcMessage msg;
  msg.type = RaftRpcType::kAppendEntries;
  msg.group_id = group_id;
  msg.from = from;
  msg.to = to;

  // 序列化 AppendEntriesArgs
  Serializer serializer;
  size_t size = serializer.getSerializeSize(args);
  RCF::ByteBuffer buffer(size);
  serializer.Serialize(&args, &buffer);
  
  // 从 ByteBuffer 复制到 vector<char>
  msg.serialized_data.resize(size);
  std::copy(buffer.getPtr(), buffer.getPtr() + size, msg.serialized_data.begin());

  return msg;
}

RaftRpcMessage RaftRpcMessage::CreateRequestFragments(raft_group_id_t group_id,
                                                      raft_node_id_t from,
                                                      raft_node_id_t to,
                                                      const RequestFragmentsArgs& args) {
  RaftRpcMessage msg;
  msg.type = RaftRpcType::kRequestFragments;
  msg.group_id = group_id;
  msg.from = from;
  msg.to = to;

  // 序列化 RequestFragmentsArgs
  Serializer serializer;
  size_t size = serializer.getSerializeSize(args);
  RCF::ByteBuffer buffer(size);
  serializer.Serialize(&args, &buffer);
  
  // 从 ByteBuffer 复制到 vector<char>
  msg.serialized_data.resize(size);
  std::copy(buffer.getPtr(), buffer.getPtr() + size, msg.serialized_data.begin());

  return msg;
}

void RaftRpcMessage::ToRequestVoteArgs(RequestVoteArgs* args) const {
  if (type != RaftRpcType::kRequestVote) {
    return;
  }
  Serializer serializer;
  // 构造一个可写的 ByteBuffer
  std::vector<char> writable_data = serialized_data;
  RCF::ByteBuffer buffer(writable_data);
  serializer.Deserialize(&buffer, args);
}

void RaftRpcMessage::ToAppendEntriesArgs(AppendEntriesArgs* args) const {
  if (type != RaftRpcType::kAppendEntries) {
    return;
  }
  Serializer serializer;
  std::vector<char> writable_data = serialized_data;
  RCF::ByteBuffer buffer(writable_data);
  serializer.Deserialize(&buffer, args);
}

void RaftRpcMessage::ToRequestFragmentsArgs(RequestFragmentsArgs* args) const {
  if (type != RaftRpcType::kRequestFragments) {
    return;
  }
  Serializer serializer;
  std::vector<char> writable_data = serialized_data;
  RCF::ByteBuffer buffer(writable_data);
  serializer.Deserialize(&buffer, args);
}

// =======================================================================
//  BatchTransport 实现
// =======================================================================

BatchTransport::BatchTransport(Config config)
    : config_(config), running_(false) {
  stats_ = {0, 0, 0, 0};
}

BatchTransport::~BatchTransport() {
  Stop();
}

void BatchTransport::Start() {
  if (running_.load()) {
    return;
  }

  running_.store(true);
  flush_thread_ = std::make_unique<std::thread>(&BatchTransport::FlushThreadMain, this);

  printf("[BATCH-TRANSPORT] Started with batch_size=%zu, flush_interval=%ldms\n",
         config_.batch_size,
         config_.flush_interval.count());
}

void BatchTransport::Stop() {
  if (!running_.load()) {
    return;
  }

  running_.store(false);
  flush_cv_.notify_all();

  if (flush_thread_ && flush_thread_->joinable()) {
    flush_thread_->join();
  }

  // 关闭前刷新所有队列
  {
    std::scoped_lock<std::mutex> lock(queues_mtx_);
    for (auto& [node_id, queue] : send_queues_) {
      std::lock_guard<std::mutex> node_lock(node_locks_[node_id]);
      if (!queue.buffer.empty()) {
        FlushQueueInternal(&queue, node_id);
      }
    }
  }

  printf("[BATCH-TRANSPORT] Stopped. Stats: sent=%lu, batches=%lu, local=%lu, flushes=%lu\n",
         stats_.total_sent_messages,
         stats_.total_sent_batches,
         stats_.total_local_deliveries,
         stats_.total_queue_flushes);
}

void BatchTransport::Enqueue(raft_node_id_t to, const RaftRpcMessage& msg) {
  // 优化：如果目标是本地节点，直接投递
  if (config_.enable_local_delivery && local_callback_ && to == self_id_) {
    local_callback_->DeliverToLocal(msg.group_id, msg);
    std::lock_guard<std::mutex> lock(stats_mtx_);
    stats_.total_local_deliveries++;
    return;
  }

  // 获取或创建发送队列
  SendQueue* queue_ptr = nullptr;
  {
    std::lock_guard<std::mutex> lock(queues_mtx_);
    auto it = send_queues_.find(to);
    if (it == send_queues_.end()) {
      send_queues_.emplace(to, SendQueue());
      it = send_queues_.find(to);
    }
    queue_ptr = &(it->second);
  }

  // 获取节点锁并操作队列
  std::lock_guard<std::mutex> node_lock(node_locks_[to]);

  SendQueue& queue = *queue_ptr;

  // CRITICAL: Immediately flush AppendEntries messages (no batching on critical path)
  if (msg.type == RaftRpcType::kAppendEntries) {
    std::vector<RaftRpcMessage> to_send;
    to_send.push_back(msg);
    if (rpc_sender_) {
      rpc_sender_->SendBatch(to, to_send);
    }
    {
      std::lock_guard<std::mutex> stats_lock(stats_mtx_);
      stats_.total_sent_messages++;
      stats_.total_sent_batches++;
    }
    return;  // Don't buffer AE messages — deliver immediately
  }

  // 防止 OOM：限制缓冲区大小
  if (queue.buffer.size() >= config_.max_buffer_size) {
    printf("[BATCH-TRANSPORT] WARNING: Buffer full for node %u, forcing flush\n", to);
    // 使用 swap 技术刷新
    std::vector<RaftRpcMessage> to_send;
    to_send.swap(queue.buffer);
    queue.last_flush_time = std::chrono::steady_clock::now();

    // 发送
    if (rpc_sender_) {
      rpc_sender_->SendBatch(to, to_send);
    }

    {
      std::lock_guard<std::mutex> stats_lock(stats_mtx_);
      stats_.total_sent_messages += to_send.size();
      stats_.total_sent_batches++;
      stats_.total_queue_flushes++;
    }
  }

  queue.buffer.push_back(msg);

  // 检查是否达到批量大小阈值
  if (queue.buffer.size() >= config_.batch_size) {
    std::vector<RaftRpcMessage> to_send;
    to_send.swap(queue.buffer);
    queue.last_flush_time = std::chrono::steady_clock::now();

    // 发送
    if (rpc_sender_) {
      rpc_sender_->SendBatch(to, to_send);
    }

    {
      std::lock_guard<std::mutex> stats_lock(stats_mtx_);
      stats_.total_sent_messages += to_send.size();
      stats_.total_sent_batches++;
      stats_.total_queue_flushes++;
    }
  }
}

void BatchTransport::EnqueueRequestVote(raft_group_id_t group_id,
                                         raft_node_id_t from,
                                         raft_node_id_t to,
                                         const RequestVoteArgs& args) {
  auto msg = RaftRpcMessage::CreateRequestVote(group_id, from, to, args);
  Enqueue(to, msg);
}

void BatchTransport::EnqueueAppendEntries(raft_group_id_t group_id,
                                           raft_node_id_t from,
                                           raft_node_id_t to,
                                           const AppendEntriesArgs& args) {
  auto msg = RaftRpcMessage::CreateAppendEntries(group_id, from, to, args);
  Enqueue(to, msg);
}

void BatchTransport::EnqueueRequestFragments(raft_group_id_t group_id,
                                              raft_node_id_t from,
                                              raft_node_id_t to,
                                              const RequestFragmentsArgs& args) {
  auto msg = RaftRpcMessage::CreateRequestFragments(group_id, from, to, args);
  Enqueue(to, msg);
}

void BatchTransport::Flush() {
  std::scoped_lock<std::mutex> lock(queues_mtx_);

  for (auto& [node_id, queue] : send_queues_) {
    std::lock_guard<std::mutex> node_lock(node_locks_[node_id]);
    if (!queue.buffer.empty()) {
      FlushQueueInternal(&queue, node_id);
    }
  }
}

void BatchTransport::FlushNode(raft_node_id_t node_id) {
  std::scoped_lock<std::mutex> lock(queues_mtx_);

  auto it = send_queues_.find(node_id);
  if (it == send_queues_.end()) {
    return;
  }

  std::lock_guard<std::mutex> node_lock(node_locks_[node_id]);
  SendQueue& queue = it->second;

  if (!queue.buffer.empty()) {
    FlushQueueInternal(&queue, node_id);
  }
}

void BatchTransport::FlushQueueInternal(SendQueue* queue, raft_node_id_t node_id) {
  if (queue->buffer.empty()) {
    return;
  }

  // 使用 swap 技术避免长时间持锁
  std::vector<RaftRpcMessage> messages_to_send;
  messages_to_send.swap(queue->buffer);

  // 更新时间戳
  queue->last_flush_time = std::chrono::steady_clock::now();

  // 更新统计
  {
    std::lock_guard<std::mutex> lock(stats_mtx_);
    stats_.total_sent_messages += messages_to_send.size();
    stats_.total_sent_batches++;
    stats_.total_queue_flushes++;
  }

  // 通过 RPC sender 发送
  if (rpc_sender_) {
    rpc_sender_->SendBatch(node_id, messages_to_send);
  } else {
    printf("[BATCH-TRANSPORT] WARNING: No RPC sender configured, dropping %zu messages\n",
           messages_to_send.size());
  }
}

void BatchTransport::FlushThreadMain() {
  printf("[BATCH-TRANSPORT] Flush thread started\n");

  while (running_.load()) {
    std::this_thread::sleep_for(config_.flush_interval);

    if (!running_.load()) {
      break;
    }

    auto now = std::chrono::steady_clock::now();
    std::scoped_lock<std::mutex> lock(queues_mtx_);

    for (auto& [node_id, queue] : send_queues_) {
      std::lock_guard<std::mutex> node_lock(node_locks_[node_id]);

      if (queue.buffer.empty()) {
        continue;
      }

      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - queue.last_flush_time);

      // 检查是否超时需要 flush
      if (elapsed >= config_.flush_interval) {
        FlushQueueInternal(&queue, node_id);
      }
    }
  }

  printf("[BATCH-TRANSPORT] Flush thread exited\n");
}

BatchTransport::Stats BatchTransport::GetStats() const {
  std::lock_guard<std::mutex> lock(stats_mtx_);
  return stats_;
}

}  // namespace raft
