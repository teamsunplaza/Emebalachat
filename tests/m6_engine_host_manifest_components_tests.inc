// REQ-043 (M6 T2): engine_host_manifest + engine_host_components unit tests.
// Included directly by tests/run_tests.cpp. Pure-parser checks; the manifest
// hash-verify tests use a temp dir + injected providers (no machine state).

void TestEngineHostManifest() {
    std::cout << "[RUN] Testing engine-host manifest parser + verifiers (REQ-043 M6 T2)..." << std::endl;
    const int failures_before = g_failed_count;
    namespace ehm = emebalachat::engine_host_manifest;
    using ehm::LoadStatus;

    const char* kGoodSha = "5c3fe0b1408a5ceb0143184ef247b11b579c525f4b02b060e6c851bb76fef1a4";

    // ---- 1. Golden document (design §3.2) ----
    {
        std::string doc = std::string("{\"schema_version\":1,\"pinned\":{\"Hy-MT2-1.8B-Q8_0.gguf\":\"") +
                          kGoodSha + "\"}}";
        const auto r = ehm::ParseManifestJson(doc);
        TEST_CHECK(r.status == LoadStatus::Ok, "manifest golden: Ok");
        TEST_CHECK(r.manifest.schema_version == 1, "manifest golden: schema_version=1");
        TEST_CHECK(r.manifest.pinned.size() == 1, "manifest golden: one pin");
        std::string sha;
        TEST_CHECK(r.manifest.FindPin("Hy-MT2-1.8B-Q8_0.gguf", sha), "manifest golden: pin hit");
        TEST_CHECK(sha == kGoodSha, "manifest golden: pin value");
        TEST_CHECK(!r.manifest.FindPin("other.gguf", sha), "manifest golden: pin miss");
        TEST_CHECK(!r.manifest.empty(), "manifest golden: not empty");
    }

    // ---- 2. Unknown fields ignored; extra pins preserved ----
    {
        std::string doc = std::string(
            "{\"schema_version\":1,\"extra\":\"ignored\","
            "\"pinned\":{\"a.gguf\":\"" ) + kGoodSha +
            "\",\"b.gguf\":\"" + kGoodSha + "\",\"unknown_pin.gguf\":\"" + kGoodSha + "\"}}";
        const auto r = ehm::ParseManifestJson(doc);
        TEST_CHECK(r.status == LoadStatus::Ok, "manifest extra pins: Ok");
        TEST_CHECK(r.manifest.pinned.size() == 3, "manifest extra pins: all preserved");
    }

    // ---- 3. Fail-closed: schema_version missing/other ----
    {
        std::string doc = std::string("{\"pinned\":{\"a.gguf\":\"") + kGoodSha + "\"}}";
        TEST_CHECK(ehm::ParseManifestJson(doc).status == LoadStatus::SchemaVersion,
                   "manifest no schema_version -> reject");
        TEST_CHECK(ehm::ParseManifestJson("{\"schema_version\":2,\"pinned\":{}}").status ==
                       LoadStatus::SchemaVersion,
                   "manifest schema_version=2 -> reject");
        TEST_CHECK(ehm::ParseManifestJson("{\"schema_version\":1}").status == LoadStatus::Malformed,
                   "manifest missing pinned{} -> Malformed");
        TEST_CHECK(ehm::ParseManifestJson("garbage").status == LoadStatus::NotJson,
                   "manifest garbage -> NotJson");
    }

    // ---- 4. Fail-closed: path escape in pinned key / bad hash form ----
    {
        std::string doc = std::string("{\"schema_version\":1,\"pinned\":{\"..\\\\evil.gguf\":\"") +
                          kGoodSha + "\"}}";
        const auto r = ehm::ParseManifestJson(doc);
        TEST_CHECK(r.status == LoadStatus::Malformed, "manifest path-escape key -> reject");
        TEST_CHECK(r.manifest.pinned.empty(), "manifest path-escape key -> pins cleared");
        const auto r2 = ehm::ParseManifestJson(
            std::string("{\"schema_version\":1,\"pinned\":{\"a.gguf\":\"" ) + kGoodSha +
            "ZZ\"}}"); // 66 chars
        TEST_CHECK(r2.status == LoadStatus::Malformed, "manifest long hash -> reject");
        const auto r3 = ehm::ParseManifestJson(
            std::string("{\"schema_version\":1,\"pinned\":{\"a.gguf\":\"") +
            "5C3FE0B1408A5CEB0143184EF247B11B579C525F4B02B060E6C851BB76FEF1A4\"}}"); // uppercase
        TEST_CHECK(r3.status == LoadStatus::Malformed,
                   "manifest uppercase hash -> reject (lowercase contract)");
        const auto r4 = ehm::ParseManifestJson(
            std::string("{\"schema_version\":1,\"pinned\":{\"a.gguf\":") + "12345}}");
        TEST_CHECK(r4.status == LoadStatus::Malformed, "manifest numeric pin value -> reject");
        const auto r5 = ehm::ParseManifestJson(
            std::string("{\"schema_version\":1,\"pinned\":{\"sub/dir.gguf\":\"") + kGoodSha + "\"}}");
        TEST_CHECK(r5.status == LoadStatus::Malformed, "manifest slash key -> reject");
    }

    // ---- 5. Verifier contract: DevTrust / Authenticode / factory ----
    {
        const std::filesystem::path fake = std::filesystem::temp_directory_path() /
                                           L"m6t2_manifest_probe.json";
        auto dev = ehm::CreateManifestVerifier("dev");
        TEST_CHECK(dev != nullptr, "verifier factory: dev mode non-null");
        TEST_CHECK(dynamic_cast<ehm::DevTrustVerifier*>(dev.get()) != nullptr,
                   "verifier factory: dev mode type");
        TEST_CHECK(dev->VerifySignature(fake), "DevTrustVerifier: always true (dev lower bound)");
        auto dflt = ehm::CreateManifestVerifier();
        TEST_CHECK(dflt->VerifySignature(fake), "CreateManifestVerifier default = dev trust");
        auto auth = ehm::CreateManifestVerifier("authenticode");
        TEST_CHECK(dynamic_cast<ehm::AuthenticodeVerifier*>(auth.get()) != nullptr,
                   "verifier factory: authenticode type");
        // Fail-closed skeleton: certificate pending -> ALWAYS false.
        TEST_CHECK(!auth->VerifySignature(fake),
                   "AuthenticodeVerifier: fail-closed (certificate pending)");
        auto unknown = ehm::CreateManifestVerifier("bogus-mode");
        TEST_CHECK(unknown->VerifySignature(fake),
                   "verifier factory: unknown mode falls back to dev trust");
        // Null-path tolerance (shape-only).
        TEST_CHECK(dev->VerifySignature({}), "DevTrustVerifier: empty path tolerated");
        TEST_CHECK(!auth->VerifySignature({}), "AuthenticodeVerifier: empty path still rejects");
    }

    // ---- 6. Hash verification: caller-injected provider ----
    {
        // Build a tiny models dir with one file whose content hashes to a
        // KNOWN provider value (the provider is a stub — the point is the
        // manifest comparison logic, not real hashing).
        const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                          L"m6t2_manifest_models";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path model = dir / L"model.gguf";
        {
            std::ofstream f(model, std::ios::binary);
            f << "model-bytes";
        }
        std::string doc = std::string("{\"schema_version\":1,\"pinned\":{\"model.gguf\":\"") +
                          kGoodSha + "\"}}";
        const auto m = ehm::ParseManifestJson(doc);
        TEST_CHECK(m.status == LoadStatus::Ok, "hash verify: manifest parses");

        static const char* kGoodShaStatic = kGoodSha; // lambda capture (no default capture needed)
        const auto provider_ok = [](const std::filesystem::path&, std::string& out) {
            out = kGoodShaStatic;
            return true;
        };
        const auto provider_bad = [](const std::filesystem::path&, std::string& out) {
            out = std::string(64, '0');
            return true;
        };
        const auto provider_fail = [](const std::filesystem::path&, std::string&) {
            return false;
        };
        const auto provider_throw = [](const std::filesystem::path&, std::string&) -> bool {
            throw std::runtime_error("provider exploded");
        };

        const auto ok = ehm::VerifyModelHashes(m.manifest, dir, {"model.gguf"}, provider_ok);
        TEST_CHECK(ok.status == ehm::HashCheckStatus::AllPinsMatch, "hash verify: matching pin passes");

        const auto bad = ehm::VerifyModelHashes(m.manifest, dir, {"model.gguf"}, provider_bad);
        TEST_CHECK(bad.status == ehm::HashCheckStatus::HashMismatch, "hash verify: mismatch rejected");
        TEST_CHECK(bad.detail == "model.gguf", "hash verify: mismatch names the file");

        const auto fail = ehm::VerifyModelHashes(m.manifest, dir, {"model.gguf"}, provider_fail);
        TEST_CHECK(fail.status == ehm::HashCheckStatus::HashProviderFailed,
                   "hash verify: provider failure reported");

        const auto thr = ehm::VerifyModelHashes(m.manifest, dir, {"model.gguf"}, provider_throw);
        TEST_CHECK(thr.status == ehm::HashCheckStatus::HashProviderFailed,
                   "hash verify: throwing provider contained");

        const auto unpinned = ehm::VerifyModelHashes(m.manifest, dir, {"unlisted.gguf"}, provider_ok);
        TEST_CHECK(unpinned.status == ehm::HashCheckStatus::NotPinned,
                   "hash verify: unregistered bundled file rejected (§V2-6)");

        // A pinned-but-absent file is NOT manifest-checkable: it has no pin,
        // so §V2-6 rejects it as NotPinned FIRST (pass-1 order in the
        // implementation). The Missing status applies to a file that IS
        // pinned AND absent — use the multi-pin doc2 for that shape.
        std::string doc_absent = std::string(
            "{\"schema_version\":1,\"pinned\":{\"absent.gguf\":\"") + kGoodSha + "\"}}";
        const auto m_absent = ehm::ParseManifestJson(doc_absent);
        TEST_CHECK(m_absent.status == LoadStatus::Ok, "hash verify: absent-pin doc parses");
        const auto missing = ehm::VerifyModelHashes(m_absent.manifest, dir, {"absent.gguf"},
                                                    provider_ok);
        TEST_CHECK(missing.status == ehm::HashCheckStatus::Missing,
                   "hash verify: pinned-but-absent file -> Missing");

        const auto noprov = ehm::VerifyModelHashes(m.manifest, dir, {"model.gguf"}, nullptr);
        TEST_CHECK(noprov.status == ehm::HashCheckStatus::HashProviderFailed,
                   "hash verify: null provider -> provider-failed");

        // Multi-file: every pinned file must EXIST for an all-match. Create
        // the second fixture file first (the dir still holds model.gguf).
        {
            std::ofstream f2(dir / L"model2.gguf", std::ios::binary);
            f2 << "model2-bytes";
        }
        std::string doc2 = std::string(
            "{\"schema_version\":1,\"pinned\":{\"model.gguf\":\"") + kGoodSha +
            "\",\"model2.gguf\":\"" + kGoodSha + "\"}}";
        const auto m2 = ehm::ParseManifestJson(doc2);
        const auto multi = ehm::VerifyModelHashes(m2.manifest, dir, {"model.gguf", "model2.gguf"},
                                                  provider_ok);
        TEST_CHECK(multi.status == ehm::HashCheckStatus::AllPinsMatch,
                   "hash verify: multi-file all-match");
        // First-failure-wins ordering with a mixed list.
        const auto mixed = ehm::VerifyModelHashes(m2.manifest, dir,
                                                  {"model.gguf", "gone.gguf"}, provider_ok);
        TEST_CHECK(mixed.status == ehm::HashCheckStatus::NotPinned,
                   "hash verify: unpinned second file wins over hash check");
        // origin:user files never enter here (caller filters) — documented by
        // the absence of an origin parameter (compile-time contract).

        std::filesystem::remove_all(dir, ec);
    }

    // ---- 7. Default loader contract ----
    {
        const auto dir = ehm::DefaultModelsDir();
        if (!dir.empty()) {
            const auto r = ehm::LoadDefaultManifest();
            TEST_CHECK(r.status == LoadStatus::Ok || r.status == LoadStatus::Missing,
                       "manifest default loader: Ok-or-Missing contract");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Engine-host manifest parser tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Engine-host manifest parser tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestEngineHostComponents() {
    std::cout << "[RUN] Testing engine-host components parser + rule A (REQ-043 M6 T2)..." << std::endl;
    const int failures_before = g_failed_count;
    namespace ehc = emebalachat::engine_host_components;
    using ehc::LoadStatus;
    using R = ehc::ReplaceDecision;

    // ---- 1. Golden document (design §3.3) ----
    {
        const char* golden =
            "{\n"
            "  \"schema_version\": 1,\n"
            "  \"components\": {\n"
            "    \"orchestrator\": {\"version\": \"0.10.1\", \"abi_version\": 2},\n"
            "    \"ggml-translate\": {\"version\": \"0.10.1\", \"abi_version\": 1,\n"
            "                         \"engine\": \"llama.cpp\", \"engine_version\": \"b6099\"}\n"
            "  }\n"
            "}";
        const auto r = ehc::ParseComponentsJson(golden);
        TEST_CHECK(r.status == LoadStatus::Ok, "components golden: Ok");
        TEST_CHECK(r.file.schema_version == 1, "components golden: schema_version=1");
        TEST_CHECK(r.file.components.size() == 2, "components golden: two entries");
        const ehc::ComponentEntry* orch = r.file.Find("orchestrator");
        TEST_CHECK(orch != nullptr, "components golden: orchestrator present");
        TEST_CHECK(orch->version == "0.10.1", "components golden: orchestrator.version");
        TEST_CHECK(orch->has_version && orch->has_abi_version, "components golden: orchestrator flags");
        TEST_CHECK(orch->abi_version == 2, "components golden: orchestrator.abi_version");
        const ehc::ComponentEntry* gg = r.file.Find("ggml-translate");
        TEST_CHECK(gg != nullptr && gg->abi_version == 1, "components golden: ggml-translate abi");
        TEST_CHECK(!gg->raw_json.empty() && gg->raw_json.find("engine_version") != std::string::npos,
                   "components golden: entry raw preserved (merge passthrough)");
    }

    // ---- 2. Unknown fields + unknown components preserved ----
    {
        const char* doc =
            "{\"schema_version\":1,\"top_unknown\":1,\"components\":{"
            "  \"orchestrator\": {\"version\":\"0.10.1\",\"abi_version\":2,\"future\":\"x\"},"
            "  \"ct2\": {\"version\":\"0.9.0\"}}}";
        const auto r = ehc::ParseComponentsJson(doc);
        TEST_CHECK(r.status == LoadStatus::Ok, "components unknown: Ok");
        TEST_CHECK(r.file.components.size() == 2, "components unknown: ct2 entry preserved");
        TEST_CHECK(r.file.Find("ct2")->version == "0.9.0", "components unknown: ct2 value");
    }

    // ---- 3. Fail-closed cases ----
    {
        TEST_CHECK(ehc::ParseComponentsJson("{\"components\":{}}").status == LoadStatus::SchemaVersion,
                   "components no schema_version -> reject");
        TEST_CHECK(ehc::ParseComponentsJson("{\"schema_version\":2,\"components\":{}}").status ==
                       LoadStatus::SchemaVersion,
                   "components schema_version=2 -> reject");
        TEST_CHECK(ehc::ParseComponentsJson("{\"schema_version\":1}").status == LoadStatus::Malformed,
                   "components missing components{} -> Malformed");
        TEST_CHECK(ehc::ParseComponentsJson(
                       "{\"schema_version\":1,\"components\":{\"a\":\"b\"}}").status ==
                       LoadStatus::Malformed,
                   "components string entry -> Malformed");
        TEST_CHECK(ehc::ParseComponentsJson(
                       "{\"schema_version\":1,\"components\":{\"a\":{}}").status ==
                       LoadStatus::NotJson,
                   "components unbalanced doc -> NotJson");
        TEST_CHECK(ehc::ParseComponentsJson(
                       "{\"schema_version\":1,\"components\":{\"a\":{\"version\":5}}}").status ==
                       LoadStatus::Malformed,
                   "components numeric version -> Malformed");
        TEST_CHECK(ehc::ParseComponentsJson(
                       "{\"schema_version\":1,\"components\":{\"a\":{\"abi_version\":\"1\"}}}").status ==
                       LoadStatus::Malformed,
                   "components quoted abi -> Malformed");
        TEST_CHECK(ehc::ParseComponentsJson(
                       "{\"schema_version\":1,\"components\":{\"a\":{\"note\":\"x\"}}}").status ==
                       LoadStatus::Malformed,
                   "components entry with neither field -> Malformed");
        // Entry with only version is legal (per-component optionality).
        TEST_CHECK(ehc::ParseComponentsJson(
                       "{\"schema_version\":1,\"components\":{\"a\":{\"version\":\"0.1\"}}}").status ==
                       LoadStatus::Ok,
                   "components version-only entry legal");
        // Entry with only abi_version is legal.
        TEST_CHECK(ehc::ParseComponentsJson(
                       "{\"schema_version\":1,\"components\":{\"a\":{\"abi_version\":3}}}").status ==
                       LoadStatus::Ok,
                   "components abi-only entry legal");
    }

    // ---- 4. Decision rule A (plan §V2-8.1, user decision A) ----
    {
        // Rule A-1: file ABSENT (empty components map) -> ALWAYS replace.
        ehc::ComponentsFile absent;
        const ehc::BundleComponent bundle{"0.10.1", 2};
        TEST_CHECK(ehc::NeedsOrchestratorReplace(absent, bundle) == R::Replace,
                   "rule A-1: absent file -> replace (no version compare)");
        // Equal version + equal abi -> keep (A-2).
        const auto same = ehc::ParseComponentsJson(
            "{\"schema_version\":1,\"components\":{\"orchestrator\":{\"version\":\"0.10.1\",\"abi_version\":2}}}");
        TEST_CHECK(same.status == LoadStatus::Ok, "rule A: same parses");
        TEST_CHECK(ehc::NeedsOrchestratorReplace(same.file, bundle) == R::Keep,
                   "rule A-2: same version+abi -> keep");
        // Missing orchestrator ENTRY with other entries present -> replace.
        const auto noentry = ehc::ParseComponentsJson(
            "{\"schema_version\":1,\"components\":{\"ggml-translate\":{\"version\":\"0.10.1\",\"abi_version\":1}}}");
        TEST_CHECK(ehc::NeedsOrchestratorReplace(noentry.file, bundle) == R::Replace,
                   "rule A-1: absent entry -> replace");
        // Lower installed abi -> replace; higher installed abi -> keep.
        const auto older = ehc::ParseComponentsJson(
            "{\"schema_version\":1,\"components\":{\"orchestrator\":{\"version\":\"0.10.1\",\"abi_version\":1}}}");
        TEST_CHECK(ehc::NeedsOrchestratorReplace(older.file, bundle) == R::Replace,
                   "rule A-2: installed abi lower -> replace");
        const auto newer = ehc::ParseComponentsJson(
            "{\"schema_version\":1,\"components\":{\"orchestrator\":{\"version\":\"0.10.1\",\"abi_version\":3}}}");
        TEST_CHECK(ehc::NeedsOrchestratorReplace(newer.file, bundle) == R::Keep,
                   "rule A-2: installed abi higher -> keep");
        // Same abi, installed version older -> replace; newer -> keep.
        const auto volder = ehc::ParseComponentsJson(
            "{\"schema_version\":1,\"components\":{\"orchestrator\":{\"version\":\"0.10.0\",\"abi_version\":2}}}");
        TEST_CHECK(ehc::NeedsOrchestratorReplace(volder.file, bundle) == R::Replace,
                   "rule A-2: installed version older -> replace");
        const auto vnewer = ehc::ParseComponentsJson(
            "{\"schema_version\":1,\"components\":{\"orchestrator\":{\"version\":\"0.10.2\",\"abi_version\":2}}}");
        TEST_CHECK(ehc::NeedsOrchestratorReplace(vnewer.file, bundle) == R::Keep,
                   "rule A-2: installed version newer -> keep");
        // Version-only entries compare on version; abi-only entries compare on abi.
        const ehc::BundleComponent vbundle{"0.10.5", 0};
        const auto vonly = ehc::ParseComponentsJson(
            "{\"schema_version\":1,\"components\":{\"orchestrator\":{\"version\":\"0.10.4\"}}}");
        TEST_CHECK(ehc::NeedsOrchestratorReplace(vonly.file, vbundle) == R::Replace,
                   "rule A: version-only compare, older -> replace");
        // Missing abi on the entry + bundle abi set -> replace (abi must match).
        const auto noabi = ehc::ParseComponentsJson(
            "{\"schema_version\":1,\"components\":{\"orchestrator\":{\"version\":\"0.10.1\"}}}");
        TEST_CHECK(ehc::NeedsOrchestratorReplace(noabi.file, bundle) == R::Replace,
                   "rule A: entry abi missing vs bundle abi -> replace");
        // NeedsComponentReplace generic name path. `same` carries ONLY the
        // orchestrator entry, so "ggml-translate" is ABSENT here — the
        // expected answer is Replace (rule A-1), not Keep.
        const ehc::BundleComponent wbundle{"0.10.1", 1};
        TEST_CHECK(ehc::NeedsComponentReplace(same.file, "ggml-translate", wbundle) == R::Replace,
                   "rule A: generic component name absent -> replace");
        TEST_CHECK(ehc::NeedsComponentReplace(same.file, "no-such", wbundle) == R::Replace,
                   "rule A: generic component name absent -> replace (2nd name)");
        // A Keep path for a NON-orchestrator name needs that entry present.
        const auto two = ehc::ParseComponentsJson(
            "{\"schema_version\":1,\"components\":{"
            "\"orchestrator\":{\"version\":\"0.10.1\",\"abi_version\":2},"
            "\"ggml-translate\":{\"version\":\"0.10.1\",\"abi_version\":1}}}");
        TEST_CHECK(ehc::NeedsComponentReplace(two.file, "ggml-translate", wbundle) == R::Keep,
                   "rule A: generic component name keep path (entry present)");
    }

    // ---- 5. Partial-update merge (design §6.3) ----
    {
        const auto cur = ehc::ParseComponentsJson(
            "{\"schema_version\":1,\"components\":{"
            "  \"orchestrator\":{\"version\":\"0.9.9\",\"abi_version\":2},"
            "  \"ggml-translate\":{\"version\":\"0.9.9\",\"abi_version\":1,\"engine\":\"llama.cpp\"},"
            "  \"ct2\":{\"version\":\"1.2.3\"}}}");
        TEST_CHECK(cur.status == LoadStatus::Ok, "merge: current parses");
        std::map<std::string, std::string> updates;
        updates["orchestrator"] = "{\"version\":\"0.10.1\",\"abi_version\":2}";
        updates["ggml-translate"] =
            "{\"version\":\"0.10.1\",\"abi_version\":1,\"engine\":\"llama.cpp\",\"engine_version\":\"b6099\"}";
        std::string out;
        TEST_CHECK(ehc::SerializeWithComponents(cur.file, updates, out), "merge: serialize ok");
        // The merged document must re-parse...
        const auto merged = ehc::ParseComponentsJson(out);
        TEST_CHECK(merged.status == LoadStatus::Ok, "merge: output re-parses");
        TEST_CHECK(merged.file.components.size() == 3, "merge: all three entries present");
        // ...updated entries carry the new values...
        TEST_CHECK(merged.file.Find("orchestrator")->version == "0.10.1",
                   "merge: orchestrator updated");
        TEST_CHECK(merged.file.Find("ggml-translate")->abi_version == 1,
                   "merge: ggml-translate updated");
        // ...and the untouched ct2 entry SURVIVES the roundtrip (§6.3 core
        // requirement: other installers' entries are preserved verbatim).
        TEST_CHECK(merged.file.Find("ct2") != nullptr && merged.file.Find("ct2")->version == "1.2.3",
                   "merge: untouched ct2 entry preserved");
        // Unknown top-level fields do not survive (schema is canonical) but
        // unknown fields INSIDE preserved entries do (raw passthrough).
        // Invalid update payload refuses the whole write (no half-merge).
        std::map<std::string, std::string> bad_updates;
        bad_updates["orchestrator"] = "{\"broken\":";
        std::string out_bad;
        TEST_CHECK(!ehc::SerializeWithComponents(cur.file, bad_updates, out_bad),
                   "merge: invalid payload refused");
        TEST_CHECK(out_bad.empty(), "merge: refused write produces no output");
        // Empty updates = pure copy-through.
        std::map<std::string, std::string> no_updates;
        std::string out_copy;
        TEST_CHECK(ehc::SerializeWithComponents(cur.file, no_updates, out_copy), "merge: copy ok");
        const auto copy = ehc::ParseComponentsJson(out_copy);
        TEST_CHECK(copy.status == LoadStatus::Ok && copy.file.components.size() == 3,
                   "merge: copy roundtrip intact");
    }

    // ---- 6. Default loader contract ----
    {
        const auto dir = ehc::DefaultEngineDir();
        if (!dir.empty()) {
            const auto r = ehc::LoadDefaultComponents();
            TEST_CHECK(r.status == LoadStatus::Ok || r.status == LoadStatus::Missing,
                       "components default loader: Ok-or-Missing contract");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Engine-host components parser tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Engine-host components parser tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
