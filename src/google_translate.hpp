#pragma once

#include <string>
#include <string_view>

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
    // Returns translated std::wstring, or original/empty on network failure.
    static std::wstring Translate(
        std::wstring_view text,
        std::string_view src_code = "AUTO",
        std::string_view tgt_code = "EN"
    );

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

    // Encodes UTF-8 string into RFC 3986 percent-encoded format.
    static std::string UrlEncode(std::string_view str);

    // Parses Google Translate response JSON, extracting and concatenating translation segments.
    static std::wstring ParseResponseJson(std::string_view json);
};

} // namespace emebalachat
