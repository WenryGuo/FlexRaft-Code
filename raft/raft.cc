#include "raft.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "batch_system_bridge.h"
#include "encoder.h"
#include "log_entry.h"
#include "log_manager.h"
#include "raft_node.h"
#include "raft_struct.h"
#include "raft_type.h"
#include "storage.h"
#include "util.h"

// Multi-raft LRC encoding support
#include "lrc_encoder.h"
#include "lrc_complementary_grouper.h"
#include "message.h"  // FragmentPlacement

// Note: stripe_format.h is NOT included here to avoid linking multi-raft library
// Packing is done inline in PackStripesToPeerKeyed()

namespace multiraft {
class LrcComplementaryGrouper;
}

namespace raft {

// ============================================================================
// RaftTransportHandler interface implementation: route async RPC replies back
// into the RaftState processing pipeline. This connects BatchRpcSender's async
// callbacks to RaftState so that heartbeat/AppendEntries replies are handled.
// ============================================================================
void RaftState::HandleRequestVoteReply(const RequestVoteReply& reply) {
  // [DEBUG] 添加日志确认 HandleRequestVoteReply 被调用
  printf("[HANDLE-REPLY-G%d-N%d] HandleRequestVoteReply called: granted=%d term=%d reply_id=%d\n",
         group_id_, id_, reply.vote_granted, reply.term, reply.reply_id);
  fflush(stdout);
  // Make a mutable copy since Process takes a pointer
  RequestVoteReply mutable_reply = reply;
  printf("[HANDLE-REPLY-G%d-N%d] Calling Process(&reply)...\n", group_id_, id_);
  fflush(stdout);
  Process(&mutable_reply);
  printf("[HANDLE-REPLY-G%d-N%d] Process() returned\n", group_id_, id_);
  fflush(stdout);
}
void RaftState::HandleAppendEntriesReply(const AppendEntriesReply& reply) {
  AppendEntriesReply mutable_reply = reply;
  Process(&mutable_reply);
}
void RaftState::HandleRequestFragmentsReply(const RequestFragmentsReply& reply) {
  RequestFragmentsReply mutable_reply = reply;
  Process(&mutable_reply);
}

RaftState *RaftState::NewRaftState(const RaftConfig &config) {
  auto ret = new RaftState;
  ret->id_ = config.id;
  ret->group_id_ = config.group_id;
  ret->N_physical_nodes_ = config.N_physical_nodes;
  ret->dynamic_k_ = config.dynamic_k;
  ret->encoding_mode_ = config.encoding_mode;

  Storage::PersistRaftState state;
  // If the storage provides a valid persisted raft state, use this
  // state to initialize this raft state instance
  if (config.storage != nullptr && (state = config.storage->PersistState(), state.valid)) {
    ret->SetCurrentTerm(state.persisted_term);
    ret->SetVoteFor(state.persisted_vote_for);
    LOG(util::kRaft, "S%d Read Persist Term%d VoteFor%d", ret->id_, ret->CurrentTerm(),
        ret->VoteFor());
  } else {
    ret->SetCurrentTerm(0);
    ret->SetVoteFor(kNotVoted);
    LOG(util::kRaft, "S%d Init with Term%d VoteFor%d", ret->id_, ret->CurrentTerm(),
        ret->VoteFor());
  }
  // On every boot, the raft peer is set to be follower
  ret->SetRole(kFollower);

  for (const auto &[id, rpc] : config.rpc_clients) {
    assert(id < 32 && "node id exceeds raft_peer_/rpc_clients_ array bounds");
    auto peer = new RaftPeer();
    ret->peers_.insert(id);
    ret->raft_peer_[id] = peer;
    ret->rpc_clients_[id] = rpc;
  }

  // Construct log manager from persistence storage
  ret->lm_ = LogManager::NewLogManager(config.storage);

  LOG(util::kRaft, "S%d Log Recover from storage LI%d", ret->id_, ret->lm_->LastLogEntryIndex());

  ret->electionTimeLimitMin_ = config.electionTimeMin;
  ret->electionTimeLimitMax_ = config.electionTimeMax;
  ret->rsm_ = config.rsm;
  ret->heartbeatTimeInterval = config::kHeartbeatInterval;
  ret->storage_ = config.storage;  // might be nullptr

  // PersistQueue: asynchronous batch persistence for unstable log entries
  if (ret->storage_ != nullptr) {
    ret->persist_queue_ = std::make_unique<PersistQueue>(ret->storage_, 32);
    LOG(util::kRaft, "S%d PersistQueue initialized", ret->id_);
  }

  ret->last_applied_ = 0;
  ret->commit_index_ = 0;
  ret->election_round_ = 0;  // Priority-based election: start at round 1.

  ret->PersistRaftState();

  // FlexibleK: Init liveness monitor state
  ret->live_monitor_.node_num = config.rpc_clients.size() + 1;
  ret->live_monitor_.me = ret->id_;

  // Multi-Raft: capture BatchSystemBridge for committed entry application
  if (config.batch_system_bridge != nullptr) {
    ret->batch_system_bridge_ = config.batch_system_bridge;
    ret->bridge_group_id_ = config.bridge_group_id;
    ret->bridge_node_id_ = config.bridge_node_id;
  }

  // Multi-Raft: BatchTransport for batched outbound RPC (if configured).
  if (config.batch_transport != nullptr) {
    ret->batch_transport_ = config.batch_transport;
  }
  ret->transport_handler_ = config.transport_handler;

  // Reserve space for recording commit start time
  ret->commit_start_time_.reserve(100000);

  // Initialize per-instance high-quality RNG with unique seed
  uint32_t seed = std::random_device{}() ^
                  static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ret));
  ret->rng_.seed(seed);

  return ret;
}

void RaftState::Init() {
  // 所有实例同时开始 election timeout 计时
  // 优先级通过 IsEligibleForElection() 控制 eligibility
  // 如果超时但无资格 -> election_round_++ -> 重置 timer
  resetElectionTimer();
  live_monitor_.Init();
}

// RequestVote RPC call
void RaftState::Process(RequestVoteArgs *args, RequestVoteReply *reply) {
  assert(args != nullptr && reply != nullptr);

  live_monitor_.UpdateLiveness(args->candidate_id);

  std::scoped_lock<std::mutex> lck(mtx_);
  LOG(util::kRaft, "S%d RequestVote From S%d AT%d [G%d]", id_, args->candidate_id, args->term, group_id_);
  printf("[ELECTION-G%d-N%d] Received RequestVote from node %d (term=%d, "
         "priority=%d, last_log_index=%u, last_log_term=%d). Our term=%d election_round=%d vote_for=%d\n",
         group_id_, id_, args->candidate_id, args->term,
         args->candidate_priority,
         args->last_log_index, args->last_log_term,
         CurrentTerm(), election_round_, VoteFor());

  reply->reply_id = id_;
  reply->group_id = group_id_;

  // Defensive check: reject vote requests from candidates that are not members of this Group.
  // This prevents cross-Group term interference in a Multi-Raft setup where the unified RPC
  // server receives RequestVote messages for many Groups. Even though messages are routed to
  // the correct RaftState by group_id, we add this check as an additional safety layer.
  // IMPORTANT: check this BEFORE term comparison so that non-members never cause us to
  // step down from leadership.
  if (!IsGroupMember(args->candidate_id)) {
    // [COMMENTED] Rejected non-group-member vote request log
    reply->term = CurrentTerm();
    reply->vote_granted = false;
    return;
  }

  // The request server has smaller term, just refuse this vote request
  // immediately and return my term to update its term
  if (args->term < CurrentTerm()) {
    // [COMMENTED] Stale term refusal log
    reply->term = CurrentTerm();
    reply->vote_granted = false;
    return;
  }

  // If this request carries a higher term, then convert my role to be
  // follower. And reset voteFor attribute for voting in this new term
  if (args->term > CurrentTerm() || Role() == kCandidate) {
    printf("[ELECTION-G%d-N%d] Stepped down from leadership. Reason: received "
           "RequestVote from node %d with higher term=%d. Our term=%d\n",
           group_id_, id_, args->candidate_id, args->term, CurrentTerm());
    convertToFollower(args->term);
  }

  reply->term = CurrentTerm();

  // Check if vote for this requesting server. Rule1 checks if current server
  // has voted; Rule2 checks if requesting server's log is newer
  bool rule1 = (VoteFor() == kNotVoted || VoteFor() == args->candidate_id);
  bool rule2 = isLogUpToDate(args->last_log_index, args->last_log_term);

  // Vote for this requesting server
  if (rule1 && rule2) {
    LOG(util::kRaft, "S%d VoteFor S%d [G%d]", id_, args->candidate_id, group_id_);
    printf("[ELECTION-G%d-N%d] Granted vote to node %d (term=%d, vote_for=%d)\n",
           group_id_, id_, args->candidate_id, args->term, VoteFor());
    reply->vote_granted = true;
    SetVoteFor(args->candidate_id);

    // persist vote for since it has been changed
    PersistRaftState();
    resetElectionTimer();
    return;
  }

  LOG(util::kRaft, "S%d RefuseVote R1=%d R2=%d [G%d]", id_, rule1, rule2, group_id_);
  // [COMMENTED] Rule-based vote refusal log
  // Refuse vote for this server
  reply->vote_granted = false;
  return;
}


void RaftState::Process(AppendEntriesArgs *args, AppendEntriesReply *reply) {
  // [DEBUG] Log AppendEntries received
  fprintf(stderr, "[AE] G%d-N%d: Received AE from N%d, entries=%ld, prev_idx=%d, prev_term=%d, prev_k=%d, leader_commit=%d\n",
          group_id_, id_, args->leader_id, (long)args->entries.size(),
          args->prev_log_index, args->prev_log_term, args->prev_k, args->leader_commit);
  fflush(stderr);

  assert(args != nullptr && reply != nullptr);

  live_monitor_.UpdateLiveness(args->leader_id);

  std::scoped_lock<std::mutex> lck(mtx_);
  // Received heartbeat from leader: reset election_round to 0
  election_round_ = 0;

  // Distinguish heartbeat (empty entries) from normal AppendEntries
  if (args->entries.empty()) {
    // [COMMENTED] Heartbeat received log
  } else {
    // [COMMENTED] AppendEntries received log (verbose)
    // printf("[AE-G%d-N%d] <- N%d: Received AppendEntries(...)\n", ...);
  }

  reply->reply_id = id_;
  reply->group_id = group_id_;
  reply->chunk_info_cnt = 0;

  // Reply false immediately if arguments' term is smaller
  if (args->term < CurrentTerm()) {
    reply->success = false;
    reply->term = CurrentTerm();
    reply->expect_index = 0;
    LOG(util::kRaft, "S%d reply to S%d with T%d EI%d", id_, args->leader_id, reply->term,
        reply->expect_index);
    return;
  }

  // Only convert to follower if term increased (not just because we're a Candidate)
  // This prevents race conditions where a Candidate receiving a Leader's heartbeat
  // immediately steps down before its own election completes
  if (args->term > CurrentTerm() || Role() == kCandidate) {
    convertToFollower(args->term);
  }
  
  resetElectionTimer();

  // [DEBUG] Safety check after resetElectionTimer
  if (election_time_out_ <= 0) {
    fprintf(stderr, "[DEBUG-AE] G%d-N%d: WARNING election_time_out_=%ld after reset\n",
            group_id_, id_, (long)election_time_out_);
  }

  // Step2: Check if current server contains a log entry at prev log index with
  // prev log term
  if (!containEntry(args->prev_log_index, args->prev_log_term, args->prev_k)) {
    // Reply false immediately since current server lacks one log entry: notify
    // the leader to send older entries
    reply->success = false;
    reply->term = CurrentTerm();
    // The check failes, the leader should send more "previous" entries
    // reply->expect_index = args->prev_log_index;
    reply->expect_index = std::min(lm_->LastLogEntryIndex() + 1, args->prev_log_index);
    LOG(util::kRaft, "S%d reply with expect index=%d", id_, reply->expect_index);
    return;
  }

  // Step3: Check conflicts and add new entries
  assert(args->entry_cnt == args->entries.size());
  if (args->entry_cnt > 0) {
    // [DEBUG] Log before checkConflictEntryAndAppendNew
    fprintf(stderr, "[AE] G%d-N%d: Calling checkConflictEntryAndAppendNew, entry_cnt=%d\n",
            group_id_, id_, args->entry_cnt);
    fflush(stderr);
    checkConflictEntryAndAppendNew(args, reply);
  }
  reply->expect_index = args->prev_log_index + args->entry_cnt + 1;
  LOG(util::kRaft, "S%d reply with expect index=%d", id_, reply->expect_index);

  // Step4: Update commit index if necessary
  if (args->leader_commit > CommitIndex()) {
    auto old_commit_idx = CommitIndex();
    raft_index_t new_entry_idx = args->prev_log_index + args->entries.size();
    auto update_commit_idx = std::min(args->leader_commit, new_entry_idx);
    SetCommitIndex(std::min(update_commit_idx, lm_->LastLogEntryIndex()));

    LOG(util::kRaft, "S%d Update CommitIndex (%d->%d)", id_, old_commit_idx, CommitIndex());
  }

  // TODO: Notify applier thread to apply newly committed entries to state
  // machine

  reply->term = CurrentTerm();
  reply->success = true;

  // [DEBUG] Log reply being sent
  fprintf(stderr, "[AE] G%d-N%d: Sending REPLY to N%d success=1 term=%d expect_idx=%d\n",
          group_id_, id_, args->leader_id, reply->term, reply->expect_index);
  fflush(stderr);

  // Commit index might have been changed, try apply committed entries
  tryApplyLogEntries();

  return;
}

