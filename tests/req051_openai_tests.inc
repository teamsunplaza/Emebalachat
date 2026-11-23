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

// 261121_0001 fix round 1 (F-4): the reasoning-combo population planner, defined
// at namespace scope in src/ui/openai_settings_window.cpp (the same split as the
// other dialog helpers the suites drive, e.g. PlanReasoningEffortItems).
// Declared here because openai_settings_window.hpp is outside the unit's write
// boundary and the suite must drive the EXACT reconciliation the dialog runs.
namespace emebalachat {
void PlanOpenAiReasoningComboPopulate(const std::vector<std::string>& items,
                                      const std::string& value,
                                      bool pending,
                                      int& out_select,
                                      std::string& out_effective,
                                      bool& out_pending);
} // namespace emebalachat

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
        cfg.reasoning_effort = "low";               // 261121_0001 U-F (C2) field
        cfg.api_key_dpapi = "UEJPSVCEN0JMT0I=";   // DPAPI-blob stand-in (ciphertext)
        cfg.api_key_sha256 = std::string(64, 'a'); // digest stand-in
        cfg.http_consent_given = true;
        ClearOpenAiSettings(cfg);
        TEST_CHECK(cfg.base_url.empty(),
                   "REQ-051 D: base_url cleared");
        TEST_CHECK(cfg.model.empty(),
                   "REQ-051 D: model cleared");
        TEST_CHECK(cfg.reasoning_effort.empty(),
                   "C2 (261121_0001 U-F): the reasoning_effort choice is cleared with the block");
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
                           body.find("reasoning_effort") != std::string::npos &&
                           body.find("api_key_dpapi") != std::string::npos &&
                           body.find("api_key_sha256") != std::string::npos &&
                           body.find("http_consent_given") != std::string::npos,
                       "REQ-051 D structural pin: the predicate clears every persisted openai "
                       "field (incl. the 261121_0001 U-F reasoning_effort)");
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

    // The REQ-051 record advanced 109 -> 110 (the OpenAiDeleteSettings tail
    // append); REQ-UC 260930_0004 then advanced 110 -> 135 (the 25-string
    // update-zone block); REQ-MD advanced 135 -> 144 (the 9-string model
    // download dialog); REQ-060 advanced 144 -> 157 (the 13 load-mode/policy
    // strings); 261121_0001 U-C advanced 199 -> 203 (the 4 v0.10.9
    // reasoning/back-trans section strings); 261121_0002 took it down by 1,
    // 203 -> 202 (the dead SEC-M1 translate_truncated_notice removal). Read
    // through a volatile to keep
    // the runtime
    // comparison (avoids C4127 on the constant expression).
    volatile int enum_count = static_cast<int>(StringId::EnumCount);
    TEST_CHECK(enum_count == 248,
               "REQ-051: StringId::EnumCount is 248 after the OpenAiDeleteSettings + REQ-UC + REQ-MD + REQ-060 + v0.10.5 theme appends + the 5 261108_0002 WS3 back-translation ids (FIX-WS3B added BackTranslateReference) + the 3 261111_0001 U6 user-model ids + the 2 261116_0003 P5 language-picker ids + the 1 261116_0003 P6a flat-menu search-row id + the 1 261116_0003 P7 engine-chip count id + the 23 261116_0003 P9b-1 user-language surface ids + the 1 261116_0003 P9b-3 U5 picker AUTO chip id + the 2 261116_0003 P9c-1b OpenAI declaration checklist ids + the 4 261121_0001 v0.10.9 U-C ids (2 OpenAI reasoning-effort ids + 2 back-translation section ids) - the 1 dead 261121_0002 SEC-M1 translate_truncated_notice id + the 5 261121_0002 progress/cancel ids + the 6 261122_0001 B-U2 OpenAI profile ids + the 8 261122_0002 Phase-2 translation-card editor ids + the 7 261122_0002 Phase-3 settings-window redesign ids + the 19 261122_0002 Phase-3 micro-pass ids + the 1 261123_0001 user-model load-failure hint id");

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
            // The branch body ends at the NEXT case label (IDC_PROFILE_SAVE_BTN).
            // 261122_0001 fix-A2 (R1-01) grew this branch, so the old
            // fixed 3500-character window silently truncated the pins past
            // persisted.SaveToFile(); bound it by the label instead.
            // 261122_0001 fix-E (round-2 glm r2 N4): a MISSING bounding label
            // must HARD-FAIL (the R1-07 pattern), never degrade the window to
            // the whole file tail — pins matching tokens from later branches
            // would pass vacuously on a structural change.
            const size_t next_case = osw_src.find("case IDC_PROFILE_SAVE_BTN", del_pos);
            TEST_CHECK(next_case != std::string::npos,
                       "REQ-051 D pin: the IDC_PROFILE_SAVE_BTN bounding label exists "
                       "(the delete-branch window must not degrade to the file tail)");
            const std::string branch =
                next_case != std::string::npos
                    ? osw_src.substr(del_pos, next_case - del_pos)
                    : osw_src.substr(del_pos, 0);
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

    // ---- (d) fix round 1 (F-4): the saved reasoning effort survives --------
    // The dialog's first population always runs against the FALLBACK list (no
    // fetch has completed), which cannot carry a server-extension value such as
    // OpenRouter's "xhigh". Before this fix the fallback's server-default item
    // overwrote the state, so the saved value was lost even when a catalog
    // advertising it arrived a moment later. The pins drive the production
    // planner through the exact round trip the dialog performs.
    {
        using emebalachat::PlanOpenAiReasoningComboPopulate;
        using emebalachat::PlanReasoningEffortItems;
        const std::vector<std::string> fallback = PlanReasoningEffortItems({});
        const auto index_of = [](const std::vector<std::string>& items,
                                 const std::string& value) {
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (items[i] == value) return static_cast<int>(i);
            }
            return -1;
        };

        // 1. WM_INITDIALOG: the fallback list cannot show "xhigh" -> the combo
        //    selects the server-default item, but the VALUE is kept pending.
        int select = -1;
        std::string effective;
        bool pending = false;
        PlanOpenAiReasoningComboPopulate(fallback, "xhigh", /*pending=*/true, select,
                                         effective, pending);
        TEST_CHECK(select == 0 && effective == "xhigh" && pending,
                   "F-4: the pre-fetch fallback population keeps the saved \"xhigh\" "
                   "(combo on the server-default item, value still carried)");

        // 2. A catalog that does NOT advertise it: still kept, still pending, so
        //    IDOK persists the user's actual choice unchanged.
        PlanOpenAiReasoningComboPopulate(fallback, effective, pending, select, effective,
                                         pending);
        TEST_CHECK(select == 0 && effective == "xhigh" && pending,
                   "F-4: a catalog without \"xhigh\" leaves the combo at server-default but "
                   "still saves the user's choice");

        // 3. A catalog that DOES advertise it: selected, pending cleared.
        const std::vector<std::string> advertised =
            PlanReasoningEffortItems({"low", "medium", "xhigh"});
        const int xhigh_index = index_of(advertised, "xhigh");
        PlanOpenAiReasoningComboPopulate(advertised, effective, pending, select, effective,
                                         pending);
        TEST_CHECK(xhigh_index > 0 && select == xhigh_index && effective == "xhigh" &&
                       !pending,
                   "F-4: a catalog that advertises \"xhigh\" selects it and ends the pending "
                   "restoration (the silent preference loss is closed)");

        // 4. The documented convergence rule survives for a value that WAS
        //    showable but the model does not offer (decision 7): "" = server
        //    default, and no stale value is carried.
        const std::vector<std::string> limited = PlanReasoningEffortItems({"low"});
        PlanOpenAiReasoningComboPopulate(limited, "high", /*pending=*/false, select,
                                         effective, pending);
        TEST_CHECK(select == 0 && effective.empty() && !pending,
                   "F-4: a shown-then-unavailable standard value still converges on the "
                   "server default (decision 7 preserved)");

        // 5. An explicit pick of the server-default item (a real "" value with no
        //    pending restoration) persists "" — never a stale effort.
        PlanOpenAiReasoningComboPopulate(fallback, "", /*pending=*/false, select, effective,
                                         pending);
        TEST_CHECK(select == 0 && effective.empty() && !pending,
                   "F-4: selecting the server-default item persists the empty value");
    }
    // The wiring: the population reconciles through the planner against the
    // STATE value (not the rebuilt combo), IDOK keeps the pending value, and an
    // explicit reasoning pick ends the restoration.
    TEST_CHECK(osw_src.find("PlanOpenAiReasoningComboPopulate(items, previous, "
                            "st->reasoning_effort_pending,") != std::string::npos,
               "F-4 pin: RefreshReasoningCombo reconciles through the pure planner");
    TEST_CHECK(osw_src.find("const std::string previous = st->reasoning_effort;") !=
                   std::string::npos,
               "F-4 pin: the population reads the effective state, not the combo that is "
               "about to be reset");
    TEST_CHECK(osw_src.find("out.reasoning_effort = st->reasoning_effort;") !=
                   std::string::npos,
               "F-4 pin: IDOK overrides the combo value with the pending saved value");
    TEST_CHECK(osw_src.find("st->reasoning_effort_pending = !st->reasoning_effort.empty();") !=
                   std::string::npos,
               "F-4 pin: WM_INITDIALOG arms the pending restoration from the saved value");
    TEST_CHECK(osw_src.find("LOWORD(wp) == IDC_REASONING_COMBO && HIWORD(wp) == CBN_SELCHANGE") !=
                   std::string::npos,
               "F-4 pin: an explicit reasoning pick ends the pending restoration");

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
    // model-fetch call site — inside the 1-arg probe helper — which the
    // detached worker invokes; the GUI spawn (2-arg overload) starts the
    // thread but performs no I/O itself.
    // 261121_0001 U-F (v0.10.9, C4): the client grew a combined entry point
    // (FetchModelsAndCapabilities — ids + per-model reasoning-effort hints
    // from the SAME /v1/models GET). The dialog may keep ListModels (ids only)
    // or move to the combined call; what must not change is the contract this
    // pin defends: ONE fetch site, inside the worker-side helper. Accepting
    // either entry point keeps that meaning without forcing the dialog unit's
    // hand.
    const size_t lm_pos = osw_src.find("OpenAiCompatibleClient::ListModels");
    const size_t caps_pos =
        osw_src.find("OpenAiCompatibleClient::FetchModelsAndCapabilities(");
    const size_t fetch_pos = lm_pos != std::string::npos ? lm_pos : caps_pos;
    TEST_CHECK(fetch_pos != std::string::npos &&
                   (lm_pos == std::string::npos) != (caps_pos == std::string::npos),
               "REQ-052 pin: exactly one model-fetch call site remains "
               "(ListModels or the C4 FetchModelsAndCapabilities)");
    const size_t probe_fn_pos = osw_src.find(
        "OpenAiModelCatalog FetchOpenAiModelCatalog(const OpenAiConfig& probe)");
    const size_t worker_pos = osw_src.find("void OpenAiFetchWorker(");
    TEST_CHECK(probe_fn_pos != std::string::npos && worker_pos != std::string::npos &&
                   probe_fn_pos < fetch_pos && fetch_pos < worker_pos,
               "REQ-052 pin: the model-fetch call is the worker-side fetch helper");
    if (worker_pos != std::string::npos) {
        const size_t worker_end = osw_src.find("\n}\n", worker_pos);
        const std::string worker_body = osw_src.substr(
            worker_pos,
            worker_end == std::string::npos ? std::string::npos : worker_end - worker_pos);
        TEST_CHECK(worker_body.find("FetchOpenAiModelCatalog(probe)") != std::string::npos,
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
    // handler drops any completion whose generation is not current. R1-9: the
    // comparison runs on the DRAINED VALUE (`result.`), never through a
    // pointer the message handed over.
    TEST_CHECK(osw_src.find("++st->fetch_generation;") != std::string::npos &&
                   osw_src.find("result.generation != st->fetch_generation") !=
                       std::string::npos,
               "REQ-052 pin: single-flight generation — stale completions are dropped");

    // (e) R1-9 (v0.10.4 sweep, CWE-822 fix): the fetch result travels BY
    // VALUE through the mutex-guarded queue and the completion message is a
    // PURE (0, 0) wake-up. The pre-fix transport handed a heap
    // OpenAiFetchResult* to PostMessageW of a top-level system-class #32770
    // dialog and the handler dereferenced + delete'd whatever LPARAM it got —
    // an arbitrary-free primitive for any same-session same-IL peer. All three
    // pieces of that shape must be gone, and the safe shape must be present.
    TEST_CHECK(osw_src.find("new OpenAiFetchResult") == std::string::npos &&
                   osw_src.find("reinterpret_cast<OpenAiFetchResult*>") ==
                       std::string::npos &&
                   osw_src.find("delete result") == std::string::npos,
               "R1-9 pin: no heap result crosses PostMessageW (pointer transport removed)");
    TEST_CHECK(osw_src.find("std::mutex g_openai_fetch_mu;") != std::string::npos &&
                   osw_src.find("std::deque<OpenAiFetchResult> g_openai_fetch_queue;") !=
                       std::string::npos,
               "R1-9 pin: the by-value fetch-result queue and its mutex exist");
    TEST_CHECK(osw_src.find("g_openai_fetch_queue.push_back(std::move(result));") !=
                   std::string::npos,
               "R1-9 pin: the worker enqueues the result BY VALUE under the queue mutex");
    TEST_CHECK(osw_src.find("::PostMessageW(dlg, kOpenAiMsgFetchDone, 0, 0)") !=
                   std::string::npos,
               "R1-9 pin: the completion message is a pure (0,0) wake-up — no heap pointer "
               "in LPARAM");
    const size_t done_pos = osw_src.find("case kOpenAiMsgFetchDone:");
    const size_t done_end = osw_src.find("case WM_DESTROY:", done_pos);
    TEST_CHECK(done_pos != std::string::npos && done_end != std::string::npos &&
                   done_pos < done_end,
               "R1-9 pin: the completion handler is locatable");
    if (done_pos != std::string::npos && done_end != std::string::npos && done_pos < done_end) {
        const std::string done_branch = osw_src.substr(done_pos, done_end - done_pos);
        TEST_CHECK(done_branch.find("reinterpret_cast") == std::string::npos &&
                       done_branch.find("(lp)") == std::string::npos,
                   "R1-9 pin: the completion handler never casts or dereferences the LPARAM");
        TEST_CHECK(done_branch.find("g_openai_fetch_queue.pop_front();") != std::string::npos &&
                       done_branch.find("RefillOpenAiModelCombo(dlg, result.catalog.models);") !=
                           std::string::npos,
                   "R1-9 pin: the completion handler drains the by-value queue and refills via "
                   "the extracted helper");
    }
    // Teardown: every drain point the transport relies on is present — the
    // in-dialog WM_INITDIALOG reset, the WM_DESTROY sweep, and the post-modal
    // sweep after DialogBoxIndirectParamW returns.
    size_t drain_count = 0;
    for (size_t p = osw_src.find("DrainOpenAiFetchQueue();"); p != std::string::npos;
         p = osw_src.find("DrainOpenAiFetchQueue();", p + 1)) {
        ++drain_count;
    }
    TEST_CHECK(drain_count >= 3,
               "R1-9 pin: the queue is drained on instance start, WM_DESTROY, and dialog return");

    // (f) WM_DESTROY liveness guard: no join (joining a worker still inside
    // its bounded network call would re-block the GUI thread this fix freed);
    // the shared flag tells a finishing worker the receiver is gone, and the
    // same branch sweeps the by-value queue (R1-9).
    const size_t destroy_pos = osw_src.find("case WM_DESTROY:");
    TEST_CHECK(destroy_pos != std::string::npos,
               "REQ-052 pin: the dialog proc handles WM_DESTROY");
    if (destroy_pos != std::string::npos) {
        // V10F-R1 F-18 grew the branch (the generation-bump block + comment
        // before the drain), so the drain now sits ~1.6 KB into the branch:
        // the window widened 1400 -> 2400 to keep covering it.
        const std::string destroy_branch = osw_src.substr(destroy_pos, 2400);
        TEST_CHECK(destroy_branch.find("fetch_alive->store(false") != std::string::npos,
                   "REQ-052 pin: WM_DESTROY flips the worker liveness flag");
        TEST_CHECK(destroy_branch.find("DrainOpenAiFetchQueue();") != std::string::npos,
                   "R1-9 pin: WM_DESTROY drains the by-value fetch-result queue");
    }

    // (g) The REQ-050/REQ-051 contracts survive the async move: the 800 ms
    // debounce is intact, the refill still plans via the REQ-051 helper
    // (never index 0), and the failure notice stays manual-only.
    TEST_CHECK(osw_src.find("kFetchDebounceMs = 800") != std::string::npos,
               "REQ-052 pin: the 800 ms debounce timer is unchanged");
    TEST_CHECK(osw_src.find("PlanOpenAiComboSelection(items, target)") != std::string::npos,
               "REQ-052 pin: the refill still plans the selection from the current text");
    TEST_CHECK(osw_src.find("if (manual && fetch_empty) {") != std::string::npos,
               "REQ-052 pin: the failure notice stays manual-only (auto path silent)");

    // (h) 261121_0001 U-F (v0.10.9, vp-plan C4): the dialog's model fetch stays
    // ONE GET. The client's combined entry point returns the id list and the
    // per-model reasoning-effort hints from the SAME /v1/models body, so the
    // settings dialog can learn a model's supported efforts without a second
    // request (and without the dialog unit owning any parsing).
    {
        std::string client_hpp;
        Req051ReadSrc("src/openai_compatible_client.hpp", client_hpp);
        std::string client_cpp;
        Req051ReadSrc("src/openai_compatible_client.cpp", client_cpp);
        TEST_CHECK(!client_hpp.empty() && !client_cpp.empty(),
                   "C4 pin: the OpenAI client TU resolves for the single-fetch pins");
        if (!client_hpp.empty() && !client_cpp.empty()) {
            TEST_CHECK(client_hpp.find("static bool FetchModelsAndCapabilities(") !=
                               std::string::npos &&
                           client_hpp.find("ParseOpenAiModelCapabilitiesJson(") !=
                               std::string::npos,
                       "C4 pin: the client declares the combined fetch and the capability parser");
            // Exactly ONE /v1/models request literal: the shared fetch helper.
            size_t models_path_sites = 0;
            for (size_t p = client_cpp.find("L\"/v1/models\""); p != std::string::npos;
                 p = client_cpp.find("L\"/v1/models\"", p + 1)) {
                ++models_path_sites;
            }
            TEST_CHECK(models_path_sites == 1,
                       "C4 pin: exactly one /v1/models request path exists (no second network call)");
            TEST_CHECK(client_cpp.find("if (ids) *ids = ParseModelsJson(response);") !=
                               std::string::npos &&
                           client_cpp.find("if (caps) *caps = ParseModelCapabilitiesJson(response);") !=
                               std::string::npos,
                       "C4 pin: one response body is parsed twice (ids + capabilities)");
            TEST_CHECK(client_cpp.find("FetchModelsAndCapabilities(cfg, &ids, nullptr)") !=
                           std::string::npos,
                       "C4 pin: ListModels delegates to the shared fetch (its signature and "
                       "behaviour are unchanged)");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-052 OpenAI async model-fetch pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-052 OpenAI async model-fetch pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
