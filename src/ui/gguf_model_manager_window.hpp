#pragma once

// ---------------------------------------------------------------------------
// REQ-048 R2-D: the registered user-.gguf model manager dialog. The user asked
// for a way to "at least rename, or delete" the registered third-party .gguf
// models (user feedback round 2, REQ-048 R2). A plain Win32 modal dialog (no
// D2D — functional form, same contract as openai_settings_window).
//
// REQ-050: the tray's two user-model rows (the file-picker row and this
// manager row) are MERGED — the manager is now the single entry point:
//   * Top row: [파일에서 추가…] (the existing file-picker registration
//     pipeline, passed in as a callback — see ShowGgufModelManagerDialog)
//     and [Hugging Face에서 추가…] (paste a model URL -> NormalizeHfUrl
//     auto-conversion (REQ-050 2-1) -> https-only host-pinned WinHTTP
//     download with progress + cancel -> registration).
//   * Below: the ListBox of the registry's origin != "bundled" models
//     (label "id — file"), Rename (small second dialog: one edit + OK/Cancel)
//     with validation (non-empty, no duplicate id, no path separators /
//     whitespace, <= 64 chars), and Delete behind an explicit confirmation —
//     the .gguf FILE ITSELF STAYS ON DISK (multi-GB files are never
//     auto-deleted; the confirmation body says so). When config.user_model_id
//     pointed at the deleted id it is reset to "" and engine_type falls back
//     to "auto" (runtime routing re-pointed).
//
// REQ-060 (session 261009_0001): the manager gained two model-lifecycle
// tools on top of REQ-048 R2-D / REQ-050:
//   * Resident-policy display + toggle — every user row's label carries the
//     entry's eviction-policy suffix ("(상주 유지)" sticky / "(사용 시 로드)"
//     otherwise; an empty/unknown policy normalizes to the on-use leg, the
//     registry parser's evict default). The [정책 전환] button flips the
//     selected user entry's resource.eviction sticky <-> evict and persists
//     through the SAME atomic registry write as rename/delete. Bundled
//     entries are never listed, so the button is inherently inert for them
//     (the enable gate is the user-row selection, like Rename/Delete). The
//     toggle deliberately does NOT touch config.user_model_id.
//   * Duplicate-file detection + cleanup — at dialog open the registered
//     user-model files are compared (normalized stem = the ".gguf" tail plus
//     any trailing "_N" copy suffix stripped; equal normalized stem AND
//     equal byte size) against each other AND against unregistered .gguf
//     files sitting in the model store dir (engine_host_registry::
//     DefaultModelsDir). A group with >= 2 members of which >= 1 is
//     registered surfaces a warning STATIC + a [중복 정리] button on the
//     top row (the add-method buttons give way while the warning is up).
//     Cleanup moves ONLY the unregistered side's files to the Recycle Bin
//     (SHFileOperationW FO_DELETE | FOF_ALLOWUNDO | FOF_NO_UI — undoable by
//     construction, never a permanent delete); registered entries and their
//     files are never touched. When every copy of a group is registered
//     there is no unregistered side, so the button stays gray until the
//     user removes one registration.
//
// Registry writes reuse engine_host_registry::SerializeRegistry (REQ-045 P4-5)
// and NEVER run on a damaged/schema-rejected document (the RegisterUserGguf
// Model loader discipline). Bundled entries are unconditionally preserved —
// the mutation helpers refuse to touch origin == "bundled".
// ---------------------------------------------------------------------------

#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

#include "../config.hpp"            // AppConfig (REQ-048 R2-D: user_model_id tracking)
#include "../engine.hpp"            // TranslationManager (delete -> EngineType::Auto re-point)
#include "engine_host_registry.hpp" // 261111_0001 U7: engine repo header (bare form; see engine_host_client.hpp above)
#include "../i18n.hpp"              // REQ-060: StringId (policy-suffix helper return)
#include "openai_settings_window.hpp" // REQ-046 P4-3 / REQ-048 P2 TemplateBuilder (proven DLGTEMPLATE contract)

