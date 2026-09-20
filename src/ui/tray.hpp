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
        // REQ-045 P4-3 + REQ-046 P4-2 (Rev2 §B-3): engine_idx 0 = Google
        // Translate, 1 = Local LLM, 2 = OpenAI Compatible, 3 = 사용자 선택
        // (.gguf). Appended (not renumbered) so existing main.cpp call sites
        // keep their meaning.
        std::function<void(int engine_idx)> on_select_engine;
        std::function<void(std::string_view code)> on_select_source_lang;
        std::function<void(std::string_view code)> on_select_target_lang;
        // REQ-025 (Phase A §2.1.A3-25): "번역툴팁"(drag) language pair selectors,
        // separate from the "키보드타이핑"(type) pair above. Same string_view
        // contract (name_en passed); main.cpp routes to ApplyLanguageChange
        // (LanguageContext::Drag).
        std::function<void(std::string_view code)> on_select_drag_source_lang;
        std::function<void(std::string_view code)> on_select_drag_target_lang;
        std::function<void()> on_swap_languages;
        std::function<void()> on_toggle_auto_send;
        std::function<void()> on_toggle_sound;
        std::function<void()> on_toggle_badge;
        std::function<void()> on_toggle_start_with_windows;
        std::function<void()> on_show_cheat_sheet;
        std::function<void()> on_show_about; // Opens the About window (wired in main.cpp)
        std::function<void()> on_exit;
        // R6 Phase 6 (plan §5.4), REQ-037/B-4 expansion (design §2-Q3):
        // UI-language selector submenu (Auto + 37 locales). Argument is the
        // canonical persisted code - "auto" or the config_code of any entry in
        // I18n::GetSupportedUiLocales() as emitted by I18n::LocaleToString
        // ("ko", "ja", "zh-CN", "zh-TW", "vi", "es", ... ; the 37 canonical
        // spellings live in i18n.cpp kLocaleMappings). main.cpp
        // validates/persists via PlanUiLocaleChange.
        std::function<void(std::string_view code)> on_select_ui_language;
        // REQ-050: REMOVED — the engine submenu's file-picker row is merged
        // into the single manager row (ID_TRAY_MANAGE_GGUF). The manager's
        // [파일에서 추가…] button consumes main.cpp's registration pipeline
        // through the callback passed to ShowGgufModelManagerDialog instead.
    };

    SystemTray();
    ~SystemTray();

    // Initializes system tray notification icon
    bool Create(HWND hOwner, HINSTANCE hInstance, const Callbacks& callbacks);

    // Removes icon from system notification area
    void Destroy();

    // Updates tray icon tooltip, status color, and checked state.
    // REQ-025: src/tgt are the TYPE pair (tip text + type submenu checks);
    // drag_src/drag_tgt drive ONLY the new drag submenu check marks - the tip
    // always keeps displaying the type pair (Phase A §2.1.A3-25 design).
    //
    // REQ-029-B (design §2.1 change 2): preferred_engine is the single source
    // of truth for the Engine submenu check mark. 0 = Google (also covers
    // "auto", which is Google-family for display), 1 = Local LLM, 2 = OpenAI
    // Compatible (REQ-045 P4-3), 3 = 사용자 선택(.gguf) (REQ-046 P4-2). It
    // carries the USER'S configured preference (config engine_type), so the
    // check can no longer lie when the local model is missing and the engine
    // honestly reports "Local (Model Missing)". active_engine above stays a
    // DISPLAY-ONLY string (tooltip + tray_update log); it is never used for
    // check decisions.
    void UpdateStatus(
        bool active,
        std::string_view active_engine,
        std::string_view src_code,
        std::string_view tgt_code,
        std::string_view drag_src_code,
        std::string_view drag_tgt_code,
        bool auto_send,
        bool sound_enabled,
        bool badge_visible,
        int preferred_engine,
        // REQ-047 U1 (designer 164500 §5.2.1): stem of the registered user
        // .gguf model (files[0] minus ".gguf"), shown after the "사용자 지정
        // 모델 (.gguf)" label whenever a model is registered, regardless of
        // the checked engine (REQ-050 3-2). Empty -> the checkable entry is
        // not appended at all (REQ-050 3-1). Defaults keep the hook.cpp call
        // sites (which don't track the model registry) untouched.
        std::string_view user_model_stem = ""
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
    // REQ-029-B: display-only engine name (tooltip + tray_update log). The
    // Engine submenu check mark NO LONGER reads this string.
    std::string active_engine_ = "Google Translate";
    // REQ-029-B (design §2.1 change 2), REQ-045 P4-3: preferred engine as
    // configured by the user. 0 = Google (also covers "auto", Google-family
    // for display), 1 = Local, 2 = OpenAI Compatible. Default 0 matches the
    // pre-existing active_engine_ default so the menu is coherent before the
    // first refresh.
    int preferred_engine_ = 0;
    // REQ-047 U1 (designer 164500 §5.2.1): cached stem for the dynamic
    // user-model tray label (empty = no model registered).
    std::string user_model_stem_;
    std::string src_code_ = "AUTO";
    std::string tgt_code_ = "EN";
    // REQ-025: drag-pair check-mark state for the "번역툴팁" submenus.
    std::string drag_src_code_ = "AUTO";
    std::string drag_tgt_code_ = "EN";
    bool auto_send_ = false;
    bool sound_enabled_ = true;
    bool badge_visible_ = true;
    std::string ui_language_ = "auto"; // R6 Phase 6: submenu check state
};

} // namespace emebalachat
