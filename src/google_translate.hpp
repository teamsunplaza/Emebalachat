#pragma once

#include <string>
#include <string_view>

namespace emebalachat {

class GoogleTranslate {
public:
    // Translates text using Google Translate free endpoint via native WinHTTP.
    // Returns translated std::wstring, or original/empty on network failure.
    static std::wstring Translate(
        std::wstring_view text,
        std::string_view src_code = "AUTO",
        std::string_view tgt_code = "EN"
    );

    // Maps internal language code to Google's ISO/BCP47 code (e.g. "FIL" -> "tl", "ZH-CN" -> "zh-CN").
    static std::string MapLanguageCode(std::string_view code);

    // Encodes UTF-8 string into RFC 3986 percent-encoded format.
    static std::string UrlEncode(std::string_view str);

    // Parses Google Translate response JSON, extracting and concatenating translation segments.
    static std::wstring ParseResponseJson(std::string_view json);
};

} // namespace emebalachat
