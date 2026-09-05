#include "config.hpp"
#include "engine.hpp"
#include "hook.hpp"
#include "i18n.hpp"
#include "mouse_hook.hpp"
#include "smart_bypass.hpp"
#include "sound.hpp"
#include "unicode_utils.hpp"
#include "win32_input.hpp"
#include "ui/badge.hpp"
#include "ui/drag_icon.hpp"
#include "ui/dpi.hpp"
#include "ui/tooltip.hpp"
#include "ui/tray.hpp"
#include "worker.hpp"

#include <windows.h>
#include <objbase.h>
#include <wtsapi32.h> // WTSRegisterSessionNotification (REQ-R14)

#include <cstdio>

namespace emebalachat {

namespace {
const wchar_t kControllerClassName[] = L"Emebalachat_ControllerWindowClass";
// REQ-R14: WM_POWERBROADCAST is sent ONLY to top-level windows - a
// message-only window never receives power broadcasts. This tiny hidden
// top-level window (WS_EX_TOOLWINDOW, zero-size, off-screen) exists purely
// so the same ControllerWndProc sees resume events; WTS session
// notifications are registered on it as well.
const wchar_t kPowerSinkClassName[] = L"Emebalachat_PowerSinkWindowClass";
// User-facing label of the default toggle combo (F9).
const wchar_t kToggleHotkeyLabel[] = L"F9";
HWND g_hControllerWnd = nullptr;
HWND g_hPowerWnd = nullptr; // REQ-R14 top-level sink for WM_POWERBROADCAST
SystemTray* g_pTray = nullptr;
KeyboardHook* g_pHook = nullptr;
FloatingBadge* g_pBadge = nullptr;
MouseHook* g_pMouseHook = nullptr; // REQ-R14 resume/unlock re-registration

// ---- REQ-R14 (audit §5 latent item 2): hook lifecycle timers ----
// kTimerHookReinstall: debounced (coalesced) reinstall triggered by
// resume/unlock messages. kTimerHookHealth: periodic watchdog that catches
// the transitions with no message of their own (UAC secure-desktop trips).
constexpr UINT_PTR kTimerHookReinstall = 5001;
constexpr UINT_PTR kTimerHookHealth = 5002;

// Re-register whichever LL hooks are installed, on the GUI thread (the only
// threads allowed to Start/Stop the hooks). Always reinstall on the event
// path: an OS silent-removal (LowLevelHooksTimeout during a desktop switch)
// leaves our HHOOK handle looking valid while the hook is dead, so the
// handle check is only the fast filter, never the decision. Reinstall()
// returns false (no-op) for hooks that are not running by design.
void ReinstallHooksAfterLifecycleEvent(const char* reason, HWND debounce_wnd) {
    // KeyboardHook::Stop() joins the REQ-R06 async worker; if a double-Ctrl+C
    // translation is mid-flight (seconds), re-installing right now would
    // stall the GUI thread for its remainder. Defer: re-arm the debounce
    // timer once and retry after the body completes (bounded by the
    // inference itself - the worker is never stuck indefinitely).
    if (g_pHook && KeyboardHook::IsDoubleCtrlCBusy()) {
        ::SetTimer(debounce_wnd, kTimerHookReinstall, kHookReinstallDebounceMs, nullptr);
        return;
    }
    if (g_pHook) {
        if (!g_pHook->Reinstall()) {
            fprintf(stderr, "MAIN/HookLifecycle/001: keyboard hook re-install failed after %s\n", reason);
        }
    }
    if (g_pMouseHook) {
        if (!g_pMouseHook->Reinstall()) {
            fprintf(stderr, "MAIN/HookLifecycle/002: mouse hook re-install failed after %s\n", reason);
        }
    }
}

LRESULT CALLBACK ControllerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // ---- REQ-R14: Sleep/Resume + Win+L session lifecycle ----
    if (ShouldReinstallHooksOnEvent(msg, wParam)) {
        // Coalesce bursts (RESUMEAUTOMATIC+RESUMESUSPEND, power+manual unlock):
        // one pending timer per window, refreshed by each event, fires ONE
        // reinstall. The debounce window is registered against the window that
        // saw the event so the reinstall (which also re-arms via that window on
        // a busy-worker deferral) uses the same timer owner.
        ::SetTimer(hwnd, kTimerHookReinstall, kHookReinstallDebounceMs, nullptr);
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    if (msg == WM_TIMER) {
        if (wParam == kTimerHookReinstall) {
            ::KillTimer(hwnd, kTimerHookReinstall);
            ReinstallHooksAfterLifecycleEvent("resume/unlock", hwnd);
            return 0;
        }
        if (wParam == kTimerHookHealth) {
            // Watchdog: thread-death detection (a dead hook thread leaves
            // IsHealthy false; a live-but-silently-removed hook cannot be
            // detected from user mode, hence the event-driven full reinstall).
            bool unhealthy = false;
            if (g_pHook && g_pHook->IsRunning() && !g_pHook->IsHealthy()) {
                unhealthy = true;
            }
            if (g_pMouseHook && g_pMouseHook->IsRunning() && !g_pMouseHook->IsHealthy()) {
                unhealthy = true;
            }
            if (unhealthy) {
                fprintf(stderr, "MAIN/HookLifecycle/003: health watchdog detected dead hook thread; reinstalling\n");
                ReinstallHooksAfterLifecycleEvent("health check", hwnd);
            }
            return 0;
        }
    }

    if (msg == SystemTray::WM_TRAYICON) {
        if (lParam == WM_RBUTTONUP) {
            if (g_pTray) {
                g_pTray->ShowContextMenu();
            }
            return 0;
        } else if (lParam == WM_LBUTTONUP) {
            if (g_pHook) {
                g_pHook->ToggleActive();
            }
            return 0;
        } else if (lParam == WM_LBUTTONDBLCLK) {
            if (g_pBadge) {
                g_pBadge->SetVisible(!g_pBadge->IsVisible());
            }
            return 0;
        }
    }

    if (msg == WM_DESTROY) {
        ::KillTimer(hwnd, kTimerHookReinstall);
        ::KillTimer(hwnd, kTimerHookHealth);
        ::PostQuitMessage(0);
        return 0;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

} // namespace emebalachat

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)pCmdLine;
    (void)nCmdShow;

