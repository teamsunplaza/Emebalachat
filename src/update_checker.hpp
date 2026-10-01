#pragma once

// ---------------------------------------------------------------------------
// REQ-UC (session 260930_0004, update-checker track): the app-side manual
// update checker v1. A MANUAL "Check for updates" button lives in the About
// window (src/ui/about_window.cpp renders the six-state update zone per the
// design spec docs/260930_0004_session_update-checker/design/spec.md); this
// module is the brain + hands behind it:
//
//   updatelogic::  PURE seams, headless-unit-testable, NO network/file I/O:
//                  version parse/compare, minimal fail-closed JSON field
//                  extraction for the GitHub /releases/latest payload, the
//                  release-notes "SHA256:" hash-line parser, the download
//                  target path builder, MB size formatting, https URL split.
//   (root)         Thin WinHTTP/CNG I/O on worker threads (blocking calls,
//                  never on the GUI thread): CheckForUpdate, DownloadUpdate
//                  (progress callback + cancel + partial-file cleanup),
//                  ComputeFileSha256Hex (BCrypt, streaming), LaunchInstaller.
//
// Privacy/failure contract (design spec + session intent): failures are QUIET
// — the UI shows a single inline line, never a modal. Network errors and the
// GitHub 403 rate-limit map to dedicated quiet states; a missing/malformed
// release-notes hash line is a verification FAILURE (fail-closed), never a
// silent pass. Only https URLs are ever fetched (the redirect followers
// refuse other schemes AND any host outside the release-infra allowlist).
// Diagnostics are shape-only (DIAG_F, no content; network-derived strings are
// control-char sanitized before logging).
//
// Integrity threat model (D6, security review 260930_0004 Finding 4): the
// SHA-256 gate defends the publisher->user TRANSIT. The expected hash rides
// the same releases/latest response as the asset URL, so publisher-side
// compromise of the GitHub release defeats the scheme by design (an embedded
// per-version hash is infeasible: v0.12.0's digest cannot be known when
// v0.10.2 ships; Authenticode/WinVerifyTrust is the documented future
// upgrade). Convention: the FIRST "SHA256:" line binds to the FIRST ".exe"
// asset — per-file checksum lists must keep the installer line first, else
// verification fails closed (availability, never integrity).
//
// BUILD NOTE (CMake boundary): the version-stamp stream owns CMakeLists.txt
// for this session, so this module is deliberately HEADER-ONLY (every
// function `inline`). about_window.cpp and run_tests.exe include it directly
// and link with zero CMake changes; update_checker.cpp is a pure
// include-the-header TU that the orchestrator can add to the Emebalachat_core
// source list later without touching any call site.
// ---------------------------------------------------------------------------

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include "diag_logger.hpp"
#include "unicode_utils.hpp" // AppendJsonUnicodeEscape (\u escape decode), ToUtf16/ToUtf8
#include "version.hpp"       // kAppVersionA (single source of truth)

