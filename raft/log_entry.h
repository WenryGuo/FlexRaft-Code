#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "SF/Archive.hpp"
#include "raft_type.h"

namespace raft {

class Serializer;
class Slice {
 public:
  // Deep copy: creates a new owned Slice with copied data
  static Slice Copy(const Slice &slice) {
    Slice result;
    if (slice.size() > 0) {
      result.data_ = new char[slice.size()];
      std::memcpy(result.data_, slice.data(), slice.size());
      result.size_ = slice.size();
      result.owned_ = true;
    }
    return result;
  }

  // Take ownership: for transferring externally-allocated memory to Slice
  static Slice Take(char *data, size_t size) {
    Slice result;
    result.data_ = data;
    result.size_ = size;
    result.owned_ = true;
    return result;
  }

  // Create non-owning view: for external pointers that Slice should not free
  static Slice View(const char *data, size_t size) {
    Slice result;
    result.data_ = const_cast<char *>(data);
    result.size_ = size;
    result.owned_ = false;
    return result;
  }

 public:
  Slice() = default;

  // For external raw pointers: create non-owning view (backward compatible)
  explicit Slice(char *data, size_t size) : data_(data), size_(size), owned_(false) {}

  // From string: takes ownership
  Slice(const std::string &s) : data_(new char[s.size()]), size_(s.size()), owned_(true) {
    if (size_ > 0) {
      std::memcpy(data_, s.c_str(), size_);
    }
  }

  ~Slice() {
    if (owned_ && data_ != nullptr) {
      delete[] data_;
      data_ = nullptr;
      size_ = 0;
      owned_ = false;
    }
  }

  // Deep copy: always copies data and takes ownership
  Slice(const Slice &other) {
    if (other.owned_ && other.size_ > 0) {
      data_ = new char[other.size_];
      std::memcpy(data_, other.data_, other.size_);
      size_ = other.size_;
      owned_ = true;
    } else if (other.data_ != nullptr) {
      // Shallow copy for non-owned slices (views)
      data_ = other.data_;
      size_ = other.size_;
      owned_ = false;
    }
  }

  Slice &operator=(const Slice &other) {
    if (this == &other) return *this;

    // Release existing owned memory
    if (owned_ && data_ != nullptr) {
      delete[] data_;
    }

    if (other.owned_ && other.size_ > 0) {
      data_ = new char[other.size_];
      std::memcpy(data_, other.data_, other.size_);
      size_ = other.size_;
      owned_ = true;
    } else if (other.data_ != nullptr) {
      data_ = other.data_;
      size_ = other.size_;
      owned_ = false;
    } else {
      data_ = nullptr;
      size_ = 0;
      owned_ = false;
    }
    return *this;
  }

  // Move semantics: transfer ownership
  Slice(Slice &&other) noexcept
      : data_(other.data_), size_(other.size_), owned_(other.owned_) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.owned_ = false;
  }

  Slice &operator=(Slice &&other) noexcept {
    if (this == &other) return *this;

    // Release existing owned memory
    if (owned_ && data_ != nullptr) {
      delete[] data_;
    }

    data_ = other.data_;
    size_ = other.size_;
    owned_ = other.owned_;

    other.data_ = nullptr;
    other.size_ = 0;
    other.owned_ = false;

    return *this;
  }

  auto data() const -> char * { return data_; }
  auto size() const -> size_t { return size_; }
  auto valid() const -> bool { return data_ != nullptr && size_ > 0; }
  auto owned() const -> bool { return owned_; }
  auto toString() const -> std::string { return std::string(data_, size_); }

  // Reset ownership flag without freeing memory (used by LogEntry destructor)
  void ResetOwned() {
    data_ = nullptr;
    size_ = 0;
    owned_ = false;
  }

  // Require both slice are valid
  auto compare(const Slice &slice) -> int {
    assert(valid() && slice.valid());
    auto cmp_len = std::min(size(), slice.size());
    auto cmp_res = std::memcmp(data(), slice.data(), cmp_len);
    if (cmp_res != 0 || size() == slice.size()) {
      return cmp_res;
    }
    return size() > slice.size() ? 1 : -1;
  }

 private:
  char *data_ = nullptr;
  size_t size_ = 0;
  bool owned_ = false;  // Ownership flag: true = Slice owns the memory and will free it
};

// ============================================================================
//  FragmentPlacement — 单个 fragment 的放置记录（LRC 正交放置使用）
//  定义在 raft 命名空间内，避免跨命名空间依赖
// ============================================================================
struct FragmentPlacement {
  enum Kind {
    kData          = 0,
    kLocalParity   = 1,
    kGlobalParity  = 2,
  };

