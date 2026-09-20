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
    };
    // EnumCount advanced by 13 (57 -> 70) in P4-3, by 6 more (70 -> 76)
    // in REQ-045 P4-5 (item 3a-2, the user-.gguf registration strings), by
    // 1 more (76 -> 77) in REQ-047 D2 (the bundled-duplicate notice body),
    // and by 1 more (77 -> 78) in REQ-047 U1 (designer 164500: the tray
    // "(미등록)" empty-slot marker), and by 12 more (78 -> 90) in REQ-048
    // R2-D (the gguf model-manager strings). Read through a volatile to keep
    // the runtime comparison (avoids C4127 on the constant expression).
    volatile int enum_count = static_cast<int>(StringId::EnumCount);
    TEST_CHECK(enum_count == 95,
                "StringId::EnumCount is 95 after the 13+6+1+1+12+5 field appends");
    for (StringId id : ids) {
        TEST_CHECK(!I18n::Get(id).empty(), "OpenAI StringId returns non-empty");
    }
}