    // -1. REQ-R15 (audit §5 latent item 3): declare Per-Monitor-V2 DPI
    // awareness BEFORE any window or DC is created. Until this call the
    // process ran DPI-UNAWARE (no manifest, no API call anywhere): on mixed
    // 100%/150%/200% multi-monitor setups Windows bitmap-stretched the badge,
    // drag icon and tooltip (blurry, wrong physical size) while the LL mouse
    // hooks delivered raw per-monitor physical coordinates - the exact
    // offset/scale mismatch the audit asked about. All UI surfaces below
    // re-scale their DIB buffers with ui::MonitorDpiAtPoint/WindowDpi per
    // monitor they land on.
    if (!emebalachat::ui::EnsurePerMonitorV2ProcessDpiAwareness()) {
        fprintf(stderr, "MAIN/WinMain/000: per-monitor-v2 DPI awareness unavailable; UI may be bitmap-scaled\n");
    }

    // 0. L3 (DLL search-path hardening, behavior-preserving part): remove the
    // current working directory from the default DLL search order so a later
    // delay-load of cublas/cublasLt/cudart (CMakeLists /DELAYLOAD) can never be
    // hijacked by an attacker-writable CWD (e.g. explorer "start in" on a temp
    // folder). PATH and the application directory remain searched, so CUDA
    // resolution via installed toolkit/driver directories is unaffected.
    // SetDefaultDllDirectories(...) is deliberately NOT called here: it would
    // drop PATH, and the installer ships no CUDA DLLs, so GPU acceleration on
    // toolkit machines resolves through PATH - restricting it breaks the CUDA
    // load chain (deferred; documented in CMakeLists.txt).
    ::SetDllDirectoryW(nullptr);

    // 1. Single Instance Mutex
    HANDLE hMutex = ::CreateMutexW(nullptr, TRUE, L"Global\\Emebalachat_SingleInstance");
    if (!hMutex || ::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::MessageBoxW(
            nullptr,
            L"Emebala Chat is already running in the background.\nCheck the system notification tray.",
            L"Emebala Chat",
            MB_OK | MB_ICONINFORMATION
        );
        if (hMutex) {
            ::CloseHandle(hMutex);
        }
        return 0;
    }

    // 2. Initialize COM for Direct2D & DirectWrite (I1 fix: check the result).
    // RPC_E_CHANGED_MODE means another component already initialized a different
    // apartment model on this thread; COM is still usable, so it is tolerated.
    // Any other failure means the COM-dependent visual layer (Direct2D badge /
    // DirectWrite / WIC logo / SAPI TTS) cannot work: log a traceable code and
    // warn the user once, then continue running the non-COM core (tray icon,
    // keyboard/mouse hooks, translation pipeline, Beep sounds) rather than
    // crashing or failing silently with an invisible UI.
    const HRESULT hrCom = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool com_available = SUCCEEDED(hrCom) || (hrCom == RPC_E_CHANGED_MODE);
    if (!com_available) {
        fprintf(stderr, "MAIN/WinMain/001: CoInitializeEx failed 0x%08lX; D2D badge, logo bitmaps and TTS are disabled\n",
                static_cast<unsigned long>(hrCom));
        ::MessageBoxW(
            nullptr,
            L"COM initialization failed.\nThe floating badge and text-to-speech will be unavailable,\nbut translation, hotkeys, tray and sounds still work.",
            L"Emebala Chat",
            MB_OK | MB_ICONWARNING
        );
    }

