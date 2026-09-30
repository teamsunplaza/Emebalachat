// ---------------------------------------------------------------------------
// 260927_0003 (MT audit Q1 fix A): bounded settle at give-up — the pairing
// integrity suite (TestWorkerSettlePipe + TestHostGiveUpSettlePins).
//
// Background (Listener audit docs/260927_0002 debug-report.md, MT audit Q1):
// worker events carry NO job id (worker_protocol.hpp EventMsg), the host's job
// wait loops classify frames BY KIND ONLY, and REQ-049's DrainWorkerPipe is a
// non-blocking poll that by its own contract covers strays that arrived while
// the dispatcher was IDLE. After any give-up (deadline + grace) with the
// worker still decoding, the worker publishes the given-up job's terminal
// event only AFTER the abort unwinds the decode — milliseconds to seconds
// after the give-up — and the back-to-back next job consumed it as its own
// answer (the host re-stamps whatever the wait loop consumed with the current
// job id). The off-by-one then persisted for the worker's lifetime: clean
// rows displaying translations generated for neighboring inputs.
//
// Fix A (Emebalachat): after the abort order, the dispatcher SETTLES — reads
// frames until the given-up job's terminal event arrives and DISCARDS it —
// before the timeout answer leaves and before the next job frame is published
// (bounded by kWorkerSettleMs <= 10 s, the audit's budget; cancellable on
// host shutdown). The settle primitive lives in WorkerManager
// (SettleWorkerPipe, Emebalachat_host_v2) so run_tests can drive it against a
// REAL named-pipe pair with a scripted worker client — no worker process.
//
// What is proven here:
//   S1 a late terminal event interleaved with heartbeat/malformed frames is
//     discarded and the pipe is left clean (nothing left to misattribute);
//   S2 a terminal event already queued at settle start is consumed
//     immediately (settle returns well inside its budget);
//   S3 an empty pipe burns the FULL budget (DeadlineExpired) without
//     touching the family state, and post-settle idle traffic is still
//     drained by REQ-049's DrainWorkerPipe (idle-drain behavior preserved);
//   S4 the cancel flag ends the settle early (host shutdown path);
//   S5 the end-to-end lockstep: scripted worker answers job1 only AFTER the
//     abort (the stray), then answers job2 promptly — the give-up sequence
//     (abort -> settle -> next job) MUST deliver job2's own translation to
//     job2 (pre-fix the dispatcher would have re-stamped job1's late final);
//   S6 F-03 residual pin: a stray that OUTLIVES an expired settle budget is
//     still mis-stamped into the back-to-back next job — the accepted bound
//     the 10 s production budget exists to prevent, pinned so any future
//     fail-loud change must update it deliberately;
//   S7 F-03 max_frames arm: a flooding heartbeat worker ends the settle on
//     the FRAME budget (DeadlineExpired, fast) having consumed exactly
//     max_frames frames;
//   S8 F-01 regression: two dispatcher threads over ONE family (the real
//     host shape) — the sibling's publish DURING the settle is refused
//     (TrySetBusy fails fast), and after the release the sibling's job
//     receives its own terminal INTACT (never discarded by the settle);
//   S9 F-03 PipeIoError arm: the worker dies mid-settle — prompt PipeIoError,
//     family state untouched (crash judgment stays with the reaper);
//   P1 structural pins: both dispatcher give-up branches (v1 + v2) call
//     SettleWorkerPipe exactly once, AFTER BuildAbort and BEFORE the client
//     answer, and never on the deadline-met Final/Error paths; plus the F-01
//     single-flight wiring (TrySetBusy before every family-pipe touch, the
//     old no-op SetBusy(true) prolog gone, Busy answer on contention).
//
// Staged as an .inc next to this runner; registered in main() after
// TestJobWaitFrameClassifier(), per the end-of-file pattern.
// ---------------------------------------------------------------------------

namespace settle_detail {

namespace hv2 = emebalachat::host_v2;
namespace wp = emebalachat::workerproto;
namespace eh = emebalachat::enginehost;

// Framed write on the worker-client end (4-byte length prefix, message mode).
inline bool ClientWriteRaw(HANDLE pipe, const std::string& json) {
    std::string frame;
    if (!eh::FrameEncode(json, frame)) return false;
    OVERLAPPED ol = {};
    ol.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ol.hEvent) return false;
    DWORD written = 0;
    BOOL ok = ::WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, &ol);
    if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
        ok = ::GetOverlappedResult(pipe, &ol, &written, TRUE);
    }
    ::CloseHandle(ol.hEvent);
    return ok && written == frame.size();
}

enum class ClientReadResult : unsigned char { Ok, Timeout, IoError };

