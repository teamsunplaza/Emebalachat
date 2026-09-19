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
// REQ-047 U1 (designer 164500, P4-4b): tray engine-submenu gguf-state
// expression — ① "로컬 LLM" -> "내장 로컬 엔진" relabel, ② dynamic user-model
// label with stem / "(미등록)" placeholder, ③ MF_SEPARATOR group boundary
// between built-in engines and the user model, ④ file-picker row relabelled
// "다른 .gguf 모델 등록…" and placed below the separator, ⑤ OpenAI "…" suffix.
// The 4-way radio contract (REQ-046) and the 2010~2014 ID band are pinned
// unchanged; the separator carries no ID (designer §5.2.3).
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
    // Dynamic label: stem appended when present, "(미등록)" placeholder when
    // empty (designer decision #1, Layer 2). The em-dash join is the spec's.
    TEST_CHECK(tray_src.find("user_model_stem_") != std::string::npos &&
                   tray_src.find("StringId::MenuEngineUserGgufEmpty") != std::string::npos,
               "REQ-047 U1 (b): tray.cpp appends the stem / '(미등록)' placeholder dynamically");
    TEST_CHECK(tray_src.find("L\" — \"") != std::string::npos,
               "REQ-047 U1 (b): tray.cpp joins the dynamic label with the em-dash separator");
    // UpdateStatus carries the new parameter with an empty default (designer
    // §5.2.1) so the hook.cpp call sites stay source-compatible.
    TEST_CHECK(tray_hdr.find("user_model_stem") != std::string::npos,
               "REQ-047 U1 (b): tray.hpp UpdateStatus declares the user_model_stem parameter");

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

    // ---- (d) main.cpp stem-resolution wiring -------------------------------
    if (!main_src.empty()) {
        TEST_CHECK(main_src.find("user_model_stem") != std::string::npos &&
                       main_src.find("FindModel(config.user_model_id)") != std::string::npos,
                   "REQ-047 U1 (d): main.cpp resolves the stem via the default registry");
        TEST_CHECK(main_src.find("rfind(\".gguf\")") != std::string::npos,
                   "REQ-047 U1 (d): main.cpp strips the '.gguf' suffix into the stem");
        // Ordering: the stem resolution lives inside refresh_tray, before the
        // UpdateStatus call it feeds.
        const size_t stem_pos = main_src.find("std::string user_model_stem;");
        const size_t call_pos = main_src.find("tray.UpdateStatus(");
        TEST_CHECK(stem_pos != std::string::npos && call_pos != std::string::npos &&
                       stem_pos < call_pos,
                   "REQ-047 U1 (d): stem resolution precedes the UpdateStatus call");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-047 U1 tray gguf labels + separator passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-047 U1 tray gguf labels + separator: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
