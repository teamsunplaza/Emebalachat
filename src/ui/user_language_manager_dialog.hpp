#pragma once

// ---------------------------------------------------------------------------
// 261116_0003 P9b-3 U4b (language-menu redesign, plan §5 P9 + D6/PR9-06): the
// "내 언어" MANAGER dialog - the surface where the user sees each registered
// user language's cached capability verdict, the shape-only evidence behind it,
// re-runs the check, and overrides it.
//
// WHY A DIALOG (not menu rows): the tray band is full (2609-2864) and the free
// block 2865-2899 holds 35 ids - not enough for per-entry retest/override
// actions. The per-entry actions therefore live here; the menu rows carry only
// the badge (the WIRING unit's scope).
//
// A plain Win32 modal form (no D2D - the openai_settings_window /
// engine_host_settings_window / user_language_dialog contract): TemplateBuilder
// builds the in-memory DLGTEMPLATE, DialogBoxIndirectParamW runs it, and every
// state change is pushed into the controls through GetDlgItem/EnableWindow.
//
// WHAT IT RENDERS, per entry (the row fold below):
//   * the STORED label (UserLanguageStoredName) - never the raw native bytes
//     (the bidi-control lesson of P9b-3 U2's refusal line: a hand-typed
//     override character must not be able to reorder a list row),
//   * the probe badge for the caller's (engine, model) key from
//     DisplayUserLanguageProbeStatus: "확인 중" / "✓ 지원" / "✗ 미지원 의심",
//     plus the UserLangOverrideActive marker when the verdict is the user's own
//     (D6 "the user is the final authority"),
//   * the cached shape-only evidence string under UserLangProbeEvidenceLabel,
//     VERBATIM (the P9a evidence grammar "nonempty=1 differs=1 ..." is compared
//     across runs, so it is never re-worded here).
//
// WHAT IT APPLIES: every change goes through U1's LOCKED verbs on the GUI
// thread (AppConfig::RecordUserLanguageProbeOverrideFor /
// ClearUserLanguageProbeOverrideFor) or through U3's runner
// (UserLanguageProbeService::RequestRetest). The dialog performs NO file I/O:
// kChanged tells the caller that a verb applied, and the caller decides when to
// persist (U2's boundary, verbatim).
//
// CHECKING IS RUNTIME-ONLY: the badge fold takes the in-flight flag from
// U3's IsInFlight; the persisted slot never holds Checking (P9a reader: a
// stored "checking" reads back as Unknown), so a reload never opens the dialog
// showing a stuck check.
//
// NO-PROBE ENGINE CONTEXT: when the caller's engine does not judge user
// languages by evidence (Google is publisher-declared, the local engine and
// Auto have no probe row - UserLanguageEngineId returns ""), the probe column
// stays EMPTY (there is no landed "publisher-declared" copy - recorded as a
// copy gap in the U4b report) and every action button is disabled.
//
// THREE SEAMS keep the unit headlessly testable (AGENTS.md §10; the
// p9b2/p9b3 manual-QA split):
//
//   SECTION A - namespace user_language_manager_dialog: the PURE folds (row /
//     rows / verb result / outcome). No window, no message loop, no global UI
//     state. Everything tests/p9b3_user_language_dialog_tests.inc exercises
//     lives here.
//
//   SECTION B - BuildUserLanguageManagerTemplate + ShowUserLanguageManagerDialogEx:
//     the only window-creating path. A test never reaches it.
//
// THE RECORDED-CALLBACK BOUNDARY (U3 handoff §4 read literally vs. the actual
// API): UserLanguageProbeService::SetRecordedCallback is a SINGLE-SLOT setter
// (user_language_probe.cpp: recorded_cb_ = std::move(callback)), and the slot's
// production owner is main.cpp's persist lambda. A modal dialog that registered
// its own refresh there would clobber that owner for the rest of the process
// (verdicts recorded afterwards would never be saved). This dialog therefore
// does NOT touch the slot: while a probe it launched is in flight it polls the
// runner on a WM_TIMER (IsInFlight + DrainOutcomes on the GUI thread), so the
// verdict lands through U3's marshal, the owner's callback still runs, and the
// list refreshes in place. Recorded in the U4b report as the one deviation from
// the brief's wording, with the assignment site as evidence.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include "../config.hpp"
#include "../i18n.hpp"                // StringId: the badge copy ids (P9b-1)
#include "../user_language_probe.hpp" // U3 runner: RequestRetest / IsInFlight / drain
#include "openai_settings_window.hpp" // TemplateBuilder (the proven DLGTEMPLATE contract)

