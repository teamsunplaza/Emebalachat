#include "mouse_hook.hpp"
#include "diag_logger.hpp"
#include "hook.hpp" // Input P1-6: KeyboardHook::ResetImeCompositionMirror on real clicks
#include "hook_thread.hpp"
#include "ui/dpi.hpp"  // ISSUE-013: ui::MonitorDpiAtPoint for the drag threshold
#include "win32_input.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>

namespace emebalachat {

namespace {

// ISSUE-013 (v0.10.7): the Win32 half of the drag-release threshold. The LL
// hook delivers PHYSICAL pixels of the monitor under the cursor, so the
// system drag metrics must be read for THAT monitor's DPI:
// GetSystemMetricsForDpi (user32, Win10 1607+) returns the metric already
// expressed in that monitor's physical pixels. Resolved through
// GetProcAddress (the dpi.cpp shcore discipline) so an older host degrades
// to the DPI-unaware GetSystemMetrics instead of failing to load. Cost: one
// MonitorDpiAtPoint + two metric reads per drag RELEASE (never per move) -
// the hook thread already calls GetSystemMetrics freely for the double-click
// detection in the WM_LBUTTONDOWN branch.
int DragThresholdPixelsForPress(POINT press_pt) {
    const UINT dpi = ui::MonitorDpiAtPoint(press_pt);
    typedef int(WINAPI* GetSystemMetricsForDpiFn)(int, UINT);
    static GetSystemMetricsForDpiFn metrics_for_dpi = []() -> GetSystemMetricsForDpiFn {
        HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<GetSystemMetricsForDpiFn>(
                            ::GetProcAddress(user32, "GetSystemMetricsForDpi"))
                      : nullptr;
    }();
    if (metrics_for_dpi) {
        return MouseHook::DragThresholdFromMetrics(metrics_for_dpi(SM_CXDRAG, dpi),
                                                   metrics_for_dpi(SM_CYDRAG, dpi));
    }
    return MouseHook::DragThresholdFromMetrics(::GetSystemMetrics(SM_CXDRAG),
                                               ::GetSystemMetrics(SM_CYDRAG));
}

} // namespace

MouseHook* MouseHook::s_instance = nullptr;

MouseHook::MouseHook() {
    s_instance = this;
    hReadyEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

MouseHook::~MouseHook() {
    Stop();
    if (s_instance == this) {
        s_instance = nullptr;
    }
    if (hReadyEvent_) {
        ::CloseHandle(hReadyEvent_);
        hReadyEvent_ = nullptr;
    }
}

bool MouseHook::Start() {
    if (running_.exchange(true)) {
        return true;
    }

    // REQ-R09: persistent delayed-click worker. Spawned here (the main thread)
    // so the LL mouse callback NEVER constructs or joins a std::thread: the C2
    // regression (commit a6e06a7) joined the previous 60 ms delayed job inside
    // LowLevelMouseProc, blocking the hook thread past LowLevelHooksTimeout
    // until Windows force-removed WH_MOUSE_LL. The worker is joined
    // deterministically in Stop() so it can never outlive the object (the
    // pre-C2 use-after-free that motivated the join stays fixed - no detach).
    bool expected = false;
    if (delayed_worker_live_.compare_exchange_strong(expected, true)) {
        delayed_click_worker_ = std::jthread([this](std::stop_token st) {
            DelayedClickWorkerLoop(st);
        });
    }

    if (hReadyEvent_) {
        ::ResetEvent(hReadyEvent_);
    }
    // R1-14 (code review mimoflash F-05): clear the late-install stand-down and
    // the published handle for THIS attempt (identical contract to
    // KeyboardHook::Start).
    quit_requested_.store(false, std::memory_order_release);
    hHook_.store(nullptr, std::memory_order_release);

    thread_ = std::thread(&MouseHook::HookThreadProc, this);

    // R1-14: bounded install handshake (shared with KeyboardHook). It joins the
    // pump on every non-installed outcome and takes a late install down via
    // quit_requested_ + WM_QUIT, so there is no unbounded join and no leaked
    // hook.
    const HookInstallOutcome install =
        FinishLowLevelHookStart(hReadyEvent_, hHook_, hook_thread_id_, quit_requested_, thread_);
    if (install == HookInstallOutcome::kInstalled) {
        return true;
    }

    if (install == HookInstallOutcome::kReadyTimeout) {
        DIAG_F("HOOK/Start/002: mouse hook install exceeded the %lu ms ready window; pump stood down "
               "(R1-14: startup continues, no leaked hook)\n",
               static_cast<unsigned long>(kHookReadyWaitMs));
    }
    // The handshake already joined the pump; tear down the delayed-click worker
    // so the object is cleanly restartable.
    running_.store(false);
    if (delayed_click_worker_.joinable()) {
        delayed_click_worker_ = std::jthread{}; // request_stop + join
    }
    delayed_worker_live_.store(false, std::memory_order_release);
    return false;
}

void MouseHook::Stop() {
    if (!running_.exchange(false)) {
        return;
    }

    // R1-14: acquire load on the pump's release store (its first action).
    const DWORD hook_tid = hook_thread_id_.load(std::memory_order_acquire);
    if (hook_tid != 0) {
        ::PostThreadMessageW(hook_tid, WM_QUIT, 0, 0);
    }

    if (thread_.joinable()) {
        thread_.join();
    }

    // Join the delayed worker on the MAIN thread after the hook thread is down.
    // The move-assignment from an empty jthread performs request_stop() + join().
    // Worst-case duration: one poll step (kDelayedClickPollMs = 16 ms) plus an
    // already-entered callback (the R10-marshaled ShowAt path is microseconds).
    // running_ is already false, so the worker's predicate can no longer fire.
    if (delayed_click_worker_.joinable()) {
        delayed_click_worker_ = std::jthread{};
    }
    delayed_worker_live_.store(false, std::memory_order_release);
}

void MouseHook::SetDragReleaseCallback(DragReleaseCallback cb) {
    // Contract: registered at startup before Start(), like the KeyboardHook
    // callbacks. The delayed worker reads drag_cb_ only while running_.
    drag_cb_ = std::move(cb);
}

void MouseHook::SetMouseDownCallback(MouseDownCallback cb) {
    mouse_down_cb_ = std::move(cb);
}

void MouseHook::SetMouseWheelCallback(MouseWheelCallback cb) {
    // Same registration contract as the callbacks above (startup, before
    // Start(); read on the hook thread only).
    mouse_wheel_cb_ = std::move(cb);
}

void MouseHook::SetEnabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        // Input P2-4 (v0.10.5): a disable mid-press used to strand
        // is_lbutton_down_ = true: the matching WM_LBUTTONUP early-returned at
        // the enabled_ gate below (before clearing the flag), so the next
        // SetEnabled(true) treated ordinary WM_MOUSEMOVE as a drag completion.
        // The click count dies with the gesture too - a paused hook must not
        // resurrect a stale double-click for the next press.
        is_lbutton_down_.store(false, std::memory_order_relaxed);
        click_count_.store(0, std::memory_order_relaxed);
    }
}

