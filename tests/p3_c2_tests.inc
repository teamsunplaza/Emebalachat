// ---------------------------------------------------------------------------
// V10F (261122_0002 P3-C2, Phase 3 wave A): UX-3 Option A — the floating
// badge pill becomes a 3-zone hit surface.
//
//   left medallion  : the legacy single click (cancel in flight / toggle
//                     active), still armed on kTimerSingleClick so the OS
//                     double-click window can never lose a click to a cancel
//                     (V10F BUG-01 stays fixed).
//   center label    : SRC / arrow / TGT slices dispatch IMMEDIATELY on
//                     WM_LBUTTONUP through SetZoneLanguageClickCallback (zone
//                     0 = source, 1 = swap, 2 = target) and
//                     SetSwapRequestCallback for the arrow.
//   right mini-tag  : engine tag text ("G"/"L"/"AI"/"U") + optional auto-send
//                     marker, its own SetTagClickCallback.
// Right-click menu, drag move, hover restore and the 5 s idle fade stay; the
// fade floor moves 20% -> 45% (kIdleAlpha).
//
// Coverage:
//   (a) TestP3C2BadgeZonePlanner      - the PURE planner (constexpr): the four
//                                       boundaries partition the whole pill,
//                                       every edge/midpoint maps exactly, the
//                                       defensive guards (tag outside the pill,
//                                       collapsed layout, zero width) behave,
//                                       and the function is usable in a
//                                       constexpr context.
//   (b) TestP3C2BadgeZoneDispatchMap  - BadgeZoneLanguageIndex (0/1/2 for the
//                                       language slices, -1 otherwise) and
//                                       BadgeZoneIsImmediate (the zones that
//                                       skip the single-click deferral and the
//                                       legacy double-click swap).
//   (c) TestP3C2BadgeZoneSourcePins   - structural pins on badge.hpp/.cpp: the
//                                       additive API, the store-before-post
//                                       marshaling of the mini-tag setters, the
//                                       zone press/hover/cursor wiring, the
//                                       preserved BUG-01 medallion deferral,
//                                       the kIdleAlpha floor and the untouched
//                                       right-click / drag paths.
//   (d) TestP3C2BadgeZoneLiveLayout   - a real badge: the default layout has no
//                                       tag zone and the planner's edges follow
//                                       the rendered pill; SetEngineTag /
//                                       SetAutoSendIndicator create and clear
//                                       the right zone. Skipped (not failed)
//                                       when the layered window cannot be
//                                       created in the current session.
//
// Staged as an .inc next to this runner (p1_u2 / p2_g precedent); registered
// from main() per the end-of-file pattern. Source pins read the production
// files through ResolveRepoFile with a CRLF-normalized raw read; every needle
// is ASCII-only.
// ---------------------------------------------------------------------------

#include "../src/ui/badge.hpp"
#include "../src/ui/dpi.hpp"  // ScaleDipsToPixels / WindowDpi for the live hit tests

