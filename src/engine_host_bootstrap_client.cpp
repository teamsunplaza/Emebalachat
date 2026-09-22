// engine_host_bootstrap_client implementation — REQ-005 (M6 T6, design §1.3/
// §9 R-3b, plan §V2-8.3). See the header for the contract. Only PRESENCE and
// (for repair) the manifest SHA-256 pin are ever inspected — user text is
// never read, logged, or transmitted. Shape-only ENGINEHOST/bootstrap/NNN.

#include "engine_host_bootstrap_client.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#include <shlobj.h>
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#include "diag_logger.hpp"          // DIAG_LOG / DIAG_F (shape-only)
#include "engine.hpp"               // kPinnedModelFilename (engine_core re-export)
#include "engine_host_json_util.hpp" // JsonParseObject / FindField (frozen primitives)
#include "unicode_utils.hpp"        // ToUtf8 / ToUtf16
// REQ-044 (P4-2): shared engine-host path constants (kEngineDirRel /
// kModelsDirRel / kOrchestratorExe / kWorkerExe / kRegistryJson, and since
// M7 A-1 the per-family worker-manifest scheme helpers) — replaces the local
// definitions that used to live at L44-49 below.
#include "engine_host_paths.hpp"    // REQ-044: shared path constants

// Fallback when the macro is somehow absent (a hand-rolled build that skipped
// CMake). Mirrors the CMake empty branch: expand to an empty string so the
// repair path stays disabled rather than failing to compile.
#ifndef EMEBALA_REPAIR_URL_VALUE
#define EMEBALA_REPAIR_URL_VALUE ""
#endif
#ifndef EMEBALA_REPAIR_URL
#define EMEBALA_REPAIR_URL EMEBALA_REPAIR_URL_VALUE
#endif

namespace emebalachat {
namespace engine_host_bootstrap {

namespace {

// REQ-044 (P4-2): the fixed component set (plan §7.1). Path constants moved
// to engine_host_paths.hpp (paths::kEngineDirRel etc.).
namespace paths = emebalachat::enginehost::paths;

// M7 A-1 (session 260922_0001): the worker manifest is no longer a FIXED
// filename in kRequired[]. The store hosts one manifest per worker family
// (worker.<family>.manifest; DEC-007 keeps the hyphenated family strings
// verbatim) plus, on pre-A-1 installs, the legacy bare worker.manifest. The
// requirement gate below resolves the translate-family manifest through the
// read scheme (new name first, legacy fallback) instead of hardcoding a
// single constant, and EnumerateWorkerManifests() surfaces every manifest
// present. The manifest SCHEMA is unchanged (worker_protocol.hpp frozen).
constexpr char kWorkerFamilyTranslate[] = "ggml-translate";

// The required components, in check order. The model filename comes from the
// pinned kPinnedModelFilename (engine_core re-export via engine.hpp).
const RequiredComponent kRequired[] = {
    {RequiredComponent::Root::Engine, "Emebala.Engine.exe"},
    {RequiredComponent::Root::Engine, "Emebalachat.Engine.ggml-translate.exe"},
    {RequiredComponent::Root::Models, "registry.json"},
    // The pinned model is appended dynamically (kPinnedModelFilename is a
    // runtime string_view, not a constexpr), as is the translate-family
    // worker manifest (M7 A-1 — resolved per the read scheme, below).
};

std::string PinnedModelRelative() {
    return std::string(kPinnedModelFilename);
}

// SHA-256 of an empty stream, lowercase hex — used to reject a HashProvider
// that returns a constant (a mock must still distinguish files by content).
constexpr std::string_view kEmptySha256 =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

std::wstring CommonDir(const wchar_t* rel) {
    PWSTR known = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known)) || !known) {
        return {};
    }
    const std::wstring base = known;
    ::CoTaskMemFree(known);
    return base + L"\\" + rel;
}

