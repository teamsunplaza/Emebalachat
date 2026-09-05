#include "config.hpp"
#include "engine.hpp"
#include "hook.hpp"
#include "i18n.hpp"
#include "version.hpp"
#include "mouse_hook.hpp"
#include "smart_bypass.hpp"
#include "sound.hpp"
#include "unicode_utils.hpp"
#include "win32_input.hpp"
#include "ui/about_window.hpp"
#include "ui/badge.hpp"
#include "ui/drag_icon.hpp"
#include "ui/dpi.hpp"
#include "ui/tooltip.hpp"
#include "ui/tray.hpp"
#include "worker.hpp"

#include <windows.h>
#include <objbase.h>
#include <wtsapi32.h> // WTSRegisterSessionNotification (REQ-R14)

#include <condition_variable> // R6 Phase 3 (audit item 7): joinable drag worker
#include <cstdio>
#include <functional> // R6 Phase 1 (B3): language-sync coordinator std::function
#include <memory>     // R6 Phase 1 (B3): payload ownership in the sync marshal
#include <mutex>      // R6 Phase 3 (audit item 7): drag job slot guard
#include <string_view>
#include <thread> // REQ-R1: drag-icon click worker (copy+translate off the GUI thread)

namespace emebalachat {

namespace {
// ---- R6 Phase 1 (B3): cross-thread language-sync marshal ----
// Posted to the controller window (GUI thread) by any NON-GUI thread that
// needs a language mutation applied: the keyboard hook thread's Ctrl+F9
// cycle (wParam=1, cycle semantics) and the drag / double-Ctrl+C worker
// threads when their src==tgt fallback substitutes a new target (wParam=0,
// payload carries the request). LPARAM is a heap LanguageSyncRequest whose
// ownership transfers to ControllerWndProc (deleted locally on post failure
// - the same REQ-R10 payload contract the tooltip seams use).
constexpr UINT kMsgApplyLanguageSync = WM_APP + 0x300;
struct LanguageSyncRequest {
    std::string source;      // empty = keep current
    std::string target;      // empty = keep current
    bool play_chime = false;
};
// Set once at startup to the wWinMain ApplyLanguageChange coordinator; invoked
// on the GUI thread from ControllerWndProc. Cleared after hook/mouse stop at
// shutdown so a late posted message can never call into destroyed state.
std::function<bool(std::string_view, std::string_view, bool, bool)> g_apply_language_change;
// Posts a sync request; never blocks (PostMessageW). Safe from hook threads.
// When the CALLER already is the controller window's GUI thread (e.g. a test
// invoking KeyboardHook::CycleTargetLanguage directly, or any future same-
// thread wiring), posting would defer behind the rest of the queue, so the
// coordinator runs inline instead - it is documented GUI-thread-only and
// re-entrant here (its config writes are locked, its view seams marshal).
void RequestLanguageSync(HWND hController, std::string source, std::string target,
                         bool cycle, bool play_chime) {
    if (!hController) {
        fprintf(stderr, "MAIN/LangSync/000: no controller window; language sync request dropped\n");
        return;
    }
    const DWORD gui_tid = ::GetWindowThreadProcessId(hController, nullptr);
    if (g_apply_language_change && gui_tid == ::GetCurrentThreadId()) {
        g_apply_language_change(source, target, cycle, play_chime);
        return;
    }
    auto p = std::make_unique<LanguageSyncRequest>();
    p->source = std::move(source);
    p->target = std::move(target);
    p->play_chime = play_chime;
    const LPARAM lp = reinterpret_cast<LPARAM>(p.release());
    if (::PostMessageW(hController, kMsgApplyLanguageSync, cycle ? 1 : 0, lp) == FALSE) {
        delete reinterpret_cast<LanguageSyncRequest*>(lp);
        fprintf(stderr, "MAIN/LangSync/002: PostMessage language sync failed (GLE %lu)\n", ::GetLastError());
    }
}
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

