#include "openai_settings_window.hpp"

#include "../config.hpp"      // REQ-051 D: AppConfig read-modify-write for the [삭제] persist
#include "../diag_logger.hpp" // REQ-047 D3 §C.4: control-creation verification logging
#include "../i18n.hpp"
#include "../unicode_utils.hpp"

#include <algorithm>
#include <atomic>
#include <deque>   // R1-9: by-value fetch-result queue (SEC-B5 house transport)
#include <functional> // 261122_0001 B-U6fix: the dropped-width plan's measurer
#include <memory>
#include <mutex>   // R1-9: guards the fetch-result queue
#include <string>
#include <string_view>
#include <thread>
#include <utility> // 261121_0001 U-H: std::move of the reasoning item list
#include <vector>

// REQ-050 (cue banners): EM_SETCUEBANNER (0x1501, ECM_FIRST + 1) ships in the
// Vista+ SDK headers; define it defensively so the cue-banner sends below
// survive an older/minimal windows.h. It only works under the UNICODE build
// (the project compiles /DUNICODE) — which is also why SendMessageW is used.
#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

namespace emebalachat {

// ---- 261116_0003 P9c-1b: the declaration checklist's pure folds -----------
// Defined BEFORE the dialog proc (which calls them). Namespace scope, like
// PlanOpenAiComboSelection below, so the unit suite drives the exact helpers
// the dialog proc calls. They stay out of the header on purpose (TU-scope
// helpers; the suite forward-declares them, which openai_settings_window.hpp
// does not).

// The checklist row set: every registry row carrying kLangOpenAi, in REGISTRY
// order, AUTO excluded (a source-only marker). THE UNFILTERED SET ON PURPOSE:
// the OpenAI engine VIEW is declaration-filtered since P9c-1a, so a checklist
// drawn from that filtered view could never show an unchecked language again
// (the P9c-1a §6 handoff hazard names the exact function never to call here).
std::vector<std::string> OpenAiDeclarationRowCodes() {
    std::vector<std::string> rows;
    for (const LanguageInfo& row : GetSupportedLanguages()) {
        if (row.code == "AUTO") continue;
        if ((row.engine_mask & kLangOpenAi) == 0) continue;
        rows.push_back(row.code);
    }
    return rows;
}

// Declaration codes -> selected row indices (the pre-select half of the
// fold). Unknown codes match no row and select nothing; duplicates in the
// declaration collapse through the per-row scan; the indices follow the ROW
// order. An empty declaration folds to an empty selection, which the OK path
// applies as "not declared" = the documented fail-open.
std::vector<int> OpenAiDeclarationSelection(const std::vector<std::string>& declared,
                                            const std::vector<std::string>& rows) {
    std::vector<int> selected;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        for (const std::string& code : declared) {
            if (code == rows[i]) {
                selected.push_back(static_cast<int>(i));
                break;
            }
        }
    }
    return selected;
}

// Selected row indices -> declaration codes. Invalid and duplicate indices
// drop; the result follows the ROW order, which IS the canonical registry
// order, so the fold hands the locked verb an already-canonical set. An
// empty selection folds to an empty declaration = not declared = the
// documented fail-open (the OpenAI views return to the full set).
std::vector<std::string> OpenAiDeclarationFromSelection(
    const std::vector<int>& selected, const std::vector<std::string>& rows) {
    std::vector<std::string> declared;
    for (const int idx : selected) {
        if (idx < 0 || static_cast<std::size_t>(idx) >= rows.size()) continue;
        const std::string& code = rows[static_cast<std::size_t>(idx)];
        if (std::find(declared.begin(), declared.end(), code) == declared.end()) {
            declared.push_back(code);
        }
    }
    return declared;
}

// ---- 261121_0001 U-H (contracts C2/C4): the reasoning-effort combo plan -----
// Defined BEFORE the dialog proc (which calls it) and at NAMESPACE scope (not
// the anonymous block) so the unit suite drives the exact algorithm the dialog
// applies - the same split as the declaration folds above and
// PlanOpenAiComboSelection below. openai_settings_window.hpp is outside this
// unit's write boundary, so the suite forward-declares it.

// An advertised effort token is server-supplied text that ends up (a) in a
// combo item and (b) in the request body. Cap it and keep it to printable
// ASCII without spaces: a hostile or sloppy /v1/models body can then inject
// neither control characters nor a multi-megabyte item, and the body stays
// unambiguous. Real tokens ("none", "minimal", "low", "medium", "high",
// "xhigh") are far inside this bound.
inline constexpr std::size_t kMaxReasoningEffortTokenChars = 32;

// The reasoning combo's item list (contract C2/C4). Item i IS the persisted
// `reasoning_effort` value; the EMPTY string is the SERVER-DEFAULT item, which
// is why it always occupies index 0 (the default selection) and needs no
// separate flag: C2 defines "" as "omit the key from the request body".
//   * the selected model advertised efforts -> [""] + those efforts,
//     deduplicated in SERVER order (the server's own order is the only ranking
//     information available);
//   * nothing usable advertised -> [""] + the standard fallback list. That
//     fallback is the whole point of the contract: OpenAI and Gemini return no
//     capability metadata at all, and vLLM / llama-server / LiteLLM rarely do,
//     so without it the setting would be unusable on most endpoints.
// A list that sanitizes to nothing (all-garbage metadata) counts as "advertised
// nothing" and takes the fallback, so junk can never leave the user with a
// one-item combo. Never returns an empty vector.
std::vector<std::string> PlanReasoningEffortItems(
    const std::vector<std::string>& advertised_efforts) {
    const auto sanitize = [](std::string_view token) -> std::string {
        std::size_t begin = 0;
        std::size_t end = token.size();
        while (begin < end && (token[begin] == ' ' || token[begin] == '\t')) ++begin;
        while (end > begin && (token[end - 1] == ' ' || token[end - 1] == '\t')) --end;
        const std::string_view trimmed = token.substr(begin, end - begin);
        if (trimmed.empty() || trimmed.size() > kMaxReasoningEffortTokenChars) return {};
        for (const char c : trimmed) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u < 0x21 || u > 0x7E) return {}; // control/space/non-ASCII
        }
        return std::string(trimmed);
    };
    std::vector<std::string> items{std::string{}}; // index 0 = server default (unset)
    for (const std::string& raw : advertised_efforts) {
        std::string token = sanitize(raw);
        if (token.empty()) continue;
        if (std::find(items.begin(), items.end(), token) != items.end()) continue;
        items.push_back(std::move(token));
    }
    if (items.size() == 1) {
        // Nothing usable was advertised: the standard list in the
        // OpenAI-documented order (none < minimal < low < medium < high).
        for (const char* value : {"none", "minimal", "low", "medium", "high"}) {
            items.emplace_back(value);
        }
    }
    return items;
}

// 261121_0001 fix round 1 (F-4): ONE (re)population step of the reasoning combo,
// as a pure function so the suite drives the exact reconciliation the dialog
// runs. The bug it fixes: the dialog's FIRST population happens before any fetch
// has completed, so it can only offer the STANDARD fallback list; a saved
// server-extension value (OpenRouter's "xhigh") is not in that list, and the old
// code overwrote the state with the fallback's server-default item — silently
// losing the user's preference before the catalog that advertises it arrived.
//   * `items`     — a PlanReasoningEffortItems() result (never empty; index 0 is
//                   the server-default item whose value is "").
//   * `value`     — the value the dialog currently carries ("" = server default).
//   * `pending`   — true while `value` came from config and no list has shown it
//                   yet (only possible for a non-empty value outside the
//                   fallback list).
// Writes the index the combo must select, the value the dialog carries after the
// population, and the new pending state. Rules:
//   * a list that carries `value`            -> select it, value unchanged,
//                                              pending cleared (it is shown now);
//   * a list that does not, while pending    -> server-default item selected,
//                                              value KEPT (the user's saved
//                                              choice survives; OK persists it,
//                                              and the request-side 400 retry
//                                              covers a model that rejects it);
//   * a list that does not, otherwise        -> server-default item selected and
//                                              the value becomes "" (the
//                                              documented converge-on-default
//                                              rule for a value the current model
//                                              does not offer).
void PlanOpenAiReasoningComboPopulate(const std::vector<std::string>& items,
                                      const std::string& value,
                                      bool pending,
                                      int& out_select,
                                      std::string& out_effective,
                                      bool& out_pending) {
    if (items.empty()) {
        // Defensive only: PlanReasoningEffortItems never returns an empty list.
        out_select = 0;
        out_effective = (pending && !value.empty()) ? value : std::string();
        out_pending = pending && !value.empty();
        return;
    }
    std::size_t match = 0;
    bool shown = false;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i] == value) {
            match = i;
            shown = true;
            break;
        }
    }
    // Index 0 is the server-default item: matching it is not "the list offers
    // the saved value", it is the absence of one.
    const bool offers_value = shown && match != 0;
    out_select = static_cast<int>(match);
    if (!offers_value && pending && !value.empty()) {
        out_effective = value;
        out_pending = true;
        return;
    }
    out_effective = items[match];
    out_pending = false;
}

// ---- 261122_0001 B-U4 (design-pick item 9): the profile-row behavior seams ---
// Everything in this block runs on the GUI thread inside the dialog proc, and
// every piece that can be decided without an HWND is a PURE function defined
// here at namespace scope (the same split as the declaration folds and the
// reasoning planners above) so tests/openai_profile_tests.inc drives the EXACT
// algorithm the dialog applies. The suite forward-declares these; they stay out
// of openai_settings_window.hpp on purpose (TU-scope helpers, not public API).

// REQ-045/REQ-050: the masked read-back placeholder ("abcdef***") that means
// "the saved pair is still in effect; do not treat this text as a key". Moved
// above the anonymous namespace by B-U4 so the namespace-scope save-path helper
// below shares this ONE definition with the IDOK / fetch-probe call sites (no
// second mask test can drift).
bool IsMaskedPlaceholder(std::string_view keyUtf8) {
    return keyUtf8.size() > 3 && keyUtf8.compare(keyUtf8.size() - 3, 3, "***") == 0;
}

// The reasoning-effort re-seed rule (design item 9, risk R3). Selecting a
// profile hands the form a NEW saved value, and the dialog's think-combo state
// has three parts: the effective value, the combo's item list, and the F-4
// "pending" flag. This helper owns the value it carries and whether that value
// is still awaiting a list that can show it. The rule is exactly the F-4
// WM_INITDIALOG convention (one saved value, one pending arm) applied to a
// mid-dialog change of provenance:
//   * a non-empty saved effort is PENDING (the combo can only show the standard
//     fallback until a catalog arrives, so a server-extension value such as
//     OpenRouter's "xhigh" must survive to the save path);
//   * an empty saved effort is the server default, nothing to preserve.
// RefreshReasoningCombo then reconciles the value against the current list, so
// a standard value is selected immediately and pending clears. Out-parameter
// form (not a struct) so the unit suite can forward-declare and drive it
// without duplicating a type definition across TUs — the same shape as
// PlanOpenAiReasoningComboPopulate above.
void PlanOpenAiReasoningReseed(std::string_view saved_value, std::string& out_value,
                               bool& out_pending) {
    out_value.assign(saved_value);
    out_pending = !out_value.empty();
}

// Display sanitizer for strings that came from user data (a profile name, or
// the base_url/model a profile name is derived from). Reuses the R2-M/G-02
// precedent rather than restating its strip set: SanitizeOpenAiErrorDetail's
// pass 1 drops the invisible bidi/format characters (U+202A-202E, U+2066-2069,
// U+200E/200F, U+061C, U+2060-2064, ZWSP/SHY/CGJ/MVS, U+FEFF) WHOLE and
// collapses control/whitespace runs, which is exactly what must never reach a
// combo row (a name with an RLO could visually reorder the list) or a derived
// name (a bidi mark between two model tokens would survive into user data).
// The SECOND argument is empty on purpose: no key is involved here, and the
// mask pass is skipped by construction for an empty key (so no occurrence of a
// configured key can ever be masked out of a profile name).
std::string SanitizeOpenAiProfileDisplayText(std::string_view text) {
    return SanitizeOpenAiErrorDetail(text, std::string_view{});
}

// The profile picker's label plan (design item 9.2). One UTF-8 label per
// entry, in LIBRARY order — the combo index IS the library index, and that
// identity is what makes CBN_SELCHANGE, [저장] and [삭제] address the same
// entry. The dialog then selects an index itself (see RefillOpenAiProfileCombo:
// an index at or past the end leaves the picker unselected, which is both the
// "opened on the active block" and the "I just deleted the selected entry"
// state).
//   * `name_template` is the localization of OpenAiProfileNameTemplate (UTF-8);
//     the dialog passes the current UI locale, the suite passes a literal.
//   * `fallback_label` is the localization of OpenAiProfileLabel, used ONLY for
//     an entry that carries neither a name nor a derivable one (a hand-edited
//     or down-migrated library); a closed combo must never show a blank row.
// A stored name is user data and is shown SANITIZED, never re-derived: the name
// is fixed at save time (design item 7), so a later locale change must not
// relabel the user's library.
std::vector<std::string> PlanOpenAiProfileComboLabels(
    const std::vector<OpenAiProfile>& profiles, std::string_view name_template,
    std::string_view fallback_label) {
    std::vector<std::string> labels;
    labels.reserve(profiles.size());
    for (const OpenAiProfile& profile : profiles) {
        std::string label = SanitizeOpenAiProfileDisplayText(profile.name);
        if (label.empty()) {
            // Nameless entry: derive the name the save path would have derived,
            // from the SANITIZED endpoint fields (bidi-strip before derivation,
            // design item 7 / R2-M).
            const std::string host =
                SanitizeOpenAiProfileDisplayText(OpenAiHostAbbrev(profile.cfg.base_url));
            const std::string model = SanitizeOpenAiProfileDisplayText(profile.cfg.model);
            label = SanitizeOpenAiProfileDisplayText(
                MakeOpenAiProfileName(host, model, name_template));
        }
        if (label.empty()) {
            label.assign(fallback_label);
        }
        labels.push_back(std::move(label));
    }
    return labels;
}

