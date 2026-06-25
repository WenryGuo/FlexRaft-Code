#include "persist_queue.h"

#include <algorithm>

#include "util.h"

namespace raft {

PersistQueue::PersistQueue(Storage* storage, int batch_size)
    : storage_(storage), batch_size_(batch_size) {
  if (storage_ != nullptr) {
    persist_thread_ = std::thread(&PersistQueue::PersistWorkerLoop, this);
  }
}

PersistQueue::~PersistQueue() {
  Shutdown();
}

void PersistQueue::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mtx_);
    running_ = false;
    cv_.notify_all();
  }
  if (persist_thread_.joinable()) {
    persist_thread_.join();
  }

  // Clean up remaining items in queue
  for (auto* item : queue_) {
    delete item;
  }
  queue_.clear();
}

raft_index_t PersistQueue::Push(LogEntry&& entry, raft_index_t index) {
  auto* item = new PersistItem(index, entry.Term(), std::move(entry));

  {
    std::lock_guard<std::mutex> lock(mtx_);
    queue_.push_back(item);
    LOG(util::kRaft, "PQ: Push I%d (queue_size=%zu)", index, queue_.size());
  }

  // Notify the worker thread
  cv_.notify_one();

  return index;
}

raft_index_t PersistQueue::LastUnpersistedIndex() const {
  std::lock_guard<std::mutex> lock(mtx_);
  if (queue_.empty()) {
    return last_persisted_index_.load();
  }
  return queue_.back()->index;
}

std::vector<PersistQueue::PersistItem*> PersistQueue::GetUnpersistedBatch(
    raft_index_t from, size_t max_batch) {
  std::vector<PersistItem*> result;
  std::lock_guard<std::mutex> lock(mtx_);

  for (auto* item : queue_) {
    if (item->index < from) continue;
    if (result.size() >= max_batch) break;
    if (!item->persisted.load()) {
      result.push_back(item);
    }
  }

  return result;
}

void PersistQueue::MarkPersisted(raft_index_t from, raft_index_t to) {
  std::lock_guard<std::mutex> lock(mtx_);

  // Mark items as persisted and remove from queue
  while (!queue_.empty() && queue_.front()->index <= to) {
    auto* item = queue_.front();
    item->persisted.store(true);
    last_persisted_index_.store(item->index);
    queue_.pop_front();
    delete item;
  }

  LOG(util::kRaft, "PQ: MarkPersisted I%d..I%d, queue_size=%zu",
      from, to, queue_.size());
}

void PersistQueue::TruncateFrom(raft_index_t idx) {
  std::lock_guard<std::mutex> lock(mtx_);

  size_t removed = 0;
  while (!queue_.empty() && queue_.front()->index >= idx) {
    auto* item = queue_.front();
    queue_.pop_front();
    delete item;
    ++removed;
  }

  LOG(util::kRaft, "PQ: TruncateFrom I%d, removed=%zu, remaining=%zu",
      idx, removed, queue_.size());
}

bool PersistQueue::IsUnpersisted(raft_index_t idx) const {
  std::lock_guard<std::mutex> lock(mtx_);

  for (const auto* item : queue_) {
    if (item->index == idx) {
      return !item->persisted.load();
    }
    // Queue is ordered by index, so if we've passed idx, it's persisted
    if (item->index > idx) {
      break;
    }
  }
  return false;
}

void PersistQueue::OnTermChange(raft_term_t new_term) {
  std::lock_guard<std::mutex> lock(mtx_);

  // Truncate all entries from the new term onwards
  // Note: entries with old term but lower index are fine to keep
  size_t removed = 0;
  while (!queue_.empty()) {
    auto* item = queue_.front();
    // For safety, remove all unpersisted entries on term change
    if (!item->persisted.load()) {
      queue_.pop_front();
      delete item;
      ++removed;
    } else {
      break;  // Persisted entries are already safe
    }
  }

  if (removed > 0) {
    LOG(util::kRaft, "PQ: OnTermChange T%d, removed=%zu unpersisted entries",
        new_term, removed);
  }
}

void PersistQueue::PersistWorkerLoop() {
  printf("[PERSIST-Q] Persist worker thread started\n");
  fflush(stdout);

  while (running_) {
    std::vector<PersistItem*> batch;

    // Wait for work with timeout
    {
      std::unique_lock<std::mutex> lock(mtx_);
      cv_.wait_for(lock, std::chrono::milliseconds(10), [this] {
        return !running_ || !queue_.empty();
      });

      if (!running_) break;

      // Collect batch of unpersisted entries
      size_t count = 0;
      for (auto* item : queue_) {
        if (count >= static_cast<size_t>(batch_size_)) break;
        if (!item->persisted.load()) {
          batch.push_back(item);
          ++count;
        }
      }
    }

    if (batch.empty()) {
      continue;
    }

    raft_index_t from_idx = batch.front()->index;
    raft_index_t to_idx = batch.back()->index;

    LOG(util::kRaft, "PQ: Persisting batch I%d..I%d (%zu entries)",
        from_idx, to_idx, batch.size());

    // Perform batch persistence
    std::vector<LogEntry> entries;
    entries.reserve(batch.size());
    for (auto* item : batch) {
      entries.push_back(item->entry);
    }

    if (storage_ != nullptr) {
      storage_->PersistEntries(from_idx, to_idx, entries);
      storage_->Sync();
    }

    // Mark as persisted
    MarkPersisted(from_idx, to_idx);
  }

  printf("[PERSIST-Q] Persist worker thread exiting\n");
  fflush(stdout);
}

}  // namespace raft
