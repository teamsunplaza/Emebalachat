// ---------------------------------------------------------------------------
// 261121_0002 (leftover release v0.10.9, sub-task T5): the multi-chunk
// translation PROGRESS + CANCEL UI, worker half. Staged as an .inc next to
// this runner; registered at the end of main() after the v0.10.9 U-A chunker
// suite, per the end-of-file pattern.
//
// Coverage:
//   (a) The pure seam predicates (src/translation_progress.hpp):
//       ShowChunkProgress only reports genuine multi-chunk plans, and
//       ChunkLoopCancelRequested is false for a null token and true once the
//       flag is set.
//   (b) CancelOutcomeSuppressesEnter: a user cancel (and ONLY a user cancel)
//       holds the intercepted Enter.
//   (c) kLocalProgressChunkUnits parity: src/engine.cpp's local predicted-plan
//       cap is the literal 1200 and equals the engine repo's
//       kV109LocalChunkMaxUtf16Units (source pin on
//       src/engine_core/translation_common.hpp + the compiled constant).
//   (d) worker.cpp / worker.hpp structure: the per-task token lifecycle, the
//       release-store cancel request, the Canceled branch (no streak, no
//       modal, notice invoked), the tone arm's explicit Canceled exclusion,
//       the Enter gate, and the task-end progress/cancelling clear.
//   (e) engine.cpp structure (mirrors the engine-manager unit's report): the
//       cancel-first attribution after both cloud empty returns, the local
//       discard BEFORE every NoteHostSuccess, the retry warrant's user-token
//       OR, and the user-cancel fallback block.
//   (f) The two cloud chunk loops poll the cancel token ABOVE their loop
//       budget checks (index comparison), and every tick site is gated by
//       ShowChunkProgress so single-chunk runs keep today's badge.
//   (g) The badge's local plan-known label: BadgeReplaceAll exists and the
//       BadgeTranslatingChunks arm precedes the {c}/{t} arm.
//   (h) Every selector locale carries the {c}/{t}/{n} badge tokens plus the
//       three new notices (a dropped token would render raw brace text).
//   (i) 261121_0002 fix round 2 unit B: F2 — manager-level pin that the
//       REQ-029-B LocalModelMissing pre-block honors the per-request user
//       token (Canceled, never LocalModelMissing), with the unset-flag
//       regression pair; plus source pins for the F2 OR'd latch (and its
//       retry-warrant mirror), the F5 badge cancelling-handler guard, and
//       the F7 observer-gated local plan computation.
//   (j) 261122_0001 A4 (F9): the {c}/{t} FILL itself (exported pure helper):
//       index/total order, position independence, replace-all, token-free
//       passthrough, the production template, every selector locale, and the
//       Render() call-site argument order.
//
// Every pin is a cheap, deterministic string/index check over the sources; the
// suite never touches the network, the registry, or a window.
// ---------------------------------------------------------------------------

