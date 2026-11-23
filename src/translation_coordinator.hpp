#pragma once

// ---------------------------------------------------------------------------
// translation_coordinator — V10F (261122_0002 P2-B, L5-B): the ONE client-side
// chunk coordinator for long-form translation.
//
// Why it exists (verified against the shipped sources):
//   * The manager's local leg hands the WHOLE source to ONE engine-host pipe
//     request (src/engine.cpp: `ToUtf8(text)` in the TryTranslate frame). The
//     frozen host protocol caps a frame body at 1 MiB
//     (engine-repo engine_host_protocol.hpp kMaxFrameBytes = 1 << 20, and a
//     larger length prefix is answered bad_request), so a large selection
//     (~>768 KiB of JSON-escaped source) could never be served locally: the
//     request failed PERMANENTLY and the app fell through to the consented
//     cloud leg or the honest failure modal.
//   * The cloud clients chunk internally at ~1,200 UTF-16 units, but they
//     stitch the whole answer before the caller sees anything
//     (all-or-nothing), and a chunk loop failure throws away every chunk that
//     already succeeded.
//
// The coordinator is a PURE core (STL only + this repo's unicode_utils
// conversion declarations; no Win32, no network, no llama), so the frame
// planning, the rolling-glossary pair protocol, the partial-result rule and
// the cancel checks are all unit-testable headlessly:
//   (a) FRAME SIZING — no frame this coordinator emits exceeds
//       kCoordinatorMaxFrameBytes of JSON-ESCAPED source (768 KiB), which is
//       the 1 MiB wire cap minus ~25% headroom; splits never land inside a
//       UTF-8 sequence and prefer a newline boundary near the cut.
//   (b) ROLLING GLOSSARY PAIRS — every sink call receives the PREVIOUS
//       frame's source text and the previous frame's TRANSLATED text, so a
//       sink that can carry terminology context (a prompt prefix, a glossary
//       hint) has it without re-deriving it; frame 0 receives two empty
//       strings.
//   (c) PARTIAL-RESULT PRESERVATION — with opts.preserve_partial (the
//       TranslationTaskContext::on_chunk_text opt-in) a failed frame k returns
//       the stitched text of frames 0..k-1 plus failed_index = k instead of
//       discarding the work; without the opt-in the legacy all-or-nothing
//       shape is returned (empty text) so a non-preview caller cannot start
//       pasting half a document.
//   (d) CANCEL — the cancel/abort seams are polled BETWEEN frames (the same
//       boundary rule as the cloud chunk loops: cancel outranks everything)
//       and a canceled run stops calling the sink immediately.
//   (e) PER-FRAME VALIDATION — CoordinatorChunkLooksDegenerate replaces any
//       reliance on a whole-STITCHED output gate for coordinator-driven
//       paths: each frame's answer is judged on its own text (empty for
//       non-blank source, runaway expansion, a consecutive identical-line
//       loop), so a degenerate frame fails THAT frame and the partial rule
//       decides what the caller still gets.
//
// Implementation note: the definitions are inline in this header on purpose —
// the core is pure and must be usable from the tests without a new link edge
// or a CMake change. src/translation_coordinator.cpp is the contract/
// static-assert TU for the same header (it defines no symbols, so adding it to
// a target is optional and harmless).
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "translation_progress.hpp"
#include "unicode_utils.hpp" // ToUtf16 (pure conversion helper, Emebalachat_core)

