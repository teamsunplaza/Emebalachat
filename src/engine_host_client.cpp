// engine_host_client implementation — REQ-043 reference client (plan §5.1-3).
// SELF-CONTAINED: Win32 + C++ standard library ONLY (see the header contract).
// The JSON helpers below are a deliberate VERBATIM copy of
// src/engine_host_protocol.hpp so this file can be lifted into the other
// Emebala workspaces unchanged; the smoke test proves wire interop with the
// host. No user text is EVER logged — the single mismatch diagnostic carries
// codes only (plan §5.4 / §6.4).
//
// REQ-043 framing note: a frame is [u32 LE length][UTF-8 JSON] written as ONE
// pipe message (§4.3). Reads use a 1 MiB+4 buffer, so one ReadFile returns the
// whole message; ERROR_MORE_DATA means the sender violated the cap and the
// session is torn down (the host does the same to oversized frames).

#include "engine_host_client.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h> // SHGetKnownFolderPath(FOLDERID_LocalAppData)

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error> // std::errc (from_chars result)
#include <utility>
#include <vector>

namespace emebalachat {
namespace engine_host {
namespace {

// ---- frozen deployment paths (§4.1 / §4.2 / §7.1) --------------------------
constexpr wchar_t kDefaultPipeName[] = L"\\\\.\\pipe\\emebala-engine-v1";
constexpr wchar_t kEngineSubdir[] = L"Emebala\\Common\\engine";
constexpr wchar_t kTokenFilename[] = L"token";
constexpr wchar_t kHostExeFilename[] = L"Emebala.Engine.exe";

// Client identity in the hello (§4.4 example). Listener/Reader copies edit
// kClientName; the version tracks the release this client shipped with.
constexpr char kClientName[] = "emebala-chat";
constexpr char kClientVersion[] = "0.10.1";

// Per-phase budgets: the 30 s contract budget (kRequestTimeoutMs) applies to
// the translate round trip; connect/handshake get their own short budgets so
// a missing host falls back fast (plan §5.4 row 2 keeps the retry cycle cheap).
constexpr int kConnectRetryCount = 3;
constexpr int kConnectRetryIntervalMs = 500;
constexpr int kHandshakeTimeoutMs = 10000;
constexpr int kRequestTimeoutMs = 30000;

// ---- local JSON copy (wire-identical to engine_host_protocol.hpp) ----------
std::string JsonEscape(std::string_view str) {
    std::string out;
    out.reserve(str.size() + 16);
    for (char c : str) {
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

struct JsonValue {
    std::string text;
    bool is_string = false;
};
using JsonPairs = std::vector<std::pair<std::string, JsonValue>>;

bool AppendUnicodeEscape(std::string& out, std::string_view src, std::size_t& pos) {
    auto hex4 = [&](std::size_t p, std::uint32_t& cp) -> bool {
        if (p + 4 > src.size()) return false;
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = src[p + static_cast<std::size_t>(i)];
            std::uint32_t d;
            if (c >= '0' && c <= '9') d = static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
            cp = (cp << 4) | d;
        }
        return true;
    };
    auto append_utf8 = [&](std::uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    };

    std::uint32_t cp = 0;
    if (!hex4(pos, cp)) return false;
    pos += 4;
    if (cp >= 0xD800 && cp <= 0xDBFF) {
        if (pos + 6 <= src.size() && src[pos] == '\\' && src[pos + 1] == 'u') {
            std::size_t save = pos;
            pos += 2;
            std::uint32_t lo = 0;
            if (hex4(pos, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                pos += 4;
                append_utf8(0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u));
                return true;
            }
            pos = save;
        }
        append_utf8(0xFFFD);
        return true;
    }
    if (cp >= 0xDC00 && cp <= 0xDFFF) {
        append_utf8(0xFFFD);
        return true;
    }
    append_utf8(cp);
    return true;
}

std::size_t ScanBalanced(std::string_view src, std::size_t pos, char open, char close) {
    if (pos >= src.size() || src[pos] != open) return std::string_view::npos;
    int depth = 0;
    for (std::size_t i = pos; i < src.size(); ++i) {
        const char c = src[i];
        if (c == '\"') {
            ++i;
            while (i < src.size() && src[i] != '\"') {
                if (src[i] == '\\') ++i;
                ++i;
            }
            if (i >= src.size()) return std::string_view::npos;
        } else if (c == open) {
            ++depth;
        } else if (c == close) {
            --depth;
            if (depth == 0) return i + 1;
        }
    }
    return std::string_view::npos;
}

class JsonReader {
public:
    explicit JsonReader(std::string_view src) : src_(src) {}

    bool ParseObject(JsonPairs& out) {
        auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        auto skip_ws = [&]() { while (pos_ < src_.size() && is_ws(src_[pos_])) ++pos_; };
        auto parse_string = [&](std::string& s) -> bool {
            if (pos_ >= src_.size() || src_[pos_] != '\"') return false;
            ++pos_;
            s.clear();
            while (pos_ < src_.size()) {
                const char c = src_[pos_++];
                if (c == '\"') return true;
                if (c == '\\') {
                    if (pos_ >= src_.size()) return false;
                    const char esc = src_[pos_++];
                    switch (esc) {
                        case '\"': s += '\"'; break;
                        case '\\': s += '\\'; break;
                        case '/':  s += '/';  break;
                        case 'b':  s += '\b'; break;
                        case 'f':  s += '\f'; break;
                        case 'n':  s += '\n'; break;
                        case 'r':  s += '\r'; break;
                        case 't':  s += '\t'; break;
                        case 'u':
                            if (!AppendUnicodeEscape(s, src_, pos_)) return false;
                            break;
                        default: s += esc; break;
                    }
                } else {
                    s += c;
                }
            }
            return false;
        };

        skip_ws();
        if (pos_ >= src_.size() || src_[pos_] != '{') return false;
        ++pos_;
        for (;;) {
            skip_ws();
            if (pos_ >= src_.size()) return false;
            if (src_[pos_] == '}') { ++pos_; return true; }
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (pos_ >= src_.size() || src_[pos_] != ':') return false;
            ++pos_;
            skip_ws();
            if (pos_ >= src_.size()) return false;
            JsonValue val;
            if (src_[pos_] == '\"') {
                if (!parse_string(val.text)) return false;
                val.is_string = true;
            } else if (src_[pos_] == '{' || src_[pos_] == '[') {
                const char open = src_[pos_];
                const char close = open == '{' ? '}' : ']';
                const std::size_t end = ScanBalanced(src_, pos_, open, close);
                if (end == std::string_view::npos) return false;
                val.text.assign(src_.substr(pos_, end - pos_));
                pos_ = end;
            } else {
                const std::size_t start = pos_;
                while (pos_ < src_.size() && src_[pos_] != ',' && src_[pos_] != '}' &&
                       !is_ws(src_[pos_])) {
                    ++pos_;
                }
                if (pos_ == start) return false;
                val.text.assign(src_.substr(start, pos_ - start));
            }
            out.emplace_back(std::move(key), std::move(val));
            skip_ws();
            if (pos_ < src_.size() && src_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < src_.size() && src_[pos_] == '}') { ++pos_; return true; }
            return false;
        }
    }

private:
    std::string_view src_;
    std::size_t pos_ = 0;
};

const JsonValue* FindField(const JsonPairs& pairs, std::string_view key) {
    for (const auto& [k, v] : pairs) {
        if (k == key) return &v;
    }
    return nullptr;
}

bool ParseUInt64(std::string_view s, std::uint64_t& out) {
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out, 10);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

bool ParseInt(std::string_view s, int& out) {
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out, 10);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

// Minimal wire-shape parsers for the three server messages the client accepts.
// Unknown result statuses fail to parse (converge to fallback) per §4.4.
bool ParseWelcomeWire(std::string_view json, int& protocol,
                      std::string& model_sha256) {
    JsonPairs p;
    if (!JsonReader(json).ParseObject(p)) return false;
    const auto* op = FindField(p, "op");
    if (!op || !op->is_string || op->text != "welcome") return false;
    const auto* proto = FindField(p, "protocol");
    if (!proto || !ParseInt(proto->text, protocol)) return false;
    const auto* sha = FindField(p, "model_sha256");
    if (!sha || !sha->is_string) return false;
    model_sha256 = sha->text;
    return true;
}

bool ParseResultWire(std::string_view json, std::uint64_t& id,
                     std::string& status, std::string& text) {
    JsonPairs p;
    if (!JsonReader(json).ParseObject(p)) return false;
    const auto* op = FindField(p, "op");
    if (!op || !op->is_string || op->text != "result") return false;
    const auto* idf = FindField(p, "id");
    if (!idf || !ParseUInt64(idf->text, id)) return false;
    const auto* st = FindField(p, "status");
    if (!st || !st->is_string) return false;
    status = st->text;
    text.clear();
    if (const auto* t = FindField(p, "text")) {
        if (t->is_string) text = t->text;
    }
    return true;
}

bool ParseErrorWire(std::string_view json, std::string& code) {
    JsonPairs p;
    if (!JsonReader(json).ParseObject(p)) return false;
    const auto* op = FindField(p, "op");
    if (!op || !op->is_string || op->text != "error") return false;
    const auto* c = FindField(p, "code");
    if (!c || !c->is_string) return false;
    code = c->text;
    return true;
}

// ---- paths ------------------------------------------------------------------
std::mutex g_paths_mu;
std::wstring g_pipe_override;
std::wstring g_token_override;
std::wstring g_exe_override;
std::wstring g_common_model_override;

std::wstring LocalAppDataDir() {
    PWSTR known = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known)) && known) {
        std::wstring out(known);
        ::CoTaskMemFree(known);
        return out;
    }
    return {};
}

std::wstring DefaultTokenPath() {
    const std::wstring lad = LocalAppDataDir();
    if (lad.empty()) return {};
    return lad + L"\\" + kEngineSubdir + L"\\" + kTokenFilename;
}

std::wstring DefaultExePath() {
    const std::wstring lad = LocalAppDataDir();
    if (lad.empty()) return {};
    return lad + L"\\" + kEngineSubdir + L"\\" + kHostExeFilename;
}

std::wstring PipeName() {
    std::lock_guard<std::mutex> lk(g_paths_mu);
    return g_pipe_override.empty() ? std::wstring(kDefaultPipeName) : g_pipe_override;
}
std::wstring TokenPath() {
    std::lock_guard<std::mutex> lk(g_paths_mu);
    return g_token_override.empty() ? DefaultTokenPath() : g_token_override;
}
std::wstring ExePath() {
    std::lock_guard<std::mutex> lk(g_paths_mu);
    return g_exe_override.empty() ? DefaultExePath() : g_exe_override;
}

// ---- session ----------------------------------------------------------------
struct Session {
    HANDLE pipe = nullptr;             // nullptr = not connected
    std::uint64_t next_id = 1;
};
std::mutex g_session_mu;
Session g_session;

void CloseSessionLocked() {
    if (g_session.pipe && g_session.pipe != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_session.pipe);
    }
    g_session.pipe = nullptr;
}

