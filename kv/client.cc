#include "client.h"

#include "config.h"
#include "encoder.h"
#include "kv_format.h"
#include "type.h"
#include "util.h"
namespace kv {
KvServiceClient::KvServiceClient(const KvClusterConfig &config, uint32_t client_id, int pool_size)
    : client_id_(client_id), pool_size_(pool_size) {
  servers_.reserve(config.size());
  for (const auto &[id, conf] : config) {
    servers_.insert({id, new rpc::KvServerRPCClient(conf.kv_rpc_addr, id, pool_size)});
  }
  curr_leader_ = kNoDetectLeader;
  curr_leader_term_ = 0;
}

KvServiceClient::~KvServiceClient() {
  for (auto [id, ptr] : servers_) {
    delete ptr;
  }
}

// ============================================================================
// 废弃单Raft API实现（使用Multi-Raft路由API替代）
// ============================================================================

// Response KvServiceClient::WaitUntilRequestDone(const Request &request) {
//   raft::util::Timer timer;
//   timer.Reset();
//   LOG(raft::util::kRaft, "[C%d] Dealing With Req (%s)", ClientId(), ToString(request).c_str());
//   while (timer.ElapseMilliseconds() < kKVRequestTimesoutCnt * 1000) {
//     if (curr_leader_ == kNoDetectLeader && DetectCurrentLeader() == kNoDetectLeader) {
//       LOG(raft::util::kRaft, "Detect No Leader");
//       sleepMs(300);
//       continue;
//     }
//     LOG(raft::util::kRaft, "[C%d] Send Req (%s) to S%d", ClientId(), ToString(request).c_str(),
//         curr_leader_);
//     auto resp = GetRPCStub(curr_leader_)->DealWithRequest(request);
//     switch (resp.err) {
//       case kOk:
//       case kKeyNotExist:
//         return resp;
//
//       case kEntryDeleted:
//       case kRequestExecTimeout:
//       case kNotALeader:
//       case kRPCCallFailed:
//         LOG(raft::util::kRaft, "[C%d] Recv Resp(err=%s), Fallback to Nonleader", ClientId(),
//             ToString(resp.err).c_str());
//         curr_leader_ = kNoDetectLeader;
//         curr_leader_term_ = 0;
//         break;
//     }
//
//     if (request.type == kAbort) {
//       return resp;
//     }
//   }
//   Response resp;
//   resp.err = kRequestExecTimeout;
//   return resp;
// }

// OperationResults KvServiceClient::Put(const std::string &key, const std::string &value) {
//   Request request = {kPut, ClientId(), 0, raft::raft_group_id_t(-1), key, value};
//   auto resp = WaitUntilRequestDone(request);
//   return OperationResults{resp.err, resp.apply_elapse_time, resp.commit_elapse_time};
// }

// OperationResults KvServiceClient::Abort() {
//   Request request = {kAbort, ClientId(), 0, raft::raft_group_id_t(-1), "", ""};
//   auto resp = WaitUntilRequestDone(request);
//   return OperationResults{resp.err, 0, 0};
// }

// OperationResults KvServiceClient::Get(const std::string &key, std::string *value) {
//   Request request = {kGet, ClientId(), 0, raft::raft_group_id_t(-1), key, std::string("")};
//   auto resp = WaitUntilRequestDone(request);
//
//   LOG(raft::util::kRaft, "[C%d] Recv Resp from S%d", ClientId(), resp.reply_server_id);
//
//   if (resp.err != kOk) {
//     return OperationResults{resp.err, resp.apply_elapse_time};
//   }
//
//   auto format = DecodeString(&resp.value);
//
//   if (format.k == 1) {
//     GetKeyFromPrefixLengthFormat(format.frag.data(), value);
//     return OperationResults{kOk, 0};
//   }
//
//   LOG(raft::util::kRaft, "[Get Partial Value: k=%d m=%d readindex=%d], start collecting", format.k,
//       format.m, resp.read_index);
//
//   int k = format.k, m = format.m;
//   raft::Encoder::EncodingResults input;
//   input.insert({format.frag_id, raft::Slice::Copy(format.frag)});
//   LOG(raft::util::kRaft, "[C%d] Add Fragment of Frag%d from S%d", ClientId(), format.frag_id,
//       resp.reply_server_id);
//
//   GatherValueTask task{key, resp.read_index, resp.reply_server_id, &input, k, m,
//                        raft::raft_group_id_t(-1)};
//   GatherValueTaskResults res{value, kOk};
//
//   DoGatherValueTask(&task, &res);
//   return OperationResults{res.err, 0};
// }

// OperationResults KvServiceClient::Delete(const std::string &key) {
//   Request request = {kDelete, ClientId(), 0, raft::raft_group_id_t(-1), key, ""};
//   auto resp = WaitUntilRequestDone(request);
//   return OperationResults{resp.err, resp.apply_elapse_time};
// }

// raft::raft_node_id_t KvServiceClient::DetectCurrentLeader() {
//   for (auto &[id, stub] : servers_) {
//     if (stub == nullptr) {
//       continue;
//     }
//     Request detect_request = {kDetectLeader, ClientId(), 0, raft::raft_group_id_t(-1), "", ""};
//     auto resp = GetRPCStub(id)->DealWithRequest(detect_request);
//     if (resp.err == kOk) {
//       if (resp.raft_term > curr_leader_term_) {
//         curr_leader_ = id;
//         curr_leader_term_ = resp.raft_term;
//       }
//     }
//   }
//   return curr_leader_;
// }

// void KvServiceClient::DoGatherValueTask(const GatherValueTask *task, GatherValueTaskResults *res) {
//   LOG(raft::util::kRaft, "[C%d] Start running Gather Value Task, k=%d, m=%d", ClientId(), task->k,
//       task->m);
//   std::atomic<bool> gather_value_done = false;
//
//   std::mutex mtx;
//
//   auto call_back = [=, this, &gather_value_done, &mtx](const GetValueResponse &resp) {
//     LOG(raft::util::kRaft, "[C%d] Recv GetValue Response from S%d", ClientId(),
//         resp.reply_server_id);
//     if (resp.err != kOk) {
//       return;
//     }
//     std::scoped_lock<std::mutex> lck(mtx);
//
//     auto fmt = DecodeString(const_cast<std::string *>(&resp.value));
//     LOG(raft::util::kRaft, "[C%d] Decode Value: k=%d, m=%d, fragid=%d", ClientId(), fmt.k, fmt.m,
//         fmt.frag_id)
//
//     if (fmt.k == 1 && fmt.m == 0) {
//       GetKeyFromPrefixLengthFormat(fmt.frag.data(), res->value);
//       res->err = kOk;
//       gather_value_done.store(true);
//       LOG(raft::util::kRaft, "[C%d] Get Full Entry, value=%s", ClientId(), res->value->c_str());
//       return;
//     } else {
//       if (fmt.k == task->k && fmt.m == task->m) {
//         task->decode_input->insert({fmt.frag_id, raft::Slice::Copy(fmt.frag)});
//         LOG(raft::util::kRaft, "[C%d] Add Fragment%d", ClientId(), fmt.frag_id);
//       }
//
//       if (!gather_value_done.load() && task->decode_input->size() >= task->k) {
//         raft::Encoder encoder;
//         raft::Slice results;
//         auto stat = encoder.DecodeSlice(*(task->decode_input), task->k, task->m, &results);
//         if (stat) {
//           GetKeyFromPrefixLengthFormat(results.data(), res->value);
//           res->err = kOk;
//           gather_value_done.store(true);
//           LOG(raft::util::kRaft, "[C%d] Decode Value Succ", ClientId());
//         } else {
//           res->err = kKVDecodeFail;
//           LOG(raft::util::kRaft, "[C%d] Decode Value Fail", ClientId());
//         }
//       }
//     }
//   };
//
//   auto clear_gather_ctx = [=, this]() {
//     for (auto &[_, frag] : *(task->decode_input)) {
//       delete[] frag.data();
//     }
//   };
//
//   auto get_req = GetValueRequest{task->key, task->read_index, task->group_id};
//   for (auto &[id, server] : servers_) {
//     if (id != task->replied_id) {
//       GetRPCStub(id)->SetRPCTimeOutMs(1000);
//       auto resp = GetRPCStub(id)->GetValue(get_req);
//       if (resp.err == kOk) {
//         call_back(resp);
//       }
//     }
//   }
//
//   raft::util::Timer timer;
//   timer.Reset();
//   while (timer.ElapseMilliseconds() <= 1000) {
//     if (gather_value_done.load() == true) {
//       clear_gather_ctx();
//       return;
//     } else {
//       sleepMs(100);
//     }
//   }
//   if (res->err == kOk) {
//     res->err = kRequestExecTimeout;
//   }
//   clear_gather_ctx();
// }
// ============================================================================
// 废弃单Raft API实现结束
// ============================================================================

// =======================================================================
//  Multi-Raft 客户端路由（确定性哈希，无状态）
// =======================================================================

// FNV-1a: 纯确定性哈希，跨进程/编译器结果一致
static uint64_t FnvHash(const std::string& key) {
    uint64_t hash = 14695981039346656037ULL;  // FNV offset basis
    for (unsigned char c : key) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;  // FNV prime
    }
    return hash;
}

