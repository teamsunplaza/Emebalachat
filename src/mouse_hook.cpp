#include "mouse_hook.hpp"
#include "diag_logger.hpp"
#include "win32_input.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>

namespace emebalachat {

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

    thread_ = std::thread(&MouseHook::HookThreadProc, this);

    if (hReadyEvent_) {
        ::WaitForSingleObject(hReadyEvent_, 2000);
    }

    if (!hHook_) {
        // Hook install failed: HookThreadProc returned without pumping; join the
        // finished thread (joinable std::thread dtor would terminate), then tear
        // down the delayed-click worker so the object is cleanly restartable.
        if (thread_.joinable()) {
            thread_.join();
        }
        running_.store(false);
        if (delayed_click_worker_.joinable()) {
            delayed_click_worker_ = std::jthread{}; // request_stop + join
        }
        delayed_worker_live_.store(false, std::memory_order_release);
        return false;
    }

    return true;
}

void MouseHook::Stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (hook_thread_id_ != 0) {
        ::PostThreadMessageW(hook_thread_id_, WM_QUIT, 0, 0);
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

void MouseHook::HookThreadProc() {
    hook_thread_id_ = ::GetCurrentThreadId();
    HINSTANCE hInst = ::GetModuleHandleW(nullptr);

    hHook_ = ::SetWindowsHookExW(
        WH_MOUSE_LL,
        MouseHook::LowLevelMouseProc,
        hInst,
        0
    );

    if (hReadyEvent_) {
        ::SetEvent(hReadyEvent_);
    }

    if (!hHook_) {
        running_.store(false);
        return;
    }

    MSG msg = {};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    if (hHook_) {
        ::UnhookWindowsHookEx(hHook_);
        hHook_ = nullptr;
    }
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
        if (s_instance->is_lbutton_down_.load(std::memory_order_relaxed)) {
            s_instance->is_lbutton_down_.store(false, std::memory_order_relaxed);

            int dx = ms->pt.x - s_instance->lbutton_down_pt_.x;
            int dy = ms->pt.y - s_instance->lbutton_down_pt_.y;
            int dist_sq = dx * dx + dy * dy;

            // Trigger drag release when Euclidean drag distance exceeds 15 pixels
            if (dist_sq >= (15 * 15)) {
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
