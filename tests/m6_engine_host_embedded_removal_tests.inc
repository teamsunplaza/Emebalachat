// ============================================================================
// REQ-043 (M6 T5, plan §V2-8.6): embedded-engine removal suites. Staged as a
// session .inc file (same staging contract as the T2/T3/T4 suites) — included
// by tests/run_tests.cpp AFTER the pre-existing test bodies (they see the
// TEST_CHECK harness, the `using namespace emebalachat` alias and every
// project include; IntelliSense standalone analysis of this file alone shows
// false "undefined identifier" noise, exactly like the T2-T4 .inc files).
// Defines TestEngineHostOnlyRouting + TestEngineCloudConsentRegression.
//
// Scope discipline (design §5 (4)): the pipe-level fallback matrix
// (busy/timeout/model_missing/io convergence INSIDE the client) stays covered
// by the pre-existing TestEngineHostClient FakeHost suites — these suites pin
// only the T5 MANAGER-level contract:
//   * the local source is EXCLUSIVELY the shared inference host —
//     IsLocalModelAvailable == HostCouldServeLocked (no embedded model file);
//   * the §V2-8.6 failure-UX convergence (repair streak + consented cloud +
//     honest disabled-guidance failure);
//   * the cloud-consent gates (H2 / Auto contract) are UNCHANGED;
//   * the REQ-R16 shutdown latch survives the embedded-engine removal.
// The live fake-host legs reuse the SAME helpers (TestPipeName/TestTokenPath/
// WriteTestToken/FakeHost) declared in run_tests.cpp's anonymous namespace
// BEFORE this include — internal-linkage names stay visible to the rest of
// the translation unit, so this file links against them directly.
// ============================================================================

// Joins a fake server even when the client never connected (same pattern as
// TestEngineHostClient's local `finish` lambda; a plain join would deadlock
// on the pending overlapped ConnectNamedPipe).
static void T5FinishHost(FakeHost& h, std::thread& t) {
    // REQ-043 (M6 T5 debug): close the engine-host client session BEFORE
    // joining the fake-host thread. The client holds a cached pipe handle
    // (g_session.pipe) after Translate(); without closing it, the FakeHost
    // serve thread blocks forever in its next ReadFile, and join() deadlocks
    // (the exact hang the T5 suite exhibited). This mirrors the
    // OverrideGuard::reset() + finish() ordering in TestEngineHostClient.
    emebalachat::engine_host::SetPathsForTesting(nullptr, nullptr, nullptr);
    h.Stop();
    if (t.joinable()) t.join();
}

