// ---------------------------------------------------------------------------
// REQ-051 Symptom C + Symptom D (OpenAI settings dialog) — pure-logic suite.
// Staged as an .inc next to tests/run_tests.cpp (the runner already includes
// ../src/ui/openai_settings_window.hpp + ../src/config.hpp, so the production
// helpers are linked in directly). Four layers, all GUI/timer/pipe-free:
//   (a) TestReq051ComboSelection — PlanOpenAiComboSelection (Symptom C):
//       exact match -> select that index; absent -> insert+select at
//       last+1 (or 0); empty target -> leave the combo alone; first
//       occurrence wins; case-sensitive; non-contiguous indices.
//   (b) TestReq051ClearOpenAiSettings — ClearOpenAiSettings (Symptom D):
//       exactly the five persisted fields cleared (base_url, model,
//       api_key_dpapi + api_key_sha256 TOGETHER — the refuse-forever pair
//       atomicity, http_consent_given) and a source pin that the predicate
//       names no engine field (engine selection untouched by construction).
//   (c) TestReq051I18nCountAndCoverage — the REQ-051 110-field/enum record
//       (mirrors req048_r2; run_tests.cpp's own mirror is integration-owned)
//       + OpenAiDeleteSettings resolves non-empty in all 37 locales with
//       zero English passthrough outside English itself.
//   (d) TestReq051OpenAiDialogStructuralPins — source pins on
//       src/ui/openai_settings_window.cpp: the refill pins the current model
//       (never index 0), the empty-fetch path leaves the combo untouched, the
//       delete branch persists via AppConfig + ClearOpenAiSettings and ends
//       non-IDOK (kDeleted), and no code path in the TU touches the engine
//       selection.
//   (e) TestReq052OpenAiAsyncFetchPins — REQ-052 source pins on the same TU:
//       the debounce timer no longer runs the synchronous fetch (the worker
//       spawn sits between KillTimer and completion), the ListModels call
//       lives only inside the worker-side helper, digest-before-protect
//       holds at BOTH compositions (fetch probe + IDOK save), the
//       single-flight generation drops stale completions, the heap result
//       crosses PostMessageW with ownership released on every path, and
//       WM_DESTROY flips the worker liveness flag instead of joining.
// ---------------------------------------------------------------------------

namespace {

// read_src with the req047_tests.inc candidate-path precedent (the runner's
// CWD is build/ two levels down from the repo root in the CMake/CTest flow).
// CRLF normalization: several production sources (openai_settings_window.cpp)
// are committed with CRLF terminators; the structural pins below match
// line-anchored needles ("\n}\n" boundaries), so the text is normalized to LF
// before use — a needle match must never depend on the checkout's autocrlf.
void Req051ReadSrc(const char* name, std::string& out) {
    const char* candidates[] = {name, (std::string("../") + name).c_str(),
                                (std::string("../../") + name).c_str()};
    for (const char* cand : candidates) {
        std::ifstream in(cand, std::ios::binary);
        if (in) {
            out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::string norm;
            norm.reserve(out.size());
            for (size_t i = 0; i < out.size(); ++i) {
                if (out[i] == '\r' && i + 1 < out.size() && out[i + 1] == '\n') continue;
                norm.push_back(out[i]);
            }
            out.swap(norm);
            return;
        }
    }
    out.clear();
}

} // namespace

