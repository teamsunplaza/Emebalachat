// ---------------------------------------------------------------------------
// 261108_0002 (WS3, v0.10.6) - the floating back-translation verification card
// in the tooltip layer. v0.10.9 (U-B, report §4 Option 2): two-panel
// [역번역]/[원문] comparison card with real clipping, wheel + drag scrolling,
// an always-visible original, a 480 DIP cap and an adaptive auto-hide timer.
//
// User intent (verbatim): "역번역 플로팅 확인 UI: 번역 결과(BBBBB)를 원문 언어로
// 자동 역번역(AAAAA')하여 플로팅 카드로 보여주기".
//
// This suite pins the card's headless-testable contract:
//   (a) TestWs3BackTransCardSize - TooltipWindow::ComputeBackTransCardSize:
//       the fixed 360 DIP width, the 480 DIP ceiling, the two-panel furniture,
//       and monotonicity in BOTH text lengths.
//   (a2) TestWs3BackTransPanelPlan - TooltipWindow::PlanBackTransPanels: the
//       two clipped viewports, the no-overlap and fits-the-card invariants,
//       the shared scroll extent, the "fits -> shrink, overflow -> scroll"
//       rule, and (v0.10.9 fix round 3, H-1) the [원문] reservation:
//       min(measured original height, 45% of the card body) so a short
//       original is fully visible beside a long back-translation.
//   (b) TestWs3BackTransTimerPlan - TooltipWindow::PlanBackTransTimer +
//       BackTransAutohideMsForContent: the 4500 ms floor, the 4500..12000 ms
//       content ramp, pause on the first pointer move, no timer churn on
//       repeated moves, the 1500 ms grace window on leave, and the immediate
//       kill on an explicit dismissal.
//   (c) TestWs3BackTransCardPlacement - TooltipWindow::PlaceBackTransCard:
//       horizontal centring on the anchor, the gap, the above->below flip when
//       the work area has no room, and the shared edge clamp.
//   (d) TestWs3BackTransLabels - FormatBackTransHeader /
//       BackTransNeedsReferenceLine: the " (EN → KO)" suffix shape, the
//       empty-label drop, and the v0.10.9 ALWAYS-show-the-original policy
//       (including an exact round trip).
//   (e) TestWs3BackTransMarshalSeam - the thread-safe entry's live contract on
//       a real TooltipWindow: an off-GUI-thread ShowBackTranslationThreadSafe
//       reaches the model through the value queue, DismissThreadSafe clears
//       pending entries, DrainMarshalQueue counts the payload, and the LIVE
//       card proves computed scrollability + the adaptive delay.
//   (f) TestWs3BackTransStructuralPins - source pins on src/ui/tooltip.cpp /
//       tooltip.hpp and src/worker.cpp: the API names the main.cpp unit is
//       written against, the message id 0x206, the timer id 4003, the width
//       reset in ShowTranslation, the dismiss-on-new-translation call, the
//       per-panel PushAxisAlignedClip windows, the shared wheel/drag plumbing,
//       the section StringIds, and the full-original-line back-translate
//       request.
//
// Self-contained: no main(), and no include beyond what tests/run_tests.cpp
// already provides (it includes ../src/ui/tooltip.hpp, <fstream>, <string>,
// <vector> and the TEST_CHECK harness). The orchestrator wires the #include
// and the single TestWs3Tooltip() call.
// ---------------------------------------------------------------------------

