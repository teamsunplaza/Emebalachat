// REQ-043 (M6 T2): engine_host_registry / engine_host_manifest /
// engine_host_components unit tests (design §3.1/§3.2/§3.3, §7 scale
// ~80/~60/~50). Included directly by tests/run_tests.cpp AFTER the test
// harness macros exist. All checks are pure-parser (no machine state).

void TestEngineHostRegistry() {
    std::cout << "[RUN] Testing engine-host registry parser (REQ-043 M6 T2)..." << std::endl;
    const int failures_before = g_failed_count;
    namespace ehr = emebalachat::engine_host_registry;
    using ehr::LoadStatus;

    // ---- 1. Golden document (design §3.1 shape) ----
    {
        const char* golden =
            "{\n"
            "  \"schema_version\": 1,\n"
            "  \"models\": [{\n"
            "    \"id\": \"hy-mt2-1.8b-q8\",\n"
            "    \"family\": \"ggml-translate\",\n"
            "    \"files\": [\"Hy-MT2-1.8B-Q8_0.gguf\"],\n"
            "    \"capabilities\": [\"translate\"],\n"
            "    \"origin\": \"bundled\",\n"
            "    \"resource\": {\"vram_mb\": 2400, \"ctx\": 4096, \"max_sessions\": 1,\n"
            "                  \"residency\": \"preload\", \"eviction\": \"sticky\", \"priority\": 10},\n"
            "    \"profiles\": {\"default\": {\"temperature\": 0.0, \"top_p\": 0.6, \"top_k\": 20,\n"
            "                                \"rep_pen\": 1.05, \"prompt_template_ref\": \"hymt2-official\"}},\n"
            "    \"lang_pairs\": [\"*\"]\n"
            "  }]\n"
            "}";
        const auto r = ehr::ParseRegistryJson(golden);
        TEST_CHECK(r.status == LoadStatus::Ok, "registry golden: Ok");
        TEST_CHECK(r.registry.schema_version == 1, "registry golden: schema_version=1");
        TEST_CHECK(r.registry.models.size() == 1, "registry golden: one model");
        const ehr::ModelEntry& m = r.registry.models[0];
        TEST_CHECK(m.id == "hy-mt2-1.8b-q8", "registry golden: id");
        TEST_CHECK(m.family == "ggml-translate", "registry golden: family");
        TEST_CHECK(m.files.size() == 1 && m.files[0] == "Hy-MT2-1.8B-Q8_0.gguf",
                   "registry golden: files[]");
        TEST_CHECK(m.capabilities.size() == 1 && m.capabilities[0] == "translate",
                   "registry golden: capabilities[]");
        TEST_CHECK(m.origin == "bundled", "registry golden: origin");
        TEST_CHECK(m.resource.vram_mb == 2400, "registry golden: resource.vram_mb");
        TEST_CHECK(m.resource.ctx == 4096, "registry golden: resource.ctx");
        TEST_CHECK(m.resource.max_sessions == 1, "registry golden: resource.max_sessions");
        TEST_CHECK(m.resource.residency == "preload", "registry golden: resource.residency");
        TEST_CHECK(m.resource.eviction == "sticky", "registry golden: resource.eviction");
        // REQ-043 (M6 T2): the §3.1 golden doc writes priority:10, but the
        // §V2-4.6 scheduler scale is 0..9, so the parser's contract clamp
        // reduces it to 9. The CLAMP is the verified behavior here.
        TEST_CHECK(m.resource.priority == 9, "registry golden: resource.priority clamped 10->9");
        TEST_CHECK(m.profiles.size() == 1, "registry golden: one profile");
        TEST_CHECK(m.profiles.count("default") == 1, "registry golden: profile key 'default'");
        const ehr::SamplingProfile* prof = &m.profiles.at("default");
        TEST_CHECK(prof->temperature == 0.0, "registry golden: profile.temperature");
        TEST_CHECK(prof->top_p == 0.6, "registry golden: profile.top_p");
        TEST_CHECK(prof->top_k == 20, "registry golden: profile.top_k");
        TEST_CHECK(prof->rep_pen == 1.05, "registry golden: profile.rep_pen");
        TEST_CHECK(prof->prompt_template_ref == "hymt2-official",
                   "registry golden: profile.prompt_template_ref");
        TEST_CHECK(!prof->raw_json.empty() && prof->raw_json.front() == '{',
                   "registry golden: profile raw preserved");
        TEST_CHECK(m.lang_pairs.size() == 1 && m.lang_pairs[0] == "*",
                   "registry golden: lang_pairs");
        TEST_CHECK(r.registry.FindModel("hy-mt2-1.8b-q8") != nullptr,
                   "registry golden: FindModel hit");
        TEST_CHECK(r.registry.FindModel("nope") == nullptr,
                   "registry golden: FindModel miss");
    }

    // ---- 2. Unknown fields ignored (document + model + resource levels) ----
    {
        const char* doc =
            "{\"schema_version\":1,\"future_top\":\"x\","
            " \"models\":[{\"id\":\"a\",\"family\":\"f\",\"files\":[\"m.gguf\"],"
            "  \"unknown_field\":123,\"resource\":{\"vram_mb\":100,\"later\":true},"
            "  \"profiles\":{\"default\":{\"temperature\":0.5,\"not_known\":[1]}}}]}";
        const auto r = ehr::ParseRegistryJson(doc);
        TEST_CHECK(r.status == LoadStatus::Ok, "registry unknown fields: Ok");
        TEST_CHECK(r.registry.models.size() == 1, "registry unknown fields: model kept");
        if (!r.registry.models.empty()) {
            TEST_CHECK(r.registry.models[0].id == "a", "registry unknown fields: item id");
            TEST_CHECK(r.registry.models[0].resource.vram_mb == 100,
                       "registry unknown fields: resource parsed");
            TEST_CHECK(r.registry.models[0].profiles.at("default").temperature == 0.5,
                       "registry unknown fields: profile parsed");
        }
        // REQ-043 (M6 T2) bisect pins (root-cause isolation for the doc-2
        // rejection observed in the first gate run): each variant drops ONE
        // candidate trigger so the failing axis is named by the check itself.
        const auto r_nobool = ehr::ParseRegistryJson(
            "{\"schema_version\":1,\"models\":[{\"id\":\"a\",\"family\":\"f\",\"files\":[\"m.gguf\"],"
            "\"unknown_field\":123,\"resource\":{\"vram_mb\":100},"
            "\"profiles\":{\"default\":{\"temperature\":0.5,\"not_known\":[1]}}}]}");
        TEST_CHECK(r_nobool.registry.models.size() == 1,
                   "registry bisect: boolean unknown in resource is NOT the trigger");
        const auto r_nounk = ehr::ParseRegistryJson(
            "{\"schema_version\":1,\"models\":[{\"id\":\"a\",\"family\":\"f\",\"files\":[\"m.gguf\"],"
            "\"resource\":{\"vram_mb\":100,\"later\":true},"
            "\"profiles\":{\"default\":{\"temperature\":0.5,\"not_known\":[1]}}}]}");
        TEST_CHECK(r_nounk.registry.models.size() == 1,
                   "registry bisect: item-level numeric unknown is NOT the trigger");
        const auto r_noprof = ehr::ParseRegistryJson(
            "{\"schema_version\":1,\"models\":[{\"id\":\"a\",\"family\":\"f\",\"files\":[\"m.gguf\"],"
            "\"unknown_field\":123,\"resource\":{\"vram_mb\":100,\"later\":true}}]}");
        TEST_CHECK(r_noprof.registry.models.size() == 1,
                   "registry bisect: profiles with unknown array is NOT the trigger");
    }

    // ---- 3. Fail-closed: missing / wrong schema_version (§V2-12-2) ----
    {
        const auto r1 = ehr::ParseRegistryJson("{\"models\":[]}");
        TEST_CHECK(r1.status == LoadStatus::SchemaVersion, "registry no schema_version -> reject");
        TEST_CHECK(r1.registry.models.empty(), "registry no schema_version -> empty registry");
        const auto r2 = ehr::ParseRegistryJson("{\"schema_version\":2,\"models\":[]}");
        TEST_CHECK(r2.status == LoadStatus::SchemaVersion, "registry schema_version=2 -> reject");
        TEST_CHECK(r2.registry.models.empty(), "registry schema_version=2 -> empty registry");
        const auto r3 = ehr::ParseRegistryJson("{\"schema_version\":\"1\",\"models\":[]}");
        TEST_CHECK(r3.status == LoadStatus::SchemaVersion, "registry quoted schema_version -> reject");
    }

    // ---- 4. Fail-closed: damaged JSON / wrong shapes ----
    {
        TEST_CHECK(ehr::ParseRegistryJson("not json at all").status == LoadStatus::NotJson,
                   "registry garbage -> NotJson");
        TEST_CHECK(ehr::ParseRegistryJson("[1,2,3]").status == LoadStatus::NotJson,
                   "registry array root -> NotJson");
        TEST_CHECK(ehr::ParseRegistryJson("{\"schema_version\":1}").status == LoadStatus::Malformed,
                   "registry missing models[] -> Malformed");
        TEST_CHECK(ehr::ParseRegistryJson("{\"schema_version\":1,\"models\":{}}").status == LoadStatus::Malformed,
                   "registry models object -> Malformed");
        TEST_CHECK(ehr::ParseRegistryJson("{\"schema_version\":1,\"models\":\"[\"}").status == LoadStatus::Malformed,
                   "registry unbalanced models -> Malformed");
        // Item-level rejection: bad items skipped, good items survive.
        const char* mixed =
            "{\"schema_version\":1,\"models\":["
            "  {\"id\":\"bad1\",\"family\":\"f\",\"files\":[\"../escape.gguf\"]},"
            "  {\"id\":\"good\",\"family\":\"f\",\"files\":[\"ok.gguf\"]},"
            "  {\"id\":\"bad2\"}"
            "]}";
        const auto r = ehr::ParseRegistryJson(mixed);
        TEST_CHECK(r.status == LoadStatus::Ok, "registry mixed items: Ok status");
        TEST_CHECK(r.registry.models.size() == 1 && r.registry.models[0].id == "good",
                   "registry mixed items: only good survives");
    }

    // ---- 5. Path escapes in files[] rejected (bare-filename rule) ----
    {
        ehr::ModelEntry sink;
        TEST_CHECK(!ehr::ParseModelItem(
            "{\"id\":\"a\",\"family\":\"f\",\"files\":[\"sub\\\\dir\\\\m.gguf\"]}", sink),
            "registry files backslash escape -> item rejected");
        TEST_CHECK(!ehr::ParseModelItem(
            "{\"id\":\"a\",\"family\":\"f\",\"files\":[\"C:/abs/m.gguf\"]}", sink),
            "registry files drive path -> item rejected");
        TEST_CHECK(!ehr::ParseModelItem(
            "{\"id\":\"a\",\"family\":\"f\",\"files\":[]}", sink),
            "registry empty files[] -> item rejected");
        TEST_CHECK(!ehr::ParseModelItem(
            "{\"id\":\"a\",\"family\":\"f\",\"files\":[\"m.gguf\",\"..\"]}", sink),
            "registry files dotdot -> item rejected");
        TEST_CHECK(!ehr::ParseModelItem(
            "{\"id\":\"a\",\"family\":\"f\",\"files\":[\"m.gguf\",\"x/y.gguf\"]}", sink),
            "registry files slash escape -> item rejected");
        TEST_CHECK(!ehr::ParseModelItem(
            "{\"id\":\"\",\"family\":\"f\",\"files\":[\"m.gguf\"]}", sink),
            "registry empty id -> item rejected");
        TEST_CHECK(!ehr::ParseModelItem(
            "{\"id\":\"a\",\"files\":[\"m.gguf\"]}", sink),
            "registry missing family -> item rejected");
    }

    // ---- 6. Wrong-typed resource fields reject the item ----
    {
        ehr::ModelEntry sink;
        TEST_CHECK(!ehr::ParseModelItem(
            "{\"id\":\"a\",\"family\":\"f\",\"files\":[\"m.gguf\"],\"resource\":{\"vram_mb\":\"2400\"}}",
            sink),
            "registry resource.vram_mb quoted -> item rejected");
        TEST_CHECK(!ehr::ParseModelItem(
            "{\"id\":\"a\",\"family\":\"f\",\"files\":[\"m.gguf\"],\"resource\":{\"priority\":1.5}}",
            sink),
            "registry resource.priority fractional -> item rejected");
    }

    // ---- 7. Resource clamps (scheduler input sanity) ----
    {
        const char* doc =
            "{\"schema_version\":1,\"models\":[{\"id\":\"a\",\"family\":\"f\","
            "\"files\":[\"m.gguf\"],\"resource\":{\"vram_mb\":99999999,\"ctx\":-5,"
            "\"max_sessions\":1000,\"priority\":42}}]}";
        const auto r = ehr::ParseRegistryJson(doc);
        TEST_CHECK(r.status == LoadStatus::Ok, "registry clamp doc: Ok");
        const ehr::ResourceProfile& rp = r.registry.models[0].resource;
        TEST_CHECK(rp.vram_mb == ehr::kClampVramMbMax, "registry clamp: vram_mb upper");
        TEST_CHECK(rp.ctx == 0, "registry clamp: ctx negative -> 0");
        TEST_CHECK(rp.max_sessions == ehr::kClampMaxSessionsMax, "registry clamp: max_sessions upper");
        TEST_CHECK(rp.priority == ehr::kClampPriorityMax, "registry clamp: priority upper");
    }
    {
        const char* doc =
            "{\"schema_version\":1,\"models\":[{\"id\":\"a\",\"family\":\"f\","
            "\"files\":[\"m.gguf\"],\"resource\":{\"max_sessions\":0,\"priority\":-3}}]}";
        const auto r = ehr::ParseRegistryJson(doc);
        TEST_CHECK(r.status == LoadStatus::Ok, "registry clamp doc2: Ok");
        TEST_CHECK(r.registry.models[0].resource.max_sessions >= ehr::kClampMaxSessionsMin,
                   "registry clamp: max_sessions floor >= 1");
        TEST_CHECK(r.registry.models[0].resource.priority == ehr::kClampPriorityMin,
                   "registry clamp: priority floor 0");
    }

    // ---- 8. origin defaults / wrong-typed rejection ----
    {
        const auto r = ehr::ParseRegistryJson(
            "{\"schema_version\":1,\"models\":[{\"id\":\"u\",\"family\":\"f\",\"files\":[\"u.gguf\"]}]}");
        TEST_CHECK(r.status == LoadStatus::Ok, "registry origin-absent: Ok");
        TEST_CHECK(r.registry.models[0].origin == "user",
                   "registry origin absent -> 'user' (manifest-exempt direction)");
        const auto r2 = ehr::ParseRegistryJson(
            "{\"schema_version\":1,\"models\":[{\"id\":\"u\",\"family\":\"f\",\"files\":[\"u.gguf\"],\"origin\":5}]}");
        TEST_CHECK(r2.registry.models.empty(), "registry origin wrong type -> item rejected");
    }

    // ---- 9. Escaped / unicode filename handling in files[] ----
    {
        ehr::ModelEntry out;
        const bool ok = ehr::ParseModelItem(
            "{\"id\":\"a\",\"family\":\"f\",\"files\":[\"caf\\u00e9 model.gguf\"]}", out);
        TEST_CHECK(ok, "registry unicode filename parses");
        TEST_CHECK(out.files.size() == 1 && out.files[0] == "caf\xC3\xA9 model.gguf",
                   "registry unicode filename unescaped to UTF-8");
        TEST_CHECK(ehr::ParseRegistryJson(
            "{\"schema_version\":1,\"models\":[{\"id\":\"a\",\"family\":\"f\",\"files\":[\"caf\\u00e9.gguf\"]}]}")
            .registry.models.size() == 1, "registry unicode filename survives document parse");
    }

    // ---- 10. Default loader contract (no machine-state dependency) ----
    {
        const auto dir = ehr::DefaultModelsDir();
        if (!dir.empty()) {
            const auto r = ehr::LoadDefaultRegistry();
            // Missing (fresh machine) or Ok (installed) are both legal here;
            // anything else means the loader itself is broken.
            TEST_CHECK(r.status == LoadStatus::Ok || r.status == LoadStatus::Missing,
                       "registry default loader: Ok-or-Missing contract");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Engine-host registry parser tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Engine-host registry parser tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}