raft::raft_node_id_t KvServiceClient::GetGroupForKey(const std::string& key, int num_groups) {
    if (num_groups <= 0) return 0;
    return static_cast<raft::raft_node_id_t>(FnvHash(key) % static_cast<uint64_t>(num_groups));
}

// ---------------------------------------------------------------------------
//  Thread-safe cache management methods
// ---------------------------------------------------------------------------

void KvServiceClient::UpdateLeaderCache(raft::raft_node_id_t group_id,
                                        raft::raft_node_id_t leader_id) {
  std::unique_lock<std::shared_mutex> lk(routing_mutex_);
  group_to_leader_[group_id] = leader_id;
  LOG(raft::util::kRaft, "[C%d] LeaderCache: group=%u -> leader=%u",
      ClientId(), group_id, leader_id);
}

void KvServiceClient::InvalidateLeaderCache(raft::raft_node_id_t group_id) {
  std::unique_lock<std::shared_mutex> lk(routing_mutex_);
  group_to_leader_.erase(group_id);
  LOG(raft::util::kRaft, "[C%d] LeaderCache: invalidated group=%u",
      ClientId(), group_id);
}

// ---------------------------------------------------------------------------
//  ProbeLeader: 验证指定节点是否为给定 group 的 leader
// ---------------------------------------------------------------------------

