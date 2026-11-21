#include "google_translate.hpp"
#include "config.hpp"
#include "diag_logger.hpp"
#include "unicode_utils.hpp"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <iomanip>
#include <memory>
#include <sstream>
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace emebalachat {

namespace {

// REQ-043 (M6 T5 debug): test-only cloud-block flag. See the header for the
// contract. Defaults to false; only tests engage it. Shape-only.
std::atomic<bool> g_cloud_blocked_for_testing{false};

// F5 (security, session 260909_0002, audit §F5 MEDIUM): hard upper bound on
// the accumulated HTTP response body. The WinHttpQueryDataAvailable/
// WinHttpReadData loop below appends WITHOUT a bound, so a MITM or a
// DNS-hijacked handler streaming an endless/huge body could exhaust process
// memory. Real translation responses are tens of KB; 10 MiB leaves ~100x
// headroom while bounding the DoS surface. Exceeding the cap aborts the read
// loop fail-closed: HttpGet returns false (caller tries the fallback
// endpoint, then degrades to the historical no-network behavior).
constexpr size_t kMaxResponseBodyBytes = 10 * 1024 * 1024;
static_assert(kMaxResponseBodyBytes == 10485760, "F5: response body cap is exactly 10 MiB");

struct WinHttpHandleDeleter {
    void operator()(HINTERNET h) const {
        if (h) {
            ::WinHttpCloseHandle(h);
        }
    }
};

using ScopedHInternet = std::unique_ptr<void, WinHttpHandleDeleter>;

// Helper: reads a JSON string starting from opening quote
bool ParseJsonString(std::string_view src, size_t& pos, std::string& out) {
    if (pos >= src.size() || src[pos] != '\"') return false;
    pos++; // skip '\"'
    out.clear();

    while (pos < src.size()) {
        char c = src[pos++];
        if (c == '\"') {
            return true;
        }
        if (c == '\\') {
            if (pos >= src.size()) return false;
            char esc = src[pos++];
            switch (esc) {
                case '\"': out += '\"'; break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    // REF-3.5 (session 260910_0006 T4): shared decoder. The
                    // former inline body decoded surrogate PAIRS but lacked the
                    // config.cpp lone-surrogate defense, so a malformed
                    // Google Translate response containing a lone \uD800-\uDFFF
                    // emitted corrupt WTF-8 (ED A0.. variants). Adoption of
                    // AppendJsonUnicodeEscape replaces lone surrogates with
                    // U+FFFD; valid inputs (pairs, BMP, ASCII escapes) are
                    // bit-identical to the old encoder. Truncation/non-hex
                    // still fail the whole string parse exactly as before.
                    if (!AppendJsonUnicodeEscape(out, src, pos)) return false;
                    break;
                }
                default:
                    out += esc;
                    break;
            }
        } else {
            out += c;
        }
    }
    return false;
}

void SkipWhitespace(std::string_view src, size_t& pos) {
    while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos]))) {
        pos++;
    }
}

// Build-pinned REQ-R05 budget: the dict-chrome-ex profile total must never
// exceed the 8 s directive, so a 403/429/hung socket cannot wedge the pipeline
// for the 16 s the audit measured (3+3+5+5 s on both call paths). These assert
// the SAME values RequestProfileForPath returns for each endpoint, i.e. the
// real HttpGet wiring, not a copy of the constants.
static_assert(GoogleTranslate::RequestProfileForPath(L"/translate_a/t?client=dict-chrome-ex&sl=ko&tl=en&q=x").resolve_ms
              + GoogleTranslate::RequestProfileForPath(L"/translate_a/t?client=dict-chrome-ex&sl=ko&tl=en&q=x").connect_ms
              + GoogleTranslate::RequestProfileForPath(L"/translate_a/t?client=dict-chrome-ex&sl=ko&tl=en&q=x").send_ms
              + GoogleTranslate::RequestProfileForPath(L"/translate_a/t?client=dict-chrome-ex&sl=ko&tl=en&q=x").receive_ms
              <= 8000,
              "REQ-R05: dict-chrome-ex profile must stay within the 8 s total budget");
