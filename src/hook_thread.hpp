#pragma once

// C4 (session 260910_0007, catalog W4): shared low-level hook message pump.
//
// Verified provenance: KeyboardHook::HookThreadProc (src/hook.cpp) and
// MouseHook::HookThreadProc (src/mouse_hook.cpp) were byte-compared line by
// line (whitespace-normalized) before extraction: 26 non-empty lines each,
// with exactly THREE deltas — the member-function name, the hook id
// (WH_KEYBOARD_LL vs WH_MOUSE_LL) and the callback symbol
// (LowLevelKeyboardProc vs LowLevelMouseProc). Everything else (thread-id
// capture, SetEvent BEFORE the install-failure check, the failure path
// `running=false; return;`, the GetMessageW/Translate/Dispatch loop terminated
// by WM_QUIT via PostThreadMessageW in Stop(), and the trailing unhook +
// null-out) was identical — including the absence of any logging. This helper
// therefore preserves both pumps exactly; it is behavior-preserving by
// construction (audit report 050600 §C4).
//
// R1-14/R1-15 (v0.10.4 sweep, 261029_0001) are the ONLY post-C4 deltas, and
// they are additive on top of the C4 body:
//   - R1-15: the pump thread raises itself to THREAD_PRIORITY_HIGHEST before
//     installing. LL hooks are starved-first candidates under CPU load, and an
//     over-budget callback makes Windows REMOVE the hook (LowLevelHooksTimeout)
//     while our HHOOK stays valid. The pump body performs no heavy work (the
//     REQ-R06 double-Ctrl+C body and the mouse delayed-click job both run on
//     their own worker threads), so this only protects callback latency.
//   - R1-14: the message queue is materialized BEFORE the handle publication,
//     the handle is published through an atomic, and the pump honours a
//     quit request observed after a late install instead of entering a pump
//     nobody would ever stop. The install handshake itself moved to
//     FinishLowLevelHookStart() below (bounded wait, never an unbounded join).
//
// Invariants that MUST stay (do not "improve" them here):
//   - SetEvent fires even when SetWindowsHookExW FAILED, so the bounded
//     WaitForSingleObject in each Start() unblocks and observes the handle as
//     null instead of timing out.
//   - WM_QUIT (posted by Stop() via PostThreadMessageW) makes GetMessageW
//     return 0/-1 and exits the loop; no other quit path exists.
//   - On install failure the pump returns without unhooking anything; the
//     owning Start() joins the finished thread and tears down its own workers.

#include <atomic>
#include <cstdint>
#include <thread>
#include <windows.h>

