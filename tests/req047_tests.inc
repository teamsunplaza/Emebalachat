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