// 261122_0001 B-U6fix: the opened-list width plan's bounds. The margin keeps
// the longest name off the list's right edge; the caps keep a 96-byte
// pathological name (480 px at 9 pt) from producing a list wider than the
// screen — 3x the closed box or 640 px, whichever is smaller (at the shipped
// 357-px closed box the 640-px cap wins, which is what fits the 96-byte cap
// with room to spare).
inline constexpr int kProfileDroppedWidthMarginPx = 8;
inline constexpr int kProfileDroppedWidthMaxPx = 640;
inline constexpr int kProfileDroppedWidthCapFactor = 3;

// 261122_0001 B-U6fix (design items 2 + 4b): the OPENED-list width plan for the
// profile picker. A closed CBS_DROPDOWNLIST truncates its CLOSED box by OS
// design (the contract note lives at the combo's template entry in
// BuildOpenAiTemplate), so the full name must be readable the moment the list
// opens: CB_SETDROPPEDWIDTH sets the list's minimum width in pixels, and this
// planner decides the value. Same contract the model combo relies on.
//   * `labels` — the combo's display strings (UTF-8, library order): exactly
//     what RefillOpenAiProfileCombo just added.
//   * `combo_width_px` — the closed box width. It is the FLOOR (the opened list
//     may never be narrower than the box it drops from; the control ignores a
//     smaller value anyway) and one of the two caps.
//   * `measure_utf16` — the font measurer. The dialog passes the combo's own
//     font through GetTextExtentPoint32W; the suite passes a deterministic
//     function. Keeping the measurer as a parameter is what makes the PLANNING
//     pure: no window, no DC, no GDI in here.
// The plan: longest entry + kProfileDroppedWidthMarginPx, capped at
// min(3 x closed width, kProfileDroppedWidthMaxPx) so a 96-byte pathological
// name cannot produce a list wider than the screen. An EMPTY library keeps the
// OS default (the closed width). A measure failure (a non-positive extent)
// falls back to the floor rather than guessing.
int PlanOpenAiProfileDroppedWidth(
    const std::vector<std::string>& labels, int combo_width_px,
    const std::function<int(std::wstring_view)>& measure_utf16) {
    const int floor_px = (std::max)(combo_width_px, 0);
    if (labels.empty() || !measure_utf16) {
        return floor_px; // empty library: leave the OS default list width alone
    }
    int widest_px = 0;
    for (const std::string& label : labels) {
        if (label.empty()) continue;
        const int extent_px = measure_utf16(ToUtf16(label));
        if (extent_px > widest_px) widest_px = extent_px;
    }
    if (widest_px <= 0) {
        return floor_px; // nothing measurable: never shrink below the closed box
    }
    const long long cap_px = (std::min)(
        static_cast<long long>(floor_px) * kProfileDroppedWidthCapFactor,
        static_cast<long long>(kProfileDroppedWidthMaxPx));
    const long long wanted_px =
        static_cast<long long>(widest_px) + kProfileDroppedWidthMarginPx;
    const long long bounded_px = (std::min)(
        wanted_px, (std::max)(cap_px, static_cast<long long>(floor_px)));
    return static_cast<int>((std::max)(bounded_px, static_cast<long long>(floor_px)));
}

// The [저장] planner (design item 9.4 + design-pick item 3). `candidate` is the
// form already resolved by the caller: the on-screen base_url / model /
// reasoning effort / consent, and a FINAL key pair (the masked-placeholder and
// digest-before-protect work happens in ResolveOpenAiProfileKeyPair, because it
// needs DPAPI and must scrub the cleartext edit buffer).
//   * identity match  -> that entry's cfg is overwritten WHOLESALE (every field,
//                        including the pair and the consent flag) and its NAME
//                        is kept — the name is user data fixed at creation, and
//                        an endpoint edit must not silently rename a profile;
//   * no identity     -> append one entry whose name is derived from the
//     and there is        SANITIZED endpoint (host abbreviation -> template ->
//     room in the         de-duplication), so two saves of the same endpoint/
//     library             model family never present two identical rows;
//   * no identity     -> REFUSED: returns false, `out_library` is the caller's
//     and the library     library UNCHANGED (byte-identical, the cap rule of
//     is at the cap       design item 3 / kMaxOpenAiProfiles).
// Out-parameter form (no plan struct) for the same cross-TU reason as
// PlanOpenAiReasoningReseed above. `out_index` is the saved entry's index in
// `out_library` and is meaningful only when the call returns true.
bool PlanOpenAiProfileSave(const std::vector<OpenAiProfile>& library,
                           const OpenAiConfig& candidate, std::string_view name_template,
                           std::vector<OpenAiProfile>& out_library,
                           std::size_t& out_index) {
    out_library = library;
    out_index = PlanOpenAiProfileUpsert(library, candidate);
    if (out_index < library.size()) {
        out_library[out_index].cfg = candidate; // upsert: cfg wholesale, name kept
        return true;
    }
    if (library.size() >= kMaxOpenAiProfiles) {
        return false; // append refused: the cap; nothing was modified
    }
    // Append: host abbreviation first, then the sanitize (the 24-byte abbrev is
    // already bounded, so the display bound can never truncate a host), then the
    // model, then the localized template, then uniqueness inside the library.
    const std::string host =
        SanitizeOpenAiProfileDisplayText(OpenAiHostAbbrev(candidate.base_url));
    const std::string model = SanitizeOpenAiProfileDisplayText(candidate.model);
    OpenAiProfile added;
    added.name = MakeUniqueOpenAiProfileName(
        library, MakeOpenAiProfileName(host, model, name_template));
    added.cfg = candidate;
    out_library.push_back(std::move(added));
    out_index = out_library.size() - 1;
    return true;
}

// 261122_0001 fix-A2 (R1-01): identity-scoped REMOVAL for the REQ-051 [설정
// 삭제] control (declaration + identity rule in openai_settings_window.hpp).
// Why it exists: the delete branch's own fresh load runs config.cpp's
// absent-key migration synthesis, so on a pre-B2 config its library is a
// verbatim mirror of the very block being cleared — clear-then-save would
// persist the retired DPAPI pair and one profile-pick would re-adopt it
// (authenticated fetch included). Both persist sites (the dialog's
// read-modify-write and main.cpp's kDeleted reconcile) therefore prune the
// credential before saving, while every unrelated profile stays.
// DIGEST FIRST is the point: the digest is stable across re-protects, the blob
// is not. The keyless fallback mirrors the synthesizer field for field
// (base_url/model), restricted to keyless entries so a keyed profile for the
// same endpoint survives a delete of a keyless block.
std::size_t EraseOpenAiProfileMatchingKey(std::vector<OpenAiProfile>& profiles,
                                          const OpenAiConfig& deleted) {
    const bool keyless = deleted.api_key_dpapi.empty() && deleted.api_key_sha256.empty();
    if (keyless && deleted.base_url.empty() && deleted.model.empty()) {
        return 0; // no identity at all: never a blanket wipe
    }
    const std::size_t before = profiles.size();
    profiles.erase(
        std::remove_if(profiles.begin(), profiles.end(),
                       [&](const OpenAiProfile& profile) {
                           if (!keyless) {
                               return (!deleted.api_key_sha256.empty() &&
                                       profile.cfg.api_key_sha256 ==
                                           deleted.api_key_sha256) ||
                                      (!deleted.api_key_dpapi.empty() &&
                                       profile.cfg.api_key_dpapi == deleted.api_key_dpapi);
                           }
                           return profile.cfg.api_key_dpapi.empty() &&
                                  profile.cfg.api_key_sha256.empty() &&
                                  EqualsIgnoreCaseAscii<char>(profile.cfg.base_url,
                                                              deleted.base_url) &&
                                  profile.cfg.model == deleted.model;
                       }),
        profiles.end());
    return before - profiles.size();
}

// The cap notice's `{n}` substitution (design item 9.4). The StringId carries
// one token; the cap lives in code (kMaxOpenAiProfiles), so the number can
// never drift from the reader's own bound. Every occurrence is replaced (a
// locale may repeat it); an absent token returns the message unchanged.
std::wstring FormatOpenAiProfileLimitNotice(std::wstring_view message, std::size_t limit) {
    constexpr std::wstring_view kToken = L"{n}";
    const std::wstring value = std::to_wstring(limit);
    std::wstring out;
    out.reserve(message.size() + value.size());
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = message.find(kToken, pos);
        if (hit == std::wstring_view::npos) {
            out.append(message.substr(pos));
            return out;
        }
        out.append(message.substr(pos, hit - pos));
        out.append(value);
        pos = hit + kToken.size();
    }
}

// The [저장] key-handling matrix (design item 9.4). `key_edit` is the API-key
// edit's UTF-8 text and is SCRUBBED IN PLACE on every path that touched a
// cleartext (the REQ-050/OF-01 contract: ProtectOpenAiApiKey scrubs the caller's
// buffer itself, and the explicit SecureZeroMemory covers the failure arm).
// `existing` is the pair the dialog carries in *st->cfg — the verified active
// pair at open, or the selected profile's verified pair.
//   * masked placeholder -> KEEP `existing`'s pair verbatim (the placeholder is
//                           display text, never a key);
//   * a typed key        -> digest BEFORE protect (hashing after protect would
//                           store SHA-256 of the scrubbed zero buffer and the
//                           pair would be refused forever, REQ-050 b6f98da),
//                           then DPAPI-protect in place;
//   * empty edit         -> an EMPTY pair (the user cleared the key; adopting
//                           the previous profile's pair here would silently
//                           resurrect a key the form no longer shows);
//   * protect failure    -> false with BOTH halves cleared, so a half-pair can
//                           never reach the refuse-forever state.
// No plaintext survives: the edit buffer is zeroed and the only key material in
// `out` is the opaque DPAPI blob + its digest.
bool ResolveOpenAiProfileKeyPair(std::string& key_edit, const OpenAiConfig& existing,
                                 OpenAiConfig& out) {
    if (IsMaskedPlaceholder(key_edit)) {
        out.api_key_dpapi = existing.api_key_dpapi;
        out.api_key_sha256 = existing.api_key_sha256;
        return true;
    }
    if (key_edit.empty()) {
        out.api_key_dpapi.clear();
        out.api_key_sha256.clear();
        return true;
    }
    OpenAiSha256Hex(key_edit, out.api_key_sha256);
    const bool protected_ok = ProtectOpenAiApiKey(key_edit, out.api_key_dpapi);
    SecureZeroMemory(key_edit.data(), key_edit.size());
    if (!protected_ok) {
        out.api_key_dpapi.clear();
        out.api_key_sha256.clear();
        return false;
    }
    return true;
}

