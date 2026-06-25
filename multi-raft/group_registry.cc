// group_registry.cc — GroupRegistry 实现
//
// 支持 Phase-based 启动：KV RPC 服务器在 Phase 8 启动
// GroupRegistry 继承自 I_KvServerRPCService，作为统一的 RPC 分发器
// 根据请求中的 group_id 将请求路由到对应的 group-specific KvServiceNode

#include "raft_store.h"
#include <cassert>
#include <chrono>
#include <thread>
#include "encoding_mode.h"
#include "kv_format.h"
#include "kv_node.h"
#include "stripe_read.h"

namespace multiraft {

GroupRegistry::GroupRegistry(int node_id, const kv::rpc::NetAddress& kv_listen_addr)
    : node_id_(node_id), listen_addr_(kv_listen_addr) {

  printf("[REGISTRY-N%d] Creating GroupRegistry (KV listen: %s:%d)\n",
         node_id_, listen_addr_.ip.c_str(), listen_addr_.port);

  // 注意：不再创建 shared KvServerRPCService（它持有单个 KvServer*）
  // GroupRegistry 自己就是 I_KvServerRPCService 的实现
  rpc_server_ = std::make_unique<kv::rpc::KvServerRPCServer>(
      listen_addr_, static_cast<raft::raft_node_id_t>(node_id_));
  rpc_server_->BindService(this);
  rpc_router_ = std::make_unique<RpcRouter>(node_id_);

  printf("[REGISTRY-N%d] GroupRegistry created (as RPC dispatcher)\n", node_id_);
}

GroupRegistry::~GroupRegistry() {
  printf("[REGISTRY-N%d] GroupRegistry destroyed\n", node_id_);
}

void GroupRegistry::SetEncodingMode(EncodingMode m) {
  encoding_mode_ = m;
  printf("[REGISTRY-N%d] encoding mode = %s (WRITE-IN logs use this)\n", node_id_,
         EncodingModeName(m));
  fflush(stdout);
}

kv::KvServiceNode* GroupRegistry::Register(GroupId gid, raft::raft_node_id_t local_nid,
                                           const kv::KvClusterConfig& group_cluster_cfg) {
  auto node = std::unique_ptr<kv::KvServiceNode>(
      kv::KvServiceNode::NewKvServiceNodeWithoutRPC(group_cluster_cfg, local_nid));
  auto* ptr = node.get();
  nodes_[{gid, local_nid}] = std::move(node);

  // 注册 local_nodes_ 映射：用于快速根据 group_id 查找本节点上的 KvServiceNode
  if (local_nid == static_cast<raft::raft_node_id_t>(node_id_)) {
    local_nodes_[gid] = ptr;
    printf("[REGISTRY-N%d] Registered local KvServiceNode: group=%u (keyed for RPC routing)\n",
           node_id_, gid);
  }

  printf("[REGISTRY-N%d] Registered KvServiceNode: group=%u local_id=%u\n",
         node_id_, gid, local_nid);
  return ptr;
}

kv::KvServiceNode* GroupRegistry::Get(GroupId gid, raft::raft_node_id_t local_nid) {
  auto it = nodes_.find({gid, local_nid});
  return (it != nodes_.end()) ? it->second.get() : nullptr;
}

kv::KvServiceNode* GroupRegistry::GetNodeForGroup(raft::raft_group_id_t group_id) {
  auto it = local_nodes_.find(group_id);
  if (it != local_nodes_.end()) {
    return it->second;
  }
  return nullptr;
}

void GroupRegistry::InitAll() {
  for (auto& [k, n] : nodes_) {
    printf("[REGISTRY-N%d] Init KvServiceNode: group=%u local_id=%u\n",
           node_id_, k.group_id, k.local_node_id);
    n->InitServiceNodeState();
  }
}

void GroupRegistry::StartAll() {
  for (auto& [k, n] : nodes_) {
    printf("[REGISTRY-N%d] Start KvServiceNode: group=%u local_id=%u\n",
           node_id_, k.group_id, k.local_node_id);
    n->StartServiceNode();
  }
}

void GroupRegistry::StartRpcServer() {
  if (rpc_server_) {
    printf("[REGISTRY-N%d] Starting KV RPC server at %s:%d\n",
           node_id_, listen_addr_.ip.c_str(), listen_addr_.port);
    fprintf(stderr, "[REGISTRY-N%d] About to start RPC server...\n", node_id_);
    rpc_server_->Start();
    fprintf(stderr, "[REGISTRY-N%d] RPC server started successfully\n", node_id_);
    printf("[REGISTRY-N%d] KV RPC server started (dispatcher mode)\n", node_id_);
  }
}

void GroupRegistry::StopAll() {
  if (rpc_server_) {
    printf("[REGISTRY-N%d] Stopping KV RPC server\n", node_id_);
    rpc_server_->Stop();
  }

  for (auto& [k, n] : nodes_) {
    n->StopServiceNode();
  }
  printf("[REGISTRY-N%d] All KvServiceNodes stopped\n", node_id_);
}

// ---------------------------------------------------------------------------
//  I_KvServerRPCService 实现：作为统一的 KV RPC 分发器
//
//  DealWithRequest 根据 request.group_id 将请求路由到正确的 group-specific
//  KvServiceNode。如果 group_id 无效或本节点不是该 group 的成员，返回错误。
// ---------------------------------------------------------------------------
kv::Response GroupRegistry::DealWithRequest(const kv::Request& req) {
  // Only log periodically to avoid log flooding
  static std::atomic<uint64_t> s_log_counter{0};
  bool do_log = (s_log_counter.fetch_add(1, std::memory_order_relaxed) % 64 == 0);

  const bool is_write = (req.type == kv::kPut || req.type == kv::kDelete);
  if (is_write) {
    const char* op = (req.type == kv::kPut) ? "PUT" : "DELETE";
    printf("[WRITE-IN][REGISTRY-N%d] path=KV-RPC enc=%s op=%s group=%u client=%u seq=%u "
           "key_len=%zu val_len=%zu\n",
           node_id_, EncodingModeName(encoding_mode_), op, req.group_id, req.client_id,
           req.sequence, req.key.size(),
           req.type == kv::kPut ? req.value.size() : static_cast<size_t>(0));
    fflush(stdout);
  }

  kv::Response resp;

  kv::KvServiceNode* target_node = nullptr;

  // Route based on group_id if valid
  if (req.group_id != raft::raft_group_id_t(-1)) {
    target_node = GetNodeForGroup(req.group_id);
  }

  // If no target found, return error (no silent fallback)
  if (!target_node) {
    if (do_log) {
      fprintf(stderr, "[REGISTRY-N%d] DealWithRequest: no node for group_id=%u (type=%d)\n",
              node_id_, req.group_id, req.type);
    }
    resp.err = kv::kNotALeader;
    resp.raft_term = 0;
    resp.group_id = req.group_id;
    return resp;
  }

  if (req.type == kv::kPut && raft_store_) {
    auto* raft_node = target_node->GetRaftNode();
    if (raft_node && raft_node->IsLeader()) {
      resp.type = req.type;
      resp.client_id = req.client_id;
      resp.sequence = req.sequence;
      resp.raft_term = raft_node->getRaftState()->CurrentTerm();
      resp.reply_server_id = static_cast<raft::raft_node_id_t>(node_id_);
      resp.group_id = req.group_id;


      auto [err, commit_elapse, apply_elapse] =
          raft_store_->StripePut(req.group_id, req.key, req.value);
      resp.err = err;
      resp.commit_elapse_time = commit_elapse;
      resp.apply_elapse_time = apply_elapse;
      printf("[GROUP-REGISTRY-N%d] StripePut done: group=%u err=%d commit_us=%lu\n",
             node_id_, req.group_id, static_cast<int>(err), commit_elapse);
      fflush(stdout);
      return resp;
    }
  }

  // ---- kDetectLeader: route to group-specific RaftNode, not KvServer ----
  // Multi-Raft: KvServer::raft_ is nullptr (RaftNode is managed externally).
  // Check via target_node->GetRaftNode() instead.
  if (req.type == kv::kDetectLeader) {
    auto* raft_node = target_node->GetRaftNode();
    if (raft_node && raft_node->IsLeader()) {
      resp.err = kv::kOk;
      resp.raft_term = raft_node->getRaftState()->CurrentTerm();
      // This node is the leader, set leader_hint to self
      resp.leader_hint = static_cast<raft::raft_node_id_t>(node_id_);
    } else {
      resp.err = kv::kNotALeader;
      resp.raft_term = raft_node ? raft_node->getRaftState()->CurrentTerm() : 0;
      // TODO: Set leader_hint to the known leader if available.
      // For a complete implementation, RaftNode should expose GetKnownLeader()
      // or maintain a voted_for cache. For now, we set it to kNoDetectLeader.
      resp.leader_hint = kv::kNoDetectLeader;
    }
    resp.reply_server_id = static_cast<raft::raft_node_id_t>(node_id_);
    resp.group_id = req.group_id;
    return resp;
  }

  kv::KvServer* kv_server = target_node->GetKvServer();
  if (!kv_server) {
    if (do_log) {
      fprintf(stderr, "[REGISTRY-N%d] DealWithRequest: null KvServer for group_id=%u\n",
              node_id_, req.group_id);
    }
    resp.err = kv::kNotALeader;
    resp.raft_term = 0;
    resp.group_id = req.group_id;
    return resp;
  }

  // ---------------------------------------------------------------------------
  // kGet: Leader-only read path for Multi-Raft + Stripe encoding
  //
  // Correct read path (leader-only):
  //   Client RouteGet() → DealWithRequest(kGet) → [leader check] → TryStripeGet()
  //     ├─ Read __stripe_meta__/key to get placement
  //     ├─ Collect k fragments via internal GetValue RPCs to peers
  //     ├─ Decode fragments → raw value
  //     └─ Return k=1 format to client
  //
  // TryStripeGet already handles all shard collection and decoding internally.
  // DoGatherValueTask in client.cc is dead code in Multi-Raft mode (kept for
  // legacy single-Raft path).
  // ---------------------------------------------------------------------------
  if (req.type == kv::kGet) {
    static std::atomic<uint64_t> s_get_log{0};
    uint64_t seq = s_get_log.fetch_add(1, std::memory_order_relaxed);
    bool do_get_log = (seq % 20 == 0);  // log every 20th GET
    resp.type = req.type;
    resp.client_id = req.client_id;
    resp.sequence = req.sequence;
    resp.reply_server_id = static_cast<raft::raft_node_id_t>(node_id_);
    resp.group_id = req.group_id;

    auto* raft_node = target_node->GetRaftNode();
    if (!raft_node || !raft_node->IsLeader()) {
      resp.err = kv::kNotALeader;
      resp.raft_term = raft_node ? raft_node->getRaftState()->CurrentTerm() : 0;
      if (do_get_log) {
        fprintf(stderr, "[GET-REJECT][REGISTRY-N%d] group=%u not leader (isLeader=%d)\n",
                node_id_, req.group_id, raft_node ? raft_node->IsLeader() : -1);
      }
      return resp;
    }
    resp.raft_term = raft_node->getRaftState()->CurrentTerm();
    if (do_get_log) {
      fprintf(stderr, "[GET-IN][REGISTRY-N%d] group=%u key_len=%zu isLeader=1 term=%d\n",
              node_id_, req.group_id, req.key.size(),
              static_cast<int>(resp.raft_term));
    }

    // Try StripeRead first (LRC/RS encoded data).
    int cluster_n = raft_store_ ? raft_store_->GetPhysicalClusterSize() : 0;
    if (cluster_n <= 0 && raft_node) {
      cluster_n = raft_node->ClusterServerNum();
    }
    if (cluster_n <= 0) {
      cluster_n = 7;  // fallback for 7-node cluster
    }
    // Use read_index=0 to skip the linearizability check on followers.
    // Fragment reads don't need strict consistency — they're keyed by stripe_id.
    kv::GetValueRequest stripe_req{req.key, 0, req.group_id};
    kv::GetValueResponse stripe_resp;
    if (TryStripeGet(target_node, static_cast<raft::raft_node_id_t>(node_id_),
                     cluster_n, stripe_req, &stripe_resp)) {
      if (do_get_log) {
        fprintf(stderr, "[GET-SUCC][REGISTRY-N%d] key_len=%zu value_size=%zu\n",
                node_id_, req.key.size(), stripe_resp.value.size());
      }
      // Wrap decoded value in kv::FormatFullEntryValueForStorage format
      // so the client's DecodeString/GetKeyFromPrefixLengthFormat can extract it.
      resp.err = kv::kOk;
      resp.value = kv::FormatFullEntryValueForStorage(stripe_resp.value);
      return resp;
    }

    // StripeRead failed: do NOT fall back to raw key lookup.
    // In stripe mode, the original user key is NOT stored in DB —
    // only __stripe_meta__/user_key and __frag__/group/stripe/frag_id keys exist.
    // Return kKeyNotExist so the client knows the stripe read failed.
    if (do_get_log) {
      fprintf(stderr,
              "[GET-FAIL][REGISTRY-N%d] kGet: TryStripeGet failed for key='%s' group=%u, "
              "no raw key fallback in stripe mode\n",
              node_id_, req.key.c_str(), req.group_id);
    }
    resp.err = kv::kKeyNotExist;
    return resp;
  }

  kv_server->DealWithRequest(&req, &resp);
  resp.group_id = req.group_id;

  if (is_write) {
    const char* op = (req.type == kv::kPut) ? "PUT" : "DELETE";
    printf("[WRITE-OUT][REGISTRY-N%d] enc=%s op=%s group=%u err=%d term=%u reply_sid=%u\n",
           node_id_, EncodingModeName(encoding_mode_), op, req.group_id, static_cast<int>(resp.err),
           static_cast<unsigned>(resp.raft_term), static_cast<unsigned>(resp.reply_server_id));
    fflush(stdout);
  }

  return resp;
}

// GetValue: Internal RPC handler for Multi-Raft shard collection.
// Called by TryStripeGet via kv->GetKVPeerServerStub(node_id)->GetValue()
// to collect fragments from peer nodes.
// NOT called by external clients — use DealWithRequest(kGet) instead.
kv::GetValueResponse GroupRegistry::GetValue(const kv::GetValueRequest& req) {
  fprintf(stderr, "[DIAG-GETVAL] N%d: GetValue req.key='%s' group=%u\n",
          node_id_, req.key.c_str(), req.group_id);
  kv::KvServiceNode* target_node = nullptr;
  if (req.group_id != static_cast<raft::raft_group_id_t>(-1)) {
    target_node = GetNodeForGroup(req.group_id);
    fprintf(stderr, "[DIAG-GETVAL] N%d: group_id=%u -> target_node=%p\n",
            node_id_, req.group_id, static_cast<void*>(target_node));
  } else if (!local_nodes_.empty()) {
    target_node = local_nodes_.begin()->second;
  }

  if (!target_node) {
    fprintf(stderr, "[REGISTRY-N%d] GetValue FAILED: no local KvServiceNode for group=%d\n",
            node_id_, static_cast<int>(req.group_id));
    return {"", kv::kNotALeader, static_cast<raft::raft_node_id_t>(node_id_)};
  }

  kv::KvServer* kv_server = target_node->GetKvServer();
  if (!kv_server) {
    return {"", kv::kNotALeader, static_cast<raft::raft_node_id_t>(node_id_)};
  }

  // Skip linearizability wait when read_index == 0
  // (Multi-Raft followers' applied_index_ may not be updated via channel_)
  if (req.read_index > 0) {
    auto start = std::chrono::steady_clock::now();
    while (kv_server->LastApplyIndex() < req.read_index) {
      if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(500)) {
        fprintf(stderr,
                "[REGISTRY-N%d] GetValue TIMEOUT waiting apply: key=%s group=%d "
                "read_index=%u last_apply=%d\n",
                node_id_, req.key.c_str(), static_cast<int>(req.group_id), req.read_index,
                kv_server->LastApplyIndex());
        return {"", kv::kRequestExecTimeout, static_cast<raft::raft_node_id_t>(node_id_)};
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  kv::GetValueResponse stripe_resp;
  int cluster_n = raft_store_ ? raft_store_->GetPhysicalClusterSize() : 0;
  if (cluster_n <= 0) {
    cluster_n = static_cast<int>(target_node->GetKvServer()->ClusterServerNum());
  }
  if (TryStripeGet(target_node, static_cast<raft::raft_node_id_t>(node_id_), cluster_n, req,
                   &stripe_resp)) {
    return stripe_resp;
  }

  std::string value;
  if (kv_server->DB()->Get(req.key, &value)) {
    return {std::move(value), kv::kOk, static_cast<raft::raft_node_id_t>(node_id_)};
  }
  return {"", kv::kKeyNotExist, static_cast<raft::raft_node_id_t>(node_id_)};
}

}  // namespace multiraft
