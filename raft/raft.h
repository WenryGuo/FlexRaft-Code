#pragma once
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <unordered_map>

#include "batch_system_bridge.h"
#include "batch_transport.h"
#include "encoder.h"
#include "log_entry.h"
#include "log_manager.h"
#include "persist_queue.h"
#include "raft_struct.h"
#include "raft_type.h"
#include "rpc.h"
#include "rsm.h"
#include "util.h"

// Forward declarations for multi-raft types (to avoid circular dependencies)
namespace multiraft {
struct LrcParams;
class LrcComplementaryGrouper;
}  // namespace multiraft

namespace raft {

class Storage;

enum RaftRole {
  kFollower = 1,
  kCandidate = 2,
  kPreLeader = 3,
  kLeader = 4,
};

namespace config {
const int64_t kHeartbeatInterval = 200;         // 200ms
const int64_t kCollectFragmentsInterval = 100;  // 100ms
const int64_t kReplicateInterval = 1000;
const int64_t kElectionTimeoutMin = 1000;  // 1000ms
constexpr int kLivenessTimeoutInterval = 200;
const int64_t kElectionTimeoutMax = 1500;  // 1500ms
};                                         // namespace config

// Forward-declare RaftTransportHandler (defined in batch_transport.h).
// RaftState inherits from it, so it needs to know the base class exists.
class RaftTransportHandler;

struct RaftConfig {
  // The node id of curernt peer. A node id is the unique identifier to
  // distinguish different raft peers
  raft_node_id_t id = 0;

  // The raft group id for this RaftState instance (used in multi-raft)
  raft_group_id_t group_id = 0;

  // The raft node id and corresponding network address of all raft peers
  // in current cluster. (including current server itself)
  std::unordered_map<raft_node_id_t, rpc::RpcClient *> rpc_clients;

  // Persistence storage, which is used to recover from failure, could be
  // nullptr. If storage is nullptr, any change to RaftState will not be
  // persisted
  Storage *storage;

  int64_t electionTimeMin, electionTimeMax;

  Rsm *rsm;

  // The number of physical nodes in the cluster (for leader balancing)
  int N_physical_nodes = 0;

  // For Multi-Raft: BatchSystemBridge pointer for committed entry application
  void* batch_system_bridge = nullptr;
  uint32_t bridge_group_id = 0;
  uint32_t bridge_node_id = 0;

  // Dynamic encoding parameter: fixed k for comparison experiments.
  // 0 = auto (compute k = live_servers - F based on live server count)
  // non-zero = use this fixed k for all encoding (m = N - k)
  raft_encoding_param_t dynamic_k = 0;

  int encoding_mode = 2; // 0=RS_F, 1=RS_3F, 2=LRC

  // For Multi-Raft: BatchTransport pointer for batched outbound RPC.
  // If set, RaftState sends via BatchTransport instead of direct rpc_clients_.
  // Uses void* to avoid circular header dependency.
  void* batch_transport = nullptr;

  // Callback interface for routing RPC replies back to this RaftState.
  // Set by RaftNode when constructing RaftConfig.
  RaftTransportHandler* transport_handler = nullptr;
};

struct ProposeResult {
  raft_index_t propose_index;
  raft_term_t propose_term;
  bool is_leader;
};

// A monitor that records the number of server that is still alive in current
// cluster
struct LivenessMonitor {
  static constexpr int kMaxNodeNum = 10;
  int node_num;
  bool response[kMaxNodeNum];
  uint64_t response_time[kMaxNodeNum];
  raft_node_id_t me;  // current server's id
  util::Timer timer;

  // void Init() { std::memset(response, true, sizeof(response)); }

  void Init() {
    timer.Reset();
    assert(me < kMaxNodeNum && "node id exceeds LivenessMonitor array bounds");
    response[me] = true;
    response_time[me] = 0;
  }

