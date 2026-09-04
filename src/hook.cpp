#include "hook.hpp"
#include "sound.hpp"
#include "unicode_utils.hpp"
#include "win32_input.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>

namespace emebalachat {

KeyboardHook* KeyboardHook::s_instance = nullptr;

KeyboardHook::KeyboardHook(AppConfig& config, PipelineWorker& worker, FloatingBadge& badge, SystemTray& tray)
    : config_(config), worker_(worker), badge_(badge), tray_(tray) {
    s_instance = this;
    hReadyEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

KeyboardHook::~KeyboardHook() {
    Stop();
    if (s_instance == this) {
        s_instance = nullptr;
    }
    if (hReadyEvent_) {
        ::CloseHandle(hReadyEvent_);
        hReadyEvent_ = nullptr;
    }
}

void KeyboardHook::SetDoubleCtrlCCallback(std::function<void()> cb) {
    // Contract: callbacks are registered at startup BEFORE Start(), identical to
    // SetEscCallback's existing contract. The worker thread reads
    // double_ctrl_c_cb_ only while running (i.e. after Start()), so no lock is
    // needed; runtime re-registration is not a supported use anywhere in the app.
    double_ctrl_c_cb_ = std::move(cb);
}

// ---- REQ-R06 (audit §2.5): double-Ctrl+C async dispatch worker ----
//
// The LowLevelKeyboardProc must never execute the user callback (clipboard read,
// engine.Translate, tooltip show): local LLM inference runs 1-2 s and exceeding
// the OS LowLevelHooksTimeout makes Windows SILENTLY unhook WH_KEYBOARD_LL.
// The callback is handed to this persistent worker thread instead. The worker is
// owned for the whole KeyboardHook lifetime: spawned in Start(), then
// request_stop()+joined deterministically in Stop() (and therefore ~KeyboardHook)
// on the main thread, so it can never outlive the hook object (same
// deterministic-lifetime rule the MouseHook delayed jobs follow - no detach).

void KeyboardHook::StartAsyncWorker() {
    bool expected = false;
    if (!double_ctrl_c_worker_live_.compare_exchange_strong(expected, true)) {
        return; // already live
    }
    double_ctrl_c_worker_ = std::jthread([this](std::stop_token st) {
        DoubleCtrlCWorkerLoop(st);
    });
}

void KeyboardHook::StopAsyncWorker() {
    // Idempotent. Main-thread only (Start failure path, Stop(), destructor).
    if (double_ctrl_c_worker_.joinable()) {
        double_ctrl_c_pending_.store(false, std::memory_order_release); // drop queued event
        // Move-assignment from an empty jthread performs request_stop() + join()
        // on the current thread (std::jthread has no join()). This is the ONLY
        // join of the worker, and it runs on the main thread, never in a hook.
        // The join waits at most the worker loop's 50 ms cv backstop.
        double_ctrl_c_worker_ = std::jthread{};
    }
    double_ctrl_c_worker_live_.store(false, std::memory_order_release);
}

void KeyboardHook::DoubleCtrlCWorkerLoop(std::stop_token st) {
    std::unique_lock<std::mutex> lk(double_ctrl_c_mutex_);
    while (true) {
        // wait_for (not wait): DispatchDoubleCtrlC() posts its event and calls
        // notify_one() WITHOUT holding double_ctrl_c_mutex_ - the hook thread
        // must never take a lock that the worker could be holding across a
        // multi-second translation. Lock-free notifies can theoretically be
        // lost between the worker's predicate check and its registration to
        // sleep, so the 50 ms timeout is the backstop that bounds the worst
        // case. A 50 ms extra delay before an LLM translation the user already
        // waits 1-2 s for is imperceptible; blocking the hook thread instead
        // would get WH_KEYBOARD_LL removed by the OS.
        double_ctrl_c_cv_.wait_for(lk, std::chrono::milliseconds(50), [this, &st]() {
            return double_ctrl_c_pending_.load(std::memory_order_acquire) || st.stop_requested();
        });
        if (st.stop_requested()) {
            break;
        }
        // exchange() consumes the slot; `continue` on a spurious wake keeps the
        // worker idle without ever clearing a busy flag a dispatch just set.
        if (!double_ctrl_c_pending_.exchange(false, std::memory_order_acq_rel)) {
            continue;
        }
        lk.unlock(); // mutex NEVER held across the callback (hook stays free)
        RunDoubleCtrlCBody();
        lk.lock();
        // Re-arm check BEFORE clearing busy: a double-Ctrl+C that arrived while
        // this body ran set pending again (accepted, because pending was false
        // for the whole body) - busy must stay true across the queued re-run.
        if (!double_ctrl_c_pending_.load(std::memory_order_acquire)) {
            double_ctrl_c_busy_.store(false, std::memory_order_release);
        }
    }
    // Exit path: a stop requested while pending/running must still clear the
    // busy flag so IsDoubleCtrlCBusy() reports honestly after Stop() joins.
    double_ctrl_c_busy_.store(false, std::memory_order_release);
}

void KeyboardHook::DispatchDoubleCtrlC() {
    KeyboardHook* self = s_instance;
    if (!self) {
        return;
    }
    if (!self->double_ctrl_c_worker_live_.load(std::memory_order_acquire)) {
        fprintf(stderr, "HOOK/DispatchDoubleCtrlC/002: worker not live (Start() failed?), event dropped\n");
        return;
    }
    // REQ-R06 non-blocking handoff, ZERO locks on the hook thread:
    //   - one acquire load (worker live)
    //   - one acq_rel exchange on the single-slot pending flag (~20 ns; if an
    //     event is already queued the new one coalesces into it - a double
    //     Ctrl+C pressed twice while a translation runs must not queue a second)
    //   - one release store (busy) + notify_one() WITHOUT the mutex (see the
    //     worker loop: the 50 ms cv backstop bounds a theoretically lost notify)
    // No Sleep, no join, no blocking lock, no allocation.
    const bool already_queued = self->double_ctrl_c_pending_.exchange(true, std::memory_order_acq_rel);
    self->double_ctrl_c_busy_.store(true, std::memory_order_release);
    if (!already_queued) {
        self->double_ctrl_c_cv_.notify_one();
    }
}

void KeyboardHook::RunDoubleCtrlCBody() {
    // Runs on double_ctrl_c_worker_ ONLY (see SetDoubleCtrlCCallback contract).
    if (!double_ctrl_c_cb_) {
        return;
    }
    try {
        double_ctrl_c_cb_();
    } catch (const std::exception& e) {
        fprintf(stderr, "HOOK/RunDoubleCtrlCBody/001: callback threw: %s\n", e.what());
    } catch (...) {
        fprintf(stderr, "HOOK/RunDoubleCtrlCBody/002: callback threw unknown exception\n");
    }
}

bool KeyboardHook::Start() {
    if (running_.exchange(true)) {
        return true;
    }

    // Compile the toggle hotkey once, before the hook proc can run (config
    // fields are startup-written/read-only after, see config.hpp I4 notes).
    toggle_spec_ = ResolvedToggleHotkey();

    // REQ-R06: the async worker must exist before the hook proc can dispatch.
    StartAsyncWorker();

    if (hReadyEvent_) {
        ::ResetEvent(hReadyEvent_);
    }

    thread_ = std::thread(&KeyboardHook::HookThreadProc, this);

    if (hReadyEvent_) {
        ::WaitForSingleObject(hReadyEvent_, 2000);
    }

    if (!hHook_) {
        // Hook install failed: HookThreadProc already returned without pumping,
        // so join the finished thread here (a joinable std::thread destroyed
        // without join calls std::terminate). Then tear the async worker back
        // down so the object stays in a clean, restartable state.
        if (thread_.joinable()) {
            thread_.join();
        }
        StopAsyncWorker();
        running_.store(false);
        return false;
    }
    return true;
}

void KeyboardHook::Stop() {
    if (!running_.exchange(false)) {
        // Hook thread was never running; still guarantee the REQ-R06 worker is
        // joined (covers destructor without Start(), and the double-Stop case).
        StopAsyncWorker();
        return;
    }

    if (hook_thread_id_ != 0) {
        ::PostThreadMessageW(hook_thread_id_, WM_QUIT, 0, 0);
    }

    if (thread_.joinable()) {
        thread_.join();
    }

    // Join the async worker AFTER the hook thread is down: a double-Ctrl+C
    // detected microseconds before WM_QUIT could otherwise race the join.
    StopAsyncWorker();
}

// ---- REQ-R08 (audit §3.2): pure, unit-testable hotkey parsing ----

namespace {

// Trim ASCII whitespace and lowercase one in-place token.
void NormalizeToken(std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    s = s.substr(b, e - b);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
}

// Maps a normalized key token to a virtual key code, 0 when unknown.
UINT VkFromKeyToken(const std::string& tok) {
    if (tok.empty()) return 0;

    // f1..f24: token starts with 'f' followed by only digits.
    if (tok[0] == 'f' && tok.size() >= 2) {
        int n = 0;
        for (size_t i = 1; i < tok.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(tok[i]))) return 0;
            n = n * 10 + (tok[i] - '0');
        }
        if (n >= 1 && n <= 24) return VK_F1 + static_cast<UINT>(n - 1);
        return 0; // f0, f25+ are not function keys
    }

    if (tok == "enter" || tok == "return") return VK_RETURN;
    if (tok == "esc" || tok == "escape") return VK_ESCAPE;
    if (tok == "tab") return VK_TAB;
    if (tok == "space") return VK_SPACE;
    if (tok == "insert" || tok == "ins") return VK_INSERT;
    if (tok == "delete" || tok == "del") return VK_DELETE;
    if (tok == "home") return VK_HOME;
    if (tok == "end") return VK_END;
    if (tok == "pgup" || tok == "prior") return VK_PRIOR;
    if (tok == "pgdn" || tok == "next") return VK_NEXT;
    if (tok == "up") return VK_UP;
    if (tok == "down") return VK_DOWN;
    if (tok == "left") return VK_LEFT;
    if (tok == "right") return VK_RIGHT;

    if (tok.size() == 1) {
        const char c = tok[0];
        if (c >= 'a' && c <= 'z') return static_cast<UINT>('A' + (c - 'a'));
        if (c >= '0' && c <= '9') return static_cast<UINT>(c);
    }
    return 0;
}

} // namespace

