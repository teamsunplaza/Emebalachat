// ---------------------------------------------------------------------------
// P2-F (261122_0002 v0.10.10 phased fixes, phase 2 wave B): the main.cpp
// reading-path unification unit. The GUI behaviors themselves (Esc/badge
// cancel routing, the four ctx wirings, the editor hand-off) live in
// wWinMain lambdas and are NOT unit-testable here - they are pinned
// structurally over src/main.cpp, the v107_drag_tooltip_tests.inc /
// p1_u5_tests.inc idiom. This suite covers:
//
//   (1) MIRROR PREDICATES - the pure decision rules main.cpp's anonymous
//       namespace introduced (ReadingJobBegin clear-then-publish, the
//       any-running fold, CancelAllReadingJobs, the preview-lease CAS pair),
//       re-derived here and exercised over a truth table. main.cpp is
//       deliberately NOT linked into run_tests.exe, so the mirrors pin the
//       protocol; the real emebalachat::ShowChunkProgress (leaf header, links
//       clean) gates the preview TryBegin predicate.
//   (2) STRUCTURAL PINS over src/main.cpp: the registry definition, the
//       ChunkPreviewLease class, the Esc unified-cancel arm (above the
//       dismiss chain, below the picker deferral), the badge unified-cancel
//       arm, the per-path TranslationTaskContext wiring of all four reading
//       lambdas (cancel token / out_error_detail carrier / lease-guarded
//       on_chunk_text), the removed racy LastChatErrorDetail read, the
//       SetOnTypingError transient-notice registration (before the P5-F1
//       last-callback anchor), the four translation_history::Push sites, the
//       tooltip->editor hand-off (SetEditRequestCallback /
//       SetOpenInWindowCallback / empty-history guard / on_done plumbing),
//       the L4-C selection-replace hotkey registration, and the RISK-04
//       supersede-cancel producers.
// Zero windows, zero network: ResolveRepoFile source reads only.
// ---------------------------------------------------------------------------

namespace {

// Re-derived mirror of main.cpp's per-job slot pair.
struct P2FMirrorSlot {
    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};
};

// Mirror of ReadingJobBegin: clear the stale token BEFORE publishing running.
void P2FMirrorJobBegin(P2FMirrorSlot& s) {
    s.cancel.store(false, std::memory_order_release);
    s.running.store(true, std::memory_order_release);
}

// Mirror of AnyReadingJobRunning (the four-slot fold).
bool P2FMirrorAnyRunning(const P2FMirrorSlot* slots, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (slots[i].running.load(std::memory_order_acquire)) return true;
    }
    return false;
}

// Mirror of CancelAllReadingJobs (latch every token; idempotent).
void P2FMirrorCancelAll(P2FMirrorSlot* slots, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        slots[i].cancel.store(true, std::memory_order_release);
    }
}

// Mirrors of the PreviewLeaseAcquire/PreviewLeaseRelease CAS pair.
unsigned long long P2FMirrorLeaseAcquire(std::atomic<unsigned long long>& lease,
                                         unsigned long long id) {
    unsigned long long expected = 0;
    return lease.compare_exchange_strong(expected, id, std::memory_order_acq_rel,
                                         std::memory_order_acquire)
               ? id
               : 0;
}
bool P2FMirrorLeaseRelease(std::atomic<unsigned long long>& lease, unsigned long long id) {
    unsigned long long expected = id;
    return lease.compare_exchange_strong(expected, 0, std::memory_order_acq_rel,
                                         std::memory_order_acquire);
}

