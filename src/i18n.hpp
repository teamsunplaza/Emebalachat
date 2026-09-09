#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace emebalachat {

// REQ-037 (P4 Batch B-3, design §2.1.1): the UI locale set expands to the full
// 37 Hy-MT2 language registry (design §2.1.1 ordering rule). Values are
// declared in kAllLanguages registry order (src/config.cpp) - Auto first -
// so the enum, the LocaleMapping table, the selector join and the registry
// share one canonical ordering.
//
// History note: the R6-era removal of FR/DE/RU (half-wired locales with no
// translation tables, plan §5.5) is SUPERSEDED by REQ-037's "all 37" mandate -
// the user explicitly forbade silent-English duplicates ("no checklist
// gaming"). The removal MECHANISM survives as the authenticity gate: a locale
// whose table is not authored is withheld from GetSupportedUiLocales() and
// refused by PlanUiLocaleChange (design §2-Q4.2). All 37 tables are authored
// in B-3, so the gate is fully open; it stays in place for future locales.
enum class UiLocale {
    Auto,
    Korean,
    English,
    Vietnamese,
    ChineseSimplified,
    ChineseTraditional,
    Japanese,
    Spanish,
    French,
    German,
    Russian,
    Thai,
    Arabic,
    Portuguese,
    Italian,
    Indonesian,
    Malay,
    Filipino,
    Khmer,
    Lao,
    Hindi,
    Bengali,
    Turkish,
    Polish,
    Dutch,
    Ukrainian,
    Persian,
    Urdu,
    Hebrew,
    Czech,
    Hungarian,
    Swedish,
    Greek,
    Romanian,
    Danish,
    Finnish,
    Norwegian,
    Burmese
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

    // Phase 4 (REQ-020): About-window "Reset to system defaults" button label
    // and its transient post-click confirmation label (plan §1.4/§2.5).
    AboutResetButton,
    AboutResetDone,

    // Tooltip footer buttons + copy feedback (plan §5.3 sweep).
    TooltipCopied,
    TooltipButtonCopy,
    TooltipButtonTts,

    // R6 Phase 6 (plan §5.4): UI-language selector submenu.
    MenuUiLanguage,     // submenu title ("Interface Language")
    MenuUiLanguageAuto, // "Auto (system language)" entry of the selector

    // REQ-025 (Phase A §2.1.A3-25): tray language-picker group headers -
    // "키보드타이핑"(typing-translation) and "번역툴팁"(drag tooltip) contexts.
    MenuTypingGroup,
    MenuTooltipGroup,

    // Common
    AutoDetect,

    // REQ-B-001 (session 260909_0001 Batch-1): per-locale brand display name
    // ("에메발라 챗" etc.). Appended last so the EnumCount completeness loop
    // covers it automatically (49x37 with TooltipNoTtsVoice below).
    AppName,
    // REQ-C-004 (session 260909_0001 Batch-1): no-voice TTS notice body
    // (Phase C, design §1.2.2 - "No Windows voice installed..." message).
    TooltipNoTtsVoice,

    // R6 Phase 5: sentinel for the table-completeness unit test
    // (every StringId below it must return a non-empty value in all locales
    // exposed by GetSupportedUiLocales() - 37 since REQ-037/B-3).
    EnumCount
};

// ---- REQ-037 (P4 Batch B-3, design §2.1.3): code-mapping table ----------
//
// One row per selectable UI locale (37 rows, NO Auto). StringToLocale,
// LocaleToString, GetLocaleCode and the two phases of DetectSystemLocale are
// all data-driven from this table, so adding a 38th language later is a
// one-row change plus a string table (design §2.1.3). The table is exposed
// (not file-static) so TestReq037LocaleMapping can pin row integrity,
// round-trips and prefix hygiene headlessly (design §2.1.6).
struct LocaleMapping {
    UiLocale locale;
    // Canonical PERSISTED spelling in config.ui_language (design §2.1.3);
    // case-insensitive at match time, canonical case in storage/output.
    const char* config_code;     // "ko", "zh-CN", "fil", "no", ...
    // BCP-47 language-subtag prefix for DetectSystemLocale phase 1
    // (GetUserDefaultLocaleName returns e.g. "ko-KR"). Matched on the
    // hyphen/underscore boundary (design §5.1 R6): subtag == prefix exactly,
    // never a raw starts_with (L"no" must not hit L"nob", L"fi" must not
    // hit L"fil"). The zh rows carry their canonical prefix for the
    // non-empty-prefix assertion, but Chinese detection is resolved by the
    // explicit script-subtag pre-check BEFORE the generic loop (design
    // §2.1.3), so the zh prefixes never participate in boundary matching.
    const wchar_t* bcp47_prefix;   // L"ko" (never empty)
    // Optional second prefix for locales Windows reports under two tags:
    // Norwegian (nb-NO primary, no-* legacy). nullptr when unused.
    const wchar_t* bcp47_prefix_alt;
    // Representative full BCP-47 tag for DWrite CreateTextFormat localeName
    // (design §2-Q5 verdict A; UI-chrome counterpart of LanguageInfo.bcp47 -
    // two owners, two purposes, cross-reference comment in src/config.cpp).
    const wchar_t* bcp47_full;   // L"ko-KR"
    // PRIMARYLANGID for DetectSystemLocale's fallback phase (design §2.1.3).
    WORD langid_primary;
    // Authenticity gate (design §2-Q4.2): false = table exists but is not
    // human-reviewed; the locale is withheld from GetSupportedUiLocales()
    // and therefore refused by PlanUiLocaleChange. REQ-037 B-3 authors all
    // 37 tables, so every row is true; the flag remains as the gate knob.
    bool authored;
};

