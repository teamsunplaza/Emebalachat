// ---------------------------------------------------------------------------
// REQ-048 R2 (user feedback round 2): worker pinned-path absolutization, tray
// pick grace, registry BOM tolerance.
//
//  (a) BOM tolerance — FUNCTIONAL: ParseRegistryJson must accept the UTF-8
//      BOM that setup.iss writes into registry.json (a BOM used to reject the
//      whole document, downgrading user-model resolution to the pinned
//      fallback on every real install).
//  (b) worker pinned-path — STRUCTURAL (ggml_translate_worker.cpp is the
//      worker-process TU, not linked here): the pinned branches must join
//      models_dir into an ABSOLUTE path; the pre-R2 bare-filename return made
//      the load resolve against the worker's cwd and answered model_missing
//      for a present bundled model (the "local engine never serves" defect).
//  (c) tray pick grace — STRUCTURAL (main.cpp pins): selecting the local
//      engine with components missing must converge on the same bounded grace
//      re-check as the boot block, not an immediate modal.
// ---------------------------------------------------------------------------

void TestReq048R2ServingPath() {
    std::cout << "[RUN] Testing REQ-048 R2 serving-path pins..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) BOM tolerance: the installer's registry.json carries a UTF-8 BOM
    {
        const std::string bom = "\xEF\xBB\xBF";
        const std::string doc = bom +
            "{\"schema_version\":1,\"models\":[{\"id\":\"user-x\",\"family\":\"ggml-translate\","
            "\"files\":[\"x.gguf\"],\"origin\":\"user\"}]}";
        const auto parsed = engine_host_registry::ParseRegistryJson(doc);
        TEST_CHECK(parsed.status == engine_host_registry::LoadStatus::Ok,
                   "REQ-048 R2 (a): BOM-prefixed registry parses Ok");
        if (parsed.status == engine_host_registry::LoadStatus::Ok) {
            const auto* m = parsed.registry.FindModel("user-x");
            TEST_CHECK(m != nullptr && m->origin == "user",
                       "REQ-048 R2 (a): BOM-prefixed registry resolves the model entry");
        }
        // And the no-BOM shape must stay Ok (no regression).
        const auto plain = engine_host_registry::ParseRegistryJson(doc.substr(3));
        TEST_CHECK(plain.status == engine_host_registry::LoadStatus::Ok,
                   "REQ-048 R2 (a): plain registry stays Ok");
    }

    // ---- (b) worker pinned-path pins --------------------------------------
    const std::filesystem::path worker_cpp = ResolveRepoFile(L"src\\ggml_translate_worker.cpp");
    if (worker_cpp.empty()) {
        std::cout << "[SKIP] src/ggml_translate_worker.cpp not resolvable; REQ-048 R2 (b) pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(worker_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        // Positive: the absolutizing helper exists and is used by BOTH pinned
        // branches (definition + empty-id return + fallback return = 3+).
        std::size_t helper_uses = 0;
        for (std::size_t pos = 0;
             (pos = src.find("pinned_abs", pos)) != std::string::npos;
             pos += 10) {
            ++helper_uses;
        }
        TEST_CHECK(helper_uses >= 3,
                   "REQ-048 R2 (b): pinned_abs helper defined and used by both pinned branches");
        TEST_CHECK(src.find("return models_dir + \"\\\\\" + pinned_name;") != std::string::npos,
                   "REQ-048 R2 (b): pinned branch returns the ABSOLUTE models_dir-pinned join");
        // Negative: the pre-R2 bare-filename pinned return is gone.
        TEST_CHECK(src.find("return std::string(emebalachat::kPinnedModelFilename.begin(),\n"
                            "                           emebalachat::kPinnedModelFilename.end());") ==
                       std::string::npos,
                   "REQ-048 R2 (b): bare-filename pinned return removed");
    }

    // ---- (c) tray pick grace pins -----------------------------------------
    const std::filesystem::path main_cpp = ResolveRepoFile(L"src\\main.cpp");
    if (main_cpp.empty()) {
        std::cout << "[SKIP] src/main.cpp not resolvable; REQ-048 R2 (c) pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(main_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        TEST_CHECK(src.find("converging on the grace re-check") != std::string::npos,
                   "REQ-048 R2 (c): tray local pick converges on the grace re-check");
        TEST_CHECK(src.find("MAIN/on_select_engine/004") != std::string::npos &&
                   src.find("WaitForComponentsPresent") != std::string::npos,
                   "REQ-048 R2 (c): tray pick grace uses the bounded wait helper");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-048 R2 serving-path checks passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-048 R2 serving-path checks: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
