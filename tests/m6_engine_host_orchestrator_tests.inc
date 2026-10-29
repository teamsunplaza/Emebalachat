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

    // ---- 9. 260930_0003 (Listener finding 3-2): the client request id is a
    // first-class SchedItem member that SURVIVES every queue transition the
    // dispatcher answers from — pop, deadline expiry (Pop + ReaperPass),
    // session drop, and backpressure eviction. The v2 dispatcher echoes it
    // in every result frame (v1 parity: DispatcherLoop answers job.id); the
    // pre-fix hardcoded 0 made session-level translate unmatchable for
    // family clients. A 0-id item (client omitted "id") stays 0 — the
    // scheduler never synthesizes one.
    {
        SchedQueue q;
        SchedItem evicted;
        SchedItem a; a.priority = 5; a.session = 7; a.request_id = 4242; a.deadline_ms = 100;
        SchedItem b; b.priority = 5; b.request_id = 777; b.session = 8;
        SchedItem c; c.priority = 5; c.request_id = 9; c.drop_eligible = true;
        TEST_CHECK(q.TryEnqueue(a, 1, evicted) == EnqueueResult::Enqueued, "sched/id: a in");
        TEST_CHECK(q.TryEnqueue(b, 2, evicted) == EnqueueResult::Enqueued, "sched/id: b in");
        // Pop: the request_id rides the popped item to the dispatcher's
        // success/failure SendResult(item.request_id, ...).
        SchedItem out, expired; bool had = false;
        TEST_CHECK(q.Pop(out, 150, expired, had) && had &&
                       expired.request_id == 4242,
                   "sched/id: the expiry report carries the client's request id");
        TEST_CHECK(q.Pop(out, 150, expired, had) && !had && out.request_id == 777,
                   "sched/id: the popped item carries the client's request id");
        // ReaperPass: bulk expiry reports keep it too.
        SchedItem d; d.priority = 5; d.request_id = 31337; d.deadline_ms = 200;
        TEST_CHECK(q.TryEnqueue(d, 3, evicted) == EnqueueResult::Enqueued, "sched/id: d in");
        std::vector<SchedItem> expired_all;
        TEST_CHECK(q.ReaperPass(300, expired_all) == 1 && expired_all[0].request_id == 31337,
                   "sched/id: the reaper report carries the client's request id");
        // Session drop: the dropped report keeps it (session_close answers).
        SchedItem e; e.priority = 5; e.session = 8; e.request_id = 5150;
        TEST_CHECK(q.TryEnqueue(e, 4, evicted) == EnqueueResult::Enqueued, "sched/id: e in");
        std::vector<SchedItem> dropped;
        TEST_CHECK(q.DropSession(8, dropped) == 1 && dropped[0].request_id == 5150,
                   "sched/id: the session-drop report carries the client's request id");
        // Backpressure eviction: the evicted report keeps it (busy answer).
        SchedQueue q2(1);
        SchedItem old; old.priority = 5; old.drop_eligible = true; old.request_id = 111;
        SchedItem nu; nu.priority = 5; nu.drop_eligible = true; nu.request_id = 222;
        TEST_CHECK(q2.TryEnqueue(old, 1, evicted) == EnqueueResult::Enqueued, "sched/id: old in");
        TEST_CHECK(q2.TryEnqueue(nu, 2, evicted) == EnqueueResult::BusyDroppedOld &&
                       evicted.request_id == 111,
                   "sched/id: the backpressure eviction report carries the client's request id");
    }

    // ---- 10. The host wiring (structural pin, skipped off-repo-root CWD —
    // the same convention as the REQ-042/REQ-045 source pins): the v2 enqueue
    // site CAPTURES msg.id into the item and the dispatcher answer sites ECHO
    // item/expired/evicted/d.request_id — if either half regresses, the v2
    // result id returns to the unmatchable hardcoded 0.
    {
        std::ifstream host_in("src/host_main.cpp", std::ios::binary);
        if (!host_in) {
            std::cout << "[SKIP] src/host_main.cpp not resolvable from the test CWD; "
                         "260930_0003 v2 id-echo wiring pins skipped."
                      << std::endl;
        } else {
            const std::string host_src((std::istreambuf_iterator<char>(host_in)),
                                       std::istreambuf_iterator<char>());
            TEST_CHECK(host_src.find("item.request_id = msg.id;") != std::string::npos,
                       "sched/id: the v2 enqueue site captures the client's request id");
            TEST_CHECK(host_src.find("requester->SendResult(item.request_id, status, out_text, "
                                     "std::move(served_model))") != std::string::npos,
                       "sched/id: the v2 success answer echoes the client's request id");
            TEST_CHECK(host_src.find("expired.request_id, enginehost::HostStatus::Timeout") !=
                           std::string::npos,
                       "sched/id: the v2 deadline-expiry answer echoes the request id");
            TEST_CHECK(host_src.find("evicted.request_id, enginehost::HostStatus::Busy") !=
                           std::string::npos,
                       "sched/id: the v2 backpressure-eviction answer echoes the request id");
            TEST_CHECK(host_src.find("d.request_id, enginehost::HostStatus::Busy") !=
                           std::string::npos,
                       "sched/id: the v2 session-drop answer echoes the request id");
            const size_t v2_begin = host_src.find("void DispatcherV2Loop(");
            const size_t v2_end = host_src.find("bool ReadMessage(Connection& conn");
            const std::string v2_region =
                (v2_begin != std::string::npos && v2_end != std::string::npos && v2_end > v2_begin)
                    ? host_src.substr(v2_begin, v2_end - v2_begin)
                    : "";
            TEST_CHECK(!v2_region.empty() &&
                           v2_region.find("SendResult(0,") == std::string::npos,
                       "sched/id: no hardcoded-0 result remains inside the v2 dispatcher");
        }
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

    // ---- 7. R1-10: bounded growth — Open() prunes TERMINAL records ----
    // The engine host is persistent (REQ-060 evicts workers, never the host)
    // and Open() runs once per translate job, so an un-pruned table grows
    // forever. Semantics pinned here: the retained count is bound by
    // 4*open + kPruneSlack (+1 for the record being opened), terminal records
    // outside the newest-terminal window are erased, live records never are,
    // and a pruned id answers exactly like any other terminal id.
    {
        SessionTable prune{std::vector<std::string>{"translate"}};
        SessionRecord req; req.capability = "translate"; req.model_id = "session-model";
        SessionRecord out;
        std::uint64_t first_id = 0;
        std::uint64_t last_id = 0;
        bool all_opened = true;
        for (int i = 0; i < 400; ++i) {
            if (prune.Open(req, out) != SessionOpenResult::Opened) { all_opened = false; break; }
            if (i == 0) first_id = out.id;
            last_id = out.id;
            if (!prune.Close(out.id)) { all_opened = false; break; }
        }
        TEST_CHECK(all_opened && first_id == 1 && last_id == 400,
                   "session/prune: 400 open/close cycles allocate monotonic ids");
        TEST_CHECK(prune.OpenCount() == 0, "session/prune: every cycle ended closed");
        TEST_CHECK(prune.TotalCount() <= SessionTable::kPruneSlack + 1,
                   "session/prune: retained records stay bounded over 400 cycles "
                   "(<= kPruneSlack + 1)");
        TEST_CHECK(prune.TotalCount() >= SessionTable::kPruneKeepTerminal,
                   "session/prune: the newest-terminal retention window is honoured");
        SessionRecord snap;
        TEST_CHECK(!prune.Find(first_id, snap),
                   "session/prune: the OLDEST terminal record was erased");
        TEST_CHECK(prune.Find(last_id, snap) && snap.state == SessionState::Closed,
                   "session/prune: the newest terminal record stays findable (id stability)");
        // A pruned id is indistinguishable from any already-terminal id: every
        // accessor already answers false/0 for that case, so nothing can
        // resurrect or mis-sequence a pruned session.
        TEST_CHECK(!prune.Close(first_id), "session/prune: close of a pruned id is a no-op");
        TEST_CHECK(!prune.Abort(first_id), "session/prune: abort of a pruned id is a no-op");
        TEST_CHECK(prune.NextEventSeq(first_id) == 0,
                   "session/prune: a pruned id allocates no event seq");
        TEST_CHECK(prune.Open(req, out) == SessionOpenResult::Opened && out.id == 401,
                   "session/prune: ids stay monotonic across a prune (never reused)");
        (void)prune.Close(out.id);
    }

    // ---- 8. R1-10: a prune never drops a LIVE session, and Aborted counts
    // as terminal exactly like Closed ----
    {
        constexpr int kLive = 12;
        SessionTable prune{std::vector<std::string>{"translate"}};
        SessionRecord live_req;
        live_req.capability = "translate";
        live_req.model_id = "live-model";
        std::uint64_t live_ids[kLive] = {};
        int opened_live = 0;
        for (int i = 0; i < kLive; ++i) {
            SessionRecord out;
            if (prune.Open(live_req, out) == SessionOpenResult::Opened) {
                live_ids[opened_live++] = out.id;
            }
        }
        TEST_CHECK(opened_live == kLive && prune.OpenCount() == kLive,
                   "session/prune-live: 12 live sessions open and counted");
        // Churn 600 sessions through the table (half closed, half aborted)
        // around the live ones, forcing several prune scans.
        SessionRecord churn; churn.capability = "translate";
        std::uint64_t oldest_churn = 0;
        std::uint64_t newest_churn = 0;
        bool churn_ok = true;
        for (int i = 0; i < 600; ++i) {
            SessionRecord out;
            if (prune.Open(churn, out) != SessionOpenResult::Opened) { churn_ok = false; break; }
            if (i == 0) oldest_churn = out.id;
            newest_churn = out.id;
            const bool terminal = (i % 2 == 0) ? prune.Close(out.id) : prune.Abort(out.id);
            if (!terminal) { churn_ok = false; break; }
        }
        TEST_CHECK(churn_ok, "session/prune-live: 600 churn sessions opened + terminally closed");
        TEST_CHECK(prune.OpenCount() == kLive,
                   "session/prune-live: OpenCount unchanged by the prune scans (12)");
        // Bound with 12 live sessions: 4*12 + 256 + 1.
        TEST_CHECK(prune.TotalCount() <=
                       SessionTable::kPruneOpenFactor * kLive + SessionTable::kPruneSlack + 1,
                   "session/prune-live: retained count bounded by 4*live + kPruneSlack + 1");
        bool all_live_found = true;
        bool live_model_kept = true;
        bool seq_ok = true;
        for (int i = 0; i < opened_live; ++i) {
            SessionRecord snap;
            if (!prune.Find(live_ids[i], snap) || snap.state != SessionState::Open) {
                all_live_found = false;
                break;
            }
            if (snap.model_id != "live-model") live_model_kept = false;
            if (prune.NextEventSeq(live_ids[i]) == 0) seq_ok = false;
        }
        TEST_CHECK(all_live_found, "session/prune-live: every live session survives the prune");
        TEST_CHECK(live_model_kept, "session/prune-live: live session records keep their fields");
        TEST_CHECK(seq_ok, "session/prune-live: live sessions still allocate event seqs");
        SessionRecord snap;
        TEST_CHECK(!prune.Find(oldest_churn, snap),
                   "session/prune-live: an OLD terminal (aborted/closed) record was erased");
        TEST_CHECK(prune.Find(newest_churn, snap) &&
                       (snap.state == SessionState::Closed ||
                        snap.state == SessionState::Aborted),
                   "session/prune-live: the newest terminal record keeps its exact state");
        // Closing the live set is still honoured after the prune scans.
        int closed_live = 0;
        for (int i = 0; i < opened_live; ++i) {
            if (prune.Close(live_ids[i])) ++closed_live;
        }
        TEST_CHECK(closed_live == kLive && prune.OpenCount() == 0,
                   "session/prune-live: live session teardown still works after a prune");
    }

    TEST_CHECK(g_failed_count == failures_before, "session: suite had no failures");
}