  void SetLivenessNumber(int num) {
    for (int i = 0; i < std::min(num, std::min(node_num, kMaxNodeNum)); ++i) {
      response[i] = true;
    }
  }

  void UpdateLiveness(raft_node_id_t id) {
    // Multi-Raft note: id is a global node ID, but node_num reflects local group size.
    // If id is outside our local group's membership, skip the update.
    if (id >= kMaxNodeNum || id >= static_cast<raft_node_id_t>(node_num)) {
      return;
    }
    response[id] = true;
    response_time[id] = timer.ElapseMilliseconds();

    // Update other server's state
    auto elapsed = response_time[id];
    for (int i = 0; i < node_num; ++i) {
      if (response[i] && (elapsed - response_time[i]) < config::kLivenessTimeoutInterval) {
        response[i] = true;
      } else {
        response[i] = false;
      }
    }
    response[me] = true;
  }

  int LiveNumber() const {
    int cnt = 0;
    for (int i = 0; i < node_num; ++i) {
      cnt += (response[i]);
    }
    return cnt;
  }

  // void UpdateLivenessState() {
  //   auto elapsed = timer.ElapseMilliseconds();
  //   for (int i = 0; i < node_num; ++i) {
  //     if (response[i] && (elapsed - response_time[i]) < 100) {
  //       response[i] = true;
  //     } else {
  //       response[i] = false;
  //     }
  //   }
  //   response[me] = true;
  // }

  bool IsAlive(raft_node_id_t target_id) const { return response[target_id]; }
};

struct SequenceGenerator {
 public:
  void Reset() { seq = 1; }
  uint64_t Next() { return seq++; }

 private:
  uint64_t seq;
};

struct PreLeaderStripeStore {
  PreLeaderStripeStore() = default;

  // [start_index, end_index] is the range of index that preLeader collects
  raft_index_t start_index, end_index;
  std::vector<Stripe> stripes;
  static constexpr int kResponseCapacity = 32;
  bool response_[kResponseCapacity];
  int node_num;
  raft_node_id_t me;

  void InitRequestFragmentsTask(raft_index_t start, raft_index_t end, int node_num,
                                raft_node_id_t me) {
    if (node_num > kResponseCapacity) {
      fprintf(stderr, "[PreLeaderStripeStore] ERROR: node_num=%d exceeds capacity %d\n",
              node_num, kResponseCapacity);
      return;
    }
    this->start_index = start;
    this->end_index = end;
    this->node_num = node_num;
    this->me = me;
    stripes.clear();
    stripes.reserve(end - start + 1);

    int stripe_cnt = end - start + 1;
    for (int i = 0; i < stripe_cnt; ++i) {
      stripes.push_back(Stripe());
    }

    // Initiate stripe
    for (auto stripe : stripes) {
      // stripe.Init();
    }

    memset(response_, false, sizeof(response_));
    if (me < kResponseCapacity) {
      response_[me] = true;
    }
  }

  void UpdateResponseState(raft_node_id_t id) {
    if (id >= 0 && id < node_num && id < kResponseCapacity) {
      response_[id] = true;
    }
  }

  bool IsCollected(raft_node_id_t id) const {
    if (id >= 0 && id < node_num && id < kResponseCapacity) {
      return response_[id];
    }
    return false;
  }

  int CollectedFragmentsCnt() const {
    int ret = 0;
    for (int i = 0; i < node_num && i < kResponseCapacity; ++i) {
      ret += response_[i];
    }
    return ret;
  }

  void AddFragments(raft_index_t idx, const LogEntry &entry, raft_frag_id_t chunk_id) {
    if (idx < start_index || idx > end_index) {
      // NOTE: idx > end_index indicates that current leader receives an entry
      // with index higher than leader's last index, in that way, it simply cut
      // off these entries because the majority of the servers doesn't have this
      // entry(otherwise the leader won't win this election)
      return;
    }
    auto array_index = idx - start_index;
    stripes[array_index].collected_fragments.insert_or_assign(chunk_id, entry);
  }
};

// A raft peer maintains the necessary information in terms of "Logic" state
// of raft algorithm
class RaftPeer {
 public:
  RaftPeer() : next_index_(0), match_index_(0) {}

