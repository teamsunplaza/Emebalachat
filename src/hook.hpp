#pragma once

#include "config.hpp"
#include "ui/badge.hpp"
#include "ui/tray.hpp"
#include "worker.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <thread>
#include <windows.h>
#include <wtsapi32.h> // WM_WTSSESSION_CHANGE / WTS_SESSION_* / registration APIs

namespace emebalachat {

// ---- REQ-R17 (audit §5 latent item 5): IME composition gating ----
//
// The cross-thread IMM probe ForegroundImeComposing() lives in win32_input.hpp
// (worker-thread-only: ImmGetContext SendMessage's the target thread and must
// NEVER run inside the LL hook - that is the REQ-R06 LowLevelHooksTimeout
// hazard class). Inside the hook, composition is tracked by the pure
// ImeMirrorNext state machine below.

// Pure gate for the Enter interception: a keydown reaching the hook's Enter
// branch must be VK_RETURN (IME-processing keys already arrive as
// VK_PROCESSKEY 0xE5 and never reach the branch), the hook active, the
// pipeline worker idle, and NO composition in progress per the hook-local
// VK_PROCESSKEY mirror. Shared by LowLevelKeyboardProc and the unit tests -
// one definition.
constexpr bool EnterTranslationAllowed(bool vk_is_return, bool active, bool worker_busy,
                                       bool ime_composing) {
    return vk_is_return && active && !worker_busy && !ime_composing;
}

// ---- S2 (Shift+Enter newline) gate: bare Enter vs Shift+Enter ----
//
// Product semantics (VP directive, R4): a BARE Enter triggers send-and-replace,
// while Shift+Enter must pass through UNTOUCHED so the target app inserts a
// newline (chat-app convention). The R3-era code never checked the Shift
// modifier in the VK_RETURN branch: with the hook active, ANY Enter (bare or
// Shift-held) was swallowed into the pipeline, so Shift+Enter could never
// produce a newline. This pure predicate is the single source of truth for the
// modifier discrimination, shared by LowLevelKeyboardProc and the unit tests.
//
// Returns true only when the Enter may be intercepted for send-and-replace:
// the base EnterTranslationAllowed conditions hold AND Shift is NOT held.
// Ctrl and Alt/Win are already handled upstream in the hook (Ctrl+Shift+Enter
// toggles auto-send, Ctrl+Enter passes through, Alt/Win combos pass through),
// so this seam only needs to discriminate the Shift case on top of the base
// gate. Keeping it pure lets the full modifier matrix be pinned headlessly.
constexpr bool EnterSendReplaceAllowed(bool vk_is_return, bool active, bool worker_busy,
                                       bool ime_composing, bool shift) {
    return EnterTranslationAllowed(vk_is_return, active, worker_busy, ime_composing) && !shift;
}

// Hook-local IME composition mirror, updated on every REAL (non-synthetic)
// keydown with O(1) relaxed-atomic traffic and ZERO cross-thread messaging.
//
// Ground truth (documented LL-hook behavior): any key the active IME
// intercepts is delivered to LowLevelKeyboardProc with vkCode VK_PROCESSKEY
// (0xE5). The Korean IME assembles jamo through such intercepted keys, but it
// LETS ENTER THROUGH as VK_RETURN even mid-composition (chat apps rely on it
// to "commit + send") - which is exactly the key our Enter branch hijacks, so
// the vkCode alone cannot detect that case. The mirror does:
//   intercepted key (VK_PROCESSKEY)      -> composition active
//   plain editing key reaching us        -> the IME is NOT intercepting it, so
//     (Esc/Tab/Back/Delete/arrows/Home/End) no composition can be alive against
//                                          it; clear the flag
//   anything else (incl. VK_RETURN)      -> keep state: the Enter branch reads
//                                          the mirror BEFORE deciding and owns
//                                          clearing it afterwards, so an
//                                          Enter that finalises a composition
//                                          is still gated on THIS press.
// Fail-open bias: a stale true costs ONE missed translation (Enter passes
// through and the app commits+sends normally); a false negative degrades to
// the audited pre-R17 behavior.
constexpr bool ImeMirrorNext(bool composing, UINT vk_code) {
    if (vk_code == VK_PROCESSKEY) {
        return true;
    }
    switch (vk_code) {
        // Composition-finalising editing keys: when one of these reaches the
        // hook as a REAL vk (not VK_PROCESSKEY), the IME demonstrably did NOT
        // consume it, so no composition can be alive against it. Clear.
        case VK_ESCAPE:
        case VK_TAB:
        case VK_BACK:
        case VK_DELETE:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_HOME:
        case VK_END:
        // S2/R5 hardening: the vertical / page navigation keys. A composition
        // cannot survive these either (the IME would have intercepted them as
        // VK_PROCESSKEY if it were live). The previous "keep state" default
        // for these meant a composition that ended without a tracked clear key
        // (e.g. the user clicked away, or the app/Electron swallowed the
        // commit) could leave the mirror stuck true and silently gate out the
        // NEXT bare Enter - the exact "typed text never translates" failure.
        case VK_UP:
        case VK_DOWN:
        case VK_PRIOR: // Page Up
        case VK_NEXT:  // Page Down
            return false;
        default:
            return composing;
    }
}

// ---- REQ-R14 (audit §5 latent item 2): hook lifecycle events policy ----
//
// Pure classifier for the controller-window messages that mark a desktop
// lifecycle transition after which the low-level hooks must be verified and
// (cheaply, deterministically) re-registered:
//   - WM_POWERBROADCAST + PBT_APMRESUMEAUTOMATIC / PBT_APMRESUMESUSPEND:
//     system came back from Sleep/Standby.
//   - WM_WTSSESSION_CHANGE + WTS_SESSION_UNLOCK: session unlocked (Win+L,
//     Ctrl+Alt+Del). The secure-desktop (UAC consent) trip itself has no
//     message; the periodic health-check watchdog timer covers its edge cases.
// Everything else (including PBT_APMSUSPEND, SESSION_LOCK - reinstalling INTO
// a locked desktop buys nothing and the unlock event follows anyway) returns
// false and must not touch the hooks.
constexpr bool ShouldReinstallHooksOnEvent(UINT msg, WPARAM wparam) {
    if (msg == WM_POWERBROADCAST) {
        return wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND;
    }
    if (msg == WM_WTSSESSION_CHANGE) {
        return wparam == WTS_SESSION_UNLOCK;
    }
    return false;
}

// Coalescing window for the re-registration: resume/unlock fires several
// related messages in quick succession (e.g. RESUMEAUTOMATIC then
// RESUMESUSPEND, or a power event plus a manual unlock). One pending timer
// refreshed by each trigger keeps the reinstall to ONE Stop()+Start() pair.
inline constexpr UINT kHookReinstallDebounceMs = 1200;
// Watchdog cadence: the handle-validity + thread-liveness check behind the
// event-driven path. Bounds the worst-case silent-unhook window (UAC secure
// desktop trips have no unlock message when the session was never locked).
inline constexpr UINT kHookHealthWatchdogMs = 30000;

class KeyboardHook {
public:
    KeyboardHook(AppConfig& config, PipelineWorker& worker, FloatingBadge& badge, SystemTray& tray);
    ~KeyboardHook();