namespace emebalachat {

// The frame budget, measured in JSON-ESCAPED source bytes. The frozen host
// wire cap is 1 MiB (engine_host_protocol.hpp kMaxFrameBytes); 768 KiB leaves
// room for the ~200-byte request envelope, the src/tgt/model_id members and
// any escaping this budget did not model.
inline constexpr std::size_t kCoordinatorMaxFrameBytes = 768u * 1024u;
static_assert(kCoordinatorMaxFrameBytes == 786432,
              "L5-B: the coordinator frame budget is exactly 768 KiB");
static_assert(kCoordinatorMaxFrameBytes < (1u << 20),
              "L5-B: the frame budget must stay under the frozen 1 MiB wire cap");

// Worst-case JSON escape cost of ONE source byte (the \u00XX form). Used as a
// cheap, allocation-free pre-check: a source that fits
// kCoordinatorMaxFrameBytes / kCoordinatorMaxEscapeFactor UTF-16 units can
// never need a split even in the pathological all-control-characters case.
inline constexpr std::size_t kCoordinatorMaxEscapeFactor = 6;

// No frame planned, or none failed.
inline constexpr std::size_t kCoordinatorNoFrame = static_cast<std::size_t>(-1);

// One source frame: the text to translate plus the delimiter bytes that follow
// it in the SOURCE (the chunker's contract: text + trailing_delimiter
// concatenated over all frames reproduces the input byte-for-byte).
struct CoordinatorChunk {
    std::string text;
    std::string trailing_delimiter;
};

// The escape cost of one source byte, mirroring the frozen host JsonEscape
// exactly (engine-repo engine_host_protocol.hpp): quote and backslash and the
// five named control characters cost 2 bytes, any other C0 control character
// costs 6 ("\u00xx"), everything else costs 1.
inline std::size_t CoordinatorEscapedCostOfByte(char c) {
    switch (c) {
        case '"':
        case '\\':
        case '\b':
        case '\f':
        case '\n':
        case '\r':
        case '\t':
            return 2;
        default:
            return static_cast<unsigned char>(c) < 0x20u ? 6u : 1u;
    }
}

// Exact JSON-escaped byte length of `utf8` (see the per-byte table above). The
// frame planner budgets against THIS value, not the raw byte count, so a
// quote-heavy or newline-heavy source cannot slip past the wire cap.
inline std::size_t CoordinatorJsonEscapedBytes(std::string_view utf8) {
    std::size_t total = 0;
    for (const char c : utf8) {
        total += CoordinatorEscapedCostOfByte(c);
    }
    return total;
}

// Byte length of the code point starting at s[i] (1..4; 1 for a stray
// continuation byte or a truncated sequence, so a split can never land inside
// a UTF-8 sequence).
inline std::size_t CoordinatorUtf8CodePointLenAt(std::string_view s, std::size_t i) {
    if (i >= s.size()) return 0;
    const auto b = static_cast<unsigned char>(s[i]);
    if (b < 0x80u) return 1;
    std::size_t want;
    if ((b & 0xE0u) == 0xC0u) {
        want = 2;
    } else if ((b & 0xF0u) == 0xE0u) {
        want = 3;
    } else if ((b & 0xF8u) == 0xF0u) {
        want = 4;
    } else {
        return 1; // stray continuation byte: consume exactly one byte
    }
    std::size_t k = 1;
    while (k < want && i + k < s.size() &&
           (static_cast<unsigned char>(s[i + k]) & 0xC0u) == 0x80u) {
        ++k;
    }
    return k;
}

// Largest prefix of `s` whose JSON-escaped size is <= limit and that ends on a
// code-point boundary.
inline std::size_t CoordinatorEscapedPrefixBytes(std::string_view s, std::size_t limit) {
    std::size_t used = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t len = CoordinatorUtf8CodePointLenAt(s, i);
        std::size_t cost = 0;
        for (std::size_t k = 0; k < len; ++k) {
            cost += CoordinatorEscapedCostOfByte(s[i + k]);
        }
        if (cost > limit - used) break; // used <= limit holds on entry
        used += cost;
        i += len;
    }
    return i;
}

// Shift a hard cut BACK to the last newline inside the trailing quarter of the
// window, so paragraph/line structure survives whenever it is available at no
// meaningful size cost. Returns `take` unchanged when no newline is there (or
// when the whole remainder fits).
inline std::size_t CoordinatorPreferredBreak(std::string_view s, std::size_t take) {
    if (take == 0 || take >= s.size()) return take;
    const std::size_t window = take / 4;
    const std::size_t floor_pos = take - window;
    for (std::size_t p = take; p > floor_pos; --p) {
        if (s[p - 1] == '\n') return p; // break AFTER the newline
    }
    return take;
}

