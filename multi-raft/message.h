#pragma once
// message.h  —  所有在 Actor 之间流转的消息类型
//
// 修改说明（2026-03-28）：
//   1. 添加路由表相关消息（路由表同步、Gossip 传播）
//   2. 添加局部编码请求/完成消息
//   3. 添加完整的日志打印
//   4. 优化消息结构，支持完整的 Multi-Raft + LRC 流程
//
// 设计原则（对应 TiKV BatchSystem）:
//   PeerFsm   ↔  处理 Raft 协议消息 (AppendEntries / Vote / Heartbeat)
//   ApplyFsm  ↔  将已 commit 的日志条目应用到 KV 状态机
//   两类 FSM 各有一个 Mailbox，Poll 线程批量拉取后顺序执行，
//   不再是"一个 group 一个线程"。

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "encoder.h"   // Slice — FlexRaft 原有类型
#include "raft_type.h" // raft::Slice

namespace multiraft {

using GroupId  = uint32_t;
using EntryId  = uint64_t;
using StripeId = uint64_t;
using Slice    = raft::Slice;

// ============================================================================
//  路由表相关类型 — 支持 LRC Local/Global Repair 的互补分组关联
// ============================================================================

// GroupTopology: 记录条带对应的完整分组拓扑信息
struct GroupTopology {
  StripeId stripe_id;                      // 条带 ID
  EntryId  entry_id;                       // 对应的日志 entry ID
  
  // 该条带分布的所有 Local Group（对应 LRC 的 k 个数据分片 + r 个全局校验）
  std::vector<GroupId> data_groups;          // k 个数据分片所在的 Local Group
  std::vector<GroupId> global_parity_groups; // r 个全局校验所在的 Local Group
  
  // 关键：互补分组关联（用于 Local/Global Repair）
  // initiator_id -> 该 initiator 生成的 l 个互补分组
  // 示例：N=7, l=2
  //   若 stripe_id 的 data_groups = [G0, G3, G5, G8]
  //   对应的 initiators = [0, 3, 5, 8] (group_id / N)
  //   每个 initiator 对应 l 个互补分组
  struct ComplementarySet {
    int initiator_id;                      // 发起节点 ID (0 <= initiator < N)
    std::vector<GroupId> complementary_groups; // 该 initiator 生成的 l 个分组
    // 示例：initiator=0 生成 [G0, G1]，其中 G0 包含 initiator 自身，G1 不包含
  };
  std::vector<ComplementarySet> complementary_sets;
  
  // 辅助字段
  int local_k;           // 每个 Local Group 的数据块数
  int r;                 // 全局校验数
  bool has_global_parities;
  
  std::string ToString() const {
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "Stripe%lu: entry=%lu data_groups=[", stripe_id, entry_id);
    for (size_t i = 0; i < data_groups.size(); ++i) {
      snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%u%s",
               data_groups[i], i < data_groups.size() - 1 ? "," : "");
    }
    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "] comp_sets=[");
    for (size_t i = 0; i < complementary_sets.size(); ++i) {
      const auto& cs = complementary_sets[i];
      snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "{i=%d,g=[", cs.initiator_id);
      for (size_t j = 0; j < cs.complementary_groups.size(); ++j) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%u%s",
                 cs.complementary_groups[j], j < cs.complementary_groups.size() - 1 ? "," : "");
      }
      snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "]}%s",
               i < complementary_sets.size() - 1 ? "," : "");
    }
    strncat(buf, "]", sizeof(buf) - strlen(buf) - 1);
    return buf;
  }
};

// 路由表：stripe_id -> GroupTopology
using RoutingTable = std::unordered_map<StripeId, GroupTopology>;

// ============================================================================
//  FragmentPlacement — 单个 fragment 的放置记录（LRC 正交放置使用）
//
//  仅依赖标准库与基本类型，定义在 message.h 内便于 ShardBundle 直接引用，
//  避免把 lrc_encoder.h / lrc_group_builder.h 拉进 message.h 的依赖链。
// ============================================================================
struct FragmentPlacement {
  enum Kind {
    kData          = 0,
    kLocalParity   = 1,
    kGlobalParity  = 2,
  };

  int  frag_id      = 0;   // 条带内全局编号 [0, k+l+r)
  int  local_group  = 0;   // 该 fragment 所属互补分区 [0, l)
  int  node_id      = 0;   // 物理节点 ID
  Kind kind         = kData;

  std::string KindString() const {
    switch (kind) {
      case kData:         return "data";
      case kLocalParity:  return "local_parity";
      case kGlobalParity: return "global_parity";
    }
    return "?";
  }

  std::string ToString() const {
    char buf[128];
    snprintf(buf, sizeof(buf), "frag=%d kind=%s LG=%d node=%d",
             frag_id, KindString().c_str(), local_group, node_id);
    return buf;
  }
};

