#pragma once

// 261123_0001 ctx-pilot: the OPTIONAL local translate-context fragment builder.
//
// WHAT THIS IS
// The local engine (the shared host's ggml-translate worker, Hy-MT2) accepts a
// pre-rendered neighbor-context fragment on the OPTIONAL TranslateMsg.context
// wire member (engine commit b3b8e56; the host advertises the capability token
// "context_v1" in its welcome). Chat translates ONE line per Enter and keeps no
// memory of the previous lines, so a continuing conversation reaches the model
// as isolated sentences. This module turns the in-memory translation-history
// ring (src/translation_history.hpp) into the fragment the host expects: the
// newest same-window source/translation pairs, rendered in the Emebala Reader
// block format.
//
// FORMAT (mirrors Emebala_Reader native/emebala-ai/src/translation/prompt.rs
// render_context_items, so both apps feed the model the same shape):
//   <context>{previous source}</context>
//   <context_translation>{its translation}</context_translation>
// Pairs are joined by ONE '\n' (the Reader's render join) and listed in
// CHRONOLOGICAL order: the oldest kept pair first, the newest last, which is
// the order the user typed them. A pair whose translation is blank renders its
// <context> block only: that is the Reader's no-invention rule (a cache-miss
// neighbor never gains a fabricated translation), applied here to a history
// entry whose forward text is empty.
//
// SELECTION RULES
//   * SAME WINDOW ONLY. An entry is eligible only when its target_hwnd EQUALS
//     the requested window AND its recorded target_pid equals the window's
//     CURRENT owner. Entries with a null target_hwnd (the ring's "unknown
//     window" marker) are skipped, and a null requested window yields NO
//     fragment. The PID half of the rule is the repo's HWND-recycling guard
//     (translation_history.hpp F-09/F-27): the OS reuses handle values, so a
//     bare handle match could otherwise attribute a closed window's
//     conversation to a new window that inherited the value. Context must
//     never cross windows: that leak is the failure this rule prevents.
//   * Then the newest `max_pairs` eligible entries, then whole-pair trimming
//     from the OLDEST side until the rendered fragment fits `max_utf16_units`
//     UTF-16 code units (the unit the engine worker's own fragment budget
//     uses). A PAIR IS THE ATOM: a partial pair is never emitted, so a
//     <context> line can never arrive without its closing block, and an older
//     pair is never kept while a newer one was dropped.
//
// KNOWN RESIDUAL (261123_0001 ctx-fix CD-01: documented, accepted for the pilot)
// The PID half of the same-window rule cannot see a SAME-PROCESS handle reuse.
// If window A closes and its owning process later creates a window that
// receives A's recycled handle value, A's still-ringed entries satisfy both
// halves of the rule and are offered as window B's context. This is the
// accepted class of the pre-existing F-09/F-27 HWND-recycling scheme (the
// editor's apply path lives with the same residual), the impact is bounded to
// same-app, same-user, on-device context mixing, the pilot ships default-OFF,
// and the alternative would change the shared Entry contract mid-pilot.
// Future hardening (deferred, needs a user decision): a per-window generation
// serial (a SetPropW stamp captured with the entry and compared here, so a
// reused handle value fails the compare) or EVENT_OBJECT_DESTROY invalidation
// of that window's ring entries.
//
// SANITIZATION (the Reader's R2-P1-03 discipline)
// The pair texts are user content riding the prompt's instruction channel, so
// the four <context> block literals are neutralized before they are embedded:
// the ASCII angle brackets of a literal tag are rewritten to U+FF1C / U+FF1E
// (case-insensitive match, idempotent, clean text is byte-identical, the text
// stays visible). Without it a chat message containing "</context>" could break
// the block. Control tokens (the <|im_end|> family) and "[ID:" batch markers
// are deliberately NOT handled here: the engine worker already scrubs the
// former from the fragment with the loaded model's real special-token list and
// neutralizes the latter before the rung-1 prompt (ggml_translate_worker.cpp,
// FT-06 parity). Duplicating them here would split one rule across two owners.
//
// LINE-BREAK FLATTENING (261123_0001 ctx-fix CF-06)
// An entry's text can be MULTI-LINE: the drag / double-Ctrl+C producers record
// whole document text for live windows. Raw CR/LF inside the newline-delimited
// <context> block would change its line shape host-side (a mis-parse, not a
// privacy escalation). Every CR, LF and CRLF run is therefore rendered as ONE
// space before assembly, so an entry always occupies the pair's single line.
// Length is not preserved, so the budget is computed from the flattened text.
//
// PRIVACY CONTRACT (do not weaken)
//   * LOCAL ENGINE PATH ONLY. The fragment is handed to the local engine-host
//     pipe and nowhere else: engine.cpp builds it INSIDE the LocalLlama serving
//     leg, after the router has decided the request is local, and passes it to
//     engine_host::TryTranslate only. The Google and OpenAI cloud legs are
//     different code paths and never see it; the user_gguf route shares this
//     local leg and is therefore excluded explicitly by the caller's gate (a
//     user model carries an arbitrary chat template, so the product decision
//     keeps the pilot on the bundled engine). Wiring context into any of those
//     paths is a privacy change, not a plumbing change.
//   * MEMORY-ONLY SOURCE: the ring is in-memory and never persisted or logged
//     (translation_history.hpp contract). This builder adds no I/O, no cache
//     and no static state of its own; every call reads the ring under its own
//     mutex through translation_history::Recent().
//   * NEVER LOGGED: nothing here logs. A caller may log SHAPE only (the
//     fragment's unit count), never its text.
//   * The host re-scrubs and re-trims the fragment worker-side. This builder
//     keeps the wire payload small and the block shape intact; it is not the
//     security boundary itself.
//
// COVERAGE (261123_0001 ctx-fix CF-01: what is actually pinned, and where)
//   * THIS builder is pinned behaviorally by Test261123LocalContextBuilder
//     (tests/261123_followup_tests.inc): pair selection, oldest-side whole-pair
//     trimming, tag neutralization and the line-break flattening.
//   * The CLIENT-side capability half (JsonStringArrayHasToken, the
//     supports_context_v1 handshake/close lifecycle, and the emission drop
//     rule) lives in the engine repo's src/engine_host_client.cpp. It is
//     pinned by that same suite's SOURCE pins (the ctx-fix round), NOT by the
//     m6/a2 suites: those pin the engine-repo JSON-helper sync and the
//     engine.cpp call sites, and neither can observe the capability gate.
//
// Header-inline (the StrictLocalFailureStatus precedent in engine.hpp): pure
// string assembly over translation_history::Recent(), no new link edge and no
// CMake change, so the router and a unit suite can both call it.

