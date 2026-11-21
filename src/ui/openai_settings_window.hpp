#pragma once

// REQ-045 P4-3 (design §3b, item 3b): the OpenAI Compatible engine settings
// dialog. A plain Win32 modal dialog (no D2D — it is a functional form, not a
// branded card) opened from the tray engine submenu. Captures:
//   * Base URL   (https forced; http requires an explicit consent confirmation
//                that persists http_consent_given)
//   * API Key    (ES_PASSWORD edit; stored DPAPI-protected, never cleartext;
//                masked read-back "abcdef***" after save)
//   * Model      (editable combo — "Fetch model list" fetches the model list
//                ASYNCHRONOUSLY on a background thread (REQ-052: the
//                synchronous 10 s WinHTTP call used to freeze the whole
//                dialog on the GUI thread) and the completion handler fills
//                the combo; when the fetch fails or returns empty the user
//                types a model id directly, per the user requirement)
// Save validates, DPAPI-protects the key, writes the openai{} block through
// the AppConfig the caller passes, and persists via SaveToFile.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

#include "../config.hpp" // 261122_0001 B-U4: OpenAiProfile (the profile library fold)
#include "../openai_compatible_client.hpp"

namespace emebalachat {

// REQ-046 P4-3 (Tech Gate 필수-5): the in-memory DLGTEMPLATE builder is a
// public symbol so the unit suite can instantiate it directly and pin the
// template style bits (WS_VISIBLE/DS_CENTER/DS_SETFOREGROUND) WITHOUT entering
// the modal DialogBoxIndirectParamW loop (which would block the headless test
// runner forever). Declaration lives here; implementation stays in the .cpp.
// The buffer layout follows the Win32 dialog-template contract (4-byte
// alignment of DLGTEMPLATE + each DLGITEMTEMPLATE).
class TemplateBuilder {
public:
    void Begin(std::wstring_view title, short w, short h, WORD itemCount);
    void AddItem(DWORD style, short x, short y, short cx, short cy,
                 WORD id, WORD clsAtom, std::wstring_view text);
    const DLGTEMPLATE* Get() const;
    size_t size() const; // REQ-048 P2: whole-buffer byte count (test byte-walk)
private:
    void EmitWord(WORD w);
    void EmitDword(DWORD d);
    void EmitStr(std::wstring_view s);
    std::vector<BYTE> buf_;
    size_t dtpl_ = 0;
};

// Shows the modal settings dialog. `parent` is the owner HWND (may be null).
// `cfg` is the current persisted openai block (read to pre-fill); on OK it is
// replaced with the edited values AND the key is DPAPI-protected in-place.
// Returns true when the user saved (caller persists via AppConfig::SaveToFile).
// REQ-051 (Symptom D): a thin compatibility wrapper over
// ShowOpenAiSettingsDialogEx — it reports ONLY the kSaved outcome as true, so
// pre-REQ-051 callers (main.cpp kMsgOpenOpenAiSettings) keep their exact
// save/switch contract; the kDeleted outcome arrives as false (the delete
// path persists itself and must never trigger the caller's engine switch).
bool ShowOpenAiSettingsDialog(HWND parent, OpenAiConfig& cfg);

// REQ-051 (Symptom D): tri-state dialog outcome. kCancelled == the dialog was
// dismissed without applying anything; kSaved == IDOK (caller persists + owns
// any engine switch, REQ-047 D3 contract); kDeleted == the [삭제] button
// cleared the five OpenAI settings fields and PERSISTED that clear itself —
// the caller must NOT switch the engine and only needs to refresh any UI that
// renders the openai block (the in-memory AppConfig::openai is reconciled by
// the caller's own save path; see the REQ-051 handoff).
enum class OpenAiSettingsOutcome { kCancelled, kSaved, kDeleted };
OpenAiSettingsOutcome ShowOpenAiSettingsDialogEx(HWND parent, OpenAiConfig& cfg);

// 261116_0003 P9c-1c: the declaration checklist's fold, carried OUT of the
// dialog so the OWNING AppConfig instance applies it. The dialog owns only the
// openai block, and its caller (main.cpp ControllerWndProc) cannot name the
// app's AppConfig, so the folded set travels back through this struct instead
// of being persisted by the dialog through a second AppConfig instance (the
// P9c-1b handoff gap: that instance's save was reverted by the owning
// instance's own save one line later).
//   * edited             - IDOK folded the checklist. false on Cancel and on
//                          the [삭제] button, where no selection was read, so
//                          the caller must NOT adopt (an untouched empty set
//                          would otherwise clear a live declaration).
//   * declared_languages - the folded set in canonical registry order (the
//                          fold is the canonicalizer); empty = the user
//                          declared nothing, i.e. ADOPT AS "CLEAR".
struct OpenAiDeclaredLanguagesFold {
    bool edited = false;
    std::vector<std::string> declared_languages;
};

// 261116_0003 P9c-1c: the full entry - the same dialog, plus the declaration
// fold written into `declaration`. The dialog performs NO config file I/O for
// the declaration any more; the caller applies the set on the LIVE AppConfig
// through AppConfig::SetOpenAiDeclaredLanguages BEFORE its own save (that save
// is what emits the array, and an empty set emits no key at all). The
// two-argument overload above stays for callers that only need the outcome
// (they fold the checklist but cannot persist it).
OpenAiSettingsOutcome ShowOpenAiSettingsDialogEx(
    HWND parent, OpenAiConfig& cfg, OpenAiDeclaredLanguagesFold& declaration);

// 261122_0001 B-U4 (design-pick items 4 + 9): the OpenAI profile LIBRARY's
// fold, the second set of edits this dialog may stage. Same single-writer
// principle as the declaration fold above — the dialog mutates only its own
// WORKING COPY (seeded from `profiles` on entry) and never performs config file
// I/O for profiles; the caller (main.cpp, U5) persists the returned library
// through its own SaveToFile.
//   * In:  `profiles.profiles` is the library the dialog opens with, so the
//          profile picker can list it. The dialog deep-copies it; the caller's
//          vector is NOT modified on entry (a cancelled dialog must leave the
//          caller's library exactly as it was).
//   * Out: `edited == true` (IDOK only) means `profiles` now holds the library
//          the user staged with the [저장]/[삭제] buttons — adopt it wholesale.
//          `edited == false` (Cancel, [설정 삭제], a failed dialog creation)
//          means "nothing to adopt": the vector is left as it came in and the
//          caller keeps its stored library. Note the [설정 삭제] (REQ-051
//          kDeleted) path deliberately reports false: that button persists its
//          own clear immediately, and the staged profile edits die with it.
struct OpenAiProfilesFold {
    bool edited = false;
    std::vector<OpenAiProfile> profiles;
};

// The library-aware entry: everything the declaration overload does, plus the
// profile fold. The profile row ([저장]/[삭제]) acts on the working copy only;
// the active `cfg` output is still the on-screen fields (REQ-051 semantics
// unchanged).
OpenAiSettingsOutcome ShowOpenAiSettingsDialogEx(
    HWND parent, OpenAiConfig& cfg, OpenAiDeclaredLanguagesFold& declaration,
    OpenAiProfilesFold& profiles);

// REQ-051 (Symptom C): the pick-or-insert plan for the model combo refill.
// `items` are the freshly fetched model ids with the combo indices they were
// added at (index = the CB_ADDSTRING return); `target` is the combo's current
// text — the saved model at dialog init, or whatever the user typed since.
// Exact, case-sensitive match (model ids are case-sensitive). Result:
//   * target found      -> select that index, no insert
//   * target non-empty,
//     absent            -> insert_target = true; select_index is the index the
//                          appended item WILL occupy (last index + 1, or 0)
//   * target empty      -> {-1, false}: leave the combo unselected (never
//                          fabricate a selection the user never made)
struct OpenAiComboSelection {
    int select_index;    // CB_SETCURSEL target; -1 = leave the combo unselected
    bool insert_target;  // CB_ADDSTRING(target) first, then select select_index
};
OpenAiComboSelection PlanOpenAiComboSelection(
    const std::vector<std::pair<int, std::string>>& items,
    const std::string& target);

// REQ-051 (Symptom D): clears EXACTLY the five persisted OpenAI settings
// fields — base_url, model, api_key_dpapi, api_key_sha256 (the DPAPI/digest
// pair is wiped TOGETHER so a half-cleared pair can never reach the refuse-
// forever WithUnprotectedKey state), http_consent_given — so a later save
// persists the cleared state. OpenAiConfig carries no engine field, so the
// engine selection is untouched by construction. Pure/side-effect-free apart
// from the referenced struct; unit-tested headlessly.
void ClearOpenAiSettings(OpenAiConfig& cfg);

// 261122_0001 fix-A2 (R1-01; security-reviewer-report-verify §"Minimal correct
// fix" item 1): the credential-scoped library prune both REQ-051 [설정 삭제]
// persist sites need. Erases from `profiles` every entry that carries the
// credential of the block being deleted and returns how many were removed (0 =
// nothing matched; the caller then leaves the library — and its "fold touched"
// record — alone). Identity rule, in that order:
//   * deleted block WITH a key pair: match on the pair, DIGEST FIRST
//     (api_key_sha256 is the stable identity — a re-protect of the same
//     cleartext yields a different DPAPI blob, so a blob-only match would miss
//     the re-saved copy) and on the DPAPI blob second (a half pair whose digest
//     was wiped still identifies its entry);
//   * deleted block WITHOUT a key pair: fall back to the migration
//     synthesizer's identity — base_url ASCII case-insensitive + model
//     case-sensitive, the PlanOpenAiProfileUpsert storage identity — because
//     that is exactly the entry config.cpp synthesizes when the
//     "openai_profiles" key is absent. The fallback only matches a KEYLESS
//     entry: a keyed entry at the same endpoint is a credential the user did
//     not ask to destroy.
// A block with no identity at all (no key, no base_url, no model) matches
// nothing. Pure: no I/O, no DPAPI, no threads. main.cpp consumes the same
// helper from its kDeleted reconcile (the second persist site).
std::size_t EraseOpenAiProfileMatchingKey(std::vector<OpenAiProfile>& profiles,
                                          const OpenAiConfig& deleted);

// REQ-048 P2: emits the production OpenAI-settings item set into `tb` (Begin
// must already have been called with the matching item count). Split out of
// ShowOpenAiSettingsDialog so the unit suite can serialize the exact
// production template and byte-walk it against the Windows DLGTEMPLATE/DLG-
// ITEMTEMPLATE parser contract without entering the modal loop.
void BuildOpenAiTemplate(TemplateBuilder& tb);

// REQ-050 (corrupt-pair recovery): headless integrity check of a persisted
// OpenAiConfig key pair — unprotects the DPAPI blob, re-hashes the cleartext,
// and compares against the persisted api_key_sha256. Returns true only when
// unprotect succeeds AND the digests match; the cleartext buffer is zeroed
// before return and never logged. The dialog proc calls this at WM_INITDIALOG
// and clears both key fields on false (the pre-REQ-050 builds persisted
// SHA-256 of the protect-scrubbed zero buffer, a pair WithUnprotectedKey
// refuses forever). Side-effect free, so the unit suite can drive it directly.
bool OpenAiKeyPairIntegrityOk(const OpenAiConfig& cfg);

} // namespace emebalachat
