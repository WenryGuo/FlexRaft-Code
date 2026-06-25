#pragma once
// batch_rpc_sender.h — Per-Node 异步连接池
//
// 核心设计：
//   1. 每个目标节点维护一个独立的 RCF RpcClient 连接池 (4个连接)
//   2. 发送使用 RCF::AsyncTwoway 异步调用，不阻塞调用线程
//   3. 每个节点内部用独立 mutex 保护并发访问

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "batch_transport.h"
#include "rcf_rpc.h"
#include "rpc.h"

namespace raft {

// =======================================================================
//  BatchRpcMessage — 批量 RPC 消息结构
// =======================================================================
struct BatchRpcMessage {
  RaftRpcType type;
  raft_group_id_t group_id;
  std::vector<char> serialized_data;
};

// =======================================================================
//  AsyncBatchRpcSenderImpl — 异步批量 RPC 发送器
//
//  每个目标节点一个连接池 (4个连接)，轮询分发异步 RPC 调用
//  完全异步化，不阻塞调用线程，不存在全局锁竞争
// =======================================================================
class AsyncBatchRpcSenderImpl : public BatchRpcSender, public RaftTransportHandler {
 public:
  AsyncBatchRpcSenderImpl();
  ~AsyncBatchRpcSenderImpl();

  void Init(raft_node_id_t self_id,
            const std::unordered_map<raft_node_id_t, rpc::NetAddress>& peers);

  void SendBatch(raft_node_id_t to,
                 const std::vector<RaftRpcMessage>& messages) override;

  struct Stats {
    uint64_t total_batches_sent = 0;
    uint64_t total_messages_sent = 0;
    uint64_t total_async_sends = 0;
    uint64_t total_errors = 0;
  };
  Stats GetStats() const;

  void SetTransportHandler(raft_group_id_t group_id, RaftTransportHandler* handler);
  void RemoveTransportHandler(raft_group_id_t group_id);

 private:
  static constexpr size_t kPoolSize = 4;

  // 连接池中单个连接及其锁
  struct PooledConn {
    std::unique_ptr<rpc::RcfClient<rpc::I_RaftRPCService>> client;
    std::mutex mtx;
  };

  // 每目标节点的连接池
  struct NodePool {
    std::array<PooledConn, kPoolSize> conns;
    std::atomic<size_t> pool_idx{0};
  };

  // 获取目标节点的连接池中的下一个连接（轮询）
  PooledConn* GetPooledConn(raft_node_id_t to);

  // 内部：发送单条 RPC
  void DispatchRpcAsync(PooledConn* conn, const BatchRpcMessage& msg);

  // 异步回调：处理 RequestVote 响应
  static void OnRequestVoteComplete(RCF::Future<RCF::ByteBuffer> ret,
                                    raft_group_id_t group_id,
                                    AsyncBatchRpcSenderImpl* sender);

  // 异步回调：处理 AppendEntries 响应
  static void OnAppendEntriesComplete(RCF::Future<RCF::ByteBuffer> ret,
                                     raft_group_id_t group_id,
                                     AsyncBatchRpcSenderImpl* sender);

  // 异步回调：处理 RequestFragments 响应
  static void OnRequestFragmentsComplete(RCF::Future<RCF::ByteBuffer> ret,
                                         raft_group_id_t group_id,
                                         AsyncBatchRpcSenderImpl* sender);

  // ================================================================
  //  RaftTransportHandler 接口实现：将 RPC 响应路由到正确的 RaftState
  // ================================================================
  void HandleRequestVoteReply(const RequestVoteReply& reply) override;
  void HandleAppendEntriesReply(const AppendEntriesReply& reply) override;
  void HandleRequestFragmentsReply(const RequestFragmentsReply& reply) override;

 private:
  raft_node_id_t self_id_;

  // Per-node address map（只读，初始化后不变）
  std::unordered_map<raft_node_id_t, rpc::NetAddress> addr_map_;

  // Per-node 连接池: node_id -> 4连接池
  // 使用 unique_ptr<NodePool> 避免 unordered_map::operator[] 对非可移动类型的限制
  mutable std::mutex pools_mutex_;
  std::unordered_map<raft_node_id_t, std::unique_ptr<NodePool>> node_pools_;

  mutable std::mutex stats_mtx_;
  Stats stats_;

  // Per-group transport handler map
  std::unordered_map<raft_group_id_t, RaftTransportHandler*> transport_handlers_;
  mutable std::mutex handler_mtx_;

  // 全局 round-robin 索引（跨所有节点共享）
  mutable std::atomic<size_t> global_pool_idx_{0};

  // In-flight RPC 计数器（用于流控）
  std::atomic<uint64_t> rpcs_in_flight_{0};
  mutable std::mutex inflight_mtx_;
  std::condition_variable inflight_cv_;

  // 内部方法
  size_t SendBatchAndTrack(raft_node_id_t to, const std::vector<RaftRpcMessage>& messages);
  void WaitUntilInflightBelow(size_t target);
  void DecrementInflightAndNotify();
};

// =======================================================================
//  RaftBatchTransportManager — 管理 BatchTransport 的生命周期
// =======================================================================
class RaftBatchTransportManager {
 public:
  RaftBatchTransportManager();
  ~RaftBatchTransportManager();

  void Init(raft_node_id_t self_id,
            const std::unordered_map<raft_node_id_t, rpc::NetAddress>& peers);
  void AttachToRaftState(RaftState* raft_state);
  BatchTransport* GetBatchTransport() const { return transport_.get(); }
  void SetTransportHandler(raft_group_id_t group_id, RaftTransportHandler* handler);
  void RemoveTransportHandler(raft_group_id_t group_id);
  void Start();
  void Stop();

 private:
  std::unique_ptr<BatchTransport> transport_;
  std::unique_ptr<AsyncBatchRpcSenderImpl> rpc_sender_;
  raft_node_id_t self_id_;
};

}  // namespace raft
