// Plan-B (REQ-B007) B-T7: Chat-side test-isolation structural pin — the
// sibling half of the B-T6 Listener pin.  Asserts Chat's OWN unit-test
// sources (the tests/ tree compiled into THIS run_tests binary) contain ZERO
// references to the Listener app image and ZERO process-creation call sites
// targeting Listener paths.
//
// Authority:
//   docs/260928_0001_session_translation-fail-fix/
//     234900_architect-planB-design.md   §7.2 (Chat side) + §11 B-T7
//     235700_architect-planB-design-v2-amendments.md  A4 (README inventory)
//     091500_debug-p3-technical-gate-planb.md  A-7 (EMEBALA_E2E_FORCE did
//       NOT exist — implemented from scratch in B-T7)
//     151143_code-light-bt6-isolation-pin.md   the Listener sibling pin this
//       mirrors (rule design proven there; adapted to Chat's .inc/run_tests
//       convention per Chat's staging contract)
//   User mandate: "chat은 chat 워크스페이스 안의 것들만 가지고 테스트" —
//   Chat tests use Chat-workspace artifacts only; never launch/foreground/
//   depend on the sibling Listener app.
//
// SCAN SCOPE (deliberate — design §7.2):
//   The tests/ tree ONLY.  tools/e2e/*.py are EXPLICITLY excluded: they are
//   documented opt-in manual harnesses (tools/e2e/README.md R1), not default
//   test targets, and they legitimately probe sibling processes via the R2
//   gate.  Files are enumerated at RUNTIME via std::filesystem over the
//   resolved repo-root tests/ directory, so new test files are covered
//   automatically — nothing is hardcoded by file name.
//
// RULES (all case-insensitive, substring semantics; expected counts all 0):
//   R1  the compact image name: app token + "listener" + ".exe" with NO
//       separator (the conventional Listener image spelling) — any occurrence.
//   R2  separated image names: app token + 1..2 separators from {'_','-',' '}
//       + "listener" + ".exe" (hyphen/space image spellings included).
//   R3  process-creation call sites (the create..process / shell..execute
//       token families) whose OWN line or the immediately following line
//       carries R1/R2 OR the bare separated form (app token + separator +
//       "listener", no ".exe").  Window-based (like B-T6 R4).
//
// DELIBERATE SCOPING (namespace-token analysis, reported NOT silent):
//   * The bare compact token WITHOUT ".exe" ("emebalalistener") is NOT
//     banned: it does not collide with a C++ namespace the way Chat's own
//     "emebalachat" token does in the Listener tree (B-T6 scoping note), and
//     the separated bare form "emebala-listener" IS a legitimate
//     client-policy client_id (engine_host_client_policy_test.cpp exercises
//     ResolvePolicy("emebala-listener", ...) and CanonicalizeClientId(
//     "emebala-listener")).  R1/R2 both REQUIRE the ".exe" image tail, and
//     R3 requires an adjacent process-creation token — so the policy
//     identifier matches NO banned form.  No whitelist/exclusion carve-out
//     is used for it.
//   * "Emebala.Engine.exe" (the shared engine host image) is NOT banned in
//     the unit tests: m6_engine_host_bootstrap_tests.inc legitimately parses
//     a manifest fixture containing that exact image name (the engine host
//     is SHARED infrastructure, not the Listener app).  The R2 pre-flight
//     gate owns engine-process isolation at the harness level.
//   * tools/e2e/ is excluded by scan scope (see SCAN SCOPE above).
//
// SELF-REFERENCE HANDLING: this .inc is TEXTUALLY INCLUDED into
// tests/run_tests.cpp, so the banned literals must not appear raw anywhere in
// this file.  Every banned token is assembled at runtime from split pieces
// (the same runtime-token-assembly idiom as B-T6), so no raw banned literal
// exists in this file and no "skip my own file" special case is needed.
//
// Included by tests/run_tests.cpp END OF FILE (after ResolveRepoFile, which
// this pin uses to locate the repo-root tests/ directory).  Uses the
// TEST_CHECK harness + std:: facilities from run_tests.cpp.

