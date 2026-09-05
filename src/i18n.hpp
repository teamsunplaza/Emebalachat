#pragma once

#include <string>
#include <string_view>
#include <windows.h>

namespace emebalachat {

enum class UiLocale {
    Auto,
    Korean,
    English,
    Japanese,
    ChineseSimplified,
    ChineseTraditional,
    Vietnamese,
    Spanish,
    French,
    German,
    Russian
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

    // Common
    AutoDetect
};

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
