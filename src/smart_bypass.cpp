#include "smart_bypass.hpp"
#include "bidi_utils.hpp"
#include "diag_logger.hpp"
#include "config.hpp"
#include "unicode_utils.hpp"

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <vector>
#include <windows.h>

namespace emebalachat {

namespace {

// REF-3.3 (session 260910_0006 T1): the former file-local DecodeNextCodePoint
// is replaced by the shared emebalachat::DecodeNextCodePoint in
// unicode_utils.hpp. Behavioral diff: unpaired surrogates now decode to the
// 0xFFFF neutral sentinel instead of the raw surrogate value. Observably
// identical here: no Is*CodePoint range below covers 0xD800-0xDFFF or 0xFFFF,
// and a built probe confirmed GetStringTypeW(CT_CTYPE1) reports C1_ALPHA=0 for
// every surrogate unit AND for 0xFFFF, so the IsLinguisticCodePoint fallback
// rejects both alike. The idx advance semantics match the old decoder exactly.

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

inline bool IsDevanagariCodePoint(uint32_t cp) {
    return (cp >= 0x0900 && cp <= 0x097F);  // Hindi
}
inline bool IsBengaliCodePoint(uint32_t cp) {
    return (cp >= 0x0980 && cp <= 0x09FF);
}
inline bool IsKhmerCodePoint(uint32_t cp) {
    return (cp >= 0x1780 && cp <= 0x17FF);
}
inline bool IsLaoCodePoint(uint32_t cp) {
    return (cp >= 0x0E80 && cp <= 0x0EFF);
}
inline bool IsMyanmarCodePoint(uint32_t cp) {
    return (cp >= 0x1000 && cp <= 0x109F);
}
inline bool IsGreekCodePoint(uint32_t cp) {
    return (cp >= 0x0370 && cp <= 0x03FF);
}

// F1 (session 260908_0003, verify 220010 root cause R1): VIETNAMESE-SPECIFIC
// codepoints only. The previous implementation also accepted the shared
// Latin-1 accented letters (0x00E0-0x00FD / 0x00C0-0x00DD: a-grave through
// y-acute etc.) - every one of which is ordinary Portuguese, French, Spanish,
// Italian or German orthography - and additionally Ũ/ũ (U+0168/U+0169), which
// Vietnamese uses but Portuguese SHARES (nasal ũ in native Portuguese words).
// Because ContainsVietnamese is a single-ANY predicate, one shared "a-grave"
// in "corazon" mislabeled the whole text Vietnamese (probe: 220010 Claim A).
// Those ranges are REMOVED here; accented Latin without a VI-specific marker
// is now script-certain but language-ambiguous and classifies as Latin-script
// AUTO content (see DetectLanguage). The remaining sets below appear ONLY in
// Vietnamese orthography among the 37 supported languages:
//   0x1EA0-0x1EF9  Latin Extended Additional precomposed VI tone marks (a-circumflex-breve, e-grave, o-horn-acute, ...)
//   U+01A0/U+01A1  O-horn (O with horn), o-horn
//   U+01AF/U+01B0  U-horn (U with horn), u-horn
//   U+0102/U+0103  A-breve, a-breve        (Romanian also writes a-breve; kept
//                                            per the F1 directive - among the
//                                            37 languages it is listed as a VI
//                                            marker, and a bare a-breve is far
//                                            rarer in the field than the VI
//                                            tone block. Residual Romanian-a-
//                                            breve misdetection is a KNOWN,
//                                            ACCEPTED limitation of the Phase-1
//                                            hybrid; Phase 2 delegates Latin
//                                            language ID to the model entirely.)
//   U+0110/U+0111  D-stroke, d-stroke      (same accepted-sharing note as
//                                            a-breve: Croatian/Slovenian also
//                                            write d-stroke.)
//   U+0168/U+0169  U-tilde DELIBERATELY EXCLUDED - shared with Portuguese.
inline bool IsVietnameseCodePoint(uint32_t cp) {
    if (cp >= 0x1EA0 && cp <= 0x1EF9) return true; // Latin Extended Additional (tone marks)
    if (cp == 0x0102 || cp == 0x0103) return true; // A-breve, a-breve
    if (cp == 0x0110 || cp == 0x0111) return true; // D-stroke, d-stroke
    if (cp == 0x01A0 || cp == 0x01A1) return true; // O-horn, o-horn
    if (cp == 0x01AF || cp == 0x01B0) return true; // U-horn, u-horn
    return false;
}

// RC-6 (v0.10.5 handoff section 2.7): the Vietnamese predicate is split in
// two. VI-EXCLUSIVE codepoints (the 1EA0 tone block plus the horns, which no
// other supported language writes) confirm Vietnamese immediately. The shared
// A-breve / D-stroke (Romanian / Croatian-Serbian orthography) no longer
// hijack a whole sentence by themselves: they count only when a VI-exclusive
// marker OR a Vietnamese stopword coexists, otherwise detection falls through
// to the Latin-script path (DetectLatinLanguage).
inline bool IsVietnameseExclusiveCodePoint(uint32_t cp) {
    return (cp >= 0x1EA0 && cp <= 0x1EF9) || // Latin Extended Additional (tone marks)
           cp == 0x01A0 || cp == 0x01A1 ||   // O-horn, o-horn
           cp == 0x01AF || cp == 0x01B0;    // U-horn, u-horn
}

inline bool IsVietnameseSharedCodePoint(uint32_t cp) {
    return cp == 0x0102 || cp == 0x0103 ||   // A-breve, a-breve (Romanian shares)
           cp == 0x0110 || cp == 0x0111;     // D-stroke, d-stroke (Croatian/Serbian share)
}

// RC-6: high-frequency Vietnamese words used as the corroborating signal for
// shared-letter-only text. Kept separate from kLatinLangs: Vietnamese is
// script-detected, not stopword-voted, and this list is only consulted when
// the text already carries a shared VI letter.
//
// R2-G-05 (261111_0001 fix-list round 2): "em" is DELIBERATELY ABSENT. It is
// ordinary Portuguese (the single most frequent word of the language), so
// keeping it here let a Portuguese sentence carrying ONE stray VI-exclusive
// letter corroborate ITSELF into "Vietnamese" - and under a Vietnamese target
// the TargetScriptDominant + already-target gate then silently skipped a real
// translation. A single exclusive marker now needs a genuinely VI-only
// function word from this list (or a second exclusive marker / two shared
// letters), so the pinned samples are unaffected: "không được" still hits
// không/được and "Xin chào bạn nhé" still hits bạn.
const wchar_t* const kVietnameseStopWords[] = {
    L"không", L"của", L"một", L"và", L"là", L"cho", L"được", L"người",
    L"những", L"trong", L"có", L"với", L"đã", L"sẽ", L"về", L"từ",
    L"này", L"khi", L"ra", L"để", L"anh", L"gì", L"sao",
    L"nào", L"rất", L"thì", L"mà", L"tôi", L"bạn", L"họ", L"chúng",
    L"vì", L"thế", L"đây", L"ấy", L"ở", L"theo", L"như", L"sau",
    L"trước", L"cũng", L"lại", L"hơn", L"rồi", L"năm", L"ngày",
};

// RC-6: does the text carry any Vietnamese stopword? Only consulted AFTER a
// shared VI letter was found, as the corroborating coexistence signal.
bool ContainsVietnameseStopWord(std::wstring_view text) {
    std::wstring token;
    const auto flush = [&]() -> bool {
        if (token.empty()) return false;
        for (size_t i = 0; i < std::size(kVietnameseStopWords); ++i) {
            if (token == kVietnameseStopWords[i]) {
                token.clear();
                return true;
            }
        }
        token.clear();
        return false;
    };
    for (wchar_t c : text) {
        if (iswalpha(c)) {
            token += towlower(c);
        } else if (flush()) {
            return true;
        }
    }
    return flush();
}

// R1-C (v0.10.7, 261111_0001 fix-list round 1): single-occurrence variant
// markers corroborate through high-frequency function words, same role as
// kVietnameseStopWords above. One marker + one stopword is real morphology;
// one stray marker inside foreign text is not. Kept small on purpose - every
// added word is a possible false corroboration for a neighboring language
// sharing the script (Arabic/Urdu share many loanwords); the 2-occurrence
// rule stays the primary signal, stopwords only rescue the 1-marker case.
// Literals are \x-escaped so the encoding gate never sees raw RTL text.
const wchar_t* const kUrduStopWords[] = {
    L"\x06C1\x06D2",       // he (hai, "is")
    L"\x06C1\x06CC\x06BA", // hain ("are")
    L"\x06A9\x0627",       // ka
    L"\x06A9\x06CC",       // ki
    L"\x06A9\x0648",       // ko
    L"\x0633\x06D2",       // se
    L"\x0646\x06C1\x06CC\x06BA", // nahin ("not")
    L"\x0627\x0648\x0631", // aur ("and")
    L"\x0645\x06CC\x06BA", // mein ("in")
    L"\x06CC\x06C1",       // yeh ("this")
};

// R2-G-05 (261111_0001 fix-list round 2): "ham" (\x0647\x0645, "also") is
// REMOVED. It is also ordinary Arabic, so one stray Persian-exclusive letter
// inside an Arabic sentence corroborated ITSELF through this word and relabeled
// the whole text Persian. Every remaining entry is Persian-only among the
// script-sharing languages (Farsi function words; the Urdu list above covers
// Urdu).
const wchar_t* const kPersianStopWords[] = {
    L"\x0627\x0633\x062A",       // ast ("is")
    L"\x0627\x06CC\x0646",       // in ("this")
    L"\x06A9\x0647",             // ke
    L"\x0631\x0627",             // ra
    L"\x0628\x0627",             // ba ("with")
    L"\x0628\x0631\x0627\x06cc", // baraye ("for")
    L"\x0646\x0647",             // na ("no")
    L"\x0645\x06CC",             // mi (verbal prefix)
    L"\x062A\x0627",             // ta ("until")
    L"\x0686\x0637\x0648\x0631", // chotor ("how") - R1-C: corroborates a lone Persian marker
};                               //   (e.g. "chotor hasti": one marker + this word -> Persian)

const wchar_t* const kUkrainianStopWords[] = {
    // R1-C (v0.10.7, 261111_0001): the U+0456 letter (Cyrillic i, also the
    // word "and") is DELIBERATELY ABSENT. It is both a Ukrainian-exclusive
    // variant marker AND a stopword, so keeping it here let a lone marker
    // corroborate ITSELF (ukrainian_marks==1 && ContainsUkrainianStopWord
    // matched the same letter), defeating the R1-C rule and relabeling Russian
    // text carrying one stray letter as "Ukrainian". Genuine Ukrainian has 2+
    // such letters (the count rule) or one of the function words below, so the
    // single-marker rescue is unaffected.
    //
    // R2-G-05 (261111_0001 fix-list round 2): "ta", "ne" and "dlya" are
    // REMOVED - they are ordinary Russian words (та/не/для), so "Это не для
    // тебя" plus one stray і corroborated the Ukrainian claim and relabeled
    // Russian text Ukrainian. Only words Russian does not use remain (або,
    // але, це, що, його, ць are all Ukrainian-only), which is the whole point
    // of the corroboration rule: a shared neighbor word is not evidence.
    L"\x0430\x0431\x043E", // abo (partial)
    L"\x0430\x043B\x0435", // ale ("but")
    L"\x0446\x0435",       // tse ("this")
    L"\x0449\x043E",       // shcho ("that")
    L"\x0457\x0433\x043E", // yoho ("his")
    L"\x0446\x044C",       // ts' (particle)
};

// Shared scanner with ContainsVietnameseStopWord: tokenizes on non-letters
// and reports whether any word matches the list. The tokenizer is deliberately
// length-agnostic (it reports one-letter tokens too - the token is non-empty at
// every boundary), so a future short function word needs no scanner change.
// (R1-C removed the only single-letter entry, the Cyrillic U+0456 stopword that
// coincided with its own variant marker.)
template <size_t N>
bool ContainsAnyStopWord(std::wstring_view text, const wchar_t* const (&words)[N]) {
    std::wstring token;
    const auto flush = [&]() -> bool {
        if (token.empty()) return false;
        for (size_t i = 0; i < N; ++i) {
            if (token == words[i]) {
                token.clear();
                return true;
            }
        }
        token.clear();
        return false;
    };
    for (wchar_t c : text) {
        if (iswalpha(c)) {
            token += towlower(c);
        } else if (flush()) {
            return true;
        }
    }
    return flush();
}

bool ContainsUrduStopWord(std::wstring_view text) {
    return ContainsAnyStopWord(text, kUrduStopWords);
}

bool ContainsPersianStopWord(std::wstring_view text) {
    return ContainsAnyStopWord(text, kPersianStopWords);
}

bool ContainsUkrainianStopWord(std::wstring_view text) {
    return ContainsAnyStopWord(text, kUkrainianStopWords);
}


// Latin SCRIPT letters - a script-family classifier, NOT a language assertion.
// F1 (session 260908_0003): widened beyond A-Za-z so accented Latin text
// (Portuguese a-tilde/o-tilde/c-cedilla, French e-acute/e-grave, Spanish
// a-acute, German u-umlaut, Turkish s-cedilla/g-breve, ...) is deterministically
// recognized as Latin-script linguistic content. Previously this was masked by
// the over-broad Vietnamese predicate inside IsLinguisticCodePoint; narrowing
// that predicate without widening this one would have pushed shared-Latin-1
// letters onto the locale-dependent GetStringTypeW fallback. Excludes the
// Latin-1 SYMBOL codepoints multiplication-sign (U+00D7) and division-sign
// (U+00F7); every other listed range is letters per the Unicode database.
inline bool IsLatinCodePoint(uint32_t cp) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
           (cp >= 0x00C0 && cp <= 0x00D6) || // Latin-1 letters A-grave .. O-diaeresis
           (cp >= 0x00D8 && cp <= 0x00F6) || // Latin-1 letters O-stroke .. o-diaeresis
           (cp >= 0x00F8 && cp <= 0x00FF) || // Latin-1 letters o-stroke .. y-diaeresis
           (cp >= 0x0100 && cp <= 0x024F) || // Latin Extended-A/B (A-macron .. w-yogh: includes a-breve, d-stroke, uhorn, schwa, s-cedilla, g-breve, i-dotless, l-stroke, r-cedilla ...)
           (cp >= 0x1E00 && cp <= 0x1EFF);   // Latin Extended Additional (A-dota-below .. y-grave; VI tone marks are the 1EA0-1EF9 subset)
}

inline bool IsLinguisticCodePoint(uint32_t cp) {
    if (IsKoreanCodePoint(cp) || IsKanaCodePoint(cp) || IsHanziCodePoint(cp) ||
        IsThaiCodePoint(cp) || IsArabicCodePoint(cp) || IsCyrillicCodePoint(cp) ||
        IsVietnameseCodePoint(cp) || IsLatinCodePoint(cp) ||
        IsDevanagariCodePoint(cp) || IsBengaliCodePoint(cp) ||
        IsKhmerCodePoint(cp) || IsLaoCodePoint(cp) ||
        IsMyanmarCodePoint(cp) || IsGreekCodePoint(cp) ||
        // G-4 (design §2.2.6, user-approved 2026-09-07): Hebrew letters are
        // explicit linguistic content via the shared bidi_utils predicate,
        // mirroring the Arabic line above instead of relying on the
        // GetStringTypeW C1_ALPHA fallback below (locale-dependent).
        IsHebrewScriptCodePoint(cp)) {
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

// REF-3.1 (session 260910_0006 T1): the former file-local CaseInsensitiveEqual
// is replaced at every call site by the shared EqualsIgnoreCaseAscii<char>
// template in unicode_utils.hpp. Equivalent under this project's default "C"
// CRT locale (no setlocale call exists in src/): std::tolower maps only
// 'A'-'Z' there, and the +32 fold here covers exactly that set; high bytes
// pass through both implementations verbatim.

// REF-3.2 (session 260910_0006 T1): single scan driver for the 16 script
// predicates below. Loop body is character-for-character the old hand-unrolled
// boilerplate (shared decoder, same idx advance, early-return on first hit).
template <typename Predicate>
bool ContainsCodePointMatching(std::wstring_view text, Predicate pred) {
    size_t idx = 0;
    while (idx < text.size()) {
        uint32_t cp = DecodeNextCodePoint(text, idx);
        if (pred(cp)) {
            return true;
        }
    }
    return false;
}

// R1-C (v0.10.7, 261111_0001 fix-list round 1): OCCURRENCE-count twin of the
// single-ANY scan above, for the sub-language corroboration rules (one
// exclusive marker amid foreign text is not a language claim; several are).
template <typename Predicate>
size_t CountCodePointsMatching(std::wstring_view text, Predicate pred) {
    size_t count = 0;
    size_t idx = 0;
    while (idx < text.size()) {
        const uint32_t cp = DecodeNextCodePoint(text, idx);
        if (pred(cp)) {
            ++count;
        }
    }
    return count;
}

} // namespace

bool ContainsKorean(std::wstring_view text) {
    return ContainsCodePointMatching(text, IsKoreanCodePoint);
}

bool ContainsKana(std::wstring_view text) {
    return ContainsCodePointMatching(text, IsKanaCodePoint);
}

bool ContainsHanzi(std::wstring_view text) {
    return ContainsCodePointMatching(text, IsHanziCodePoint);
}

bool ContainsCyrillic(std::wstring_view text) {
    return ContainsCodePointMatching(text, IsCyrillicCodePoint);
}

bool ContainsArabic(std::wstring_view text) {
    return ContainsCodePointMatching(text, IsArabicCodePoint);
}

bool ContainsHebrew(std::wstring_view text) {
    // G-4 (REQ-040, design §2.2.6, user-approved 2026-09-07): Hebrew-script
    // detection, same scan pattern as ContainsArabic. The U+0590-U+05FF range
    // lives in bidi_utils (IsHebrewScriptCodePoint) so the translation
    // trigger and the REQ-038 first-strong scanner share one source of truth.
    return ContainsCodePointMatching(text, IsHebrewScriptCodePoint);
}

bool ContainsThai(std::wstring_view text) {
    return ContainsCodePointMatching(text, IsThaiCodePoint);
}

bool ContainsVietnamese(std::wstring_view text) {
    // RC-6 (v0.10.5 handoff section 2.7): VI-exclusive markers confirm
    // Vietnamese immediately; the shared A-breve/D-stroke only count with a
    // VI-exclusive marker or a Vietnamese stopword coexisting. Shared-only
    // text (Romanian "casă", Croatian "đak") falls through to the Latin path.
    //
    // R1-C (v0.10.7, 261111_0001 fix-list round 1): an EXCLUSIVE marker alone
    // no longer confirms Vietnamese. One U+01A1 inside an otherwise-English
    // sentence ("... with one mark: ơ") used to flip the whole label to
    // "Vietnamese" (ContainsVietnamese runs BEFORE DetectLatinLanguage in
    // LabelForFamilyAndDetectLanguageByScriptPriority), and under a
    // Vietnamese target TargetScriptDominant + the already-target check
    // then silently skipped the translation. A sub-language claim now needs
    // CORROBORATION: at least one exclusive marker AND at least two
    // shared-language markers (or a Vietnamese stopword). The pinned
    // "Đừng ăn đó" sample (tests/run_tests.cpp:981) carries 1 exclusive (ừ)
    // + 3 shared (Đ, ă, đ) and stays Vietnamese; a lone exclusive in
    // Latin-dominant text falls through to the Latin decision.
    const size_t exclusive = CountCodePointsMatching(text, IsVietnameseExclusiveCodePoint);
    if (exclusive == 0) {
        // No exclusive marker: shared letters alone never claim Vietnamese
        // (RC-6, unchanged) - but a stopword still corroborates shared
        // letters exactly as before.
        if (!ContainsCodePointMatching(text, IsVietnameseSharedCodePoint)) {
            return false;
        }
        return ContainsVietnameseStopWord(text);
    }
    if (exclusive >= 2) {
        // Two independent tone-marked letters are real Vietnamese
        // morphology ("Xin chào thế giới": ế + ộ); the defect class is a
        // SINGLE stray marker, not two.
        return true;
    }
    const size_t shared = CountCodePointsMatching(text, IsVietnameseSharedCodePoint);
    if (shared >= 2) {
        return true;
    }
    // A single exclusive marker needs a second signal: two shared letters
    // ("Đừng ăn đó": ừ + Đ, ă, đ) or a Vietnamese stopword. A lone marker in
    // otherwise-foreign text falls through to the Latin decision.
    return ContainsVietnameseStopWord(text);
}

bool ContainsLatin(std::wstring_view text) {
    return ContainsCodePointMatching(text, IsLatinCodePoint);
}

bool ContainsDevanagari(std::wstring_view text) {
    return ContainsCodePointMatching(text, IsDevanagariCodePoint);
}

bool ContainsBengali(std::wstring_view text) {
    return ContainsCodePointMatching(text, IsBengaliCodePoint);
}

bool ContainsKhmer(std::wstring_view text) {
    return ContainsCodePointMatching(text, IsKhmerCodePoint);
}

bool ContainsLao(std::wstring_view text) {
    return ContainsCodePointMatching(text, IsLaoCodePoint);
}

bool ContainsMyanmar(std::wstring_view text) {
    return ContainsCodePointMatching(text, IsMyanmarCodePoint);
}

bool ContainsGreek(std::wstring_view text) {
    return ContainsCodePointMatching(text, IsGreekCodePoint);
}

bool HasLinguisticContent(std::wstring_view text) {
    return ContainsCodePointMatching(text, IsLinguisticCodePoint);
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

namespace {

struct LangStopWords {
    const char* lang_name;      // e.g. "English", "Spanish", etc.
    const wchar_t* const* words;
    size_t count;
    int min_hits;
};

const wchar_t* const kStopWordsEN[] = { L"the", L"is", L"are", L"was", L"were", L"this", L"that", L"have", L"has", L"with", L"for", L"you", L"and", L"not", L"but", L"from", L"they", L"will", L"would", L"been", L"can" };
const wchar_t* const kStopWordsES[] = { L"el", L"la", L"los", L"las", L"de", L"en", L"que", L"es", L"un", L"una", L"por", L"con", L"para", L"del", L"se", L"al", L"como", L"más", L"pero", L"su" };
const wchar_t* const kStopWordsFR[] = { L"le", L"la", L"les", L"des", L"est", L"une", L"dans", L"pour", L"que", L"sur", L"pas", L"avec", L"son", L"qui", L"ont", L"aux", L"ces", L"par", L"cette", L"tout" };
const wchar_t* const kStopWordsDE[] = { L"der", L"die", L"das", L"und", L"ist", L"ein", L"eine", L"den", L"dem", L"auf", L"für", L"nicht", L"mit", L"sich", L"des", L"von", L"auch", L"noch", L"wie" };
const wchar_t* const kStopWordsPT[] = { L"o", L"a", L"os", L"as", L"e", L"em", L"na", L"no", L"do", L"da", L"que", L"de", L"não", L"um", L"uma", L"para", L"com", L"por", L"mais", L"como", L"dos", L"das", L"seu", L"sua", L"foi", L"são", L"tem", L"nos", L"essa" };
const wchar_t* const kStopWordsIT[] = { L"che", L"non", L"una", L"del", L"per", L"con", L"sono", L"della", L"anche", L"come", L"dei", L"gli", L"alla", L"questo", L"quella", L"molto", L"suo", L"sua" };
const wchar_t* const kStopWordsID[] = { L"yang", L"dan", L"ini", L"itu", L"untuk", L"dengan", L"tidak", L"dari", L"pada", L"akan", L"saya", L"bisa", L"ada", L"sudah", L"juga", L"mereka", L"kami", L"kita" };
const wchar_t* const kStopWordsMS[] = { L"yang", L"dan", L"ini", L"itu", L"untuk", L"dengan", L"tidak", L"dari", L"pada", L"akan", L"kami", L"juga", L"mereka", L"telah", L"boleh", L"ada", L"oleh" };
const wchar_t* const kStopWordsFIL[] = { L"ang", L"mga", L"sa", L"na", L"ng", L"at", L"ay", L"ko", L"si", L"ni", L"hindi", L"ito", L"para", L"nang", L"ako", L"siya", L"nila", L"kami", L"namin" };
const wchar_t* const kStopWordsTR[] = { L"bir", L"ve", L"bu", L"için", L"ile", L"olan", L"gibi", L"daha", L"çok", L"ama", L"kadar", L"ben", L"benim", L"onun", L"bize", L"şey", L"olarak" };
const wchar_t* const kStopWordsPL[] = { L"nie", L"się", L"jest", L"jak", L"ale", L"lub", L"czy", L"dla", L"już", L"był", L"być", L"tak", L"też", L"przez", L"tylko", L"ich", L"jego", L"jej" };
const wchar_t* const kStopWordsNL[] = { L"het", L"een", L"van", L"dat", L"met", L"voor", L"zijn", L"aan", L"ook", L"maar", L"nog", L"werd", L"wel", L"hun", L"naar", L"uit", L"bij", L"kan", L"deze" };
const wchar_t* const kStopWordsCS[] = { L"není", L"jsem", L"jsou", L"jako", L"aby", L"jeho", L"její", L"také", L"nebo", L"byl", L"být", L"jen", L"tak", L"ale", L"než", L"které", L"které" };
const wchar_t* const kStopWordsHU[] = { L"nem", L"egy", L"hogy", L"meg", L"van", L"már", L"csak", L"vagy", L"még", L"mint", L"sem", L"igen", L"volt", L"lett", L"lesz", L"azt", L"ezt" };
const wchar_t* const kStopWordsSV[] = { L"och", L"att", L"det", L"som", L"för", L"med", L"den", L"var", L"har", L"men", L"inte", L"kan", L"ska", L"ett", L"också", L"från", L"eller", L"vid" };
const wchar_t* const kStopWordsRO[] = { L"este", L"sunt", L"care", L"din", L"pentru", L"sau", L"acest", L"această", L"prin", L"fost", L"mai", L"doar", L"între", L"poate", L"dacă", L"cea" };
const wchar_t* const kStopWordsDA[] = { L"og", L"den", L"det", L"til", L"for", L"med", L"som", L"har", L"kan", L"ved", L"skal", L"alle", L"ikke", L"hun", L"han", L"var", L"der", L"fra" };
const wchar_t* const kStopWordsFI[] = { L"ja", L"on", L"oli", L"tai", L"kun", L"niin", L"olen", L"olet", L"mutta", L"eikä", L"ovat", L"joka", L"siitä", L"tämä", L"myös", L"kuin", L"nyt", L"vain" };
const wchar_t* const kStopWordsNO[] = { L"og", L"det", L"som", L"for", L"med", L"har", L"kan", L"til", L"den", L"fra", L"var", L"men", L"han", L"hun", L"alle", L"ikke", L"eller", L"meg", L"seg" };

const LangStopWords kLatinLangs[] = {
    // N4 (session 260910_0001): English confidence raised 2 -> 3. Two stopword
    // hits proved too loose in the field (emebalachat_260910063910.log /
    // _260910073906.log): weak-English reads that also collide with another
    // ASCII-Latin language tie at 2-2, fall back to "Auto Detect", and spend a
    // pointless EN->EN identity inference when the target is English. With 3+
    // distinct-occurrence hits the read is high-confidence English and the
    // already-target bypass (ShouldTranslate step 5) fires instead; 2-hit text
    // now routes as AUTO so Hy-MT2's own language ID decides. 3 keeps the
    // Indonesian/Malay/Filipino false-bypass risk low (F5 audit 181530:
    // pure-ASCII Latin can never be hard-asserted as English).
    { "English", kStopWordsEN, std::size(kStopWordsEN), 3 },
    { "Spanish", kStopWordsES, std::size(kStopWordsES), 2 },
    { "French", kStopWordsFR, std::size(kStopWordsFR), 2 },
    { "German", kStopWordsDE, std::size(kStopWordsDE), 2 },
    { "Portuguese", kStopWordsPT, std::size(kStopWordsPT), 2 },
    { "Italian", kStopWordsIT, std::size(kStopWordsIT), 2 },
    { "Indonesian", kStopWordsID, std::size(kStopWordsID), 2 },
    { "Malay", kStopWordsMS, std::size(kStopWordsMS), 2 },
    { "Filipino", kStopWordsFIL, std::size(kStopWordsFIL), 2 },
    { "Turkish", kStopWordsTR, std::size(kStopWordsTR), 2 },
    { "Polish", kStopWordsPL, std::size(kStopWordsPL), 2 },
    { "Dutch", kStopWordsNL, std::size(kStopWordsNL), 2 },
    { "Czech", kStopWordsCS, std::size(kStopWordsCS), 2 },
    { "Hungarian", kStopWordsHU, std::size(kStopWordsHU), 2 },
    { "Swedish", kStopWordsSV, std::size(kStopWordsSV), 2 },
    { "Romanian", kStopWordsRO, std::size(kStopWordsRO), 2 },
    { "Danish", kStopWordsDA, std::size(kStopWordsDA), 2 },
    { "Finnish", kStopWordsFI, std::size(kStopWordsFI), 2 },
    { "Norwegian", kStopWordsNO, std::size(kStopWordsNO), 2 }
};

// RC-1 (v0.10.5 handoff section 2.2): high-frequency English core vocabulary.
// A 1-2 token Latin input matching one of these (or an English stopword) is
// high-confidence English, so single words like "hello" / "good morning" no
// longer collapse into the AUTO marker (which skipped the already-target
// bypass and, after a rung-1 echo, fed raw completion to the chat template).
const wchar_t* const kEnglishCoreWords[] = {
    L"hello", L"world", L"thanks", L"thank", L"yes", L"no", L"ok", L"okay",
    L"good", L"morning", L"please", L"sorry", L"help", L"test", L"update",
    L"file", L"error", L"hi", L"hey", L"bye", L"goodbye", L"welcome",
    L"night", L"evening", L"afternoon", L"name", L"love", L"time", L"day",
    L"today", L"tomorrow", L"yesterday", L"work", L"home", L"water",
    L"friend", L"family", L"happy"
};

// RC-1: non-English distinctive diacritics - letters that (among the 37
// supported languages) appear in exactly one language's orthography, so a
// 1-2 token word carrying one is immediately classifiable. Shared letters
// (á, é, ü, ç, ...) are deliberately ABSENT: they stay the AUTO marker (F1).
struct DistinctiveDiacriticEntry {
    uint32_t cp;
    const char* lang_name;
};

const DistinctiveDiacriticEntry kDistinctiveDiacritics[] = {
    {0x00F1, "Spanish"},              // ñ
    {0x00DF, "German"},               // ß
    {0x0153, "French"},               // œ
    {0x00E3, "Portuguese"},           // ã
    {0x00F5, "Portuguese"},           // õ
    {0x011F, "Turkish"},              // ğ
    {0x0131, "Turkish"},              // dotless ı
    {0x0105, "Polish"},               // ą
    {0x0119, "Polish"},               // ę
    {0x0142, "Polish"},               // ł
    {0x0144, "Polish"},               // ń
    {0x015B, "Polish"},               // ś
    {0x017A, "Polish"},               // ź
    {0x017C, "Polish"},               // ż
    {0x016F, "Czech"},                // ů
    {0x011B, "Czech"},                // ě
    {0x0159, "Czech"},                // ř
    {0x0165, "Czech"},                // ť
    {0x010F, "Czech"},                // ď
    {0x0103, "Romanian"},             // ă
    {0x00EE, "Romanian"},             // î
    {0x0219, "Romanian"},             // ș (comma-below; Romanian-only)
    {0x021B, "Romanian"},             // ț (comma-below; Romanian-only)
    {0x0163, "Romanian"},             // ţ
    {0x0151, "Hungarian"},            // ő
    {0x0171, "Hungarian"},            // ű
};
// F-13 (v0.10.5 review round 1): deliberately ABSENT from the table - ù
// (French AND Italian "piu'"/"giu'") and ş (Romanian legacy AND Turkish) are
// shared letters, so a 1-2 token Italian/Turkish word would be mislabeled;
// shared letters stay the AUTO marker (F1).

bool TokenInWordList(std::wstring_view token, const wchar_t* const* words, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (token == words[i]) {
            return true;
        }
    }
    return false;
}

// v0.10.7 (session 261111_0001 U2, CEO language-fairness requirement): the
// WS1 (261108_0002) "ASCII word shape" fallback is replaced by an explicit
// English positive-evidence lexicon. The retired helper
// (IsAsciiEnglishShapedToken) resolved ANY pure-ASCII word of >= 3 letters
// carrying a vowel to "English", so a French/Spanish/Vietnamese/Indonesian
// single word ("bonjour", "informazione") read as English and ShouldTranslate
// then SILENTLY skipped the translation toward an English target. Letter
// shape cannot decide word identity, so a single token now resolves English
// only when the token itself is known English; every other single word keeps
// the AUTO marker and the engine's built-in language ID decides.
//
// Membership rules (keep this list SMALL - every added word is a possible
// false-English read that silently skips a real translation):
//   * the WS1 reported single words, whose English read is a pinned contract
//     (tests/run_tests.cpp and tests/ws1_singleword_tests.inc pin them);
//   * "hotel", the RC-1 loanword pin that flipped to English in WS1.
// English function words, stopwords and the frequent core vocabulary are
// handled EARLIER by kStopWordsEN / kEnglishCoreWords (precedence unchanged),
// and a distinctive non-English diacritic still wins over this list.
//
// SCOPE: a SINGLE token only, deliberately. A 2-token ASCII-Latin phrase
// cannot be told apart from Swahili/Tagalog/Indonesian ("Sisitiki sana",
// "Hindi ko"), and the F5 adjudication (audit 181530 Option B) forbids
// re-opening the silent untranslated passthrough for exactly those users -
// ShouldTranslate must keep translating them toward an English target.
const wchar_t* const kEnglishSingleWordEvidence[] = {
    L"cyber", L"company", L"astrology", L"clue", L"nature", L"medicine",
    L"hotel",
};

const char* DistinctiveDiacriticLanguage(std::wstring_view token) {
    for (wchar_t c : token) {
        for (const DistinctiveDiacriticEntry& entry : kDistinctiveDiacritics) {
            if (c == entry.cp) {
                return entry.lang_name;
            }
        }
    }
    return nullptr;
}

std::string DetectLatinLanguage(std::wstring_view text) {
    std::vector<std::wstring> tokens;
    std::wstring current_token;
    for (wchar_t c : text) {
        if (iswalpha(c)) {
            current_token += towlower(c);
        } else if (!current_token.empty()) {
            tokens.push_back(current_token);
            current_token.clear();
        }
    }
    if (!current_token.empty()) {
        tokens.push_back(current_token);
    }

    if (tokens.size() < 3) {
        // RC-1 (v0.10.5 handoff section 2.2): the <3-token floor used to force
        // every single word ("hello", "apple", "good morning") into the AUTO
        // marker. Resolve 1-2 token inputs immediately instead: a non-English
        // distinctive diacritic, an English core-vocabulary/stopword hit
        // (high-confidence English), or a single non-English stopword hit
        // (ties stay ambiguous). Anything else keeps the AUTO marker.
        //
        // R1-C (v0.10.7, 261111_0001 fix-list round 1): the distinctive-
        // diacritic claim is corroborated the only way a 1-2 token sample
        // allows. The RC-1/WS1 contract (pinned in tests/run_tests.cpp:941
        // "señor" -> Spanish and :1002 "Liebe Grüße" -> German, plus
        // tests/ws1_singleword_tests.inc) keeps a lone distinctive diacritic
        // SUFFICIENT for 1-2 token words - a content-blind sample that small
        // offers no second lexical signal, and that adjudication is not
        // re-opened here. What IS new: the claim is REFUSED on
        // COUNTER-EVIDENCE - (a) a second, DIFFERENT language's distinctive
        // diacritic in the same sample (mixed markers = ambiguous), (b) an
        // English CORE-VOCABULARY word (G-04 below), or (c) another Latin
        // language's stopword hit (lexical evidence for a neighbor). A
        // refused claim keeps the AUTO marker so the engine's built-in
        // language ID decides, exactly like the other corroboration
        // fallbacks in this file.
        const char* diacritic_lang = nullptr;
        bool mixed_diacritics = false;
        for (const auto& token : tokens) {
            if (const char* lang = DistinctiveDiacriticLanguage(token)) {
                if (diacritic_lang && !EqualsIgnoreCaseAscii(
                        std::string_view{diacritic_lang}, std::string_view{lang})) {
                    mixed_diacritics = true;
                }
                if (!diacritic_lang) {
                    diacritic_lang = lang;
                }
            }
        }
        if (diacritic_lang) {
            // G-04 (261111_0001 fix-list round 2): the counter-evidence scan
            // consults the English CORE VOCABULARY as well as kLatinLangs
            // stopwords. "hello" / "thanks" / "good" are in neither
            // kStopWordsEN nor any other word list here, so "hello senor"
            // (hello + the Spanish n-tilde) used to resolve "Spanish" - and
            // under a Spanish target the already-target gate then silently
            // skipped an English speaker's request (no translation, no
            // message). A high-frequency English core word beside a diacritic
            // token is positive evidence for a DIFFERENT language, so it
            // blocks the diacritic's claim symmetrically with a neighbor
            // stopword; the engine's language ID then decides.
            const char* counter_reason = nullptr;
            for (const auto& token : tokens) {
                if (TokenInWordList(token, kEnglishCoreWords,
                                    std::size(kEnglishCoreWords))) {
                    counter_reason = "counter-evidence English core word";
                    break;
                }
                for (const auto& lang : kLatinLangs) {
                    if (!EqualsIgnoreCaseAscii(std::string_view{lang.lang_name},
                                               std::string_view{diacritic_lang}) &&
                        TokenInWordList(token, lang.words, lang.count)) {
                        counter_reason = "counter-evidence stopword";
                        break;
                    }
                }
                if (counter_reason) break;
            }
            if (!mixed_diacritics && counter_reason == nullptr) {
                DIAG_F("SMART_BYPASS/DetectLatinLanguage: 1-2 token path detected=%s (distinctive diacritic)\n", diacritic_lang);
                return diacritic_lang;
            }
            DIAG_F("SMART_BYPASS/DetectLatinLanguage: 1-2 token diacritic %s uncorroborated (%s); AUTO marker\n",
                   diacritic_lang, mixed_diacritics ? "mixed diacritics" : counter_reason);
            return "Auto Detect";
        }
        for (const auto& token : tokens) {
            if (TokenInWordList(token, kEnglishCoreWords, std::size(kEnglishCoreWords)) ||
                TokenInWordList(token, kStopWordsEN, std::size(kStopWordsEN))) {
                DIAG_F("SMART_BYPASS/DetectLatinLanguage: 1-2 token path detected=English\n");
                return "English";
            }
        }
        int best_hits = 0;
        const char* best_lang = nullptr;
        bool tie = false;
        for (const auto& lang : kLatinLangs) {
            if (EqualsIgnoreCaseAscii(std::string_view{lang.lang_name}, std::string_view{"English"})) {
                continue; // English resolved above
            }
            int hits = 0;
            for (const auto& token : tokens) {
                if (TokenInWordList(token, lang.words, lang.count)) {
                    ++hits;
                }
            }
            if (hits >= 1) {
                if (hits > best_hits) {
                    best_hits = hits;
                    best_lang = lang.lang_name;
                    tie = false;
                } else if (hits == best_hits) {
                    tie = true;
                }
            }
        }
        if (best_lang && !tie) {
            DIAG_F("SMART_BYPASS/DetectLatinLanguage: 1-2 token path detected=%s with %d hits\n", best_lang, best_hits);
            return best_lang;
        }
        // WS1 (261108_0002): English resolution for the single words that
        // reach here - the reported words (cyber, company, astrology, clue,
        // nature, medicine) are in no core-vocabulary or stopword list, so
        // they used to fall through to the AUTO marker. That marker is what
        // routed them into the AUTO-source prompt branch, where the RC-3
        // brand fences primed the hallucinations WS1 removed. The evidence is
        // now LEXICAL (kEnglishSingleWordEvidence above), not shape-based:
        // v0.10.7 (261111_0001 U2) removed the "any ASCII word with a vowel is
        // English" rule because it silently skipped French/Spanish/
        // Vietnamese/Indonesian single words toward an English target.
        //
        // SINGLE token only: a 2-token ASCII-Latin phrase stays AUTO so the F5
        // adjudication keeps translating Swahili/Tagalog/Indonesian toward
        // English instead of silently bypassing them as "already English".
        if (tokens.size() == 1 &&
            TokenInWordList(tokens.front(), kEnglishSingleWordEvidence,
                            std::size(kEnglishSingleWordEvidence))) {
            DIAG_F("SMART_BYPASS/DetectLatinLanguage: 1-2 token path detected=English (single-token English evidence)\n");
            return "English";
        }
        return "Auto Detect";
    }

    int best_hits = 0;
    const char* best_lang = nullptr;
    bool tie = false;

    for (const auto& lang : kLatinLangs) {
        int hits = 0;
        for (const auto& token : tokens) {
            for (size_t i = 0; i < lang.count; ++i) {
                if (token == lang.words[i]) {
                    hits++;
                    break;
                }
            }
        }
        
        if (hits > 0 && hits >= lang.min_hits) {
            if (hits > best_hits) {
                best_hits = hits;
                best_lang = lang.lang_name;
                tie = false;
            } else if (hits == best_hits) {
                tie = true;
            }
        }
    }

    if (best_lang && !tie) {
        DIAG_F("SMART_BYPASS/DetectLatinLanguage: detected=%s with %d hits\n", best_lang, best_hits);
        return best_lang;
    }

    return "Auto Detect";
}

} // namespace

// F5 (session 260908_0003, ask audit 181530 condition 1 Option B): the F1
// helper ContainsDiacriticLatin is REMOVED with the supersession of the
// pure-ASCII "English" label (see DetectLanguage below) - diacritic and
// pure-ASCII Latin now share one policy (AUTO), so the distinction no longer
// gates anything.

namespace {

// ---------------------------------------------------------------------------
// v0.10.7 (session 261111_0001 U2, CEO language-fairness requirement):
// DOMINANT-SCRIPT model for DetectLanguage.
//
// The previous decision was an ANY-CONTAINS priority cascade: ContainsKorean
// ran first, so ONE Hangul syllable anywhere in an otherwise English sentence
// ("The configuration file is corrupted. 한") labeled the whole selection
// "Korean". Under a Korean target that became a Korean->Korean request - the
// typing prompt read "Translate the following Korean text into Korean" - and
// on the drag path a spurious ResolveEffectiveTarget pivot; the sentence was
// never translated correctly. Every other branch carried the same bias one
// script down (a single Thai/Arabic/Cyrillic character hijacked the label).
//
// The model below counts LETTERS per script family over the trimmed text
// (spaces, punctuation, digits and symbols carry no language signal and are
// skipped) and lets a family own the label only when it holds an absolute
// majority of the letters AND clearly leads the runner-up. Two documented
// escape hatches keep every existing pure-script outcome byte-identical:
//   * a near-tie (the runner-up within 10% of the leader), and
//   * no clear majority (the leader holds at most 50% of the letters)
// both fall through to DetectLanguageByScriptPriority, the historical chain.
// ---------------------------------------------------------------------------
enum class ScriptFamily {
    Korean = 0,
    Japanese,   // Hiragana / Katakana (folds Hanzi in when kana is present)
    Chinese,    // Hanzi without any kana
    Thai,
    Arabic,
    Hebrew,
    Cyrillic,
    Devanagari,
    Bengali,
    Khmer,
    Lao,
    Myanmar,
    Greek,
    Latin,
    Other,      // other alphabetic code points (Armenian, Georgian, ...)
    Count
};

constexpr size_t kScriptFamilyCount = static_cast<size_t>(ScriptFamily::Count);

struct ScriptTally {
    size_t by_family[kScriptFamilyCount] = {};
    size_t letters = 0;
};

ScriptFamily ScriptFamilyOfCodePoint(uint32_t cp) {
    if (IsKoreanCodePoint(cp)) return ScriptFamily::Korean;
    if (IsKanaCodePoint(cp)) return ScriptFamily::Japanese;
    if (IsHanziCodePoint(cp)) return ScriptFamily::Chinese;
    if (IsThaiCodePoint(cp)) return ScriptFamily::Thai;
    if (IsArabicCodePoint(cp)) return ScriptFamily::Arabic;
    if (IsHebrewScriptCodePoint(cp)) return ScriptFamily::Hebrew;
    if (IsCyrillicCodePoint(cp)) return ScriptFamily::Cyrillic;
    if (IsDevanagariCodePoint(cp)) return ScriptFamily::Devanagari;
    if (IsBengaliCodePoint(cp)) return ScriptFamily::Bengali;
    if (IsKhmerCodePoint(cp)) return ScriptFamily::Khmer;
    if (IsLaoCodePoint(cp)) return ScriptFamily::Lao;
    if (IsMyanmarCodePoint(cp)) return ScriptFamily::Myanmar;
    if (IsGreekCodePoint(cp)) return ScriptFamily::Greek;
    if (IsLatinCodePoint(cp)) return ScriptFamily::Latin;
    return ScriptFamily::Other;
}

ScriptTally TallyScriptFamilies(std::wstring_view text) {
    ScriptTally tally;
    size_t idx = 0;
    while (idx < text.size()) {
        const uint32_t cp = DecodeNextCodePoint(text, idx);
        if (!IsLinguisticCodePoint(cp)) {
            continue;
        }
        ++tally.letters;
        ++tally.by_family[static_cast<size_t>(ScriptFamilyOfCodePoint(cp))];
    }
    // Kana is written by Japanese alone (no other supported language shares
    // it), so the documented "kana takes precedence over Hanzi" rule survives
    // the ratio model: a text carrying any kana is measured as one Japanese
    // family, otherwise a kanji-heavy Japanese sentence would read Chinese.
    const size_t japanese = static_cast<size_t>(ScriptFamily::Japanese);
    const size_t chinese = static_cast<size_t>(ScriptFamily::Chinese);
    if (tally.by_family[japanese] > 0) {
        tally.by_family[japanese] += tally.by_family[chinese];
        tally.by_family[chinese] = 0;
    }
    return tally;
}

// Fills `out_family` and returns true only for a CLEAR winner; false means
// "mixed, plural, or unclassifiable - keep the historical priority order".
bool TryDominantScriptFamily(const ScriptTally& tally, ScriptFamily& out_family) {
    size_t best = 0;
    size_t second = 0;
    ScriptFamily best_family = ScriptFamily::Count;
    for (size_t i = 0; i < kScriptFamilyCount; ++i) {
        const size_t count = tally.by_family[i];
        if (count > best) {
            second = best;
            best = count;
            best_family = static_cast<ScriptFamily>(i);
        } else if (count > second) {
            second = count;
        }
    }
    if (tally.letters == 0 || best == 0 || best_family == ScriptFamily::Count) {
        return false;
    }
    if (best_family == ScriptFamily::Other) {
        // Nothing this table can name (Armenian/Georgian/...): the priority
        // chain below keeps its "Unknown" answer.
        return false;
    }
    if (best * 2 <= tally.letters) {
        return false; // plurality, not a majority
    }
    if (second * 10 >= best * 9) {
        return false; // near-tie: the runner-up is within 10% of the leader
    }
    out_family = best_family;
    return true;
}

// Arabic script serves three supported languages; these variant letters are
// the documented disambiguation, moved here verbatim from the old chain so the
// dominant branch and the fallback chain cannot drift.
//
// R1-C (v0.10.7, 261111_0001 fix-list round 1): a SINGLE variant letter no
// longer flips the sub-language label. One Urdu-exclusive letter inside an
// otherwise-Arabic sentence used to relabel the whole text "Urdu" (and one
// Persian letter "Persian"), so under an Urdu/Persian target the
// TargetScriptDominant + already-target gates silently skipped a genuine
// Arabic->Urdu translation (Arabic and Urdu share the script family, so the
// 70%-gate always passed). Variant claims now need CORROBORATION: at least
// two variant-letter occurrences, or one occurrence plus a language stopword.
// Pure Arabic (no variant letter) keeps the exact "Arabic" answer.
std::string DetectArabicVariant(std::wstring_view text) {
    size_t urdu_marks = 0;
    size_t persian_marks = 0;
    size_t idx = 0;
    while (idx < text.size()) {
        const uint32_t cp = DecodeNextCodePoint(text, idx);
        if (cp == 0x0679 || cp == 0x0688 || cp == 0x0691 || cp == 0x06BA || cp == 0x06D2 || cp == 0x06BE) {
            ++urdu_marks;
        } else if (cp == 0x067E || cp == 0x0686 || cp == 0x0698 || cp == 0x06AF) {
            ++persian_marks;
        }
    }
    if (urdu_marks >= 2) return "Urdu";
    if (persian_marks >= 2) return "Persian";
    if (urdu_marks == 1 && ContainsUrduStopWord(text)) return "Urdu";
    if (persian_marks == 1 && ContainsPersianStopWord(text)) return "Persian";
    return "Arabic";
}

// Cyrillic script serves Russian and Ukrainian; the four variant letters are
// the documented disambiguation (also moved verbatim).
//
// R1-C corroboration (same rule as DetectArabicVariant above): one
// Ukrainian-exclusive letter (ґ, є, і, ї) inside Russian text used to
// relabel the WHOLE selection "Ukrainian" - a stray і in Russian prose is
// common (a Latin i typed on the wrong layout lands on the same key). Two
// occurrences, or one plus a Ukrainian stopword, are required now; pure
// Russian keeps the exact "Russian" answer.
std::string DetectCyrillicVariant(std::wstring_view text) {
    const size_t ukrainian_marks = CountCodePointsMatching(text, [](uint32_t cp) {
        return cp == 0x0491 || cp == 0x0454 || cp == 0x0456 || cp == 0x0457;
    });
    if (ukrainian_marks >= 2) return "Ukrainian";
    if (ukrainian_marks == 1 && ContainsUkrainianStopWord(text)) return "Ukrainian";
    return "Russian";
}

// Script family -> language label. Empty means "no label from this family";
// DetectLanguage then re-runs the priority chain (never reached for a family
// TryDominantScriptFamily accepted, kept as a total-function guard).
std::string LabelForScriptFamily(ScriptFamily family, std::wstring_view text) {
    switch (family) {
        case ScriptFamily::Korean: return "Korean";
        case ScriptFamily::Japanese: return "Japanese";
        case ScriptFamily::Chinese: return "Chinese Simplified";
        case ScriptFamily::Thai: return "Thai";
        case ScriptFamily::Arabic: return DetectArabicVariant(text);
        case ScriptFamily::Hebrew: return "Hebrew";
        case ScriptFamily::Cyrillic: return DetectCyrillicVariant(text);
        case ScriptFamily::Devanagari: return "Hindi";
        case ScriptFamily::Bengali: return "Bengali";
        case ScriptFamily::Khmer: return "Khmer";
        case ScriptFamily::Lao: return "Lao";
        case ScriptFamily::Myanmar: return "Burmese";
        case ScriptFamily::Greek: return "Greek";
        case ScriptFamily::Latin:
            // Vietnamese shares the Latin script and is confirmed by its
            // VI-exclusive code points (RC-6 rule, unchanged); every other
            // Latin text goes through the Latin language path.
            if (ContainsVietnamese(text)) return "Vietnamese";
            return DetectLatinLanguage(text);
        case ScriptFamily::Other:
        case ScriptFamily::Count:
            break;
    }
    return std::string();
}

// The historical script-priority chain, kept verbatim as (a) the decision for
// pure-script text, where the dominant family IS the first matching branch,
// and (b) the documented fallback for near-ties and pluralities.
std::string DetectLanguageByScriptPriority(std::wstring_view trimmed) {
    // Script priority order:
    if (ContainsKorean(trimmed)) return "Korean";
    if (ContainsKana(trimmed)) return "Japanese"; // Kana takes precedence over Hanzi for Japanese
    if (ContainsThai(trimmed)) return "Thai";
    
    if (ContainsArabic(trimmed)) return DetectArabicVariant(trimmed);

    // G-4 (user-approved 2026-09-07): Hebrew gets its OWN label immediately
    // after Arabic (same pattern) - reporting Hebrew as "Arabic" would corrupt
    // downstream logs and the already-target bypass (NormalizeLanguageCode
    // resolves "Hebrew" -> registry code "HE", so ShouldTranslate now bypasses
    // Hebrew text under a Hebrew target instead of shipping it to the engine).
    if (ContainsHebrew(trimmed)) return "Hebrew";
    
    if (ContainsCyrillic(trimmed)) return DetectCyrillicVariant(trimmed);
    
    if (ContainsVietnamese(trimmed)) return "Vietnamese";
    if (ContainsHanzi(trimmed)) return "Chinese Simplified";

    if (ContainsDevanagari(trimmed)) return "Hindi";
    if (ContainsBengali(trimmed)) return "Bengali";
    if (ContainsKhmer(trimmed)) return "Khmer";
    if (ContainsLao(trimmed)) return "Lao";
    if (ContainsMyanmar(trimmed)) return "Burmese";
    if (ContainsGreek(trimmed)) return "Greek";

    // F1 (session 260908_0003, verify 220010 root cause R2): Latin is a SCRIPT
    // family, not a language. The old unconditional `return "English"` here
    // (a) force-labeled French/Spanish/Portuguese/German text English,
    // (b) let the ShouldTranslate already-target gate SILENTLY bypass any
    //     Latin->English request (Claim C), and
    // (c) got INJECTED as the engine source under Auto Detect (ADR-A1-2),
    //     overriding Hy-MT2's own language ID with a wrong label.
    // F5 Phase 2 (session 260908_0003, ask audit 181530 condition 1 Option B,
    // VP/user adjudication of the user SCOPE DIRECTIVE "Hy-MT2에서 지원하는
    // 모든 언어쌍을 정확하게 100% 지원"): pure-ASCII Latin SENTENCES keep the
    // canonical AUTO name - ASCII Latin cannot distinguish English from
    // Indonesian/Malay/Tagalog/Swahili (all Hy-MT2-supported), so asserting
    // "English" re-opened a SILENT untranslated passthrough for exactly the
    // multilingual users the directive champions. NormalizeLanguageCode maps
    // AUTO to "AUTO", BuildPrompt injects no source token, and Hy-MT2's
    // built-in language ID decides (it separates Indonesian from English).
    // RC-1 (v0.10.5, F-10 review round 1) amends the blanket claim the old
    // comment made ("ANY Latin-script text returns Auto Detect", "step 5 can
    // never see an English label again"): DetectLatinLanguage's 1-2 token
    // path DOES resolve single words - English core vocabulary/stopwords read
    // "English" (so the already-target bypass fires for "hello" targeting
    // English), and a non-English distinctive diacritic or stopword reads its
    // language. Sentences (>= 3 tokens) are unchanged: AUTO, model decides.
    // Non-Latin scripts keep the contract above unchanged - their labels are
    // script-certain and still drive the already-target bypass.
    if (ContainsLatin(trimmed)) {
        std::string latin_detected = DetectLatinLanguage(trimmed);
        return latin_detected;
    }

    return "Unknown";
}

} // namespace

// v0.10.7 (session 261111_0001 U2, CEO language-fairness requirement):
// DOMINANT-SCRIPT decision before the historical priority chain.
//
// The old body was the any-contains cascade now kept as
// DetectLanguageByScriptPriority: `ContainsKorean(trimmed)` ran first, so one
// Hangul syllable anywhere in an otherwise English sentence ("The
// configuration file is corrupted. 한") labeled the WHOLE selection "Korean".
// Under a Korean target that became a Korean->Korean identity request (the
// prompt read "Translate the following Korean text into Korean") and on the
// drag path a spurious ResolveEffectiveTarget pivot. The ratio model below
// labels by MAJORITY script instead, and the two documented mixed-text cases
// (near-tie, no majority) keep the historical chain, so pure-script input is
// byte-identical to the pre-v0.10.7 answer.
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

    const ScriptTally tally = TallyScriptFamilies(trimmed);
    ScriptFamily dominant = ScriptFamily::Count;
    if (TryDominantScriptFamily(tally, dominant)) {
        const std::string label = LabelForScriptFamily(dominant, trimmed);
        if (!label.empty()) {
            DIAG_F("SMART_BYPASS/DetectLanguage: dominant script family=%d letters=%zu label=%s\n",
                   static_cast<int>(dominant), tally.letters, label.c_str());
            return label;
        }
    } else {
        DIAG_F("SMART_BYPASS/DetectLanguage: no dominant script (letters=%zu), priority order decides\n",
               tally.letters);
    }

    return DetectLanguageByScriptPriority(trimmed);
}

namespace {

// RC-6 (v0.10.5 handoff section 2.7): Japanese shinjitai-only ideographs.
// Curated set (bounded, not exhaustive): characters a simplified-Chinese
// writer would not use (駅, 図, 読, 語, 訳, ...). Pure-kanji words carrying
// one of these cannot be Chinese Simplified, so an ambiguous short word is
// labeled Japanese instead when the user's pair involves Japanese.
constexpr uint32_t kJapaneseOnlyKanji[] = {
    0x4E9C, 0x4FA1, 0x50CD, 0x5186, 0x53B3, 0x55B6, 0x56E3, 0x56F2,
    0x56F3, 0x5727, 0x58CA, 0x5909, 0x5A2F, 0x5BDB, 0x5BFE, 0x5E2F,
    0x5EC3, 0x5F10, 0x5F3E, 0x5F93, 0x6075, 0x6255, 0x62E1, 0x6319,
    0x635C, 0x63B2, 0x63FA, 0x65C9, 0x66C9, 0x67FB, 0x6804, 0x6848,
    0x685C, 0x691C, 0x6B69, 0x6B73, 0x6B8B, 0x6BCE, 0x6C17, 0x6CA2,
    0x6D45, 0x6D5C, 0x6DDD, 0x6E08, 0x6E80, 0x6EDE, 0x6F5C, 0x702C,
    0x713C, 0x72A0, 0x72ED, 0x7363, 0x7573, 0x767A, 0x770C, 0x7815,
    0x7A0E, 0x7A83, 0x7ADC, 0x7C8B, 0x7D04, 0x7D9A, 0x7DD1, 0x7E01,
    0x7E26, 0x8074, 0x81D3, 0x8217, 0x8535, 0x85AC, 0x899A, 0x89A7,
    0x89B3, 0x8A33, 0x8A9E, 0x8AAD, 0x8B21, 0x8CCE, 0x8DF4, 0x8EF1,
    0x8FBC, 0x905F, 0x90F7, 0x9154, 0x91C8, 0x9244, 0x9271, 0x9332,
    0x933C, 0x967A, 0x96D1, 0x970A, 0x99C4, 0x99C7, 0x9A12, 0x9A13,
    0x9AED, 0x9D8F, 0x9EBA, 0x9ED2,
};

// RC-6: traditional-only ideographs (Traditional Chinese but neither
// simplified Chinese nor Japanese, e.g. 學, 體, 國, 這, 龍).
constexpr uint32_t kTraditionalOnlyKanji[] = {
    0x5011, 0x570B, 0x5B78, 0x5E6B, 0x6C92, 0x7063, 0x70BA, 0x8166,
    0x81FA, 0x88E1, 0x8AAA, 0x8B80, 0x8B93, 0x8C93, 0x9019, 0x908A,
    0x91AB, 0x95DC, 0x96DC, 0x9748, 0x975C, 0x9910, 0x9AD4, 0x9DC4,
    0x9E7D, 0x9EB5, 0x9EBC, 0x9EDE, 0x9EE8, 0x9F8D,
};

bool CodePointInTable(uint32_t cp, const uint32_t* table, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (table[i] == cp) {
            return true;
        }
    }
    return false;
}

// BUG-7 (v0.10.5 handoff section 2.8): does a code point belong to the
// script family the target language is written in? Latin-script targets
// accept the whole Latin family; JA accepts Han + Kana. AUTO/unknown
// targets accept nothing (the dominance gate then never fires).
bool MatchesTargetScriptCodePoint(uint32_t cp, std::string_view target_code) {
    const std::string code = NormalizeLanguageCode(target_code);
    if (code.empty() || code == "AUTO") return false;
    if (code == "KO") return IsKoreanCodePoint(cp);
    if (code == "JA") return IsHanziCodePoint(cp) || IsKanaCodePoint(cp);
    if (code.starts_with("ZH")) return IsHanziCodePoint(cp);
    if (code == "RU" || code == "UK") return IsCyrillicCodePoint(cp);
    if (code == "TH") return IsThaiCodePoint(cp);
    if (code == "AR" || code == "FA" || code == "UR") return IsArabicCodePoint(cp);
    if (code == "HE") return IsHebrewScriptCodePoint(cp);
    if (code == "HI") return IsDevanagariCodePoint(cp);
    if (code == "BN") return IsBengaliCodePoint(cp);
    if (code == "KM") return IsKhmerCodePoint(cp);
    if (code == "LO") return IsLaoCodePoint(cp);
    if (code == "MY") return IsMyanmarCodePoint(cp);
    if (code == "EL") return IsGreekCodePoint(cp);
    return IsLatinCodePoint(cp); // the Latin-script family targets
}

// BUG-7: the already-target skip may fire only when the target script is
// DOMINANT (>= 70% of the letters). DetectLanguage labels by script PRIORITY
// (one Hangul syllable makes "Please review the 기획 document" "Korean"), so
// without this gate a foreign sentence carrying a single target-script word
// was silently never translated.
bool TargetScriptDominant(std::wstring_view text, std::string_view target_code) {
    size_t letters = 0;
    size_t target_letters = 0;
    size_t idx = 0;
    while (idx < text.size()) {
        const uint32_t cp = DecodeNextCodePoint(text, idx);
        if (!IsLinguisticCodePoint(cp)) {
            continue; // digits, punctuation, emoji carry no language signal
        }
        ++letters;
        if (MatchesTargetScriptCodePoint(cp, target_code)) {
            ++target_letters;
        }
    }
    return letters > 0 && target_letters * 10 >= letters * 7;
}

// RC-6: the configured pair for this decision involves Japanese or
// Traditional Chinese, where a short pure-kanji word is genuinely ambiguous
// (翻訳 is Japanese, 翻译 is simplified; the ZH-CN label must not be forced).
bool PairInvolvesJapaneseOrTraditional(std::string_view target_code,
                                       std::string_view source_norm) {
    const std::string tgt = NormalizeLanguageCode(target_code);
    const std::string src = NormalizeLanguageCode(source_norm);
    return tgt == "JA" || tgt == "ZH-TW" || src == "JA" || src == "ZH-TW";
}

size_t CountHanziCodePoints(std::wstring_view text) {
    size_t count = 0;
    size_t idx = 0;
    while (idx < text.size()) {
        const uint32_t cp = DecodeNextCodePoint(text, idx);
        if (IsHanziCodePoint(cp)) {
            ++count;
        }
    }
    return count;
}

} // namespace

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

    // F1 (session 260908_0003, verify 220010 root cause R3): resolve the
    // source pin FIRST - an explicit user pin is ground truth. "Auto Detect"
    // (and any unresolvable/empty token, which normalizes to AUTO) means no
    // pin. The old logic ran DetectLanguage's script result against the target
    // even under a pin, so a pinned-French text with shared diacritics that
    // misdetected as Vietnamese was wrongly bypassed toward a Vietnamese
    // target, and pinned sources could be overridden by detection noise.
    std::string source_norm = NormalizeLanguageCode(source_code_or_name);
    const bool src_pinned = source_norm != "AUTO" && !source_code_or_name.empty() &&
                            !EqualsIgnoreCaseAscii(source_code_or_name, std::string_view{"Auto Detect"});

    // 5. If input text is already in the target language, bypass translation
    //    immediately - but ONLY under Auto Detect. With an explicit pin, the
    //    pin-vs-target comparison in step 7 owns the identity decision. The
    //    "Auto Detect" detection outcome (diacritic Latin - script-certain,
    //    language-ambiguous) must never match a real target, so it is skipped
    //    here as well: the request passes through to the engine.
    //    F5 (audit 181530 Option B): DetectLanguage returns "Auto Detect"
    //    for ALL Latin script (pure-ASCII included) EXCEPT the RC-1 (v0.10.5)
    //    1-2 token path: single high-frequency English words ("hello",
    //    "good morning") resolve to "English" deliberately, so the already-
    //    target bypass fires for them instead of spending an engine round
    //    trip that a rung-1 echo would poison. The labels that reach this
    //    comparison are script-certain (KO/JA/ZH/TH/AR/HE/RU + true-VI) or
    //    the RC-1 short-English read; an ASCII-Latin SENTENCE targeting
    //    English still routes as AUTO translation (the model's language ID
    //    owns the call), and pinned English -> English still bypasses
    //    through step 7 (a user pin is ground truth).
    std::string detected = DetectLanguage(trimmed);
    if (!src_pinned && detected != "Unknown" && detected != "Auto Detect") {
        // RC-6 (v0.10.5 handoff section 2.7): a short pure-kanji word (<= 4
        // ideographs, no kana) is not force-labeled "Chinese Simplified" when
        // the configured pair involves Japanese or Traditional Chinese - the
        // label would both falsely bypass Japanese text under a ZH target and
        // poison the prompt source. Japanese-only shinjitai confirm Japanese,
        // traditional-only ideographs confirm Chinese Traditional, and
        // anything ambiguous stays the AUTO marker so the model decides.
        if (detected == "Chinese Simplified" &&
            PairInvolvesJapaneseOrTraditional(target_code, source_norm)) {
            const size_t hanzi_count = CountHanziCodePoints(trimmed);
            if (hanzi_count >= 1 && hanzi_count <= 4 && !ContainsKana(trimmed)) {
                if (ContainsCodePointMatching(trimmed, [](uint32_t cp) {
                        return CodePointInTable(cp, kJapaneseOnlyKanji, std::size(kJapaneseOnlyKanji));
                    })) {
                    detected = "Japanese";
                } else if (ContainsCodePointMatching(trimmed, [](uint32_t cp) {
                               return CodePointInTable(cp, kTraditionalOnlyKanji, std::size(kTraditionalOnlyKanji));
                           })) {
                    detected = "Chinese Traditional";
                } else {
                    detected = "Auto Detect";
                }
            }
        }
        // BUG-7 (v0.10.5 handoff section 2.8): the detection-based already-
        // target skip fires only while the target script is DOMINANT (>= 70%
        // of the letters), so "Please review the 기획 document" still
        // translates to Korean instead of being silently skipped.
        const bool target_script_dominant = TargetScriptDominant(trimmed, target_code);
        if (target_script_dominant &&
            (EqualsIgnoreCaseAscii(std::string_view{detected}, std::string_view{target_name}) ||
             EqualsIgnoreCaseAscii(std::string_view{NormalizeLanguageCode(detected)}, std::string_view{target_code}))) {
            // R5 observability: pin the exact bypass so "didn't translate after
            // a language switch" is attributable at runtime.
            DIAG_F(
                    "SMART_BYPASS/ShouldTranslate/001: already-target bypass (detected=%s, target=%s/%s)\n",
                    detected.c_str(), target_name.c_str(), target_code.c_str());
            return false;
        }
        if (target_script_dominant && target_code.starts_with("ZH") &&
            detected.find("Chinese") != std::string::npos) {
            if (EqualsIgnoreCaseAscii(std::string_view{detected}, std::string_view{target_name})) {
                DIAG_F(
                        "SMART_BYPASS/ShouldTranslate/002: Chinese-variant bypass (detected=%s, target=%s/%s)\n",
                        detected.c_str(), target_name.c_str(), target_code.c_str());
                return false;
            }
        }
    }

    // 6. Resolve source language (F1: uses the pin predicate computed above)
    std::string effective_source_name;
    if (src_pinned) {
        const auto* src_info = FindLanguageByCode(source_norm);
        effective_source_name = src_info ? src_info->name_en : std::string(source_code_or_name);
    } else {
        effective_source_name = detected;
    }

    // 7. Check if configured source matches target
    if (effective_source_name != "Unknown") {
        // BUG-7 (v0.10.5, round 2): the config-based same-language skip runs
        // through the SAME TargetScriptDominant gate as the detection-based
        // skip in step 5. For an unpinned source effective_source_name IS
        // detected, so without this gate step 7 re-applied the detected ==
        // target bypass for mixed-script text ("Please review the 기획
        // document" under a Korean target) that step 5 had just refused -
        // the sentence was silently never translated. The skip now fires
        // only while the target script carries >= 70% of the letters, for
        // the configured source exactly as for the detected one.
        const bool target_script_dominant = TargetScriptDominant(trimmed, target_code);
        if (target_script_dominant &&
            (EqualsIgnoreCaseAscii(std::string_view{effective_source_name}, std::string_view{target_name}) ||
             EqualsIgnoreCaseAscii(std::string_view{NormalizeLanguageCode(effective_source_name)}, std::string_view{target_code}))) {
            return false;
        }

        // Chinese variant normalization
        if (target_script_dominant && target_code.starts_with("ZH") &&
            (effective_source_name.find("Chinese") != std::string::npos)) {
            if (EqualsIgnoreCaseAscii(std::string_view{effective_source_name}, std::string_view{target_name})) {
                return false;
            }
        }
    }

    return true;
}

} // namespace emebalachat
