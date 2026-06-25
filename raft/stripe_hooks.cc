#include "stripe_hooks.h"

#include "raft.h"

namespace raft {

static StripeIsCommandHook g_is_stripe_cmd = nullptr;
static StripeEncodeHook g_stripe_encode = nullptr;

void RegisterStripeHooks(StripeIsCommandHook is_cmd, StripeEncodeHook encode) {
  g_is_stripe_cmd = is_cmd;
  g_stripe_encode = encode;
}

bool StripeIsStripeCommand(const LogEntry& entry) {
  if (entry.Type() != kNormal) return false;
  if (g_is_stripe_cmd) return g_is_stripe_cmd(entry);
  return false;
}

bool StripeTryEncodeReplication(RaftState* state, raft_index_t raft_index, Stripe* stripe) {
  if (!g_stripe_encode) return false;
  return g_stripe_encode(state, raft_index, stripe);
}

}  // namespace raft
