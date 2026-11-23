// V10F UX-2 (261122_0002 P3-C3): the settings hub. See settings_hub_window.hpp
// for the contract; this TU holds the pure plan (SECTION A, headless-tested by
// tests/p3_c3_tests.inc) and the only window-creating code (SECTION B, linked
// but never entered by the suite).

#include "settings_hub_window.hpp"

#include "../diag_logger.hpp"
#include "../hook.hpp"          // KeyboardHook::ParseHotkey / HotkeySpecToDisplayString
#include "../unicode_utils.hpp" // ToUtf8 / ToUtf16

#include <cstdlib> // strtod / strtol (C locale is the process default)
#include <cwchar>  // std::swprintf
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <commctrl.h>
#include <shellapi.h>

#pragma comment(lib, "comctl32.lib") // the runtime tab strip (InitCommonControlsEx)

namespace emebalachat {
namespace settings_hub {

// ---- SECTION A: the pure plan -------------------------------------------

namespace {

bool IsAsciiSpace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

std::string_view TrimAscii(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && IsAsciiSpace(text[begin])) ++begin;
    std::size_t end = text.size();
    while (end > begin && IsAsciiSpace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

float ClampFloatValue(float value, float lo, float hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

int ClampIntValue(int value, int lo, int hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

bool EqualsAsciiNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

// Replaces the FIRST occurrence of `token` in `text`, once (the repo-wide
// "{n}"/"{s}" convention - there is no printf layer anywhere in i18n).
std::wstring ReplaceTokenOnce(std::wstring text, std::wstring_view token,
                              std::wstring_view value) {
    if (token.empty()) return text;
    const std::size_t pos = text.find(token);
    if (pos != std::wstring::npos) {
        text.replace(pos, token.size(), value);
    }
    return text;
}

std::string CanonicalHotkey(const KeyboardHook::HotkeySpec& spec) {
    return ToUtf8(KeyboardHook::HotkeySpecToDisplayString(spec));
}

} // namespace

bool ParseHubFloat(std::string_view text, float min_value, float max_value,
                   float& out) {
    const std::string_view trimmed = TrimAscii(text);
    if (trimmed.empty()) return false;
    const std::string copy(trimmed);
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    // Reject partial parses ("1x") AND non-finite values ("nan" / "inf" parse
    // fully but would poison the config): a NaN fails both comparisons below.
    if (end == nullptr || end == copy.c_str() || *end != '\0') return false;
    if (!(value >= -1.0e18 && value <= 1.0e18)) return false;
    out = ClampFloatValue(static_cast<float>(value), min_value, max_value);
    return true;
}

bool ParseHubInt(std::string_view text, int min_value, int max_value, int& out) {
    const std::string_view trimmed = TrimAscii(text);
    if (trimmed.empty()) return false;
    const std::string copy(trimmed);
    char* end = nullptr;
    const long value = std::strtol(copy.c_str(), &end, 10);
    if (end == nullptr || end == copy.c_str() || *end != '\0') return false;
    // strtol saturates at LONG_MIN/LONG_MAX; clamp in the long domain first so
    // the int cast can never wrap.
    const long lo = static_cast<long>(min_value);
    const long hi = static_cast<long>(max_value);
    const long clamped = value < lo ? lo : (value > hi ? hi : value);
    out = static_cast<int>(clamped);
    return true;
}

std::wstring FormatHubFloat(float value) {
    wchar_t buf[64] = {};
    if (std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%.3f",
                      static_cast<double>(value)) < 0) {
        return std::wstring();
    }
    std::wstring text(buf);
    const std::size_t dot = text.find(L'.');
    if (dot != std::wstring::npos) {
        while (!text.empty() && text.back() == L'0') text.pop_back();
        if (!text.empty() && text.back() == L'.') text.pop_back();
    }
    return text;
}

HubHotkeyField MakeHubHotkeyField(std::string_view raw) {
    HubHotkeyField field;
    const std::string_view trimmed = TrimAscii(raw);
    field.raw.assign(trimmed.begin(), trimmed.end());
    // The ONE parser the hook itself uses: no second vocabulary, and the
    // canonical spelling it produces round-trips (hook.hpp:621-626).
    const KeyboardHook::HotkeySpec spec = KeyboardHook::ParseHotkey(field.raw);
    field.valid = spec.valid;
    if (field.valid) {
        field.canonical = ToUtf8(KeyboardHook::HotkeySpecToDisplayString(spec));
    }
    return field;
}

HubDraft MakeHubDraft(const AppConfig& config, const HubRuntimeState& runtime) {
    HubDraft draft;
    // One locked snapshot for every field it carries (the tray/menu idiom).
    const AppConfig::Snapshot snap = config.GetSnapshot();
    draft.auto_send = snap.auto_send;
    // 261123_0001 ctx-pilot: rides the same locked snapshot (not a separate
    // mutex read), so the seed and the persisted value can never skew here.
    draft.local_context = snap.local_context_enabled;
    draft.back_translation = snap.back_translation_enabled;
    draft.sound = snap.sound_enabled;
    draft.drag_to_translate = snap.drag_to_translate;
    draft.ui_language = snap.ui_language;
    draft.ui_theme = AppConfig::NormalizeUiTheme(snap.ui_theme);

    // The two values AppConfig does not carry.
    draft.badge_visible = runtime.badge_visible;
    draft.start_with_windows = I18n::IsStartWithWindowsEnabled();

    // cloud_fallback_enabled has no Snapshot entry (it is read by the engine off
    // the GUI-thread critical path); the GUI-thread read is the documented one.
    draft.cloud_fallback = config.cloud_fallback_enabled;

    // The hotkey rows seed through the RESOLVE helpers, not the raw strings: a
    // pre-existing malformed config value already behaves as the default in the
    // hook, and showing its garbage spelling here would refuse [확인] forever.
    draft.hotkey[0] =
        MakeHubHotkeyField(CanonicalHotkey(KeyboardHook::ResolveToggleFromConfig(
            config.hotkey_toggle)));
    draft.hotkey[1] =
        MakeHubHotkeyField(CanonicalHotkey(KeyboardHook::ResolveLangFromConfig(
            config.hotkey_lang)));
    draft.hotkey[2] =
        MakeHubHotkeyField(CanonicalHotkey(KeyboardHook::ResolveModeFromConfig(
            config.hotkey_mode)));
    draft.hotkey[3] =
        MakeHubHotkeyField(CanonicalHotkey(KeyboardHook::ResolveSelectionFromConfig(
            config.hotkey_selection_translate)));

    draft.warmup_combo_index =
        HubWarmupComboIndex(config.GetEngineHostConfig().warmup);
    draft.diag_log_enabled = config.diag_log_enabled;
    draft.diag_log_content = config.diag_log_content;

    draft.temperature = ToUtf8(FormatHubFloat(config.temperature));
    draft.top_p = ToUtf8(FormatHubFloat(config.top_p));
    draft.top_k = std::to_string(config.top_k);
    draft.repetition_penalty = ToUtf8(FormatHubFloat(config.repetition_penalty));
    return draft;
}

HubApplyPlan FoldHubDraft(const HubDraft& edited, const HubDraft& seed) {
    HubApplyPlan plan;
    plan.ui_theme = AppConfig::NormalizeUiTheme(edited.ui_theme);
    plan.auto_send = edited.auto_send;
    plan.local_context = edited.local_context; // 261123_0001 ctx-pilot
    plan.back_translation = edited.back_translation;
    plan.sound = edited.sound;
    plan.diag_log_enabled = edited.diag_log_enabled;
    plan.diag_log_content = edited.diag_log_content;
    plan.warmup = HubComboIndexWarmup(edited.warmup_combo_index);

    // ---- hotkeys: the canonical spelling is the only thing ever persisted ----
    std::string* hotkey_out[4] = {
        &plan.extra.hotkey_toggle,
        &plan.extra.hotkey_lang,
        &plan.extra.hotkey_mode,
        &plan.extra.hotkey_selection_translate,
    };
    bool* hotkey_err[4] = {
        &plan.errors.hotkey_toggle,
        &plan.errors.hotkey_lang,
        &plan.errors.hotkey_mode,
        &plan.errors.hotkey_selection,
    };
    for (int i = 0; i < 4; ++i) {
        if (!edited.hotkey[i].valid) {
            *hotkey_err[i] = true;
            continue;
        }
        *hotkey_out[i] = edited.hotkey[i].canonical;
    }

    // ---- sampling: parse + clamp, refuse unparsable text ----
    plan.extra.temperature = 0.0f;
    plan.extra.top_p = 0.6f;
    plan.extra.top_k = 20;
    plan.extra.repetition_penalty = 1.0f;
    if (!ParseHubFloat(edited.temperature, kHubTemperatureMin, kHubTemperatureMax,
                       plan.extra.temperature)) {
        plan.errors.temperature = true;
    }
    if (!ParseHubFloat(edited.top_p, kHubTopPMin, kHubTopPMax, plan.extra.top_p)) {
        plan.errors.top_p = true;
    }
    if (!ParseHubInt(edited.top_k, kHubTopKMin, kHubTopKMax, plan.extra.top_k)) {
        plan.errors.top_k = true;
    }
    if (!ParseHubFloat(edited.repetition_penalty, kHubRepPenaltyMin,
                       kHubRepPenaltyMax, plan.extra.repetition_penalty)) {
        plan.errors.repetition_penalty = true;
    }

    // ---- the two lock-free toggles ----
    plan.extra.drag_to_translate = edited.drag_to_translate;
    plan.extra.cloud_fallback_enabled = edited.cloud_fallback;

    // ---- the diff against the seed (an untouched hub never writes) ----
    plan.any_change =
        plan.ui_theme != AppConfig::NormalizeUiTheme(seed.ui_theme) ||
        plan.auto_send != seed.auto_send ||
        plan.local_context != seed.local_context ||
        plan.back_translation != seed.back_translation ||
        plan.sound != seed.sound ||
        plan.diag_log_enabled != seed.diag_log_enabled ||
        plan.diag_log_content != seed.diag_log_content ||
        plan.warmup != HubComboIndexWarmup(seed.warmup_combo_index) ||
        plan.extra.drag_to_translate != seed.drag_to_translate ||
        plan.extra.cloud_fallback_enabled != seed.cloud_fallback;
    for (int i = 0; i < 4; ++i) {
        if (edited.hotkey[i].valid && edited.hotkey[i].canonical != seed.hotkey[i].canonical) {
            plan.any_change = true;
        }
    }
    // A seed that cannot be re-parsed (impossible from MakeHubDraft, but a
    // hand-built draft in a test may be) never registers as a change.
    float seed_value = plan.extra.temperature;
    if (ParseHubFloat(seed.temperature, kHubTemperatureMin, kHubTemperatureMax,
                      seed_value) &&
        seed_value != plan.extra.temperature) {
        plan.any_change = true;
    }
    seed_value = plan.extra.top_p;
    if (ParseHubFloat(seed.top_p, kHubTopPMin, kHubTopPMax, seed_value) &&
        seed_value != plan.extra.top_p) {
        plan.any_change = true;
    }
    int seed_int = plan.extra.top_k;
    if (ParseHubInt(seed.top_k, kHubTopKMin, kHubTopKMax, seed_int) &&
        seed_int != plan.extra.top_k) {
        plan.any_change = true;
    }
    seed_value = plan.extra.repetition_penalty;
    if (ParseHubFloat(seed.repetition_penalty, kHubRepPenaltyMin,
                      kHubRepPenaltyMax, seed_value) &&
        seed_value != plan.extra.repetition_penalty) {
        plan.any_change = true;
    }

    plan.ok = !plan.errors.any();
    return plan;
}

std::wstring ComposePairSummary(std::string_view source, std::string_view target) {
    if (source.empty() && target.empty()) return std::wstring();
    return I18n::GetLanguageDisplayName(source) + L" \u2192 " +
           I18n::GetLanguageDisplayName(target);
}

std::wstring ComposeUserLanguageSummary(const std::vector<UserLanguage>& entries) {
    constexpr std::size_t kMaxShown = 4;
    std::wstring out;
    const std::size_t shown = entries.size() < kMaxShown ? entries.size() : kMaxShown;
    for (std::size_t i = 0; i < shown; ++i) {
        if (i != 0) out += L", ";
        out += ToUtf16(UserLanguageStoredName(entries[i]));
    }
    if (entries.size() > shown) out += L" \u2026";
    return out;
}

std::wstring ComposeDeclaredLanguagesSummary(
    const std::vector<std::string>& declared) {
    if (declared.empty()) return std::wstring();
    const std::wstring count = std::to_wstring(declared.size());
    return ReplaceTokenOnce(I18n::Get(StringId::LanguageSearchCount), L"{n}", count);
}

std::wstring DescribeUiLanguageValue(std::string_view persisted) {
    const std::string_view trimmed = TrimAscii(persisted);
    if (trimmed.empty() || EqualsAsciiNoCase(trimmed, "auto")) {
        return I18n::Get(StringId::MenuUiLanguageAuto);
    }
    std::wstring code = ToUtf16(trimmed);
    for (wchar_t& ch : code) {
        if (ch >= L'a' && ch <= L'z') {
            ch = static_cast<wchar_t>(ch - L'a' + L'A');
        }
    }
    return code;
}

UserLanguageRemovalPlan PlanUserLanguageRemoval(
    const std::vector<UserLanguage>& entries,
    std::string_view selected_stored_name,
    bool writes_allowed) {
    UserLanguageRemovalPlan plan;
    // PR9-12: while a newer-schema block is preserved verbatim every write verb
    // refuses, so an offered delete would silently do nothing.
    if (!writes_allowed) {
        plan.carrier_blocks = true;
        return plan;
    }
    if (selected_stored_name.empty()) return plan;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (UserLanguageStoredName(entries[i]) == selected_stored_name) {
            plan.enabled = true;
            plan.index = i;
            return plan;
        }
    }
    plan.not_found = true;
    return plan;
}

} // namespace settings_hub

// ---------------------------------------------------------------------------
// SECTION B: the window
// ---------------------------------------------------------------------------
namespace settings_hub {
namespace {

const WORD kHotkeyEditIds[4] = {kIdHotkeyToggle, kIdHotkeyLang, kIdHotkeyMode,
                                kIdHotkeySelection};

// The single live dialog's mutable state. Static because a MODELESS dialog
// outlives the OpenSettingsHub stack frame (the translation_editor_window
// State idiom); every field is touched on the GUI thread only.
struct HubDialogState {
    AppConfig* config = nullptr;
    HubActions actions; // deep copy of the injection slot at open time
    HubRuntimeState runtime;
    HubDraft seed; // what [취소] would leave untouched
    HWND hwnd = nullptr;
    // The last VALID canonical spelling per hotkey slot (HubHotkeySlot order):
    // an unparsable edit springs back to it, so the field can never hold
    // something the hook would refuse.
    std::string hotkey_last_valid[4];
};

// The process-wide injection slot + the live window (GUI-thread-only:
// SetHubActions at startup, the rest inside the dialog proc). No lock.
HubActions g_actions;
HubDialogState* g_live = nullptr;

void SetCtrlText(HWND dlg, int id, std::wstring_view text) {
    if (HWND c = ::GetDlgItem(dlg, id)) {
        const std::wstring value(text);
        ::SetWindowTextW(c, value.c_str());
    }
}

void SetCtrlEnabled(HWND dlg, int id, bool enabled) {
    if (HWND c = ::GetDlgItem(dlg, id)) {
        ::EnableWindow(c, enabled ? TRUE : FALSE);
    }
}

bool IsChecked(HWND dlg, int id) {
    return ::IsDlgButtonChecked(dlg, id) == BST_CHECKED;
}

std::string ReadEditUtf8(HWND dlg, int id) {
    HWND edit = ::GetDlgItem(dlg, id);
    if (edit == nullptr) return std::string();
    const int length = ::GetWindowTextLengthW(edit);
    if (length <= 0) return std::string();
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int got = ::GetWindowTextW(edit, text.data(),
                                     static_cast<int>(text.size()));
    if (got <= 0) return std::string();
    text.resize(static_cast<std::size_t>(got));
    return ToUtf8(text);
}

int ReadComboIndex(HWND dlg, int id, int fallback) {
    HWND combo = ::GetDlgItem(dlg, id);
    if (combo == nullptr) return fallback;
    const int sel = static_cast<int>(::SendMessageW(combo, CB_GETCURSEL, 0, 0));
    return sel < 0 ? fallback : sel;
}

// The four page summaries + the UI-language value cell, always re-read from the
// LIVE config: any callback the hub launches (picker / add dialog / manager /
// declared reset) can move them while the window stays open.
void RefreshDerivedLabels(HWND dlg, const AppConfig& config) {
    const AppConfig::Snapshot snap = config.GetSnapshot();
    SetCtrlText(dlg, kIdUiLanguageValue,
                settings_hub::DescribeUiLanguageValue(snap.ui_language));
    SetCtrlText(dlg, kIdTypingPairValue,
                settings_hub::ComposePairSummary(snap.type_source_language,
                                                 snap.type_target_language));
    SetCtrlText(dlg, kIdDragPairValue,
                settings_hub::ComposePairSummary(snap.drag_source_language,
                                                 snap.drag_target_language));
    SetCtrlText(dlg, kIdUserLangSummary,
                settings_hub::ComposeUserLanguageSummary(config.user_languages));
    SetCtrlText(dlg, kIdDeclaredSummary,
                settings_hub::ComposeDeclaredLanguagesSummary(
                    config.openai_declared_languages));
}

void SyncWarmupDesc(HWND dlg) {
    const int index = ReadComboIndex(dlg, kIdWarmupCombo, 0);
    SetCtrlText(dlg, kIdWarmupDesc,
                I18n::Get(HubComboIndexWarmup(index) ? StringId::LoadModeWarmupDesc
                                                     : StringId::LoadModeOnUseDesc));
}

// Pushes the whole draft into the controls (WM_INITDIALOG and the post-callback
// refresh after the user-language surfaces changed).
void SyncControlsFromDraft(HWND dlg, const HubDraft& draft) {
    // 261123_0001 ctx-pilot: kIdLocalContext joins the General toggle row set
    // (8 entries, template order: the auto-send row, then its context row).
    const int toggle_ids[8] = {kIdAutoSend, kIdBackTranslation, kIdLocalContext,
                               kIdSound, kIdBadge, kIdStartWithWindows,
                               kIdDragToTranslate, kIdCloudFallback};
    const bool toggle_values[8] = {
        draft.auto_send,      draft.back_translation, draft.local_context,
        draft.sound,          draft.badge_visible,    draft.start_with_windows,
        draft.drag_to_translate, draft.cloud_fallback};
    for (int i = 0; i < 8; ++i) {
        ::CheckDlgButton(dlg, toggle_ids[i],
                         toggle_values[i] ? BST_CHECKED : BST_UNCHECKED);
    }

    ::CheckDlgButton(dlg, kIdThemeFluent,
                     draft.ui_theme == "fluent" ? BST_CHECKED : BST_UNCHECKED);
    ::CheckDlgButton(dlg, kIdThemeEditorial,
                     draft.ui_theme == "editorial" ? BST_CHECKED : BST_UNCHECKED);
    ::CheckDlgButton(dlg, kIdThemeMonolith,
                     draft.ui_theme == "monolith" ? BST_CHECKED : BST_UNCHECKED);

    for (int i = 0; i < 4; ++i) {
        SetCtrlText(dlg, kHotkeyEditIds[i],
                    ToUtf16(draft.hotkey[i].valid ? draft.hotkey[i].canonical
                                                  : draft.hotkey[i].raw));
    }

    if (HWND combo = ::GetDlgItem(dlg, kIdWarmupCombo)) {
        ::SendMessageW(combo, CB_SETCURSEL,
                       static_cast<WPARAM>(draft.warmup_combo_index < 0
                                               ? 0
                                               : draft.warmup_combo_index),
                       0);
    }
    SyncWarmupDesc(dlg);

    ::CheckDlgButton(dlg, kIdDiagLogEnabled,
                     draft.diag_log_enabled ? BST_CHECKED : BST_UNCHECKED);
    ::CheckDlgButton(dlg, kIdDiagLogContent,
                     draft.diag_log_content ? BST_CHECKED : BST_UNCHECKED);
    SetCtrlText(dlg, kIdTemperature, ToUtf16(draft.temperature));
    SetCtrlText(dlg, kIdTopP, ToUtf16(draft.top_p));
    SetCtrlText(dlg, kIdTopK, ToUtf16(draft.top_k));
    SetCtrlText(dlg, kIdRepPenalty, ToUtf16(draft.repetition_penalty));
}

// Reads every editable control back into the draft (the seed supplies the
// fields that have no control: ui_language, and the badge/registry values the
// hub applies outside AppConfig).
HubDraft ReadDraftFromControls(HWND dlg, const HubDraft& seed) {
    HubDraft draft = seed;
    draft.auto_send = IsChecked(dlg, kIdAutoSend);
    draft.local_context = IsChecked(dlg, kIdLocalContext); // 261123_0001 ctx-pilot
    draft.back_translation = IsChecked(dlg, kIdBackTranslation);
    draft.sound = IsChecked(dlg, kIdSound);
    draft.badge_visible = IsChecked(dlg, kIdBadge);
    draft.start_with_windows = IsChecked(dlg, kIdStartWithWindows);
    draft.drag_to_translate = IsChecked(dlg, kIdDragToTranslate);
    draft.cloud_fallback = IsChecked(dlg, kIdCloudFallback);

    draft.ui_theme = IsChecked(dlg, kIdThemeEditorial)  ? "editorial"
                     : IsChecked(dlg, kIdThemeMonolith) ? "monolith"
                                                        : "fluent";

    for (int i = 0; i < 4; ++i) {
        draft.hotkey[i] = MakeHubHotkeyField(ReadEditUtf8(dlg, kHotkeyEditIds[i]));
    }

    draft.warmup_combo_index =
        ReadComboIndex(dlg, kIdWarmupCombo, seed.warmup_combo_index);

    draft.diag_log_enabled = IsChecked(dlg, kIdDiagLogEnabled);
    draft.diag_log_content = IsChecked(dlg, kIdDiagLogContent);
    draft.temperature = ReadEditUtf8(dlg, kIdTemperature);
    draft.top_p = ReadEditUtf8(dlg, kIdTopP);
    draft.top_k = ReadEditUtf8(dlg, kIdTopK);
    draft.repetition_penalty = ReadEditUtf8(dlg, kIdRepPenalty);
    return draft;
}

// Enables exactly the controls whose backing callback exists. A half-wired
// build shows a greyed row instead of a button that silently does nothing.
void SyncActionGates(HWND dlg, const HubActions& actions) {
    SetCtrlEnabled(dlg, kIdUiLanguageSearch,
                   static_cast<bool>(actions.open_ui_language_picker));
    SetCtrlEnabled(dlg, kIdTypingPairChange,
                   static_cast<bool>(actions.change_typing_pair));
    SetCtrlEnabled(dlg, kIdDragPairChange,
                   static_cast<bool>(actions.change_drag_pair));
    SetCtrlEnabled(dlg, kIdOpenAi, static_cast<bool>(actions.open_openai));
    SetCtrlEnabled(dlg, kIdGgufManager,
                   static_cast<bool>(actions.open_gguf_manager));
    SetCtrlEnabled(dlg, kIdModelDownload,
                   static_cast<bool>(actions.open_model_download));
    SetCtrlEnabled(dlg, kIdUserLangAdd,
                   static_cast<bool>(actions.open_user_language));
    SetCtrlEnabled(dlg, kIdUserLangManage,
                   static_cast<bool>(actions.open_user_language_manager));
    SetCtrlEnabled(dlg, kIdCheckUpdates, static_cast<bool>(actions.check_updates));
    SetCtrlEnabled(dlg, kIdResetDefaults, static_cast<bool>(actions.reset_defaults));
    SetCtrlEnabled(dlg, kIdCheatSheet, static_cast<bool>(actions.open_cheat_sheet));
    SetCtrlEnabled(dlg, kIdBadge, static_cast<bool>(actions.set_badge_visible));

    // The remaining extra fields (drag / cloud fallback) have no AppConfig verb
    // and stay with the caller's apply_extra; the hotkey/sampling/diag fields
    // moved to the locked config verbs in the V10F-R1 F-02 round.
    const bool extras = static_cast<bool>(actions.apply_extra);
    for (int i = 0; i < 4; ++i) SetCtrlEnabled(dlg, kHotkeyEditIds[i], extras);
    SetCtrlEnabled(dlg, kIdTemperature, extras);
    SetCtrlEnabled(dlg, kIdTopP, extras);
    SetCtrlEnabled(dlg, kIdTopK, extras);
    SetCtrlEnabled(dlg, kIdRepPenalty, extras);
    SetCtrlEnabled(dlg, kIdDragToTranslate, extras);
    SetCtrlEnabled(dlg, kIdCloudFallback, extras);
}

void ShowPage(HWND dlg, int tab) {
    const int active = (tab >= 0 && tab < kHubTabCount) ? tab : 0;
    for (std::size_t i = 0; i < kHubPageControlCount; ++i) {
        const WORD id = kHubPageControlIds[i];
        if (HWND control = ::GetDlgItem(dlg, id)) {
            ::ShowWindow(control, HubControlTab(id) == active ? SW_SHOW : SW_HIDE);
        }
    }
}

// The tab strip: SysTabControl32 has NO predefined dialog-template class atom,
// so it is created here into a MapDialogRect-mapped DLU rect (the HfAdd
// progress-bar precedent). It is a HEADER BAND only (y 6..30 DLU): the page
// controls are DLGTEMPLATE children BELOW it, so a tab repaint can never erase
// one.
void CreateTabStrip(HWND dlg) {
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TAB_CLASSES;
    ::InitCommonControlsEx(&icc);

    RECT rc = kHubTabStripDlu;
    ::MapDialogRect(dlg, &rc);
    HWND tab = ::CreateWindowExW(
        0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_FOCUSNEVER, rc.left, rc.top,
        rc.right - rc.left, rc.bottom - rc.top, dlg,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdTab)),
        ::GetModuleHandleW(nullptr), nullptr);
    if (tab == nullptr) {
        DIAG_F("UI/SettingsHub/001: tab strip creation failed (GLE %lu)\n",
               ::GetLastError());
        return;
    }
    // The tab does not inherit the dialog font (created at runtime).
    if (HFONT font = reinterpret_cast<HFONT>(
            ::SendMessageW(dlg, WM_GETFONT, 0, 0))) {
        ::SendMessageW(tab, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    for (int i = 0; i < kHubTabCount; ++i) {
        const std::wstring caption = I18n::Get(HubTabCaptionId(i));
        TCITEMW item = {};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(caption.c_str());
        ::SendMessageW(tab, TCM_INSERTITEMW, static_cast<WPARAM>(i),
                       reinterpret_cast<LPARAM>(&item));
    }
    ::SendMessageW(tab, TCM_SETCURSEL, 0, 0);
}

void FocusFirstInvalid(HWND dlg, const HubFieldErrors& errors) {
    if (errors.hotkey_toggle) {
        ::SetFocus(::GetDlgItem(dlg, kIdHotkeyToggle));
    } else if (errors.hotkey_lang) {
        ::SetFocus(::GetDlgItem(dlg, kIdHotkeyLang));
    } else if (errors.hotkey_mode) {
        ::SetFocus(::GetDlgItem(dlg, kIdHotkeyMode));
    } else if (errors.hotkey_selection) {
        ::SetFocus(::GetDlgItem(dlg, kIdHotkeySelection));
    } else if (errors.temperature) {
        ::SetFocus(::GetDlgItem(dlg, kIdTemperature));
    } else if (errors.top_p) {
        ::SetFocus(::GetDlgItem(dlg, kIdTopP));
    } else if (errors.top_k) {
        ::SetFocus(::GetDlgItem(dlg, kIdTopK));
    } else if (errors.repetition_penalty) {
        ::SetFocus(::GetDlgItem(dlg, kIdRepPenalty));
    }
}

// EN_KILLFOCUS: a combo is normalized to its canonical spelling and kept as the
// field's last valid value; an unparsable one springs back to that value (the
// manager dialog's CheckDlgButton discipline: the control mirrors what would be
// stored, never the keystroke). No new string: the beep + the revert ARE the
// feedback.
void NormalizeHotkeyEdit(HWND dlg, HubDialogState* st, WORD id) {
    const int slot = HubHotkeySlot(id);
    if (slot < 0) return;
    const HubHotkeyField field = MakeHubHotkeyField(ReadEditUtf8(dlg, id));
    if (field.valid) {
        st->hotkey_last_valid[slot] = field.canonical;
        SetCtrlText(dlg, id, ToUtf16(field.canonical));
        return;
    }
    SetCtrlText(dlg, id, ToUtf16(st->hotkey_last_valid[slot]));
    ::MessageBeep(MB_ICONWARNING);
    DIAG_F("UI/SettingsHub/002: hotkey slot %d refused an unparsable combo\n", slot);
}

void OpenConfigFolder(HWND dlg) {
    const std::filesystem::path config_path = AppConfig::GetDefaultConfigPath();
    const std::filesystem::path folder = config_path.parent_path();
    if (folder.empty()) {
        DIAG_F("UI/SettingsHub/003: config folder unresolved\n");
        return;
    }
    // "explore" is the briefed verb; older shells can refuse it on some
    // paths, so "open" is the fallback (both end in Explorer).
    HINSTANCE rc = ::ShellExecuteW(dlg, L"explore", folder.c_str(), nullptr,
                                   nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(rc) <= 32) {
        rc = ::ShellExecuteW(dlg, L"open", folder.c_str(), nullptr, nullptr,
                             SW_SHOWNORMAL);
    }
    if (reinterpret_cast<INT_PTR>(rc) <= 32) {
        DIAG_F("UI/SettingsHub/004: ShellExecuteW failed (code %lld)\n",
               static_cast<long long>(reinterpret_cast<INT_PTR>(rc)));
    }
}

// [확인]: fold, apply through the locked verbs / atomics, hand the two
// no-locked-verb fields (drag / cloud fallback) to the caller, persist,
// report. Stays open (with the focus on the first offending edit) when the
// fold refused.
void ApplyAndClose(HWND dlg, HubDialogState* st) {
    if (st->config == nullptr) {
        ::DestroyWindow(dlg);
        return;
    }
    const HubDraft edited = ReadDraftFromControls(dlg, st->seed);
    const HubApplyPlan plan = FoldHubDraft(edited, st->seed);
    if (!plan.ok) {
        DIAG_F("UI/SettingsHub/005: apply refused; unparsable field(s) remain\n");
        FocusFirstInvalid(dlg, plan.errors);
        ::MessageBeep(MB_ICONWARNING);
        return; // never destroy: the user fixes the field
    }

    HubApplyResult result;
    result.any_change = plan.any_change;

    // Locked verb 1: the "engine_host" block (BUG-08 write surface).
    EngineHostConfig engine_host = st->config->GetEngineHostConfig();
    if (engine_host.warmup != plan.warmup) {
        engine_host.warmup = plan.warmup;
        st->config->SetEngineHostConfig(engine_host);
        result.engine_host_changed = true;
    }

    // Locked verb 2: the UI theme.
    const std::string seed_theme = AppConfig::NormalizeUiTheme(st->seed.ui_theme);
    if (plan.ui_theme != seed_theme) {
        st->config->SetUiTheme(plan.ui_theme);
        result.theme_changed = true;
    }

    // Atomic toggles: the tray's own GUI-thread flip idiom (on_toggle_sound /
    // on_toggle_back_translate), so hook / worker readers stay race-free.
    st->config->auto_send.store(plan.auto_send, std::memory_order_relaxed);
    st->config->back_translation_enabled.store(plan.back_translation,
                                               std::memory_order_relaxed);
    st->config->sound_enabled.store(plan.sound, std::memory_order_relaxed);

    // 261123_0001 ctx-pilot: local_context_enabled is a mutex-backed field, not
    // one of the atomics above, so it writes through its own locked verb: an
    // off-thread SaveToFile serialises it under mutex_, and a plain assignment
    // here would be the BUG-08 interleave this file's contract forbids.
    st->config->SetLocalContextEnabled(plan.local_context);

    // Startup-applied diagnostics switches: no runtime reader exists (main.cpp
    // caches them into diag::SetEnabled / SetContentLogging before threads), so
    // the write takes effect on restart. V10F-R1 F-02: through the locked verbs
    // (the BUG-08 class — an off-thread SaveToFile serialises these fields under
    // mutex_, so a plain GUI-thread write can interleave with that iteration).
    st->config->SetDiagLogEnabled(plan.diag_log_enabled);
    st->config->SetDiagLogContent(plan.diag_log_content);

    // The remaining extra fields (drag / cloud fallback): applied by the caller
    // (see the header contract). The hotkey / sampling fields travel in the same
    // struct but the caller applies them through the locked config verbs
    // (V10F-R1 F-02).
    if (st->actions.apply_extra) {
        st->actions.apply_extra(plan.extra);
    }

    // Runtime-only surfaces.
    if (st->actions.set_badge_visible &&
        edited.badge_visible != st->seed.badge_visible) {
        st->actions.set_badge_visible(edited.badge_visible);
    }
    if (edited.start_with_windows != st->seed.start_with_windows) {
        // The registry Run key (the tray's ID_TRAY_START_WITH_WINDOWS path).
        I18n::SetStartWithWindows(edited.start_with_windows);
    }

    if (plan.any_change) {
        st->config->SaveToFile();
    }
    DIAG_F("UI/SettingsHub/006: applied change=%d theme=%d engine_host=%d\n",
           plan.any_change ? 1 : 0, result.theme_changed ? 1 : 0,
           result.engine_host_changed ? 1 : 0);

    if (st->actions.after_apply) {
        st->actions.after_apply(result);
    }
    ::DestroyWindow(dlg);
}

// One injected action + the derived-label refresh (every surface it opens can
// move the pair / user-language / declared summaries underneath).
void RunAction(HWND dlg, HubDialogState* st, const std::function<void()>& action) {
    if (!action) {
        DIAG_F("UI/SettingsHub/007: action not wired\n");
        return;
    }
    action();
    if (st->config != nullptr) {
        RefreshDerivedLabels(dlg, *st->config);
        // The picker persists the UI language itself; re-seed the draft half so
        // a later [확인] does not diff against a stale value.
        st->seed.ui_language = st->config->GetSnapshot().ui_language;
    }
}

INT_PTR CALLBACK SettingsHubDialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<HubDialogState*>(
        ::GetWindowLongPtrW(dlg, GWLP_USERDATA));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<HubDialogState*>(lp);
        ::SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        if (st == nullptr || st->config == nullptr) {
            DIAG_F("UI/SettingsHub/008: dialog init without a config\n");
            // MODELESS: DestroyWindow, never EndDialog (which only ends a modal
            // DialogBoxIndirectParamW loop).
            ::DestroyWindow(dlg);
            return FALSE;
        }
        st->hwnd = dlg;
        g_live = st;

        // Control-creation verification (the REQ-047 D3 contract): a control
        // that failed to materialize must be separable from a logic bug.
        if (!::GetDlgItem(dlg, kIdAutoSend) ||
            !::GetDlgItem(dlg, kIdLocalContext) ||
            !::GetDlgItem(dlg, kIdDiagLogEnabled) ||
            !::GetDlgItem(dlg, kIdUserLangAdd)) {
            DIAG_F("UI/SettingsHub/009: page controls missing after dialog init\n");
        }

        // Fixed combo content (CBS_DROPDOWNLIST: no free text).
        if (HWND combo = ::GetDlgItem(dlg, kIdWarmupCombo)) {
            ::SendMessageW(combo, CB_ADDSTRING, 0,
                           reinterpret_cast<LPARAM>(
                               I18n::Get(StringId::LoadModeOnUse).c_str()));
            ::SendMessageW(combo, CB_ADDSTRING, 0,
                           reinterpret_cast<LPARAM>(
                               I18n::Get(StringId::LoadModeWarmup).c_str()));
        }

        CreateTabStrip(dlg);
        SyncControlsFromDraft(dlg, st->seed);
        for (int i = 0; i < 4; ++i) {
            st->hotkey_last_valid[i] = st->seed.hotkey[i].canonical;
        }
        ShowPage(dlg, 0);
        SyncActionGates(dlg, st->actions);
        if (st->config != nullptr) {
            RefreshDerivedLabels(dlg, *st->config);
        }
        return TRUE;
    }
    case WM_NOTIFY: {
        const LPNMHDR header = reinterpret_cast<LPNMHDR>(lp);
        // TCN_SELCHANGE is a NEGATIVE notification code; cast the header's
        // UINT code to int so the comparison is signed on both sides (/W4
        // C4287 would flag the unsigned/negative mix otherwise).
        if (header != nullptr && header->idFrom == kIdTab &&
            static_cast<int>(header->code) == TCN_SELCHANGE) {
            const int selected = TabCtrl_GetCurSel(header->hwndFrom);
            ShowPage(dlg, selected < 0 ? 0 : selected);
            return TRUE;
        }
        return FALSE;
    }
    case WM_COMMAND: {
        if (st == nullptr) return FALSE;
        const WORD id = LOWORD(wp);
        const WORD code = HIWORD(wp);
        switch (id) {
        case IDOK:
            ApplyAndClose(dlg, st);
            return TRUE;
        case IDCANCEL: // [취소], Esc and the caption X all discard
            ::DestroyWindow(dlg);
            return TRUE;
        case kIdWarmupCombo:
            if (code == CBN_SELCHANGE) {
                SyncWarmupDesc(dlg);
                return TRUE;
            }
            return FALSE;
        case kIdHotkeyToggle:
        case kIdHotkeyLang:
        case kIdHotkeyMode:
        case kIdHotkeySelection:
            if (code == EN_KILLFOCUS) {
                NormalizeHotkeyEdit(dlg, st, id);
                return TRUE;
            }
            return FALSE;
        case kIdUiLanguageSearch:
            if (code == BN_CLICKED) {
                RunAction(dlg, st, st->actions.open_ui_language_picker);
                return TRUE;
            }
            return FALSE;
        case kIdTypingPairChange:
            if (code == BN_CLICKED) {
                RunAction(dlg, st, st->actions.change_typing_pair);
                return TRUE;
            }
            return FALSE;
        case kIdDragPairChange:
            if (code == BN_CLICKED) {
                RunAction(dlg, st, st->actions.change_drag_pair);
                return TRUE;
            }
            return FALSE;
        case kIdOpenAi:
            if (code == BN_CLICKED) {
                RunAction(dlg, st, st->actions.open_openai);
                return TRUE;
            }
            return FALSE;
        case kIdGgufManager:
            if (code == BN_CLICKED) {
                RunAction(dlg, st, st->actions.open_gguf_manager);
                return TRUE;
            }
            return FALSE;
        case kIdModelDownload:
            if (code == BN_CLICKED) {
                RunAction(dlg, st, st->actions.open_model_download);
                return TRUE;
            }
            return FALSE;
        case kIdUserLangAdd:
            if (code == BN_CLICKED) {
                RunAction(dlg, st, st->actions.open_user_language);
                return TRUE;
            }
            return FALSE;
        case kIdUserLangManage:
            if (code == BN_CLICKED) {
                RunAction(dlg, st, st->actions.open_user_language_manager);
                return TRUE;
            }
            return FALSE;
        case kIdCheckUpdates:
            if (code == BN_CLICKED) {
                RunAction(dlg, st, st->actions.check_updates);
                return TRUE;
            }
            return FALSE;
        case kIdResetDefaults:
            if (code == BN_CLICKED) {
                RunAction(dlg, st, st->actions.reset_defaults);
                return TRUE;
            }
            return FALSE;
        case kIdCheatSheet:
            if (code == BN_CLICKED) {
                RunAction(dlg, st, st->actions.open_cheat_sheet);
                return TRUE;
            }
            return FALSE;
        case kIdResetDeclared: {
            if (code != BN_CLICKED) return FALSE;
            const std::size_t before = st->config->openai_declared_languages.size();
            // Locked verb + immediate persist: a reset is a complete edit on its
            // own (the REQ-051 delete-path precedent), so it must not depend on
            // the user also pressing [확인].
            st->config->SetOpenAiDeclaredLanguages({});
            st->config->SaveToFile();
            DIAG_F("UI/SettingsHub/010: declared languages reset (had=%zu)\n",
                   before);
            RefreshDerivedLabels(dlg, *st->config);
            return TRUE;
        }
        case kIdOpenConfigFolder:
            if (code == BN_CLICKED) {
                OpenConfigFolder(dlg);
                return TRUE;
            }
            return FALSE;
        default:
            return FALSE;
        }
    }
    case WM_CLOSE:
        ::DestroyWindow(dlg);
        return TRUE;
    case WM_DESTROY:
        if (st != nullptr) {
            st->hwnd = nullptr;
        }
        g_live = nullptr;
        return TRUE;
    }
    return FALSE;
}

} // namespace
} // namespace settings_hub