KeyboardHook::HotkeySpec KeyboardHook::ParseHotkey(std::string_view spec) {
    HotkeySpec out;
    bool vk_seen = false;

    size_t start = 0;
    while (start <= spec.size()) {
        const size_t plus = spec.find('+', start);
        std::string tok(spec.substr(start, plus == std::string_view::npos ? std::string_view::npos : plus - start));
        NormalizeToken(tok);

        if (tok.empty()) {
            // Empty token ("Win+", "Ctrl++", trailing '+'): invalid syntax.
            return HotkeySpec{};
        }

        if (tok == "ctrl" || tok == "control") {
            out.ctrl = true;
        } else if (tok == "shift") {
            out.shift = true;
        } else if (tok == "alt" || tok == "menu" || tok == "option") {
            out.alt = true;
        } else if (tok == "win" || tok == "windows" || tok == "super") {
            out.win = true;
        } else {
            const UINT vk = VkFromKeyToken(tok);
            if (vk == 0 || vk_seen) {
                // Unknown key token, or a second main key ("Ctrl+Alt+F9+Enter").
                return HotkeySpec{};
            }
            out.vk = vk;
            vk_seen = true;
        }

        if (plus == std::string_view::npos) {
            break;
        }
        start = plus + 1;
    }

    out.valid = vk_seen;
    return out;
}

