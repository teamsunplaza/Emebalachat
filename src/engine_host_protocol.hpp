#pragma once

// ---------------------------------------------------------------------------
// engine_host_protocol — REQ-043: pure (Win32-free) helpers for the frozen
// shared-inference-host wire protocol (plan §4, DO NOT EDIT THE CONTRACT).
// Shared by:
//   * src/host_main.cpp  (the Emebala.Engine.exe host)
//   * tests/run_tests.cpp (golden-case protocol tests)
// The reference client (src/engine_host_client.cpp) carries a VERBATIM copy
// of the JSON/parse helpers so it stays self-contained for the other Emebala
// workspaces — the two implementations are wire-identical by construction and
// the smoke test proves interop end to end.
//
// Transport (§4.1-4.3): named pipe \\.\pipe\emebala-engine-v1, message mode,
// frames are [u32 LE length][UTF-8 JSON]; a frame is written as ONE pipe
// message; 1 MiB cap (a larger length prefix is answered with bad_request and
// the connection is dropped — the undrained body dies with the disconnect).
// Messages (§4.4): hello/welcome/translate/result/cancel/error, protocol=1.
// Unknown ops converge to bad_request; unknown result statuses fail to parse.
// ---------------------------------------------------------------------------

#include <charconv>
#include <cstdint>
#include <cstdio> // JsonEscape \u00XX formatting
#include <string>
#include <string_view>
#include <system_error> // std::errc (from_chars result)
#include <utility>
#include <vector>

