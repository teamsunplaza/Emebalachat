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
#include "../engine_host_registry.hpp"
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

// ---- Template emission (unit-suite byte-walk, same split precedent as
// BuildOpenAiTemplate: Begin() must already have been called with the matching
// item count — 7 for the manager, 4 for the rename prompt, 5 for the HF add
// dialog) ----
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
