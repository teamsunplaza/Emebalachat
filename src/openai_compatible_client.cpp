#include "openai_compatible_client.hpp"

#include "config.hpp"        // BuildPrompt-free language names via FindLanguageByCode
#include "diag_logger.hpp"   // shape-only DIAG (never key/body)
#include "engine.hpp"        // TruncateHeadTailWindow (shared head/tail clamp)
#include "i18n.hpp"          // TranslateTruncatedNotice (user-facing clamp notice)
#include "unicode_utils.hpp" // ToUtf8 / ToUtf16

#include <atomic>
#include <cctype>
#include <memory>
#include <sstream>
#include <vector>
#include <windows.h>
#include <bcrypt.h>   // REQ-045: CNG SHA-256 (core does not link engine_core)
#include <wincrypt.h> // REQ-045: DPAPI CryptProtectData / CryptUnprotectData
#include <winhttp.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "winhttp.lib")

namespace emebalachat {

namespace {

// REQ-045 (design §3b): mirrors google_translate.cpp's F5 cap so a hostile or
// misbehaving server cannot exhaust memory on an unbounded body read.
constexpr size_t kMaxResponseBodyBytes = 10 * 1024 * 1024;
static_assert(kMaxResponseBodyBytes == 10485760, "REQ-045: response body cap is exactly 10 MiB");

// REQ-045 (design §3b): input clamp parity with Google (kMaxCloudQueryUnits =
// 1500 UTF-16 units). The head/tail window keeps first+last 748 units + 3-unit
// marker strictly under the cap, identical to google_translate.cpp.
constexpr size_t kMaxInputUnits = 1500;
constexpr size_t kEllipsisMarkerUnits = 3;
constexpr size_t kKeepPerSide = (kMaxInputUnits - kEllipsisMarkerUnits) / 2;
static_assert(2 * kKeepPerSide + kEllipsisMarkerUnits <= kMaxInputUnits,
              "REQ-045: head/tail window fits the 1500-unit cap");

// REQ-045 (design §3b): request profile. The Google gtx fallback uses
// resolve/connect 3000, send/receive 5000; the settings-dialog "fetch models"
// call needs to be snappier on the GUI thread, so ListModels uses a tighter
// 10 s total budget while ChatCompletion mirrors the proven Google split.
struct OpenAiHttpProfile {
    int resolve_ms;
    int connect_ms;
    int send_ms;
    int receive_ms;
};
constexpr OpenAiHttpProfile kListModelsProfile{3000, 3000, 2000, 2000}; // 10 s total
constexpr OpenAiHttpProfile kChatProfile{3000, 3000, 5000, 5000};       // 16 s, Google parity

struct WinHttpHandleDeleter {
    void operator()(HINTERNET h) const {
        if (h) {
            ::WinHttpCloseHandle(h);
        }
    }
};
using ScopedHInternet = std::unique_ptr<void, WinHttpHandleDeleter>;

// ---- base64 (RFC 4648, no external dependency) ----

std::string Base64Encode(const unsigned char* data, size_t len) {
    static const char kTbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = static_cast<unsigned>(data[i]) << 16;
        if (i + 1 < len) v |= static_cast<unsigned>(data[i + 1]) << 8;
        if (i + 2 < len) v |= static_cast<unsigned>(data[i + 2]);
        out += kTbl[(v >> 18) & 0x3F];
        out += kTbl[(v >> 12) & 0x3F];
        out += (i + 1 < len) ? kTbl[(v >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? kTbl[v & 0x3F] : '=';
    }
    return out;
}

bool Base64Decode(std::string_view in, std::vector<unsigned char>& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    int acc = 0, nbits = 0;
    for (char c : in) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        int v = val(c);
        if (v < 0) return false;
        acc = (acc << 6) | v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            out.push_back(static_cast<unsigned char>((acc >> nbits) & 0xFF));
        }
    }
    return true;
}

// ---- minimal JSON primitives (cloned from google_translate.cpp's proven,
// dependency-free parser; reused for both the models list and the chat
// completion response). Never logs content. ----

void SkipWs(std::string_view s, size_t& pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
}

bool ParseJsonString(std::string_view s, size_t& pos, std::string& out) {
    if (pos >= s.size() || s[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < s.size()) {
        char c = s[pos++];
        if (c == '"') return true;
        if (c == '\\') {
            if (pos >= s.size()) return false;
            char e = s[pos++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    // Reuse the hardened shared decoder (lone-surrogate safe).
                    if (!AppendJsonUnicodeEscape(out, s, pos)) return false;
                    break;
                }
                default: out += e; break;
            }
        } else {
            out += c;
        }
    }
    return false;
}

// Finds the next '"key"' at the current object level and positions `pos` just
// past the colon, returning the key. Returns false at the closing '}'.
bool NextKey(std::string_view s, size_t& pos, std::string& key) {
    while (pos < s.size()) {
        char c = s[pos];
        if (c == '}') return false;
        if (c == '"') {
            if (!ParseJsonString(s, pos, key)) return false;
            SkipWs(s, pos);
            if (pos < s.size() && s[pos] == ':') { ++pos; SkipWs(s, pos); return true; }
            return false;
        }
        ++pos;
    }
    return false;
}

// Skips a JSON value starting at `pos` (string / number / true / false / null
// / balanced object / balanced array).
void SkipValue(std::string_view s, size_t& pos) {
    SkipWs(s, pos);
    if (pos >= s.size()) return;
    char c = s[pos];
    if (c == '"') {
        std::string dummy;
        ParseJsonString(s, pos, dummy);
    } else if (c == '{' || c == '[') {
        const char open = c, close = (c == '{') ? '}' : ']';
        int depth = 0;
        while (pos < s.size()) {
            char d = s[pos];
            if (d == '"') {
                std::string dummy;
                ParseJsonString(s, pos, dummy);
                continue;
            }
            if (d == open) ++depth;
            else if (d == close) { --depth; ++pos; if (depth == 0) return; continue; }
            ++pos;
        }
    } else {
        while (pos < s.size() && s[pos] != ',' && s[pos] != '}' && s[pos] != ']') ++pos;
    }
}

// Parses the /v1/models response: {"object":"list","data":[{"id":"...",...},...]}
std::vector<std::string> ParseModelsJson(std::string_view json) {
    std::vector<std::string> ids;
    size_t pos = 0;
    SkipWs(json, pos);
    if (pos >= json.size() || json[pos] != '{') return ids;
    ++pos;
    std::string key;
    while (NextKey(json, pos, key)) {
        if (key == "data" && pos < json.size() && json[pos] == '[') {
            ++pos; // into data array
            while (pos < json.size()) {
                SkipWs(json, pos);
                if (pos >= json.size() || json[pos] == ']') break;
                if (json[pos] == '{') {
                    ++pos; // into model object
                    std::string mkey, id;
                    while (NextKey(json, pos, mkey)) {
                        if (mkey == "id" && pos < json.size() && json[pos] == '"') {
                            ParseJsonString(json, pos, id);
                        } else {
                            SkipValue(json, pos);
                        }
                        SkipWs(json, pos);
                        if (pos < json.size() && json[pos] == ',') ++pos;
                    }
                    if (!id.empty()) ids.push_back(std::move(id));
                } else {
                    SkipValue(json, pos);
                }
                SkipWs(json, pos);
                if (pos < json.size() && json[pos] == ',') ++pos;
            }
            break; // data is the only array we need
        }
        SkipValue(json, pos);
        SkipWs(json, pos);
        if (pos < json.size() && json[pos] == ',') ++pos;
    }
    return ids;
}

// Parses the /v1/chat/completions response and returns choices[0].message.content.
std::string ParseChatJson(std::string_view json) {
    size_t pos = 0;
    SkipWs(json, pos);
    if (pos >= json.size() || json[pos] != '{') return {};
    ++pos;
    std::string key;
    while (NextKey(json, pos, key)) {
        if (key == "choices" && pos < json.size() && json[pos] == '[') {
            ++pos; // into choices array
            SkipWs(json, pos);
            if (pos < json.size() && json[pos] == '{') {
                ++pos; // into choice object
                std::string ckey;
                while (NextKey(json, pos, ckey)) {
                    if (ckey == "message" && pos < json.size() && json[pos] == '{') {
                        ++pos; // into message object
                        std::string fkey, content;
                        while (NextKey(json, pos, fkey)) {
                            if (fkey == "content" && pos < json.size() && json[pos] == '"') {
                                ParseJsonString(json, pos, content);
                            } else {
                                SkipValue(json, pos);
                            }
                            SkipWs(json, pos);
                            if (pos < json.size() && json[pos] == ',') ++pos;
                        }
                        return content;
                    }
                    SkipValue(json, pos);
                    SkipWs(json, pos);
                    if (pos < json.size() && json[pos] == ',') ++pos;
                }
            }
            break;
        }
        SkipValue(json, pos);
        SkipWs(json, pos);
        if (pos < json.size() && json[pos] == ',') ++pos;
    }
    return {};
}

// Resolves a base URL into (host, port, is_https, path-prefix) for WinHTTP.
// The base may carry a path prefix (e.g. "https://host/v1"); we append the
// endpoint path onto it. Returns false when the URL cannot be decomposed.
bool DecomposeBaseUrl(std::string_view base, std::wstring& host, INTERNET_PORT& port,
                      bool& https, std::wstring& prefix) {
    https = false;
    port = INTERNET_DEFAULT_HTTP_PORT;
    host.clear();
    prefix.clear();

    // scheme://
    auto scheme_end = base.find("://");
    if (scheme_end == std::string_view::npos) return false;
    std::string scheme = std::string(base.substr(0, scheme_end));
    for (auto& ch : scheme) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (scheme == "https") { https = true; port = INTERNET_DEFAULT_HTTPS_PORT; }
    else if (scheme == "http") { https = false; port = INTERNET_DEFAULT_HTTP_PORT; }
    else return false;

    std::string rest = std::string(base.substr(scheme_end + 3));
    // Strip userinfo (not supported) and split host[:port] from path.
    auto at = rest.find('@');
    if (at != std::string::npos) return false; // credentials in URL unsupported
    std::string authority, path;
    auto slash = rest.find('/');
    if (slash == std::string::npos) { authority = rest; path = ""; }
    else { authority = rest.substr(0, slash); path = rest.substr(slash); }

    // host[:port]
    std::string h = authority, ps;
    auto colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        h = authority.substr(0, colon);
        ps = authority.substr(colon + 1);
    } else if (colon != std::string::npos && authority.front() == '[') {
        // [v6]:port
        auto rb = authority.rfind(']');
        if (rb == std::string::npos || rb + 1 >= authority.size() || authority[rb + 1] != ':') return false;
        h = authority.substr(0, rb + 1);
        ps = authority.substr(rb + 2);
    }
    if (h.empty()) return false;
    if (!ps.empty()) {
        int p = 0;
        for (char c : ps) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            p = p * 10 + (c - '0');
            if (p > 65535) return false;
        }
        port = static_cast<INTERNET_PORT>(p);
    }

    host = ToUtf16(h);
    if (path.empty() || path == "/") prefix = L"";
    else {
        prefix = ToUtf16(path);
        while (!prefix.empty() && prefix.back() == L'/') prefix.pop_back();
    }
    return !host.empty();
}

