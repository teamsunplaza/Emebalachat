#pragma once

// ---------------------------------------------------------------------------
// V10F UX-2 (261122_0002 P3-C3, v0.10.10 phased fixes): the SETTINGS HUB.
// ONE modeless Win32 window with a four-tab strip
// (SettingsTabGeneral / SettingsTabEngines / SettingsTabLanguages /
// SettingsTabAdvanced) that consolidates the surfaces the tray used to scatter
// over three submenus and five modal dialogs:
//
//   General   - the seven runtime toggles (auto-send, floating back-translation,
//               sound, floating badge, start-with-Windows, drag-to-translate,
//               cloud fallback), the UI-language row ([검색…] opens the shared
//               picker through an injected callback), the three UI themes, and
//               the four hotkey combos.
//   Engines   - the local load policy (engine_host.warmup) ABSORBED into a combo
//               here (the REQ-060 dialog stays as its own entry, unchanged) plus
//               the four engine-management actions (OpenAI settings / GGUF model
//               manager / bundled-model download / reset the OpenAI declared-
//               language selection) through injected callbacks.
//   Languages - the typing and drag language-pair summaries with a per-pair
//               [변경…] action, and the user-language CRUD entries ([내 언어
//               추가…] / [내 언어 관리…]).
//   Advanced  - the two diagnostic-log switches (with the content-logging
//               warning), the four sampling parameters, [config.json 폴더 열기]
//               (ShellExecuteW on %LOCALAPPDATA%\Emebalachat), and the
//               [업데이트 확인…] / [기본값 리셋…] / [단축키 안내…] actions.
//
// HOUSE CONTRACT (the openai / engine-host / user-language dialog pattern):
//   * TemplateBuilder builds the in-memory DLGTEMPLATE, CreateDialogIndirectParamW
//     runs it (MODELESS - the hub is a settings surface the user keeps open
//     while it launches the modal dialogs above; single instance: a second
//     OpenSettingsHub just re-focuses the live window, the
//     translation_editor_window precedent).
//   * System-class controls only in the template (STATIC / BUTTON / EDIT /
//     COMBOBOX atoms). The SysTabControl32 has NO predefined dialog-template
//     atom, so the tab strip is created at runtime in WM_INITDIALOG into a
//     MapDialogRect-mapped DLU rect (the HfAdd progress-bar precedent).
//   * Page switching is ShowWindow over the flat control set (every page
//     control is declared in kHubPageControlIds, and HubControlTab is the ONE
//     id->page mapping, so a control can never end up on two pages or on none).
//   * [확인] applies; [취소] / Esc / the caption ✕ discard. Edits reach
//     AppConfig ONLY through its LOCKED verbs (SetUiTheme / SetEngineHostConfig
//     / SetOpenAiDeclaredLanguages) or through the atomic toggles the app has
//     always flipped from the GUI thread (auto_send / back_translation_enabled /
//     sound_enabled, the tray on_toggle_sound precedent). The fields that have
//     NO locked verb (drag_to_translate, cloud_fallback_enabled) are handed to
//     the caller's apply_extra callback instead of being written here: a plain
//     std::string / float write racing an off-thread SaveToFile() is exactly the
//     class of bug V10F BUG-08 / PR9-12 closed for the openai / engine_host /
//     user-language blocks. The hotkey_* / sampling / diag_log_* fields got
//     their locked verbs in the V10F-R1 F-02 round and now write through them.
//     See the cross-unit contract in the unit report.
//   * DPI: the template is authored in DLU (the dialog manager maps it through
//     the dialog font / per-monitor DPI); everything created at runtime goes
//     through MapDialogRect first.
//
// The pure halves live in SECTION A below so the unit suite drives the whole
// save plan headlessly (zero windows, the p9b3 / p2_c split).
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include "../config.hpp"              // AppConfig, UserLanguage
#include "../i18n.hpp"                // StringId (the tab captions)
#include "engine_host_client.hpp"     // EngineHostConfig (warmup, REQ-060)
#include "openai_settings_window.hpp" // TemplateBuilder (the DLGTEMPLATE contract)

