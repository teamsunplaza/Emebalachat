// ---------------------------------------------------------------------------
// REQ-051 (session 260921, symptom B): the local transient-failure retry
// policy (classification + single-retry warrant) and the Enter-path
// EngineFailed streak surfacing. Staged as an .inc next to this runner;
// registered near the end of main() after TestReq051CaptureRetryPolicy.
//
// Coverage:
//   (a) ClassifyEngineHostFailure over the frozen client's failure-contract
//       codes: connect/busy/io/timeout are Transient; disabled/
//       no_host_binary/unauthorized/version_mismatch/pin_mismatch/
//       bad_request/model_missing/engine_failed are Permanent; empty and
//       unknown codes fail PERMANENT (fail-safe).
//   (b) EngineHostTransientRetryWarranted matrix: transient + budget + fast
//       first attempt + no cancel -> retry; each guard independently kills
//       it (permanent code, spent budget, over-ceiling first attempt,
//       REQ-R16 cancel). Boundary pins at the ceiling (15 s since the
//       REQ-051 U-1 FIX 1 raise — the pins track the shipped constant).
//   (c) The constants: ONE retry, 400 ms backoff (the 300-500 ms handoff
//       band), 15 s first-attempt ceiling (clears the worker's 12 s decode
//       wall-clock timeout inside the frozen 30 s budget).
//   (d) EnterEngineFailureSurfacesModal: strict statuses keep the REQ-047
//       threshold-1 surface; EngineFailed surfaces only on a local leg at
//       the streak threshold; a pure-cloud leg never surfaces.
//   (e) ABSOLUTE SECURITY BOUNDARY (structural pins on engine.cpp): the
//       retry block never invokes the cloud/openai seams, the post-retry
//       convergence still reaches CloudConsentBlocked under a strict-local
//       pick (text stays on-device), and the REQ-R16 cancel re-check guards
//       the post-sleep retry. Same source-inspection technique as
//       TestReq048CloudFailureSignaling / TestReq051CaptureRetryPolicy.
//   (f)+(g) R1-25 (v0.10.4 sweep): an explicit truth table over EVERY token
//       the frozen client emits (connect/busy/io/timeout transient;
//       connect:peer - the R1-7 peer-identity gate - plus no_host_binary/
//       no_token/disabled/unauthorized/version_mismatch/pin_mismatch/
//       bad_request/model_missing/engine_failed and unknown/empty codes
//       permanent), and the warrant evaluated for each code rather than for
//       one representative.
//   (h) R1-25 FIX (U-H4b): the client emits "connect:<oserr>"
//       (engine_host_client.cpp), not the bare "connect" the predicate used to
//       match, so real connect failures never retried. The errno form is
//       Transient now (digits only), "connect:peer" stays Permanent, and the
//       client's token construction is still pinned on its own side so the
//       classifier can never drift away from what is actually emitted.
// ---------------------------------------------------------------------------