void P2FReadSrc(const wchar_t* rel, std::string& out) {
    out.clear();
    const std::filesystem::path p = ResolveRepoFile(rel);
    if (p.empty()) return;
    std::ifstream in(p, std::ios::binary);
    if (!in) return;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::size_t P2FCountOf(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t pos = hay.find(needle); pos != std::string::npos;
         pos = hay.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

// Bounded [from, to) slice of a source (empty when either bound is missing).
std::string P2FRegion(const std::string& src, const std::string& from, const std::string& to) {
    const std::size_t a = src.find(from);
    if (a == std::string::npos) return std::string();
    const std::size_t b = src.find(to, a + from.size());
    if (b == std::string::npos) return std::string();
    return src.substr(a, b - a);
}

} // namespace

void TestP2FRegistryMirrorPredicates() {
    std::cout << "[RUN] Testing V10F P2-F registry/lease mirror predicates..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- the begin protocol: clear-cancel-then-publish-running -------------
    P2FMirrorSlot s;
    TEST_CHECK(!s.running.load() && !s.cancel.load(),
               "P2-F (1): a fresh slot is idle with an unset token");
    s.cancel.store(true, std::memory_order_release); // stale latch from a previous run
    P2FMirrorJobBegin(s);
    TEST_CHECK(s.running.load(std::memory_order_acquire) &&
                   !s.cancel.load(std::memory_order_acquire),
               "P2-F (1): begin publishes running AND clears the stale token");
    s.cancel.store(true, std::memory_order_release); // user cancel mid-job
    P2FMirrorSlot s2;
    P2FMirrorJobBegin(s2);
    TEST_CHECK(s.cancel.load(std::memory_order_acquire) &&
                   !s2.cancel.load(std::memory_order_acquire),
               "P2-F (1): a cancel latched mid-job survives until the NEXT begin clears it");
    s.running.store(false, std::memory_order_release);
    TEST_CHECK(!s.running.load(std::memory_order_acquire),
               "P2-F (1): the scope-exit store clears running (job finished)");

    // ---- the any-running fold + cancel-all over the real four-slot shape ---
    P2FMirrorSlot slots[4] = {};
    TEST_CHECK(!P2FMirrorAnyRunning(slots, 4),
               "P2-F (2): all-idle registry reports nothing running");
    for (std::size_t i = 0; i < 4; ++i) {
        P2FMirrorJobBegin(slots[i]);
        TEST_CHECK(P2FMirrorAnyRunning(slots, 4),
                   "P2-F (2): slot running makes the whole registry active");
        P2FMirrorCancelAll(slots, 4);
        for (std::size_t j = 0; j < 4; ++j) {
            TEST_CHECK(slots[j].cancel.load(std::memory_order_acquire),
                       "P2-F (2): cancel-all latches EVERY job token");
        }
        P2FMirrorCancelAll(slots, 4); // idempotent: a second latch is harmless
        TEST_CHECK(P2FMirrorAnyRunning(slots, 4),
                   "P2-F (2): re-latching does not disturb the running flags");
        slots[i].running.store(false, std::memory_order_release);
        P2FMirrorJobBegin(slots[i]); // next job start clears its own stale token only
        TEST_CHECK(!slots[i].cancel.load(std::memory_order_acquire),
                   "P2-F (2): the next begin clears the stale latch (never cancels a new job)");
        slots[i].running.store(false, std::memory_order_release);
    }

    // ---- the preview lease: single writer, wrong-id release is a no-op -----
    std::atomic<unsigned long long> lease{0};
    const unsigned long long id1 = P2FMirrorLeaseAcquire(lease, 1);
    TEST_CHECK(id1 == 1 && lease.load() == 1,
               "P2-F (3): the free lease grants the first acquirer");
    TEST_CHECK(P2FMirrorLeaseAcquire(lease, 2) == 0 && lease.load() == 1,
               "P2-F (3): a held lease rejects the second acquirer (single writer)");
    TEST_CHECK(!P2FMirrorLeaseRelease(lease, 2) && lease.load() == 1,
               "P2-F (3): releasing with a wrong id is rejected and changes nothing");
    TEST_CHECK(P2FMirrorLeaseRelease(lease, 1) && lease.load() == 0,
               "P2-F (3): the holder releases and the lease returns to the free state");
    const unsigned long long id2 = P2FMirrorLeaseAcquire(lease, 3);
    TEST_CHECK(id2 == 3, "P2-F (3): the freed lease grants a fresh id");
    P2FMirrorLeaseRelease(lease, 3);
    TEST_CHECK(!P2FMirrorLeaseRelease(lease, 3) && lease.load() == 0,
               "P2-F (3): a stale second release after the free state is a no-op");

    // ---- the real gating predicate (leaf header, links into this runner) ---
    TEST_CHECK(!emebalachat::ShowChunkProgress(0) && !emebalachat::ShowChunkProgress(1),
               "P2-F (4): single-chunk runs never begin a preview (pre-V10F behavior)");
    TEST_CHECK(emebalachat::ShowChunkProgress(2),
               "P2-F (4): a genuine multi-chunk plan is the preview signal");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] V10F P2-F registry/lease mirror predicates passed." << std::endl;
    } else {
        std::cout << "[FAIL] V10F P2-F registry/lease mirror predicates: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestP2FMainStructurePins() {
    std::cout << "[RUN] Testing V10F P2-F main.cpp structural pins..." << std::endl;
    const int failures_before = g_failed_count;

    std::string main_src;
    P2FReadSrc(L"src\\main.cpp", main_src);
    TEST_CHECK(!main_src.empty(), "P2-F (5): src/main.cpp readable for the structural pins");
    if (main_src.empty()) {
        std::cout << "[FAIL] V10F P2-F main.cpp structural pins: source unreadable." << std::endl;
        return;
    }

    // ---- the registry definition + the pure protocol order -----------------
    TEST_CHECK(main_src.find("struct ReadingJobRegistry") != std::string::npos,
               "P2-F (5): the registry type is defined in main.cpp");
    {
        // Bound at the line-start close: "{false};" on the first field would
        // otherwise match a naive "};" needle.
        const std::string reg_body = P2FRegion(main_src, "struct ReadingJobRegistry", "\n};");
        TEST_CHECK(!reg_body.empty(), "P2-F (5): the registry body is locatable");
        TEST_CHECK(reg_body.find("drag_running") != std::string::npos &&
                       reg_body.find("retranslate_running") != std::string::npos &&
                       reg_body.find("back_running") != std::string::npos &&
                       reg_body.find("dbl_running") != std::string::npos,
                   "P2-F (5): all four job-type slot pairs exist");
        TEST_CHECK(reg_body.find("preview_lease") != std::string::npos,
                   "P2-F (5): the single-writer preview lease lives in the registry");
    }
    {
        // ReadingJobBegin MUST clear the token BEFORE publishing running.
        const std::string begin_body = P2FRegion(main_src, "inline void ReadingJobBegin", "}");
        const std::size_t clear_pos = begin_body.find("cancel.store(false");
        const std::size_t publish_pos = begin_body.find("running.store(true");
        TEST_CHECK(clear_pos != std::string::npos && publish_pos != std::string::npos &&
                       clear_pos < publish_pos,
                   "P2-F (5): ReadingJobBegin clears the stale token before publishing running");
        const std::string cancel_all = P2FRegion(main_src, "inline void CancelAllReadingJobs", "}");
        TEST_CHECK(P2FCountOf(cancel_all, ".store(true") == 4,
                   "P2-F (5): CancelAllReadingJobs latches exactly the four reading tokens");
    }
    {
        // The lease class: RAII retire backstop + the progress gate + the
        // three tooltip entry points + the CAS primitives.
        const std::string lease_body = P2FRegion(main_src, "class ChunkPreviewLease", "};");
        TEST_CHECK(!lease_body.empty(), "P2-F (5): the ChunkPreviewLease class is locatable");
        TEST_CHECK(lease_body.find("~ChunkPreviewLease() { End(false); }") != std::string::npos,
                   "P2-F (5): the lease dtor retires a still-running preview (cancel backstop)");
        TEST_CHECK(lease_body.find("ShowChunkProgress") != std::string::npos,
                   "P2-F (5): TryBegin gates on ShowChunkProgress (multi-chunk only)");
        TEST_CHECK(lease_body.find("BeginChunkPreview") != std::string::npos &&
                       lease_body.find("AppendChunkPreviewText") != std::string::npos &&
                       lease_body.find("EndChunkPreview") != std::string::npos,
                   "P2-F (5): the lease drives the P2-C preview contract");
        TEST_CHECK(lease_body.find("PreviewLeaseAcquire") != std::string::npos &&
                       lease_body.find("PreviewLeaseRelease") != std::string::npos,
                   "P2-F (5): the lease serializes through the CAS primitives");
        TEST_CHECK(P2FCountOf(main_src, "ChunkPreviewLease ") >= 4,
                   "P2-F (5): all four reading paths construct a preview lease");
    }

    // ---- Task 1: the Esc unified-cancel arm --------------------------------
    {
        const std::string esc_body = P2FRegion(main_src, "hook.SetEscCallback",
                                               "// Tooltip language switcher");
        TEST_CHECK(!esc_body.empty(), "P2-F (6): the Esc callback body is locatable");
        const std::size_t picker_pos = esc_body.find("esc deferred: language picker owns the key");
        const std::size_t unified_pos = esc_body.find("AnyReadingJobRunning(reading_jobs)");
        const std::size_t dismiss_pos = esc_body.find("if (tooltip.IsVisible())");
        TEST_CHECK(picker_pos != std::string::npos && unified_pos != std::string::npos &&
                       dismiss_pos != std::string::npos &&
                       picker_pos < unified_pos && unified_pos < dismiss_pos,
                   "P2-F (6) Task 1: picker deferral -> unified cancel -> dismiss chain order");
        TEST_CHECK(esc_body.find("worker.RequestCancelTranslation()") != std::string::npos &&
                       esc_body.find("CancelAllReadingJobs(reading_jobs)") != std::string::npos,
                   "P2-F (6) Task 1: the unified arm cancels the pipeline AND every reading job");
        TEST_CHECK(esc_body.find("esc unified cancel: pipeline=%d") != std::string::npos,
                   "P2-F (6) Task 1: the consume is evidenced (shape-only diag)");
    }

    // ---- Task 2: the badge unified-cancel arm ------------------------------
    {
        const std::string badge_body = P2FRegion(main_src, "badge.SetClickCallback",
                                                 "badge.SetDoubleClickCallback");
        TEST_CHECK(!badge_body.empty(), "P2-F (7): the badge click body is locatable");
        TEST_CHECK(badge_body.find("worker.IsBusy() || emebalachat::AnyReadingJobRunning(reading_jobs)") !=
                       std::string::npos,
                   "P2-F (7) Task 2: the click predicates on the unified registry");
        TEST_CHECK(badge_body.find("CancelAllReadingJobs(reading_jobs)") != std::string::npos &&
                       badge_body.find("hook.ToggleActive()") != std::string::npos,
                   "P2-F (7) Task 2: busy -> cancel-all, idle -> legacy toggle");
        TEST_CHECK(main_src.find("worker.IsBusy() ? worker.RequestCancelTranslation() : hook.ToggleActive()") ==
                       std::string::npos,
                   "P2-F (7) Task 2 zero-loss: the old pipeline-only ternary is fully replaced");
    }

    // ---- Task 3: the four reading paths wire the task context --------------
    {
        const std::string drag_body = P2FRegion(main_src, "auto run_drag_translate",
                                                "SingleSlotWorker<DragTranslateJob>");
        TEST_CHECK(!drag_body.empty(), "P2-F (8): the drag lambda is locatable");
        TEST_CHECK(drag_body.find("&reading_jobs.drag_cancel") != std::string::npos &&
                       drag_body.find("drag_ctx.out_error_detail") != std::string::npos &&
                       drag_body.find("drag_ctx.on_chunk_text") != std::string::npos &&
                       drag_body.find("ReadingJobScope drag_scope") != std::string::npos,
                   "P2-F (8) Task 3: the drag path wires cancel/detail/preview + a scope guard");
        TEST_CHECK(drag_body.find("drag_translate_canceled") != std::string::npos,
                   "P2-F (8): the drag cancel arm stays silent (no failure notice)");
        const std::size_t end_false = drag_body.find("drag_preview.End(false)");
        const std::size_t modal = drag_body.find("RequestEngineUnavailableModal");
        // The receiver-qualified needle: a failure-branch COMMENT mentions
        // the old ShowTranslationThreadSafe fall-through, the call does not
        // repeat it.
        const std::size_t end_true = drag_body.find("drag_preview.End(true)");
        const std::size_t show = drag_body.find("tooltip.ShowTranslationThreadSafe(");
        TEST_CHECK(end_false != std::string::npos && modal != std::string::npos &&
                       end_false < modal,
                   "P2-F (8): the drag failure path retires the preview BEFORE the modal/notice");
        TEST_CHECK(end_true != std::string::npos && show != std::string::npos && end_true < show,
                   "P2-F (8): the drag success path freezes the preview BEFORE the final card");
    }
    {
        const std::string rt_body = P2FRegion(main_src, "auto run_retranslate",
                                              "SingleSlotWorker<RetranslateJob> retranslate_worker");
        TEST_CHECK(!rt_body.empty(), "P2-F (8): the retranslate lambda is locatable");
        TEST_CHECK(rt_body.find("&reading_jobs.retranslate_cancel") != std::string::npos &&
                       rt_body.find("rt_ctx.out_error_detail") != std::string::npos &&
                       rt_body.find("rt_ctx.on_chunk_text") != std::string::npos &&
                       rt_body.find("ReadingJobScope rt_scope") != std::string::npos,
                   "P2-F (8) Task 3: the retranslate path wires cancel/detail/preview + a scope guard");
        const std::size_t end_false = rt_body.find("rt_preview.End(false)");
        const std::size_t modal = rt_body.find("RequestEngineUnavailableModal");
        const std::size_t end_true = rt_body.find("rt_preview.End(true)");
        const std::size_t done = rt_body.find("job.on_done(translated)");
        TEST_CHECK(end_false != std::string::npos && modal != std::string::npos && end_false < modal,
                   "P2-F (8): the retranslate modal path retires the preview first");
        TEST_CHECK(end_true != std::string::npos && done != std::string::npos && end_true < done,
                   "P2-F (8): the editor result freezes the preview before on_done fires");
    }
    {
        const std::string bt_body = P2FRegion(main_src, "auto run_back_translate",
                                              "SingleSlotWorker<BackTranslateJob>");
        TEST_CHECK(!bt_body.empty(), "P2-F (8): the back-translate lambda is locatable");
        TEST_CHECK(bt_body.find("&reading_jobs.back_cancel") != std::string::npos &&
                       bt_body.find("bt_ctx.out_error_detail") != std::string::npos &&
                       bt_body.find("bt_ctx.on_chunk_text") != std::string::npos &&
                       bt_body.find("ReadingJobScope back_scope") != std::string::npos,
                   "P2-F (8) Task 3: the back-translate path wires cancel/detail/preview + a scope guard");
        const std::size_t end_true = bt_body.find("bt_preview.End(true)");
        const std::size_t show = bt_body.find("ShowBackTranslationThreadSafe");
        TEST_CHECK(end_true != std::string::npos && show != std::string::npos && end_true < show,
                   "P2-F (8): the back-translate success freezes the preview before the card");
    }
    {
        const std::string dbl_body = P2FRegion(main_src, "hook.SetDoubleCtrlCCallback",
                                               "hook.SetSelectionReplaceCallback");
        TEST_CHECK(!dbl_body.empty(), "P2-F (8): the double-Ctrl+C body is locatable");
        TEST_CHECK(dbl_body.find("&reading_jobs.dbl_cancel") != std::string::npos &&
                       dbl_body.find("dbl_ctx.out_error_detail") != std::string::npos &&
                       dbl_body.find("dbl_ctx.on_chunk_text") != std::string::npos &&
                       dbl_body.find("ReadingJobScope dbl_scope") != std::string::npos,
                   "P2-F (8) Task 3: the double-Ctrl+C path wires cancel/detail/preview + a scope guard");
        TEST_CHECK(dbl_body.find("dbl_ctrl_c_canceled") != std::string::npos,
                   "P2-F (8): the double-Ctrl+C cancel arm stays silent");
        const std::size_t end_false = dbl_body.find("dbl_preview.End(false)");
        const std::size_t modal = dbl_body.find("RequestEngineUnavailableModal");
        const std::size_t end_true = dbl_body.find("dbl_preview.End(true)");
        const std::size_t show = dbl_body.find("tooltip.ShowTranslationThreadSafe(");
        TEST_CHECK(end_false != std::string::npos && modal != std::string::npos && end_false < modal,
                   "P2-F (8): the double-Ctrl+C failure path retires the preview first");
        TEST_CHECK(end_true != std::string::npos && show != std::string::npos && end_true < show,
                   "P2-F (8): the double-Ctrl+C success freezes the preview before the card");
    }
    TEST_CHECK(P2FCountOf(main_src, "LastChatErrorDetail(") == 0,
               "P2-F (8) Task 3: the racy process-global detail read is gone from main.cpp");

    // ---- Task 4: the typing-error transient notice --------------------------
    {
        const std::size_t typing_pos = main_src.find("worker.SetOnTypingError(");
        const std::size_t engine_unavail_pos = main_src.find("worker.SetEngineUnavailableCallback");
        TEST_CHECK(typing_pos != std::string::npos && engine_unavail_pos != std::string::npos &&
                       typing_pos < engine_unavail_pos,
                   "P2-F (9) Task 4: the typing-error seam registers BEFORE the P5-F1 last-callback anchor");
        const std::string typing_body = P2FRegion(main_src, "worker.SetOnTypingError(", "});");
        TEST_CHECK(typing_body.find("ShowMessageThreadSafe") != std::string::npos &&
                       typing_body.find("TooltipTranslateFailed") != std::string::npos,
                   "P2-F (9) Task 4: the detail marshals through the transient autohide notice");
    }

    // ---- Task 5: the translation-history pushes ----------------------------
    TEST_CHECK(P2FCountOf(main_src, "translation_history::Push") == 5,
               "P2-F (10) Task 5: exactly five Push sites (drag, dbl, retranslate x2, back)");
    {
        const std::string drag_body = P2FRegion(main_src, "auto run_drag_translate",
                                                "SingleSlotWorker<DragTranslateJob>");
        const std::string rt_body = P2FRegion(main_src, "auto run_retranslate",
                                              "SingleSlotWorker<RetranslateJob> retranslate_worker");
        const std::string bt_body = P2FRegion(main_src, "auto run_back_translate",
                                              "SingleSlotWorker<BackTranslateJob>");
        const std::string dbl_body = P2FRegion(main_src, "hook.SetDoubleCtrlCCallback",
                                               "hook.SetSelectionReplaceCallback");
        TEST_CHECK(drag_body.find("Push({ selected, translated") != std::string::npos,
                   "P2-F (10): the drag success records {src, forward, back empty}");
        TEST_CHECK(dbl_body.find("Push({ copied, translated") != std::string::npos,
                   "P2-F (10): the double-Ctrl+C success records its entry");
        TEST_CHECK(P2FCountOf(rt_body, "Push({ job.src, translated") == 2,
                   "P2-F (10): BOTH retranslate success arms record (ordinary + editor)");
        TEST_CHECK(bt_body.find("Push({ job.original_text, job.forward_text, back_text") !=
                       std::string::npos,
                   "P2-F (10): the back-translate success pushes the FULL triple as a fresh entry");
    }

    // ---- Task 6: the tooltip -> editor hand-off -----------------------------
    TEST_CHECK(main_src.find("tooltip.SetEditRequestCallback(") != std::string::npos &&
                   main_src.find("tooltip.SetOpenInWindowCallback(") != std::string::npos,
               "P2-F (11) Task 6: both tooltip header callbacks are wired");
    TEST_CHECK(main_src.find("translation_editor_window::Open(recent.front(), editor_retranslate_fn)") !=
                   std::string::npos,
               "P2-F (11) Task 6: the editor opens on the newest history entry");
    TEST_CHECK(main_src.find("translation_history::Recent()") != std::string::npos &&
                   main_src.find("translation_editor_open_skipped: history empty") != std::string::npos,
               "P2-F (11) Task 6: an empty history is guarded (logged no-op, no new StringId)");
    TEST_CHECK(main_src.find("std::function<void(const std::wstring&)> on_done;") != std::string::npos,
               "P2-F (11): the retranslate job carries the editor completion");
    TEST_CHECK(main_src.find("job.on_done = std::move(on_done)") != std::string::npos &&
                   P2FCountOf(main_src, "job.on_done") >= 6,
               "P2-F (11): the editor completion is invoked on every terminal arm");
    TEST_CHECK(P2FCountOf(main_src, "editor_retranslate_fn") >= 2,
               "P2-F (11): the editor retranslate seam is defined and handed to Open()");

    // ---- Task 7: the L4-C selection-replace hotkey ---------------------------
    TEST_CHECK(P2FCountOf(main_src, "hook.SetSelectionReplaceCallback(") == 1,
               "P2-F (12) Task 7: exactly one selection-replace registration");
    {
        const std::string l4_body = P2FRegion(main_src, "hook.SetSelectionReplaceCallback",
                                              "// ESC Key Dismissal");
        TEST_CHECK(l4_body.find("worker.PostSelectionReplaceTask(target_hwnd)") != std::string::npos,
                   "P2-F (12) Task 7: the combo posts the selection-replace task");
    }

    // ---- RISK-04: the supersede-cancel producers -----------------------------
    TEST_CHECK(P2FCountOf(main_src, "RISK-04") >= 3,
               "P2-F (13): supersede-cancel is documented at every producer");
    TEST_CHECK(main_src.find("reading_jobs.drag_cancel.store(true") != std::string::npos,
               "P2-F (13): a fresh drag click supersedes an in-flight drag");
    TEST_CHECK(P2FCountOf(main_src, "reading_jobs.retranslate_cancel.store(true") >= 3,
               "P2-F (13): every retranslate producer (2 picks + editor) supersedes");

    // ---- zero-feature-loss sweep ---------------------------------------------
    TEST_CHECK(main_src.find("worker.SetBackTranslateCallback") != std::string::npos &&
                   main_src.find("worker.SetFirstMultiChunkHintCallback") != std::string::npos &&
                   main_src.find("worker.SetEngineUnavailableCallback") != std::string::npos,
               "P2-F (14): every pre-existing worker registration survives");
    TEST_CHECK(main_src.find("esc deferred: language picker owns the key") != std::string::npos,
               "P2-F (14): the FUP-02 picker deferral survives");
    TEST_CHECK(main_src.find("CopyChordWithSettledRetry") != std::string::npos,
               "P2-F (14): the REQ-051 drag capture driver survives");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] V10F P2-F main.cpp structural pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] V10F P2-F main.cpp structural pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// Single registration entry (the run_tests.cpp end-of-file pattern): the
// integrator adds `#include "p2_f_tests.inc"` to the include cluster and
// `TestP2FV10FMainReadingUnification();` inside main().
void TestP2FV10FMainReadingUnification() {
    TestP2FRegistryMirrorPredicates();
    TestP2FMainStructurePins();
}