// R1-17 (v0.10.4 sweep, 261029_0001): the busy-target publication ordering is
// a memory-model property that is unobservable on the shipped x64 build, so it
// is pinned structurally (the same convention as the REQ-042/REQ-045 source
// pins): the winning exchange must carry a release half AND the HWND store
// must be a release store, published in that order before the queue push. A
// regression to relaxed/acquire-only is invisible on x64 and only bites on a
// weakly ordered architecture - exactly why it needs a pin rather than a
// runtime test.
void TestBusyTargetPublishOrdering() {
    std::cout << "[RUN] Testing PostTask busy-target publication ordering (R1-17)..."
              << std::endl;
    const int failures_before = g_failed_count;
    std::ifstream in("src/worker.cpp", std::ios::binary);
    if (!in) {
        std::cout << "[SKIP] src/worker.cpp not resolvable from the test CWD; "
                     "R1-17 publication-order pins skipped." << std::endl;
        return;
    }
    const std::string src((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    const std::size_t post = src.find("bool PipelineWorker::PostTask(");
    const std::size_t end = src.find("void PipelineWorker::WorkerLoop(", post);
    TEST_CHECK(post != std::string::npos && end != std::string::npos && end > post,
               "worker/post: the PostTask body is locatable for the ordering pin");
    if (post == std::string::npos || end == std::string::npos || end <= post) {
        TEST_CHECK(g_failed_count == failures_before, "worker/post: suite had no failures");
        return;
    }
    const std::string body = src.substr(post, end - post);
    const std::size_t busy_at =
        body.find("is_busy_.exchange(true, std::memory_order_acq_rel)");
    const std::size_t hwnd_at =
        body.find("busy_target_hwnd_.store(target_hwnd, std::memory_order_release)");
    const std::size_t push_at = body.find("queue_.push(");
    TEST_CHECK(busy_at != std::string::npos,
               "worker/post: the winning exchange is acq_rel (release half, R1-17)");
    TEST_CHECK(hwnd_at != std::string::npos,
               "worker/post: the HWND publication is a release store (R1-17)");
    TEST_CHECK(busy_at != std::string::npos && hwnd_at != std::string::npos &&
                   push_at != std::string::npos && busy_at < hwnd_at && hwnd_at < push_at,
               "worker/post: busy -> hwnd -> queue push ordering preserved (REQ-003)");

    // R1-17 consumer half (U-G residual, closed in the U-H2 unit): the two
    // accessors the hook thread reads must ACQUIRE, because a release store
    // pairs only with an acquire load of the same atomic. Relaxed loads on this
    // side leave the publisher's edge with no matching half (invisible on x64,
    // wrong on a weakly ordered build).
    {
        std::ifstream hpp("src/worker.hpp", std::ios::binary);
        if (!hpp) {
            std::cout << "[SKIP] src/worker.hpp not resolvable from the test CWD; "
                         "R1-17 consumer-side pins skipped." << std::endl;
        } else {
            const std::string hsrc((std::istreambuf_iterator<char>(hpp)),
                                   std::istreambuf_iterator<char>());
            TEST_CHECK(hsrc.find("is_busy_.load(std::memory_order_acquire)") != std::string::npos,
                       "worker/read: IsBusy() loads with acquire (R1-17 consumer half)");
            TEST_CHECK(hsrc.find("busy_target_hwnd_.load(std::memory_order_acquire)") !=
                           std::string::npos,
                       "worker/read: BusyTargetHwnd() loads with acquire (R1-17 consumer half)");
            TEST_CHECK(hsrc.find("is_busy_.load(std::memory_order_relaxed)") == std::string::npos &&
                           hsrc.find("busy_target_hwnd_.load(std::memory_order_relaxed)") ==
                               std::string::npos,
                       "worker/read: no relaxed load survives on the busy/hwnd pair");
            TEST_CHECK(hsrc.find("Relaxed ordering suffices") == std::string::npos,
                       "worker/read: the stale 'relaxed ordering suffices' comment is gone");
        }
    }
    TEST_CHECK(g_failed_count == failures_before, "worker/post: suite had no failures");
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