namespace {

// Control IDs.
enum : WORD {
    IDC_STATIC_BASE = 100,
    IDC_BASE_URL,
    IDC_STATIC_KEY,
    IDC_API_KEY,
    IDC_STATIC_MODEL,
    IDC_MODEL_COMBO,
    IDC_FETCH_BTN,
    IDC_MASKED_LABEL,
    IDC_DELETE_BTN,   // REQ-051 D: [설정 삭제] push button (bottom-left row)
    // 261116_0003 P9c-1b: the "이 모델이 지원하는 언어" declaration checklist —
    // a section label, ONE multi-select LISTBOX, and the fail-open hint line.
    IDC_DECLARE_TITLE,
    IDC_DECLARE_LIST,
    IDC_DECLARE_HINT,
    // 261121_0001 U-H (contracts C2/C4): the reasoning-effort row — a label
    // and ONE closed pick list of the efforts the selected model supports
    // (server-advertised when available, the standard list otherwise).
    IDC_REASONING_LABEL,
    IDC_REASONING_COMBO,
    // 261122_0001 B-U3 (design-pick.md item 5): the profile row — a closed
    // profile picker plus the [저장]/[삭제] upsert/delete buttons that act on
    // the library copy (BEHAVIOR lands in U4; this unit owns ids + template
    // geometry only). Appended AFTER the existing ids: the REQ-048 byte pin
    // and every existing numeric id stay untouched.
    IDC_PROFILE_LABEL,
    IDC_PROFILE_COMBO,     // CBS_DROPDOWNLIST (a profile is picked, not typed)
    IDC_PROFILE_SAVE_BTN,
    IDC_PROFILE_DEL_BTN,
};

// 261121_0001 U-H (contract C4): the by-value payload of the model fetch — the
// ids the endpoint lists PLUS, per model, the reasoning efforts it advertises.
// ONE GET produces both (OpenAiCompatibleClient::FetchModelsAndCapabilities),
// so filling the reasoning combo costs no extra round trip. Declared before
// OpenAiDialogState/OpenAiFetchResult, which both carry it; every member is a
// plain value so the R1-9 "no pointer crosses PostMessageW" transport rule
// still holds unchanged.
struct OpenAiModelCatalog {
    std::vector<std::string> models;                  // empty == fetch failed / none
    std::vector<OpenAiModelCapability> capabilities;   // subset the server described
};

struct OpenAiDialogState {
    OpenAiConfig* cfg;   // in/out; key DPAPI-protected on save
    bool saved = false;
    bool deleted = false; // REQ-051 D: [삭제] persisted the cleared block itself
    // 261116_0003 P9c-1c: true once IDOK folded the declaration checklist.
    // The fold result rides out through the declaration-aware entry
    // (OpenAiDeclaredLanguagesFold) so the OWNING config applies it; false on
    // Cancel and on the [삭제] button, where no selection was read and the
    // caller must not adopt anything. GUI thread only.
    bool declaration_edited = false;
    // REQ-052 (async fetch): single-flight generation — every spawn
    // increments it; a completion carrying an older generation is stale (a
    // newer fetch superseded it) and is dropped. GUI thread only.
    unsigned long long fetch_generation = 0;
    // REQ-052 (async fetch): spawn origin. The call site sets it
    // immediately before spawning (both call sites are GUI-thread) and the
    // spawn copies it into the worker args: a manual completion may show
    // the failure notice, an auto completion stays silent. GUI thread only.
    bool fetch_manual = false;
    // 260922_0001 A6 (UI audit rank 6, disable-only feedback): the fetch
    // button's disabled mirror of the single-flight bid above. Set true by
    // the successful spawn in DoFetchModels (manual button AND debounced
    // auto-fetch), cleared by the completion handler on EVERY exit path
    // (current, stale-generation, empty result) so a superseded fetch can
    // never leave the button stuck gray. No i18n string accompanies it
    // (§5 A6 scope: the graying alone is the feedback). GUI thread only.
    bool fetch_button_busy = false;
    // REQ-052 (async fetch): liveness shared with every detached worker
    // (one shared_ptr copy each). WM_DESTROY flips it to false so a worker
    // finishing after the dialog is gone drops its own result value (R1-9: it
    // is never a heap object) instead of posting to a destroyed hwnd.
    std::shared_ptr<std::atomic<bool>> fetch_alive =
        std::make_shared<std::atomic<bool>>(true);
    // 261116_0003 P9c-1b: the declaration checklist's in/out. `declared_in`
    // captures the canonical set at WM_INITDIALOG (empty = not declared =
    // nothing pre-selected, the documented fail-open); `declared_out`
    // receives the folded selection at IDOK and rides out through the
    // declaration-aware entry (261116_0003 P9c-1c), where the OWNING config
    // applies it. `declare_rows` is the listbox order — the UNFILTERED
    // OpenAI-capable registry codes — that the selection folds through. GUI
    // thread only.
    std::vector<std::string> declared_in;
    std::vector<std::string> declared_out;
    std::vector<std::string> declare_rows;
    // 261121_0001 U-H (contract C4): what the LAST completed fetch learned
    // about the models — the ids plus, per model, the reasoning efforts the
    // server advertised. Stays empty until a fetch succeeds (and stays empty
    // forever on the endpoints that advertise nothing, the common case), which
    // is exactly when PlanReasoningEffortItems falls back to the standard list.
    // GUI thread only: the worker hands it over by value in OpenAiFetchResult.
    OpenAiModelCatalog catalog;
    // 261121_0001 U-H (contract C2): the combo's item VALUES as last rendered
    // (index 0 = the server-default item = ""). The combo shows localized
    // labels, not values, so this is the only lossless index -> value map when
    // IDOK reads the selection back. GUI thread only.
    std::vector<std::string> reasoning_items;
    // The reasoning value the dialog opened with (cfg->reasoning_effort).
    // Only a fallback for the very first population: once the combo holds
    // items, the combo itself is the single source of truth, so a repopulation
    // can neither lose nor invent a selection.
    std::string reasoning_effort;
    // 261121_0001 fix round 1 (F-4): true while `reasoning_effort` came from the
    // saved config and NO list has been able to show it yet. The first
    // population always runs against the fallback list (no fetch has completed),
    // which cannot carry a server-extension value such as OpenRouter's "xhigh";
    // this flag keeps that value alive until a catalog that advertises it
    // arrives, and keeps it for the OK path even if none ever does. Cleared when
    // a list shows the value and whenever the user makes an explicit pick. GUI
    // thread only.
    bool reasoning_effort_pending = false;
    // 261122_0001 B-U4 (design-pick item 9.1): the profile library's WORKING
    // COPY — the dialog's only mutable view of the library. Seeded from the
    // caller's fold on entry, mutated by [저장] (upsert) and [삭제] (erase), and
    // handed back through OpenAiProfilesFold on the IDOK path. The dialog never
    // writes it to disk (single-writer: the caller persists), and the active
    // `openai` block stays the on-screen fields (REQ-051 semantics). GUI thread
    // only.
    std::vector<OpenAiProfile> profiles;
    // True once [저장] or [삭제] changed that working copy. The dialog return
    // path reads it to decide whether the fold carries the library out — false
    // on Cancel and on the REQ-051 [설정 삭제] path, where the staged profile
    // edits are discarded and the caller keeps its stored library. GUI thread
    // only.
    bool profiles_edited = false;
};

std::wstring GetCtrlText(HWND dlg, int id) {
    HWND c = ::GetDlgItem(dlg, id);
    if (!c) return {};
    int len = ::GetWindowTextLengthW(c);
    std::wstring s(static_cast<size_t>(len) + 1, L'\0');
    ::GetWindowTextW(c, s.data(), len + 1);
    s.resize(static_cast<size_t>(len));
    return s;
}

void SetCtrlText(HWND dlg, int id, std::wstring_view text) {
    if (HWND c = ::GetDlgItem(dlg, id)) {
        ::SetWindowTextW(c, std::wstring(text).c_str());
    }
}

// REQ-050 (auto-fetch): debounce timer for the model-list auto-fetch. The
// timer id lives in the Win32 timer namespace (no clash with the control IDs
// 100..117 above). Re-arming an existing id via SetTimer is a silent
// kill+restart, which is exactly the debounce semantics the EN_CHANGE
// handler wants.
constexpr UINT_PTR kFetchTimerId = 101;
constexpr UINT kFetchDebounceMs = 800;

// REQ-052 (async fetch): worker->dialog completion message. R1-9 (v0.10.4
// sweep, CWE-822 fix): the message is a PURE (0, 0) WAKE-UP and carries NO
// payload. This dialog is a top-level system-class #32770 owned by a
// medium-IL asInvoker process, and the id is a compile-time WM_APP constant,
// so ANY same-session same-IL process can post it with an arbitrary LPARAM.
// Transporting a heap pointer in lParam therefore handed a peer an
// arbitrary-free primitive (deref + delete of attacker memory). The result
// now travels BY VALUE through the mutex-guarded deque below, exactly the
// SEC-B5 / SEC-ADJ house pattern (src/main.cpp kMsgApplyLanguageSync,
// src/ui/tooltip.cpp's three marshal seams): producer enqueues then posts;
// the handler ignores wParam/lParam and drains; teardown drains. WM_APP band
// — this dialog owns its ids (the GGUF add dialog uses WM_APP+0x10/+0x11).
constexpr UINT kOpenAiMsgFetchDone = WM_APP + 0x20;

// REQ-052 (async fetch): the fetch result, delivered BY VALUE (R1-9). The
// worker builds it on its own stack, pushes a copy into the queue and forgets
// it; the GUI handler drains the queue and applies the generation filter to
// the drained VALUE (`generation` is the single-flight key: a completion older
// than the dialog's current fetch generation is stale — a newer fetch
// superseded it — and must be dropped, not refilled). No pointer ever crosses
// PostMessageW, so an externally posted message cannot name any memory.
struct OpenAiFetchResult {
    unsigned long long generation;
    bool manual; // button-originated spawn (may show the failure notice)
    // 261121_0001 U-H (contract C4): the ids AND the per-model reasoning
    // capabilities the same GET returned, so the completion handler can fill
    // the reasoning combo without touching the network.
    OpenAiModelCatalog catalog;
};

// R1-9: the lifetime-safe holder for the in-flight results. The queue is
// file-static (tooltip.cpp's solution), NOT a dialog-state member: a detached
// worker may finish after the dialog is gone and must still have a valid
// object to enqueue into. Scope note copied from the tooltip contract: this
// dialog is modal on the GUI thread, so one instance is live at a time; the
// WM_INITDIALOG drain discards anything a post-teardown worker left behind, so
// no entry is ever applied to a later instance (its generation is >= 1, and a
// fresh dialog starts at 0, so even the in-band path drops it). Sole
// consumers: the GUI-thread completion drain and the teardown sweeps
// (WM_INITDIALOG / WM_DESTROY / dialog-return).
std::mutex g_openai_fetch_mu;
std::deque<OpenAiFetchResult> g_openai_fetch_queue;

// R1-9: discard every result still queued, REMOVING without applying (the
// tooltip DrainMarshalQueue semantics). Called on the GUI thread only: at
// WM_INITDIALOG (never apply a pre-instance entry), at WM_DESTROY (a closing
// dialog must not keep a worker's models alive), and right after
// DialogBoxIndirectParamW returns (catches a worker that passed the liveness
// check just before the flag flipped).
size_t DrainOpenAiFetchQueue() {
    std::lock_guard<std::mutex> lk(g_openai_fetch_mu);
    const size_t drained = g_openai_fetch_queue.size();
    g_openai_fetch_queue.clear();
    return drained;
}

// REQ-052 (async fetch): the network boundary of the model-list fetch,
// split out of the old synchronous DoFetchModels(dlg, st) body. Runs ONLY
// on the detached worker thread — the caller hands over the probe composed
// on the GUI thread (by value) — because the GUI thread must never enter
// the synchronous WinHTTP call again: that was the audit P0 freeze (the
// whole dialog went '응답 없음' mid-typing while the bounded 10 s budget ran
// inline).
// 261121_0001 U-H (contract C4): the call is now
// FetchModelsAndCapabilities, so the SAME GET that lists the models also
// reports the reasoning efforts each one advertises. That is the only source
// of truth for "what may I send", and it replaces the id-only ListModels call
// outright: a second request would double the latency and could disagree with
// the first. The capabilities travel back with the ids (OpenAiModelCatalog).
OpenAiModelCatalog FetchOpenAiModelCatalog(const OpenAiConfig& probe) {
    OpenAiModelCatalog catalog;
    OpenAiCompatibleClient::FetchModelsAndCapabilities(
        probe, &catalog.models, &catalog.capabilities);
    return catalog;
}

// REQ-052 (async fetch): the detached worker body. It owns ONLY the probe
// copy it was handed and its own result VALUE — no dialog state, no config —
// so a dialog destroyed mid-fetch can never catch it touching freed memory.
// After the bounded network call it pushes the result into the shared queue
// and posts the pure (0, 0) wake-up; a dialog that already closed is caught by
// the liveness flag (drop the value locally, nothing was queued) or by the
// failed post (the entry stays queued for the next drain — the tooltip
// contract, see g_openai_fetch_queue). Detached at spawn: its whole lifetime
// is the bounded WinHTTP budget, and the generation check (not a join) is what
// keeps a late completion off the combo — joining here would reintroduce the
// very GUI-thread block this fix removes.
void OpenAiFetchWorker(std::shared_ptr<std::atomic<bool>> alive, HWND dlg,
                       unsigned long long generation, bool manual,
                       OpenAiConfig probe) {
    // R1-9: BY VALUE — no heap object, no ownership transfer over the message
    // queue. The dialog-gone check runs AFTER the fetch (the answer is only
    // interesting if someone is still listening) and the value simply dies
    // here when nobody is.
    OpenAiFetchResult result{generation, manual, FetchOpenAiModelCatalog(probe)};
    if (!alive->load(std::memory_order_acquire)) {
        return; // dialog gone: nothing to deliver, nothing leaked
    }
    {
        std::lock_guard<std::mutex> lk(g_openai_fetch_mu);
        g_openai_fetch_queue.push_back(std::move(result));
    }
    // R1-9: pure (0, 0) wake-up — the payload is already in the queue, so an
    // external post of this id can name no memory and drain nothing it did not
    // enqueue. A failed post leaves the entry queued (drain-safe): the next
    // wake-up or a teardown sweep pops it exactly once.
    if (::PostMessageW(dlg, kOpenAiMsgFetchDone, 0, 0) == FALSE) {
        DIAG_F("UI/OpenAiSettings/009: PostMessage kOpenAiMsgFetchDone failed (GLE %lu); "
               "result left queued\n",
               ::GetLastError());
    }
}

// REQ-050 (auto-fetch): a key is "available" for the debounced fetch when
// the edit holds anything (a typed key or the masked placeholder of the
// stored pair) or the stored pair itself survived the WM_INITDIALOG
// integrity check (a mismatch clears both fields, so a non-empty blob here
// is a verified pair).
bool KeyAvailableForFetch(HWND dlg, const OpenAiConfig* cfg) {
    if (!GetCtrlText(dlg, IDC_API_KEY).empty()) return true;
    return !cfg->api_key_dpapi.empty();
}

// REQ-052 (async fetch): the combo refill, extracted UNCHANGED from the old
// synchronous DoFetchModels tail — this is the same post-success logic, now
// executed by the GUI completion handler instead of inline after the
// network call returned on the GUI thread.
// REQ-051 (Symptom C): the refill no longer clobbers the current model —
// it pins the combo's current text (saved model or user-typed) via
// PlanOpenAiComboSelection: exact match -> select it; absent -> append it
// and select the append; empty fetch -> the combo is left completely
// untouched so the injected current-model text stays visible.
void RefillOpenAiModelCombo(HWND dlg, const std::vector<std::string>& models) {
    if (models.empty()) {
        // REQ-051 (Symptom C): fetch failed / empty — do NOT reset the
        // combo (CB_RESETCONTENT would also wipe the edit text on this
        // CBS_DROPDOWN control, hiding the injected current-model text).
        // Leaving the list + text intact keeps the in-use model visible
        // and selectable; the manual-fetch button still surfaces the
        // failure notice, the silent auto path stays silent.
        return;
    }
    HWND combo = ::GetDlgItem(dlg, IDC_MODEL_COMBO);
    if (!combo) return;
    // REQ-051 (Symptom C): capture the current-model text BEFORE the
    // reset — after CB_RESETCONTENT the edit text is gone.
    const std::string target = ToUtf8(GetCtrlText(dlg, IDC_MODEL_COMBO));
    ::SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    std::vector<std::pair<int, std::string>> items;
    items.reserve(models.size());
    for (const auto& m : models) {
        const int idx = static_cast<int>(::SendMessageW(
            combo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(ToUtf16(m).c_str())));
        if (idx >= 0) items.emplace_back(idx, m);
    }
    // REQ-051 (Symptom C): select the saved/current model when the list
    // holds it; otherwise append the current text and select THAT (the
    // pre-REQ-051 code unconditionally selected index 0, clobbering the
    // saved model). Correctness of the selection beats everything else.
    const OpenAiComboSelection plan = PlanOpenAiComboSelection(items, target);
    if (plan.insert_target) {
        const int idx = static_cast<int>(::SendMessageW(
            combo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(ToUtf16(target).c_str())));
        if (idx >= 0) ::SendMessageW(combo, CB_SETCURSEL, idx, 0);
    } else if (plan.select_index >= 0) {
        ::SendMessageW(combo, CB_SETCURSEL, plan.select_index, 0);
    }
}

// 261121_0001 U-H: the efforts `model` advertised in the last completed fetch,
// or nullptr when the fetch did not describe it (no fetch yet, a typed/unlisted
// id, or an endpoint that publishes no capability metadata at all — OpenAI and
// Gemini both). nullptr is not an error: the caller then takes the standard
// fallback list.
const std::vector<std::string>* FindAdvertisedReasoningEfforts(
    const OpenAiModelCatalog& catalog, const std::string& model) {
    if (model.empty()) return nullptr;
    for (const OpenAiModelCapability& cap : catalog.capabilities) {
        if (cap.id == model) return &cap.reasoning_efforts;
    }
    return nullptr;
}

// The reasoning combo's current VALUE (contract C2: "" = the server default,
// whose request omits the key). The combo SHOWS localized labels, so the
// selection is mapped back through the item values captured at the last
// population; a missing control — or a selection no population produced — falls
// back to the value the dialog opened with.
std::string ReasoningEffortSelection(HWND dlg, const OpenAiDialogState* st) {
    HWND combo = ::GetDlgItem(dlg, IDC_REASONING_COMBO);
    if (!combo) return st->reasoning_effort;
    const LRESULT sel = ::SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (sel < 0 || static_cast<std::size_t>(sel) >= st->reasoning_items.size()) {
        return st->reasoning_effort;
    }
    return st->reasoning_items[static_cast<std::size_t>(sel)];
}

// 261121_0001 U-H: (re)build the reasoning combo for the model currently in
// the model combo, keeping the current pick whenever the new list still offers
// it. Called at WM_INITDIALOG and on every model change / fetch completion, so
// the list always belongs to the model the user is looking at.
void RefreshReasoningCombo(HWND dlg, OpenAiDialogState* st) {
    HWND combo = ::GetDlgItem(dlg, IDC_REASONING_COMBO);
    if (!combo) return;
    // Capture what the dialog carries BEFORE the reset. The state value, not a
    // control read: during the pending restoration (F-4) the combo legitimately
    // shows the server-default item while the effective value is still the saved
    // one, and losing that distinction here is exactly the silent preference
    // loss this fix removes.
    const std::string previous = st->reasoning_effort;
    const std::string model = ToUtf8(GetCtrlText(dlg, IDC_MODEL_COMBO));
    const std::vector<std::string>* advertised =
        FindAdvertisedReasoningEfforts(st->catalog, model);
    const std::vector<std::string> no_efforts;
    std::vector<std::string> items =
        PlanReasoningEffortItems(advertised ? *advertised : no_efforts);
    // F-4: the selection/value/pending reconciliation is the pure planner, so
    // the suite pins the exact rules the dialog applies.
    int select = 0;
    std::string effective = previous;
    bool pending = st->reasoning_effort_pending;
    PlanOpenAiReasoningComboPopulate(items, previous, st->reasoning_effort_pending,
                                     select, effective, pending);
    ::SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const std::string& value : items) {
        const std::wstring label = value.empty()
            ? I18n::Get(StringId::OpenAiReasoningServerDefault)
            : ToUtf16(value);
        ::SendMessageW(combo, CB_ADDSTRING, 0,
                       reinterpret_cast<LPARAM>(label.c_str()));
    }
    ::SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(select), 0);
    // A value the selected model does not offer leaves the combo on the
    // server-default item: sending an effort the endpoint rejects is a hard
    // HTTP 400, so the combo converges on what it can actually show. The value
    // itself survives while pending (F-4). Shape-only log (no content, no model
    // id).
    if (select == 0 && !previous.empty()) {
        DIAG_F("UI/OpenAiSettings/011: the combo cannot show the current reasoning "
               "effort; it fell back to the server default%s\n",
               pending ? " (the saved value is kept for the save path)" : "");
    }
    st->reasoning_items = std::move(items);
    st->reasoning_effort = std::move(effective);
    st->reasoning_effort_pending = pending;
}