// Bounded framed read on the worker-client end. A timeout cancels the
// overlapped read so the scripted worker thread can always wind down.
inline ClientReadResult ClientReadRaw(HANDLE pipe, std::string& json, DWORD timeout_ms) {
    static thread_local std::vector<char> buf((1u << 20) + 4);
    OVERLAPPED ol = {};
    ol.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ol.hEvent) return ClientReadResult::IoError;
    DWORD read = 0;
    BOOL ok = ::ReadFile(pipe, buf.data(), static_cast<DWORD>(buf.size()), &read, &ol);
    if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
        const DWORD wr = ::WaitForSingleObject(ol.hEvent, timeout_ms);
        if (wr == WAIT_TIMEOUT) {
            ::CancelIoEx(pipe, &ol);
            ::CloseHandle(ol.hEvent);
            return ClientReadResult::Timeout;
        }
        ok = ::GetOverlappedResult(pipe, &ol, &read, TRUE);
    }
    ::CloseHandle(ol.hEvent);
    if (!ok) return ClientReadResult::IoError;
    if (read < 4) return ClientReadResult::IoError;
    uint32_t len = 0;
    eh::FrameReadLengthPrefix(buf.data(), len);
    if (static_cast<size_t>(len) + 4 != read) return ClientReadResult::IoError;
    json.assign(buf.data() + 4, static_cast<size_t>(len));
    return ClientReadResult::Ok;
}

inline std::string FrameOp(const std::string& json) {
    eh::JsonPairs fields;
    if (!eh::JsonParseObject(json, fields)) return std::string();
    const auto* op = eh::detail::FindField(fields, "op");
    return (op && op->is_string) ? op->text : std::string();
}

inline std::string BuildFinalEvent(std::uint64_t seq, const std::string& text) {
    wp::EventMsg ev;
    ev.session = 0;
    ev.kind = wp::EventKind::Final;
    ev.seq = seq;
    ev.text = text;
    return wp::BuildEvent(ev);
}

// One WorkerManager + one scripted worker client connected over a REAL named
// pipe. The spawn seam injects a never-signaled event as the process handle
// and kicks the client thread (the seam itself runs with the manager mutex
// held — the documented EnsureSpawned convoy), which connects, completes the
// announce handshake, then runs the scenario's scripted behavior.
struct Fixture {
    explicit Fixture(std::function<void(Fixture&)> b)
        : mgr([](const hv2::SpawnRequest& req, void* user) -> hv2::SpawnResult {
                  auto* self = static_cast<Fixture*>(user);
                  self->pipe_name = req.pipe_name;
                  self->token = req.token;
                  hv2::SpawnResult r;
                  r.ok = true;
                  r.process = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
                  r.done_event = r.process;
                  self->client_thread = std::thread([self] { self->ClientMain(); });
                  return r;
              },
              [](HANDLE h, void*) { if (h) ::CloseHandle(h); },
              this)
        , behavior(std::move(b)) {
        wchar_t self_path[MAX_PATH] = {0};
        ::GetModuleFileNameW(nullptr, self_path, MAX_PATH);
        mgr.RegisterFamily(L"ggml-translate", self_path); // exists by construction
    }

    ~Fixture() {
        // Join the scripted client FIRST (its reads are bounded), then drop
        // its pipe end: the manager dtor's GracefulStop then sees EOF.
        if (client_thread.joinable()) client_thread.join();
        if (client && client != INVALID_HANDLE_VALUE) ::CloseHandle(client);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    hv2::WorkerHandle* Spawn() { return mgr.EnsureSpawned(L"ggml-translate"); }

    // Scripted-worker-side close (S9): closes the client end exactly once and
    // nulls the handle so ~Fixture never double-closes a recycled HANDLE value.
    void CloseClient() {
        if (client && client != INVALID_HANDLE_VALUE) {
            ::CloseHandle(client);
            client = nullptr;
        }
    }

    void RecordOp(const std::string& op) {
        std::lock_guard<std::mutex> lk(ops_mu);
        ops.push_back(op);
    }
    std::vector<std::string> Ops() {
        std::lock_guard<std::mutex> lk(ops_mu);
        return ops;
    }

    hv2::WorkerManager mgr;
    std::function<void(Fixture&)> behavior;
    std::wstring pipe_name;
    std::string token;
    HANDLE client = nullptr;
    std::thread client_thread;
    std::mutex ops_mu;
    std::vector<std::string> ops;

private:
    void ClientMain() {
        HANDLE h = nullptr;
        for (int attempt = 0; attempt < 100; ++attempt) {
            h = ::CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) break;
            ::Sleep(25);
        }
        client = h;
        if (!client || client == INVALID_HANDLE_VALUE) return;
        DWORD mode = PIPE_READMODE_MESSAGE;
        ::SetNamedPipeHandleState(client, &mode, nullptr, nullptr);
        if (!ClientWriteRaw(client, wp::BuildAnnounce(wp::EmbeddedWorkerManifest(), token))) {
            return;
        }
        behavior(*this);
    }
};

inline int64_t SteadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace settle_detail