#include <cstddef>
#include <string>
#include <string_view>
#include <utility> // std::move
#include <vector>

#include <windows.h> // HWND, GetWindowThreadProcessId

#include "translation_history.hpp"

namespace emebalachat {
namespace translation_context {

// Default fragment shape. Two pairs is the pilot's "the lines the user just
// typed" window (the analysis doc recommends 1-3 same-window previous lines).
// 2000 UTF-16 units is the SAME budget the engine worker enforces on the
// fragment (ggml_translate_worker.cpp kContextFragmentMaxUnits), so a fragment
// this builder emits is never trimmed again worker-side.
inline constexpr std::size_t kDefaultMaxPairs = 2;
inline constexpr std::size_t kDefaultMaxUtf16Units = 2000;

namespace detail {

// The Reader's render literals (prompt.rs render_context_items). Their sizes
// are the block overhead the pair budget below accounts for.
inline constexpr std::wstring_view kContextOpen = L"<context>";
inline constexpr std::wstring_view kContextClose = L"</context>";
inline constexpr std::wstring_view kTranslationOpen = L"<context_translation>";
inline constexpr std::wstring_view kTranslationClose = L"</context_translation>";

// The literal write order of a translated pair: [<context>text</context>]
// [LF][<context_translation>text</context_translation>].
inline constexpr wchar_t kPairLineBreak = L'\n';
// The separator the Reader joins PAIRS with.
inline constexpr wchar_t kPairSeparator = L'\n';

// The tags neutralize_context_tags recognizes, LONGEST FIRST so a shorter tag
// can never consume the prefix of a longer one (the Reader's array order).
inline constexpr std::wstring_view kContextTagLiterals[] = {
    L"</context_translation>",
    L"<context_translation>",
    L"</context>",
    L"<context>",
};

// TRUE for empty and whitespace-only text (ASCII whitespace only, the repo's
// Rule A shape; the same predicate the coordinator's degenerate-output gate
// uses locally).
inline bool IsBlank(std::wstring_view text) {
    for (const wchar_t c : text) {
        if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n') return false;
    }
    return true;
}

// 261123_0001 ctx-pilot: neutralize <context> block literals so an embedded
// one cannot close the block or open a new one. Mirrors the Reader's
// neutralize_context_tags: an ASCII '<' that starts one of the four literals is
// replaced by U+FF1C, that literal's trailing '>' by U+FF1E, and the inner text
// is copied verbatim (so the payload stays readable). Case-insensitive match,
// idempotent (the replacement leaves no ASCII literal behind), and clean input
// is returned byte-identical. Length is preserved, so a caller's size budget
// stays valid across the call.
inline std::wstring NeutralizeContextTags(std::wstring_view input) {
    if (input.find(L'<') == std::wstring_view::npos) {
        return std::wstring(input);
    }
    std::wstring lowered(input);
    for (wchar_t& c : lowered) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    const std::wstring_view lowered_view(lowered);
    std::wstring out;
    out.reserve(input.size());
    std::size_t i = 0;
    while (i < input.size()) {
        if (input[i] == L'<') {
            bool matched = false;
            for (const std::wstring_view tag : kContextTagLiterals) {
                if (lowered_view.substr(i, tag.size()) == tag) {
                    out.push_back(L'\uFF1C');
                    out.append(input.substr(i + 1, tag.size() - 2));
                    out.push_back(L'\uFF1E');
                    i += tag.size();
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
        }
        out.push_back(input[i]);
        ++i;
    }
    return out;
}

// 261123_0001 ctx-fix (CF-06): flatten line breaks before embedding. A
// multi-line entry (the drag / double-Ctrl+C producers record whole document
// text for live windows) would otherwise put raw CR/LF inside the
// newline-delimited <context> block and change its line shape host-side. Each
// CR, LF and CRLF run becomes ONE space, so an entry always renders on the
// pair's single line. Length is NOT preserved (the budget below is recomputed
// from the returned text).
inline std::wstring SingleLine(std::wstring_view input) {
    std::wstring out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const wchar_t c = input[i];
        if (c == L'\r' || c == L'\n') {
            out.push_back(L' ');
            if (c == L'\r' && i + 1 < input.size() && input[i + 1] == L'\n') ++i;
            continue;
        }
        out.push_back(c);
    }
    return out;
}

// UTF-16 units one PAIR occupies once rendered, WITHOUT the separator that
// joins it to the next pair (the caller adds that).
inline std::size_t PairUnits(bool has_translation, std::size_t original_units,
                             std::size_t forward_units) {
    std::size_t units = kContextOpen.size() + original_units + kContextClose.size();
    if (has_translation) {
        units += 1 /*kPairLineBreak*/ + kTranslationOpen.size() + forward_units +
                 kTranslationClose.size();
    }
    return units;
}

} // namespace detail

// Build the fragment for ONE target window. Returns an EMPTY string when there
// is no context to send (unknown window, no eligible pair, or a zero budget),
// and an empty result is the documented "no context" signal: the caller then
// leaves the optional wire member off and the frame stays the frozen v1 shape.
//
// Pure apart from the ring read: no allocation outlives the call, no state is
// cached, and the result is a value the caller owns.
inline std::wstring BuildFragment(HWND target_hwnd,
                                  std::size_t max_pairs = kDefaultMaxPairs,
                                  std::size_t max_utf16_units = kDefaultMaxUtf16Units) {
    if (target_hwnd == nullptr || max_pairs == 0 || max_utf16_units == 0) {
        return {};
    }
    // The window's CURRENT owner (0 for a dead handle). The ring records the
    // same PID at Push time, so requiring equality is the HWND-recycling guard;
    // a dead handle can never be the target of a live paste and yields no
    // context.
    DWORD target_pid = 0;
    ::GetWindowThreadProcessId(target_hwnd, &target_pid);
    if (target_pid == 0) {
        return {};
    }

    // Newest-first walk (Recent() is newest first). Blocks accumulate newest
    // first; trimming is a STOP, never a rewrite, which is what keeps a whole
    // pair the smallest unit that can be dropped.
    std::vector<std::wstring> blocks; // newest first
    std::size_t used_units = 0;
    for (const translation_history::Entry& entry : translation_history::Recent()) {
        if (blocks.size() >= max_pairs) break;
        if (entry.target_hwnd == nullptr || entry.target_hwnd != target_hwnd) continue;
        if (entry.target_pid == 0 || entry.target_pid != target_pid) continue;
        if (detail::IsBlank(entry.original)) continue;

        // 261123_0001 ctx-fix (CF-06): flatten line breaks BEFORE measuring, so
        // the budget below counts exactly the text that is embedded.
        const std::wstring original =
            detail::SingleLine(detail::NeutralizeContextTags(entry.original));
        const bool has_translation = !detail::IsBlank(entry.forward);
        const std::wstring forward =
            has_translation
                ? detail::SingleLine(detail::NeutralizeContextTags(entry.forward))
                : std::wstring();
        std::wstring block;
        block.reserve(detail::PairUnits(has_translation, original.size(), forward.size()));
        block.append(detail::kContextOpen);
        block.append(original);
        block.append(detail::kContextClose);
        if (has_translation) {
            block.push_back(detail::kPairLineBreak);
            block.append(detail::kTranslationOpen);
            block.append(forward);
            block.append(detail::kTranslationClose);
        }

        const std::size_t joined_units =
            block.size() + (blocks.empty() ? std::size_t{0} : std::size_t{1} /*kPairSeparator*/);
        if (used_units + joined_units > max_utf16_units) {
            // This candidate is the OLDEST pair considered so far and it does
            // not fit, so the kept set stays the newest CONTIGUOUS run. The
            // break (never a continue) is what holds the rule that an older
            // pair is never kept while a newer one was dropped; pair sizes
            // vary, so a smaller older pair might have fitted here, and
            // admitting it would skip a line the user typed BETWEEN two pairs
            // the fragment kept.
            break;
        }
        used_units += joined_units;
        blocks.push_back(std::move(block));
    }
    if (blocks.empty()) {
        return {};
    }

    std::wstring fragment;
    fragment.reserve(used_units);
    for (std::size_t i = blocks.size(); i-- > 0;) {
        if (!fragment.empty()) fragment.push_back(detail::kPairSeparator);
        fragment.append(blocks[i]);
    }
    return fragment;
}

} // namespace translation_context
} // namespace emebalachat