void TestReq051ComboSelection() {
    std::cout << "[RUN] Testing REQ-051 Symptom C combo pick-or-insert plan..." << std::endl;
    const int failures_before = g_failed_count;

    using Items = std::vector<std::pair<int, std::string>>;

    // Empty target: never select, never insert — the combo is left alone
    // (no fabricated first-item selection).
    {
        const auto r = PlanOpenAiComboSelection({}, "");
        TEST_CHECK(r.select_index == -1 && !r.insert_target,
                   "REQ-051 C: empty target + empty list -> leave combo alone");
        const auto r2 = PlanOpenAiComboSelection(Items{{0, "a"}, {1, "b"}}, "");
        TEST_CHECK(r2.select_index == -1 && !r2.insert_target,
                   "REQ-051 C: empty target + populated list -> leave combo alone");
    }

    // Empty list + non-empty target: insert at index 0 and select it.
    {
        const auto r = PlanOpenAiComboSelection({}, "my-model");
        TEST_CHECK(r.select_index == 0 && r.insert_target,
                   "REQ-051 C: empty list -> insert target at index 0 and select it");
    }

    // Exact match: select THAT index, no insert (the Symptom-C core: the
    // saved model must win over the old unconditional index-0 selection).
    {
        const auto r = PlanOpenAiComboSelection(Items{{0, "alpha"}, {1, "beta"}}, "beta");
        TEST_CHECK(r.select_index == 1 && !r.insert_target,
                   "REQ-051 C: exact match selects that index (no insert)");
        // A model id that is a PREFIX of a listed id must NOT match.
        const auto p = PlanOpenAiComboSelection(Items{{0, "gpt-4o"}, {1, "gpt-4o-mini"}},
                                                "gpt-4o");
        TEST_CHECK(p.select_index == 0 && !p.insert_target,
                   "REQ-051 C: exact equality — a listed id matching the whole target wins");
        const auto p2 = PlanOpenAiComboSelection(Items{{0, "gpt-4o-mini"}}, "gpt-4o");
        TEST_CHECK(p2.insert_target && p2.select_index == 1,
                   "REQ-051 C: a prefix of a listed id is NOT a match -> insert");
    }

    // Absent target: insert at last+1 and select the append.
    {
        const auto r = PlanOpenAiComboSelection(Items{{0, "alpha"}, {1, "beta"}}, "gamma");
        TEST_CHECK(r.insert_target && r.select_index == 2,
                   "REQ-051 C: absent target -> insert after the last item (index 2)");
    }

    // Non-contiguous combo indices: the pair<int,string> indices are
    // authoritative (a future diff-update refill may leave gaps).
    {
        const auto r = PlanOpenAiComboSelection(Items{{0, "a"}, {5, "b"}}, "z");
        TEST_CHECK(r.insert_target && r.select_index == 6,
                   "REQ-051 C: append index follows the LAST pair index + 1, not items.size()");
        const auto m = PlanOpenAiComboSelection(Items{{0, "a"}, {5, "b"}}, "b");
        TEST_CHECK(m.select_index == 5 && !m.insert_target,
                   "REQ-051 C: match selects the pair's own index (5), not its position");
    }

    // First occurrence wins on duplicates.
    {
        const auto r = PlanOpenAiComboSelection(Items{{0, "dup"}, {3, "dup"}}, "dup");
        TEST_CHECK(r.select_index == 0 && !r.insert_target,
                   "REQ-051 C: duplicate entries -> the first occurrence is selected");
    }

    // Model ids are case-sensitive: no case folding anywhere.
    {
        const auto r = PlanOpenAiComboSelection(Items{{0, "Model-A"}}, "model-a");
        TEST_CHECK(r.insert_target && r.select_index == 1,
                   "REQ-051 C: matching is case-sensitive (no folded match)");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-051 Symptom C combo selection passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-051 Symptom C combo selection: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestReq051ClearOpenAiSettings() {
    std::cout << "[RUN] Testing REQ-051 Symptom D clear-settings predicate..." << std::endl;
    const int failures_before = g_failed_count;

    // Fully populated block: every one of the five persisted fields clears.
    // The key pair (DPAPI blob + digest) must clear TOGETHER — a half-cleared
    // pair is exactly what made WithUnprotectedKey refuse a valid key
    // forever (REQ-050 b6f98da), so the predicate itself must be atomic.
    {
        OpenAiConfig cfg;
        cfg.base_url = "https://api.example.com/v1";
        cfg.model = "qwen-3.8-27b-flex";
        cfg.api_key_dpapi = "UEJPSVCEN0JMT0I=";   // DPAPI-blob stand-in (ciphertext)
        cfg.api_key_sha256 = std::string(64, 'a'); // digest stand-in
        cfg.http_consent_given = true;
        ClearOpenAiSettings(cfg);
        TEST_CHECK(cfg.base_url.empty(),
                   "REQ-051 D: base_url cleared");
        TEST_CHECK(cfg.model.empty(),
                   "REQ-051 D: model cleared");
        TEST_CHECK(cfg.api_key_dpapi.empty(),
                   "REQ-051 D: api_key_dpapi cleared (pair atomicity, half 1)");
        TEST_CHECK(cfg.api_key_sha256.empty(),
                   "REQ-051 D: api_key_sha256 cleared (pair atomicity, half 2)");
        TEST_CHECK(cfg.http_consent_given == false,
                   "REQ-051 D: http_consent_given reset to false (re-consent required)");
    }

    // Pair-only block: both halves still clear together.
    {
        OpenAiConfig cfg;
        cfg.api_key_dpapi = "blob";
        cfg.api_key_sha256 = "digest";
        ClearOpenAiSettings(cfg);
        TEST_CHECK(cfg.api_key_dpapi.empty() && cfg.api_key_sha256.empty(),
                   "REQ-051 D: a pair-only block still clears both halves together");
    }

    // Already-empty block: idempotent (a repeated delete is a no-op, never a
    // regression into refuse-forever).
    {
        OpenAiConfig cfg;
        ClearOpenAiSettings(cfg);
        TEST_CHECK(cfg.base_url.empty() && cfg.model.empty() &&
                       cfg.api_key_dpapi.empty() && cfg.api_key_sha256.empty() &&
                       !cfg.http_consent_given,
                   "REQ-051 D: clearing an already-empty block is a stable no-op");
    }

    // Structural pin (the engine-selection guarantee): the predicate's
    // definition names exactly the five openai fields and no engine field —
    // OpenAiConfig carries no engine member, so "engine_type untouched" holds
    // by construction and any future engine leak into this helper is a
    // source-visible regression.
    std::string osw_src;
    Req051ReadSrc("src/ui/openai_settings_window.cpp", osw_src);
    if (osw_src.empty()) {
        std::cout << "[SKIP] src/ui/openai_settings_window.cpp not resolvable from the test CWD; "
                     "REQ-051 D predicate pins skipped." << std::endl;
        return;
    }
    {
        const size_t def_pos = osw_src.find("void ClearOpenAiSettings(OpenAiConfig& cfg) {");
        TEST_CHECK(def_pos != std::string::npos,
                   "REQ-051 D structural pin: ClearOpenAiSettings definition is locatable");
        if (def_pos != std::string::npos) {
            const size_t def_end = osw_src.find("\n}\n", def_pos);
            const std::string body = osw_src.substr(
                def_pos, def_end == std::string::npos ? std::string::npos : def_end - def_pos);
            TEST_CHECK(body.find("base_url") != std::string::npos &&
                           body.find("model") != std::string::npos &&
                           body.find("api_key_dpapi") != std::string::npos &&
                           body.find("api_key_sha256") != std::string::npos &&
                           body.find("http_consent_given") != std::string::npos,
                       "REQ-051 D structural pin: the predicate clears all five fields");
            TEST_CHECK(body.find("engine") == std::string::npos &&
                           body.find("Engine") == std::string::npos,
                       "REQ-051 D structural pin: the predicate never touches engine state");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-051 Symptom D clear predicate passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-051 Symptom D clear predicate: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestReq051I18nCountAndCoverage() {
    std::cout << "[RUN] Testing REQ-051 i18n count record + delete-caption coverage..."
              << std::endl;
    const int failures_before = g_failed_count;

    // The REQ-051 record: EnumCount advanced 109 -> 110 (the
    // OpenAiDeleteSettings tail append). Read through a volatile to keep the
    // runtime comparison (avoids C4127 on the constant expression).
    volatile int enum_count = static_cast<int>(StringId::EnumCount);
    TEST_CHECK(enum_count == 110,
               "REQ-051: StringId::EnumCount is 110 after the OpenAiDeleteSettings append");

    // The new caption resolves non-empty in all 37 authored tables, and no
    // non-English locale falls back to the English text (real translations
    // mandate; TestReq048R2I18nCoverage's extended range pins the same gate
    // across the whole recent-field band).
    const UiLocale initial = I18n::GetCurrentLocale();
    I18n::SetLocale(UiLocale::English);
    const std::wstring en = I18n::Get(StringId::OpenAiDeleteSettings);
    TEST_CHECK(en == L"Delete settings",
               "REQ-051: en OpenAiDeleteSettings is the reference text");
    int blank = 0;
    int english_copy = 0;
    for (int li = static_cast<int>(UiLocale::Korean);
         li <= static_cast<int>(UiLocale::Burmese); ++li) {
        if (li == static_cast<int>(UiLocale::English)) continue;
        I18n::SetLocale(static_cast<UiLocale>(li));
        const std::wstring s = I18n::Get(StringId::OpenAiDeleteSettings);
        if (s.empty()) ++blank;
        if (s == en) ++english_copy;
    }
    I18n::SetLocale(initial); // suite-state hygiene
    TEST_CHECK(blank == 0,
               "REQ-051: OpenAiDeleteSettings is non-empty in all 37 locales");
    TEST_CHECK(english_copy == 0,
               "REQ-051: OpenAiDeleteSettings is a real translation in every non-English locale");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-051 i18n count + coverage passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-051 i18n count + coverage: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestReq051OpenAiDialogStructuralPins() {
    std::cout << "[RUN] Testing REQ-051 OpenAI dialog structural pins..." << std::endl;
    const int failures_before = g_failed_count;

    std::string osw_src;
    Req051ReadSrc("src/ui/openai_settings_window.cpp", osw_src);
    std::string i18n_src;
    Req051ReadSrc("src/i18n.cpp", i18n_src);
    if (osw_src.empty()) {
        std::cout << "[SKIP] src/ui/openai_settings_window.cpp not resolvable from the test CWD; "
                     "REQ-051 structural pins skipped." << std::endl;
        return;
    }

    // ---- (a) Symptom C refill pins ----------------------------------------
    // The refill plans the selection from the current combo text via the
    // pure helper — the old unconditional CB_SETCURSEL index 0 is gone.
    TEST_CHECK(osw_src.find("PlanOpenAiComboSelection(items, target)") != std::string::npos,
               "REQ-051 C pin: the refill plans the selection from the current combo text");
    TEST_CHECK(osw_src.find("if (!models.empty()) ::SendMessageW(combo, CB_SETCURSEL, 0, 0);") ==
                   std::string::npos,
               "REQ-051 C pin: the unconditional index-0 selection is gone");
    // The current-model text is captured BEFORE CB_RESETCONTENT (the reset
    // wipes the edit text of a CBS_DROPDOWN combo). The send itself is
    // matched exactly — earlier comment text also names the message.
    const size_t reset_pos = osw_src.find("::SendMessageW(combo, CB_RESETCONTENT, 0, 0);");
    const size_t target_pos = osw_src.find("GetCtrlText(dlg, IDC_MODEL_COMBO));");
    TEST_CHECK(reset_pos != std::string::npos && target_pos != std::string::npos,
               "REQ-051 C pin: reset and target-capture are both locatable");
    if (reset_pos != std::string::npos && target_pos != std::string::npos) {
        // The pre-reset capture lives inside RefillOpenAiModelCombo; the FIRST
        // reset send in the TU is the refill's, so the capture must
        // appear at or before it (the INIT-time SetCtrlText is an edit,
        // not a GetCtrlText of the combo).
        TEST_CHECK(target_pos < reset_pos,
                   "REQ-051 C pin: the current-model text is read BEFORE CB_RESETCONTENT");
    }
    // Empty fetch: the combo (and its injected current-model text) is left
    // untouched — the early return precedes the refill's reset.
    {
        const size_t empty_pos = osw_src.find("if (models.empty()) {");
        TEST_CHECK(empty_pos != std::string::npos && empty_pos < reset_pos,
                   "REQ-051 C pin: an empty fetch returns before the combo reset "
                   "(text-injection path preserved)");
    }

    // ---- (b) Symptom D delete-branch pins ---------------------------------
    {
        const size_t del_pos = osw_src.find("case IDC_DELETE_BTN");
        TEST_CHECK(del_pos != std::string::npos,
                   "REQ-051 D pin: the delete branch exists");
        if (del_pos != std::string::npos) {
            // The branch body ends at the closing "        }" of the case;
            // the next top-level "case " label (there is none after it)
            // bounds it — take a generous window and pin within it.
            const std::string branch = osw_src.substr(del_pos, 3500);
            TEST_CHECK(branch.find("AppConfig persisted;") != std::string::npos,
                       "REQ-051 D pin: the delete persists through an AppConfig instance");
            TEST_CHECK(branch.find("persisted.LoadFromFile()") != std::string::npos,
                       "REQ-051 D pin: the delete loads the canonical config first");
            TEST_CHECK(branch.find("ClearOpenAiSettings(persisted.openai)") != std::string::npos,
                       "REQ-051 D pin: the delete clears exactly the five openai fields");
            TEST_CHECK(branch.find("persisted.SaveToFile()") != std::string::npos,
                       "REQ-051 D pin: the delete saves the cleared config");
            TEST_CHECK(branch.find("st->deleted = true;") != std::string::npos &&
                           branch.find("::EndDialog(dlg, IDC_DELETE_BTN);") != std::string::npos,
                       "REQ-051 D pin: the delete ends with a NON-IDOK code (kDeleted outcome)");
            // Ordering: clear BEFORE save (a save of the uncleared block
            // would persist nothing).
            const size_t clear_pos = branch.find("ClearOpenAiSettings(persisted.openai)");
            const size_t save_pos = branch.find("persisted.SaveToFile()");
            TEST_CHECK(clear_pos != std::string::npos && save_pos != std::string::npos &&
                           clear_pos < save_pos,
                       "REQ-051 D pin: clear precedes save in the delete branch");
            // The delete branch must NEVER flip the engine selection — the
            // whole TU is engine-free (no SetEngineType* call anywhere).
            TEST_CHECK(branch.find("SetEngineType") == std::string::npos,
                       "REQ-051 D pin: the delete branch never touches the engine selection");
        }
    }
    TEST_CHECK(osw_src.find("SetEngineType") == std::string::npos &&
                   osw_src.find("SetEngineTypeName") == std::string::npos,
               "REQ-051 D pin: openai_settings_window.cpp has no engine mutation at all");
    // The tri-state contract: the deleted outcome is reported distinctly and
    // the bool wrapper reports only kSaved (pre-REQ-051 callers keep cancel
    // semantics for a delete — engine untouched, no save/switch side effects).
    TEST_CHECK(osw_src.find("OpenAiSettingsOutcome::kDeleted") != std::string::npos,
               "REQ-051 D pin: ShowOpenAiSettingsDialogEx reports the kDeleted outcome");
    TEST_CHECK(osw_src.find("ShowOpenAiSettingsDialogEx(parent, cfg) == OpenAiSettingsOutcome::kSaved") !=
                   std::string::npos,
               "REQ-051 D pin: the bool wrapper is true only for kSaved");
    // The delete button is a real template control (i18n caption).
    TEST_CHECK(osw_src.find("IDC_DELETE_BTN") != std::string::npos &&
                   osw_src.find("StringId::OpenAiDeleteSettings") != std::string::npos,
               "REQ-051 D pin: the template wires the i18n delete caption to IDC_DELETE_BTN");

    // ---- (c) i18n plumbing pins -------------------------------------------
    if (!i18n_src.empty()) {
        TEST_CHECK(i18n_src.find("X(openai_delete_settings)") != std::string::npos,
                   "REQ-051 pin: the X-macro carries the openai_delete_settings field");
        TEST_CHECK(i18n_src.find("case StringId::OpenAiDeleteSettings:") != std::string::npos,
                   "REQ-051 pin: the Get() switch maps OpenAiDeleteSettings");
        TEST_CHECK(i18n_src.find(".openai_delete_settings = L\"설정 삭제\",") != std::string::npos,
                   "REQ-051 pin: the Korean designated initializer carries the caption");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-051 OpenAI dialog structural pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-051 OpenAI dialog structural pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// REQ-052 (OpenAI settings dialog freeze fix) — structural pins on
// src/ui/openai_settings_window.cpp. The synchronous ListModels call moved
// off the GUI thread onto a detached worker (audit P0: the dialog went
// '응답 없음' mid-typing while the 800 ms debounce fired the 10 s WinHTTP
// call inline). All pins are source-textual: the dialog logic is GUI code.
// ---------------------------------------------------------------------------
void TestReq052OpenAiAsyncFetchPins() {
    std::cout << "[RUN] Testing REQ-052 OpenAI async model-fetch pins..." << std::endl;
    const int failures_before = g_failed_count;

    std::string osw_src;
    Req051ReadSrc("src/ui/openai_settings_window.cpp", osw_src);
    if (osw_src.empty()) {
        std::cout << "[SKIP] src/ui/openai_settings_window.cpp not resolvable from the test CWD; "
                     "REQ-052 async-fetch pins skipped." << std::endl;
        return;
    }

    // (a) The debounce timer SPAWNS the fetch; it no longer runs any
    // synchronous network call itself.
    const size_t timer_pos = osw_src.find("case WM_TIMER:");
    const size_t timer_end = osw_src.find("case WM_COMMAND:");
    TEST_CHECK(timer_pos != std::string::npos && timer_end != std::string::npos &&
                   timer_pos < timer_end,
               "REQ-052 pin: the WM_TIMER branch is locatable");
    if (timer_pos != std::string::npos && timer_end != std::string::npos &&
        timer_pos < timer_end) {
        const std::string timer_branch = osw_src.substr(timer_pos, timer_end - timer_pos);
        TEST_CHECK(timer_branch.find("KillTimer") != std::string::npos,
                   "REQ-052 pin: WM_TIMER still kills the debounce timer first");
        TEST_CHECK(timer_branch.find("DoFetchModels(dlg, st);") != std::string::npos,
                   "REQ-052 pin: WM_TIMER drives the async fetch spawn");
        TEST_CHECK(timer_branch.find("ListModels") == std::string::npos &&
                       timer_branch.find("OpenAiCompatibleClient") == std::string::npos,
                   "REQ-052 pin: WM_TIMER performs no synchronous network call");
    }

    // (b) The network boundary lives only off the GUI thread: exactly one
    // ListModels call site — inside the 1-arg probe helper — which the
    // detached worker invokes; the GUI spawn (2-arg overload) starts the
    // thread but performs no I/O itself.
    const size_t lm_pos = osw_src.find("OpenAiCompatibleClient::ListModels");
    TEST_CHECK(lm_pos != std::string::npos &&
                   osw_src.find("OpenAiCompatibleClient::ListModels", lm_pos + 1) ==
                       std::string::npos,
               "REQ-052 pin: exactly one ListModels call site remains");
    const size_t probe_fn_pos = osw_src.find(
        "std::vector<std::string> DoFetchModels(const OpenAiConfig& probe)");
    const size_t worker_pos = osw_src.find("void OpenAiFetchWorker(");
    TEST_CHECK(probe_fn_pos != std::string::npos && worker_pos != std::string::npos &&
                   probe_fn_pos < lm_pos && lm_pos < worker_pos,
               "REQ-052 pin: the ListModels call is the worker-side fetch helper");
    if (worker_pos != std::string::npos) {
        const size_t worker_end = osw_src.find("\n}\n", worker_pos);
        const std::string worker_body = osw_src.substr(
            worker_pos,
            worker_end == std::string::npos ? std::string::npos : worker_end - worker_pos);
        TEST_CHECK(worker_body.find("DoFetchModels(probe)") != std::string::npos,
                   "REQ-052 pin: the worker invokes the fetch helper");
    }
    const size_t spawn_pos =
        osw_src.find("bool DoFetchModels(HWND dlg, OpenAiDialogState* st)");
    const size_t proc_pos = osw_src.find("INT_PTR CALLBACK OpenAiSettingsProc");
    TEST_CHECK(spawn_pos != std::string::npos && proc_pos != std::string::npos &&
                   spawn_pos < proc_pos,
               "REQ-052 pin: the GUI spawn precedes the dialog proc");
    if (spawn_pos != std::string::npos && proc_pos != std::string::npos &&
        spawn_pos < proc_pos) {
        const std::string spawn_src = osw_src.substr(spawn_pos, proc_pos - spawn_pos);
        TEST_CHECK(spawn_src.find("std::thread(OpenAiFetchWorker") != std::string::npos &&
                       spawn_src.find(".detach()") != std::string::npos,
                   "REQ-052 pin: the GUI spawn starts the detached worker thread");
        TEST_CHECK(spawn_src.find("ListModels") == std::string::npos &&
                       spawn_src.find("OpenAiCompatibleClient") == std::string::npos,
                   "REQ-052 pin: the GUI spawn performs no network I/O itself");
    }

    // (c) digest-BEFORE-protect (REQ-050 b6f98da contract) at BOTH
    // compositions: the fetch probe and the IDOK save.
    const size_t fetch_sha_pos =
        osw_src.find("OpenAiSha256Hex(keyUtf8, probe.api_key_sha256);");
    const size_t fetch_prot_pos =
        osw_src.find("ProtectOpenAiApiKey(keyUtf8, probe.api_key_dpapi);");
    TEST_CHECK(fetch_sha_pos != std::string::npos && fetch_prot_pos != std::string::npos &&
                   fetch_sha_pos < fetch_prot_pos,
               "REQ-052 pin: the fetch probe keeps digest-before-protect");
    const size_t ok_sha_pos = osw_src.find("OpenAiSha256Hex(keyUtf8, out.api_key_sha256);");
    const size_t ok_prot_pos = osw_src.find("ProtectOpenAiApiKey(keyUtf8, out.api_key_dpapi)");
    TEST_CHECK(ok_sha_pos != std::string::npos && ok_prot_pos != std::string::npos &&
                   ok_sha_pos < ok_prot_pos,
               "REQ-052 pin: the IDOK save keeps digest-before-protect");

    // (d) Single-flight: the spawn bumps the generation; the completion
    // handler drops any completion whose generation is not current.
    TEST_CHECK(osw_src.find("++st->fetch_generation;") != std::string::npos &&
                   osw_src.find("result->generation != st->fetch_generation") !=
                       std::string::npos,
               "REQ-052 pin: single-flight generation — stale completions are dropped");

    // (e) The heap result crosses PostMessageW with ownership transfer; the
    // worker has a self-release fallback (failed post / dialog gone) and the
    // GUI handler deletes on every path (current + stale/orphaned).
    TEST_CHECK(osw_src.find("new OpenAiFetchResult") != std::string::npos &&
                   osw_src.find("::PostMessageW(dlg, kOpenAiMsgFetchDone") !=
                       std::string::npos,
               "REQ-052 pin: the worker delivers the heap result via PostMessage");
    size_t release_count = 0;
    for (size_t p = osw_src.find("delete result;"); p != std::string::npos;
         p = osw_src.find("delete result;", p + 1)) {
        ++release_count;
    }
    TEST_CHECK(release_count >= 2,
               "REQ-052 pin: both the worker fallback and the GUI handler release the result");
    const size_t done_pos = osw_src.find("case kOpenAiMsgFetchDone:");
    TEST_CHECK(done_pos != std::string::npos &&
                   osw_src.find("delete result;", done_pos) != std::string::npos &&
                   osw_src.find("RefillOpenAiModelCombo(dlg, models);", done_pos) !=
                       std::string::npos,
               "REQ-052 pin: the completion handler refills via the extracted helper "
               "and releases the result");

    // (f) WM_DESTROY liveness guard: no join (joining a worker still inside
    // its bounded network call would re-block the GUI thread this fix freed);
    // the shared flag tells a finishing worker the receiver is gone.
    const size_t destroy_pos = osw_src.find("case WM_DESTROY:");
    TEST_CHECK(destroy_pos != std::string::npos,
               "REQ-052 pin: the dialog proc handles WM_DESTROY");
    if (destroy_pos != std::string::npos) {
        const std::string destroy_branch = osw_src.substr(destroy_pos, 900);
        TEST_CHECK(destroy_branch.find("fetch_alive->store(false") != std::string::npos,
                   "REQ-052 pin: WM_DESTROY flips the worker liveness flag");
    }

    // (g) The REQ-050/REQ-051 contracts survive the async move: the 800 ms
    // debounce is intact, the refill still plans via the REQ-051 helper
    // (never index 0), and the failure notice stays manual-only.
    TEST_CHECK(osw_src.find("kFetchDebounceMs = 800") != std::string::npos,
               "REQ-052 pin: the 800 ms debounce timer is unchanged");
    TEST_CHECK(osw_src.find("PlanOpenAiComboSelection(items, target)") != std::string::npos,
               "REQ-052 pin: the refill still plans the selection from the current text");
    TEST_CHECK(osw_src.find("if (manual && models.empty()) {") != std::string::npos,
               "REQ-052 pin: the failure notice stays manual-only (auto path silent)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-052 OpenAI async model-fetch pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-052 OpenAI async model-fetch pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
