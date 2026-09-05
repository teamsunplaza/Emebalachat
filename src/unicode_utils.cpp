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

} // namespace emebalachat
