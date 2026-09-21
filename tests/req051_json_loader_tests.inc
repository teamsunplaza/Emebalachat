// ---------------------------------------------------------------------------
// REQ-051 (handoff §6 bullet 2): JSON loader leniency audit — the 9cf0d40
// config.cpp BOM disaster lesson is "asymmetry between what the writer saves
// and what the reader accepts comes back as self-destruction". This suite
// pins the BOM tolerance for every host-side JSON reader route that is
// linkable here (src/config.cpp itself carries the 9cf0d40 §8 regression
// cases; frozen worker_protocol.hpp pipe frames are out of scope — no BOM
// concept on the wire).
//
// Coverage:
//   (a) pure logic: the shared engine_host_json::SkipUtf8Bom / StartsWithUtf8Bom
//       helpers over variants — no BOM, BOM, BOM+leading whitespace, empty
//       input, partial BOM bytes, look-alike third byte, double BOM
//       (exactly ONE BOM is ever skipped; a double-BOM document stays
//       malformed and is still rejected by the frozen parser).
//   (b) runtime routes: ReadTextFileUtf8 temp-file round-trip, the public
//       in-memory engine_host_manifest::ParseManifestJson, and the boot
//       enginehost::LoadUserModelIdFromConfig through a temp lad root —
//       plus fail-closed proof that BOM tolerance never weakens structural
//       rejection (missing pinned{} stays Malformed, garbage stays NotJson,
//       the REQ-046 C1 gate still applies under a BOM).
//   (c) structural pins (ResolveRepoFile precedent): each owned reader route
//       passes its input through the SHARED helper in
//       src/engine_host_json_util.hpp — a single definition, no re-inlined
//       byte checks — so the tolerance cannot silently fork again.
//
// Staged as an .inc next to tests/run_tests.cpp; registered near the end of
// main() (after TestReq050HfAddDialog), per the end-of-file pattern.
// ---------------------------------------------------------------------------