void RaftState::Process(AppendEntriesReply *reply) {
  assert(reply != nullptr);

  // [DEBUG] Log ALL AE replies to diagnose replication issues
  fprintf(stderr, "[AE-REPLY] G%d-N%d: Received AE Reply from N%d success=%d term=%d expect_idx=%d\n",
          group_id_, id_, reply->reply_id, reply->success, reply->term, reply->expect_index);
  fflush(stderr);

  // Note: Liveness monitor must be processed without exclusive access
  live_monitor_.UpdateLiveness(reply->reply_id);

  std::scoped_lock<std::mutex> lck(mtx_);

  // NOTE: AE reply logging removed to prevent log flooding.
  // Log only anomalous cases (reply not successful).
  if (reply->success == 0) {
    // [COMMENTED] AE reply FAIL log (verbose)
  }

  // Check if this reply is expired
  if (Role() != kLeader || reply->term < CurrentTerm()) {
    return;
  }

  if (reply->term > CurrentTerm()) {
    convertToFollower(reply->term);
    return;
  }

  auto peer_id = reply->reply_id;

  // Safety check: ensure peer_id is in bounds BEFORE accessing raft_peer_
  if (peer_id >= 32) {
    fprintf(stderr, "[AE-REPLY-G%d-N%d] WARNING: Invalid peer %d (out of bounds)\n",
            group_id_, id_, peer_id);
    return;
  }

  auto node = raft_peer_[peer_id];
  if (node == nullptr) {
    fprintf(stderr, "[AE-REPLY-G%d-N%d] WARNING: Invalid peer %d (raft_peer_ is null)\n",
            group_id_, id_, peer_id);
    return;
  }

  if (reply->success) {  // Requested entries are successfully replicated
    // Update nextIndex and matchIndex for this server
    auto update_nextIndex = reply->expect_index;
    auto update_matchIndex = update_nextIndex - 1;

    if (auto ctx = GetRecoveryCtx(reply->reply_id); ctx) {
      if (reply->chunk_info_cnt &&
          reply->chunk_infos.at(0).raft_index == ctx->start_recovery_index_) {
        auto peer = reply->reply_id;
        auto dura = util::DurationToMicros(ctx->start_time_, util::NowTime());

        EmitRecoveryRecord(peer, reply->chunk_infos.size(), dura);
        // Clear the context related to recovery in case that two recovery operations mixed up
        ClearRecoveryCtx(peer);

        // [COMMENTED] Recover chunk stats log
      }
    }

    if (node->NextIndex() < update_nextIndex) {
      node->SetNextIndex(update_nextIndex);
      LOG(util::kRaft, "S%d update peer S%d NI%d", id_, peer_id, node->NextIndex());
    }

    if (node->MatchIndex() < update_matchIndex) {
      node->SetMatchIndex(update_matchIndex);
      LOG(util::kRaft, "S%d update peer S%d MI%d", id_, peer_id, node->MatchIndex());
    }

    fprintf(stderr, "[MATCH] G%d-N%d: N%d reply has %zu chunk_infos\n",
            group_id_, id_, reply->reply_id, reply->chunk_infos.size());

    for (const auto &ci : reply->chunk_infos) {
      auto raft_index = ci.GetRaftIndex();
      if (node->matchChunkInfo.count(raft_index) == 0 ||
          ci.GetK() < node->matchChunkInfo[raft_index].GetK()) {
        node->matchChunkInfo[raft_index] = ci;
      }
    }

    // [DEBUG] About to call tryUpdateCommitIndex
    fprintf(stderr, "[AE-REPLY] G%d-N%d: tryUpdateCommitIndex called\n", group_id_, id_);

    tryUpdateCommitIndex();

    fprintf(stderr, "[AE-REPLY] G%d-N%d: tryUpdateCommitIndex done\n", group_id_, id_);
  } else {
    // NOTE: Simply set NextIndex to be expect_index might be error since the
    // message comes from reply might not be meaningful message Update nextIndex
    // to be expect index reply->expect_index = 0 means when receiving this AE
    // args, the server has higher term, thus this expect_index is of no means
    if (reply->expect_index != 0) {
      node->SetNextIndex(reply->expect_index);
      LOG(util::kRaft, "S%d Update S%d NI%d", id_, peer_id, node->NextIndex());
    }
  }

  // [DEBUG] About to call tryApplyLogEntries
  fprintf(stderr, "[AE-REPLY] G%d-N%d: tryApplyLogEntries called\n", group_id_, id_);
  fflush(stderr);

  // TODO: May require applier to apply this log entry
  tryApplyLogEntries();

  fprintf(stderr, "[AE-REPLY] G%d-N%d: tryApplyLogEntries returned, EXIT Process(AppendEntriesReply)\n",
          group_id_, id_);
  fflush(stderr);
  return;
}

void RaftState::Process(RequestVoteReply *reply) {
  assert(reply != nullptr);

  // [DEBUG] 添加详细日志 - 加锁前的状态
  int pre_lock_role = Role();
  int pre_lock_term = CurrentTerm();
  int pre_lock_vote_cnt = vote_me_cnt_;
  printf("[PROCESS-VOTEREP-G%d-N%d] Process(RequestVoteReply) ENTER: "
         "reply_id=%d term=%d granted=%d [PRE-LOCK] role=%d term=%d vote_cnt=%d\n",
         group_id_, id_, reply->reply_id, reply->term, reply->vote_granted,
         pre_lock_role, pre_lock_term, pre_lock_vote_cnt);
  fflush(stdout);

  // 添加：即将获取锁的地址
  printf("[PROCESS-VOTEREP-G%d-N%d] [DEBUG] About to acquire mutex mtx_ at %p\n",
         group_id_, id_, (void*)&mtx_);
  fflush(stdout);

  live_monitor_.UpdateLiveness(reply->reply_id);

  // 添加：UpdateLiveness之后
  printf("[PROCESS-VOTEREP-G%d-N%d] [DEBUG] After UpdateLiveness, acquiring lock...\n",
         group_id_, id_);
  fflush(stdout);

  std::scoped_lock<std::mutex> lck(mtx_);

  // 添加：成功获取锁
  printf("[PROCESS-VOTEREP-G%d-N%d] [LOCKED] vote_me_cnt=%d (role=%d term=%d reply->term=%d)\n",
         group_id_, id_, vote_me_cnt_, Role(), CurrentTerm(), reply->term);
  fflush(stdout);

  LOG(util::kRaft, "S%d HandleVoteResp from S%d term=%d grant=%d [G%d]", id_, reply->reply_id,
      reply->term, reply->vote_granted, group_id_);

  // 添加：验证计数器一致性
  printf("[PROCESS-VOTEREP-G%d-N%d] [DEBUG] Counter check: vote_me_cnt_=%d, pre_lock_vote_cnt=%d, diff=%d\n",
         group_id_, id_, vote_me_cnt_, pre_lock_vote_cnt, vote_me_cnt_ - pre_lock_vote_cnt);
  fflush(stdout);

  // Current raft peer is no longer candidate, or the term is expired
  printf("[PROCESS-VOTEREP-G%d-N%d] Checking: Role=%d (expected %d=kCandidate), reply->term=%d CurrentTerm=%d\n",
         group_id_, id_, Role(), kCandidate, reply->term, CurrentTerm());
  if (Role() != kCandidate || reply->term < CurrentTerm()) {
    printf("[PROCESS-VOTEREP-G%d-N%d] REJECT: Role!=Candidate or reply term < current term. "
           "Role=%d kCandidate=%d\n",
           group_id_, id_, Role(), kCandidate);
    fflush(stdout);
    return;
  }

  // Receive higher raft term, convert to be follower
  if (reply->term > CurrentTerm()) {
    printf("[ELECTION-G%d-N%d] Received higher term %d from vote response. "
           "Converting to follower (our term=%d)\n",
           group_id_, id_, reply->term, CurrentTerm());
    convertToFollower(reply->term);
    return;
  }

  if (reply->vote_granted == true) {
    printf("[PROCESS-VOTEREP-G%d-N%d] Vote GRANTED! Incrementing vote count from %d...\n",
           group_id_, id_, vote_me_cnt_);
    fflush(stdout);
    incrementVoteMeCnt();
    printf("[PROCESS-VOTEREP-G%d-N%d] Vote count updated: %d/%d (need %d, livenessLevel=%d)\n",
           group_id_, id_, vote_me_cnt_, GetClusterServerNumber(), livenessLevel() + 1, livenessLevel());
    // Win votes of the majority of the cluster
    printf("[PROCESS-VOTEREP-G%d-N%d] Checking win condition: vote_me_cnt=%d >= livenessLevel+1=%d ?\n",
           group_id_, id_, vote_me_cnt_, livenessLevel() + 1);
    if (vote_me_cnt_ >= livenessLevel() + 1) {
      printf("[ELECTION-G%d-N%d] Won election (vote_count=%d/%d). "
             "Entering PreLeader phase. Term=%d, commit_index=%u, last_log_index=%u\n",
             group_id_, id_, vote_me_cnt_, GetClusterServerNumber(),
             CurrentTerm(), CommitIndex(), lm_->LastLogEntryIndex());
      fflush(stdout);
      convertToPreLeader();
    } else {
      printf("[PROCESS-VOTEREP-G%d-N%d] Not enough votes yet. Need %d more.\n",
             group_id_, id_, livenessLevel() + 1 - vote_me_cnt_);
    }
  } else {
    printf("[PROCESS-VOTEREP-G%d-N%d] Vote NOT granted by node %d\n", group_id_, id_, reply->reply_id);
  }
  return;
}

void RaftState::Process(RequestFragmentsArgs *args, RequestFragmentsReply *reply) {
  assert(args != nullptr && reply != nullptr);

  live_monitor_.UpdateLiveness(args->leader_id);

  std::scoped_lock<std::mutex> lck(mtx_);

  LOG(util::kRaft, "S%d RECV ReqFrag From S%d(EC) (T%d SI=%d EI=%d)", id_, args->leader_id,
      args->term, args->start_index, args->last_index);

  reply->reply_id = id_;
  reply->start_index = args->start_index;

  if (args->term < CurrentTerm()) {
    reply->term = CurrentTerm();
    reply->entry_cnt = 0;
    reply->fragments.clear();
    reply->success = false;

    LOG(util::kRaft, "S%d REFUSE ReqFrag: Higher Term(%d>%d)", id_, CurrentTerm(), args->term);
    return;
  }

  // Only convert to follower if term increased (not just because we're a Candidate)
  // This prevents race conditions where a Candidate receiving a Leader's heartbeat
  // immediately steps down before its own election completes
  if (args->term > CurrentTerm() || Role() == kCandidate) {
    convertToFollower(args->term);
  }
  resetElectionTimer();

  raft_index_t raft_index = args->start_index;
  for (; raft_index <= args->last_index; ++raft_index) {
    if (auto ptr = lm_->GetSingleLogEntry(raft_index); ptr) {
      reply->fragments.push_back(*ptr);
    } else {
      break;
    }
  }
  LOG(util::kRaft, "S%d Submit fragments(I%d->I%d)", id_, args->start_index, raft_index - 1);

  reply->term = CurrentTerm();
  reply->success = true;
  reply->entry_cnt = reply->fragments.size();
  return;
}

void RaftState::Process(RequestFragmentsReply *reply) {
  assert(reply != nullptr);

  live_monitor_.UpdateLiveness(reply->reply_id);

  std::scoped_lock<std::mutex> lck(mtx_);

  LOG(util::kRaft, "S%d RECV ReqFragReply From S%d", id_, reply->reply_id);

  if (Role() != kPreLeader || reply->term < CurrentTerm()) {
    return;
  }

  if (reply->term > CurrentTerm()) {
    convertToFollower(reply->term);
    return;
  }

  LOG(util::kRaft, "S%d ReqFrag Resp (Cnt=%d)", id_, reply->entry_cnt);

  // May ommit duplicate response
  if (preleader_stripe_store_.IsCollected(reply->reply_id)) {
    LOG(util::kRaft, "S%d ommit ReqFragReply from S%d", id_, reply->reply_id);
    return;
  }

  // TODO: RequestFragments may occur multiple times
  // TODO: Store collected fragments into some place and decode them to get the
  // complete entry
  //
  int check_idx = 0;
  for (const auto &entry : reply->fragments) {
    assert(check_idx + reply->start_index == entry.Index());

    // Debug:
    // --------------------------------------------------------------------
    LOG(util::kRaft, "S%d add Frag at I%d info:%s, FragId=%d", id_, entry.Index(),
        entry.ToString().c_str(), reply->reply_id);
    // --------------------------------------------------------------------
    preleader_stripe_store_.AddFragments(entry.Index(), entry,
                                         static_cast<raft_frag_id_t>(reply->reply_id));
    check_idx++;
  }

  preleader_stripe_store_.UpdateResponseState(reply->reply_id);
  PreLeaderBecomeLeader();
  return;
}

