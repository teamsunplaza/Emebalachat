// ---------------------------------------------------------------------------
// REQ-053 (session 260922_0001, UI audit fix bundle A1-A8): the tray single-
// click debounce state machine, the About dynamic feature-block planner, the
// badge WM_DPICHANGED handler + device-lost recovery, the tooltip menu focus
// restore, the OpenAI fetch-button busy feedback, the GGUF manager action-
// button sync, and the sound worker queue policy. Staged as an .inc next to
// this runner (260922_0001 D); registered near the end of main() after
// TestReq052TouchFilterPredicate(), per the end-of-file pattern.
//
// Coverage:
//   (a) TestReq053TrayToggleDebounce — the pure TrayToggleDebounce state
//       machine (tray_toggle_debounce.hpp, A1): single UP + expiry toggles
//       once, UP + DBLCLK + expiry toggles zero times, UP + UP + expiry
//       collapses to one toggle at the LAST click, stray expiry is a no-op.
//   (b) TestReq053AboutFeatureLayout — the pure
//       AboutWindow::PlanAboutFeatureLayout planner (about_window.hpp, A2):
//       1/2/3-line heights, the 40 DIP block floor, cumulative top offsets,
//       the squeeze trigger/skip boundary, and the RTL marker math
//       (card_w - 33 DIP), plus source pins that Render consumes the plan
//       and the old fixed `230 + i*42` grid is gone.
//   (c) TestReq053BadgeDpiChangedPins — badge.cpp source pins (A3): the new
//       case WM_DPICHANGED and its 5-step order (suggested rect -> WindowDpi
//       -> SetWindowPos(SWP_NOACTIVATE) -> ReallocateBuffer -> Render).
//   (d) TestReq053DeviceLostRecovery — the A4 recovery branch: the runtime
//       IsRecoverableDeviceLost predicate (asset_loader.hpp, already linked)
//       plus badge pins — Render(bool reentry_allowed), bool
//       RecreateAfterDeviceLost(), the exactly-one Render(false) recursion
//       call, and the skipped commit on the failed/already-recovering passes.
//   (e) TestReq053TooltipFocusRestorePins — tooltip.cpp source pins (A5):
//       exactly two GetForegroundWindow backups, two window-liveness-guarded
//       SetForegroundWindow restores, each ordered backup < modal menu <
//       restore on both language menus.
//   (f) TestReq053OpenAiButtonDisablePins — openai_settings_window.cpp source
//       pins (A6): one FALSE disable (spawn), TWO TRUE restores (stale drop +
//       normal completion — the A6 report's finding), the fetch_button_busy
//       field, and no new StringId (EnumCount still 110).
//   (g) TestReq053GgufButtonSyncPins — gguf_model_manager_window.cpp source
//       pins (A7): SyncManagerActionButtons definition + 3 call sites,
//       the LBN_SELCHANGE branch, the post-rebuild sync, and the rename/
//       delete EnableWindow pair.
//   (h) TestReq053SoundQueuePolicy — the pure SoundEnqueuePolicy boundary
//       (sound.hpp, A8) with a drop-oldest queue simulation, plus sound.cpp
//       pins: kToneTable + both static_asserts intact, the detached
//       std::jthread pattern gone (one std::thread worker + call_once), and
//       the ShutdownSound join wired from main.cpp.
//
// Source pins read through ResolveRepoFile + raw-byte find() (the
// TestReq048CloudFailureSignaling / TestReq052 precedent; every needle is
// ASCII-only, and the reader normalizes CRLF so line-anchored needles never
// depend on the checkout's autocrlf).
// ---------------------------------------------------------------------------

#include <deque> // 260922_0001 D: drop-oldest simulation mirrors g_queue's std::deque ops

#include "../src/tray_toggle_debounce.hpp" // 260922_0001 A1 (not included by this runner)

namespace {

// Resolve + read a production source for the REQ-053 structural pins (same
// candidate-path precedent as the req047/req051/req052 readers; CRLF-
// normalized).
void Req053ReadSrc(const wchar_t* rel, std::string& out) {
    out.clear();
    const std::filesystem::path p = ResolveRepoFile(rel);
    if (p.empty()) return;
    std::ifstream in(p, std::ios::binary);
    if (!in) return;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string norm;
    norm.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\r' && i + 1 < out.size() && out[i + 1] == '\n') continue;
        norm.push_back(out[i]);
    }
    out.swap(norm);
}

// Count non-overlapping occurrences of an ASCII needle.
size_t Req053Count(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) {
        ++n;
    }
    return n;
}

// Drive the tray debounce state machine over a fixed event sequence and count
// how many times the sequence would actually toggle the engine.
int Req053RunToggleSequence(const TrayToggleEvent* events, size_t count) {
    bool pending = false;
    int toggles = 0;
    for (size_t i = 0; i < count; ++i) {
        const TrayToggleDecision d = TrayToggleDebounceStateMachine(events[i], pending);
        pending = d.timer_pending;
        if (d.action == TrayToggleAction::kToggleNow) ++toggles;
    }
    return toggles;
}

} // namespace

// ---- (a) tray single-click debounce state machine (A1) ---------------------

