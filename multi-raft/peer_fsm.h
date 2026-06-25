#pragma once
// peer_fsm.h  —  PeerFsm Actor
//
// 修改说明（2026-03-29）：
//   1. 解决循环依赖问题，将接口分离到 peer_fsm_interface.h
//   2. 支持与 RaftStore 交互获取 tracker 和 routing table
//   3. 使用 FlexRaft 原生 RS 编码（LRC 暂时禁用）
//
// PeerFsm 的职责:
//   - 持有并驱动 FlexRaft 的 RaftNode 状态机（tick / step）
//   - 处理 MsgRaftMessage: 调用 RaftNode::Step()
//   - 处理 MsgProposeShard: 调用 RaftNode::Propose()，将 shard 写入 Raft log
//   - 处理 MsgTick: 驱动 Raft 定时器（心跳、选举超时）
//   - 处理 MsgShardCommitAck: 跨 group commit 聚合（转发给 CrossGroupTracker）
//   - 将 committed entries 发给对应的 ApplyFsm Mailbox

#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>

#include "encoding_mode.h"
#include "kv_node.h"      // KvServiceNode (FlexRaft 原有)
#include "mailbox.h"
#include "message.h"
#include "peer_fsm_interface.h"

namespace multiraft {

// -----------------------------------------------------------------------
//  PeerFsm
// -----------------------------------------------------------------------
class PeerFsm {
 public:
  PeerFsm(GroupId group_id,
          raft::raft_node_id_t local_node_id,
          kv::KvServiceNode* kv_node,
          Mailbox<ApplyMsg>* apply_mb,
          CrossGroupTrackerInterface* tracker,
          RaftRouterInterface* router,
          RaftStoreInterface* store)
      : group_id_(group_id),
        local_node_id_(local_node_id),
        kv_node_(kv_node),
        apply_mb_(apply_mb),
        tracker_(tracker),
        router_(router),
        store_(store),
        last_status_print_time_(0),
        tick_count_(0),
        last_is_leader_(false) {
        printf("[PEER-G%d-N%d] PeerFsm created\n", group_id_, local_node_id_);
    if (kv_node_ && kv_node_->GetRaftNode()) {
      auto raft_node = kv_node_->GetRaftNode();
      printf("[PEER-G%d-N%d] RaftNode initialized at %p\n", group_id_, local_node_id_, (void*)raft_node);
    }
  }

  GroupId GetGroupId() const { return group_id_; }
  raft::raft_node_id_t GetNodeId() const { return local_node_id_; }

  // 获取本节点是否是该 group 的 leader
  bool IsLeader() const {
    if (kv_node_ && kv_node_->GetRaftNode()) {
      return kv_node_->GetRaftNode()->IsLeader();
    }
    return false;
  }

  // 获取 Raft 状态详细信息（用于诊断）
  void PrintDetailedStatus() const {
    if (kv_node_ && kv_node_->GetRaftNode()) {
      auto raft_node = kv_node_->GetRaftNode();
      auto* raft_state = raft_node->getRaftState();
      if (raft_state) {
        printf("[PEER-G%d-N%d] ===== DETAILED STATUS =====\n", group_id_, local_node_id_);
        printf("[PEER-G%d-N%d]   Role: %d (0=Follower,1=Candidate,2=PreLeader,3=Leader)\n",
               group_id_, local_node_id_, (int)raft_state->Role());
        printf("[PEER-G%d-N%d]   Term: %u\n", group_id_, local_node_id_, raft_state->CurrentTerm());
        printf("[PEER-G%d-N%d]   VoteFor: %d\n", group_id_, local_node_id_, raft_state->VoteFor());
        printf("[PEER-G%d-N%d]   LastIndex: %u\n", group_id_, local_node_id_, raft_state->LastIndex());
        printf("[PEER-G%d-N%d]   CommitIndex: %u\n", group_id_, local_node_id_, raft_state->CommitIndex());
        printf("[PEER-G%d-N%d] ==============================\n", group_id_, local_node_id_);
      }
    }
  }

  // 打印本 group 的状态
  void PrintStatus() const {
    uint32_t term = 0;
    if (kv_node_ && kv_node_->GetRaftNode() && kv_node_->GetRaftNode()->getRaftState()) {
      term = kv_node_->GetRaftNode()->getRaftState()->CurrentTerm();
    }
    printf("[PEER-G%d-N%d] Status: %s(Term%u)\n",
           group_id_, local_node_id_, IsLeader() ? "LEADER" : "FOLLOWER", term);
  }

