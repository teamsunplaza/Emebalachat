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
// ---------------------------------------------------------------------------

#pragma once

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

// REQ-044 (P4-2): replaces kWorkerManifest in
// engine_host_bootstrap_client.cpp L48.
constexpr wchar_t kWorkerManifest[] = L"worker.manifest";

// REQ-044 (P4-2): replaces kRegistryJson in
// engine_host_bootstrap_client.cpp L49.
constexpr wchar_t kRegistryJson[] = L"registry.json";

} // namespace paths
} // namespace enginehost
} // namespace emebalachat
