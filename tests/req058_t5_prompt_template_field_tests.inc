// ---------------------------------------------------------------------------
// REQ-CP T5 (session 260928_0001, design §9 row 7 + §11 T5): the worker
// protocol's OPTIONAL `prompt_template` JobMsg field — BOTH header copies plus
// the two Chat-side connection points (worker pass-through + host stamps).
//
// What this suite pins:
//   1. Parse/build discipline on the Chat copy (the ggml-translate worker's
//      contract): an old frame without the field parses OK with ref "";
//      a mistyped (non-string) member is a MALFORMED job (returns false);
//      a round trip with a non-empty ref preserves the ref byte-for-byte and
//      the builder emits the member ONLY when non-empty (the pre-T5 wire shape
//      is byte-identical when the ref is unset — old<->new interop).
//   2. Worker pass-through: ggml_translate_worker.cpp forwards
//      job.prompt_template into LocalInferenceEngine::Translate's template_ref
//      (the T4 connection point is now connected — the field exists on the
//      struct since T5 added it).
//   3. Host stamps: host_main.cpp sets jm.prompt_template =
//      <policy>.prompt_template_ref in BOTH dispatcher loops (the v1 build
//      from job_policy, the v2 build from item_policy — exactly where T3
//      stamps jm.sampling). The compiled-in resolver default IS
//      "hymt2-official" and BuildJob omits the empty member, so an absent
//      policy file is behavior-identical to the pre-T5 frame.
//   4. The behavioral leg (per the delegation + T3's verification style):
//      seed a temp-lad policy file with the listener section's
//      prompt_template_ref = subtitle-realtime-v1 and assert
//      LoadClientPolicyLive resolves it — this is the host-side seam the
//      dispatcher consumes when stamping the job frame (the same reader the
//      T3 leg already exercises for sampling; A6 honesty: the dispatcher loop
//      itself has no unit seam, so the frame-carrying leg is a source pin).
//   5. Cross-repo JobMsg-surface parity: the two worker_protocol.hpp copies
//      (Chat src + the Listener repo's maintained port) must keep the shared
//      surface — JobMsg, BuildJob, ParseJob — TOKEN-IDENTICAL. The Listener
//      copy is not a whole-file mirror (it is a port with the §7.3 PCM
//      extension + family-constant edits, per its own header comment and the
//      tech gate R-T6 note), so the pin targets the shared surface, not the
//      whole file: extract the `struct JobMsg {`, `BuildJob(const JobMsg&`,
//      and `ParseJob(` blocks from both copies, reduce each to a
//      comment/whitespace/decoration-free token stream (the
//      TestReq044VerbatimSync reducer precedent), and require byte equality.
//
// The cross-repo differential FAILS LOUDLY (never silently skips) when the
// Listener copy cannot be located — a parity pin that skips proves nothing.
// ---------------------------------------------------------------------------

