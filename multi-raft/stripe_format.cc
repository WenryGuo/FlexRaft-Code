#include "stripe_format.h"

#include <cstring>

namespace multiraft {

static void AppendU64(std::string* out, uint64_t v) {
  out->append(reinterpret_cast<const char*>(&v), 8);
}

static bool ReadU64(const char*& p, const char* end, uint64_t* v) {
  if (static_cast<size_t>(end - p) < 8) return false;
  std::memcpy(v, p, 8);
  p += 8;
  return true;
}

// Use 8-byte length prefix to match Serializer::PutPrefixLengthSlice (sizeof(size_t)=8 on 64-bit)
static void AppendU32(std::string* out, uint32_t v) {
  uint64_t v64 = v;
  out->append(reinterpret_cast<const char*>(&v64), 8);
}

static bool ReadU32(const char*& p, const char* end, uint32_t* v) {
  if (static_cast<size_t>(end - p) < 8) return false;
  uint64_t v64 = 0;
  std::memcpy(&v64, p, 8);
  *v = static_cast<uint32_t>(v64);
  p += 8;
  return true;
}

std::string StripeWriteCommand::Serialize() const {
  std::string out;
  out.append(kStripeMagic, 4);
  AppendU32(&out, group_id);
  uint8_t mode = static_cast<uint8_t>(encoding_mode);
  out.append(reinterpret_cast<const char*>(&mode), 1);
  AppendU32(&out, static_cast<uint32_t>(user_key.size()));
  out.append(user_key);
  AppendU32(&out, static_cast<uint32_t>(user_value.size()));
  out.append(user_value);
  return out;
}

bool StripeWriteCommand::Deserialize(const char* data, size_t len, StripeWriteCommand* out) {
  if (len < 4 || std::memcmp(data, kStripeMagic, 4) != 0) return false;
  const char* p = data + 4;
  const char* end = data + len;
  uint32_t gid = 0;
  if (!ReadU32(p, end, &gid)) return false;
  if (p >= end) return false;
  uint8_t mode = static_cast<uint8_t>(*p++);
  uint32_t klen = 0;
  if (!ReadU32(p, end, &klen)) return false;
  if (static_cast<size_t>(end - p) < klen) return false;
  out->user_key.assign(p, klen);
  p += klen;
  uint32_t vlen = 0;
  if (!ReadU32(p, end, &vlen)) return false;
  if (static_cast<size_t>(end - p) < vlen) return false;
  out->user_value.assign(p, vlen);
  p += vlen;
  out->group_id = gid;
  out->encoding_mode = static_cast<EncodingMode>(mode);
  return true;
}

bool StripeWriteCommand::IsStripeCommand(const char* data, size_t len) {
  return len >= 4 && std::memcmp(data, kStripeMagic, 4) == 0;
}

std::string StripeLogMeta::Serialize() const {
  std::string out;
  AppendU32(&out, static_cast<uint32_t>(user_key.size()));
  out.append(user_key);
  AppendU64(&out, stripe_id);
  AppendU64(&out, entry_id);
  AppendU32(&out, group_id);
  AppendU32(&out, static_cast<uint32_t>(original_size));
  uint8_t mode = static_cast<uint8_t>(encoding_mode);
  out.append(reinterpret_cast<const char*>(&mode), 1);
  AppendU32(&out, static_cast<uint32_t>(k));
  AppendU32(&out, static_cast<uint32_t>(l));
  AppendU32(&out, static_cast<uint32_t>(r));
  AppendU32(&out, static_cast<uint32_t>(m));
  AppendU32(&out, static_cast<uint32_t>(placement.size()));
  for (const auto& fp : placement) {
    AppendU32(&out, static_cast<uint32_t>(fp.frag_id));
    AppendU32(&out, static_cast<uint32_t>(fp.kind));
    AppendU32(&out, static_cast<uint32_t>(fp.local_group));
    AppendU32(&out, static_cast<uint32_t>(fp.node_id));
  }
  return out;
}

bool StripeLogMeta::Deserialize(const char* data, size_t len, StripeLogMeta* out) {
  const char* p = data;
  const char* end = data + len;
  uint32_t klen = 0;
  if (!ReadU32(p, end, &klen)) return false;
  if (static_cast<size_t>(end - p) < klen) return false;
  out->user_key.assign(p, klen);
  p += klen;
  if (!ReadU64(p, end, &out->stripe_id)) return false;
  if (!ReadU64(p, end, &out->entry_id)) return false;
  uint32_t gid = 0;
  if (!ReadU32(p, end, &gid)) return false;
  out->group_id = gid;
  uint32_t orig = 0;
  if (!ReadU32(p, end, &orig)) return false;
  out->original_size = orig;
  if (p >= end) return false;
  uint8_t mode = static_cast<uint8_t>(*p++);
  out->encoding_mode = static_cast<EncodingMode>(mode);
  uint32_t kv = 0;
  if (!ReadU32(p, end, &kv)) return false;
  out->k = static_cast<int>(kv);
  if (!ReadU32(p, end, &kv)) return false;
  out->l = static_cast<int>(kv);
  if (!ReadU32(p, end, &kv)) return false;
  out->r = static_cast<int>(kv);
  if (!ReadU32(p, end, &kv)) return false;
  out->m = static_cast<int>(kv);
  uint32_t pcnt = 0;
  if (!ReadU32(p, end, &pcnt)) return false;
  out->placement.clear();
  out->placement.reserve(pcnt);
  for (uint32_t i = 0; i < pcnt; ++i) {
    FragmentPlacement fp;
    uint32_t v = 0;
    if (!ReadU32(p, end, &v)) return false;
    fp.frag_id = static_cast<int>(v);
    if (!ReadU32(p, end, &v)) return false;
    fp.kind = static_cast<FragmentPlacement::Kind>(v);
    if (!ReadU32(p, end, &v)) return false;
    fp.local_group = static_cast<int>(v);
    if (!ReadU32(p, end, &v)) return false;
    fp.node_id = static_cast<int>(v);
    out->placement.push_back(fp);
  }
  return true;
}

std::string PackNodeFragments(const StripeLogMeta& meta,
                              const std::vector<PackedNodeFragment>& frags) {
  std::string out;
  out.append(kPackedFragMagic, 4);
  auto meta_bytes = meta.Serialize();
  AppendU32(&out, static_cast<uint32_t>(meta_bytes.size()));
  out.append(meta_bytes);
  AppendU32(&out, static_cast<uint32_t>(frags.size()));
  for (const auto& f : frags) {
    AppendU32(&out, static_cast<uint32_t>(f.frag_id));
    AppendU32(&out, static_cast<uint32_t>(f.bytes.size()));
    out.append(f.bytes);
  }
  return out;
}

bool UnpackNodeFragments(const char* data, size_t len, StripeLogMeta* meta_out,
                         std::vector<PackedNodeFragment>* frags_out) {
  if (len < 4 || std::memcmp(data, kPackedFragMagic, 4) != 0) return false;
  const char* p = data + 4;
  const char* end = data + len;
  uint32_t meta_len = 0;
  if (!ReadU32(p, end, &meta_len)) return false;
  if (static_cast<size_t>(end - p) < meta_len) return false;
  if (!StripeLogMeta::Deserialize(p, meta_len, meta_out)) return false;
  p += meta_len;
  uint32_t cnt = 0;
  if (!ReadU32(p, end, &cnt)) return false;
  frags_out->clear();
  frags_out->reserve(cnt);
  for (uint32_t i = 0; i < cnt; ++i) {
    PackedNodeFragment pf;
    uint32_t fid = 0;
    if (!ReadU32(p, end, &fid)) return false;
    pf.frag_id = static_cast<int>(fid);
    uint32_t flen = 0;
    if (!ReadU32(p, end, &flen)) return false;
    if (static_cast<size_t>(end - p) < flen) return false;
    pf.bytes.assign(p, flen);
    p += flen;
    frags_out->push_back(std::move(pf));
  }
  return true;
}

// ============================================================================
//  Merged Storage: meta + local fragments serialized together
//
//  Format:
//    [4-byte magic: 'MFST']
//    [8-byte meta_len]
//    [meta_len bytes: StripeLogMeta serialized]
//    [8-byte frag_count]
//    For each fragment:
//      [8-byte frag_id]
//      [8-byte data_len]
//      [data_len bytes: fragment data]
// ============================================================================
std::string SerializeMergedFragStore(const StripeLogMeta& meta,
                                     const std::vector<PackedNodeFragment>& frags) {
  std::string out;
  out.append(kMergedFragMagic, 4);

  // Serialize meta
  auto meta_bytes = meta.Serialize();
  AppendU32(&out, static_cast<uint32_t>(meta_bytes.size()));
  out.append(meta_bytes);

  // Serialize fragments
  AppendU32(&out, static_cast<uint32_t>(frags.size()));
  for (const auto& f : frags) {
    AppendU32(&out, static_cast<uint32_t>(f.frag_id));
    AppendU32(&out, static_cast<uint32_t>(f.bytes.size()));
    out.append(f.bytes);
  }

  return out;
}

bool DeserializeMergedFragStore(const char* data, size_t len,
                                 StripeLogMeta* meta_out,
                                 std::vector<PackedNodeFragment>* frags_out) {
  if (len < 4 || std::memcmp(data, kMergedFragMagic, 4) != 0) return false;
  const char* p = data + 4;
  const char* end = data + len;

  // Deserialize meta
  uint32_t meta_len = 0;
  if (!ReadU32(p, end, &meta_len)) return false;
  if (static_cast<size_t>(end - p) < meta_len) return false;
  if (!StripeLogMeta::Deserialize(p, meta_len, meta_out)) return false;
  p += meta_len;

  // Deserialize fragments
  uint32_t frag_cnt = 0;
  if (!ReadU32(p, end, &frag_cnt)) return false;
  frags_out->clear();
  frags_out->reserve(frag_cnt);

  for (uint32_t i = 0; i < frag_cnt; ++i) {
    PackedNodeFragment pf;
    uint32_t v = 0;

    if (!ReadU32(p, end, &v)) return false;
    pf.frag_id = static_cast<int>(v);

    if (!ReadU32(p, end, &v)) return false;
    uint32_t data_len = v;
    if (static_cast<size_t>(end - p) < data_len) return false;
    pf.bytes.assign(p, data_len);
    p += data_len;

    frags_out->push_back(std::move(pf));
  }

  return true;
}

}  // namespace multiraft
