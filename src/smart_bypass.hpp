#pragma once

#include <string>
#include <string_view>

namespace emebalachat {

// Returns true if text contains any Korean characters (Hangul syllables, Jamo, Compatibility Jamo).
bool ContainsKorean(std::wstring_view text);

// Returns true if text contains Japanese Hiragana or Katakana.
bool ContainsKana(std::wstring_view text);

// Returns true if text contains Hanzi / CJK Ideographs.
bool ContainsHanzi(std::wstring_view text);

// Returns true if text contains Cyrillic characters (Russian, Ukrainian).
bool ContainsCyrillic(std::wstring_view text);

// Returns true if text contains Arabic script characters.
bool ContainsArabic(std::wstring_view text);

// Returns true if text contains Thai script characters.
bool ContainsThai(std::wstring_view text);

// Returns true if text contains Vietnamese specific diacritics or modified characters.
bool ContainsVietnamese(std::wstring_view text);

// Returns true if text contains Latin alphabetic characters.
bool ContainsLatin(std::wstring_view text);

// Detects language of given text based on script analysis.
// Returns "Korean", "Japanese", "Vietnamese", "Chinese Simplified", "Russian", "Thai", "Arabic", "English", or "Unknown".
std::string DetectLanguage(std::wstring_view text);

// Returns true if text represents a standalone URL or web domain.
bool IsUrl(std::wstring_view text);

// Returns true if text contains any linguistic characters (letters/ideographs) as opposed to pure numbers, symbols, spaces, or emojis.
bool HasLinguisticContent(std::wstring_view text);

// Core translation decision engine.
// Evaluates whether the given text needs translation to target_code_or_name.
// Returns false if text is empty/whitespace, a URL, pure digits/symbols/emojis, or if source language matches target.
bool ShouldTranslate(
    std::wstring_view text,
    std::string_view target_code_or_name,
    std::string_view source_code_or_name = "Auto Detect"
);

} // namespace emebalachat