void TestReq058T5PromptTemplateField() {
    std::cout << "[RUN] Testing REQ-CP T5 worker-protocol prompt_template field..."
              << std::endl;
    const int failures_before = g_failed_count;
    namespace wp = emebalachat::workerproto;

    // ---- 1. parse/build pins on the Chat copy --------------------------------
    {
        // Old frame (no prompt_template member) parses OK; the ref defaults to "".
        wp::JobMsg m;
        TEST_CHECK(wp::ParseJob(
                       "{\"op\":\"job\",\"job\":9,\"session\":2,\"kind\":\"translate\","
                       "\"src\":\"ko\",\"tgt\":\"en\",\"text\":\"hello\"}",
                       m) &&
                       m.job == 9 && m.session == 2 && m.src == "ko" && m.tgt == "en" &&
                       m.text == "hello" && m.prompt_template.empty(),
                   "REQ-CP T5: an old job frame (no prompt_template) parses ok, ref \"\"");
        TEST_CHECK(!m.sampling_present, "REQ-CP T5: the old frame leaves sampling_present false");

        // A mistyped (number) prompt_template member is a malformed job.
        wp::JobMsg bad;
        TEST_CHECK(!wp::ParseJob(
                       "{\"op\":\"job\",\"job\":1,\"kind\":\"translate\","
                       "\"prompt_template\":17}",
                       bad),
                   "REQ-CP T5: a mistyped (number) prompt_template member is a malformed job");

        // A round trip with a non-empty ref preserves it byte-for-byte; the
        // builder emits the member ONLY when non-empty (pre-T5 wire shape).
        wp::JobMsg t;
        t.job = 5;
        t.session = 1;
        t.src = "ko";
        t.tgt = "en";
        t.text = "line";
        t.prompt_template = "subtitle-realtime-v1";
        const std::string framed = wp::BuildJob(t);
        TEST_CHECK(framed.find("\"prompt_template\":\"subtitle-realtime-v1\"") !=
                       std::string::npos,
                   "REQ-CP T5: BuildJob emits the non-empty prompt_template member");
        wp::JobMsg back;
        TEST_CHECK(wp::ParseJob(framed, back) && back.prompt_template == "subtitle-realtime-v1",
                   "REQ-CP T5: the non-empty ref round-trips byte-for-byte");

        // Unset ref -> the member is OMITTED (byte-identical to the pre-T5 shape).
        wp::JobMsg u;
        u.job = 6;
        u.src = "ko";
        u.tgt = "en";
        u.text = "x";
        const std::string unframed = wp::BuildJob(u);
        TEST_CHECK(unframed.find("prompt_template") == std::string::npos,
                   "REQ-CP T5: an unset ref keeps the pre-T5 wire shape (member omitted)");
        wp::JobMsg uback;
        TEST_CHECK(wp::ParseJob(unframed, uback) && uback.prompt_template.empty(),
                   "REQ-CP T5: an omitted member parses back to the empty ref");

        // The field is a registry-style template ref id, never user text:
        // the builder escapes it like every other wire string.
        wp::JobMsg esc;
        esc.job = 7;
        esc.src = "ko";
        esc.tgt = "en";
        esc.text = "x";
        esc.prompt_template = "ref\"with\\escape";
        wp::JobMsg esc_back;
        TEST_CHECK(wp::ParseJob(wp::BuildJob(esc), esc_back) &&
                       esc_back.prompt_template == "ref\"with\\escape",
                   "REQ-CP T5: the ref is escaped/ unescaped like any wire string");
    }

    // ---- 2. worker pass-through pin ------------------------------------------
    {
        const std::filesystem::path worker_src = ResolveRepoFile(L"src\\ggml_translate_worker.cpp");
        if (worker_src.empty()) {
            std::cout << "[SKIP] src/ggml_translate_worker.cpp not resolvable; T5 worker pin skipped."
                      << std::endl;
        } else {
            std::ifstream in(worker_src, std::ios::binary);
            const std::string src((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            TEST_CHECK(src.find("job.prompt_template") != std::string::npos,
                       "REQ-CP T5: the worker forwards job.prompt_template into Translate");
            TEST_CHECK(src.find("template_ref=*/job.prompt_template") != std::string::npos,
                       "REQ-CP T5: the forwarded ref is exactly the job's prompt_template member");
            TEST_CHECK(src.find("template_ref=*/\"\"") == std::string::npos,
                       "REQ-CP T5: the T4 empty-ref placeholder is gone (the pass-through is connected)");
        }
    }

    // ---- 3. host stamps + the frame carries the seeded listener ref ----------
    {
        // Behavioral (temp-lad, the T2/T3 fixture discipline): seed the listener
        // section with prompt_template_ref = subtitle-realtime-v1 and assert the
        // live reader resolves it — the seam the dispatcher consumes.
        namespace fs = std::filesystem;
        namespace cp = emebalachat::enginehost::clientpolicy;
        std::error_code ec;
        const fs::path root = fs::temp_directory_path(ec) / L"emebala_req058_t5_prompt_template";
        if (ec) {
            TEST_CHECK(false, "REQ-CP T5 fixture: temp dir unavailable");
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
                       "\"sampling\":{\"temperature\":0.0,\"top_p\":0.6,\"top_k\":20,\"rep_pen\":1.05},"
                       "\"prompt_template_ref\":\"subtitle-realtime-v1\"}}}";
            }
            // Margin for coarse-mtime filesystems (the req055 rewrite discipline).
            ::Sleep(120);
            const cp::ClientPolicy p =
                emebalachat::enginehost::LoadClientPolicyLive("emebala-listner", root.wstring());
            TEST_CHECK(p.prompt_template_ref == "subtitle-realtime-v1",
                       "REQ-CP T5: a seeded listener policy resolves prompt_template_ref "
                       "subtitle-realtime-v1 (the ref the v1 frame carries)");
            // Absent file -> the compiled-in default (the builder omits the member,
            // so the frame is byte-identical to the pre-T5 shape).
            const fs::path root_missing =
                fs::temp_directory_path(ec) / L"emebala_req058_t5_missing";
            const cp::ClientPolicy d =
                emebalachat::enginehost::LoadClientPolicyLive("emebala-listner",
                                                              root_missing.wstring());
            TEST_CHECK(d.prompt_template_ref == cp::kDefaultPromptTemplateRef,
                       "REQ-CP T5: an absent policy file -> the compiled-in hymt2-official default "
                       "(behavior-identical frame)");
            fs::remove_all(root, ec);
            ec.clear();
        }

        // Source pins: both dispatcher loops stamp the resolved ref (the T4
        // report pins the pre-T5 placeholder comment at L498-502; a worker copy
        // that still carries it is a stale port — the T5 hunk is missing).
        const std::filesystem::path host_main = ResolveRepoFile(L"src\\host_main.cpp");
        const std::filesystem::path listener_hdr = ResolveRepoFile(L"..\\Emebala_Listner\\src\\worker_protocol.hpp");
        if (host_main.empty()) {
            std::cout << "[SKIP] src/host_main.cpp not resolvable; T5 host stamps skipped."
                      << std::endl;
        } else {
            std::ifstream in(host_main, std::ios::binary);
            const std::string src((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            const size_t v1_pos = src.find("void DispatcherLoop(");
            const size_t v2_pos = src.find("void DispatcherV2Loop(");
            TEST_CHECK(v1_pos != std::string::npos && v2_pos != std::string::npos &&
                           v2_pos > v1_pos,
                       "REQ-CP T5: both dispatcher loop definitions are present and ordered");
            // v1: the stamp sits inside the v1 build (before the v2 definition).
            TEST_CHECK(src.find("jm.prompt_template = job_policy.prompt_template_ref", v1_pos) <
                           v2_pos,
                       "REQ-CP T5: the v1 job frame stamps the resolved policy prompt_template_ref");
            // v2: the stamp sits inside the v2 build (after the v2 definition).
            TEST_CHECK(src.find("jm.prompt_template = item_policy.prompt_template_ref", v2_pos) !=
                           std::string::npos,
                       "REQ-CP T5: the v2 job frame stamps the resolved policy prompt_template_ref");
        }
        if (listener_hdr.empty()) {
            // Fail loudly: a cross-repo parity pin that skips proves nothing.
            TEST_CHECK(false, "REQ-CP T5 parity: the Listener repo worker_protocol.hpp must be "
                              "locatable (sibling checkout) for the byte-parity pin");
        } else {
            std::ifstream in(listener_hdr, std::ios::binary);
            const std::string lsrc((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            TEST_CHECK(lsrc.find("std::string prompt_template;") != std::string::npos,
                       "REQ-CP T5 parity: the Listener copy carries the JobMsg prompt_template field");
            TEST_CHECK(lsrc.find("\\\"prompt_template\\\"") != std::string::npos,
                       "REQ-CP T5 parity: the Listener copy's builder/parser handle the member");
            TEST_CHECK(lsrc.find("T5 connects to `job.prompt_template`") == std::string::npos,
                       "REQ-CP T5 parity: the Listener copy is not a stale pre-T5 port (the T4 "
                       "placeholder comment is absent)");
        }
    }

    // ---- 4. cross-repo JobMsg-surface token parity ---------------------------
    {
        const std::filesystem::path chat_hdr = ResolveRepoFile(L"src\\worker_protocol.hpp");
        const std::filesystem::path listener_hdr = ResolveRepoFile(L"..\\Emebala_Listner\\src\\worker_protocol.hpp");
        if (chat_hdr.empty() || listener_hdr.empty()) {
            TEST_CHECK(false, "REQ-CP T5 parity: both worker_protocol.hpp copies must be locatable "
                              "for the JobMsg-surface byte-parity pin");
        } else {
            std::ifstream ci(chat_hdr, std::ios::binary);
            std::ifstream li(listener_hdr, std::ios::binary);
            const std::string csrc((std::istreambuf_iterator<char>(ci)),
                                   std::istreambuf_iterator<char>());
            const std::string lsrc((std::istreambuf_iterator<char>(li)),
                                   std::istreambuf_iterator<char>());
            struct SurfaceCase {
                const char* label;
                const char* chat_sig;
                const char* listener_sig;
            };
            const SurfaceCase cases[] = {
                {"JobMsg", "struct JobMsg {", "struct JobMsg {"},
                {"BuildJob", "inline std::string BuildJob(", "inline std::string BuildJob("},
                {"ParseJob", "inline bool ParseJob(", "inline bool ParseJob("},
            };
            for (const auto& kase : cases) {
                std::string cbody, lbody;
                TEST_CHECK(VerbatimExtractBlock(csrc, kase.chat_sig, cbody),
                           std::string("REQ-CP T5 parity: Chat copy defines ") + kase.label);
                TEST_CHECK(VerbatimExtractBlock(lsrc, kase.listener_sig, lbody),
                           std::string("REQ-CP T5 parity: Listener copy defines ") + kase.label);
                std::string cr, lr;
                const bool cok = VerbatimReduceFragment(cbody, cr);
                const bool lok = VerbatimReduceFragment(lbody, lr);
                TEST_CHECK(cok && lok,
                           std::string("REQ-CP T5 parity: both ") + kase.label +
                               " bodies reduce cleanly");
                if (cok && lok) {
                    TEST_CHECK(cr == lr,
                               std::string("REQ-CP T5 parity: ") + kase.label +
                                   " token stream matches across both copies (byte-parity of the "
                                   "shared surface)");
                }
            }
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-CP T5 prompt_template field checks passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-CP T5 prompt_template field: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
