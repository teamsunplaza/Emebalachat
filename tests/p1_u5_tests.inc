// ---------------------------------------------------------------------------
// v0.10.10 phased fixes (261122_0002 session, unit P1-U5: main.cpp integrator
// + tooltip + language_search_palette). The GUI behaviors themselves (tray
// debounce wiring, palette click exemptions, shutdown ordering) live in
// main.cpp lambdas / WndProcs and are NOT unit-testable here - they are
// pinned structurally over the sources, the v107_drag_tooltip_tests.inc
// idiom. This suite covers the two PURE decisions the unit introduced:
//
//   (1) language_palette::IsLanguagePaletteWindowClass - the FUP-02 palette
//       exemption's class-name half (the pid half + the WindowFromPoint /
//       GetAncestor walk stay at the main.cpp call site).
//   (2) emebalachat::CarriedSourceCodeForSourcePick - the FUP-03 rule that an
//       "Auto Detect" source pick drops the carried detection code so the
//       run_retranslate empty-path re-detect actually runs.
//
// plus structural pins over language_search_palette.* (the atomic picker-open
// flag with acquire/release, the single class-name definition), tooltip.*
// (the picker-session source snapshot latch/consume) and main.cpp (BUG-02
// debounce arm, D1 drag-icon press flag, D3 release-time console guard,
// FUP-02 palette exemptions + snapshot fallback, FUP-03 carry clearing,
// BUG-08 locked config write, BUG-09/RISK-01 shutdown cancel + reorder).
// Zero windows, zero network: ResolveRepoFile source reads only.
// ---------------------------------------------------------------------------
void TestP1U5TrayPaletteGuards() {
    std::cout << "[RUN] Testing V10F P1-U5 tray/palette/integrator guards..." << std::endl;
    const int failures_before = g_failed_count;

    const auto read_src = [](const wchar_t* rel, std::string& out) {
        const std::filesystem::path p = ResolveRepoFile(rel);
        if (p.empty()) return false;
        std::ifstream in(p, std::ios::binary);
        if (!in) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    };
    const auto count_of = [](const std::string& hay, const std::string& needle) {
        std::size_t n = 0;
        for (std::size_t pos = hay.find(needle); pos != std::string::npos;
             pos = hay.find(needle, pos + needle.size())) {
            ++n;
        }
        return n;
    };

    // ---- 1) pure: IsLanguagePaletteWindowClass ----------------------------
    // The positive leg pins the constant to the name the window layer
    // actually registers (a drift between the predicate and the registered
    // class would silently disable every exemption).
    TEST_CHECK(std::wstring_view(emebalachat::language_palette::kLanguagePaletteWindowClass) ==
                   L"Emebalachat_LanguageSearchPalette",
               "P1-U5 (1): the palette window class constant is the registered name");
    TEST_CHECK(emebalachat::language_palette::IsLanguagePaletteWindowClass(
                   L"Emebalachat_LanguageSearchPalette"),
               "P1-U5 (1): the exact picker class exempts");
    TEST_CHECK(!emebalachat::language_palette::IsLanguagePaletteWindowClass(L""),
               "P1-U5 (1): empty class never exempts");
    TEST_CHECK(!emebalachat::language_palette::IsLanguagePaletteWindowClass(L"Edit"),
               "P1-U5 (1): the picker's EDIT child class never exempts");
    TEST_CHECK(!emebalachat::language_palette::IsLanguagePaletteWindowClass(L"ListBox"),
               "P1-U5 (1): the picker's LISTBOX child class never exempts");
    TEST_CHECK(!emebalachat::language_palette::IsLanguagePaletteWindowClass(L"#32768"),
               "P1-U5 (1): a popup menu is not the picker");
    TEST_CHECK(
        !emebalachat::language_palette::IsLanguagePaletteWindowClass(
            L"Emebalachat_LanguageSearchPaletteExtra"),
        "P1-U5 (1): a mere prefix of the picker class never exempts (exact compare)");
    TEST_CHECK(!emebalachat::language_palette::IsLanguagePaletteWindowClass(
                   L"emebalachat_languagesearchpalette"),
               "P1-U5 (1): the compare is case-sensitive");

    // ---- 2) pure: CarriedSourceCodeForSourcePick ----------------------------
    // AUTO spellings (registry name, code, empty, unresolvable token) all
    // clear the carry; concrete picks keep it verbatim.
    TEST_CHECK(emebalachat::CarriedSourceCodeForSourcePick("KO", "Auto Detect").empty(),
               "P1-U5 (2): an 'Auto Detect' pick drops the carried detection code");
    TEST_CHECK(emebalachat::CarriedSourceCodeForSourcePick("KO", "auto detect").empty(),
               "P1-U5 (2): the AUTO decision folds case (name lookup)");
    TEST_CHECK(emebalachat::CarriedSourceCodeForSourcePick("KO", "AUTO").empty(),
               "P1-U5 (2): the AUTO code spelling drops the carry too");
    TEST_CHECK(emebalachat::CarriedSourceCodeForSourcePick("KO", "").empty(),
               "P1-U5 (2): an empty pick normalizes to AUTO and drops the carry");
    TEST_CHECK(
        emebalachat::CarriedSourceCodeForSourcePick("KO", "not-a-language-at-all").empty(),
        "P1-U5 (2): an unresolvable pick normalizes to AUTO and drops the carry");
    TEST_CHECK(emebalachat::CarriedSourceCodeForSourcePick("KO", "Korean") == "KO",
               "P1-U5 (2): a concrete name pick keeps the carried code");
    TEST_CHECK(emebalachat::CarriedSourceCodeForSourcePick("KO", "ja") == "KO",
               "P1-U5 (2): a concrete code pick keeps the carried code");
    TEST_CHECK(emebalachat::CarriedSourceCodeForSourcePick("", "Japanese").empty(),
               "P1-U5 (2): an empty carry stays empty for a concrete pick");

    // ---- 3) structural pins -------------------------------------------------
    std::string palette_hpp, palette_cpp, tooltip_hpp, tooltip_cpp, main_src;
    TEST_CHECK(read_src(L"src\\ui\\language_search_palette.hpp", palette_hpp),
               "P1-U5 (3): language_search_palette.hpp readable");
    TEST_CHECK(read_src(L"src\\ui\\language_search_palette.cpp", palette_cpp),
               "P1-U5 (3): language_search_palette.cpp readable");
    TEST_CHECK(read_src(L"src\\ui\\tooltip.hpp", tooltip_hpp),
               "P1-U5 (3): tooltip.hpp readable");
    TEST_CHECK(read_src(L"src\\ui\\tooltip.cpp", tooltip_cpp),
               "P1-U5 (3): tooltip.cpp readable");
    TEST_CHECK(read_src(L"src\\main.cpp", main_src),
               "P1-U5 (3): main.cpp readable");

    if (!palette_hpp.empty()) {
        // One shared class-name definition (SECTION A), consumed by the pure
        // predicate AND the .cpp registration alias.
        TEST_CHECK(count_of(palette_hpp, "kLanguagePaletteWindowClass") >= 2,
                   "P1-U5 (3): the header exposes the class constant + predicate");
        TEST_CHECK(palette_hpp.find("inline bool IsLanguagePaletteWindowClass") !=
                       std::string::npos,
                   "P1-U5 (3): the pure predicate is header-inline (testable)");
    }
    if (!palette_cpp.empty()) {
        // FUP-02 Option A: the single-instance flag is atomic with the
        // acquire/release pairing the hook-thread readers rely on.
        TEST_CHECK(palette_cpp.find("std::atomic<bool> s_palette_open") != std::string::npos,
                   "P1-U5 (3): s_palette_open is a std::atomic<bool>");
        TEST_CHECK(palette_cpp.find("memory_order_acquire") != std::string::npos &&
                       palette_cpp.find("memory_order_release") != std::string::npos,
                   "P1-U5 (3): the picker-open flag uses acquire loads + release stores");
        // The raw class literal must live ONLY in the header (single
        // definition; the .cpp aliases it).
        TEST_CHECK(palette_cpp.find("Emebalachat_LanguageSearchPalette\"") ==
                       std::string::npos,
                   "P1-U5 (3): the .cpp no longer carries its own class literal");
        TEST_CHECK(palette_cpp.find("language_palette::kLanguagePaletteWindowClass") !=
                       std::string::npos,
                   "P1-U5 (3): the .cpp registration aliases the shared constant");
    }
    if (!tooltip_hpp.empty()) {
        TEST_CHECK(tooltip_hpp.find("CarriedSourceCodeForSourcePick") != std::string::npos,
                   "P1-U5 (3): the FUP-03 carry helper is header-inline");
        TEST_CHECK(tooltip_hpp.find("LanguagePickerSourceSnapshot") != std::string::npos,
                   "P1-U5 (3): the FUP-02 snapshot type is declared");
        TEST_CHECK(tooltip_hpp.find("LatchLanguagePickerSourceSnapshot") != std::string::npos &&
                       tooltip_hpp.find("ClearLanguagePickerSourceSnapshot") !=
                           std::string::npos,
                   "P1-U5 (3): the snapshot latch/clear accessors are declared");
    }
    if (!tooltip_cpp.empty()) {
        // Latch BEFORE the modal picker runs, clear AFTER the dispatch, on
        // every outcome (both inside the same picker arm).
        const std::size_t latch_pos = tooltip_cpp.find("LatchLanguagePickerSourceSnapshot();");
        const std::size_t show_pos = tooltip_cpp.find("ShowLanguageSearchPalette(hwnd, *init");
        const std::size_t clear_pos = tooltip_cpp.find("ClearLanguagePickerSourceSnapshot();");
        TEST_CHECK(latch_pos != std::string::npos && show_pos != std::string::npos &&
                       latch_pos < show_pos,
                   "P1-U5 (3): the snapshot latches before the modal picker opens");
        TEST_CHECK(clear_pos != std::string::npos && show_pos < clear_pos,
                   "P1-U5 (3): the snapshot clears after the pick dispatch");
        TEST_CHECK(tooltip_cpp.find("lang_picker_snapshot_code_ = source_lang_code_;") !=
                       std::string::npos,
                   "P1-U5 (3): the latch captures the RAW source code (AUTO reads as empty)");
    }
    if (!main_src.empty()) {
        // BUG-02: the single click routes through the debounce machine and
        // arms kTimerTrayToggle; the toggle itself stays in the WM_TIMER arm.
        const std::size_t up_pos = main_src.find("TrayToggleEvent::kLeftButtonUp");
        const std::size_t arm_pos =
            main_src.find("SetTimer(hwnd, kTimerTrayToggle, ::GetDoubleClickTime(), nullptr)");
        TEST_CHECK(up_pos != std::string::npos && arm_pos != std::string::npos &&
                       up_pos < arm_pos,
                   "P1-U5 (3) BUG-02: WM_LBUTTONUP arms the debounce timer via the state machine");
        TEST_CHECK(main_src.find("TrayToggleEvent::kTimerExpired") != std::string::npos,
                   "P1-U5 (3) BUG-02: the timer expiry still performs the toggle");
        TEST_CHECK(main_src.find("g_pBadge->SetVisible(!g_pBadge->IsVisible())") !=
                       std::string::npos,
                   "P1-U5 (3) BUG-02 zero-loss: double-click still toggles badge visibility only");

        // D1: the drag-icon press ownership flag (reset / press-time store /
        // release-time consult).
        TEST_CHECK(count_of(main_src, "drag_press_own_drag_icon") >= 3,
                   "P1-U5 (3) D1: the drag-icon press flag is reset, stamped and consulted");

        // D3 (main side): the release-time console guard sits in the
        // drag-release callback, before RequestShowAt; the click-time guard
        // stays as defense-in-depth.
        TEST_CHECK(count_of(main_src, "IsConsoleCaptureUnsafe(::GetForegroundWindow())") >= 2,
                   "P1-U5 (3) D3: both the release-time and click-time console guards exist");
        const std::size_t release_pos = main_src.find("SetDragReleaseCallback");
        const std::size_t showat_pos = main_src.find("RequestShowAt(drag_icon.GetHwnd()");
        TEST_CHECK(release_pos != std::string::npos && showat_pos != std::string::npos &&
                       release_pos < showat_pos,
                   "P1-U5 (3) D3: the release-time guard precedes the icon summon");

        // FUP-02: palette exemptions in the mouse/drag/ESC callbacks + the
        // snapshot fallback in both language-change callbacks.
        TEST_CHECK(count_of(main_src, "drag_press_own_palette") >= 3,
                   "P1-U5 (3) FUP-02: the palette press flag is reset, stamped and consulted");
        TEST_CHECK(count_of(main_src, "OwnLanguagePaletteWindowAtPoint") >= 3,
                   "P1-U5 (3) FUP-02: the press-point, release-point and callback exemptions");
        TEST_CHECK(main_src.find("IsLanguagePaletteWindowClass") != std::string::npos,
                   "P1-U5 (3) FUP-02: main.cpp exempts through the pure class predicate");
        TEST_CHECK(main_src.find("esc deferred: language picker owns the key") !=
                       std::string::npos,
                   "P1-U5 (3) FUP-02: ESC defers to the modal picker");
        TEST_CHECK(count_of(main_src, "GetLanguagePickerSourceSnapshot()") >= 2,
                   "P1-U5 (3) FUP-02: both language-change callbacks consume the snapshot");

        // FUP-03: the source-change callback routes the carry through the
        // AUTO-clearing helper.
        TEST_CHECK(main_src.find("CarriedSourceCodeForSourcePick(src_code") !=
                       std::string::npos,
                   "P1-U5 (3) FUP-03: the source pick clears the carry on AUTO");

        // BUG-08 call site: the locked verb, not a direct member write.
        TEST_CHECK(main_src.find("config.SetEngineHostConfig(out);") != std::string::npos,
                   "P1-U5 (3) BUG-08: the engine-host settings write uses the locked verb");

        // BUG-09/RISK-01: pipeline cancel + tray/config retired before the
        // bounded worker joins; the TerminateProcess backstop stays inside
        // probe_service.ShutdownJoinWorkers (engine repo, untouched here).
        // 261122_0002 P2-F added two earlier, NON-shutdown call sites (the ESC
        // arm and the badge arm both cancel an active translation), so the
        // ordering claim is pinned on the shutdown block: the worker cancel
        // that FOLLOWS the engine latch.
        const std::size_t eng_cancel = main_src.find("engine.RequestCancel();");
        const std::size_t worker_cancel =
            eng_cancel == std::string::npos
                ? std::string::npos
                : main_src.find("worker.RequestCancelTranslation();", eng_cancel);
        const std::size_t tray_destroy = main_src.find("tray.Destroy();");
        const std::size_t exit_save = main_src.rfind("config.SaveToFile();");
        const std::size_t drag_join = main_src.find("drag_translate_worker.Join()");
        TEST_CHECK(eng_cancel != std::string::npos && worker_cancel != std::string::npos &&
                       eng_cancel < worker_cancel,
                   "P1-U5 (3) BUG-09: the pipeline translation is cancelled at shutdown start");
        TEST_CHECK(tray_destroy != std::string::npos && drag_join != std::string::npos &&
                       tray_destroy < drag_join,
                   "P1-U5 (3) RISK-01: the tray icon is destroyed before the bounded joins");
        TEST_CHECK(exit_save != std::string::npos && exit_save < drag_join,
                   "P1-U5 (3) RISK-01: the config persist precedes the bounded joins");
        TEST_CHECK(main_src.find("ShutdownJoinWorkers") != std::string::npos,
                   "P1-U5 (3) RISK-01: the bounded probe join + its backstop stay last");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] V10F P1-U5 tray/palette/integrator guards completed." << std::endl;
    } else {
        std::cout << "[FAIL] V10F P1-U5 tray/palette/integrator guards: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
