// ---------------------------------------------------------------------------
// engine_host_manifest.cpp — REQ-043 (M6 T2, design §3.2): manifest.json
// parser + IManifestVerifier implementations. See the header for the contract.
// ---------------------------------------------------------------------------

#include "engine_host_manifest.hpp"

#include <algorithm>
#include <cctype> // std::tolower (defensive hash-case normalization)

#include "diag_logger.hpp"

namespace emebalachat {
namespace engine_host_manifest {

namespace {

// Exactly 64 lowercase hex chars — the sha256 form the installer writes and
// ComputeFileSha256 (engine_core) emits.
bool IsSha256Hex(std::string_view s) {
    if (s.size() != 64) return false;
    for (const char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

} // namespace

// ---- IManifestVerifier implementations --------------------------------------

bool DevTrustVerifier::VerifySignature(const std::filesystem::path& manifest_path) {
    // REQ-043 development-mode trust anchor: no code-signing certificate is
    // provisioned yet (design §9 R-2), so the installer-written manifest is
    // trusted WITHOUT cryptographic verification. The explicit lower bound is
    // announced ONCE per process (shape-only: path presence, never content).
    static bool announced = false;
    if (!announced) {
        announced = true;
        const bool has_path = !manifest_path.empty();
        DIAG_F("ENGINEHOST/Manifest/001: dev-trust mode active — manifest "
               "signature verification SKIPPED (no certificate provisioned; "
               "installer-written manifest trusted; path_resolved=%d)\n",
               has_path ? 1 : 0);
    }
    return true;
}

bool AuthenticodeVerifier::VerifySignature(const std::filesystem::path& manifest_path) {
    (void)manifest_path;
    // REQ-043 TODO (replace when the Emebala code-signing certificate is
    // provisioned — design §3.2/§10, plan §V2-6): call WinVerifyTrust with
    // WINTRUST_ACTION_GENERIC_VERIFY_V2 against this manifest path and pin
    // the signer chain to the Emebala certificate. Until then this
    // implementation must stay FAIL-CLOSED: returning true here would accept
    // an unverifiable certificate chain quietly, which is strictly less safe
    // than the documented dev-mode lower bound (DevTrustVerifier).
    DIAG_F("ENGINEHOST/Manifest/002: AuthenticodeVerifier consulted but the "
           "certificate chain is not provisioned (certificate pending) — "
           "fail-closed reject\n");
    return false;
}

std::unique_ptr<IManifestVerifier> CreateManifestVerifier(const char* mode) {
    if (mode && std::string_view(mode) == "authenticode") {
        return std::make_unique<AuthenticodeVerifier>();
    }
    // "dev" (default) and any unknown mode string land here: dev-trust is the
    // documented development lower bound (design §10).
    return std::make_unique<DevTrustVerifier>();
}

// ---- document parsing ---------------------------------------------------------

LoadResult ParseManifestJson(std::string_view json) {
    LoadResult result;
    enginehost::JsonPairs doc;
    if (!enginehost::JsonParseObject(json, doc)) {
        result.status = LoadStatus::NotJson;
        return result;
    }

    // schema_version: REQUIRED == 1 (fail-closed, §V2-12-2).
    int schema = 0;
    if (!engine_host_json::GetInt(doc, "schema_version", schema) ||
        schema != kManifestSchemaVersion) {
        result.status = LoadStatus::SchemaVersion;
        DIAG_F("ENGINEHOST/Manifest/003: manifest.json rejected (schema_version "
               "missing or unsupported, got=%d)\n", schema);
        return result;
    }
    result.manifest.schema_version = schema;

    // pinned: REQUIRED object. Keys are bare filenames; values are 64-hex
    // sha256 strings. Unknown fields elsewhere are ignored; unknown PINS are
    // contract data and preserved verbatim.
    const enginehost::JsonValue* pinned = enginehost::detail::FindField(doc, "pinned");
    if (!pinned || pinned->is_string || pinned->text.empty() ||
        pinned->text.front() != '{') {
        result.status = LoadStatus::Malformed;
        DIAG_F("ENGINEHOST/Manifest/004: manifest.json rejected (pinned{} "
               "missing or wrong-typed)\n");
        return result;
    }
    enginehost::JsonPairs pins;
    if (!enginehost::JsonParseObject(pinned->text, pins)) {
        result.status = LoadStatus::Malformed;
        DIAG_F("ENGINEHOST/Manifest/005: manifest.json rejected (pinned{} not "
               "parseable)\n");
        return result;
    }
    for (const auto& [name, val] : pins) {
        if (!engine_host_json::IsBareFilename(name)) {
            // A path escape in a pin key is tampering: reject the DOCUMENT
            // (fail-closed beats ignoring a single hostile entry).
            result.status = LoadStatus::Malformed;
            DIAG_F("ENGINEHOST/Manifest/006: manifest.json rejected (pinned key "
                   "is not a bare filename, len=%zu)\n", name.size());
            result.manifest.pinned.clear();
            return result;
        }
        if (!val.is_string || !IsSha256Hex(val.text)) {
            result.status = LoadStatus::Malformed;
            DIAG_F("ENGINEHOST/Manifest/007: manifest.json rejected (pinned "
                   "value for '%s' is not a 64-hex sha256)\n", name.c_str());
            result.manifest.pinned.clear();
            return result;
        }
        result.manifest.pinned.emplace(name, val.text);
    }
    result.status = LoadStatus::Ok;
    return result;
}

// ---- hash verification ---------------------------------------------------------

HashCheckResult VerifyModelHashes(const Manifest& manifest,
                                  const std::filesystem::path& models_dir,
                                  const std::vector<std::string>& bundled_files,
                                  const HashProvider& hash_provider) {
    HashCheckResult result;
    if (!hash_provider) {
        result.status = HashCheckStatus::HashProviderFailed;
        result.detail = "no_provider";
        return result;
    }
    // §V2-6: EVERY bundled model file must carry a manifest pin — an unpinned
    // file is rejected even before it is hashed (order matters: the manifest
    // is the authority for what may exist in Common\models at all).
    for (const auto& file : bundled_files) {
        std::string pinned_sha;
        if (!manifest.FindPin(file, pinned_sha)) {
            result.status = HashCheckStatus::NotPinned;
            result.detail = file;
            DIAG_F("ENGINEHOST/Manifest/008: bundled model file has no manifest "
                   "pin — rejected (name_len=%zu)\n", file.size());
            return result;
        }
    }
    for (const auto& file : bundled_files) {
        std::string pinned_sha;
        (void)manifest.FindPin(file, pinned_sha); // guaranteed pinned by pass 1
        const std::filesystem::path full = models_dir /
            std::filesystem::path(file.begin(), file.end());
        std::error_code ec;
        if (!std::filesystem::is_regular_file(full, ec) || ec) {
            result.status = HashCheckStatus::Missing;
            result.detail = file;
            DIAG_F("ENGINEHOST/Manifest/009: pinned model file missing on disk "
                   "(name_len=%zu)\n", file.size());
            return result;
        }
        std::string actual_sha;
        bool hashed = false;
        try {
            hashed = hash_provider(full, actual_sha);
        } catch (...) {
            hashed = false; // a throwing provider degrades to provider-failed
        }
        if (!hashed) {
            result.status = HashCheckStatus::HashProviderFailed;
            result.detail = file;
            DIAG_F("ENGINEHOST/Manifest/010: hash provider failed for a pinned "
                   "model file (name_len=%zu)\n", file.size());
            return result;
        }
        // Normalize case for the comparison; both sides are lowercase hex by
        // contract, but a defensive lowercase never weakens the check.
        std::string actual = actual_sha;
        std::transform(actual.begin(), actual.end(), actual.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (actual != pinned_sha) {
            result.status = HashCheckStatus::HashMismatch;
            result.detail = file;
            DIAG_F("ENGINEHOST/Manifest/011: model file hash != pinned manifest "
                   "hash — rejected (name_len=%zu)\n", file.size());
            return result;
        }
    }
    result.status = HashCheckStatus::AllPinsMatch;
    return result;
}

std::filesystem::path DefaultModelsDir() {
    std::filesystem::path dir;
    size_t len = 0;
    if (_wgetenv_s(&len, nullptr, 0, L"LOCALAPPDATA") == 0 && len > 1) {
        std::wstring v(len, L'\0');
        if (_wgetenv_s(&len, &v[0], len, L"LOCALAPPDATA") == 0) {
            while (!v.empty() && v.back() == L'\0') v.pop_back();
            if (!v.empty()) {
                dir = std::filesystem::path(v) / L"Emebala" / L"Common" / L"models";
            }
        }
    }
    return dir;
}

LoadResult LoadDefaultManifest() {
    LoadResult result;
    const std::filesystem::path dir = DefaultModelsDir();
    if (dir.empty()) {
        result.status = LoadStatus::Missing;
        return result;
    }
    const std::filesystem::path path = dir / L"manifest.json";
    std::string text;
    switch (engine_host_json::ReadTextFileUtf8(path, text)) {
        case engine_host_json::FileReadOutcome::Ok:
            break;
        case engine_host_json::FileReadOutcome::Missing:
            result.status = LoadStatus::Missing;
            return result;
        case engine_host_json::FileReadOutcome::ReadError:
            result.status = LoadStatus::ReadError;
            DIAG_F("ENGINEHOST/Manifest/012: manifest.json exists but could not "
                   "be read (locked/permission); serving an empty manifest\n");
            return result;
    }
    return ParseManifestJson(text);
}

} // namespace engine_host_manifest
} // namespace emebalachat
