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

// ---------------------------------------------------------------------------
// REQ-048 R2-D: the registered user-.gguf model manager (rename/delete UI).
// User request: "다른 gguf 모델을 할 거면 이름이라도 수정할 수 있던가, 등록한
// 모델을 삭제라도 할 수 있던가". Five layers:
//   (a) i18n field-count mirror with the NEW total (95) — the same sizeof
//       technique as TestReq044I18nFieldOrder; that suite's mirror in
//       run_tests.cpp is frozen at 78 by the editing mandate, so the R2-D
//       total is pinned here instead;
//   (b) functional — ValidateUserModelId rules (empty / >64 / separators /
//       whitespace / duplicate / self), UserModelsOf filtering (bundled
//       excluded), rename -> SerializeRegistry -> re-parse (id changed,
//       files + origin intact, bundled + untouched users preserved),
//       delete -> re-parse (entry gone, bundled preserved, rename/delete
//       guards on bundled and duplicate ids);
//   (c) manager-template linkage — BuildGgufManagerTemplate emits through
//       the shared REQ-048 P2 TemplateBuilder with the production item
//       count (5), no modal loop entered;
//   (d) i18n runtime pins — Korean carries the real translations, English
//       the placeholder strings (37-table completeness rides the existing
//       TestR6P5P6I18n EnumCount loop);
//   (e) structural pins — tray.cpp "모델 관리…" item + WM_APP+0x501 deferred
//       post + the frozen 4-way radio IDs (2010~2014) intact; main.cpp
//       kMsgOpenGgufManager + the open/refresh coordinator; the manager
//       window cpp reuses TemplateBuilder + SerializeRegistry + the config
//       contract.
// ---------------------------------------------------------------------------

// The header is not part of run_tests.cpp's include block (that TU is
// orchestrator-owned); the .inc includes it directly, exactly like the
// declarations it consumes from core.
#include "../src/ui/gguf_model_manager_window.hpp" // REQ-048 R2-D: pure helpers + BuildGgufManagerTemplate

namespace req048_r2d_detail {

// Mirror of LocalizedStrings (src/i18n.cpp) — field COUNT and TYPE only,
// with the REQ-048 R2-D total (78 pre-R2-D fields + 12 gguf-manager fields +
// the REQ-050 dialog OK/Cancel pair). TestReq044I18nFieldOrder pins the count
// with the identical sizeof technique; its copy lives in run_tests.cpp and is
// frozen by the editing mandate, so the new 99-field total is asserted here.
struct Req048R2LstrMirror {
    const wchar_t* f[99];
};

} // namespace req048_r2d_detail

