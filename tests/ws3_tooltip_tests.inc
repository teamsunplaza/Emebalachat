// ---------------------------------------------------------------------------
// 261108_0002 (WS3, v0.10.6) - the floating back-translation verification card
// in the tooltip layer.
//
// User intent (verbatim): "역번역 플로팅 확인 UI: 번역 결과(BBBBB)를 원문 언어로
// 자동 역번역(AAAAA')하여 플로팅 카드로 보여주기".
//
// This suite pins the card's headless-testable contract:
//   (a) TestWs3BackTransCardSize - TooltipWindow::ComputeBackTransCardSize:
//       the fixed 360 DIP width, the minimum/maximum height clamp, and
//       monotonicity in BOTH text lengths (a longer body or a longer reference
//       line can never make the card shorter).
//   (b) TestWs3BackTransTimerPlan - TooltipWindow::PlanBackTransTimer: the
//       4500 ms auto-dismiss, pause on the first pointer move, no timer churn
//       on repeated moves, the 1500 ms grace window on leave, and the
//       immediate kill on an explicit dismissal.
//   (c) TestWs3BackTransCardPlacement - TooltipWindow::PlaceBackTransCard:
//       horizontal centring on the anchor, the gap, the above->below flip when
//       the work area has no room, and the shared edge clamp.
//   (d) TestWs3BackTransLabels - FormatBackTransHeader / FormatBackTransReference
//       / BackTransNeedsReferenceLine: the " (EN → KO)" suffix shape, the
//       empty-label drop, the "↳ 원문: " reference prefix, and the
//       AAAAA' == AAAAA suppression rule.
//   (e) TestWs3BackTransMarshalSeam - the thread-safe entry's live contract on
//       a real TooltipWindow: an off-GUI-thread ShowBackTranslationThreadSafe
//       reaches the model through the value queue, DismissThreadSafe clears
//       pending entries, and DrainMarshalQueue counts the payload.
//   (f) TestWs3BackTransStructuralPins - source pins on src/ui/tooltip.cpp /
//       tooltip.hpp: the API names the main.cpp unit is written against, the
//       message id 0x206, the timer id 4003, the width reset in
//       ShowTranslation, the dismiss-on-new-translation call, and the
//       hover/ESC wiring.
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

    // Empty payload floors at the minimum height (volatile pins avoid C4127).
    volatile int min_h = TT::kBackTransCardMinHeightDip;
    volatile int max_h = TT::kBackTransCardMaxHeightDip;
    TEST_CHECK(TT::ComputeBackTransCardSize(0, 0).height == min_h,
               "WS3 size: an empty payload floors at the minimum height");
    TEST_CHECK(TT::ComputeBackTransCardSize(-5, -5).height == min_h,
               "WS3 size: negative lengths clamp to the minimum (defensive)");
    TEST_CHECK(min_h > 0 && max_h > min_h,
               "WS3 size: the min/max band is well formed");

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

    // A reference line never shrinks the card (it is additive).
    for (int len = 0; len <= 400; len += 37) {
        const int without = TT::ComputeBackTransCardSize(len, 0).height;
        const int with = TT::ComputeBackTransCardSize(len, 60).height;
        TEST_CHECK(with >= without,
                   "WS3 size: adding a reference line never shrinks the card");
    }

    // Clamp ceiling: a pathological body stops at the maximum.
    TEST_CHECK(TT::ComputeBackTransCardSize(100000, 100000).height == max_h,
               "WS3 size: a pathological payload clamps at the maximum height");

    // Growth actually happens at the wrap boundary (one extra line = one line
    // height). Lengths are chosen well above the minimum-height clamp so the
    // clamp cannot mask the step.
    volatile int per_line = TT::kBackTransCharsPerLine;
    volatile int line_h = TT::kBackTransLineHeightDip;
    TEST_CHECK(per_line == 44 && line_h == 20,
               "WS3 size: the wrap width and line height are the documented constants");
    TEST_CHECK(TT::ComputeBackTransCardSize(per_line * 5, 0).height + line_h ==
                   TT::ComputeBackTransCardSize(per_line * 5 + 1, 0).height,
               "WS3 size: crossing the wrap boundary adds exactly one line height");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] WS3 back-translation card size planner passed." << std::endl;
    } else {
        std::cout << "[FAIL] WS3 back-translation card size planner: "
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

    // Spec constants: 4500 ms auto-dismiss, 1500 ms hover grace.
    volatile DWORD autohide = TT::kBackTransAutohideMs;
    volatile DWORD grace = TT::kBackTransHoverGraceMs;
    TEST_CHECK(autohide == 4500, "WS3 timer: auto-dismiss is 4500 ms");
    TEST_CHECK(grace == 1500, "WS3 timer: the hover grace window is 1500 ms");

    // Shown -> arm the full autohide window.
    const TT::BackTransTimerPlan shown = TT::PlanBackTransTimer(Ev::Shown, false);
    TEST_CHECK(shown.action == Act::ArmAutohide, "WS3 timer: showing the card arms the autohide");
    TEST_CHECK(shown.delay_ms == 4500, "WS3 timer: the armed delay is 4500 ms");

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

    // The grace window is strictly shorter than the autohide window, so a
    // hover cycle always ends the card sooner than a fresh show would.
    TEST_CHECK(grace < autohide,
               "WS3 timer: the grace window is shorter than the full autohide window");

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

    // Reference line: "<localized label>AAAAA".
    // 261108_0002 (FIX-WS3B): the label is no longer a hardcoded Korean
    // literal - it comes from StringId::BackTranslateReference, so the test
    // must SET the locale it asserts against instead of relying on whatever
    // the host machine's UI locale happens to be.
    {
        const UiLocale initial = I18n::GetCurrentLocale();

        I18n::SetLocale(UiLocale::Korean);
        const std::wstring ko = TT::FormatBackTransReference(L"안녕하세요");
        TEST_CHECK(ko == L"↳ 원문: 안녕하세요",
                   "WS3 label ko: the reference line carries the U+21B3 marker and the original");
        TEST_CHECK(ko.rfind(L"↳ 원문: ", 0) == 0,
                   "WS3 label ko: the reference prefix is front-anchored");

        I18n::SetLocale(UiLocale::English);
        const std::wstring en = TT::FormatBackTransReference(L"hello");
        TEST_CHECK(en == L"↳ Original: hello",
                   "WS3 label en: the reference label is the English string, not Korean");
        TEST_CHECK(en.find(L"원문") == std::wstring::npos,
                   "WS3 label en: the Korean label never leaks into a non-Korean locale");

        // FIX-WS3B's whole point: a non-Korean, non-English locale also gets
        // its own label (this is the regression the CEO reported).
        I18n::SetLocale(UiLocale::Italian);
        const std::wstring it = TT::FormatBackTransReference(L"ciao");
        TEST_CHECK(it == L"↳ Originale: ciao",
                   "WS3 label it: the Italian label is used");
        TEST_CHECK(it.find(L"원문") == std::wstring::npos,
                   "WS3 label it: the Korean label never leaks into Italian");

        I18n::SetLocale(initial);
    }

    // The reference line is suppressed on an exact round trip.
    TEST_CHECK(!TT::BackTransNeedsReferenceLine(L"same", L"same"),
               "WS3 label: an exact round trip suppresses the reference line");
    TEST_CHECK(TT::BackTransNeedsReferenceLine(L"same", L"different"),
               "WS3 label: a changed round trip shows the reference line");
    TEST_CHECK(!TT::BackTransNeedsReferenceLine(L"", L"anything"),
               "WS3 label: an empty original never shows a reference line");
    TEST_CHECK(TT::BackTransNeedsReferenceLine(L"a", L"a "),
               "WS3 label: a whitespace-only difference still shows the reference");

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
    TEST_CHECK(tooltip.GetBackTransReferenceLine() == L"↳ 원문: 안녕하세요",
               "WS3 marshal: the reference line carries the original text");

    // An exact round trip drops the reference line.
    tooltip.ShowBackTranslationThreadSafe(L"Hello", L"Hello", L"EN", L"KO",
                                          true, nullptr, nullptr);
    TEST_CHECK(tooltip.IsBackTransMatched(), "WS3 marshal: the matched verdict reaches the model");
    TEST_CHECK(tooltip.GetBackTransReferenceLine().empty(),
               "WS3 marshal: an exact round trip leaves the reference line empty");

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
        TEST_CHECK(tooltip.GetBackTransReferenceLine() == L"↳ 원문: 원문",
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
               "WS3 pin: the auto-dismiss delay is 4500 ms");
    TEST_CHECK(hpp.find("static constexpr DWORD kBackTransHoverGraceMs = 1500;") !=
                   std::string::npos,
               "WS3 pin: the hover grace window is 1500 ms");

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

    // ---- rendering conventions ----
    TEST_CHECK(cpp.find("void TooltipWindow::RenderBackTranslation() {") != std::string::npos,
               "WS3 pin: the card has its own render body");    {
        const std::string render = Ws3TipFuncBody(cpp, "void TooltipWindow::RenderBackTranslation() {");
        TEST_CHECK(!render.empty(), "WS3 pin: RenderBackTranslation body is locatable");
        TEST_CHECK(render.find("theme.state_ready") != std::string::npos,
                   "WS3 pin: the ✓ pill uses the theme's ready/green token");
        TEST_CHECK(render.find("theme.accent_primary") != std::string::npos,
                   "WS3 pin: the ⚡ pill uses the theme's accent token");
        TEST_CHECK(render.find("StringId::BackTranslateMatchExact") != std::string::npos &&
                       render.find("StringId::BackTranslateMatchCheck") != std::string::npos,
                   "WS3 pin: both pill labels come from the i18n table");
        TEST_CHECK(render.find("backtrans_body_format_") != std::string::npos &&
                       render.find("backtrans_ref_format_") != std::string::npos,
                   "WS3 pin: the body and reference line use their own formats");
        TEST_CHECK(render.find("theme.text_muted") != std::string::npos,
                   "WS3 pin: the reference line is muted");
    }
    TEST_CHECK(cpp.find("StringId::BackTranslateHeader") != std::string::npos,
               "WS3 pin: the card header comes from the i18n table");
    // The 13pt SemiBold body format is created once, in Create().
    TEST_CHECK(Ws3TipCount(cpp, "&backtrans_body_format_") == 1,
               "WS3 pin: the body format is created exactly once (Create)");
    TEST_CHECK(cpp.find("DWRITE_FONT_WEIGHT_SEMI_BOLD,") != std::string::npos,
               "WS3 pin: the card body is SemiBold");
    TEST_CHECK(cpp.find("backtrans_body_format_->Release();") != std::string::npos &&
                   cpp.find("backtrans_ref_format_->Release();") != std::string::npos &&
                   cpp.find("backtrans_header_format_->Release();") != std::string::npos,
               "WS3 pin: every card format is released in Destroy");

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
    TestWs3BackTransTimerPlan();
    TestWs3BackTransCardPlacement();
    TestWs3BackTransLabels();
    TestWs3BackTransHeaderRowPlan();
    TestWs3BackTransMarshalSeam();
    TestWs3BackTransStructuralPins();
}
