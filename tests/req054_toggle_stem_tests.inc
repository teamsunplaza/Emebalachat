// ---------------------------------------------------------------------------
// REQ-054 (session history) + REQ-056 (derive-at-open, the actual contract).
// REQ-054 history: pressing F9 hid the "사용자 선택(.gguf) — <stem>" entry
// (the hook paths pushed an explicit empty stem into the tray's cache), and
// the check mark lied after hotkeys (a legacy bool formula). Both bugs lived
// in PUSHED COPIES the tray kept (user_model_stem_, preferred_engine_).
// REQ-056 removes the copies entirely: the engine submenu's two config-
// derived values are DERIVED AT OPEN — ShowContextMenu asks main.cpp's
// resolver (SetEngineMenuResolver, GUI thread, single authority) at menu
// build time. UpdateStatus no longer carries either value; the hook paths
// refresh only runtime state. No caches -> no stale paths.
//   (a) tray.hpp — EngineMenuView + SetEngineMenuResolver declared; the old
//       UpdateStatus push parameters (optional stem, preferred_engine) gone;
//   (b) tray.cpp — no cached members (user_model_stem_/preferred_engine_);
//       the menu consumes the resolver's view;
//   (c) hook.cpp — the hook paths push nothing config-derived (no
//       TrayPreferredEngineIndex argument, no empty-stem marker);
//   (d) main.cpp — the resolver publishes stem + check-index resolution and
//       is wired before the message loop; the engaged-optional stem hack is
//       gone;
//   (e) main.cpp — F9-pause drag-icon UX (REQ-054, still valid): the
//       active-change callback dismisses the icon behind an !active guard,
//       and the icon click callback gates on hook.IsActive() before the
//       worker submit;
//   (f) behavioral — the shared 4-way check-mark mapping (TrayPreferredEngineIndex),
//       now consumed by main.cpp's resolver only.
// ResolveRepoFile source pins (L97 of run_tests.cpp); every file unresolvable
// -> the section is skipped, exactly like the req048_r2 pattern.
// ---------------------------------------------------------------------------

// run_tests.cpp's include block does not pull in tray.hpp; this inc includes
// it directly for the behavioral TrayPreferredEngineIndex pins below (pragma-
// once safe; the same mid-TU discipline as req048_r2_tests.inc).
#include "../src/ui/tray.hpp" // REQ-054: TrayPreferredEngineIndex (shared 4-way mapping)

