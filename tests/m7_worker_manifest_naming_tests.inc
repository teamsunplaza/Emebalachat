// ---------------------------------------------------------------------------
// M7 A-1 (session 260922_0001): worker.<family>.manifest naming-scheme suite
// (TestM7WorkerManifestNaming). Pins the per-family manifest contract that
// lets ggml-translate and ggml-asr coexist in the ONE shared engine store
// without overwriting each other's manifest (DEC-007: hyphenated family
// strings verbatim). Covers, per the A-1 completion criteria:
//   (a) per-family path/name resolution + scheme recognition,
//   (b) multi-manifest enumeration and load (schema UNCHANGED — the frozen
//       worker_protocol.hpp parser is the loader),
//   (c) legacy bare worker.manifest read fallback (new scheme takes priority
//       when both exist; writes only ever use the new scheme),
//   (d) non-overwrite coexistence (functional: distinct family bytes both
//       survive side by side; structural: CMake / setup.iss / bootstrap ship
//       and read the new name, and the frozen schema header stays naming-
//       scheme-free).
// Staging uses a temp directory (never %LOCALAPPDATA%\Emebala); cleaned on
// exit. Structural pins follow the TestReq044VerbatimSync source-text
// precedent via ResolveRepoFile.
// ---------------------------------------------------------------------------