    // 3. Load or create AppConfig
    emebalachat::AppConfig config;
    config.LoadFromFile();

    // 4. Initialize Universal i18n
    emebalachat::I18n::Initialize(config.ui_language);
    if (config.target_language.empty()) {
        config.target_language = emebalachat::I18n::GetDefaultTargetLanguage(emebalachat::I18n::GetCurrentLocale());
    }

    // 5. Initialize Sound State
    emebalachat::SetSoundEnabled(config.sound_enabled);

    // 5. Initialize Translation Manager
    emebalachat::EngineType engine_type = emebalachat::EngineType::Auto;
    if (config.engine_type == "google") {
        engine_type = emebalachat::EngineType::GoogleTranslate;
    } else if (config.engine_type == "local") {
        engine_type = emebalachat::EngineType::LocalLlama;
    }
    // REQ-R11 (audit §4 M3): normalize a relative model_path against the
    // EXECUTABLE directory, not the CWD. Run-registry autostart launches with
    // CWD=C:\Windows\System32, where the CWD-relative "models/...gguf" never
    // resolves and model loading failed. Resolving HERE - at the single
    // config->engine handoff - makes the path absolute BEFORE it reaches both
    // validation time (IsValidModelPath, src/engine.cpp) and load time
    // (llama_model_load_from_file), so the two can never disagree. The
    // persisted config.json value stays untouched (relative = portable across
    // install locations). An explicitly emptied config.model_path is restored
    // to the AppConfig default before resolution so the engine's internal
    // CWD-relative fallback (engine.cpp ctor) is never reached in the app flow.
    std::string model_path_raw = config.model_path;
    if (model_path_raw.empty()) {
        model_path_raw = emebalachat::AppConfig{}.model_path;
    }
    const std::string resolved_model_path =
        emebalachat::ResolveModelPath(model_path_raw);
    emebalachat::TranslationManager engine(engine_type, resolved_model_path);
    // I3: this is the single source of truth - main.cpp loaded config.json once
    // and pushes every value into the engine explicitly (the engine constructor
    // no longer performs its own disk load).
    engine.SetSamplingParams(config.temperature, config.top_p, config.top_k, config.repetition_penalty);
    // H2 consent gate: propagate the privacy-first cloud_fallback_enabled flag so
    // the engine never silently transmits typed text to Google without consent.
    engine.SetCloudFallbackEnabled(config.cloud_fallback_enabled);

    // 6. Create Hidden Controller Window for Tray & Message Pump
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = emebalachat::ControllerWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = emebalachat::kControllerClassName;
    ::RegisterClassExW(&wc);

    HWND hController = ::CreateWindowExW(
        0,
        emebalachat::kControllerClassName,
        L"Emebala Chat Controller",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInstance, nullptr
    );
    emebalachat::g_hControllerWnd = hController;

    // REQ-R14 (audit §5 latent item 2): lifecycle event sink for the hook
    // re-registration policy. WM_POWERBROADCAST (Sleep/Resume) is delivered
    // ONLY to top-level windows - the message-only controller above can never
    // receive it - so create a hidden zero-size tool window with the same
    // WndProc. WTS_SESSION_LOCK/UNLOCK (Win+L, Ctrl+Alt+Del, RDP reconnect)
    // are registered on it too (NOTIFY_FOR_THIS_SESSION: no SYSTEM_SERVICE_
    // RIGHTS needed for a normal interactive session). The UAC secure-desktop
    // trip itself delivers no message at all; ControllerWndProc's periodic
    // health-check watchdog (armed in section 9) is the belt-and-braces for
    // silent OS unhook around desktop switches.
    {
        WNDCLASSEXW wc_ps = {};
        wc_ps.cbSize = sizeof(WNDCLASSEXW);
        wc_ps.lpfnWndProc = emebalachat::ControllerWndProc;
        wc_ps.hInstance = hInstance;
        wc_ps.lpszClassName = emebalachat::kPowerSinkClassName;
        ::RegisterClassExW(&wc_ps);

        HWND hPower = ::CreateWindowExW(
            WS_EX_TOOLWINDOW,
            emebalachat::kPowerSinkClassName,
            L"Emebala Chat Power Sink",
            WS_POPUP,
            -1000, -1000, 0, 0,
            nullptr, nullptr, hInstance, nullptr
        );
        emebalachat::g_hPowerWnd = hPower;
        if (!hPower) {
            fprintf(stderr, "MAIN/WinMain/005: power sink window creation failed (GLE %lu); Sleep/Resume hook reinstall degraded to watchdog-only\n",
                    ::GetLastError());
        } else if (!::WTSRegisterSessionNotification(hPower, NOTIFY_FOR_THIS_SESSION)) {
            fprintf(stderr, "MAIN/WinMain/006: WTSRegisterSessionNotification failed (GLE %lu); Win+L hook reinstall degraded to watchdog-only\n",
                    ::GetLastError());
        }
    }

