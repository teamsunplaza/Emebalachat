// ---------------------------------------------------------------------------
// REQ-055: the engine host must route plain local requests with the CURRENT
// config pin, not the boot snapshot.
// User report: "Pausing with F9 and resuming with F9 makes the engine revert
// to the built-in local engine instead of my selected engine." Root cause
// (live-evidenced): host_main cached config.json's C1-gated user_model_id ONCE
// at boot (g_user_model_id) and both dispatchers served it — v1 relayed it via
// session_open once (bool model_relayed) and probed only the pinned model
// file on the fast path; v2 read the same snapshot per job. A host booted
// under user_gguf kept serving the user model for plain local requests for as
// long as traffic kept it alive — the only refresh was the 10-minute idle-
// exit respawn, and a busy host never idles out. The worker's reset path is
// verified, no worker change: session_open with an EMPTY model_id sets
// active_model_id="" and ResolveModelFile maps empty -> pinned Hy-MT2.
// EnsureWorkerModelRelayed's old early return skipped even that reset.
// Fix: LoadUserModelIdFromConfigLive (mtime/size-cached live read, reusing
// the exact C1-gated parse) per job in both dispatchers; the relay now sends
// empty ids (reset-to-pinned) and both loops do string change-detection.
//   (a-d) behavioral — the live reader against rotating temp lad dirs
//         (TestReq046HostUserModelIdGate fixture precedent).
//   (e)   source pins — host_main.cpp uses the live pin, the resolved fast
//         path, keeps g_user_model_id only at declaration + boot log, and
//         EnsureWorkerModelRelayed no longer skips empty ids; setup.iss
//         bumped to .r6 (orchestrator + worker binaries changed, REQ-057).
// ResolveRepoFile source pins (L97 of run_tests.cpp); unresolvable file ->
// the section is skipped, exactly like the req048_r2 pattern.
// ---------------------------------------------------------------------------

