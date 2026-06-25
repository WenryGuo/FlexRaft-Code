#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "encoding_mode.h"
#include "message.h"

namespace multiraft {

inline constexpr char kStripeMagic[4] = {'S', 'T', 'R', 'I'};
inline constexpr char kPackedFragMagic[4] = {'M', 'F', 'R', 'G'};

inline std::string FragDbKey(GroupId group_id, StripeId stripe_id, int frag_id) {
  return "__frag__/" + std::to_string(group_id) + "/" + std::to_string(stripe_id) + "/" +
         std::to_string(frag_id);
}

// Key format: "__stripe_meta__/{group_id}/{user_key}"
// Group ID is included to prevent cross-group key collisions in multi-group deployments
inline std::string StripeMetaDbKey(GroupId group_id, const std::string& user_key) {
  return "__stripe_meta__/" + std::to_string(group_id) + "/" + user_key;
}

enum class ApplyPayloadKind : uint8_t {
  kLegacy = 0,
  kStripePacked = 1,
  kStripeCommand = 2,
  kLrcFragments = 3,  // 新增
};

struct StripeWriteCommand {
  GroupId group_id = 0;
  std::string user_key;
  std::string user_value;
  EncodingMode encoding_mode = EncodingMode::kLrc;

  std::string Serialize() const;
  static bool Deserialize(const char* data, size_t len, StripeWriteCommand* out);
  static bool IsStripeCommand(const char* data, size_t len);
};

struct StripeLogMeta {
  std::string user_key;
  StripeId stripe_id = 0;
  EntryId entry_id = 0;
  GroupId group_id = 0;
  size_t original_size = 0;
  EncodingMode encoding_mode = EncodingMode::kLrc;
  int k = 0;
  int l = 0;
  int r = 0;
  int m = 0;  // RS parity count (RS modes)
  std::vector<FragmentPlacement> placement;

  std::string Serialize() const;
  static bool Deserialize(const char* data, size_t len, StripeLogMeta* out);
};

struct PackedNodeFragment {
  int frag_id = 0;
  std::string bytes;
};

// Pack multiple fragments assigned to one node into one blob (for one LogEntry).
std::string PackNodeFragments(const StripeLogMeta& meta,
                              const std::vector<PackedNodeFragment>& frags);

bool UnpackNodeFragments(const char* data, size_t len, StripeLogMeta* meta_out,
                         std::vector<PackedNodeFragment>* frags_out);

// ============================================================================
//  Merged Storage Interface — Single IO reads meta + local fragments
//
//  Storage Layout:
//    Primary Key:   "__stripe/{group_id}/{user_key}" → Value: meta + local frags
//    Frag Index:    "__frag_idx/{group_id}/{frag_id}" → Value: primary key
// ============================================================================

// Make primary storage key for a stripe
inline std::string MakeStripePrimaryKey(GroupId group_id, const std::string& user_key) {
  return "__stripe/" + std::to_string(group_id) + "/" + user_key;
}

// Make fragment index key for frag_id lookup (used for repair scenarios)
inline std::string MakeFragIndexKey(GroupId group_id, int frag_id) {
  return "__frag_idx/" + std::to_string(group_id) + "/" + std::to_string(frag_id);
}

// Magic for merged storage format
inline constexpr char kMergedFragMagic[4] = {'M', 'F', 'S', 'T'};

// Serialize merged storage: meta + local fragments in one value
std::string SerializeMergedFragStore(const StripeLogMeta& meta,
                                     const std::vector<PackedNodeFragment>& frags);

// Deserialize merged storage
bool DeserializeMergedFragStore(const char* data, size_t len,
                                  StripeLogMeta* meta_out,
                                  std::vector<PackedNodeFragment>* frags_out);

}  // namespace multiraft
