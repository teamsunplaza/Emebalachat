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
// Invariants that MUST stay (do not "improve" them here):
//   - SetEvent fires even when SetWindowsHookExW FAILED, so the 2 s
//     WaitForSingleObject in each Start() unblocks and observes hHook_ == null
//     instead of timing out.
//   - WM_QUIT (posted by Stop() via PostThreadMessageW) makes GetMessageW
//     return 0/-1 and exits the loop; no other quit path exists.
//   - On install failure the pump returns without unhooking anything; the
//     owning Start() joins the finished thread and tears down its own workers.

#include <atomic>
#include <windows.h>

namespace emebalachat {

// Runs the LL hook install -> message pump -> unhook sequence on the calling
// thread. All state is written through the references so each owner class keeps
// its own members (and its Start()/Stop() handshake logic) unchanged.
inline void RunLowLevelHookPump(UINT hookId,
                                HOOKPROC proc,
                                HANDLE hReadyEvent,
                                std::atomic<bool>& running,
                                HHOOK& hookHandle,
                                DWORD& hookThreadId) {
    hookThreadId = ::GetCurrentThreadId();
    HINSTANCE hInst = ::GetModuleHandleW(nullptr);

    hookHandle = ::SetWindowsHookExW(hookId, proc, hInst, 0);

    if (hReadyEvent) {
        ::SetEvent(hReadyEvent);
    }

    if (!hookHandle) {
        running.store(false);
        return;
    }

    MSG msg = {};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    if (hookHandle) {
        ::UnhookWindowsHookEx(hookHandle);
        hookHandle = nullptr;
    }
}

} // namespace emebalachat
