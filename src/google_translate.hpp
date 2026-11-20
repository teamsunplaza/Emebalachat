#pragma once

#include <string>
#include <string_view>
#include <vector>

// U-G (session 261121_0001, vp-plan contract C1): the shared translation
// chunker lives in the engine repo and is header-only/pure; it reaches this
// header through the existing PUBLIC include of Emebala_engine_host.
#include "translation_chunker.hpp"

namespace emebalachat {

// ---- REQ-R05: per-endpoint WinHTTP request profiles ---------------------
// clients5.google.com/translate_a/t?client=dict-chrome-ex is the Chrome
// extension-only endpoint: with a non-Chrome User-Agent Google answers
// 403/429, and the previous 3/3/5/5 s timeout split let such a response
// (or a hung socket) block the worker for up to 16 s, wedging the whole
// translation pipeline (audit §2.4). The Chrome profile therefore sends a
// standard desktop Chrome UA + Chrome-typical headers and caps every
// phase so the total request budget stays <= 8 s. The public gtx
// fallback endpoint keeps the honest product UA and the previous split.
//
// The selection is a pure constexpr path -> profile function
// (GoogleTranslate::RequestProfileForPath) used directly by HttpGet, so
// google_translate.cpp pins the REAL wiring with static_asserts and the
// test suite asserts it headlessly - no live socket needed.
struct GoogleHttpProfile {
    std::wstring_view user_agent;   // NUL-terminated source literal
    int resolve_ms;                 // WinHttpSetTimeouts: resolve phase
    int connect_ms;                 // WinHttpSetTimeouts: connect phase
    int send_ms;                    // WinHttpSetTimeouts: send phase
    int receive_ms;                 // WinHttpSetTimeouts: receive phase
};

// REQ-R05: standard stable-channel Chrome desktop UA. dict-chrome-ex gatekeeps
// on the Chrome/<major> token; Emebalachat/0.10 gets 403/429. Version 128 is a
// pinned stable release line; bump alongside engine updates. Literals are
// NUL-terminated so .data() is safe for WinHttpOpen.
inline constexpr std::wstring_view kChromeUserAgent =
    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    L"(KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36";

// L4 lineage: truthful product UA, retained for the public gtx endpoint which
// explicitly does not require a browser identity.
inline constexpr std::wstring_view kProductUserAgent =
    L"Emebalachat/0.10 (+https://github.com/teamsunplaza/Emebalachat)";

class GoogleTranslate {
public:
    // Translates text using Google Translate free endpoint via native WinHTTP.
    // U-G (session 261121_0001, report §3.2 Google row, contract C1): inputs
    // longer than kCloudChunkUnits are translated IN FULL — split into chunks,
    // one GET per chunk on the existing request path, reassembled byte-exactly
    // — never head/tail-truncated.
    // Returns translated std::wstring, or EMPTY on network failure (REQ-048 P3:
    // an identity passthrough used to masquerade failures as successes). Fix
    // round 2 G-1: the sequential loop also fails closed when the aggregate of
    // all chunk response bytes exceeds kCloudMaxTotalResponseBytes (4 MiB).
    static std::wstring Translate(
        std::wstring_view text,
        std::string_view src_code = "AUTO",
        std::string_view tgt_code = "EN"
    );

    // REQ-043 (M6 T5 debug): test-only cloud-block hook. When engaged,
    // Translate() short-circuits to empty (simulating an unreachable cloud)
    // WITHOUT touching WinHTTP. This exists so the T5 manager-routing suites
    // can exercise the manager->cloud_call() seam deterministically, without
    // depending on the live network (the original design hung the test
    // process in a WinHTTP sync call on the second invocation). Production
    // callers never engage this hook; the flag defaults to false and the
    // setter is only called from tests. Shape-only: no user text is logged.
    static void SetCloudBlockedForTesting(bool blocked);
    static bool IsCloudBlockedForTesting();

