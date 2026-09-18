#pragma once

// ---------------------------------------------------------------------------
// engine_host_components — REQ-043 (M6 T2, design §1.3/§3.3, plan §V2-8.1):
// parser for %LOCALAPPDATA%\Emebala\Common\engine\components.json, shared by
// the bootstrapper and the installer-side decision helpers (REQ-005/006).
//
// Schema (design §3.3):
//   { "schema_version": 1,
//     "components": {
//       "orchestrator":  { "version": "0.10.1", "abi_version": 2 },
//       "ggml-translate":{ "version": "0.10.1", "abi_version": 1,
//                          "engine": "llama.cpp", "engine_version": "b6099" },
//       ... future families (ct2/onnx — §V2-8.2 partial-install compat) } }
// Contract:
//   * schema_version REQUIRED == 1 (fail-closed otherwise, §V2-12-2).
//   * Unknown FIELDS ignored; unknown COMPONENT entries are PRESERVED — the
//     file is shared install state written by several installers (design §6.3
//     partial update: only the components this installer replaces may be
//     rewritten, every other entry must survive the roundtrip). To make that
//     possible the parser keeps the RAW component-object text for every
//     entry, and SerializeWithComponents() emits a merged document that
//     updates only the named entries.
//   * A component entry needs at least one of version (string) /
//     abi_version (integer); an entry with NEITHER usable field is rejected
//     (it cannot feed decision rule A).
//   * Damaged/unreadable -> empty components map + status (no exceptions).
//
// Decision rule A helpers (plan §V2-8.1, user decision A 2026-09-18):
//   NeedsOrchestratorReplace / NeedsComponentReplace — "component absent or
//   older than the bundle" -> replace. ABSENCE of the file/entry ALWAYS
//   answers "replace" (rule A-1: no version string comparison is attempted
//   for missing data — that is the whole point of rule A).
// ---------------------------------------------------------------------------

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "engine_host_json_util.hpp"
#include "engine_host_protocol.hpp" // unmodified frozen header (JSON primitives)

namespace emebalachat {
namespace engine_host_components {

// The one schema_version this parser accepts (§V2-12-2).
inline constexpr int kComponentsSchemaVersion = 1;

enum class LoadStatus {
    Ok,             // parsed (possibly zero components)
    Missing,        // file does not exist (v1-generation install — rule A-1)
    ReadError,      // exists but unreadable (locked/permission)
    SchemaVersion,  // schema_version missing or != 1 (fail-closed reject)
    NotJson,        // top-level document is not a JSON object
    Malformed,      // components{} missing / wrong-typed entry
};

// One components.<name> entry. version/abi_version are optional PER DESIGN
// (a family may ship only one of them); "present" flags distinguish absent
// from empty. raw_json keeps the full original object for the merge write.
struct ComponentEntry {
    std::string name;
    std::string version;      // e.g. "0.10.1" (empty when absent)
    bool has_version = false;
    int abi_version = 0;
    bool has_abi_version = false;
    std::string raw_json;     // full entry object, raw (merge passthrough, §6.3)
};

struct ComponentsFile {
    int schema_version = 0;
    std::map<std::string, ComponentEntry> components; // key = component name
    bool empty() const { return components.empty(); }
    const ComponentEntry* Find(const std::string& name) const {
        const auto it = components.find(name);
        return it == components.end() ? nullptr : &it->second;
    }
};

struct LoadResult {
    LoadStatus status = LoadStatus::Missing;
    ComponentsFile file;
};

// Parse a components document from an in-memory UTF-8 JSON string.
LoadResult ParseComponentsJson(std::string_view json);

// Load from %LOCALAPPDATA%\Emebala\Common\engine\components.json.
LoadResult LoadDefaultComponents();

// ---- rule A helpers (plan §V2-8.1) ------------------------------------------

// Bundle-side decision input: what this installer carries.
struct BundleComponent {
    std::string version;   // bundled version string, e.g. ENGINE_BUNDLED_VERSION
    int abi_version = 0;   // bundled abi constant (orchestrator: 2)
};

enum class ReplaceDecision { Replace, Keep };

// Decision rule A-2 for ONE component: absent entry -> Replace (rule A-1
// applies per component: no data to compare means replace). Present entry ->
// compare abi_version first (higher bundled abi replaces), then version
// string (lexicographic, the installer's existing comparison semantics).
ReplaceDecision NeedsComponentReplace(const ComponentsFile& file,
                                      const std::string& name,
                                      const BundleComponent& bundle);

// Orchestrator convenience wrapper of NeedsComponentReplace. TRUE when the
// components file/entry is absent ("replace" — rule A-1: missing data never
// blocks a replacement).
ReplaceDecision NeedsOrchestratorReplace(const ComponentsFile& file,
                                         const BundleComponent& orchestrator_bundle);

// ---- partial-update merge (design §6.3) --------------------------------------

// Serialize a merged components document: start from `current` (unknown
// components and unknown fields inside preserved entries are kept verbatim),
// then REPLACE only the entries named in `updates` with the given JSON
// object texts. `updates` entries are raw '{...}' texts (the caller — the
// installer/bootstrapper — controls the exact fields it writes). The output
// keeps a stable entry order: current entries first (original order where
// the map preserves it), then new update-only entries, alphabetically.
// Returns false on serialization failure (invalid raw update text) — the
// caller must not write a half-merged file.
bool SerializeWithComponents(const ComponentsFile& current,
                             const std::map<std::string, std::string>& updates,
                             std::string& out_json);

// Test seam: resolve the default engine directory
// (%LOCALAPPDATA%\Emebala\Common\engine) or empty when LOCALAPPDATA is absent.
std::filesystem::path DefaultEngineDir();

} // namespace engine_host_components
} // namespace emebalachat
