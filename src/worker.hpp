#pragma once

#include "config.hpp"
#include "engine.hpp"
#include "ui/badge.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

namespace emebalachat {

struct PipelineTask {
    bool is_shift_enter = false;
    HWND target_hwnd = nullptr;
};

// REQ-R03 (Batch D1) path-matrix predicate - single source of truth, shared by
// worker.cpp (pinned there with static_assert at each call decision) and the
// unit tests. ExecuteTask attempts a paste only when the translation is
// non-empty and differs from the source; therefore every no-paste outcome -
// translated empty (engine failure / consent block), translated == source, or
// paste cancelled by the H1 foreground guard - collapses to paste_succeeded
// == false and MUST release the block selection exactly once (VK_RIGHT),
// otherwise the user's next keystroke destroys the whole highlighted message
// (audit §2.2 text evaporation). The ONLY path that skips the release is a
// successful paste, where Ctrl+V consumed the selection itself.
constexpr bool SelectionReleaseRequired(bool paste_succeeded) {
    return !paste_succeeded;
}

// R5 (Debug-Surgical): the Enter-path empty-capture verdict, as one pure
// predicate so worker.cpp and the unit tests assert on ONE definition
// (same discipline as SelectionReleaseRequired). True = the exact case the
// user reported ("Enter sent my text untranslated"): the worker intercepted a
// bare Enter, but the capture came back EMPTY - not a smart-bypass (that
// means the text already matched the target and passing through is the
// product's contract), and not a re-routed keystroke. For empty capture the
// original text is still sitting in the target app; sending Enter would
// submit it untranslated and SILENTLY - the R1 "tooltip must always land"
// complaint. So: hold the send and surface a TooltipNoSelection-style notice
// instead (wired via SetEmptyCaptureCallback, marshaled to the GUI thread
// by the already thread-safe ShowMessageThreadSafe seam).
constexpr bool EmptyCaptureNeedsHold(bool captured_empty, bool smart_bypass) {
    return captured_empty && !smart_bypass;
}

class PipelineWorker {
public:
    PipelineWorker(AppConfig& config, TranslationManager& engine, FloatingBadge& badge);
    ~PipelineWorker();

    void Start();
    void Stop();

    // Enqueues a translation pipeline task if not already busy.
    // Returns true if task was accepted, false if currently busy.
    bool PostTask(bool is_shift_enter, HWND target_hwnd = nullptr);

    // Returns true if the worker is actively executing a task.
    bool IsBusy() const { return is_busy_.load(std::memory_order_relaxed); }

    // R5 (Debug-Surgical): called (on the worker thread, never the hook
    // thread) exactly when the Enter path takes the new hold-and-notice
    // empty-capture branch (see EmptyCaptureNeedsHold). The registered
    // callback must marshal to the GUI thread itself - main.cpp registers a
    // wrapper over TooltipWindow::ShowMessageThreadSafe, which is already the
    // REQ-R10 thread-safe seam. Follows the existing SetXxxCallback contract:
    // registered once at startup BEFORE Start(), read-only afterwards
    // (identical discipline to KeyboardHook::SetDoubleCtrlCCallback), so the
    // worker thread reads it without a lock.
    void SetEmptyCaptureCallback(std::function<void()> cb) {
        empty_capture_cb_ = std::move(cb);
    }

private:
    void WorkerLoop(std::stop_token stop_token);
    void ExecuteTask(const PipelineTask& task);

    AppConfig& config_;
    TranslationManager& engine_;
    FloatingBadge& badge_;

    std::atomic<bool> is_busy_{false};
    std::atomic<bool> running_{false};

    // R5: set once at startup via SetEmptyCaptureCallback (see contract there).
    std::function<void()> empty_capture_cb_;

    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::queue<PipelineTask> queue_;
    std::jthread thread_;
};

} // namespace emebalachat
