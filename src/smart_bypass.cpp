#include "smart_bypass.hpp"
#include "bidi_utils.hpp"
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

bool ContainsHebrew(std::wstring_view text) {
    // G-4 (REQ-040, design §2.2.6, user-approved 2026-09-07): Hebrew-script
    // detection, same scan pattern as ContainsArabic. The U+0590-U+05FF range
    // lives in bidi_utils (IsHebrewScriptCodePoint) so the translation
    // trigger and the REQ-038 first-strong scanner share one source of truth.
    size_t idx = 0;
    while (idx < text.size()) {
        uint32_t cp = DecodeNextCodePoint(text, idx);
        if (IsHebrewScriptCodePoint(cp)) {
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

bool ContainsDevanagari(std::wstring_view text) {
    size_t idx = 0;
    while (idx < text.size()) {
        uint32_t cp = DecodeNextCodePoint(text, idx);
        if (IsDevanagariCodePoint(cp)) {
            return true;
        }
    }
    return false;
}

bool ContainsBengali(std::wstring_view text) {
    size_t idx = 0;
    while (idx < text.size()) {
        uint32_t cp = DecodeNextCodePoint(text, idx);
        if (IsBengaliCodePoint(cp)) {
            return true;
        }
    }
    return false;
}

bool ContainsKhmer(std::wstring_view text) {
    size_t idx = 0;
    while (idx < text.size()) {
        uint32_t cp = DecodeNextCodePoint(text, idx);
        if (IsKhmerCodePoint(cp)) {
            return true;
        }
    }
    return false;
}

bool ContainsLao(std::wstring_view text) {
    size_t idx = 0;
    while (idx < text.size()) {
        uint32_t cp = DecodeNextCodePoint(text, idx);
        if (IsLaoCodePoint(cp)) {
            return true;
        }
    }
    return false;
}

bool ContainsMyanmar(std::wstring_view text) {
    size_t idx = 0;
    while (idx < text.size()) {
        uint32_t cp = DecodeNextCodePoint(text, idx);
        if (IsMyanmarCodePoint(cp)) {
            return true;
        }
    }
    return false;
}

bool ContainsGreek(std::wstring_view text) {
    size_t idx = 0;
    while (idx < text.size()) {
        uint32_t cp = DecodeNextCodePoint(text, idx);
        if (IsGreekCodePoint(cp)) {
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
    { "English", kStopWordsEN, std::size(kStopWordsEN), 2 },
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
    
    if (ContainsArabic(trimmed)) {
        // Disambiguate Arabic, Persian, Urdu
        bool is_urdu = false;
        bool is_persian = false;
        size_t idx = 0;
        while (idx < trimmed.size()) {
            uint32_t cp = DecodeNextCodePoint(trimmed, idx);
            if (cp == 0x0679 || cp == 0x0688 || cp == 0x0691 || cp == 0x06BA || cp == 0x06D2 || cp == 0x06BE) {
                is_urdu = true;
                break;
            }
            if (cp == 0x067E || cp == 0x0686 || cp == 0x0698 || cp == 0x06AF) {
                is_persian = true;
            }
        }
        if (is_urdu) return "Urdu";
        if (is_persian) return "Persian";
        return "Arabic";
    }

    // G-4 (user-approved 2026-09-07): Hebrew gets its OWN label immediately
    // after Arabic (same pattern) - reporting Hebrew as "Arabic" would corrupt
    // downstream logs and the already-target bypass (NormalizeLanguageCode
    // resolves "Hebrew" -> registry code "HE", so ShouldTranslate now bypasses
    // Hebrew text under a Hebrew target instead of shipping it to the engine).
    if (ContainsHebrew(trimmed)) return "Hebrew";
    
    if (ContainsCyrillic(trimmed)) {
        // Disambiguate Russian vs Ukrainian
        bool is_ukrainian = false;
        size_t idx = 0;
        while (idx < trimmed.size()) {
            uint32_t cp = DecodeNextCodePoint(trimmed, idx);
            if (cp == 0x0491 || cp == 0x0454 || cp == 0x0456 || cp == 0x0457) {
                is_ukrainian = true;
                break;
            }
        }
        if (is_ukrainian) return "Ukrainian";
        return "Russian";
    }
    
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
    // F1 Phase 1 kept the "English" label for PURE-ASCII Latin so the EN->EN
    // identity bypass survived. F5 Phase 2 (this session, ask audit 181530
    // condition 1 Option B, VP/user adjudication of the user SCOPE DIRECTIVE
    // "Hy-MT2에서 지원하는 모든 언어쌍을 정확하게 100% 지원") removes the
    // residue: pure-ASCII Latin cannot distinguish English from Indonesian/
    // Malay/Tagalog/Swahili (all Hy-MT2-supported, all ASCII-Latin), so
    // asserting "English" re-opened a SILENT untranslated passthrough for
    // exactly the multilingual users the directive champions. Policy now: ANY
    // Latin-script text returns the canonical AUTO name - NormalizeLanguageCode
    // maps it to "AUTO", BuildPrompt injects no source token, and Hy-MT2's
    // built-in language ID decides (it separates Indonesian from English).
    // Accepted cost, per the audit: a short English sentence targeting English
    // spends one local inference and returns near-identical text (no user
    // harm; the old failure was silent). The true EN->EN identity bypass now
    // exists ONLY for an explicitly pinned English source (ShouldTranslate
    // step 7; step 5 can never see an "English" label again). Non-Latin
    // scripts keep the contract above unchanged - their labels are script-
    // certain and still drive the already-target bypass.
    if (ContainsLatin(trimmed)) {
        std::string latin_detected = DetectLatinLanguage(trimmed);
        return latin_detected;
    }

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

    // F1 (session 260908_0003, verify 220010 root cause R3): resolve the
    // source pin FIRST - an explicit user pin is ground truth. "Auto Detect"
    // (and any unresolvable/empty token, which normalizes to AUTO) means no
    // pin. The old logic ran DetectLanguage's script result against the target
    // even under a pin, so a pinned-French text with shared diacritics that
    // misdetected as Vietnamese was wrongly bypassed toward a Vietnamese
    // target, and pinned sources could be overridden by detection noise.
    std::string source_norm = NormalizeLanguageCode(source_code_or_name);
    const bool src_pinned = source_norm != "AUTO" && !source_code_or_name.empty() &&
                            !CaseInsensitiveEqual(source_code_or_name, "Auto Detect");

    // 5. If input text is already in the target language, bypass translation
    //    immediately - but ONLY under Auto Detect. With an explicit pin, the
    //    pin-vs-target comparison in step 7 owns the identity decision. The
    //    "Auto Detect" detection outcome (diacritic Latin - script-certain,
    //    language-ambiguous) must never match a real target, so it is skipped
    //    here as well: the request passes through to the engine.
    //    F5 (audit 181530 Option B): DetectLanguage now returns "Auto Detect"
    //    for ALL Latin script (pure-ASCII included), so the labels that can
    //    still reach this comparison are script-certain languages only (KO/JA/
    //    ZH/TH/AR/HE/RU + true-VI markers). The detection-based "English"
    //    bypass retired with the label; the identity-bypass predicate is now
    //    keyed on script CERTAINTY, not on the "English" name: an ASCII-Latin
    //    sentence targeting English routes as AUTO translation (the model's
    //    language ID owns the call), while pinned English -> English still
    //    bypasses through step 7 (a user pin is ground truth).
    std::string detected = DetectLanguage(trimmed);
    if (!src_pinned && detected != "Unknown" && detected != "Auto Detect") {
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
