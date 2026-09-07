#pragma once

// P4 Batch B-1 (session 260907_0002, design §2-Q1 / §2.2.1): BiDi direction
// helpers for REQ-038 (RTL rendering). Two pure seams, no Win32/ICU in the
// logic so they are unit-testable headlessly (TestBidiUtils):
//
//   * Locale/language direction: a static closed set {AR, FA, UR, HE} —
//     exactly 4 of the 37 registry languages are RTL (design [P2 §A2]).
//     Consumed by UI-chrome callers (tooltip body per target language, About
//     body per UI locale). IsRtlLanguageCode accepts ISO codes AND English /
//     native names by resolving through the kAllLanguages registry
//     (src/config.cpp), so the tooltip's target_lang_ wstring (which carries
//     name_en) needs no payload format change.
//   * Per-string fallback: GuessBaseDirection implements the UAX #9 P2/P3
//     first-strong heuristic over UTF-16 text whose language is unknown at
//     render time (message-mode notices, future content paths).
//
// Boring-technology constraint (design §1.2.7): NO ICU, NO third-party BiDi
// library — DWrite already applies full UAX #9 internally; we only feed it
// the correct paragraph base direction (IDWriteTextFormat::SetReadingDirection
// call sites land in B-2/B-5).

#include <string_view>

#include "i18n.hpp"

namespace emebalachat {

// Base reading direction of a text run or a UI locale.
enum class TextDirection { LTR, RTL };

// True iff the UI locale's script is right-to-left. Static closed set:
// Arabic, Persian, Urdu, Hebrew (design §2-Q1 verdict A). Pure.
//
// Implementation note: resolution goes through I18n::LocaleToString + the
// RTL code set instead of an enum switch, because the RTL UiLocale
// enumerators are introduced in B-3 (design §2.1.1). This keeps B-1
// file-disjoint from src/i18n.* while making the function become correct for
// Arabic/Persian/Urdu/Hebrew the moment B-3 extends the enum and its
// LocaleToString mapping — no edit here required.
bool IsRtlLocale(UiLocale locale);

// True iff a translation-language token is RTL. Accepts the closed set
// {AR, FA, UR, HE} case-insensitively AND canonical English names ("Arabic")
// AND native names ("العربية"), resolved via the 38-entry kAllLanguages
// registry (FindLanguageByCode / FindLanguageByName in src/config.cpp).
// Unresolvable or empty tokens are false (they normalize to "AUTO"). Pure.
bool IsRtlLanguageCode(std::string_view code);

// Convenience wrapper for chrome callers that need a TextDirection, not a bool.
TextDirection DirectionForLocale(UiLocale locale);

// UAX #9 P2/P3 "first strong character" heuristic over UTF-16 text.
//
//   * Weak/neutral code points are SKIPPED: ASCII/Unicode digits, whitespace,
//     punctuation and symbols, and all Bidi_Control / bidi-formatting chars
//     (ZWNJ..RLM U+200C–U+200F, embeddings LRE/RLE/PDF U+202A–U+202E, BN U+2060–U+2064,
//     isolates LRI..PDI U+2066–U+2069, BOM U+FEFF).
//   * First strong RTL character => RTL: Hebrew blocks (U+0590–U+05FF,
//     presentation forms U+FB1D–U+FB4F, Samaritan U+07C0–U+085F — class R)
//     and Arabic blocks (U+0600–U+06FF, U+0750–U+077F, U+0870–U+08FF,
//     U+FB50–U+FDFF, U+FE70–U+FEFF — class AL, treated as RTL per design §2-Q1).
//   * First strong LTR character => LTR: Latin, Greek, Cyrillic, Armenian,
//     Indic, Southeast-Asian, Georgian, Hangul, Kana, CJK ideographs and the
//     remaining BMP/astral letter ranges (class L).
//   * No strong character at all => LTR (UAX #9 P2 default).
//
// Surrogate pairs are decoded before classification. Pure: no ICU, no Win32
// categoricals (static range tables only, per design §2-Q1 implementation note).
TextDirection GuessBaseDirection(std::wstring_view text);

} // namespace emebalachat
