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
// REQ-048 R2-D + REQ-050: the registered user-.gguf model manager
// (rename/delete UI) and its merged add-methods row (add-from-file callback +
// Hugging Face resolve-URL download dialog).
// User request: "다른 gguf 모델을 할 거면 이름이라도 수정할 수 있던가, 등록한
// 모델을 삭제라도 할 수 있던가". Five layers:
//   (a) i18n field-count mirror with the NEW total (107) — the same sizeof
//       technique as TestReq044I18nFieldOrder; that suite's mirror in
//       run_tests.cpp is frozen at the pre-REQ-050 count by the editing
//       mandate, so the REQ-050 total is pinned here instead;
//   (b) functional — ValidateUserModelId rules (empty / >64 / separators /
//       whitespace / duplicate / self), UserModelsOf filtering (bundled
//       excluded), rename -> SerializeRegistry -> re-parse (id changed,
//       files + origin intact, bundled + untouched users preserved),
//       delete -> re-parse (entry gone, bundled preserved, rename/delete
//       guards on bundled and duplicate ids);
//   (c) manager-template linkage — BuildGgufManagerTemplate emits through
//       the shared REQ-048 P2 TemplateBuilder with the production item
//       count (7: REQ-050 added the two top-row add buttons), no modal loop
//       entered; REQ-050 adds the HF add-dialog template pins (5 items) and
//       the add-button StringId checks;
//   (d) i18n runtime pins — Korean carries the real translations, English
//       the placeholder strings (37-table completeness rides the existing
//       TestR6P5P6I18n EnumCount loop);
//   (e) structural pins — tray.cpp single merged manager row + WM_APP+0x501
//       deferred post + the frozen 4-way radio IDs (2010~2014) intact;
//       main.cpp kMsgOpenGgufManager + the open/refresh coordinator + the
//       REQ-050 INV-B3 deferred-open; the manager window cpp reuses
//       TemplateBuilder + SerializeRegistry + the config contract and hosts
//       the HF download (WinHTTP + progress bar + value-only worker posts).
// ---------------------------------------------------------------------------

// The header is not part of run_tests.cpp's include block (that TU is
// orchestrator-owned); the .inc includes it directly, exactly like the
// declarations it consumes from core.
#include "../src/ui/gguf_model_manager_window.hpp" // REQ-048 R2-D: pure helpers + BuildGgufManagerTemplate

namespace req048_r2d_detail {

// Mirror of LocalizedStrings (src/i18n.cpp) — field COUNT and TYPE only,
// with the REQ-050 total (78 pre-R2-D fields + 17 gguf-manager family fields
// + tooltip/repair-transient pair + the REQ-050 dialog OK/Cancel pair + the
// 8 merged-manager/HF fields). TestReq044I18nFieldOrder pins the count with
// the identical sizeof technique; its copy lives in run_tests.cpp and is
// frozen by the editing mandate, so the 107-field total is asserted here.
struct Req048R2LstrMirror {
    const wchar_t* f[107];
};

} // namespace req048_r2d_detail