static_assert(GoogleTranslate::RequestProfileForPath(L"/translate_a/single?client=gtx&sl=ko&tl=en&dt=t&q=x").resolve_ms == 3000
              && GoogleTranslate::RequestProfileForPath(L"/translate_a/single?client=gtx&sl=ko&tl=en&dt=t&q=x").connect_ms == 3000
              && GoogleTranslate::RequestProfileForPath(L"/translate_a/single?client=gtx&sl=ko&tl=en&dt=t&q=x").send_ms == 5000
              && GoogleTranslate::RequestProfileForPath(L"/translate_a/single?client=gtx&sl=ko&tl=en&dt=t&q=x").receive_ms == 5000,
              "REQ-R05: gtx profile keeps the previous honest-UA phase split");

// Executes an HTTPS GET request using WinHTTP and returns response string.
// REQ-R05: request profile (UA + timeouts) is selected by RequestProfileForPath.
// The dict-chrome-ex path (clients5.google.com) is Chrome-extension-only and
// must present a Chrome UA plus Chrome-typical headers, on the trimmed <= 8 s
// timeout budget. The gtx fallback endpoint is a public API and keeps the
// truthful product UA and the previous split.
bool HttpGet(const std::wstring& host, const std::wstring& path, std::string& response_body) {
    const GoogleHttpProfile profile = GoogleTranslate::RequestProfileForPath(path);
    const bool chrome_endpoint = GoogleTranslate::IsDictChromeExEndpoint(path);

    ScopedHInternet hSession(::WinHttpOpen(
        profile.user_agent.data(),
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    ));
    if (!hSession) return false;

    ::WinHttpSetTimeouts(hSession.get(), profile.resolve_ms, profile.connect_ms,
                         profile.send_ms, profile.receive_ms);

    // R1-18a (F-03, v0.10.4 sweep): parity with openai_compatible_client.cpp -
    // never follow a redirect. The GET request line carries the user's text in
    // q=, so a 30x answered by clients5/translate.googleapis.com would replay
    // that text to the redirect target (CWE-601; TLS still validates the
    // target, so this needs a compromised endpoint/CA, hence LOW). Neither
    // endpoint needs a redirect: clients5.google.com and
    // translate.googleapis.com answer the translate_a GET with 200. HttpGet
    // treats every non-200 as an honest transport failure (a 3xx now stays a
    // 3xx instead of being followed), which falls through to the gtx endpoint
    // or the no-network path.
    // WINHTTP_OPTION_REDIRECT_POLICY_NEVER == 0 (winhttp.h defines the name;
    // the literal keeps this block byte-identical to the OpenAI client's, so a
    // reviewer can diff the two clients against each other).
    DWORD redirect_policy_disable = 0;
    // R2-11 (session 261029_0001): the result of this call was ignored, so a
    // failed policy set silently left WinHTTP's default (FOLLOW) in place and
    // the CWE-601 protection this block exists for was gone - with no trace
    // anywhere. Fail OPEN on purpose (neither endpoint needs a redirect and the
    // translate path must not die over a hardening knob), but make the
    // degradation VISIBLE and shape-only: error code only, never the request.
    if (!::WinHttpSetOption(hSession.get(), WINHTTP_OPTION_REDIRECT_POLICY,
                            &redirect_policy_disable, sizeof(redirect_policy_disable))) {
        // R3 (261029_0001, round-3 security L-2): stderr-only was the visibility
        // gap - a windowless context has no stderr, so route it through DIAG_F
        // (dual sink: stderr + the opt-in diagnostic log). Still fail-open and
        // shape-only (error code only).
        DIAG_F("GOOGLE_T/HttpGet/004: WinHttpSetOption(WINHTTP_OPTION_REDIRECT_POLICY=NEVER) "
               "failed (err=%lu); redirects may be followed on this session (fail-open, R2-11)\n",
               ::GetLastError());
    }

    ScopedHInternet hConnect(::WinHttpConnect(
        hSession.get(),
        host.c_str(),
        INTERNET_DEFAULT_HTTPS_PORT,
        0
    ));
    if (!hConnect) return false;

    ScopedHInternet hRequest(::WinHttpOpenRequest(
        hConnect.get(),
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE
    ));
    if (!hRequest) return false;

    // REQ-R05: Chrome-typical headers ride on the dict-chrome-ex profile only.
    std::wstring extra_headers;
    if (chrome_endpoint) {
        extra_headers = std::wstring(GoogleTranslate::ChromeAdditionalHeaders());
    }

    BOOL send_ok = ::WinHttpSendRequest(
        hRequest.get(),
        extra_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extra_headers.c_str(),
        static_cast<DWORD>(extra_headers.size()),
        WINHTTP_NO_REQUEST_DATA,
        0,
        0,
        0
    );
    if (!send_ok) {
        fwprintf(stderr,
                 L"GOOGLE_T/HttpGet/001: WinHttpSendRequest failed (err=%lu, chrome_profile=%s, host=%ls)\n",
                 ::GetLastError(), chrome_endpoint ? L"true" : L"false", host.c_str());
        return false;
    }

    BOOL recv_ok = ::WinHttpReceiveResponse(hRequest.get(), nullptr);
    if (!recv_ok) return false;

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    ::WinHttpQueryHeaders(
        hRequest.get(),
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status_code,
        &status_size,
        WINHTTP_NO_HEADER_INDEX
    );
    if (status_code != 200) {
        return false;
    }

    response_body.clear();
    DWORD bytes_avail = 0;
    bool body_overflowed = false;
    while (::WinHttpQueryDataAvailable(hRequest.get(), &bytes_avail) && bytes_avail > 0) {
        // F5: bound the buffer BEFORE allocating the next chunk, so a hostile
        // server cannot inflate the pending allocation past the cap; the
        // post-append check covers slow-drip chunking where each individual
        // bytes_avail is small but the cumulative body is not.
        if (response_body.size() + static_cast<size_t>(bytes_avail) > kMaxResponseBodyBytes) {
            body_overflowed = true;
            break;
        }
        std::vector<char> buffer(bytes_avail);
        DWORD bytes_read = 0;
        if (::WinHttpReadData(hRequest.get(), buffer.data(), bytes_avail, &bytes_read) && bytes_read > 0) {
            response_body.append(buffer.data(), bytes_read);
            if (response_body.size() > kMaxResponseBodyBytes) {
                body_overflowed = true;
                break;
            }
        } else {
            break;
        }
    }
    if (body_overflowed) {
        // Fail-closed: discard the partial body and report failure. The
        // caller (Translate) then tries the fallback endpoint, and if that
        // also fails returns the original text (historical no-network
        // behavior) - a partial/corrupt oversized body is never parsed.
        DIAG_F("GOOGLE_T/HttpGet/002: response body exceeded %zu-byte cap; aborting read (DoS guard)\n",
               kMaxResponseBodyBytes);
        return false;
    }

    return !response_body.empty();
}

