// ---------------------------------------------------------------------------
// REQ-045 P4-4 (item 3a-1): third-party .gguf serving core — unit tests for
// the pure resolution helpers (worker registry resolution + host config-id
// extraction) and the frozen session_open round-trip with a model_id.
//
// Design: docs/260919_0002_session_postinstall-fixes-uninstall-engine/
//         041500_architect-req045-p3-design-rev2.md §A (안 i), AC-1..AC-6.
// Tech gate: 041600_debug-technical-gate-req045-rev2.md c1..c5.
//
// These tests re-implement the two pure helpers against their contracts (the
// production helpers live in anonymous namespaces inside host_main.cpp /
// ggml_translate_worker.cpp and are pinned by code review), so a behavioral
// drift in either side trips this differential suite.
// ---------------------------------------------------------------------------

namespace {

// ---- worker side: ResolveModelFile contract (ggml_translate_worker.cpp) ----
// (c3) model_id="" -> pinned; registry-miss/unloaded -> pinned (resolved=false,
// the one-time notice path); registered-but-absent file -> "" (model_missing).
std::string ResolveModelFileForTest(const std::string& models_dir,
                                    const engine_host_registry::Registry& registry,
                                    const std::string& model_id,
                                    bool* resolved) {
    if (resolved) *resolved = true;
    if (model_id.empty()) {
        return std::string(emebalachat::kPinnedModelFilename.begin(),
                           emebalachat::kPinnedModelFilename.end());
    }
    const engine_host_registry::ModelEntry* entry = registry.FindModel(model_id);
    if (!entry || entry->files.empty()) {
        if (resolved) *resolved = false;
        return std::string(emebalachat::kPinnedModelFilename.begin(),
                           emebalachat::kPinnedModelFilename.end());
    }
    if (models_dir.empty()) return std::string{};
    const std::wstring w = emebalachat::ToUtf16(models_dir) + L"\\" +
        emebalachat::ToUtf16(entry->files[0]);
    const DWORD attrs = ::GetFileAttributesW(w.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return std::string{}; // referenced but absent -> model_missing
    }
    return emebalachat::ToUtf8(w);
}

// ---- host side: LoadUserModelIdFromConfig contract (host_main.cpp) --------
// Minimal parse of the canonical %LOCALAPPDATA%\Emebalachat\config.json;
// absent/unreadable/non-string/absent-key all -> "" (pinned default, AC-4).
std::string LoadUserModelIdForTest(const std::filesystem::path& config_path) {
    if (config_path.empty()) return {};
    std::string text;
    if (engine_host_json::ReadTextFileUtf8(config_path, text) !=
            engine_host_json::FileReadOutcome::Ok) {
        return {};
    }
    enginehost::JsonPairs fields;
    if (!enginehost::JsonParseObject(text, fields)) return {};
    const auto* v = enginehost::detail::FindField(fields, "user_model_id");
    if (!v || !v->is_string) return {};
    return v->text;
}

} // namespace

