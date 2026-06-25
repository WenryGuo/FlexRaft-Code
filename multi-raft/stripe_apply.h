#pragma once

#include <string>

#include "encoding_mode.h"
#include "kv_node.h"
#include "message.h"
#include "stripe_format.h"

namespace multiraft {

// Apply a packed node fragment log entry (follower path).
bool ApplyStripePacked(kv::KvServiceNode* kv_node, GroupId group_id, raft::raft_node_id_t node_id,
                       const std::string& payload);

// Apply a stripe write command from leader kNormal log (leader path).
// [DEPRECATED] No longer needed with peer-keyed mode.
bool ApplyStripeCommand(kv::KvServiceNode* kv_node, GroupId group_id,
                        raft::raft_node_id_t node_id, EncodingMode mode, int cluster_n,
                        EntryId entry_id, const std::string& cmd_bytes);

}  // namespace multiraft