void TestReq048R2GgufManager() {
    std::cout << "[RUN] Testing REQ-048 R2-D gguf model manager..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) i18n field-count mirror (REQ-050 total 107) ----------------
    static_assert(sizeof(req048_r2d_detail::Req048R2LstrMirror) % sizeof(const wchar_t*) == 0,
                  "REQ-048 R2-D: Req048R2LstrMirror must be an array of uniform pointers");
    constexpr std::size_t kR2dFieldCount =
        sizeof(req048_r2d_detail::Req048R2LstrMirror) / sizeof(const wchar_t*); // == 107
    static_assert(kR2dFieldCount == 107,
                  "REQ-048 R2-D/REQ-050: LocalizedStrings field count is 107 (78 + 17 + 2 + "
                  "2 + 8) - update the i18n.cpp X-macro, all 37 locale tables, AND this mirror");
    // Runtime record through a volatile (avoids C4127 under /W4), matching
    // the TestReq044I18nFieldOrder discipline.
    volatile std::size_t observed_field_count = kR2dFieldCount;
    volatile int observed_enum_count = static_cast<int>(StringId::EnumCount);
    TEST_CHECK(observed_field_count == 107,
               "REQ-048 R2-D/REQ-050 (a): LocalizedStrings field count is 107");
    TEST_CHECK(observed_enum_count == 107,
               "REQ-048 R2-D/REQ-050 (a): StringId::EnumCount is 107 (struct fields == "
               "switch cases, through HfDone)");

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

    // ---- (c) manager template linkage + production item set -------------
    // REQ-050: 7 items (2 add buttons + list + empty + 3 action buttons),
    // dialog 242x112. The byte-walk pins the exact ids/geometry/titles the
    // Windows DLGITEMTEMPLATE parser consumes (the req048_detail readers from
    // req048_template_tests.inc, included earlier in this TU).
    {
        const UiLocale tpl_locale = I18n::GetCurrentLocale();
        I18n::SetLocale(UiLocale::Korean); // add-button + manager labels
        using namespace req048_detail;
        TemplateBuilder tb;
        tb.Begin(L"probe", 242, 112, /*itemCount=*/7);
        BuildGgufManagerTemplate(tb);
        const BYTE* base = reinterpret_cast<const BYTE*>(tb.Get());
        TEST_CHECK(base != nullptr, "REQ-050 (c): manager template buffer non-null");
        if (base) {
            const WORD cdit = ReadWord(base, 8);
            TEST_CHECK(cdit == 7,
                       "REQ-050 (c): manager template emits 7 items (add-file + add-HF "
                       "+ list + empty + rename + delete + close)");
            // Expected items in emission order: id, x, y, cx, cy, title.
            const WORD kMgrStaticCls = 0x0082;
            const WORD kMgrBtnCls = 0x0080;
            const WORD kMgrListCls = 0x0083;
            struct Exp { WORD id; WORD cls; short x, y, cx, cy; std::wstring title; };
            const Exp kExp[] = {
                {4104, kMgrBtnCls,    8,   5, 110, 13, L"파일에서 추가…"},
                {4105, kMgrBtnCls,    124, 5, 110, 13, L"Hugging Face에서 추가…"},
                {4100, kMgrListCls,   8,   22, 226, 58, L""},
                {4101, kMgrStaticCls, 8,   22, 226, 58,
                 I18n::Get(StringId::GgufManagerEmpty)},
                {4102, kMgrBtnCls,    8,   86, 60, 13,
                 I18n::Get(StringId::GgufManagerRename)},
                {4103, kMgrBtnCls,    80,  86, 60, 13,
                 I18n::Get(StringId::GgufManagerDelete)},
                {IDCANCEL, kMgrBtnCls, 172, 86, 62, 13,
                 I18n::Get(StringId::GgufManagerClose)},
            };
            size_t off = 18; // header: style(4) exstyle(4) cdit(2) x y cx cy
            off += 2;        // menu
            off += 2;        // windowClass
            off += 2 * (1 + std::wstring(L"probe").size()); // title + NUL
            off += 2;        // pointsize
            off += 2 * (1 + std::wstring(L"Segoe UI").size()); // font + NUL
            bool walk_ok = true;
            for (WORD iItem = 0; iItem < cdit && iItem < 7; ++iItem) {
                off += (4 - off % 4) % 4; // DWORD-align relative to the template
                const Exp& e = kExp[iItem];
                const WORD id = ReadWord(base, off + 16);
                const WORD cls = ReadWord(base, off + 20);
                const short x = static_cast<short>(ReadWord(base, off + 8));
                const short y = static_cast<short>(ReadWord(base, off + 10));
                const short cx = static_cast<short>(ReadWord(base, off + 12));
                const short cy = static_cast<short>(ReadWord(base, off + 14));
                size_t toff = off + 22; // fixed part 18 + class array 4
                const std::wstring title = ReadString(base, &toff);
                if (id != e.id || cls != e.cls || x != e.x || y != e.y ||
                    cx != e.cx || cy != e.cy || title != e.title) {
                    walk_ok = false;
                    break;
                }
                off = toff + 2; // creation-data count WORD
            }
            TEST_CHECK(walk_ok,
                       "REQ-050 (c): manager template items 1:1 — add-file/add-HF "
                       "buttons on the top row, list+actions shifted down (7 items)");
            TEST_CHECK(off == tb.size(),
                       "REQ-050 (c): manager template walk consumed exactly size() bytes");
        }
        I18n::SetLocale(tpl_locale); // suite-state hygiene
    }

    // ---- (c2) REQ-050: the HF add-dialog template (5 items) -------------
    {
        const UiLocale tpl_locale = I18n::GetCurrentLocale();
        I18n::SetLocale(UiLocale::English); // DialogOk/Cancel reference
        using namespace req048_detail;
        TemplateBuilder tb;
        tb.Begin(L"probe", 256, 88, /*itemCount=*/5);
        BuildHfAddTemplate(tb);
        const auto* dt = tb.Get();
        TEST_CHECK(dt != nullptr && dt->cdit == 5,
                   "REQ-050 (c2): HF add template emits 5 items (URL label + URL edit "
                   "+ status + Download(IDOK) + Cancel(IDCANCEL))");
        const BYTE* base = reinterpret_cast<const BYTE*>(tb.Get());
        if (base) {
            const WORD kHfStaticCls = 0x0082;
            const WORD kHfEditCls = 0x0081;
            const WORD kHfBtnCls = 0x0080;
            struct Exp { WORD id; WORD cls; short x, y, cx, cy; std::wstring title; };
            const Exp kExp[] = {
                {4300, kHfStaticCls, 8,   6,  120, 9,
                 I18n::Get(StringId::HfUrlLabel)},
                {4301, kHfEditCls,   8,   17, 240, 12, L""},
                {4302, kHfStaticCls, 8,   52, 240, 9,  L""},
                {IDOK, kHfBtnCls,    110, 66, 64,  13,
                 I18n::Get(StringId::DialogOk)},
                {IDCANCEL, kHfBtnCls, 182, 66, 64, 13,
                 I18n::Get(StringId::DialogCancel)},
            };
            size_t off = 18 + 2 + 2;
            off += 2 * (1 + std::wstring(L"probe").size());
            off += 2;
            off += 2 * (1 + std::wstring(L"Segoe UI").size());
            bool walk_ok = true;
            for (WORD iItem = 0; iItem < 5; ++iItem) {
                off += (4 - off % 4) % 4;
                const Exp& e = kExp[iItem];
                if (ReadWord(base, off + 16) != e.id ||
                    ReadWord(base, off + 20) != e.cls ||
                    static_cast<short>(ReadWord(base, off + 8)) != e.x ||
                    static_cast<short>(ReadWord(base, off + 10)) != e.y ||
                    static_cast<short>(ReadWord(base, off + 12)) != e.cx ||
                    static_cast<short>(ReadWord(base, off + 14)) != e.cy) {
                    walk_ok = false;
                    break;
                }
                size_t toff = off + 22;
                if (ReadString(base, &toff) != e.title) {
                    walk_ok = false;
                    break;
                }
                off = toff + 2;
            }
            TEST_CHECK(walk_ok,
                       "REQ-050 (c2): HF add template items 1:1 — URL edit w=240, "
                       "Download(IDOK)/Cancel reuse DialogOk/DialogCancel");
            TEST_CHECK(off == tb.size(),
                       "REQ-050 (c2): HF add template walk consumed exactly size() bytes");
        }
        I18n::SetLocale(tpl_locale); // suite-state hygiene
    }

    // ---- (d) i18n runtime pins (ko translated / en placeholder) ---------
    const UiLocale initial_locale = I18n::GetCurrentLocale();
    I18n::SetLocale(UiLocale::Korean);
    TEST_CHECK(I18n::Get(StringId::MenuManageGgufModels) == L"모델 관리…",
               "REQ-048 R2-D (d): ko MenuManageGgufModels designated-init mapping intact");
    TEST_CHECK(I18n::Get(StringId::GgufManagerTitle) == L"사용자 모델 관리",
               "REQ-048 R2-D (d): ko GgufManagerTitle designated-init mapping intact");
    // REQ-050: the merged-manager add row + HF dialog Korean references.
    TEST_CHECK(I18n::Get(StringId::GgufManagerAddFile) == L"파일에서 추가…",
               "REQ-050 (d): ko GgufManagerAddFile is the designer reference");
    TEST_CHECK(I18n::Get(StringId::GgufManagerAddHf) == L"Hugging Face에서 추가…",
               "REQ-050 (d): ko GgufManagerAddHf is the designer reference");
    TEST_CHECK(I18n::Get(StringId::HfAddTitle) == L"Hugging Face 모델 추가",
               "REQ-050 (d): ko HfAddTitle is the designer reference");
    TEST_CHECK(I18n::Get(StringId::HfUrlLabel) == L"모델 URL",
               "REQ-050 (d): ko HfUrlLabel is the designer reference");
    TEST_CHECK(I18n::Get(StringId::HfDownloading) == L"다운로드 중…",
               "REQ-050 (d): ko HfDownloading is the designer reference");
    TEST_CHECK(I18n::Get(StringId::HfFailed) == L"다운로드에 실패했습니다.",
               "REQ-050 (d): ko HfFailed is the designer reference");
    TEST_CHECK(I18n::Get(StringId::HfInvalidUrl) ==
                   L"huggingface.co 모델 주소를 입력하세요 (…/resolve/… 형식)",
               "REQ-050 (d): ko HfInvalidUrl is the designer reference");
    TEST_CHECK(I18n::Get(StringId::HfDone) == L"등록되었습니다.",
               "REQ-050 (d): ko HfDone is the designer reference");
    I18n::SetLocale(UiLocale::English);
    TEST_CHECK(I18n::Get(StringId::GgufManagerEmpty) == L"No user models registered.",
               "REQ-048 R2-D (d): en GgufManagerEmpty tail field intact");
    TEST_CHECK(I18n::Get(StringId::GgufManagerClose) == L"Close",
               "REQ-048 R2-D (d): en GgufManagerClose tail field intact");
    TEST_CHECK(I18n::Get(StringId::GgufManagerAddFile) == L"Add from file…",
               "REQ-050 (d): en GgufManagerAddFile tail field intact");
    TEST_CHECK(I18n::Get(StringId::HfInvalidUrl) ==
                   L"Enter a huggingface.co model URL (…/resolve/… format)",
               "REQ-050 (d): en HfInvalidUrl tail field intact");
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
        // REQ-050: the browse row is merged into this single manager row —
        // the menu no longer appends ID_TRAY_BROWSE_GGUF (the constant stays
        // defined so the frozen 2010~2014 band numbering is untouched).
        TEST_CHECK(src.find("MF_STRING, ID_TRAY_BROWSE_GGUF") == std::string::npos,
                   "REQ-050 (e): the standalone browse row is gone from the menu");
        TEST_CHECK(src.find("MF_STRING, ID_TRAY_MANAGE_GGUF") != std::string::npos,
                   "REQ-050 (e): the single merged manager row renders as a plain MF_STRING item");
        // The new row is a PLAIN item (no radio) with its own ID below 2020.
        TEST_CHECK(src.find("ID_TRAY_MANAGE_GGUF = 2015") != std::string::npos,
                   "REQ-048 R2-D (e): tray manage-models row uses its own ID (2015)");
        TEST_CHECK(src.find("StringId::MenuManageGgufModels") != std::string::npos,
                   "REQ-048 R2-D (e): tray row renders the localized MenuManageGgufModels");
        // REQ-050: the menu wires no on_browse_gguf callback anymore (the
        // file-picker pipeline reaches the manager through the add_from_file
        // argument; see the main.cpp pins below).
        TEST_CHECK(src.find("on_browse_gguf") == std::string::npos,
                   "REQ-050 (e): tray.cpp no longer dispatches an on_browse_gguf callback");
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
        // REQ-050: the INV-B3 empty-registry path now defers the merged
        // manager open (pure (0,0) wake-up) instead of running the picker
        // inline, and the file-picker pipeline travels into the manager as
        // the add_from_file callback.
        TEST_CHECK(src.find("user_gguf selected but no model registered; opening model manager") !=
                       std::string::npos,
                   "REQ-050 (e): INV-B3 (no model registered) routes into the merged manager");
        TEST_CHECK(src.find("RegisterUserGgufModel(config, engine)") != std::string::npos,
                   "REQ-050 (e): the file-picker registration pipeline is still the add-from-file body");
        const size_t invb3_pos = src.find("opening model manager");
        const size_t invb3_post = src.find("kMsgOpenGgufManager", invb3_pos);
        TEST_CHECK(invb3_pos != std::string::npos && invb3_post != std::string::npos &&
                       invb3_pos < invb3_post,
                   "REQ-050 (e): INV-B3 posts kMsgOpenGgufManager (deferred open contract)");
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
        // REQ-050: the HF download path — WinHTTP, runtime progress bar,
        // value-only worker posts, and the fail-closed URL gate.
        TEST_CHECK(src.find("WinHttpOpen") != std::string::npos &&
                       src.find("WINHTTP_FLAG_SECURE") != std::string::npos,
                   "REQ-050 (e): HF download uses WinHTTP with the secure flag");
        TEST_CHECK(src.find("PROGRESS_CLASSW") != std::string::npos &&
                       src.find("InitCommonControlsEx") != std::string::npos,
                   "REQ-050 (e): HF dialog creates the progress bar at runtime");
        TEST_CHECK(src.find("kHfMsgDone") != std::string::npos &&
                       src.find("std::thread(HfDownloadWorker") != std::string::npos,
                   "REQ-050 (e): the download pumps on a worker thread with value-only posts");
        TEST_CHECK(src.find("IsHfResolveUrl") != std::string::npos &&
                       src.find("https://huggingface.co/") != std::string::npos,
                   "REQ-050 (e): the fail-closed huggingface.co resolve-URL gate is wired");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-048 R2-D gguf model manager checks passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-048 R2-D gguf model manager checks: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}


