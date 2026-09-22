// ---------------------------------------------------------------------------
// REQ-047 D2 (design §B.3, Rev2 §6, diagnosis 235010): bundled-origin
// reuse-path rejection — the user-.gguf registration picker must REFUSE a
// re-pick of a registry entry whose origin is "bundled" (e.g. the built-in
// Hy-MT2-1.8B-Q8_0.gguf) instead of silently re-pointing config at the
// bundled id (the P2 root cause: the newly registered model never took
// effect).
//
// main.cpp is the GUI entrypoint TU and is deliberately NOT linked into
// run_tests.exe, so the reuse-loop edit is pinned by source inspection
// (TestReq045EngineUnavailableModal / TestReq046TrayMenuStructure precedent),
// while the decision predicate itself (bundled vs user origin) is re-derived
// here against the real parser output.
// ---------------------------------------------------------------------------

namespace {

// The exact predicate the reuse loop applies to a registry match (design
// §B.3 pseudocode): a matched entry with origin == "bundled" takes the
// rejection branch; any other origin (parser default "user") keeps reuse.
bool Req047IsBundledReject(const engine_host_registry::ModelEntry& m) {
    return m.origin == "bundled";
}

// REQ-050 Q2 mirror of main.cpp's GgufFileStem/IsSameUserGgufStem (the GUI
// TU is not linked here): the reuse predicate must treat the entry stem
// equal to the source stem, or to the source stem + "_<digits>" (this
// TU's own collision-suffix convention), as the SAME model — re-adding a
// source after a suffix-bump must reuse the entry, not copy the file again.
std::string Req050MirrorGgufFileStem(std::string name) {
    const auto dot = name.rfind(".gguf");
    if (dot != std::string::npos && dot + 5 == name.size()) {
        name.resize(dot);
    }
    return name;
}
bool Req050MirrorSameUserGgufStem(const std::string& entry_file,
                                  const std::string& source_stem) {
    const std::string entry_stem = Req050MirrorGgufFileStem(entry_file);
    if (entry_stem == source_stem) return true;
    if (entry_stem.size() > source_stem.size() + 1 &&
        entry_stem.compare(0, source_stem.size(), source_stem) == 0 &&
        entry_stem[source_stem.size()] == '_') {
        for (size_t i = source_stem.size() + 1; i < entry_stem.size(); ++i) {
            if (entry_stem[i] < '0' || entry_stem[i] > '9') return false;
        }
        return true;
    }
    return false;
}

} // namespace

