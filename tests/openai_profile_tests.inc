// ===========================================================================
// 261122_0001 B-U1 (OpenAI 설정 창 프로필 재설계, builder draw 5; design-pick
// items 1/2/3/7): the OpenAI profile LIBRARY — config schema, parse/serialize/
// caps/migration, and the pure name-derivation helpers.
//
// Covered, per the U1 unit contract:
//   (1) round trip: parse -> emit -> re-parse equality (values AND bytes)
//   (2) empty library => the "openai_profiles" key is NOT emitted while the
//       fold was never touched; fix-A1 R1-12 emits an explicit [] once a
//       present/loaded fold was emptied (see TestOpenAiProfileFixA1)
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
// ---------------------------------------------------------------------------
// 261122_0001 B-U4: the dialog TU's pure PROFILE seams, defined at namespace
// scope in src/ui/openai_settings_window.cpp. Declared here (the suite rides
// inside run_tests.cpp, which links Emebalachat_core) because these helpers stay
// out of openai_settings_window.hpp on purpose (TU-scope seams, not public API)
// — the same split the declaration folds and the reasoning planners use. They
// are declared as PLAIN functions with out-parameters, not with structs, so no
// type definition has to be duplicated across TUs.
namespace emebalachat {
// 261122_0001 fix-D (R1-14): 2-arg exact compare — the predicate now takes the
// rendered mask string the dialog produced for its current pair (see
// RenderedOpenAiKeyPlaceholder in the dialog TU).
bool IsMaskedPlaceholder(std::string_view keyUtf8, std::string_view rendered_mask);
std::string RenderedOpenAiKeyPlaceholder(const OpenAiConfig& pair);
void PlanOpenAiReasoningReseed(std::string_view saved_value, std::string& out_value,
                               bool& out_pending);
std::string SanitizeOpenAiProfileDisplayText(std::string_view text);
std::vector<std::string> PlanOpenAiProfileComboLabels(
    const std::vector<OpenAiProfile>& profiles, std::string_view name_template,
    std::string_view fallback_label);
bool PlanOpenAiProfileSave(const std::vector<OpenAiProfile>& library,
                           const OpenAiConfig& candidate, std::string_view name_template,
                           std::vector<OpenAiProfile>& out_library,
                           std::size_t& out_index);
std::wstring FormatOpenAiProfileLimitNotice(std::wstring_view message, std::size_t limit);
bool ResolveOpenAiProfileKeyPair(std::string& key_edit, const OpenAiConfig& existing,
                                 OpenAiConfig& out);
// 261122_0001 B-U6fix (design item 4b): the opened-list width plan, defined at
// namespace scope in src/ui/openai_settings_window.cpp for the same cross-TU
// reason as the planners above: the suite drives the exact planning function
// the dialog hands to CB_SETDROPPEDWIDTH, with a deterministic measurer instead
// of GDI. The measurer is a std::function so the planner itself stays pure (no
// window, no DC, no GDI inside it).
int PlanOpenAiProfileDroppedWidth(
    const std::vector<std::string>& labels, int combo_width_px,
    const std::function<int(std::wstring_view)>& measure_utf16);
} // namespace emebalachat

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
        // 261122_0001 fix-A1 (R1-12): a PRESENT empty array marks the fold
        // touched, so the re-save EMITS the explicit [] — the pre-fix-A1 pin
        // asserted the key was omitted here, which is exactly the R1-12
        // resurrection hole (the next load's migration would re-synthesize).
        TEST_CHECK(emptied.ToJsonString().find("\"openai_profiles\": []") != std::string::npos,
                   "empty library: re-emission emits the explicit [] (R1-12)");
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

