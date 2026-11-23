// ---------------------------------------------------------------------------
// V10F (v0.10.10 phased fixes, P1-U2): badge single-click timer restoration
// (BUG-01) + DPI-aware double-click tolerance (D2). Staged as an .inc next to
// this runner; registered near the end of main() per the end-of-file pattern.
//
// Coverage:
//   (a) TestP1U2BadgeSingleClickTimerArm — badge.cpp pins: WM_LBUTTONUP arms
//       kTimerSingleClick via SetTimer sized to single_click_delay_ms_
//       (BUG-01 Option A: the immediate click_callback_() call made a badge
//       double-click (language swap) first fire pause/cancel before
//       WM_LBUTTONDBLCLK could run); WM_LBUTTONDBLCLK still KillTimers it;
//       WM_TIMER still dispatches click_callback_; the is_dragging_ branch
//       keeps its position_callback_ path and precedes the arm (drag
//       handling untouched).
//   (b) TestP1U2MouseDoubleClickDpiTolerance — mouse_hook.cpp pins: the
//       WM_LBUTTONDOWN double-click tolerance comes from the shared cached
//       GetSystemMetricsForDpi helper resolved for the press point's monitor
//       DPI (D2 Option A); no DPI-unaware ::GetSystemMetrics(SM_CXDOUBLECLK/
//       SM_CYDOUBLECLK) read remains on the hook path; the drag threshold
//       keeps its DPI-aware ISSUE-013 resolution through the same helper; the
//       pure DoubleClickToleranceForDpi factor keeps the Win32 read injected
//       (both axes, one dpi, no clamping).
//
// NOTE (ownership): mouse_hook.hpp is NOT owned by P1-U2 (brief: badge.cpp/
// hpp + mouse_hook.cpp only), so the pure tolerance factor stays file-local
// in mouse_hook.cpp's anonymous namespace and cannot be linked into this TU;
// like the req057b/req058 no-seam suites these are ResolveRepoFile source
// pins rather than direct calls.
//
// Source pins read through ResolveRepoFile + raw-byte find() (CRLF-
// normalized, the req052 precedent; every needle is ASCII-only).
// ---------------------------------------------------------------------------