// U-G (session 261121_0001, report §3.2 Google row): one translation chunk
// through the EXISTING dual-endpoint request path — clients5 dict-chrome-ex
// primary, translate.googleapis.com gtx fallback, REQ-R05 request profiles
// and the F5 response-body cap untouched. `utf8_chunk` is the chunker's text
// only (delimiters ride along untranslated, re-appended by the caller), so
// it is guaranteed <= kCloudChunkUnits UTF-16 units and the GET request line
// stays under the measured ~16 KB endpoint limit. Returns the translated
// chunk, or EMPTY when both endpoints fail / parse empty (REQ-048 P3: the
// caller converts that into a whole-request failure so a partial translation
// never masquerades as complete).
std::wstring TranslateChunkUtf8(const std::string& utf8_chunk,
                                const std::string& sl,
                                const std::string& tl) {
    const std::string encoded_q = GoogleTranslate::UrlEncode(utf8_chunk);
    std::string response;

    // Primary: clients5.google.com with dict-chrome-ex
    std::string path_a = "/translate_a/t?client=dict-chrome-ex&sl=" + sl + "&tl=" + tl + "&q=" + encoded_q;
    if (HttpGet(L"clients5.google.com", ToUtf16(path_a), response)) {
        std::wstring result = GoogleTranslate::ParseResponseJson(response);
        if (!result.empty()) {
            return result;
        }
    }

    // Fallback: translate.googleapis.com with gtx endpoint
    std::string path_b = "/translate_a/single?client=gtx&sl=" + sl + "&tl=" + tl + "&dt=t&q=" + encoded_q;
    if (HttpGet(L"translate.googleapis.com", ToUtf16(path_b), response)) {
        std::wstring result = GoogleTranslate::ParseResponseJson(response);
        if (!result.empty()) {
            return result;
        }
    }

    // REQ-048 P3: both endpoints failed for this chunk — empty is the
    // failure signal; Translate() fails the whole request closed on it.
    return {};
}

} // namespace

