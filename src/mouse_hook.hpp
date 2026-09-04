#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <windows.h>

namespace emebalachat {

class MouseHook {
public:
    // Callback invoked when mouse drag release exceeds threshold (> 15px), or
    // after the double/triple-click settle window confirms a multi-click.
    using DragReleaseCallback = std::function<void(int x, int y)>;

    // Callback invoked on mouse button down (used for outside-click dismissals)
    using MouseDownCallback = std::function<void(int x, int y)>;

    // Multi-click settle window: how long a double/triple-click waits before
    // the drag-release callback fires, so the target app can finish its text
    // selection highlight. 60 ms matches the pre-REQ-R09 delayed-thread window.
    static constexpr uint64_t kMultiClickDebounceMs = 60;

    // Re-check cadence for the delayed-click worker: bounds (a) how quickly a
    // job notices it was invalidated by a newer click and (b) how long Stop()'s
    // join can take after the worker was told to stop (<= one poll step).
    // The hook path itself never waits.
    static constexpr uint32_t kDelayedClickPollMs = 16;

    // ---- REQ-R09 (audit §3.3) pure invalidation predicate, headless-testable ----
    //
    // A delayed multi-click job carries the click sequence it was created for
    // (captured_seq). The hook thread owns click_seq_ and bumps it on every
    // physical press (WM_LBUTTONDOWN) AND on every confirmed multi-click
    // (WM_LBUTTONUP). The job fires only while it is still the latest click:
    //
    //   fire  <=>  running && captured_seq == current_seq && now >= deadline
    //
    // A newer physical click (seq bump), Stop() (running=false), or the settle
    // window not yet expired each keep the job from firing. This replaces the
    // C2-regression join() inside LowLevelMouseProc: the hook callback never
    // waits for the previous job - the previous job observes the newer sequence
    // on its own thread and no-ops.
    static constexpr bool ShouldFireDelayedClick(
        uint64_t captured_seq,
        uint64_t current_seq,
        bool running,
        uint64_t now_ms,
        uint64_t deadline_ms
    ) {
        return running && captured_seq == current_seq && now_ms >= deadline_ms;
    }

    // Pure settle-window math: job deadline = arm time + debounce window.
    static constexpr uint64_t MultiClickDeadlineMs(uint64_t arm_tick_ms) {
        return arm_tick_ms + kMultiClickDebounceMs;
    }

    MouseHook();
    ~MouseHook();

    bool Start();
    void Stop();

    void SetDragReleaseCallback(DragReleaseCallback cb);
    void SetMouseDownCallback(MouseDownCallback cb);

    void SetEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
    bool IsEnabled() const { return enabled_.load(std::memory_order_relaxed); }
    int GetClickCount() const { return click_count_.load(std::memory_order_relaxed); }

    // ---- Test/inspection seams for the headless-testable parts of REQ-R09
    // (see TestMouseHookDebounce in tests/run_tests.cpp). They mirror the exact
    // operations the LL callback performs; production code never calls them. ----

    // Current multi-click sequence counter (invalidation token owned by the
    // hook thread).
    uint64_t ClickSeqForTest() const { return click_seq_.load(std::memory_order_acquire); }

    // One physical-click sequence bump, identical to the fetch_add the hook
    // callback performs, so the invalidation path is exercisable without real
    // mouse input.
    uint64_t BumpClickSeqForTest() {
        return click_seq_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    // Public face of the internal arm seam used by LowLevelMouseProc.
    void ArmDelayedClickForTest(uint64_t seq, POINT pt) { ArmDelayedClick(seq, pt); }

private:
    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    void HookThreadProc();

    // REQ-R09: hand a confirmed multi-click to the persistent delayed-click
    // worker. Runs ON the hook thread. Budget: one GetTickCount64() + one
    // try_lock of a mutex the worker holds only for microsecond snapshots
    // (never across its settle wait or callback) + three scalar writes +
    // notify_one. If the try_lock ever loses the microsecond race the event is
    // coalesced (traced to stderr) instead of waiting on the hook thread.
    // Lazily spawns the worker on first use; never reads/writes a std::thread
    // handle beyond that one-time empty-slot move; never joins; never sleeps.
    void ArmDelayedClick(uint64_t seq, POINT pt);

    // Body of the persistent delayed-click worker (std::jthread member, joined
    // deterministically by Stop() on the MAIN thread - so a job can never
    // outlive the object, unlike the pre-C2 detached-thread design). Holds the
    // settle window with sleep steps <= kDelayedClickPollMs OUTSIDE the job
    // mutex, then evaluates the pure ShouldFireDelayedClick predicate.
    void DelayedClickWorkerLoop(std::stop_token st);

    std::atomic<bool> enabled_{true};
    std::atomic<bool> running_{false};
    DWORD hook_thread_id_ = 0;
    HHOOK hHook_ = nullptr;
    HANDLE hReadyEvent_ = nullptr;
    std::thread thread_;

    // Drag and multi-click detection state. The hook callback runs
    // single-threaded on the hook thread; the delayed worker reads click_seq_
    // (acquire) for the invalidation check and running_ for the stop check.
    // click_count_ is atomic because GetClickCount() may be called from any
    // thread (tests).
    std::atomic<bool> is_lbutton_down_{false};
    POINT lbutton_down_pt_ = {};
    DWORD last_click_time_ = 0;
    POINT last_click_pt_ = {};
    std::atomic<int> click_count_{0};
    std::atomic<uint64_t> click_seq_{0};

    // Delayed-click job (REQ-R09). Persistent worker, live while running_.
    // ALL job_* field access (write by hook thread, read by worker) happens
    // under job_mutex_; job_generation_ is the publication counter the worker
    // waits on. The worker releases job_mutex_ via wait_for and its settle
    // sleep, so the hook thread's try_lock succeeds in practice always.
    std::jthread delayed_click_worker_;
    std::atomic<bool> delayed_worker_live_{false};
    std::mutex job_mutex_;
    std::condition_variable job_cv_;
    uint64_t job_generation_ = 0;   // guarded by job_mutex_
    uint64_t job_seq_ = 0;          // guarded by job_mutex_
    uint64_t job_deadline_ms_ = 0;  // guarded by job_mutex_
    POINT job_pt_ = {};             // guarded by job_mutex_

    DragReleaseCallback drag_cb_;
    MouseDownCallback mouse_down_cb_;

    static MouseHook* s_instance;
};

} // namespace emebalachat