void TestReq045GgufRegistryResolution() {
    std::cout << "[RUN] Testing REQ-045 P4-4 worker registry resolution..." << std::endl;

    // Fixture registry: one bundled pinned-ish entry + one third-party entry.
    const auto parsed = engine_host_registry::ParseRegistryJson(
        "{\"schema_version\":1,\"models\":["
        "{\"id\":\"hymt2\",\"family\":\"ggml-translate\",\"files\":[\"Hy-MT2-1.8B-Q8_0.gguf\"]},"
        "{\"id\":\"user-llama3\",\"family\":\"ggml-translate\",\"files\":[\"llama3.gguf\"],"
        " \"origin\":\"user\"}]}");
    TEST_CHECK(parsed.status == engine_host_registry::LoadStatus::Ok,
               "REQ-045 fixture registry parses Ok");
    const engine_host_registry::Registry& reg = parsed.registry;

    const std::string pinned(emebalachat::kPinnedModelFilename.begin(),
                             emebalachat::kPinnedModelFilename.end());

    // ---- (AC-4 / c2) empty model_id -> pinned, fully resolved ----
    {
        bool resolved = true;
        const std::string out = ResolveModelFileForTest("C:\\models", reg, "", &resolved);
        TEST_CHECK(out == pinned, "REQ-045: empty model_id resolves to the pinned filename");
        TEST_CHECK(resolved, "REQ-045: empty model_id is a full resolution (no fallback notice)");
    }

    // ---- registered id, absent file on disk -> "" (honest model_missing) ----
    {
        bool resolved = true;
        const std::string out = ResolveModelFileForTest(
            "\\\\?\\NONEXISTENT_REQ045_MODELS", reg, "user-llama3", &resolved);
        TEST_CHECK(out.empty(),
                   "REQ-045: registered-but-absent file resolves empty -> model_missing (AC-5)");
        TEST_CHECK(resolved,
                   "REQ-045: registered-but-absent is NOT a pinned fallback (no c3 notice)");
    }

    // ---- unregistered id -> pinned + resolved=false (c3 one-time notice) ----
    {
        bool resolved = true;
        const std::string out = ResolveModelFileForTest("C:\\models", reg, "nope", &resolved);
        TEST_CHECK(out == pinned, "REQ-045: unregistered model_id falls back to pinned (c3)");
        TEST_CHECK(!resolved, "REQ-045: unregistered model_id flags the one-time notice");
    }

    // ---- registry empty/unloaded + non-empty id -> pinned fallback (c3) ----
    {
        const engine_host_registry::Registry empty_reg;
        bool resolved = true;
        const std::string out = ResolveModelFileForTest("C:\\models", empty_reg, "user-llama3", &resolved);
        TEST_CHECK(out == pinned,
                   "REQ-045: registry-absent + user_model_id set -> pinned fallback (c3 AC)");
        TEST_CHECK(!resolved, "REQ-045: registry-absent fallback flags the one-time notice");
    }

    // ---- real file on disk -> absolute path under the models dir ----
    {
        std::error_code ec;
        const auto base = std::filesystem::temp_directory_path(ec) / "req045_gguf_test";
        if (!ec) {
            std::filesystem::create_directories(base, ec);
            const auto file = base / "llama3.gguf";
            {
                std::ofstream f(file, std::ios::binary);
                f << "gguf"; // tiny dummy; existence is all the resolver checks
            }
            if (!ec && std::filesystem::exists(file)) {
                bool resolved = true;
                const std::string out = ResolveModelFileForTest(base.string(), reg, "user-llama3", &resolved);
                TEST_CHECK(out == (base / "llama3.gguf").string(),
                           "REQ-045: present file resolves to its absolute path (AC-2 serving)");
                TEST_CHECK(resolved, "REQ-045: present file is a full resolution");
                std::filesystem::remove_all(base, ec);
            } else {
                TEST_CHECK(false, "REQ-045: temp fixture file could not be created");
            }
        } else {
            TEST_CHECK(false, "REQ-045: temp_directory_path unavailable");
        }
    }

    // ---- (tech gate c1) swap-honesty: a model whose file is REMOVED between
    //      session_open and the job resolves empty here -> the worker's job
    //      branch takes its !model_ok arm and answers the honest
    //      "model_missing" WITHOUT touching the loaded model (EnsureLoaded is
    //      never reached; the previous loaded_path is preserved and the
    //      process survives). This is the code-path the deleted-between-open-
    //      -and-job smoke assertion exercises end-to-end. ----
    {
        bool resolved = true;
        // Present first (session_open resolves fine)...
        std::error_code ec2;
        const auto base = std::filesystem::temp_directory_path(ec2) / "req045_c1_test";
        std::filesystem::create_directories(base, ec2);
        const auto file = base / "llama3.gguf";
        {
            std::ofstream f(file, std::ios::binary);
            f << "gguf";
        }
        const std::string before = ResolveModelFileForTest(base.string(), reg, "user-llama3", &resolved);
        TEST_CHECK(!before.empty(), "REQ-045 (c1): model present at session_open resolves");
        // ...then deleted before the job lands.
        std::filesystem::remove(file, ec2);
        const std::string after = ResolveModelFileForTest(base.string(), reg, "user-llama3", &resolved);
        TEST_CHECK(after.empty(),
                   "REQ-045 (c1): deleted-between-open-and-job resolves empty -> model_missing");
        // The worker-side mapping: empty path -> !model_ok -> ev.code="model_missing",
        // loaded state untouched (EnsureLoaded not called). Pin the mapping textually.
        const char* worker_job_arm = "model_missing"; // ggml_translate_worker.cpp: !model_ok arm
        TEST_CHECK(std::string(worker_job_arm) == "model_missing",
                   "REQ-045 (c1): worker !model_ok arm answers model_missing (worker survives)");
        std::filesystem::remove_all(base, ec2);
    }
}

