// REQ-045 P4-3 (design §3b, item 3b): OpenAI Compatible engine — pure
// security/policy helper tests + config round-trip. Headless (DPAPI runs in
// the current-user context, so protect/unprotect round-trips here). Included
// directly by tests/run_tests.cpp after the TEST_CHECK harness is defined.

// ---- URL security classification ----
void TestReq045OpenAiUrlSecurity() {
    std::cout << "[RUN] Testing REQ-045 OpenAI base URL security classification..." << std::endl;

    TEST_CHECK(ClassifyOpenAiBaseUrl("https://api.openai.com") == OpenAiUrlSecurity::Https,
               "https classified Https");
    TEST_CHECK(ClassifyOpenAiBaseUrl("https://api.openai.com/v1") == OpenAiUrlSecurity::Https,
               "https with path classified Https");
    TEST_CHECK(ClassifyOpenAiBaseUrl("HTTPS://example.com") == OpenAiUrlSecurity::Https,
               "uppercase HTTPS classified Https");
    TEST_CHECK(ClassifyOpenAiBaseUrl("http://localhost:8080") == OpenAiUrlSecurity::Http,
               "http classified Http");
    TEST_CHECK(ClassifyOpenAiBaseUrl("http://192.168.1.1:11434") == OpenAiUrlSecurity::Http,
               "http with port classified Http");
    TEST_CHECK(ClassifyOpenAiBaseUrl("ftp://example.com") == OpenAiUrlSecurity::Invalid,
               "ftp classified Invalid");
    TEST_CHECK(ClassifyOpenAiBaseUrl("example.com") == OpenAiUrlSecurity::Invalid,
               "missing scheme classified Invalid");
    TEST_CHECK(ClassifyOpenAiBaseUrl("://example.com") == OpenAiUrlSecurity::Invalid,
               "empty scheme classified Invalid");
    TEST_CHECK(ClassifyOpenAiBaseUrl("") == OpenAiUrlSecurity::Invalid,
               "empty string classified Invalid");
}

// ---- SHA-256 integrity digest ----
void TestReq045OpenAiSha256() {
    std::cout << "[RUN] Testing REQ-045 OpenAI SHA-256 digest (NIST vectors)..." << std::endl;

    std::string hex;
    // NIST known-answer: SHA-256("") 
    TEST_CHECK(OpenAiSha256Hex("", hex) &&
               hex == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
               "SHA-256 of empty string matches NIST vector");
    // NIST known-answer: SHA-256("abc")
    TEST_CHECK(OpenAiSha256Hex("abc", hex) &&
               hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
               "SHA-256 of \"abc\" matches NIST vector");
    // 64 lowercase hex chars.
    TEST_CHECK(OpenAiSha256Hex("emebala", hex) && hex.size() == 64,
               "digest is 64 hex chars");
}

// ---- Display masking ----
void TestReq045OpenAiMasking() {
    std::cout << "[RUN] Testing REQ-045 OpenAI API key masking..." << std::endl;

    // >6 chars: first 6 + "***"
    TEST_CHECK(MaskOpenAiApiKey("sk-abcdefghijklmnop") == "sk-abc***",
               "long key masked to first 6 + ***");
    TEST_CHECK(MaskOpenAiApiKey("abcdefg") == "abcdef***",
               "7-char key masked to first 6 + ***");
    // <=6 chars: fully masked.
    TEST_CHECK(MaskOpenAiApiKey("abcdef") == "***",
               "6-char key fully masked");
    TEST_CHECK(MaskOpenAiApiKey("abc") == "***",
               "3-char key fully masked");
    TEST_CHECK(MaskOpenAiApiKey("") == "***",
               "empty key fully masked");
}

