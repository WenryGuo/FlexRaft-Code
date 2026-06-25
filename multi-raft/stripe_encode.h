#pragma once

#include <vector>

#include "encoding_mode.h"
#include "message.h"
#include "stripe_format.h"
#include "raft_type.h"

namespace multiraft {

struct StripeEncodeResult {
  StripeLogMeta meta;
  std::vector<raft::Slice> all_fragments;  // frag_id order
  bool ok = false;
};

bool EncodeStripePayload(EncodingMode mode, int N, GroupId group_id, EntryId eid,
                         const raft::Slice& data, StripeEncodeResult* out);

}  // namespace multiraft