  int  frag_id     = 0;   // 条带内全局编号 [0, k+l+r)
  int  local_group = 0;   // 该 fragment 所属互补分区 [0, l)
  int  node_id     = 0;   // 物理节点 ID
  Kind kind        = kData;

  std::string KindString() const {
    switch (kind) {
      case kData:         return "data";
      case kLocalParity:  return "local_parity";
      case kGlobalParity: return "global_parity";
    }
    return "?";
  }

  std::string ToString() const {
    char buf[128];
    snprintf(buf, sizeof(buf), "frag=%d kind=%s LG=%d node=%d",
             frag_id, KindString().c_str(), local_group, node_id);
    return buf;
  }
};

class Stripe;
class LogEntry {
  friend class Serializer;

 public:
  LogEntry() = default;
  ~LogEntry() {
    // Release fragments (owned by LogEntry)
    // NOTE: We must reset owned_ flag after delete[] to prevent double-free
    // when fragments_.clear() destroys the Slice objects.
    for (auto &frag : fragments_) {
      if (frag.valid() && frag.owned()) {
        delete[] frag.data();
        frag.ResetOwned();  // Prevent double-free in clear()
      }
    }
    fragments_.clear();
    frag_ids_.clear();

    // Release owned Slice members
    // NOTE: After deleting the owned data, we call ResetOwned() to prevent
    // the automatic member destructor from trying to delete again.
    if (command_data_.valid() && command_data_.owned()) {
      delete[] command_data_.data();
      command_data_.ResetOwned();
    }

    if (not_encoded_slice_.valid() && not_encoded_slice_.owned()) {
      delete[] not_encoded_slice_.data();
      not_encoded_slice_.ResetOwned();
    }

    if (fragment_slice_.valid() && fragment_slice_.owned()) {
      delete[] fragment_slice_.data();
      fragment_slice_.ResetOwned();
    }
  }

  // Deep copy constructor for fragments
  LogEntry(const LogEntry &other)
      : term(other.term),
        index(other.index),
        type(other.type),
        chunk_info(other.chunk_info),
        start_fragment_offset(other.start_fragment_offset),
        command_size_(other.command_size_) {
    // Manually copy command_data_ to ensure deep copy
    if (other.command_data_.valid()) {
      char *data = new char[other.command_data_.size()];
      std::memcpy(data, other.command_data_.data(), other.command_data_.size());
      command_data_ = Slice::Take(data, other.command_data_.size());
    }

    if (other.not_encoded_slice_.valid()) {
      char *data = new char[other.not_encoded_slice_.size()];
      std::memcpy(data, other.not_encoded_slice_.data(), other.not_encoded_slice_.size());
      not_encoded_slice_ = Slice::Take(data, other.not_encoded_slice_.size());
    }

    if (other.fragment_slice_.valid()) {
      char *data = new char[other.fragment_slice_.size()];
      std::memcpy(data, other.fragment_slice_.data(), other.fragment_slice_.size());
      fragment_slice_ = Slice::Take(data, other.fragment_slice_.size());
    }

    placement_ = other.placement_;

    // Deep copy fragments
    for (const auto &frag : other.fragments_) {
      if (frag.valid()) {
        char *data = new char[frag.size()];
        std::memcpy(data, frag.data(), frag.size());
        fragments_.emplace_back(Slice::Take(data, frag.size()));
      } else {
        fragments_.emplace_back();
      }
    }

    // Copy frag_ids
    frag_ids_ = other.frag_ids_;
  }