// REQ-R05: Chrome-typical headers for the dict-chrome-ex profile. Format
// follows the WinHttpSendRequest contract: "Header: value\r\n" per line,
// CRLF-terminated, no leading/trailing junk. NOTE: intentionally NO
// Accept-Encoding - WinHTTP here does not decompress gzip responses, and
// claiming gzip support without a handler would corrupt ParseResponseJson
// input (silent failure worse than 403). UA strings and the endpoint->profile
// selection are constexpr in the header so both static_asserts (below) and
// the test suite can pin them without a live socket.
std::wstring GoogleTranslate::ChromeAdditionalHeaders() {
    return L"Accept: */*\r\n"
           L"Accept-Language: en-US,en;q=0.9\r\n";
}

// REF-3.4 (session 260910_0006): the former 36-branch if-chain collapsed to
// the four mappings that are NOT the plain ASCII lowercase of the canonical
// code, plus the lowercase pass-through. Equivalence rests on two facts
// pinned by TestRef34MapLanguageCodePins (tests/run_tests.cpp):
//   1. NormalizeLanguageCode returns ONLY one of the 90 uppercase registry
//      codes (kAllLanguages, config.cpp) or "AUTO" - never mixed case; and
//   2. for all 90 codes, Google's target equals ASCII-lowercase(code) except
//      ZH-CN->zh-CN, ZH-TW->zh-TW, FIL->tl, HE->iw ("AUTO"->"auto" is covered
//      by the lowercase rule itself).
// 261116_0003 P2 (fix round 2, step5preview F-01): the counts above were
// written for the 38-row registry and were falsified by the 38->47 growth;
// the exception list itself is unchanged (all 9 new codes are lowercase-equal,
// TestRef34MapLanguageCodePins carries the +9 rows).
// 261116_0003 P3: 47 -> 90 (tranche A+B); the exception list is STILL
// unchanged — all 43 new codes are lowercase-equal (the pin carries +43).
// The old chain matched `norm`, so the raw-input tolower fallback at the tail
// was unreachable (any norm value hit a branch first); lowering `norm` keeps
// it reachable-by-construction identical: norm is never empty because an
// empty input normalizes to "AUTO" -> "auto".
std::string GoogleTranslate::MapLanguageCode(std::string_view code) {
    std::string norm = NormalizeLanguageCode(code);
    if (norm == "ZH-CN") return "zh-CN";
    if (norm == "ZH-TW") return "zh-TW";
    if (norm == "FIL") return "tl"; // Tagalog / Filipino
    if (norm == "HE") return "iw";

    std::string res;
    res.reserve(norm.size());
    for (char c : norm) {
        res += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return res.empty() ? "en" : res;
}

std::string GoogleTranslate::UrlEncode(std::string_view str) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex << std::uppercase;

    for (unsigned char c : str) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << static_cast<int>(c);
        }
    }

    return escaped.str();
}