void TestWorkerSettlePipe() {
    std::cout << "[RUN] Testing worker give-up settle (260927_0003, MT audit fix A)..."
              << std::endl;
    const int failures_before = g_failed_count;
    using namespace settle_detail;
    const std::wstring family(L"ggml-translate");

    // ---- S1: late terminal interleaved with heartbeat + malformed is discarded ----
    {
        Fixture f([](Fixture& fx) {
            ::Sleep(150);
            ClientWriteRaw(fx.client, wp::BuildHeartbeat(1));
            ClientWriteRaw(fx.client,
                           "{\"op\":\"event\",\"session\":0,\"kind\":\"bogus\",\"seq\":1}");
            ::Sleep(150);
            ClientWriteRaw(fx.client, BuildFinalEvent(1, "STALE-FROM-GIVEN-UP-JOB"));
        });
        hv2::WorkerHandle* w = f.Spawn();
        TEST_CHECK(w != nullptr, "settle S1: handshake completes (manager Ready)");
        if (w) {
            const hv2::WorkerManager::SettleOutcome rc =
                f.mgr.SettleWorkerPipe(family, 5000);
            TEST_CHECK(rc == hv2::WorkerManager::SettleOutcome::TerminalDiscarded,
                       "settle S1: late terminal after give-up is discarded");
            TEST_CHECK(f.mgr.DrainWorkerPipe(family) == 0,
                       "settle S1: pipe left clean (heartbeat + malformed consumed too)");
            TEST_CHECK(f.mgr.State(family) == hv2::WorkerState::Ready,
                       "settle S1: family still Ready after the settle");
        }
    }

    // ---- S2: terminal already queued at settle start is consumed immediately ----
    {
        Fixture f([](Fixture& fx) {
            ClientWriteRaw(fx.client, BuildFinalEvent(1, "QUEUED-STALE-FINAL"));
        });
        hv2::WorkerHandle* w = f.Spawn();
        TEST_CHECK(w != nullptr, "settle S2: handshake completes (manager Ready)");
        if (w) {
            ::Sleep(300); // let the frame land before the settle starts
            const int64_t t0 = SteadyNowMs();
            const hv2::WorkerManager::SettleOutcome rc =
                f.mgr.SettleWorkerPipe(family, 5000);
            const int64_t elapsed = SteadyNowMs() - t0;
            TEST_CHECK(rc == hv2::WorkerManager::SettleOutcome::TerminalDiscarded,
                       "settle S2: queued terminal consumed");
            TEST_CHECK(elapsed < 3000,
                       "settle S2: returns promptly once the terminal arrives (budget not burned)");
        }
    }

    // ---- S3: empty pipe burns the full budget; REQ-049 idle drain preserved ----
    {
        Fixture f([](Fixture& fx) {
            ::Sleep(1200); // outlive the settle budget
            ClientWriteRaw(fx.client, wp::BuildHeartbeat(2)); // post-settle idle traffic
        });
        hv2::WorkerHandle* w = f.Spawn();
        TEST_CHECK(w != nullptr, "settle S3: handshake completes (manager Ready)");
        if (w) {
            const int64_t t0 = SteadyNowMs();
            const hv2::WorkerManager::SettleOutcome rc =
                f.mgr.SettleWorkerPipe(family, 400);
            const int64_t elapsed = SteadyNowMs() - t0;
            TEST_CHECK(rc == hv2::WorkerManager::SettleOutcome::DeadlineExpired,
                       "settle S3: no terminal -> DeadlineExpired (hung worker cannot wedge the dispatcher)");
            TEST_CHECK(elapsed >= 300 && elapsed < 5000,
                       "settle S3: bounded by the budget (full 400 ms burned, no overrun)");
            TEST_CHECK(f.mgr.State(family) == hv2::WorkerState::Ready,
                       "settle S3: family still Ready after a budget-expired settle");
            ::Sleep(1500); // let the post-settle heartbeat land
            TEST_CHECK(f.mgr.DrainWorkerPipe(family) == 1,
                       "settle S3: REQ-049 idle drain still drains post-settle traffic");
        }
    }

    // ---- S4: the cancel flag ends the settle early (host shutdown path) ----
    {
        Fixture f([](Fixture&) { ::Sleep(3000); });
        hv2::WorkerHandle* w = f.Spawn();
        TEST_CHECK(w != nullptr, "settle S4: handshake completes (manager Ready)");
        if (w) {
            std::atomic<bool> cancel{false};
            std::thread setter([&cancel] {
                ::Sleep(300);
                cancel.store(true, std::memory_order_release);
            });
            const int64_t t0 = SteadyNowMs();
            const hv2::WorkerManager::SettleOutcome rc =
                f.mgr.SettleWorkerPipe(family, 10000, &cancel);
            const int64_t elapsed = SteadyNowMs() - t0;
            setter.join();
            TEST_CHECK(rc == hv2::WorkerManager::SettleOutcome::Cancelled,
                       "settle S4: cancel -> Cancelled");
            TEST_CHECK(elapsed < 3000,
                       "settle S4: returns early on cancel (10 s budget not burned)");
        }
    }

    // ---- S5: end-to-end lockstep — the stray can NEVER reach the next job ----
    {
        // Scripted worker: job1 is "slow"; its final is published only AFTER
        // the abort unwinds (the stray); job2 answers promptly. Pre-fix, the
        // dispatcher's await loop for job2 read job1's late final first and
        // re-stamped it as job2's answer.
        Fixture f([](Fixture& fx) {
            std::string json;
            if (ClientReadRaw(fx.client, json, 10000) == ClientReadResult::Ok) {
                fx.RecordOp(FrameOp(json)); // expect "job"
            }
            ::Sleep(200); // decode still running when the host gives up
            if (ClientReadRaw(fx.client, json, 10000) == ClientReadResult::Ok) {
                fx.RecordOp(FrameOp(json)); // expect "abort"
            }
            ::Sleep(200); // abort unwinds between tokens
            ClientWriteRaw(fx.client, BuildFinalEvent(1, "TRANSLATION-OF-JOB-ONE"));
            if (ClientReadRaw(fx.client, json, 10000) == ClientReadResult::Ok) {
                fx.RecordOp(FrameOp(json)); // expect "job"
            }
            ::Sleep(50);
            ClientWriteRaw(fx.client, BuildFinalEvent(2, "translation-of-job-two"));
        });
        hv2::WorkerHandle* w = f.Spawn();
        TEST_CHECK(w != nullptr, "settle S5: handshake completes (manager Ready)");
        if (w) {
            // Dispatch job1; it never answers within its (simulated) deadline.
            wp::JobMsg j1;
            j1.job = 1;
            j1.session = 0;
            j1.src = "ko";
            j1.tgt = "en";
            j1.text = "first";
            TEST_CHECK(f.mgr.TrySetBusy(family),
                       "settle S5: owner dispatcher acquires the family (F-01 prolog)");
            TEST_CHECK(f.mgr.SendToWorker(family, wp::BuildJob(j1)),
                       "settle S5: job1 published");
            ::Sleep(300); // job1 deadline passes with the worker still decoding

            // Give-up sequence (fix A): abort -> bounded settle -> answer.
            TEST_CHECK(f.mgr.SendToWorker(family, wp::BuildAbort(0)),
                       "settle S5: abort published at give-up");
            const hv2::WorkerManager::SettleOutcome rc =
                f.mgr.SettleWorkerPipe(family, 5000, nullptr);
            TEST_CHECK(rc == hv2::WorkerManager::SettleOutcome::TerminalDiscarded,
                       "settle S5: the given-up job's late terminal is swallowed");

            // Next job, dispatched back-to-back (REQ-049 pre-dispatch drain
            // parity with both real dispatchers).
            (void)f.mgr.DrainWorkerPipe(family);
            wp::JobMsg j2;
            j2.job = 2;
            j2.session = 0;
            j2.src = "ko";
            j2.tgt = "en";
            j2.text = "second";
            TEST_CHECK(f.mgr.SendToWorker(family, wp::BuildJob(j2)),
                       "settle S5: job2 published");

            // The await loop both dispatchers run: classify by kind, first
            // terminal frame wins. It MUST be job2's own answer.
            std::string got_text;
            bool got_final = false;
            const int64_t await_deadline = SteadyNowMs() + 8000;
            while (!got_final && SteadyNowMs() < await_deadline) {
                std::string json;
                const auto rr = f.mgr.ReadFromWorker(family, json, 250);
                if (rr != hv2::WorkerManager::WorkerRead::Ok) continue;
                wp::EventMsg ev;
                const hv2::JobWaitFrame frame = hv2::ClassifyJobWaitFrame(json, ev);
                if (frame == hv2::JobWaitFrame::Final) {
                    got_text = ev.text;
                    got_final = true;
                }
            }
            f.mgr.SetBusy(family, false);
            TEST_CHECK(got_final, "settle S5: job2 answered");
            TEST_CHECK(got_text == "translation-of-job-two",
                       "settle S5: job2 receives JOB2'S translation (the late stray is never re-stamped)");
            const std::vector<std::string> ops = f.Ops();
            TEST_CHECK(ops.size() == 3 && ops[0] == "job" && ops[1] == "abort" &&
                           ops[2] == "job",
                       "settle S5: scripted worker observed job -> abort -> job");
        }
    }

    // ---- S6: PINNED accepted residual — an EXPIRED settle cannot protect the next job ----
    // F-03 (review round 2): the documented post-expiry window is pinned by a
    // test. A settle whose budget expires BEFORE the stray terminal arrives
    // leaves that stray on the pipe; the back-to-back next job's REQ-049
    // drain already ran (it saw an empty pipe), so the stray lands in the
    // next job's await window and is mis-stamped — exactly the original
    // pairing skew, reachable only when the worker outlives the budget. This
    // is the accepted bound the 10 s production budget exists to prevent; if
    // the residual is ever changed (e.g. made fail-loud), update this pin.
    {
        Fixture f([](Fixture& fx) {
            std::string json;
            if (ClientReadRaw(fx.client, json, 10000) == ClientReadResult::Ok) {
                fx.RecordOp(FrameOp(json)); // expect "job"
            }
            ::Sleep(200);
            if (ClientReadRaw(fx.client, json, 10000) == ClientReadResult::Ok) {
                fx.RecordOp(FrameOp(json)); // expect "abort"
            }
            ::Sleep(900); // OUTLIVES the 400 ms settle budget below
            ClientWriteRaw(fx.client, BuildFinalEvent(1, "STALE-AFTER-EXPIRED-SETTLE"));
        });
        hv2::WorkerHandle* w = f.Spawn();
        TEST_CHECK(w != nullptr, "settle S6: handshake completes (manager Ready)");
        if (w) {
            wp::JobMsg j1;
            j1.job = 1;
            j1.session = 0;
            j1.src = "ko";
            j1.tgt = "en";
            j1.text = "first";
            TEST_CHECK(f.mgr.TrySetBusy(family), "settle S6: owner acquires");
            TEST_CHECK(f.mgr.SendToWorker(family, wp::BuildJob(j1)),
                       "settle S6: job1 published");
            ::Sleep(300); // job1's deadline passes
            TEST_CHECK(f.mgr.SendToWorker(family, wp::BuildAbort(0)),
                       "settle S6: abort published at give-up");
            const hv2::WorkerManager::SettleOutcome rc =
                f.mgr.SettleWorkerPipe(family, 400); // short budget: expires first
            TEST_CHECK(rc == hv2::WorkerManager::SettleOutcome::DeadlineExpired,
                       "settle S6: the budget expires before the stray arrives");
            // The stray has NOT arrived (worker publishes it ~1.2 s in, the
            // settle expired at ~0.7 s), so even the REQ-049 pre-publish drain
            // sees nothing. Publish the back-to-back next job like the real
            // dispatcher prolog does.
            (void)f.mgr.DrainWorkerPipe(family);
            wp::JobMsg j2;
            j2.job = 2;
            j2.session = 0;
            j2.src = "ko";
            j2.tgt = "en";
            j2.text = "second";
            TEST_CHECK(f.mgr.SendToWorker(family, wp::BuildJob(j2)),
                       "settle S6: job2 published back-to-back");
            std::string got_text;
            bool got_final = false;
            const int64_t await_deadline = SteadyNowMs() + 8000;
            while (!got_final && SteadyNowMs() < await_deadline) {
                std::string json;
                if (f.mgr.ReadFromWorker(family, json, 250) !=
                    hv2::WorkerManager::WorkerRead::Ok) {
                    continue;
                }
                wp::EventMsg ev;
                const hv2::JobWaitFrame frame = hv2::ClassifyJobWaitFrame(json, ev);
                if (frame == hv2::JobWaitFrame::Final) {
                    got_text = ev.text;
                    got_final = true;
                }
            }
            f.mgr.SetBusy(family, false);
            TEST_CHECK(got_final && got_text == "STALE-AFTER-EXPIRED-SETTLE",
                       "settle S6: PINNED residual — a stray that outlives an EXPIRED settle "
                       "is still mis-stamped into the next job (the 10 s bound is the fix)");
            const std::vector<std::string> ops = f.Ops();
            TEST_CHECK(ops.size() == 2 && ops[0] == "job" && ops[1] == "abort",
                       "settle S6: scripted worker observed job -> abort");
        }
    }

    // ---- S7: the max_frames budget arm — a flooding heartbeat worker ----
    // F-03 (review round 2): 64 heartbeats queued, max_frames = 8 with a 5 s
    // time budget — the settle must end on the FRAME budget (DeadlineExpired,
    // fast) having consumed exactly 8 frames, leaving 56 for the drain.
    {
        Fixture f([](Fixture& fx) {
            for (int i = 0; i < 64; ++i) {
                if (!ClientWriteRaw(fx.client, wp::BuildHeartbeat(i + 1))) break;
            }
        });
        hv2::WorkerHandle* w = f.Spawn();
        TEST_CHECK(w != nullptr, "settle S7: handshake completes (manager Ready)");
        if (w) {
            ::Sleep(300); // let the flood land
            const int64_t t0 = SteadyNowMs();
            const hv2::WorkerManager::SettleOutcome rc =
                f.mgr.SettleWorkerPipe(family, 5000, nullptr, /*max_frames=*/8);
            const int64_t elapsed = SteadyNowMs() - t0;
            TEST_CHECK(rc == hv2::WorkerManager::SettleOutcome::DeadlineExpired,
                       "settle S7: frame-budget exhaustion -> DeadlineExpired");
            TEST_CHECK(elapsed < 3000,
                       "settle S7: the FRAME budget fired (not the 5 s time budget)");
            TEST_CHECK(f.mgr.DrainWorkerPipe(family) == 56,
                       "settle S7: exactly 8 of the 64 heartbeats consumed by the settle");
        }
    }

    // ---- S8: F-01 regression — the sibling dispatcher cannot publish mid-settle ----
    // One real manager + one real pipe; TWO dispatcher threads, the shape the
    // concurrent host runs (v1 loop + v2 loop over the same translate
    // family). D1 owns the family (job1 -> give-up -> settle); D2 is the
    // sibling: its publish attempt DURING the settle must fail fast (Busy
    // answer), and after D1's release its retry must succeed and its job's
    // terminal must arrive INTACT — never discarded by D1's settle.
    {
        Fixture f([](Fixture& fx) {
            std::string json;
            if (ClientReadRaw(fx.client, json, 10000) == ClientReadResult::Ok) {
                fx.RecordOp(FrameOp(json)); // expect "job" (D1's job1)
            }
            ::Sleep(200);
            if (ClientReadRaw(fx.client, json, 10000) == ClientReadResult::Ok) {
                fx.RecordOp(FrameOp(json)); // expect "abort"
            }
            ::Sleep(300); // job1's terminal lands while D1 is inside the settle
            ClientWriteRaw(fx.client, BuildFinalEvent(1, "TRANSLATION-OF-JOB-ONE"));
            if (ClientReadRaw(fx.client, json, 10000) == ClientReadResult::Ok) {
                fx.RecordOp(FrameOp(json)); // expect "job" (D2's job2, post-settle ONLY)
            }
            ::Sleep(50);
            ClientWriteRaw(fx.client, BuildFinalEvent(2, "translation-of-job-two"));
        });
        hv2::WorkerHandle* w = f.Spawn();
        TEST_CHECK(w != nullptr, "settle S8: handshake completes (manager Ready)");
        if (w) {
            std::atomic<int> d1_phase{0}; // 0 = job1 published, 1 = settling, 2 = released
            std::atomic<bool> d1_acquired{false};
            std::atomic<bool> d2_first_acquire{true};  // must flip to false
            std::atomic<bool> d2_second_acquire{false}; // must flip to true
            std::atomic<bool> d2_got_final{false};
            std::string d2_text;
            hv2::WorkerManager::SettleOutcome d1_outcome =
                hv2::WorkerManager::SettleOutcome::DeadlineExpired;

            // D1: the owner dispatcher (v1-style give-up sequence).
            std::thread d1([&] {
                d1_acquired.store(f.mgr.TrySetBusy(family), std::memory_order_release);
                if (!d1_acquired.load(std::memory_order_acquire)) return;
                wp::JobMsg j1;
                j1.job = 1;
                j1.session = 0;
                j1.src = "ko";
                j1.tgt = "en";
                j1.text = "first";
                (void)f.mgr.SendToWorker(family, wp::BuildJob(j1));
                ::Sleep(300); // job1's deadline passes; the worker is still decoding
                (void)f.mgr.SendToWorker(family, wp::BuildAbort(0));
                d1_phase.store(1, std::memory_order_release);
                d1_outcome = f.mgr.SettleWorkerPipe(family, 5000, nullptr);
                d1_phase.store(2, std::memory_order_release);
                f.mgr.SetBusy(family, false);
            });
            // D2: the sibling dispatcher (v2-style prolog). Must be refused
            // while D1 owns the family, served after the release.
            std::thread d2([&] {
                const int64_t settle_wait = SteadyNowMs() + 5000;
                while (d1_phase.load(std::memory_order_acquire) < 1 &&
                       SteadyNowMs() < settle_wait) {
                    ::Sleep(5);
                }
                d2_first_acquire.store(f.mgr.TrySetBusy(family),
                                       std::memory_order_release); // expect FALSE
                const int64_t release_wait = SteadyNowMs() + 8000;
                while (d1_phase.load(std::memory_order_acquire) < 2 &&
                       SteadyNowMs() < release_wait) {
                    ::Sleep(5);
                }
                d2_second_acquire.store(f.mgr.TrySetBusy(family),
                                        std::memory_order_release); // expect TRUE
                if (!d2_second_acquire.load(std::memory_order_acquire)) return;
                wp::JobMsg j2;
                j2.job = 2;
                j2.session = 0;
                j2.src = "ko";
                j2.tgt = "en";
                j2.text = "second";
                (void)f.mgr.SendToWorker(family, wp::BuildJob(j2));
                const int64_t await_deadline = SteadyNowMs() + 8000;
                while (!d2_got_final.load(std::memory_order_acquire) &&
                       SteadyNowMs() < await_deadline) {
                    std::string json;
                    if (f.mgr.ReadFromWorker(family, json, 250) !=
                        hv2::WorkerManager::WorkerRead::Ok) {
                        continue;
                    }
                    wp::EventMsg ev;
                    const hv2::JobWaitFrame frame = hv2::ClassifyJobWaitFrame(json, ev);
                    if (frame == hv2::JobWaitFrame::Final) {
                        d2_text = ev.text;
                        d2_got_final.store(true, std::memory_order_release);
                    }
                }
                f.mgr.SetBusy(family, false);
            });
            d1.join();
            d2.join();
            TEST_CHECK(d1_acquired.load(std::memory_order_acquire),
                       "settle S8: owner dispatcher acquired the family");
            TEST_CHECK(d1_outcome == hv2::WorkerManager::SettleOutcome::TerminalDiscarded,
                       "settle S8: the settle swallowed the given-up job's late terminal");
            TEST_CHECK(!d2_first_acquire.load(std::memory_order_acquire),
                       "settle S8: sibling publish DURING the settle is refused "
                       "(TrySetBusy fails fast; no sibling frames can race the settle)");
            TEST_CHECK(d2_second_acquire.load(std::memory_order_acquire),
                       "settle S8: sibling publish AFTER the release acquires cleanly");
            TEST_CHECK(d2_got_final.load(std::memory_order_acquire) &&
                           d2_text == "translation-of-job-two",
                       "settle S8: the sibling job's ONLY terminal is NEVER discarded — "
                       "job2 receives job2's own translation");
            const std::vector<std::string> ops = f.Ops();
            TEST_CHECK(ops.size() == 3 && ops[0] == "job" && ops[1] == "abort" &&
                           ops[2] == "job",
                       "settle S8: scripted worker saw job -> abort -> job (no mid-settle publish)");
        }
    }

    // ---- S9: the PipeIoError arm — the worker dies mid-settle ----
    // F-03 (review round 2): the client end drops while the settle is blocked
    // reading; the settle must end PipeIoError (promptly, not at the budget),
    // and must NOT mutate the family state — the reaper owns crash judgment.
    {
        Fixture f([](Fixture& fx) {
            ::Sleep(200); // let the settle block on its first read
            fx.CloseClient();
        });
        hv2::WorkerHandle* w = f.Spawn();
        TEST_CHECK(w != nullptr, "settle S9: handshake completes (manager Ready)");
        if (w) {
            const int64_t t0 = SteadyNowMs();
            const hv2::WorkerManager::SettleOutcome rc =
                f.mgr.SettleWorkerPipe(family, 5000);
            const int64_t elapsed = SteadyNowMs() - t0;
            TEST_CHECK(rc == hv2::WorkerManager::SettleOutcome::PipeIoError,
                       "settle S9: worker death mid-settle -> PipeIoError");
            TEST_CHECK(elapsed < 3000,
                       "settle S9: returns promptly on pipe death (budget not burned)");
            TEST_CHECK(f.mgr.State(family) == hv2::WorkerState::Ready,
                       "settle S9: family state untouched (crash judgment stays with the reaper)");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Worker give-up settle tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Worker give-up settle tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestHostGiveUpSettlePins() {
    std::cout << "[RUN] Testing host give-up settle structural pins (260927_0003)..."
              << std::endl;
    const int failures_before = g_failed_count;

    // The dispatcher loops live in the EmebalaEngine exe (not linkable), so
    // the wiring is pinned the established way: source-text pins through
    // ResolveRepoFile (same approach as the REQ-044 verbatim sync and the
    // req0xx structural suites).
    const std::filesystem::path host_path = ResolveRepoFile(L"src\\host_main.cpp");
    TEST_CHECK(!host_path.empty(), "settle pins: host_main.cpp located");
    if (host_path.empty()) {
        std::cout << "[FAIL] Host give-up settle pins: host_main.cpp missing." << std::endl;
        return;
    }
    std::ifstream host_in(host_path, std::ios::binary);
    const std::string host_src((std::istreambuf_iterator<char>(host_in)),
                               std::istreambuf_iterator<char>());

    const std::filesystem::path wmgr_path = ResolveRepoFile(L"src\\host_v2_worker_manager.hpp");
    TEST_CHECK(!wmgr_path.empty(), "settle pins: host_v2_worker_manager.hpp located");
    std::string wmgr_src;
    if (!wmgr_path.empty()) {
        std::ifstream wmgr_in(wmgr_path, std::ios::binary);
        wmgr_src.assign(std::istreambuf_iterator<char>(wmgr_in),
                        std::istreambuf_iterator<char>());
    }

    // The settle budget: the audit's <= 10 s budget, defined exactly once.
    TEST_CHECK(host_src.find("constexpr int kWorkerSettleMs = 10000;") != std::string::npos,
               "settle pins: kWorkerSettleMs = 10000 defined");
    {
        const std::string needle("kWorkerSettleMs");
        size_t pos = std::string::npos;
        int count = 0;
        while ((pos = host_src.find(needle, pos == std::string::npos ? 0 : pos + 1)) !=
               std::string::npos) {
            ++count;
        }
        TEST_CHECK(count == 3, // definition + v1 call site + v2 call site
                   "settle pins: kWorkerSettleMs referenced exactly where expected");
    }

    // Per-dispatcher pins: exactly ONE settle call, AFTER the abort order and
    // BEFORE the client answer, and only in the give-up branch (the
    // deadline-met Final/Error branches precede it with no settle).
    auto PinDispatcher = [&host_src](const char* begin_marker, const char* end_marker,
                                     const char* answer_marker) {
        const size_t begin = host_src.find(begin_marker);
        const size_t end = host_src.find(end_marker, begin == std::string::npos ? 0 : begin);
        TEST_CHECK(begin != std::string::npos && end != std::string::npos && end > begin,
                   "settle pins: dispatcher region located in host_main.cpp");
        if (begin == std::string::npos || end == std::string::npos || end <= begin) return;
        const std::string region = host_src.substr(begin, end - begin);
        const size_t settle = region.find("SettleWorkerPipe(");
        TEST_CHECK(settle != std::string::npos,
                   "settle pins: dispatcher calls SettleWorkerPipe");
        TEST_CHECK(region.find("SettleWorkerPipe(", settle + 1) == std::string::npos,
                   "settle pins: exactly one settle call in the dispatcher");
        const size_t abort = region.find("wp::BuildAbort(0)");
        TEST_CHECK(abort != std::string::npos && abort < settle,
                   "settle pins: abort ordered before the settle (D-2 chain)");
        const size_t answer = region.find(answer_marker);
        TEST_CHECK(answer != std::string::npos && settle < answer,
                   "settle pins: settle ordered before the client answer");
        const size_t final_branch = region.find("JobWaitFrame::Final");
        TEST_CHECK(final_branch != std::string::npos && final_branch < settle,
                   "settle pins: deadline-met Final path precedes the give-up settle");
        const size_t error_branch = region.find("JobWaitFrame::Error");
        TEST_CHECK(error_branch != std::string::npos && error_branch < settle,
                   "settle pins: deadline-met Error path precedes the give-up settle");

        // 260927_0003 F-01 (review round 2): the single-flight acquire gates
        // EVERY family-pipe touch in the prolog. Exactly one TrySetBusy, it
        // precedes the REQ-049 drain and the job publish, the old no-op
        // SetBusy(true) is gone, and the fail-fast branch answers Busy.
        // B-T4fix1 (session 260928_0001, design §3.6): the per-job pool entry
        // e->family replaced the loop-local `family`, so the same ordering
        // window is re-anchored to TrySetBusy(e->family) — invariant unchanged.
        const size_t acquire = region.find("TrySetBusy(e->family)");
        TEST_CHECK(acquire != std::string::npos,
                   "settle pins: dispatcher acquires the family via TrySetBusy(e->family)");
        TEST_CHECK(region.find("TrySetBusy(e->family)", acquire + 1) == std::string::npos,
                   "settle pins: exactly one TrySetBusy in the dispatcher");
        const size_t drain = region.find("DrainWorkerPipe(e->family");
        TEST_CHECK(drain != std::string::npos && acquire < drain,
                   "settle pins: the single-flight acquire precedes the REQ-049 drain");
        const size_t publish = region.find("SendToWorker(e->family, wp::BuildJob");
        TEST_CHECK(publish != std::string::npos && acquire < publish,
                   "settle pins: the single-flight acquire precedes the job publish");
        TEST_CHECK(region.find("SetBusy(e->family, true)") == std::string::npos &&
                       region.find("SetBusy(family, true)") == std::string::npos,
                   "settle pins: the old no-op SetBusy(true) prolog is gone");
        TEST_CHECK(region.find("HostStatus::Busy") != std::string::npos,
                   "settle pins: the contended publish answers Busy (existing status)");
    };
    PinDispatcher("void DispatcherLoop(host_v2::WorkerManager& wmgr)",
                  "void DispatcherV2Loop(host_v2::WorkerManager& wmgr)",
                  "job.conn->SendResult(job.id, status");
    PinDispatcher("void DispatcherV2Loop(host_v2::WorkerManager& wmgr)",
                  "bool ReadMessage(Connection& conn",
                  // 260930_0003 (Listener finding 3-2): the v2 answer echoes the
                  // client's request id (SchedItem.request_id) instead of the
                  // pre-fix hardcoded 0 — same single answer site, same ordering.
                  "requester->SendResult(item.request_id, status");

    // The manager primitive is declared with the bounded-settle contract.
    TEST_CHECK(wmgr_src.find("enum class SettleOutcome") != std::string::npos,
               "settle pins: SettleOutcome declared in the manager header");
    TEST_CHECK(wmgr_src.find("SettleWorkerPipe(const std::wstring& family, int timeout_ms") !=
                   std::string::npos,
               "settle pins: SettleWorkerPipe declared (bounded, cancel-injectable)");
    TEST_CHECK(wmgr_src.find("bool TrySetBusy(const std::wstring& family);") !=
                   std::string::npos,
               "settle pins: TrySetBusy declared (F-01 single-flight acquire)");
    TEST_CHECK(host_src.find("TrySetBusy(e->family)") != std::string::npos,
               "settle pins: TrySetBusy(e->family) wired in both dispatcher prologs");
    {
        // B-T4fix1 strengthen (design §3.6): the dispatcher prologs must NOT
        // reintroduce the hardcoded per-loop family literal — routing is
        // per-job through the pool entry's e->family.
        const size_t v1_pos = host_src.find("void DispatcherLoop(host_v2::WorkerManager& wmgr)");
        const size_t v2_pos = host_src.find("void DispatcherV2Loop(host_v2::WorkerManager& wmgr)");
        const size_t read_msg_pos = host_src.find("bool ReadMessage(Connection& conn");
        TEST_CHECK(v1_pos != std::string::npos && v2_pos != std::string::npos,
                   "settle pins: both dispatcher loops located for the family-literal pin");
        if (v1_pos != std::string::npos && v2_pos != std::string::npos) {
            const std::string v1_region = host_src.substr(v1_pos, v2_pos - v1_pos);
            const std::string v2_region =
                host_src.substr(v2_pos, (read_msg_pos == std::string::npos || read_msg_pos < v2_pos)
                                            ? std::string::npos
                                            : read_msg_pos - v2_pos);
            TEST_CHECK(v1_region.find("const std::wstring family(kWorkerFamilyTranslate)") ==
                           std::string::npos,
                       "settle pins: v1 prolog no longer hardcodes the translate family literal");
            TEST_CHECK(v2_region.find("const std::wstring family(kWorkerFamilyTranslate)") ==
                           std::string::npos,
                       "settle pins: v2 prolog no longer hardcodes the translate family literal");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Host give-up settle structural pins completed." << std::endl;
    } else {
        std::cout << "[FAIL] Host give-up settle pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