void TestReq051EngineRetryPolicy() {
    std::cout << "[RUN] Testing REQ-051 engine retry policy..." << std::endl;
    const int failures_before = g_failed_count;
    using emebalachat::EngineHostFailureClass;

    // ---- (a) transient vs permanent classification -------------------------
    TEST_CHECK(ClassifyEngineHostFailure("connect") == EngineHostFailureClass::Transient &&
                   ClassifyEngineHostFailure("busy") == EngineHostFailureClass::Transient &&
                   ClassifyEngineHostFailure("io") == EngineHostFailureClass::Transient &&
                   ClassifyEngineHostFailure("timeout") == EngineHostFailureClass::Transient,
               "REQ-051: connect/busy/io/timeout classify as TRANSIENT");
    TEST_CHECK(ClassifyEngineHostFailure("disabled") == EngineHostFailureClass::Permanent &&
                   ClassifyEngineHostFailure("no_host_binary") == EngineHostFailureClass::Permanent &&
                   ClassifyEngineHostFailure("unauthorized") == EngineHostFailureClass::Permanent &&
                   ClassifyEngineHostFailure("version_mismatch") == EngineHostFailureClass::Permanent &&
                   ClassifyEngineHostFailure("pin_mismatch") == EngineHostFailureClass::Permanent,
               "REQ-051: handshake/deployment codes are PERMANENT");
    TEST_CHECK(ClassifyEngineHostFailure("bad_request") == EngineHostFailureClass::Permanent &&
                   ClassifyEngineHostFailure("model_missing") == EngineHostFailureClass::Permanent &&
                   ClassifyEngineHostFailure("engine_failed") == EngineHostFailureClass::Permanent,
               "REQ-051: request/decode codes are PERMANENT (retrying a real decode failure only reloads)");
    TEST_CHECK(ClassifyEngineHostFailure("") == EngineHostFailureClass::Permanent &&
                   ClassifyEngineHostFailure("bogus_code") == EngineHostFailureClass::Permanent,
               "REQ-051: empty/unknown codes fail PERMANENT (fail-safe)");

    // ---- (b) the retry warrant matrix --------------------------------------
    TEST_CHECK(EngineHostTransientRetryWarranted("connect", 0, 2000, false),
               "REQ-051: transient fast failure warrants the one retry");
    TEST_CHECK(!EngineHostTransientRetryWarranted("connect", 1, 2000, false),
               "REQ-051: the retry budget (1) is never exceeded");
    TEST_CHECK(!EngineHostTransientRetryWarranted("busy", 0, 30500, false),
               "REQ-051: a first attempt that burned the 30 s budget does NOT "
               "retry (converges to the honest modal instead)");
    // REQ-051 U-1 FIX 1 (session 260922): the ceiling moved 10 s -> 15 s so
    // the retry still fires after the worker-side 12 s decode wall-clock
    // timeout (12 s + 400 ms + 12 s ~= 25 s < the frozen 30 s budget). The
    // boundary pins moved WITH the constant — they exist to catch silent
    // drift, not to freeze a superseded value.
    TEST_CHECK(EngineHostTransientRetryWarranted("io", 0, 15000, false),
               "REQ-051: the first-attempt ceiling is inclusive (15 s)");
    TEST_CHECK(!EngineHostTransientRetryWarranted("connect", 0, 15001, false),
               "REQ-051: past the ceiling the retry is refused");
    TEST_CHECK(!EngineHostTransientRetryWarranted("connect", 0, 2000, true),
               "REQ-051: a REQ-R16 cancel intent outranks the retry");
    TEST_CHECK(!EngineHostTransientRetryWarranted("model_missing", 0, 100, false),
               "REQ-051: a permanent code never warrants a retry");

    // ---- (c) the policy constants -------------------------------------------
    volatile int retry_max = kEngineHostTransientRetryMax;
    TEST_CHECK(retry_max == 1,
               "REQ-051: exactly ONE identical-input retry (never a loop)");
    volatile uint32_t backoff = kEngineHostTransientRetryBackoffMs;
    TEST_CHECK(backoff >= 300 && backoff <= 500,
               "REQ-051: the retry backoff stays inside the 300-500 ms band");
    volatile uint64_t ceiling = kEngineHostRetryFirstAttemptCeilingMs;
    TEST_CHECK(ceiling == 15000,
               "REQ-051 U-1 FIX 1: the fast-failure ceiling is 15 s (clears "
               "the 12 s decode wall-clock timeout)");

    // ---- (d) the Enter-path EngineFailed streak surfacing ------------------
    volatile int threshold = kEnterEngineFailureStreakThreshold;
    TEST_CHECK(threshold == 3,
               "REQ-051: the EngineFailed streak threshold is 3 consecutive failures");
    TEST_CHECK(EnterEngineFailureSurfacesModal(true, false, 1) &&
                   EnterEngineFailureSurfacesModal(true, true, 1),
               "REQ-051: strict local statuses keep the REQ-047 threshold-1 surface");
    TEST_CHECK(!EnterEngineFailureSurfacesModal(false, true, 2),
               "REQ-051: a single transient EngineFailed never surfaces");
    TEST_CHECK(EnterEngineFailureSurfacesModal(false, true, 3) &&
                   EnterEngineFailureSurfacesModal(false, true, 6),
               "REQ-051: a local-leg EngineFailed streak surfaces at/after the threshold");
    TEST_CHECK(!EnterEngineFailureSurfacesModal(false, false, 3) &&
                   !EnterEngineFailureSurfacesModal(false, false, 20),
               "REQ-051: a pure-cloud leg (explicit google/openai) never surfaces "
               "the local modal");

    // ---- (e) the never-newly-cloud boundary (structural) --------------------
    const std::filesystem::path eng_path = ResolveRepoFile(L"src\\engine.cpp");
    TEST_CHECK(!eng_path.empty(),
               "REQ-051: src/engine.cpp located via ResolveRepoFile");
    if (!eng_path.empty()) {
        std::ifstream eng_in(eng_path, std::ios::binary);
        TEST_CHECK(static_cast<bool>(eng_in),
                   "REQ-051: src/engine.cpp opened for reading");
        if (eng_in) {
            const std::string eng_src((std::istreambuf_iterator<char>(eng_in)),
                                      std::istreambuf_iterator<char>());
            TEST_CHECK(!eng_src.empty(), "REQ-051: src/engine.cpp non-empty after read");

            // The retry block, bounded from the first-attempt timing variable
            // (the start of the retry-decision region, BEFORE the /046 log) to
            // the /047 log. The warrant evaluation sits between them: the
            // product computes the first-attempt elapsed, evaluates
            // EngineHostTransientRetryWarranted, logs /046 only when the
            // warrant passed, then sleeps and re-runs the local leg.
            const size_t retry_pos = eng_src.find("const uint64_t first_attempt_ms");
            const size_t retry_end = eng_src.find("ENGINE/Translate/047");
            TEST_CHECK(retry_pos != std::string::npos && retry_end != std::string::npos &&
                           retry_end > retry_pos,
                       "REQ-051 pin: the transient retry block is locatable");
            if (retry_pos != std::string::npos && retry_end != std::string::npos &&
                retry_end > retry_pos) {
                const std::string retry_body =
                    eng_src.substr(retry_pos, retry_end - retry_pos);
                TEST_CHECK(retry_body.find("EngineHostTransientRetryWarranted(") !=
                               std::string::npos,
                           "REQ-051 pin: the retry is gated by the pure warrant");
                TEST_CHECK(retry_body.find("::Sleep(kEngineHostTransientRetryBackoffMs)") !=
                               std::string::npos,
                           "REQ-051 pin: the retry uses the 300-500 ms backoff constant");
                TEST_CHECK(retry_body.find("cancel_requested_.load") != std::string::npos,
                           "REQ-051 pin: the REQ-R16 cancel re-check guards the retry");
                // ABSOLUTE SECURITY BOUNDARY: the retry block only re-runs the
                // local engine-host leg - it must never touch the cloud/openai
                // seams (a strict-local pick with cloud_fallback_enabled=false
                // can therefore never newly transmit text).
                TEST_CHECK(retry_body.find("cloud_call") == std::string::npos &&
                               retry_body.find("openai_call") == std::string::npos &&
                               retry_body.find("GoogleTranslate::Translate") == std::string::npos,
                           "REQ-051 pin: the retry block NEVER invokes the cloud/openai seams");
            }

            // The convergence after the retry still reaches the strict-local
            // honest failure (CloudConsentBlocked) - the security boundary on
            // the far side of the retry.
            const size_t warrant_pos = eng_src.find("EngineHostTransientRetryWarranted(");
            const size_t strict_block = eng_src.find(
                "set_status(TranslationStatus::CloudConsentBlocked);");
            TEST_CHECK(warrant_pos != std::string::npos && strict_block != std::string::npos &&
                           warrant_pos < strict_block,
                       "REQ-051 pin: the strict-local CloudConsentBlocked convergence "
                       "survives AFTER the retry (text stays on-device without consent)");
            // And the consented cloud legs still exist beyond it (Auto /
            // explicit fallback unchanged - the retry only DELAYS them).
            const size_t auto_fallback = eng_src.find("return cloud_call();", retry_end);
            TEST_CHECK(auto_fallback != std::string::npos,
                       "REQ-051 pin: the consented Auto/fallback cloud legs remain");
        }
    }

    // ---- (f) R1-25 (v0.10.4 sweep): exhaustive failure-class truth table ----
    // The frozen client contract (engine_host_client.hpp) enumerates the tokens
    // the local seam can emit. The table below is exhaustive over it and every
    // row is explicit, so a code added to the client later cannot silently
    // inherit a class without a decision recorded here.
    struct WireRow {
        const char* code;
        EngineHostFailureClass expected;
    };
    const WireRow wire_rows[] = {
        // Transient: the client contract documents these as recoverable by an
        // identical-input retry.
        {"connect", EngineHostFailureClass::Transient},
        // R1-25 U-H4b FIX: the client's REAL connect token carries the OS errno
        // (engine_host_client.cpp: "connect:" + std::to_string(last_err)); the
        // bare "connect" row above covers only the empty-candidate case. 2 =
        // ERROR_FILE_NOT_FOUND (no listener yet), 231 = ERROR_PIPE_BUSY (every
        // instance busy): the "orchestrator still booting / spawn in flight"
        // symptom the REQ-051 retry exists for.
        {"connect:2", EngineHostFailureClass::Transient},
        {"connect:231", EngineHostFailureClass::Transient},
        {"busy", EngineHostFailureClass::Transient},
        {"io", EngineHostFailureClass::Transient},
        {"timeout", EngineHostFailureClass::Transient},
        // R1-7 (U-D) peer-identity rejection: PERMANENT. The named pipe answered
        // from a process that is not the installed host image; a 400 ms retry
        // would re-run the same rejection (and re-log the shape-only line).
        {"connect:peer", EngineHostFailureClass::Permanent},
        // Deployment / handshake / request codes: each one has a cause that a
        // second identical request cannot change.
        {"no_host_binary", EngineHostFailureClass::Permanent},
        {"no_token", EngineHostFailureClass::Permanent},
        {"disabled", EngineHostFailureClass::Permanent},
        {"unauthorized", EngineHostFailureClass::Permanent},
        {"version_mismatch", EngineHostFailureClass::Permanent},
        {"pin_mismatch", EngineHostFailureClass::Permanent},
        {"bad_request", EngineHostFailureClass::Permanent},
        {"model_missing", EngineHostFailureClass::Permanent},
        {"engine_failed", EngineHostFailureClass::Permanent},
    };
    int wire_rows_ok = 0;
    for (const WireRow& row : wire_rows) {
        if (ClassifyEngineHostFailure(row.code) == row.expected) {
            ++wire_rows_ok;
        } else {
            std::cout << "  [DETAIL] R1-25 table row disagreed for '" << row.code << "'"
                      << std::endl;
        }
    }
    TEST_CHECK(wire_rows_ok == static_cast<int>(sizeof(wire_rows) / sizeof(wire_rows[0])),
               "R1-25: the failure-class table matches every client token (6 transient, 10 permanent)");
    TEST_CHECK(ClassifyEngineHostFailure("connect:peer") !=
                   ClassifyEngineHostFailure("connect") &&
                   ClassifyEngineHostFailure("connect:peer") !=
                       ClassifyEngineHostFailure("connect:231"),
               "R1-25: connect:peer is PERMANENT while connect and connect:<errno> are transient "
               "(U-D distinction survives any refactor)");

    // ---- (g) R1-25: the warrant over the table, not just one code ----------
    // Every transient code warrants exactly one retry under the fast-failure
    // budget and loses it under each independent guard (cancel, spent budget,
    // over-ceiling first attempt). connect:<errno> is included here (U-H4b): the
    // warrant, not only the classifier, must see the retry actually fire.
    int transient_warrant_ok = 0;
    for (const char* code : {"connect", "connect:2", "connect:231", "busy", "io", "timeout"}) {
        if (EngineHostTransientRetryWarranted(code, 0, 0, false) &&
            !EngineHostTransientRetryWarranted(code, 0, 0, true) &&
            !EngineHostTransientRetryWarranted(code, 1, 0, false) &&
            !EngineHostTransientRetryWarranted(
                code, 0, kEngineHostRetryFirstAttemptCeilingMs + 1, false)) {
            ++transient_warrant_ok;
        }
    }
    TEST_CHECK(transient_warrant_ok == 6,
               "R1-25: all six transient codes (connect, connect:<errno> x2, busy, io, timeout) "
               "warrant the retry, and each guard kills it "
               "(cancel / budget spent / first attempt over the ceiling)");
    int permanent_warrant_ok = 0;
    for (const char* code : {"connect:peer", "no_host_binary", "no_token", "disabled",
                             "unauthorized", "version_mismatch", "pin_mismatch", "bad_request",
                             "model_missing", "engine_failed", "", "ENGINE_FAILED"}) {
        if (!EngineHostTransientRetryWarranted(code, 0, 0, false)) {
            ++permanent_warrant_ok;
        }
    }
    TEST_CHECK(permanent_warrant_ok == 12,
               "R1-25: no permanent/unknown code warrants a retry even on a 0 ms first attempt "
               "(case-sensitive: ENGINE_FAILED is unknown -> fail-safe permanent)");

    // ---- (h) R1-25 FIX (U-H4b): the errno-suffixed connect token -----------
    // The client does NOT emit the bare "connect" for a refused/timed-out pipe:
    // ConnectAndHandshake builds "connect:" + the last OS error
    // (engine_host_client.cpp: `err = peer_rejected ? "connect:peer" : "connect:" +
    // std::to_string(last_err);`); the bare form covers only the empty-candidate
    // case. Until U-H4b the classifier compared the exact string "connect", so
    // every real connect failure classified PERMANENT and the REQ-051 retry -
    // built for exactly the "connect racing the orchestrator's spawn" symptom -
    // never fired for it. The fix folds the errno form into Transient and keeps
    // connect:peer Permanent. No retry count, backoff, or retry path changed.
    TEST_CHECK(ClassifyEngineHostFailure("connect:2") == EngineHostFailureClass::Transient &&
                   ClassifyEngineHostFailure("connect:231") == EngineHostFailureClass::Transient,
               "R1-25 U-H4b: connect:2 (no listener) / connect:231 (all instances busy) are "
               "TRANSIENT now, so the one-shot retry fires for a real connect failure");
    TEST_CHECK(EngineHostTransientRetryWarranted("connect:2", 0, 2000, false) &&
                   !EngineHostTransientRetryWarranted("connect:2", 1, 2000, false) &&
                   !EngineHostTransientRetryWarranted("connect:2", 0, 2000, true),
               "R1-25 U-H4b: connect:<errno> warrants exactly ONE retry (the budget and the "
               "REQ-R16 cancel still outrank it)");
    TEST_CHECK(EngineHostTransientRetryWarranted(
                   "connect:231", 0, kEngineHostRetryFirstAttemptCeilingMs, false) &&
                   !EngineHostTransientRetryWarranted(
                       "connect:231", 0, kEngineHostRetryFirstAttemptCeilingMs + 1, false),
               "R1-25 U-H4b: the 15 s fast-failure ceiling still bounds the connect:errno retry");
    TEST_CHECK(ClassifyEngineHostFailure("connect:peer") == EngineHostFailureClass::Permanent &&
                   !EngineHostTransientRetryWarranted("connect:peer", 0, 0, false),
               "R1-25 U-H4b: connect:peer stays PERMANENT (the R1-7 peer-identity rejection is "
               "never retried, not even on a 0 ms first attempt)");
    // Fail-safe edges of the errno matcher itself: digits only, nothing else.
    TEST_CHECK(IsEngineHostConnectErrnoCode("connect:0") &&
                   IsEngineHostConnectErrnoCode("connect:1234567890") &&
                   !IsEngineHostConnectErrnoCode("connect") &&
                   !IsEngineHostConnectErrnoCode("connect:") &&
                   !IsEngineHostConnectErrnoCode("connect: 2") &&
                   !IsEngineHostConnectErrnoCode("connect:2x") &&
                   !IsEngineHostConnectErrnoCode("connect:-2") &&
                   !IsEngineHostConnectErrnoCode("connect:peer") &&
                   !IsEngineHostConnectErrnoCode("Connect:2") &&
                   !IsEngineHostConnectErrnoCode("connectx:2") &&
                   !IsEngineHostConnectErrnoCode(""),
               "R1-25 U-H4b: IsEngineHostConnectErrnoCode accepts digits only (empty / space / "
               "sign / letters / case / wrong prefix fall back to PERMANENT)");
    // The token construction stays pinned on the CLIENT side, so the classifier
    // can never drift away from what is actually emitted.
    {
        const std::filesystem::path client = ResolveRepoFile(L"src\\engine_host_client.cpp");
        if (client.empty()) {
            std::cout << "[SKIP] src/engine_host_client.cpp not resolvable; R1-25 token "
                         "construction pin skipped." << std::endl;
        } else {
            std::ifstream in(client, std::ios::binary);
            const std::string src((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            TEST_CHECK(src.find("\"connect:\" + std::to_string(last_err)") != std::string::npos,
                       "R1-25 U-H4b: the client still emits connect:<oserr>, the token the "
                       "classifier now treats as Transient");
            TEST_CHECK(src.find("\"connect:peer\"") != std::string::npos,
                       "R1-25: the peer-identity rejection token is the literal connect:peer");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-051 engine retry policy passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-051 engine retry policy: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