ProposeResult RaftState::Propose(const CommandData &command) {
  // Phase 1: Pre-compute expensive operations OUTSIDE the lock
  // This avoids blocking other operations while encoding/placing fragments
  std::vector<FragmentPlacement> placement;
  std::vector<Slice> all_frags;
  ChunkInfo ci{0, 0};
  int total_frags = 0;
  bool use_lrc = (encoding_mode_ == 2 && HasLrcGrouper());
  fprintf(stderr, "[DEBUG-PROPOSE] G%d-N%d: use_lrc=%d enc_mode=%d has_lrc=%d\n",
          group_id_, id_, use_lrc ? 1 : 0, encoding_mode_, HasLrcGrouper() ? 1 : 0);
  fflush(stderr);

  if (use_lrc) {
    auto* grouper = static_cast<multiraft::LrcComplementaryGrouper*>(lrc_grouper_);
    const auto& lrc_params = grouper->GetLrcParams();

    // Get placement (this is read-only and can be computed outside lock)
    auto multiraft_placement = grouper->GetNodePlacementsVector();
    placement.reserve(multiraft_placement.size());
    for (const auto& mp : multiraft_placement) {
      FragmentPlacement fp;
      fp.frag_id = mp.frag_id;
      fp.local_group = mp.local_group;
      fp.node_id = mp.node_id;
      fp.kind = static_cast<FragmentPlacement::Kind>(mp.kind);
      placement.push_back(fp);
    }

    // LRC encode (expensive operation done outside lock)
    Slice data_to_encode(command.command_data.data() + command.start_fragment_offset,
                     command.command_data.size() - command.start_fragment_offset);
    
    multiraft::LrcEncoder encoder(lrc_params);
    multiraft::LrcStripe stripe;
    if (encoder.EncodeStripe(data_to_encode, &stripe)) {
      ci.SetK(lrc_params.k);
      ci.SetL(lrc_params.l);
      ci.SetR(lrc_params.r);
      ci.SetLrcGroupId(grouper->GetLrcGroupIdForNode(id_));
      ci.SetRaftIndex(0);  // index set later

      // === KEY META（新增）：用于 key-only 复制路径 ===
      // STRI header layout: magic(4) + group_id(8) + mode(1) + key_len(8) + key(N) + val_len(8) = 29 + N
      // start_fragment_offset = 29 + N, so key_size = start_fragment_offset - 29 = N (the real key length).
      ci.SetKeySize(command.start_fragment_offset - (4 + 8 + 1 + 8 + 8));  // = key_len
      ci.SetTotalSize(command.command_data.size() - command.start_fragment_offset);
      LOG(util::kRaft, "S%d LRC key meta: key_size=%u total_size=%u cmd_size=%zu",
          id_, ci.GetKeySize(), ci.GetTotalSize(), command.command_data.size());

      total_frags = stripe.total_frags();

      // Collect all fragments (must deep-copy: stripe's internal buffers will be freed when stripe goes out of scope)
      all_frags.reserve(stripe.total_frags());
      for (const auto& f : stripe.data_shards) all_frags.push_back(Slice::Copy(f));
      for (const auto& f : stripe.local_parities) all_frags.push_back(Slice::Copy(f));
      for (const auto& f : stripe.global_parities) all_frags.push_back(Slice::Copy(f));
    } else {
      LOG(util::kRaft, "S%d LRC encoding failed", id_);
      return ProposeResult{0, 0, false};
    }
  }

  // Phase 2: Critical section - atomic index allocation and entry append
  // FIX: Previous design had race condition where multiple threads could get
  // the same index. Now we atomically assign index and append in one lock.
  raft_index_t next_entry_index;
  raft_term_t propose_term;
  {
    std::unique_lock<std::mutex> lck(mtx_);

    if (Role() != kLeader) {
      return ProposeResult{0, 0, false};
    }

    propose_term = CurrentTerm();

    // Build entry with placeholder index (will be set atomically)
    LogEntry entry;
    entry.SetType(kNormal);
    entry.SetCommandData(command.command_data);
    entry.SetTerm(propose_term);
    entry.SetStartOffset(command.start_fragment_offset);

    if (use_lrc) {
      auto* grouper = static_cast<multiraft::LrcComplementaryGrouper*>(lrc_grouper_);
      entry.SetChunkInfo(ci);
      entry.SetPlacement(placement);
      for (const auto& frag : all_frags) {
        entry.AddFragment(frag);
      }
    } else {
      entry.SetChunkInfo({0, 0});  // placeholder
    }

    // Atomic: allocate index AND append to ring buffer in one lock acquisition
    auto result = lm_->AppendLogEntryAtomic(entry);
    if (result.status != kOk) {
      LOG(util::kRaft, "S%d AppendLogEntryAtomic failed", id_);
      return ProposeResult{0, 0, false};
    }
    next_entry_index = result.index;

    // Update LRC chunk info with actual index
    if (use_lrc) {
      auto* grouper = static_cast<multiraft::LrcComplementaryGrouper*>(lrc_grouper_);
      auto* stored_entry = lm_->GetSingleLogEntry(next_entry_index);
      if (stored_entry) {
        ci.SetRaftIndex(next_entry_index);
        stored_entry->SetChunkInfo(ci);
      }
    }

    LOG(util::kRaft, "S%d Propose atomic append I%d T%d (ptr=%p)",
        id_, next_entry_index, propose_term, command.command_data.data());
  }
  // Lock released here

  // Phase 3: Asynchronous persistence via PersistQueue
  // FIX: Previous design blocked on storage_->Sync() causing:
  // 1. Performance bottleneck (disk I/O on critical path)
  // 2. Rollback races (DeleteEntriesFrom could delete other thread's data)
  // Now we push to PersistQueue which handles batching and background sync.
  auto* entry_for_persist = lm_->GetSingleLogEntry(next_entry_index);
  if (entry_for_persist && persist_queue_) {
    // Copy entry to push (PersistQueue takes ownership of its internal copy)
    LogEntry entry_copy = *entry_for_persist;
    persist_queue_->Push(std::move(entry_copy), next_entry_index);
    LOG(util::kRaft, "S%d Pushed I%d to PersistQueue (async)", id_, next_entry_index);
  }

  // Record the start time of committing an entry
  commit_start_time_[next_entry_index] = std::chrono::high_resolution_clock::now();

  // Phase 4: Trigger replication (parallel with persistence)
  // FIX: Previous design waited for Sync() before replicating.
  // Now we replicate immediately while persistence happens in background.
  ReplicateNewProposeEntry(next_entry_index);

  return ProposeResult{next_entry_index, propose_term, true};
}

bool RaftState::isLogUpToDate(raft_index_t raft_index, raft_term_t raft_term) {
  LOG(util::kRaft, "S%d CheckLog (LastTerm=%d ArgTerm=%d) (LastIndex=%d ArgIndex=%d)", id_,
      lm_->LastLogEntryTerm(), raft_term, lm_->LastLogEntryIndex(), raft_index);

  if (raft_term > lm_->LastLogEntryTerm()) {
    return true;
  }
  if (raft_term == lm_->LastLogEntryTerm() && raft_index >= lm_->LastLogEntryIndex()) {
    return true;
  }
  return false;
}

void RaftState::checkConflictEntryAndAppendNew(AppendEntriesArgs *args, AppendEntriesReply *reply) {
  assert(args->entry_cnt == args->entries.size());
  auto old_idx = lm_->LastLogEntryIndex();
  auto array_index = 0;

  for (; array_index < args->entries.size(); ++array_index) {
    auto raft_index = array_index + args->prev_log_index + 1;
    if (raft_index > lm_->LastLogEntryIndex()) {
      break;
    }
    if (args->entries[array_index].Term() != lm_->TermAt(raft_index)) {
      // Debug --------------------------------------------
      auto old_last_index = lm_->LastLogEntryIndex();
      lm_->DeleteLogEntriesFrom(raft_index);

      // FIX: Check PersistQueue before deleting from storage.
      // If the entry is still in PersistQueue (unpersisted), we don't need to
      // delete from storage - it was never persisted. This prevents race where
      // storage could have been written by the background worker while we're
      // trying to truncate.
      if (storage_ != nullptr) {
        if (persist_queue_ && persist_queue_->IsUnpersisted(raft_index)) {
          LOG(util::kRaft, "S%d Skip storage rollback for I%d (still unpersisted)",
              id_, raft_index);
          persist_queue_->TruncateFrom(raft_index);
        } else {
          storage_->DeleteEntriesFrom(raft_index);
        }
      }

      LOG(util::kRaft, "S%d Del Entry (%d->%d)", id_, old_last_index, lm_->LastLogEntryIndex());
      break;
    }

    bool do_overwrite = false;

    // Check if we need to overwrite this entry, note that all these entries are
    // aligned with (index, term)
    auto ent = lm_->GetSingleLogEntry(raft_index);
    if (NeedOverwriteLogEntry(ent->GetChunkInfo(), args->entries[array_index].GetChunkInfo())) {
      lm_->OverWriteLogEntry(args->entries[array_index], raft_index);
      if (!do_overwrite) {
        if (storage_) {
          storage_->OverwriteEntry(raft_index, args->entries[array_index]);
        }
        do_overwrite = true;
      } else {
        if (storage_) {
          storage_->AppendEntry(*lm_->GetSingleLogEntry(raft_index));
        }
      }
      LOG(util::kRaft, "S%d OVERWRITE I%d ConflictIndex=I%d", id_, raft_index, raft_index);
    } else {
      // Just simply overwrite version number
      LOG(util::kRaft, "S%d I%d ChunkInfo(%s)", id_, raft_index,
          args->entries[array_index].GetChunkInfo().ToString().c_str());
      // A previous entry might have been truncated, append it
      if (do_overwrite && storage_) {
        storage_->AppendEntry(*lm_->GetSingleLogEntry(raft_index));
      }
    }

    ent = lm_->GetSingleLogEntry(raft_index);
    reply->chunk_infos.push_back(ent->GetChunkInfo());
    LOG(util::kRaft, "S%d REPLY (I%d T%d ChunkInfo(%s))", id_, raft_index, ent->Term(),
        ent->GetChunkInfo().ToString().c_str());
  }
  // For those new entries
  auto old_last_index = lm_->LastLogEntryIndex();
  for (auto i = array_index; i < args->entries.size(); ++i) {
    auto raft_index = args->prev_log_index + i + 1;
    // Debug -------------------------------------------
    lm_->AppendLogEntry(args->entries[i]);
    if (storage_) {
      storage_->AppendEntry(args->entries[i]);
    }

    auto reply_chunk_info = args->entries[i].GetChunkInfo();
    LOG(util::kRaft, "S%d APPEND I%d ChunkInfo(%s)", id_, raft_index,
        reply_chunk_info.ToString().c_str());
    reply->chunk_infos.push_back(reply_chunk_info);
  }

  LOG(util::kRaft, "S%d APPEND(%d->%d) ENTCNT=%d", id_, old_last_index, lm_->LastLogEntryIndex(),
      args->entries.size());

  reply->chunk_info_cnt = reply->chunk_infos.size();

  // Persist newly added log entries, or persist the changes to deleted log
  // entries
  if (storage_ != nullptr) {
    // storage_->Sync();
  }
}

void RaftState::tryUpdateCommitIndex() {
  // [DEBUG] Enter tryUpdateCommitIndex
  fprintf(stderr, "[COMMIT] G%d-N%d: tryUpdateCommitIndex ENTER CommitIdx=%d LastIdx=%d\n",
          group_id_, id_, CommitIndex(), lm_->LastLogEntryIndex());
  fflush(stderr);

  for (auto N = CommitIndex() + 1; N <= lm_->LastLogEntryIndex(); ++N) {
    int agree_cnt = 1;

    // [FIX] For LRC entries, get k from chunk info instead of checking encoded_stripe_
    auto* ent = lm_->GetSingleLogEntry(N);
    bool is_lrc_entry = (ent && ent->IsLrcEncoded());

    // The entry at index N has not been replicated yet, which means index >=
    // N has not been replicated neither, directly return
    // For LRC entries, we skip the encoded_stripe_ check since LRC doesn't use it
    if (!is_lrc_entry && encoded_stripe_.count(N) == 0) {
      return;
    }

    // Last encoding k is like a requirement for commitment
    // For LRC entries, get k from chunk info; for RS entries, get from last_encoding_
    raft_encoding_param_t commit_require_k;
    if (is_lrc_entry) {
      commit_require_k = ent->GetChunkInfo().GetK();
    } else {
      commit_require_k = GetLastEncodingK(N);
    }

    // Get the number of agreement for now
    for (auto id : peers_) {
      auto node = raft_peer_[id];
      if (node == nullptr) {
        continue;
      }
      if (node->matchChunkInfo.count(N) == 0) {
        continue;
      }
      int peer_k = node->matchChunkInfo[N].GetK();
      bool k_match = (peer_k == commit_require_k);
      if (k_match) {
        agree_cnt += 1;
      }
    }

    // LRC commit threshold: F + ceil(k/2)
    int commit_threshold = livenessLevel() + (commit_require_k + 1) / 2;

    // For LRC, commit requires F + ceil(k/2) agreeing nodes
    if (agree_cnt >= commit_threshold &&
        lm_->GetSingleLogEntry(N)->Term() == CurrentTerm()) {
      SetCommitIndex(N);
      fprintf(stderr, "[COMMIT] G%d-N%d: COMMITTED index=%d (agree=%d req=%d)\n",
              group_id_, id_, N, agree_cnt, commit_threshold);
      // Index N is committed, no need to track them any more
      // removeLastReplicateVersionAt(N);
      // removeTrackVersionOfAll(N);

      // Update the commit latency of all entry before N
      // !!! Perf: Recoding Commit latency
      if (commit_start_time_.count(N) != 0) {
        auto end = std::chrono::high_resolution_clock::now();
        auto dura =
            std::chrono::duration_cast<std::chrono::microseconds>(end - commit_start_time_[N]);
        commit_elapse_time_[N] = dura.count();
      }
    } else {
      fprintf(stderr, "[COMMIT] G%d-N%d: I%d NOT COMMITTED (agree=%d < req=%d)\n",
              group_id_, id_, N, agree_cnt, commit_threshold);
    }
  }
  fprintf(stderr, "[COMMIT] G%d-N%d: tryUpdateCommitIndex EXIT CommitIdx=%d\n",
          group_id_, id_, CommitIndex());
  fflush(stderr);
}

