// ---------------------------------------------------------------------------
// REQ-048 P1: boot component-check grace re-check (user device P1: the
// "local translation unavailable" modal fired immediately after a fresh
// install while the installer's model download was still in flight). The
// no-repair-URL branch of the main.cpp boot check now waits out a bounded
// grace window (40 x 3 s, first check immediate) on a detached thread before
// converging on the modal; when the components land inside the window the
// modal is suppressed (DIAG ENGINEHOST/bootstrap/017).
//
// The wait loop itself is the linkable unit under test here:
// bs::WaitForComponentsPresent(check, attempts, interval) with an injected
// predicate (no filesystem state, 0 ms interval). main.cpp is the GUI
// entrypoint TU and is deliberately NOT linked into run_tests.exe, so the
// grace wiring is pinned by source inspection (TestReq045EngineUnavailableModal
// precedent) through ResolveRepoFile. Staged as an .inc next to this runner;
// registered near the end of main() after the REQ-047 suites.
// ---------------------------------------------------------------------------

void TestReq048BootGraceRecheck() {
    std::cout << "[RUN] Testing REQ-048 P1 boot grace re-check..." << std::endl;
    const int failures_before = g_failed_count;
    namespace bs = emebalachat::engine_host_bootstrap;

    // 0 ms interval keeps the loop fast while preserving the call cadence
    // (sleep-for(0) between re-checks is a yield, not a stall).
    const std::chrono::milliseconds kZeroMs(0);

    // ---- (a) components already present: true on the FIRST (immediate) check
    {
        int calls = 0;
        const bool arrived = bs::WaitForComponentsPresent([&calls] { ++calls; return true; },
                                                          5, kZeroMs);
        TEST_CHECK(arrived, "REQ-048 P1 (a): check true on the first attempt -> true");
        TEST_CHECK(calls == 1, "REQ-048 P1 (a): exactly 1 check call (immediate, no pre-wait)");
    }

    // ---- (b) installer mid-download: false, false, true -> true at attempt 3
    {
        int calls = 0;
        const bool arrived = bs::WaitForComponentsPresent(
            [&calls] { ++calls; return calls == 3; }, 5, kZeroMs);
        TEST_CHECK(arrived, "REQ-048 P1 (b): components arrive on the 3rd re-check -> true");
        TEST_CHECK(calls == 3, "REQ-048 P1 (b): exactly 3 check calls (no extra re-checks)");
    }

    // ---- (c) genuinely absent: all 5 attempts false -> false, 5 calls
    {
        int calls = 0;
        const bool arrived = bs::WaitForComponentsPresent([&calls] { ++calls; return false; },
                                                          5, kZeroMs);
        TEST_CHECK(!arrived, "REQ-048 P1 (c): never present within the window -> false");
        TEST_CHECK(calls == 5, "REQ-048 P1 (c): all 5 attempts exhausted before giving up");
    }

    // ---- (d) degenerate: attempts <= 0 -> false WITHOUT invoking the check
    {
        int calls = 0;
        const bool arrived = bs::WaitForComponentsPresent([&calls] { ++calls; return true; },
                                                          0, kZeroMs);
        TEST_CHECK(!arrived, "REQ-048 P1 (d): attempts=0 -> false");
        TEST_CHECK(calls == 0, "REQ-048 P1 (d): attempts=0 never invokes the check");
    }

    // ---- (e) header-declaration / link proof: the exact production signature
    // binds at compile time and links against Emebalachat_core (the calls in
    // (a)-(d) already link; this pins the signature shape explicitly). The
    // runtime comparison reads through a volatile so it stays a genuine
    // runtime check (TestReq044I18nFieldOrder C4127 precedent).
    {
        bool (*sig)(const std::function<bool()>&, int, std::chrono::milliseconds) =
            &bs::WaitForComponentsPresent;
        volatile const bool bound = sig != nullptr;
        TEST_CHECK(bound,
                   "REQ-048 P1 (e): WaitForComponentsPresent binds with the declared "
                   "(std::function<bool()>, int, std::chrono::milliseconds) signature");
    }

    // ---- (f) main.cpp structural pins (GUI TU not linked here) -------------
    const std::filesystem::path main_cpp = ResolveRepoFile(L"src\\main.cpp");
    if (main_cpp.empty()) {
        std::cout << "[SKIP] src/main.cpp not resolvable from the test CWD; "
                     "REQ-048 P1 structural pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(main_cpp, std::ios::binary);
        const std::string main_src((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());

        // The grace path exists: the wait helper is invoked from the boot
        // component-check block with the CheckComponents re-check lambda.
        TEST_CHECK(main_src.find("bs::WaitForComponentsPresent") != std::string::npos,
                   "REQ-048 P1 structural pin: main.cpp boot block calls the grace re-check helper");
        TEST_CHECK(main_src.find("CheckComponents().missing.empty()") != std::string::npos,
                   "REQ-048 P1 structural pin: the grace re-check lambda re-probes CheckComponents()");
        // The suppression log line the spec mandates (kept verbatim).
        TEST_CHECK(main_src.find(
                       "bootstrap/017: components arrived within grace window; "
                       "suppressing repair-unavailable modal") != std::string::npos,
                   "REQ-048 P1 structural pin: grace-window suppression logs ENGINEHOST/bootstrap/017");

        // Regression guards — the pre-existing structural pins must survive
        // this edit (TestReq045EngineUnavailableModal pins the same strings).
        TEST_CHECK(main_src.find("ShowLocalEngineUnavailableModal") != std::string::npos,
                   "REQ-048 P1 structural pin: ShowLocalEngineUnavailableModal helper preserved");
        TEST_CHECK(main_src.find("RequestEngineUnavailableModal") != std::string::npos,
                   "REQ-048 P1 structural pin: RequestEngineUnavailableModal marshal seam preserved");
        TEST_CHECK(main_src.find("bs::CheckComponents()") != std::string::npos,
                   "REQ-048 P1 structural pin: bs::CheckComponents() call preserved");
        TEST_CHECK(main_src.find("PostMessageW(hController, kMsgEngineUnavailableModal, 0, 0)") !=
                       std::string::npos,
                   "REQ-048 P1 structural pin: pure (0,0) modal wake-up (SEC-ADJ) preserved");

        // The grace path converges on the SAME modal the URL branch uses —
        // the RepairFailed* wording must still be referenced from the no-URL
        // branch (only its timing changed, not its copy).
        TEST_CHECK(main_src.find("StringId::RepairFailedTitle") != std::string::npos &&
                       main_src.find("StringId::RepairFailedBody") != std::string::npos,
                   "REQ-048 P1 structural pin: grace-exhausted path reuses the RepairFailedTitle/Body wording");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-048 P1 boot grace re-check passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-048 P1 boot grace re-check: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