namespace emebalachat {

// ---------------------------------------------------------------------------
// Runtime-only values the hub cannot derive from AppConfig. main.cpp owns both
// live surfaces (the badge visibility flag, nothing else today), so it passes
// them in at open time; an unwired caller gets the truthful defaults.
// ---------------------------------------------------------------------------
struct HubRuntimeState {
    bool badge_visible = true;
};

namespace settings_hub {

// ---- SECTION A: the pure plan --------------------------------------------

// The four tabs, in strip order. The caption of tab N is HubTabCaptionId(N);
// the pinned witness is HubTabCaptionId(0) == SettingsTabGeneral and
// HubTabCaptionId(kHubTabCount - 1) == SettingsTabAdvanced.
enum class HubTab : int {
    kGeneral = 0,
    kEngines = 1,
    kLanguages = 2,
    kAdvanced = 3,
    kCount = 4,
};
inline constexpr int kHubTabCount = static_cast<int>(HubTab::kCount);

// Pure (constexpr): tab index -> the localized caption id. Out-of-range reads
// the FIRST tab's caption, so a bad index can never render a blank tab.
constexpr StringId HubTabCaptionId(int tab) {
    return tab == static_cast<int>(HubTab::kEngines)     ? StringId::SettingsTabEngines
           : tab == static_cast<int>(HubTab::kLanguages) ? StringId::SettingsTabLanguages
           : tab == static_cast<int>(HubTab::kAdvanced)  ? StringId::SettingsTabAdvanced
                                                         : StringId::SettingsTabGeneral;
}

// ---- dialog geometry + control ids (template-local; the 4800 band is free:
// manager 4700s, add-language 4600s, engine-host 4400s, HF-add 4300s, gguf
// manager 4100s, palette 4001-4005, openai 100s) ----
enum : WORD {
    kIdTab = 4800, // runtime-created tab strip (never in the template)

    // General page (4801-4823)
    kIdAutoSend = 4801,
    kIdBackTranslation,
    kIdSound,
    kIdBadge,
    kIdStartWithWindows,
    kIdDragToTranslate,
    kIdCloudFallback,
    kIdUiLanguageLabel,
    kIdUiLanguageValue,
    kIdUiLanguageSearch,
    kIdUiThemeLabel,
    kIdThemeFluent,
    kIdThemeEditorial,
    kIdThemeMonolith,
    kIdHotkeyHeader,
    kIdHotkeyToggleLabel,
    kIdHotkeyToggle,
    kIdHotkeyLangLabel,
    kIdHotkeyLang,
    kIdHotkeyModeLabel,
    kIdHotkeyMode,
    kIdHotkeySelectionLabel,
    kIdHotkeySelection,

    // Engines page (4830-4837)
    kIdWarmupLabel = 4830,
    kIdWarmupCombo,
    kIdWarmupDesc,
    kIdOpenAi,
    kIdGgufManager,
    kIdModelDownload,
    kIdResetDeclared,
    kIdDeclaredSummary,

    // Languages page (4840-4849)
    kIdTypingPairLabel = 4840,
    kIdTypingPairValue,
    kIdTypingPairChange,
    kIdDragPairLabel,
    kIdDragPairValue,
    kIdDragPairChange,
    kIdUserLangHeader,
    kIdUserLangSummary,
    kIdUserLangAdd,
    kIdUserLangManage,