// TODO: Use a specific thread to commit applied entries to application
// Do not call this function in RPC, which may results in blocked RPC
void RaftState::tryApplyLogEntries() {
  // Debug logging removed - only log when there are actual entries to apply
  while (last_applied_ < commit_index_) {
    auto old_apply_idx = last_applied_;

    // apply this message on state machine:
    if (rsm_ != nullptr) {
      // In asynchronize applying scheme, the applier thread may find that one
      // entry has been released due to the main thread adding more commands.
      LogEntry ent;
      auto stat = lm_->GetSingleLogEntry(last_applied_ + 1);
      if (stat == nullptr) {
        fprintf(stderr, "[APPLY-ERROR] G%d-N%d: GetSingleLogEntry failed for idx=%d\n",
                group_id_, id_, last_applied_ + 1);
        break;
      }
      ent = *stat;
      rsm_->ApplyLogEntry(ent);
      LOG(util::kRaft, "S%d Push ent(I%d T%d) to channel", id_, ent.Index(), ent.Term());
    }
    if (batch_system_bridge_ != nullptr) {
      // Multi-Raft path: push committed entry to ApplyFsm mailbox via bridge
      auto stat = lm_->GetSingleLogEntry(last_applied_ + 1);
      if (stat == nullptr) {
        fprintf(stderr, "[APPLY-ERROR] G%d-N%d: GetSingleLogEntry failed for idx=%d\n",
                group_id_, id_, last_applied_ + 1);
        break;
      }
      LogEntry ent = *stat;

      auto* bridge = static_cast<BatchSystemBridge*>(batch_system_bridge_);
      BatchSystemBridge::ApplyEntryToGroup(bridge_group_id_, bridge_node_id_, ent);
      LOG(util::kRaft, "S%d Push ent(I%d T%d) to ApplyFsm via bridge", id_, ent.Index(), ent.Term());
    }
    last_applied_ += 1;
    LOG(util::kRaft, "S%d APPLY(%d->%d)", id_, old_apply_idx, last_applied_);
  }
}

void RaftState::convertToFollower(raft_term_t term) {
  // This assertion ensures that the server will only convert to follower with
  // higher term. i.e. The term attribute in followre is monotonically
  // increasing
  assert(term >= CurrentTerm());
  LOG(util::kRaft, "S%d ToFollower(T%d->T%d) [G%d]", id_, CurrentTerm(), term, group_id_);
  printf("[ELECTION-G%d-N%d] Role changed: %s -> Follower (term=%d->%d) election_round=%d\n",
         group_id_, id_,
         Role() == kLeader    ? "Leader" :
         Role() == kCandidate ? "Candidate" :
         Role() == kPreLeader ? "PreLeader" : "Follower",
         CurrentTerm(), term, election_round_);

  SetRole(kFollower);
  if (term > CurrentTerm()) {
    SetVoteFor(kNotVoted);
    SetCurrentTerm(term);
    PersistRaftState();
  }
}

void RaftState::convertToCandidate() {
  printf("[ELECTION-G%d-N%d] Role changed: %s -> Candidate "
         "(term=%d, election_round=%d, priority_rank=%d/%d)\n",
         group_id_, id_,
         Role() == kFollower ? "Follower" :
         Role() == kPreLeader ? "PreLeader" : "Candidate",
         CurrentTerm() + 1, election_round_ + 1,
         GetPriorityRank(), GetClusterServerNumber());
  SetRole(kCandidate);
  election_round_++;
  resetElectionTimer();
  startElection();
}

void RaftState::convertToLeader() {
  printf("[ELECTION-G%d-N%d] [DEBUG] convertToLeader ENTRY: acquiring lock\n", group_id_, id_);
  fflush(stdout);
  std::scoped_lock<std::mutex> lck(mtx_);
  printf("[ELECTION-G%d-N%d] [DEBUG] convertToLeader: lock acquired, calling convertToLeaderInternal()\n",
         group_id_, id_);
  fflush(stdout);
  convertToLeaderInternal();
  printf("[ELECTION-G%d-N%d] [DEBUG] convertToLeader: convertToLeaderInternal() returned, Role=%d\n",
         group_id_, id_, Role());
  fflush(stdout);
}

void RaftState::convertToLeaderInternal() {
  printf("[ELECTION-G%d-N%d] [DEBUG] convertToLeaderInternal ENTRY: Role()=%d (kPreLeader=%d)\n",
         group_id_, id_, Role(), kPreLeader);
  fflush(stdout);

  // Re-check Role under lock to detect stale calls
  if (Role() != kPreLeader) {
    printf("[ELECTION-G%d-N%d] [WARN] convertToLeaderInternal: Role is not kPreLeader, returning! Role=%d\n",
           group_id_, id_, Role());
    fflush(stdout);
    return;
  }

  LOG(util::kRaft, "S%d ToLeader(T%d) LI%d [G%d]", id_, CurrentTerm(), lm_->LastLogEntryIndex(), group_id_);
  SetRole(kLeader);

  election_round_ = 0;  // Reset election round counter on leader election.
  auto preleader_to_leader_dura = util::DurationToMicros(preleader_timepoint_, util::NowTime());
  printf("[ELECTION-G%d-N%d] BECAME LEADER! Term=%d, last_log_index=%u, "
         "last_log_term=%d. Election duration: %lu us. Recovered %lu entries. "
         "priority_rank=%d/%d election_round=%d\n",
         group_id_, id_, CurrentTerm(), lm_->LastLogEntryIndex(),
         lm_->LastLogEntryTerm(), preleader_to_leader_dura,
         preleader_recover_ent_cnt_,
         GetPriorityRank(), GetClusterServerNumber(), election_round_);
  fflush(stdout);

  resetNextIndexAndMatchIndex();
  broadcastHeartbeat();
  resetHeartbeatTimer();
  resetReplicateTimer();
}

void RaftState::convertToPreLeader() {
  LOG(util::kRaft, "S%d ToPreLeader(T%d) COMMIT I%d LI%d [G%d]", id_, CurrentTerm(), CommitIndex(),
      lm_->LastLogEntryIndex(), group_id_);
  printf("[ELECTION-G%d-N%d] Role changed: Candidate -> PreLeader (term=%d, "
         "commit_index=%u, last_log_index=%u)\n",
         group_id_, id_, CurrentTerm(), CommitIndex(), lm_->LastLogEntryIndex());
  fflush(stdout);
  SetRole(kPreLeader);
  preleader_timepoint_ = util::NowTime();

  printf("[ELECTION-G%d-N%d] [DEBUG] convertToPreLeader: After SetRole(kPreLeader), Role()=%d\n",
         group_id_, id_, Role());
  fflush(stdout);

  // If there is no entry need to be collected, become leader immediately
  if (CommitIndex() == lm_->LastLogEntryIndex()) {
    printf("[ELECTION-G%d-N%d] [DEBUG] No entries to collect, calling convertToLeaderInternal()\n",
           group_id_, id_);
    fflush(stdout);
    convertToLeaderInternal();  // Already holding lock
    return;
  }
  printf("[ELECTION-G%d-N%d] [DEBUG] Need to collect %u fragments\n",
         group_id_, id_, lm_->LastLogEntryIndex() - CommitIndex());
  fflush(stdout);
  collectFragments();
}

void RaftState::resetNextIndexAndMatchIndex() {
  auto next_index = lm_->LastLogEntryIndex() + 1;
  LOG(util::kRaft, "S%d set NI=%d MI=%d", id_, next_index, 0);
  // Since there is no match index yet, the server simply set it to be 0
  for (auto peer_id : peers_) {
    if (peer_id >= 32) {
      fprintf(stderr, "[WARN-G%d-N%d] resetNextIndexAndMatchIndex: peer_id %d out of bounds\n",
              group_id_, id_, peer_id);
      continue;
    }
    auto* peer = raft_peer_[peer_id];
    if (peer == nullptr) {
      fprintf(stderr, "[WARN-G%d-N%d] resetNextIndexAndMatchIndex: raft_peer_[%d] is null\n",
              group_id_, id_, peer_id);
      continue;
    }
    peer->SetNextIndex(next_index);
    peer->SetMatchIndex(0);
    LOG(util::kRaft, "S%d set S%d NI=%d MI=%d", id_, peer_id, next_index, 0);
  }
}

// Returns 0-based priority rank within the group.
// In the new uniform group design:
//   - node_id == group_id: rank = N (highest priority, preferred leader)
//   - others: stable secondary rank 0..N-2 (derived from a hash of group_id and node_id)
// The rank is fixed per RaftState instance and never changes.
int RaftState::GetPriorityRank() const {
  int N = GetClusterServerNumber();
  if (static_cast<int>(id_) == static_cast<int>(group_id_)) {
    return N;  // highest priority: preferred leader for this group
  }
  // Fixed secondary priority: stable hash of (group_id, node_id) mapped to 0..N-2.
  // This is NOT random per call — it is deterministic and fixed for the instance lifetime.
  uint64_t h = (static_cast<uint64_t>(group_id_) << 32) ^
               (static_cast<uint64_t>(id_) << 16) ^ 0x9e3779b9;
  return static_cast<int>(h % N);  // rank 0..N for non-preferred nodes
}

// Returns true if this node is eligible to become a candidate in the current election round.
// New scheme (uniform groups):
//   Round 1: only the node where node_id == group_id is eligible (preferred leader).
//   Round 2+: all nodes eligible (standard Raft with priority order from GetPriorityRank()).
bool RaftState::IsEligibleForElection() const {
  int cluster_size = GetClusterServerNumber();
  int max_priority_rank = cluster_size;
  int my_rank = GetPriorityRank();
  // 初始选举
  if(election_round_ <= 1) return my_rank == max_priority_rank;

  // 当前所处阶段
  int stage = election_round_ / 5;
  int allowed_quarters = stage + 1;
  if(allowed_quarters >= 4) return true; // 已经过了前三个阶段，所有节点都可以成为候选者
  int allowed_count = (max_priority_rank * allowed_quarters) / 4; // 每个阶段允许的候选者数量
  // 安全兜底：无论集群多小，必须保证至少有 1 个最高优先级的节点可以发起选举
  if (allowed_count < 1) {
    allowed_count = 1;
  }
  int threshold = max_priority_rank - allowed_count;
  return my_rank > threshold;

}

// [REQUIRE] Current thread holds the lock of raft state
void RaftState::startElection() {
  assert(Role() == kCandidate);

  printf("[ELECTION-G%d-N%d] >>> STARTING ELECTION <<< "
         "term=%d->%d election_round=%d priority_rank=%d/%d "
         "last_log_index=%u last_log_term=%d peers_count=%zu\n",
         group_id_, id_,
         CurrentTerm(), CurrentTerm() + 1,
         election_round_,
         GetPriorityRank(), GetClusterServerNumber(),
         lm_->LastLogEntryIndex(), lm_->LastLogEntryTerm(),
         peers_.size());
  fflush(stdout);

  current_term_++;
  vote_for_ = id_;
  vote_me_cnt_ = 1;
  printf("[ELECTION-G%d-N%d] [DEBUG] Initialized: vote_me_cnt_=1, livenessLevel()=%d, need=%d\n",
         group_id_, id_, livenessLevel(), livenessLevel() + 1);
  PersistRaftState();

  auto args = RequestVoteArgs{
      CurrentTerm(), id_, lm_->LastLogEntryIndex(),
      lm_->LastLogEntryTerm(), group_id_, GetPriorityRank()};

  // Multi-Raft fix: In single-node clusters (or when we already have quorum via self-vote),
  // peers_ is empty so no RequestVote is sent. Immediately check if we've won.
  if (vote_me_cnt_ >= livenessLevel() + 1) {
    printf("[ELECTION-G%d-N%d] Won election immediately (vote_count=%d >= %d). "
           "Converting to PreLeader.\n",
           group_id_, id_, vote_me_cnt_, livenessLevel() + 1);
    fflush(stdout);
    convertToPreLeader();
    return;
  }

  // 并发发送 RequestVote：收集所有有效 peer
  std::vector<raft_node_id_t> target_peers;
  for (auto id : peers_) {
    if (id == id_) continue;
    if (id < 0 || id >= 32 || rpc_clients_[id] == nullptr) {
      printf("[ELECTION-G%d-N%d] WARNING: Skipping N%d (id=%d, valid=%s, rpc=nullptr)\n",
             group_id_, id_, id, id,
             (id >= 0 && id < 32) ? "yes" : "no");
      continue;
    }
    target_peers.push_back(id);
  }

  if (target_peers.empty()) {
    printf("[ELECTION-G%d-N%d] No peers to send RequestVote\n", group_id_, id_);
    return;
  }

  printf("[ELECTION-G%d-N%d] Sending RequestVote to %zu peers in parallel (async)\n",
         group_id_, id_, target_peers.size());
  fflush(stdout);

  // Use async RPC - returns immediately without blocking.
  // The callback will handle replies asynchronously without holding the election lock.
  for (raft_node_id_t peer_id : target_peers) {
    printf("[ELECTION-G%d-N%d] -> N%d: Sending RequestVote(term=%d, priority=%d) [async]\n",
           group_id_, id_, peer_id, args.term, args.candidate_priority);
    fflush(stdout);
    rpc_clients_[peer_id]->sendAsyncMessage(args);
  }

  printf("[ELECTION-G%d-N%d] All RequestVote RPCs initiated (%zu peers). "
         "About to release lock. vote_me_cnt_=%d role=%d\n",
         group_id_, id_, target_peers.size(), vote_me_cnt_, Role());
  fflush(stdout);
}

void RaftState::broadcastHeartbeat() {
  // NOTE: Heartbeat broadcast logging removed to prevent log flooding.
  // Uncomment for heartbeat debugging:
  // printf("[HEARTBEAT-G%d-N%d] Broadcasting heartbeat(term=%d) to %zu peers\n",
  //        group_id_, id_, CurrentTerm(), peers_.size());
  for (auto id : peers_) {
    if (id != id_) {
      // Safety check: ensure this peer is valid
      if (id < 32 && rpc_clients_[id] != nullptr && raft_peer_[id] != nullptr) {
        sendHeartBeat(id);
      } else {
        fprintf(stderr, "[HEARTBEAT-G%d-N%d] WARNING: Skipping invalid peer %d\n",
                group_id_, id_, id);
      }
    }
  }
}

