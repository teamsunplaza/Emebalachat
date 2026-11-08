#pragma once

// ---------------------------------------------------------------------------
// translation_common — REQ-043 (plan §5.1-1): the translation-only core of the
// local Hy-MT2 engine, extracted from src/engine.cpp's private
// TranslationManager::LlamaEngine so that BOTH the shared inference host
// (Emebala.Engine.exe, src/host_main.cpp) and the M6-removed in-app path ran
// BYTE-IDENTICAL inference logic with zero duplication.
// REQ-043 (M6 T1, design §4.2/D-1): this pair now lives in src/engine_core/
// inside the Emebalachat_engine_core static library (physical move only).
// REQ-043 (M6 fold2): the in-app embedded inference path has since been
// REMOVED (no-llama app exe; the shared host is the only local serving
// source, plan §V2-8.6) — this header documents the extraction move only.
//
// What moved here VERBATIM from engine.cpp (bodies unchanged, comments kept):
//   * kPenaltyLastN + its compile-time pin
//   * the REQ-R01 context-budget static_asserts
//   * LlamaAbortIfCanceled / LlamaLoadProgress (REQ-R16 abort/progress seams)
//   * the P7-F2 split_mode enum-layout static_assert
//   * SetGpuOffloadParams (definition; declaration stays in engine.hpp)
//   * TranslationManager::LlamaEngine (both the HAVE_LLAMA_CPP implementation
//     and the no-llama stub), renamed LocalInferenceEngine
//
// Deliberately NOT moved (still shared through Emebalachat_core): the pure
// helpers declared in engine.hpp — TruncateHeadTailWindow, ScrubControlTokenTexts,
// CollectControlTokenTexts, IsValidModelPath, VerifyModelSha256,
// kExpectedModelSha256 — plus BuildPrompt (config.cpp) and NormalizeNFC /
// ToUtf8 / ToUtf16 (unicode_utils.cpp). The host links Emebalachat_core, so it
// resolves the same objects; nothing is duplicated.
//
// Cancellation contract (unchanged from the pre-extraction code): the engine
// reads *cancel_flag between decode steps and from llama.cpp's abort callback
// (CPU chunks). REQ-051 U-1 FIX 2 splits the MODEL-LOAD leg onto its own
// load_cancel_flag (default: aliases cancel_flag, so legacy callers are
// byte-identical): the worker points it at a never-set atomic so a per-job
// abort unwinds the DECODE but never discards a multi-minute load (the
// never-converging load spiral). Callers serialize inference; both flag
// addresses must stay stable for the engine's lifetime (TranslationManager
// wires its own latch; the host wires one fixed per-request atomic — see
// host_main.cpp).
//
// REQ-051 U-1 FIX 1: the decode loop additionally carries a wall-clock budget
// (kDecodeWallClockBudgetMs) and an input-scaled generation cap
// (ScaledMaxGenTokens): a degenerate loop on a user GGUF stops sampling at the
// budget and reports exhaustion through decode_wall_clock_exhausted() so the
// worker can answer the frozen "timeout" code (transient -> client retry).
//
// RT-C (260926_0003, task RT-C "D안-개선"): EnsureLoaded's FIRST load leg is
// decided by the VRAM auto gate (engine_core_helpers.hpp:
// kMtGpuFreeVramThresholdBytes / ParseMtGpuOverride / DecideMtGpuOffload /
// QueryMtAdapterFreeVramBytes), evaluated once per process and cached: free
// local VRAM >= 1.5 GiB -> GPU offload (unchanged P7-F2 behavior), below ->
// the CPU leg runs DIRECTLY (no CUDA context initialized, so a co-resident
// Listener ASR worker keeps its VRAM on 8 GB-class cards). The env var
// EMEBALA_MT_GPU=1/0 forces GPU/CPU over the auto decision; a forced GPU load
// on a driverless machine still degrades through the existing CPU retry
// below. The gate never changes the wire protocol — worker manifest, spawn
// command line and frame vocabulary are untouched (the override reaches the
// worker through the inherited process environment; see
// ggml_translate_worker.cpp).
//
// At extraction time the in-app path's runtime behavior was UNCHANGED by the
// move: TranslationManager::LlamaEngine was a zero-member derived class
// (engine.cpp), forwarding every call to this implementation verbatim. That
// in-app class was later deleted outright in M6 — only the worker-side copy
// remains live now.
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(HAVE_LLAMA_CPP) || __has_include("llama.h")
#ifndef HAVE_LLAMA_CPP
#define HAVE_LLAMA_CPP 1
#endif
#include "llama.h"
#endif

// REQ-051 U-1 FIX 1: the input-scaled generation cap clamps against
// kLlamaGenReserve (the REQ-R01 budget constant lives in this library's
// llama-independent helper header; re-exported through engine.hpp for the
// existing consumers).
#include "engine_core_helpers.hpp"