  raft_index_t NextIndex() const { return next_index_; }
  void SetNextIndex(raft_index_t next_index) { next_index_ = next_index; }

  raft_index_t MatchIndex() const { return match_index_; }
  void SetMatchIndex(raft_index_t match_index) { match_index_ = match_index; }

 public:
  raft_index_t next_index_, match_index_;
  std::unordered_map<raft_index_t, ChunkInfo> matchChunkInfo;
};

class RaftState : public RaftTransportHandler {
 public:
  // Construct a RaftState instance from a specified configuration.
  static RaftState *NewRaftState(const RaftConfig &);
  static const raft_node_id_t kNotVoted = -1;

 public:
  RaftState() = default;

  RaftState(const RaftState &) = delete;
  RaftState &operator=(const RaftState &) = delete;

  // Recovery related struct
  struct RecoveryCtx {
    util::TimePoint start_time_;
    raft_index_t start_recovery_index_;
  };

  struct RecoveryRecord {
    raft_node_id_t recover_node;
    int entry_cnt;  // Number of entries to be recovered
    uint32_t dura;
  };

 public:
  // Process a bunch of RPC request or response, the first parameter is the
  // input of this process, the second parameter is the output.
  void Process(RequestVoteArgs *args, RequestVoteReply *reply);
  void Process(RequestVoteReply *reply);

  void Process(AppendEntriesArgs *args, AppendEntriesReply *reply);
  void Process(AppendEntriesReply *reply);

  void Process(RequestFragmentsArgs *args, RequestFragmentsReply *reply);
  void Process(RequestFragmentsReply *reply);

  // RaftTransportHandler interface (inherited from RaftTransportHandler base)
  void HandleRequestVoteReply(const RequestVoteReply& reply) override;
  void HandleAppendEntriesReply(const AppendEntriesReply& reply) override;
  void HandleRequestFragmentsReply(const RequestFragmentsReply& reply) override;

  // This is a command from upper level application, the raft instance is
  // supposed to copy this entry to its own log and replicate it to other
  // followers
  ProposeResult Propose(const CommandData &command);

  raft::raft_index_t LastIndex() {
    std::scoped_lock<std::mutex> mtx(this->mtx_);
    return lm_->LastLogEntryIndex();
  }

 public:
  // Init all necessary status of raft state, including reset election timer
  void Init();

  // The driver clock periodically call the tick function to so that raft peer
  // make progress
  void Tick();

  raft_term_t CurrentTerm() const { return current_term_; }
  void SetCurrentTerm(raft_term_t term) { current_term_ = term; }

  raft_node_id_t VoteFor() const { return vote_for_; }
  void SetVoteFor(raft_node_id_t node) { vote_for_ = node; }

  raft_group_id_t GroupId() const { return group_id_; }
  void SetGroupId(raft_group_id_t gid) { group_id_ = gid; }

  void SetPhysicalClusterSize(int N) { N_physical_nodes_ = N; }

  // Set batch transport and transport handler after construction.
  // Called by RaftNode::PostInit() to inject the transport layer.
  void SetBatchTransport(void* transport) { batch_transport_ = transport; }
  void SetTransportHandler(RaftTransportHandler* handler) { transport_handler_ = handler; }

  RaftRole Role() const { return role_; }
  void SetRole(RaftRole role) { role_ = role; }

  // ALERT: This public interface should only be used in test case
  void SetVoteCnt(int cnt) { vote_me_cnt_ = cnt; }

  raft_index_t CommitIndex() const { return commit_index_; }
  void SetCommitIndex(raft_index_t raft_index) { commit_index_ = raft_index; }