void TestReq048R2GgufManager() {
    std::cout << "[RUN] Testing REQ-048 R2-D gguf model manager..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) i18n field-count mirror (new total 99) ---------------------
    static_assert(sizeof(req048_r2d_detail::Req048R2LstrMirror) % sizeof(const wchar_t*) == 0,
                  "REQ-048 R2-D: Req048R2LstrMirror must be an array of uniform pointers");
    constexpr std::size_t kR2dFieldCount =
        sizeof(req048_r2d_detail::Req048R2LstrMirror) / sizeof(const wchar_t*); // == 99
    static_assert(kR2dFieldCount == 99,
                  "REQ-048 R2-D: LocalizedStrings field count is 99 (78 + 12 + 5 + 2 + 2) - "
                  "update the i18n.cpp X-macro, all 37 locale tables, AND this mirror");
    // Runtime record through a volatile (avoids C4127 under /W4), matching
    // the TestReq044I18nFieldOrder discipline.
    volatile std::size_t observed_field_count = kR2dFieldCount;
    volatile int observed_enum_count = static_cast<int>(StringId::EnumCount);
    TEST_CHECK(observed_field_count == 99,
               "REQ-048 R2-D (a): LocalizedStrings field count is 99");
    TEST_CHECK(observed_enum_count == 99,
               "REQ-048 R2-D (a): StringId::EnumCount is 99 (78 + 12 + 5 + 2 manager strings + 2 dialog)");

    // ---- (b) functional: validation + rename/delete round-trips ---------
    // Registry doc: one bundled (never listed/renamed/deleted) + two users.
    const std::string doc =
        "{\"schema_version\":1,\"models\":["
        "{\"id\":\"hymt2\",\"family\":\"ggml-translate\","
        "\"files\":[\"Hy-MT2-1.8B-Q8_0.gguf\"],\"origin\":\"bundled\"},"
        "{\"id\":\"user-alpha\",\"family\":\"ggml-translate\","
        "\"files\":[\"alpha.gguf\"],\"origin\":\"user\"},"
        "{\"id\":\"user-beta\",\"family\":\"ggml-translate\","
        "\"files\":[\"beta.gguf\"],\"origin\":\"user\"}"
        "]}";
    const auto loaded = engine_host_registry::ParseRegistryJson(doc);
    TEST_CHECK(loaded.status == engine_host_registry::LoadStatus::Ok,
               "REQ-048 R2-D (b): manager test registry parses Ok");
    engine_host_registry::Registry reg = loaded.registry; // working copy

    // Validation rules (REQ-048 R2-D): empty / length / separators /
    // whitespace / duplicate / self all pinned.
    TEST_CHECK(ValidateUserModelId("", reg, "user-alpha") == GgufModelIdError::Empty,
               "REQ-048 R2-D (b): empty new id rejected");
    TEST_CHECK(ValidateUserModelId(std::string(65, 'x'), reg, "user-alpha") ==
                   GgufModelIdError::TooLong,
               "REQ-048 R2-D (b): 65-char new id rejected (> 64)");
    TEST_CHECK(ValidateUserModelId(std::string(64, 'x'), reg, "user-alpha") ==
                   GgufModelIdError::Ok,
               "REQ-048 R2-D (b): exactly-64-char new id accepted");
    TEST_CHECK(ValidateUserModelId("has space", reg, "user-alpha") == GgufModelIdError::BadChars,
               "REQ-048 R2-D (b): whitespace in new id rejected");
    TEST_CHECK(ValidateUserModelId("a/b", reg, "user-alpha") == GgufModelIdError::BadChars,
               "REQ-048 R2-D (b): '/' separator rejected");
    TEST_CHECK(ValidateUserModelId("a\\b", reg, "user-alpha") == GgufModelIdError::BadChars,
               "REQ-048 R2-D (b): '\\\\' separator rejected");
    TEST_CHECK(ValidateUserModelId("a:b", reg, "user-alpha") == GgufModelIdError::BadChars,
               "REQ-048 R2-D (b): drive colon rejected");
    TEST_CHECK(ValidateUserModelId("user-beta", reg, "user-alpha") == GgufModelIdError::Duplicate,
               "REQ-048 R2-D (b): duplicate of another entry rejected");
    TEST_CHECK(ValidateUserModelId("user-alpha", reg, "user-alpha") == GgufModelIdError::Ok,
               "REQ-048 R2-D (b): current id passes (dialog no-ops new == old)");

    // User listing excludes bundled, registry order kept.
    const auto users = UserModelsOf(reg);
    TEST_CHECK(users.size() == 2 &&
                   users[0]->id == "user-alpha" && users[1]->id == "user-beta",
               "REQ-048 R2-D (b): UserModelsOf lists only the user-origin entries");

    // Rename -> serialize -> re-parse: id changes, files/origin ride along,
    // bundled + untouched user entries are preserved byte-for-byte.
    TEST_CHECK(RenameUserModelEntry(reg, "user-alpha", "user-gamma"),
               "REQ-048 R2-D (b): rename applies in memory");
    const std::string renamed_doc = engine_host_registry::SerializeRegistry(reg);
    TEST_CHECK(!renamed_doc.empty(), "REQ-048 R2-D (b): renamed registry serializes");
    const auto re_parsed = engine_host_registry::ParseRegistryJson(renamed_doc);
    TEST_CHECK(re_parsed.status == engine_host_registry::LoadStatus::Ok &&
                   re_parsed.registry.models.size() == 3,
               "REQ-048 R2-D (b): renamed registry round-trips (3 models)");
    if (re_parsed.status == engine_host_registry::LoadStatus::Ok) {
        const auto* gamma = re_parsed.registry.FindModel("user-gamma");
        TEST_CHECK(gamma != nullptr && gamma->origin == "user" &&
                       gamma->files.size() == 1 && gamma->files[0] == "alpha.gguf",
                   "REQ-048 R2-D (b): renamed id keeps files[] and origin");
        TEST_CHECK(re_parsed.registry.FindModel("user-alpha") == nullptr,
                   "REQ-048 R2-D (b): old id gone after rename");
        TEST_CHECK(re_parsed.registry.FindModel("user-beta") != nullptr,
                   "REQ-048 R2-D (b): untouched user entry preserved");
        const auto* bundled = re_parsed.registry.FindModel("hymt2");
        TEST_CHECK(bundled != nullptr && bundled->origin == "bundled" &&
                       bundled->files.size() == 1 &&
                       bundled->files[0] == "Hy-MT2-1.8B-Q8_0.gguf",
                   "REQ-048 R2-D (b): bundled entry preserved through rename");
    }
    // Rename guards.
    TEST_CHECK(!RenameUserModelEntry(reg, "hymt2", "bundled-new"),
               "REQ-048 R2-D (b): renaming a bundled entry refused");
    TEST_CHECK(!RenameUserModelEntry(reg, "user-beta", "user-gamma"),
               "REQ-048 R2-D (b): rename onto an existing id refused");
    TEST_CHECK(!RenameUserModelEntry(reg, "missing-id", "whatever"),
               "REQ-048 R2-D (b): renaming an unknown id refused");

    // Delete -> serialize -> re-parse: entry removed, bundled survives.
    TEST_CHECK(RemoveUserModelEntry(reg, "user-beta"),
               "REQ-048 R2-D (b): delete applies in memory");
    TEST_CHECK(!RemoveUserModelEntry(reg, "hymt2"),
               "REQ-048 R2-D (b): deleting a bundled entry refused");
    const std::string deleted_doc = engine_host_registry::SerializeRegistry(reg);
    const auto re_parsed2 = engine_host_registry::ParseRegistryJson(deleted_doc);
    TEST_CHECK(re_parsed2.status == engine_host_registry::LoadStatus::Ok &&
                   re_parsed2.registry.models.size() == 2,
               "REQ-048 R2-D (b): deleted registry round-trips (2 models)");
    if (re_parsed2.status == engine_host_registry::LoadStatus::Ok) {
        TEST_CHECK(re_parsed2.registry.FindModel("user-beta") == nullptr,
                   "REQ-048 R2-D (b): deleted user entry gone");
        TEST_CHECK(re_parsed2.registry.FindModel("user-gamma") != nullptr,
                   "REQ-048 R2-D (b): remaining user entry survives deletion");
        TEST_CHECK(re_parsed2.registry.FindModel("hymt2") != nullptr,
                   "REQ-048 R2-D (b): bundled entry survives deletion");
    }

    // ---- (c) manager template linkage + production item count -----------
    {
        TemplateBuilder tb;
        tb.Begin(L"probe", 210, 96, /*itemCount=*/5);
        BuildGgufManagerTemplate(tb);
        const auto* dt = tb.Get();
        TEST_CHECK(dt != nullptr && dt->cdit == 5,
                   "REQ-048 R2-D (c): manager template emits 5 items through the "
                   "shared REQ-048 P2 TemplateBuilder (list + empty + 3 buttons)");
        TEST_CHECK(tb.size() > sizeof(DLGTEMPLATE),
                   "REQ-048 R2-D (c): manager template buffer is non-trivial");
    }

    // ---- (d) i18n runtime pins (ko translated / en placeholder) ---------
    const UiLocale initial_locale = I18n::GetCurrentLocale();
    I18n::SetLocale(UiLocale::Korean);
    TEST_CHECK(I18n::Get(StringId::MenuManageGgufModels) == L"모델 관리…",
               "REQ-048 R2-D (d): ko MenuManageGgufModels designated-init mapping intact");
    TEST_CHECK(I18n::Get(StringId::GgufManagerTitle) == L"사용자 모델 관리",
               "REQ-048 R2-D (d): ko GgufManagerTitle designated-init mapping intact");
    I18n::SetLocale(UiLocale::English);
    TEST_CHECK(I18n::Get(StringId::GgufManagerEmpty) == L"No user models registered.",
               "REQ-048 R2-D (d): en GgufManagerEmpty tail field intact");
    TEST_CHECK(I18n::Get(StringId::GgufManagerClose) == L"Close",
               "REQ-048 R2-D (d): en GgufManagerClose tail field intact");
    I18n::SetLocale(initial_locale); // suite-state hygiene

    // ---- (e) structural pins --------------------------------------------
    const std::filesystem::path tray_cpp = ResolveRepoFile(L"src\\ui\\tray.cpp");
    if (tray_cpp.empty()) {
        std::cout << "[SKIP] src/ui/tray.cpp not resolvable; REQ-048 R2-D (e) tray pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(tray_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        // The new row is a PLAIN item (no radio) with its own ID below 2020.
        TEST_CHECK(src.find("ID_TRAY_MANAGE_GGUF = 2015") != std::string::npos,
                   "REQ-048 R2-D (e): tray manage-models row uses its own ID (2015)");
        TEST_CHECK(src.find("StringId::MenuManageGgufModels") != std::string::npos,
                   "REQ-048 R2-D (e): tray row renders the localized MenuManageGgufModels");
        // Frozen 4-way radio contract (REQ-046) untouched by the new row.
        TEST_CHECK(src.find("ID_TRAY_ENGINE_GOOGLE = 2010") != std::string::npos &&
                       src.find("ID_TRAY_ENGINE_LOCAL = 2011") != std::string::npos &&
                       src.find("ID_TRAY_ENGINE_OPENAI = 2012") != std::string::npos &&
                       src.find("ID_TRAY_ENGINE_USER_GGUF = 2013") != std::string::npos &&
                       src.find("ID_TRAY_BROWSE_GGUF = 2014") != std::string::npos,
                   "REQ-048 R2-D (e): frozen 4-way radio band 2010~2014 intact");
        // Deferred open: same modality-avoidance contract as WM_APP+0x500.
        TEST_CHECK(src.find("WM_APP + 0x501") != std::string::npos,
                   "REQ-048 R2-D (e): tray dispatches the deferred WM_APP+0x501 open");
    }

    const std::filesystem::path main_cpp = ResolveRepoFile(L"src\\main.cpp");
    if (main_cpp.empty()) {
        std::cout << "[SKIP] src/main.cpp not resolvable; REQ-048 R2-D (e) main pins skipped."
                  << std::endl;
    } else {
        std::ifstream in(main_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        TEST_CHECK(src.find("kMsgOpenGgufManager = WM_APP + 0x501") != std::string::npos,
                   "REQ-048 R2-D (e): main.cpp defines kMsgOpenGgufManager = WM_APP + 0x501");
        TEST_CHECK(src.find("g_open_gguf_model_manager") != std::string::npos,
                   "REQ-048 R2-D (e): main.cpp publishes the open+refresh coordinator");
        TEST_CHECK(src.find("ShowGgufModelManagerDialog") != std::string::npos,
                   "REQ-048 R2-D (e): main.cpp opens the manager dialog on the GUI thread");
    }

    const std::filesystem::path mgr_cpp = ResolveRepoFile(L"src\\ui\\gguf_model_manager_window.cpp");
    if (mgr_cpp.empty()) {
        std::cout << "[SKIP] src/ui/gguf_model_manager_window.cpp not resolvable; "
                     "REQ-048 R2-D (e) manager pins skipped." << std::endl;
    } else {
        std::ifstream in(mgr_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        TEST_CHECK(src.find("TemplateBuilder") != std::string::npos,
                   "REQ-048 R2-D (e): manager window reuses the REQ-048 P2 TemplateBuilder");
        TEST_CHECK(src.find("SerializeRegistry") != std::string::npos,
                   "REQ-048 R2-D (e): manager persists through the REQ-045 registry writer");
        TEST_CHECK(src.find("SetUserModelId") != std::string::npos,
                   "REQ-048 R2-D (e): manager tracks config.user_model_id across renames/deletes");
        TEST_CHECK(src.find("BuildGgufManagerTemplate") != std::string::npos,
                   "REQ-048 R2-D (e): manager template emission split for the unit suite");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-048 R2-D gguf model manager checks passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-048 R2-D gguf model manager checks: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}


// ---------------------------------------------------------------------------
// REQ-048 R2-F (user directive): the most-recent StringIds (the gguf
// manager family + the REQ-050 dialog OK/Cancel pair, indices 78..96) must be
// REAL translations in every one of the 37 supported locales — no English
// passthrough. Language-level English fallback exists only for UNSUPPORTED
// languages; a supported locale that shows English for a shipped string is a
// defect (the REQ-045/047 backlog shipped exactly that way, which this gate
// now prevents from recurring).
// ---------------------------------------------------------------------------

void TestReq048R2I18nCoverage() {
    std::cout << "[RUN] Testing REQ-048 R2 recent-field translation coverage..." << std::endl;
    const int failures_before = g_failed_count;
    const UiLocale initial = I18n::GetCurrentLocale();

    I18n::SetLocale(UiLocale::English);
    std::wstring en[21];
    const int first = static_cast<int>(StringId::MenuManageGgufModels);
    const int last = static_cast<int>(StringId::DialogCancel); // REQ-050 tail
    for (int id = first; id <= last; ++id) {
        en[id - first] = I18n::Get(static_cast<StringId>(id));
    }

    int english_copy = 0;
    for (int li = static_cast<int>(UiLocale::Korean);
         li <= static_cast<int>(UiLocale::Burmese); ++li) {
        if (li == static_cast<int>(UiLocale::English)) continue; // the reference itself
        I18n::SetLocale(static_cast<UiLocale>(li));
        for (int id = first; id <= last; ++id) {
            if (I18n::Get(static_cast<StringId>(id)) == en[id - first]) ++english_copy;
        }
    }
    I18n::SetLocale(initial); // suite-state hygiene
    TEST_CHECK(english_copy == 0,
               "REQ-048 R2-F/R4 + REQ-050: the 21 recent strings are translated in all 37 "
               "locales (zero English-identical fields)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-048 R2 recent-field translation coverage passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-048 R2 i18n coverage: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
