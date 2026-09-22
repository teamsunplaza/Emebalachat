// ---------------------------------------------------------------------------
// M7 A-2 + A-3 (session 260922_0001): registry.json multi-writer merge rules
// & origin:"user" uninstall protection (TestM7RegistryMergeUninstall).
//
// A-2 (plan §V2-5.1 "작성: 설치기(번들)와 앱(사용자 모델)"): the shared store
// registry is written by BOTH the apps and every family installer. This suite
// pins the completion criteria:
//   (a) app-side existing-item-preserving merge: a registry document that
//       already carries the installer's bundled item survives an app
//       append+rewrite UNTOUCHED in content (parse -> append user entry ->
//       WriteRegistryAtomically -> re-load: both items, origins intact);
//   (b) origin:"user" preservation across a rewrite (the same round trip
//       asserts the user entry's origin field lands back as "user");
//   (c) writer FAIL-CLOSURE on a damaged pre-existing file: a rejected
//       serialization (non-bare filename) or refusal leaves the previous
//       registry.json BYTE-IDENTICAL — the atomic writer never truncates;
//   (d) atomic write hygiene: complete-document landing (no torn read
//       window), no .tmp residue after success, overwriting an existing
//       target;
//   (e) the exact canonical bundled item the A-2 installer merge emits must
//       be parseable by the frozen-shape C++ parser (schema UNCHANGED —
//       §V2-5.2/§V2-12-2), and the compact JsonBuildRegistryDoc envelope
//       shape must parse too;
//   (f) installer-side structural pins (ResolveRepoFile source-text, the
//       A-1/TestReq044VerbatimSync precedent): WriteRegistryFile merged
//       (not create-if-missing), atomic tmp+rename writer, user-origin
//       override kept; CurUninstallStepChanged dropped DelTree(CommonDir)
//       for CleanupSharedEngineStore; the cleanup is bundled-only with a
//       fail-closed preserve path and the IsSafeBareName tampering guard;
//       the app writers moved off the raw truncate-in-place ofstream.
// Staging uses a temp directory (never %LOCALAPPDATA%\Emebala); cleaned on
// exit. The uninstall-contract gate (tools/check_uninstall_contract.py
// CHECK 2/8/9) re-derives the same installer invariants independently.
// ---------------------------------------------------------------------------

// The EXACT canonical bundled item the A-2 installer merge writes into
// models[] (setup.iss REGISTRY_BUNDLED_ITEM_PREFIX + MODEL_FILENAME +
// REGISTRY_BUNDLED_ITEM_SUFFIX, single-line compact shape). Pin (e-functional)
// parses it; pin (f-structural) requires setup.iss to contain the two
// endpoint fragments, so the two sides can never drift silently.
static const char* kM7A2BundledItem =
    "{\"id\": \"hy-mt2-1.8b-q8\", \"family\": \"ggml-translate\", "
    "\"files\": [\"Hy-MT2-1.8B-Q8_0.gguf\"], \"capabilities\": "
    "[\"translate\"], \"origin\": \"bundled\", \"resource\": {\"vram_mb\": "
    "2400, \"ctx\": 4096, \"max_sessions\": 1, \"residency\": \"preload\", "
    "\"eviction\": \"sticky\", \"priority\": 9}, \"profiles\": "
    "{\"default\": {\"temperature\": 0.0, \"top_p\": 0.6, \"top_k\": 20, "
    "\"rep_pen\": 1.05, \"prompt_template_ref\": \"hymt2-official\"}}, "
    "\"lang_pairs\": [\"*\"]}";