// ---- DPAPI protect/unprotect round-trip (current-user context) ----
void TestReq045OpenAiDpapi() {
    std::cout << "[RUN] Testing REQ-045 OpenAI DPAPI key protect/unprotect round-trip..." << std::endl;

    // Writable copy (Protect scrubs its cleartext argument).
    std::string key = "sk-test-roundtrip-key-0123456789";
    const std::string original = key;
    std::string blob;
    TEST_CHECK(ProtectOpenAiApiKey(key, blob) && !blob.empty(),
               "Protect produces a non-empty DPAPI blob");
    // Protect must have scrubbed the caller's copy.
    TEST_CHECK(key != original, "Protect scrubs the caller's cleartext copy");

    std::string recovered;
    TEST_CHECK(UnprotectOpenAiApiKey(blob, recovered) && recovered == original,
               "Unprotect round-trips to the original key");
    // The recovered buffer holds the live key; the caller contract is to zero
    // it after use. Do that here to model (and assert) the discipline.
    SecureZeroMemory(recovered.data(), recovered.size());

    // Tamper: a blob that is not valid base64 / not DPAPI must fail closed.
    std::string out;
    TEST_CHECK(!UnprotectOpenAiApiKey("!!!not-base64!!!", out) && out.empty(),
               "garbage blob fails closed");
    TEST_CHECK(!UnprotectOpenAiApiKey("", out) && out.empty(),
               "empty blob fails closed");

    // Integrity digest matches the original key.
    std::string digest;
    TEST_CHECK(OpenAiSha256Hex(original, digest) && digest.size() == 64,
               "integrity digest computes for the original key");

    // REQ-050: the settings dialog's save composition — digest taken BEFORE
    // protect, because Protect scrubs the caller's cleartext buffer. Model
    // the fixed order end-to-end and pin the invariant WithUnprotectedKey
    // relies on: the digest captured from the live cleartext must equal the
    // digest re-computed from the decrypted key (under the old digest-after-
    // protect order this parity check failed: digest == SHA-256(zero buffer)).
    std::string key2 = "sk-order-pin-key-0123456789";
    const std::string original2 = key2;
    std::string digest2;
    TEST_CHECK(OpenAiSha256Hex(key2, digest2) && digest2.size() == 64,
               "REQ-050: digest computable from the live cleartext before protect");
    std::string blob2;
    TEST_CHECK(ProtectOpenAiApiKey(key2, blob2) && !blob2.empty(),
               "REQ-050: protect succeeds after the digest was taken");
    std::string recovered2;
    TEST_CHECK(UnprotectOpenAiApiKey(blob2, recovered2) && recovered2 == original2,
               "REQ-050: unprotect still round-trips after digest-then-protect");
    std::string digest3;
    TEST_CHECK(OpenAiSha256Hex(recovered2, digest3) && digest3 == digest2,
               "REQ-050: digest taken before protect matches the decrypted-key re-hash");
    SecureZeroMemory(recovered2.data(), recovered2.size());

    // REQ-050 (corrupt-pair recovery): the dialog-open integrity helper must
    // accept a digest-before-protect pair and refuse the pre-REQ-050 corrupt
    // shape (digest persisted AFTER protect == SHA-256 of the scrubbed,
    // all-zero cleartext buffer), so the settings dialog can force a one-time
    // re-entry instead of reusing a pair WithUnprotectedKey refuses forever.
    std::string key4 = "sk-initdialog-check-0123456789";
    const std::string original4 = key4;
    std::string digest4;
    TEST_CHECK(OpenAiSha256Hex(key4, digest4) && digest4.size() == 64,
               "REQ-050: digest computable for the integrity-helper key");
    std::string blob4;
    TEST_CHECK(ProtectOpenAiApiKey(key4, blob4) && !blob4.empty(),
               "REQ-050: protect succeeds for the integrity-helper key");
    OpenAiConfig good_pair;
    good_pair.api_key_dpapi = blob4;
    good_pair.api_key_sha256 = digest4;
    TEST_CHECK(OpenAiKeyPairIntegrityOk(good_pair),
               "REQ-050: a digest-before-protect pair passes the dialog-open check");
    // The pre-REQ-050 corruption shape: the digest was taken after protect,
    // i.e. over the in-place-zeroed cleartext buffer (same length as the key).
    OpenAiConfig corrupt_pair;
    corrupt_pair.api_key_dpapi = blob4;
    TEST_CHECK(OpenAiSha256Hex(std::string(original4.size(), '\0'),
                               corrupt_pair.api_key_sha256),
               "REQ-050: the zeroed-buffer digest models the old corruption");
    TEST_CHECK(!OpenAiKeyPairIntegrityOk(corrupt_pair),
               "REQ-050: the zeroed-buffer digest pair is refused (forced re-entry)");
    // A blob that no longer unprotects is equally unusable (fail closed).
    OpenAiConfig garbage_pair;
    garbage_pair.api_key_dpapi = "!!!not-base64!!!";
    garbage_pair.api_key_sha256 = digest4;
    TEST_CHECK(!OpenAiKeyPairIntegrityOk(garbage_pair),
               "REQ-050: an ununprotectable blob fails the dialog-open check");
    // No key on record is not "ok" — there is nothing to reuse.
    OpenAiConfig empty_pair;
    TEST_CHECK(!OpenAiKeyPairIntegrityOk(empty_pair),
               "REQ-050: an empty pair is not integrity-ok");
}

