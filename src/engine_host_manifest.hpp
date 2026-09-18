#pragma once

// ---------------------------------------------------------------------------
// engine_host_manifest — REQ-043 (M6 T2, design §1.3/§3.2, plan §V2-6):
// parser for %LOCALAPPDATA%\Emebala\Common\models\manifest.json plus the
// manifest signature-verifier seam.
//
// Schema (design §3.2):
//   { "schema_version": 1, "pinned": { "<bare filename>": "<64-hex sha256>",
//                                      ... } }
// Contract:
//   * schema_version REQUIRED == 1 (fail-closed otherwise, §V2-12-2).
//   * Unknown fields ignored; unknown FILENAMES in pinned{} are preserved
//     verbatim (the map IS the contract — extra pins are not errors).
//   * pinned keys must be bare filenames (IsBareFilename — the manifest
//     resolves them against the fixed Common\models dir; separators/escapes
//     are tampering and reject the DOCUMENT).
//   * pinned values must be exactly 64 lowercase hex chars (the sha256 form
//     the installer writes and ComputeFileSha256 emits).
//   * Damaged/unreadable -> empty manifest + status (no exceptions escape).
//
// Signature verification (design §3.2 snippet, kept as written):
//   IManifestVerifier::VerifySignature(manifest_path) — the dev-mode default
//   (DevTrustVerifier) returns true WITHOUT any cryptographic check because
//   no code-signing certificate is provisioned yet (design §9 R-2: the
//   unsigned manifest is treated as "development/pre-release"); it emits ONE
//   shape-only DIAG note the first time it vouches for a path.
//   AuthenticodeVerifier is a fail-closed SKELETON: its WinVerifyTrust call
//   path is deliberately NOT implemented (REQ-043 TODO) — wiring a call that
//   cannot validate the Emebala certificate chain yet would either silently
//   pass untrusted manifests or break every install. It returns false with
//   status "certificate pending" until the certificate is provisioned.
//
// Hash matching (design §3.2 rule 2 — model file sha256 == pinned value):
//   ComputeFileSha256 lives in Emebalachat_engine_core; core must NOT link
//   engine_core (T1 rule, design §4.2), so the check is CALLER-INJECTED:
//   VerifyModelHashes takes a std::function hash provider. The host (which
//   links engine_core) supplies ComputeFileSha256; core never does.
//
// Privacy: shape-only DIAG (paths, counts, filenames, reason codes) — never
// file content; file sink stays opt-in (diag_logger default OFF).
// ---------------------------------------------------------------------------

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "engine_host_json_util.hpp"
#include "engine_host_protocol.hpp" // unmodified frozen header (JSON primitives)

namespace emebalachat {
namespace engine_host_manifest {

// The one schema_version this parser accepts (§V2-12-2).
inline constexpr int kManifestSchemaVersion = 1;

// Design §3.2 snippet (kept verbatim in shape): the seam the real
// Authenticode implementation will fill once the Emebala certificate exists.
class IManifestVerifier {
public:
    virtual ~IManifestVerifier() = default;
    // Manifest file signature/trust verification. Dev mode always returns
    // true (see DevTrustVerifier below).
    virtual bool VerifySignature(const std::filesystem::path& manifest_path) = 0;
};

// Development-mode default: trust the installer-written manifest (signature
// verification skipped — certificate not yet provisioned, design §9 R-2).
// Emits exactly ONE shape-only DIAG line per process (first VerifySignature
// call) so dev deployments are traceable in enabled logs.
class DevTrustVerifier final : public IManifestVerifier {
public:
    bool VerifySignature(const std::filesystem::path& manifest_path) override;
};

// Authenticode skeleton (REQ-043 TODO — replace when the certificate lands).
// Fail-closed: ALWAYS returns false ("certificate pending"). No WinVerifyTrust
// call is compiled — an unverifiable chain must not quietly pass (design §10
// "development mode explicit lower bound").
class AuthenticodeVerifier final : public IManifestVerifier {
public:
    bool VerifySignature(const std::filesystem::path& manifest_path) override;
};

// Factory (design §3.2). mode strings: "dev" (default) | "authenticode".
// Unknown mode strings fall back to the dev verifier (fail-open only toward
// the EXPLICITLY documented dev lower bound; production code passes a
// known-literal). nullptr never returned.
std::unique_ptr<IManifestVerifier> CreateManifestVerifier(const char* mode = "dev");

// ---- document model ---------------------------------------------------------

enum class LoadStatus {
    Ok,             // parsed (possibly zero pins)
    Missing,        // file does not exist
    ReadError,      // exists but unreadable (locked/permission)
    SchemaVersion,  // schema_version missing or != 1 (fail-closed reject)
    NotJson,        // top-level document is not a JSON object
    Malformed,      // pinned{} missing / wrong-typed / bad key or hash form
};

struct Manifest {
    int schema_version = 0;
    std::map<std::string, std::string> pinned; // filename -> lowercase hex sha256
    bool empty() const { return pinned.empty(); }
    // Look up the pin for a model file. Returns false when the file is NOT in
    // the manifest (the §V2-6 rejection rule for bundled models).
    bool FindPin(const std::string& filename, std::string& out_sha256) const {
        const auto it = pinned.find(filename);
        if (it == pinned.end()) return false;
        out_sha256 = it->second;
        return true;
    }
};

struct LoadResult {
    LoadStatus status = LoadStatus::Missing;
    Manifest manifest;
};

// Parse a manifest document from an in-memory UTF-8 JSON string. Never throws.
LoadResult ParseManifestJson(std::string_view json);

// Load from %LOCALAPPDATA%\Emebala\Common\models\manifest.json.
LoadResult LoadDefaultManifest();

// ---- hash verification (caller-injected provider, see header note) ----------

// Hash provider signature: mirrors engine_core's
//   bool ComputeFileSha256(const std::filesystem::path&, std::string& out_hex)
// The host/worker (which link Emebalachat_engine_core) inject it; core code
// never links engine_core (T1 boundary rule).
using HashProvider = std::function<bool(const std::filesystem::path&, std::string&)>;

enum class HashCheckStatus {
    AllPinsMatch,       // every bundled file's hash equals its pinned value
    Missing,            // a bundled model file does not exist on disk
    NotPinned,          // a bundled model file has NO manifest entry -> reject (§V2-6)
    HashMismatch,       // file hash != pinned value (corrupt/tampered)
    HashProviderFailed, // the injected provider could not hash the file
};

struct HashCheckResult {
    HashCheckStatus status = HashCheckStatus::AllPinsMatch;
    std::string detail; // filename or reason token (shape-only, no content)
};

// Verify the given model files against the manifest. `models_dir` is the
// Common\models directory the bare filenames resolve against. `origin_user`
// files must NOT be passed here (origin:"user" is manifest-exempt, §V2-6 —
// the CALLER filters; this function treats every input as a bundled file).
// Never throws; a throwing provider is contained by this function.
HashCheckResult VerifyModelHashes(const Manifest& manifest,
                                  const std::filesystem::path& models_dir,
                                  const std::vector<std::string>& bundled_files,
                                  const HashProvider& hash_provider);

// Test seam: resolve the default models directory
// (%LOCALAPPDATA%\Emebala\Common\models) or empty when LOCALAPPDATA is absent.
std::filesystem::path DefaultModelsDir();

} // namespace engine_host_manifest
} // namespace emebalachat