// JSON string escaper for the request body (control chars + quote + backslash).
std::string JsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// Builds the {"model","messages":[...],"temperature":0} chat body.
std::string BuildChatBody(std::string_view model, std::string_view system, std::string_view user) {
    std::ostringstream b;
    b << "{\"model\":\"" << JsonEscape(model)
      << "\",\"messages\":[{\"role\":\"system\",\"content\":\"" << JsonEscape(system)
      << "\"},{\"role\":\"user\",\"content\":\"" << JsonEscape(user)
      << "\"}],\"temperature\":0}";
    return b.str();
}

// Core synchronous request helper shared by ListModels and ChatCompletion.
// Issues the request and, on HTTP 200, fills response_body (bounded). Returns
// the HTTP status, or 0 on a transport failure. Shape-only DIAG.
DWORD HttpRequest(bool https, const std::wstring& host, INTERNET_PORT port,
                  const std::wstring& method, const std::wstring& path,
                  const std::string& auth_header, const std::string& body,
                  const OpenAiHttpProfile& profile, std::string& response_body) {
    response_body.clear();

    ScopedHInternet hSession(::WinHttpOpen(
        L"EmebalaChat/1.0 (OpenAI-Compatible)", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!hSession) return 0;
    ::WinHttpSetTimeouts(hSession.get(), profile.resolve_ms, profile.connect_ms,
                         profile.send_ms, profile.receive_ms);

    // REQ-045 security follow-up: disable redirects so the Authorization: Bearer
    // header can never be replayed to a redirect target (https→http downgrade /
    // host hop). OpenAI-compatible servers must not need redirects.
    // WINHTTP_REDIRECT_POLICY_DISABLE == 0 (not exposed by the project's Windows
    // SDK winhttp.h, so the literal 0 is used per MS docs).
    DWORD redirect_policy_disable = 0;
    ::WinHttpSetOption(hSession.get(), WINHTTP_OPTION_REDIRECT_POLICY,
                       &redirect_policy_disable, sizeof(redirect_policy_disable));

    ScopedHInternet hConnect(::WinHttpConnect(hSession.get(), host.c_str(), port, 0));
    if (!hConnect) return 0;

    ScopedHInternet hRequest(::WinHttpOpenRequest(
        hConnect.get(), method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0));
    if (!hRequest) return 0;

    // Bearer auth + JSON content type. The header carries the key; it is built
    // here and never logged (shape-only DIAG below).
    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!auth_header.empty()) {
        headers += L"Authorization: Bearer " + ToUtf16(auth_header) + L"\r\n";
    }

    BOOL send_ok = ::WinHttpSendRequest(
        hRequest.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (!send_ok) {
        DIAG_F("OPENAI/HttpRequest/001: WinHttpSendRequest failed (err=%lu, https=%d)\n",
               ::GetLastError(), https ? 1 : 0);
        return 0;
    }
    if (!::WinHttpReceiveResponse(hRequest.get(), nullptr)) return 0;

    DWORD status = 0, status_size = sizeof(status);
    ::WinHttpQueryHeaders(hRequest.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                          WINHTTP_NO_HEADER_INDEX);

    if (status != 200) {
        DIAG_F("OPENAI/HttpRequest/002: non-200 status=%lu (https=%d, body_len=0)\n",
               status, https ? 1 : 0);
        return status;
    }

    DWORD avail = 0;
    bool overflow = false;
    while (::WinHttpQueryDataAvailable(hRequest.get(), &avail) && avail > 0) {
        if (response_body.size() + static_cast<size_t>(avail) > kMaxResponseBodyBytes) {
            overflow = true;
            break;
        }
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (::WinHttpReadData(hRequest.get(), buf.data(), avail, &read) && read > 0) {
            response_body.append(buf.data(), read);
            if (response_body.size() > kMaxResponseBodyBytes) { overflow = true; break; }
        } else {
            break;
        }
    }
    if (overflow) {
        DIAG_F("OPENAI/HttpRequest/003: response body exceeded %zu-byte cap; aborting (DoS guard)\n",
               kMaxResponseBodyBytes);
        response_body.clear();
        return 0;
    }
    DIAG_F("OPENAI/HttpRequest/004: ok (https=%d, body_len=%zu)\n",
           https ? 1 : 0, response_body.size());
    return 200;
}

// Unprotects the key for a single request and zeroes it afterwards. Returns
// false (and surfaces no key) when unprotect fails or the integrity digest
// does not match — the request is then aborted.
bool WithUnprotectedKey(const OpenAiConfig& cfg, std::string& out_key) {
    if (cfg.api_key_dpapi.empty()) return false;
    std::string key;
    if (!UnprotectOpenAiApiKey(cfg.api_key_dpapi, key)) {
        DIAG_F("OPENAI/WithUnprotectedKey/001: DPAPI unprotect failed (key length after failed attempt not logged)\n");
        return false;
    }
    // REQ-045 (design §3b, User Decision 12:38): integrity digest check — the
    // decrypted key must hash to the persisted digest, else tamper/corruption.
    if (!cfg.api_key_sha256.empty()) {
        std::string digest;
        if (!OpenAiSha256Hex(key, digest) || digest != cfg.api_key_sha256) {
            SecureZeroMemory(key.data(), key.size());
            DIAG_F("OPENAI/WithUnprotectedKey/002: SHA-256 digest mismatch; refusing to use key\n");
            return false;
        }
    }
    out_key = std::move(key);
    return true;
}

} // namespace