    bool Start();
    void Stop();

    // ---- REQ-R14 (audit §5 latent item 2): hook lifecycle resilience ----
    // True while the hook thread was started and has not stopped (does NOT
    // verify the OS-side hook is still alive - see IsHealthy()).
    bool IsRunning() const { return running_.load(std::memory_order_acquire); }
    // Health check for the main-thread watchdog: true only while the hook
    // thread is running AND a hook handle was successfully installed.
    bool IsHealthy() const {
        return running_.load(std::memory_order_acquire) &&
               hHook_ != nullptr;
    }
    // Verify-and-reinstall seam used after Sleep/Resume, Win+L unlock, and
    // secure-desktop transitions. LL hooks normally survive these, but the
    // OS can silently drop WH_KEYBOARD_LL (LowLevelHooksTimeout) exactly
    // around desktop switches; re-registration is a few-millisecond
    // deterministic insurance policy. Refuses to resurrect a hook the user
    // (or shutdown) stopped: returns false without side effects when not
    // running. MUST be called on the main thread (same contract as
    // Start/Stop - it joins the hook thread).
    bool Reinstall() {
        if (!running_.load(std::memory_order_acquire)) {
            return false;
        }
        Stop();
        return Start();
    }

    void SetActive(bool active);
    bool IsActive() const { return is_active_.load(std::memory_order_relaxed); }
    void ToggleActive();
    void CycleTargetLanguage();
    void ToggleAutoSend();

