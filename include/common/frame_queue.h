// Bounded queue between the receive thread and the display thread.
//
// Synchronization: one mutex protects the deque; a condition variable wakes a
// waiting pop(). push() NEVER blocks: when the queue is full the OLDEST frame
// is dropped, so a slow display cannot stall network reception. The same
// policy is used for TCP and UDP.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

#include "common/frame.h"

class FrameQueue {
public:
    enum class Pop { FRAME, TIMEOUT, CLOSED };

    explicit FrameQueue(std::size_t cap) : cap_(cap == 0 ? 1 : cap) {}

    // Returns true and sets dropped_id if an old frame had to be discarded.
    bool push(Frame&& f, std::uint32_t& dropped_id)
    {
        bool dropped = false;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (q_.size() >= cap_) {
                dropped_id = q_.front().id;
                q_.pop_front();
                dropped = true;
            }
            q_.push_back(std::move(f));
        }
        cv_.notify_one();
        return dropped;
    }

    // Wait up to timeout_ms. CLOSED is only returned once the queue is empty.
    Pop pop(Frame& out, int timeout_ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                     [this] { return !q_.empty() || closed_; });
        if (!q_.empty()) {
            out = std::move(q_.front());
            q_.pop_front();
            return Pop::FRAME;
        }
        return closed_ ? Pop::CLOSED : Pop::TIMEOUT;
    }

    // No more frames will be pushed.
    void close()
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            closed_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Frame> q_;
    std::size_t cap_;
    bool closed_ = false;
};