// ---- Pure policy / security helpers ----

OpenAiUrlSecurity ClassifyOpenAiBaseUrl(std::string_view base_url) {
    auto scheme_end = base_url.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) return OpenAiUrlSecurity::Invalid;
    std::string scheme = std::string(base_url.substr(0, scheme_end));
    for (auto& ch : scheme) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (scheme == "https") return OpenAiUrlSecurity::Https;
    if (scheme == "http") return OpenAiUrlSecurity::Http;
    return OpenAiUrlSecurity::Invalid;
}

bool ProtectOpenAiApiKey(std::string_view cleartext, std::string& out_dpapi_base64) {
    out_dpapi_base64.clear();
    if (cleartext.empty()) return false;

    DATA_BLOB in{};
    in.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(cleartext.data()));
    in.cbData = static_cast<DWORD>(cleartext.size());
    DATA_BLOB out{};
    // REQ-045 (design §3b): CryptProtectData with per-user scope (no
    // CRYPTPROTECT_LOCAL_MACHINE) so the blob binds to the current Windows
    // user, not the machine.
    if (!::CryptProtectData(&in, L"EmebalaChat OpenAI API Key", nullptr, nullptr, nullptr,
                            0, &out)) {
        SecureZeroMemory(in.pbData, in.cbData);
        return false;
    }
    out_dpapi_base64 = Base64Encode(out.pbData, out.cbData);
    ::LocalFree(out.pbData);
    // Scrub the caller's cleartext copy after a successful protect.
    SecureZeroMemory(in.pbData, in.cbData);
    return true;
}