    // REQ-R05: standard Chrome desktop UA sent on the dict-chrome-ex profile.
    static constexpr std::wstring_view ChromeUserAgent() {
        return kChromeUserAgent;
    }

    // REQ-R05: Chrome-typical request headers (CRLF-separated, CRLF-terminated)
    // appended via WinHttpSendRequest on the dict-chrome-ex profile.
    static std::wstring ChromeAdditionalHeaders();

    // REQ-R05: true when a request path targets the Chrome-extension-only
    // dict-chrome-ex endpoint (drives profile selection inside HttpGet).
    static constexpr bool IsDictChromeExEndpoint(std::wstring_view path) {
        return path.find(L"client=dict-chrome-ex") != std::wstring_view::npos;
    }

    // REQ-R05: pure endpoint -> (UA, timeouts) selection actually consumed by
    // HttpGet. Chrome profile: 1500/2000/2000/2500 ms (total exactly 8000);
    // gtx profile: 3000/3000/5000/5000 ms with the truthful product UA.
    static constexpr GoogleHttpProfile RequestProfileForPath(std::wstring_view path) {
        if (IsDictChromeExEndpoint(path)) {
            // resolve 1.5s / connect 2s / send 2s / receive 2.5s = exactly 8s cap.
            return GoogleHttpProfile{ kChromeUserAgent, 1500, 2000, 2000, 2500 };
        }
        // Public gtx endpoint: previous (unchanged) 3/3/5/5 s split, product UA.
        return GoogleHttpProfile{ kProductUserAgent, 3000, 3000, 5000, 5000 };
    }

    // Maps internal language code to Google's ISO/BCP47 code (e.g. "FIL" -> "tl", "ZH-CN" -> "zh-CN").
    static std::string MapLanguageCode(std::string_view code);

    // U-G (session 261121_0001, report §3.2 Google row, vp-plan contract C1):
    // per-chunk cloud query size in UTF-16 code units. Long inputs are
    // translated IN FULL via SplitTextIntoTranslationChunks (see
    // BuildCloudChunks) with one GET per chunk — the v0.10.8-and-earlier
    // SEC-M1 head/tail truncation (kMaxCloudQueryUnits + ClampCloudQuery,
    // deleted in this session) silently dropped the middle of any message
    // over 1,500 units, the CEO-reported defect. The bound is now
    // transport-only: the endpoint is URL-byte bounded (live probes:
    // 16,200 B OK / 18,000 B -> 400 on both endpoints), worst case is Korean
    // 1 UTF-16 unit -> 3 UTF-8 bytes -> 9 percent-encoding chars, so 1,200
    // units encode to <= 10,800 B per request, under the OK boundary with
    // headroom for the fixed path prefix.
    static inline constexpr size_t kCloudChunkUnits = 1200;
    static_assert(kCloudChunkUnits * 9 < 16200,
                  "U-G: worst-case Korean encoding must stay under the measured 16,200-byte OK boundary");

    // U-G + fix round 1 (F-1): whether a chunk needs its own GET. FALSE when the
    // chunk's text carries no translatable content — empty (the delimiter-only
    // shape) or ASCII-whitespace-only — because there is nothing to translate
    // and a blank query either wastes a GET or produces an empty parse that the
    // caller turns into a WHOLE-request failure. Translate() re-appends such a
    // chunk's text and trailing_delimiter verbatim, so the byte-exact
    // reassembly is unaffected. Whitespace is the chunker's own definition: no
    // byte greater than 0x20. The OpenAI client applies the same rule
    // (OpenAiChunkNeedsRequest); both exist because the pure predicate belongs
    // in the shared chunker header, which this fix round does not own.
    static constexpr bool ChunkNeedsRequest(std::string_view text) {
        for (const char c : text) {
            if (static_cast<unsigned char>(c) > 0x20) return true;
        }
        return false;
    }

