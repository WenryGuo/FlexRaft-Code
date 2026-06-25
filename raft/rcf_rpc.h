#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "RCF/ByteBuffer.hpp"
#include "RCF/ClientStub.hpp"
#include "RCF/Future.hpp"
#include "RCF/InitDeinit.hpp"
#include "RCF/RcfServer.hpp"
#include "raft.h"
#include "raft_struct.h"
#include "raft_type.h"
#include "rpc.h"
#include "serializer.h"

namespace raft {
namespace rpc {

namespace config {
// Each RPC call size must not exceed 512MB
static constexpr size_t kMaxMessageLength = 512 * 1024 * 1024;

// RequestVote 超时：必须远小于 Raft 选举超时（1500ms）
// 100ms 允许一次网络重传后立即失败，Tick() 会触发重试
static constexpr size_t kRequestVoteTimeout = 100;   // 100ms

// AppendEntries 超时：可以稍长，因为 leader 会在下一次 heartbeat 重试
static constexpr size_t kAppendEntriesTimeout = 500; // 500ms

// 保持向后兼容（指向 RequestVote 超时）
static constexpr size_t kRPCTimeout = kRequestVoteTimeout;
};  // namespace config

RCF_BEGIN(I_RaftRPCService, "I_RaftRPCService")
RCF_METHOD_R1(RCF::ByteBuffer, RequestVote, const RCF::ByteBuffer &)
RCF_METHOD_R1(RCF::ByteBuffer, AppendEntries, const RCF::ByteBuffer &)
RCF_METHOD_R1(RCF::ByteBuffer, RequestFragments, const RCF::ByteBuffer &)
RCF_END(I_RaftRPCService)

RCF_BEGIN(I_GroupNotificationService, "I_GroupNotificationService")
RCF_METHOD_R1(RCF::ByteBuffer, GroupNotification, const RCF::ByteBuffer &)
RCF_END(I_GroupNotificationService)

// Some statistics about one rpc call arguments
struct RPCArgStats {
  size_t arg_size;
  util::TimePoint start_time;
};

// Some statistics about one rpc call
struct RPCStats {
  size_t arg_size;
  size_t resp_size;
  int64_t total_time;
  int64_t transfer_time;
  int64_t process_time;

  std::string ToString() const {
    char buf[512];
    sprintf(buf,
            "[Total Time = %ld us][Process Time = %ld us][Transfer Time = "
            "%ld us][Args "
            "size=%luB][Reply size=%luB]",
            total_time, process_time, total_time - process_time, arg_size, resp_size);
    return std::string(buf);
  }
};

struct RPCStatsRecorder {
 public:
  RPCStatsRecorder() : history_() {
    history_.reserve(10000);
    // Add a default history result
    history_.push_back({0, 0, 0, 0, 0});
  }

  // Write the results to a specified file
  void Dump(const std::string &dst);
  void Dump(std::ofstream &of);

  void Add(const RPCStats &stat) { history_.push_back(stat); }

  std::vector<RPCStats> history_;
};

class RaftRPCService {
 public:
  RaftRPCService() = default;
  void SetRaftState(RaftState *raft) { raft_ = raft; }
  RCF::ByteBuffer RequestVote(const RCF::ByteBuffer &arg_buf);
  RCF::ByteBuffer AppendEntries(const RCF::ByteBuffer &arg_buf);
  RCF::ByteBuffer RequestFragments(const RCF::ByteBuffer &arg_buf);

 private:
  RaftState *raft_;
};

// An implementation of RpcClient interface using RCF (Remote Call Framework)
// Uses a connection pool to support concurrent calls from multiple RaftState
// instances on the same physical node. RCF RcfClient does NOT support concurrent
// calls on a single instance.
class RCFRpcClient final : public RpcClient {
  // Pool size per target: 4 concurrent calls max
  // In a 7-node cluster, each node has 7 Raft groups, and each group sends
  // up to 6 async RequestVote calls during election. With all groups starting
  // elections simultaneously, we need enough connections to avoid contention.
  static constexpr size_t kPoolSize = 4;

 public:
  // Construction
  RCFRpcClient(const NetAddress &target_address, raft_node_id_t id);