namespace emebalachat {

// R1-14: install handshake budget. The pre-fix code waited a bare 2000 ms on
// the ready event and then joined whatever thread it found; both values are
// preserved here as named constants so the timeout is visible at the call site
// and pinnable in the tests.
inline constexpr DWORD kHookReadyWaitMs = 2000;
// Late-install unwind budget: the quit request is retried a few times because a
// PostThreadMessageW can only land once the pump's message queue exists (the
// pump creates it as its first action, before it publishes its thread id).
// Worst case added to a timed-out start: attempts * retry = 100 ms.
inline constexpr int kHookQuitPostAttempts = 5;
inline constexpr DWORD kHookQuitPostRetryMs = 20;

enum class HookInstallOutcome : uint8_t {
    kInstalled,     // hook registered and pumping
    kInstallFailed, // SetWindowsHookExW returned null; pump already returned
    kReadyTimeout,  // install did not finish in the window; pump stood down
};

// Runs the LL hook install -> message pump -> unhook sequence on the calling
// thread. All state is written through the references so each owner class keeps
// its own members (and its Start()/Stop() handshake logic) unchanged.
inline void RunLowLevelHookPump(UINT hookId,
                                HOOKPROC proc,
                                HANDLE hReadyEvent,
                                std::atomic<bool>& running,
                                std::atomic<HHOOK>& hookHandle,
                                std::atomic<DWORD>& hookThreadId,
                                std::atomic<bool>& quitRequested) {
    hookThreadId.store(::GetCurrentThreadId(), std::memory_order_release);

    // R1-14: materialize this thread's message queue NOW. PostThreadMessageW
    // (the late-install quit below, and Stop()'s WM_QUIT) fails for a thread
    // that has no queue yet; doing it first makes every later post land.
    MSG probe = {};
    ::PeekMessageW(&probe, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    // R1-15 (env research F2): LL hook pumps are a documented starvation
    // candidate — Windows calls the hook within LowLevelHooksTimeout and
    // SILENTLY removes it when that budget is blown, leaving a live thread with
    // a valid-looking HHOOK ("live-but-dead"). The pump does no heavy work, so
    // the highest priority class only bounds the callback's scheduling latency.
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    HHOOK installed = ::SetWindowsHookExW(hookId, proc, hInst, 0);
    hookHandle.store(installed, std::memory_order_release);

    if (hReadyEvent) {
        ::SetEvent(hReadyEvent);
    }

    if (!installed) {
        running.store(false);
        return;
    }

    // R1-14: late-success takeover. A Start() whose bounded wait expired has
    // already asked this pump to stand down (quitRequested) and posted WM_QUIT;
    // honour the flag instead of entering a GetMessageW loop nobody would ever
    // quit — the pre-fix infinite join.
    if (quitRequested.load(std::memory_order_acquire)) {
        ::UnhookWindowsHookEx(installed);
        hookHandle.store(nullptr, std::memory_order_release);
        running.store(false);
        return;
    }

    MSG msg = {};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    if (HHOOK current = hookHandle.exchange(nullptr, std::memory_order_acq_rel)) {
        ::UnhookWindowsHookEx(current);
    }
}

// R1-14 (code review mimoflash F-05): bounded, deterministic install handshake
// shared by KeyboardHook::Start() and MouseHook::Start(). Pre-fix, Start()
// waited 2000 ms for the ready event, read the handle without synchronization
// and, on a timeout, joined the hook thread — which could later complete
// SetWindowsHookExW, publish a handle and enter the pump forever, hanging
// startup (and every REQ-R14 Reinstall()) on the GUI thread.
//
// Post-fix contract:
//   - the ready event bounds the install; the handle is read with acquire on a
//     store the pump made before SetEvent, so the outcome is never torn;
//   - with no ready event handle at all (CreateEventW failed - the only way it
//     can be null), the same window is spent polling the published handle, so
//     the handshake is bounded even then (the pre-fix `if (hReadyEvent_)` form
//     skipped its wait and could join a thread that was still starting up);
//   - a ready-event hit with a null handle means the install FAILED (the pump's
//     only early return), so joining there is immediate;
//   - a timeout takes the pump down instead of waiting for it: quit request,
//     WM_QUIT post (retried while the queue may still be materializing), then a
//     join that is bounded by construction — either the pump observes the quit
//     flag after its install returns, or it retrieves the posted WM_QUIT and
//     exits its loop, unhooking its own handle in both cases;
//   - the caller tears down its own worker threads on any non-kInstalled
//     outcome. Nothing is leaked and nothing hangs.
inline HookInstallOutcome FinishLowLevelHookStart(HANDLE hReadyEvent,
                                                  std::atomic<HHOOK>& hookHandle,
                                                  std::atomic<DWORD>& hookThreadId,
                                                  std::atomic<bool>& quitRequested,
                                                  std::thread& thread) {
    bool ready_hit = false;
    if (hReadyEvent) {
        ready_hit = ::WaitForSingleObject(hReadyEvent, kHookReadyWaitMs) == WAIT_OBJECT_0;
    } else {
        const ULONGLONG deadline = ::GetTickCount64() + kHookReadyWaitMs;
        for (;;) {
            if (hookHandle.load(std::memory_order_acquire) != nullptr) {
                ready_hit = true;
                break;
            }
            if (::GetTickCount64() >= deadline) {
                break;
            }
            ::Sleep(kHookQuitPostRetryMs);
        }
    }

    if (ready_hit) {
        if (hookHandle.load(std::memory_order_acquire) != nullptr) {
            return HookInstallOutcome::kInstalled;
        }
        // Ready signalled with no handle: SetWindowsHookExW failed and the pump
        // has already returned, so this join cannot block on a live pump.
        if (thread.joinable()) {
            thread.join();
        }
        return HookInstallOutcome::kInstallFailed;
    }

    quitRequested.store(true, std::memory_order_release);
    for (int attempt = 0; attempt < kHookQuitPostAttempts; ++attempt) {
        // Re-read the id every attempt: the pump stores it as its first action,
        // which may land after our wait expired.
        const DWORD tid = hookThreadId.load(std::memory_order_acquire);
        if (tid != 0 && ::PostThreadMessageW(tid, WM_QUIT, 0, 0)) {
            break;
        }
        // The queue may not exist yet (pump between its thread-id store and its
        // PeekMessageW). Wait on the ready event so a pump that finishes the
        // install wakes us immediately; otherwise back off a bounded step.
        if (hReadyEvent) {
            if (::WaitForSingleObject(hReadyEvent, kHookQuitPostRetryMs) == WAIT_OBJECT_0) {
                break;
            }
        } else {
            ::Sleep(kHookQuitPostRetryMs);
        }
    }
    if (thread.joinable()) {
        thread.join();
    }
    return HookInstallOutcome::kReadyTimeout;
}

} // namespace emebalachat
