// host_v2_scheduler implementation — REQ-043 (M6 T4, design §1.1, §V2-4.6).
// The .hpp holds the contract comments; this file is the mechanics. The
// <chrono> include backs the Scheduler façade's default clock lambda.

#include "host_v2_scheduler.hpp"

#include <algorithm>
#include <chrono>

namespace emebalachat {
namespace host_v2 {

// ---- SchedQueue (pure core) -------------------------------------------------

EnqueueResult SchedQueue::TryEnqueue(SchedItem item, std::uint64_t seq,
                                     SchedItem& evicted) {
    item.enqueue_seq = seq;
    if (item.priority < 0) item.priority = 0;
    if (item.priority > 9) item.priority = 9;

    if (items_.size() < depth_) {
        items_.push_back(std::move(item));
        std::sort(items_.begin(), items_.end(),
                  [this](const SchedItem& a, const SchedItem& b) {
                      return OrderBefore(a, b);
                  });
        return EnqueueResult::Enqueued;
    }
    // Saturated. drop_eligible=true: evict the OLDEST eligible resident and
    // take its slot (§V2-4.6 "가장 오래된 것부터 폐기"); the caller answers
    // busy for the DISCARDED item, not the newcomer.
    if (item.drop_eligible) {
        auto oldest_eligible = std::find_if(items_.begin(), items_.end(),
            [](const SchedItem& it) { return it.drop_eligible; });
        if (oldest_eligible != items_.end()) {
            evicted = *oldest_eligible; // report BEFORE replacement
            *oldest_eligible = std::move(item);
            std::sort(items_.begin(), items_.end(),
                      [this](const SchedItem& a, const SchedItem& b) {
                          return OrderBefore(a, b);
                      });
            return EnqueueResult::BusyDroppedOld;
        }
        // Full AND every resident is non-eligible: an eligible newcomer cannot
        // preempt protected work -> busy without enqueueing.
        return EnqueueResult::BusyOverflow;
    }
    // Non-eligible + full: finite-queue overflow (v1 §4.4 depth budget) —
    // busy, NOT queued.
    return EnqueueResult::BusyOverflow;
}

bool SchedQueue::Pop(SchedItem& out, std::int64_t now_ms, SchedItem& expired_out,
                     bool& had_expired) {
    had_expired = false;
    // Expired work never dispatches: discard the head first (§V2-4.6).
    while (!items_.empty() && items_.front().deadline_ms != 0 &&
           now_ms >= items_.front().deadline_ms) {
        expired_out = std::move(items_.front());
        items_.pop_front();
        had_expired = true;
        return true; // one expiry per call; the caller re-pops to drain
    }
    if (items_.empty()) return false;
    out = std::move(items_.front());
    items_.pop_front();
    return true;
}

std::size_t SchedQueue::ReaperPass(std::int64_t now_ms, std::vector<SchedItem>& expired) {
    std::size_t n = 0;
    for (auto it = items_.begin(); it != items_.end();) {
        if (it->deadline_ms != 0 && now_ms >= it->deadline_ms) {
            expired.push_back(std::move(*it));
            it = items_.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    return n;
}

std::size_t SchedQueue::DropSession(std::uint64_t session, std::vector<SchedItem>& dropped) {
    if (session == 0) return 0;
    std::size_t n = 0;
    for (auto it = items_.begin(); it != items_.end();) {
        if (it->session == session) {
            dropped.push_back(std::move(*it));
            it = items_.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    return n;
}

// ---- Scheduler (thread-safe façade) -----------------------------------------

EnqueueResult Scheduler::Enqueue(SchedItem item, SchedItem& evicted) {
    std::lock_guard<std::mutex> lk(mu_);
    const EnqueueResult r = q_.TryEnqueue(std::move(item), next_seq_, evicted);
    ++next_seq_;
    return r;
}

bool Scheduler::Pop(SchedItem& out, SchedItem& expired_out, bool& had_expired) {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.Pop(out, now_(), expired_out, had_expired);
}

std::size_t Scheduler::ReaperPass(std::vector<SchedItem>& expired) {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.ReaperPass(now_(), expired);
}

std::size_t Scheduler::DropSession(std::uint64_t session, std::vector<SchedItem>& dropped) {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.DropSession(session, dropped);
}

std::size_t Scheduler::Size() {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.Size();
}

} // namespace host_v2
} // namespace emebalachat
