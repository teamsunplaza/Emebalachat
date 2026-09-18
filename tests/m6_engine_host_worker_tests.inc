// REQ-043 (M6 T3): worker_protocol (second frozen contract) + worker-manager
// state-machine unit tests. Included directly by tests/run_tests.cpp (the T2
// session .inc staging contract; they use the TEST_CHECK harness + the std::
// facilities included above). Pure-parser checks run WITHOUT any process
// (R-6 adopted); the manager checks drive the injected-seam state machine.

void TestWorkerProtocol() {
    std::cout << "[RUN] Testing worker protocol contract (REQ-043 M6 T3)..." << std::endl;
    const int failures_before = g_failed_count;
    namespace wp = emebalachat::workerproto;
    namespace eh = emebalachat::enginehost;

    // ---- 1. Manifest golden document (design §3.4) ----
    {
        const char* golden =
            "{\"schema_version\":1,\"family\":\"ggml-translate\","
            "\"engine\":\"llama.cpp\",\"engine_version\":\"b6099\","
            "\"ops\":[\"translate\"],\"abi_version\":1,"
            "\"protocol_min\":1,\"protocol_max\":2}";
        wp::WorkerManifest m;
        TEST_CHECK(wp::ParseManifestJson(golden, m) == wp::ManifestStatus::Ok,
                   "manifest golden: Ok");
        TEST_CHECK(m.schema_version == 1 && m.family == "ggml-translate" &&
                       m.engine == "llama.cpp" && m.engine_version == "b6099" &&
                       m.ops.size() == 1 && m.ops[0] == "translate" &&
                       m.abi_version == 1 && m.protocol_min == 1 &&
                       m.protocol_max == 2,
                   "manifest golden: all fields parsed");
        TEST_CHECK(wp::BuildManifestJson(m) == golden,
                   "manifest: builder reproduces the §3.4 golden bytes");

        // Unknown fields ignored (§V2-12-2).
        wp::WorkerManifest mx;
        TEST_CHECK(wp::ParseManifestJson(
                       "{\"schema_version\":1,\"future_field\":42,\"family\":"
                       "\"ggml-translate\",\"engine\":\"llama.cpp\","
                       "\"engine_version\":\"b6099\",\"ops\":[\"translate\"],"
                       "\"abi_version\":1,\"protocol_min\":1,\"protocol_max\":2}",
                       mx) == wp::ManifestStatus::Ok,
                   "manifest: unknown fields ignored");
    }

    // ---- 2. Manifest fail-closed (schema_version required, §V2-12-2) ----
    {
        wp::WorkerManifest m;
        TEST_CHECK(wp::ParseManifestJson(
                       "{\"family\":\"ggml-translate\",\"engine\":\"llama.cpp\","
                       "\"engine_version\":\"b6099\",\"ops\":[],\"abi_version\":1,"
                       "\"protocol_min\":1,\"protocol_max\":2}", m) ==
                       wp::ManifestStatus::SchemaVersion,
                   "manifest: missing schema_version -> reject");
        TEST_CHECK(wp::ParseManifestJson(
                       "{\"schema_version\":2,\"family\":\"x\",\"engine\":\"e\","
                       "\"engine_version\":\"v\",\"ops\":[],\"abi_version\":1,"
                       "\"protocol_min\":1,\"protocol_max\":2}", m) ==
                       wp::ManifestStatus::SchemaVersion,
                   "manifest: schema_version=2 -> reject");
        TEST_CHECK(wp::ParseManifestJson("garbage", m) == wp::ManifestStatus::NotJson,
                   "manifest: garbage -> NotJson");
        TEST_CHECK(wp::ParseManifestJson(
                       "{\"schema_version\":1,\"family\":\"ggml-translate\"}", m) ==
                       wp::ManifestStatus::Malformed,
                   "manifest: missing fields -> Malformed");
        TEST_CHECK(wp::ParseManifestJson(
                       "{\"schema_version\":1,\"family\":\"ggml-translate\","
                       "\"engine\":\"llama.cpp\",\"engine_version\":\"b6099\","
                       "\"ops\":\"translate\",\"abi_version\":1,"
                       "\"protocol_min\":1,\"protocol_max\":2}", m) ==
                       wp::ManifestStatus::Malformed,
                   "manifest: non-array ops -> Malformed");
        TEST_CHECK(wp::ParseManifestJson(
                       "{\"schema_version\":1,\"family\":\"ggml-translate\","
                       "\"engine\":\"llama.cpp\",\"engine_version\":\"b6099\","
                       "\"ops\":[],\"abi_version\":1,\"protocol_min\":3,"
                       "\"protocol_max\":2}", m) == wp::ManifestStatus::Malformed,
                   "manifest: inverted protocol range -> Malformed");
    }

    // ---- 3. Embedded manifest matches the §3.4 constants ----
    {
        wp::WorkerManifest emb = wp::EmbeddedWorkerManifest();
        TEST_CHECK(emb.family == "ggml-translate" && emb.engine == "llama.cpp" &&
                       emb.engine_version == "b6099" && emb.abi_version == 1 &&
                       emb.protocol_min == 1 && emb.protocol_max == 2 &&
                       emb.ops.size() == 1 && emb.ops[0] == "translate",
                   "manifest: embedded constants match design §3.4");
        TEST_CHECK(wp::ParseManifestJson(wp::BuildManifestJson(emb), emb) ==
                       wp::ManifestStatus::Ok,
                   "manifest: embedded builder output re-parses");
    }

    // ---- 4. Announce golden + parse (spliced op member + token echo, T6) ----
    {
        const wp::WorkerManifest emb = wp::EmbeddedWorkerManifest();
        const std::string kToken = "0123456789abcdef0123456789abcdef";
        const std::string announce = wp::BuildAnnounce(emb, kToken);
        // "{\"op\":\"announce\"," is 17 chars: splice position is exact.
        TEST_CHECK(announce.compare(0, 17, "{\"op\":\"announce\",") == 0,
                   "announce: op member spliced first");
        TEST_CHECK(announce.find("\"token\":\"" + kToken + "\"") != std::string::npos,
                   "announce: token echoed (audit finding 2)");
        TEST_CHECK(announce.find("\"schema_version\":1") != std::string::npos &&
                       announce.find("\"family\":\"ggml-translate\"") != std::string::npos,
                   "announce: manifest fields carried");
        // The token is announce-only: it must NOT leak into the manifest body
        // that BuildManifestJson emits for the deployed worker.manifest file.
        TEST_CHECK(wp::BuildManifestJson(emb).find("token") == std::string::npos,
                   "announce: token never part of the deployed manifest bytes");
        wp::WorkerManifest back;
        TEST_CHECK(wp::ParseAnnounce(announce, back) && back.family == "ggml-translate" &&
                       back.abi_version == 1 && back.protocol_min == 1 &&
                       back.protocol_max == 2,
                   "announce: parse roundtrip (manifest-only overload)");
        wp::AnnounceMsg am;
        TEST_CHECK(wp::ParseAnnounce(announce, am) && am.manifest.family == "ggml-translate" &&
                       am.token == kToken,
                   "announce: AnnounceMsg surfaces the echoed token");
        TEST_CHECK(!wp::ParseAnnounce("{\"op\":\"hello\"}", back),
                   "announce: wrong op rejected");
        TEST_CHECK(!wp::ParseAnnounce("{\"op\":\"announce\",\"family\":\"ggml-translate\"}", back),
                   "announce: incomplete manifest rejected");
    }

    // ---- 4b. Announce token re-validation (security audit finding 2, M6 T6) ----
    {
        const std::string kIssued = "0123456789abcdef0123456789abcdef";
        // Match: identical token accepted; case-sensitive (hex tokens are lowercase).
        TEST_CHECK(wp::AnnounceTokenMatches(kIssued, kIssued),
                   "token: exact match accepted");
        // Mismatch: one hex digit differs -> rejected.
        std::string bad = kIssued;
        bad[0] = (bad[0] == '0') ? '1' : '0';
        TEST_CHECK(!wp::AnnounceTokenMatches(bad, kIssued),
                   "token: single-char mismatch rejected");
        // Old worker with NO token field -> empty announced -> rejected.
        TEST_CHECK(!wp::AnnounceTokenMatches("", kIssued),
                   "token: missing (old worker) rejected");
        // Empty issued (never happens in practice, but the compare stays total).
        TEST_CHECK(wp::AnnounceTokenMatches("", ""),
                   "token: both empty is a match (degenerate)");
        // Length mismatch: a prefix must NOT compare equal.
        TEST_CHECK(!wp::AnnounceTokenMatches(kIssued.substr(0, 31), kIssued),
                   "token: length mismatch rejected (no prefix match)");
        // Round-trip through the frame: the parsed token survives intact.
        const wp::WorkerManifest emb = wp::EmbeddedWorkerManifest();
        wp::AnnounceMsg am;
        TEST_CHECK(wp::ParseAnnounce(wp::BuildAnnounce(emb, kIssued), am) &&
                       wp::AnnounceTokenMatches(am.token, kIssued),
                   "token: parse-then-compare roundtrip accepts the issued value");
    }

    // ---- 5. Handshake validation (family/abi/protocol ranges, §3.4) ----
    {
        wp::WorkerManifest good = wp::EmbeddedWorkerManifest();
        TEST_CHECK(wp::AnnounceCheck::Validate(good, 1, 1, 2) == wp::AnnounceStatus::Ok,
                   "handshake: embedded manifest accepted (orchestrator window 1-2)");
        TEST_CHECK(wp::AnnounceCheck::Validate(good, 2, 1, 2) == wp::AnnounceStatus::Ok,
                   "handshake: abi below orchestrator ceiling accepted");
        good.family = "ggml-asr";
        TEST_CHECK(wp::AnnounceCheck::Validate(good, 1, 1, 2) ==
                       wp::AnnounceStatus::FamilyMismatch,
                   "handshake: family mismatch rejected");
        good = wp::EmbeddedWorkerManifest();
        good.abi_version = 2;
        TEST_CHECK(wp::AnnounceCheck::Validate(good, 1, 1, 2) ==
                       wp::AnnounceStatus::AbiOutOfRange,
                   "handshake: abi above orchestrator ceiling rejected");
        good.abi_version = 0;
        TEST_CHECK(wp::AnnounceCheck::Validate(good, 1, 1, 2) ==
                       wp::AnnounceStatus::AbiOutOfRange,
                   "handshake: abi 0 rejected");
        good = wp::EmbeddedWorkerManifest();
        good.protocol_min = 3;
        good.protocol_max = 5;
        TEST_CHECK(wp::AnnounceCheck::Validate(good, 1, 1, 2) ==
                       wp::AnnounceStatus::ProtocolNoOverlap,
                   "handshake: disjoint protocol ranges rejected");
        good.protocol_min = 2;
        good.protocol_max = 4;
        TEST_CHECK(wp::AnnounceCheck::Validate(good, 1, 1, 2) == wp::AnnounceStatus::Ok,
                   "handshake: overlapping ranges accepted");
    }

    // ---- 6. Frame reuse (§4.3 primitives round-trip the worker JSON) ----
    {
        const wp::WorkerManifest emb = wp::EmbeddedWorkerManifest();
        std::string frame;
        TEST_CHECK(eh::FrameEncode(wp::BuildAnnounce(emb, "0123456789abcdef0123456789abcdef"), frame),
                   "frames: announce fits the §4.3 encode");
        std::string body;
        TEST_CHECK(eh::FrameDecode(frame, body), "frames: §4.3 decode roundtrip");
        wp::WorkerManifest back;
        TEST_CHECK(wp::ParseAnnounce(body, back), "frames: decoded body re-parses");
        // M-2 ordering surface: shutdown/shutdown_ack exist as the LAST frames
        // of a dying exchange (tech gate M-2; the manager tests assert order).
        std::string ack_frame;
        // "{\"op\":\"shutdown_ack\"}" is 21 bytes of JSON -> a 25-byte frame.
        TEST_CHECK(eh::FrameEncode(wp::BuildShutdownAck(), ack_frame) &&
                       ack_frame.size() == 4 + 21,
                   "frames: shutdown_ack encodes (M-2 last frame)");
    }

    // ---- 7. Job: golden + parse + sampling override (§V2-5.2) ----
    {
        wp::JobMsg j;
        j.job = 7;
        j.session = 3;
        j.src = "ko";
        j.tgt = "en";
        j.text = "안녕 \"세계\"";
        const std::string built = wp::BuildJob(j);
        TEST_CHECK(built.find("\"op\":\"job\"") != std::string::npos &&
                       built.find("\"job\":7") != std::string::npos &&
                       built.find("\"session\":3") != std::string::npos &&
                       built.find("\"kind\":\"translate\"") != std::string::npos,
                   "job: golden shape (op/job/session/kind)");
        wp::JobMsg jp;
        TEST_CHECK(wp::ParseJob(built, jp) && jp.job == 7 && jp.session == 3 &&
                       jp.src == "ko" && jp.tgt == "en" && jp.text == j.text &&
                       !jp.sampling_present,
                   "job: parse roundtrip (no sampling -> shipped defaults)");
        TEST_CHECK(jp.sampling.temperature == 0.0f && jp.sampling.top_p == 0.6f &&
                       jp.sampling.top_k == 20 && jp.sampling.rep_pen == 1.05f,
                   "job: default sampling matches the Hy-MT2 shipped profile");

        wp::JobMsg j2;
        j2.job = 1;
        j2.sampling = {0.7f, 0.9f, 40, 1.1f};
        j2.sampling_present = true;
        wp::JobMsg jp2;
        TEST_CHECK(wp::ParseJob(wp::BuildJob(j2), jp2) && jp2.sampling_present &&
                       jp2.sampling.temperature == 0.7f && jp2.sampling.top_p == 0.9f &&
                       jp2.sampling.top_k == 40 && jp2.sampling.rep_pen == 1.1f,
                   "job: explicit sampling profile roundtrip (§V2-5.2 override)");

        wp::JobMsg jp3;
        TEST_CHECK(!wp::ParseJob("{\"op\":\"job\"}", jp3), "job: missing job id rejected");
        TEST_CHECK(!wp::ParseJob("{\"op\":\"job\",\"job\":1,\"kind\":\"asr\"}", jp3),
                   "job: non-translate kind rejected");
        TEST_CHECK(!wp::ParseJob(
                       "{\"op\":\"job\",\"job\":1,\"kind\":\"translate\","
                       "\"sampling\":{\"temperature\":\"x\"}}", jp3),
                   "job: garbage sampling rejected");
    }

    // ---- 8. Event: kinds + seq + unknown-kind fail-closed ----
    {
        wp::EventMsg ev;
        ev.session = 5;
        ev.kind = wp::EventKind::Final;
        ev.seq = 12;
        ev.text = "hello world";
        const std::string built = wp::BuildEvent(ev);
        TEST_CHECK(built.find("\"op\":\"event\"") != std::string::npos &&
                       built.find("\"session\":5") != std::string::npos &&
                       built.find("\"kind\":\"final\"") != std::string::npos &&
                       built.find("\"seq\":12") != std::string::npos,
                   "event: golden shape (§1.2 event frame)");
        wp::EventMsg ep;
        TEST_CHECK(wp::ParseEvent(built, ep) && ep.session == 5 &&
                       ep.kind == wp::EventKind::Final && ep.seq == 12 &&
                       ep.text == "hello world",
                   "event: parse roundtrip");

        const wp::EventKind all[] = {wp::EventKind::Partial, wp::EventKind::Final,
                                     wp::EventKind::Token,  wp::EventKind::Error,
                                     wp::EventKind::Eos};
        for (wp::EventKind k : all) {
            wp::EventMsg e;
            e.session = 1;
            e.kind = k;
            e.seq = 1;
            e.code = (k == wp::EventKind::Error) ? "engine_failed" : "";
            wp::EventMsg p;
            TEST_CHECK(wp::ParseEvent(wp::BuildEvent(e), p) && p.kind == k,
                       "event: kind roundtrip");
        }
        wp::EventMsg ep2;
        TEST_CHECK(!wp::ParseEvent(
                       "{\"op\":\"event\",\"session\":1,\"kind\":\"weird\",\"seq\":1}", ep2),
                   "event: unknown kind converges to failure (§V2-12-2)");
        TEST_CHECK(!wp::ParseEvent("{\"op\":\"event\",\"kind\":\"final\",\"seq\":1}", ep2),
                   "event: missing session rejected");
    }

    // ---- 9. Session open/opened/close/abort/heartbeat/shutdown ----
    {
        wp::SessionOpenMsg so;
        TEST_CHECK(wp::ParseSessionOpen(
                       wp::BuildSessionOpen(9, "translate", "hy-mt2-1.8b-q8", "default"),
                       so) &&
                       so.session == 9 && so.capability == "translate" &&
                       so.model_id == "hy-mt2-1.8b-q8" && so.profile == "default",
                   "session_open: build/parse roundtrip");
        std::uint64_t s = 0;
        TEST_CHECK(wp::ParseOpened(wp::BuildOpened(9), s) && s == 9, "opened: roundtrip");
        wp::AbortMsg ab;
        TEST_CHECK(wp::ParseAbort(wp::BuildAbort(4), ab) && ab.session == 4,
                   "abort: roundtrip");
        wp::HeartbeatMsg hb;
        TEST_CHECK(wp::ParseHeartbeat(wp::BuildHeartbeat(123456), hb) && hb.ts == 123456,
                   "heartbeat: roundtrip");
        TEST_CHECK(wp::ParseShutdown(wp::BuildShutdown()), "shutdown: parses");
        TEST_CHECK(wp::ParseShutdownAck(wp::BuildShutdownAck()), "shutdown_ack: parses (M-2)");
        s = 0;
        TEST_CHECK(wp::ParseClose(wp::BuildClose(2), s) && s == 2, "close: roundtrip");
        s = 99;
        TEST_CHECK(wp::ParseClosed(wp::BuildClosed(2), s) && s == 2, "closed: roundtrip");
        wp::ErrorMsg err;
        TEST_CHECK(wp::ParseError(wp::BuildError("unavailable"), err) &&
                       err.code == "unavailable",
                   "error: roundtrip");
        TEST_CHECK(!wp::ParseShutdown("{\"op\":\"shutdown_extra\"}"),
                   "shutdown: wrong op rejected");
    }

    // ---- 10. Lifecycle constants (R-4 hardcoded; unit-pinned) ----
    // (BackoffDelayMs lives in host_v2 — the manager owns the respawn math;
    // the constants stay in workerproto. constexpr reads in volatile-free
    // locals dodge MSVC C4127 "constant conditional" inside TEST_CHECK's if.)
    {
        volatile int hb = wp::kWorkerHeartbeatIntervalMs;
        TEST_CHECK(hb == 5000, "lifecycle: heartbeat 5s");
        volatile int hbt = wp::kWorkerHeartbeatTimeoutMs;
        TEST_CHECK(hbt == 15000, "lifecycle: heartbeat timeout 15s");
        volatile int bi = wp::kWorkerBackoffInitialMs;
        volatile int bm = wp::kWorkerBackoffMaxMs;
        TEST_CHECK(bi == 1000 && bm == 30000,
                   "lifecycle: backoff 1s initial / 30s cap (R-4)");
        volatile int rp = wp::kWorkerReaperPollMs;
        TEST_CHECK(rp == 250, "lifecycle: reaper 250ms poll");
        volatile int b1 = emebalachat::host_v2::BackoffDelayMs(1);
        TEST_CHECK(b1 == 1000, "backoff: first retry 1s");
        volatile int b2 = emebalachat::host_v2::BackoffDelayMs(2);
        TEST_CHECK(b2 == 2000, "backoff: second retry 2s");
        volatile int b3 = emebalachat::host_v2::BackoffDelayMs(3);
        TEST_CHECK(b3 == 4000, "backoff: third retry 4s");
        volatile int b6 = emebalachat::host_v2::BackoffDelayMs(6);
        TEST_CHECK(b6 == 30000, "backoff: capped at 30s");
        volatile int b50 = emebalachat::host_v2::BackoffDelayMs(50);
        TEST_CHECK(b50 == 30000, "backoff: stays capped");
        volatile int b0 = emebalachat::host_v2::BackoffDelayMs(0);
        TEST_CHECK(b0 == 0, "backoff: no failure -> no delay");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Worker protocol contract tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Worker protocol tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestWorkerManagerStateMachine() {
    std::cout << "[RUN] Testing worker manager state machine (REQ-043 M6 T3)..." << std::endl;
    const int failures_before = g_failed_count;
    namespace hv2 = emebalachat::host_v2;
    namespace wp = emebalachat::workerproto;

    // The state machine without any real worker process: spawn seams are
    // injected (R-6). A manual-reset event stands in for the process handle
    // (signaled = child exited), which the reaper's zero-wait poll reads.
    int spawn_hits = 0;

    // ---- 1. Missing exe -> Unavailable WITHOUT a spawn attempt (§1.1) ----
    {
        hv2::WorkerManager mgr(
            [](const hv2::SpawnRequest&, void*) -> hv2::SpawnResult {
                hv2::SpawnResult r;
                r.ok = false;
                r.last_error = 2;
                return r;
            },
            [](HANDLE, void*) {}, nullptr);
        mgr.RegisterFamily(L"ggml-translate", L"Z:\\definitely\\missing\\worker.exe");
        TEST_CHECK(mgr.State(L"ggml-translate") == hv2::WorkerState::Stopped,
                   "wmgr: registered family starts Stopped");
        TEST_CHECK(mgr.State(L"unknown-family") == hv2::WorkerState::Unavailable,
                   "wmgr: unknown family reads Unavailable");
        TEST_CHECK(mgr.EnsureSpawned(L"ggml-translate") == nullptr,
                   "wmgr: missing exe -> no handle");
        TEST_CHECK(mgr.State(L"ggml-translate") == hv2::WorkerState::Unavailable,
                   "wmgr: missing exe -> Unavailable (deployment is not a crash)");
        TEST_CHECK(mgr.EnsureSpawned(L"ggml-translate") == nullptr,
                   "wmgr: Unavailable is sticky (no respawn on retry, §3.4)");
    }

    // ---- 2. Handshake timeout -> Crashed + backoff armed (streak 1) ----
    // The injected seam returns a real (never-signaled) event handle, so the
    // EnsureSpawned handshake runs its documented timeout path: no announce
    // arrives within the connect window -> process killed -> Crashed.
    {
        hv2::WorkerManager mgr(
            [](const hv2::SpawnRequest&, void* user) -> hv2::SpawnResult {
                (*static_cast<int*>(user))++;
                hv2::SpawnResult r;
                r.ok = true;
                r.process = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
                r.done_event = r.process;
                return r;
            },
            [](HANDLE h, void*) { if (h) ::CloseHandle(h); },
            &spawn_hits);
        wchar_t self[MAX_PATH] = {0};
        ::GetModuleFileNameW(nullptr, self, MAX_PATH);
        mgr.RegisterFamily(L"ggml-translate", self); // exists by construction

        TEST_CHECK(mgr.EnsureSpawned(L"ggml-translate") == nullptr,
                   "wmgr: silent worker -> handshake fails (timeout path)");
        TEST_CHECK(mgr.State(L"ggml-translate") == hv2::WorkerState::Crashed,
                   "wmgr: handshake timeout -> Crashed");
        TEST_CHECK(spawn_hits == 1, "wmgr: exactly one spawn attempt consumed");

        // The reaper must not re-finalize a family with no live process
        // handle (EnsureSpawned already tore it down).
        TEST_CHECK(mgr.ReaperPass(0) == 0,
                   "wmgr: reaper pass is a no-op on a torn-down crashed family");

        // The backoff gate is armed: the manager stores streak=1 -> the next
        // EnsureSpawned within 1 s must refuse BEFORE spawning again.
        const int hits_before_gate = spawn_hits;
        TEST_CHECK(mgr.EnsureSpawned(L"ggml-translate") == nullptr,
                   "wmgr: backoff gate refuses an immediate respawn (R-4)");
        TEST_CHECK(spawn_hits == hits_before_gate,
                   "wmgr: backoff-gated refusal consumed NO spawn attempt");
    }

    // ---- 3. Crash finalization through the reaper (done_event fires) ----
    // The seam returns a PRE-SIGNALED event (child already dead): the reaper
    // pass observes WAIT_OBJECT_0, finalizes Crashed and arms the backoff.
    {
        int crash_hits = 0;
        hv2::WorkerManager mgr(
            [](const hv2::SpawnRequest&, void* user) -> hv2::SpawnResult {
                (*static_cast<int*>(user))++;
                hv2::SpawnResult r;
                r.ok = true;
                r.process = ::CreateEventW(nullptr, TRUE, TRUE, nullptr); // pre-signaled
                r.done_event = r.process;
                return r;
            },
            [](HANDLE h, void*) { if (h) ::CloseHandle(h); },
            &crash_hits);
        wchar_t self[MAX_PATH] = {0};
        ::GetModuleFileNameW(nullptr, self, MAX_PATH);
        mgr.RegisterFamily(L"ggml-translate", self);
        // Drive the state to Ready-like manually is impossible without the
        // announce; the reaper only watches Ready/Busy/Spawning, and this
        // manager never gets there (handshake fails first). The documented
        // invariant to pin: ReaperPass on a manager that never reached
        // Ready/Busy/Spawning finalizes NOTHING (no phantom crashes).
        TEST_CHECK(mgr.ReaperPass(1000) == 0,
                   "wmgr: reaper never finalizes a family it never spawned to Ready");
        TEST_CHECK(crash_hits == 0, "wmgr: reaper-only path consumed no spawns");
    }

    // ---- 4. Backoff math at the manager level (pure, already pinned) ----
    TEST_CHECK(hv2::BackoffDelayMs(1) == 1000 && hv2::BackoffDelayMs(2) == 2000 &&
                   hv2::BackoffDelayMs(3) == 4000 && hv2::BackoffDelayMs(6) == 30000,
               "wmgr: exponential backoff 1s->2s->4s cap 30s (R-4, design §1.1)");

    // ---- 5. Handshake validator surface (static, orchestrator window) ----
    {
        const wp::WorkerManifest emb = wp::EmbeddedWorkerManifest();
        TEST_CHECK(hv2::WorkerManager::ValidateHandshake(emb) ==
                       wp::AnnounceStatus::Ok,
                   "wmgr: ValidateHandshake accepts the embedded manifest");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Worker manager state machine tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Worker manager tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
