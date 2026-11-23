// ---------------------------------------------------------------------------
// R3 fix unit (261122_0002 v0.10.10 phased fixes, ROUND 1, tag V10F-R1):
// worker/hook/config remediations.
//   * F-03: ExecuteTask excludes selection_replace tasks from the FlushIme
//     prologue - FlushIme injects a synthetic VK_RIGHT that would collapse
//     the user's live selection before the flow's own capture; the selection
//     flow's own composing probe (ForegroundImeComposing) stays the authority.
//     Behavioral half is impossible (Win32 injection), so an ordering pin.
//   * F-05: the hook's selection-replace dispatch gate gains IsActive() (an
//     inactive hook swallows nothing) and the DEFAULT combo moves
//     Ctrl+Shift+T -> Ctrl+Alt+T WITHOUT migrating stored user configs.
//   * F-14: the FUP-07 CAS comment corrected (K==0 CAS 0->add SUCCEEDS across
//     a reset; the real bounded residual race documented). Pin-only.
//   * F-17 (config half): openai.http_consent_origin persists next to
//     http_consent_given (default "") behind the locked verbs
//     SetHttpConsentOrigin()/GetHttpConsentOrigin() (mutex_-guarded, the
//     SetEngineHostConfig discipline).
// Source pins read through ResolveRepoFile + raw-byte find() (CRLF-normalized,
// the p2_d/p2_e precedent; every needle is ASCII-only).
// ---------------------------------------------------------------------------

namespace r3_fix_tests {

void R3ReadSrc(const wchar_t* rel, std::string& out) {
    out.clear();
    const std::filesystem::path p = ResolveRepoFile(rel);
    if (p.empty()) return;
    std::ifstream in(p, std::ios::binary);
    if (!in) return;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string norm;
    norm.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\r' && i + 1 < out.size() && out[i + 1] == '\n') continue;
        norm.push_back(out[i]);
    }
    out.swap(norm);
}

// ---- F-03: FlushIme is Enter-only ------------------------------------------

void TestR3FlushImeSkip() {
    std::cout << "[RUN] Testing R3 F-03 FlushIme skip for selection_replace..." << std::endl;
    const int failures_before = g_failed_count;

    std::string worker_cpp;
    R3ReadSrc(L"src\\worker.cpp", worker_cpp);
    TEST_CHECK(!worker_cpp.empty(), "R3 F-03: src/worker.cpp resolves for the pins");

    const size_t kind_branch = worker_cpp.find("if (task.selection_replace) {");
    const size_t handoff =
        worker_cpp.find("ExecuteSelectionReplaceTask(task, backup, restorer.active);", kind_branch);
    const size_t flush = worker_cpp.find("\n    FlushIme();", kind_branch);
    const size_t ime_backstop = worker_cpp.find("if (ForegroundImeComposing()) {", kind_branch);
    TEST_CHECK(kind_branch != std::string::npos && handoff != std::string::npos,
               "R3 F-03: the selection-replace kind branch + handoff are locatable");
    TEST_CHECK(flush != std::string::npos && handoff != std::string::npos && handoff < flush,
               "R3 F-03: FlushIme runs AFTER the selection kind returns (Enter-only)");
    TEST_CHECK(flush != std::string::npos && ime_backstop != std::string::npos && flush < ime_backstop,
               "R3 F-03: the Enter path still flushes BEFORE the IME backstop re-probe");
    TEST_CHECK(worker_cpp.find("V10F-R1 F-03") != std::string::npos,
               "R3 F-03: the moved flush carries its V10F-R1 tag");
    // The selection flow's own composing authority stays in its body.
    const size_t body_start = worker_cpp.find(
        "void PipelineWorker::ExecuteSelectionReplaceTask(const PipelineTask& task,");
    TEST_CHECK(body_start != std::string::npos &&
                   worker_cpp.find("if (ForegroundImeComposing()) {", body_start) != std::string::npos,
               "R3 F-03: ExecuteSelectionReplaceTask keeps its own composing probe");
    // Exactly one FlushIme() call site remains in the worker.
    size_t pos = 0;
    int sites = 0;
    while ((pos = worker_cpp.find("FlushIme();", pos)) != std::string::npos) {
        ++sites;
        pos += 10;
    }
    TEST_CHECK(sites == 1, "R3 F-03: exactly one FlushIme() call site in worker.cpp");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R3 F-03 FlushIme skip pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] R3 F-03 FlushIme skip pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- F-05: IsActive() gate + Ctrl+Alt+T default (no migration) -------------

