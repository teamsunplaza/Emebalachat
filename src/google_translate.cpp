#include "google_translate.hpp"
#include "config.hpp"
#include "unicode_utils.hpp"

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
                    if (pos + 4 > src.size()) return false;
                    std::string hex(src.substr(pos, 4));
                    pos += 4;
                    try {
                        uint32_t code = std::stoul(hex, nullptr, 16);
                        // Check for UTF-16 surrogate pair (e.g. \uD83D\uDE80)
                        if (code >= 0xD800 && code <= 0xDBFF && pos + 6 <= src.size() && src[pos] == '\\' && src[pos + 1] == 'u') {
                            std::string low_hex(src.substr(pos + 2, 4));
                            uint32_t low_code = std::stoul(low_hex, nullptr, 16);
                            if (low_code >= 0xDC00 && low_code <= 0xDFFF) {
                                pos += 6;
                                code = 0x10000 + ((code - 0xD800) << 10) + (low_code - 0xDC00);
                            }
                        }

                        if (code < 0x80) {
                            out += static_cast<char>(code);
                        } else if (code < 0x800) {
                            out += static_cast<char>(0xC0 | (code >> 6));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        } else if (code < 0x10000) {
                            out += static_cast<char>(0xE0 | (code >> 12));
                            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        } else {
                            out += static_cast<char>(0xF0 | (code >> 18));
                            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        }
                    } catch (...) {
                        return false;
                    }
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
    while (::WinHttpQueryDataAvailable(hRequest.get(), &bytes_avail) && bytes_avail > 0) {
        std::vector<char> buffer(bytes_avail);
        DWORD bytes_read = 0;
        if (::WinHttpReadData(hRequest.get(), buffer.data(), bytes_avail, &bytes_read) && bytes_read > 0) {
            response_body.append(buffer.data(), bytes_read);
        } else {
            break;
        }
    }

    return !response_body.empty();
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

std::string GoogleTranslate::MapLanguageCode(std::string_view code) {
    std::string norm = NormalizeLanguageCode(code);
    if (norm == "AUTO") return "auto";
    if (norm == "EN") return "en";
    if (norm == "KO") return "ko";
    if (norm == "VI") return "vi";
    if (norm == "ZH-CN") return "zh-CN";
    if (norm == "ZH-TW") return "zh-TW";
    if (norm == "JA") return "ja";
    if (norm == "ES") return "es";
    if (norm == "FR") return "fr";
    if (norm == "DE") return "de";
    if (norm == "RU") return "ru";
    if (norm == "TH") return "th";
    if (norm == "AR") return "ar";
    if (norm == "PT") return "pt";
    if (norm == "IT") return "it";
    if (norm == "ID") return "id";
    if (norm == "MS") return "ms";
    if (norm == "FIL") return "tl"; // Tagalog / Filipino
    if (norm == "KM") return "km";
    if (norm == "LO") return "lo";
    if (norm == "HI") return "hi";
    if (norm == "BN") return "bn";
    if (norm == "TR") return "tr";
    if (norm == "PL") return "pl";
    if (norm == "NL") return "nl";
    if (norm == "UK") return "uk";
    if (norm == "FA") return "fa";
    if (norm == "UR") return "ur";
    if (norm == "HE") return "iw";
    if (norm == "CS") return "cs";
    if (norm == "HU") return "hu";
    if (norm == "SV") return "sv";
    if (norm == "EL") return "el";
    if (norm == "RO") return "ro";
    if (norm == "DA") return "da";
    if (norm == "FI") return "fi";
    if (norm == "NO") return "no";
    if (norm == "MY") return "my";

    // Default fallback to lowercase
    std::string res;
    res.reserve(code.size());
    for (char c : code) {
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
                    while (pos < json.size() && json[pos] != ']') {
                        if (json[pos] == '\"') {
                            std::string dummy;
                            ParseJsonString(json, pos, dummy);
                        } else {
                            pos++;
                        }
                    }
                    if (pos < json.size() && json[pos] == ']') pos++;
                }
                SkipWhitespace(json, pos);
                if (pos < json.size() && json[pos] == ',') pos++;
            }
        } else if (pos < json.size() && json[pos] == '\"') {
            // Format 1: [ ["seg1", "ko"], ["seg2", "ko"] ]
            // We are already inside the first inner array: [ "seg1", "ko" ]
            std::string seg;
            if (ParseJsonString(json, pos, seg)) {
                accumulated_utf8.append(seg);
            }
            // Skip until ']'
            while (pos < json.size() && json[pos] != ']') {
                if (json[pos] == '\"') {
                    std::string dummy;
                    ParseJsonString(json, pos, dummy);
                } else {
                    pos++;
                }
            }
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
                    while (pos < json.size() && json[pos] != ']') {
                        if (json[pos] == '\"') {
                            std::string dummy;
                            ParseJsonString(json, pos, dummy);
                        } else {
                            pos++;
                        }
                    }
                    if (pos < json.size() && json[pos] == ']') pos++;
                } else {
                    break;
                }
            }
        }
    }

    return ToUtf16(accumulated_utf8);
}

std::wstring GoogleTranslate::Translate(
    std::wstring_view text,
    std::string_view src_code,
    std::string_view tgt_code
) {
    if (text.empty()) {
        return {};
    }

    std::string sl = MapLanguageCode(src_code);
    std::string tl = MapLanguageCode(tgt_code);
    std::string utf8_text = ToUtf8(text);
    std::string encoded_q = UrlEncode(utf8_text);

    // Primary: clients5.google.com with dict-chrome-ex
    std::string path_a = "/translate_a/t?client=dict-chrome-ex&sl=" + sl + "&tl=" + tl + "&q=" + encoded_q;
    std::wstring wpath_a = ToUtf16(path_a);

    std::string response;
    if (HttpGet(L"clients5.google.com", wpath_a, response)) {
        std::wstring result = ParseResponseJson(response);
        if (!result.empty()) {
            return result;
        }
    }

    // Fallback: translate.googleapis.com with gtx endpoint
    std::string path_b = "/translate_a/single?client=gtx&sl=" + sl + "&tl=" + tl + "&dt=t&q=" + encoded_q;
    std::wstring wpath_b = ToUtf16(path_b);
    if (HttpGet(L"translate.googleapis.com", wpath_b, response)) {
        std::wstring result = ParseResponseJson(response);
        if (!result.empty()) {
            return result;
        }
    }

    // Return original text if network translation fails
    return std::wstring(text);
}

} // namespace emebalachat
