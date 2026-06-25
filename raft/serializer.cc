#include "serializer.h"

#include <cstring>

#include "RCF/ByteBuffer.hpp"
#include "log_entry.h"
#include "raft_struct.h"
#include "raft_type.h"

namespace raft {
Serializer Serializer::NewSerializer() { return Serializer(); }

char *Serializer::serialize_logentry_helper(const LogEntry *entry, char *dst) {
  // Simply copy the entire LogEntry structure
  // NOTE: This copies the vector internal pointers, but that's OK because
  // we explicitly serialize the vector data separately below.
  // The receiver will need to handle this carefully during deserialization.
  std::memcpy(dst, entry, sizeof(LogEntry));
  dst += sizeof(LogEntry);
  dst = PutPrefixLengthSlice(entry->NotEncodedSlice(), dst);
  fprintf(stderr, "[DEBUG-SERIALIZE] not_encoded: size=%zu\n", entry->NotEncodedSlice().size());
  dst = PutPrefixLengthSlice(entry->FragmentSlice(), dst);
  fprintf(stderr, "[DEBUG-SERIALIZE] frag: size=%zu\n", entry->FragmentSlice().size());

  // Serialize LRC fragments_ (std::vector<Slice>) with frag_ids
  {
    uint32_t frag_count = static_cast<uint32_t>(entry->Fragments().size());
    std::memcpy(dst, &frag_count, sizeof(frag_count));
    dst += sizeof(frag_count);

    // Get frag_ids (parallel to fragments)
    const auto& frag_ids = entry->FragIds();

    for (size_t i = 0; i < entry->Fragments().size(); ++i) {
      const auto& frag = entry->Fragments()[i];
      fprintf(stderr, "[DEBUG-SERIALIZE] frag[%zu]: size=%zu\n", i, frag.size());
      // Get frag_id: use FragIds() if available, otherwise use index
      int32_t frag_id = (i < frag_ids.size()) ? frag_ids[i] : static_cast<int32_t>(i);
      std::memcpy(dst, &frag_id, sizeof(frag_id));
      dst += sizeof(frag_id);
      dst = PutPrefixLengthSlice(frag, dst);
    }
  }

  // Serialize placement_ (std::vector<FragmentPlacement>)
  {
    uint32_t placement_count = static_cast<uint32_t>(entry->Placement().size());
    fprintf(stderr, "[DEBUG-SERIALIZE] placement_count=%u\n", placement_count);
    std::memcpy(dst, &placement_count, sizeof(placement_count));
    dst += sizeof(placement_count);
    for (const auto& p : entry->Placement()) {
      fprintf(stderr, "[DEBUG-SERIALIZE] placement: frag_id=%d local_group=%d node_id=%d kind=%d\n",
              p.frag_id, p.local_group, p.node_id, static_cast<int>(p.kind));
      std::memcpy(dst, &p.frag_id, sizeof(p.frag_id));
      dst += sizeof(p.frag_id);
      std::memcpy(dst, &p.local_group, sizeof(p.local_group));
      dst += sizeof(p.local_group);
      std::memcpy(dst, &p.node_id, sizeof(p.node_id));
      dst += sizeof(p.node_id);
      uint32_t kind = static_cast<uint32_t>(p.kind);
      std::memcpy(dst, &kind, sizeof(kind));
      dst += sizeof(kind);
    }
  }

  return dst;
}

const char *Serializer::deserialize_logentry_helper(const char *src, LogEntry *entry) {
  // Step 1: Use memcpy for the entire LogEntry structure
  // NOTE: This copies the vector internal pointers, which point to heap memory
  // that will be freed when the serialized buffer is freed. We handle this below.

  // First, destroy any existing state in entry
  entry->~LogEntry();

  // Copy the LogEntry structure
  std::memcpy(entry, src, sizeof(LogEntry));

  // Step 2: CRITICAL - Clear the vectors in the copied entry to avoid double-free
  // The memcpy copied the vector internal pointers, but the actual data was
  // allocated separately and will be freed with the serialized buffer.
  // By clearing (not freeing) these vectors, we avoid double-free.
  // The fragments will be rebuilt by SetFragments() below.
  entry->fragments_.clear();
  entry->placement_.clear();
  // NOTE: payload_ no longer exists - removed

  // Step 3: Skip past the serialized scalar fields that we already copied
  // and the serialized vector data
  src += sizeof(LogEntry);

  // Step 4: Deserialize the Slice fields (these have their data copied from buffer)
  Slice not_encoded, frag;
  src = ParsePrefixLengthSlice(src, &not_encoded);
  src = ParsePrefixLengthSlice(src, &frag);

  // Set the slices properly
  entry->SetNotEncodedSlice(not_encoded);
  entry->SetFragmentSlice(frag);

  if (entry->Type() == kNormal) {
    entry->SetCommandData(not_encoded);
  }

  // Step 5: Deserialize LRC fragments_ with frag_ids
  // New format: [frag_id(4) + size(4) + data(N)] per fragment
  // IMPORTANT: The serialized fragments were allocated by PutPrefixLengthSlice
  // and their pointers were copied by memcpy. The original serialized entry's
  // destructor would try to delete[] them, causing double-free.
  // By setting new fragments here, we take ownership of NEW memory.
  std::vector<Slice> frags;
  std::vector<int> frag_ids;
  {
    uint32_t frag_count = 0;
    std::memcpy(&frag_count, src, sizeof(frag_count));
    src += sizeof(frag_count);
    
    // Limit frag_count to prevent buffer overflow
    const uint32_t kMaxFragCount = 1024;
    if (frag_count > kMaxFragCount) {
      fprintf(stderr, "[SERIALIZER] WARNING: frag_count=%u exceeds limit, clamping to %u\n", frag_count, kMaxFragCount);
      frag_count = kMaxFragCount;
    }
    
    frags.reserve(frag_count);
    frag_ids.reserve(frag_count);
    for (uint32_t i = 0; i < frag_count; ++i) {
      // Read frag_id (4 bytes)
      int32_t frag_id = 0;
      std::memcpy(&frag_id, src, sizeof(frag_id));
      src += sizeof(frag_id);
      frag_ids.push_back(frag_id);
      // Read fragment data: [size(8) + data(N)] — size prefix is sizeof(size_t)
      // to match PutPrefixLengthSlice() in serialize_logentry_helper().
      size_t frag_size = 0;
      std::memcpy(&frag_size, src, sizeof(frag_size));
      src += sizeof(frag_size);
      if (frag_size > 16 * 1024 * 1024) {  // Sanity check: max 16MB per fragment
        fprintf(stderr, "[SERIALIZER] ERROR: frag_size=%zu too large\n", frag_size);
        return nullptr;
      }
      char* frag_data = new char[frag_size];
      std::memcpy(frag_data, src, frag_size);
      frags.push_back(Slice::Take(frag_data, frag_size));
      src += frag_size;
    }
    entry->SetFragments(frags);  // This performs deep copy, taking ownership
    entry->SetFragIds(frag_ids); // Restore the correct frag_ids
  }

  // Step 6: Deserialize placement_
  std::vector<FragmentPlacement> placement;
  {
    uint32_t placement_count = 0;
    std::memcpy(&placement_count, src, sizeof(placement_count));
    src += sizeof(placement_count);
    
    // Limit placement_count to prevent buffer overflow
    const uint32_t kMaxPlacementCount = 256;
    if (placement_count > kMaxPlacementCount) {
      fprintf(stderr, "[SERIALIZER] WARNING: placement_count=%u exceeds limit, clamping to %u\n", placement_count, kMaxPlacementCount);
      placement_count = kMaxPlacementCount;
    }
    
    placement.reserve(placement_count);
    for (uint32_t i = 0; i < placement_count; ++i) {
      FragmentPlacement p;
      std::memcpy(&p.frag_id, src, sizeof(p.frag_id));
      src += sizeof(p.frag_id);
      std::memcpy(&p.local_group, src, sizeof(p.local_group));
      src += sizeof(p.local_group);
      std::memcpy(&p.node_id, src, sizeof(p.node_id));
      src += sizeof(p.node_id);
      uint32_t kind = 0;
      std::memcpy(&kind, src, sizeof(kind));
      src += sizeof(kind);
      p.kind = static_cast<FragmentPlacement::Kind>(kind);
      placement.push_back(p);
    }
    entry->SetPlacement(placement);
  }

  return src;
}

const char *Serializer::deserialize_logentry_withbound(const char *src, size_t len,
                                                       LogEntry *entry) {
  fprintf(stderr, "[DEBUG] deserialize_logentry_withbound: len=%zu sizeof(LogEntry)=%zu\n",
          len, sizeof(LogEntry));
  if (len < sizeof(LogEntry)) {
    fprintf(stderr, "[DEBUG] FAIL: len < sizeof(LogEntry)\n");
    return nullptr;
  }

  // Step 1: Destroy any existing state in entry
  entry->~LogEntry();

  // Step 2: Copy scalar fields one by one (NOT the vector fields)
  std::memcpy(&entry->term,                   src + offsetof(LogEntry, term),                   sizeof(entry->term));
  std::memcpy(&entry->index,                  src + offsetof(LogEntry, index),                  sizeof(entry->index));
  std::memcpy(&entry->type,                   src + offsetof(LogEntry, type),                   sizeof(entry->type));
  std::memcpy(&entry->chunk_info,             src + offsetof(LogEntry, chunk_info),             sizeof(entry->chunk_info));
  std::memcpy(&entry->start_fragment_offset,   src + offsetof(LogEntry, start_fragment_offset),   sizeof(entry->start_fragment_offset));
  std::memcpy(&entry->command_size_,           src + offsetof(LogEntry, command_size_),           sizeof(entry->command_size_));
  // Note: fragments_ and placement_ (vectors) are left empty - will be repopulated below
  // Note: command_data_, not_encoded_slice_, fragment_slice_ (owned Slices) left null

  // Step 3: Advance past the serialized LogEntry header
  src += sizeof(LogEntry);
  len -= sizeof(LogEntry);

  Slice not_encoded, frag;
  auto tmp_src = src;
  fprintf(stderr, "[DEBUG] Deserialize slices: len=%zu\n", len);
  src = ParsePrefixLengthSliceWithBound(src, len, &not_encoded);
  if (src == nullptr) {
    fprintf(stderr, "[DEBUG] FAIL: ParsePrefixLengthSlice not_encoded failed\n");
    return nullptr;
  }
  fprintf(stderr, "[DEBUG] not_encoded parsed: size=%zu, len=%zu remaining\n",
          not_encoded.size(), len - (src - tmp_src));
  len -= (src - tmp_src);
  tmp_src = src;
  src = ParsePrefixLengthSliceWithBound(src, len, &frag);
  if (src == nullptr) {
    fprintf(stderr, "[DEBUG] FAIL: ParsePrefixLengthSlice frag failed\n");
    return nullptr;
  }
  fprintf(stderr, "[DEBUG] frag parsed: size=%zu, len=%zu remaining\n",
          frag.size(), len - (src - tmp_src));
  len -= (src - tmp_src);
  fprintf(stderr, "[DEBUG] After slices: src is at frag_count position, len=%zu\n", len);
  fprintf(stderr, "[DEBUG] Bytes at frag_count: %02x %02x %02x %02x (should be 02 00 00 00 = 2)\n",
          (unsigned char)src[0], (unsigned char)src[1], (unsigned char)src[2], (unsigned char)src[3]);

  entry->SetNotEncodedSlice(not_encoded);
  entry->SetFragmentSlice(frag);

  if (entry->Type() == kNormal) {
    entry->SetCommandData(not_encoded);
  }

  // Deserialize LRC fragments_ (std::vector<Slice>) with frag_ids
  // New format: [frag_id(4) + size(4) + data(N)] per fragment
  {
    fprintf(stderr, "[DEBUG] Deserialize fragments: len=%zu\n", len);
    uint32_t frag_count = 0;
    std::memcpy(&frag_count, src, sizeof(frag_count));
    src += sizeof(frag_count);
    len -= sizeof(frag_count);
    fprintf(stderr, "[DEBUG] frag_count=%u\n", frag_count);
    std::vector<Slice> frags;
    std::vector<int> frag_ids;
    frags.reserve(frag_count);
    frag_ids.reserve(frag_count);
    for (uint32_t i = 0; i < frag_count; ++i) {
      // Read frag_id (4 bytes)
      if (len < sizeof(int32_t)) return nullptr;
      int32_t frag_id = 0;
      std::memcpy(&frag_id, src, sizeof(frag_id));
      src += sizeof(frag_id);
      len -= sizeof(frag_id);
      frag_ids.push_back(frag_id);
      // Read fragment data: [size(8) + data(N)] — size prefix is sizeof(size_t)
      // to match PutPrefixLengthSlice() in serialize_logentry_helper().
      if (len < sizeof(size_t)) return nullptr;
      size_t frag_size = 0;
      std::memcpy(&frag_size, src, sizeof(frag_size));
      fprintf(stderr, "[DEBUG] frag[%u]: frag_size=%zu, len=%zu\n", i, frag_size, len);
      src += sizeof(frag_size);
      len -= sizeof(frag_size);
      if (frag_size > 16 * 1024 * 1024) {  // Sanity check: max 16MB
        fprintf(stderr, "[DEBUG] FAIL: frag_size=%zu too large\n", frag_size);
        return nullptr;
      }
      if (len < frag_size) {
        fprintf(stderr, "[DEBUG] FAIL: len=%zu < frag_size=%zu\n", len, frag_size);
        return nullptr;
      }
      char* frag_data = new char[frag_size];
      std::memcpy(frag_data, src, frag_size);
      frags.push_back(Slice::Take(frag_data, frag_size));
      src += frag_size;
      len -= frag_size;
      fprintf(stderr, "[DEBUG] frag[%u] complete: new len=%zu\n", i, len);
    }
    entry->SetFragments(frags);  // Takes ownership of NEW memory
    entry->SetFragIds(frag_ids); // Restore the correct frag_ids
  }

  // Deserialize placement_
  {
    fprintf(stderr, "[DEBUG] Deserialize placement: len=%zu\n", len);
    uint32_t placement_count = 0;
    // Show raw bytes for debugging
    fprintf(stderr, "[DEBUG] placement raw bytes: %02x %02x %02x %02x\n",
            (unsigned char)src[0], (unsigned char)src[1], (unsigned char)src[2], (unsigned char)src[3]);
    std::memcpy(&placement_count, src, sizeof(placement_count));
    src += sizeof(placement_count);
    len -= sizeof(placement_count);
    fprintf(stderr, "[DEBUG] placement_count=%u\n", placement_count);
    std::vector<FragmentPlacement> placement;
    placement.reserve(placement_count);
    for (uint32_t i = 0; i < placement_count; ++i) {
      if (len < sizeof(int) * 4) {
        fprintf(stderr, "[DEBUG] FAIL: placement len=%zu < 16\n", len);
        return nullptr;
      }
      FragmentPlacement p;
      std::memcpy(&p.frag_id, src, sizeof(p.frag_id));
      src += sizeof(p.frag_id);
      len -= sizeof(p.frag_id);
      std::memcpy(&p.local_group, src, sizeof(p.local_group));
      src += sizeof(p.local_group);
      len -= sizeof(p.local_group);
      std::memcpy(&p.node_id, src, sizeof(p.node_id));
      src += sizeof(p.node_id);
      len -= sizeof(p.node_id);
      uint32_t kind = 0;
      std::memcpy(&kind, src, sizeof(kind));
      src += sizeof(kind);
      len -= sizeof(kind);
      p.kind = static_cast<FragmentPlacement::Kind>(kind);
      placement.push_back(p);
    }
    entry->SetPlacement(placement);
  }

  return src;
}

void Serializer::Serialize(const LogEntry *entry, RCF::ByteBuffer *buffer) {
  serialize_logentry_helper(entry, buffer->getPtr());
}

void Serializer::Deserialize(const RCF::ByteBuffer *buffer, LogEntry *entry) {
  deserialize_logentry_helper(buffer->getPtr(), entry);
}

void Serializer::Serialize(const RequestVoteArgs *args, RCF::ByteBuffer *buffer) {
  auto dst = buffer->getPtr();
  std::memcpy(dst, args, sizeof(RequestVoteArgs));
}

void Serializer::Deserialize(const RCF::ByteBuffer *buffer, RequestVoteArgs *args) {
  auto src = buffer->getPtr();
  std::memcpy(args, src, sizeof(RequestVoteArgs));
}

void Serializer::Serialize(const RequestVoteReply *reply, RCF::ByteBuffer *buffer) {
  auto dst = buffer->getPtr();
  std::memcpy(dst, reply, sizeof(RequestVoteReply));
}

void Serializer::Deserialize(const RCF::ByteBuffer *buffer, RequestVoteReply *reply) {
  auto src = buffer->getPtr();
  std::memcpy(reply, src, sizeof(RequestVoteReply));
}

void Serializer::Serialize(const AppendEntriesArgs *args, RCF::ByteBuffer *buffer) {
  assert(args->entry_cnt == args->entries.size());
  auto dst = buffer->getPtr();
  std::memcpy(dst, args, kAppendEntriesArgsHdrSize);
  dst += kAppendEntriesArgsHdrSize;
  fprintf(stderr, "[SERIALIZER] Serialize AE: entry_cnt=%ld\n", (long)args->entry_cnt);
  for (const auto &ent : args->entries) {
    dst = serialize_logentry_helper(&ent, dst);
  }
  fprintf(stderr, "[SERIALIZER] Serialize AE: total_buffer_size=%ld (entries=%ld)\n",
          (long)(dst - buffer->getPtr()), (long)(dst - buffer->getPtr() - kAppendEntriesArgsHdrSize));
  fflush(stderr);
}

void Serializer::Deserialize(const RCF::ByteBuffer *buffer, AppendEntriesArgs *args) {
  const char *src = buffer->getPtr();
  size_t remaining = buffer->getLength();
  
  if (remaining < kAppendEntriesArgsHdrSize) {
    fprintf(stderr, "[SERIALIZER] ERROR: Buffer too small for AppendEntriesArgs header: %zu < %zu\n",
            remaining, kAppendEntriesArgsHdrSize);
    return;
  }
  
  std::memcpy(args, src, kAppendEntriesArgsHdrSize);
  src += kAppendEntriesArgsHdrSize;
  remaining -= kAppendEntriesArgsHdrSize;
  
  fprintf(stderr, "[SERIALIZER] Deserialize AE: entry_cnt=%ld remaining=%zu\n",
          (long)args->entry_cnt, remaining);
  fflush(stderr);
  
  args->entries.reserve(args->entry_cnt);
  for (decltype(args->entry_cnt) i = 0; i < args->entry_cnt; ++i) {
    LogEntry ent;
    const char* result = deserialize_logentry_withbound(src, remaining, &ent);
    if (result == nullptr) {
      fprintf(stderr, "[SERIALIZER] ERROR: Failed to deserialize entry %zu (entry_cnt=%ld remaining=%zu)\n",
              (size_t)i, (long)args->entry_cnt, remaining);
      break;
    }
    args->entries.push_back(ent);
    src = result;
    remaining = buffer->getLength() - (src - buffer->getPtr());
  }
}

void Serializer::Serialize(const AppendEntriesReply *reply, RCF::ByteBuffer *buffer) {
  auto dst = buffer->getPtr();
  // std::memcpy(dst, reply, sizeof(AppendEntriesReply));
  std::memcpy(dst, reply, kAppendEntriesReplyHdrSize);
  dst += kAppendEntriesReplyHdrSize;
  for (const auto &chunk_info : reply->chunk_infos) {
    std::memcpy(dst, &chunk_info, sizeof(ChunkInfo));
    dst += sizeof(ChunkInfo);
  }
}

void Serializer::Deserialize(const RCF::ByteBuffer *buffer, AppendEntriesReply *reply) {
  auto src = buffer->getPtr();
  std::memcpy(reply, src, kAppendEntriesReplyHdrSize);
  src += kAppendEntriesReplyHdrSize;
  for (int i = 0; i < reply->chunk_info_cnt; ++i) {
    ChunkInfo ci;
    std::memcpy(&ci, src, sizeof(ChunkInfo));
    src += sizeof(ChunkInfo);
    reply->chunk_infos.push_back(ci);
  }
}

void Serializer::Serialize(const RequestFragmentsArgs *args, RCF::ByteBuffer *buffer) {
  auto dst = buffer->getPtr();
  std::memcpy(dst, args, sizeof(RequestFragmentsArgs));
}

void Serializer::Deserialize(const RCF::ByteBuffer *buffer, RequestFragmentsArgs *args) {
  auto src = buffer->getPtr();
  std::memcpy(args, src, sizeof(RequestFragmentsArgs));
}

void Serializer::Serialize(const RequestFragmentsReply *reply, RCF::ByteBuffer *buffer) {
  assert(reply->entry_cnt == reply->fragments.size());
  auto dst = buffer->getPtr();
  std::memcpy(dst, reply, kRequestFragmentsReplyHdrSize);
  dst += kRequestFragmentsReplyHdrSize;
  for (const auto &ent : reply->fragments) {
    dst = serialize_logentry_helper(&ent, dst);
  }
}

void Serializer::Deserialize(const RCF::ByteBuffer *buffer, RequestFragmentsReply *reply) {
  const char *src = buffer->getPtr();
  std::memcpy(reply, src, kRequestFragmentsReplyHdrSize);
  src += kRequestFragmentsReplyHdrSize;
  reply->fragments.reserve(reply->entry_cnt);
  for (decltype(reply->entry_cnt) i = 0; i < reply->entry_cnt; ++i) {
    LogEntry ent;
    src = deserialize_logentry_helper(src, &ent);
    reply->fragments.push_back(ent);
  }
}

char *Serializer::PutPrefixLengthSlice(const Slice &slice, char *buf) {
  *reinterpret_cast<size_t *>(buf) = slice.size();
  buf += sizeof(size_t);
  std::memcpy(buf, slice.data(), slice.size());
  return buf + slice.size();
}

const char *Serializer::ParsePrefixLengthSlice(const char *buf, Slice *slice) {
  size_t size = *reinterpret_cast<const size_t *>(buf);
  char *data = new char[size];
  buf += sizeof(size_t);
  std::memcpy(data, buf, size);
  *slice = Slice(data, size);
  return buf + size;
}

const char *Serializer::ParsePrefixLengthSliceWithBound(const char *buf, size_t len, Slice *slice) {
  if (len < sizeof(size_t)) {
    return nullptr;
  }
  size_t size = *reinterpret_cast<const size_t *>(buf);
  if (size + sizeof(size_t) > len) {  // Beyond range
    return nullptr;
  }
  char *data = new char[size];
  buf += sizeof(size_t);
  std::memcpy(data, buf, size);
  *slice = Slice(data, size);
  return buf + size;
}

static char* put_string(const std::string& s, char* dst) {
  size_t len = s.size();
  std::memcpy(dst, &len, sizeof(len));
  dst += sizeof(len);
  if (len > 0) {
    std::memcpy(dst, s.data(), len);
    dst += len;
  }
  return dst;
}

static const char* get_string(const char* src, std::string* s) {
  size_t len;
  std::memcpy(&len, src, sizeof(len));
  src += sizeof(len);
  if (len > 0) {
    s->assign(src, len);
    src += len;
  } else {
    s->clear();
  }
  return src;
}

static size_t string_size(const std::string& s) {
  return sizeof(size_t) + s.size();
}

void Serializer::Serialize(const GroupNotificationArgs *args, RCF::ByteBuffer *buffer) {
  char* dst = buffer->getPtr();
  std::memcpy(dst, &args->source_node_id, sizeof(args->source_node_id));
  dst += sizeof(args->source_node_id);
  std::memcpy(dst, &args->total_groups, sizeof(args->total_groups));
  dst += sizeof(args->total_groups);

  size_t group_count = args->groups.size();
  std::memcpy(dst, &group_count, sizeof(group_count));
  dst += sizeof(group_count);

  for (const auto& g : args->groups) {
    std::memcpy(dst, &g.group_id, sizeof(g.group_id));
    dst += sizeof(g.group_id);
    std::memcpy(dst, &g.initiator_id, sizeof(g.initiator_id));
    dst += sizeof(g.initiator_id);
    std::memcpy(dst, &g.initiator_generated_index, sizeof(g.initiator_generated_index));
    dst += sizeof(g.initiator_generated_index);
    std::memcpy(dst, &g.complementary_group_count, sizeof(g.complementary_group_count));
    dst += sizeof(g.complementary_group_count);

    size_t comp_count = g.complementary_group_indices.size();
    std::memcpy(dst, &comp_count, sizeof(comp_count));
    dst += sizeof(comp_count);
    for (auto idx : g.complementary_group_indices) {
      std::memcpy(dst, &idx, sizeof(idx));
      dst += sizeof(idx);
    }

    size_t member_count = g.members.size();
    std::memcpy(dst, &member_count, sizeof(member_count));
    dst += sizeof(member_count);
    for (const auto& m : g.members) {
      std::memcpy(dst, &m.node_id, sizeof(m.node_id));
      dst += sizeof(m.node_id);
      dst = put_string(m.raft_rpc_addr, dst);
      dst = put_string(m.kv_rpc_addr, dst);
      dst = put_string(m.raft_log_filename, dst);
      dst = put_string(m.kv_dbname, dst);
    }
  }
}

void Serializer::Deserialize(const RCF::ByteBuffer *buffer, GroupNotificationArgs *args) {
  const char* src = buffer->getPtr();
  std::memcpy(&args->source_node_id, src, sizeof(args->source_node_id));
  src += sizeof(args->source_node_id);
  std::memcpy(&args->total_groups, src, sizeof(args->total_groups));
  src += sizeof(args->total_groups);

  size_t group_count;
  std::memcpy(&group_count, src, sizeof(group_count));
  src += sizeof(group_count);
  args->groups.resize(group_count);

  for (size_t gi = 0; gi < group_count; ++gi) {
    auto& g = args->groups[gi];
    std::memcpy(&g.group_id, src, sizeof(g.group_id));
    src += sizeof(g.group_id);
    std::memcpy(&g.initiator_id, src, sizeof(g.initiator_id));
    src += sizeof(g.initiator_id);
    std::memcpy(&g.initiator_generated_index, src, sizeof(g.initiator_generated_index));
    src += sizeof(g.initiator_generated_index);
    std::memcpy(&g.complementary_group_count, src, sizeof(g.complementary_group_count));
    src += sizeof(g.complementary_group_count);

    size_t comp_count;
    std::memcpy(&comp_count, src, sizeof(comp_count));
    src += sizeof(comp_count);
    g.complementary_group_indices.resize(comp_count);
    for (size_t ci = 0; ci < comp_count; ++ci) {
      std::memcpy(&g.complementary_group_indices[ci], src, sizeof(raft_group_id_t));
      src += sizeof(raft_group_id_t);
    }

    size_t member_count;
    std::memcpy(&member_count, src, sizeof(member_count));
    src += sizeof(member_count);
    g.members.resize(member_count);
    for (size_t mi = 0; mi < member_count; ++mi) {
      auto& m = g.members[mi];
      std::memcpy(&m.node_id, src, sizeof(m.node_id));
      src += sizeof(m.node_id);
      src = get_string(src, &m.raft_rpc_addr);
      src = get_string(src, &m.kv_rpc_addr);
      src = get_string(src, &m.raft_log_filename);
      src = get_string(src, &m.kv_dbname);
    }
  }
}

void Serializer::Serialize(const GroupNotificationReply *reply, RCF::ByteBuffer *buffer) {
  char* dst = buffer->getPtr();
  std::memcpy(dst, &reply->success, sizeof(reply->success));
  dst += sizeof(reply->success);
  std::memcpy(dst, &reply->reply_node_id, sizeof(reply->reply_node_id));
  dst += sizeof(reply->reply_node_id);
}

void Serializer::Deserialize(const RCF::ByteBuffer *buffer, GroupNotificationReply *reply) {
  const char* src = buffer->getPtr();
  std::memcpy(&reply->success, src, sizeof(reply->success));
  src += sizeof(reply->success);
  std::memcpy(&reply->reply_node_id, src, sizeof(reply->reply_node_id));
}

size_t Serializer::getSerializeSize(const GroupNotificationArgs &args) {
  size_t ret = sizeof(args.source_node_id) + sizeof(args.total_groups) + sizeof(size_t);
  for (const auto& g : args.groups) {
    ret += sizeof(g.group_id) + sizeof(g.initiator_id) + sizeof(g.initiator_generated_index)
         + sizeof(g.complementary_group_count) + sizeof(size_t)
         + g.complementary_group_indices.size() * sizeof(raft_group_id_t)
         + sizeof(size_t);
    for (const auto& m : g.members) {
      ret += sizeof(m.node_id) + string_size(m.raft_rpc_addr)
           + string_size(m.kv_rpc_addr) + string_size(m.raft_log_filename)
           + string_size(m.kv_dbname);
    }
  }
  return ret;
}

size_t Serializer::getSerializeSize(const GroupNotificationReply &reply) {
  return sizeof(reply.success) + sizeof(reply.reply_node_id);
}

size_t Serializer::getSerializeSize(const LogEntry &entry) {
  // The serialized size includes:
  // 1. sizeof(LogEntry) - the raw structure
  // 2. not_encoded_slice data
  // 3. fragment_slice data
  // 4. fragments_ vector data
  // 5. placement_ vector data
  size_t ret = sizeof(LogEntry);
  ret += entry.NotEncodedSlice().size();
  ret += entry.FragmentSlice().size();
  ret += 2 * sizeof(size_t);  // Prefix lengths for slices

  // LRC fragments_
  ret += sizeof(uint32_t);  // frag_count
  for (const auto& frag : entry.Fragments()) {
    ret += sizeof(uint32_t) + sizeof(size_t) + frag.size();  // frag_id(4) + size_prefix(8) + data(N)
  }

  // placement_
  ret += sizeof(uint32_t);  // placement_count
  ret += entry.Placement().size() * (sizeof(int) * 3 + sizeof(uint32_t));

  // Make size 4B alignment
  return (ret - 1) / 4 * 4 + 4;
}

size_t Serializer::getSerializeSize(const RequestVoteArgs &args) { return sizeof(args); }

size_t Serializer::getSerializeSize(const RequestVoteReply &reply) { return sizeof(reply); }

size_t Serializer::getSerializeSize(const AppendEntriesArgs &args) {
  size_t ret = kAppendEntriesArgsHdrSize;
  for (const auto &ent : args.entries) {
    ret += getSerializeSize(ent);
  }
  // Make the size 4B alignment
  return (ret - 1) / 4 * 4 + 4;
}

size_t Serializer::getSerializeSize(const AppendEntriesReply &reply) {
  size_t ret = kAppendEntriesReplyHdrSize;
  ret += reply.chunk_info_cnt * sizeof(ChunkInfo);
  return ret;
}

size_t Serializer::getSerializeSize(const RequestFragmentsArgs &args) { return sizeof(args); }

size_t Serializer::getSerializeSize(const RequestFragmentsReply &reply) {
  size_t ret = kRequestFragmentsReplyHdrSize;
  for (const auto &ent : reply.fragments) {
    ret += getSerializeSize(ent);
  }
  return ret;
}

// ============================================================================
//  SerializeForApply - 序列化为 Apply 消息
//  格式: [magic(4) + version(4) + kind(1)] + payload
// ============================================================================

std::string SerializeForApply(const LogEntry& entry, const std::string& user_key, size_t original_size) {
  std::string result;
  result.reserve(1024);

  // Header: magic + version + kind
  result.append("APLY", 4);
  uint32_t version = 1;
  char ver_buf[4];
  EncodeFixed32(ver_buf, version);
  result.append(ver_buf, 4);

  if (entry.IsLrcEncoded()) {
    // Kind = kLrcFragments (3)
    result.push_back(static_cast<char>(3));

    const auto& ci = entry.GetChunkInfo();

    // Prefer the already extracted user_key from ExtractApplyMeta().
    // Fallback to entry.CommandData() only if caller passed an empty key.
    std::string apply_user_key = user_key;
    if (apply_user_key.empty()) {
      uint32_t key_size = ci.GetKeySize();
      auto cmd = entry.CommandData();
      if (cmd.valid() && key_size > 0) {
        // Detect STRI header to skip it (Leader-side entry keeps the full STRI blob).
        const char* p = cmd.data();
        size_t header_skip = 0;
        if (cmd.size() >= 21 && p[0] == 'S' && p[1] == 'T' && p[2] == 'R' && p[3] == 'I') {
          header_skip = 21;
        }
        if (header_skip + key_size <= cmd.size()) {
          apply_user_key.assign(p + header_skip, key_size);
        } else {
          apply_user_key.assign(p, std::min(key_size, static_cast<uint32_t>(cmd.size())));
        }
      }
    }

    // user_key
    uint32_t key_len = static_cast<uint32_t>(apply_user_key.size());
    char key_len_buf[4];
    EncodeFixed32(key_len_buf, key_len);
    result.append(key_len_buf, 4);
    result.append(apply_user_key);

    // ChunkInfo (包含 key_size 和 total_size)
    char ci_buf[28];
    EncodeFixed32(ci_buf, ci.GetK());
    EncodeFixed32(ci_buf + 4, ci.GetRaftIndex());
    EncodeFixed32(ci_buf + 8, ci.GetL());
    EncodeFixed32(ci_buf + 12, ci.GetR());
    EncodeFixed32(ci_buf + 16, static_cast<uint32_t>(ci.GetLrcGroupId()));
    EncodeFixed32(ci_buf + 20, ci.GetKeySize());
    EncodeFixed32(ci_buf + 24, ci.GetTotalSize());
    result.append(ci_buf, 28);

    // Placement
    const auto& placement = entry.Placement();
    uint32_t placement_cnt = static_cast<uint32_t>(placement.size());
    char cnt_buf[4];
    EncodeFixed32(cnt_buf, placement_cnt);
    result.append(cnt_buf, 4);
    for (const auto& fp : placement) {
      char fp_buf[16];
      EncodeFixed32(fp_buf, static_cast<uint32_t>(fp.node_id));
      EncodeFixed32(fp_buf + 4, static_cast<uint32_t>(fp.frag_id));
      EncodeFixed32(fp_buf + 8, static_cast<uint32_t>(fp.local_group));
      EncodeFixed32(fp_buf + 12, static_cast<uint32_t>(fp.kind));
      result.append(fp_buf, 16);
    }

    // Fragments: each fragment stored with its frag_id for correct mapping
    // Format: [frag_id(4) + size(4) + data(N)] per fragment
    const auto& fragments = entry.Fragments();
    const auto& frag_ids = entry.FragIds();
    uint32_t frag_cnt = static_cast<uint32_t>(fragments.size());
    fprintf(stderr, "[DEBUG-SERIALIZE] SerializeForApply: frag_cnt=%u, frag_ids.size()=%zu\n",
            frag_cnt, frag_ids.size());
    EncodeFixed32(cnt_buf, frag_cnt);
    result.append(cnt_buf, 4);
    for (size_t i = 0; i < fragments.size(); ++i) {
      uint32_t frag_id = (i < frag_ids.size()) ? static_cast<uint32_t>(frag_ids[i]) : static_cast<uint32_t>(i);
      uint32_t frag_size = static_cast<uint32_t>(fragments[i].size());
      fprintf(stderr, "[DEBUG-SERIALIZE]   frag[%zu]: frag_id=%u size=%u\n", i, frag_id, frag_size);
      char frag_header[8];
      EncodeFixed32(frag_header, frag_id);
      EncodeFixed32(frag_header + 4, frag_size);
      result.append(frag_header, 8);
      result.append(fragments[i].data(), frag_size);
    }
    fflush(stderr);

    // original_size (needed for read truncation)
    char size_buf[8];
    EncodeFixed64(size_buf, static_cast<uint64_t>(original_size));
    result.append(size_buf, 8);
  } else {
    // Kind = kLegacy (0)
    result.push_back(static_cast<char>(0));

    // command_data
    auto cmd = entry.CommandData();
    uint32_t cmd_len = static_cast<uint32_t>(cmd.size());
    char len_buf[4];
    EncodeFixed32(len_buf, cmd_len);
    result.append(len_buf, 4);
    if (cmd_len > 0) {
      result.append(cmd.data(), cmd_len);
    }
  }

  return result;
}

}  // namespace raft