    // 7. Initialize Direct2D Floating Badge (restoring saved position if present)
    emebalachat::FloatingBadge badge;
    emebalachat::g_pBadge = &badge;
    // Create() fails cleanly (returns false) when COM/D2D are unavailable; the
    // badge's public methods all no-op with a null hwnd_, so the rest of the
    // app keeps running without the visual pill (I1 graceful degradation).
    if (!badge.Create(
            hInstance,
            emebalachat::ToUtf16(config.source_language),
            emebalachat::ToUtf16(config.target_language),
            config.badge_x,
            config.badge_y
        )) {
        fprintf(stderr, "MAIN/WinMain/002: FloatingBadge::Create failed; running without the badge UI\n");
    }

    // Persist badge desktop coordinates whenever dragged by the user
    badge.SetPositionCallback([&](int x, int y) {
        config.SetBadgePosition(x, y);
        config.SaveToFile();
    });

    // Asynchronously warm up local model in background so first translation is instantaneous (< 100ms)
    std::thread warmup_thread;
    if (engine.IsLocalModelAvailable()) {
        warmup_thread = std::thread([&engine]() {
            engine.PreloadLocalModel();
        });
    }

    // 8. Initialize System Tray with Callbacks
    emebalachat::SystemTray tray;
    emebalachat::g_pTray = &tray;

    emebalachat::PipelineWorker worker(config, engine, badge);
    emebalachat::KeyboardHook hook(config, worker, badge, tray);
    emebalachat::g_pHook = &hook;

    // 8.5. Initialize Drag-to-Translate Components
    emebalachat::DragIconWindow drag_icon;
    drag_icon.Create(hInstance);

    emebalachat::TooltipWindow tooltip;
    tooltip.Create(hInstance);

    emebalachat::MouseHook mouse_hook;
    emebalachat::g_pMouseHook = &mouse_hook; // REQ-R14 resume/unlock re-registration

    emebalachat::SystemTray::Callbacks trayCallbacks;
    trayCallbacks.on_toggle_active = [&]() {
        // REQ-R07: SetActive() fires active_change_cb_ on every real state
        // change, so the MouseHook sync happens there exactly once. No manual
        // SetEnabled call here anymore - it double-toggled nothing before only
        // because SetEnabled is idempotent; keeping it would risk drift.
        hook.ToggleActive();
    };

    trayCallbacks.on_select_engine = [&](int engine_idx) {
        // I4: runtime mutations go through the locked setters because the hook
        // and worker threads read these fields concurrently.
        if (engine_idx == 0) {
            engine.SetEngineType(emebalachat::EngineType::GoogleTranslate);
            config.SetEngineTypeName("google");
        } else {
            engine.SetEngineType(emebalachat::EngineType::LocalLlama);
            config.SetEngineTypeName("local");
        }
        config.SaveToFile();
        const auto snap = config.GetSnapshot();
        tray.UpdateStatus(
            hook.IsActive(),
            engine.GetActiveEngineName(),
            snap.source_language,
            snap.target_language,
            snap.auto_send,
            snap.sound_enabled,
            badge.IsVisible()
        );
    };

    trayCallbacks.on_select_source_lang = [&](std::string_view code) {
        config.SetSourceLanguage(std::string(code)); // I4: locked setter
        config.SaveToFile();
        const auto snap = config.GetSnapshot();
        badge.SetLanguages(emebalachat::ToUtf16(snap.source_language), emebalachat::ToUtf16(snap.target_language));
        tray.UpdateStatus(
            hook.IsActive(),
            engine.GetActiveEngineName(),
            snap.source_language,
            snap.target_language,
            snap.auto_send,
            snap.sound_enabled,
            badge.IsVisible()
        );
    };