  raft_index_t LastLogIndex() const { return lm_->LastLogEntryIndex(); }
  raft_term_t TermAt(raft_index_t raft_index) const { return lm_->TermAt(raft_index); }

  int GetClusterServerNumber() const { return peers_.size() + 1; }
  raft_node_id_t NodeId() const { return id_; }
  int GetPhysicalClusterSize() const { return N_physical_nodes_; }

  // Check if a given node is a member of this Group.
  // peers_ contains all peer node IDs in this Group (populated from config.rpc_clients
  // which is built from the per-group cluster config during RaftNode construction).
  // Note: includes self (id_) as a member.
  bool IsGroupMember(raft_node_id_t node_id) const {
    return node_id == id_ || peers_.count(node_id) > 0;
  }

  uint64_t CommitLatency(raft_index_t raft_index) const {
    if (commit_elapse_time_.count(raft_index) == 0) {
      return -1;
    } else {
      return commit_elapse_time_.at(raft_index);
    }
  }

 public:
  // Check specified raft_index and raft_term is newer than log entries stored
  // in current raft peer. Return true if it is, otherwise returns false
  bool isLogUpToDate(raft_index_t raft_index, raft_term_t raft_term);

  // Check if current raft peer has exactly an entry of specified raft_term at
  // specific raft_index
  bool containEntry(raft_index_t raft_index, raft_term_t raft_term, raft_encoding_param_t prev_k);

  // When receiving AppendEntries Reply, the raft peer checks all peers match
  // index condition and may update the commit_index field
  void tryUpdateCommitIndex();

  void tryApplyLogEntries();

  // Encoding specified log entry with encoding parameter k, m, the results is
  // written into specified stripe
  void EncodeRaftEntry(raft_index_t raft_index, raft_encoding_param_t k, raft_encoding_param_t m,
                       Stripe *stripe);

  // Decoding all fragments contained in a stripe into a complete log entry
  bool DecodingRaftEntry(Stripe *stripe, LogEntry *ent);

  bool NeedOverwriteLogEntry(const ChunkInfo &old_info, const ChunkInfo &new_info);

  void FilterDuplicatedCollectedFragments(Stripe &stripes);

  bool FindFullEntryInStripe(const Stripe *stripe, LogEntry *ent);

  // Iterate through the entries carried by input args and check if there is
  // conflicting entry: Same index but different term. If there is one, delete
  // all following entries. Add any new entries that are not in raft's log
  void checkConflictEntryAndAppendNew(AppendEntriesArgs *args, AppendEntriesReply *reply);

  // Reset the next index and match index fields when current server becomes
  // leader
  void resetNextIndexAndMatchIndex();

  uint32_t NextSequence() { return seq_gen_.Next(); }

  void tickOnFollower();
  void tickOnCandidate();
  void tickOnLeader();
  void tickOnPreLeader();

  void resetElectionTimer();
  void resetHeartbeatTimer();
  void resetPreLeaderTimer();
  void resetReplicateTimer();

  void convertToFollower(raft_term_t term);
  void convertToCandidate();
  void convertToLeader();
  void convertToPreLeader();

 private:
  // Internal version of convertToLeader that assumes caller holds mtx_
  void convertToLeaderInternal();

  void PersistRaftState();

  // A private function that is used to start a new election
  void startElection();

  // Replicate entries to all other raft peers
  void broadcastHeartbeat();

  // Collect all needed fragments
  void collectFragments();

  void incrementVoteMeCnt() { vote_me_cnt_++; }

  // For a cluster consists of 2F+1 server, F is called the liveness
  // level, which is the maximum number of failure servers the cluster
  // can tolerant
  int livenessLevel() const { return peers_.size() / 2; }

  // Send heartbeat messages to target raft peer
  void sendHeartBeat(raft_node_id_t peer);

  // Send appendEntries messages to target raft peer
  void sendAppendEntries(raft_node_id_t peer);