namespace {

// Reads a repo-relative source file the way the other source-pin suites do
// (exe dir, then one and two levels up - the test binary lives in build/).
// CRLF is normalized to LF so line-anchored needles never depend on the
// checkout's autocrlf setting.
std::string Ws3TipReadSrc(const char* rel) {
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

std::size_t Ws3TipCount(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    std::size_t pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

// Function body = [marker, first column-0 '}' after it). Same helper shape the
// W5/C1+C2 and REQ-052 suites use for body-scoped pins.
std::string Ws3TipFuncBody(const std::string& src, const std::string& marker) {
    const size_t i = src.find(marker);
    if (i == std::string::npos) return std::string();
    const size_t j = src.find("\n}", i);
    return src.substr(i, (j == std::string::npos ? src.size() : j) - i);
}

} // namespace

// ---- (a) card size planner ------------------------------------------------

void TestWs3BackTransCardSize() {
    std::cout << "[RUN] Testing WS3 back-translation card size planner..." << std::endl;
    const int failures_before = g_failed_count;

    using TT = emebalachat::TooltipWindow;

    // The card width is FIXED at 360 DIP - the same width ShowTranslation
    // resets to, so the two modes can never leave each other a stale width.
    volatile int fixed_w = TT::kBackTransCardWidthDip;
    TEST_CHECK(fixed_w == 360, "WS3 size: the card is 360 DIP wide");
    TEST_CHECK(TT::ComputeBackTransCardSize(0, 0).width == 360 &&
                   TT::ComputeBackTransCardSize(9999, 9999).width == 360,
               "WS3 size: the width never varies with content");

    // v0.10.9 (U-B, defect 3-2): the ceiling moved 340 -> 480 DIP.
    volatile int min_h = TT::kBackTransCardMinHeightDip;
    volatile int max_h = TT::kBackTransCardMaxHeightDip;
    TEST_CHECK(max_h == 480, "WS3 size (v0.10.9): the card cap is 480 DIP");
    TEST_CHECK(min_h > 0 && max_h > min_h,
               "WS3 size: the min/max band is well formed");

    // v0.10.9 (U-B): an empty payload is the two-panel furniture plus one body
    // line and one [원문] line: 44 + 72 + 20 + 16 = 152 DIP.
    volatile int empty_h = TT::ComputeBackTransCardSize(0, 0).height;
    TEST_CHECK(empty_h == 152,
               "WS3 size (v0.10.9): an empty payload is the furniture + one line each");
    TEST_CHECK(TT::ComputeBackTransCardSize(-5, -5).height == empty_h,
               "WS3 size: negative lengths clamp to the empty payload (defensive)");
    TEST_CHECK(empty_h >= min_h,
               "WS3 size: the two-panel furniture never dips below the floor");

    // Monotonic non-decreasing in the BODY length: a sweep across the whole
    // range, including the wrap boundaries.
    bool body_monotonic = true;
    int prev = TT::ComputeBackTransCardSize(0, 0).height;
    for (int len = 1; len <= 2000; ++len) {
        const int h = TT::ComputeBackTransCardSize(len, 0).height;
        if (h < prev) body_monotonic = false;
        prev = h;
    }
    TEST_CHECK(body_monotonic, "WS3 size: height is monotonic in the body length");

    // Monotonic non-decreasing in the REFERENCE length.
    bool ref_monotonic = true;
    prev = TT::ComputeBackTransCardSize(120, 0).height;
    for (int len = 1; len <= 2000; ++len) {
        const int h = TT::ComputeBackTransCardSize(120, len).height;
        if (h < prev) ref_monotonic = false;
        prev = h;
    }
    TEST_CHECK(ref_monotonic, "WS3 size: height is monotonic in the reference length");

    // An original never shrinks the card (the [원문] panel is additive).
    for (int len = 0; len <= 400; len += 37) {
        const int without = TT::ComputeBackTransCardSize(len, 0).height;
        const int with = TT::ComputeBackTransCardSize(len, 60).height;
        TEST_CHECK(with >= without,
                   "WS3 size: adding an original never shrinks the card");
    }

    // Clamp ceiling: a pathological body stops at the maximum.
    TEST_CHECK(TT::ComputeBackTransCardSize(100000, 100000).height == max_h,
               "WS3 size: a pathological payload clamps at the maximum height");

    // Growth actually happens at the wrap boundary (one extra line = one line
    // height for the BODY and the 16 DIP [원문] line for the original).
    volatile int per_line = TT::kBackTransCharsPerLine;
    volatile int line_h = TT::kBackTransLineHeightDip;
    volatile int ref_line_h = TT::kBackTransRefLineDip;
    TEST_CHECK(per_line == 44 && line_h == 20 && ref_line_h == 16,
               "WS3 size: the wrap width and both line heights are the documented constants");
    TEST_CHECK(TT::ComputeBackTransCardSize(per_line * 5, 0).height + line_h ==
                   TT::ComputeBackTransCardSize(per_line * 5 + 1, 0).height,
               "WS3 size: crossing the body wrap boundary adds exactly one body line");
    TEST_CHECK(TT::ComputeBackTransCardSize(0, per_line * 3).height + ref_line_h ==
                   TT::ComputeBackTransCardSize(0, per_line * 3 + 1).height,
               "WS3 size: crossing the original's wrap boundary adds exactly one ref line");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] WS3 back-translation card size planner passed." << std::endl;
    } else {
        std::cout << "[FAIL] WS3 back-translation card size planner: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (a2) two-panel layout planner (v0.10.9 U-B) ---------------------------

void TestWs3BackTransPanelPlan() {
    std::cout << "[RUN] Testing WS3 two-panel back-translation layout..." << std::endl;
    const int failures_before = g_failed_count;

    using TT = emebalachat::TooltipWindow;

    // ---- the report's own case: 20-line back-translation + 3-line original --
    // 20 * 20 = 400 DIP of body, 3 * 16 = 48 DIP of original.
    // v0.10.9 fix round 3 (H-1 / QA F3): the original reserves its measured
    // height up to 45% of the body area, so this case shows the WHOLE original
    // at offset 0 instead of a ~43 DIP slice only scrollable into view.
    {
        const TT::BackTransPanelPlan p = TT::PlanBackTransPanels(400, 48);
        TEST_CHECK(p.card_height_dip == TT::kBackTransCardMaxHeightDip,
                   "WS3 panels: the long comparison uses the full 480 DIP card");
        TEST_CHECK(p.scrollable, "WS3 panels: a long comparison is scrollable");
        TEST_CHECK(p.back_view_h >= TT::kBackTransMinPanelBodyDip &&
                       p.ref_view_h >= TT::kBackTransMinPanelBodyDip,
                   "WS3 panels: both viewports keep their minimum body height");
        TEST_CHECK(p.back_view_h > p.ref_view_h,
                   "WS3 panels: the overflowing panel gets the larger share");
        // No overlap: the back viewport ends before the [원문] panel starts.
        TEST_CHECK(p.back_view_top + p.back_view_h <= p.ref_panel_top,
                   "WS3 panels: the two viewports never overlap");
        // The whole stack closes the card exactly (no overdraw past the edge).
        TEST_CHECK(p.ref_view_top + p.ref_view_h + TT::kBackTransPanelBottomPadDip +
                           TT::kBackTransCardBottomPadDip ==
                       p.card_height_dip,
                   "WS3 panels: both panels + furniture fit the card exactly");
        // The shared scroll extent is the largest single-panel overflow.
        TEST_CHECK(p.max_scroll_offset ==
                       (p.back_content_h - p.back_view_h > p.ref_content_h - p.ref_view_h
                            ? p.back_content_h - p.back_view_h
                            : p.ref_content_h - p.ref_view_h),
                   "WS3 panels: the shared offset extent is the largest panel overflow");
        TEST_CHECK(p.max_scroll_offset > 0, "WS3 panels: the overflow case actually scrolls");
        TEST_CHECK(p.shared_viewport_h ==
                       (p.back_view_h > p.ref_view_h ? p.back_view_h : p.ref_view_h),
                   "WS3 panels: the shared clamp basis is the taller viewport");

        // ---- H-1 pins (fix round 3): the original is visible WHOLE ----------
        TEST_CHECK(p.ref_view_h == p.ref_content_h,
                   "WS3 H-1: the 3-line original is fully visible without scrolling");
        TEST_CHECK(p.ref_view_h == 3 * TT::kBackTransRefLineDip,
                   "WS3 H-1: the reservation equals the measured 3 original lines");
        TEST_CHECK(p.ref_view_h <= p.body_area_dip * TT::kBackTransRefSharePct / 100,
                   "WS3 H-1: the original stays inside the 45% share cap");
        TEST_CHECK(p.max_scroll_offset == p.back_content_h - p.back_view_h,
                   "WS3 H-1: only the back-translation has overflow left to scroll");
        TEST_CHECK(p.back_view_h + p.ref_view_h == p.body_area_dip,
                   "WS3 H-1: the two viewports fill the body area exactly");
        TEST_CHECK(p.body_area_dip == TT::kBackTransCardMaxHeightDip -
                                          TT::kBackTransContentTopDip -
                                          TT::kBackTransFurnitureDip,
                   "WS3 H-1: the body area is the card minus header furniture");
    }

    // ---- H-1 (fix round 3): a pathological original hits the 45% cap -------
    // 30 * 16 = 480 DIP of original (taller than the whole body area) beside
    // the same 20-line body: both sides overflow, so the cap decides.
    {
        const TT::BackTransPanelPlan p =
            TT::PlanBackTransPanels(400, 30 * TT::kBackTransRefLineDip);
        const int cap = p.body_area_dip * TT::kBackTransRefSharePct / 100;
        TEST_CHECK(p.scrollable && p.ref_content_h > p.body_area_dip,
                   "WS3 H-1: the 30-line original overflows the body area");
        TEST_CHECK(p.ref_view_h == cap,
                   "WS3 H-1: an overflowing original is capped at 45% of the body");
        TEST_CHECK(p.ref_view_h < p.ref_content_h,
                   "WS3 H-1: the capped original still scrolls (no text is lost)");
        TEST_CHECK(p.back_view_h == p.body_area_dip - p.ref_view_h && p.back_view_h > cap,
                   "WS3 H-1: the back-translation takes the remainder and stays larger");
        TEST_CHECK(p.ref_view_top + p.ref_view_h + TT::kBackTransPanelBottomPadDip +
                           TT::kBackTransCardBottomPadDip ==
                       p.card_height_dip,
                   "WS3 H-1: the capped layout still closes the card exactly");
        TEST_CHECK(p.max_scroll_offset == p.ref_content_h - p.ref_view_h,
                   "WS3 H-1: the capped original is the panel with the longest overflow");
    }

    // ---- a short payload FITS: card shrinks, no scroll, natural sizes -------
    {
        const TT::BackTransPanelPlan p = TT::PlanBackTransPanels(40, 16);
        TEST_CHECK(!p.scrollable, "WS3 panels: a fitting payload never scrolls");
        TEST_CHECK(p.max_scroll_offset == 0, "WS3 panels: a fitting payload has no scroll range");
        TEST_CHECK(p.back_view_h == 40 && p.ref_view_h == 16,
                   "WS3 panels: a fitting payload keeps its natural viewport heights");
        TEST_CHECK(p.card_height_dip == 172,
                   "WS3 panels: the card shrinks to exactly the content it holds");
        TEST_CHECK(p.ref_view_top + p.ref_view_h + TT::kBackTransPanelBottomPadDip +
                           TT::kBackTransCardBottomPadDip ==
                       p.card_height_dip,
                   "WS3 panels: the fits-the-card invariant holds in the short case too");
        // H-1 (fix round 3): content that already fits must be byte-for-byte
        // the pre-fix layout - the reservation only governs overflow.
        TEST_CHECK(p.back_view_top == 64 && p.ref_panel_top == 112 &&
                       p.ref_label_top == 118 && p.ref_view_top == 138,
                   "WS3 H-1: the fits-case stack (64/112/118/138) is unchanged");
        TEST_CHECK(p.body_area_dip == 56,
                   "WS3 H-1: the fits-case body area is the content it holds");
    }

    // ---- a short ORIGINAL must not be starved in the other direction -------
    // (a 1-line body next to a very long original)
    {
        const TT::BackTransPanelPlan p = TT::PlanBackTransPanels(20, 800);
        TEST_CHECK(p.scrollable && p.ref_view_h > p.back_view_h,
                   "WS3 panels: the original wins the space when IT is the long one");
        TEST_CHECK(p.back_view_h == TT::kBackTransMinPanelBodyDip ||
                       p.back_view_h == p.back_content_h,
                   "WS3 panels: the short body never takes more than it needs");
        TEST_CHECK(p.ref_view_top + p.ref_view_h + TT::kBackTransPanelBottomPadDip +
                           TT::kBackTransCardBottomPadDip ==
                       p.card_height_dip,
                   "WS3 panels: the [원문]-heavy case still fits the card");
    }

    // ---- header_extra (wrapped verdict pill) pushes the whole stack down ----
    {
        const TT::BackTransPanelPlan no_extra = TT::PlanBackTransPanels(200, 32, 0);
        const TT::BackTransPanelPlan extra = TT::PlanBackTransPanels(200, 32, 30);
        TEST_CHECK(extra.content_top_dip == no_extra.content_top_dip + 30,
                   "WS3 panels: the wrapped pill row moves the panels down by its height");
        TEST_CHECK(extra.back_label_top == extra.content_top_dip,
                   "WS3 panels: the [역번역] label follows the content top");
        TEST_CHECK(extra.card_height_dip == no_extra.card_height_dip + 30 ||
                       extra.card_height_dip == TT::kBackTransCardMaxHeightDip,
                   "WS3 panels: the extra pill row grows the card (or hits the cap)");
    }

    // ---- degenerate inputs -------------------------------------------------
    {
        const TT::BackTransPanelPlan empty = TT::PlanBackTransPanels(0, 0);
        TEST_CHECK(empty.card_height_dip > 0 && empty.back_view_h == 0 &&
                       empty.ref_view_h == 0 && !empty.scrollable &&
                       empty.max_scroll_offset == 0,
                   "WS3 panels: an empty payload yields a non-scrolling, viewport-less card");
        TEST_CHECK(TT::PlanBackTransPanels(-40, -16).card_height_dip ==
                       empty.card_height_dip,
                   "WS3 panels: negative measurements clamp to zero");
        const TT::BackTransPanelPlan pathological = TT::PlanBackTransPanels(100000, 100000);
        TEST_CHECK(pathological.card_height_dip == TT::kBackTransCardMaxHeightDip &&
                       pathological.max_scroll_offset > 0,
                   "WS3 panels: a pathological payload caps the card and scrolls");
        TEST_CHECK(pathological.ref_view_top + pathological.ref_view_h +
                           TT::kBackTransPanelBottomPadDip +
                           TT::kBackTransCardBottomPadDip ==
                       pathological.card_height_dip,
                   "WS3 panels: even the pathological case cannot overdraw the card");
    }

    // ---- monotonic in the body extent + the layout invariants over a grid --
    {
        bool monotonic = true;
        int prev_h = TT::PlanBackTransPanels(0, 0).card_height_dip;
        for (int c = 0; c <= 600; c += 7) {
            const int h = TT::PlanBackTransPanels(c, 40).card_height_dip;
            if (h < prev_h) monotonic = false;
            prev_h = h;
        }
        TEST_CHECK(monotonic, "WS3 panels: the card height is monotonic in the body extent");

        // Every (body, original) pair must satisfy the two render-safety
        // invariants: the panels never overlap, and the stack never overdraws
        // the card. This is the grid the old single-viewport code failed.
        bool invariants_hold = true;
        bool non_negative = true;
        // H-1 (fix round 3) over the same grid: the original ALWAYS receives
        // min(measured, 45% of the body), i.e. it is fully visible whenever it
        // fits the cap, and NO panel is ever handed more than it needs.
        bool ref_reserve_holds = true;
        bool no_wasted_space = true;
        for (int body = 0; body <= 720; body += 60) {
            for (int orig = 0; orig <= 400; orig += 40) {
                const TT::BackTransPanelPlan p = TT::PlanBackTransPanels(body, orig);
                if (p.back_view_top + p.back_view_h > p.ref_panel_top) invariants_hold = false;
                if (p.ref_view_top + p.ref_view_h + TT::kBackTransPanelBottomPadDip +
                        TT::kBackTransCardBottomPadDip != p.card_height_dip) {
                    invariants_hold = false;
                }
                if (p.back_view_h < 0 || p.ref_view_h < 0 || p.max_scroll_offset < 0 ||
                    p.shared_viewport_h < 0) {
                    non_negative = false;
                }
                const int cap = p.body_area_dip * TT::kBackTransRefSharePct / 100;
                const int reserved = p.ref_content_h < cap ? p.ref_content_h : cap;
                if (p.ref_view_h < reserved) ref_reserve_holds = false;
                if (p.ref_view_h > p.ref_content_h || p.back_view_h > p.back_content_h) {
                    no_wasted_space = false;
                }
                if (p.scrollable &&
                    p.back_view_h + p.ref_view_h != p.body_area_dip) {
                    no_wasted_space = false;
                }
            }
        }
        TEST_CHECK(invariants_hold,
                   "WS3 panels: the overlap/fit invariants hold across the whole grid");
        TEST_CHECK(non_negative,
                   "WS3 panels: no viewport or scroll extent is ever negative");
        TEST_CHECK(ref_reserve_holds,
                   "WS3 H-1: the original always gets min(measured, 45% of the body)");
        TEST_CHECK(no_wasted_space,
                   "WS3 H-1: no panel is given more than it needs; overflow splits the body exactly");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] WS3 two-panel back-translation layout passed." << std::endl;
    } else {
        std::cout << "[FAIL] WS3 two-panel back-translation layout: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (b) hover pause / auto-dismiss state machine -------------------------

void TestWs3BackTransTimerPlan() {
    std::cout << "[RUN] Testing WS3 back-translation timer plan..." << std::endl;
    const int failures_before = g_failed_count;

    using TT = emebalachat::TooltipWindow;
    using Ev = TT::BackTransTimerEvent;
    using Act = TT::BackTransTimerAction;

    // Spec constants: the 4500 ms FLOOR (v0.10.9 U-B: no longer the fixed
    // delay) and the 1500 ms hover grace.
    volatile DWORD autohide = TT::kBackTransAutohideMs;
    volatile DWORD grace = TT::kBackTransHoverGraceMs;
    volatile DWORD autohide_max = TT::kBackTransAutohideMaxMs;
    TEST_CHECK(autohide == 4500, "WS3 timer: the autohide floor is 4500 ms");
    TEST_CHECK(grace == 1500, "WS3 timer: the hover grace window is 1500 ms");
    TEST_CHECK(autohide_max == 12000, "WS3 timer (v0.10.9): the autohide ceiling is 12000 ms");

    // ---- v0.10.9 (U-B, defect 3-5c): the adaptive content ramp -------------
    volatile std::size_t free_chars = TT::kBackTransAutohideFreeChars;
    volatile DWORD per_char = TT::kBackTransAutohideMsPerChar;
    TEST_CHECK(free_chars == 120 && per_char == 10,
               "WS3 timer (v0.10.9): the ramp is 10 ms/char beyond 120 free characters");
    TEST_CHECK(TT::BackTransAutohideMsForContent(0) == 4500 &&
                   TT::BackTransAutohideMsForContent(1) == 4500 &&
                   TT::BackTransAutohideMsForContent(free_chars) == 4500,
               "WS3 timer (v0.10.9): a short card keeps the 4500 ms floor");
    TEST_CHECK(TT::BackTransAutohideMsForContent(free_chars + 1) == 4510,
               "WS3 timer (v0.10.9): the first character past the free band adds 10 ms");
    TEST_CHECK(TT::BackTransAutohideMsForContent(400) == 4500 + 280 * 10,
               "WS3 timer (v0.10.9): a mid-length card scales linearly");
    TEST_CHECK(TT::BackTransAutohideMsForContent(870) == 12000,
               "WS3 timer (v0.10.9): the ramp saturates at 870 displayed characters");
    TEST_CHECK(TT::BackTransAutohideMsForContent(1000000) == 12000 &&
                   TT::BackTransAutohideMsForContent(static_cast<std::size_t>(-1)) == 12000,
               "WS3 timer (v0.10.9): the delay saturates and never overflows");
    // Monotonic non-decreasing across the whole ramp.
    bool ramp_monotonic = true;
    DWORD prev_ms = TT::BackTransAutohideMsForContent(0);
    for (std::size_t c = 1; c <= 1200; ++c) {
        const DWORD ms = TT::BackTransAutohideMsForContent(c);
        if (ms < prev_ms) ramp_monotonic = false;
        prev_ms = ms;
    }
    TEST_CHECK(ramp_monotonic, "WS3 timer (v0.10.9): the delay is monotonic in the content length");

    // Shown -> arm the autohide. The DEFAULT delay is the short-card floor, so
    // every pre-v0.10.9 call shape keeps its timing.
    const TT::BackTransTimerPlan shown = TT::PlanBackTransTimer(Ev::Shown, false);
    TEST_CHECK(shown.action == Act::ArmAutohide, "WS3 timer: showing the card arms the autohide");
    TEST_CHECK(shown.delay_ms == 4500, "WS3 timer: the default armed delay is the 4500 ms floor");
    // The adaptive delay flows through the planner (the live show path passes
    // BackTransAutohideMsForContent(displayed chars)).
    const TT::BackTransTimerPlan adaptive = TT::PlanBackTransTimer(Ev::Shown, false, 9000);
    TEST_CHECK(adaptive.action == Act::ArmAutohide && adaptive.delay_ms == 9000,
               "WS3 timer (v0.10.9): a caller-supplied adaptive delay is armed verbatim");
    TEST_CHECK(TT::PlanBackTransTimer(Ev::Shown, true, 12000).delay_ms == 12000,
               "WS3 timer (v0.10.9): the ceiling delay is armed verbatim too");

    // First pointer move over the card -> pause.
    const TT::BackTransTimerPlan move_first = TT::PlanBackTransTimer(Ev::PointerMoved, false);
    TEST_CHECK(move_first.action == Act::PauseAutohide,
               "WS3 timer: the first pointer move pauses the countdown");
    // Repeated moves while paused -> nothing (no per-pixel timer churn).
    const TT::BackTransTimerPlan move_again = TT::PlanBackTransTimer(Ev::PointerMoved, true);
    TEST_CHECK(move_again.action == Act::None,
               "WS3 timer: repeated pointer moves are a no-op while paused");

    // Leave -> grace, ALWAYS 1500 ms (a hover past the autohide window must not
    // extend the card indefinitely).
    const TT::BackTransTimerPlan leave_paused = TT::PlanBackTransTimer(Ev::PointerLeft, true);
    TEST_CHECK(leave_paused.action == Act::ArmGrace,
               "WS3 timer: leaving the card arms the grace window");
    TEST_CHECK(leave_paused.delay_ms == 1500, "WS3 timer: the grace delay is 1500 ms");
    const TT::BackTransTimerPlan leave_unpaused = TT::PlanBackTransTimer(Ev::PointerLeft, false);
    TEST_CHECK(leave_unpaused.action == Act::ArmGrace && leave_unpaused.delay_ms == 1500,
               "WS3 timer: the grace window is the same whether or not a pause was recorded");

    // Explicit dismissal -> kill.
    const TT::BackTransTimerPlan dismissed = TT::PlanBackTransTimer(Ev::Dismissed, true);
    TEST_CHECK(dismissed.action == Act::KillTimers,
               "WS3 timer: an explicit dismissal kills the timer");

    // Re-entry during the grace window must pause again. The planner sees a
    // "pointer moved" event; the pause flag is released when the grace timer
    // is armed, so the re-entry reaches PauseAutohide rather than None.
    // (The flag reset itself lives in ApplyBackTransTimerPlan and is pinned
    // structurally below.)
    const TT::BackTransTimerPlan reenter = TT::PlanBackTransTimer(Ev::PointerMoved, false);
    TEST_CHECK(reenter.action == Act::PauseAutohide,
               "WS3 timer: a re-entry after the grace arm pauses again");

    // The grace window is strictly shorter than even the FLOOR, so a hover
    // cycle always ends the card sooner than a fresh show would.
    TEST_CHECK(grace < autohide,
               "WS3 timer: the grace window is shorter than the autohide floor");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] WS3 back-translation timer plan passed." << std::endl;
    } else {
        std::cout << "[FAIL] WS3 back-translation timer plan: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (c) placement math ---------------------------------------------------

void TestWs3BackTransCardPlacement() {
    std::cout << "[RUN] Testing WS3 back-translation card placement..." << std::endl;
    const int failures_before = g_failed_count;

    using TT = emebalachat::TooltipWindow;
    const RECT work = {0, 0, 1920, 1080};

    // Centred on the anchor, gap + height above it.
    {
        const RECT anchor = {500, 400, 502, 420};
        const POINT p = TT::PlaceBackTransCard(anchor, true, 12, 360, 120, work);
        TEST_CHECK(p.x == 321, "WS3 place: the card is centred on the anchor");
        TEST_CHECK(p.y == 268, "WS3 place: the card sits gap+height above the anchor");
    }

    // A wide anchor is still centred on its midpoint.
    {
        const RECT anchor = {400, 400, 800, 420};
        const POINT p = TT::PlaceBackTransCard(anchor, true, 12, 360, 120, work);
        TEST_CHECK(p.x == 420, "WS3 place: a wide anchor centres on its midpoint");
    }

    // No room above -> flip below the anchor.
    {
        const RECT anchor = {500, 10, 502, 30};
        const POINT p = TT::PlaceBackTransCard(anchor, true, 12, 360, 120, work);
        TEST_CHECK(p.y == 42, "WS3 place: no room above flips the card below");
        TEST_CHECK(p.y >= static_cast<int>(anchor.bottom),
                   "WS3 place: the flipped card clears the anchor");
    }

    // No room below (below mode) -> mirror flip above.
    {
        const RECT anchor = {500, 1000, 502, 1020};
        const POINT p = TT::PlaceBackTransCard(anchor, false, 12, 360, 120, work);
        TEST_CHECK(p.y + 120 <= static_cast<int>(anchor.top),
                   "WS3 place: no room below flips the card above");
    }

    // The shared 10 px edge clamp applies on every side.
    {
        const RECT anchor = {2, 400, 4, 420};
        const POINT p = TT::PlaceBackTransCard(anchor, true, 12, 360, 120, work);
        TEST_CHECK(p.x >= static_cast<int>(work.left) + 10,
                   "WS3 place: the left edge clamp holds");
        const RECT right_anchor = {1910, 400, 1912, 420};
        const POINT pr = TT::PlaceBackTransCard(right_anchor, true, 12, 360, 120, work);
        TEST_CHECK(pr.x + 360 <= static_cast<int>(work.right) - 10,
                   "WS3 place: the right edge clamp holds");
    }

    // A secondary monitor with negative virtual coordinates still clamps into
    // its own work area (the multi-monitor contract ClampWindowOrigin keeps).
    {
        const RECT left_work = {-1920, 0, 0, 1080};
        const RECT anchor = {-1000, 400, -998, 420};
        const POINT p = TT::PlaceBackTransCard(anchor, true, 12, 360, 120, left_work);
        TEST_CHECK(p.x >= -1920 + 10 && p.x + 360 <= 0 - 10,
                   "WS3 place: a negative-coordinate work area clamps correctly");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] WS3 back-translation card placement passed." << std::endl;
    } else {
        std::cout << "[FAIL] WS3 back-translation card placement: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (d) label formatting -------------------------------------------------

void TestWs3BackTransLabels() {
    std::cout << "[RUN] Testing WS3 back-translation label formatting..." << std::endl;
    const int failures_before = g_failed_count;

    using TT = emebalachat::TooltipWindow;

    // Header: "<title> (SRC → TGT)". The spec's example is
    // "🔄 역번역 확인 (EN → KO)".
    {
        const std::wstring h = TT::FormatBackTransHeader(L"🔄 역번역 확인", L"EN", L"KO");
        TEST_CHECK(h == L"🔄 역번역 확인 (EN → KO)",
                   "WS3 label: the header carries the title and the SRC → TGT pair");
        TEST_CHECK(h.find(L" → ") != std::wstring::npos,
                   "WS3 label: the language pair uses the U+2192 arrow with spaces");
        TEST_CHECK(h.find(L"->") == std::wstring::npos,
                   "WS3 label: the ASCII arrow form is never used");
    }

    // Empty labels are dropped rather than rendered as "()".
    TEST_CHECK(TT::FormatBackTransHeader(L"T", L"", L"") == L"T",
               "WS3 label: both labels empty leaves the bare title");
    TEST_CHECK(TT::FormatBackTransHeader(L"T", L"EN", L"") == L"T (EN)",
               "WS3 label: a missing target drops the arrow");
    TEST_CHECK(TT::FormatBackTransHeader(L"T", L"", L"KO") == L"T (KO)",
               "WS3 label: a missing source drops the arrow");

    // ---- v0.10.9 (U-B, defect 3-5a): the original is ALWAYS shown ----------
    // The pre-v0.10.9 rule suppressed the reference line when AAAAA' == AAAAA.
    // That is exactly the round trip the user wants to verify, and the input
    // box no longer holds the original, so the policy is now "whenever there
    // is an original".
    TEST_CHECK(TT::BackTransNeedsReferenceLine(L"same", L"same"),
               "WS3 label (v0.10.9): an exact round trip STILL shows the original");
    TEST_CHECK(TT::BackTransNeedsReferenceLine(L"same", L"different"),
               "WS3 label (v0.10.9): a changed round trip shows the original");
    TEST_CHECK(!TT::BackTransNeedsReferenceLine(L"", L"anything"),
               "WS3 label (v0.10.9): an empty original has nothing to show");
    TEST_CHECK(!TT::BackTransNeedsReferenceLine(L"", L""),
               "WS3 label (v0.10.9): both sides empty shows nothing");
    TEST_CHECK(TT::BackTransNeedsReferenceLine(L"a", L"a "),
               "WS3 label (v0.10.9): a whitespace-only difference shows the original");
    // The policy ignores back_text entirely (documented, and the reason the
    // exact-round-trip case above flipped from false to true).
    TEST_CHECK(TT::BackTransNeedsReferenceLine(L"x", L"y") ==
                   TT::BackTransNeedsReferenceLine(L"x", L"x"),
               "WS3 label (v0.10.9): the decision depends only on the original");

    // ---- v0.10.9 (U-B): the two SECTION headers come from the i18n table ----
    // Contract C5 owns the literals (another unit authors them); this suite
    // only proves the ids exist, resolve non-empty in the shipped tables, and
    // are stable across a locale switch - a missing entry would render a
    // caption-less panel.
    {
        const UiLocale initial_locale = I18n::GetCurrentLocale();
        I18n::SetLocale(UiLocale::Korean);
        const std::wstring back_ko = I18n::Get(StringId::BackTransSectionBack);
        const std::wstring orig_ko = I18n::Get(StringId::BackTransSectionOriginal);
        TEST_CHECK(!back_ko.empty(),
                   "WS3 label (v0.10.9): StringId::BackTransSectionBack resolves (ko)");
        TEST_CHECK(!orig_ko.empty(),
                   "WS3 label (v0.10.9): StringId::BackTransSectionOriginal resolves (ko)");
        TEST_CHECK(back_ko != orig_ko,
                   "WS3 label (v0.10.9): the two section headers are distinct strings");
        // Fix round 1 (F-5): the C5 strings carry NO brackets. The renderer
        // comment used to claim BackTransSectionBack IS the literal "[역번역]";
        // the bracketed forms in the comments are shorthand for the SECTION.
        // Pinning the invariant (not the comment) keeps a future table edit from
        // silently disagreeing with the drawing code's documentation.
        TEST_CHECK(back_ko.front() != L'[' && back_ko.back() != L']' &&
                       orig_ko.front() != L'[' && orig_ko.back() != L']',
                   "WS3 label (F-5): the C5 section headers carry no brackets");
        I18n::SetLocale(UiLocale::English);
        TEST_CHECK(!I18n::Get(StringId::BackTransSectionBack).empty() &&
                       !I18n::Get(StringId::BackTransSectionOriginal).empty(),
                   "WS3 label (v0.10.9): both section headers resolve in English too");
        I18n::SetLocale(initial_locale);
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] WS3 back-translation label formatting passed." << std::endl;
    } else {
        std::cout << "[FAIL] WS3 back-translation label formatting: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (e) live marshal seam ------------------------------------------------

void TestWs3BackTransMarshalSeam() {
    std::cout << "[RUN] Testing WS3 back-translation marshal seam..." << std::endl;
    const int failures_before = g_failed_count;

    using TT = emebalachat::TooltipWindow;
    const HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    // 261108_0002 (FIX-WS3B): the reference line is localized now, so this
    // suite pins the Korean form against an explicitly-set locale instead of
    // the host machine's UI locale.
    const UiLocale initial = I18n::GetCurrentLocale();
    I18n::SetLocale(UiLocale::Korean);

    TT tooltip;
    if (!tooltip.Create(hInst)) {
        std::cout << "[SKIP] WS3 marshal: TooltipWindow could not be created." << std::endl;
        return;
    }

    // Same-thread call runs inline (no deadlock, model updated).
    tooltip.ShowBackTranslationThreadSafe(L"안녕하세요", L"Hello", L"KO", L"EN",
                                          false, nullptr, nullptr);
    TEST_CHECK(tooltip.IsVisible(), "WS3 marshal: the card is visible after a show");
    TEST_CHECK(tooltip.IsBackTranslationMode(), "WS3 marshal: the card reports back-translation mode");
    TEST_CHECK(tooltip.GetBackTransText() == L"Hello",
               "WS3 marshal: the body carries the back-translated text");
    TEST_CHECK(tooltip.IsBackTransMatched() == false,
               "WS3 marshal: the caller's pill verdict is stored verbatim");
    TEST_CHECK(tooltip.GetBackTransHeader().find(L"KO") != std::wstring::npos &&
                   tooltip.GetBackTransHeader().find(L"EN") != std::wstring::npos,
               "WS3 marshal: the header carries both language labels");
    // v0.10.9 (U-B): the panel body is the RAW original (the panel's own
    // section header supplies the "[원문]" label).
    TEST_CHECK(tooltip.GetBackTransReferenceLine() == L"안녕하세요",
               "WS3 marshal: the [원문] panel body carries the original text");
    // v0.10.9 (U-B, defect 3-1): scrollability is COMPUTED from the measured
    // content - a one-line show must not scroll, and its shared scroll state
    // must be zeroed.
    TEST_CHECK(!tooltip.IsScrollableForTest(),
               "WS3 marshal (v0.10.9): a fitting card is not scrollable");
    TEST_CHECK(tooltip.ScrollOffsetForTest() == 0.0f,
               "WS3 marshal (v0.10.9): a fresh card always starts unscrolled");
    TEST_CHECK(tooltip.GetBackTransPanelPlanForTest().scrollable == false &&
                   tooltip.GetBackTransPanelPlanForTest().back_view_h > 0,
               "WS3 marshal (v0.10.9): the measured panel plan has a real body viewport");
    // v0.10.9 (U-B, defect 3-5c): the autohide window is the adaptive one.
    TEST_CHECK(tooltip.GetBackTransAutohideMsForTest() ==
                   TT::BackTransAutohideMsForContent(std::wstring(L"안녕하세요").size() +
                                                     std::wstring(L"Hello").size()),
               "WS3 marshal (v0.10.9): the adaptive autohide delay is measured from the content");
    TEST_CHECK(tooltip.GetBackTransAutohideMsForTest() >= TT::kBackTransAutohideMs &&
                   tooltip.GetBackTransAutohideMsForTest() <= TT::kBackTransAutohideMaxMs,
               "WS3 marshal (v0.10.9): the delay stays inside [4500, 12000] ms");

    // ---- v0.10.9 (U-B): a LONG back-translation is scrollable, measured -----
    // 20 wrapped lines of body + a 3-line original: the exact case the CEO
    // reported as clipped-and-unscrollable.
    {
        std::wstring long_back;
        for (int i = 0; i < 20; ++i) {
            long_back += L"This is a fairly long back translated line number ";
            long_back += std::to_wstring(i + 1);
            long_back += L" of twenty.\n";
        }
        const std::wstring short_original = L"짧은 원문 첫 줄\n둘째 줄\n셋째 줄";
        tooltip.ShowBackTranslationThreadSafe(short_original, long_back, L"KO", L"EN",
                                              false, nullptr, nullptr);
        TEST_CHECK(tooltip.IsBackTranslationMode(), "WS3 marshal: the long fixture shows");
        TEST_CHECK(tooltip.IsScrollableForTest(),
                   "WS3 marshal (v0.10.9): a long back-translation enables scrolling");
        const TT::BackTransPanelPlan plan = tooltip.GetBackTransPanelPlanForTest();
        TEST_CHECK(plan.scrollable && plan.card_height_dip == TT::kBackTransCardMaxHeightDip,
                   "WS3 marshal (v0.10.9): the long card grows to the 480 DIP cap");
        TEST_CHECK(plan.back_view_h >= TT::kBackTransMinPanelBodyDip &&
                       plan.ref_view_h >= TT::kBackTransMinPanelBodyDip,
                   "WS3 marshal (v0.10.9): both panels keep a usable viewport");
        TEST_CHECK(plan.ref_view_h == plan.ref_content_h,
                   "WS3 marshal (H-1): the measured 3-line original is fully visible");
        TEST_CHECK(plan.ref_view_top + plan.ref_view_h + TT::kBackTransPanelBottomPadDip +
                           TT::kBackTransCardBottomPadDip == plan.card_height_dip,
                   "WS3 marshal (v0.10.9): the measured plan fits the card exactly");
        TEST_CHECK(tooltip.ContentHeightForTest() > 0.0f,
                   "WS3 marshal (v0.10.9): the shared content extent is populated");
        // A wheel notch now moves the shared offset (defect 3-1: it used to be
        // dropped because scrollable_ was hardcoded false).
        tooltip.ScrollByDipWheel(-WHEEL_DELTA);
        TEST_CHECK(tooltip.ScrollOffsetForTest() > 0.0f,
                   "WS3 marshal (v0.10.9): a wheel notch scrolls the comparison card");
        // ... and back to the top, clamped at 0 (no negative offset).
        for (int i = 0; i < 200 && tooltip.ScrollOffsetForTest() > 0.0f; ++i) {
            tooltip.ScrollByDipWheel(WHEEL_DELTA);
        }
        TEST_CHECK(tooltip.ScrollOffsetForTest() == 0.0f,
                   "WS3 marshal (v0.10.9): scrolling up clamps at zero");
    }

    // An exact round trip still shows the original (defect 3-5a).
    tooltip.ShowBackTranslationThreadSafe(L"Hello", L"Hello", L"EN", L"KO",
                                          true, nullptr, nullptr);
    TEST_CHECK(tooltip.IsBackTransMatched(), "WS3 marshal: the matched verdict reaches the model");
    TEST_CHECK(tooltip.GetBackTransReferenceLine() == L"Hello",
               "WS3 marshal (v0.10.9): an exact round trip STILL shows the original");

    // Off-thread call marshals through the value queue.
    {
        std::thread producer([&tooltip]() {
            tooltip.ShowBackTranslationThreadSafe(L"원문", L"source text", L"KO", L"EN",
                                                  true, nullptr, nullptr);
        });
        producer.join();
        // Unpumped: the payload is queued, the model is untouched.
        TEST_CHECK(tooltip.GetBackTransText() == L"Hello",
                   "WS3 marshal: an unpumped off-thread show leaves the model alone");
        PumpThreadMessagesOnce();
        TEST_CHECK(tooltip.GetBackTransText() == L"source text",
                   "WS3 marshal: the WndProc drained the queue and applied the payload");
        TEST_CHECK(tooltip.IsBackTransMatched(),
                   "WS3 marshal: the marshaled pill verdict survives the queue");
        TEST_CHECK(tooltip.GetBackTransReferenceLine() == L"원문",
                   "WS3 marshal: the marshaled original text survives the queue");
    }

    // DismissThreadSafe clears the card AND discards still-queued payloads, so
    // a show enqueued a moment before the dismissal cannot resurrect it.
    {
        std::thread producer([&tooltip]() {
            tooltip.ShowBackTranslationThreadSafe(L"부활", L"resurrect", L"KO", L"EN",
                                                  false, nullptr, nullptr);
        });
        producer.join();
        tooltip.DismissThreadSafe();
        PumpThreadMessagesOnce();
        TEST_CHECK(!tooltip.IsVisible(), "WS3 marshal: DismissThreadSafe hides the card");
        TEST_CHECK(!tooltip.IsBackTranslationMode(),
                   "WS3 marshal: DismissThreadSafe leaves back-translation mode");
        TEST_CHECK(tooltip.GetBackTransText().empty(),
                   "WS3 marshal: DismissThreadSafe clears the body");
        TEST_CHECK(tooltip.GetBackTransReferenceLine().empty(),
                   "WS3 marshal: DismissThreadSafe clears the reference line");
        TEST_CHECK(tooltip.GetBackTransText() != L"resurrect",
                   "WS3 marshal: the queued show was discarded, not applied");
    }

    // DrainMarshalQueue counts the back-translation payload like the others.
    {
        TT::BackTranslationPayload p;
        p.original_text = L"drain";
        p.back_text = L"drained";
        p.source_lang_label = L"KO";
        p.target_lang_label = L"EN";
        p.meaning_matched = true;
        TEST_CHECK(TT::PostPayloadForTest(tooltip.GetHwnd(), TT::kShowBackTranslationMessage, &p),
                   "WS3 marshal: the back-translation payload queues through the test seam");
        TEST_CHECK(tooltip.DrainMarshalQueue() == 1,
                   "WS3 marshal: DrainMarshalQueue counts the back-translation payload");
        PumpThreadMessagesOnce();
        TEST_CHECK(!tooltip.IsBackTranslationMode(),
                   "WS3 marshal: a drained payload never reaches the model");
    }

    // The next translation dismisses the card (the spec's "next translation"
    // rule) and restores the 360 DIP translation width.
    {
        tooltip.ShowBackTranslationThreadSafe(L"a", L"b", L"KO", L"EN", false, nullptr, nullptr);
        TEST_CHECK(tooltip.IsBackTranslationMode(), "WS3 marshal: fixture shows the card again");
        tooltip.ShowTranslation(200, 200, L"src", "KO", "English", L"translated");
        TEST_CHECK(!tooltip.IsBackTranslationMode(),
                   "WS3 marshal: the next translation dismisses the back-translation card");
        TEST_CHECK(tooltip.GetTranslatedText() == L"translated",
                   "WS3 marshal: the translation card takes the window back over");
        TEST_CHECK(tooltip.GetBackTransText().empty(),
                   "WS3 marshal: the dismissed card left no body behind");
    }

    tooltip.Destroy();
    I18n::SetLocale(initial); // suite-state hygiene

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] WS3 back-translation marshal seam passed." << std::endl;
    } else {
        std::cout << "[FAIL] WS3 back-translation marshal seam: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (f) structural pins --------------------------------------------------

void TestWs3BackTransStructuralPins() {
    std::cout << "[RUN] Testing WS3 back-translation structural pins..." << std::endl;
    const int failures_before = g_failed_count;

    const std::string cpp = Ws3TipReadSrc("src/ui/tooltip.cpp");
    const std::string hpp = Ws3TipReadSrc("src/ui/tooltip.hpp");
    const std::string worker = Ws3TipReadSrc("src/worker.cpp");
    if (cpp.empty() || hpp.empty()) {
        std::cout << "[SKIP] src/ui/tooltip.{cpp,hpp} not resolvable; WS3 pins skipped."
                  << std::endl;
        return;
    }

    // ---- the API contract the main.cpp unit is written against ----
    TEST_CHECK(hpp.find("void ShowBackTranslation(const std::wstring& original_text,") !=
                   std::string::npos,
               "WS3 pin: ShowBackTranslation is declared with the contracted parameter order");
    TEST_CHECK(hpp.find("void ShowBackTranslationThreadSafe(const std::wstring& original_text,") !=
                   std::string::npos,
               "WS3 pin: ShowBackTranslationThreadSafe is declared");
    TEST_CHECK(hpp.find("void DismissThreadSafe();") != std::string::npos,
               "WS3 pin: DismissThreadSafe (pre-existing) is still declared");
    TEST_CHECK(cpp.find("void TooltipWindow::ShowBackTranslation(const std::wstring& original_text,") !=
                   std::string::npos,
               "WS3 pin: ShowBackTranslation is defined with the contracted signature");
    TEST_CHECK(cpp.find("void TooltipWindow::ShowBackTranslationThreadSafe(") !=
                   std::string::npos,
               "WS3 pin: ShowBackTranslationThreadSafe is defined");
    // Argument order: original, back, source label, target label, matched,
    // target hwnd, badge hwnd.
    {
        const size_t def = cpp.find("void TooltipWindow::ShowBackTranslationThreadSafe(");
        const std::string sig = cpp.substr(def, 700);
        const size_t a_orig = sig.find("original_text");
        const size_t a_back = sig.find("back_text");
        const size_t a_src = sig.find("source_lang_label");
        const size_t a_tgt = sig.find("target_lang_label");
        const size_t a_match = sig.find("meaning_matched");
        const size_t a_thwnd = sig.find("target_hwnd");
        const size_t a_bhwnd = sig.find("badge_hwnd");
        TEST_CHECK(a_orig != std::string::npos && a_back != std::string::npos &&
                       a_src != std::string::npos && a_tgt != std::string::npos &&
                       a_match != std::string::npos && a_thwnd != std::string::npos &&
                       a_bhwnd != std::string::npos &&
                       a_orig < a_back && a_back < a_src && a_src < a_tgt &&
                       a_tgt < a_match && a_match < a_thwnd && a_thwnd < a_bhwnd,
                   "WS3 pin: the parameter order matches the contract exactly");
    }

    // ---- message id + timer id ----
    TEST_CHECK(hpp.find("static constexpr UINT kShowBackTranslationMessage = WM_APP + 0x206;") !=
                   std::string::npos,
               "WS3 pin: kShowBackTranslationMessage is WM_APP + 0x206");
    TEST_CHECK(cpp.find("case kShowBackTranslationMessage: {") != std::string::npos,
               "WS3 pin: the WndProc handles kShowBackTranslationMessage");
    TEST_CHECK(hpp.find("static constexpr UINT_PTR kTimerBackTranslateAutohide = 4003;") !=
                   std::string::npos,
               "WS3 pin: kTimerBackTranslateAutohide is timer id 4003");
    TEST_CHECK(cpp.find("if (wParam == kTimerBackTranslateAutohide) {") != std::string::npos,
               "WS3 pin: the WM_TIMER branch consumes kTimerBackTranslateAutohide");
    TEST_CHECK(hpp.find("static constexpr DWORD kBackTransAutohideMs = 4500;") !=
                   std::string::npos,
               "WS3 pin: the autohide floor is 4500 ms");
    TEST_CHECK(hpp.find("static constexpr DWORD kBackTransHoverGraceMs = 1500;") !=
                   std::string::npos,
               "WS3 pin: the hover grace window is 1500 ms");
    // v0.10.9 (U-B, defect 3-5c): the adaptive ramp's constants + helper.
    TEST_CHECK(hpp.find("static constexpr DWORD kBackTransAutohideMaxMs = 12000;") !=
                   std::string::npos,
               "WS3 pin (v0.10.9): the adaptive autohide ceiling is 12000 ms");
    TEST_CHECK(hpp.find("static constexpr DWORD BackTransAutohideMsForContent(std::size_t displayed_chars)") !=
                   std::string::npos,
               "WS3 pin (v0.10.9): the content-scaled autohide helper exists");
    TEST_CHECK(cpp.find("backtrans_autohide_ms_ = BackTransAutohideMsForContent(") !=
                   std::string::npos,
               "WS3 pin (v0.10.9): the show path arms the adaptive delay");
    {
        const std::string apply = Ws3TipFuncBody(cpp, "void TooltipWindow::ApplyBackTransTimerPlan(");
        TEST_CHECK(apply.find("backtrans_autohide_ms_") != std::string::npos,
                   "WS3 pin (v0.10.9): the planner receives the adaptive delay");
    }
    // Wheel/drag re-arm the countdown (defect 3-5c).
    {
        const std::string wheel = Ws3TipFuncBody(cpp, "void TooltipWindow::ScrollByDipWheel(");
        TEST_CHECK(wheel.find("RearmBackTransAutohideForScroll();") != std::string::npos,
                   "WS3 pin (v0.10.9): a wheel scroll re-arms the autohide countdown");
        TEST_CHECK(wheel.find("ActiveScrollViewportDip()") != std::string::npos,
                   "WS3 pin (v0.10.9): the wheel clamps against the active mode's viewport");
        const std::string drag = Ws3TipFuncBody(cpp, "bool TooltipWindow::UpdateScrollThumbDrag(");
        TEST_CHECK(!drag.empty() && drag.find("RearmBackTransAutohideForScroll();") !=
                                        std::string::npos,
                   "WS3 pin (v0.10.9): a thumb drag re-arms the autohide countdown");
        const std::string rearm =
            Ws3TipFuncBody(cpp, "void TooltipWindow::RearmBackTransAutohideForScroll(");
        TEST_CHECK(!rearm.empty() && rearm.find("backtrans_hover_paused_") != std::string::npos,
                   "WS3 pin (v0.10.9): the re-arm respects the hover pause");
    }

    // ---- v0.10.9 (U-B, defect 3-1): scrollable is COMPUTED, never hardcoded --
    {
        const std::string show = Ws3TipFuncBody(cpp, "void TooltipWindow::ShowBackTranslation(");
        TEST_CHECK(!show.empty(), "WS3 pin: ShowBackTranslation body is locatable");
        TEST_CHECK(show.find("scrollable_ = false;") == std::string::npos,
                   "WS3 pin (v0.10.9): ShowBackTranslation no longer hardcodes scrollable_ = false");
        TEST_CHECK(show.find("scrollable_ = panel_plan.scrollable;") != std::string::npos,
                   "WS3 pin (v0.10.9): scrollability comes from the measured panel plan");
        TEST_CHECK(show.find("BackTransPanelPlan panel_plan = measure_panels(") != std::string::npos,
                   "WS3 pin (v0.10.9): both panels are measured before layout");
        TEST_CHECK(show.find("MeasureBackTransTextHeight(") != std::string::npos,
                   "WS3 pin (v0.10.9): the card measures its text with DirectWrite");
        TEST_CHECK(show.find("BackTransAutohideMsForContent(") != std::string::npos,
                   "WS3 pin (v0.10.9): the adaptive delay is computed in the show path");
        // The measured line height drives the wheel step.
        TEST_CHECK(show.find("line_height_dip_ = measured_line_height_dip") != std::string::npos,
                   "WS3 pin (v0.10.9): the wheel step uses the measured line height");
        // The original is always stored (defect 3-5a) and the pre-v0.10.9
        // inline formatter is gone - the [원문] panel has its own section header.
        TEST_CHECK(show.find("? original_text") != std::string::npos,
                   "WS3 pin (v0.10.9): the [원문] body is the raw original text");
        TEST_CHECK(show.find("FormatBackTransReference(original_text)") == std::string::npos,
                   "WS3 pin (v0.10.9): the inline reference formatter is no longer called");
    }

    // ---- v0.10.9 (U-B, defects 3-3/3-4): per-panel clipping in the renderer --
    {
        const std::string render = Ws3TipFuncBody(cpp, "void TooltipWindow::RenderBackTranslation() {");
        TEST_CHECK(!render.empty(), "WS3 pin: RenderBackTranslation body is locatable");
        // TWO clip windows, one per panel - the defect was ZERO.
        TEST_CHECK(Ws3TipCount(render, "PushAxisAlignedClip(") >= 2,
                   "WS3 pin (v0.10.9): both panels draw inside their own clip window");
        TEST_CHECK(Ws3TipCount(render, "PushAxisAlignedClip(") ==
                       Ws3TipCount(render, "PopAxisAlignedClip();"),
                   "WS3 pin (v0.10.9): every clip window is popped (balanced calls)");
        TEST_CHECK(render.find("StringId::BackTransSectionBack") != std::string::npos &&
                       render.find("StringId::BackTransSectionOriginal") != std::string::npos,
                   "WS3 pin (v0.10.9): both section headers come from the i18n table");
        TEST_CHECK(render.find("backtrans_section_format_") != std::string::npos,
                   "WS3 pin (v0.10.9): the section headers use their own format");
        TEST_CHECK(render.find("ClampScrollOffset(scroll_offset_dip_") != std::string::npos,
                   "WS3 pin (v0.10.9): each panel clamps the shared offset to its own extent");
        TEST_CHECK(render.find("panels.ref_view_top") != std::string::npos &&
                       render.find("panels.back_view_top") != std::string::npos,
                   "WS3 pin (v0.10.9): the render reads the measured panel geometry");
        // One shared scrollbar for the one shared offset.
        TEST_CHECK(render.find("ScrollbarThumbHeightDip(") != std::string::npos &&
                       render.find("ScrollbarThumbTopDip(") != std::string::npos,
                   "WS3 pin (v0.10.9): the card draws its own shared scrollbar thumb");
        // No fixed one-line original box survives (defect 3-4).
        TEST_CHECK(render.find("current_height_) - 26.0f") == std::string::npos,
                   "WS3 pin (v0.10.9): the fixed 16 DIP original box is gone");
    }
    // Header/body/reference/scrollbar styling pins kept from v0.10.6.
    {
        const std::string render = Ws3TipFuncBody(cpp, "void TooltipWindow::RenderBackTranslation() {");
        TEST_CHECK(render.find("theme.state_ready") != std::string::npos,
                   "WS3 pin: the ✓ pill uses the theme's ready/green token");
        TEST_CHECK(render.find("theme.accent_primary") != std::string::npos,
                   "WS3 pin: the ⚡ pill uses the theme's accent token");
        TEST_CHECK(render.find("StringId::BackTranslateMatchExact") != std::string::npos &&
                       render.find("StringId::BackTranslateMatchCheck") != std::string::npos,
                   "WS3 pin: both pill labels come from the i18n table");
        TEST_CHECK(render.find("backtrans_body_format_") != std::string::npos &&
                       render.find("backtrans_ref_format_") != std::string::npos,
                   "WS3 pin: the body and the original use their own formats");
        TEST_CHECK(render.find("theme.text_secondary") != std::string::npos,
                   "WS3 pin (v0.10.9): the [원문] body is secondary text, the body is primary");
    }

    // ---- the second value queue (SEC-ADJ by-value transport) ----
    TEST_CHECK(cpp.find("std::mutex g_tt_backtrans_mu;") != std::string::npos &&
                   cpp.find("std::deque<TooltipWindow::BackTranslationPayload> g_tt_backtrans_queue;") !=
                       std::string::npos,
               "WS3 pin: the back-translation value queue exists with its own mutex");
    TEST_CHECK(cpp.find("::PostMessageW(hwnd_, kShowBackTranslationMessage, 0, 0)") !=
                   std::string::npos,
               "WS3 pin: the wake-up post carries no payload (0, 0)");
    TEST_CHECK(cpp.find("g_tt_backtrans_queue.push_back(") != std::string::npos &&
                   cpp.find("g_tt_backtrans_queue.pop_front();") != std::string::npos,
               "WS3 pin: the queue is pushed by the producer and popped by the WndProc");
    TEST_CHECK(cpp.find("g_tt_backtrans_queue.clear();") != std::string::npos,
               "WS3 pin: the queue is cleared on teardown / dismissal");

    // ---- width reset + dismiss-on-new-translation in ShowTranslation ----
    {
        const std::string show = Ws3TipFuncBody(cpp, "void TooltipWindow::ShowTranslation(");
        TEST_CHECK(!show.empty(), "WS3 pin: ShowTranslation body is locatable");
        TEST_CHECK(show.find("current_width_ = kBackTransCardWidthDip;") != std::string::npos,
                   "WS3 pin: ShowTranslation resets the card width to 360 DIP");
        TEST_CHECK(show.find("HideBackTranslationCard();") != std::string::npos,
                   "WS3 pin: ShowTranslation dismisses the back-translation card");
        // The reset must precede the body measurement (which reads the width).
        const size_t reset = show.find("current_width_ = kBackTransCardWidthDip;");
        const size_t measure = show.find("static_cast<float>(current_width_ - 28)");
        TEST_CHECK(reset != std::string::npos && measure != std::string::npos && reset < measure,
                   "WS3 pin: the width reset precedes the body measurement");
    }

    // ---- anchor tiers ----
    TEST_CHECK(cpp.find("POINT TooltipWindow::ResolveBackTranslationAnchor(") != std::string::npos,
               "WS3 pin: ResolveBackTranslationAnchor is defined");
    {
        const std::string anchor = Ws3TipFuncBody(cpp, "POINT TooltipWindow::ResolveBackTranslationAnchor(");
        TEST_CHECK(anchor.find("::GetGUIThreadInfo(tid, &gti)") != std::string::npos,
                   "WS3 pin: tier 1 reads the caret through GetGUIThreadInfo");
        TEST_CHECK(anchor.find("::ClientToScreen(gti.hwndCaret, &tl)") != std::string::npos,
                   "WS3 pin: tier 1 maps the caret rect through its own window");
        TEST_CHECK(anchor.find("gti.hwndFocus") != std::string::npos,
                   "WS3 pin: tier 1 falls back to the focus window");
        TEST_CHECK(anchor.find("emebalachat::ui::ScaleDipsToPixels(180, dpi_)") != std::string::npos,
                   "WS3 pin: the focus-window fallback is gated on the 180 DIP compact-control cap");
        TEST_CHECK(anchor.find("::IsWindowVisible(badge_hwnd)") != std::string::npos,
                   "WS3 pin: tier 2 requires a visible badge");
        TEST_CHECK(anchor.find("::GetCursorPos(&cursor)") != std::string::npos,
                   "WS3 pin: tier 3 falls back to the cursor");
        TEST_CHECK(anchor.find("PlaceBackTransCard(") != std::string::npos,
                   "WS3 pin: every tier funnels through the pure placement helper");
        TEST_CHECK(anchor.find("::MonitorFromPoint(") != std::string::npos,
                   "WS3 pin: the work area comes from the anchor's monitor");
    }

    // ---- hover pause / resume / ESC / close wiring ----
    {
        const std::string move = Ws3TipFuncBody(cpp, "case WM_MOUSEMOVE: {");
        TEST_CHECK(!move.empty(), "WS3 pin: the WM_MOUSEMOVE branch is locatable");
        TEST_CHECK(move.find("BackTransTimerEvent::PointerMoved") != std::string::npos,
                   "WS3 pin: WM_MOUSEMOVE feeds the pause planner");
        TEST_CHECK(move.find("TME_LEAVE") != std::string::npos,
                   "WS3 pin: the card requests WM_MOUSELEAVE tracking");
    }
    {
        const std::string leave = Ws3TipFuncBody(cpp, "case WM_MOUSELEAVE: {");
        TEST_CHECK(leave.find("BackTransTimerEvent::PointerLeft") != std::string::npos,
                   "WS3 pin: WM_MOUSELEAVE arms the grace window");
    }
    // The grace arm RELEASES the pause flag, so re-entering the card during
    // the grace window pauses the countdown again instead of letting the
    // 1500 ms timer run out under the pointer.
    {
        const std::string apply = Ws3TipFuncBody(cpp, "void TooltipWindow::ApplyBackTransTimerPlan(");
        TEST_CHECK(!apply.empty(), "WS3 pin: ApplyBackTransTimerPlan body is locatable");
        const size_t grace = apply.find("case BackTransTimerAction::ArmGrace:");
        const size_t reset = apply.find("backtrans_hover_paused_ = false;", grace);
        const size_t next_case = apply.find("case BackTransTimerAction::PauseAutohide:", grace);
        TEST_CHECK(grace != std::string::npos && reset != std::string::npos &&
                       next_case != std::string::npos && reset < next_case,
                   "WS3 pin: arming the grace window releases the hover pause flag");
    }
    TEST_CHECK(cpp.find("case WM_KEYDOWN: {") != std::string::npos &&
                   cpp.find("pThis->is_backtrans_mode_ && wParam == VK_ESCAPE") !=
                       std::string::npos,
               "WS3 pin: ESC dismisses the card");
    TEST_CHECK(cpp.find("if (IsPointInRect(pThis->backtrans_close_rect_, bx, by)) {") !=
                   std::string::npos,
               "WS3 pin: the ✕ button dismisses the card on click");
    TEST_CHECK(cpp.find("reason=close_button") != std::string::npos &&
                   cpp.find("reason=esc") != std::string::npos &&
                   cpp.find("reason=autohide") != std::string::npos,
               "WS3 pin: all three dismissal paths are DIAG-attributed");

    // ---- rendering conventions (format lifecycle) ----
    TEST_CHECK(cpp.find("void TooltipWindow::RenderBackTranslation() {") != std::string::npos,
               "WS3 pin: the card has its own render body");
    TEST_CHECK(cpp.find("StringId::BackTranslateHeader") != std::string::npos,
               "WS3 pin: the card header comes from the i18n table");
    // Every card format is created exactly once, in Create().
    TEST_CHECK(Ws3TipCount(cpp, "&backtrans_body_format_") == 1,
               "WS3 pin: the body format is created exactly once (Create)");
    TEST_CHECK(Ws3TipCount(cpp, "&backtrans_ref_format_") == 1,
               "WS3 pin: the [원문] format is created exactly once (Create)");
    TEST_CHECK(Ws3TipCount(cpp, "&backtrans_section_format_") == 1,
               "WS3 pin (v0.10.9): the section-header format is created exactly once (Create)");
    TEST_CHECK(cpp.find("DWRITE_FONT_WEIGHT_SEMI_BOLD,") != std::string::npos,
               "WS3 pin: the card body is SemiBold");
    TEST_CHECK(cpp.find("backtrans_body_format_->Release();") != std::string::npos &&
                   cpp.find("backtrans_ref_format_->Release();") != std::string::npos &&
                   cpp.find("backtrans_header_format_->Release();") != std::string::npos &&
                   cpp.find("backtrans_section_format_->Release();") != std::string::npos,
               "WS3 pin: every card format is released in Destroy (RAII-correct)");
    // The 11.5pt [원문] body the report asks for (11pt pre-v0.10.9).
    {
        const size_t ref_fmt = cpp.find("&backtrans_ref_format_");
        const std::string ctx = ref_fmt == std::string::npos
                                    ? std::string()
                                    : cpp.substr(ref_fmt > 400 ? ref_fmt - 400 : 0, 400);
        TEST_CHECK(ctx.find("11.5f,") != std::string::npos,
                   "WS3 pin (v0.10.9): the [원문] body format is 11.5pt");
    }

    // ---- v0.10.9 (U-B): wheel + drag scroll wiring in backtrans mode --------
    {
        const std::string down = Ws3TipFuncBody(cpp, "case WM_LBUTTONDOWN: {");
        TEST_CHECK(!down.empty(), "WS3 pin: the WM_LBUTTONDOWN branch is locatable");
        TEST_CHECK(down.find("BeginScrollThumbDrag(") != std::string::npos,
                   "WS3 pin (v0.10.9): a press on the shared thumb starts a drag");
        const std::string up = Ws3TipFuncBody(cpp, "case WM_LBUTTONUP: {");
        TEST_CHECK(!up.empty() && up.find("EndScrollThumbDrag();") != std::string::npos,
                   "WS3 pin (v0.10.9): the release ends the shared-thumb drag");
        const std::string move = Ws3TipFuncBody(cpp, "case WM_MOUSEMOVE: {");
        TEST_CHECK(!move.empty() &&
                       move.find("UpdateScrollThumbDrag(y)") != std::string::npos,
                   "WS3 pin (v0.10.9): moving with the thumb held scrolls the card");
        const std::string cursor_branch = Ws3TipFuncBody(cpp, "case WM_SETCURSOR: {");
        TEST_CHECK(!cursor_branch.empty() &&
                       cursor_branch.find("scrollbar_thumb_rect_") != std::string::npos,
                   "WS3 pin (v0.10.9): the hand cursor covers the shared thumb");
    }
    // The wheel delivery path is untouched (the LL hook posts kScrollMessage).
    TEST_CHECK(cpp.find("pThis->ScrollByDipWheel(static_cast<int>(lParam));") != std::string::npos,
               "WS3 pin: kScrollMessage still drives ScrollByDipWheel");

    // ---- v0.10.9 (U-B, defect 3-5b): worker.cpp sends the FULL original ----
    if (!worker.empty()) {
        const size_t req = worker.find("const BackTranslateRequest request{");
        TEST_CHECK(req != std::string::npos,
                   "WS3 pin (v0.10.9): the worker builds a BackTranslateRequest");
        if (req != std::string::npos) {
            const std::string ctor = worker.substr(req, 240);
            TEST_CHECK(ctor.find("{line, back_translate_forward_text,") != std::string::npos,
                       "WS3 pin (v0.10.9): original_text is the full captured line");
            TEST_CHECK(ctor.find("{engine_input,") == std::string::npos,
                       "WS3 pin (v0.10.9): the tail fragment is no longer the original");
        }
    } else {
        std::cout << "[SKIP] src/worker.cpp not resolvable; the request pin is skipped."
                  << std::endl;
    }

    // ---- the window style is the pre-existing non-activating layered popup ----
    TEST_CHECK(cpp.find("WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE") !=
                   std::string::npos,
               "WS3 pin: the card reuses the non-activating layered popup style");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] WS3 back-translation structural pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] WS3 back-translation structural pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (g) 261108_0002 (FIX-WS3B): header-row layout planner ----------------
// The shipped v0.10.6 card put the header and the verdict pill on one fixed
// 360 DIP row, so long-string locales (Italian "Verifica retrotraduzione" +
// "Controlla la sfumatura", German "Rückübersetzungs-Prüfung" + "Nuance
// prüfen") overlapped. The planner below wraps the pill to its own row; this
// suite pins the rule with the REAL i18n strings, not synthetic lengths.
void TestWs3BackTransHeaderRowPlan() {
    std::cout << "[RUN] Testing WS3 FIX-WS3B header-row plan..." << std::endl;
    const int failures_before = g_failed_count;

    using TT = emebalachat::TooltipWindow;
    using Layout = TT::BackTransHeaderLayout;
    const UiLocale initial = I18n::GetCurrentLocale();

    // ---- constants the planner depends on ----
    volatile float pad = TT::kBackTransPadDip;
    volatile float close_w = TT::kBackTransCloseW;
    volatile float close_right = TT::kBackTransCloseRightDip;
    volatile float close_gap = TT::kBackTransCloseGapDip;
    volatile float header_gap = TT::kBackTransHeaderGapDip;
    TEST_CHECK(pad == 14.0f && close_w == 20.0f && close_right == 12.0f &&
                   close_gap == 8.0f && header_gap == 8.0f,
               "FIX-WS3B: the header-row geometry constants are the documented values");
    // The close button's left edge on the 360 DIP card is 328 DIP.
    TEST_CHECK(360.0f - close_right - close_w == 328.0f,
               "FIX-WS3B: the close button's left edge is 328 DIP on the 360 DIP card");

    // ---- the three-way rule, on raw DIP widths ----
    {
        const TT::BackTransHeaderPlan single = TT::PlanBackTransHeaderRowDip(120.0f, 90.0f, 360.0f);
        TEST_CHECK(single.layout == Layout::SingleRow,
                   "FIX-WS3B: a short header + short pill stay on one row");
        TEST_CHECK(single.pill_row_h == 0.0f,
                   "FIX-WS3B: the single-row plan adds no height");
        TEST_CHECK(single.ellipsize == false,
                   "FIX-WS3B: the single-row plan never ellipsizes");

        const TT::BackTransHeaderPlan wrapped = TT::PlanBackTransHeaderRowDip(200.0f, 120.0f, 360.0f);
        TEST_CHECK(wrapped.layout == Layout::PillOnOwnRow,
                   "FIX-WS3B: a long header + long pill wrap the pill to its own row");
        TEST_CHECK(wrapped.pill_row_h > 0.0f,
                   "FIX-WS3B: the wrapped pill row grows the card");
        TEST_CHECK(wrapped.ellipsize == false,
                   "FIX-WS3B: the pill wraps BEFORE the header is ever clipped");

        const TT::BackTransHeaderPlan clipped = TT::PlanBackTransHeaderRowDip(400.0f, 120.0f, 360.0f);
        TEST_CHECK(clipped.layout == Layout::EllipsizeHeader,
                   "FIX-WS3B: only a header that cannot fit alone is ellipsized");
        TEST_CHECK(clipped.ellipsize == true,
                   "FIX-WS3B: the ellipsize flag is set in that case");
        TEST_CHECK(clipped.header_width_dip == wrapped.header_width_dip,
                   "FIX-WS3B: the ellipsized header uses the same own-row budget");
    }

    // ---- the pill is never clipped ----
    {
        const TT::BackTransHeaderPlan tiny = TT::PlanBackTransHeaderRowDip(120.0f, 10.0f, 360.0f);
        TEST_CHECK(tiny.pill_width_dip == TT::kBackTransPillMinWidthDip,
                   "FIX-WS3B: a sub-minimum pill measurement floors at 96 DIP");
        const TT::BackTransHeaderPlan huge = TT::PlanBackTransHeaderRowDip(120.0f, 900.0f, 360.0f);
        TEST_CHECK(huge.pill_width_dip == 360.0f - 2.0f * TT::kBackTransPadDip,
                   "FIX-WS3B: an oversized pill clamps to the content column");
        TEST_CHECK(huge.pill_width_dip <= 360.0f,
                   "FIX-WS3B: the pill box never exceeds the card width");
    }

    // ---- the header never reaches the close button ----
    {
        for (int hdr_cp = 0; hdr_cp <= 120; hdr_cp += 7) {
            for (int pill_cp = 0; pill_cp <= 60; pill_cp += 11) {
                const TT::BackTransHeaderPlan p =
                    TT::PlanBackTransHeaderRow(hdr_cp, pill_cp, 360);
                const float header_right = TT::kBackTransPadDip + p.header_width_dip;
                TEST_CHECK(header_right <= 328.0f - TT::kBackTransCloseGapDip,
                           "FIX-WS3B: the header budget never reaches the close button");
                TEST_CHECK(p.pill_width_dip > 0.0f,
                           "FIX-WS3B: the pill always has a positive width");
            }
        }
    }

    // ---- REAL locale strings: short locales stay single-row ----
    // The header the card actually renders is "<localized title> (KO → EN)",
    // so the measured input is the title plus the 9-character label suffix.
    const auto header_for = [](UiLocale loc) -> std::wstring {
        I18n::SetLocale(loc);
        return I18n::Get(StringId::BackTranslateHeader) + L" (KO → EN)";
    };
    const auto pill_for = [](UiLocale loc) -> std::wstring {
        I18n::SetLocale(loc);
        return I18n::Get(StringId::BackTranslateMatchCheck);
    };

    struct LocaleCase {
        UiLocale locale;
        const char* code;
        bool expect_single_row;
    };
    const LocaleCase kCases[] = {
        {UiLocale::Korean,             "ko",    true},
        {UiLocale::Japanese,           "ja",    true},
        {UiLocale::ChineseSimplified,  "zh-CN", true},
        {UiLocale::English,            "en",    true},
        {UiLocale::Italian,            "it",    false},
        {UiLocale::German,             "de",    false},
    };
    for (const LocaleCase& c : kCases) {
        const std::wstring header = header_for(c.locale);
        const std::wstring pill = pill_for(c.locale);
        const TT::BackTransHeaderPlan p = TT::PlanBackTransHeaderRow(
            static_cast<int>(header.size()), static_cast<int>(pill.size()), 360);
        if (c.expect_single_row) {
            TEST_CHECK(p.layout == Layout::SingleRow,
                       "FIX-WS3B: a short locale keeps the single-row layout");
            TEST_CHECK(p.pill_row_h == 0.0f,
                       "FIX-WS3B: a short locale adds no pill row height");
        } else {
            TEST_CHECK(p.layout != Layout::SingleRow,
                       "FIX-WS3B: a long locale wraps the pill off the header row");
            TEST_CHECK(p.pill_row_h > 0.0f,
                       "FIX-WS3B: a long locale grows the card by the pill row");
            TEST_CHECK(p.ellipsize == false,
                       "FIX-WS3B: a long locale wraps the pill instead of clipping the header");
        }
    }

    // The German and Italian header strings are the CEO-reported overlap cases;
    // pin their literal lengths so a future shortening is a visible test change.
    I18n::SetLocale(UiLocale::German);
    TEST_CHECK(I18n::Get(StringId::BackTranslateHeader) == L"🔄 Rückübersetzungs-Prüfung",
               "FIX-WS3B de: the header literal is the reported long string");
    I18n::SetLocale(UiLocale::Italian);
    TEST_CHECK(I18n::Get(StringId::BackTranslateHeader) == L"🔄 Verifica retrotraduzione",
               "FIX-WS3B it: the header literal is the reported long string");
    TEST_CHECK(I18n::Get(StringId::BackTranslateMatchCheck) == L"⚡ Controlla la sfumatura",
               "FIX-WS3B it: the pill literal is the reported long string");

    // ---- card height math includes the extra row ----
    {
        const TT::BackTransHeaderPlan p = TT::PlanBackTransHeaderRow(60, 30, 360);
        const int extra = static_cast<int>(p.pill_row_h);
        const TT::BackTransCardSize single = TT::ComputeBackTransCardSize(80, 20, 0);
        const TT::BackTransCardSize wrapped = TT::ComputeBackTransCardSize(80, 20, extra);
        TEST_CHECK(extra > 0, "FIX-WS3B: the long-locale plan produces a positive extra row");
        TEST_CHECK(wrapped.height == single.height + extra,
                   "FIX-WS3B: the card height includes the wrapped pill row");
        TEST_CHECK(wrapped.width == single.width,
                   "FIX-WS3B: wrapping the pill never changes the card width");
        // The default argument keeps every pre-FIX-WS3B call site byte-identical.
        TEST_CHECK(TT::ComputeBackTransCardSize(80, 20).height == single.height,
                   "FIX-WS3B: the default extra-row argument preserves the old height");
    }

    // ---- the wrapped row is strictly taller than the single row ----
    // (Both operands must actually land in different branches: a "long" pair
    // that still fits one row would make this comparison vacuous.)
    {
        const TT::BackTransHeaderPlan single = TT::PlanBackTransHeaderRow(20, 10, 360);
        const TT::BackTransHeaderPlan wrapped = TT::PlanBackTransHeaderRow(60, 30, 360);
        TEST_CHECK(single.layout == Layout::SingleRow,
                   "FIX-WS3B: the short pair is a single-row plan");
        TEST_CHECK(wrapped.layout != Layout::SingleRow,
                   "FIX-WS3B: the long pair is a wrapped plan");
        TEST_CHECK(wrapped.pill_row_h > single.pill_row_h,
                   "FIX-WS3B: the wrapped plan is taller than the single-row plan");
    }

    I18n::SetLocale(initial); // suite-state hygiene

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] WS3 FIX-WS3B header-row plan passed." << std::endl;
    } else {
        std::cout << "[FAIL] WS3 FIX-WS3B header-row plan: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// Orchestrator entry point (wired from tests/run_tests.cpp).
void TestWs3Tooltip() {
    TestWs3BackTransCardSize();
    TestWs3BackTransPanelPlan();   // v0.10.9 (U-B): the two-panel layout
    TestWs3BackTransTimerPlan();
    TestWs3BackTransCardPlacement();
    TestWs3BackTransLabels();
    TestWs3BackTransHeaderRowPlan();
    TestWs3BackTransMarshalSeam();
    TestWs3BackTransStructuralPins();
}