void TestR3SelectionGateAndDefault() {
    std::cout << "[RUN] Testing R3 F-05 selection gate + default combo..." << std::endl;
    const int failures_before = g_failed_count;

    std::string hook_cpp, hook_hpp, config_hpp;
    R3ReadSrc(L"src\\hook.cpp", hook_cpp);
    R3ReadSrc(L"src\\hook.hpp", hook_hpp);
    R3ReadSrc(L"src\\config.hpp", config_hpp);
    TEST_CHECK(!hook_cpp.empty() && !hook_hpp.empty() && !config_hpp.empty(),
               "R3 F-05: hook/config sources resolve for the pins");

    // The gate: IsActive() FIRST, before the callback + spec match.
    const size_t gate = hook_cpp.find(
        "if (s_instance->IsActive() && s_instance->selection_replace_cb_ &&\n"
        "            HotkeyMatches(s_instance->selection_spec_, kbd->vkCode, ctrl, shift, alt, win)) {");
    TEST_CHECK(gate != std::string::npos,
               "R3 F-05: the dispatch gate carries the IsActive() conjunct");
    TEST_CHECK(hook_cpp.find("V10F-R1 F-05") != std::string::npos,
               "R3 F-05: the gate hunk carries its V10F-R1 tag");
    // The default: config string + compiled spec agree on Ctrl+Alt+T.
    TEST_CHECK(config_hpp.find("std::string hotkey_selection_translate = \"Ctrl+Alt+T\";") !=
                   std::string::npos,
               "R3 F-05: config.hpp's default hotkey_selection_translate is Ctrl+Alt+T");
    TEST_CHECK(hook_hpp.find("kDefaultSelectionHotkey{ 'T', /*ctrl*/true, /*shift*/false, "
                             "/*alt*/true, /*win*/false, /*valid*/true };") != std::string::npos,
               "R3 F-05: the compiled default spec is Ctrl+Alt+T");

    // Behavioral: the compiled default + the config default agree, and a
    // stored Ctrl+Shift+T is NOT migrated (F-05: stored configs keep their
    // value verbatim; only fresh/absent keys see the new default).
    {
        using HK = KeyboardHook;
        static_assert(HK::kDefaultSelectionHotkey.ctrl && HK::kDefaultSelectionHotkey.alt &&
                          !HK::kDefaultSelectionHotkey.shift && HK::kDefaultSelectionHotkey.vk == 'T',
                      "R3 F-05: the compiled default is Ctrl+Alt+T");
        AppConfig fresh;
        TEST_CHECK(fresh.hotkey_selection_translate == "Ctrl+Alt+T",
                   "R3 F-05: a fresh AppConfig defaults to Ctrl+Alt+T");
        AppConfig stored;
        TEST_CHECK(stored.FromJsonString("{\"hotkey_selection_translate\": \"Ctrl+Shift+T\"}") &&
                       stored.hotkey_selection_translate == "Ctrl+Shift+T",
                   "R3 F-05: a stored Ctrl+Shift+T keeps its value (deliberately no migration)");
        AppConfig absent;
        TEST_CHECK(absent.FromJsonString("{\"hotkey_toggle\": \"F9\"}") &&
                       absent.hotkey_selection_translate == "Ctrl+Alt+T",
                   "R3 F-05: an absent key falls back to the new default");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R3 F-05 selection gate/default passed." << std::endl;
    } else {
        std::cout << "[FAIL] R3 F-05 selection gate/default: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- F-14: the corrected FUP-07 CAS comment --------------------------------

void TestR3Fup07CommentPin() {
    std::cout << "[RUN] Testing R3 F-14 FUP-07 CAS comment..." << std::endl;
    const int failures_before = g_failed_count;

    std::string hook_hpp;
    R3ReadSrc(L"src\\hook.hpp", hook_hpp);
    TEST_CHECK(!hook_hpp.empty(), "R3 F-14: src/hook.hpp resolves for the pins");
    TEST_CHECK(hook_hpp.find("V10F-R1 F-14") != std::string::npos,
               "R3 F-14: the corrected interleaving block carries its V10F-R1 tag");
    TEST_CHECK(hook_hpp.find("the REAL residual race") != std::string::npos &&
                   hook_hpp.find("CAS(expected=0, desired=0+add)") != std::string::npos,
               "R3 F-14: the comment documents the K==0 CAS-success residual race");
    // The wrong claim is gone.
    TEST_CHECK(hook_hpp.find("publishes after the zero-store fails on the value (K==0)") ==
                   std::string::npos,
               "R3 F-14: the old wrong 'fails on the value (K==0)' claim is gone");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R3 F-14 FUP-07 comment pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] R3 F-14 FUP-07 comment pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- F-17 (config half): openai.http_consent_origin -------------------------

void TestR3HttpConsentOrigin() {    std::cout << "[RUN] Testing R3 F-17 http_consent_origin persistence..." << std::endl;
    const int failures_before = g_failed_count;

    std::string config_hpp, config_cpp;
    R3ReadSrc(L"src\\config.hpp", config_hpp);
    R3ReadSrc(L"src\\config.cpp", config_cpp);
    TEST_CHECK(!config_hpp.empty() && !config_cpp.empty(),
               "R3 F-17: config sources resolve for the pins");
    TEST_CHECK(config_hpp.find("void SetHttpConsentOrigin(std::string origin);") != std::string::npos &&
                   config_hpp.find("std::string GetHttpConsentOrigin() const;") != std::string::npos,
               "R3 F-17: the locked verbs are declared");
    TEST_CHECK(config_cpp.find("void AppConfig::SetHttpConsentOrigin(std::string origin)") !=
                   std::string::npos &&
                   config_cpp.find("std::string AppConfig::GetHttpConsentOrigin() const") !=
                       std::string::npos,
               "R3 F-17: the locked verbs are defined");
    TEST_CHECK(config_cpp.find("openai_http_consent_origin = std::move(origin);") != std::string::npos &&
                   config_cpp.find("std::lock_guard<std::mutex> lock(mutex_);") != std::string::npos,
               "R3 F-17: the write verb takes mutex_ (the locked-writer discipline)");
    TEST_CHECK(config_cpp.find("ParseOpenAiHttpConsentOrigin") != std::string::npos,
               "R3 F-17: the load path parses the origin out of the openai block");
    TEST_CHECK(config_cpp.find("kMaxOpenAiHttpConsentOriginConfigBytes") != std::string::npos,
               "R3 F-17: the origin is byte-capped at the load choke point");

    // Behavioral: default "" + the key rides the openai block round trip.
    {
        AppConfig cfg;
        TEST_CHECK(cfg.GetHttpConsentOrigin().empty(),
                   "R3 F-17: the default consent origin is empty");
        const std::string json = cfg.ToJsonString();
        const size_t openai_block = json.find("\"openai\": {");
        const size_t origin_key = json.find("\"http_consent_origin\": \"\"");
        TEST_CHECK(openai_block != std::string::npos && origin_key != std::string::npos &&
                       openai_block < origin_key,
                   "R3 F-17: the writer emits http_consent_origin inside the openai block");

        // Round trip next to http_consent_given.
        cfg.openai.http_consent_given = true;  // pre-thread direct write (unit test)
        cfg.SetHttpConsentOrigin("192.168.0.10:11434");
        AppConfig reloaded;
        TEST_CHECK(reloaded.FromJsonString(cfg.ToJsonString()),
                   "R3 F-17: a config carrying the origin parses back");
        TEST_CHECK(reloaded.GetHttpConsentOrigin() == "192.168.0.10:11434",
                   "R3 F-17: the consent origin round-trips verbatim");
        TEST_CHECK(reloaded.openai.http_consent_given,
                   "R3 F-17: the neighboring consent flag survives the same round trip");

        // A pre-F-17 config.json (no such key) loads with the "" default.
        AppConfig legacy;
        TEST_CHECK(legacy.FromJsonString(
                       "{\"openai\": {\"base_url\": \"http://old:1\", \"http_consent_given\": true}}") &&
                       legacy.GetHttpConsentOrigin().empty() && legacy.openai.http_consent_given,
                   "R3 F-17: a pre-F-17 openai block keeps the empty default");

        // The verb is the writer: overwrite + read-back.
        AppConfig verb;
        verb.SetHttpConsentOrigin("a:1");
        verb.SetHttpConsentOrigin("b:2");
        TEST_CHECK(verb.GetHttpConsentOrigin() == "b:2",
                   "R3 F-17: SetHttpConsentOrigin overwrites, GetHttpConsentOrigin reads back");

        // JSON-safety: a quote-bearing value escapes on write and restores on load.
        AppConfig esc;
        esc.SetHttpConsentOrigin("ho\"st:1");
        AppConfig esc_re;
        TEST_CHECK(esc_re.FromJsonString(esc.ToJsonString()) &&
                       esc_re.GetHttpConsentOrigin() == "ho\"st:1",
                   "R3 F-17: a quote-bearing origin round-trips (JSON-escaped on write)");

        // The cap: an over-long value truncates at the bounded choke point.
        AppConfig big;
        const std::string oversize(300, 'a');
        const std::string capped(256, 'a');
        big.SetHttpConsentOrigin(oversize);
        AppConfig big_re;
        TEST_CHECK(big_re.FromJsonString(big.ToJsonString()) &&
                       big_re.GetHttpConsentOrigin() == capped,
                   "R3 F-17: an over-long origin truncates at the 256-byte cap");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R3 F-17 http_consent_origin passed." << std::endl;
    } else {
        std::cout << "[FAIL] R3 F-17 http_consent_origin: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- F-17: the request-time origin-match matrix (the R1 pure helper the ----
// R2 engine leg consumes; integrator-added with the helper itself).
void TestR3OriginMatchMatrix() {
    std::cout << "[RUN] Testing R3 F-17 OpenAiHttpConsentOriginMatches matrix..." << std::endl;
    const int failures_before = g_failed_count;

    struct Case {
        const char* base;
        const char* stored;
        bool expect;
    };
    const Case cases[] = {
        // The http consent grammar (mirrors OpenAiHttpConsentScope).
        {"http://192.168.1.10:8080", "192.168.1.10:8080", true},
        {"http://192.168.1.10", "192.168.1.10:80", true},       // default port 80
        {"http://user:pass@host:8080", "host:8080", true},      // userinfo stripped
        {"http://[::1]:8080", "[::1]:8080", true},              // bracketed IPv6
        {"http://[::1]", "[::1]:80", true},                     // IPv6 default port
        {"HTTP://Host:8080", "host:8080", true},                // case-insensitive
        {"http://host:8080/p?x#y", "host:8080", true},          // path/query/fragment trimmed
        {"http://host", "host", true},                          // stored port fill (80)
        {"http://host:8080", "HOST:8080", true},                // stored side normalized
        // Negative: host/port mismatch.
        {"http://host:8080", "other:8080", false},
        {"http://host:8080", "host:9090", false},
        // https parses through the grammar (443 default) but never matches an
        // http consent scope.
        {"https://host", "host:443", false},
        {"https://host:443", "host:443", false},
        {"https://host:8443", "host:8443", false},
        // Invalid bases / scopes.
        {"ftp://host", "host:80", false},
        {"notaurl", "notaurl:80", false},
        {"http://", ":80", false},
        {"http://:80", ":80", false},
        {"http://host:abc", "host:abc", false},
        {"http://host:8080", "", false},
        {"", "host:80", false},
    };
    for (const Case& c : cases) {
        const bool got = OpenAiHttpConsentOriginMatches(c.base, c.stored);
        TEST_CHECK(got == c.expect,
                   "R3 F-17: origin match matrix case (base, stored) matches the grammar");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R3 F-17 origin match matrix passed." << std::endl;
    } else {
        std::cout << "[FAIL] R3 F-17 origin match matrix: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void R3RegisterTests() {
    TestR3FlushImeSkip();
    TestR3SelectionGateAndDefault();
    TestR3Fup07CommentPin();
    TestR3HttpConsentOrigin();
    TestR3OriginMatchMatrix();
}

} // namespace r3_fix_tests