std::wstring GoogleTranslate::ParseResponseJson(std::string_view json) {
    // Robust parser that extracts translation segments from any of Google's array formats:
    // Format 1: [["translated text", "detected_lang"]]
    // Format 2: [[["translated part 1", "source", ...], ["translated part 2", ...]], ...]
    // Format 3: ["translated text"]
    size_t pos = 0;
    SkipWhitespace(json, pos);
    if (pos >= json.size() || json[pos] != '[') return {};
    pos++; // skip outer '['

    SkipWhitespace(json, pos);
    if (pos >= json.size()) return {};

    std::string accumulated_utf8;

    // BUG-8 (v0.10.5 handoff section 2.9): the segment-tail skip must track
    // nested '[' / ']' depth AND string literals. The historical scan for
    // the next ']' desynchronized on any nested sub-array (or a ']' inside a
    // quoted string), making the outer loop parse metadata arrays as
    // translated text. This helper skips to the segment's OWN closing ']'.
    auto skip_to_segment_close = [&json](size_t& p) {
        int depth = 0;
        while (p < json.size()) {
            const char c = json[p];
            if (c == '"') {
                std::string dummy;
                if (!ParseJsonString(json, p, dummy)) {
                    return; // unterminated string: give up the skip
                }
            } else if (c == '[') {
                ++depth;
                ++p;
            } else if (c == ']') {
                if (depth == 0) {
                    return;
                }
                --depth;
                ++p;
            } else {
                ++p;
            }
        }
    };

    if (json[pos] == '\"') {
        // Format 3: ["translated text"]
        std::string text;
        if (ParseJsonString(json, pos, text)) {
            accumulated_utf8 = text;
        }
    } else if (json[pos] == '[') {
        pos++; // skip second '['
        SkipWhitespace(json, pos);

        if (pos < json.size() && json[pos] == '[') {
            // Format 2: [[ [ "seg1", ... ], [ "seg2", ... ] ], ...]
            while (pos < json.size()) {
                SkipWhitespace(json, pos);
                if (pos >= json.size() || json[pos] == ']') break;
                // G-3 (261121_0001 fix round 2, SEC-F03): a non-'[' / ']' / ','
                // element at segment-array level (a bare string or number)
                // advances NO cursor below, which wedged this loop at 100% CPU
                // forever on a malformed/hostile body (CWE-835) — the WinHTTP
                // timeouts never cover the parse loop, so the pipeline worker
                // stalled permanently. The F-1 lesson, exactly as the repo's
                // other JSON walkers apply it (AppendContentParts et al.):
                // capture the iterator before the element parse and force one
                // byte of progress when nothing consumed any.
                const size_t iter_start = pos;
                if (json[pos] == '[') {
                    pos++; // skip segment '['
                    SkipWhitespace(json, pos);
                    if (pos < json.size() && json[pos] == '\"') {
                        std::string seg;
                        if (ParseJsonString(json, pos, seg)) {
                            accumulated_utf8.append(seg);
                        }
                    }
                    // Skip remaining segment elements until ']'
                    skip_to_segment_close(pos);
                    if (pos < json.size() && json[pos] == ']') pos++;
                }
                SkipWhitespace(json, pos);
                if (pos < json.size() && json[pos] == ',') pos++;
                if (pos == iter_start) ++pos; // non-progress guard (never stall)
            }
        } else if (pos < json.size() && json[pos] == '\"') {
            // Format 1: [ ["seg1", "ko"], ["seg2", "ko"] ]
            // We are already inside the first inner array: [ "seg1", "ko" ]
            std::string seg;
            if (ParseJsonString(json, pos, seg)) {
                accumulated_utf8.append(seg);
            }
            // Skip until ']'
            skip_to_segment_close(pos);
            if (pos < json.size() && json[pos] == ']') pos++;

            // Check if there are subsequent segments
            while (pos < json.size()) {
                SkipWhitespace(json, pos);
                if (pos >= json.size() || json[pos] == ']') break;
                if (json[pos] == ',') pos++;
                SkipWhitespace(json, pos);
                if (pos < json.size() && json[pos] == '[') {
                    pos++;
                    SkipWhitespace(json, pos);
                    if (pos < json.size() && json[pos] == '\"') {
                        std::string next_seg;
                        if (ParseJsonString(json, pos, next_seg)) {
                            accumulated_utf8.append(next_seg);
                        }
                    }
                    skip_to_segment_close(pos);
                    if (pos < json.size() && json[pos] == ']') pos++;
                } else {
                    break;
                }
            }
        }
    }

    return ToUtf16(accumulated_utf8);
}

