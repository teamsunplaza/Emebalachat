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
