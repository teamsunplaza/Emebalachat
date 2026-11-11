// ---------------------------------------------------------------------------
// v0.10.7 (session 261111_0001 U1) - drag-to-translate dead-zone bug family
// (audit IDs D1a / D1b / D4 / D3-feedback / ISSUE-009).
//
// Defects pinned here:
//
//  (a) TooltipWindow::Dismiss() off-thread path left visibility set until the
//      GUI thread drained kDismissMessage (tooltip.cpp), so IsVisible() was
//      stale for 40-100 ms and main.cpp's drag-release gate dropped the first
//      gesture of a fast re-drag. Fix: visibility is cleared before the call
//      returns on ANY thread; the posted message still performs the GUI
//      teardown, exactly once, and (since G-06 below) only for the card
//      generation it carries (the handler is idempotent - a stale/duplicate
//      message is a no-op, and a fresh Show* supersedes a queued dismissal).
//
//  (b) main.cpp SetDragReleaseCallback dropped EVERY drag release while a
//      tooltip card was visible (translation cards never auto-hide), which
//      parked a dead zone over the whole desktop. Fix: visibility is only a
//      local qualifier - a gesture that STARTED inside the card rect (card-
//      internal button double-click / in-card drag; the card is hit-test
//      opaque) is suppressed, anything else dismisses the card and shows the
//      drag icon. NOT unit-testable here: the logic lives in main.cpp lambdas
//      over live hook/UI objects; covered by structural source pins below.
//
//  (c) run_drag_translate fell through to ShowTranslationThreadSafe(..., L"")
//      after the empty-translation failure notice (and after the strict-local
//      failure modal), leaving a permanent empty ghost card (ShowTranslation
//      kills the notice's kTimerMessageAutohide and arms no hide of its own).
//      Fix: every empty-result path returns before ShowTranslationThreadSafe.
//      NOT unit-testable here (main.cpp lambda) - structural pins below.
//
//  (d) The smart-bypass drop (ShouldTranslate false) was a silent return.
//      Fix: a brief user-visible notice (StringId::TooltipNoSelection, the
//      existing "click cannot produce a translation" family - no dedicated
//      StringId exists and src/i18n.* is owned by another unit). Structural
//      pin below.
//
//  (e) DragIconWindow show/hide race: a kShowAtMessage posted before an
//      inline GUI-thread Hide() landed AFTER it and stranded a clickable icon
//      at a dead selection site. Fix: a monotonic generation stamp (WPARAM)
//      plus a single-slot pending request; Hide() bumps the generation and
//      empties the slot, and the handler drops any show whose stamp no longer
//      matches (ShouldApplyShowAt - pinned purely, plus a live race fixture).
//  (f) R1-D (fix-list round 1): the four split atomics behind that seam let a
//      preempted producer publish an OLDER generation last (newest show
//      dropped, stale one rendered), a Hide() invalidation be overwritten (icon
//      resurrected at a dead site), and the x/y coords tear (x2,y1). Fix: ONE
//      DragShowArbiter (drag_icon.hpp) serializes {token, coords, slot} + the
//      Hide invalidation under a single mutex. Pinned purely below with
//      deterministic interleavings, plus a live concurrent stress loop.
//  (g) R2-G-06 (fix-list round 2): an off-thread Dismiss() cleared the atomic
//      visible_ and THEN stored a separate teardown stamp, so a GUI-side Show*
//      landing in that window published a FRESH card that the worker's late
//      stamp then tore down. Fix: TooltipCardVisibility (tooltip.hpp) makes
//      {visible, teardown-owed, card generation} one mutex-guarded transition,
//      and the posted kDismissMessage carries the generation of the card it
//      began on (a newer card makes it stale). Pinned purely below.
//  (h) R2-G-10 (fix-list round 2): the kHideMessage handler re-entered Hide(),
//      which unconditionally invalidated the arbiter, so a BeginShow that
//      landed while the hide was queued lost its pending slot
//      (Hide -> pending=0 -> BeginShow -> pending=T -> drain -> pending=0 ->
//      TryApply(T) false). Fix: Hide() returns the hide's own arbiter token
//      and the handler re-invalidates only while that token is current.
//  (i) R2-G-03 / G-13 (fix-list round 2): the double-Ctrl+C prompt source now
//      also resolves through DetectLanguageForPrompt, and the double-Ctrl+C
//      terminal branches (strict-failure modal, empty result) plus the
//      press-time own-popup gate are pinned structurally.
//
// Wiring: included by tests/run_tests.cpp (with the other .inc files) and
// dispatched as RunV107DragTooltipTests(g_failed_count) from the suite table.
//
// Self-contained: no main(), and no include beyond what tests/run_tests.cpp
// already provides (it includes ../src/ui/tooltip.hpp, ../src/ui/drag_icon.hpp,
// <thread>, <fstream>, <string> and the TEST_CHECK harness).
// ---------------------------------------------------------------------------