void TestM7WorkerManifestNaming() {
    std::cout << "[RUN] Testing M7 A-1 per-family worker manifest naming..." << std::endl;
    const int failures_before = g_failed_count;
    namespace paths = emebalachat::enginehost::paths;
    namespace wp = emebalachat::workerproto;
    namespace bs = emebalachat::engine_host_bootstrap;

    // ---- (a) per-family name construction + scheme recognition --------------
    {
        TEST_CHECK(paths::WorkerManifestName(L"ggml-translate") ==
                       L"worker.ggml-translate.manifest",
                   "naming: ggml-translate family builds the per-family name");
        TEST_CHECK(paths::WorkerManifestName(L"ggml-asr") == L"worker.ggml-asr.manifest",
                   "naming: ggml-asr family builds the per-family name (DEC-007 hyphen verbatim)");
        TEST_CHECK(paths::WorkerManifestName(std::wstring{}) == paths::kWorkerManifestLegacy,
                   "naming: empty family degrades to the legacy bare name (defensive)");

        TEST_CHECK(paths::IsWorkerManifestName(paths::kWorkerManifestLegacy),
                   "scheme: legacy worker.manifest is recognized on reads");
        TEST_CHECK(paths::IsWorkerManifestName(L"worker.ggml-translate.manifest"),
                   "scheme: per-family name is recognized");
        TEST_CHECK(!paths::IsWorkerManifestName(L"worker..manifest"),
                   "scheme: empty family segment is NOT a manifest (worker..manifest)");
        TEST_CHECK(!paths::IsWorkerManifestName(L"worker.manifestx"),
                   "scheme: wrong suffix is not a manifest");
        TEST_CHECK(!paths::IsWorkerManifestName(L"registry.json"),
                   "scheme: unrelated store files are not manifests");
        TEST_CHECK(!paths::IsWorkerManifestName(L"worker."),
                   "scheme: prefix alone is not a manifest");
    }

    // Shared staging area under the temp dir — the real store is never touched.
    const std::filesystem::path stage =
        std::filesystem::temp_directory_path() / "emebala_m7_manifest_naming_test";
    std::error_code ec;
    std::filesystem::remove_all(stage, ec);
    std::filesystem::create_directories(stage, ec);
    TEST_CHECK(!ec && std::filesystem::exists(stage), "stage: temp store created");

    // ---- (b)+(d-functional) multi-manifest enumeration and load -------------
    {
        // Two families land in the SAME directory with the per-family scheme —
        // exactly the overwrite hazard the single-file name created. Each file
        // is written from the frozen builder with its own family/ops so the
        // loaded bytes are family-distinct and verifiable.
        wp::WorkerManifest mt = wp::EmbeddedWorkerManifest();
        const std::string mt_bytes = wp::BuildManifestJson(mt); // family ggml-translate

        wp::WorkerManifest ma;
        ma.schema_version = wp::kWorkerSchemaVersion;
        ma.family = "ggml-asr";
        ma.engine = "transcribe.cpp";
        ma.engine_version = "test-pin";
        ma.ops = {"asr"};
        ma.abi_version = 1;
        ma.protocol_min = 1;
        ma.protocol_max = 2;
        const std::string ma_bytes = wp::BuildManifestJson(ma);

        {
            std::ofstream f(stage / "worker.ggml-translate.manifest",
                            std::ios::binary | std::ios::trunc);
            f << mt_bytes;
        }
        {
            std::ofstream f(stage / "worker.ggml-asr.manifest",
                            std::ios::binary | std::ios::trunc);
            f << ma_bytes;
        }
        // Decoys that must NOT be enumerated.
        {
            std::ofstream f(stage / "registry.json", std::ios::binary | std::ios::trunc);
            f << "{}";
        }
        {
            std::ofstream f(stage / "worker.bogus.txt", std::ios::binary | std::ios::trunc);
            f << "not a manifest";
        }

        const std::vector<std::string> listed = bs::EnumerateWorkerManifestsIn(stage);
        TEST_CHECK(listed.size() == 2, "enumerate: exactly the two manifests surface");
        TEST_CHECK(listed.size() == 2 && listed[0] == "worker.ggml-asr.manifest" &&
                       listed[1] == "worker.ggml-translate.manifest",
                   "enumerate: sorted bare filenames, decoys excluded");

        // Load every enumerated manifest through the FROZEN parser (schema
        // unchanged — A-1 touched only file naming/enumeration). Both must
        // parse and carry their own family: neither file overwrote the other.
        int loaded_families = 0;
        for (const std::string& name : listed) {
            std::ifstream in(stage / name, std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            wp::WorkerManifest m;
            if (wp::ParseManifestJson(ss.str(), m) == wp::ManifestStatus::Ok &&
                m.family == (name == "worker.ggml-asr.manifest" ? std::string("ggml-asr")
                                                                : std::string("ggml-translate"))) {
                ++loaded_families;
            }
        }
        TEST_CHECK(loaded_families == 2,
                   "load: both family manifests parse intact side by side (no clobber)");

        // ---- (c) legacy read fallback ----------------------------------------
        TEST_CHECK(bs::ResolveWorkerManifestName(stage, "ggml-translate") ==
                       "worker.ggml-translate.manifest",
                   "fallback: new-scheme file wins when both schemes could apply");
        {
            // Legacy-only store (pre-A-1 install): the bare name must resolve.
            const std::filesystem::path legacy_dir = stage / "legacy_store";
            std::filesystem::create_directories(legacy_dir, ec);
            {
                std::ofstream f(legacy_dir / "worker.manifest",
                                std::ios::binary | std::ios::trunc);
                f << mt_bytes;
            }
            TEST_CHECK(bs::ResolveWorkerManifestName(legacy_dir, "ggml-translate") ==
                           "worker.manifest",
                       "fallback: legacy worker.manifest is accepted when no new-scheme file exists");
            // Absent family entirely -> empty (caller reports the CANONICAL name
            // missing; repair writes the new scheme only).
            TEST_CHECK(bs::ResolveWorkerManifestName(legacy_dir, "ggml-asr").empty(),
                       "fallback: a family with no manifest at all resolves to empty");
        }
    }
    std::filesystem::remove_all(stage, ec);

    // ---- (d-structural) deployment wiring ships the new scheme --------------
    {
        const auto read_src = [](const wchar_t* rel, std::string& out) {
            const std::filesystem::path p = ResolveRepoFile(rel);
            if (p.empty()) return false;
            std::ifstream in(p, std::ios::binary);
            if (!in) return false;
            std::ostringstream ss;
            ss << in.rdbuf();
            out = ss.str();
            return true;
        };

        std::string cmake_src;
        TEST_CHECK(read_src(L"CMakeLists.txt", cmake_src), "pin: CMakeLists.txt readable");
        TEST_CHECK(cmake_src.find("worker.ggml-translate.manifest") != std::string::npos,
                   "pin: CMake generates/deploys the per-family manifest name");
        TEST_CHECK(cmake_src.find("${CMAKE_CURRENT_BINARY_DIR}/worker.manifest") ==
                       std::string::npos,
                   "pin: CMake no longer references the bare legacy artifact name");

        std::string iss_src;
        TEST_CHECK(read_src(L"installer\\setup.iss", iss_src), "pin: setup.iss readable");
        TEST_CHECK(iss_src.find("Source: \"..\\build\\worker.ggml-translate.manifest\"") !=
                       std::string::npos,
                   "pin: installer [Files] sources the per-family manifest artifact");
        TEST_CHECK(iss_src.find("Source: \"..\\build\\worker.manifest\"") == std::string::npos,
                   "pin: installer no longer sources the bare legacy artifact name");
        TEST_CHECK(iss_src.find("WORKER_MANIFEST_FILENAME = 'worker.ggml-translate.manifest'") !=
                       std::string::npos,
                   "pin: installer constant carries the per-family deployed name");

        std::string boot_src;
        TEST_CHECK(read_src(L"src\\engine_host_bootstrap_client.cpp", boot_src),
                   "pin: bootstrap client readable");
        TEST_CHECK(boot_src.find("{RequiredComponent::Root::Engine, \"worker.manifest\"}") ==
                       std::string::npos,
                   "pin: kRequired[] no longer hardcodes a single manifest filename");
        TEST_CHECK(boot_src.find("EnumerateWorkerManifestsIn") != std::string::npos,
                   "pin: the store enumeration seam is implemented in the bootstrap client");
        TEST_CHECK(boot_src.find("ResolveManifestIn") != std::string::npos,
                   "pin: the new-scheme-first/legacy-fallback resolver is wired into the client");

        // The frozen schema header must NOT acquire naming-scheme knowledge —
        // A-1 added the scheme at the path/deployment layer only.
        std::string wp_src;
        TEST_CHECK(read_src(L"src\\worker_protocol.hpp", wp_src), "pin: worker_protocol.hpp readable");
        TEST_CHECK(wp_src.find("worker.ggml-translate.manifest") == std::string::npos &&
                       wp_src.find("WorkerManifestName") == std::string::npos,
                   "pin: frozen worker_protocol.hpp schema layer stays naming-scheme-free");
    }

    std::cout << "  m7 manifest naming: "
              << (g_failed_count - failures_before) << " new failures" << std::endl;
}