  // [FIX] Deep copy assignment operator for fragments
  // The default assignment operator causes double-free when LogEntry objects
  // with LRC fragments are copied (e.g., in LogManager::appendEntryHelper)
  LogEntry &operator=(const LogEntry &other) {
    if (this == &other) return *this;

    // Free existing fragments
    // NOTE: Must reset owned_ flag after delete[] to prevent double-free
    for (auto &frag : fragments_) {
      if (frag.valid() && frag.owned()) {
        delete[] frag.data();
        frag.ResetOwned();  // Prevent double-free in clear()
      }
    }
    fragments_.clear();

    // Release owned Slice members
    if (command_data_.valid() && command_data_.owned()) {
      delete[] command_data_.data();
      command_data_.ResetOwned();
    }
    if (not_encoded_slice_.valid() && not_encoded_slice_.owned()) {
      delete[] not_encoded_slice_.data();
      not_encoded_slice_.ResetOwned();
    }
    if (fragment_slice_.valid() && fragment_slice_.owned()) {
      delete[] fragment_slice_.data();
      fragment_slice_.ResetOwned();
    }

    // Copy simple fields
    term = other.term;
    index = other.index;
    type = other.type;
    chunk_info = other.chunk_info;
    start_fragment_offset = other.start_fragment_offset;
    command_size_ = other.command_size_;

    // Deep copy Slice members
    if (other.command_data_.valid()) {
      char *data = new char[other.command_data_.size()];
      std::memcpy(data, other.command_data_.data(), other.command_data_.size());
      command_data_ = Slice::Take(data, other.command_data_.size());
    }
    if (other.not_encoded_slice_.valid()) {
      char *data = new char[other.not_encoded_slice_.size()];
      std::memcpy(data, other.not_encoded_slice_.data(), other.not_encoded_slice_.size());
      not_encoded_slice_ = Slice::Take(data, other.not_encoded_slice_.size());
    }
    if (other.fragment_slice_.valid()) {
      char *data = new char[other.fragment_slice_.size()];
      std::memcpy(data, other.fragment_slice_.data(), other.fragment_slice_.size());
      fragment_slice_ = Slice::Take(data, other.fragment_slice_.size());
    }

    placement_ = other.placement_;

    // Deep copy fragments
    for (const auto &frag : other.fragments_) {
      if (frag.valid()) {
        char *data = new char[frag.size()];
        std::memcpy(data, frag.data(), frag.size());
        fragments_.emplace_back(Slice::Take(data, frag.size()));
      } else {
        fragments_.emplace_back();
      }
    }

    // Copy frag_ids
    frag_ids_ = other.frag_ids_;

    return *this;
  }

  auto Index() const -> raft_index_t { return index; }
  void SetIndex(raft_index_t index) { this->index = index; }

  auto Term() const -> raft_term_t { return term; }
  void SetTerm(raft_term_t term) { this->term = term; }

  auto Type() const -> raft_entry_type { return type; }
  void SetType(raft_entry_type type) { this->type = type; }

  auto GetChunkInfo() const -> ChunkInfo { return chunk_info; }
  void SetChunkInfo(const ChunkInfo &chunk_info) { this->chunk_info = chunk_info; }

  auto StartOffset() const -> int { return start_fragment_offset; }
  void SetStartOffset(int off) { start_fragment_offset = off; }

  auto CommandData() const -> Slice { return Type() == kNormal ? command_data_ : Slice(); }
  auto CommandLength() const -> int { return command_size_; }
  void SetCommandLength(int size) { command_size_ = size; }

  void SetCommandData(const Slice &slice) {
    // 先释放旧的 command_data_（如果拥有所有权）
    if (command_data_.valid() && command_data_.owned()) {
      delete[] command_data_.data();
    }

    // 从 slice 复制数据并获取所有权
    if (slice.valid()) {
      char *data = new char[slice.size()];
      std::memcpy(data, slice.data(), slice.size());
      command_data_ = Slice::Take(data, slice.size());
    } else {
      command_data_ = Slice();
    }
    command_size_ = slice.size();
  }

  auto NotEncodedSlice() const -> Slice {
    return Type() == kNormal ? CommandData() : not_encoded_slice_;
  }
  void SetNotEncodedSlice(const Slice &slice) {
    // Safely release old memory if owned and valid
    if (not_encoded_slice_.valid() && not_encoded_slice_.owned() && not_encoded_slice_.data() != nullptr) {
      delete[] not_encoded_slice_.data();
    }
    // Deep copy and take ownership
    if (slice.valid() && slice.data() != nullptr) {
      char *data = new char[slice.size()];
      std::memcpy(data, slice.data(), slice.size());
      not_encoded_slice_ = Slice::Take(data, slice.size());
    } else {
      not_encoded_slice_ = Slice();
    }
  }

  auto FragmentSlice() const -> Slice { return Type() == kNormal ? Slice() : fragment_slice_; }
  void SetFragmentSlice(const Slice &slice) {
    // Safely release old memory if owned and valid
    if (fragment_slice_.valid() && fragment_slice_.owned() && fragment_slice_.data() != nullptr) {
      delete[] fragment_slice_.data();
    }
    // Deep copy and take ownership
    if (slice.valid() && slice.data() != nullptr) {
      char *data = new char[slice.size()];
      std::memcpy(data, slice.data(), slice.size());
      fragment_slice_ = Slice::Take(data, slice.size());
    } else {
      fragment_slice_ = Slice();
    }
  }

  // === LRC 相关接口 ===
  bool IsLrcEncoded() const { return chunk_info.IsLrcEncoded(); }
  int LrcGroupId() const { return chunk_info.GetLrcGroupId(); }