namespace emebalachat {

// REQ-UC: GitHub REST endpoint for the latest (non-prerelease) release. The
// repo slug is factual brand data (never translated).
inline constexpr wchar_t kUpdateApiHost[] = L"api.github.com";
inline constexpr char    kUpdateApiPath[] = "/repos/teamsunplaza/Emebalachat/releases/latest";

// REQ-UC: truthful product UA (GitHub REQUIRES a User-Agent header; 403
// otherwise — google_translate.hpp carries the same product-UA pattern for
// its public endpoint). Built at runtime from kAppVersionW so version bumps
// cannot drift the UA string.
inline std::wstring UpdateCheckerUserAgent() {
    return L"Emebalachat/" + std::wstring(kAppVersionW) +
           L" (+https://github.com/teamsunplaza/Emebalachat)";
}

// ===========================================================================
// Pure logic (headless-testable; no I/O, no COM, no network)
// ===========================================================================
namespace updatelogic {

// REQ-UC: numeric version triplet. Pre-release/build suffixes are stripped at
// parse time (the /latest endpoint already excludes prereleases, and the
// session spec mandates ignoring them in comparisons anyway).
struct VersionTriplet {
    int major = 0;
    int minor = 0;
    int patch = 0;
};

// REQ-UC: parse "v0.10.2" / "0.10.2" / "1.2" / "v2" (1..3 dot-separated
// numeric components, optional leading 'v'/'V', optional -pre/+build suffix).
// Anything else (empty, non-numeric, 4+ components, signs) is MALFORMED ->
// nullopt (fail-closed: a malformed remote tag can never read as "newer").
inline std::optional<VersionTriplet> ParseVersion(std::string_view text) {
    std::string t(text);
    const size_t cut = t.find_first_of("-+");
    if (cut != std::string::npos) t.resize(cut); // strip prerelease/build
    if (!t.empty() && (t.front() == 'v' || t.front() == 'V')) t.erase(t.begin());
    if (t.empty()) return std::nullopt;

    VersionTriplet v;
    int component = 0;
    size_t pos = 0;
    while (pos <= t.size()) {
        const size_t dot = t.find('.', pos);
        const std::string part =
            t.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
        if (part.empty() || part.size() > 9) return std::nullopt;
        int value = 0;
        for (const char c : part) {
            if (c < '0' || c > '9') return std::nullopt;
            value = value * 10 + (c - '0');
        }
        if (component == 0) v.major = value;
        else if (component == 1) v.minor = value;
        else if (component == 2) v.patch = value;
        else return std::nullopt; // 4+ components
        ++component;
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    if (component == 0) return std::nullopt;
    return v;
}

// REQ-UC: three-way numeric triplet compare: <0 a<b, 0 equal, >0 a>b.
inline int CompareVersions(const VersionTriplet& a, const VersionTriplet& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

struct ReleaseAsset {
    std::string name;
    std::string download_url;
    std::uint64_t size = 0; // bytes (GitHub "size" field; 0 when absent)
};

struct ReleaseInfo {
    std::string tag;      // "tag_name", e.g. "v0.11.0"
    std::string html_url; // release page (diagnostics only)
    std::string body;     // release notes (carries the SHA256: hash line)
    std::vector<ReleaseAsset> assets;
};

// ---- tiny fail-closed JSON field scanner ---------------------------------
// Hand-rolled for exactly the fields the flow consumes. Malformed input at
// any decision point fails the WHOLE parse (nullopt), never yields a
// half-known release.

inline void JsonSkipWs(std::string_view s, size_t& p) {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
}

// Parse a JSON string starting at the opening quote (s[p] == '"').
inline bool JsonParseStringAt(std::string_view s, size_t& p, std::string& out) {
    if (p >= s.size() || s[p] != '"') return false;
    ++p;
    out.clear();
    while (p < s.size()) {
        const char c = s[p++];
        if (c == '"') return true;
        if (c == '\\') {
            if (p >= s.size()) return false;
            const char esc = s[p++];
            switch (esc) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u':
                    if (!AppendJsonUnicodeEscape(out, s, p)) return false; // lone surrogates -> U+FFFD
                    break;
                default: return false; // unknown escape: fail-closed
            }
        } else {
            out += c;
        }
    }
    return false; // unterminated
}

// Parse-at-position convenience for const positions (JsonFindObjectValue
// results): the caller's position is not advanced.
inline bool JsonParseStringValue(std::string_view s, size_t pos, std::string& out) {
    return JsonParseStringAt(s, pos, out);
}

// REQ-UC SEC (security review 260930_0004, Finding 3 / CWE-674): the JSON
// walker recurses per nesting level; the response body may come from a
// compromised endpoint, so nesting is hard-capped. Past the cap the input is
// treated as malformed (fail-closed) and the parse rejects the release.
inline constexpr unsigned kMaxJsonNestingDepth = 64;

// Skip one JSON value of any kind starting at p (primitive, string, array, or
// nested object — depth-walked, depth-capped). Returns false on malformed
// input or when `depth` exceeds kMaxJsonNestingDepth.
inline bool JsonSkipValue(std::string_view s, size_t& p, unsigned depth = 0) {
    if (depth > kMaxJsonNestingDepth) return false;
    JsonSkipWs(s, p);
    if (p >= s.size()) return false;
    const char c = s[p];
    if (c == '"') {
        std::string dummy;
        return JsonParseStringAt(s, p, dummy);
    }
    if (c == '{') {
        ++p;
        JsonSkipWs(s, p);
        if (p < s.size() && s[p] == '}') { ++p; return true; }
        while (p < s.size()) {
            std::string key;
            if (!JsonParseStringAt(s, p, key)) return false;
            JsonSkipWs(s, p);
            if (p >= s.size() || s[p] != ':') return false;
            ++p;
            if (!JsonSkipValue(s, p, depth + 1)) return false;
            JsonSkipWs(s, p);
            if (p < s.size() && s[p] == ',') { ++p; JsonSkipWs(s, p); continue; }
            if (p < s.size() && s[p] == '}') { ++p; return true; }
            return false;
        }
        return false;
    }
    if (c == '[') {
        ++p;
        JsonSkipWs(s, p);
        if (p < s.size() && s[p] == ']') { ++p; return true; }
        while (p < s.size()) {
            if (!JsonSkipValue(s, p, depth + 1)) return false;
            JsonSkipWs(s, p);
            if (p < s.size() && s[p] == ',') { ++p; continue; }
            if (p < s.size() && s[p] == ']') { ++p; return true; }
            return false;
        }
        return false;
    }
    // primitive: number/true/false/null — consume until , } ] or whitespace
    while (p < s.size() && s[p] != ',' && s[p] != '}' && s[p] != ']' &&
           s[p] != ' ' && s[p] != '\t' && s[p] != '\r' && s[p] != '\n') {
        ++p;
    }
    return true;
}

// Find the value position of `key` inside the object whose '{' sits at
// `obj_start`. Only the object's OWN keys are considered (nested objects are
// skipped whole via JsonSkipValue, depth-capped). Returns the value position,
// or nullopt when the key is absent / the object is malformed.
inline std::optional<size_t> JsonFindObjectValue(std::string_view s, size_t obj_start,
                                                 std::string_view key,
                                                 unsigned depth = 0) {
    size_t p = obj_start;
    JsonSkipWs(s, p);
    if (p >= s.size() || s[p] != '{') return std::nullopt;
    ++p;
    JsonSkipWs(s, p);
    if (p < s.size() && s[p] == '}') return std::nullopt;
    while (p < s.size()) {
        std::string k;
        if (!JsonParseStringAt(s, p, k)) return std::nullopt;
        JsonSkipWs(s, p);
        if (p >= s.size() || s[p] != ':') return std::nullopt;
        ++p;
        JsonSkipWs(s, p);
        if (k == key) return p;
        if (!JsonSkipValue(s, p, depth + 1)) return std::nullopt;
        JsonSkipWs(s, p);
        if (p < s.size() && s[p] == ',') { ++p; JsonSkipWs(s, p); continue; }
        if (p < s.size() && s[p] == '}') return std::nullopt;
        return std::nullopt;
    }
    return std::nullopt;
}

// Parse a non-negative integer at p (JSON number, digits only after ws).
inline bool JsonParseUint64At(std::string_view s, size_t p, std::uint64_t& out) {
    JsonSkipWs(s, p);
    std::uint64_t v = 0;
    bool any = false;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
        any = true;
        const unsigned digit = static_cast<unsigned>(s[p] - '0');
        if (v > (UINT64_MAX - digit) / 10) { out = UINT64_MAX; return true; } // clamp overflow
        v = v * 10 + digit;
        ++p;
    }
    if (!any) return false;
    out = v;
    return true;
}

// REQ-UC: parse the GitHub GET /repos/{owner}/{repo}/releases/latest payload
// into exactly what the flow needs. tag_name is REQUIRED (absent/malformed ->
// nullopt, fail-closed). assets[] entries keep browser_download_url + name +
// size; an entry without a URL cannot be picked and is skipped.
inline std::optional<ReleaseInfo> ParseLatestRelease(std::string_view json) {
    size_t root = 0;
    JsonSkipWs(json, root);
    if (root >= json.size() || json[root] != '{') return std::nullopt;

    // Whole-document well-formedness gate (fail-closed): an unterminated
    // string or a broken nested structure ANYWHERE in the payload rejects the
    // release, even when the four fields we read parsed cleanly first.
    {
        size_t validate = root;
        if (!JsonSkipValue(json, validate)) return std::nullopt;
    }

    ReleaseInfo info;
    if (const auto p = JsonFindObjectValue(json, root, "tag_name")) {
        if (!JsonParseStringValue(json, *p, info.tag) || info.tag.empty()) return std::nullopt;
    } else {
        return std::nullopt; // no tag -> no release
    }
    if (const auto p = JsonFindObjectValue(json, root, "html_url")) {
        JsonParseStringValue(json, *p, info.html_url); // best-effort (diagnostics)
    }
    if (const auto p = JsonFindObjectValue(json, root, "body")) {
        JsonParseStringValue(json, *p, info.body); // best-effort
    }
    if (const auto p = JsonFindObjectValue(json, root, "assets")) {
        size_t a = *p;
        JsonSkipWs(json, a);
        if (a < json.size() && json[a] == '[') {
            ++a;
            JsonSkipWs(json, a);
            if (a < json.size() && json[a] == ']') {
                ++a; // empty assets array
            } else {
                while (a < json.size()) {
                    JsonSkipWs(json, a);
                    if (a < json.size() && json[a] == ']') { ++a; break; }
                    if (json[a] != '{') break;
                    const size_t obj = a; // depth 1 (an element of the root array)
                    ReleaseAsset asset;
                    if (const auto up = JsonFindObjectValue(json, obj, "browser_download_url", 1)) {
                        JsonParseStringValue(json, *up, asset.download_url);
                    }
                    if (!asset.download_url.empty()) {
                        if (const auto np = JsonFindObjectValue(json, obj, "name", 1)) {
                            JsonParseStringValue(json, *np, asset.name);
                        }
                        if (const auto sp = JsonFindObjectValue(json, obj, "size", 1)) {
                            JsonParseUint64At(json, *sp, asset.size);
                        }
                        info.assets.push_back(std::move(asset));
                    }
                    if (!JsonSkipValue(json, a)) break; // consume this object (a sits at '{')
                    JsonSkipWs(json, a);
                    if (a < json.size() && json[a] == ',') { ++a; continue; }
                    if (a < json.size() && json[a] == ']') { ++a; break; }
                    break;
                }
            }
        }
    }
    return info;
}

// REQ-UC: find the verification hash line in the release notes: a line whose
// first non-blank content is "SHA256:" followed by exactly 64 hex chars (any
// case; trailing whitespace/CR tolerated). Returns the LOWERCASE digest, or
// nullopt when the line is ABSENT or MALFORMED — both are verification
// failures by design (fail-closed: no hash line, no install).
inline std::optional<std::string> ExtractSha256FromNotes(std::string_view body) {
    size_t pos = 0;
    while (pos <= body.size()) {
        const size_t eol = body.find('\n', pos);
        const std::string_view line = body.substr(
            pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        const size_t start = line.find_first_not_of(" \t");
        if (start != std::string_view::npos && line.substr(start, 7) == "SHA256:") {
            const std::string_view rest = line.substr(start + 7);
            const size_t hs = rest.find_first_not_of(" \t\r");
            if (hs == std::string_view::npos) return std::nullopt;
            const size_t he = rest.find_first_of(" \t\r", hs);
            const std::string_view token = rest.substr(
                hs, he == std::string_view::npos ? std::string_view::npos : he - hs);
            if (token.size() != 64) return std::nullopt;
            std::string out;
            out.reserve(64);
            for (const char c : token) {
                if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
                out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return out;
        }
        if (eol == std::string_view::npos) break;
        pos = eol + 1;
    }
    return std::nullopt;
}

// REQ-UC: the download target %TEMP%\Emebalachat_Update_<ver>.exe. The tag is
// sanitized to [A-Za-z0-9._-] (leading 'v' kept; path separators and every
// other character dropped), so the built path can never escape the temp dir
// no matter what a hostile release tag says. Falls back to "update" when
// nothing safe remains.
inline std::wstring BuildDownloadTargetPath(std::wstring_view temp_dir, std::string_view version_tag) {
    std::wstring ver;
    if (!version_tag.empty() && (version_tag.front() == 'v' || version_tag.front() == 'V')) {
        version_tag.remove_prefix(1);
    }
    for (const char c : version_tag) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '.' || c == '_' || c == '-') {
            ver += static_cast<wchar_t>(uc);
        }
    }
    if (ver.empty()) ver = L"update";
    std::wstring out(temp_dir);
    if (!out.empty() && out.back() != L'\\') out += L'\\';
    out += L"Emebalachat_Update_";
    out += ver;
    out += L".exe";
    return out;
}

// REQ-UC: human size for the consent dialog ("about 700 MB"). Nearest MiB,
// never below 1 MB for a non-empty payload.
inline std::wstring FormatSizeMb(std::uint64_t bytes) {
    std::uint64_t mb = (bytes + 512 * 1024) / (1024 * 1024);
    if (mb == 0 && bytes > 0) mb = 1;
    wchar_t buf[32] = {};
    std::swprintf(buf, 32, L"%llu MB", static_cast<unsigned long long>(mb));
    return buf;
}

// REQ-UC: progress meta line "336 / 700 MB" (floor MiB, tabular numerals).
inline std::wstring FormatProgressMb(std::uint64_t received, std::uint64_t total) {
    wchar_t buf[64] = {};
    std::swprintf(buf, 64, L"%llu / %llu MB",
                  static_cast<unsigned long long>(received / (1024 * 1024)),
                  static_cast<unsigned long long>(total / (1024 * 1024)));
    return buf;
}

// REQ-UC: split an https URL into host + path for WinHttpConnect/OpenRequest.
// Pure + fail-closed: only the https scheme is accepted (the initial request
// and the redirect follower share this gate); userinfo/port are rejected for
// v1 (GitHub serves on the default port; INTERNET_DEFAULT_HTTPS_PORT is used).
inline std::optional<std::pair<std::string, std::string>> SplitHttpsUrl(std::string_view url) {
    constexpr std::string_view kPrefix = "https://";
    if (url.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;
    const std::string_view rest = url.substr(kPrefix.size());
    const size_t slash = rest.find('/');
    const std::string_view host = rest.substr(0, slash);
    if (host.empty() || host.find('@') != std::string_view::npos ||
        host.find(':') != std::string_view::npos) {
        return std::nullopt;
    }
    return std::make_pair(std::string(host),
                          slash == std::string_view::npos
                              ? std::string("/")
                              : std::string(rest.substr(slash)));
}

// REQ-UC SEC (security review 260930_0004, Finding 2): redirect-target host
// allowlist. The asset URL and every redirect Location come from the release
// payload (attacker-influenceable under endpoint compromise), so the fetch may
// only ever talk to the GitHub release infrastructure — anything else fails
// closed. Pure + unit-pinned.
inline constexpr std::string_view kGitHubUserContentSuffix = ".githubusercontent.com";
inline bool IsAllowedUpdateHost(std::string_view host) {
    if (host == "api.github.com" || host == "github.com") return true;
    // *.githubusercontent.com (objects/release-assets/...): requires at least
    // one label char before the dot, so the bare domain does not match.
    return host.size() > kGitHubUserContentSuffix.size() &&
           host.compare(host.size() - kGitHubUserContentSuffix.size(),
                        kGitHubUserContentSuffix.size(), kGitHubUserContentSuffix) == 0;
}

// REQ-UC SEC (security review 260930_0004, Finding 1 / CWE-400): the download
// size gate. `received`/`content_length` are the live byte counters (the
// header value may be absent/lying); `advertised_size` is the JSON asset size
// (0 = unknown). Absolute sanity bound first (the spec installer is ~700 MB;
// a runaway/chunked stream must never fill the disk), then a cross-check
// against the advertised size so a replaced/wrong file fails fast. All
// arithmetic is overflow-safe. Pure + unit-pinned.
inline constexpr std::uint64_t kMaxDownloadBytes = 2ull * 1024 * 1024 * 1024; // 2 GiB
inline constexpr std::uint64_t kDownloadAdvertisedSlack = 64ull * 1024 * 1024;

enum class DownloadCapStatus {
    WithinCap,
    OverAbsoluteCap,   // > 2 GiB hard bound
    OverAdvertisedCap  // > advertised*2 + 64 MiB (advertised known)
};

inline DownloadCapStatus CheckDownloadCapCapped(std::uint64_t received,
                                                std::uint64_t content_length,
                                                std::uint64_t advertised_size,
                                                std::uint64_t absolute_cap,
                                                std::uint64_t slack) {
    if (content_length > absolute_cap || received > absolute_cap) {
        return DownloadCapStatus::OverAbsoluteCap;
    }
    if (advertised_size > 0) {
        // bound = advertised*2 + slack, saturated at the absolute cap so a
        // garbage (e.g. clamped UINT64_MAX) advertised size cannot overflow.
        std::uint64_t bound = absolute_cap;
        if (advertised_size <= (absolute_cap - slack) / 2) {
            bound = advertised_size * 2 + slack;
        }
        if (content_length > bound || received > bound) {
            return DownloadCapStatus::OverAdvertisedCap;
        }
    }
    return DownloadCapStatus::WithinCap;
}

inline DownloadCapStatus CheckDownloadCap(std::uint64_t received,
                                          std::uint64_t content_length,
                                          std::uint64_t advertised_size) {
    return CheckDownloadCapCapped(received, content_length, advertised_size,
                                  kMaxDownloadBytes, kDownloadAdvertisedSlack);
}

// REQ-MD (260930_0004): the parameterized twin — the on-demand model download
// (1.91 GB model) raises the absolute cap to 4 GiB while keeping the same
// advertised cross-check. Pure + unit-pinned.
inline constexpr std::uint64_t kModelMaxDownloadBytes = 4ull * 1024 * 1024 * 1024; // 4 GiB

// REQ-MD (260930_0004): per-fetch options for DownloadUpdate. The update path
// keeps its pinned defaults; the model path passes an HF allowlist + a larger
// cap WITHOUT changing any update-path behavior (existing pins hold).
enum class DownloadPhase {
    Downloading, // default (no callback)
    Verifying    // bytes complete; streaming the SHA-256 (the 1.9 GB model
                 // hash takes seconds — the model dialog shows this phase)
};
struct DownloadOptions {
    // Absolute byte cap (default: the update path's 2 GiB).
    std::uint64_t absolute_cap = kMaxDownloadBytes;
    // Host allowlist override; nullptr = the GitHub release allowlist.
    bool (*host_allowed)(std::string_view) = nullptr;
    // Phase notifications (empty = ignore). Called with Verifying right
    // before the post-download hash and never otherwise.
    std::function<void(DownloadPhase)> phase_cb;
};

} // namespace updatelogic

// ===========================================================================
// I/O (blocking worker-thread seams; never call from the GUI thread)
// ===========================================================================

enum class UpdateCheckStatus {
    UpToDate,        // remote tag <= our version
    UpdateAvailable, // remote tag parses and is strictly newer
    Offline,         // network failure, non-200 (non-rate-limit), or unparseable 200
    RateLimited      // HTTP 403 with X-RateLimit-Remaining: 0
};

struct UpdateCheckOutcome {
    UpdateCheckStatus status = UpdateCheckStatus::Offline;
    updatelogic::ReleaseInfo release; // valid only when status == UpdateAvailable
};

enum class UpdateDownloadStatus {
    Ok,           // downloaded AND SHA-256-verified
    Failed,       // network/IO failure (partial file deleted)
    HashMismatch, // hash line missing/malformed OR digest mismatch (deleted; fail-closed)
    Cancelled     // user cancel (partial file deleted)
};

namespace update_detail {

// REQ-UC SEC (security review 260930_0004, Finding 6 / CWE-117): network-
// derived strings (release tags, etc.) are control-char sanitized before they
// reach the diagnostics stream so a crafted tag cannot forge log lines.
inline std::string SanitizeForDiag(std::string_view s) {
    std::string out(s);
    for (auto& ch : out) {
        const unsigned char uc = static_cast<unsigned char>(ch);
        if (uc < 0x20 || uc == 0x7F) ch = '?';
    }
    return out;
}

// REQ-UC SEC (security review 260930_0004, Finding 2): pin WinHTTP's redirect
// policy to NEVER so redirects are handled ONLY by the explicit manual
// followers below (https scheme gate + host allowlist + hop cap). Without
// this, the WinHTTP default (WINHTTP_OPTION_REDIRECT_POLICY_DEFAULT) silently
// auto-follows https->https hops and the manual gates are dead code (the
// REQ-045 precedent at openai_compatible_client.cpp pins the same option for
// the same reason; the SDK constant WINHTTP_OPTION_REDIRECT_POLICY_NEVER == 0
// is used by name here).
inline void PinRedirectPolicyNever(HINTERNET session) {
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    ::WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
}

// REQ-UC: WinHTTP GET returning the status code + (2xx) body. Follows up to
// 5 redirects EXPLICITLY (the session policy is pinned to never-follow):
// Location is read from 301/302/303/307/308, re-targeted only when the new
// URL is https AND on the GitHub release-infra allowlist. Body is capped at
// max_body (fail-closed: overflow -> empty body). `rate_limited` is set when
// a 403 carries X-RateLimit-Remaining: 0.
inline bool HttpGetWithStatus(const std::wstring& host, const std::wstring& path,
                              std::string& out_body, long& out_status, bool& rate_limited,
                              size_t max_body, const std::atomic<bool>& cancel) {
    out_body.clear();
    out_status = 0;
    rate_limited = false;

    std::wstring cur_host = host;
    std::wstring cur_path = path;
    for (int hop = 0; hop < 6; ++hop) {
        if (cancel.load(std::memory_order_relaxed)) return false;

        HINTERNET hSession = ::WinHttpOpen(UpdateCheckerUserAgent().c_str(),
                                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                           WINHTTP_NO_PROXY_NAME,
                                           WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return false;
        // REQ-UC: the 12 s spec budget, split 3/3/3/3 across the phases.
        ::WinHttpSetTimeouts(hSession, 3000, 3000, 3000, 3000);
        PinRedirectPolicyNever(hSession);

        HINTERNET hConnect = ::WinHttpConnect(hSession, cur_host.c_str(),
                                              INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) { ::WinHttpCloseHandle(hSession); return false; }

        HINTERNET hRequest = ::WinHttpOpenRequest(hConnect, L"GET", cur_path.c_str(),
                                                  nullptr, WINHTTP_NO_REFERER,
                                                  WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                  WINHTTP_FLAG_SECURE);
        if (!hRequest) {
            ::WinHttpCloseHandle(hConnect);
            ::WinHttpCloseHandle(hSession);
            return false;
        }

        // The GitHub v3 API documents the versioned Accept header; the asset
        // redirect target ignores it harmlessly. NOTE: WinHttpSendRequest's
        // dwHeadersLength counts CHARACTERS, not bytes (google_translate's
        // HttpGet relies on the same wstring::size() convention).
        const wchar_t kHeaders[] = L"Accept: application/vnd.github+json\r\n";
        if (!::WinHttpSendRequest(hRequest, kHeaders,
                                  static_cast<DWORD>(std::wcslen(kHeaders)),
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !::WinHttpReceiveResponse(hRequest, nullptr)) {
            ::WinHttpCloseHandle(hRequest);
            ::WinHttpCloseHandle(hConnect);
            ::WinHttpCloseHandle(hSession);
            return false;
        }

        DWORD status_code = 0;
        DWORD status_size = sizeof(status_code);
        ::WinHttpQueryHeaders(hRequest,
                              WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX,
                              &status_code, &status_size, WINHTTP_NO_HEADER_INDEX);
        out_status = static_cast<long>(status_code);

        if (status_code == 301 || status_code == 302 || status_code == 303 ||
            status_code == 307 || status_code == 308) {
            wchar_t location[2048] = {};
            DWORD loc_size = sizeof(location);
            const BOOL have_loc =
                ::WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION,
                                      WINHTTP_HEADER_NAME_BY_INDEX,
                                      location, &loc_size, WINHTTP_NO_HEADER_INDEX);
            ::WinHttpCloseHandle(hRequest);
            ::WinHttpCloseHandle(hConnect);
            ::WinHttpCloseHandle(hSession);
            if (!have_loc) return false;
            const auto split = updatelogic::SplitHttpsUrl(ToUtf8(location));
            // Fail-closed: non-https target OR any host outside the GitHub
            // release-infra allowlist kills the fetch.
            if (!split || !updatelogic::IsAllowedUpdateHost(split->first)) return false;
            cur_host = ToUtf16(split->first);
            cur_path = ToUtf16(split->second);
            continue;
        }

        if (status_code == 403) {
            wchar_t remaining[64] = {};
            DWORD rem_size = sizeof(remaining);
            if (::WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CUSTOM,
                                      L"X-RateLimit-Remaining",
                                      remaining, &rem_size, WINHTTP_NO_HEADER_INDEX)) {
                std::wstring v(remaining);
                const size_t b = v.find_first_not_of(L" \t\r\n");
                const size_t e = v.find_last_not_of(L" \t\r\n");
                v = (b == std::wstring::npos) ? L"" : v.substr(b, e - b + 1);
                if (v == L"0") rate_limited = true;
            }
        }

        if (status_code == 200) {
            bool overflowed = false;
            DWORD avail = 0;
            while (::WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
                if (cancel.load(std::memory_order_relaxed)) {
                    ::WinHttpCloseHandle(hRequest);
                    ::WinHttpCloseHandle(hConnect);
                    ::WinHttpCloseHandle(hSession);
                    return false;
                }
                if (out_body.size() + static_cast<size_t>(avail) > max_body) {
                    overflowed = true;
                    break;
                }
                std::vector<char> buf(avail);
                DWORD read = 0;
                if (::WinHttpReadData(hRequest, buf.data(), avail, &read) && read > 0) {
                    out_body.append(buf.data(), read);
                    if (out_body.size() > max_body) { overflowed = true; break; }
                } else {
                    break;
                }
            }
            if (overflowed) {
                DIAG_F("UPDATE/HttpGet/001: response body exceeded %zu-byte cap; failing closed\n",
                       max_body);
                out_body.clear();
            }
        }

        ::WinHttpCloseHandle(hRequest);
        ::WinHttpCloseHandle(hConnect);
        ::WinHttpCloseHandle(hSession);
        return true;
    }
    return false; // redirect hop guard exhausted
}

} // namespace update_detail

// REQ-UC: blocking latest-release check (worker thread). Quiet by
// construction: every failure mode lands in Offline or RateLimited and the
// caller renders one inline line — no modal, ever.
inline UpdateCheckOutcome CheckForUpdate(const std::atomic<bool>& cancel) {
    UpdateCheckOutcome out;
    out.status = UpdateCheckStatus::Offline;

    std::string body;
    long status = 0;
    bool rate_limited = false;
    if (!update_detail::HttpGetWithStatus(kUpdateApiHost, ToUtf16(kUpdateApiPath),
                                          body, status, rate_limited,
                                          1024 * 1024 /* 1 MiB: release JSON is tens of KB */,
                                          cancel)) {
        return out; // transport failure (incl. user cancel) -> quiet offline
    }
    if (status == 403 && rate_limited) {
        out.status = UpdateCheckStatus::RateLimited;
        return out;
    }
    if (status != 200 || body.empty()) return out;

    const auto info = updatelogic::ParseLatestRelease(body);
    if (!info) {
        DIAG_F("UPDATE/Check/001: releases/latest payload unparseable (HTTP 200, %zu bytes); quiet offline\n",
               body.size());
        return out;
    }
    const auto remote = updatelogic::ParseVersion(info->tag);
    const auto local = updatelogic::ParseVersion(kAppVersionA);
    if (!remote || !local) {
        DIAG_F("UPDATE/Check/002: version parse failed (tag='%s'); quiet offline\n",
               update_detail::SanitizeForDiag(info->tag).c_str());
        return out;
    }
    if (updatelogic::CompareVersions(*remote, *local) > 0) {
        out.status = UpdateCheckStatus::UpdateAvailable;
        out.release = std::move(*info);
    } else {
        out.status = UpdateCheckStatus::UpToDate;
    }
    return out;
}

// REQ-UC: pick the asset to download — the FIRST asset whose name ends in
// ".exe" (case-INSENSITIVE, e.g. "SETUP.EXE" counts; Finding 5 fix); falls
// back to the first asset with a URL. Pure.
inline const updatelogic::ReleaseAsset* PickInstallerAsset(const updatelogic::ReleaseInfo& release) {
    const updatelogic::ReleaseAsset* first_with_url = nullptr;
    for (const auto& a : release.assets) {
        if (a.download_url.empty()) continue;
        if (!first_with_url) first_with_url = &a;
        if (a.name.size() >= 4) {
            char tail[4] = {};
            for (int i = 0; i < 4; ++i) {
                tail[i] = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(a.name[a.name.size() - 4 + i])));
            }
            if (std::memcmp(tail, ".exe", 4) == 0) return &a;
        }
    }
    return first_with_url;
}