// ---------------------------------------------------------------------------
// 261122_0001 fix-A1 (builder draw 11; fix-list-round1 rows R1-02 / R1-03 /
// R1-12): the profile library's LOCKED VERB, the parse FIELD CAPS, the
// fail-closed DIGEST check, and the EXPLICIT-EMPTY-ARRAY emission.
//
//   R1-02  SetOpenAiProfiles mirrors SetOpenAiDeclaredLanguages: mutex_ +
//          move-in, the same lock the serialiser takes. The write-surface pin
//          below scans src/config.hpp + src/config.cpp ONLY — the remaining
//          main.cpp direct caller is unit A2's migration, per the fix-A1
//          scope (the member stays public until then; config.hpp's member
//          contract documents the rule).
//   R1-03  base_url <= 2048 B, model <= 512 B, api_key_dpapi <= 16384 B,
//          truncated at a UTF-8 code-point boundary, at the shared
//          ApplyOpenAiConfigPair choke point (active block AND entries).
//          api_key_sha256 must be exactly 64 lowercase hex chars; anything
//          else clears BOTH key fields (fail-closed pair atomicity).
//   R1-12  openai_profiles_set: the verb marks it even for an empty fold, the
//          load path marks it on a PRESENT key, the writer emits [] while
//          set-and-empty, and the key stays omitted only while never set —
//          so "deleted every profile" survives reload + re-save without the
//          migration re-synthesizing (parse stays presence-gated).
//
// Zero network, zero UI: pure config-layer checks only.
// ---------------------------------------------------------------------------
void TestOpenAiProfileFixA1() {
    std::cout << "[RUN] Testing 261122_0001 fix-A1 (R1-02/R1-03/R1-12)..." << std::endl;
    const int failures_before = g_failed_count;

    auto read_src = [](const std::filesystem::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    };
    auto trim_ascii = [](std::string_view text) {
        while (!text.empty() &&
               std::isspace(static_cast<unsigned char>(text.front()))) {
            text.remove_prefix(1);
        }
        while (!text.empty() &&
               std::isspace(static_cast<unsigned char>(text.back()))) {
            text.remove_suffix(1);
        }
        return std::string(text);
    };
    auto count_of = [](const std::string& hay, const std::string& needle) {
        std::size_t count = 0;
        for (std::size_t pos = hay.find(needle); pos != std::string::npos;
             pos = hay.find(needle, pos + needle.size())) {
            ++count;
        }
        return count;
    };
    const std::string kValidDigest(64, 'a'); // a well-formed digest stand-in

    // ---- R1-02 (a): the write-surface pin over config.hpp/cpp --------------
    {
        const std::filesystem::path config_cpp = ResolveRepoFile(L"src\\config.cpp");
        const std::filesystem::path config_hpp = ResolveRepoFile(L"src\\config.hpp");
        std::string cpp_src;
        std::string hpp_src;
        const bool cpp_read = !config_cpp.empty() && read_src(config_cpp, cpp_src);
        const bool hpp_read = !config_hpp.empty() && read_src(config_hpp, hpp_src);
        // R1-07 lesson: a source pin that cannot resolve its file must HARD-FAIL,
        // never print PASS over zero assertions.
        TEST_CHECK(cpp_read && hpp_read,
                   "fix-A1 R1-02: src/config.cpp + src/config.hpp resolve for the "
                   "write-surface pin");
        if (cpp_read) {
            const std::size_t begin_marker = cpp_src.find("fix-A1 R1-02 BEGIN");
            const std::size_t end_marker = cpp_src.find("fix-A1 R1-02 END");
            const bool markers_ok = begin_marker != std::string::npos &&
                                    end_marker != std::string::npos &&
                                    begin_marker < end_marker;
            TEST_CHECK(markers_ok,
                       "fix-A1 R1-02: the write-surface region markers exist and are "
                       "ordered");
            const std::string region =
                markers_ok ? cpp_src.substr(begin_marker, end_marker - begin_marker)
                           : std::string();
            // Structure pin, anchored so it cannot pass vacuously: the verb is
            // DEFINED inside the region and takes mutex_ exactly once — the
            // same shape as SetOpenAiDeclaredLanguages (guard + move-in).
            TEST_CHECK(region.find("void AppConfig::SetOpenAiProfiles(") !=
                           std::string::npos &&
                           count_of(region, "std::lock_guard<std::mutex> lock(mutex_);") == 1 &&
                           region.find("openai_profiles = std::move(profiles);") !=
                               std::string::npos &&
                           region.find("openai_profiles_set = true;") != std::string::npos,
                       "fix-A1 R1-02: SetOpenAiProfiles is the region's one locked writer "
                       "(guard + move + fold-touched mark, mirroring "
                       "SetOpenAiDeclaredLanguages)");
            // Every mutation-shaped mention of the member OUTSIDE the region
            // must be one of the two documented load-path statements in
            // FromJsonString (which already holds mutex_): the authoritative
            // replace and the synthesis clear+append.
            const char* const needles[] = {
                "openai_profiles = ",          "openai_profiles.clear()",
                "openai_profiles.push_back(",  "openai_profiles.emplace_back(",
                "openai_profiles.erase(",      "openai_profiles.insert(",
                "openai_profiles.assign(",     "openai_profiles.resize(",
                "openai_profiles.pop_back(",   "openai_profiles.swap(",
            };
            std::size_t outside = 0;
            std::string outside_detail;
            std::size_t inside = 0;
            for (const char* needle : needles) {
                const std::string text(needle);
                for (std::size_t pos = cpp_src.find(text); pos != std::string::npos;
                     pos = cpp_src.find(text, pos + text.size())) {
                    // Identifier-boundary rule (the PR9-12 pin's trick): a hit
                    // glued to a larger name ("raw_openai_profiles = v", the
                    // FromJsonString raw-capture variable) is not the member.
                    if (pos > 0) {
                        const char prev = cpp_src[pos - 1];
                        const bool ident = (prev >= 'a' && prev <= 'z') ||
                                           (prev >= 'A' && prev <= 'Z') ||
                                           (prev >= '0' && prev <= '9') ||
                                           prev == '_' || prev == '.';
                        if (ident) {
                            continue;
                        }
                    }
                    if (markers_ok && pos > begin_marker && pos < end_marker) {
                        ++inside;
                        continue;
                    }
                    // The line the mention sits on, trimmed, must be a
                    // whitelisted load-path statement and nothing else.
                    const std::size_t line_begin = cpp_src.rfind('\n', pos) + 1;
                    const std::size_t line_end = cpp_src.find('\n', pos);
                    const std::string line =
                        trim_ascii(std::string_view(cpp_src)
                                       .substr(line_begin, line_end - line_begin));
                    const bool load_path =
                        line == "openai_profiles = std::move(parsed_profiles);" ||
                        line == "openai_profiles.clear();" ||
                        line == "openai_profiles.push_back(std::move(synthesized));";
                    if (!load_path) {
                        ++outside;
                        outside_detail += line + " | ";
                    }
                }
            }
            TEST_CHECK(outside == 0,
                       "fix-A1 R1-02: no openai_profiles mutation survives outside the "
                       "locked verb and the three load-path statements");
            if (outside != 0) {
                std::cerr << "  [fix-A1-write-surface] " << outside_detail << std::endl;
            }
            TEST_CHECK(inside >= 1,
                       "fix-A1 R1-02: the marked region really carries a mutation (the pin "
                       "cannot pass vacuously)");
            TEST_CHECK(count_of(cpp_src, "openai_profiles = std::move(parsed_profiles);") == 1 &&
                           count_of(cpp_src, "openai_profiles.clear();") == 1 &&
                           count_of(cpp_src,
                                    "openai_profiles.push_back(std::move(synthesized));") == 1,
                       "fix-A1 R1-02: the whitelisted load-path statements appear exactly "
                       "once each");
            // The fold-touched flag has exactly two writers: the verb and the
            // present-key parse branch (never the writer emission, which reads).
            TEST_CHECK(count_of(cpp_src, "openai_profiles_set = true;") == 2,
                       "fix-A1 R1-12: openai_profiles_set is written in exactly two places "
                       "(the verb + the load path)");
        }
        if (hpp_read) {
            // config.hpp declares the member and the flag; it mutates neither.
            TEST_CHECK(hpp_src.find("openai_profiles = ") == std::string::npos &&
                           hpp_src.find("openai_profiles.push_back(") == std::string::npos &&
                           hpp_src.find("openai_profiles.clear()") == std::string::npos,
                       "fix-A1 R1-02: config.hpp declares the member without mutating it");
            TEST_CHECK(count_of(hpp_src, "openai_profiles_set = false;") == 1 &&
                           hpp_src.find("bool openai_profiles_set = false;") !=
                               std::string::npos,
                       "fix-A1 R1-12: the flag is declared false exactly once (never set "
                       "outside config.cpp)");
            TEST_CHECK(hpp_src.find("void SetOpenAiProfiles(std::vector<OpenAiProfile> "
                                    "profiles);") != std::string::npos,
                       "fix-A1 R1-02: the locked verb is declared on AppConfig");
        }
    }

    // ---- fix-E (R2-N2): the ACTIVE openai block's locked write surface -------
    // Round-2 closure of the same PR9-12 class on the older REQ-045/047/051
    // member: the GUI-thread settings-dialog write now goes through a locked
    // verb, so it can no longer interleave with an off-thread SaveToFile()
    // serialisation of the block under mutex_. config.cpp/config.hpp are read
    // again here (this block sits outside the R1-02(a) scope that owns
    // cpp_src/hpp_src).
    {
        const std::filesystem::path e_config_cpp = ResolveRepoFile(L"src\\config.cpp");
        const std::filesystem::path e_config_hpp = ResolveRepoFile(L"src\\config.hpp");
        std::string e_cpp_src, e_hpp_src;
        const bool e_cpp_read = !e_config_cpp.empty() &&
                                read_src(e_config_cpp, e_cpp_src);
        const bool e_hpp_read = !e_config_hpp.empty() &&
                                read_src(e_config_hpp, e_hpp_src);
        TEST_CHECK(e_cpp_read && e_hpp_read,
                   "fix-E R2-N2: src/config.cpp + src/config.hpp resolve for the "
                   "openai-block write-surface pin");
        if (e_cpp_read) {
            const std::size_t e_begin = e_cpp_src.find("fix-E R2-N2 BEGIN");
            const std::size_t e_end = e_cpp_src.find("fix-E R2-N2 END");
            const bool e_markers_ok = e_begin != std::string::npos &&
                                      e_end != std::string::npos && e_begin < e_end;
            TEST_CHECK(e_markers_ok,
                       "fix-E R2-N2: the openai-block write-surface region markers exist "
                       "and are ordered");
            const std::string e_region =
                e_markers_ok ? e_cpp_src.substr(e_begin, e_end - e_begin) : std::string();
            // Structure pin, anchored so it cannot pass vacuously: the verb is
            // DEFINED inside the region and takes mutex_ exactly once — the
            // same guard + move-in shape as SetOpenAiProfiles.
            TEST_CHECK(e_region.find("void AppConfig::SetOpenAiConfig(") != std::string::npos &&
                           count_of(e_region, "std::lock_guard<std::mutex> lock(mutex_);") == 1 &&
                           e_region.find("openai = std::move(block);") != std::string::npos,
                       "fix-E R2-N2: SetOpenAiConfig is the region's one locked writer "
                       "(guard + move-in, mirroring SetOpenAiProfiles)");
            // Zero direct whole-block writers OUTSIDE the region: the load
            // path writes field-by-field through the shared
            // ApplyOpenAiConfigPair choke point (an `out` reference parameter,
            // never the member), so exactly ONE `openai = ` may exist in the
            // whole file — the verb body's own move-in, inside the region
            // (round-3 hardening: whole-file count, not a suffix scan, so a
            // writer placed BEFORE the BEGIN marker cannot evade).
            TEST_CHECK(count_of(e_cpp_src, "openai = ") == 1 &&
                           e_cpp_src.find("openai = ") >= e_begin &&
                           e_cpp_src.find("openai = ") < e_end,
                       "fix-E R2-N2: exactly one `openai = ` whole-block writer exists in "
                       "config.cpp and it is the locked verb's move-in inside the region");
        }
        if (e_hpp_read) {
            TEST_CHECK(e_hpp_src.find("void SetOpenAiConfig(OpenAiConfig block);") !=
                           std::string::npos,
                       "fix-E R2-N2: the locked verb is declared on AppConfig");
            TEST_CHECK(e_hpp_src.find("openai = ") == std::string::npos,
                       "fix-E R2-N2: config.hpp declares the member without mutating it");
        }
    }

    // ---- fix-E (R2-N2): zero direct writers in the runtime callers ----------
    {
        const std::filesystem::path main_cpp = ResolveRepoFile(L"src\\main.cpp");
        std::string main_src;
        if (!main_cpp.empty()) {
            std::ifstream in(main_cpp, std::ios::binary);
            main_src.assign((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
        }
        // R1-07 lesson: a source pin that cannot resolve its file must
        // HARD-FAIL, never print PASS over zero assertions.
        TEST_CHECK(!main_src.empty(),
                   "fix-E R2-N2: src/main.cpp resolves for the zero-direct-writers pin");
        if (!main_src.empty()) {
            TEST_CHECK(main_src.find("config.openai = ") == std::string::npos,
                       "fix-E R2-N2: no direct `config.openai = ` writer survives in "
                       "main.cpp (both dialog sites go through the locked verb)");
            TEST_CHECK(count_of(main_src, "config.SetOpenAiConfig(") == 2,
                       "fix-E R2-N2: both runtime write sites call the locked verb "
                       "(g_apply_openai_settings + g_apply_openai_deleted)");
        }
    }

    // ---- R1-02 (b): the verb's observable behavior --------------------------
    {
        AppConfig cfg;
        std::vector<OpenAiProfile> library;
        library.push_back(MakeProfile("one", "https://a.example.com", "m1"));
        library.push_back(MakeProfile("two", "https://b.example.com", "m2"));
        cfg.SetOpenAiProfiles(library);
        TEST_CHECK(cfg.openai_profiles.size() == 2 &&
                       ProfileVectorsEqual(cfg.openai_profiles, library),
                   "R1-02 verb: the library moves in and reads back verbatim");
        // A second call replaces wholesale (the fold is authoritative).
        cfg.SetOpenAiProfiles({MakeProfile("only", "https://c.example.com", "m3")});
        TEST_CHECK(cfg.openai_profiles.size() == 1 &&
                       cfg.openai_profiles[0].name == "only",
                   "R1-02 verb: a second write replaces the library");
        // An empty write is a real edit, observable through the writer (R1-12
        // couples the verb to the explicit [] emission).
        cfg.SetOpenAiProfiles({});
        TEST_CHECK(cfg.openai_profiles.empty() &&
                       cfg.ToJsonString().find("\"openai_profiles\": []") !=
                           std::string::npos,
                   "R1-02/R1-12 verb: SetOpenAiProfiles({}) empties the fold and the "
                   "writer emits the explicit []");
    }

    // ---- R1-03 (c): the field caps on the ACTIVE openai block ---------------
    {
        AppConfig ascii_cap;
        const std::string long_url(2100, 'x'); // far past the 2048-byte cap
        TEST_CHECK(ascii_cap.FromJsonString("{\"openai\": {\"base_url\": \"" + long_url +
                                            "\", \"model\": \"" + std::string(600, 'm') +
                                            "\", \"api_key_dpapi\": \"" +
                                            std::string(17000, 'k') + "\"}}"),
                   "R1-03 caps: an oversized active block still loads (lenient posture)");
        TEST_CHECK(ascii_cap.openai.base_url.size() == 2048,
                   "R1-03 caps: base_url capped at 2048 bytes");
        TEST_CHECK(ascii_cap.openai.model.size() == 512,
                   "R1-03 caps: model capped at 512 bytes");
        TEST_CHECK(ascii_cap.openai.api_key_dpapi.size() == 16384,
                   "R1-03 caps: api_key_dpapi capped at 16384 bytes");

        AppConfig utf8_cap;
        // 2047 ASCII bytes + one 3-byte code point: the cap falls INSIDE the
        // sequence, so the cut must retreat to the boundary (2047), never
        // emit a split trailing byte. Same shape for model and the blob.
        const std::string hangul = "\xed\x95\x9c"; // U+D55C 한
        TEST_CHECK(utf8_cap.FromJsonString(
                       "{\"openai\": {\"base_url\": \"" + std::string(2047, 'x') + hangul +
                       "\", \"model\": \"" + std::string(510, 'm') + hangul +
                       "\", \"api_key_dpapi\": \"" + std::string(16383, 'k') + hangul +
                       "\"}}"),
                   "R1-03 caps: a multibyte-at-the-cap block loads");
        TEST_CHECK(utf8_cap.openai.base_url.size() == 2047 &&
                       (utf8_cap.openai.base_url.back() & 0x80) == 0,
                   "R1-03 caps: base_url truncated at the UTF-8 code-point boundary "
                   "(no split sequence)");
        TEST_CHECK(utf8_cap.openai.model.size() == 510 &&
                       (utf8_cap.openai.model.back() & 0x80) == 0,
                   "R1-03 caps: model truncated at the UTF-8 code-point boundary");
        TEST_CHECK(utf8_cap.openai.api_key_dpapi.size() == 16383 &&
                       (utf8_cap.openai.api_key_dpapi.back() & 0x80) == 0,
                   "R1-03 caps: api_key_dpapi truncated at the UTF-8 code-point boundary");

        // Honest values sit far below the caps: untouched.
        AppConfig honest;
        TEST_CHECK(honest.FromJsonString(
                       "{\"openai\": {\"base_url\": \"https://api.openai.com/v1\", "
                       "\"model\": \"gpt-4o-mini\"}}") &&
                       honest.openai.base_url == "https://api.openai.com/v1" &&
                       honest.openai.model == "gpt-4o-mini",
                   "R1-03 caps: honest values pass through verbatim (no behavior change)");
    }

    // ---- R1-03 (d): the same caps on PROFILE ENTRIES (the shared choke point)
    {
        AppConfig entries;
        TEST_CHECK(entries.FromJsonString(
                       "{\"openai_profiles\": [{\"name\": \"p\", \"base_url\": \"" +
                       std::string(2100, 'x') + "\", \"model\": \"" + std::string(600, 'm') +
                       "\", \"api_key_dpapi\": \"" + std::string(17000, 'k') + "\"}], "
                       "\"ui_language\": \"ko\"}"),
                   "R1-03 caps: an oversized entry still loads");
        TEST_CHECK(entries.openai_profiles.size() == 1 &&
                       entries.openai_profiles[0].cfg.base_url.size() == 2048 &&
                       entries.openai_profiles[0].cfg.model.size() == 512 &&
                       entries.openai_profiles[0].cfg.api_key_dpapi.size() == 16384,
                   "R1-03 caps: entry base_url/model/blob capped at the same sizes");
    }

    // ---- R1-03 (e): the digest fail-closed check -----------------------------
    {
        // Malformed digest shapes (writer order: dpapi BEFORE sha256): the
        // whole key pair clears, the block's other fields are untouched.
        const std::string uppercase = [&] {
            std::string v = kValidDigest;
            for (char& c : v) {
                c = static_cast<char>(c - 'a' + 'A');
            }
            return v;
        }();
        const std::string non_hex = std::string(63, 'a') + 'g';
        const char* const malformed[] = {
            "0123456789abcdef", // wrong length
            uppercase.c_str(),  // right length, wrong case
            non_hex.c_str(),    // right length, non-hex
            "",                 // empty digest vouches for nothing
        };
        for (const char* bad : malformed) {
            const std::string doc =
                std::string("{\"openai\": {\"base_url\": \"https://api.openai.com\", ") +
                "\"model\": \"gpt-4o-mini\", \"reasoning_effort\": \"low\", "
                "\"api_key_dpapi\": \"QUJD\", \"api_key_sha256\": \"" + bad +
                "\", \"http_consent_given\": true}}";
            AppConfig block;
            TEST_CHECK(block.FromJsonString(doc) && block.openai.api_key_dpapi.empty() &&
                           block.openai.api_key_sha256.empty(),
                       std::string("R1-03 digest: malformed active-block digest clears the "
                                   "pair (fail-closed): ") +
                           (bad[0] == '\0' ? std::string("<empty>") : std::string(bad)));
            TEST_CHECK(block.openai.base_url == "https://api.openai.com" &&
                           block.openai.model == "gpt-4o-mini" &&
                           block.openai.reasoning_effort == "low" &&
                           block.openai.http_consent_given,
                       "R1-03 digest: clearing the pair leaves the rest of the block intact");
            // The same fail-closed rule runs on profile entries.
            const std::string entry_doc =
                std::string("{\"openai_profiles\": [{\"name\": \"p\", \"base_url\": "
                            "\"https://api.openai.com\", \"model\": \"gpt-4o-mini\", "
                            "\"api_key_dpapi\": \"QUJD\", \"api_key_sha256\": \"") +
                bad + "\"}], \"ui_language\": \"ko\"}";
            AppConfig entry;
            TEST_CHECK(entry.FromJsonString(entry_doc) &&
                           entry.openai_profiles.size() == 1 &&
                           entry.openai_profiles[0].cfg.api_key_dpapi.empty() &&
                           entry.openai_profiles[0].cfg.api_key_sha256.empty(),
                       "R1-03 digest: malformed entry digest clears the pair (fail-closed)");
        }
        // A malformed digest with NO blob beside it stays a clean empty pair.
        AppConfig no_blob;
        TEST_CHECK(no_blob.FromJsonString(
                       "{\"openai\": {\"api_key_sha256\": \"not-a-digest\"}}") &&
                       no_blob.openai.api_key_dpapi.empty() &&
                       no_blob.openai.api_key_sha256.empty(),
                   "R1-03 digest: a malformed digest alone reads as no key material");
        // A well-formed pair round-trips untouched, block and entry alike.
        const std::string pair_doc =
            std::string("{\"openai\": {\"base_url\": \"https://api.openai.com\", "
                        "\"model\": \"gpt-4o-mini\", \"api_key_dpapi\": \"QUJD\", "
                        "\"api_key_sha256\": \"") +
            kValidDigest +
            "\"}, \"openai_profiles\": [{\"name\": \"p\", \"api_key_dpapi\": \"QUJD\", "
            "\"api_key_sha256\": \"" +
            kValidDigest + "\"}], \"ui_language\": \"ko\"}";
        AppConfig good;
        TEST_CHECK(good.FromJsonString(pair_doc) && good.openai.api_key_dpapi == "QUJD" &&
                       good.openai.api_key_sha256 == kValidDigest &&
                       good.openai_profiles.size() == 1 &&
                       good.openai_profiles[0].cfg.api_key_dpapi == "QUJD" &&
                       good.openai_profiles[0].cfg.api_key_sha256 == kValidDigest,
                   "R1-03 digest: a well-formed pair round-trips verbatim");
    }

    // ---- R1-12 (f): the explicit empty-array emission ------------------------
    {
        // The full resurrection-closing loop: a legacy file (no key) migrates
        // to one synthesized profile; the user deletes every profile through
        // the verb; the save emits []; the reload of THAT text keeps the fold
        // empty even with the openai block still configured, and a second
        // save stays []. (Pre-fix-A1, the reload synthesized the profile back.)
        AppConfig migrated;
        TEST_CHECK(migrated.FromJsonString(
                       "{\"ui_language\": \"ko\", \"openai\": {\"base_url\": "
                       "\"https://api.openai.com\", \"model\": \"gpt-4o-mini\"}}") &&
                       migrated.openai_profiles.size() == 1,
                   "R1-12: the pre-fix migration still synthesizes one profile");
        migrated.SetOpenAiProfiles({});
        const std::string saved = migrated.ToJsonString();
        TEST_CHECK(saved.find("\"openai_profiles\": []") != std::string::npos,
                   "R1-12: deleting every profile saves an explicit []");
        AppConfig reloaded;
        TEST_CHECK(reloaded.FromJsonString(saved) && reloaded.openai_profiles.empty(),
                   "R1-12: the reload reads the empty fold (no migration despite the "
                   "configured openai block)");
        TEST_CHECK(reloaded.ToJsonString().find("\"openai_profiles\": []") !=
                       std::string::npos,
                   "R1-12: the re-save keeps the explicit [] (stable round trip)");
        // A never-touched fold keeps the key omitted entirely.
        AppConfig fresh;
        TEST_CHECK(fresh.ToJsonString().find("openai_profiles") == std::string::npos,
                   "R1-12: a never-touched fold omits the key entirely");
        // A non-empty fold load sets the flag too, so deleting down to zero
        // through the verb after any load still saves [].
        AppConfig loaded_nonempty;
        TEST_CHECK(loaded_nonempty.FromJsonString(
                       "{\"openai_profiles\": [{\"name\": \"p\"}], \"ui_language\": \"ko\"}"),
                   "R1-12: a non-empty library loads");
        loaded_nonempty.SetOpenAiProfiles({});
        TEST_CHECK(loaded_nonempty.ToJsonString().find("\"openai_profiles\": []") !=
                       std::string::npos,
                   "R1-12: delete-all after a library load saves the explicit []");
    }

    // ---- R1-03 (g): the migration synthesis inherits the caps ----------------
    {
        // The synthesized profile is BUILT from the parsed `openai` block, so
        // the capped/digest-checked values flow into it for free.
        AppConfig capped;
        TEST_CHECK(capped.FromJsonString(
                       "{\"openai\": {\"base_url\": \"https://api.openai.com\", "
                       "\"model\": \"" +
                       std::string(600, 'm') + "\"}}") &&
                       capped.openai_profiles.size() == 1 &&
                       capped.openai_profiles[0].cfg.model.size() == 512,
                   "R1-03 migration: the synthesized profile inherits the capped model");
        AppConfig bad_key;
        TEST_CHECK(bad_key.FromJsonString(
                       "{\"openai\": {\"base_url\": \"https://api.openai.com\", "
                       "\"model\": \"gpt-4o-mini\", \"api_key_dpapi\": \"QUJD\", "
                       "\"api_key_sha256\": \"not-a-digest\"}}") &&
                       bad_key.openai_profiles.size() == 1 &&
                       bad_key.openai_profiles[0].cfg.api_key_dpapi.empty() &&
                       bad_key.openai_profiles[0].cfg.api_key_sha256.empty() &&
                       bad_key.openai_profiles[0].cfg.base_url ==
                           "https://api.openai.com",
                   "R1-03 migration: the synthesized profile inherits the fail-closed "
                   "pair clearing (blob + digest empty, identity fields intact)");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261122_0001 fix-A1 (R1-02/R1-03/R1-12) passed."
                  << std::endl;
    } else {
        std::cout << "[FAIL] 261122_0001 fix-A1 (R1-02/R1-03/R1-12): "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// 261122_0001 B-U2 (builder draw 6): the OpenAI profile row's six StringIds.
//
// The completeness loop in TestR6P5P6I18n already enforces non-empty for every
// StringId below EnumCount in every authored locale (and Get()'s `default:
// return L""` is what makes a missing switch case fail it), so this suite does
// NOT repeat that whole loop. It owns what the shared loop cannot see:
//   (a) the EnumCount total at this unit's boundary (207 -> 213);
//   (b) the insertion order: the six ids sit IMMEDIATELY after
//       BackTransSectionOriginal and immediately before the 261121_0002
//       progress/cancel run, so every later relative adjacency pin (the
//       P6a/P7 run, the P9c-1b run, the P5 EnumCount sentinel) still holds;
//   (c) a targeted non-empty cross-check of exactly these six ids per locale;
//   (d) the English-copy scan over ALL authored tables for the five TEXT ids
//       (the template is excluded: " / " shapes legitimately repeat). A
//       dropped designated initializer keeps the English struct default, which
//       the shared non-empty loop cannot see. An exact "Save"/"Delete"
//       loanword must be whitelisted per locale below, never blanket-allowed;
//   (e) the token pins: exactly one {h} and one {m} in every table's template
//       and exactly one {n} in every table's cap notice. 261122_0001 B-U4's
//       formatter (FormatOpenAiProfileLimitNotice) substitutes EVERY occurrence,
//       so a second token would render as a second number rather than literally
//       — the one-token rule stays the locale tables' contract either way (a
//       duplicated token is a table bug, not a rendering mode).
// ---------------------------------------------------------------------------
void TestOpenAiProfileI18nStrings() {
    std::cout << "[TEST] 261122_0001 B-U2: OpenAI profile row strings (89 locales)..."
              << std::endl;
    const int failures_before = g_failed_count;
    const UiLocale initial = I18n::GetCurrentLocale();
    const auto& locales = GetSupportedUiLocales();
    TEST_CHECK(static_cast<int>(locales.size()) == AuthoredUiLocaleCount(),
               "B-U2: the profile strings are authored for every authored UI locale (count derived)");

    const StringId kIds[] = {
        StringId::OpenAiProfileLabel,
        StringId::OpenAiProfileSave,
        StringId::OpenAiProfileDelete,
        StringId::OpenAiProfileNameTemplate,
        StringId::OpenAiProfileDeleteConfirm,
        StringId::OpenAiProfileLimit,
    };
    const wchar_t* kEn[] = {
        L"Profile",
        L"Save",
        L"Delete",
        L"{h} / {m}",
        L"Delete this profile? The stored API key will be removed too.",
        L"You can save up to {n} profiles. Delete unused profiles to add more.",
    };
    const wchar_t* kKo[] = {
        L"프로필",
        L"저장",
        L"삭제",
        L"{h}의 {m} 모델",
        L"이 프로필을 삭제할까요? 저장된 API 키도 함께 삭제됩니다.",
        L"프로필은 최대 {n}개까지 저장할 수 있습니다. 사용하지 않는 프로필을 삭제해 주세요.",
    };
    // volatile keeps the comparisons non-constant under /W4 (C4127 discipline).
    volatile std::size_t kIdCount = sizeof(kIds) / sizeof(kIds[0]);
    volatile std::size_t kEnCount = sizeof(kEn) / sizeof(kEn[0]);
    volatile std::size_t kKoCount = sizeof(kKo) / sizeof(kKo[0]);
    TEST_CHECK(kIdCount == 6 && kEnCount == kIdCount && kKoCount == kIdCount,
               "B-U2: the inventory is the six profile ids with one English and one Korean expectation each");

    // ---- (a) EnumCount: 207 -> 213 at this unit's boundary; 213 -> 221 after
    // the 261122_0002 Phase-2 i18n worker appended the 8 translation-card ids;
    // 221 -> 228 after the 261122_0002 Phase-3 i18n worker appended the 7
    // settings-window redesign ids; 228 -> 247 after the 261122_0002 Phase-3
    // micro-pass appended the 19 settings-hub / cheat-sheet ids; 247 -> 248
    // after 261123_0001 appended the 1 user-model load-failure hint id;
    // 250 -> 251 after 261124_0001 appended the 2 model-load-failed ids --
    volatile int enum_count = static_cast<int>(StringId::EnumCount);
    TEST_CHECK(enum_count == 251,
               "B-U2: StringId::EnumCount is 251 (207 + the six OpenAI profile ids + the 8 261122_0002 Phase-2 translation-card editor ids + the 7 261122_0002 Phase-3 settings-window redesign ids + the 19 261122_0002 Phase-3 micro-pass ids + the 1 261123_0001 user-model load-failure hint id + the 1 261123_0001 Unit-C local-context checkbox id + the 2 261124_0001 model-load-failed ids)");

    // ---- (b) insertion order ----------------------------------------------
    // BackTransSectionOriginal + 1 == the first profile id, the six ids are a
    // contiguous run, and the run ENDS where the pre-existing progress/cancel
    // run begins. The P5 sentinel witness is that every LATER id shifted by
    // the same +6 instead of drifting; since 261122_0002 it sat at
    // EnumCount - 35, at EnumCount - 36 since 261123_0001, at EnumCount
    // - 37 since 261123_0001 Unit-C, and at EnumCount - 39 since 261124_0001
    // (the 8 Phase-2 + the 7 Phase-3 + the 19 Phase-3 micro-pass + the 1
    // 261123_0001 hint + the 1 261123_0001 Unit-C + the 2 261124_0001
    // model-load-failed ids now close the enum tail).
    volatile int back_original = static_cast<int>(StringId::BackTransSectionOriginal);
    volatile int profile_label = static_cast<int>(StringId::OpenAiProfileLabel);
    volatile int profile_save = static_cast<int>(StringId::OpenAiProfileSave);
    volatile int profile_delete = static_cast<int>(StringId::OpenAiProfileDelete);
    volatile int profile_template = static_cast<int>(StringId::OpenAiProfileNameTemplate);
    volatile int profile_confirm = static_cast<int>(StringId::OpenAiProfileDeleteConfirm);
    volatile int profile_limit = static_cast<int>(StringId::OpenAiProfileLimit);
    volatile int progress_after = static_cast<int>(StringId::BadgeTranslatingProgress);
    volatile int p5_sentinel = static_cast<int>(StringId::LanguageSearchNoMatch);
    TEST_CHECK(profile_label == back_original + 1 &&
                   profile_save == profile_label + 1 &&
                   profile_delete == profile_save + 1 &&
                   profile_template == profile_delete + 1 &&
                   profile_confirm == profile_template + 1 &&
                   profile_limit == profile_confirm + 1 &&
                   progress_after == profile_limit + 1 &&
                   p5_sentinel == enum_count - 39,
               "B-U2: the six profile ids are contiguous right after "
               "BackTransSectionOriginal and right before the 261121_0002 "
               "progress/cancel run, so the P5 sentinel witness (EnumCount - 39) "
               "and every relative adjacency pin still hold");

    // ---- (c) the designed English and Korean values, verbatim --------------
    I18n::SetLocale(UiLocale::English);
    int en_mismatch = 0;
    for (std::size_t i = 0; i < kIdCount; ++i) {
        if (I18n::Get(kIds[i]) != kEn[i]) {
            ++en_mismatch;
        }
    }
    TEST_CHECK(en_mismatch == 0,
               "B-U2 en: all six values are the authored English literals (the "
               "positional table landed on the six matching struct slots)");
    I18n::SetLocale(UiLocale::Korean);
    int ko_mismatch = 0;
    for (std::size_t i = 0; i < kIdCount; ++i) {
        if (I18n::Get(kIds[i]) != kKo[i]) {
            ++ko_mismatch;
        }
    }
    TEST_CHECK(ko_mismatch == 0,
               "B-U2 ko: all six Korean values match the authored literals verbatim");

    // ---- (d) English-copy scan + (e) token pins, one pass over 89 tables ---
    struct LoanwordWhitelist {
        UiLocale locale;
        StringId id;
    };
    const LoanwordWhitelist kWhitelist[] = {
        // Filipino (Taglish table: "I-download", "naka-install", "Modelo"):
        // "profile" IS the borrowed English word here, so this one exact match
        // is authored, not a missed translation. No other locale is allowed an
        // exact English copy of any text-bearing profile id.
        {UiLocale::Filipino, StringId::OpenAiProfileLabel},
    };
    const int kTextIds[] = {0, 1, 2, 4, 5};
    int non_empty = 0;
    int english_copy = 0;
    int whitelist_hits = 0;
    int token_bad = 0;
    for (const UiLocaleEntry& entry : locales) {
        I18n::SetLocale(entry.locale);
        bool all_non_empty = true;
        for (std::size_t i = 0; i < kIdCount; ++i) {
            if (I18n::Get(kIds[i]).empty()) {
                all_non_empty = false;
            }
        }
        if (all_non_empty) {
            ++non_empty;
        }
        for (int i : kTextIds) {
            const std::wstring value = I18n::Get(kIds[i]);
            if (entry.locale == UiLocale::English || value != kEn[i]) {
                continue;
            }
            bool whitelisted = false;
            for (const LoanwordWhitelist& allow : kWhitelist) {
                if (allow.locale == entry.locale && allow.id == kIds[i]) {
                    whitelisted = true;
                }
            }
            if (whitelisted) {
                ++whitelist_hits;
            } else {
                ++english_copy;
            }
        }
        const std::wstring name_template = I18n::Get(StringId::OpenAiProfileNameTemplate);
        const std::wstring cap_notice = I18n::Get(StringId::OpenAiProfileLimit);
        const auto exactly_one = [](const std::wstring& hay, const wchar_t* token) {
            const std::size_t first = hay.find(token);
            return first != std::wstring::npos &&
                   hay.find(token, first + std::wstring(token).size()) == std::wstring::npos;
        };
        if (!exactly_one(name_template, L"{h}") || !exactly_one(name_template, L"{m}") ||
            !exactly_one(cap_notice, L"{n}")) {
            ++token_bad;
        }
    }
    I18n::SetLocale(initial); // suite-state hygiene
    TEST_CHECK(non_empty == static_cast<int>(locales.size()),
               "B-U2: the six profile ids resolve non-empty in every authored locale "
               "(on top of the shared EnumCount completeness loop)");
    TEST_CHECK(english_copy == 0,
               "B-U2: no authored table falls back to the English profile text (the one "
               "whitelisted Taglish loanword excepted; a placeholder copy fails here)");
    TEST_CHECK(static_cast<int>(whitelist_hits) ==
                   static_cast<int>(sizeof(kWhitelist) / sizeof(kWhitelist[0])),
               "B-U2: every loanword whitelist entry is still a LIVE exact match (a stale "
               "entry would silently re-open the fallback hole)");
    TEST_CHECK(token_bad == 0,
               "B-U2: every table keeps exactly one {h} and one {m} in the name template "
               "and exactly one {n} in the cap notice");

    // ---- (f) designator presence + the English positional slot -------------
    std::string i18n_src;
    if (!ReadRepoSrc(L"src\\i18n.cpp", i18n_src)) {
        TEST_CHECK(false, "B-U2: src/i18n.cpp must be readable for the designator-presence pin");
    } else {
        const char* kFields[] = {
            "openai_profile_label",
            "openai_profile_save",
            "openai_profile_delete",
            "openai_profile_name_template",
            "openai_profile_delete_confirm",
            "openai_profile_limit",
        };
        const std::size_t kExpectedDesignators =
            static_cast<std::size_t>(AuthoredUiLocaleCount() - 1);
        int field_short = 0;
        for (const char* field : kFields) {
            const std::string designator = std::string("    .") + field + " = ";
            std::size_t count = 0;
            for (std::size_t at = i18n_src.find(designator); at != std::string::npos;
                 at = i18n_src.find(designator, at + 1)) {
                ++count;
            }
            if (count != kExpectedDesignators) {
                ++field_short;
            }
        }
        TEST_CHECK(field_short == 0,
                   "B-U2: each of the six fields is designated in exactly one table per "
                   "authored locale (the positional English one excepted) - no locale "
                   "inherits the English default (count derived from the gate)");

        const std::string table_open = "const LocalizedStrings kStringsEnglish = {";
        const std::size_t table_at = i18n_src.find(table_open);
        TEST_CHECK(table_at != std::string::npos,
                   "B-U2: the English positional table is locatable");
        if (table_at != std::string::npos) {
            const std::size_t table_end = i18n_src.find("\n};", table_at);
            const std::string body = i18n_src.substr(table_at, table_end - table_at);
            const std::size_t p_original = body.find("L\"Original text\",");
            const std::size_t p_label = body.find("L\"Profile\",");
            const std::size_t p_save = body.find("L\"Save\",");
            const std::size_t p_delete = body.find("L\"Delete\",");
            const std::size_t p_template = body.find("L\"{h} / {m}\",");
            const std::size_t p_confirm =
                body.find("L\"Delete this profile? The stored API key will be removed too.\",");
            const std::size_t p_limit = body.find(
                "L\"You can save up to {n} profiles. Delete unused profiles to add more.\",");
            const std::size_t p_progress = body.find("{c}/{t}");
            TEST_CHECK(p_original != std::string::npos && p_label != std::string::npos &&
                           p_save != std::string::npos && p_delete != std::string::npos &&
                           p_template != std::string::npos && p_confirm != std::string::npos &&
                           p_limit != std::string::npos && p_progress != std::string::npos &&
                           p_original < p_label && p_label < p_save && p_save < p_delete &&
                           p_delete < p_template && p_template < p_confirm &&
                           p_confirm < p_limit && p_limit < p_progress,
                       "B-U2 en: the six literals sit between \"Original text\" and the "
                       "progress/cancel run in the positional table (slot-for-slot with "
                       "the struct block)");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261122_0001 B-U2 OpenAI profile i18n pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] 261122_0001 B-U2 OpenAI profile i18n pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ===========================================================================
// 261122_0001 B-U4 (builder draw 8; design-pick items 4 + 9): the OpenAI
// settings dialog's PROFILE-ROW BEHAVIOR and the fold that carries the staged
// library out of the modal loop.
//
// Boundary, stated honestly (see the unit report): the dialog proc itself needs
// a live HWND (DialogBoxIndirectParamW) and this suite has no dialog harness, so
// the checks split in two layers —
//   (A) the PURE seams the proc calls, driven directly with real values: the
//       combo label plan, the save/upsert plan (identity match / unique derived
//       name / cap refusal), the {n} limit-notice formatter, the key-pair matrix
//       (REAL DPAPI + REAL digest), and the reasoning re-seed rule (risk R3);
//   (B) SOURCE-STRUCTURE pins for the wiring that only exists inside the proc:
//       the WM_INITDIALOG seed, CBN_SELCHANGE, the two button cases, the
//       IDOK / IDCANCEL / [설정 삭제] fold semantics and the "the dialog never
//       writes the library to disk" rule.
// Layer (B) asserts the shape, not the runtime: a live-dialog smoke test of the
// picker/confirmation boxes stays a manual QA item.
// ===========================================================================
void TestOpenAiProfileDialogBehavior() {
    std::cout << "[TEST] 261122_0001 B-U4: OpenAI profile dialog behavior + fold..."
              << std::endl;
    const int failures_before = g_failed_count;
    namespace eb = emebalachat;

    // ---- (A1) the combo label plan ---------------------------------------
    {
        const std::string kTemplate = "{h} / {m}";
        const std::string kFallback = "Profile";
        const std::vector<OpenAiProfile> library{
            MakeProfile("openai / gpt-4o-mini", "https://api.openai.com", "gpt-4o-mini"),
            MakeProfile("local llama", "http://localhost:8000", "llama-3"),
        };
        const std::vector<std::string> labels =
            eb::PlanOpenAiProfileComboLabels(library, kTemplate, kFallback);
        TEST_CHECK(labels.size() == library.size(),
                   "B-U4 combo: one label per library entry (index identity)");
        TEST_CHECK(labels.size() == 2 && labels[0] == "openai / gpt-4o-mini" &&
                       labels[1] == "local llama",
                   "B-U4 combo: a stored name is shown VERBATIM (library order, never "
                   "re-derived from the endpoint)");

        // A nameless entry (hand-edited / older file) still gets a usable row:
        // the name the save path would derive, from the endpoint.
        const std::vector<OpenAiProfile> nameless{
            MakeProfile("", "https://api.openai.com", "gpt-4o-mini")};
        const std::vector<std::string> derived =
            eb::PlanOpenAiProfileComboLabels(nameless, kTemplate, kFallback);
        TEST_CHECK(derived.size() == 1 && derived[0] == "openai / gpt-4o-mini",
                   "B-U4 combo: a nameless entry falls back to the derived name");

        // ... and an entry with neither name nor endpoint falls back to the
        // localized Profile label (a closed combo never shows a blank row).
        const std::vector<OpenAiProfile> blank{MakeProfile("", "", "")};
        const std::vector<std::string> fallback =
            eb::PlanOpenAiProfileComboLabels(blank, kTemplate, kFallback);
        TEST_CHECK(fallback.size() == 1 && fallback[0] == kFallback,
                   "B-U4 combo: a fully empty entry shows the Profile fallback label");
        TEST_CHECK(eb::PlanOpenAiProfileComboLabels({}, kTemplate, kFallback).empty(),
                   "B-U4 combo: an empty library yields no labels");

        // Ordering is the library's, not sorted: the picker index must stay the
        // library index (that is what the select/save/delete paths address).
        const std::vector<OpenAiProfile> unsorted{
            MakeProfile("zeta", "https://zeta.example", "z"),
            MakeProfile("alpha", "https://alpha.example", "a"),
        };
        const std::vector<std::string> unsorted_labels =
            eb::PlanOpenAiProfileComboLabels(unsorted, kTemplate, kFallback);
        TEST_CHECK(unsorted_labels.size() == 2 && unsorted_labels[0] == "zeta" &&
                       unsorted_labels[1] == "alpha",
                   "B-U4 combo: labels keep LIBRARY order (never sorted)");
    }

    // ---- (A2) display sanitize before derivation (R2-M/G-02 precedent) ----
    {
        // The bidi override / isolate marks and the control characters that
        // must never reach a combo row or a derived name.
        const std::string kRlo = "\xE2\x80\xAE";  // U+202E RIGHT-TO-LEFT OVERRIDE
        const std::string kLri = "\xE2\x81\xA6";  // U+2066 LEFT-TO-RIGHT ISOLATE
        TEST_CHECK(eb::SanitizeOpenAiProfileDisplayText("a" + kRlo + "b") == "ab",
                   "B-U4 sanitize: U+202E is dropped whole (never reordered)");
        TEST_CHECK(eb::SanitizeOpenAiProfileDisplayText("a" + kLri + "b") == "ab",
                   "B-U4 sanitize: U+2066 is dropped whole");
        // Invisible FORMAT characters are dropped whole; control characters (C0)
        // collapse to ONE space — that is the R2-M/G-02 pass-1 contract this
        // helper reuses (a name must stay a single readable line, never carry
        // the control itself).
        TEST_CHECK(eb::SanitizeOpenAiProfileDisplayText("a\x01\x02" "b") == "a b",
                   "B-U4 sanitize: a C0 control run collapses to one space");
        TEST_CHECK(eb::SanitizeOpenAiProfileDisplayText("a\nb") == "a b",
                   "B-U4 sanitize: a newline can never reach a name/row");
        // NO key masking happens on this path: with no key argument the mask
        // pass is skipped by construction, so a key-shaped name is untouched.
        TEST_CHECK(eb::SanitizeOpenAiProfileDisplayText("sk-abcdefghijklmnop") ==
                       "sk-abcdefghijklmnop",
                   "B-U4 sanitize: the display path never masks (no key is involved)");
        TEST_CHECK(eb::SanitizeOpenAiProfileDisplayText("openai / gpt-4o") ==
                       "openai / gpt-4o",
                   "B-U4 sanitize: ordinary text passes through unchanged");

        // The sanitize runs BEFORE the name derivation, so a bidi mark in the
        // endpoint or the model cannot land in the stored name.
        const std::vector<OpenAiProfile> dirty{
            MakeProfile("", "https://api.open" + kRlo + "ai.com", "gpt" + kLri + "-4o")};
        const std::vector<std::string> labels =
            eb::PlanOpenAiProfileComboLabels(dirty, "{h} / {m}", "Profile");
        TEST_CHECK(labels.size() == 1 && labels[0] == "openai / gpt-4o",
                   "B-U4 sanitize: a derived label carries neither bidi mark");
        // No raw DPAPI digest / key stand-in is ever shown; the label is text.
        TEST_CHECK(labels.size() == 1 && labels[0].find("QUJD") == std::string::npos,
                   "B-U4 combo: the key material is never part of a label");
    }

    // ---- (A3) the save plan: upsert keeps the name -----------------------
    {
        OpenAiConfig candidate;
        candidate.base_url = "HTTPS://API.OPENAI.COM"; // case-different, same identity
        candidate.model = "gpt-4o-mini";              // model compare is case-SENSITIVE
        candidate.reasoning_effort = "high";
        candidate.api_key_dpapi = "bmV3LWJsb2I=";
        candidate.api_key_sha256 = std::string(64, 'b');
        candidate.http_consent_given = true;

        const std::vector<OpenAiProfile> library{
            MakeProfile("openai / gpt-4o-mini", "https://api.openai.com", "gpt-4o-mini"),
            MakeProfile("local llama", "http://localhost:8000", "llama-3"),
        };
        std::vector<OpenAiProfile> saved;
        std::size_t index = 999;
        TEST_CHECK(eb::PlanOpenAiProfileSave(library, candidate, "{h} / {m}", saved, index),
                   "B-U4 save: a matching identity is applied (never refused)");
        TEST_CHECK(saved.size() == library.size(),
                   "B-U4 save: an upsert does not grow the library (no combo growth)");
        TEST_CHECK(index == 0 && saved[0].name == "openai / gpt-4o-mini",
                   "B-U4 save: the matched entry KEEPS its name (identity by "
                   "base_url ASCII-case-insensitively + model exactly)");
        TEST_CHECK(saved[0].cfg.base_url == candidate.base_url &&
                       saved[0].cfg.model == candidate.model &&
                       saved[0].cfg.reasoning_effort == "high" &&
                       saved[0].cfg.api_key_dpapi == candidate.api_key_dpapi &&
                       saved[0].cfg.api_key_sha256 == candidate.api_key_sha256 &&
                       saved[0].cfg.http_consent_given,
                   "B-U4 save: the matched entry's cfg is replaced WHOLESALE (every "
                   "field, incl. the pair + consent)");
        TEST_CHECK(saved[1].name == library[1].name &&
                       saved[1].cfg.base_url == library[1].cfg.base_url,
                   "B-U4 save: the other entries are untouched");

        // A model-case difference is a DIFFERENT model: append, not upsert.
        OpenAiConfig other = candidate;
        other.model = "GPT-4O-MINI";
        std::vector<OpenAiProfile> appended;
        std::size_t appended_index = 0;
        TEST_CHECK(eb::PlanOpenAiProfileSave(library, other, "{h} / {m}", appended,
                                             appended_index),
                   "B-U4 save: a different model id is applied as an append");
        TEST_CHECK(appended.size() == library.size() + 1 &&
                       appended_index == library.size(),
                   "B-U4 save: the append lands last and reports its own index");
    }

    // ---- (A4) the save plan: the derived, de-duplicated name -------------
    {
        const std::vector<OpenAiProfile> library{
            MakeProfile("openai / gpt-4o", "https://api.openai.com", "gpt-4o")};
        OpenAiConfig candidate;
        candidate.base_url = "https://api.openai.com";
        candidate.model = "gpt-4o-mini";
        candidate.reasoning_effort = "";
        std::vector<OpenAiProfile> saved;
        std::size_t index = 0;
        TEST_CHECK(eb::PlanOpenAiProfileSave(library, candidate, "{h} / {m}", saved, index),
                   "B-U4 name: a new identity is appended");
        TEST_CHECK(index == 1 && saved.size() == 2,
                   "B-U4 name: the appended entry is the library's last one");
        // The template substitutes the host ABBREVIATION (second-to-last label)
        // and the model — the design-pick §7 derivation, template-driven so the
        // saved name follows the UI locale once.
        TEST_CHECK(saved.size() == 2 && saved[1].name == "openai / gpt-4o-mini",
                   "B-U4 name: the display name is derived from host abbrev + model via "
                   "the localized template");
        TEST_CHECK(saved.size() == 2 && saved[1].cfg.base_url == candidate.base_url &&
                       saved[1].cfg.model == candidate.model,
                   "B-U4 name: the stored cfg keeps the RAW endpoint (only the name is "
                   "derived from sanitized copies)");

        // A template from another locale (Korean style, the real StringId form)
        // is honored verbatim.
        std::vector<OpenAiProfile> ko_saved;
        std::size_t ko_index = 0;
        TEST_CHECK(eb::PlanOpenAiProfileSave(library, candidate, "{h}의 {m} 모델", ko_saved,
                                             ko_index),
                   "B-U4 name: the save applies with a non-ASCII template");
        TEST_CHECK(ko_saved.size() == 2 && ko_saved[1].name == "openai의 gpt-4o-mini 모델",
                   "B-U4 name: the localized template's own connector is used");

        // Name collision: the SAME derived base on a different identity gets
        // the " (2)" de-duplication (two rows must never read identically).
        const std::vector<OpenAiProfile> collided{
            MakeProfile("openai / gpt-4o-mini", "https://api.openai.com", "gpt-4o")};
        std::vector<OpenAiProfile> unique_saved;
        std::size_t unique_index = 0;
        TEST_CHECK(eb::PlanOpenAiProfileSave(collided, candidate, "{h} / {m}",
                                             unique_saved, unique_index),
                   "B-U4 name: a colliding name does not refuse the save");
        TEST_CHECK(unique_saved.size() == 2 && unique_saved[1].name == "openai / gpt-4o-mini (2)",
                   "B-U4 name: a name collision resolves through \" (2)\"");

        // The derived name is bidi-free even when the endpoint carries a mark.
        OpenAiConfig dirty = candidate;
        dirty.model = "gpt-4o\xE2\x80\xAE-mini";
        std::vector<OpenAiProfile> dirty_saved;
        std::size_t dirty_index = 0;
        TEST_CHECK(eb::PlanOpenAiProfileSave(library, dirty, "{h} / {m}", dirty_saved,
                                             dirty_index),
                   "B-U4 name: a bidi-marked model still saves");
        TEST_CHECK(dirty_saved.size() == 2 &&
                       dirty_saved[1].name.find("\xE2\x80\xAE") == std::string::npos &&
                       dirty_saved[1].name == "openai / gpt-4o-mini",
                   "B-U4 name: the derived name is sanitized (strip before derivation)");
    }

    // ---- (A5) the cap: refuse the append, leave the library untouched ----
    {
        std::vector<OpenAiProfile> full;
        for (std::size_t i = 0; i < kMaxOpenAiProfiles; ++i) {
            full.push_back(MakeProfile("profile " + std::to_string(i),
                                       "https://host" + std::to_string(i) + ".example",
                                       "model-" + std::to_string(i)));
        }
        TEST_CHECK(full.size() == 32 && kMaxOpenAiProfiles == 32,
                   "B-U4 cap: the pinned cap is 32 profiles");

        OpenAiConfig append_candidate;
        append_candidate.base_url = "https://new-host.example";
        append_candidate.model = "brand-new-model";
        std::vector<OpenAiProfile> refused;
        std::size_t refused_index = 999;
        const bool applied =
            eb::PlanOpenAiProfileSave(full, append_candidate, "{h} / {m}", refused,
                                      refused_index);
        TEST_CHECK(!applied,
                   "B-U4 cap: an append at the cap is REFUSED (the dialog then shows "
                   "the localized limit notice)");
        TEST_CHECK(refused.size() == full.size() && ProfileVectorsEqual(refused, full),
                   "B-U4 cap: the refused save leaves the library BYTE-IDENTICAL");

        // An UPSERT into the same full library is still allowed: the cap only
        // bounds the number of entries, not the right to edit one.
        OpenAiConfig upsert_candidate;
        upsert_candidate.base_url = "https://host7.example";
        upsert_candidate.model = "model-7";
        upsert_candidate.reasoning_effort = "low";
        std::vector<OpenAiProfile> upserted;
        std::size_t upserted_index = 999;
        TEST_CHECK(eb::PlanOpenAiProfileSave(full, upsert_candidate, "{h} / {m}", upserted,
                                             upserted_index),
                   "B-U4 cap: editing an existing entry at the cap still applies");
        TEST_CHECK(upserted.size() == full.size() && upserted_index == 7 &&
                       upserted[7].cfg.reasoning_effort == "low" &&
                       upserted[7].name == full[7].name,
                   "B-U4 cap: the in-place edit keeps the count, index and name");
    }

    // ---- (A6) the limit notice: the {n} substitution ---------------------
    {
        const std::wstring notice =
            eb::FormatOpenAiProfileLimitNotice(L"max {n} profiles", kMaxOpenAiProfiles);
        TEST_CHECK(notice == L"max 32 profiles",
                   "B-U4 notice: {n} is replaced with the cap number");
        TEST_CHECK(eb::FormatOpenAiProfileLimitNotice(L"a {n} b {n} c", 7) == L"a 7 b 7 c",
                   "B-U4 notice: every occurrence is substituted");
        TEST_CHECK(eb::FormatOpenAiProfileLimitNotice(L"no token here", 7) ==
                       L"no token here",
                   "B-U4 notice: a token-free message is returned unchanged");
        TEST_CHECK(eb::FormatOpenAiProfileLimitNotice(L"{n}", 0).empty() == false &&
                       eb::FormatOpenAiProfileLimitNotice(L"{n}", 0) == L"0",
                   "B-U4 notice: the substitution is numeric, not padded");

        // The REAL localized notice carries the token, so the production call
        // site (I18n::Get(StringId::OpenAiProfileLimit) + the cap) renders a
        // number instead of a literal "{n}".
        const UiLocale initial = I18n::GetCurrentLocale();
        I18n::SetLocale(UiLocale::English);
        const std::wstring en = I18n::Get(StringId::OpenAiProfileLimit);
        I18n::SetLocale(initial); // suite-state hygiene
        TEST_CHECK(en.find(L"{n}") != std::wstring::npos,
                   "B-U4 notice: the en OpenAiProfileLimit string carries the {n} token");
        TEST_CHECK(eb::FormatOpenAiProfileLimitNotice(en, kMaxOpenAiProfiles)
                           .find(L"{n}") == std::wstring::npos &&
                       eb::FormatOpenAiProfileLimitNotice(en, kMaxOpenAiProfiles)
                               .find(L"32") != std::wstring::npos,
                   "B-U4 notice: the en notice renders the cap (no literal token left)");
    }

    // ---- (A7) the key-pair matrix (real DPAPI, real digest) --------------
    {
        // The masked-placeholder predicate itself (shared by IDOK, the fetch
        // probe and the profile save). 261122_0001 fix-D (R1-14): EXACT
        // compare against the RENDERED mask — the old suffix heuristic kept
        // the old pair for ANY typed key ending in "***".
        TEST_CHECK(eb::IsMaskedPlaceholder("abcdef***", "abcdef***"),
                   "B-U4 key: the exact rendered mask text is recognized");
        TEST_CHECK(!eb::IsMaskedPlaceholder("abcdef***", "sk-oth***") &&
                       !eb::IsMaskedPlaceholder("abcdef***", "") &&
                       !eb::IsMaskedPlaceholder("sk-live-abc***", "abcdef***") &&
                       !eb::IsMaskedPlaceholder("", "abcdef***"),
                   "B-U4 key: any other text — a different mask, an empty rendered "
                   "mask, a typed key that merely ends in ***, an empty edit — is "
                   "NOT the placeholder");

        const std::string kPlaintext = "sk-live-0123456789abcdef";
        std::string expected_digest;
        TEST_CHECK(OpenAiSha256Hex(kPlaintext, expected_digest),
                   "B-U4 key: the reference digest is computable");
        OpenAiConfig existing;
        existing.base_url = "https://api.openai.com";
        std::string existing_edit = kPlaintext;
        TEST_CHECK(ProtectOpenAiApiKey(existing_edit, existing.api_key_dpapi),
                   "B-U4 key: the reference pair is protectable (DPAPI available)");
        OpenAiSha256Hex(kPlaintext, existing.api_key_sha256);
        TEST_CHECK(eb::OpenAiKeyPairIntegrityOk(existing),
                   "B-U4 key: the reference pair passes the REQ-050 integrity check");
        TEST_CHECK(eb::RenderedOpenAiKeyPlaceholder(existing) ==
                       MaskOpenAiApiKey(kPlaintext),
                   "B-U4 key: the rendered placeholder reproduces the dialog's seeded "
                   "mask text exactly (the exact-compare ruler)");

        // (i) masked placeholder -> the pair is KEPT verbatim.
        {
            OpenAiConfig out;
            std::string edit = MaskOpenAiApiKey(kPlaintext);
            TEST_CHECK(eb::ResolveOpenAiProfileKeyPair(edit, existing, out),
                       "B-U4 key[i]: a masked placeholder resolves without error");
            TEST_CHECK(out.api_key_dpapi == existing.api_key_dpapi &&
                           out.api_key_sha256 == existing.api_key_sha256,
                       "B-U4 key[i]: the masked placeholder KEEPS the existing pair");
            TEST_CHECK(edit == MaskOpenAiApiKey(kPlaintext),
                       "B-U4 key[i]: the placeholder (display text, not a secret) is "
                       "left as it is");
            TEST_CHECK(out.api_key_dpapi != kPlaintext &&
                           out.api_key_dpapi.find(kPlaintext) == std::string::npos,
                       "B-U4 key[i]: the stored blob is never the cleartext");
        }

        // (ii) a typed key -> digest BEFORE protect, cleartext scrubbed.
        {
            OpenAiConfig out;
            std::string edit = kPlaintext;
            TEST_CHECK(eb::ResolveOpenAiProfileKeyPair(edit, existing, out),
                       "B-U4 key[ii]: a typed key resolves without error");
            TEST_CHECK(out.api_key_sha256 == expected_digest,
                       "B-U4 key[ii]: the digest is SHA-256 of the TYPED key "
                       "(digest-before-protect)");
            TEST_CHECK(!out.api_key_dpapi.empty() &&
                           out.api_key_dpapi.find(kPlaintext) == std::string::npos &&
                           out.api_key_sha256.find(kPlaintext) == std::string::npos,
                       "B-U4 key[ii]: the pair carries no plaintext (opaque blob + "
                       "digest only)");
            TEST_CHECK(eb::OpenAiKeyPairIntegrityOk(out),
                       "B-U4 key[ii]: the produced pair verifies (the key is usable, "
                       "not refused forever)");
            bool scrubbed = true;
            for (const char c : edit) {
                if (c != '\0') scrubbed = false;
            }
            TEST_CHECK(scrubbed,
                       "B-U4 key[ii]: the edit buffer is SecureZeroMemory'd (no "
                       "plaintext survives in reachable state)");
        }

        // (iii) an empty edit -> no pair (a cleared key clears the profile's).
        {
            OpenAiConfig out = existing;
            std::string edit;
            TEST_CHECK(eb::ResolveOpenAiProfileKeyPair(edit, existing, out),
                       "B-U4 key[iii]: an empty edit resolves without error");
            TEST_CHECK(out.api_key_dpapi.empty() && out.api_key_sha256.empty(),
                       "B-U4 key[iii]: an empty edit stores NO pair (it never "
                       "resurrects the previous profile's key)");
        }

        // (iv) mask-shaped text with NO pair behind it -> typed input
        // (fix-D R1-14): with an empty `existing` the dialog rendered no
        // placeholder, so nothing can compare equal to one — the edit is
        // digested as a key either way.
        {
            OpenAiConfig none;
            OpenAiConfig out;
            std::string edit = "***";
            TEST_CHECK(!eb::IsMaskedPlaceholder(
                           edit, eb::RenderedOpenAiKeyPlaceholder(none)),
                       "B-U4 key[iv]: with no pair there is no rendered mask to equal");
            TEST_CHECK(eb::ResolveOpenAiProfileKeyPair(edit, none, out),
                       "B-U4 key[iv]: a 3-character mask is digested as typed input");
            TEST_CHECK(!out.api_key_dpapi.empty(),
                       "B-U4 key[iv]: ... so the edit is treated as a typed key");
        }
        {
            OpenAiConfig none;
            OpenAiConfig out;
            std::string edit = "abcdef***";
            std::string literal_digest;
            OpenAiSha256Hex(edit, literal_digest);
            TEST_CHECK(eb::ResolveOpenAiProfileKeyPair(edit, none, out),
                       "B-U4 key[iv]: mask-shaped text with an EMPTY existing pair "
                       "resolves as typed input");
            TEST_CHECK(!out.api_key_dpapi.empty() &&
                           out.api_key_sha256 == literal_digest,
                       "B-U4 key[iv]: ... and stores the LITERAL text's pair (nothing "
                       "to keep — there is no rendered mask without a pair)");
        }

        // No plaintext anywhere in the library the dialog would stage.
        {
            std::string edit = kPlaintext;
            OpenAiConfig candidate;
            candidate.base_url = "https://api.openai.com";
            candidate.model = "gpt-4o-mini";
            TEST_CHECK(eb::ResolveOpenAiProfileKeyPair(edit, existing, candidate) &&
                           candidate.api_key_sha256 == expected_digest,
                       "B-U4 key: the save-path candidate carries the protected pair");
            std::vector<OpenAiProfile> out_library;
            std::size_t index = 0;
            TEST_CHECK(eb::PlanOpenAiProfileSave({}, candidate, "{h} / {m}", out_library,
                                                 index),
                       "B-U4 key: the candidate saves into an empty library");
            bool leak = false;
            for (const OpenAiProfile& profile : out_library) {
                if (profile.name.find(kPlaintext) != std::string::npos ||
                    profile.cfg.base_url.find(kPlaintext) != std::string::npos ||
                    profile.cfg.model.find(kPlaintext) != std::string::npos ||
                    profile.cfg.api_key_dpapi.find(kPlaintext) != std::string::npos ||
                    profile.cfg.api_key_sha256.find(kPlaintext) != std::string::npos ||
                    profile.cfg.reasoning_effort.find(kPlaintext) != std::string::npos) {
                    leak = true;
                }
            }
            TEST_CHECK(!leak,
                       "B-U4 key: no string field of the staged library contains the "
                       "cleartext key");
        }
    }

    // ---- (A8) the reasoning re-seed rule (risk R3) -----------------------
    {
        std::string value = "stale";
        bool pending = false;
        eb::PlanOpenAiReasoningReseed("xhigh", value, pending);
        TEST_CHECK(value == "xhigh" && pending,
                   "R3: a selected profile's non-empty effort is carried as PENDING "
                   "(the F-4 convention, so a server extension survives)");
        eb::PlanOpenAiReasoningReseed("", value, pending);
        TEST_CHECK(value.empty() && !pending,
                   "R3: an empty saved effort re-seeds as the server default, not pending");

        // The end-to-end consequence, through the REAL combo planner: a
        // standard value is shown immediately; a server-extension value stays
        // pending until a catalog advertises it, and survives to the save path.
        const std::vector<std::string> fallback = PlanReasoningEffortItems({});
        int select = -1;
        std::string effective;
        bool still_pending = false;
        eb::PlanOpenAiReasoningReseed("high", value, pending);
        PlanOpenAiReasoningComboPopulate(fallback, value, pending, select, effective,
                                        still_pending);
        TEST_CHECK(select > 0 && effective == "high" && !still_pending,
                   "R3: a standard effort is selected right away and leaves pending");

        eb::PlanOpenAiReasoningReseed("xhigh", value, pending);
        PlanOpenAiReasoningComboPopulate(fallback, value, pending, select, effective,
                                        still_pending);
        TEST_CHECK(select == 0 && effective == "xhigh" && still_pending,
                   "R3: a server-extension effort stays pending against the fallback "
                   "list (never silently lost by a profile pick)");
    }

    // ---- (A9) the B-U6fix opened-list width plan (design item 4b) ------
    // The closed profile picker truncates by OS design (see the contract note
    // in BuildOpenAiTemplate); the OPENED list must always carry the full name.
    // CB_SETDROPPEDWIDTH takes PIXELS, and the planning is pure: the suite
    // injects a deterministic measurer (5 px per UTF-16 code unit) so every rule
    // is exact — no GDI, no window, no font.
    {
        const auto measure = [](std::wstring_view text) {
            return static_cast<int>(text.size()) * 5;
        };
        constexpr int kClosedPx = 357; // the shipped 204-DLU closed box at 96 DPI

        // (i) empty library => the closed box width (the OS default stands).
        TEST_CHECK(eb::PlanOpenAiProfileDroppedWidth({}, kClosedPx, measure) == kClosedPx,
                   "B-U6fix plan (i): an empty library leaves the OS default list width "
                   "(the closed box)");

        // (ii) a name that already fits changes nothing: the list is never
        // NARROWER than the box it drops from.
        {
            const std::vector<std::string> labels = {"openai / gpt-4o-mini"};
            const int widest_px = measure(ToUtf16(labels[0]));
            TEST_CHECK(widest_px < kClosedPx,
                       "B-U6fix plan (ii): the fixture name is narrower than the box");
            TEST_CHECK(eb::PlanOpenAiProfileDroppedWidth(labels, kClosedPx, measure) ==
                           kClosedPx,
                       "B-U6fix plan (ii): a name shorter than the closed box keeps the "
                       "closed width (never a narrower list)");
        }

        // (iii) the longest entry + the 8-px margin, taken over the WHOLE
        // library (a short first entry must not mask a long second one). The
        // fixture is deliberately wider than the closed box, so the floor rule
        // cannot mask the margin rule.
        {
            const std::vector<std::string> labels = {
                "a", std::string(80, '0'), "0123456789"};
            const int widest_px = measure(ToUtf16(labels[1]));
            TEST_CHECK(widest_px > kClosedPx && widest_px + 8 < 640,
                       "B-U6fix plan (iii): the fixture is wider than the box and under "
                       "the 640-px cap");
            TEST_CHECK(eb::PlanOpenAiProfileDroppedWidth(labels, kClosedPx, measure) ==
                           widest_px + 8,
                       "B-U6fix plan (iii): the opened list is the longest entry + 8 px");
        }

        // (iv) the 96-byte pathological name (the OPENED-list target): 96 'x' at
        // 5 px each = 480 px + 8 = 488, still inside the 640-px cap and far
        // beyond the closed box — the exact contract the U6 probe re-checks.
        {
            const std::vector<std::string> labels(1, std::string(kMaxOpenAiProfileNameBytes,
                                                                 'x'));
            TEST_CHECK(labels[0].size() == 96,
                       "B-U6fix plan (iv): the pathological fixture is the 96-byte cap");
            TEST_CHECK(eb::PlanOpenAiProfileDroppedWidth(labels, kClosedPx, measure) ==
                           480 + 8,
                       "B-U6fix plan (iv): the 96-byte name gets a 488-px opened list "
                       "(the audited closed-truncation contract)");
            TEST_CHECK(488 > kClosedPx,
                       "B-U6fix plan (iv): the opened list is WIDER than the closed box "
                       "(the truncation is never information loss)");
        }

        // (v) the cap: min(3 x closed width, 640 px). A 1000-px name is clamped
        // to 640 on the shipped box; a narrow box clamps to 3x itself.
        {
            const std::vector<std::string> huge(1, std::string(200, 'x')); // 1000 px
            TEST_CHECK(eb::PlanOpenAiProfileDroppedWidth(huge, kClosedPx, measure) == 640,
                       "B-U6fix plan (v): the list is capped at 640 px on the shipped box "
                       "(3 x 357 exceeds it)");
            TEST_CHECK(eb::PlanOpenAiProfileDroppedWidth(huge, 100, measure) == 300,
                       "B-U6fix plan (v): a narrow box caps the list at 3x its own width");
        }

        // (vi) the floor always wins: a short name on a WIDE box keeps the box.
        {
            const std::vector<std::string> labels = {"a"};
            TEST_CHECK(eb::PlanOpenAiProfileDroppedWidth(labels, 700, measure) == 700,
                       "B-U6fix plan (vi): the closed box is the floor (a name narrower "
                       "than the box never shrinks the list)");
        }

        // (vii) a measure failure falls back to the floor instead of guessing.
        {
            const std::vector<std::string> labels = {"openai / gpt-4o-mini"};
            TEST_CHECK(eb::PlanOpenAiProfileDroppedWidth(
                           labels, kClosedPx, [](std::wstring_view) { return 0; }) ==
                           kClosedPx,
                       "B-U6fix plan (vii): a zero extent falls back to the closed width");
            TEST_CHECK(eb::PlanOpenAiProfileDroppedWidth(labels, kClosedPx,
                                                         nullptr) == kClosedPx,
                       "B-U6fix plan (vii): an absent measurer falls back to the closed "
                       "width");
        }

        // (viii) the plan measures UTF-16 UNIT COUNTS, not UTF-8 bytes: a CJK
        // name (3 code points, 9 UTF-8 bytes) must reach the measurer as 3 units
        // — measuring the UTF-8 length would over-ask by 3x.
        {
            const std::vector<std::string> labels = {"\xED\x94\x84\xEB\xA1\x9C\xED\x95\x84"};
            TEST_CHECK(labels[0].size() == 9,
                       "B-U6fix plan (viii): the fixture name is 9 UTF-8 bytes");
            TEST_CHECK(eb::PlanOpenAiProfileDroppedWidth(labels, 10, measure) == 3 * 5 + 8,
                       "B-U6fix plan (viii): the measurer receives UTF-16 code units "
                       "(9 bytes -> 3 units -> 15 px + 8)");
        }
    }

    // ---- (A10) the B-U6fix dialog wiring (source-structure) -------------
    // The plan must be APPLIED on every combo rebuild, not just at init: a
    // [저장]/[삭제] can change the longest name, and the list width is planned
    // from the names just added. The probe's live pass covers the runtime effect;
    // this pin keeps the call site from drifting out of the refill helper.
    {
        std::string osw_src;
        const char* candidates[] = {"src/ui/openai_settings_window.cpp",
                                    "../src/ui/openai_settings_window.cpp",
                                    "../../src/ui/openai_settings_window.cpp"};
        for (const char* cand : candidates) {
            std::ifstream in(cand, std::ios::binary);
            if (!in) continue;
            osw_src.assign((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
            break;
        }
        TEST_CHECK(!osw_src.empty(),
                   "B-U6fix wiring: src/ui/openai_settings_window.cpp is resolvable");
        if (!osw_src.empty()) {
            const auto count_of = [](const std::string& hay, const std::string& needle) {
                std::size_t n = 0;
                for (std::size_t p = hay.find(needle); p != std::string::npos;
                     p = hay.find(needle, p + needle.size())) {
                    ++n;
                }
                return n;
            };
            TEST_CHECK(count_of(osw_src, "PlanOpenAiProfileDroppedWidth(") == 2,
                       "B-U6fix wiring: the plan is defined and called exactly once "
                       "(definition + the CB_SETDROPPEDWIDTH site)");
            TEST_CHECK(count_of(osw_src, "::SendMessageW(combo, CB_SETDROPPEDWIDTH") == 1,
                       "B-U6fix wiring: exactly one CB_SETDROPPEDWIDTH send exists (the "
                       "comments that name the message do not count)");
            const std::size_t refill_at = osw_src.find("void RefillOpenAiProfileCombo(");
            TEST_CHECK(refill_at != std::string::npos,
                       "B-U6fix wiring: the profile combo refill helper is locatable");
            if (refill_at != std::string::npos) {
                // The next definition after the refill helper bounds its body
                // (CRLF-safe: no "\n}\n" scan needed).
                const std::size_t apply_sel_at =
                    osw_src.find("void ApplyOpenAiProfileSelection(", refill_at);
                const std::string body = osw_src.substr(
                    refill_at, apply_sel_at == std::string::npos ? std::string::npos
                                                                 : apply_sel_at - refill_at);
                const std::size_t reset_at = body.find("CB_RESETCONTENT");
                const std::size_t plan_at = body.find("ApplyOpenAiProfileDroppedWidth(");
                TEST_CHECK(reset_at != std::string::npos && plan_at != std::string::npos &&
                               reset_at < plan_at,
                           "B-U6fix wiring: the width is re-planned AFTER the rebuild "
                           "(CB_RESETCONTENT -> … -> the apply call)");
                TEST_CHECK(body.find("WM_GETFONT") == std::string::npos &&
                               osw_src.find("SendMessageW(combo, WM_GETFONT") !=
                                   std::string::npos,
                           "B-U6fix wiring: the measurer runs in the apply helper with the "
                           "combo's OWN font (WM_GETFONT), not the refill");
            }
        }
    }

    // ---- (B) the proc wiring / the fold contract (source-structure) ------
    std::string osw_src;
    {
        const char* candidates[] = {"src/ui/openai_settings_window.cpp",
                                    "../src/ui/openai_settings_window.cpp",
                                    "../../src/ui/openai_settings_window.cpp"};
        for (const char* cand : candidates) {
            std::ifstream in(cand, std::ios::binary);
            if (!in) continue;
            osw_src.assign((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
            std::string norm;
            norm.reserve(osw_src.size());
            for (std::size_t i = 0; i < osw_src.size(); ++i) {
                if (osw_src[i] == '\r' && i + 1 < osw_src.size() && osw_src[i + 1] == '\n') {
                    continue;
                }
                norm.push_back(osw_src[i]);
            }
            osw_src.swap(norm);
            break;
        }
    }
    if (osw_src.empty()) {
        // 261122_0001 fix-D (R1-07): a source pin NEVER passes over zero
        // assertions — the A10 block below and the fix-A2 (C) block already
        // hard-fail this way; a mis-set CWD must turn the gate red, not print
        // a vacuous PASS (the old [SKIP] did exactly that).
        TEST_CHECK(false, "B-U4: src/ui/openai_settings_window.cpp must be resolvable "
                          "from the test CWD (R1-07: the proc-wiring pins never pass "
                          "over zero assertions)");
    } else {
        const std::size_t init_at = osw_src.find("case WM_INITDIALOG");
        const std::size_t command_at = osw_src.find("case WM_COMMAND");
        const std::size_t save_at = osw_src.find("case IDC_PROFILE_SAVE_BTN");
        const std::size_t del_at = osw_src.find("case IDC_PROFILE_DEL_BTN");
        const std::size_t ok_at = osw_src.find("case IDOK");
        const std::size_t cancel_at = osw_src.find("case IDCANCEL");
        const std::size_t settings_del_at = osw_src.find("case IDC_DELETE_BTN");

        // (B1) WM_INITDIALOG lists the library with NO selection.
        TEST_CHECK(init_at != std::string::npos && command_at != std::string::npos &&
                       init_at < command_at,
                   "B-U4 init: WM_INITDIALOG precedes WM_COMMAND");
        if (init_at != std::string::npos && command_at != std::string::npos &&
            init_at < command_at) {
            const std::string init_src = osw_src.substr(init_at, command_at - init_at);
            TEST_CHECK(init_src.find("RefillOpenAiProfileCombo(dlg, st, st->profiles.size());") !=
                           std::string::npos,
                       "B-U4 init: the picker is seeded from the library with an index "
                       "past the end (NO selection)");
        }

        // (B2) CBN_SELCHANGE drives the refill, and only for the profile combo.
        TEST_CHECK(osw_src.find("LOWORD(wp) == IDC_PROFILE_COMBO && HIWORD(wp) == "
                                "CBN_SELCHANGE") != std::string::npos &&
                       osw_src.find("ApplyOpenAiProfileSelection(dlg, st);") !=
                           std::string::npos,
                   "B-U4 select: CBN_SELCHANGE on IDC_PROFILE_COMBO calls the refill helper");
        {
            const std::size_t apply_at = osw_src.find("void ApplyOpenAiProfileSelection(");
            TEST_CHECK(apply_at != std::string::npos,
                       "B-U4 select: the refill helper is defined at TU scope");
            if (apply_at != std::string::npos) {
                const std::size_t apply_end = osw_src.find("\n}\n", apply_at);
                const std::string body = osw_src.substr(
                    apply_at, apply_end == std::string::npos ? std::string::npos
                                                            : apply_end - apply_at);
                TEST_CHECK(body.find("OpenAiKeyPairIntegrityOk(profile.cfg)") !=
                                   std::string::npos &&
                               body.find("UnprotectOpenAiApiKey(") != std::string::npos,
                           "B-U4 select: the key pair is adopted ONLY through the REQ-050 "
                           "integrity check");
                TEST_CHECK(body.find("st->cfg->api_key_dpapi.clear();") != std::string::npos &&
                               body.find("st->cfg->api_key_sha256.clear();") != std::string::npos,
                           "B-U4 select: an unusable/absent pair is NOT adopted (both "
                           "halves cleared, the placeholder never lingers)");
                TEST_CHECK(body.find("SetCtrlText(dlg, IDC_BASE_URL") != std::string::npos &&
                               body.find("SetCtrlText(dlg, IDC_MODEL_COMBO") != std::string::npos &&
                               body.find("MaskOpenAiApiKey(") != std::string::npos,
                           "B-U4 select: base URL / model / masked key are refilled");
                TEST_CHECK(body.find("st->cfg->http_consent_given = profile.cfg."
                                     "http_consent_given;") != std::string::npos,
                           "B-U4 select: the profile's http consent is adopted (the gate "
                           "does not re-ask)");
                TEST_CHECK(body.find("PlanOpenAiReasoningReseed(profile.cfg.reasoning_effort") !=
                                   std::string::npos &&
                               body.find("RefreshReasoningCombo(dlg, st);") != std::string::npos,
                           "B-U4 select: the reasoning effort is re-seeded through the R3 "
                           "helper and the combo repopulated");
                TEST_CHECK(body.find("SecureZeroMemory(") != std::string::npos,
                           "B-U4 select: the unprotected cleartext is zeroed before return");
            }
        }

        // (B3) [저장]: the save path composition, no file I/O.
        TEST_CHECK(save_at != std::string::npos && del_at != std::string::npos &&
                       save_at < del_at,
                   "B-U4 save: the [저장] case precedes the [삭제] case");
        if (save_at != std::string::npos && del_at != std::string::npos &&
            save_at < del_at) {
            const std::string save_src = osw_src.substr(save_at, del_at - save_at);
            TEST_CHECK(save_src.find("ResolveOpenAiProfileKeyPair(keyUtf8, *st->cfg, "
                                     "candidate)") != std::string::npos,
                       "B-U4 save: the key matrix runs through the shared helper");
            // 261122_0001 fix-D (R1-13/c): the SAME ClassifyOpenAiBaseUrl guard
            // IDOK runs refuses junk endpoints on [저장] before any key work.
            TEST_CHECK(save_src.find("ClassifyOpenAiBaseUrl(candidate.base_url)") !=
                           std::string::npos &&
                       save_src.find("StringId::OpenAiInvalidBaseUrl") != std::string::npos,
                       "B-U4 save: a junk endpoint is refused with the IDOK failure "
                       "message before any key work (R1-13/c)");
            // 261122_0001 fix-D (R1-13/d): the DPAPI-protect failure caption is
            // the generic failure notice, never the model-fetch string.
            TEST_CHECK(save_src.find("StringId::OpenAiFetchFailed") == std::string::npos,
                       "B-U4 save: the DPAPI-protect failure does NOT reuse the fetch "
                       "failure caption (R1-13/d)");
            TEST_CHECK(save_src.find("PlanOpenAiProfileSave(") != std::string::npos &&
                           save_src.find("kMaxOpenAiProfiles") != std::string::npos &&
                           save_src.find("FormatOpenAiProfileLimitNotice(") != std::string::npos &&
                           save_src.find("StringId::OpenAiProfileLimit") != std::string::npos,
                       "B-U4 save: the plan runs and the cap refusal shows the localized "
                       "limit notice");
            TEST_CHECK(save_src.find("st->profiles_edited = true;") != std::string::npos &&
                           save_src.find("RefillOpenAiProfileCombo(dlg, st, saved_index);") !=
                               std::string::npos,
                       "B-U4 save: a successful save stages the library and keeps the "
                       "saved entry selected");
            TEST_CHECK(save_src.find("SaveToFile") == std::string::npos &&
                           save_src.find("AppConfig persisted") == std::string::npos,
                       "B-U4 save: the dialog performs NO config file I/O for profiles "
                       "(single-writer principle)");
            TEST_CHECK(save_src.find("MB_ICONWARNING") != std::string::npos,
                       "B-U4 save: a protect failure surfaces the existing failure notice");
            // The reasoning read is IDOK's (combo value, pending value wins).
            TEST_CHECK(save_src.find("ReasoningEffortSelection(dlg, st)") != std::string::npos &&
                           save_src.find("st->reasoning_effort_pending") != std::string::npos,
                       "B-U4 save: the staged reasoning effort follows the F-4 pending rule");
        }

        // (B4) [삭제]: selection gate, confirmation, erase, rebuild/clear.
        if (del_at != std::string::npos) {
            const std::size_t del_end = osw_src.find("\n        }\n        }\n", del_at);
            const std::string del_src = osw_src.substr(
                del_at, del_end == std::string::npos ? 3000 : del_end - del_at);
            TEST_CHECK(del_src.find("CB_GETCURSEL") != std::string::npos &&
                           del_src.find("st->profiles.size()") != std::string::npos,
                       "B-U4 delete: the branch reads the picker's current selection");
            TEST_CHECK(del_src.find("MB_YESNO | MB_ICONWARNING") != std::string::npos &&
                           del_src.find("StringId::OpenAiProfileDeleteConfirm") !=
                               std::string::npos,
                       "B-U4 delete: the deletion is confirmed through the localized "
                       "OpenAiProfileDeleteConfirm box");
            // No selection = no action: the guard returns before the confirmation
            // and before the erase (a stray CBN_SELCHANGE from a refill lands
            // here).
            TEST_CHECK(del_src.find("if (sel < 0 || static_cast<std::size_t>(sel) >= "
                                    "st->profiles.size())") != std::string::npos,
                       "B-U4 delete: an empty selection is a silent no-op (never 'the "
                       "first profile')");
            // Only a YES answer deletes: the `!= IDYES` early return precedes the
            // erase.
            {
                const std::size_t no_at = del_src.find("!= IDYES");
                const std::size_t erase_at = del_src.find("st->profiles.erase(");
                TEST_CHECK(no_at != std::string::npos && erase_at != std::string::npos &&
                               no_at < erase_at,
                           "B-U4 delete: a NO answer returns BEFORE the erase (only the "
                           "confirmed deletion mutates the library)");
            }
            TEST_CHECK(del_src.find("st->profiles.erase(") != std::string::npos &&
                           del_src.find("st->profiles_edited = true;") != std::string::npos,
                       "B-U4 delete: a YES answer removes the entry from the working copy "
                       "and stages the library");
            TEST_CHECK(del_src.find("RefillOpenAiProfileCombo(dlg, st, st->profiles.size());") !=
                           std::string::npos,
                       "B-U4 delete: the picker is rebuilt with the selection CLEARED");
            TEST_CHECK(del_src.find("SaveToFile") == std::string::npos,
                       "B-U4 delete: removing a profile never writes the config file");
        }

        // (B5) the fold contract in the dialog entry.
        // V10F-R1 F-17: the library-aware overload gained the trailing
        // consent-origin channel parameter (defaulted at every deprecated
        // wrapper), so the needle pins the full landed signature.
        TEST_CHECK(osw_src.find("OpenAiProfilesFold& profiles,\n"
                                "                                                 std::string* "
                                "http_consent_origin) {") != std::string::npos,
                   "B-U4 fold: the library-aware overload is defined (V10F-R1 F-17: with "
                   "the consent-origin channel)");
        TEST_CHECK(osw_src.find("st.profiles = profiles.profiles;") != std::string::npos,
                   "B-U4 fold: the working copy is seeded from the fold's INPUT half");
        {
            // The fold starts cleared so a caller that never reaches IDOK reads
            // "nothing to adopt", and the reset lands BEFORE the modal loop.
            const std::size_t entry_reset = osw_src.find("profiles.edited = false;");
            const std::size_t modal_at = osw_src.find("::DialogBoxIndirectParamW(");
            TEST_CHECK(entry_reset != std::string::npos && modal_at != std::string::npos &&
                           entry_reset < modal_at,
                       "B-U4 fold: the fold is reset to edited=false on entry, before the "
                       "modal loop (Cancel / failed creation / kDeleted report nothing to "
                       "adopt)");
        }
        TEST_CHECK(osw_src.find("profiles.edited = saved && st.profiles_edited;") !=
                       std::string::npos,
                   "B-U4 fold: the library rides out ONLY on the saved+IDOK path (Cancel "
                   "and a failed creation report edited=false)");
        TEST_CHECK(osw_src.find("profiles.profiles = std::move(st.profiles);") !=
                       std::string::npos,
                   "B-U4 fold: the adopted library is handed to the caller");
        {
            const std::size_t deleted_return =
                osw_src.find("if (st.deleted) return OpenAiSettingsOutcome::kDeleted;");
            const std::size_t fold_assign =
                osw_src.find("profiles.edited = saved && st.profiles_edited;");
            TEST_CHECK(deleted_return != std::string::npos && fold_assign != std::string::npos &&
                           deleted_return < fold_assign,
                       "B-U4 fold: the REQ-051 [설정 삭제] (kDeleted) return precedes the "
                       "profile fold, so that path can never carry the staged library");
        }
        TEST_CHECK(osw_src.find("return ShowOpenAiSettingsDialogEx(parent, cfg, declaration, "
                                "profiles, nullptr);") != std::string::npos,
                   "B-U4 fold: the declaration-only overload forwards to the library-aware "
                   "one (existing callers keep their contract; V10F-R1 F-17 passes no "
                   "consent-origin channel)");
        TEST_CHECK(osw_src.find("ShowOpenAiSettingsDialogEx(parent, cfg) == "
                                "OpenAiSettingsOutcome::kSaved") != std::string::npos,
                   "B-U4 fold: the bool wrapper still reports only kSaved");

        // (B6) REQ-051 semantics unchanged: IDOK writes the on-screen fields
        // only, and the [설정 삭제] branch stages no profile edit.
        if (ok_at != std::string::npos && cancel_at != std::string::npos &&
            ok_at < cancel_at) {
            const std::string ok_src = osw_src.substr(ok_at, cancel_at - ok_at);
            TEST_CHECK(ok_src.find("*st->cfg = std::move(out);") != std::string::npos,
                       "B-U4 IDOK: the active block output is still the on-screen fields");
            TEST_CHECK(ok_src.find("st->profiles") == std::string::npos,
                       "B-U4 IDOK: the IDOK branch never mutates the profile library "
                       "(staging is the buttons' job)");
        }
        if (settings_del_at != std::string::npos && save_at != std::string::npos &&
            settings_del_at < save_at) {
            const std::string settings_del_src =
                osw_src.substr(settings_del_at, save_at - settings_del_at);
            TEST_CHECK(settings_del_src.find("profiles_edited") == std::string::npos &&
                           settings_del_src.find("st->profiles") == std::string::npos,
                       "B-U4 [설정 삭제]: the REQ-051 delete-settings branch never touches "
                       "the staged profile library (its edits die with the dialog)");
            TEST_CHECK(settings_del_src.find("ClearOpenAiSettings(persisted.openai)") !=
                               std::string::npos &&
                           settings_del_src.find("::EndDialog(dlg, IDC_DELETE_BTN);") !=
                               std::string::npos,
                       "B-U4 [설정 삭제]: the immediate-persist + kDeleted semantics are "
                       "unchanged");
        }
        // The picker is refilled from the library exactly at the three intended
        // sites (open, save, delete) — no fourth, hidden rebuild.
        std::size_t refill_sites = 0;
        for (std::size_t p = osw_src.find("RefillOpenAiProfileCombo(dlg, st,");
             p != std::string::npos; p = osw_src.find("RefillOpenAiProfileCombo(dlg, st,", p + 1)) {
            ++refill_sites;
        }
        TEST_CHECK(refill_sites == 3,
                   "B-U4 wiring: the picker is rebuilt exactly at open / save / delete");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261122_0001 B-U4 OpenAI profile dialog behavior + fold passed."
                  << std::endl;
    } else {
        std::cout << "[FAIL] 261122_0001 B-U4 OpenAI profile dialog behavior + fold: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ===========================================================================
// 261122_0001 B-U5 (builder draw 9; design-pick items 4 + 10): the main.cpp
// CALLER wiring for the profile fold — the snapshot seed chain, the 4-arg
// library-aware dialog entry, the outcome branch matrix, and the single-writer
// persist contract.
//
// Boundary, stated honestly (the SEC-B5 note in main.cpp itself): main.cpp is
// the GUI entrypoint TU and is deliberately NOT linked into run_tests.exe, so
// the wiring is pinned at SOURCE-TEXT level — the established raw-read +
// CRLF-normalize idiom this file already uses for the dialog pins (the U4
// suite above). Layer note: the dialog-side fold semantics are pinned by the
// U4 suite; this suite pins only the caller half.
// ===========================================================================
void TestOpenAiProfileCallerWiring() {
    std::cout << "[TEST] 261122_0001 B-U5: OpenAI profile caller wiring (main.cpp)..."
              << std::endl;
    const int failures_before = g_failed_count;

    // ---- (0) main.cpp resolves (raw read + CRLF normalize) ---------------
    std::string main_src;
    {
        const char* candidates[] = {"src/main.cpp", "../src/main.cpp", "../../src/main.cpp"};
        for (const char* cand : candidates) {
            std::ifstream in(cand, std::ios::binary);
            if (!in) continue;
            std::string raw((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
            std::string norm;
            norm.reserve(raw.size());
            for (std::size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') {
                    continue;
                }
                norm.push_back(raw[i]);
            }
            main_src.swap(norm);
            break;
        }
    }
    const auto count_in = [](const std::string& hay, const std::string& needle) {
        std::size_t n = 0;
        for (std::size_t p = hay.find(needle); p != std::string::npos;
             p = hay.find(needle, p + needle.size())) {
            ++n;
        }
        return n;
    };

    if (main_src.empty()) {
        // 261122_0001 fix-D (R1-07): main.cpp is deliberately NOT linked into
        // run_tests.exe, so these source pins are its ONLY coverage — an
        // unresolvable source must FAIL, never degrade to a green PASS with
        // zero assertions (the old [SKIP]+[PASS] did exactly that).
        TEST_CHECK(false, "B-U5: src/main.cpp must be resolvable from the test CWD "
                          "(R1-07: the caller-wiring pins never pass over zero "
                          "assertions — they are the only main.cpp coverage)");
        return;
    }

    // ---- (a)+(d) the apply lambda: adoption BEFORE the single save --------
    {
        const std::size_t apply_at = main_src.find("g_apply_openai_settings = [&]");
        const std::size_t apply_end = main_src.find("g_apply_openai_deleted = [&]", apply_at);
        TEST_CHECK(apply_at != std::string::npos && apply_end != std::string::npos &&
                       apply_end > apply_at,
                   "B-U5 (a): the apply lambda is locatable");
        if (apply_at != std::string::npos && apply_end != std::string::npos &&
            apply_end > apply_at) {
            const std::string body = main_src.substr(apply_at, apply_end - apply_at);
            const std::size_t adopt_at =
                body.find("config.SetOpenAiProfiles(std::move(profiles->profiles));");
            const std::size_t save_at = body.find("config.SaveToFile();");
            TEST_CHECK(adopt_at != std::string::npos && save_at != std::string::npos &&
                           adopt_at < save_at,
                       "B-U5 (a): the staged library is adopted on the LIVE config BEFORE the "
                       "lambda's single save (P9c-1c single-writer: the emitted config.json "
                       "carries exactly the library the dialog staged) — and through the "
                       "locked verb since fix-A2 (R1-02 caller half)");
            TEST_CHECK(body.find("if (profiles && profiles->edited)") != std::string::npos,
                       "B-U5 (a): the adoption is gated on the fold pointer AND the edited "
                       "flag (a Cancel nullptr and an unedited fold both skip it)");
            TEST_CHECK(count_in(body, "config.SaveToFile();") == 1,
                       "B-U5 (d): the apply lambda holds exactly ONE persist site — a second "
                       "SaveToFile here would double-write the staged library");
            TEST_CHECK(count_in(body, "SetOpenAiProfiles(") == 1,
                       "B-U5 (d): the apply lambda names the library verb exactly once (the "
                       "adoption) — a second site would mean a second writer");
        }
    }

    // ---- (b)+(c)+(d) the deferred handler: seed, 4-arg call, branches -----
    const std::size_t handler_at = main_src.find("if (msg == kMsgOpenOpenAiSettings) {");
    TEST_CHECK(handler_at != std::string::npos,
               "B-U5 (b): the deferred kMsgOpenOpenAiSettings handler is locatable");
    if (handler_at != std::string::npos) {
        const std::size_t handler_end =
            main_src.find("if (msg == kMsgOpenGgufManager)", handler_at);
        const std::string handler =
            main_src.substr(handler_at, handler_end == std::string::npos
                                            ? std::string::npos
                                            : handler_end - handler_at);

        // (c) the dialog opens through the library-aware entry, fed by
        // the snapshot seed chain (tray copy -> handler move -> dialog copy).
        // V10F-R1 F-17: the entry gained the trailing consent-origin channel
        // (out-param), so the needle pins the full landed call shape.
        TEST_CHECK(handler.find("emebalachat::ShowOpenAiSettingsDialogEx(") !=
                       std::string::npos &&
                       handler.find("/*parent=*/emebalachat::g_hControllerWnd, edited, "
                                    "declaration, profiles_fold,\n"
                                    "                &consent_origin_scope);") !=
                           std::string::npos,
                   "B-U5 (c): the dialog is invoked with the library-aware overload "
                   "(parent, edited cfg, declaration fold, profile fold, consent-origin "
                   "channel)");
        TEST_CHECK(handler.find("profiles_fold.profiles = std::move(*emebalachat::"
                                "s_pending_openai_profiles);") != std::string::npos &&
                       handler.find("emebalachat::s_pending_openai_profiles.reset();") !=
                           std::string::npos,
                   "B-U5 (c): the handler seeds the fold input from the deferred snapshot "
                   "(a move — the fold owns its deep copy; the dialog then takes its own "
                   "working copy from the fold input)");
        TEST_CHECK(main_src.find("emebalachat::s_pending_openai_profiles = "
                                 "config.openai_profiles;") != std::string::npos,
                   "B-U5 (c): the tray dispatch snapshots the library next to the openai "
                   "block (a copy — the AppConfig vector never aliases the fold)");
        TEST_CHECK(count_in(main_src, "s_pending_openai_profiles.reset();") == 3,
                   "B-U5 (c): the snapshot slot is dropped at ALL THREE consumption sites "
                   "(the handler + the PostMessage-failure rollback in "
                   "open_openai_settings_deferred + the REQ-047 D3-mandated inline copy in "
                   "the engine_idx==2 no-profile branch), never left armed");

        // (b) kCancelled: nothing adopted, nothing persisted.
        const std::size_t cancel_at = handler.find("OpenAiSettingsOutcome::kCancelled");
        const std::size_t saved_at = handler.find("OpenAiSettingsOutcome::kSaved");
        TEST_CHECK(cancel_at != std::string::npos && saved_at != std::string::npos &&
                       cancel_at < saved_at,
                   "B-U5 (b): the Cancel branch precedes the Save branch");
        if (cancel_at != std::string::npos && saved_at != std::string::npos &&
            cancel_at < saved_at) {
            const std::string cancel_branch = handler.substr(cancel_at, saved_at - cancel_at);
            TEST_CHECK(cancel_branch.find("openai_profiles") == std::string::npos &&
                           cancel_branch.find("SaveToFile") == std::string::npos,
                       "B-U5 (b): kCancelled mutates neither the stored library nor the "
                       "file (the dialog reported the fold unedited, and this branch "
                       "passes nullptr for it)");
            TEST_CHECK(cancel_branch.find("g_apply_openai_settings(edited, /*saved=*/false, "
                                          "nullptr, nullptr);") != std::string::npos,
                       "B-U5 (b): kCancelled passes nullptr for the fold (double-guard)");
        }

        // (b) kDeleted: the fold is deliberately NOT applied (design item 4);
        // the credential-scoped prune travels through the seam (fix-A2 R1-01).
        const std::size_t del_at = handler.find("MAIN/on_select_engine/004");
        TEST_CHECK(del_at != std::string::npos,
                   "B-U5 (b): the kDeleted reconcile branch is locatable");
        if (del_at != std::string::npos) {
            const std::string deleted_branch = handler.substr(del_at);
            TEST_CHECK(deleted_branch.find("SaveToFile") == std::string::npos &&
                           deleted_branch.find("profiles_fold") == std::string::npos,
                       "B-U5 (b): kDeleted (REQ-051 [설정 삭제]) applies NO fold and "
                       "persists nothing itself — the stored profile library is preserved "
                       "as staged (design-pick item 4) and the seed copy is never adopted");
            TEST_CHECK(deleted_branch.find("g_apply_openai_deleted(edited);") !=
                           std::string::npos,
                       "B-U5 (b): kDeleted reconciles the cleared openai block through its "
                       "own seam (engine selection untouched)");
        }

        // (d) negative control: the handler itself never persists.
        TEST_CHECK(handler.find("SaveToFile") == std::string::npos,
                   "B-U5 (d): the WndProc handler contains NO SaveToFile — a persist "
                   "added here would be a second writer racing the apply lambda's save");
        TEST_CHECK(handler.find("config.openai_profiles") == std::string::npos,
                   "B-U5 (d): the WndProc handler never names the owning library member "
                   "directly — adoption travels only through the fold and the wWinMain "
                   "apply lambda (this handler cannot name the AppConfig)");
    }

    // ---- (d) whole-file negative control (fix-A2 R1-01/R1-02): NO direct
    // assignment to the library survives in main.cpp, and every write goes
    // through the locked verb — exactly two call sites: the apply-lambda
    // adoption (a [저장] fold) and the kDeleted credential prune. A third
    // would mean a new writer near this path.
    TEST_CHECK(count_in(main_src, "config.openai_profiles =") == 0,
               "fix-A2 (b): main.cpp holds NO direct config.openai_profiles assignment — "
               "the R1-02 write rule is enforced on the owning config (the member stays "
               "public only for the load path and startup seeding)");
    TEST_CHECK(count_in(main_src, "config.SetOpenAiProfiles(") == 2,
               "fix-A2 (b): main.cpp writes the library only through the locked verb, at "
               "exactly two sites — the apply-lambda adoption and the kDeleted prune");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261122_0001 B-U5 OpenAI profile caller wiring pins passed."
                  << std::endl;
    } else {
        std::cout << "[FAIL] 261122_0001 B-U5 OpenAI profile caller wiring pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ===========================================================================
// 261122_0001 fix-A2 (builder draw 12; fix-list-round1 row R1-01, CONFIRMED
// HIGH by the verifier trace in security-reviewer-report-verify.md): the
// [설정 삭제] credential RESURRECTION kill on BOTH persist sites, plus the
// R1-02 caller-side verb migration.
//
// Why the delete leaked: the delete path's own fresh LoadFromFile runs
// config.cpp's absent-key migration synthesis, so on a pre-B2 config the
// library it is about to save is a verbatim mirror of the block being cleared
// (full DPAPI pair). Clearing only the block left the pair in
// "openai_profiles", one profile-pick away from re-adoption (the pick alone
// re-arms the authenticated model fetch). main.cpp's kDeleted reconcile then
// saved the same library a second time, so either site alone left the
// credential behind.
//
//   (A) EraseOpenAiProfileMatchingKey identity matrix (pure): digest first
//       (stable across re-protects, unlike the DPAPI blob), the blob second,
//       the synthesizer's (base_url ASCII-case-insensitive, model
//       case-sensitive) identity for a KEYLESS deleted block — and nothing at
//       all for an identity-less block.
//   (B) round trip: a pre-B2 document through the DIALOG's own statement
//       sequence, then through the kDeleted sequence on an independent
//       instance — the persisted JSON carries neither the digest nor the blob,
//       and an unrelated profile survives both.
//   (C) source pins for both sites and the "no direct member assignment" rule
//       (R1-02 caller half).
//
// Zero network, zero UI.
// ===========================================================================
void TestOpenAiProfileFixA2() {
    std::cout << "[RUN] Testing 261122_0001 fix-A2 (R1-01 delete resurrection)..." << std::endl;
    const int failures_before = g_failed_count;

    const std::string kDigestA(64, 'a');
    const std::string kDigestB(64, 'b');
    const std::string kDigestC(64, 'c');

    // ---- (A) the identity matrix -----------------------------------------
    {
        auto make_entry = [](const std::string& name, const std::string& base_url,
                             const std::string& model, const std::string& dpapi,
                             const std::string& digest) {
            OpenAiProfile entry;
            entry.name = name;
            entry.cfg.base_url = base_url;
            entry.cfg.model = model;
            entry.cfg.api_key_dpapi = dpapi;
            entry.cfg.api_key_sha256 = digest;
            return entry;
        };
        auto make_deleted = [](const std::string& base_url, const std::string& model,
                               const std::string& dpapi, const std::string& digest) {
            OpenAiConfig cfg;
            cfg.base_url = base_url;
            cfg.model = model;
            cfg.api_key_dpapi = dpapi;
            cfg.api_key_sha256 = digest;
            return cfg;
        };
        const std::vector<OpenAiProfile> kLibrary = {
            make_entry("openai / gpt-4o-mini", "https://api.openai.com", "gpt-4o-mini",
                       "blob-v1", kDigestA),
            make_entry("other / other-model", "https://api.other.example", "other-model",
                       "blob-x", kDigestB),
            make_entry("openai / gpt-4o-mini (2)", "https://api.openai.com", "gpt-4o-mini",
                       "blob-c", kDigestC),
        };

        // (A1) DIGEST FIRST: the same key re-protected yields a different blob
        // but the same digest, so the mirror must still be found.
        {
            std::vector<OpenAiProfile> library = kLibrary;
            const std::size_t removed = EraseOpenAiProfileMatchingKey(
                library, make_deleted("https://api.openai.com", "gpt-4o-mini",
                                      "blob-v2-reprotected", kDigestA));
            TEST_CHECK(removed == 1 && library.size() == 2,
                       "fix-A2 (A1): a re-protected pair (new blob, same digest) still "
                       "identifies the mirror — exactly one entry removed");
            TEST_CHECK(library.size() == 2 && library[0].name == "other / other-model" &&
                           library[1].name == "openai / gpt-4o-mini (2)",
                       "fix-A2 (A1): the unrelated entry AND the different-key entry at the "
                       "same endpoint both survive");
        }
        // (A2) digest-only deleted block (half pair, blob wiped).
        {
            std::vector<OpenAiProfile> library = kLibrary;
            TEST_CHECK(EraseOpenAiProfileMatchingKey(
                           library, make_deleted("", "", "", kDigestA)) == 1 &&
                           library.size() == 2 && library[0].name == "other / other-model",
                       "fix-A2 (A2): a digest-only deleted block (half pair) still matches");
        }
        // (A3) blob-only deleted block (digest wiped).
        {
            std::vector<OpenAiProfile> library = kLibrary;
            TEST_CHECK(EraseOpenAiProfileMatchingKey(
                           library, make_deleted("", "", "blob-x", "")) == 1 &&
                           library.size() == 2 &&
                           library[0].name == "openai / gpt-4o-mini",
                       "fix-A2 (A3): a blob-only deleted block (half pair) still matches — "
                       "the stored blob is the second identity");
        }
        // (A4) KEYLESS fallback: the synthesizer's (base_url, model) identity,
        // restricted to keyless entries so a keyed profile for the same
        // endpoint survives.
        {
            std::vector<OpenAiProfile> library = {
                make_entry("openai / gpt-4o-mini", "https://api.openai.com", "gpt-4o-mini",
                           "", ""),
                make_entry("openai / gpt-4o-mini (2)", "https://api.openai.com",
                           "gpt-4o-mini", "blob-c", kDigestC),
                make_entry("keyless / other-model", "https://api.other.example",
                           "other-model", "", ""),
            };
            const std::size_t removed = EraseOpenAiProfileMatchingKey(
                library, make_deleted("https://api.openai.com", "gpt-4o-mini", "", ""));
            TEST_CHECK(removed == 1 && library.size() == 2,
                       "fix-A2 (A4): a keyless delete removes exactly the identity-matching "
                       "keyless (synthesized) entry");
            TEST_CHECK(library.size() == 2 && library[0].name == "openai / gpt-4o-mini (2)" &&
                           library[1].name == "keyless / other-model",
                       "fix-A2 (A4): the DIFFERENT-key profile at the same endpoint and the "
                       "unrelated keyless profile both survive");
        }
        // (A5) the fallback's base_url compare is ASCII-case-insensitive (the
        // PlanOpenAiProfileUpsert storage identity), model stays exact.
        {
            std::vector<OpenAiProfile> library = {
                make_entry("openai / gpt-4o-mini", "https://api.openai.com", "gpt-4o-mini",
                           "", ""),
            };
            std::vector<OpenAiProfile> same = library;
            TEST_CHECK(EraseOpenAiProfileMatchingKey(
                           same, make_deleted("HTTPS://Api.OpenAI.com", "gpt-4o-mini", "",
                                              "")) == 1 &&
                           same.empty(),
                       "fix-A2 (A5): the keyless fallback compares base_url "
                       "case-insensitively (hosts are case-insensitive)");
            std::vector<OpenAiProfile> cased = library;
            TEST_CHECK(EraseOpenAiProfileMatchingKey(
                           cased, make_deleted("https://api.openai.com", "GPT-4o-mini", "",
                                              "")) == 0 &&
                           cased.size() == 1,
                       "fix-A2 (A5): ... and the model stays case-SENSITIVE (model ids are "
                       "opaque tokens, not hosts)");
        }
        // (A6) an identity-less deleted block matches NOTHING (no blanket wipe).
        {
            std::vector<OpenAiProfile> library = kLibrary;
            const std::size_t removed =
                EraseOpenAiProfileMatchingKey(library, OpenAiConfig{});
            TEST_CHECK(removed == 0 && library.size() == kLibrary.size(),
                       "fix-A2 (A6): a block with no key and no endpoint removes nothing");
        }
        // (A7) a KEYED delete never touches a keyless entry at the same
        // identity (the digest is the identity, the endpoint is only the
        // keyless fallback).
        {
            std::vector<OpenAiProfile> library = {
                make_entry("keyless mirror", "https://api.openai.com", "gpt-4o-mini", "", ""),
                make_entry("openai / gpt-4o-mini", "https://api.openai.com", "gpt-4o-mini",
                           "blob-v1", kDigestA),
            };
            TEST_CHECK(EraseOpenAiProfileMatchingKey(
                           library, make_deleted("https://api.openai.com", "gpt-4o-mini",
                                                 "blob-c", kDigestC)) == 0 &&
                           library.size() == 2,
                       "fix-A2 (A7): deleting a key whose digest matches no entry leaves the "
                       "keyless entry at the same endpoint untouched (digest first)");
        }
    }

    // ---- (B) round trip: the migration mirror cannot outlive the delete ----
    {
        std::string plaintext = "sk-live-fix-a2-0123456789";
        std::string digest;
        TEST_CHECK(OpenAiSha256Hex(plaintext, digest) && digest.size() == 64,
                   "fix-A2 (B): the reference digest is computable");
        std::string blob1;
        std::string edit1 = plaintext;
        TEST_CHECK(ProtectOpenAiApiKey(edit1, blob1) && !blob1.empty(),
                   "fix-A2 (B): the reference DPAPI pair is protectable");
        std::string blob2;
        std::string edit2 = plaintext;
        TEST_CHECK(ProtectOpenAiApiKey(edit2, blob2) && blob2 != blob1,
                   "fix-A2 (B): a RE-PROTECT of the same key yields a different blob — the "
                   "reason the helper matches the digest first");

        const std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
        const std::filesystem::path dialog_file =
            temp_dir / "emebala_fixa2_dialog_delete.json";
        const std::filesystem::path main_file = temp_dir / "emebala_fixa2_main_delete.json";
        const std::filesystem::path unrelated_file =
            temp_dir / "emebala_fixa2_unrelated_delete.json";

        auto write_file = [](const std::filesystem::path& path, const std::string& text) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out << text;
            return out.good();
        };
        auto read_file = [](const std::filesystem::path& path) {
            std::ifstream in(path, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        };
        // The pre-B2 document the verifier's exploit scenario starts from: a
        // populated "openai" block, NO "openai_profiles" key.
        const std::string pre_b2 =
            "{\n"
            "  \"engine_type\": \"openai\",\n"
            "  \"openai\": {\n"
            "    \"base_url\": \"https://api.openai.com\",\n"
            "    \"model\": \"gpt-4o-mini\",\n"
            "    \"reasoning_effort\": \"low\",\n"
            "    \"api_key_dpapi\": \"" + blob1 + "\",\n"
            "    \"api_key_sha256\": \"" + digest + "\",\n"
            "    \"http_consent_given\": true\n"
            "  }\n"
            "}\n";
        TEST_CHECK(write_file(dialog_file, pre_b2) && write_file(main_file, pre_b2),
                   "fix-A2 (B): the pre-B2 fixtures are written (temp dir)");

        // (B1) the DIALOG's delete sequence, statement for statement.
        {
            AppConfig persisted;
            TEST_CHECK(persisted.LoadFromFile(dialog_file),
                       "fix-A2 (B1): the delete path's own fresh load succeeds");
            TEST_CHECK(persisted.openai_profiles.size() == 1 &&
                           persisted.openai_profiles[0].cfg.api_key_sha256 == digest &&
                           !persisted.openai_profiles[0].cfg.api_key_dpapi.empty(),
                       "fix-A2 (B1): that load re-synthesized the migration mirror WITH the "
                       "key pair — the leak's precondition, reproduced");
            const OpenAiConfig retired = persisted.openai;
            TEST_CHECK(retired.api_key_sha256 == digest,
                       "fix-A2 (B1): the identity is captured BEFORE the clear");
            ClearOpenAiSettings(persisted.openai);
            std::vector<OpenAiProfile> kept_profiles = persisted.openai_profiles;
            const std::size_t retired_count =
                EraseOpenAiProfileMatchingKey(kept_profiles, retired);
            TEST_CHECK(retired_count == 1 && kept_profiles.empty(),
                       "fix-A2 (B1): the mirror carrying the retired key is removed");
            if (retired_count > 0) {
                persisted.SetOpenAiProfiles(std::move(kept_profiles));
            }
            TEST_CHECK(persisted.SaveToFile(dialog_file),
                       "fix-A2 (B1): the delete's own save succeeds");

            const std::string document = read_file(dialog_file);
            TEST_CHECK(!document.empty(), "fix-A2 (B1): the persisted document is readable");
            TEST_CHECK(document.find(digest) == std::string::npos,
                       "fix-A2 (B1): no digest of the retired key anywhere in the persisted "
                       "JSON (whole-document scan)");
            TEST_CHECK(document.find(blob1) == std::string::npos,
                       "fix-A2 (B1): no DPAPI blob of the retired key anywhere in the "
                       "persisted JSON (whole-document scan)");
            TEST_CHECK(document.find("\"api_key_dpapi\": \"\"") != std::string::npos,
                       "fix-A2 (B1): the active block's pair fields persist EMPTY (the "
                       "field names stay — REQ-051 clears the values, not the schema)");
            TEST_CHECK(document.find(plaintext) == std::string::npos,
                       "fix-A2 (B1): nor the cleartext");

            // R1-12 interplay: the prune went through the verb, so an emptied
            // library persists as an explicit [] and the next load cannot
            // re-run the migration synthesis.
            TEST_CHECK(document.find("\"openai_profiles\": []") != std::string::npos,
                       "fix-A2 (B1): the emptied library persists as an explicit [] (the "
                       "fold-touched record), so the migration cannot re-synthesize");
            AppConfig reloaded;
            TEST_CHECK(reloaded.FromJsonString(document) &&
                           reloaded.openai_profiles.empty() && reloaded.openai.base_url.empty(),
                       "fix-A2 (B1): a reload of the delete's output carries no profile");
        }

        // (B2) main.cpp's kDeleted sequence on an INDEPENDENT instance: the
        // second persist site closes the leak by itself.
        {
            AppConfig owning;
            TEST_CHECK(owning.LoadFromFile(main_file),
                       "fix-A2 (B2): the owning config loads the same pre-B2 document");
            TEST_CHECK(owning.openai_profiles.size() == 1,
                       "fix-A2 (B2): the startup synthesis is the second site's precondition");
            const OpenAiConfig retired = owning.openai;
            owning.openai = OpenAiConfig{};
            std::vector<OpenAiProfile> kept_profiles = owning.openai_profiles;
            if (EraseOpenAiProfileMatchingKey(kept_profiles, retired) > 0) {
                owning.SetOpenAiProfiles(std::move(kept_profiles));
            }
            TEST_CHECK(owning.SaveToFile(main_file),
                       "fix-A2 (B2): the kDeleted reconcile save succeeds");
            const std::string document = read_file(main_file);
            TEST_CHECK(!document.empty() && document.find(digest) == std::string::npos &&
                           document.find(blob1) == std::string::npos,
                       "fix-A2 (B2): the second persist site alone also writes no trace of "
                       "the retired credential (this is the site the dialog's save cannot "
                       "cover)");
        }

        // (B3) an unrelated pre-existing profile survives both sites.
        {
            std::string unrelated_digest;
            std::string unrelated_plaintext = "sk-other-account-987654321";
            std::string unrelated_blob;
            std::string unrelated_edit = unrelated_plaintext;
            TEST_CHECK(ProtectOpenAiApiKey(unrelated_edit, unrelated_blob) &&
                           OpenAiSha256Hex(unrelated_plaintext, unrelated_digest) &&
                           unrelated_digest.size() == 64,
                       "fix-A2 (B3): the unrelated profile's pair is protectable");
            const std::string with_unrelated =
                "{\n"
                "  \"openai\": {\n"
                "    \"base_url\": \"https://api.openai.com\",\n"
                "    \"model\": \"gpt-4o-mini\",\n"
                "    \"api_key_dpapi\": \"" + blob1 + "\",\n"
                "    \"api_key_sha256\": \"" + digest + "\"\n"
                "  },\n"
                "  \"openai_profiles\": [\n"
                "    {\n"
                "      \"name\": \"openai / gpt-4o-mini\",\n"
                "      \"base_url\": \"https://api.openai.com\",\n"
                "      \"model\": \"gpt-4o-mini\",\n"
                "      \"api_key_dpapi\": \"" + blob1 + "\",\n"
                "      \"api_key_sha256\": \"" + digest + "\"\n"
                "    },\n"
                "    {\n"
                "      \"name\": \"other / other-model\",\n"
                "      \"base_url\": \"https://api.other.example\",\n"
                "      \"model\": \"other-model\",\n"
                "      \"api_key_dpapi\": \"" + unrelated_blob + "\",\n"
                "      \"api_key_sha256\": \"" + unrelated_digest + "\"\n"
                "    }\n"
                "  ]\n"
                "}\n";
            TEST_CHECK(write_file(unrelated_file, with_unrelated),
                       "fix-A2 (B3): the two-profile fixture is written");
            AppConfig persisted;
            TEST_CHECK(persisted.LoadFromFile(unrelated_file) &&
                           persisted.openai_profiles.size() == 2,
                       "fix-A2 (B3): the present-key library loads authoritatively");
            const OpenAiConfig retired = persisted.openai;
            ClearOpenAiSettings(persisted.openai);
            std::vector<OpenAiProfile> kept_profiles = persisted.openai_profiles;
            if (EraseOpenAiProfileMatchingKey(kept_profiles, retired) > 0) {
                persisted.SetOpenAiProfiles(std::move(kept_profiles));
            }
            TEST_CHECK(persisted.SaveToFile(unrelated_file),
                       "fix-A2 (B3): the delete's save succeeds");
            const std::string document = read_file(unrelated_file);
            TEST_CHECK(document.find(digest) == std::string::npos &&
                           document.find(blob1) == std::string::npos,
                       "fix-A2 (B3): the mirror's credential is gone");
            TEST_CHECK(document.find("other / other-model") != std::string::npos &&
                           document.find(unrelated_digest) != std::string::npos,
                       "fix-A2 (B3): the unrelated profile and its pair survive the delete");
            AppConfig reloaded;
            TEST_CHECK(reloaded.FromJsonString(document) &&
                           reloaded.openai_profiles.size() == 1 &&
                           reloaded.openai_profiles[0].name == "other / other-model",
                       "fix-A2 (B3): a reload shows exactly the unrelated profile");
        }

        // (B4) negative: a KEYLESS active block removes only the keyless
        // identity-matching profile; a differently-keyed profile at the same
        // endpoint stays.
        {
            const std::filesystem::path keyless_file =
                temp_dir / "emebala_fixa2_keyless_delete.json";
            const std::string keyless_doc =
                "{\n"
                "  \"openai\": {\n"
                "    \"base_url\": \"https://api.openai.com\",\n"
                "    \"model\": \"gpt-4o-mini\"\n"
                "  },\n"
                "  \"openai_profiles\": [\n"
                "    {\n"
                "      \"name\": \"openai / gpt-4o-mini\",\n"
                "      \"base_url\": \"https://api.openai.com\",\n"
                "      \"model\": \"gpt-4o-mini\"\n"
                "    },\n"
                "    {\n"
                "      \"name\": \"other key\",\n"
                "      \"base_url\": \"https://api.openai.com\",\n"
                "      \"model\": \"gpt-4o-mini\",\n"
                "      \"api_key_dpapi\": \"" + blob2 + "\",\n"
                "      \"api_key_sha256\": \"" + digest + "\"\n"
                "    }\n"
                "  ]\n"
                "}\n";
            TEST_CHECK(write_file(keyless_file, keyless_doc),
                       "fix-A2 (B4): the keyless fixture is written");
            AppConfig persisted;
            TEST_CHECK(persisted.LoadFromFile(keyless_file) &&
                           persisted.openai_profiles.size() == 2,
                       "fix-A2 (B4): the fixture's two profiles load");
            const OpenAiConfig retired = persisted.openai;
            TEST_CHECK(retired.api_key_dpapi.empty() && retired.api_key_sha256.empty(),
                       "fix-A2 (B4): the deleted block carries no key pair (the fallback "
                       "path)");
            ClearOpenAiSettings(persisted.openai);
            std::vector<OpenAiProfile> kept_profiles = persisted.openai_profiles;
            const std::size_t removed =
                EraseOpenAiProfileMatchingKey(kept_profiles, retired);
            TEST_CHECK(removed == 1 && kept_profiles.size() == 1 &&
                           kept_profiles[0].name == "other key",
                       "fix-A2 (B4): a keyless delete removes only the keyless "
                       "identity-matching profile; the same-endpoint profile with a "
                       "DIFFERENT key is left untouched");
            std::filesystem::remove(keyless_file);
        }

        std::filesystem::remove(dialog_file);
        std::filesystem::remove(main_file);
        std::filesystem::remove(unrelated_file);
    }

    // ---- (C) source pins: both persist sites + the write rule -------------
    {
        auto read_norm = [](const wchar_t* relative, std::string& out) {
            const std::filesystem::path path = ResolveRepoFile(relative);
            if (path.empty()) return false;
            std::ifstream in(path, std::ios::binary);
            if (!in) return false;
            const std::string raw((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            std::string norm;
            norm.reserve(raw.size());
            for (std::size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') continue;
                norm.push_back(raw[i]);
            }
            out.swap(norm);
            return true;
        };
        std::string dialog_src;
        std::string main_src;
        const bool dialog_ok = read_norm(L"src\\ui\\openai_settings_window.cpp", dialog_src);
        const bool main_ok = read_norm(L"src\\main.cpp", main_src);
        TEST_CHECK(dialog_ok && main_ok,
                   "fix-A2 (C): both persist-site sources resolve (R1-07 lesson: a source "
                   "pin never passes over zero assertions)");

        // (C1) the dialog's delete branch order: capture -> clear -> prune -> save.
        if (dialog_ok) {
            const std::size_t del_at = dialog_src.find("case IDC_DELETE_BTN: {");
            const std::size_t next_case = dialog_src.find("case IDC_PROFILE_SAVE_BTN", del_at);
            const std::string branch =
                del_at == std::string::npos
                    ? std::string()
                    : dialog_src.substr(del_at, next_case == std::string::npos
                                                    ? std::string::npos
                                                    : next_case - del_at);
            TEST_CHECK(!branch.empty(), "fix-A2 (C1): the delete branch is locatable");
            const std::size_t capture = branch.find("const OpenAiConfig retired = persisted.openai;");
            const std::size_t clear =
                branch.find("ClearOpenAiSettings(persisted.openai)");
            const std::size_t prune = branch.find("EraseOpenAiProfileMatchingKey(");
            const std::size_t write = branch.find("persisted.SetOpenAiProfiles(");
            const std::size_t save = branch.find("persisted.SaveToFile()");
            TEST_CHECK(capture != std::string::npos && clear != std::string::npos &&
                           prune != std::string::npos && write != std::string::npos &&
                           save != std::string::npos,
                       "fix-A2 (C1): capture / clear / prune / verb / save are all present "
                       "in the delete branch");
            TEST_CHECK(capture < clear && clear < prune && prune < write && write < save,
                       "fix-A2 (C1): the identity is captured BEFORE the clear, the prune "
                       "runs before the verb and the verb before the save — the "
                       "resurrection order is closed");
        }

        // (C2) the helper matches the digest first and only then falls back to
        // (base_url, model) for a keyless block.
        if (dialog_ok) {
            const std::size_t helper_at =
                dialog_src.find("std::size_t EraseOpenAiProfileMatchingKey(");
            const std::string helper =
                helper_at == std::string::npos ? std::string() : dialog_src.substr(helper_at, 3000);
            TEST_CHECK(!helper.empty(), "fix-A2 (C2): the helper definition is locatable");
            const std::size_t digest_at = helper.find("profile.cfg.api_key_sha256 ==");
            const std::size_t blob_at = helper.find("profile.cfg.api_key_dpapi ==");
            const std::size_t fallback_at = helper.find("EqualsIgnoreCaseAscii<char>(");
            TEST_CHECK(digest_at != std::string::npos && blob_at != std::string::npos &&
                           fallback_at != std::string::npos,
                       "fix-A2 (C2): the helper names the digest, the blob and the keyless "
                       "fallback");
            TEST_CHECK(digest_at < blob_at && blob_at < fallback_at,
                       "fix-A2 (C2): DIGEST first, the blob second, the (base_url, model) "
                       "fallback last — the stable identity wins");
            TEST_CHECK(helper.find("deleted.base_url.empty() && deleted.model.empty()") !=
                               std::string::npos &&
                           helper.find("profile.cfg.api_key_dpapi.empty() &&") !=
                               std::string::npos,
                       "fix-A2 (C2): an identity-less block removes nothing, and the "
                       "fallback only matches keyless entries");
        }

        // (C3) main.cpp's kDeleted seam: capture -> prune -> verb -> save, and
        // no direct member assignment anywhere (R1-02 caller half).
        if (main_ok) {
            const std::size_t seam_at =
                main_src.find("g_apply_openai_deleted = [&](const emebalachat::OpenAiConfig& edited) {");
            const std::size_t seam_end = main_src.find("REQ-048 R2-D", seam_at);
            const std::string seam =
                seam_at == std::string::npos
                    ? std::string()
                    : main_src.substr(seam_at, seam_end == std::string::npos
                                                    ? std::string::npos
                                                    : seam_end - seam_at);
            TEST_CHECK(!seam.empty(), "fix-A2 (C3): the kDeleted seam body is locatable");
            const std::size_t capture = seam.find("const emebalachat::OpenAiConfig retired = config.openai;");
            // 261122_0001 fix-E (R2-N2): the mirror write is the LOCKED VERB
            // now (the old `config.openai = edited;` direct assignment is the
            // exact race this unit migrated).
            const std::size_t mirror = seam.find("config.SetOpenAiConfig(edited);");
            const std::size_t prune = seam.find("EraseOpenAiProfileMatchingKey(");
            const std::size_t write = seam.find("config.SetOpenAiProfiles(");
            const std::size_t save = seam.find("config.SaveToFile();");
            TEST_CHECK(capture != std::string::npos && mirror != std::string::npos &&
                           prune != std::string::npos && write != std::string::npos &&
                           save != std::string::npos,
                       "fix-A2 (C3): the kDeleted seam captures the retired identity, "
                       "prunes, writes through the verb and saves");
            TEST_CHECK(capture < mirror && mirror < prune && prune < write && write < save,
                       "fix-A2 (C3): capture precedes the clear, and the prune + verb precede "
                       "the seam's SaveToFile (the second persist site is closed)");
            TEST_CHECK(seam.find("openai_profiles =") == std::string::npos,
                       "fix-A2 (C3): the seam never assigns the library member directly");
            std::size_t direct_assigns = 0;
            for (std::size_t pos = main_src.find("config.openai_profiles =");
                 pos != std::string::npos;
                 pos = main_src.find("config.openai_profiles =", pos + 1)) {
                ++direct_assigns;
            }
            TEST_CHECK(direct_assigns == 0,
                       "fix-A2 (C3): NO direct config.openai_profiles assignment survives in "
                       "main.cpp — every write routes through SetOpenAiProfiles (R1-02)");
            TEST_CHECK(main_src.find("emebalachat::s_pending_openai_profiles = "
                                     "config.openai_profiles;") != std::string::npos,
                       "fix-A2 (C3): the deferred-open seed stays a read COPY (not a "
                       "persist site: the kDeleted branch never applies the fold)");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 261122_0001 fix-A2 delete resurrection suite passed." << std::endl;
    } else {
        std::cout << "[FAIL] 261122_0001 fix-A2 delete resurrection suite: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

} // namespace openai_profile_tests
