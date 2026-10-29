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

// REQ-060: NOMINMAX must precede EVERY include in this TU — overlapped_io_util.hpp
// (line below) pulls in <windows.h>, and the warmup backoff uses std::min.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "engine_host_client.hpp"
#include "overlapped_io_util.hpp" // REQ-L02/REQ-L32 부록 ②-4: cancel-approve 대기 공용 헬퍼

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
#include <thread> // REQ-060: warmup persistent-connection worker
#include <utility>
#include <vector>

namespace emebalachat {
namespace engine_host {
namespace {

// ---- frozen deployment paths (§4.1 / §4.2 / §7.1) --------------------------
constexpr wchar_t kDefaultPipeName[] = L"\\\\.\\pipe\\emebala-engine-v1";
// REQ-L28/REQ-L32 부록 ②-1 (session 260925): v1 fallback candidate — a
// v1-capable orchestrator may serve protocol 1 on the PRIMARY shared-engine
// pipe only; the frozen -v1 name then has no listener and the connect matrix
// falls through to this one. A version_mismatch answer to the protocol:1
// hello is reported cleanly (see ConnectAndHandshake). Token-identical with
// the Listener copy (e07d0de) so both sides try the same order.
constexpr wchar_t kPrimaryPipeName[] = L"\\\\.\\pipe\\emebala-engine";
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

// REQ-057: `model` receives the OPTIONAL served-model id echo from the result
// frame (cleared first; stays empty when the host omitted the member — an old
// host). Registry-id metadata only, never user content.
bool ParseResultWire(std::string_view json, std::uint64_t& id,
                     std::string& status, std::string& text,
                     std::string& model) {
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
    model.clear();
    if (const auto* md = FindField(p, "model")) {
        if (!md->is_string) return false; // REQ-057: mistyped optional member is malformed
        model = md->text;
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

// REQ-L28 (session 260923_0001): connect candidates in priority order — the
// PRIMARY shared-engine pipe first, then the frozen -v1 fallback name. Unified
// with the Listener copy so both sides try the same order (default -> v1). A test
// override replaces the whole list with the single override pipe.
std::vector<std::wstring> PipeCandidates() {
    std::lock_guard<std::mutex> lk(g_paths_mu);
    if (!g_pipe_override.empty()) return {g_pipe_override};
    return {std::wstring(kPrimaryPipeName), std::wstring(kDefaultPipeName)};
}
std::wstring TokenPath() {
    std::lock_guard<std::mutex> lk(g_paths_mu);
    return g_token_override.empty() ? DefaultTokenPath() : g_token_override;
}
std::wstring ExePath() {
    std::lock_guard<std::mutex> lk(g_paths_mu);
    return g_exe_override.empty() ? DefaultExePath() : g_exe_override;
}

// R1-7 (v0.10.4 sweep): normalize a path for the peer-identity compare. ONLY
// the equivalences Win32 itself applies are folded in — dropping anything else
// could make two different files compare equal, the one direction a fail-closed
// gate must never take:
//   * trailing dots/spaces (stripped from every Win32 file name),
//   * forward slashes (Win32 treats them as separators),
//   * the \\?\ long-path prefix and its \\?\UNC\ form,
//   * duplicated separators (the object manager ignores them).
// Case folding is NOT done here: CompareStringOrdinal(ignore-case) applies
// proper Unicode simple case folding (user profile names can be non-ASCII).
std::wstring NormalizePeerPath(std::wstring_view path) {
    std::size_t end = path.size();
    while (end > 0 && (path[end - 1] == L'.' || path[end - 1] == L' ')) --end;

    std::wstring out;
    out.reserve(end);
    for (std::size_t i = 0; i < end; ++i) {
        out.push_back(path[i] == L'/' ? L'\\' : path[i]);
    }

    auto ascii_lower = [](wchar_t c) -> wchar_t {
        return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
    };
    auto starts_with_ci = [&](std::wstring_view s, std::wstring_view prefix) {
        if (s.size() < prefix.size()) return false;
        for (std::size_t i = 0; i < prefix.size(); ++i) {
            if (ascii_lower(s[i]) != ascii_lower(prefix[i])) return false;
        }
        return true;
    };
    if (out.size() > 4 && out[0] == L'\\' && out[1] == L'\\' && out[2] == L'?' &&
        out[3] == L'\\') {
        std::wstring rest = out.substr(4);
        // \\?\UNC\server\share -> \\server\share (the same object path).
        out = starts_with_ci(rest, L"UNC\\") ? L"\\\\" + rest.substr(4) : std::move(rest);
    }

    // Keep a leading UNC "\\" (one separator there names a device path),
    // collapse every other run of separators to one.
    std::wstring collapsed;
    collapsed.reserve(out.size());
    const std::size_t lead =
        (out.size() >= 2 && out[0] == L'\\' && out[1] == L'\\') ? 2 : 0;
    collapsed.append(out, 0, lead);
    for (std::size_t i = lead; i < out.size(); ++i) {
        if (out[i] == L'\\' && i > lead && collapsed.back() == L'\\') continue;
        collapsed.push_back(out[i]);
    }
    return collapsed;
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

// Write one complete frame as ONE pipe message (§4.3).
// REQ-L02 (session 260923_0001): the write path previously had NO cancel on
// timeout — the overlapped write kept running against a released event.
// Now it goes through WaitOverlappedOrCancelApprove like every other path.
// P5-fix D2: the event handle is owned by ScopedOverlappedEvent so no exit
// path (including the timeout early-return) can skip the close.
bool WriteFrame(HANDLE pipe, std::string_view json) {
    std::string frame;
    frame.reserve(4 + json.size());
    const std::uint32_t len = static_cast<std::uint32_t>(json.size());
    for (unsigned i = 0; i < 4; ++i) {
        frame.push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
    }
    frame.append(json);

    ScopedOverlappedEvent event;
    if (!event.IsValid()) return false;
    OVERLAPPED ol = {};
    ol.hEvent = event.Get();
    DWORD written = 0;
    const SteadyClock::time_point deadline = SteadyClock::now() + std::chrono::milliseconds(15000);
    BOOL ok = ::WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, &ol);
    if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
        ok = WaitOverlappedOrCancelApprove(pipe, ol, RemainingMs(deadline), written);
    }
    // REQ-L02 (session 260923_0001): the helper already acked the cancel before
    // returning false; the event is released when `event` goes out of scope at
    // the end of this function — strictly after the cancel-approve wait. The
    // pipe handle is never closed here.
    const bool success = ok && written == frame.size();
    return success;
}

enum class ReadOutcome { Ok, Timeout, IoError };

// Read exactly one pipe message into `json` (frame header + body), enforcing
// the deadline. A message larger than the buffer (the 1 MiB cap) is an IO
// error and tears the session down.
// P5-fix D2: the event handle is owned by ScopedOverlappedEvent so the
// timeout early-return below can no longer leak it (a321916 regression).
ReadOutcome ReadFrame(HANDLE pipe, std::string& json, long long timeout_ms) {
    constexpr DWORD kBufSize = (1u << 20) + 4;
    static thread_local std::vector<char> buf; // 1 MiB TLS, allocated once per thread
    if (buf.size() < kBufSize) buf.resize(kBufSize);

    ScopedOverlappedEvent event;
    if (!event.IsValid()) return ReadOutcome::IoError;
    OVERLAPPED ol = {};
    ol.hEvent = event.Get();
    DWORD read = 0;
    const SteadyClock::time_point deadline = SteadyClock::now() + std::chrono::milliseconds(timeout_ms);
    BOOL ok = ::ReadFile(pipe, buf.data(), kBufSize, &read, &ol);
    if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
        // REQ-L02 (session 260923_0001): cancel + kernel cancel-approve wait —
        // the helper blocks until the kernel acks the cancel, only then may the
        // event be released.
        ok = WaitOverlappedOrCancelApprove(pipe, ol, RemainingMs(deadline), read);
        if (!ok) {
            // Timeout-vs-ioerror judgment preserved: a completed cancel leaves
            // ERROR_OPERATION_ABORTED; a still-live read past the deadline is a
            // timeout; anything else is an io error.
            // The event is released when `event` goes out of scope — the helper
            // already completed the cancel-ack before returning false, so the
            // close here is safe on every path.
            return ::GetLastError() == ERROR_OPERATION_ABORTED || RemainingMs(deadline) <= 0
                       ? ReadOutcome::Timeout
                       : ReadOutcome::IoError;
        }
    }
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

// Connect ONE pipe candidate with the plan §5.1-4 retry/wait matrix
// (WaitNamedPipe + CreateFile, then retries paced at 500 ms x3). `spawned`
// tracks the spawn-on-demand-once rule ACROSS candidates. Returns a live
// handle or INVALID_HANDLE_VALUE (the last OS error is stored in last_err).
//
// R1-7 (v0.10.4 sweep): a successful CreateFileW is NOT yet a live handle. The
// pipe SERVER must be the host image this client itself spawns, verified here —
// before the caller can read the boot token or write the hello — because the
// canonical name is ownerless while the real host is idle-exited and any local
// user can pre-create it. A wrong peer is closed immediately and counted as a
// connect failure (`peer_rejected`), so the retry matrix and the next candidate
// still run, and the final err token becomes "connect:peer". The check is three
// cheap syscalls once per connection, no polling.
HANDLE ConnectPipeWithRetry(const std::wstring& pipe, const EngineHostConfig& cfg,
                            bool& spawned, DWORD& last_err, bool& peer_rejected) {
    // The expected image is the SAME ExePath() SpawnHost launches (resolved
    // once: the path cannot change under a test override mid-call).
    const std::wstring expected_host = ExePath();
    bool peer_logged = false; // one shape-only line per candidate, not per attempt
    for (int attempt = 0;; ++attempt) {
        ::WaitNamedPipeW(pipe.c_str(), kConnectRetryIntervalMs);
        HANDLE h = ::CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE,
                                 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            if (PeerHandleIsExpectedHost(h, expected_host.c_str())) return h;
            // Fail closed: never hand a wrong peer to the handshake. The token
            // file is not even read for this attempt.
            ::CloseHandle(h);
            peer_rejected = true;
            if (!peer_logged) {
                LogHandshakeMismatch("peer identity mismatch");
                peer_logged = true;
            }
        } else {
            last_err = ::GetLastError();
        }
        if (attempt >= kConnectRetryCount) return INVALID_HANDLE_VALUE;
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
}

// Connect + handshake. On success g_session.pipe is live and the welcome pin
// has been verified. `err` receives a stable machine token on failure.
bool ConnectAndHandshake(const EngineHostConfig& cfg, std::string& err) {
    // REQ-L28 (session 260923_0001): pipe priority — the primary shared-engine
    // pipe first, then the frozen -v1 fallback name (see kPrimaryPipeName). The
    // full retry matrix runs per candidate; the first candidate that ACCEPTS a
    // connection carries the handshake — a version_mismatch answer there is
    // reported cleanly (never a hang: the handshake read is bounded by
    // kHandshakeTimeoutMs).
    const std::vector<std::wstring> candidates = PipeCandidates();
    if (candidates.empty()) { err = "connect"; return false; }

    HANDLE h = INVALID_HANDLE_VALUE;
    bool spawned = false;
    bool peer_rejected = false; // R1-7: a candidate answered from a non-host image
    DWORD last_err = ERROR_SUCCESS;
    for (const std::wstring& pipe : candidates) {
        h = ConnectPipeWithRetry(pipe, cfg, spawned, last_err, peer_rejected);
        if (h != INVALID_HANDLE_VALUE) break;
    }
    if (h == INVALID_HANDLE_VALUE) {
        // Shape-only diagnostic: the bare code lets logs distinguish
        // "no listener" (2) from "all instances busy" (231) etc. R1-7: when any
        // candidate's instance existed but its process image was not the host,
        // the token is "peer" (sticky for the cycle: that is the reason local
        // serving was refused, and the gate already logged one line).
        err = peer_rejected ? "connect:peer" : "connect:" + std::to_string(last_err);
        return false;
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

// R1-7 (v0.10.4 sweep): the pure peer-path predicate. See the header contract:
// only Win32's own path equivalences are folded in, and empty input never
// matches (a deployment whose ExePath() cannot be resolved must fail closed).
bool PeerPathMatches(std::wstring_view expected, std::wstring_view actual) {
    const std::wstring e = NormalizePeerPath(expected);
    const std::wstring a = NormalizePeerPath(actual);
    if (e.empty() || a.empty()) return false;
    // Ordinal (not locale) comparison with ignore-case: deterministic for the
    // whole process lifetime and correct for non-ASCII profile directories.
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), e.data(),
                                  static_cast<int>(e.size()), TRUE) == CSTR_EQUAL;
}

bool PeerHandleIsExpectedHost(void* pipe_handle, const wchar_t* expected_host_path) {
    if (!pipe_handle || pipe_handle == INVALID_HANDLE_VALUE || !expected_host_path) {
        return false;
    }
    const HANDLE pipe = static_cast<HANDLE>(pipe_handle);
    DWORD server_pid = 0;
    if (!::GetNamedPipeServerProcessId(pipe, &server_pid) || server_pid == 0) {
        return false; // not a pipe handle, or the server end is gone
    }
    // PROCESS_QUERY_LIMITED_INFORMATION only: enough for the image path and the
    // one access right that still works across integrity levels. A peer we are
    // not allowed to query (another user's process) is a mismatch, which is
    // exactly the R1-7 attack shape.
    const HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, server_pid);
    if (!proc) return false;
    // NT paths can reach 32767 characters; a MAX_PATH buffer would turn a
    // long-path deployment into a silent false rejection.
    std::wstring image(32767, L'\0');
    DWORD len = static_cast<DWORD>(image.size());
    const BOOL ok = ::QueryFullProcessImageNameW(proc, 0, image.data(), &len);
    ::CloseHandle(proc);
    if (!ok || len == 0) return false;
    image.resize(len);
    return PeerPathMatches(expected_host_path, image);
}

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

// REQ-060: the model_id-pinned request path (declared in engine.hpp — the
// self-contained header is frozen for the sibling workspaces, so the new
// overload is declared by the consumer side). `model_id` is the OPTIONAL
// served-model registry pin: emitted ONLY when non-empty, so an empty pin
// keeps the byte shape every pre-REQ-060 receiver expects.
bool TryTranslate(const EngineHostConfig& cfg,
                  const std::string& src,
                  const std::string& tgt,
                  const std::string& text,
                  const std::string& model_id,
                  std::string& out,
                  std::string& err_code,
                  std::string& served_model) {
    out.clear();
    err_code.clear();
    served_model.clear(); // REQ-057: set only when the result frame carried the echo

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
    std::string req = std::string("{\"op\":\"translate\",\"id\":") + std::to_string(id) +
                      ",\"src\":\"" + JsonEscape(src) + "\",\"tgt\":\"" + JsonEscape(tgt) +
                      "\",\"text\":\"" + JsonEscape(text) +
                      "\",\"timeout_ms\":" + std::to_string(kRequestTimeoutMs);
    // REQ-060: the OPTIONAL model_id member, emitted ONLY when non-empty —
    // the exact emission rule (and member position) of BuildTranslate, so the
    // inline assembly stays wire-identical to the frozen helper; an empty pin
    // leaves the pre-REQ-060 frame bytes untouched.
    if (!model_id.empty()) {
        req += ",\"model_id\":\"" + JsonEscape(model_id) + "\"";
    }
    req += "}";
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
        std::string status, rtext, rmodel;
        if (!ParseResultWire(frame, rid, status, rtext, rmodel)) {
            err_code = "io"; // unparseable frame: treat as protocol drift
            CloseSessionLocked();
            return false;
        }
        if (rid != id) continue;
        if (status == "ok") {
            out = std::move(rtext);
            served_model = std::move(rmodel); // REQ-057: served-model echo
            return true;
        }
        err_code = status; // engine_failed | model_missing | timeout | bad_request | busy
        return false;
    }
}

// REQ-057: the served-model-aware contract form forwards to the REQ-060
// pinned path with an EMPTY pin — the translate frame then stays byte-
// identical to the pre-REQ-060 shape (the model_id member is emitted only
// when non-empty).
bool TryTranslate(const EngineHostConfig& cfg,
                  const std::string& src,
                  const std::string& tgt,
                  const std::string& text,
                  std::string& out,
                  std::string& err_code,
                  std::string& served_model) {
    return TryTranslate(cfg, src, tgt, text, std::string() /*model_id: unpinned*/,
                        out, err_code, served_model);
}

bool TryTranslate(const EngineHostConfig& cfg,
                  const std::string& src,
                  const std::string& tgt,
                  const std::string& text,
                  std::string& out,
                  std::string& err_code) {
    // REQ-057: the 6-arg contract form forwards to the served-model-aware
    // overload and discards the echo (existing callers stay source-compatible).
    std::string ignored_model;
    return TryTranslate(cfg, src, tgt, text, out, err_code, ignored_model);
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

// ---- REQ-060: warmup persistent-connection manager --------------------------
//
// Contract (declared in engine.hpp; the self-contained header is frozen for
// the sibling workspaces, so the new entry points are declared by the
// consumer side): while the active engine is the shared host AND
// cfg.warmup AND cfg.enabled, one background thread holds a DEDICATED pipe
// connection whose hello carried warmup=true — the host preloads the
// translation model at hello time and keeps the worker resident for as long
// as the connection lives (defeating the worker_idle_ms idle eviction from
// the client side). The connection NEVER sends a translate frame; the
// one-shot TryTranslate path above is untouched and fully independent.
//
// Threading model: the worker thread owns every pipe handle it opens
// (connect -> hello/welcome -> hold -> close); no cross-thread handle close
// exists. State pushes (SetState) only snapshot the decision inputs under a
// short mutex and signal an event — never blocking the caller. Teardown
// (Shutdown) latches the stop event and CancelIoEx-es the ONE published
// in-flight pipe handle under the same mutex that guards its close, so the
// cancel can never race a handle reuse. The pipe is published as soon as the
// connection exists (before the hello write), so the cancel covers the whole
// handshake; the join is then bounded by the worker's next stop check (a
// canceled read/write unwinds via the cancel-ack path of
// WaitOverlappedOrCancelApprove). The instance is deliberately
// leaked (new, never deleted): no static-destructor std::thread hazard, and
// ExitProcess reaps the thread and events on every termination path.

namespace {

constexpr int kWarmupHoldPollMs = 1000;     // server-drop health-check cadence
constexpr int kWarmupBackoffStartMs = 1000; // reconnect ladder 1s -> 2s -> ... 30s
constexpr int kWarmupBackoffCapMs = 30000;

class WarmupManager {
public:
    WarmupManager() = default;
    WarmupManager(const WarmupManager&) = delete;
    WarmupManager& operator=(const WarmupManager&) = delete;

    // Snapshots the decision inputs and wakes the worker for re-evaluation.
    // Cheap and non-blocking (no I/O, no join) — safe from any thread.
    void SetState(const EngineHostConfig& cfg, bool local_route_active) {
        std::lock_guard<std::mutex> lk(mu_);
        cfg_ = cfg;
        local_route_active_ = local_route_active;
        if (shutting_down_) return;
        EnsureEventsLocked();
        if (!stop_event_ || !state_event_) return; // event creation failure: stay inert
        // REQ-060: reap a worker that returned on its own (the WAIT_FAILED
        // defensive leg of Run) before the gate — joinable() stays true for
        // a finished-but-never-joined thread, so without this the gate below
        // could never respawn and warmup would stop permanently.
        ReapFinishedWorkerLocked();
        if (DesiredLocked() && !worker_.joinable()) {
            worker_ = std::thread([this] { Run(); });
        }
        // Re-evaluation wake (hold / backoff / idle park). An unsignaled
        // auto-reset event stays latched, so a push arriving while the
        // worker is busy costs one harmless extra evaluation loop.
        ::SetEvent(state_event_);
    }

    // Process-exit teardown: idempotent, bounded. Latches stop, then cancels
    // any in-flight I/O on the published pipe (under mu_, so no close can
    // interleave and stale-handle CancelIoEx is impossible). REQ-060: the
    // pipe is published as soon as the connection exists (before the hello
    // write), so the cancel covers the whole handshake, not just the hold
    // phase. Then joins.
    void Shutdown() {
        std::thread join_target;
        {
            std::lock_guard<std::mutex> lk(mu_);
            shutting_down_ = true;
            EnsureEventsLocked();
            ::SetEvent(stop_event_);
            ::SetEvent(state_event_);
            if (pipe_) ::CancelIoEx(pipe_, nullptr);
            join_target = std::move(worker_);
        }
        if (join_target.joinable()) join_target.join();
        // Events and the manager itself stay allocated for the process
        // lifetime (deliberate leak — see the section header).
    }

private:
    bool DesiredLocked() const {
        // The connection is held only while the shared host IS the active
        // local source and the user asked for warmup. `cfg_.enabled` is part
        // of the gate so a disabled engine_host block never holds a pipe.
        return cfg_.warmup && cfg_.enabled && local_route_active_;
    }

    bool StopRequested() {
        std::lock_guard<std::mutex> lk(mu_);
        return shutting_down_;
    }

    bool Desired() {
        std::lock_guard<std::mutex> lk(mu_);
        return DesiredLocked();
    }

    void EnsureEventsLocked() {
        if (!stop_event_) stop_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!state_event_) state_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    // REQ-060 (dispute-verification-report §5.1(a)): joinable() cannot tell a
    // live thread from a finished-but-never-joined one. Run() exits on its
    // own only on the WAIT_FAILED defensive leg, but when it does, worker_
    // stays joinable and the SetState restart gate would never respawn.
    // Reap finished workers (zero-timeout probe, then an immediate join) so
    // the gate sees the true state. Call with mu_ held.
    void ReapFinishedWorkerLocked() {
        if (!worker_.joinable()) return;
        if (::WaitForSingleObject(worker_.native_handle(), 0) == WAIT_OBJECT_0)
            worker_.join();
    }

    // REQ-060 (dispute-verification-report §5.2(b)): clear the in-flight
    // publish (only if still `h`) and close — the same mu_-guarded
    // unpublish-before-close pairing as drop_held, so Shutdown's CancelIoEx
    // under mu_ can never observe a closed or reused handle.
    void UnpublishAndClose(HANDLE h) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (pipe_ == h) pipe_ = nullptr;
        }
        if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
    }

    // One connect + warmup-hello + welcome-validation cycle. Returns a live
    // pipe to HOLD, nullptr on any failure (the caller paces the retries).
    // Best-effort and silent: every failure here reconverges through the
    // reconnect ladder, and the one-shot TryTranslate path reports the real
    // serving errors with the documented UX chain.
    HANDLE ConnectWarmup() {
        const EngineHostConfig cfg = [this] {
            std::lock_guard<std::mutex> lk(mu_);
            return cfg_;
        }();
        // Cheap gate first: an undeployed host binary means the translate
        // path would answer no_host_binary too — retry on the ladder.
        if (!IsHostBinaryPresent()) return nullptr;
        if (StopRequested()) return nullptr;

        const std::vector<std::wstring> candidates = PipeCandidates();
        HANDLE h = INVALID_HANDLE_VALUE;
        bool spawned = false;
        // R1-7: the warmup connection inherits the peer-identity gate through
        // the shared helper (the REQ-060 hold must never authenticate a
        // squatter either). The rejection already logged its one shape-only
        // line; this path stays silent otherwise, as documented above.
        bool peer_rejected = false;
        DWORD last_err = ERROR_SUCCESS;
        for (const std::wstring& pipe : candidates) {
            h = ConnectPipeWithRetry(pipe, cfg, spawned, last_err, peer_rejected);
            if (h != INVALID_HANDLE_VALUE) break;
            if (StopRequested()) return nullptr;
        }
        if (h == INVALID_HANDLE_VALUE) return nullptr;

        // REQ-060 (dispute-verification-report §5.2(b)): publish the pipe the
        // moment the connection exists — BEFORE the hello write — so Shutdown's
        // CancelIoEx aborts a wedged hello/welcome too. The old publish-after-
        // welcome left the whole handshake uncancellable (Shutdown join bound
        // ~17-32 s). The mu_ pairing with every close is unchanged: pipe_ is
        // cleared under mu_ before the handle close, so CancelIoEx can never
        // observe a stale handle.
        {
            std::lock_guard<std::mutex> lk(mu_);
            pipe_ = h;
        }

        std::string token;
        if (!ReadTokenFile(token)) { UnpublishAndClose(h); return nullptr; }
        if (StopRequested()) { UnpublishAndClose(h); return nullptr; }

        // REQ-060: the warmup hello — identical member order to the
        // translate path's hello plus the OPTIONAL warmup flag (the
        // BuildHello emission rule: the member appears only when true).
        const std::string hello =
            std::string("{\"op\":\"hello\",\"protocol\":1,\"token\":\"") + JsonEscape(token) +
            "\",\"client\":\"" + kClientName +
            "\",\"client_version\":\"" + kClientVersion + "\",\"warmup\":true}";
        if (!WriteFrame(h, hello)) { UnpublishAndClose(h); return nullptr; }
        if (StopRequested()) { UnpublishAndClose(h); return nullptr; }

        std::string reply;
        const ReadOutcome rc = ReadFrame(h, reply, kHandshakeTimeoutMs);
        if (rc != ReadOutcome::Ok) { UnpublishAndClose(h); return nullptr; }
        std::string code;
        if (ParseErrorWire(reply, code)) { UnpublishAndClose(h); return nullptr; }
        int protocol = 0;
        std::string sha;
        if (!ParseWelcomeWire(reply, protocol, sha)) { UnpublishAndClose(h); return nullptr; }
        if (protocol != 1 || sha != kExpectedModelSha256) {
            UnpublishAndClose(h); // version/pin drift: the translate path reports it
            return nullptr;
        }
        return h; // pipe_ stays published; Run's drop_held unpublishes pre-close
    }

    void Run() {
        // The handles are created before the thread starts and never closed
        // (deliberate leak), so snapshotting them here is race-free.
        const HANDLE waits[2] = {stop_event_, state_event_};
        HANDLE held_pipe = nullptr;
        int backoff_ms = kWarmupBackoffStartMs;

        auto drop_held = [&]() {
            {
                std::lock_guard<std::mutex> lk(mu_);
                pipe_ = nullptr; // unpublish BEFORE the close (CancelIoEx pairing)
            }
            if (held_pipe && held_pipe != INVALID_HANDLE_VALUE) ::CloseHandle(held_pipe);
            held_pipe = nullptr;
        };

        for (;;) {
            if (StopRequested()) break;
            if (!Desired()) {
                if (held_pipe) drop_held();
                backoff_ms = kWarmupBackoffStartMs;
                // Park until the next state push or shutdown.
                const DWORD wr = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (wr == WAIT_OBJECT_0) break;
                continue; // state change: re-evaluate
            }
            if (!held_pipe) {
                held_pipe = ConnectWarmup();
                if (held_pipe) {
                    backoff_ms = kWarmupBackoffStartMs;
                    continue; // hold loop below
                }
                backoff_ms = std::min(backoff_ms * 2, kWarmupBackoffCapMs);
                const DWORD wr = ::WaitForMultipleObjects(
                    2, waits, FALSE, static_cast<DWORD>(backoff_ms));
                if (wr == WAIT_OBJECT_0) break;
                continue; // timeout: retry the ladder; state wake: re-evaluate
            }
            // Hold the connection: wake on shutdown, on a state push, or on
            // the poll cadence for the server-drop health check.
            const DWORD wr = ::WaitForMultipleObjects(
                2, waits, FALSE, static_cast<DWORD>(kWarmupHoldPollMs));
            if (wr == WAIT_OBJECT_0) break;
            if (wr == WAIT_OBJECT_0 + 1) continue; // re-evaluate desired
            if (wr == WAIT_FAILED) break;          // defensive: never spin
            if (!::PeekNamedPipe(held_pipe, nullptr, 0, nullptr, nullptr, nullptr)) {
                // Server dropped us: reconnect. The ladder resets to 1 s so a
                // healthy host recovers instantly; a still-down host climbs
                // 1s -> 2s -> ... -> 30s through the connect-failure path.
                drop_held();
                backoff_ms = kWarmupBackoffStartMs;
            }
        }
        if (held_pipe) drop_held();
    }

    std::mutex mu_;
    EngineHostConfig cfg_;
    bool local_route_active_ = false;
    bool shutting_down_ = false;
    HANDLE pipe_ = nullptr;   // in-flight pipe, published for Shutdown's cancel
    HANDLE stop_event_ = nullptr;  // manual-reset: stays latched for late checks
    HANDLE state_event_ = nullptr; // auto-reset: one consume per push
    std::thread worker_;
};

WarmupManager& WarmupInstance() {
    static WarmupManager* instance = new WarmupManager();
    return *instance;
}

} // namespace

void WarmupSetState(const EngineHostConfig& cfg, bool local_route_active) {
    WarmupInstance().SetState(cfg, local_route_active);
}

void WarmupShutdown() {
    WarmupInstance().Shutdown();
}

} // namespace engine_host
} // namespace emebalachat