KeyboardHook::HotkeySpec KeyboardHook::ResolvedToggleHotkey() const {
    // Delegates to the shared pure seam (hook.hpp) so Start() and the unit
    // tests assert on ONE definition of the legacy-"F9"->Win+F9 migration.
    return ResolveToggleFromConfig(config_.hotkey_toggle);
}

void KeyboardHook::SetActive(bool active) {
    if (is_active_.exchange(active) != active) {
        badge_.SetStatus(active ? BadgeStatus::Active : BadgeStatus::Disabled);
        // I4: this runs on the hook thread; read shared fields via a locked
        // snapshot instead of touching std::string members unsynchronized.
        const AppConfig::Snapshot snap = config_.GetSnapshot();
        tray_.UpdateStatus(
            active,
            snap.engine_type == "auto" ? "Google Translate (Auto)" : snap.engine_type,
            snap.source_language,
            snap.target_language,
            snap.auto_send,
            snap.sound_enabled,
            badge_.IsVisible()
        );
        // REQ-R08 visual feedback: the floating badge above IS the visual
        // state indicator (green=active/gray=disabled, and it renders even
        // when the tray tooltip is not hovered), plus the audible chime below.
        if (active) {
            PlayToggleOn();
        } else {
            PlayToggleOff();
        }
        // REQ-R07: single choke point for external state observers. Fires on
        // EVERY real change, including the F9/Win+F9 path inside the hook, so
        // main.cpp keeps mouse_hook.SetEnabled() 1:1 in sync with is_active_.
        if (active_change_cb_) {
            active_change_cb_(active);
        }
    }
}