void RaftState::collectFragments() {
  // Initiate a fragments collection task
  LOG(util::kRaft, "S%d Collect Fragments(I%d->I%d)", id_, CommitIndex() + 1,
      lm_->LastLogEntryIndex());

  // Initiate a request fragments task
  auto recover_start_index = CommitIndex() + 1;
  preleader_stripe_store_.InitRequestFragmentsTask(recover_start_index, lm_->LastLogEntryIndex(),
                                                   peers_.size() + 1, id_);
  preleader_timer_.Reset();
  preleader_recover_ent_cnt_ = lm_->LastLogEntryIndex() + 1 - recover_start_index;

  for (int i = 0; i < preleader_stripe_store_.stripes.size(); ++i) {
    raft_index_t r_idx = i + preleader_stripe_store_.start_index;
    auto ent = lm_->GetSingleLogEntry(r_idx);
    assert(ent != nullptr);

    // NOTE: The stripe meta data is set by current leader, however, that might
    // be invalid since a collected stripe may contain multiple kind of entries
    // i.e. with different k and m parameters
    Stripe &stripe = preleader_stripe_store_.stripes[i];

    // The stripe must filtered fragments that does not match specified index
    // and term
    stripe.raft_index = ent->Index();
    stripe.raft_term = ent->Term();

    if (ent->Type() == kFragments) {
      stripe.collected_fragments.insert_or_assign(static_cast<raft_frag_id_t>(id_), *ent);

      LOG(util::kRaft, "S%d Add FragId%d into Stripe I%d", id_, static_cast<raft_frag_id_t>(id_),
          ent->Index());
    } else if (ent->Type() == kNormal) {
      // No need to collect this entry since leader has full entry
      stripe.collected_fragments.insert_or_assign(static_cast<raft_frag_id_t>(id_), *ent);

      LOG(util::kRaft, "S%d Skip Collecting I%d because of full entry", id_, ent->Index());
    } else {
      // This ent might be a null entry which carries no data
    }
  }
  //
  RequestFragmentsArgs args;
  args.term = CurrentTerm();
  args.leader_id = id_;
  args.start_index = recover_start_index;
  args.last_index = lm_->LastLogEntryIndex();
  //
  for (auto id : peers_) {
    if (id == id_) {
      continue;
    }
    auto rpc = rpc_clients_[id];
    // [COMMENTED] DEBUG: RequestFragments RPC pointer log
    rpc->sendMessage(args);
  }
}

void RaftState::resetElectionTimer() {
  // Each instance has an independent high-quality RNG for election timeout
  // Priority controlled via IsEligibleForElection()
  std::uniform_int_distribution<int64_t> dist(electionTimeLimitMin_, electionTimeLimitMax_ - 1);
  election_time_out_ = dist(rng_);
  election_timer_.Reset();
}

void RaftState::resetHeartbeatTimer() { heartbeat_timer_.Reset(); }
void RaftState::resetPreLeaderTimer() { preleader_timer_.Reset(); }
void RaftState::resetReplicateTimer() {
  replicate_timer_.Reset();
}

void RaftState::Tick() {
  // For kFollower/kCandidate/kPreLeader: hold mtx_ throughout the state transition.
  // For kLeader: release mtx_ before calling tickOnLeader() so that other RaftState
  // instances on the same physical node can still acquire mtx_ to check their election
  // timers while this instance is sending heartbeat RPCs (which may block).
  RaftRole r = Role();
  switch (r) {
    case kFollower:
    case kCandidate: {
      std::scoped_lock<std::mutex> lck(mtx_);
      if (r == kFollower) {
        tickOnFollower();
      } else {
        tickOnCandidate();
      }
      return;
    }
    case kPreLeader: {
      std::scoped_lock<std::mutex> lck(mtx_);
      tickOnPreLeader();
      return;
    }
    case kLeader:
      // tickOnLeader() manages its own lock internally (read state under lock,
      // release, then send RPCs without holding mtx_)
      tickOnLeader();
      return;
    default:
      assert(0);
  }
}

void RaftState::tickOnFollower() {
  if (election_timer_.ElapseMilliseconds() < election_time_out_) {
    return;
  }
  // NOTE: Election timer expiration logging removed to prevent log flooding.
  // Keep important logs: role changes, election outcomes, vote decisions.
  if (!IsEligibleForElection()) {
    // Silently skip: not eligible this round
    return;
  }
  convertToCandidate();
}

void RaftState::tickOnCandidate() {
  if (election_timer_.ElapseMilliseconds() < election_time_out_) {
    return;
  }
  // NOTE: Candidate timer expiration logging reduced to avoid log flooding.
  // Only log every ~100s (every 100th invocation) unless state changes.
  static std::atomic<uint64_t> s_cand{0};
  bool do_clog = (s_cand.fetch_add(1, std::memory_order_relaxed) % 100 == 0);
  if (do_clog || !IsEligibleForElection()) {
    if (!IsEligibleForElection()) {
      // Log only when becoming ineligible (important state change)
      printf("[ELECTION-G%d-N%d] Not eligible for election. "
             "Converting to follower. election_round++ -> %d\n",
             group_id_, id_, election_round_ + 1);
    } else {
      // [COMMENTED] Periodic candidate retry log (every ~100 invocations)
    }
  }
  if (!IsEligibleForElection()) {
    convertToFollower(CurrentTerm());
    election_round_++;
    resetElectionTimer();
    return;
  }
  resetElectionTimer();
  startElection();
}

void RaftState::tickOnLeader() {
  bool should_send_heartbeat = false;
  bool should_replicate = false;
  {
    std::scoped_lock<std::mutex> lck(mtx_);
    // Re-check Role() under lock to avoid TOCTOU: role might have changed
    // between the atomic read in Tick() and lock acquisition
    if (Role() != kLeader) {
      return;
    }
    should_send_heartbeat = (heartbeat_timer_.ElapseMilliseconds() >= heartbeatTimeInterval);
    should_replicate = (replicate_timer_.ElapseMilliseconds() >= config::kReplicateInterval);
  }
  // NOTE: mtx_ is NOT held during heartbeat/replication sends.
  // This is critical for Multi-Raft: when this RaftState sends heartbeat RPCs
  // (which may block for 10-100ms), other RaftState instances on the same
  // physical node can still acquire mtx_ to check/reset their election timers.
  if (should_send_heartbeat) {
    broadcastHeartbeat();
    resetHeartbeatTimer();
  }
  if (should_replicate) {
    ReplicateEntries();
    resetReplicateTimer();
  }
}

void RaftState::PersistRaftState() {
  if (storage_ != nullptr) {
    storage_->PersistState(Storage::PersistRaftState{true, CurrentTerm(), VoteFor()});
  }
}

void RaftState::tickOnPreLeader() {
  // Check if we can become leader immediately (collected enough fragments).
  // This handles both the single-node case and normal multi-node recovery.
  PreLeaderBecomeLeader();

  if (preleader_timer_.ElapseMilliseconds() < config::kCollectFragmentsInterval) {
    return;
  }
  collectFragments();
  resetPreLeaderTimer();
}

void RaftState::EncodeRaftEntry(raft_index_t raft_index, raft_encoding_param_t k,
                                raft_encoding_param_t m, Stripe *stripe) {
  assert(raft_index <= lm_->LastLogEntryIndex());
  assert(k > 0 && "k must be > 0 for encoding");
  auto ent = lm_->GetSingleLogEntry(raft_index);
  assert(ent != nullptr);

  stripe->raft_index = ent->Index();
  stripe->raft_term = ent->Term();
  stripe->fragments.clear();

  LOG(util::kRaft, "S%d Encode I%d T%d K%d M%d", id_, raft_index, stripe->raft_term, k, m);
  Encoder::EncodingResults results;
  auto data_to_encode = ent->CommandData().data() + ent->StartOffset();
  auto datasize_to_encode = ent->CommandData().size() - ent->StartOffset();
  Slice encode_slice = Slice(data_to_encode, datasize_to_encode);
  encoder_.EncodeSlice(encode_slice, k, m, &results);

  for (const auto &[frag_id, frag] : results) {
    LogEntry encoded_ent;
    encoded_ent.SetIndex(raft_index);
    encoded_ent.SetTerm(stripe->raft_term);
    encoded_ent.SetType(kFragments);
    encoded_ent.SetChunkInfo(ChunkInfo{k, raft_index});
    encoded_ent.SetStartOffset(ent->StartOffset());

    encoded_ent.SetCommandLength(ent->CommandLength());
    encoded_ent.SetNotEncodedSlice(Slice::Copy(
        Slice(ent->CommandData().data(), ent->StartOffset())));
    // CRITICAL: Copy the frag Slice to owned memory immediately.
    // The frag pointer references Encoder's internal buffer (padded_data_ or
    // encode_output_), which will be invalidated on the next EncodeSlice call.
    // Using Slice::Copy() ensures the LogEntry owns its data independently.
    encoded_ent.SetFragmentSlice(Slice::Copy(frag));
    stripe->fragments[frag_id] = encoded_ent;
  }
}

// ============================================================================
// EncodeRaftEntryLrc — LRC encoding with latency-aware orthogonal placement
//
// This function encodes a log entry using LRC instead of RS encoding,
// and uses the latency-aware complementary grouper to determine which
// physical node receives which fragment.
//
// Parameters:
//   raft_index: The log entry index
//   lrc_params: LRC parameters (k, l, r)
//   stripe: Output stripe to store encoded fragments
//
// Returns: The LRC group ID assigned to this stripe
// ============================================================================
int RaftState::EncodeRaftEntryLrc(raft_index_t raft_index,
                                   const multiraft::LrcParams& lrc_params,
                                   Stripe* stripe) {
  assert(raft_index <= lm_->LastLogEntryIndex());
  auto ent = lm_->GetSingleLogEntry(raft_index);
  assert(ent != nullptr);

  stripe->raft_index = ent->Index();
  stripe->raft_term = ent->Term();
  stripe->fragments.clear();

  // Calculate LRC group ID based on stripe index (round-robin)
  int lrc_group_id = raft_index % lrc_params.l;
  stripe->lrc_group_id = lrc_group_id;

  LOG(util::kRaft, "S%d EncodeRaftEntryLrc I%d T%d %s lrc_group=%d",
      id_, raft_index, stripe->raft_term, lrc_params.ToString().c_str(), lrc_group_id);

  // Use LrcEncoder for LRC encoding
  multiraft::LrcEncoder lrc_enc(lrc_params);

  // Get data to encode
  auto data_to_encode = ent->CommandData().data() + ent->StartOffset();
  auto datasize_to_encode = ent->CommandData().size() - ent->StartOffset();
  Slice encode_slice = Slice(data_to_encode, datasize_to_encode);

  // Perform LRC encoding
  multiraft::LrcStripe lrc_stripe;
  if (!lrc_enc.EncodeStripe(encode_slice, &lrc_stripe)) {
    LOG(util::kRaft, "S%d EncodeRaftEntryLrc FAILED for I%d", id_, raft_index);
    return -1;
  }

  // Build fragment map based on placements
  // The fragments map is keyed by frag_id, not node_id
  int frag_idx = 0;

  // Data fragments [0, k)
  for (int i = 0; i < lrc_params.k && frag_idx < static_cast<int>(lrc_stripe.data_shards.size()); ++i) {
    const raft::Slice& frag = lrc_stripe.data_shards[i];

    LogEntry encoded_ent;
    encoded_ent.SetIndex(raft_index);
    encoded_ent.SetTerm(stripe->raft_term);
    encoded_ent.SetType(kFragments);
    encoded_ent.SetChunkInfo(ChunkInfo{static_cast<raft_encoding_param_t>(lrc_params.k), raft_index});
    encoded_ent.SetStartOffset(ent->StartOffset());
    encoded_ent.SetCommandLength(ent->CommandLength());
    encoded_ent.SetNotEncodedSlice(Slice::Copy(
        Slice(ent->CommandData().data(), ent->StartOffset())));
    // Copy fragment to owned memory before lrc_stripe is freed
    encoded_ent.SetFragmentSlice(Slice::Copy(frag));

    // Use frag_id as key (for orthogonal placement, we need to send specific frags to specific nodes)
    stripe->fragments[frag_idx] = encoded_ent;
    frag_idx++;
  }

  // Local parity fragments [k, k+l)
  for (int i = 0; i < lrc_params.l; ++i) {
    const raft::Slice& frag = lrc_stripe.local_parities[i];

    LogEntry encoded_ent;
    encoded_ent.SetIndex(raft_index);
    encoded_ent.SetTerm(stripe->raft_term);
    encoded_ent.SetType(kFragments);
    encoded_ent.SetChunkInfo(ChunkInfo{static_cast<raft_encoding_param_t>(lrc_params.k), raft_index});
    encoded_ent.SetStartOffset(ent->StartOffset());
    encoded_ent.SetCommandLength(ent->CommandLength());
    encoded_ent.SetNotEncodedSlice(Slice::Copy(
        Slice(ent->CommandData().data(), ent->StartOffset())));
    // Copy fragment to owned memory before lrc_stripe is freed
    encoded_ent.SetFragmentSlice(Slice::Copy(frag));

    stripe->fragments[frag_idx] = encoded_ent;
    frag_idx++;
  }

  // Global parity fragments [k+l, k+l+r)
  for (int i = 0; i < lrc_params.r; ++i) {
    const raft::Slice& frag = lrc_stripe.global_parities[i];

    LogEntry encoded_ent;
    encoded_ent.SetIndex(raft_index);
    encoded_ent.SetTerm(stripe->raft_term);
    encoded_ent.SetType(kFragments);
    encoded_ent.SetChunkInfo(ChunkInfo{static_cast<raft_encoding_param_t>(lrc_params.k), raft_index});
    encoded_ent.SetStartOffset(ent->StartOffset());
    encoded_ent.SetCommandLength(ent->CommandLength());
    encoded_ent.SetNotEncodedSlice(Slice::Copy(
        Slice(ent->CommandData().data(), ent->StartOffset())));
    // Copy fragment to owned memory before lrc_stripe is freed
    encoded_ent.SetFragmentSlice(Slice::Copy(frag));

    stripe->fragments[frag_idx] = encoded_ent;
    frag_idx++;
  }

  // Store all fragments in Stripe.all_fragments for peer-keyed packing
  // Format: [0,k) data, [k,k+l) local parity, [k+l,k+l+r) global parity
  stripe->all_fragments.clear();
  stripe->all_fragments.reserve(static_cast<size_t>(lrc_params.k + lrc_params.l + lrc_params.r));

  // Data fragments [0, k) — copy to owned memory before lrc_stripe is freed
  for (int i = 0; i < lrc_params.k && i < static_cast<int>(lrc_stripe.data_shards.size()); ++i) {
    stripe->all_fragments.push_back(Slice::Copy(lrc_stripe.data_shards[i]));
  }
  // Local parity fragments [k, k+l)
  for (int i = 0; i < lrc_params.l && i < static_cast<int>(lrc_stripe.local_parities.size()); ++i) {
    stripe->all_fragments.push_back(Slice::Copy(lrc_stripe.local_parities[i]));
  }
  // Global parity fragments [k+l, k+l+r)
  for (int i = 0; i < lrc_params.r && i < static_cast<int>(lrc_stripe.global_parities.size()); ++i) {
    stripe->all_fragments.push_back(Slice::Copy(lrc_stripe.global_parities[i]));
  }

  LOG(util::kRaft, "S%d EncodeRaftEntryLrc I%d done: %d fragments stored in all_fragments",
      id_, raft_index, static_cast<int>(stripe->all_fragments.size()));

  // Clean up LRC stripe memory (fragments are now owned by stripe via Slice::Copy)
  lrc_stripe.FreeMemory();

  return lrc_group_id;
}