namespace emebalachat {

// REQ-051 U-1 FIX 1 (live bug U-1, user-GGUF serving intermittency): the
// decode wall-clock budget. A healthy Hy-MT2 translation finishes in
// ~78-171 ms warm / <= ~2 s worst case; a degenerate non-Hy-MT2 loop under
// VRAM contention burned the ENTIRE 2048-token generation reserve at
// ~14 ms/token (~28.5 s), colliding with the frozen 30 s client budget.
// 12 s is generous for every legitimate translation (even a 10x-thrashed
// decode gets ONE honest timeout + the client's one-shot transient retry
// instead of a 30 s freeze): 12 s + 400 ms backoff + 12 s ~= 25 s stays
// inside the frozen 30 s budget.
inline constexpr int kDecodeWallClockBudgetMs = 12000;

// REQ-059 perf (live: MiLM long paragraphs slow/failing): input-scaled
// decode wall-clock budget. The flat 12 s REQ-051 budget was sized for
// Hy-MT2-1.8B (healthy <= ~2 s); MiLM-class 4B user models decode ~2x+
// slower per token, so a long LEGITIMATE translation could exceed 12 s,
// be discarded -> "timeout" -> the one-shot retry exhausts -> the user
// sees a failure on long paragraphs. The budget now scales with the
// generation cap at a worst-case-assumed 50 tok/s (20 ms/token) —
// clamp(max_gen_tokens * 20, kDecodeWallClockBudgetMs /*12 s floor, this
// named constant is UNTOUCHED*/, kDecodeWallClockBudgetMaxMs). The 12 s
// floor holds while max_gen_tokens <= 600 (prompt n <= 118); long
// translations (both models) scale up to 27 s, staying under the frozen
// 30 s v1 client budget including the one-shot retry headroom. The
// REQ-051 U-1 latch semantics (timeout wire code on exhaust) are
// unchanged.
inline constexpr int kDecodeMsPerAssumedToken = 20;
inline constexpr int kDecodeWallClockBudgetMaxMs = 27000;

// Backend P1-3 (v0.10.5 handoff section 4.4 item 3): the over-budget paragraph
// chunking path runs the per-chunk attempt ladder with a FRESH decode budget
// per chunk, so N chunks could accumulate N x (up to 27 s) and blow the
// frozen 30 s host/client IPC timeout. The whole chunked request therefore
// carries ONE accumulated deadline from the request start: every chunk's
// decode deadline is clamped to it, and the chunk loop refuses to start a
// new chunk past it. 28 s leaves ~2 s of headroom for IPC + stitching.
inline constexpr int kChunkedRequestBudgetMs = 28000;

// REQ-059 perf: budget_ms = clamp(max_gen_tokens * kDecodeMsPerAssumedToken,
// kDecodeWallClockBudgetMs, kDecodeWallClockBudgetMaxMs). Pure/constexpr so
// the unit suite pins the clamp headlessly (mirrors ScaledMaxGenTokens).
constexpr int ScaledDecodeWallClockBudgetMs(int max_gen_token_count) {
    const long long scaled =
        static_cast<long long>(max_gen_token_count) * kDecodeMsPerAssumedToken;
    if (scaled < kDecodeWallClockBudgetMs) {
        return kDecodeWallClockBudgetMs;
    }
    if (scaled > kDecodeWallClockBudgetMaxMs) {
        return kDecodeWallClockBudgetMaxMs;
    }
    return static_cast<int>(scaled);
}

// REQ-051 U-1 FIX 1: input-scaled generation cap constants. Translation
// outputs beyond (4x input + 128) tokens do not legitimately exist; the cap
// is floored at 256 (a one-token prompt still deserves a full sentence) and
// clamped at kLlamaGenReserve (2048 stays the named ceiling constant — the
// unit-test pins and the REQ-R01 static_asserts are untouched).
inline constexpr int kScaledGenFloorTokens = 256;
inline constexpr int kScaledGenPerInputToken = 4;
inline constexpr int kScaledGenFlatTokens = 128;

// REQ-051 U-1 FIX 1: max_gen_tokens = min(kLlamaGenReserve, max(256, 4n+128)).
// Pure/constexpr so the unit suite can pin the whole matrix headlessly.
// Overflow-safe: the multiply happens only after the ceiling crossover
// short-circuit, so adversarial token counts can never reach it.
constexpr int ScaledMaxGenTokens(int input_token_count) {
    if (input_token_count <= 0) {
        return kScaledGenFloorTokens;
    }
    constexpr int kCeilingCrossover =
        (kLlamaGenReserve - kScaledGenFlatTokens) / kScaledGenPerInputToken; // 480
    if (input_token_count >= kCeilingCrossover) {
        return kLlamaGenReserve;
    }
    const int scaled = input_token_count * kScaledGenPerInputToken + kScaledGenFlatTokens;
    return scaled < kScaledGenFloorTokens ? kScaledGenFloorTokens : scaled;
}

// RC-4 (v0.10.5 handoff section 2.5): short-input generation cap. The
// historical floor hands every input at least kScaledGenFloorTokens (256)
// generation tokens, and the caller passes the WHOLE formatted prompt token
// count (template wrapper included), so a one-word translation carried a
// 256-token runway for hallucinated essays. A short single-line source
// (<= 16 UTF-8 bytes, no line break) can only ever produce a word or one
// sentence, so its budget is capped to 64 tokens. WS1 (261108_0002) narrows
// that further for a source that is ONE word (see
// kSingleWordSourceGenCapTokens below). kLlamaGenReserve is the
// "no additional constraint" sentinel: the caller takes the lesser of
// ScaledMaxGenTokens and this cap, so longer inputs keep their historical
// budget byte-identically. Pure/constexpr so the unit suite pins it
// headlessly (mirrors ScaledMaxGenTokens).
inline constexpr std::size_t kShortSourceMaxUtf8Bytes = 16;
inline constexpr int kShortSourceGenCapTokens = 64;
// WS1 (261108_0002, WS1-gates): the SINGLE-WORD cap. The user report's
// hallucinations all start from ONE word ("cyber", "company", "astrology",
// "clue", "nature", "medicine"), and 64 tokens of runway is what lets a 1.8B
// model continue the word into a paragraph, a numbered quiz answer or a
// dialogue turn ("assistant: ..."). A whitespace-free <= 16-byte source is a
// single word, whose translation (measured on the shipped model: 1-10 code
// points, well under 10 tokens for every shipped pair) can never legitimately
// need more than 16 tokens. Multi-word short sources keep the RC-4 64-token
// cap. kLlamaGenReserve stays the "no additional constraint" sentinel.
inline constexpr int kSingleWordSourceGenCapTokens = 16;
constexpr int ShortSourceGenTokenCap(std::size_t source_utf8_bytes,
                                     bool source_has_line_break,
                                     bool source_has_whitespace = true) noexcept {
    if (source_has_line_break || source_utf8_bytes > kShortSourceMaxUtf8Bytes) {
        return kLlamaGenReserve;
    }
    return source_has_whitespace ? kShortSourceGenCapTokens : kSingleWordSourceGenCapTokens;
}

// ---------------------------------------------------------------------------
// R1-2 (session 261029_0001, v0.10.4 sweep): sampler shape for ONE decode
// attempt. Why: the shipped profile is temperature=0.0 / top_p=0.6 / top_k=20 /
// repetition_penalty=1.05 (worker_protocol.hpp sampling defaults, stamped on
// every job by the host), but the temp<=0.001 branch built a BARE greedy
// sampler, so the configured penalty was silently dropped — on out-of-
// distribution input a greedy 1.8B translator then degenerates into exact
// repetition loops (debug-specialist reports RC3 / C3). R1-2 builds
// penalties -> greedy when a penalty is configured. The decision is pure so
// the unit suite pins the whole matrix without a model, and the no-llama build
// keeps it (no llama types involved).
//
// Byte-identity contract: with rep_pen <= 1.0 the shape is BareGreedy — the
// historical single llama_sampler_init_greedy() allocation, unchanged. Only a
// rep_pen > 1.0 takes the chain, so every existing configuration that did not
// ask for a penalty keeps the exact pre-R1-2 token stream.
// top_k / top_p stay OUT of the greedy chain on purpose: both only prune
// lower-logit candidates (top_k keeps the k best; top_p with min_keep=1 always
// keeps the argmax), so at temperature 0 they are mathematically argmax-
// invariant — they cannot change one sampled token and would only add a
// per-token sort. Greedy determinism is what the REQ-059 golden pins rely on.
// ---------------------------------------------------------------------------
enum class SamplerShape {
    Dist,            // temperature > kGreedyTemperatureThreshold (unchanged chain)
    BareGreedy,      // greedy, no penalty configured (pre-R1-2 shape, byte-identical)
    PenalizedGreedy, // greedy with the configured repetition penalty (R1-2)
};

// The greedy branch boundary inherited from the pre-R1-2 code
// (`temperature <= 0.001f`); named so the production branch and the pins cannot
// drift apart.
inline constexpr float kGreedyTemperatureThreshold = 0.001f;

constexpr SamplerShape DecideSamplerShape(float temperature, float rep_pen) {
    if (temperature > kGreedyTemperatureThreshold) {
        return SamplerShape::Dist;
    }
    return rep_pen > 1.0f ? SamplerShape::PenalizedGreedy : SamplerShape::BareGreedy;
}

// ---------------------------------------------------------------------------
// R1-1 (session 261029_0001, v0.10.4 sweep): the output-validation ladder.
// Why: the pre-R1-1 acceptance predicate only rejected empty / exact-echo /
// bare-special-token outputs, so assistant-style prose, wrong-language essays
// and repetition loops reached the user with status=ok — the CEO's "assist
// symptom" (debug-specialist reports RC1/RC2/RC4, live repro
// "abcdefghijklmnopqrstuvwxyz" -> "Translation is not possible... Please
// contact us for further assistance..."). The gates below are MODEL-AGNOSTIC
// text predicates (no Hy-MT2 tokens, no chat-template assumptions, no model
// names), so they protect any current or future GGUF — that is the "works with
// ANY model" requirement. All three operate on the TRIMMED output of BOTH
// rungs of the REQ-059 ladder; a rung-1 rejection degrades to the completion
// form, and a rung-2 rejection answers std::nullopt (honest engine_failed)
// instead of shipping garbage.
//
// Script families come from the i18n language registry codes (kAllLanguages in
// config.cpp) and the codepoint predicates come from smart_bypass.cpp — no
// second, drifting script table. Unknown/AUTO targets fail OPEN (mask 0), so
// the gate can never reject on a language the registry does not know.
// ---------------------------------------------------------------------------

// R1-1 repetition thresholds, CALIBRATED in R2-6 (session 261029_0001,
// code-reviewer-mimo F-01). The line/token-run rule mirrors the debug reports'
// "repeated >= 3 times"; the suffix rule needs an 8-token minimum span.
//
// R2-6 calibration (why the envelope below, not the pre-R2-6 one):
//   * Rule A now requires CONSECUTIVE identical lines. The pre-R2-6 rule
//     counted a line repeated anywhere after it, so a transcript with "Yes."
//     three times spread across a dialogue was read as a loop.
//   * Rule B (one token repeated in a row) gained TWO guards, because a
//     three-token reduplication is ordinary human emphasis ("no no no",
//     "Help! Help! Help!", "ha ha ha"):
//       (a) SPAN FLOOR: the longest identical-token run must cover at least
//           kRepeatedTokenRunMinCoveragePercent of the output's whitespace
//           tokens. A degenerate one-token loop runs to the token cap, so its
//           coverage approaches 100%; an emphatic reduplication inside a real
//           sentence is a small fraction (3 of 11 tokens = 27%).
//       (b) SOURCE GUARD (same repo idiom as HasMetaLeakage): when the SOURCE
//           itself carries a run of kRepeatedTokenRunMinCount identical
//           whitespace tokens, the repetition is the user's own content, not a
//           loop - the two sides translate a reduplication into a
//           reduplication, so a token-identity match across a language pair is
//           impossible but the SHAPE match is exact.
//   * Threshold choice (justified with the pinned cases): 50% coverage keeps
//     "the cat sat on the mat and said no no no" (3/11 = 27%) accepted while a
//     cap-length one-token loop (100%) is still rejected. Any coverage floor
//     below 50% starts accepting a token run that occupies most of the answer;
//     100% would re-admit a mid-sentence emphasis once the model repeats four
//     tokens. The span floor alone cannot separate a three-token WHOLE output
//     ("no no no") from a three-token loop - only the source guard can, which
//     is why both guards are required.
inline constexpr int kRepeatedLineMinCount = 3;
inline constexpr int kRepeatedTokenRunMinCount = 3;
inline constexpr int kRepeatedTokenRunMinCoveragePercent = 50;
inline constexpr int kSuffixRepetitionMinRepeats = 3;
inline constexpr int kSuffixRepetitionMinSpanTokens = 8;
// A loop whose period exceeds this many whitespace tokens is out of the
// practical envelope: a whole 64+ token sentence repeated verbatim is caught
// by the line rule whenever it is line-separated, and the bound keeps the
// periodic scan cheap enough for the mid-decode probe below.
inline constexpr int kSuffixRepetitionMaxPeriodTokens = 64;
// The line rule scans at most this many trailing non-empty lines (a loop lives
// at the tail; the bound keeps the probe O(1)-ish on long multi-line outputs).
inline constexpr int kRepetitionMaxScannedLines = 256;

// R1-1 wrong-script boundary: the rule only applies above this many output
// code points, so a two-character legitimate answer is never gated.
inline constexpr std::size_t kWrongScriptMinOutputCodePoints = 2;

// R2-7 (session 261029_0001, code-reviewer-gemini F-05): the longest run of
// characters before a line-start ':' that still counts as a LABEL ("Source:",
// "영어:", "Target language:"). Longer than this, OR carrying sentence-ending
// punctuation before the colon, the line is not a label. The predicate is
// shape-only (R3, round-3 F-04): a short sentence that reaches its colon with
// no . ! ? before it ("The answer: 42") still qualifies, so the exemption is
// bounded by that shape - not by a claim that prose can never widen the guard.
inline constexpr std::size_t kColonLabelMaxCodeUnits = 32;

// R2-16 (session 261029_0001, CEO decision "언제나 잘 번역"; thresholds RULED by
// measurement in round 3 - see the ruling note at the end of this block): the
// FOURTH output rule, for an answer that is in the target script but mostly
// something else. Two independent shapes, both required to be extreme because
// a single quoted foreign name must never be rejected:
//   (i)  unexpected-script share >= kUnexpectedScriptSharePercent of the
//        output's code points AND the output is more than
//        kUnexpectedScriptLengthExpansionPercent/100 times the source length.
//        "Unexpected" = a script carried by NEITHER the target NOR the source.
//        The conjunction is what protects legit quoting: a foreign term inside
//        a long sentence is a small share, and a faithful translation does not
//        triple in length. Calibration data (measured against the live worker,
//        and pinned in the unit suite): the live KO->EN "음" essay is 977 code
//        points against 1 source code point (~977x) but only 6.3% of it is Han,
//        so rule (i) ALONE would not reject it - which is exactly why rule (ii)
//        exists; a legit Korean sentence quoting a Japanese title
//        (吾輩は猫である) measures 0.54x length expansion with a 20%
//        foreign-script share, so neither rule fires on it.
//   (ii) tiny-source amplification: a source of at most
//        kAmplifiedSourceMaxCodePoints code points answered with at least
//        kAmplifiedOutputMinCodePoints. The live residual is exactly this
//        shape ("음" -> a 977-code-point essay).
//        SOURCE-SHAPE CARVE-OUT, BOUNDED (R3, ceiling added by 261029_0001
//        FIX-D2): a source that is nothing but one unit repeated ("ㅋㅋㅋㅋ",
//        "kkkk", "haha", "no no no") is laughter / emphasis reduplication, and
//        a chatty model's laughter expansion of it is a legitimate translation.
//        Such a source is exempt from rule (ii) only UP TO the
//        laughter-legitimate ceiling kAmplifiedRepetitionOutputMinCodePoints
//        (120 code points); at or above it the rule applies again
//        (IsPureRepetitionSource). The ceiling is what keeps the carve-out
//        honest: a real laughter answer measures well under it (the longest
//        legitimate reduplication answer measured on the rebuilt worker is 19
//        code points: "no no no no no no" -> "아니, 아니, 아니, 아니, 아니."),
//        while the SAME sources are answered by the shipped model with
//        980-1110-code-point repeated-paragraph essays - nonsense output, not
//        laughter, which the CEO ruling "언제나 잘 번역" must never ship as `ok`.
//        The carve-out is narrow in the other direction too: it must cover the
//        WHOLE source. A token/line loop in the ANSWER is still caught earlier
//        by rules A/B; a NON-consecutive period-N prose loop is not covered by
//        them, which is exactly why the ceiling exists instead of a blanket
//        exemption (the R3 draft exempted those essays; FIX-D2 measured that
//        cost and closed it).
//
// THRESHOLD RULING (round-3 measured corpus ruling, recorded here so the
// numbers are not re-litigated from memory): the calibration corpus's longest
// legitimate short-source answer is 22 code points ("안녕" -> "Hello, nice to
// meet you."), so the previous 40-code-point floor left under 2x margin for a
// chattier model; the floor is 64, which keeps ~3x margin over the measured
// legitimate maximum while the live residual (977 code points) is still caught
// with a 15x margin. The repetition-source ceiling is 120 code points: 6x the
// longest legitimate reduplication answer measured on the rebuilt worker
// (19 code points) and still 8x BELOW the shortest measured nonsense essay
// (980 code points, i.e. 958 UTF-8 bytes for "kkkk").
// Rule (i) keeps the >3x length gate rather than the >10x number floated as a
// caution: the 30%-share conjunct already suppresses the quoting false positive
// the >10x number was protecting, and no case in the measured corpus sits
// between 3x and 10x. All three numbers are pinned by static_assert in the unit
// suite; changing any of them requires a new measurement.
inline constexpr int kUnexpectedScriptSharePercent = 30;
inline constexpr int kUnexpectedScriptLengthExpansionPercent = 300;
inline constexpr std::size_t kAmplifiedSourceMaxCodePoints = 4;
inline constexpr std::size_t kAmplifiedOutputMinCodePoints = 64;
// 261029_0001 FIX-D2: the ceiling of the pure-repetition source carve-out.
// Below it a laughter expansion of "ㅋㅋㅋㅋ" / "kkkk" / "haha" is accepted; at
// or above it the answer is no longer laughter (the measured essays are
// 980-1110 code points) and rule (ii) rejects it again.
inline constexpr std::size_t kAmplifiedRepetitionOutputMinCodePoints = 120;

// RC-2 (v0.10.5 handoff section 2.3): the two-tier tiny-source amplification
// guard. Tier 1 covers sources of at most kAmplifiedSourceMaxCodePoints code
// points (the pinned 4 stays UNCHANGED). Tier 2 covers 5..15 code point
// sources PLUS single-token (whitespace-free) sources of any length. The
// historical pinned constants (kAmplifiedSourceMaxCodePoints /
// kAmplifiedOutputMinCodePoints) keep their values (unit-suite
// static_asserts); the tier thresholds are separate constants.
//
// WS1 (261108_0002, WS1-gates) RE-CALIBRATION of both tiers. Why the RC-2
// numbers (Tier 1 = 32 cp at 10x, Tier 2 = 64 cp at 6x) missed the user
// report: the hallucinations for the reported single words measure 14-32 code
// points ("이메발라의 별자리 운세는 어떨까요?" = 19 for "astrology", "이메일은
// 어떻게 보내나요?" = 14, "이 문서는 Emebala의 출처를 기반으로 작성되었습니다." = 32
// for "cyber", "1. 당신은 어떤 방식으로 번역하나요" = 20), i.e. all UNDER the 64
// Tier-2 floor, so every one of them shipped as status=ok. The floors are now
// expressed against the SOURCE LENGTH the way the hallucination actually
// scales (a model that "explains" a word writes a sentence, not 6x the word):
//   * Tier 1 (1-4 code points): output >= max(16, 4x source). A 1-code-point
//     interjection answered with a 16-code-point sentence is already the
//     chatty shape; 16 is the smallest floor that leaves a real one-word
//     translation untouched (measured legit: "OK" -> "좋습니다." = 5,
//     "Hi" -> "안녕하세요!" = 6, "음" -> "네, 그렇습니다." = 9).
//   * Tier 2, SINGLE WORD (whitespace-free, <= 15 code points): output >=
//     max(20, 4x source). This is the user-report window: 20 code points is
//     ~2x the longest measured legit one-word answer and ~4x the shortest
//     reported hallucination; the 4x ratio keeps a longer word's proportional
//     answer (e.g. 14 cp -> 56 cp) legal.
//   * Tier 2, 5..15 code points WITH whitespace, and single words LONGER than
//     15 code points: the RC-2 64 cp at 6x envelope is UNCHANGED (a phrase
//     legitimately expands more than a word, and a 20+ code point single token
//     is a compound whose translation is correspondingly long).
// The pure-repetition laughter carve-out (above) is applied FIRST and is
// untouched: a laughter answer to "ㅋㅋㅋㅋ" still passes under its own ceiling.
inline constexpr std::size_t kAmplifiedTinyTier1MinOutputCodePoints = 16;
inline constexpr std::size_t kAmplifiedTinyTier1Ratio = 4;
inline constexpr std::size_t kAmplifiedTinyTier2SourceMaxCodePoints = 15;
inline constexpr std::size_t kAmplifiedTinyTier2Ratio = 6;
inline constexpr std::size_t kAmplifiedSingleWordOutputMinCodePoints = 20;
inline constexpr std::size_t kAmplifiedSingleWordRatio = 4;

// Script classes a text can carry. Bit flags so "target script present" is a
// single mask test; JA is the one multi-script target (Han|Kana).
enum class ScriptClass : std::uint32_t {
    None = 0u,
    Latin = 1u << 0,
    Hangul = 1u << 1,
    Han = 1u << 2,
    Kana = 1u << 3,
    Cyrillic = 1u << 4,
    Thai = 1u << 5,
    Arabic = 1u << 6,
    Devanagari = 1u << 7,
    Bengali = 1u << 8,
    Khmer = 1u << 9,
    Lao = 1u << 10,
    Hebrew = 1u << 11,
    Greek = 1u << 12,
    Myanmar = 1u << 13,
};

constexpr std::uint32_t ToMask(ScriptClass script) {
    return static_cast<std::uint32_t>(script);
}

// Language-registry code -> script mask (the codes are kAllLanguages' `code`
// column; the unit suite pins that EVERY registered target resolves non-zero,
// so adding a language without a script entry fails the build's test run).
struct LanguageScriptEntry {
    const char* code;
    std::uint32_t script_mask;
};

inline constexpr LanguageScriptEntry kLanguageScriptTable[] = {
    {"EN", ToMask(ScriptClass::Latin)},   {"VI", ToMask(ScriptClass::Latin)},
    {"ES", ToMask(ScriptClass::Latin)},   {"FR", ToMask(ScriptClass::Latin)},
    {"DE", ToMask(ScriptClass::Latin)},   {"PT", ToMask(ScriptClass::Latin)},
    {"IT", ToMask(ScriptClass::Latin)},   {"ID", ToMask(ScriptClass::Latin)},
    {"MS", ToMask(ScriptClass::Latin)},   {"FIL", ToMask(ScriptClass::Latin)},
    {"TR", ToMask(ScriptClass::Latin)},   {"PL", ToMask(ScriptClass::Latin)},
    {"NL", ToMask(ScriptClass::Latin)},   {"CS", ToMask(ScriptClass::Latin)},
    {"HU", ToMask(ScriptClass::Latin)},   {"SV", ToMask(ScriptClass::Latin)},
    {"RO", ToMask(ScriptClass::Latin)},   {"DA", ToMask(ScriptClass::Latin)},
    {"FI", ToMask(ScriptClass::Latin)},   {"NO", ToMask(ScriptClass::Latin)},
    {"KO", ToMask(ScriptClass::Hangul)},
    {"ZH-CN", ToMask(ScriptClass::Han)},
    {"ZH-TW", ToMask(ScriptClass::Han)},
    {"JA", ToMask(ScriptClass::Han) | ToMask(ScriptClass::Kana)},
    {"RU", ToMask(ScriptClass::Cyrillic)},
    {"UK", ToMask(ScriptClass::Cyrillic)},
    {"TH", ToMask(ScriptClass::Thai)},
    {"AR", ToMask(ScriptClass::Arabic)},
    {"FA", ToMask(ScriptClass::Arabic)},
    {"UR", ToMask(ScriptClass::Arabic)},
    {"HI", ToMask(ScriptClass::Devanagari)},
    {"BN", ToMask(ScriptClass::Bengali)},
    {"KM", ToMask(ScriptClass::Khmer)},
    {"LO", ToMask(ScriptClass::Lao)},
    {"HE", ToMask(ScriptClass::Hebrew)},
    {"EL", ToMask(ScriptClass::Greek)},
    {"MY", ToMask(ScriptClass::Myanmar)},
};

// Script mask for an i18n language code ("KO", "ZH-CN", ...). Case-insensitive;
// an unknown / AUTO / empty code returns 0 (the wrong-script gate then fails
// open rather than guessing a script).
std::uint32_t TargetScriptClasses(std::string_view tgt_code);

// Script mask of every class present in `text` (bitwise OR).
std::uint32_t TextScriptClasses(std::wstring_view text);

// Number of Unicode code points in `text` (surrogate-pair aware).
std::size_t CountCodePoints(std::wstring_view text);

// R1-1 wrong script: zero target-script code points in a non-trivial output
// while the source carried a script the target does not use. Reads as: the
// engine answered in some other language (the live "asdf" EN->KO English
// essay, "ㅋㅋㅋ" KO->EN Korean/Chinese loops). `tgt_script_classes == 0`
// (unknown target) never rejects.
//
// WS1 (261108_0002, WS1-gates) LEAK DOMINANCE. Why the pre-WS1 rule missed the
// user report: the early "target script present -> pass" exit treated ONE
// embedded Latin word as proof of a Korean translation, so a 58-Hangul +
// "astrology" answer to a Korean source shipped as status=ok for an English
// target. The gate now also rejects the leak shape: the source carried at
// least kWrongScriptLeakMinSourceCodePoints (10) code points in scripts the
// target does not use, and the output STILL retains at least
// kWrongScriptLeakRetainedPercent (40%) of them. 10 is the smallest source
// that can carry a real word in the other script (a 9-code-point quote is
// noise, and the base rule never fires on it anyway); 40% is the point where
// "a term kept verbatim" becomes "the answer was not translated". A faithful
// translation that quotes one foreign name stays far below both bounds (a
// 5-character Korean quote inside a 100-character source retains 5%).
inline constexpr std::size_t kWrongScriptLeakMinSourceCodePoints = 10;
inline constexpr int kWrongScriptLeakRetainedPercent = 40;
bool IsWrongScriptOutput(std::wstring_view output, std::wstring_view source,
                         std::uint32_t tgt_script_classes);

// R1-1 repetition, rules A + B (R2-6 calibrated envelope):
//   A: >= kRepeatedLineMinCount CONSECUTIVE identical non-empty lines (blank
//      lines ignored). Non-consecutive repeats are not a loop.
//   B: one whitespace token repeated >= kRepeatedTokenRunMinCount times in a
//      row, the run covering >= kRepeatedTokenRunMinCoveragePercent of the
//      output's tokens, AND the source carrying no identical-token run of its
//      own (the user's reduplication is content - see the threshold block).
bool HasRepeatedLineOrTokenRun(std::wstring_view text, std::wstring_view source = {});

// R1-1 repetition, rule C: the output's trailing tokens form >= 3 identical
// consecutive blocks (period 1..kSuffixRepetitionMaxPeriodTokens) covering at
// least kSuffixRepetitionMinSpanTokens tokens.
bool HasSuffixRepetition(std::wstring_view text);

// R1-1 repetition, combined: the single predicate used by the post-decode gate
// AND the mid-decode probe, so both sites always agree. `source` feeds the R2-6
// Rule-B source guard (a reduplication the user typed is content, not a loop);
// the default-empty overload keeps the pre-R2-6 text-only envelope for callers
// that genuinely have no source (and for the pure predicate pins).
bool OutputHasRepetition(std::wstring_view text, std::wstring_view source = {});

// F-04 (v0.10.5 review round 1): conversational meta-leakage needles (apology
// openers, "the following is", assistant offers) only fire for a SHORT source.
// The talk-back failure mode exists only when the model answers a word or a
// short phrase instead of translating; an opener-leading output for a longer
// source is its translation ("Sorry, but I cannot" -> "죄송하지만 ...") and
// must never be rejected. Label-style prefixes ending in a colon stay
// unconditional. The handoff source guard is untouched: a needle the SOURCE
// itself carries never fires.
inline constexpr std::size_t kConversationalNeedleMaxSourceCodePoints = 15;

// R1-1 meta leakage: a table-driven refusal opener ("translation is not
// possible", "i can't translate", "please contact us for"), an assistant-style
// opener ("sure, here", "as an ai", "here are some tips"), or a "<language>:"
// label line. A string the SOURCE text itself carries is never leakage (repo
// idiom, same source-guard as StripEndOfConversationMarkers), so user text
// about these phrases survives untouched. R2-7 extends the guard to a
// legitimately TRANSLATED label: when the source line at the same index starts
// with its own colon label, the translated label on that line is content
// ("출처: 로이터" -> "Source: Reuters", "English: Fluent" -> "영어: 유창함").
bool HasMetaLeakage(std::wstring_view text, std::wstring_view source);

// R2-16 rule (i), pure: an answer whose code points are at least
// kUnexpectedScriptSharePercent carried by a script in NEITHER the target nor
// the source, AND longer than kUnexpectedScriptLengthExpansionPercent/100 x the
// source. `tgt_script_classes == 0` (AUTO/unknown target) fails OPEN: without a
// target there is no "unexpected" to measure.
bool IsUnexpectedScriptHeavyOutput(std::wstring_view output, std::wstring_view source,
                                   std::uint32_t tgt_script_classes);

// R2-16 rule (ii), pure: the two-tier tiny-source amplification guard (RC-2,
// v0.10.5; floors RE-CALIBRATED by WS1 261108_0002). A source that is itself a
// pure repetition (see below) keeps the bounded laughter ceiling instead of
// the flat floors. Otherwise:
//   Tier 1 (source <= kAmplifiedSourceMaxCodePoints = 4 code points):
//     output >= max(kAmplifiedTinyTier1MinOutputCodePoints = 16,
//                   source_cp * kAmplifiedTinyTier1Ratio = 4x).
//   Tier 2 (5..15 code points, or any single whitespace-free token):
//     output >= kAmplifiedOutputMinCodePoints (64) at a
//     >= kAmplifiedTinyTier2Ratio (6x) expansion EXCEPT for a single
//     whitespace-free word of <= 15 code points, whose floor is
//     max(kAmplifiedSingleWordOutputMinCodePoints = 20,
//         source_cp * kAmplifiedSingleWordRatio = 4x) - the user-report
//     window (the 14-32 code-point hallucinations measured for the reported
//     words; a real one-word answer is 2-11: 회사, 점성학, 사이버, 자연, 의학,
//     Unternehmen, Cyberspace).
bool IsAmplifiedTinySourceOutput(std::wstring_view output, std::wstring_view source);

// R3 source-shape carve-out for rule (ii), pure: true when the WHOLE source is
// one unit repeated AT LEAST THREE TIMES ("ㅋㅋㅋㅋ", "kkkk", "!!!!", "no no
// no"; RC-2, v0.10.5: the floor is three repetitions, so everyday 2-char AA
// words ("점점", "妈妈") and 4-char ABAB words ("바이바이", "haha") are NOT
// laughter). Whitespace is ignored for the shape test; one code point alone
// is never a repetition (that is the live "음" residual the rule exists for).
// Used by rule (ii) to
// give such a source the bounded laughter ceiling
// (kAmplifiedRepetitionOutputMinCodePoints) rather than a blanket exemption: a
// laughter answer passes, a 980-1110-code-point essay is still rejected.
bool IsPureRepetitionSource(std::wstring_view source);

// WS1 (261108_0002, WS1-gates) source shape: true when the WHOLE (trimmed)
// source is ONE word - 1..kSingleWordSourceMaxCodePoints (20) code points, no
// whitespace, no punctuation, and at least one letter (a script-bearing code
// point, so "12345" / "..." / "???" are not words). A pure repetition source
// ("ㅋㅋㅋㅋ", "no no no") is never a word here: it keeps the R3/RC-2 laughter
// carve-out. This is the shape every reported hallucination started from and
// the precondition of the structural gate below; the amplification tier uses
// the slightly broader "single whitespace-free token" shape RC-2 defined, so a
// punctuated token ("OK!!") keeps its historical tier.
inline constexpr std::size_t kSingleWordSourceMaxCodePoints = 20;
bool IsSingleWordSource(std::wstring_view source);

// WS1 (261108_0002, WS1-gates) STRUCTURAL single-word gate, pure: "the model
// answered a word with a SENTENCE". Fires only when the source is one word
// (IsSingleWordSource) and the output carries a sentence shape that no
// one-word translation can legitimately have:
//   (a) it OPENS with a numbered/bullet marker ("1.", "1)", "1、", "- ..."), the
//       numbered-quiz hallucination;
//   (b) it contains '?' or '？' anywhere - an answer to a word is not a
//       question;
//   (c) it ENDS a sentence ('.', '!', '。') AND carries >= 3 whitespace
//       separated words - the "explanation" shape.
// A real one-word translation is one word, so every clause is impossible for
// it by construction; multi-word sources and multi-line sources never reach
// the gate at all (a sentence translation legitimately ends with '.').
bool IsSingleWordToSentenceHallucination(std::wstring_view output, std::wstring_view source);

// WS1 (261108_0002, WS1-gates) DECODE NORMALIZATION, pure: strip a leading
// assistant/role header from a rung output ("assistant: 사이버" -> "사이버").
// The reported single-word hallucinations arrive as a dialogue turn
// ("assistant: <answer>", "model: <answer>", "<|assistant|>\n<answer>"), and
// the header itself is garbage: dropping it lets the answer be judged on its
// own merits instead of being rejected as meta leakage.
//   * prefixes (ASCII case-insensitive, at offset 0): "assistant:", "model:",
//     "ai:", "bot:", "<|assistant|>"; the immediate horizontal whitespace
//     after the prefix goes with it.
//   * SOURCE GUARD (the repo idiom used by StripEndOfConversationMarkers and
//     HasMetaLeakage): a prefix the SOURCE text itself begins with is never
//     stripped - it is the user's content, not a leaked header.
//   * LABEL GUARD (the R2-7 idiom): when the SOURCE's first line starts with
//     its own colon label, a leading target-language label is a TRANSLATED
//     label ("모델: GPT-4" -> "Model: GPT-4") and is never stripped.
// Returns true when something was stripped. In-place on UTF-8 (the prefixes are
// pure ASCII, and UTF-8 continuation bytes can never collide with them).
bool StripLeadingRoleHeader(std::string& text, std::string_view source);

// WS1 (261108_0002, WS1-gates) item 10, pure: the leading-colon cleanup
// decision. A stray ':' is dropped only when
//   * the (already trimmed) output starts with ':',
//   * the LEADING-WHITESPACE-TRIMMED source does NOT start with ':' (the user's
//     own colon is content), and
//   * the output does not open an emoji :shortcode: (^:[A-Za-z0-9_+-]+:) -
//     ":laughing: 하하" is an emoji answer, not a ChatML artefact.
bool ShouldStripLeadingColon(std::string_view output, std::string_view source);

// WS1 (261108_0002, WS1-gates) item 9b, pure: does an exactly-matching echo
// PREFIX prove the model is still copying untranslated text? Used by the
// mid-decode echo probe to spare a mixed-script continuation: a source that
// mixes the target script with another one ("Ah... I fell asleep... :laughing:
// 사실 저는..." translated into English) legitimately starts its translation with
// the part that is ALREADY in the target script, so an exact prefix match there
// is not an echo. True when
//   * the prefix carries a code point whose script the target does not use
//     (proof of untranslated text), or
//   * every script the source carries is outside the target (script-homogeneous
//     source: no part of it can be "already in the target", so exact tracking
//     can only be copying).
bool IsUntranslatedEchoPrefix(std::wstring_view prefix, std::wstring_view source,
                              std::uint32_t tgt_script_classes);

enum class OutputRejectReason : std::uint8_t {
    None = 0,
    Repetition = 1,
    WrongScript = 2,
    MetaLeakage = 3,
    ScriptLeak = 4, // R2-16: unexpected-script / amplified-tiny-source answer
    // WS1 (261108_0002, WS1-gates): the structural one-word-to-sentence shape
    // (IsSingleWordToSentenceHallucination). Its own value because the shape
    // is a different failure from a script leak and the shape-only diagnostic
    // must be able to tell them apart; every consumer is in this library plus
    // the unit suite (no wire vocabulary keys on OutputRejectReason), so the
    // extra enumerator costs one switch case and one pin.
    SingleWordSentence = 5,
};

// Short stable name for the shape-only diagnostic lines (never the content).
const char* OutputRejectReasonName(OutputRejectReason reason);

// R1-1 combined gate over one trimmed rung output. Empty output returns None
// (the ladder's own empty/echo handling owns that case).
//
// Rule order (first match wins, most specific diagnostic name first):
// Repetition -> MetaLeakage -> WrongScript -> ScriptLeak (rule (i) | rule (ii))
// -> SingleWordSentence. WS1 (261108_0002) puts the structural single-word
// gate LAST on purpose: a calibrated shape that already rejects an essay keeps
// its own reason name, and the structural gate only claims the answers the
// calibrated floors still miss (the "explanation" of one word that sits
// between the new single-word floor and the old 64-code-point one). Rejecting is
// what both do; the order only decides the diagnostic.
// WS1 also moved MetaLeakage in front of WrongScript: the new wrong-script
// leak-dominance clause fires on retained foreign text, which is exactly what a
// source echo inside a refusal preamble looks like, and the refusal name is the
// more useful one.
OutputRejectReason ClassifyTranslationOutput(std::wstring_view output,
                                             std::wstring_view source,
                                             std::string_view tgt_code);

// 260930_0003 (다, CEO decision session 260930_0003): paragraph-boundary
// chunking of over-budget inputs. When the prompt token count exceeds
// kLlamaPromptTokenBudget, Translate() splits the SOURCE at blank-line
// boundaries, translates each chunk sequentially in the original order, and
// stitches the outputs back with the ORIGINAL separators verbatim - the old
// head+tail shrink that discarded the MIDDLE of a long input is replaced.
// Model-independent (pure text), so the unit suite pins it headlessly and it
// compiles in both llama and no-llama configurations.
struct ParagraphSplit {
    // blocks[i] is the i-th blank-line-separated paragraph, in original order.
    std::vector<std::wstring> blocks;
    // separators[i] is the VERBATIM whitespace run found AFTER blocks[i] in the
    // source ("" when nothing followed the last block). Stitching a fully
    // translated input back together is blocks[0] + separators[0] + blocks[1]
    // + ... + separators[n-1]. Consecutive blank lines (e.g. "\r\n\r\n\r\n")
    // are preserved as ONE verbatim separator run.
    std::vector<std::wstring> separators;
};

// Splits text into alternating content blocks and blank-line separators.
// CRLF-aware; a whitespace run qualifies as a separator only when it contains
// >= 2 line breaks (a blank line) - single newlines stay inside a block.
ParagraphSplit SplitParagraphBlocks(std::wstring_view text);

// Reassembles blocks[first..last] with their INTERNAL separators
// (separators[first..last-1]). The separator AFTER the range is deliberately
// NOT included - the caller appends it when stitching chunk outputs, so the
// original separators survive BETWEEN translated chunks.
std::wstring JoinParagraphBlocks(const ParagraphSplit& split, size_t first, size_t last);

// 260930_0003 (다): the single over-budget decision point. A prompt token
// count strictly above the budget selects the chunking path; at or below it
// Translate() runs the historical single-shot path byte-identically.
constexpr bool PromptNeedsChunking(int token_count) {
    return token_count > kLlamaPromptTokenBudget;
}

// 260930_0003 (다) + P1/P2 remediation (review 260930): pure greedy chunk
// packer behind an INJECTABLE measure, so the whole packing decision is
// unit-testable headlessly (the production measure tokenizes through the
// loaded model's vocab; tests substitute a fake). `measure(first, last)`
// returns the token count of JoinParagraphBlocks(split, first, last) wrapped
// as a prompt, or a negative value on tokenizer failure (-> std::nullopt).
// Returns the inclusive [first,last] block ranges of the chunks, in original
// order, covering every block exactly once.
//
// P1 fix: a range whose joined text is EMPTY (possible only as a LEADING
// blank-only range - SplitParagraphBlocks emits blocks[0]=="" when the input
// starts with a blank-line run) is FORCE-EXTENDED with the next block without
// measuring. Shipping a zero-length chunk would make translate_once("") hit
// the empty-prompt tokenizer failure and abort the ENTIRE request as
// engine_failed - the exact long-paste regression chunking was built to fix.
// The leading separator stays attached to the chunk text, so the stitched
// reconstruction of the input remains byte-faithful.
template <typename Measure>
std::optional<std::vector<std::pair<size_t, size_t>>> PackParagraphChunks(
    const ParagraphSplit& split, int token_budget, Measure measure) {
    std::vector<std::pair<size_t, size_t>> ranges;
    const size_t n = split.blocks.size();
    size_t b = 0;
    while (b < n) {
        size_t e = b;
        while (e + 1 < n) {
            if (!JoinParagraphBlocks(split, b, e).empty()) {
                const int tokens = measure(b, e + 1);
                if (tokens < 0) {
                    return std::nullopt; // tokenizer failure
                }
                if (tokens > token_budget) {
                    break; // close the chunk at e
                }
            }
            ++e; // fits (or the range so far is blank-only: P1 force-extend)
        }
        ranges.emplace_back(b, e);
        b = e + 1;
    }
    return ranges;
}

// REQ-043: headless local inference engine (see file header for provenance).
// All members are public exactly as in the original struct; the host drives it
// from its single inference worker thread, the app from TranslationManager
// under its mutex — the caller-serialized discipline the original code
// documented (control_token_texts needs no separate lock) is preserved by
// both call sites.
class LocalInferenceEngine {
public:
#ifdef HAVE_LLAMA_CPP
    llama_model* model = nullptr;
    llama_context* ctx = nullptr;
    const llama_vocab* vocab = nullptr;
#else
    // REQ-043: no-llama stub keeps the SAME member layout surface the
    // TranslationManager leg touches (loaded_path/cancel_flag) so the derived
    // shim in engine.cpp compiles in both configurations.
    void* model = nullptr;
    void* ctx = nullptr;
    void* vocab = nullptr;
#endif
    std::string loaded_path;
    // REQ-R16: cancellation flag owned by the caller (null when the engine was
    // created without one, e.g. in isolation tests - then cancellation is
    // simply unavailable and behavior is the old full-run).
    const std::atomic<bool>* cancel_flag = nullptr;
    // REQ-051 U-1 FIX 2: the model-load cancellation flag. NULL (the default)
    // makes the load leg ALIAS cancel_flag — byte-identical to the historical
    // single-flag behavior. A caller that must never discard an in-flight
    // load (the ggml-translate worker: a per-job abort unwinds the decode via
    // cancel_flag but the multi-minute load, once started, runs to completion
    // so the next job never repays it) points this at its own never-set
    // atomic. EnsureLoaded's progress callback and its cancel checks read
    // THIS flag (see LoadCancelRequested); the decode loop keeps cancel_flag.
    const std::atomic<bool>* load_cancel_flag = nullptr;
    // REQ-051 U-1 FIX 1: set when a Translate() decode loop exhausts
    // kDecodeWallClockBudgetMs (the worker maps it to the frozen "timeout"
    // wire code); cleared at every Translate() entry. Plain bool — Translate()
    // is caller-serialized, exactly like control_texts_built below.
    bool decode_wall_clock_exhausted_ = false;