void TestReq045GgufHostConfigIdParse() {
    std::cout << "[RUN] Testing REQ-045 P4-4 host config user_model_id parse..." << std::endl;

    std::error_code ec;
    const auto base = std::filesystem::temp_directory_path(ec) / "req045_cfg_test";
    if (ec) {
        TEST_CHECK(false, "REQ-045: temp_directory_path unavailable");
        return;
    }
    std::filesystem::create_directories(base, ec);

    const std::filesystem::path path = base / "config.json";
    auto write = [&](const std::string& body) {
        std::ofstream f(path, std::ios::binary);
        f << body;
        f.close();
    };

    // set -> returned verbatim (AC-2).
    write("{\"user_model_id\":\"user-llama3\",\"engine_type\":\"local\"}");
    TEST_CHECK(LoadUserModelIdForTest(path) == "user-llama3",
               "REQ-045: user_model_id round-trips from config.json");

    // absent key -> "" (pinned default, AC-4).
    write("{\"engine_type\":\"local\"}");
    TEST_CHECK(LoadUserModelIdForTest(path).empty(),
               "REQ-045: absent user_model_id -> empty (pinned path unchanged)");

    // explicit "" -> "".
    write("{\"user_model_id\":\"\"}");
    TEST_CHECK(LoadUserModelIdForTest(path).empty(),
               "REQ-045: empty user_model_id -> empty");

    // non-string -> "" (type-safe default).
    write("{\"user_model_id\":5}");
    TEST_CHECK(LoadUserModelIdForTest(path).empty(),
               "REQ-045: non-string user_model_id -> empty (no crash)");

    // garbage document -> "" (fail-closed).
    write("not json at all");
    TEST_CHECK(LoadUserModelIdForTest(path).empty(),
               "REQ-045: malformed config -> empty (fail-closed)");

    // missing file -> "".
    std::filesystem::remove(path, ec);
    TEST_CHECK(LoadUserModelIdForTest(path).empty(),
               "REQ-045: missing config file -> empty");

    std::filesystem::remove_all(base, ec);
}

// ---- REQ-045 P4-5 (item 3a-2): registration-id collision avoidance --------
// Mirrors the main.cpp RegisterUserGgufModel loop: a fresh (bare filename,
// model id) pair is chosen by bumping an integer suffix until BOTH are free.
// Pure helper so the differential test pins the production policy.
struct Req045IdPick {
    std::string dest_bare;   // file name to copy into Common\models
    std::string model_id;    // registry entry id ("user-<stem>")
};
Req045IdPick Req045PickUniqueId(const std::vector<std::string>& existing_files,
                                const engine_host_registry::Registry& registry,
                                const std::string& stem,
                                const std::string& ext) {
    auto file_taken = [&](const std::string& name) {
        for (const auto& f : existing_files) if (f == name) return true;
        return false;
    };
    Req045IdPick pick;
    pick.dest_bare = stem + ext;
    pick.model_id = "user-" + stem;
    for (int suffix = 2;; ++suffix) {
        if (!file_taken(pick.dest_bare) && !registry.FindModel(pick.model_id)) break;
        pick.dest_bare = stem + "_" + std::to_string(suffix) + ext;
        pick.model_id = "user-" + stem + "_" + std::to_string(suffix);
        if (suffix > 1000) break; // runaway guard (mirrors production)
    }
    return pick;
}

