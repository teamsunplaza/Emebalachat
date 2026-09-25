// ---------------------------------------------------------------------------
// engine_host_paths.hpp — REQ-044 (P4-2): shared filesystem-path constants for
// the engine-host family (orchestrator / bootstrap client / ggml-translate
// worker). Header-only, #pragma once, constexpr wchar_t[] arrays only — no
// functions, no global state, zero ODR risk.
//
// Consolidation target (docs/260919_0001_session_code-quality-audit-compression/
// 081200_architect-req044-p3-architecture-design.md §item 3):
//   - host_main.cpp previously defined kEngineDirRel / kModelDirRel /
//     kTokenFilename locally (L119-121).
//   - engine_host_bootstrap_client.cpp previously defined kEngineDirRel /
//     kModelsDirRel / kOrchestratorExe / kWorkerExe / kWorkerManifest /
//     kRegistryJson locally (L44-49).
//   - ggml_translate_worker.cpp used an inline literal at the model-path site.
//
// Deliberately EXCLUDED:
//   - Pipe names ("emebala-engine", "emebala-engine-v1") — frozen wire-protocol
//     contracts, kept in their respective modules (host_main.cpp /
//     engine_host_protocol.hpp / engine_host_client.cpp).
//   - engine_host_client.cpp / engine_host_client.hpp — self-contained VERBATIM
//     contract; must not include any external header (git diff must stay 0).
//
// CMake: no change required — header-only, no new source TU.
//
// M7 A-1 (session 260922_0001): the worker-manifest FILENAME scheme moved from
// the single bare name "worker.manifest" to the per-family scheme
// "worker.<family>.manifest" so several worker families (ggml-translate,
// ggml-asr, ...) can coexist in the one shared engine store without
// overwriting each other. Writes use the new scheme ONLY; reads additionally
// accept the legacy bare name kWorkerManifestLegacy (backward compatibility
// with installs predating the scheme). The manifest SCHEMA itself is untouched
// (worker_protocol.hpp stays frozen; only the file naming/enumeration changed).
// The header adds <string>/<string_view>; all helpers are `inline` — still
// no global state, zero ODR risk.
// ---------------------------------------------------------------------------

#pragma once

#include <string>
#include <string_view>

namespace emebalachat {
namespace enginehost {
namespace paths {

// REQ-044 (P4-2): replaces kEngineDirRel in host_main.cpp L119 and
// engine_host_bootstrap_client.cpp L44.
constexpr wchar_t kEngineDirRel[] = L"Emebala\\Common\\engine";

// REQ-044 (P4-2): replaces kModelDirRel in host_main.cpp L120,
// kModelsDirRel in engine_host_bootstrap_client.cpp L45, and the inline
// literal in ggml_translate_worker.cpp L263.
constexpr wchar_t kModelsDirRel[] = L"Emebala\\Common\\models";

// REQ-044 (P4-2): replaces kTokenFilename in host_main.cpp L121.
constexpr wchar_t kTokenFilename[] = L"token";

// REQ-044 (P4-2): replaces kOrchestratorExe in
// engine_host_bootstrap_client.cpp L46.
constexpr wchar_t kOrchestratorExe[] = L"Emebala.Engine.exe";

// REQ-044 (P4-2): replaces kWorkerExe in
// engine_host_bootstrap_client.cpp L47.
constexpr wchar_t kWorkerExe[] = L"Emebalachat.Engine.ggml-translate.exe";

// M7 A-1 (session 260922_0001): the legacy single-file manifest name. Kept as
// the READ fallback for stores written before the per-family scheme (a pre-A-1
// installer laid down exactly this file). New writes NEVER use it. Formerly
// REQ-044's kWorkerManifest (the only accepted manifest name).
constexpr wchar_t kWorkerManifestLegacy[] = L"worker.manifest";

// M7 A-1: per-family manifest file-name scheme  worker.<family>.manifest
// (family string used verbatim, hyphen included — DEC-007: "ggml-asr",
// "ggml-translate"). The two constants below are the fixed scheme edges; the
// helpers build/check names against them.
constexpr wchar_t kWorkerManifestPrefix[] = L"worker.";
constexpr wchar_t kWorkerManifestSuffix[] = L".manifest";

// Build the canonical (write-scheme) manifest filename for a family:
//   WorkerManifestName(L"ggml-translate") -> L"worker.ggml-translate.manifest"
// An empty family yields the legacy bare name (defensive; callers pass a
// registered family). Returns a std::wstring because it concatenates.
inline std::wstring WorkerManifestName(std::wstring_view family) {
    if (family.empty()) return std::wstring(kWorkerManifestLegacy);
    return std::wstring(kWorkerManifestPrefix) + std::wstring(family) +
           std::wstring(kWorkerManifestSuffix);
}

// True when `name` (a bare filename) belongs to the worker-manifest scheme:
// either a per-family worker.<family>.manifest or the legacy worker.manifest.
// Used by the store enumeration (bootstrap requirement gate, repair gate,
// orchestrator manifest load): everything matching is a manifest candidate.
// A per-family name must have a non-empty family segment in the middle.
inline bool IsWorkerManifestName(std::wstring_view name) {
    if (name == std::wstring_view(kWorkerManifestLegacy)) return true;
    constexpr std::wstring_view prefix(kWorkerManifestPrefix);
    constexpr std::wstring_view suffix(kWorkerManifestSuffix);
    if (name.size() <= prefix.size() + suffix.size()) return false;
    if (name.substr(0, prefix.size()) != prefix) return false;
    if (name.substr(name.size() - suffix.size()) != suffix) return false;
    return !name.substr(prefix.size(),
                        name.size() - prefix.size() - suffix.size()).empty();
}

// Extract the family segment from a per-family manifest name
// (worker.<family>.manifest -> <family>). Returns EMPTY for the legacy bare
// worker.manifest (no family segment) and for any name outside the scheme —
// callers that must distinguish the two gate on IsWorkerManifestName first.
// P2-1 (session 260925_0001): the orchestrator's exe-adjacent enumeration
// registers one family per discovered manifest, so a worker family deploys by
// dropping one exe + one manifest next to Emebala.Engine.exe.
inline std::wstring WorkerFamilyFromManifestName(std::wstring_view name) {
    constexpr std::wstring_view prefix(kWorkerManifestPrefix);
    constexpr std::wstring_view suffix(kWorkerManifestSuffix);
    if (name.size() <= prefix.size() + suffix.size()) return {};
    if (name.substr(0, prefix.size()) != prefix) return {};
    if (name.substr(name.size() - suffix.size()) != suffix) return {};
    const std::wstring_view middle = name.substr(
        prefix.size(), name.size() - prefix.size() - suffix.size());
    if (middle.empty()) return {};
    return std::wstring(middle);
}

// REQ-044 (P4-2): replaces kRegistryJson in
// engine_host_bootstrap_client.cpp L49.
constexpr wchar_t kRegistryJson[] = L"registry.json";

} // namespace paths
} // namespace enginehost
} // namespace emebalachat
