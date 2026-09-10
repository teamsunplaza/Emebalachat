#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace emebalachat {

// ASCII-only case-insensitive string comparison (REF-3.1 unification).
// Folds 'A'-'Z' with +32 ('a' - 'A'); every other code unit is compared
// verbatim. Locale-independent by construction: it NEVER calls std::tolower,
// so it cannot drift under a non-C CRT locale. For CharT=char this is
// byte-equivalent to the `std::tolower(static_cast<unsigned char>(c))` idiom
// the project previously duplicated in bidi_utils.cpp / smart_bypass.cpp /
// config.cpp: those call sites ran exclusively under the default "C" locale
// (no setlocale call exists anywhere in src/), where tolower maps only
// 'A'-'Z', and high bytes (>= 0x80, e.g. UTF-8 lead/continuation bytes) pass
// through both implementations unchanged and compare equal byte-for-byte.
template <typename CharT>
constexpr bool EqualsIgnoreCaseAscii(std::basic_string_view<CharT> a,
                                     std::basic_string_view<CharT> b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        CharT ca = a[i];
        CharT cb = b[i];
        if (ca >= CharT('A') && ca <= CharT('Z')) {
            ca = static_cast<CharT>(ca - CharT('A') + CharT('a'));
        }
        if (cb >= CharT('A') && cb <= CharT('Z')) {
            cb = static_cast<CharT>(cb - CharT('A') + CharT('a'));
        }
        if (ca != cb) return false;
    }
    return true;
}

// Case-insensitive (ASCII fold) prefix test built on the same fold idiom:
// `haystack` starts with `prefix` ignoring ASCII A-Z case. Empty prefix is a
// prefix of every haystack (matches std::string::compare(0, 0, "") == 0
// semantics the A6-converted containment sites relied on). Behavior-identical
// to comparing the first prefix.size() code units in a fully lowercased domain,
// because the fold is idempotent and length-preserving (f(f(x)) == f(x)).
// (A6, session 260910_0007 W3: replaces the lowered-std::string temporaries the
// path-containment checks used to allocate in engine.cpp / config.cpp.)
template <typename CharT>
constexpr bool StartsWithIgnoreCaseAscii(std::basic_string_view<CharT> haystack,
                                         std::basic_string_view<CharT> prefix) noexcept {
    if (haystack.size() < prefix.size()) return false;
    return EqualsIgnoreCaseAscii(haystack.substr(0, prefix.size()), prefix);
}

// Shared path-containment predicate for '/'-separated (generic-format) paths:
// true when `joined` IS `base`, or is strictly inside `base` (prefix match plus
// a '/' segment-boundary character, so "/foobar" is NOT inside "/foo").
// Trailing '/'s on `base` are trimmed before comparing. All comparisons fold
// ASCII A-Z only; every other code unit (including UTF-8 continuation bytes of
// non-ASCII path segments) compares verbatim.
//
// (A6, session 260910_0007 W3): this is the single implementation of the
// containment expression that was duplicated (byte-identical modulo its two
// file-local mirror lowercasers: engine.cpp LowerAscii, config.cpp
// LowerPathAscii) in IsValidModelPath's path-traversal guard and
// config.cpp PathInsideBase. Both sites now call here; the mirrors are gone.
template <typename CharT>
constexpr bool IsPathContainedIgnoreCaseAscii(std::basic_string_view<CharT> joined,
                                              std::basic_string_view<CharT> base) noexcept {
    while (!base.empty() && base.back() == CharT('/')) {
        base.remove_suffix(1);
    }
    if (EqualsIgnoreCaseAscii(joined, base)) return true;
    return joined.size() > base.size() &&
           StartsWithIgnoreCaseAscii(joined, base) &&
           joined[base.size()] == CharT('/');
}

// Value DecodeNextCodePoint returns for unpaired UTF-16 surrogate units.
// 0xFFFF is a Unicode noncharacter that matches no script-range predicate
// and no UAX #9 strong-type table in this codebase, so callers treat it as
// neutral/skip. (REF-3.3 equivalence proof, session 260910_0006 T1: the
// smart_bypass decoder used to return the RAW surrogate value instead;
// both behaviors are observably identical because (a) no Is*CodePoint /
// InAny range covers 0xD800-0xDFFF or 0xFFFF, and (b) a built probe showed
// GetStringTypeW(CT_CTYPE1) reports C1_ALPHA=0 for 0xD800, 0xDBFF, 0xDC00,
// 0xDFFF AND 0xFFFF, so even the locale-backed alpha fallback inside
// IsLinguisticCodePoint rejects both values alike. The idx advance semantics
// are identical either way, so Contains*/GuessBaseDirection cannot tell the
// two decoders apart.)
inline constexpr std::uint32_t kSurrogateNeutralSentinel = 0xFFFFu;