bool UnprotectOpenAiApiKey(std::string_view dpapi_base64, std::string& out_cleartext) {
    out_cleartext.clear();
    if (dpapi_base64.empty()) return false;
    std::vector<unsigned char> blob;
    if (!Base64Decode(dpapi_base64, blob)) return false;

    DATA_BLOB in{};
    in.pbData = blob.data();
    in.cbData = static_cast<DWORD>(blob.size());
    DATA_BLOB out{};
    if (!::CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
        return false;
    }
    out_cleartext.assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
    ::LocalFree(out.pbData);
    return true;
}

bool OpenAiSha256Hex(std::string_view data, std::string& out_hex) {
    out_hex.clear();
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (::BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return false;
    }
    DWORD hash_len = 0, dummy = 0;
    bool ok = ::BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
                                  reinterpret_cast<PUCHAR>(&hash_len),
                                  sizeof(hash_len), &dummy, 0) == 0 && hash_len > 0;
    std::vector<unsigned char> hash(hash_len);
    if (ok) {
        ok = ::BCryptHash(hAlg, nullptr, 0,
                          const_cast<PUCHAR>(reinterpret_cast<const unsigned char*>(data.data())),
                          static_cast<ULONG>(data.size()), hash.data(), hash_len) == 0;
    }
    ::BCryptCloseAlgorithmProvider(hAlg, 0);
    if (!ok) return false;

    static const char kHex[] = "0123456789abcdef";
    out_hex.resize(hash.size() * 2);
    for (size_t i = 0; i < hash.size(); ++i) {
        out_hex[2 * i] = kHex[(hash[i] >> 4) & 0xF];
        out_hex[2 * i + 1] = kHex[hash[i] & 0xF];
    }
    return true;
}