void MouseHook::HookThreadProc() {
    // C4 (session 260910_0007): body extracted verbatim to the shared
    // RunLowLevelHookPump (src/hook_thread.hpp) — same pump as the keyboard
    // hook, identical modulo hook id + callback (see the byte-comparison note
    // and preserved invariants there). R1-15 (priority boost) and R1-14
    // (late-install takeover) live in that shared pump, so both hook threads
    // inherit them from one site.
    RunLowLevelHookPump(WH_MOUSE_LL, MouseHook::LowLevelMouseProc,
                        hReadyEvent_, running_, hHook_, hook_thread_id_, quit_requested_);
}

// ---- REQ-R09 (audit §3.3): delayed multi-click dispatch, hook-thread-safe ----

void MouseHook::ArmDelayedClick(uint64_t seq, POINT pt) {
    if (!delayed_worker_live_.load(std::memory_order_acquire)) {
        // Worker spawn failed (resource exhaustion) or Start() never ran.
        // Last-resort synchronous handoff: bounded because the R10-marshaled
        // main.cpp handler only does a PostMessage here. Losing the double-click
        // event silently would be worse.
        DIAG_F("MOUSE_HOOK/ArmDelayedClick/001: worker not live; firing inline\n");
        if (drag_cb_) {
            drag_cb_(pt.x, pt.y);
        }
        return;
    }

    // Hook-thread budget: GetTickCount64 + up to 4 uncontended try_lock attempts
    // (the worker holds job_mutex_ only for microsecond snapshots - it releases
    // the lock via wait_for and settles OUTSIDE it). No Sleep, no join, no
    // allocation. If every attempt fails the event still must not be lost, so
    // fire inline rather than drop.
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (job_mutex_.try_lock()) {
            job_seq_ = seq;
            job_pt_ = pt;
            job_deadline_ms_ = MultiClickDeadlineMs(::GetTickCount64());
            ++job_generation_;
            job_mutex_.unlock();
            job_cv_.notify_one(); // outside the lock: bounded, never blocks
            return;
        }
        ::SwitchToThread(); // user-mode yield only; never Sleep on a hook thread
    }

    DIAG_F("MOUSE_HOOK/ArmDelayedClick/002: job mutex contended 4x; firing inline\n");
    if (drag_cb_) {
        drag_cb_(pt.x, pt.y);
    }
}

