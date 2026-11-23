// ---------------------------------------------------------------------------
// 261123_0001 (v0.10.10 rebuild-2 follow-ups): the user_gguf model-missing
// route decision table (Bug A), the fresh-create OS-language seeding (Bug B),
// and the i18n wiring pins for the +1 StringId UserModelLoadFailedHint plus
// the GgufManagerTogglePolicy caption rename (Bug C). Staged as an .inc next
// to this runner; registered at the end of main() after
// r4_fix_tests::R4RegisterTests(), per the end-of-file pattern.
//
// Coverage:
//   (a) Test261123UserGgufMissingRoute. The header-inline pure decision
//       RouteUserGgufModelMissing (src/engine.hpp, the
//       StrictLocalFailureStatus precedent): all four
//       (pinned_model_file_present, load_failure_manager_already_shown_this_session)
//       combinations assert the documented 261123_0001 outcomes. (The E2
//       StrictLocalFailureStatus mapping itself is already pinned by
//       TestP3C4StrictLocalFailureStatus, tests/p3_c4_tests.inc; not
//       duplicated here.)
//   (b) Test261123FreshCreateOsSeed. Real-temp-dir LoadFromFile pins: a
//       missing explicit path auto-creates the file and seeds the drag pair
//       from the TRUE OS language (DetectSystemLocale + LocaleToString, the
//       same Initialize-independent source the production
//       ResolveDragDefaultTarget has used since 261123_0001), while an
//       existing file keeps its explicit values verbatim (no reseed).
//   (c) Test261123I18nWiringPins. StringId::UserModelLoadFailedHint resolves
//       non-empty in every selectable locale (the R6 P5 selector-vector loop,
//       self-scaling with the authenticity gate), and the Korean table
//       carries the authored KO hint literal plus the renamed policy caption
//       "로드 방식 전환". (EnumCount == 248 is already pinned at ten sites by
//       the 261123 i18n micro-pass; not duplicated here.)
// ---------------------------------------------------------------------------

namespace {

// The shared OS-language expectation for the 261123 Bug B pins: derived from
// the SAME Initialize-independent source the production
// ResolveDragDefaultTarget uses (src/config.cpp), never from the app-UI-locale
// seam (I18n::GetSystemLanguageCode) and never by calling the function under
// test.
std::string F261123ExpectedDragDefault() {
    const std::string sys_code =
        NormalizeLanguageCode(I18n::LocaleToString(I18n::DetectSystemLocale()));
    const LanguageInfo* sys_info = FindLanguageByCode(sys_code);
    return (!sys_info || sys_info->code == "AUTO") ? "English" : sys_info->name_en;
}

}  // namespace

// ---- (a) Bug A: the user_gguf model-missing route decision table -----------