// ---- SECTION B: the public entry points ----------------------------------

void BuildSettingsHubTemplate(TemplateBuilder& tb) {
    using namespace settings_hub;
    const DWORD LBL = WS_CHILD;
    const DWORD CHK = WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX;
    const DWORD RDO = WS_CHILD | WS_TABSTOP | BS_AUTORADIOBUTTON;
    const DWORD BTN = WS_CHILD | WS_TABSTOP;
    const DWORD EDT = WS_CHILD | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL;
    // CBS_DROPDOWNLIST: the fixed two-option load policy (the REQ-060 dialog's
    // combo shape; the drop height leaves both rows visible, no scrollbar).
    const DWORD COMBO = WS_CHILD | WS_TABSTOP | WS_BORDER | CBS_DROPDOWNLIST;
    const DWORD GRP = WS_GROUP;
    const WORD STATIC_CLS = 0x0082; // "STATIC"
    const WORD BTN_CLS = 0x0080;    // "BUTTON"
    const WORD EDIT_CLS = 0x0081;   // "EDIT"
    const WORD COMBO_CLS = 0x0085;  // "COMBOBOX"

    // ---- General (tab 0) --------------------------------------------------
    tb.AddItem(CHK | GRP, 8, 38, 196, 10, kIdAutoSend, BTN_CLS,
               I18n::Get(StringId::MenuAutoSend));
    tb.AddItem(CHK, 212, 38, 200, 10, kIdBackTranslation, BTN_CLS,
               I18n::Get(StringId::MenuBackTranslation));
    // 261123_0001 ctx-pilot: the new checkbox owns the full-width slot DIRECTLY
    // below the auto-send row (the cloud-fallback row's shape, so the longer
    // caption never clips), which pushes every General row from y 52 down by
    // 12 DLU. The page then ends at y 220, still 68 DLU clear of the y 288
    // footer, so kHubHeightDlu stays 312 (no shell growth is needed).
    tb.AddItem(CHK, 8, 52, 404, 10, kIdLocalContext, BTN_CLS,
               I18n::Get(StringId::SettingsLocalContext));
    tb.AddItem(CHK, 8, 64, 196, 10, kIdSound, BTN_CLS,
               I18n::Get(StringId::MenuSoundFeedback));
    tb.AddItem(CHK, 212, 64, 200, 10, kIdBadge, BTN_CLS,
               I18n::Get(StringId::MenuShowBadge));
    tb.AddItem(CHK, 8, 78, 196, 10, kIdStartWithWindows, BTN_CLS,
               I18n::Get(StringId::MenuStartWithWindows));
    tb.AddItem(CHK, 212, 78, 200, 10, kIdDragToTranslate, BTN_CLS,
               I18n::Get(StringId::SettingsDragToTranslate));
    tb.AddItem(CHK, 8, 92, 404, 10, kIdCloudFallback, BTN_CLS,
               I18n::Get(StringId::SettingsCloudFallback));
    tb.AddItem(LBL | GRP, 8, 110, 80, 9, kIdUiLanguageLabel, STATIC_CLS,
               I18n::Get(StringId::MenuUiLanguage));
    tb.AddItem(LBL, 92, 110, 120, 9, kIdUiLanguageValue, STATIC_CLS, L"");
    tb.AddItem(BTN, 216, 106, 60, 14, kIdUiLanguageSearch, BTN_CLS,
               I18n::Get(StringId::MenuSearchLangs));
    tb.AddItem(LBL, 8, 126, 80, 9, kIdUiThemeLabel, STATIC_CLS,
               I18n::Get(StringId::MenuUiTheme));
    tb.AddItem(RDO | GRP, 92, 124, 100, 10, kIdThemeFluent, BTN_CLS,
               I18n::Get(StringId::ThemeFluent));
    tb.AddItem(RDO, 196, 124, 100, 10, kIdThemeEditorial, BTN_CLS,
               I18n::Get(StringId::ThemeEditorial));
    tb.AddItem(RDO, 300, 124, 100, 10, kIdThemeMonolith, BTN_CLS,
               I18n::Get(StringId::ThemeMonolith));
    tb.AddItem(LBL | GRP, 8, 142, 404, 9, kIdHotkeyHeader, STATIC_CLS,
               I18n::Get(StringId::MenuCheatSheet));
    tb.AddItem(LBL, 8, 156, 120, 9, kIdHotkeyToggleLabel, STATIC_CLS,
               I18n::Get(StringId::SettingsHotkeyToggle));
    tb.AddItem(EDT, 132, 153, 180, 13, kIdHotkeyToggle, EDIT_CLS, L"");
    tb.AddItem(LBL, 8, 174, 120, 9, kIdHotkeyLangLabel, STATIC_CLS,
               I18n::Get(StringId::SettingsHotkeyLang));
    tb.AddItem(EDT, 132, 171, 180, 13, kIdHotkeyLang, EDIT_CLS, L"");
    tb.AddItem(LBL, 8, 192, 120, 9, kIdHotkeyModeLabel, STATIC_CLS,
               I18n::Get(StringId::SettingsHotkeyMode));
    tb.AddItem(EDT, 132, 189, 180, 13, kIdHotkeyMode, EDIT_CLS, L"");
    tb.AddItem(LBL, 8, 210, 120, 9, kIdHotkeySelectionLabel, STATIC_CLS,
               I18n::Get(StringId::SettingsHotkeySelection));
    tb.AddItem(EDT, 132, 207, 180, 13, kIdHotkeySelection, EDIT_CLS, L"");

    // ---- Engines (tab 1) --------------------------------------------------
    tb.AddItem(LBL | GRP, 8, 38, 240, 9, kIdWarmupLabel, STATIC_CLS,
               I18n::Get(StringId::EngineLoadSettingsTitle));
    tb.AddItem(COMBO, 8, 52, 240, 40, kIdWarmupCombo, COMBO_CLS, L"");
    // Two lines tall so the longer locale descriptions never clip.
    tb.AddItem(LBL, 8, 96, 404, 22, kIdWarmupDesc, STATIC_CLS, L"");
    tb.AddItem(BTN, 8, 126, 190, 16, kIdOpenAi, BTN_CLS,
               I18n::Get(StringId::OpenAiSettingsAction));
    tb.AddItem(BTN, 204, 126, 190, 16, kIdGgufManager, BTN_CLS,
               I18n::Get(StringId::MenuManageGgufModels));
    tb.AddItem(BTN, 8, 148, 190, 16, kIdModelDownload, BTN_CLS,
               I18n::Get(StringId::ModelDlCaption));
    tb.AddItem(BTN, 204, 148, 190, 16, kIdResetDeclared, BTN_CLS,
               I18n::Get(StringId::ResetDeclaredLanguages));
    tb.AddItem(LBL, 8, 170, 404, 9, kIdDeclaredSummary, STATIC_CLS, L"");

    // ---- Languages (tab 2) ------------------------------------------------
    tb.AddItem(LBL | GRP, 8, 38, 100, 9, kIdTypingPairLabel, STATIC_CLS,
               I18n::Get(StringId::MenuTypingGroup));
    tb.AddItem(LBL, 112, 38, 220, 9, kIdTypingPairValue, STATIC_CLS, L"");
    tb.AddItem(BTN, 340, 34, 72, 14, kIdTypingPairChange, BTN_CLS,
               I18n::Get(StringId::MenuSearchLangs));
    tb.AddItem(LBL, 8, 58, 100, 9, kIdDragPairLabel, STATIC_CLS,
               I18n::Get(StringId::MenuTooltipGroup));
    tb.AddItem(LBL, 112, 58, 220, 9, kIdDragPairValue, STATIC_CLS, L"");
    tb.AddItem(BTN, 340, 54, 72, 14, kIdDragPairChange, BTN_CLS,
               I18n::Get(StringId::MenuSearchLangs));
    tb.AddItem(LBL, 8, 86, 404, 9, kIdUserLangHeader, STATIC_CLS,
               I18n::Get(StringId::UserLangsGroup));
    tb.AddItem(LBL, 8, 100, 404, 22, kIdUserLangSummary, STATIC_CLS, L"");
    tb.AddItem(BTN, 8, 128, 190, 16, kIdUserLangAdd, BTN_CLS,
               I18n::Get(StringId::UserLangAddTitle));
    tb.AddItem(BTN, 204, 128, 190, 16, kIdUserLangManage, BTN_CLS,
               I18n::Get(StringId::UserLangsGroup));

    // ---- Advanced (tab 3) -------------------------------------------------
    tb.AddItem(CHK | GRP, 8, 38, 300, 10, kIdDiagLogEnabled, BTN_CLS,
               I18n::Get(StringId::SettingsDiagLogEnabled));
    tb.AddItem(CHK, 8, 52, 404, 10, kIdDiagLogContent, BTN_CLS,
               I18n::Get(StringId::SettingsDiagLogContent));
    tb.AddItem(LBL, 8, 66, 404, 20, kIdDiagContentWarning, STATIC_CLS,
               I18n::Get(StringId::SettingsDiagContentWarning));
    tb.AddItem(LBL, 8, 92, 120, 9, kIdTemperatureLabel, STATIC_CLS,
               I18n::Get(StringId::SettingsTemperature));
    tb.AddItem(EDT, 132, 89, 80, 13, kIdTemperature, EDIT_CLS, L"");
    tb.AddItem(LBL, 220, 92, 100, 9, kIdTopPLabel, STATIC_CLS,
               I18n::Get(StringId::SettingsTopP));
    tb.AddItem(EDT, 324, 89, 88, 13, kIdTopP, EDIT_CLS, L"");
    tb.AddItem(LBL, 8, 110, 120, 9, kIdTopKLabel, STATIC_CLS,
               I18n::Get(StringId::SettingsTopK));
    tb.AddItem(EDT, 132, 107, 80, 13, kIdTopK, EDIT_CLS, L"");
    tb.AddItem(LBL, 220, 110, 100, 9, kIdRepPenaltyLabel, STATIC_CLS,
               I18n::Get(StringId::SettingsRepetitionPenalty));
    tb.AddItem(EDT, 324, 107, 88, 13, kIdRepPenalty, EDIT_CLS, L"");
    tb.AddItem(BTN, 8, 132, 190, 16, kIdOpenConfigFolder, BTN_CLS,
               I18n::Get(StringId::OpenConfigFolder));
    tb.AddItem(BTN, 204, 132, 190, 16, kIdCheckUpdates, BTN_CLS,
               I18n::Get(StringId::UpdateCheck));
    tb.AddItem(BTN, 8, 154, 190, 16, kIdResetDefaults, BTN_CLS,
               I18n::Get(StringId::AboutResetButton));
    tb.AddItem(BTN, 204, 154, 190, 16, kIdCheatSheet, BTN_CLS,
               I18n::Get(StringId::MenuCheatSheet));

    // ---- footer (always visible: HubControlTab -> -1, so the page switch
    // never touches them and they must carry WS_VISIBLE themselves) ----------
    tb.AddItem(BTN | WS_VISIBLE | BS_DEFPUSHBUTTON, 272, 288, 64, 14, IDOK,
               BTN_CLS, I18n::Get(StringId::DialogOk));
    tb.AddItem(BTN | WS_VISIBLE, 344, 288, 64, 14, IDCANCEL, BTN_CLS,
               I18n::Get(StringId::DialogCancel));
}