bool FileExists(const std::filesystem::path& p) {
    const DWORD attrs = ::GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::filesystem::path RootDir(RequiredComponent::Root root) {
    return CommonDir(root == RequiredComponent::Root::Engine ? paths::kEngineDirRel : paths::kModelsDirRel);
}

std::string RootPrefix(RequiredComponent::Root root) {
    return root == RequiredComponent::Root::Engine ? "engine/" : "models/";
}

// M7 A-1: resolve the READ-scheme manifest name for one family inside `dir`:
// the canonical worker.<family>.manifest first; then, ONLY for the
// ggml-translate family, the pre-A-1 legacy bare worker.manifest (backward
// compatibility — the pre-A-1 store held exactly one manifest and it was the
// translate one; WRITES never choose the legacy name). A different family
// (e.g. ggml-asr) must NOT claim the legacy file as its own: its manifest
// only ever exists under the per-family name. Returns the chosen BARE
// filename as UTF-8, empty when no candidate belongs to this family.
// Pure presence check — file content is never touched here.
std::string ResolveManifestIn(const std::filesystem::path& dir,
                              const std::wstring& family_w) {
    if (dir.empty()) return {};
    const std::wstring modern = emebalachat::enginehost::paths::WorkerManifestName(family_w);
    if (FileExists(dir / modern)) return ToUtf8(modern);
    if (family_w == ToUtf16(kWorkerFamilyTranslate)) {
        const std::wstring legacy(emebalachat::enginehost::paths::kWorkerManifestLegacy);
        if (FileExists(dir / legacy)) return ToUtf8(legacy);
    }
    return {};
}

// Split "engine/x" / "models/x" into root + remainder. False when the prefix
// is unknown or the remainder is empty / not bare.
bool SplitPrefixed(const std::string& prefixed, RequiredComponent::Root& root, std::string& rel) {
    const std::string_view sv(prefixed);
    std::string_view body;
    if (sv.rfind("engine/", 0) == 0) {
        root = RequiredComponent::Root::Engine;
        body = sv.substr(7);
    } else if (sv.rfind("models/", 0) == 0) {
        root = RequiredComponent::Root::Models;
        body = sv.substr(7);
    } else {
        return false;
    }
    if (body.empty()) return false;
    rel.assign(body);
    return true;
}

bool IsBareName(std::string_view name) {
    if (name.empty() || name.size() > 128) return false;
    if (name == "." || name == "..") return false;
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '\0') return false;
    }
    return true;
}

bool IsHex64(std::string_view s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return true;
}

std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// ---- HTTPS download (WinHTTP, the google_translate.cpp discipline) ----------
// Downloads `url` (already known to start with https://) into `out_path` via a
// sibling temp file `<out_path>.download`. Bounded body (the manifest and the
// components are small; a 64 MiB cap mirrors the F5 DoS guard). Returns true
// only when the full body landed and the temp file is closed.
struct WinHttpHandleDeleter {
    void operator()(HINTERNET h) const {
        if (h) ::WinHttpCloseHandle(h);
    }
};
using ScopedHInternet = std::unique_ptr<void, WinHttpHandleDeleter>;

constexpr size_t kMaxDownloadBytes = 64 * 1024 * 1024; // 64 MiB cap (DoS guard)

