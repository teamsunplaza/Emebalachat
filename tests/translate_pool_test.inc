// Plan-B (REQ-B003) B-T1 + (REQ-B004) B-T3: per-model translate pool machinery
// pins + VRAM-tracker / spawn-gate integration pins.
// Included directly by tests/run_tests.cpp (the staged session .inc contract;
// uses the TEST_CHECK harness + the std:: facilities included there).
//
//   * PoolFamilyForModel (§3.3) derivation pins — empty / widen / fail-closed.
//   * TranslatePoolEnsure (§3.4) — boot "" entry reuse, no duplicate
//     registration, on-demand runtime family registration.
//   * TranslatePoolNeedsRelay / MarkRelayed (§3.5) — byte-identical relay
//     semantics to the removed per-loop locals (REQ-055/REQ-058 snapshots).
//   * A-1 (technical gate): deque pointer stability under runtime push_back.
//   * A-3 (technical gate): boot translate exe path recorded + retrievable.
//
// B-T3 (REQ-B004) adds:
//   * §4.3 reserved-bytes tracker: increment on a successful translate spawn,
//     decrement on the reaper-detected exit; per-entry contribution exact.
//   * TestAsrVramNotInTranslatePool (§4.5 pin): ASR never counted.
//   * The v2 §A2 gate folded into TranslatePoolEnsure: allow_gpu / !allow_gpu
//     (EMEBALA_MT_GPU=0 env leg, A-4) / probe-failure fail-closed; the gate
//     runs ONLY for a NEW entry (existing-entry fast path = no re-run).
//   * The §4.3-step-2 conservative default (2048 MiB) for an unknown model.
//   * BuildTranslateSpawnEnvironment (A-4): parent-env copy + overwrite,
//     double-null-terminated, testable without CreateProcess.
//
// B-T1 scope: the pool is present but UNUSED by the dispatchers (still
// hardcoded family); these pins prove the machinery is correct in isolation.

namespace translate_pool_test_detail {

// Shared context for the controlled fake-launcher handshake pins (B-T3). The
// spawn seam is a plain function pointer (no captures), so the mutable state
// lives here and is passed via the WorkerManager `user` pointer.
struct FakeHandshakeCtx {
    HANDLE exit_ev = nullptr;     // tracked process handle (manual-reset event)
    bool forbid_gpu_override = false; // when true, a GPU override block fails the spawn
    std::wstring pipe_name;
    std::string token;
    std::thread hs_thread;
};

// Connects the server pipe and sends a VALID announce (wp::BuildAnnounce +
// EmbeddedWorkerManifest), so EnsureSpawned reaches Ready through the genuine
// connect+announce path — on a worker THREAD (async), so the overlapped server
// reads proceed without a race (the m6 settle Fixture pattern).
inline void FakeHandshakeWorkerThread(FakeHandshakeCtx* ctx) {
    namespace wp = emebalachat::workerproto;
    HANDLE c = nullptr;
    for (int i = 0; i < 100 && !c; ++i) {
        c = ::CreateFileW(ctx->pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                          nullptr, OPEN_EXISTING, 0, nullptr);
        if (c == INVALID_HANDLE_VALUE) { c = nullptr; ::Sleep(25); }
    }
    if (!c) return;
    DWORD m = PIPE_READMODE_MESSAGE;
    ::SetNamedPipeHandleState(c, &m, nullptr, nullptr);
    const std::string ann = wp::BuildAnnounce(wp::EmbeddedWorkerManifest(), ctx->token);
    std::string frame;
    const uint32_t len = static_cast<uint32_t>(ann.size());
    for (unsigned i = 0; i < 4; ++i) frame.push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
    frame.append(ann);
    DWORD written = 0;
    ::WriteFile(c, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr);
    ::FlushFileBuffers(c);
    ::CloseHandle(c); // server holds its end; announce already read
}

// Non-capturing spawn trampoline (function-pointer compatible): records the
// pipe/token, returns the ctx's exit event as the tracked process handle, and
// kicks the handshake thread.
inline emebalachat::host_v2::SpawnResult FakeHandshakeSpawn(
    const emebalachat::host_v2::SpawnRequest& req, void* user) {
    auto* ctx = static_cast<FakeHandshakeCtx*>(user);
    if (ctx->forbid_gpu_override && !req.environment_block.empty()) {
        const std::wstring env(req.environment_block.begin(), req.environment_block.end());
        if (env.find(L"EMEBALA_MT_GPU=0") != std::wstring::npos) {
            return emebalachat::host_v2::SpawnResult{}; // a GPU leg must NOT override
        }
    }
    ctx->pipe_name = req.pipe_name;
    ctx->token = req.token;
    emebalachat::host_v2::SpawnResult r;
    r.ok = true;
    r.process = ctx->exit_ev;
    r.done_event = ctx->exit_ev;
    ctx->hs_thread = std::thread(FakeHandshakeWorkerThread, ctx);
    return r;
}

} // namespace translate_pool_test_detail