std::string MaskOpenAiApiKey(std::string_view cleartext) {
    // Keys of 6 or fewer chars are fully masked; longer keys show only the
    // first 6 chars (User Decision 12:38).
    if (cleartext.size() <= 6) return "***";
    std::string out = std::string(cleartext.substr(0, 6));
    out += "***";
    return out;
}

// ---- WinHTTP client ----

std::vector<std::string> OpenAiCompatibleClient::ListModels(const OpenAiConfig& cfg) {
    // https-only unless the user consented to plaintext http (design §3b).
    const OpenAiUrlSecurity sec = ClassifyOpenAiBaseUrl(cfg.base_url);
    if (sec == OpenAiUrlSecurity::Invalid || (sec == OpenAiUrlSecurity::Http && !cfg.http_consent_given)) {
        DIAG_F("OPENAI/ListModels/001: base URL rejected (security=%d, consent=%d)\n",
               static_cast<int>(sec), cfg.http_consent_given ? 1 : 0);
        return {};
    }

    std::string key;
    if (!WithUnprotectedKey(cfg, key)) return {};
    struct KeyZeroer {
        std::string& k;
        ~KeyZeroer() { SecureZeroMemory(k.data(), k.size()); }
    } zeroer{key};

    std::wstring host, prefix;
    INTERNET_PORT port = 0;
    bool https = false;
    if (!DecomposeBaseUrl(cfg.base_url, host, port, https, prefix)) return {};

    const std::wstring path = prefix.empty() ? L"/v1/models" : prefix + L"/v1/models";
    std::string body;
    std::string response;
    const DWORD status = HttpRequest(https, host, port, L"GET", path, key, body,
                                     kListModelsProfile, response);
    if (status != 200) return {};
    return ParseModelsJson(response);
}

