// ---------------------------------------------------------------------------
// REQ-047 D2 (design §B.3, Rev2 §6, diagnosis 235010): bundled-origin
// reuse-path rejection — the user-.gguf registration picker must REFUSE a
// re-pick of a registry entry whose origin is "bundled" (e.g. the built-in
// Hy-MT2-1.8B-Q8_0.gguf) instead of silently re-pointing config at the
// bundled id (the P2 root cause: the newly registered model never took
// effect).
//
// main.cpp is the GUI entrypoint TU and is deliberately NOT linked into
// run_tests.exe, so the reuse-loop edit is pinned by source inspection
// (TestReq045EngineUnavailableModal / TestReq046TrayMenuStructure precedent),
// while the decision predicate itself (bundled vs user origin) is re-derived
// here against the real parser output.
// ---------------------------------------------------------------------------

namespace {

// The exact predicate the reuse loop applies to a registry match (design
// §B.3 pseudocode): a matched entry with origin == "bundled" takes the
// rejection branch; any other origin (parser default "user") keeps reuse.
bool Req047IsBundledReject(const engine_host_registry::ModelEntry& m) {
    return m.origin == "bundled";
}

} // namespace

void TestReq047BundledReuseRejection() {
    std::cout << "[RUN] Testing REQ-047 D2 bundled-origin reuse rejection..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) parser-level: origin defaults + explicit values --------------
    // The bundled entry as installed by setup.iss: id "hymt2", the pinned
    // filename, origin "bundled".
    const auto parsed = engine_host_registry::ParseRegistryJson(
        "{\"schema_version\":1,\"models\":["
        "{\"id\":\"hymt2\",\"family\":\"ggml-translate\","
        "\"files\":[\"Hy-MT2-1.8B-Q8_0.gguf\"],\"origin\":\"bundled\"},"
        "{\"id\":\"user-llama3\",\"family\":\"ggml-translate\","
        "\"files\":[\"llama3.gguf\"],\"origin\":\"user\"}]}");
    TEST_CHECK(parsed.status == engine_host_registry::LoadStatus::Ok,
               "REQ-047 D2: fixture registry parses Ok");
    const engine_host_registry::Registry& reg = parsed.registry;

    // (a-1) bundled match -> reject path.
    const engine_host_registry::ModelEntry* bundled = nullptr;
    for (const auto& m : reg.models) {
        if (!m.files.empty() && m.files[0] == "Hy-MT2-1.8B-Q8_0.gguf") bundled = &m;
    }
    TEST_CHECK(bundled != nullptr, "REQ-047 D2: bundled fixture entry found by filename");
    if (bundled) {
        TEST_CHECK(bundled->origin == "bundled",
                   "REQ-047 D2: parser preserves origin \"bundled\"");
        TEST_CHECK(Req047IsBundledReject(*bundled),
                   "REQ-047 D2 (a): bundled registry match selects the REJECT branch");
    }

    // (a-2) user match -> reuse path kept (regression guard on the sibling).
    const engine_host_registry::ModelEntry* user = reg.FindModel("user-llama3");
    TEST_CHECK(user != nullptr && user->origin == "user",
               "REQ-047 D2: parser preserves origin \"user\"");
    if (user) {
        TEST_CHECK(!Req047IsBundledReject(*user),
                   "REQ-047 D2 (b): user-origin registry match keeps the REUSE branch");
    }

    // (a-3) parser default: an entry with NO origin key parses as "user"
    // (engine_host_registry.hpp contract), so a legacy hand-edited registry
    // never trips the bundled rejection.
    const auto legacy = engine_host_registry::ParseRegistryJson(
        "{\"schema_version\":1,\"models\":["
        "{\"id\":\"user-legacy\",\"family\":\"ggml-translate\","
        "\"files\":[\"legacy.gguf\"]}]}");
    TEST_CHECK(legacy.status == engine_host_registry::LoadStatus::Ok,
               "REQ-047 D2: origin-less fixture registry parses Ok");
    if (legacy.status == engine_host_registry::LoadStatus::Ok) {
        const auto* e = legacy.registry.FindModel("user-legacy");
        TEST_CHECK(e != nullptr && e->origin == "user",
                   "REQ-047 D2: absent origin key defaults to \"user\" (parser contract)");
        if (e) {
            TEST_CHECK(!Req047IsBundledReject(*e),
                       "REQ-047 D2: origin-less entry keeps the REUSE branch");
        }
    }

    // ---- (b) main.cpp structural pins (GUI TU not linked here) ------------
    auto read_src = [](const char* name, std::string& out) {
        const char* candidates[] = {name, (std::string("../") + name).c_str(),
                                    (std::string("../../") + name).c_str()};
        for (const char* cand : candidates) {
            std::ifstream in(cand, std::ios::binary);
            if (in) {
                out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                return;
            }
        }
    };
    std::string main_src;
    read_src("src/main.cpp", main_src);
    if (main_src.empty()) {
        std::cout << "[SKIP] src/main.cpp not resolvable from the test CWD; REQ-047 D2 structural pins skipped."
                  << std::endl;
        return;
    }

    // The reuse loop gates on the bundled origin BEFORE any config write.
    TEST_CHECK(main_src.find("m.origin == \"bundled\"") != std::string::npos,
               "REQ-047 D2 structural pin: reuse loop compares the matched entry's origin to \"bundled\"");
    // The rejection branch shows the new notice body and reuses the existing
    // registration title (no new title key, per design §B.3).
    TEST_CHECK(main_src.find("StringId::UserGgufBundledDuplicateBody") != std::string::npos,
               "REQ-047 D2 structural pin: bundled branch shows UserGgufBundledDuplicateBody");
    TEST_CHECK(main_src.find("MAIN/RegisterUserGguf/006b: bundled entry re-pick refused") != std::string::npos,
               "REQ-047 D2 structural pin: bundled branch logs the refusal (DIAG 006b)");
    // The rejection branch must NOT touch config/engine: the only Set* calls
    // inside the loop stay on the reuse (post-gate) path. Pin the ordering:
    // the bundled-gate string appears BEFORE the first SetUserModelId in the
    // reuse loop body.
    {
        const size_t gate_pos = main_src.find("m.origin == \"bundled\"");
        const size_t reuse_pos = main_src.find("config.SetUserModelId(m.id)");
        TEST_CHECK(gate_pos != std::string::npos && reuse_pos != std::string::npos &&
                       gate_pos < reuse_pos,
                   "REQ-047 D2 structural pin: bundled gate precedes the config Set* reuse writes");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-047 D2 bundled-origin reuse rejection passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-047 D2 bundled-origin reuse rejection: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestReq047BundledDuplicateI18n() {
    std::cout << "[RUN] Testing REQ-047 D2 UserGgufBundledDuplicateBody i18n coverage..." << std::endl;
    const int failures_before = g_failed_count;

    // (c) The new key resolves NON-EMPTY in all 37 selectable locales (the
    // EnumCount completeness loop in TestR6P5P6I18n covers every key; this
    // pins the D2 key explicitly with its own failure message, per the
    // TestR6P5P6I18n REQ-206/208 pattern). Iterates every UiLocale value in
    // [Korean, EnumCount) — the 37 authored tables (Auto is the detection
    // sentinel, not a table).
    const UiLocale initial = I18n::GetCurrentLocale();
    int blank = 0;
    for (int li = static_cast<int>(UiLocale::Korean);
         li <= static_cast<int>(UiLocale::Burmese); ++li) {
        I18n::SetLocale(static_cast<UiLocale>(li));
        if (I18n::Get(StringId::UserGgufBundledDuplicateBody).empty()) ++blank;
    }
    I18n::SetLocale(initial); // suite-state hygiene
    TEST_CHECK(blank == 0,
               "REQ-047 D2 (c): UserGgufBundledDuplicateBody is non-empty in all 37 locales");

    // The English source text follows the Rev2 §6 softened wording: it names
    // the built-in local translation engine WITHOUT quoting the 'Local LLM'
    // menu label.
    I18n::SetLocale(UiLocale::English);
    const std::wstring en = I18n::Get(StringId::UserGgufBundledDuplicateBody);
    I18n::SetLocale(initial);
    TEST_CHECK(en.find(L"already built into") != std::wstring::npos,
               "REQ-047 D2: English body states the model is already built in");
    TEST_CHECK(en.find(L"No registration is needed") != std::wstring::npos,
               "REQ-047 D2: English body states no registration is needed");
    TEST_CHECK(en.find(L"built-in local translation engine") != std::wstring::npos,
               "REQ-047 D2: English body points at the built-in local translation engine");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-047 D2 i18n coverage passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-047 D2 i18n coverage: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
