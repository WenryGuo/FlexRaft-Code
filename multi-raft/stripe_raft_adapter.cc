#include "stripe_raft_adapter.h"

#include <cstdio>
#include <map>
#include <vector>

#include "stripe_encode.h"
#include "stripe_format.h"
#include "stripe_hooks.h"
#include "raft.h"

namespace multiraft {

// ============================================================================
// [DEPRECATED] Stripe Hook Mechanism
//
// This entire file is deprecated. Encoding and packing is now done directly
// in RaftState::EncodeRaftEntryLrc() and RaftState::PackStripesToPeerKeyed().
//
// The hook mechanism (RegisterStripeHooks, StripeTryEncodeReplication) is no
// longer used. This file is kept for backward compatibility but will be removed
// in a future version.
// ============================================================================

namespace {

// [DEPRECATED] Check if entry is a StripeWriteCommand
bool IsStripeCmdHook(const raft::LogEntry& entry) {
  if (entry.Type() != raft::kNormal) return false;
  auto slice = entry.CommandData();
  return StripeWriteCommand::IsStripeCommand(slice.data(), slice.size());
}

// [DEPRECATED] Encode stripe using hook mechanism
// This function is no longer called - encoding is now done in RaftState
bool EncodeStripeHook(raft::RaftState* state, raft::raft_index_t raft_index,
                      raft::Stripe* stripe) {
  // This hook is no longer used. RaftState::EncodeRaftEntryLrc() handles encoding.
  (void)state;
  (void)raft_index;
  (void)stripe;
  return false;
}

}  // namespace

// [DEPRECATED] Register stripe hooks
// No longer needed - encoding/packing is now done in Raft layer
void RegisterStripeRaftHooks() {
  // Hook mechanism deprecated - encoding now done in RaftState::EncodeRaftEntryLrc
  // This function is kept for backward compatibility but does nothing
  static bool once = false;
  if (once) return;
  once = true;
  // raft::RegisterStripeHooks(IsStripeCmdHook, EncodeStripeHook);  // Disabled
  printf("[STRIPE-ADAPTER] [DEPRECATED] Hook mechanism disabled - encoding in Raft layer\n");
}

}  // namespace multiraft