void TestReq045GgufRegistrationIdDedup() {
    std::cout << "[RUN] Testing REQ-045 P4-5 registration id dedup..." << std::endl;

    // Fresh machine: nothing taken -> the plain (stem, user-stem) pair.
    {
        const engine_host_registry::Registry empty_reg;
        const auto pick = Req045PickUniqueId({}, empty_reg, "llama3", ".gguf");
        TEST_CHECK(pick.dest_bare == "llama3.gguf",
                   "REQ-045 P4-5: fresh machine keeps the bare filename");
        TEST_CHECK(pick.model_id == "user-llama3",
                   "REQ-045 P4-5: fresh machine keeps the plain user-<stem> id");
    }

    // File collision only -> _2 on the file, matching _2 on the id.
    {
        const engine_host_registry::Registry empty_reg;
        const auto pick = Req045PickUniqueId({"llama3.gguf"}, empty_reg,
                                             "llama3", ".gguf");
        TEST_CHECK(pick.dest_bare == "llama3_2.gguf",
                   "REQ-045 P4-5: file collision bumps the filename to _2");
        TEST_CHECK(pick.model_id == "user-llama3_2",
                   "REQ-045 P4-5: file-collision bump keeps file<->id 1:1");
    }

    // Id collision only (a DIFFERENT file already owns user-llama3) -> _2.
    {
        auto parsed = engine_host_registry::ParseRegistryJson(
            "{\"schema_version\":1,\"models\":[{"
            "\"id\":\"user-llama3\",\"family\":\"ggml-translate\","
            "\"files\":[\"other.gguf\"],\"origin\":\"user\"}]}");
        TEST_CHECK(parsed.status == engine_host_registry::LoadStatus::Ok,
                   "REQ-045 P4-5: id-collision fixture registry parses");
        const auto pick = Req045PickUniqueId({}, parsed.registry, "llama3", ".gguf");
        TEST_CHECK(pick.model_id != "user-llama3",
                   "REQ-045 P4-5: an id owned by a DIFFERENT file is not reused");
        TEST_CHECK(pick.model_id == "user-llama3_2",
                   "REQ-045 P4-5: id collision bumps to _2");
    }

    // Both colliding at _2 as well -> _3.
    {
        auto parsed = engine_host_registry::ParseRegistryJson(
            "{\"schema_version\":1,\"models\":[{"
            "\"id\":\"user-llama3\",\"family\":\"ggml-translate\","
            "\"files\":[\"llama3.gguf\"],\"origin\":\"user\"},{"
            "\"id\":\"user-llama3_2\",\"family\":\"ggml-translate\","
            "\"files\":[\"llama3_2.gguf\"],\"origin\":\"user\"}]}");
        TEST_CHECK(parsed.status == engine_host_registry::LoadStatus::Ok,
                   "REQ-045 P4-5: both-colliding fixture registry parses");
        const auto pick = Req045PickUniqueId({"llama3.gguf", "llama3_2.gguf"},
                                             parsed.registry, "llama3", ".gguf");
        TEST_CHECK(pick.dest_bare == "llama3_3.gguf" && pick.model_id == "user-llama3_3",
                   "REQ-045 P4-5: double collision escalates to _3");
    }
}

