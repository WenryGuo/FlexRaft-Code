#pragma once
// raft_store.h  —  Multi-Raft Actor System 的核心骨架
//
// 7-Phase Startup Architecture:
//   Phase 1: Parse config file
//   Phase 2: Create unified Raft RPC server (bind raft_addr)
//   Phase 3: Build connection pool to all other physical nodes
//   Phase 4: Build uniform group memberships locally (no RPC needed)
//   Phase 5: Create Raft instances directly from local memberships
//   Phase 6: Unified election start
//   Phase 7: Create KV RPC server (one per physical node)

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "encoding_mode.h"
#include "kv_node.h"
#include "type.h"
#include "lrc_encoder.h"
#include "lrc_placement.h"
#include "latency_matrix.h"
#include "lrc_complementary_grouper.h"
#include "mailbox.h"
#include "message.h"
#include "peer_fsm.h"
#include "peer_fsm_interface.h"
#include "stripe_apply.h"
#include "serializer.h"  // DecodeFixed32, SerializeForApply
#include "stripe_format.h"
#include "stripe_raft_adapter.h"
#include "rpc_router.h"
#include "rpc.h"
#include "raft_unified_rpc_server.h"
#include "raft_type.h"
#include "raft_node.h"
#include "batch_rpc_sender.h"
#include "rocksdb/write_batch.h"

namespace raft {
class RaftTransportHandler;
class RaftState;
}

namespace multiraft {

// =======================================================================
//  ApplyFsm  —  将已 commit 的 log entry 应用到 KV 状态机
// =======================================================================
class ApplyFsm {
 public:
  ApplyFsm(kv::KvServiceNode* kv_node, GroupId group_id, raft::raft_node_id_t node_id,
           EncodingMode enc_mode, int cluster_n)
      : kv_node_(kv_node),
        group_id_(group_id),
        node_id_(node_id),
        enc_mode_(enc_mode),
        cluster_n_(cluster_n),
        mb_(1024) {
    // [DEBUG] ApplyFsm constructor: kv_node=%p group=%u node=%u
    fprintf(stderr, "[DEBUG-APPLY-FSM] CTOR: kv_node=%p group=%u node=%u enc=%d n=%d\n",
            (void*)kv_node, group_id, node_id, (int)enc_mode, cluster_n);
    fflush(stderr);
  }

  Mailbox<ApplyMsg>& mailbox() { return mb_; }

  GroupId GetGroupId() const { return group_id_; }

  bool HandleBatch(std::vector<ApplyMsg>& batch) {
    for (auto& m : batch) {
      bool cont = std::visit([this](auto& msg) -> bool {
        return Handle(msg);
      }, m);
      if (!cont) return false;
    }
    return true;
  }

 private:
  bool Handle(MsgApplyCommitted& m) {
    // [DEBUG] ApplyFsm::Handle: kv_node_=%p group=%u node=%u
    fprintf(stderr, "[DEBUG-APPLY-FSM] Handle enter: kv_node_=%p group=%u node=%u entry=%u\n",
            (void*)kv_node_, group_id_, node_id_, m.entry_id);
    fflush(stderr);

    if (m.data.empty()) {
      if (m.on_applied) m.on_applied();
      return true;
    }

    const char* data = reinterpret_cast<const char*>(m.data.data());
    size_t data_len = m.data.size();

    // New unified payload format (APLY magic)
    // Format: [magic: 4 bytes = 'APLY'] [version: 4 bytes = 1] [kind: 1 byte]
    // Kind = kLrcFragments (3): [user_key_len: 4] [user_key] [ChunkInfo] [Placement count + entries] [Fragment count + data]
    // Kind = kLegacy (0): [command_len: 4] [command_data]
    if (data_len >= 9 &&
        data[0] == 'A' && data[1] == 'P' && data[2] == 'L' && data[3] == 'Y') {
      // Verify version
      uint32_t version = raft::DecodeFixed32(data + 4);
      if (version != 1) {
        fprintf(stderr, "[DEBUG-APPLY-FSM] Handle: unsupported APLY version %u\n", version);
        fflush(stderr);
      } else {
        uint8_t kind = static_cast<uint8_t>(data[8]);
        const char* payload = data + 9;
        size_t payload_len = data_len - 9;

        if (kind == 3) {  // kLrcFragments
          // Fail-fast: need at least 4 bytes for user_key_len
          if (payload_len < 4) {
            fprintf(stderr, "[DEBUG-APPLY-FSM] Handle: APLY kLrcFragments payload too short for user_key_len\n");
            fflush(stderr);
          } else {
            uint32_t user_key_len = raft::DecodeFixed32(payload);
            const char* user_key_str = payload + 4;
            size_t remaining = payload_len - 4;

            // Fail-fast: bounds check for user_key
            if (remaining < user_key_len) {
              fprintf(stderr, "[DEBUG-APPLY-FSM] Handle: APLY user_key_len=%u exceeds remaining %zu\n",
                      user_key_len, remaining);
              fflush(stderr);
            } else {
              std::string user_key(user_key_str, user_key_len);
              const char* chunk_ptr = user_key_str + user_key_len;
              size_t chunk_remaining = remaining - user_key_len;

              // Fail-fast: need at least 28 bytes for ChunkInfo (k:4, raft_index:4, l:4, r:4, lrc_group_id:4, key_size:4, total_size:4)
              if (chunk_remaining < 28) {
                fprintf(stderr, "[DEBUG-APPLY-FSM] Handle: APLY ChunkInfo too short\n");
                fflush(stderr);
              } else {
                raft::ChunkInfo chunk;
                chunk.k = raft::DecodeFixed32(chunk_ptr);
                // FIX: ChunkInfo format must match SerializeForApply
                // SerializeForApply writes: [k:4, raft_index:4, l:4, r:4, lrc_group_id:4, key_size:4, total_size:4]
                chunk.raft_index = raft::DecodeFixed32(chunk_ptr + 4);
                chunk.l = raft::DecodeFixed32(chunk_ptr + 8);
                chunk.r = raft::DecodeFixed32(chunk_ptr + 12);
                chunk.lrc_group_id = raft::DecodeFixed32(chunk_ptr + 16);
                chunk.key_size = raft::DecodeFixed32(chunk_ptr + 20);
                chunk.total_size = raft::DecodeFixed32(chunk_ptr + 24);
                const char* placement_ptr = chunk_ptr + 28;
                size_t placement_remaining = chunk_remaining - 28;

                // Fail-fast: need at least 4 bytes for placement count
                if (placement_remaining < 4) {
                  fprintf(stderr, "[DEBUG-APPLY-FSM] Handle: APLY placement count too short\n");
                  fflush(stderr);
                } else {
                  uint32_t placement_count = raft::DecodeFixed32(placement_ptr);
                  const char* frag_ptr = placement_ptr + 4;
                  size_t frag_remaining = placement_remaining - 4;

                  // Fail-fast: need at least placement_count * 16 bytes for placements
                  size_t placement_size = static_cast<size_t>(placement_count) * 16;
                  if (frag_remaining < placement_size) {
                    fprintf(stderr, "[DEBUG-APPLY-FSM] Handle: APLY placements exceed buffer\n");
                    fflush(stderr);
                  } else {
                    std::vector<raft::FragmentPlacement> placements;
                    placements.reserve(placement_count);
                    for (uint32_t i = 0; i < placement_count; ++i) {
                      raft::FragmentPlacement fp;
                      // FIX: FragmentPlacement format must match SerializeForApply
                      // SerializeForApply writes: [node_id:4, frag_id:4, local_group:4, kind:4]
                      // NOT: [frag_id:4, local_group:4, node_id:4, kind:4]
                      fp.node_id = raft::DecodeFixed32(frag_ptr + i * 16);
                      fp.frag_id = raft::DecodeFixed32(frag_ptr + i * 16 + 4);
                      fp.local_group = raft::DecodeFixed32(frag_ptr + i * 16 + 8);
                      fp.kind = static_cast<raft::FragmentPlacement::Kind>(
                          raft::DecodeFixed32(frag_ptr + i * 16 + 12));
                      placements.push_back(fp);
                    }
                    const char* count_ptr = frag_ptr + placement_size;
                    size_t count_remaining = frag_remaining - placement_size;

                    // Fail-fast: need at least 4 bytes for fragment count
                    if (count_remaining < 4) {
                      fprintf(stderr, "[DEBUG-APPLY-FSM] Handle: APLY fragment count too short\n");
                      fflush(stderr);
                    } else {
                      uint32_t frag_count = raft::DecodeFixed32(count_ptr);
                      const char* frag_data_ptr = count_ptr + 4;
                      size_t frag_data_remaining = count_remaining - 4;

                      std::vector<raft::Slice> fragments;
                      fragments.reserve(frag_count);

                      // Build a map from frag_id to fragment data
                      // New format: [frag_id(4) + frag_size(4) + frag_data(N)] per fragment
                      std::unordered_map<uint32_t, raft::Slice> frag_map;
                      frag_map.reserve(frag_count);

                      // Fail-fast: bounds check for fragment data
                      for (uint32_t i = 0; i < frag_count; ++i) {
                        // Read frag_id (4 bytes)
                        if (frag_data_remaining < 4) {
                          fprintf(stderr, "[DEBUG-APPLY-FSM] Handle: APLY frag %u missing frag_id\n", i);
                          fflush(stderr);
                          break;
                        }
                        uint32_t frag_id = raft::DecodeFixed32(frag_data_ptr);
                        frag_data_ptr += 4;
                        frag_data_remaining -= 4;

                        // Read frag_size (4 bytes)
                        if (frag_data_remaining < 4) {
                          fprintf(stderr, "[DEBUG-APPLY-FSM] Handle: APLY frag_id=%u missing size\n", frag_id);
                          fflush(stderr);
                          break;
                        }
                        uint32_t frag_size = raft::DecodeFixed32(frag_data_ptr);
                        const char* frag_bytes = frag_data_ptr + 4;
                        frag_data_remaining -= 4;

                        if (frag_data_remaining < frag_size) {
                          fprintf(stderr, "[DEBUG-APPLY-FSM] Handle: APLY frag_id=%u data exceeds buffer\n", frag_id);
                          fflush(stderr);
                          break;
                        }

                        // Use Slice::Copy() to avoid dangling references
                        raft::Slice src(const_cast<char*>(frag_bytes), frag_size);
                        raft::Slice owned_frag = raft::Slice::Copy(src);
                        // Store by frag_id in the map
                        frag_map[frag_id] = owned_frag;
                        // Also store in vector at position i for direct access
                        fragments.push_back(owned_frag);
                        frag_data_ptr = frag_bytes + frag_size;
                        frag_data_remaining -= frag_size;
                      }

                      // Parse original_size (8 bytes, appended after fragments)
                      size_t original_size = 0;
                      if (frag_data_remaining >= 8) {
                        original_size = raft::DecodeFixed64(frag_data_ptr);
                        frag_data_ptr += 8;
                        // frag_data_remaining -= 8;  // not needed after this point
                      }

                      // Now write to KV store
                      // Write meta to MetaCF
                      multiraft::StripeLogMeta meta;
                      meta.user_key = user_key;
                      meta.stripe_id = m.stripe_id;
                      meta.entry_id = m.entry_id;
                      meta.group_id = group_id_;
                      meta.encoding_mode = enc_mode_;
                      meta.k = static_cast<int>(chunk.k);
                      meta.l = static_cast<int>(chunk.l);
                      meta.r = static_cast<int>(chunk.r);
                      meta.m = 0;
                      meta.original_size = original_size;
                      // Build placements from parsed data
                      for (const auto& fp : placements) {
                        multiraft::FragmentPlacement fp_out;
                        fp_out.frag_id = fp.frag_id;
                        fp_out.local_group = fp.local_group;
                        fp_out.node_id = fp.node_id;
                        fp_out.kind = static_cast<multiraft::FragmentPlacement::Kind>(
                            static_cast<uint32_t>(fp.kind));
                        meta.placement.push_back(fp_out);
                      }

                      std::string meta_key = StripeMetaDbKey(group_id_, user_key);
                      std::string meta_val = meta.Serialize();

                      // Atomic WriteBatch: meta + local fragments in one RocksDB write
                      rocksdb::WriteBatch batch;
                      batch.Put(meta_key, meta_val);

                      // Write local fragments to DataCF
                      // Only write fragments that this node should store (based on placement)
                      for (const auto& fp : meta.placement) {
                        // Only write fragments stored on this node
                        if (fp.node_id != node_id_) {
                          continue;
                        }
                        // Lookup fragment by frag_id using the map
                        auto it = frag_map.find(static_cast<uint32_t>(fp.frag_id));
                        if (it == frag_map.end()) {
                          fprintf(stderr, "[DEBUG-APPLY-FSM] Handle: frag_id=%d not found in frag_map\n",
                                  fp.frag_id);
                          fflush(stderr);
                          continue;
                        }
                        std::string data_key = FragDbKey(group_id_, m.stripe_id, fp.frag_id);
                        const raft::Slice& frag = it->second;
                        batch.Put(data_key, std::string(frag.data(), frag.size()));
                      }

                      kv_node_->GetKvServer()->DB()->Write(&batch);

                      // Defensive sanity check: re-read the meta to make sure the key was
                      // actually written correctly (catches off-by-N bugs in key_size
                      // calculation, key-vs-group-id mixups, etc.).
                      std::string sanity_value;
                      kv::StorageEngine* db_for_sanity = kv_node_->GetKvServer()->DB();
                      bool readback_ok =
                          db_for_sanity->Get(meta_key, &sanity_value) && sanity_value == meta_val;
                      if (!readback_ok) {
                        fprintf(stderr,
                                "[APPLY-FSM][N%d] meta write/readback mismatch: meta_key='%s' "
                                "expected_len=%zu got_len=%zu\n",
                                node_id_, meta_key.c_str(), meta_val.size(),
                                sanity_value.size());
                        fflush(stderr);
                      }

                      fprintf(stderr, "[DEBUG-APPLY-FSM] Handle: APLY kLrcFragments meta=%s frags=%zu\n",
                              user_key.c_str(), fragments.size());
                      fflush(stderr);
                    }
                  }
                }
              }
            }
          }
        } else if (kind == 0) {  // kLegacy
          // Fail-fast: need at least 4 bytes for command_len
          if (payload_len < 4) {
            fprintf(stderr, "[DEBUG-APPLY-FSM] Handle: APLY kLegacy payload too short for command_len\n");
            fflush(stderr);
          } else {
            uint32_t cmd_len = raft::DecodeFixed32(payload);
            if (payload_len >= 4 + cmd_len) {
              fprintf(stderr, "[DEBUG-APPLY-FSM] Handle: APLY kLegacy cmd_len=%u\n", cmd_len);
              fflush(stderr);
              std::string key = "__lrc__:" + std::to_string(m.entry_id) + ":" +
                               std::to_string(m.stripe_id) + ":" + std::to_string(m.group_id);
              std::string value(payload + 4, cmd_len);
              kv_node_->GetKvServer()->DB()->Put(key, value);
            } else {
              fprintf(stderr, "[DEBUG-APPLY-FSM] Handle: APLY kLegacy cmd_len=%u exceeds payload %zu\n",
                      cmd_len, payload_len - 4);
              fflush(stderr);
            }
          }
        } else {
          fprintf(stderr, "[DEBUG-APPLY-FSM] Handle: unknown APLY kind %u\n", kind);
          fflush(stderr);
        }
      }
    } else {
      // Legacy format handling (original code path)
      auto kind = static_cast<ApplyPayloadKind>(m.data[0]);
      const char* p = reinterpret_cast<const char*>(m.data.data() + 1);
      size_t plen = m.data.size() - 1;

      switch (kind) {
        case ApplyPayloadKind::kStripePacked: {
          if (plen < 4) break;
          uint32_t flen = 0;
          std::memcpy(&flen, p, 4);
          if (plen < 4 + flen) break;
          fprintf(stderr, "[DEBUG-APPLY-FSM] Handle: kStripePacked flen=%u\n", flen);
          fflush(stderr);
          ApplyStripePacked(kv_node_, group_id_, node_id_, std::string(p + 4, flen));
          break;
        }
        case ApplyPayloadKind::kStripeCommand: {
          if (plen < 4) break;
          uint32_t clen = 0;
          std::memcpy(&clen, p, 4);
          if (plen < 4 + clen) break;
          fprintf(stderr, "[DEBUG-APPLY-FSM] Handle: kStripeCommand clen=%u\n", clen);
          fflush(stderr);
          ApplyStripeCommand(kv_node_, group_id_, node_id_, enc_mode_, cluster_n_, m.entry_id,
                           std::string(p + 4, clen));
          break;
        }
        default: {
          fprintf(stderr, "[DEBUG-APPLY-FSM] Handle: kLegacy plen=%zu\n", plen);
          fflush(stderr);
          std::string key = "__lrc__:" + std::to_string(m.entry_id) + ":" +
                            std::to_string(m.stripe_id) + ":" + std::to_string(m.group_id);
          std::string value(p, plen);
          kv_node_->GetKvServer()->DB()->Put(key, value);
          break;
        }
      }
    }

    // Update applied_index_ so that ExecuteGetOperation can spin-wait correctly.
    kv_node_->UpdateAppliedIndex(m.entry_id);

    if (m.on_applied) m.on_applied();
    return true;
  }

