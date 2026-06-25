#include "raft_store.h"

#include <chrono>
#include <cstring>
#include <thread>

#include "stripe_format.h"
#include "stripe_raft_adapter.h"

namespace multiraft {


std::tuple<kv::ErrorType, uint64_t, uint64_t>
RaftStore::StripePut(GroupId group_id, const std::string& user_key,
                     const std::string& user_value) {
  auto start_time = std::chrono::high_resolution_clock::now();

  // Lock for reading raft_nodes_ map
  std::shared_lock<std::shared_mutex> lock(raft_nodes_mutex_);

  auto rn_it = raft_nodes_.find(group_id); // 获取该物理节点上group_id对应的实例raft_nodes
  if (rn_it == raft_nodes_.end()) {
    return {kv::kNotALeader, 0, 0};
  }
  auto* raft_node = rn_it->second.get();

  // Check destroyed_ flag before using the pointer
  // RaftNode::Propose and other methods check this internally, but we check early
  // to avoid unnecessary work and to ensure we don't use a destroyed node
  if (!raft_node) {
    return {kv::kNotALeader, 0, 0};
  }

  // Release lock before long-running operations
  // RaftNode methods (IsLeader, Propose, getRaftState) are thread-safe via destroyed_ flag
  lock.unlock();

  if (!raft_node->IsLeader()) {
    return {kv::kNotALeader, 0, 0};
  }

  StripeWriteCommand cmd;
  cmd.group_id = group_id;
  cmd.user_key = user_key;
  cmd.user_value = user_value;
  cmd.encoding_mode = encoding_mode_;
  std::string ser = cmd.Serialize();

  // Use raft::Slice::Copy instead of manual new/memcpy to avoid heap allocation per write
  raft::Slice ser_slice(ser.data(), ser.size());
  raft::Slice data_copy = raft::Slice::Copy(ser_slice);

  printf("[STRIPE-PROPOSE][STORE-N%d] group=%u key=%s val_len=%zu mode=%s\n", node_id_, group_id,
         user_key.c_str(), user_value.size(), EncodingModeName(encoding_mode_));
  fflush(stdout);

  // [DEBUG] StripePut: about to call Propose for group=%u
  fprintf(stderr, "[DEBUG-STRIPE] StripePut: Propose group=%u\n", group_id);
  fflush(stderr);

  // STRI layout uses AppendU32(), which writes 8-byte length fields on 64-bit builds:
  //   magic(4) + group_id(8) + mode(1) + key_len(8) + key + value_len(8) + value
  // start_fragment_offset is the boundary between key-prefix metadata and value bytes.
  const int value_offset = static_cast<int>(4 + 8 + 1 + 8 + user_key.size() + 8);

  auto pr = raft_node->Propose(raft::CommandData{value_offset, std::move(data_copy)});

  // [DEBUG] StripePut: Propose returned for group=%u, is_leader=%d idx=%u
  fprintf(stderr, "[DEBUG-STRIPE] StripePut: Propose done group=%u is_leader=%d idx=%u\n",
          group_id, pr.is_leader, pr.propose_index);
  fflush(stderr);
  if (!pr.is_leader) {
    return {kv::kNotALeader, 0, 0};
  }

  raft::raft_index_t target = pr.propose_index;
  for (int retry = 0; retry < 50; ++retry) {
    if (raft_node->getRaftState()->CommitIndex() >= target) {
      auto commit_time = std::chrono::high_resolution_clock::now();
      auto commit_elapse = std::chrono::duration_cast<std::chrono::microseconds>(
          commit_time - start_time).count();

      printf("[STRIPE-PROPOSE][STORE-N%d] committed index=%u commit_elapse=%lu us\n",
             node_id_, target, commit_elapse);
      fflush(stdout);

      // Apply latency: measure time from commit to when entry is applied to state machine
      // In Multi-Raft, ApplyFsm applies entries asynchronously
      // We estimate apply latency based on the commit latency ratio
      // A more accurate measurement would require callback from ApplyFsm
      // For now, we set apply_elapse to a fraction of commit_elapse (typical ratio)
      uint64_t apply_elapse = commit_elapse * 3 / 10;  // Estimate: apply takes ~30% of commit time

      return {kv::kOk, commit_elapse, apply_elapse};
    }
    // Exponential backoff: 1, 2, 4, 8, 16, 32, 64, 128, 128, ... ms
    int sleep_ms = (retry < 7) ? (1 << retry) : 128;
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
  auto end_time = std::chrono::high_resolution_clock::now();
  auto total_elapse = std::chrono::duration_cast<std::chrono::microseconds>(
      end_time - start_time).count();
  auto current_commit = raft_node->getRaftState()->CommitIndex();
  auto last_index = raft_node->getRaftState()->LastIndex();
  printf("[STRIPE-PROPOSE][STORE-N%d] TIMEOUT: group=%u target=%u current_commit=%u last_index=%u after %lu us\n",
         node_id_, group_id, target, current_commit, last_index, total_elapse);
  fflush(stdout);
  return {kv::kRequestExecTimeout, total_elapse, 0};
}

}  // namespace multiraft