void TestReq055LiveUserModelPin() {
    std::cout << "[RUN] Testing REQ-055 live user-model pin..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a-d) behavioral: the mtime/size-cached live reader -------------
    namespace fs = std::filesystem;
    std::error_code ec;
    auto fixture_fail = [&](const char* what) {
        TEST_CHECK(false, what);
    };
    const fs::path rootA = fs::temp_directory_path(ec) / L"emebala_req055_live_a";
    const fs::path rootB = fs::temp_directory_path(ec) / L"emebala_req055_live_b";
    if (ec) {
        fixture_fail("REQ-055 fixture: temp dir unavailable");
    } else {
        auto write_config = [&](const fs::path& root, const std::string& body) {
            fs::create_directories(root / L"Emebalachat", ec);
            ec.clear();
            std::ofstream out(root / L"Emebalachat" / L"config.json",
                              std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                fixture_fail("REQ-055 fixture: temp config.json write failed");
                return;
            }
            out << body;
        };
        fs::remove_all(rootA, ec);
        ec.clear();
        fs::remove_all(rootB, ec);
        ec.clear();

        // (a) dir A under user_gguf -> the id comes through.
        write_config(rootA, "{\"engine_type\": \"user_gguf\", \"user_model_id\": \"user-live\"}");
        TEST_CHECK(enginehost::LoadUserModelIdFromConfigLive(rootA.wstring()) == "user-live",
                   "REQ-055 (a): live read returns the C1-gated id (user_gguf)");

        // (b) dir B with engine_type=local + the same id persisted -> "" —
        // the single-slot cache is keyed by the RESOLVED lad dir, so a
        // different config root must not see A's cached id.
        write_config(rootB, "{\"engine_type\": \"local\", \"user_model_id\": \"user-live\"}");
        TEST_CHECK(enginehost::LoadUserModelIdFromConfigLive(rootB.wstring()).empty(),
                   "REQ-055 (b): a different lad dir is cache-isolated "
                   "(local + persisted id -> \"\")");
        // And A's entry still answers from its own signature.
        TEST_CHECK(enginehost::LoadUserModelIdFromConfigLive(rootA.wstring()) == "user-live",
                   "REQ-055 (b): the original dir still resolves its own id");

        // (c) rewrite dir A's config to local -> the mtime/size change
        // invalidates the cache and the live read returns "" (the
        // user_gguf -> local switch that used to stay stale until respawn).
        // The rewrite also changes the file size, so the signature moves even
        // on filesystems with coarse mtime granularity; the small sleep adds
        // margin for those that timestamp in ticks.
        write_config(rootA, "{\"engine_type\": \"local\", \"user_model_id\": \"user-live\"}");
        ::Sleep(120);
        TEST_CHECK(enginehost::LoadUserModelIdFromConfigLive(rootA.wstring()).empty(),
                   "REQ-055 (c): a rewritten config (switch to local) is re-read "
                   "live -> \"\" (reset to pinned)");

        // (d) a never-created dir -> "" without crashing.
        const fs::path rootC = fs::temp_directory_path(ec) / L"emebala_req055_live_c_missing";
        TEST_CHECK(enginehost::LoadUserModelIdFromConfigLive(rootC.wstring()).empty(),
                   "REQ-055 (d): a missing config dir -> \"\" (no crash)");

        fs::remove_all(rootA, ec);
        ec.clear();
        fs::remove_all(rootB, ec);
        ec.clear();
    }

    // ---- (e) source pins: the dispatchers use the live pin ---------------
    const std::filesystem::path host_main = ResolveRepoFile(L"src\\host_main.cpp");
    if (host_main.empty()) {
        std::cout << "[SKIP] src/host_main.cpp not resolvable; REQ-055 (e) pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(host_main, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        // Both dispatchers read the live pin per job.
        TEST_CHECK(src.find("LoadUserModelIdFromConfigLive") != std::string::npos,
                   "REQ-055 (e): the dispatchers use the live config pin");
        // v1 fast path probes the RESOLVED model file, not the pinned-only
        // ModelFileExists() shadow of the boot snapshot.
        TEST_CHECK(src.find("ModelFileExistsFor(pin)") != std::string::npos,
                   "REQ-055 (e): the v1 fast path probes the resolved model file");
        // g_user_model_id survives ONLY at its declaration + the boot log
        // block (assignment + the two uses inside the host/004 DIAG_LOG
        // line) = 4 textual occurrences; any dispatcher read trips this.
        {
            size_t pos = src.find("g_user_model_id");
            int occurrences = 0;
            while (pos != std::string::npos) {
                ++occurrences;
                pos = src.find("g_user_model_id", pos + 1);
            }
            TEST_CHECK(occurrences == 4,
                       "REQ-055 (e): g_user_model_id appears only at its declaration "
                       "and the boot log (4 occurrences) — no dispatcher reads it");
        }
        // EnsureWorkerModelRelayed relays empty ids now (the reset path): the
        // early-return skip is gone. Scope the search to the function body.
        const size_t fn_pos = src.find("EnsureWorkerModelRelayed");
        const size_t fn_end = src.find("DispatcherLoop", fn_pos);
        const size_t skip_pos = fn_pos == std::string::npos
                                    ? std::string::npos
                                    : src.find("model_id.empty() ||", fn_pos);
        TEST_CHECK(skip_pos == std::string::npos ||
                       (fn_end != std::string::npos && skip_pos > fn_end),
                   "REQ-055 (e): EnsureWorkerModelRelayed no longer skips empty ids");
    }

    // setup.iss: the orchestrator binary changed, so the bundled engine
    // revision bumps (component-model rule A-2 would otherwise keep a stale
    // store copy on same-version updates).
    const std::filesystem::path setup_iss = ResolveRepoFile(L"installer\\setup.iss");
    if (setup_iss.empty()) {
        std::cout << "[SKIP] installer/setup.iss not resolvable; REQ-055 revision pin skipped."
                  << std::endl;
    } else {
        std::ifstream in(setup_iss, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        TEST_CHECK(src.find("#define ENGINE_REVISION \".r6\"") != std::string::npos,
                   "REQ-055 (e): ENGINE_REVISION bumped to .r6 (orchestrator + worker binaries changed)");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-055 live user-model pin checks passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-055 live user-model pin: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