    trayCallbacks.on_select_target_lang = [&](std::string_view name) {
        config.SetTargetLanguage(std::string(name)); // I4: locked setter
        config.SaveToFile();
        const auto snap = config.GetSnapshot();
        badge.SetLanguages(emebalachat::ToUtf16(snap.source_language), emebalachat::ToUtf16(snap.target_language));
        tray.UpdateStatus(
            hook.IsActive(),
            engine.GetActiveEngineName(),
            snap.source_language,
            snap.target_language,
            snap.auto_send,
            snap.sound_enabled,
            badge.IsVisible()
        );
    };

    trayCallbacks.on_swap_languages = [&]() {
        const auto snap_in = config.GetSnapshot(); // I4: consistent read for the swap logic
        std::string current_src_norm = emebalachat::NormalizeLanguageCode(snap_in.source_language);
        std::string current_tgt_norm = emebalachat::NormalizeLanguageCode(snap_in.target_language);

        std::string new_src;
        std::string new_tgt;

        if (current_src_norm == "AUTO") {
            // When source is Auto Detect (e.g. Auto Detect -> English)
            // new source becomes current target (English)
            const auto* pTgtInfo = emebalachat::FindLanguageByCode(current_tgt_norm);
            new_src = pTgtInfo ? pTgtInfo->name_en : snap_in.target_language;

            // new target becomes user's OS native language (or English if native is English)
            std::string sys_lang = emebalachat::I18n::GetSystemLanguageCode();
            std::string sys_norm = emebalachat::NormalizeLanguageCode(sys_lang);
            if (sys_norm == "AUTO" || sys_norm.empty()) sys_norm = "KO";

            if (current_tgt_norm == sys_norm) {
                new_tgt = (sys_norm == "EN") ? "Korean" : "English";
            } else {
                const auto* pSysInfo = emebalachat::FindLanguageByCode(sys_norm);
                new_tgt = pSysInfo ? pSysInfo->name_en : "Korean";
            }
        } else {
            // Direct swap between two concrete languages (e.g. Korean <-> English)
            new_src = snap_in.target_language;
            new_tgt = snap_in.source_language;
        }

        config.SetLanguages(std::move(new_src), std::move(new_tgt)); // I4: single locked write
        config.SaveToFile();

        const auto snap = config.GetSnapshot();
        badge.SetLanguages(emebalachat::ToUtf16(snap.source_language), emebalachat::ToUtf16(snap.target_language));
        tray.UpdateStatus(
            hook.IsActive(),
            engine.GetActiveEngineName(),
            snap.source_language,
            snap.target_language,
            snap.auto_send,
            snap.sound_enabled,
            badge.IsVisible()
        );
        emebalachat::PlayLangChange();
    };

    trayCallbacks.on_toggle_auto_send = [&]() {
        hook.ToggleAutoSend();
    };

    trayCallbacks.on_toggle_sound = [&]() {
        // I4: sound_enabled is std::atomic (read by hook thread for beep gating).
        const bool next = !config.sound_enabled.load(std::memory_order_relaxed);
        config.sound_enabled.store(next, std::memory_order_relaxed);
        config.SaveToFile();
        emebalachat::SetSoundEnabled(next);
        const auto snap = config.GetSnapshot();
        tray.UpdateStatus(
            hook.IsActive(),
            engine.GetActiveEngineName(),
            snap.source_language,
            snap.target_language,
            snap.auto_send,
            next,
            badge.IsVisible()
        );
    };

    trayCallbacks.on_toggle_badge = [&]() {
        badge.SetVisible(!badge.IsVisible());
        // REQ-R12 (audit §4 I4, main.cpp:344): the tray callback runs on the
        // GUI thread while the hook thread's Ctrl+F9 cycle writes
        // target_language under mutex_ (hook.cpp CycleLanguage). Reading
        // config.source_language/target_language directly here was the
        // remaining data race. GetSnapshot() is the thread-safe accessor the
        // other tray callbacks already use.
        const auto snap = config.GetSnapshot();
        tray.UpdateStatus(
            hook.IsActive(),
            engine.GetActiveEngineName(),
            snap.source_language,
            snap.target_language,
            snap.auto_send,
            snap.sound_enabled,
            badge.IsVisible()
        );
    };

    trayCallbacks.on_toggle_start_with_windows = [&]() {
        // State toggled inside tray menu
    };

    trayCallbacks.on_show_cheat_sheet = [&]() {
        ::MessageBoxW(
            nullptr,
            emebalachat::I18n::Get(emebalachat::StringId::CheatSheetBody).c_str(),
            emebalachat::I18n::Get(emebalachat::StringId::CheatSheetTitle).c_str(),
            MB_OK | MB_ICONINFORMATION
        );
    };

