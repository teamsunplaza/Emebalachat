// mutex_family_pin_tests.inc — B3 fix regression pins (session 260928_0001).
//
// Root cause pinned: the translate worker's single-instance mutex name was a
// family-agnostic constant (worker_protocol.hpp kWorkerSingleInstanceMutexName),
// so the B-T4 per-family pool's SECOND-family worker hit ERROR_ALREADY_EXISTS at
// the wWinMain gate, exited 0 pre-connect, and wedged the host in the 15 s
// ConnectNamedPipe timeout (wmgr/008) + model_missing. The fix parameterizes
// the mutex name per family:
//   * worker_protocol.hpp  : wp::MutexNameForFamily(family) — legacy exact for
//                            ""/"ggml-translate", hyphen->underscore suffix map.
//   * host_v2_worker_manager: SpawnRequest.family -> `--mutex <family>` on the
//                            child command line (LaunchWorkerProcess).
//   * ggml_translate_worker : WorkerArgs.mutex_family parsed from --mutex; the
//                            wWinMain gate derives the name BEFORE CreateMutexW.
//
// These pins are PURE (no process spawn): the derivation contract is pinned
// directly, and the host propagation is pinned through the injectable SpawnFn
// seam (the same fake-launcher pattern the existing worker-manager suites use).