namespace {

// Resolve + read a production source for the P1-U2 structural pins (same
// candidate-path precedent as the req052 reader; CRLF-normalized so
// line-anchored needles never depend on the checkout's autocrlf).
void P1U2ReadSrc(const wchar_t* rel, std::string& out) {
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

} // namespace

// ---- (a) badge single-click timer arm (V10F BUG-01) -----------------------

void TestP1U2BadgeSingleClickTimerArm() {
    std::cout << "[RUN] Testing P1-U2 badge single-click timer arm (V10F BUG-01)..." << std::endl;
    const int failures_before = g_failed_count;

    std::string badge;
    P1U2ReadSrc(L"src\\ui\\badge.cpp", badge);
    if (badge.empty()) {
        std::cout << "[SKIP] badge.cpp not resolvable; P1-U2 badge pins skipped." << std::endl;
        return;
    }

    // The regression (261104_0001 UI-BUG-3): WM_LBUTTONUP invoked
    // click_callback_() directly while the surrounding comment still said the
    // single-click timer was armed, so a double-click (language swap) first
    // toggled pause/cancel before WM_LBUTTONDBLCLK could KillTimer it. The
    // fix re-arms kTimerSingleClick sized to single_click_delay_ms_.
    const size_t btn_up = badge.find("case WM_LBUTTONUP: {");
    const size_t btn_up_end =
        (btn_up != std::string::npos) ? badge.find("case WM_LBUTTONDBLCLK", btn_up) : std::string::npos;
    TEST_CHECK(btn_up != std::string::npos && btn_up_end != std::string::npos &&
                   btn_up_end > btn_up,
               "P1-U2 pin: the WM_LBUTTONUP case body is locatable");
    std::string up_body;
    if (btn_up != std::string::npos && btn_up_end != std::string::npos && btn_up_end > btn_up) {
        up_body = badge.substr(btn_up, btn_up_end - btn_up);
    }

    const size_t arm = up_body.find(
        "::SetTimer(hwnd, kTimerSingleClick, self->single_click_delay_ms_, nullptr)");
    TEST_CHECK(arm != std::string::npos,
               "P1-U2 pin: WM_LBUTTONUP arms kTimerSingleClick sized to single_click_delay_ms_ "
               "(BUG-01 Option A)");
    TEST_CHECK(up_body.find("self->click_callback_();") == std::string::npos,
               "P1-U2 pin: WM_LBUTTONUP no longer fires click_callback_ immediately "
               "(the double-click window gets the first claim)");

    // Drag handling unchanged: the is_dragging_ branch still owns the
    // position_callback_ hand-off and precedes the click-in-place arm.
    const size_t drag_branch = up_body.find("if (self->is_dragging_) {");
    const size_t click_branch = up_body.find("} else if (self->is_mouse_down_) {");
    TEST_CHECK(drag_branch != std::string::npos && click_branch != std::string::npos &&
                   drag_branch < click_branch && click_branch < arm,
               "P1-U2 pin: the drag branch precedes the click-in-place timer arm "
               "(is_dragging_ path untouched)");

    // The double-click side of the contract still cancels the pending
    // single-click before firing its own callback.
    const size_t dblclk = badge.find("case WM_LBUTTONDBLCLK");
    const size_t dblclk_end =
        (dblclk != std::string::npos) ? badge.find("case WM_RBUTTONUP", dblclk) : std::string::npos;
    const size_t dbl_kill = (dblclk != std::string::npos)
                                ? badge.find("::KillTimer(hwnd, kTimerSingleClick)", dblclk)
                                : std::string::npos;
    TEST_CHECK(dblclk != std::string::npos && dblclk_end != std::string::npos &&
                   dblclk_end > dblclk && dbl_kill != std::string::npos && dbl_kill < dblclk_end,
               "P1-U2 pin: WM_LBUTTONDBLCLK still KillTimers the pending single-click");

    // And the deferred dispatch still happens exactly once, on WM_TIMER.
    const size_t timer_case = badge.find("case WM_TIMER");
    const size_t timer_single =
        (timer_case != std::string::npos) ? badge.find("wParam == kTimerSingleClick", timer_case)
                                          : std::string::npos;
    const size_t timer_fire =
        (timer_case != std::string::npos) ? badge.find("self->click_callback_();", timer_case)
                                          : std::string::npos;
    TEST_CHECK(timer_case != std::string::npos && timer_single != std::string::npos &&
                   timer_fire != std::string::npos && timer_single < timer_fire,
               "P1-U2 pin: WM_TIMER dispatches click_callback_ on kTimerSingleClick");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P1-U2 badge single-click timer arm pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] P1-U2 badge single-click timer arm pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (b) DPI-aware double-click tolerance (V10F D2) -----------------------

void TestP1U2MouseDoubleClickDpiTolerance() {
    std::cout << "[RUN] Testing P1-U2 DPI-aware double-click tolerance (V10F D2)..." << std::endl;
    const int failures_before = g_failed_count;

    std::string mcpp;
    P1U2ReadSrc(L"src\\mouse_hook.cpp", mcpp);
    if (mcpp.empty()) {
        std::cout << "[SKIP] mouse_hook.cpp not resolvable; P1-U2 mouse pins skipped." << std::endl;
        return;
    }

    // D2 Option A: the press gate's tolerance rect comes from the shared
    // cached GetSystemMetricsForDpi helper, resolved for the press point's
    // monitor DPI (the same MonitorDpiAtPoint discipline as the drag
    // threshold). The DPI-unaware reads must be gone.
    TEST_CHECK(mcpp.find("::GetSystemMetrics(SM_CXDOUBLECLK)") == std::string::npos &&
                   mcpp.find("::GetSystemMetrics(SM_CYDOUBLECLK)") == std::string::npos,
               "P1-U2 pin: no DPI-unaware SM_CXDOUBLECLK/SM_CYDOUBLECLK read remains");
    TEST_CHECK(mcpp.find("DoubleClickToleranceForPress(ms->pt)") != std::string::npos,
               "P1-U2 pin: the WM_LBUTTONDOWN gate resolves the tolerance for the press point");

    // The shared resolution: one GetProcAddress-backed cached
    // GetSystemMetricsForDpi pointer serves BOTH the drag threshold and the
    // double-click tolerance, with the DPI-unaware fallback preserved for
    // older hosts.
    TEST_CHECK(mcpp.find("GetProcAddress(user32, \"GetSystemMetricsForDpi\")") != std::string::npos,
               "P1-U2 pin: GetSystemMetricsForDpi is still resolved through GetProcAddress "
               "(older-host fallback intact)");
    TEST_CHECK(mcpp.find("SystemMetricForDpiOrFallback(SM_CXDRAG, dpi)") != std::string::npos &&
                   mcpp.find("SystemMetricForDpiOrFallback(SM_CYDRAG, dpi)") != std::string::npos,
               "P1-U2 pin: the drag threshold keeps its DPI-aware ISSUE-013 resolution "
               "through the shared helper");
    TEST_CHECK(mcpp.find("MouseHook::DragThresholdFromMetrics(") != std::string::npos,
               "P1-U2 pin: the drag threshold still composes through the pure combine helper");

    // The pure factor: Win32 read injected, both axes read for the same dpi,
    // no clamping (a 0 metric must pass through untouched).
    TEST_CHECK(mcpp.find("DoubleClickToleranceForDpi(int (*metric_for_dpi)(int, UINT), UINT dpi)") !=
                   std::string::npos,
               "P1-U2 pin: the tolerance resolution keeps its pure injected-metric shape");
    TEST_CHECK(mcpp.find("metric_for_dpi(SM_CXDOUBLECLK, dpi)") != std::string::npos &&
                   mcpp.find("metric_for_dpi(SM_CYDOUBLECLK, dpi)") != std::string::npos,
               "P1-U2 pin: both tolerance axes read through the injected resolver "
               "for the same dpi");

    // The DPI-aware tolerance feeds the historical comparison unchanged.
    const size_t down_branch = mcpp.find("if (wParam == WM_LBUTTONDOWN) {");
    TEST_CHECK(down_branch != std::string::npos, "P1-U2 pin: the press branch is locatable");
    if (down_branch != std::string::npos) {
        const size_t tol = mcpp.find("DoubleClickToleranceForPress(ms->pt)", down_branch);
        const size_t cmp = mcpp.find("d_last_y <= max_dbl_cy", down_branch);
        TEST_CHECK(tol != std::string::npos && cmp != std::string::npos && tol < cmp,
                   "P1-U2 pin: the DPI-aware tolerance precedes the double-click comparison");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P1-U2 DPI-aware double-click tolerance pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] P1-U2 DPI-aware double-click tolerance pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