    trayCallbacks.on_exit = [&]() {
        ::PostQuitMessage(0);
    };

    tray.Create(hController, hInstance, trayCallbacks);
    // REQ-R12 (I4 sweep): startup-only call (hook/worker threads start in
    // section 9, so direct field reads were already safe under config.hpp's
    // I4 contract), converted to GetSnapshot() for uniform accessor
    // discipline across every UpdateStatus call site.
    {
        const auto snap = config.GetSnapshot();
        tray.UpdateStatus(
            true,
            engine.GetActiveEngineName(),
            snap.source_language,
            snap.target_language,
            snap.auto_send,
            snap.sound_enabled,
            badge.IsVisible()
        );
    }

    // Drag release threshold callback (> 15px). Runs on the mouse-hook thread
    // (drag) or the delayed-click worker thread (multi-click settle) - never on
    // the GUI thread, so every D2D touch below MUST be marshaled.
    mouse_hook.SetDragReleaseCallback([&](int x, int y) {
        // REQ-R12 (I4 sweep): this lambda runs on the mouse-hook thread.
        // drag_to_translate is startup-written/immutable per config.hpp's I4
        // contract, but it is also part of Snapshot, so reading it through
        // GetSnapshot() future-proofs against live config reload and matches
        // the accessor discipline every other cross-thread callback uses.
        // Uncontended mutex_ (~µs) - acceptable on the hook thread (D3 audit).
        const auto snap = config.GetSnapshot();
        if (!snap.drag_to_translate || !hook.IsActive() || tooltip.IsVisible()) {
            return;
        }
        // REQ-R10 (audit §3.4): the single-threaded D2D render target belongs
        // to the main GUI thread. Call ShowAt directly here produced
        // D2DERR_WRONG_THREAD and an invisible icon. PostMessage to the icon's
        // own window: non-blocking for the hook thread, WndProc renders on the
        // GUI thread.
        emebalachat::DragIconWindow::RequestShowAt(drag_icon.GetHwnd(), x + 12, y + 12);
    });

    // Outside-click dismissal for DragIconWindow and TooltipWindow
    mouse_hook.SetMouseDownCallback([&](int x, int y) {
        POINT pt = { x, y };
        if (drag_icon.IsVisible()) {
            RECT r = {};
            ::GetWindowRect(drag_icon.GetHwnd(), &r);
            if (!::PtInRect(&r, pt)) {
                drag_icon.Hide();
            }
        }
        if (tooltip.IsVisible()) {
            RECT r = {};
            ::GetWindowRect(tooltip.GetHwnd(), &r);
            if (!::PtInRect(&r, pt)) {
                tooltip.Dismiss();
            }
        }
    });

    // Click on DragIconWindow triggers translation of active selection.
    // Invoked from DragIconWindow::WndProc => runs ON THE MAIN GUI THREAD.
    drag_icon.SetClickCallback([&](int click_x, int click_y) {
        emebalachat::ClipboardBackup backup;
        emebalachat::BackupClipboard(backup);

        // D2-flagged (report issue 1): the old CopySelection(); Sleep(35);
        // pattern had the same stale-read race as audit §2.3 for slow (Electron
        // IPC) targets. The D2 public seam confirms the copy via
        // GetClipboardSequenceNumber() polling and returns false instead of
        // exposing stale clipboard text.
        if (!emebalachat::CopySelectionWithSequenceWait()) {
            emebalachat::RestoreClipboard(backup);
            return;
        }
        std::wstring selected = emebalachat::GetClipboardText();
        emebalachat::RestoreClipboard(backup);

        if (selected.empty() || selected.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
            return;
        }

        std::string detected = emebalachat::DetectLanguage(selected);
        std::string src_code = emebalachat::NormalizeLanguageCode(detected);
        std::string tgt_lang = config.GetSnapshot().target_language; // I4: snapshot read (this runs on the GUI thread)
        std::string tgt_code = emebalachat::NormalizeLanguageCode(tgt_lang);

        if (tgt_code == src_code) {
            std::string sys_lang = emebalachat::I18n::GetSystemLanguageCode();
            std::string sys_code = emebalachat::NormalizeLanguageCode(sys_lang);
            if (sys_code == src_code || sys_code.empty()) {
                tgt_lang = (src_code == "EN") ? "Korean" : "English";
            } else {
                const auto* pInfo = emebalachat::FindLanguageByCode(sys_code);
                tgt_lang = pInfo ? pInfo->name_en : "Korean";
            }
        }

        badge.SetStatus(emebalachat::BadgeStatus::Translating);
        std::wstring translated = engine.Translate(selected, detected, tgt_lang);
        badge.SetStatus(emebalachat::BadgeStatus::Active);

        // Copy path is main-thread; direct D2D call is correct here.
        tooltip.ShowTranslation(click_x, click_y, selected, src_code, tgt_lang, translated);
    });