    bool CancelRequested() const {
        return cancel_flag && cancel_flag->load(std::memory_order_acquire);
    }

    // REQ-051 U-1 FIX 2: the effective load-cancellation flag (load_cancel_flag
    // when wired, cancel_flag otherwise). EnsureLoaded consults this so the
    // default configuration keeps the legacy single-flag semantics.
    const std::atomic<bool>* EffectiveLoadCancelFlag() const {
        return load_cancel_flag ? load_cancel_flag : cancel_flag;
    }
    bool LoadCancelRequested() const {
        const std::atomic<bool>* flag = EffectiveLoadCancelFlag();
        return flag && flag->load(std::memory_order_acquire);
    }
    bool decode_wall_clock_exhausted() const {
        return decode_wall_clock_exhausted_;
    }
    // SEC-B2 (session 260911_0002, verify 233020): vocab-derived control-token
    // scrub set, built lazily on the first Translate() after a (re)load and
    // invalidated by Unload(). Translation requests are caller-serialized, so
    // this cache needs no separate lock (original contract, kept verbatim).
    std::vector<std::wstring> control_token_texts;
    bool control_texts_built = false;

    LocalInferenceEngine();
    ~LocalInferenceEngine();

    void Unload();
    bool EnsureLoaded(const std::string& path);

    // R6 Phase 4 (B2, plan §4.1 item 2): src_name is the resolved SOURCE token
    // for the prompt hint (""/AUTO = no hint, historical behavior).
    //
    // REQ-CP T4 (design §8 + tech-gate A2): template_ref selects the rung-1
    // prompt template (config.cpp registry). Defaulted to "" = hymt2-official,
    // so every existing caller compiles and behaves byte-identically until T5
    // wires the worker-protocol prompt_template field. Per A2 the ref steers
    // the rung-1 BuildPrompt ONLY; the rung-2 completion retry keeps its single
    // template-independent form.
    std::wstring Translate(
        std::wstring_view text,
        std::string_view tgt_name,
        std::string_view src_name,
        const std::string& path,
        float temperature = 0.0f,
        float top_p = 0.6f,
        int top_k = 20,
        float rep_pen = 1.05f,
        std::string_view template_ref = {}
    );
};

} // namespace emebalachat