void TestReq051JsonBomSkipHelper() {
    std::cout << "[RUN] Testing REQ-051 shared UTF-8 BOM skip helper..." << std::endl;
    const int failures_before = g_failed_count;
    namespace ehj = emebalachat::engine_host_json;

    const std::string kBom = "\xEF\xBB\xBF"; // UTF-8 BOM, EF BB BF (idem 9cf0d40 §8)

    // No BOM: the view is returned unchanged (same bytes, same length).
    const std::string plain = "{\"a\":1}";
    {
        const std::string_view v = ehj::SkipUtf8Bom(plain);
        TEST_CHECK(v.data() == plain.data() && v.size() == plain.size(),
                   "REQ-051: no-BOM input passes through unchanged");
        TEST_CHECK(!ehj::StartsWithUtf8Bom(plain),
                   "REQ-051: StartsWithUtf8Bom false on plain JSON");
    }

    // BOM only: skips to empty. The helper never dereferences past the end.
    {
        const std::string bom_only = kBom;
        const std::string_view v = ehj::SkipUtf8Bom(bom_only);
        TEST_CHECK(v.empty(), "REQ-051: BOM-only input skips to an empty view");
        TEST_CHECK(ehj::StartsWithUtf8Bom(bom_only),
                   "REQ-051: StartsWithUtf8Bom true on a lone BOM");
    }

    // BOM + document: exactly the 3 BOM bytes are removed.
    {
        const std::string bom_doc = kBom + plain;
        const std::string_view v = ehj::SkipUtf8Bom(bom_doc);
        TEST_CHECK(v == plain, "REQ-051: BOM + document -> document");
        // The skip is a view: the source buffer is never mutated.
        TEST_CHECK(bom_doc.size() == plain.size() + 3,
                   "REQ-051: skip leaves the source string untouched");
    }

    // BOM + benign prefix whitespace: the helper strips the BOM, the frozen
    // parser's own whitespace skip handles the rest (integration point).
    {
        const std::string ws_doc = kBom + " \t\r\n{\"a\":1}";
        enginehost::JsonPairs pairs;
        TEST_CHECK(enginehost::JsonParseObject(ehj::SkipUtf8Bom(ws_doc), pairs),
                   "REQ-051: BOM + leading whitespace + document parses");
    }

    // Empty input: pass-through, no crash.
    {
        const std::string_view v = ehj::SkipUtf8Bom("");
        TEST_CHECK(v.empty(), "REQ-051: empty input -> empty view");
        TEST_CHECK(!ehj::StartsWithUtf8Bom(""), "REQ-051: empty input is not a BOM");
    }

    // Partial BOM bytes: a truncated BOM is NOT a BOM and must pass through
    // (stripping it would silently eat user bytes).
    {
        const std::string p1 = "\xEF";
        const std::string p2 = "\xEF\xBB";
        const std::string p3 = "\xEF\xBB{"; // partial BOM immediately followed by '{'
        TEST_CHECK(ehj::SkipUtf8Bom(p1) == p1, "REQ-051: single 0xEF passes through");
        TEST_CHECK(ehj::SkipUtf8Bom(p2) == p2, "REQ-051: 0xEF 0xBB passes through");
        TEST_CHECK(ehj::SkipUtf8Bom(p3) == p3,
                   "REQ-051: partial BOM + '{' is NOT stripped (no silent byte eat)");
        TEST_CHECK(!ehj::StartsWithUtf8Bom(p2), "REQ-051: partial BOM is not a BOM");
    }

    // Look-alike third byte (0xBE instead of 0xBF): not a BOM.
    {
        const std::string fake = "\xEF\xBB\xBE{";
        TEST_CHECK(ehj::SkipUtf8Bom(fake) == fake,
                   "REQ-051: EF BB BE look-alike is not stripped");
    }

    // Double BOM: exactly ONE BOM is skipped; the document stays malformed
    // and the frozen parser still rejects it (tolerance never becomes
    // acceptance).
    {
        const std::string dbl = kBom + kBom + "{}";
        const std::string_view v = ehj::SkipUtf8Bom(dbl);
        TEST_CHECK(v.size() == 5 && v[0] == '\xEF',
                   "REQ-051: double BOM -> exactly one BOM skipped");
        enginehost::JsonPairs pairs;
        TEST_CHECK(!enginehost::JsonParseObject(v, pairs),
                   "REQ-051: double-BOM document is still rejected (fail-closed)");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-051 shared UTF-8 BOM skip helper passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-051 shared UTF-8 BOM skip helper: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestReq051JsonReaderBomRoutes() {
    std::cout << "[RUN] Testing REQ-051 JSON reader BOM routes (manifest/config/file)..."
              << std::endl;
    const int failures_before = g_failed_count;
    namespace ehj = emebalachat::engine_host_json;
    namespace ehm = emebalachat::engine_host_manifest;
    namespace fs = std::filesystem;

    const std::string kBom = "\xEF\xBB\xBF";
    std::error_code ec;

    // ---- (a) ReadTextFileUtf8: file-layer BOM strip (the original REQ-043
    // tolerance, now routed through the shared predicate — byte-identical).
    {
        const fs::path dir = fs::temp_directory_path(ec) / L"emebala_req051_json_file";
        if (ec) {
            TEST_CHECK(false, "REQ-051: temp dir unavailable");
        } else {
            fs::remove_all(dir, ec);
            ec.clear();
            fs::create_directories(dir, ec);
            if (ec) {
                TEST_CHECK(false, "REQ-051: temp dir creation failed");
            } else {
                const std::string body = "{\"schema_version\":1,\"pinned\":{}}";
                std::string out;

                // BOM-prefixed file -> Ok, BOM stripped.
                {
                    std::ofstream f(dir / L"bom.json", std::ios::binary | std::ios::trunc);
                    f << kBom << body;
                }
                TEST_CHECK(ehj::ReadTextFileUtf8(dir / L"bom.json", out) == ehj::FileReadOutcome::Ok,
                           "REQ-051: BOM-prefixed file reads Ok");
                TEST_CHECK(out == body, "REQ-051: file read strips exactly the BOM");

                // Plain file -> Ok, byte-identical.
                {
                    std::ofstream f(dir / L"plain.json", std::ios::binary | std::ios::trunc);
                    f << body;
                }
                TEST_CHECK(ehj::ReadTextFileUtf8(dir / L"plain.json", out) == ehj::FileReadOutcome::Ok,
                           "REQ-051: plain file reads Ok");
                TEST_CHECK(out == body, "REQ-051: plain file content unchanged");

                // Partial-BOM prefix is NOT stripped (ReadTextFileUtf8 only
                // removes a full EF BB BF; the parser stays responsible for
                // rejecting the malformed remainder).
                {
                    std::ofstream f(dir / L"partial.json", std::ios::binary | std::ios::trunc);
                    f << "\xEF\xBB" << body;
                }
                TEST_CHECK(ehj::ReadTextFileUtf8(dir / L"partial.json", out) == ehj::FileReadOutcome::Ok,
                           "REQ-051: partial-BOM file reads Ok");
                TEST_CHECK(out.size() == body.size() + 2,
                           "REQ-051: partial-BOM prefix preserved (no byte eat)");

                // Empty file -> Ok, empty content.
                std::ofstream(dir / L"empty.json", std::ios::binary | std::ios::trunc);
                TEST_CHECK(ehj::ReadTextFileUtf8(dir / L"empty.json", out) == ehj::FileReadOutcome::Ok,
                           "REQ-051: empty file reads Ok");
                TEST_CHECK(out.empty(), "REQ-051: empty file -> empty content");

                // Missing file -> Missing.
                TEST_CHECK(ehj::ReadTextFileUtf8(dir / L"nope.json", out) == ehj::FileReadOutcome::Missing,
                           "REQ-051: missing file -> Missing");
            }
            fs::remove_all(dir, ec);
            ec.clear();
        }
    }

    // ---- (b) engine_host_manifest::ParseManifestJson: the public in-memory
    // reader entry point must accept a BOM-prefixed document (before the
    // REQ-051 fix only the file-read path was tolerant).
    const std::string good_doc = "{\"schema_version\":1,\"pinned\":{}}";
    TEST_CHECK(ehm::ParseManifestJson(kBom + good_doc).status == ehm::LoadStatus::Ok,
               "REQ-051: BOM-prefixed manifest document parses Ok");
    TEST_CHECK(ehm::ParseManifestJson(kBom + good_doc + "\n").status == ehm::LoadStatus::Ok,
               "REQ-051: BOM + trailing newline manifest parses Ok");
    TEST_CHECK(ehm::ParseManifestJson(good_doc).status == ehm::LoadStatus::Ok,
               "REQ-051: no-BOM manifest still parses Ok (non-regression)");
    // Fail-closed proof: the BOM skip never weakens structural rejection.
    TEST_CHECK(ehm::ParseManifestJson(kBom + "{\"schema_version\":1}").status ==
                   ehm::LoadStatus::Malformed,
               "REQ-051: BOM + missing pinned{} stays Malformed");
    TEST_CHECK(ehm::ParseManifestJson(kBom + "garbage").status == ehm::LoadStatus::NotJson,
               "REQ-051: BOM + garbage stays NotJson");

    // ---- (c) enginehost::LoadUserModelIdFromConfig: boot config read through
    // a temp lad root (TestReq046HostUserModelIdGate fixture precedent).
    {
        const fs::path root = fs::temp_directory_path(ec) / L"emebala_req051_json_cfg";
        if (ec) {
            TEST_CHECK(false, "REQ-051: temp dir unavailable");
        } else {
            fs::remove_all(root, ec);
            ec.clear();
            fs::create_directories(root / L"Emebalachat", ec);
            if (ec) {
                TEST_CHECK(false, "REQ-051: temp config dir creation failed");
            } else {
                const std::wstring lad = root.wstring();
                auto write_config = [&](const std::string& body) {
                    std::ofstream out(root / L"Emebalachat" / L"config.json",
                                      std::ios::binary | std::ios::trunc);
                    if (!out.is_open()) {
                        TEST_CHECK(false, "REQ-051: temp config.json write failed");
                        return;
                    }
                    out << body;
                };

                // BOM + user_gguf + id -> the id is cached (BOM must not send
                // the host down the pinned fallback).
                write_config(kBom + "{\"engine_type\": \"user_gguf\", \"user_model_id\": \"user-bom\"}");
                TEST_CHECK(enginehost::LoadUserModelIdFromConfig(lad) == "user-bom",
                           "REQ-051: BOM-prefixed config.json -> user_model_id read");
                // BOM + trailing newline, app-save shape.
                write_config(kBom +
                             "{\"engine_type\": \"user_gguf\", \"user_model_id\": \"user-bom\"}\n");
                TEST_CHECK(enginehost::LoadUserModelIdFromConfig(lad) == "user-bom",
                           "REQ-051: BOM + trailing newline config.json -> id read");
                // BOM + engine_type=local: the REQ-046 C1 gate still applies.
                write_config(kBom + "{\"engine_type\": \"local\", \"user_model_id\": \"user-bom\"}");
                TEST_CHECK(enginehost::LoadUserModelIdFromConfig(lad).empty(),
                           "REQ-051: BOM + engine_type=local -> C1 gate intact (\"\" -> pinned)");
            }
            fs::remove_all(root, ec);
            ec.clear();
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-051 JSON reader BOM routes passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-051 JSON reader BOM routes: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestReq051JsonLoaderStructuralPins() {
    std::cout << "[RUN] Testing REQ-051 JSON loader structural pins (shared helper routing)..."
              << std::endl;
    const int failures_before = g_failed_count;

    // ResolveRepoFile precedent (TestReq048CloudFailureSignaling): the source
    // is UTF-8; every needle below is pure ASCII, so byte-level find() is
    // exact.
    const std::filesystem::path ju_path = ResolveRepoFile(L"src\\engine_host_json_util.hpp");
    TEST_CHECK(!ju_path.empty(), "REQ-051: src/engine_host_json_util.hpp located via ResolveRepoFile");
    if (ju_path.empty()) {
        std::cout << "[FAIL] REQ-051: engine_host_json_util.hpp not resolvable; structural pins failed."
                  << std::endl;
        return;
    }
    std::ifstream ju_in(ju_path, std::ios::binary);
    const std::string ju_src((std::istreambuf_iterator<char>(ju_in)),
                             std::istreambuf_iterator<char>());
    TEST_CHECK(!ju_src.empty(), "REQ-051: engine_host_json_util.hpp non-empty after read");

    // The shared helpers exist with REQ-051 provenance, defined exactly once
    // each — the single-definition rule is what prevents the tolerance from
    // silently forking again (the audit's converge step). Needles stop at the
    // parameter TYPE, no closing paren: the definitions name their parameters
    // ("(std::string_view text)"), and a full-signature needle would not match.
    TEST_CHECK(ju_src.find("inline bool StartsWithUtf8Bom(std::string_view") != std::string::npos,
               "REQ-051: shared StartsWithUtf8Bom defined in engine_host_json_util.hpp");
    TEST_CHECK(ju_src.find("inline std::string_view SkipUtf8Bom(std::string_view") != std::string::npos,
               "REQ-051: shared SkipUtf8Bom defined in engine_host_json_util.hpp");
    TEST_CHECK(ju_src.find("REQ-051") != std::string::npos,
               "REQ-051: BOM helper carries the REQ-051 provenance tag");
    {
        const std::size_t first = ju_src.find("inline bool StartsWithUtf8Bom");
        TEST_CHECK(first != std::string::npos &&
                       ju_src.find("inline bool StartsWithUtf8Bom", first + 1) == std::string::npos,
                   "REQ-051: StartsWithUtf8Bom has exactly one definition");
        const std::size_t first_skip = ju_src.find("inline std::string_view SkipUtf8Bom");
        TEST_CHECK(first_skip != std::string::npos &&
                       ju_src.find("inline std::string_view SkipUtf8Bom", first_skip + 1) ==
                           std::string::npos,
                   "REQ-051: SkipUtf8Bom has exactly one definition");
    }

    // ReadTextFileUtf8 routes through the shared predicate — and the old
    // re-inlined byte check is gone from its body. (Anchor on the definition
    // signature: the header's top comment also mentions ReadTextFileUtf8.)
    {
        const std::size_t fn = ju_src.find("FileReadOutcome ReadTextFileUtf8");
        TEST_CHECK(fn != std::string::npos, "REQ-051: ReadTextFileUtf8 definition present");
        const std::string window = ju_src.substr(fn, 2000);
        TEST_CHECK(window.find("StartsWithUtf8Bom") != std::string::npos,
                   "REQ-051: ReadTextFileUtf8 routes through the shared BOM predicate");
        TEST_CHECK(window.find("data[0]) == 0xEF") == std::string::npos,
                   "REQ-051: no re-inlined BOM byte check left in ReadTextFileUtf8");
    }

    // Every owned reader entry point routes its input through the helper.
    const std::filesystem::path mf_path = ResolveRepoFile(L"src\\engine_host_manifest.cpp");
    TEST_CHECK(!mf_path.empty(), "REQ-051: src/engine_host_manifest.cpp located via ResolveRepoFile");
    if (!mf_path.empty()) {
        std::ifstream mf_in(mf_path, std::ios::binary);
        const std::string mf_src((std::istreambuf_iterator<char>(mf_in)),
                                 std::istreambuf_iterator<char>());
        const std::size_t fn = mf_src.find("ParseManifestJson");
        TEST_CHECK(fn != std::string::npos, "REQ-051: ParseManifestJson present");
        if (fn != std::string::npos) {
            const std::string window = mf_src.substr(fn, 800);
            TEST_CHECK(window.find("SkipUtf8Bom") != std::string::npos,
                       "REQ-051: ParseManifestJson skips the BOM via the shared helper");
            TEST_CHECK(window.find("REQ-051") != std::string::npos,
                       "REQ-051: ParseManifestJson BOM skip carries the provenance tag");
        }
    }
    const std::filesystem::path cr_path = ResolveRepoFile(L"src\\engine_host_config_reader.cpp");
    TEST_CHECK(!cr_path.empty(), "REQ-051: src/engine_host_config_reader.cpp located via ResolveRepoFile");
    if (!cr_path.empty()) {
        std::ifstream cr_in(cr_path, std::ios::binary);
        const std::string cr_src((std::istreambuf_iterator<char>(cr_in)),
                                 std::istreambuf_iterator<char>());
        const std::size_t fn = cr_src.find("LoadUserModelIdFromConfig");
        TEST_CHECK(fn != std::string::npos, "REQ-051: LoadUserModelIdFromConfig present");
        if (fn != std::string::npos) {
            const std::string window = cr_src.substr(fn, 1400);
            TEST_CHECK(window.find("SkipUtf8Bom") != std::string::npos,
                       "REQ-051: LoadUserModelIdFromConfig skips the BOM via the shared helper");
        }
    }

    // Service pin (not owned by this scope): the 9cf0d40 config.cpp BOM fix
    // this audit generalizes must not be regressed by any parallel wave.
    const std::filesystem::path cfg_path = ResolveRepoFile(L"src\\config.cpp");
    if (!cfg_path.empty()) {
        std::ifstream cfg_in(cfg_path, std::ios::binary);
        const std::string cfg_src((std::istreambuf_iterator<char>(cfg_in)),
                                  std::istreambuf_iterator<char>());
        TEST_CHECK(cfg_src.find("REQ-051-1") != std::string::npos,
                   "REQ-051: src/config.cpp keeps its 9cf0d40 BOM-skip (REQ-051-1) intact");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-051 JSON loader structural pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-051 JSON loader structural pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