// One shape-only stderr line for the mismatch classes (plan §5.4 row 3).
// Never includes user text — codes and lengths only.
// REQ-043 (M6 T5, plan §V2-8.6 wording sync): no embedded engine exists —
// this client only reports the failure; the engine router applies the
// documented UX chain (respawn -> one-click repair -> consent-gated cloud
// -> explicit feature-unavailable notice).
void LogHandshakeMismatch(const char* what) {
    fprintf(stderr, "ENGINEHOST/client/001: %s; local serving unavailable (repair path)\n", what);
}

bool FileExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadTokenFile(std::string& token) {
    token.clear();
    const std::wstring path = TokenPath();
    if (path.empty()) return false;
    const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[64] = {0};
    DWORD read = 0;
    const BOOL ok = ::ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
    ::CloseHandle(h);
    if (!ok) return false;
    // Tolerate a trailing newline; the token itself is [0-9a-f]{32}.
    std::string t(buf, buf + read);
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ' || t.back() == '\t')) {
        t.pop_back();
    }
    if (t.size() != 32) return false;
    for (char c : t) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) return false;
    }
    token = std::move(t);
    return true;
}

bool SpawnHost() {
    const std::wstring exe = ExePath();
    if (!FileExists(exe)) return false;
    // CreateProcessW needs a writable command-line buffer.
    std::wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; // §5.1-4: hidden background process
    PROCESS_INFORMATION pi = {};
    const BOOL ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) return false;
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

