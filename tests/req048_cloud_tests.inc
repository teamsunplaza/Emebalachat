// ---------------------------------------------------------------------------
// REQ-048 P3: cloud (Google) failure must surface as an EMPTY result so
// engine.cpp's cloud_call records TranslationStatus::EngineFailed — never as a
// silent identity passthrough (returning the original text) that the worker
// treats as success ("translation == source" → skip paste, no failure sound,
// no modal, and the success sentinel unlatches the modal). This was the
// user-device P3 무번역 root cause: google_translate.cpp returned the original
// text on dual-endpoint network failure, and the only caller (engine.cpp
// cloud_call) mapped any non-empty result to Ok. Staged as an .inc next to
// this runner; registered near the end of main() (after
// TestReq048OpenAiTemplateSerialization).
//
// Coverage:
//   (a) runtime: the test-only cloud-block hook (SetCloudBlockedForTesting)
//       short-circuits Translate() to empty WITHOUT touching WinHTTP — pinned
//       here as part of the failure contract (the m6 T5 suites already rely on
//       this shape; see m6_engine_host_embedded_removal_tests.inc).
//   (b) structural pin: google_translate.cpp contains NO
//       `return std::wstring(text);` identity passthrough, and the
//       dual-endpoint failure path converges on `return {};`. Source is
//       located with ResolveRepoFile and read as raw bytes; all needles are
//       pure ASCII, so the UTF-8 source encoding cannot false-match/false-miss
//       (TestReq044VerbatimSync / TestReq045EngineUnavailableModal precedent).
//   (c) engine-level signaling (engine.cpp cloud_call: empty -> EngineFailed)
//       is deliberately NOT pinned here: engine.cpp is out of scope for this
//       wave and already handles the empty outcome — documented, not tested.
//
// The SUCCESS contract is unchanged by P3: any non-empty parse (primary or
// fallback endpoint) still returns the translated text, plus the SEC-M1
// truncation notice when the query was clamped. Only the failure arm changed.
// ---------------------------------------------------------------------------

void TestReq048CloudFailureSignaling() {
    std::cout << "[RUN] Testing REQ-048 P3 cloud failure signaling..." << std::endl;
    const int failures_before = g_failed_count;
    using emebalachat::GoogleTranslate;

    // ---- (a) runtime: the blocked-cloud path returns EMPTY ----------------
    // Same engage/restore discipline as the m6 T5 suites: engage the hook,
    // exercise the seam, restore unconditionally (no early returns between
    // engage and restore, so the process-wide flag cannot leak to later
    // suites).
    TEST_CHECK(!GoogleTranslate::IsCloudBlockedForTesting(),
               "REQ-048 P3: cloud-block hook starts disengaged");
    GoogleTranslate::SetCloudBlockedForTesting(true);
    TEST_CHECK(GoogleTranslate::IsCloudBlockedForTesting(),
               "REQ-048 P3: cloud-block hook engaged");
    // REQ-043 (M6 T5 debug) contract: blocked Translate() returns empty (the
    // historical no-network outcome) without touching WinHTTP — this IS the
    // failure shape P3 requires from the real network-failure arm too.
    const std::wstring blocked = GoogleTranslate::Translate(L"hello", "English", "Korean");
    TEST_CHECK(blocked.empty(),
               "REQ-048 P3: blocked cloud returns EMPTY (failure contract: no "
               "identity passthrough, no WinHTTP touched)");
    GoogleTranslate::SetCloudBlockedForTesting(false);
    TEST_CHECK(!GoogleTranslate::IsCloudBlockedForTesting(),
               "REQ-048 P3: cloud-block hook restored (disengaged)");

    // ---- (b) structural pins: google_translate.cpp failure shape ----------
    // The real dual-endpoint failure arm cannot be exercised hermetically
    // (it requires both live endpoints to fail), so it is pinned by source
    // inspection — same technique as TestReq044VerbatimSync (ResolveRepoFile
    // + raw-byte read) and TestReq045EngineUnavailableModal (structural pins
    // for a TU whose behavior cannot be linked here). The source is UTF-8;
    // every needle below is pure ASCII, so byte-level find() is exact.
    const std::filesystem::path gt_path = ResolveRepoFile(L"src\\google_translate.cpp");
    TEST_CHECK(!gt_path.empty(),
               "REQ-048 P3: src/google_translate.cpp located via ResolveRepoFile");
    if (gt_path.empty()) {
        std::cout << "[FAIL] REQ-048 P3: google_translate.cpp not resolvable "
                     "from the test CWD; structural pins failed."
                  << std::endl;
        return;
    }
    std::ifstream gt_in(gt_path, std::ios::binary);
    TEST_CHECK(static_cast<bool>(gt_in),
               "REQ-048 P3: google_translate.cpp opened for reading");
    if (!gt_in) {
        std::cout << "[FAIL] REQ-048 P3: google_translate.cpp unreadable; structural pins failed."
                  << std::endl;
        return;
    }
    const std::string gt_src((std::istreambuf_iterator<char>(gt_in)),
                             std::istreambuf_iterator<char>());
    TEST_CHECK(!gt_src.empty(), "REQ-048 P3: google_translate.cpp non-empty after read");

    // The P3 root cause, pinned dead: the identity passthrough must not exist
    // anywhere in the TU. (ClampCloudQuery's `return { std::wstring(text),
    // false };` is a pair-return in a different function and does not match
    // this exact literal.)
    TEST_CHECK(gt_src.find("return std::wstring(text);") == std::string::npos,
               "REQ-048 P3 pin: no `return std::wstring(text);` identity "
               "passthrough remains in google_translate.cpp");

    // The dual-endpoint failure arm converges on `return {};`. Pin it
    // positionally: the empty return must appear AFTER the fallback endpoint
    // marker (translate.googleapis.com), i.e. at the tail of Translate()
    // where both endpoints have been tried — not at one of the earlier
    // legitimate `return {};` sites (empty input, test-block hook).
    const std::size_t fallback_pos = gt_src.find("translate.googleapis.com");
    TEST_CHECK(fallback_pos != std::string::npos,
               "REQ-048 P3 pin: fallback endpoint (translate.googleapis.com) present");
    if (fallback_pos != std::string::npos) {
        const std::size_t fail_ret = gt_src.find("return {};", fallback_pos);
        TEST_CHECK(fail_ret != std::string::npos,
                   "REQ-048 P3 pin: dual-endpoint failure path returns `{}` "
                   "(empty = failure signal for engine.cpp cloud_call)");
        // And the passthrough must not sit between the fallback endpoint and
        // that failure return either (a half-refactored arm).
        if (fail_ret != std::string::npos) {
            TEST_CHECK(gt_src.find("return std::wstring(text);", fallback_pos) == std::string::npos ||
                           gt_src.find("return std::wstring(text);", fallback_pos) > fail_ret,
                       "REQ-048 P3 pin: no identity passthrough after the "
                       "fallback endpoint before the empty failure return");
        }
    }

    // ---- (c) engine-level signaling: documented, not pinned ---------------
    // The consumer contract this empty result feeds is: engine.cpp cloud_call
    // maps empty -> TranslationStatus::EngineFailed (failure sound + modal,
    // success sentinel NOT cleared). That mapping lives in engine.cpp, which
    // is explicitly out of scope for the REQ-048 P3 wave (already handles
    // empty -> EngineFailed), so it is recorded here as the rationale for the
    // empty-failure contract rather than pinned as a source assertion.

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-048 P3 cloud failure signaling tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-048 P3 cloud failure signaling tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