bool KvServiceClient::ProbeLeader(raft::raft_node_id_t node_id,
                                  raft::raft_group_id_t group_id) {
  if (node_id == kNoDetectLeader) {
    return false;
  }

  Request req{kDetectLeader, ClientId(), 0, group_id, "", ""};
  auto resp = GetRPCStub(node_id)->DealWithRequest(req);

  if (resp.err == kOk) {
    return true;
  }

  // Even if not leader, update cache with leader_hint if available
  if (resp.err == kNotALeader && resp.leader_hint != kNoDetectLeader) {
    UpdateLeaderCache(group_id, resp.leader_hint);
  }

  return false;
}

// ---------------------------------------------------------------------------
//  GetPeersForGroup: 获取指定 group 的所有成员节点列表
//
//  假设环境中已存在该方法实现。如果不存在，客户端将使用 fallback 逻辑
//  (假设 group_id == node_id)。
// ---------------------------------------------------------------------------

std::vector<raft::raft_node_id_t> KvServiceClient::GetPeersForGroup(
    raft::raft_node_id_t group_id) {
  // 预留接口：实际的 peer 列表获取可能需要通过配置中心或服务发现
  // 当前实现返回空 vector，触发 fallback 逻辑 (假设 group_id == node_id)
  //
  // 未来实现示例：
  //   return config_center_->GetPeersForGroup(group_id);
  // 或：
  //   return group_membership_service_->GetPeers(group_id);
  return {};
}

// ---------------------------------------------------------------------------
//  DetectLeaderForGroup: 重构后的 Multi-Raft leader 探测逻辑
//
//  修复了以下问题：
//    1. Data Race: 不再使用 const_cast 修改 shared_lock 保护的数据
//    2. 组成员发现: 优先使用 GetPeersForGroup 获取 peer 列表
//    3. Leader Hint: 利用 Raft 协议的 redirect 机制
//    4. 避免死循环: 有序遍历 peer 列表，最多探测 N 次后放弃
// ---------------------------------------------------------------------------

raft::raft_node_id_t KvServiceClient::DetectLeaderForGroup(raft::raft_node_id_t group_id) {
  // ---- Step 1: 尝试使用缓存的 leader ----
  {
    std::shared_lock<std::shared_mutex> lk(routing_mutex_);
    auto it = group_to_leader_.find(group_id);
    if (it != group_to_leader_.end() && it->second != kNoDetectLeader) {
      raft::raft_node_id_t cached = it->second;
      lk.unlock();  // 释放读锁后再进行 RPC 探测

      if (ProbeLeader(cached, group_id)) {
        LOG(raft::util::kRaft, "[C%d] DetectLeaderForGroup(%u): cache hit -> leader=%u",
            ClientId(), group_id, cached);
        return cached;
      }

      // 缓存失效（RPC 探测失败），清除缓存
      InvalidateLeaderCache(group_id);
    }
  }

  // ---- Step 2: 获取 group 的所有 peer 节点列表 ----
  std::vector<raft::raft_node_id_t> peers = GetPeersForGroup(group_id);
  if (peers.empty()) {
    // Fallback: 如果没有 peer 信息，假设 group_id == node_id
    peers.push_back(group_id);
  }

  // ---- Step 3: 遍历 peer 列表探测 leader ----
  for (raft::raft_node_id_t peer_id : peers) {
    Request req{kDetectLeader, ClientId(), 0, group_id, "", ""};
    auto resp = GetRPCStub(peer_id)->DealWithRequest(req);

    if (resp.err == kOk) {
      // 找到 leader，更新缓存并返回
      UpdateLeaderCache(group_id, peer_id);
      LOG(raft::util::kRaft, "[C%d] DetectLeaderForGroup(%u): found leader=%u via peer=%u",
          ClientId(), group_id, peer_id, peer_id);
      return peer_id;
    }

    // ---- Step 4: 利用 Leader Hint（Raft 重定向机制）----
    // 当节点返回 kNotALeader 时，Raft 协议标准支持返回 leader_hint
    // 我们直接向 hint 指向的节点发起请求，而不是盲目重试
    if (resp.err == kNotALeader && resp.leader_hint != kNoDetectLeader) {
      LOG(raft::util::kRaft, "[C%d] DetectLeaderForGroup(%u): got hint -> leader=%u from peer=%u",
          ClientId(), group_id, resp.leader_hint, peer_id);

      // 直接向 hint 指向的节点验证
      if (ProbeLeader(resp.leader_hint, group_id)) {
        UpdateLeaderCache(group_id, resp.leader_hint);
        LOG(raft::util::kRaft, "[C%d] DetectLeaderForGroup(%u): confirmed leader=%u via hint",
            ClientId(), group_id, resp.leader_hint);
        return resp.leader_hint;
      }
    }
  }

  // 无法找到 leader
  LOG(raft::util::kRaft, "[C%d] DetectLeaderForGroup(%u): NO LEADER FOUND after probing %zu peers",
      ClientId(), group_id, peers.size());
  return kNoDetectLeader;
}