void TestTranslationProgressCancel() {
    std::cout << "[RUN] Testing 261121_0002 multi-chunk progress + cancel (worker)..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) the pure seam predicates ---------------------------------------
    TEST_CHECK(!emebalachat::ShowChunkProgress(0) && !emebalachat::ShowChunkProgress(1),
               "261121_0002: an empty / single-chunk plan reports NO progress (badge stays byte-identical)");
    TEST_CHECK(emebalachat::ShowChunkProgress(2) && emebalachat::ShowChunkProgress(12),
               "261121_0002: two or more chunks switch the badge to its progress label");
    std::atomic<bool> cancel_flag{false};
    TEST_CHECK(!emebalachat::ChunkLoopCancelRequested(nullptr),
               "261121_0002: a default (null) token never reports a cancel");
    TEST_CHECK(!emebalachat::ChunkLoopCancelRequested(&cancel_flag),
               "261121_0002: an unset token reports no cancel");
    cancel_flag.store(true, std::memory_order_release);
    TEST_CHECK(emebalachat::ChunkLoopCancelRequested(&cancel_flag),
               "261121_0002: a set token reports the cancel (release store / acquire load pair)");

    // ---- (b) the Enter-suppression predicate --------------------------------
    TEST_CHECK(CancelOutcomeSuppressesEnter(TranslationStatus::Canceled),
               "261121_0002: a user cancel HOLDS the intercepted Enter");
    TEST_CHECK(!CancelOutcomeSuppressesEnter(TranslationStatus::Ok) &&
                   !CancelOutcomeSuppressesEnter(TranslationStatus::EngineFailed) &&
                   !CancelOutcomeSuppressesEnter(TranslationStatus::CloudConsentBlocked) &&
                   !CancelOutcomeSuppressesEnter(TranslationStatus::LocalModelMissing),
               "261121_0002: every non-cancel outcome keeps the established Enter contract "
               "(failures still forward the original by design)");
    TEST_CHECK(!CancelOutcomeSuppressesEnter(TranslationStatus::InputEmpty),
               "261121_0002: an empty input is not a cancel either");

    // ---- (c) kLocalProgressChunkUnits parity --------------------------------
    // The engine repo header is in this runner's TOP include block, so the
    // compiled constant joins the source pins below (a drift in either
    // direction trips one of the two).
    {
        volatile std::size_t engine_cap = emebalachat::kV109LocalChunkMaxUtf16Units;
        TEST_CHECK(engine_cap == 1200,
                   "261121_0002: the engine's kV109LocalChunkMaxUtf16Units is 1200 (compiled constant)");
    }
    auto read_repo_src = [](const wchar_t* rel, std::string& out) -> bool {
        const std::filesystem::path path = ResolveRepoFile(rel);
        if (path.empty()) return false;
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out.assign((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
        return !out.empty();
    };

    std::string engine_src;
    if (!read_repo_src(L"src\\engine.cpp", engine_src)) {
        TEST_CHECK(false, "261121_0002: src/engine.cpp resolvable from the test CWD "
                          "(run the suite from the REPO ROOT - build/ CWDs skip source pins)");
    } else {
        TEST_CHECK(engine_src.find("constexpr std::size_t kLocalProgressChunkUnits = 1200") !=
                       std::string::npos,
                   "261121_0002: the manager's local plan cap is the literal 1200");
        TEST_CHECK(engine_src.find("SplitTextIntoTranslationChunks(ToUtf8(text), kLocalProgressChunkUnits)") !=
                       std::string::npos,
                   "261121_0002: the plan is built with the shared splitter at that cap");
        // The parity itself: the engine repo splits its decode ladder at
        // kV109LocalChunkMaxUtf16Units, so the client-side prediction must be
        // the SAME number.
        std::string engine_common_src;
        if (!read_repo_src(L"src\\engine_core\\translation_common.hpp", engine_common_src)) {
            std::cout << "[SKIP] src/engine_core/translation_common.hpp not resolvable "
                         "(EMEBALA_ENGINE_REPO_DIR); the kV109 parity source pin was skipped."
                      << std::endl;
        } else {
            TEST_CHECK(engine_common_src.find("kV109LocalChunkMaxUtf16Units = 1200") !=
                           std::string::npos,
                       "261121_0002: the engine repo's chunk cap is 1200 (kLocalProgressChunkUnits parity)");
        }
    }

    // ---- (d) worker.cpp / worker.hpp structure ------------------------------
    std::string worker_src;
    std::string worker_hpp_src;
    if (!read_repo_src(L"src\\worker.cpp", worker_src) ||
        !read_repo_src(L"src\\worker.hpp", worker_hpp_src)) {
        TEST_CHECK(false, "261121_0002: src/worker.cpp / src/worker.hpp resolvable from the "
                          "test CWD (run the suite from the REPO ROOT)");
    } else {
        // Header half: the one-definition predicate + the seam declarations.
        TEST_CHECK(worker_hpp_src.find("constexpr bool CancelOutcomeSuppressesEnter(TranslationStatus s)") !=
                       std::string::npos,
                   "261121_0002: CancelOutcomeSuppressesEnter has ONE definition (worker.hpp)");
        TEST_CHECK(worker_hpp_src.find("void RequestCancelTranslation();") != std::string::npos,
                   "261121_0002: worker.hpp declares the GUI-thread cancel request");
        TEST_CHECK(worker_hpp_src.find("SetTranslationCancelledCallback") != std::string::npos &&
                       worker_hpp_src.find("SetFirstMultiChunkHintCallback") != std::string::npos,
                   "261121_0002: worker.hpp declares both new SetXxxCallback seams");
        TEST_CHECK(worker_hpp_src.find("std::shared_ptr<std::atomic_bool> cancel_token_;") !=
                       std::string::npos,
                   "261121_0002: the per-task cancel token is a shared_ptr<atomic_bool> member");
        TEST_CHECK(worker_hpp_src.find("bool first_multichunk_hint_fired_") != std::string::npos,
                   "261121_0002: the multi-chunk hint has its one-shot latch");

        // RequestCancelTranslation: idle no-op, release-store, immediate badge
        // feedback.
        const size_t req_pos = worker_src.find("void PipelineWorker::RequestCancelTranslation()");
        const size_t req_end = worker_src.find("void PipelineWorker::OnFirstMultiChunkTick");
        TEST_CHECK(req_pos != std::string::npos && req_end != std::string::npos &&
                       req_end > req_pos,
                   "261121_0002 pin: RequestCancelTranslation is locatable in worker.cpp");
        if (req_pos != std::string::npos && req_end != std::string::npos && req_end > req_pos) {
            const std::string req_body = worker_src.substr(req_pos, req_end - req_pos);
            TEST_CHECK(req_body.find("token->store(true, std::memory_order_release)") !=
                           std::string::npos,
                       "261121_0002 pin: the cancel flag is published with a RELEASE store");
            TEST_CHECK(req_body.find("if (!token)") != std::string::npos,
                       "261121_0002 pin: an idle worker makes the request a no-op");
            TEST_CHECK(req_body.find("badge_.SetCancellingThreadSafe(true)") != std::string::npos,
                       "261121_0002 pin: the request arms the badge's cancelling feedback");
        }

        // ExecuteTask: the task context built immediately before the call.
        TEST_CHECK(worker_src.find("TranslationTaskContext ctx;") != std::string::npos &&
                       worker_src.find("ctx.cancel = cancel_token.get();") != std::string::npos,
                   "261121_0002 pin: ExecuteTask hands the per-task token to the engine");
        TEST_CHECK(worker_src.find("ctx.on_chunk = [this](std::size_t index, std::size_t total)") !=
                       std::string::npos,
                   "261121_0002 pin: ExecuteTask installs the chunk-tick callback");
        TEST_CHECK(worker_src.find("badge_.SetProgress(index, total);") != std::string::npos &&
                       worker_src.find("OnFirstMultiChunkTick(total);") != std::string::npos,
                   "261121_0002 pin: each tick reaches the badge and the one-shot hint");
        const size_t translate_call = worker_src.find(
            "engine_.Translate(engine_input, resolved_engine_src, snap.type_target_language, &status, ctx)");
        const size_t token_clear = worker_src.find("cancel_guard.Clear();");
        TEST_CHECK(translate_call != std::string::npos,
                   "261121_0002 pin: the engine call carries the task context");
        TEST_CHECK(token_clear != std::string::npos && token_clear > translate_call,
                   "261121_0002 pin: the token is cleared right after the engine leg returns");

        // The Canceled branch: notice only, and it lands BEFORE the streak
        // block (the branch must not be able to feed the streak).
        const size_t canceled_branch = worker_src.find("if (status == TranslationStatus::Canceled) {");
        const size_t streak_first_increment = worker_src.find("++consecutive_engine_failures_");
        TEST_CHECK(canceled_branch != std::string::npos && streak_first_increment != std::string::npos &&
                       canceled_branch < streak_first_increment,
                   "261121_0002 pin: the Canceled branch precedes the repair-streak block");
        if (canceled_branch != std::string::npos) {
            const size_t branch_end = worker_src.find("if (engine_unavailable_cb_)", canceled_branch);
            TEST_CHECK(branch_end != std::string::npos && branch_end > canceled_branch,
                       "261121_0002 pin: the Canceled branch is bounded by the streak block");
            if (branch_end != std::string::npos && branch_end > canceled_branch) {
                const std::string branch_body =
                    worker_src.substr(canceled_branch, branch_end - canceled_branch);
                TEST_CHECK(branch_body.find("translation_cancelled_cb_()") != std::string::npos,
                           "261121_0002 pin: the Canceled branch invokes the cancelled-notice callback");
                TEST_CHECK(branch_body.find("++consecutive_engine_failures_") == std::string::npos &&
                               branch_body.find("engine_unavailable_cb_(") == std::string::npos,
                           "261121_0002 pin: the Canceled branch neither counts a failure nor "
                           "surfaces the engine-unavailable modal");
            }
        }

        // The failure-tone arm must exclude Canceled EXPLICITLY: the span from
        // its condition to the tone call names no Canceled comparison.
        const size_t tone_arm = worker_src.find("if (!pasted && translated.empty() &&");
        const size_t tone_call = worker_src.find("PlaySoundAsync(SoundType::Disable)", tone_arm);
        TEST_CHECK(tone_arm != std::string::npos && tone_call != std::string::npos &&
                       tone_call > tone_arm,
                   "261121_0002 pin: the failure-tone arm is locatable");
        if (tone_arm != std::string::npos && tone_call != std::string::npos && tone_call > tone_arm) {
            const std::string tone_body = worker_src.substr(tone_arm, tone_call - tone_arm);
            TEST_CHECK(tone_body.find("status == TranslationStatus::Canceled") == std::string::npos,
                       "261121_0002 pin: the tone arm excludes TranslationStatus::Canceled");
            TEST_CHECK(worker_src.find("TranslationStatus::Canceled is deliberately NOT") !=
                           std::string::npos,
                       "261121_0002 pin: the exclusion is stated explicitly next to the arm");
        }

        // The Enter gate consumes the shared predicate and lands after the
        // identity arm, before the injection.
        const size_t enter_gate = worker_src.find("if (CancelOutcomeSuppressesEnter(status))");
        const size_t enter_inject = worker_src.find("if (inject_enter && h1_ok)");
        TEST_CHECK(enter_gate != std::string::npos && enter_inject != std::string::npos &&
                       enter_gate < enter_inject,
                   "261121_0002 pin: the Enter gate consumes CancelOutcomeSuppressesEnter "
                   "before the injection");
        if (enter_gate != std::string::npos) {
            const size_t gate_end = worker_src.find("}", enter_gate);
            TEST_CHECK(gate_end != std::string::npos &&
                           worker_src.substr(enter_gate, gate_end - enter_gate)
                                   .find("inject_enter = false;") != std::string::npos,
                       "261121_0002 pin: the gate clears inject_enter (the Enter is held)");
        }

        // Task-end clear: progress + cancelling are reset BEFORE the status.
        const size_t progress_clear = worker_src.find("badge_.SetProgress(0, 0);");
        const size_t cancelling_clear = worker_src.find("badge_.SetCancellingThreadSafe(false);");
        const size_t status_active = worker_src.find("badge_.SetStatus(BadgeStatus::Active);");
        TEST_CHECK(progress_clear != std::string::npos && cancelling_clear != std::string::npos &&
                       status_active != std::string::npos && progress_clear < status_active &&
                       cancelling_clear < status_active,
                   "261121_0002 pin: the task-end path clears progress/cancelling BEFORE the "
                   "status reset");

        // One-shot hint: ShowChunkProgress gate + latch consumed once.
        const size_t tick_pos = worker_src.find("void PipelineWorker::OnFirstMultiChunkTick");
        TEST_CHECK(tick_pos != std::string::npos &&
                       worker_src.find("if (!ShowChunkProgress(total)) return;", tick_pos) !=
                           std::string::npos &&
                       worker_src.find("first_multichunk_hint_fired_ = true;", tick_pos) !=
                           std::string::npos,
                   "261121_0002 pin: the multi-chunk hint is one-shot and gated on the plan size");
    }

    // ---- (e) engine.cpp structure (manager half, per the T3 report) ---------
    if (engine_src.empty()) {
        std::cout << "[SKIP] src/engine.cpp unavailable; the manager structural pins were skipped."
                  << std::endl;
    } else {
        // e1: cancel-first attribution after BOTH cloud empty returns.
        const size_t google_seam = engine_src.find("auto cloud_call = [&]()");
        const size_t openai_seam = engine_src.find("auto openai_call = [&]()");
        const size_t cloud_failed = engine_src.find(
            "set_status(res.empty() ? TranslationStatus::EngineFailed : TranslationStatus::Ok);");
        TEST_CHECK(google_seam != std::string::npos && openai_seam != std::string::npos &&
                       cloud_failed != std::string::npos && google_seam < cloud_failed &&
                       cloud_failed < openai_seam,
                   "261121_0002 pin: the google seam's failure attribution is locatable");
        if (google_seam != std::string::npos && openai_seam != std::string::npos &&
            openai_seam > google_seam) {
            const std::string google_body = engine_src.substr(google_seam, openai_seam - google_seam);
            const size_t google_forward = google_body.find("GoogleTranslate::Translate(");
            const size_t google_cancel = google_body.find("ChunkLoopCancelRequested(ctx.cancel)");
            const size_t google_failed = google_body.find("TranslationStatus::EngineFailed");
            TEST_CHECK(google_forward != std::string::npos && google_cancel != std::string::npos &&
                           google_failed != std::string::npos && google_forward < google_cancel &&
                           google_cancel < google_failed,
                       "261121_0002 pin: the google leg consults the user token AFTER the call and "
                       "BEFORE the EngineFailed attribution");
        }
        const size_t openai_end = engine_src.find("// REQ-045 P4-3: OpenAI explicit pick", openai_seam);
        TEST_CHECK(openai_seam != std::string::npos && openai_end != std::string::npos &&
                       openai_end > openai_seam,
                   "261121_0002 pin: the openai seam is locatable");
        if (openai_seam != std::string::npos && openai_end != std::string::npos &&
            openai_end > openai_seam) {
            const std::string openai_body = engine_src.substr(openai_seam, openai_end - openai_seam);
            const size_t openai_cancel = openai_body.find("ChunkLoopCancelRequested(ctx.cancel)");
            const size_t openai_failed = openai_body.find("ENGINE/Translate/045b");
            TEST_CHECK(openai_cancel != std::string::npos && openai_failed != std::string::npos &&
                           openai_cancel < openai_failed,
                       "261121_0002 pin: the openai leg's cancel check precedes its failure DIAG");
        }

        // e2: the local discard sits BEFORE NoteHostSuccess on BOTH attempts
        // (there are exactly two success notes - first attempt + retry).
        size_t success_pos = engine_src.find("NoteHostSuccess();");
        int success_sites = 0;
        int discard_ok = 0;
        while (success_pos != std::string::npos) {
            ++success_sites;
            const size_t cancel_before = engine_src.rfind("ChunkLoopCancelRequested(ctx.cancel)",
                                                          success_pos);
            if (cancel_before != std::string::npos &&
                engine_src.find("set_status(TranslationStatus::Canceled);", cancel_before) <
                    success_pos) {
                ++discard_ok;
            }
            success_pos = engine_src.find("NoteHostSuccess();", success_pos + 1);
        }
        TEST_CHECK(success_sites == 2,
                   "261121_0002 pin: the local leg notes exactly two host successes (attempt + retry)");
        TEST_CHECK(discard_ok == success_sites && success_sites == 2,
                   "261121_0002 pin: BOTH attempts discard a cancelled result BEFORE NoteHostSuccess");

        // e3: the REQ-051 retry warrant consumes the user token (OR with the
        // REQ-R16 shutdown latch).
        const size_t warrant = engine_src.find("EngineHostTransientRetryWarranted(host_err,");
        const size_t backoff = engine_src.find("::Sleep(kEngineHostTransientRetryBackoffMs)", warrant);
        TEST_CHECK(warrant != std::string::npos && backoff != std::string::npos && backoff > warrant,
                   "261121_0002 pin: the transient-retry warrant is locatable");
        if (warrant != std::string::npos && backoff != std::string::npos && backoff > warrant) {
            const std::string warrant_body = engine_src.substr(warrant, backoff - warrant);
            const size_t latch_operand = warrant_body.find("cancel_requested_.load");
            const size_t user_operand = warrant_body.find("ChunkLoopCancelRequested(ctx.cancel)");
            TEST_CHECK(warrant_body.find("cancel_requested_.load(std::memory_order_acquire) ||") !=
                           std::string::npos,
                       "261121_0002 pin: the warrant ORs the user token onto the shutdown latch");
            TEST_CHECK(latch_operand != std::string::npos && user_operand != std::string::npos &&
                           latch_operand < user_operand,
                       "261121_0002 pin: the user token is the second operand of that OR");
        }

        // e4: a user cancel blocks the Auto -> cloud fallback after a failed
        // local attempt.
        const size_t retry_failed = engine_src.find("ENGINE/Translate/047");
        const size_t auto_fallback = engine_src.find("if (preferred == EngineType::Auto)", retry_failed);
        TEST_CHECK(retry_failed != std::string::npos && auto_fallback != std::string::npos &&
                       auto_fallback > retry_failed,
                   "261121_0002 pin: the post-retry convergence block is locatable");
        if (retry_failed != std::string::npos && auto_fallback != std::string::npos &&
            auto_fallback > retry_failed) {
            const std::string fallback_body =
                engine_src.substr(retry_failed, auto_fallback - retry_failed);
            TEST_CHECK(fallback_body.find("ChunkLoopCancelRequested(ctx.cancel)") != std::string::npos &&
                           fallback_body.find("set_status(TranslationStatus::Canceled);") !=
                               std::string::npos,
                       "261121_0002 pin: a user cancel converges to Canceled BEFORE the Auto cloud "
                       "fallback (the text is never transmitted after an abort)");
        }

        // e5: the local plan-known tick precedes the pipe request, and every
        // tick site is gated by ShowChunkProgress (single-chunk runs keep the
        // byte-identical pre-feature badge).
        const size_t plan_tick = engine_src.find("ctx.on_chunk(0, plan.size())");
        const size_t first_try = engine_src.find("engine_host::TryTranslate(", plan_tick);
        TEST_CHECK(plan_tick != std::string::npos && first_try != std::string::npos &&
                       first_try > plan_tick,
                   "261121_0002 pin: the (0, n) plan tick precedes the pipe request");
        TEST_CHECK(engine_src.find("if (ShowChunkProgress(plan.size()) && ctx.on_chunk)") !=
                       std::string::npos,
                   "261121_0002 pin: the local plan tick is gated by ShowChunkProgress");

        // e6 (F2, fix round 2 unit B): the REQ-029-B pre-block ORs the
        // per-request user token onto the REQ-R16 shutdown latch — a
        // user-cancelled task queued on translate_mutex_ classifies as
        // Canceled, never LocalModelMissing (verifier: a revert of this OR
        // must trip BOTH this pin and the manager-level (i) check).
        const size_t f2_preblock = engine_src.find(
            "if (preferred == EngineType::LocalLlama && !HostCouldServeFor(host_cfg)) {");
        const size_t f2_verdict = engine_src.find(
            "set_status(TranslationStatus::LocalModelMissing);", f2_preblock);
        TEST_CHECK(f2_preblock != std::string::npos && f2_verdict != std::string::npos &&
                       f2_verdict > f2_preblock,
                   "261121_0002 F2 pin: the REQ-029-B pre-block and its verdict are locatable");
        if (f2_preblock != std::string::npos && f2_verdict != std::string::npos &&
            f2_verdict > f2_preblock) {
            const std::string f2_body = engine_src.substr(f2_preblock, f2_verdict - f2_preblock);
            const size_t f2_latch = f2_body.find("cancel_requested_.load(std::memory_order_acquire)");
            const size_t f2_or = f2_body.find("||");
            const size_t f2_user = f2_body.find("ChunkLoopCancelRequested(ctx.cancel)");
            const size_t f2_canceled = f2_body.find("set_status(TranslationStatus::Canceled);");
            TEST_CHECK(f2_latch != std::string::npos && f2_or != std::string::npos &&
                           f2_user != std::string::npos && f2_latch < f2_or && f2_or < f2_user,
                       "261121_0002 F2 pin: the pre-block latch check ORs the user token "
                       "(latch || user-token operand order)");
            TEST_CHECK(f2_canceled != std::string::npos && f2_canceled > f2_user,
                       "261121_0002 F2 pin: the OR'd check converges to Canceled");
            // Mirror (the :919-920 pattern): the pre-block's user-token check
            // must PRECEDE the retry warrant's — the pre-block may not become
            // the latch-only site again.
            const size_t f2_warrant = engine_src.find("EngineHostTransientRetryWarranted(host_err,");
            TEST_CHECK(f2_user != std::string::npos && f2_warrant != std::string::npos &&
                           f2_preblock + f2_user < f2_warrant,
                       "261121_0002 F2 pin: the pre-block user-token check precedes the "
                       "retry-warrant mirror (index order)");
        }

        // e7 (F7, fix round 2 unit B): the local plan computation is gated on
        // an installed observer — the default-ctx callers (back-translate,
        // drag, re-translate, double-Ctrl+C) carry an empty on_chunk whose
        // only consumer is the badge tick, so the split is skipped for them.
        const size_t f7_gate = engine_src.find("const auto plan = ctx.on_chunk");
        TEST_CHECK(f7_gate != std::string::npos &&
                       engine_src.find(
                           "? SplitTextIntoTranslationChunks(ToUtf8(text), kLocalProgressChunkUnits)") !=
                           std::string::npos &&
                       engine_src.find(": std::vector<TranslationChunk>{}") != std::string::npos,
                   "261121_0002 F7 pin: the local plan split is gated on ctx.on_chunk "
                   "(no observer -> empty plan, same predicate as the tick)");
        TEST_CHECK(f7_gate != std::string::npos && plan_tick != std::string::npos &&
                       f7_gate < plan_tick,
                   "261121_0002 F7 pin: the gated plan computation precedes the (0, n) tick");
    }

    // ---- (f) the cloud loops poll the cancel token above the budget --------
    {
        std::string google_src;
        if (!read_repo_src(L"src\\google_translate.cpp", google_src)) {
            std::cout << "[SKIP] src/google_translate.cpp not resolvable; its loop-order pin was skipped."
                      << std::endl;
        } else {
            const size_t cancel_check = google_src.find("ChunkLoopCancelRequested(ctx.cancel)");
            const size_t budget_check =
                google_src.find("ChunkLoopBudgetExhausted(::GetTickCount64() - loop_started_ms)");
            TEST_CHECK(cancel_check != std::string::npos && budget_check != std::string::npos &&
                           cancel_check < budget_check,
                       "261121_0002 pin: the google chunk loop honors a cancel BEFORE its "
                       "budget check");
            TEST_CHECK(google_src.find("if (ShowChunkProgress(chunks.size()) && ctx.on_chunk)") !=
                           std::string::npos,
                       "261121_0002 pin: the google tick is gated by ShowChunkProgress");
        }
        std::string openai_src;
        if (!read_repo_src(L"src\\openai_compatible_client.cpp", openai_src)) {
            std::cout << "[SKIP] src/openai_compatible_client.cpp not resolvable; its loop-order "
                         "pin was skipped." << std::endl;
        } else {
            const size_t cancel_check = openai_src.find("ChunkLoopCancelRequested(ctx.cancel)");
            const size_t budget_check =
                openai_src.find("OpenAiChunkLoopBudgetExhausted(::GetTickCount64() - loop_started_ms)");
            TEST_CHECK(cancel_check != std::string::npos && budget_check != std::string::npos &&
                           cancel_check < budget_check,
                       "261121_0002 pin: the openai chunk loop honors a cancel BEFORE its "
                       "budget check");
            TEST_CHECK(openai_src.find("if (ShowChunkProgress(chunks.size()) && ctx.on_chunk)") !=
                           std::string::npos,
                       "261121_0002 pin: the openai tick is gated by ShowChunkProgress");
        }
    }

    // ---- (g) the badge's local plan-known label ---------------------------
    {
        std::string badge_src;
        if (!read_repo_src(L"src\\ui\\badge.cpp", badge_src)) {
            std::cout << "[SKIP] src/ui/badge.cpp not resolvable; its label-precedence pin was "
                         "skipped." << std::endl;
        } else {
            TEST_CHECK(badge_src.find("void BadgeReplaceAll(") != std::string::npos,
                       "261121_0002 pin: the badge's shared token-replacement helper exists");
            const size_t cancelling_arm = badge_src.find("StringId::BadgeCancelling");
            const size_t chunks_arm = badge_src.find("StringId::BadgeTranslatingChunks");
            const size_t counter_arm = badge_src.find("StringId::BadgeTranslatingProgress");
            TEST_CHECK(cancelling_arm != std::string::npos && chunks_arm != std::string::npos &&
                           counter_arm != std::string::npos && cancelling_arm < chunks_arm &&
                           chunks_arm < counter_arm,
                       "261121_0002 pin: the Translating arm prefers Cancelling > chunk-count > "
                       "{c}/{t} counter");
            TEST_CHECK(badge_src.find("BadgeReplaceAll(statusText, L\"{n}\"") != std::string::npos,
                       "261121_0002 pin: the chunk-count arm fills its {n} token via BadgeReplaceAll");

            // F5 (fix round 2 unit B): the cancelling handler drops a stale
            // "set true" — cross-thread PostMessage order is not FIFO, so a
            // set-true posted before the task-end clear can arrive after it;
            // the true-set is guarded on the Translating status (a straggler
            // can only be stale, since RequestCancelTranslation only fires
            // while a task is in flight) and the set-false clear always
            // applies.
            const size_t f5_case = badge_src.find("case WM_BADGE_SET_CANCELLING:");
            const size_t f5_case_end = badge_src.find("case WM_MOUSELEAVE:", f5_case);
            TEST_CHECK(f5_case != std::string::npos && f5_case_end != std::string::npos &&
                           f5_case_end > f5_case,
                       "261121_0002 F5 pin: the cancelling handler body is locatable and bounded");
            if (f5_case != std::string::npos && f5_case_end != std::string::npos &&
                f5_case_end > f5_case) {
                const std::string f5_body = badge_src.substr(f5_case, f5_case_end - f5_case);
                const size_t f5_guard = f5_body.find(
                    "if (wParam == 0 || self->status_ == BadgeStatus::Translating)");
                const size_t f5_set = f5_body.find("self->cancelling_ = (wParam != 0);");
                TEST_CHECK(f5_guard != std::string::npos && f5_set != std::string::npos &&
                               f5_guard < f5_set,
                           "261121_0002 F5 pin: the cancelling true-set is guarded on the "
                           "Translating status (a stale straggler is dropped)");
            }

            // R3-4 (261121_0002 fix round 3): the same-thread FAST PATH of
            // SetCancellingThreadSafe must carry the SAME guard. Its sole
            // production caller (RequestCancelTranslation, GUI thread) always
            // takes that path, so an unguarded set-true there would keep the
            // F5 straggler leak reachable with the handler guard intact.
            const size_t r34_fn = badge_src.find(
                "void FloatingBadge::SetCancellingThreadSafe(bool cancelling)");
            const size_t r34_fn_end = badge_src.find("void FloatingBadge::SetLanguages(",
                                                     r34_fn);
            TEST_CHECK(r34_fn != std::string::npos && r34_fn_end != std::string::npos &&
                           r34_fn_end > r34_fn,
                       "261121_0002 R3-4 pin: the SetCancellingThreadSafe body is locatable "
                       "and bounded");
            if (r34_fn != std::string::npos && r34_fn_end != std::string::npos &&
                r34_fn_end > r34_fn) {
                const std::string r34_body = badge_src.substr(r34_fn, r34_fn_end - r34_fn);
                const size_t r34_guard = r34_body.find(
                    "if (!cancelling || status_ == BadgeStatus::Translating)");
                const size_t r34_set = r34_body.find("cancelling_ = cancelling;");
                TEST_CHECK(r34_guard != std::string::npos && r34_set != std::string::npos &&
                               r34_guard < r34_set,
                           "261121_0002 R3-4 pin: the same-thread fast path guards its "
                           "set-true on the Translating status (the clear always applies)");
            }
        }
    }

    // ---- (h) the badge label tokens resolve in EVERY selector locale -------
    // The badge fills {c}/{t} (cloud per-chunk counter) and {n} (local
    // plan-known count) by literal token replacement, so a locale row that
    // drops a token would render the raw brace text on the pill. Iterating the
    // selector (the authenticity gate's authored set) instead of a literal
    // count keeps this true through the UI-language phases.
    {
        const auto& locales = GetSupportedUiLocales();
        int token_rows_ok = 0;
        for (const UiLocaleEntry& entry : locales) {
            I18n::SetLocale(entry.locale);
            const std::wstring progress = I18n::Get(StringId::BadgeTranslatingProgress);
            const std::wstring chunks = I18n::Get(StringId::BadgeTranslatingChunks);
            const bool row_ok =
                progress.find(L"{c}") != std::wstring::npos &&
                progress.find(L"{t}") != std::wstring::npos &&
                chunks.find(L"{n}") != std::wstring::npos &&
                !I18n::Get(StringId::BadgeCancelling).empty() &&
                !I18n::Get(StringId::TooltipTranslationCancelled).empty() &&
                !I18n::Get(StringId::TooltipCancelHint).empty();
            if (row_ok) ++token_rows_ok;
        }
        I18n::SetLocale(UiLocale::English);
        TEST_CHECK(!locales.empty() && token_rows_ok == static_cast<int>(locales.size()),
                   "261121_0002: every selector locale carries the {c}/{t}/{n} badge tokens and "
                   "the cancelling / cancelled / hint notices");
    }

    // ---- (i) F2 (fix round 2 unit B): the pre-block honors the user token --
    // Manager-level, per the verifier's sketch (security-reviewer-report-
    // verify.md §5): strict local pin + host-not-servable (engine_host
    // enabled=false), the REQ-R16 latch untouched (cancel_requested_ ==
    // false), a pre-set ctx.cancel flag -> the result MUST classify as
    // Canceled (no tone, no modal, the Enter held downstream) — never
    // LocalModelMissing. The regression pair (flag unset) must keep the
    // REQ-029-B masquerade guard: LocalModelMissing, no cloud. The pre-block
    // returns BEFORE the 040 routing log and every cloud seam, so this check
    // touches no network, no registry, no window — mirroring how the m6 T5
    // suite constructs this exact host-not-servable shape.
    {
        emebalachat::TranslationManager f2_strict(emebalachat::EngineType::LocalLlama, "");
        emebalachat::EngineHostConfig f2_off;
        f2_off.enabled = false;  // HostCouldServeFor == false regardless of the binary
        f2_strict.SetEngineHostConfig(f2_off);

        std::atomic<bool> f2_user_cancel{true};
        emebalachat::TranslationTaskContext f2_ctx;
        f2_ctx.cancel = &f2_user_cancel;
        emebalachat::TranslationStatus f2_st = emebalachat::TranslationStatus::Ok;
        const std::wstring f2_res = f2_strict.Translate(L"hello", "KO", "EN", &f2_st, f2_ctx);
        TEST_CHECK(f2_res.empty() && f2_st == emebalachat::TranslationStatus::Canceled,
                   "261121_0002 F2: a pre-set user cancel classifies the host-absent local pin "
                   "as Canceled (never LocalModelMissing)");

        // Regression pair: NO user cancel -> the REQ-029-B guard must still
        // pre-block to LocalModelMissing (a revert that drops the OR, or a
        // botched fix that cancels unconditionally, trips exactly one of the
        // two checks).
        emebalachat::TranslationTaskContext f2_ctx_quiet;
        f2_st = emebalachat::TranslationStatus::Ok;
        const std::wstring f2_res2 = f2_strict.Translate(L"hello", "KO", "EN", &f2_st, f2_ctx_quiet);
        TEST_CHECK(f2_res2.empty() && f2_st == emebalachat::TranslationStatus::LocalModelMissing,
                   "261121_0002 F2: without a user cancel the pre-block keeps LocalModelMissing "
                   "(REQ-029-B survives for non-cancelled requests)");
    }

    // ---- (j) 261122_0001 A4 (F9): the {c}/{t} token FILL itself -----------
    // Fix-list round 1 deferred this as "needs UI instantiation". That reason
    // was wrong: Render() only COMPOSES the label; the replacement is a pure
    // string transform (now exported through badge.hpp, body unchanged), so it
    // is pinned here with no Direct2D target and no window. Before this block
    // a {c}/{t} swap was uncaught: (g) pinned arm precedence, (h) pinned the
    // per-locale token PRESENCE, nothing pinned the substitution.
    {
        // Distinct values (3 vs 12) so a swap changes the bytes.
        const std::wstring a4_forward = emebalachat::BadgeFillProgressTokens(L"{c}/{t}", 3, 12);
        TEST_CHECK(a4_forward == L"3/12",
                   "261122_0001 A4 (F9): {c}/{t} fills index-then-total (3/12, never 12/3)");
        // Position independence: the tokens are matched by NAME, not by order,
        // so a reversed template still maps {t}->total and {c}->index. This
        // fails if the implementation swaps its two replacement calls.
        TEST_CHECK(emebalachat::BadgeFillProgressTokens(L"{t}/{c}", 3, 12) == L"12/3",
                   "261122_0001 A4 (F9): a reversed template still maps {t}->total, {c}->index");
        // Replace-all: every occurrence is filled, not just the first.
        TEST_CHECK(emebalachat::BadgeFillProgressTokens(L"{c} of {t} ({c})", 2, 5) ==
                       L"2 of 5 (2)",
                   "261122_0001 A4 (F9): every {c}/{t} occurrence is replaced");
        // No token -> byte-identical passthrough (the pre-feature label path).
        TEST_CHECK(emebalachat::BadgeFillProgressTokens(L"Translating...", 3, 12) ==
                       L"Translating...",
                   "261122_0001 A4 (F9): a token-free label is returned unchanged");
        // The PRODUCTION template (StringId::BadgeTranslatingProgress) filled at
        // runtime: no raw braces survive, both numbers are present, and the
        // swapped fill differs (the swap a real regression would produce).
        const std::wstring a4_tpl = I18n::Get(StringId::BadgeTranslatingProgress);
        const std::wstring a4_filled = emebalachat::BadgeFillProgressTokens(a4_tpl, 3, 12);
        const std::wstring a4_rev = emebalachat::BadgeFillProgressTokens(a4_tpl, 12, 3);
        TEST_CHECK(a4_tpl.find(L"{c}/{t}") != std::wstring::npos &&
                       a4_filled.find(L"{") == std::wstring::npos &&
                       a4_filled.find(L"3") != std::wstring::npos &&
                       a4_filled.find(L"12") != std::wstring::npos &&
                       a4_filled != a4_rev,
                   "261122_0001 A4 (F9): the English progress template renders 3/12 with no "
                   "raw braces left");

        // Every selector locale, same transform (a locale row that reorders the
        // tokens must still fill both).
        const auto& a4_locales = GetSupportedUiLocales();
        int a4_locale_rows_ok = 0;
        for (const UiLocaleEntry& entry : a4_locales) {
            I18n::SetLocale(entry.locale);
            const std::wstring tpl = I18n::Get(StringId::BadgeTranslatingProgress);
            const std::wstring filled = emebalachat::BadgeFillProgressTokens(tpl, 3, 12);
            const std::wstring rev = emebalachat::BadgeFillProgressTokens(tpl, 12, 3);
            if (filled.find(L"{") == std::wstring::npos &&
                filled.find(L"3") != std::wstring::npos &&
                filled.find(L"12") != std::wstring::npos && filled != rev) {
                ++a4_locale_rows_ok;
            }
        }
        I18n::SetLocale(UiLocale::English);
        TEST_CHECK(!a4_locales.empty() &&
                       a4_locale_rows_ok == static_cast<int>(a4_locales.size()),
                   "261122_0001 A4 (F9): every selector locale fills {c}/{t} without leaving "
                   "brace text (and a swapped fill differs)");

        // The seam is intentional and the CALL SITE passes (index, total) in
        // that order - a swap at the call site would leave the helper test
        // above green, so pin the argument order too.
        std::string a4_hpp_src;
        TEST_CHECK(read_repo_src(L"src\\ui\\badge.hpp", a4_hpp_src) &&
                       a4_hpp_src.find("std::wstring BadgeFillProgressTokens(") !=
                           std::string::npos,
                   "261122_0001 A4 (F9): the fill helper is exported through badge.hpp "
                   "(the seam is deliberate, not a test-only hack)");
        std::string a4_badge_src;
        if (!read_repo_src(L"src\\ui\\badge.cpp", a4_badge_src)) {
            std::cout << "[SKIP] src/ui/badge.cpp not resolvable; the F9 call-site pin was "
                         "skipped." << std::endl;
        } else {
            const size_t a4_site = a4_badge_src.find("statusText = BadgeFillProgressTokens(");
            TEST_CHECK(a4_site != std::string::npos,
                       "261122_0001 A4 (F9): the Translating arm composes its label through "
                       "BadgeFillProgressTokens");
            if (a4_site != std::string::npos) {
                const size_t a4_site_end = a4_badge_src.find(");", a4_site);
                const std::string a4_args =
                    a4_site_end == std::string::npos
                        ? std::string()
                        : a4_badge_src.substr(a4_site, a4_site_end - a4_site);
                TEST_CHECK(a4_site_end != std::string::npos &&
                               a4_args.find("progress_index, progress_total") !=
                                   std::string::npos &&
                               a4_args.find("progress_total, progress_index") ==
                                   std::string::npos,
                           "261122_0001 A4 (F9): the call site passes (index, total) in that "
                           "order (a swapped call would go uncaught by the helper pin)");
            }
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261121_0002 multi-chunk progress + cancel (worker) passed." << std::endl;
    } else {
        std::cout << "[FAIL] 261121_0002 multi-chunk progress + cancel (worker): "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