// 261122_0001 B-U6fix (design item 2): push the opened-list width plan onto the
// live control. Measuring happens with the combo's OWN font (WM_GETFONT — the
// DS_SETFONT dialog font the template installs), so the plan matches what the
// list will actually render; the plan itself is the pure helper above. The
// control ignores CB_SETDROPPEDWIDTH on a style that cannot drop down, so this
// is safe on every path that reaches here. Failure to obtain a DC keeps the OS
// default list width (the closed box) — never a hard error, never a shorter
// list than the box.
void ApplyOpenAiProfileDroppedWidth(HWND combo,
                                    const std::vector<std::string>& labels) {
    RECT rc{};
    if (!::GetClientRect(combo, &rc)) return;
    const int combo_width_px = rc.right - rc.left;
    HDC dc = ::GetDC(combo);
    if (!dc) return;
    HFONT font = reinterpret_cast<HFONT>(::SendMessageW(combo, WM_GETFONT, 0, 0));
    HGDIOBJ old_font = font ? ::SelectObject(dc, font) : nullptr;
    const int width_px = PlanOpenAiProfileDroppedWidth(
        labels, combo_width_px,
        [dc](std::wstring_view text) -> int {
            if (text.empty()) return 0;
            SIZE sz{};
            if (!::GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()),
                                         &sz)) {
                return 0; // measure failure: the plan keeps the closed-box floor
            }
            return static_cast<int>(sz.cx);
        });
    if (old_font) ::SelectObject(dc, old_font);
    ::ReleaseDC(combo, dc);
    ::SendMessageW(combo, CB_SETDROPPEDWIDTH, static_cast<WPARAM>(width_px), 0);
}

// 261122_0001 B-U4 (design item 9.2): (re)populate the profile picker from the
// WORKING library. `select_index >= profiles.size()` leaves the picker
// UNSELECTED — the state the dialog opens in (it always opens on the active
// block, never on a profile) and the state after the selected entry was just
// deleted. The label list and the index rule are the pure plan
// (PlanOpenAiProfileCombo), so the suite pins exactly what the combo shows.
// The picker is a closed CBS_DROPDOWNLIST: a refill can never fabricate a
// typed profile, and CB_SETCURSEL is programmatic (it does NOT raise
// CBN_SELCHANGE), so a refill cannot re-enter ApplyOpenAiProfileSelection.
void RefillOpenAiProfileCombo(HWND dlg, OpenAiDialogState* st, std::size_t select_index) {
    HWND combo = ::GetDlgItem(dlg, IDC_PROFILE_COMBO);
    if (!combo) return;
    const std::vector<std::string> labels = PlanOpenAiProfileComboLabels(
        st->profiles, ToUtf8(I18n::Get(StringId::OpenAiProfileNameTemplate)),
        ToUtf8(I18n::Get(StringId::OpenAiProfileLabel)));
    ::SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const std::string& label : labels) {
        const std::wstring wide = ToUtf16(label);
        ::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide.c_str()));
    }
    // An index at or past the end leaves the picker UNSELECTED: that is the
    // "dialog opened on the active block" state and the "the selected profile
    // was just deleted" state, never a fabricated pick.
    if (select_index < st->profiles.size()) {
        ::SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(select_index), 0);
    }
    // 261122_0001 B-U6fix (design item 2): EVERY rebuild re-plans the opened
    // list's width, so a [저장]/[삭제] that changed the longest name is reflected
    // the next time the list drops open. The closed box may truncate a name
    // longer than the realistic derived worst (OS design, see the contract note
    // on the combo's template entry); the opened list always fits.
    ApplyOpenAiProfileDroppedWidth(combo, labels);
}

// 261122_0001 B-U4 (design item 9.3): a profile pick refills the form from that
// profile's cfg. Order matters and is part of the contract:
//   1. the KEY PAIR is resolved first, because the masked placeholder in the
//      edit is display text and the IDOK/[저장] paths read the actual pair from
//      the dialog's in/out copy (*st->cfg). The pair is adopted ONLY when it
//      passes the REQ-050 integrity check (unprotect + digest re-hash); a pair
//      that fails is NOT adopted and the key edit is cleared, which is the
//      REQ-050 posture — the user re-enters the key once and the save path
//      stores a valid pair. An entry with no pair clears BOTH as well: keeping
//      the previous profile's placeholder visible would silently re-adopt the
//      previous profile's key on the next save.
//   2. base URL / model text. Each SetWindowTextW raises EN_CHANGE, which
//      re-arms the REQ-050 debounce timer through the existing handler, so an
//      https+key profile auto-fetches its model list exactly like a reopened
//      saved dialog (and the completion repopulates the reasoning row).
//   3. http consent, adopted from the profile so the consent gate does not
//      re-ask about an endpoint the user already accepted.
//   4. the reasoning effort is RE-SEEDED through the F-4 pending convention
//      (PlanOpenAiReasoningReseed) and the combo repopulated for the model now
//      on screen.
void ApplyOpenAiProfileSelection(HWND dlg, OpenAiDialogState* st) {
    HWND combo = ::GetDlgItem(dlg, IDC_PROFILE_COMBO);
    if (!combo) return;
    const LRESULT sel = ::SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (sel < 0 || static_cast<std::size_t>(sel) >= st->profiles.size()) {
        return; // no pick (or a stale index): nothing to refill
    }
    const OpenAiProfile& profile = st->profiles[static_cast<std::size_t>(sel)];

    std::string key;
    const bool pair_adopted = !profile.cfg.api_key_dpapi.empty() &&
                              OpenAiKeyPairIntegrityOk(profile.cfg) &&
                              UnprotectOpenAiApiKey(profile.cfg.api_key_dpapi, key);
    if (pair_adopted) {
        st->cfg->api_key_dpapi = profile.cfg.api_key_dpapi;
        st->cfg->api_key_sha256 = profile.cfg.api_key_sha256;
    } else {
        st->cfg->api_key_dpapi.clear();
        st->cfg->api_key_sha256.clear();
        SetCtrlText(dlg, IDC_API_KEY, L"");
        if (!profile.cfg.api_key_dpapi.empty()) {
            // Shape-only: the key material itself is never logged.
            DIAG_F("UI/OpenAiSettings/013: the selected profile's API-key pair failed "
                   "the integrity check; the key field was cleared for re-entry\n");
        }
    }
    SetCtrlText(dlg, IDC_BASE_URL, ToUtf16(profile.cfg.base_url));
    SetCtrlText(dlg, IDC_MODEL_COMBO, ToUtf16(profile.cfg.model));
    if (pair_adopted) {
        SetCtrlText(dlg, IDC_API_KEY, ToUtf16(MaskOpenAiApiKey(key)));
    }
    SecureZeroMemory(key.data(), key.size());
    st->cfg->http_consent_given = profile.cfg.http_consent_given;
    PlanOpenAiReasoningReseed(profile.cfg.reasoning_effort, st->reasoning_effort,
                              st->reasoning_effort_pending);
    RefreshReasoningCombo(dlg, st);
}

// REQ-050 (auto-fetch) / REQ-052 (async): the model-list fetch shared by the
// manual "Fetch model list" button and the debounced auto-fetch — the
// pre-REQ-052 DoFetchModels body split at the network boundary. The GUI
// thread still reads the controls, composes the probe (digest BEFORE
// protect) and runs the http consent gate; ONLY the synchronous network
// call moved onto the detached worker. The RESULT NOTICE contract is
// unchanged: a declined fetch (http consent denied) or a failed spawn
// returns false so the manual button shows the failure box immediately
// (exactly as before); a spawned-but-empty fetch shows it from the
// completion handler; the auto path stays silent in every case.
bool DoFetchModels(HWND dlg, OpenAiDialogState* st) {
    OpenAiConfig probe;
    probe.base_url = ToUtf8(GetCtrlText(dlg, IDC_BASE_URL));
    std::string keyUtf8 = ToUtf8(GetCtrlText(dlg, IDC_API_KEY));
    if (IsMaskedPlaceholder(keyUtf8)) {
        probe.api_key_dpapi = st->cfg->api_key_dpapi;
        probe.api_key_sha256 = st->cfg->api_key_sha256;
    } else if (!keyUtf8.empty()) {
        // REQ-050: digest BEFORE protect — ProtectOpenAiApiKey scrubs
        // the caller's cleartext buffer in place, so hashing after it
        // persisted SHA-256(zero buffer) and the probed key never
        // matched the saved digest.
        OpenAiSha256Hex(keyUtf8, probe.api_key_sha256);
        ProtectOpenAiApiKey(keyUtf8, probe.api_key_dpapi);
    }
    SecureZeroMemory(keyUtf8.data(), keyUtf8.size());
    // REQ-050: the pre-save fetch hit the same http consent gate as
    // IDOK, but the probe never set http_consent_given, so an http://
    // base URL was always rejected before any network I/O. Mirror the
    // IDOK flow: ask the SAME consent question when the probed base
    // URL is http and no consent is on record; abort the fetch on NO.
    const OpenAiUrlSecurity probe_sec = ClassifyOpenAiBaseUrl(probe.base_url);
    if (probe_sec == OpenAiUrlSecurity::Http && !st->cfg->http_consent_given) {
        const int rc = ::MessageBoxW(
            dlg, I18n::Get(StringId::OpenAiHttpWarningBody).c_str(),
            I18n::Get(StringId::OpenAiHttpWarningTitle).c_str(),
            MB_YESNO | MB_ICONWARNING);
        if (rc != IDYES) return false; // user declined: abort the fetch
        probe.http_consent_given = true;
    } else {
        probe.http_consent_given = st->cfg->http_consent_given;
    }
    // REQ-052: single-flight — bump the generation BEFORE the spawn so the
    // completion handler drops any older in-flight completion (a newer
    // fetch supersedes it; the stale worker only wasted its bounded
    // WinHTTP budget and its drained result value).
    ++st->fetch_generation;
    // REQ-052: the probe (base URL + DPAPI key pair + consent) crosses into
    // the worker BY VALUE — the cleartext key never leaves this thread (it
    // was scrubbed above) and the worker touches no dialog state.
    try {
        std::thread(OpenAiFetchWorker, st->fetch_alive, dlg, st->fetch_generation,
                    st->fetch_manual, std::move(probe))
            .detach();
    } catch (...) {
        // Thread creation failure (resource exhaustion): the fetch never
        // started — roll the supersede back so an older in-flight
        // completion is not falsely demoted to stale.
        DIAG_F("UI/OpenAiSettings/008: model-list fetch worker spawn failed\n");
        --st->fetch_generation;
        return false;
    }
    // 260922_0001 A6 (UI audit rank 6): the worker is in flight — gray the
    // fetch button so the click is visibly acknowledged (the audit finding:
    // no loading feedback at all). This single site covers BOTH spawn
    // callers: the manual IDC_FETCH_BTN below and the debounced auto-fetch
    // (WM_TIMER). The declined-consent and failed-spawn paths returned
    // above, so they keep the button enabled with their existing notice.
    st->fetch_button_busy = true;
    ::EnableWindow(::GetDlgItem(dlg, IDC_FETCH_BTN), FALSE);
    return true;
}

