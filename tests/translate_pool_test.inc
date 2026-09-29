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

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Translate pool machinery tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Translate pool machinery tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