void KeyboardHook::ToggleActive() {
    SetActive(!is_active_.load());
}

void KeyboardHook::CycleTargetLanguage() {
    std::string next_tgt = config_.CycleLanguage(); // locked mutator + save inside
    const AppConfig::Snapshot snap = config_.GetSnapshot(); // I4: hook-thread reads
    badge_.SetLanguages(ToUtf16(snap.source_language), ToUtf16(next_tgt));
    tray_.UpdateStatus(
        is_active_.load(),
        snap.engine_type == "auto" ? "Google Translate (Auto)" : snap.engine_type,
        snap.source_language,
        next_tgt,
        snap.auto_send,
        snap.sound_enabled,
        badge_.IsVisible()
    );
    PlayLangChange();
}

void KeyboardHook::ToggleAutoSend() {
    // I4: auto_send is std::atomic; this read-modify-write happens on the hook
    // thread while the worker thread reads it per task.
    const bool next = !config_.auto_send.load(std::memory_order_relaxed);
    config_.auto_send.store(next, std::memory_order_relaxed);
    config_.SaveToFile();
    const AppConfig::Snapshot snap = config_.GetSnapshot(); // I4: hook-thread reads
    tray_.UpdateStatus(
        is_active_.load(),
        snap.engine_type == "auto" ? "Google Translate (Auto)" : snap.engine_type,
        snap.source_language,
        snap.target_language,
        next,
        snap.sound_enabled,
        badge_.IsVisible()
    );
    PlayModeChange();
}

