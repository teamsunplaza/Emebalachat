// P2-1 (session 260925_0001): family-coexistence asr relay suites. The
// stream relay itself lives in host_main.cpp (EmebalaEngine target, not
// linked into run_tests), so these suites pin the PURE contract pieces the
// relay is built on:
//   * the registered-family announce gate (ggml-asr accepted, foreign
//     families rejected) — worker_protocol.hpp P2-1 edit;
//   * the §7.3 W-PCM-1 pcm namespace port (feed_audio META, binary frame
//     codec, the META->binary lockstep predicates, W-FLUSH-1);
//   * the session_close -> close / cancel -> abort relay mapping bytes;
//   * the exe-adjacent worker.<family>.manifest family extraction
//     (engine_host_paths.hpp P2-1 helper) that drives the enumeration
//     registration in host_main.cpp.
// Included directly by tests/run_tests.cpp (the .inc staging contract).

void TestP21AsrAnnounce() {
    std::cout << "[RUN] Testing P2-1 asr announce gate (family coexistence)..." << std::endl;
    const int failures_before = g_failed_count;
    namespace wp = emebalachat::workerproto;
    namespace hv2 = emebalachat::host_v2;

    // ---- 1. Registered family set (P2-1) ----
    TEST_CHECK(wp::IsRegisteredWorkerFamily("ggml-translate"),
               "p2-1 families: ggml-translate registered (M6 baseline)");
    TEST_CHECK(wp::IsRegisteredWorkerFamily("ggml-asr"),
               "p2-1 families: ggml-asr registered (P2-1 addition)");
    TEST_CHECK(!wp::IsRegisteredWorkerFamily("ggml-tts"),
               "p2-1 families: a foreign family is not registered");
    TEST_CHECK(!wp::IsRegisteredWorkerFamily(""),
               "p2-1 families: empty family is not registered");
    TEST_CHECK(!wp::IsRegisteredWorkerFamily("ggml-asr "),
               "p2-1 families: near-miss spelling is not registered");
    volatile std::size_t fam_count = sizeof(wp::kWorkerFamiliesRegistered) /
                                     sizeof(wp::kWorkerFamiliesRegistered[0]);
    TEST_CHECK(fam_count == 2, "p2-1 families: exactly two registered families");

    // ---- 2. The ggml-asr announce is accepted end to end ----
    // The Listener-authored worker manifest (Emebala_Listner
    // worker_manifest_ggml_asr.json, deployed as worker.ggml-asr.manifest).
    {
        wp::WorkerManifest m;
        TEST_CHECK(wp::ParseManifestJson(
                       "{\"schema_version\":1,\"family\":\"ggml-asr\","
                       "\"engine\":\"transcribe.cpp\",\"engine_version\":\"83a75c1\","
                       "\"ops\":[\"asr\"],\"abi_version\":1,"
                       "\"protocol_min\":1,\"protocol_max\":2}", m) == wp::ManifestStatus::Ok,
                   "p2-1 announce: the ggml-asr manifest parses");
        TEST_CHECK(m.family == "ggml-asr" && m.ops.size() == 1 && m.ops[0] == "asr",
                   "p2-1 announce: family/ops parsed");
        TEST_CHECK(wp::AnnounceCheck::Validate(m, wp::kWorkerAbiVersion,
                                               wp::kWorkerProtocolMin,
                                               wp::kWorkerProtocolMax) == wp::AnnounceStatus::Ok,
                   "p2-1 announce: ggml-asr passes the orchestrator gate");
        TEST_CHECK(hv2::WorkerManager::ValidateHandshake(m) == wp::AnnounceStatus::Ok,
                   "p2-1 announce: WorkerManager::ValidateHandshake accepts ggml-asr");
        // The announce round trip (token echo, audit finding 2) holds for the
        // asr family exactly like the translate family.
        wp::AnnounceMsg am;
        TEST_CHECK(wp::ParseAnnounce(wp::BuildAnnounce(m, "0123456789abcdef0123456789abcdef"),
                                     am) &&
                       am.manifest.family == "ggml-asr" &&
                       wp::AnnounceTokenMatches(am.token, "0123456789abcdef0123456789abcdef"),
                   "p2-1 announce: asr announce + token echo roundtrip");
        // abi/protocol gates apply unchanged on the asr family.
        wp::WorkerManifest bad_abi = m;
        bad_abi.abi_version = wp::kWorkerAbiVersion + 1;
        TEST_CHECK(wp::AnnounceCheck::Validate(bad_abi, wp::kWorkerAbiVersion,
                                               wp::kWorkerProtocolMin,
                                               wp::kWorkerProtocolMax) ==
                       wp::AnnounceStatus::AbiOutOfRange,
                   "p2-1 announce: asr abi above the ceiling rejected");
        wp::WorkerManifest bad_proto = m;
        bad_proto.protocol_min = 3;
        bad_proto.protocol_max = 5;
        TEST_CHECK(wp::AnnounceCheck::Validate(bad_proto, wp::kWorkerAbiVersion,
                                               wp::kWorkerProtocolMin,
                                               wp::kWorkerProtocolMax) ==
                       wp::AnnounceStatus::ProtocolNoOverlap,
                   "p2-1 announce: asr disjoint protocol window rejected");
        // The translate embedded manifest still validates (no regression).
        TEST_CHECK(hv2::WorkerManager::ValidateHandshake(wp::EmbeddedWorkerManifest()) ==
                       wp::AnnounceStatus::Ok,
                   "p2-1 announce: the translate embedded manifest still accepted");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-1 asr announce gate tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-1 asr announce tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestP21AsrPcmClause() {
    std::cout << "[RUN] Testing P2-1 §7.3 W-PCM-1 pcm clause..." << std::endl;
    const int failures_before = g_failed_count;
    namespace wp = emebalachat::workerproto;
    namespace eh = emebalachat::enginehost;
    namespace pcm = emebalachat::workerproto::pcm;

    // ---- 1. Format + chunk constants (port pins) ----
    {
        TEST_CHECK(pcm::kFeedAudioFormat == "pcm_s16le_16k_mono",
                   "pcm: the feed format is pcm_s16le_16k_mono");
        volatile std::uint32_t chunk = pcm::kMaxPcmChunkBytes;
        TEST_CHECK(chunk == 65536, "pcm: the chunk cap is 64 KiB");
    }

    // ---- 2. feed_audio META build/parse roundtrip ----
    {
        const std::string meta = pcm::BuildFeedAudioMeta(42, 7, pcm::kFeedAudioFormat);
        TEST_CHECK(meta.find("\"op\":\"feed_audio\"") != std::string::npos &&
                       meta.find("\"session\":42") != std::string::npos &&
                       meta.find("\"seq\":7") != std::string::npos &&
                       meta.find("\"format\":\"pcm_s16le_16k_mono\"") != std::string::npos,
                   "pcm: META golden shape");
        pcm::FeedAudioMsg m;
        TEST_CHECK(pcm::ParseFeedAudioMeta(meta, m) && m.session == 42 && m.seq == 7 &&
                       m.format == "pcm_s16le_16k_mono",
                   "pcm: META parse roundtrip");
        // Fail-closed pins (the relay's bad_request predicate).
        pcm::FeedAudioMsg bad;
        TEST_CHECK(!pcm::ParseFeedAudioMeta("{\"op\":\"job\"}", bad),
                   "pcm: wrong op rejected");
        TEST_CHECK(!pcm::ParseFeedAudioMeta("{\"op\":\"feed_audio\",\"seq\":1}", bad),
                   "pcm: missing session rejected");
        TEST_CHECK(!pcm::ParseFeedAudioMeta(
                       "{\"op\":\"feed_audio\",\"session\":1,\"seq\":1,\"format\":5}", bad),
                   "pcm: non-string format rejected");
        TEST_CHECK(!pcm::ParseFeedAudioMeta("garbage", bad), "pcm: garbage rejected");
    }

    // ---- 3. Binary frame codec (JSON + PCM payloads) ----
    {
        std::vector<std::uint8_t> pcm(pcm::kMaxPcmChunkBytes, 0xAB); // one full chunk
        std::vector<std::uint8_t> wire;
        TEST_CHECK(pcm::EncodeBinaryFrame(pcm, wire),
                   "pcm: a full 64 KiB chunk encodes");
        TEST_CHECK(wire.size() == eh::kFrameHeaderSize + pcm.size(),
                   "pcm: encoded size is header + payload");
        std::vector<std::uint8_t> back;
        TEST_CHECK(pcm::DecodeFrame(wire, back) && back == pcm,
                   "pcm: 64 KiB chunk decodes byte-identical");
        // Over-cap refused.
        std::vector<std::uint8_t> huge(eh::kMaxFrameBytes + 1, 0);
        std::vector<std::uint8_t> wire_huge;
        TEST_CHECK(!pcm::EncodeBinaryFrame(huge, wire_huge),
                   "pcm: over-1MiB payload refused");
        // Truncated/garbage frames refused.
        std::vector<std::uint8_t> truncated(wire.begin(), wire.begin() + 100);
        std::vector<std::uint8_t> scratch;
        TEST_CHECK(!pcm::DecodeFrame(truncated, scratch),
                   "pcm: truncated frame refused");
    }

    // ---- 4. W-FLUSH-1: the zero-length binary frame ----
    {
        // The flush rides ONE META + ONE zero-length binary frame.
        std::vector<std::uint8_t> flush_frame;
        TEST_CHECK(pcm::EncodeBinaryFrame(nullptr, 0, flush_frame) &&
                       flush_frame.size() == eh::kFrameHeaderSize,
                   "pcm: W-FLUSH-1 encodes as a bare 4-byte header");
        std::uint32_t len = 1;
        TEST_CHECK(eh::FrameReadLengthPrefix(
                       reinterpret_cast<const char*>(flush_frame.data()), len) &&
                       len == 0,
                   "pcm: W-FLUSH-1 declares a zero payload");
        std::vector<std::uint8_t> payload;
        TEST_CHECK(pcm::DecodeFrame(flush_frame, payload) && payload.empty(),
                   "pcm: W-FLUSH-1 decodes to an empty payload");
        TEST_CHECK(pcm::IsFlushMarker(payload), "pcm: empty payload IS the flush marker");
        std::vector<std::uint8_t> nonempty(2, 0);
        TEST_CHECK(!pcm::IsFlushMarker(nonempty),
                   "pcm: a non-empty payload is NOT a flush marker");
        // The full lockstep pair on the wire: META frame + binary frame.
        const std::string meta = pcm::BuildFeedAudioMeta(9, 3, pcm::kFeedAudioFormat);
        std::string meta_wire;
        TEST_CHECK(eh::FrameEncode(meta, meta_wire), "pcm: META fits the §4.3 frame");
        TEST_CHECK(meta_wire.size() == eh::kFrameHeaderSize + meta.size() &&
                       flush_frame.size() == eh::kFrameHeaderSize,
                   "pcm: W-FLUSH-1 is exactly two frames (META + 4-byte binary)");
    }

    // ---- 5. The lockstep sniff predicates ----
    {
        TEST_CHECK(pcm::LooksLikeJsonObject(pcm::BuildFeedAudioMeta(1, 1, pcm::kFeedAudioFormat)),
                   "pcm: a META frame LooksLikeJsonObject");
        const std::vector<std::uint8_t> pcm_payload = {0x01, 0x02, 0x03, 0x04};
        TEST_CHECK(!pcm::LooksLikeJsonObject(std::string_view(
                       reinterpret_cast<const char*>(pcm_payload.data()),
                       pcm_payload.size())),
                   "pcm: a raw PCM payload does NOT LookLikeJsonObject");
        TEST_CHECK(!pcm::LooksLikeJsonObject(""), "pcm: empty payload is not JSON");
    }

    // ---- 6. session_close -> close / cancel -> abort relay mapping ----
    // The exact bytes the relay writes to the worker for the client's
    // session-level ops (W-FLUSH-1: close is the session END, never a flush).
    {
        std::uint64_t s = 0;
        const std::string close_frame = wp::BuildClose(42);
        TEST_CHECK(close_frame == "{\"op\":\"close\",\"session\":42}",
                   "p2-1 relay: session_close maps to the worker close bytes");
        TEST_CHECK(wp::ParseClose(close_frame, s) && s == 42,
                   "p2-1 relay: close roundtrip");
        const std::string abort_frame = wp::BuildAbort(42);
        TEST_CHECK(abort_frame == "{\"op\":\"abort\",\"session\":42}",
                   "p2-1 relay: cancel maps to the worker abort bytes (W-ABORT-1)");
        wp::AbortMsg ab;
        TEST_CHECK(wp::ParseAbort(abort_frame, ab) && ab.session == 42,
                   "p2-1 relay: abort roundtrip");
        // The worker's closed answer (relayed back to the client verbatim).
        TEST_CHECK(wp::ParseClosed(wp::BuildClosed(42), s) && s == 42,
                   "p2-1 relay: closed roundtrip (the relayed session end)");
    }

    // ---- 7. The Listener's asr session_open parses through the relay gate ----
    // The relay forwards the ORIGINAL frame; the only orchestrator-side parse
    // is this gate (capability check). The nested options must survive
    // untouched (the worker reads model_id/file/lang_hint from either level).
    {
        const std::string listener_open =
            "{\"op\":\"session_open\",\"session\":7,\"capability\":\"asr\","
            "\"model_id\":\"nemotron-3.5-asr-streaming-0.6b-q8\","
            "\"options\":{\"lang_hint\":\"auto\",\"partial_on\":true,"
            "\"reserved\":true,\"drop_eligible\":false}}";
        wp::SessionOpenMsg m;
        TEST_CHECK(wp::ParseSessionOpen(listener_open, m) && m.session == 7 &&
                       m.capability == "asr" &&
                       m.model_id == "nemotron-3.5-asr-streaming-0.6b-q8",
                   "p2-1 relay: the Listener asr session_open passes the gate");
    }

    // ---- 8. Manifest-name family extraction (the enumeration driver) ----
    {
        namespace paths = emebalachat::enginehost::paths;
        TEST_CHECK(paths::WorkerFamilyFromManifestName(L"worker.ggml-asr.manifest") ==
                       L"ggml-asr",
                   "p2-1 enum: worker.ggml-asr.manifest -> ggml-asr");
        TEST_CHECK(paths::WorkerFamilyFromManifestName(L"worker.ggml-translate.manifest") ==
                       L"ggml-translate",
                   "p2-1 enum: worker.ggml-translate.manifest -> ggml-translate");
        TEST_CHECK(paths::WorkerFamilyFromManifestName(L"worker.manifest").empty(),
                   "p2-1 enum: the legacy bare manifest has no family");
        TEST_CHECK(paths::WorkerFamilyFromManifestName(L"worker..manifest").empty(),
                   "p2-1 enum: an empty family segment yields empty");
        TEST_CHECK(paths::WorkerFamilyFromManifestName(L"random.exe").empty(),
                   "p2-1 enum: a non-manifest name yields empty");
        TEST_CHECK(paths::WorkerFamilyFromManifestName(L"worker.foo.manifest") == L"foo",
                   "p2-1 enum: any middle segment is the family");
        // Round trip with the builder.
        TEST_CHECK(paths::WorkerFamilyFromManifestName(
                       paths::WorkerManifestName(L"ggml-asr")) == L"ggml-asr",
                   "p2-1 enum: build-then-extract roundtrip");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-1 §7.3 pcm clause tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-1 pcm clause tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// P2-1 stabilization (session 260925_0001, live-test defects 2/3): the asr
// relay claim lifecycle. The relay itself lives in host_main.cpp (not linked
// into run_tests), so these suites drive the PURE state machine it is built
// on (host_v2_asr_relay.hpp): claim -> disconnect/worker-death -> release ->
// reopen, plus the SessionTable recovery that outlives the claim.
void TestP21AsrRelayLifecycle() {
    std::cout << "[RUN] Testing P2-1 asr relay claim lifecycle (zombie/reopen hardening)..."
              << std::endl;
    const int failures_before = g_failed_count;
    namespace hv2 = emebalachat::host_v2;
    using Claim = hv2::AsrRelayClaim;

    // Stand-in connection identities (the pure claim never dereferences).
    int conn_a = 0, conn_b = 0;

    // ---- 1. The single slot: the racing second open fails fast ----
    {
        Claim c;
        TEST_CHECK(c.TryClaim(&conn_a, 7, 101) && c.active && !c.stop,
                   "claim: the first open takes the slot");
        TEST_CHECK(!c.TryClaim(&conn_b, 8, 102),
                   "claim: a racing second open is refused (max_sessions=1 parity)");
        TEST_CHECK(c.IsLiveFor(&conn_a) && c.IsLiveFor(&conn_a, 7) &&
                       !c.IsLiveFor(&conn_b) && !c.IsLiveFor(&conn_a, 8),
                   "claim: the live gates key on owner + wire session");
        const Claim::TeardownPlan foreign = c.PlanTeardown(&conn_b);
        TEST_CHECK(!foreign.ours,
                   "claim: a foreign connection's teardown leaves the slot alone");
    }

    // ---- 2. Zombie repro (defect 2): claim held, the connection TEARS DOWN
    // (abnormal disconnect / a protocol-error exit — no session_close ever
    // arrives). The teardown plan must order the worker close, and once the
    // caller applies it the slot MUST be free for the next client — the
    // live-test zombie wedged exactly here.
    {
        Claim c;
        c.TryClaim(&conn_a, 7, 101);
        const Claim::TeardownPlan p = c.PlanTeardown(&conn_a);
        TEST_CHECK(p.ours && p.order_worker_close && p.wire_session == 7 && p.table_id == 101,
                   "teardown: the owner's disconnect orders the worker close + table close");
        TEST_CHECK(c.active && c.stop,
                   "teardown: stop is ordered so the relay thread exits");
        // Caller-side application: the worker close is ordered, the relay
        // thread drains on stop (its LAST act self-releases the slot), then
        // the record disowns.
        c.ReleaseByRelay();
        c.Disown();
        TEST_CHECK(c.TryClaim(&conn_b, 9, 103),
                   "teardown: after the plan the slot is FREE for the next client");
    }

    // ---- 3. Worker death mid-stream (defect 3): the relay self-releases ----
    // (the synthesized engine_failed answer itself is pinned in host_main.cpp)
    // and the slot is reclaimable IMMEDIATELY — the next open must not wait.
    {
        Claim c;
        c.TryClaim(&conn_a, 7, 101);
        c.ReleaseByRelay(); // the relay's LAST act
        TEST_CHECK(!c.active, "reopen: the relay self-release frees the slot");
        TEST_CHECK(c.TryClaim(&conn_b, 9, 103),
                   "reopen: a new client claims right after a worker death");
        const Claim::TeardownPlan pb = c.PlanTeardown(&conn_b);
        TEST_CHECK(pb.ours && pb.order_worker_close && pb.table_id == 103,
                   "reopen: the new owner's teardown orders its own session close");
        c.ReleaseByRelay();
        c.Disown();
        // The OLD owner is gone for good: after the handoff it owns nothing.
        const Claim::TeardownPlan pa = c.PlanTeardown(&conn_a);
        TEST_CHECK(!pa.ours,
                   "reopen: after the handoff the old owner recovers nothing");
    }

    // ---- 4. Recovery: the SessionTable record OUTLIVES the claim ----
    // (relay self-released, THEN the owner disconnects; also the reused
    // connection object must not recover stale ids after its own teardown).
    {
        Claim c;
        c.TryClaim(&conn_a, 7, 101);
        c.ReleaseByRelay();
        const Claim::TeardownPlan p = c.PlanTeardown(&conn_a);
        TEST_CHECK(p.ours && !p.order_worker_close && p.table_id == 101,
                   "recovery: the owner's late teardown still closes the table record");
        c.Disown();
        TEST_CHECK(c.TryClaim(&conn_b, 9, 103), "recovery: the slot frees for reuse");
        // The connection OBJECT is reused for the new client: its own
        // teardown plans the NEW session, exactly once.
        const Claim::TeardownPlan pb = c.PlanTeardown(&conn_b);
        TEST_CHECK(pb.ours && pb.order_worker_close && pb.table_id == 103,
                   "recovery: the reused object's teardown plans its OWN session");
        c.ReleaseByRelay();
        c.Disown();
        const Claim::TeardownPlan p2 = c.PlanTeardown(&conn_b);
        TEST_CHECK(!p2.ours,
                   "recovery: teardown is idempotent (no double table close)");
    }

    // ---- 5. The client's session_close (PlanSessionEnd) ----
    {
        Claim c;
        c.TryClaim(&conn_a, 7, 101);
        const Claim::TeardownPlan wrong = c.PlanSessionEnd(&conn_a, 8);
        TEST_CHECK(!wrong.ours, "close: a foreign session id falls through");
        const Claim::TeardownPlan p = c.PlanSessionEnd(&conn_a, 7);
        TEST_CHECK(p.ours && p.order_worker_close && p.wire_session == 7 &&
                       p.table_id == 101 && c.stop,
                   "close: the matching live session orders the worker close");
        // Caller-side application: the relay drains on stop (self-release),
        // then the slot disowns.
        c.ReleaseByRelay();
        c.Disown();
        TEST_CHECK(c.TryClaim(&conn_b, 9, 103),
                   "close: the slot frees immediately after the client close");
    }

    // ---- 6. The open-failure unwind (ReleaseByOwner) ----
    {
        Claim c;
        c.TryClaim(&conn_a, 7, 101);
        TEST_CHECK(!c.ReleaseByOwner(&conn_b, 7),
                   "unwind: a foreign owner cannot release the slot");
        TEST_CHECK(!c.ReleaseByOwner(&conn_a, 8),
                   "unwind: a stale session id cannot release the slot");
        TEST_CHECK(c.ReleaseByOwner(&conn_a, 7),
                   "unwind: the matching owner releases after a failed open");
        TEST_CHECK(!c.active && c.conn == nullptr && c.table_id == 0,
                   "unwind: the owner fields are fully cleared");
        const Claim::TeardownPlan p = c.PlanTeardown(&conn_a);
        TEST_CHECK(!p.ours,
                   "unwind: no teardown recovery may see the failed open");
    }

    // ---- 7. The Listener retry: same connection, same pinned session id ----
    // (the client pins the wire id across engine_failed reconnects; the new
    // host table record must be the one the teardown closes).
    {
        Claim c;
        c.TryClaim(&conn_a, 7, 101);
        c.ReleaseByRelay();
        TEST_CHECK(c.TryClaim(&conn_a, 7, 104),
                   "retry: the same owner reclaims the pinned session id");
        const Claim::TeardownPlan p = c.PlanTeardown(&conn_a);
        TEST_CHECK(p.ours && p.order_worker_close && p.wire_session == 7 &&
                       p.table_id == 104,
                   "retry: the NEW record is the one the teardown closes");
        c.ReleaseByRelay();
        c.Disown();
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-1 asr relay claim lifecycle tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-1 asr relay lifecycle tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// P2-1 stabilization (session 260925_0001, live-test defect 1): the asr
// open cold-start wait. EnsureSpawned can answer nullptr FAST while the
// family is backoff-gated (Crashed) — the pre-fix first open converted that
// into an instant "unavailable". The classifier decides whether the open
// retries inside its ONE 120 s deadline.
void TestP21AsrColdStartWait() {
    std::cout << "[RUN] Testing P2-1 asr open cold-start wait classifier..." << std::endl;
    const int failures_before = g_failed_count;
    namespace hv2 = emebalachat::host_v2;

    const std::int64_t now = 100000;        // arbitrary clock snapshot
    const std::int64_t deadline = now + 120000; // kAsrOpenTimeoutMs

    // Transient states retry inside the deadline — Crashed is THE live
    // cold-start shape (the backoff gate defers the respawn a few seconds).
    TEST_CHECK(hv2::AsrSpawnRetryWarranted(hv2::WorkerState::Crashed, now, deadline),
               "cold-start: a backoff-gated respawn retries, not unavailable");
    TEST_CHECK(hv2::AsrSpawnRetryWarranted(hv2::WorkerState::Stopped, now, deadline),
               "cold-start: an unspawned family retries");
    TEST_CHECK(hv2::AsrSpawnRetryWarranted(hv2::WorkerState::Spawning, now, deadline),
               "cold-start: a spawn still in flight retries");
    TEST_CHECK(hv2::AsrSpawnRetryWarranted(hv2::WorkerState::Ready, now, deadline),
               "cold-start: a transient Ready observation retries once");
    TEST_CHECK(hv2::AsrSpawnRetryWarranted(hv2::WorkerState::Busy, now, deadline),
               "cold-start: a transient Busy observation retries once");
    // Unavailable is PERMANENT (missing exe / announce mismatch — a retry
    // cannot fix a wrong deployment): the open fails fast.
    TEST_CHECK(!hv2::AsrSpawnRetryWarranted(hv2::WorkerState::Unavailable, now, deadline),
               "cold-start: a wrong deployment fails fast");
    // The deadline bounds everything: a wedged spawn cannot pin the open.
    TEST_CHECK(!hv2::AsrSpawnRetryWarranted(hv2::WorkerState::Crashed, deadline, deadline),
               "cold-start: at the deadline the wait ends");
    TEST_CHECK(!hv2::AsrSpawnRetryWarranted(hv2::WorkerState::Stopped, deadline + 1, deadline),
               "cold-start: past the deadline the wait ends");
    TEST_CHECK(!hv2::AsrSpawnRetryWarranted(hv2::WorkerState::Unavailable, deadline + 1, deadline),
               "cold-start: past the deadline even a retry-worthy state ends");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-1 asr cold-start wait tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-1 asr cold-start wait tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// P2-1 stabilization (session 260925_0001, live defect: persistent
// `unavailable` under sustained load): the stalled-worker recovery chain.
// Live evidence: the CPU-bound asr worker stopped draining mid-stream; the
// 1 MiB orchestrator->worker pipe backed up; the bounded write wait fired;
// the family was marked Crashed while the STUCK WORKER PROCESS stayed alive
// holding the per-family single-instance mutex — every backoff respawn then
// exited on that mutex (connect timeout) and the family wedged (`unavailable`
// forever). The fix: a STALLED write kills the worker (mutex frees, reaper
// finalizes, backoff respawn produces a healthy worker); a BROKEN pipe needs
// no kill. These checks pin the pure decision and the post-kill reopen chain.
void TestP21AsrStallRecovery() {
    std::cout << "[RUN] Testing P2-1 asr stalled-worker recovery decision..." << std::endl;
    const int failures_before = g_failed_count;
    namespace hv2 = emebalachat::host_v2;
    using Claim = hv2::AsrRelayClaim;

    // ---- 1. The kill decision ----
    TEST_CHECK(!hv2::WorkerWriteFailureNeedsKill(hv2::FrameWriteResult::Ok),
               "stall: an OK write never needs a kill");
    TEST_CHECK(!hv2::WorkerWriteFailureNeedsKill(hv2::FrameWriteResult::Broken),
               "stall: a BROKEN pipe needs no kill (the peer is already exiting)");
    TEST_CHECK(hv2::WorkerWriteFailureNeedsKill(hv2::FrameWriteResult::Stalled),
               "stall: a STALLED peer must be killed (frees the worker mutex)");

    // ---- 2. The post-kill reopen chain (pure composition) ----
    // Orchestrator view after the kill: family Crashed + backoff pending, and
    // the client reconnects. The claim slot must be free (the loop-end
    // teardown released it) and the spawn wait must retry the Crashed family
    // inside the open deadline instead of failing fast.
    {
        int dead_conn = 0, new_conn = 0;
        Claim c;
        c.TryClaim(&dead_conn, 7, 101);
        // The feed write stalled -> teardown plan -> worker killed -> the
        // connection ends; the slot frees for the reconnecting client.
        const Claim::TeardownPlan p = c.PlanTeardown(&dead_conn);
        TEST_CHECK(p.ours && p.order_worker_close,
                   "stall: the stalled session's teardown orders the worker close");
        c.ReleaseByRelay();
        c.Disown();
        TEST_CHECK(c.TryClaim(&new_conn, 9, 103),
                   "stall: the reconnecting client claims the freed slot");
        const int64_t now = 50000;
        const int64_t deadline = now + 120000;
        TEST_CHECK(hv2::AsrSpawnRetryWarranted(hv2::WorkerState::Crashed, now, deadline),
                   "stall: the open waits through the Crashed backoff gate");
        const Claim::TeardownPlan p2 = c.PlanTeardown(&new_conn);
        TEST_CHECK(p2.ours && p2.order_worker_close,
                   "stall: the reopened session serves normally");
        c.ReleaseByRelay();
        c.Disown();
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-1 asr stalled-worker recovery tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-1 asr stalled-worker recovery tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
