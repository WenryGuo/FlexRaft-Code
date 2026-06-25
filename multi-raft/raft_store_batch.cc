#include "raft_store.h"
#include <algorithm>
#include <chrono>

namespace multiraft {

// BuildPartitions — 根据 peer_threads_ 和 apply_threads_ 的数量，
// 将 peer_entries_ / apply_entries_ 均匀划分为若干段，
// 每段由一个 Poll 线程独占。
void BatchSystem::BuildPartitions() {
    // Peer 分区
    peer_part_start_.resize(peer_thread_count_);
    peer_part_end_.resize(peer_thread_count_);
    if (peer_thread_count_ > 0 && !peer_entries_.empty()) {
        size_t total = peer_entries_.size();
        size_t base = total / peer_thread_count_;
        size_t rem  = total % peer_thread_count_;
        size_t pos  = 0;
        for (int i = 0; i < peer_thread_count_; ++i) {
            peer_part_start_[i] = pos;
            size_t slice = base + (i < static_cast<int>(rem) ? 1 : 0);
            peer_part_end_[i]   = pos + slice;
            pos += slice;
        }
    }

    // Apply 分区
    apply_part_start_.resize(apply_thread_count_);
    apply_part_end_.resize(apply_thread_count_);
    if (apply_thread_count_ > 0 && !apply_entries_.empty()) {
        size_t total = apply_entries_.size();
        size_t base = total / apply_thread_count_;
        size_t rem  = total % apply_thread_count_;
        size_t pos  = 0;
        for (int i = 0; i < apply_thread_count_; ++i) {
            apply_part_start_[i] = pos;
            size_t slice = base + (i < static_cast<int>(rem) ? 1 : 0);
            apply_part_end_[i]   = pos + slice;
            pos += slice;
        }
    }
    printf("[BATCH-N%d] Partitions built: %zu peers / %zu applys over %d+%d threads\n",
           node_id_, peer_entries_.size(), apply_entries_.size(),
           peer_thread_count_, apply_thread_count_);
}

// PeerPollLoop — 线程 thread_id 只处理自己分区内的 PeerFsm Mailbox
void BatchSystem::PeerPollLoop(int thread_id) {
    size_t start = (thread_id < static_cast<int>(peer_part_start_.size()))
                       ? peer_part_start_[thread_id] : 0;
    size_t end   = (thread_id < static_cast<int>(peer_part_end_.size()))
                       ? peer_part_end_[thread_id] : peer_entries_.size();

    std::vector<PeerMsg> batch;
    while (running_.load()) {
        bool did_work = false;
        for (size_t idx = start; idx < end; ++idx) {
            auto& entry = peer_entries_[idx];
            batch.clear();
            size_t n = entry->peer_mb.drain(batch, max_batch_);
            if (n > 0) {
                did_work = true;
                entry->peer_fsm->HandleBatch(batch);
            }
        }
        // 空闲时阻塞等待，依赖 Mailbox 的 cv_consumer_ 唤醒（零 CPU 消耗，微秒级响应）
        if (!did_work) {
            for (size_t idx = start; idx < end; ++idx) {
                peer_entries_[idx]->peer_mb.wait_for(std::chrono::milliseconds(100));
            }
        }
    }
    printf("[BATCH-N%d] PeerPollLoop-%d exiting\n", node_id_, thread_id);
}

// ApplyPollLoop — work-stealing: each thread processes ALL groups in every iteration.
// This ensures no group starves when thread count > group count.
void BatchSystem::ApplyPollLoop(int thread_id) {
    (void)thread_id;  // unused; all threads process all groups

    std::vector<ApplyMsg> batch;
    while (running_.load()) {
        bool did_work = false;
        // Process ALL apply groups on every iteration for maximum responsiveness
        for (size_t idx = 0; idx < apply_entries_.size(); ++idx) {
            auto& entry = apply_entries_[idx];
            batch.clear();
            size_t n = entry->apply_fsm->mailbox().drain(batch, max_batch_);
            if (n > 0) {
                did_work = true;
                entry->apply_fsm->HandleBatch(batch);
            }
        }
        // 空闲时阻塞等待，依赖 Mailbox 的 cv_consumer_ 唤醒（零 CPU 消耗，微秒级响应）
        if (!did_work) {
            for (size_t idx = 0; idx < apply_entries_.size(); ++idx) {
                apply_entries_[idx]->apply_fsm->mailbox().wait_for(std::chrono::milliseconds(100));
            }
        }
    }
    printf("[BATCH-N%d] ApplyPollLoop-%d exiting\n", node_id_, thread_id);
}

}  // namespace multiraft