void MouseHook::DelayedClickWorkerLoop(std::stop_token st) {
    std::unique_lock<std::mutex> lk(job_mutex_);
    uint64_t seen_generation = 0;
    for (;;) {
        job_cv_.wait_for(lk, std::chrono::milliseconds(kDelayedClickPollMs), [this, &st, seen_generation]() {
            return seen_generation != job_generation_ || st.stop_requested();
        });
        if (st.stop_requested()) {
            break;
        }
        if (seen_generation == job_generation_) {
            continue; // timed backstop while idle - keep waiting, zero cost
        }
        seen_generation = job_generation_;
        const uint64_t captured_seq = job_seq_;
        const uint64_t deadline_ms = job_deadline_ms_;
        const POINT pt = job_pt_;
        lk.unlock(); // mutex NEVER held across the settle window or callback

        // Settle loop on the worker thread. A newer physical click bumped
        // click_seq_, so ShouldFireDelayedClick goes false and this job
        // no-ops; the newer click's own arm (bigger generation) wakes the next
        // iteration. Stop() sets running_ false, which also blocks firing.
        while (!st.stop_requested()) {
            const uint64_t now_ms = ::GetTickCount64();
            if (ShouldFireDelayedClick(
                    captured_seq,
                    click_seq_.load(std::memory_order_acquire),
                    running_.load(std::memory_order_acquire),
                    now_ms,
                    deadline_ms)) {
                if (drag_cb_) {
                    drag_cb_(pt.x, pt.y);
                }
                break;
            }
            if (now_ms >= deadline_ms) {
                break; // settle window expired without firing = invalidated
            }
            ::Sleep(kDelayedClickPollMs);
        }
        lk.lock();
    }
}