void TestChatTestIsolationPin() {
    std::cout << "[RUN] Testing Chat test-isolation pin (Plan-B B-T7, REQ-B007)..."
              << std::endl;
    const int failures_before = g_failed_count;

    // Runtime-assembled tokens (keep this file free of raw banned literals).
    const std::string kAppRoot    = "emebala";
    const std::string kListenWord = "listener";
    const std::string kExeTail    = std::string(".") + "exe";
    const char        kSeps[]     = "_- ";  // separators between app + listen

    // R1: compact image name (no separator, ".exe" tail).
    const std::string tok_compact_exe = kAppRoot + kListenWord + kExeTail;
    // R3 spawn-token families.
    const std::string tok_spawn_a = std::string("create") + "process";
    const std::string tok_spawn_b = std::string("shell") + "execute";

    // Lower-case helper (local to this pin; run_tests.cpp does not export one).
    auto lower_copy = [](std::string s) {
        for (char& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    };
    // Split a text into lines (LF boundaries; CRLF files leave a trailing \r
    // on each line, harmless for substring matching).
    auto split_lines = [](const std::string& text) {
        std::vector<std::string> lines;
        std::size_t start = 0;
        for (std::size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == '\n') {
                lines.push_back(text.substr(start, i - start));
                start = i + 1;
            }
        }
        if (!text.empty() && text.back() == '\n') {
            lines.pop_back();
        }
        return lines;
    };
    // R2: separated image names (app token + 1..2 separators + listen + ".exe").
    auto scan_separated_exe = [&](const std::string& lower_text) {
        const std::size_t root_len = kAppRoot.size();
        const std::size_t word_len = kListenWord.size();
        const std::size_t exe_len  = kExeTail.size();
        for (std::size_t p = lower_text.find(kAppRoot, 0);
             p != std::string::npos;
             p = lower_text.find(kAppRoot, p + 1)) {
            std::size_t q = p + root_len;
            std::size_t sep = 0;
            while (sep < 2 && q < lower_text.size() &&
                   std::strchr(kSeps, lower_text[q]) != nullptr) {
                ++q;
                ++sep;
            }
            if (sep < 1) continue;
            if (lower_text.compare(q, word_len, kListenWord) != 0) continue;
            q += word_len;
            if (lower_text.compare(q, exe_len, kExeTail) != 0) continue;
            return true;
        }
        return false;
    };
    // R3 helper: does a single line carry a Listener app token (R1, R2, or
    // the bare separated form)?
    auto line_has_listener_token = [&](const std::string& lower_line) {
        if (lower_line.find(tok_compact_exe) != std::string::npos) return true;
        // Separated form, with OR without the ".exe" tail (R3 window).
        const std::size_t root_len = kAppRoot.size();
        const std::size_t word_len = kListenWord.size();
        for (std::size_t p = lower_line.find(kAppRoot, 0);
             p != std::string::npos;
             p = lower_line.find(kAppRoot, p + 1)) {
            std::size_t q = p + root_len;
            std::size_t sep = 0;
            while (sep < 2 && q < lower_line.size() &&
                   std::strchr(kSeps, lower_line[q]) != nullptr) {
                ++q;
                ++sep;
            }
            if (sep < 1) continue;
            if (lower_line.compare(q, word_len, kListenWord) != 0) continue;
            return true;
        }
        return false;
    };
    // R3: a spawn-token line whose own line or the NEXT line carries a
    // Listener app token.
    auto scan_spawn_window = [&](const std::vector<std::string>& lower_lines) {
        for (std::size_t i = 0; i < lower_lines.size(); ++i) {
            const std::string& line = lower_lines[i];
            const bool is_spawn =
                line.find(tok_spawn_a) != std::string::npos ||
                line.find(tok_spawn_b) != std::string::npos;
            if (!is_spawn) continue;
            if (line_has_listener_token(line)) return true;
            if (i + 1 < lower_lines.size() &&
                line_has_listener_token(lower_lines[i + 1])) {
                return true;
            }
        }
        return false;
    };

    // Locate the repo-root tests/ directory through ResolveRepoFile (the
    // run_tests.cpp L1 portability helper).  We probe a file we KNOW exists
    // (run_tests.cpp itself) to recover the tests/ dir, then enumerate every
    // regular file under it at runtime.
    const std::filesystem::path run_tests_cpp =
        ResolveRepoFile(L"tests\\run_tests.cpp");
    TEST_CHECK(!run_tests_cpp.empty(),
               "B-T7: tests/run_tests.cpp resolves via ResolveRepoFile");
    if (run_tests_cpp.empty()) {
        if (g_failed_count == failures_before) {
            std::cout << "[PASS] Chat test-isolation pin completed (unresolvable; "
                         "see failure above)."
                      << std::endl;
        }
        return;
    }
    const std::filesystem::path tests_dir = run_tests_cpp.parent_path();

    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(
             tests_dir, std::filesystem::directory_options::skip_permission_denied,
             ec),
         end;
         it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code fec;
        if (it->is_regular_file(fec)) {
            files.push_back(it->path());
        }
    }
    TEST_CHECK(!files.empty(), "B-T7: enumerated at least one file under tests/");

    std::size_t r1_hits = 0, r2_hits = 0, r3_hits = 0, unreadable = 0;
    for (const auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        if (!in) {
            ++unreadable;
            continue;
        }
        const std::string text(
            (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string lower = lower_copy(text);
        if (lower.find(tok_compact_exe) != std::string::npos) {
            ++r1_hits;
            std::cout << "  [B-T7 R1 hit] " << f << std::endl;
        }
        if (scan_separated_exe(lower)) {
            ++r2_hits;
            std::cout << "  [B-T7 R2 hit] " << f << std::endl;
        }
        if (scan_spawn_window(split_lines(lower))) {
            ++r3_hits;
            std::cout << "  [B-T7 R3 hit] " << f << std::endl;
        }
    }

    std::cout << "  [B-T7] scanned " << files.size() << " file(s) under "
              << tests_dir << " (unreadable: " << unreadable << "); R1=" << r1_hits
              << " R2=" << r2_hits << " R3=" << r3_hits << std::endl;

    TEST_CHECK(r1_hits == 0,
               "B-T7: zero compact Listener-image references in Chat unit tests");
    TEST_CHECK(r2_hits == 0,
               "B-T7: zero separated Listener-image references in Chat unit tests");
    TEST_CHECK(r3_hits == 0,
               "B-T7: zero Listener-targeting process-creation windows in Chat "
               "unit tests");

    // In-process detector self-tests (non-tautology): prove each rule FIRES on
    // a known-positive sample and stays silent on the documented near-misses.
    // These use locally-assembled literals (not the repo), so they are pure
    // logic checks independent of the tree state.
    {
        const std::string sample_compact = std::string("start ") + "emebala" +
                                           "Listener" + "." + "exe" + " now";
        const std::string sample_separated = std::string("run ") + "emebala" +
                                             "-" + "Listener" + "." + "exe";
        const std::string sample_policy_id = std::string("\"emebala") + "-" +
                                             "listener\"";
        const std::string sample_engine_img = std::string("Emebala") + "." +
                                              "Engine" + "." + "exe";
        const std::string sample_spawn_next = std::string("create") + "process" +
            "W(exe_path,\n    // arg names " + "emebala" + "_" + "Listener";

        TEST_CHECK(lower_copy(sample_compact).find(tok_compact_exe) !=
                       std::string::npos,
                   "B-T7 self-test: R1 fires on the compact image sample");
        TEST_CHECK(scan_separated_exe(lower_copy(sample_separated)),
                   "B-T7 self-test: R2 fires on the separated image sample");
        TEST_CHECK(!scan_separated_exe(lower_copy(sample_policy_id)),
                   "B-T7 self-test: the bare hyphenated policy id does NOT match "
                   "R2 (no .exe tail)");
        TEST_CHECK(lower_copy(sample_engine_img).find(tok_compact_exe) ==
                       std::string::npos,
                   "B-T7 self-test: the shared engine image does NOT match R1");
        const auto spawn_lines =
            split_lines(lower_copy(sample_spawn_next));
        TEST_CHECK(scan_spawn_window(spawn_lines),
                   "B-T7 self-test: R3 fires on a next-line separated Listener "
                   "token inside a spawn window");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Chat test-isolation pin completed." << std::endl;
    } else {
        std::cout << "[FAIL] Chat test-isolation pin: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}