  // 打印 group 所有成员的状态（仅在 leader 节点调用）
  void PrintGroupMemberStatus() const {
    if (!IsLeader()) return;

    auto* raft_node = kv_node_ ? kv_node_->GetRaftNode() : nullptr;
    auto* raft_state = raft_node ? raft_node->getRaftState() : nullptr;
    if (!raft_state) return;

    printf("\n===== GROUP %u LEADER STATUS (Node %u) =====\n",
           group_id_, local_node_id_);
    printf("  Term: %u\n", raft_state->CurrentTerm());
    printf("  LastLogIndex: %u\n", raft_state->LastIndex());
    printf("  CommitIndex: %u\n", raft_state->CommitIndex());
    printf("  Members:\n");

    // 打印每个 follower 成员的状态
    for (auto peer_id : raft_state->peers_) {
      auto* peer = raft_state->raft_peer_[peer_id];
      if (peer) {
        printf("    Node %u: NextIndex=%u MatchIndex=%u\n",
               peer_id, peer->NextIndex(), peer->MatchIndex());
      }
    }
    printf("    Node %u: (self, leader)\n", local_node_id_);
    printf("=========================================\n");
    fflush(stdout);
  }

  // Poll 线程批量处理消息（每次最多 max_batch 条）
  // 返回 false 表示收到 MsgStop，Poll 线程应将此 FSM 从调度表移除
  bool HandleBatch(std::vector<PeerMsg>& batch) {
    // 统计消息类型
    int tick_count = 0;
    int other_count = 0;
    for (auto& m : batch) {
      if (std::holds_alternative<MsgTick>(m)) {
        tick_count++;
      } else {
        other_count++;
      }
    }

    // 心跳消息只打印摘要，不打印详细信息
    // if (tick_count > 0) {
    //   printf("[PEER-G%d-N%d] Tick x%d", group_id_, local_node_id_, tick_count);
    //   if (other_count > 0) {
    //     printf(" + %d other msg(s)", other_count);
    //   }
    //   printf("\n");
    // }

    // 处理所有消息
    for (auto& m : batch) {
      if (!HandleOne(m)) return false;
    }
    return true;
  }

  // 检查并打印 Leader 状态变化（每 5 秒调用一次）
  void CheckAndPrintLeaderStatus() {
    auto now = std::time(nullptr);
    if (now - last_status_print_time_ >= 5) {
      bool is_leader = IsLeader();
      if (is_leader != last_is_leader_ || last_is_leader_) {
        printf("[PEER-G%d-N%d] *** Status changed: %s ***\n",
               group_id_, local_node_id_, is_leader ? "LEADER" : "FOLLOWER");
      }
      last_status_print_time_ = now;
      last_is_leader_ = is_leader;
    }
  }

 private:
  bool HandleOne(PeerMsg& m) {
    return std::visit([this](auto& msg) -> bool {
      return Handle(msg);
    }, m);
  }

  // ---- MsgRaftMessage: 来自其他 peer 的 Raft 协议消息 ----
  bool Handle(MsgRaftMessage& m) {
    // [COMMENTED] MsgRaftMessage received log (runtime verbose)
    (void)m;
    return true;
  }

  // ---- MsgProposeShard: 协调者发来的分片，写入本 group 的 Raft log ----
  bool Handle(MsgProposeShard& m) {
    // [COMMENTED] MsgProposeShard verbose logs (runtime)
    // 检查是否是 leader
    if (kv_node_) {
      auto raft_node = kv_node_->GetRaftNode();
      if (raft_node) {
        // 将 shard bundle 序列化为字节，作为 Raft log entry 的 data 字段
        std::string serialized = SerializeBundle(m.bundle);

        // 调用 FlexRaft RaftNode 的 Propose（Leader 才会成功）
        raft::CommandData cmd{0, raft::Slice(serialized)};
        auto pr = raft_node->Propose(cmd);

        std::cerr << "[PEER-G" << static_cast<int>(group_id_) << "-N" << static_cast<int>(local_node_id_)
                  << "] PROPOSE_DONE is_leader=" << pr.is_leader
                  << " has_cb=" << (m.cb ? 1 : 0) << std::endl;

        if (m.cb) m.cb(pr.is_leader);
      } else {
        printf("[PEER-G%d-N%d] ERROR: RaftNode is null!\n", group_id_, local_node_id_);
        if (m.cb) m.cb(false);
      }
    } else {
      printf("[PEER-G%d-N%d] ERROR: kv_node is null!\n", group_id_, local_node_id_);
      if (m.cb) m.cb(false);
    }
    return true;
  }