bool HttpsDownloadToFile(const std::wstring& url, const std::filesystem::path& out_path) {
    // Decompose the https:// URL into host + path. The scheme prefix is
    // guaranteed by the caller; the remainder is split on the first '/'.
    const std::wstring rest = url.substr(std::wstring(L"https://").size());
    const size_t slash = rest.find(L'/');
    const std::wstring host = (slash == std::wstring::npos) ? rest : rest.substr(0, slash);
    const std::wstring path = (slash == std::wstring::npos) ? L"/" : rest.substr(slash);
    if (host.empty()) return false;

    ScopedHInternet session(::WinHttpOpen(
        L"EmebalaChat/1.0 (engine-host repair)",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return false;
    // Conservative bootstrap timeouts: the repair runs in the background, so a
    // slow network must never wedge startup (resolve/connect 5 s, io 15 s).
    ::WinHttpSetTimeouts(session.get(), 5000, 5000, 15000, 15000);

    ScopedHInternet connect(::WinHttpConnect(session.get(), host.c_str(),
                                             INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connect) return false;

    ScopedHInternet request(::WinHttpOpenRequest(connect.get(), L"GET", path.c_str(),
                                                 nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 WINHTTP_FLAG_SECURE));
    if (!request) return false;

    if (!::WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        return false;
    }
    if (!::WinHttpReceiveResponse(request.get(), nullptr)) return false;

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    ::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                          WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        DIAG_F("ENGINEHOST/bootstrap/004: repair download HTTP %lu (host=%ls path_len=%zu)\n",
               status, host.c_str(), path.size());
        return false;
    }

    const std::filesystem::path tmp = out_path.wstring() + L".download";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        DWORD avail = 0;
        size_t total = 0;
        while (::WinHttpQueryDataAvailable(request.get(), &avail) && avail > 0) {
            if (total + static_cast<size_t>(avail) > kMaxDownloadBytes) {
                DIAG_F("ENGINEHOST/bootstrap/005: repair download exceeded %zu-byte cap\n",
                       kMaxDownloadBytes);
                out.close();
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                return false;
            }
            std::vector<char> buf(avail);
            DWORD read = 0;
            if (!::WinHttpReadData(request.get(), buf.data(), avail, &read) || read == 0) break;
            out.write(buf.data(), static_cast<std::streamsize>(read));
            if (!out) {
                out.close();
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                return false;
            }
            total += read;
        }
        if (total == 0) {
            out.close();
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    return true;
}

} // namespace

std::string_view CompiledRepairUrl() {
    // The macro stringizes EMEBALA_REPAIR_URL; when the build did not provide
    // a value it expands to an empty string (see CMakeLists.txt), which the
    // caller treats as "repair disabled".
    return EMEBALA_REPAIR_URL_VALUE;
}

ParseStatus ParseRepairManifest(std::string_view json, std::vector<RepairFile>& out) {
    out.clear();
    enginehost::JsonPairs pairs;
    if (!enginehost::JsonParseObject(json, pairs)) return ParseStatus::NotJson;
    const auto* sv = enginehost::detail::FindField(pairs, "schema_version");
    int schema = 0;
    if (!sv || !enginehost::detail::ParseInt(sv->text, schema) || schema != 1) {
        return ParseStatus::SchemaVersion; // missing / unparseable / != 1 (fail-closed)
    }
    const auto* files = enginehost::detail::FindField(pairs, "files");
    // files must be a nested object; the flat JsonPairs view keeps the raw
    // source text of the value, which we re-parse as an object.
    if (!files) return ParseStatus::Malformed;
    // The flat parser records a nested object's raw source text in `text`.
    enginehost::JsonPairs file_pairs;
    if (!enginehost::JsonParseObject(files->text, file_pairs)) return ParseStatus::Malformed;
    for (const auto& [name, value] : file_pairs) {
        if (!IsBareName(name)) return ParseStatus::Malformed; // path traversal guard
        if (!value.is_string || !IsHex64(value.text)) return ParseStatus::Malformed;
        RepairFile rf;
        rf.name = name;
        rf.sha256 = ToLowerAscii(value.text);
        out.push_back(std::move(rf));
    }
    if (out.empty()) return ParseStatus::Malformed; // a manifest with no files is useless
    return ParseStatus::Ok;
}

ComponentCheckResult CheckComponents() {
    ComponentCheckResult r;
    const std::filesystem::path engine_dir = RootDir(RequiredComponent::Root::Engine);
    const std::filesystem::path models_dir = RootDir(RequiredComponent::Root::Models);
    r.engine_dir_resolved = !engine_dir.empty();
    r.models_dir_resolved = !models_dir.empty();

    auto check_one = [&](RequiredComponent::Root root, const std::string& rel) {
        const std::filesystem::path dir = RootDir(root);
        if (dir.empty()) {
            // LOCALAPPDATA unresolved: report as missing so the caller converges
            // on guidance (cannot repair what cannot be located).
            r.missing.push_back(RootPrefix(root) + rel);
            return;
        }
        if (!FileExists(dir / ToUtf16(rel))) {
            r.missing.push_back(RootPrefix(root) + rel);
        }
    };

    for (const RequiredComponent& c : kRequired) check_one(c.root, c.relative);

    // M7 A-1: the translate-family worker manifest resolves through the READ
    // scheme (worker.ggml-translate.manifest first, legacy worker.manifest as
    // the backward-compatible fallback). When neither exists, report the
    // CANONICAL new-scheme name as missing — repair then fetches/installs the
    // new-scheme file only (writes never choose the legacy name). The missing
    // string is emitted by the same engine/ prefix machinery as the fixed
    // components, so RepairMissingComponents treats it identically.
    {
        const std::wstring family_w = ToUtf16(kWorkerFamilyTranslate);
        const std::string present = ResolveManifestIn(engine_dir, family_w);
        check_one(RequiredComponent::Root::Engine,
                  present.empty() ? ToUtf8(paths::WorkerManifestName(family_w)) : present);
    }

    check_one(RequiredComponent::Root::Models, PinnedModelRelative());
    return r;
}

std::vector<std::string> EnumerateWorkerManifestsIn(const std::filesystem::path& engine_dir) {
    std::vector<std::string> out;
    if (engine_dir.empty()) return out;
    std::error_code ec;
    std::filesystem::directory_iterator it(engine_dir,
                                           std::filesystem::directory_options::skip_permission_denied,
                                           ec);
    if (ec) return out; // absent/unreadable dir -> empty enumeration (never throws)
    const std::filesystem::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec) continue;
        const std::wstring name = it->path().filename().wstring();
        if (paths::IsWorkerManifestName(name)) {
            out.push_back(ToUtf8(name));
        }
    }
    std::sort(out.begin(), out.end()); // deterministic order for callers/tests
    return out;
}

