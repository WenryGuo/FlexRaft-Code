#pragma once

#include <cstdint>

#include "encoder.h"
#include "log_entry.h"
#include "raft_type.h"

namespace raft {

class RaftState;

// Optional hooks registered by multi-raft for stripe EC replication.
using StripeEncodeHook = bool (*)(RaftState* state, raft_index_t raft_index, Stripe* stripe);

using StripeIsCommandHook = bool (*)(const LogEntry& entry);

void RegisterStripeHooks(StripeIsCommandHook is_cmd, StripeEncodeHook encode);

bool StripeIsStripeCommand(const LogEntry& entry);
bool StripeTryEncodeReplication(RaftState* state, raft_index_t raft_index, Stripe* stripe);

}  // namespace raft
