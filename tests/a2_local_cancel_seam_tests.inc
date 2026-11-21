// ---------------------------------------------------------------------------
// 261122_0001 A2: the local-engine REAL-TIME CANCEL SEAM (builder draw 3).
// Staged as an .inc next to this runner; registered at the end of main() after
// the 261121_0002 progress/cancel suite it extends, per the end-of-file
// pattern. Consumes the in-file fake-host helpers (TestPipeName /
// TestTokenPath / WriteTestToken) defined above.
//
// What is being proven:
//   (a) WIRE: the seam sends EXACTLY {"op":"cancel","id":N} — byte-equal to
//       the frozen BuildCancel(id) — on the in-flight connection from another
//       thread while a translate request is blocked, and the host's aborted
//       answer (status=timeout, the frozen abort mapping) returns the blocked
//       TryTranslate promptly.
//   (b) WIRING: a cancel flag set mid-local-translate makes the manager's
//       helper thread fire the seam with the SAME wire id the translate used
//       (observed: the mock's translate id == the mock's cancel id), the call
//       returns promptly, the status is TranslationStatus::Canceled (the
//       261121_0002 path: no failure tone, no modal), and NO repair streak is
//       recorded for the user-aborted request.
//   (c) NEGATIVE: the flag never set -> full in-flight window elapses, the
//       translation completes Ok, and the mock observes NO cancel frame
//       (both a token-carrying ctx and the default ctx).
//   (d) SEAM NEGATIVES: nothing in flight / a non-matching id / an invalid
//       identity all decline WITHOUT touching the pipe (the mock records no
//       frame), and a subsequent well-formed cancel still aborts.
//   (e) SOURCE PINS: engine.hpp declares the seam + the defaulted
//       out_request_id slot; engine.cpp arms the LocalCancelForwarder and
//       passes the slot to BOTH TryTranslate calls, and the early cancel
//       check precedes the repair-signal recorder; the engine repo publishes
//       the in-flight request and defines the seam.
//
// Mock model (A2CancelFakeHost): after parsing a translate frame the server
// issues a BOUNDED read (the in-flight window). A cancel frame inside the
// window with the matching id aborts the job and is answered with the frozen
// status=timeout — the exact answer the real dispatcher gives an aborted job
// (engine-contract.md §7); the window expiring with no cancel answers the
// scripted ok. One server thread, frame-read based, no sleep-and-hope.
// ---------------------------------------------------------------------------

namespace {

// 261122_0001 A2: scripted cancel-aware fake host (see the suite header for
// the model). Single pipe instance; speaks the frozen wire protocol via the
// same enginehost helpers the REQ-043 FakeHost uses.
class A2CancelFakeHost {
public:
    A2CancelFakeHost(std::wstring pipe_name,
                     std::string welcome_sha,
                     int translate_delay_ms,
                     std::string translate_text)
        : pipe_name_(std::move(pipe_name))
        , welcome_sha_(std::move(welcome_sha))
        , translate_delay_ms_(translate_delay_ms)
        , translate_text_(std::move(translate_text)) {
        stop_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        accept_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        // Synchronous ctor-time creation, exactly like FakeHost: the instance
        // must exist before the client's first CreateFileW attempt.
        pipe_ = ::CreateNamedPipeW(pipe_name_.c_str(),
                                   PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE,
                                   1, (1u << 20) + 4, (1u << 20) + 4, 0, nullptr);
        served_ = pipe_ != INVALID_HANDLE_VALUE;
    }
    ~A2CancelFakeHost() {
        if (stop_event_) ::CloseHandle(stop_event_);
        if (accept_event_) ::CloseHandle(accept_event_);
        if (pipe_ && pipe_ != INVALID_HANDLE_VALUE) ::CloseHandle(pipe_);
    }
    A2CancelFakeHost(const A2CancelFakeHost&) = delete;
    A2CancelFakeHost& operator=(const A2CancelFakeHost&) = delete;

