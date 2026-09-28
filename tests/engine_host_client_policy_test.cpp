// REQ-CP (session 260928_0001, design §4/§5/§11 T1): ClientPolicyResolver
// unit pins — schema_version reject, unknown-client fail-closed, alias
// canonicalization, per-key tolerance, unknown-field ignore, partial sampling,
// prompt_template_ref default, merge precedence. Included directly by
// tests/run_tests.cpp AFTER the test harness macros exist (the repo staging
// contract; the resolver target lives in Emebalachat_core). All checks are
// pure-parser (no machine state).

void TestEngineHostClientPolicy() {
    std::cout << "[RUN] Testing engine-host client-policy resolver (REQ-CP T1)..." << std::endl;
    namespace cp = emebalachat::enginehost::clientpolicy;
    using cp::ResolveStatus;

    // The design §5 golden document: three seeded sections with distinct
    // sampling so each client's resolution is individually observable.
    const char* kGolden =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"clients\": {\n"
        "    \"emebala-chat\": {\n"
        "      \"model_id\": \"\",\n"
        "      \"profile_name\": \"\",\n"
        "      \"sampling\": {\"temperature\": 0.0, \"top_p\": 0.6, \"top_k\": 20,\n"
        "                     \"rep_pen\": 1.05},\n"
        "      \"prompt_template_ref\": \"hymt2-official\"\n"
        "    },\n"
        "    \"emebala-listner\": {\n"
        "      \"model_id\": \"\",\n"
        "      \"sampling\": {\"temperature\": 0.0, \"top_p\": 0.6, \"top_k\": 20,\n"
        "                     \"rep_pen\": 1.05},\n"
        "      \"prompt_template_ref\": \"subtitle-realtime-v1\"\n"
        "    },\n"
        "    \"emebala-reader\": {\n"
        "      \"model_id\": \"\",\n"
        "      \"sampling\": {\"temperature\": 0.3, \"top_p\": 0.9, \"top_k\": 40,\n"
        "                     \"rep_pen\": 1.05},\n"
        "      \"prompt_template_ref\": \"literary-flow-v1\"\n"
        "    }\n"
        "  }\n"
        "}";

    // ---- 1. Golden document: each client's own section applies ----
    {
        const auto chat = cp::ResolvePolicy("emebala-chat", kGolden);
        TEST_CHECK(chat.status == ResolveStatus::Ok, "policy golden: chat Ok");
        TEST_CHECK(chat.policy.client_id == "emebala-chat", "policy golden: chat canonical id");
        TEST_CHECK(chat.policy.model_id.empty(), "policy golden: chat empty model_id -> pinned");
        TEST_CHECK(chat.policy.sampling_present, "policy golden: chat sampling_present");
        TEST_CHECK(chat.policy.sampling.temperature == 0.0f, "policy golden: chat temperature");
        TEST_CHECK(chat.policy.sampling.top_p == 0.6f, "policy golden: chat top_p");
        TEST_CHECK(chat.policy.sampling.top_k == 20, "policy golden: chat top_k");
        TEST_CHECK(chat.policy.sampling.rep_pen == 1.05f, "policy golden: chat rep_pen");
        TEST_CHECK(chat.policy.prompt_template_ref == "hymt2-official",
                   "policy golden: chat template ref");

        const auto listner = cp::ResolvePolicy("emebala-listner", kGolden);
        TEST_CHECK(listner.status == ResolveStatus::Ok, "policy golden: listner Ok");
        TEST_CHECK(listner.policy.prompt_template_ref == "subtitle-realtime-v1",
                   "policy golden: listner subtitle template");

        const auto reader = cp::ResolvePolicy("emebala-reader", kGolden);
        TEST_CHECK(reader.status == ResolveStatus::Ok, "policy golden: reader Ok");
        TEST_CHECK(reader.policy.sampling.temperature == 0.3f, "policy golden: reader temperature");
        TEST_CHECK(reader.policy.sampling.top_p == 0.9f, "policy golden: reader top_p");
        TEST_CHECK(reader.policy.sampling.top_k == 40, "policy golden: reader top_k");
        TEST_CHECK(reader.policy.prompt_template_ref == "literary-flow-v1",
                   "policy golden: reader literary template");
    }

    // ---- 2. schema_version missing / != 1 -> whole file ignored, defaults ----
    {
        const auto r1 = cp::ResolvePolicy("emebala-chat", "{\"clients\":{}}");
        TEST_CHECK(r1.status == ResolveStatus::Defaults, "policy no schema_version -> Defaults");
        TEST_CHECK(r1.policy.client_id == "emebala-chat", "policy reject: canonical id kept");
        TEST_CHECK(r1.policy.model_id.empty(), "policy reject: model_id default");
        TEST_CHECK(!r1.policy.sampling_present, "policy reject: sampling_present false");
        TEST_CHECK(r1.policy.sampling.temperature == cp::kDefaultTemperature,
                   "policy reject: temperature default");
        TEST_CHECK(r1.policy.sampling.top_p == cp::kDefaultTopP, "policy reject: top_p default");
        TEST_CHECK(r1.policy.sampling.top_k == cp::kDefaultTopK, "policy reject: top_k default");
        TEST_CHECK(r1.policy.sampling.rep_pen == cp::kDefaultRepPen,
                   "policy reject: rep_pen default");
        TEST_CHECK(r1.policy.prompt_template_ref == cp::kDefaultPromptTemplateRef,
                   "policy reject: template default");

        // != 1 rejected even when the rest of the document is perfect.
        const std::string v2 = std::string("{\"schema_version\":2,\"clients\":{"
                                           "\"emebala-chat\":{\"model_id\":\"x\"}}}");
        const auto r2 = cp::ResolvePolicy("emebala-chat", v2);
        TEST_CHECK(r2.status == ResolveStatus::Defaults, "policy schema_version=2 -> Defaults");
        TEST_CHECK(r2.policy.model_id.empty(), "policy schema_version=2: model_id NOT applied");

        // A quoted "1" is mistyped -> reject (typed-JSON-int discipline).
        const auto r3 = cp::ResolvePolicy("emebala-chat",
                                          "{\"schema_version\":\"1\",\"clients\":{}}");
        TEST_CHECK(r3.status == ResolveStatus::Defaults, "policy quoted schema_version -> Defaults");
    }

    // ---- 3. Unknown client id -> compiled-in defaults (item-D fail-closed) ----
    {
        const auto r = cp::ResolvePolicy("mystery-client", kGolden);
        TEST_CHECK(r.status == ResolveStatus::UnknownClient,
                   "policy unknown client -> UnknownClient");
        TEST_CHECK(r.policy.client_id == "mystery-client",
                   "policy unknown client: id echoed canonical");
        TEST_CHECK(r.policy.model_id.empty(), "policy unknown client: pinned default model");
        TEST_CHECK(!r.policy.sampling_present, "policy unknown client: no sampling");
        TEST_CHECK(r.policy.prompt_template_ref == "hymt2-official",
                   "policy unknown client: default template");

        // The item-D regression shape verbatim: the Listener's real hello id
        // has NO section in a chat-only seeded file -> "" (pinned), the exact
        // behavior RelayPinForClient produced before the resolver existed.
        const char* chat_only =
            "{\"schema_version\":1,\"clients\":{\"emebala-chat\":{"
            "\"model_id\":\"user-milm-4b\",\"sampling\":{\"temperature\":0.2}}}}";
        const auto lr = cp::ResolvePolicy("emebala-listner", chat_only);
        TEST_CHECK(lr.status == ResolveStatus::UnknownClient,
                   "policy chat-only file: listner UnknownClient");
        TEST_CHECK(lr.policy.model_id.empty(), "policy chat-only file: listner stays pinned");
    }

    // ---- 4. Alias canonicalization (design §4 table) ----
    {
        // The corrected spelling maps onto the canonical typo-id the Listener
        // actually sends in its hello.
        const auto a = cp::ResolvePolicy("emebala-listener", kGolden);
        TEST_CHECK(a.status == ResolveStatus::Ok, "policy alias: emebala-listener -> listner Ok");
        TEST_CHECK(a.policy.client_id == "emebala-listner",
                   "policy alias: canonical id is the typo-id");
        TEST_CHECK(a.policy.prompt_template_ref == "subtitle-realtime-v1",
                   "policy alias: listner section applied through the alias");

        // Case- and whitespace-insensitive canonicalization.
        const auto b = cp::ResolvePolicy("  Emebala-Listener ", kGolden);
        TEST_CHECK(b.status == ResolveStatus::Ok, "policy alias: trim+case fold -> Ok");
        TEST_CHECK(b.policy.client_id == "emebala-listner",
                   "policy alias: trimmed/canonicalized id");

        // The typo-id itself is already canonical.
        const auto c = cp::ResolvePolicy("emebala-listner", kGolden);
        TEST_CHECK(c.status == ResolveStatus::Ok, "policy alias: typo-id direct -> Ok");

        // Direct canonicalization pins.
        TEST_CHECK(cp::CanonicalizeClientId("emebala-listener") == "emebala-listner",
                   "canonicalize: corrected -> typo-id");
        TEST_CHECK(cp::CanonicalizeClientId("EMEBALA-CHAT") == "emebala-chat",
                   "canonicalize: case fold");
        TEST_CHECK(cp::CanonicalizeClientId("  emebala-reader\t") == "emebala-reader",
                   "canonicalize: trim");
        TEST_CHECK(cp::CanonicalizeClientId("").empty(), "canonicalize: empty -> empty");
        TEST_CHECK(cp::CanonicalizeClientId("   ").empty(), "canonicalize: whitespace -> empty");

        // Known-set membership (the compiled-in three, alias-resolved).
        TEST_CHECK(cp::IsKnownClientId("emebala-chat"), "known: chat");
        TEST_CHECK(cp::IsKnownClientId("emebala-listner"), "known: listner (typo-id)");
        TEST_CHECK(cp::IsKnownClientId("emebala-listener"), "known: listner (corrected alias)");
        TEST_CHECK(cp::IsKnownClientId("emebala-reader"), "known: reader");
        TEST_CHECK(!cp::IsKnownClientId("mystery-client"), "known: mystery -> false");
        TEST_CHECK(!cp::IsKnownClientId(""), "known: empty -> false");
    }

    // ---- 5. Per-key tolerance: one wrong-typed key defaults, rest applies ----
    {
        // top_k as a STRING is the design's own example: that key defaults,
        // the sibling keys in the SAME sampling object still apply.
        const char* doc =
            "{\"schema_version\":1,\"clients\":{\"emebala-listner\":{"
            "\"model_id\":\"user-milm-4b\","
            "\"sampling\":{\"temperature\":0.4,\"top_p\":0.8,\"top_k\":\"20\",\"rep_pen\":1.1}}}}";
        const auto r = cp::ResolvePolicy("emebala-listner", doc);
        TEST_CHECK(r.status == ResolveStatus::Ok, "policy per-key: section Ok");
        TEST_CHECK(r.policy.model_id == "user-milm-4b", "policy per-key: model_id applied");
        TEST_CHECK(r.policy.sampling_present, "policy per-key: sampling_present (siblings usable)");
        TEST_CHECK(r.policy.sampling.temperature == 0.4f, "policy per-key: temperature applied");
        TEST_CHECK(r.policy.sampling.top_p == 0.8f, "policy per-key: top_p applied");
        TEST_CHECK(r.policy.sampling.top_k == cp::kDefaultTopK,
                   "policy per-key: wrong-typed top_k -> shipped default 20");
        TEST_CHECK(r.policy.sampling.rep_pen == 1.1f, "policy per-key: rep_pen applied");

        // A wrong-typed scalar field: model_id as a NUMBER is that key's
        // default only — the section's sampling still applies.
        const char* doc2 =
            "{\"schema_version\":1,\"clients\":{\"emebala-listner\":{"
            "\"model_id\":42,\"sampling\":{\"temperature\":0.7}}}}";
        const auto r2 = cp::ResolvePolicy("emebala-listner", doc2);
        TEST_CHECK(r2.status == ResolveStatus::Ok, "policy per-key scalar: section Ok");
        TEST_CHECK(r2.policy.model_id.empty(), "policy per-key scalar: model_id default");
        TEST_CHECK(r2.policy.sampling.temperature == 0.7f,
                   "policy per-key scalar: sampling still applies");
    }

    // ---- 6. Unknown fields ignored (document level, section level, sampling) ----
    {
        const char* doc =
            "{\"schema_version\":1,\"future_top\":\"x\",\"clients\":{"
            "\"emebala-listner\":{\"model_id\":\"m1\",\"comment\":\"user note\","
            "\"unknown_nested\":{\"a\":[1,2]},"
            "\"sampling\":{\"temperature\":0.9,\"not_a_key\":[true],\"top_p\":0.7},"
            "\"prompt_template_ref\":\"subtitle-realtime-v1\",\"another\":null}}}";
        const auto r = cp::ResolvePolicy("emebala-listner", doc);
        TEST_CHECK(r.status == ResolveStatus::Ok, "policy unknown fields: Ok");
        TEST_CHECK(r.policy.model_id == "m1", "policy unknown fields: model_id applied");
        TEST_CHECK(r.policy.sampling.temperature == 0.9f,
                   "policy unknown fields: known sampling key applied");
        TEST_CHECK(r.policy.sampling.top_p == 0.7f,
                   "policy unknown fields: known sampling key after unknown applied");
        TEST_CHECK(r.policy.prompt_template_ref == "subtitle-realtime-v1",
                   "policy unknown fields: template ref applied");
    }

    // ---- 7. Empty / blank / garbage / BOM input -> defaults ----
    {
        const auto r0 = cp::ResolvePolicy("emebala-chat", "");
        TEST_CHECK(r0.status == ResolveStatus::Defaults, "policy empty text -> Defaults");
        const auto r1 = cp::ResolvePolicy("emebala-chat", "   \r\n\t  ");
        TEST_CHECK(r1.status == ResolveStatus::Defaults, "policy blank text -> Defaults");
        const auto r2 = cp::ResolvePolicy("emebala-chat", "not json at all");
        TEST_CHECK(r2.status == ResolveStatus::Defaults, "policy garbage -> Defaults");
        const auto r3 = cp::ResolvePolicy("emebala-chat", "[1,2,3]");
        TEST_CHECK(r3.status == ResolveStatus::Defaults, "policy array root -> Defaults");
        // A leading BOM is tolerated (the §5 seeded file is BOM-free, but a
        // BOM-emitting editor must not downgrade the whole policy).
        const std::string bom = std::string("\xEF\xBB\xBF") + kGolden;
        const auto r4 = cp::ResolvePolicy("emebala-listner", bom);
        TEST_CHECK(r4.status == ResolveStatus::Ok, "policy BOM-prefixed golden -> Ok");
        TEST_CHECK(r4.policy.prompt_template_ref == "subtitle-realtime-v1",
                   "policy BOM-prefixed: section applied");
    }

    // ---- 8. Partial sampling section: present keys win, absent = shipped ----
    {
        const char* doc =
            "{\"schema_version\":1,\"clients\":{\"emebala-listner\":{"
            "\"sampling\":{\"temperature\":0.5,\"rep_pen\":1.2}}}}";
        const auto r = cp::ResolvePolicy("emebala-listner", doc);
        TEST_CHECK(r.status == ResolveStatus::Ok, "policy partial sampling: Ok");
        TEST_CHECK(r.policy.sampling_present, "policy partial sampling: present");
        TEST_CHECK(r.policy.sampling.temperature == 0.5f, "policy partial: temperature wins");
        TEST_CHECK(r.policy.sampling.rep_pen == 1.2f, "policy partial: rep_pen wins");
        TEST_CHECK(r.policy.sampling.top_p == cp::kDefaultTopP,
                   "policy partial: absent top_p = shipped 0.6");
        TEST_CHECK(r.policy.sampling.top_k == cp::kDefaultTopK,
                   "policy partial: absent top_k = shipped 20");

        // A sampling object with NO usable key at all is not a section: the
        // default sampling_present stays false.
        const char* doc2 =
            "{\"schema_version\":1,\"clients\":{\"emebala-listner\":{"
            "\"sampling\":{\"top_k\":\"20\",\"temperature\":\"hot\"}}}}";
        const auto r2 = cp::ResolvePolicy("emebala-listner", doc2);
        TEST_CHECK(r2.status == ResolveStatus::Ok, "policy all-wrong sampling: section Ok");
        TEST_CHECK(!r2.policy.sampling_present,
                   "policy all-wrong sampling: sampling_present stays false");
        TEST_CHECK(r2.policy.sampling.temperature == cp::kDefaultTemperature,
                   "policy all-wrong sampling: temperature default");
        TEST_CHECK(r2.policy.sampling.top_k == cp::kDefaultTopK,
                   "policy all-wrong sampling: top_k default");
    }

    // ---- 9. prompt_template_ref absent -> "hymt2-official" ----
    {
        const char* doc =
            "{\"schema_version\":1,\"clients\":{\"emebala-listner\":{"
            "\"sampling\":{\"temperature\":0.1}}}}";
        const auto r = cp::ResolvePolicy("emebala-listner", doc);
        TEST_CHECK(r.status == ResolveStatus::Ok, "policy no template: Ok");
        TEST_CHECK(r.policy.prompt_template_ref == "hymt2-official",
                   "policy no template: compiled-in default");
        // A bogus ref passes through verbatim (R-D: the WORKER maps unknown
        // refs to hymt2-official — the resolver keeps forward-compat raw).
        const char* doc2 =
            "{\"schema_version\":1,\"clients\":{\"emebala-listner\":{"
            "\"prompt_template_ref\":\"bogus-v9\"}}}";
        const auto r2 = cp::ResolvePolicy("emebala-listner", doc2);
        TEST_CHECK(r2.policy.prompt_template_ref == "bogus-v9",
                   "policy unknown template ref: verbatim passthrough (worker falls back)");
    }

    // ---- 10. Merge precedence: request > client profile > compiled-in defaults ----
    {
        const auto resolved = cp::ResolvePolicy("emebala-listner", kGolden);

        // No overrides: the resolved policy passes through unchanged.
        const auto m0 = cp::MergeForRequest(resolved.policy, "", nullptr, false, "");
        TEST_CHECK(m0.model_id == resolved.policy.model_id, "merge identity: model_id");
        TEST_CHECK(m0.sampling_present == resolved.policy.sampling_present,
                   "merge identity: sampling_present");
        TEST_CHECK(m0.sampling.temperature == resolved.policy.sampling.temperature,
                   "merge identity: sampling");
        TEST_CHECK(m0.prompt_template_ref == resolved.policy.prompt_template_ref,
                   "merge identity: template");

        // A v2 session model_id override wins over the profile.
        const auto m1 = cp::MergeForRequest(resolved.policy, "session-model-x",
                                            nullptr, false, "");
        TEST_CHECK(m1.model_id == "session-model-x", "merge: request model_id wins");
        TEST_CHECK(m1.sampling.temperature == resolved.policy.sampling.temperature,
                   "merge: request model override leaves sampling alone");

        // A job-level sampling override wins over the profile sampling.
        cp::SamplingPolicy js; // an in-memory sampling override
        js.temperature = 0.77f; js.top_p = 0.33f; js.top_k = 7; js.rep_pen = 1.7f;
        const auto m2 = cp::MergeForRequest(resolved.policy, "", &js, true, "");
        TEST_CHECK(m2.sampling_present, "merge: job sampling override present");
        TEST_CHECK(m2.sampling.temperature == 0.77f && m2.sampling.top_k == 7,
                   "merge: job sampling values win");
        TEST_CHECK(m2.model_id == resolved.policy.model_id,
                   "merge: sampling override leaves model alone");

        // A template override wins; an absent override keeps the profile ref.
        const auto m3 = cp::MergeForRequest(resolved.policy, "", nullptr, false,
                                            "custom-template");
        TEST_CHECK(m3.prompt_template_ref == "custom-template", "merge: request template wins");
    }

    // ---- 11. CompiledInDefaults is the exact reject-path shape ----
    {
        const auto d = cp::CompiledInDefaults("emebala-listner");
        TEST_CHECK(d.client_id == "emebala-listner", "defaults: canonical id");
        TEST_CHECK(d.model_id.empty() && d.profile_name.empty(), "defaults: empty id slots");
        TEST_CHECK(!d.sampling_present, "defaults: sampling_present false");
        TEST_CHECK(d.sampling.temperature == 0.0f && d.sampling.top_p == 0.6f &&
                       d.sampling.top_k == 20 && d.sampling.rep_pen == 1.05f,
                   "defaults: shipped sampling 0.0/0.6/20/1.05");
        TEST_CHECK(d.prompt_template_ref == "hymt2-official", "defaults: hymt2-official");
    }
}