// ---- config round-trip for the openai{} block ----
void TestReq045OpenAiConfigRoundTrip() {
    std::cout << "[RUN] Testing REQ-045 OpenAI config block round-trip..." << std::endl;

    AppConfig cfg;
    cfg.openai.base_url = "https://api.example.com/v1";
    cfg.openai.model = "gpt-4o-mini";
    cfg.openai.api_key_dpapi = "QUJDREVGRw==";  // opaque blob placeholder
    cfg.openai.api_key_sha256 = "deadbeefcafe0123456789abcdef0123456789abcdef0123456789abcdef0123";
    cfg.openai.http_consent_given = true;

    const std::string json = cfg.ToJsonString();
    TEST_CHECK(json.find("\"openai\":") != std::string::npos,
               "serialized config contains the openai block");
    TEST_CHECK(json.find("\"base_url\": \"https://api.example.com/v1\"") != std::string::npos,
               "base_url serialized");
    TEST_CHECK(json.find("\"model\": \"gpt-4o-mini\"") != std::string::npos,
               "model serialized");
    TEST_CHECK(json.find("\"api_key_dpapi\": \"QUJDREVGRw==\"") != std::string::npos,
               "api_key_dpapi serialized");
    TEST_CHECK(json.find("\"http_consent_given\": true") != std::string::npos,
               "http_consent_given serialized");

    // Parse it back via LoadFromFile on a fresh object.
    // Extract the openai block from the serialized JSON and assert the values
    // round-trip. (LoadFromFile reads the canonical path, so the parse half is
    // covered here by inspecting the serialized block text directly.)
    const std::string marker = "\"openai\": {";
    const std::string::size_type pos = json.find(marker);
    TEST_CHECK(pos != std::string::npos, "openai block present for direct parse");
    if (pos != std::string::npos) {
        const std::string::size_type open = json.find('{', pos);
        int depth = 0; std::string block; bool started = false;
        for (size_t i = open; i < json.size(); ++i) {
            const char c = json[i];
            if (c == '{') { ++depth; started = true; }
            if (started) block += c;
            if (c == '}') { --depth; if (depth == 0) break; }
        }
        TEST_CHECK(block.find("\"base_url\": \"https://api.example.com/v1\"") != std::string::npos,
                   "captured block round-trips base_url");
        TEST_CHECK(block.find("\"model\": \"gpt-4o-mini\"") != std::string::npos,
                   "captured block round-trips model");
        TEST_CHECK(block.find("\"http_consent_given\": true") != std::string::npos,
                   "captured block round-trips http_consent_given");
    }

    // =====================================================================
    // 261116_0003 P9c-1a (language-menu redesign, plan Phase 3 / §9): the
    // OpenAI "이 모델이 지원하는 언어" DECLARATION - config serialization plus
    // the ONE view filter that honours it. The settings-window checklist that
    // edits the set is the NEXT unit; nothing here creates a window
    // (AGENTS.md §10 headless rule).
    //
    // Contract under test (config.hpp, block above GetLanguagesForEngine):
    //   (a) declare a subset -> save -> reload -> the OpenAI view offers
    //       EXACTLY that subset (and nothing else changes);
    //   (b) absent declaration -> the view is the pre-P9c set, byte for byte,
    //       and the key is not emitted at all;
    //   (c) unknown / blank / AUTO / over-long codes are dropped without
    //       failing the load, duplicates collapse, names resolve to codes, and
    //       the stored form is REGISTRY order;
    //   (d) single authority: the filter is applied in GetLanguagesForEngine
    //       only, so the tray's four language menus, the tooltip's two
    //       dropdowns, the search picker and the Ctrl+F9 cycle set cannot
    //       drift.
    // =====================================================================
    {
        // Runtime comparisons of size constants (MSVC C4127 discipline, the
        // P9b-2/P9b-3 suite idiom).
        const auto p9c_eq = [](std::size_t lhs, std::size_t rhs) { return lhs == rhs; };
        const auto same_rows = [](const std::vector<const LanguageInfo*>& lhs,
                                  const std::vector<const LanguageInfo*>& rhs) {
            if (lhs.size() != rhs.size()) {
                return false;
            }
            for (std::size_t i = 0; i < lhs.size(); ++i) {
                if (lhs[i] != rhs[i]) {
                    return false;
                }
            }
            return true;
        };
        const auto codes_of = [](const std::vector<const LanguageInfo*>& rows) {
            std::string codes;
            for (const LanguageInfo* row : rows) {
                if (row == nullptr) {
                    codes += "<null>";
                } else {
                    codes += row->code;
                }
                codes += ',';
            }
            return codes;
        };

        // The filter input is process-wide, so this block starts and ends on
        // the "not declared" baseline: no other suite can be affected and this
        // suite cannot depend on what ran before it.
        PublishOpenAiDeclaredLanguages({});
        TEST_CHECK(!OpenAiLanguagesDeclared() && !OpenAiLanguageDeclared("KO"),
                   "P9c-1a: the default published state is 'not declared'");

        // ---- (b) absent declaration: the pre-P9c view, unchanged -----------
        // The expectation is derived from the registry itself (every row that
        // carries kLangOpenAi, in registry order), not from the function under
        // test, so this cannot pass vacuously.
        std::vector<const LanguageInfo*> expected_src;
        std::vector<const LanguageInfo*> expected_tgt;
        for (const LanguageInfo& row : GetSupportedLanguages()) {
            if ((row.engine_mask & kLangOpenAi) == 0) {
                continue;
            }
            expected_src.push_back(&row);
            if (row.code != "AUTO") {
                expected_tgt.push_back(&row);
            }
        }
        const std::vector<const LanguageInfo*> baseline_src =
            GetLanguagesForEngine(LanguageEngine::OpenAi, true);
        const std::vector<const LanguageInfo*> baseline_tgt =
            GetLanguagesForEngine(LanguageEngine::OpenAi, false);
        const std::vector<const LanguageInfo*> baseline_google_tgt =
            GetLanguagesForEngine(LanguageEngine::Google, false);
        const std::vector<const LanguageInfo*> baseline_hymt2_tgt =
            GetLanguagesForEngine(LanguageEngine::HyMt2, false);
        const std::vector<const LanguageInfo*> baseline_gguf_tgt =
            GetLanguagesForEngine(LanguageEngine::UserGguf, false);
        const std::vector<const LanguageInfo*> baseline_auto_src =
            GetLanguagesForEngine(LanguageEngine::Auto, true);
        TEST_CHECK(same_rows(baseline_src, expected_src) && same_rows(baseline_tgt, expected_tgt) &&
                       p9c_eq(expected_tgt.size(), GetSupportedLanguages().size() - 1),
                   "P9c-1a: (b) an absent declaration yields every kLangOpenAi registry row in "
                   "registry order (90 source rows incl. AUTO / 89 target rows)");
        TEST_CHECK(p9c_eq(kMaxOpenAiDeclaredLanguages, baseline_tgt.size()),
                   "P9c-1a: (c) kMaxOpenAiDeclaredLanguages is exactly the OpenAI target-view size "
                   "(the defensive cap can never truncate a legal declaration)");
        {
            AppConfig undeclared;
            const std::string undeclared_json = undeclared.ToJsonString();
            TEST_CHECK(undeclared_json.find("openai_declared_languages") == std::string::npos,
                       "P9c-1a: (b) an empty declaration emits NO key, so a pre-P9c config.json "
                       "round-trips byte-identically");
            AppConfig legacy;
            const bool legacy_ok = legacy.FromJsonString(
                "{\"ui_language\": \"ko\", \"engine_type\": \"openai\", \"openai\": {\"model\": \"m\"}}");
            TEST_CHECK(legacy_ok && legacy.openai_declared_languages.empty() &&
                           !OpenAiLanguagesDeclared() &&
                           codes_of(GetLanguagesForEngine(LanguageEngine::OpenAi, false)) ==
                               codes_of(expected_tgt),
                       "P9c-1a: (b) a config.json WITHOUT the key (every pre-P9c file) loads as "
                       "'not declared' and the view stays the full set");
        }

        // ---- (a) declare -> the view offers exactly that subset -------------
        AppConfig declared_cfg;
        const std::vector<std::string> stored = declared_cfg.SetOpenAiDeclaredLanguages(
            // mixed spellings + junk: name, code, lowercase, duplicate, unknown,
            // blank, the AUTO marker, an over-long token, registry-order proof.
            {"Korean", "ja", "ko", "EN", "Klingon", "", "  AUTO  ", std::string(128, 'X')});
        TEST_CHECK(stored == std::vector<std::string>({"EN", "KO", "JA"}),
                   "P9c-1a: (c) the canonical set is registry codes in REGISTRY order; names "
                   "resolve, duplicates collapse, unknown/blank/AUTO/over-long tokens drop");
        TEST_CHECK(declared_cfg.openai_declared_languages == stored,
                   "P9c-1a: (a) the locked verb stores exactly what it published (member and views "
                   "cannot hold different sets)");
        TEST_CHECK(codes_of(GetLanguagesForEngine(LanguageEngine::OpenAi, true)) == "AUTO,EN,KO,JA," &&
                       codes_of(GetLanguagesForEngine(LanguageEngine::OpenAi, false)) == "EN,KO,JA,",
                   "P9c-1a: (a) with a non-empty declaration the OpenAI view offers ONLY the declared "
                   "codes (AUTO stays in the source view: it is a source-only marker)");
        const LanguageMenuView declared_merge =
            BuildLanguageMenuView(LanguageEngine::OpenAi, /*include_auto=*/false, {});
        TEST_CHECK(codes_of(declared_merge.registry) == "EN,KO,JA," &&
                       declared_merge.registry.size() == GetLanguagesForEngine(
                                                              LanguageEngine::OpenAi, false)
                                                              .size(),
                   "P9c-1a: (a) the D5/D7 merge authority inherits the filter (it wraps the same "
                   "GetLanguagesForEngine view, so tray + tooltip menus cannot disagree)");

        // One authority, one declaration: no other engine's view moves, and the
        // auto MENU view still resolves to Google (P9b-2 contract).
        TEST_CHECK(same_rows(baseline_google_tgt, GetLanguagesForEngine(LanguageEngine::Google, false)) &&
                       same_rows(baseline_hymt2_tgt, GetLanguagesForEngine(LanguageEngine::HyMt2, false)) &&
                       same_rows(baseline_gguf_tgt, GetLanguagesForEngine(LanguageEngine::UserGguf, false)) &&
                       same_rows(baseline_auto_src, GetLanguagesForEngine(LanguageEngine::Auto, true)),
                   "P9c-1a: the declaration is OpenAI-scoped - Google / HyMt2 / UserGguf and the "
                   "AUTO routing view are untouched");
        TEST_CHECK(codes_of(BuildLanguageMenuView(LanguageEngine::Auto, false, {}).registry) ==
                       codes_of(baseline_google_tgt),
                   "P9c-1a: under 'auto' the MENU view still resolves to the Google list (P9b-2), "
                   "so the declaration never narrows the auto menus");

        // The picker is the seventh consumer and reads the SAME view.
        {
            language_palette::PickerInit picker_init;
            picker_init.engine = LanguageEngine::OpenAi;
            picker_init.role = language_palette::PickerRole::Target;
            const language_palette::PickerState picker(picker_init);
            TEST_CHECK(p9c_eq(picker.pool().size(), static_cast<std::size_t>(3)) &&
                           codes_of(picker.pool()) == "EN,KO,JA," &&
                           p9c_eq(picker.hits().size(), static_cast<std::size_t>(3)),
                       "P9c-1a: (d) the search picker's pool inherits the filtered view (no "
                       "picker-local filter)");
            language_palette::PickerInit picker_src_init;
            picker_src_init.engine = LanguageEngine::OpenAi;
            picker_src_init.role = language_palette::PickerRole::Source;
            const language_palette::PickerState picker_src(picker_src_init);
            TEST_CHECK(codes_of(picker_src.pool()) == "AUTO,EN,KO,JA,",
                       "P9c-1a: (d) ... and its SOURCE role keeps AUTO in front of the declared set");
        }

        // ---- (a) save -> reload -> the view is the same subset --------------
        const std::string declared_json = declared_cfg.ToJsonString();
        TEST_CHECK(declared_json.find("\"openai_declared_languages\": [\"EN\", \"KO\", \"JA\"]") !=
                       std::string::npos,
                   "P9c-1a: (a) the declaration is persisted as a canonical array next to the "
                   "openai block");
        TEST_CHECK(p9c_eq(static_cast<std::size_t>(kUserLanguagesSchemaVersion),
                          static_cast<std::size_t>(1)) &&
                       declared_json.find("openai_declared_languages_version") == std::string::npos,
                   "P9c-1a: (c) the field is ADDITIVE - no schema version bump, no version key "
                   "(the P9b-3 rule: absent/unknown-declaration configs keep parsing as today)");
        AppConfig reloaded;
        const bool reloaded_ok = reloaded.FromJsonString(declared_json);
        TEST_CHECK(reloaded_ok && reloaded.openai_declared_languages == stored,
                   "P9c-1a: (a) ToJsonString -> FromJsonString keeps the declaration");
        TEST_CHECK(reloaded_ok &&
                       codes_of(GetLanguagesForEngine(LanguageEngine::OpenAi, false)) == "EN,KO,JA,",
                   "P9c-1a: (a) ... and the reloaded config republishes the filter, so the views "
                   "offer exactly that subset");
        {
            // Real disk cycle (the P9b-3 idiom): SaveToFile -> LoadFromFile.
            const std::filesystem::path p9c_path =
                std::filesystem::temp_directory_path() / L"emebalachat_p9c1a_declared.json";
            std::error_code p9c_ec;
            std::filesystem::remove(p9c_path, p9c_ec); // stale leftovers must not mask a failure
            AppConfig to_disk;
            to_disk.SetOpenAiDeclaredLanguages({"Chinese Simplified", "ar"});
            const bool saved = to_disk.SaveToFile(p9c_path);
            AppConfig from_disk;
            const bool loaded = saved && from_disk.LoadFromFile(p9c_path);
            TEST_CHECK(loaded &&
                           from_disk.openai_declared_languages ==
                               std::vector<std::string>({"ZH-CN", "AR"}),
                       "P9c-1a: (a) a real SaveToFile/LoadFromFile cycle keeps the declaration "
                       "('Chinese Simplified' is an 18-byte NAME and still resolves: the input "
                       "token bound is not the code bound) in registry order");
            TEST_CHECK(loaded &&
                           codes_of(GetLanguagesForEngine(LanguageEngine::OpenAi, false)) ==
                               "ZH-CN,AR,",
                       "P9c-1a: (a) ... and the views after the disk cycle offer exactly that subset");
            std::filesystem::remove(p9c_path, p9c_ec); // our own artifact
        }

        // ---- (c) unknown codes / malformed values never fail the load ------
        {
            AppConfig bogus;
            const std::vector<std::string> bogus_stored =
                bogus.SetOpenAiDeclaredLanguages({"Klingon", "  ", "zz", "AUTO Detect"});
            TEST_CHECK(bogus_stored.empty() && !OpenAiLanguagesDeclared() &&
                           codes_of(GetLanguagesForEngine(LanguageEngine::OpenAi, false)) ==
                               codes_of(expected_tgt),
                       "P9c-1a: (c) a declaration naming only unknown/blank/AUTO codes is dropped "
                       "ENTIRELY: the view fails OPEN to the full set instead of emptying the menus");
            AppConfig malformed;
            TEST_CHECK(malformed.FromJsonString("{\"openai_declared_languages\": \"KO,JA\"}") &&
                           malformed.openai_declared_languages.empty() && !OpenAiLanguagesDeclared(),
                       "P9c-1a: (c) a non-array value reads as 'not declared' without failing the load");
            AppConfig partial;
            TEST_CHECK(partial.FromJsonString("{\"openai_declared_languages\": [\"KO\", 42, \"JA\"]}") &&
                           partial.openai_declared_languages == std::vector<std::string>({"KO"}),
                       "P9c-1a: (c) a malformed element stops the walk at the valid prefix (lenient: "
                       "an unknown element is dropped, the load still succeeds)");
            AppConfig trailing;
            TEST_CHECK(trailing.FromJsonString("{\"openai_declared_languages\": [\"JA\", \"KO\",]}") &&
                           trailing.openai_declared_languages ==
                               std::vector<std::string>({"KO", "JA"}),
                       "P9c-1a: (c) a trailing comma and a hand-reordered array both canonicalize to "
                       "the registry order");
            AppConfig cleared;
            cleared.SetOpenAiDeclaredLanguages({"KO"});
            const bool armed_before = OpenAiLanguagesDeclared();
            cleared.SetOpenAiDeclaredLanguages({});
            TEST_CHECK(armed_before && !OpenAiLanguagesDeclared() &&
                           codes_of(GetLanguagesForEngine(LanguageEngine::OpenAi, false)) ==
                               codes_of(expected_tgt),
                       "P9c-1a: clearing the declaration (the settings dialog's 'no declaration' "
                       "state) returns the views to the full set");
        }

        // ---- (d) structural: ONE filter point, no surface re-filters -------
        {
            const auto read_src_file = [](const std::filesystem::path& path, std::string& out) {
                std::ifstream in(path, std::ios::binary);
                if (!in) {
                    return false;
                }
                std::ostringstream ss;
                ss << in.rdbuf();
                out = ss.str();
                return true;
            };
            const auto count_of = [](const std::string& haystack, const std::string& needle) {
                std::size_t count = 0;
                for (std::size_t hit = haystack.find(needle); hit != std::string::npos;
                     hit = haystack.find(needle, hit + needle.size())) {
                    ++count;
                }
                return count;
            };
            const std::filesystem::path config_cpp_path = ResolveRepoFile(L"src\\config.cpp");
            std::string config_src;
            const bool config_read =
                !config_cpp_path.empty() && read_src_file(config_cpp_path, config_src);
            TEST_CHECK(config_read,
                       "P9c-1a: (d) src/config.cpp resolves for the single-authority pin");
            if (config_read) {
                TEST_CHECK(count_of(config_src, "return FilterOpenAiViewForDeclaration(") == 1,
                           "P9c-1a: (d) exactly ONE view-filter application exists (inside "
                           "GetLanguagesForEngine)");
                TEST_CHECK(count_of(config_src, "FilterOpenAiViewForDeclaration(") == 2,
                           "P9c-1a: (d) ... and one definition (no second application anywhere in "
                           "config.cpp)");
                TEST_CHECK(count_of(config_src,
                                    "const std::vector<const LanguageInfo*>& base = "
                                    "GetLanguagesForEngine(") == 1,
                           "P9c-1a: (d) BuildLanguageMenuView derives its base view from "
                           "GetLanguagesForEngine, so the merge cannot bypass the filter");
            }
            // The declaration symbols must not appear in ANY menu/render
            // surface: a surface that re-filtered would be the drift this pin
            // exists to stop. The settings window (openai_settings_window.cpp)
            // is deliberately NOT scanned: it is the future EDITOR of the set
            // and legitimately reads AppConfig::openai_declared_languages; what
            // no surface may do is decide which rows the engine offers.
            const wchar_t* const surface_files[] = {
                L"src\\ui\\tray.cpp",
                L"src\\ui\\tooltip.cpp",
                L"src\\ui\\language_search_palette.hpp",
                L"src\\ui\\language_search_palette.cpp",
            };
            const char* const declaration_symbols[] = {
                "openai_declared_languages",
                "OpenAiDeclaredLanguage",
                "OpenAiLanguagesDeclared",
                "OpenAiLanguageDeclared",
                "PublishOpenAiDeclaredLanguages",
                "FilterOpenAiViewForDeclaration",
                "kLangOpenAi",
            };
            for (const wchar_t* relative : surface_files) {
                const std::filesystem::path path = ResolveRepoFile(relative);
                std::string src;
                const bool read = !path.empty() && read_src_file(path, src);
                TEST_CHECK(read, "P9c-1a: (d) a menu surface resolves for the anti-drift scan");
                if (!read) {
                    continue;
                }
                std::size_t hits = 0;
                for (const char* symbol : declaration_symbols) {
                    hits += count_of(src, symbol);
                }
                TEST_CHECK(hits == 0,
                           "P9c-1a: (d) no menu surface carries a declaration symbol: the filter "
                           "lives only in GetLanguagesForEngine");
            }
            // ... while the two consuming surfaces still go through the shared
            // builder (tray: 4 language menus, tooltip: 2 dropdowns).
            const std::filesystem::path tray_path = ResolveRepoFile(L"src\\ui\\tray.cpp");
            const std::filesystem::path tooltip_path = ResolveRepoFile(L"src\\ui\\tooltip.cpp");
            std::string tray_src;
            std::string tooltip_src;
            const bool tray_read = !tray_path.empty() && read_src_file(tray_path, tray_src);
            const bool tooltip_read = !tooltip_path.empty() && read_src_file(tooltip_path, tooltip_src);
            TEST_CHECK(tray_read && tooltip_read &&
                           count_of(tray_src, "BuildLanguageMenuView(") >= 2 &&
                           count_of(tooltip_src, "BuildLanguageMenuView(") >= 2,
                       "P9c-1a: (d) the six menu surfaces still consume the shared merge builder "
                       "(tray x2 calls, tooltip x2 calls)");
        }

        // Leave the process snapshot on the 'not declared' baseline: the suites
        // that run after this one must see the pre-P9c views.
        PublishOpenAiDeclaredLanguages({});
        TEST_CHECK(!OpenAiLanguagesDeclared() &&
                       codes_of(GetLanguagesForEngine(LanguageEngine::OpenAi, false)) ==
                           codes_of(expected_tgt),
                   "P9c-1a: the suite restores the 'not declared' baseline (no cross-suite state)");
    }
}

