// Plan-B (REQ-B003) B-T1: per-model translate pool machinery pins.
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
// B-T1 scope: the pool is present but UNUSED by the dispatchers (still
// hardcoded family); these pins prove the machinery is correct in isolation.

void TestTranslatePool() {
    std::cout << "[RUN] Testing translate pool machinery (Plan-B B-T1)..." << std::endl;
    const int failures_before = g_failed_count;
    namespace hv2 = emebalachat::host_v2;
    namespace tp = emebalachat::host_v2::translate_pool;

    const wchar_t* kTranslateFamily = L"ggml-translate";
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

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Translate pool machinery tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Translate pool machinery tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
