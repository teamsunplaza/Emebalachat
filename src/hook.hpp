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

namespace emebalachat {

class KeyboardHook {
public:
    KeyboardHook(AppConfig& config, PipelineWorker& worker, FloatingBadge& badge, SystemTray& tray);
    ~KeyboardHook();

    bool Start();
    void Stop();

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

    // The compiled-in default toggle combo (Win+F9). Bare F9 is intentionally
    // NOT offered as a default anymore: it collides with the VS/VS Code
    // breakpoint and Excel recalculate global hotkeys (audit §3.2). config.hpp's
    // legacy default string "F9" is also migrated to this value by
    // ResolvedToggleHotkey() - hotkey_toggle is parsed here (not in config.cpp),
    // so this batch needs no config.cpp changes.
    static constexpr HotkeySpec kDefaultToggleHotkey{ VK_F9, /*ctrl*/false, /*shift*/false, /*alt*/false, /*win*/true, /*valid*/true };

    // REQ-R08: pure resolution seam shared by Start() and the tests: a legacy
    // or invalid raw config string (default "F9", empty, unknown syntax)
    // migrates to kDefaultToggleHotkey; any other valid combo is honored as-is.
    // No new config keys: config_.hotkey_toggle is the existing field.
    static HotkeySpec ResolveToggleFromConfig(std::string_view raw_toggle) {
        const HotkeySpec parsed = ParseHotkey(raw_toggle);
        if (!parsed.valid || parsed == MakePressSpec(VK_F9, false, false, false, false)) {
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

private:
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    void HookThreadProc();

    // Effective toggle combo: config_.hotkey_toggle parsed via ParseHotkey,
    // migrated to kDefaultToggleHotkey when unset/invalid or legacy bare "F9".
    HotkeySpec ResolvedToggleHotkey() const;

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