namespace emebalachat {
namespace enginehost {

// ---- frozen contract constants -------------------------------------------
inline constexpr std::string_view kPipeName = "\\\\.\\pipe\\emebala-engine-v1";
inline constexpr std::uint32_t kMaxFrameBytes = 1u << 20;      // §4.1: 1 MiB
inline constexpr int kProtocolVersion = 1;                     // §4.4
inline constexpr std::size_t kMaxQueueDepth = 8;               // §4.4 busy rule
inline constexpr int kDefaultTranslateTimeoutMs = 30000;       // §5.3 default
inline constexpr int kMinTranslateTimeoutMs = 1000;            // sanity floor
inline constexpr int kMaxTranslateTimeoutMs = 600000;          // sanity ceiling
inline constexpr long long kDefaultIdleExitMs = 600000;        // §4.4 idle exit
inline constexpr std::string_view kModelDisplayName = "Hy-MT2-1.8B-Q8_0";

// §4.4 result status enumeration (order fixed; unknown -> parse failure).
enum class HostStatus : unsigned char {
    Ok,
    EngineFailed,
    ModelMissing,
    Timeout,
    BadRequest,
    Busy,
};

inline constexpr std::string_view kStatusOk = "ok";
inline constexpr std::string_view kStatusEngineFailed = "engine_failed";
inline constexpr std::string_view kStatusModelMissing = "model_missing";
inline constexpr std::string_view kStatusTimeout = "timeout";
inline constexpr std::string_view kStatusBadRequest = "bad_request";
inline constexpr std::string_view kStatusBusy = "busy";

// §4.4 error codes (handshake-level; result-level failures use HostStatus).
inline constexpr std::string_view kErrUnauthorized = "unauthorized";
inline constexpr std::string_view kErrVersionMismatch = "version_mismatch";
inline constexpr std::string_view kErrBadRequest = "bad_request";

inline std::string_view StatusToString(HostStatus s) {
    switch (s) {
        case HostStatus::Ok:           return kStatusOk;
        case HostStatus::EngineFailed: return kStatusEngineFailed;
        case HostStatus::ModelMissing: return kStatusModelMissing;
        case HostStatus::Timeout:      return kStatusTimeout;
        case HostStatus::BadRequest:   return kStatusBadRequest;
        case HostStatus::Busy:         return kStatusBusy;
    }
    return {};
}

// Unknown / misspelled statuses return false (the §4.4 convergence rule:
// a client must treat an unparseable status as a failed request).
inline bool StatusFromString(std::string_view s, HostStatus& out) {
    if (s == kStatusOk)           { out = HostStatus::Ok;           return true; }
    if (s == kStatusEngineFailed) { out = HostStatus::EngineFailed; return true; }
    if (s == kStatusModelMissing) { out = HostStatus::ModelMissing; return true; }
    if (s == kStatusTimeout)      { out = HostStatus::Timeout;      return true; }
    if (s == kStatusBadRequest)   { out = HostStatus::BadRequest;   return true; }
    if (s == kStatusBusy)         { out = HostStatus::Busy;         return true; }
    return false;
}

// ---- minimal JSON ----------------------------------------------------------
// Escape: byte-identical semantics to the config.cpp EscapeJsonString the
// whole codebase already uses (control chars < 0x20 -> \uXXXX short forms for
// the named ones, \u00XX generic otherwise).
inline std::string JsonEscape(std::string_view str) {
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

// A parsed value: JSON strings arrive UNESCAPED (is_string=true); bools are
// "true"/"false" and numbers/objects/arrays keep their RAW source text.
struct JsonValue {
    std::string text;
    bool is_string = false;
};
using JsonPairs = std::vector<std::pair<std::string, JsonValue>>;

namespace detail {

// Append one complete \uXXXX escape (with UTF-16 surrogate-pair lookahead) to
// `out`, reading from src[pos] where pos sits on the first hex digit. Wire-
// identical to emebalachat::AppendJsonUnicodeEscape (REF-3.5): lone surrogates
// become U+FFFD, a failed pair lookahead rolls pos back, non-hex -> false.
inline bool AppendUnicodeEscape(std::string& out, std::string_view src, std::size_t& pos) {
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
        // High surrogate: look ahead for a \uDC00-\uDFFF partner.
        if (pos + 6 <= src.size() && src[pos] == '\\' && src[pos + 1] == 'u') {
            std::size_t save = pos;
            pos += 2;
            std::uint32_t lo = 0;
            if (hex4(pos, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                pos += 4;
                append_utf8(0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u));
                return true;
            }
            pos = save; // not a valid pair: re-parse the candidate normally
        }
        append_utf8(0xFFFD);
        return true;
    }
    if (cp >= 0xDC00 && cp <= 0xDFFF) {
        append_utf8(0xFFFD); // lone low surrogate
        return true;
    }
    append_utf8(cp);
    return true;
}

// Scan a balanced '{...}' or '[...]' group starting at src[pos] (which must
// sit ON the opening bracket). Honors string literals (so braces inside JSON
// strings cannot skew the depth count) and backslash escapes. Returns the
// exclusive end position, or npos on imbalance.
inline std::size_t ScanBalanced(std::string_view src, std::size_t pos, char open, char close) {
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
        SkipWs();
        if (pos_ >= src_.size() || src_[pos_] != '{') return false;
        ++pos_;
        for (;;) {
            SkipWs();
            if (pos_ >= src_.size()) return false;
            if (src_[pos_] == '}') { ++pos_; return true; }
            std::string key;
            if (!ParseStringToken(key)) return false;
            SkipWs();
            if (pos_ >= src_.size() || src_[pos_] != ':') return false;
            ++pos_;
            SkipWs();
            if (pos_ >= src_.size()) return false;
            JsonValue val;
            if (src_[pos_] == '\"') {
                if (!ParseStringToken(val.text)) return false;
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
                       !IsWs(src_[pos_])) {
                    ++pos_;
                }
                if (pos_ == start) return false;
                val.text.assign(src_.substr(start, pos_ - start));
            }
            out.emplace_back(std::move(key), std::move(val));
            SkipWs();
            if (pos_ < src_.size() && src_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < src_.size() && src_[pos_] == '}') { ++pos_; return true; }
            return false;
        }
    }

private:
    static bool IsWs(char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }
    void SkipWs() {
        while (pos_ < src_.size() && IsWs(src_[pos_])) ++pos_;
    }
    // src_[pos_] must be the opening quote; consumes through the closing one.
    bool ParseStringToken(std::string& out) {
        if (pos_ >= src_.size() || src_[pos_] != '\"') return false;
        ++pos_;
        out.clear();
        while (pos_ < src_.size()) {
            const char c = src_[pos_++];
            if (c == '\"') return true;
            if (c == '\\') {
                if (pos_ >= src_.size()) return false;
                const char esc = src_[pos_++];
                switch (esc) {
                    case '\"': out += '\"'; break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u':
                        if (!AppendUnicodeEscape(out, src_, pos_)) return false;
                        break;
                    default:
                        out += esc; // permissive, mirrors the config.cpp reader
                        break;
                }
            } else {
                out += c;
            }
        }
        return false;
    }
    std::string_view src_;
    std::size_t pos_ = 0;
};

inline const JsonValue* FindField(const JsonPairs& pairs, std::string_view key) {
    for (const auto& [k, v] : pairs) {
        if (k == key) return &v;
    }
    return nullptr;
}

inline bool ParseUInt64(std::string_view s, std::uint64_t& out) {
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out, 10);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

inline bool ParseInt(std::string_view s, int& out) {
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out, 10);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

} // namespace detail