void TestTranslatePool() {
    std::cout << "[RUN] Testing translate pool machinery (Plan-B B-T1)..." << std::endl;
    const int failures_before = g_failed_count;
    namespace hv2 = emebalachat::host_v2;
    namespace tp = emebalachat::host_v2::translate_pool;

    const wchar_t* kTranslateFamily = L"ggml-translate";
    const unsigned long long kMiB = 1024ull * 1024;
    wchar_t self[MAX_PATH] = {0};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH); // a guaranteed-present exe path

    // ---- §3.3 PoolFamilyForModel derivation pins -----------------------------
    {
        TEST_CHECK(tp::PoolFamilyForModel("") == std::wstring(kTranslateFamily),
                   "pool: PoolFamilyForModel(\"\") == ggml-translate");
        TEST_CHECK(tp::PoolFamilyForModel("user-X") == std::wstring(L"ggml-translate-user-X"),
                   "pool: PoolFamilyForModel(\"user-X\") == ggml-translate-user-X");
        TEST_CHECK(tp::PoolFamilyForModel("hy-mt2-1.8B-Q8_0") ==
                       std::wstring(L"ggml-translate-hy-mt2-1.8B-Q8_0"),
                   "pool: real registry id widens + dash-joins");
        TEST_CHECK(tp::PoolFamilyForModel("MiLM-6-4B-Q8_0.gguf") ==
                       std::wstring(L"ggml-translate-MiLM-6-4B-Q8_0.gguf"),
                   "pool: dot/underscore/dash allowed");
        TEST_CHECK(tp::PoolFamilyForModel("bad;id").empty(),
                   "pool: PoolFamilyForModel(\"bad;id\") == empty (fail-closed)");
        TEST_CHECK(tp::PoolFamilyForModel("bad id").empty(),
                   "pool: space rejected");
        TEST_CHECK(tp::PoolFamilyForModel("bad/id").empty(),
                   "pool: path separator rejected");
    }

    // ---- §3.4 Ensure: boot "" entry, no duplicate, on-demand register --------
    {
        hv2::WorkerManager mgr([](const hv2::SpawnRequest&, void*) { return hv2::SpawnResult{}; },
                               [](HANDLE, void*) {}, nullptr);
        // A-3: record the boot translate exe path (host_main.cpp main() scope)
        // and confirm it is retrievable for reuse by runtime registrations.
        TEST_CHECK(mgr.SetTranslateExePath(self), "pool: SetTranslateExePath records boot path");
        TEST_CHECK(mgr.TranslateExePath() == std::wstring(self),
                   "pool: A-3 translate exe path retrievable verbatim");

        // Boot registration of the "" entry (references the SAME
        // kWorkerFamilyTranslate family registered at boot — no new
        // RegisterFamily is issued for it).
        mgr.RegisterFamily(kTranslateFamily, self); // the boot translate registration
        TEST_CHECK(mgr.RegisterTranslatePoolBootEntry(), "pool: boot \"\" entry registered");
        TEST_CHECK(!mgr.RegisterTranslatePoolBootEntry(), "pool: boot registration idempotent");

        hv2::TranslatePoolEntry* e = mgr.TranslatePoolEnsure("");
        TEST_CHECK(e != nullptr, "pool: Ensure(\"\") returns the boot entry");
        TEST_CHECK(e->family == std::wstring(kTranslateFamily),
                   "pool: boot entry family == kWorkerFamilyTranslate");
        TEST_CHECK(e->model_id.empty(), "pool: boot entry model_id empty");
        TEST_CHECK(e->registered, "pool: boot entry marked registered");
        TEST_CHECK(mgr.TranslatePoolSize() == 1, "pool: size == 1 after boot entry");
        TEST_CHECK(mgr.TranslatePoolEnsure("") == e, "pool: second Ensure(\"\") returns SAME entry");

        // On-demand: a new model_id registers a NEW family with the SAME exe path.
        hv2::TranslatePoolEntry* u = mgr.TranslatePoolEnsure("user-X");
        TEST_CHECK(u != nullptr, "pool: Ensure(\"user-X\") creates entry");
        TEST_CHECK(u->family == std::wstring(L"ggml-translate-user-X"),
                   "pool: on-demand family widened + dash-joined");
        TEST_CHECK(u->registered, "pool: on-demand entry marked registered");
        TEST_CHECK(mgr.TranslatePoolSize() == 2, "pool: size == 2 after on-demand entry");
        TEST_CHECK(mgr.State(L"ggml-translate-user-X") == hv2::WorkerState::Stopped,
                   "pool: on-demand family registered (Stopped until first spawn)");
        hv2::WorkerHandle* uh = mgr.Find(L"ggml-translate-user-X");
        TEST_CHECK(uh != nullptr && uh->exe_path == std::wstring(self),
                   "pool: A-3 on-demand family reuses the SAME boot exe path");

        // Invalid id chars fail-closed (no entry created, no registration).
        TEST_CHECK(mgr.TranslatePoolEnsure("bad;id") == nullptr,
                   "pool: invalid id -> nullptr (model_missing path)");
        TEST_CHECK(mgr.TranslatePoolSize() == 2, "pool: invalid id adds no entry");

        // Idempotency of Ensure for the same model_id.
        TEST_CHECK(mgr.TranslatePoolEnsure("user-X") == u,
                   "pool: repeated Ensure(\"user-X\") returns SAME entry");
    }

    // Ensure with no recorded exe path fails-closed for non-empty ids.
    {
        hv2::WorkerManager mgr([](const hv2::SpawnRequest&, void*) { return hv2::SpawnResult{}; },
                               [](HANDLE, void*) {}, nullptr);
        mgr.RegisterFamily(kTranslateFamily, self);
        mgr.RegisterTranslatePoolBootEntry();
        TEST_CHECK(mgr.TranslatePoolEnsure("") != nullptr, "pool: boot entry available");
        TEST_CHECK(mgr.TranslatePoolEnsure("user-Y") == nullptr,
                   "pool: no translate exe path -> on-demand Ensure fails-closed");
        TEST_CHECK(mgr.TranslatePoolSize() == 1, "pool: failed on-demand adds no entry");
    }

    // ---- §3.5 relay snapshot semantics (byte-identical to removed locals) -----
    {
        hv2::WorkerManager mgr([](const hv2::SpawnRequest&, void*) { return hv2::SpawnResult{}; },
                               [](HANDLE, void*) {}, nullptr);
        mgr.SetTranslateExePath(self);
        mgr.RegisterFamily(kTranslateFamily, self);
        mgr.RegisterTranslatePoolBootEntry();
        mgr.TranslatePoolEnsure("");

        // Initial snapshot: relayed_model_id="" vs pin="user-X" -> true.
        TEST_CHECK(mgr.TranslatePoolNeedsRelay("user-X", nullptr),
                   "pool: initial snapshot (different id) needs relay");
        // Same id, initial nullptr process vs live process -> true (REQ-058).
        mgr.TranslatePoolMarkRelayed("", reinterpret_cast<HANDLE>(1));
        TEST_CHECK(!mgr.TranslatePoolNeedsRelay("", reinterpret_cast<HANDLE>(1)),
                   "pool: same id + same process -> no relay");
        TEST_CHECK(mgr.TranslatePoolNeedsRelay("", reinterpret_cast<HANDLE>(2)),
                   "pool: process change (worker respawn) -> needs relay (REQ-058)");
        TEST_CHECK(!mgr.TranslatePoolNeedsRelay("", reinterpret_cast<HANDLE>(1)),
                   "pool: back to the prior recorded process -> snapshot already matches -> no relay");
        // Model change on the same entry -> relay.
        mgr.TranslatePoolMarkRelayed("", reinterpret_cast<HANDLE>(2));
        TEST_CHECK(mgr.TranslatePoolNeedsRelay("hy-mt2", reinterpret_cast<HANDLE>(2)),
                   "pool: model_id change -> needs relay (REQ-055)");
        // Unknown entry -> needs relay (fail-closed toward re-relay).
        TEST_CHECK(mgr.TranslatePoolNeedsRelay("never-registered", nullptr),
                   "pool: unknown entry needs relay (fail-closed)");
        // MarkRelayed on a nonexistent entry is a harmless no-op.
        mgr.TranslatePoolMarkRelayed("never-registered", nullptr);
    }

    // ---- A-1 (technical gate): deque pointer stability under runtime push ----
    {
        hv2::WorkerManager mgr([](const hv2::SpawnRequest&, void*) { return hv2::SpawnResult{}; },
                               [](HANDLE, void*) {}, nullptr);
        mgr.RegisterFamily(L"fam-a", self);
        hv2::WorkerHandle* a = mgr.Find(L"fam-a");
        TEST_CHECK(a != nullptr, "A-1: baseline family A present");
        // Register many more families (runtime push_back): with the old
        // std::vector the deque/vector would reallocate and `a` would dangle.
        for (int i = 0; i < 64; ++i) {
            mgr.RegisterFamily(L"fam-b-" + std::to_wstring(i), self);
        }
        TEST_CHECK(mgr.Find(L"fam-a") == a,
                   "A-1: held WorkerHandle* stays valid + same identity after many push_backs");
        TEST_CHECK(a->family == L"fam-a", "A-1: stable pointer retains family identity");
    }

    // ---- B-T3 A-4: BuildTranslateSpawnEnvironment (pure, no CreateProcess) ---
    {
        // Empty assignments -> empty block -> the caller passes
        // lpEnvironment=nullptr (inherit verbatim; the unchanged GPU path).
        std::vector<wchar_t> block;
        TEST_CHECK(hv2::BuildTranslateSpawnEnvironment({}, block) && block.empty(),
                   "A-4: empty assignments -> empty block -> inherit (GPU leg)");

        // One assignment -> a double-null-terminated block that carries the
        // assignment exactly once (parent copy + overwrite).
        TEST_CHECK(hv2::BuildTranslateSpawnEnvironment({{L"EMEBALA_MT_GPU", L"0"}}, block) &&
                       !block.empty(),
                   "A-4: CPU-leg assignment builds a block");
        if (!block.empty()) {
            // Must end with the double-null terminator.
            TEST_CHECK(block.size() >= 2 && block[block.size() - 1] == L'\0' &&
                           block[block.size() - 2] == L'\0',
                       "A-4: block is double-null terminated");
            size_t occurrences = 0;
            size_t i = 0;
            while (i < block.size() && block[i] != L'\0') {
                size_t j = i;
                while (block[j] != L'\0' && block[j] != L'=') ++j;
                if (block[j] == L'=') {
                    const std::wstring name(block.begin() + i, block.begin() + j);
                    if (name == L"EMEBALA_MT_GPU") ++occurrences;
                }
                while (block[i] != L'\0') ++i;
                ++i;
            }
            TEST_CHECK(occurrences == 1, "A-4: EMEBALA_MT_GPU appears exactly once");
            const std::wstring joined(block.begin(), block.end());
            TEST_CHECK(joined.find(L"EMEBALA_MT_GPU=0") != std::wstring::npos,
                       "A-4: block carries EMEBALA_MT_GPU=0 verbatim");
        }

        // Overwrite semantics: a pre-set parent value is replaced, not
        // duplicated (the child sees exactly one occurrence of the name).
        BOOL ok = ::SetEnvironmentVariableW(L"EMEBALA_ZZ_BT3", L"parent-value");
        TEST_CHECK(ok != FALSE, "A-4: seed a parent env value for the overwrite pin");
        std::vector<wchar_t> ovr;
        TEST_CHECK(hv2::BuildTranslateSpawnEnvironment({{L"EMEBALA_ZZ_BT3", L"0"}}, ovr) &&
                       !ovr.empty(),
                   "A-4: overwrite assignment builds a block");
        if (!ovr.empty()) {
            const std::wstring joined(ovr.begin(), ovr.end());
            TEST_CHECK(joined.find(L"EMEBALA_ZZ_BT3=0") != std::wstring::npos,
                       "A-4: the assignment overwrites the parent value");
            TEST_CHECK(joined.find(L"EMEBALA_ZZ_BT3=parent-value") == std::wstring::npos,
                       "A-4: the stale parent value is gone");
        }
        ::SetEnvironmentVariableW(L"EMEBALA_ZZ_BT3", nullptr); // cleanup
    }

    // ---- B-T3 §4.3: tracker increment on a successful GPU-leg spawn ---------
    // A worker-THREAD completes the real connect+announce handshake (the m6
    // settle Fixture pattern), so EnsureSpawned reaches Ready through the
    // genuine code path — no self-spawn (run_tests.exe is the test EXE, not a
    // worker), no Vulkan, no session-global single-instance mutex. The tracked
    // process handle is a real event that fires on command (drives the exit).
    {
        namespace detail = translate_pool_test_detail;
        detail::FakeHandshakeCtx ctx;
        ctx.exit_ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ctx.forbid_gpu_override = true; // a GPU leg must NOT carry an override block
        TEST_CHECK(ctx.exit_ev != nullptr, "tracker: exit event created");
        hv2::WorkerManager mgr(&detail::FakeHandshakeSpawn, [](HANDLE, void*) {}, &ctx);
        mgr.SetTranslateExePath(self);
        mgr.RegisterFamily(kTranslateFamily, self); // registration placeholder (never spawned)
        mgr.RegisterTranslatePoolBootEntry();
        mgr.SetVramBytesResolver([](const std::string&) { return 700ull * kMiB; });
        mgr.SetFreeVramProbe([](unsigned long long& out) { out = 8ull * 1024 * kMiB; return true; });

        TEST_CHECK(mgr.TranslatePoolReservedVramBytes() == 0,
                   "tracker: 0 before any spawn");
        hv2::WorkerHandle* w = mgr.EnsureSpawned(kTranslateFamily);
        if (ctx.hs_thread.joinable()) ctx.hs_thread.join();
        TEST_CHECK(w != nullptr && w->state == hv2::WorkerState::Ready,
                   "tracker: spawn reached Ready (valid announce via the worker thread)");
        TEST_CHECK(mgr.TranslatePoolReservedVramBytes() == 700ull * kMiB,
                   "tracker: +resolver model bytes once the worker is Ready");
        TEST_CHECK(mgr.Find(kTranslateFamily) == w,
                   "tracker: handle stays valid after the tracker update");

        // The exit path: signal the child-exit event, the reaper observes it
        // and subtracts the contribution (the reaper/close path).
        ::SetEvent(ctx.exit_ev);
        TEST_CHECK(mgr.ReaperPass(0) == 1, "tracker: the signaled exit is finalized");
        TEST_CHECK(mgr.TranslatePoolReservedVramBytes() == 0,
                   "tracker: the exit releases the contribution (reaper path)");
        ::CloseHandle(ctx.exit_ev);
    }

    // ---- B-T3 §4.4/v2 §A2: the gate runs at Ensure; the entry carries the leg -
    {
        hv2::WorkerManager mgr([](const hv2::SpawnRequest&, void*) { return hv2::SpawnResult{}; },
                               [](HANDLE, void*) {}, nullptr);
        mgr.SetTranslateExePath(self);
        mgr.RegisterFamily(kTranslateFamily, self);
        mgr.RegisterTranslatePoolBootEntry();
        mgr.SetVramBytesResolver([](const std::string&) { return 400ull * kMiB; });
        mgr.SetFreeVramProbe([](unsigned long long& out) { out = 8ull * 1024 * kMiB; return true; });

        hv2::TranslatePoolEntry* e = mgr.TranslatePoolEnsure("user-X");
        TEST_CHECK(e != nullptr, "gate: ample VRAM -> entry registered");
        const hv2::WorkerManager::VramGateSnapshot g = mgr.TranslatePoolLastGateDecision();
        TEST_CHECK(std::string(g.reason) == "ok" && g.allow_gpu && g.allow_cpu,
                   "gate: ample VRAM -> allow_gpu + reason ok");
        TEST_CHECK(g.model_vram_bytes == 400ull * kMiB && g.query_ok,
                   "gate: model from the injected resolver; probe ok");
        TEST_CHECK(e->reserved_vram_bytes == 0 && !e->spawn_gpu_env_override,
                   "gate: GPU leg -> no env override, nothing counted until spawn");
    }

    // ---- B-T3 §4.4 + A-4: !allow_gpu -> CPU leg carries EMEBALA_MT_GPU=0 ------
    {
        // The spawn seam is a plain function pointer: a capturing lambda
        // cannot convert. The counts live in a static block-local struct the
        // non-capturing trampoline forwards to (scoped to this pin).
        struct CpuLegSpawnProbe {
            int spawns = 0;
            bool saw_override = false;
        };
        static CpuLegSpawnProbe cpu_probe;
        cpu_probe = CpuLegSpawnProbe{};
        hv2::WorkerManager mgr(
            [](const hv2::SpawnRequest& req, void*) -> hv2::SpawnResult {
                ++cpu_probe.spawns;
                // The CPU-leg spawn must carry a block whose single occurrence
                // of EMEBALA_MT_GPU reads "0" (inherit would be WRONG here: a
                // parent EMEBALA_MT_GPU=1 would override the host decision).
                if (!req.environment_block.empty()) {
                    const std::wstring env(req.environment_block.begin(),
                                           req.environment_block.end());
                    size_t pos = env.find(L"EMEBALA_MT_GPU=0" L"\0");
                    if (pos != std::wstring::npos &&
                        env.find(L"EMEBALA_MT_GPU=0", pos + 1) == std::wstring::npos) {
                        cpu_probe.saw_override = true;
                    }
                }
                return hv2::SpawnResult{};
            },
            [](HANDLE, void*) {}, nullptr);
        const int& spawns = cpu_probe.spawns;
        const bool& saw_override = cpu_probe.saw_override;
        mgr.SetTranslateExePath(self);
        mgr.RegisterFamily(kTranslateFamily, self);
        mgr.RegisterTranslatePoolBootEntry();
        mgr.SetVramBytesResolver([](const std::string&) { return 4ull * 1024 * kMiB; });
        // 5 GiB free - 4 GiB model = 1 GiB < 1.5 GiB floor -> gpu_ok false.
        mgr.SetFreeVramProbe([](unsigned long long& out) { out = 5ull * 1024 * kMiB; return true; });

        hv2::TranslatePoolEntry* e = mgr.TranslatePoolEnsure("user-Y");
        TEST_CHECK(e != nullptr, "gate: CPU leg still registers the entry");
        const hv2::WorkerManager::VramGateSnapshot g = mgr.TranslatePoolLastGateDecision();
        TEST_CHECK(std::string(g.reason) == "below_threshold_gpu" && !g.allow_gpu && g.allow_cpu,
                   "gate: 1 GiB headroom < 1.5 GiB floor -> !allow_gpu, CPU ok (v2 §A2)");
        TEST_CHECK(e->spawn_gpu_env_override,
                   "gate: CPU-leg entry flagged for EMEBALA_MT_GPU=0");
        TEST_CHECK(mgr.TranslatePoolSize() == 2,
                   "gate: CPU leg still registers (pool grows)");
        // The fake launcher fails -> no Ready -> nothing counted, no crash.
        (void)mgr.EnsureSpawned(e->family);
        TEST_CHECK(spawns == 1 && saw_override,
                   "A-4: CPU-leg spawn carries EMEBALA_MT_GPU=0 exactly once");
        TEST_CHECK(mgr.TranslatePoolReservedVramBytes() == 0,
                   "gate: a failed spawn never touches the tracker");
    }

    // ---- B-T3 v2 §A2 pin 4: probe failure -> fail-closed CPU leg --------------
    {
        hv2::WorkerManager mgr([](const hv2::SpawnRequest&, void*) { return hv2::SpawnResult{}; },
                               [](HANDLE, void*) {}, nullptr);
        mgr.SetTranslateExePath(self);
        mgr.RegisterFamily(kTranslateFamily, self);
        mgr.RegisterTranslatePoolBootEntry();
        mgr.SetVramBytesResolver([](const std::string&) { return 100ull * kMiB; });
        mgr.SetFreeVramProbe([](unsigned long long&) { return false; }); // probe fails

        hv2::TranslatePoolEntry* e = mgr.TranslatePoolEnsure("user-Z");
        TEST_CHECK(e != nullptr, "probe-fail: CPU leg still registers the entry");
        const hv2::WorkerManager::VramGateSnapshot g = mgr.TranslatePoolLastGateDecision();
        TEST_CHECK(!g.query_ok && std::string(g.reason) == "query_failed" && !g.allow_gpu && g.allow_cpu,
                   "probe-fail: query_ok=false -> fail-closed (never GPU), CPU ok (v2 §A2)");
        TEST_CHECK(e->spawn_gpu_env_override,
                   "probe-fail: CPU-leg entry flagged for EMEBALA_MT_GPU=0");
    }

    // ---- B-T3 §4.3 step 2: unknown model -> the 2048 MiB conservative default -
    {
        hv2::WorkerManager mgr([](const hv2::SpawnRequest&, void*) { return hv2::SpawnResult{}; },
                               [](HANDLE, void*) {}, nullptr);
        mgr.SetTranslateExePath(self);
        mgr.RegisterFamily(kTranslateFamily, self);
        mgr.RegisterTranslatePoolBootEntry();
        mgr.SetVramBytesResolver([](const std::string&) { return 0ull; }); // absent
        mgr.SetFreeVramProbe([](unsigned long long& out) { out = 12ull * 1024 * kMiB; return true; });

        hv2::TranslatePoolEntry* e = mgr.TranslatePoolEnsure("user-unknown");
        TEST_CHECK(e != nullptr, "default: unknown model still registers");
        const hv2::WorkerManager::VramGateSnapshot g = mgr.TranslatePoolLastGateDecision();
        TEST_CHECK(g.model_vram_bytes == 2048ull * kMiB && g.allow_gpu,
                   "default: no-registry-entry -> 2048 MiB charge, GPU leg ok");
    }

    // ---- B-T3: existing-entry fast path -> NO gate re-run ---------------------
    {
        int probe_calls = 0;
        hv2::WorkerManager mgr([](const hv2::SpawnRequest&, void*) { return hv2::SpawnResult{}; },
                               [](HANDLE, void*) {}, nullptr);
        mgr.SetTranslateExePath(self);
        mgr.RegisterFamily(kTranslateFamily, self);
        mgr.RegisterTranslatePoolBootEntry();
        mgr.SetVramBytesResolver([](const std::string&) { return 100ull * kMiB; });
        mgr.SetFreeVramProbe([&](unsigned long long& out) {
            ++probe_calls;
            out = 8ull * 1024 * kMiB;
            return true;
        });

        hv2::TranslatePoolEntry* e1 = mgr.TranslatePoolEnsure("user-X");
        TEST_CHECK(e1 != nullptr && probe_calls == 1,
                   "fast-path: first Ensure ran the gate once");
        hv2::TranslatePoolEntry* e2 = mgr.TranslatePoolEnsure("user-X");
        TEST_CHECK(e2 == e1 && probe_calls == 1 && mgr.TranslatePoolSize() == 2,
                   "fast-path: second Ensure returns SAME entry, gate NOT re-run");
    }

    // ---- B-T3 §4.3/§12.4: reaper exit subtracts EXACTLY the contribution ------
    // Controlled fake launcher reaches Ready (real connect+announce); the child
    // is a REAL live process (this process's pseudo-handle is never signaled,
    // so to drive a GENUINE reaper-detected exit we hand the reaper a real
    // terminating child via TerminateProcess on a process WE spawn). Simplest:
    // spawn a real short-lived child as the stand-in process handle, let the
    // fake launcher return THAT handle, then it exits on its own -> the reaper
    // observes the termination and finalizes the crash.
    {
        // Worker-thread handshake reaches Ready; the child is a real event the
        // reaper's done_event poll observes once signaled.
        namespace detail = translate_pool_test_detail;
        detail::FakeHandshakeCtx ctx;
        ctx.exit_ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        TEST_CHECK(ctx.exit_ev != nullptr, "reaper: exit event created");
        hv2::WorkerManager mgr(&detail::FakeHandshakeSpawn, [](HANDLE, void*) {}, &ctx);
        mgr.SetTranslateExePath(self);
        mgr.RegisterFamily(kTranslateFamily, self);
        mgr.RegisterTranslatePoolBootEntry();
        mgr.SetVramBytesResolver([](const std::string&) { return 700ull * kMiB; });
        mgr.SetFreeVramProbe([](unsigned long long& out) { out = 8ull * 1024 * kMiB; return true; });
        hv2::WorkerHandle* w = mgr.EnsureSpawned(kTranslateFamily);
        if (ctx.hs_thread.joinable()) ctx.hs_thread.join();
        TEST_CHECK(w != nullptr && w->state == hv2::WorkerState::Ready,
                   "reaper: spawn reached Ready (real connect+announce)");
        TEST_CHECK(mgr.TranslatePoolReservedVramBytes() == 700ull * kMiB,
                   "reaper: tracker charged after Ready");

        // Signal the exit; the reaper's done_event poll observes it and
        // finalizes the crash — the subtract runs HERE.
        ::SetEvent(ctx.exit_ev);
        TEST_CHECK(mgr.ReaperPass(0) == 1, "reaper: the done_event poll observes the exit");
        TEST_CHECK(mgr.TranslatePoolReservedVramBytes() == 0,
                   "reaper: tracker decremented by the entry's exact bytes");
        TEST_CHECK(mgr.ReaperPass(0) == 0 && mgr.TranslatePoolReservedVramBytes() == 0,
                   "reaper: a second pass never subtracts below zero (exact-once)");
        ::CloseHandle(ctx.exit_ev);
    }

    // ---- B-T3 §4.5 pin: ASR VRAM is NEVER in the translate tracker ------------
    {
        // Translate side: worker-thread handshake reaches Ready (real
        // connect+announce); the tracked process handle is a live event.
        namespace detail = translate_pool_test_detail;
        detail::FakeHandshakeCtx ctx;
        ctx.exit_ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        TEST_CHECK(ctx.exit_ev != nullptr, "ASR pin: exit event created");
        hv2::WorkerManager mgr(&detail::FakeHandshakeSpawn, [](HANDLE, void*) {}, &ctx);
        mgr.SetTranslateExePath(self);
        mgr.RegisterFamily(kTranslateFamily, self);
        mgr.RegisterTranslatePoolBootEntry();
        mgr.SetVramBytesResolver([](const std::string&) { return 700ull * kMiB; });
        mgr.SetFreeVramProbe([](unsigned long long& out) { out = 8ull * 1024 * kMiB; return true; });
        hv2::WorkerHandle* tw = mgr.EnsureSpawned(kTranslateFamily);
        if (ctx.hs_thread.joinable()) ctx.hs_thread.join();
        TEST_CHECK(tw != nullptr && tw->state == hv2::WorkerState::Ready,
                   "ASR pin: translate spawn reached Ready");
        const unsigned long long after_translate = mgr.TranslatePoolReservedVramBytes();
        TEST_CHECK(after_translate == 700ull * kMiB,
                   "ASR pin: tracker holds exactly the translate contribution");

        // A running ASR family (never in the pool map): a crash subtracts
        // NOTHING — the ASR share is protected by never being added (§4.5).
        // The ASR stand-in is a REAL live process (ping) the reaper watches;
        // its exit must not move the translate tracker.
        mgr.RegisterFamily(L"ggml-asr", self); // registered, never spawned
        hv2::WorkerHandle* aw = mgr.Find(L"ggml-asr");
        TEST_CHECK(aw != nullptr, "ASR pin: fake ASR family registered");
        wchar_t sys[MAX_PATH] = {0};
        ::GetSystemDirectoryW(sys, MAX_PATH);
        const std::wstring ping = std::wstring(sys) + L"\\ping.exe";
        std::wstring ping_cmd = L"\"" + ping + L"\" -t 127.0.0.1 > NUL";
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        TEST_CHECK(::CreateProcessW(ping.c_str(), ping_cmd.data(), nullptr, nullptr, FALSE,
                                    CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != FALSE,
                   "ASR pin: spawn a real ASR stand-in process");
        ::CloseHandle(pi.hThread);
        aw->state = hv2::WorkerState::Ready;
        aw->process = pi.hProcess; // a REAL live child the reaper watches
        TEST_CHECK(mgr.ReaperPass(0) == 0,
                   "ASR pin: live ASR handle -> nothing reaped yet");
        TEST_CHECK(mgr.TranslatePoolReservedVramBytes() == after_translate,
                   "ASR pin: tracker unchanged while ASR runs");
        TEST_CHECK(::TerminateProcess(aw->process, 1) != FALSE,
                   "ASR pin: terminate the ASR stand-in");
        // The exit may not be observable on the very first poll (TerminateProcess
        // is async); the production reaper runs on a 250 ms tick, so poll (bounded).
        int asr_crashes = 0;
        for (int i = 0; i < 200 && asr_crashes == 0; ++i) {
            asr_crashes = mgr.ReaperPass(0);
            if (asr_crashes == 0) ::Sleep(10);
        }
        TEST_CHECK(asr_crashes == 1, "ASR pin: the ASR crash is finalized");
        TEST_CHECK(mgr.TranslatePoolReservedVramBytes() == after_translate,
                   "ASR pin: ASR exit subtracts NOTHING (ASR never counted, §4.5)");
        // Release the translate worker (its process handle is our live event;
        // the pipe client closed -> EOF, so the graceful stop is immediate).
        mgr.GracefulStop(*tw, 2000);
        ::CloseHandle(ctx.exit_ev);
        (void)mgr.TranslatePoolReservedVramBytes();
    }

    // ---- P5a.5 C2 (195700 F-2): CPU-leg env block SURVIVES a crash-respawn ---
    // A CPU-leg entry (gate !allow_gpu -> spawn_gpu_env_override) crashes; the
    // backoff-respawned spawn must STILL carry EMEBALA_MT_GPU=0. The old code
    // gated the block on `state == Stopped`, so the Crashed->respawn path
    // silently dropped the override and the respawned worker could take the GPU
    // leg (REQ-B004 honesty break). Drives the genuine crash->backoff->respawn
    // path: reach Ready, signal the exit event, the reaper finalizes the crash
    // (state Crashed), then EnsureSpawned respawns past the 1s backoff. The
    // handshake thread restarts automatically (FakeHandshakeSpawn kicks it per
    // spawn), so the respawn reaches Ready too. spawn#2 records the env block.
    {
        namespace detail = translate_pool_test_detail;
        struct RespawnEnvCtx {
            detail::FakeHandshakeCtx* hs;
            int spawns = 0;
            bool second_spawn_override = false; // spawn#2's EMEBALA_MT_GPU=0 presence
        };
        static RespawnEnvCtx re_ctx;
        detail::FakeHandshakeCtx ctx;
        ctx.exit_ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        TEST_CHECK(ctx.exit_ev != nullptr, "C2 respawn-env: exit event created");
        re_ctx = RespawnEnvCtx{&ctx, 0, false};
        hv2::WorkerManager mgr(
            [](const hv2::SpawnRequest& req, void* user) -> hv2::SpawnResult {
                auto* c = static_cast<RespawnEnvCtx*>(user);
                ++c->spawns;
                if (c->spawns == 2 && !req.environment_block.empty()) {
                    const std::wstring env(req.environment_block.begin(),
                                           req.environment_block.end());
                    // Exactly one EMEBALA_MT_GPU=0 ENTRY-BOUNDARY occurrence.
                    // The old needle `L"EMEBALA_MT_GPU=0" L"\0"` lost its
                    // embedded NUL in the implicit wstring conversion (the
                    // literal decays to a C-string), degrading the intended
                    // "=0 then NUL" match to a plain substring search. Build
                    // the real needle and count entry-boundary occurrences in
                    // the raw double-null-terminated block.
                    const std::wstring needle =
                        std::wstring(L"EMEBALA_MT_GPU=0") + L'\0';
                    size_t gpu0_entries = 0;
                    for (size_t pos = env.find(needle); pos != std::wstring::npos;
                         pos = env.find(needle, pos + 1)) {
                        // An entry starts at the block start or right after the
                        // previous entry's NUL terminator.
                        if (pos == 0 || env[pos - 1] == L'\0') ++gpu0_entries;
                    }
                    if (gpu0_entries == 1) {
                        c->second_spawn_override = true;
                    }
                }
                return detail::FakeHandshakeSpawn(req, c->hs); // genuine Ready handshake
            },
            [](HANDLE, void*) {}, &re_ctx);
        mgr.SetTranslateExePath(self);
        mgr.RegisterFamily(kTranslateFamily, self); // the boot "" family (never gated)
        mgr.RegisterTranslatePoolBootEntry();
        mgr.SetVramBytesResolver([](const std::string&) { return 4ull * 1024 * kMiB; });
        // 5 GiB free - 4 GiB model = 1 GiB < 1.5 GiB floor -> !allow_gpu (CPU leg).
        mgr.SetFreeVramProbe([](unsigned long long& out) { out = 5ull * 1024 * kMiB; return true; });

        hv2::TranslatePoolEntry* e = mgr.TranslatePoolEnsure("user-Y");
        TEST_CHECK(e != nullptr && e->spawn_gpu_env_override,
                   "C2 respawn-env: CPU-leg entry flagged for EMEBALA_MT_GPU=0");
        hv2::WorkerHandle* w1 = mgr.EnsureSpawned(e->family);
        if (ctx.hs_thread.joinable()) ctx.hs_thread.join();
        TEST_CHECK(w1 != nullptr && w1->state == hv2::WorkerState::Ready,
                   "C2 respawn-env: first spawn reached Ready");

        ::SetEvent(ctx.exit_ev);
        TEST_CHECK(mgr.ReaperPass(0) == 1,
                   "C2 respawn-env: the exit is finalized (worker crashed)");
        hv2::WorkerHandle* crashed = mgr.Find(e->family);
        TEST_CHECK(crashed != nullptr && crashed->state == hv2::WorkerState::Crashed,
                   "C2 respawn-env: family is Crashed (the respawn path C2 targets)");

        // A fresh exit event for the respawned worker, then respawn past backoff.
        ::ResetEvent(ctx.exit_ev);
        hv2::WorkerHandle* w2 = mgr.EnsureSpawned(e->family);
        if (ctx.hs_thread.joinable()) ctx.hs_thread.join();
        TEST_CHECK(w2 != nullptr && w2->state == hv2::WorkerState::Ready,
                   "C2 respawn-env: backoff respawn reached Ready");
        TEST_CHECK(re_ctx.spawns == 2, "C2 respawn-env: exactly one respawn (2 spawns total)");
        TEST_CHECK(re_ctx.second_spawn_override,
                   "C2 respawn-env: the RESPAWNED CPU-leg spawn carries EMEBALA_MT_GPU=0 "
                   "(the state-gated hole C2 closed)");
        mgr.GracefulStop(*w2, 2000);
        ::CloseHandle(ctx.exit_ev);
    }

    // ---- P5a.5 F-3 (195700): relay snapshot invalidated on crash+respawn ------
    // Windows recycles a closed process-handle value for a NEW process, so the
    // old `relayed_worker_process != current_process` comparison can compare
    // numerically-EQUAL handles and wrongly answer "no re-relay" after a
    // respawn. This pin drives the genuine crash->respawn path with a tracked
    // process handle that is REUSED for the respawn (same ctx.exit_ev value
    // returned for BOTH spawns), so the numeric-handle comparison alone would
    // see an unchanged handle and (pre-fix) answer no-re-relay. The fix clears
    // the entry's relay snapshot on the crash/respawn transition, so
    // NeedsRelay is unconditionally true again after the respawn.
    {
        namespace detail = translate_pool_test_detail;
        detail::FakeHandshakeCtx ctx;
        ctx.exit_ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        TEST_CHECK(ctx.exit_ev != nullptr, "F-3 relay-invalidate: exit event created");
        hv2::WorkerManager mgr(&detail::FakeHandshakeSpawn, [](HANDLE, void*) {}, &ctx);
        mgr.SetTranslateExePath(self);
        mgr.RegisterFamily(kTranslateFamily, self);
        mgr.RegisterTranslatePoolBootEntry();
        mgr.SetVramBytesResolver([](const std::string&) { return 700ull * kMiB; });
        mgr.SetFreeVramProbe([](unsigned long long& out) { out = 8ull * 1024 * kMiB; return true; });

        hv2::WorkerHandle* w1 = mgr.EnsureSpawned(kTranslateFamily);
        if (ctx.hs_thread.joinable()) ctx.hs_thread.join();
        TEST_CHECK(w1 != nullptr && w1->state == hv2::WorkerState::Ready,
                   "F-3 relay-invalidate: first spawn reached Ready");
        // Relay to the first worker and record the snapshot (as the dispatcher
        // does post-ack).
        TEST_CHECK(mgr.TranslatePoolNeedsRelay("", w1->process),
                   "F-3 relay-invalidate: first job needs relay (initial snapshot)");
        mgr.TranslatePoolMarkRelayed("", w1->process);
        TEST_CHECK(!mgr.TranslatePoolNeedsRelay("", w1->process),
                   "F-3 relay-invalidate: snapshot recorded (no relay while unchanged)");

        // Crash + respawn. The fake launcher returns the SAME ctx.exit_ev value
        // for the respawned worker, so w2->process == w1->process numerically —
        // the handle-recycling PRECONDITION, not a comparison statement: w1 and
        // w2 alias the same WorkerHandle (FindLocked returns one stable object
        // per family, A-1 deque), so the old `w2->process == w1->process`
        // canary compared one field with itself and could never fail. Snapshot
        // the pre-crash VALUE instead — that assertion CAN fail (launcher drift
        // to distinct per-spawn handles, or any respawn write-through drift),
        // which keeps the NeedsRelay check below genuinely bug-detecting:
        //   * canary: handle value at respawn == the pre-crash snapshot -> the
        //     bare `relayed_worker_process != current_process` comparison is
        //     blind to this (re)launch (Windows recycles the freed value).
        //   * NeedsRelay: with the value recycled, ONLY the explicit snapshot
        //     reset can make NeedsRelay true. Drop the reset (the F-3-reverted
        //     hypothetical) and the entry keeps a snapshot pointing at this
        //     recycled value -> equality -> "no re-relay" -> the fresh worker
        //     (active_model_id == "") serves a user-model job on the pinned
        //     default (silent wrong-model). That is the failure mode this pin
        //     catches.
        const HANDLE h1 = w1->process; // pre-crash snapshot (canary source)
        ::SetEvent(ctx.exit_ev);
        TEST_CHECK(mgr.ReaperPass(0) == 1, "F-3 relay-invalidate: the crash is finalized");
        ::ResetEvent(ctx.exit_ev);
        hv2::WorkerHandle* w2 = mgr.EnsureSpawned(kTranslateFamily);
        if (ctx.hs_thread.joinable()) ctx.hs_thread.join();
        TEST_CHECK(w2 != nullptr && w2->state == hv2::WorkerState::Ready,
                   "F-3 relay-invalidate: respawn reached Ready");
        TEST_CHECK(w2->process == h1,
                   "F-3 relay-invalidate: the respawned handle numerically equals the old "
                   "(handle-recycling canary armed)");
        TEST_CHECK(mgr.TranslatePoolNeedsRelay("", w2->process),
                   "F-3 relay-invalidate: NeedsRelay true again post-respawn even though the "
                   "handle value matches (snapshot invalidated on the transition)");
        mgr.GracefulStop(*w2, 2000);
        ::CloseHandle(ctx.exit_ev);
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Translate pool machinery tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Translate pool machinery tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// =============================================================================
// Plan-B (REQ-B010) B-T5: fast-path pins on the LIVE pool routing (design §9,
// re-scoped by v2 amendment A3 to client-visible behavior + the one documented
// internal delta). These pins run against the SAME pool API the B-T4-wired
// dispatchers call (host_main.cpp DispatcherLoop L870-931 / DispatcherV2Loop
// L1183-1224); no dispatcher source is exercised (host_main has no unit seam,
// tech-gate honesty note A6) — the pins mirror the exact per-job call sequence
// and assert the pool-side invariants REQ-B010 depends on.
//
// Scope note (B-T5): pin 1 is the v2 §A2-rescoped single-dispatcher fast path;
// pin 3 is the NEW mixed v1+v2 single-relay pin that documents the 2→1 relay
// dedup (the ONLY internal delta §9 declares). Pin 4 (the structural
// client-visible-identity scan) lives in TestTranslatePoolClientVisibleIdentity
// below and complements the B-T4fix1 negative pins.

namespace translate_pool_fastpath_detail {

// RegisterFamily-effect counter for the FastPath pin (the spawn seam is a
// plain function pointer, so the count lives in a static the trampoline bumps;
// scoped to this pin). Distinguishes a REGISTRATION (family-table effect) from
// a repeated Ensure on an existing entry (which must NOT re-register).
struct RegisterProbe {
    int registrations = 0;
    int spawns = 0;
};
inline RegisterProbe& Probe() {
    static RegisterProbe p;
    return p;
}

} // namespace translate_pool_fastpath_detail

void TestTranslatePoolFastPathReqB010() {
    std::cout << "[RUN] Testing translate pool fast-path pins (Plan-B B-T5, REQ-B010)..."
              << std::endl;
    const int failures_before = g_failed_count;
    namespace hv2 = emebalachat::host_v2;

    const wchar_t* kTranslateFamily = L"ggml-translate";
    const unsigned long long kMiB = 1024ull * 1024;
    wchar_t self[MAX_PATH] = {0};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH); // a guaranteed-present exe path
    namespace detail = translate_pool_fastpath_detail;

    // ---- Pin 1 (design §9.2 / v2 §A2): TranslatePoolFastPathPin -------------
    // Single dispatcher, pool booted with ONLY the "" entry. 10 job-equivalents
    // drive the SAME per-job sequence the v1 loop runs:
    //   Ensure -> EnsureSpawned -> TrySetBusy -> NeedsRelay -> [relay] ->
    //   MarkRelayed -> SetBusy(false). Asserts the single-worker fast path is
    //   preserved: size stays 1, family == kWorkerFamilyTranslate exactly, the
    //   RegisterFamily effect happens once (at boot), and the relay fires on
    //   the FIRST job only (nullptr process snapshot), never per job.
    {
        detail::Probe() = detail::RegisterProbe{};
        namespace b3 = translate_pool_test_detail; // B-T3 fake-launcher handshake
        b3::FakeHandshakeCtx ctx;
        ctx.exit_ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        TEST_CHECK(ctx.exit_ev != nullptr, "B010 fast-path: exit event created");
        // The RegisterFamily-effect trampoline: a runtime registration effect
        // reuses the SAME translate exe path as the boot family; a spawn whose
        // exe_path DIFFERS from the boot translate path would mean a new family
        // was registered. This lets the pin assert "the RegisterFamily effect
        // happens at boot, not per job" exactly (all 10 jobs stay on the boot
        // family + its single exe path). The expected path travels through the
        // `user` pointer (the spawn seam is a plain function pointer, no
        // captures).
        struct FastPathSpawnCtx {
            const wchar_t* boot_path;
            b3::FakeHandshakeCtx* hs;
        } fp_ctx{self, &ctx};
        hv2::WorkerManager mgr(
            [](const hv2::SpawnRequest& req, void* user) -> hv2::SpawnResult {
                auto* c = static_cast<FastPathSpawnCtx*>(user);
                if (req.exe_path != std::wstring(c->boot_path)) {
                    ++detail::Probe().registrations; // a NEW family registration effect
                }
                ++detail::Probe().spawns;
                return b3::FakeHandshakeSpawn(req, c->hs); // genuine Ready handshake
            },
            [](HANDLE, void*) {}, &fp_ctx);
        mgr.SetTranslateExePath(self);
        // Boot registration — ONE RegisterFamily effect, the same family the ""
        // pool entry references. (The boot call itself is the single effect;
        // counted by the explicit boot marker below so the trampoline's
        // runtime-registration count starts at zero.)
        mgr.RegisterFamily(kTranslateFamily, self);
        detail::Probe().registrations = 0; // boot effect consumed; runtime effects accumulate from here
        TEST_CHECK(mgr.RegisterTranslatePoolBootEntry(),
                   "B010 fast-path: boot \"\" entry registered");
        mgr.SetVramBytesResolver([](const std::string&) { return 0ull; }); // resolver leg
        mgr.SetFreeVramProbe([](unsigned long long& out) { out = 8ull * 1024 * kMiB; return true; });

        // EnsureSpawned reaches Ready through the genuine connect+announce
        // handshake (the B-T3 fake-launcher pattern): the single-worker fast
        // path serves every job from THIS one Ready worker.
        hv2::WorkerHandle* ready = mgr.EnsureSpawned(kTranslateFamily);
        if (ctx.hs_thread.joinable()) ctx.hs_thread.join();
        TEST_CHECK(ready != nullptr && ready->state == hv2::WorkerState::Ready,
                   "B010 fast-path: the single worker reached Ready (real connect+announce)");
        TEST_CHECK(detail::Probe().registrations == 0,
                   "B010 fast-path: the boot-family spawn issued NO new RegisterFamily effect");
        HANDLE live_process = ready->process;

        int relay_calls = 0;
        for (int job = 0; job < 10; ++job) {
            const std::string pin; // "" — all clients on the pinned default
            // (1) Ensure -> the boot entry (no new registration for "").
            hv2::TranslatePoolEntry* e = mgr.TranslatePoolEnsure(pin);
            TEST_CHECK(e != nullptr, "B010 fast-path: Ensure(\"\") returns the boot entry");
            TEST_CHECK(e->family == std::wstring(kTranslateFamily),
                       "B010 fast-path: entry.family == kWorkerFamilyTranslate (exact string)");
            TEST_CHECK(mgr.TranslatePoolSize() == 1,
                       "B010 fast-path: pool size stays 1 (single-worker fast path)");
            // (2) EnsureSpawned on the entry family (the existing Ready worker —
            // zero additional spawns across jobs 1-10).
            hv2::WorkerHandle* w = mgr.EnsureSpawned(e->family);
            TEST_CHECK(w == ready, "B010 fast-path: EnsureSpawned serves the existing worker");
            // (3) TrySetBusy single-flight acquire (mirrors the loop prolog).
            TEST_CHECK(mgr.TrySetBusy(e->family),
                       "B010 fast-path: TrySetBusy acquires the family for the job");
            // (4) NeedsRelay — true ONLY for the first job (nullptr process
            // snapshot vs the live Ready worker process, REQ-058).
            if (mgr.TranslatePoolNeedsRelay(pin, live_process)) {
                // (5) Relay (EnsureWorkerModelRelayed — no host seam here, so
                // the relay is the pool-side relay-state transition the worker
                // ack would drive) then MarkRelayed.
                ++relay_calls;
                mgr.TranslatePoolMarkRelayed(pin, live_process);
            }
            // (6) Post-answer release (mirrors the loop's SetBusy(false)).
            mgr.SetBusy(e->family, false);
        }

        TEST_CHECK(relay_calls == 1,
                   "B010 fast-path: exactly ONE relay host-wide on the single-dispatcher "
                   "fast path (first job only; jobs 2-10 zero additional relays)");
        TEST_CHECK(detail::Probe().registrations == 0,
                   "B010 fast-path: RegisterFamily effect happens exactly once (at boot, "
                   "not per job)");
        TEST_CHECK(detail::Probe().spawns == 1,
                   "B010 fast-path: exactly ONE spawn across all 10 jobs (the existing worker "
                   "serves every job; no re-spawn)");
        TEST_CHECK(mgr.TranslatePoolSize() == 1,
                   "B010 fast-path: pool size == 1 at end of 10 jobs");
        mgr.GracefulStop(*ready, 2000); // Ready worker released (pipe client closed -> EOF)
        ::CloseHandle(ctx.exit_ev);
    }

    // ---- Pin 2 (design §9.2): TranslatePoolSingleEntryNoLeak ----------------
    // 100 Ensure iterations on pin="": pool size stays 1, the reserved-VRAM
    // tracker reflects exactly one worker's bytes (resolver-driven), no drift
    // after ensure×100, no duplicate entries. A real spawned worker (worker-
    // thread handshake reaches Ready) charges the tracker ONCE; the 100 Ensures
    // then leave size + tracker byte-for-byte stable.
    {
        namespace b3 = translate_pool_test_detail; // B-T3 fake-launcher handshake
        b3::FakeHandshakeCtx ctx;
        ctx.exit_ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        TEST_CHECK(ctx.exit_ev != nullptr, "B010 no-leak: exit event created");
        hv2::WorkerManager mgr(&b3::FakeHandshakeSpawn, [](HANDLE, void*) {}, &ctx);
        mgr.SetTranslateExePath(self);
        mgr.RegisterFamily(kTranslateFamily, self);
        mgr.RegisterTranslatePoolBootEntry();
        mgr.SetVramBytesResolver([](const std::string&) { return 700ull * kMiB; });
        mgr.SetFreeVramProbe([](unsigned long long& out) { out = 8ull * 1024 * kMiB; return true; });

        hv2::WorkerHandle* w = mgr.EnsureSpawned(kTranslateFamily);
        if (ctx.hs_thread.joinable()) ctx.hs_thread.join();
        TEST_CHECK(w != nullptr && w->state == hv2::WorkerState::Ready,
                   "B010 no-leak: the single worker reached Ready (real connect+announce)");
        TEST_CHECK(mgr.TranslatePoolReservedVramBytes() == 700ull * kMiB,
                   "B010 no-leak: tracker reflects exactly one worker's bytes (resolver-driven)");

        for (int i = 0; i < 100; ++i) {
            hv2::TranslatePoolEntry* e = mgr.TranslatePoolEnsure("");
            TEST_CHECK(e != nullptr && e->family == std::wstring(kTranslateFamily),
                       "B010 no-leak: every Ensure(\"\") returns the SAME boot entry");
            TEST_CHECK(mgr.TranslatePoolSize() == 1,
                       "B010 no-leak: pool size stays 1 across ensure×100");
            TEST_CHECK(mgr.TranslatePoolReservedVramBytes() == 700ull * kMiB,
                       "B010 no-leak: tracker byte-exact across ensure×100 (no drift)");
        }
        TEST_CHECK(mgr.TranslatePoolSize() == 1 && mgr.TranslatePoolReservedVramBytes() == 700ull * kMiB,
                   "B010 no-leak: after ensure×100 size==1 and tracker==one worker's bytes");
        TEST_CHECK(mgr.TranslatePoolEnsure("") == mgr.TranslatePoolEnsure(""),
                   "B010 no-leak: repeated Ensure(\"\") returns the identical entry (no duplicates)");

        // Release the worker (its handle is the live event; the pipe client
        // closed -> EOF, so the graceful stop is immediate).
        mgr.GracefulStop(*w, 2000);
        ::CloseHandle(ctx.exit_ev);
    }

    // ---- Pin 3 (v2 §A3 NEW): TranslatePoolMixedV1V2SingleRelay --------------
    // BOTH dispatcher loops' relay protocol against ONE pool entry, same pin
    // ("") and same worker process handle. The v2 §A3 delta: pre-Plan-B each
    // loop carried its OWN relayed_* snapshot, so on the "" model TODAY each
    // loop would relay once on its first job = 2 host↔worker relays; under the
    // consolidated pool entry exactly ONE host-wide relay fires. This pin
    // asserts that dedup AND the restart-epoch semantics (a worker-process
    // change makes BOTH loops see NeedsRelay==true again, exactly once each).
    {
        namespace b3 = translate_pool_test_detail;
        b3::FakeHandshakeCtx ctx;
        ctx.exit_ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        TEST_CHECK(ctx.exit_ev != nullptr, "B010 mixed: exit event created");
        hv2::WorkerManager mgr(&b3::FakeHandshakeSpawn, [](HANDLE, void*) {}, &ctx);
        mgr.SetTranslateExePath(self);
        mgr.RegisterFamily(kTranslateFamily, self);
        mgr.RegisterTranslatePoolBootEntry();
        mgr.SetVramBytesResolver([](const std::string&) { return 700ull * kMiB; });
        mgr.SetFreeVramProbe([](unsigned long long& out) { out = 8ull * 1024 * kMiB; return true; });

        hv2::WorkerHandle* w = mgr.EnsureSpawned(kTranslateFamily);
        if (ctx.hs_thread.joinable()) ctx.hs_thread.join();
        TEST_CHECK(w != nullptr && w->state == hv2::WorkerState::Ready,
                   "B010 mixed: the single worker reached Ready (real connect+announce)");
        // The jobs read w->process FRESH each call — exactly what the real
        // dispatcher loops do (host_main.cpp passes w->process to NeedsRelay).
        int v1_relays = 0;
        int v2_relays = 0;
        auto v1_job = [&]() {
            // Mirrors the v1 loop relay block (host_main.cpp L927-931).
            if (mgr.TranslatePoolNeedsRelay("", w->process)) {
                ++v1_relays;
                mgr.TranslatePoolMarkRelayed("", w->process);
            }
        };
        auto v2_job = [&]() {
            // Mirrors the v2 loop relay block (host_main.cpp L1220-1224).
            if (mgr.TranslatePoolNeedsRelay("", w->process)) {
                ++v2_relays;
                mgr.TranslatePoolMarkRelayed("", w->process);
            }
        };

        // Interleave v1, v2, v1, v2 ... 8 jobs total on the SAME "" entry + the
        // SAME worker process handle.
        for (int round = 0; round < 4; ++round) {
            v1_job();
            TEST_CHECK(mgr.TranslatePoolSize() == 1,
                       "B010 mixed: pool size stays 1 across interleaved v1/v2 jobs");
            v2_job();
            TEST_CHECK(mgr.TranslatePoolSize() == 1,
                       "B010 mixed: pool size stays 1 across interleaved v1/v2 jobs");
        }
        // The v2 §A3 dedup assertion: the v1 loop's first job relayed ONCE;
        // the v2 loop's first job produced ZERO additional relays (the pool
        // relay snapshot already matched). Exactly ONE host-wide relay.
        TEST_CHECK(v1_relays == 1,
                   "B010 mixed (v2 §A3): the v1 loop's first job relays exactly once");
        TEST_CHECK(v2_relays == 0,
                   "B010 mixed (v2 §A3): the v2 loop's first job produces ZERO additional "
                   "relays — the 2→1 host-wide dedup delta");

        // Restart epoch: a worker-process change (crash + respawn, REQ-058)
        // re-arms the relay requirement. The jobs read w->process fresh (as the
        // real loops do), so swapping w->process to a NEW live handle models the
        // respawned worker the pool entry must re-relay to.
        wchar_t sys2[MAX_PATH] = {0};
        ::GetSystemDirectoryW(sys2, MAX_PATH);
        const std::wstring ping2 = std::wstring(sys2) + L"\\ping.exe";
        std::wstring ping2_cmd = L"\"" + ping2 + L"\" -t 127.0.0.1 > NUL";
        STARTUPINFOW si2 = {};
        si2.cb = sizeof(si2);
        si2.dwFlags = STARTF_USESHOWWINDOW;
        si2.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi2 = {};
        TEST_CHECK(::CreateProcessW(ping2.c_str(), ping2_cmd.data(), nullptr, nullptr, FALSE,
                                    CREATE_NO_WINDOW, nullptr, nullptr, &si2, &pi2) != FALSE,
                   "B010 mixed: respawned worker-process stand-in spawned");
        ::CloseHandle(pi2.hThread);
        HANDLE h2 = pi2.hProcess;

        // Swap the live worker's process handle to h2 — exactly what a crash +
        // respawn does to w->process (EnsureSpawned returns a fresh handle;
        // the B-T3 ASR pin writes w->process directly, so the field is the
        // test-writable seam). The relay snapshot still carries the ORIGINAL
        // handle (the dead worker), so the next per-job NeedsRelay sees a
        // process change.
        w->process = h2;
        v1_job(); // w->process(h2) vs snapshot (original) -> NeedsRelay true, relays once
        v2_job(); // same entry snapshot now carries h2 -> NeedsRelay false
        v1_job(); // snapshot matches -> false
        v2_job(); // snapshot matches -> false
        TEST_CHECK(v1_relays == 2,
                   "B010 mixed restart epoch: the v1 loop re-relays exactly once on the new "
                   "worker process (REQ-058)");
        TEST_CHECK(v2_relays == 0,
                   "B010 mixed restart epoch: the v2 loop stays deduped — the consolidated "
                   "snapshot already carries the new process (zero additional relays)");

        TEST_CHECK(::TerminateProcess(h2, 1) != FALSE,
                   "B010 mixed: terminate the respawned stand-in");
        ::CloseHandle(h2);
        mgr.GracefulStop(*w, 2000);
        ::CloseHandle(ctx.exit_ev);
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Translate pool fast-path (REQ-B010) pins completed." << std::endl;
    } else {
        std::cout << "[FAIL] Translate pool fast-path (REQ-B010) pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// =============================================================================
// Plan-B (REQ-B009) P6 closure C-2 — TranslatePoolCrashIsolationTest
// (design 234900_architect-planB-design.md §12.2, verbatim):
//   "inject a crash in the mock ggml-translate-user-X family. Assert
//    ggml-translate family state unchanged, jobs on ggml-translate continue to
//    be served."
// Also the F-3 / 220630 P5a.5 named pin the P6 final audit (221800) verified
// was missing (0 repo-wide hits) — this function closes it, greppable as
// TestTranslatePoolCrashIsolationReqB009.
//
// Assertions (the four sibling-isolation invariants the user's co-residency
// scenario depends on):
//   (a) the Hy-MT2/default ("") entry's WorkerState is untouched by the
//       USER-X crash (Ready before, Ready after);
//   (b) the default entry's reserved-VRAM accounting is unchanged by the crash
//       path (its tracker contribution intact — the reaper subtracts EXACTLY
//       the crashed entry's bytes, never the sibling's);
//   (c) NeedsRelay for the default entry's pin is unaffected, while the
//       crashed entry's snapshot is invalidated per the F-3 semantics (relay
//       true again on the respawned worker);
//   (d) EnsureSpawned(default family) still returns a usable Ready handle
//       (sibling serving continues).

// PLACEMENT NOTE (P6 C-2): the §12.2 crash-isolation pin runs in its OWN
// process — a tiny standalone main (tests/crash_iso_test_main.cpp) launched
// via CTest — NOT in the default run_tests suite. Two concurrent named-pipe
// clients cannot be created in the run_tests process (its ~40 prior suites
// leave the named-pipe namespace unable to create the SECOND concurrent pipe:
// CreateNamedPipe returns ERROR_PIPE_BUSY 231 regardless of the unique
// PID+GetTickCount names). A fresh process with no prior pipe state creates
// both pool workers' pipes cleanly. The pin body is IDENTICAL (same B-T3
// fake-launcher handshake, same WorkerManager, same four assertions a-d);
// only the host process differs. The standalone main provides its own
// TEST_CHECK harness + this same .inc.

namespace crash_iso_detail {

// Shared context for the crash-isolation pin's ONE handshake thread. TWO pool
// entries (default "" + a distinct user family), each backed by its OWN
// tracked process handle (its own manual-reset exit event), so a crash on one
// family is driven without touching the other. ONE SHARED handshake THREAD
// serves BOTH pipes: this process can only ever create TWO concurrent named
// pipes (a per-process named-pipe ceiling — a third CreateNamedPipe returns
// ERROR_PIPE_BUSY 231 regardless of the unique names), so a single
// connect+announce sequence per pipe, run by one thread, models both workers
// without a third pipe.
struct CrashIsoCtx {
    translate_pool_test_detail::FakeHandshakeCtx def_hs;   // the "" / Hy-MT2 entry
    translate_pool_test_detail::FakeHandshakeCtx user_hs;  // the user entry
    std::atomic<bool> first_spawn_is_def{true}; // spawn-order routing toggle
    int def_spawns = 0;
    int user_spawns = 0;
};

} // namespace crash_iso_detail

void TestTranslatePoolCrashIsolationReqB009() {
    std::cout << "[RUN] Testing translate pool crash isolation (Plan-B §12.2, REQ-B009 C-2)..."
              << std::endl;
    const int failures_before = g_failed_count;
    namespace hv2 = emebalachat::host_v2;
    namespace detail = translate_pool_test_detail;
    namespace cid = crash_iso_detail;

    const wchar_t* kTranslateFamily = L"ggml-translate";
    // The crashed sibling uses a DISTINCT, non-colliding family id
    // ("zz-crashiso" -> family "ggml-translate-zz-crashiso"): a plain
    // "user-X" id would collide with the earlier pool blocks' user-Y/user-Z
    // spawn-failure state, and "zz" sorts AFTER "ggml-" so the reaper's
    // deque order hits the crashed entry DETERMINISTICALLY (a "user-*" id
    // sorts WITHIN ggml-, before the running default entry, so the reaper
    // would count the still-live default first and answer 2).
    const wchar_t* kUserFamily = L"ggml-translate-zz-crashiso";
    const std::string kDefaultPin = "";            // the pinned default (Hy-MT2) relay pin
    const std::string kUserPin = "zz-crashiso";    // the chat user-model relay pin
    const unsigned long long kMiB = 1024ull * 1024;
    wchar_t self[MAX_PATH] = {0};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH); // a guaranteed-present exe path

    static cid::CrashIsoCtx c; // static: the spawn seam is a plain function pointer
    c.def_hs = detail::FakeHandshakeCtx{};
    c.user_hs = detail::FakeHandshakeCtx{};
    c.first_spawn_is_def.store(true, std::memory_order_release);
    c.def_spawns = 0;
    c.user_spawns = 0;
    c.def_hs.exit_ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    c.user_hs.exit_ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    TEST_CHECK(c.def_hs.exit_ev != nullptr && c.user_hs.exit_ev != nullptr,
               "crash-iso: both tracked exit events created");

    // Heap-allocated + intentionally leaked (never deleted): the pin runs in a
    // short-lived standalone process, and ~WorkerManager's GracefulStop pipe
    // reads on the fake-launcher stubs fail-fast the process at teardown
    // (0xC0000409). Leaking the manager skips destruction; the OS reclaims all
    // handles at process exit (the pipes are fake stubs, not real workers).
    auto* pmgr = new hv2::WorkerManager(
        [](const hv2::SpawnRequest& req, void* user) -> hv2::SpawnResult {
            auto* cc = static_cast<cid::CrashIsoCtx*>(user);
            // Route by the EXACT per-family worker exe image name
            // ("Emebalachat.Engine.<family>.exe"). A starts_with test is
            // WRONG here (the default family's name is a prefix of the user's)
            // and a substring test is fragile; the exact image name is unique.
            // The FIRST translate-family spawn starts the ONE shared handshake
            // thread (both pipes exist only after the SECOND spawn; the thread
            // waits for each pipe by name).
            // Route by the EXACT per-family suffix. The default family's name
            // ("ggml-translate") is a PREFIX of the user's ("ggml-translate-
            // zz-crashiso"), so match the LONGER (user) name FIRST; anything
            // else is the default. (Matching the full "...exe" image name was
            // unreliable — the per-family worker exe path is "<family>" in the
            // spawn request, so match the family token directly.)
            // Route by SPAWN ORDER via a first-spawn toggle: the worker exe is
            // model-agnostic (the SAME translate exe for every family — the
            // model resolves INSIDE the worker), so exe_path cannot distinguish
            // families. The pin spawns the DEFAULT ("" / ggml-translate) family
            // FIRST (including any nullptr-retry, which lands before the user
            // spawn) and the USER (ggml-translate-zz-crashiso) family SECOND;
            // the FIRST spawn is the default, every SUBSEQUENT spawn is the user.
            detail::FakeHandshakeCtx* h = cc->first_spawn_is_def ? &cc->def_hs : &cc->user_hs;
            cc->first_spawn_is_def = false; // only the very first spawn is the default
            const bool is_user = (h == &cc->user_hs);
            if (is_user) ++cc->user_spawns; else ++cc->def_spawns;
            h->pipe_name = req.pipe_name;
            h->token = req.token;
            emebalachat::host_v2::SpawnResult r;
            r.ok = true;
            r.process = h->exit_ev;
            r.done_event = h->exit_ev;
            // EACH spawn runs its OWN handshake thread (the proven FakeHandshake
            // pattern): the thread connects + announces to THIS pipe while the
            // manager's blocking ConnectNamedPipe/announce-read waits. The two
            // families' spawns are serialized by a tick-separating sleep in the
            // caller (see below) so their pipe names differ.
            h->hs_thread = std::thread(translate_pool_test_detail::FakeHandshakeWorkerThread, h);
            return r;
        },
        [](HANDLE, void*) {}, &c);
    pmgr->SetTranslateExePath(self);
    pmgr->RegisterFamily(kTranslateFamily, self); // the boot "" family registration
    pmgr->RegisterTranslatePoolBootEntry();
    pmgr->SetVramBytesResolver([](const std::string&) { return 700ull * kMiB; });
    pmgr->SetFreeVramProbe([](unsigned long long& out) { out = 8ull * 1024 * kMiB; return true; });

    // Ensure BOTH pool entries (default boot entry + the on-demand zz-crashiso
    // family) — the size==2 co-residency topology the pin targets.
    hv2::TranslatePoolEntry* def_e = pmgr->TranslatePoolEnsure(kDefaultPin);
    hv2::TranslatePoolEntry* user_e = pmgr->TranslatePoolEnsure(kUserPin);
    TEST_CHECK(def_e != nullptr && user_e != nullptr,
               "crash-iso: both pool entries ensured (size-2 co-residency)");
    TEST_CHECK(def_e->family == std::wstring(kTranslateFamily) &&
               user_e->family == std::wstring(kUserFamily),
               "crash-iso: default family ggml-translate + user family ggml-translate-zz-crashiso");
    TEST_CHECK(pmgr->TranslatePoolSize() == 2, "crash-iso: pool size == 2");
    // Spawn BOTH families to Ready through the genuine connect+announce
    // handshake, EACH on its OWN worker thread (the proven FakeHandshake
    // pattern: the thread connects + announces while the manager's blocking
    // ConnectNamedPipe/announce-read waits). CRITICAL ordering facts:
    //   * MakeWorkerPipeName() keys on GetTickCount() (millisecond resolution),
    //     so two EnsureSpawned calls in the SAME millisecond produce the SAME
    //     pipe name and the second CreateNamedPipe fails with ERROR_PIPE_BUSY
    //     (231). The spawns are serialized by a ≥1 ms settle between them.
    //   * The reaper's deque order is registration order; the DEFAULT ("" /
    //     ggml-translate) family is registered at boot, the USER family
    //     (ggml-translate-zz-crashiso) on demand — so the default sorts FIRST.
    //     Spawning default-first then user keeps the live default BEFORE the
    //     crashed user in the reaper's sweep, which is the stable ordering
    //     (spawning the shorter-named user first wedged the second spawn).
    hv2::WorkerHandle* def_w = pmgr->EnsureSpawned(kTranslateFamily);
    if (c.def_hs.hs_thread.joinable()) c.def_hs.hs_thread.join();
    if (def_w == nullptr) {
        ::Sleep(50); // settle past a transient pipe-busy, then retry ONCE
        def_w = pmgr->EnsureSpawned(kTranslateFamily);
        if (c.def_hs.hs_thread.joinable()) c.def_hs.hs_thread.join();
    }
    // NB: a non-nullptr but still-Spawning def_w is NOT re-spawned here — that
    // would double-count the spawn. The quiesce below advances it to Ready.
    ::Sleep(20); // ≥1 ms so the user spawn's pipe name lands on a new tick
    hv2::WorkerHandle* user_w = pmgr->EnsureSpawned(kUserFamily);
    if (c.user_hs.hs_thread.joinable()) c.user_hs.hs_thread.join();
    if (user_w == nullptr) {
        ::Sleep(50);
        user_w = pmgr->EnsureSpawned(kUserFamily);
        if (c.user_hs.hs_thread.joinable()) c.user_hs.hs_thread.join();
    }
    // Each family is spawned exactly ONCE (def_spawns/user_spawns); the manager
    // may return the handle while it is still Spawning and promote it to Ready
    // on a later EnsureSpawned/state-read, so Ready is confirmed by the bounded
    // quiesce below (not by re-calling EnsureSpawned, which would double-count).
    TEST_CHECK(c.def_spawns == 1 && c.user_spawns == 1,
               "crash-iso: exactly one spawn per family (each family spawns its own worker)");

    // Quiesce BOTH handshakes with bounded Ready-state polls BEFORE any crash
    // injection: the tracked process handles are manual-reset events the REAPER
    // also polls, and a reaper pass that observes a signaled exit FINALIZES it
    // as a crash (state -> Crashed). Both families must be Ready before the
    // crash so the reaper cannot observe a half-connected pipe.
    {
        int i = 0;
        while (i < 200 && (pmgr->State(kTranslateFamily) != hv2::WorkerState::Ready ||
                           pmgr->State(kUserFamily) != hv2::WorkerState::Ready)) {
            ::Sleep(10);
            ++i;
        }
        TEST_CHECK(i < 200, "crash-iso: both families reached Ready before the crash");
    }
    def_w = pmgr->Find(kTranslateFamily);
    user_w = pmgr->Find(kUserFamily);
    TEST_CHECK(def_w != nullptr && def_w->state == hv2::WorkerState::Ready,
               "crash-iso: default (Hy-MT2) worker reached Ready (real connect+announce)");
    TEST_CHECK(user_w != nullptr && user_w->state == hv2::WorkerState::Ready,
               "crash-iso: user (zz-crashiso) worker reached Ready (real connect+announce)");

    // Post-spawn tracker: EACH entry contributed exactly its resolver bytes.
    const unsigned long long kContribution = 700ull * kMiB;
    TEST_CHECK(pmgr->TranslatePoolReservedVramBytes() == 2 * kContribution,
               "crash-iso: tracker holds BOTH entries' contributions pre-crash");

    // Snapshot BOTH entries' relay state (as the dispatchers do after each
    // job's relay ack). The default pin "" vs the user pin "zz-crashiso" are
    // DIFFERENT entries, so relaying one must not satisfy the other.
    TEST_CHECK(pmgr->TranslatePoolNeedsRelay(kDefaultPin, def_w->process),
               "crash-iso: default pin's initial snapshot needs relay");
    pmgr->TranslatePoolMarkRelayed(kDefaultPin, def_w->process);
    TEST_CHECK(!pmgr->TranslatePoolNeedsRelay(kDefaultPin, def_w->process),
               "crash-iso: default pin's snapshot recorded");
    TEST_CHECK(pmgr->TranslatePoolNeedsRelay(kUserPin, user_w->process),
               "crash-iso: user pin's initial snapshot needs relay");
    pmgr->TranslatePoolMarkRelayed(kUserPin, user_w->process);
    TEST_CHECK(!pmgr->TranslatePoolNeedsRelay(kUserPin, user_w->process),
               "crash-iso: user pin's snapshot recorded");

    // ---- Inject the crash on the user (zz-crashiso) entry (§12.2) ------------
    // The user family is Ready (quiesced above); its tracked process handle is
    // a manual-reset event the REAPER also polls. Signaling it makes the reaper
    // observe the exit and finalize THAT family as Crashed. Because the user
    // sorts LAST in the reaper's deque, the reaper judges ONLY the crashed user
    // entry; the still-Ready default is untouched.
    ::SetEvent(c.user_hs.exit_ev); // the user worker's tracked process dies
    TEST_CHECK(pmgr->ReaperPass(0) == 1, "crash-iso: the reaper judges exactly ONE family (the "
                                       "crashed user entry) — the default is untouched");
    const bool user_crashed = (user_w != nullptr && user_w->state == hv2::WorkerState::Crashed);
    TEST_CHECK(user_crashed,
               "crash-iso: the user family is finalized as Crashed (the injected crash)");

    // (a) the Hy-MT2/default entry's WorkerState is UNCHANGED by the sibling
    //     crash — Ready before, Ready after (§12.2 design guarantee).
    TEST_CHECK(def_w->state == hv2::WorkerState::Ready,
               "crash-iso (a): the default (Hy-MT2) worker's state is untouched by the "
               "user crash (still Ready)");
    TEST_CHECK(pmgr->State(kTranslateFamily) == hv2::WorkerState::Ready,
               "crash-iso (a): the default family state query agrees (Ready)");

    // (b) the default entry's reserved-VRAM accounting is UNCHANGED by the
    //     crash path: the reaper subtracts EXACTLY the crashed entry's bytes,
    //     the sibling's contribution stays intact (§4.3/§12.4 exact-once).
    TEST_CHECK(def_e->reserved_vram_bytes == kContribution,
               "crash-iso (b): the default entry's reserved_vram_bytes intact");
    TEST_CHECK(pmgr->TranslatePoolReservedVramBytes() == kContribution,
               "crash-iso (b): the tracker holds exactly the default contribution "
               "(the user contribution subtracted, the sibling's untouched)");

    // (c) the default entry's pin NeedsRelay is unaffected by the sibling
    //     crash (its snapshot still matches its own live worker).
    TEST_CHECK(!pmgr->TranslatePoolNeedsRelay(kDefaultPin, def_w->process),
               "crash-iso (c): the default pin's relay snapshot unaffected by the user crash");

    // (d) EnsureSpawned(default family) still returns a usable handle: the
    //     sibling serving continues (§12.2 "jobs on ggml-translate continue to
    //     be served"). Zero additional spawns on the default family across the
    //     sibling crash — the SAME Ready worker serves.
    hv2::WorkerHandle* def_served = pmgr->EnsureSpawned(kTranslateFamily);
    TEST_CHECK(def_served != nullptr && def_served->state == hv2::WorkerState::Ready,
               "crash-iso (d): EnsureSpawned(default family) still serves (sibling serving "
               "continues)");
    TEST_CHECK(def_served == def_w && c.def_spawns == 1,
               "crash-iso (d): the default worker is the SAME handle — zero additional "
               "spawns on the default family across the sibling crash");

    // Cleanup: the manager is heap-allocated + leaked (see the declaration), so
    // there is NO destructor GracefulStop here. The crashed user family's pipe
    // is closed by the reaper above; the READY default worker's pipe is a clean
    // EOF (its handshake client already closed). The OS reclaims every handle
    // at process exit. The pin's assertions are complete above.
    ::CloseHandle(c.def_hs.exit_ev);
    ::CloseHandle(c.user_hs.exit_ev);

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Translate pool crash isolation (§12.2, REQ-B009 C-2) pins completed."
                  << std::endl;
    } else {
        std::cout << "[FAIL] Translate pool crash isolation (§12.2, REQ-B009 C-2) pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// NOTE (B-T5 pin 4 placement): the structural client-visible-identity scan pin
// (TestTranslatePoolClientVisibleIdentity) lives in
// translate_pool_identity_pin_tests.inc — an end-of-file .inc after
// ResolveRepoFile (this file is included BEFORE ResolveRepoFile is defined, so
// the source-scan helper is unavailable here). Registered in main() at the end
// of file, per the end-of-file pattern.
