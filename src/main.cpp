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
#include "ui/tooltip.hpp"
#include "ui/tray.hpp"
#include "worker.hpp"

#include <windows.h>
#include <objbase.h>

namespace emebalachat {

namespace {
const wchar_t kControllerClassName[] = L"Emebalachat_ControllerWindowClass";
HWND g_hControllerWnd = nullptr;
SystemTray* g_pTray = nullptr;
KeyboardHook* g_pHook = nullptr;
FloatingBadge* g_pBadge = nullptr;

LRESULT CALLBACK ControllerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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

    // 1. Single Instance Mutex
    HANDLE hMutex = ::CreateMutexW(nullptr, TRUE, L"Global\\Emebalachat_SingleInstance");
    if (!hMutex || ::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::MessageBoxW(
            nullptr,
            L"Emebalachat is already running in the background.\nCheck the system notification tray.",
            L"Emebalachat",
            MB_OK | MB_ICONINFORMATION
        );
        if (hMutex) {
            ::CloseHandle(hMutex);
        }
        return 0;
    }

    // 2. Initialize COM for Direct2D & DirectWrite
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    (void)hr;

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
    emebalachat::TranslationManager engine(engine_type, config.model_path);
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
        L"Emebalachat Controller",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInstance, nullptr
    );
    emebalachat::g_hControllerWnd = hController;

    // 7. Initialize Direct2D Floating Badge (restoring saved position if present)
    emebalachat::FloatingBadge badge;
    emebalachat::g_pBadge = &badge;
    badge.Create(
        hInstance,
        emebalachat::ToUtf16(config.source_language),
        emebalachat::ToUtf16(config.target_language),
        config.badge_x,
        config.badge_y
    );

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

    emebalachat::SystemTray::Callbacks trayCallbacks;
    trayCallbacks.on_toggle_active = [&]() {
        hook.ToggleActive();
        mouse_hook.SetEnabled(hook.IsActive());
    };

    trayCallbacks.on_select_engine = [&](int engine_idx) {
        if (engine_idx == 0) {
            engine.SetEngineType(emebalachat::EngineType::GoogleTranslate);
            config.engine_type = "google";
        } else {
            engine.SetEngineType(emebalachat::EngineType::LocalLlama);
            config.engine_type = "local";
        }
        config.SaveToFile();
        tray.UpdateStatus(
            hook.IsActive(),
            engine.GetActiveEngineName(),
            config.source_language,
            config.target_language,
            config.auto_send,
            config.sound_enabled,
            badge.IsVisible()
        );
    };

    trayCallbacks.on_select_source_lang = [&](std::string_view code) {
        config.source_language = std::string(code);
        config.SaveToFile();
        badge.SetLanguages(emebalachat::ToUtf16(config.source_language), emebalachat::ToUtf16(config.target_language));
        tray.UpdateStatus(
            hook.IsActive(),
            engine.GetActiveEngineName(),
            config.source_language,
            config.target_language,
            config.auto_send,
            config.sound_enabled,
            badge.IsVisible()
        );
    };

    trayCallbacks.on_select_target_lang = [&](std::string_view name) {
        config.target_language = std::string(name);
        config.SaveToFile();
        badge.SetLanguages(emebalachat::ToUtf16(config.source_language), emebalachat::ToUtf16(config.target_language));
        tray.UpdateStatus(
            hook.IsActive(),
            engine.GetActiveEngineName(),
            config.source_language,
            config.target_language,
            config.auto_send,
            config.sound_enabled,
            badge.IsVisible()
        );
    };

    trayCallbacks.on_swap_languages = [&]() {
        std::string current_src_norm = emebalachat::NormalizeLanguageCode(config.source_language);
        std::string current_tgt_norm = emebalachat::NormalizeLanguageCode(config.target_language);

        std::string new_src;
        std::string new_tgt;

        if (current_src_norm == "AUTO") {
            // When source is Auto Detect (e.g. Auto Detect -> English)
            // new source becomes current target (English)
            const auto* pTgtInfo = emebalachat::FindLanguageByCode(current_tgt_norm);
            new_src = pTgtInfo ? pTgtInfo->name_en : config.target_language;

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
            new_src = config.target_language;
            new_tgt = config.source_language;
        }

        config.source_language = new_src;
        config.target_language = new_tgt;
        config.SaveToFile();

        badge.SetLanguages(emebalachat::ToUtf16(config.source_language), emebalachat::ToUtf16(config.target_language));
        tray.UpdateStatus(
            hook.IsActive(),
            engine.GetActiveEngineName(),
            config.source_language,
            config.target_language,
            config.auto_send,
            config.sound_enabled,
            badge.IsVisible()
        );
        emebalachat::PlayLangChange();
    };