INT_PTR CALLBACK OpenAiSettingsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<OpenAiDialogState*>(
        ::GetWindowLongPtrW(dlg, GWLP_USERDATA));
    switch (msg) {
    case WM_INITDIALOG: {
        st = reinterpret_cast<OpenAiDialogState*>(lp);
        ::SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        // R1-9: a fresh instance starts with an EMPTY queue. A worker from a
        // previous dialog whose post failed leaves its entry behind (the
        // tooltip "left queued" contract); it must never be applied here, so
        // the leftover is discarded rather than filtered (its generation is
        // >= 1 against this instance's 0, but the drain is the deterministic
        // guarantee).
        DrainOpenAiFetchQueue();
        // REQ-047 D3 (architect §C.4 "API입력칸 미노출" verification): the
        // template itself is proven-good (20 controls incl. the 261122_0001
        // B-U3 profile row, OK/Cancel, the REQ-051 delete button, the
        // 261116_0003 P9c-1b declaration checklist section, and the
        // 261121_0001 U-H reasoning-effort row), but
        // if any edit/combo control failed to materialize the dialog would
        // look like the
        // reported "API input field missing" symptom. Surface a distinct log
        // line per missing control so a future regression separates "window
        // failed to activate" (D3 root cause) from "control failed to create".
        if (!::GetDlgItem(dlg, IDC_BASE_URL)) {
            DIAG_F("UI/OpenAiSettings/001: IDC_BASE_URL control missing after dialog init\n");
        }
        if (!::GetDlgItem(dlg, IDC_API_KEY)) {
            DIAG_F("UI/OpenAiSettings/002: IDC_API_KEY control missing after dialog init\n");
        }
        if (!::GetDlgItem(dlg, IDC_MODEL_COMBO)) {
            DIAG_F("UI/OpenAiSettings/003: IDC_MODEL_COMBO control missing after dialog init\n");
        }
        if (!::GetDlgItem(dlg, IDC_REASONING_COMBO)) {
            DIAG_F("UI/OpenAiSettings/010: IDC_REASONING_COMBO control missing after "
                   "dialog init\n");
        }
        // REQ-050 (cue banners): gray placeholder hints on both single-line
        // edits while they are empty (EM_SETCUEBANNER — see the define above;
        // works on Vista+ under the UNICODE build).
        if (HWND base_edit = ::GetDlgItem(dlg, IDC_BASE_URL)) {
            ::SendMessageW(base_edit, EM_SETCUEBANNER, TRUE,
                           reinterpret_cast<LPARAM>(
                               I18n::Get(StringId::OpenAiBaseUrlHint).c_str()));
        }
        if (HWND key_edit = ::GetDlgItem(dlg, IDC_API_KEY)) {
            ::SendMessageW(key_edit, EM_SETCUEBANNER, TRUE,
                           reinterpret_cast<LPARAM>(
                               I18n::Get(StringId::OpenAiApiKeyHint).c_str()));
        }
        SetCtrlText(dlg, IDC_BASE_URL, ToUtf16(st->cfg->base_url));
        SetCtrlText(dlg, IDC_MODEL_COMBO, ToUtf16(st->cfg->model));
        // 261121_0001 U-H (contract C2): seed the reasoning combo from the
        // saved value. No fetch has completed yet, so no model advertises any
        // effort and the standard fallback list applies; a completed fetch
        // repopulates this combo from the selected model's own list (see the
        // kOpenAiMsgFetchDone handler).
        // Fix round 1 (F-4): a saved value OUTSIDE that fallback list (e.g.
        // OpenRouter's "xhigh") is only pending at this point — it is restored
        // the moment a catalog advertises it, and it survives to the save path
        // even if none does.
        st->reasoning_effort = st->cfg->reasoning_effort;
        st->reasoning_effort_pending = !st->reasoning_effort.empty();
        RefreshReasoningCombo(dlg, st);
        // REQ-050 (corrupt-pair recovery): upgraders from the pre-REQ-050
        // build carry api_key_sha256 = SHA-256 of a ZEROED buffer (the old
        // dialog hashed after ProtectOpenAiApiKey scrubbed the cleartext), so
        // the masked-placeholder path would keep reusing a pair
        // WithUnprotectedKey refuses forever (live: OPENAI/WithUnprotectedKey/
        // 002). Verify the persisted pair headlessly at dialog open; on
        // mismatch — or an unprotect failure, which is equally unusable —
        // clear BOTH fields in this in-memory copy so the user is forced to
        // re-enter the key once and the digest-before-protect save path
        // stores a valid pair. Shape-only: no key material is logged.
        if (!st->cfg->api_key_dpapi.empty() && !OpenAiKeyPairIntegrityOk(*st->cfg)) {
            st->cfg->api_key_dpapi.clear();
            st->cfg->api_key_sha256.clear();
            DIAG_F("UI/OpenAiSettings/004: persisted API-key pair failed the integrity "
                   "check; fields cleared for re-entry\n");
        }
        if (!st->cfg->api_key_dpapi.empty()) {
            std::string key;
            if (UnprotectOpenAiApiKey(st->cfg->api_key_dpapi, key)) {
                SetCtrlText(dlg, IDC_API_KEY, ToUtf16(MaskOpenAiApiKey(key)));
                SecureZeroMemory(key.data(), key.size());
            }
        }
        ::SetDlgItemTextW(dlg, IDC_MASKED_LABEL,
                          I18n::Get(StringId::OpenAiKeyMasked).c_str());
        // 261116_0003 P9c-1b: populate the declaration checklist from the
        // UNFILTERED OpenAI-capable registry rows — NEVER the declaration-
        // filtered OpenAI engine view (P9c-1a), which would hide every
        // unchecked language from the list (the P9c-1a §6 handoff hazard).
        // The declared entries are
        // pre-selected via the published-snapshot read seam (empty set =
        // nothing selected = the documented fail-open; the hint line states
        // it). The display name reuses I18n::GetLanguageDisplayName, so the
        // checklist adds no language-name strings of its own.
        if (HWND declare_list = ::GetDlgItem(dlg, IDC_DECLARE_LIST)) {
            st->declare_rows = OpenAiDeclarationRowCodes();
            for (const std::string& code : st->declare_rows) {
                const int idx = static_cast<int>(::SendMessageW(
                    declare_list, LB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(
                        I18n::GetLanguageDisplayName(code).c_str())));
                if (idx >= 0 && OpenAiLanguageDeclared(code)) {
                    ::SendMessageW(declare_list, LB_SETSEL, TRUE,
                                   static_cast<LPARAM>(idx));
                    st->declared_in.push_back(code);
                }
            }
        }
        // REQ-050 (auto-fetch): reopening a saved dialog repopulates the
        // model combo — both values already present (https base URL + a
        // usable key) arms the same debounce timer the EN_CHANGE handler
        // uses, so the list arrives without a button click.
        if (ClassifyOpenAiBaseUrl(ToUtf8(GetCtrlText(dlg, IDC_BASE_URL))) ==
                OpenAiUrlSecurity::Https &&
            KeyAvailableForFetch(dlg, st->cfg)) {
            ::SetTimer(dlg, kFetchTimerId, kFetchDebounceMs, nullptr);
        }
        // 261122_0001 B-U4 (design item 9.2): list the working library's
        // display names in the profile picker, with NO selection — the dialog
        // always opens on the active block above, never on a stored profile, so
        // an accidental [삭제] cannot claim a profile the user never picked.
        RefillOpenAiProfileCombo(dlg, st, st->profiles.size());
        return TRUE;
    }
    case WM_TIMER:
        // REQ-050 (auto-fetch) / REQ-052 (async): debounce timer expired —
        // kill it BEFORE spawning (the kill also cancels any timer an
        // in-flight keystroke batch re-armed) and start the ASYNC fetch:
        // the network call runs on a worker thread now, so the GUI thread
        // no longer blocks on the bounded budget mid-typing (the audit P0
        // '응답 없음' freeze). fetch_manual = false keeps this completion on
        // the silent auto path (never a result notice, per REQ-050).
        if (wp == kFetchTimerId) {
            ::KillTimer(dlg, kFetchTimerId);
            st->fetch_manual = false;
            // 260922_0001 A6: the auto-fetch shares the manual path's busy
            // feedback — a successful spawn grays the button and its
            // completion restores it (DoFetchModels owns both transitions).
            DoFetchModels(dlg, st);
            return TRUE;
        }
        return FALSE;
    case WM_COMMAND:
        // 261121_0001 U-H (contract C4): the reasoning list belongs to the
        // selected model, so a model change repopulates it. CBN_SELCHANGE is
        // the pick from the fetched list; CBN_EDITCHANGE covers the combo's
        // free-text leg (CBS_DROPDOWN), where a hand-typed id is by definition
        // absent from every fetched list and therefore gets the fallback list.
        if (st && LOWORD(wp) == IDC_MODEL_COMBO &&
            (HIWORD(wp) == CBN_SELCHANGE || HIWORD(wp) == CBN_EDITCHANGE)) {
            RefreshReasoningCombo(dlg, st);
            return TRUE;
        }
        // Fix round 1 (F-4): a pick in the reasoning combo is the user's explicit
        // choice, so it ends the pending restoration — including a deliberate
        // pick of the server-default item, which the old code could not
        // distinguish from a fallback.
        if (st && LOWORD(wp) == IDC_REASONING_COMBO && HIWORD(wp) == CBN_SELCHANGE) {
            st->reasoning_effort = ReasoningEffortSelection(dlg, st);
            st->reasoning_effort_pending = false;
            return TRUE;
        }
        // 261122_0001 B-U4 (design item 9.3): a pick in the profile picker
        // refills the form from that profile's cfg (key pair / base URL /
        // model / reasoning effort / consent — see ApplyOpenAiProfileSelection).
        // The picker is a closed list, so this is the ONLY way a profile is
        // ever chosen; a CBN_SELCHANGE with no current selection (raised by the
        // CB_RESETCONTENT of a refill) is ignored by the helper.
        if (st && LOWORD(wp) == IDC_PROFILE_COMBO && HIWORD(wp) == CBN_SELCHANGE) {
            ApplyOpenAiProfileSelection(dlg, st);
            return TRUE;
        }
        // REQ-050 (auto-fetch): debounce model-list fetches while the user
        // types. https-only — an http:// base must NEVER auto-fetch (the
        // consent question may appear only on explicit button/OK actions,
        // never as a popup mid-typing), so the timer is killed whenever the
        // base URL drops out of the https shape or no key is available.
        if (HIWORD(wp) == EN_CHANGE &&
            (LOWORD(wp) == IDC_BASE_URL || LOWORD(wp) == IDC_API_KEY)) {
            if (ClassifyOpenAiBaseUrl(ToUtf8(GetCtrlText(dlg, IDC_BASE_URL))) ==
                    OpenAiUrlSecurity::Https &&
                KeyAvailableForFetch(dlg, st->cfg)) {
                ::SetTimer(dlg, kFetchTimerId, kFetchDebounceMs, nullptr);
            } else {
                ::KillTimer(dlg, kFetchTimerId);
            }
            return TRUE;
        }
        switch (LOWORD(wp)) {
        case IDC_FETCH_BTN: {
            // REQ-050/REQ-052: the fetch (probe + consent gate + worker
            // spawn) lives in DoFetchModels(dlg, st), shared with the
            // debounced auto-fetch; fetch_manual marks this completion as
            // button-originated so the completion handler may show the
            // failure notice the silent auto path must never show. A
            // DECLINED fetch (http consent denied) or a failed spawn shows
            // the notice immediately — the exact pre-REQ-052 contract.
            // 260922_0001 A6: the successful spawn inside DoFetchModels
            // already disabled the button (busy feedback); its false return
            // (declined consent / failed spawn) leaves the button enabled
            // and shows the notice below — the pre-REQ-052 contract.
            st->fetch_manual = true;
            if (!DoFetchModels(dlg, st)) {
                ::MessageBoxW(dlg, I18n::Get(StringId::OpenAiFetchFailed).c_str(),
                              I18n::Get(StringId::OpenAiSettingsTitle).c_str(),
                              MB_OK | MB_ICONINFORMATION);
            }
            return TRUE;
        }
        case IDOK: {
            OpenAiConfig out;
            out.base_url = ToUtf8(GetCtrlText(dlg, IDC_BASE_URL));
            out.model = ToUtf8(GetCtrlText(dlg, IDC_MODEL_COMBO));
            // 261121_0001 U-H (contract C2): the reasoning combo's selected item
            // IS the persisted value; the server-default item is "", which the
            // request body renders by omitting the key entirely. Read from the
            // control here rather than from a remembered flag, so what the user
            // sees on screen is exactly what gets saved.
            out.reasoning_effort = ReasoningEffortSelection(dlg, st);
            if (st->reasoning_effort_pending) {
                // Fix round 1 (F-4): the combo shows the server-default item
                // because the current list cannot carry the saved value (e.g.
                // OpenRouter's "xhigh" against the pre-fetch fallback list). The
                // saved value is still the user's choice, so it is what this
                // dialog persists (the client's HTTP-400 retry covers a model
                // that rejects it). An explicit pick clears the flag in the
                // CBN_SELCHANGE handler above, so this arm only fires for a
                // value the user never touched.
                out.reasoning_effort = st->reasoning_effort;
            }
            std::string keyUtf8 = ToUtf8(GetCtrlText(dlg, IDC_API_KEY));

            const OpenAiUrlSecurity sec = ClassifyOpenAiBaseUrl(out.base_url);
            if (sec == OpenAiUrlSecurity::Invalid) {
                ::MessageBoxW(dlg, I18n::Get(StringId::OpenAiInvalidBaseUrl).c_str(),
                              I18n::Get(StringId::OpenAiSettingsTitle).c_str(),
                              MB_OK | MB_ICONWARNING);
                SecureZeroMemory(keyUtf8.data(), keyUtf8.size());
                return TRUE;
            }
            if (sec == OpenAiUrlSecurity::Http && !st->cfg->http_consent_given) {
                const int rc = ::MessageBoxW(
                    dlg, I18n::Get(StringId::OpenAiHttpWarningBody).c_str(),
                    I18n::Get(StringId::OpenAiHttpWarningTitle).c_str(),
                    MB_YESNO | MB_ICONWARNING);
                if (rc != IDYES) {
                    SecureZeroMemory(keyUtf8.data(), keyUtf8.size());
                    return TRUE;
                }
                out.http_consent_given = true;
            } else {
                out.http_consent_given = st->cfg->http_consent_given;
            }

            if (IsMaskedPlaceholder(keyUtf8)) {
                out.api_key_dpapi = st->cfg->api_key_dpapi;
                out.api_key_sha256 = st->cfg->api_key_sha256;
            } else if (!keyUtf8.empty()) {
                // REQ-050: digest BEFORE protect — ProtectOpenAiApiKey scrubs
                // the caller's cleartext buffer in place (SecureZeroMemory),
                // so hashing after it persisted SHA-256(zero buffer);
                // WithUnprotectedKey re-hashes the real decrypted key, the
                // comparison never matched, and every saved key was refused
                // (live: OPENAI/WithUnprotectedKey/002). On protect failure
                // the already-computed digest is discarded with `out`.
                OpenAiSha256Hex(keyUtf8, out.api_key_sha256);
                if (!ProtectOpenAiApiKey(keyUtf8, out.api_key_dpapi)) {
                    SecureZeroMemory(keyUtf8.data(), keyUtf8.size());
                    ::MessageBoxW(dlg, I18n::Get(StringId::OpenAiFetchFailed).c_str(),
                                  I18n::Get(StringId::OpenAiSettingsTitle).c_str(),
                                  MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
            }
            SecureZeroMemory(keyUtf8.data(), keyUtf8.size());

            // 261116_0003 P9c-1b: fold the declaration checklist selection
            // back through the SAME unfiltered row list (listbox index ==
            // row index, which is why the template list must never sort).
            // 261116_0003 P9c-1c: the dialog applies NOTHING here. The folded
            // set rides out through the declaration-aware entry and the
            // OWNING config applies it through the locked declaration verb
            // before its own save, so exactly one writer and one save path
            // exist (the P9c-1b second-instance read-modify-write was
            // reverted by that owner's later save, which still carried the
            // pre-dialog set). The fold stays canonical (registry order), so
            // the verb's canonicalizer is an identity on it.
            {
                std::vector<int> selected;
                if (HWND declare_list = ::GetDlgItem(dlg, IDC_DECLARE_LIST)) {
                    const int sel_count = static_cast<int>(
                        ::SendMessageW(declare_list, LB_GETSELCOUNT, 0, 0));
                    if (sel_count > 0) {
                        selected.resize(static_cast<std::size_t>(sel_count));
                        ::SendMessageW(
                            declare_list, LB_GETSELITEMS,
                            static_cast<WPARAM>(sel_count),
                            reinterpret_cast<LPARAM>(selected.data()));
                    }
                }
                st->declared_out =
                    OpenAiDeclarationFromSelection(selected, st->declare_rows);
                st->declaration_edited = true;
            }

            *st->cfg = std::move(out);
            st->saved = true;
            ::EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            ::EndDialog(dlg, IDCANCEL);
            return TRUE;
        case IDC_DELETE_BTN: {
            // REQ-051 (Symptom D, decisions.md 260921 17:08 — user-frozen
            // scope): clear EXACTLY the saved OpenAI settings (base_url,
            // model, 261121_0001 U-H reasoning_effort, the
            // api_key_dpapi+api_key_sha256 pair TOGETHER so a
            // half-cleared pair can never reach the refuse-forever
            // WithUnprotectedKey state, http_consent_given) and persist,
            // leaving the engine selection untouched. The persist is a
            // read-modify-write of the canonical config.json through a
            // SECOND AppConfig instance: the dialog only ever receives the
            // openai block, so file-level persistence is the only save path
            // that cannot touch engine_type. On success the in/out copy is
            // cleared too and the dialog ends with a NON-IDOK code (kDeleted
            // outcome) so pre-REQ-051 callers — main.cpp
            // kMsgOpenOpenAiSettings via the bool wrapper — see plain cancel
            // semantics (refresh_tray only, NO engine switch, no persisted
            // engine_type="openai"). main.cpp's in-memory AppConfig::openai
            // is reconciled by the caller adopting ShowOpenAiSettingsDialogEx
            // (see the REQ-051 handoff); until then the file is correct and
            // any later in-memory save is the documented integration gap.
            // Fail-closed: a load/save failure keeps the dialog open with
            // every field intact — nothing is ever half-cleared.
            // 261122_0001 B-U4 (design item 9.8): the STAGED PROFILE LIBRARY
            // edits die with this button. It ends the dialog with a NON-IDOK
            // code, so the profile fold reports edited = false and the caller
            // keeps its stored library — only the ACTIVE block's settings are
            // deleted here, and a half-applied profile stage can never leak out
            // through this path.
            // 261122_0001 fix-A2 (R1-01): the STORED library is pruned instead
            // — identity-scoped, never wholesale. The load above just
            // re-synthesized this block's migration mirror (config.cpp's
            // absent-key synthesis), so clearing only the block and saving
            // would persist the very key pair this button exists to destroy,
            // one profile-pick away from re-adoption. Capture the identity
            // BEFORE the clear, then drop every entry carrying it.
            AppConfig persisted;
            if (!persisted.LoadFromFile()) {
                DIAG_F("UI/OpenAiSettings/005: delete requested but config load failed; "
                       "settings left intact\n");
                return TRUE;
            }
            const OpenAiConfig retired = persisted.openai;
            ClearOpenAiSettings(persisted.openai);
            std::vector<OpenAiProfile> kept_profiles = persisted.openai_profiles;
            const std::size_t retired_count =
                EraseOpenAiProfileMatchingKey(kept_profiles, retired);
            if (retired_count > 0) {
                // The locked verb, never a direct member write (R1-02): it also
                // marks the fold touched, so an emptied library persists as an
                // explicit [] and the migration cannot re-synthesize later.
                persisted.SetOpenAiProfiles(std::move(kept_profiles));
            }
            if (!persisted.SaveToFile()) {
                DIAG_F("UI/OpenAiSettings/006: delete requested but config save failed; "
                       "settings left intact\n");
                return TRUE;
            }
            DIAG_F("UI/OpenAiSettings/007: saved OpenAI settings deleted "
                   "(base_url/model/reasoning effort/key pair/consent cleared; "
                   "%zu stored profile(s) carrying the retired key removed; engine "
                   "selection untouched)\n",
                   retired_count);
            *st->cfg = OpenAiConfig{};
            st->deleted = true;
            ::EndDialog(dlg, IDC_DELETE_BTN);
            return TRUE;
        }
        case IDC_PROFILE_SAVE_BTN: {
            // 261122_0001 B-U4 (design item 9.4): [저장] upserts the CURRENT
            // form into the working library. It performs NO config file I/O
            // (single writer: the caller persists the folded library) and it
            // does NOT touch the active `openai` output — that is still IDOK's
            // on-screen fields (REQ-051 semantics unchanged).
            OpenAiConfig candidate;
            candidate.base_url = ToUtf8(GetCtrlText(dlg, IDC_BASE_URL));
            candidate.model = ToUtf8(GetCtrlText(dlg, IDC_MODEL_COMBO));
            // The same read IDOK performs: the reasoning combo's selected item
            // IS the persisted value, and the F-4 pending value wins while no
            // list can show it (the user's saved choice must survive a save).
            candidate.reasoning_effort = ReasoningEffortSelection(dlg, st);
            if (st->reasoning_effort_pending) {
                candidate.reasoning_effort = st->reasoning_effort;
            }
            candidate.http_consent_given = st->cfg->http_consent_given;
            // Key matrix (design item 9.4): a masked placeholder keeps the pair
            // the dialog carries, a typed key is digested BEFORE protect and
            // scrubbed in place, an empty edit stores no pair. No cleartext
            // leaves this block.
            std::string keyUtf8 = ToUtf8(GetCtrlText(dlg, IDC_API_KEY));
            const bool key_ok = ResolveOpenAiProfileKeyPair(keyUtf8, *st->cfg, candidate);
            SecureZeroMemory(keyUtf8.data(), keyUtf8.size());
            if (!key_ok) {
                // ProtectOpenAiApiKey failed: nothing is staged (the plan below
                // never runs), so the working library is exactly as it was.
                ::MessageBoxW(dlg, I18n::Get(StringId::OpenAiFetchFailed).c_str(),
                              I18n::Get(StringId::OpenAiSettingsTitle).c_str(),
                              MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            std::vector<OpenAiProfile> updated_library;
            std::size_t saved_index = 0;
            const bool saved_applied = PlanOpenAiProfileSave(
                st->profiles, candidate,
                ToUtf8(I18n::Get(StringId::OpenAiProfileNameTemplate)), updated_library,
                saved_index);
            if (!saved_applied) {
                // The ONLY refusal reason is the cap: no identity match and the
                // library already holds kMaxOpenAiProfiles. Refuse, say why (the
                // cap number substituted into the localized notice), and modify
                // NOTHING — `updated_library` was the untouched copy, so the
                // working library stays byte-identical.
                const std::wstring notice = FormatOpenAiProfileLimitNotice(
                    I18n::Get(StringId::OpenAiProfileLimit), kMaxOpenAiProfiles);
                ::MessageBoxW(dlg, notice.c_str(),
                              I18n::Get(StringId::OpenAiSettingsTitle).c_str(),
                              MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            st->profiles = std::move(updated_library);
            st->profiles_edited = true;
            // Keep the just-saved entry SELECTED: saved_index is the upsert match
            // or the appended entry, so the picker shows what [저장] just wrote.
            RefillOpenAiProfileCombo(dlg, st, saved_index);
            DIAG_F("UI/OpenAiSettings/012: OpenAI profile saved (index %lu, library %lu)\n",
                   static_cast<unsigned long>(saved_index),
                   static_cast<unsigned long>(st->profiles.size()));
            return TRUE;
        }
        case IDC_PROFILE_DEL_BTN: {
            // 261122_0001 B-U4 (design item 9.5): [삭제] removes the SELECTED
            // profile from the WORKING library after an explicit confirmation
            // (the prompt states that the stored key goes with it). No
            // selection = nothing to delete = a silent no-op; the dialog never
            // guesses "the first profile". The stored DPAPI pair dies with the
            // entry — this path never held cleartext, so there is nothing to
            // scrub, and the caller's stored library is untouched until it
            // adopts the fold.
            HWND profile_combo = ::GetDlgItem(dlg, IDC_PROFILE_COMBO);
            const LRESULT sel =
                profile_combo ? ::SendMessageW(profile_combo, CB_GETCURSEL, 0, 0) : CB_ERR;
            if (sel < 0 || static_cast<std::size_t>(sel) >= st->profiles.size()) {
                return TRUE;
            }
            if (::MessageBoxW(dlg, I18n::Get(StringId::OpenAiProfileDeleteConfirm).c_str(),
                              I18n::Get(StringId::OpenAiSettingsTitle).c_str(),
                              MB_YESNO | MB_ICONWARNING) != IDYES) {
                return TRUE;
            }
            st->profiles.erase(st->profiles.begin() + sel);
            st->profiles_edited = true;
            // Rebuild and CLEAR the selection: the row the user just deleted
            // must not stay highlighted (a follow-up [삭제] would target the
            // entry that slid into its index).
            RefillOpenAiProfileCombo(dlg, st, st->profiles.size());
            DIAG_F("UI/OpenAiSettings/014: OpenAI profile deleted (library %lu)\n",
                   static_cast<unsigned long>(st->profiles.size()));
            return TRUE;
        }
        }
        return FALSE;
    case kOpenAiMsgFetchDone: {
        // REQ-052: a fetch worker finished its bounded network call.
        // R1-9 (v0.10.4, CWE-822 fix): lp is IGNORED — it is not read, not
        // cast, not dereferenced. The completion payload lives in
        // g_openai_fetch_queue BY VALUE, so a message this process did not
        // enqueue (any same-session peer can post the id) drains nothing.
        // Drain one entry at a time under the lock so a result enqueued while
        // an earlier one is being applied is consumed by this same wake-up,
        // and apply the single-flight generation filter to the DRAINED VALUE —
        // never to memory the message named.
        for (;;) {
            OpenAiFetchResult result{};
            {
                std::lock_guard<std::mutex> lk(g_openai_fetch_mu);
                if (g_openai_fetch_queue.empty()) {
                    break;
                }
                result = std::move(g_openai_fetch_queue.front());
                g_openai_fetch_queue.pop_front();
            }
            if (!st) {
                // Pre-WM_INITDIALOG window: no state to apply and no button to
                // restore (it was never disabled). The value dies here.
                continue;
            }
            if (result.generation != st->fetch_generation) {
                // 260922_0001 A6: this exit restores the button too — a
                // superseded (stale) fetch must not leave it gray while the
                // newer worker it yielded to is still running.
                st->fetch_button_busy = false;
                ::EnableWindow(::GetDlgItem(dlg, IDC_FETCH_BTN), TRUE);
                continue; // stale (a newer fetch superseded it) or orphaned
            }
            const bool manual = result.manual;
            const bool fetch_empty = result.catalog.models.empty();
            // 260922_0001 A6: the in-flight fetch is done — restore the button
            // BEFORE the refill/notice below so the failure box never sits over
            // a still-gray button.
            st->fetch_button_busy = false;
            ::EnableWindow(::GetDlgItem(dlg, IDC_FETCH_BTN), TRUE);
            // Same post-success logic as before REQ-052 (RefillOpenAiModelCombo
            // is the extracted inline path): an empty fetch leaves the combo
            // untouched so the typed/current model stays visible; the manual
            // button keeps its failure notice, the silent auto path stays
            // silent.
            RefillOpenAiModelCombo(dlg, result.catalog.models);
            // 261121_0001 U-H (contract C4): adopt the capabilities the SAME GET
            // returned and re-derive the reasoning list for whatever model the
            // refill left selected. An empty result changes nothing (no
            // capabilities to learn from) — the "fetch failed, keep what the
            // dialog showed" rule above, applied to the reasoning row too.
            if (!fetch_empty) {
                st->catalog = std::move(result.catalog);
                RefreshReasoningCombo(dlg, st);
            }
            if (manual && fetch_empty) {
                ::MessageBoxW(dlg, I18n::Get(StringId::OpenAiFetchFailed).c_str(),
                              I18n::Get(StringId::OpenAiSettingsTitle).c_str(),
                              MB_OK | MB_ICONINFORMATION);
            }
        }
        return TRUE;
    }
    case WM_DESTROY:
        // REQ-052: a detached fetch worker may still be inside its bounded
        // network call when the dialog closes. The worker never touches
        // this state — it owns only its own result value and the hwnd value —
        // so there is nothing to join; flipping the liveness flag tells a
        // finishing worker the receiver is gone, so it drops its value instead
        // of posting to a destroyed hwnd.
        if (st && st->fetch_alive) {
            st->fetch_alive->store(false, std::memory_order_release);
        }
        // R1-9 teardown sweep: discard every result still queued (a superseded
        // fetch, or one whose wake-up will never be dispatched) — REMOVE
        // without applying, mirroring the tooltip DrainMarshalQueue contract.
        // Nothing heap-allocated exists on this path, so nothing can leak past
        // the dialog.
        DrainOpenAiFetchQueue();
        return TRUE;
    }
    return FALSE;
}

} // namespace

// REQ-051 (Symptom C): pick-or-insert plan for the model-combo refill. Pure
// (the combo is stateless w.r.t. this helper), so the unit suite drives the
// exact algorithm the refill applies. Model ids are case-sensitive, so the
// match is exact and case-sensitive; the first occurrence wins. An empty
// target never selects or inserts — the combo is left untouched rather than
// fabricating a selection the user never made.
OpenAiComboSelection PlanOpenAiComboSelection(
    const std::vector<std::pair<int, std::string>>& items,
    const std::string& target) {
    if (target.empty()) return {-1, false};
    for (const auto& [index, text] : items) {
        if (text == target) return {index, false};
    }
    return {items.empty() ? 0 : items.back().first + 1, true};
}

// REQ-051 (Symptom D): clears exactly the persisted OpenAI settings fields.
// The key pair (DPAPI blob + SHA-256 digest) is wiped TOGETHER: a half-cleared
// pair is what made WithUnprotectedKey refuse a valid key forever (REQ-050
// b6f98da contract). engine selection is untouched — OpenAiConfig carries no
// engine field. 261121_0001 U-H (contract C2): the reasoning effort is one of
// those settings, so a delete must clear it too — otherwise the next save would
// resurrect an effort the user just wiped along with the endpoint that
// advertised it.
void ClearOpenAiSettings(OpenAiConfig& cfg) {
    cfg.base_url.clear();
    cfg.model.clear();
    cfg.reasoning_effort.clear();
    cfg.api_key_dpapi.clear();
    cfg.api_key_sha256.clear();
    cfg.http_consent_given = false;
}

// REQ-050 (corrupt-pair recovery): headless integrity check of a persisted
// key pair — unprotects the DPAPI blob, re-hashes the cleartext, and compares
// against the persisted digest. The cleartext is zeroed before return; it is
// never logged. Returns false on ANY failure or mismatch, including an
// empty/missing digest, so a pair this app cannot vouch for never reaches the
// masked-placeholder reuse path (IDC_FETCH_BTN / IDOK). Namespace scope (not
// the anonymous block above) so the unit suite exercises the exact helper the
// dialog proc calls.
bool OpenAiKeyPairIntegrityOk(const OpenAiConfig& cfg) {
    if (cfg.api_key_dpapi.empty()) return false;
    std::string key;
    std::string digest;
    const bool ok = UnprotectOpenAiApiKey(cfg.api_key_dpapi, key) &&
                    OpenAiSha256Hex(key, digest) &&
                    digest == cfg.api_key_sha256;
    SecureZeroMemory(key.data(), key.size());
    return ok;
}

// ---- In-memory dialog template builder ----
// REQ-046 P4-3 (Tech Gate 필수-5): class declaration moved to
// openai_settings_window.hpp (public emebalachat symbol) so the unit suite can
// pin the template style bits without entering the modal loop. The method
// implementations stay here. Serializes a DLGTEMPLATE + DLGITEMTEMPLATEs into
// a byte buffer with correct 4-byte alignment (per the Win32 dialog-template
// contract).
void TemplateBuilder::Begin(std::wstring_view title, short w, short h, WORD itemCount) {
    // Pad to a 4-byte boundary before the (DWORD-aligned) DLGTEMPLATE.
    while (buf_.size() % 4 != 0) buf_.push_back(0);
    dtpl_ = buf_.size();
    // REQ-046 P4-3 (Rev2 §C, Tech Gate 권고-6): WS_VISIBLE makes the dialog
    // actually show (without it the dialog came up as a minimized/invisible
    // shell — the reported "최소화된 창" symptom); DS_CENTER centers it on the
    // owner screen; DS_SETFOREGROUND forces foreground so it does not open
    // behind other windows.
    EmitDword(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT | DS_MODALFRAME |
              WS_VISIBLE | DS_CENTER | DS_SETFOREGROUND);
    EmitDword(0);                       // dwExtendedStyle
    // REQ-048 P2: cdit must follow dwExtendedStyle — the Win32 DLGTEMPLATE
    // contract orders the header style, dwExtendedStyle, cdit, x, y, cx, cy.
    // The pre-P2 layout emitted x,y,cx,cy first, so the loader read cx as the
    // item count and cdit as cy (an 8-DLU caption strip with zero parsed
    // items — the byte-level root cause of the device symptom "title + X
    // only, no input fields").
    EmitWord(itemCount);                // cdit
    EmitWord(0); EmitWord(0);           // x, y
    EmitWord(w); EmitWord(h);           // cx, cy
    EmitWord(0);                        // menu (none)
    EmitWord(0);                        // windowClass (none)
    EmitStr(title);
    EmitWord(9);                        // pointsize
    EmitStr(L"Segoe UI");
}

void TemplateBuilder::AddItem(DWORD style, short x, short y, short cx, short cy,
                              WORD id, WORD clsAtom, std::wstring_view text) {
    // Each DLGITEMTEMPLATE must be 4-byte aligned relative to the start
    // of the DLGTEMPLATE.
    while ((buf_.size() - dtpl_) % 4 != 0) buf_.push_back(0);
    EmitDword(style);
    EmitDword(0);                       // dwExtendedStyle
    EmitWord(x); EmitWord(y); EmitWord(cx); EmitWord(cy);
    EmitWord(id);
    // REQ-048 P2: the fixed DLGITEMTEMPLATE part ends at id (18 bytes total:
    // style 4 + exstyle 4 + x/y/cx/cy 8 + id 2); the class array follows
    // immediately. A leading 0xFFFF WORD marks the next WORD as a system-
    // class ordinal (0x0080..0x0085). The pre-P2 fake pad WORD after id and
    // the missing prefix desynced the parser walk, so every control after
    // the first was misparsed.
    EmitWord(0xFFFF);
    EmitWord(clsAtom);
    EmitStr(text);
    EmitWord(0);                        // extraData count = 0
}

const DLGTEMPLATE* TemplateBuilder::Get() const {
    return reinterpret_cast<const DLGTEMPLATE*>(buf_.data() + dtpl_);
}

// REQ-048 P2: whole-buffer byte count. For a freshly constructed builder
// dtpl_ == 0, so size() is exactly the serialized template length the test
// byte-walk consumes to.
size_t TemplateBuilder::size() const { return buf_.size(); }

void TemplateBuilder::EmitWord(WORD w) {
    buf_.push_back(static_cast<BYTE>(w & 0xFF));
    buf_.push_back(static_cast<BYTE>((w >> 8) & 0xFF));
}

void TemplateBuilder::EmitDword(DWORD d) { EmitWord(LOWORD(d)); EmitWord(HIWORD(d)); }

void TemplateBuilder::EmitStr(std::wstring_view s) {
    for (wchar_t ch : s) EmitWord(static_cast<WORD>(ch));
    EmitWord(0);
}

// REQ-048 P2: production item-set emission, split out of
// ShowOpenAiSettingsDialog so the unit suite serializes the exact bytes the
// dialog hands to DialogBoxIndirectParamW. The control IDs (IDC_*) live in
// the anonymous namespace above; a namespace-scope definition in this same
// TU can reference them. itemCount must match the AddItem calls below.
// 261122_0001 B-U3 (design-pick.md items 5 + 8a): the template is REBUILT in
// the binding B1 row order — profile row (NEW, top) -> 기본 URL -> API 키 ->
// masked 라벨 -> 모델 + [모델 목록 가져오기] -> 추론강도 -> 언어 선언 섹션
// (타이틀/리스트/힌트) -> 액션 행 ([설정 삭제]/OK/Cancel) — 16 -> 20 items
// (the 4 profile-row controls), shell 262x190 -> 396x208 DLU.
// GEOMETRY (design item 8a, OFFLINE measurement — runtime measured placement
// stays forbidden; the fixed DLU coords are the house invariant): the
// tools_tmp_261122_bu3/measure_labels utility walked ALL 89 UI-locale tables
// through the REAL I18n::Get strings under the template's dialog font (9 pt
// Segoe UI, GetTextExtentPoint32W, DLU = px*4/tmAveCharWidth) and the shell /
// columns are sized to the worst offender + margin in every locale:
//   * label column x=8 w=114 — worst label = OpenAiReasoningLabel @ ta 110
//     DLU (fits w-4);
//   * edit column x=130 w=354 — worst combo item =
//     OpenAiReasoningServerDefault @ bg 194 DLU (fits w-4) and the model combo
//     shares the row with the fetch button;
//   * fetch button w=126 — OpenAiFetchModels @ ml 118 DLU (+8 margin);
//   * profile save/delete buttons w=74/68 — OpenAiProfileSave @ ml 66 /
//     OpenAiProfileDelete @ ml 60 (+8 margin: both buttons are at their
//     measured minimum, so no width can be reclaimed from them);
//   * [설정 삭제] w=142 — OpenAiDeleteSettings @ ml 134 (+8 margin);
//   * OK w=64 (DialogOk @ ru 47 +8), Cancel w=70 (DialogCancel @ si 61 +8);
//   * masked label / declaration title / hint span x=8 w=476 (full width;
//     the hint greedily wraps to <= 2 lines — worst vi — and 2 lines fit
//     cy=18).
// 261122_0001 B-U6fix (design items 1 + 4c; the U6 89-locale hidden-window
// probe's single defect — 89 clip rows, all the closed profile picker): the
// profile picker's closed box was 108 DLU = 189 px, far below the 341 px the
// REALISTIC worst derived profile name measures at (a host at the 24-byte
// OpenAiHostAbbrev cap + " / " + a model id at 32 bytes). The row's buttons
// were ALREADY at their measured minimum (74/68 = the widest locale + 8), so
// the only axis left is the shell: 396 -> 492 DLU, of which the profile combo
// takes 204 DLU. Measurement forced the growth (design item 1's escape hatch);
// every other row tracks the new content right edge (484) so no dead strip or
// floating OK/Cancel row is left behind. The combo was sized in the
// DS_SETFONT-faithful DLU base (7 px at 96 DPI, validated against the live
// dialog: 693 px client / 396 DLU in the U6 probe) rather than the legacy
// tmAveCharWidth base (6 px) the label pin uses, because the legacy base is
// 14% pessimistic and would cost 28 DLU of dialog width for no visual gain.
// The per-build GDI pin lives in tests/req048_template_tests.inc
// (TestOpenAiLabelFits), and the 89-locale hidden-window probe (U6)
// re-verifies the same numbers against a live dialog.
void BuildOpenAiTemplate(TemplateBuilder& tb) {
    const DWORD LBL = WS_CHILD | WS_VISIBLE;
    const DWORD EDT = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP;
    const DWORD COMBO = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | CBS_DROPDOWN;
    // 261121_0001 U-H: the reasoning picker is a CLOSED list (CBS_DROPDOWNLIST):
    // every item comes from PlanReasoningEffortItems, so a free-text leg could
    // only produce a value the model does not accept (a hard HTTP 400).
    // 261122_0001 B-U3: the profile picker is the same closed idiom — a
    // profile is PICKED from the library, never typed (U4 wires the list).
    // Styles: WS_CHILD|WS_VISIBLE|WS_BORDER|WS_TABSTOP = 0x50810000,
    // +CBS_DROPDOWNLIST (0x0003) = 0x50810003 (same idiom as the REQ-060 mode
    // combo).
    const DWORD COMBO_LIST = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP |
                             CBS_DROPDOWNLIST;
    const DWORD BTN = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
    // 261116_0003 P9c-1b: the declaration checklist's multi-select LISTBOX.
    // LBS_MULTIPLESEL is the standard Win32 multi-select idiom; LBS_NOTIFY +
    // LBS_HASSTRINGS accompany it. The list NEVER sorts: the listbox index IS
    // the row index of the unfiltered registry-order fold (a sorted list
    // would desync the IDOK fold), and the declaration's canonical form is
    // registry order. Styles:
    // WS_CHILD|WS_VISIBLE|WS_BORDER|WS_VSCROLL|WS_TABSTOP = 0x50A10000,
    // +LBS_HASSTRINGS(0x40) +LBS_MULTIPLESEL(0x08) +LBS_NOTIFY(0x01) =
    // 0x50A10049.
    const DWORD LIST = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
                       WS_TABSTOP | LBS_HASSTRINGS | LBS_MULTIPLESEL | LBS_NOTIFY;
    const WORD STATIC_CLS = 0x0082;  // "STATIC"
    const WORD EDIT_CLS   = 0x0081;  // "EDIT"
    const WORD BTN_CLS    = 0x0080;  // "BUTTON"
    const WORD COMBO_CLS  = 0x0085;  // "COMBOBOX"
    const WORD LISTBOX_CLS = 0x0083; // "LISTBOX" (261116_0003 P9c-1b)

    // 261122_0001 B-U3 (B1 row 1): the profile row — label, closed picker,
    // [저장], [삭제]. The buttons are right-aligned to the content edge
    // (484 = shell 492 - 8) and the combo fills the gap; U4 gives the
    // buttons their behavior, the captions are i18n NOW.
    //
    // 261122_0001 B-U6fix (design item 3): the CLOSED-box display contract.
    // A CBS_DROPDOWNLIST renders the SELECTED item in its closed box and the
    // OS truncates whatever does not fit — that truncation is by design, never
    // a clip of content the user cannot reach. This box is sized (w=204 DLU =
    // 357 px at 96 DPI) to the REALISTIC worst DERIVED name: a host at the
    // 24-byte OpenAiHostAbbrev cap + " / " + a model id at 32 bytes, measured
    // at 341 px / 195 DLU in this template's 9 pt Segoe UI (TestOpenAiLabelFits
    // re-measures and pins it on every build), so every name the [저장] path
    // can derive fits WITHOUT truncation. A LONGER name — the profile-name cap
    // allows 96 bytes, 480 px — truncates in the closed box and is shown in
    // full by the OPENED list: RefillOpenAiProfileCombo always sizes that list
    // to its longest entry (CB_SETDROPPEDWIDTH), the same contract the model
    // combo relies on. Nothing is lost or mis-selected either way.
    tb.AddItem(LBL, 8, 6, 114, 9, IDC_PROFILE_LABEL, STATIC_CLS,
               I18n::Get(StringId::OpenAiProfileLabel));
    // h=64 is the dropdown extent (the closed box is ~12 DLU), the same
    // convention as IDC_MODEL_COMBO; the popup list is its own window, so it
    // opens upward when the dialog edge leaves no room below.
    tb.AddItem(COMBO_LIST, 130, 5, 204, 64, IDC_PROFILE_COMBO, COMBO_CLS, L"");
    tb.AddItem(BTN, 338, 5, 74, 12, IDC_PROFILE_SAVE_BTN, BTN_CLS,
               I18n::Get(StringId::OpenAiProfileSave));
    tb.AddItem(BTN, 416, 5, 68, 12, IDC_PROFILE_DEL_BTN, BTN_CLS,
               I18n::Get(StringId::OpenAiProfileDelete));

    // B1 row 2: 기본 URL.
    tb.AddItem(LBL, 8, 22, 114, 9, IDC_STATIC_BASE, STATIC_CLS,
               I18n::Get(StringId::OpenAiBaseUrlLabel));
    // REQ-050 (single-line edit limits): single-line EDITs without
    // ES_AUTOHSCROLL reject input beyond their visible width (device-confirmed
    // on the HF add dialog's URL edit, same latent hazard here) — base URLs
    // and API keys are routinely longer than the edit, so both scroll.
    tb.AddItem(EDT | ES_AUTOHSCROLL, 130, 21, 354, 12, IDC_BASE_URL, EDIT_CLS, L"");

    // B1 row 3: API 키.
    tb.AddItem(LBL, 8, 38, 114, 9, IDC_STATIC_KEY, STATIC_CLS,
               I18n::Get(StringId::OpenAiApiKeyLabel));
    tb.AddItem(EDT | ES_AUTOHSCROLL | ES_PASSWORD, 130, 37, 354, 12, IDC_API_KEY,
               EDIT_CLS, L"");

    // B1 row 4: the masked-read-back 라벨. Set at WM_INITDIALOG from
    // StringId::OpenAiKeyMasked (the key itself lands in the password edit
    // above); spans the full content width like the declaration lines below.
    tb.AddItem(LBL, 8, 54, 476, 9, IDC_MASKED_LABEL, STATIC_CLS, L"");

    // B1 row 5: 모델 + [모델 목록 가져오기]. The fetch button is right-aligned
    // (w=126 = the widest locale, ml, + 8) and the combo fills the gap; a
    // hand-typed id longer than the combo scrolls (CBS_DROPDOWN's edit leg).
    tb.AddItem(LBL, 8, 70, 114, 9, IDC_STATIC_MODEL, STATIC_CLS,
               I18n::Get(StringId::OpenAiModelLabel));
    tb.AddItem(COMBO, 130, 69, 224, 64, IDC_MODEL_COMBO, COMBO_CLS, L"");
    tb.AddItem(BTN, 358, 69, 126, 12, IDC_FETCH_BTN, BTN_CLS,
               I18n::Get(StringId::OpenAiFetchModels));

    // B1 row 6: 추론강도. The combo is a CLOSED list whose widest ITEM is the
    // localized server-default label (bg 194 DLU < 354-4), so no locale
    // clips the closed box.
    tb.AddItem(LBL, 8, 86, 114, 9, IDC_REASONING_LABEL, STATIC_CLS,
               I18n::Get(StringId::OpenAiReasoningLabel));
    tb.AddItem(COMBO_LIST, 130, 85, 354, 64, IDC_REASONING_COMBO, COMBO_CLS, L"");

    // B1 row 7: the 언어 선언 섹션 (261116_0003 P9c-1b) — the title, ONE
    // multi-select LISTBOX of every OpenAI-capable language (display name),
    // and the fail-open hint line. The hint wraps to at most two lines in
    // every locale (offline-measured; worst ta fills 616 DLU, so the grown
    // 476-DLU static holds it in two lines with room to spare) and two lines
    // fit cy=18.
    tb.AddItem(LBL, 8, 98, 476, 9, IDC_DECLARE_TITLE, STATIC_CLS,
               I18n::Get(StringId::OpenAiDeclaredTitle));
    tb.AddItem(LIST, 8, 109, 476, 52, IDC_DECLARE_LIST, LISTBOX_CLS, L"");
    tb.AddItem(LBL, 8, 164, 476, 18, IDC_DECLARE_HINT, STATIC_CLS,
               I18n::Get(StringId::OpenAiDeclaredHint));

    // B1 row 8: the 액션 행. REQ-051 (Symptom D): the [설정 삭제] button owns
    // the bottom-LEFT corner, far from OK/Cancel — a destructive action gets
    // its own corner so it cannot be hit by muscle memory aiming at OK.
    // w=142 = the widest locale (ml 134) + 8; OK/Cancel are right-aligned
    // (Cancel w=70 = si 61 + 8; OK keeps 64).
    tb.AddItem(BTN, 8, 186, 142, 13, IDC_DELETE_BTN, BTN_CLS,
               I18n::Get(StringId::OpenAiDeleteSettings));
    // REQ-050: OK/Cancel are real i18n strings (StringId::DialogOk /
    // DialogCancel), not hardcoded English — the locale tables carry the
    // conventional native button label for each locale.
    tb.AddItem(BTN | BS_DEFPUSHBUTTON, 346, 186, 64, 13, IDOK, BTN_CLS,
               I18n::Get(StringId::DialogOk));
    tb.AddItem(BTN, 414, 186, 70, 13, IDCANCEL, BTN_CLS,
               I18n::Get(StringId::DialogCancel));
}

OpenAiSettingsOutcome ShowOpenAiSettingsDialogEx(HWND parent, OpenAiConfig& cfg,
                                                 OpenAiDeclaredLanguagesFold& declaration,
                                                 OpenAiProfilesFold& profiles) {
    // 261116_0003 P9c-1c: `declaration` starts cleared on every entry so a
    // caller that never reaches IDOK (Cancel, [삭제], a failed dialog creation)
    // reads edited=false and an empty set - the "nothing to adopt" contract.
    declaration.edited = false;
    declaration.declared_languages.clear();
    OpenAiDialogState st{&cfg, false, false};
    // 261122_0001 B-U4 (design-pick item 9.1): seed the dialog's WORKING library
    // copy from the fold's INPUT half. A deep copy, deliberately not a move: a
    // caller that hands its live vector over and then cancels must still have
    // its library intact. `edited` starts false and the fold's vector is left
    // alone on entry, so "not edited" always means "the caller's vector is
    // still the truth" (see OpenAiProfilesFold in the header).
    st.profiles = profiles.profiles;
    profiles.edited = false;

    const std::wstring title = I18n::Get(StringId::OpenAiSettingsTitle);
    TemplateBuilder tb;
    // REQ-051: 16 items — the REQ-051 delete button joined the 10-control
    // REQ-048 P2 template, 261116_0003 P9c-1b added the 3-control declaration
    // checklist section (shell 262x130 -> 262x174 DLU), and 261121_0001 U-H
    // added the 2-control reasoning-effort row (shell 262x174 -> 262x190 DLU).
    // 261122_0001 B-U3 (design-pick.md items 5 + 8a): the template is rebuilt
    // in the B1 row order with the 4-control profile row on top (16 -> 20
    // items), and the OFFLINE-measured 89-locale geometry (worst offender +
    // margin) grows the shell to 396x208 DLU — see BuildOpenAiTemplate.
    // 261122_0001 B-U6fix (design items 1 + 4c): the U6 hidden-window probe
    // found the one remaining defect — the closed profile picker (108 DLU) could
    // not show a realistic derived profile name (341 px = 195 DLU) in ANY of the
    // 89 locales. The row's buttons were already at their measured minimum, so
    // the shell grows 396 -> 492 DLU and the profile combo takes 204 DLU; every
    // other row re-anchors to the new content right edge (484). See
    // BuildOpenAiTemplate for the full measurement trail.
    tb.Begin(title, 492, 208, /*itemCount=*/20);
    BuildOpenAiTemplate(tb);

    const INT_PTR rc = ::DialogBoxIndirectParamW(
        ::GetModuleHandleW(nullptr), tb.Get(), parent, OpenAiSettingsProc,
        reinterpret_cast<LPARAM>(&st));
    // R1-9 teardown sweep (after the modal loop): a worker that passed the
    // liveness check just before WM_DESTROY flipped the flag may have enqueued
    // after the in-dialog drain ran, and its wake-up has no live receiver any
    // more. Discard it here so no instance leaves a payload behind; anything
    // that races THIS sweep is caught by the next instance's WM_INITDIALOG
    // drain (the accepted bounded trade-off of not joining a detached worker).
    DrainOpenAiFetchQueue();
    // 261116_0003 P9c-1c: hand the fold out. `edited` is true only when IDOK
    // read the checklist, so a Cancel/[삭제] outcome always reports "nothing
    // to adopt" and the caller can leave a live declaration alone.
    declaration.edited = st.declaration_edited;
    declaration.declared_languages = st.declared_out;
    // REQ-051 (Symptom D): the [삭제] button persisted the cleared block
    // itself and ended with a NON-IDOK code; report it distinctly so the
    // caller never routes a deletion through the save/switch (engine-flip)
    // path. Deleted wins over saved (the button ends the dialog immediately,
    // so the two can never co-occur).
    if (st.deleted) return OpenAiSettingsOutcome::kDeleted;
    const bool saved = st.saved && rc == IDOK;
    // 261122_0001 B-U4 (design items 9.6/9.7/9.8): the staged profile library
    // rides out on the IDOK path ONLY. Cancel, a failed dialog creation and the
    // REQ-051 [설정 삭제] path all leave `edited` false — the [설정 삭제] path
    // returned above, and its staged profile edits die with it (the caller keeps
    // its stored library). On IDOK the caller adopts the vector wholesale and
    // persists it through its own single SaveToFile.
    profiles.edited = saved && st.profiles_edited;
    if (profiles.edited) {
        profiles.profiles = std::move(st.profiles);
    }
    return saved ? OpenAiSettingsOutcome::kSaved : OpenAiSettingsOutcome::kCancelled;
}

OpenAiSettingsOutcome ShowOpenAiSettingsDialogEx(HWND parent, OpenAiConfig& cfg,
                                                 OpenAiDeclaredLanguagesFold& declaration) {
    // 261116_0003 P9c-1c: declaration-only callers keep their exact contract.
    // 261122_0001 B-U4: the profile row is still usable inside this dialog (it
    // edits the local working copy), but with no library to hand out the edits
    // are discarded on return — a caller that needs them persisted must use the
    // library-aware overload above.
    OpenAiProfilesFold profiles;
    return ShowOpenAiSettingsDialogEx(parent, cfg, declaration, profiles);
}

OpenAiSettingsOutcome ShowOpenAiSettingsDialogEx(HWND parent, OpenAiConfig& cfg) {
    // 261116_0003 P9c-1c: outcome-only callers keep their exact contract. The
    // declaration is folded by IDOK but discarded here (there is no config to
    // apply it to), so a caller that needs the declaration persisted must use
    // the declaration-aware overload above.
    OpenAiDeclaredLanguagesFold declaration;
    return ShowOpenAiSettingsDialogEx(parent, cfg, declaration);
}

bool ShowOpenAiSettingsDialog(HWND parent, OpenAiConfig& cfg) {
    // REQ-051 (Symptom D): pre-REQ-051 callers keep their exact contract —
    // only the kSaved outcome reads as true (kDeleted arrives as false, i.e.
    // cancel semantics: engine untouched, no save/switch side effects).
    return ShowOpenAiSettingsDialogEx(parent, cfg) == OpenAiSettingsOutcome::kSaved;
}

} // namespace emebalachat
