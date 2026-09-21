// ---------------------------------------------------------------------------
// REQ-052 (session 260922, UI defect bundle): the tooltip header overlap
// clamp, the badge DPI-drag re-render / drag-threshold fallback / single-
// click delay, the about-window text clip + reset confirm, and the pen/touch
// injection filter. Staged as an .inc next to this runner; registered near
// the end of main() after TestReq051DecodeWallClockSemantics(), per the
// end-of-file pattern.
//
// Coverage:
//   (a) TestReq052HeaderTargetButtonPlan — the pure TooltipWindow::
//       PlanHeaderTargetButton planner (tooltip.hpp): the landed compile-time
//       static_asserts + runtime pins + an invariant sweep (right edge never
//       reaches the close button, width never below the 40 DIP minimum, the
//       start never right of the pull-left limit).
//   (b) TestReq052TooltipHeaderPins — structural pins on src/ui/tooltip.cpp:
//       the C2 site-7 target-pill measurement, Render consuming the planner
//       (the old hardcoded 90 DIP box is gone), and the close-before-target
//       hover order.
//   (c) TestReq052BadgeDpiDragReRender — badge.cpp: the dpiChanged flag,
//       ReallocateBuffer BEFORE SetWindowPos (size applied, not SWP_NOSIZE),
//       and the full Render() restoring the destroyed DIB contents.
//   (d) TestReq052BadgeDragThresholdFallback — badge.cpp: SM_CXDRAG/SM_CYDRAG
//       with the <= 0 -> 3px fallback (a registry-set 0 metric must not turn
//       click tremor into drags).
//   (e) TestReq052BadgeSingleClickUsesDoubleClickTime — badge.hpp/.cpp: the
//       fixed 220 ms constant is gone; the delay is captured once from
//       GetDoubleClickTime() and drives the kTimerSingleClick timer.
//   (f) TestReq052AboutWindowFixes — about_window.hpp/.cpp: CLIP on the 3
//       feature draws, the MB_YESNO reset confirm, and the confirm_pending_
//       guards on both dismissal paths (kDismissMessage + WM_KILLFOCUS).
//   (g) TestReq052TouchFilterPredicate — the pure MouseHook::IsFromTouchInjection
//       predicate (mouse_hook.hpp; the header is already linked into this
//       runner) + structural pins on the two drag-gate guards in
//       mouse_hook.cpp (EXTRA_INFO_MARKER self-filter untouched).
//
// Source pins read through ResolveRepoFile + raw-byte find() (the
// TestReq048CloudFailureSignaling precedent; every needle is ASCII-only).
// ---------------------------------------------------------------------------