// REQ-UC: streaming SHA-256 (CNG/BCrypt) of a file, lowercase hex. bcrypt is
// already in the Emebalachat_core link set (openai_compatible_client); no new
// library is introduced. 1 MiB chunks keep a ~700 MB installer off the heap.
inline bool ComputeFileSha256Hex(const std::wstring& path, std::string& out_hex) {
    out_hex.clear();
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (::BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return false;
    }
    struct AlgGuard {
        BCRYPT_ALG_HANDLE* p;
        ~AlgGuard() { if (p && *p) ::BCryptCloseAlgorithmProvider(*p, 0); }
    } alg_guard{&hAlg};

    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (::BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) != 0) return false;
    struct HashGuard {
        BCRYPT_HASH_HANDLE* p;
        ~HashGuard() { if (p && *p) ::BCryptDestroyHash(*p); }
    } hash_guard{&hHash};

    HANDLE hFile = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    struct FileGuard {
        HANDLE h;
        ~FileGuard() { if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    } file_guard{hFile};

    unsigned char digest[32] = {};
    std::vector<unsigned char> chunk(1024 * 1024);
    for (;;) {
        DWORD read = 0;
        if (!::ReadFile(hFile, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr)) {
            return false;
        }
        if (read == 0) break;
        if (::BCryptHashData(hHash, chunk.data(), read, 0) != 0) return false;
    }
    if (::BCryptFinishHash(hHash, digest, sizeof(digest), 0) != 0) return false;

    static constexpr char kHex[] = "0123456789abcdef";
    out_hex.reserve(64);
    for (const unsigned char b : digest) {
        out_hex += kHex[b >> 4];
        out_hex += kHex[b & 0x0F];
    }
    return true;
}