void SetHubActions(HubActions actions) {
    settings_hub::g_actions = std::move(actions);
}

void OpenSettingsHub(AppConfig& config, const HubRuntimeState& runtime,
                     HWND parent) {
    using namespace settings_hub;

    // Single instance: a second open re-focuses the live window (the
    // translation_editor_window contract) instead of stacking a twin.
    if (g_live != nullptr && g_live->hwnd != nullptr &&
        ::IsWindow(g_live->hwnd)) {
        ::ShowWindow(g_live->hwnd, SW_SHOW);
        ::SetForegroundWindow(g_live->hwnd);
        return;
    }

    static HubDialogState state; // modeless lifetime (see g_live's note)
    state = HubDialogState{};
    state.config = &config;
    state.actions = g_actions; // deep copy: a later SetHubActions cannot swap it
    state.runtime = runtime;
    state.seed = MakeHubDraft(config, runtime);
    state.hwnd = nullptr;

    TemplateBuilder tb;
    tb.Begin(I18n::Get(StringId::SettingsHubTitle), kHubWidthDlu, kHubHeightDlu,
             kHubItemCount);
    BuildSettingsHubTemplate(tb);

    HWND dlg = ::CreateDialogIndirectParamW(
        ::GetModuleHandleW(nullptr), tb.Get(), parent, SettingsHubDialogProc,
        reinterpret_cast<LPARAM>(&state));
    if (dlg == nullptr) {
        DIAG_F("UI/SettingsHub/011: CreateDialogIndirectParamW failed (GLE %lu)\n",
               ::GetLastError());
        return;
    }
    ::ShowWindow(dlg, SW_SHOW);
    ::SetForegroundWindow(dlg);
}

} // namespace emebalachat
