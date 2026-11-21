// ===========================================================================
// 261122_0001 B-U1 (OpenAI 설정 창 프로필 재설계, builder draw 5; design-pick
// items 1/2/3/7): the OpenAI profile LIBRARY — config schema, parse/serialize/
// caps/migration, and the pure name-derivation helpers.
//
// Covered, per the U1 unit contract:
//   (1) round trip: parse -> emit -> re-parse equality (values AND bytes)
//   (2) empty library => the "openai_profiles" key is NOT emitted
//   (3) entry-count cap (the 33rd entry is dropped)
//   (4) name byte cap at a UTF-8 code-point boundary
//   (5) malformed entry skipped; non-array value => empty library
//   (6) migration synthesis (name via the helpers, openai block byte-identical,
//       NO synthesis while the key is present, deterministic)
//   (7) downgrade posture: the "openai" active block parse/emission is
//       untouched by the library (the pre-U1 contract pins)
//   (8) OpenAiHostAbbrev table (second-to-last label / IP / relative / cap)
//   (9) MakeOpenAiProfileName template substitution + empty-side rules
//  (10) MakeUniqueOpenAiProfileName " (2)" chain, cap-respecting
//  (11) PlanOpenAiProfileUpsert identity (base_url case-insensitive, model
//       case-sensitive, no-match => append index)
//  (12) negative key-material assertion: no emitted field named "api_key"
//       (only the dpapi/sha256 pair exists)
//
// Zero network, zero UI: pure config-layer checks only.
// ===========================================================================
namespace openai_profile_tests {

bool ProfilesEqual(const OpenAiProfile& a, const OpenAiProfile& b) {
    return a.name == b.name && a.cfg.base_url == b.cfg.base_url &&
           a.cfg.model == b.cfg.model && a.cfg.api_key_dpapi == b.cfg.api_key_dpapi &&
           a.cfg.api_key_sha256 == b.cfg.api_key_sha256 &&
           a.cfg.http_consent_given == b.cfg.http_consent_given &&
           a.cfg.reasoning_effort == b.cfg.reasoning_effort;
}

bool ProfileVectorsEqual(const std::vector<OpenAiProfile>& a,
                         const std::vector<OpenAiProfile>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!ProfilesEqual(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

OpenAiProfile MakeProfile(std::string name,
                          std::string base_url,
                          std::string model,
                          std::string dpapi = "QUJDREVGRw==",
                          std::string sha256 = "0123456789abcdef0123456789abcdef"
                                               "0123456789abcdef0123456789abcdef",
                          bool consent = false,
                          std::string reasoning = "") {
    OpenAiProfile profile;
    profile.name = std::move(name);
    profile.cfg.base_url = std::move(base_url);
    profile.cfg.model = std::move(model);
    profile.cfg.api_key_dpapi = std::move(dpapi);
    profile.cfg.api_key_sha256 = std::move(sha256);
    profile.cfg.http_consent_given = consent;
    profile.cfg.reasoning_effort = std::move(reasoning);
    return profile;
}

void TestOpenAiProfileConfigSchema() {
    const int failures_before = g_failed_count;

    // ---- (8) OpenAiHostAbbrev: the design-pick table ----------------------
    struct {
        const char* input;
        const char* expected;
    } kHostAbbrev[] = {
        {"https://api.openai.com/v1", "openai"},
        {"https://api.openai.com", "openai"},
        {"https://API.OpenAI.COM", "openai"},          // ASCII case fold
        {"api.openai.com", "openai"},                  // scheme-less host
        {"openai.com", "openai"},                      // two labels: 2nd-to-last
        {"https://user:pass@api.openai.com", "openai"},// userinfo stripped
        {"localhost:8000", "localhost"},               // scheme-less host:port
        {"http://localhost:8000/v1", "localhost"},
        {"192.168.0.10", "192.168.0.10"},              // IPv4: whole host
        {"http://192.168.0.10:8080/v1", "192.168.0.10"},
        {"http://[::1]:5000", "[::1]"},                // [v6] keeps its brackets
        {"http://[2001:db8::1]/v1", "[2001:db8::1]"},
        {"https://single", "single"},                  // single label: whole
        {"", ""},                                      // empty: no host
        {"/v1/chat/completions", ""},                  // relative: no host
        {"?api.openai.com", ""},                       // query-only: no host
    };
    for (const auto& row : kHostAbbrev) {
        TEST_CHECK(OpenAiHostAbbrev(row.input) == row.expected,
                   std::string("OpenAiHostAbbrev: '") + row.input + "' -> '" +
                       row.expected + "'");
    }
    // Long second-to-last label: 26 bytes -> 21 bytes + "…" = 24 total, the
    // ellipsis counting toward the cap.
    const std::string long_abbrev =
        OpenAiHostAbbrev("https://api.my-very-long-endpoint-name.com");
    TEST_CHECK(long_abbrev == "my-very-long-endpoint\xE2\x80\xA6" &&
                   long_abbrev.size() == 24,
               "OpenAiHostAbbrev: 24-byte cap with a code-point-safe … suffix");
    // A multi-byte SECOND-TO-LAST label is cut on a code-point boundary: the
    // 가 run sits one label above .com, 10 x "가" = 30 bytes -> 7 x "가"
    // (21 bytes) + "…".
    TEST_CHECK(OpenAiHostAbbrev("https://api.가가가가가가가가가가가.com/v1") ==
                   std::string("가가가가가가가\xE2\x80\xA6"),
               "OpenAiHostAbbrev: UTF-8 boundary kept under the cap");

    // ---- (9) MakeOpenAiProfileName: template + empty-side rules ----------
    TEST_CHECK(MakeOpenAiProfileName("openai", "gpt-4o-mini", "{h} / {m}") ==
                   "openai / gpt-4o-mini",
               "profile name: {h} / {m} substitution");
    TEST_CHECK(MakeOpenAiProfileName("openai", "gpt-4o-mini", "{h}의 {m} 모델") ==
                   "openai의 gpt-4o-mini 모델",
               "profile name: the ko template substitutes the same tokens");
    TEST_CHECK(MakeOpenAiProfileName("", "gpt-4o-mini", "{h}의 {m} 모델") == "gpt-4o-mini",
               "profile name: empty host -> model alone (no connector fragment)");
    TEST_CHECK(MakeOpenAiProfileName("openai", "", "{h}의 {m} 모델") == "openai",
               "profile name: empty model -> host alone");
    TEST_CHECK(MakeOpenAiProfileName("", "", "{h} / {m}").empty(),
               "profile name: both empty -> empty (the caller handles it)");
    TEST_CHECK(MakeOpenAiProfileName("h", "m", "model {m} on {h}!") == "model m on h!",
               "profile name: token order is template-driven");
    {
        // 96-byte cap on the RESULT: 90 h's + " / " + "model" = 98 -> 96.
        const std::string capped =
            MakeOpenAiProfileName(std::string(90, 'h'), "model", "{h} / {m}");
        TEST_CHECK(capped.size() == 96 && capped.compare(0, 93, std::string(90, 'h') + " / ") == 0,
                   "profile name: 96-byte code-point cap on the result");
    }

    // ---- (10) MakeUniqueOpenAiProfileName: the " (2)" chain ---------------
    {
        std::vector<OpenAiProfile> existing = {MakeProfile("Alpha", "https://a.example.com", "m")};
        TEST_CHECK(MakeUniqueOpenAiProfileName(existing, "Beta") == "Beta",
                   "unique name: a free base returns unchanged");
        TEST_CHECK(MakeUniqueOpenAiProfileName(existing, "Alpha") == "Alpha (2)",
                   "unique name: first collision gets ' (2)'");
        existing.push_back(MakeProfile("Alpha (2)", "https://b.example.com", "m"));
        TEST_CHECK(MakeUniqueOpenAiProfileName(existing, "Alpha") == "Alpha (3)",
                   "unique name: the chain continues with ' (3)'");
        // Cap-respecting: base 96 x's already taken -> 92 x's + " (2)" = 96.
        std::vector<OpenAiProfile> taken = {MakeProfile(std::string(96, 'x'), "https://c.example.com", "m")};
        const std::string deduped = MakeUniqueOpenAiProfileName(taken, std::string(96, 'x'));
        TEST_CHECK(deduped.size() == 96 && deduped.compare(0, 92, std::string(92, 'x')) == 0 &&
                       deduped.compare(92, 4, " (2)") == 0,
                   "unique name: the suffix stays whole inside the 96-byte cap");
    }

    // ---- (11) PlanOpenAiProfileUpsert: identity ---------------------------
    {
        std::vector<OpenAiProfile> existing = {
            MakeProfile("one", "https://api.openai.com", "gpt-4o-mini"),
            MakeProfile("two", "http://LOCALHOST:8000", "Custom"),
        };
        OpenAiConfig same_identity;
        same_identity.base_url = "HTTPS://API.OPENAI.COM"; // scheme+host case differs
        same_identity.model = "gpt-4o-mini";
        TEST_CHECK(PlanOpenAiProfileUpsert(existing, same_identity) == 0,
                   "upsert plan: base_url matches ASCII case-insensitively");
        OpenAiConfig other_port;
        other_port.base_url = "http://localhost:8000";
        other_port.model = "Custom";
        TEST_CHECK(PlanOpenAiProfileUpsert(existing, other_port) == 1,
                   "upsert plan: a second identity resolves to its own index");
        OpenAiConfig model_case;
        model_case.base_url = "https://api.openai.com";
        model_case.model = "GPT-4O-MINI"; // model ids are case-SENSITIVE
        TEST_CHECK(PlanOpenAiProfileUpsert(existing, model_case) == existing.size(),
                   "upsert plan: model case difference means a NEW profile");
        OpenAiConfig no_match;
        no_match.base_url = "https://elsewhere.example.com";
        no_match.model = "other";
        TEST_CHECK(PlanOpenAiProfileUpsert(existing, no_match) == existing.size(),
                   "upsert plan: no match -> append index (existing.size())");
    }

    // ---- (1) round trip: parse -> emit -> re-parse -------------------------
    {
        AppConfig cfg;
        TEST_CHECK(cfg.FromJsonString(
                       "{\"ui_language\": \"ko\", \"openai\": {\"base_url\": "
                       "\"https://api.openai.com\", \"model\": \"gpt-4o-mini\"}, "
                       "\"openai_profiles\": ["
                       "{\"name\": \"openai / gpt-4o-mini\", \"base_url\": "
                       "\"https://api.openai.com\", \"model\": \"gpt-4o-mini\", "
                       "\"reasoning_effort\": \"low\", \"api_key_dpapi\": \"QUJD\", "
                       "\"api_key_sha256\": \"0123456789abcdef0123456789abcdef0123"
                       "456789abcdef0123456789abcdef\", \"http_consent_given\": true}, "
                       "{\"name\": \"클로\\r\\n\"}]}") ,
                   "round trip: the library parses");
        TEST_CHECK(cfg.openai_profiles.size() == 2,
                   "round trip: both entries load");
        const std::string emitted = cfg.ToJsonString();
        TEST_CHECK(emitted.find("\"openai_profiles\": [") != std::string::npos,
                   "round trip: the key is emitted for a non-empty library");
        AppConfig reloaded;
        TEST_CHECK(reloaded.FromJsonString(emitted) &&
                       ProfileVectorsEqual(reloaded.openai_profiles, cfg.openai_profiles),
                   "round trip: re-parse reproduces the library verbatim");
        TEST_CHECK(reloaded.ToJsonString() == emitted,
                   "round trip: the second save is byte-identical (stable emission)");
        // The escaped control characters of entry 2 survive the round trip.
        TEST_CHECK(reloaded.openai_profiles[1].name == "클로\r\n",
                   "round trip: a stored name keeps its escaped bytes");
    }

    // ---- (2) empty library => key NOT emitted -----------------------------
    {
        AppConfig fresh;
        TEST_CHECK(fresh.ToJsonString().find("openai_profiles") == std::string::npos,
                   "empty library: a default config emits no key");
        AppConfig emptied;
        TEST_CHECK(emptied.FromJsonString("{\"openai_profiles\": [], \"ui_language\": \"ko\"}") &&
                       emptied.openai_profiles.empty(),
                   "empty library: a present empty array reads as empty");
        TEST_CHECK(emptied.ToJsonString().find("openai_profiles") == std::string::npos,
                   "empty library: re-emission still omits the key");
    }

    // ---- (3) entry-count cap: the 33rd entry is dropped -------------------
    {
        std::string json = "{\"openai_profiles\": [";
        for (int i = 0; i < 33; ++i) {
            if (i != 0) {
                json += ", ";
            }
            json += "{\"name\": \"p" + std::to_string(i) + "\"}";
        }
        json += "], \"ui_language\": \"ko\"}";
        AppConfig capped;
        TEST_CHECK(capped.FromJsonString(json) && capped.openai_profiles.size() == 32 &&
                       capped.openai_profiles.front().name == "p0" &&
                       capped.openai_profiles.back().name == "p31",
                   "count cap: exactly kMaxOpenAiProfiles entries survive, in order");
    }

    // ---- (4) name cap at a UTF-8 code-point boundary ----------------------
    {
        const std::string long_name = std::string(95, 'x') + "한"; // 95 + 3 = 98 bytes
        AppConfig named;
        TEST_CHECK(named.FromJsonString("{\"openai_profiles\": [{\"name\": \"" + long_name +
                                        "\"}], \"ui_language\": \"ko\"}"),
                   "name cap: over-long name loads");
        TEST_CHECK(named.openai_profiles.size() == 1 &&
                       named.openai_profiles[0].name == std::string(95, 'x'),
                   "name cap: truncated at the 96-byte code-point boundary "
                   "(the 3-byte code point is not split)");
    }

    // ---- (5) malformed entry skipped / non-array => empty ------------------
    {
        AppConfig skipped;
        TEST_CHECK(skipped.FromJsonString(
                       "{\"openai_profiles\": [{\"name\": \"a\"}, garbage, 42, "
                       "{\"name\": \"b\"}], \"ui_language\": \"ko\"}") &&
                       skipped.openai_profiles.size() == 2 &&
                       skipped.openai_profiles[0].name == "a" &&
                       skipped.openai_profiles[1].name == "b",
                   "malformed: non-object elements are skipped, valid entries survive");
        for (const char* bad : {"\"oops\"", "{\"name\": \"x\"}", "42"}) {
            AppConfig non_array;
            const std::string doc = std::string("{\"openai_profiles\": ") + bad + "}";
            TEST_CHECK(non_array.FromJsonString(doc) && non_array.openai_profiles.empty(),
                       std::string("malformed: non-array value -> empty library: ") + bad);
        }
        AppConfig unterminated;
        TEST_CHECK(!unterminated.FromJsonString(
                       "{\"openai_profiles\": [{\"name\": \"a\""),
                   "malformed: an unterminated array fails the load closed (the "
                   "top-level capture rejects it, defaults retained)");
    }

    // ---- (6) migration synthesis ------------------------------------------
    {
        const std::string legacy =
            "{\"ui_language\": \"ko\", \"openai\": {\"base_url\": "
            "\"https://api.openai.com\", \"model\": \"gpt-4o-mini\", "
            "\"reasoning_effort\": \"low\", \"api_key_dpapi\": \"QUJD\", "
            "\"api_key_sha256\": \"0123456789abcdef0123456789abcdef0123456789abcdef"
            "0123456789abcdef\", \"http_consent_given\": true}}";
        AppConfig migrated;
        TEST_CHECK(migrated.FromJsonString(legacy) && migrated.openai_profiles.size() == 1,
                   "migration: absent key + non-empty openai block -> one profile");
        TEST_CHECK(migrated.openai_profiles[0].name == "openai / gpt-4o-mini",
                   "migration: the name derives via the helpers (host abbrev + template)");
        TEST_CHECK(migrated.openai_profiles[0].cfg.base_url == "https://api.openai.com" &&
                       migrated.openai_profiles[0].cfg.model == "gpt-4o-mini" &&
                       migrated.openai_profiles[0].cfg.api_key_dpapi == "QUJD" &&
                       migrated.openai_profiles[0].cfg.reasoning_effort == "low" &&
                       migrated.openai_profiles[0].cfg.http_consent_given,
                   "migration: the synthesized cfg copies the openai block");
        TEST_CHECK(migrated.openai.base_url == "https://api.openai.com" &&
                       migrated.openai.model == "gpt-4o-mini" &&
                       migrated.openai.api_key_dpapi == "QUJD" &&
                       migrated.openai.api_key_sha256.size() == 64 &&
                       migrated.openai.reasoning_effort == "low" &&
                       migrated.openai.http_consent_given,
                   "migration: the openai block itself stays byte-identical");
        // The next save persists the synthesized library (the migration lands).
        TEST_CHECK(migrated.ToJsonString().find("\"openai_profiles\": [") != std::string::npos,
                   "migration: the next save emits the synthesized library");
        // Deterministic: the same input file yields the same library.
        AppConfig migrated_again;
        TEST_CHECK(migrated_again.FromJsonString(legacy) &&
                       ProfileVectorsEqual(migrated_again.openai_profiles,
                                           migrated.openai_profiles),
                   "migration: synthesis is deterministic for the same input");
        // A localhost endpoint derives the host-only abbrev.
        AppConfig local;
        TEST_CHECK(local.FromJsonString(
                       "{\"openai\": {\"base_url\": \"http://localhost:8000\", \"model\": "
                       "\"llama-3\"}}") &&
                       local.openai_profiles.size() == 1 &&
                       local.openai_profiles[0].name == "localhost / llama-3",
                   "migration: localhost endpoint -> 'localhost / llama-3'");
        // Model-only block (no usable host): the name is the model alone.
        AppConfig model_only;
        TEST_CHECK(model_only.FromJsonString("{\"openai\": {\"model\": \"gpt-4o-mini\"}}") &&
                       model_only.openai_profiles.size() == 1 &&
                       model_only.openai_profiles[0].name == "gpt-4o-mini",
                   "migration: model without a host -> model-alone name");
        // Consent alone is NOT "configured": no synthesis.
        AppConfig consent_only;
        TEST_CHECK(consent_only.FromJsonString(
                       "{\"openai\": {\"http_consent_given\": true}}") &&
                       consent_only.openai_profiles.empty(),
                   "migration: consent alone does not synthesize a profile");
        // NO synthesis while the key is present (even empty, even with a full
        // openai block beside it).
        AppConfig key_present_empty;
        TEST_CHECK(key_present_empty.FromJsonString(legacy.substr(0, legacy.size() - 1) +
                                                    ", \"openai_profiles\": []}") &&
                       key_present_empty.openai_profiles.empty(),
                   "migration: a present (empty) key suppresses the synthesis");
        AppConfig key_present_full;
        TEST_CHECK(key_present_full.FromJsonString(
                       "{\"openai\": {\"base_url\": \"https://api.openai.com\", "
                       "\"model\": \"gpt-4o-mini\"}, \"openai_profiles\": [{\"name\": "
                       "\"existing\", \"base_url\": \"https://x.example.com\", "
                       "\"model\": \"m\"}]}") &&
                       key_present_full.openai_profiles.size() == 1 &&
                       key_present_full.openai_profiles[0].name == "existing",
                   "migration: a present library wins; nothing is synthesized");
    }

    // ---- (7) downgrade posture: the openai active block is untouched ------
    {
        AppConfig cfg;
        TEST_CHECK(cfg.FromJsonString(
                       "{\"openai\": {\"base_url\": \"https://api.openai.com\", "
                       "\"model\": \"gpt-4o-mini\", \"reasoning_effort\": \"low\"}, "
                       "\"openai_profiles\": [{\"name\": \"p\", \"base_url\": "
                       "\"https://x.example.com\", \"model\": \"m\"}], "
                       "\"ui_language\": \"ko\"}"),
                   "downgrade: a library-bearing file still loads");
        TEST_CHECK(cfg.openai.base_url == "https://api.openai.com" &&
                       cfg.openai.model == "gpt-4o-mini" &&
                       cfg.openai.reasoning_effort == "low",
                   "downgrade: the openai block parses exactly as pre-U1");
        // The active block's emission shape is byte-pinned (field order), so a
        // pre-U1 build reading a new file still finds its block intact.
        const std::string emitted = cfg.ToJsonString();
        TEST_CHECK(emitted.find(
                       "\"openai\": {\n"
                       "    \"base_url\": \"https://api.openai.com\",\n"
                       "    \"model\": \"gpt-4o-mini\",\n"
                       "    \"reasoning_effort\": \"low\",\n") != std::string::npos,
                   "downgrade: the openai block emission keeps its exact pre-U1 shape");
    }

    // ---- (12) negative key-material assertion ------------------------------
    {
        AppConfig cfg;
        cfg.FromJsonString(
            "{\"openai\": {\"base_url\": \"https://api.openai.com\", \"model\": \"m\", "
            "\"api_key_dpapi\": \"QUJD\", \"api_key_sha256\": \"0123456789abcdef\"}, "
            "\"openai_profiles\": [{\"name\": \"p\", \"base_url\": "
            "\"https://api.openai.com\", \"model\": \"m\", \"api_key_dpapi\": \"QUJD\", "
            "\"api_key_sha256\": \"0123456789abcdef\"}], \"ui_language\": \"ko\"}");
        const std::string emitted = cfg.ToJsonString();
        TEST_CHECK(emitted.find("\"api_key\"") == std::string::npos,
                   "key material: no emitted field is NAMED api_key (cleartext never exists)");
        TEST_CHECK(emitted.find("api_key_dpapi") != std::string::npos &&
                       emitted.find("api_key_sha256") != std::string::npos,
                   "key material: only the dpapi/sha256 pair fields exist");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261122_0001 B-U1 OpenAI profile config schema + helpers passed."
                  << std::endl;
    } else {
        std::cout << "[FAIL] 261122_0001 B-U1 OpenAI profile config schema + helpers: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

} // namespace openai_profile_tests