// The 37 mapping rows, in UiLocale enum order (kAllLanguages registry order).
const std::vector<LocaleMapping>& GetLocaleMappings();

// Pure BCP-47 tag -> UiLocale resolution used by DetectSystemLocale phase 1
// (zh script pre-check + boundary-checked prefix loop). Returns UiLocale::Auto
// as the "no match" sentinel (Auto is never produced by detection itself),
// letting the caller fall through to the LANGID phase. Exposed headless for
// TestReq037LocaleMapping (design §2.1.6: nb-NO -> Norwegian, fil -> Filipino,
// unknown -> sentinel).
UiLocale LocaleFromBcp47Tag(std::wstring_view tag);

// ---- R6 Phase 6 (plan §5.4): UI-language selector data + pure change plan ----

// One selectable UI locale: enum value + ENDONYM (the language's own name,
// the standard for language pickers: "한국어", "日本語", "简体中文" ... - these
// are deliberately NOT translated per entry).
struct UiLocaleEntry {
    UiLocale locale;
    const wchar_t* native_name;
};

// The 37 selector locales (REQ-037/B-3), built once by joining the
// LocaleMapping table against the kAllLanguages registry (endonym source of
// truth, design §2.1.4) through the authenticity gate. Display order per
// design §2-Q3: the legacy front block (KO, JA, zh-CN, zh-TW, VI, ES) keeps
// its muscle memory, the remaining 30 follow registry order, English last.
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
// locale at apply time; any AUTHORED-locale code is stored verbatim in
// lowercase-insensitive canonical form (design §2.1.3 aliases accepted).
// Values that are not selectable codes - unknown spellings, or a locale
// withheld by the authenticity gate - are REFUSED (valid=false) so config can
// never carry a half-wired locale again (the gate's enforcement point).
UiLocaleChangePlan PlanUiLocaleChange(std::string_view current_persisted,
                                      std::string_view requested);

// Pure overload taking the selector vector explicitly (design §2-Q4.2): the
// gate's enforcement point is the selector loop, so tests pin "a locale
// withheld from the vector is refused" with a SYNTHETIC reduced selector,
// independent of whether every real locale is authored this batch. The
// production entry point above simply forwards GetSupportedUiLocales().
UiLocaleChangePlan PlanUiLocaleChangeWithSelector(const std::vector<UiLocaleEntry>& selector,
                                                  std::string_view current_persisted,
                                                  std::string_view requested);

class I18n {
public:
    // Initializes localization state based on config override or Windows OS UI language
    static void Initialize(std::string_view config_ui_lang = "auto");

    // Sets active UI locale
    static void SetLocale(UiLocale locale);

    // Returns currently active UI locale
    static UiLocale GetCurrentLocale();

    // Returns locale identifier (e.g. "ko", "ja", "zh-CN", "zh-TW", "vi", "es",
    // "ar", "my", ... - all 37 canonical codes; design §2.1.3 table-driven).
    static std::string_view GetLocaleCode();
    static std::string GetSystemLanguageCode() { return std::string(GetLocaleCode()); }

    // Returns localized UI string for given StringId
    static std::wstring Get(StringId id);

    // Returns localized language display name for a given language code (e.g. "KO" -> "한국어 (Korean)")
    static std::wstring GetLanguageDisplayName(std::string_view lang_code);

    // Detects Windows system UI language via GetUserDefaultLocaleName() / GetUserDefaultUILanguage()
    static UiLocale DetectSystemLocale();

    // Conversion helpers
    static UiLocale StringToLocale(std::string_view str);
    static std::string LocaleToString(UiLocale locale);

    // Windows Startup registry helpers
    static bool IsStartWithWindowsEnabled();
    static void SetStartWithWindows(bool enable);
};

} // namespace emebalachat