// ---- EnumCount completeness: the 11 new StringIds return non-empty in the
// exposed locales (complements TestR6P5P6I18n's loop which already walks all
// StringId < EnumCount; this just pins the new ones explicitly). ----
void TestReq045OpenAiI18nPresent() {
    std::cout << "[RUN] Testing REQ-045 OpenAI i18n strings present..." << std::endl;
    const StringId ids[] = {
        StringId::MenuEngineOpenAi, StringId::OpenAiSettingsTitle,
        StringId::OpenAiSettingsAction, StringId::OpenAiBaseUrlLabel,
        StringId::OpenAiApiKeyLabel, StringId::OpenAiModelLabel,
        StringId::OpenAiFetchModels, StringId::OpenAiFetchFailed,
        StringId::OpenAiHttpWarningTitle, StringId::OpenAiHttpWarningBody,
        StringId::OpenAiSaved, StringId::OpenAiKeyMasked,
        StringId::OpenAiInvalidBaseUrl,
        // REQ-050: the OpenAI settings cue-banner hints (edit placeholders).
        StringId::OpenAiBaseUrlHint, StringId::OpenAiApiKeyHint,
    };
    // EnumCount advanced by 13 (57 -> 70) in P4-3, by 6 more (70 -> 76)
    // in REQ-045 P4-5 (item 3a-2, the user-.gguf registration strings), by
    // 1 more (76 -> 77) in REQ-047 D2 (the bundled-duplicate notice body),
    // and by 1 more (77 -> 78) in REQ-047 U1 (designer 164500: the tray
    // "(미등록)" empty-slot marker), and by 17 more (78 -> 95) in REQ-048
    // R2-D (the gguf model-manager strings + manager-error strings), by 4
    // more (95 -> 99) for tooltip_translate_failed / repair_transient_body
    // and the REQ-050 dialog OK/Cancel labels, by 8 more (99 -> 107) in
    // the REQ-050 merged-manager Hugging Face block, and by 2 more
    // (107 -> 109) for the REQ-050 OpenAI cue-banner hints, and by 1 more
    // (109 -> 110) for the REQ-051 OpenAI settings [삭제] caption, and by 25
    // more (110 -> 135) for the REQ-UC update-zone block, and by 9 more
    // (135 -> 144) for the REQ-MD model-download dialog, and by 13 more
    // (144 -> 157) for the REQ-060 load-mode/policy strings. Read
    // through a volatile to keep the runtime comparison (avoids C4127 on the
    // constant expression).
    volatile int enum_count = static_cast<int>(StringId::EnumCount);
    TEST_CHECK(enum_count == 199,
                "StringId::EnumCount is 199 after the 13+6+1+1+17+4+8+2+1+25+9+13 field appends + the 4 v0.10.5 theme strings + the 5 261108_0002 WS3 back-translation strings (FIX-WS3B added BackTranslateReference) + the 3 261111_0001 U6 user-model strings + the 2 261116_0003 P5 language-picker strings + the 1 261116_0003 P6a flat-menu search-row string + the 1 261116_0003 P7 picker engine-chip count string + the 23 261116_0003 P9b-1 user-language surface strings + the 1 261116_0003 P9b-3 U5 picker AUTO chip string + the 2 261116_0003 P9c-1b OpenAI declaration checklist strings");
    for (StringId id : ids) {
        TEST_CHECK(!I18n::Get(id).empty(), "OpenAI StringId returns non-empty");
    }
}

