#include "smart_bypass.hpp"
#include "diag_logger.hpp"
#include "config.hpp"
#include "unicode_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwctype>
#include <vector>
#include <windows.h>

namespace emebalachat {

namespace {

inline uint32_t DecodeNextCodePoint(std::wstring_view sv, size_t& idx) {
    wchar_t c = sv[idx++];
    if (c >= 0xD800 && c <= 0xDBFF && idx < sv.size()) {
        wchar_t low = sv[idx];
        if (low >= 0xDC00 && low <= 0xDFFF) {
            idx++;
            return 0x10000 + ((static_cast<uint32_t>(c) - 0xD800) << 10) + (static_cast<uint32_t>(low) - 0xDC00);
        }
    }
    return static_cast<uint32_t>(c);
}

inline bool IsKoreanCodePoint(uint32_t cp) {
    return (cp >= 0xAC00 && cp <= 0xD7A3) || // Hangul Syllables: 가..힣
           (cp >= 0x1100 && cp <= 0x11FF) || // Hangul Jamo
           (cp >= 0x3130 && cp <= 0x318F) || // Hangul Compatibility Jamo: ㅋㅋㅋ, ㅎㅎ, etc.
           (cp >= 0xA960 && cp <= 0xA97F) || // Hangul Jamo Extended-A
           (cp >= 0xD7B0 && cp <= 0xD7FF);   // Hangul Jamo Extended-B
}

inline bool IsKanaCodePoint(uint32_t cp) {
    return (cp >= 0x3040 && cp <= 0x309F) || // Hiragana
           (cp >= 0x30A0 && cp <= 0x30FF) || // Katakana
           (cp >= 0x31F0 && cp <= 0x31FF);   // Katakana Phonetic Extensions
}

inline bool IsThaiCodePoint(uint32_t cp) {
    return (cp >= 0x0E00 && cp <= 0x0E7F);
}

inline bool IsArabicCodePoint(uint32_t cp) {
    return (cp >= 0x0600 && cp <= 0x06FF) ||
           (cp >= 0x0750 && cp <= 0x077F) ||
           (cp >= 0x08A0 && cp <= 0x08FF);
}

inline bool IsCyrillicCodePoint(uint32_t cp) {
    return (cp >= 0x0400 && cp <= 0x04FF) ||
           (cp >= 0x0500 && cp <= 0x052F);
}

inline bool IsHanziCodePoint(uint32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0xF900 && cp <= 0xFAFF);
}

inline bool IsVietnameseCodePoint(uint32_t cp) {
    if (cp >= 0x1EA0 && cp <= 0x1EF9) return true; // Latin Extended Additional (tone marks)
    if (cp == 0x0102 || cp == 0x0103) return true; // Ă, ă
    if (cp == 0x0110 || cp == 0x0111) return true; // Đ, đ
    if (cp == 0x0168 || cp == 0x0169) return true; // Ũ, ũ
    if (cp == 0x01A0 || cp == 0x01A1) return true; // Ơ, ơ
    if (cp == 0x01AF || cp == 0x01B0) return true; // Ư, ư
    switch (cp) {
        case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3:
        case 0x00E8: case 0x00E9: case 0x00EA:
        case 0x00EC: case 0x00ED:
        case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5:
        case 0x00F9: case 0x00FA:
        case 0x00FD:
        case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3:
        case 0x00C8: case 0x00C9: case 0x00CA:
        case 0x00CC: case 0x00CD:
        case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5:
        case 0x00D9: case 0x00DA:
        case 0x00DD:
            return true;
        default:
            return false;
    }
}

inline bool IsLatinCodePoint(uint32_t cp) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
}

inline bool IsLinguisticCodePoint(uint32_t cp) {
    if (IsKoreanCodePoint(cp) || IsKanaCodePoint(cp) || IsHanziCodePoint(cp) ||
        IsThaiCodePoint(cp) || IsArabicCodePoint(cp) || IsCyrillicCodePoint(cp) ||
        IsVietnameseCodePoint(cp) || IsLatinCodePoint(cp)) {
        return true;
    }

    if (cp <= 0xFFFF) {
        wchar_t wch = static_cast<wchar_t>(cp);
        WORD ctype1 = 0;
        if (::GetStringTypeW(CT_CTYPE1, &wch, 1, &ctype1)) {
            if (ctype1 & C1_ALPHA) {
                return true;
            }
        }
    }
    return false;
}