  // Multi-Raft: batch transport helpers — route all outbound RPCs through BatchTransport
  // to avoid concurrent-call errors when multiple Raft groups share the same RcfClient.
  void sendRequestVoteViaTransport(raft_node_id_t to, const RequestVoteArgs& args);
  void sendAppendEntriesViaTransport(raft_node_id_t to, const AppendEntriesArgs& args);
  void sendRequestFragmentsViaTransport(raft_node_id_t to, const RequestFragmentsArgs& args);

  void initLivenessMonitorState() { live_monitor_.Init(); }

  // In flexibleK, the leader needs to send AppendEntries arguments in every
  // heartbeat round
  // void replicateEntries();

  // The preleader will try becoming leader if all requested fragments are
  // decoded into complete log entries
  void PreLeaderBecomeLeader();

  void DecodeCollectedStripe();

  // Replicate a new proposed entry indexed by specified raft_index to alive
  // servers [Require]: Given entry has already been added into log
  void ReplicateNewProposeEntry(raft_index_t raft_index);

  // This process checks if re-encoding is needed for each uncommitted entry. If
  // it is, re-encoding and replicate entries to all followers; otherwise,
  // simply replicate entries according to the NextIndex of each followers
  void ReplicateEntries();

  // Some re-encoding work might by needed due to number of alive servers has
  // been changed.
  void MaybeReEncodingAndReplicate();

  void UpdateLastEncodingK(raft_index_t raft_index, raft_encoding_param_t k) {
    last_encoding_.insert_or_assign(raft_index, k);
  }

  auto GetLastEncodingK(raft_index_t raft_index) -> raft_encoding_param_t {
    // Returns 0 means this entry has not been encoded yet
    // There are two cases for a given raft index and its associated k
    // 1. The entry is complete, we shall see its current encoding k
    // 2. The entry is a chunk, we shall directly returns its k in chunk info
    auto ent = lm_->GetSingleLogEntry(raft_index);
    if (ent == nullptr) {
      return 0;
    }

    switch (ent->Type()) {
      case kNormal: {
        if (last_encoding_.count(raft_index) == 0) {
          return 0;
        }
        return last_encoding_[raft_index];
      }
      case kFragments: {
        return ent->GetChunkInfo().GetK();
      }
      default:
        return 0;
    }
  }

  int AliveServersOfLastPoint() const { return alive_servers_of_last_point_; }
  void UpdateAliveServers(int num) { alive_servers_of_last_point_ = num; }

 public:
  // For concurrency control. A raft state instance might be accessed via
  // multiple threads, e.g. RPC thread that receives request; The state machine
  // thread that peridically apply committed log entries, and so on
  std::mutex mtx_;

  // The id of current raft peer
  raft_node_id_t id_;

  // The group id of this RaftState (used in multi-raft for logging)
  raft_group_id_t group_id_;

  // Record current raft peer's state is Follower, or Candidate, or Leader
  RaftRole role_;

  // Current Term of raft peer, initiated to be 0 when first bootsup
  raft_term_t current_term_;

  // The peer that this peer has voted in current term, initiated to be -1
  // when first bootsup
  raft_node_id_t vote_for_;

  // The raft index of log entry that has been committed and applied to state
  // machine, does not need persistence
  raft_index_t commit_index_;
  raft_index_t last_applied_;

  // Manage all log entries
  LogManager *lm_;
  Storage *storage_;

  // For FlexibleK and CRaft: We need to detect the number of live servers
  LivenessMonitor live_monitor_;
  Encoder encoder_;
  SequenceGenerator seq_gen_;
  // For each index, there is an associated stripe that contains the encoded
  // data
  std::map<raft_index_t, Stripe *> encoded_stripe_;

  // For each index, last_encoding contains the most recent encoding parameters
  // k since it determines if there is a newer version of encoding
  std::unordered_map<raft_index_t, raft_encoding_param_t> last_encoding_;

