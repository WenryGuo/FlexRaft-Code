#pragma once
// mailbox.h  —  MPSC (多生产者单消费者) Mailbox
//
// 对应 TiKV 的 BasicMailbox<N>。
// 同一时刻只有一个 Poll 线程在消费某个 Mailbox（线性一致性保证）。
// 多个线程可以并发向同一个 Mailbox push 消息。
//
// 支持有界队列 + 阻塞式背压：当队列满时，push() 会阻塞直到有空间。
// 这防止 Raft 复制快于 ApplyFsm 消费时的无界内存增长。

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>

namespace multiraft {

// -----------------------------------------------------------------------
//  Mailbox<Msg>
//  - push(): 任意线程调用，O(1)，持锁入队后通知调度器
//            当队列达到 max_capacity 时阻塞（背压）
//  - drain(): Poll 线程批量取出最多 max_batch 条消息
// -----------------------------------------------------------------------
template <typename Msg>
class Mailbox {
 public:
  static constexpr size_t kDefaultCapacity = 1024;

  // max_capacity: 有界队列容量，达到时 push() 阻塞（0 = 无界，向后兼容）
  explicit Mailbox(size_t max_capacity = 0)
      : max_capacity_(max_capacity) {}

  // 向 mailbox 投递一条消息，唤醒正在等待的 Poll 线程
  // 当队列满时阻塞，直到有空间（背压机制）
  void push(Msg&& m) {
    {
      std::unique_lock<std::mutex> lk(mu_);
      // 背压：队列满时等待直到被 drain 唤醒
      if (max_capacity_ > 0) {
        cv_producer_.wait(lk, [this] { return q_.size() < max_capacity_; });
      }
      q_.push(std::move(m));
    }
    cv_consumer_.notify_one();
  }

  // Poll 线程调用：批量取出最多 max_batch 条，写入 out
  // 返回取出的消息数量
  size_t drain(std::vector<Msg>& out, size_t max_batch) {
    std::unique_lock<std::mutex> lk(mu_);
    size_t n = 0;
    while (!q_.empty() && n < max_batch) {
      out.push_back(std::move(q_.front()));
      q_.pop();
      ++n;
    }
    // 唤醒可能阻塞在 push() 的生产者
    if (max_capacity_ > 0 && n > 0) {
      cv_producer_.notify_one();
    }
    return n;
  }

  // 阻塞等待直到有消息或超时（供 Poll 线程在空闲时调用）
  void wait_for(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_consumer_.wait_for(lk, timeout, [this] { return !q_.empty(); });
  }

  bool empty() const {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.empty();
  }

  size_t size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.size();
  }

  // 近似大小（无需加锁，不精确但无竞争）
  size_t size_approx() const {
    return q_.size();
  }

  // 检查是否接近满（有界队列监控）
  bool nearly_full() const {
    if (max_capacity_ == 0) return false;
    std::lock_guard<std::mutex> lk(mu_);
    return q_.size() >= max_capacity_ * 3 / 4;  // 75% 告警
  }

 private:
  mutable std::mutex            mu_;
  std::condition_variable       cv_consumer_;  // 消费者（Poll）等待
  std::condition_variable       cv_producer_;  // 生产者等待（队列满时）
  std::queue<Msg>              q_;
  size_t                       max_capacity_;  // 0 = 无界
};

}  // namespace multiraft