    if (msg == kMsgApplyLanguageSync) {
        // R6 Phase 1 (B3, plan §2.3): hook/worker-thread language mutations
        // marshal here so they run on the GUI thread through the same
        // ApplyLanguageChange coordinator as every other surface. wParam=1
        // requests a target-language cycle (payload strings ignored);
        // wParam=0 applies the payload's (source,target) request.
        const std::unique_ptr<LanguageSyncRequest> p(
            reinterpret_cast<LanguageSyncRequest*>(lParam));
        if (g_apply_language_change) {
            g_apply_language_change(
                p ? std::string_view{ p->source } : std::string_view{},
                p ? std::string_view{ p->target } : std::string_view{},
                wParam != 0,
                p ? p->play_chime : true);
        }
        return 0;
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
    // NOTE (R6 Phase 5 sweep): the two startup MessageBox texts below are now
    // routed through i18n (StringId::AppAlreadyRunning / AppComFailed). They
    // run BEFORE I18n::Initialize (config not loaded yet on the second
    // instance), so they render the English table - behaviorally identical to
    // the old hardcoded literals, but the strings live in exactly one place.
    HANDLE hMutex = ::CreateMutexW(nullptr, TRUE, L"Global\\Emebalachat_SingleInstance");
    if (!hMutex || ::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::MessageBoxW(
            nullptr,
            emebalachat::I18n::Get(emebalachat::StringId::AppAlreadyRunning).c_str(),
            emebalachat::kAppNameW.data(),
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
            emebalachat::I18n::Get(emebalachat::StringId::AppComFailed).c_str(),
            emebalachat::kAppNameW.data(),
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

    // R5 (Debug-Surgical): surface the Enter-path empty-capture failure that
    // was previously silent. Same contract as the drag-path failure notice
    // (REQ-R1(b)): the callback runs on the PIPELINE WORKER thread, and
    // TooltipWindow::ShowMessageThreadSafe is the existing REQ-R10 thread-
    // safe seam that marshals the D2D render onto the GUI thread. Registered
    // once at startup BEFORE worker.Start() (read-only afterwards, same
    // discipline as every other SetXxxCallback).
    worker.SetEmptyCaptureCallback([&tooltip]() {
        POINT cur;
        if (!::GetCursorPos(&cur)) {
            cur = { 0, 0 };
        }
        // R6 Phase 5 (plan §5.3): notice header literal -> localized
        // StringId::TooltipTitle (was the hardcoded L"Emebala Chat" header).
        tooltip.ShowMessageThreadSafe(cur.x, cur.y,
                                       emebalachat::I18n::Get(emebalachat::StringId::TooltipTitle),
                                       emebalachat::I18n::Get(emebalachat::StringId::TooltipNoSelection));
    });

    // REQ-005 (plan §2.2): branded About popup. Singleton next to the tooltip;
    // Create failures degrade to a no-op About menu item (all public methods
    // guard hwnd_), mirroring the badge's graceful-degradation contract.
    emebalachat::AboutWindow about_window;
    if (!about_window.Create(hInstance)) {
        fprintf(stderr, "MAIN/WinMain/007: AboutWindow::Create failed; About menu item disabled\n");
    }

    emebalachat::MouseHook mouse_hook;
    emebalachat::g_pMouseHook = &mouse_hook; // REQ-R14 resume/unlock re-registration

    // R6 Phase 1 (B3, architect plan §2.3/§2.4 Option A): the single-source-of-
    // truth language coordinator. AppConfig is the ONE authority; every language
    // mutation - from ANY surface (tooltip language menu, tray source/target
    // submenus, tray Swap / badge double-click, Ctrl+F9 cycle, startup config
    // load) - routes through this one function so config.json, the badge, the
    // tray tip/checkmarks and the visible tooltip can never drift apart again
    // (the reported B3 desync bug). All decision logic lives in the pure
    // PlanLanguageSync seam (config.hpp), pinned by TestB3LanguageSync.
    //
    // Thread contract (plan §2.3): must run on the GUI thread. Every tray/menu/
    // tooltip callback already does; the hook thread's Ctrl+F9 cycle and the
    // drag / double-Ctrl+C worker threads marshal through RequestLanguageSync
    // (kMsgApplyLanguageSync on the controller window). Badge/tray/tooltip view
    // updates additionally marshal internally (REQ-R10 seams), so this function
    // never touches another thread's D2D target directly.
    //
    // Returns true when the (possibly no-op) plan was applied. Invalid
    // (unresolvable) requests are refused WITHOUT any write - config and all
    // surfaces keep the previous consistent state (INV-1 guard).
    auto ApplyLanguageChange =
        [&](std::string_view new_source, std::string_view new_target,
            bool cycle_target, bool play_chime) -> bool {
        auto snap = config.GetSnapshot(); // I4: consistent read
        std::string req_src(new_source);
        std::string req_tgt(new_target);
        if (cycle_target) {
            // Ctrl+F9 cycle: next target from the CURRENT persisted target.
            req_tgt = emebalachat::CycleTargetLanguage(snap.target_language);
        }
        const emebalachat::LanguageSyncPlan plan = emebalachat::PlanLanguageSync(
            snap.source_language, snap.target_language, req_src, req_tgt);
        if (!plan.valid) {
            fprintf(stderr, "MAIN/LangSync/001: refused unresolvable language request (src='%s', tgt='%s')\n",
                    std::string(new_source).c_str(), std::string(new_target).c_str());
            return false;
        }
        // INV-3: persistence is synchronous inside this call. A no-op re-pick
        // of the already-active language skips the locked write and the disk
        // churn, but the view refreshes below still run (self-heal against
        // any external drift).
        if (plan.changed) {
            config.SetLanguages(plan.source_language, plan.target_language); // I4: single locked write
            config.SaveToFile();
            snap = config.GetSnapshot(); // read the authoritative post-write state
        }
        // plan.surface_updates order (planner): Badge -> Tray -> Tooltip.
        for (const emebalachat::LanguageSurface surface : plan.surface_updates) {
            switch (surface) {
                case emebalachat::LanguageSurface::Badge:
                    badge.SetLanguages(emebalachat::ToUtf16(snap.source_language),
                                       emebalachat::ToUtf16(snap.target_language));
                    break;
                case emebalachat::LanguageSurface::Tray:
                    tray.UpdateStatus(
                        hook.IsActive(),
                        engine.GetActiveEngineName(),
                        snap.source_language,
                        snap.target_language,
                        snap.auto_send,
                        snap.sound_enabled,
                        badge.IsVisible()
                    );
                    break;
                case emebalachat::LanguageSurface::Tooltip:
                    // Best-effort view sync: no-op while hidden/message-mode.
                    tooltip.RefreshTargetLanguageFromConfig(snap.target_language);
                    break;
            }
        }
        if (plan.changed && play_chime) {
            emebalachat::PlayLangChange();
        }
        return true;
    };
    // Publish for the controller WndProc (hook/worker-thread marshal target).
    emebalachat::g_apply_language_change = ApplyLanguageChange;

    // R6 Phase 6 (plan §5.4): locale-change propagation coordinator, mirroring
    // the ApplyLanguageChange pattern above. After I18n::SetLocale, re-render
    // every surface in the plan's order (tray -> badge -> tooltip -> About).
    // GUI-thread-only: called from the tray selector callback (its
    // TrackPopupMenuEx runs on this thread). Surfaces that cache localized
    // strings outside Render() are handled explicitly:
    //   * tray: menu rebuilds lazily on every ShowContextMenu (fresh I18n::Get
    //     each build); UpdateStatus refreshes the localized icon tip.
    //   * badge: SetLanguages nudge forces a repaint of the localized parts.
    //   * tooltip: RefreshTargetLanguageFromConfig repaints while visible (the
    //     same value is fine - it is a view-refresh nudge); hidden tooltips
    //     pick up new strings on the next Show (plan: acceptable).
    //   * About: RequestLocaleRefresh re-applies the caption and repaints the
    //     localized body while visible.
    auto RefreshAllUiForLocaleChange = [&]() {
        const auto snap = config.GetSnapshot(); // I4: consistent read
        tray.SetUiLanguage(snap.ui_language);   // submenu check-mark mirror
        tray.UpdateStatus(
            hook.IsActive(),
            engine.GetActiveEngineName(),
            snap.source_language,
            snap.target_language,
            snap.auto_send,
            snap.sound_enabled,
            badge.IsVisible()
        );
        badge.SetLanguages(emebalachat::ToUtf16(snap.source_language),
                           emebalachat::ToUtf16(snap.target_language));
        tooltip.RefreshTargetLanguageFromConfig(snap.target_language);
        about_window.RequestLocaleRefresh();
    };
    // R6 Phase 1 (B3): Ctrl+F9 cycle now routes through the SAME coordinator.
    // Fires on the hook thread -> RequestLanguageSync posts to the controller
    // window (GUI thread). Set before hook.Start(); read-only afterwards
    // (active_change_cb_ contract).
    hook.SetLanguageCycleCallback([]() {
        emebalachat::RequestLanguageSync(emebalachat::g_hControllerWnd,
                                         std::string{}, std::string{}, true, true);
    });

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

    // R6 Phase 1 (B3): tray source/target submenu picks are REQUESTS to the
    // coordinator (plan §2.4). The previous hand-maintained config-write +
    // badge + tray sequences are deleted - the coordinator is now the only
    // writer (INV-2), and it additionally syncs a visible tooltip.
    trayCallbacks.on_select_source_lang = [&](std::string_view code) {
        ApplyLanguageChange(code, std::string_view{}, false, false);
    };

    trayCallbacks.on_select_target_lang = [&](std::string_view name) {
        ApplyLanguageChange(std::string_view{}, name, false, false);
    };

    trayCallbacks.on_swap_languages = [&]() {
        // R6 Phase 1 (B3): the swap POLICY (AUTO source becomes the concrete
        // old target, new target falls back to the OS-native language, plain
        // pairs swap directly) is unchanged; only the APPLY step moved into
        // the coordinator, which persists + refreshes badge/tray/tooltip in
        // one place instead of this hand-maintained sequence.
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

        // Coordinator is the ONLY writer from here (INV-2): canonicalizes,
        // persists synchronously (INV-3), refreshes badge -> tray -> tooltip.
        // The chime plays only on an actual change (same semantics as before,
        // which always played; a swap can never be a no-op except K<->K).
        ApplyLanguageChange(new_src, new_tgt, false, true);
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

    // REQ-005: tray menu "About Emebala Chat…" (Batch 1's guarded hook, now
    // wired). Tray callbacks run on the GUI thread; AboutWindow::Show centers
    // on the monitor under the current cursor position.
    trayCallbacks.on_show_about = [&]() {
        POINT cursor = {};
        ::GetCursorPos(&cursor);
        about_window.Show(cursor.x, cursor.y);
    };

    // R6 Phase 6 (plan §5.4): tray UI-language selector. Runs on the GUI
    // thread (tray callback). Validation + canonicalization + refusal of
    // unknown/removed codes (fr/de/ru) live in the pure PlanUiLocaleChange
    // seam (i18n.hpp), pinned by TestR6P5P6I18n. Persistence follows the I4
    // pattern: locked SetUiLanguage + synchronous SaveToFile (same contract
    // as the language coordinator's INV-3). A no-op re-pick skips the write
    // but still refreshes the views (self-heal). "auto" resolves to the
    // DETECTED system locale at apply time; the persisted value stays "auto"
    // so a later OS-language change is honored on next start.
    trayCallbacks.on_select_ui_language = [&](std::string_view code) {
        const auto snap = config.GetSnapshot(); // I4: consistent read
        const emebalachat::UiLocaleChangePlan plan =
            emebalachat::PlanUiLocaleChange(snap.ui_language, code);
        if (!plan.valid) {
            fprintf(stderr, "MAIN/UiLocale/001: refused unknown UI-language code '%s'\n",
                    std::string(code).c_str());
            return;
        }
        if (plan.changed) {
            config.SetUiLanguage(plan.persisted_value); // I4: locked write
            config.SaveToFile();
        }
        const emebalachat::UiLocale applied =
            (plan.applied == emebalachat::UiLocale::Auto)
                ? emebalachat::I18n::DetectSystemLocale()
                : plan.applied;
        emebalachat::I18n::SetLocale(applied);
        RefreshAllUiForLocaleChange();
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
    // R6 Phase 6: mirror the persisted ui_language into the tray's selector
    // check state (snapshot read; the warmup thread already exists here).
    tray.SetUiLanguage(config.GetSnapshot().ui_language);
    // R6 Phase 1 (B3, plan §2.4 "config reload at startup"): initial surface
    // alignment runs through the SAME coordinator as every runtime mutation
    // (empty request = refresh-only: valid, unchanged, no persist). This is
    // why the badge is re-pushed here even though Create() already received
    // the loaded values: one code path, so a startup-only special case can
    // never drift from the runtime one again.
    ApplyLanguageChange(std::string_view{}, std::string_view{}, false, false);

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
        // REQ-005 (plan §2.2): click-outside dismissal for the About popup.
        // AboutWindow::Dismiss marshals internally when called off-thread.
        if (about_window.IsVisible()) {
            RECT r = {};
            ::GetWindowRect(about_window.GetHwnd(), &r);
            if (!::PtInRect(&r, pt)) {
                about_window.Dismiss();
            }
        }
    });

    // REQ-002 (plan §2.1): forward wheel input to the tooltip. The LL hook
    // callback runs on the hook thread; Windows routes WM_MOUSEWHEEL to the
    // FOCUSED window only, and the tooltip is WS_EX_NOACTIVATE (never focused),
    // so the hook is the sole delivery path. Gate: tooltip visible + cursor
    // inside its rect (physical px, same units as the hook's ms->pt). Post a
    // kScrollMessage (REQ-R10 marshaling) and return: FORWARD-ONLY - the hook
    // proc always passes the event to CallNextHookEx afterwards (see
    // mouse_hook.cpp), so underlying apps keep their own wheel behavior and
    // the LowLevelHooksTimeout budget sees only an IsWindowVisible +
    // GetWindowRect + PostMessage-cheap callback.
    mouse_hook.SetMouseWheelCallback([&](int x, int y, int delta) {
        // Only atomics + thread-safe Win32 here (I4 discipline): is_message_mode_
        // is a plain bool owned by the GUI thread, so the message-mode gate lives
        // in ScrollByDipWheel (runs on the GUI thread after the post), not here.
        if (!tooltip.IsVisible()) {
            return;
        }
        RECT r = {};
        if (!::GetWindowRect(tooltip.GetHwnd(), &r)) {
            return;
        }
        if (!::PtInRect(&r, POINT{ x, y })) {
            return;
        }
        ::PostMessageW(tooltip.GetHwnd(), emebalachat::TooltipWindow::kScrollMessage, 0,
                       static_cast<LPARAM>(delta));
    });

    // Click on DragIconWindow triggers translation of active selection.
    // Invoked from DragIconWindow::WndProc => runs ON THE MAIN GUI THREAD.
    //
    // REQ-R1 (session 260905_0001) root-cause fix. Two coupled defects made the
    // tooltip "never appear" for the user:
    //   (a) The blocking clipboard-sequence wait (CopySelectionWithSequenceWait
    //       polls with Sleep up to ~200 ms) and the synchronous engine.Translate
    //       (seconds for the local LLM, a network RTT for Google) BOTH ran here
    //       on the GUI thread, freezing the message pump mid-click.
    //   (b) Every failure branch (copy gate false, empty text) returned SILENTLY
    //       - no tooltip, no message - so a slow/again-empty target produced
    //       exactly the reported "I click the floating button and nothing
    //       happens" with zero feedback.
    // Fix (minimal, root-cause): hand the entire copy+translate to a dedicated
    // worker thread and ALWAYS land a tooltip - the translation on success, a
    // clear notice on failure - via the existing REQ-R10 thread-safe marshal
    // seams (ShowTranslationThreadSafe / ShowMessageThreadSafe), never touching
    // the single-threaded D2D target off the GUI thread.
    // ---- R6 Phase 3 (audit item 7): bounded, JOINABLE drag-translate worker ----
    // Replaces the per-click std::thread(...).detach(). The detached variant
    // had NO happens-before with shutdown: config/engine/badge/tooltip are
    // wWinMain stack objects, and a detached thread still inside
    // engine.Translate (seconds on the local LLM) could outlive their scope ->
    // the use-after-free class plan §3.5 A9 asked about. The shutdown
    // WaitInferenceIdle(2000) only WARNS on timeout; it cannot join a detached
    // thread. Pattern: the proven REQ-R06 double-Ctrl+C worker (src/hook.cpp
    // L50-109) - one persistent std::jthread, single pending slot,
    // deterministic join in the shutdown sequence. Latest-wins coalescing
    // matches the B1-H1 generation guard, and a superseded job that never
    // starts is now also never COMPUTED (Phase 2 Issue #1's wasted-work item).
    struct DragTranslateJob {
        int x = 0;
        int y = 0;
        uint64_t gen = emebalachat::TooltipWindow::kGenNone;
    };
    std::mutex drag_job_mutex;
    std::condition_variable drag_job_cv;
    bool drag_job_pending = false;
    DragTranslateJob drag_job;

    // The copy + translate body, verbatim from the old detached thread. Runs
    // on drag_translate_worker below; the config/engine/badge/tooltip
    // references are now provably safe - the worker is joined BEFORE those
    // objects can leave scope, and before the surfaces' Destroy() runs.
    auto run_drag_translate = [&](int click_x, int click_y, uint64_t gen) {
        emebalachat::ClipboardBackup backup;
        emebalachat::BackupClipboard(backup);

        // D2-flagged (report issue 1): the old CopySelection(); Sleep(35);
        // pattern had the same stale-read race as audit §2.3 for slow
        // (Electron IPC) targets. The D2 public seam confirms the copy via
        // GetClipboardSequenceNumber() polling and returns false instead of
        // exposing stale clipboard text. On the worker thread, its
        // Sleep-polling never freezes the GUI pump.
        if (!emebalachat::CopySelectionWithSequenceWait()) {
            emebalachat::RestoreClipboard(backup);
            fprintf(stderr, "MAIN/DragIconClick/001: clipboard copy not confirmed; selection lost or target too slow\n");
            // REQ-R1(b): failure is now user-visible, not silent.
            // R6 B1-H1: carries this request's generation - a superseded
            // drag must not stamp a notice over a newer result either.
            tooltip.ShowMessageThreadSafe(click_x, click_y,
                                          emebalachat::I18n::Get(emebalachat::StringId::TooltipTitle),
                                          emebalachat::I18n::Get(emebalachat::StringId::TooltipCopyFailed),
                                          gen);
            return;
        }
        std::wstring selected = emebalachat::GetClipboardText();
        emebalachat::RestoreClipboard(backup);

        if (selected.empty() || selected.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
            fprintf(stderr, "MAIN/DragIconClick/002: clipboard copy confirmed but text empty\n");
            tooltip.ShowMessageThreadSafe(click_x, click_y,
                                          emebalachat::I18n::Get(emebalachat::StringId::TooltipTitle),
                                          emebalachat::I18n::Get(emebalachat::StringId::TooltipNoSelection),
                                          gen);
            return;
        }

        std::string detected = emebalachat::DetectLanguage(selected);
        std::string src_code = emebalachat::NormalizeLanguageCode(detected);
        std::string tgt_lang = config.GetSnapshot().target_language; // I4: snapshot read (worker thread)
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
            // R6 Phase 1 (B3): the src==tgt substitution was previously
            // EPHEMERAL - only this tooltip used it while config/badge/tray
            // kept the (now meaningless) old target. Post it to the GUI-
            // thread coordinator (fire-and-forget PostMessage; this worker
            // thread must never touch the surfaces directly - plan §2.3),
            // so every surface follows the language actually translated to.
            emebalachat::RequestLanguageSync(emebalachat::g_hControllerWnd,
                                             std::string{}, tgt_lang, false, false);
        }

        badge.SetStatus(emebalachat::BadgeStatus::Translating);
        std::wstring translated = engine.Translate(selected, detected, tgt_lang);
        badge.SetStatus(emebalachat::BadgeStatus::Active);

        // Worker thread, so marshal the D2D render to the GUI thread via the
        // REQ-R10 thread-safe seam (audit §3.4) instead of a direct call.
        // R6 B1-H1: if a newer request was stamped while this thread was
        // inside engine.Translate (local LLM takes seconds), the tooltip
        // drops this stale payload instead of showing the previous
        // translation (the reported intermittent bug).
        tooltip.ShowTranslationThreadSafe(click_x, click_y, selected, src_code, tgt_lang, translated,
                                          gen);
    };

    std::jthread drag_translate_worker(
        [&drag_job_mutex, &drag_job_cv, &drag_job_pending, &drag_job,
         &run_drag_translate](std::stop_token st) {
            for (;;) {
                DragTranslateJob job;
                {
                    std::unique_lock<std::mutex> lk(drag_job_mutex);
                    // The GUI-thread producer sets pending UNDER this mutex,
                    // so the textbook cv protocol holds with no lost wakeup
                    // (no time backstop needed - unlike the hook-thread
                    // producer in hook.cpp, which must stay lock-free).
                    drag_job_cv.wait(lk, [&st, &drag_job_pending]() {
                        return st.stop_requested() || drag_job_pending;
                    });
                    if (st.stop_requested()) {
                        break; // a pending job is superseded anyway (guard drops it)
                    }
                    job = drag_job;
                    drag_job_pending = false;
                }
                // Mutex NOT held across clipboard work / engine.Translate
                // (same rule as KeyboardHook::DoubleCtrlCWorkerLoop).
                run_drag_translate(job.x, job.y, job.gen);
            }
        });

    // Click on DragIconWindow: runs ON THE MAIN GUI THREAD (its WndProc).
    // Stamp the generation at trigger time (R6 Phase 2 B1-H1), hand the job
    // to the worker, return immediately - the message pump is never blocked
    // by clipboard settle or inference.
    drag_icon.SetClickCallback([&](int click_x, int click_y) {
        const uint64_t gen = tooltip.BeginTranslationRequest();
        {
            std::lock_guard<std::mutex> lk(drag_job_mutex);
            // Latest-wins overwrite: a job the worker has NOT popped yet is
            // replaced (its render would be dropped by the generation guard
            // anyway). A job already popped runs to completion and is guarded
            // at render time - unchanged Phase 2 semantics.
            drag_job = DragTranslateJob{ click_x, click_y, gen };
            drag_job_pending = true;
        }
        drag_job_cv.notify_one();
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

        // R6 Phase 2 (B1-H1): stamp the generation at trigger time on this
        // (REQ-R06 worker) thread; every show below carries it.
        const uint64_t gen = tooltip.BeginTranslationRequest();

        // R6 Phase 2 (B1-H3, plan §1): the old code did a fixed ::Sleep(40)
        // and read the clipboard unconditionally. On slow (Electron IPC)
        // targets the app's copy handler may not have committed within 40 ms,
        // so the read returned the PREVIOUS clipboard content -> the stale
        // translation the user reported. Now the capture goes through the
        // REQ-R04-proven confirmed-capture seam (GetClipboardSequenceNumber
        // polling, same as the drag path): the re-issued Ctrl+C must make the
        // sequence provably move and settle, otherwise nothing is read (never
        // a stale value).
        if (!emebalachat::CopySelectionWithSequenceWait()) {
            fprintf(stderr, "MAIN/DoubleCtrlC/001: clipboard copy not confirmed; refusing stale read\n");
            return;
        }
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
            // R6 Phase 1 (B3): same coordinator routing as the drag path above
            // (this body runs on the hook's REQ-R06 async worker thread).
            emebalachat::RequestLanguageSync(emebalachat::g_hControllerWnd,
                                             std::string{}, tgt_lang, false, false);
        }

        POINT cursor = {};
        ::GetCursorPos(&cursor);

        badge.SetStatus(emebalachat::BadgeStatus::Translating);
        std::wstring translated = engine.Translate(copied, detected, tgt_lang);
        badge.SetStatus(emebalachat::BadgeStatus::Active);

        // REQ-R10-adjacent: worker thread, so marshal the D2D render to the GUI
        // thread instead of calling ShowTranslation() directly (audit §3.4).
        // R6 B1-H1: generation-guarded (a drag request stamped meanwhile makes
        // this result the stale one, and the tooltip drops it instead).
        tooltip.ShowTranslationThreadSafe(
            cursor.x + 12, cursor.y + 12, copied, src_code, tgt_lang, translated, gen);
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
        if (about_window.IsVisible()) {
            about_window.Dismiss();
            return true;
        }
        return false;
    });

    // Tooltip language switcher: THE reported B3 bug site. Previously this
    // callback re-translated + re-showed the tooltip ONLY - config, badge and
    // tray kept the old target and drifted forever (architect plan §1 B3).
    // R6 Phase 1: the menu pick is now a REQUEST to the single coordinator
    // (INV-2): persist -> badge -> tray -> tooltip label sync, THEN the local
    // re-translation with the NEW target passed explicitly (INV-5), then the
    // content re-show at the current position.
    tooltip.SetLanguageChangeCallback([&](std::string_view new_target_lang) {
        // R6 Phase 2 (B1-H1): the language change is itself a NEW translate
        // request - stamp it so an in-flight older drag result cannot
        // overwrite the re-translation when it lands.
        const uint64_t gen = tooltip.BeginTranslationRequest();

        std::wstring src = tooltip.GetSourceText();
        std::string src_code = tooltip.GetSourceLangCode();
        std::string new_tgt = std::string(new_target_lang);

        RECT r = {};
        ::GetWindowRect(tooltip.GetHwnd(), &r);

        // Runs on the GUI thread (tooltip WndProc) - the coordinator's
        // required thread per plan §2.3. If the request is refused (invalid
        // name), everything stays consistent at the OLD pair; the re-show
        // below just re-renders the current state.
        ApplyLanguageChange(std::string_view{}, new_tgt, false, false);

        badge.SetStatus(emebalachat::BadgeStatus::Translating);
        std::wstring translated = engine.Translate(src, src_code, new_tgt);
        badge.SetStatus(emebalachat::BadgeStatus::Active);

        // Runs on the GUI thread: the fresh generation this callback just
        // stamped makes the guard accept it unconditionally (>= latest).
        tooltip.ShowTranslation(r.left, r.top, src, src_code, new_tgt, translated, gen);
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
    // R6 Phase 3 (audit item 7): join the drag-translate worker here, BEFORE
    // the language-sync queue drain below (no producer can post a
    // kMsgApplyLanguageSync payload after this point) and before any surface
    // Destroy(). Bounded: engine.RequestCancel() makes an in-flight decode
    // unwind at the next token boundary (REQ-R16), and a clipboard settle is
    // <= ~200 ms. Warmup-thread join precedent ~30 lines below.
    drag_translate_worker.request_stop();
    drag_job_cv.notify_all();
    if (drag_translate_worker.joinable()) {
        drag_translate_worker.join();
    }
    // R6 Phase 1 (B3): drain any language-sync requests still queued before
    // the coordinator is retired, so a cycle posted a moment before shutdown
    // still persists instead of being silently dropped. Peek-only sweep: other
    // messages (including the WM_QUIT DestroyWindow will post) are left for the
    // normal teardown path below.
    {
        MSG m = {};
        while (::PeekMessageW(&m, hController, emebalachat::kMsgApplyLanguageSync,
                              emebalachat::kMsgApplyLanguageSync, PM_REMOVE)) {
            const std::unique_ptr<emebalachat::LanguageSyncRequest> p(
                reinterpret_cast<emebalachat::LanguageSyncRequest*>(m.lParam));
            if (p && emebalachat::g_apply_language_change) {
                emebalachat::g_apply_language_change(p->source, p->target,
                                                     m.wParam != 0, p->play_chime);
            }
        }
    }
    // Retire the coordinator BEFORE the surfaces it writes to are destroyed.
    // The double-Ctrl+C worker (joined by hook.Stop() above) and the drag
    // worker (joined immediately before this block, R6 Phase 3) can no longer
    // post sync requests; the hook thread itself is down. Once unset,
    // ControllerWndProc frees any still-queued payload without touching
    // config/engine/badge/tray/tooltip.
    emebalachat::g_apply_language_change = nullptr;
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
    about_window.Destroy();

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
