// ---------------------------------------------------------------------------
// 261122_0002 P2-B (v0.10.10 phased fixes, unit P2-B: engine + coordinator):
//   L5-B core        - the pure chunk coordinator (src/translation_coordinator
//                      .hpp): escaped frame sizing under the frozen 1 MiB wire
//                      cap, byte-exact frame join, rolling (prev_source,
//                      prev_target) glossary pairs, partial-result
//                      preservation, between-frame cancel, per-frame output
//                      validation.
//   Fix 1 (context)  - TranslationTaskContext's three additive members
//                      (abort / out_error_detail / on_chunk_text).
//   Fix 2 (cancel)   - the abort bridge at Translate() entry, the aliased
//                      cancel slot for the cloud chunk loops, and the
//                      LocalCancelForwarder's RequestAbortCurrent fan-out.
//   Fix 4 (preview)  - the cumulative on_chunk_text plumbing on every engine
//                      path.
// Staged as an .inc next to this runner; registered from main() per the
// end-of-file pattern (the p1_u*.inc precedent).
//
// Everything here is either a pure function call or a source pin over
// src/engine.cpp / src/translation_progress.hpp / src/translation_coordinator
// .hpp: no network, no registry, no window, no engine spawn.
// ---------------------------------------------------------------------------

#include "translation_coordinator.hpp" // V10F P2-B: the pure coordinator

namespace {

std::size_t P2BCountOf(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size())) {
        ++n;
    }
    return n;
}

bool P2BReadSrc(const wchar_t* rel, std::string& out) {
    out.clear();
    const std::filesystem::path path = ResolveRepoFile(rel);
    if (path.empty()) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return !out.empty();
}