namespace emebalachat {

// ---- Pure helpers (headless-testable; no GUI, no logging side effects) ----

// New-id validation verdict for the rename dialog (REQ-048 R2-D rules).
enum class GgufModelIdError {
    Ok,
    Empty,     // zero-length (or whitespace-only input trimmed to nothing)
    TooLong,   // > 64 chars (kMaxUserModelIdLen)
    BadChars,  // path separator, drive colon, whitespace, or control char
    Duplicate, // already taken by another registry entry
};

// REQ-048 R2-D: rename-id ceiling (kept well below filesystem limits; the
// registry id doubles as a display label in the tray).
inline constexpr std::size_t kMaxUserModelIdLen = 64;

// Validates `new_id` against the rename rules. `current_id` (the id being
// renamed FROM) is excluded from the duplicate check so a no-change rename
// reports Ok; the dialog short-circuits new == old before validating anyway.
GgufModelIdError ValidateUserModelId(const std::string& new_id,
                                     const engine_host_registry::Registry& registry,
                                     const std::string& current_id);

// The origin != "bundled" entries, in registry order (the manager list shows
// exactly these; bundled entries are never listed, renamed, or deleted).
std::vector<const engine_host_registry::ModelEntry*>
UserModelsOf(const engine_host_registry::Registry& registry);

// Renames the origin != "bundled" entry `old_id` -> `new_id` IN MEMORY.
// Returns false when the entry is missing/bundled or `new_id` is taken.
// No file I/O here — the caller serializes + writes (loud-failure policy).
bool RenameUserModelEntry(engine_host_registry::Registry& registry,
                          const std::string& old_id, const std::string& new_id);

// Removes the origin != "bundled" entry `id` IN MEMORY (bundled entries are
// unconditionally preserved). Returns false when no user entry matches.
bool RemoveUserModelEntry(engine_host_registry::Registry& registry,
                          const std::string& id);

// ---- REQ-060 (session 261009_0001): resident-policy display + duplicate
// detection pure helpers (headless-testable; no GUI, no I/O) ---------------

// Maps a registry entry's eviction policy to its list-label suffix string:
// "sticky" -> GgufPolicySticky ("(상주 유지)"), EVERYTHING else — including the
// empty string a hand-edited or freshly serialized entry may carry — ->
// GgufPolicyEvict ("(사용 시 로드)"). The empty-policy normalization matches the
// registry parser's documented evict default.
StringId ManagerPolicySuffixId(const std::string& eviction);

// The duplicate-comparison name of a model file: the filename with a
// trailing ".gguf" (case-insensitive) stripped, then any trailing "_<digits>"
// copy suffix stripped — "MiLMMT-46-4B_2.gguf" -> "MiLMMT-46-4B". Two files
// are duplicate CANDIDATES when their normalized stems match (the caller
// additionally requires an equal byte size; the stem alone is too loose).
std::string ManagerDuplicateStem(const std::string& filename);

// REQ-060 M-1 fix (security review round, session 261009_0001): the
// duplicate cleanup's "registered" guard. True when `file_name` equals ANY
// member of ANY registry entry's files[] — every origin (the bundled Hy-MT2
// entry included, not only user models) and every files[] slot (files[1+]
// included, not only files[0]). The name compare is ASCII case-insensitive
// (the store is a Windows volume). The cleanup paths consult this immediately
// before acting, so a file ANY registry entry references can never become a
// Recycle-Bin target. Pure scan, no I/O.
bool IsRegisteredModelFile(const engine_host_registry::Registry& registry,
                           const std::wstring& file_name);

// ---- REQ-050: Hugging Face resolve-URL gate (fail-closed, headless-testable)
//
// IsHfResolveUrl is the CANONICAL-shape gate: it accepts only model FILE URLs
// of the exact shape  https://huggingface.co/<repo>/resolve/<revision>/<path...>
// — anything else (other hosts, http, /tree/ pages, query/fragment tricks,
// empty or dot path segments) is rejected. The scheme + host prefix is
// compared case-sensitively: the validator is deliberately stricter than a
// browser so a lookalike URL can never reach WinHTTP.
bool IsHfResolveUrl(std::string_view url);

// REQ-050 (user item 2-1): user-facing URL auto-conversion. Accepts the three
// shapes a user actually pastes from huggingface.co and normalizes each to
// the canonical resolve form above BEFORE any network I/O:
//   * https://huggingface.co/<repo>/resolve/<rev>/<file...>  (verbatim)
//   * https://huggingface.co/<repo>/blob/<rev>/<file...>     (-> /resolve/)
//   * https://huggingface.co/<repo>?show_file_info=<file>    (percent-decoded
//     file param, single occurrence only -> /resolve/main/<file>)
// Everything else fails closed: wrong host, http, /tree/ paths, a blob/page
// URL whose file cannot be determined, multiple (or unknown) query params,
// malformed percent-encoding, and any path traversal. On success *out holds
// a string that passes IsHfResolveUrl (re-validated inside); on failure
// *out is empty. `out` may be null (plain predicate form).
bool NormalizeHfUrl(std::string_view url, std::string* out_resolve_url);

// Derives the LOCAL filename (last path segment) from a resolve URL already
// accepted by IsHfResolveUrl. Rejects an empty segment, "." / "..", any
// separator/drive-colon/control char (engine_host_json::IsBareFilename), and
// the additional Windows-invalid filename characters (<> "|?*). The URL is
// re-validated inside, so callers can pass an arbitrary string safely.
bool HfResolveFilename(std::string_view url, std::string* out_filename);

// REQ-052: decodes the value-only kHfMsgProgress wParam. The worker posts the
// percent as a WORD (0..100), with (WORD)-1 as the "server gave no
// Content-Length" marquee sentinel. The pre-fix handler did a plain
// WORD->int cast, so the sentinel decoded as 65535, the marquee branch never
// ran and the status line read "(65535%)". The payload is value-only, so the
// WPARAM is truncated to its low WORD first and any upper bits are discarded
// by design.
inline int HfDecodeProgressPercent(WPARAM wp) {
    const WORD raw = static_cast<WORD>(wp);
    return raw == static_cast<WORD>(-1) ? -1 : static_cast<int>(raw);
}

// ---- Template emission (unit-suite byte-walk, same split precedent as
// BuildOpenAiTemplate: Begin() must already have been called with the matching
// item count — 8 for the manager (REQ-060 added the policy-toggle button),
// 4 for the rename prompt, 5 for the HF add dialog) ----
void BuildGgufManagerTemplate(TemplateBuilder& tb);

// REQ-050: the Hugging Face add-dialog item set (URL label + URL edit +
// status static + Download(IDOK, DialogOk label) + Cancel(IDCANCEL,
// DialogCancel label) = 5). The progress bar is NOT a template item — the
// system-class-ordinal TemplateBuilder cannot emit "msctls_progress32", so
// the dialog proc creates it at runtime (pinned structurally in the tests).
void BuildHfAddTemplate(TemplateBuilder& tb);

// REQ-050 (user item 2-2): the runtime progress bar's DLU-space rect. The
// template cannot carry msctls_progress32 (no system-class ordinal), so the
// dialog proc creates the bar at runtime — through MapDialogRect. The pre-fix
// code handed these DLU numbers to CreateWindowExW RAW (which takes PIXELS),
// landing the bar across the middle of the URL edit (the user's "the square
// box looks covered" report). left/top/right/bottom in dialog units.
inline constexpr RECT kHfProgressRectDlu = { 8, 35, 254, 47 };

// REQ-060: the duplicate-warning pair is NOT part of the dialog template (the
// warning appears only when a duplicate group exists, so the two controls are
// created at runtime — the kHfProgressRectDlu precedent — and take over the
// top add-method row while the warning is up). Both rects are DIALOG UNITS and
// must pass through MapDialogRect before CreateWindowExW (which takes pixels).
// left/top/right/bottom in dialog units.
inline constexpr RECT kDupWarningLabelRectDlu = { 8, 5, 196, 18 };
inline constexpr RECT kDupWarningButtonRectDlu = { 204, 5, 278, 18 };

// ---- The modal manager dialog ----
// `parent` is the owner HWND (g_hControllerWnd from main.cpp). On any applied
// rename/delete the registry is serialized + written, and config is kept in
// sync (rename: matching user_model_id follows; delete: matching
// user_model_id resets to "" + engine_type "auto" + runtime engine re-point).
// Returns true when the registry changed (caller refreshes the tray label).
//
// REQ-050: `add_from_file` is the app's existing file-picker registration
// pipeline (main.cpp's RegisterUserGgufModel + tray refresh). It lives in the
// GUI entrypoint TU (deliberately not linked into Emebalachat_core), so the
// manager reaches it through this callback — invoked from the top-row
// [파일에서 추가…] button. May be empty (the dialog disables the button).
bool ShowGgufModelManagerDialog(HWND parent, AppConfig& config,
                                TranslationManager& engine,
                                const std::function<void()>& add_from_file);

// REQ-050: the modal "Hugging Face 모델 추가" dialog (URL -> https-only
// host-pinned download with progress + cancel -> registration). Runs its own
// modal loop; the download pumps on a worker thread and reports back through
// value-only WM_APP messages (no pointer crosses PostMessageW — the SEC-ADJ
// house contract). The worker is joined before the dialog closes.
// Returns true when a model was registered (caller reloads + refreshes).
bool ShowHfAddDialog(HWND parent, AppConfig& config, TranslationManager& engine);

} // namespace emebalachat