// ============================================================================
//  Shard: 一条 LRC 编码后分配给某个 group 的数据单元
//
//  字段说明：
//    - local_data_shards / local_parity / global_parities：
//        RS 路径下仅 local_data_shards 单元素；LRC 路径下分别承载该条带的
//        k 个数据分片、l 个局部校验、r 个全局校验。
//    - all_fragments / placement：LRC 路径下额外的"打平后"全量 fragment
//        与每个 fragment 的正交放置记录。RS 路径下为空。
// ============================================================================
struct ShardBundle {
  EntryId              entry_id;
  StripeId             stripe_id;
  GroupId              group_id;
  std::vector<Slice>   local_data_shards;    // local_k 个数据分片
  Slice                local_parity;          // 1 个局部校验分片
  std::vector<Slice>   global_parities;       // 仅 group 0 携带，其余为空
  size_t               original_size;         // 原始数据字节数

  // ----- LRC / RS-3F 模式专用 -----
  std::vector<Slice>             all_fragments;  // 与 placement 等长；RS-F 模式留空
  std::vector<FragmentPlacement> placement;       // 每个 fragment 的正交放置记录

  // ----- 编码模式（与 multiraft::EncodingMode 一一对应；序列化时占 1 字节） -----
  uint8_t encoding_mode = 0;  // 0=RS_F, 1=RS_3F, 2=LRC
};

// ============================================================================
//  PeerMsg: 发往 PeerFsm 的消息
// ============================================================================

struct MsgRaftMessage {
  // Raft 协议消息（AppendEntries / Vote / Heartbeat）
  std::string payload;
};

struct MsgProposeShard {
  // 协调者发来的 LRC 分片，写入本 group 的 Raft log
  ShardBundle bundle;
  std::function<void(bool)> cb;      // propose 结果回调
};

struct MsgTick {};                   // 定时驱动 Raft 状态机

struct MsgShardCommitAck {           // 远端 group 已 commit，更新跨组追踪器
  EntryId entry_id;
  GroupId from_group;
};

struct MsgStop {};                   // 关闭该 PeerFsm

// ---- 新增消息类型（2026-03-28）----

struct MsgLocalEncodeRequest {
  // Leader 请求某个 group 执行局部编码
  EntryId              entry_id;
  StripeId             stripe_id;
  GroupId              from_group;   // 请求来自哪个 group（协调者）
  GroupId              target_group;  // 执行局部编码的目标 group
  std::vector<Slice>   data_frags;    // 该 group 的数据分片（来自全局编码）
  std::function<void(bool, ShardBundle*)> cb;  // 编码完成回调
};

struct MsgLocalEncodeResult {
  // group 执行完局部编码后，返回结果给协调者
  EntryId              entry_id;
  StripeId             stripe_id;
  GroupId              group_id;      // 执行编码的 group
  bool                 success;       // 是否成功
  ShardBundle          bundle;        // 编码后的 shard bundle
};

struct MsgRoutingTableSync {
  // Gossip 协议：路由表同步消息
  RoutingTable         routes;
  int                  ttl;           // 剩余跳数
};

struct MsgRoutingTableUpdate {
  // 更新本地路由表
  StripeId             stripe_id;
  EntryId              entry_id;
  std::vector<GroupId> groups;
  bool                 has_global_parities;
  std::vector<GroupId> global_parity_groups;
};

struct MsgWriteRequest {
  // 客户端写入请求（用于 RaftStore 入口）
  Slice                data;
  std::function<void(bool, EntryId)> cb;  // 回调：成功/失败 + entry_id
};

struct MsgGroupIdUpdate {
  // 更新 group_id 的消息
  GroupId              old_group_id;
  GroupId              new_group_id;
  std::function<void(bool)> cb;  // 更新结果回调
};

using PeerMsg = std::variant<
    MsgRaftMessage,
    MsgProposeShard,
    MsgTick,
    MsgShardCommitAck,
    MsgStop,
    MsgLocalEncodeRequest,
    MsgLocalEncodeResult,
    MsgRoutingTableSync,
    MsgRoutingTableUpdate,
    MsgWriteRequest,
    MsgGroupIdUpdate
>;

// ============================================================================
//  ApplyMsg: 发往 ApplyFsm 的消息
// ============================================================================
struct MsgApplyCommitted {
  EntryId              entry_id;
  StripeId             stripe_id;
  GroupId              group_id;
  std::vector<uint8_t> data;         // 打包好的 shard 字节
  std::function<void()> on_applied;  // 写入 RocksDB 后回调
};

struct MsgApplyStop {};

using ApplyMsg = std::variant<MsgApplyCommitted, MsgApplyStop>;

}  // namespace multiraft