  // A place for storing fragments come from RequestFragments
  PreLeaderStripeStore preleader_stripe_store_;

  auto GetRecoveryCtx(raft_node_id_t id) -> RecoveryCtx * {
    if (recovery_ctx_.count(id) == 0) {
      return nullptr;
    }
    return recovery_ctx_[id];
  }

  void AddNewRecoveryCtx(raft_node_id_t id) { recovery_ctx_[id] = new RecoveryCtx(); }

  void ClearRecoveryCtx(raft_node_id_t id) { recovery_ctx_.erase(id); }

  void EmitRecoveryRecord(raft_node_id_t node, int ent_cnt, uint32_t dura) {
    recover_records_.push_back(RecoveryRecord{node, ent_cnt, dura});
  }

 public:
  std::set<raft_node_id_t> peers_;
  RaftPeer *raft_peer_[32] = {nullptr};
  rpc::RpcClient *rpc_clients_[32] = {nullptr};
  // std::unordered_map<raft_node_id_t, RaftPeer *> peers_;
  // std::unordered_map<raft_node_id_t, rpc::RpcClient *> rpc_clients_;

  util::Timer election_timer_;   // Record elapse time during election
  util::Timer heartbeat_timer_;  // Record elapse time since last heartbeat
  util::Timer preleader_timer_;  // Record fragments collection time
  util::Timer replicate_timer_;  // Record replication timer

  // Election time should be between [min, max), set by configuration
  int64_t electionTimeLimitMin_, electionTimeLimitMax_;
  // A randomized election timeout based on above interval
  int64_t election_time_out_;
  int64_t heartbeatTimeInterval;

  // Per-instance high-quality random number generator for election timeout
  std::mt19937 rng_;

  // Stagger offset for election timer start (Option A fix):
  // Each group's preferred node starts its election timer at a different time
  // to prevent cross-group election interference. In milliseconds.
  int64_t election_timer_start_offset_ms_ = 0;

  // Heartbeat timeout for leader failure fallback.
  // Non-preferred followers wait this long before opening elections to non-preferred nodes.
  int64_t heartbeat_timeout_ms_ = 10000;  // 10 seconds default

  // For calculating the commit latency
  std::unordered_map<raft_index_t, util::TimePoint> commit_start_time_;

  // Elapse time of microseconds
  std::unordered_map<raft_index_t, uint64_t> commit_elapse_time_;

  // Context of recovery operations
  std::unordered_map<raft_node_id_t, RecoveryCtx *> recovery_ctx_;
  std::vector<RecoveryRecord> recover_records_;

  int alive_servers_of_last_point_;

 public:
  // Returns 0-based priority rank within the group. 0 = lowest priority,
  // peers_.size() = highest priority.
  int GetPriorityRank() const;

  // Returns true if this node is eligible to become a candidate in the current round.
  bool IsEligibleForElection() const;

  // ========================================================================
  // Dynamic Encoding Parameter API (for comparison experiments)
  // ========================================================================
  // Set fixed k for encoding. 0 = auto mode (k = live_servers - F).
  // non-zero = use this fixed k for all entries (m = N - k).
  void SetDynamicK(raft_encoding_param_t k) {
    dynamic_k_ = k;
    LOG(util::kRaft, "S%d SetDynamicK=%d [G%d]", id_, k, group_id_);
  }
  auto GetDynamicK() const -> raft_encoding_param_t { return dynamic_k_; }
  bool HasFixedK() const { return dynamic_k_ != 0; }

  int GetEncodingMode() const { return encoding_mode_; }
  void SetEncodingMode(int mode) { encoding_mode_ = mode; }

  // Some report information about preleader phase
  util::TimePoint preleader_timepoint_;
  uint64_t preleader_recover_ent_cnt_ = 0;