    // Double Ctrl+C Hotkey Detection (< 400ms).
    // REQ-R06 (audit §2.5): KeyboardHook dispatches this callback onto its own
    // persistent worker thread, NOT the LowLevelKeyboardProc thread - engine
    // inference below can run seconds and would get WH_KEYBOARD_LL silently
    // unhooked by the OS once past LowLevelHooksTimeout. Worker-thread rules:
    //  - clipboard reads and PostMessage-based badge updates are fine here;
    //  - NEVER call TooltipWindow/DragIconWindow render paths directly (their
    //    single-threaded D2D targets belong to the main GUI thread,
    //    D2DERR_WRONG_THREAD). tooltip.ShowTranslationThreadSafe() marshals.
    hook.SetDoubleCtrlCCallback([&]() {
        // REQ-R12 (I4 sweep): REQ-R06 worker thread -> snapshot read instead of
        // the direct config.drag_to_translate field access (see the drag
        // callback above for the same-pattern rationale).
        if (!config.GetSnapshot().drag_to_translate || !hook.IsActive()) {
            return;
        }

        // Settle for the target app's Ctrl+C clipboard write (same 40 ms the
        // old code used - now off the hook thread, so it is harmless).
        ::Sleep(40);
        std::wstring copied = emebalachat::GetClipboardText();
        if (copied.empty() || copied.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
            return;
        }

        std::string detected = emebalachat::DetectLanguage(copied);
        std::string src_code = emebalachat::NormalizeLanguageCode(detected);
        std::string tgt_lang = config.GetSnapshot().target_language; // I4: snapshot read (REQ-R06: runs on the hook's async worker thread)
        std::string tgt_code = emebalachat::NormalizeLanguageCode(tgt_lang);

        if (tgt_code == src_code) {
            std::string sys_lang = emebalachat::I18n::GetSystemLanguageCode();
            std::string sys_code = emebalachat::NormalizeLanguageCode(sys_lang);
            if (sys_code == src_code || sys_code.empty()) {
                tgt_lang = (src_code == "EN") ? "Korean" : "English";
            } else {
                const auto* pInfo = emebalachat::FindLanguageByCode(sys_code);
                tgt_lang = pInfo ? pInfo->name_en : "Korean";
            }
        }

        POINT cursor = {};
        ::GetCursorPos(&cursor);

        badge.SetStatus(emebalachat::BadgeStatus::Translating);
        std::wstring translated = engine.Translate(copied, detected, tgt_lang);
        badge.SetStatus(emebalachat::BadgeStatus::Active);

        // REQ-R10-adjacent: worker thread, so marshal the D2D render to the GUI
        // thread instead of calling ShowTranslation() directly (audit §3.4).
        tooltip.ShowTranslationThreadSafe(
            cursor.x + 12, cursor.y + 12, copied, src_code, tgt_lang, translated);
    });

    // ESC Key Dismissal
    hook.SetEscCallback([&]() -> bool {
        if (tooltip.IsVisible()) {
            tooltip.Dismiss();
            return true;
        }
        if (drag_icon.IsVisible()) {
            drag_icon.Hide();
            return true;
        }
        return false;
    });

    // Tooltip language switcher re-translation
    tooltip.SetLanguageChangeCallback([&](std::string_view new_target_lang) {
        std::wstring src = tooltip.GetSourceText();
        std::string src_code = tooltip.GetSourceLangCode();
        std::string new_tgt = std::string(new_target_lang);

        RECT r = {};
        ::GetWindowRect(tooltip.GetHwnd(), &r);

        badge.SetStatus(emebalachat::BadgeStatus::Translating);
        std::wstring translated = engine.Translate(src, src_code, new_tgt);
        badge.SetStatus(emebalachat::BadgeStatus::Active);

        tooltip.ShowTranslation(r.left, r.top, src, src_code, new_tgt, translated);
    });