    void Stop() { if (stop_event_) ::SetEvent(stop_event_); }
    std::thread Start() { return std::thread([this] { Serve(); }); }

    bool served() const { return served_; }
    // The request is in flight: the translate frame was parsed, the bounded
    // in-flight-window read has been issued.
    bool translate_seen() const { return translate_seen_.load(std::memory_order_acquire); }
    std::uint64_t last_translate_id() const { return last_translate_id_; }
    bool cancel_seen() const { return cancel_seen_.load(std::memory_order_acquire); }
    std::uint64_t last_cancel_id() const { return last_cancel_id_; }
    std::string last_cancel_frame() const { return last_cancel_frame_; }
    int results_sent() const { return results_sent_; }
    std::string last_result_status() const { return last_result_status_; }

private:
    enum class ReadRc { Ok, Timeout, Stopped, IoError };

    // One complete frame as ONE pipe message (same shape as FakeHost).
    bool WriteFrame(const std::string& json) {
        std::string frame;
        emebalachat::enginehost::FrameEncode(json, frame);
        OVERLAPPED ol = {};
        ol.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ol.hEvent) return false;
        DWORD written = 0;
        BOOL ok = ::WriteFile(pipe_, frame.data(), static_cast<DWORD>(frame.size()), &written, &ol);
        if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
            ok = ::GetOverlappedResult(pipe_, &ol, &written, TRUE);
        }
        ::CloseHandle(ol.hEvent);
        return ok && written == frame.size();
    }

    // Bounded frame read with a stop wake. A fresh event per call: after a
    // cancel-ack the event may be left signaled, and a reused manual-reset
    // event would false-trigger the NEXT wait (the FakeHost one-shot event
    // discipline, kept verbatim).
    ReadRc ReadFrameTimed(std::string& json, DWORD timeout_ms) {
        static thread_local std::vector<char> buf((1u << 20) + 4);
        OVERLAPPED ol = {};
        ol.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ol.hEvent) return ReadRc::IoError;
        DWORD read = 0;
        BOOL ok = ::ReadFile(pipe_, buf.data(), static_cast<DWORD>(buf.size()), &read, &ol);
        if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
            const HANDLE waits[2] = {ol.hEvent, stop_event_};
            const DWORD wr = ::WaitForMultipleObjects(2, waits, FALSE, timeout_ms);
            if (wr == WAIT_OBJECT_0 + 1) {
                ::CancelIoEx(pipe_, &ol); // stop: cancel + kernel cancel-ack
                DWORD discarded = 0;
                (void)::GetOverlappedResult(pipe_, &ol, &discarded, TRUE);
                ::CloseHandle(ol.hEvent);
                return ReadRc::Stopped;
            }
            if (wr == WAIT_TIMEOUT) {
                ::CancelIoEx(pipe_, &ol); // window expired: same cancel-ack rule
                DWORD discarded = 0;
                (void)::GetOverlappedResult(pipe_, &ol, &discarded, TRUE);
                ::CloseHandle(ol.hEvent);
                return ReadRc::Timeout;
            }
            if (wr != WAIT_OBJECT_0) {
                ::CloseHandle(ol.hEvent);
                return ReadRc::IoError;
            }
            DWORD got = 0;
            if (!::GetOverlappedResult(pipe_, &ol, &got, FALSE)) {
                ::CloseHandle(ol.hEvent);
                return ReadRc::IoError;
            }
            read = got;
        } else if (!ok) {
            ::CloseHandle(ol.hEvent);
            return ReadRc::IoError;
        }
        ::CloseHandle(ol.hEvent);
        if (read < 4) return ReadRc::IoError;
        std::uint32_t len = 0;
        emebalachat::enginehost::FrameReadLengthPrefix(buf.data(), len);
        if (static_cast<size_t>(len) + 4 != read) return ReadRc::IoError;
        json.assign(buf.data() + 4, static_cast<size_t>(len));
        return ReadRc::Ok;
    }

    void Serve() {
        if (!served_) return;

        OVERLAPPED ol = {};
        ol.hEvent = accept_event_;
        BOOL ok = ::ConnectNamedPipe(pipe_, &ol);
        if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
            const HANDLE waits[2] = {accept_event_, stop_event_};
            if (::WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
                return; // stopped before any client arrived
            }
            DWORD dummy = 0;
            if (!::GetOverlappedResult(pipe_, &ol, &dummy, TRUE)) return;
        } else if (!ok && ::GetLastError() != ERROR_PIPE_CONNECTED) {
            return;
        }

        // Handshake: hello -> welcome (the client's pin check is part of the
        // seam under test, so the welcome carries the caller-supplied sha).
        std::string frame;
        if (ReadFrameTimed(frame, 30000) != ReadRc::Ok) return;
        emebalachat::enginehost::WelcomeMsg w;
        w.protocol = 1;
        w.model = "Hy-MT2-1.8B-Q8_0";
        w.model_sha256 = welcome_sha_;
        w.capabilities = {"translate"};
        if (!WriteFrame(emebalachat::enginehost::BuildWelcome(w))) return;

        for (;;) {
            std::string msg;
            const ReadRc rc = ReadFrameTimed(msg, static_cast<DWORD>(translate_delay_ms_));
            if (rc != ReadRc::Ok) return; // idle window / stop / IO: session over
            emebalachat::enginehost::JsonPairs fields;
            if (!emebalachat::enginehost::JsonParseObject(msg, fields)) return;
            const auto* op = emebalachat::enginehost::detail::FindField(fields, "op");
            if (!op) return;
            if (op->text == "translate") {
                emebalachat::enginehost::TranslateMsg m;
                if (!emebalachat::enginehost::ParseTranslate(msg, m)) return;
                last_translate_id_ = m.id;
                translate_seen_.store(true, std::memory_order_release);
                // In-flight window: the client is blocked awaiting the result.
                // A cancel for THIS id inside the window aborts the job (the
                // frozen status=timeout answer); the window expiring answers
                // the scripted ok. This is exactly the real dispatcher's
                // observable behavior (engine-contract.md §7, C4).
                std::string next;
                const ReadRc wc =
                    ReadFrameTimed(next, static_cast<DWORD>(translate_delay_ms_));
                if (wc == ReadRc::Stopped || wc == ReadRc::IoError) return;
                bool aborted = false;
                if (wc == ReadRc::Ok) {
                    emebalachat::enginehost::JsonPairs f2;
                    if (!emebalachat::enginehost::JsonParseObject(next, f2)) return;
                    const auto* op2 = emebalachat::enginehost::detail::FindField(f2, "op");
                    if (!op2) return;
                    if (op2->text != "cancel") return; // drift: only cancel fits the window
                    emebalachat::enginehost::CancelMsg cm;
                    if (!emebalachat::enginehost::ParseCancel(next, cm)) return;
                    last_cancel_frame_ = next;
                    last_cancel_id_ = cm.id;
                    cancel_seen_.store(true, std::memory_order_release);
                    aborted = (cm.id == last_translate_id_);
                }
                emebalachat::enginehost::ResultMsg r;
                r.id = last_translate_id_;
                r.status = aborted ? emebalachat::enginehost::HostStatus::Timeout
                                   : emebalachat::enginehost::HostStatus::Ok;
                r.text = aborted ? "" : translate_text_;
                if (!WriteFrame(emebalachat::enginehost::BuildResult(r))) return;
                ++results_sent_;
                last_result_status_ = aborted ? "timeout" : "ok";
            } else if (op->text == "cancel") {
                // Cancel while NOTHING is in flight: the frozen contract says
                // IGNORED (the connection stays open; §4.4 idempotent no-op).
                // Record it and keep serving.
                emebalachat::enginehost::CancelMsg cm;
                if (!emebalachat::enginehost::ParseCancel(msg, cm)) return;
                last_cancel_frame_ = msg;
                last_cancel_id_ = cm.id;
                cancel_seen_.store(true, std::memory_order_release);
            } else {
                return;
            }
        }
    }

    std::wstring pipe_name_;
    std::string welcome_sha_;
    int translate_delay_ms_;
    std::string translate_text_;
    HANDLE pipe_ = nullptr;
    HANDLE stop_event_ = nullptr;
    HANDLE accept_event_ = nullptr;
    bool served_ = false;
    std::atomic<bool> translate_seen_{false};
    std::uint64_t last_translate_id_ = 0;
    std::atomic<bool> cancel_seen_{false};
    std::uint64_t last_cancel_id_ = 0;
    std::string last_cancel_frame_;
    int results_sent_ = 0;
    std::string last_result_status_;
};

} // namespace

