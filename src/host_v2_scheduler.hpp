#ifndef EMEBALACHAT_HOST_V2_SCHEDULER_HPP
#define EMEBALACHAT_HOST_V2_SCHEDULER_HPP

// ---------------------------------------------------------------------------
// host_v2_scheduler — REQ-043 (M6 T4, design §1.1 "스케줄러 큐", plan §V2-4.6):
// the v2 multi-session queue (EmebalaEngine target).
//
// Contract (§V2-4.6):
//   * Queue key = (priority ASC, enqueued_at ASC). priority is a v2-profile
//     request field, default 5, clamped 0..9.
//   * drop_eligible=true items (subtitle-style backpressure): when the queue
//     is at saturation the OLDEST drop_eligible item is DISCARDED and the new
//     one takes its place; the discarded item is reported to the caller so
//     ITS request answers busy.
//   * drop_eligible=false items wait in a finite queue that inherits the v1
//     §4.4 depth budget (8); overflow answers busy WITHOUT enqueueing.
//   * deadline_ms: expired queued items are discarded (each expiry reported)
//     so the deadline reaper answers timeout (§V2-4.6).
//   * Worker-family resource profile (registry, §V2-5.2) caps concurrent
//     contexts (max_sessions); M6 ggml-translate ships max_sessions=1, which
//     preserves the v1 §4.5 single-context serialization contract. The
//     dispatcher (host_main.cpp) consults the manager's Busy state, so the
//     queue itself stays context-count-free (single responsibility).
//   * The v1 PROFILE path (immediate-busy depth rule, §4.4) BYPASSES this
//     queue entirely — host_main.cpp's frozen JobQueue behavior is preserved
//     verbatim (profile separation, §V2-4.7).
//
// Threading: the pure decision core (SchedQueue) is lock-free and testable;
// the Scheduler façade adds the mutex + a monotonic enqueue sequence + an
// injectable clock (tests pin time for deadline/drop ordering).
//
// Privacy: this module never sees request text (SchedItem.user is an opaque
// caller token) and logs nothing — the host owns all shape-only diagnostics.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace emebalachat {
namespace host_v2 {

// v1 §4.4 depth budget inherited for the finite queue (plan §V2-4.6: "초기
// 상한은 v1 §4.4의 8을 계승").
inline constexpr std::size_t kSchedulerQueueDepth = 8;

// Default priority when a v2 request omits the field (§V2-4.6: 기본 5).
inline constexpr int kSchedulerDefaultPriority = 5;

// One queued request. `user` is an opaque caller token (the host stores its
// Connection* there); the scheduler never dereferences it.
struct SchedItem {
    std::uint64_t enqueue_seq = 0;            // host-global monotonic (FIFO tiebreak)
    int priority = kSchedulerDefaultPriority; // 0..9 (clamped), ASC dispatches first
    bool drop_eligible = false;               // backpressure-permitted (subtitle class)
    std::int64_t deadline_ms = 0;             // absolute; 0 = no deadline
    std::uint64_t session = 0;                // owning session (0 = sessionless job)
    void* user = nullptr;                     // opaque caller context (never touched)
};

// TryEnqueue outcomes. Busy answers are the CALLER's job (no pipe IO here).
enum class EnqueueResult : unsigned char {
    Enqueued,       // sits in the queue, awaiting dispatch
    BusyOverflow,   // queue full, nothing evictable -> caller answers busy for `item`
    BusyDroppedOld, // queue full: `evicted` (oldest eligible) was discarded to
                    // make room; the caller answers busy for `evicted`
};

// Pure decision core: queue contents + admission rules with NO locks and NO
// clock (the caller supplies now_ms and the monotonic seq). Unit-pinned.
class SchedQueue {
public:
    explicit SchedQueue(std::size_t depth = kSchedulerQueueDepth)
        : depth_(depth == 0 ? 1 : depth) {}

    // Admission per §V2-4.6. `seq` must be monotonically increasing per host.
    // On BusyDroppedOld the discarded resident is copied into `evicted`.
    EnqueueResult TryEnqueue(SchedItem item, std::uint64_t seq, SchedItem& evicted);

    // Pop the best next item (priority ASC, then enqueue_seq ASC). Expired
    // head items are discarded FIRST (reported via `expired_out` +
    // `had_expired`) — expired work never dispatches (§V2-4.6 deadline rule).
    // False when nothing remains. One expiry is reported per call; the caller
    // re-pops to drain further expiries.
    bool Pop(SchedItem& out, std::int64_t now_ms, SchedItem& expired_out,
             bool& had_expired);

    // Deadline reaper pass: removes every expired item, appending each to
    // `expired` (the caller answers timeout per item). Returns the count.
    std::size_t ReaperPass(std::int64_t now_ms, std::vector<SchedItem>& expired);

    // Drops every queued item owned by `session` (session teardown); each is
    // appended to `dropped` for caller-side unavailable answers. Returns the
    // count. Session 0 (sessionless jobs) is never a drop target.
    std::size_t DropSession(std::uint64_t session, std::vector<SchedItem>& dropped);

    std::size_t Size() const { return items_.size(); }
    bool Empty() const { return items_.empty(); }
    std::size_t Depth() const { return depth_; }

private:
    static bool OrderBefore(const SchedItem& a, const SchedItem& b) {
        if (a.priority != b.priority) return a.priority < b.priority; // ASC (§V2-4.6)
        return a.enqueue_seq < b.enqueue_seq;                          // arrival FIFO
    }
    std::size_t depth_;
    std::deque<SchedItem> items_; // sorted by (priority, seq) at every mutation
};

// Thread-safe façade over SchedQueue: mutex + monotonic sequence + injectable
// clock (steady_clock by default; tests pin time).
class Scheduler {
public:
    using NowFn = std::function<std::int64_t()>;

    explicit Scheduler(NowFn now = nullptr)
        : now_(now ? std::move(now)
                   : [] {
                         return std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                             .count();
                     }) {}

    // See SchedQueue::TryEnqueue. The discarded item (BusyDroppedOld) lands in
    // `evicted`; sequence assignment is internal and thread-safe.
    EnqueueResult Enqueue(SchedItem item, SchedItem& evicted);

    // See SchedQueue::Pop. Thread-safe.
    bool Pop(SchedItem& out, SchedItem& expired_out, bool& had_expired);

    // See SchedQueue::ReaperPass. Thread-safe.
    std::size_t ReaperPass(std::vector<SchedItem>& expired);

    // See SchedQueue::DropSession. Thread-safe.
    std::size_t DropSession(std::uint64_t session, std::vector<SchedItem>& dropped);

    std::size_t Size();

private:
    std::mutex mu_;
    SchedQueue q_;
    std::uint64_t next_seq_ = 1; // host-global enqueue sequence (>= 1)
    NowFn now_;
};

} // namespace host_v2
} // namespace emebalachat

#endif // EMEBALACHAT_HOST_V2_SCHEDULER_HPP