// REQ-UC: download + verify. Streams to dest_path (CREATE_ALWAYS truncates a
// stale partial from a previous attempt), reports progress, honors cancel
// between chunks, follows redirects EXPLICITLY (session pinned to never-follow;
// https + GitHub allowlist gate on the initial URL and every hop), enforces
// the download size cap (absolute 2 GiB + advertised-size cross-check) on both
// the Content-Length and the live byte stream, then verifies the SHA-256
// against `expected_sha256` (already lowercase; a nullopt expected hash is a
// HashMismatch by design: fail-closed, no hash line no install). EVERY
// non-Ok exit deletes the partial file.
inline UpdateDownloadStatus DownloadUpdate(
    std::string_view download_url,
    const std::wstring& dest_path,
    std::uint64_t advertised_size,
    const std::optional<std::string>& expected_sha256,
    const std::function<void(std::uint64_t received, std::uint64_t total)>& on_progress,
    const std::atomic<bool>& cancel,
    const updatelogic::DownloadOptions& options = updatelogic::DownloadOptions{}) {

    if (cancel.load(std::memory_order_relaxed)) return UpdateDownloadStatus::Cancelled;

    const auto host_ok = [&](const std::string& host) {
        return options.host_allowed ? options.host_allowed(host)
                                    : updatelogic::IsAllowedUpdateHost(host);
    };

    // Initial URL gate BEFORE the file exists: https only + allowlisted host.
    const auto initial = updatelogic::SplitHttpsUrl(download_url);
    if (!initial || !host_ok(initial->first)) {
        DIAG_F("UPDATE/Download/002: download URL rejected (non-https or non-allowlisted host; fail-closed)\n");
        return UpdateDownloadStatus::Failed;
    }

    // Output file: on destruction, close + delete UNLESS keep_file was set
    // (only the verified Ok path keeps it; HashMismatch deletes explicitly).
    // SEC Finding 8 note: if the process dies mid-download the worker thread is
    // terminated before this guard can run, leaving an orphaned partial — it
    // is never executed (launch requires full download + verified hash) and
    // the next attempt's CREATE_ALWAYS truncates it.
    struct OutFile {
        HANDLE h = INVALID_HANDLE_VALUE;
        std::wstring path;
        bool keep_file = false;
        ~OutFile() {
            if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
            if (!keep_file && !path.empty()) ::DeleteFileW(path.c_str());
        }
    } out;
    out.path = dest_path;
    out.h = ::CreateFileW(dest_path.c_str(), GENERIC_WRITE, 0, nullptr,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out.h == INVALID_HANDLE_VALUE) {
        DIAG_F("UPDATE/Download/001: cannot create '%ls' (GLE %lu)\n",
               dest_path.c_str(), ::GetLastError());
        return UpdateDownloadStatus::Failed;
    }

    std::uint64_t received = 0;
    std::uint64_t total = 0;

    // Explicit redirect follower: the session policy is pinned to never-follow,
    // so 301/302/303/307/308 are re-targeted here, one allowlisted https hop at
    // a time (hop cap 5). Progress math is unchanged by the cap checks (percent
    // derives from received/total only).
    std::wstring cur_host = ToUtf16(initial->first);
    std::wstring cur_path = ToUtf16(initial->second);
    bool got_200 = false;
    for (int hop = 0; hop < 6 && !got_200; ++hop) {
        if (cancel.load(std::memory_order_relaxed)) {
            return UpdateDownloadStatus::Cancelled; // OutFile deletes the partial
        }

        HINTERNET hSession = ::WinHttpOpen(UpdateCheckerUserAgent().c_str(),
                                            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                            WINHTTP_NO_PROXY_NAME,
                                            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return UpdateDownloadStatus::Failed;
        // Big-file budget: generous per-phase caps so a slow link is not killed
        // mid-stream; user cancel still aborts promptly between chunks.
        ::WinHttpSetTimeouts(hSession, 5000, 10000, 10000, 30000);
        update_detail::PinRedirectPolicyNever(hSession);
        struct InternetGuard {
            HINTERNET h;
            ~InternetGuard() { if (h) ::WinHttpCloseHandle(h); }
        } session_guard{hSession};

        HINTERNET hConnect = ::WinHttpConnect(hSession, cur_host.c_str(),
                                              INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) return UpdateDownloadStatus::Failed;
        struct ConnectGuard {
            HINTERNET h;
            ~ConnectGuard() { if (h) ::WinHttpCloseHandle(h); }
        } connect_guard{hConnect};

        HINTERNET hRequest = ::WinHttpOpenRequest(hConnect, L"GET", cur_path.c_str(),
                                                  nullptr, WINHTTP_NO_REFERER,
                                                  WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                  WINHTTP_FLAG_SECURE);
        if (!hRequest) return UpdateDownloadStatus::Failed;
        struct RequestGuard {
            HINTERNET h;
            ~RequestGuard() { if (h) ::WinHttpCloseHandle(h); }
        } request_guard{hRequest};

        if (!::WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !::WinHttpReceiveResponse(hRequest, nullptr)) {
            return UpdateDownloadStatus::Failed;
        }

        DWORD status_code = 0;
        DWORD status_size = sizeof(status_code);
        ::WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX,
                              &status_code, &status_size, WINHTTP_NO_HEADER_INDEX);

        if (status_code == 301 || status_code == 302 || status_code == 303 ||
            status_code == 307 || status_code == 308) {
            wchar_t location[2048] = {};
            DWORD loc_size = sizeof(location);
            const BOOL have_loc =
                ::WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION,
                                      WINHTTP_HEADER_NAME_BY_INDEX,
                                      location, &loc_size, WINHTTP_NO_HEADER_INDEX);
            if (!have_loc) return UpdateDownloadStatus::Failed;
            const auto split = updatelogic::SplitHttpsUrl(ToUtf8(location));
            // Fail-closed: non-https target OR any host outside the allowlist
            // kills the download (partial deleted).
            if (!split || !host_ok(split->first)) {
                DIAG_F("UPDATE/Download/007: redirect to a non-https or non-allowlisted host refused (fail-closed)\n");
                return UpdateDownloadStatus::Failed;
            }
            cur_host = ToUtf16(split->first);
            cur_path = ToUtf16(split->second);
            continue;
        }

        if (status_code != 200) {
            DIAG_F("UPDATE/Download/003: asset GET returned HTTP %lu\n", status_code);
            return UpdateDownloadStatus::Failed;
        }
        got_200 = true;

        // Content-Length (display + cap input; may be absent or wrong).
        total = 0;
        {
            wchar_t length[64] = {};
            DWORD len_size = sizeof(length);
            if (::WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH,
                                      WINHTTP_HEADER_NAME_BY_INDEX,
                                      length, &len_size, WINHTTP_NO_HEADER_INDEX)) {
                for (const wchar_t c : std::wstring(length)) {
                    if (c >= L'0' && c <= L'9') {
                        total = total * 10 + static_cast<unsigned>(c - L'0');
                    }
                }
            }
        }

        // SEC Finding 1: refuse absurd payloads BEFORE streaming (a fabricated
        // Content-Length dies here; a lying/absent one dies in the loop).
        if (updatelogic::CheckDownloadCapCapped(received, total, advertised_size,
                                                options.absolute_cap,
                                                updatelogic::kDownloadAdvertisedSlack) !=
            updatelogic::DownloadCapStatus::WithinCap) {
            DIAG_F("UPDATE/Download/006: Content-Length %llu exceeds the download cap (advertised %llu); failing closed\n",
                   static_cast<unsigned long long>(total),
                   static_cast<unsigned long long>(advertised_size));
            return UpdateDownloadStatus::Failed; // OutFile guard deletes the partial
        }

        if (on_progress) on_progress(received, total);
        std::vector<char> buf(256 * 1024);
        for (;;) {
            if (cancel.load(std::memory_order_relaxed)) {
                return UpdateDownloadStatus::Cancelled; // OutFile deletes the partial
            }
            DWORD avail = 0;
            if (!::WinHttpQueryDataAvailable(hRequest, &avail)) return UpdateDownloadStatus::Failed;
            if (avail == 0) break;
            while (avail > 0) {
                const DWORD chunk =
                    avail > static_cast<DWORD>(buf.size()) ? static_cast<DWORD>(buf.size()) : avail;
                DWORD read = 0;
                if (!::WinHttpReadData(hRequest, buf.data(), chunk, &read) || read == 0) {
                    return UpdateDownloadStatus::Failed;
                }
                DWORD written = 0;
                if (!::WriteFile(out.h, buf.data(), read, &written, nullptr) || written != read) {
                    DIAG_F("UPDATE/Download/004: short write to '%ls'\n", dest_path.c_str());
                    return UpdateDownloadStatus::Failed;
                }
                avail -= read;
                received += read;
                // Per-chunk cap check: a chunked/runaway stream fails fast,
                // long before the disk can fill.
                if (updatelogic::CheckDownloadCapCapped(received, total, advertised_size,
                                                        options.absolute_cap,
                                                        updatelogic::kDownloadAdvertisedSlack) !=
                    updatelogic::DownloadCapStatus::WithinCap) {
                    DIAG_F("UPDATE/Download/006: stream hit the download cap at %llu bytes (advertised %llu); failing closed\n",
                           static_cast<unsigned long long>(received),
                           static_cast<unsigned long long>(advertised_size));
                    return UpdateDownloadStatus::Failed; // OutFile guard deletes the partial
                }
            }
            if (on_progress) on_progress(received, total);
        }
    }
    if (!got_200) {
        return UpdateDownloadStatus::Failed; // redirect hop guard exhausted
    }

    // Close before hashing so the digest reads complete bytes; the file now
    // survives the OutFile guard (verification decides its final fate).
    if (!::FlushFileBuffers(out.h)) return UpdateDownloadStatus::Failed;
    ::CloseHandle(out.h);
    out.h = INVALID_HANDLE_VALUE;
    out.keep_file = true;

    std::string actual;
    if (options.phase_cb) options.phase_cb(updatelogic::DownloadPhase::Verifying);
    if (!ComputeFileSha256Hex(dest_path, actual)) {
        ::DeleteFileW(dest_path.c_str());
        return UpdateDownloadStatus::Failed;
    }
    if (!expected_sha256 || actual != *expected_sha256) {
        DIAG_F("UPDATE/Download/005: SHA-256 verification failed (expected %s, got %s); file deleted\n",
               expected_sha256 ? expected_sha256->c_str() : "(no hash line in release notes)",
               actual.c_str());
        ::DeleteFileW(dest_path.c_str());
        return UpdateDownloadStatus::HashMismatch;
    }
    return UpdateDownloadStatus::Ok;
}

// REQ-UC: launch the verified installer (the installer handles the running
// app). The CALLER owns the app exit (PostQuitMessage on the GUI thread) —
// this helper only ShellExecuteWs. Returns false when the launch itself
// failed (the caller stays in the Ready state so the user can retry).
//
// TOCTOU note (security review 260930_0004, Finding 7 / CWE-367): the
// verified file sits at a predictable %TEMP% path between hashing and launch
// (and while "Later" parks it in Ready). Swapping it would require code
// already running as the SAME USER, which is outside the desktop-app threat
// model — same-user code can already do anything. If that model ever changes,
// the hardening options are: re-hash immediately before launch, or hold the
// file open with FILE_SHARE_NONE from download through launch.
inline bool LaunchInstaller(const std::wstring& path) {
    HINSTANCE h = ::ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(h) <= 32) {
        DIAG_F("UPDATE/Launch/001: ShellExecuteW failed (INT_PTR %lld) for %ls\n",
               static_cast<long long>(reinterpret_cast<INT_PTR>(h)), path.c_str());
        return false;
    }
    return true;
}

} // namespace emebalachat
