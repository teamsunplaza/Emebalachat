#include "hook.hpp"
#include "diag_logger.hpp"
#include "sound.hpp"
#include "unicode_utils.hpp"
#include "win32_input.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>

namespace emebalachat {

namespace {

// ---- 260905 diagnostics: foreground-window info cache (hook-thread safe) ----
// GetWindowTextW on ANOTHER process's window SendMessage's WM_GETTEXT to that
// window's thread, so an uncached lookup per keystroke at typing speed is both
// wasteful and a stall vector. Cache keyed by HWND with a ~200 ms TTL per the
// VP directive; GetClassNameW is local (no cross-thread send). The cache is
// only ever touched from the hook thread (LowLevelKeyboardProc), which is a
// single-threaded context for WH_KEYBOARD_LL delivery.
struct ForegroundWindowCache {
    HWND hwnd = nullptr;
    DWORD next_refresh_ms = 0; // GetTickCount64() low-32 semantics
    wchar_t title[120] = L"";
    wchar_t cls[120] = L"";
};
ForegroundWindowCache g_fg_cache;

// UTF-8 copies of the cached title/class for the current foreground window,
// refreshed at most every kFgCacheTtlMs while the HWND stays the same (an HWND
// change refreshes immediately: the window switch itself is the interesting
// diagnostic event and costs one lookup).
inline constexpr DWORD kFgCacheTtlMs = 200;
HWND RefreshForegroundWindowInfo(std::string& out_title_utf8, std::string& out_cls_utf8) {
    HWND hwnd = ::GetForegroundWindow();
    const ULONGLONG now = ::GetTickCount64();
    bool stale = true;
    if (hwnd == g_fg_cache.hwnd && hwnd != nullptr) {
        stale = (now >= static_cast<ULONGLONG>(g_fg_cache.next_refresh_ms));
    }
    if (stale && hwnd) {
        g_fg_cache.hwnd = hwnd;
        g_fg_cache.next_refresh_ms = static_cast<DWORD>(now + kFgCacheTtlMs);
        ::GetWindowTextW(hwnd, g_fg_cache.title, 120);
        ::GetClassNameW(hwnd, g_fg_cache.cls, 120);
    }
    out_title_utf8 = emebalachat::ToUtf8(g_fg_cache.title);
    out_cls_utf8 = emebalachat::ToUtf8(g_fg_cache.cls);
    return hwnd;
}

// Resulting character for a keydown, per the VP directive (ToUnicode with a
// MapVirtualKey fallback). ToUnicodeEx consults the FOREGROUND window's
// layout; Microsoft documents an internal SendMessageTimeout for dead-key
// resolution, a residual REQ-R06 latency vector - accepted deliberately for
// this user-authorized diagnostic build (the bugs under investigation include
// stalls the user cannot otherwise see) and noted in the r7 report. VK_-level
// non-printable keys (arrows, F-keys) skip ToUnicode entirely: they have no
// char and MapVirtualKey yields none. IME-composing keys arrive as
// VK_PROCESSKEY and are marked ime=1 with char='?' instead (no meaningful
// per-key char exists for them).
char KeyChar(UINT vk, UINT scan, bool ctrl, bool shift, bool alt) {
    // ToUnicodeEx is only interesting for character-producing vks; every other
    // key would return 0/-1 anyway and the negative return touches the
    // dead-key state machine, so the allowlist IS the correctness filter.
    // Letters/digits have vk == ASCII code. The 0xBA..0xC0 / 0xDB..0xDE / 0xE2
    // literals are the OEM punctuation keys (VK_OEM_* names are gated by SDK
    // WINVER settings and cannot be relied on here - VK_MUTE already proved
    // that). Numpad digits/operators are included (NumLock state feeds the
    // synthesized key state below).
    const bool char_candidate =
        vk == VK_SPACE ||
        (vk >= '0' && vk <= '9') ||
        (vk >= 'A' && vk <= 'Z') ||
        (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) ||
        vk == VK_ADD || vk == VK_SUBTRACT || vk == VK_MULTIPLY ||
        vk == VK_DIVIDE || vk == VK_DECIMAL || vk == VK_SEPARATOR ||
        (vk >= 0xBA && vk <= 0xC0) ||
        (vk >= 0xDB && vk <= 0xDE) ||
        vk == 0xE2;
    if (char_candidate && !ctrl && !alt) {
        // Build the key state from ASYNC reads: the hook thread pumps no
        // keyboard messages, so its GetKeyboardState thread state is stale —
        // GetAsyncKeyState reflects the real global state per modifier.
        BYTE ks[256] = {};
        if (shift) ks[VK_SHIFT] = 0x80;
        if (ctrl) ks[VK_CONTROL] = 0x80;
        if (alt) ks[VK_MENU] = 0x80;
        if ((::GetAsyncKeyState(VK_CAPITAL) & 1) != 0) ks[VK_CAPITAL] = 1;
        wchar_t buf[4] = {};
        const int n = ::ToUnicodeEx(vk, scan, ks, buf, 3, 0, nullptr);
        if (n >= 1 && buf[0] >= 0x20 && buf[0] < 0x7F) {
            return static_cast<char>(buf[0]);
        }
        if (n >= 1 && buf[0] >= 0x7F) {
            return static_cast<char>(0x7F); // non-ASCII char present (Hangul
                                            // jamo direct output etc.): mark
                                            // with a sentinel the KEY line
                                            // renders as '<uni>'
        }
        // n <= 0 (no mapping or dead key): fall through to MapVirtualKey.
    }
    UINT c = ::MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR);
    c &= 0x7FFFu; // bit 15 marks dead keys; treat as no char
    if (c == 0) {
        return 0;
    }
    if (c >= L'A' && c <= L'Z' && !shift) {
        c += (L'a' - L'A');
    }
    return (c < 0x7F) ? static_cast<char>(c) : 0x7F;
}

// Printable token for the mapped char: literal char, '_' for space,
// '<uni>' for a non-ASCII result, '?' when the key has no char mapping.
const char* KeyCharToken(char c, char (&buf)[8]) {
    if (c == 0) {
        buf[0] = '?'; buf[1] = 0;
    } else if (c == ' ') {
        buf[0] = '_'; buf[1] = 0;
    } else if (c == 0x7F) {
        buf[0] = '<'; buf[1] = 'u'; buf[2] = 'n'; buf[3] = 'i'; buf[4] = '>'; buf[5] = 0;
    } else {
        buf[0] = c; buf[1] = 0;
    }
    return buf;
}

} // namespace

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
        DIAG_F("HOOK/DispatchDoubleCtrlC/002: worker not live (Start() failed?), event dropped\n");
        return;
    }
    DIAG_LOG("PIPELINE", "double_ctrl_c detected -> dispatched to async worker");
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
        DIAG_F("HOOK/RunDoubleCtrlCBody/001: callback threw: %s\n", e.what());
    } catch (...) {
        DIAG_F("HOOK/RunDoubleCtrlCBody/002: callback threw unknown exception\n");
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
        DIAG_LOG("STATE", "active %d -> %d (badge/tray/sound/observers refresh)",
                 active ? 0 : 1, active ? 1 : 0);
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
    DIAG_LOG("HOTKEY", "Ctrl+F9 language cycle requested");
    // R6 Phase 1 (B3, plan §2.3): when main.cpp wired the coordinator seam,
    // the ENTIRE cycle (next-target computation + locked persist + badge/tray/
    // tooltip refresh) runs on the GUI thread through the single
    // ApplyLanguageChange authority, like every other surface. This hook-
    // thread path only posts a non-blocking request. The inline implementation
    // below stays as the standalone/unit-test fallback when unwired.
    if (lang_cycle_cb_) {
        lang_cycle_cb_();
        return;
    }
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
    DIAG_LOG("STATE", "auto_send %d -> %d (Ctrl+Shift+Enter / toggle path)",
             !next ? 1 : 0, next ? 1 : 0);
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

        // ---- 260905 diagnostics (user-authorized keystroke CONTENT logging) ----
        // EVERY real (non-synthetic) keydown is recorded: vk, scan, modifier
        // states, the resulting char, the IME-composing flag, and the target
        // foreground window title+class (cached ~200 ms, see
        // RefreshForegroundWindowInfo). Enqueue-only: this whole block formats
        // a string and hands it to the diag queue; file I/O happens on the
        // logger's own thread. vk==VK_PROCESSKEY (IME-intercepted key) is
        // marked ime=1 and carries char '?'.
        {
            std::string fg_title;
            std::string fg_cls;
            const HWND fg = RefreshForegroundWindowInfo(fg_title, fg_cls);
            const bool ime_key = (kbd->vkCode == VK_PROCESSKEY);
            const char ch = ime_key ? 0 : KeyChar(kbd->vkCode, kbd->scanCode, ctrl, shift, alt);
            char chtok[8];
            KeyCharToken(ch, chtok);
            const bool composing = s_instance->ime_composing_.load(std::memory_order_relaxed);
            DIAG_LOG("KEY", "vk=0x%02X scan=0x%02X %s%s%s%s char=%s ime=%d composing=%d fg=%p class=%s title=%s",
                     kbd->vkCode, kbd->scanCode,
                     shift ? "S" : "-", ctrl ? "C" : "-", alt ? "A" : "-", win ? "W" : "-",
                     chtok, ime_key ? 1 : 0, composing ? 1 : 0,
                     reinterpret_cast<void*>(fg), fg_cls.c_str(), fg_title.c_str());
        }

        // REQ-R08: the configured toggle combo (default Win+F9) is matched
        // FIRST - before the blanket Alt/Win passthrough below - and it only
        // swallows when the FULL modifier set matches toggle_spec_ exactly
        // (HotkeyMatches: vk + ctrl + shift + alt + win, pinned by unit tests).
        // Everything else containing Alt or Win still passes through untouched
        // (Excel Alt+Enter, game Alt+Enter, native Win shortcuts). Bare F9 is
        // no longer a toggle by default (audit §3.2), so VS/Excel keep it.
        if (KeyboardHook::HotkeyMatches(s_instance->toggle_spec_, kbd->vkCode, ctrl, shift, alt, win)) {
            DIAG_LOG("HOTKEY", "toggle combo matched (vk=0x%02X modifiers=%s%s%s%s) -> ToggleActive",
                     kbd->vkCode, ctrl ? "C" : "-", shift ? "S" : "-", alt ? "A" : "-", win ? "W" : "-");
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
        // ---- 260905 diagnostics: Enter-path decision trace. EVERY gate
        // evaluation in this branch is logged with its input values and the
        // outcome + reason, so a later bug report reads as a complete
        // decision record of "what happened when Enter was pressed and why".
        // Enter/Shift+Enter get the explicit KEY-line form the VP requested;
        // the per-branch ENTER_GATE lines carry the pipeline gate snapshot. ----
        if (kbd->vkCode == VK_RETURN) {
            DIAG_LOG("KEY", "Enter (shift=%d) - pipeline candidate", shift ? 1 : 0);
            const bool dbg_composing =
                s_instance->ime_composing_.load(std::memory_order_relaxed);
            if (ctrl && shift) {
                DIAG_LOG("ENTER_GATE",
                         "outcome=auto_send_toggle reason=ctrl+shift active=%d busy=%d "
                         "ime_composing=%d shift=1 auto_send_new=%d",
                         s_instance->IsActive() ? 1 : 0, s_instance->worker_.IsBusy() ? 1 : 0,
                         dbg_composing ? 1 : 0,
                         (!s_instance->config_.auto_send.load(std::memory_order_relaxed)) ? 1 : 0);
                // Ctrl+Shift+Enter toggles Auto-Send mode
                s_instance->ToggleAutoSend();
                return 1;
            }

            if (ctrl) {
                DIAG_LOG("ENTER_GATE",
                         "outcome=pass_through reason=ctrl_enter active=%d busy=%d "
                         "ime_composing=%d shift=%d",
                         s_instance->IsActive() ? 1 : 0, s_instance->worker_.IsBusy() ? 1 : 0,
                         dbg_composing ? 1 : 0, shift ? 1 : 0);
                // Ctrl+Enter is normal pass-through
                return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
            }

            // S2 fix (Shift+Enter newline): Shift+Enter must pass through
            // UNTOUCHED so the target app inserts a newline (universal chat-app
            // convention). The R3-era code never tested the Shift modifier here,
            // so with the hook active ANY Enter (bare or Shift-held) was
            // swallowed into the send-and-replace pipeline and Shift+Enter could
            // never produce a newline. Gate it BEFORE the interception below:
            // only a BARE Enter may be hijacked. (Ctrl+Shift+Enter already
            // returned above as the auto-send toggle; this branch sees only
            // plain Shift+Enter.) The IME mirror intentionally KEEPS its state
            // for a VK_RETURN (ImeMirrorNext), and we are returning without
            // touching the pipeline, so no mirror clear is needed here either.
            if (shift) {
                DIAG_LOG("ENTER_GATE",
                         "outcome=pass_through reason=shift_enter_newline active=%d busy=%d "
                         "ime_composing=%d shift=1",
                         s_instance->IsActive() ? 1 : 0, s_instance->worker_.IsBusy() ? 1 : 0,
                         dbg_composing ? 1 : 0);
                return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
            }

            // Composition open: the Enter belongs to the IME commit cycle.
            // Hand it through untouched and retire the mirror flag.
            if (dbg_composing) {
                DIAG_LOG("ENTER_GATE",
                         "outcome=pass_through reason=ime_composing_commit active=%d busy=%d "
                         "ime_composing=1 shift=0 (mirror cleared)",
                         s_instance->IsActive() ? 1 : 0, s_instance->worker_.IsBusy() ? 1 : 0);
                s_instance->ime_composing_.store(false, std::memory_order_relaxed);
                return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
            }

            // Only a BARE Enter reaches here (Shift and Ctrl already returned
            // above). The shared S2 predicate decides interception (mirror just
            // cleared -> composing input is false; shift is false by now). One
            // definition pinned by the unit tests.
            // R5 observability: log the gate inputs so a silent "Enter sent
            // untranslated" can be attributed to the exact failing gate.
            const bool gate_active = s_instance->IsActive();
            const bool gate_busy = s_instance->worker_.IsBusy();
            const bool gate_allowed = EnterSendReplaceAllowed(
                /*vk_is_return*/ true,
                gate_active,
                gate_busy,
                /*ime_composing*/ false,
                /*shift*/ shift);
            // Full decision record EVERY time (file sink; the stderr sites
            // below keep their historical spam discipline unchanged):
            // active, busy, ime_composing (just cleared -> 0 by here), shift,
            // and the predicate verdict with the reason it produced.
            DIAG_LOG("ENTER_GATE",
                     "bare_enter evaluated active=%d worker_busy=%d ime_composing=0 shift=%d "
                     "verdict=%s reason=%s",
                     gate_active ? 1 : 0, gate_busy ? 1 : 0, shift ? 1 : 0,
                     gate_allowed ? "INTERCEPT" : "PASS_THROUGH",
                     gate_allowed ? "all_gates_open"
                                  : (gate_busy ? "worker_busy" : (gate_active ? "gate_denied_unspecified" : "hook_inactive")));
            if (gate_allowed) {
                // Capture target window HWND at interception time for process-aware selection
                HWND target_hwnd = ::GetForegroundWindow();

                // Post task to worker and intercept Enter from reaching target control
                if (s_instance->worker_.PostTask(shift, target_hwnd)) {
                    DIAG_F("HOOK/Enter/001: bare Enter intercepted -> pipeline task posted (hwnd=%p)\n",
                           reinterpret_cast<void*>(target_hwnd));
                    DIAG_LOG("ENTER_GATE", "outcome=task_posted hwnd=%p",
                             reinterpret_cast<void*>(target_hwnd));
                    return 1; // Intercepted immediately (< 1ms)!
                }
                DIAG_LOG("ENTER_GATE",
                         "outcome=pass_through reason=posttask_refused_race (gate allowed but "
                         "worker accepted no task: busy flipped between IsBusy() and PostTask) "
                         "hwnd=%p",
                         reinterpret_cast<void*>(target_hwnd));
            } else if (gate_active && !gate_busy) {
                // Log only the meaningfully-gated case. When !active or busy
                // the user has the feature off / a task in flight: passing
                // through is expected, not a failure, and logging EVERY
                // such Enter would spam stderr at typing speed (and the log
                // would carry no diagnostic information beyond what
                // HOOK/Enter/001's absence already proves).
                DIAG_F("HOOK/Enter/002: bare Enter PASSED THROUGH untranslated while active=!1 busy=0 "
                       "(ime mirror race or unspecified gate; see HOOK/Enter/001 absence)\n");
            }
        }
    }

    return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
}

} // namespace emebalachat