namespace {

// Resolve + read a production source for the REQ-052 structural pins (same
// candidate-path precedent as the req047/req051 readers; CRLF-normalized so
// line-anchored needles never depend on the checkout's autocrlf).
void Req052ReadSrc(const wchar_t* rel, std::string& out) {
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

// ---- (a) the pure header target-button planner ----------------------------

void TestReq052HeaderTargetButtonPlan() {
    std::cout << "[RUN] Testing REQ-052 header target-button plan..." << std::endl;
    const int failures_before = g_failed_count;
    // (No using-declarations: MSVC C2885 rejects class-member using at
    // namespace scope — qualify every reference explicitly.)

    // Constants (volatile pins: direct constant comparisons trip C4127).
    volatile float gap = TooltipWindow::kHeaderCloseGapDip;
    volatile float min_w = TooltipWindow::kHeaderTargetMinWidthDip;
    TEST_CHECK(gap == 8.0f, "REQ-052: the header close gap is 8 DIP");
    TEST_CHECK(min_w == 40.0f, "REQ-052: the header target minimum width is 40 DIP");

    // The four landed static_assert scenarios, re-pinned at runtime.
    const auto nominal = TooltipWindow::PlanHeaderTargetButton(44.0f, 60.0f, 360.0f, 90.0f);
    TEST_CHECK(nominal.x == 132.0f && nominal.width == 90.0f,
               "REQ-052: nominal source tag keeps the natural start (44+60+28) "
               "and the old 90 DIP width");
    const auto clamped = TooltipWindow::PlanHeaderTargetButton(44.0f, 165.0f, 360.0f, 90.0f);
    TEST_CHECK(clamped.x == 237.0f && clamped.width == 83.0f,
               "REQ-052: wide source tag clamps the width so the right edge "
               "lands exactly 8 DIP clear of the close button (the P1 overlap)");
    const auto pulled = TooltipWindow::PlanHeaderTargetButton(44.0f, 250.0f, 360.0f, 90.0f);
    TEST_CHECK(pulled.x == 280.0f && pulled.width == 40.0f,
               "REQ-052: extreme source tag pulls the start left so the 40 DIP "
               "minimum still fits (button stays clickable)");
    const auto floored = TooltipWindow::PlanHeaderTargetButton(44.0f, 60.0f, 360.0f, 20.0f);
    TEST_CHECK(floored.width == 40.0f,
               "REQ-052: a sub-minimum desired width floors at 40 DIP");

    // The exact P1 geometry: the pre-fix box ran to 44+165+28+90 = 327 DIP vs
    // the close button at 360-32 = 328 DIP (1 DIP overlap -> the language menu
    // sat on top of the dismiss control). The plan must now keep right+8 <=
    // close_left.
    const auto p1 = TooltipWindow::PlanHeaderTargetButton(44.0f, 165.0f, 360.0f, 90.0f);
    TEST_CHECK(p1.x + p1.width + 8.0f <= 360.0f - 32.0f + 1e-6f,
               "REQ-052: the German-wide source-tag case now clears the close "
               "button by the full 8 DIP gap");

    // Invariant sweep over a grid of source tags, card widths, and desired
    // widths: (1) right edge + gap <= close left, (2) width >= the minimum,
    // (3) x never right of close_left - (min + gap), (4) when not pulled the
    // start is the natural src_tag_x + src_tag_width + 28.
    bool inv_ok = true;
    const float src_xs[] = {0.0f, 20.0f, 44.0f};
    const float src_ws[] = {0.0f, 60.0f, 165.0f, 250.0f, 400.0f};
    const float card_ws[] = {200.0f, 360.0f, 640.0f};
    const float desires[] = {0.0f, 20.0f, 40.0f, 90.0f, 200.0f, 1000.0f};
    for (float sx : src_xs) {
        for (float sw : src_ws) {
            for (float cw : card_ws) {
                for (float dw : desires) {
                    const auto p = TooltipWindow::PlanHeaderTargetButton(sx, sw, cw, dw);
                    const float close_left = cw - 32.0f;
                    if (!(p.x + p.width <= close_left - TooltipWindow::kHeaderCloseGapDip + 1e-4f))
                        inv_ok = false;
                    if (!(p.width >= TooltipWindow::kHeaderTargetMinWidthDip - 1e-6f)) inv_ok = false;
                    if (!(p.x <= close_left - (TooltipWindow::kHeaderTargetMinWidthDip + TooltipWindow::kHeaderCloseGapDip) + 1e-4f))
                        inv_ok = false;
                    const float natural = sx + sw + 28.0f;
                    if (natural <= close_left - (TooltipWindow::kHeaderTargetMinWidthDip + TooltipWindow::kHeaderCloseGapDip) &&
                        p.x != natural) {
                        inv_ok = false;
                    }
                }
            }
        }
    }
    TEST_CHECK(inv_ok,
               "REQ-052: the planner invariants hold across the sweep "
               "(right edge clears the close button, width floors at 40 DIP, "
               "start pulls left only when the natural start would not fit)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-052 header target-button plan passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-052 header target-button plan: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (b) tooltip.cpp structural pins ---------------------------------------

void TestReq052TooltipHeaderPins() {
    std::cout << "[RUN] Testing REQ-052 tooltip header structural pins..." << std::endl;
    const int failures_before = g_failed_count;

    std::string tip;
    Req052ReadSrc(L"src\\ui\\tooltip.cpp", tip);
    if (tip.empty()) {
        std::cout << "[SKIP] src/ui/tooltip.cpp not resolvable; REQ-052 header pins skipped."
                  << std::endl;
        return;
    }

    // C2 site 7: the target-pill measurement joined the cache (floor == the
    // pre-REQ-052 90 DIP box; the label is the display name + L" ▾").
    TEST_CHECK(tip.find("site 7 (REQ-052): target-language pill width") != std::string::npos,
               "REQ-052 pin: the C2 cache gained the target-pill measurement site");
    TEST_CHECK(tip.find("float tgt_w = 90.0f; // floor == the pre-REQ-052 hardcoded box") !=
                   std::string::npos,
               "REQ-052 pin: the target-pill floor preserves the old 90 DIP box");
    TEST_CHECK(tip.find("tgt_btn_w_ = tgt_w;") != std::string::npos &&
                   tip.find("tgt_label_key_ = tgt_label;") != std::string::npos,
               "REQ-052 pin: the target width + label key are cached like the other sites");

    // Render consumes the pure planner; the old hardcoded box is gone.
    TEST_CHECK(tip.find("const HeaderTargetButtonPlan tgt_plan = PlanHeaderTargetButton(") !=
                   std::string::npos,
               "REQ-052 pin: Render plans the target rect through the pure planner");
    TEST_CHECK(tip.find("lang_btn_rect_ = D2D1::RectF(tgt_plan.x, 8.0f, "
                        "tgt_plan.x + tgt_plan.width, 30.0f);") != std::string::npos,
               "REQ-052 pin: lang_btn_rect_ derives from the plan (single source of truth)");
    TEST_CHECK(tip.find("float tgt_btn_width = 90.0f;") == std::string::npos &&
                   tip.find("float tgt_btn_x = src_tag_x + src_tag_width + 28.0f;") ==
                       std::string::npos,
               "REQ-052 pin: the hardcoded 90 DIP target box is fully replaced");

    // Close-before-target hover order (on any residual overlap the dismiss
    // control wins, matching the WM_LBUTTONUP click order).
    const size_t hover_close = tip.find("IsPointInRect(pThis->close_btn_rect_, x, y)) new_hover = 4");
    const size_t hover_target = tip.find("IsPointInRect(pThis->lang_btn_rect_, x, y)) new_hover = 3");
    TEST_CHECK(hover_close != std::string::npos && hover_target != std::string::npos &&
                   hover_close < hover_target,
               "REQ-052 pin: the hover order is close (4) BEFORE target (3)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-052 tooltip header pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-052 tooltip header pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (c) badge cross-DPI drag re-render ------------------------------------

void TestReq052BadgeDpiDragReRender() {
    std::cout << "[RUN] Testing REQ-052 badge cross-DPI drag re-render pins..." << std::endl;
    const int failures_before = g_failed_count;

    std::string badge;
    Req052ReadSrc(L"src\\ui\\badge.cpp", badge);
    if (badge.empty()) {
        std::cout << "[SKIP] src/ui/badge.cpp not resolvable; REQ-052 badge pins skipped."
                  << std::endl;
        return;
    }

    // The DPI-change flag tracks the ReallocateBuffer (which destroys the DIB).
    TEST_CHECK(badge.find("bool dpiChanged = false;") != std::string::npos,
               "REQ-052 pin: the drag path tracks whether the DPI changed");
    const size_t realloc_pos = badge.find("self->ReallocateBuffer(self->PhysW(), self->PhysH());");
    const size_t flag_pos = badge.find("dpiChanged = true;");
    TEST_CHECK(realloc_pos != std::string::npos && flag_pos != std::string::npos &&
                   realloc_pos < flag_pos,
               "REQ-052 pin: ReallocateBuffer sets dpiChanged (the DIB was destroyed)");

    // The new physical size is actually applied: SWP_NOSIZE drops out exactly
    // when the DPI changed (the old unconditional SWP_NOSIZE left the ghost
    // pill at the old scale).
    TEST_CHECK(badge.find("SWP_NOACTIVATE | (dpiChanged ? 0 : SWP_NOSIZE)") != std::string::npos,
               "REQ-052 pin: SetWindowPos applies the size when the DPI changed");

    // Full render restores the destroyed DIB contents, after the position set.
    const size_t setwindowpos_pos = badge.find("SWP_NOACTIVATE | (dpiChanged ? 0 : SWP_NOSIZE)");
    const size_t rerender_pos = badge.find("if (dpiChanged) {");
    TEST_CHECK(setwindowpos_pos != std::string::npos && rerender_pos != std::string::npos &&
                   setwindowpos_pos < rerender_pos &&
                   badge.find("self->Render();", rerender_pos) != std::string::npos,
               "REQ-052 pin: a DPI change re-renders after SetWindowPos "
               "(mirrors the WM_DPICHANGED house pattern)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-052 badge DPI-drag re-render pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-052 badge DPI-drag re-render pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (d) badge drag-threshold zero fallback --------------------------------

void TestReq052BadgeDragThresholdFallback() {
    std::cout << "[RUN] Testing REQ-052 badge drag-threshold fallback pins..." << std::endl;
    const int failures_before = g_failed_count;

    std::string badge;
    Req052ReadSrc(L"src\\ui\\badge.cpp", badge);
    if (badge.empty()) {
        std::cout << "[SKIP] src/ui/badge.cpp not resolvable; REQ-052 threshold pins skipped."
                  << std::endl;
        return;
    }

    // The system metric replaces the old fixed 4 px threshold, with the
    // historical 3 px fallback when the registry-set metric is 0 (abs(d) >= 0
    // would otherwise turn click tremor into drags and swallow every click).
    TEST_CHECK(badge.find("int dragX = ::GetSystemMetrics(SM_CXDRAG);") != std::string::npos &&
                   badge.find("int dragY = ::GetSystemMetrics(SM_CYDRAG);") != std::string::npos,
               "REQ-052 pin: the drag threshold reads the system SM_CXDRAG/SM_CYDRAG metrics");
    TEST_CHECK(badge.find("if (dragX <= 0) dragX = 3;") != std::string::npos &&
                   badge.find("if (dragY <= 0) dragY = 3;") != std::string::npos,
               "REQ-052 pin: a zero/unavailable metric falls back to the historical 3 px");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-052 badge drag-threshold fallback pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-052 badge drag-threshold fallback pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (e) badge single-click delay from GetDoubleClickTime -------------------

void TestReq052BadgeSingleClickUsesDoubleClickTime() {
    std::cout << "[RUN] Testing REQ-052 badge single-click delay pins..." << std::endl;
    const int failures_before = g_failed_count;

    std::string badge_h;
    Req052ReadSrc(L"src\\ui\\badge.hpp", badge_h);
    std::string badge;
    Req052ReadSrc(L"src\\ui\\badge.cpp", badge);
    if (badge_h.empty() || badge.empty()) {
        std::cout << "[SKIP] badge.hpp/.cpp not resolvable; REQ-052 click-delay pins skipped."
                  << std::endl;
        return;
    }

    // The fixed 220 ms constant is gone (it fired the pause toggle ahead of a
    // slow double-click); the delay is a member captured at Create.
    TEST_CHECK(badge_h.find("kSingleClickDelayMs") == std::string::npos,
               "REQ-052 pin: the fixed kSingleClickDelayMs constant is retired");
    TEST_CHECK(badge_h.find("UINT single_click_delay_ms_ = 220;") != std::string::npos,
               "REQ-052 pin: the delay is an instance member (default 220, overwritten at Create)");

    // Create() captures the OS double-click window once; the click-in-place
    // timer uses the captured value.
    TEST_CHECK(badge.find("single_click_delay_ms_ = ::GetDoubleClickTime();") != std::string::npos,
               "REQ-052 pin: Create() captures GetDoubleClickTime() (the OS double-click window)");
    TEST_CHECK(badge.find("::SetTimer(hwnd, kTimerSingleClick, self->single_click_delay_ms_, nullptr);") !=
                   std::string::npos,
               "REQ-052 pin: the single-click timer is sized to the captured double-click window");
    TEST_CHECK(badge.find("kSingleClickDelayMs") == std::string::npos,
               "REQ-052 pin: no stale kSingleClickDelayMs use remains in the cpp");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-052 badge single-click delay pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-052 badge single-click delay pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (f) about-window clip + reset confirm ---------------------------------

void TestReq052AboutWindowFixes() {
    std::cout << "[RUN] Testing REQ-052 about-window clip + reset confirm pins..." << std::endl;
    const int failures_before = g_failed_count;

    std::string abt;
    Req052ReadSrc(L"src\\ui\\about_window.cpp", abt);
    std::string abt_h;
    Req052ReadSrc(L"src\\ui\\about_window.hpp", abt_h);
    if (abt.empty() || abt_h.empty()) {
        std::cout << "[SKIP] about_window.cpp/.hpp not resolvable; REQ-052 about pins skipped."
                  << std::endl;
        return;
    }

    // CLIP on the 3 feature draws (one DrawText inside the 3-iteration loop).
    // Needle anchors on the argument form (", D2D1_DRAW_TEXT_OPTIONS_CLIP);")
    // — the explanatory comment above the loop also names the flag.
    size_t clips = 0;
    for (size_t p = abt.find(", D2D1_DRAW_TEXT_OPTIONS_CLIP);"); p != std::string::npos;
         p = abt.find(", D2D1_DRAW_TEXT_OPTIONS_CLIP);", p + 1)) {
        ++clips;
    }
    TEST_CHECK(clips == 1,
               "REQ-052 pin: exactly one clipped feature draw (inside the 3-block loop)");
    const size_t loop_pos = abt.find("for (int i = 0; i < 3; ++i) {");
    const size_t clip_pos = abt.find(", D2D1_DRAW_TEXT_OPTIONS_CLIP);");
    TEST_CHECK(loop_pos != std::string::npos && clip_pos != std::string::npos &&
                   loop_pos < clip_pos,
               "REQ-052 pin: the CLIP draw sits inside the 3-feature loop "
               "(wrapping translations no longer bleed into the next block)");

    // Reset confirm: MB_YESNO gate, decline arm, no new StringIds.
    TEST_CHECK(abt.find("confirm_pending_ = true;") != std::string::npos &&
                   abt.find("MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND") != std::string::npos &&
                   abt.find("confirm_pending_ = false;") != std::string::npos &&
                   abt.find("if (confirm != IDYES) {") != std::string::npos,
               "REQ-052 pin: the destructive reset is confirm-gated (Yes runs the "
               "callback, anything else leaves state untouched)");
    TEST_CHECK(abt.find("std::wstring confirm_body = I18n::Get(StringId::AboutResetButton);") !=
                   std::string::npos,
               "REQ-052 pin: the confirm message reuses AboutResetButton + '?' "
               "(no new StringIds)");

    // Both dismissal paths hold off while the modal box is up.
    const size_t dismiss_pos = abt.find("case kDismissMessage: {");
    const size_t dismiss_end = abt.find("case WM_", dismiss_pos + 10);
    const size_t killfocus_pos = abt.find("case WM_KILLFOCUS: {");
    const size_t killfocus_end = abt.find("case WM_", killfocus_pos + 10);
    TEST_CHECK(dismiss_pos != std::string::npos &&
                   abt.find("if (pThis->confirm_pending_) {", dismiss_pos) != std::string::npos &&
                   (dismiss_end == std::string::npos ||
                    abt.find("if (pThis->confirm_pending_) {", dismiss_pos) < dismiss_end),
               "REQ-052 pin: kDismissMessage holds off while the confirm box is up");
    TEST_CHECK(killfocus_pos != std::string::npos &&
                   abt.find("if (pThis->confirm_pending_) {", killfocus_pos) != std::string::npos &&
                   (killfocus_end == std::string::npos ||
                    abt.find("if (pThis->confirm_pending_) {", killfocus_pos) < killfocus_end),
               "REQ-052 pin: WM_KILLFOCUS holds off while the confirm box is up");

    // The hpp carries the flag with its rationale.
    TEST_CHECK(abt_h.find("bool confirm_pending_ = false;") != std::string::npos,
               "REQ-052 pin: about_window.hpp declares the confirm_pending_ guard");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-052 about-window fix pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-052 about-window fix pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (g) pen/touch injection filter ----------------------------------------

void TestReq052TouchFilterPredicate() {
    std::cout << "[RUN] Testing REQ-052 pen/touch injection filter..." << std::endl;
    const int failures_before = g_failed_count;

    // Pure predicate (mouse_hook.hpp, already linked into this runner).
    volatile DWORD signature = MouseHook::kFromTouchSignature;
    volatile DWORD mask = MouseHook::kFromTouchMask;
    TEST_CHECK(signature == 0xFF515700UL,
               "REQ-052: the FROMTOUCH signature is 0xFF515700");
    TEST_CHECK(mask == 0xFFFFFF00UL,
               "REQ-052: the FROMTOUCH mask keeps the low byte for event data");

    volatile bool touch_down = MouseHook::IsFromTouchInjection(0xFF515700UL);
    volatile bool touch_data = MouseHook::IsFromTouchInjection(0xFF515700UL | 0x7FUL);
    volatile bool real_mouse = MouseHook::IsFromTouchInjection(0UL);
    volatile bool below_sig = MouseHook::IsFromTouchInjection(0xFF5156FFUL);
    volatile bool one_bit = MouseHook::IsFromTouchInjection(1UL);
    volatile bool shifted = MouseHook::IsFromTouchInjection(0x515700FFUL);
    TEST_CHECK(touch_down,
               "REQ-052: the bare FROMTOUCH signature is filtered");
    TEST_CHECK(touch_data,
               "REQ-052: signature + low-byte event data is still filtered");
    TEST_CHECK(!real_mouse,
               "REQ-052: genuine mouse input (dwExtraInfo == 0) is never filtered");
    TEST_CHECK(!below_sig,
               "REQ-052: a value just below the signature does not match");
    TEST_CHECK(!one_bit,
               "REQ-052: an arbitrary nonzero dwExtraInfo does not match");
    TEST_CHECK(!shifted,
               "REQ-052: shifted signature bits do not match");

    // Structural pins on the hook: exactly two bypass guards (the press and
    // the release branches), each side-effect-free (CallNextHookEx only), and
    // the EXTRA_INFO_MARKER self-filter is untouched (still exactly one
    // EXTRA_INFO_MARKER definition site in the hpp).
    std::string mhpp;
    Req052ReadSrc(L"src\\mouse_hook.hpp", mhpp);
    std::string mcpp;
    Req052ReadSrc(L"src\\mouse_hook.cpp", mcpp);
    if (mhpp.empty() || mcpp.empty()) {
        std::cout << "[SKIP] mouse_hook.hpp/.cpp not resolvable; REQ-052 touch pins skipped."
                  << std::endl;
        return;
    }

    size_t guards = 0;
    for (size_t p = mcpp.find("IsFromTouchInjection(static_cast<DWORD>(ms->dwExtraInfo))");
         p != std::string::npos;
         p = mcpp.find("IsFromTouchInjection(static_cast<DWORD>(ms->dwExtraInfo))", p + 1)) {
        ++guards;
    }
    TEST_CHECK(guards == 2,
               "REQ-052 pin: exactly two touch-injection guards (WM_LBUTTONDOWN + WM_LBUTTONUP)");

    // The DOWN guard precedes the drag/double-click state machine; the UP
    // guard precedes the drag-gate trip. Both bypass with CallNextHookEx only.
    const size_t down_branch = mcpp.find("if (wParam == WM_LBUTTONDOWN) {");
    const size_t up_branch = mcpp.find("} else if (wParam == WM_LBUTTONUP) {");
    const size_t g1 = mcpp.find("IsFromTouchInjection(static_cast<DWORD>(ms->dwExtraInfo))");
    const size_t g2 = mcpp.find("IsFromTouchInjection(static_cast<DWORD>(ms->dwExtraInfo))", g1 + 1);
    TEST_CHECK(down_branch != std::string::npos && up_branch != std::string::npos &&
                   down_branch < g1 && g1 < up_branch && g2 > up_branch,
               "REQ-052 pin: the guards sit inside the press and release branches");
    TEST_CHECK(mcpp.find("return ::CallNextHookEx(nullptr, nCode, wParam, lParam);", g1) != std::string::npos,
               "REQ-052 pin: the press guard is a pure bypass (no state mutation)");

    // The anti-reentrancy self-filter is byte-stable (exactly one marker
    // constant site in the hpp, untouched by this change).
    const size_t marker_pos = mhpp.find("EXTRA_INFO_MARKER");
    TEST_CHECK(marker_pos != std::string::npos,
               "REQ-052 pin: the EXTRA_INFO_MARKER self-filter contract remains in the hpp");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-052 touch filter predicate passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-052 touch filter predicate: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