// Decodes one Unicode code point from UTF-16 code units starting at text[idx]
// and advances idx past the consumed unit(s). (REF-3.3 unification of the
// former file-local smart_bypass DecodeNextCodePoint and bidi_utils
// DecodeCodePoint; their advance semantics were already identical.)
//
//   * valid high+low pair  -> consumes 2 units, returns the astral code point
//   * unpaired high (next unit is not a low surrogate, or end of string)
//     -> consumes 1 unit, returns kSurrogateNeutralSentinel
//   * lone low surrogate   -> consumes 1 unit, returns kSurrogateNeutralSentinel
//   * any other unit       -> consumes 1 unit, returns it
//
// Precondition: idx < text.size() (callers loop with exactly this guard).
constexpr std::uint32_t DecodeNextCodePoint(std::wstring_view text,
                                            std::size_t& idx) noexcept {
    const std::uint32_t unit = static_cast<std::uint16_t>(text[idx]);
    if (unit >= 0xD800u && unit <= 0xDBFFu) {
        if (idx + 1 < text.size()) {
            const std::uint32_t lo = static_cast<std::uint16_t>(text[idx + 1]);
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                idx += 2;
                return 0x10000u + ((unit - 0xD800u) << 10) + (lo - 0xDC00u);
            }
        }
        idx += 1;
        return kSurrogateNeutralSentinel;
    }
    if (unit >= 0xDC00u && unit <= 0xDFFFu) {
        idx += 1;
        return kSurrogateNeutralSentinel;
    }
    idx += 1;
    return unit;
}

// Normalizes input wide string to Unicode NFC form using Win32 NormalizeString(NormalizationC).
std::wstring NormalizeNFC(std::wstring_view input);

// Multi-line block fix: normalizes every line break to the CRLF pair Windows
// edit controls expect. Lone '\n' and lone '\r' each become "\r\n"; an existing
// CRLF pair is preserved (never doubled). Everything else — including UTF-16
// surrogate pairs, since 0x0D/0x0A never participate in surrogates — passes
// through unchanged. Pure and script-agnostic: works identically for KO/EN/JA/
// ZH/VI/ES and any other text. Applied symmetrically to the captured source
// block and the translated result so the "translation == source" comparison in
// the worker never fails on line-ending representation alone.
std::wstring NormalizeNewlinesToCRLF(std::wstring_view input);

// Converts a UTF-16 wide string to UTF-8 encoded std::string.
std::string ToUtf8(std::wstring_view wstr);

// Converts a UTF-8 encoded string to UTF-16 std::wstring.
std::wstring ToUtf16(std::string_view str);

// Decodes one complete JSON `\uXXXX` escape — including the optional UTF-16
// surrogate-pair lookahead — straight out of the source buffer, appending the
// result as UTF-8 to `out`. (REF-3.5, session 260910_0006 T4: shared by
// config.cpp SimpleJsonReader::ParseString and google_translate.cpp
// ParseJsonString; config.cpp's I2-defended implementation is the reference.)
//
// Preconditions: pos points at the FIRST HEX DIGIT following the 'u' of the
// escape (the caller's switch already consumed the backslash and 'u'), and
// pos < src.size() is not required — a truncated tail is the false case.
//
// Contract (bit-for-bit the config.cpp reference semantics):
//   * true  -> exactly one of:
//       - valid pair (high + `\uXXXX` low): consumes all 12 chars, appends the
//         4-byte UTF-8 of the astral code point;
//       - BMP / ASCII escape: consumes 4 hex digits, appends 1-3 byte UTF-8;
//       - LONE high surrogate (valid pair not formed) or LONE low surrogate:
//         appends U+FFFD (EF BF BD), NEVER raw surrogate WTF-8 (the I2 fix).
//         When the pair lookahead consumed a failed candidate (`\u` + bad
//         hex), pos is ROLLED BACK to the candidate's start so the caller's
//         main loop re-parses that escape normally as its own `\uXXXX`.
//   * false -> the 4 hex digits are missing (truncation) or `std::stoul`
//     throws (non-hex). The caller must fail the whole string parse.
//   Note: stoul parses a hex PREFIX, so `\u12zz` decodes to U+0012 here —
//   this permissive behavior is identical in both original parsers and is
//   preserved deliberately (pin-tested), not silently tightened.
//
// The full-case scope (lookahead + rollback + UTF-8 encoder, not just the
// code-unit-to-UTF-8 step) is intentional: both call sites' `case 'u'` bodies
// already differed ONLY in the lone-surrogate defense, so sharing the entire
// case collapses the duplicated hex-reading and encoding tables too.
bool AppendJsonUnicodeEscape(std::string& out, std::string_view src,
                             std::size_t& pos);

} // namespace emebalachat