// Parse a flat JSON object. Unknown extra fields are ignored by callers
// (§4.3 extension rule); malformed input -> false.
inline bool JsonParseObject(std::string_view json, JsonPairs& out) {
    detail::JsonReader reader(json);
    return reader.ParseObject(out);
}

// Parse a raw JSON string array (as produced by JsonParseObject for '[...]'
// values) into plain strings. Non-string elements / malformed shape -> false.
// Elements use the same escape rules as object-string values, including the
// \uXXXX surrogate handling (lone surrogates -> U+FFFD).
inline bool JsonParseStringArray(std::string_view raw, std::vector<std::string>& out) {
    auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::size_t pos = 0;
    while (pos < raw.size() && is_ws(raw[pos])) ++pos;
    if (pos >= raw.size() || raw[pos] != '[') return false;
    const std::size_t end = detail::ScanBalanced(raw, pos, '[', ']');
    if (end == std::string_view::npos) return false;
    out.clear();
    // `end` is one past the closing ']'; the bracket itself sits at end-1.
    std::size_t i = pos + 1;
    for (;;) {
        while (i < end - 1 && is_ws(raw[i])) ++i;
        if (i == end - 1 && raw[i] == ']') return true; // empty or after last element
        if (i >= end - 1 || raw[i] != '\"') return false;
        ++i;
        std::string elem;
        bool closed = false;
        while (i < end) {
            const char c = raw[i++];
            if (c == '\"') { closed = true; break; }
            if (c == '\\') {
                if (i >= end) return false;
                const char esc = raw[i++];
                switch (esc) {
                    case '\"': elem += '\"'; break;
                    case '\\': elem += '\\'; break;
                    case '/':  elem += '/';  break;
                    case 'b':  elem += '\b'; break;
                    case 'f':  elem += '\f'; break;
                    case 'n':  elem += '\n'; break;
                    case 'r':  elem += '\r'; break;
                    case 't':  elem += '\t'; break;
                    case 'u':
                        if (!detail::AppendUnicodeEscape(elem, raw, i)) return false;
                        break;
                    default:
                        elem += esc; // permissive, mirrors the object reader
                        break;
                }
            } else {
                elem += c;
            }
        }
        if (!closed) return false;
        out.push_back(std::move(elem));
        while (i < end - 1 && is_ws(raw[i])) ++i;
        if (i < end - 1 && raw[i] == ',') { ++i; continue; }
        if (i == end - 1 && raw[i] == ']') return true;
        return false;
    }
}

// ---- frames (§4.3) ---------------------------------------------------------

inline constexpr std::size_t kFrameHeaderSize = 4;

// [u32 LE length][UTF-8 JSON]. The wire cap is the CALLER's policy: encoding
// refuses lengths above kMaxFrameBytes so a buggy sender can never emit an
// unsendable frame.
inline bool FrameEncode(std::string_view json, std::string& out) {
    if (json.size() > kMaxFrameBytes) return false;
    out.clear();
    out.reserve(kFrameHeaderSize + json.size());
    const std::uint32_t len = static_cast<std::uint32_t>(json.size());
    for (unsigned i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
    }
    out.append(json);
    return true;
}