// Overlapped helpers (the pipe handle is opened with FILE_FLAG_OVERLAPPED so
// the per-request contract timeout is enforceable even if the host hangs).
using SteadyClock = std::chrono::steady_clock;

long long RemainingMs(SteadyClock::time_point deadline) {
    const auto now = SteadyClock::now();
    if (now >= deadline) return 0;
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
}

bool WaitOverlapped(HANDLE pipe, OVERLAPPED& ol, long long timeout_ms, DWORD& transferred) {
    const DWORD wait = timeout_ms <= 0 ? 0u : static_cast<DWORD>(timeout_ms);
    const DWORD wr = ::WaitForSingleObject(ol.hEvent, wait);
    if (wr != WAIT_OBJECT_0) return false; // timeout (or abandoned -> treat as io error)
    return ::GetOverlappedResult(pipe, &ol, &transferred, FALSE) != FALSE;
}

// Write one complete frame as ONE pipe message (§4.3).
bool WriteFrame(HANDLE pipe, std::string_view json) {
    std::string frame;
    frame.reserve(4 + json.size());
    const std::uint32_t len = static_cast<std::uint32_t>(json.size());
    for (unsigned i = 0; i < 4; ++i) {
        frame.push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
    }
    frame.append(json);

    OVERLAPPED ol = {};
    ol.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ol.hEvent) return false;
    DWORD written = 0;
    const SteadyClock::time_point deadline = SteadyClock::now() + std::chrono::milliseconds(15000);
    BOOL ok = ::WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, &ol);
    if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
        ok = WaitOverlapped(pipe, ol, RemainingMs(deadline), written);
    }
    const bool success = ok && written == frame.size();
    ::CloseHandle(ol.hEvent);
    return success;
}

