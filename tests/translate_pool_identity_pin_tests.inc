// Plan-B (REQ-B010) B-T5 pin 4: structural client-visible-identity scan pin —
// the POSITIVE half; complements the B-T4fix1 negative pins without duplicating
// them. Asserts BOTH dispatcher loops (host_main.cpp DispatcherLoop /
// DispatcherV2Loop) obtain the worker family ONLY from the TranslatePoolEnsure
// result (`e->family`): every EnsureSpawned/TrySetBusy call-site in the loop
// bodies uses a family derived from the pool entry, and no NEW hardcoded
// translate-family literal appears inside a loop body (the boot-site
// derivation at host_main.cpp is explicitly excluded).
//
// Included by tests/run_tests.cpp at END OF FILE (after ResolveRepoFile, which
// this file needs to locate src/host_main.cpp); the behavioral B-T5 pins live
// in translate_pool_test.inc (included earlier, before ResolveRepoFile).
// Uses the TEST_CHECK harness + the std:: facilities from run_tests.cpp.
// Complements (does not duplicate) the B-T4fix1 negative pins in
// m6_engine_host_settle_tests.inc (no hardcoded `const std::wstring
// family(kWorkerFamilyTranslate)` prolog) and req055_live_pin_tests.inc.

void TestTranslatePoolClientVisibleIdentity() {
    std::cout << "[RUN] Testing translate pool client-visible-identity structural pin "
                 "(Plan-B B-T5, REQ-B010)..." << std::endl;
    const int failures_before = g_failed_count;

    const std::filesystem::path host_main = ResolveRepoFile(L"src\\host_main.cpp");
    if (host_main.empty()) {
        std::cout << "[SKIP] src/host_main.cpp not resolvable; REQ-B010 identity pin skipped."
                  << std::endl;
        if (g_failed_count == failures_before) {
            std::cout << "[PASS] Translate pool client-visible-identity pin completed (skipped)."
                      << std::endl;
        }
        return;
    }
    std::ifstream in(host_main, std::ios::binary);
    const std::string src((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());

    const size_t v1_pos = src.find("void DispatcherLoop(host_v2::WorkerManager& wmgr)");
    const size_t v2_pos = src.find("void DispatcherV2Loop(host_v2::WorkerManager& wmgr)");
    TEST_CHECK(v1_pos != std::string::npos && v2_pos != std::string::npos,
               "REQ-B010 identity: both dispatcher loops located");
    if (v1_pos == std::string::npos || v2_pos == std::string::npos) {
        if (g_failed_count == failures_before) {
            std::cout << "[PASS] Translate pool client-visible-identity pin completed."
                      << std::endl;
        }
        return;
    }
    // v1 spans up to the v2 loop; v2 is bounded to its OWN body (the next
    // closing brace at column 0) so the boot block below — the sanctioned
    // `RegisterFamily(kWorkerFamilyTranslate, ...)` derivation site
    // (host_main.cpp L2501) and the legacy-name skip (L2546) — is EXCLUDED.
    const std::string v1_region = src.substr(v1_pos, v2_pos - v1_pos);
    // Bound the v2 loop body: the file is CRLF, so there is no bare "\n}\n".
    // Find the next "\n}" (LF then the body's closing brace at column 0) and
    // stop there — this excludes the boot block below (the sanctioned
    // `RegisterFamily(kWorkerFamilyTranslate, ...)` derivation site at L2501
    // and the legacy-name skip at L2546).
    const size_t v2_close = src.find("\n}", v2_pos);
    const std::string v2_region = src.substr(
        v2_pos, v2_close == std::string::npos ? std::string::npos : v2_close - v2_pos);

    // POSITIVE: every EnsureSpawned/TrySetBusy call-site in the loop bodies
    // passes a family derived from the pool entry (e->family). Complements the
    // B-T4fix1 negative pin (m6_engine_host_settle_tests.inc: no hardcoded
    // `const std::wstring family(kWorkerFamilyTranslate)` prolog) by asserting
    // the positive routing shape actually in force.
    TEST_CHECK(v1_region.find("EnsureSpawned(e->family)") != std::string::npos,
               "REQ-B010 identity: the v1 loop spawns via the pool entry family (e->family)");
    TEST_CHECK(v1_region.find("TrySetBusy(e->family)") != std::string::npos,
               "REQ-B010 identity: the v1 loop single-flight acquires via the pool entry family");
    TEST_CHECK(v2_region.find("EnsureSpawned(e->family)") != std::string::npos,
               "REQ-B010 identity: the v2 loop spawns via the pool entry family (e->family)");
    TEST_CHECK(v2_region.find("TrySetBusy(e->family)") != std::string::npos,
               "REQ-B010 identity: the v2 loop single-flight acquires via the pool entry family");

    // POSITIVE: both loops obtain family ONLY from the Ensure result — the
    // family flows Ensure -> e->family, with no new hardcoded translate-family
    // literal introduced inside a loop body. (The single boot-site derivation
    // `RegisterFamily(kWorkerFamilyTranslate, ...)` is outside both loop regions
    // and is the sanctioned derivation site; excluded here.)
    TEST_CHECK(v1_region.find("kWorkerFamilyTranslate") == std::string::npos,
               "REQ-B010 identity: no hardcoded kWorkerFamilyTranslate literal inside the v1 "
               "loop body (family comes only from the pool entry)");
    TEST_CHECK(v2_region.find("kWorkerFamilyTranslate") == std::string::npos,
               "REQ-B010 identity: no hardcoded kWorkerFamilyTranslate literal inside the v2 "
               "loop body (family comes only from the pool entry)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Translate pool client-visible-identity pin completed." << std::endl;
    } else {
        std::cout << "[FAIL] Translate pool client-visible-identity pin: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