// Plan the frames for a list of source chunks: every emitted frame's
// JSON-escaped text is <= max_escaped_bytes, splits are UTF-8 safe and prefer
// a newline, and the (text, trailing_delimiter) join over the result still
// reproduces each input chunk byte-for-byte. A chunk that already fits (and
// every empty — delimiter-only — chunk) passes through unchanged.
inline std::vector<CoordinatorChunk> PlanCoordinatorFrames(
    std::vector<CoordinatorChunk> chunks,
    std::size_t max_escaped_bytes = kCoordinatorMaxFrameBytes) {
    const std::size_t budget =
        max_escaped_bytes == 0 ? kCoordinatorMaxFrameBytes : max_escaped_bytes;
    std::vector<CoordinatorChunk> out;
    out.reserve(chunks.size());
    for (CoordinatorChunk& chunk : chunks) {
        if (chunk.text.empty() || CoordinatorJsonEscapedBytes(chunk.text) <= budget) {
            out.push_back(std::move(chunk));
            continue;
        }
        const std::string delimiter = chunk.trailing_delimiter;
        std::string_view rest(chunk.text);
        while (!rest.empty()) {
            std::size_t take = CoordinatorEscapedPrefixBytes(rest, budget);
            if (take == 0) {
                // A sub-code-point budget still makes progress: one code point.
                take = CoordinatorUtf8CodePointLenAt(rest, 0);
            }
            take = CoordinatorPreferredBreak(rest, take);
            out.push_back(CoordinatorChunk{std::string(rest.substr(0, take)), std::string()});
            rest.remove_prefix(take);
        }
        out.back().trailing_delimiter = delimiter;
    }
    return out;
}

// Translate ONE frame. Returns std::nullopt on failure (the coordinator owns
// the decision — see the partial rule). `prev_source` / `prev_target` are the
// previous frame's source text and translated text (both empty for frame 0),
// the rolling glossary pair (contract (b)).
using CoordinatorSink = std::function<std::optional<std::wstring>(
    const std::wstring& source_chunk,
    const std::wstring& prev_source,
    const std::wstring& prev_target)>;

struct CoordinatorOptions {
    std::size_t max_frame_bytes = kCoordinatorMaxFrameBytes;
    // Contract (c): return the stitched prefix instead of an empty string when
    // a frame fails. The engine sets this from PartialResultsOptIn(ctx) — the
    // preview sink's presence.
    bool preserve_partial = false;
    // Contract (d): the two cancel seams. Both are polled BETWEEN frames,
    // before the sink is called, and a set flag stops the run.
    const std::atomic<bool>* cancel = nullptr;
    const std::atomic<bool>* abort = nullptr;
    // Progress tick for frame i+1 of total (the badge's counter form). Optional.
    std::function<void(std::size_t index_1based, std::size_t total)> on_chunk_begin;
    // Contract (c) / L3-B preview: the CUMULATIVE stitched translation after
    // every completed frame (never a delta). Optional.
    std::function<void(const std::wstring& cumulative)> on_chunk_text;
};

struct CoordinatorResult {
    std::wstring text;                        // stitched (partial when allowed)
    std::size_t total_frames = 0;             // frames planned
    std::size_t completed_frames = 0;         // frames that produced text
    std::size_t failed_index = kCoordinatorNoFrame; // frame that failed (npos = none)
    bool canceled = false;                    // a cancel/abort seam stopped the run
    bool partial = false;                     // text is a prefix, not the whole answer
};

