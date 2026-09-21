// ---------------------------------------------------------------------------
// REQ-051 (session 260921, symptom B): the local transient-failure retry
// policy (classification + single-retry warrant) and the Enter-path
// EngineFailed streak surfacing. Staged as an .inc next to this runner;
// registered near the end of main() after TestReq051CaptureRetryPolicy.
//
// Coverage:
//   (a) ClassifyEngineHostFailure over the frozen client's failure-contract
//       codes: connect/busy/io/timeout are Transient; disabled/
//       no_host_binary/unauthorized/version_mismatch/pin_mismatch/
//       bad_request/model_missing/engine_failed are Permanent; empty and
//       unknown codes fail PERMANENT (fail-safe).
//   (b) EngineHostTransientRetryWarranted matrix: transient + budget + fast
//       first attempt + no cancel -> retry; each guard independently kills
//       it (permanent code, spent budget, over-ceiling first attempt,
//       REQ-R16 cancel). Boundary pins at the ceiling (15 s since the
//       REQ-051 U-1 FIX 1 raise — the pins track the shipped constant).
//   (c) The constants: ONE retry, 400 ms backoff (the 300-500 ms handoff
//       band), 15 s first-attempt ceiling (clears the worker's 12 s decode
//       wall-clock timeout inside the frozen 30 s budget).
//   (d) EnterEngineFailureSurfacesModal: strict statuses keep the REQ-047
//       threshold-1 surface; EngineFailed surfaces only on a local leg at
//       the streak threshold; a pure-cloud leg never surfaces.
//   (e) ABSOLUTE SECURITY BOUNDARY (structural pins on engine.cpp): the
//       retry block never invokes the cloud/openai seams, the post-retry
//       convergence still reaches CloudConsentBlocked under a strict-local
//       pick (text stays on-device), and the REQ-R16 cancel re-check guards
//       the post-sleep retry. Same source-inspection technique as
//       TestReq048CloudFailureSignaling / TestReq051CaptureRetryPolicy.
// ---------------------------------------------------------------------------

