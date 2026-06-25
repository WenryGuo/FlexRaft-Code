#include "batch_system_bridge.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

#include "../multi-raft/message.h"
#include "../multi-raft/stripe_format.h"  // ApplyPayloadKind, StripeWriteCommand
#include "log_entry.h"
#include "serializer.h"

namespace raft {

// =============================================================================
//  Helper: Extract a user-level key from LogEntry for routing/identification.
// =============================================================================
// =============================================================================
//  Helper: Extract user-level key AND original data size from LogEntry.
//  For kFragments: parses the MFRG header once and returns both values.
//  Returns false if extraction fails.
// =============================================================================
static bool ExtractApplyMeta(const LogEntry& entry, std::string* user_key, size_t* original_size) {
  const auto& ci0 = entry.GetChunkInfo();
  fprintf(stderr,
          "[DEBUG-BRIDGE] ExtractApplyMeta: entry.Type=%d IsLrcEncoded=%d k=%d l=%d r=%d "
          "lrc_gid=%d idx=%u key_size=%u\n",
          static_cast<int>(entry.Type()), entry.IsLrcEncoded() ? 1 : 0,
          ci0.GetK(), ci0.GetL(), ci0.GetR(), ci0.GetLrcGroupId(),
          entry.Index(), ci0.GetKeySize());
  if (entry.Type() == kFragments) {
    // FragmentSlice contains MFRG packed data.
    // MFRG format: [magic:4][meta_len:4][user_key_len:4][user_key...][frags...]
    const auto& fs = entry.FragmentSlice();
    if (fs.size() >= 12) {  // minimum: magic(4) + meta_len(4) + user_key_len(4)
      const char* p = fs.data();
      if (p[0] == 'M' && p[1] == 'F' && p[2] == 'R' && p[3] == 'G') {
        uint32_t key_len = raft::DecodeFixed32(p + 8);
        // Sanity: key_len <= 1024, and total slice must be large enough
        if (key_len <= 1024 && fs.size() >= 12 + key_len) {
          if (user_key) *user_key = std::string(p + 12, key_len);
          if (original_size) *original_size = entry.NotEncodedSlice().size();
          return true;
        }
      }
    }
    return false;
  } else if (entry.IsLrcEncoded()) {
    // === LRC KEY-ONLY 格式：command_data 只含 key 部分 ===
    // key 存储在 command_data 前 key_size 字节
    //
    // 在 Multi-Raft 写入路径中，Leader 的 LogEntry.command_data 仍然保存着完整的
    // STRI blob（[magic:4][group_id:8][mode:1][key_len:8][key:N][val_len:8][value]），
    // 而不是 peer-keyed 模式下被裁剪过的纯 key 形式。这里需要兼容这两种情况：
    //   1) STRI blob: 跳过 21 字节 header，再读 key_size 字节
    //   2) key-only : 直接读前 key_size 字节
    const auto& ci = entry.GetChunkInfo();
    auto cmd = entry.CommandData();
    uint32_t key_size = ci.GetKeySize();
    if (!cmd.valid() || key_size == 0) {
      fprintf(stderr,
              "[DEBUG-BRIDGE] ExtractApplyMeta(LRC): invalid - cmd.valid=%d key_size=%u "
              "cmd.size=%zu idx=%u\n",
              cmd.valid() ? 1 : 0, key_size, cmd.size(), entry.Index());
      return false;
    }
    const char* p = cmd.data();
    size_t len = cmd.size();
    size_t header_skip = 0;
    if (len >= 21 && p[0] == 'S' && p[1] == 'T' && p[2] == 'R' && p[3] == 'I') {
      // STRI blob on Leader side: skip [STRI:4][group_id:8][mode:1][key_len:8] = 21 bytes
      header_skip = 21;
    }
    if (key_size > 0 && (header_skip + key_size) <= len) {
      if (user_key) user_key->assign(p + header_skip, key_size);
      if (original_size) *original_size = ci.GetTotalSize();
      return true;
    }
    fprintf(stderr,
            "[DEBUG-BRIDGE] ExtractApplyMeta(LRC): bounds fail - key_size=%u header_skip=%zu "
            "len=%zu idx=%u\n",
            key_size, header_skip, len, entry.Index());
    return false;
  } else {
    // Legacy STRI format: [STRI:4][group_id:4][mode:1][key_len:4][user_key][val_len:4]
    auto cmd = entry.CommandData();
    const char* p = cmd.data();
    size_t len = cmd.size();
    if (len >= 13 && p[0] == 'S' && p[1] == 'T' && p[2] == 'R' && p[3] == 'I') {
      uint32_t key_len = raft::DecodeFixed32(p + 9);  // skip magic(4)+group_id(4)+mode(1)
      if (key_len <= 1024 && len >= static_cast<size_t>(13 + key_len + 4)) {
        if (user_key) *user_key = std::string(p + 13, key_len);
        if (original_size) *original_size = static_cast<size_t>(raft::DecodeFixed32(p + 13 + key_len));
        return true;
      }
    }
    return false;
  }
}

static std::string ExtractUserKey(const LogEntry& entry) {
  std::string key;
  ExtractApplyMeta(entry, &key, nullptr);
  return key;
}

// =============================================================================
//  Fine-grained locking: one shared_mutex per (group_id, node_id) pair.
//  Different groups on different nodes can apply in parallel.
//  The global mutex protects the entry map; per-entry mutexes guard the ptr.
// =============================================================================
struct MailboxEntry {
  void* ptr = nullptr;
  std::shared_mutex mtx;  // Protects ptr during unregistration
};

static std::shared_mutex& GetGlobalMutex() {
  static std::shared_mutex mu;
  return mu;
}

static std::unordered_map<uint64_t, MailboxEntry>& GetEntryMap() {
  static std::unordered_map<uint64_t, MailboxEntry> m;
  return m;
}

static uint64_t MakeKeyInternal(uint32_t group_id, uint32_t node_id) {
  return (static_cast<uint64_t>(group_id) << 32) | node_id;
}

/* static */ std::shared_mutex& BatchSystemBridge::GetMutex() {
  return GetGlobalMutex();
}

/* static */ std::unordered_map<uint64_t, void*>& BatchSystemBridge::GetMailboxMap() {
  // Legacy compatibility: return a snapshot of the current mailboxes.
  // Caller expects a map they can iterate. We return the raw entry pointers.
  static std::unordered_map<uint64_t, void*> snapshot;
  std::lock_guard<std::shared_mutex> lk(GetGlobalMutex());
  snapshot.clear();
  for (auto& [k, e] : GetEntryMap()) {
    if (e.ptr != nullptr) {
      snapshot[k] = e.ptr;
    }
  }
  return snapshot;
}

/* static */ void BatchSystemBridge::RegisterMailbox(uint32_t group_id,
                                                      uint32_t node_id,
                                                      void* mailbox_ptr) {
  uint64_t key = MakeKeyInternal(group_id, node_id);
  std::lock_guard<std::shared_mutex> lk(GetGlobalMutex());
  GetEntryMap()[key].ptr = mailbox_ptr;
}

/* static */ void BatchSystemBridge::UnregisterMailbox(uint32_t group_id, uint32_t node_id) {
  uint64_t key = MakeKeyInternal(group_id, node_id);
  std::lock_guard<std::shared_mutex> lk(GetGlobalMutex());
  auto& entry = GetEntryMap()[key];
  std::unique_lock<std::shared_mutex> lk2(entry.mtx);
  entry.ptr = nullptr;
}

/* static */ bool BatchSystemBridge::HasMailbox(uint32_t group_id, uint32_t node_id) {
  uint64_t key = MakeKeyInternal(group_id, node_id);
  std::shared_lock<std::shared_mutex> lk(GetGlobalMutex());
  auto it = GetEntryMap().find(key);
  if (it == GetEntryMap().end()) return false;
  std::shared_lock<std::shared_mutex> lk2(it->second.mtx);
  return it->second.ptr != nullptr;
}

/* static */ void BatchSystemBridge::ApplyEntry(uint32_t group_id,
                                                uint32_t node_id,
                                                const LogEntry& entry) {
  uint64_t key = MakeKeyInternal(group_id, node_id);

  // Hot path: shared_lock allows concurrent ApplyEntry calls from multiple groups
  void* raw_ptr = nullptr;
  {
    std::shared_lock<std::shared_mutex> lk(GetGlobalMutex());
    auto it = GetEntryMap().find(key);
    if (it == GetEntryMap().end()) {
      // [DEBUG] ApplyEntry: mailbox not found for group=%u node=%u
      fprintf(stderr, "[DEBUG-BRIDGE] mailbox not found: group=%u node=%u\n", group_id, node_id);
      return;
    }
    std::shared_lock<std::shared_mutex> lk2(it->second.mtx);
    raw_ptr = it->second.ptr;
  }

  if (raw_ptr == nullptr) {
    // [DEBUG] ApplyEntry: mailbox ptr is null for group=%u node=%u
    fprintf(stderr, "[DEBUG-BRIDGE] mailbox ptr null: group=%u node=%u\n", group_id, node_id);
    return;
  }

  // [DEBUG] ApplyEntry: sending to mailbox ptr=%p group=%u node=%u entry_idx=%u
  fprintf(stderr, "[DEBUG-BRIDGE] ApplyEntry: ptr=%p group=%u node=%u idx=%u\n",
          raw_ptr, group_id, node_id, entry.Index());
  fflush(stderr);

  auto* mb = static_cast<multiraft::Mailbox<multiraft::ApplyMsg>*>(raw_ptr);

  // 构造 MsgApplyCommitted message
  multiraft::MsgApplyCommitted msg;
  msg.entry_id = entry.Index();
  msg.stripe_id = entry.Index();
  msg.group_id = group_id;

  // 将 LogEntry 打包为字节
  // 使用统一的 SerializeForApply 接口，同时提取 user_key 和 original_size
  std::string user_key;
  size_t original_size = 0;
  if (!ExtractApplyMeta(entry, &user_key, &original_size)) {
    fprintf(stderr, "[DEBUG-BRIDGE] ExtractApplyMeta failed: idx=%u\n", entry.Index());
    return;
  }
  std::string serialized = SerializeForApply(entry, user_key, original_size);
  msg.data.assign(serialized.begin(), serialized.end());

  // [DEBUG] ApplyEntry: pushing msg to mailbox ptr=%p group=%u idx=%u data_size=%zu
  fprintf(stderr, "[DEBUG-BRIDGE] push: mb=%p group=%u idx=%u data_size=%zu\n",
          (void*)mb, group_id, entry.Index(), msg.data.size());
  fflush(stderr);

  mb->push(std::move(msg));
}

/* static */ void BatchSystemBridge::ApplyEntryToGroup(uint32_t group_id,
                                                         uint32_t node_id,
                                                         const LogEntry& entry) {
  ApplyEntry(group_id, node_id, entry);
}

}  // namespace raft