namespace {

// Repo-relative source reader (same 3-prefix idiom as the ws3 suite; CRLF is
// normalized to LF so needles never depend on the checkout's autocrlf).
std::string V107DtReadSrc(const char* rel) {
    std::string out;
    const char* prefixes[] = {"", "../", "../../"};
    for (const char* pre : prefixes) {
        std::ifstream in((std::string(pre) + rel).c_str(), std::ios::binary);
        if (in) {
            out.assign((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
            break;
        }
    }
    std::string norm;
    norm.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\r' && i + 1 < out.size() && out[i + 1] == '\n') continue;
        norm.push_back(out[i]);
    }
    return norm;
}

// Function body = [marker, first column-0 '}' after it). Same helper shape the
// ws3/REQ-052 suites use for body-scoped pins.
std::string V107DtFuncBody(const std::string& src, const char* marker) {
    const size_t i = src.find(marker);
    if (i == std::string::npos) return std::string();
    const size_t j = src.find("\n}", i);
    return src.substr(i, (j == std::string::npos ? src.size() : j) - i);
}

} // namespace

int RunV107DragTooltipTests(int& failures) {
    std::cout << "[RUN] Testing v0.10.7 drag/tooltip dead-zone fixes (261111_0001 U1)..."
              << std::endl;
    const int failures_before = g_failed_count;

    using TT = emebalachat::TooltipWindow;
    using DI = emebalachat::DragIconWindow;
    const HINSTANCE hInst = ::GetModuleHandleW(nullptr);

    // =====================================================================
    // 1) (a) live Dismiss() semantics: visible_ clears on the CALLING thread,
    //    the GUI teardown runs later, exactly once, from the posted message.
    // =====================================================================
    {
        TT tooltip;
        if (!tooltip.Create(hInst)) {
            std::cout << "[SKIP] V107-DT: TooltipWindow could not be created." << std::endl;
        } else {
            tooltip.ShowMessage(120, 120, L"v107", L"card A", TT::kGenNone);
            TEST_CHECK(tooltip.IsVisible(), "V107-DT (a): fixture card A is visible");
            TEST_CHECK(tooltip.IsMessageMode(), "V107-DT (a): fixture card A is a message card");

            // Off-thread Dismiss() (the hook-thread call shape).
            {
                std::thread producer([&tooltip]() { tooltip.Dismiss(); });
                producer.join();
            }
            TEST_CHECK(!tooltip.IsVisible(),
                       "V107-DT (a): off-thread Dismiss() clears visible_ BEFORE returning "
                       "(no stale IsVisible() gate)");
            // The GUI teardown is still owed at this point: the posted
            // kDismissMessage has not been pumped yet, so the model state
            // (message mode) is untouched - teardown is deferred, not lost.
            TEST_CHECK(tooltip.IsMessageMode(),
                       "V107-DT (a): GUI teardown is deferred to the posted message");
            PumpThreadMessagesOnce();
            TEST_CHECK(!tooltip.IsVisible(),
                       "V107-DT (a): the marshaled teardown keeps the card hidden");
            TEST_CHECK(!tooltip.IsMessageMode(),
                       "V107-DT (a): the marshaled teardown reset the message-mode state");

            // A duplicate kDismissMessage when nothing is pending/visible is a
            // pure no-op (idempotent handler, no double teardown, no crash).
            ::PostMessageW(tooltip.GetHwnd(), TT::kDismissMessage, 0, 0);
            ::PostMessageW(tooltip.GetHwnd(), TT::kDismissMessage, 0, 0);
            PumpThreadMessagesOnce();
            TEST_CHECK(!tooltip.IsVisible(),
                       "V107-DT (a): duplicate kDismissMessage with nothing pending is a no-op");

            // A fresh show SUPERSEDES a dismissal still queued behind it.
            tooltip.ShowMessage(130, 130, L"v107", L"card B", TT::kGenNone);
            {
                std::thread producer([&tooltip]() { tooltip.Dismiss(); });
                producer.join();
            }
            TEST_CHECK(!tooltip.IsVisible(),
                       "V107-DT (a): second off-thread Dismiss() again clears visible_ at once");
            tooltip.ShowMessage(140, 140, L"fresh", L"card C", TT::kGenNone);
            TEST_CHECK(tooltip.IsVisible() && tooltip.GetMessageHeader() == L"fresh",
                       "V107-DT (a): a fresh card replaces the dismissed one before the pump");
            PumpThreadMessagesOnce(); // the stale kDismissMessage must be dropped
            TEST_CHECK(tooltip.IsVisible(),
                       "V107-DT (a): stale kDismissMessage does NOT tear down a fresher card");
            TEST_CHECK(tooltip.GetMessageHeader() == L"fresh",
                       "V107-DT (a): the fresher card's content survives the stale message");

            // The ordinary on-thread dismissal path still tears down.
            tooltip.Dismiss();
            TEST_CHECK(!tooltip.IsVisible() && !tooltip.IsMessageMode(),
                       "V107-DT (a): on-thread Dismiss() still tears down (unchanged behavior)");

            // (a-2) DismissThreadSafe carries the same immediate-clear contract.
            tooltip.ShowMessage(150, 150, L"v107", L"card D", TT::kGenNone);
            {
                std::thread producer([&tooltip]() { tooltip.DismissThreadSafe(); });
                producer.join();
            }
            TEST_CHECK(!tooltip.IsVisible(),
                       "V107-DT (a): off-thread DismissThreadSafe() clears visible_ BEFORE returning");
            PumpThreadMessagesOnce();
            TEST_CHECK(!tooltip.IsMessageMode(),
                       "V107-DT (a): DismissThreadSafe's marshaled teardown completed");

            // Off-thread Dismiss() with nothing visible stays a no-op.
            {
                std::thread producer([&tooltip]() { tooltip.Dismiss(); });
                producer.join();
            }
            TEST_CHECK(!tooltip.IsVisible(),
                       "V107-DT (a): off-thread Dismiss() on a hidden card is a no-op");

            tooltip.Destroy();
        }
    }

    // =====================================================================
    // 1b) (G-06, fix-list round 2) the tooltip's show/dismiss transition is
    //     ONE unit. Adversarial sequence: a dismissal begins on card A, a
    //     FRESH card B is published while the dismissal's message is still
    //     queued, and the queued dismissal must be REFUSED (B survives).
    //     The pre-fix split (visible_.exchange, then
    //     dismiss_teardown_pending_.store) left {shown=true, owed=true} at the
    //     same time in exactly that window and tore card B down.
    // =====================================================================
    {
        using CV = emebalachat::TooltipCardVisibility;
        CV vis;
        TEST_CHECK(!vis.IsVisible(), "V107-DT (G-06): a fresh state has nothing visible");
        uint64_t token = 0;
        TEST_CHECK(!vis.TryBeginDismiss(token),
                   "V107-DT (G-06): dismissing nothing is refused (the caller's early return)");

        const uint64_t card_a = vis.PublishShow();
        TEST_CHECK(vis.IsVisible() && card_a != CV::kNoGeneration &&
                       vis.Generation() == card_a,
                   "V107-DT (G-06): publishing a card marks it visible under a new generation");

        // The off-thread Dismiss(): visibility cleared, teardown owed, and the
        // generation of the card it began on - all from ONE transition.
        TEST_CHECK(vis.TryBeginDismiss(token) && token == card_a,
                   "V107-DT (G-06): the dismissal is stamped with the card it began on");
        TEST_CHECK(!vis.IsVisible() && vis.DismissalOwed(),
                   "V107-DT (G-06): the begun dismissal is hidden + teardown-owed (no torn state)");

        // ADVERSARIAL INTERLEAVING: a fresh card lands while the dismissal's
        // message is still queued behind it.
        const uint64_t card_b = vis.PublishShow();
        TEST_CHECK(card_b != card_a && vis.IsVisible() && !vis.DismissalOwed(),
                   "V107-DT (G-06): the fresh card publishes visible with nothing owed");
        TEST_CHECK(!vis.TryCompleteDismiss(token),
                   "V107-DT (G-06): the stale dismissal is refused - it cannot tear the fresh card down");
        TEST_CHECK(vis.IsVisible() && vis.Generation() == card_b,
                   "V107-DT (G-06): the fresh card survives the stale dismissal");

        // The fresh card's OWN dismissal still completes (the seam is not stuck).
        uint64_t token_b = 0;
        TEST_CHECK(vis.TryBeginDismiss(token_b) && token_b == card_b,
                   "V107-DT (G-06): the fresh card's dismissal is stamped with ITS generation");
        TEST_CHECK(vis.TryCompleteDismiss(token_b),
                   "V107-DT (G-06): the current dismissal completes and owes a teardown");
        TEST_CHECK(!vis.IsVisible() && !vis.DismissalOwed(),
                   "V107-DT (G-06): completing consumes the owed stamp");
        TEST_CHECK(!vis.TryCompleteDismiss(token_b),
                   "V107-DT (G-06): a duplicate completion is a no-op");
        TEST_CHECK(!vis.TryCompleteDismiss(0),
                   "V107-DT (G-06): a zero stamp (forged/legacy message) never completes");

        // R1-G restore: a failed dismissal post restores the coherent pre-post
        // state (shown, nothing owed) so a later path retries - but ONLY while
        // that same dismissal still owns the state.
        const uint64_t card_c = vis.PublishShow();
        uint64_t token_c = 0;
        TEST_CHECK(vis.TryBeginDismiss(token_c) && token_c == card_c,
                   "V107-DT (G-06): the restore fixture begins on card C");
        TEST_CHECK(vis.RestoreShown(token_c),
                   "V107-DT (G-06/R1-G): a failed post restores 'shown, nothing owed'");
        TEST_CHECK(vis.IsVisible() && !vis.DismissalOwed(),
                   "V107-DT (G-06/R1-G): the restored state is coherent");
        TEST_CHECK(!vis.TryCompleteDismiss(token_c),
                   "V107-DT (G-06/R1-G): the restored state has no teardown to complete");
        TEST_CHECK(!vis.RestoreShown(token_c),
                   "V107-DT (G-06/R1-G): a duplicate restore is refused");
        // A STALE restore must not resurrect a card that superseded it.
        const uint64_t card_d = vis.PublishShow();
        TEST_CHECK(card_d != card_c, "V107-DT (G-06): the superseding card has a newer generation");
        TEST_CHECK(!vis.RestoreShown(token_c),
                   "V107-DT (G-06/R1-G): a stale restore is refused");
        TEST_CHECK(vis.IsVisible() && vis.Generation() == card_d,
                   "V107-DT (G-06/R1-G): the superseding card is untouched by the stale restore");
    }

    // =====================================================================
    // 1c) (G-06) the same sequence through a LIVE window: dismiss posted
    //     off-thread, fresh card shown before the pump, the queued dismissal
    //     must be a no-op - and the fresh card's own dismissal must still work.
    // =====================================================================
    {
        TT tooltip;
        if (!tooltip.Create(hInst)) {
            std::cout << "[SKIP] V107-DT: G-06 live fixture could not create a tooltip."
                      << std::endl;
        } else {
            tooltip.ShowMessage(160, 160, L"v107", L"g06 card A", TT::kGenNone);
            {
                std::thread producer([&tooltip]() { tooltip.Dismiss(); });
                producer.join();
            }
            TEST_CHECK(!tooltip.IsVisible(),
                       "V107-DT (G-06 live): the off-thread dismissal hid card A at once");
            tooltip.ShowMessage(170, 170, L"fresh", L"g06 card B", TT::kGenNone);
            TEST_CHECK(tooltip.IsVisible() && tooltip.GetMessageHeader() == L"fresh",
                       "V107-DT (G-06 live): card B is published before the pump");
            PumpThreadMessagesOnce(); // the stale kDismissMessage must be refused
            TEST_CHECK(tooltip.IsVisible() && tooltip.GetMessageHeader() == L"fresh",
                       "V107-DT (G-06 live): the stale dismissal did NOT tear down the fresh card");
            tooltip.Dismiss();
            TEST_CHECK(!tooltip.IsVisible() && !tooltip.IsMessageMode(),
                       "V107-DT (G-06 live): the fresh card's own dismissal still tears down");
            tooltip.Destroy();
        }
    }

    // =====================================================================
    // 2) (e) pure generation-stamp decision (DragIconWindow::ShouldApplyShowAt).
    // =====================================================================
    {
        TEST_CHECK(!DI::ShouldApplyShowAt(0, 0),
                   "V107-DT (e): a zero stamp never applies (no pending show)");
        TEST_CHECK(!DI::ShouldApplyShowAt(0, 7),
                   "V107-DT (e): a zero stamp never applies even with a live slot");
        TEST_CHECK(DI::ShouldApplyShowAt(1, 1),
                   "V107-DT (e): the newest stamped show applies");
        TEST_CHECK(DI::ShouldApplyShowAt(9, 9),
                   "V107-DT (e): higher stamps behave identically");
        TEST_CHECK(!DI::ShouldApplyShowAt(4, 5),
                   "V107-DT (e): a show superseded by a newer one is dropped");
        TEST_CHECK(!DI::ShouldApplyShowAt(5, 4),
                   "V107-DT (e): a forged/newer stamp mismatching the slot is dropped");
        TEST_CHECK(!DI::ShouldApplyShowAt(5, 0),
                   "V107-DT (e): a show invalidated by Hide() (slot emptied) is dropped");
    }

    // =====================================================================
    // 3) (e) live stale-show drop: the fact-5 race end to end on a real
    //    DragIconWindow (show posted before an inline Hide() must not land).
    // =====================================================================
    {
        DI icon;
        if (!icon.Create(hInst)) {
            std::cout << "[SKIP] V107-DT: DragIconWindow could not be created." << std::endl;
        } else {
            // Post the show from a non-GUI thread (the mouse-hook call shape),
            // then Hide() INLINE on the GUI thread - the exact race from the
            // audit (the inline hide bypasses the posted queue).
            {
                std::thread producer([&icon]() {
                    DI::RequestShowAt(icon.GetHwnd(), 500, 400);
                });
                producer.join();
            }
            icon.Hide();
            PumpThreadMessagesOnce();
            TEST_CHECK(!icon.IsVisible(),
                       "V107-DT (e): kShowAtMessage posted before Hide() is dropped as stale "
                       "(no stranded icon at a dead selection site)");

            // A fresh request after the hide still shows (the seam is not
            // stuck-closed by the invalidation).
            TEST_CHECK(DI::RequestShowAt(icon.GetHwnd(), 520, 420),
                       "V107-DT (e): a fresh RequestShowAt posts");
            PumpThreadMessagesOnce();
            TEST_CHECK(icon.IsVisible(),
                       "V107-DT (e): a fresh show applies after the hide");

            // Two rapid shows coalesce latest-wins: the older stamp is dropped,
            // the newer one renders.
            {
                std::thread producer([&icon]() {
                    DI::RequestShowAt(icon.GetHwnd(), 540, 440);
                    DI::RequestShowAt(icon.GetHwnd(), 560, 460);
                });
                producer.join();
            }
            PumpThreadMessagesOnce();
            TEST_CHECK(icon.IsVisible(),
                       "V107-DT (e): rapid shows leave the icon visible (latest wins)");
            RECT r = {};
            ::GetWindowRect(icon.GetHwnd(), &r);
            TEST_CHECK(r.left == 560 && r.top == 460,
                       "V107-DT (e): the newest of two rapid shows owns the position");

            icon.Hide();
            TEST_CHECK(!icon.IsVisible(), "V107-DT (e): Hide() still hides the icon");
            // (e-2) null hwnd still fails cleanly (regression guard).
            TEST_CHECK(!DI::RequestShowAt(nullptr, 10, 20),
                       "V107-DT (e): RequestShowAt(null hwnd) fails cleanly");
            icon.Destroy();
        }
    }

    // =====================================================================
    // 4) (f) R1-D: the pure DragShowArbiter interleavings, driven
    //    deterministically at the API level (the arbiter's guarantee: the slot
    //    always holds the LAST BeginShow, and a Hide always beats a show stamped
    //    before it), plus a live concurrent stress loop on a real icon.
    // =====================================================================
    {
        // (f-1) latest-wins: the newer show owns the slot; the older is dropped.
        {
            emebalachat::DragShowArbiter arb;
            const uint64_t g1 = arb.BeginShow(100, 200);
            const uint64_t g2 = arb.BeginShow(300, 400);
            TEST_CHECK(g2 > g1, "V107-DT (f): BeginShow tokens are monotonic");
            int x = -1, y = -1;
            TEST_CHECK(!arb.TryApply(g1, x, y),
                       "V107-DT (f): the older of two shows is dropped (latest wins)");
            TEST_CHECK(arb.TryApply(g2, x, y) && x == 300 && y == 400,
                       "V107-DT (f): the newest show applies with its OWN coords (no tearing)");
        }
        // (f-2) Hide defeats a show stamped before it (the resurrection race the
        // split-atomic design had: a producer publishing slot=gen AFTER a Hide
        // emptied it). BeginShow is atomic now, so that interleaving is
        // impossible; the guarantee is asserted directly.
        {
            emebalachat::DragShowArbiter arb;
            const uint64_t g1 = arb.BeginShow(100, 200);
            arb.Invalidate();
            int x = -1, y = -1;
            TEST_CHECK(!arb.TryApply(g1, x, y),
                       "V107-DT (f): a show stamped before Hide() is dropped (no resurrection)");
        }
        // (f-3) a show AFTER a Hide still applies (seam not stuck-closed).
        {
            emebalachat::DragShowArbiter arb;
            arb.Invalidate();
            const uint64_t g2 = arb.BeginShow(300, 400);
            int x = -1, y = -1;
            TEST_CHECK(arb.TryApply(g2, x, y) && x == 300 && y == 400,
                       "V107-DT (f): a fresh show after Hide() still applies");
        }
        // (f-4) the zero sentinel never applies and BeginShow never returns it.
        {
            emebalachat::DragShowArbiter arb;
            const uint64_t g = arb.BeginShow(5, 6);
            int x = -1, y = -1;
            TEST_CHECK(!arb.TryApply(0, x, y), "V107-DT (f): a zero stamp never applies");
            TEST_CHECK(g != 0, "V107-DT (f): BeginShow never returns the zero sentinel");
        }
        // (f-5) adversarial concurrency on a LIVE icon: a producer RequestShowAt
        // races a GUI-thread Hide() every trial. The winner is not deterministic,
        // but the INVARIANT is: the icon is either hidden or shows exactly the one
        // requested position - never a torn (x2,y1) point, never a stale
        // resurrection after Hide. This is the seam the deterministic (f-1..4)
        // cases prove in isolation, exercised under real thread interleaving.
        {
            DI icon;
            if (!icon.Create(hInst)) {
                std::cout << "[SKIP] V107-DT: DragIconWindow stress could not be created." << std::endl;
            } else {
                for (int trial = 0; trial < 40; ++trial) {
                    const int px = 640 + trial;
                    const int py = 520 + trial;
                    std::atomic<bool> go{false};
                    std::thread producer([&icon, &go, px, py]() {
                        while (!go.load(std::memory_order_acquire)) {}
                        DI::RequestShowAt(icon.GetHwnd(), px, py);
                    });
                    go.store(true, std::memory_order_release);
                    icon.Hide(); // GUI thread, races the producer's RequestShowAt
                    producer.join();
                    PumpThreadMessagesOnce();
                    if (icon.IsVisible()) {
                        RECT r = {};
                        ::GetWindowRect(icon.GetHwnd(), &r);
                        TEST_CHECK(r.left == px && r.top == py,
                                   "V107-DT (f): concurrent show/Hide never leaves torn or stale coords");
                    }
                    icon.Hide();
                    PumpThreadMessagesOnce();
                }
                icon.Destroy();
            }
        }
        // (f-6) G-10 (fix-list round 2): Hide -> BeginShow -> drain. A hide
        // stamped BEFORE a newer show must not invalidate the newer show's slot
        // when its marshaled message is finally drained.
        {
            emebalachat::DragShowArbiter arb;
            arb.BeginShow(100, 200);
            const uint64_t hide_token = arb.Invalidate();
            const uint64_t newer = arb.BeginShow(300, 400);
            int x = -1, y = -1;
            TEST_CHECK(!arb.InvalidateIfCurrent(hide_token),
                       "V107-DT (f/G-10): draining a superseded hide does NOT invalidate");
            TEST_CHECK(arb.TryApply(newer, x, y) && x == 300 && y == 400,
                       "V107-DT (f/G-10): the newer show survives Hide->BeginShow->drain");
            // The same hide drained while still current DOES invalidate.
            const uint64_t hide_current = arb.Invalidate();
            TEST_CHECK(arb.InvalidateIfCurrent(hide_current),
                       "V107-DT (f/G-10): a current hide still invalidates on drain");
            TEST_CHECK(!arb.TryApply(newer, x, y),
                       "V107-DT (f/G-10): a current hide still drops the show it targets");
            TEST_CHECK(!arb.InvalidateIfCurrent(hide_current),
                       "V107-DT (f/G-10): a duplicate drain of the same hide is a no-op");
            TEST_CHECK(!arb.InvalidateIfCurrent(0),
                       "V107-DT (f/G-10): a zero hide token never invalidates");
        }
        // (f-7) G-10 live: hide requested from a non-GUI thread, then a NEWER
        // show, then the pump drains both. The icon must be VISIBLE at the new
        // position: before the fix the drained hide re-invalidated the arbiter
        // (pending=0) and the newer show's TryApply failed.
        {
            DI icon;
            if (!icon.Create(hInst)) {
                std::cout << "[SKIP] V107-DT: G-10 live fixture could not be created." << std::endl;
            } else {
                {
                    std::thread producer([&icon]() { icon.Hide(); });
                    producer.join();
                }
                TEST_CHECK(DI::RequestShowAt(icon.GetHwnd(), 580, 480),
                           "V107-DT (f/G-10 live): the newer show posts after the queued hide");
                PumpThreadMessagesOnce();
                TEST_CHECK(icon.IsVisible(),
                           "V107-DT (f/G-10 live): the newer show survives the drained hide");
                RECT r = {};
                ::GetWindowRect(icon.GetHwnd(), &r);
                TEST_CHECK(r.left == 580 && r.top == 480,
                           "V107-DT (f/G-10 live): the surviving show owns its own position");
                icon.Hide();
                PumpThreadMessagesOnce();
                TEST_CHECK(!icon.IsVisible(),
                           "V107-DT (f/G-10 live): an inline Hide() still hides (unchanged)");
                icon.Destroy();
            }
        }
        // (f-8) F-2 (round 3): the REVERSED drain order. A newer show applies
        // FIRST, then the stale hide message drains. Before the fix
        // CompleteHide() logged the stale token and STILL ran the
        // unconditional HideWindow(), hiding the icon the newer show had just
        // placed while NO further show message was pending (it had already
        // drained) - the drag-release affordance vanished outright. The
        // filtered PeekMessageW is what makes the ordering deterministic: the
        // hide is posted first (hook-thread Hide), the show second, and the
        // show message is dispatched out of FIFO order.
        {
            DI icon;
            if (!icon.Create(hInst)) {
                std::cout << "[SKIP] V107-DT: F-2 reversed-drain fixture could not be created."
                          << std::endl;
            } else {
                {
                    std::thread hider([&icon]() { icon.Hide(); });
                    hider.join(); // the hide message is queued now
                }
                TEST_CHECK(DI::RequestShowAt(icon.GetHwnd(), 600, 470),
                           "V107-DT (f/F-2): the newer show posts behind the queued hide");
                MSG show_msg = {};
                const bool drained_show =
                    ::PeekMessageW(&show_msg, icon.GetHwnd(), DI::kShowAtMessage,
                                   DI::kShowAtMessage, PM_REMOVE) != FALSE;
                TEST_CHECK(drained_show, "V107-DT (f/F-2): the show message is drained first");
                if (drained_show) {
                    ::TranslateMessage(&show_msg);
                    ::DispatchMessageW(&show_msg);
                }
                TEST_CHECK(icon.IsVisible(),
                           "V107-DT (f/F-2): the newer show applied before the stale hide drains");
                PumpThreadMessagesOnce(); // drains the stale hide only
                TEST_CHECK(icon.IsVisible(),
                           "V107-DT (f/F-2): the stale hide does NOT tear down the applied newer icon");
                RECT r = {};
                ::GetWindowRect(icon.GetHwnd(), &r);
                TEST_CHECK(r.left == 600 && r.top == 470,
                           "V107-DT (f/F-2): the surviving icon keeps the newer show's position");
                icon.Hide();
                PumpThreadMessagesOnce();
                TEST_CHECK(!icon.IsVisible(),
                           "V107-DT (f/F-2): an inline Hide() still hides (unchanged)");
                icon.Destroy();
            }
        }
    }

    // =====================================================================
    // 5) structural pins on the NOT-unit-testable seams (main.cpp lambdas).
    // =====================================================================
    {
        const std::string main_cpp = V107DtReadSrc("src\\main.cpp");
        const std::string tip_cpp = V107DtReadSrc("src\\ui\\tooltip.cpp");
        const std::string tip_hpp = V107DtReadSrc("src\\ui\\tooltip.hpp");
        const std::string icon_cpp = V107DtReadSrc("src\\ui\\drag_icon.cpp");
        const std::string icon_hpp = V107DtReadSrc("src\\ui\\drag_icon.hpp");
        if (main_cpp.empty() || tip_cpp.empty() || icon_cpp.empty()) {
            std::cout << "[SKIP] V107-DT source pins: sources not resolvable." << std::endl;
        } else {
            // (a) pins: off-thread Dismiss performs the WHOLE show/dismiss
            // transition (visibility cleared, teardown owed, card generation
            // captured) before the post, and the handler completes it exactly
            // once for that generation; the handler is idempotent.
            const std::string dismiss_body = V107DtFuncBody(tip_cpp, "void TooltipWindow::Dismiss()");
            const size_t begin_dismiss = dismiss_body.find("card_visibility_.TryBeginDismiss(");
            const size_t post = dismiss_body.find("::PostMessageW(hwnd_, kDismissMessage");
            TEST_CHECK(begin_dismiss != std::string::npos && post != std::string::npos &&
                           begin_dismiss < post,
                       "V107-DT pin (a/G-06): Dismiss() starts the dismissal BEFORE posting kDismissMessage");
            // The transition itself is one locked unit in the header: visible
            // cleared, teardown owed, generation captured - in that order.
            const size_t begin_decl = tip_hpp.find("bool TryBeginDismiss(uint64_t& out_generation)");
            TEST_CHECK(begin_decl != std::string::npos,
                       "V107-DT pin (a/G-06): TooltipCardVisibility::TryBeginDismiss exists");
            {
                const std::string begin_slice =
                    (begin_decl == std::string::npos) ? std::string()
                                                      : tip_hpp.substr(begin_decl, 400);
                const size_t hid = begin_slice.find("visible_ = false;");
                const size_t owed = begin_slice.find("teardown_owed_ = true;");
                const size_t gen = begin_slice.find("out_generation = generation_;");
                TEST_CHECK(hid != std::string::npos && owed != std::string::npos &&
                               gen != std::string::npos && hid < owed && owed < gen,
                           "V107-DT pin (a/G-06): the dismissal transition is ONE unit "
                           "(hidden -> owed -> generation)");
            }
            TEST_CHECK(tip_cpp.find("void TooltipWindow::TeardownCard()") != std::string::npos,
                       "V107-DT pin (a): the shared TeardownCard() seam exists");
            const std::string wnd = V107DtFuncBody(tip_cpp, "LRESULT CALLBACK TooltipWindow::WndProc");
            const size_t hdl = wnd.find("case kDismissMessage:");
            const size_t complete =
                wnd.find("TryCompleteDismiss(dismiss_generation)", hdl);
            TEST_CHECK(hdl != std::string::npos && complete != std::string::npos &&
                           wnd.find("TeardownCard()", complete) != std::string::npos,
                       "V107-DT pin (a/G-06): kDismissMessage tears down exactly once, only for "
                       "the generation it carries");
            TEST_CHECK(tip_hpp.find("TooltipCardVisibility card_visibility_;") != std::string::npos,
                       "V107-DT pin (a/G-06): the window owns the coherent visibility state");
            TEST_CHECK(tip_cpp.find("card_visibility_.ClearDismissalOwed()") != std::string::npos,
                       "V107-DT pin (a): teardown releases the owed stamp");
            TEST_CHECK(tip_cpp.find("card_visibility_.PublishShow()") != std::string::npos,
                       "V107-DT pin (a/G-06): every Show* path publishes through the same type");
            // R1-G pin: the off-thread Dismiss checks the PostMessageW result
            // (bounded retry + coherent restore on persistent failure), so a
            // failed post can never strand a shown window with no teardown owed.
            const size_t post_ok = dismiss_body.find("== TRUE", post);
            TEST_CHECK(post != std::string::npos && post_ok != std::string::npos &&
                           dismiss_body.find("static_cast<WPARAM>(dismiss_generation)",
                                             post) < post_ok,
                       "V107-DT pin (a/R1-G): the off-thread Dismiss posts the card generation "
                       "and checks the PostMessageW result");
            // G-12 (fix-list round 2): the retry runs on the keyboard/mouse hook
            // thread, where Sleep() eats the LowLevelHooksTimeout budget. It
            // must YIELD, never sleep.
            TEST_CHECK(dismiss_body.find("::SwitchToThread()") != std::string::npos,
                       "V107-DT pin (a/G-12): the post retry yields with SwitchToThread()");
            TEST_CHECK(V107DtFuncBody(tip_cpp, "void TooltipWindow::Dismiss()").find("::Sleep(") ==
                           std::string::npos,
                       "V107-DT pin (a/G-12): Dismiss() never sleeps on the hook thread");

            // (b) pins: the global IsVisible() suppression is gone; the drag
            // release dismisses the card and continues to the icon; gestures
            // starting inside the card rect are scoped out via the drag-start
            // point captured in the mouse-down callback.
            TEST_CHECK(main_cpp.find("|| tooltip.IsVisible())") == std::string::npos,
                       "V107-DT pin (b): the tooltip.IsVisible() drag-release suppression is gone");
            TEST_CHECK(main_cpp.find("drag_start_x") != std::string::npos &&
                           main_cpp.find("drag_start_y") != std::string::npos,
                       "V107-DT pin (b): the gesture start point is captured");
            TEST_CHECK(main_cpp.find("gesture started inside tooltip rect") != std::string::npos,
                       "V107-DT pin (b): card-internal gestures are scoped, not globally suppressed");
            const size_t vis_gate = main_cpp.find("if (tooltip.IsVisible()) {");
            const size_t dismiss_call = main_cpp.find("tooltip.Dismiss();", vis_gate);
            const size_t show_call = main_cpp.find("RequestShowAt(drag_icon.GetHwnd()", vis_gate);
            TEST_CHECK(vis_gate != std::string::npos && dismiss_call != std::string::npos &&
                           show_call != std::string::npos && dismiss_call < show_call,
                       "V107-DT pin (b): a visible card is dismissed and the drag icon is still requested");

            // (c) pin: the failure notice returns BEFORE ShowTranslationThreadSafe.
            // Needle is the CALL (`tooltip.ShowTranslationThreadSafe`), not the
            // bare name - the explanatory comment above the call also names it.
            const std::string drag_body = V107DtFuncBody(main_cpp, "auto run_drag_translate = ");
            const size_t failed_notice = drag_body.find("TooltipTranslateFailed");
            const size_t show_trans = drag_body.find("tooltip.ShowTranslationThreadSafe");
            const size_t ret_after = drag_body.find("return;", failed_notice);
            TEST_CHECK(failed_notice != std::string::npos && show_trans != std::string::npos &&
                           ret_after != std::string::npos && ret_after < show_trans,
                       "V107-DT pin (c): empty-result failure notice returns before "
                       "ShowTranslationThreadSafe (no empty ghost card)");

            // (d) pin: the smart-bypass drop shows the reused notice (the
            // notice call must be the FIRST ShowMessageThreadSafe after the
            // bypass gate - i.e. this path, not a later failure notice).
            // (d) pin: the smart-bypass drop shows the reused notice. Re-anchored
            // by I1: the bypass block's own explanatory comment names
            // TooltipNoSelection ~286 chars BEFORE the call, so the StringId
            // must be searched FROM the first ShowMessageThreadSafe, not from
            // the bypass marker. The pin still asserts: the FIRST
            // ShowMessageThreadSafe after the bypass gate references
            // TooltipNoSelection within 300 chars (this path's notice, not a
            // later failure notice).
            const size_t bypass = drag_body.find("drag_bypass path=icon");
            const size_t notice_call = drag_body.find("ShowMessageThreadSafe", bypass);
            const size_t notice_sel = drag_body.find("TooltipNoSelection", notice_call);
            TEST_CHECK(bypass != std::string::npos && notice_call != std::string::npos &&
                           notice_sel != std::string::npos && notice_call < notice_sel &&
                           (notice_sel - notice_call) < 300,
                       "V107-DT pin (d): the smart-bypass drop surfaces a user-visible notice");

            // (e)/(f) pins: the pure decision exists, the single arbiter
            // serializes the publication, and the handler applies atomically.
            TEST_CHECK(icon_hpp.find("static constexpr bool ShouldApplyShowAt(") != std::string::npos,
                       "V107-DT pin (e): the pure show/hide generation decision exists");
            TEST_CHECK(icon_hpp.find("struct DragShowArbiter") != std::string::npos &&
                           icon_hpp.find("uint64_t BeginShow(") != std::string::npos &&
                           icon_hpp.find("uint64_t Invalidate()") != std::string::npos &&
                           icon_hpp.find("bool InvalidateIfCurrent(uint64_t hide_token)") !=
                               std::string::npos &&
                           icon_hpp.find("bool TryApply(") != std::string::npos,
                       "V107-DT pin (f/G-10): the DragShowArbiter exposes BeginShow/Invalidate/"
                       "InvalidateIfCurrent/TryApply");
            TEST_CHECK(icon_cpp.find("DragShowArbiter s_drag_show_arbiter;") != std::string::npos &&
                           icon_cpp.find("s_drag_show_arbiter.BeginShow(x, y)") != std::string::npos &&
                           icon_cpp.find("const uint64_t hide_token = s_drag_show_arbiter.Invalidate()") !=
                               std::string::npos,
                       "V107-DT pin (f): RequestShowAt/Hide route through the one arbiter");
            // G-10: the marshaled hide carries its own token, and the GUI handler
            // re-invalidates only while that token is current.
            TEST_CHECK(icon_cpp.find("kHideMessage, static_cast<WPARAM>(hide_token)") !=
                           std::string::npos,
                       "V107-DT pin (f/G-10): kHideMessage carries the hide's arbiter token");
            const std::string complete_hide =
                V107DtFuncBody(icon_cpp, "void DragIconWindow::CompleteHide(uint64_t hide_token)");
            TEST_CHECK(complete_hide.find("InvalidateIfCurrent(hide_token)") != std::string::npos,
                       "V107-DT pin (f/G-10): the hide drain invalidates only when still current");
            // F-2 (round 3): a superseded hide must RETURN before the window
            // teardown. The old body logged the stale token and fell through
            // to HideWindow(), which hid an icon a newer show had already
            // placed with no further show message pending.
            {
                const size_t stale_guard = complete_hide.find("InvalidateIfCurrent(hide_token)");
                const size_t stale_return = complete_hide.find("return;", stale_guard);
                const size_t teardown = complete_hide.find("HideWindow();", stale_guard);
                TEST_CHECK(stale_guard != std::string::npos && stale_return != std::string::npos &&
                               teardown != std::string::npos && stale_return < teardown,
                           "V107-DT pin (f/F-2): the stale-hide arm returns BEFORE HideWindow()");
            }
            const size_t hide_case_begin = icon_cpp.find("case DragIconWindow::kHideMessage:");
            const size_t hide_case_end =
                (hide_case_begin == std::string::npos)
                    ? std::string::npos
                    : icon_cpp.find("case ", hide_case_begin + 10);
            const std::string hide_case =
                (hide_case_begin == std::string::npos || hide_case_end == std::string::npos)
                    ? std::string()
                    : icon_cpp.substr(hide_case_begin, hide_case_end - hide_case_begin);
            TEST_CHECK(!hide_case.empty() &&
                           hide_case.find("CompleteHide(hide_token)") != std::string::npos &&
                           hide_case.find("pThis->Hide()") == std::string::npos,
                       "V107-DT pin (f/G-10): the kHideMessage handler no longer re-enters Hide()");
            // Round 3 (F-2 + legacy-token seam): a token-less post (WPARAM==0,
            // never a real token) is an unconditional GUI hide; a stamped post
            // routes through CompleteHide, which skips the teardown when a
            // newer show already owns the slot.
            TEST_CHECK(hide_case.find("if (hide_token == 0)") != std::string::npos &&
                           hide_case.find("pThis->HideWindow();") != std::string::npos,
                       "V107-DT pin (F-2): the token-less legacy hide branch is explicit");
            const std::string show_case = V107DtFuncBody(icon_cpp, "case DragIconWindow::kShowAtMessage:");
            TEST_CHECK(show_case.find("s_drag_show_arbiter.TryApply") != std::string::npos,
                       "V107-DT pin (f): the kShowAtMessage handler applies via TryApply (atomic decision + coords)");

            // (g) R1-H (fix-list round 1) + G-03 (round 2): EVERY drag-family
            // prompt source resolves via the GATED DetectLanguageForPrompt
            // accessor, never a bare DetectLanguage call - pinned against a
            // future regression to the ungated label (which feeds the engine
            // prompt AND the pivot).
            // The func-body extractor spans the sibling lambdas too (it stops at
            // the first column-0 brace, past this lambda's indented `};`), so the
            // needles are the path-specific capture variables: `selected` for
            // run_drag_translate and `copied` for the double-Ctrl+C callback.
            const size_t prompt_label =
                drag_body.find("emebalachat::DetectLanguageForPrompt(selected)");
            TEST_CHECK(prompt_label != std::string::npos,
                       "V107-DT pin (g): run_drag_translate resolves the drag prompt label via "
                       "DetectLanguageForPrompt (gated, not a bare DetectLanguage)");
            // G-03: region between the two callback registrations brackets the
            // double-Ctrl+C lambda exactly.
            const size_t dbl_begin = main_cpp.find("hook.SetDoubleCtrlCCallback(");
            const size_t dbl_end =
                (dbl_begin == std::string::npos)
                    ? std::string::npos
                    : main_cpp.find("hook.SetEscCallback(", dbl_begin);
            const std::string dbl_body =
                (dbl_begin == std::string::npos || dbl_end == std::string::npos)
                    ? std::string()
                    : main_cpp.substr(dbl_begin, dbl_end - dbl_begin);
            TEST_CHECK(!dbl_body.empty(),
                       "V107-DT pin (g/G-03): the double-Ctrl+C callback region is locatable");
            TEST_CHECK(dbl_body.find("emebalachat::DetectLanguageForPrompt(copied)") !=
                           std::string::npos,
                       "V107-DT pin (g/G-03): the double-Ctrl+C prompt source uses "
                       "DetectLanguageForPrompt (gated, not a bare DetectLanguage)");
            TEST_CHECK(dbl_body.find("emebalachat::DetectLanguage(copied)") == std::string::npos,
                       "V107-DT pin (g/G-03): no bare DetectLanguage(copied) remains on the "
                       "double-Ctrl+C prompt path");

            // (h) G-13 (fix-list round 2): the double-Ctrl+C terminal branches.
            // R1-A: a strict engine failure shows the modal and RETURNS, and an
            // empty result shows the failure notice and RETURNS - neither may
            // reach ShowTranslationThreadSafe (the empty ghost card).
            const size_t dbl_show = dbl_body.rfind("tooltip.ShowTranslationThreadSafe(");
            const size_t modal_check = dbl_body.find(
                "if (dbl_status == emebalachat::TranslationStatus::CloudConsentBlocked");
            const size_t modal_return =
                (modal_check == std::string::npos)
                    ? std::string::npos
                    : dbl_body.find("return;", modal_check);
            TEST_CHECK(modal_check != std::string::npos && modal_return != std::string::npos &&
                           dbl_show != std::string::npos && modal_return < dbl_show,
                       "V107-DT pin (h/R1-A): the double-Ctrl+C strict-failure modal returns "
                       "before ShowTranslationThreadSafe");
            const size_t dbl_empty = dbl_body.find("TooltipTranslateFailed");
            const size_t dbl_empty_return =
                (dbl_empty == std::string::npos) ? std::string::npos
                                                 : dbl_body.find("return;", dbl_empty);
            TEST_CHECK(dbl_empty != std::string::npos && dbl_empty_return != std::string::npos &&
                           dbl_show != std::string::npos && dbl_empty_return < dbl_show,
                       "V107-DT pin (h/R1-A): the double-Ctrl+C empty-result notice returns "
                       "before ShowTranslationThreadSafe");
            TEST_CHECK(dbl_body.find("RequestEngineUnavailableModal(") != std::string::npos,
                       "V107-DT pin (h/R1-A): the strict-failure modal path exists on the "
                       "double-Ctrl+C flow");

            // (i) G-13 (fix-list round 2): the PRESS-time own-popup gate (R1-E).
            // A gesture whose mouse-down landed on one of our own popup menus is
            // exempted by the drag-release callback UNCONDITIONALLY - before (and
            // independently of) the tooltip-visibility block - so a no-card
            // menu multi-click never summons the drag icon.
            const size_t md_begin = main_cpp.find("mouse_hook.SetMouseDownCallback(");
            const size_t md_end =
                (md_begin == std::string::npos)
                    ? std::string::npos
                    : main_cpp.find("mouse_hook.SetMouseWheelCallback(", md_begin);
            const std::string mouse_down_body =
                (md_begin == std::string::npos || md_end == std::string::npos)
                    ? std::string()
                    : main_cpp.substr(md_begin, md_end - md_begin);
            const size_t mv_begin = main_cpp.find("mouse_hook.SetDragReleaseCallback(");
            const size_t mv_end = (mv_begin == std::string::npos)
                                      ? std::string::npos
                                      : main_cpp.find("mouse_hook.SetMouseDownCallback(", mv_begin);
            const std::string drag_release_body =
                (mv_begin == std::string::npos || mv_end == std::string::npos)
                    ? std::string()
                    : main_cpp.substr(mv_begin, mv_end - mv_begin);
            TEST_CHECK(mouse_down_body.find("drag_press_own_popup.store(false") != std::string::npos &&
                           mouse_down_body.find("drag_press_own_popup.store(true") != std::string::npos,
                       "V107-DT pin (i/R1-E): every press re-evaluates the own-popup predicate");
            const size_t press_gate = drag_release_body.find("drag_press_own_popup.load(");
            const size_t release_vis_gate = drag_release_body.find("if (tooltip.IsVisible())");
            TEST_CHECK(press_gate != std::string::npos && release_vis_gate != std::string::npos &&
                           press_gate < release_vis_gate,
                       "V107-DT pin (i/R1-E): the own-popup gate runs BEFORE the "
                       "tooltip-visibility block (exempt even with no card visible)");
            TEST_CHECK(drag_release_body.find("press started over own popup menu") !=
                           std::string::npos,
                       "V107-DT pin (i/R1-E): the exempted gesture is observable in the log");
        }
    }

    const int failed = g_failed_count - failures_before;
    // Report the suite delta to the caller WITHOUT double counting: when the
    // caller passes the harness global itself, it is already authoritative.
    if (&failures != &g_failed_count) {
        failures += failed;
    }
    if (failed == 0) {
        std::cout << "[PASS] v0.10.7 drag/tooltip dead-zone fixes passed." << std::endl;
    } else {
        std::cout << "[FAIL] v0.10.7 drag/tooltip dead-zone fixes: " << failed
                  << " check(s) failed." << std::endl;
    }
    return failed;
}