  RCFRpcClient &operator=(const RCFRpcClient &) = delete;
  RCFRpcClient(const RCFRpcClient &) = delete;

 public:
  void SetRaftState(RaftState *raft) { raft_ = raft; }
  void Dump(const std::string &filename) { recorder_.Dump(filename); }
  void Dump(std::ofstream &of) { recorder_.Dump(of); }

 public:
  void Init() override;
  void sendMessage(const RequestVoteArgs &args) override;
  void sendMessage(const AppendEntriesArgs &args) override;
  void sendMessage(const RequestFragmentsArgs &args) override;
  void sendAsyncMessage(const AppendEntriesArgs &args) override;
  void sendAsyncMessage(const RequestVoteArgs &args) override;

  // Lightweight async callbacks (called from lambdas following batch_rpc_sender.cc pattern)
  void OnRequestVoteComplete(RCF::Future<RCF::ByteBuffer> ret, raft_group_id_t group_id);
  void OnAppendEntriesComplete(RCF::Future<RCF::ByteBuffer> ret, raft_group_id_t group_id);

  void setState(void *state) override { raft_ = reinterpret_cast<RaftState *>(state); }
  // Store weak_ptr to raft_state_owner_ so async callbacks can safely extend lifetime
  void setRaftStateOwnerPtr(std::weak_ptr<void> p) { raft_state_owner_wp_ = p; }

  // GroupNotification RPC (for Multi-Raft group setup)
  void sendGroupNotification(const GroupNotificationArgs &args);

  void setMaxTransportLength(RcfClient<I_RaftRPCService> *ptr) {
    ptr->getClientStub().getTransport().setMaxOutgoingMessageLength(config::kMaxMessageLength);
    ptr->getClientStub().getTransport().setMaxIncomingMessageLength(config::kMaxMessageLength);
  }

  void stop() override {
    fprintf(stderr, "[RPC-CLIENT-N%u] STOP called, clearing connection pool\n", id_);
    fflush(stderr);
    stopped_ = true;
    destroyed_.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(init_mutex_);
    pool_valid_.store(false, std::memory_order_release);
    for (size_t i = 0; i < kPoolSize; ++i) {
      pool_[i].reset();
    }
    fprintf(stderr, "[RPC-CLIENT-N%u] STOP complete\n", id_);
    fflush(stderr);
  }
  void recover() override {
    fprintf(stderr, "[RPC-CLIENT-N%u] RECOVER called, clearing connection pool\n", id_);
    fflush(stderr);
    stopped_ = false;
    std::lock_guard<std::mutex> lock(init_mutex_);
    pool_valid_.store(false, std::memory_order_release);
    for (size_t i = 0; i < kPoolSize; ++i) {
      pool_[i].reset();
    }
    fprintf(stderr, "[RPC-CLIENT-N%u] RECOVER complete\n", id_);
    fflush(stderr);
  }

 private:
  // Static callbacks (don't need client instance, but signature must match)
  static void onRequestVoteComplete(RCF::Future<RCF::ByteBuffer> ret,
                                    RaftState *raft, raft_node_id_t peer);
  static void onAppendEntriesComplete(RCF::Future<RCF::ByteBuffer> ret,
                                      RaftState *raft, raft_node_id_t peer, RPCArgStats arg_stats,
                                      RPCStatsRecorder *recorder);
  static void onAppendEntriesCompleteRecordTimer(RCF::Future<RCF::ByteBuffer> ret,
                                                 RaftState *raft, raft_node_id_t peer,
                                                 util::AppendEntriesRPCPerfCounter counter);
  static void onRequestFragmentsComplete(RCF::Future<RCF::ByteBuffer> ret,
                                         RaftState *raft, raft_node_id_t peer);

  // Get a pooled connection and its associated mutex (round-robin dispatch)
  RcfClient<I_RaftRPCService> *getPooledConn(std::mutex *&out_mutex);

 private:
  RaftState *raft_ = nullptr;
  RCF::RcfInit rcf_init_;
  NetAddress target_address_;
  bool stopped_ = false;
  std::atomic<bool> destroyed_{false};
  raft_node_id_t id_ = 0;
  // weak_ptr to raft_state_owner_ (set via setRaftStateOwnerPtr).
  // Callbacks lock this to safely extend RaftState lifetime.
  std::weak_ptr<void> raft_state_owner_wp_;