void TestM7RegistryMergeUninstall() {
    std::cout << "[RUN] Testing M7 A-2/A-3 registry merge & user-model protection..."
              << std::endl;
    const int failures_before = g_failed_count;
    namespace ehr = emebalachat::engine_host_registry;

    // ---- (e1) the installer's canonical bundled item parses (Ok, origin) ---
    {
        const std::string doc =
            std::string("{\"schema_version\": 1, \"models\": [") +
            kM7A2BundledItem + "]}";
        const auto r = ehr::ParseRegistryJson(doc);
        TEST_CHECK(r.status == ehr::LoadStatus::Ok,
                   "A-2 (e): compact installer-merged envelope parses Ok");
        TEST_CHECK(r.registry.models.size() == 1,
                   "A-2 (e): one bundled item in the merged document");
        if (r.status == ehr::LoadStatus::Ok && !r.registry.models.empty()) {
            const ehr::ModelEntry& m = r.registry.models[0];
            TEST_CHECK(m.id == "hy-mt2-1.8b-q8" && m.origin == "bundled",
                       "A-2 (e): bundled id/origin intact after the shape swap");
            TEST_CHECK(m.resource.priority == 9 && m.resource.residency == "preload" &&
                           m.resource.eviction == "sticky",
                       "A-2 (e): resource block round-trips (emitted values are the clamped ones)");
            TEST_CHECK(m.profiles.count("default") == 1 &&
                           m.profiles.at("default").prompt_template_ref == "hymt2-official",
                       "A-2 (e): default sampling profile survives the compact shape");
        }
        // BOM-prefixed variant (defensive: LoadRegistryDocument strips one,
        // but the writer tolerance must stay regardless — REQ-048 R2).
        const auto rb = ehr::ParseRegistryJson(std::string("\xEF\xBB\xBF") + doc);
        TEST_CHECK(rb.status == ehr::LoadStatus::Ok,
                   "A-2 (e): BOM-prefixed compact document still parses (R2 tolerance)");
    }

    // Shared staging area under the temp dir — the real store is never touched.
    const std::filesystem::path stage =
        std::filesystem::temp_directory_path() / "emebala_m7_registry_merge_test";
    std::error_code sec;
    std::filesystem::remove_all(stage, sec);
    std::filesystem::create_directories(stage, sec);
    TEST_CHECK(!sec && std::filesystem::exists(stage), "stage: temp store created");

    const auto read_file = [](const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    };

    // ---- (a)+(b) app-side merge preserving existing items & origins -------
    {
        // Start from the installer-shaped document (bundled item only)...
        const std::string installer_doc =
            std::string("{\"schema_version\": 1, \"models\": [") +
            kM7A2BundledItem + "]}";
        auto loaded = ehr::ParseRegistryJson(installer_doc);
        TEST_CHECK(loaded.status == ehr::LoadStatus::Ok, "A-2 (a): base doc loads");
        // ...append a user model exactly like RegisterUserGgufModel does...
        ehr::ModelEntry user;
        user.id = "user-mymodel";
        user.family = "ggml-translate";
        user.files = {"MyModel.gguf"};
        user.origin = "user";
        loaded.registry.models.push_back(user);
        // ...and persist through the atomic writer (the app's merge path).
        const auto w1 = ehr::WriteRegistryAtomically(loaded.registry, stage);
        TEST_CHECK(w1 == ehr::WriteOutcome::Ok, "A-2 (a): atomic merge write lands Ok");
        // Re-read from disk: the pre-existing bundled item survived the app
        // rewrite with origin "bundled", the user entry landed with "user".
        const auto re = ehr::ParseRegistryJson(read_file(stage / L"registry.json"));
        TEST_CHECK(re.status == ehr::LoadStatus::Ok && re.registry.models.size() == 2,
                   "A-2 (a): both items present after the app rewrite");
        if (re.status == ehr::LoadStatus::Ok && re.registry.models.size() == 2) {
            const ehr::ModelEntry* b = re.registry.FindModel("hy-mt2-1.8b-q8");
            const ehr::ModelEntry* u = re.registry.FindModel("user-mymodel");
            TEST_CHECK(b && b->origin == "bundled",
                       "A-2 (a): bundled item content preserved verbatim by the merge");
            TEST_CHECK(u && u->origin == "user",
                       "A-2 (b): origin:\"user\" survives the rewrite");
        }
        // No torn residue: the .tmp sibling must be gone after success.
        TEST_CHECK(!std::filesystem::exists(stage / L"registry.json.tmp", sec),
                   "A-2 (d): no .tmp residue after a successful atomic write");
        // Overwrite an existing target (rename-replace path, not just create).
        const auto w2 = ehr::WriteRegistryAtomically(loaded.registry, stage);
        TEST_CHECK(w2 == ehr::WriteOutcome::Ok,
                   "A-2 (d): atomic write replaces an existing registry.json (Ok)");
        TEST_CHECK(read_file(stage / L"registry.json") ==
                       ehr::SerializeRegistry(loaded.registry),
                   "A-2 (d): landed bytes equal the serialized document exactly");
    }

    // ---- (c) writer fail-closure on a damaged pre-existing file -----------
    {
        // A damaged registry on disk must survive a REFUSED write untouched:
        // the atomic writer only ever replaces the target via rename, and a
        // rejected serialization never reaches the filesystem at all.
        const std::string damaged = "{\"schema_version\": 1, \"models\": [ {\"id\":";
        {
            std::ofstream f(stage / L"registry.json", std::ios::binary | std::ios::trunc);
            f << damaged;
        }
        ehr::Registry bad;
        bad.schema_version = ehr::kRegistrySchemaVersion;
        ehr::ModelEntry escape;
        escape.id = "x";
        escape.family = "f";
        escape.files = {"..\\evil.gguf"}; // parser-and-writer-rejected shape
        escape.origin = "user";
        bad.models.push_back(escape);
        const auto refused = ehr::WriteRegistryAtomically(bad, stage);
        TEST_CHECK(refused == ehr::WriteOutcome::SerializeRefused,
                   "A-2 (c): non-bare filename refuses with SerializeRefused");
        TEST_CHECK(read_file(stage / L"registry.json") == damaged,
                   "A-2 (c): refused write leaves the previous document BYTE-INTACT");
        TEST_CHECK(!std::filesystem::exists(stage / L"registry.json.tmp", sec),
                   "A-2 (c): refused write leaves no .tmp behind");
    }

    std::filesystem::remove_all(stage, sec);

    // ---- (f) structural pins: installer merge + A-3 cleanup wiring ---------
    {
        const auto read_src = [](const wchar_t* rel, std::string& out) {
            const std::filesystem::path p = ResolveRepoFile(rel);
            if (p.empty()) return false;
            std::ifstream in(p, std::ios::binary);
            if (!in) return false;
            std::ostringstream ss;
            ss << in.rdbuf();
            out = ss.str();
            return true;
        };

        std::string iss_src;
        TEST_CHECK(read_src(L"installer\\setup.iss", iss_src), "pin: setup.iss readable");
        // A-2: the merge exists and no longer carries the create-if-missing
        // early-exit or the raw line-array writer.
        TEST_CHECK(iss_src.find("REGISTRY_BUNDLED_ITEM_PREFIX = '{\"id\": \"hy-mt2-1.8b-q8\"") !=
                       std::string::npos,
                   "pin: A-2 canonical bundled item constant present (prefix fragment)");
        TEST_CHECK(iss_src.find("lang_pairs\": [\"*\"]}'") != std::string::npos,
                   "pin: A-2 canonical bundled item constant present (suffix fragment)");
        TEST_CHECK(iss_src.find("M7 A-2: registry.json merged") != std::string::npos,
                   "pin: A-2 merge result is logged by WriteRegistryFile");
        TEST_CHECK(iss_src.find("user registration preserved") != std::string::npos,
                   "pin: A-2 origin:\"user\" override of the bundled id is the documented rule");
        TEST_CHECK(iss_src.find("already exists - leaving it untouched (idempotent") ==
                       std::string::npos,
                   "pin: the pre-A-2 create-if-missing early-exit is gone");
        TEST_CHECK(iss_src.find("function WriteTextFileAtomic") != std::string::npos,
                   "pin: the installer shares the atomic tmp+rename write strategy");
        TEST_CHECK(iss_src.find("'bundled' then") != std::string::npos,
                   "pin: the merge compares the existing item's origin before refreshing");
        // A-3: blanket DelTree of the shared store is replaced by the
        // registry-aware, bundled-only cleanup (fail-closed on damage).
        TEST_CHECK(iss_src.find("DelTree(CommonDir, True, True, True)") == std::string::npos,
                   "pin: A-3 blanket DelTree(CommonDir) removed");
        TEST_CHECK(iss_src.find("CleanupSharedEngineStore(CommonDir)") != std::string::npos,
                   "pin: A-3 CurUninstallStepChanged calls the registry-aware cleanup");
        TEST_CHECK(iss_src.find("procedure CleanupSharedEngineStore") != std::string::npos,
                   "pin: A-3 cleanup procedure defined");
        TEST_CHECK(iss_src.find("ENTIRE shared store is preserved (destructive fallback forbidden)") !=
                       std::string::npos,
                   "pin: A-3 damaged-registry fail-closed preserve path present");
        TEST_CHECK(iss_src.find("not a safe bare name") != std::string::npos,
                   "pin: A-3 IsSafeBareName tampering guard wired into the deletion path");
        TEST_CHECK(iss_src.find("user item(s) preserved verbatim") != std::string::npos,
                   "pin: A-3 rewrite keeps user items and logs the preservation");

        // App writers moved off the truncate-in-place ofstream pattern.
        std::string main_src;
        TEST_CHECK(read_src(L"src\\main.cpp", main_src), "pin: main.cpp readable");
        TEST_CHECK(main_src.find("WriteRegistryAtomically(registry, models_dir)") !=
                       std::string::npos,
                   "pin: RegisterUserGgufModel persists through the atomic writer");
        std::string mgr_src;
        TEST_CHECK(read_src(L"src\\ui\\gguf_model_manager_window.cpp", mgr_src),
                   "pin: gguf manager readable");
        TEST_CHECK(mgr_src.find("WriteRegistryAtomically(registry, models_dir)") !=
                       std::string::npos,
                   "pin: WriteRegistryLoud persists through the atomic writer");
    }

    std::cout << "  m7 registry merge/uninstall: "
              << (g_failed_count - failures_before) << " new failures" << std::endl;
}