  // ---- MsgTick: 定时驱动 Raft 状态机 ----
  bool Handle(MsgTick& m) {
    // RaftNode 已经有自己的 ticker 线程在驱动 Tick，这里不需要额外处理
    (void)m;
    return true;
  }

  // ---- MsgShardCommitAck: 远端 group 已 commit，更新跨组追踪器 ----
  bool Handle(MsgShardCommitAck& m) {
    printf("[PEER-G%d-N%d] Received commit ack from group %u for entry_id=%lu\n",
           group_id_, local_node_id_, m.from_group, m.entry_id);

    if (tracker_) {
      tracker_->OnLocalCommit(m.entry_id, m.from_group);
    }
    return true;
  }

  // ---- MsgLocalEncodeRequest: 请求执行局部编码（LRC 暂时禁用） ----
  bool Handle(MsgLocalEncodeRequest& m) {
    printf("[PEER-G%d-N%d] MsgLocalEncodeRequest received (LRC disabled)\n",
           group_id_, local_node_id_);
    (void)m;
    return true;
  }

  // ---- MsgLocalEncodeResult: 局部编码完成结果 ----
  bool Handle(MsgLocalEncodeResult& m) {
    printf("[PEER-G%d-N%d] MsgLocalEncodeResult received\n", group_id_, local_node_id_);
    (void)m;
    return true;
  }

  // ---- MsgRoutingTableSync: Gossip 路由表同步 ----
  bool Handle(MsgRoutingTableSync& m) {
    printf("[PEER-G%d-N%d] Received routing table sync (ttl=%d, entries=%zu)\n",
           group_id_, local_node_id_, m.ttl, m.routes.size());

    if (store_) {
      store_->MergeRoutingTable(m.routes, m.ttl - 1);
    }
    return true;
  }

  // ---- MsgRoutingTableUpdate: 更新路由表 ----
  bool Handle(MsgRoutingTableUpdate& m) {
    printf("[PEER-G%d-N%d] Received routing table update for stripe_id=%lu\n",
           group_id_, local_node_id_, m.stripe_id);

    if (store_) {
      store_->AddRouteEntry(m.stripe_id, m.entry_id, m.groups,
                            m.has_global_parities, m.global_parity_groups);
    }
    return true;
  }

  // ---- MsgWriteRequest: 客户端写入请求 ----
  bool Handle(MsgWriteRequest& m) {
    printf("\n[WRITE-IN][PEER-G%d-N%d] path=stripe(LocalPropose) enc=%s data_size=%zu\n",
           group_id_, local_node_id_,
           store_ ? EncodingModeName(static_cast<EncodingMode>(store_->GetClusterEncodingMode()))
                  : "?",
           m.data.size());
    fflush(stdout);
    printf("[PEER-G%d-N%d] ===== CLIENT WRITE REQUEST =====\n", group_id_, local_node_id_);
    printf("[PEER-G%d-N%d] data_size=%zu\n", group_id_, local_node_id_, m.data.size());

    // 调用 store 的 LocalProposeToGroup
    if (store_) {
      store_->LocalProposeToGroup(group_id_, m.data, [this](bool ok, EntryId eid) {
        printf("[PEER-G%d-N%d] Write completed: ok=%d entry_id=%lu\n",
               group_id_, local_node_id_, ok ? 1 : 0, eid);
      });
    }
    return true;
  }

  // ---- MsgGroupIdUpdate: 更新 group_id ----
  bool Handle(MsgGroupIdUpdate& m) {
    printf("\n[PEER-G%d-N%d] ===== GROUP ID UPDATE =====\n", group_id_, local_node_id_);
    printf("[PEER-G%d-N%d] old_group_id=%u new_group_id=%u\n",
           group_id_, local_node_id_, m.old_group_id, m.new_group_id);

    if (m.old_group_id != group_id_) {
      printf("[PEER-G%d-N%d] ERROR: old_group_id mismatch! Expected %u, got %u\n",
             group_id_, local_node_id_, group_id_, m.old_group_id);
      if (m.cb) m.cb(false);
      return true;
    }

    // 更新 group_id
    GroupId old_id = group_id_;
    group_id_ = m.new_group_id;

    printf("[PEER-G%d-N%d] Group ID updated: %u -> %u\n",
           old_id, local_node_id_, old_id, group_id_);

    if (m.cb) m.cb(true);
    return true;
  }