// ============================================================================
// Inline packing helper for PackStripesToPeerKeyed
// ============================================================================
namespace {
// Pack fragments into a simple binary format:
// [4-byte magic: 'MFRG']
// [4-byte meta_len]
// [meta bytes]
// [4-byte frag_count]
// For each frag: [4-byte frag_id] [4-byte data_len] [data bytes]
void AppendU32ToString(std::string* out, uint32_t v) {
  out->append(reinterpret_cast<const char*>(&v), 4);
}

// Extract user_key from StripeWriteCommand payload (without full deserialization)
// Format: [4-byte magic] [4-byte group_id] [1-byte mode] [4-byte key_len] [key bytes] ...
// Returns empty string if extraction fails
std::string ExtractUserKeyFromStripeCommand(const char* data, size_t len) {
  if (len < 13) return "";  // min: magic(4) + gid(4) + mode(1) + key_len(4)
  if (std::memcmp(data, "STRI", 4) != 0) return "";
  const char* p = data + 4 + 4 + 1;  // skip magic + group_id + mode
  uint32_t key_len = 0;
  std::memcpy(&key_len, p, 4);
  p += 4;
  if (key_len > 1024 || key_len > len - 13) return "";  // sanity check
  return std::string(p, key_len);
}

std::string PackFragmentsInline(const std::vector<int>& frag_ids,
                                const std::vector<raft::Slice>& all_frags,
                                int k, int l, int r,
                                raft::raft_index_t stripe_id,
                                const std::string& user_key) {
  std::string out;
  out.append("MFRG", 4);  // magic

  // Build meta string inline (compatible with StripeLogMeta::Deserialize)
  std::string meta;
  AppendU32ToString(&meta, static_cast<uint32_t>(user_key.size()));  // user_key size
  meta.append(user_key);  // user_key
  AppendU32ToString(&meta, static_cast<uint32_t>(stripe_id));  // stripe_id
  AppendU32ToString(&meta, static_cast<uint32_t>(stripe_id));  // entry_id
  AppendU32ToString(&meta, 0);  // group_id (will be set by ApplyStripePacked)
  AppendU32ToString(&meta, 0);  // original_size
  meta.push_back(static_cast<char>(2));  // encoding_mode = kLrc
  AppendU32ToString(&meta, static_cast<uint32_t>(k));
  AppendU32ToString(&meta, static_cast<uint32_t>(l));
  AppendU32ToString(&meta, static_cast<uint32_t>(r));
  AppendU32ToString(&meta, 0);  // m
  AppendU32ToString(&meta, 0);  // placement count

  // Meta length
  AppendU32ToString(&out, static_cast<uint32_t>(meta.size()));
  out.append(meta);

  // Fragments
  AppendU32ToString(&out, static_cast<uint32_t>(frag_ids.size()));
  for (int frag_id : frag_ids) {
    if (frag_id >= 0 && frag_id < static_cast<int>(all_frags.size())) {
      const raft::Slice& frag = all_frags[frag_id];
      AppendU32ToString(&out, static_cast<uint32_t>(frag_id));
      AppendU32ToString(&out, static_cast<uint32_t>(frag.size()));
      out.append(frag.data(), frag.size());
    }
  }

  return out;
}
}  // namespace

// ============================================================================
// PackStripesToPeerKeyed — Pack LRC fragments using lrc_grouper placement rules
//
// Uses LrcComplementaryGrouper::GetFragmentsForNode() to determine which
// fragments each peer should receive, then packs them into peer-keyed LogEntry.
//
// Input:  stripe->all_fragments contains k+l+r fragments (by frag_id order)
// Output: stripe->fragments is keyed by peer_id (node_id), each entry contains
//         packed fragments for that peer
// ============================================================================
void RaftState::PackStripesToPeerKeyed(raft_index_t raft_index, Stripe* stripe, const std::string& user_key) {
  if (stripe->peer_keyed_fragments) {
    return;  // Already packed
  }

  if (stripe->all_fragments.empty()) {
    LOG(util::kRaft, "S%d PackStripesToPeerKeyed I%d: all_fragments empty, skip", id_, raft_index);
    return;
  }

  auto* grouper = static_cast<multiraft::LrcComplementaryGrouper*>(lrc_grouper_);
  if (!grouper) {
    LOG(util::kRaft, "S%d PackStripesToPeerKeyed I%d: no lrc_grouper, skip", id_, raft_index);
    return;
  }

  const auto& lrc_params = grouper->GetLrcParams();
  int N = GetClusterServerNumber();

  // Clear fragments map, will rebuild with peer_id as key
  stripe->fragments.clear();

  // Iterate all nodes to pack their assigned fragments
  for (int node_id = 0; node_id < N; ++node_id) {
    const auto& frag_ids = grouper->GetFragmentsForNode(node_id);
    if (frag_ids.empty()) {
      continue;
    }

    // Collect fragments for this node (store as raw struct to avoid linking multi-raft)
    struct LocalFrag {
      int frag_id;
      std::string bytes;
    };
    std::vector<LocalFrag> local_frags;
    for (int frag_id : frag_ids) {
      if (frag_id >= 0 && frag_id < static_cast<int>(stripe->all_fragments.size())) {
        const raft::Slice& frag = stripe->all_fragments[frag_id];
        LocalFrag lf;
        lf.frag_id = frag_id;
        lf.bytes.assign(frag.data(), frag.size());
        local_frags.push_back(std::move(lf));
      }
    }

    if (local_frags.empty()) {
      continue;
    }

    // Pack fragments inline (avoid linking multi-raft library)
    std::string packed = PackFragmentsInline(frag_ids, stripe->all_fragments,
                                            lrc_params.k, lrc_params.l, lrc_params.r,
                                            stripe->raft_index, user_key);

    // Create LogEntry
    raft::LogEntry encoded_ent;
    encoded_ent.SetIndex(raft_index);
    encoded_ent.SetTerm(stripe->raft_term);
    encoded_ent.SetType(kFragments);
    encoded_ent.SetChunkInfo(ChunkInfo{static_cast<raft_encoding_param_t>(lrc_params.k), raft_index});
    encoded_ent.SetStartOffset(0);
    encoded_ent.SetCommandLength(0);

    // Transfer ownership of packed data
    char* pack_copy = new char[packed.size()];
    std::memcpy(pack_copy, packed.data(), packed.size());
    encoded_ent.SetFragmentSlice(raft::Slice(pack_copy, packed.size()));

    // Key by peer_id (node_id)
    stripe->fragments[static_cast<raft_frag_id_t>(node_id)] = encoded_ent;
  }

  stripe->peer_keyed_fragments = true;
}

bool RaftState::DecodingRaftEntry(Stripe *stripe, LogEntry *ent) {
  // Debug:
  // ------------------------------------------------------------------
  LOG(util::kRaft, "S%d Decode Stripe I%d", id_, stripe->raft_index);
  // ------------------------------------------------------------------
  //
  // A corner case: Check if there already exists an full entry, maybe newly
  // elected leader is the old leader.
  if (FindFullEntryInStripe(stripe, ent)) {
    return true;
  }

  auto id = static_cast<raft_frag_id_t>(id_);

  auto k = stripe->collected_fragments[id].GetChunkInfo().GetK();
  auto m = GetClusterServerNumber() - k;

  LOG(util::kRaft, "S%d Decode Entry K=%d M=%d, Collect Size=%d", id_, k, m,
      stripe->collected_fragments.size());

  int not_encoded_size = stripe->collected_fragments[id].StartOffset();
  int complete_ent_size =
      not_encoded_size + (stripe->collected_fragments[id].FragmentSlice().size() * k);

  LOG(util::kRaft, "S%d Estimate NotEncodeSize=%d CompleteSize=%d", id_, not_encoded_size,
      complete_ent_size);

  auto data = new char[complete_ent_size + 16];
  std::memcpy(data, stripe->collected_fragments[id].NotEncodedSlice().data(),
              stripe->collected_fragments[id].NotEncodedSlice().size());

  Encoder::EncodingResults input;
  // for (const auto &ent : stripe->collected_fragments) {
  // input.insert({ent.GetVersion().GetFragmentId(), ent.FragmentSlice()});
  // LOG(util::kRaft, "S%d Decode: Add Input(FragId%d)",
  // ent.GetVersion().GetFragmentId());
  // }

  for (const auto &[chunk_id, ent] : stripe->collected_fragments) {
    input.insert_or_assign(chunk_id, ent.FragmentSlice());
    LOG(util::kRaft, "S%d Decode: Add Input(FragId%d)", id_, chunk_id);
  }

  int decode_size = 0;
  // Specify that decoding data should be written to the data pointer that at
  // not_encoded_size place
  if (!encoder_.DecodeSliceHelper(input, k, m, data + not_encoded_size, &decode_size)) {
    delete[] data;
    return false;
  }

  int origin_size = stripe->collected_fragments[id].CommandLength();

  ent->SetIndex(stripe->raft_index);
  ent->SetTerm(stripe->raft_term);
  ent->SetType(kNormal);
  ent->SetCommandData(Slice(data, origin_size));
  ent->SetStartOffset(not_encoded_size);
  ent->SetChunkInfo(ChunkInfo{0, ent->Index()});
  LOG(util::kRaft, "S%d Decode Results: Ent(%s)", id_, ent->ToString().c_str());

  return true;
}

