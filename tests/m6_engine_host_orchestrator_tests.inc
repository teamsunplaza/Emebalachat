// REQ-043 (M6 T4): orchestrator-v2 unit tests — scheduler pure logic, session
// table lifecycle, health ratio math, and the v2 welcome golden shape.
// Included directly by tests/run_tests.cpp (the T2/T3 session .inc staging
// contract; they use the TEST_CHECK harness + the std:: facilities included
// above). No process, no pipe, no clock wall-time dependence (R-6).

void TestHostV2Scheduler() {
    std::cout << "[RUN] Testing host_v2 scheduler (REQ-043 M6 T4)..." << std::endl;
    const int failures_before = g_failed_count;
    using namespace emebalachat::host_v2;
    namespace eh = emebalachat::enginehost;

    // ---- 1. Depth budget (v1 §4.4's 8 inherited) + overflow busy ----
    {
        SchedQueue q; // default depth 8
        TEST_CHECK(q.Depth() == eh::kMaxQueueDepth, "sched: default depth inherits v1 §4.4's 8");
        int enqueued = 0;
        SchedItem evicted;
        for (int i = 0; i < 8; ++i) {
            SchedItem it;
            it.priority = 5;
            if (q.TryEnqueue(it, static_cast<std::uint64_t>(i + 1), evicted) ==
                EnqueueResult::Enqueued) {
                ++enqueued;
            }
        }
        TEST_CHECK(enqueued == 8 && q.Size() == 8, "sched: 8 items fill the finite queue");
        SchedItem overflow;
        overflow.priority = 5;
        TEST_CHECK(q.TryEnqueue(overflow, 99, evicted) == EnqueueResult::BusyOverflow,
                   "sched: depth 8 overflow -> busy WITHOUT enqueueing");
        TEST_CHECK(q.Size() == 8, "sched: overflow did not grow the queue");
    }

    // ---- 2. Priority ASC then arrival FIFO (§V2-4.6 key) ----
    {
        SchedQueue q;
        SchedItem evicted;
        // Enqueue out of order: priorities 5,1,9,1 — expect pop order 1,1,5,9.
        const int prios[] = {5, 1, 9, 1};
        for (int i = 0; i < 4; ++i) {
            SchedItem it;
            it.priority = prios[i];
            TEST_CHECK(q.TryEnqueue(it, static_cast<std::uint64_t>(i + 1), evicted) ==
                           EnqueueResult::Enqueued,
                       "sched: admission while below depth");
        }
        const int expected[] = {1, 1, 5, 9};
        for (int i = 0; i < 4; ++i) {
            SchedItem out, exp;
            bool had = false;
            TEST_CHECK(q.Pop(out, 0, exp, had) && !had && out.priority == expected[i],
                       "sched: (priority ASC, arrival ASC) pop order");
        }
        SchedItem out2, exp2;
        bool had2 = false;
        TEST_CHECK(!q.Pop(out2, 0, exp2, had2), "sched: drained queue pops false");
    }

    // ---- 3. drop_eligible: the OLDEST eligible is evicted, reported (§V2-4.6) ----
    {
        SchedQueue q(2);
        SchedItem evicted;
        SchedItem a; a.priority = 5; a.drop_eligible = true;  // seq 1
        SchedItem b; b.priority = 5; b.drop_eligible = true;  // seq 2
        TEST_CHECK(q.TryEnqueue(a, 1, evicted) == EnqueueResult::Enqueued, "sched/drop: a in");
        TEST_CHECK(q.TryEnqueue(b, 2, evicted) == EnqueueResult::Enqueued, "sched/drop: b in");
        SchedItem c; c.priority = 5; c.drop_eligible = true;  // seq 3 evicts a (oldest)
        TEST_CHECK(q.TryEnqueue(c, 3, evicted) == EnqueueResult::BusyDroppedOld,
                   "sched/drop: saturated eligible insert evicts the oldest");
        TEST_CHECK(evicted.enqueue_seq == 1,
                   "sched/drop: the EVICTED (oldest eligible) item is reported for busy");
        // Remaining: b (2) then c (3).
        SchedItem out, exp; bool had = false;
        TEST_CHECK(q.Pop(out, 0, exp, had) && out.enqueue_seq == 2,
                   "sched/drop: resident b survives");
        TEST_CHECK(q.Pop(out, 0, exp, had) && out.enqueue_seq == 3,
                   "sched/drop: newcomer c took the slot");
    }

    // ---- 4. drop_eligible cannot preempt non-eligible saturation ----
    {
        SchedQueue q(2);
        SchedItem evicted;
        SchedItem a; a.priority = 5; a.drop_eligible = false;
        SchedItem b; b.priority = 5; b.drop_eligible = false;
        (void)q.TryEnqueue(a, 1, evicted);
        (void)q.TryEnqueue(b, 2, evicted);
        SchedItem c; c.priority = 5; c.drop_eligible = true;
        TEST_CHECK(q.TryEnqueue(c, 3, evicted) == EnqueueResult::BusyOverflow,
                   "sched/drop: eligible newcomer cannot evict protected work");
        TEST_CHECK(q.Size() == 2, "sched/drop: protected residents intact");
    }

    // ---- 5. Deadline expiry: pop skips expired; reaper reports (§V2-4.6) ----
    {
        SchedQueue q;
        SchedItem evicted;
        SchedItem a; a.priority = 5; a.deadline_ms = 100;
        SchedItem b; b.priority = 5; b.deadline_ms = 0; // no deadline
        (void)q.TryEnqueue(a, 1, evicted);
        (void)q.TryEnqueue(b, 2, evicted);
        SchedItem out, expired; bool had = false;
        TEST_CHECK(q.Pop(out, 150, expired, had) && had,
                   "sched/deadline: expired head is discarded, not dispatched");
        TEST_CHECK(expired.enqueue_seq == 1, "sched/deadline: the expired item is reported");
        TEST_CHECK(q.Pop(out, 150, expired, had) && !had && out.enqueue_seq == 2,
                   "sched/deadline: the live item survives the expired head");
        // Reaper bulk pass.
        SchedItem e1; e1.priority = 5; e1.deadline_ms = 200;
        SchedItem e2; e2.priority = 5; e2.deadline_ms = 300;
        SchedItem live; live.priority = 5; live.deadline_ms = 999;
        (void)q.TryEnqueue(e1, 3, evicted);
        (void)q.TryEnqueue(e2, 4, evicted);
        (void)q.TryEnqueue(live, 5, evicted);
        std::vector<SchedItem> expired_all;
        TEST_CHECK(q.ReaperPass(350, expired_all) == 2, "sched/deadline: reaper expires 2");
        TEST_CHECK(expired_all.size() == 2 && expired_all[0].enqueue_seq == 3 &&
                       expired_all[1].enqueue_seq == 4,
                   "sched/deadline: reaper reports each expiry");
        TEST_CHECK(q.Size() == 1, "sched/deadline: only the live item remains");
    }

    // ---- 6. Session teardown drops only that session's items ----
    {
        SchedQueue q;
        SchedItem evicted;
        SchedItem a; a.priority = 5; a.session = 7;
        SchedItem b; b.priority = 5; b.session = 8;
        (void)q.TryEnqueue(a, 1, evicted);
        (void)q.TryEnqueue(b, 2, evicted);
        std::vector<SchedItem> dropped;
        TEST_CHECK(q.DropSession(7, dropped) == 1, "sched/session: drop 1 item of session 7");
        TEST_CHECK(dropped.size() == 1 && dropped[0].session == 7,
                   "sched/session: the dropped item is reported");
        TEST_CHECK(q.Size() == 1, "sched/session: other session's item stays");
        std::vector<SchedItem> none;
        TEST_CHECK(q.DropSession(0, none) == 0, "sched/session: session 0 is never a target");
    }

    // ---- 7. Scheduler façade: pinned clock + monotonic seq + eviction ----
    {
        int64_t now = 1000;
        Scheduler s([&now] { return now; });
        SchedItem evicted;
        SchedItem a; a.priority = 5; a.deadline_ms = 1100;
        TEST_CHECK(s.Enqueue(a, evicted) == EnqueueResult::Enqueued, "sched/façade: enqueue");
        SchedItem b; b.priority = 5; b.deadline_ms = 0;
        TEST_CHECK(s.Enqueue(b, evicted) == EnqueueResult::Enqueued, "sched/façade: enqueue 2");
        now = 1200; // both a (deadline 1100) now expired
        SchedItem out, expired; bool had = false;
        TEST_CHECK(s.Pop(out, expired, had) && had && expired.enqueue_seq == 1,
                   "sched/façade: pinned clock drives deadline expiry");
        TEST_CHECK(s.Pop(out, expired, had) && !had && out.enqueue_seq == 2,
                   "sched/façade: monotonic seq assigned by the façade");
    }

    // ---- 8. Clamps: priority outside 0..9 is clamped (§V2-4.6) ----
    {
        SchedQueue q;
        SchedItem evicted;
        SchedItem hi; hi.priority = 42;   // -> 9
        SchedItem lo; lo.priority = -7;   // -> 0
        (void)q.TryEnqueue(hi, 1, evicted);
        (void)q.TryEnqueue(lo, 2, evicted);
        SchedItem out, exp; bool had = false;
        TEST_CHECK(q.Pop(out, 0, exp, had) && out.priority == 0,
                   "sched/clamp: negative priority clamps to 0");
        TEST_CHECK(q.Pop(out, 0, exp, had) && out.priority == 9,
                   "sched/clamp: oversized priority clamps to 9");
    }

    TEST_CHECK(g_failed_count == failures_before, "sched: suite had no failures");
}