void Test261123UserGgufMissingRoute() {
    std::cout << "[RUN] Testing 261123_0001 user_gguf model-missing route decision table..."
              << std::endl;
    const int failures_before = g_failed_count;

    // (file_present=false, shown=false): a file-missing miss is a REAL state
    // change (unregistered / deleted / moved model) - the manager is the
    // right fix the first time.
    TEST_CHECK(RouteUserGgufModelMissing(false, false) == UserGgufMissingRoute::kOpenManager,
               "261123 A: file missing + manager never shown -> kOpenManager");
    // (false, true): the file-missing arm is NEVER latched - a later deletion
    // re-opens the manager even after a load-failure session already showed it.
    TEST_CHECK(RouteUserGgufModelMissing(false, true) == UserGgufMissingRoute::kOpenManager,
               "261123 A: file missing + load-failure manager already shown -> kOpenManager "
               "(never latched)");
    // (true, false): the file EXISTS but the host could not serve it (the
    // HF-incompatible-model class) - the first miss opens the manager and the
    // caller arms the sticky per-session flag.
    TEST_CHECK(RouteUserGgufModelMissing(true, false) == UserGgufMissingRoute::kOpenManager,
               "261123 A: file present + first load failure -> kOpenManager");
    // (true, true): the same load-failure state recurred - degrade to the
    // quiet tooltip hint, no modal re-open on every translation.
    TEST_CHECK(RouteUserGgufModelMissing(true, true) == UserGgufMissingRoute::kShowTooltipHint,
               "261123 A: file present + repeated load failure -> kShowTooltipHint");
    // The caller switches on the enum: the two routes must be distinct values.
    // (volatile witnesses keep the comparison runtime-evaluated - a folded
    // constant comparison trips C4127 in the TEST_CHECK if.)
    volatile UserGgufMissingRoute distinct_a = UserGgufMissingRoute::kOpenManager;
    volatile UserGgufMissingRoute distinct_b = UserGgufMissingRoute::kShowTooltipHint;
    TEST_CHECK(distinct_a != distinct_b,
               "261123 A: the two routes are distinct values");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261123 user_gguf route decision table" << std::endl;
    } else {
        std::cout << "[FAIL] 261123 user_gguf route decision table: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (b) Bug B: fresh-create seeds the TRUE OS language; sticky preserved ---

void Test261123FreshCreateOsSeed() {
    std::cout << "[RUN] Testing 261123_0001 fresh-create OS-language seeding..." << std::endl;
    const int failures_before = g_failed_count;

    const std::string expected_drag_default = F261123ExpectedDragDefault();

    std::error_code ec;
    const std::filesystem::path base =
        std::filesystem::temp_directory_path(ec) / "emebalachat_261123_followup";
    if (!ec) std::filesystem::remove_all(base, ec); // stale leftovers must not mask a failure
    ec.clear();
    if (!ec) std::filesystem::create_directories(base, ec);

    // ---- 1) Missing file: LoadFromFile auto-creates AND seeds the OS language
    //         (nested "sub" dir: SaveToFileLocked must create the directory).
    {
        const std::filesystem::path cfg = base / "sub" / "config.json";
        AppConfig fresh;
        TEST_CHECK(fresh.LoadFromFile(cfg),
                   "261123 B: LoadFromFile(missing explicit path) succeeds and auto-creates");
        const bool created = std::filesystem::exists(cfg, ec) && !ec;
        ec.clear();
        TEST_CHECK(created, "261123 B: the config file was created on disk (incl. nested dir)");
        const auto snap = fresh.GetSnapshot();
        TEST_CHECK(snap.drag_target_language == expected_drag_default,
                   "261123 B: fresh-create drag target seeds the TRUE OS language (REQ-007), "
                   "not the app-UI-locale default");
        TEST_CHECK(snap.drag_source_language == "Auto Detect",
                   "261123 B: fresh-create drag source is Auto Detect (REQ-006)");
        TEST_CHECK(snap.type_source_language == "Auto Detect",
                   "261123 B: fresh-create type source is Auto Detect (REQ-015)");
        TEST_CHECK(snap.type_target_language == "English",
                   "261123 B: fresh-create type target is English (REQ-016)");
        // The seeded value must have been PERSISTED (the save writes the sticky
        // keys), not only held in memory - a reload sees the same seed.
        AppConfig reloaded;
        TEST_CHECK(reloaded.LoadFromFile(cfg),
                   "261123 B: the auto-created file reloads");
        TEST_CHECK(reloaded.GetSnapshot().drag_target_language == expected_drag_default,
                   "261123 B: the seeded OS language persisted to disk (sticky on restart)");
        std::filesystem::remove_all(base / "sub", ec);
        ec.clear();
    }

    // ---- 2) Existing file with explicit values: kept verbatim, NO reseed.
    {
        const std::filesystem::path dir = base / "sticky";
        std::filesystem::create_directories(dir, ec);
        ec.clear();
        const std::filesystem::path cfg = dir / "config.json";
        {
            std::ofstream out(cfg, std::ios::binary);
            out << "{\n"
                   "  \"drag_source_language\": \"Auto Detect\",\n"
                   "  \"drag_target_language\": \"Vietnamese\",\n"
                   "  \"type_source_language\": \"Japanese\",\n"
                   "  \"type_target_language\": \"English\"\n"
                   "}\n";
        }
        AppConfig loaded;
        TEST_CHECK(loaded.LoadFromFile(cfg),
                   "261123 B: LoadFromFile(existing explicit path) parses");
        const auto snap = loaded.GetSnapshot();
        TEST_CHECK(snap.drag_target_language == "Vietnamese",
                   "261123 B: an existing drag target is preserved verbatim (no OS reseed)");
        TEST_CHECK(snap.drag_source_language == "Auto Detect",
                   "261123 B: the existing drag source is preserved verbatim");
        TEST_CHECK(snap.type_source_language == "Japanese",
                   "261123 B: the existing type source is preserved verbatim");
        TEST_CHECK(snap.type_target_language == "English",
                   "261123 B: the existing type target is preserved verbatim (REQ-016)");
        std::filesystem::remove_all(dir, ec);
        ec.clear();
    }

    // Cleanup: the whole temp dir is this suite's own artifact (best-effort).
    std::filesystem::remove_all(base, ec);

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261123 fresh-create OS-language seeding" << std::endl;
    } else {
        std::cout << "[FAIL] 261123 fresh-create OS-language seeding: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (c) Bug C / i18n: the +1 id wiring + the renamed policy caption --------

void Test261123I18nWiringPins() {
    std::cout << "[RUN] Testing 261123_0001 i18n wiring pins (UserModelLoadFailedHint + "
                 "policy caption)..."
              << std::endl;
    const int failures_before = g_failed_count;
    const UiLocale before = I18n::GetCurrentLocale();

    // ---- 1) UserModelLoadFailedHint resolves non-empty in EVERY selectable
    //         locale (the R6 P5 selector-vector loop, self-scaling with the
    //         authenticity gate rather than a hardcoded 89).
    const auto& locales = GetSupportedUiLocales();
    TEST_CHECK(static_cast<int>(locales.size()) == AuthoredUiLocaleCount(),
               "261123 C: the selector holds exactly the authored mapping rows");
    for (const auto& e : locales) {
        I18n::SetLocale(e.locale);
        TEST_CHECK(!I18n::Get(StringId::UserModelLoadFailedHint).empty(),
                   "261123 C: UserModelLoadFailedHint non-empty in every selectable locale");
    }

    // ---- 2) Korean carries the AUTHORED KO hint literal (not the EN draft).
    I18n::SetLocale(UiLocale::Korean);
    TEST_CHECK(I18n::Get(StringId::UserModelLoadFailedHint) ==
                   L"사용자 모델을 불러오지 못했습니다. 모델 관리에서 파일과 호환성을 확인하세요.",
               "261123 C: the Korean table carries the authored KO user-model load-failure hint");

    // ---- 3) The Bug C caption rename: Korean alone carries "로드 방식 전환".
    TEST_CHECK(I18n::Get(StringId::GgufManagerTogglePolicy) == L"로드 방식 전환",
               "261123 C: the Korean policy caption is renamed to '로드 방식 전환'");

    I18n::SetLocale(before); // restore the shared locale state for later suites

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261123 i18n wiring pins" << std::endl;
    } else {
        std::cout << "[FAIL] 261123 i18n wiring pins: " << (g_failed_count - failures_before)
                  << " check(s) failed." << std::endl;
    }
}

// ---- (d) Bug A integration rule: the two-latch wire, pinned at source ------
//
// 261123_0001 R2 (fix F-06): the decision table above proves the pure rule but
// cannot see the WIRING, and main.cpp is not linked into run_tests.exe (the
// TestHookSelfHealResilience seam), so the two-latch contract is pinned on the
// source text of RequestModelMissingOfferOrModal plus the kMsgOpenGgufManager
// drain handler:
//   * ONE arm site in the whole file, guarded by `if (file_present)` (the
//     load-failure arm; a file-missing open never arms it), and it arms AFTER
//     the manager-open post (261123_0001 R2 fix F-02: a failed post must not
//     burn the session's one manager open);
//   * nothing in src/main.cpp ever stores false into the session latch, so the
//     drain handler cannot clear it (that handler releases only the
//     coalescing latch).

void Test261123BugATwoLatchSourcePins() {
    std::cout << "[RUN] Testing 261123_0001 Bug A two-latch source pins..." << std::endl;
    const int failures_before = g_failed_count;

    std::string main_src;
    const bool have_src = ReadRepoSrc(L"src\\main.cpp", main_src);
    TEST_CHECK(have_src, "261123 A/F-06: src/main.cpp is readable for the two-latch pins");

    if (have_src) {
        const auto count_of = [](const std::string& haystack, const std::string& needle) {
            std::size_t n = 0;
            for (std::size_t at = haystack.find(needle); at != std::string::npos;
                 at = haystack.find(needle, at + needle.size())) {
                ++n;
            }
            return n;
        };

        const std::string kDecl =
            "std::atomic<bool> g_gguf_manager_load_failure_shown{false};";
        const std::string kArm = "g_gguf_manager_load_failure_shown.store(";
        const std::string kFnSig =
            "void RequestModelMissingOfferOrModal(std::string_view engine_type";
        const std::string kHandlerOpen = "if (msg == kMsgOpenGgufManager) {";

        // ---- 1) one declaration, one true-valued arm site, never cleared ----
        TEST_CHECK(count_of(main_src, kDecl) == 1,
                   "261123 A/F-06: the session latch is declared exactly once");
        TEST_CHECK(count_of(main_src, kArm) == 1,
                   "261123 A/F-06: src/main.cpp holds exactly one store site for the session "
                   "latch (armed only on the file-present load-failure arm)");
        TEST_CHECK(main_src.find("g_gguf_manager_load_failure_shown.store(true, "
                                "std::memory_order_release)") != std::string::npos,
                   "261123 A/F-06: the single arm site stores true (release)");
        TEST_CHECK(main_src.find("g_gguf_manager_load_failure_shown.store(false") ==
                       std::string::npos,
                   "261123 A/F-06: nothing in src/main.cpp ever clears the session latch "
                   "(it resets only with the process)");

        // ---- 2) the arm sits in the load-failure arm, after a successful post --
        const std::size_t fn_at = main_src.find(kFnSig);
        const std::size_t handler_at = main_src.find(kHandlerOpen);
        const std::size_t store_at = main_src.find(kArm);
        const bool located = fn_at != std::string::npos && handler_at != std::string::npos &&
                             store_at != std::string::npos && fn_at < handler_at;
        TEST_CHECK(located,
                   "261123 A/F-06: the offer helper, the drain handler and the arm site are "
                   "locatable in that order");
        if (located) {
            TEST_CHECK(store_at > fn_at && store_at < handler_at,
                       "261123 A/F-06: the arm site lives inside the offer helper and not in "
                       "the drain handler");
            // The nearest enclosing guard must be the load-failure arm. The
            // block close is searched as "\n        }" (no trailing-newline
            // dependency, so the pin holds whatever the checkout's EOL class).
            const std::size_t guard_at = main_src.rfind("if (file_present) {", store_at);
            const std::size_t guard_close = guard_at == std::string::npos
                                                ? std::string::npos
                                                : main_src.find("\n        }", guard_at);
            TEST_CHECK(guard_at != std::string::npos && guard_at > fn_at && guard_at < store_at,
                       "261123 A/F-06: the arm site is guarded by `if (file_present)` (a "
                       "file-missing open never arms the session latch)");
            TEST_CHECK(guard_at != std::string::npos && guard_close != std::string::npos &&
                           store_at < guard_close,
                       "261123 A/F-06: the arm site is INSIDE the file_present block");
            // 261123_0001 R2 (fix F-02): arm-after-post. The helper's own
            // wake-up post precedes the arm and its failure branch returns, so
            // a failed PostMessageW leaves the session latch unarmed.
            const std::size_t post_at =
                main_src.find("kMsgOpenGgufManager, 0, 0) == FALSE", fn_at);
            TEST_CHECK(post_at != std::string::npos && post_at > fn_at && post_at < store_at,
                       "261123 A/F-06: the session latch arms AFTER the manager-open post "
                       "(fix F-02)");
            TEST_CHECK(post_at != std::string::npos && post_at < store_at &&
                           main_src.substr(post_at, store_at - post_at).find("return;") !=
                               std::string::npos,
                       "261123 A/F-06: the post-failure branch returns before the arm, so a "
                       "failed post cannot arm the session latch (fix F-02)");
            // ---- 3) the drain handler releases only the coalescing latch ----
            const std::size_t body_end = main_src.find("\n        return 0;", handler_at);
            TEST_CHECK(body_end != std::string::npos && body_end > handler_at,
                       "261123 A/F-06: the drain handler body is delimited by its return 0");
            if (body_end != std::string::npos && body_end > handler_at) {
                const std::string handler_body =
                    main_src.substr(handler_at, body_end - handler_at);
                TEST_CHECK(handler_body.find(kArm) == std::string::npos,
                           "261123 A/F-06: the kMsgOpenGgufManager drain handler never stores "
                           "the session latch");
                TEST_CHECK(handler_body.find("g_gguf_manager_offer_posted.store(false") !=
                               std::string::npos,
                           "261123 A/F-06: the drain handler does release the coalescing latch "
                           "(positive control: the slice is the real handler)");
            }
            const std::size_t route_at = main_src.find("RouteUserGgufModelMissing(", fn_at);
            TEST_CHECK(route_at != std::string::npos && route_at < handler_at,
                       "261123 A/F-06: the helper classifies through RouteUserGgufModelMissing "
                       "(positive control: the slice is the real helper)");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261123 Bug A two-latch source pins" << std::endl;
    } else {
        std::cout << "[FAIL] 261123 Bug A two-latch source pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