enum class ReadOutcome { Ok, Timeout, IoError };

// Read exactly one pipe message into `json` (frame header + body), enforcing
// the deadline. A message larger than the buffer (the 1 MiB cap) is an IO
// error and tears the session down.
ReadOutcome ReadFrame(HANDLE pipe, std::string& json, long long timeout_ms) {
    constexpr DWORD kBufSize = (1u << 20) + 4;
    static thread_local std::vector<char> buf; // 1 MiB TLS, allocated once per thread
    if (buf.size() < kBufSize) buf.resize(kBufSize);

    OVERLAPPED ol = {};
    ol.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ol.hEvent) return ReadOutcome::IoError;
    DWORD read = 0;
    const SteadyClock::time_point deadline = SteadyClock::now() + std::chrono::milliseconds(timeout_ms);
    BOOL ok = ::ReadFile(pipe, buf.data(), kBufSize, &read, &ol);
    if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
        ok = WaitOverlapped(pipe, ol, RemainingMs(deadline), read);
        if (!ok) {
            ::CancelIoEx(pipe, &ol);
            ::CloseHandle(ol.hEvent);
            return ::GetLastError() == ERROR_OPERATION_ABORTED || RemainingMs(deadline) <= 0
                       ? ReadOutcome::Timeout
                       : ReadOutcome::IoError;
        }
    }
    ::CloseHandle(ol.hEvent);
    if (!ok || read < 4) return ReadOutcome::IoError;
    std::uint32_t len = 0;
    len = (static_cast<std::uint32_t>(static_cast<unsigned char>(buf[0]))      ) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(buf[1])) <<  8) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(buf[2])) << 16) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(buf[3])) << 24);
    if (len > (1u << 20) || static_cast<std::size_t>(len) + 4 != read) {
        return ReadOutcome::IoError; // cap violation or framing corruption
    }
    json.assign(buf.data() + 4, static_cast<std::size_t>(len));
    return ReadOutcome::Ok;
}