LRESULT CALLBACK MouseHook::LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION || !s_instance) {
        return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);

    // Bypass synthetic input events
    if (ms->dwExtraInfo == EXTRA_INFO_MARKER) {
        return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    // Input P1-6 (v0.10.5): any real click retires the keyboard hook's IME
    // composition mirror. A click commits or dismisses an open IME composition
    // without the keyboard hook ever seeing a composition-finalising key, so
    // the mirror would otherwise stay stuck true and silently gate out the
    // NEXT bare Enter (the "typed text never translates" failure). Placed
    // BEFORE the enabled_ gate: mirror staleness is not translation-state
    // dependent, and pausing/re-enabling must not inherit a stale mirror.
    // Never swallows the event; one relaxed atomic store.
    if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN) {
        KeyboardHook::ResetImeCompositionMirror();
    }

    // REQ-002: wheel forwarding for the WS_EX_NOACTIVATE tooltip happens
    // BEFORE the enabled_ gate: reading an already-shown tooltip is unrelated
    // to the translation-active state, so scrolling must work while paused.
    // FORWARD-ONLY (plan §2.1 risk 1): we always CallNextHookEx and never
    // swallow the event, protecting both the LowLevelHooksTimeout budget and
    // the underlying app's own wheel handling. The callback contract is
    // PostMessage-cheap (see main.cpp wiring).
    if (wParam == WM_MOUSEWHEEL) {
        if (s_instance->mouse_wheel_cb_) {
            s_instance->mouse_wheel_cb_(ms->pt.x, ms->pt.y,
                                        static_cast<int>(GET_WHEEL_DELTA_WPARAM(wParam)));
        }
        return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    // D2 (debug report A10): outside-click dismissal moves in front of the
    // enabled_ translation gate, mirroring the wheel branch's deliberate
    // Batch-2 pre-gate placement above. Dismissing an already-visible
    // tooltip / About / drag icon is unrelated to the translation-active
    // state, so a click outside must dismiss those popups while paused. The
    // event is still NEVER swallowed (CallNextHookEx at the bottom keeps the
    // LowLevelHooksTimeout budget and target-app input intact) and the
    // WM_LBUTTONDOWN tracking below (double-click count, drag origin,
    // click_seq_ invalidation) stays gated - pausing keeps suspending every
    // translation trigger path exactly as before. Note the hermeticity claim
    // in TestMouseHookDebounce ("proc early-returns while disabled") still
    // holds for THAT fixture: it registers no mouse-down callback, so this
    // branch is a null-cb no-op there and physical clicks cannot touch the
    // debounce state, which lives behind the gate.
    if (wParam == WM_LBUTTONDOWN && s_instance->mouse_down_cb_) {
        s_instance->mouse_down_cb_(ms->pt.x, ms->pt.y);
    }

    if (!s_instance->enabled_.load(std::memory_order_relaxed)) {
        return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    if (wParam == WM_LBUTTONDOWN) {
        // REQ-052 (audit P1): never let a pen/touch press (0xFF515700
        // dwExtraInfo signature) arm the drag origin or the double-click
        // tracking below - without this, the matching touch release trips the
        // drag gate and spawns the drag-translate icon on every pen
        // stroke or touch scroll. Real mouse input has dwExtraInfo == 0 and
        // never matches. Pure touch sequences never enter the state machine
        // because the press is filtered here, before is_lbutton_down_ is set.
        // The signature lives in bits 8-31 of the LOW dword of the 64-bit
        // dwExtraInfo (MS injection contract); the DWORD predicate + mask
        // never examine anything above bit 31, so the low-dword cast is the
        // exact filter (C4244: no implicit ULONG_PTR -> DWORD narrowing).
        if (IsFromTouchInjection(static_cast<DWORD>(ms->dwExtraInfo))) {
            return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
        }
        DWORD now = ms->time;
        int d_last_x = std::abs(ms->pt.x - s_instance->last_click_pt_.x);
        int d_last_y = std::abs(ms->pt.y - s_instance->last_click_pt_.y);
        UINT max_dbl_time = ::GetDoubleClickTime();
        int max_dbl_cx = ::GetSystemMetrics(SM_CXDOUBLECLK);
        int max_dbl_cy = ::GetSystemMetrics(SM_CYDOUBLECLK);

        if ((now - s_instance->last_click_time_ <= max_dbl_time) &&
            (d_last_x <= max_dbl_cx) && (d_last_y <= max_dbl_cy)) {
            s_instance->click_count_.fetch_add(1, std::memory_order_relaxed);
        } else {
            s_instance->click_count_.store(1, std::memory_order_relaxed);
        }

        s_instance->last_click_time_ = now;
        s_instance->last_click_pt_ = ms->pt;
        // REQ-R09 invalidation: every new physical click bumps the sequence,
        // so any in-flight delayed job captured at an older sequence becomes
        // stale on its own thread. This is the join-free debounce.
        s_instance->click_seq_.fetch_add(1, std::memory_order_acq_rel);

        s_instance->is_lbutton_down_.store(true, std::memory_order_relaxed);
        s_instance->lbutton_down_pt_ = ms->pt;
        // D2: mouse_down_cb_ moved to the pre-gate dispatch above (dismissal
        // must work while paused). No second invocation here.
    } else if (wParam == WM_MOUSEMOVE) {
        // O(1) performance guarantee: do no heavy computation during mouse movement
        return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
    } else if (wParam == WM_LBUTTONUP) {
        // REQ-052 (audit P1): a pen/touch release must not reach the drag gate
        // or the multi-click settle arm below (both fire drag_cb). This also
        // covers the mixed case of a real mouse press followed by a touch
        // release, which would otherwise measure drag distance from the real
        // press origin. Low-dword cast: see the WM_LBUTTONDOWN guard above.
        if (IsFromTouchInjection(static_cast<DWORD>(ms->dwExtraInfo))) {
            return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
        }
        if (s_instance->is_lbutton_down_.load(std::memory_order_relaxed)) {
            s_instance->is_lbutton_down_.store(false, std::memory_order_relaxed);

            // 64-bit delta math (Input P2-4, v0.10.5): the 32-bit int square
            // overflowed for cross-screen drags, wrapping dist_sq negative and
            // misfiring (or never firing) the drag gate.
            const int64_t dx = static_cast<int64_t>(ms->pt.x) - s_instance->lbutton_down_pt_.x;
            const int64_t dy = static_cast<int64_t>(ms->pt.y) - s_instance->lbutton_down_pt_.y;
            const int64_t dist_sq = dx * dx + dy * dy;

            // ISSUE-013 (v0.10.7): the threshold comes from the system drag
            // metrics for the press monitor's DPI (SM_CXDRAG / SM_CYDRAG max,
            // GetSystemMetricsForDpi), replacing the hard-coded 15 px that
            // ignored both the user's setting and high-DPI panels.
            const int drag_threshold =
                DragThresholdPixelsForPress(s_instance->lbutton_down_pt_);
            const int64_t threshold_sq =
                static_cast<int64_t>(drag_threshold) * drag_threshold;

            // Trigger drag release when Euclidean drag distance exceeds the
            // system drag threshold
            if (dist_sq >= threshold_sq) {
                // Input P2-4 (v0.10.5): a completed drag is not a click. The
                // press had already incremented click_count_; leaving it armed
                // made the NEXT press within GetDoubleClickTime() count as a
                // double-click and fire a false delayed selection. Reset it
                // with the gesture.
                s_instance->click_count_.store(0, std::memory_order_relaxed);
                if (s_instance->drag_cb_) {
                    s_instance->drag_cb_(ms->pt.x, ms->pt.y);
                }
            } else if (s_instance->click_count_.load(std::memory_order_relaxed) >= 2) {
                // Double-click (word selection) or triple-click (paragraph
                // selection): 60 ms settle lets the target app complete its
                // selection highlight. REQ-R09: hand off to the persistent
                // delayed worker via the non-blocking ArmDelayedClick seam -
                // NO thread join here (the C2 regression, audit §3.3).
                const uint64_t current_seq =
                    s_instance->click_seq_.fetch_add(1, std::memory_order_acq_rel) + 1;
                s_instance->ArmDelayedClick(current_seq, ms->pt);
            }
        }
    }

    return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
}

} // namespace emebalachat