inline bool FrameReadLengthPrefix(const char* bytes, std::uint32_t& len) {
    if (!bytes) return false;
    len = (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[0]))      ) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[1])) <<  8) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[2])) << 16) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[3])) << 24);
    return true;
}

// Decode a COMPLETE frame (header + body). False when the header/body sizes
// disagree or the declared body exceeds the 1 MiB cap (§4.1 -> bad_request
// is the caller's job; this helper only validates).
inline bool FrameDecode(std::string_view frame, std::string& json) {
    if (frame.size() < kFrameHeaderSize) return false;
    std::uint32_t len = 0;
    FrameReadLengthPrefix(frame.data(), len);
    if (len > kMaxFrameBytes) return false;
    if (static_cast<std::size_t>(len) != frame.size() - kFrameHeaderSize) return false;
    json.assign(frame.substr(kFrameHeaderSize));
    return true;
}

// ---- messages (§4.4) -------------------------------------------------------

struct HelloMsg {
    int protocol = 0;
    std::string token;
    std::string client;
    std::string client_version;
};

struct WelcomeMsg {
    int protocol = 0;
    std::string model;
    std::string model_sha256;
    std::vector<std::string> capabilities;
};

struct TranslateMsg {
    std::uint64_t id = 0;
    std::string src;
    std::string tgt;
    std::string text;
    int timeout_ms = kDefaultTranslateTimeoutMs;
};

struct ResultMsg {
    std::uint64_t id = 0;
    HostStatus status = HostStatus::EngineFailed;
    std::string text;
};

struct ErrorMsg {
    std::string code;
};

struct CancelMsg {
    std::uint64_t id = 0;
};

inline std::string BuildHello(const HelloMsg& m) {
    return std::string("{\"op\":\"hello\",\"protocol\":") + std::to_string(m.protocol) +
           ",\"token\":\"" + JsonEscape(m.token) +
           "\",\"client\":\"" + JsonEscape(m.client) +
           "\",\"client_version\":\"" + JsonEscape(m.client_version) + "\"}";
}

inline std::string BuildWelcome(const WelcomeMsg& m) {
    std::string caps = "[";
    for (std::size_t i = 0; i < m.capabilities.size(); ++i) {
        if (i) caps += ',';
        caps += std::string("\"") + JsonEscape(m.capabilities[i]) + "\"";
    }
    caps += ']';
    return std::string("{\"op\":\"welcome\",\"protocol\":") + std::to_string(m.protocol) +
           ",\"model\":\"" + JsonEscape(m.model) +
           "\",\"model_sha256\":\"" + JsonEscape(m.model_sha256) +
           "\",\"capabilities\":" + caps + "}";
}

inline std::string BuildTranslate(const TranslateMsg& m) {
    return std::string("{\"op\":\"translate\",\"id\":") + std::to_string(m.id) +
           ",\"src\":\"" + JsonEscape(m.src) +
           "\",\"tgt\":\"" + JsonEscape(m.tgt) +
           "\",\"text\":\"" + JsonEscape(m.text) +
           "\",\"timeout_ms\":" + std::to_string(m.timeout_ms) + "}";
}

inline std::string BuildResult(const ResultMsg& m) {
    return std::string("{\"op\":\"result\",\"id\":") + std::to_string(m.id) +
           ",\"status\":\"" + std::string(StatusToString(m.status)) +
           "\",\"text\":\"" + JsonEscape(m.text) + "\"}";
}

inline std::string BuildError(std::string_view code) {
    return std::string("{\"op\":\"error\",\"code\":\"") + JsonEscape(code) + "\"}";
}

inline std::string BuildCancel(std::uint64_t id) {
    return std::string("{\"op\":\"cancel\",\"id\":") + std::to_string(id) + "}";
}

// ---- parsers (strict on the frozen shape; extras ignored per §4.3) ----------

