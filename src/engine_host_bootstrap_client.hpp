#pragma once

// ---------------------------------------------------------------------------
// engine_host_bootstrap_client — REQ-005 (M6 T6, design §1.3/§9 R-3b,
// plan §V2-8.3): the APP-side repair bootstrapper. At Chat startup it checks
// the fixed engine-host component paths for the required components; anything
// missing is reported, and — ONLY when a repair-manifest URL was compiled in
// (EMEBALA_REPAIR_URL, a COMPILE-TIME constant per design §9 R-3b, NEVER a
// config.json key) — the missing files are silently repaired by downloading
// them over HTTPS (WinHTTP, the google_translate.cpp init/timeout discipline),
// SHA-256-verified via an INJECTED HashProvider, and atomically moved into
// place with a `.prev` backup. When the URL is empty (the shipped default) or
// the repair cannot complete (offline, bad hash, non-https URL), the result is
// a plain "repair unavailable" code so the CALLER converges on the user-facing
// guidance UX (§V2-8.6: no silent failure).
//
// Component set (the fixed common layout, plan §7.1 / host_main.cpp kEngineDirRel):
//   %LOCALAPPDATA%\Emebala\Common\engine\Emebala.Engine.exe        (orchestrator)
//   %LOCALAPPDATA%\Emebala\Common\engine\Emebalachat.Engine.ggml-translate.exe (worker)
//   %LOCALAPPDATA%\Emebala\Common\engine\worker.<family>.manifest  (per-family
//     manifest scheme, M7 A-1; the pre-A-1 legacy name "worker.manifest" is
//     still ACCEPTED on reads — the requirement gate falls back to it, and the
//     store enumeration below surfaces both)
//   %LOCALAPPDATA%\Emebala\Common\models\<pinned model>            (kPinnedModelFilename)
//   %LOCALAPPDATA%\Emebala\Common\models\registry.json
// Only PRESENCE (and, for repair, the manifest hash) is inspected — user text
// is never read, logged, or transmitted (shape-only diagnostics).
//
// HashProvider injection (design §1.3 note, T2 HashProvider decision): core
// must NOT grow its own SHA-256 (the engine_core implementation is llama-side
// and core->engine_core is forbidden), so the caller injects the hash function
// (the app wires its existing client-side implementation / Windows CNG; the
// unit tests inject a deterministic mock). The bootstrapper computes a hex
// SHA-256 through this seam and compares case-insensitively against the
// manifest entry.
//
// DIAG: shape-only ENGINEHOST/bootstrap/NNN codes (paths/lengths/hashes only).
// ---------------------------------------------------------------------------

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace emebalachat {
namespace engine_host_bootstrap {

// One required component on disk. `relative` is the path RELATIVE to the
// common root (either the engine dir or the models dir — see Root below).
struct RequiredComponent {
    enum class Root : unsigned char {
        Engine,  // %LOCALAPPDATA%\Emebala\Common\engine
        Models,  // %LOCALAPPDATA%\Emebala\Common\models
    };
    Root root;
    std::string relative; // forward-slash relative path, bare filename enforced
};

// The repair-manifest document schema (minimal, consistent with design §3.3
// components and §V2-12-2 unknown-field-ignore):
//   { "schema_version": 1,
//     "files": { "<bare-name>": "<lowercase sha256 hex>", ... } }
// File names MUST be bare (no path separators / ".."); anything else is
// rejected so a hostile manifest can never redirect a write outside the root.
struct RepairFile {
    std::string name;   // bare filename
    std::string sha256; // expected lowercase hex digest
};

enum class ParseStatus : unsigned char {
    Ok,
    NotJson,
    SchemaVersion, // missing / != 1 schema_version
    Malformed,     // files{} missing / wrong types / non-bare names
};

// The compiled-in repair URL (EMEBALA_REPAIR_URL). Empty when the build did
// not provide one — then the repair path is completely disabled.
std::string_view CompiledRepairUrl();

// Parse a repair-manifest document. Fail-closed; damaged input never throws.
ParseStatus ParseRepairManifest(std::string_view json, std::vector<RepairFile>& out);

// ---- component presence check ----------------------------------------------
// Resolves the fixed common engine/models dirs (empty when LOCALAPPDATA is
// unavailable). Pure filesystem existence checks — no content is read.
struct ComponentCheckResult {
    std::vector<std::string> missing; // relative paths (engine/ or models/ prefix)
    bool engine_dir_resolved = false;
    bool models_dir_resolved = false;
};
ComponentCheckResult CheckComponents();

// REQ-048 P1: bounded grace re-check for boot-time component presence. Calls
// check() up to `attempts` times — the FIRST call happens immediately (no
// pre-wait), then `interval` is slept between attempts — and returns true on
// the first true. Returns false when every attempt is false, or when
// attempts <= 0 (check is never invoked). The injected seam keeps the wait
// loop unit-testable without filesystem state; production passes a
// CheckComponents() re-check lambda.
bool WaitForComponentsPresent(const std::function<bool()>& check, int attempts,
                              std::chrono::milliseconds interval);

// ---- repair ------------------------------------------------------------------
// The caller-injected hash seam: given an absolute file path, write the
// lowercase hex SHA-256 of its content. Return false on any failure (the
// bootstrapper then rejects the file — fail-closed).
using HashProvider = std::function<bool(const std::filesystem::path&, std::string&)>;

enum class RepairOutcome : unsigned char {
    // All required components are present — nothing to do.
    NothingToRepair,
    // The missing components were downloaded, hash-verified and installed.
    Repaired,
    // Nothing was missing OR the repair ran; the ONLY distinction the caller
    // needs is "can the local engine serve now". All of the following are the
    // §V2-8.6 "repair unavailable" convergence (the caller surfaces guidance):
    RepairDisabled,   // no URL compiled in (the shipped default)
    RepairNotNeeded,  // alias of NothingToRepair (kept for readability)
    DownloadFailed,   // offline / transport error (WinHTTP)
    ManifestRejected, // non-https URL, bad manifest schema, non-bare names
    HashMismatch,     // a downloaded file failed its SHA-256 pin
    InstallFailed,    // the verified file could not be moved into place
};

const char* RepairOutcomeToString(RepairOutcome o);

// Repair the given missing components. `url` is the repair-manifest base URL
// (typically CompiledRepairUrl()); it MUST begin with "https://" or the whole
// repair is rejected (ManifestRejected). `hash` is the injected SHA-256 seam.
// Each file is fetched as `<url-dir><name>` — the manifest lists BARE names
// resolved against the same directory as the manifest itself — written to a
// sibling `<name>.download`, hash-verified, then the existing file (if any) is
// renamed to `<name>.prev` and the download is moved into place (§V2-8.4
// minimal form). One retry on transport failure; retries never re-verify a
// hash-mismatched file (a bad pin is permanent).
//
// `missing` entries carry an "engine/" or "models/" prefix identifying the
// target root (the same strings CheckComponents emits). Returns the outcome;
// shape-only ENGINEHOST/bootstrap/NNN diagnostics on every non-Ok path.
RepairOutcome RepairMissingComponents(const std::vector<std::string>& missing,
                                      std::string_view url,
                                      const HashProvider& hash);

// Test seam: expose the target root resolution for one prefixed relative path
// ("engine/x" -> engine dir, "models/x" -> models dir). Empty when unresolved.
std::filesystem::path ResolveTargetPath(const std::string& prefixed_relative);

// ---- M7 A-1 per-family worker-manifest scheme -------------------------------
// Enumerate every worker-manifest file present in the common ENGINE directory
// (both the per-family worker.<family>.manifest scheme and the pre-A-1 legacy
// worker.manifest are surfaced; see engine_host_paths.hpp::IsWorkerManifestName).
// Returns bare filenames in sorted order. An unresolvable/absent engine dir
// yields an empty vector. The requirement gate and the repair flow enumerate
// manifests this way instead of hardcoding a single file name; the
// orchestrator's multi-manifest load uses the same enumeration.
std::vector<std::string> EnumerateWorkerManifests();

// Test seam: the same enumeration restricted to an arbitrary directory
// (`engine_dir`), so unit tests can stage fake stores without touching
// %LOCALAPPDATA%. Never throws — iteration errors converge on the entries
// collected so far.
std::vector<std::string> EnumerateWorkerManifestsIn(const std::filesystem::path& engine_dir);

// Test seam: resolve the manifest file NAME for one worker family inside an
// arbitrary directory using the read scheme — new-scheme name first, falling
// back to the legacy bare name when only that exists on disk. Returns the
// FILENAME (not a path) of the chosen candidate, or empty when the directory
// holds neither. `dir` is injected so unit tests can stage fake stores without
// touching %LOCALAPPDATA%.
std::string ResolveWorkerManifestName(const std::filesystem::path& dir,
                                      const std::string& family);

} // namespace engine_host_bootstrap
} // namespace emebalachat