// Drive the frame list through `sink` under the contracts above. An empty plan
// or an empty sink is a no-op success (no frames, no failure) — never a
// Canceled/EngineFailed attribution the caller did not ask for.
inline CoordinatorResult RunTranslationCoordinator(std::vector<CoordinatorChunk> source_chunks,
                                                   const CoordinatorSink& sink,
                                                   const CoordinatorOptions& opts = {}) {
    CoordinatorResult result;
    const std::vector<CoordinatorChunk> frames =
        PlanCoordinatorFrames(std::move(source_chunks), opts.max_frame_bytes);
    result.total_frames = frames.size();
    if (!sink || frames.empty()) {
        return result;
    }

    std::wstring stitched;
    std::wstring prev_source;
    std::wstring prev_target;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        // Contract (d): the boundary check runs BEFORE the frame's request, so
        // a cancel posted between frames never starts another one.
        if (ChunkLoopCancelRequested(opts.cancel) || ChunkLoopAbortRequested(opts.abort)) {
            result.canceled = true;
            break;
        }
        if (opts.on_chunk_begin) {
            opts.on_chunk_begin(i + 1, frames.size());
        }
        const std::wstring source = ToUtf16(frames[i].text);
        std::optional<std::wstring> part = sink(source, prev_source, prev_target);
        if (!part) {
            result.failed_index = i;
            break;
        }
        stitched += *part;
        stitched += ToUtf16(frames[i].trailing_delimiter);
        ++result.completed_frames;
        prev_source = source;
        prev_target = std::move(*part);
        // Contract (c)/L3-B: the preview always carries the CUMULATIVE text.
        if (opts.on_chunk_text) {
            opts.on_chunk_text(stitched);
        }
    }

    const bool incomplete = result.canceled || result.failed_index != kCoordinatorNoFrame;
    if (incomplete && !opts.preserve_partial) {
        // The legacy all-or-nothing shape: a caller without a preview surface
        // must not receive a truncated document.
        result.text.clear();
        return result;
    }
    result.text = std::move(stitched);
    result.partial = incomplete && !result.text.empty();
    return result;
}

// ---- per-frame output validation (contract (e)) ---------------------------

// A "runaway" answer: more than kCoordinatorRunawayRatio times the source's
// length plus a fixed slack. A decode loop blows far past this ratio; a real
// translation (even a CJK->UTF-16 expansion) stays inside it.
inline constexpr std::size_t kCoordinatorRunawayRatio = 8;
inline constexpr std::size_t kCoordinatorRunawaySlackUnits = 512;
// A "line loop": this many CONSECUTIVE identical non-empty lines.
inline constexpr std::size_t kCoordinatorRepeatedLineMinCount = 3;

// TRUE when `text` carries kCoordinatorRepeatedLineMinCount consecutive
// identical non-empty lines (ASCII-space-trimmed; the repo's Rule A shape).
inline bool CoordinatorHasRepeatedLineRun(std::wstring_view text) {
    std::size_t run = 0;
    std::wstring previous;
    std::size_t line_start = 0;
    while (line_start <= text.size()) {
        std::size_t line_end = text.find(L'\n', line_start);
        if (line_end == std::wstring_view::npos) line_end = text.size();
        std::wstring_view line = text.substr(line_start, line_end - line_start);
        while (!line.empty() && (line.front() == L' ' || line.front() == L'\t' ||
                                 line.front() == L'\r')) {
            line.remove_prefix(1);
        }
        while (!line.empty() && (line.back() == L' ' || line.back() == L'\t' ||
                                 line.back() == L'\r')) {
            line.remove_suffix(1);
        }
        if (!line.empty()) {
            const std::wstring current(line);
            run = (current == previous) ? run + 1 : 1;
            previous = current;
            if (run >= kCoordinatorRepeatedLineMinCount) {
                return true;
            }
        }
        if (line_end >= text.size()) break;
        line_start = line_end + 1;
    }
    return false;
}

// Client-side, per-frame replacement for the engine repo's whole-STITCHED
// repetition re-gate (which stays in the worker, per job, unchanged): a frame
// whose answer is empty for real source text, expands like a loop, or repeats
// one line consecutively is treated as that frame's failure. Pure and
// model-agnostic; never logs content.
inline bool CoordinatorChunkLooksDegenerate(std::wstring_view source,
                                            std::wstring_view target) {
    const auto is_blank = [](std::wstring_view s) {
        for (const wchar_t c : s) {
            if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n') return false;
        }
        return true;
    };
    // (1) an empty answer for a non-blank source is a failure, never a
    // translation (the clients' own empty-means-failure contract).
    if (is_blank(target)) {
        return !is_blank(source);
    }
    // (2) runaway expansion.
    if (target.size() >
        kCoordinatorRunawayRatio * source.size() + kCoordinatorRunawaySlackUnits) {
        return true;
    }
    // (3) a consecutive-line loop, with the repo's source guard: a run the
    // SOURCE itself carries is the user's content, never a decode loop.
    if (!CoordinatorHasRepeatedLineRun(target)) {
        return false;
    }
    return !CoordinatorHasRepeatedLineRun(source);
}

} // namespace emebalachat
