#pragma once

// ---------------------------------------------------------------------------
// engine_host_registry — REQ-043 (M6 T2, design §1.3/§3.1, plan §V2-5):
// parser for %LOCALAPPDATA%\Emebala\Common\models\registry.json.
//
// Schema (design §3.1, §V2-12-2 rules):
//   { "schema_version": 1, "models": [ { id, family, files[], capabilities[],
//       origin, resource{vram_mb,ctx,max_sessions,residency,eviction,priority},
//       profiles{ <name>: {temperature, top_p, top_k, rep_pen,
//                           prompt_template_ref, ...} }, lang_pairs[] } ] }
// Contract:
//   * schema_version is REQUIRED and must equal 1 — a missing/other version is
//     document REJECTION (fail-closed, §V2-12-2).
//   * Unknown fields are IGNORED (both document-level and per-model).
//   * Damaged/unreadable file -> EMPTY registry + a status code. NO exceptions
//     escape this module; the caller decides what an empty registry means.
//   * resource fields are type-checked and RANGE-CLAMPED (a scheduler-policy
//     input must be finite and sane; clamping beats rejection for robustness —
//     the values feed scheduling, not correctness).
//   * files[] entries must be bare filenames (they resolve against the fixed
//     Common\models dir; a separator/escape is tampering -> item rejected).
//   * origin:"user" items are ACCEPTED by the parser (design §3.1: resource is
//     an estimate allowed); manifest verification is the consumer's concern.
//
// Privacy: DIAG lines are shape-only (path, lengths, model id, reason codes)
// and the file sink itself stays opt-in (diag_logger default OFF).
// ---------------------------------------------------------------------------

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "engine_host_json_util.hpp"
#include "engine_host_protocol.hpp" // unmodified frozen header (JSON primitives)

namespace emebalachat {
namespace engine_host_registry {

// The one schema_version this parser accepts (§V2-12-2).
inline constexpr int kRegistrySchemaVersion = 1;

// Range clamps for resource profile numbers (design §3.1 example values; the
// clamps are parser-level sanity floors/ceilings — policy lives in data).
inline constexpr int kClampVramMbMax = 1024 * 100;   // 100 GB ceiling
inline constexpr int kClampCtxMax = 1024 * 1024;     // 1M context ceiling
inline constexpr int kClampMaxSessionsMin = 1;       // at least 1 session
inline constexpr int kClampMaxSessionsMax = 64;
inline constexpr int kClampPriorityMin = 0;          // 0-9 scheduler scale (§V2-4.6)
inline constexpr int kClampPriorityMax = 9;

// Load status for the caller's branching (no exceptions leave this module).
enum class LoadStatus {
    Ok,              // file read + parsed (possibly zero models)
    Missing,         // file does not exist (fresh machine / pre-install)
    ReadError,       // exists but could not be read (lock/permission)
    SchemaVersion,   // missing schema_version or != 1 (fail-closed reject)
    NotJson,         // top-level document is not a JSON object
    Malformed,       // structurally broken JSON or wrong-typed required field
};

struct ResourceProfile {
    int vram_mb = 0;          // clamped 0..kClampVramMbMax
    int ctx = 0;              // clamped 0..kClampCtxMax
    int max_sessions = 1;     // clamped kClampMaxSessionsMin..kClampMaxSessionsMax
    std::string residency;    // "preload" | "ondemand" (unknown -> "ondemand")
    std::string eviction;     // "sticky" | "evict"   (unknown -> "evict")
    int priority = 5;         // clamped kClampPriorityMin..kClampPriorityMax
};

// One profiles{} entry: the known sampling keys are extracted; everything else
// inside the profile object is ignored (kept available via raw_json).
struct SamplingProfile {
    std::string name;             // profile key, e.g. "default"
    double temperature = 0.0;
    double top_p = 1.0;
    int top_k = 0;
    double rep_pen = 1.0;
    std::string prompt_template_ref; // e.g. "hymt2-official"
    std::string raw_json;            // full profile object, raw (merge passthrough)
};

// One models[] entry. Unknown FIELDS are ignored (drop), unknown FILES inside
// files[] stay as parsed (the manifest check decides their fate).
struct ModelEntry {
    std::string id;
    std::string family;
    std::vector<std::string> files;
    std::vector<std::string> capabilities;
    std::string origin;                 // "bundled" | "user" (unknown -> "user")
    ResourceProfile resource;
    std::map<std::string, SamplingProfile> profiles; // key = profile name
    std::vector<std::string> lang_pairs;
};

struct Registry {
    int schema_version = 0;
    std::vector<ModelEntry> models;
    bool empty() const { return models.empty(); }
    const ModelEntry* FindModel(const std::string& id) const {
        for (const auto& m : models) {
            if (m.id == id) return &m;
        }
        return nullptr;
    }
};

// Parse result: status != Ok => registry is left EMPTY (fail-closed). A status
// is returned even on success so callers can distinguish empty-but-valid docs.
struct LoadResult {
    LoadStatus status = LoadStatus::Missing;
    Registry registry;
};

// Parse a registry document from an in-memory UTF-8 JSON string. Never throws.
LoadResult ParseRegistryJson(std::string_view json);

// REQ-045 P4-5 (item 3a-2, design §A.3): the registry WRITER. Serializes a
// Registry back to the UTF-8 JSON document shape the parser accepts
// (schema_version + models[] with id/family/files[]/origin and the optional
// blocks). Every string is re-escaped; every files[] entry is re-checked with
// IsBareFilename (the writer refuses to emit a path-escape just as the parser
// refuses to read one). The output is byte-stable for a fixed input, so
// write→parse round-trips are identity-checked by the unit tests.
std::string SerializeRegistry(const Registry& registry);

// Parse + report a single model item (exposed for tests). Returns false when
// the item is rejected (missing id/files, non-string fields, path escape in
// files[]); a rejected item does NOT abort the whole document parse.
bool ParseModelItem(std::string_view raw_item, ModelEntry& out);

// Load from %LOCALAPPDATA%\Emebala\Common\models\registry.json. Missing file ->
// LoadStatus::Missing + empty registry (a fresh machine state, not an error).
LoadResult LoadDefaultRegistry();

// Test seam: resolve the default models directory
// (%LOCALAPPDATA%\Emebala\Common\models) or empty when LOCALAPPDATA is absent.
std::filesystem::path DefaultModelsDir();

} // namespace engine_host_registry
} // namespace emebalachat