// ---- (a) Fix 1: the additive context members ------------------------------
void P2BContextFields() {
    std::cout << "[RUN] P2-B Fix 1: TranslationTaskContext additive members..." << std::endl;
    const int failures_before = g_failed_count;

    emebalachat::TranslationTaskContext ctx;
    TEST_CHECK(ctx.cancel == nullptr && ctx.abort == nullptr &&
                   ctx.out_error_detail == nullptr && !ctx.on_chunk && !ctx.on_chunk_text,
               "P2-B Fix 1: a default-constructed context carries every seam in its absent "
               "state (no behavior change for existing callers)");
    TEST_CHECK(!emebalachat::AbortRequested(ctx) &&
                   !emebalachat::PartialResultsOptIn(ctx) &&
                   !emebalachat::ChunkLoopAbortRequested(nullptr),
               "P2-B Fix 1: the default context is neither aborted nor a partial-result opt-in");

    std::wstring preview;
    ctx.on_chunk_text = [&](const std::wstring& t) { preview = t; };
    TEST_CHECK(emebalachat::PartialResultsOptIn(ctx),
               "P2-B Fix 1: the preview sink's presence IS the partial-result opt-in");
    ctx.on_chunk_text(L"abc");
    TEST_CHECK(preview == L"abc",
               "P2-B Fix 1: the preview sink is callable with the cumulative contract type");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-B Fix 1 context fields passed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-B Fix 1 context fields: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (b) Fix 2: the abort truth table (pure + manager-level) --------------
void P2BAbortBridge() {
    std::cout << "[RUN] P2-B Fix 2: abort seam truth table + manager bridge..." << std::endl;
    const int failures_before = g_failed_count;

    // Pure truth table over the pointer form and the context form.
    std::atomic<bool> flag{false};
    TEST_CHECK(!emebalachat::ChunkLoopAbortRequested(nullptr),
               "P2-B Fix 2: a null abort seam never aborts");
    TEST_CHECK(!emebalachat::ChunkLoopAbortRequested(&flag),
               "P2-B Fix 2: an unset abort seam does not abort");
    emebalachat::TranslationTaskContext ctx;
    ctx.abort = &flag;
    TEST_CHECK(!emebalachat::AbortRequested(ctx),
               "P2-B Fix 2: an unset seam through the context form does not abort");
    flag.store(true, std::memory_order_release);
    TEST_CHECK(emebalachat::ChunkLoopAbortRequested(&flag) && emebalachat::AbortRequested(ctx),
               "P2-B Fix 2: a set abort seam reports the abort (release/acquire pair)");

    // Manager-level rows. The host is disabled so the request returns at the
    // REQ-029-B pre-block — no pipe, no network, no spawn (the 261121_0002 F2
    // suite's shape).
    {
        emebalachat::EngineHostConfig host_off;
        host_off.enabled = false;

        // Row 1: an aborted request must classify as Canceled, never as the
        // "model missing" failure.
        emebalachat::TranslationManager aborted_mgr(emebalachat::EngineType::LocalLlama, "");
        aborted_mgr.SetEngineHostConfig(host_off);
        std::atomic<bool> abort_flag{true};
        emebalachat::TranslationTaskContext abort_ctx;
        abort_ctx.abort = &abort_flag;
        emebalachat::TranslationStatus st = emebalachat::TranslationStatus::Ok;
        const std::wstring r1 = aborted_mgr.Translate(L"hello", "KO", "EN", &st, abort_ctx);
        TEST_CHECK(r1.empty() && st == emebalachat::TranslationStatus::Canceled,
                   "P2-B Fix 2: a set ABORT seam (no user token) classifies the request as "
                   "Canceled — the pre-block honors the bridge");

        // Row 2: the same context with the seam clear keeps the strict-local
        // honest failure.
        abort_flag.store(false, std::memory_order_release);
        st = emebalachat::TranslationStatus::Ok;
        const std::wstring r2 = aborted_mgr.Translate(L"hello", "KO", "EN", &st, abort_ctx);
        TEST_CHECK(r2.empty() && st == emebalachat::TranslationStatus::LocalModelMissing,
                   "P2-B Fix 2: a clear abort seam keeps LocalModelMissing (no unconditional "
                   "cancel)");

        // Row 3: the CALLER's context is never mutated by the bridge (the
        // manager works on its own copy; the latch pointer is internal).
        emebalachat::TranslationManager plain_mgr(emebalachat::EngineType::LocalLlama, "");
        plain_mgr.SetEngineHostConfig(host_off);
        emebalachat::TranslationTaskContext plain_ctx;
        st = emebalachat::TranslationStatus::Ok;
        const std::wstring r3 = plain_mgr.Translate(L"hello", "KO", "EN", &st, plain_ctx);
        TEST_CHECK(r3.empty() && st == emebalachat::TranslationStatus::LocalModelMissing &&
                       plain_ctx.abort == nullptr && plain_ctx.cancel == nullptr,
                   "P2-B Fix 2: the bridge works on an internal copy (the caller's null seams "
                   "stay null)");

        // Row 4: the shutdown latch still wins on its own (unchanged REQ-R16).
        emebalachat::TranslationManager latched_mgr(emebalachat::EngineType::LocalLlama, "");
        latched_mgr.SetEngineHostConfig(host_off);
        latched_mgr.RequestCancel();
        emebalachat::TranslationTaskContext latched_ctx;
        st = emebalachat::TranslationStatus::Ok;
        const std::wstring r4 = latched_mgr.Translate(L"hello", "KO", "EN", &st, latched_ctx);
        TEST_CHECK(r4.empty() && st == emebalachat::TranslationStatus::Canceled,
                   "P2-B Fix 2: the REQ-R16 latch still classifies as Canceled on its own");
    }

    // Structural pins over the wiring.
    std::string engine_src;
    TEST_CHECK(P2BReadSrc(L"src\\engine.cpp", engine_src),
               "P2-B Fix 2: src/engine.cpp resolvable (run from the REPO ROOT)");
    if (!engine_src.empty()) {
        TEST_CHECK(engine_src.find("TranslationTaskContext req_ctx = ctx;") != std::string::npos,
                   "P2-B Fix 2: Translate() bridges through ONE internal context copy");
        TEST_CHECK(engine_src.find("req_ctx.abort = &cancel_requested_;") != std::string::npos,
                   "P2-B Fix 2: a null abort seam is bridged to the manager's shutdown latch");
        TEST_CHECK(engine_src.find("if (req_ctx.cancel == nullptr) {") != std::string::npos &&
                       engine_src.find("req_ctx.cancel = req_ctx.abort;") != std::string::npos,
                   "P2-B Fix 2: an UNSET cancel slot is aliased to the abort seam so the "
                   "existing cloud chunk loops honor it");
        TEST_CHECK(engine_src.find("void SetAbortToken(const std::atomic<bool>* abort)") !=
                       std::string::npos,
                   "P2-B Fix 2: the forwarder takes the bridged abort seam");
        TEST_CHECK(engine_src.find("if (Requested())") != std::string::npos &&
                       engine_src.find("ChunkLoopAbortRequested(abort_)") != std::string::npos,
                   "P2-B Fix 2: the forwarder poll loop watches BOTH seams");
        TEST_CHECK(engine_src.find("GoogleTranslate::RequestAbortCurrent();") != std::string::npos &&
                       engine_src.find("OpenAiCompatibleClient::RequestAbortCurrent();") !=
                           std::string::npos,
                   "P2-B Fix 2: a trip closes the in-flight cloud request of BOTH clients");
        TEST_CHECK(engine_src.find("cloud_cancel_forwarder.Start(ctx.cancel);") != std::string::npos &&
                       engine_src.find("openai_cancel_forwarder.Start(ctx.cancel);") !=
                           std::string::npos,
                   "P2-B Fix 2: each cloud leg arms its own forwarder (a blocked WinHTTP "
                   "receive is now abortable)");
        TEST_CHECK(engine_src.find("res.empty() && AbortRequested(req_ctx)") != std::string::npos,
                   "P2-B Fix 2: an empty cloud result caused by the abort seam is attributed "
                   "Canceled before EngineFailed");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-B Fix 2 abort bridge passed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-B Fix 2 abort bridge: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (c) Fix 3: the pure coordinator's five contracts --------------------
void P2BCoordinatorCore() {
    std::cout << "[RUN] P2-B Fix 3: coordinator sizing/pairs/partial/cancel/validation..."
              << std::endl;
    const int failures_before = g_failed_count;

    // (a) frame sizing: a 2 MiB source (with a supplementary code point at the
    // tail) splits into frames that each stay under the escaped budget, split
    // on code-point boundaries, and join back byte-for-byte.
    {
        std::string big;
        big.reserve(2u * 1024u * 1024u + 8u);
        for (std::size_t i = 0; i < 2u * 1024u * 1024u; ++i) big.push_back('a');
        big += "\xF0\x9F\x98\x80"; // U+1F600
        const std::vector<emebalachat::CoordinatorChunk> frames =
            emebalachat::PlanCoordinatorFrames(
                {emebalachat::CoordinatorChunk{big, std::string()}});
        TEST_CHECK(frames.size() >= 3,
                   "P2-B Fix 3 (a): a 2 MiB source needs several frames");
        bool every_fits = true;
        std::string joined;
        for (const emebalachat::CoordinatorChunk& frame : frames) {
            if (emebalachat::CoordinatorJsonEscapedBytes(frame.text) >
                emebalachat::kCoordinatorMaxFrameBytes) {
                every_fits = false;
            }
            joined += frame.text;
            joined += frame.trailing_delimiter;
        }
        TEST_CHECK(every_fits,
                   "P2-B Fix 3 (a): every frame's JSON-escaped size is <= 768 KiB (the 1 MiB "
                   "wire cap with headroom)");
        TEST_CHECK(joined == big,
                   "P2-B Fix 3 (a): the frame join reproduces the source byte-for-byte "
                   "(UTF-8-safe splits, no lost delimiter)");
    }
    // Escape accounting: the planner budgets the ESCAPED size, so a
    // quote/newline-heavy source cannot slip past the wire cap.
    {
        std::string quotes(200000, '"');
        TEST_CHECK(emebalachat::CoordinatorJsonEscapedBytes(quotes) == 400000,
                   "P2-B Fix 3 (a): quotes cost 2 bytes of frame budget each");
        std::string controls(10, '\x01');
        TEST_CHECK(emebalachat::CoordinatorJsonEscapedBytes(controls) == 60,
                   "P2-B Fix 3 (a): a generic control byte costs the 6-byte \\u00XX form");
        std::string escape_heavy(400000, '"'); // 800 KiB of escaped budget
        const std::vector<emebalachat::CoordinatorChunk> escaped_frames =
            emebalachat::PlanCoordinatorFrames(
                {emebalachat::CoordinatorChunk{escape_heavy, std::string()}});
        bool escaped_fits = true;
        for (const emebalachat::CoordinatorChunk& frame : escaped_frames) {
            if (emebalachat::CoordinatorJsonEscapedBytes(frame.text) >
                emebalachat::kCoordinatorMaxFrameBytes) {
                escaped_fits = false;
            }
        }
        TEST_CHECK(escaped_frames.size() >= 2 && escaped_fits,
                   "P2-B Fix 3 (a): an escape-heavy source is split on the ESCAPED size");
    }

    const std::vector<emebalachat::CoordinatorChunk> three{
        emebalachat::CoordinatorChunk{"one\n", "\n"},
        emebalachat::CoordinatorChunk{"two\n", "\n"},
        emebalachat::CoordinatorChunk{"three", ""}};

    // (b) rolling pairs.
    {
        std::vector<std::wstring> prev_src;
        std::vector<std::wstring> prev_tgt;
        emebalachat::CoordinatorSink sink =
            [&](const std::wstring& s, const std::wstring& pv, const std::wstring& pt)
            -> std::optional<std::wstring> {
            prev_src.push_back(pv);
            prev_tgt.push_back(pt);
            return std::wstring(L"T") + s;
        };
        emebalachat::CoordinatorResult res =
            emebalachat::RunTranslationCoordinator(three, sink);
        TEST_CHECK(res.completed_frames == 3 &&
                       res.failed_index == emebalachat::kCoordinatorNoFrame && !res.canceled &&
                       !res.partial,
                   "P2-B Fix 3 (b): three frames complete, none failed, no cancel");
        TEST_CHECK(prev_src.size() == 3 && prev_src[0].empty() && prev_tgt[0].empty(),
                   "P2-B Fix 3 (b): frame 0 receives two EMPTY glossary strings");
        TEST_CHECK(prev_src[1] == L"one\n" && prev_tgt[1] == L"Tone\n",
                   "P2-B Fix 3 (b): frame 1 receives frame 0's (source, target) pair");
        TEST_CHECK(prev_src[2] == L"two\n" && prev_tgt[2] == L"Ttwo\n",
                   "P2-B Fix 3 (b): the pair ROLLS (frame 2 gets frame 1, not frame 0)");
        TEST_CHECK(res.text == L"Tone\n\nTtwo\n\nTthree",
                   "P2-B Fix 3 (b): the stitched text carries every source delimiter verbatim");
    }

    // (c) partial-result preservation, with and without the opt-in.
    {
        int calls = 0;
        emebalachat::CoordinatorSink fails_second =
            [&](const std::wstring& s, const std::wstring&, const std::wstring&)
            -> std::optional<std::wstring> {
            if (++calls == 2) return std::nullopt;
            return std::wstring(L"T") + s;
        };
        emebalachat::CoordinatorOptions opting_in;
        opting_in.preserve_partial = true;
        const emebalachat::CoordinatorResult partial =
            emebalachat::RunTranslationCoordinator(three, fails_second, opting_in);
        TEST_CHECK(partial.failed_index == 1 && partial.completed_frames == 1 && partial.partial,
                   "P2-B Fix 3 (c): a failed frame k reports failed_index = k");
        TEST_CHECK(partial.text == L"Tone\n\n",
                   "P2-B Fix 3 (c): the opt-in keeps the stitched prefix of frames 0..k-1");

        calls = 0;
        const emebalachat::CoordinatorOptions no_opt_in;
        const emebalachat::CoordinatorResult all_or_nothing =
            emebalachat::RunTranslationCoordinator(three, fails_second, no_opt_in);
        TEST_CHECK(all_or_nothing.text.empty() && !all_or_nothing.partial &&
                       all_or_nothing.failed_index == 1,
                   "P2-B Fix 3 (c): WITHOUT the opt-in the result stays all-or-nothing (no "
                   "truncated document reaches a paste path)");
    }

    // (d) cancel between frames (both seams).
    {
        std::atomic<bool> cancel_flag{false};
        int calls = 0;
        emebalachat::CoordinatorOptions cancel_opts;
        cancel_opts.preserve_partial = true;
        cancel_opts.cancel = &cancel_flag;
        emebalachat::CoordinatorSink trips_after_first =
            [&](const std::wstring& s, const std::wstring&, const std::wstring&)
            -> std::optional<std::wstring> {
            ++calls;
            cancel_flag.store(true, std::memory_order_release);
            return std::wstring(L"T") + s;
        };
        const emebalachat::CoordinatorResult canceled =
            emebalachat::RunTranslationCoordinator(three, trips_after_first, cancel_opts);
        TEST_CHECK(canceled.canceled && calls == 1 && canceled.completed_frames == 1,
                   "P2-B Fix 3 (d): a cancel posted in frame 0 stops the run before frame 1");
        TEST_CHECK(canceled.text == L"Tone\n\n" && canceled.partial,
                   "P2-B Fix 3 (d): a canceled run keeps only the opt-in prefix");
        TEST_CHECK(canceled.failed_index == emebalachat::kCoordinatorNoFrame,
                   "P2-B Fix 3 (d): a cancel is NOT reported as a failed frame");

        std::atomic<bool> abort_flag{false};
        int abort_calls = 0;
        emebalachat::CoordinatorOptions abort_opts;
        abort_opts.abort = &abort_flag;
        emebalachat::CoordinatorSink never_runs =
            [&](const std::wstring& s, const std::wstring&, const std::wstring&)
            -> std::optional<std::wstring> {
            ++abort_calls;
            return s;
        };
        abort_flag.store(true, std::memory_order_release);
        const emebalachat::CoordinatorResult aborted =
            emebalachat::RunTranslationCoordinator(three, never_runs, abort_opts);
        TEST_CHECK(aborted.canceled && abort_calls == 0 && aborted.text.empty(),
                   "P2-B Fix 3 (d): a pre-set ABORT seam stops the run before the first frame "
                   "and (without the opt-in) returns nothing");
    }

    // (e) per-frame output validation.
    {
        TEST_CHECK(emebalachat::CoordinatorChunkLooksDegenerate(L"hello", L"  \n"),
                   "P2-B Fix 3 (e): a blank answer for real source text is degenerate");
        std::wstring runaway(5000, L'x');
        TEST_CHECK(emebalachat::CoordinatorChunkLooksDegenerate(L"hello", runaway),
                   "P2-B Fix 3 (e): a runaway expansion is degenerate");
        TEST_CHECK(!emebalachat::CoordinatorChunkLooksDegenerate(L"hello", L"x x x x"),
                   "P2-B Fix 3 (e): a short emphatic answer is NOT degenerate");
        TEST_CHECK(emebalachat::CoordinatorChunkLooksDegenerate(L"a\nb", L"same\nsame\nsame\n"),
                   "P2-B Fix 3 (e): three consecutive identical lines are degenerate");
        TEST_CHECK(!emebalachat::CoordinatorChunkLooksDegenerate(L"same\nsame\nsame\n",
                                                                L"same\nsame\nsame\n"),
                   "P2-B Fix 3 (e): a run the SOURCE carries is the user's content, not a loop");
        TEST_CHECK(!emebalachat::CoordinatorChunkLooksDegenerate(L"hi", L"hi"),
                   "P2-B Fix 3 (e): a short pass-through answer is not rejected");
    }

    // Empty plan / empty sink: a no-op success, never a failure attribution.
    {
        const emebalachat::CoordinatorResult empty_plan =
            emebalachat::RunTranslationCoordinator({}, emebalachat::CoordinatorSink{});
        TEST_CHECK(empty_plan.total_frames == 0 &&
                       empty_plan.failed_index == emebalachat::kCoordinatorNoFrame &&
                       !empty_plan.canceled && empty_plan.text.empty(),
                   "P2-B Fix 3: an empty plan is a no-op success (no false failure)");
    }

    // Structural pins for the engine wiring.
    std::string engine_src;
    if (P2BReadSrc(L"src\\engine.cpp", engine_src)) {
        TEST_CHECK(engine_src.find("PlanCoordinatorFrames(") != std::string::npos &&
                       engine_src.find("RunTranslationCoordinator(frames, sink, opts)") !=
                           std::string::npos &&
                       engine_src.find("TranslateLocalFrames(") != std::string::npos,
                   "P2-B Fix 3: the local leg drives the pure coordinator");
        TEST_CHECK(engine_src.find("text.size() > (kCoordinatorMaxFrameBytes / "
                                   "kCoordinatorMaxEscapeFactor)") != std::string::npos,
                   "P2-B Fix 3: the coordinator path is gated by the allocation-free size "
                   "pre-check");
        TEST_CHECK(engine_src.find("if (coordinator_frames.size() <= 1) {") != std::string::npos &&
                       engine_src.find("coordinator_frames.clear();") != std::string::npos,
                   "P2-B Fix 3: a single frame falls back to the LEGACY single-request path "
                   "(observable behavior preserved)");
        TEST_CHECK(engine_src.find("ToUtf8(text), request_model_id,") != std::string::npos,
                   "P2-B Fix 3: the legacy single-frame request shape is still in the file");
        TEST_CHECK(P2BCountOf(engine_src, "NoteHostSuccess();") == 2,
                   "P2-B Fix 3: still exactly TWO literal 'NoteHostSuccess();' notes (the "
                   "single-request attempt + its retry). F-11 adds the multi-frame success "
                   "note spelled with an argument comment (see V10F-R1 R2), so the "
                   "261121_0002/p1_u4 two-site counts stay green");
        TEST_CHECK(engine_src.find("CoordinatorChunkLooksDegenerate(source_chunk, part)") !=
                       std::string::npos,
                   "P2-B Fix 3: coordinator frames are validated PER FRAME (no whole-stitched "
                   "re-gate reliance)");
        TEST_CHECK(engine_src.find("opts.preserve_partial = PartialResultsOptIn(req_ctx);") !=
                       std::string::npos,
                   "P2-B Fix 3: the partial opt-in comes from the ONE header rule");
        // Cancel attribution: a seam-tripped frame must be re-labeled Canceled
        // (never a failed frame, never a cloud fallback for an aborted task).
        TEST_CHECK(engine_src.find("if (out.canceled_by_seam) {") != std::string::npos &&
                       engine_src.find("out.run.canceled = true;") != std::string::npos,
                   "P2-B Fix 3: a cancel observed inside a frame relabels the whole run Canceled");
        TEST_CHECK(engine_src.find("if (frame_run.run.canceled || IsAnyCancelRequested(req_ctx)) {") !=
                       std::string::npos,
                   "P2-B Fix 3: the manager branch reports Canceled (and never falls back to "
                   "cloud) when the run was canceled");
    } else {
        TEST_CHECK(false, "P2-B Fix 3: src/engine.cpp resolvable (run from the REPO ROOT)");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-B Fix 3 coordinator core passed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-B Fix 3 coordinator core: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (d) Fix 4: the cumulative preview plumbing --------------------------
void P2BPreviewPlumbing() {
    std::cout << "[RUN] P2-B Fix 4: cumulative preview plumbing..." << std::endl;
    const int failures_before = g_failed_count;

    // The CUMULATIVE contract: each tick is the whole stitched prefix, so a
    // receiver that REPLACES its text is correct, and a client that streams
    // deltas and then emits a final whole-answer tick cannot duplicate text.
    const std::vector<emebalachat::CoordinatorChunk> two{
        emebalachat::CoordinatorChunk{"a\n", "\n"},
        emebalachat::CoordinatorChunk{"b", ""}};
    std::vector<std::wstring> ticks;
    emebalachat::CoordinatorOptions opts;
    opts.on_chunk_text = [&](const std::wstring& t) { ticks.push_back(t); };
    emebalachat::CoordinatorSink sink =
        [](const std::wstring& s, const std::wstring&, const std::wstring&)
        -> std::optional<std::wstring> {
        return std::wstring(L"[") + s + L"]";
    };
    const emebalachat::CoordinatorResult res =
        emebalachat::RunTranslationCoordinator(two, sink, opts);
    TEST_CHECK(ticks.size() == 2 && ticks[0] == L"[a\n]\n" && ticks[1] == L"[a\n]\n[b]",
               "P2-B Fix 4: every tick is the CUMULATIVE stitched text (replace-safe)");
    TEST_CHECK(ticks.size() == res.completed_frames,
               "P2-B Fix 4: one preview tick per completed frame");

    std::string engine_src;
    if (P2BReadSrc(L"src\\engine.cpp", engine_src)) {
        TEST_CHECK(P2BCountOf(engine_src, "req_ctx.on_chunk_text(") >= 4,
                   "P2-B Fix 4: all engine paths tick the preview (google, openai, both local "
                   "success legs) and the coordinator streams per frame");
        TEST_CHECK(engine_src.find("req_ctx.on_chunk_text && !res.empty()") != std::string::npos ||
                       engine_src.find("if (req_ctx.on_chunk_text && !res.empty()) {") !=
                           std::string::npos,
                   "P2-B Fix 4: a cloud tick only fires for real text (never an empty failure)");
        TEST_CHECK(engine_src.find("if (req_ctx.on_chunk_text && !host_out.empty()) {") !=
                       std::string::npos,
                   "P2-B Fix 4: the local single-request success legs tick too");
        TEST_CHECK(engine_src.find("opts.on_chunk_text = req_ctx.on_chunk_text;") !=
                       std::string::npos,
                   "P2-B Fix 4: the coordinator receives the caller's preview sink");
        TEST_CHECK(engine_src.find("req_ctx.out_error_detail->clear();") != std::string::npos &&
                       engine_src.find("*req_ctx.out_error_detail = openai_error_detail;") !=
                           std::string::npos,
                   "P2-B Fix 1/RISK-03: the cause slot is cleared at entry and filled on the "
                   "openai leg");
    } else {
        TEST_CHECK(false, "P2-B Fix 4: src/engine.cpp resolvable (run from the REPO ROOT)");
    }

    // Header pins: the additive members and the coordinator constants.
    std::string progress_src;
    if (P2BReadSrc(L"src\\translation_progress.hpp", progress_src)) {
        TEST_CHECK(progress_src.find("const std::atomic<bool>* abort = nullptr;") !=
                           std::string::npos &&
                       progress_src.find("std::string* out_error_detail = nullptr;") !=
                           std::string::npos &&
                       progress_src.find("std::function<void(const std::wstring&)> on_chunk_text;") !=
                           std::string::npos,
                   "P2-B Fix 1: the three additive context members are declared verbatim "
                   "(P2-A/P2-C/P2-E code against these names)");
        TEST_CHECK(progress_src.find("inline bool AbortRequested(const TranslationTaskContext& ctx)") !=
                           std::string::npos &&
                       progress_src.find(
                           "inline bool PartialResultsOptIn(const TranslationTaskContext& ctx)") !=
                           std::string::npos,
                   "P2-B: the abort and partial-opt-in rules live in ONE header (no drift)");
    } else {
        TEST_CHECK(false, "P2-B Fix 1: src/translation_progress.hpp resolvable");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-B Fix 4 preview plumbing passed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-B Fix 4 preview plumbing: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

} // namespace

// V10F-R1 (261122_0002 fix-list round 1, unit R2: engine + coordinator +
// CMake): F-11 (the multi-frame coordinator success notes host success; a
// partial prefix surfaces TranslationStatus::Partial), F-12 (the contract TU
// src/translation_coordinator.cpp joins Emebalachat_core), F-17 engine half
// (request-time http consent-origin gate in the OpenAI routing leg).
namespace {
void P2BR2V10FR1Fixes() {
    std::cout << "[RUN] V10F-R1 R2: F-11 coordinator success/partial + F-12 CMake "
                 "listing + F-17 engine origin gate..."
              << std::endl;
    const int failures_before = g_failed_count;

    // F-11: the new status value exists and is distinct from Ok.
    TEST_CHECK(emebalachat::TranslationStatus::Partial != emebalachat::TranslationStatus::Ok,
               "V10F-R1 F-11: Partial is a distinct status, never the plain-Ok attribution");

    std::string eng_hpp;
    if (P2BReadSrc(L"src\\engine.hpp", eng_hpp)) {
        const std::size_t lmm = eng_hpp.find("LocalModelMissing,");
        const std::size_t partial = eng_hpp.find("Partial,");
        TEST_CHECK(lmm != std::string::npos && partial != std::string::npos && partial > lmm,
                   "V10F-R1 F-11: Partial is appended AFTER LocalModelMissing, so every "
                   "existing status keeps its integer value");
    } else {
        TEST_CHECK(false, "V10F-R1 F-11: src/engine.hpp resolvable");
    }

    std::string engine_src;
    if (P2BReadSrc(L"src\\engine.cpp", engine_src)) {
        // F-11: the COMPLETE multi-frame run notes the host success. It is
        // spelled with an argument comment so the literal "NoteHostSuccess();"
        // count of the two single-request legs stays at exactly 2 (the
        // 261121_0002 / p1_u4 / p3_c4 pins).
        TEST_CHECK(P2BCountOf(engine_src, "NoteHostSuccess();") == 2,
                   "V10F-R1 F-11: still exactly TWO literal single-request host-success "
                   "sites (the counted pins stay green)");
        TEST_CHECK(engine_src.find("NoteHostSuccess(/* V10F-R1 F-11") != std::string::npos,
                   "V10F-R1 F-11: the COMPLETE multi-frame run notes host success (the "
                   "repair streak clears on a successful long-form run)");
        const std::size_t note_at = engine_src.find("NoteHostSuccess(/* V10F-R1 F-11");
        const std::size_t note_guard = engine_src.rfind("if (!frame_run.run.partial) {", note_at);
        TEST_CHECK(note_guard != std::string::npos && note_guard < note_at,
                   "V10F-R1 F-11: the success note sits under the !partial guard (a partial "
                   "run arms neither recorder)");
        TEST_CHECK(engine_src.find("frame_run.run.partial ? TranslationStatus::Partial") !=
                       std::string::npos &&
                       engine_src.find(": TranslationStatus::Ok);") != std::string::npos,
                   "V10F-R1 F-11: a partial multi-frame prefix surfaces Partial through "
                   "out_status instead of plain Ok");
        TEST_CHECK(engine_src.find("does not clear the repair streak; the next single-frame") ==
                       std::string::npos,
                   "V10F-R1 F-11: the retired P2-B 'deliberately NOT called' note is gone");

        // F-17 engine half: the request-time origin gate in the OpenAI leg.
        const std::size_t openai_leg = engine_src.find("if (preferred == EngineType::OpenAi) {");
        TEST_CHECK(openai_leg != std::string::npos,
                   "V10F-R1 F-17: the OpenAI explicit-pick leg is locatable");
        if (openai_leg != std::string::npos) {
            const std::size_t leg_end = engine_src.find("return openai_call();", openai_leg);
            const std::string leg_body =
                engine_src.substr(openai_leg, leg_end == std::string::npos
                                                  ? std::string::npos
                                                  : leg_end - openai_leg);
            TEST_CHECK(leg_body.find("openai_http_consent_origin_denied()") !=
                           std::string::npos &&
                           leg_body.find("set_status(TranslationStatus::CloudConsentBlocked);") !=
                               std::string::npos,
                       "V10F-R1 F-17: an http origin mismatch fails closed in the routing "
                       "leg and surfaces the existing consent-blocked status");
            TEST_CHECK(leg_body.find("ENGINE/Translate/045c") != std::string::npos,
                       "V10F-R1 F-17: the denial is observable in the diagnostic log "
                       "(shape-only)");
        }
        TEST_CHECK(engine_src.find("OpenAiHttpConsentOriginMatches(openai_cfg.base_url,") !=
                       std::string::npos &&
                       engine_src.find("consent_cfg.GetHttpConsentOrigin())") !=
                       std::string::npos,
                   "V10F-R1 F-17: the gate codes against the R1 pure helper with the R3 "
                   "persisted-origin accessor");
        TEST_CHECK(engine_src.find("ClassifyOpenAiBaseUrl(openai_cfg.base_url) !=\n"
                                   "            OpenAiUrlSecurity::Http") != std::string::npos ||
                       engine_src.find("OpenAiUrlSecurity::Http") != std::string::npos,
                   "V10F-R1 F-17: the origin gate applies to http bases only (https/invalid "
                   "bases keep the client's own gates)");
    } else {
        TEST_CHECK(false,
                   "V10F-R1 F-11/F-17: src/engine.cpp resolvable (run from the REPO ROOT)");
    }

    // F-12: the contract/static-assert TU participates in the build.
    std::string cmake_txt;
    if (P2BReadSrc(L"CMakeLists.txt", cmake_txt)) {
        TEST_CHECK(cmake_txt.find("src/translation_coordinator.cpp") != std::string::npos,
                   "V10F-R1 F-12: src/translation_coordinator.cpp is listed in "
                   "Emebalachat_core (the contract TU actually compiles)");
    } else {
        TEST_CHECK(false, "V10F-R1 F-12: CMakeLists.txt resolvable");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] V10F-R1 R2 fixes passed." << std::endl;
    } else {
        std::cout << "[FAIL] V10F-R1 R2 fixes: " << (g_failed_count - failures_before)
                  << " check(s) failed." << std::endl;
    }
}
} // namespace

// Single registration entry (the run_tests.cpp end-of-file pattern): the
// integrator adds `#include "p2_b_tests.inc"` to the include cluster and
// `TestP2BCoordinatorAndCancelBridge();` inside main().
void TestP2BCoordinatorAndCancelBridge() {
    P2BContextFields();
    P2BAbortBridge();
    P2BCoordinatorCore();
    P2BPreviewPlumbing();
    P2BR2V10FR1Fixes(); // V10F-R1 (fix-list round 1, unit R2): F-11 / F-12 / F-17
}