  bool Handle(MsgApplyStop&) {
    // [COMMENTED] ApplyFsm stop log
    return false;
  }

  kv::KvServiceNode*       kv_node_;
  GroupId                  group_id_;
  raft::raft_node_id_t     node_id_;
  EncodingMode             enc_mode_;
  int                      cluster_n_;
  // Unbounded MPSC mailbox for committed entries from RaftState.
  // Backpressure is handled upstream via BatchTransport max_buffer_size.
  Mailbox<ApplyMsg>        mb_;
};

// =======================================================================
//  CrossGroupTracker  —  跨 group commit 聚合
// =======================================================================
using EntryCommitCallback = std::function<void(EntryId)>;

class CrossGroupTracker : public CrossGroupTrackerInterface {
 public:
  explicit CrossGroupTracker(int num_groups) : num_groups_(num_groups) {}

  void SetGroupCount(int num_groups) { num_groups_ = num_groups; }

  void Register(EntryId eid, std::function<void(EntryId)> cb) override {
    std::lock_guard<std::mutex> lk(mu_);
    states_[eid] = State{0, std::move(cb)};
    // [COMMENTED] Tracker register log (runtime verbose)
  }

  void OnLocalCommit(EntryId eid, GroupId gid) override {
    std::function<void(EntryId)> cb;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto it = states_.find(eid);
      if (it == states_.end()) {
        // [COMMENTED] Tracker warning: entry not found
        return;
      }

      uint64_t mask = uint64_t(1) << gid;
      if (it->second.mask & mask) {
        // [COMMENTED] Tracker warning: group already committed
        return;
      }
      it->second.mask |= mask;

      // [COMMENTED] Tracker group commit progress log

      if (__builtin_popcountll(it->second.mask) == num_groups_) {
        cb = std::move(it->second.cb);
        states_.erase(it);
      }
    }
    if (cb) {
      // [COMMENTED] Tracker all groups committed callback log
      cb(eid);
    }
  }

  void Abort(EntryId eid) override {
    std::lock_guard<std::mutex> lk(mu_);
    // [COMMENTED] Tracker abort log
    states_.erase(eid);
  }

 private:
  struct State {
    uint64_t             mask = 0;
    std::function<void(EntryId)> cb;
  };

  int num_groups_;
  std::mutex mu_;
  std::unordered_map<EntryId, State> states_;
};

// =======================================================================
//  RoutingTableManager  —  路由表管理
// =======================================================================
class RoutingTableManager : public RoutingTableManagerInterface {
 public:
  RoutingTableManager(int num_groups) : num_groups_(num_groups) {}

  void SetGroupCount(int num_groups) { num_groups_ = num_groups; }

  void AddRoute(const GroupTopology& entry) override {
    std::unique_lock<std::shared_mutex> lk(rw_mutex_);
    routing_table_[entry.stripe_id] = entry;
    // [COMMENTED] Route added log (runtime verbose)
  }

  bool GetRoute(StripeId stripe_id, GroupTopology* out) const {
    std::shared_lock<std::shared_mutex> lk(rw_mutex_);
    auto it = routing_table_.find(stripe_id);
    if (it != routing_table_.end()) {
      *out = it->second;
      return true;
    }
    return false;
  }

  RoutingTable GetAllRoutes() const override {
    std::shared_lock<std::shared_mutex> lk(rw_mutex_);
    return routing_table_;
  }

  void MergeRoutes(const RoutingTable& other, int ttl) override {
    if (ttl <= 0) return;
    std::unique_lock<std::shared_mutex> lk(rw_mutex_);
    int merged = 0;
    for (const auto& [stripe_id, entry] : other) {
      auto it = routing_table_.find(stripe_id);
      if (it == routing_table_.end()) {
        routing_table_[stripe_id] = entry;
        merged++;
      }
    }
    if (merged > 0) {
      // [COMMENTED] Route merged log (runtime verbose)
    }
  }

  int Size() const override {
    std::shared_lock<std::shared_mutex> lk(rw_mutex_);
    return static_cast<int>(routing_table_.size());
  }

  void* GetSelf() override { return this; }

