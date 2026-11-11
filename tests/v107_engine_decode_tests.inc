// ---------------------------------------------------------------------------
// v0.10.7 (session 261111_0001, coder unit U4): decoding-defect fixes for the
// Hy-MT2-1.8B local engine. Staged as an .inc next to run_tests.cpp; defines
// RunV107EngineDecodeTests(int& failures). Registered in main() right after
// TestS260930ChunkStitch() (same translation-core cluster).
//
// Coverage (all pure / headless, no GGUF needed):
//   1. Sampler-shape selection: DecideSamplerShape already selects
//      BareGreedy for rep_pen <= 1.0 (R1-2 pins); the v0.10.7 two-stage
//      decision V107EffectiveRepPen forces the penalty-free first pass and
//      the bounded 1.10 retry, and V107MaxSamplerRetries() == 1.
//   2. Micro-chunk threshold: kV107ChunkTokenThreshold == 300 and
//      V107NeedsMicroChunking boundary (300 fits single-shot, 301 chunks).
//   3. Previous-chunk context prefix: BuildV107PrevChunkContextPrefix is
//      empty for an empty previous chunk and clearly marked otherwise.
//   4. Length-ratio guardrail: V107LengthRatioViolated decision matrix
//      (exempt <= 40 source code points; [30%, 250%] band above it),
//      plus the OutputRejectReason::LengthRatio wiring (name + classify).
//   5. Shipped defaults: worker_protocol.hpp SamplingParams.rep_pen == 1.0
//      and AppConfig.repetition_penalty == 1.0 (source pins + live struct).
//   6. Source pins: the retry wiring exists in translation_common.cpp
//      (V107EffectiveRepPen at the sampler switch, the bounded retry arms,
//      the micro-chunk threshold at the split decision, the prev-chunk
//      prefix in the chunk loop, the length-ratio gate in the classifier).
// ---------------------------------------------------------------------------

#include "engine_core/translation_common.hpp" // mid-TU include, pragma-once safe (engine repo since 261111_0001 U7; resolved via the engine libs' PUBLIC include dirs)

