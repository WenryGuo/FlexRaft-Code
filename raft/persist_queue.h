#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "log_entry.h"
#include "raft_type.h"
#include "storage.h"

namespace raft {

// PersistQueue - Unstable Log Buffer for asynchronous batch persistence.
//
// This class solves the concurrency issues in the original Propose() design:
// 1. Index allocation race: entries are assigned indices atomically in LogManager
// 2. Sync blocking: disk I/O is offloaded to a background thread
// 3. Rollback safety: truncation only affects in-memory queue, not persisted data
//
// Architecture:
//   Propose() -> PersistQueue::Push() -> Background Worker -> Storage::PersistEntries()
//                 ↓
//           ReplicateNewProposeEntry() (parallel, no waiting)
//
class PersistQueue {
 public:
  // Item stored in the unstable buffer before persistence
  struct PersistItem {
    raft_index_t index;
    raft_term_t term;
    LogEntry entry;
    std::atomic<bool> persisted{false};  // true once written to disk

    PersistItem(raft_index_t idx, raft_term_t trm, LogEntry&& ent)
        : index(idx), term(trm), entry(std::move(ent)) {}
  };

  PersistQueue(Storage* storage, int batch_size = 32);
  ~PersistQueue();

  // Non-copyable, non-movable
  PersistQueue(const PersistQueue&) = delete;
  PersistQueue& operator=(const PersistQueue&) = delete;
  PersistQueue(PersistQueue&&) = delete;
  PersistQueue& operator=(PersistQueue&&) = delete;

  // Push an entry into the unstable buffer.
  // The entry will be persisted asynchronously by the background worker.
  // Returns the index assigned to this entry.
  raft_index_t Push(LogEntry&& entry, raft_index_t index);

  // Get the index of the last entry in the queue (not yet persisted)
  raft_index_t LastUnpersistedIndex() const;

  // Get all unpersisted entries as a batch.
  // Returns entries with indices in [from, to] range.
  // MarkPersisted() should be called after successful persistence.
  std::vector<PersistItem*> GetUnpersistedBatch(raft_index_t from, size_t max_batch);

  // Mark entries as persisted (after successful disk write)
  void MarkPersisted(raft_index_t from, raft_index_t to);

  // Truncate entries from index onwards (for rollback after losing leadership).
  // This only removes from in-memory queue, NOT from storage.
  void TruncateFrom(raft_index_t idx);

  // Check if an entry at given index is still unpersisted.
  // Returns true if the entry is still in the queue and needs rollback.
  // Called before storage_->DeleteEntriesFrom() to avoid deleting persisted data.
  bool IsUnpersisted(raft_index_t idx) const;

  // Notify that a new term has started (leader election).
  // Entries from previous term in the queue should be truncated.
  // This is called when becoming leader to clean up stale entries.
  void OnTermChange(raft_term_t new_term);

  // Get the highest index that has been persisted to disk
  raft_index_t GetLastPersistedIndex() const { return last_persisted_index_.load(); }

  // Shutdown the background worker gracefully
  void Shutdown();

  // Storage accessor (for batch persistence)
  Storage* storage() const { return storage_; }

 private:
  void PersistWorkerLoop();

 private:
  Storage* storage_;
  int batch_size_;

  mutable std::mutex mtx_;
  std::deque<PersistItem*> queue_;  // ordered by index

  std::atomic<raft_index_t> last_persisted_index_{0};
  std::atomic<bool> running_{true};
  std::thread persist_thread_;
  std::condition_variable cv_;
};

}  // namespace raft