void TestReq051EngineRetryPolicy() {
    std::cout << "[RUN] Testing REQ-051 engine retry policy..." << std::endl;
    const int failures_before = g_failed_count;
    using emebalachat::EngineHostFailureClass;

    // ---- (a) transient vs permanent classification -------------------------
    TEST_CHECK(ClassifyEngineHostFailure("connect") == EngineHostFailureClass::Transient &&
                   ClassifyEngineHostFailure("busy") == EngineHostFailureClass::Transient &&
                   ClassifyEngineHostFailure("io") == EngineHostFailureClass::Transient &&
                   ClassifyEngineHostFailure("timeout") == EngineHostFailureClass::Transient,
               "REQ-051: connect/busy/io/timeout classify as TRANSIENT");
    TEST_CHECK(ClassifyEngineHostFailure("disabled") == EngineHostFailureClass::Permanent &&
                   ClassifyEngineHostFailure("no_host_binary") == EngineHostFailureClass::Permanent &&
                   ClassifyEngineHostFailure("unauthorized") == EngineHostFailureClass::Permanent &&
                   ClassifyEngineHostFailure("version_mismatch") == EngineHostFailureClass::Permanent &&
                   ClassifyEngineHostFailure("pin_mismatch") == EngineHostFailureClass::Permanent,
               "REQ-051: handshake/deployment codes are PERMANENT");
    TEST_CHECK(ClassifyEngineHostFailure("bad_request") == EngineHostFailureClass::Permanent &&
                   ClassifyEngineHostFailure("model_missing") == EngineHostFailureClass::Permanent &&
                   ClassifyEngineHostFailure("engine_failed") == EngineHostFailureClass::Permanent,
               "REQ-051: request/decode codes are PERMANENT (retrying a real decode failure only reloads)");
    TEST_CHECK(ClassifyEngineHostFailure("") == EngineHostFailureClass::Permanent &&
                   ClassifyEngineHostFailure("bogus_code") == EngineHostFailureClass::Permanent,
               "REQ-051: empty/unknown codes fail PERMANENT (fail-safe)");

    // ---- (b) the retry warrant matrix --------------------------------------
    TEST_CHECK(EngineHostTransientRetryWarranted("connect", 0, 2000, false),
               "REQ-051: transient fast failure warrants the one retry");
    TEST_CHECK(!EngineHostTransientRetryWarranted("connect", 1, 2000, false),
               "REQ-051: the retry budget (1) is never exceeded");
    TEST_CHECK(!EngineHostTransientRetryWarranted("busy", 0, 30500, false),
               "REQ-051: a first attempt that burned the 30 s budget does NOT "
               "retry (converges to the honest modal instead)");
    // REQ-051 U-1 FIX 1 (session 260922): the ceiling moved 10 s -> 15 s so
    // the retry still fires after the worker-side 12 s decode wall-clock
    // timeout (12 s + 400 ms + 12 s ~= 25 s < the frozen 30 s budget). The
    // boundary pins moved WITH the constant — they exist to catch silent
    // drift, not to freeze a superseded value.
    TEST_CHECK(EngineHostTransientRetryWarranted("io", 0, 15000, false),
               "REQ-051: the first-attempt ceiling is inclusive (15 s)");
    TEST_CHECK(!EngineHostTransientRetryWarranted("connect", 0, 15001, false),
               "REQ-051: past the ceiling the retry is refused");
    TEST_CHECK(!EngineHostTransientRetryWarranted("connect", 0, 2000, true),
               "REQ-051: a REQ-R16 cancel intent outranks the retry");
    TEST_CHECK(!EngineHostTransientRetryWarranted("model_missing", 0, 100, false),
               "REQ-051: a permanent code never warrants a retry");

    // ---- (c) the policy constants -------------------------------------------
    volatile int retry_max = kEngineHostTransientRetryMax;
    TEST_CHECK(retry_max == 1,
               "REQ-051: exactly ONE identical-input retry (never a loop)");
    volatile uint32_t backoff = kEngineHostTransientRetryBackoffMs;
    TEST_CHECK(backoff >= 300 && backoff <= 500,
               "REQ-051: the retry backoff stays inside the 300-500 ms band");
    volatile uint64_t ceiling = kEngineHostRetryFirstAttemptCeilingMs;
    TEST_CHECK(ceiling == 15000,
               "REQ-051 U-1 FIX 1: the fast-failure ceiling is 15 s (clears "
               "the 12 s decode wall-clock timeout)");

    // ---- (d) the Enter-path EngineFailed streak surfacing ------------------
    volatile int threshold = kEnterEngineFailureStreakThreshold;
    TEST_CHECK(threshold == 3,
               "REQ-051: the EngineFailed streak threshold is 3 consecutive failures");
    TEST_CHECK(EnterEngineFailureSurfacesModal(true, false, 1) &&
                   EnterEngineFailureSurfacesModal(true, true, 1),
               "REQ-051: strict local statuses keep the REQ-047 threshold-1 surface");
    TEST_CHECK(!EnterEngineFailureSurfacesModal(false, true, 2),
               "REQ-051: a single transient EngineFailed never surfaces");
    TEST_CHECK(EnterEngineFailureSurfacesModal(false, true, 3) &&
                   EnterEngineFailureSurfacesModal(false, true, 6),
               "REQ-051: a local-leg EngineFailed streak surfaces at/after the threshold");
    TEST_CHECK(!EnterEngineFailureSurfacesModal(false, false, 3) &&
                   !EnterEngineFailureSurfacesModal(false, false, 20),
               "REQ-051: a pure-cloud leg (explicit google/openai) never surfaces "
               "the local modal");

    // ---- (e) the never-newly-cloud boundary (structural) --------------------
    const std::filesystem::path eng_path = ResolveRepoFile(L"src\\engine.cpp");
    TEST_CHECK(!eng_path.empty(),
               "REQ-051: src/engine.cpp located via ResolveRepoFile");
    if (!eng_path.empty()) {
        std::ifstream eng_in(eng_path, std::ios::binary);
        TEST_CHECK(static_cast<bool>(eng_in),
                   "REQ-051: src/engine.cpp opened for reading");
        if (eng_in) {
            const std::string eng_src((std::istreambuf_iterator<char>(eng_in)),
                                      std::istreambuf_iterator<char>());
            TEST_CHECK(!eng_src.empty(), "REQ-051: src/engine.cpp non-empty after read");

            // The retry block, bounded from the first-attempt timing variable
            // (the start of the retry-decision region, BEFORE the /046 log) to
            // the /047 log. The warrant evaluation sits between them: the
            // product computes the first-attempt elapsed, evaluates
            // EngineHostTransientRetryWarranted, logs /046 only when the
            // warrant passed, then sleeps and re-runs the local leg.
            const size_t retry_pos = eng_src.find("const uint64_t first_attempt_ms");
            const size_t retry_end = eng_src.find("ENGINE/Translate/047");
            TEST_CHECK(retry_pos != std::string::npos && retry_end != std::string::npos &&
                           retry_end > retry_pos,
                       "REQ-051 pin: the transient retry block is locatable");
            if (retry_pos != std::string::npos && retry_end != std::string::npos &&
                retry_end > retry_pos) {
                const std::string retry_body =
                    eng_src.substr(retry_pos, retry_end - retry_pos);
                TEST_CHECK(retry_body.find("EngineHostTransientRetryWarranted(") !=
                               std::string::npos,
                           "REQ-051 pin: the retry is gated by the pure warrant");
                TEST_CHECK(retry_body.find("::Sleep(kEngineHostTransientRetryBackoffMs)") !=
                               std::string::npos,
                           "REQ-051 pin: the retry uses the 300-500 ms backoff constant");
                TEST_CHECK(retry_body.find("cancel_requested_.load") != std::string::npos,
                           "REQ-051 pin: the REQ-R16 cancel re-check guards the retry");
                // ABSOLUTE SECURITY BOUNDARY: the retry block only re-runs the
                // local engine-host leg - it must never touch the cloud/openai
                // seams (a strict-local pick with cloud_fallback_enabled=false
                // can therefore never newly transmit text).
                TEST_CHECK(retry_body.find("cloud_call") == std::string::npos &&
                               retry_body.find("openai_call") == std::string::npos &&
                               retry_body.find("GoogleTranslate::Translate") == std::string::npos,
                           "REQ-051 pin: the retry block NEVER invokes the cloud/openai seams");
            }

            // The convergence after the retry still reaches the strict-local
            // honest failure (CloudConsentBlocked) - the security boundary on
            // the far side of the retry.
            const size_t warrant_pos = eng_src.find("EngineHostTransientRetryWarranted(");
            const size_t strict_block = eng_src.find(
                "set_status(TranslationStatus::CloudConsentBlocked);");
            TEST_CHECK(warrant_pos != std::string::npos && strict_block != std::string::npos &&
                           warrant_pos < strict_block,
                       "REQ-051 pin: the strict-local CloudConsentBlocked convergence "
                       "survives AFTER the retry (text stays on-device without consent)");
            // And the consented cloud legs still exist beyond it (Auto /
            // explicit fallback unchanged - the retry only DELAYS them).
            const size_t auto_fallback = eng_src.find("return cloud_call();", retry_end);
            TEST_CHECK(auto_fallback != std::string::npos,
                       "REQ-051 pin: the consented Auto/fallback cloud legs remain");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-051 engine retry policy passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-051 engine retry policy: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