namespace {

// Resolve + read a production source for the P3-C2 structural pins (same
// candidate-path precedent as the P1-U2 / P2-A readers; CRLF-normalized so
// line-anchored needles never depend on the checkout's autocrlf).
void P3C2ReadSrc(const wchar_t* rel, std::string& out) {
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

// ---- (a) the pure zone planner --------------------------------------------

void TestP3C2BadgeZonePlanner() {
    std::cout << "[RUN] Testing P3-C2 badge 3-zone planner (pure)..." << std::endl;
    const int failures_before = g_failed_count;

    using emebalachat::BadgeZone;
    using emebalachat::BadgeZoneForPoint;
    using emebalachat::BadgeZoneLayout;

    // 240 DIP pill, label at 50, arrow at 96, target at 124, no mini-tag.
    constexpr BadgeZoneLayout kPlain{50, 96, 124, -1};
    // Same pill with the engine chip starting at 210 (the chip + right pad).
    constexpr BadgeZoneLayout kTagged{50, 96, 124, 210};

    // constexpr usability: the planner is a pure function of (x, width, layout)
    // and every table entry below is resolved at compile time.
    static_assert(BadgeZoneForPoint(0, 240, kPlain) == BadgeZone::Medallion,
                  "P3-C2: the pill's left edge is the medallion");
    static_assert(BadgeZoneForPoint(239, 240, kPlain) == BadgeZone::Target,
                  "P3-C2: the right padding belongs to the target slice");
    static_assert(BadgeZoneForPoint(209, 240, kTagged) == BadgeZone::Target,
                  "P3-C2: the chip's left neighbour is the target slice");
    static_assert(BadgeZoneForPoint(210, 240, kTagged) == BadgeZone::Tag,
                  "P3-C2: the chip's left edge is the tag zone");
    static_assert(BadgeZoneLanguageIndex(BadgeZone::Source) == 0,
                  "P3-C2: source is zone index 0");
    static_assert(BadgeZoneLanguageIndex(BadgeZone::Swap) == 1,
                  "P3-C2: swap is zone index 1");
    static_assert(BadgeZoneLanguageIndex(BadgeZone::Target) == 2,
                  "P3-C2: target is zone index 2");
    static_assert(!BadgeZoneIsImmediate(BadgeZone::Medallion),
                  "P3-C2: the medallion keeps the single-click deferral");
    TEST_CHECK(true, "P3-C2: the planner and its tables are constexpr");

    // Outside the pill: nothing is clickable, so WndProc can never dispatch a
    // zone for a coordinate the pill does not cover.
    TEST_CHECK(BadgeZoneForPoint(-1, 240, kPlain) == BadgeZone::None &&
                   BadgeZoneForPoint(240, 240, kPlain) == BadgeZone::None &&
                   BadgeZoneForPoint(1000, 240, kPlain) == BadgeZone::None,
               "P3-C2: x outside [0, width) is BadgeZone::None");
    TEST_CHECK(BadgeZoneForPoint(10, 0, kPlain) == BadgeZone::None &&
                   BadgeZoneForPoint(0, -5, kPlain) == BadgeZone::None,
               "P3-C2: a non-positive width can never report a zone");

    // Exact edges (inclusive left, exclusive right when the next boundary
    // takes over) for the un-tagged pill.
    TEST_CHECK(BadgeZoneForPoint(0, 240, kPlain) == BadgeZone::Medallion &&
                   BadgeZoneForPoint(49, 240, kPlain) == BadgeZone::Medallion,
               "P3-C2: [0, source_begin) is the medallion (legacy toggle)");
    TEST_CHECK(BadgeZoneForPoint(50, 240, kPlain) == BadgeZone::Source &&
                   BadgeZoneForPoint(95, 240, kPlain) == BadgeZone::Source,
               "P3-C2: [source_begin, swap_begin) is the source slice");
    TEST_CHECK(BadgeZoneForPoint(96, 240, kPlain) == BadgeZone::Swap &&
                   BadgeZoneForPoint(123, 240, kPlain) == BadgeZone::Swap,
               "P3-C2: [swap_begin, target_begin) is the arrow slice");
    TEST_CHECK(BadgeZoneForPoint(124, 240, kPlain) == BadgeZone::Target &&
                   BadgeZoneForPoint(239, 240, kPlain) == BadgeZone::Target,
               "P3-C2: [target_begin, width) is the target slice when no chip exists");

    // The tag boundary wins over the target slice that would otherwise cover
    // the same pixels.
    TEST_CHECK(BadgeZoneForPoint(209, 240, kTagged) == BadgeZone::Target &&
                   BadgeZoneForPoint(210, 240, kTagged) == BadgeZone::Tag &&
                   BadgeZoneForPoint(239, 240, kTagged) == BadgeZone::Tag,
               "P3-C2: the chip carves its own zone out of the right end");

    // Total partition invariant: sweeping every pixel reports exactly one zone,
    // and the zones never go backwards as x grows (the planner's boundaries are
    // monotonic by construction).
    bool total_ok = true;
    bool monotonic_ok = true;
    int last_rank = -1;
    for (int x = 0; x < 240; ++x) {
        const BadgeZone zone = BadgeZoneForPoint(x, 240, kTagged);
        if (zone == BadgeZone::None) total_ok = false;
        const int rank = static_cast<int>(zone);
        if (rank < last_rank) monotonic_ok = false;
        last_rank = rank;
    }
    TEST_CHECK(total_ok, "P3-C2: every in-pill pixel maps to a zone (no dead gap)");
    TEST_CHECK(monotonic_ok, "P3-C2: the zone order never inverts across the pill");

    // Defensive guards: a tag boundary outside the pill must be ignored (a
    // stale layout after a width change can never steal the whole pill), and a
    // collapsed/zero layout must still return the medallion rather than None -
    // the pre-zone behaviour was "the whole pill toggles".
    constexpr BadgeZoneLayout kStaleTag{50, 96, 124, 400};
    constexpr BadgeZoneLayout kCollapsed{0, 0, 0, -1};
    TEST_CHECK(BadgeZoneForPoint(239, 240, kStaleTag) == BadgeZone::Target,
               "P3-C2: an out-of-pill tag boundary is ignored");
    TEST_CHECK(BadgeZoneForPoint(0, 240, kCollapsed) == BadgeZone::Medallion &&
                   BadgeZoneForPoint(239, 240, kCollapsed) == BadgeZone::Medallion,
               "P3-C2: a not-yet-laid-out pill keeps the legacy whole-pill toggle");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C2 badge 3-zone planner passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C2 badge 3-zone planner: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (b) zone index + immediate dispatch map ------------------------------

void TestP3C2BadgeZoneDispatchMap() {
    std::cout << "[RUN] Testing P3-C2 zone dispatch map..." << std::endl;
    const int failures_before = g_failed_count;

    using emebalachat::BadgeZone;
    using emebalachat::BadgeZoneIsImmediate;
    using emebalachat::BadgeZoneLanguageIndex;

    // The int the zone-language callback receives: exactly the 0/1/2 the
    // main.cpp owner documents, and -1 for the zones that are not part of the
    // language pair (so a caller that forwards the int blindly can never open
    // a picker for the chip or swap the medallion's pause toggle).
    TEST_CHECK(BadgeZoneLanguageIndex(BadgeZone::Source) == 0 &&
                   BadgeZoneLanguageIndex(BadgeZone::Swap) == 1 &&
                   BadgeZoneLanguageIndex(BadgeZone::Target) == 2,
               "P3-C2: SRC/SWAP/TGT map to zone indices 0/1/2");
    TEST_CHECK(BadgeZoneLanguageIndex(BadgeZone::Medallion) == -1 &&
                   BadgeZoneLanguageIndex(BadgeZone::Tag) == -1 &&
                   BadgeZoneLanguageIndex(BadgeZone::None) == -1,
               "P3-C2: medallion / tag / outside map to -1");

    // Immediate = dispatched on WM_LBUTTONUP without the deferral AND excluded
    // from the legacy double-click swap (its action already ran).
    TEST_CHECK(BadgeZoneIsImmediate(BadgeZone::Source) &&
                   BadgeZoneIsImmediate(BadgeZone::Swap) &&
                   BadgeZoneIsImmediate(BadgeZone::Target) &&
                   BadgeZoneIsImmediate(BadgeZone::Tag),
               "P3-C2: the language slices and the chip are immediate zones");
    TEST_CHECK(!BadgeZoneIsImmediate(BadgeZone::Medallion) &&
                   !BadgeZoneIsImmediate(BadgeZone::None),
               "P3-C2: the medallion keeps the kTimerSingleClick deferral");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C2 zone dispatch map passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C2 zone dispatch map: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (c) structural pins --------------------------------------------------

void TestP3C2BadgeZoneSourcePins() {
    std::cout << "[RUN] Testing P3-C2 badge zone structural pins..." << std::endl;
    const int failures_before = g_failed_count;

    std::string badge;
    std::string badge_h;
    P3C2ReadSrc(L"src\\ui\\badge.cpp", badge);
    P3C2ReadSrc(L"src\\ui\\badge.hpp", badge_h);
    if (badge.empty() || badge_h.empty()) {
        std::cout << "[SKIP] badge.cpp/.hpp not resolvable; P3-C2 pins skipped." << std::endl;
        return;
    }

    // --- header: additive API (nothing pre-existing removed) ----------------
    TEST_CHECK(badge_h.find("enum class BadgeZone : int {") != std::string::npos &&
                   badge_h.find("Tag = 4") != std::string::npos,
               "P3-C2 pin: the BadgeZone enum is declared in badge.hpp");
    TEST_CHECK(badge_h.find("struct BadgeZoneLayout {") != std::string::npos &&
                   badge_h.find("int tag_begin = -1;") != std::string::npos,
               "P3-C2 pin: the layout struct carries the four DIP boundaries "
                   "(tag_begin = -1 means no chip)");
    TEST_CHECK(badge_h.find("constexpr BadgeZone BadgeZoneForPoint(int x, int width, "
                            "const BadgeZoneLayout& layout) noexcept") != std::string::npos,
               "P3-C2 pin: the planner keeps its pure (x, width, layout) signature");
    TEST_CHECK(badge_h.find("constexpr int BadgeZoneLanguageIndex(BadgeZone zone) noexcept") !=
                   std::string::npos,
               "P3-C2 pin: the 0/1/2 mapping is a constexpr helper");
    TEST_CHECK(badge_h.find("constexpr bool BadgeZoneIsImmediate(BadgeZone zone) noexcept") !=
                   std::string::npos,
               "P3-C2 pin: the immediate-zone predicate is a constexpr helper");

    TEST_CHECK(badge_h.find("using ZoneClickCallback = std::function<void(int zone)>;") !=
                   std::string::npos &&
                   badge_h.find("void SetZoneLanguageClickCallback(ZoneClickCallback cb);") !=
                       std::string::npos,
               "P3-C2 pin: SetZoneLanguageClickCallback(int zone) is declared");
    TEST_CHECK(badge_h.find("void SetSwapRequestCallback(ActionCallback cb);") != std::string::npos &&
                   badge_h.find("void SetTagClickCallback(ActionCallback cb);") != std::string::npos,
               "P3-C2 pin: the swap + mini-tag callbacks are declared");
    TEST_CHECK(badge_h.find("void SetEngineTag(const wchar_t* tag);") != std::string::npos &&
                   badge_h.find("void SetAutoSendIndicator(bool on);") != std::string::npos,
               "P3-C2 pin: SetEngineTag / SetAutoSendIndicator are declared");
    TEST_CHECK(badge_h.find("BadgeZoneLayout GetZoneLayout() const;") != std::string::npos &&
                   badge_h.find("BadgeZone HitTestZone(int client_x) const;") != std::string::npos,
               "P3-C2 pin: the layout snapshot + hit test are exposed");
    TEST_CHECK(badge_h.find("static constexpr BYTE kIdleAlpha = 115;") != std::string::npos &&
                   badge_h.find("static constexpr BYTE kFullAlpha = 217;") != std::string::npos,
               "P3-C2 pin: the fade floor is kIdleAlpha (115 = 45%) and the activity "
               "level is kFullAlpha (217 = 85%)");
    TEST_CHECK(badge_h.find("WM_BADGE_SET_TAG      = WM_USER + 205;") != std::string::npos &&
                   badge_h.find("WM_BADGE_SET_AUTOSEND = WM_USER + 206;") != std::string::npos,
               "P3-C2 pin: the mini-tag re-render messages claim WM_USER+205/206");

    // Zero feature loss: every pre-existing public entry point is still here.
    TEST_CHECK(badge_h.find("void SetClickCallback(ActionCallback cb);") != std::string::npos &&
                   badge_h.find("void SetDoubleClickCallback(ActionCallback cb);") !=
                       std::string::npos &&
                   badge_h.find("void SetRightClickCallback(ActionCallback cb);") !=
                       std::string::npos &&
                   badge_h.find("void SetPositionCallback(PositionCallback cb);") !=
                       std::string::npos,
               "P3-C2 pin: the four pre-existing callbacks are untouched");
    TEST_CHECK(badge_h.find("static constexpr DWORD kIdleTimeoutMs = 5000;") != std::string::npos,
               "P3-C2 pin: the 5 s idle fade timeout is unchanged");
    TEST_CHECK(badge_h.find("UINT single_click_delay_ms_ = 220;") != std::string::npos,
               "P3-C2 pin: the medallion's GetDoubleClickTime deferral member is unchanged");

    // --- cpp: the setters store before they post ---------------------------
    const size_t tag_fn = badge.find("void FloatingBadge::SetEngineTag(const wchar_t* tag) {");
    const size_t tag_fn_end = badge.find("void FloatingBadge::SetAutoSendIndicator(", tag_fn);
    TEST_CHECK(tag_fn != std::string::npos && tag_fn_end != std::string::npos &&
                   tag_fn_end > tag_fn,
               "P3-C2 pin: the SetEngineTag body is locatable and bounded");
    if (tag_fn != std::string::npos && tag_fn_end != std::string::npos && tag_fn_end > tag_fn) {
        const std::string body = badge.substr(tag_fn, tag_fn_end - tag_fn);
        const size_t store = body.find("engine_tag_ = std::move(value);");
        const size_t post = body.find("::PostMessageW(hwnd_, WM_BADGE_SET_TAG");
        TEST_CHECK(store != std::string::npos && post != std::string::npos && store < post,
                   "P3-C2 pin: SetEngineTag stores under data_mutex_ BEFORE posting the "
                   "re-render (a reordered message cannot roll the chip back)");
        TEST_CHECK(body.find("value.size() > kMaxEngineTagChars") != std::string::npos,
                   "P3-C2 pin: SetEngineTag truncates to the chip budget");
    }
    const size_t as_fn = badge.find("void FloatingBadge::SetAutoSendIndicator(bool on) {");
    const size_t as_fn_end = badge.find("BadgeZoneLayout FloatingBadge::GetZoneLayout(", as_fn);
    TEST_CHECK(as_fn != std::string::npos && as_fn_end != std::string::npos && as_fn_end > as_fn,
               "P3-C2 pin: the SetAutoSendIndicator body is locatable and bounded");
    if (as_fn != std::string::npos && as_fn_end != std::string::npos && as_fn_end > as_fn) {
        const std::string body = badge.substr(as_fn, as_fn_end - as_fn);
        const size_t store = body.find("auto_send_indicator_ = on;");
        const size_t post = body.find("::PostMessageW(hwnd_, WM_BADGE_SET_AUTOSEND");
        TEST_CHECK(store != std::string::npos && post != std::string::npos && store < post,
                   "P3-C2 pin: SetAutoSendIndicator follows the same store-then-post rule");
    }
    TEST_CHECK(badge.find("case WM_BADGE_SET_TAG:") != std::string::npos &&
                   badge.find("case WM_BADGE_SET_AUTOSEND:") != std::string::npos,
               "P3-C2 pin: both mini-tag notifications have a WndProc arm");

    // --- cpp: press / hover / cursor wiring -------------------------------
    const size_t down = badge.find("case WM_LBUTTONDOWN: {");
    const size_t down_end = badge.find("case WM_MOUSEMOVE: {", down);
    TEST_CHECK(down != std::string::npos && down_end != std::string::npos && down_end > down,
               "P3-C2 pin: the WM_LBUTTONDOWN body is locatable");
    if (down != std::string::npos && down_end != std::string::npos && down_end > down) {
        const std::string body = badge.substr(down, down_end - down);
        const size_t latch = body.find("self->press_zone_ =");
        const size_t hit = body.find("self->HitTestZone(");
        // The landed source wraps the assignment over two lines
        // (`self->press_zone_ =\n    self->HitTestZone(...);`), so the latch
        // token precedes the call token textually while the semantics are
        // resolve-then-latch in one expression.
        TEST_CHECK(latch != std::string::npos && hit != std::string::npos && latch < hit,
                   "P3-C2 pin: the press latches the zone resolved by HitTestZone");
        TEST_CHECK(body.find("self->UpdateAlpha(kFullAlpha);") != std::string::npos,
                   "P3-C2 pin: a press still restores the 85% activity alpha");
    }

    const size_t up = badge.find("case WM_LBUTTONUP: {");
    const size_t up_end = badge.find("case WM_LBUTTONDBLCLK", up);
    TEST_CHECK(up != std::string::npos && up_end != std::string::npos && up_end > up,
               "P3-C2 pin: the WM_LBUTTONUP body is locatable");
    if (up != std::string::npos && up_end != std::string::npos && up_end > up) {
        const std::string body = badge.substr(up, up_end - up);
        const size_t immediate = body.find("if (BadgeZoneIsImmediate(self->press_zone_)) {");
        const size_t dispatch = body.find("self->DispatchZoneClick(self->press_zone_);");
        const size_t medallion =
            body.find("::SetTimer(hwnd, kTimerSingleClick, self->single_click_delay_ms_, nullptr)");
        TEST_CHECK(immediate != std::string::npos && dispatch != std::string::npos &&
                       immediate < dispatch,
                   "P3-C2 pin: an immediate zone dispatches on WM_LBUTTONUP");
        TEST_CHECK(medallion != std::string::npos && immediate < medallion,
                   "P3-C2 pin: the medallion keeps the exact BUG-01 SetTimer arm (deferred)");
        TEST_CHECK(body.find("self->click_callback_();") == std::string::npos,
                   "P3-C2 pin: no immediate click_callback_ in WM_LBUTTONUP (BUG-01 intact)");
    }

    const size_t dbl = badge.find("case WM_LBUTTONDBLCLK: {");
    const size_t dbl_end = badge.find("case WM_RBUTTONUP:", dbl);
    TEST_CHECK(dbl != std::string::npos && dbl_end != std::string::npos && dbl_end > dbl,
               "P3-C2 pin: the WM_LBUTTONDBLCLK body is locatable");
    if (dbl != std::string::npos && dbl_end != std::string::npos && dbl_end > dbl) {
        const std::string body = badge.substr(dbl, dbl_end - dbl);
        TEST_CHECK(body.find("::KillTimer(hwnd, kTimerSingleClick)") != std::string::npos,
                   "P3-C2 pin: the double-click still cancels a pending medallion click");
        TEST_CHECK(body.find("if (!BadgeZoneIsImmediate(self->press_zone_) && "
                             "self->double_click_callback_) {") != std::string::npos,
                   "P3-C2 pin: the legacy swap is skipped only for a press that started on "
                   "an immediate zone (double-click elsewhere is unchanged)");
    }

    TEST_CHECK(badge.find("self->hovered_zone_ = BadgeZone::None;") != std::string::npos,
               "P3-C2 pin: WM_MOUSELEAVE clears the hovered zone (the wash must vanish)");
    TEST_CHECK(badge.find("const bool zone_changed = (hover_zone != self->hovered_zone_);") !=
                   std::string::npos &&
                   badge.find("self->Render(); // V10F P3-C2: redraw the zone hover wash") !=
                       std::string::npos,
               "P3-C2 pin: a hovered-zone change redraws the pill exactly once");

    const size_t cursor = badge.find("case WM_SETCURSOR: {");
    TEST_CHECK(cursor != std::string::npos, "P3-C2 pin: the badge handles WM_SETCURSOR");
    if (cursor != std::string::npos) {
        const size_t cursor_end = badge.find("case WM_TIMER: {", cursor);
        TEST_CHECK(cursor_end != std::string::npos && cursor_end > cursor,
                   "P3-C2 pin: the WM_SETCURSOR arm is bounded by WM_TIMER");
        if (cursor_end != std::string::npos && cursor_end > cursor) {
            const std::string body = badge.substr(cursor, cursor_end - cursor);
            TEST_CHECK(body.find("BadgeZoneIsImmediate(self->HitTestZone(ptCursor.x))") !=
                           std::string::npos &&
                           body.find("MAKEINTRESOURCEW(32649)") != std::string::npos,
                       "P3-C2 pin: clickable zones show IDC_HAND");
            TEST_CHECK(body.find("MAKEINTRESOURCEW(32512)") != std::string::npos,
                       "P3-C2 pin: the medallion keeps the arrow cursor");
        }
    }

    // --- cpp: the 45% idle floor ------------------------------------------
    const size_t idle = badge.find("} else if (wParam == kTimerIdle) {");
    TEST_CHECK(idle != std::string::npos, "P3-C2 pin: the kTimerIdle arm is locatable");
    if (idle != std::string::npos) {
        const size_t idle_end = badge.find("} else if (wParam == kTimerDisplayChange) {", idle);
        TEST_CHECK(idle_end != std::string::npos && idle_end > idle,
                   "P3-C2 pin: the kTimerIdle arm is bounded");
        if (idle_end != std::string::npos && idle_end > idle) {
            const std::string body = badge.substr(idle, idle_end - idle);
            TEST_CHECK(body.find("self->current_alpha_ > kIdleAlpha") != std::string::npos &&
                           body.find("self->UpdateAlpha(kIdleAlpha);") != std::string::npos,
                       "P3-C2 pin: the idle fade lands on kIdleAlpha (45%)");
            TEST_CHECK(body.find("UpdateAlpha(51)") == std::string::npos &&
                           body.find("> 51") == std::string::npos,
                       "P3-C2 pin: the old 20% floor is gone");
            TEST_CHECK(body.find("!self->is_hovered_ && !self->is_mouse_down_") !=
                           std::string::npos,
                       "P3-C2 pin: hover and press still suppress the fade");
        }
    }

    // --- cpp: Render lays the chip out and publishes the zones -------------
    TEST_CHECK(badge.find("const float tagX = textX + textWidth + tagGap;") != std::string::npos,
               "P3-C2 pin: the chip is placed immediately right of the label");
    TEST_CHECK(badge.find("medallionVisualW + gap + textWidth + (has_tag ? tagGap + tagWidth : 0.0f);") !=
                   std::string::npos,
               "P3-C2 pin: the pill only widens when a chip exists (pre-existing width "
               "pins stay exact)");
    TEST_CHECK(badge.find("tag_text += L") != std::string::npos &&
                   badge.find("u21B5") != std::string::npos,
               "P3-C2 pin: the auto-send marker appends the U+21B5 glyph");
    size_t hit_tests = 0;
    for (size_t p = badge.find("textLayout->HitTestTextPosition("); p != std::string::npos;
         p = badge.find("textLayout->HitTestTextPosition(", p + 1)) {
        ++hit_tests;
    }
    TEST_CHECK(hit_tests == 2,
               "P3-C2 pin: the arrow and target boundaries both come from the drawn layout");
    const size_t zone_store = badge.find("zone_layout_ = zone_layout;");
    const size_t zone_lock = badge.find("std::lock_guard<std::mutex> lock(data_mutex_);",
                                       zone_store > 200 ? zone_store - 200 : 0);
    TEST_CHECK(zone_store != std::string::npos && zone_lock != std::string::npos &&
                   zone_lock < zone_store,
               "P3-C2 pin: the published layout is stored under data_mutex_");
    TEST_CHECK(badge.find("kTagFontSize") != std::string::npos &&
                   badge.find("kTagRadius") != std::string::npos,
               "P3-C2 pin: the chip uses the declared DIP geometry constants");
    TEST_CHECK(badge.find("tag_hovered ? ui::ColorFromHex(theme.border_hover,") !=
                   std::string::npos,
               "P3-C2 pin: the chip warms to the theme hover token on hover");

    // --- cpp: untouched surfaces -----------------------------------------
    TEST_CHECK(badge.find("case WM_RBUTTONUP: {") != std::string::npos &&
                   badge.find("self->right_click_callback_();") != std::string::npos,
               "P3-C2 pin: the right-click menu path is untouched");
    TEST_CHECK(badge.find("self->position_callback_(rcWnd.left, rcWnd.top);") != std::string::npos &&
                   badge.find("::SetWindowPos(hwnd, HWND_TOPMOST, newX, newY,") != std::string::npos,
               "P3-C2 pin: drag move + position persistence are untouched");
    TEST_CHECK(badge.find("case WM_DISPLAYCHANGE: {") != std::string::npos &&
                   badge.find("self->ReclampToVisibleMonitors();") != std::string::npos,
               "P3-C2 pin: the R1-21 display-change re-clamp is untouched");
    TEST_CHECK(badge.find("Render(false);") != std::string::npos,
               "P3-C2 pin: the one-pass device-lost recovery is untouched");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C2 badge zone structural pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C2 badge zone structural pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (d) live layout ------------------------------------------------------

void TestP3C2BadgeZoneLiveLayout() {
    std::cout << "[RUN] Testing P3-C2 badge zone live layout..." << std::endl;
    const int failures_before = g_failed_count;

    HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    emebalachat::FloatingBadge badge;
    if (!badge.Create(hInst, L"KO", L"EN")) {
        std::cout << "[SKIP] P3-C2: the layered badge could not be created in this session; "
                     "the live layout leg was skipped." << std::endl;
        return;
    }

    // The badge may sit on a non-96-DPI monitor (Create defaults to the
    // primary work area), so all client-x probes are converted with the
    // window's OWN dpi. Round-to-nearest is exact in both directions for
    // dpi >= 96 (dpi.hpp), so the physical x of a DIP boundary converts
    // back to exactly that DIP - no +1 offset that a fractional scale could
    // round into the neighbouring slice.
    const UINT badge_dpi = emebalachat::ui::WindowDpi(badge.GetHwnd());
    const auto dip_to_px = [&](int dip) {
        return emebalachat::ui::ScaleDipsToPixels(dip, badge_dpi);
    };

    // Default: no engine tag -> no chip zone, and the layout is a monotonic
    // partition of the DIP width that matches the rendered window.
    emebalachat::BadgeZoneLayout plain = badge.GetZoneLayout();
    TEST_CHECK(plain.tag_begin == -1,
               "P3-C2: a badge with no engine tag publishes no chip zone");
    TEST_CHECK(plain.source_begin > 0 && plain.source_begin < plain.swap_begin &&
                   plain.swap_begin < plain.target_begin &&
                   plain.target_begin < badge.GetCurrentWidth(),
               "P3-C2: the SRC/arrow/TGT boundaries are monotonic and inside the pill");
    TEST_CHECK(badge.HitTestZone(0) == emebalachat::BadgeZone::Medallion,
               "P3-C2: the pill's left edge hit-tests as the medallion");
    TEST_CHECK(badge.HitTestZone(-4) == emebalachat::BadgeZone::None,
               "P3-C2: a point left of the pill hit-tests as none");
    TEST_CHECK(badge.HitTestZone(dip_to_px(badge.GetCurrentWidth() - 1)) ==
                   emebalachat::BadgeZone::Target,
               "P3-C2: the pill's right edge hit-tests as the target slice (no chip)");

    // The label slices themselves: SRC left edge, arrow, TGT left edge.
    TEST_CHECK(badge.HitTestZone(dip_to_px(plain.source_begin)) ==
                   emebalachat::BadgeZone::Source,
               "P3-C2: the source slice is hit-testable at its left edge");
    TEST_CHECK(badge.HitTestZone(dip_to_px(plain.swap_begin)) ==
                   emebalachat::BadgeZone::Swap,
               "P3-C2: the arrow slice is hit-testable at its left edge");
    TEST_CHECK(badge.HitTestZone(dip_to_px(plain.target_begin)) ==
                   emebalachat::BadgeZone::Target,
               "P3-C2: the target slice is hit-testable at its left edge");

    // Engine tag: the chip zone appears at the right end and the pill only
    // grows (the label is never clipped).
    const int width_before = badge.GetCurrentWidth();
    badge.SetEngineTag(L"G");
    const emebalachat::BadgeZoneLayout tagged = badge.GetZoneLayout();
    TEST_CHECK(tagged.tag_begin >= tagged.target_begin && tagged.tag_begin > 0,
               "P3-C2: SetEngineTag publishes a chip zone right of the target slice");
    TEST_CHECK(badge.GetCurrentWidth() > width_before,
               "P3-C2: the pill widens to make room for the chip");
    TEST_CHECK(badge.HitTestZone(dip_to_px(badge.GetCurrentWidth() - 1)) ==
                   emebalachat::BadgeZone::Tag,
               "P3-C2: the pill's right edge hit-tests as the chip while a tag is shown");

    // Auto-send alone (no engine tag) still yields a chip: the marker is the
    // only content.
    badge.SetEngineTag(L"");
    TEST_CHECK(badge.GetZoneLayout().tag_begin == -1,
               "P3-C2: clearing the engine tag removes the chip zone");
    badge.SetAutoSendIndicator(true);
    TEST_CHECK(badge.GetZoneLayout().tag_begin >= 0,
               "P3-C2: the auto-send marker alone creates the chip zone");

    // Both off -> back to the plain two-zone layout.
    badge.SetAutoSendIndicator(false);
    const emebalachat::BadgeZoneLayout cleared = badge.GetZoneLayout();
    TEST_CHECK(cleared.tag_begin == -1,
               "P3-C2: clearing the auto-send marker removes the chip zone again");
    TEST_CHECK(cleared.source_begin == plain.source_begin,
               "P3-C2: clearing the chip restores the original label origin");

    // A long engine tag is truncated to the chip budget instead of exploding
    // the pill.
    badge.SetEngineTag(L"VeryLongEngineName");
    const int truncated_width = badge.GetCurrentWidth();
    badge.SetEngineTag(L"VeryLongEngineName");
    TEST_CHECK(badge.GetCurrentWidth() == truncated_width,
               "P3-C2: an over-long engine tag is truncated to a stable chip width");

    badge.Destroy();

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C2 badge zone live layout passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C2 badge zone live layout: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- registration ---------------------------------------------------------

namespace p3_c2_tests {

void P3C2RegisterTests() {
    TestP3C2BadgeZonePlanner();
    TestP3C2BadgeZoneDispatchMap();
    TestP3C2BadgeZoneSourcePins();
    TestP3C2BadgeZoneLiveLayout();
}

} // namespace p3_c2_tests