  std::vector<GroupId> GetComplementaryGroups(GroupId group_id, int N) const {
    std::shared_lock<std::shared_mutex> lk(rw_mutex_);
    for (const auto& [sid, topo] : routing_table_) {
      for (const auto& cs : topo.complementary_sets) {
        for (GroupId g : cs.complementary_groups) {
          if (g == group_id) return cs.complementary_groups;
        }
      }
    }
    return {};
  }

 private:
  int num_groups_;
  mutable std::shared_mutex rw_mutex_;
  RoutingTable routing_table_;
};

// =======================================================================
//  RaftRouter  —  消息路由
// =======================================================================
class RaftRouter : public RaftRouterInterface {
 public:
  using PeerMailbox = Mailbox<PeerMsg>;

  RaftRouter(int node_id) : node_id_(node_id) {}

  void Register(GroupId gid, raft::raft_node_id_t nid, PeerMailbox* mb) {
    std::lock_guard<std::mutex> lk(mu_);
    local_peers_[Key(gid, nid)] = mb;
    printf("[ROUTER-N%d] Registered PeerFsm: group=%u node=%u\n", node_id_, gid, nid);
  }

  bool SendToPeer(GroupId to_group, raft::raft_node_id_t to_node, PeerMsg msg) override {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = local_peers_.find(Key(to_group, to_node));
    if (it != local_peers_.end()) {
      it->second->push(std::move(msg));
      return true;
    }
    printf("[ROUTER-N%d] Peer not found locally: group=%u node=%u\n",
           node_id_, to_group, to_node);
    return false;
  }

  void BroadcastToGroup(GroupId to_group, PeerMsg msg) override {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [key, mb] : local_peers_) {
      if (GroupFromKey(key) == to_group) {
        mb->push(std::move(msg));
      }
    }
  }

  PeerMailbox* GetGroupMailbox(GroupId to_group) override {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [key, mb] : local_peers_) {
      if (GroupFromKey(key) == to_group) {
        return mb;
      }
    }
    return nullptr;
  }

  int GetLocalPeerCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int>(local_peers_.size());
  }

 private:
  static uint64_t Key(GroupId gid, raft::raft_node_id_t nid) {
    return (uint64_t(gid) << 32) | uint32_t(nid);
  }
  static GroupId GroupFromKey(uint64_t key) {
    return GroupId(key >> 32);
  }

  int node_id_;
  mutable std::mutex mu_;
  std::unordered_map<uint64_t, PeerMailbox*> local_peers_;
};

// =======================================================================
//  BatchSystem  —  少量 Poll 线程驱动所有 PeerFsm 和 ApplyFsm
// =======================================================================
struct FsmEntry {
  std::unique_ptr<PeerFsm>     peer_fsm;
  Mailbox<PeerMsg>             peer_mb{4096};  // 有界队列，防止无界内存增长
  std::atomic<bool>            peer_locked{false};
};

struct ApplyEntry {
  std::unique_ptr<ApplyFsm>  apply_fsm;
  Mailbox<ApplyMsg>          apply_mb{1024};  // 有界队列，防止无界内存增长
  std::atomic<bool>           apply_locked{false};
};

class BatchSystem {
 public:
  BatchSystem(int num_peer_threads, int num_apply_threads, int max_batch, int node_id)
      : max_batch_(max_batch), running_(false), node_id_(node_id) {
    peer_thread_count_  = num_peer_threads;
    apply_thread_count_ = num_apply_threads;
  }

  void RegisterPeer(std::unique_ptr<PeerFsm> fsm, Mailbox<PeerMsg>** out_mb) {
    auto entry    = std::make_unique<FsmEntry>();
    entry->peer_fsm = std::move(fsm);
    peer_entries_.push_back(std::move(entry));
    *out_mb         = &peer_entries_.back()->peer_mb;  // Fix: use pointer AFTER push_back
    printf("[BATCH-N%d] Registered PeerFsm, total peers=%zu\n",
           node_id_, peer_entries_.size());
  }

  void RegisterApply(std::unique_ptr<ApplyFsm> fsm, Mailbox<ApplyMsg>** out_mb) {
    auto entry      = std::make_unique<ApplyEntry>();
    entry->apply_fsm = std::move(fsm);
    *out_mb          = &entry->apply_fsm->mailbox();
    apply_entries_.push_back(std::move(entry));
    printf("[BATCH-N%d] Registered ApplyFsm, total applies=%zu\n",
           node_id_, apply_entries_.size());
  }

  // Build partition tables after all registrations are done (called from Start())
  void BuildPartitions();

  void Start() {
    running_.store(true);
    BuildPartitions();
    for (int i = 0; i < peer_thread_count_; ++i) {
      peer_threads_.emplace_back([this, i] { PeerPollLoop(i); });
    }
    for (int i = 0; i < apply_thread_count_; ++i) {
      apply_threads_.emplace_back([this, i] { ApplyPollLoop(i); });
    }
    tick_thread_ = std::thread([this] { TickLoop(); });
    printf("[BATCH-N%d] Started: %d peer + %d apply threads\n",
           node_id_, peer_thread_count_, apply_thread_count_);
  }

  void Stop() {
    running_.store(false);
    for (auto& e : peer_entries_)   e->peer_mb.push(MsgStop{});
    for (auto& e : apply_entries_)   e->apply_fsm->mailbox().push(MsgApplyStop{});
    for (auto& t : peer_threads_)   if (t.joinable()) t.join();
    for (auto& t : apply_threads_)  if (t.joinable()) t.join();
    if (tick_thread_.joinable()) tick_thread_.join();
    printf("[BATCH-N%d] BatchSystem stopped\n", node_id_);
  }

  int GetPeerFsmCount() const { return static_cast<int>(peer_entries_.size()); }
  int GetApplyFsmCount() const { return static_cast<int>(apply_entries_.size()); }

  PeerFsm* GetPeerFsm(int index) const {
    if (index >= 0 && index < static_cast<int>(peer_entries_.size())) {
      return peer_entries_[index]->peer_fsm.get();
    }
    return nullptr;
  }

  // Backpressure monitoring: check if any ApplyFsm mailbox is nearly full
  bool IsAnyMailboxNearlyFull() const {
    for (auto& e : apply_entries_) {
      if (e->apply_fsm->mailbox().nearly_full()) return true;
    }
    return false;
  }

 private:
  // Partition-based polling: each thread owns a subset of entries.
  // This enables true parallelism — different groups are processed simultaneously.
  void PeerPollLoop(int thread_id);
  void ApplyPollLoop(int thread_id);

  void TickLoop() {
    while (running_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      for (auto& entry : peer_entries_)
        entry->peer_mb.push(MsgTick{});
    }
  }

  int                                    max_batch_;
  int                                    peer_thread_count_;
  int                                    apply_thread_count_;
  std::atomic<bool>                      running_;
  int                                    node_id_;
  std::vector<std::unique_ptr<FsmEntry>>  peer_entries_;
  std::vector<std::unique_ptr<ApplyEntry>> apply_entries_;
  std::vector<std::thread>               peer_threads_;
  std::vector<std::thread>               apply_threads_;
  std::thread                            tick_thread_;

  // Partitions: each thread owns a specific set of entries.
  // thread i owns entries[partition_start[i] .. partition_end[i)-1]
  // When num_entries == 0 or threads == 0, partition vectors are empty.
  std::vector<size_t> peer_part_start_;
  std::vector<size_t> peer_part_end_;
  std::vector<size_t> apply_part_start_;
  std::vector<size_t> apply_part_end_;
};

// =======================================================================
//  GroupMembership  —  分组成员信息
// =======================================================================
struct GroupMembership {
  GroupId               group_id;
  raft::raft_node_id_t  local_node_id;
  kv::KvClusterConfig    group_cluster_cfg;
};

// =======================================================================
//  GroupRegistry  —  物理节点上所有 KvServiceNode 的注册表
// =======================================================================
struct NodeKey {
  GroupId              group_id;
  raft::raft_node_id_t local_node_id;
  bool operator==(const NodeKey& o) const {
    return group_id == o.group_id && local_node_id == o.local_node_id;
  }
};
struct NodeKeyHash {
  size_t operator()(const NodeKey& k) const {
    return std::hash<uint64_t>()((uint64_t(k.group_id) << 32) |
                                  uint32_t(k.local_node_id));
  }
};

class RaftStore;

class GroupRegistry : public kv::rpc::I_KvServerRPCService {
 public:
  GroupRegistry(int node_id, const kv::rpc::NetAddress& kv_listen_addr);
  ~GroupRegistry();

  kv::KvServiceNode* Register(GroupId gid, raft::raft_node_id_t local_nid,
                              const kv::KvClusterConfig& group_cluster_cfg);

  kv::KvServiceNode* Get(GroupId gid, raft::raft_node_id_t local_nid);

  void InitAll();
  void StartAll();
  void StartRpcServer();
  void StopAll();

  RpcRouter* GetRpcRouter() { return rpc_router_.get(); }
  kv::rpc::KvServerRPCServer* GetRpcServer() { return rpc_server_.get(); }

  void RegisterPeerMailbox(GroupId gid, raft::raft_node_id_t local_nid,
                          Mailbox<PeerMsg>* mb) {
    rpc_router_->RegisterPeerMailbox(gid, local_nid, mb);
  }

  int GetNodeCount() const { return static_cast<int>(nodes_.size()); }

  // ---- I_KvServerRPCService dispatcher implementation ----
  // Route DealWithRequest to the correct group-specific KvServer based on group_id
  kv::Response DealWithRequest(const kv::Request& req);
  // GetValue is less common in multi-raft; route to group 0 as default
  kv::GetValueResponse GetValue(const kv::GetValueRequest& req);

  // Mirrored from RaftStore::SetEncodingMode — used when logging incoming KV RPC writes.
  void SetEncodingMode(EncodingMode m);

  EncodingMode GetEncodingMode() const { return encoding_mode_; }

  void SetRaftStore(RaftStore* store) { raft_store_ = store; }

 private:
  RaftStore* raft_store_ = nullptr;
  // Route to the local KvServiceNode for the given group
  kv::KvServiceNode* GetNodeForGroup(raft::raft_group_id_t group_id);
  int node_id_;
  kv::rpc::NetAddress listen_addr_;
  std::unordered_map<NodeKey, std::unique_ptr<kv::KvServiceNode>, NodeKeyHash> nodes_;
  // Fast lookup: group_id → local KvServiceNode (keyed by gid, node_id_)
  std::unordered_map<raft::raft_group_id_t, kv::KvServiceNode*> local_nodes_;
  std::unique_ptr<kv::rpc::KvServerRPCServer> rpc_server_;
  std::unique_ptr<RpcRouter> rpc_router_;
  EncodingMode encoding_mode_ = EncodingMode::kRsF;
};