void TestReq047BundledReuseRejection() {
    std::cout << "[RUN] Testing REQ-047 D2 bundled-origin reuse rejection..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) parser-level: origin defaults + explicit values --------------
    // The bundled entry as installed by setup.iss: id "hymt2", the pinned
    // filename, origin "bundled".
    const auto parsed = engine_host_registry::ParseRegistryJson(
        "{\"schema_version\":1,\"models\":["
        "{\"id\":\"hymt2\",\"family\":\"ggml-translate\","
        "\"files\":[\"Hy-MT2-1.8B-Q8_0.gguf\"],\"origin\":\"bundled\"},"
        "{\"id\":\"user-llama3\",\"family\":\"ggml-translate\","
        "\"files\":[\"llama3.gguf\"],\"origin\":\"user\"}]}");
    TEST_CHECK(parsed.status == engine_host_registry::LoadStatus::Ok,
               "REQ-047 D2: fixture registry parses Ok");
    const engine_host_registry::Registry& reg = parsed.registry;

    // (a-1) bundled match -> reject path.
    const engine_host_registry::ModelEntry* bundled = nullptr;
    for (const auto& m : reg.models) {
        if (!m.files.empty() && m.files[0] == "Hy-MT2-1.8B-Q8_0.gguf") bundled = &m;
    }
    TEST_CHECK(bundled != nullptr, "REQ-047 D2: bundled fixture entry found by filename");
    if (bundled) {
        TEST_CHECK(bundled->origin == "bundled",
                   "REQ-047 D2: parser preserves origin \"bundled\"");
        TEST_CHECK(Req047IsBundledReject(*bundled),
                   "REQ-047 D2 (a): bundled registry match selects the REJECT branch");
    }

    // (a-2) user match -> reuse path kept (regression guard on the sibling).
    const engine_host_registry::ModelEntry* user = reg.FindModel("user-llama3");
    TEST_CHECK(user != nullptr && user->origin == "user",
               "REQ-047 D2: parser preserves origin \"user\"");
    if (user) {
        TEST_CHECK(!Req047IsBundledReject(*user),
                   "REQ-047 D2 (b): user-origin registry match keeps the REUSE branch");
    }

    // (a-3) parser default: an entry with NO origin key parses as "user"
    // (engine_host_registry.hpp contract), so a legacy hand-edited registry
    // never trips the bundled rejection.
    const auto legacy = engine_host_registry::ParseRegistryJson(
        "{\"schema_version\":1,\"models\":["
        "{\"id\":\"user-legacy\",\"family\":\"ggml-translate\","
        "\"files\":[\"legacy.gguf\"]}]}");
    TEST_CHECK(legacy.status == engine_host_registry::LoadStatus::Ok,
               "REQ-047 D2: origin-less fixture registry parses Ok");
    if (legacy.status == engine_host_registry::LoadStatus::Ok) {
        const auto* e = legacy.registry.FindModel("user-legacy");
        TEST_CHECK(e != nullptr && e->origin == "user",
                   "REQ-047 D2: absent origin key defaults to \"user\" (parser contract)");
        if (e) {
            TEST_CHECK(!Req047IsBundledReject(*e),
                       "REQ-047 D2: origin-less entry keeps the REUSE branch");
        }
    }

    // (a-4) REQ-050 Q2 (duplicate-copies fix): the normalized stem predicate
    // re-derived above against the real cases from the field report — an
    // exact re-pick, a suffix-bumped re-pick (_2, multi-digit _23), and the
    // near-misses that must stay FRESH registrations.
    TEST_CHECK(Req050MirrorSameUserGgufStem("MiLMMT-Latest-4.gguf", "MiLMMT-Latest-4"),
               "REQ-050 Q2 (a): exact stem match reuses the entry (no second copy)");
    TEST_CHECK(Req050MirrorSameUserGgufStem("MiLMMT-Latest-4_2.gguf", "MiLMMT-Latest-4"),
               "REQ-050 Q2 (a): '_2'-suffixed entry matches the re-picked source stem");
    TEST_CHECK(Req050MirrorSameUserGgufStem("model_23.gguf", "model"),
               "REQ-050 Q2 (a): multi-digit '_23' suffix matches the source stem");
    TEST_CHECK(!Req050MirrorSameUserGgufStem("model_2.GGUF", "model"),
               "REQ-050 Q2 (a): an uppercase .GGUF tail is NOT stripped (case-sensitive "
               "rfind policy, GgufFileStem) -> degrades to a missed reuse, never a wrong one");
    TEST_CHECK(!Req050MirrorSameUserGgufStem("model_v2.gguf", "model"),
               "REQ-050 Q2 (a): a non-digit suffix is NOT our bump -> fresh registration");
    TEST_CHECK(!Req050MirrorSameUserGgufStem("model_2x.gguf", "model"),
               "REQ-050 Q2 (a): a digit-then-letter suffix is NOT our bump");
    TEST_CHECK(!Req050MirrorSameUserGgufStem("model.gguf", "model_2"),
               "REQ-050 Q2 (a): the reverse direction (entry shorter) does NOT match");
    TEST_CHECK(!Req050MirrorSameUserGgufStem("other.gguf", "MiLMMT-Latest-4"),
               "REQ-050 Q2 (a): an unrelated stem does NOT match");
    TEST_CHECK(Req050MirrorSameUserGgufStem("Hy-MT2-1.8B-Q8_0.gguf", "Hy-MT2-1.8B-Q8"),
               "REQ-050 Q2 (a): a '_0'-suffixed look-alike of the bundled stem matches, "
               "so the bundled gate below is what rejects it");

    // ---- (b) main.cpp structural pins (GUI TU not linked here) ------------
    auto read_src = [](const char* name, std::string& out) {
        const char* candidates[] = {name, (std::string("../") + name).c_str(),
                                    (std::string("../../") + name).c_str()};
        for (const char* cand : candidates) {
            std::ifstream in(cand, std::ios::binary);
            if (in) {
                out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                return;
            }
        }
    };
    std::string main_src;
    read_src("src/main.cpp", main_src);
    if (main_src.empty()) {
        std::cout << "[SKIP] src/main.cpp not resolvable from the test CWD; REQ-047 D2 structural pins skipped."
                  << std::endl;
        return;
    }

    // The reuse loop gates on the bundled origin BEFORE any config write.
    TEST_CHECK(main_src.find("m.origin == \"bundled\"") != std::string::npos,
               "REQ-047 D2 structural pin: reuse loop compares the matched entry's origin to \"bundled\"");
    // The rejection branch shows the new notice body and reuses the existing
    // registration title (no new title key, per design §B.3).
    TEST_CHECK(main_src.find("StringId::UserGgufBundledDuplicateBody") != std::string::npos,
               "REQ-047 D2 structural pin: bundled branch shows UserGgufBundledDuplicateBody");
    TEST_CHECK(main_src.find("MAIN/RegisterUserGguf/006b: bundled entry re-pick refused") != std::string::npos,
               "REQ-047 D2 structural pin: bundled branch logs the refusal (DIAG 006b)");
    // The rejection branch must NOT touch config/engine: the only Set* calls
    // inside the loop stay on the reuse (post-gate) path. Pin the ordering:
    // the bundled-gate string appears BEFORE the first SetUserModelId in the
    // reuse loop body.
    {
        const size_t gate_pos = main_src.find("m.origin == \"bundled\"");
        const size_t reuse_pos = main_src.find("config.SetUserModelId(m.id)");
        TEST_CHECK(gate_pos != std::string::npos && reuse_pos != std::string::npos &&
                       gate_pos < reuse_pos,
                   "REQ-047 D2 structural pin: bundled gate precedes the config Set* reuse writes");
    }
    // REQ-050 Q2 (duplicate-copies fix): the reuse loop matches on the
    // STEM-NORMALIZED predicate (source stem passed pre-stripped, the same
    // 'stem' the fresh path feeds the id/file bump), so a suffix-bumped
    // entry reuses instead of re-copying. The bundled gate is INSIDE the
    // widened match — predicate < gate < reuse writes.
    TEST_CHECK(main_src.find("bool IsSameUserGgufStem(const std::string& entry_file, const std::string& source_stem)") !=
                   std::string::npos &&
                   main_src.find("std::string GgufFileStem(std::string name)") != std::string::npos,
               "REQ-050 Q2 structural pin: main.cpp defines the GgufFileStem/IsSameUserGgufStem helpers");
    TEST_CHECK(main_src.find("IsSameUserGgufStem(m.files[0], stem)") != std::string::npos,
               "REQ-050 Q2 structural pin: the reuse loop matches on the normalized stem predicate");
    {
        const size_t pred_pos = main_src.find("IsSameUserGgufStem(m.files[0], stem)");
        const size_t gate_pos = main_src.find("m.origin == \"bundled\"");
        const size_t reuse_pos = main_src.find("config.SetUserModelId(m.id)");
        TEST_CHECK(pred_pos != std::string::npos && gate_pos != std::string::npos &&
                       reuse_pos != std::string::npos && pred_pos < gate_pos &&
                       gate_pos < reuse_pos,
                   "REQ-050 Q2 structural pin: normalized predicate < bundled gate < reuse writes");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-047 D2 bundled-origin reuse rejection passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-047 D2 bundled-origin reuse rejection: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestReq047BundledDuplicateI18n() {
    std::cout << "[RUN] Testing REQ-047 D2 UserGgufBundledDuplicateBody i18n coverage..." << std::endl;
    const int failures_before = g_failed_count;

    // (c) The new key resolves NON-EMPTY in all 37 selectable locales (the
    // EnumCount completeness loop in TestR6P5P6I18n covers every key; this
    // pins the D2 key explicitly with its own failure message, per the
    // TestR6P5P6I18n REQ-206/208 pattern). Iterates every UiLocale value in
    // [Korean, EnumCount) — the 37 authored tables (Auto is the detection
    // sentinel, not a table).
    const UiLocale initial = I18n::GetCurrentLocale();
    int blank = 0;
    for (int li = static_cast<int>(UiLocale::Korean);
         li <= static_cast<int>(UiLocale::Burmese); ++li) {
        I18n::SetLocale(static_cast<UiLocale>(li));
        if (I18n::Get(StringId::UserGgufBundledDuplicateBody).empty()) ++blank;
    }
    I18n::SetLocale(initial); // suite-state hygiene
    TEST_CHECK(blank == 0,
               "REQ-047 D2 (c): UserGgufBundledDuplicateBody is non-empty in all 37 locales");

    // The English source text follows the Rev2 §6 softened wording: it names
    // the built-in local translation engine WITHOUT quoting the 'Local LLM'
    // menu label.
    I18n::SetLocale(UiLocale::English);
    const std::wstring en = I18n::Get(StringId::UserGgufBundledDuplicateBody);
    I18n::SetLocale(initial);
    TEST_CHECK(en.find(L"already built into") != std::wstring::npos,
               "REQ-047 D2: English body states the model is already built in");
    TEST_CHECK(en.find(L"No registration is needed") != std::wstring::npos,
               "REQ-047 D2: English body states no registration is needed");
    TEST_CHECK(en.find(L"built-in local translation engine") != std::wstring::npos,
               "REQ-047 D2: English body points at the built-in local translation engine");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-047 D2 i18n coverage passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-047 D2 i18n coverage: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// REQ-047 D3 (design §C, Tech Gate §D3, diagnosis 235020): tray -> OpenAI
// settings dialog DEFERRED open. The P2 root cause was entering
// DialogBoxIndirectParamW synchronously from on_select_engine(2) while the
// TrackPopupMenuEx modality was still tearing down, with owner=HWND_MESSAGE
// providing no visual restore point -> "minimized window, no input". The fix
// defers the dialog open to the ControllerWndProc via kMsgOpenOpenAiSettings
// (WM_APP + 0x500, pure (0,0) SEC-ADJ wake-up; payload in the GUI-thread
// store s_pending_openai_cfg guarded by the s_openai_dialog_pending latch).
//
// main.cpp is the GUI entrypoint TU and is deliberately NOT linked into
// run_tests.exe, so the deferred-open wiring is pinned by source inspection
// (TestReq047BundledReuseRejection precedent), while the WM_APP band
// uniqueness is asserted numerically against the known occupants.
// ---------------------------------------------------------------------------

void TestReq047OpenAiDeferredOpen() {
    std::cout << "[RUN] Testing REQ-047 D3 deferred OpenAI settings open..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) WM_APP band uniqueness: 0x500 free, 0x100~0x400 occupied ------
    // WM_APP is 0x8000. The deferred-open message must not collide with any
    // existing house band (drag_icon 0x1xx, tooltip 0x2xx, language_sync
    // 0x300, about 0x3xx, engine_modal 0x400).
    constexpr UINT kBase = 0x8000; // WM_APP
    constexpr UINT kMsgOpenOpenAiSettings = kBase + 0x500;
    // The known WM_APP-band occupants (Tech Gate §D3 #1): drag_icon 0x1xx,
    // tooltip 0x2xx, language_sync 0x300, about 0x3xx, engine_modal 0x400.
    // Sampled at each band's lower bound; 0x500 must equal NONE of them and
    // sit strictly above the highest (0x400). The array keeps the comparison
    // non-constant (avoids C4127) and self-documents the occupied set.
    const UINT occupied_bands[] = {
        kBase + 0x100, // drag_icon lower bound
        kBase + 0x201, // tooltip lower bound
        kBase + 0x300, // language_sync
        kBase + 0x301, // about lower bound
        kBase + 0x400, // engine_modal
    };
    bool band_occupied_0x500 = false;
    UINT highest_occupied = 0;
    for (UINT band : occupied_bands) {
        if (band == kMsgOpenOpenAiSettings) band_occupied_0x500 = true;
        if (band > highest_occupied) highest_occupied = band;
    }
    TEST_CHECK(!band_occupied_0x500,
               "REQ-047 D3 (a): WM_APP+0x500 is unique vs the 0x100~0x400 occupants");
    TEST_CHECK(kMsgOpenOpenAiSettings > highest_occupied,
               "REQ-047 D3 (a): WM_APP+0x500 is above the highest occupied band (0x400)");

    // ---- (b) main.cpp structural pins (GUI TU not linked here) --------------
    auto read_src = [](const char* name, std::string& out) {
        const char* candidates[] = {name, (std::string("../") + name).c_str(),
                                    (std::string("../../") + name).c_str()};
        for (const char* cand : candidates) {
            std::ifstream in(cand, std::ios::binary);
            if (in) {
                out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                return;
            }
        }
    };
    std::string main_src;
    read_src("src/main.cpp", main_src);
    std::string osw_src;
    read_src("src/ui/openai_settings_window.cpp", osw_src);
    if (main_src.empty()) {
        std::cout << "[SKIP] src/main.cpp not resolvable from the test CWD; REQ-047 D3 structural pins skipped."
                  << std::endl;
        return;
    }

    // The deferred-open message constant is declared in the 0x500 band.
    TEST_CHECK(main_src.find("kMsgOpenOpenAiSettings = WM_APP + 0x500") != std::string::npos,
               "REQ-047 D3 structural pin: kMsgOpenOpenAiSettings is WM_APP + 0x500");

    // The whole on_select_engine(2) body must NOT call ShowOpenAiSettingsDialog
    // (nor DialogBoxIndirect directly) — the immediate call was the P2 root
    // cause. It must instead PostMessage the deferred-open message.
    {
        // Isolate the index-2 branch: from its opening comment marker to the
        // next "} else if (engine_idx == 3)" branch.
        const size_t branch_pos = main_src.find("} else if (engine_idx == 2) {");
        const size_t next_pos = main_src.find("} else if (engine_idx == 3) {", branch_pos);
        TEST_CHECK(branch_pos != std::string::npos && next_pos != std::string::npos &&
                       next_pos > branch_pos,
                   "REQ-047 D3 structural pin: on_select_engine index-2 branch is locatable");
        if (branch_pos != std::string::npos && next_pos != std::string::npos &&
            next_pos > branch_pos) {
            const std::string branch = main_src.substr(branch_pos, next_pos - branch_pos);
            TEST_CHECK(branch.find("ShowOpenAiSettingsDialog") == std::string::npos,
                       "REQ-047 D3 structural pin: index-2 branch does NOT call ShowOpenAiSettingsDialog");
            TEST_CHECK(branch.find("DialogBoxIndirect") == std::string::npos,
                       "REQ-047 D3 structural pin: index-2 branch enters NO dialog box synchronously");
            TEST_CHECK(branch.find("PostMessageW") != std::string::npos &&
                           branch.find("kMsgOpenOpenAiSettings") != std::string::npos,
                       "REQ-047 D3 structural pin: index-2 branch defers via PostMessageW(kMsgOpenOpenAiSettings)");
            // The latch guards against a double-post while one open is pending.
            TEST_CHECK(branch.find("s_openai_dialog_pending") != std::string::npos,
                       "REQ-047 D3 structural pin: index-2 branch latches s_openai_dialog_pending");
        }
    }

    // The ControllerWndProc handler exists and returns 0 (Tech Gate 반영사항 #4).
    {
        const size_t handler_pos = main_src.find("if (msg == kMsgOpenOpenAiSettings) {");
        TEST_CHECK(handler_pos != std::string::npos,
                   "REQ-047 D3 structural pin: ControllerWndProc handles kMsgOpenOpenAiSettings");
        if (handler_pos != std::string::npos) {
            // The handler must open the dialog (parent = g_hControllerWnd, owner
            // contract unchanged per §C.3) and route the save/switch through
            // the coordinator.
            const size_t window = main_src.find("WM_TIMER", handler_pos);
            const std::string handler = main_src.substr(
                handler_pos, (window == std::string::npos ? main_src.size() : window) - handler_pos);
            TEST_CHECK(handler.find("ShowOpenAiSettingsDialog") != std::string::npos,
                       "REQ-047 D3 structural pin: handler opens ShowOpenAiSettingsDialog");
            TEST_CHECK(handler.find("g_hControllerWnd") != std::string::npos,
                       "REQ-047 D3 structural pin: handler keeps parent=g_hControllerWnd (§C.3 owner contract)");
            TEST_CHECK(handler.find("g_apply_openai_settings") != std::string::npos,
                       "REQ-047 D3 structural pin: handler routes save/switch via g_apply_openai_settings");
            TEST_CHECK(handler.find("return 0;") != std::string::npos,
                       "REQ-047 D3 structural pin: handler returns 0 (Tech Gate #4)");
            // Latch is released before the dialog opens (re-entry safety).
            TEST_CHECK(handler.find("s_openai_dialog_pending = false;") != std::string::npos,
                       "REQ-047 D3 structural pin: handler releases the latch before opening");
        }
    }

    // The startup publishes the save/switch body and shutdown retires it.
    TEST_CHECK(main_src.find("g_apply_openai_settings = [&]") != std::string::npos,
               "REQ-047 D3 structural pin: startup publishes g_apply_openai_settings");
    TEST_CHECK(main_src.find("g_apply_openai_settings = nullptr;") != std::string::npos,
               "REQ-047 D3 structural pin: shutdown retires g_apply_openai_settings");

    // ---- (c) §C.4 control-creation verification lives in the dialog proc ----
    if (!osw_src.empty()) {
        TEST_CHECK(osw_src.find("IDC_API_KEY control missing") != std::string::npos,
                   "REQ-047 D3 §C.4: dialog proc logs a missing IDC_API_KEY control");
        TEST_CHECK(osw_src.find("GetDlgItem(dlg, IDC_API_KEY)") != std::string::npos,
                   "REQ-047 D3 §C.4: dialog proc verifies IDC_API_KEY via GetDlgItem");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-047 D3 deferred OpenAI settings open passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-047 D3 deferred OpenAI settings open: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// REQ-047 D1 (design §A, Tech Gate §D1, diagnosis 235000): restore the
// engine-unavailable modal on a strict local translation failure. The P2 root
// cause was REQ-046 d4d5108 deleting the translate-time modal trigger — after
// engine.cpp:504 sets CloudConsentBlocked (or :397 sets LocalModelMissing) the
// worker logged the status and dropped it, so the user saw "Enter pressed,
// nothing translated, no notice". The fix adds a worker callback fired on the
// two strict statuses, marshaled through the existing SEC-ADJ modal seam with
// a GUI-thread streak latch (every-trigger + streak-latch, design §A.4).
//
// main.cpp is the GUI entrypoint TU and is deliberately NOT linked into
// run_tests.exe, so the wiring is pinned by source inspection (D2/D3
// precedent), while the status-branch predicate and the latch policy are
// re-derived here against the real TranslationStatus enum.
// ---------------------------------------------------------------------------

namespace {

// The exact branch predicate worker.cpp applies after Translate() returns
// (design §A.2): only the two strict local statuses surface the modal; every
// other status (Ok, InputEmpty, EngineFailed on the Auto cloud waterfall,
// Canceled) keeps the existing silence. `produced_output` mirrors the
// `!translated.empty()` success gate so an Ok status with output resets the
// latch while an Ok status with empty output (defensive — Translate()
// contractually returns non-empty on Ok) does not.
bool Req047StrictFailureSurfacesModal(TranslationStatus status, bool produced_output) {
    if (status == TranslationStatus::CloudConsentBlocked ||
        status == TranslationStatus::LocalModelMissing) {
        return true;
    }
    return status == TranslationStatus::Ok && produced_output;
}

// Whether the status should RESET the GUI-thread streak latch (the success
// signal the worker sends as an Ok-with-output callback).
bool Req047StatusResetsLatch(TranslationStatus status, bool produced_output) {
    return status == TranslationStatus::Ok && produced_output;
}

} // namespace

void TestReq047EngineUnavailableModal() {
    std::cout << "[RUN] Testing REQ-047 D1 engine-unavailable modal trigger..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) strict-failure branch predicate --------------------------------
    TEST_CHECK(Req047StrictFailureSurfacesModal(TranslationStatus::CloudConsentBlocked, false),
               "REQ-047 D1 (a): CloudConsentBlocked surfaces the modal");
    TEST_CHECK(Req047StrictFailureSurfacesModal(TranslationStatus::LocalModelMissing, false),
               "REQ-047 D1 (a): LocalModelMissing surfaces the modal");
    // Auto-path cloud waterfall failure (EngineFailed) keeps the existing
    // silence — design §A.2 explicitly excludes it.
    TEST_CHECK(!Req047StrictFailureSurfacesModal(TranslationStatus::EngineFailed, false),
               "REQ-047 D1 (a): Auto-path EngineFailed stays silent (design §A.2)");
    TEST_CHECK(!Req047StrictFailureSurfacesModal(TranslationStatus::InputEmpty, false),
               "REQ-047 D1 (a): InputEmpty stays silent");
    TEST_CHECK(!Req047StrictFailureSurfacesModal(TranslationStatus::Canceled, false),
               "REQ-047 D1 (a): Canceled stays silent");
    TEST_CHECK(Req047StrictFailureSurfacesModal(TranslationStatus::Ok, true),
               "REQ-047 D1 (a): Ok with output signals the latch reset");
    TEST_CHECK(!Req047StrictFailureSurfacesModal(TranslationStatus::Ok, false),
               "REQ-047 D1 (a): Ok WITHOUT output does not signal (defensive)");

    // ---- (b) latch policy simulation (GUI-thread state machine) -------------
    // Mirrors g_engine_modal_latched's lifecycle: set when a strict-failure
    // modal is enqueued, cleared when the success sentinel drains. The
    // simulation runs on one thread — the real flag is GUI-thread-only by
    // contract (Tech Gate §D1 #2), which this single-threaded model captures.
    {
        bool latched = false; // initial state at startup
        int modal_count = 0;

        auto on_strict_failure = [&]() {
            if (latched) return; // suppressed re-arm
            latched = true;
            ++modal_count;
        };
        auto on_success = [&]() { latched = false; };

        // Three consecutive failures -> exactly ONE modal (streak-latch).
        on_strict_failure();
        on_strict_failure();
        on_strict_failure();
        TEST_CHECK(modal_count == 1,
                   "REQ-047 D1 (b): 3 consecutive strict failures produce 1 modal (latch)");
        // Success resets -> the next failure re-arms.
        on_success();
        on_strict_failure();
        TEST_CHECK(modal_count == 2,
                   "REQ-047 D1 (b): success resets the latch; a later failure re-arms");
        // Two more failures while latched -> still suppressed.
        on_strict_failure();
        on_strict_failure();
        TEST_CHECK(modal_count == 2,
                   "REQ-047 D1 (b): further latched failures stay suppressed");
        // Recovery again.
        on_success();
        TEST_CHECK(!latched, "REQ-047 D1 (b): latch cleared after the second recovery");
    }

    // ---- (c) structural pins: worker.cpp branch, main.cpp wiring ------------
    auto read_src = [](const char* name, std::string& out) {
        const char* candidates[] = {name, (std::string("../") + name).c_str(),
                                    (std::string("../../") + name).c_str()};
        for (const char* cand : candidates) {
            std::ifstream in(cand, std::ios::binary);
            if (in) {
                out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                return;
            }
        }
    };
    std::string worker_src;
    read_src("src/worker.cpp", worker_src);
    std::string main_src;
    read_src("src/main.cpp", main_src);
    std::string engine_src;
    read_src("src/engine.cpp", engine_src);
    if (worker_src.empty() || main_src.empty()) {
        std::cout << "[SKIP] src/worker.cpp / src/main.cpp not resolvable from the test CWD; "
                     "REQ-047 D1 structural pins skipped."
                  << std::endl;
        return;
    }

    // worker.cpp: the status branch distinguishes the two strict statuses.
    TEST_CHECK(worker_src.find("status == TranslationStatus::CloudConsentBlocked") !=
                       std::string::npos &&
                   worker_src.find("status == TranslationStatus::LocalModelMissing") !=
                       std::string::npos,
               "REQ-047 D1 structural pin: worker.cpp branches on both strict statuses");
    TEST_CHECK(worker_src.find("SetEngineUnavailableCallback") != std::string::npos ||
                   worker_src.find("engine_unavailable_cb_") != std::string::npos,
               "REQ-047 D1 structural pin: worker.cpp fires the engine-unavailable callback");
    // The callback is fired with Ok on the success path (latch reset signal).
    TEST_CHECK(worker_src.find("engine_unavailable_cb_(TranslationStatus::Ok)") !=
                   std::string::npos,
               "REQ-047 D1 structural pin: worker.cpp signals Ok on the success path");

    // main.cpp: registration + latch + drain-handler management.
    TEST_CHECK(main_src.find("SetEngineUnavailableCallback") != std::string::npos,
               "REQ-047 D1 structural pin: main.cpp registers SetEngineUnavailableCallback");
    TEST_CHECK(main_src.find("g_engine_modal_latched") != std::string::npos,
               "REQ-047 D1 structural pin: main.cpp owns the GUI-thread streak latch");
    TEST_CHECK(main_src.find("RequestEngineUnavailableModal") != std::string::npos,
               "REQ-047 D1 structural pin: main.cpp marshals through the SEC-ADJ modal seam");
    // The drain handler manages the latch (set on show, clear on success).
    TEST_CHECK(main_src.find("g_engine_modal_latched = true") != std::string::npos &&
                   main_src.find("g_engine_modal_latched = false") != std::string::npos,
               "REQ-047 D1 structural pin: drain handler sets and clears the latch");
    // The strict-failure wording reuses the existing RepairFailed* pair
    // (design §A.5: reuse first, no new i18n key).
    TEST_CHECK(main_src.find("StringId::RepairFailedTitle") != std::string::npos &&
                   main_src.find("StringId::RepairFailedBody") != std::string::npos,
               "REQ-047 D1 structural pin: strict-failure modal reuses RepairFailedTitle/Body");

    // engine.cpp MUST stay untouched (A-2 rejection: router purity). The
    // status branch that triggers the modal lives in worker.cpp, NOT engine.cpp.
    if (!engine_src.empty()) {
        TEST_CHECK(engine_src.find("RequestEngineUnavailableModal") == std::string::npos,
                   "REQ-047 D1 structural pin: engine.cpp stays UI-free (no modal call)");
        TEST_CHECK(engine_src.find("SetEngineUnavailableCallback") == std::string::npos,
                   "REQ-047 D1 structural pin: engine.cpp has no worker-callback wiring");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-047 D1 engine-unavailable modal trigger passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-047 D1 engine-unavailable modal trigger: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// REQ-047 P5-F1 (technical review 175600 finding #1, design §A.4 ⚠️,
// Tech Gate §D1 #2): the streak-latch judgment must live ONLY in the GUI-
// thread kMsgEngineUnavailableModal drain handler. The pre-P5 worker callback
// wrapper tested g_engine_modal_latched BEFORE the GUI hop, letting the
// worker thread touch GUI-only state (a cross-thread bool race — exactly what
// design §A.4 warned about). main.cpp is not linked into run_tests.exe, so
// the fix is pinned by source inspection (same precedent as the D1 pins
// above) plus a drain-policy re-derivation against the real enum.
// ---------------------------------------------------------------------------

void TestReq047P5F1LatchDrainOwnership() {
    std::cout << "[RUN] Testing REQ-047 P5-F1 latch drain ownership..." << std::endl;
    const int failures_before = g_failed_count;

    auto read_src = [](const char* name, std::string& out) {
        const char* candidates[] = {name, (std::string("../") + name).c_str(),
                                    (std::string("../../") + name).c_str()};
        for (const char* cand : candidates) {
            std::ifstream in(cand, std::ios::binary);
            if (in) {
                out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                return;
            }
        }
    };
    std::string main_src;
    read_src("src/main.cpp", main_src);
    if (main_src.empty()) {
        std::cout << "[SKIP] src/main.cpp not resolvable from the test CWD; "
                     "REQ-047 P5-F1 pins skipped."
                  << std::endl;
        return;
    }

    // ---- (a) worker callback wrapper: NO latch access -----------------------
    // Isolate the SetEngineUnavailableCallback registration body (the worker
    // wrapper) and prove it never names the latch. The wrapper runs on the
    // WORKER thread; the latch is GUI-thread-only.
    {
        const size_t reg_pos = main_src.find("SetEngineUnavailableCallback");
        TEST_CHECK(reg_pos != std::string::npos,
                   "REQ-047 P5-F1 (a): main.cpp registers SetEngineUnavailableCallback");
        if (reg_pos != std::string::npos) {
            // The wrapper body ends at the "});" that closes the lambda; the
            // engine-unavailable registration is the LAST Set*Callback call in
            // the file, so rfind from the registration point is unique.
            const size_t body_end = main_src.find("});", reg_pos);
            TEST_CHECK(body_end != std::string::npos && body_end > reg_pos,
                       "REQ-047 P5-F1 (a): worker callback wrapper body is locatable");
            if (body_end != std::string::npos && body_end > reg_pos) {
                const std::string wrapper = main_src.substr(reg_pos, body_end - reg_pos);
                TEST_CHECK(wrapper.find("g_engine_modal_latched") == std::string::npos,
                           "REQ-047 P5-F1 (a): worker callback wrapper never touches the latch");
                TEST_CHECK(wrapper.find("RequestEngineUnavailableModal") != std::string::npos,
                           "REQ-047 P5-F1 (a): wrapper marshals via the SEC-ADJ modal seam");
            }
        }
    }

    // ---- (b) drain handler: sole latch judge --------------------------------
    // Extract the kMsgEngineUnavailableModal handler body (bounded by the next
    // top-level message branch) and prove the latch set/clear/suppress all
    // live there, on the GUI thread.
    {
        const size_t handler_pos = main_src.find("if (msg == kMsgEngineUnavailableModal)");
        TEST_CHECK(handler_pos != std::string::npos,
                   "REQ-047 P5-F1 (b): kMsgEngineUnavailableModal drain handler exists");
        if (handler_pos != std::string::npos) {
            const size_t handler_end = main_src.find("if (msg == kMsgOpenOpenAiSettings)",
                                                     handler_pos);
            TEST_CHECK(handler_end != std::string::npos && handler_end > handler_pos,
                       "REQ-047 P5-F1 (b): drain handler body is locatable");
            if (handler_end != std::string::npos && handler_end > handler_pos) {
                const std::string handler = main_src.substr(handler_pos, handler_end - handler_pos);
                TEST_CHECK(handler.find("g_engine_modal_latched = true") != std::string::npos,
                           "REQ-047 P5-F1 (b): drain handler sets the latch before showing");
                TEST_CHECK(handler.find("g_engine_modal_latched = false") != std::string::npos,
                           "REQ-047 P5-F1 (b): drain handler clears the latch on the success sentinel");
                TEST_CHECK(handler.find("if (emebalachat::g_engine_modal_latched)") != std::string::npos,
                           "REQ-047 P5-F1 (b): drain handler suppresses re-arms while latched");
            }
        }
    }

    // ---- (c) drain policy re-derivation (state machine) ---------------------
    // Mirror the drain handler's policy against the real enum: success
    // sentinel clears, strict-failure + latched drops, strict-failure +
    // unlatched shows + latches. Strict statuses come from the real
    // TranslationStatus enum (same predicate shape as Req047StrictFailureSurfacesModal).
    {
        bool latched = false; // GUI-thread state at startup
        int shown = 0;
        int dropped = 0;
        auto drain = [&](const char* title, TranslationStatus status_hint) {
            // Sentinel = empty title+body WITHOUT a strict status (the producers
            // enqueue strict failures as title=body=nullptr + status; a success
            // is status=Ok with both empty). The status check keeps the two
            // request kinds mutually exclusive — a nullptr title alone is NOT
            // enough (a strict failure also carries nullptr title/body).
            const bool strict_failure =
                status_hint == TranslationStatus::CloudConsentBlocked ||
                status_hint == TranslationStatus::LocalModelMissing;
            const bool success_sentinel = (title == nullptr) && !strict_failure;
            if (success_sentinel) {
                latched = false;
                return;
            }
            if (!strict_failure) {
                return; // non-strict request: outside the latch policy
            }
            if (latched) {
                ++dropped; // suppressed re-arm
                return;
            }
            latched = true;
            ++shown;
        };
        drain(nullptr, TranslationStatus::Ok);       // success sentinel -> clear
        drain(nullptr, TranslationStatus::CloudConsentBlocked); // strict #1 -> show
        drain(nullptr, TranslationStatus::CloudConsentBlocked); // strict #2 -> drop (latched)
        drain(nullptr, TranslationStatus::LocalModelMissing);   // strict #3 -> drop (latched)
        TEST_CHECK(shown == 1 && dropped == 2 && latched,
                   "REQ-047 P5-F1 (c): 3 consecutive strict failures -> 1 modal, 2 suppressed");
        drain(nullptr, TranslationStatus::Ok);       // recovery -> clear
        TEST_CHECK(!latched, "REQ-047 P5-F1 (c): success sentinel clears the latch");
        drain(nullptr, TranslationStatus::LocalModelMissing);   // strict #4 -> re-arm
        TEST_CHECK(shown == 2 && latched,
                   "REQ-047 P5-F1 (c): a later failure re-arms after recovery");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-047 P5-F1 latch drain ownership passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-047 P5-F1 latch drain ownership: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// REQ-047 P5-F2 (technical review 175600 finding #2): the three drag-family
// translation paths — drag-icon click (run_drag_translate), tooltip F9
// re-translation (run_retranslate), double Ctrl+C — used to call the 3-arg
// Translate overload, which DISCARDS the status; a CloudConsentBlocked /
// LocalModelMissing strict failure therefore died silently on those paths.
// Each path must now use the 4-arg (status-out) overload and marshal strict
// failures (and the success sentinel) through the same SEC-ADJ modal seam as
// the Enter pipeline. engine.{hpp,cpp} stay untouched — the 4-arg overload
// already exists (engine.hpp L213-218).
// ---------------------------------------------------------------------------

void TestReq047P5F2DragPathsStatusWiring() {
    std::cout << "[RUN] Testing REQ-047 P5-F2 drag-paths status wiring..." << std::endl;
    const int failures_before = g_failed_count;

    auto read_src = [](const char* name, std::string& out) {
        const char* candidates[] = {name, (std::string("../") + name).c_str(),
                                    (std::string("../../") + name).c_str()};
        for (const char* cand : candidates) {
            std::ifstream in(cand, std::ios::binary);
            if (in) {
                out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                return;
            }
        }
    };
    std::string main_src;
    read_src("src/main.cpp", main_src);
    std::string engine_src;
    read_src("src/engine.cpp", engine_src);
    if (main_src.empty()) {
        std::cout << "[SKIP] src/main.cpp not resolvable from the test CWD; "
                     "REQ-047 P5-F2 pins skipped."
                  << std::endl;
        return;
    }

    // ---- (a) each drag-family path receives the status ----------------------
    // The three 4-arg Translate call sites (one per path) with a non-null
    // status out-pointer. Count the exact shape to prove no path regressed to
    // the status-discarding 3-arg form.
    {
        const int k4ArgSites =
            static_cast<int>(std::count(main_src.begin(), main_src.end(), '&'));
        (void)k4ArgSites; // informational only; the per-site pins below are precise
        TEST_CHECK(main_src.find("engine.Translate(selected, eff_src, tgt_lang, &drag_status)") !=
                       std::string::npos,
                   "REQ-047 P5-F2 (a): drag-icon path uses the status-out overload");
        TEST_CHECK(main_src.find("engine.Translate(job.src, eff_src, tgt_lang, &rt_status)") !=
                       std::string::npos,
                   "REQ-047 P5-F2 (a): tooltip F9 re-translation path uses the status-out overload");
        TEST_CHECK(main_src.find("engine.Translate(copied, eff_src, tgt_lang, &dbl_status)") !=
                       std::string::npos,
                   "REQ-047 P5-F2 (a): double Ctrl+C path uses the status-out overload");
    }

    // ---- (b) each path marshals strict failures to the modal seam ------------
    // The strict-failure predicate + RequestEngineUnavailableModal appear in
    // all three path bodies (P5-F2 tag marks each site).
    {
        const size_t tag_count = [&]() {
            size_t n = 0;
            size_t pos = 0;
            while ((pos = main_src.find("REQ-047 P5-F2", pos)) != std::string::npos) {
                ++n;
                pos += std::string("REQ-047 P5-F2").size();
            }
            return n;
        }();
        TEST_CHECK(tag_count >= 3,
                   "REQ-047 P5-F2 (b): all three drag-family paths carry the P5-F2 tag");
        TEST_CHECK(main_src.find("drag_status == emebalachat::TranslationStatus::CloudConsentBlocked") !=
                       std::string::npos,
                   "REQ-047 P5-F2 (b): drag-icon path branches on the strict statuses");
        TEST_CHECK(main_src.find("rt_status == emebalachat::TranslationStatus::CloudConsentBlocked") !=
                       std::string::npos,
                   "REQ-047 P5-F2 (b): retranslate path branches on the strict statuses");
        TEST_CHECK(main_src.find("dbl_status == emebalachat::TranslationStatus::CloudConsentBlocked") !=
                       std::string::npos,
                   "REQ-047 P5-F2 (b): double Ctrl+C path branches on the strict statuses");
        // Every strict branch marshals through the shared SEC-ADJ seam (the
        // same RequestEngineUnavailableModal the Enter pipeline uses).
        TEST_CHECK(main_src.find("StringId::RepairFailedTitle") != std::string::npos &&
                       main_src.find("StringId::RepairFailedBody") != std::string::npos,
                   "REQ-047 P5-F2 (b): strict failures reuse the RepairFailedTitle/Body wording");
    }

    // ---- (c) success sentinel: a non-empty result resets the latch -----------
    // Each path's success branch (else-if !translated.empty()) sends the
    // empty-title sentinel through the same seam.
    {
        TEST_CHECK(main_src.find("Success sentinel: resets the streak latch") != std::string::npos ||
                       main_src.find("Success sentinel: resets the streak latch (same seam as Enter)") !=
                           std::string::npos,
                   "REQ-047 P5-F2 (c): drag paths reset the latch on success via the sentinel");
    }

    // ---- (d) engine.cpp stays frozen (router-purity contract) ---------------
    if (!engine_src.empty()) {
        TEST_CHECK(engine_src.find("RequestEngineUnavailableModal") == std::string::npos,
                   "REQ-047 P5-F2 (d): engine.cpp stays UI-free (no modal call)");
        TEST_CHECK(engine_src.find("CloudConsentBlocked") != std::string::npos,
                   "REQ-047 P5-F2 (d): engine.cpp still sets the strict statuses");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-047 P5-F2 drag-paths status wiring passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-047 P5-F2 drag-paths status wiring: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// REQ-047 U1 (designer 164500, P4-4b): tray engine-submenu gguf-state
// expression — ① "로컬 LLM" -> "내장 로컬 엔진" relabel, ② dynamic user-model
// label (stem whenever a model is registered, regardless of the checked
// engine; entry absent when nothing is registered — REQ-050 3-1/3-2), ③
// MF_SEPARATOR group boundary between built-in engines and the user model,
// ④ file-picker row relabelled "다른 .gguf 모델 등록…" (merged into the
// manager row by REQ-050) and placed below the separator, ⑤ OpenAI "…"
// suffix. The 4-way radio contract (REQ-046) and the 2010~2014 ID band are
// pinned unchanged; the separator carries no ID (designer §5.2.3).
// ---------------------------------------------------------------------------

void TestReq047U1TrayGgufLabels() {
    std::cout << "[RUN] Testing REQ-047 U1 tray gguf labels + separator..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) i18n: the 5 designer strings resolve in all 37 locales --------
    // The EnumCount completeness loop in TestR6P5P6I18n covers every key;
    // these pins name the U1 keys explicitly (TestReq047BundledDuplicateI18n
    // precedent) and verify the designer's exact Korean wording.
    const UiLocale initial = I18n::GetCurrentLocale();
    int blank = 0;
    for (int li = static_cast<int>(UiLocale::Korean);
         li <= static_cast<int>(UiLocale::Burmese); ++li) {
        I18n::SetLocale(static_cast<UiLocale>(li));
        if (I18n::Get(StringId::MenuEngineUserGgufEmpty).empty()) ++blank;
        if (I18n::Get(StringId::MenuEngineLocal).empty()) ++blank;
        if (I18n::Get(StringId::MenuEngineOpenAi).empty()) ++blank;
        if (I18n::Get(StringId::MenuEngineUserGguf).empty()) ++blank;
        if (I18n::Get(StringId::MenuBrowseGgufFile).empty()) ++blank;
    }
    I18n::SetLocale(initial); // suite-state hygiene
    TEST_CHECK(blank == 0,
               "REQ-047 U1 (a): the 5 touched StringIds are non-empty in all 37 locales");

    // Designer-exact Korean pins (decisions #1-#4, spec §6).
    I18n::SetLocale(UiLocale::Korean);
    TEST_CHECK(I18n::Get(StringId::MenuEngineLocal) == L"내장 로컬 엔진 (Hy-MT2-1.8B 오프라인)",
               "REQ-047 U1 (a): ko MenuEngineLocal is the designer's relabel");
    TEST_CHECK(I18n::Get(StringId::MenuEngineOpenAi) == L"OpenAI 호환 (사용자 지정 서버)…",
               "REQ-047 U1 (a): ko MenuEngineOpenAi carries the '…' suffix");
    TEST_CHECK(I18n::Get(StringId::MenuEngineUserGguf) == L"사용자 지정 모델 (.gguf)",
               "REQ-047 U1 (a): ko MenuEngineUserGguf is the dynamic-label base");
    TEST_CHECK(I18n::Get(StringId::MenuBrowseGgufFile) == L"다른 .gguf 모델 등록…",
               "REQ-047 U1 (a): ko MenuBrowseGgufFile is the registration action");
    TEST_CHECK(I18n::Get(StringId::MenuEngineUserGgufEmpty) == L"(미등록)",
               "REQ-047 U1 (a): ko MenuEngineUserGgufEmpty is the '(미등록)' placeholder");
    I18n::SetLocale(initial);

    // ---- (b) tray.cpp structural pins (GUI TU inspected, not linked) -------
    auto read_src = [](const char* name, std::string& out) {
        const char* candidates[] = {name, (std::string("../") + name).c_str(),
                                    (std::string("../../") + name).c_str()};
        for (const char* cand : candidates) {
            std::ifstream in(cand, std::ios::binary);
            if (in) {
                out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                return;
            }
        }
    };
    std::string tray_src;
    read_src("src/ui/tray.cpp", tray_src);
    std::string tray_hdr;
    read_src("src/ui/tray.hpp", tray_hdr);
    std::string main_src;
    read_src("src/main.cpp", main_src);
    if (tray_src.empty() || tray_hdr.empty()) {
        std::cout << "[SKIP] src/ui/tray.* not resolvable from the test CWD; "
                     "REQ-047 U1 structural pins skipped." << std::endl;
        return;
    }

    // MF_SEPARATOR group boundary between the built-in engines and the user
    // model (designer decision #1, Layer 3).
    TEST_CHECK(tray_src.find("MF_SEPARATOR, 0, nullptr") != std::string::npos,
               "REQ-047 U1 (b): tray.cpp inserts an MF_SEPARATOR into the engine submenu");
    // Dynamic label (REQ-050 3-1/3-2 revision): the " — <stem>" suffix is
    // appended whenever a model is registered, REGARDLESS of the checked
    // engine (the old pref==3 gate is gone); with nothing registered the
    // checkable entry is not appended at all, so the "(미등록)" placeholder
    // left the tray menu with the always-append entry. The em-dash join is
    // the spec's. REQ-056: the stem is a menu-build-time local derived from
    // the resolver's EngineMenuView (no cached member).
    TEST_CHECK(tray_src.find("view.user_model_stem") != std::string::npos &&
                   tray_src.find("StringId::MenuEngineUserGgufEmpty") == std::string::npos,
               "REQ-047 U1 (b): tray.cpp joins the registered stem via the em-dash; the "
               "'(미등록)' placeholder is gone (REQ-050 3-1)");
    TEST_CHECK(tray_src.find("if (!user_model_stem.empty())") != std::string::npos &&
                   tray_src.find("if (pref == 3)") == std::string::npos,
               "REQ-047 U1 (b): the checkable entry exists only while a model is registered "
               "and its stem shows under any checked engine (REQ-050 3-1/3-2)");
    TEST_CHECK(tray_src.find("L\" — \"") != std::string::npos,
               "REQ-047 U1 (b): tray.cpp joins the dynamic label with the em-dash separator");
    // REQ-056 (derive-at-open): the tray declares the resolver seam; the old
    // UpdateStatus user_model_stem parameter is gone.
    TEST_CHECK(tray_hdr.find("struct EngineMenuView") != std::string::npos &&
                   tray_hdr.find("SetEngineMenuResolver") != std::string::npos,
               "REQ-056: tray.hpp declares EngineMenuView + SetEngineMenuResolver "
               "(derive-at-open replaces the UpdateStatus stem parameter)");

    // ---- (c) REQ-046 contract pins: ID band 2010~2014 + 4-way radio --------
    // (regression guard — the separator must NOT consume an ID or a slot).
    TEST_CHECK(tray_src.find("ID_TRAY_ENGINE_GOOGLE = 2010") != std::string::npos &&
                   tray_src.find("ID_TRAY_ENGINE_LOCAL = 2011") != std::string::npos &&
                   tray_src.find("ID_TRAY_ENGINE_OPENAI = 2012") != std::string::npos &&
                   tray_src.find("ID_TRAY_ENGINE_USER_GGUF = 2013") != std::string::npos &&
                   tray_src.find("ID_TRAY_BROWSE_GGUF = 2014") != std::string::npos,
               "REQ-047 U1 (c): the engine ID band 2010~2014 is unchanged");
    TEST_CHECK(tray_src.find("pref == 0 ? MF_CHECKED") != std::string::npos &&
                   tray_src.find("pref == 1 ? MF_CHECKED") != std::string::npos &&
                   tray_src.find("pref == 2 ? MF_CHECKED") != std::string::npos &&
                   tray_src.find("pref == 3 ? MF_CHECKED") != std::string::npos,
               "REQ-047 U1 (c): the 4-way radio check structure is preserved");

    // ---- (d) main.cpp stem-resolution wiring (REQ-056: inside the resolver) -
    if (!main_src.empty()) {
        TEST_CHECK(main_src.find("user_model_stem") != std::string::npos &&
                       main_src.find("FindModel(config.user_model_id)") != std::string::npos,
                   "REQ-047 U1 (d): main.cpp resolves the stem via the default registry");
        TEST_CHECK(main_src.find("rfind(\".gguf\")") != std::string::npos,
                   "REQ-047 U1 (d): main.cpp strips the '.gguf' suffix into the stem");
        // REQ-056: the resolution lives inside the SetEngineMenuResolver
        // lambda (derive-at-open), which is wired before the message loop.
        const size_t resolver_pos = main_src.find("SetEngineMenuResolver");
        const size_t stem_pos = main_src.find("std::string user_model_stem;", resolver_pos);
        const size_t msg_pos = main_src.find("while (::GetMessageW");
        TEST_CHECK(resolver_pos != std::string::npos && stem_pos != std::string::npos &&
                       stem_pos > resolver_pos,
                   "REQ-056: stem resolution lives inside the engine-menu resolver");
        TEST_CHECK(msg_pos != std::string::npos && resolver_pos < msg_pos,
                   "REQ-056: the resolver is wired before the message loop");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-047 U1 tray gguf labels + separator passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-047 U1 tray gguf labels + separator: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// REQ-051 (Symptom D): the new OpenAiDeleteSettings StringId — the settings
// dialog [삭제] push-button caption — resolves NON-EMPTY in all 37 selectable
// locales, with the designer-exact Korean and English references (the user
// mandate: real translations in every locale, no English placeholders; the
// zero-English-passthrough gate rides TestReq048R2I18nCoverage's extended
// range). TestReq047BundledDuplicateI18n precedent.
// ---------------------------------------------------------------------------

void TestReq051OpenAiDeleteI18n() {
    std::cout << "[RUN] Testing REQ-051 OpenAiDeleteSettings i18n coverage..." << std::endl;
    const int failures_before = g_failed_count;

    // (a) non-empty in all 37 authored tables (Auto is the detection
    // sentinel, not a table).
    const UiLocale initial = I18n::GetCurrentLocale();
    int blank = 0;
    for (int li = static_cast<int>(UiLocale::Korean);
         li <= static_cast<int>(UiLocale::Burmese); ++li) {
        I18n::SetLocale(static_cast<UiLocale>(li));
        if (I18n::Get(StringId::OpenAiDeleteSettings).empty()) ++blank;
    }
    I18n::SetLocale(initial); // suite-state hygiene
    TEST_CHECK(blank == 0,
               "REQ-051 (a): OpenAiDeleteSettings is non-empty in all 37 locales");

    // (b) designer-exact reference strings.
    I18n::SetLocale(UiLocale::Korean);
    TEST_CHECK(I18n::Get(StringId::OpenAiDeleteSettings) == L"설정 삭제",
               "REQ-051 (b): ko OpenAiDeleteSettings is the designer reference");
    I18n::SetLocale(UiLocale::English);
    TEST_CHECK(I18n::Get(StringId::OpenAiDeleteSettings) == L"Delete settings",
               "REQ-051 (b): en OpenAiDeleteSettings is the reference");
    I18n::SetLocale(initial);

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-051 OpenAiDeleteSettings i18n coverage passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-051 OpenAiDeleteSettings i18n coverage: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