namespace emebalachat {

// Outcome of one manager-dialog run.
//   kClosed  - no verb applied (or the user closed/cancelled); the config is
//              byte-identical to what the caller passed in.
//   kChanged - at least one override verb APPLIED in memory. The caller
//              persists; this dialog never writes the file. A retest launch is
//              NOT a change (U3's recorded callback owns the persist for a
//              verdict).
enum class UserLanguageManagerOutcome { kClosed = 0, kChanged = 1 };

namespace user_language_manager_dialog {

// ---- dialog geometry / control ids (template-local; the 4700 band is free:
// openai 100s, palette 4001-4005, gguf manager 4100s, add 4200s, HF-add 4300s,
// engine-host 4400s, add-language dialog 4600s). The WM_TIMER id sits just past
// the controls so a control id and a timer id can never alias. ----
enum : WORD {
    kIdList = 4700, // LISTBOX of stored labels + inline badges
    kIdBadge,       // STATIC: the selected row's badge cell (full text)
    kIdEvidenceLabel, // STATIC: UserLangProbeEvidenceLabel
    kIdEvidence,      // STATIC: the cached shape-only evidence string, verbatim
    kIdRetest,        // BUTTON: UserLangProbeRetest -> U3 RequestRetest
    kIdOverrideSupported,   // BUTTON: UserLangOverrideSupported
    kIdOverrideUnsupported, // BUTTON: UserLangOverrideUnsupported
    kIdOverrideMarker, // BS_AUTOCHECKBOX: UserLangOverrideActive (the marker AND
                       // the clear-override affordance - see the header note)
    kIdDisclaimer,     // STATIC: UserLangSupportNotQuality (D6 mandated line)
};
// The WM_TIMER id used by the in-flight poll (NOT a control).
inline constexpr UINT_PTR kInFlightTimerId = 4710;
inline constexpr UINT kInFlightPollMs = 120;

inline constexpr short kDialogWidthDlu = 420;
inline constexpr short kDialogHeightDlu = 300;
// 8 app controls (list / badge / evidence label / evidence text / the three
// action buttons / the marker checkbox / the disclaimer) + Close.
inline constexpr WORD kDialogItemCount = 10;

// ---- badge composition landmarks (public so the suite composes the expected
// strings with the production pieces instead of a copy of them) ----
inline constexpr wchar_t kBadgeSupportedPrefix[] = L"\u2713 ";   // "✓ "
inline constexpr wchar_t kBadgeUnsupportedPrefix[] = L"\u2717 "; // "✗ "
// Separates the verdict copy from the override marker ("지원 · 직접 설정함").
inline constexpr wchar_t kBadgeMarkerSeparator[] = L" \u00B7 ";
// Separates the stored label from the badge inside one LISTBOX row.
inline constexpr wchar_t kListBadgeSeparator[] = L" - ";
// Separates UserLangProbeEvidenceLabel from the cached evidence string.
inline constexpr wchar_t kEvidenceSeparator[] = L": ";

// ---- SECTION A: the pure folds -------------------------------------------

// One rendered row: everything the dialog needs to draw one entry and to gate
// its per-entry actions. Every string is derived from the STORED name (never
// the raw native bytes) or from the cached probe slot (never the entry text).
struct UserLanguageManagerRow {
    // UserLanguageStoredName(entry): the locked verbs' key AND the menu
    // dispatch payload. "" only for a schema-degenerate entry (empty names).
    std::string stored_name;
    // stored_name as UTF-16 - the row label the LISTBOX shows.
    std::wstring label;
    // The badge cell text: "확인 중" / "✓ 지원" / "✗ 미지원 의심" (+ the marker
    // suffix when an override is active). EMPTY when the engine does not judge
    // by evidence and when nothing has been judged yet (D6: Unknown draws no
    // badge).
    std::wstring badge_text;
    // The PRIMARY copy id behind badge_text (UserLangProbeChecking /
    // Supported / Unsupported). Meaningful only when badge_shown.
    StringId badge_id = StringId::UserLangProbeChecking;
    bool badge_shown = false;
    // An override is set for this (engine, model) key: the displayed verdict is
    // the user's, and ClearUserLanguageProbeOverrideFor is meaningful.
    bool override_active = false;
    // A probe for this key is in flight right now (U3 IsInFlight). runtime-only.
    bool in_flight = false;
    // The caller's engine judges user languages by evidence
    // (UserLanguageProbeEngineJudgedByEvidence: OpenAI-compatible / user-.gguf).
    bool engine_judges = false;
    // The cached shape-only evidence string, VERBATIM ("" = none cached).
    std::wstring evidence;
    // UserLangProbeEvidenceLabel + ": " + evidence ("" = nothing to show).
    std::wstring evidence_line;
    // The LISTBOX item text: label, plus " - badge" when a badge is shown.
    std::wstring list_text;
    // Per-action enable state (the dialog applies these verbatim).
    bool retest_enabled = false;
    bool override_supported_enabled = false;
    bool override_unsupported_enabled = false;
    bool clear_override_enabled = false;
};

// The row fold: one entry + the caller's (engine, model) context + the
// runtime in-flight flag + the write gate -> the rendered row.
//
//   * `model` is the UTF-8 model half of the probe-cache key (the OpenAI model
//     string or the user-.gguf registry id); it must be the SAME value the
//     runner used, or the row addresses a different slot.
//   * `in_flight` comes from UserLanguageProbeService::IsInFlight for this
//     entry's stored name and key; it is the ONLY source of the checking badge
//     (the persisted slot never holds Checking).
//   * `writes_allowed` is !AppConfig::HasUserLanguagesForwardCompatCarrier():
//     while a newer-schema block is preserved every write verb refuses, so the
//     dialog must not offer the actions (PR9-12 carrier rule, U2's precedent).
UserLanguageManagerRow FoldUserLanguageManagerRow(const UserLanguage& entry,
                                                  LanguageEngine engine,
                                                  std::string_view model,
                                                  bool in_flight,
                                                  bool writes_allowed);

// The whole list for one refresh: a snapshot of config.user_languages (read on
// the GUI thread, config.hpp:999-1003) + the runner's in-flight set. A null
// service reads every row as not in flight (the headless/tests shape).
std::vector<UserLanguageManagerRow> FoldUserLanguageManagerRows(
    const std::vector<UserLanguage>& entries,
    LanguageEngine engine,
    std::string_view model,
    const UserLanguageProbeService* service,
    bool writes_allowed);

// The model half of the key this dialog addresses: the caller's `model` when it
// supplied one, else the config's implied model (UserLanguageProbeModelFor - the
// OpenAI block's model or the user-.gguf registry id) so a bare call still finds
// the slots the runner writes. "" stays "" for an engine with no model half.
std::string ResolveUserLanguageManagerModel(LanguageEngine engine,
                                            const AppConfig& config,
                                            std::wstring_view model);

// One verb result -> what the dialog does with it.
//   Applied -> changed (the caller must persist)
//   anything else (carrier / not found / missing key / invalid status / ...)
//   -> refused, nothing changed. The dialog keeps itself open in both cases.
struct UserLanguageManagerActionResult {
    bool changed = false;
    bool refused = false;
};
UserLanguageManagerActionResult FoldUserLanguageManagerVerbResult(
    UserLanguageWriteResult result);

// A retest launch is NEVER a config change: U3 records the verdict later,
// through its own recorded callback (the slot's owner persists). A refused
// launch (no service, unsupported engine, unknown entry, in-flight key) is
// reported so the caller can log it shape-only.
UserLanguageManagerActionResult FoldUserLanguageManagerRetest(bool launched);

// The dialog outcome fold: any applied verb at any point -> kChanged, else
// kClosed (a cancelled dialog that applied nothing).
UserLanguageManagerOutcome FoldUserLanguageManagerOutcome(bool any_applied);

// ---- SECTION B: the window ----------------------------------------------

// Production item-set emission, split out of ShowUserLanguageManagerDialogEx so
// the unit suite byte-walks the exact bytes handed to DialogBoxIndirectParamW
// (BuildOpenAiTemplate / BuildEngineHostSettingsTemplate /
// BuildUserLanguageTemplate precedent). Begin() must already have been called
// with kDialogItemCount and the kDialogWidthDlu x kDialogHeightDlu geometry.
void BuildUserLanguageManagerTemplate(TemplateBuilder& tb);

// Shows the modal manager dialog.
//   parent        - owner HWND (may be null).
//   config        - read (the registry + the carrier flag) and written IN MEMORY
//                   through U1's locked verbs. No file I/O.
//   engine        - the translation engine the user is judging against; the
//                   probe-cache key half + the evidence gate derive from it.
//   model         - the UTF-8 model half of the key as UTF-16. Production must
//                   pass UserLanguageProbeModelFor(engine, config) so the rows
//                   address the slots the runner writes; an empty value falls
//                   back to that same helper (see ResolveUserLanguageManagerModel).
//   probe_service - the U3 runner (RequestRetest + IsInFlight + drain). Pass the
//                   caller's service instance; nullptr disables 다시 시험 (the
//                   badge/override verbs still work).
UserLanguageManagerOutcome ShowUserLanguageManagerDialogEx(
    HWND parent,
    AppConfig& config,
    LanguageEngine engine,
    const std::wstring& model,
    UserLanguageProbeService* probe_service = nullptr);

} // namespace user_language_manager_dialog
} // namespace emebalachat