std::vector<TranslationChunk> GoogleTranslate::BuildCloudChunks(std::wstring_view text) {
    // The chunker works on UTF-8 and counts UTF-16 code units; its contract
    // (C1) guarantees every chunk text fits kCloudChunkUnits and that joining
    // text + trailing_delimiter over all chunks, in order, reproduces
    // ToUtf8(text) byte-for-byte. max_units == 0 would mean unlimited and is
    // never passed here.
    return SplitTextIntoTranslationChunks(ToUtf8(text), kCloudChunkUnits);
}

void GoogleTranslate::SetCloudBlockedForTesting(bool blocked) {
    g_cloud_blocked_for_testing.store(blocked, std::memory_order_release);
}

bool GoogleTranslate::IsCloudBlockedForTesting() {
    return g_cloud_blocked_for_testing.load(std::memory_order_acquire);
}

std::wstring GoogleTranslate::Translate(
    std::wstring_view text,
    std::string_view src_code,
    std::string_view tgt_code,
    const TranslationTaskContext& ctx
) {
    if (text.empty()) {
        return {};
    }

    // REQ-043 (M6 T5 debug): test-only cloud-block hook. When engaged,
    // Translate() returns empty (the historical no-network outcome) WITHOUT
    // touching WinHTTP. This keeps the T5 manager-routing suites hermetic
    // (no live-network dependency, no WinHTTP stall) while still exercising
    // the manager->cloud_call() seam end-to-end.
    if (IsCloudBlockedForTesting()) {
        return {};
    }

    // U-G (session 261121_0001, report §3.2 Google row, contract C1): the
    // cloud query is no longer head/tail-truncated at 1,500 units (the old
    // SEC-M1 ClampCloudQuery, which silently deleted the middle of long
    // messages). The FULL text is split into C1 chunks and each chunk rides
    // the existing sequential dual-endpoint request path (REQ-R05 profiles
    // and timeouts unchanged) with its own primary -> gtx fallback. Every
    // cloud feeder (drag-icon, double-Ctrl+C, tooltip re-translate, the
    // Enter-translate worker, the local->cloud fallback) funnels through
    // this single choke point, and each chunk's text is <= kCloudChunkUnits
    // UTF-16 units, so every GET request line stays under the measured
    // ~16 KB endpoint limit (worst-case Korean: 10,800 encoded bytes).
    const std::vector<TranslationChunk> chunks = BuildCloudChunks(text);
    if (chunks.empty()) {
        // Whitespace-only input (chunker contract): nothing to translate, so
        // identity IS the correct result. This short-circuit runs BEFORE any
        // network I/O — it is NOT the REQ-048 P3 failure arm (that arm is
        // TranslateChunkUtf8's dual-endpoint `return {};`, still below).
        return std::wstring(text);
    }
    DIAG_F("GOOGLE_T/Translate/004: input of %zu UTF-16 units split into %zu cloud chunks\n",
           text.size(), chunks.size());

    const std::string sl = MapLanguageCode(src_code);
    const std::string tl = MapLanguageCode(tgt_code);

    std::wstring result;
    result.reserve(text.size());
    // F-2 (261121_0001 fix round 1): the sequential loop's whole-request
    // deadline starts before the first chunk; GetTickCount64 is monotonic.
    const unsigned long long loop_started_ms = ::GetTickCount64();
    // G-1 (261121_0001 fix round 2, SEC-F01 bytes half): the aggregate
    // response-byte counter starts at zero; every chunk response adds its
    // parsed piece size below and the whole request fails closed at
    // kCloudMaxTotalResponseBytes (see the header for the headroom math).
    size_t total_response_bytes = 0;
    for (size_t i = 0; i < chunks.size(); ++i) {
        const TranslationChunk& chunk = chunks[i];
        // 261121_0002 T1 (design-pick §Plumbing, Google row): a user cancel is
        // honored at the TOP of the iteration — BEFORE the loop-budget check,
        // before the no-content fold, before any further I/O — so a long run
        // unwinds within one chunk's request time (an in-flight synchronous
        // WinHTTP call cannot be aborted; see the design's honest limits). {}
        // is the EXISTING fail-closed arm; this function does not classify the
        // outcome — the manager attributes Canceled vs EngineFailed (T3).
        if (ChunkLoopCancelRequested(ctx.cancel)) {
            DIAG_F("GOOGLE_T/Translate/008: cancel requested at chunk %zu/%zu; "
                   "whole request unwinds (shape-only)\n",
                   i + 1, chunks.size());
            return {};
        }
        if (!ChunkNeedsRequest(chunk.text)) {
            // Pure-delimiter chunk (and, since fix round 1 F-1, the
            // whitespace-only shape the chunker emits at a packing boundary):
            // re-append its bytes verbatim; no GET. Nothing is translated and
            // nothing is lost — the byte-exact reassembly is unchanged.
            result += ToUtf16(chunk.text);
            result += ToUtf16(chunk.trailing_delimiter);
            continue;
        }
        if (ChunkLoopBudgetExhausted(::GetTickCount64() - loop_started_ms)) {
            // F-2: whole request fails closed through the existing arm below; the
            // localized notice carries the message (no new i18n string).
            DIAG_F("GOOGLE_T/Translate/006: chunk-loop budget exhausted (chunk %zu/%zu, "
                   "budget_ms=%llu); whole request fails closed\n",
                   i + 1, chunks.size(),
                   static_cast<unsigned long long>(GoogleTranslate::kChunkLoopTotalBudgetMs));
            return {};
        }
        // 261121_0002 T1 (design-pick §Plumbing): the progress tick fires
        // immediately BEFORE this chunk's request, once per chunk that actually
        // issues one — the pure-delimiter fold above already skipped its
        // iteration, so the reported index never advances without a request.
        // TranslateChunkUtf8's internal primary -> gtx fallback is still ONE
        // chunk request, so it never adds a second tick.
        // ShowChunkProgress gates on total > 1, which keeps a single-chunk run
        // byte-identical to the pre-feature build.
        if (ShowChunkProgress(chunks.size()) && ctx.on_chunk) {
            ctx.on_chunk(i + 1, chunks.size());
        }
        const std::wstring piece = TranslateChunkUtf8(chunk.text, sl, tl);
        if (piece.empty()) {
            // U-G: ANY chunk failure fails the WHOLE request — a partial
            // translation must not masquerade as complete (REQ-048 P3: empty
            // maps to EngineFailed in engine.cpp's cloud_call, surfacing the
            // failure sound/modal). No chunk is ever silently dropped.
            DIAG_F("GOOGLE_T/Translate/005: cloud chunk %zu/%zu failed; whole request fails closed\n",
                   i + 1, chunks.size());
            return {};
        }
        // G-1 (SEC-F01 bytes half): count this chunk response's parsed bytes
        // toward the AGGREGATE cap before appending — a hostile endpoint
        // streaming ~10 MiB per chunk must fail at 4 MiB total, not at
        // ~2.2 GB in the reassembled result.
        total_response_bytes += piece.size() * sizeof(wchar_t);
        if (total_response_bytes > kCloudMaxTotalResponseBytes) {
            // The existing whole-request failure arm: {} -> EngineFailed ->
            // the localized notice carries the message (no new i18n string).
            DIAG_F("GOOGLE_T/Translate/007: aggregate response bytes exceeded (chunk %zu/%zu, "
                   "total_bytes=%zu, cap_bytes=%zu); whole request fails closed\n",
                   i + 1, chunks.size(), total_response_bytes,
                   static_cast<size_t>(GoogleTranslate::kCloudMaxTotalResponseBytes));
            return {};
        }
        result += piece;
        // Byte-exact reassembly by the chunker contract: the delimiter rides
        // along untranslated, so the original structure (paragraph breaks,
        // sentence stops) always survives.
        result += ToUtf16(chunk.trailing_delimiter);
    }
    return result;
}

} // namespace emebalachat