    void SetDoubleCtrlCCallback(std::function<void()> cb);
    void SetEscCallback(std::function<bool()> cb) { esc_cb_ = std::move(cb); }

    // REQ-R07 (audit §3.1) + REQ-R08 (audit §3.2): invoked from SetActive() on
    // EVERY real active-state change (hook-thread or main-thread context).
    // main.cpp wires it to (a) mouse_hook.SetEnabled(active) for 1:1 sync - the
    // F9/Win+F9 path inside the keyboard hook used to leave MouseHook disabled
    // forever - and (b) a thread-safe tooltip bubble for the visual feedback.
    void SetActiveChangeCallback(std::function<void(bool)> cb) { active_change_cb_ = std::move(cb); }

    // ---- REQ-R08 (audit §3.2): toggle-hotkey parsing, pure + unit-testable ----
    struct HotkeySpec {
        UINT vk = 0;
        bool ctrl = false;
        bool shift = false;
        bool alt = false;
        bool win = false;
        bool valid = false;

        // Explicit memberwise equality (kept constexpr; equivalent to a
        // C++20 defaulted operator== but with zero toolchain ambiguity).
        constexpr bool operator==(const HotkeySpec& o) const {
            return vk == o.vk && ctrl == o.ctrl && shift == o.shift &&
                   alt == o.alt && win == o.win && valid == o.valid;
        }
    };

    // Parses "Win+F9" / "Ctrl+Shift+Enter"-style strings (case-insensitive,
    // whitespace-tolerant). Returns {valid=false} for empty/unknown syntax.
    static HotkeySpec ParseHotkey(std::string_view spec);

    // Builds the spec for a physical keypress given the async modifier states
    // sampled inside the hook callback. valid=true so it compares equal to a
    // parsed spec that names the same combo.
    static constexpr HotkeySpec MakePressSpec(UINT vk, bool ctrl, bool shift, bool alt, bool win) {
        return HotkeySpec{ vk, ctrl, shift, alt, win, true };
    }

    // The EXACT predicate LowLevelKeyboardProc applies to every keydown against
    // the compiled toggle spec (vk equality + full modifier-set equality). The
    // unit tests assert on this shared definition, so the "swallow only when
    // the combo matches" audit requirement (REQ-R08) is pinned, not re-implemented.
    static constexpr bool HotkeyMatches(const HotkeySpec& spec, UINT vk, bool ctrl, bool shift, bool alt, bool win) {
        return spec.valid && vk == spec.vk && MakePressSpec(vk, ctrl, shift, alt, win) == spec;
    }

    // True when vk is a left/right Windows key. Used by the Win+F9 path to
    // consume exactly one Win key-up after a swallowed Win combo, so Explorer
    // does not open the Start menu on release (AutoHotkey-style suppression).
    static constexpr bool IsWinKey(UINT vk) { return vk == VK_LWIN || vk == VK_RWIN; }

    // The compiled-in default toggle combo (bare F9).
    static constexpr HotkeySpec kDefaultToggleHotkey{ VK_F9, /*ctrl*/false, /*shift*/false, /*alt*/false, /*win*/false, /*valid*/true };

    // Pure resolution seam shared by Start() and the tests: valid combos are
    // honored as-is (including bare "F9"); invalid or empty raw config strings
    // fall back to kDefaultToggleHotkey.
    static HotkeySpec ResolveToggleFromConfig(std::string_view raw_toggle) {
        const HotkeySpec parsed = ParseHotkey(raw_toggle);
        if (!parsed.valid) {
            return kDefaultToggleHotkey;
        }
        return parsed;
    }