inline bool ParseHello(std::string_view json, HelloMsg& m) {
    JsonPairs p;
    if (!JsonParseObject(json, p)) return false;
    const auto* op = detail::FindField(p, "op");
    if (!op || !op->is_string || op->text != "hello") return false;
    const auto* proto = detail::FindField(p, "protocol");
    if (!proto || !detail::ParseInt(proto->text, m.protocol)) return false;
    const auto* token = detail::FindField(p, "token");
    if (!token || !token->is_string) return false;
    m.token = token->text;
    if (const auto* c = detail::FindField(p, "client")) {
        m.client = c->is_string ? c->text : "";
    }
    if (const auto* cv = detail::FindField(p, "client_version")) {
        m.client_version = cv->is_string ? cv->text : "";
    }
    return true;
}

inline bool ParseWelcome(std::string_view json, WelcomeMsg& m) {
    JsonPairs p;
    if (!JsonParseObject(json, p)) return false;
    const auto* op = detail::FindField(p, "op");
    if (!op || !op->is_string || op->text != "welcome") return false;
    const auto* proto = detail::FindField(p, "protocol");
    if (!proto || !detail::ParseInt(proto->text, m.protocol)) return false;
    const auto* model = detail::FindField(p, "model");
    if (!model || !model->is_string) return false;
    m.model = model->text;
    const auto* sha = detail::FindField(p, "model_sha256");
    if (!sha || !sha->is_string) return false;
    m.model_sha256 = sha->text;
    m.capabilities.clear();
    if (const auto* caps = detail::FindField(p, "capabilities")) {
        if (!JsonParseStringArray(caps->text, m.capabilities)) return false;
    }
    return true;
}

inline bool ParseTranslate(std::string_view json, TranslateMsg& m) {
    JsonPairs p;
    if (!JsonParseObject(json, p)) return false;
    const auto* op = detail::FindField(p, "op");
    if (!op || !op->is_string || op->text != "translate") return false;
    const auto* id = detail::FindField(p, "id");
    if (!id || !detail::ParseUInt64(id->text, m.id)) return false;
    if (const auto* s = detail::FindField(p, "src")) {
        if (s->is_string) m.src = s->text;
    }
    if (const auto* t = detail::FindField(p, "tgt")) {
        if (t->is_string) m.tgt = t->text;
    }
    if (const auto* x = detail::FindField(p, "text")) {
        if (x->is_string) m.text = x->text;
    }
    m.timeout_ms = kDefaultTranslateTimeoutMs;
    if (const auto* to = detail::FindField(p, "timeout_ms")) {
        int v = 0;
        if (detail::ParseInt(to->text, v)) m.timeout_ms = v;
    }
    return true;
}

inline bool ParseResult(std::string_view json, ResultMsg& m) {
    JsonPairs p;
    if (!JsonParseObject(json, p)) return false;
    const auto* op = detail::FindField(p, "op");
    if (!op || !op->is_string || op->text != "result") return false;
    const auto* id = detail::FindField(p, "id");
    if (!id || !detail::ParseUInt64(id->text, m.id)) return false;
    const auto* st = detail::FindField(p, "status");
    if (!st || !st->is_string) return false;
    if (!StatusFromString(st->text, m.status)) return false; // unknown -> fail
    m.text.clear();
    if (const auto* t = detail::FindField(p, "text")) {
        if (t->is_string) m.text = t->text;
    }
    return true;
}

inline bool ParseError(std::string_view json, ErrorMsg& m) {
    JsonPairs p;
    if (!JsonParseObject(json, p)) return false;
    const auto* op = detail::FindField(p, "op");
    if (!op || !op->is_string || op->text != "error") return false;
    const auto* code = detail::FindField(p, "code");
    if (!code || !code->is_string) return false;
    m.code = code->text;
    return true;
}

inline bool ParseCancel(std::string_view json, CancelMsg& m) {
    JsonPairs p;
    if (!JsonParseObject(json, p)) return false;
    const auto* op = detail::FindField(p, "op");
    if (!op || !op->is_string || op->text != "cancel") return false;
    const auto* id = detail::FindField(p, "id");
    if (!id || !detail::ParseUInt64(id->text, m.id)) return false;
    return true;
}

} // namespace enginehost
} // namespace emebalachat
