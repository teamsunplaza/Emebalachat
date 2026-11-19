#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 261116_0003 P4 (language-menu redesign, plan section 3 D4 + section 4
// "Aliases"): the alias-search data contract and the folding matcher the P5
// search palette consumes.
//
// The DATA lives in the generated src/language_alias.cpp (CLDR-derived
// 37-locale x 89-target matrix + the hand-curated script-name table + the
// fold table; generator: tools/generate_language_aliases.py with a provenance
// header). The LOGIC below is hand-written and deliberately OUTSIDE the
// generated file, so regeneration can never clobber reviewed code.
//
// Design record (vp-proposal decision 2): the runtime GetLocaleInfoEx exonym
// path is dropped - it serves exactly one language per process (the Windows
// display language), which cannot satisfy "a German exonym query must hit
// under a Korean UI". The static matrix indexes all 37 UI locales at once,
// which is what makes cross-locale exonym search work.
#include "config.hpp"        // LanguageInfo (the pool element type)
#include "unicode_utils.hpp" // EqualsIgnoreCaseAscii (reused, not re-implemented)

namespace emebalachat {

// Rank tier of one alias atom. LOWER IS BETTER (the enum values ARE the sort
// key, plan D4 order: code > native > English > exonym > script-alias).
enum class AliasTier : std::uint8_t {
    Code = 0,     // the registry code ("KO") or its bcp47 tag ("zh-HK")
    Native = 1,   // LanguageInfo::name_native (the language's own name)
    English = 2,  // LanguageInfo::name_en + the CLDR English name
    Exonym = 3,   // the CLDR name in any of the other 36 UI locales
    Script = 4,   // the hand-curated script-name table (plan D4)
    // Not a match: the empty-query "unfiltered pool" row (see SearchLanguages).
    None = 0xFF,
};

// How a query met an alias; LOWER IS BETTER within one tier.
enum class AliasMatchKind : std::uint8_t {
    Exact = 0,     // folded alias == folded query
    Prefix = 1,    // folded alias starts with the folded query
    Substring = 2, // folded query occurs inside the folded alias (>= 3 code points)
};

// LanguageAliasAtom::locale_index value for an atom with no source locale
// (code / native / English registry name / script alias).
inline constexpr std::uint8_t kAliasNoLocale = 0xFF;

// One alias string of one language (generated data).
struct LanguageAliasAtom {
    const char* text;           // UTF-8 literal, process-lifetime storage
    AliasTier tier;
    std::uint8_t locale_index;  // index into LanguageAliasTable::locales, or kAliasNoLocale
};

// [first, first + count) slice of LanguageAliasTable::atoms for one language.
struct LanguageAliasGroup {
    const char* code;       // registry code, e.g. "KO", "ZH-CN", "AUTO"
    std::uint32_t first;
    std::uint32_t count;
};

// One folding pair: `codepoint` folds to `replacement` (ASCII, possibly
// empty = the codepoint is dropped). Sorted by codepoint (binary-searched).
struct LanguageAliasFoldEntry {
    char32_t codepoint;
    const char* replacement;
};

// The whole generated table, one extern symbol.
struct LanguageAliasTable {
    const char* const* locales;             // the 37 UI locales as CLDR ids
    std::size_t locale_count;
    const LanguageAliasAtom* atoms;         // grouped by language, registry order
    std::size_t atom_count;
    const LanguageAliasGroup* groups;       // registry order, contiguous slices
    std::size_t group_count;                // 90 = AUTO + 89 targets
    const LanguageAliasFoldEntry* fold;     // codepoint -> ASCII, sorted
    std::size_t fold_count;
};

// Defined in the generated TU (src/language_alias.cpp).
extern const LanguageAliasTable kLanguageAlias;

// Forward declaration: the fold is implemented below the detail helpers, which
// use it to build the lazy folded-atom cache.
inline std::string FoldLanguageAliasText(std::string_view utf8);

namespace language_alias_detail {

// Decodes one UTF-8 sequence starting at `pos` and advances `consumed`.
// Not a validating decoder: an invalid lead byte or a truncated/broken
// sequence decodes as the single raw byte (so a query and an alias containing
// the same malformed bytes still fold the same way; nothing else can match).
inline char32_t DecodeUtf8CodePoint(std::string_view text, std::size_t pos,
                                    std::size_t& consumed) {
    consumed = 1;
    const unsigned char lead = static_cast<unsigned char>(text[pos]);
    if (lead < 0x80u) {
        return lead;
    }
    std::size_t length = 0;
    char32_t value = 0;
    if ((lead & 0xE0u) == 0xC0u) {
        length = 2;
        value = lead & 0x1Fu;
    } else if ((lead & 0xF0u) == 0xE0u) {
        length = 3;
        value = lead & 0x0Fu;
    } else if ((lead & 0xF8u) == 0xF0u) {
        length = 4;
        value = lead & 0x07u;
    } else {
        return lead;
    }
    if (pos + length > text.size()) {
        return lead;
    }
    for (std::size_t i = 1; i < length; ++i) {
        const unsigned char next = static_cast<unsigned char>(text[pos + i]);
        if ((next & 0xC0u) != 0x80u) {
            return lead;
        }
        value = (value << 6) | (next & 0x3Fu);
    }
    consumed = length;
    return value;
}

inline const char* LookupFoldReplacement(char32_t codepoint) {
    std::size_t low = 0;
    std::size_t high = kLanguageAlias.fold_count;
    while (low < high) {
        const std::size_t mid = low + (high - low) / 2;
        if (kLanguageAlias.fold[mid].codepoint < codepoint) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    if (low < kLanguageAlias.fold_count &&
        kLanguageAlias.fold[low].codepoint == codepoint) {
        return kLanguageAlias.fold[low].replacement;
    }
    return nullptr;
}

// Code-point count of folded UTF-8 text (lead bytes only): the substring gate
// counts CHARACTERS, so a 2-character CJK query is gated exactly like "ab".
inline std::size_t CountCodePoints(std::string_view utf8) {
    std::size_t count = 0;
    for (char byte : utf8) {
        if ((static_cast<unsigned char>(byte) & 0xC0u) != 0x80u) {
            ++count;
        }
    }
    return count;
}

// The whole alias table folded ONCE, lazily (immutable cache; the magic-static
// init is thread-safe). Index-parallel to kLanguageAlias.atoms. This keeps a
// live filter keystroke allocation-free and O(alias bytes) once per query.
inline const std::vector<std::string>& FoldedAliasAtoms() {
    static const std::vector<std::string> kFolded = [] {
        std::vector<std::string> folded;
        folded.reserve(kLanguageAlias.atom_count);
        for (std::size_t i = 0; i < kLanguageAlias.atom_count; ++i) {
            const char* text = kLanguageAlias.atoms[i].text;
            folded.push_back(text == nullptr ? std::string() : FoldLanguageAliasText(text));
        }
        return folded;
    }();
    return kFolded;
}

inline const LanguageAliasGroup* FindAliasGroup(std::string_view code) {
    for (std::size_t i = 0; i < kLanguageAlias.group_count; ++i) {
        if (EqualsIgnoreCaseAscii(std::string_view{kLanguageAlias.groups[i].code}, code)) {
            return &kLanguageAlias.groups[i];
        }
    }
    return nullptr;
}

} // namespace language_alias_detail

// Folds one UTF-8 string into the comparison space shared by queries and
// aliases: ASCII case fold + diacritic fold (the generated NFD table covers
// Latin-1 / Latin Extended-A/B / Latin Extended Additional, so "Français" ->
// "francais" and "Tiếng Việt" -> "tieng viet") + Cyrillic and Greek
// transliteration ("Русский" -> "russkij", "Ελληνικά" -> "ellinika", so a
// Latin-letter query finds a non-Latin alias) + whitespace collapse/trim and
// zero-width/bidi-control removal. ASCII tab/CR/LF become ' ' here on the
// sub-0x80 fast path (PR4-04): the generated fold table starts at U+00A0 and
// is only consulted for codepoints >= 0x80, so a sub-0x80 table entry would
// be unreachable dead data - both this matcher and the generator's Python
// mirror fold ASCII whitespace inline. Code points with no fold entry (Han,
// Hangul, Thai, Devanagari, ...) pass through unchanged: those scripts are
// matched literally.
inline std::string FoldLanguageAliasText(std::string_view utf8) {
    std::string folded;
    folded.reserve(utf8.size());
    std::size_t pos = 0;
    while (pos < utf8.size()) {
        const std::size_t start = pos;
        std::size_t consumed = 1;
        char32_t codepoint = language_alias_detail::DecodeUtf8CodePoint(utf8, pos, consumed);
        pos += consumed;
        if (codepoint >= U'A' && codepoint <= U'Z') {
            folded.push_back(static_cast<char>(codepoint - U'A' + U'a'));
            continue;
        }
        if (codepoint < 0x80u) {
            if (codepoint == 0x09u || codepoint == 0x0Au || codepoint == 0x0Du) {
                folded.push_back(' ');
            } else {
                folded.push_back(static_cast<char>(codepoint));
            }
            continue;
        }
        const char* replacement = language_alias_detail::LookupFoldReplacement(codepoint);
        if (replacement != nullptr) {
            folded.append(replacement);
            continue;
        }
        folded.append(utf8.substr(start, consumed));
    }
    // Collapse whitespace runs and trim both ends: "  tieng   viet " and
    // "tieng viet" must fold to the same key (titles/typographic spaces in
    // CLDR names already folded to ' ' through the table).
    std::string normalized;
    normalized.reserve(folded.size());
    bool pending_space = false;
    for (char byte : folded) {
        if (byte == ' ') {
            pending_space = !normalized.empty();
            continue;
        }
        if (pending_space) {
            normalized.push_back(' ');
            pending_space = false;
        }
        normalized.push_back(byte);
    }
    return normalized;
}

// One language in a search result: the row plus WHY it matched (the picker
// may show the matched alias; P5 renders the language, not this struct).
struct LanguageSearchHit {
    const LanguageInfo* language;  // pool element (registry row, never null)
    AliasTier tier;                // best tier that matched (None for the empty query)
    AliasMatchKind match;          // best match kind within that tier
    const char* alias;             // the matching alias text, or null
};

// 261116_0003 P4 (plan D4): the searchable-picker matcher.
//
//   query - the user's typed text (UTF-8). Empty (or whitespace-only) means
//           "no filter": the pool comes back in pool order (registry order for
//           GetLanguagesForEngine views), every hit tier = None.
//   pool  - the language view to search, e.g. GetLanguagesForEngine(engine,
//           include_auto). ONLY pool rows are returned, so an engine-filtered
//           view can never leak a language that engine does not offer.
//
// Ranking (plan D4): (tier, match kind, pool order) - cheapest tuple first.
// Tier order is code > native > English > exonym > script alias; within one
// tier an EXACT match beats a PREFIX match beats a SUBSTRING match; remaining
// ties keep pool (= registry) order, so the result is deterministic.
// Substring matches are gated to queries of >= 3 folded code points (a 1-2
// character query would otherwise hit half the list).
//
// One row per language: the per-language scan keeps only the best-scoring
// alias, so a language can never appear twice.
//
// A pool row the generated table does not cover (a future user-defined
// language, P9) still matches through its own code / name_en / name_native -
// the fallback keeps the picker useful even if the data lags the registry.
inline std::vector<LanguageSearchHit> SearchLanguages(
        std::string_view query, const std::vector<const LanguageInfo*>& pool) {
    std::vector<LanguageSearchHit> hits;
    hits.reserve(pool.size());

    const std::string folded_query = FoldLanguageAliasText(query);
    if (folded_query.empty()) {
        for (const LanguageInfo* language : pool) {
            if (language == nullptr) {
                continue;
            }
            hits.push_back(LanguageSearchHit{language, AliasTier::None,
                                             AliasMatchKind::Exact, nullptr});
        }
        return hits;
    }

    const std::size_t query_codepoints = language_alias_detail::CountCodePoints(folded_query);
    const auto matches = [&](std::string_view folded_alias, AliasMatchKind& kind) {
        if (folded_alias.empty()) {
            return false;
        }
        if (folded_alias == folded_query) {
            kind = AliasMatchKind::Exact;
            return true;
        }
        if (folded_alias.size() > folded_query.size() &&
            folded_alias.compare(0, folded_query.size(), folded_query) == 0) {
            kind = AliasMatchKind::Prefix;
            return true;
        }
        if (query_codepoints >= 3 &&
            folded_alias.find(folded_query) != std::string_view::npos) {
            kind = AliasMatchKind::Substring;
            return true;
        }
        return false;
    };

    const std::vector<std::string>& folded_atoms = language_alias_detail::FoldedAliasAtoms();
    for (const LanguageInfo* language : pool) {
        if (language == nullptr) {
            continue;
        }
        bool found = false;
        AliasTier best_tier = AliasTier::None;
        AliasMatchKind best_kind = AliasMatchKind::Exact;
        const char* best_alias = nullptr;

        const LanguageAliasGroup* group = language_alias_detail::FindAliasGroup(language->code);
        if (group != nullptr) {
            for (std::uint32_t offset = 0; offset < group->count; ++offset) {
                const std::size_t index = static_cast<std::size_t>(group->first) + offset;
                if (index >= kLanguageAlias.atom_count) {
                    break; // defensive: a malformed generated slice must not overrun
                }
                AliasMatchKind kind = AliasMatchKind::Exact;
                if (!matches(folded_atoms[index], kind)) {
                    continue;
                }
                const AliasTier tier = kLanguageAlias.atoms[index].tier;
                if (!found || tier < best_tier ||
                    (tier == best_tier && kind < best_kind)) {
                    found = true;
                    best_tier = tier;
                    best_kind = kind;
                    best_alias = kLanguageAlias.atoms[index].text;
                }
            }
        } else {
            // Data-lag fallback: the row's own three forms, same tiers/ranks.
            const struct {
                const std::string* text;
                AliasTier tier;
            } fallback[] = {
                {&language->code, AliasTier::Code},
                {&language->name_en, AliasTier::English},
                {&language->name_native, AliasTier::Native},
            };
            for (const auto& candidate : fallback) {
                AliasMatchKind kind = AliasMatchKind::Exact;
                if (!matches(FoldLanguageAliasText(*candidate.text), kind)) {
                    continue;
                }
                if (!found || candidate.tier < best_tier ||
                    (candidate.tier == best_tier && kind < best_kind)) {
                    found = true;
                    best_tier = candidate.tier;
                    best_kind = kind;
                    best_alias = nullptr;
                }
            }
        }

        if (found) {
            hits.push_back(LanguageSearchHit{language, best_tier, best_kind, best_alias});
        }
    }

    std::stable_sort(hits.begin(), hits.end(),
                     [](const LanguageSearchHit& a, const LanguageSearchHit& b) {
                         if (a.tier != b.tier) {
                             return a.tier < b.tier;
                         }
                         return a.match < b.match;
                     });
    return hits;
}

} // namespace emebalachat
