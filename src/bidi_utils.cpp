#include "bidi_utils.hpp"

#include "config.hpp"
#include "i18n.hpp"
#include "unicode_utils.hpp"

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace emebalachat {

namespace {

// REF-3.1 (session 260910_0006 T1): the former file-local EqualsIgnoreCaseAscii
// is replaced by the shared template in unicode_utils.hpp. Equivalent under
// this project's default "C" CRT locale (no setlocale call exists in src/):
// std::tolower mapped only 'A'-'Z' there, exactly the +32 fold set; high bytes
// compared verbatim in both implementations.

// The closed RTL language-code set (design §2-Q1 verdict A / P2 §A2): exactly
// 4 of the 37 registry languages are right-to-left.
constexpr std::array<std::string_view, 4> kRtlLanguageCodes = {
    "AR",  // Arabic
    "FA",  // Persian
    "UR",  // Urdu
    "HE",  // Hebrew
};

bool MatchesRtlCodeSet(std::string_view code) {
    for (const std::string_view rtl : kRtlLanguageCodes) {
        if (EqualsIgnoreCaseAscii(code, rtl)) return true;
    }
    return false;
}

// ---- First-strong classification tables (UAX #9 strong types only) --------
// Inclusive [first, last] code-point ranges (design §2-Q1 implementation
// note: static range checks, no Win32/ICU categoricals). Characters in NO
// table are treated as weak/neutral and SKIPPED by the scanner - which covers
// exactly the design's skip list: ASCII digits, whitespace, punctuation and
// symbols, and every Bidi_Control / bidi-formatting char (U+200B..U+200F
// ZW*SP/LRM/RLM, U+202A..U+202E LRE/RLE/PDF, U+2060..U+2064 BN, U+2066..U+2069
// LRI/RLI/FSI/PDI, U+FEFF BOM): none of them fall inside any strong range
// below.

struct CodeRange {
    unsigned int first;
    unsigned int last;
};

constexpr bool InAny(const std::span<const CodeRange> table, const unsigned int cp) {
    for (const CodeRange& r : table) {
        if (cp >= r.first && cp <= r.last) return true;
    }
    return false;
}

// Hebrew base block (UAX #9 class R), defined ONCE and reused by the
// first-strong table below and the exported G-4 predicate
// (IsHebrewScriptCodePoint) so the two can never drift.
constexpr CodeRange kHebrewBlock = {0x0590, 0x05FF};

// Class R (explicit right-to-left), per design §2-Q1:
//   U+0590–U+05FF  Hebrew
//   U+07C0–U+085F  NKo/Samaritan/Mandaic band - the design's "and U+07C0–U+085F
//                  Samaritan if trivial" range, taken verbatim
//   U+FB1D–U+FB4F  Hebrew presentation forms
constexpr CodeRange kStrongR[] = {
    kHebrewBlock,
    {0x07C0, 0x085F},
    {0xFB1D, 0xFB4F},
};

// Class AL (Arabic letter) blocks - treated as RTL per design §2-Q1:
//   U+0600–U+06FF  Arabic (AN digit sub-ranges filtered separately below)
//   U+0750–U+07BF  Arabic Supplement + Thaana (design's U+0750–U+077F, Thaana
//                  extended to keep the RTL band contiguous)
//   U+0860–U+08FF  Syriac Supplement + Arabic Extended-A (design's
//                  U+08A0–U+08FF, Syriac Supplement extended: also RTL letters)
//   U+FB50–U+FDFF  Arabic Presentation Forms-A
//   U+FE70–U+FEFC  Arabic Presentation Forms-B letters (design's U+FE70–U+FEFF
//                  minus U+FEFD/FEFF: those are nonletters / BOM, neutral)
constexpr CodeRange kStrongAL[] = {
    {0x0600, 0x06FF},
    {0x0750, 0x07BF},
    {0x0860, 0x08FF},
    {0xFB50, 0xFDFF},
    {0xFE70, 0xFEFC},
};

// AN (Arabic Number) digit sub-ranges of the Arabic block: Unicode BidiClass
// AN - NOT strong, so first-strong scanning must SKIP them (design §2-Q1
// "digits, ... skip"): "١٢٣ مرحبا" -> RTL via the letters; "١٢٣ hello" -> LTR.
//   U+0660–U+0669  Arabic-Indic digits
//   U+06F0–U+06F9  Extended Arabic-Indic digits
//   U+07C0–U+07C9  NKo digits (AN; inside the R band above - filter first)
constexpr CodeRange kDigitsAN[] = {
    {0x0660, 0x0669},
    {0x06F0, 0x06F9},
    {0x07C0, 0x07C9},
};

// Class L (strong left-to-right) blocks - curated to design §2-Q1 ("Latin,
// Hangul, Kana, Han, Thai, etc."): every alphabetic range outside the R/AL
// tables above, for the scripts the 37 translation languages can produce.
// Script digit blocks are deliberately EXCLUDED (Nd => treated neutral like
// ASCII digits, per the design's blanket "digits ... skip" rule).
constexpr CodeRange kStrongL[] = {
    {0x0041, 0x005A},  // Latin A-Z
    {0x0061, 0x007A},  // Latin a-z
    {0x00AA, 0x00AA},  // ordinal indicator (Lo)
    {0x00B5, 0x00B5},  // micro sign (L)
    {0x00BA, 0x00BA},  // ordinal indicator (Lo)
    {0x00C0, 0x00D6},  // Latin-1 letters up to Ö
    {0x00D8, 0x00F6},  // Latin-1 letters Ö..ö (0x00D7 'x' multiply kept neutral)
    {0x00F8, 0x02FF},  // Latin Ext-A/B, IPA, modifier letters (0x00F7 ':' kept out above)
    {0x0370, 0x037D},  // Greek letters (0x037E ';' question mark excluded)
    {0x037F, 0x03FF},  // Greek and Coptic (tonos/teleia noise chars classed L:
                       // heuristic, documented for the §5.2 debug review)
    {0x0400, 0x052F},  // Cyrillic + supplement (RU/UK primary)
    {0x0531, 0x058F},  // Armenian
    {0x0900, 0x0965},  // Devanagari letters (digits 0x0966.. excluded) - HI
    {0x0980, 0x09E3},  // Bengali letters (digits 0x09E6.. excluded) - BN
    {0x0A00, 0x0A65},  // Gurmukhi letters
    {0x0A80, 0x0AE5},  // Gujarati letters
    {0x0B00, 0x0B65},  // Odia letters
    {0x0B80, 0x0BE5},  // Tamil letters
    {0x0C00, 0x0C65},  // Telugu letters
    {0x0C80, 0x0CE5},  // Kannada letters
    {0x0D00, 0x0D65},  // Malayalam letters
    {0x0D80, 0x0DF6},  // Sinhala letters
    {0x0E01, 0x0E3A},  // Thai letters - TH
    {0x0E40, 0x0E46},  // Thai above-vowels + lakh kan
    {0x0E81, 0x0EAC},  // Lao letters - LO
    {0x0EB1, 0x0EBC},  // Lao vowels (0x0EBD excluded; tone marks stay neutral)
    {0x0EC0, 0x0EC4},  // Lao pre-vowels
    {0x0F40, 0x0F6C},  // Tibetan letters
    {0x1000, 0x102A},  // Myanmar letters - MY
    {0x1031, 0x1031},  // Myanmar vowel sign E (Lo-class)
    {0x1038, 0x1038},  // Myanmar sign visarga (Lo-class)
    {0x1050, 0x105D},  // Tai Viet letters
    {0x10A0, 0x10C5},  // Georgian Asomtavruli
    {0x10D0, 0x10FA},  // Georgian Mkhedruli
    {0x1100, 0x11FF},  // Hangul Jamo - KO
    {0x1200, 0x137D},  // Ethiopic
    {0x13A0, 0x13FF},  // Cherokee
    {0x1401, 0x167F},  // Canadian Aboriginal Syllabics
    {0x1681, 0x169A},  // Runic
    {0x16A0, 0x16EA},  // Ogham
    {0x1780, 0x17B3},  // Khmer letters - KM
    {0x1820, 0x18AB},  // Mongolian
    {0x1900, 0x191E},  // Limbu letters
    {0x1950, 0x196D},  // Tai Le letters
    {0x1970, 0x1974},  // Tai Le vowels
    {0x1B05, 0x1B33},  // Balinese letters (digits 1B4F+ excluded? kept letters only)
    {0x2E80, 0x2FD5},  // CJK radicals + Kangxi
    {0x3005, 0x3007},  // CJK iteration marks / ideographic number one
    {0x3021, 0x3029},  // CJK hangul-number-style numerals (Han, strong L)
    {0x3041, 0x3096},  // Hiragana - JA
    {0x30A1, 0x30FA},  // Katakana - JA
    {0x30FC, 0x30FF},  // Katakana prolongation / iteration / vu
    {0x3105, 0x312F},  // Bopomofo - zh
    {0x3131, 0x318E},  // Hangul compatibility jamo - KO
    {0x31A0, 0x31BF},  // Bopomofo extended
    {0x3400, 0x4DBF},  // CJK unified ideographs ext A - zh
    {0x4E00, 0x9FFF},  // CJK unified ideographs - zh
    {0xA000, 0xA48C},  // Yi syllables
    {0xAC00, 0xD7A3},  // Hangul syllables - KO
    {0xF900, 0xFAFF},  // CJK compatibility ideographs (0xFA7E/0xFA7F are marks;
                       // tolerated range noise, no product script affected)
};

// Astral class-L sweep: CJK ext B-G, Tangut, Nushu (KO/JA/zh readership).
constexpr CodeRange kStrongLAstral[] = {
    {0x17000, 0x18AFF},  // Tangut + components area
    {0x1B170, 0x1B2FF},  // Nushu
    {0x20000, 0x2FFFD},  // CJK ext B/C/D/E/F (2A700..2EBE5) + Ideographic Description
    {0x30000, 0x3134A},  // CJK ext G
};

// Astral class-R: modern RTL scripts only (out of the 37-language scope;
// listed for first-strong completeness per design §2-Q1 strong-types rule).
constexpr CodeRange kStrongRAstral[] = {
    {0x10D00, 0x10D3F},  // Hanifi Rohingya
    {0x1E900, 0x1E943},  // Adlam letters (U+1E950.. formatting chars excluded)
};

// REF-3.3 (session 260910_0006 T1): the former file-local DecodeCodePoint is
// replaced by the shared emebalachat::DecodeNextCodePoint in unicode_utils.hpp.
// Byte-identical semantics, including the 0xFFFF neutral sentinel for unpaired
// surrogates (kSurrogateNeutralSentinel == kNeutralSentinel): surrogates match
// no InAny() table below, so they are skipped as neutral (mismatched halves
// carry no script).

} // namespace

bool IsRtlLanguageCode(std::string_view code) {
    if (code.empty()) return false;
    // Resolve through the 38-entry kAllLanguages registry so BOTH canonical
    // ISO codes ("AR") and English/native names ("Arabic" / design §1.3.1)
    // work: the tooltip's target_lang_ carries name_en ([tooltip.cpp L643]),
    // which needs no payload format change. Registry lookup is exact-match
    // (case-insensitive), so partial tokens like "A" or "AR " never hit.
    if (const LanguageInfo* by_code = FindLanguageByCode(code)) {
        return MatchesRtlCodeSet(by_code->code);
    }
    if (const LanguageInfo* by_name = FindLanguageByName(code)) {
        return MatchesRtlCodeSet(by_name->code);
    }
    // Unknown token: NormalizeLanguageCode yields "AUTO" (config.cpp L331) -> LTR.
    return false;
}

bool IsRtlLocale(UiLocale locale) {
    // Resolves config-code -> RTL set instead of switching on the enum: the
    // RTL UiLocale enumerators (Arabic/Persian/Urdu/Hebrew) are introduced in
    // B-3 (design §2.1.1) and this batch must not touch src/i18n.*. When B-3
    // grows LocaleToString to the 38-value table, "ar"/"fa"/"ur"/"he" land
    // here and this function becomes true for exactly those four locales BY
    // CONSTRUCTION - no edit in bidi_utils, no enum-list drift.
    // Today's 8-value enum (Auto/KO/EN/JA/zh-CN/zh-TW/VI/ES) is all-LTR.
    const std::string code = I18n::LocaleToString(locale);
    if (code.empty() || EqualsIgnoreCaseAscii(std::string_view{code}, std::string_view{"auto"})) return false;
    return MatchesRtlCodeSet(code);
}

TextDirection DirectionForLocale(const UiLocale locale) {
    return IsRtlLocale(locale) ? TextDirection::RTL : TextDirection::LTR;
}

bool IsHebrewScriptCodePoint(const unsigned int cp) {
    // G-4 (design §2.2.6, user-approved 2026-09-07): exported to smart_bypass
    // so ContainsHebrew and the first-strong scanner read the same range
    // constant (see kHebrewBlock above). Pure, no Win32.
    return cp >= kHebrewBlock.first && cp <= kHebrewBlock.last;
}

TextDirection GuessBaseDirection(const std::wstring_view text) {
    // UAX #9 P2/P3 first-strong heuristic (design §2-Q1): scan in logical
    // order; the FIRST strong-type code point decides the paragraph base
    // direction; R and AL both mean RTL; weak/neutral code points (digits
    // including AN Arabic-Indic, punctuation, symbols, whitespace, all bidi
    // controls/isolates/BOM) are skipped; no strong char -> LTR (P2 default).
    std::size_t idx = 0;
    while (idx < text.size()) {
        const unsigned int cp = DecodeNextCodePoint(text, idx);
        if (InAny(kDigitsAN, cp)) continue;
        if (InAny(kStrongR, cp)) return TextDirection::RTL;
        if (InAny(kStrongAL, cp)) return TextDirection::RTL;
        if (InAny(kStrongRAstral, cp)) return TextDirection::RTL;
        if (InAny(kStrongL, cp)) return TextDirection::LTR;
        if (InAny(kStrongLAstral, cp)) return TextDirection::LTR;
        // Neutral/weak: keep scanning.
    }
    return TextDirection::LTR;
}

} // namespace emebalachat