std::vector<std::string> EnumerateWorkerManifests() {
    return EnumerateWorkerManifestsIn(RootDir(RequiredComponent::Root::Engine));
}

std::string ResolveWorkerManifestName(const std::filesystem::path& dir,
                                      const std::string& family) {
    return ResolveManifestIn(dir, ToUtf16(family));
}

std::filesystem::path ResolveTargetPath(const std::string& prefixed_relative) {
    RequiredComponent::Root root;
    std::string rel;
    if (!SplitPrefixed(prefixed_relative, root, rel)) return {};
    const std::filesystem::path dir = RootDir(root);
    if (dir.empty()) return {};
    return dir / ToUtf16(rel);
}

bool WaitForComponentsPresent(const std::function<bool()>& check, int attempts,
                              std::chrono::milliseconds interval) {
    // REQ-048 P1: first check runs IMMEDIATELY (no pre-wait) so an
    // already-complete component set returns at once; only the re-checks sleep.
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(interval);
        }
        if (check && check()) return true;
    }
    return false;
}

const char* RepairOutcomeToString(RepairOutcome o) {
    switch (o) {
        case RepairOutcome::NothingToRepair: return "nothing_to_repair";
        case RepairOutcome::Repaired:        return "repaired";
        case RepairOutcome::RepairDisabled:  return "repair_disabled";
        case RepairOutcome::RepairNotNeeded: return "repair_not_needed";
        case RepairOutcome::DownloadFailed:  return "download_failed";
        case RepairOutcome::ManifestRejected: return "manifest_rejected";
        case RepairOutcome::HashMismatch:    return "hash_mismatch";
        case RepairOutcome::InstallFailed:   return "install_failed";
    }
    return "unknown";
}