// =======================================================================
//  GossipThread  —  Gossip 协议路由表同步
// =======================================================================
class GossipThread {
 public:
  GossipThread(RoutingTableManager* routing_table, int node_id)
      : routing_table_(routing_table), node_id_(node_id), running_(false) {}

  void Start() {
    running_ = true;
    gossip_thread_ = std::thread([this] { GossipLoop(); });
    printf("[GOSSIP-N%d] Gossip thread started\n", node_id_);
  }

  void Stop() {
    running_ = false;
    if (gossip_thread_.joinable()) gossip_thread_.join();
    printf("[GOSSIP-N%d] Gossip thread stopped\n", node_id_);
  }

 private:
  void GossipLoop() {
    while (running_) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      // [COMMENTED] Gossip routing table size periodic log (runtime verbose)
    }
  }

  RoutingTableManager* routing_table_;
  int                  node_id_;
  std::atomic<bool>    running_;
  std::thread          gossip_thread_;
};

// =======================================================================
//  RaftStore  —  顶层入口
// =======================================================================
class RaftStore : public RaftStoreInterface {
 public:
  // Phase-based constructor: defers Raft instance creation
  RaftStore(int node_id,
            const raft::rpc::NetAddress& raft_listen_addr,
            const kv::rpc::NetAddress& kv_listen_addr,
            int k = 1, int r = 0,
            int peer_threads  = 2,
            int apply_threads = 2,
            int max_batch     = 64)
      : node_id_(node_id),
        k_(k), r_(r), l_(1), local_k_(k),
        tracker_(0),
        routing_table_(0),
        registry_(node_id, kv_listen_addr),
        router_(node_id),
        batch_system_(peer_threads, apply_threads, max_batch, node_id),
        gossip_(&routing_table_, node_id),
        encoder_(new raft::Encoder()),
        cluster_size_(0),
        kv_listen_addr_(kv_listen_addr),
        raft_listen_addr_(raft_listen_addr) {

    printf("\n========================================\n");
    printf("[STORE-N%d] RaftStore created (phase-based)\n", node_id_);
    registry_.SetRaftStore(this);
    printf("[STORE-N%d] Raft RPC: %s:%d\n", node_id_, raft_listen_addr.ip.c_str(), raft_listen_addr.port);
    printf("[STORE-N%d] KV RPC:   %s:%d\n", node_id_, kv_listen_addr.ip.c_str(), kv_listen_addr.port);
    printf("========================================\n\n");

    unified_raft_rpc_server_ = std::make_unique<raft::rpc::RaftUnifiedRpcServer>(
        node_id_, raft_listen_addr);
    raft::rpc::RaftUnifiedRpcServer::SetGlobalService(unified_raft_rpc_server_->GetService());
    printf("[STORE-N%d] Unified Raft RPC server created\n", node_id_);

    batch_transport_manager_ = std::make_unique<raft::RaftBatchTransportManager>();
  }

  ~RaftStore() { Stop(); }

  // ---- Phase 5: Create all Raft instances ----
  void CreateRaftInstances(const std::vector<GroupMembership>& memberships) {
    printf("\n[STORE-N%d] Creating %zu Raft instances...\n", node_id_, memberships.size());
    memberships_ = memberships;

    std::unordered_map<raft::raft_node_id_t, raft::rpc::NetAddress> peer_addrs;
    for (const auto& mem : memberships) {
      for (const auto& [nid, addr_info] : mem.group_cluster_cfg) {
        if (peer_addrs.find(nid) == peer_addrs.end()) {
          peer_addrs[nid] = addr_info.raft_rpc_addr;
        }
      }
    }
    if (batch_transport_manager_ && !peer_addrs.empty()) {
      batch_transport_manager_->Init(static_cast<raft::raft_node_id_t>(node_id_), peer_addrs);
    }

    for (const auto& mem : memberships) {
      std::string cluster_cfg_keys;
      for (const auto& [k, v] : mem.group_cluster_cfg) {
        cluster_cfg_keys += std::to_string(k) + " ";
      }
      printf("[PHASE-5] Inspecting membership: group=%u local_id=%u cluster_cfg={%s}\n",
             mem.group_id, mem.local_node_id, cluster_cfg_keys.c_str());
      fflush(stdout);

      // Validate membership before registration
      if (mem.group_cluster_cfg.empty()) {
        fprintf(stderr, "[STORE-N%d] FATAL: group %u local_id %u: cluster_cfg is empty\n",
                node_id_, mem.group_id, mem.local_node_id);
        fflush(stderr);
        std::abort();
      }
      if (mem.group_cluster_cfg.count(mem.local_node_id) == 0) {
        fprintf(stderr, "[STORE-N%d] FATAL: group %u local_id %u: local_node_id NOT in cluster_cfg\n",
                node_id_, mem.group_id, mem.local_node_id);
        fprintf(stderr, "[STORE-N%d]   cluster_cfg keys: %s\n",
                node_id_, cluster_cfg_keys.c_str());
        fprintf(stderr, "[STORE-N%d]   local_node_id = %u\n",
                node_id_, mem.local_node_id);
        fflush(stderr);
        std::abort();
      }

      kv::KvServiceNode* node = nullptr;
      try {
        node = registry_.Register(mem.group_id, mem.local_node_id, mem.group_cluster_cfg);
      } catch (const std::exception& e) {
        fprintf(stderr, "[STORE-N%d] FATAL: registry_.Register exception for group=%u: %s\n",
                node_id_, mem.group_id, e.what());
        fflush(stderr);
        std::abort();
      } catch (...) {
        fprintf(stderr, "[STORE-N%d] FATAL: registry_.Register unknown exception for group=%u\n",
                node_id_, mem.group_id);
        fflush(stderr);
        std::abort();
      }

      // ================================================================
      // Create RaftNode and inject into KvServiceNode
      // This is the core of Multi-Raft: one RaftState per group
      // ================================================================
      raft::RaftNode::NodeConfig node_config;
      node_config.node_id_me = mem.local_node_id;
      for (const auto& [peer_id, addr_info] : mem.group_cluster_cfg) {
        node_config.servers[peer_id] = addr_info.raft_rpc_addr;
      }
      node_config.N_physical_nodes = physical_cluster_size_;
      node_config.encoding_mode = static_cast<int>(encoding_mode_);

      printf("[STORE-N%d] Creating RaftNode for group=%u local_id=%u...\n",
             node_id_, mem.group_id, mem.local_node_id);
      fflush(stdout);
      try {
        auto raft_node = std::make_unique<raft::RaftNode>(node_config);
        {
          // Write lock for raft_nodes_ map modification
          std::unique_lock<std::shared_mutex> lock(raft_nodes_mutex_);
          raft_nodes_[mem.group_id] = std::move(raft_node);
        }
      } catch (const std::exception& e) {
        fprintf(stderr, "[STORE-N%d] FATAL: RaftNode constructor threw for group=%u: %s\n",
                node_id_, mem.group_id, e.what());
        fflush(stderr);
        std::abort();
      } catch (...) {
        fprintf(stderr, "[STORE-N%d] FATAL: RaftNode constructor threw unknown for group=%u\n",
                node_id_, mem.group_id);
        fflush(stderr);
        std::abort();
      }
      raft_nodes_[mem.group_id]->SetUnifiedRpcService(unified_raft_rpc_server_->GetService());

      // Set ApplyFsm mailbox as RSM bridge
      raft_nodes_[mem.group_id]->SetRsmAndApplyMailbox(
          mem.group_id, mem.local_node_id,
          &batch_system_, this);

      // Inject into KvServiceNode (owned=false: RaftStore manages RaftNode lifecycle)
      node->SetRaftNode(raft_nodes_[mem.group_id].get(), false);

      // Set up peer KV stubs for cross-node GetValue RPCs (stripe_read path)
      if (!peer_kv_addrs_.empty()) {
        std::vector<raft::raft_node_id_t> peer_ids;
        for (size_t i = 0; i < peer_kv_addrs_.size(); ++i) {
          peer_ids.push_back(static_cast<raft::raft_node_id_t>(i));
        }
        node->SetKVPeerServers(peer_kv_addrs_, peer_ids);
      }

      printf("[STORE-N%d] Created RaftNode for group=%u local_id=%u (addr=%s:%d)\n",
             node_id_, mem.group_id, mem.local_node_id,
             node_config.servers.at(mem.local_node_id).ip.c_str(),
             node_config.servers.at(mem.local_node_id).port);
      fflush(stdout);

      // Create and register ApplyFsm
      printf("[STORE-N%d] Creating ApplyFsm for group=%u...\n", node_id_, mem.group_id);
      fflush(stdout);
      Mailbox<ApplyMsg>* apply_mb = nullptr;
      int cluster_n = physical_cluster_size_ > 0
                          ? physical_cluster_size_
                          : static_cast<int>(mem.group_cluster_cfg.size());
      auto apply_fsm = std::make_unique<ApplyFsm>(node, mem.group_id, mem.local_node_id,
                                                  encoding_mode_, cluster_n);
      try {
        batch_system_.RegisterApply(std::move(apply_fsm), &apply_mb);
      } catch (const std::exception& e) {
        fprintf(stderr, "[STORE-N%d] FATAL: RegisterApply threw for group=%u: %s\n",
                node_id_, mem.group_id, e.what());
        fflush(stderr);
        std::abort();
      } catch (...) {
        fprintf(stderr, "[STORE-N%d] FATAL: RegisterApply threw unknown for group=%u\n",
                node_id_, mem.group_id);
        fflush(stderr);
        std::abort();
      }
      raft_nodes_[mem.group_id]->RegisterApplyMailbox(apply_mb);
      printf("[BATCH-N%d] Registered ApplyFsm, total applies=%zu\n",
             node_id_, batch_system_.GetApplyFsmCount());
      fflush(stdout);
      printf("[RAFT-NODE-N%d] ApplyFsm mailbox registered for group=%u\n",
             mem.local_node_id, mem.group_id);
      fflush(stdout);

      // Create and register PeerFsm
      printf("[STORE-N%d] Creating PeerFsm for group=%u...\n", node_id_, mem.group_id);
      fflush(stdout);
      Mailbox<PeerMsg>* peer_mb = nullptr;
      auto peer_fsm = std::make_unique<PeerFsm>(
          mem.group_id, mem.local_node_id,
          node, apply_mb, &tracker_, &router_, this);
      try {
        batch_system_.RegisterPeer(std::move(peer_fsm), &peer_mb);
      } catch (const std::exception& e) {
        fprintf(stderr, "[STORE-N%d] FATAL: RegisterPeer threw for group=%u: %s\n",
                node_id_, mem.group_id, e.what());
        fflush(stderr);
        std::abort();
      } catch (...) {
        fprintf(stderr, "[STORE-N%d] FATAL: RegisterPeer threw unknown for group=%u\n",
                node_id_, mem.group_id);
        fflush(stderr);
        std::abort();
      }

      router_.Register(mem.group_id, mem.local_node_id, peer_mb);
      registry_.RegisterPeerMailbox(mem.group_id, mem.local_node_id, peer_mb);

      printf("[STORE-N%d] Created Raft instance: group=%u local_id=%u\n",
             node_id_, mem.group_id, mem.local_node_id);
      fflush(stdout);
    }

    // // ========== 分发 LRC grouper ==========
    // DistributeLrcGrouperToRaftNodes();
    // // ====================================

    printf("[STORE-N%d] All %zu Raft instances created\n\n", node_id_, memberships.size());
  }