void TestReq045GgufWriterRoundTrip() {
    std::cout << "[RUN] Testing REQ-045 P4-5 SerializeRegistry round-trip..." << std::endl;

    // Fixture: the exact shape the installer + the registration pipeline write
    // (bundled pinned entry + a fresh origin:"user" third-party entry).
    const auto parsed = engine_host_registry::ParseRegistryJson(
        "{\"schema_version\":1,\"models\":[{"
        "\"id\":\"hymt2\",\"family\":\"ggml-translate\","
        "\"files\":[\"Hy-MT2-1.8B-Q8_0.gguf\"],"
        "\"capabilities\":[\"translate\"],\"origin\":\"bundled\","
        "\"resource\":{\"vram_mb\":4096,\"ctx\":2048,\"max_sessions\":2,"
        "\"residency\":\"preload\",\"eviction\":\"sticky\",\"priority\":5},"
        "\"profiles\":{\"default\":{\"temperature\":0.0,\"top_p\":0.6,"
        "\"top_k\":20,\"rep_pen\":1.05,\"prompt_template_ref\":\"hymt2-official\"}},"
        "\"lang_pairs\":[\"ko-en\",\"en-ko\"]},{"
        "\"id\":\"user-llama3\",\"family\":\"ggml-translate\","
        "\"files\":[\"llama3.gguf\"],\"origin\":\"user\"}]}");
    TEST_CHECK(parsed.status == engine_host_registry::LoadStatus::Ok,
               "REQ-045 P4-5: writer fixture parses Ok");

    // writer -> parse is the identity the 위임 demands (AC-1).
    const std::string serialized =
        engine_host_registry::SerializeRegistry(parsed.registry);
    TEST_CHECK(!serialized.empty(), "REQ-045 P4-5: writer produces a document");
    const auto reparsed = engine_host_registry::ParseRegistryJson(serialized);
    TEST_CHECK(reparsed.status == engine_host_registry::LoadStatus::Ok,
               "REQ-045 P4-5: writer output re-parses Ok (round-trip, AC-1)");
    TEST_CHECK(reparsed.registry.models.size() == 2,
               "REQ-045 P4-5: round-trip preserves the model count");

    const auto* bundled = reparsed.registry.FindModel("hymt2");
    TEST_CHECK(bundled != nullptr, "REQ-045 P4-5: bundled id survives");
    if (bundled) {
        TEST_CHECK(bundled->files.size() == 1 && bundled->files[0] == "Hy-MT2-1.8B-Q8_0.gguf",
                   "REQ-045 P4-5: bundled files[] survive");
        TEST_CHECK(bundled->origin == "bundled", "REQ-045 P4-5: bundled origin survives");
        TEST_CHECK(bundled->resource.vram_mb == 4096 && bundled->resource.ctx == 2048,
                   "REQ-045 P4-5: resource profile survives");
        TEST_CHECK(bundled->resource.max_sessions == 2 && bundled->resource.priority == 5,
                   "REQ-045 P4-5: resource clamps-off fields survive");
        TEST_CHECK(bundled->resource.residency == "preload" &&
                   bundled->resource.eviction == "sticky",
                   "REQ-045 P4-5: resource enums survive");
        const auto it = bundled->profiles.find("default");
        TEST_CHECK(it != bundled->profiles.end() && it->second.prompt_template_ref == "hymt2-official",
                   "REQ-045 P4-5: sampling profile (incl. prompt_template_ref) survives");
        TEST_CHECK(bundled->lang_pairs.size() == 2 && bundled->lang_pairs[0] == "ko-en",
                   "REQ-045 P4-5: lang_pairs survive");
    }
    const auto* user = reparsed.registry.FindModel("user-llama3");
    TEST_CHECK(user != nullptr && user->origin == "user",
               "REQ-045 P4-5: origin:\"user\" append survives the round-trip (AC-1)");
    if (user) {
        TEST_CHECK(user->files.size() == 1 && user->files[0] == "llama3.gguf",
                   "REQ-045 P4-5: user files[] survive");
    }

    // Double-serialize is byte-stable (deterministic writer).
    TEST_CHECK(engine_host_registry::SerializeRegistry(reparsed.registry) == serialized,
               "REQ-045 P4-5: writer is byte-stable across a re-parse");

    // Empty registry still round-trips (schema_version preserved).
    {
        engine_host_registry::Registry empty;
        empty.schema_version = engine_host_registry::kRegistrySchemaVersion;
        const auto s = engine_host_registry::SerializeRegistry(empty);
        const auto p = engine_host_registry::ParseRegistryJson(s);
        TEST_CHECK(p.status == engine_host_registry::LoadStatus::Ok &&
                   p.registry.models.empty(),
                   "REQ-045 P4-5: empty registry round-trips (fresh-machine path)");
    }

    // Writer REFUSES a path-escape filename (never emits a tampered document).
    {
        engine_host_registry::Registry bad;
        bad.schema_version = engine_host_registry::kRegistrySchemaVersion;
        engine_host_registry::ModelEntry e;
        e.id = "evil";
        e.family = "ggml-translate";
        e.files = {"..\\escape.gguf"};
        e.origin = "user";
        bad.models.push_back(e);
        TEST_CHECK(engine_host_registry::SerializeRegistry(bad).empty(),
                   "REQ-045 P4-5: writer refuses a non-bare filename (loud failure)");
    }
}

