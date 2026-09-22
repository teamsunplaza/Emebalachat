// ---------------------------------------------------------------------------
// REQ-057: the served-model id echo — end-to-end mismatch diagnosability.
// Background: this session's host bug served a user-registered model
// (MiLM-4B) for requests meant for the built-in Hy-MT2 and the app could NOT
// detect it: the v1 result carried no model identity (the welcome SHA pin
// compares the host's HARD-CODED expectation, not the loaded model). The
// worker now echoes the ACTUALLY-SERVED registry id on every terminal event
// (worker_protocol.hpp EventMsg.model, OPTIONAL member per the wire-freeze
// discipline — old<->new interop is unaffected), the orchestrator relays it
// as the OPTIONAL v1 result-frame "model" member, the self-contained client
// exposes it on a TryTranslate overload, and the app compares it against the
// selected engine's expected id (user_gguf -> config.user_model_id via the
// C1-gated live read; anything else -> the bundled registry id) after every
// successful local translate — diagnostic log only, no user-visible notice.
//   (a) behavioral — ResolveBundledModelId (the pinned-default id rule shared
//       by the worker echo and the app's expectation). The EventMsg
//       present/absent round-trip cases live in m6_engine_host_worker_tests
//       (section 8, REQ-057 block) — referenced here, not duplicated.
//   (b) source pins — worker populates ev.model in the job branch (scoped
//       between resolve_active_model and the event build); SendResult splices
//       the optional member onto the frozen BuildResult bytes; both
//       dispatchers plumb ev.model; the client parser extracts the member;
//       engine.cpp carries the mismatch DIAG_F; worker_protocol.hpp declares
//       the EventMsg member; engine_host_protocol.hpp BuildResult itself is
//       untouched (the model member lives ONLY in the host-side splice) while
//       its ParseResult parse-knows the member (the REQ-044 client-subset
//       verbatim invariant).
// ResolveRepoFile source pins (run_tests.cpp L102); unresolvable file ->
// the section is skipped, exactly like the req048_r2 pattern.
// ---------------------------------------------------------------------------

