#pragma once
// batch_transport.h — 批量传输层
//
// 核心设计：
// 1. 不允许 Raft 实例直接发送 RPC，必须通过 transport 层
// 2. 在 transport 层维护发送队列，按目标节点分组
// 3. 支持批量 flush（基于大小阈值和时间间隔）
// 4. 使用 swap 技术避免长时间持锁

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// 前向声明，避免循环依赖
namespace raft {
struct RequestVoteArgs;
struct AppendEntriesArgs;
struct RequestFragmentsArgs;
struct RequestVoteReply;
struct AppendEntriesReply;
struct RequestFragmentsReply;
}

#include "raft_type.h"

namespace raft {

// 消息类型枚举
enum class RaftRpcType : uint8_t {
  kRequestVote = 0,
  kAppendEntries = 1,
  kRequestFragments = 2
};

// 统一的 RPC 消息包装结构
struct RaftRpcMessage {
  RaftRpcType type;
  raft_group_id_t group_id;
  raft_node_id_t from;
  raft_node_id_t to;

  // 序列化后的数据（用于批量传输）
  std::vector<char> serialized_data;

  // 便捷构造函数
  static RaftRpcMessage CreateRequestVote(raft_group_id_t group_id,
                                          raft_node_id_t from,
                                          raft_node_id_t to,
                                          const RequestVoteArgs& args);

  static RaftRpcMessage CreateAppendEntries(raft_group_id_t group_id,
                                            raft_node_id_t from,
                                            raft_node_id_t to,
                                            const AppendEntriesArgs& args);

  static RaftRpcMessage CreateRequestFragments(raft_group_id_t group_id,
                                               raft_node_id_t from,
                                               raft_node_id_t to,
                                               const RequestFragmentsArgs& args);

  // 反序列化获取原始消息
  void ToRequestVoteArgs(RequestVoteArgs* args) const;
  void ToAppendEntriesArgs(AppendEntriesArgs* args) const;
  void ToRequestFragmentsArgs(RequestFragmentsArgs* args) const;
};

// 批量 RPC 请求结构
struct BatchRpcRequest {
  raft_node_id_t from;
  raft_node_id_t to;
  std::vector<RaftRpcMessage> messages;
};

// 发送队列
struct SendQueue {
  std::vector<RaftRpcMessage> buffer;
  std::chrono::steady_clock::time_point last_flush_time;

  SendQueue() { last_flush_time = std::chrono::steady_clock::now(); }
};

// RPC 发送回调接口
// BatchTransport 通过此接口将批量消息发送到网络
class BatchRpcSender {
 public:
  virtual ~BatchRpcSender() = default;

  // 发送批量请求到指定节点
  virtual void SendBatch(raft_node_id_t to, const std::vector<RaftRpcMessage>& messages) = 0;
};

// 本地投递回调接口（用于本地消息快速投递）
class LocalDeliveryCallback {
 public:
  virtual ~LocalDeliveryCallback() = default;
  virtual void DeliverToLocal(raft_group_id_t group_id, const RaftRpcMessage& msg) = 0;
};

// =======================================================================
//  BatchTransport — 批量传输层
//
//  使用方式：
//  1. 创建 BatchTransport 实例并 Start()
//  2. 配置 self_id 和 sender（RPC 客户端）
//  3. 通过 Enqueue* 系列接口投递消息
//  4. Transport 自动批量 flush 到目标节点
// =======================================================================
class BatchTransport {
 public:
  // 配置
  struct Config {
    size_t batch_size = 16;                             // 触发 flush 的批量大小
    std::chrono::milliseconds flush_interval{2};         // 定时 flush 间隔（ms）
    size_t max_buffer_size = 256;                        // 单个节点最大缓冲数量
    bool enable_local_delivery = true;                  // 启用本地投递优化
  };

 public:
  explicit BatchTransport(Config config);
  ~BatchTransport();

  // 不可复制
  BatchTransport(const BatchTransport&) = delete;
  BatchTransport& operator=(const BatchTransport&) = delete;

  // 初始化 transport，启动 flush 线程
  void Start();

  // 停止 transport
  void Stop();

  // 设置本地节点 ID（用于本地投递优化）
  void SetSelfId(raft_node_id_t self_id) { self_id_ = self_id; }

  // 设置 RPC 发送器
  void SetBatchRpcSender(BatchRpcSender* sender) { rpc_sender_ = sender; }

  // 设置本地投递回调
  void SetLocalDeliveryCallback(LocalDeliveryCallback* callback) {
    local_callback_ = callback;
  }

  // =====================================================================
  // 投递接口（线程安全）
  // =====================================================================

  // 直接投递消息
  void Enqueue(raft_node_id_t to, const RaftRpcMessage& msg);

  // 便捷重载：投递 RequestVote
  void EnqueueRequestVote(raft_group_id_t group_id,
                         raft_node_id_t from,
                         raft_node_id_t to,
                         const RequestVoteArgs& args);

  // 便捷重载：投递 AppendEntries
  void EnqueueAppendEntries(raft_group_id_t group_id,
                            raft_node_id_t from,
                            raft_node_id_t to,
                            const AppendEntriesArgs& args);

  // 便捷重载：投递 RequestFragments
  void EnqueueRequestFragments(raft_group_id_t group_id,
                               raft_node_id_t from,
                               raft_node_id_t to,
                               const RequestFragmentsArgs& args);

  // =====================================================================
  // Flush 接口
  // =====================================================================

  // 手动触发 flush（刷新所有节点）
  void Flush();

  // 强制刷新指定节点的消息
  void FlushNode(raft_node_id_t node_id);

  // =====================================================================
  // 统计信息
  // =====================================================================

  struct Stats {
    uint64_t total_sent_messages;
    uint64_t total_sent_batches;
    uint64_t total_local_deliveries;
    uint64_t total_queue_flushes;
  };
  Stats GetStats() const;

 private:
  // 内部 flush 实现（使用 swap 技术）
  void FlushQueueInternal(SendQueue* queue, raft_node_id_t node_id);

  // flush 线程主函数
  void FlushThreadMain();

 private:
  Config config_;
  std::unordered_map<raft_node_id_t, SendQueue> send_queues_;
  mutable std::mutex queues_mtx_;

  // 每个节点的锁，防止并发 flush 同一节点
  std::unordered_map<raft_node_id_t, std::mutex> node_locks_;

  // flush 线程
  std::unique_ptr<std::thread> flush_thread_;
  std::atomic<bool> running_;
  std::condition_variable flush_cv_;
  mutable std::mutex stats_mtx_;
  Stats stats_;

  // 依赖组件
  BatchRpcSender* rpc_sender_ = nullptr;
  LocalDeliveryCallback* local_callback_ = nullptr;
  raft_node_id_t self_id_ = 0;
};

// =======================================================================
//  RaftState 需要实现的回调接口
// =======================================================================
class RaftTransportHandler {
 public:
  virtual ~RaftTransportHandler() = default;
  virtual void HandleRequestVoteReply(const RequestVoteReply& reply) = 0;
  virtual void HandleAppendEntriesReply(const AppendEntriesReply& reply) = 0;
  virtual void HandleRequestFragmentsReply(const RequestFragmentsReply& reply) = 0;
};

}  // namespace raft
