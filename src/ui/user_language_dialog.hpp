#pragma once

// ---------------------------------------------------------------------------
// 261116_0003 P9b-3 U2 (language-menu redesign, plan §5 P9 + D5/PR9-11): the
// "내 언어 추가" (add my language) dialog. A plain Win32 modal form (no D2D —
// functional form, the openai_settings_window / engine_host_settings_window
// contract) that captures TWO user-typed fields and SHOWS the derived ones:
//
//   * Native name    - mandatory. The only field the schema requires.
//   * English name   - optional (the registry back-fills it when the code
//                      resolves and the user left it empty, PR9-07).
//
//   * Language code  - DERIVED and read-only: the D5 ladder
//                      (alias matrix -> installed locale names -> no code).
//   * Script         - DERIVED and read-only: InferUserLanguageScript over the
//                      cleaned name, falling back to the resolved code's script
//                      (both rules live in MakeUserLanguage; the dialog never
//                      re-implements them).
//   * No-code notice - UserLangNoCodeNotice when the ladder resolved nothing
//                      (D5: "no code = OpenAI/user-.gguf only").
//   * Refusal line   - UserLangNameReserved with the "{n}" token substituted
//                      once by the cleaned typed name (PR9-11).
//
// The dialog APPLIES the add IN MEMORY through AppConfig::AddUserLanguage
// (U1's locked mutator) and performs NO file I/O: the caller decides whether
// and when to persist (SaveToFile). The mutator, not the dialog, is the
// authority on the name verdict — it re-validates inside the lock — and the
// outcome fold below maps its result to this module's disposition, so
// "Applied" is the ONLY path that closes the dialog as kAdded.
//
// Two seams keep the unit headlessly testable (AGENTS.md §10: no window is
// created by the suite, the same split P9b-2 recorded):
//
//   SECTION A — namespace user_language_dialog: the PURE validation /
//     derivation fold. Zero Win32 calls beyond the shared TemplateBuilder, no
//     message loop, no global UI state. Everything
//     tests/p9b3_user_language_dialog_tests.inc exercises lives here.
//
//   SECTION B — ShowUserLanguageDialogEx: the only window-creating entry. A
//     test never reaches it, and no build step runs it.
//
// SECURITY (PR9-03's recorded P9b obligation): every DERIVED/preview row
// renders the CLEANED form of the typed text — the same
// sanitize-then-cap choke point the storage path applies (config.cpp
// SanitizeUserLanguageField/CleanUserLanguageField, PR9-03 + PR10-08 strip
// set). The dialog reaches it through the PUBLIC builder MakeUserLanguage
// rather than a new declaration (the sanitizer itself is an anonymous-namespace
// helper in config.cpp), so the strip set is never duplicated. The typed text
// itself is NEVER logged: this is user content (the picker's typed-query
// precedent, language_search_palette.cpp:296-303) and its only diagnostics are
// shape-only reason codes.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include "../config.hpp"
#include "openai_settings_window.hpp" // TemplateBuilder (the proven DLGTEMPLATE contract)

