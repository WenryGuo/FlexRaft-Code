// raft_unified_rpc_server.cc — 统一的 Raft RPC 服务器实现

#include "raft_unified_rpc_server.h"

#include <cstdio>
#include <cstring>

#include "raft_struct.h"
#include "serializer.h"

namespace raft {
namespace rpc {

// =======================================================================
//  RaftUnifiedRPCService 实现
// =======================================================================

void RaftUnifiedRPCService::RegisterRaftState(raft_group_id_t group_id, RaftState* raft_state) {
  std::unique_lock<std::shared_mutex> lock(mu_);
  raft_states_[group_id] = raft_state;
  printf("[RAFT-UNIFIED-SVC] Registered RaftState for group=%u (total: %zu)\n",
         group_id, raft_states_.size());
}

void RaftUnifiedRPCService::UnregisterRaftState(raft_group_id_t group_id) {
  std::unique_lock<std::shared_mutex> lock(mu_);
  raft_states_.erase(group_id);
  printf("[RAFT-UNIFIED-SVC] Unregistered RaftState for group=%u (total: %zu)\n",
         group_id, raft_states_.size());
}

RaftState* RaftUnifiedRPCService::GetRaftState(raft_group_id_t group_id) {
  std::shared_lock<std::shared_mutex> lock(mu_);
  auto it = raft_states_.find(group_id);
  if (it != raft_states_.end()) {
    return it->second;
  }
  return nullptr;
}

RCF::ByteBuffer RaftUnifiedRPCService::RequestVote(const RCF::ByteBuffer& arg_buf) {
  RequestVoteArgs args;
  RequestVoteReply reply;

  auto serializer = Serializer::NewSerializer();

  try {
    serializer.Deserialize(&arg_buf, &args);
  } catch (const std::exception& e) {
    printf("[RAFT-UNIFIED-SVC] RequestVote deserialize ERROR: %s\n", e.what());
    reply.vote_granted = false;
    reply.term = 0;
    RCF::ByteBuffer out_buf(serializer.getSerializeSize(reply));
    serializer.Serialize(&reply, &out_buf);
    return out_buf;
  }

  raft_group_id_t group_id = args.group_id;
  printf("[RAFT-UNIFIED-SVC] RequestVote: group=%u candidate=%d term=%d\n",
         group_id, args.candidate_id, args.term);

  // 获取对应的 RaftState
  RaftState* target_raft = GetRaftState(group_id);
  if (!target_raft) {
    // 降级：使用第一个可用的 RaftState（用于单 raft 实例场景）
    std::shared_lock<std::shared_mutex> lock(mu_);
    if (!raft_states_.empty()) {
      target_raft = raft_states_.begin()->second;
      printf("[RAFT-UNIFIED-SVC] WARNING: No RaftState for group=%u, using first available\n", group_id);
    }
  }

  if (target_raft) {
    try {
      printf("[RAFT-UNIFIED-SVC] RequestVote: calling Process() for group=%u\n", group_id);
      fflush(stdout);
      target_raft->Process(&args, &reply);
      printf("[RAFT-UNIFIED-SVC] RequestVote: Process() returned for group=%u\n", group_id);
      fflush(stdout);
    } catch (const std::exception& e) {
      printf("[RAFT-UNIFIED-SVC] ERROR: RaftState[group=%u] exception in RequestVote Process: %s\n",
             group_id, e.what());
      fflush(stdout);
      reply.vote_granted = false;
      reply.term = 0;
    }
  } else {
    printf("[RAFT-UNIFIED-SVC] ERROR: No RaftState found for RequestVote group=%u\n", group_id);
    fflush(stdout);
    reply.vote_granted = false;
    reply.term = 0;
  }

  printf("[RAFT-UNIFIED-SVC] RequestVote: group=%u sending reply (granted=%d term=%d)\n",
         group_id, reply.vote_granted, reply.term);
  fflush(stdout);

  try {
    size_t reply_size = serializer.getSerializeSize(reply);
    printf("[RAFT-UNIFIED-SVC] RequestVote: group=%u reply_size=%zu\n", group_id, reply_size);
    fflush(stdout);
    RCF::ByteBuffer out_buf(reply_size);
    serializer.Serialize(&reply, &out_buf);
    printf("[RAFT-UNIFIED-SVC] RequestVote: group=%u returning reply (buf_len=%zu)\n", group_id, out_buf.getLength());
    fflush(stdout);
    return out_buf;
  } catch (const std::exception& e) {
    printf("[RAFT-UNIFIED-SVC] ERROR: RequestVote serialize failed: %s\n", e.what());
    fflush(stdout);
    return RCF::ByteBuffer();
  }
}

RCF::ByteBuffer RaftUnifiedRPCService::AppendEntries(const RCF::ByteBuffer& arg_buf) {
  AppendEntriesArgs args;
  AppendEntriesReply reply;

  auto serializer = Serializer::NewSerializer();

  try {
    serializer.Deserialize(&arg_buf, &args);
  } catch (const std::exception& e) {
    printf("[RAFT-UNIFIED-SVC] AppendEntries deserialize ERROR: %s\n", e.what());
    reply.success = false;
    reply.term = 0;
    RCF::ByteBuffer out_buf(serializer.getSerializeSize(reply));
    serializer.Serialize(&reply, &out_buf);
    return out_buf;
  }

  raft_group_id_t group_id = args.group_id;

  // 获取对应的 RaftState
  RaftState* target_raft = GetRaftState(group_id);
  if (!target_raft) {
    // 降级：使用第一个可用的 RaftState（用于单 raft 实例场景）
    std::shared_lock<std::shared_mutex> lock(mu_);
    if (!raft_states_.empty()) {
      target_raft = raft_states_.begin()->second;
      printf("[RAFT-UNIFIED-SVC] WARNING: No RaftState for group=%u, using first available\n", group_id);
    }
  }

  if (target_raft) {
    try {
      target_raft->Process(&args, &reply);
    } catch (const std::bad_alloc& e) {
      printf("[RAFT-UNIFIED-SVC] FATAL: RaftState[group=%u] bad_alloc in Process: %s\n",
             group_id, e.what());
      reply.success = false;
      reply.term = 0;
    } catch (const std::exception& e) {
      printf("[RAFT-UNIFIED-SVC] ERROR: RaftState[group=%u] exception in Process: %s\n",
             group_id, e.what());
      reply.success = false;
      reply.term = 0;
    }
  } else {
    printf("[RAFT-UNIFIED-SVC] ERROR: No RaftState found for AppendEntries group=%u\n", group_id);
    reply.success = false;
    reply.term = 0;
  }

  try {
    RCF::ByteBuffer out_buf(serializer.getSerializeSize(reply));
    serializer.Serialize(&reply, &out_buf);
    return out_buf;
  } catch (const std::exception& e) {
    printf("[RAFT-UNIFIED-SVC] ERROR: AppendEntries serialize failed: %s\n", e.what());
    fflush(stdout);
    return RCF::ByteBuffer();
  }
}

RCF::ByteBuffer RaftUnifiedRPCService::RequestFragments(const RCF::ByteBuffer& arg_buf) {
  RequestFragmentsArgs args;
  RequestFragmentsReply reply;

  auto serializer = Serializer::NewSerializer();
  serializer.Deserialize(&arg_buf, &args);

  raft_group_id_t group_id = args.group_id;
  printf("[RAFT-UNIFIED-SVC] RequestFragments received: from_node=%u, group_id=%u, term=%u\n",
         args.leader_id, group_id, args.term);
  fflush(stdout);

  // 获取对应的 RaftState
  RaftState* target_raft = GetRaftState(group_id);
  if (!target_raft) {
    // 降级：使用第一个可用的 RaftState（用于单 raft 实例场景）
    std::shared_lock<std::shared_mutex> lock(mu_);
    if (!raft_states_.empty()) {
      target_raft = raft_states_.begin()->second;
      printf("[RAFT-UNIFIED-SVC] WARNING: No RaftState for group=%u, using first available\n", group_id);
    }
  }

  if (target_raft) {
    target_raft->Process(&args, &reply);
  } else {
    printf("[RAFT-UNIFIED-SVC] ERROR: No RaftState found for RequestFragments group=%u\n", group_id);
  }

  RCF::ByteBuffer out_buf(serializer.getSerializeSize(reply));
  serializer.Serialize(&reply, &out_buf);
  return out_buf;
}

RCF::ByteBuffer RaftUnifiedRPCService::GroupNotification(const RCF::ByteBuffer& arg_buf) {
  GroupNotificationArgs args;
  GroupNotificationReply reply;

  auto serializer = Serializer::NewSerializer();
  serializer.Deserialize(&arg_buf, &args);

  printf("[RAFT-UNIFIED-SVC] Received GroupNotification from node %u, %d groups\n",
         args.source_node_id, args.total_groups);

  for (int i = 0; i < args.total_groups; i++) {
    const auto& group = args.groups[i];
    printf("[RAFT-UNIFIED-SVC]   Group %u: %zu members", group.group_id, group.members.size());
    if (!group.complementary_group_indices.empty()) {
      printf(" (complementary to groups:");
      for (auto idx : group.complementary_group_indices) {
        printf(" %u", idx);
      }
      printf(")");
    }
    printf("\n");
    for (const auto& member : group.members) {
      printf("[RAFT-UNIFIED-SVC]     - node %u @ %s\n",
             member.node_id, member.raft_rpc_addr.c_str());
    }
  }

  // 调用回调函数，让 RaftStore 创建 Raft 实例
  if (group_notification_callback_) {
    try {
      group_notification_callback_(args);
      reply.success = 1;
    } catch (const std::exception& e) {
      printf("[RAFT-UNIFIED-SVC] GroupNotification callback failed: %s\n", e.what());
      reply.success = 0;
    }
  } else {
    printf("[RAFT-UNIFIED-SVC] WARNING: No GroupNotification callback set\n");
    reply.success = 1;  // 仍然返回成功，只是记录警告
  }

  reply.reply_node_id = static_cast<raft_node_id_t>(-1);  // TODO: 设置为实际节点ID

  RCF::ByteBuffer out_buf(serializer.getSerializeSize(reply));
  serializer.Serialize(&reply, &out_buf);
  return out_buf;
}

// =======================================================================
//  RaftUnifiedRpcServer 实现
// =======================================================================

RaftUnifiedRpcServer::RaftUnifiedRpcServer(int physical_node_id, const NetAddress& listen_addr)
    : physical_node_id_(physical_node_id),
      addr_(listen_addr),
      server_(RCF::TcpEndpoint(listen_addr.ip, listen_addr.port)),
      running_(false) {
  printf("[RAFT-UNIFIED-P%d] Created unified RPC server at %s:%d\n",
         physical_node_id_, listen_addr.ip.c_str(), listen_addr.port);
}

RaftUnifiedRpcServer::~RaftUnifiedRpcServer() {
  Stop();
}

void RaftUnifiedRpcServer::Start() {
  if (running_) {
    printf("[RAFT-UNIFIED-P%d] Server already running\n", physical_node_id_);
    return;
  }

  // 检查端口是否已被占用
  if (server_.isStarted()) {
    printf("[RAFT-UNIFIED-P%d] Server already bound, stopping first\n", physical_node_id_);
    server_.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  try {
    server_.getServerTransport().setMaxIncomingMessageLength(config::kMaxMessageLength);
    // Multi-Raft: dynamic thread pool size based on number of registered groups.
    // Each group needs at least 2 threads for concurrent RPC handling.
    // Minimum 16 threads ensures good concurrency even for small deployments.
    int num_threads = std::max(16, static_cast<int>(service_.GetRegisteredGroupCount() * 2));
    printf("[RAFT-UNIFIED-P%d] Starting with %d RPC threads (groups=%zu)\n",
           physical_node_id_, num_threads, service_.GetRegisteredGroupCount());
    RCF::ThreadPoolPtr tp(new RCF::ThreadPool(num_threads));
    server_.setThreadPool(tp);
    server_.bind<I_RaftRPCService>(service_);
    server_.bind<I_GroupNotificationService>(service_);
    server_.start();

    running_ = true;

    printf("[RAFT-UNIFIED-P%d] Server started successfully at %s:%d\n",
           physical_node_id_, addr_.ip.c_str(), addr_.port);
  } catch (const RCF::Exception& e) {
    printf("[RAFT-UNIFIED-P%d] Failed to start: %s\n", physical_node_id_, e.what());
    running_ = false;
    throw;
  }
}

void RaftUnifiedRpcServer::Stop() {
  if (!running_) {
    return;
  }

  running_ = false;
  server_.stop();

  printf("[RAFT-UNIFIED-P%d] Server stopped\n", physical_node_id_);
}

void RaftUnifiedRpcServer::RegisterRaftState(raft_group_id_t group_id, RaftState* raft_state) {
  service_.RegisterRaftState(group_id, raft_state);
}

void RaftUnifiedRpcServer::UnregisterRaftState(raft_group_id_t group_id) {
  service_.UnregisterRaftState(group_id);
}

RaftUnifiedRPCService* RaftUnifiedRpcServer::GetGlobalService() {
  return global_service_;
}

void RaftUnifiedRpcServer::SetGlobalService(RaftUnifiedRPCService* svc) {
  global_service_ = svc;
}

}  // namespace rpc
}  // namespace raft
