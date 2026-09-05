#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace emebalachat {

// R6 Phase 6 (architect plan §5.5, user decision "remove"): French, German and
// Russian were declared here but had NO populated translation table (they
// silently fell back to English - the half-wired state plan §5.1 flagged).
// They are REMOVED from the enum; fr/de/ru config values now resolve to
// English explicitly (I18n::StringToLocale default branch). FR/DE/RU remain
// fully supported as TRANSLATION languages (src/config.cpp language registry)
// - only the UI-localization locale set changed.
enum class UiLocale {
    Auto,
    Korean,
    English,
    Japanese,
    ChineseSimplified,
    ChineseTraditional,
    Vietnamese,
    Spanish
};

enum class StringId {
    // Menu & Status
    MenuStatusActive,
    MenuStatusPaused,
    MenuEngine,
    MenuEngineGoogle,
    MenuEngineLocal,
    MenuSourceLang,
    MenuTargetLang,
    MenuSwapLangs,
    MenuAutoSend,
    MenuSoundFeedback,
    MenuShowBadge,
    MenuStartWithWindows,
    MenuCheatSheet,
    MenuExit,
    MenuAbout,

    // Cheat Sheet Dialog
    CheatSheetTitle,
    CheatSheetBody,

    // About Window
    AboutTitle,

    // Badge Status Words
    BadgeActive,
    BadgeTranslating,
    BadgePaused,

    // Tray Tooltip
    TooltipTitle,

    // Drag-icon click feedback (REQ-R1, session 260905_0001): transient
    // notice bodies shown when the click cannot produce a translation.
    TooltipCopyFailed,  // clipboard copy could not be confirmed (slow/lost selection)
    TooltipNoSelection, // copy confirmed but the captured text was empty/whitespace

    // ---- R6 Phase 5 (plan §5.2/§5.3): About body + migrated literals ----
    // Startup diagnostics (single-instance notice, COM-failure warning).
    AppAlreadyRunning,
    AppComFailed,

    // About window body copy (marketing text -> translated in all locales;
    // the contact LINES carry universal factual data - phone/address/name -
    // with localized labels, per the plan's §5.2 decision).
    AboutTagline,
    AboutFeature0,
    AboutFeature1,
    AboutFeature2,
    AboutEtymology,
    AboutLinkWebsite,
    AboutLinkContact,
    AboutLinkReddit,   // replaces AboutLinkDownload (Reddit r/emebala, user decision)
    AboutContactOrg,
    AboutContactPhone,
    AboutContactLead,

    // Tooltip footer buttons + copy feedback (plan §5.3 sweep).
    TooltipCopied,
    TooltipButtonCopy,
    TooltipButtonTts,

    // R6 Phase 6 (plan §5.4): UI-language selector submenu.
    MenuUiLanguage,     // submenu title ("Interface Language")
    MenuUiLanguageAuto, // "Auto (system language)" entry of the selector

    // Common
    AutoDetect,

    // R6 Phase 5: sentinel for the table-completeness unit test
    // (every StringId below it must return a non-empty value in all 7 locales).
    EnumCount
};

// ---- R6 Phase 6 (plan §5.4): UI-language selector data + pure change plan ----

// One selectable UI locale: enum value + ENDONYM (the language's own name,
// the standard for language pickers: "한국어", "日本語", "简体中文" ... - these
// are deliberately NOT translated per entry).
struct UiLocaleEntry {
    UiLocale locale;
    const wchar_t* native_name;
};

// The 7 populated locales, in selector display order (KO, JA, zh-CN, zh-TW,
// VI, ES, EN). FR/DE/RU are gone with the enum values (removed, plan §5.5).
const std::vector<UiLocaleEntry>& GetSupportedUiLocales();

// Surfaces RefreshAllUiForLocaleChange (src/main.cpp) must re-render after a
// locale switch, in the plan §5.4 propagation order.
enum class LocaleSurface : unsigned char {
    Tray,    // menu rebuild (rebuild is lazy per ShowContextMenu) + tip update
    Badge,   // SetLanguages nudge -> localized status/language label
    Tooltip, // RequestLocaleRefresh -> re-render while visible
    About,   // RequestLocaleRefresh -> caption + body re-render while visible
};

struct UiLocaleChangePlan {
    std::string persisted_value;          // value to store in config.ui_language
    UiLocale applied = UiLocale::Auto;    // Auto sentinel = resolve via DetectSystemLocale at apply time
    bool valid = false;                   // false = unknown code: apply NOTHING
    bool changed = false;                 // false = same persisted value (views still refresh: self-heal)
    std::vector<LocaleSurface> surfaces;  // empty unless valid
};

// Pure planner (headless-testable per plan §7.2): "auto" selects the system
// locale at apply time; any populated-locale code is stored verbatim in
// lowercase-insensitive canonical form. Unknown values (e.g. the removed
// "fr"/"de"/"ru") are REFUSED (valid=false) so config can never carry a
// half-wired locale again.
UiLocaleChangePlan PlanUiLocaleChange(std::string_view current_persisted,
                                      std::string_view requested);

class I18n {
public:
    // Initializes localization state based on config override or Windows OS UI language
    static void Initialize(std::string_view config_ui_lang = "auto");

    // Sets active UI locale
    static void SetLocale(UiLocale locale);

    // Returns currently active UI locale
    static UiLocale GetCurrentLocale();

    // Returns locale identifier (e.g. "ko", "ja", "zh-CN", "zh-TW", "vi", "es", "fr", "de", "ru", "en")
    static std::string_view GetLocaleCode();
    static std::string GetSystemLanguageCode() { return std::string(GetLocaleCode()); }

    // Returns localized UI string for given StringId
    static std::wstring Get(StringId id);

    // Returns localized language display name for a given language code (e.g. "KO" -> "한국어 (Korean)")
    static std::wstring GetLanguageDisplayName(std::string_view lang_code);

    // Detects Windows system UI language via GetUserDefaultLocaleName() / GetUserDefaultUILanguage()
    static UiLocale DetectSystemLocale();

    // Returns recommended default target language for a given OS locale
    static std::string GetDefaultTargetLanguage(UiLocale locale);

    // Conversion helpers
    static UiLocale StringToLocale(std::string_view str);
    static std::string LocaleToString(UiLocale locale);

    // Windows Startup registry helpers
    static bool IsStartWithWindowsEnabled();
    static void SetStartWithWindows(bool enable);
};

} // namespace emebalachat
