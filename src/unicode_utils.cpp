#include "unicode_utils.hpp"

#include <windows.h>


namespace emebalachat {

std::wstring NormalizeNFC(std::wstring_view input) {
    if (input.empty()) {
        return {};
    }

    // Determine required character count
    int needed = ::NormalizeString(
        NormalizationC,
        input.data(),
        static_cast<int>(input.size()),
        nullptr,
        0
    );

    if (needed <= 0) {
        // Fallback gracefully to returning input copy if normalization fails
        return std::wstring(input);
    }

    std::wstring result(needed, L'\0');
    int written = ::NormalizeString(
        NormalizationC,
        input.data(),
        static_cast<int>(input.size()),
        result.data(),
        needed
    );

    if (written <= 0) {
        return std::wstring(input);
    }

    result.resize(written);
    return result;
}

std::wstring NormalizeNewlinesToCRLF(std::wstring_view input) {
    // Multi-line block fix. Pure single pass over UTF-16 units; 0x0D/0x0A never
    // participate in surrogate pairs, so per-unit scanning is surrogate-safe
    // for every script (KO/EN/JA/ZH/VI/ES, emoji included).
    if (input.find_first_of(L"\r\n") == std::wstring_view::npos) {
        return std::wstring(input);
    }
    std::wstring out;
    out.reserve(input.size() + 8);
    for (size_t i = 0; i < input.size(); ++i) {
        const wchar_t c = input[i];
        if (c == L'\r') {
            out += L"\r\n";
            // Collapse a following '\n' of an existing CRLF pair so CRLF never
            // doubles ("\r\n" stays "\r\n", not "\r\r\n").
            if (i + 1 < input.size() && input[i + 1] == L'\n') {
                ++i;
            }
        } else if (c == L'\n') {
            out += L"\r\n";
        } else {
            out += c;
        }
    }
    return out;
}

std::string ToUtf8(std::wstring_view wstr) {
    if (wstr.empty()) {
        return {};
    }

    int needed = ::WideCharToMultiByte(
        CP_UTF8,
        0,
        wstr.data(),
        static_cast<int>(wstr.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );

    if (needed <= 0) {
        return {};
    }

    std::string str(needed, '\0');
    int written = ::WideCharToMultiByte(
        CP_UTF8,
        0,
        wstr.data(),
        static_cast<int>(wstr.size()),
        str.data(),
        needed,
        nullptr,
        nullptr
    );

    if (written <= 0) {
        return {};
    }

    str.resize(written);
    return str;
}

std::wstring ToUtf16(std::string_view str) {
    if (str.empty()) {
        return {};
    }

    int needed = ::MultiByteToWideChar(
        CP_UTF8,
        0,
        str.data(),
        static_cast<int>(str.size()),
        nullptr,
        0
    );

    if (needed <= 0) {
        return {};
    }

    std::wstring wstr(needed, L'\0');
    int written = ::MultiByteToWideChar(
        CP_UTF8,
        0,
        str.data(),
        static_cast<int>(str.size()),
        wstr.data(),
        needed
    );

    if (written <= 0) {
        return {};
    }

    wstr.resize(written);
    return wstr;
}

// REF-3.5 (session 260910_0006 T4): extracted verbatim from config.cpp
// SimpleJsonReader::ParseString case 'u' (the I2-defended reference
// implementation). google_translate.cpp ParseJsonString case 'u' then adopts
// it, closing that parser's lone-surrogate WTF-8 gap. See the header comment
// for the full contract. Every statement below is a line-for-line move of
// config.cpp L209-269 except: (a) src_/pos_ become parameters, (b) the two
// "return false" failure paths become "return false" of the helper (callers
// propagate them to their own string-parse failure), and (c) the success
// path falls through to the shared UTF-8 encoder then returns true.
bool AppendJsonUnicodeEscape(std::string& out, std::string_view src,
                             std::size_t& pos) {
    // Reads the next 4 chars at pos as a hex code unit; false on truncation
    // or when std::stoul throws (no leading hex digit at all).
    auto read_hex4 = [&src, &pos](std::uint32_t& out_code) -> bool {
        if (pos + 4 > src.size()) return false;
        const std::string hex(src.substr(pos, 4));
        pos += 4;
        try {
            out_code = static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
        } catch (...) {
            return false;
        }
        return true;
    };

    std::uint32_t code = 0;
    if (!read_hex4(code)) return false;

    // I2 fix: decode UTF-16 surrogate pairs (high D800-DBFF followed by low
    // DC00-DFFF) into the real code point. A LONE surrogate is not valid
    // scalar Unicode; encoding it would emit corrupt WTF-8 bytes. Replace
    // lone surrogates with U+FFFD so the output is always well-formed UTF-8.
    if (code >= 0xD800u && code <= 0xDBFFu) {
        bool paired = false;
        if (pos + 6 <= src.size() && src[pos] == '\\' && src[pos + 1] == 'u') {
            const std::size_t save_pos = pos;
            pos += 2; // step over "\u"
            std::uint32_t low = 0;
            if (read_hex4(low) && low >= 0xDC00u && low <= 0xDFFFu) {
                code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
                paired = true;
            } else {
                pos = save_pos; // not a valid pair; re-parse next escape normally
            }
        }
        if (!paired) {
            code = 0xFFFDu;
        }
    } else if (code >= 0xDC00u && code <= 0xDFFFu) {
        code = 0xFFFDu; // lone low surrogate
    }

    if (code < 0x80u) {
        out += static_cast<char>(code);
    } else if (code < 0x800u) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000u) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        // 4-byte UTF-8 for supplementary-plane code points
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
    return true;
}

} // namespace emebalachat