namespace emebalachat {

// Outcome of one dialog run.
//   kAdded     - the entry is ALREADY in the AppConfig (added in memory through
//                the locked mutator). The caller persists; this dialog never
//                writes the file.
//   kCancelled - nothing was added; the config is untouched.
enum class UserLanguageDialogOutcome { kCancelled = 0, kAdded = 1 };

namespace user_language_dialog {

// ---- dialog geometry / control ids (template-local, unrelated to the tray
// menu id bands: the add-language trigger row is the WIRING unit's scope and
// takes ids >= 2865). The 4600s are free (openai 100s, manager 4100s, HF-add
// 4300s, engine-host 4400s, palette 4001-4005 — all dialog-local). ----
enum : WORD {
    kIdNativeLabel = 4600,
    kIdNativeEdit,
    kIdEnglishLabel,
    kIdEnglishEdit,
    kIdCodeLabel,
    kIdCodeValue,
    kIdScriptLabel,
    kIdScriptValue,
    kIdNoCodeNotice,
    kIdRefusal,
    kIdAdd,
};
inline constexpr short kDialogWidthDlu = 260;
inline constexpr short kDialogHeightDlu = 168;
// 10 app controls (2 labels + 2 edits + 2 derived label/value pairs + the
// notice + the refusal line) + Add + Cancel.
inline constexpr WORD kDialogItemCount = 12;

// ---- SECTION A: the pure fold -------------------------------------------

// One candidate entry, derived from the two typed fields. Every string here is
// the CLEANED (strip + trim + byte cap) form, i.e. exactly what the entry would
// carry if added right now:
//   native_name   - cleaned typed native name. ALSO the "{n}" value the refusal
//                   message renders (never the raw typed bytes: a bidi control
//                   must not be able to reorder the refusal line).
//   english_name  - the entry's stored English form. When the user left the
//                   field empty and the code resolved, this is the registry
//                   back-fill (PR9-07) — the EDIT control is never rewritten,
//                   so the user keeps seeing what they typed.
//   code          - resolved registry code; "" = free-form engines only (D5).
//   script_mask   - the script the entry will carry (name inference first, the
//                   resolved code's script second — both rules live in
//                   MakeUserLanguage; this struct only names the result).
//   script_token  - UserLanguageScriptToken(script_mask): "latin",
//                   "hangul+latin", ... ; "" when the mask is 0.
//   no_code       - render the UserLangNoCodeNotice. True only when a name WAS
//                   typed and the ladder resolved nothing, so an empty form
//                   does not open with the notice already showing.
//   verdict       - PR9-11 creation verdict (same fold the mutator applies).
struct UserLanguageCandidate {
    std::string native_name;
    std::string english_name;
    std::string code;
    std::uint32_t script_mask = 0;
    std::string script_token;
    bool no_code = false;
    bool name_present = false;
    UserLanguageNameVerdict verdict = UserLanguageNameVerdict::Accepted;
};

// Derives the candidate from the two typed fields. `existing` is the registry
// the entry would join (AppConfig::user_languages). `locale_source` is the D5
// stage-2 seam, so the suite pins the resolved code without depending on which
// system locales this machine has installed (the 3-arg MakeUserLanguage /
// ResolveUserLanguageCode seam).
UserLanguageCandidate DeriveUserLanguageCandidate(
    std::string_view native_name,
    std::string_view english_name,
    const std::vector<UserLanguage>& existing,
    LocaleLanguageSource locale_source);
// Same, with the live EnumerateSystemLocaleLanguages() seam.
UserLanguageCandidate DeriveUserLanguageCandidate(
    std::string_view native_name,
    std::string_view english_name,
    const std::vector<UserLanguage>& existing);

// Replaces the FIRST occurrence of the "{n}" token in `template_text` with
// `value`, once. The repo-wide token convention (there is no printf/format
// layer anywhere in i18n — CountPhrase precedent, language_search_palette.cpp
// :240-250): a template that lost the token comes back UNCHANGED (the caller
// then falls back to the bare template) and a second token is left verbatim, so
// "substituted exactly once" is a property of this function, not of the
// translation tables.
std::wstring SubstituteNameTokenOnce(std::wstring_view template_text,
                                     std::wstring_view value);

// The inline refusal line for one PR9-11 verdict.
//   Reserve/Duplicate -> refuse = true, message = UserLangNameReserved with
//                        "{n}" replaced once by `cleaned_native_name`.
//   Accepted          -> refuse = false, message empty (nothing to say).
struct UserLanguageRefusal {
    bool refuse = false;
    std::wstring message;
};
UserLanguageRefusal BuildUserLanguageNameRefusal(UserLanguageNameVerdict verdict,
                                                std::string_view cleaned_native_name);

// Why the Add button did not commit. NameTaken is the ONLY block with landed
// copy (UserLangNameReserved); the other three keep the dialog open silently
// (P9b-1 landed no copy for them — recorded in the U2 report as a copy gap).
enum class UserLanguageAddBlock : std::uint8_t {
    None = 0,                  // the write applied
    EmptyName = 1,             // RejectedEmptyName: the Add button is inert
    NameTaken = 2,             // RejectedReservedName / RejectedDuplicateName
    Capacity = 3,              // RejectedCapacity (kMaxUserLanguages)
    ForwardCompatStored = 4,   // RefusedForwardCompatCarrier (edit would be lost)
};

// The outcome fold the dialog proc obeys: close/kAdded ONLY for Applied.
struct UserLanguageAddDisposition {
    UserLanguageDialogOutcome outcome = UserLanguageDialogOutcome::kCancelled;
    bool close_dialog = false;
    UserLanguageAddBlock block = UserLanguageAddBlock::None;
    // Mapped verdict for the refusal copy; Accepted when the block is not a
    // name verdict (the window then renders no message).
    UserLanguageNameVerdict name_verdict = UserLanguageNameVerdict::Accepted;
};
UserLanguageAddDisposition FoldUserLanguageAddResult(UserLanguageWriteResult result);

// ---- SECTION B: the window ----------------------------------------------

// Production item-set emission, split out of ShowUserLanguageDialogEx so the
// unit suite byte-walks the exact bytes the dialog hands to
// DialogBoxIndirectParamW (BuildOpenAiTemplate / BuildEngineHostSettingsTemplate
// precedent). Begin() must already have been called with the matching item
// count (kDialogItemCount) and geometry (kDialogWidthDlu x kDialogHeightDlu).
void BuildUserLanguageTemplate(TemplateBuilder& tb);

// Shows the modal add-language dialog. `parent` is the owner HWND (may be
// null). `config` is read (the existing registry for the PR9-11 duplicate
// check, and the forward-compat carrier for the refuse-to-edit rule) and
// written IN MEMORY through AppConfig::AddUserLanguage on a successful add.
// No file I/O: the caller persists (SaveToFile) and owns any menu refresh.
UserLanguageDialogOutcome ShowUserLanguageDialogEx(HWND parent, AppConfig& config);

} // namespace user_language_dialog
} // namespace emebalachat