void TestReq053TrayToggleDebounce() {
    std::cout << "[RUN] Testing REQ-053 tray single-click debounce..." << std::endl;
    const int failures_before = g_failed_count;

    const TrayToggleEvent kUp = TrayToggleEvent::kLeftButtonUp;
    const TrayToggleEvent kDbl = TrayToggleEvent::kLeftButtonDblClk;
    const TrayToggleEvent kExp = TrayToggleEvent::kTimerExpired;

    // (UP -> expiry) = exactly one toggle (the plain single click).
    {
        const TrayToggleEvent seq[] = { kUp, kExp };
        volatile int toggles = Req053RunToggleSequence(seq, 2);
        TEST_CHECK(toggles == 1,
                   "REQ-053: a single UP followed by the timer expiry toggles once");
    }
    // (UP -> DBLCLK -> expiry) = zero toggles: the double-click only flips the
    // badge (main.cpp), never the engine.
    {
        const TrayToggleEvent seq[] = { kUp, kDbl, kExp };
        volatile int toggles = Req053RunToggleSequence(seq, 3);
        TEST_CHECK(toggles == 0,
                   "REQ-053: UP + DBLCLK + expiry never toggles the engine");
    }
    // (UP -> UP -> expiry) = one toggle, evaluated at the LAST click (each UP
    // re-arms; a burst of rapid single clicks collapses to one toggle).
    {
        const TrayToggleEvent seq[] = { kUp, kUp, kExp };
        volatile int toggles = Req053RunToggleSequence(seq, 3);
        TEST_CHECK(toggles == 1,
                   "REQ-053: two rapid single clicks collapse to one toggle");
    }
    {
        const TrayToggleEvent seq[] = { kUp, kUp, kUp, kExp };
        volatile int toggles = Req053RunToggleSequence(seq, 4);
        TEST_CHECK(toggles == 1,
                   "REQ-053: three rapid single clicks still collapse to one toggle");
    }
    // Stray expiry with nothing armed is a no-op.
    {
        const TrayToggleEvent seq[] = { kExp };
        volatile int toggles = Req053RunToggleSequence(seq, 1);
        TEST_CHECK(toggles == 0,
                   "REQ-053: a stray timer expiry with no pending arm is a no-op");
    }
    // DBLCLK with nothing armed still cancels (and stays armed=false).
    {
        const TrayToggleEvent seq[] = { kDbl, kExp };
        volatile int toggles = Req053RunToggleSequence(seq, 2);
        TEST_CHECK(toggles == 0,
                   "REQ-053: a DBLCLK with no pending arm never toggles");
    }

    // Per-event action + carried armed state (the transition contract the
    // WndProc consumes).
    const TrayToggleDecision up_idle = TrayToggleDebounceStateMachine(kUp, false);
    TEST_CHECK(up_idle.action == TrayToggleAction::kArmTimer && up_idle.timer_pending,
               "REQ-053: UP from idle arms the timer and records the arm");
    const TrayToggleDecision up_armed = TrayToggleDebounceStateMachine(kUp, true);
    TEST_CHECK(up_armed.action == TrayToggleAction::kArmTimer && up_armed.timer_pending,
               "REQ-053: UP while armed re-arms (restarts the debounce window)");
    const TrayToggleDecision dbl_armed = TrayToggleDebounceStateMachine(kDbl, true);
    TEST_CHECK(dbl_armed.action == TrayToggleAction::kCancelTimer && !dbl_armed.timer_pending,
               "REQ-053: DBLCLK cancels the pending arm");
    const TrayToggleDecision dbl_idle = TrayToggleDebounceStateMachine(kDbl, false);
    TEST_CHECK(dbl_idle.action == TrayToggleAction::kCancelTimer && !dbl_idle.timer_pending,
               "REQ-053: DBLCLK with no arm is still a (harmless) cancel");
    const TrayToggleDecision exp_armed = TrayToggleDebounceStateMachine(kExp, true);
    TEST_CHECK(exp_armed.action == TrayToggleAction::kToggleNow && !exp_armed.timer_pending,
               "REQ-053: expiry with an armed timer toggles and clears the arm");
    const TrayToggleDecision exp_idle = TrayToggleDebounceStateMachine(kExp, false);
    TEST_CHECK(exp_idle.action == TrayToggleAction::kNone && !exp_idle.timer_pending,
               "REQ-053: expiry with no arm is kNone");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-053 tray single-click debounce passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-053 tray single-click debounce: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (b) About dynamic feature-block layout (A2) ---------------------------

void TestReq053AboutFeatureLayout() {
    std::cout << "[RUN] Testing REQ-053 About feature-block layout plan..." << std::endl;
    const int failures_before = g_failed_count;

    // (Nested types: qualify through AboutWindow — no using-declarations, per
    // the req052 C2885 note. The planner return type is taken with `auto`.)
    AboutWindow::AboutFeatureMetrics one_line[3] = {{12.0f}, {12.0f}, {12.0f}};
    AboutWindow::AboutFeatureMetrics three_line[3] = {{60.0f}, {60.0f}, {60.0f}};
    AboutWindow::AboutFeatureMetrics mixed[3] = {{40.0f}, {40.0f}, {12.0f}};
    AboutWindow::AboutFeatureMetrics at_boundary[3] = {{42.0f}, {32.0f}, {32.0f}};
    AboutWindow::AboutFeatureMetrics over_boundary[3] = {{43.0f}, {32.0f}, {32.0f}};

    // 1-line (state: every natural height floors at 40 DIP): the plan must
    // reproduce the OLD fixed geometry exactly — tops 230/272/314, next_free
    // 354 — so the 1-line locales render byte-identically to pre-fix.
    const auto p1 = AboutWindow::PlanAboutFeatureLayout(one_line, false, 440.0f);
    volatile float p1_h0 = p1.block_h[0];
    volatile float p1_h1 = p1.block_h[1];
    volatile float p1_h2 = p1.block_h[2];
    TEST_CHECK(p1_h0 == 40.0f && p1_h1 == 40.0f && p1_h2 == 40.0f,
               "REQ-053: a 1-line block floors at the historical 40 DIP height");
    volatile float p1_t0 = p1.block_top[0];
    volatile float p1_t1 = p1.block_top[1];
    volatile float p1_t2 = p1.block_top[2];
    volatile float p1_nf = p1.next_free_y;
    TEST_CHECK(p1_t0 == 230.0f && p1_t1 == 272.0f && p1_t2 == 314.0f,
               "REQ-053: the 1-line case keeps the old 230/+42/+42 stack");
    TEST_CHECK(p1_nf == 354.0f,
               "REQ-053: the 1-line next_free_y is the old 354 baseline");

    // 3-line worst case: blocks grow to their natural height first, then the
    // squeeze pass brings next_free_y back onto the design ceiling (364).
    const auto p3 = AboutWindow::PlanAboutFeatureLayout(three_line, false, 440.0f);
    volatile float p3_nf = p3.next_free_y;
    TEST_CHECK(p3_nf > 363.9f && p3_nf < 364.1f,
               "REQ-053: the 3-line squeeze lands next_free_y on the 364 DIP ceiling");
    bool p3_floors_ok = p3.block_h[0] >= 40.0f && p3.block_h[1] >= 40.0f && p3.block_h[2] >= 40.0f;
    TEST_CHECK(p3_floors_ok,
               "REQ-053: the squeeze never cuts a block below the 40 DIP floor");
    bool p3_cumulative = p3.block_top[0] == 230.0f &&
                         p3.block_top[1] == p3.block_top[0] + p3.block_h[0] + 2.0f &&
                         p3.block_top[2] == p3.block_top[1] + p3.block_h[1] + 2.0f &&
                         p3.next_free_y == p3.block_top[2] + p3.block_h[2];
    TEST_CHECK(p3_cumulative,
               "REQ-053: squeezed tops stay cumulative (2 DIP inter-block gap)");

    // Mixed heights: only the tall blocks shed (pro-rata of their excess
    // above 40), the floored block is untouched.
    const auto pm = AboutWindow::PlanAboutFeatureLayout(mixed, false, 440.0f);
    volatile float pm_h0 = pm.block_h[0];
    volatile float pm_h1 = pm.block_h[1];
    volatile float pm_h2 = pm.block_h[2];
    volatile float pm_nf = pm.next_free_y;
    TEST_CHECK(pm_h0 == 45.0f && pm_h1 == 45.0f && pm_h2 == 40.0f,
               "REQ-053: a mixed 48/48/40 stack sheds 3 DIP per tall block for its "
               "6 DIP overflow (the floored block keeps 40)");
    TEST_CHECK(pm_nf == 364.0f,
               "REQ-053: the mixed case lands next_free_y on 364");
    volatile float pm_t1 = pm.block_top[1];
    volatile float pm_t2 = pm.block_top[2];
    TEST_CHECK(pm_t1 == 277.0f && pm_t2 == 324.0f,
               "REQ-053: squeezed cumulative tops are recomputed from the new heights");

    // Squeeze trigger boundary: next_free_y == 364 exactly is NOT squeezed
    // (the pass fires only on overflow > 0); one DIP more fires it.
    const auto pb = AboutWindow::PlanAboutFeatureLayout(at_boundary, false, 440.0f);
    volatile float pb_h0 = pb.block_h[0];
    volatile float pb_nf = pb.next_free_y;
    volatile float pb_t1 = pb.block_top[1];
    TEST_CHECK(pb_h0 == 50.0f && pb_t1 == 282.0f && pb_nf == 364.0f,
               "REQ-053: next_free_y of exactly 364 does not trigger the squeeze");
    const auto po = AboutWindow::PlanAboutFeatureLayout(over_boundary, false, 440.0f);
    volatile float po_h0 = po.block_h[0];
    volatile float po_nf = po.next_free_y;
    TEST_CHECK(po_h0 == 50.0f && po_nf == 364.0f,
               "REQ-053: a 1 DIP overflow squeezes the single tap-block back to 364");

    // Cumulative-offset + floor invariant sweep (LTR and RTL, several card
    // widths): every top is the running sum and next_free_y closes the stack;
    // markers mirror at card_w - 33 for RTL.
    bool inv_ok = true;
    const float heights[] = {0.0f, 12.0f, 32.0f, 33.0f, 40.0f, 60.0f, 120.0f};
    const float card_ws[] = {360.0f, 440.0f, 512.0f};
    for (float h : heights) {
        AboutWindow::AboutFeatureMetrics m[3] = {{h}, {h}, {h}};
        for (float cw : card_ws) {
            for (int rtl = 0; rtl < 2; ++rtl) {
                const auto p = AboutWindow::PlanAboutFeatureLayout(m, rtl != 0, cw);
                if (!(p.block_h[0] >= 40.0f && p.block_h[1] >= 40.0f && p.block_h[2] >= 40.0f))
                    inv_ok = false;
                if (p.block_top[0] != 230.0f) inv_ok = false;
                if (p.block_top[1] != p.block_top[0] + p.block_h[0] + 2.0f) inv_ok = false;
                if (p.block_top[2] != p.block_top[1] + p.block_h[1] + 2.0f) inv_ok = false;
                if (p.next_free_y != p.block_top[2] + p.block_h[2]) inv_ok = false;
                const float expect_marker = (rtl != 0) ? (cw - 33.0f) : 28.0f;
                if (p.marker_x[0] != expect_marker || p.marker_x[1] != expect_marker ||
                    p.marker_x[2] != expect_marker) {
                    inv_ok = false;
                }
            }
        }
    }
    TEST_CHECK(inv_ok,
               "REQ-053: the planner invariants hold across the sweep "
               "(cumulative tops, next_free closure, 40 DIP floor, mirrored markers)");

    // RTL marker math pinned explicitly (fix plan §5 A2 risk item 2).
    AboutWindow::AboutFeatureMetrics any_m[3] = {{12.0f}, {12.0f}, {12.0f}};
    const auto rtl = AboutWindow::PlanAboutFeatureLayout(any_m, true, 440.0f);
    volatile float rtl_marker = rtl.marker_x[0];
    TEST_CHECK(rtl_marker == 407.0f,
               "REQ-053: the RTL marker sits card_w - 33 DIP in from the right edge");
    const auto ltr = AboutWindow::PlanAboutFeatureLayout(any_m, false, 440.0f);
    volatile float ltr_marker = ltr.marker_x[0];
    TEST_CHECK(ltr_marker == 28.0f,
               "REQ-053: the LTR marker keeps the historical 28 DIP left inset");

    // Render() consumes the plan; the old fixed `230 + i*42` grid is gone; the
    // per-block CLIP stays (residual overflow is contained, never bleeding).
    std::string abt;
    Req053ReadSrc(L"src\\ui\\about_window.cpp", abt);
    if (abt.empty()) {
        std::cout << "[SKIP] src/ui/about_window.cpp not resolvable; REQ-053 About pins skipped."
                  << std::endl;
        return;
    }
    TEST_CHECK(abt.find("const AboutFeaturePlan plan = PlanAboutFeatureLayout(feature_metrics, ui_rtl, w);") !=
                   std::string::npos,
               "REQ-053 pin: Render plans the feature section through the pure planner");
    TEST_CHECK(abt.find("static_cast<float>(i) * 42.0f") == std::string::npos,
               "REQ-053 pin: the old fixed 230 + i*42 grid is fully replaced");
    TEST_CHECK(abt.find("AboutFeatureMetrics feature_metrics[3] = {};") != std::string::npos &&
                   abt.find("feature_metrics[i].height = m.height;") != std::string::npos,
               "REQ-053 pin: Render fills the metrics from IDWriteTextLayout::GetMetrics");
    TEST_CHECK(abt.find("const float base = plan.next_free_y;") != std::string::npos,
               "REQ-053 pin: the bottom-section anchors derive from plan.next_free_y");
    TEST_CHECK(Req053Count(abt, ", D2D1_DRAW_TEXT_OPTIONS_CLIP);") == 1,
               "REQ-053 pin: the single per-block CLIP draw is retained");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-053 About feature-block layout plan passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-053 About feature-block layout plan: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (c) badge WM_DPICHANGED handler (A3) ---------------------------------

void TestReq053BadgeDpiChangedPins() {
    std::cout << "[RUN] Testing REQ-053 badge WM_DPICHANGED pins..." << std::endl;
    const int failures_before = g_failed_count;

    std::string badge;
    Req053ReadSrc(L"src\\ui\\badge.cpp", badge);
    if (badge.empty()) {
        std::cout << "[SKIP] src/ui/badge.cpp not resolvable; REQ-053 badge DPI pins skipped."
                  << std::endl;
        return;
    }

    // The handler exists (the A3 finding: WndProc had no WM_DPICHANGED case at
    // all, so live display-scaling changes left the physical DIB stale and the
    // compositor upscaled the blit).
    const size_t dpi_case = badge.find("case WM_DPICHANGED: {");
    TEST_CHECK(dpi_case != std::string::npos,
               "REQ-053 pin: badge WndProc handles WM_DPICHANGED");

    // The 5-step house pattern, in order, all INSIDE that case block:
    //   1. suggested origin rect from lParam
    //   2. dpi_ refresh through the ui/dpi helper
    //   3. SetWindowPos at our OWN extents with SWP_NOACTIVATE (not SWP_NOSIZE)
    //   4. ReallocateBuffer re-binds SetDpi + BindDC on the fresh DIB
    //   5. full Render() (which terminates in its own UpdateAlpha commit)
    const size_t suggested = badge.find("const RECT* suggested = reinterpret_cast<const RECT*>(lParam);",
                                        dpi_case);
    const size_t dpi_refresh = badge.find("self->dpi_ = emebalachat::ui::WindowDpi(hwnd);", dpi_case);
    const size_t setpos = badge.find("SWP_NOZORDER | SWP_NOACTIVATE);", dpi_case);
    const size_t realloc = badge.find("self->ReallocateBuffer(self->PhysW(), self->PhysH());",
                                      dpi_case);
    const size_t rerender = badge.find("self->Render();", dpi_case);
    TEST_CHECK(suggested != std::string::npos && dpi_refresh != std::string::npos &&
                   setpos != std::string::npos && realloc != std::string::npos &&
                   rerender != std::string::npos,
               "REQ-053 pin: all 5 WM_DPICHANGED steps are present in the case block");
    TEST_CHECK(suggested < dpi_refresh && dpi_refresh < setpos && setpos < realloc &&
                   realloc < rerender,
               "REQ-053 pin: the 5 steps run in the house order "
               "(suggested -> WindowDpi -> SetWindowPos -> ReallocateBuffer -> Render)");
    TEST_CHECK(badge.find("::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,", dpi_case) !=
                       std::string::npos &&
                   badge.find("self->PhysW(), self->PhysH(),", dpi_case) != std::string::npos,
               "REQ-053 pin: the resize keeps our own PhysW/PhysH extents (a suggested-size "
               "window rect would rescale the blit the fix targets)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-053 badge WM_DPICHANGED pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-053 badge WM_DPICHANGED pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (d) device-lost recovery (A4) ----------------------------------------

void TestReq053DeviceLostRecovery() {
    std::cout << "[RUN] Testing REQ-053 device-lost recovery pins..." << std::endl;
    const int failures_before = g_failed_count;

    // Runtime re-pin of the branch predicate (asset_loader.hpp, already linked
    // into this runner): only D2DERR_RECREATE_TARGET enters the recovery path.
    volatile HRESULT hr_lost = D2DERR_RECREATE_TARGET;
    volatile HRESULT hr_ok = S_OK;
    volatile HRESULT hr_wrong = D2DERR_WRONG_STATE;
    volatile HRESULT hr_fail = E_FAIL;
    TEST_CHECK(IsRecoverableDeviceLost(hr_lost),
               "REQ-053: D2DERR_RECREATE_TARGET is the recoverable device-lost code");
    TEST_CHECK(!IsRecoverableDeviceLost(hr_ok) && !IsRecoverableDeviceLost(hr_wrong) &&
                   !IsRecoverableDeviceLost(hr_fail),
               "REQ-053: success / wrong-state / generic failure never enter recovery");

    std::string badge_h;
    Req053ReadSrc(L"src\\ui\\badge.hpp", badge_h);
    std::string badge;
    Req053ReadSrc(L"src\\ui\\badge.cpp", badge);
    if (badge_h.empty() || badge.empty()) {
        std::cout << "[SKIP] badge.hpp/.cpp not resolvable; REQ-053 recovery pins skipped."
                  << std::endl;
        return;
    }

    // Render() gained the recursion cap; RecreateAfterDeviceLost() now reports
    // success (void -> bool) so the caller can skip the pixel commit.
    TEST_CHECK(badge_h.find("void Render(bool reentry_allowed = true);") != std::string::npos,
               "REQ-053 pin: badge.hpp declares Render(bool reentry_allowed = true)");
    TEST_CHECK(badge_h.find("bool RecreateAfterDeviceLost();") != std::string::npos,
               "REQ-053 pin: badge.hpp declares RecreateAfterDeviceLost() -> bool");
    TEST_CHECK(badge.find("bool FloatingBadge::RecreateAfterDeviceLost() {") != std::string::npos,
               "REQ-053 pin: the definition returns bool");

    // Recursion cap: exactly ONE re-entrant Render call in the whole file, and
    // it is guarded by reentry_allowed AND a successful recreation.
    TEST_CHECK(Req053Count(badge, "Render(false);") == 1,
               "REQ-053 pin: exactly one re-entrant Render(false) call (depth capped at 2)");
    const size_t guard = badge.find("if (reentry_allowed && RecreateAfterDeviceLost()) {");
    const size_t reentry = badge.find("Render(false);");
    TEST_CHECK(guard != std::string::npos && reentry != std::string::npos && guard < reentry,
               "REQ-053 pin: the re-entrant pass runs only when reentry is allowed and "
               "recreation succeeded");

    // Failed / already-recovering passes skip the UpdateAlpha commit (no blank
    // or stale bitmap ever reaches the layered window).
    const size_t lost_if = badge.find("if (IsRecoverableDeviceLost(hr)) {");
    const size_t commit = badge.find("UpdateAlpha(current_alpha_);", lost_if);
    TEST_CHECK(lost_if != std::string::npos && commit != std::string::npos &&
                   lost_if < reentry && reentry < guard + 200 && guard < commit,
               "REQ-053 pin: the device-lost branch precedes the UpdateAlpha commit");
    TEST_CHECK(badge.find("return; // commit skipped: nothing valid to blit this pass") !=
                   std::string::npos,
               "REQ-053 pin: the skip-commit return on the failed/already-recovering pass is intact");

    // RecreateAfterDeviceLost body: two failures (no factory / CreateTarget
    // failed) report false; the success tail reallocates + reloads the logo
    // and reports true.
    const size_t recreate_def = badge.find("bool FloatingBadge::RecreateAfterDeviceLost() {");
    const size_t next_fn = badge.find("LRESULT CALLBACK FloatingBadge::WndProc", recreate_def);
    if (recreate_def != std::string::npos && next_fn != std::string::npos && next_fn > recreate_def) {
        const std::string body = badge.substr(recreate_def, next_fn - recreate_def);
        TEST_CHECK(Req053Count(body, "return false;") == 2,
                   "REQ-053 pin: both recreation-failure paths (null factory / CreateTarget) "
                   "report false");
        TEST_CHECK(Req053Count(body, "return true;") == 1 &&
                       body.find("ReallocateBuffer(PhysW(), PhysH());") != std::string::npos &&
                       body.find("LoadLogoBitmap();") != std::string::npos,
                   "REQ-053 pin: the success tail reallocates + reloads the logo and reports true");
    } else {
        TEST_CHECK(false, "REQ-053 pin: the RecreateAfterDeviceLost body is bounded by the WndProc");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-053 device-lost recovery pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-053 device-lost recovery pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (e) tooltip language-menu focus restore (A5) --------------------------

void TestReq053TooltipFocusRestorePins() {
    std::cout << "[RUN] Testing REQ-053 tooltip focus-restore pins..." << std::endl;
    const int failures_before = g_failed_count;

    std::string tip;
    Req053ReadSrc(L"src\\ui\\tooltip.cpp", tip);
    if (tip.empty()) {
        std::cout << "[SKIP] src/ui/tooltip.cpp not resolvable; REQ-053 focus pins skipped."
                  << std::endl;
        return;
    }

    // Two menus (source + target) -> two backups, two liveness-guarded
    // restores, two SetForegroundWindow(hwnd) takeovers.
    TEST_CHECK(Req053Count(tip, "HWND hwndPrev = ::GetForegroundWindow();") == 2,
               "REQ-053 pin: both language menus back up the foreground window");
    TEST_CHECK(Req053Count(tip, "::SetForegroundWindow(hwndPrev);") == 2,
               "REQ-053 pin: both menus restore the backed-up foreground window");
    TEST_CHECK(Req053Count(tip, "if (hwndPrev && hwndPrev != hwnd && ::IsWindow(hwndPrev)) {") == 2,
               "REQ-053 pin: the restore is gated on a live, non-self previous window "
               "(a window destroyed inside the modal loop is skipped)");
    TEST_CHECK(Req053Count(tip, "::SetForegroundWindow(hwnd);") == 2,
               "REQ-053 pin: exactly the two pre-existing menu takeovers remain");

    // Order on each site: backup -> SetForegroundWindow(hwnd) -> modal
    // TrackPopupMenuEx -> DestroyMenu -> guarded restore. (A restore before the
    // menu would hand focus straight back to the old window mid-menu.)
    const size_t b1 = tip.find("HWND hwndPrev = ::GetForegroundWindow();");
    const size_t t1 = tip.find("::TrackPopupMenuEx(", b1);
    const size_t r1 = tip.find("::SetForegroundWindow(hwndPrev);", b1);
    TEST_CHECK(b1 != std::string::npos && t1 != std::string::npos && r1 != std::string::npos &&
                   b1 < t1 && t1 < r1,
               "REQ-053 pin: menu site 1 backs up before the modal loop and restores after it");
    const size_t b2 = tip.find("HWND hwndPrev = ::GetForegroundWindow();", b1 + 1);
    const size_t t2 = tip.find("::TrackPopupMenuEx(", b2);
    const size_t r2 = tip.find("::SetForegroundWindow(hwndPrev);", b2);
    TEST_CHECK(b2 != std::string::npos && t2 != std::string::npos && r2 != std::string::npos &&
                   b1 < b2 && b2 < t2 && t2 < r2,
               "REQ-053 pin: menu site 2 follows the same backup -> menu -> restore order");
    TEST_CHECK(tip.find("::DestroyMenu(hMenu);", t1) < r1 && tip.find("::DestroyMenu(hMenu);", t2) < r2,
               "REQ-053 pin: the menu handle is destroyed before the focus restore "
               "(no restore inside the menu's ownership window)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-053 tooltip focus-restore pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-053 tooltip focus-restore pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (f) OpenAI fetch-button busy feedback (A6) ---------------------------

void TestReq053OpenAiButtonDisablePins() {
    std::cout << "[RUN] Testing REQ-053 OpenAI fetch-button pins..." << std::endl;
    const int failures_before = g_failed_count;

    std::string osw;
    Req053ReadSrc(L"src\\ui\\openai_settings_window.cpp", osw);
    if (osw.empty()) {
        std::cout << "[SKIP] src/ui/openai_settings_window.cpp not resolvable; REQ-053 A6 pins skipped."
                  << std::endl;
        return;
    }

    // The busy field lives on the dialog state (cpp-local struct) and tracks
    // the button's enabled state 1:1.
    TEST_CHECK(osw.find("bool fetch_button_busy = false;") != std::string::npos,
               "REQ-053 pin: OpenAiDialogState carries fetch_button_busy");
    TEST_CHECK(Req053Count(osw, "st->fetch_button_busy = true;") == 1 &&
                   Req053Count(osw, "st->fetch_button_busy = false;") == 2,
               "REQ-053 pin: the flag flips true once (spawn) and false twice (stale drop + done)");

    // One disable on the successful spawn (the site shared by the manual and
    // the debounced auto-fetch), and TWO restores: the stale-completion drop AND
    // the normal completion (the A6 report finding: a single-TRUE pin would
    // silently re-break the superseded-fetch path).
    const size_t disables = Req053Count(osw, "::EnableWindow(::GetDlgItem(dlg, IDC_FETCH_BTN), FALSE);");
    const size_t enables = Req053Count(osw, "::EnableWindow(::GetDlgItem(dlg, IDC_FETCH_BTN), TRUE);");
    TEST_CHECK(disables == 1,
               "REQ-053 pin: exactly one IDC_FETCH_BTN disable (the shared spawn site)");
    TEST_CHECK(enables >= 2,
               "REQ-053 pin: at least two IDC_FETCH_BTN restores (stale drop + normal completion)");

    // Ordering at the spawn site: the generation bump / spawn precede the gray-out.
    const size_t generation_bump = osw.find("++st->fetch_generation;");
    const size_t spawn = osw.find("OpenAiFetchWorker, st->fetch_alive, dlg, st->fetch_generation,");
    const size_t disable = osw.find("::EnableWindow(::GetDlgItem(dlg, IDC_FETCH_BTN), FALSE);");
    TEST_CHECK(generation_bump != std::string::npos && spawn != std::string::npos &&
                   disable != std::string::npos && generation_bump < spawn && spawn < disable,
               "REQ-053 pin: the gray-out follows the successful spawn (the declined-consent "
               "and failed-spawn paths return earlier and keep the button enabled)");

    // Stale-drop restore precedes the normal-completion restore; the latter
    // sits before the refill/notice so no failure box sits over a gray button.
    const size_t stale_restore = osw.find("::EnableWindow(::GetDlgItem(dlg, IDC_FETCH_BTN), TRUE);");
    const size_t done_restore = osw.find("::EnableWindow(::GetDlgItem(dlg, IDC_FETCH_BTN), TRUE);",
                                         stale_restore + 1);
    const size_t refill = osw.find("RefillOpenAiModelCombo(dlg, models);", done_restore);
    TEST_CHECK(stale_restore < done_restore && done_restore != std::string::npos &&
                   refill != std::string::npos && done_restore < refill,
               "REQ-053 pin: the completion restore precedes the combo refill");

    // No new i18n strings: the fix is graying feedback only (CPO scope). The
    // StringId tail is unchanged — still 110 ids, the REQ-051 record.
    volatile int enum_count = static_cast<int>(StringId::EnumCount);
    TEST_CHECK(enum_count == 110,
               "REQ-053 pin: StringId::EnumCount is still 110 (A6 added no strings)");
    TEST_CHECK(Req053Count(osw, "260922_0001 A6") >= 4,
               "REQ-053 pin: the A6 tag annotates the busy-feedback sites");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-053 OpenAI fetch-button pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-053 OpenAI fetch-button pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (g) GGUF manager action-button sync (A7) -----------------------------

void TestReq053GgufButtonSyncPins() {
    std::cout << "[RUN] Testing REQ-053 GGUF manager button-sync pins..." << std::endl;
    const int failures_before = g_failed_count;

    std::string gguf;
    Req053ReadSrc(L"src\\ui\\gguf_model_manager_window.cpp", gguf);
    if (gguf.empty()) {
        std::cout << "[SKIP] src/ui/gguf_model_manager_window.cpp not resolvable; REQ-053 A7 pins skipped."
                  << std::endl;
        return;
    }

    // Helper declared + defined (the forward declaration exists because the
    // rebuild function below it re-syncs).
    TEST_CHECK(Req053Count(gguf, "void SyncManagerActionButtons(HWND dlg, ManagerDialogState* st);") == 1,
               "REQ-053 pin: SyncManagerActionButtons is forward-declared");
    TEST_CHECK(Req053Count(gguf, "void SyncManagerActionButtons(HWND dlg, ManagerDialogState* st) {") == 1,
               "REQ-053 pin: SyncManagerActionButtons is defined");
    TEST_CHECK(gguf.find("const bool enabled = SelectedUserIndex(dlg, st) >= 0;") != std::string::npos,
               "REQ-053 pin: the pair follows the list selection (single source of truth)");
    TEST_CHECK(Req053Count(gguf, "::EnableWindow(b, enabled ? TRUE : FALSE);") == 2,
               "REQ-053 pin: both IDC_MGR_RENAME and IDC_MGR_DELETE are driven by the helper");

    // Three call sites: post-rebuild, WM_INITDIALOG, LBN_SELCHANGE.
    const size_t calls = Req053Count(gguf, "SyncManagerActionButtons(dlg, st);");
    TEST_CHECK(calls == 3,
               "REQ-053 pin: exactly three sync call sites (rebuild + init + selchange)");

    // The LBN_SELCHANGE branch exists and ends in a sync (Ctrl+click
    // deselection -> SelectedUserIndex -1 -> both buttons gray out).
    const size_t lbn = gguf.find("if (HIWORD(wp) == LBN_SELCHANGE) {");
    const size_t lbn_sync = gguf.find("SyncManagerActionButtons(dlg, st);", lbn);
    const size_t lbn_next_case = gguf.find("case IDC_MGR_RENAME:", lbn);
    TEST_CHECK(gguf.find("case IDC_MGR_LIST:", 0) != std::string::npos && lbn != std::string::npos &&
                   lbn_sync != std::string::npos && lbn_next_case != std::string::npos &&
                   lbn < lbn_sync && lbn_sync < lbn_next_case,
               "REQ-053 pin: IDC_MGR_LIST handles LBN_SELCHANGE by syncing the action pair");

    // Post-rebuild sync: inside RebuildManagerList, after LB_SETCURSEL (which is
    // silent — it posts no LBN_SELCHANGE, so the explicit sync is the only
    // re-enable after a rebuild).
    const size_t rebuild_def = gguf.find("void RebuildManagerList(HWND dlg, ManagerDialogState* st) {");
    const size_t setcursel = gguf.find("::SendMessageW(list, LB_SETCURSEL, 0, 0);", rebuild_def);
    const size_t rebuild_sync = gguf.find("SyncManagerActionButtons(dlg, st);", rebuild_def);
    const size_t selected_def = gguf.find("int SelectedUserIndex(HWND dlg, const ManagerDialogState* st) {",
                                          rebuild_def);
    TEST_CHECK(rebuild_def != std::string::npos && setcursel != std::string::npos &&
                   rebuild_sync != std::string::npos && selected_def != std::string::npos &&
                   setcursel < rebuild_sync && rebuild_sync < selected_def,
               "REQ-053 pin: RebuildManagerList re-syncs after the auto-select (LB_SETCURSEL is silent)");

    // WM_INITDIALOG syncs right after its rebuild, before WM_COMMAND starts.
    // Anchored on the MANAGER proc (the file also carries RenameModelProc /
    // HF-download procs with their own WM_INITDIALOG cases).
    const size_t mgr_proc = gguf.find("INT_PTR CALLBACK GgufManagerProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {");
    const size_t init_case = gguf.find("case WM_INITDIALOG: {", mgr_proc);
    const size_t init_rebuild = gguf.find("RebuildManagerList(dlg, st);", init_case);
    const size_t init_sync = gguf.find("SyncManagerActionButtons(dlg, st);", init_case);
    const size_t wm_command = gguf.find("case WM_COMMAND:", init_case);
    TEST_CHECK(mgr_proc != std::string::npos && init_case != std::string::npos &&
                   init_rebuild != std::string::npos && init_sync != std::string::npos &&
                   wm_command != std::string::npos && mgr_proc < init_case &&
                   init_rebuild < init_sync && init_sync < wm_command,
               "REQ-053 pin: the manager WM_INITDIALOG rebuilds then syncs before WM_COMMAND");

    // The rename/delete click guards stay as defensive `idx < 0` returns (the
    // gray-out is the real gate; the handlers are unreachable by click).
    TEST_CHECK(gguf.find("case IDC_MGR_RENAME: {") != std::string::npos &&
                   gguf.find("case IDC_MGR_DELETE: {") != std::string::npos,
               "REQ-053 pin: both action handlers remain present");
    TEST_CHECK(Req053Count(gguf, "// 260922_0001 A7: defensive guard") == 2,
               "REQ-053 pin: both handlers annotate the guard as defensive");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-053 GGUF manager button-sync pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-053 GGUF manager button-sync pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (h) sound queue policy + single worker (A8) --------------------------

void TestReq053SoundQueuePolicy() {
    std::cout << "[RUN] Testing REQ-053 sound queue policy..." << std::endl;
    const int failures_before = g_failed_count;

    // Saturation bound + boundary semantics: the queue admits while there is
    // room (size < depth) and sheds the oldest only AT/OVER the bound.
    volatile std::size_t depth = kMaxQueueDepth;
    TEST_CHECK(depth == 8,
               "REQ-053: kMaxQueueDepth is 8");
    TEST_CHECK(SoundEnqueuePolicy(0, kMaxQueueDepth) == SoundEnqueueDecision::kEnqueue &&
                   SoundEnqueuePolicy(7, kMaxQueueDepth) == SoundEnqueueDecision::kEnqueue,
               "REQ-053: 7 queued tones still enqueue (one free slot)");
    TEST_CHECK(SoundEnqueuePolicy(8, kMaxQueueDepth) == SoundEnqueueDecision::kDropOldestEnqueue,
               "REQ-053: a full queue (8) drops the oldest before enqueueing");
    TEST_CHECK(SoundEnqueuePolicy(9, kMaxQueueDepth) == SoundEnqueueDecision::kDropOldestEnqueue,
               "REQ-053: over-saturation stays in the drop-oldest branch");
    TEST_CHECK(SoundEnqueuePolicy(0, 0) == SoundEnqueueDecision::kDropOldestEnqueue,
               "REQ-053: a zero depth takes the drop branch (no division / no crash)");

    // Drop-oldest simulation mirroring PlaySoundAsync's g_queue operations: the
    // freshest feedback always lands, the stalest waiting tone is the one lost,
    // and the resident queue never exceeds the bound.
    std::deque<int> q;
    bool sim_ok = true;
    for (int tone = 1; tone <= 12; ++tone) {
        const std::size_t size_before = q.size();
        const bool drop = SoundEnqueuePolicy(size_before, kMaxQueueDepth) ==
                          SoundEnqueueDecision::kDropOldestEnqueue;
        if (drop) {
            if (q.empty()) sim_ok = false; // nothing to shed: policy/state mismatch
            q.pop_front();
        }
        q.push_back(tone);
        if (q.size() > kMaxQueueDepth) sim_ok = false;
    }
    TEST_CHECK(sim_ok,
               "REQ-053: the drop-oldest sequence keeps the queue at or below the bound");
    TEST_CHECK(q.size() == 8 && q.front() == 5 && q.back() == 12,
               "REQ-053: after 12 rapid tones the queue holds 5..12 (the four stalest were shed)");

    std::string snd;
    Req053ReadSrc(L"src\\sound.cpp", snd);
    std::string main_src;
    Req053ReadSrc(L"src\\main.cpp", main_src);
    if (snd.empty()) {
        std::cout << "[SKIP] src/sound.cpp not resolvable; REQ-053 sound pins skipped." << std::endl;
        return;
    }

    // kToneTable contract re-pin (CPO rank 8: tone/frequency/duration must not
    // move): size-4 table, both static_asserts, and the four byte-identical
    // rows.
    TEST_CHECK(snd.find("constexpr std::array<ToneSpec, 4> kToneTable = {{") != std::string::npos,
               "REQ-053 pin: kToneTable is still exactly 4 rows");
    TEST_CHECK(snd.find("static_assert(kToneTable.size() == static_cast<std::size_t>(SoundType::ToggleAutoSend) + 1,") !=
                   std::string::npos &&
                   snd.find("static_assert(static_cast<std::size_t>(SoundType::Enable) == 0,") !=
                       std::string::npos,
               "REQ-053 pin: both kToneTable static_asserts are intact");
    TEST_CHECK(snd.find("{1000, 100},") != std::string::npos &&
                   snd.find("{400, 120},") != std::string::npos &&
                   snd.find("{800, 80},") != std::string::npos &&
                   snd.find("{1200, 80},") != std::string::npos,
               "REQ-053 pin: the four tone rows are byte-identical (Enable/Disable/CycleLang/ToggleAutoSend)");

    // The detached per-beep thread pattern is gone: no std::jthread(...)
    // construction and no detach() in code; exactly ONE std::thread worker,
    // created through std::call_once.
    TEST_CHECK(Req053Count(snd, "std::jthread(") == 0 && Req053Count(snd, ".detach()") == 0,
               "REQ-053 pin: the detached per-beep std::jthread pattern is fully removed");
    TEST_CHECK(Req053Count(snd, "std::thread(") == 1 &&
                   snd.find("std::call_once(g_worker_once, [] {") != std::string::npos,
               "REQ-053 pin: exactly one resident worker thread, created once");
    TEST_CHECK(snd.find("::Beep(tone.freq, tone.duration);") != std::string::npos &&
                   snd.find("g_queue_cv.wait(lock, [] {") != std::string::npos,
               "REQ-053 pin: the worker owns the blocking Beep behind the queue condition variable");
    TEST_CHECK(snd.find("SoundEnqueuePolicy(g_queue.size(), kMaxQueueDepth) ==") != std::string::npos &&
                   snd.find("g_queue.pop_front();") != std::string::npos &&
                   snd.find("g_queue.push_back(tone);") != std::string::npos,
               "REQ-053 pin: PlaySoundAsync enqueues through the pure policy with drop-oldest");
    TEST_CHECK(snd.find("g_worker.join();") != std::string::npos &&
                   snd.find("g_stop.store(true, std::memory_order_relaxed);") != std::string::npos,
               "REQ-053 pin: ShutdownSound latches stop and joins the worker");

    if (main_src.empty()) {
        std::cout << "[SKIP] src/main.cpp not resolvable; REQ-053 shutdown wiring pin skipped."
                  << std::endl;
    } else {
        TEST_CHECK(main_src.find("emebalachat::ShutdownSound();") != std::string::npos,
                   "REQ-053 pin: main.cpp's clean-shutdown path calls ShutdownSound()");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-053 sound queue policy passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-053 sound queue policy: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
