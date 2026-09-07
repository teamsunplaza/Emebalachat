#pragma once

#include "config.hpp"
#include "engine.hpp"
#include "ui/badge.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
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

// REQ-034 F3-B (design 173700_architect §2.2.1, user rule: "엔터 치면 자동으로
// 입력되는 것을 체크할 때와 안 체크할 때의 차이가 분명히 있어야 한다"): a retry
// Enter fired right after a SUCCESSFUL paste is a re-translation intent, not
// a "no selection" mistake. The F3 log signature (emebalachat_260907171452
// L432/563/601): stored offset == caret -> EM_SETSEL(last,last) empty range
// -> Ctrl+C changes nothing -> 180 ms stale-refuse -> EMPTY capture -> the R5
// hold above would surface a FALSE TooltipNoSelection notice repeatedly. This
// pure predicate is the time-window gate applied at the worker's
// EmptyCaptureNeedsHold entry: inside the window after the last successful
// paste, the notice is suppressed and Enter is silently delivered to the app
// (ReleaseSelectionOnce + SendEnterKey). Outside the window - no paste
// recorded (sentinel 0) or window elapsed - the general empty Enter keeps the
// existing hold_send + notice behavior EXACTLY (constraint C-5;
// EmptyCaptureNeedsHold itself is untouched). Same shared-definition
// discipline as SelectionReleaseRequired / EmptyCaptureNeedsHold: worker.cpp
// and the unit tests assert on ONE definition.
//
// last_paste_ms uses 0 as the never-pasted sentinel: GetTickCount64() is
// effectively never 0 after system uptime exceeds one millisecond, so 0 is
// unambiguous and keeps "no paste history" out of the window. Non-monotonic
// pairs (now < last, only possible with a clock anomaly) return false so the
// gate can never unsigned-underflow into a bogus "inside window".
constexpr uint64_t kPasteEmptySuppressMs = 2000;

constexpr bool PasteWindowSuppressesNotice(uint64_t now_ms, uint64_t last_paste_ms) {
    if (last_paste_ms == 0) return false;
    if (now_ms < last_paste_ms) return false;
    return (now_ms - last_paste_ms) <= kPasteEmptySuppressMs;
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

    // REQ-034 F3-B: GetTickCount64() stamp of the last SUCCESSFUL paste
    // (pasted == true branch in ExecuteTask). Read by the empty-capture
    // paste-window gate at the EmptyCaptureNeedsHold entry (see
    // PasteWindowSuppressNotice contract in this header). Written and read
    // only on the pipeline worker thread - the atomic is defensive
    // (design §2.2.1), so relaxed ordering is sufficient. 0 = never pasted.
    std::atomic<uint64_t> last_paste_ms_{0};

    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::queue<PipelineTask> queue_;
    std::jthread thread_;
};

} // namespace emebalachat