void RaftState::ReplicateNewProposeEntry(raft_index_t raft_index) {
  fprintf(stderr, "[DEBUG-RAFT] ReplicateNewProposeEntry called: group=%d id=%d idx=%d encoding_mode=%d\n",
          group_id_, id_, raft_index, encoding_mode_);
  fflush(stderr);
  LOG(util::kRaft, "S%d REPLICATE NEW ENTRY I%d", id_, raft_index);

  auto* ent = lm_->GetSingleLogEntry(raft_index);
  if (!ent) {
    LOG(util::kRaft, "S%d ERROR: Entry I%d not found", id_, raft_index);
    return;
  }

  // Check if we should use LRC encoding with orthogonal placement
  if (encoding_mode_ == 2 && HasLrcGrouper() && ent->IsLrcEncoded()) {
    // LRC mode: generate peer-specific LogEntry based on placement
    auto* grouper = static_cast<multiraft::LrcComplementaryGrouper*>(lrc_grouper_);

    // Get placement from LogEntry
    const auto& placement = ent->Placement();
    // 打印 ent->Placement()，验证实际存储的 placement
    printf("[LRC-PLACEMENT-G%d-N%d] I%d: ent->Placement() (%zu entries):\n",
          group_id_, id_, raft_index, placement.size());   
    for (const auto& fp : placement) {      
        printf("  %s\n", fp.ToString().c_str());   
    }  
    fflush(stdout);
    // Get all fragments directly from LogEntry
    // Note: frag_id = position in vector (0, 1, 2, ..., k+l+r-1)
    // Fragments are deep-copied via AddFragment in Propose, so they are safe to access
    const auto& all_frags = ent->Fragments();

    // 日志打印，debug
    // This ensures consistency with BuildNodeToFragmentsMapping() algorithm
    printf("[LRC-PLACEMENT-G%d-N%d] I%d: grouper assignment (from BuildNodeToFragmentsMapping):\n",
           group_id_, id_, raft_index);
    for (int n = 0; n < grouper->GetN(); ++n) {
      const auto& frag_ids = grouper->GetFragmentsForNode(n);
      printf("  Node %d: %zu frags", n, frag_ids.size());
      for (int fid : frag_ids) {
        printf(" %d", fid);
      }
      printf("\n");
    }
    fflush(stdout);

    // [FIX] Update last_encoding_ BEFORE the loop so prev_k can be correctly read
    UpdateLastEncodingK(raft_index, ent->GetChunkInfo().GetK());

    // Send to each peer with their assigned fragments
    for (auto peer : peers_) {
      if (peer == id_) continue;  // Skip self

      // FIX: Use grouper's GetFragmentsForNode() directly
      const auto& frag_ids = grouper->GetFragmentsForNode(peer);
      
      printf("[LRC-REPL-G%d-N%d] Peer %d assigned %zu frags via grouper:",
             group_id_, id_, peer, frag_ids.size());
      for (int fid : frag_ids) {
        printf("  frag_id=%d\t", fid);
      }
      printf("\n");
      fflush(stdout);
      
      if (frag_ids.empty()) {
        printf("[LRC-REPL-G%d-N%d] WARNING: No frags for peer %d at I%d\n",
               group_id_, id_, peer, raft_index);
        fflush(stdout);
        continue;
      }
      
      // Build peer-specific fragments from all_frags using frag_ids
      // Sort frag_ids to ensure index = frag_id (required for correct serialization)
      std::vector<int> sorted_frag_ids = frag_ids;
      std::sort(sorted_frag_ids.begin(), sorted_frag_ids.end());

      std::vector<Slice> peer_frags;
      for (int fid : sorted_frag_ids) {
        if (fid >= 0 && fid < static_cast<int>(all_frags.size())) {
          peer_frags.push_back(all_frags[fid]);
        } else {
          printf("[LRC-REPL-G%d-N%d] ERROR: frag_id=%d out of range (all_frags size=%zu)\n",
                 group_id_, id_, fid, all_frags.size());
        }
      }

      // Check if peer_frags is empty
      if (peer_frags.empty()) {
        printf("[LRC-REPL-G%d-N%d] WARNING: No fragments for peer %d at I%d\n",
               group_id_, id_, peer, raft_index);
        fflush(stdout);
        continue;
      }

      // Create peer-specific LogEntry with proper fragments
      LogEntry peer_entry;
      peer_entry.SetIndex(raft_index);
      peer_entry.SetTerm(ent->Term());
      peer_entry.SetType(kNormal);
      peer_entry.SetChunkInfo(ent->GetChunkInfo());

      // === KEY-ONLY 复制：只传 key 部分，不传 value ===
      // STRI header layout: [magic:4][group_id:8][mode:1][key_len:8][key:N]
      // key 起始于偏移 21, 长度 = ChunkInfo.key_size
      Slice cmd_data = ent->CommandData();
      uint32_t key_size = ent->GetChunkInfo().GetKeySize();
      if (key_size > 0 && (21 + key_size) <= cmd_data.size()) {
        Slice key_slice(cmd_data.data() + 21, key_size);
        peer_entry.SetCommandData(key_slice);
      } else {
        // Fallback: empty command_data (do not pass key)
        peer_entry.SetCommandData(Slice());
      }
      // 恢复原始 CommandLength（SetCommandData 会设置 command_size_，需要覆盖）
      peer_entry.SetCommandLength(ent->GetChunkInfo().GetTotalSize());

      // Add all placement to every node
      peer_entry.SetPlacement(ent->Placement());

      // Add fragments for this peer (already sorted by frag_id, so index = frag_id)
      printf("[LRC-REPL-G%d-N%d] DEBUG: Adding %zu fragments with ids:", group_id_, id_, peer_frags.size());
      for (size_t i = 0; i < peer_frags.size(); ++i) {
        printf(" %d", sorted_frag_ids[i]);
      }
      printf("\n");
      fflush(stdout);
      for (size_t i = 0; i < peer_frags.size(); ++i) {
        peer_entry.AddFragmentWithId(sorted_frag_ids[i], peer_frags[i]);
      }

      printf("[LRC-REPL-G%d-N%d] Sending %zu frags to peer %d at I%d (k=%d,l=%d,r=%d)\n",
             group_id_, id_, peer_frags.size(), peer, raft_index,
             ent->GetChunkInfo().GetK(), ent->GetChunkInfo().GetL(), ent->GetChunkInfo().GetR());
      fflush(stdout);

      auto prev_index = raft_index - 1;
      auto prev_term = (prev_index > 0) ?
          lm_->GetSingleLogEntry(prev_index)->Term() : 0;
      auto prev_k = GetLastEncodingK(prev_index);

      auto args = AppendEntriesArgs{CurrentTerm(), group_id_, id_, prev_index, prev_term,
                                    prev_k, CommitIndex()};
      assert(args.group_id == group_id_);

      args.entries.push_back(peer_entry);
      args.entry_cnt = 1;

      // Log the AE message being sent (using printf for node0log)
      printf("[LRC-REPL-G%d-N%d] AE To S%d: prev_idx=%d prev_term=%d prev_k=%d commit=%d\n",
             group_id_, id_, peer, args.prev_log_index, args.prev_log_term, args.prev_k, args.leader_commit);
      fflush(stdout);

      rpc_clients_[peer]->sendAsyncMessage(args);
    }
  } else {
    // Non-LRC mode: use original sendAppendEntries
    auto live_servers = live_monitor_.LiveNumber();

    // Check if re-encoding is needed and encode the new entry
    if (live_servers != AliveServersOfLastPoint()) {
      LOG(util::kRaft, "S%d Live server %d, LastPoint %d, reencoding", id_,
          live_servers, AliveServersOfLastPoint());
      UpdateAliveServers(live_servers);
      MaybeReEncodingAndReplicate();
    }

    // Encode the new entry if not already encoded
    if (encoded_stripe_.count(raft_index) == 0) {
      auto encode_k = (dynamic_k_ > 0) ? dynamic_k_ : live_servers - livenessLevel();
      auto encode_m = GetClusterServerNumber() - encode_k;

      auto* stripe = new Stripe();
      EncodeRaftEntry(raft_index, encode_k, encode_m, stripe);
      encoded_stripe_.insert_or_assign(raft_index, stripe);
      UpdateLastEncodingK(raft_index, encode_k);
    }

    // Now send AppendEntries
    for (auto peer_id : peers_) {
      if (peer_id != id_) {
        if (live_monitor_.IsAlive(peer_id)) {
          sendAppendEntries(peer_id);
        } else {
          sendHeartBeat(peer_id);
        }
      }
    }
  }

  // Reset replication timer
  resetReplicateTimer();
}

void RaftState::ReplicateEntries() {
  LOG(util::kRaft, "S%d ReplicateEntries()", id_);
  auto live_servers = live_monitor_.LiveNumber();
  LOG(util::kRaft, "S%d Estimate Now Live Servers=%d, Last Point=%d", id_, live_servers,
      AliveServersOfLastPoint());
  if (live_servers != AliveServersOfLastPoint()) {
    UpdateAliveServers(live_servers);
    MaybeReEncodingAndReplicate();
  } else {
    // Otherwise send these entries as original raft does
  for (auto peer_id : peers_) {
    if (peer_id != id_) {
      if (live_monitor_.IsAlive(peer_id)) {
        sendAppendEntries(peer_id);
      } else {
        sendHeartBeat(peer_id);
        }
      }
    }
  }
}

void RaftState::MaybeReEncodingAndReplicate() {
  LOG(util::kRaft, "S%d MAY REENCODE ENTRIES", id_);

  auto live_servers = live_monitor_.LiveNumber();

  // Guard: cannot encode if not enough live servers (need > livenessLevel for valid k)
  if (live_servers <= livenessLevel()) {
    LOG(util::kRaft, "S%d Not enough live servers (%d <= %d) for encoding, skipping",
        id_, live_servers, livenessLevel());
    return;
  }

  // Dynamic encoding: if dynamic_k_ > 0, use it as fixed k for experiments.
  // Otherwise, compute k from live server count (auto mode).
  raft_encoding_param_t encode_k;
  if (dynamic_k_ > 0) {
    encode_k = dynamic_k_;
  } else {
    encode_k = live_servers - livenessLevel();
  }
  raft_encoding_param_t encode_m = GetClusterServerNumber() - encode_k;
  LOG(util::kRaft, "S%d Estimate %d Server Alive K:%d M:%d [dynamic_k=%d G%d]",
      id_, live_servers, encode_k, encode_m, dynamic_k_, group_id_);

  // Step1: ReEncoding all necessary entries from CommitIndex() + 1 to
  // LastIndex()
  auto last_index = lm_->LastLogEntryIndex();
  for (auto raft_index = CommitIndex() + 1; raft_index <= last_index; ++raft_index) {
    // last_k = 0 means this entry has not been encoded yet
    auto last_k = GetLastEncodingK(raft_index);
    if (last_k != 0 && encode_k >= last_k) {
      continue;
    }
    // A smaller k, needs re-encoding the entry
    auto stripe = new Stripe();
    EncodeRaftEntry(raft_index, encode_k, encode_m, stripe);
    encoded_stripe_.insert_or_assign(raft_index, stripe);
    UpdateLastEncodingK(raft_index, encode_k);
    LOG(util::kRaft, "S%d Encode Entry I%d with (K%d, M%d)", id_, raft_index, encode_k, encode_m);
  }

  // Step2: replicate all entries by reversing NextIndex and MatchIndex
  for (auto peer_id : peers_) {
    if (peer_id != id_) {
      auto peer = raft_peer_[peer_id];
      peer->SetNextIndex(CommitIndex() + 1);
      peer->SetMatchIndex(CommitIndex());
      LOG(util::kRaft, "S%d REVERSE S%d NI To %d", id_, peer_id, CommitIndex() + 1);
      if (live_monitor_.IsAlive(peer_id)) {
        sendAppendEntries(peer_id);
      } else {
        sendHeartBeat(peer_id);
      }
    }
  }
}

// TODO: Update logic
// void RaftState::replicateEntries() {
//   LOG(util::kRaft, "S%d REPLICATE ENTRIES", id_);
//   auto live_servers = live_monitor_.LiveNumber();
//
//   int encode_k = live_servers - livenessLevel();
//   int encode_m = livenessLevel();
//   auto version_num = VersionNumber{CurrentTerm(), NextSequence()};
//
//   LOG(util::kRaft, "S%d Estimate %d Server Alive K:%d M:%d VERSION NUM:%s",
//   id_,
//       live_servers, encode_k, encode_m, version_num.ToString().c_str());
//
//   // Step1: Encoding all necessary entries from CommitIndex() to LastIndex()
//   auto last_index = lm_->LastLogEntryIndex();
//   for (auto raft_index = CommitIndex() + 1; raft_index <= last_index;
//        ++raft_index) {
//     bool need_encoding = false;
//     if (encoded_stripe_.count(raft_index) == 0) {
//       need_encoding = true;
//     } else {
//       auto encoded_version = encoded_stripe_.at(raft_index)->version;
//       if (encoded_version.GetK() != encode_k ||
//           encoded_version.GetM() != encode_m) {
//         need_encoding = true;
//       }
//     }
//     if (need_encoding) {
//       auto stripe = new Stripe();
// #ifdef ENABLE_PERF_RECORDING
//       util::EncodingEntryPerfCounter perf_counter(encode_k, encode_m);
// #endif
//       EncodingRaftEntry(raft_index, encode_k, encode_m, version_num, stripe);
// #ifdef ENABLE_PERF_RECORDING
//       perf_counter.Record();
//       PERF_LOG(&perf_counter);
// #endif
//       encoded_stripe_.insert_or_assign(raft_index, stripe);
//     } else {
//       // Simply update the encoding version
//       auto stripe = encoded_stripe_[raft_index];
//       assert(stripe->version.GetK() == encode_k);
//       assert(stripe->version.GetM() == encode_m);
//     }
//
//     auto new_version = Version{version_num, encode_k, encode_m};
//     auto stripe = encoded_stripe_[raft_index];
//     stripe->version = new_version;
//
//     // Update each fragments version number
//     for (auto &[id, frag] : stripe->fragments) {
//       auto frag_version = new_version;
//       frag_version.SetFragmentId(id);
//       frag.SetVersion(frag_version);
//     }
//
//     // Update Commit Requirements
//     last_replicate_.insert_or_assign(raft_index, new_version);
//   }
//
//   // Step2: construct a map to decide which fragment will each follower
//   receive std::map<raft_node_id_t, raft_frag_id_t> frag_map; int
//   start_frag_id = 0; for (const auto &[id, _] : peers_) {
//     if (live_monitor_.IsAlive(id)) {
//       frag_map[id] = start_frag_id++;
//     }
//   }
//
//   // Step3: For each follower, send out the messages
//   for (const auto &[id, _] : peers_) {
//     if (id == id_) {
//       continue;
//     }
//     if (!live_monitor_.IsAlive(id)) {
//       LOG(util::kRaft, "S%d detect S%d is not alive, send heartbeat", id_,
//       id); sendHeartBeat(id); continue;
//     }
//
//     // Otherwise send true messages
//     // Construct an AE args
//     AppendEntriesArgs args;
//     args.term = CurrentTerm();
//     args.leader_id = id_;
//     args.leader_commit = CommitIndex();
//
//     auto next_index = peers_[id]->NextIndex();
//
//     if (next_index < CommitIndex() + 1) {
//       // Fill in with leader's entries to replenish entries that this
//       follower
//       // lacks
//       for (auto idx = next_index; idx <= CommitIndex(); ++idx) {
//         args.entries.push_back(*lm_->GetSingleLogEntry(idx));
//       }
//       // For those followers whoes required entries fall behind the
//       // CommitIndex(), the leader simply sends its local data. Most notably,
//       // send an empty entry to such follower does not affect the safety
//       LOG(util::kRaft, "S%d Replenish ent I(%d->%d) To S%d", id_, next_index,
//           CommitIndex(), id);
//       args.prev_log_index = next_index - 1;
//       args.prev_log_term = lm_->TermAt(args.prev_log_index);
//     } else {
//       args.prev_log_index = CommitIndex();
//       args.prev_log_term = lm_->TermAt(args.prev_log_index);
//     }
//
//     // Start send entries within the range [CommitIndex()+1, LastIndex()]
//     auto send_index = CommitIndex() + 1;
//     for (; send_index <= lm_->LastLogEntryIndex(); ++send_index) {
//       assert(encoded_stripe_.count(send_index) != 0);
//       auto fragment_id = frag_map[id];
//       auto fragment = encoded_stripe_[send_index]->fragments[fragment_id];
//       assert(fragment_id == fragment.GetVersion().fragment_id);
//       args.entries.push_back(fragment);
//       LOG(util::kRaft, "S%d Send (I%d T%d FragId%d) To S%d", id_, send_index,
//           fragment.Term(), fragment_id, id);
//     }
//     args.entry_cnt = args.entries.size();
//     rpc_clients_[id]->sendMessage(args);
//   }
//   // After each replicate entries, reset the replication Timer
//   resetReplicateTimer();
// }

