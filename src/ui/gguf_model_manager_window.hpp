#pragma once

// ---------------------------------------------------------------------------
// REQ-048 R2-D: the registered user-.gguf model manager dialog. The user asked
// for a way to "at least rename, or delete" the registered third-party .gguf
// models (user feedback round 2, REQ-048 R2). A plain Win32 modal dialog (no
// D2D — functional form, same contract as openai_settings_window) opened from
// the tray engine submenu's "모델 관리…" row (tray.cpp, plain item — NOT part
// of the frozen 4-way radio band 2010~2014).
//
// Contents:
//   * ListBox of the registry's origin != "bundled" models (label "id — file")
//   * Rename (small second dialog: one edit + OK/Cancel) with validation:
//     non-empty, no duplicate id, no path separators / whitespace, <= 64 chars
//   * Delete behind an explicit confirmation — the .gguf FILE ITSELF STAYS ON
//     DISK (multi-GB files are never auto-deleted; the confirmation body says
//     so). When config.user_model_id pointed at the deleted id it is reset to
//     "" and engine_type falls back to "auto" (runtime routing re-pointed).
//   * config.user_model_id tracked across renames (old id -> new id).
//
// Registry writes reuse engine_host_registry::SerializeRegistry (REQ-045 P4-5)
// and NEVER run on a damaged/schema-rejected document (the RegisterUserGguf
// Model loader discipline). Bundled entries are unconditionally preserved —
// the mutation helpers refuse to touch origin == "bundled".
// ---------------------------------------------------------------------------

#include <string>
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

// ---- Template emission (unit-suite byte-walk, same split precedent as
// BuildOpenAiTemplate: Begin() must already have been called with the matching
// item count — 5 for the manager, 4 for the rename prompt) ----
void BuildGgufManagerTemplate(TemplateBuilder& tb);

// ---- The modal manager dialog ----
// `parent` is the owner HWND (g_hControllerWnd from main.cpp). On any applied
// rename/delete the registry is serialized + written, and config is kept in
// sync (rename: matching user_model_id follows; delete: matching
// user_model_id resets to "" + engine_type "auto" + runtime engine re-point).
// Returns true when the registry changed (caller refreshes the tray label).
bool ShowGgufModelManagerDialog(HWND parent, AppConfig& config,
                                TranslationManager& engine);

} // namespace emebalachat