  const auto& Fragments() const { return fragments_; }
  const auto& FragIds() const { return frag_ids_; }
  void SetFragments(const std::vector<Slice> &frags) {
    // Deep copy fragments to avoid dangling pointers from shallow copy
    // This fixes use-after-free when frags is a temporary (e.g., in deserialization)

    // First, safely clear existing fragments
    // NOTE: Must reset owned_ flag after delete[] to prevent double-free
    for (auto &frag : fragments_) {
      if (frag.valid() && frag.owned()) {
        delete[] frag.data();
        frag.ResetOwned();
      }
    }
    fragments_.clear();
    frag_ids_.clear();

    // Limit the number of fragments to prevent buffer overflow issues
    const size_t kMaxFragments = 256;
    size_t count = std::min(frags.size(), kMaxFragments);
    fragments_.reserve(count);
    frag_ids_.reserve(count);

    for (size_t i = 0; i < count; ++i) {
      const auto &frag = frags[i];
      if (frag.valid() && frag.data() != nullptr) {
        // Validate the slice before copying
        if (frag.size() < 1024 * 1024) {  // Max 1MB per fragment
          char *data = new char[frag.size()];
          std::memcpy(data, frag.data(), frag.size());
          fragments_.emplace_back(Slice::Take(data, frag.size()));
          frag_ids_.push_back(static_cast<int>(i));  // Default: index = frag_id
        } else {
          // Invalid slice size, add empty
          fragments_.emplace_back();
          frag_ids_.push_back(-1);
        }
      } else {
        // Invalid slice, add empty
        fragments_.emplace_back();
        frag_ids_.push_back(-1);
      }
    }
  }
  void AddFragment(const Slice &frag) {
    if (!frag.valid() || frag.data() == nullptr) return;
    char *data = new char[frag.size()];
    std::memcpy(data, frag.data(), frag.size());
    fragments_.emplace_back(Slice::Take(data, frag.size()));
    frag_ids_.push_back(static_cast<int>(fragments_.size() - 1));  // Default: last index = frag_id
  }
  void AddFragmentWithId(int frag_id, const Slice &frag) {
    if (!frag.valid() || frag.data() == nullptr) return;
    char *data = new char[frag.size()];
    std::memcpy(data, frag.data(), frag.size());
    fragments_.emplace_back(Slice::Take(data, frag.size()));
    frag_ids_.push_back(frag_id);  // Use provided frag_id
    // Debug: print to stderr which will go to server log
  }

  const auto& Placement() const { return placement_; }
  void SetPlacement(const std::vector<FragmentPlacement> &p) { placement_ = p; }

  // 设置 frag_ids_ (在反序列化后调用)
  void SetFragIds(const std::vector<int> &ids) { frag_ids_ = ids; }

  // 根据 node_id 获取分配给该节点的 fragments
  std::vector<Slice> GetFragmentsForNode(int node_id) const;

  // 设置 LRC 参数（便捷方法）
  void SetLrcParams(int k, int l, int r, int lrc_group_id) {
    chunk_info.SetK(k);
    chunk_info.SetL(l);
    chunk_info.SetR(r);
    chunk_info.SetLrcGroupId(lrc_group_id);
  }

  // Serialization function required by RCF
  // void serialize(SF::Archive &ar);
  //
  // Dump some important information
  std::string ToString() const {
    char buf[256];
    sprintf(buf,
            "LogEntry{term=%d, index=%d, type=%s, chunkinfo=%s, "
            "commandlen=%d, start_off=%d}",
            Term(), Index(), EntryTypeToString(Type()), chunk_info.ToString().c_str(),
            CommandLength(), StartOffset());

    return std::string(buf);
  }

 private:
  // These three attributes are allocated when creating a command
  raft_term_t term;
  raft_index_t index;
  raft_entry_type type;  // Full entry or fragments

  // Information of this chunk that is contained in this raft entry
  ChunkInfo chunk_info;

  // [REQUIRE] specified by user, indicating the start offset of command
  // data for encoding
  int start_fragment_offset;
  int command_size_;

  Slice command_data_;       // Spcified by user, valid iff type = normal
  Slice not_encoded_slice_;  // Command data not being encoded
  Slice fragment_slice_;     // Fragments of encoded data

  // === LRC 分片数据 ===
  std::vector<Slice> fragments_;      // Leader: all_fragments, Follower: assigned_frags
  std::vector<int> frag_ids_;        // frag_id for each fragment (parallel to fragments_)
  std::vector<FragmentPlacement> placement_;  // 放置规则
};

auto operator==(const LogEntry &lhs, const LogEntry &rhs) -> bool;
}  // namespace raft