void TestA2LocalCancelSeam() {
    std::cout << "[RUN] Testing 261122_0001 A2 local-engine real-time cancel seam..."
              << std::endl;
    const int failures_before = g_failed_count;
    namespace ehc = emebalachat::engine_host;
    using emebalachat::EngineHostConfig;
    using emebalachat::EngineType;
    using emebalachat::TranslationManager;
    using emebalachat::TranslationStatus;
    using emebalachat::TranslationTaskContext;

    const std::string kGoodPin(emebalachat::kExpectedModelSha256);
    // The full in-flight hold; every promptness bound sits far below it, so a
    // silently-non-aborting seam fails its bound instead of passing slowly.
    const int kHoldMs = 6000;
    const uint64_t kPromptBoundMs = 4000;

    wchar_t self_exe[MAX_PATH] = {0};
    ::GetModuleFileNameW(nullptr, self_exe, MAX_PATH);

    auto elapsed_ms = [](std::chrono::steady_clock::time_point t0) {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count());
    };
    auto finish_host = [](A2CancelFakeHost& h, std::thread& t) {
        h.Stop();
        if (t.joinable()) t.join();
    };
    // Hermetic guard for the manager scenarios: a wiring bug that fell
    // through to the cloud must fail as EngineFailed, never touch the
    // network, and never be mistaken for Canceled.
    struct CloudBlockGuard {
        CloudBlockGuard() { emebalachat::GoogleTranslate::SetCloudBlockedForTesting(true); }
        ~CloudBlockGuard() { emebalachat::GoogleTranslate::SetCloudBlockedForTesting(false); }
    };

    // ---- (e) source pins ----------------------------------------------------
    {
        auto read_src = [](const wchar_t* rel, std::string& out) {
            const std::filesystem::path p = ResolveRepoFile(rel);
            if (p.empty()) return false;
            std::ifstream in(p, std::ios::binary);
            out.assign((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
            return !out.empty();
        };
        auto count_of = [](const std::string& hay, const std::string& needle) {
            size_t n = 0;
            for (size_t p = hay.find(needle); p != std::string::npos;
                 p = hay.find(needle, p + needle.size())) {
                ++n;
            }
            return n;
        };
        std::string engine_hpp, engine_cpp, client_hpp, client_cpp;
        const bool have_engine = read_src(L"src\\engine.hpp", engine_hpp) &&
                                 read_src(L"src\\engine.cpp", engine_cpp);
        const bool have_client = read_src(L"src\\engine_host_client.hpp", client_hpp) &&
                                 read_src(L"src\\engine_host_client.cpp", client_cpp);
        TEST_CHECK(have_engine && have_client,
                   "261122_0001 A2: engine.hpp/engine.cpp and the engine-repo client "
                   "sources resolve from the test CWD (run the suite from the REPO ROOT)");
        if (have_engine) {
            // The seam is declared by the consumer side (REQ-060 precedent) with
            // the required, default-free identity argument.
            TEST_CHECK(engine_hpp.find(
                           "bool CancelTranslateRequest(const EngineHostClientIdentity& identity,") !=
                           std::string::npos,
                       "261122_0001 A2 pin: engine.hpp declares the cancel seam (identity "
                       "required, first)");
            // The request-id exposure is a DEFAULTED trailing out-param, so every
            // existing TryTranslate call site stays source-compatible.
            TEST_CHECK(engine_hpp.find("std::atomic<std::uint64_t>* out_request_id = nullptr") !=
                           std::string::npos,
                       "261122_0001 A2 pin: the pinned overload exposes the wire id through "
                       "a defaulted out-param (source compatibility)");
            // The wiring: forwarder armed only when a token exists, and the slot
            // handed to BOTH pipe attempts (first + REQ-051 retry).
            TEST_CHECK(engine_cpp.find("class LocalCancelForwarder") != std::string::npos,
                       "261122_0001 A2 pin: engine.cpp carries the bounded cancel forwarder");
            TEST_CHECK(engine_cpp.find("cancel_forwarder.Start(ctx.cancel)") != std::string::npos,
                       "261122_0001 A2 pin: the forwarder arms only when the request carries "
                       "a cancel token");
            TEST_CHECK(count_of(engine_cpp, "cancel_id_slot))") == 2,
                       "261122_0001 A2 pin: the id slot reaches BOTH TryTranslate call sites "
                       "(first attempt + transient retry)");
            // The aborted answer must not arm the repair signal: the early cancel
            // check converges to Canceled BEFORE the NoteHostFailure recorder.
            const size_t early = engine_cpp.find(
                "261122_0001 A2: a failure observed after the user token flipped");
            const size_t nh = engine_cpp.find("const int fail_streak = NoteHostFailure();");
            TEST_CHECK(early != std::string::npos && nh != std::string::npos && early < nh,
                       "261122_0001 A2 pin: the mid-flight cancel check precedes the "
                       "repair-signal recorder (no spurious streak on a user abort)");
        }
        if (have_client) {
            TEST_CHECK(client_hpp.find(
                           "bool CancelTranslateRequest(const EngineHostClientIdentity& identity,") !=
                           std::string::npos,
                       "261122_0001 A2 pin: the engine-repo client header declares the seam");
            TEST_CHECK(client_cpp.find("InFlightPublishGuard in_flight_publish(") !=
                           std::string::npos,
                       "261122_0001 A2 pin: the translate path publishes the in-flight "
                       "{id, pipe} pair for the seam");
            TEST_CHECK(client_cpp.find("if (out_request_id)") != std::string::npos,
                       "261122_0001 A2 pin: the wire id is published through the out-param "
                       "before the blocking read");
            TEST_CHECK(client_cpp.find("bool CancelTranslateRequest(") != std::string::npos,
                       "261122_0001 A2 pin: the seam is defined in the engine repo client");
        }
    }

    // ---- (a) wire: the seam sends exactly {"op":"cancel","id":N} -------------
    {
        const int n = 80;
        ehc::SetPathsForTesting(TestPipeName(n).c_str(),
                                TestTokenPath(n).wstring().c_str(), self_exe);
        WriteTestToken(TestTokenPath(n));
        A2CancelFakeHost host(TestPipeName(n), kGoodPin, kHoldMs, "a2-should-be-discarded");
        std::thread host_thread = host.Start();

        EngineHostConfig cfg;
        cfg.spawn = false;
        std::atomic<std::uint64_t> id_slot{0};
        std::string out, err, served;
        bool tr_ok = true;
        const auto t0 = std::chrono::steady_clock::now();
        // The translate runs on its own thread — this test thread is the
        // "separate thread" the seam fires from, exactly like the manager's
        // helper thread in production.
        std::thread tr([&] {
            tr_ok = ehc::TryTranslate(ehc::kChatEngineHostIdentity, cfg, "ko", "en",
                                      "cancel me mid-flight", std::string() /*unpinned*/,
                                      out, err, served, &id_slot);
        });
        const bool saw_translate = [&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!host.translate_seen() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return host.translate_seen();
        }();
        TEST_CHECK(saw_translate,
                   "A2 (a): the fake host observed the translate frame (request in flight)");
        // The wire id must be published WHILE the call is blocked.
        const auto id_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (id_slot.load(std::memory_order_acquire) == 0 &&
               std::chrono::steady_clock::now() < id_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const std::uint64_t published_id = id_slot.load(std::memory_order_acquire);
        TEST_CHECK(published_id != 0,
                   "A2 (a): TryTranslate published the wire id before blocking on the result");
        TEST_CHECK(host.last_translate_id() == published_id,
                   "A2 (a): the published id IS the id on the translate frame");

        const bool cancel_sent = ehc::CancelTranslateRequest(ehc::kChatEngineHostIdentity,
                                                             published_id);
        TEST_CHECK(cancel_sent, "A2 (a): the seam accepted the cancel (in-flight match)");
        tr.join();
        const uint64_t elapsed = elapsed_ms(t0);

        // The host's aborted answer is the frozen status=timeout mapping.
        TEST_CHECK(!tr_ok && err == "timeout",
                   "A2 (a): the aborted request returns promptly with the host's timeout "
                   "answer (the manager maps this to Canceled)");
        TEST_CHECK(host.cancel_seen(), "A2 (a): the cancel frame reached the host");
        TEST_CHECK(host.last_cancel_id() == published_id,
                   "A2 (a): the cancel carries the SAME id the translate used");
        TEST_CHECK(host.last_cancel_frame() == emebalachat::enginehost::BuildCancel(published_id),
                   "A2 (a): the frame is byte-exact {\"op\":\"cancel\",\"id\":N} "
                   "(frozen BuildCancel)");
        TEST_CHECK(host.last_cancel_frame() ==
                       std::string("{\"op\":\"cancel\",\"id\":") + std::to_string(published_id) +
                           "}",
                   "A2 (a): the literal frame shape is exactly the spec string");
        TEST_CHECK(elapsed < kPromptBoundMs,
                   "A2 (a): blocked TryTranslate returned promptly after the seam fired "
                   "(well inside the full-hold window)");
        TEST_CHECK(host.results_sent() == 1, "A2 (a): exactly one result answered");
        finish_host(host, host_thread);
        std::error_code rm_ec;
        std::filesystem::remove(TestTokenPath(n), rm_ec);
    }

    // ---- (b) wiring: flag mid-translate -> seam with the same id, Canceled --
    {
        CloudBlockGuard cloud_guard;
        const int n = 81;
        ehc::SetPathsForTesting(TestPipeName(n).c_str(),
                                TestTokenPath(n).wstring().c_str(), self_exe);
        WriteTestToken(TestTokenPath(n));
        A2CancelFakeHost host(TestPipeName(n), kGoodPin, kHoldMs, "a2-should-be-discarded");
        std::thread host_thread = host.Start();

        EngineHostConfig cfg;
        cfg.spawn = false;
        TranslationManager mgr(EngineType::LocalLlama, "");
        mgr.SetEngineHostConfig(cfg);
        TEST_CHECK(mgr.GetActiveEngineName() == "Hy-MT2-1.8B (Shared Host)",
                   "A2 (b): strict local + deployed fake host names the shared-host leg");

        std::atomic<bool> user_cancel{false};
        TranslationTaskContext ctx;
        ctx.cancel = &user_cancel;
        TranslationStatus st = TranslationStatus::Ok;
        std::wstring res;
        const auto t0 = std::chrono::steady_clock::now();
        std::thread req([&] { res = mgr.Translate(L"hello", "KO", "EN", &st, ctx); });
        const bool saw_translate = [&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!host.translate_seen() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return host.translate_seen();
        }();
        TEST_CHECK(saw_translate, "A2 (b): the manager's request reached the host (in flight)");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        user_cancel.store(true, std::memory_order_release); // the user aborts mid-flight
        req.join();
        const uint64_t elapsed = elapsed_ms(t0);

        TEST_CHECK(res.empty() && st == TranslationStatus::Canceled,
                   "A2 (b): the mid-flight cancel converges to TranslationStatus::Canceled "
                   "(no tone, no modal — the 261121_0002 path)");
        TEST_CHECK(host.cancel_seen(),
                   "A2 (b): the manager's helper thread fired the seam");
        TEST_CHECK(host.last_cancel_id() == host.last_translate_id(),
                   "A2 (b): the seam fired with the SAME wire id the translate used");
        TEST_CHECK(host.last_cancel_frame() ==
                       emebalachat::enginehost::BuildCancel(host.last_translate_id()),
                   "A2 (b): the fired frame is the exact frozen cancel shape");
        TEST_CHECK(elapsed < kPromptBoundMs,
                   "A2 (b): the cancelled request returned promptly (not the full hold)");
        TEST_CHECK(mgr.HostFailureStreak() == 0,
                   "A2 (b): a user-aborted request records NO repair-signal streak");
        TEST_CHECK(host.results_sent() == 1, "A2 (b): exactly one result answered");
        finish_host(host, host_thread);
        std::error_code rm_ec;
        std::filesystem::remove(TestTokenPath(n), rm_ec);
    }

    // ---- (c) negative: no cancel -> the full window elapses, Ok, no frame ---
    {
        CloudBlockGuard cloud_guard;
        // (c1) a token that is never set
        {
            const int n = 82;
            ehc::SetPathsForTesting(TestPipeName(n).c_str(),
                                    TestTokenPath(n).wstring().c_str(), self_exe);
            WriteTestToken(TestTokenPath(n));
            A2CancelFakeHost host(TestPipeName(n), kGoodPin, 300, "a2-ok");
            std::thread host_thread = host.Start();
            EngineHostConfig cfg;
            cfg.spawn = false;
            TranslationManager mgr(EngineType::LocalLlama, "");
            mgr.SetEngineHostConfig(cfg);

            std::atomic<bool> quiet_flag{false};
            TranslationTaskContext ctx;
            ctx.cancel = &quiet_flag;
            TranslationStatus st = TranslationStatus::Canceled;
            const auto t0 = std::chrono::steady_clock::now();
            const std::wstring res = mgr.Translate(L"hello", "KO", "EN", &st, ctx);
            const uint64_t elapsed = elapsed_ms(t0);
            TEST_CHECK(res == L"a2-ok" && st == TranslationStatus::Ok,
                       "A2 (c1): without a cancel the translation completes Ok");
            TEST_CHECK(!host.cancel_seen(),
                       "A2 (c1): NO cancel frame left the client when the flag never set");
            TEST_CHECK(host.results_sent() == 1,
                       "A2 (c1): exactly one result answered");
            TEST_CHECK(elapsed >= 200,
                       "A2 (c1): the request really rode out the in-flight window "
                       "(an instant return would mean the cancel path ran)");
            TEST_CHECK(mgr.HostFailureStreak() == 0,
                       "A2 (c1): the successful leg leaves the repair signal clear");
            finish_host(host, host_thread);
            std::error_code rm_ec;
            std::filesystem::remove(TestTokenPath(n), rm_ec);
        }
        // (c2) no token at all (the pre-feature call shape)
        {
            const int n = 83;
            ehc::SetPathsForTesting(TestPipeName(n).c_str(),
                                    TestTokenPath(n).wstring().c_str(), self_exe);
            WriteTestToken(TestTokenPath(n));
            A2CancelFakeHost host(TestPipeName(n), kGoodPin, 300, "a2-ok-default");
            std::thread host_thread = host.Start();
            EngineHostConfig cfg;
            cfg.spawn = false;
            TranslationManager mgr(EngineType::LocalLlama, "");
            mgr.SetEngineHostConfig(cfg);

            TranslationStatus st = TranslationStatus::Canceled;
            const std::wstring res = mgr.Translate(L"hello", "KO", "EN", &st);
            TEST_CHECK(res == L"a2-ok-default" && st == TranslationStatus::Ok,
                       "A2 (c2): the default ctx keeps the byte-identical pre-feature path");
            TEST_CHECK(!host.cancel_seen(),
                       "A2 (c2): the default ctx arms no forwarder and sends no cancel");
            finish_host(host, host_thread);
            std::error_code rm_ec;
            std::filesystem::remove(TestTokenPath(n), rm_ec);
        }
    }

    // ---- (d) seam negatives: decline without touching the pipe --------------
    {
        const int n = 84;
        ehc::SetPathsForTesting(TestPipeName(n).c_str(),
                                TestTokenPath(n).wstring().c_str(), self_exe);
        WriteTestToken(TestTokenPath(n));
        A2CancelFakeHost host(TestPipeName(n), kGoodPin, kHoldMs, "a2-neg");
        std::thread host_thread = host.Start();

        // Nothing in flight -> best-effort decline, no crash, no I/O.
        TEST_CHECK(!ehc::CancelTranslateRequest(ehc::kChatEngineHostIdentity, 1),
                   "A2 (d): a cancel with nothing in flight declines (best-effort no-op)");
        // An invalid identity is rejected before any pipe I/O (repo rule).
        const ehc::EngineHostClientIdentity bad_identity{"not-a-real-app", "1.0.0"};
        TEST_CHECK(!ehc::CancelTranslateRequest(bad_identity, 1),
                   "A2 (d): an invalid identity declines before any pipe I/O");

        EngineHostConfig cfg;
        cfg.spawn = false;
        std::atomic<std::uint64_t> id_slot{0};
        std::string out, err, served;
        bool tr_ok = true;
        std::thread tr([&] {
            tr_ok = ehc::TryTranslate(ehc::kChatEngineHostIdentity, cfg, "ko", "en",
                                      "cancel seam negatives", std::string(), out, err,
                                      served, &id_slot);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while ((!host.translate_seen() ||
                id_slot.load(std::memory_order_acquire) == 0) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const std::uint64_t in_flight_id = id_slot.load(std::memory_order_acquire);
        TEST_CHECK(host.translate_seen() && in_flight_id != 0,
                   "A2 (d): the negative-scenario request is in flight with a published id");

        // A non-matching id declines WITHOUT writing (the mock's in-flight
        // read would record any frame that actually arrived).
        TEST_CHECK(!ehc::CancelTranslateRequest(ehc::kChatEngineHostIdentity,
                                                in_flight_id + 1),
                   "A2 (d): a non-matching request id declines");
        TEST_CHECK(!ehc::CancelTranslateRequest(bad_identity, in_flight_id),
                   "A2 (d): the invalid identity declines even with a live in-flight id");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        TEST_CHECK(!host.cancel_seen(),
                   "A2 (d): the declined cancels wrote NOTHING to the pipe");

        // The well-formed cancel still aborts afterwards (state not poisoned).
        TEST_CHECK(ehc::CancelTranslateRequest(ehc::kChatEngineHostIdentity, in_flight_id),
                   "A2 (d): a subsequent well-formed cancel is accepted");
        tr.join();
        TEST_CHECK(!tr_ok && err == "timeout",
                   "A2 (d): the accepted cancel aborts the in-flight request");
        TEST_CHECK(host.cancel_seen() && host.last_cancel_id() == in_flight_id,
                   "A2 (d): only the matching cancel frame reached the host");
        finish_host(host, host_thread);
        std::error_code rm_ec;
        std::filesystem::remove(TestTokenPath(n), rm_ec);
    }

    // Restore the process-wide absent sentinel for any suite registered after
    // this one (the established pattern of the R1-11 suite).
    ehc::SetPathsForTesting(nullptr, nullptr, L"\\\\?\\NONEXISTENT_EME_HOST\\engine.exe");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261122_0001 A2 local cancel seam checks passed." << std::endl;
    } else {
        std::cout << "[FAIL] 261122_0001 A2 local cancel seam: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