// ---------------------------------------------------------------------------
// REQ-048 R2-F (user directive) + REQ-050 tail extension: the most-recent
// StringIds (the gguf manager family + the REQ-050 dialog OK/Cancel pair +
// the 8 merged-manager/HF strings, indices 78..106) must be REAL translations
// in every one of the 37 supported locales — no English passthrough.
// Language-level English fallback exists only for UNSUPPORTED languages; a
// supported locale that shows English for a shipped string is a defect (the
// REQ-045/047 backlog shipped exactly that way, which this gate now prevents
// from recurring).
// ---------------------------------------------------------------------------

void TestReq048R2I18nCoverage() {
    std::cout << "[RUN] Testing REQ-048 R2 recent-field translation coverage..." << std::endl;
    const int failures_before = g_failed_count;
    const UiLocale initial = I18n::GetCurrentLocale();

    I18n::SetLocale(UiLocale::English);
    std::wstring en[29];
    const int first = static_cast<int>(StringId::MenuManageGgufModels);
    const int last = static_cast<int>(StringId::HfDone); // REQ-050 tail (through hf_done)
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
               "REQ-048 R2-F/R4 + REQ-050: the 29 recent strings (manager family through "
               "HfDone) are translated in all 37 locales (zero English-identical fields)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-048 R2 recent-field translation coverage passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-048 R2 i18n coverage: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// REQ-050: the Hugging Face resolve-URL gate — the headless-logic proof for
// the [Hugging Face에서 추가…] download path. The validator is https-only and
// host-pinned, so a local mock server can never impersonate huggingface.co;
// these pins therefore cover the URL acceptance/rejection matrix and the
// filename derivation, while a REAL-network download against huggingface.co
// stays a manual QA item. The download worker itself (WinHTTP streaming,
// progress, cancel, 20 GB cap) is GUI-threaded and pinned structurally in
// TestReq048R2GgufManager (e).
// ---------------------------------------------------------------------------

void TestReq050HfUrlGate() {
    std::cout << "[RUN] Testing REQ-050 HF resolve-URL gate + filename derivation..."
              << std::endl;
    const int failures_before = g_failed_count;

    // ---- acceptance matrix (the exact shapes the dialog must allow) -------
    TEST_CHECK(IsHfResolveUrl("https://huggingface.co/a/b/resolve/main/m.gguf"),
               "REQ-050: canonical repo/revision/file resolve URL accepted");
    TEST_CHECK(IsHfResolveUrl("https://huggingface.co/user/repo/resolve/v1.0/"
                              "model-Q8_0.gguf"),
               "REQ-050: nested repo path + tagged revision accepted");
    TEST_CHECK(IsHfResolveUrl("https://huggingface.co/a/resolve/main/m.gguf"),
               "REQ-050: single-segment repo accepted");

    // ---- rejection matrix (fail-closed; nothing else may reach WinHTTP) ----
    TEST_CHECK(!IsHfResolveUrl("http://huggingface.co/a/b/resolve/main/m.gguf"),
               "REQ-050: http:// (no TLS) rejected");
    TEST_CHECK(!IsHfResolveUrl("https://evil.com/huggingface.co/a/b/resolve/main/m.gguf"),
               "REQ-050: foreign host with the HF path in it rejected");
    TEST_CHECK(!IsHfResolveUrl("https://huggingface.co.evil.com/a/resolve/main/m.gguf"),
               "REQ-050: lookalike host rejected");
    TEST_CHECK(!IsHfResolveUrl("https://HuggingFace.co/a/b/resolve/main/m.gguf"),
               "REQ-050: case-folded host rejected (pinned, case-sensitive)");
    TEST_CHECK(!IsHfResolveUrl("https://huggingface.co/a/b/blob/main/m.gguf"),
               "REQ-050: /blob/ page URL rejected (only /resolve/ files)");
    TEST_CHECK(!IsHfResolveUrl("https://huggingface.co/a/b/resolve/main/m.gguf?download=true"),
               "REQ-050: query string rejected");
    TEST_CHECK(!IsHfResolveUrl("https://huggingface.co/a/b/resolve/main/m.gguf#frag"),
               "REQ-050: fragment rejected");
    TEST_CHECK(!IsHfResolveUrl("https://huggingface.co/a/b/resolve/main/"),
               "REQ-050: trailing slash (empty last segment) rejected");
    TEST_CHECK(!IsHfResolveUrl("https://huggingface.co/a/b/resolve//m.gguf"),
               "REQ-050: empty revision rejected");
    TEST_CHECK(!IsHfResolveUrl("https://huggingface.co/a/b/resolve/main"),
               "REQ-050: revision-only (no file path) rejected");
    TEST_CHECK(!IsHfResolveUrl("https://huggingface.co//resolve/main/m.gguf"),
               "REQ-050: empty repo rejected");
    TEST_CHECK(!IsHfResolveUrl("https://huggingface.co/a/b/resolve/main/../m.gguf"),
               "REQ-050: dot-dot segment rejected");
    TEST_CHECK(!IsHfResolveUrl("https://huggingface.co/a/b/resolve/main/./m.gguf"),
               "REQ-050: dot segment rejected");
    TEST_CHECK(!IsHfResolveUrl("huggingface.co/a/b/resolve/main/m.gguf"),
               "REQ-050: scheme-less string rejected");
    TEST_CHECK(!IsHfResolveUrl("https://huggingface.co/"),
               "REQ-050: bare host rejected");
    TEST_CHECK(!IsHfResolveUrl(""),
               "REQ-050: empty string rejected");

    // ---- filename derivation ----------------------------------------------
    std::string name;
    TEST_CHECK(HfResolveFilename("https://huggingface.co/a/b/resolve/main/m.gguf", &name) &&
                   name == "m.gguf",
               "REQ-050: filename derived from the last path segment");
    TEST_CHECK(HfResolveFilename(
                   "https://huggingface.co/user/repo/resolve/v1.0/Model Q4_K_M.gguf",
                   &name) && name == "Model Q4_K_M.gguf",
               "REQ-050: filename with spaces preserved (legal bare filename)");
    TEST_CHECK(HfResolveFilename(
                   "https://huggingface.co/a/resolve/main/모델.gguf", &name) &&
                   name == "모델.gguf",
               "REQ-050: non-ASCII filename preserved (registration pipeline allows it)");
    TEST_CHECK(!HfResolveFilename("https://huggingface.co/a/resolve/main/", &name),
               "REQ-050: empty filename rejected");
    TEST_CHECK(!HfResolveFilename("https://huggingface.co/a/resolve/main/..", &name),
               "REQ-050: '..' as the last segment rejected");
    TEST_CHECK(!HfResolveFilename("https://huggingface.co/a/b/blob/main/m.gguf", &name),
               "REQ-050: derivation refuses a /blob/ URL even when the last segment is clean");
    TEST_CHECK(!HfResolveFilename("http://huggingface.co/a/resolve/main/m.gguf", &name),
               "REQ-050: derivation refuses a non-HTTPS URL even when the segment is clean");
    TEST_CHECK(!HfResolveFilename(
                   "https://huggingface.co/a/resolve/main/" +
                   std::string(201, 'x'), &name),
               "REQ-050: over-long filename rejected (kHfMaxFilenameLen=200 ceiling)");
    // out == nullptr is a safe no-op false (defensive API contract).
    TEST_CHECK(!HfResolveFilename("https://huggingface.co/a/resolve/main/m.gguf",
                                  nullptr),
               "REQ-050: null out-parameter rejected without writing");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-050 HF resolve-URL gate checks passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-050 HF URL gate: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}