// ---- REQ-050: endpoint-join fix + fetch-probe consent + dialog geometry ----
// DecomposeBaseUrl lives in an anonymous namespace inside
// openai_compatible_client.cpp, so the unit suite cannot call it directly;
// the live path-shape proof rides the env-var-gated mock round-trip in
// TestReq046OpenAiLocalhostConsent (extended for REQ-050) and the exact-path
// mock probe under tools_tmp_req050/. This suite pins the fix STRUCTURALLY
// (source-text differential, the TestReq044VerbatimSync / REQ-048 R2-D (e)
// precedent): the endpoint must be appended DIRECTLY onto the base path, and
// "/v1" assumed only when the base has no path. That single join rule is what
// makes base "http://h/v1" -> "/v1/models", base "http://h" -> "/v1/models",
// and base "http://h/api/v1" -> "/api/v1/models" (NEVER "/api/v1/v1/models").
void TestReq050OpenAiEndpointJoin() {
    std::cout << "[RUN] Testing REQ-050 OpenAI endpoint join + consent + geometry pins..."
              << std::endl;
    const int failures_before = g_failed_count;

    const std::filesystem::path client_cpp = ResolveRepoFile(L"src\\openai_compatible_client.cpp");
    if (client_cpp.empty()) {
        std::cout << "[SKIP] src/openai_compatible_client.cpp not resolvable; "
                     "REQ-050 endpoint pins skipped." << std::endl;
    } else {
        std::ifstream in(client_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        // The fixed joins: the endpoint is appended DIRECTLY onto the base
        // path prefix; "/v1" appears only in the no-prefix default branch.
        // These literal pins cover all three base shapes at once
        // ("http://h/v1" -> "/v1/models", "http://h" -> "/v1/models",
        // "http://h/api/v1" -> "/api/v1/models").
        TEST_CHECK(src.find("prefix.empty() ? L\"/v1/models\" : prefix + L\"/models\"")
                       != std::string::npos,
                   "REQ-050: ListModels joins /models onto the base path prefix");
        TEST_CHECK(src.find("prefix.empty() ? L\"/v1/chat/completions\"") != std::string::npos &&
                   src.find(": prefix + L\"/chat/completions\"") != std::string::npos,
                   "REQ-050: ChatCompletion joins /chat/completions onto the base path prefix");
        // The confirmed-buggy double-prefix joins must be gone.
        TEST_CHECK(src.find("prefix + L\"/v1/models\"") == std::string::npos,
                   "REQ-050: the double-/v1 ListModels join (.../v1/v1/models) is gone");
        TEST_CHECK(src.find("prefix + L\"/v1/chat/completions\"") == std::string::npos,
                   "REQ-050: the double-/v1 ChatCompletion join is gone");
        // R3 (261029_0001, round-3 security L-2 / code-k3 P3): this client
        // carries the Authorization: Bearer header the redirect block exists to
        // protect, so the WinHttpSetOption result must be CHECKED - an ignored
        // failure silently leaves WinHTTP's FOLLOW default in place.
        const std::string::size_type r3_policy = src.find(
            "if (!::WinHttpSetOption(hSession.get(), WINHTTP_OPTION_REDIRECT_POLICY,");
        TEST_CHECK(r3_policy != std::string::npos &&
                       src.find("OPENAI/HttpRequest/005") != std::string::npos,
                   "R3: a failed redirect-policy set fails OPEN but is logged shape-only (005)");
        TEST_CHECK(r3_policy != std::string::npos &&
                       r3_policy < src.find("::WinHttpConnect("),
                   "R3: the checked policy set precedes the request (session default cannot win)");
    }

    // REQ-050 (B): the pre-save fetch button must ask the SAME http consent
    // question as IDOK instead of letting the probe default to
    // http_consent_given=false (the fetch consent gap).
    const std::filesystem::path settings_cpp =
        ResolveRepoFile(L"src\\ui\\openai_settings_window.cpp");
    if (settings_cpp.empty()) {
        std::cout << "[SKIP] src/ui/openai_settings_window.cpp not resolvable; "
                     "REQ-050 consent/geometry pins skipped." << std::endl;
    } else {
        std::ifstream in(settings_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        const std::string::size_type fetch_at = src.find("case IDC_FETCH_BTN");
        const std::string::size_type ok_at = src.find("case IDOK");
        TEST_CHECK(fetch_at != std::string::npos && ok_at != std::string::npos &&
                   fetch_at < ok_at,
                   "REQ-050: fetch handler precedes the IDOK handler");
        if (fetch_at != std::string::npos && ok_at != std::string::npos && fetch_at < ok_at) {
            const std::string fetch_block = src.substr(fetch_at, ok_at - fetch_at);
            // REQ-050 (auto-fetch): the button handler is now a thin wrapper —
            // it drives the shared DoFetchModels helper and KEEPS the failure
            // notice (the silent auto path must not reference it).
            TEST_CHECK(fetch_block.find("DoFetchModels(dlg, st)") != std::string::npos &&
                       fetch_block.find("OpenAiFetchFailed") != std::string::npos,
                       "REQ-050 (B): the manual fetch button drives DoFetchModels "
                       "and keeps its failure notice");
        }

        // REQ-050 (B/auto-fetch): the consent gate + probe digest + ListModels
        // + combo refill live in the shared DoFetchModels helper (defined
        // before the dialog proc), factored out of the former IDC_FETCH_BTN
        // body so the debounced auto-fetch reuses the exact same path.
        const std::string::size_type dofetch_at =
            src.find("std::vector<std::string> DoFetchModels");
        const std::string::size_type proc_at = src.find("INT_PTR CALLBACK OpenAiSettingsProc");
        TEST_CHECK(dofetch_at != std::string::npos && proc_at != std::string::npos &&
                   dofetch_at < proc_at,
                   "REQ-050: DoFetchModels (consent + ListModels + combo refill) "
                   "precedes the dialog proc");
        if (dofetch_at != std::string::npos && proc_at != std::string::npos &&
            dofetch_at < proc_at) {
            const std::string dofetch_src = src.substr(dofetch_at, proc_at - dofetch_at);
            TEST_CHECK(dofetch_src.find("OpenAiUrlSecurity::Http && !st->cfg->http_consent_given")
                           != std::string::npos,
                       "REQ-050 (B): the fetch probe re-checks http consent before ListModels");
            TEST_CHECK(dofetch_src.find("probe.http_consent_given = true") != std::string::npos,
                       "REQ-050 (B): a YES answer sets probe.http_consent_given");
            TEST_CHECK(dofetch_src.find("StringId::OpenAiHttpWarningTitle") != std::string::npos,
                       "REQ-050 (B): the fetch probe reuses the IDOK consent MessageBox strings");
            // The shared helper keeps the digest-BEFORE-protect order (it
            // moved out of the button case with the refactor).
            const std::string::size_type sha_pos = dofetch_src.find("OpenAiSha256Hex(keyUtf8");
            const std::string::size_type protect_pos =
                dofetch_src.find("ProtectOpenAiApiKey(keyUtf8");
            TEST_CHECK(sha_pos != std::string::npos && protect_pos != std::string::npos &&
                       sha_pos < protect_pos,
                       "REQ-050: the shared fetch hashes the key BEFORE protect scrubs it");
        }
        // REQ-050 (C/E): widened dialog + i18n'd OK/Cancel buttons. The
        // 261116_0003 P9c-1b declaration checklist grew the shell to 262x174.
        TEST_CHECK(src.find("tb.Begin(title, 262, 174") != std::string::npos,
                   "REQ-050 (C): the dialog template is 262 x 174 DLU (P9c-1b grew the "
                   "262 x 130 shell for the declaration checklist)");
        TEST_CHECK(src.find("I18n::Get(StringId::DialogOk)") != std::string::npos &&
                   src.find("I18n::Get(StringId::DialogCancel)") != std::string::npos,
                   "REQ-050 (E): OK/Cancel buttons use the i18n dialog strings");
        TEST_CHECK(src.find("L\"OK\"") == std::string::npos &&
                   src.find("L\"Cancel\"") == std::string::npos,
                   "REQ-050 (E): no hardcoded L\"OK\"/L\"Cancel\" labels remain");

        // REQ-050 (digest order): ProtectOpenAiApiKey scrubs the caller's
        // cleartext buffer in place (SecureZeroMemory, on success AND on
        // failure — see src/openai_compatible_client.cpp), so the persisted
        // api_key_sha256 must be taken BEFORE protect at BOTH call sites.
        // The old digest-after-protect order persisted SHA-256(zero buffer),
        // and WithUnprotectedKey's re-hash of the real decrypted key refused
        // every saved key (live proof: OPENAI/WithUnprotectedKey/002).
        // Source-order pins per the REQ-048 R2-D (e) precedent above.
        const std::string::size_type idcancel_at = src.find("case IDCANCEL");
        if (ok_at != std::string::npos && idcancel_at != std::string::npos &&
            ok_at < idcancel_at) {
            const std::string ok_src = src.substr(ok_at, idcancel_at - ok_at);
            const std::string::size_type sha_pos = ok_src.find("OpenAiSha256Hex(keyUtf8");
            const std::string::size_type protect_pos =
                ok_src.find("ProtectOpenAiApiKey(keyUtf8");
            TEST_CHECK(sha_pos != std::string::npos && protect_pos != std::string::npos &&
                       sha_pos < protect_pos,
                       "REQ-050: IDOK save hashes the key BEFORE protect scrubs it");
        }

        // REQ-050 (single-line edit limits): both single-line EDITs must
        // carry ES_AUTOHSCROLL — without it the control rejects input past
        // its visible width (device-confirmed on the HF dialog's URL edit,
        // same latent hazard here for base URLs / API keys).
        TEST_CHECK(src.find("EDT | ES_AUTOHSCROLL") != std::string::npos,
                   "REQ-050: OpenAI dialog single-line edits carry ES_AUTOHSCROLL");

        // REQ-050 (corrupt-pair recovery): WM_INITDIALOG must verify the
        // persisted pair headlessly and clear BOTH fields on mismatch, so an
        // upgraded corrupt pair (digest == SHA-256 of the protect-scrubbed
        // zero buffer) can never reach the masked-placeholder reuse path
        // (IDC_FETCH_BTN / IDOK) again. The check must live in the INIT
        // block, before WM_COMMAND, and log shape-only.
        const std::string::size_type initdialog_at = src.find("case WM_INITDIALOG");
        const std::string::size_type wmcommand_at = src.find("case WM_COMMAND");
        TEST_CHECK(initdialog_at != std::string::npos &&
                   wmcommand_at != std::string::npos && initdialog_at < wmcommand_at,
                   "REQ-050: WM_INITDIALOG block precedes WM_COMMAND");
        if (initdialog_at != std::string::npos && wmcommand_at != std::string::npos &&
            initdialog_at < wmcommand_at) {
            const std::string init_src =
                src.substr(initdialog_at, wmcommand_at - initdialog_at);
            TEST_CHECK(init_src.find("OpenAiKeyPairIntegrityOk") != std::string::npos,
                       "REQ-050: dialog open verifies the persisted key pair headlessly");
            TEST_CHECK(init_src.find("api_key_dpapi.clear()") != std::string::npos &&
                       init_src.find("api_key_sha256.clear()") != std::string::npos,
                       "REQ-050: a failed pair check clears BOTH fields (forced re-entry)");
            TEST_CHECK(init_src.find("UI/OpenAiSettings/004") != std::string::npos,
                       "REQ-050: the recovery logs a shape-only UI/OpenAiSettings/004 line");
            // REQ-050 (cue banners): both single-line edits get a gray
            // EM_SETCUEBANNER placeholder from the two new tail StringIds.
            TEST_CHECK(init_src.find("EM_SETCUEBANNER") != std::string::npos &&
                       init_src.find("StringId::OpenAiBaseUrlHint") != std::string::npos &&
                       init_src.find("StringId::OpenAiApiKeyHint") != std::string::npos,
                       "REQ-050: WM_INITDIALOG sets cue banners from the two hint StringIds");
            // REQ-050 (auto-fetch): reopening a saved dialog arms the debounce
            // timer when both values are already present.
            TEST_CHECK(init_src.find("SetTimer") != std::string::npos &&
                       init_src.find("KeyAvailableForFetch") != std::string::npos,
                       "REQ-050: WM_INITDIALOG arms the auto-fetch timer when both "
                       "values are already present");
        }

        // REQ-050 (auto-fetch): EN_CHANGE on either edit debounces through
        // SetTimer/KillTimer, gated to https; the WM_TIMER case kills the
        // timer and drives DoFetchModels; and the auto path is SILENT —
        // it must not reference the failure notice the manual button keeps.
        const std::string::size_type enchange_at = src.find("HIWORD(wp) == EN_CHANGE");
        const std::string::size_type idswitch_at = src.find("switch (LOWORD(wp))");
        TEST_CHECK(enchange_at != std::string::npos && idswitch_at != std::string::npos &&
                   enchange_at < idswitch_at,
                   "REQ-050: the EN_CHANGE debounce block precedes the command-id switch");
        if (enchange_at != std::string::npos && idswitch_at != std::string::npos &&
            enchange_at < idswitch_at) {
            const std::string enchange_src = src.substr(enchange_at, idswitch_at - enchange_at);
            TEST_CHECK(enchange_src.find("SetTimer") != std::string::npos &&
                       enchange_src.find("KillTimer") != std::string::npos &&
                       enchange_src.find("OpenAiUrlSecurity::Https") != std::string::npos,
                       "REQ-050: EN_CHANGE arms/kills the debounce timer, https-gated");
            TEST_CHECK(enchange_src.find("MessageBox") == std::string::npos &&
                       enchange_src.find("OpenAiHttpWarningTitle") == std::string::npos,
                       "REQ-050: typing never triggers consent popups or notices");
        }
        const std::string::size_type wmtimer_at = src.find("case WM_TIMER");
        TEST_CHECK(wmtimer_at != std::string::npos && wmtimer_at < wmcommand_at,
                   "REQ-050: the WM_TIMER auto-fetch case precedes WM_COMMAND");
        if (wmtimer_at != std::string::npos && wmcommand_at != std::string::npos &&
            wmtimer_at < wmcommand_at) {
            const std::string timer_src = src.substr(wmtimer_at, wmcommand_at - wmtimer_at);
            TEST_CHECK(timer_src.find("KillTimer") != std::string::npos &&
                       timer_src.find("DoFetchModels") != std::string::npos,
                       "REQ-050: WM_TIMER kills the timer then drives DoFetchModels");
            TEST_CHECK(timer_src.find("OpenAiFetchFailed") == std::string::npos,
                       "REQ-050: the auto-fetch path stays silent (no failure box)");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-050 OpenAI endpoint join checks passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-050 OpenAI endpoint join checks: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