std::wstring OpenAiCompatibleClient::ChatCompletion(const OpenAiConfig& cfg,
                                                    std::string_view source_lang,
                                                    std::string_view target_lang,
                                                    std::wstring_view text) {
    if (text.empty()) return {};
    const OpenAiUrlSecurity sec = ClassifyOpenAiBaseUrl(cfg.base_url);
    if (sec == OpenAiUrlSecurity::Invalid || (sec == OpenAiUrlSecurity::Http && !cfg.http_consent_given)) {
        DIAG_F("OPENAI/ChatCompletion/001: base URL rejected (security=%d, consent=%d)\n",
               static_cast<int>(sec), cfg.http_consent_given ? 1 : 0);
        return {};
    }
    if (cfg.model.empty()) return {};

    std::string key;
    if (!WithUnprotectedKey(cfg, key)) return {};
    struct KeyZeroer {
        std::string& k;
        ~KeyZeroer() { SecureZeroMemory(k.data(), k.size()); }
    } zeroer{key};

    // Input clamp parity with Google (design §3b): head/tail window + the same
    // user-facing truncation notice. Never silent.
    std::wstring input(text);
    bool truncated = false;
    if (input.size() > kMaxInputUnits) {
        input = TruncateHeadTailWindow(input, kKeepPerSide);
        truncated = true;
        DIAG_F("OPENAI/ChatCompletion/002: input %zu units exceeded %zu cap; head/tail window applied\n",
               text.size(), kMaxInputUnits);
    }

    std::wstring host, prefix;
    INTERNET_PORT port = 0;
    bool https = false;
    if (!DecomposeBaseUrl(cfg.base_url, host, port, https, prefix)) return {};

    // System prompt (design §3b): language names via the registry so a code or
    // a name both resolve; unresolvable tokens are injected raw.
    auto name_of = [](std::string_view code_or_name) -> std::string {
        std::string norm = NormalizeLanguageCode(code_or_name);
        if (const auto* l = FindLanguageByCode(norm)) return l->name_en;
        return std::string(code_or_name);
    };
    const std::string src_name = name_of(source_lang);
    const std::string tgt_name = name_of(target_lang);
    const std::string system = "Translate the user's message from " + src_name +
                               " to " + tgt_name + ". Output only the translation.";
    const std::string body = BuildChatBody(cfg.model, system, ToUtf8(input));

    const std::wstring path = prefix.empty() ? L"/v1/chat/completions"
                                             : prefix + L"/v1/chat/completions";
    std::string response;
    const DWORD status = HttpRequest(https, host, port, L"POST", path, key, body,
                                     kChatProfile, response);
    if (status != 200) return {};

    std::string content = ParseChatJson(response);
    if (content.empty()) return {};
    std::wstring out = ToUtf16(content);
    SecureZeroMemory(content.data(), content.size());
    if (out.empty()) return {};

    if (truncated) {
        out += L"\n";
        out += I18n::Get(StringId::TranslateTruncatedNotice);
    }
    return out;
}

} // namespace emebalachat