void TestHostV2Session() {
    std::cout << "[RUN] Testing host_v2 session table (REQ-043 M6 T4)..." << std::endl;
    const int failures_before = g_failed_count;
    using namespace emebalachat::host_v2;

    // P2-1 (session 260925_0001): the deployment serves translate + asr.
    SessionTable t{std::vector<std::string>{"translate", "asr"}};

    // ---- 1. Host-global incrementing ids from 1 ----
    {
        SessionRecord r; r.capability = "translate";
        SessionRecord out;
        TEST_CHECK(t.Open(r, out) == SessionOpenResult::Opened && out.id == 1,
                   "session: first id is 1 (host-global increment)");
        TEST_CHECK(t.Open(r, out) == SessionOpenResult::Opened && out.id == 2,
                   "session: ids increment globally (connection-independent)");
    }

    // ---- 2. Unallocated capability fails closed (§V2-4.5 unavailable) ----
    {
        // P2-1: "asr" is now a SERVED capability — it opens.
        SessionRecord r; r.capability = "asr";
        SessionRecord out;
        TEST_CHECK(t.Open(r, out) == SessionOpenResult::Opened,
                   "session: P2-1 asr capability opens");
        TEST_CHECK(t.Close(out.id), "session: P2-1 asr session closes");
        // A capability outside the served set stays unavailable.
        SessionRecord other; other.capability = "tts";
        TEST_CHECK(t.Open(other, out) == SessionOpenResult::Unavailable,
                   "session: unserved capability -> unavailable");
        SessionRecord empty; empty.capability = "";
        TEST_CHECK(t.Open(empty, out) == SessionOpenResult::BadArguments,
                   "session: empty capability -> bad arguments");
    }

    // ---- 3. Lifecycle: open -> close is terminal; idempotent re-close ----
    {
        SessionRecord r; r.capability = "translate";
        SessionRecord out;
        (void)t.Open(r, out);
        TEST_CHECK(t.Close(out.id), "session: close an open session");
        TEST_CHECK(!t.Close(out.id), "session: double close is a no-op (idempotent)");
        TEST_CHECK(!t.Abort(out.id), "session: abort after close is a no-op");
        SessionRecord snap;
        TEST_CHECK(t.Find(out.id, snap) && snap.state == SessionState::Closed,
                   "session: terminal record stays queryable");
    }

    // ---- 4. Abort terminal state ----
    {
        SessionRecord r; r.capability = "translate";
        SessionRecord out;
        (void)t.Open(r, out);
        TEST_CHECK(t.Abort(out.id), "session: abort an open session");
        TEST_CHECK(!t.Close(out.id), "session: close after abort is a no-op");
    }

    // ---- 5. Event seq per session from 1 (§V2-4.4) ----
    {
        SessionRecord r; r.capability = "translate";
        SessionRecord a, b;
        (void)t.Open(r, a);
        (void)t.Open(r, b);
        TEST_CHECK(t.NextEventSeq(a.id) == 1, "session: first event seq is 1");
        TEST_CHECK(t.NextEventSeq(a.id) == 2, "session: seq increments per session");
        TEST_CHECK(t.NextEventSeq(b.id) == 1, "session: seq is per-session, not global");
        (void)t.Close(a.id);
        TEST_CHECK(t.NextEventSeq(a.id) == 0, "session: no events from a closed session");
    }

    // ---- 6. Connection teardown auto-closes OWNED open sessions ----
    {
        SessionTable local{std::vector<std::string>{"translate"}};
        SessionRecord ra; ra.capability = "translate"; ra.connection = &ra; // owner A
        SessionRecord rb; rb.capability = "translate"; rb.connection = &rb; // owner B
        SessionRecord rc; rc.capability = "translate"; rc.connection = &ra; // owner A
        SessionRecord a, b, c;
        TEST_CHECK(local.Open(ra, a) == SessionOpenResult::Opened, "session/teardown: a opens");
        TEST_CHECK(local.Open(rb, b) == SessionOpenResult::Opened, "session/teardown: b opens");
        TEST_CHECK(local.Open(rc, c) == SessionOpenResult::Opened, "session/teardown: c opens");
        TEST_CHECK(a.connection == &ra && b.connection == &rb && c.connection == &ra,
                   "session/teardown: Open copies the owning connection");
        TEST_CHECK(local.OpenCount() == 3, "session/teardown: 3 open sessions");
        // A DIFFERENT (never-seen) owner closes nothing.
        int sentinel = 0;
        std::vector<std::uint64_t> closed_none;
        TEST_CHECK(local.CloseOwnedByConnection(&sentinel, closed_none) == 0,
                   "session/teardown: a stranger owner closes nothing");
        TEST_CHECK(local.OpenCount() == 3, "session/teardown: sessions survive a stranger");
        // Owner A's teardown closes a and c (b belongs to B).
        std::vector<std::uint64_t> closed;
        TEST_CHECK(local.CloseOwnedByConnection(&ra, closed) == 2,
                   "session/teardown: owner A closes its 2 sessions");
        TEST_CHECK(closed.size() == 2, "session/teardown: the closed ids are reported");
        TEST_CHECK(local.OpenCount() == 1, "session/teardown: only b remains open");
        SessionRecord snap;
        TEST_CHECK(local.Find(b.id, snap) && snap.state == SessionState::Open,
                   "session/teardown: b is untouched");
        // Idempotent: tearing down A again closes nothing.
        std::vector<std::uint64_t> closed_again;
        TEST_CHECK(local.CloseOwnedByConnection(&ra, closed_again) == 0,
                   "session/teardown: repeat teardown is a no-op");
    }

    TEST_CHECK(g_failed_count == failures_before, "session: suite had no failures");
}

