#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <windows.h>
#include <shellapi.h>

namespace emebalachat {

// REQ-054/REQ-056: the engine submenu's 4-way check-mark index (0 = Google/
// auto, 1 = Local, 2 = OpenAI, 3 = 사용자 선택(.gguf)) derived from the
// persisted config engine_type string. Single definition consumed by
// main.cpp's engine-menu resolver (REQ-056 derive-at-open). Pre-REQ-054 the
// hook paths passed a legacy bool "(type != local)" as this index, mapping
// user_gguf -> Local (the "F9 reverts my engine" report: the menu tick, not
// routing).
inline int TrayPreferredEngineIndex(std::string_view engine_type) {
    return (engine_type == "local") ? 1
         : (engine_type == "openai") ? 2
         : (engine_type == "user_gguf") ? 3
         : 0;
}

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
    // active_engine stays a DISPLAY-ONLY string (tooltip + tray_update log).
    //
    // REQ-056 (derive-at-open): the Engine submenu's two config-derived
    // values — the 4-way preferred-engine check index and the registered
    // user-model stem — are NO LONGER pushed through UpdateStatus and cached.
    // Every tray bug of the 260922 session (F9 stem clobber REQ-054, check-
    // mark formula REQ-054) came from those push paths. ShowContextMenu now
    // asks main.cpp's resolver (SetEngineMenuResolver) at menu build time;
    // UpdateStatus carries only runtime state.
    void UpdateStatus(
        bool active,
        std::string_view active_engine,
        std::string_view src_code,
        std::string_view tgt_code,
        std::string_view drag_src_code,
        std::string_view drag_tgt_code,
        bool auto_send,
        bool sound_enabled,
        bool badge_visible
    );

    // REQ-056: derive-at-open view of the engine submenu's two config-derived
    // pieces. Invoked ON THE GUI THREAD from ShowContextMenu at menu build
    // time — main.cpp is the single authority (registry + config resolution);
    // the tray no longer caches either value, so no push path can go stale.
    struct EngineMenuView {
        int preferred_engine = 0;      // TrayPreferredEngineIndex(engine_type)
        std::string user_model_stem;   // "" = no user model registered
    };
    void SetEngineMenuResolver(std::function<EngineMenuView()> resolver);

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
    // REQ-056: derive-at-open — the engine submenu's config-derived values
    // (check index + user-model stem) are resolved from main.cpp at menu
    // build time; nothing is cached here.
    std::function<EngineMenuView()> resolver_;
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
