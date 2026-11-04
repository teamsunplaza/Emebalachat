#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <windows.h>
#include <shellapi.h>

#include "ui/theme.hpp"

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

// ---- W1 (R1-22, session 261029_0001): hover-tip / status-row composition ----
//
// NOTIFYICONDATAW::szTip is a FIXED wchar_t buffer INCLUDING the terminator.
// Shell_NotifyIcon silently drops an over-long tip and wcsncpy_s(_TRUNCATE)
// cuts mid-word, so the composed tip is capped here - derived from the struct
// itself so the number can never drift from the SDK.
inline constexpr size_t kTrayTipMaxChars =
    sizeof(NOTIFYICONDATAW{}.szTip) / sizeof(wchar_t) - 1;

// The localized engine rows (MenuEngineGoogle / MenuEngineLocal / ...) are
// full sentences of 26-44 characters and would eat the whole tip budget
// (research §8 residual risk 1: "prefer engine short code over full localized
// engine sentence"). This maps BOTH push shapes onto one short product/model
// token: the GUI path passes TranslationManager::GetActiveEngineName()'s
// display stem ("Hy-MT2-1.8B (Shared Host)"), the hook path passes the raw
// persisted engine_type ("local"). Empty input stays empty (no engine segment).
std::string TrayEngineLabel(std::string_view active_engine);

// The pieces of the hover tip, already localized by the caller. Keeping the
// composition pure (no I18n call inside) is what makes the szTip budget and
// the drop order unit-testable headlessly.
struct TrayTipParts {
    std::wstring_view title;
    std::wstring_view state;          // BadgeActive / BadgePaused
    std::wstring_view engine;         // TrayEngineLabel result (may be empty)
    std::wstring_view src_display;    // I18n::GetLanguageDisplayName(...)
    std::wstring_view tgt_display;
    std::wstring_view auto_send_label; // MenuAutoSend
    bool auto_send = false;
};

// "Title (State) - Engine · SRC → TGT[ · AutoSend]", never longer than
// kTrayTipMaxChars. Overflow drops the auto-send tail FIRST (it is the only
// optional segment), then ellipsizes - a visible "…" instead of the silent
// mid-word cut wcsncpy_s would produce.
std::wstring ComposeTrayTip(const TrayTipParts& parts);

// "State · SRC → TGT": appended to the tray's top status row (the localized
// MenuStatus* sentence already carries the "상태:" prefix, so the row reads
// "상태: 활성 (F9: 일시 정지) · 자동 감지 → 영어" - the research's W1 shape).
std::wstring ComposeTrayStatusLabel(std::wstring_view state_label,
                                    std::wstring_view src_display,
                                    std::wstring_view tgt_display);

// ---- W2 (R1-22): pure swap planner for BOTH language pairs ----
//
// The tray Swap row / badge double-click (TYPE pair) and the new 번역툴팁 Swap
// row (DRAG pair) share ONE policy definition, so a product change to the
// AUTO-pivot rule can never land on only one of the two surfaces:
//   * source is AUTO Detect -> the concrete old target becomes the source and
//     the new target falls back to the OS-native language (English when the
//     OS language IS the old target, so the swap always reverses something);
//   * otherwise the two concrete languages swap directly.
// Result strings are name_en (the coordinator's stored form). Pure: the caller
// passes the pair and the OS language code, so both arms and every OS-language
// edge (including the unsupported -> "KO" fallback) are unit-testable.
struct LanguageSwapPlan {
    std::string source;
    std::string target;
};
LanguageSwapPlan PlanLanguageSwap(std::string_view cur_source,
                                  std::string_view cur_target,
                                  std::string_view system_language_code);

class SystemTray {
public:
    using ThemeSelectCallback = std::function<void(const std::string&)>;

    static constexpr UINT kCmdThemeFluent    = 2381;
    static constexpr UINT kCmdThemeEditorial = 2382;
    static constexpr UINT kCmdThemeMonolith  = 2383;

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
        // W2 (R1-22, session 261029_0001): the 번역툴팁 group's own Swap row.
        // Same policy as on_swap_languages (PlanLanguageSwap) but applied to the
        // DRAG pair through ApplyLanguageChange(LanguageContext::Drag, ...).
        // Appended (not reordered) so every existing wiring keeps its meaning.
        std::function<void()> on_swap_drag_languages;
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
        // REQ-060 (session 261009_0001, engine-idle-eviction-warmup): the
        // engine submenu's "번역 모델 로드 설정…" entry row (below the model
        // manager row). Entry row, NOT a check target — it opens the
        // translation-model load-mode dialog. main.cpp defers the modal open
        // to the controller window past the TrackPopupMenuEx teardown (the
        // REQ-047 D3 contract shared with the manager/OpenAI rows).
        std::function<void()> on_open_engine_load_settings;
        ThemeSelectCallback on_select_theme;
    };

    SystemTray();
    ~SystemTray();

    // Initializes system tray notification icon
    bool Create(HWND hOwner, HINSTANCE hInstance, const Callbacks& callbacks);

    // Removes icon from system notification area
    void Destroy();

    // Updates tray icon tooltip, status color, and checked state.
    // REQ-025: src/tgt are the TYPE pair (tip text + type submenu checks);
    // drag_src/drag_tgt drive ONLY the drag submenu check marks - the tip
    // always keeps displaying the type pair (Phase A §2.1.A3-25 design).
    // active_engine is display-only: W1 (R1-22) puts its SHORT token in the
    // tip (TrayEngineLabel) and still logs it through tray_update; the Engine
    // submenu check mark never reads it (REQ-056).
    //
    // R2-5 (FIX-B2): an EMPTY active_engine means "no resolved display stem"
    // and PRESERVES the stored one. The GUI thread's refresh_tray() is the
    // single authority (engine.GetActiveEngineName()); the hook-thread
    // refreshes pass empty so an F9 / Ctrl+Shift+Enter / Ctrl+F9 can no
    // longer clobber "Hy-MT2-1.8B" with the raw engine_type token.
    //
    // W1 (R1-22): the tip is now composed by ComposeTrayTip - state, engine,
    // type pair and (when on) the auto-send tail, capped at kTrayTipMaxChars.
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

    // W1 (R1-22) test seam: the tip text UpdateStatus last composed and copied
    // into szTip. run_tests exercises UpdateStatus on a non-Created tray (the
    // Shell_NotifyIcon call is a harmless no-op), so this lets the suite prove
    // the REAL surface honours the szTip budget, not just the pure helper.
    std::wstring_view TipTextForTest() const noexcept { return nid_.szTip; }

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
    void SetOnThemeSelectCallback(ThemeSelectCallback cb);
    void SetUiTheme(std::string_view theme_key);
    void RefreshTheme();

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
    ThemeSelectCallback on_theme_select_;

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
    std::string ui_theme_ = "fluent";
};

} // namespace emebalachat