    // REQ-R08 visual feedback: localized state-change bubble at the cursor,
    // audio chime comes from SetActive() itself. Runs on the hook thread
    // (Win+F9 path) or the main thread (badge/tray paths) - the thread-safe
    // tooltip seam marshals to the GUI thread either way.
    hook.SetActiveChangeCallback([&](bool active) {
        // REQ-R07 (audit §3.1): 1:1 sync. This lambda is the ONLY place that
        // touches mouse_hook.SetEnabled for activation state, and it fires on
        // every real SetActive() change - including the Win+F9 hotkey path that
        // previously left the mouse hook permanently disabled.
        mouse_hook.SetEnabled(active);

        POINT cursor = {};
        ::GetCursorPos(&cursor);
        const std::wstring body = std::wstring(active
                ? emebalachat::I18n::Get(emebalachat::StringId::BadgeActive)
                : emebalachat::I18n::Get(emebalachat::StringId::BadgePaused))
            + L" (" + emebalachat::kToggleHotkeyLabel + L")";
        tooltip.ShowMessageThreadSafe(cursor.x + 12, cursor.y + 12,
                                       emebalachat::kToggleHotkeyLabel, body);
    });

    // Badge Mouse Controls:
    // Single Click: Toggle Active / Paused
    badge.SetClickCallback([&]() {
        // Badge callbacks run on the GUI thread (badge WndProc). ToggleActive()
        // now syncs MouseHook through the REQ-R07 callback; the manual
        // SetEnabled(hook.IsActive()) that used to sit here read the flag
        // BEFORE the toggle completed and could re-disable an enabled hook.
        hook.ToggleActive();
    });

    // Double Click: Swap Source ⇄ Target
    badge.SetDoubleClickCallback([&]() {
        trayCallbacks.on_swap_languages();
    });

    // Right Click: Directly popup the full Settings Menu right at the badge!
    badge.SetRightClickCallback([&]() {
        tray.ShowContextMenu();
    });

    // 9. Start Pipeline Worker, Keyboard Hook, and Mouse Hook
    worker.Start();
    hook.Start();
    mouse_hook.Start();

    // REQ-R14 watchdog: re-verify hook thread liveness every 30 s (covers the
    // UAC secure-desktop trip and any silent OS removal path that emitted no
    // resume/unlock message to this session).
    ::SetTimer(hController, emebalachat::kTimerHookHealth,
               emebalachat::kHookHealthWatchdogMs, nullptr);

    // 10. Startup sound
    emebalachat::PlayToggleOn();

    // 11. Main Message Loop
    MSG msg = {};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    // 12. Clean Shutdown
    //
    // REQ-R16 (audit §5 latent item 4): deterministic, cancellation-first
    // teardown. Order matters:
    //  a) engine.RequestCancel() latches the llama.cpp stop flag. An in-flight
    //     decode unwinds at the next token boundary (the decode loop checks
    //     between steps; the CPU path also aborts mid-batch via
    //     llama_context_params.abort_callback), and an in-progress model-load
    //     on the WARMUP thread unwinds via llama_model_params.progress_callback.
    //     Without this latch, hook.Stop()/worker.Stop() below could block for
    //     seconds inside a 512-token generation, and the process could exit
    //     while the warmup thread still held llama.cpp state (zombie thread ->
    //     leaked CUDA context at process teardown).
    //  b) The old code NEVER joined warmup_thread: a still-loading thread
    //     destroyed as joinable std::thread called std::terminate. It is now
    //     bounded-joined after the cancel latch.
    //  c) WaitInferenceIdle gives the bounded (2 s) confirmation that no
    //     llama.cpp call is still on any thread before the backend is freed
    //     by ~TranslationManager (llama_free/llama_model_free/
    //     llama_backend_free ordering: ctx before model, both before backend).
    engine.RequestCancel();
    mouse_hook.Stop();
    hook.Stop();
    worker.Stop();
    if (warmup_thread.joinable()) {
        warmup_thread.join(); // bounded: load aborts via progress_callback
    }
    if (!engine.WaitInferenceIdle(2000)) {
        fprintf(stderr, "MAIN/Shutdown/004: engine did not report idle within 2 s; forcing teardown (decode loop may still be winding down)\n");
    }
    if (emebalachat::g_hPowerWnd) {
        ::WTSUnRegisterSessionNotification(emebalachat::g_hPowerWnd);
    }
    tray.Destroy();
    badge.Destroy();
    tooltip.Destroy();
    drag_icon.Destroy();

    if (hController) {
        ::DestroyWindow(hController);
    }
    if (emebalachat::g_hPowerWnd) {
        ::DestroyWindow(emebalachat::g_hPowerWnd);
        emebalachat::g_hPowerWnd = nullptr;
    }
    emebalachat::g_pMouseHook = nullptr;
    emebalachat::g_pHook = nullptr;
    emebalachat::g_pBadge = nullptr;
    emebalachat::g_pTray = nullptr;
    emebalachat::g_hControllerWnd = nullptr;

    config.SaveToFile();
    ::CoUninitialize();

    if (hMutex) {
        ::ReleaseMutex(hMutex);
        ::CloseHandle(hMutex);
    }

    return 0;
}