int RunV107EngineDecodeTests(int& failures) {
    std::cout << "[RUN] Testing v0.10.7 engine decode fixes (261111_0001 U4)..." << std::endl;
    const int failures_before = g_failed_count;
    namespace em = emebalachat;

    // ---- 1. two-stage deterministic sampler decision ----
    {
        static_assert(em::kV107RepetitionRetryPenalty == 1.10f,
                      "v0.10.7: the single bounded retry penalty is 1.10");
        static_assert(em::V107MaxSamplerRetries() == 1,
                      "v0.10.7: NEVER more than one sampler retry");
        // First pass with a penalty-free configured profile stays pure greedy.
        TEST_CHECK(em::V107EffectiveRepPen(1.0f, 0) == 1.0f,
                   "v0.10.7: first pass with rep_pen 1.0 stays penalty-free (pure greedy)");
        TEST_CHECK(em::DecideSamplerShape(0.0f, em::V107EffectiveRepPen(1.0f, 0)) ==
                       em::SamplerShape::BareGreedy,
                   "v0.10.7: shipped profile (temp 0, rep_pen 1.0) selects BareGreedy");
        // The retry pass applies exactly the 1.10 penalty.
        TEST_CHECK(em::V107EffectiveRepPen(1.0f, 1) == 1.10f,
                   "v0.10.7: the one retry uses kV107RepetitionRetryPenalty (1.10)");
        TEST_CHECK(em::DecideSamplerShape(0.0f, em::V107EffectiveRepPen(1.0f, 1)) ==
                       em::SamplerShape::PenalizedGreedy,
                   "v0.10.7: the retry pass takes the penalized-greedy chain");
        // A second retry is impossible (bounded): retry_count 2 still 1.10,
        // and the caller-side bound is V107MaxSamplerRetries() == 1.
        TEST_CHECK(em::V107EffectiveRepPen(1.0f, 2) == 1.10f,
                   "v0.10.7: retry_count is clamped by the caller bound (value stable at 1.10)");
        // R2-C: a legacy configured penalty > 1.0 is honored on the first
        // pass (existing user configs keep their stored value); the retry
        // penalty is max(1.10, configured) — a configured 1.05 is RAISED to
        // the 1.10 retry penalty (the old code kept 1.05 and gave legacy
        // configs no retry escalation at all), a configured 1.20 is never
        // lowered.
        TEST_CHECK(em::V107EffectiveRepPen(1.05f, 0) == 1.05f,
                   "v0.10.7: a legacy configured rep_pen > 1.0 is honored on pass 1");
        TEST_CHECK(em::V107EffectiveRepPen(1.05f, 1) == 1.10f,
                   "v0.10.7 R2-C: the retry raises a legacy 1.05 to the 1.10 retry penalty");
        TEST_CHECK(em::V107EffectiveRepPen(1.20f, 1) == 1.20f,
                   "v0.10.7 R2-C: a configured penalty above the retry value is never lowered");
        TEST_CHECK(em::DecideSamplerShape(0.0f, 1.05f) == em::SamplerShape::PenalizedGreedy,
                   "v0.10.7: legacy 1.05 profile still takes PenalizedGreedy (R1-2 intact)");
        // Temperature still selects the branch alone.
        TEST_CHECK(em::DecideSamplerShape(0.7f, em::V107EffectiveRepPen(1.0f, 0)) ==
                       em::SamplerShape::Dist,
                   "v0.10.7: temperature > 0.001 keeps the Dist chain regardless of retry state");
    }

    // ---- 2. micro-chunk threshold ----
    {
        static_assert(em::kV107ChunkTokenThreshold == 300,
                      "v0.10.7: micro-chunk threshold is ~300 tokens");
        static_assert(em::kV107ChunkTokenThreshold < em::kLlamaPromptTokenBudget,
                      "v0.10.7: the micro-chunk threshold sits far below the hard 6128 ceiling");
        TEST_CHECK(!em::V107NeedsMicroChunking(0), "v0.10.7: zero tokens never micro-chunks");
        TEST_CHECK(!em::V107NeedsMicroChunking(300),
                   "v0.10.7: exactly 300 tokens fits the single-shot path");
        TEST_CHECK(em::V107NeedsMicroChunking(301),
                   "v0.10.7: 301 tokens selects micro-chunking");
        TEST_CHECK(em::V107NeedsMicroChunking(6128) && em::V107NeedsMicroChunking(6129),
                   "v0.10.7: 6128 and 6129 tokens both select micro-chunking");
        // The hard ceiling invariant is untouched.
        TEST_CHECK(em::PromptNeedsChunking(6129) && !em::PromptNeedsChunking(6128),
                   "v0.10.7: the 6128 hard per-chunk ceiling (PromptNeedsChunking) is unchanged");

        // R2-B + G-08 (round-2): pin the SPLIT DECISION itself, behaviorally,
        // bound to the SHARED PRODUCTION HELPER V107ShouldMicroChunk — not to
        // a locally duplicated predicate (the round-1 form duplicated
        // `!V107NeedsMicroChunking(n)` inline and stayed green if the
        // production condition was reverted). Single-shot iff NOT
        // V107ShouldMicroChunk(n), at the 300/301 and 6128/6129 boundaries.
        // PromptNeedsChunking must NOT take part in the split decision (it
        // stays the per-chunk ceiling only).
        static_assert(em::V107ShouldMicroChunk(300) == false &&
                          em::V107ShouldMicroChunk(301) == true &&
                          em::V107ShouldMicroChunk(6128) == true &&
                          em::V107ShouldMicroChunk(6129) == true,
                      "v0.10.7 G-08: the shared split helper chunks strictly above 300");
        const int kProbe[] = {0, 1, 300, 301, 6128, 6129, 20000};
        for (const int n : kProbe) {
            const bool single_shot = !em::V107ShouldMicroChunk(n); // the production split condition
            const bool expect_single = (n <= 300);
            TEST_CHECK(single_shot == expect_single,
                       "v0.10.7 R2-B/G-08: the split decision is single-shot exactly at/below 300 tokens (n=" +
                       std::to_string(n) + ")");
        }
        // At 301..6128 the old `||` condition took the single-shot path
        // (PromptNeedsChunking false there); the decision must now chunk.
        TEST_CHECK(!em::PromptNeedsChunking(301) && em::V107ShouldMicroChunk(301),
                   "v0.10.7 R2-B: 301 tokens chunks even though the 6128 ceiling is not reached");
        TEST_CHECK(!em::PromptNeedsChunking(6128) && em::V107ShouldMicroChunk(6128),
                   "v0.10.7 R2-B: 6128 tokens chunks even though the 6128 ceiling itself is not exceeded");
        // Negative: PromptNeedsChunking no longer selects the split — above
        // the 6128 hard ceiling the split decision is still micro-chunking
        // alone (identical expression, different ceiling).
        TEST_CHECK(em::PromptNeedsChunking(6129) && em::V107ShouldMicroChunk(6129) &&
                       em::V107ShouldMicroChunk(6129) == em::V107NeedsMicroChunking(6129),
                   "v0.10.7 G-08: PromptNeedsChunking does not select the split (6129 chunks via the same helper)");
    }

    // ---- 2b. G-07: the chunk-budget projection + up-front refusal (pure) ----
    {
        static_assert(em::kChunkedRequestBudgetMs == 28000,
                      "v0.10.7 G-07: the whole-request chunk budget stays 28 s");
        static_assert(em::V107ProjectedPerChunkBudgetMs() == em::kV107MeasuredWorstChunkMs,
                      "v0.10.7 G-07: per-chunk projection is the measured worst chunk (the bounded sampler retry stays a mid-loop degenerate-tail clamp, not part of the projection)");
        // Zero / degenerate inputs project zero work and never refuse.
        TEST_CHECK(em::V107ProjectedChunkBudgetMs(0, 0) == 0,
                   "v0.10.7 G-07: zero prompt / zero chunks projects zero ms");
        TEST_CHECK(!em::V107ChunkBudgetExceeded(301, 1),
                   "v0.10.7 G-07: a single 301-token chunk is never refused");
        // The packer measures the FULL prompt (template overhead included)
        // against 300, so N ~= (n-O)/(300-O), NOT ceil(n/300): the projection
        // takes the conservative max(packed, ceil(n/300)).
        // 6128-token prompt: the helper's conservative ceil(6128/300) FLOOR
        // is 21 chunks (the corrected (n-O)/(300-O) estimate is ~26, and the
        // projection takes the larger of packed/floor) -> 21 * 2000 ms
        // = 42000 ms projected > 28000 -> refused up front. F-3 (round 3)
        // aligned the production comment's worked examples with the
        // (n-O)/(300-O) formula; this row pins the ceil floor the helper
        // still applies.
        TEST_CHECK(em::V107ProjectedChunkBudgetMs(6128, 21) == 21LL * em::V107ProjectedPerChunkBudgetMs(),
                   "v0.10.7 G-07: 6128-token prompt projects 21 chunks x per-chunk worst");
        TEST_CHECK(em::V107ChunkBudgetExceeded(6128, 21),
                   "v0.10.7 G-07: a 6128-token / 21-chunk request is refused UP FRONT (projection 42 s > 28 s)");
        // A 2500-token (1-2 page) input: the ceil floor is 9 chunks (the
        // corrected (n-O)/(300-O) estimate is ~11 -> 22000 ms, still under the
        // budget) -> 9 * 2000 = 18000 ms <= 28000 -> NOT refused; it runs
        // under the whole-request deadline.
        TEST_CHECK(em::V107ProjectedChunkBudgetMs(2500, 9) == 18000LL,
                   "v0.10.7 G-07: a 2500-token / 9-chunk request projects 18 s");
        TEST_CHECK(!em::V107ChunkBudgetExceeded(2500, 9),
                   "v0.10.7 G-07: a 2500-token / 9-chunk request is NOT refused (18 s <= 28 s)");
        // Boundary: the largest chunk count that still fits the budget.
        // 28000 / 2000 = 14 chunks exactly -> 28000 ms is NOT > 28000, so 14
        // chunks pass, 15 refuse.
        TEST_CHECK(!em::V107ChunkBudgetExceeded(4200, 14),
                   "v0.10.7 G-07: 14 chunks project exactly 28000 ms — at the deadline, not over it");
        TEST_CHECK(em::V107ChunkBudgetExceeded(4201, 15),
                   "v0.10.7 G-07: 15 chunks project 30000 ms > 28000 — refused");
        // The projection never under-counts: a packed count BELOW the
        // ceil(n/300) bound is raised to the bound (conservative).
        TEST_CHECK(em::V107ProjectedChunkBudgetMs(6128, 1) ==
                       em::V107ProjectedChunkBudgetMs(6128, 21),
                   "v0.10.7 G-07: the projection floors the chunk count at ceil(n/300)");
        // The frozen client deadline still bounds the whole request: the
        // budget constant stays under the 30 s v1 client timeout.
        static_assert(em::kChunkedRequestBudgetMs < 30000,
                      "v0.10.7 G-07: the whole-request budget stays under the frozen 30 s client timeout");
    }

    // ---- 3. previous-chunk context prefix (pure builder) ----
    {
        TEST_CHECK(em::BuildV107PrevChunkContextPrefix(L"").empty(),
                   "v0.10.7: empty previous chunk -> empty context prefix (first chunk)");
        const std::wstring prefix = em::BuildV107PrevChunkContextPrefix(L"이전 문단 내용");
        TEST_CHECK(prefix.find(L"이전 문단 내용") != std::wstring::npos,
                   "v0.10.7: the prefix carries the previous source chunk verbatim");
        TEST_CHECK(prefix.find(L"<<<") != std::wstring::npos &&
                       prefix.find(L">>>") != std::wstring::npos,
                   "v0.10.7: the prefix is clearly delimited (read-only markers)");
        TEST_CHECK(prefix.find(L"do not translate") != std::wstring::npos,
                   "v0.10.7: the prefix is marked read-only context");
        TEST_CHECK(prefix.front() == L'<' && prefix.find(L"\n") != std::wstring::npos,
                   "v0.10.7: the marker opens the block on its own line");
    }

    // ---- 4. length-ratio guardrail decision ----
    {
        static_assert(em::kV107LengthGuardMinSourceCodePoints == 40,
                      "v0.10.7: the guardrail scope floor is 40 source code points");
        static_assert(em::kV107LengthRatioMinPercent == 30 && em::kV107LengthRatioMaxPercent == 250,
                      "v0.10.7: the guardrail band is [30%, 250%]");
        // Exempt: sources at or below 40 code points never trip the guard,
        // whatever the output length (single words / short messages /
        // pure-numeric / punctuation-only sources unchanged).
        TEST_CHECK(!em::V107LengthRatioViolated(1, 1000),
                   "v0.10.7: a 1-code-point source is exempt (huge output allowed)");
        TEST_CHECK(!em::V107LengthRatioViolated(40, 0),
                   "v0.10.7: a 40-code-point source is exempt (empty output allowed)");
        TEST_CHECK(!em::V107LengthRatioViolated(20, 5000),
                   "v0.10.7: pure-numeric/short sources are exempt regardless of ratio");
        // Band boundaries for a 100-code-point source: [30, 250].
        TEST_CHECK(!em::V107LengthRatioViolated(100, 30),
                   "v0.10.7: output at exactly 30% of a 100-cp source passes");
        TEST_CHECK(!em::V107LengthRatioViolated(100, 250),
                   "v0.10.7: output at exactly 250% of a 100-cp source passes");
        TEST_CHECK(em::V107LengthRatioViolated(100, 29),
                   "v0.10.7: output below 30% of the source is rejected (dropped content)");
        TEST_CHECK(em::V107LengthRatioViolated(100, 251),
                   "v0.10.7: output above 250% of the source is rejected (invented content)");
        TEST_CHECK(!em::V107LengthRatioViolated(100, 100),
                   "v0.10.7: a 1:1 translation passes");
        TEST_CHECK(!em::V107LengthRatioViolated(41, 41),
                   "v0.10.7: just above the floor, a 1:1 output passes");
        TEST_CHECK(em::V107LengthRatioViolated(41, 10),
                   "v0.10.7: just above the floor, a 10-cp output (<30% of 41) is rejected");
        // R2-F: non-multiple-of-100 source lengths — the band edge is EXACT
        // (cross-multiplication), not floored. 41 cp source: 30% = 12.3, so a
        // 12-cp output (29.27%) must VIOLATE and a 13-cp output (31.7%) must
        // pass; the old floored bound (41*30/100 = 12) admitted the 12.
        TEST_CHECK(em::V107LengthRatioViolated(41, 12),
                   "v0.10.7 R2-F: 41-cp source + 12-cp output (29.27%) is rejected (exact bound)");
        TEST_CHECK(!em::V107LengthRatioViolated(41, 13),
                   "v0.10.7 R2-F: 41-cp source + 13-cp output (31.7%) passes (exact bound)");
        // Upper edge, same source: 250% of 41 = 102.5, so 102 passes, 103 violates.
        TEST_CHECK(!em::V107LengthRatioViolated(41, 102),
                   "v0.10.7 R2-F: 41-cp source + 102-cp output (248.8%) passes (exact bound)");
        TEST_CHECK(em::V107LengthRatioViolated(41, 103),
                   "v0.10.7 R2-F: 41-cp source + 103-cp output (251.2%) is rejected (exact bound)");
        // Another non-multiple: 7-cp source is under the 40-cp floor (exempt),
        // 99-cp source: 30% = 29.7 -> 29 violates, 30 passes.
        TEST_CHECK(em::V107LengthRatioViolated(99, 29),
                   "v0.10.7 R2-F: 99-cp source + 29-cp output (29.29%) is rejected");
        TEST_CHECK(!em::V107LengthRatioViolated(99, 30),
                   "v0.10.7 R2-F: 99-cp source + 30-cp output (30.3%) passes");
        // Large source: 1000 cp -> [300, 2500].
        TEST_CHECK(em::V107LengthRatioViolated(1000, 299),
                   "v0.10.7: 29.9% of a 1000-cp source is rejected");
        TEST_CHECK(!em::V107LengthRatioViolated(1000, 300),
                   "v0.10.7: exactly 30% of a 1000-cp source passes");
        TEST_CHECK(!em::V107LengthRatioViolated(1000, 2500),
                   "v0.10.7: exactly 250% of a 1000-cp source passes");
        TEST_CHECK(em::V107LengthRatioViolated(1000, 2501),
                   "v0.10.7: 250.1% of a 1000-cp source is rejected");

        // Wiring: the reason name + the classifier actually use the guard.
        TEST_CHECK(std::string(em::OutputRejectReasonName(em::OutputRejectReason::LengthRatio)) ==
                       "length-ratio",
                   "v0.10.7: OutputRejectReason::LengthRatio has its diagnostic name");
        // A long MULTI-WORD source answered with a tiny output classifies as
        // LengthRatio (the fixture source carries whitespace so the
        // single-token amplification tier cannot claim it first; the output
        // is Hangul so the wrong-script gate cannot claim it either).
        std::wstring long_src;
        for (int i = 0; i < 40; ++i) {
            long_src += L"word ";
        }
        long_src += L"end"; // 41 words, 205 code points, multi-word
        TEST_CHECK(em::CountCodePoints(long_src) > em::kV107LengthGuardMinSourceCodePoints,
                   "v0.10.7: fixture source is above the guardrail floor");
        const std::wstring tiny_out = L"\uC815\uB2F5"; // 2 Hangul cp
        TEST_CHECK(em::ClassifyTranslationOutput(tiny_out, long_src, "KO") ==
                       em::OutputRejectReason::LengthRatio,
                   "v0.10.7: 205-cp source answered with 2 cp -> LengthRatio via the gate");
        // The REPETITION gate runs before the length gate, so a pathologically
        // repetitive in-band answer is claimed by Repetition (gate-order pin:
        // the recurring-token detector owns this shape, LengthRatio never sees
        // it). 30x "번역 " is exactly such a run.
        std::wstring ok_out;
        for (int i = 0; i < 30; ++i) {
            ok_out += L"\uBC88\uC5ED "; // 30x the same Hangul word, 150 cp, in [30%,250%]
        }
        TEST_CHECK(em::ClassifyTranslationOutput(ok_out, long_src, "KO") ==
                       em::OutputRejectReason::Repetition,
                   "v0.10.7: a repetitive in-band answer is claimed by the Repetition gate first");
        // A faithful-length long answer of DISTINCT words is NOT rejected by
        // any gate (no token run -> repetition silent; 92/205 = 45% in the
        // length band; Hangul answer to a KO target passes the script gates).
        static const wchar_t* const kDistinctWords[] = {
            L"\uC0AC\uACFC", L"\uBC14\uB098\uB098", L"\uD559\uAD50", L"\uCE5C\uAD6C",
            L"\uAC00\uBC29", L"\uCC45\uC0C1", L"\uC758\uC790", L"\uCC3D\uBB38",
            L"\uB3C4\uC2DC", L"\uC0AC\uB78C", L"\uC2DC\uAC04", L"\uC138\uACC4",
            L"\uC0B0\uCC45", L"\uAC15\uBB3C", L"\uD558\uB298", L"\uBCC4\uBE5B",
            L"\uB098\uBB34", L"\uC0C8\uBCBD", L"\uC800\uB141", L"\uC544\uCE68",
            L"\uACC4\uC808", L"\uBC14\uB78C", L"\uAD6C\uB984", L"\uD5A5\uAE30",
            L"\uC194\uB098\uAE30", L"\uB2EC\uBE5B", L"\uBCC4\uB4E4", L"\uAC15\uAC00",
            L"\uC232\uC18D", L"\uBC14\uB2E4\uAC00",
        };
        std::wstring distinct_out;
        for (const wchar_t* w : kDistinctWords) {
            distinct_out += w;
            distinct_out += L' ';
        }
        const std::size_t distinct_cp = em::CountCodePoints(distinct_out);
        const std::size_t long_src_cp = em::CountCodePoints(long_src);
        TEST_CHECK(distinct_cp * 100 >= long_src_cp * 30 && distinct_cp * 100 <= long_src_cp * 250,
                   "v0.10.7: distinct-word fixture stays inside the [30%, 250%] length band");
        TEST_CHECK(em::ClassifyTranslationOutput(distinct_out, long_src, "KO") ==
                       em::OutputRejectReason::None,
                   "v0.10.7: a distinct-word in-band long answer passes the whole gate");
        // A short source with a big output is NOT length-rejected (exempt),
        // regardless of the amplification rules (5 cp multi-word source: the
        // pre-existing gates own that shape, not LengthRatio).
        const std::wstring short_src = L"hi there"; // 8 cp, multi-word
        const std::wstring big_out(60, L'\uC815');
        TEST_CHECK(em::ClassifyTranslationOutput(big_out, short_src, "KO") !=
                       em::OutputRejectReason::LengthRatio,
                   "v0.10.7: a <=40-cp source never yields LengthRatio (exempt scope)");
    }

    // ---- 5. shipped defaults (live structs + source pins) ----
    {
        emebalachat::workerproto::SamplingParams sp;
        TEST_CHECK(sp.rep_pen == 1.0f,
                   "v0.10.7: shipped SamplingParams.rep_pen default is 1.0");
        TEST_CHECK(sp.temperature == 0.0f && sp.top_p == 0.6f && sp.top_k == 20,
                   "v0.10.7: temperature/top_p/top_k defaults unchanged");
        AppConfig cfg;
        TEST_CHECK(std::abs(cfg.repetition_penalty - 1.0f) < 0.001f,
                   "v0.10.7: shipped AppConfig.repetition_penalty default is 1.0");
        TEST_CHECK(std::abs(cfg.temperature - 0.0f) < 0.001f &&
                       std::abs(cfg.top_p - 0.6f) < 0.001f && cfg.top_k == 20,
                   "v0.10.7: AppConfig temperature/top_p/top_k defaults unchanged");

        const std::filesystem::path proto = ResolveRepoFile(L"src\\worker_protocol.hpp");
        const std::filesystem::path ex = ResolveRepoFile(L"config.example.json");
        const std::filesystem::path cfg_hpp = ResolveRepoFile(L"src\\config.hpp");
        if (!proto.empty()) {
            std::ifstream in(proto, std::ios::binary);
            const std::string src((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            TEST_CHECK(src.find("float rep_pen = 1.0f;") != std::string::npos,
                       "v0.10.7: worker_protocol.hpp ships rep_pen = 1.0f");
            TEST_CHECK(src.find("float rep_pen = 1.05f;") == std::string::npos,
                       "v0.10.7: the old 1.05f shipped default is gone from worker_protocol.hpp");
        }
        if (!ex.empty()) {
            std::ifstream in(ex, std::ios::binary);
            const std::string src((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            TEST_CHECK(src.find("\"repetition_penalty\": 1.0") != std::string::npos,
                       "v0.10.7: config.example.json ships repetition_penalty 1.0");
        }
        if (!cfg_hpp.empty()) {
            std::ifstream in(cfg_hpp, std::ios::binary);
            const std::string src((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            TEST_CHECK(src.find("float repetition_penalty = 1.0f;") != std::string::npos,
                       "v0.10.7: config.hpp ships repetition_penalty = 1.0f");
        }
    }

    // ---- 6. wiring source pins in translation_common.cpp ----
    {
        const std::filesystem::path tc_path =
            ResolveRepoFile(L"src\\engine_core\\translation_common.cpp");
        if (tc_path.empty()) {
            std::cout << "[SKIP] src/engine_core/translation_common.cpp not resolvable; "
                         "v0.10.7 wiring pins skipped." << std::endl;
        } else {
            std::ifstream in(tc_path, std::ios::binary);
            const std::string src((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            TEST_CHECK(src.find("V107EffectiveRepPen(rep_pen, sampler_retries)") != std::string::npos,
                       "v0.10.7: the sampler switch consumes the two-stage effective rep_pen");
            TEST_CHECK(src.find("sampler_retries < V107MaxSamplerRetries()") != std::string::npos,
                       "v0.10.7: every retry arm is bounded by V107MaxSamplerRetries()");
            TEST_CHECK(src.find("PackParagraphChunks(split, kV107ChunkTokenThreshold, measure_chunk)") !=
                           std::string::npos,
                       "v0.10.7: the packer budget is the ~300-token micro-chunk threshold");
            // R2-B + G-08 (round-2): pin the DECISION text, not just the
            // predicate name — the single-shot arm must be gated on the
            // SHARED helper V107ShouldMicroChunk ALONE, and the old
            // `|| !PromptNeedsChunking(...)` form must be gone. A substring
            // pin that still passes with the bug present is not a pin, so
            // the check requires the exact helper call.
            TEST_CHECK(src.find("if (!V107ShouldMicroChunk(n_full_prompt))") != std::string::npos,
                       "v0.10.7 R2-B/G-08: the split decision is `if (!V107ShouldMicroChunk(n_full_prompt))`");
            TEST_CHECK(src.find("if (!V107NeedsMicroChunking(n_full_prompt))") == std::string::npos,
                       "v0.10.7 G-08: the split arm no longer calls the raw predicate directly");
            TEST_CHECK(src.find("!V107NeedsMicroChunking(n_full_prompt) || !PromptNeedsChunking") ==
                           std::string::npos,
                       "v0.10.7 R2-B: the broken `|| !PromptNeedsChunking(...)` split condition is gone");
            // G-07: the up-front refusal is wired into the chunked path.
            TEST_CHECK(src.find("V107ChunkBudgetExceeded(n_full_prompt, chunks.size())") != std::string::npos,
                       "v0.10.7 G-07: the chunked path refuses up front via V107ChunkBudgetExceeded");
            // G-16 (round-2): the retry DIAGs print the EFFECTIVE penalty
            // max(1.10, configured), not the literal 1.10.
            TEST_CHECK(src.find("V107EffectiveRepPen(rep_pen, sampler_retries))\n") != std::string::npos ||
                           src.find("static_cast<double>(V107EffectiveRepPen(rep_pen, sampler_retries))") != std::string::npos,
                       "v0.10.7 G-16: the retry DIAGs print the effective (max) retry penalty");
            {
                // Count the retry DIAG lines that still print the literal.
                size_t pos = 0;
                int literal_110_diags = 0;
                const std::string literal =
                    "static_cast<double>(kV107RepetitionRetryPenalty))";
                while ((pos = src.find(literal, pos)) != std::string::npos) {
                    // Only the DIAG_F retry lines matter; the helper's own
                    // definition site also contains the constant but not in
                    // this cast form.
                    ++literal_110_diags;
                    pos += literal.size();
                }
                TEST_CHECK(literal_110_diags == 0,
                           "v0.10.7 G-16: no retry DIAG prints the literal 1.10 anymore");
            }
            TEST_CHECK(src.find("BuildV107PrevChunkContextPrefix(chunks[k - 1])") != std::string::npos,
                       "v0.10.7: the previous source chunk is carried into the current chunk's prompt");
            TEST_CHECK(src.find("V107LengthRatioViolated(CountCodePoints(source), CountCodePoints(output))") !=
                           std::string::npos,
                       "v0.10.7: the length-ratio guard runs inside ClassifyTranslationOutput");

            // ---- 6b. G-16 (round-2): regression pins for R2-C and R2-H ----
            // A substring pin that still passes when the bug returns is not a
            // pin, so these check the ABSENCE of the buggy form and the
            // PRESENCE of the fix inside the retry-label window.
            //
            // R2-C: the retry arms must fire on the repetition DETECTOR alone.
            // The buggy form conjoined `DecideSamplerShape(temperature,
            // rep_pen) == SamplerShape::BareGreedy` — note `rep_pen`, not the
            // effective value; the production sampler switch uses
            // `effective_rep_pen`, so this exact string can ONLY appear in the
            // removed conjunct.
            TEST_CHECK(src.find("DecideSamplerShape(temperature, rep_pen)") == std::string::npos,
                       "v0.10.7 G-16 R2-C pin: no retry arm re-introduces the BareGreedy conjunct");
            TEST_CHECK(src.find("sampler_retries < V107MaxSamplerRetries()") != std::string::npos,
                       "v0.10.7 G-16 R2-C pin: the retry arms stay bounded by V107MaxSamplerRetries() alone");
            //
            // R2-H: the backward-goto retry labels must reset the per-attempt
            // abort flags (the goto skips the declarations above the loop). Pin
            // each flag reset inside the window right after its label.
            {
                const size_t rerun = src.find("rerun_rung1_with_penalty:");
                TEST_CHECK(rerun != std::string::npos, "v0.10.7: the rung-1 retry label exists");
                if (rerun != std::string::npos) {
                    const std::string window = src.substr(rerun, 600);
                    TEST_CHECK(window.find("rung1_aborted_echo = false;") != std::string::npos &&
                                   window.find("rung1_aborted_repetition = false;") != std::string::npos &&
                                   window.find("rung2_repetition_aborted = false;") != std::string::npos,
                               "v0.10.7 G-16 R2-H pin: rerun_rung1_with_penalty resets ALL THREE abort flags");
                }
                const size_t same = src.find("retry_same_rung:");
                TEST_CHECK(same != std::string::npos, "v0.10.7: the same-rung retry label exists");
                if (same != std::string::npos) {
                    const std::string window = src.substr(same, 300);
                    TEST_CHECK(window.find("rung1_aborted_echo = false;") != std::string::npos,
                               "v0.10.7 G-16 R2-H pin: retry_same_rung resets rung1_aborted_echo");
                }
            }

            // ---- 6c. F-3 (261111_0001 round 3): the stale comment claims are
            // GONE and the corrected wording is present. Each pair asserts the
            // ABSENCE of the exact stale phrase AND the presence of its
            // replacement, so the pin fails whichever way the drift returns.
            {
                // (a) pass 0 runs the CONFIGURED penalty (V107EffectiveRepPen
                // returns the configured value at retry_count == 0) - it is
                // penalty-free only under the shipped 1.0 default.
                TEST_CHECK(src.find("penalty-free pass") == std::string::npos,
                           "v0.10.7 F-3(a): DIAG 055 no longer calls pass 0 the 'penalty-free pass'");
                TEST_CHECK(src.find("penalty-free first pass") == std::string::npos,
                           "v0.10.7 F-3(a): the retry rationale no longer calls pass 0 'penalty-free'");
                TEST_CHECK(src.find("repetition detected on the configured first pass") !=
                               std::string::npos,
                           "v0.10.7 F-3(a): DIAG 055 names the configured first pass");
                // (b) the projection EXCLUDES the bounded sampler retry
                // (V107ProjectedChunkBudgetMs is chunks x 2000 ms only).
                TEST_CHECK(src.find("incl. the one bounded sampler retry") == std::string::npos,
                           "v0.10.7 F-3(b): the refusal comment no longer claims the projection includes the retry");
                TEST_CHECK(src.find("does NOT add") != std::string::npos &&
                               src.find("V107ProjectedChunkBudgetMs does NOT add") != std::string::npos,
                           "v0.10.7 F-3(b): the refusal comment states the retry is excluded from the projection");
                // (c) the packer measures the FULL prompt (build_final_prompt
                // through measure_chunk), not source text alone.
                TEST_CHECK(src.find("of source text per chunk") == std::string::npos,
                           "v0.10.7 F-3(c): the packing budget is no longer described as source-only");
                TEST_CHECK(src.find("source text PLUS the instruction/template") != std::string::npos,
                           "v0.10.7 F-3(c): the packing comment names the full prompt (source + overhead)");

                // (d) the header's worked examples follow (n-O)/(300-O), not
                // ceil(n/300).
                const std::filesystem::path tch_path =
                    ResolveRepoFile(L"src\\engine_core\\translation_common.hpp");
                TEST_CHECK(!tch_path.empty(), "v0.10.7 F-3 pin: translation_common.hpp resolves");
                if (!tch_path.empty()) {
                    std::ifstream hin(tch_path, std::ios::binary);
                    const std::string hpp((std::istreambuf_iterator<char>(hin)),
                                          std::istreambuf_iterator<char>());
                    TEST_CHECK(hpp.find("N is ~9 for a 2500-token") == std::string::npos &&
                                   hpp.find("~20 for a 6128-token") == std::string::npos,
                               "v0.10.7 F-3(d): the old ceil(n/300) example counts are gone");
                    TEST_CHECK(hpp.find("~9-chunk (2500-token)") == std::string::npos &&
                                   hpp.find("~21-chunk (6128-token)") == std::string::npos,
                               "v0.10.7 F-3(d): the old ceil(n/300) chunk/seconds example is gone");
                    TEST_CHECK(hpp.find("ceil((2500 - 64) / (300 - 64)) = 11") != std::string::npos &&
                                   hpp.find("ceil((6128 - 64) / (300 - 64)) = 26") != std::string::npos,
                               "v0.10.7 F-3(d): the worked examples follow the documented (n-O)/(300-O) formula");
                    TEST_CHECK(hpp.find("~11-chunk (2500-token)") != std::string::npos &&
                                   hpp.find("~26-chunk (6128-token)") != std::string::npos,
                               "v0.10.7 F-3(d): the projection example uses the corrected chunk counts");
                }
            }
        }
    }

    // ---- 7. R2-K + G-09 (round-2): EXHAUSTIVE multilingual needle matrix ----
    // Covers ALL 42 production needle spellings of the 261111_0001 U6 block
    // (translation_common.cpp's U6 table, es/fr/de/pt/ru/it/vi/id/tr/ar/hi/th):
    // every spelling gets its own DISTINCT positive row below, plus an
    // all-caps positive for every cased script (only matchable since G-09
    // folds Unicode simple case at COMPARISON time — under the old ASCII-only
    // fold those rows could never match), plus per-language negatives: the
    // source guard, the R2-7 colon-label exemption, BUG-10
    // require_continuation and F-04 short_source_only. The old round-1 rows
    // duplicated "both cased spellings" byte-identically for fr/pt/ru/vi/tr;
    // each spelling now has exactly one row.
    {
        // Every U6 spelling, verbatim from the production table. Labels
        // (colon-terminated) need no continuation; conversational openers need
        // content after them (BUG-10) on a short source (F-04) — "hi" is 2 cp.
        struct Spelling {
            const wchar_t* needle;
            const char* name;
        };
        static const Spelling kSpellings[] = {
            // Spanish
            {L"traducción:", "es label lower"}, {L"Traducción:", "es label cap"},
            {L"aquí está la traducción", "es opener lower"}, {L"Aquí está la traducción", "es opener cap"},
            // French (note the French space-before-colon spelling)
            {L"traduction :", "fr label lower"}, {L"Traduction :", "fr label cap"},
            {L"voici la traduction", "fr opener lower"}, {L"Voici la traduction", "fr opener cap"},
            // German
            {L"übersetzung:", "de label lower"}, {L"Übersetzung:", "de label cap"},
            {L"hier ist die übersetzung", "de opener lower"}, {L"Hier ist die Übersetzung", "de opener cap"},
            // Portuguese
            {L"tradução:", "pt label lower"}, {L"Tradução:", "pt label cap"},
            {L"aqui está a tradução", "pt opener lower"}, {L"Aqui está a tradução", "pt opener cap"},
            // Russian
            {L"перевод:", "ru label lower"}, {L"Перевод:", "ru label cap"},
            {L"вот перевод", "ru opener lower"}, {L"Вот перевод", "ru opener cap"},
            // Italian
            {L"traduzione:", "it label lower"}, {L"Traduzione:", "it label cap"},
            {L"ecco la traduzione", "it opener lower"}, {L"Ecco la traduzione", "it opener cap"},
            // Vietnamese
            {L"bản dịch:", "vi label lower"}, {L"Bản dịch:", "vi label cap"},
            {L"đây là bản dịch", "vi opener lower"}, {L"Đây là bản dịch", "vi opener cap"},
            // Indonesian
            {L"terjemahan:", "id label lower"}, {L"Terjemahan:", "id label cap"},
            {L"berikut terjemahannya", "id opener lower"}, {L"Berikut terjemahannya", "id opener cap"},
            // Turkish (dotted capital İ for the sentence-initial spelling)
            {L"çeviri:", "tr label lower"}, {L"Çeviri:", "tr label cap"},
            {L"işte çeviri", "tr opener lower"}, {L"İşte çeviri", "tr opener cap"},
            // Arabic (caseless script — one spelling each)
            {L"الترجمة:", "ar label"}, {L"إليك الترجمة", "ar opener"},
            // Hindi (caseless script — one spelling each)
            {L"अनुवाद:", "hi label"}, {L"यह रहा अनुवाद", "hi opener"},
            // Thai (caseless script — one spelling each)
            {L"คำแปล:", "th label"}, {L"นี่คือคำแปล", "th opener"},
        };
        static_assert(sizeof(kSpellings) / sizeof(kSpellings[0]) == 42,
                      "v0.10.7 G-09: the matrix enumerates all 42 U6 production spellings");
        for (const Spelling& sp : kSpellings) {
            const std::wstring out = std::wstring(sp.needle) + L" x"; // continuation for BUG-10
            TEST_CHECK(em::HasMetaLeakage(out, L"hi"),
                       std::string("v0.10.7 G-09 exhaustive positive spelling: ") + sp.name);
        }

        // G-09: all-caps non-ASCII positives — these can ONLY match because
        // the matcher folds Unicode simple case at comparison time (the old
        // ASCII-only fold left the registered cased variants dead).
        struct CapsCase {
            const wchar_t* output;
            const char* name;
        };
        static const CapsCase kCaps[] = {
            {L"TRADUCCIÓN: hola", "es label all-caps"},
            {L"AQUÍ ESTÁ LA TRADUCCIÓN: hola", "es opener all-caps"},
            {L"TRADUCTION : bonjour", "fr label all-caps"},
            {L"VOICI LA TRADUCTION: bonjour", "fr opener all-caps"},
            {L"ÜBERSETZUNG: hallo", "de label all-caps"},
            {L"HIER IST DIE ÜBERSETZUNG: hallo", "de opener all-caps"},
            {L"TRADUÇÃO: olá", "pt label all-caps"},
            {L"AQUI ESTÁ A TRADUÇÃO: olá", "pt opener all-caps"},
            {L"ПЕРЕВОД: привет", "ru label all-caps"},
            {L"ВОТ ПЕРЕВОД: привет", "ru opener all-caps"},
            {L"TRADUZIONE: ciao", "it label all-caps"},
            {L"ECCO LA TRADUZIONE: ciao", "it opener all-caps"},
            {L"BẢN DỊCH: xin chào", "vi label all-caps"},
            {L"ĐÂY LÀ BẢN DỊCH: xin chào", "vi opener all-caps"},
            {L"TERJEMAHAN: halo", "id label all-caps (ASCII)"},
            {L"BERIKUT TERJEMAHANNYA: halo", "id opener all-caps (ASCII)"},
            {L"ÇEVİRİ: merhaba", "tr label all-caps"},
            {L"İŞTE ÇEVİRİ: merhaba", "tr opener all-caps"},
        };
        for (const CapsCase& cc : kCaps) {
            TEST_CHECK(em::HasMetaLeakage(cc.output, L"hi"),
                       std::string("v0.10.7 G-09 all-caps (comparison-time fold): ") + cc.name);
        }

        // Per-language guards, generated for EVERY new language: source
        // guard (needle in source = content), R2-7 translated-label
        // exemption, BUG-10 require_continuation, F-04 short_source_only.
        struct LangNeedles {
            const char* lang;
            const wchar_t* label;    // one colon-label spelling
            const wchar_t* opener;   // one conversational-opener spelling
        };
        static const LangNeedles kLangs[] = {
            {"es", L"Traducción:", L"Aquí está la traducción"},
            {"fr", L"Traduction :", L"Voici la traduction"},
            {"de", L"Übersetzung:", L"Hier ist die Übersetzung"},
            {"pt", L"Tradução:", L"Aqui está a tradução"},
            {"ru", L"Перевод:", L"Вот перевод"},
            {"it", L"Traduzione:", L"Ecco la traduzione"},
            {"vi", L"Bản dịch:", L"Đây là bản dịch"},
            {"id", L"Terjemahan:", L"Berikut terjemahannya"},
            {"tr", L"Çeviri:", L"İşte çeviri"},
            {"ar", L"الترجمة:", L"إليك الترجمة"},
            {"hi", L"अनुवाद:", L"यह रहा अनुवाद"},
            {"th", L"คำแปล:", L"นี่คือคำแปล"},
        };
        for (const LangNeedles& ln : kLangs) {
            const std::string tag = std::string("v0.10.7 G-09 ") + ln.lang;
            // Source guard on the LABEL needle: the source carries it -> content.
            TEST_CHECK(!em::HasMetaLeakage(std::wstring(ln.label) + L" x", ln.label),
                       tag + " source guard (label in source)");
            // Source guard on the OPENER needle.
            TEST_CHECK(!em::HasMetaLeakage(std::wstring(ln.opener) + L": x", ln.opener),
                       tag + " source guard (opener in source)");
            // R2-7: the source line carries its OWN colon label, so the
            // translated label is content, not leakage.
            TEST_CHECK(!em::HasMetaLeakage(std::wstring(ln.label) + L" X", L"Quelle : X"),
                       tag + " R2-7 translated-label exemption");
            // BUG-10: opener with nothing after it is a one-word translation.
            TEST_CHECK(!em::HasMetaLeakage(ln.opener, L"hi"),
                       tag + " BUG-10 opener without continuation does not fire");
            // F-04: the same opener is a legitimate translation of a long source.
            TEST_CHECK(!em::HasMetaLeakage(std::wstring(ln.opener) + L" de cette phrase",
                                           L"this is a longer source text"),
                       tag + " F-04 opener on a >15-cp source does not fire");
        }

        // Clean answers never fire (controls).
        TEST_CHECK(!em::HasMetaLeakage(L"hola", L"hi"), "v0.10.7 G-09: clean one-word answer");
        TEST_CHECK(!em::HasMetaLeakage(L"привет", L"hi"), "v0.10.7 G-09: clean Cyrillic answer");
        TEST_CHECK(!em::HasMetaLeakage(L"สวัสดี", L"hello"), "v0.10.7 G-09: clean Thai answer");
    }

    failures = g_failed_count - failures_before;
    if (failures == 0) {
        std::cout << "[PASS] v0.10.7 engine decode tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] v0.10.7 engine decode tests: " << failures
                  << " check(s) failed." << std::endl;
    }
    return failures;
}
