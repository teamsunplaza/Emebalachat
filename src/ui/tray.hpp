#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <windows.h>
#include <shellapi.h>

namespace emebalachat {

class SystemTray {
public:
    struct Callbacks {
        std::function<void()> on_toggle_active;
        std::function<void(int engine_idx)> on_select_engine; // 0 = Google Translate, 1 = Local LLM
        std::function<void(std::string_view code)> on_select_source_lang;
        std::function<void(std::string_view code)> on_select_target_lang;
        std::function<void()> on_swap_languages;
        std::function<void()> on_toggle_auto_send;
        std::function<void()> on_toggle_sound;
        std::function<void()> on_toggle_badge;
        std::function<void()> on_toggle_start_with_windows;
        std::function<void()> on_show_cheat_sheet;
        std::function<void()> on_show_about; // Opens the About window (wired in main.cpp)
        std::function<void()> on_exit;
        // R6 Phase 6 (plan §5.4): UI-language selector submenu. Argument is the
        // canonical locale code ("auto", "ko", "ja", "zh-CN", "zh-TW", "vi",
        // "es", "en"); main.cpp validates/persists via PlanUiLocaleChange.
        std::function<void(std::string_view code)> on_select_ui_language;
    };

    SystemTray();
    ~SystemTray();

    // Initializes system tray notification icon
    bool Create(HWND hOwner, HINSTANCE hInstance, const Callbacks& callbacks);

    // Removes icon from system notification area
    void Destroy();

    // Updates tray icon tooltip, status color, and checked state
    void UpdateStatus(
        bool active,
        std::string_view active_engine,
        std::string_view src_code,
        std::string_view tgt_code,
        bool auto_send,
        bool sound_enabled,
        bool badge_visible
    );

    // R6 Phase 6: mirrors the persisted config.ui_language value ("auto" or a
    // locale code) for the UI-language submenu check mark. Called by main.cpp
    // at startup and from RefreshAllUiForLocaleChange (menu itself rebuilds
    // lazily on every ShowContextMenu, so no explicit rebuild nudge is needed).
    void SetUiLanguage(std::string_view persisted_code);

    // Displays popup context menu at current cursor coordinates
    void ShowContextMenu();

    static constexpr UINT WM_TRAYICON = WM_USER + 100;

private:
    HICON CreateStatusIcon(bool active);

    HWND hOwner_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    NOTIFYICONDATAW nid_ = {};
    HICON hCurrentIcon_ = nullptr;
    Callbacks callbacks_;

    bool is_active_ = true;
    std::string active_engine_ = "Google Translate";
    std::string src_code_ = "AUTO";
    std::string tgt_code_ = "EN";
    bool auto_send_ = false;
    bool sound_enabled_ = true;
    bool badge_visible_ = true;
    std::string ui_language_ = "auto"; // R6 Phase 6: submenu check state
};

} // namespace emebalachat