    // U-G + fix round 1 (F-2): whole-request wall-clock budget for the
    // sequential per-chunk GET loop. Each chunk can cost up to two endpoint
    // attempts (~8 s + ~16 s worst case on the REQ-R05 profiles), so a maximal
    // Enter capture (~219 chunks) could hold the pipeline worker for roughly an
    // hour with no progress surface. 300,000 ms (5 min) is far above any
    // realistic full-text translation at those profiles and short enough that a
    // pathological input gives up honestly; the loop checks it before every
    // chunk request and fails the whole request closed (the localized failure
    // notice is the message, no new i18n string).
    static inline constexpr unsigned long long kChunkLoopTotalBudgetMs = 300000;
    static_assert(kChunkLoopTotalBudgetMs == 300000,
                  "F-2: the Google chunk-loop budget is exactly 300,000 ms (5 min)");

    // Pure form of that deadline test, so the suite pins the edge without a
    // clock: 299,999 -> keep going, 300,000 -> abort.
    static constexpr bool ChunkLoopBudgetExhausted(unsigned long long elapsed_ms) {
        return elapsed_ms >= kChunkLoopTotalBudgetMs;
    }

    // U-G + fix round 2 (G-1, SEC-F01 bytes half): the AGGREGATE response-byte
    // ceiling for ONE whole Translate call. Each chunk GET's body is already
    // capped at 10 MiB (kMaxResponseBodyBytes in the .cpp), but a maximal
    // Enter capture makes ~219 chunk requests, so a hostile endpoint answering
    // ~10 MiB per chunk accumulated ~2.2 GB in the reassembled `result` before
    // anything failed. The loop therefore sums the parsed bytes every chunk
    // response contributed and returns {} (the existing REQ-048 P3 failure arm)
    // the moment the sum exceeds this cap. Headroom math: one bounded chunk is
    // <= 1,200 UTF-16 units of source, so even a pathological-but-legitimate
    // translation (4x expansion of CJK into UTF-16) is ~9.6 KB per chunk;
    // 4 MiB is >400x that per-chunk figure, and a full 219-chunk worst-case
    // run is ~2.1 MB, i.e. the cap leaves ~2x over the entire maximal input.
    // The loop only sees the parsed piece (the GET body is consumed inside
    // TranslateChunkUtf8), so it accumulates piece bytes — the memory the
    // attack actually exhausts — counting wchar_t as the 2 bytes of this
    // UTF-16 build; the re-appended source delimiters are source-bounded and
    // not attacker-controlled.
    static inline constexpr size_t kCloudMaxTotalResponseBytes = 4u * 1024u * 1024u;
    static_assert(kCloudMaxTotalResponseBytes == 4194304,
                  "G-1: the aggregate cloud response-byte cap is exactly 4 MiB");

    // U-G: pure chunk seam (unit-testable, no network) — the exact chunk list
    // Translate() sends: SplitTextIntoTranslationChunks(ToUtf8(text),
    // kCloudChunkUnits). Translate() issues one GET per chunk and reassembles
    // translated text + trailing_delimiter over all chunks, reproducing the
    // source structure byte-exactly (delimiters are never sent to the
    // endpoint, so they survive verbatim). The chunker never splits inside a
    // UTF-8 sequence or surrogate pair, and a run that still exceeds
    // max_units is hard-split by the chunker itself (max_units guarantees the
    // transport bound) — no separate truncation path exists. Empty or
    // whitespace-only input yields an empty vector (chunker contract);
    // Translate() short-circuits that case to the input itself BEFORE any
    // network I/O.
    static std::vector<TranslationChunk> BuildCloudChunks(std::wstring_view text);

    // Encodes UTF-8 string into RFC 3986 percent-encoded format.
    static std::string UrlEncode(std::string_view str);

    // Parses Google Translate response JSON, extracting and concatenating translation segments.
    static std::wstring ParseResponseJson(std::string_view json);
};

} // namespace emebalachat
