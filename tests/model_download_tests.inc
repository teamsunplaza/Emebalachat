// ---------------------------------------------------------------------------
// REQ-MD (session 260930_0004, decisions.md D9): on-demand bundled-model
// download — headless pins for the pure logic (HF allowlist, cap math with
// the 4 GiB model cap, offer-plan transitions, missing-only predicate,
// registry merge shape, .sha256ok marker round-trip, the SHA-256 pin sync).
// Staged as an .inc next to this runner; registered at the end of main(),
// per the end-of-file pattern.
// ---------------------------------------------------------------------------

void TestModelDownloadLogic() {
    std::cout << "[RUN] Testing REQ-MD model download logic..." << std::endl;
    const int failures_before = g_failed_count;

    namespace md = emebalachat::modeldownloader;
    using updatelogic::CheckDownloadCapCapped;
    using updatelogic::DownloadCapStatus;

    // ---- HF host allowlist (the generalized download seam) -----------------
    TEST_CHECK(md::IsAllowedModelHost("huggingface.co"), "MD: huggingface.co allowed");
    TEST_CHECK(md::IsAllowedModelHost("cdn-lfs.huggingface.co"), "MD: *.huggingface.co allowed");
    TEST_CHECK(md::IsAllowedModelHost("cdn-lfs-us-1.huggingface.co"), "MD: regional CDN allowed");
    TEST_CHECK(md::IsAllowedModelHost("cas-bridge.xethub.hf.co"), "MD: *.hf.co allowed");
    TEST_CHECK(!md::IsAllowedModelHost("huggingface.co.evil.com"), "MD: look-alike refused");
    TEST_CHECK(!md::IsAllowedModelHost("hf.co"), "MD: bare hf.co refused (no dot-label)");
    TEST_CHECK(!md::IsAllowedModelHost("github.com"), "MD: non-HF hosts refused");
    TEST_CHECK(!md::IsAllowedModelHost("api.github.com"), "MD: GitHub API NOT in the model allowlist");
    TEST_CHECK(!md::IsAllowedModelHost(""), "MD: empty host refused");
    // And the update path's allowlist is untouched by the generalization.
    TEST_CHECK(updatelogic::IsAllowedUpdateHost("github.com") &&
                   !updatelogic::IsAllowedUpdateHost("huggingface.co"),
               "MD: the update allowlist stays GitHub-only (default options pin)");

    // ---- cap math with the model's 4 GiB absolute cap ----------------------
    const std::uint64_t kGiB = 1024ull * 1024 * 1024;
    const std::uint64_t kModelCap = 4 * kGiB;
    TEST_CHECK(CheckDownloadCapCapped(0, md::kModelExpectedBytes, md::kModelExpectedBytes,
                                      kModelCap, updatelogic::kDownloadAdvertisedSlack) ==
                   DownloadCapStatus::WithinCap,
               "MD: the 1.91 GB model fits the 4 GiB cap");
    TEST_CHECK(CheckDownloadCapCapped(0, 3 * kGiB, 0, kModelCap, 64ull * 1024 * 1024) ==
                   DownloadCapStatus::WithinCap,
               "MD: a 3 GiB Content-Length still fits the 4 GiB cap");
    TEST_CHECK(CheckDownloadCapCapped(0, 5 * kGiB, 0, kModelCap, 64ull * 1024 * 1024) ==
                   DownloadCapStatus::OverAbsoluteCap,
               "MD: 5 GiB dies at the 4 GiB cap");
    // Advertised cross-check: bound = expected*2 + 64 MiB.
    const std::uint64_t bound = md::kModelExpectedBytes * 2 + 64ull * 1024 * 1024;
    TEST_CHECK(CheckDownloadCapCapped(0, bound + 1, md::kModelExpectedBytes,
                                      kModelCap, 64ull * 1024 * 1024) ==
                   DownloadCapStatus::OverAdvertisedCap,
               "MD: a lying Content-Length beyond 2x+slack fails fast");
    // The update path's own cap is unchanged (2 GiB).
    TEST_CHECK(CheckDownloadCapCapped(0, 3 * kGiB, 0, updatelogic::kMaxDownloadBytes,
                                      updatelogic::kDownloadAdvertisedSlack) ==
                   DownloadCapStatus::OverAbsoluteCap,
               "MD: update-path cap unchanged at 2 GiB");

    // ---- missing-only predicate (models/ prefix gate) ----------------------
    TEST_CHECK(!md::MissingIsModelOnly({}), "MD: empty missing list -> no offer");
    TEST_CHECK(md::MissingIsModelOnly({"models/Hy-MT2-1.8B-Q8_0.gguf"}),
               "MD: model-only missing set qualifies");
    TEST_CHECK(md::MissingIsModelOnly({"models/Hy-MT2-1.8B-Q8_0.gguf", "models/registry.json"}),
               "MD: model + registry missing still qualifies (both fixable)");
    TEST_CHECK(!md::MissingIsModelOnly({"engine/Emebala.Engine.exe"}),
               "MD: engine-binary missing -> the download cannot help");
    TEST_CHECK(!md::MissingIsModelOnly({"models/Hy-MT2-1.8B-Q8_0.gguf", "engine/worker.exe"}),
               "MD: mixed missing set -> old notice path");

    // ---- offer plan transitions --------------------------------------------
    const std::uint64_t kEnough = md::kModelExpectedBytes + md::kDiskReserveBytes;
    TEST_CHECK(md::PlanOffer(true, false, kEnough) == md::OfferPlan::Offer,
               "MD: engine ok + model absent + space -> Offer");
    TEST_CHECK(md::PlanOffer(true, true, kEnough) == md::OfferPlan::Nothing,
               "MD: model already present -> Nothing");
    TEST_CHECK(md::PlanOffer(false, false, kEnough) == md::OfferPlan::Nothing,
               "MD: engine binaries missing -> Nothing (download cannot help)");
    TEST_CHECK(md::PlanOffer(true, false, kEnough - 1) == md::OfferPlan::NoDiskSpace,
               "MD: short free space -> NoDiskSpace");
    TEST_CHECK(md::PlanOffer(true, false, 0) == md::OfferPlan::NoDiskSpace,
               "MD: unknown free space (0) -> NoDiskSpace (fail-closed)");

    // ---- SHA-256 pin sync (the 4th mirror vs the pinned 3-way gate) ---------
    TEST_CHECK(md::kModelSha256Pin == emebalachat::engine_host::kExpectedModelSha256,
               "MD: app-side pin mirrors engine_host::kExpectedModelSha256");
    TEST_CHECK(md::kModelSha256Pin.size() == 64,
               "MD: pin is exactly 64 lowercase hex chars");
    bool pin_lower_hex = true;
    for (const char c : md::kModelSha256Pin) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) pin_lower_hex = false;
    }
    TEST_CHECK(pin_lower_hex, "MD: pin is lowercase hex only");
    // run_tests links engine_core: pin the runtime copy too (the 3-way gate
    // files cannot be seen from C++; this closes the loop on the 4th mirror).
    TEST_CHECK(md::kModelSha256Pin == std::string_view(emebalachat::kExpectedModelSha256),
               "MD: pin mirrors engine_core kExpectedModelSha256 (runtime verify pin)");

    // ---- registry merge shape (round-trip through the real writer) ---------
    {
        using namespace emebalachat::engine_host_registry;
        Registry reg;
        reg.schema_version = 1;
        md::MergeBundledEntry(reg);
        TEST_CHECK(reg.models.size() == 1 && reg.models[0].id == md::kBundledRegistryId,
                   "MD: merge appends the pinned bundled id");
        TEST_CHECK(reg.models[0].origin == "bundled" && reg.models[0].files.size() == 1 &&
                       reg.models[0].files[0] == md::kModelFilename,
                   "MD: bundled entry carries the pinned filename");
        TEST_CHECK(reg.models[0].resource.vram_mb == 2400 && reg.models[0].resource.priority == 9 &&
                       reg.models[0].resource.residency == "preload" &&
                       reg.models[0].resource.eviction == "sticky",
                   "MD: resource block mirrors the installer shape");
        TEST_CHECK(reg.models[0].profiles.count("default") == 1 &&
                       reg.models[0].profiles.at("default").prompt_template_ref == "hymt2-official",
                   "MD: default profile pins hymt2-official");
        TEST_CHECK(reg.models[0].lang_pairs.size() == 1 && reg.models[0].lang_pairs[0] == "*",
                   "MD: lang_pairs ['*']");
        // Idempotence: a second merge never duplicates the slot.
        md::MergeBundledEntry(reg);
        TEST_CHECK(reg.models.size() == 1, "MD: merge is idempotent (no duplicate bundled entry)");
        // Serialize -> parse round-trip (the writer refuses non-bare names;
        // the parser must read back the identical shape).
        const std::string doc = SerializeRegistry(reg);
        const LoadResult back = ParseRegistryJson(doc);
        TEST_CHECK(back.status == LoadStatus::Ok && back.registry.models.size() == 1 &&
                       back.registry.models[0].id == md::kBundledRegistryId,
                   "MD: bundled entry round-trips through Serialize/Parse");
    }

    // ---- .sha256ok marker round-trip (engine_core semantics) ---------------
    {
        std::error_code ec;
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path(ec) / L"emebala_md_marker_test";
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path file = dir / emebalachat::ToUtf16(md::kModelFilename);
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << "fake-gguf";
        }
        TEST_CHECK(md::WriteSha256OkMarker(file), "MD: marker writes");
        // Mirror engine_core MarkerMatchesFile: 3-line shape, size+mtime match.
        const std::filesystem::path marker = dir / (emebalachat::ToUtf16(md::kModelFilename) + L".sha256ok");
        std::ifstream in(marker, std::ios::binary);
        std::string hex;
        long long epoch = -1;
        unsigned long long size = 0;
        TEST_CHECK(static_cast<bool>(in >> hex >> epoch >> size),
                   "MD: marker carries the 3-line shape");
        TEST_CHECK(hex == md::kModelSha256Pin, "MD: marker hex is the pinned hash");
        TEST_CHECK(size == 9, "MD: marker size matches the file");
        const auto mtime = std::filesystem::last_write_time(file, ec);
        TEST_CHECK(!ec && epoch == static_cast<long long>(mtime.time_since_epoch().count()),
                   "MD: marker mtime stamp matches the file (engine_core cache will hit)");
        std::filesystem::remove_all(dir, ec);
    }

    // ---- new StringIds: non-empty + placeholders in all 37 locales ----------
    {
        const UiLocale initial = I18n::GetCurrentLocale();
        int ok = 0;
        for (const auto& entry : GetSupportedUiLocales()) {
            I18n::SetLocale(entry.locale);
            bool all = true;
            for (int id = static_cast<int>(StringId::ModelDlCaption);
                 id <= static_cast<int>(StringId::ModelDlDiskSpace); ++id) {
                if (I18n::Get(static_cast<StringId>(id)).empty()) all = false;
            }
            if (I18n::Get(StringId::ModelDlConsentBody).find(L"{s}") == std::wstring::npos) all = false;
            if (I18n::Get(StringId::ModelDlDiskSpace).find(L"{s}") == std::wstring::npos) all = false;
            if (all) ++ok;
        }
        TEST_CHECK(ok == 37, "MD: all 9 model-download strings non-empty (+tokens) in all 37 locales");
        I18n::SetLocale(UiLocale::English);
        TEST_CHECK(I18n::Get(StringId::ModelDlConsentBody).find(L"{s}") != std::wstring::npos,
                   "MD: en consent body keeps the {s} size token");
        I18n::SetLocale(initial);
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-MD model download logic passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-MD model download logic: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Delta re-review fixes (session 260930_0004 second pass): D-01/N1 (retry
// std::terminate), D-02 (dead zone retry + empty-rect hit test), D-03/D-04
// (stale count/comment pins). Structural pins read the production sources;
// the IsPointInRect empty-rect rule is a functional pin (linked helper).
// ---------------------------------------------------------------------------

void TestDeltaRereviewFixes() {
    std::cout << "[RUN] Testing delta re-review D-01/D-02 fixes..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- D-02 functional: empty rects never hit (shared hit-test helper) ---
    {
        using emebalachat::IsPointInRect;
        const D2D1_RECT_F empty_rect = D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);
        TEST_CHECK(!IsPointInRect(empty_rect, 0.0f, 0.0f),
                   "D-02: the (0,0) degenerate corner of an EMPTY rect is not a hit");
        const D2D1_RECT_F real = D2D1::RectF(10.0f, 10.0f, 50.0f, 40.0f);
        TEST_CHECK(IsPointInRect(real, 10.0f, 10.0f) && IsPointInRect(real, 50.0f, 40.0f),
                   "D-02: real rects keep the inclusive boundary contract");
        TEST_CHECK(!IsPointInRect(real, 9.9f, 25.0f) && !IsPointInRect(real, 25.0f, 41.0f),
                   "D-02: outside stays outside");
    }

    // ---- D-01 structural: join BEFORE re-creating the worker ----------------
    {
        std::string dlg;
        const char* prefixes[] = {"", "../", "../../"};
        for (const char* pre : prefixes) {
            std::ifstream in((std::string(pre) + "src/ui/model_download_dialog.cpp").c_str(),
                             std::ios::binary);
            if (in) {
                dlg.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                break;
            }
        }
        TEST_CHECK(!dlg.empty(), "D-01: model_download_dialog.cpp resolvable for the pin");
        if (!dlg.empty()) {
            const size_t join_pos = dlg.find("if (st->worker.joinable())");
            const size_t spawn_pos = dlg.find("st->worker = std::thread(");
            TEST_CHECK(join_pos != std::string::npos && spawn_pos != std::string::npos &&
                           join_pos < spawn_pos,
                       "D-01/N1: the Retry/download path joins the prior worker BEFORE assigning a new std::thread (no terminate)");
            TEST_CHECK(dlg.find("worker.detach()") == std::string::npos,
                       "D-01: no detach anywhere — one owner discipline (join only)");
            // The single-dialog-exit join still exists (OfferModelDownload).
            const size_t exit_join = dlg.find("st.worker.join()");
            TEST_CHECK(exit_join != std::string::npos,
                       "D-01: the dialog-exit join remains (retry join + exit join, no detach)");
        }
    }

    // ---- D-02 structural: the footer owns update_check_rect_ only in Idle ---
    {
        std::string abt;
        const char* prefixes[] = {"", "../", "../../"};
        for (const char* pre : prefixes) {
            std::ifstream in((std::string(pre) + "src/ui/about_window.cpp").c_str(),
                             std::ios::binary);
            if (in) {
                abt.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                break;
            }
        }
        TEST_CHECK(!abt.empty(), "D-02: about_window.cpp resolvable for the pin");
        if (!abt.empty()) {
            TEST_CHECK(abt.find("if (upd_w > 0.0f) {\n            update_check_rect_ =") !=
                           std::string::npos,
                       "D-02: the footer assigns update_check_rect_ only when it owns the affordance (Idle pill)");
            // The zone retry states still set the rect (Snoozed block anchor).
            TEST_CHECK(abt.find("update_check_rect_ = plan_button(check_label, cx, top + 38.0f);") !=
                           std::string::npos,
                       "D-02: the zone retry states still compute their live hit rect");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Delta re-review D-01/D-02 fixes passed." << std::endl;
    } else {
        std::cout << "[FAIL] Delta re-review fixes: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
