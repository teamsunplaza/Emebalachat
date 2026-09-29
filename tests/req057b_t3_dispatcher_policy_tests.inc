// ---------------------------------------------------------------------------
// REQ-CP T3 (session 260928_0001, design §9 rows 3-5 + §11 T3 + tech-gate
// amendment A1): the dispatcher-side integration of the per-client engine
// policy into BOTH worker-dispatch loops.
//
// What this suite pins:
//   1. Pin resolution via the resolver — RelayPinForClient is policy-driven:
//      chat keeps the legacy C1-gated live user_gguf pin when its policy
//      section model_id is EMPTY (item-D verbatim), a non-empty policy
//      model_id wins, and listener / unknown clients stay "" (pinned bundled
//      default). The fail-closed behavior rides the T2 reader, so an absent /
//      garbage / schema-rejected policy file resolves to the compiled-in
//      defaults (model_id "") -> ZERO behavior change when the file is absent.
//   2. v1 job-frame sampling stamp — the v1 build sets jm.sampling_present =
//      true and copies the resolved client-policy numerics (option b host-side
//      Listener profile; the frozen v1 client<->host wire is untouched, only
//      the host<->worker INTERNAL frame gains the sampling member). The
//      resolved defaults ARE the shipped 0.0/0.6/20/1.05, so an absent file is
//      numerically identical to the old never-set frame.
//   3. Client-scoped session id for v1 — jm.session = StableClientSessionId
//      (client), a deterministic FNV-1a of the canonical id offset into a
//      disjoint [1e9, 5e9) range (tech-gate A1: never collides with the v2
//      enqueue_seq job-id namespace or a real v2 session-table id). Verified
//      safe against the worker: it reads job.session ONLY to echo it on the
//      terminal event (no cache key, no per-id SessionOpenMsg).
//   4. A1 v2 body copy — SchedItem carries src/tgt/text (inert data, like
//      `client`), the enqueue site captures them, and the v2 job build copies
//      them into the worker frame + the same sampling stamp (fixes the latent
//      empty-body gap).
//   5. The policy model_id change rides the EXISTING changed-pin re-relay
//      comparison (EnsureWorkerModelRelayed) — a bare assignment would silently
//      serve the old model (tech-gate R-A / T2 owner note), so the source pin
//      asserts the pin still flows through RelayPinForClient per job and the
//      re-relay comparison is intact.
//
// Verification method (tech-gate honesty note A6): host_main.cpp builds ONLY
// into the EmebaEngine exe — there is NO dispatcher unit-test seam. These are
// therefore SOURCE pins (the ResolveRepoFile pattern, same as REQ-055(e) /
// REQ-057(b)) + the full-suite regression, NOT behavioral unit tests. The one
// behavioral leg seeds a temp-lad policy file and asserts the listener section
// sampling reaches the resolver output (proving the host-side reader seam the
// dispatcher consumes), mirroring the T2 req056 fixture.
// ---------------------------------------------------------------------------