    // Advanced page (4852-4866)
    kIdDiagLogEnabled = 4852,
    kIdDiagLogContent,
    kIdDiagContentWarning,
    kIdTemperatureLabel,
    kIdTemperature,
    kIdTopPLabel,
    kIdTopP,
    kIdTopKLabel,
    kIdTopK,
    kIdRepPenaltyLabel,
    kIdRepPenalty,
    kIdOpenConfigFolder,
    kIdCheckUpdates,
    kIdResetDefaults,
    kIdCheatSheet,
};

// DLU geometry. The strip is a header band only (y 6..30): the page controls
// are DLGTEMPLATE children BELOW it (y >= 36), never children of the tab, so no
// tab repaint can ever erase a page control. [확인]/[취소] sit at y 288.
inline constexpr short kHubWidthDlu = 420;
inline constexpr short kHubHeightDlu = 312;
// {left, top, right, bottom} in DLU: x 8..412, y 6..30.
inline constexpr RECT kHubTabStripDlu{8, 6, 412, 30};
// 56 page controls (23 General + 8 Engines + 10 Languages + 15 Advanced) +
// [확인] + [취소].
inline constexpr WORD kHubItemCount = 58;

// Every page control, in creation order. The ShowPage loop walks THIS table, so
// a control added to the template but forgotten here stays hidden (and the
// count pin in tests/p3_c3_tests.inc fails loudly).
inline constexpr WORD kHubPageControlIds[] = {
    kIdAutoSend, kIdBackTranslation, kIdSound, kIdBadge, kIdStartWithWindows,
    kIdDragToTranslate, kIdCloudFallback, kIdUiLanguageLabel, kIdUiLanguageValue,
    kIdUiLanguageSearch, kIdUiThemeLabel, kIdThemeFluent, kIdThemeEditorial,
    kIdThemeMonolith, kIdHotkeyHeader, kIdHotkeyToggleLabel, kIdHotkeyToggle,
    kIdHotkeyLangLabel, kIdHotkeyLang, kIdHotkeyModeLabel, kIdHotkeyMode,
    kIdHotkeySelectionLabel, kIdHotkeySelection,
    kIdWarmupLabel, kIdWarmupCombo, kIdWarmupDesc, kIdOpenAi, kIdGgufManager,
    kIdModelDownload, kIdResetDeclared, kIdDeclaredSummary,
    kIdTypingPairLabel, kIdTypingPairValue, kIdTypingPairChange, kIdDragPairLabel,
    kIdDragPairValue, kIdDragPairChange, kIdUserLangHeader, kIdUserLangSummary,
    kIdUserLangAdd, kIdUserLangManage,
    kIdDiagLogEnabled, kIdDiagLogContent, kIdDiagContentWarning,
    kIdTemperatureLabel, kIdTemperature, kIdTopPLabel, kIdTopP, kIdTopKLabel,
    kIdTopK, kIdRepPenaltyLabel, kIdRepPenalty, kIdOpenConfigFolder,
    kIdCheckUpdates, kIdResetDefaults, kIdCheatSheet,
};
inline constexpr std::size_t kHubPageControlCount =
    sizeof(kHubPageControlIds) / sizeof(kHubPageControlIds[0]);

// Pure (constexpr): control id -> owning tab, or -1 for chrome the page switch
// never touches (the tab strip itself, [확인], [취소]).
constexpr int HubControlTab(WORD id) {
    return (id >= kIdAutoSend && id <= kIdHotkeySelection) ? 0
           : (id >= kIdWarmupLabel && id <= kIdDeclaredSummary) ? 1
           : (id >= kIdTypingPairLabel && id <= kIdUserLangManage) ? 2
           : (id >= kIdDiagLogEnabled && id <= kIdCheatSheet) ? 3
                                                              : -1;
}

// Pure (constexpr): a hotkey EDIT control -> its slot in HubDraft (0 toggle,
// 1 language cycle, 2 auto-send toggle, 3 selection translate), -1 otherwise.
constexpr int HubHotkeySlot(WORD id) {
    return id == kIdHotkeyToggle ? 0
           : id == kIdHotkeyLang ? 1
           : id == kIdHotkeyMode ? 2
           : id == kIdHotkeySelection ? 3
                                      : -1;
}

// REQ-060's combo mapping, carried here so the hub and the still-live
// engine_host_settings_window cannot disagree: index 0 = "load when used"
// (warmup false, the engine default), index 1 = preload.
constexpr int HubWarmupComboIndex(bool warmup) { return warmup ? 1 : 0; }
constexpr bool HubComboIndexWarmup(int index) { return index == 1; }

// Sampling ranges the Advanced page accepts. A parseable number is CLAMPED into
// its range (the field is the user's intent, never an out-of-range engine
// config); unparsable text is REFUSED (errors.*), which keeps [확인] open with
// the focus on the offending edit instead of persisting a zeroed parameter.
inline constexpr float kHubTemperatureMin = 0.0f;
inline constexpr float kHubTemperatureMax = 2.0f;
inline constexpr float kHubTopPMin = 0.01f;
inline constexpr float kHubTopPMax = 1.0f;
inline constexpr int kHubTopKMin = 0; // 0 = top-k disabled (llama.cpp)
inline constexpr int kHubTopKMax = 1000;
inline constexpr float kHubRepPenaltyMin = 0.5f;
inline constexpr float kHubRepPenaltyMax = 2.0f;

// Trimmed decimal ("0.6", "1", " 1.05 ") -> clamped float. false for empty /
// non-numeric / partially numeric text ("1x", "0.6.1"). The C locale is the
// process default (the app never calls setlocale), so '.' is the separator.
bool ParseHubFloat(std::string_view text, float min_value, float max_value,
                   float& out);
// Trimmed integer -> clamped int, same refusal rules.
bool ParseHubInt(std::string_view text, int min_value, int max_value, int& out);
// "0.6" / "1" / "1.05" - three decimals, trailing zeros and a trailing '.'
// stripped, so a value the user typed survives the round trip byte-exactly
// whenever it has <= 3 decimals.
std::wstring FormatHubFloat(float value);

// One hotkey row. `raw` is exactly what the user typed; `canonical` is the
// KeyboardHook::ParseHotkey-normalized display form ("Ctrl+F9", modifier order
// Ctrl+Shift+Alt+Win - HotkeySpecToDisplayString), and `valid` is the parser's
// verdict. The dialog only ever persists `canonical`, so the config can never
// carry a spelling the hook would refuse.
struct HubHotkeyField {
    std::string raw;
    std::string canonical;
    bool valid = false;
};

// Parses one typed combo through the EXISTING parser (KeyboardHook::ParseHotkey
// / HotkeySpecToDisplayString in hook.hpp) - no second vocabulary.
HubHotkeyField MakeHubHotkeyField(std::string_view raw);

// The hub's editable state: control values as plain data, seeded from the live
// AppConfig so [취소] has nothing to undo.
struct HubDraft {
    // Runtime toggles.
    bool auto_send = false;
    bool back_translation = false;
    bool sound = true;
    bool badge_visible = true;
    bool start_with_windows = false;
    bool drag_to_translate = true;
    bool cloud_fallback = false;
    // Display-only: the persisted UI-language token (the picker callback is the
    // only writer, so the hub never persists it itself).
    std::string ui_language = "auto";
    // Raw persisted theme token; folded through AppConfig::NormalizeUiTheme.
    std::string ui_theme = "fluent";
    // Four hotkey rows, in HubHotkeySlot order.
    HubHotkeyField hotkey[4];
    // REQ-060 load policy (combo index).
    int warmup_combo_index = 0;
    // Advanced page.
    bool diag_log_enabled = false;
    bool diag_log_content = false;
    std::string temperature = "0";
    std::string top_p = "0.6";
    std::string top_k = "20";
    std::string repetition_penalty = "1";
};

// Seeds the draft from the live config (GUI thread: direct reads, the same
// discipline the tray/menus use) plus the injected runtime state.
HubDraft MakeHubDraft(const AppConfig& config, const HubRuntimeState& runtime);

// The values the hub itself does NOT write: drag_to_translate and
// cloud_fallback_enabled have no locked AppConfig verb (see the file header's
// house contract). The hotkey_* / sampling fields ALSO travel in this struct
// (the fold is the parsed plan), but the caller applies them through the
// locked config verbs (V10F-R1 F-02), never by direct assignment.
struct HubExtraSettings {
    std::string hotkey_toggle;
    std::string hotkey_lang;
    std::string hotkey_mode;
    std::string hotkey_selection_translate;
    float temperature = 0.0f;
    float top_p = 0.6f;
    int top_k = 20;
    float repetition_penalty = 1.0f;
    bool drag_to_translate = true;
    bool cloud_fallback_enabled = false;
};

// Per-field refusal map. `any()` gates [확인].
struct HubFieldErrors {
    bool hotkey_toggle = false;
    bool hotkey_lang = false;
    bool hotkey_mode = false;
    bool hotkey_selection = false;
    bool temperature = false;
    bool top_p = false;
    bool top_k = false;
    bool repetition_penalty = false;

