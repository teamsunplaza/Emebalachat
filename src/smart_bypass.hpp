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

// Returns true if text contains Arabic script characters (U+0600-U+06FF,
// U+0750-U+077F, U+08A0-U+08FF). Hebrew script is NOT Arabic: it has its own
// predicate below since G-4 (REQ-040, user-approved 2026-09-07).
bool ContainsArabic(std::wstring_view text);

// Returns true if text contains Hebrew script characters (U+0590-U+05FF).
// G-4 extension of the smart-bypass detection; the range constant is shared
// with bidi_utils' first-strong scanner (src/bidi_utils.hpp) so the
// translation trigger and the REQ-038 render heuristic cannot drift.
bool ContainsHebrew(std::wstring_view text);

// Returns true if text contains Thai script characters.
bool ContainsThai(std::wstring_view text);

// Returns true if text contains Vietnamese-SPECIFIC codepoints only (F1,
// session 260908_0003: Latin Extended Additional tone marks 0x1EA0-0x1EF9
// plus O-horn/U-horn/A-breve/D-stroke. Shared Latin-1 accented letters and
// U-tilde - Portuguese orthography too - are NOT Vietnamese markers anymore).
bool ContainsVietnamese(std::wstring_view text);

// Returns true if text contains Latin-SCRIPT letters (accents included). A
// script-family classifier, not a language assertion (F1).
bool ContainsLatin(std::wstring_view text);

bool ContainsDevanagari(std::wstring_view text);
bool ContainsBengali(std::wstring_view text);
bool ContainsKhmer(std::wstring_view text);
bool ContainsLao(std::wstring_view text);
bool ContainsMyanmar(std::wstring_view text);
bool ContainsGreek(std::wstring_view text);

// 261111_0001 U6 (v0.10.7, R2-02/R2-04 prompt-label governance): the ONE
// accessor every engine-prompt source label must come from. DetectLanguage
// itself carries the v0.10.7 dominant-script gate (U2's ratio model: a
// family owns the label only with an absolute majority and a clear lead;
// near-ties fall back to the historical priority chain), so a raw call is
// already dominance-gated — this wrapper adds the prompt-side contract on
// top, with NO duplicated logic:
//   * "Unknown" (empty/garbage input) resolves to the canonical AUTO
//     marker, so the engine's built-in language ID decides instead of a
//     bogus label reaching the prompt;
//   * every prompt consumer (typing path src/worker.cpp, drag path
//     src/main.cpp, and the back-translation ladder source that rides the
//     same resolved label) goes through ONE documented seam, so a future
//     label-governance rule has a single insertion point.
// Deliberately NOT folded into DetectLanguage itself: ShouldTranslate
// consumes the raw label for the already-target bypass and its own RC-6
// kanji pair gate, and DetectLanguage must stay a pure text->label fn.
//
// Detects language of given text based on script analysis.
// Returns "Korean", "Japanese", "Vietnamese", "Chinese Simplified", "Russian",
// "Thai", "Arabic", "Hebrew", "Auto Detect", or "Unknown". ("English" as a
// SENTENCE label was removed by F5; the RC-1 1-2 token path below restores it
// for single words; see below.)
// ("Hebrew" added by G-4, REQ-040 batch B-6, user-approved 2026-09-07:
// Hebrew text must never be reported as "Arabic" - the name feeds
// NormalizeLanguageCode -> registry "HE" and the already-target bypass.)
// (F1, session 260908_0003, verify 220010: diacritic-bearing LATIN text
// returns the canonical AUTO name "Auto Detect" instead of a forced language
// identity - NormalizeLanguageCode maps it to "AUTO", so the drag/typing
// chains inject NO source token and Hy-MT2's built-in language ID decides.
// F-10/RC-1 (v0.10.5) carve the narrow exception: 1-2 token inputs are
// resolved by DetectLatinLanguage before this rule is reached.)
// (F5 Phase 2, session 260908_0003, ask audit 181530 condition 1 Option B,
// VP/user adjudication: the pure-ASCII "English" label is REMOVED for general
// Latin text. ASCII Latin cannot separate English from Indonesian/Malay/
// Tagalog/Swahili (all Hy-MT2-supported), so keeping it re-opened a silent
// untranslated passthrough for Latin-script non-English users targeting
// English: SENTENCES return "Auto Detect" and the model's built-in language
// ID decides. RC-1 (v0.10.5 handoff section 2.2) restores a NARROW exception:
// 1-2 token inputs matching high-frequency English core vocabulary or
// stopwords resolve to "English" (single common words spend no engine round
// trip), and 1-2 token words with a non-English distinctive diacritic or
// stopword resolve to that language. The true EN->EN identity bypass for
// sentences survives ONLY for an explicitly pinned English source
// (ShouldTranslate step 7). Accepted cost per the audit: a multi-word English
// sentence under Auto targeting English spends one local inference and
// returns near-identical output.)
// (G-04, 261111_0001 fix-list round 2: that short-sample claim is REFUSED on
// counter-evidence - a second language's distinctive diacritic, another Latin
// language's stopword, or an English core-vocabulary word - and then falls
// back to the AUTO marker, so "hello senor" is no longer asserted Spanish.)
std::string DetectLanguage(std::wstring_view text);

// THE PROMPT-LABEL ACCESSOR (governance contract documented above):
// dominance-gated via DetectLanguage, plus the "Unknown" -> "Auto Detect"
// normalization every engine-prompt consumer must share. Inline, thin — the
// logic lives exclusively in DetectLanguage (U2); no duplication here.
inline std::string DetectLanguageForPrompt(std::wstring_view text) {
    std::string label = DetectLanguage(text);
    if (label == "Unknown") {
        return "Auto Detect";
    }
    return label;
}

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
