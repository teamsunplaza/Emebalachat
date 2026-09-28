// ---------------------------------------------------------------------------
// REQ-CP T2 (session 260928_0001, design §11 T2): the LIVE per-client engine
// policy reader — LoadClientPolicyLive over %LOCALAPPDATA%\Emebala\Common\
// engine_client_policy.json. Extends the REQ-055 mtime/size single-slot live
// cache pattern from the C1 user-model pin to the family-shared policy file,
// resolving each job through the T1 pure core (clientpolicy::ResolvePolicy).
//
// Coverage (the §11 T2 pin list):
//   (a)  mtime/size cache HIT — an unchanged file is served from the cached
//        CONTENT without a re-read: rotate the client id between two calls on
//        the SAME unchanged file and observe the per-id resolution (chat vs
//        listner) — a content-level (not resolution-level) cache yields both,
//        a resolution-level cache would mis-serve the second id.
//   (b)  mtime/size cache MISS — a content change moves the signature and the
//        new policy is picked up live (no host respawn).
//   (c)  file deleted between calls -> compiled-in defaults, no crash, and a
//        later re-created file re-seeds normally (REQ-055 attribute-failure
//        bypass discipline).
//   (d)  torn write — a partial document written WITHOUT the writer's
//        tmp+rename commit (truncate mid-JSON) -> the reader is fail-closed
//        to defaults for that call and NEVER crashes; a following good write
//        restores service. (Rename atomicity is the WRITER's job; this reader
//        only guarantees no crash + fail-closed on garbage.)
//   (e)  absent store / never-created dir -> defaults, no crash (first-run
//        state is normal, the reader never creates the file or dir).
//   (f)  lad_override cache isolation — two distinct lad roots each resolve
//        their own file (single-slot cache keyed by the resolved lad dir).
//
// Fixture: temp lad roots + <root>\Emebala\Common\engine_client_policy.json,
// mirroring the req055_live_pin_tests.inc / req051_json_loader temp-dir seam.
// The T1 resolver core (engine_host_client_policy_test.cpp) owns the parse
// semantics; these pins only cover the LIVE layer (cache + fail-closure).
//
// Staged as an .inc next to tests/run_tests.cpp; registered near the end of
// main() right after TestEngineHostClientPolicy(), per the end-of-file pattern.
// ---------------------------------------------------------------------------

namespace {

// The full three-client policy document (design §5 golden), used to prove the
// cache carries CONTENT (per-client sections), not one frozen resolution.
const char* kReq056GoldenPolicy =
    "{"
    "\"schema_version\":1,"
    "\"clients\":{"
    "\"emebala-chat\":{\"model_id\":\"\",\"profile_name\":\"\","
    "\"sampling\":{\"temperature\":0.0,\"top_p\":0.6,\"top_k\":20,\"rep_pen\":1.05},"
    "\"prompt_template_ref\":\"hymt2-official\"},"
    "\"emebala-listner\":{\"model_id\":\"user-listener-live\",\"profile_name\":\"\","
    "\"sampling\":{\"temperature\":0.1,\"top_p\":0.7,\"top_k\":15,\"rep_pen\":1.1},"
    "\"prompt_template_ref\":\"subtitle-realtime-v1\"},"
    "\"emebala-reader\":{\"model_id\":\"\",\"profile_name\":\"\","
    "\"sampling\":{\"temperature\":0.3,\"top_p\":0.9,\"top_k\":40,\"rep_pen\":1.05},"
    "\"prompt_template_ref\":\"literary-flow-v1\"}"
    "}}";

// Written into the Common dir of `root`; returns false (and fails the check)
// when the file cannot be opened for write.
bool Req056WritePolicy(const std::filesystem::path& root, const std::string& body) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(root / L"Emebala" / L"Common", ec);
    ec.clear();
    std::ofstream out(root / L"Emebala" / L"Common" / L"engine_client_policy.json",
                      std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out << body;
    return true;
}

} // namespace

