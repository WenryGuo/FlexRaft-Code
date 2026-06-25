#pragma once
#include <cstdint>
#include <string>

#include "RCF/ByteBuffer.hpp"
#include "log_entry.h"
#include "raft_struct.h"
#include "raft_type.h"

namespace raft {

// Fixed-width integer encoding (portable, little-endian)
// No alignment issues, works on both x86 and ARM
inline uint32_t DecodeFixed32(const char* src) {
  return (static_cast<uint32_t>(static_cast<unsigned char>(src[0])) |
          (static_cast<uint32_t>(static_cast<unsigned char>(src[1])) << 8) |
          (static_cast<uint32_t>(static_cast<unsigned char>(src[2])) << 16) |
          (static_cast<uint32_t>(static_cast<unsigned char>(src[3])) << 24));
}

inline void EncodeFixed32(char* dst, uint32_t value) {
  dst[0] = static_cast<char>(value & 0xff);
  dst[1] = static_cast<char>((value >> 8) & 0xff);
  dst[2] = static_cast<char>((value >> 16) & 0xff);
  dst[3] = static_cast<char>((value >> 24) & 0xff);
}

inline uint64_t DecodeFixed64(const char* src) {
  return (static_cast<uint64_t>(static_cast<unsigned char>(src[0])) |
          (static_cast<uint64_t>(static_cast<unsigned char>(src[1])) << 8) |
          (static_cast<uint64_t>(static_cast<unsigned char>(src[2])) << 16) |
          (static_cast<uint64_t>(static_cast<unsigned char>(src[3])) << 24) |
          (static_cast<uint64_t>(static_cast<unsigned char>(src[4])) << 32) |
          (static_cast<uint64_t>(static_cast<unsigned char>(src[5])) << 40) |
          (static_cast<uint64_t>(static_cast<unsigned char>(src[6])) << 48) |
          (static_cast<uint64_t>(static_cast<unsigned char>(src[7])) << 56));
}

inline void EncodeFixed64(char* dst, uint64_t value) {
  dst[0] = static_cast<char>(value & 0xff);
  dst[1] = static_cast<char>((value >> 8) & 0xff);
  dst[2] = static_cast<char>((value >> 16) & 0xff);
  dst[3] = static_cast<char>((value >> 24) & 0xff);
  dst[4] = static_cast<char>((value >> 32) & 0xff);
  dst[5] = static_cast<char>((value >> 40) & 0xff);
  dst[6] = static_cast<char>((value >> 48) & 0xff);
  dst[7] = static_cast<char>((value >> 56) & 0xff);
}

// Serialize LogEntry fields for Apply (replaces ECLogPayload)
// Format: [magic(4) + version(4) + kind(1)] + payload
// For kLrcFragments kind: user_key + ChunkInfo(k/l/r/lrc_group) + placements + fragments + original_size(8)
std::string SerializeForApply(const LogEntry& entry, const std::string& user_key, size_t original_size = 0);

class Serializer {
 public:
  static Serializer NewSerializer();

 public:
  void Serialize(const LogEntry *entry, RCF::ByteBuffer *buffer);
  void Deserialize(const RCF::ByteBuffer *buffer, LogEntry *entry);

  void Serialize(const RequestVoteArgs *args, RCF::ByteBuffer *buffer);
  void Deserialize(const RCF::ByteBuffer *buffer, RequestVoteArgs *args);

  void Serialize(const RequestVoteReply *reply, RCF::ByteBuffer *buffer);
  void Deserialize(const RCF::ByteBuffer *buffer, RequestVoteReply *reply);

  void Serialize(const AppendEntriesArgs *args, RCF::ByteBuffer *buffer);
  void Deserialize(const RCF::ByteBuffer *buffer, AppendEntriesArgs *args);

  void Serialize(const AppendEntriesReply *reply, RCF::ByteBuffer *buffer);
  void Deserialize(const RCF::ByteBuffer *buffer, AppendEntriesReply *reply);

  void Serialize(const RequestFragmentsArgs *args, RCF::ByteBuffer *buffer);
  void Deserialize(const RCF::ByteBuffer *buffer, RequestFragmentsArgs *args);

  void Serialize(const RequestFragmentsReply *reply, RCF::ByteBuffer *buffer);
  void Deserialize(const RCF::ByteBuffer *buffer, RequestFragmentsReply *reply);

  size_t getSerializeSize(const LogEntry &entry);
  size_t getSerializeSize(const RequestVoteArgs &args);
  size_t getSerializeSize(const RequestVoteReply &reply);
  size_t getSerializeSize(const AppendEntriesArgs &args);
  size_t getSerializeSize(const AppendEntriesReply &reply);
  size_t getSerializeSize(const RequestFragmentsArgs &args);
  size_t getSerializeSize(const RequestFragmentsReply &reply);

  // Group notification serialization
  void Serialize(const GroupNotificationArgs *args, RCF::ByteBuffer *buffer);
  void Deserialize(const RCF::ByteBuffer *buffer, GroupNotificationArgs *args);
  void Serialize(const GroupNotificationReply *reply, RCF::ByteBuffer *buffer);
  void Deserialize(const RCF::ByteBuffer *buffer, GroupNotificationReply *reply);
  size_t getSerializeSize(const GroupNotificationArgs &args);
  size_t getSerializeSize(const GroupNotificationReply &reply);

  // Put/Parse a slice in prefix-length format at specified buf position and
  // returns with a pointer to the next position
  char *PutPrefixLengthSlice(const Slice &slice, char *buf);
  const char *ParsePrefixLengthSlice(const char *buf, Slice *slice);
  const char *ParsePrefixLengthSliceWithBound(const char *buf, size_t len, Slice *slice);

  char *serialize_logentry_helper(const LogEntry *entry, char *dst);
  const char *deserialize_logentry_helper(const char *src, LogEntry *entry);

  // Parse serialized data within [src, src + len) into a LogEntry, if the parse
  // successed, returns the next position to parse; otherwise returns nullptr.
  const char *deserialize_logentry_withbound(const char *src, size_t len, LogEntry *entry);
};
}  // namespace raft