void TestReq057bT3DispatcherPolicy() {
    std::cout << "[RUN] Testing REQ-CP T3 dispatcher policy integration..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- behavioral: a seeded listener policy section reaches resolution ----
    namespace fs = std::filesystem;
    namespace cp = emebalachat::enginehost::clientpolicy;
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / L"emebala_req057b_t3_dispatch";
    if (ec) {
        TEST_CHECK(false, "REQ-CP T3 fixture: temp dir unavailable");
    } else {
        fs::remove_all(root, ec);
        ec.clear();
        fs::create_directories(root / L"Emebala" / L"Common", ec);
        ec.clear();
        {
            std::ofstream out(root / L"Emebala" / L"Common" / L"engine_client_policy.json",
                              std::ios::binary | std::ios::trunc);
            out << "{\"schema_version\":1,\"clients\":{"
                   "\"emebala-listner\":{\"model_id\":\"\",\"profile_name\":\"\","
                   "\"sampling\":{\"temperature\":0.2,\"top_p\":0.85,\"top_k\":40,\"rep_pen\":1.1},"
                   "\"prompt_template_ref\":\"subtitle-realtime-v1\"}}}";
        }
        // Margin for filesystems with coarse mtime granularity so the live
        // reader's mtime/size signature is stable before the read (the same
        // discipline as the req055 rewrite pin).
        ::Sleep(120);
        const cp::ClientPolicy p =
            emebalachat::enginehost::LoadClientPolicyLive("emebala-listner", root.wstring());
        TEST_CHECK(p.sampling_present, "REQ-CP T3: seeded listener section marks sampling_present");
        TEST_CHECK(p.sampling.temperature > 0.19f && p.sampling.temperature < 0.21f,
                   "REQ-CP T3: seeded listener temperature reaches the resolved policy");
        TEST_CHECK(p.sampling.top_k == 40, "REQ-CP T3: seeded listener top_k reaches the resolved policy");
        // Absent file (never-created dir) -> fail-closed defaults, numerically
        // the shipped values (the v1/v2 sampling stamp is identical to the old
        // never-set behavior when the policy file is absent).
        const fs::path rootMissing = fs::temp_directory_path(ec) / L"emebala_req057b_t3_missing";
        const cp::ClientPolicy d =
            emebalachat::enginehost::LoadClientPolicyLive("emebala-listner", rootMissing.wstring());
        TEST_CHECK(!d.sampling_present && d.model_id.empty() &&
                       d.sampling.temperature == cp::kDefaultTemperature &&
                       d.sampling.top_p == cp::kDefaultTopP &&
                       d.sampling.top_k == cp::kDefaultTopK &&
                       d.sampling.rep_pen == cp::kDefaultRepPen,
                   "REQ-CP T3: absent policy file -> compiled-in defaults (item-D equivalence)");
        fs::remove_all(root, ec);
        ec.clear();
    }

    // ---- source pins: host_main.cpp dispatcher wiring ----------------------
    const std::filesystem::path host_main = ResolveRepoFile(L"src\\host_main.cpp");
    if (host_main.empty()) {
        std::cout << "[SKIP] src/host_main.cpp not resolvable; REQ-CP T3 source pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(host_main, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());

        // (1) Pin resolution via the resolver: RelayPinForClient resolves the
        // live client policy; chat keeps the legacy C1 live pin as the empty
        // fallback; the C1 live reader is still reachable (item-D gate).
        TEST_CHECK(src.find("LoadClientPolicyLive") != std::string::npos,
                   "REQ-CP T3: the dispatcher resolves the per-client policy via the live reader");
        TEST_CHECK(src.find("RelayPinForClient") != std::string::npos,
                   "REQ-CP T3: RelayPinForClient remains the shared pin gate for both loops");
        {
            // The wrapper still reaches the C1-gated live user_gguf read for
            // chat (the item-D REQ-055(e) source pin keeps passing).
            const size_t pin_pos = src.find("RelayPinForClient");
            TEST_CHECK(pin_pos != std::string::npos &&
                           src.find("LoadUserModelIdFromConfigLive", pin_pos) != std::string::npos,
                       "REQ-CP T3: chat keeps the legacy C1 live pin as the empty-model_id fallback");
        }

        // (2) v1 sampling stamp: the v1 build sets sampling_present=true and
        // copies the four resolved numerics. Anchor the scope on the FUNCTION
        // DEFINITIONS (the bare names also appear in comments/includes, so a
        // naive find would mis-order the loops).
        {
            const size_t v1_pos = src.find("void DispatcherLoop(");
            const size_t v2_pos = src.find("void DispatcherV2Loop(");
            TEST_CHECK(v1_pos != std::string::npos && v2_pos != std::string::npos &&
                           v2_pos > v1_pos,
                       "REQ-CP T3: both dispatcher loop definitions are present and ordered");
            // Scope the v1 build between the two function definitions.
            TEST_CHECK(src.find("jm.sampling_present = true", v1_pos) < v2_pos,
                       "REQ-CP T3: the v1 job frame stamps sampling_present = true");
            TEST_CHECK(src.find("jm.sampling.temperature = job_policy.sampling.temperature",
                                v1_pos) < v2_pos,
                       "REQ-CP T3: the v1 job frame copies the resolved policy temperature");
            TEST_CHECK(src.find("jm.sampling.top_k = job_policy.sampling.top_k", v1_pos) < v2_pos,
                       "REQ-CP T3: the v1 job frame copies the resolved policy top_k");
        }

        // (3) Client-scoped session id for v1.
        TEST_CHECK(src.find("StableClientSessionId") != std::string::npos,
                   "REQ-CP T3: the client-scoped stable session id helper exists");
        TEST_CHECK(src.find("jm.session = StableClientSessionId(job.client)") != std::string::npos,
                   "REQ-CP T3: the v1 job frame uses the client-scoped session id");
        {
            // The id is offset into a disjoint range (tech-gate A1).
            const size_t fn_pos = src.find("StableClientSessionId");
            TEST_CHECK(fn_pos != std::string::npos &&
                           src.find("1000000000ULL", fn_pos) != std::string::npos,
                       "REQ-CP T3: StableClientSessionId offsets into the disjoint [1e9, 5e9) range");
        }

        // (4) A1 v2 body copy + sampling: the v2 build copies item.src/tgt/text
        // and stamps the same sampling.
        {
            const size_t v2_pos = src.find("void DispatcherV2Loop(");
            TEST_CHECK(v2_pos != std::string::npos &&
                           src.find("jm.src = item.src;", v2_pos) != std::string::npos,
                       "REQ-CP T3: the v2 job frame copies the item src (A1 body fix)");
            TEST_CHECK(src.find("jm.tgt = item.tgt;", v2_pos) != std::string::npos,
                       "REQ-CP T3: the v2 job frame copies the item tgt (A1 body fix)");
            TEST_CHECK(src.find("jm.text = item.text;", v2_pos) != std::string::npos,
                       "REQ-CP T3: the v2 job frame copies the item text (A1 body fix)");
            TEST_CHECK(src.find("jm.sampling_present = true", v2_pos) != std::string::npos,
                       "REQ-CP T3: the v2 job frame stamps sampling_present = true");
        }

        // (5) The policy model_id change rides the changed-pin re-relay: the
        // re-relay comparison is intact in both loops (tech-gate R-A).
        // B-T4fix1 (session 260928_0001): B-T4 consolidated the per-loop relay
        // snapshots into the pool entry (design §3.5), so the changed-pin /
        // changed-model comparison moved from inline `pin != relayed_model_id`
        // / `model_id != relayed_model_id` literals into the pool-owned
        // TranslatePoolNeedsRelay — byte-identical semantics, still gated per
        // model AND worker process. R-A is now pinned as (a) both loops calling
        // the pool relay contract and (b) the manager's dual-condition body.
        {
            const size_t v1_pos = src.find("void DispatcherLoop(");
            const size_t v2_pos = src.find("void DispatcherV2Loop(");
            TEST_CHECK(v1_pos != std::string::npos && v2_pos != std::string::npos &&
                           v2_pos > v1_pos,
                       "REQ-CP T3: both dispatcher loop definitions located for the R-A pin");
            if (v1_pos != std::string::npos && v2_pos != std::string::npos) {
                const std::string v1_region = src.substr(v1_pos, v2_pos - v1_pos);
                TEST_CHECK(v1_region.find("TranslatePoolNeedsRelay(") != std::string::npos,
                           "REQ-CP T3: the v1 loop routes its changed-pin re-relay through "
                           "TranslatePoolNeedsRelay (R-A)");
                TEST_CHECK(src.find("TranslatePoolNeedsRelay(", v2_pos) != std::string::npos,
                           "REQ-CP T3: the v2 loop routes its changed-model re-relay through "
                           "TranslatePoolNeedsRelay (R-A)");
            }
        }

        // The enqueue site captures the translate body into the SchedItem.
        TEST_CHECK(src.find("item.src = msg.src;") != std::string::npos &&
                       src.find("item.tgt = msg.tgt;") != std::string::npos &&
                       src.find("item.text = msg.text;") != std::string::npos,
                   "REQ-CP T3: the v2 enqueue site captures src/tgt/text into the SchedItem");
    }

    // ---- source pins: host_v2_scheduler.hpp SchedItem body fields ----------
    const std::filesystem::path sched = ResolveRepoFile(L"src\\host_v2_scheduler.hpp");
    if (sched.empty()) {
        std::cout << "[SKIP] src/host_v2_scheduler.hpp not resolvable; REQ-CP T3 sched pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(sched, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        TEST_CHECK(src.find("std::string src;") != std::string::npos &&
                       src.find("std::string tgt;") != std::string::npos &&
                       src.find("std::string text;") != std::string::npos,
                   "REQ-CP T3: SchedItem carries the src/tgt/text body fields (A1, inert data)");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-CP T3 dispatcher policy integration checks passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-CP T3 dispatcher policy integration: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