  // Set the RSM (Replicated State Machine) for applying committed log entries.
  // Used in Multi-Raft mode to connect the KvServer's channel_ to the RaftNode
  // after construction, so that committed entries flow through BOTH the
  // BatchSystemBridge (ApplyFsm) and the RSM channel (ApplyRequestCommandThread).
  void SetRsm(Rsm* rsm) { rsm_ = rsm; }

  // ------------------------------------------------------------------------
  // LRC Latency-Aware Complementary Grouping
  // ------------------------------------------------------------------------
  // Set the LRC complementary grouper for latency-aware orthogonal placement
  void SetLrcGrouper(void* grouper) { lrc_grouper_ = grouper; }
  // Check if LRC grouper is available
  bool HasLrcGrouper() const { return lrc_grouper_ != nullptr; }

 private:
  int vote_me_cnt_;
  Rsm *rsm_;

  // Priority-based election: progressive round counter.
  // Each round, more nodes are allowed to become candidates (top ceil(N/2) by node ID).
  // Resets to 1 when a leader is elected or when starting a new election.
  int election_round_ = 1;

  // The number of physical nodes for leader balancing across groups.
  int N_physical_nodes_ = 0;

  // Dynamic encoding parameter: 0 = auto (k = live - F), non-zero = fixed k.
  // Used for comparison experiments (e.g., fix k=3, k=5, k=7).
  raft_encoding_param_t dynamic_k_ = 0;

  // Encoding mode for Multi-Raft stripe EC: 0=RS_F, 1=RS_3F, 2=LRC.
  int encoding_mode_ = 2;

  // Multi-Raft: BatchSystemBridge for pushing committed entries to ApplyFsm mailbox
  // (void* to avoid circular header dependency)
  void* batch_system_bridge_ = nullptr;
  uint32_t bridge_group_id_ = 0;
  uint32_t bridge_node_id_ = 0;

  // Multi-Raft: BatchTransport for batched outbound RPC.
  // If set, use batch transport for sending; otherwise fall back to direct rpc_clients_.
  // Uses void* to avoid circular header dependency.
  void* batch_transport_ = nullptr;
  // Callback for routing RPC replies back to this RaftState.
  RaftTransportHandler* transport_handler_ = nullptr;

  // ------------------------------------------------------------------------
  // LRC Latency-Aware Complementary Grouping
  // ------------------------------------------------------------------------
  // Pointer to the LRC complementary grouper (owned by RaftStore, shared across groups)
  void* lrc_grouper_ = nullptr;

  // ------------------------------------------------------------------------
  // PersistQueue - Asynchronous batch persistence
  // ------------------------------------------------------------------------
  // Pointer to the PersistQueue for asynchronous log persistence.
  // Initialized in RaftState::NewRaftState() when storage != nullptr.
  std::unique_ptr<PersistQueue> persist_queue_;

  // Get the last index that has been persisted to disk
  raft_index_t GetLastPersistedIndex() const {
    if (persist_queue_) {
      return persist_queue_->GetLastPersistedIndex();
    }
    return storage_ ? storage_->LastIndex() : 0;
  }

  // Encode entry using LRC with latency-aware orthogonal placement
  // Returns the assigned LRC group ID for this stripe
  // Note: placements are queried from lrc_grouper_ by the sendAppendEntriesLrc function
  int EncodeRaftEntryLrc(raft_index_t raft_index, const multiraft::LrcParams& lrc_params,
                         Stripe* stripe);

  // Pack LRC-encoded fragments into peer-keyed format using lrc_grouper placement rules
  // Each peer receives exactly the fragments assigned to it by GetFragmentsForNode()
  void PackStripesToPeerKeyed(raft_index_t raft_index, Stripe* stripe, const std::string& user_key);

  // Send entries using LRC orthogonal placement
  // Each peer receives exactly 2 fragments based on latency-aware complementary grouping
  void sendAppendEntriesLrc(raft_node_id_t peer, raft_index_t start_index, AppendEntriesArgs* args);
};
}  // namespace raft