    // ---- REQ-R06 (audit §2.5): async double-Ctrl+C dispatch ----
    // DispatchDoubleCtrlC() is the ONLY thing the LowLevelKeyboardProc does when
    // a double-Ctrl+C is detected. It performs a non-blocking handoff (try_lock
    // + flag flip + notify_one) to a persistent worker thread and returns in
    // microseconds - it never runs the user callback, never sleeps, never joins,
    // never blocks on a contended lock. Exposed publicly so the threading
    // contract is unit-testable without installing a real hook
    // (TestKeyboardHookAsyncDispatch()).
    static void DispatchDoubleCtrlC();
    // True while a dispatched double-Ctrl+C body is executing on the worker.
    static bool IsDoubleCtrlCBusy() {
        return s_instance ? s_instance->double_ctrl_c_busy_.load(std::memory_order_relaxed) : false;
    }

    // Read-only accessor of the compiled toggle combo (set in Start()).
    HotkeySpec ToggleHotkey() const { return toggle_spec_; }

    // REQ-R14 test seam: the OS thread id of the running hook thread (0 when
    // not started). Tests prove Reinstall() actually replaced the thread.
    DWORD HookThreadIdForTest() const { return hook_thread_id_; }

private:
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    void HookThreadProc();

    // Effective toggle combo: config_.hotkey_toggle parsed via ParseHotkey,
    // migrated to kDefaultToggleHotkey when unset/invalid or legacy bare "F9".
    HotkeySpec ResolvedToggleHotkey() const;

    // REQ-R17: hook-local IME composition mirror (see ImeMirrorNext). Owned by
    // the hook thread; relaxed atomics because no other thread reads it.
    std::atomic<bool> ime_composing_{false};

    // REQ-R06 persistent-worker lifecycle. Start() spawns the loop, Stop() (and
    // therefore ~KeyboardHook) request_stop()+joins it deterministically so the
    // worker can never outlive the hook object. Runs ONLY on the main thread.
    void StartAsyncWorker();
    void StopAsyncWorker();
    void DoubleCtrlCWorkerLoop(std::stop_token st);
    // Executes the user callback on the worker thread. Never called from the
    // hook callback. double_ctrl_c_cb_ is startup-written/read-only-after-Start
    // (same contract as esc_cb_/toggle_spec_), so no lock guards it here.
    void RunDoubleCtrlCBody();

    AppConfig& config_;
    PipelineWorker& worker_;
    FloatingBadge& badge_;
    SystemTray& tray_;

    std::atomic<bool> is_active_{true};
    std::atomic<bool> running_{false};
    DWORD hook_thread_id_ = 0;
    HHOOK hHook_ = nullptr;
    HANDLE hReadyEvent_ = nullptr;
    std::thread thread_;

    HotkeySpec toggle_spec_{};
    std::atomic<bool> suppress_win_keyup_{false};

    DWORD last_ctrl_c_time_ = 0;
    std::function<void()> double_ctrl_c_cb_;
    std::function<bool()> esc_cb_;
    std::function<void(bool)> active_change_cb_;

    // REQ-R06 worker state. double_ctrl_c_pending_ is a single-slot flag owned
    // by the worker loop; double_ctrl_c_busy_ is a relaxed atomic read by the
    // hook thread in DispatchDoubleCtrlC() and by IsDoubleCtrlCBusy(). The
    // mutex exists only to make the condition_variable wait legal; the hook
    // thread NEVER touches it (see DispatchDoubleCtrlC: pure atomics + notify).
    std::jthread double_ctrl_c_worker_;
    std::atomic<bool> double_ctrl_c_worker_live_{false};
    std::mutex double_ctrl_c_mutex_;
    std::condition_variable double_ctrl_c_cv_;
    std::atomic<bool> double_ctrl_c_pending_{false};
    std::atomic<bool> double_ctrl_c_busy_{false};

    static KeyboardHook* s_instance;
};

} // namespace emebalachat