RepairOutcome RepairMissingComponents(const std::vector<std::string>& missing,
                                      std::string_view url,
                                      const HashProvider& hash) {
    if (missing.empty()) return RepairOutcome::NothingToRepair;

    // https://-only (design §10 "임의 URL 금지"): anything else is rejected
    // outright — the repair never talks to a non-TLS endpoint.
    constexpr std::string_view kHttpsPrefix = "https://";
    if (url.rfind(kHttpsPrefix, 0) != 0) {
        DIAG_F("ENGINEHOST/bootstrap/001: repair URL is empty or not https:// — repair disabled (len=%zu scheme_ok=%d)\n",
               url.size(), url.rfind(kHttpsPrefix, 0) == 0 ? 1 : 0);
        return RepairOutcome::RepairDisabled; // empty OR non-https -> disabled
    }
    if (!hash) {
        DIAG_F("ENGINEHOST/bootstrap/002: no HashProvider injected — repair unavailable\n");
        return RepairOutcome::HashMismatch; // cannot verify -> fail-closed
    }

    // Fetch the manifest itself (bare "repair.json" resolved against the URL).
    const std::wstring url_w = ToUtf16(std::string(url));
    const std::wstring manifest_url = url_w + (url_w.back() == L'/' ? L"" : L"/") + L"repair.json";
    const std::filesystem::path scratch = RootDir(RequiredComponent::Root::Engine);
    if (scratch.empty()) {
        return RepairOutcome::InstallFailed; // no common dir to stage into
    }
    const std::filesystem::path manifest_tmp = scratch / L"repair.json.download";

    // One retry on transport failure (offline blip); a hash mismatch never retries.
    std::vector<RepairFile> files;
    bool manifest_ok = false;
    for (int attempt = 0; attempt < 2 && !manifest_ok; ++attempt) {
        std::error_code ec;
        std::filesystem::remove(manifest_tmp, ec);
        if (!HttpsDownloadToFile(manifest_url, scratch / L"repair.json")) {
            DIAG_F("ENGINEHOST/bootstrap/003: manifest download failed (attempt=%d)\n", attempt + 1);
            continue;
        }
        std::ifstream in(manifest_tmp, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        if (ParseRepairManifest(ss.str(), files) != ParseStatus::Ok) {
            DIAG_F("ENGINEHOST/bootstrap/006: repair manifest rejected (status=malformed)\n");
            std::error_code ec2;
            std::filesystem::remove(manifest_tmp, ec2);
            return RepairOutcome::ManifestRejected; // permanent — no retry
        }
        manifest_ok = true;
    }
    {
        std::error_code ec;
        std::filesystem::remove(manifest_tmp, ec);
    }
    if (!manifest_ok) return RepairOutcome::DownloadFailed; // offline / transport

    // Only repair the components we actually need; look each up by bare name.
    for (const std::string& prefixed : missing) {
        RequiredComponent::Root root;
        std::string rel;
        if (!SplitPrefixed(prefixed, root, rel)) {
            DIAG_F("ENGINEHOST/bootstrap/007: unexpected missing path shape (len=%zu)\n", prefixed.size());
            return RepairOutcome::ManifestRejected;
        }
        const std::string bare = std::filesystem::path(rel).filename().string();
        const RepairFile* pin = nullptr;
        for (const RepairFile& rf : files) {
            if (rf.name == bare) {
                pin = &rf;
                break;
            }
        }
        if (!pin) {
            DIAG_F("ENGINEHOST/bootstrap/008: manifest has no entry for %s\n", bare.c_str());
            return RepairOutcome::ManifestRejected; // cannot verify -> fail-closed
        }

        const std::filesystem::path target_dir = RootDir(root);
        const std::filesystem::path target = target_dir / ToUtf16(rel);
        const std::wstring file_url = url_w + (url_w.back() == L'/' ? L"" : L"/") +
                                      ToUtf16(pin->name);

        bool downloaded = false;
        for (int attempt = 0; attempt < 2 && !downloaded; ++attempt) {
            std::error_code ec;
            std::filesystem::remove(target.wstring() + L".download", ec);
            if (!HttpsDownloadToFile(file_url, target)) {
                DIAG_F("ENGINEHOST/bootstrap/009: %s download failed (attempt=%d)\n",
                       bare.c_str(), attempt + 1);
                continue;
            }
            downloaded = true;
        }
        if (!downloaded) {
            std::error_code ec;
            std::filesystem::remove(target.wstring() + L".download", ec);
            return RepairOutcome::DownloadFailed;
        }

        // SHA-256 pin via the injected provider (a constant provider is caught
        // by the empty-file sentinel). Case-insensitive hex compare.
        std::string actual;
        const std::filesystem::path staged = std::filesystem::path(target.wstring() + L".download");
        if (!hash(staged, actual) || !IsHex64(actual)) {
            DIAG_F("ENGINEHOST/bootstrap/010: %s hash provider failed\n", bare.c_str());
            std::error_code ec;
            std::filesystem::remove(staged, ec);
            return RepairOutcome::HashMismatch;
        }
        if (actual == std::string(kEmptySha256)) {
            DIAG_F("ENGINEHOST/bootstrap/011: %s hash provider returned the empty sentinel (constant mock?)\n",
                   bare.c_str());
            std::error_code ec;
            std::filesystem::remove(staged, ec);
            return RepairOutcome::HashMismatch;
        }
        if (ToLowerAscii(actual) != pin->sha256) {
            DIAG_F("ENGINEHOST/bootstrap/012: %s SHA-256 mismatch (pin mismatch)\n", bare.c_str());
            std::error_code ec;
            std::filesystem::remove(staged, ec);
            return RepairOutcome::HashMismatch; // permanent — no retry
        }

        // Atomic-ish install: back the existing file up to .prev, then move the
        // verified download into place (§V2-8.4 minimal form). std::filesystem
        // rename is MoveFileW under the hood — atomic within one volume (both
        // files live in the same target dir).
        std::error_code ec;
        const std::filesystem::path prev = std::filesystem::path(target.wstring() + L".prev");
        std::filesystem::remove(prev, ec); // drop a stale .prev from a prior run
        ec.clear();
        if (FileExists(target)) {
            std::filesystem::rename(target, prev, ec);
            if (ec) {
                DIAG_F("ENGINEHOST/bootstrap/013: %s .prev backup failed\n", bare.c_str());
                std::error_code ec2;
                std::filesystem::remove(staged, ec2);
                return RepairOutcome::InstallFailed;
            }
        }
        ec.clear();
        std::filesystem::rename(staged, target, ec);
        if (ec) {
            DIAG_F("ENGINEHOST/bootstrap/014: %s install move failed\n", bare.c_str());
            // Best-effort rollback so the engine dir is never left without the file.
            std::error_code ec2;
            if (FileExists(prev)) std::filesystem::rename(prev, target, ec2);
            std::filesystem::remove(staged, ec2);
            return RepairOutcome::InstallFailed;
        }
        DIAG_LOG("ENGINEHOST", "bootstrap/015: repaired %s (sha256 ok)", bare.c_str());
    }

    return RepairOutcome::Repaired;
}

} // namespace engine_host_bootstrap
} // namespace emebalachat