void KeyboardHook::HookThreadProc() {
    hook_thread_id_ = ::GetCurrentThreadId();
    HINSTANCE hInst = ::GetModuleHandleW(nullptr);

    hHook_ = ::SetWindowsHookExW(
        WH_KEYBOARD_LL,
        KeyboardHook::LowLevelKeyboardProc,
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

LRESULT CALLBACK KeyboardHook::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION || !s_instance) {
        return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    const auto* kbd = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);

    // Completely bypass all synthetic inputs generated by Emebalachat
    if (kbd->dwExtraInfo == EXTRA_INFO_MARKER) {
        return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    // REQ-R08: after a Win+<combo> swallow, Explorer would otherwise open the
    // Start menu on the Win key release (the shell sees the Win press without
    // its normal key-up pair). Consume exactly one Win key-up after each
    // consumed Win combo. One relaxed atomic CAS; no allocation, no blocking.
    if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
        if (IsWinKey(kbd->vkCode)) {
            bool expected = true;
            if (s_instance->suppress_win_keyup_.compare_exchange_strong(
                    expected, false, std::memory_order_relaxed)) {
                return 1;
            }
        }
        return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
        // REQ-R17 (audit §5 latent item 5): O(1) hook-local IME mirror update
        // (pure ImeMirrorNext, no cross-thread calls). Must run BEFORE the
        // Enter branch reads the flag; VK_RETURN keeps the state so an Enter
        // pressed mid-composition sees the composing=true set by the jamo
        // keys that preceded it.
        s_instance->ime_composing_.store(
            ImeMirrorNext(s_instance->ime_composing_.load(std::memory_order_relaxed),
                          kbd->vkCode),
            std::memory_order_relaxed);

        bool ctrl = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        bool alt = (::GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        bool win = ((::GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) || ((::GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);

        // REQ-R08: the configured toggle combo (default Win+F9) is matched
        // FIRST - before the blanket Alt/Win passthrough below - and it only
        // swallows when the FULL modifier set matches toggle_spec_ exactly
        // (HotkeyMatches: vk + ctrl + shift + alt + win, pinned by unit tests).
        // Everything else containing Alt or Win still passes through untouched
        // (Excel Alt+Enter, game Alt+Enter, native Win shortcuts). Bare F9 is
        // no longer a toggle by default (audit §3.2), so VS/Excel keep it.
        if (KeyboardHook::HotkeyMatches(s_instance->toggle_spec_, kbd->vkCode, ctrl, shift, alt, win)) {
            s_instance->ToggleActive();
            if (win) {
                s_instance->suppress_win_keyup_.store(true, std::memory_order_relaxed);
            }
            return 1; // Consumed: state change (audio+visual feedback in SetActive)
        }

        // Always let Alt or Win key combinations pass through immediately
        // (preserves Excel newline Alt+Enter, game fullscreen Alt+Enter, Win shortcuts, etc.)
        if (alt || win) {
            return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        // ESC hotkey dismissal
        if (kbd->vkCode == VK_ESCAPE) {
            if (s_instance->esc_cb_ && s_instance->esc_cb_()) {
                return 1; // Consumed: dismissed overlay UI
            }
        }

        // Double Ctrl+C hotkey detection (< 400ms)
        if (kbd->vkCode == 'C' && ctrl && !shift && !alt && !win) {
            DWORD now = ::GetTickCount();
            if (s_instance->last_ctrl_c_time_ != 0 && (now - s_instance->last_ctrl_c_time_ <= 400) && (now - s_instance->last_ctrl_c_time_ >= 20)) {
                s_instance->last_ctrl_c_time_ = 0;
                // REQ-R06 (audit §2.5): hand the whole job to the async worker
                // and return. The callback (clipboard settle + engine.Translate,
                // up to seconds) must NEVER run on this thread: exceeding
                // LowLevelHooksTimeout makes Windows silently remove
                // WH_KEYBOARD_LL. DispatchDoubleCtrlC() is a non-blocking
                // atomic exchange + flag store + notify_one (no sleep, no join,
                // no lock, no allocation).
                KeyboardHook::DispatchDoubleCtrlC();
            } else {
                s_instance->last_ctrl_c_time_ = now;
            }
            return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        // Ctrl+F9: cycle target language. (The toggle combo itself matched
        // above; with toggle=Win+F9 this branch no longer collides with the
        // VS/VS Code breakpoint key, and Alt/Win combos never reach here.)
        if (kbd->vkCode == VK_F9) {
            if (ctrl && !shift) {
                s_instance->CycleTargetLanguage();
                return 1;
            }
        }

        // Enter keystroke interception.
        //
        // REQ-R17 (audit §5 latent item 5): TWO gates, zero hook-thread
        // cross-thread calls. (1) Keys consumed by the IME arrive as
        // VK_PROCESSKEY (0xE5) and never reach this branch at all. (2) The
        // Korean IME lets Enter through as VK_RETURN mid-composition (chat
        // apps rely on "commit + send"); the hook-local ImeMirrorNext flag
        // (updated above from the intercepted jamo keys) refuses the hijack
        // while a composition is open. That pass-through Enter IS the IME's
        // commit keystroke, so the mirror is cleared here: the composition
        // this flag described is over the moment the app receives the Enter
        // (a NEW composition re-sets it via its own VK_PROCESSKEY keys, so
        // the flag can never go stale-true and silently kill the NEXT
        // translation - the stuck-flag failure mode this ordering prevents).
        // The hijack path below runs only when the flag is already false, so
        // it needs no clear of its own. A race-window backstop remains
        // worker-side (ExecuteTask re-probes GCS_COMPSTR via IMM before
        // touching the clipboard).
        if (kbd->vkCode == VK_RETURN) {
            if (ctrl && shift) {
                // Ctrl+Shift+Enter toggles Auto-Send mode
                s_instance->ToggleAutoSend();
                return 1;
            }

            if (ctrl) {
                // Ctrl+Enter is normal pass-through
                return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
            }

            // Composition open: the Enter belongs to the IME commit cycle.
            // Hand it through untouched and retire the mirror flag.
            if (s_instance->ime_composing_.load(std::memory_order_relaxed)) {
                s_instance->ime_composing_.store(false, std::memory_order_relaxed);
                return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
            }

            // Regular Enter or Shift+Enter: shared predicate decides
            // interception (mirror just cleared -> composing input is false).
            if (EnterTranslationAllowed(
                    /*vk_is_return*/ true,
                    s_instance->IsActive(),
                    s_instance->worker_.IsBusy(),
                    /*ime_composing*/ false)) {
                // Capture target window HWND at interception time for process-aware selection
                HWND target_hwnd = ::GetForegroundWindow();

                // Post task to worker and intercept Enter from reaching target control
                if (s_instance->worker_.PostTask(shift, target_hwnd)) {
                    return 1; // Intercepted immediately (< 1ms)!
                }
            }
        }
    }

    return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
}

} // namespace emebalachat
