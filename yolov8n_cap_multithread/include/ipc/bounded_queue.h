#pragma once

// bounded_queue.h — Thread-safe bounded FIFO with configurable drop policy.
//
// Design rationale:
//   The inference pipeline must never block on IPC consumers. BoundedQueue sits
//   between the realtime pipeline and the slower IPC sender thread. When the
//   consumer falls behind, the queue drops the oldest frame (kDropOldest) so
//   the consumer always receives the most recent detections once it catches up.
//
// Thread model:
//   push() — called from the main pipeline thread (single producer).
//   pop()  — called from the IPC sender thread (single consumer).
//   Both operations are fully thread-safe; the queue may be safely shared
//   across multiple producers/consumers if needed later.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

template<typename T>
class BoundedQueue {
public:
    /// When the queue is full, choose which item to sacrifice.
    enum class DropPolicy {
        kDropOldest,   ///< Discard the front (oldest) item to make room.
        kDropNewest,   ///< Reject the incoming item; queue is unchanged.
    };

    explicit BoundedQueue(std::size_t capacity,
                          DropPolicy  policy = DropPolicy::kDropOldest)
        : capacity_(capacity), policy_(policy), closed_(false) {}

    // Non-copyable, non-movable (owns a mutex + condvar).
    BoundedQueue(const BoundedQueue&)            = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    // -----------------------------------------------------------------------
    // push — non-blocking.
    //
    // Returns true  when the item was accepted without evicting anything.
    // Returns false when an item was dropped (oldest evicted or newest
    // rejected, per policy). A 'false' return does NOT mean the push failed —
    // the new item is still in the queue when kDropOldest is set.
    // -----------------------------------------------------------------------
    bool push(T item) {
        bool dropped = false;
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (closed_) return false;
            if (queue_.size() >= capacity_) {
                if (policy_ == DropPolicy::kDropOldest) {
                    queue_.pop_front();
                    dropped = true;
                } else {
                    // kDropNewest: silently discard the incoming item.
                    return false;
                }
            }
            queue_.push_back(std::move(item));
        }
        cv_.notify_one();
        return !dropped;
    }

    // -----------------------------------------------------------------------
    // pop — blocking with deadline.
    //
    // Waits up to `timeout` for an item. Fills `out` and returns true on
    // success. Returns false on timeout or if the queue was closed and
    // is now empty. Callers should loop and check a stop flag after a
    // false return.
    // -----------------------------------------------------------------------
    bool pop(T& out,
             std::chrono::milliseconds timeout = std::chrono::milliseconds(200)) {
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_for(lk, timeout,
                [this]{ return !queue_.empty() || closed_; }))
            return false;   // timed out
        if (queue_.empty()) return false;   // closed and drained
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    // -----------------------------------------------------------------------
    // Utility
    // -----------------------------------------------------------------------

    std::size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return queue_.size();
    }

    std::size_t capacity() const { return capacity_; }

    // Signal close: unblocks any waiting pop() calls. No more pushes succeed
    // after this. Existing items can still be drained via pop().
    void close() {
        { std::lock_guard<std::mutex> lk(mu_); closed_ = true; }
        cv_.notify_all();
    }

private:
    const std::size_t       capacity_;
    const DropPolicy        policy_;
    bool                    closed_;
    std::deque<T>           queue_;
    mutable std::mutex      mu_;
    std::condition_variable cv_;
};
