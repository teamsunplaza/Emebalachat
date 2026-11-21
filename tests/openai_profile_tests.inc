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
// ---------------------------------------------------------------------------
// 261122_0001 B-U4: the dialog TU's pure PROFILE seams, defined at namespace
// scope in src/ui/openai_settings_window.cpp. Declared here (the suite rides
// inside run_tests.cpp, which links Emebalachat_core) because these helpers stay
// out of openai_settings_window.hpp on purpose (TU-scope seams, not public API)
// — the same split the declaration folds and the reasoning planners use. They
// are declared as PLAIN functions with out-parameters, not with structs, so no
// type definition has to be duplicated across TUs.
namespace emebalachat {
bool IsMaskedPlaceholder(std::string_view keyUtf8);
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

    // ---- (a) EnumCount: 207 -> 213 at this unit's boundary -----------------
    volatile int enum_count = static_cast<int>(StringId::EnumCount);
    TEST_CHECK(enum_count == 213,
               "B-U2: StringId::EnumCount is 213 (207 + the six OpenAI profile ids)");

    // ---- (b) insertion order ----------------------------------------------
    // BackTransSectionOriginal + 1 == the first profile id, the six ids are a
    // contiguous run, and the run ENDS where the pre-existing progress/cancel
    // run begins. The P5 tail sentinel is the witness that every later id
    // shifted by the same +6 instead of drifting.
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
                   p5_sentinel == enum_count - 1,
               "B-U2: the six profile ids are contiguous right after "
               "BackTransSectionOriginal and right before the 261121_0002 "
               "progress/cancel run, so the P5 tail sentinel and every relative "
               "adjacency pin still hold");

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
        // probe and the profile save).
        TEST_CHECK(eb::IsMaskedPlaceholder("abcdef***"),
                   "B-U4 key: the masked read-back text is recognized");
        TEST_CHECK(!eb::IsMaskedPlaceholder("***") &&
                       !eb::IsMaskedPlaceholder("sk-abcdef") &&
                       !eb::IsMaskedPlaceholder(""),
                   "B-U4 key: a short mask, a plain key and an empty edit are NOT "
                   "placeholders");

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

        // (iv) a placeholder with no pair behind it -> still no pair.
        {
            OpenAiConfig none;
            OpenAiConfig out;
            std::string edit = "***";
            TEST_CHECK(!eb::IsMaskedPlaceholder(edit),
                       "B-U4 key[iv]: the 3-character mask is not a placeholder");
            TEST_CHECK(eb::ResolveOpenAiProfileKeyPair(edit, none, out),
                       "B-U4 key[iv]: a 3-character mask is digested as typed input");
            TEST_CHECK(!out.api_key_dpapi.empty(),
                       "B-U4 key[iv]: ... so the edit is treated as a typed key");
        }
        {
            OpenAiConfig none;
            OpenAiConfig out;
            std::string edit = "abcdef***";
            TEST_CHECK(eb::ResolveOpenAiProfileKeyPair(edit, none, out),
                       "B-U4 key[iv]: a placeholder with an EMPTY existing pair resolves");
            TEST_CHECK(out.api_key_dpapi.empty() && out.api_key_sha256.empty(),
                       "B-U4 key[iv]: ... and adopts nothing (no pair to keep)");
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
        std::cout << "[SKIP] src/ui/openai_settings_window.cpp not resolvable from the "
                     "test CWD; the B-U4 proc-wiring pins are skipped." << std::endl;
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
        TEST_CHECK(osw_src.find("OpenAiProfilesFold& profiles) {") != std::string::npos,
                   "B-U4 fold: the library-aware overload is defined");
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
                                "profiles);") != std::string::npos,
                   "B-U4 fold: the declaration-only overload forwards to the library-aware "
                   "one (existing callers keep their contract)");
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

} // namespace openai_profile_tests