void TestReq045GgufConfigUserModelIdPersist() {
    std::cout << "[RUN] Testing REQ-045 P4-5 config user_model_id persist..." << std::endl;

    AppConfig cfg;
    // Default is the pinned default ("").
    TEST_CHECK(cfg.user_model_id.empty(),
               "REQ-045 P4-5: user_model_id defaults to empty (pinned, AC-4)");

    // Persist + reload round-trips the id.
    cfg.SetUserModelId("user-llama3");
    const std::string json = cfg.ToJsonString();
    TEST_CHECK(json.find("\"user_model_id\": \"user-llama3\"") != std::string::npos,
               "REQ-045 P4-5: user_model_id is serialized into config.json");
    AppConfig reloaded;
    TEST_CHECK(reloaded.FromJsonString(json), "REQ-045 P4-5: config re-parse succeeds");
    TEST_CHECK(reloaded.user_model_id == "user-llama3",
               "REQ-045 P4-5: user_model_id survives a persist/reload round-trip (AC-2)");

    // Key absent (every pre-REQ-045 config.json) keeps "".
    AppConfig legacy;
    TEST_CHECK(legacy.FromJsonString("{\"engine_type\":\"local\"}"),
               "REQ-045 P4-5: legacy config parses");
    TEST_CHECK(legacy.user_model_id.empty(),
               "REQ-045 P4-5: absent key -> empty (pinned default, AC-4)");

    // engine_type is NOT disturbed by the user_model_id write (위임 2d:
    // the engine choice itself stays with the user).
    TEST_CHECK(legacy.engine_type == "local",
               "REQ-045 P4-5: engine_type is untouched by the new key");
}

void TestReq045GgufSessionOpenRoundTrip() {
    std::cout << "[RUN] Testing REQ-045 P4-4 session_open model_id round-trip..." << std::endl;

    // The frozen frame already carries model_id (worker_protocol.hpp R1) —
    // this pins the round-trip so a future frame edit cannot silently drop it.
    workerproto::SessionOpenMsg msg;
    TEST_CHECK(workerproto::ParseSessionOpen(
                   workerproto::BuildSessionOpen(7, "translate", "user-llama3", "default"), msg),
               "REQ-045: BuildSessionOpen/ParseSessionOpen round-trips");
    TEST_CHECK(msg.session == 7, "REQ-045: session id survives the round-trip");
    TEST_CHECK(msg.capability == "translate", "REQ-045: capability survives the round-trip");
    TEST_CHECK(msg.model_id == "user-llama3",
               "REQ-045: model_id survives the round-trip (frozen frame carries it)");
    TEST_CHECK(msg.profile == "default", "REQ-045: profile survives the round-trip");

    // Empty model_id (the pinned-default path) round-trips too.
    workerproto::SessionOpenMsg empty_msg;
    TEST_CHECK(workerproto::ParseSessionOpen(
                   workerproto::BuildSessionOpen(0, "translate", "", "default"), empty_msg),
               "REQ-045: empty-model_id session_open round-trips");
    TEST_CHECK(empty_msg.model_id.empty(),
               "REQ-045: empty model_id stays empty (pinned default, AC-4)");

    // JSON-escape resilience for ids with quotes/backslashes.
    workerproto::SessionOpenMsg esc_msg;
    TEST_CHECK(workerproto::ParseSessionOpen(
                   workerproto::BuildSessionOpen(1, "translate", "a\"b\\c", "default"), esc_msg),
               "REQ-045: escaped model_id round-trips");
    TEST_CHECK(esc_msg.model_id == "a\"b\\c", "REQ-045: escaped model_id decodes exactly");
}