// Connect + handshake. On success g_session.pipe is live and the welcome pin
// has been verified. `err` receives a stable machine token on failure.
bool ConnectAndHandshake(const EngineHostConfig& cfg, std::string& err) {
    const std::wstring pipe = PipeName();
    if (pipe.empty()) { err = "connect"; return false; }

    HANDLE h = nullptr;
    bool spawned = false;
    DWORD last_err = ERROR_SUCCESS;
    for (int attempt = 0;; ++attempt) {
        ::WaitNamedPipeW(pipe.c_str(), kConnectRetryIntervalMs);
        h = ::CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE,
                          0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        last_err = ::GetLastError();
        if (attempt >= kConnectRetryCount) {
            // Shape-only diagnostic: the bare code lets logs distinguish
            // "no listener" (2) from "all instances busy" (231) etc.
            err = "connect:" + std::to_string(last_err);
            return false;
        }
        // §5.1-4: spawn on demand (once), then retries paced at 500 ms (x3).
        // The fixed sleep is load-bearing: WaitNamedPipe returns IMMEDIATELY
        // when no pipe instance exists yet, so without it all three retries
        // would blast through within milliseconds - far faster than a freshly
        // spawned host can create its pipe instances (the very race the plan
        // 500 ms pacing exists to cover). SpawnHost is cheap when the binary
        // is absent.
        if (cfg.spawn && !spawned) {
            SpawnHost();
            spawned = true;
        }
        ::Sleep(kConnectRetryIntervalMs);
    }
    g_session.pipe = h;

    std::string token;
    if (!ReadTokenFile(token)) {
        err = "no_token";
        return false;
    }

    const std::string hello = std::string("{\"op\":\"hello\",\"protocol\":1,\"token\":\"") +
                              JsonEscape(token) + "\",\"client\":\"" + kClientName +
                              "\",\"client_version\":\"" + kClientVersion + "\"}";
    if (!WriteFrame(h, hello)) { err = "io"; return false; }

    std::string reply;
    const ReadOutcome rc = ReadFrame(h, reply, kHandshakeTimeoutMs);
    if (rc != ReadOutcome::Ok) { err = rc == ReadOutcome::Timeout ? "timeout" : "io"; return false; }

    std::string code;
    if (ParseErrorWire(reply, code)) {
        // §5.4 row 3: one shape-only line, then fall back.
        if (code == "unauthorized") {
            LogHandshakeMismatch("token rejected (unauthorized)");
            err = "unauthorized";
        } else if (code == "version_mismatch") {
            LogHandshakeMismatch("protocol version_mismatch");
            err = "version_mismatch";
        } else {
            err = code.empty() ? "bad_request" : code;
        }
        return false;
    }

    int protocol = 0;
    std::string sha;
    if (!ParseWelcomeWire(reply, protocol, sha)) { err = "io"; return false; }
    if (protocol != 1) {
        LogHandshakeMismatch("welcome protocol != 1 (version_mismatch)");
        err = "version_mismatch";
        return false;
    }
    if (sha != kExpectedModelSha256) {
        LogHandshakeMismatch("welcome model_sha256 pin mismatch");
        err = "pin_mismatch";
        return false;
    }
    return true;
}

} // namespace

// REQ-043 (plan §5.1-2): see the header for the contract. Pure existence
// check — the engine manager uses it to keep Auto/strict-local routing
// honest when the shared host binary is not deployed (repair path, §V2-8.6).
bool IsHostBinaryPresent() {
    std::lock_guard<std::mutex> lk(g_paths_mu);
    return FileExists(g_exe_override.empty() ? DefaultExePath() : g_exe_override);
}

// REQ-043: common model location (pure-ASCII path, so widening IS the UTF-8).
// Honors the test override (SetCommonModelPathForTesting).
// REQ-043 P6 fold2: the path is pure-ASCII by contract (fixed "Emebala\Common\"
// tail + an ASCII-only %LOCALAPPDATA% directory on every supported install),
// so the wchar_t->char narrowing below is lossless; the explicit cast makes
// the C4244 surface quiet under /W4. Behavior unchanged: spawn/existence only.
bool TryGetCommonModelPath(std::string& out_utf8) {
    out_utf8.clear();
    std::wstring path;
    {
        std::lock_guard<std::mutex> lk(g_paths_mu);
        if (!g_common_model_override.empty()) {
            path = g_common_model_override;
        }
    }
    if (path.empty()) {
        const std::wstring lad = LocalAppDataDir();
        if (lad.empty()) return false;
        path = lad + L"\\Emebala\\Common\\models\\Hy-MT2-1.8B-Q8_0.gguf";
    }
    out_utf8.clear();
    out_utf8.reserve(path.size());
    for (const wchar_t ch : path) {
        out_utf8.push_back(static_cast<char>(ch));
    }
    return true;
}