  // ---- Phase 6b: Initialize all Raft instances ----
  void InitRaftInstances() {
    printf("\n[STORE-N%d] Initializing all Raft instances...\n", node_id_);

    // [DEPRECATED] Hook mechanism removed - encoding/packing now done in Raft layer
    // RegisterStripeRaftHooks();

    if (unified_raft_rpc_server_) {
      unified_raft_rpc_server_->GetService()->SetGroupNotificationCallback(
          [this](const raft::GroupNotificationArgs& args) {
            this->HandleGroupNotification(args);
          });
    }

    // 正确的初始化顺序：
    // 1. InitAll() — 初始化所有 KvServiceNode → KvServer::Init() → raft_->Init()
    // 2. PostInit() — 将 RaftState 注册到 UnifiedRpcServer
    
    registry_.InitAll();

    for (const auto& mem : memberships_) {
      kv::KvServiceNode* node = registry_.Get(mem.group_id, mem.local_node_id);
      if (node && node->GetRaftNode()) {
        node->GetRaftNode()->PostInit(
            mem.group_id,
            unified_raft_rpc_server_->GetService(),
            batch_transport_manager_->GetBatchTransport());
        // Register this RaftState as the transport handler for its group
        // so async RPC replies can be routed back.
        if (batch_transport_manager_) {
          auto* raft_state = node->GetRaftNode()->getRaftState();
          batch_transport_manager_->SetTransportHandler(
              mem.group_id, raft_state);
        }
        printf("[STORE-N%d] Init Raft: group=%u node=%u\n",
               node_id_, mem.group_id, mem.local_node_id);
      } else {
        fprintf(stderr, "[STORE-N%d] WARNING: no RaftNode for group=%u node=%u\n",
                node_id_, mem.group_id, mem.local_node_id);
      }
    }
    printf("[STORE-N%d] All Raft instances initialized\n\n", node_id_);
  }

  // ---- Phase 6: Start all Raft instances (unified election start) ----
  void StartRaftInstances() {
    printf("\n[STORE-N%d] Starting all Raft instances...\n", node_id_);
    fflush(stdout);

    if (batch_transport_manager_) batch_transport_manager_->Start();
    if (unified_raft_rpc_server_) {
      try {
        unified_raft_rpc_server_->Start();
      } catch (const std::exception& e) {
        fprintf(stderr, "[STORE-N%d] FATAL: unified_raft_rpc_server_->Start() threw: %s\n",
                node_id_, e.what());
        std::abort();
      } catch (...) {
        fprintf(stderr, "[STORE-N%d] FATAL: unified_raft_rpc_server_->Start() threw unknown exception\n",
                node_id_);
        std::abort();
      }
    }

    // Start all RaftNode instances (ticker thread + applier thread)
    {
      std::shared_lock<std::shared_mutex> lock(raft_nodes_mutex_);
      for (auto& [group_id, raft_node] : raft_nodes_) {
        raft_node->Start();
      }
    }

    registry_.StartAll();
    batch_system_.Start();
    gossip_.Start();
    StartStatusPrintThread();

    printf("[STORE-N%d] All Raft instances started (election will begin)\n\n", node_id_);
    fflush(stdout);
  }

  // ---- Phase 8: Start KV RPC server ----
  void StartKVRpcServer() {
    registry_.StartRpcServer();
    printf("[STORE-N%d] KV RPC server started\n", node_id_);
  }

  // ---- Legacy Init/Start for compatibility ----
  void Init() {
    InitRaftInstances();
  }

  void Start() {
    StartRaftInstances();
  }

  void Stop() {
    printf("\n[STORE-N%d] Stopping all services...\n", node_id_);
    StopStatusPrintThread();
    // Remove transport handlers BEFORE stopping batch_transport_manager_ and
    // destroying raft_nodes_. This prevents dangling pointer access if async
    // RPC replies arrive during shutdown.
    for (const auto& mem : memberships_) {
      if (batch_transport_manager_) {
        batch_transport_manager_->RemoveTransportHandler(mem.group_id);
      }
    }
    gossip_.Stop();
    batch_system_.Stop();
    if (batch_transport_manager_) batch_transport_manager_->Stop();
    // Unregister all RaftStates from the unified RPC service so no more
    // RPC requests arrive during destruction of raft_nodes_.
    if (unified_raft_rpc_server_) {
      for (const auto& mem : memberships_) {
        unified_raft_rpc_server_->UnregisterRaftState(mem.group_id);
      }
    }
    registry_.StopAll();
    if (unified_raft_rpc_server_) unified_raft_rpc_server_->Stop();
    printf("[STORE-N%d] All services stopped\n", node_id_);
  }

  // =====================================================================
  //  RaftStoreInterface 实现
  // =====================================================================
  RoutingTableManagerInterface* GetRoutingTableManager() override { return &routing_table_; }
  CrossGroupTrackerInterface* GetTracker() override { return &tracker_; }

  RoutingTable GetRoutingTable() const override {
    return routing_table_.GetAllRoutes();
  }

  void MergeRoutingTable(const RoutingTable& routes, int ttl) override {
    routing_table_.MergeRoutes(routes, ttl);
  }

  void AddRouteEntry(StripeId stripe_id, EntryId entry_id,
                     const std::vector<GroupId>& data_groups,
                     bool has_global_parities = false,
                     const std::vector<GroupId>& global_parity_groups = {}) override {
    GroupTopology entry;
    entry.stripe_id = stripe_id;
    entry.entry_id = entry_id;
    entry.data_groups = data_groups;
    entry.has_global_parities = has_global_parities;
    entry.global_parity_groups = global_parity_groups;
    entry.local_k = local_k_;
    entry.r = r_;
    routing_table_.AddRoute(entry);
  }

  void UpdateGroupId(GroupId, GroupId, std::function<void(bool)>) override {}

  void SetGroupDynamicK(GroupId group_id, raft::raft_encoding_param_t k) override {
    std::shared_lock<std::shared_mutex> lock(raft_nodes_mutex_);
    auto it = raft_nodes_.find(group_id);
    if (it == raft_nodes_.end()) {
      fprintf(stderr, "[STORE-N%d] SetGroupDynamicK: group %u not found\n", node_id_, group_id);
      return;
    }
    it->second->SetDynamicK(k);
    printf("[STORE-N%d] SetGroupDynamicK: group=%u k=%u\n", node_id_, group_id, k);
  }

  int GetClusterEncodingMode() const override {
    return static_cast<int>(encoding_mode_);
  }

  // Client kPut path: propose stripe write command to group Raft log.
  // Returns (error, commit_elapse_us, apply_elapse_us)
  std::tuple<kv::ErrorType, uint64_t, uint64_t>
  StripePut(GroupId group_id, const std::string& user_key,
            const std::string& user_value);

  // ========================================================================
  // [DISABLED] LocalProposeToGroup — 仅用于测试的简化写入路径
  // 正式写入路径应使用 StripePut 或直接走 EncodeRaftEntry
  // 通过设置 encoding_mode_ = kRsF 可以禁用此路径
  // ========================================================================
  void LocalProposeToGroup(GroupId group_id,
                           const raft::Slice& data,
                           std::function<void(bool, EntryId)> cb) override {
    // 直接调用 StripePut 路径，不在此处进行编码
    // 这样可以复用现有的写入逻辑，避免重复编码
    printf("[STORE-N%d] LocalProposeToGroup called (DISABLED - redirecting to StripePut)\n",
           node_id_);
    fflush(stdout);
    
    // 使用简化的 key/value 格式调用 StripePut
    // 这里创建一个唯一的 key 来标识这次写入
    static std::atomic<uint64_t> seq{0};
    std::string user_key = "__local_propose_" + std::to_string(seq.fetch_add(1));
    StripePut(group_id, user_key, std::string(data.data(), data.size()));
    
    // 简化回调
    cb(true, 0);  // 实际结果由 StripePut 返回
  }

 private:
  // ---------------------------------------------------------------------
  //  EncodeForRsF — RS(F+1, F), total = 2F+1 = N, 1 frag/node.
  //  与既有 RS 行为兼容：用 (k_, r_) 调 EncodeSlice，挑选 frag[group_id]。
  // ---------------------------------------------------------------------
  bool EncodeForRsF(const raft::Slice& data, GroupId group_id, EntryId eid,
                    ShardBundle* bundle) {
    (void)eid;
    if (k_ > 1 || r_ > 0) {
      raft::Encoder::EncodingResults encoded;
      if (encoder_->EncodeSlice(data, k_, r_, &encoded)) {
        auto it = encoded.find(static_cast<raft::raft_frag_id_t>(group_id));
        if (it != encoded.end()) {
          char* copy = new char[it->second.size()];
          std::memcpy(copy, it->second.data(), it->second.size());
          bundle->local_data_shards.emplace_back(copy, it->second.size());
          return true;
        }
      }
    }
    // 落到 fallback: 整条 data 作为一个 shard
    bundle->local_data_shards.push_back(data);
    return true;
  }