void TestReq054ToggleStemPreserve() {
    std::cout << "[RUN] Testing REQ-054/REQ-056 engine-menu pins..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) tray.hpp: the derive-at-open seam replaces the push parameters
    const std::filesystem::path tray_hpp = ResolveRepoFile(L"src\\ui\\tray.hpp");
    if (tray_hpp.empty()) {
        std::cout << "[SKIP] src/ui/tray.hpp not resolvable; REQ-056 (a) pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(tray_hpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        TEST_CHECK(src.find("struct EngineMenuView") != std::string::npos &&
                       src.find("SetEngineMenuResolver") != std::string::npos,
                   "REQ-056 (a): tray.hpp declares EngineMenuView + SetEngineMenuResolver "
                   "(the derive-at-open seam)");
        TEST_CHECK(src.find("std::optional<std::string_view> user_model_stem") ==
                       std::string::npos,
                   "REQ-056 (a): the UpdateStatus stem push parameter is gone "
                   "(REQ-054's optional is obsolete)");
        TEST_CHECK(src.find("int preferred_engine,") == std::string::npos &&
                       src.find("preferred_engine_") == std::string::npos,
                   "REQ-056 (a): UpdateStatus carries no preferred_engine argument and "
                   "no cached member remains");
    }

    // ---- (b) tray.cpp: no cached copies; the menu consumes the resolver's view
    const std::filesystem::path tray_cpp = ResolveRepoFile(L"src\\ui\\tray.cpp");
    if (tray_cpp.empty()) {
        std::cout << "[SKIP] src/ui/tray.cpp not resolvable; REQ-056 (b) pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(tray_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        TEST_CHECK(src.find("user_model_stem_") == std::string::npos &&
                       src.find("preferred_engine_") == std::string::npos,
                   "REQ-056 (b): the tray keeps no cached stem/check-index copies");
        TEST_CHECK(src.find("view.user_model_stem") != std::string::npos &&
                       src.find("view.preferred_engine") != std::string::npos,
                   "REQ-056 (b): ShowContextMenu derives the menu from the resolver's view");
    }

    // ---- (c) hook.cpp: the hook paths push nothing config-derived ----------
    const std::filesystem::path hook_cpp = ResolveRepoFile(L"src\\hook.cpp");
    if (hook_cpp.empty()) {
        std::cout << "[SKIP] src/hook.cpp not resolvable; REQ-056 (c) pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(hook_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        TEST_CHECK(src.find("\"\",  // REQ-047 U1") == std::string::npos,
                   "REQ-054 (c): the explicit empty-stem argument (the F9 wipe) "
                   "is gone from the hook paths");
        TEST_CHECK(src.find("TrayPreferredEngineIndex(") == std::string::npos,
                   "REQ-056 (c): the hook paths no longer pass a preferred-engine "
                   "expression to UpdateStatus (derive-at-open)");
        TEST_CHECK(src.find("REQ-056") != std::string::npos,
                   "REQ-056 (c): the hook paths carry the derive-at-open note");
    }

    // ---- (d) main.cpp: the resolver is the single authority ----------------
    const std::filesystem::path main_cpp = ResolveRepoFile(L"src\\main.cpp");
    if (main_cpp.empty()) {
        std::cout << "[SKIP] src/main.cpp not resolvable; REQ-056 (d) pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(main_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        const size_t resolver_pos = src.find("SetEngineMenuResolver");
        TEST_CHECK(resolver_pos != std::string::npos,
                   "REQ-056 (d): main.cpp publishes the engine-menu resolver");
        // Both resolutions live INSIDE the resolver lambda (scoped finds).
        TEST_CHECK(resolver_pos != std::string::npos &&
                       src.find("TrayPreferredEngineIndex(snap.engine_type)",
                                resolver_pos) != std::string::npos,
                   "REQ-056 (d): the check-index mapping is resolved inside the resolver");
        TEST_CHECK(resolver_pos != std::string::npos &&
                       src.find("LoadDefaultRegistry", resolver_pos) != std::string::npos,
                   "REQ-056 (d): the stem resolution (registry) lives inside the resolver");
        // REQ-054's engaged-optional stem pass is gone with the parameter.
        TEST_CHECK(src.find("std::string_view(user_model_stem)") == std::string::npos,
                   "REQ-056 (d): the engaged-optional stem pass at the UpdateStatus call "
                   "is gone");
        // Wiring precedes the message loop (ShowContextMenu can only fire
        // after both exist).
        const size_t msg_pos = src.find("while (::GetMessageW");
        TEST_CHECK(msg_pos != std::string::npos && resolver_pos < msg_pos,
                   "REQ-056 (d): the resolver is wired before the message loop");

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

    // ---- (f) behavioral: the shared 4-way check-mark mapping ---------------
    // The mapping is pinned here functionally so "user_gguf -> 3" (and every
    // other arm) can never silently drift; REQ-056 leaves main.cpp's resolver
    // as its only consumer.
    TEST_CHECK(emebalachat::TrayPreferredEngineIndex("local") == 1,
               "REQ-054 (f): \"local\" maps to the Local check index (1)");
    TEST_CHECK(emebalachat::TrayPreferredEngineIndex("openai") == 2,
               "REQ-054 (f): \"openai\" maps to the OpenAI check index (2)");
    TEST_CHECK(emebalachat::TrayPreferredEngineIndex("user_gguf") == 3,
               "REQ-054 (f): \"user_gguf\" maps to the 사용자 선택(.gguf) check index (3)");
    TEST_CHECK(emebalachat::TrayPreferredEngineIndex("google") == 0,
               "REQ-054 (f): \"google\" maps to the Google check index (0)");
    TEST_CHECK(emebalachat::TrayPreferredEngineIndex("auto") == 0,
               "REQ-054 (f): \"auto\" maps to the Google check index (0, Google-family)");
    TEST_CHECK(emebalachat::TrayPreferredEngineIndex("bogus") == 0,
               "REQ-054 (f): an unknown engine_type falls back to Google (0)");
    TEST_CHECK(emebalachat::TrayPreferredEngineIndex("") == 0,
               "REQ-054 (f): an empty engine_type falls back to Google (0)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-054/REQ-056 engine-menu checks passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-054/REQ-056 engine-menu checks: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