    trayCallbacks.on_toggle_auto_send = [&]() {
        hook.ToggleAutoSend();
    };

    trayCallbacks.on_toggle_sound = [&]() {
        config.sound_enabled = !config.sound_enabled;
        config.SaveToFile();
        emebalachat::SetSoundEnabled(config.sound_enabled);
        tray.UpdateStatus(
            hook.IsActive(),
            engine.GetActiveEngineName(),
            config.source_language,
            config.target_language,
            config.auto_send,
            config.sound_enabled,
            badge.IsVisible()
        );
    };

    trayCallbacks.on_toggle_badge = [&]() {
        badge.SetVisible(!badge.IsVisible());
        tray.UpdateStatus(
            hook.IsActive(),
            engine.GetActiveEngineName(),
            config.source_language,
            config.target_language,
            config.auto_send,
            config.sound_enabled,
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
    tray.UpdateStatus(
        true,
        engine.GetActiveEngineName(),
        config.source_language,
        config.target_language,
        config.auto_send,
        config.sound_enabled,
        badge.IsVisible()
    );

    // Drag release threshold callback (> 15px)
    mouse_hook.SetDragReleaseCallback([&](int x, int y) {
        if (!config.drag_to_translate || !hook.IsActive() || tooltip.IsVisible()) {
            return;
        }
        drag_icon.ShowAt(x + 12, y + 12);
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

    // Click on DragIconWindow triggers translation of active selection
    drag_icon.SetClickCallback([&](int click_x, int click_y) {
        emebalachat::ClipboardBackup backup;
        emebalachat::BackupClipboard(backup);

        emebalachat::CopySelection();
        ::Sleep(35);
        std::wstring selected = emebalachat::GetClipboardText();
        emebalachat::RestoreClipboard(backup);

        if (selected.empty() || selected.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
            return;
        }

        std::string detected = emebalachat::DetectLanguage(selected);
        std::string src_code = emebalachat::NormalizeLanguageCode(detected);
        std::string tgt_lang = config.target_language;
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

        tooltip.ShowTranslation(click_x, click_y, selected, src_code, tgt_lang, translated);
    });

    // Double Ctrl+C Hotkey Detection (< 400ms)
    hook.SetDoubleCtrlCCallback([&]() {
        if (!config.drag_to_translate || !hook.IsActive()) {
            return;
        }

        ::Sleep(40);
        std::wstring copied = emebalachat::GetClipboardText();
        if (copied.empty() || copied.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
            return;
        }

        std::string detected = emebalachat::DetectLanguage(copied);
        std::string src_code = emebalachat::NormalizeLanguageCode(detected);
        std::string tgt_lang = config.target_language;
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

        tooltip.ShowTranslation(cursor.x + 12, cursor.y + 12, copied, src_code, tgt_lang, translated);
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

    // Badge Mouse Controls:
    // Single Click: Toggle Active / Paused
    badge.SetClickCallback([&]() {
        hook.ToggleActive();
        mouse_hook.SetEnabled(hook.IsActive());
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

    // 10. Startup sound
    emebalachat::PlayToggleOn();

    // 11. Main Message Loop
    MSG msg = {};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    // 12. Clean Shutdown
    mouse_hook.Stop();
    hook.Stop();
    worker.Stop();
    tray.Destroy();
    badge.Destroy();
    tooltip.Destroy();
    drag_icon.Destroy();

    if (hController) {
        ::DestroyWindow(hController);
    }

    config.SaveToFile();
    ::CoUninitialize();

    if (hMutex) {
        ::ReleaseMutex(hMutex);
        ::CloseHandle(hMutex);
    }

    return 0;
}