  // ---------------------------------------------------------------------
  //  EncodeForRs3F — RS(F+1, 3F+1), total = 2N, 2 random frag/node.
  // ---------------------------------------------------------------------
  bool EncodeForRs3F(const raft::Slice& data, GroupId group_id, EntryId eid,
                     int N, ShardBundle* bundle) {
    if (N < 5) {
      fprintf(stderr, "[STORE-N%d][RS_3F] N=%d < 5; cannot encode\n",
              node_id_, N);
      return false;
    }
    int F = N / 2;
    int k_param = F + 1;
    int m_param = 3 * F + 1;
    int total   = k_param + m_param;  // == 2N

    printf("[STORE-N%d][RS_3F] group=%u eid=%lu RS(%d,%d) total=%d\n",
           node_id_, group_id, eid, k_param, m_param, total);

    raft::Encoder::EncodingResults encoded;
    raft::Encoder enc;
    if (!enc.EncodeSlice(data, k_param, m_param, &encoded)) {
      return false;
    }
    if (static_cast<int>(encoded.size()) != total) {
      fprintf(stderr,
              "[STORE-N%d][RS_3F] unexpected encoded.size()=%zu (want %d)\n",
              node_id_, encoded.size(), total);
      return false;
    }

    // 随机洗牌后两两分给 N 个 node（确定性 seed=group_id+1）
    auto plan = BuildRsRandomPlacement(
        N, total, static_cast<std::uint64_t>(group_id) + 1);

    // Deep-copy 全部 fragments 到 bundle->all_fragments (按 frag_id 顺序)
    bundle->all_fragments.reserve(static_cast<size_t>(total));
    for (int i = 0; i < total; ++i) {
      const raft::Slice& src = encoded.at(i);
      char* copy = new char[src.size()];
      std::memcpy(copy, src.data(), src.size());
      bundle->all_fragments.emplace_back(copy, src.size());
    }
    // 转换 RsRandomPlacement -> FragmentPlacement
    bundle->placement.reserve(static_cast<size_t>(total));
    for (const auto& rp : plan) {
      FragmentPlacement fp;
      fp.frag_id     = rp.frag_id;
      fp.node_id     = rp.node_id;
      fp.local_group = 0;  // RS_3F 没有局部组概念
      fp.kind = (rp.frag_id < k_param) ? FragmentPlacement::kData
                                       : FragmentPlacement::kGlobalParity;
      bundle->placement.push_back(fp);
    }

    // 兼容字段：把前 k 个 data 也放进 local_data_shards
    for (int i = 0; i < k_param; ++i) {
      bundle->local_data_shards.push_back(bundle->all_fragments[i]);
    }

    printf("[STORE-N%d][RS_3F] eid=%lu placement (%d entries, 2/node):\n",
           node_id_, eid, total);
    for (const auto& fp : bundle->placement) {
      printf("[STORE-N%d][RS_3F]   %s\n", node_id_, fp.ToString().c_str());
    }
    return true;
  }

  // ------------------------------------------------------------------------
  //  EncodeForLrc — LRC(F+1, 2, 2N-k-l) using LrcComplementaryGrouper
  //  统一使用 LrcComplementaryGrouper 的 GetNodePlacementsVector() 作为放置算法
  // ------------------------------------------------------------------------
  bool EncodeForLrc(const raft::Slice& data, GroupId group_id, EntryId eid,
                    int N, ShardBundle* bundle) {
    if (N < 5) {
      fprintf(stderr, "[STORE-N%d][LRC] N=%d < 5; cannot encode\n",
              node_id_, N);
      return false;
    }
    try {
      LrcParams lp = LrcParams::FromCapacity(N, /*l=*/2);
      printf("[STORE-N%d][LRC] group=%u eid=%lu %s total=%d\n",
             node_id_, group_id, eid, lp.ToString().c_str(),
             lp.total_shards());

      LrcEncoder enc(lp);
      LrcStripe  stripe;
      if (!enc.EncodeStripe(data, &stripe)) {
        return false;
      }

      // ========== 使用 LrcComplementaryGrouper 的放置算法 ==========
      std::vector<FragmentPlacement> placement;

      if (lrc_grouper_ && latency_aware_enabled_) {
        // 优先使用 grouper 的放置向量（正确的算法）
        placement = lrc_grouper_->GetNodePlacementsVector();
        printf("[STORE-N%d][LRC] eid=%lu Using LrcComplementaryGrouper placement (2-frag/node)\n",
               node_id_, eid);
      } else {
        // 备用：不应该走到这里，latency_aware 应该始终启用
        fprintf(stderr, "[STORE-N%d][LRC] eid=%lu WARNING: lrc_grouper_ not available, "
                        "this should not happen!\n", node_id_, eid);
        return false;
      }

      // Deep-copy: data → all_fragments[0..k), local → [k..k+l), global → [k+l..)
      bundle->all_fragments.reserve(static_cast<size_t>(lp.total_shards()));
      for (int i = 0; i < lp.k; ++i) {
        const raft::Slice& src = stripe.data_shards[i];
        char* copy = new char[src.size()];
        std::memcpy(copy, src.data(), src.size());
        bundle->all_fragments.emplace_back(copy, src.size());
      }
      for (int i = 0; i < lp.l; ++i) {
        const raft::Slice& src = stripe.local_parities[i];
        char* copy = new char[src.size()];
        std::memcpy(copy, src.data(), src.size());
        bundle->all_fragments.emplace_back(copy, src.size());
      }
      for (int i = 0; i < lp.r; ++i) {
        const raft::Slice& src = stripe.global_parities[i];
        char* copy = new char[src.size()];
        std::memcpy(copy, src.data(), src.size());
        bundle->all_fragments.emplace_back(copy, src.size());
      }
      bundle->placement = std::move(placement);

      // 兼容字段：把 data shards 也放进 local_data_shards
      for (int i = 0; i < lp.k; ++i) {
        bundle->local_data_shards.push_back(bundle->all_fragments[i]);
      }

      printf("[STORE-N%d][LRC] eid=%lu encoded total_frags=%d "
             "(k=%d + l=%d + r=%d) frag_size=%zu\n",
             node_id_, eid, lp.total_shards(), lp.k, lp.l, lp.r,
             stripe.frag_size);
      printf("[STORE-N%d][LRC] eid=%lu placement (%zu entries, 2/node):\n",
             node_id_, eid, bundle->placement.size());
      for (const auto& fp : bundle->placement) {
        printf("[STORE-N%d][LRC]   frag_id=%d -> node_id=%d kind=%s\n",
               node_id_, fp.frag_id, fp.node_id,
               fp.kind == FragmentPlacement::Kind::kData ? "Data" :
               fp.kind == FragmentPlacement::Kind::kLocalParity ? "Local" : "Global");
      }

      stripe.FreeMemory();
      return true;
    } catch (const std::exception& e) {
      fprintf(stderr,
              "[STORE-N%d][LRC] encode failed: %s\n",
              node_id_, e.what());
      return false;
    }
  }

 public:

  // =====================================================================
  //  Utility methods
  // =====================================================================
  RoutingTable GetRoutingTableAll() const { return routing_table_.GetAllRoutes(); }
  int GetNodeId() const { return node_id_; }
  int GetRaftInstanceCount() const { return batch_system_.GetPeerFsmCount(); }

  void PrintAllGroupLeaderStatus() const {
    printf("\n[STORE-N%d] GROUP LEADER STATUS\n", node_id_);
    for (int i = 0; i < batch_system_.GetPeerFsmCount(); ++i) {
      auto* peer = batch_system_.GetPeerFsm(i);
      if (peer) peer->PrintStatus();
    }
  }

  RaftRouter* GetRouter() { return &router_; }
  int GetK() const { return k_; }
  int GetR() const { return r_; }

  void SetLrcParams(int k, int l, int r) {
    k_ = k; l_ = l; r_ = r; local_k_ = k_ / l_;
    tracker_.SetGroupCount(l_);
    routing_table_.SetGroupCount(l_);
  }

  raft::rpc::RaftUnifiedRpcServer* GetUnifiedRpcServer() const {
    return unified_raft_rpc_server_.get();
  }

  raft::RaftBatchTransportManager* GetBatchTransportManager() const {
    return batch_transport_manager_.get();
  }

  // 启用/禁用 collect_only 模式：
  // true  = 收到 GroupNotification 时只收集 membership，不创建 raft 实例
  // false = 正常行为，收到 GroupNotification 时直接创建 raft 实例
  void SetCollectOnly(bool enable) { collect_only_ = enable; }
  bool IsCollectOnly() const { return collect_only_; }

  // 主动接收远程 GroupNotification 并收集 membership（collect_only=true 时使用）
  void ReceiveGroupBroadcast(const raft::GroupNotificationArgs& args) {
    HandleGroupNotification(args);
  }

