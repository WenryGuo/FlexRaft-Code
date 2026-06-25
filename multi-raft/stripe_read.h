#pragma once

#include "type.h"
#include "kv_node.h"

namespace multiraft {

bool TryStripeGet(kv::KvServiceNode* node, raft::raft_node_id_t my_id, int cluster_n,
                  const kv::GetValueRequest& req, kv::GetValueResponse* resp);

}  // namespace multiraft