void TestReq056ClientPolicyLive() {
    std::cout << "[RUN] Testing REQ-CP T2 live client-policy reader..." << std::endl;
    const int failures_before = g_failed_count;
    namespace cpol = emebalachat::enginehost::clientpolicy;

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path rootA = fs::temp_directory_path(ec) / L"emebala_req056_policy_a";
    const fs::path rootB = fs::temp_directory_path(ec) / L"emebala_req056_policy_b";
    if (ec) {
        TEST_CHECK(false, "REQ-CP T2 fixture: temp dir unavailable");
    } else {
        fs::remove_all(rootA, ec);
        ec.clear();
        fs::remove_all(rootB, ec);
        ec.clear();
        auto fail = [&](const char* what) { TEST_CHECK(false, what); };

        // (e) a never-created dir -> defaults, no crash (first-run normal state).
        {
            const fs::path rootMissing =
                fs::temp_directory_path(ec) / L"emebala_req056_policy_missing";
            const auto d = enginehost::LoadClientPolicyLive("emebala-listner",
                                                            rootMissing.wstring());
            // ClientPolicy carries no status; a defaults-path result is proven by
            // the defaults FIELD SET (bundled model, no section sampling, compiled-in
            // template). T1 keeps client_id == the CANONICAL id even on defaults
            // (CompiledInDefaults populates it), so it is not the discriminator.
            TEST_CHECK(d.client_id == "emebala-listner" && d.model_id.empty() &&
                           !d.sampling_present &&
                           d.prompt_template_ref == cpol::kDefaultPromptTemplateRef,
                       "REQ-CP T2 (e): a missing Common store -> compiled-in defaults "
                       "(canonical id, bundled model, hymt2-official)");
        }

        // (a)+(f) seed A and B with the SAME content; each root resolves its own
        // file and the cache is keyed per resolved lad dir.
        if (!Req056WritePolicy(rootA, kReq056GoldenPolicy))
            fail("REQ-CP T2 fixture: seed A write failed");
        if (!Req056WritePolicy(rootB, kReq056GoldenPolicy))
            fail("REQ-CP T2 fixture: seed B write failed");

        const auto aChat = enginehost::LoadClientPolicyLive("emebala-chat", rootA.wstring());
        TEST_CHECK(aChat.client_id == "emebala-chat" && aChat.model_id.empty() &&
                       aChat.prompt_template_ref == "hymt2-official",
                   "REQ-CP T2 (a): chat section resolves from A (canonical id, bundled "
                   "model, hymt2-official)");
        const auto bListner =
            enginehost::LoadClientPolicyLive("emebala-listner", rootB.wstring());
        TEST_CHECK(bListner.client_id == "emebala-listner" &&
                       bListner.model_id == "user-listener-live" &&
                       bListner.prompt_template_ref == "subtitle-realtime-v1",
                   "REQ-CP T2 (f): a second lad dir resolves its OWN listner section "
                   "(cache keyed per dir)");
        // A's entry still answers from its own signature.
        const auto aChatAgain = enginehost::LoadClientPolicyLive("emebala-chat", rootA.wstring());
        TEST_CHECK(aChatAgain.client_id == "emebala-chat" && aChatAgain.model_id.empty(),
                   "REQ-CP T2 (f): the original dir still resolves its own section");

        // (a) cache HIT is CONTENT-level, not resolution-level: on the SAME
        // unchanged file, rotating the client id between calls must yield each
        // id's own section. A resolution-level cache would mis-serve the second
        // call (it would return the first id's frozen resolution).
        {
            const auto listner = enginehost::LoadClientPolicyLive("emebala-listner",
                                                                  rootA.wstring());
            TEST_CHECK(listner.client_id == "emebala-listner" &&
                           listner.model_id == "user-listener-live" &&
                           listner.sampling_present && listner.sampling.temperature == 0.1f &&
                           listner.sampling.top_k == 15,
                       "REQ-CP T2 (a): unchanged file, rotated client id -> the listner "
                       "section resolves (content-level cache, sampling values live)");
            const auto reader = enginehost::LoadClientPolicyLive("emebala-reader",
                                                                 rootA.wstring());
            TEST_CHECK(reader.client_id == "emebala-reader" && reader.model_id.empty() &&
                           reader.sampling.temperature == 0.3f && reader.sampling.top_k == 40,
                       "REQ-CP T2 (a): unchanged file, third client id -> the reader "
                       "section resolves (not the cached chat/listner resolution)");
            // Unknown client on a valid file -> the canonical id is echoed but the
            // policy is the safe defaults (bundled model, no section sampling), and
            // the OTHER clients' cached sections are unaffected by the lookup.
            const auto unknown = enginehost::LoadClientPolicyLive("mystery-app", rootA.wstring());
            TEST_CHECK(unknown.client_id == "mystery-app" && unknown.model_id.empty() &&
                           !unknown.sampling_present &&
                           unknown.sampling.temperature == cpol::kDefaultTemperature,
                       "REQ-CP T2 (a): unknown client on a valid file -> compiled-in "
                       "defaults (canonical id echoed, safe policy)");
        }

        // (b) cache MISS — rewrite A's file (different content) so the mtime/size
        // signature moves; the new policy is picked up live without a respawn.
        // The rewrite also changes the size, so the signature moves even on
        // filesystems with coarse mtime granularity; the small sleep adds margin.
        {
            const char* updated =
                "{\"schema_version\":1,\"clients\":{"
                "\"emebala-chat\":{\"model_id\":\"user-chat-swapped\",\"profile_name\":\"\","
                "\"sampling\":{\"temperature\":0.4,\"top_p\":0.8,\"top_k\":30,\"rep_pen\":1.2},"
                "\"prompt_template_ref\":\"hymt2-official\"}}}";
            if (!Req056WritePolicy(rootA, updated))
                fail("REQ-CP T2 (b) fixture: updated A write failed");
            ::Sleep(120);
            const auto chat = enginehost::LoadClientPolicyLive("emebala-chat", rootA.wstring());
            TEST_CHECK(chat.client_id == "emebala-chat" &&
                           chat.model_id == "user-chat-swapped" &&
                           chat.sampling.temperature == 0.4f && chat.sampling.top_k == 30,
                       "REQ-CP T2 (b): a rewritten policy file is re-read live (cache "
                       "miss on content change)");
            // The old listner section is gone from the new doc -> the defaults field
            // set, proving the cache now holds the NEW content.
            const auto listner = enginehost::LoadClientPolicyLive("emebala-listner",
                                                                  rootA.wstring());
            TEST_CHECK(listner.client_id == "emebala-listner" && listner.model_id.empty() &&
                           !listner.sampling_present,
                       "REQ-CP T2 (b): the replaced document no longer serves the old "
                       "listner section");
        }

        // (d) torn write — truncate mid-JSON (a partial write WITHOUT the writer's
        // tmp+rename commit). The reader must be fail-closed to defaults for that
        // call and NEVER crash; a following good write restores service.
        {
            if (!Req056WritePolicy(rootA, "{\"schema_version\":1,\"clients\":{\"emebala-chat\":{"))
                fail("REQ-CP T2 (d) fixture: torn write failed");
            ::Sleep(120);
            const auto torn = enginehost::LoadClientPolicyLive("emebala-chat", rootA.wstring());
            TEST_CHECK(torn.client_id == "emebala-chat" && torn.model_id.empty() &&
                           !torn.sampling_present &&
                           torn.prompt_template_ref == cpol::kDefaultPromptTemplateRef,
                       "REQ-CP T2 (d): a torn (partial, uncommitted) write -> fail-closed "
                       "to compiled-in defaults, no crash");
            // A subsequent good write restores the section (rename atomicity is the
            // writer's job; the reader just recovers on the next valid document).
            if (!Req056WritePolicy(rootA, kReq056GoldenPolicy))
                fail("REQ-CP T2 (d) fixture: post-torn rewrite failed");
            ::Sleep(120);
            const auto healed = enginehost::LoadClientPolicyLive("emebala-listner",
                                                                 rootA.wstring());
            TEST_CHECK(healed.client_id == "emebala-listner" &&
                           healed.model_id == "user-listener-live",
                       "REQ-CP T2 (d): after a good rewrite the live policy is served "
                       "again (torn write only cost one job's defaults)");
        }

        // (c) file deleted between calls -> defaults, no crash; a later re-created
        // file re-seeds (REQ-055 attribute-failure bypass discipline).
        {
            fs::remove_all(rootA / L"Emebala", ec);
            ec.clear();
            const auto afterDelete =
                enginehost::LoadClientPolicyLive("emebala-listner", rootA.wstring());
            TEST_CHECK(afterDelete.client_id == "emebala-listner" &&
                           afterDelete.model_id.empty() && !afterDelete.sampling_present,
                       "REQ-CP T2 (c): file deleted between calls -> compiled-in defaults "
                       "(no crash)");
            if (!Req056WritePolicy(rootA, kReq056GoldenPolicy))
                fail("REQ-CP T2 (c) fixture: re-seed write failed");
            ::Sleep(120);
            const auto reseeded =
                enginehost::LoadClientPolicyLive("emebala-listner", rootA.wstring());
            TEST_CHECK(reseeded.client_id == "emebala-listner" &&
                           reseeded.model_id == "user-listener-live",
                       "REQ-CP T2 (c): a re-created file re-seeds the live policy");
        }

        fs::remove_all(rootA, ec);
        ec.clear();
        fs::remove_all(rootB, ec);
        ec.clear();
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-CP T2 live client-policy reader checks passed."
                  << std::endl;
    } else {
        std::cout << "[FAIL] REQ-CP T2 live client-policy reader: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