bool CaseInsensitiveEqual(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

bool ContainsKorean(std::wstring_view text) {
    size_t idx = 0;
    while (idx < text.size()) {
        uint32_t cp = DecodeNextCodePoint(text, idx);
        if (IsKoreanCodePoint(cp)) {
            return true;
        }
    }
    return false;
}

bool ContainsKana(std::wstring_view text) {
    size_t idx = 0;
    while (idx < text.size()) {
        uint32_t cp = DecodeNextCodePoint(text, idx);
        if (IsKanaCodePoint(cp)) {
            return true;
        }
    }
    return false;
}

bool ContainsHanzi(std::wstring_view text) {
    size_t idx = 0;
    while (idx < text.size()) {
        uint32_t cp = DecodeNextCodePoint(text, idx);
        if (IsHanziCodePoint(cp)) {
            return true;
        }
    }
    return false;
}

bool ContainsCyrillic(std::wstring_view text) {
    size_t idx = 0;
    while (idx < text.size()) {
        uint32_t cp = DecodeNextCodePoint(text, idx);
        if (IsCyrillicCodePoint(cp)) {
            return true;
        }
    }
    return false;
}

bool ContainsArabic(std::wstring_view text) {
    size_t idx = 0;
    while (idx < text.size()) {
        uint32_t cp = DecodeNextCodePoint(text, idx);
        if (IsArabicCodePoint(cp)) {
            return true;
        }
    }
    return false;
}

bool ContainsThai(std::wstring_view text) {
    size_t idx = 0;
    while (idx < text.size()) {
        uint32_t cp = DecodeNextCodePoint(text, idx);
        if (IsThaiCodePoint(cp)) {
            return true;
        }
    }
    return false;
}

bool ContainsVietnamese(std::wstring_view text) {
    size_t idx = 0;
    while (idx < text.size()) {
        uint32_t cp = DecodeNextCodePoint(text, idx);
        if (IsVietnameseCodePoint(cp)) {
            return true;
        }
    }
    return false;
}

bool ContainsLatin(std::wstring_view text) {
    size_t idx = 0;
    while (idx < text.size()) {
        uint32_t cp = DecodeNextCodePoint(text, idx);
        if (IsLatinCodePoint(cp)) {
            return true;
        }
    }
    return false;
}

bool HasLinguisticContent(std::wstring_view text) {
    size_t idx = 0;
    while (idx < text.size()) {
        uint32_t cp = DecodeNextCodePoint(text, idx);
        if (IsLinguisticCodePoint(cp)) {
            return true;
        }
    }
    return false;
}

bool IsUrl(std::wstring_view text) {
    size_t start = 0;
    while (start < text.size() && iswspace(text[start])) start++;
    size_t end = text.size();
    while (end > start && iswspace(text[end - 1])) end--;
    if (start >= end) return false;

    std::wstring_view s = text.substr(start, end - start);

    // Standalone URL does not have internal whitespace
    for (wchar_t c : s) {
        if (iswspace(c)) return false;
    }

    auto starts_with_ic = [](std::wstring_view str, std::wstring_view prefix) {
        if (str.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (towlower(str[i]) != towlower(prefix[i])) return false;
        }
        return true;
    };

    if (starts_with_ic(s, L"http://") ||
        starts_with_ic(s, L"https://") ||
        starts_with_ic(s, L"ftp://") ||
        starts_with_ic(s, L"www.")) {
        return true;
    }

    static const std::vector<std::wstring_view> kTlds = {
        L".com", L".org", L".net", L".edu", L".gov", L".io",
        L".ai", L".kr", L".jp", L".cn", L".vn", L".me", L".info"
    };

    for (auto tld : kTlds) {
        size_t pos = 0;
        while (pos + tld.size() <= s.size()) {
            bool match = true;
            for (size_t i = 0; i < tld.size(); ++i) {
                if (towlower(s[pos + i]) != towlower(tld[i])) {
                    match = false;
                    break;
                }
            }
            if (match && pos > 0) {
                size_t after = pos + tld.size();
                if (after == s.size() || s[after] == L'/') {
                    return true;
                }
            }
            pos++;
        }
    }

    return false;
}

std::string DetectLanguage(std::wstring_view text) {
    std::wstring norm = NormalizeNFC(text);

    // Trim whitespace
    size_t start = 0;
    while (start < norm.size() && iswspace(norm[start])) start++;
    size_t end = norm.size();
    while (end > start && iswspace(norm[end - 1])) end--;
    if (start >= end) {
        return "Unknown";
    }

    std::wstring_view trimmed = std::wstring_view(norm).substr(start, end - start);

    if (!HasLinguisticContent(trimmed)) {
        return "Unknown";
    }

    // Script priority order:
    if (ContainsKorean(trimmed)) return "Korean";
    if (ContainsKana(trimmed)) return "Japanese"; // Kana takes precedence over Hanzi for Japanese
    if (ContainsThai(trimmed)) return "Thai";
    if (ContainsArabic(trimmed)) return "Arabic";
    if (ContainsCyrillic(trimmed)) return "Russian";
    if (ContainsVietnamese(trimmed)) return "Vietnamese";
    if (ContainsHanzi(trimmed)) return "Chinese Simplified";
    if (ContainsLatin(trimmed)) return "English";

    return "Unknown";
}

bool ShouldTranslate(
    std::wstring_view text,
    std::string_view target_code_or_name,
    std::string_view source_code_or_name
) {
    // 1. Trim whitespace
    size_t start = 0;
    while (start < text.size() && iswspace(text[start])) start++;
    size_t end = text.size();
    while (end > start && iswspace(text[end - 1])) end--;
    if (start >= end) {
        return false;
    }

    std::wstring_view trimmed = text.substr(start, end - start);

    // 2. Fast URL bypass
    if (IsUrl(trimmed)) {
        return false;
    }

    // 3. Fast non-linguistic bypass (digits, punctuation, emojis, spaces)
    if (!HasLinguisticContent(trimmed)) {
        return false;
    }

    // 4. Resolve target language
    std::string target_code = NormalizeLanguageCode(target_code_or_name);
    const auto* target_info = FindLanguageByCode(target_code);
    std::string target_name = target_info ? target_info->name_en : std::string(target_code_or_name);

    // 5. If input text is already in the target language, bypass translation immediately
    std::string detected = DetectLanguage(trimmed);
    if (detected != "Unknown") {
        if (CaseInsensitiveEqual(detected, target_name) ||
            CaseInsensitiveEqual(NormalizeLanguageCode(detected), target_code)) {
            // R5 observability: pin the exact bypass so "didn't translate after
            // a language switch" is attributable at runtime.
            DIAG_F(
                    "SMART_BYPASS/ShouldTranslate/001: already-target bypass (detected=%s, target=%s/%s)\n",
                    detected.c_str(), target_name.c_str(), target_code.c_str());
            return false;
        }
        if (target_code.starts_with("ZH") && detected.find("Chinese") != std::string::npos) {
            if (CaseInsensitiveEqual(detected, target_name)) {
                DIAG_F(
                        "SMART_BYPASS/ShouldTranslate/002: Chinese-variant bypass (detected=%s, target=%s/%s)\n",
                        detected.c_str(), target_name.c_str(), target_code.c_str());
                return false;
            }
        }
    }

    // 6. Resolve source language
    std::string effective_source_name;
    std::string source_norm = NormalizeLanguageCode(source_code_or_name);
    if (source_norm != "AUTO" && !source_code_or_name.empty() && !CaseInsensitiveEqual(source_code_or_name, "Auto Detect")) {
        const auto* src_info = FindLanguageByCode(source_norm);
        effective_source_name = src_info ? src_info->name_en : std::string(source_code_or_name);
    } else {
        effective_source_name = detected;
    }

    // 7. Check if configured source matches target
    if (effective_source_name != "Unknown") {
        if (CaseInsensitiveEqual(effective_source_name, target_name) ||
            CaseInsensitiveEqual(NormalizeLanguageCode(effective_source_name), target_code)) {
            return false;
        }

        // Chinese variant normalization
        if (target_code.starts_with("ZH") && (effective_source_name.find("Chinese") != std::string::npos)) {
            if (CaseInsensitiveEqual(effective_source_name, target_name)) {
                return false;
            }
        }
    }

    return true;
}

} // namespace emebalachat