OperationResults KvServiceClient::RoutePut(const std::string& key, const std::string& value) {
  if (num_groups_ <= 0) {
    num_groups_ = static_cast<int>(servers_.size());
  }
  raft::raft_node_id_t group_id = GetGroupForKey(key, num_groups_);
  raft::raft_node_id_t leader = DetectLeaderForGroup(group_id);

  LOG(raft::util::kRaft, "[C%d] RoutePut: key='%s' -> group=%d leader=%d",
      ClientId(), key.c_str(), group_id, leader);

  if (leader == kNoDetectLeader) {
    fprintf(stderr, "[C%u] RoutePut FAILED: no leader for group=%d\n",
            ClientId(), group_id);
    return OperationResults{kNotALeader, 0, 0};
  }

  Request req{kPut, ClientId(), 0, group_id, key, value};
  auto resp = GetRPCStub(leader)->DealWithRequest(req);

  return OperationResults{resp.err, resp.apply_elapse_time, resp.commit_elapse_time};
}

OperationResults KvServiceClient::RouteGet(const std::string& key, std::string* value) {
  if (num_groups_ <= 0) {
    num_groups_ = static_cast<int>(servers_.size());
  }

  raft::raft_node_id_t group_id = GetGroupForKey(key, num_groups_);

  LOG(raft::util::kRaft, "[C%d] RouteGet: key='%s' -> group=%d",
      ClientId(), key.c_str(), group_id);

  raft::raft_node_id_t leader = DetectLeaderForGroup(group_id);
  if (leader == kNoDetectLeader) {
    fprintf(stderr, "[C%u] RouteGet FAILED: no leader for group=%d\n",
            ClientId(), group_id);
    return OperationResults{kNotALeader, 0, 0};
  }

  Request req{kGet, ClientId(), 0, group_id, key, std::string("")};
  auto resp = GetRPCStub(leader)->DealWithRequest(req);

  if (resp.err == kOk && value) {
    auto format = DecodeString(&resp.value);
    if (format.k == 1) {
      // k=1: resp.value contains FormatFullEntryValueForStorage format:
      //   [k=1(4bytes)][m=0(4bytes)][frag_id=0(4bytes)][prefix_len(4bytes)][user_value bytes]
      // DecodeString sets frag = user_value bytes (after prefix_len header).
      // GetKeyFromPrefixLengthFormat expects [prefix_len][data] — it skips 4 bytes then reads.
      // This correctly extracts the raw user value.
      GetKeyFromPrefixLengthFormat(format.frag.data(), value);
      LOG(raft::util::kRaft, "[C%d] RouteGet SUCC: key='%s' (k=1, decoded %zu bytes)",
          ClientId(), key.c_str(), value->size());
      return OperationResults{kOk, 0, 0};
    }

    // Multi-Raft + stripe 模式下，服务器端已解码值并返回 k=1 格式
    // 此分支不应被执行到这里
    fprintf(stderr, "[C%d] RouteGet WARNING: unexpected k=%d (expected k=1 for decoded value)\n",
            ClientId(), format.k);
    return OperationResults{kKVDecodeFail, 0, 0};
  }

  LOG(raft::util::kRaft, "[C%d] RouteGet FAILED: err=%d", ClientId(), resp.err);
  return OperationResults{resp.err, resp.apply_elapse_time, 0};
}

}  // namespace kv
