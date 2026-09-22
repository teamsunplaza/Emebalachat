// ---------------------------------------------------------------------------
// REQ-054: tray user-model stem preservation across hotkey refreshes.
// User-reported defect: pressing F9 (or Ctrl+F9 / Ctrl+Shift+Enter) hid the
// "사용자 선택(.gguf) — <stem>" entry from the engine submenu until the next
// registry-aware refresh. Root cause: the three hook-thread hotkey paths call
// SystemTray::UpdateStatus with an explicit EMPTY user_model_stem, and the
// tray cached that empty string unconditionally — wiping the stem the
// registry-aware refresh_tray (main.cpp, GUI thread) had resolved. The model
// itself stayed registered in registry.json; this was a pure tray display
// regression.
// Fix: the UpdateStatus stem parameter became std::optional<std::string_view>
// defaulting to std::nullopt = KEEP the cached stem; an engaged value
// (including empty) sets/clears it. These pins are the structural mirror of
// that contract:
//   (a) tray.hpp — the optional parameter declaration + <optional> include;
//   (b) tray.cpp — the cache write is guarded by has_value() and the guard
//       precedes the write;
//   (c) hook.cpp — the explicit-empty-stem markers are gone and the REQ-054
//       contract note is present;
//   (d) main.cpp — refresh_tray still resolves the stem and passes it as an
//       ENGAGED optional (explicit std::string_view construction — passing
//       the std::string directly would not compile, two implicit user-defined
//       conversions), so an empty resolved stem still clears the entry;
//   (e) main.cpp — F9-pause drag-icon UX: the active-change callback dismisses
//       the icon behind an !active guard, and the icon click callback gates
//       on hook.IsActive() before the worker submit (the icon is only shown
//       while active, so a stale post-pause icon must be neither visible nor
//       clickable).
// ResolveRepoFile source pins (L97 of run_tests.cpp); every file unresolvable
// -> the section is skipped, exactly like the req048_r2 pattern.
// ---------------------------------------------------------------------------

void TestReq054ToggleStemPreserve() {
    std::cout << "[RUN] Testing REQ-054 toggle stem preservation pins..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) tray.hpp: the optional parameter + <optional> include ---------
    const std::filesystem::path tray_hpp = ResolveRepoFile(L"src\\ui\\tray.hpp");
    if (tray_hpp.empty()) {
        std::cout << "[SKIP] src/ui/tray.hpp not resolvable; REQ-054 (a) pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(tray_hpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        TEST_CHECK(src.find("#include <optional>") != std::string::npos,
                   "REQ-054 (a): tray.hpp includes <optional>");
        TEST_CHECK(src.find("std::optional<std::string_view> user_model_stem = std::nullopt") !=
                       std::string::npos,
                   "REQ-054 (a): UpdateStatus declares the stem as optional, "
                   "defaulting to nullopt (preserve)");
    }

    // ---- (b) tray.cpp: the cache write is guarded by has_value() -----------
    const std::filesystem::path tray_cpp = ResolveRepoFile(L"src\\ui\\tray.cpp");
    if (tray_cpp.empty()) {
        std::cout << "[SKIP] src/ui/tray.cpp not resolvable; REQ-054 (b) pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(tray_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        TEST_CHECK(src.find("user_model_stem_ = *user_model_stem;") != std::string::npos,
                   "REQ-054 (b): the cache write dereferences the engaged optional");
        // Ordering: the has_value guard must open BEFORE the guarded write —
        // an unconditional write would reintroduce the F9 wipe.
        const size_t guard_pos = src.find("if (user_model_stem.has_value())");
        const size_t write_pos = src.find("user_model_stem_ = *user_model_stem;");
        TEST_CHECK(guard_pos != std::string::npos && write_pos != std::string::npos &&
                       guard_pos < write_pos,
                   "REQ-054 (b): the has_value() guard precedes the cache write");
    }

    // ---- (c) hook.cpp: the explicit-empty-stem markers are gone ------------
    const std::filesystem::path hook_cpp = ResolveRepoFile(L"src\\hook.cpp");
    if (hook_cpp.empty()) {
        std::cout << "[SKIP] src/hook.cpp not resolvable; REQ-054 (c) pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(hook_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        TEST_CHECK(src.find("\"\",  // REQ-047 U1") == std::string::npos,
                   "REQ-054 (c): the explicit empty-stem argument (the F9 wipe) "
                   "is gone from the hook paths");
        TEST_CHECK(src.find("REQ-054") != std::string::npos,
                   "REQ-054 (c): the hook paths carry the preserve-contract note");
    }

    // ---- (d) main.cpp: refresh_tray resolves the stem and passes it engaged -
    const std::filesystem::path main_cpp = ResolveRepoFile(L"src\\main.cpp");
    if (main_cpp.empty()) {
        std::cout << "[SKIP] src/main.cpp not resolvable; REQ-054 (d) pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(main_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        TEST_CHECK(src.find("std::string user_model_stem;") != std::string::npos,
                   "REQ-054 (d): refresh_tray still resolves the stem from the registry");
        // The engaged pass rides the tray.UpdateStatus call — an empty resolved
        // stem must still reach the tray as an engaged (clearing) value.
        const size_t call_pos = src.find("tray.UpdateStatus(");
        const size_t engaged_pos = src.find("std::string_view(user_model_stem)");
        TEST_CHECK(call_pos != std::string::npos && engaged_pos != std::string::npos &&
                       call_pos < engaged_pos,
                   "REQ-054 (d): refresh_tray passes the resolved stem as an engaged "
                   "optional at the UpdateStatus call (empty resolves to a clear)");

        // ---- (e) F9-pause UX: the drag icon is dismissed and unclickable ----
        // The icon is only SHOWN while active (SetDragReleaseCallback gates on
        // hook.IsActive()), but nothing hid it on pause and its click callback
        // had no active gate — a stale icon kept offering translate while the
        // app claimed to be paused. drag_icon.Hide() appears at other sites
        // too, so the finds are scoped from the callback position onward.
        const size_t cb_pos = src.find("SetActiveChangeCallback");
        const size_t guard_pos = src.find("if (!active)", cb_pos);
        const size_t hide_pos = src.find("drag_icon.Hide()", cb_pos);
        TEST_CHECK(cb_pos != std::string::npos && hide_pos != std::string::npos &&
                       cb_pos < hide_pos,
                   "REQ-054 (e): the active-change callback dismisses the drag icon "
                   "on pause");
        TEST_CHECK(guard_pos != std::string::npos && guard_pos < hide_pos,
                   "REQ-054 (e): the icon Hide rides the !active guard "
                   "(pause-only dismissal)");
        // Click gate: the SetClickCallback lambda must drop the click while
        // paused BEFORE any worker submit (the only drag entry point that
        // lacked the fire-time IsActive() gate).
        const size_t click_pos = src.find("SetClickCallback");
        const size_t gate_pos = src.find("if (!hook.IsActive())", click_pos);
        const size_t submit_pos = src.find("drag_translate_worker.Submit", click_pos);
        TEST_CHECK(click_pos != std::string::npos && gate_pos != std::string::npos &&
                       submit_pos != std::string::npos &&
                       click_pos < gate_pos && gate_pos < submit_pos,
                   "REQ-054 (e): the icon click callback gates on hook.IsActive() "
                   "before the drag-translate submit");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-054 toggle stem preservation checks passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-054 toggle stem preservation: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