  // ---- MsgStop: 关闭该 PeerFsm ----
  bool Handle(MsgStop& m) {
    printf("[PEER-G%d-N%d] Received stop message, shutting down\n",
           group_id_, local_node_id_);
    (void)m;
    return false;
  }

  // 将 ShardBundle 打包成字节（简单 length-prefixed 序列化）
  //
  // 顺序（追加式，向后兼容）：
  //   entry_id, stripe_id, group_id, original_size
  //   local_data_shards [count, (len, bytes)*]
  //   local_parity      [len, bytes]
  //   global_parities   [count, (len, bytes)*]
  //   all_fragments     [count, (len, bytes)*]    （LRC 路径，RS 路径 count=0）
  //   placement         [count, (frag_id, kind, local_group, node_id)*]
  static std::string SerializeBundle(const ShardBundle& b) {
    std::string out;
    out.append(reinterpret_cast<const char*>(&b.entry_id), 8);
    out.append(reinterpret_cast<const char*>(&b.stripe_id), 8);
    out.append(reinterpret_cast<const char*>(&b.group_id), 4);
    out.append(reinterpret_cast<const char*>(&b.original_size), 8);

    uint32_t local_count = b.local_data_shards.size();
    out.append(reinterpret_cast<const char*>(&local_count), 4);
    for (const auto& s : b.local_data_shards) {
      uint32_t len = s.size();
      out.append(reinterpret_cast<const char*>(&len), 4);
      out.append(s.data(), s.size());
    }

    uint32_t parity_len = b.local_parity.size();
    out.append(reinterpret_cast<const char*>(&parity_len), 4);
    if (parity_len > 0) {
      out.append(b.local_parity.data(), parity_len);
    }

    uint32_t global_count = b.global_parities.size();
    out.append(reinterpret_cast<const char*>(&global_count), 4);
    for (const auto& s : b.global_parities) {
      uint32_t len = s.size();
      out.append(reinterpret_cast<const char*>(&len), 4);
      out.append(s.data(), s.size());
    }

    // ---- LRC 专用：all_fragments ----
    uint32_t all_count = b.all_fragments.size();
    out.append(reinterpret_cast<const char*>(&all_count), 4);
    for (const auto& s : b.all_fragments) {
      uint32_t len = s.size();
      out.append(reinterpret_cast<const char*>(&len), 4);
      out.append(s.data(), s.size());
    }

    // ---- LRC 专用：placement ----
    uint32_t place_count = b.placement.size();
    out.append(reinterpret_cast<const char*>(&place_count), 4);
    for (const auto& fp : b.placement) {
      int32_t frag_id     = fp.frag_id;
      int32_t kind        = static_cast<int32_t>(fp.kind);
      int32_t local_group = fp.local_group;
      int32_t node_id     = fp.node_id;
      out.append(reinterpret_cast<const char*>(&frag_id),     4);
      out.append(reinterpret_cast<const char*>(&kind),        4);
      out.append(reinterpret_cast<const char*>(&local_group), 4);
      out.append(reinterpret_cast<const char*>(&node_id),     4);
    }

    // ---- 编码模式：1 字节，末尾追加（向后兼容；旧 reader 解析到 placement 后停止） ----
    out.append(reinterpret_cast<const char*>(&b.encoding_mode), 1);

    return out;
  }

  raft::raft_group_id_t group_id_;
  raft::raft_node_id_t  local_node_id_;
  kv::KvServiceNode*    kv_node_;
  Mailbox<ApplyMsg>*    apply_mb_;
  CrossGroupTrackerInterface* tracker_;
  RaftRouterInterface*   router_;
  RaftStoreInterface*   store_;

  // 状态跟踪
  time_t last_status_print_time_;  // 上次打印状态的时间
  int64_t tick_count_;             // 心跳计数
  bool last_is_leader_;            // 上次的 Leader 状态
};

}  // namespace multiraft
