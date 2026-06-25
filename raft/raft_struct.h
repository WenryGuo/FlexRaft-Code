#pragma once
#include <string>
#include <vector>

#include "log_entry.h"
#include "raft_type.h"

namespace raft {
struct RequestVoteArgs {
  raft_term_t term;

  raft_node_id_t candidate_id;

  raft_index_t last_log_index;

  raft_term_t last_log_term;

  raft_group_id_t group_id = 0;

  // Candidate's priority (higher = better). Included in RequestVote for logging.
  int candidate_priority;
};

struct RequestVoteReply {
  raft_term_t term;

  int vote_granted;

  raft_node_id_t reply_id;

  // group_id is used to route the reply to the correct RaftState in multi-raft
  raft_group_id_t group_id = 0;
};

struct AppendEntriesArgs {
  // Leader's term when sending this AppendEntries RPC.
  raft_term_t term;

  // group_id is used by RaftUnifiedRPCService to route to the right RaftState.
  // Placed immediately after term so it is covered by kAppendEntriesArgsHdrSize=36.
  raft_group_id_t group_id = 0;

  // The leader's identifier
  raft_node_id_t leader_id;

  raft_index_t prev_log_index;
  raft_term_t prev_log_term;
  raft_encoding_param_t prev_k;

  raft_index_t leader_commit;

  int64_t entry_cnt;
  std::vector<LogEntry> entries;
};

struct AppendEntriesReply {
  // The raft term of the server when processing one AppendEntries RPC call.
  // Used to update the leader's term
  raft_term_t term;

  // Denote if the follower successfully append specified log entries to its
  // own log manager. Return 1 if append is successful, otherwise returns 0
  int success;

  // The next raft index the raft peer wants the leader to send. If success is
  // true, the expect_index is prev_log_index + entry_cnt + 1; otherwise it is
  // the first index that differs from the leader
  raft_index_t expect_index;

  // The raft node id of the server that makes this reply
  raft_node_id_t reply_id;

  // group_id is used to route the reply to the correct RaftState in multi-raft
  raft_group_id_t group_id = 0;

  uint32_t padding;

  int chunk_info_cnt;
  std::vector<ChunkInfo> chunk_infos;
};

struct RequestFragmentsArgs {
  // group_id is used by RaftUnifiedRPCService to route to the right RaftState
  raft_group_id_t group_id;

  // The term when leader(Or pre-leader) sends out this RequestFragments RPC to
  // collect fragments in order to recover the original entry contents
  raft_term_t term;

  raft_node_id_t leader_id;

  // [start, last] specifies the range of fragments the pre-leader process
  // requires.
  raft_index_t start_index, last_index;
};

struct RequestFragmentsReply {
  raft_node_id_t reply_id;

  raft_term_t term;

  // If there is some replied fragment entries, the start index of it, it should
  // be exactly the same index as that is contained in corresponding
  // RequestFragmentsArgs
  raft_index_t start_index;

  // Request Fragments may fail, e.g. If requested server has higher term, which
  // invalidates the leadership of current leader
  int success;

  int entry_cnt;
  std::vector<LogEntry> fragments;
};

// A struct that indicates the command specified by user of the raft cluster
struct CommandData {
  int start_fragment_offset;
  // The ownership of data contained in this command_data is handled to Raft, if
  // you call RaftState->Process(..)
  Slice command_data;
};

enum {
  // kAppendEntriesArgsHdrSize = sizeof(AppendEntriesArgs header fields, with padding)
  // = sizeof(term) + sizeof(group_id) + sizeof(leader_id) + sizeof(prev_log_index)
  //   + sizeof(prev_log_term) + sizeof(prev_k) + sizeof(leader_commit) + sizeof(entry_cnt)
  // = 4+4+4+4+4+4+4+8 = 36, but struct pads to 40 (leader_commit[4] + entry_cnt[8] -> 8-align)
  kAppendEntriesArgsHdrSize = 40,
  kAppendEntriesReplyHdrSize = sizeof(raft_term_t) + sizeof(int) + sizeof(raft_index_t) +
                               sizeof(raft_node_id_t) + sizeof(raft_group_id_t) +
                               sizeof(uint32_t) + sizeof(int),
  kRequestFragmentsReplyHdrSize =
      sizeof(raft_node_id_t) + sizeof(raft_term_t) + sizeof(raft_index_t) + sizeof(int) * 2,
};

// ========================================================================
// Multi-Raft Group Notification Structures
// ========================================================================

// 分组成员信息
struct GroupMemberInfo {
  raft_node_id_t node_id;
  std::string raft_rpc_addr;   // 格式: "ip:port"
  std::string kv_rpc_addr;     // 格式: "ip:port"
  std::string raft_log_filename;
  std::string kv_dbname;
};

// 分组信息
struct GroupInfo {
  raft_group_id_t group_id;
  int initiator_id;
  int initiator_generated_index;
  int complementary_group_count;
  std::vector<raft_group_id_t> complementary_group_indices;
  std::vector<GroupMemberInfo> members;
};

// 分组通知参数（用于 RPC 通知）
struct GroupNotificationArgs {
  raft_node_id_t source_node_id;
  int total_groups;
  std::vector<GroupInfo> groups;
};

// 分组通知响应
struct GroupNotificationReply {
  int success;
  raft_node_id_t reply_node_id;
};

}  // namespace raft