  // 收集 remote membership 信息但不创建 raft 实例（Phase 4.5 使用）
  void CollectMembershipOnly(const raft::GroupNotificationArgs& args) {
    printf("\n[STORE-N%d] CollectMembershipOnly from node %u (%d groups)\n",
           node_id_, args.source_node_id, args.total_groups);

    int groups_added = 0;
    for (int i = 0; i < args.total_groups; i++) {
      const auto& group = args.groups[i];
      bool i_am_member = false;
      for (const auto& member : group.members) {
        if (member.node_id == static_cast<raft::raft_node_id_t>(node_id_)) {
          i_am_member = true;
          break;
        }
      }
      if (!i_am_member) continue;

      kv::KvClusterConfig group_cfg;
      for (const auto& member : group.members) {
        kv::KvServiceNodeConfig nc;
        nc.id = member.node_id;
        nc.group_id = group.group_id;

        std::string ip;
        int port;
        auto addr_str = member.raft_rpc_addr;
        size_t pos = addr_str.rfind(':');
        if (pos != std::string::npos) {
          nc.raft_rpc_addr = raft::rpc::NetAddress{
              addr_str.substr(0, pos),
              static_cast<uint16_t>(std::stoi(addr_str.substr(pos + 1)))};
        }
        addr_str = member.kv_rpc_addr;
        pos = addr_str.rfind(':');
        if (pos != std::string::npos) {
          nc.kv_rpc_addr = kv::rpc::NetAddress{
              addr_str.substr(0, pos),
              static_cast<uint16_t>(std::stoi(addr_str.substr(pos + 1)))};
        }
        nc.raft_log_filename = member.raft_log_filename;
        // 如果是本地节点但 kv_dbname 为空（来自 RPC 时未包含），从本地已知信息重建
        if (member.node_id == static_cast<raft::raft_node_id_t>(node_id_) && member.kv_dbname.empty()) {
          char buf[256];
          snprintf(buf, sizeof(buf), "%s.g%u", local_kv_db_base_.c_str(), group.group_id);
          nc.kv_dbname = buf;
        } else {
          nc.kv_dbname = member.kv_dbname;
        }
        group_cfg[member.node_id] = nc;
      }

      // DEFENSIVE: If local node info is missing from RPC (possible deserialization
      // corruption), fill it in from local known addresses to prevent FATAL crashes.
      auto local_nid = static_cast<raft::raft_node_id_t>(node_id_);
      if (group_cfg.count(local_nid) == 0) {
        fprintf(stderr, "[STORE-N%d] WARNING: local node %u missing from RPC for group %u, "
                "filling from local addresses (possible RPC corruption)\n",
                node_id_, local_nid, group.group_id);
        kv::KvServiceNodeConfig nc;
        nc.id = local_nid;
        nc.group_id = group.group_id;
        nc.raft_rpc_addr = raft_listen_addr_;
        nc.kv_rpc_addr = kv_listen_addr_;
        // Reconstruct log path from other members' patterns
        for (const auto& [nid, cfg] : group_cfg) {
          std::string base_raft_log = cfg.raft_log_filename;
          size_t dot = base_raft_log.rfind('.');
          if (dot != std::string::npos) {
            nc.raft_log_filename = base_raft_log.substr(0, dot) + ".g" + std::to_string(group.group_id);
          } else {
            nc.raft_log_filename = "/tmp/raft_g" + std::to_string(group.group_id) + "_n" + std::to_string(node_id_);
          }
          // 本地节点需要 kv_dbname，从本地已知信息重建
          if (!local_kv_db_base_.empty()) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s.g%u", local_kv_db_base_.c_str(), group.group_id);
            nc.kv_dbname = buf;
          } else {
            nc.kv_dbname = "";
          }
          break;
        }
        group_cfg[local_nid] = nc;
      }

      GroupMembership m;
      m.group_id = group.group_id;
      m.local_node_id = static_cast<raft::raft_node_id_t>(node_id_);
      m.group_cluster_cfg = group_cfg;

      // Deduplicate AND push under a SINGLE lock to prevent data races.
      // The previous code held the lock only for the check, released it,
      // then re-acquired for the push — allowing another thread to modify
      // memberships_ between check and push (heap corruption / double-push).
      bool duplicate = false;
      {
        std::lock_guard<std::mutex> lk(memberships_mutex_);
        for (const auto& existing : memberships_) {
          if (existing.group_id == m.group_id && existing.local_node_id == m.local_node_id) {
            duplicate = true;
            break;
          }
        }
        if (!duplicate) {
          memberships_.push_back(m);
        }
      }
      if (duplicate) {
        printf("[STORE-N%d] CollectMembershipOnly: group %u already collected, skipping\n",
               node_id_, m.group_id);
      } else {
        groups_added++;
      }
    }
    int notify_count;
    {
      std::lock_guard<std::mutex> lk(memberships_mutex_);
      remote_group_notification_count_++;
      notify_count = remote_group_notification_count_;
    }
    printf("[STORE-N%d] CollectMembershipOnly: added %d groups from node %u (total remote notifies: %d)\n",
           node_id_, groups_added, args.source_node_id, notify_count);
  }

  int GetRemoteGroupNotificationCount() const { return remote_group_notification_count_; }
  void ResetRemoteGroupNotificationCount() { remote_group_notification_count_ = 0; }

  // 获取 Phase 4.5 收集到的所有 membership
  std::vector<GroupMembership> GetCollectedMemberships() const {
    std::lock_guard<std::mutex> lk(memberships_mutex_);
    return memberships_;
  }

  // 向 memberships_ 注入一个 membership（用于 Phase 4.5 结束后补充 Phase 4 自创建的 membership）
  void InjectMembership(const GroupMembership& m) {
    // Validate before injection
    if (m.group_cluster_cfg.empty()) {
      fprintf(stderr, "[STORE-N%d] FATAL: InjectMembership: group %u: cluster_cfg is empty\n",
              node_id_, m.group_id);
      std::abort();
    }
    std::string cluster_cfg_keys;
    for (const auto& [k, v] : m.group_cluster_cfg) {
      cluster_cfg_keys += std::to_string(k) + " ";
    }
    if (m.group_cluster_cfg.count(m.local_node_id) == 0) {
      fprintf(stderr, "\n[STORE-N%d] FATAL: InjectMembership: group %u local_id %u: local_node_id NOT in cluster_cfg\n",
              node_id_, m.group_id, m.local_node_id);
      fprintf(stderr, "[STORE-N%d]   cluster_cfg keys: %s\n",
              node_id_, cluster_cfg_keys.c_str());
      fprintf(stderr, "[STORE-N%d]   local_node_id = %u\n",
              node_id_, m.local_node_id);
      fflush(stderr);
      std::abort();
    }

    std::lock_guard<std::mutex> lk(memberships_mutex_);
    // Deduplicate
    for (const auto& existing : memberships_) {
      if (existing.group_id == m.group_id && existing.local_node_id == m.local_node_id) {
        printf("[STORE-N%d] InjectMembership: group %u already exists, skipping\n",
               node_id_, m.group_id);
        return;
      }
    }
    memberships_.push_back(m);
    printf("[STORE-N%d] InjectMembership: added group %u local_id=%u\n",
           node_id_, m.group_id, m.local_node_id);
  }

  void SetPeerAddrs(const std::vector<kv::rpc::NetAddress>& raft_addrs,
                    const std::vector<kv::rpc::NetAddress>& kv_addrs,
                    int cluster_size) {
    peer_raft_addrs_ = raft_addrs;
    peer_kv_addrs_ = kv_addrs;
    cluster_size_ = cluster_size;
  }

  // 设置本地节点的 KV DB 基础路径（如 /tmp/kv_n0），用于在 CollectMembershipOnly 中重建 kv_dbname
  void SetLocalKvDbBase(const std::string& base) {
    local_kv_db_base_ = base;
  }

  void SetPhysicalClusterSize(int N) { physical_cluster_size_ = N; }
  int GetPhysicalClusterSize() const { return physical_cluster_size_; }

  // ------------------------------------------------------------------------
  // InitLatencyMatrix — 初始化延迟矩阵（仅生成随机延迟数据）
  //
  // 输入：num_nodes（物理节点数量）
  // 输出：latency_matrix_（N×N 延迟矩阵）
  //
  // 职责：仅负责生成确定性随机延迟矩阵
  // 注意：此函数不依赖网络，可在任何 Phase 调用
  // ------------------------------------------------------------------------
  void InitLatencyMatrix(int num_nodes) {
    if (num_nodes < 3) {
      printf("[STORE-N%d] InitLatencyMatrix: N=%d too small, skipping\n", node_id_, num_nodes);
      return;
    }

    printf("[STORE-N%d] InitLatencyMatrix: N=%d\n", node_id_, num_nodes);

    // 使用 Knuth 乘法哈希生成固定 seed，确保所有节点生成相同的矩阵
    std::uint64_t seed = static_cast<std::uint64_t>(num_nodes) * 2654435761ULL;

    // 创建延迟矩阵（随机生成，仅模拟真实延迟）
    latency_matrix_ = std::make_unique<LatencyMatrix>(num_nodes);
    latency_matrix_->GenerateRandomMatrix(30, 300, seed);  // 30ms~300ms, 固定 seed
    latency_matrix_->PrintMatrix();

    printf("[STORE-N%d] InitLatencyMatrix: done, N=%d, seed=%lu\n",
           node_id_, num_nodes, seed);
  }

  // ------------------------------------------------------------------------
  // BuildLrcGroups — 构建 LRC 互补分组（依赖延迟矩阵已就绪）
  //
  // 输入：latency_matrix_（已有延迟矩阵）, num_nodes
  // 输出：lrc_grouper_（LRC 分组器）, latency_aware_enabled_
  //
  // 职责：基于延迟矩阵构建 LRC 互补分组
  // 前提：InitLatencyMatrix 必须已调用
  // ------------------------------------------------------------------------
  void BuildLrcGroups(int num_nodes) {
    if (!latency_matrix_) {
      printf("[STORE-N%d] BuildLrcGroups: latency_matrix_ is null, call InitLatencyMatrix first\n",
             node_id_);
      return;
    }

    if (num_nodes < 3) {
      printf("[STORE-N%d] BuildLrcGroups: N=%d too small, skipping\n", node_id_, num_nodes);
      return;
    }

    printf("[STORE-N%d] BuildLrcGroups: N=%d\n", node_id_, num_nodes);

    // 使用与 InitLatencyMatrix 相同的 seed，保证一致性
    std::uint64_t seed = static_cast<std::uint64_t>(num_nodes) * 2654435761ULL;

    // 创建 LRC 分组器并构建分组
    lrc_grouper_ = std::make_unique<LrcComplementaryGrouper>(num_nodes, seed);
    lrc_grouper_->BuildComplementaryGroups(*latency_matrix_);
    lrc_grouper_->PrintGroups();

    latency_aware_enabled_ = true;

    // 更新 LRC 参数
    const auto& lp = lrc_grouper_->GetLrcParams();
    SetLrcParams(lp.k, lp.l, lp.r);

    printf("[STORE-N%d] BuildLrcGroups: done, latency_aware_enabled=%d\n",
           node_id_, latency_aware_enabled_);
  }

  // ------------------------------------------------------------------------
  // InitLatencyAware — 便捷入口：初始化延迟感知（组合 InitLatencyMatrix + BuildLrcGroups）
  //
  // 调用时机：Phase 4 末尾，在 SetPhysicalClusterSize 之后调用
  //
  // 内部依次执行：
  //   1. InitLatencyMatrix(num_nodes) — 初始化延迟矩阵
  //   2. BuildLrcGroups(num_nodes)   — 构建 LRC 分组
  // ------------------------------------------------------------------------
  void InitLatencyAware(int num_nodes) {
    InitLatencyMatrix(num_nodes);
    // LRC requires N >= 5 (FromBandwidth: N must be >= 5)
    // Changed from >= 7 to >= 5 to support smaller clusters
    if (num_nodes >= 5) {
      BuildLrcGroups(num_nodes);
    }
  }

  // 分发 lrc_grouper_ 给所有 RaftNode
  void DistributeLrcGrouperToRaftNodes() {
    if (!lrc_grouper_) {
      printf("[STORE-N%d] DistributeLrcGrouper: lrc_grouper_ is null, skipping\n", node_id_);
      return;
    }

    std::shared_lock<std::shared_mutex> lock(raft_nodes_mutex_);
    int count = 0;
    for (auto& [group_id, raft_node] : raft_nodes_) {
      raft_node->SetLrcGrouper(lrc_grouper_.get());
      count++;
    }
    printf("[STORE-N%d] DistributeLrcGrouper: distributed to %d RaftNodes\n", node_id_, count);
  }

  // ---- Getter methods for latency-aware LRC ----
  bool IsLatencyAwareEnabled() const { return latency_aware_enabled_; }
  LrcComplementaryGrouper* GetLrcGrouper() const { return lrc_grouper_.get(); }
  LatencyMatrix* GetLatencyMatrix() const { return latency_matrix_.get(); }

  //  ---- Encoding mode (multi-raft write path) ------------------------------
  // Selects between RS(F+1, F), RS(F+1, 3F+1), and LRC(F+1, 2, 2N-k-2) at
  // runtime. Must be set BEFORE CreateRaftInstances (so each RaftNode picks
  // up the corresponding commit threshold).
  void SetEncodingMode(EncodingMode m) {
    encoding_mode_ = m;
    registry_.SetEncodingMode(m);
    // [DEPRECATED] Hook mechanism removed - encoding/packing now done in Raft layer
    // RegisterStripeRaftHooks();
    printf("[STORE-N%d] Encoding mode set to %s\n",
           node_id_, EncodingModeName(m));
  }
  EncodingMode GetEncodingMode() const { return encoding_mode_; }

  // 提前初始化 batch_transport_manager_（在 Phase 4.5 之前调用）
  // 这样 RPC 服务器启动后就能正常路由消息
  void InitBatchTransport() {
    if (!batch_transport_manager_) return;
    std::unordered_map<raft::raft_node_id_t, raft::rpc::NetAddress> peer_addrs;
    for (size_t i = 0; i < peer_raft_addrs_.size(); ++i) {
      raft::rpc::NetAddress addr;
      addr.ip = peer_raft_addrs_[i].ip;
      addr.port = peer_raft_addrs_[i].port;
      peer_addrs[static_cast<raft::raft_node_id_t>(i)] = addr;
    }
    if (!peer_addrs.empty()) {
      batch_transport_manager_->Init(
          static_cast<raft::raft_node_id_t>(node_id_), peer_addrs);
      printf("[STORE-N%d] BatchTransport pre-initialized for %zu peers\n",
             node_id_, peer_addrs.size());
    }
  }

 private:
  // =====================================================================
  //  HandleGroupNotification — 处理来自其他节点的分组通知
  //  (用于 Phase 5: 分组结果通过 RaftRPC 同步)
  // =====================================================================
  void HandleGroupNotification(const raft::GroupNotificationArgs& args) {
    printf("\n[STORE-N%d] HandleGroupNotification from node %u (%d groups)\n",
           node_id_, args.source_node_id, args.total_groups);

    int groups_added = 0;
    for (int i = 0; i < args.total_groups; i++) {
      const auto& group = args.groups[i];
      bool i_am_member = false;
      for (const auto& member : group.members) {
        if (member.node_id == static_cast<raft::raft_node_id_t>(node_id_)) {
          i_am_member = true;
          break;
        }
      }
      if (!i_am_member) continue;

      kv::KvClusterConfig group_cfg;
      for (const auto& member : group.members) {
        std::string ip;
        int port;

        kv::KvServiceNodeConfig nc;
        nc.id = member.node_id;
        nc.group_id = group.group_id;

        auto addr_str = member.raft_rpc_addr;
        size_t pos = addr_str.rfind(':');
        if (pos != std::string::npos) {
          nc.raft_rpc_addr = raft::rpc::NetAddress{
              addr_str.substr(0, pos),
              static_cast<uint16_t>(std::stoi(addr_str.substr(pos + 1)))};
        }

        addr_str = member.kv_rpc_addr;
        pos = addr_str.rfind(':');
        if (pos != std::string::npos) {
          nc.kv_rpc_addr = kv::rpc::NetAddress{
              addr_str.substr(0, pos),
              static_cast<uint16_t>(std::stoi(addr_str.substr(pos + 1)))};
        }

        nc.raft_log_filename = member.raft_log_filename;
        // 只有本地节点需要 kv_dbname，非本地节点不需要
        // 如果是本地节点但 kv_dbname 为空（来自 RPC 时未包含），从本地已知信息重建
        if (member.node_id == static_cast<raft::raft_node_id_t>(node_id_)) {
          if (!member.kv_dbname.empty()) {
            nc.kv_dbname = member.kv_dbname + ".g" + std::to_string(group.group_id);
          } else if (!local_kv_db_base_.empty()) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s.g%u", local_kv_db_base_.c_str(), group.group_id);
            nc.kv_dbname = buf;
          } else {
            nc.kv_dbname = "";
          }
        } else {
          nc.kv_dbname = "";
        }
        group_cfg[member.node_id] = nc;
      }

      GroupMembership m;
      m.group_id = group.group_id;
      m.local_node_id = static_cast<raft::raft_node_id_t>(node_id_);
      m.group_cluster_cfg = group_cfg;

      if (collect_only_) {
        // Use the same locked pattern as CollectMembershipOnly
        bool duplicate = false;
        {
          std::lock_guard<std::mutex> lk(memberships_mutex_);
          for (const auto& existing : memberships_) {
            if (existing.group_id == m.group_id && existing.local_node_id == m.local_node_id) {
              duplicate = true;
              break;
            }
          }
          if (!duplicate) {
            memberships_.push_back(m);
          }
        }
        if (!duplicate) {
          groups_added++;
          printf("[STORE-N%d] CollectOnly: group %u added\n",
                 node_id_, group.group_id);
        }
        continue;
      }

      kv::KvServiceNode* node = registry_.Register(
          m.group_id, m.local_node_id, m.group_cluster_cfg);

      // [FIX] 创建 RaftNode 并注入，与 CreateRaftInstances() 保持一致
      raft::RaftNode::NodeConfig node_config;
      node_config.node_id_me = m.local_node_id;
      for (const auto& [peer_id, addr_info] : m.group_cluster_cfg) {
        node_config.servers[peer_id] = addr_info.raft_rpc_addr;
      }
      node_config.N_physical_nodes = physical_cluster_size_;
      node_config.encoding_mode = static_cast<int>(encoding_mode_);

      auto raft_node = std::make_unique<raft::RaftNode>(node_config);
      {
        // Write lock for raft_nodes_ map modification
        std::unique_lock<std::shared_mutex> lock(raft_nodes_mutex_);
        raft_nodes_[m.group_id] = std::move(raft_node);
      }
      raft_nodes_[m.group_id]->SetUnifiedRpcService(unified_raft_rpc_server_->GetService());
      // [FIX] 设置 ApplyFsm mailbox 作为 RSM bridge
      raft_nodes_[m.group_id]->SetRsmAndApplyMailbox(
          m.group_id, m.local_node_id,
          &batch_system_, this);
      node->SetRaftNode(raft_nodes_[m.group_id].get(), false);
      printf("[STORE-N%d] HandleGroupNotification: created RaftNode for group=%u\n",
             node_id_, m.group_id);

      Mailbox<ApplyMsg>* apply_mb = nullptr;
      int cluster_n = physical_cluster_size_ > 0 ? physical_cluster_size_
                                                 : static_cast<int>(m.group_cluster_cfg.size());
      auto apply_fsm = std::make_unique<ApplyFsm>(node, m.group_id, m.local_node_id,
                                                  encoding_mode_, cluster_n);
      batch_system_.RegisterApply(std::move(apply_fsm), &apply_mb);
      // 注册 ApplyFsm mailbox 到 bridge
      raft_nodes_[m.group_id]->RegisterApplyMailbox(apply_mb);

      Mailbox<PeerMsg>* peer_mb = nullptr;
      auto peer_fsm = std::make_unique<PeerFsm>(
          m.group_id, m.local_node_id,
          node, apply_mb, &tracker_, &router_, this);
      batch_system_.RegisterPeer(std::move(peer_fsm), &peer_mb);

      router_.Register(m.group_id, m.local_node_id, peer_mb);
      registry_.RegisterPeerMailbox(m.group_id, m.local_node_id, peer_mb);

      memberships_.push_back(m);
      groups_added++;
    }
    printf("[STORE-N%d] HandleGroupNotification: added %d groups\n", node_id_, groups_added);
  }

 private:
  int                  node_id_;
  int                  k_ = 1;
  int                  r_ = 0;
  int                  l_ = 1;
  int                  local_k_ = 1;
  mutable std::mutex memberships_mutex_;
  std::vector<GroupMembership> memberships_;
  CrossGroupTracker    tracker_;
  RoutingTableManager  routing_table_;
  GroupRegistry        registry_;
  RaftRouter           router_;
  BatchSystem          batch_system_;
  GossipThread         gossip_;
  std::unique_ptr<raft::Encoder> encoder_;

  std::unique_ptr<raft::rpc::RaftUnifiedRpcServer> unified_raft_rpc_server_;
  std::unique_ptr<raft::RaftBatchTransportManager> batch_transport_manager_;

  // 每个 group 一个 RaftNode，由 RaftStore 统一管理生命周期
  // 避免 Multi-Raft 场景下被 KvServiceNode 析构时 double-free
  std::unordered_map<GroupId, std::unique_ptr<raft::RaftNode>> raft_nodes_;
  // Thread-safety mutex for raft_nodes_ access
  mutable std::shared_mutex raft_nodes_mutex_;

  std::vector<kv::rpc::NetAddress> peer_raft_addrs_;
  std::vector<kv::rpc::NetAddress> peer_kv_addrs_;
  int cluster_size_ = 0;
  kv::rpc::NetAddress kv_listen_addr_;
  raft::rpc::NetAddress raft_listen_addr_;
  bool collect_only_ = false;
  int remote_group_notification_count_ = 0;
  std::string local_kv_db_base_;  // 本地节点的 KV DB 基础路径（如 /tmp/kv_n0）
  int physical_cluster_size_ = 0;
  // Multi-raft encoding mode. Defaults to legacy RS_F behaviour for
  // backwards compatibility; bench_server_multiraft sets it via --encoding.
  EncodingMode encoding_mode_ = EncodingMode::kRsF;
  // ------------------------------------------------------------------------
  // 延迟感知 LRC 互补分组相关
  // ------------------------------------------------------------------------
  std::unique_ptr<LatencyMatrix> latency_matrix_;              // 延迟矩阵
  std::unique_ptr<LrcComplementaryGrouper> lrc_grouper_;       // LRC 分组器
  bool latency_aware_enabled_ = false;                          // 是否启用延迟感知

  // ------------------------------------------------------------------------
  // Group Leader 状态定时打印线程
  // ------------------------------------------------------------------------
  std::thread status_print_thread_;
  std::atomic<bool> status_print_running_{false};

  // 启动状态打印线程（每5秒打印一次所有 group leader 的成员状态）
  void StartStatusPrintThread() {
    status_print_running_.store(true);
    status_print_thread_ = std::thread([this] {
      while (status_print_running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (status_print_running_.load()) {
          PrintAllGroupMemberStatus();
        }
      }
    });
    printf("[STORE-N%d] Status print thread started (5s interval)\n", node_id_);
  }

  // 停止状态打印线程
  void StopStatusPrintThread() {
    status_print_running_.store(false);
    if (status_print_thread_.joinable()) {
      status_print_thread_.join();
    }
    printf("[STORE-N%d] Status print thread stopped\n", node_id_);
  }

  // 打印所有 group 的 leader 成员状态（每5秒由定时线程调用）
  void PrintAllGroupMemberStatus() {
    time_t now = std::time(nullptr);
    printf("\n[TIMESTAMP: %ld] [STORE-N%d] GROUP LEADER MEMBER STATUS\n", now, node_id_);
    for (int i = 0; i < batch_system_.GetPeerFsmCount(); ++i) {
      auto* peer = batch_system_.GetPeerFsm(i);
      if (peer) {
        peer->PrintGroupMemberStatus();
      }
    }
    fflush(stdout);
  }
};

}  // namespace multiraft