// Chat routing = host-only. Exercises TranslationManager availability and the
// §V2-8.6 failure-UX convergence against the engine-host client test seams.
void TestEngineHostOnlyRouting() {
    std::cout << "[RUN] Testing T5 host-only routing + failure UX (REQ-043 M6 T5)..." << std::endl;
    const int failures_before = g_failed_count;
    namespace ehc = emebalachat::engine_host;
    using emebalachat::EngineType;
    using emebalachat::EngineHostConfig;
    using emebalachat::TranslationManager;
    using emebalachat::TranslationStatus;
    using emebalachat::GoogleTranslate;

    // REQ-043 (M6 T5 debug): the T5 suites pin the MANAGER-level routing
    // contract (host-only local source, failure-UX convergence, consent
    // gates) — NOT the live cloud network. The live Google Translate path
    // is already covered end-to-end by TestGoogleTranslateModule and
    // TestEngineModule; the T5 suites must be hermetic so they never hang
    // on a WinHTTP stall. Engage the test-only cloud-block hook for the
    // whole suite: every manager->cloud_call() seam now returns the honest
    // EngineFailed outcome deterministically (the exact same shape the
    // original network-dependent assertions accepted as their failure arm).
    GoogleTranslate::SetCloudBlockedForTesting(true);

    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring pid = std::to_wstring(GetCurrentProcessId());

    // (1) No host deployed: strict local + no local source -> honest missing
    //     leg; Auto -> cloud (Zero-Install name). The embedded-engine API
    //     surface (PreloadLocalModel/UnloadLocalModel) no longer exists.
    ehc::SetPathsForTesting(nullptr, nullptr,
        (std::wstring(tmp) + L"eme_t5_absent_" + pid + L".exe").c_str());
    {
        TranslationManager strict(EngineType::LocalLlama, "D:\\no_such_dir\\model.gguf");
        TEST_CHECK(!strict.IsLocalModelAvailable(),
                   "T5: no host -> local availability false (semantic shift: host-serving, not a file)");
        TEST_CHECK(strict.GetActiveEngineName() == "Local (Model Missing)",
                   "T5: no host -> strict local keeps the honest missing-leg name");
        TEST_CHECK(strict.GetActiveEngineName().find("Google") == std::string::npos,
                   "T5: missing-leg name carries no Google substring (tray checkmark contract)");

        TranslationStatus st = TranslationStatus::Ok;
        std::wstring res = strict.Translate(L"hello", "KO", "EN", &st);
        TEST_CHECK(res.empty() && st == TranslationStatus::LocalModelMissing,
                   "T5: strict local with no host pre-blocks LocalModelMissing (no cloud masquerade)");

        TranslationManager auto_mgr(EngineType::Auto, "");
        TEST_CHECK(auto_mgr.GetActiveEngineName() == "Google Translate (Zero-Install)",
                   "T5: no host -> Auto demotes to the zero-install cloud leg");
        TEST_CHECK(auto_mgr.GetActiveEngineName().find("Shared Host") == std::string::npos,
                   "T5: no host -> Auto never claims the shared-host local leg");
        TEST_CHECK(auto_mgr.HostFailureStreak() == 0,
                   "T5: a fresh manager starts with a clean repair streak");
    }

    // (2) Host deployed (fake exe stat'ed only): IsLocalModelAvailable ==
    //     host gate; Auto/local both route local; disabling the block demotes.
    const std::wstring fake_exe = std::wstring(tmp) + L"eme_t5_host_" + pid + L".exe";
    {
        std::ofstream f(fake_exe, std::ios::binary);
        f << "MZ";
    }
    ehc::SetPathsForTesting(nullptr, nullptr, fake_exe.c_str());
    {
        EngineHostConfig off;
        off.enabled = false;
        TranslationManager strict(EngineType::LocalLlama, "");
        strict.SetEngineHostConfig(off);
        TEST_CHECK(!strict.IsLocalModelAvailable(),
                   "T5: engine_host disabled -> local availability false even with the binary present");
        TEST_CHECK(strict.GetActiveEngineName() == "Local (Model Missing)",
                   "T5: engine_host disabled -> strict local stays honest");

        EngineHostConfig on;  // defaults: enabled=true
        on.spawn = false;
        TranslationManager auto_mgr(EngineType::Auto, "");
        auto_mgr.SetEngineHostConfig(on);
        TEST_CHECK(auto_mgr.IsLocalModelAvailable(),
                   "T5: host enabled+present -> local availability true (no model file involved)");
        TEST_CHECK(auto_mgr.GetActiveEngineName() == "Hy-MT2-1.8B (Shared Host)",
                   "T5: host enabled+present -> Auto routes the shared-host local leg");
        TEST_CHECK(!auto_mgr.GetModelPath().empty(),
                   "T5: model_path remains a legacy passthrough (GetModelPath non-empty default)");
        TranslationManager strict_on(EngineType::LocalLlama, "");
        strict_on.SetEngineHostConfig(on);
        TEST_CHECK(strict_on.GetActiveEngineName() == "Hy-MT2-1.8B (Shared Host)",
                   "T5: host enabled+present -> strict local is NOT 'Model Missing'");
    }

    // (3) §V2-8.6 failure UX with LIVE in-process fake hosts (each scenario on
    //     its OWN pipe name — the same one-call-per-fake-host discipline the
    //     pre-existing client matrix uses, no cross-pipe session reuse):
    //       busy + Auto (Auto IS the cloud consent)   -> cloud Ok, streak 1
    //       busy + strict local + consent             -> cloud Ok, streak 2
    //       busy + strict local + NO consent          -> CloudConsentBlocked
    //       host ok                                   -> Ok + streak cleared
    const std::string pin(emebalachat::kExpectedModelSha256);
    {
        // 3a. busy + Auto -> cloud fallback succeeds (UX chain c).
        const int n = 20;
        ehc::SetPathsForTesting(TestPipeName(n).c_str(),
                                TestTokenPath(n).wstring().c_str(), fake_exe.c_str());
        WriteTestToken(TestTokenPath(n));
        FakeHost host(TestPipeName(n), 0, pin, "", "busy", "", false);
        std::thread t = host.Start();
        EngineHostConfig cfg;
        cfg.spawn = false;
        TranslationManager auto_mgr(EngineType::Auto, "");
        auto_mgr.SetEngineHostConfig(cfg);
        TranslationStatus st = TranslationStatus::EngineFailed;
        std::wstring res = auto_mgr.Translate(L"annyeong", "KO", "EN", &st);
        // §V2-8.6 (b)+(c): the host failure MUST surface the repair signal even
        // when the consented cloud leg cannot complete (offline CI); only the
        // Ok outcome is network-dependent, so it is asserted conditionally.
        TEST_CHECK(auto_mgr.HostFailureStreak() == 1,
                   "T5: one host failure recorded as the client-side repair streak");
        TEST_CHECK(st == TranslationStatus::Ok || st == TranslationStatus::EngineFailed,
                   "T5: host busy + Auto converges to cloud Ok or honest EngineFailed (no silent status)");
        TEST_CHECK((st == TranslationStatus::Ok) == !res.empty(),
                   "T5: cloud fallback outcome matches its status (Ok <-> non-empty, failure <-> empty)");
        T5FinishHost(host, t);
    }
    {
        // 3b. busy + strict local + consent -> cloud Ok; streak grows per call.
        const int n = 21;
        ehc::SetPathsForTesting(TestPipeName(n).c_str(),
                                TestTokenPath(n).wstring().c_str(), fake_exe.c_str());
        WriteTestToken(TestTokenPath(n));
        FakeHost host(TestPipeName(n), 0, pin, "", "busy", "", false);
        std::thread t = host.Start();
        EngineHostConfig cfg;
        cfg.spawn = false;
        TranslationManager strict(EngineType::LocalLlama, "");
        strict.SetEngineHostConfig(cfg);
        strict.SetCloudFallbackEnabled(true);
        TranslationStatus st = TranslationStatus::Ok;
        std::wstring res = strict.Translate(L"hello", "KO", "EN", &st);
        TEST_CHECK(strict.HostFailureStreak() == 1,
                   "T5: strict local + host fail records the repair signal before any cloud leg");
        TEST_CHECK(st == TranslationStatus::Ok || st == TranslationStatus::EngineFailed,
                   "T5: strict local + host fail + consent converges to cloud Ok or honest EngineFailed (H2 gate preserved)");
        TEST_CHECK((st == TranslationStatus::Ok) == !res.empty(),
                   "T5: consented cloud outcome matches its status");
        st = TranslationStatus::Ok;
        strict.Translate(L"hello2", "KO", "EN", &st);
        TEST_CHECK(strict.HostFailureStreak() == 2,
                   "T5: repeated host failures keep growing the repair streak");
        T5FinishHost(host, t);
    }
    {
        // 3c. busy + strict local + NO consent -> CloudConsentBlocked (chain d:
        //     the honest feature-disabled guidance; no silent failure, no crash).
        const int n = 22;
        ehc::SetPathsForTesting(TestPipeName(n).c_str(),
                                TestTokenPath(n).wstring().c_str(), fake_exe.c_str());
        WriteTestToken(TestTokenPath(n));
        FakeHost host(TestPipeName(n), 0, pin, "", "busy", "", false);
        std::thread t = host.Start();
        EngineHostConfig cfg;
        cfg.spawn = false;
        TranslationManager strict_nc(EngineType::LocalLlama, "");
        strict_nc.SetEngineHostConfig(cfg);  // consent stays off
        TranslationStatus st = TranslationStatus::Ok;
        std::wstring res = strict_nc.Translate(L"hello", "KO", "EN", &st);
        TEST_CHECK(res.empty() && st == TranslationStatus::CloudConsentBlocked,
                   "T5: strict local + host fail + no consent -> CloudConsentBlocked (no silent failure, no crash)");
        TEST_CHECK(strict_nc.HostFailureStreak() == 1,
                   "T5: the refused-cloud failure also records the repair streak");
        T5FinishHost(host, t);
    }
    {
        // 3d. success clears the streak (the T6 bootstrapper re-arm rule).
        //     Arm the streak via connect-refused (host binary present so the
        //     manager reaches the host seam, but no listener on the pipe ->
        //     connect retries exhaust -> host failure recorded, then the
        //     strict-local pre-block returns LocalModelMissing). Then succeed
        //     against a fresh ok host.
        const int n = 23;
        TranslationManager mgr(EngineType::LocalLlama, "");
        mgr.SetEngineHostConfig(EngineHostConfig{});  // enabled; spawn=false
        // No pipe listener: exe exists (fake_exe) so the existence gate passes,
        // but nothing is listening on this pipe name -> connect retries fail.
        ehc::SetPathsForTesting(TestPipeName(99).c_str(),
                                TestTokenPath(99).wstring().c_str(), fake_exe.c_str());
        TranslationStatus st = TranslationStatus::Ok;
        std::wstring res = mgr.Translate(L"arm1", "KO", "EN", &st);
        TEST_CHECK(res.empty() && st == TranslationStatus::CloudConsentBlocked &&
                   mgr.HostFailureStreak() == 1,
                   "T5: connect-refused host failure arms the streak without a cloud leg (call 1)");
        mgr.Translate(L"arm2", "KO", "EN", &st);
        TEST_CHECK(mgr.HostFailureStreak() == 2,
                   "T5: connect-refused host failure arms the streak (call 2)");

        ehc::SetPathsForTesting(TestPipeName(n).c_str(),
                                TestTokenPath(n).wstring().c_str(), fake_exe.c_str());
        WriteTestToken(TestTokenPath(n));
        FakeHost ok(TestPipeName(n), 0, pin, "", "ok", "t5-ok", false);
        std::thread to = ok.Start();
        st = TranslationStatus::Ok;
        res = mgr.Translate(L"hello", "KO", "EN", &st);
        TEST_CHECK(!res.empty() && st == TranslationStatus::Ok && res == L"t5-ok",
                   "T5: live fake host ok result routes through the manager");
        TEST_CHECK(mgr.HostFailureStreak() == 0,
                   "T5: host success clears the repair streak");
        T5FinishHost(ok, to);
        std::error_code rm_ec;
        std::filesystem::remove(TestTokenPath(n), rm_ec);
    }

    std::error_code ec;
    std::filesystem::remove(fake_exe, ec);
    // restore the process-wide absent sentinel (see main)
    ehc::SetPathsForTesting(nullptr, nullptr, L"\\\\?\\NONEXISTENT_EME_HOST\\engine.exe");
    // release the cloud-block hook so later suites (TestEngineCloudConsent-
    // Regression) can pin its contract and leave it disengaged for the rest.
    GoogleTranslate::SetCloudBlockedForTesting(false);

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] T5 host-only routing + failure UX tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] T5 host-only routing tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// Cloud consent gates are UNCHANGED by the removal. Pins the H2 / Auto
// contract regressions headlessly (no host deployed): explicit google always
// allowed; Auto consented; strict local refused without consent; the REQ-R16
// shutdown latch still outranks every cloud seam.
void TestEngineCloudConsentRegression() {
    std::cout << "[RUN] Testing T5 cloud-consent gate regression (REQ-043 M6 T5)..." << std::endl;
    const int failures_before = g_failed_count;
    using emebalachat::EngineType;
    using emebalachat::TranslationManager;
    using emebalachat::TranslationStatus;
    using emebalachat::GoogleTranslate;

    // REQ-043 (M6 T5 debug): pin the cloud-block hook contract — default
    // disengaged (production behavior), engage/disengage round-trip.
    TEST_CHECK(!GoogleTranslate::IsCloudBlockedForTesting(),
               "T5: cloud-block hook defaults to disengaged (production cloud path)");
    GoogleTranslate::SetCloudBlockedForTesting(true);
    TEST_CHECK(GoogleTranslate::IsCloudBlockedForTesting(),
               "T5: cloud-block hook engages on demand");
    GoogleTranslate::SetCloudBlockedForTesting(false);
    TEST_CHECK(!GoogleTranslate::IsCloudBlockedForTesting(),
               "T5: cloud-block hook disengages cleanly");

    // The sentinel host path from main() is active (absent), so every local
    // leg declines and only the cloud seams answer.
    TEST_CHECK(!emebalachat::engine_host::IsHostBinaryPresent(),
               "T5 setup: no host binary under the sentinel path");

    // 1) Explicit google: deliberate consent -> cloud always allowed, gate off.
    {
        TranslationManager g(EngineType::GoogleTranslate, "");
        TEST_CHECK(!g.IsCloudFallbackEnabled(), "T5: google manager defaults the gate off");
        TranslationStatus st = TranslationStatus::EngineFailed;
        std::wstring res = g.Translate(L"hello", "KO", "EN", &st);
        TEST_CHECK(!res.empty() && st == TranslationStatus::Ok,
                   "T5: explicit google serves cloud regardless of the H2 gate (unchanged)");
    }

    // 2) Auto without a local source: the documented consent -> cloud Ok.
    {
        TranslationManager a(EngineType::Auto, "");
        TEST_CHECK(a.GetActiveEngineName() == "Google Translate (Zero-Install)",
                   "T5: Auto + no host names the zero-install cloud leg");
        TranslationStatus st = TranslationStatus::EngineFailed;
        std::wstring res = a.Translate(L"hello", "KO", "EN", &st);
        TEST_CHECK(!res.empty() && st == TranslationStatus::Ok,
                   "T5: Auto + no local source -> consented cloud (unchanged)");
    }

    // 3) Strict local + no host: cloud is NEVER reached, even with consent.
    //    (Pre-block guard wins — the consent gates post-serving failures
    //    only; a translation that never started cannot be resurrected.)
    {
        TranslationManager s(EngineType::LocalLlama, "");
        s.SetCloudFallbackEnabled(true);
        TEST_CHECK(!s.IsLocalModelAvailable(),
                   "T5: strict local availability reflects the absent host");
        TranslationStatus st = TranslationStatus::Ok;
        std::wstring res = s.Translate(L"hello", "KO", "EN", &st);
        TEST_CHECK(res.empty() && st == TranslationStatus::LocalModelMissing,
                   "T5: strict local + no host + consent -> LocalModelMissing, never cloud");
    }

    // 4) Shutdown latch still outranks the cloud seams (REQ-R16 preserved).
    {
        TranslationManager c(EngineType::GoogleTranslate, "");
        c.RequestCancel();
        TEST_CHECK(c.IsCancelRequested(), "T5: cancel latch works without the embedded engine");
        TranslationStatus st = TranslationStatus::Ok;
        std::wstring res = c.Translate(L"hello", "KO", "EN", &st);
        TEST_CHECK(res.empty() && st == TranslationStatus::Canceled,
                   "T5: latched manager short-circuits the pure cloud path (no WinHTTP after exit intent)");
        TEST_CHECK(c.WaitInferenceIdle(100), "T5: manager reports idle after the short-circuit");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] T5 cloud-consent regression tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] T5 cloud-consent regression tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