void TestReq057ServedModelEcho() {
    std::cout << "[RUN] Testing REQ-057 served-model echo..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) behavioral: the pinned-default id resolution ------------------
    {
        const std::string doc =
            "{\"schema_version\":1,\"models\":["
            "{\"id\":\"hy-mt2-1.8b-q8\",\"family\":\"ggml-translate\","
            "\"files\":[\"Hy-MT2-1.8B-Q8_0.gguf\"],\"origin\":\"bundled\"},"
            "{\"id\":\"user-alpha\",\"family\":\"ggml-translate\","
            "\"files\":[\"alpha.gguf\"],\"origin\":\"user\"}"
            "]}";
        const auto loaded = engine_host_registry::ParseRegistryJson(doc);
        TEST_CHECK(loaded.status == engine_host_registry::LoadStatus::Ok &&
                       engine_host_registry::ResolveBundledModelId(loaded.registry) ==
                           "hy-mt2-1.8b-q8",
                   "REQ-057 (a): the bundled entry's id resolves as the pinned default");
        // First bundled entry wins when several are registered.
        const std::string two_bundled =
            "{\"schema_version\":1,\"models\":["
            "{\"id\":\"user-alpha\",\"family\":\"ggml-translate\","
            "\"files\":[\"alpha.gguf\"],\"origin\":\"user\"},"
            "{\"id\":\"b-first\",\"family\":\"ggml-translate\","
            "\"files\":[\"b1.gguf\"],\"origin\":\"bundled\"},"
            "{\"id\":\"b-second\",\"family\":\"ggml-translate\","
            "\"files\":[\"b2.gguf\"],\"origin\":\"bundled\"}"
            "]}";
        const auto loaded2 = engine_host_registry::ParseRegistryJson(two_bundled);
        TEST_CHECK(loaded2.status == engine_host_registry::LoadStatus::Ok &&
                       engine_host_registry::ResolveBundledModelId(loaded2.registry) == "b-first",
                   "REQ-057 (a): the FIRST bundled entry wins (registry order)");
        // No bundled entry -> "" (the echo then stays in the pre-REQ-057 shape).
        const std::string user_only =
            "{\"schema_version\":1,\"models\":["
            "{\"id\":\"user-alpha\",\"family\":\"ggml-translate\","
            "\"files\":[\"alpha.gguf\"],\"origin\":\"user\"}"
            "]}";
        const auto loaded3 = engine_host_registry::ParseRegistryJson(user_only);
        TEST_CHECK(loaded3.status == engine_host_registry::LoadStatus::Ok &&
                       engine_host_registry::ResolveBundledModelId(loaded3.registry).empty(),
                   "REQ-057 (a): a user-only registry resolves no pinned id");
        engine_host_registry::Registry empty_reg;
        TEST_CHECK(engine_host_registry::ResolveBundledModelId(empty_reg).empty(),
                   "REQ-057 (a): an empty registry resolves no pinned id");
    }
    // The header-side v1 result parser parse-knows the OPTIONAL member
    // behaviorally (the REQ-044 client-subset invariant anchor): present,
    // absent, and mistyped.
    {
        enginehost::ResultMsg rm;
        TEST_CHECK(enginehost::ParseResult(
                       "{\"op\":\"result\",\"id\":3,\"status\":\"ok\",\"text\":\"hi\","
                       "\"model\":\"user-x\"}",
                       rm) &&
                       rm.model == "user-x" && rm.text == "hi" &&
                       rm.status == enginehost::HostStatus::Ok,
                   "REQ-057 (a): the v1 result parser extracts the optional echo");
        enginehost::ResultMsg rm2;
        TEST_CHECK(enginehost::ParseResult(
                       "{\"op\":\"result\",\"id\":3,\"status\":\"ok\",\"text\":\"hi\"}", rm2) &&
                       rm2.model.empty(),
                   "REQ-057 (a): a pre-REQ-057 result frame parses with an empty echo");
        enginehost::ResultMsg rm3;
        TEST_CHECK(!enginehost::ParseResult(
                       "{\"op\":\"result\",\"id\":3,\"status\":\"ok\",\"text\":\"hi\","
                       "\"model\":123}",
                       rm3),
                   "REQ-057 (a): a mistyped echo member fails the result parse");
    }
    // The EventMsg optional-member round-trip (present/absent/mistyped) is
    // pinned behaviorally in m6_engine_host_worker_tests.inc section 8
    // (REQ-057 block) — extended in place per the step-1 mandate, referenced
    // here instead of duplicating the cases.

    // ---- (b) source pins ----------------------------------------------------
    const std::filesystem::path proto_hpp = ResolveRepoFile(L"src\\worker_protocol.hpp");
    if (proto_hpp.empty()) {
        std::cout << "[SKIP] src/worker_protocol.hpp not resolvable; REQ-057 pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(proto_hpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        // EventMsg declares the OPTIONAL served-model echo member.
        const size_t msg_pos = src.find("struct EventMsg {");
        const size_t msg_end = msg_pos == std::string::npos
                                   ? std::string::npos
                                   : src.find("};", msg_pos);
        const std::string msg_scope =
            (msg_pos != std::string::npos && msg_end != std::string::npos)
                ? src.substr(msg_pos, msg_end - msg_pos)
                : "";
        TEST_CHECK(!msg_scope.empty() && msg_scope.find("std::string model;") != std::string::npos,
                   "REQ-057 (b): EventMsg declares the optional served-model member");
        // BuildEvent emits the member ONLY when populated (optional discipline).
        const size_t build_pos = src.find("inline std::string BuildEvent(");
        const size_t build_end = build_pos == std::string::npos
                                     ? std::string::npos
                                     : src.find("inline std::string BuildClosed(", build_pos);
        const std::string build_scope =
            (build_pos != std::string::npos && build_end != std::string::npos)
                ? src.substr(build_pos, build_end - build_pos)
                : "";
        TEST_CHECK(!build_scope.empty() &&
                       build_scope.find("if (!m.model.empty())") != std::string::npos &&
                       build_scope.find("\\\"model\\\"") != std::string::npos,
                   "REQ-057 (b): BuildEvent emits the echo only when non-empty");
        // ParseEvent extracts the member (strictly typed, mirroring code).
        const size_t parse_pos = src.find("inline bool ParseEvent(");
        const size_t parse_end = parse_pos == std::string::npos
                                     ? std::string::npos
                                     : src.find("inline bool ParseClosed(", parse_pos);
        const std::string parse_scope =
            (parse_pos != std::string::npos && parse_end != std::string::npos)
                ? src.substr(parse_pos, parse_end - parse_pos)
                : "";
        TEST_CHECK(!parse_scope.empty() &&
                       parse_scope.find("FindField(p, \"model\")") != std::string::npos,
                   "REQ-057 (b): ParseEvent extracts the optional echo member");
    }

    const std::filesystem::path worker_cpp = ResolveRepoFile(L"src\\ggml_translate_worker.cpp");
    if (worker_cpp.empty()) {
        std::cout << "[SKIP] src/ggml_translate_worker.cpp not resolvable; REQ-057 pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(worker_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        // The job branch sets ev.model for the terminal event: scoped between
        // the per-job resolve and the event build.
        const size_t job_pos = src.find("resolve_active_model(&model_resolved)");
        const size_t ev_pos = job_pos == std::string::npos
                                  ? std::string::npos
                                  : src.find("wp::BuildEvent(ev)", job_pos);
        const std::string job_scope =
            (job_pos != std::string::npos && ev_pos != std::string::npos)
                ? src.substr(job_pos, ev_pos - job_pos)
                : "";
        TEST_CHECK(!job_scope.empty() &&
                       job_scope.find("ev.model = served_model_id;") != std::string::npos,
                   "REQ-057 (b): the worker populates ev.model in the job branch");
        TEST_CHECK(!job_scope.empty() &&
                       job_scope.find("ResolveBundledModelId") != std::string::npos,
                   "REQ-057 (b): the served-id rule consults the bundled registry id");
    }

    const std::filesystem::path host_main = ResolveRepoFile(L"src\\host_main.cpp");
    if (host_main.empty()) {
        std::cout << "[SKIP] src/host_main.cpp not resolvable; REQ-057 pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(host_main, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        // SendResult splices the optional member onto the frozen BuildResult
        // bytes — status/text stay byte-identical, emitted only when non-empty.
        const size_t sr_pos = src.find("void SendResult(");
        const size_t sr_end = sr_pos == std::string::npos
                                  ? std::string::npos
                                  : src.find("void SendError(", sr_pos);
        const std::string sr_scope =
            (sr_pos != std::string::npos && sr_end != std::string::npos)
                ? src.substr(sr_pos, sr_end - sr_pos)
                : "";
        TEST_CHECK(!sr_scope.empty() && sr_scope.find("BuildResult(m)") != std::string::npos,
                   "REQ-057 (b): SendResult still builds on the frozen BuildResult bytes");
        TEST_CHECK(!sr_scope.empty() && sr_scope.find("\\\"model\\\"") != std::string::npos &&
                       sr_scope.find("!model.empty()") != std::string::npos,
                   "REQ-057 (b): SendResult splices the optional served-model member");
        // Both dispatcher loops plumb the worker's terminal-event echo — the
        // Final AND the Error leg of EACH loop (4 assignment sites).
        {
            size_t pos = src.find("served_model = ev.model;");
            int sites = 0;
            while (pos != std::string::npos) {
                ++sites;
                pos = src.find("served_model = ev.model;", pos + 1);
            }
            TEST_CHECK(sites == 4,
                       "REQ-057 (b): every dispatcher terminal leg plumbs ev.model "
                       "(Final+Error in each of the v1 and v2 loops)");
        }
        TEST_CHECK(src.find("SendResult(job.id, status, out_text, std::move(served_model))") !=
                       std::string::npos &&
                       src.find("SendResult(0, status, out_text, std::move(served_model))") !=
                       std::string::npos,
                   "REQ-057 (b): both SendResult answer paths carry the echo");
    }

    const std::filesystem::path frozen_proto = ResolveRepoFile(L"src\\engine_host_protocol.hpp");
    if (frozen_proto.empty()) {
        std::cout << "[SKIP] src/engine_host_protocol.hpp not resolvable; REQ-057 pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(frozen_proto, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        // The frozen v1 contract stays untouched on the WIRE: BuildResult
        // keeps its exact pre-REQ-057 byte shape (the model member lives ONLY
        // in the host-side splice), and ResultMsg gains no builder field.
        const size_t br_pos = src.find("inline std::string BuildResult(");
        const size_t br_end = br_pos == std::string::npos
                                  ? std::string::npos
                                  : src.find("inline std::string BuildError(", br_pos);
        const std::string br_scope =
            (br_pos != std::string::npos && br_end != std::string::npos)
                ? src.substr(br_pos, br_end - br_pos)
                : "";
        TEST_CHECK(!br_scope.empty() &&
                       br_scope.find("model") == std::string::npos,
                   "REQ-057 (b): the frozen BuildResult carries no model member "
                   "(additive change stays host-side)");
        // Parse-side contract awareness: the header's ParseResult extracts
        // the OPTIONAL member so the self-contained client's reduced copy
        // stays a strict subset of this parser (the REQ-044 verbatim-sync
        // invariant). The builder above stays byte-frozen.
        const size_t pr_pos = src.find("inline bool ParseResult(");
        const size_t pr_end = pr_pos == std::string::npos
                                  ? std::string::npos
                                  : src.find("inline bool ParseError(", pr_pos);
        const std::string pr_scope =
            (pr_pos != std::string::npos && pr_end != std::string::npos)
                ? src.substr(pr_pos, pr_end - pr_pos)
                : "";
        TEST_CHECK(!pr_scope.empty() &&
                       pr_scope.find("FindField(p, \"model\")") != std::string::npos,
                   "REQ-057 (b): the frozen ParseResult parse-knows the optional member "
                   "(client subset invariant preserved)");
    }

    const std::filesystem::path client_cpp = ResolveRepoFile(L"src\\engine_host_client.cpp");
    if (client_cpp.empty()) {
        std::cout << "[SKIP] src/engine_host_client.cpp not resolvable; REQ-057 pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(client_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        const size_t pr_pos = src.find("bool ParseResultWire(");
        const size_t pr_end = pr_pos == std::string::npos
                                  ? std::string::npos
                                  : src.find("bool ParseErrorWire(", pr_pos);
        const std::string pr_scope =
            (pr_pos != std::string::npos && pr_end != std::string::npos)
                ? src.substr(pr_pos, pr_end - pr_pos)
                : "";
        TEST_CHECK(!pr_scope.empty() &&
                       pr_scope.find("FindField(p, \"model\")") != std::string::npos,
                   "REQ-057 (b): the client result parser extracts the optional member");
        const size_t tt_pos = src.find("bool TryTranslate(");
        const size_t tt_end = tt_pos == std::string::npos
                                  ? std::string::npos
                                  : src.find("void SetPathsForTesting(", tt_pos);
        const std::string tt_scope =
            (tt_pos != std::string::npos && tt_end != std::string::npos)
                ? src.substr(tt_pos, tt_end - tt_pos)
                : "";
        TEST_CHECK(!tt_scope.empty() && tt_scope.find("served_model = std::move(rmodel)") !=
                       std::string::npos,
                   "REQ-057 (b): TryTranslate reports the echo on success");
    }

    const std::filesystem::path engine_cpp = ResolveRepoFile(L"src\\engine.cpp");
    if (engine_cpp.empty()) {
        std::cout << "[SKIP] src/engine.cpp not resolvable; REQ-057 pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(engine_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        TEST_CHECK(src.find("served model '%s' != expected '%s' (engine_type=%s)") !=
                       std::string::npos,
                   "REQ-057 (b): the mismatch DIAG_F exists (served vs expected)");
        TEST_CHECK(src.find("LoadUserModelIdFromConfigLive") != std::string::npos,
                   "REQ-057 (b): the expected id uses the C1-gated live user pin");
        TEST_CHECK(src.find("ResolveBundledModelId") != std::string::npos,
                   "REQ-057 (b): the expected id falls back to the bundled registry id");
        // Both success legs (first attempt + transient retry) diagnose.
        {
            size_t pos = src.find("DiagnoseServedModelMismatch(preferred_type_, served_model)");
            int sites = 0;
            while (pos != std::string::npos) {
                ++sites;
                pos = src.find("DiagnoseServedModelMismatch(preferred_type_, served_model)",
                               pos + 1);
            }
            TEST_CHECK(sites == 2,
                       "REQ-057 (b): both local success legs run the mismatch diagnosis");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-057 served-model echo checks passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-057 served-model echo: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