namespace mf_pin {

// ---- derivation contract ----------------------------------------------------

void TestMutexNameForFamily() {
    std::cout << "[RUN] Testing per-family mutex name derivation (B3 fix)..." << std::endl;
    const int failures_before = g_failed_count;
    namespace wp = emebalachat::workerproto;

    // 1. The bare default family maps to the EXACT legacy constant — a mixed
    //    old/new host+worker pair keeps single-instance protection for the
    //    default family (this is the deployed name since M6).
    TEST_CHECK(wp::MutexNameForFamily(L"ggml-translate") ==
                   L"Local\\EmebalaEngine_Worker_ggml_translate",
               "mutex: ggml-translate -> exact legacy constant");
    TEST_CHECK(wp::MutexNameForFamily(L"") ==
                   L"Local\\EmebalaEngine_Worker_ggml_translate",
               "mutex: empty (arg-absent) -> exact legacy constant (backward compat)");

    // 2. A pool-derived user family maps to a DISTINCT new name (hyphens ->
    //    underscores, matching the legacy constant's own convention).
    const std::wstring user =
        wp::MutexNameForFamily(L"ggml-translate-user-MiLMMT-46-4B-v1.0.Q5_K_M_3");
    TEST_CHECK(user == L"Local\\EmebalaEngine_Worker_ggml_translate_user_MiLMMT_46_4B_v1.0.Q5_K_M_3",
               "mutex: user family -> hyphen-to-underscore derived name");
    TEST_CHECK(user != wp::MutexNameForFamily(L"ggml-translate"),
               "mutex: user family name is distinct from the legacy name");

    // 3. Two distinct families derive distinct names (the B-T4 pool
    //    coexistence invariant — this is the exact property the old shared
    //    constant violated).
    const std::wstring a = wp::MutexNameForFamily(L"ggml-translate-user-X");
    const std::wstring b = wp::MutexNameForFamily(L"ggml-translate-user-Y");
    TEST_CHECK(a != b, "mutex: two distinct user families derive distinct names");
    TEST_CHECK(a != wp::MutexNameForFamily(L""),
               "mutex: user family differs from arg-absent legacy name");
    TEST_CHECK(b != wp::MutexNameForFamily(L"ggml-translate"),
               "mutex: second user family differs from the bare default");

    // 4. Determinism: the same family always derives the same name.
    TEST_CHECK(wp::MutexNameForFamily(L"ggml-translate-user-X") == a,
               "mutex: derivation is deterministic (same family -> same name)");

    // 5. The charset guard: only the sanitizer-restricted [A-Za-z0-9._-]
    //    surface reaches this helper (PoolFamilyForModel enforces it); a
    //    no-dash family passes through the suffix unchanged.
    TEST_CHECK(wp::MutexNameForFamily(L"ggml-translate-user.X_1") ==
                   L"Local\\EmebalaEngine_Worker_ggml_translate_user.X_1",
               "mutex: dot/underscore/alnum pass through the suffix unchanged");

    std::cout << "[DONE] MutexNameForFamily (failures: "
              << (g_failed_count - failures_before) << ")" << std::endl;
}

// ---- host propagation: EnsureSpawned stamps family into SpawnRequest --------

void TestSpawnRequestFamilyPropagation() {
    std::cout << "[RUN] Testing SpawnRequest.family propagation (B3 fix)..." << std::endl;
    const int failures_before = g_failed_count;
    namespace hv2 = emebalachat::host_v2;

    struct Ctx {
        std::vector<std::wstring> seen_families;
    } ctx;

    // Fake launcher (the established seam pattern): records req.family, then
    // fails the spawn so EnsureSpawned returns quickly WITHOUT any pipe
    // handshake — the family capture happens before the spawn result matters.
    hv2::SpawnFn fake = [](const hv2::SpawnRequest& req, void* user) -> hv2::SpawnResult {
        auto* c = static_cast<Ctx*>(user);
        c->seen_families.push_back(req.family);
        hv2::SpawnResult r;
        r.ok = false; // never reaches the handshake
        r.last_error = ERROR_INVALID_PARAMETER;
        return r;
    };

    // 1. A directly-registered bare family reaches the launcher verbatim.
    //    Use the exe path of THIS test binary (guaranteed present) so the
    //    exe-existence gate passes and the fake launcher is actually invoked.
    {
        wchar_t exe_path[MAX_PATH] = {0};
        const DWORD n = ::GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        TEST_CHECK(n > 0, "prop: this test exe path resolves for the fake spawn");
        hv2::WorkerManager mgr(fake, [](HANDLE, void*) {}, &ctx);
        mgr.RegisterFamily(L"ggml-translate", exe_path);
        hv2::WorkerHandle* h = mgr.EnsureSpawned(L"ggml-translate");
        TEST_CHECK(h == nullptr, "prop: failed fake spawn -> EnsureSpawned nullptr");
        TEST_CHECK(ctx.seen_families.size() == 1,
                   "prop: the fake launcher saw exactly one spawn");
        if (ctx.seen_families.size() == 1) {
            TEST_CHECK(ctx.seen_families[0] == L"ggml-translate",
                       "prop: bare family reaches the launcher verbatim");
        }
    }

    // 2. A runtime pool-registered user family also reaches the launcher
    //    verbatim (the B-T4 second-family path that used to collide).
    ctx.seen_families.clear();
    {
        wchar_t exe_path[MAX_PATH] = {0};
        const DWORD n = ::GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        TEST_CHECK(n > 0, "prop: exe path resolves (pool leg)");
        hv2::WorkerManager mgr(fake, [](HANDLE, void*) {}, &ctx);
        mgr.SetTranslateExePath(exe_path);
        // TranslatePoolEnsure("user-MiLMMT-46-4B-v1.0.Q5_K_M_3") derives the
        // family and registers it; EnsureSpawned then stamps req.family.
        hv2::TranslatePoolEntry* e =
            mgr.TranslatePoolEnsure("user-MiLMMT-46-4B-v1.0.Q5_K_M_3");
        TEST_CHECK(e != nullptr, "prop: pool entry registered for the user model");
        if (e) {
            hv2::WorkerHandle* h = mgr.EnsureSpawned(e->family);
            TEST_CHECK(h == nullptr, "prop: failed fake spawn -> nullptr (pool leg)");
            bool found = false;
            for (const std::wstring& f : ctx.seen_families) {
                if (f == e->family) found = true;
            }
            TEST_CHECK(found, "prop: pool family reaches the launcher verbatim");
        }
    }

    std::cout << "[DONE] SpawnRequest.family propagation (failures: "
              << (g_failed_count - failures_before) << ")" << std::endl;
}

// ---- structural pin: LaunchWorkerProcess forwards --mutex ------------------

void TestLaunchWorkerProcessMutexArgPin() {
    std::cout << "[RUN] Testing LaunchWorkerProcess --mutex structural pin..." << std::endl;
    const int failures_before = g_failed_count;

    // Structural source-text pin (the established source-scan pattern): the
    // real launcher must append the --mutex family arg next to --pipe/--token.
    // A behavioral pin cannot drive the REAL CreateProcessW here (no worker
    // exe to spawn), so the propagation itself is pinned behaviorally by
    // TestSpawnRequestFamilyPropagation above; this pin guards the one line
    // that turns req.family into the child command-line arg.
    const std::filesystem::path src = ResolveRepoFile(L"src\\host_v2_worker_manager.cpp");
    if (src.empty()) {
        TEST_CHECK(false, "launch pin: host_v2_worker_manager.cpp must resolve");
    } else {
        std::ifstream in(src, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string body = ss.str();
        TEST_CHECK(body.find("--mutex ") != std::string::npos,
                   "launch pin: LaunchWorkerProcess appends --mutex to the child cmdline");
        TEST_CHECK(body.find("req.family") != std::string::npos,
                   "launch pin: the cmdline reads req.family");
    }

    // Structural pin on the worker side: wWinMain derives the mutex name from
    // the parsed family BEFORE CreateMutexW (the parse must precede the gate).
    const std::filesystem::path wsrc = ResolveRepoFile(L"src\\ggml_translate_worker.cpp");
    if (wsrc.empty()) {
        TEST_CHECK(false, "launch pin: ggml_translate_worker.cpp must resolve");
    } else {
        std::ifstream win(wsrc, std::ios::binary);
        std::stringstream wss;
        wss << win.rdbuf();
        const std::string wbody = wss.str();
        const size_t parse_pos = wbody.find("ParseArgs(pCmdLine)");
        const size_t mutex_pos = wbody.find("CreateMutexW");
        TEST_CHECK(parse_pos != std::string::npos && mutex_pos != std::string::npos,
                   "launch pin: worker parses args and creates the mutex");
        TEST_CHECK(parse_pos < mutex_pos,
                   "launch pin: args parse PRECEDES the mutex gate (family-derived name)");
        TEST_CHECK(wbody.find("MutexNameForFamily") != std::string::npos,
                   "launch pin: worker derives the name via wp::MutexNameForFamily");
    }

    std::cout << "[DONE] LaunchWorkerProcess --mutex structural pin (failures: "
              << (g_failed_count - failures_before) << ")" << std::endl;
}

} // namespace mf_pin