  // Connection pool: created lazily on first RPC, cleared on stop()/recover()
  // Each pooled connection is protected by its own mutex for thread-safe dispatch.
  std::array<std::unique_ptr<RcfClient<I_RaftRPCService>>, kPoolSize> pool_;
  std::array<std::mutex, kPoolSize> pool_mutexes_;
  std::mutex init_mutex_;
  std::atomic<bool> pool_valid_{false};
  std::atomic<size_t> pool_idx_{0};

  RPCStatsRecorder recorder_;
};

class RCFRpcServer final : public RpcServer {
 public:
  RCFRpcServer(const NetAddress &my_address);

 public:
  void Start() override;
  void Stop() override;
  void dealWithMessage(const RequestVoteArgs &reply) override;
  void setState(void *state) override {
    service_.SetRaftState(reinterpret_cast<RaftState *>(state));
  }

 private:
  RCF::RcfInit rcf_init_;
  RCF::RcfServer server_;
  RaftRPCService service_;
};

// =======================================================================
//  Inline implementations — connection pool + all RPC send methods
// =======================================================================
inline RCFRpcClient::RCFRpcClient(const NetAddress &target_address, raft_node_id_t id)
    : target_address_(target_address), id_(id), stopped_(false) {}

inline void RCFRpcClient::Init() {}

inline RcfClient<I_RaftRPCService> *RCFRpcClient::getPooledConn(std::mutex *&out_mutex) {
  // Ensure pool is initialized (thread-safe double-check)
  if (!pool_valid_.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> init_lock(init_mutex_);
    if (!pool_valid_.load(std::memory_order_acquire)) {
      for (size_t i = 0; i < kPoolSize; ++i) {
        pool_[i] = std::make_unique<RcfClient<I_RaftRPCService>>(
            RCF::TcpEndpoint(target_address_.ip, target_address_.port));
        setMaxTransportLength(pool_[i].get());
      }
      pool_valid_.store(true, std::memory_order_release);
      printf("[RPC-CLIENT-N%u] Connection pool (%zu conns) to %s:%d\n",
             id_, kPoolSize, target_address_.ip.c_str(), target_address_.port);
    }
  }

  // Round-robin dispatch: each connection has its own mutex
  size_t idx = pool_idx_.fetch_add(1) % kPoolSize;
  out_mutex = &pool_mutexes_[idx];

  // [DEBUG] Verify pool entry is valid
  if (!pool_[idx]) {
    fprintf(stderr, "[RPC-CLIENT-N%u] WARNING: pool_[%zu] is nullptr!\n", id_, idx);
    fflush(stderr);
    return nullptr;
  }

  return pool_[idx].get();
}

inline void RCFRpcClient::sendMessage(const RequestVoteArgs &args) {
  if (stopped_) return;
  std::mutex *mtx = nullptr;
  auto *client = getPooledConn(mtx);
  if (!client) return;
  if (mtx) mtx->lock();

  // 设置 RequestVote 专用超时（100ms，远小于选举超时1500ms）
  client->getClientStubPtr()->setRemoteCallTimeoutMs(config::kRequestVoteTimeout);
  
  auto serializer = Serializer::NewSerializer();
  RCF::ByteBuffer arg_buf(serializer.getSerializeSize(args));
  serializer.Serialize(&args, &arg_buf);

  try {
    RCF::ByteBuffer ret_buf = client->RequestVote(arg_buf);
    RequestVoteReply reply;
    auto ret_serializer = Serializer::NewSerializer();
    ret_serializer.Deserialize(&ret_buf, &reply);
    // [DEBUG] 添加日志确认 reply 被正确接收和处理
    printf("[RPC-CLIENT-N%d] Received RequestVoteReply: granted=%d term=%d reply_id=%d this=%p raft_=%p\n",
           id_, reply.vote_granted, reply.term, reply.reply_id, (void*)this, (void*)raft_);
    fflush(stdout);
    if (raft_ != nullptr) {
      printf("[RPC-CLIENT-N%d] Calling raft_->Process() for RequestVoteReply...\n", id_);
      fflush(stdout);
      raft_->Process(&reply);
      printf("[RPC-CLIENT-N%d] raft_->Process() returned\n", id_);
      fflush(stdout);
    } else {
      printf("[RPC-CLIENT-N%d] WARNING: raft_ is NULL! Cannot process reply.\n", id_);
      fflush(stdout);
    }
  } catch (const RCF::Exception &e) {
    printf("[RPC-CLIENT] RequestVote SYNC-ERROR: target=%s:%d error=%s\n",
           target_address_.ip.c_str(), target_address_.port, e.what());
  }
  if (mtx) mtx->unlock();
}

inline void RCFRpcClient::sendMessage(const AppendEntriesArgs &args) {
  if (stopped_) return;
  std::mutex *mtx = nullptr;
  auto *client = getPooledConn(mtx);
  if (!client) return;
  if (mtx) mtx->lock();

  // 使用 AppendEntries 专用超时（500ms）
  client->getClientStubPtr()->setRemoteCallTimeoutMs(config::kAppendEntriesTimeout);
  auto serializer = Serializer::NewSerializer();
  RCF::ByteBuffer arg_buf(serializer.getSerializeSize(args));
  serializer.Serialize(&args, &arg_buf);

  try {
    RCF::ByteBuffer ret_buf = client->AppendEntries(arg_buf);
    AppendEntriesReply reply;
    auto ret_serializer = Serializer::NewSerializer();
    ret_serializer.Deserialize(&ret_buf, &reply);
    if (raft_ != nullptr) {
      raft_->Process(&reply);
    }
    // NOTE: Success log removed to prevent log flooding during normal operation.
    // Uncomment for verbose RPC debugging:
    // printf("[RPC-CLIENT] AppendEntries SUCCESS: target=%s:%d group_id=%u leader=%u entries=%zu\n",
    //        target_address_.ip.c_str(), target_address_.port, args.group_id, args.leader_id, args.entries.size());
  } catch (const RCF::Exception &e) {
    printf("[RPC-CLIENT] AppendEntries SYNC-ERROR: target=%s:%d group_id=%u error=%s\n",
           target_address_.ip.c_str(), target_address_.port, args.group_id, e.what());
  }
  if (mtx) mtx->unlock();
}

inline void RCFRpcClient::sendMessage(const RequestFragmentsArgs &args) {
  if (stopped_) return;
  std::mutex *mtx = nullptr;
  auto *client = getPooledConn(mtx);
  if (!client) return;
  if (mtx) mtx->lock();

  // 使用 RequestVote 超时（100ms）
  client->getClientStubPtr()->setRemoteCallTimeoutMs(config::kRequestVoteTimeout);
  
  auto serializer = Serializer::NewSerializer();
  RCF::ByteBuffer arg_buf(serializer.getSerializeSize(args));
  serializer.Serialize(&args, &arg_buf);

  try {
    RCF::ByteBuffer ret_buf = client->RequestFragments(arg_buf);
    RequestFragmentsReply reply;
    auto ret_serializer = Serializer::NewSerializer();
    ret_serializer.Deserialize(&ret_buf, &reply);
    if (raft_ != nullptr) {
      raft_->Process(&reply);
    }
  } catch (const RCF::Exception &e) {
    printf("[RPC-CLIENT] RequestFragments SYNC-ERROR: target=%s:%d error=%s\n",
           target_address_.ip.c_str(), target_address_.port, e.what());
  }
  if (mtx) mtx->unlock();
}

// =======================================================================
//  Async callback implementations (called from lightweight lambdas)
// =======================================================================

// Async callback for RequestVote RPC completion.
// Follows the same pattern as batch_rpc_sender.cc: the Future is captured
// by the lambda and passed to this function when RCF invokes the callback.
inline void RCFRpcClient::OnRequestVoteComplete(
    RCF::Future<RCF::ByteBuffer> ret,
    raft_group_id_t group_id) {
  auto e = ret.getAsyncException();
  if (e.get()) {
    printf("[RPC-CLIENT-N%u] RequestVote ASYNC-ERROR: group_id=%u error=%s\n",
           id_, group_id, e->getErrorString().c_str());
  } else {
    RCF::ByteBuffer ret_buf = ret;
    RequestVoteReply reply;
    Serializer::NewSerializer().Deserialize(&ret_buf, &reply);
    if (raft_ != nullptr) {
      raft_->Process(&reply);
    }
  }
}

// Async callback for AppendEntries RPC completion.
inline void RCFRpcClient::OnAppendEntriesComplete(
    RCF::Future<RCF::ByteBuffer> ret,
    raft_group_id_t group_id) {
  auto e = ret.getAsyncException();
  if (e.get()) {
    printf("[RPC-CLIENT-N%u] AppendEntries ASYNC-ERROR: group_id=%u error=%s\n",
           id_, group_id, e->getErrorString().c_str());
  } else {
    RCF::ByteBuffer ret_buf = ret;
    AppendEntriesReply reply;
    Serializer::NewSerializer().Deserialize(&ret_buf, &reply);
    printf("[RPC-CB] group=%d from=%d success=%d\n",
           group_id, reply.reply_id, reply.success);
    fflush(stdout);
    if (raft_ != nullptr) {
      raft_->Process(&reply);
    }
  }
}

// =======================================================================
//  Async RPC send methods — lightweight lambda + member function callback
// =======================================================================
// Follows the same pattern as batch_rpc_sender.cc:
//   1. Create Future as local variable (BEFORE lambda)
//   2. Create lightweight lambda that only captures Future and calls member function
//   3. RCF copies the lambda into std::function<void()>, so keep it small
//   4. Assign async return value to the Future variable
// =======================================================================

// Async RequestVote — lightweight lambda pattern (same as batch_rpc_sender.cc)
inline void RCFRpcClient::sendAsyncMessage(const RequestVoteArgs &args) {
  if (stopped_) return;
  std::mutex *mtx = nullptr;
  auto *client = getPooledConn(mtx);
  if (!client) return;
  if (mtx) mtx->lock();

  client->getClientStubPtr()->setRemoteCallTimeoutMs(config::kRequestVoteTimeout);
  auto serializer = Serializer::NewSerializer();
  RCF::ByteBuffer arg_buf(serializer.getSerializeSize(args));
  serializer.Serialize(&args, &arg_buf);
  if (mtx) mtx->unlock();

  // Create Future BEFORE lambda (critical: lambda captures the Future by value)
  RCF::Future<RCF::ByteBuffer> ret;
  // Lightweight lambda: use [=] to capture all needed variables BY VALUE.
  // This is CRITICAL: capturing by reference (&args, &ret) would create dangling
  // pointers since these are stack variables that go out of scope when this function returns.
  // The [=] capture makes copies that remain valid when RCF invokes the callback later.
  auto cb = [=]() {
    this->OnRequestVoteComplete(ret, args.group_id);
  };
  // Assign async return value to ret — RCF will fill in the state when complete
  ret = client->RequestVote(RCF::AsyncTwoway(cb), arg_buf);
}

// Async AppendEntries — lightweight lambda pattern (same as batch_rpc_sender.cc)
inline void RCFRpcClient::sendAsyncMessage(const AppendEntriesArgs &args) {
  if (stopped_) return;
  std::mutex *mtx = nullptr;
  auto *client = getPooledConn(mtx);
  if (!client) return;
  if (mtx) mtx->lock();

  client->getClientStubPtr()->setRemoteCallTimeoutMs(config::kAppendEntriesTimeout);
  auto serializer = Serializer::NewSerializer();
  RCF::ByteBuffer arg_buf(serializer.getSerializeSize(args));
  serializer.Serialize(&args, &arg_buf);
  if (mtx) mtx->unlock();

  // Create Future BEFORE lambda (critical: lambda captures the Future by value)
  RCF::Future<RCF::ByteBuffer> ret;
  // Use [=] to capture all needed variables BY VALUE.
  // CRITICAL: capturing by reference would create dangling pointers.
  auto cb = [=]() {
    this->OnAppendEntriesComplete(ret, args.group_id);
  };
  // Assign async return value to ret — RCF will fill in the state when complete
  ret = client->AppendEntries(RCF::AsyncTwoway(cb), arg_buf);
}

}  // namespace rpc
}  // namespace raft