void SetCommonModelPathForTesting(const wchar_t* model_path) {
    std::lock_guard<std::mutex> lk(g_paths_mu);
    g_common_model_override = model_path ? model_path : L"";
}

bool TryTranslate(const EngineHostConfig& cfg,
                  const std::string& src,
                  const std::string& tgt,
                  const std::string& text,
                  std::string& out,
                  std::string& err_code) {
    out.clear();
    err_code.clear();

    if (!cfg.enabled) { err_code = "disabled"; return false; }

    // §5.4 row 1 (REQ-043 wording sync, plan §V2-8.6): host binary absent ->
    // local serving is unavailable; the engine router applies the documented
    // UX chain (repair -> consent-gated cloud -> feature-unavailable notice).
    if (!FileExists(ExePath())) { err_code = "no_host_binary"; return false; }

    std::lock_guard<std::mutex> lk(g_session_mu);

    if (!g_session.pipe) {
        if (!ConnectAndHandshake(cfg, err_code)) {
            CloseSessionLocked();
            return false;
        }
    }

    const std::uint64_t id = g_session.next_id++;
    const std::string req = std::string("{\"op\":\"translate\",\"id\":") + std::to_string(id) +
                            ",\"src\":\"" + JsonEscape(src) + "\",\"tgt\":\"" + JsonEscape(tgt) +
                            "\",\"text\":\"" + JsonEscape(text) +
                            "\",\"timeout_ms\":" + std::to_string(kRequestTimeoutMs) + "}";
    if (!WriteFrame(g_session.pipe, req)) {
        err_code = "io";
        CloseSessionLocked();
        return false;
    }

    // Read until OUR result arrives (the host only sends this connection's
    // results; the loop is belt-and-suspenders against protocol drift).
    const SteadyClock::time_point deadline =
        SteadyClock::now() + std::chrono::milliseconds(kRequestTimeoutMs);
    for (;;) {
        const long long remaining = RemainingMs(deadline);
        if (remaining <= 0) {
            err_code = "timeout";
            CloseSessionLocked();
            return false;
        }
        std::string frame;
        const ReadOutcome rc = ReadFrame(g_session.pipe, frame, remaining);
        if (rc != ReadOutcome::Ok) {
            // §5.4 row 5: host died mid-request -> fall back now, respawn next call.
            err_code = rc == ReadOutcome::Timeout ? "timeout" : "io";
            CloseSessionLocked();
            return false;
        }
        std::string code;
        if (ParseErrorWire(frame, code)) {
            err_code = code.empty() ? "bad_request" : code;
            return false; // protocol-level error: connection stays (§4.4 only closes on handshake errors)
        }
        std::uint64_t rid = 0;
        std::string status, rtext;
        if (!ParseResultWire(frame, rid, status, rtext)) {
            err_code = "io"; // unparseable frame: treat as protocol drift
            CloseSessionLocked();
            return false;
        }
        if (rid != id) continue;
        if (status == "ok") {
            out = std::move(rtext);
            return true;
        }
        err_code = status; // engine_failed | model_missing | timeout | bad_request | busy
        return false;
    }
}

void SetPathsForTesting(const wchar_t* pipe_name,
                        const wchar_t* token_path,
                        const wchar_t* exe_path) {
    std::lock_guard<std::mutex> lk(g_paths_mu);
    g_pipe_override = pipe_name ? pipe_name : L"";
    g_token_override = token_path ? token_path : L"";
    g_exe_override = exe_path ? exe_path : L"";
    std::lock_guard<std::mutex> slk(g_session_mu);
    CloseSessionLocked();
}

} // namespace engine_host
} // namespace emebalachat