void TestHostV2Health() {
    std::cout << "[RUN] Testing host_v2 health counters (REQ-043 M6 T4)..." << std::endl;
    const int failures_before = g_failed_count;
    using namespace emebalachat::host_v2;

    // ---- 1. Fresh boot: vacuous-healthy session ratio, zero fallback ----
    {
        HealthCounters h;
        TEST_CHECK(std::abs(h.SessionSuccessRatio() - 1.0) < 1e-9,
                   "health: 0 sessions -> session_success_ratio 1.0 (nothing failed)");
        TEST_CHECK(std::abs(h.FallbackRatio() - 0.0) < 1e-9,
                   "health: 0 jobs -> fallback_ratio 0.0");
        TEST_CHECK(h.JobsTotal() == 0 && h.SessionsTotal() == 0,
                   "health: fresh counters are zero");
    }

    // ---- 2. Job buckets (success/failure/fallback) ----
    {
        HealthCounters h;
        h.RecordJob(HealthOutcome::Success);
        h.RecordJob(HealthOutcome::Success);
        h.RecordJob(HealthOutcome::Success);
        h.RecordJob(HealthOutcome::Failure);
        h.RecordJob(HealthOutcome::Fallback);
        TEST_CHECK(h.JobsTotal() == 5, "health: 5 jobs counted");
        // fallback 1/5 = 0.2
        TEST_CHECK(std::abs(h.FallbackRatio() - 0.2) < 1e-9,
                   "health: fallback_ratio = fallback/total = 0.2");
        TEST_CHECK(h.JobsFallback() == 1, "health: fallback counter = 1");
    }

    // ---- 3. Session ratio ----
    {
        HealthCounters h;
        h.RecordSession(true);
        h.RecordSession(true);
        h.RecordSession(true);
        h.RecordSession(false);
        TEST_CHECK(h.SessionsTotal() == 4 && h.SessionsOk() == 3,
                   "health: session counters tracked");
        TEST_CHECK(std::abs(h.SessionSuccessRatio() - 0.75) < 1e-9,
                   "health: session_success_ratio = ok/total = 0.75");
    }

    // ---- 4. Monotonicity (no reset path exists) ----
    {
        HealthCounters h;
        h.RecordJob(HealthOutcome::Fallback);
        const double before = h.FallbackRatio();
        h.RecordJob(HealthOutcome::Success);
        TEST_CHECK(h.FallbackRatio() < before,
                   "health: success dilutes the fallback ratio (monotonic totals)");
    }

    TEST_CHECK(g_failed_count == failures_before, "health: suite had no failures");
}