    bool any() const {
        return hotkey_toggle || hotkey_lang || hotkey_mode || hotkey_selection ||
               temperature || top_p || top_k || repetition_penalty;
    }
};

// What [확인] will write. `ok == false` means at least one edit is unparsable:
// nothing is applied, the dialog stays open with the focus on the first bad
// field. `any_change` is the pure difference against the seed, so an untouched
// hub never writes the file.
struct HubApplyPlan {
    bool ok = false;
    bool any_change = false;
    std::string ui_theme; // normalized token
    bool auto_send = false;
    bool back_translation = false;
    bool sound = true;
    bool diag_log_enabled = false;
    bool diag_log_content = false;
    bool warmup = false;
    HubExtraSettings extra;
    HubFieldErrors errors;
};

// The save plan fold: parse + clamp + normalize + diff against `seed`.
HubApplyPlan FoldHubDraft(const HubDraft& edited, const HubDraft& seed);

// What actually changed, handed to the caller's after_apply callback AFTER the
// hub persisted, so main.cpp re-pushes exactly the surfaces that moved (theme ->
// ui::SetActiveTheme + tray/tooltip/about repaint; engine_host -> the runtime
// engine warmup re-evaluation).
struct HubApplyResult {
    bool any_change = false;
    bool theme_changed = false;
    bool engine_host_changed = false;
    bool ui_language_changed = false; // reserved: the picker persists itself
};

// ---- pure display composition (no Win32; the suite pins the wording) ----

// "SRC → TGT". Both halves are the persisted stored names; I18n::
// GetLanguageDisplayName resolves registry codes and passes unknown tokens
// through verbatim, so a user-language stored name renders as typed.
std::wstring ComposePairSummary(std::string_view source, std::string_view target);

// The registered user languages as one line: the first four stored names joined
// by ", ", then " …" when more remain. Empty registry -> empty string (the hub
// then shows the add hint instead).
std::wstring ComposeUserLanguageSummary(const std::vector<UserLanguage>& entries);

// The OpenAI declared-language count line, or "" when nothing is declared
// (empty = the full generous set, so there is nothing to say).
std::wstring ComposeDeclaredLanguagesSummary(
    const std::vector<std::string>& declared);

// The UI-language VALUE cell: "auto" (any case) -> MenuUiLanguageAuto, else the
// canonical code uppercased ("ko" -> "KO"). The picker persists canonical
// lowercase codes (PlanUiLocaleChange), so this is display-only.
std::wstring DescribeUiLanguageValue(std::string_view persisted);

// ---- the user-language CRUD guard shared with the manager dialog ----------

// The manager's [삭제] guard (and the hub's, if it ever grows one): resolves the
// selected row to the index AppConfig::RemoveUserLanguage takes, and refuses
// while the forward-compat carrier is armed (PR9-12: every write verb refuses,
// so an offered delete would silently do nothing) or when the name is gone.
struct UserLanguageRemovalPlan {
    bool enabled = false;
    std::size_t index = 0;
    bool carrier_blocks = false;
    bool not_found = false;
};
UserLanguageRemovalPlan PlanUserLanguageRemoval(
    const std::vector<UserLanguage>& entries,
    std::string_view selected_stored_name,
    bool writes_allowed);

} // namespace settings_hub

// ---------------------------------------------------------------------------
// The actions main.cpp injects (the g_open_* global callback pattern, one slot
// per surface). EVERY callback is optional: an unwired callback disables the
// control it serves, so a half-wired build shows a greyed row instead of a
// button that silently does nothing.
// ---------------------------------------------------------------------------
struct HubActions {
    // The eight briefed surfaces.
    std::function<void()> open_openai;
    std::function<void()> open_gguf_manager;
    std::function<void()> open_model_download;
    std::function<void()> open_user_language;
    std::function<void()> open_user_language_manager;
    std::function<void()> open_cheat_sheet;
    std::function<void()> check_updates;
    std::function<void()> reset_defaults;
    // Additive (UX-2 wiring contract): the shared UI-language picker, the two
    // language-pair coordinators, the live badge visibility flip, the
    // no-locked-verb-field apply (drag / cloud fallback; the hotkey / sampling
    // fields ride the struct but are applied through the locked config verbs,
    // V10F-R1 F-02), and the post-save surface re-push.
    std::function<void()> open_ui_language_picker;
    std::function<void()> change_typing_pair;
    std::function<void()> change_drag_pair;
    std::function<void(bool)> set_badge_visible;
    std::function<void(const settings_hub::HubExtraSettings&)> apply_extra;
    std::function<void(const settings_hub::HubApplyResult&)> after_apply;
};

// Installs the action set. GUI thread, before OpenSettingsHub (the
// g_open_gguf_model_manager contract: set once at startup, read-only after).
// The hub deep-copies the set, so a caller may let its local copy die.
void SetHubActions(HubActions actions);

// ---- SECTION B: the window ----------------------------------------------

// Production item-set emission, split out of OpenSettingsHub so the unit suite
// byte-walks the exact bytes (BuildOpenAiTemplate precedent). Begin() must
// already have been called with kHubItemCount and the kHubWidthDlu x
// kHubHeightDlu geometry.
void BuildSettingsHubTemplate(TemplateBuilder& tb);

// Opens (or re-focuses) the single settings-hub window. `config` is read to
// seed the draft and written ONLY through its locked verbs / atomic toggles on
// [확인] (then SaveToFile). `runtime` supplies the two values AppConfig does not
// carry. `parent` is the owner HWND (may be null). Returns nothing: the hub is
// modeless, and every persisted edit reaches the caller's after_apply callback.
void OpenSettingsHub(AppConfig& config,
                     const HubRuntimeState& runtime = HubRuntimeState{},
                     HWND parent = nullptr);

} // namespace emebalachat