void RaftState::sendHeartBeat(raft_node_id_t peer) {
  // Safety check: ensure rpc_clients_[peer] is valid
  if (peer >= 32 || rpc_clients_[peer] == nullptr || raft_peer_[peer] == nullptr) {
    return;
  }

  auto next_index = raft_peer_[peer]->NextIndex();
  auto prev_index = next_index - 1;
  auto prev_term = lm_->TermAt(prev_index);
  auto prev_k = GetLastEncodingK(prev_index);

  auto args = AppendEntriesArgs{CurrentTerm(), group_id_, id_, prev_index, prev_term,
                                prev_k, CommitIndex(), 0, std::vector<LogEntry>()};

  printf("[AE-SEND] G%d-N%d -> N%d: leader_id=%d prev_idx=%d prev_term=%d prev_k=%d entries=0\n",
         group_id_, id_, peer, args.leader_id, prev_index, prev_term, prev_k);
  fflush(stdout);

  rpc_clients_[peer]->sendAsyncMessage(args);
}

void RaftState::sendAppendEntries(raft_node_id_t peer) {
  LOG(util::kRaft, "S%d sendAppendEntries To S%d", id_, peer);
  auto next_index = raft_peer_[peer]->NextIndex();
  auto prev_index = next_index - 1;
  auto prev_term = lm_->TermAt(prev_index);
  auto prev_k = GetLastEncodingK(prev_index);

  auto args = AppendEntriesArgs{CurrentTerm(), group_id_, id_, prev_index, prev_term, prev_k, CommitIndex()};

  auto require_entry_cnt = lm_->LastLogEntryIndex() - prev_index;
  args.entries.reserve(require_entry_cnt);

  if (next_index <= CommitIndex()) {
    LOG(util::kRaft, "[CC] S%d Recover data for S%d(I%d->I%d)", id_, peer, next_index,
        lm_->LastLogEntryIndex());
    // This is a recovery task, we need to record the recovery stats
    if (GetRecoveryCtx(peer) == nullptr) {
      AddNewRecoveryCtx(peer);
      GetRecoveryCtx(peer)->start_recovery_index_ = next_index;
      GetRecoveryCtx(peer)->start_time_ = util::NowTime();
    }
  }

  // Check if we should use LRC latency-aware orthogonal placement
  if (HasLrcGrouper() && encoding_mode_ == 2) {  // LRC mode
    sendAppendEntriesLrc(peer, next_index, &args);
  } else {
    // Original RS encoding: frag_id equals peer_id
    for (auto raft_index = next_index; raft_index <= lm_->LastLogEntryIndex(); ++raft_index) {
      args.entries.push_back(encoded_stripe_[raft_index]->fragments[peer]);
    }
  }

  LOG(util::kRaft, "S%d AE To S%d (I%d->I%d) at T%d", id_, peer, next_index,
      lm_->LastLogEntryIndex(), CurrentTerm());

  // In LRC mode, each raft index produces exactly 1 entry (containing multiple fragments
  // assigned to this peer), so args.entries.size() should equal the number of raft indices
  // from next_index to LastLogEntryIndex().
  // For non-LRC mode, each raft index also produces 1 entry.
  // The assertion checks that we have the correct number of entries.
  if (args.entries.size() != static_cast<size_t>(lm_->LastLogEntryIndex() - next_index + 1)) {
    fprintf(stderr, "[FATAL-G%d-N%d] sendAppendEntries: entry count mismatch! "
            "expected %d, got %zu (next_index=%d, last_index=%d)\n",
            group_id_, id_,
            lm_->LastLogEntryIndex() - next_index + 1,
            args.entries.size(),
            next_index,
            lm_->LastLogEntryIndex());
    abort();
  }
  args.entry_cnt = args.entries.size();

  // NOTE: Debug log removed to prevent log flooding during normal operation.
  rpc_clients_[peer]->sendAsyncMessage(args);
}

// ============================================================================
// sendAppendEntriesLrc — Send entries using LRC orthogonal placement
//
// CRITICAL INVARIANT: For each raft index (i.e., each key/data), there MUST be
// exactly 1 LogEntry sent to each peer. That entry contains ALL fragments
// assigned to that peer for this raft index.
//
// IMPORTANT: For LRC entries, the fragments are stored directly in the LogEntry
// in lm_, NOT in encoded_stripe_. The encoded_stripe_ is used for RS encoding,
// but LRC entries store their fragments directly in the LogEntry.
//
// We use placement information to filter fragments for each peer.
// ============================================================================
void RaftState::sendAppendEntriesLrc(raft_node_id_t peer, raft_index_t start_index,
                                      AppendEntriesArgs* args) {
  // For each raft index, send exactly 1 entry containing the peer's fragments
  for (auto raft_index = start_index; raft_index <= lm_->LastLogEntryIndex(); ++raft_index) {
    // IMPORTANT: For LRC entries, get the LogEntry from lm_, not from encoded_stripe_
    const LogEntry* ent = lm_->GetSingleLogEntry(raft_index);
    if (ent == nullptr) {
      fprintf(stderr, "[ERROR-G%d-N%d] sendAppendEntriesLrc: no entry for index %d\n",
              group_id_, id_, raft_index);
      continue;
    }

    // Create a LogEntry for this peer containing ONLY the fragments assigned to it
    LogEntry peer_entry;
    peer_entry.SetIndex(raft_index);
    peer_entry.SetTerm(ent->Term());
    peer_entry.SetType(kNormal);
    peer_entry.SetChunkInfo(ent->GetChunkInfo());
    peer_entry.SetPlacement(ent->Placement());

    // === KEY-ONLY 复制：只传 key 部分，不传 value ===
    // 从 ChunkInfo 获取 key meta，从 command_data 提取 key
    // STRI header layout: [magic:4][group_id:8][mode:1][key_len:8][key:N] — key 起始于偏移 21
    Slice cmd_data = ent->CommandData();
    uint32_t key_size = ent->GetChunkInfo().GetKeySize();
    if (key_size > 0 && (21 + key_size) <= cmd_data.size()) {
      Slice key_slice(cmd_data.data() + 21, key_size);
      peer_entry.SetCommandData(key_slice);
    } else {
      // Fallback: 空 command_data（不传 key）
      peer_entry.SetCommandData(Slice());
    }
    peer_entry.SetCommandLength(ent->GetChunkInfo().GetTotalSize());

    // Get fragments from the LogEntry and filter by placement
    const auto& all_frags = ent->Fragments();
    const auto& placement = ent->Placement();


    int frags_added = 0;
    if (placement.empty()) {
      printf("[Debug] No placement for index %d\n", raft_index);
      // Fallback: if no placement, send all fragments
      for (const auto& frag : all_frags) {
        peer_entry.AddFragment(frag);
        frags_added++;
      }
    } else {
              // 打印 ent->Placement()，验证实际存储的 placement
      printf("[sendAppendEntriesLrc-PLACEMENT-G%d-N%d] I%d: ent->Placement() (%zu entries):\n",
        group_id_, id_, raft_index, placement.size());   
      for (const auto& fp : placement) {      
          printf("  %s\n", fp.ToString().c_str());   
      }  
      fflush(stdout);
      printf("[DEBUG] all_frags.size()=%zu, placement.size()=%zu\n",
             all_frags.size(), placement.size());
      for (size_t i = 0; i < placement.size(); ++i) {
        if (placement[i].node_id == peer) {
          int frag_id = placement[i].frag_id;
          printf("[DEBUG] i=%zu: frag_id=%d, node_id=%d, all_frags.size()=%zu, frag_id_check=%s\n",
                 i, frag_id, placement[i].node_id, all_frags.size(),
                 (frag_id >= 0 && frag_id < static_cast<int>(all_frags.size())) ? "PASS" : "FAIL");
          if (frag_id >= 0 && frag_id < static_cast<int>(all_frags.size())) {
            peer_entry.AddFragment(all_frags[frag_id]);
            frags_added++;
            printf("[DEBUG] Added frag_id=%d, frags_added now=%d\n", frag_id, frags_added);
          }
        }
      }
    }

    // CRITICAL INVARIANT: Each raft index must have at least 1 fragment
    if (frags_added == 0) {
      fprintf(stderr, "[FATAL-G%d-N%d] sendAppendEntriesLrc: NO fragments for peer %d "
              "at index %d! LRC placement is broken!\n",
              group_id_, id_, peer, raft_index);
      abort();
    }

    LOG(util::kRaft, "S%d LRC: Send %d frags to peer %d for index %d",
        id_, frags_added, peer, raft_index);

    // Add this entry to the AppendEntriesArgs
    args->entries.push_back(peer_entry);
  }
}

bool RaftState::containEntry(raft_index_t raft_index, raft_term_t raft_term,
                             raft_encoding_param_t prev_k) {
  // [DEBUG] Safety check: ensure lm_ is valid
  if (lm_ == nullptr) {
    fprintf(stderr, "[DEBUG-CONTAIN] G%d-N%d: FATAL lm_ is null!\n", group_id_, id_);
    fflush(stderr);
    return false;
  }

  LOG(util::kRaft, "S%d ContainEntry? (LT%d AT%d) (LI%d AI%d))", id_, lm_->LastLogEntryTerm(),
      raft_term, lm_->LastLogEntryIndex(), raft_index);

  if (raft_index == lm_->LastSnapshotIndex()) {
    return raft_term == lm_->LastSnapshotTerm();
  }
  const LogEntry *entry = lm_->GetSingleLogEntry(raft_index);
  if (entry == nullptr || entry->Term() != raft_term) {
    return false;
  }

  if (entry->GetChunkInfo().GetK() != prev_k) {
    return false;
  }
  return true;
}

void RaftState::PreLeaderBecomeLeader() {
  LOG(util::kRaft, "S%d PreLeaderStore Response: %d", id_,
      preleader_stripe_store_.CollectedFragmentsCnt());
  printf("[ELECTION-G%d-N%d] [DEBUG] PreLeaderBecomeLeader: collected=%d need=%d\n",
         group_id_, id_, preleader_stripe_store_.CollectedFragmentsCnt(), livenessLevel() + 1);
  fflush(stdout);
  if (preleader_stripe_store_.CollectedFragmentsCnt() >= livenessLevel() + 1) {
    LOG(util::kRaft, "S%d Rebuild fragments", id_);
    printf("[ELECTION-G%d-N%d] [DEBUG] PreLeaderBecomeLeader: enough fragments, decoding...\n",
           group_id_, id_);
    fflush(stdout);
    DecodeCollectedStripe();
    printf("[ELECTION-G%d-N%d] [DEBUG] PreLeaderBecomeLeader: after DecodeCollectedStripe, calling convertToLeaderInternal()\n",
           group_id_, id_);
    fflush(stdout);
    convertToLeaderInternal();  // Already holding lock from callers
    printf("[ELECTION-G%d-N%d] [DEBUG] PreLeaderBecomeLeader: convertToLeaderInternal() returned, Role=%d\n",
           group_id_, id_, Role());
    fflush(stdout);
  }
}

// TODO: Update logic
void RaftState::DecodeCollectedStripe() {
  // Debug:
  // ------------------------------------------------------------------
  LOG(util::kRaft, "S%d Decode Collected Stripes", id_);
  // ------------------------------------------------------------------
  for (int i = 0; i < preleader_stripe_store_.stripes.size(); ++i) {
    auto &stripe = preleader_stripe_store_.stripes[i];
    if (stripe.CollectFragmentsCount() == 0) {
      continue;
    }

    // For a stripe, filter the entry
    FilterDuplicatedCollectedFragments(stripe);

    LogEntry entry;
    auto succ = DecodingRaftEntry(&stripe, &entry);
    auto r_idx = i + preleader_stripe_store_.start_index;
    if (succ) {
      lm_->OverWriteLogEntry(entry, r_idx);
      LOG(util::kRaft, "S%d OverWrite Decoded Entry Info:%s", id_, entry.ToString().c_str());
      if (storage_ != nullptr) {
        storage_->OverwriteEntry(r_idx, entry);
      }
    } else {
      // Failed to decode a full entry, delete all preceding log entries
      auto last_index = lm_->LastLogEntryIndex();
      lm_->DeleteLogEntriesFrom(r_idx);
      LOG(util::kRaft, "S%d DelEntry(I%d)", id_, r_idx, last_index);

      // FIX: Check PersistQueue before deleting from storage.
      if (storage_ != nullptr) {
        if (persist_queue_ && persist_queue_->IsUnpersisted(r_idx)) {
          LOG(util::kRaft, "S%d Skip storage rollback for I%d (still unpersisted)",
              id_, r_idx);
          persist_queue_->TruncateFrom(r_idx);
        } else {
          storage_->DeleteEntriesFrom(r_idx);
          storage_->Sync();
        }
      }
      return;
    }
  }

  if (storage_ != nullptr) {
    storage_->Sync();
  }
}

bool RaftState::NeedOverwriteLogEntry(const ChunkInfo &old_info, const ChunkInfo &new_info) {
  // A smaller k means a newer version of encoded parities
  return new_info.GetK() < old_info.GetK();
}

void RaftState::FilterDuplicatedCollectedFragments(Stripe &stripes) {
  // The stripe may contain multiple fragments with different encoding
  // parameters, this function is responsible for only remaining those entries
  // that can be successfully decoded
}

bool RaftState::FindFullEntryInStripe(const Stripe *stripe, LogEntry *ent) {
  for (const auto &[id, frag] : stripe->collected_fragments) {
    if (frag.Type() == kNormal) {
      *ent = frag;
      return true;
    }
  }
  return false;
}

}  // namespace raft