void TestHostV2WelcomeGolden() {
    std::cout << "[RUN] Testing v2 welcome golden shape (REQ-043 M6 T4)..." << std::endl;
    const int failures_before = g_failed_count;
    namespace eh = emebalachat::enginehost;

    // The v2 welcome is BUILT (not parsed) by host_main.cpp from the
    // registry + health. Golden-shape check: the §V2-4.7 example keys parse
    // back through the frozen JSON primitives, and the v1 welcome builder is
    // byte-unchanged (no new fields — the frozen contract). P2-1 extends the
    // v2 shape: capabilities list BOTH translate and asr, and registry
    // models advertising either capability appear in models[].
    {
        const std::string v2_example =
            "{\"op\":\"welcome\",\"protocol\":2,"
            "\"models\":[{\"id\":\"hy-mt2-1.8b-q8\",\"family\":\"ggml-translate\","
            "\"capabilities\":[\"translate\"],\"default\":true},"
            "{\"id\":\"nemotron-3.5-asr-streaming-0.6b-q8\",\"family\":\"ggml-asr\","
            "\"capabilities\":[\"asr\"],\"default\":false}],"
            "\"capabilities\":[\"translate\",\"asr\"],"
            "\"health\":{\"session_success_ratio\":0.997,\"fallback_ratio\":0.004}}";
        eh::JsonPairs p;
        TEST_CHECK(eh::JsonParseObject(v2_example, p), "welcome v2 golden: parses");
        const auto* op = eh::detail::FindField(p, "op");
        const auto* proto = eh::detail::FindField(p, "protocol");
        const auto* models = eh::detail::FindField(p, "models");
        const auto* caps = eh::detail::FindField(p, "capabilities");
        const auto* health = eh::detail::FindField(p, "health");
        TEST_CHECK(op && op->is_string && op->text == "welcome", "welcome v2 golden: op");
        TEST_CHECK(proto && !proto->is_string && proto->text == "2",
                   "welcome v2 golden: protocol 2");
        TEST_CHECK(models && !models->is_string && models->text.find("\"ggml-translate\"") !=
                       std::string::npos &&
                       models->text.find("\"ggml-asr\"") != std::string::npos,
                   "welcome v2 golden: models[] with the translate + asr families (P2-1)");
        TEST_CHECK(health && !health->is_string &&
                       health->text.find("session_success_ratio") != std::string::npos &&
                       health->text.find("fallback_ratio") != std::string::npos,
                   "welcome v2 golden: health{session_success_ratio,fallback_ratio}");
        // P2-1 pins: the served set advertises asr, and the asr registry
        // model rides the models[] list.
        std::vector<std::string> cap_list;
        TEST_CHECK(caps && eh::JsonParseStringArray(caps->text, cap_list) &&
                       std::find(cap_list.begin(), cap_list.end(), "translate") !=
                           cap_list.end() &&
                       std::find(cap_list.begin(), cap_list.end(), "asr") != cap_list.end(),
                   "welcome v2 golden: capabilities [translate, asr] (P2-1)");
        TEST_CHECK(models && models->text.find("nemotron-3.5-asr-streaming-0.6b-q8") !=
                       std::string::npos,
                   "welcome v2 golden: the asr model id is listed (P2-1)");
    }

    // The v1 welcome builder gains NOTHING (frozen bytes).
    {
        eh::WelcomeMsg m;
        m.protocol = eh::kProtocolVersion;
        m.model = std::string(eh::kModelDisplayName);
        m.model_sha256 = std::string(emebalachat::kExpectedModelSha256);
        m.capabilities = {"translate"};
        const std::string built = eh::BuildWelcome(m);
        TEST_CHECK(built.find("\"protocol\":1") != std::string::npos,
                   "welcome v1: protocol stays 1 (frozen)");
        TEST_CHECK(built.find("health") == std::string::npos &&
                       built.find("models") == std::string::npos,
                   "welcome v1: NO v2 fields leak into the frozen shape");
    }

    TEST_CHECK(g_failed_count == failures_before, "welcome: suite had no failures");
}
