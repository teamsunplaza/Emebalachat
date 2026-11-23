// ---------------------------------------------------------------------------
// P2-D (261122_0002 v0.10.10 phased fixes, phase 2 wave A): worker/input
// pipeline unit - behavioral suites + structural source pins.
//   * RISK-03: the typing-mode OpenAI failure surface. The Enter path now reads
//     the PER-REQUEST cause (TranslationTaskContext::out_error_detail, the
//     L5-B carrier) with the process-global LastChatErrorDetail() slot as the
//     fallback, and dispatches it to the registered SetOnTypingError callback
//     (main.cpp marshals to the GUI thread). Behavioral half: the pure
//     conversion/read path cannot run headlessly without a network engine, so
//     the carrier WIRING is pinned structurally (ctx slot set before the call,
//     detail read -> callback -> tone order preserved).
//   * L4-C: the explicit "translate my selection and replace it in place"
//     flow. Behavioral half: the two pure plan helpers (PlanSelectionCapture,
//     PlanSelectionReplaceOutcome) and the config/hotkey round-trip of
//     hotkey_selection_translate (default "Ctrl+Alt+T" since V10F-R1 F-05,
//     pure resolver, no collision with the three REQ-022 bound rows).
//     Structural half: the selection-primitive-free capture seam, the
//     kind-gated ExecuteTask branch, and the hook's off-thread dispatch
//     (V10F-R1 F-05: gated on IsActive() too).
//   * FUP-04 (Option C): the out-of-band raw rescue payload. Behavioral half:
//     RescueRawPayloadNeedsConsume truth table. Structural half: both over-guard
//     rescue verdicts fill CopySelectedText's out_raw_rescue_text gated on the
//     rescue provenance, and the worker's consume-then-send-through arm sits
//     before the R5 empty-capture hold.
// Staged as an .inc next to this runner; registered from main() per the
// end-of-file pattern (the p1_u*.inc precedent).
// ---------------------------------------------------------------------------

namespace {

// Resolve + read a production source for the P2-D structural pins (same
// candidate + CRLF-normalization precedent as the P1-U1/P1-U4 readers: the
// needles below are ASCII-only, so CRLF normalization keeps them stable).
void P2DReadSrc(const wchar_t* rel, std::string& out) {
    out.clear();
    const std::filesystem::path p = ResolveRepoFile(rel);
    if (p.empty()) return;
    std::ifstream in(p, std::ios::binary);
    if (!in) return;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string norm;
    norm.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\r' && i + 1 < out.size() && out[i + 1] == '\n') continue;
        norm.push_back(out[i]);
    }
    out.swap(norm);
}

size_t P2DCountOcc(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t pos = hay.find(needle); pos != std::string::npos;
         pos = hay.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

} // namespace

// ---- (a) hotkey_selection_translate: config round-trip + pure resolver ------

void TestP2DSelectionHotkeyConfig() {
    std::cout << "[RUN] Testing P2-D hotkey_selection_translate config/resolver..." << std::endl;
    const int failures_before = g_failed_count;

    // Default + JSON round-trip through the public string seams (the
    // TestConfigModule precedent - no disk I/O in the suite).
    // V10F-R1 F-05: the default is Ctrl+Alt+T (was Ctrl+Shift+T); stored user
    // configs keep their value, so only the compiled-in default and the
    // absent-key path change.
    {
        AppConfig cfg;
        TEST_CHECK(cfg.hotkey_selection_translate == "Ctrl+Alt+T",
                   "P2-D: Default hotkey_selection_translate is Ctrl+Alt+T (V10F-R1 F-05)");
        const std::string json = cfg.ToJsonString();
        TEST_CHECK(json.find("\"hotkey_selection_translate\"") != std::string::npos,
                   "P2-D: SaveToFile's JSON carries the hotkey_selection_translate key");

        AppConfig custom;
        custom.hotkey_selection_translate = "Ctrl+Alt+L";
        const std::string custom_json = custom.ToJsonString();
        AppConfig reloaded;
        TEST_CHECK(reloaded.FromJsonString(custom_json),
                   "P2-D: the emitted JSON parses back");
        TEST_CHECK(reloaded.hotkey_selection_translate == "Ctrl+Alt+L",
                   "P2-D: a custom selection-translate combo round-trips verbatim");

        // Absence keeps the compile-time default (a pre-V10F config.json).
        AppConfig legacy;
        TEST_CHECK(legacy.FromJsonString("{\"hotkey_toggle\": \"F9\"}") &&
                       legacy.hotkey_selection_translate == "Ctrl+Alt+T",
                   "P2-D: a config.json without the key keeps the compile-time default (Ctrl+Alt+T)");
        // A stored Ctrl+Shift+T value is NOT migrated (F-05: the stored user
        // config keeps its value verbatim).
        AppConfig stored;
        TEST_CHECK(stored.FromJsonString("{\"hotkey_selection_translate\": \"Ctrl+Shift+T\"}") &&
                       stored.hotkey_selection_translate == "Ctrl+Shift+T",
                   "P2-D F-05: a stored Ctrl+Shift+T keeps its value (no migration)");
    }

    // Pure resolver matrix (hook.hpp, ONE definition shared with Start()).
    {
        using HK = KeyboardHook;
        static_assert(HK::kDefaultSelectionHotkey.vk == 'T' &&
                          HK::kDefaultSelectionHotkey.ctrl && !HK::kDefaultSelectionHotkey.shift &&
                          HK::kDefaultSelectionHotkey.alt && !HK::kDefaultSelectionHotkey.win &&
                          HK::kDefaultSelectionHotkey.valid,
                      "P2-D: the compiled-in selection-translate default must be Ctrl+Alt+T (V10F-R1 F-05)");
        TEST_CHECK(HK::ResolveSelectionFromConfig("Ctrl+Alt+T") == HK::kDefaultSelectionHotkey,
                   "P2-D: 'Ctrl+Alt+T' resolves to the default spec");
        TEST_CHECK(HK::ResolveSelectionFromConfig("") == HK::kDefaultSelectionHotkey,
                   "P2-D: an empty config value falls back to the default spec");
        TEST_CHECK(HK::ResolveSelectionFromConfig("nonsense") == HK::kDefaultSelectionHotkey,
                   "P2-D: an unparsable config value falls back to the default spec");
        const HK::HotkeySpec alt_combo = HK::ResolveSelectionFromConfig("Ctrl+Alt+L");
        TEST_CHECK(alt_combo == HK::ParseHotkey("Ctrl+Alt+L") && alt_combo.valid &&
                       alt_combo.vk == 'L' && alt_combo.ctrl && alt_combo.alt,
                   "P2-D: a valid custom combo is honored as parsed");
        // The compiled spec round-trips through the display form (the cheat
        // sheet / cursor bubble vocabulary).
        TEST_CHECK(HK::HotkeySpecToDisplayString(HK::kDefaultSelectionHotkey) == L"Ctrl+Alt+T",
                   "P2-D: the default combo's display string round-trips to 'Ctrl+Alt+T'");
        TEST_CHECK(HK::ParseHotkey("Ctrl+Alt+T") == HK::kDefaultSelectionHotkey,
                   "P2-D: the display vocabulary parses back to the compiled default");
        // Priority contract: the three REQ-022 bound rows win a collision; with
        // none of them matching, the selection row is a separate later match
        // (its match block runs AFTER ResolveBoundHotkeyAction in the proc).
        const HK::HotkeySpec* const bound[] = {&HK::kDefaultToggleHotkey,
                                               &HK::kDefaultLangHotkey,
                                               &HK::kDefaultModeHotkey};
        TEST_CHECK(HK::ResolveBoundHotkeyAction(bound, 3, 'T',
                                                /*ctrl*/true, /*shift*/true, /*alt*/false,
                                                /*win*/false) == HK::HotkeyAction::None &&
                       HK::ResolveBoundHotkeyAction(bound, 3, 'T',
                                                    /*ctrl*/true, /*shift*/false, /*alt*/true,
                                                    /*win*/false) == HK::HotkeyAction::None,
                   "P2-D: Ctrl+Shift+T and Ctrl+Alt+T each match none of the three bound rows (no shadowing)");
        TEST_CHECK(HK::HotkeyMatches(HK::kDefaultSelectionHotkey, 'T', true, false, true, false) &&
                       !HK::HotkeyMatches(HK::kDefaultSelectionHotkey, 'T', true, true, false, false),
                   "P2-D: the selection combo's exact-modifier match (extra Alt required)");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-D selection hotkey config/resolver passed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-D selection hotkey config/resolver: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (b) the pure plan helpers (L4-C + FUP-04 decision tables) --------------

void TestP2DSelectionPlanPredicates() {
    std::cout << "[RUN] Testing P2-D selection plan predicates..." << std::endl;
    const int failures_before = g_failed_count;

    // PlanSelectionCapture: exactly ONE action per verdict, and the verdict ->
    // action mapping the flow relies on.
    {
        const SelectionCaptureVerdict ok = PlanSelectionCapture(EnterCaptureResult::Ok);
        TEST_CHECK(ok.translate && !ok.notify_no_selection && !ok.notify_copy_failed,
                   "P2-D: an Ok selection capture translates");
        const SelectionCaptureVerdict empty = PlanSelectionCapture(EnterCaptureResult::EmptySelection);
        TEST_CHECK(!empty.translate && empty.notify_no_selection && !empty.notify_copy_failed,
                   "P2-D: an empty selection surfaces the existing no-selection notice");
        const SelectionCaptureVerdict dropped = PlanSelectionCapture(EnterCaptureResult::CopyChordFailed);
        TEST_CHECK(!dropped.translate && !dropped.notify_no_selection && dropped.notify_copy_failed,
                   "P2-D: a dropped copy chord surfaces the existing copy-failed notice");
        const SelectionCaptureVerdict excluded = PlanSelectionCapture(EnterCaptureResult::EditorExcluded);
        TEST_CHECK(!excluded.translate && excluded.notify_copy_failed,
                   "P2-D: an editor/terminal exclusion refuses with the copy-failed notice");
        const SelectionCaptureVerdict oversize = PlanSelectionCapture(EnterCaptureResult::GuardAbort);
        TEST_CHECK(!oversize.translate && oversize.notify_copy_failed,
                   "P2-D: an over-guard (document-sized) selection is refused before the engine");
        // Totality + single-action invariant across every enum value.
        bool one_action_only = true;
        bool at_least_one = true;
        for (int i = 0; i <= static_cast<int>(EnterCaptureResult::CopyChordDropped); ++i) {
            const SelectionCaptureVerdict v =
                PlanSelectionCapture(static_cast<EnterCaptureResult>(i));
            const int actions = (v.translate ? 1 : 0) + (v.notify_no_selection ? 1 : 0) +
                                (v.notify_copy_failed ? 1 : 0);
            if (actions > 1) one_action_only = false;
            if (actions == 0) at_least_one = false;
        }
        TEST_CHECK(one_action_only,
                   "P2-D: no verdict can both translate and notify (disjoint arms)");
        TEST_CHECK(at_least_one,
                   "P2-D: every verdict has an action (the seam is total - no silent no-op)");
        static_assert(PlanSelectionCapture(EnterCaptureResult::Ok).translate,
                      "P2-D: the usable arm is a compile-time constant");
    }

    // PlanSelectionReplaceOutcome: paste / tone matrix.
    {
        const SelectionReplaceOutcome canceled = PlanSelectionReplaceOutcome(true, true, false);
        TEST_CHECK(!canceled.paste && !canceled.play_error_tone,
                   "P2-D: a user cancel pastes nothing and plays no failure tone");
        const SelectionReplaceOutcome failed = PlanSelectionReplaceOutcome(false, true, false);
        TEST_CHECK(!failed.paste && failed.play_error_tone,
                   "P2-D: an empty translation pastes nothing and plays the failure tone");
        const SelectionReplaceOutcome identity = PlanSelectionReplaceOutcome(false, false, true);
        TEST_CHECK(!identity.paste && !identity.play_error_tone,
                   "P2-D: an identity translation pastes nothing (no no-op replacement)");
        const SelectionReplaceOutcome normal = PlanSelectionReplaceOutcome(false, false, false);
        TEST_CHECK(normal.paste && !normal.play_error_tone,
                   "P2-D: a real translation replaces the selection in place");
        // A cancel always wins over an empty translation (no tone on cancel).
        TEST_CHECK(!PlanSelectionReplaceOutcome(true, false, false).paste,
                   "P2-D: a cancel suppresses the paste even with a non-empty result");
    }

    // RescueRawPayloadNeedsConsume (FUP-04): the 8-row truth table.
    {
        TEST_CHECK(RescueRawPayloadNeedsConsume(true, true, true),
                   "P2-D FUP-04: rescued + empty return + reported payload -> consume");
        TEST_CHECK(!RescueRawPayloadNeedsConsume(true, true, false),
                   "P2-D FUP-04: no reported payload -> the legacy empty-capture arms stay");
        TEST_CHECK(!RescueRawPayloadNeedsConsume(true, false, true),
                   "P2-D FUP-04: a NON-empty capture keeps every existing arm byte-identical");
        TEST_CHECK(!RescueRawPayloadNeedsConsume(false, true, true),
                   "P2-D FUP-04: a non-rescued capture never consumes (no synthetic span)");
        TEST_CHECK(!RescueRawPayloadNeedsConsume(false, false, false),
                   "P2-D FUP-04: the all-false shape is a no-op");
        // BUG-005 interlock: the new gate is NOT a widening of the old one.
        static_assert(!RescueLiveSelectionNeedsConsume(true, false),
                      "P2-D: the pre-existing BUG-005 gate still refuses an EMPTY returned capture");
        static_assert(RescueRawPayloadNeedsConsume(true, true, true),
                      "P2-D: the FUP-04 gate owns exactly that out-of-band shape");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-D selection plan predicates passed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-D selection plan predicates: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (c) structural pins: worker.hpp / worker.cpp ---------------------------

void TestP2DWorkerStructurePins() {
    std::cout << "[RUN] Testing P2-D worker.cpp/worker.hpp structural pins..." << std::endl;
    const int failures_before = g_failed_count;

    std::string worker_cpp, worker_hpp;
    P2DReadSrc(L"src\\worker.cpp", worker_cpp);
    P2DReadSrc(L"src\\worker.hpp", worker_hpp);
    TEST_CHECK(!worker_cpp.empty(), "P2-D: src/worker.cpp resolves for the pins");
    TEST_CHECK(!worker_hpp.empty(), "P2-D: src/worker.hpp resolves for the pins");

    // ---- worker.hpp: the new seams exist with their contracted shapes -------
    TEST_CHECK(worker_hpp.find("    bool selection_replace = false;") != std::string::npos,
               "P2-D: PipelineTask carries the selection_replace kind flag (default false)");
    TEST_CHECK(worker_hpp.find("    bool PostSelectionReplaceTask(HWND target_hwnd);") != std::string::npos,
               "P2-D: PostSelectionReplaceTask is declared");
    TEST_CHECK(worker_hpp.find("    void SetOnTypingError(std::function<void(const std::wstring&)> cb) {") !=
                   std::string::npos,
               "P2-D: SetOnTypingError takes the UTF-16 detail (RISK-03)");
    TEST_CHECK(worker_hpp.find("constexpr bool RescueRawPayloadNeedsConsume(bool select_all_rescued,") !=
                   std::string::npos &&
                   worker_hpp.find("struct SelectionCaptureVerdict {") != std::string::npos &&
                   worker_hpp.find("constexpr SelectionCaptureVerdict PlanSelectionCapture(EnterCaptureResult capture) {") !=
                       std::string::npos &&
                   worker_hpp.find("struct SelectionReplaceOutcome {") != std::string::npos &&
                   worker_hpp.find("constexpr SelectionReplaceOutcome PlanSelectionReplaceOutcome(bool canceled,") !=
                       std::string::npos,
               "P2-D: all three pure plan helpers have ONE header definition");
    TEST_CHECK(worker_hpp.find("    std::function<void(const std::wstring&)> typing_error_cb_;") !=
                   std::string::npos,
               "P2-D: the typing-error callback member is declared");

    // ---- RISK-03 wiring in worker.cpp --------------------------------------
    {
        // The per-request carrier is armed BEFORE the engine call, so the
        // manager/client writes into this task's own slot.
        const size_t carrier = worker_cpp.find("ctx.out_error_detail = &request_error_detail;");
        const size_t enter_call = worker_cpp.find(
            "engine_.Translate(engine_input, resolved_engine_src, snap.type_target_language, &status, ctx)");
        TEST_CHECK(carrier != std::string::npos && enter_call != std::string::npos &&
                       carrier < enter_call,
                   "P2-D RISK-03: the per-request detail carrier is set before the Enter Translate call");
        // The OpenAI failure arm reads the carrier first and keeps the global
        // slot only as the fallback.
        const size_t detail_read = worker_cpp.find("std::wstring detail = RequestErrorDetailAsWide(ctx);");
        const size_t fallback = worker_cpp.find("OpenAiCompatibleClient::LastChatErrorDetail()", detail_read);
        TEST_CHECK(detail_read != std::string::npos && fallback != std::string::npos &&
                       detail_read < fallback,
                   "P2-D RISK-03: the per-request detail is read before the global-slot fallback");
        // Callback dispatch happens AFTER the shape-only DIAG and BEFORE the
        // failure tone - the failure-tone arm's own ordering pins are untouched.
        const size_t diag = worker_cpp.find("WORKER/ExecuteTask/055: openai leg failed", detail_read);
        const size_t dispatch = worker_cpp.find("typing_error_cb_(detail);", detail_read);
        const size_t tone = worker_cpp.find("PlaySoundAsync(SoundType::Disable);", detail_read);
        TEST_CHECK(diag != std::string::npos && dispatch != std::string::npos &&
                       tone != std::string::npos && detail_read < diag && diag < dispatch &&
                       dispatch < tone,
                   "P2-D RISK-03: detail read -> /055 DIAG -> SetOnTypingError dispatch -> tone");
        TEST_CHECK(worker_cpp.find("if (!detail.empty() && typing_error_cb_) {") != std::string::npos,
                   "P2-D RISK-03: an empty detail keeps the old tone-only surface (no blank notice)");
    }

    // ---- L4-C: the kind-gated branch and its placement ----------------------
    {
        const size_t kind_branch = worker_cpp.find("if (task.selection_replace) {");
        const size_t handoff =
            worker_cpp.find("ExecuteSelectionReplaceTask(task, backup, restorer.active);", kind_branch);
        const size_t ime_backstop = worker_cpp.find("if (ForegroundImeComposing()) {");
        TEST_CHECK(kind_branch != std::string::npos && handoff != std::string::npos &&
                       handoff - kind_branch < 400,
                   "P2-D L4-C: ExecuteTask hands the kind to ExecuteSelectionReplaceTask");
        TEST_CHECK(kind_branch != std::string::npos && ime_backstop != std::string::npos &&
                       kind_branch < ime_backstop,
                   "P2-D L4-C: the selection branch precedes the Enter-specific IME send-through backstop");
        // The flow body: capture seam + paste primitive + both plan helpers.
        const size_t body_start = worker_cpp.find(
            "void PipelineWorker::ExecuteSelectionReplaceTask(const PipelineTask& task,");
        const size_t body_end = worker_cpp.find("bool PipelineWorker::PostSelectionReplaceTask(HWND target_hwnd) {");
        TEST_CHECK(body_start != std::string::npos && body_end != std::string::npos && body_end > body_start,
                   "P2-D L4-C: ExecuteSelectionReplaceTask body is locatable");
        if (body_start != std::string::npos && body_end != std::string::npos && body_end > body_start) {
            const std::string body = worker_cpp.substr(body_start, body_end - body_start);
            TEST_CHECK(body.find("CopyUserSelectionText(task.target_hwnd, &capture_result)") !=
                           std::string::npos,
                       "P2-D L4-C: the flow captures through the selection-only seam");
            TEST_CHECK(body.find("PasteAndRestore(translated, backup, task.target_hwnd, &restore_ok)") !=
                           std::string::npos,
                       "P2-D L4-C: the translation replaces the selection via PasteAndRestore");
            TEST_CHECK(body.find("PlanSelectionCapture(capture_result)") != std::string::npos &&
                           body.find("PlanSelectionReplaceOutcome(") != std::string::npos,
                       "P2-D L4-C: both decision tables are consumed (ONE definition)");
            TEST_CHECK(body.find("task.shift_enter_count") == std::string::npos,
                       "P2-D L4-C: K is not consulted anywhere in the selection flow");
            TEST_CHECK(body.find("last_paste_canonical_ = CanonicalFormForLedger(translated);") !=
                           std::string::npos,
                       "P2-D L4-C: the ledger is refreshed through the S2 canonical pair");
            // Never injects Enter: no send-through helper, no raw SendEnterKey.
            TEST_CHECK(body.find("SendThroughWithNewlineTracking") == std::string::npos &&
                           body.find("SendEnterKey") == std::string::npos,
                       "P2-D L4-C: the selection flow injects NO Enter (hotkey, not an intercepted Enter)");
            // The clipboard restorer discipline is the shared F7 algebra.
            TEST_CHECK(body.find("ClipboardRestorerStaysArmed(replaced, restore_ok)") != std::string::npos,
                       "P2-D L4-C: the scope-exit restorer is disarmed only on a confirmed restore");
        }
        // PostSelectionReplaceTask: same publication ordering as PostTask.
        const size_t post_start = body_end;
        const size_t post_end = worker_cpp.find(
            "} // namespace emebalachat", post_start);
        TEST_CHECK(post_start != std::string::npos && post_end != std::string::npos && post_end > post_start,
                   "P2-D L4-C: PostSelectionReplaceTask body is locatable");
        if (post_start != std::string::npos && post_end != std::string::npos && post_end > post_start) {
            const std::string body = worker_cpp.substr(post_start, post_end - post_start);
            const size_t busy = body.find("is_busy_.exchange(true, std::memory_order_acq_rel)");
            const size_t hwnd = body.find("busy_target_hwnd_.store(target_hwnd, std::memory_order_release)");
            const size_t push = body.find("queue_.push(");
            TEST_CHECK(busy != std::string::npos && hwnd != std::string::npos &&
                           push != std::string::npos && busy < hwnd && hwnd < push,
                       "P2-D L4-C: busy -> hwnd -> queue push ordering preserved for the new task kind");
            TEST_CHECK(body.find("selection_task.selection_replace = true;") != std::string::npos,
                       "P2-D L4-C: the posted task carries the selection kind");
        }
    }

    // ---- FUP-04: the rescue-raw consume arm --------------------------------
    {
        const size_t gate = worker_cpp.find(
            "if (RescueRawPayloadNeedsConsume(provenance.rescued, line.empty(),");
        const size_t consume = worker_cpp.find(
            "ConsumeRescueSelectionIdentityPaste(raw_rescue_text, task.target_hwnd", gate);
        const size_t send = worker_cpp.find("SendThroughWithNewlineTracking(task.target_hwnd", gate);
        const size_t hold = worker_cpp.find("const bool empty_capture_hold = EmptyCaptureNeedsHold(", gate);
        TEST_CHECK(gate != std::string::npos && consume != std::string::npos &&
                       send != std::string::npos && hold != std::string::npos,
                   "P2-D FUP-04: the rescue-raw arm is locatable");
        TEST_CHECK(gate != std::string::npos && hold != std::string::npos && gate < hold,
                   "P2-D FUP-04: the arm runs BEFORE the R5 empty-capture hold (no false no-selection notice)");
        TEST_CHECK(consume != std::string::npos && send != std::string::npos && consume < send,
                   "P2-D FUP-04: the identity consume precedes the send-through Enter (no Enter over a live selection)");
        TEST_CHECK(worker_cpp.find("std::wstring raw_rescue_text;") != std::string::npos &&
                       worker_cpp.find("&provenance, &raw_rescue_text)") != std::string::npos,
                   "P2-D FUP-04: ExecuteTask passes the raw-rescue carrier into the capture seam");
        // Inventory tripwire: FUP-04 legitimately adds the NINTH send-through
        // terminal (and the eighth two-arg form). The two pre-V10F count pins
        // (run_tests.cpp TestWorkerB1SendThroughPins and the BUG-005 section,
        // plus ws2_capture_tests.inc) still assert 8/7 and MUST be updated with
        // this unit's integration.
        TEST_CHECK(P2DCountOcc(worker_cpp, "SendThroughWithNewlineTracking(task.target_hwnd") == 9,
                   "P2-D FUP-04: nine send-through terminals now exist (8 pre-V10F + the rescue-raw arm)");
        TEST_CHECK(P2DCountOcc(worker_cpp, "SendThroughWithNewlineTracking(task.target_hwnd, task.is_shift_enter)") == 8,
                   "P2-D FUP-04: eight plain two-arg sites (7 pre-V10F + the rescue-raw arm)");
        // The pre-existing inventories this unit must NOT disturb.
        TEST_CHECK(P2DCountOcc(worker_cpp, "ReleaseSelectionOnce();") == 5,
                   "P2-D: the ReleaseSelectionOnce inventory is untouched (5)");
        TEST_CHECK(P2DCountOcc(worker_cpp, "if (RescueLiveSelectionNeedsConsume(") == 5,
                   "P2-D: the BUG-005 consume-gate inventory is untouched (5)");
        TEST_CHECK(P2DCountOcc(worker_cpp, "last_paste_text_.clear()") == 2,
                   "P2-D: the two ledger clears are untouched");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-D worker structural pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-D worker structural pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (d) structural pins: win32_input / hook -------------------------------

void TestP2DInputAndHookPins() {
    std::cout << "[RUN] Testing P2-D win32_input/hook structural pins..." << std::endl;
    const int failures_before = g_failed_count;

    std::string wi_cpp, wi_hpp, hook_cpp, hook_hpp;
    P2DReadSrc(L"src\\win32_input.cpp", wi_cpp);
    P2DReadSrc(L"src\\win32_input.hpp", wi_hpp);
    P2DReadSrc(L"src\\hook.cpp", hook_cpp);
    P2DReadSrc(L"src\\hook.hpp", hook_hpp);
    TEST_CHECK(!wi_cpp.empty() && !wi_hpp.empty(), "P2-D: win32_input sources resolve");
    TEST_CHECK(!hook_cpp.empty() && !hook_hpp.empty(), "P2-D: hook sources resolve");

    // ---- the selection-only capture seam -----------------------------------
    TEST_CHECK(wi_hpp.find("std::wstring* out_raw_rescue_text = nullptr);") != std::string::npos,
               "P2-D FUP-04: CopySelectedText gained the optional raw-rescue out-param (defaulted)");
    TEST_CHECK(wi_hpp.find("std::wstring CopyUserSelectionText(HWND hwnd,") != std::string::npos,
               "P2-D L4-C: CopyUserSelectionText is declared");
    const size_t seam_start = wi_cpp.find(
        "std::wstring CopyUserSelectionText(HWND hwnd, EnterCaptureResult* capture_result) {");
    const size_t seam_end = wi_cpp.find("bool IsSameWindowForInjection(HWND expected_target", seam_start);
    TEST_CHECK(seam_start != std::string::npos && seam_end != std::string::npos && seam_end > seam_start,
               "P2-D L4-C: the selection capture seam body is locatable");
    if (seam_start != std::string::npos && seam_end != std::string::npos && seam_end > seam_start) {
        const std::string body = wi_cpp.substr(seam_start, seam_end - seam_start);
        TEST_CHECK(body.find("CopyChordWithSettledRetry(kDragCopyChordAttempts, kDragCopyRetryBackoffMs)") !=
                       std::string::npos,
                   "P2-D L4-C: the plain Ctrl+C rides the shared per-attempt re-baselining retry driver");
        TEST_CHECK(body.find("IsEnterTranslateExcludedApp(hwnd)") != std::string::npos,
                   "P2-D L4-C: the editor/terminal exclusion precedes any chord (FUP-01 interlock)");
        TEST_CHECK(body.find("EnterCaptureWithinGuard(text.size(), nl)") != std::string::npos,
                   "P2-D L4-C: the over-guard (document-sized) refusal is the V3 abuse-defense parity");
        // The contract: NO selection primitive, NO SelectAll rescue.
        TEST_CHECK(body.find("SelectAll()") == std::string::npos &&
                       body.find("SelectMessageBlock()") == std::string::npos &&
                       body.find("EditCaretTracker_TrySelectNewText") == std::string::npos &&
                       body.find("SelectAllRescueWarranted") == std::string::npos,
                   "P2-D L4-C: the seam performs no selection primitive and no SelectAll rescue");
        TEST_CHECK(body.find("t_paste_capture_focus.valid = true;") != std::string::npos,
                   "P2-D L4-C: the capture-time focus record is written for PasteAndRestore's gate");
    }
    // FUP-04: BOTH over-guard rescue arms report the payload, gated on rescue.
    TEST_CHECK(P2DCountOcc(wi_cpp, "if (provenance_local.rescued && out_raw_rescue_text) {") == 2,
               "P2-D FUP-04: both EmptyTail and GuardAbort report the raw rescued payload");
    TEST_CHECK(P2DCountOcc(wi_cpp, "*out_raw_rescue_text = text;") == 2,
               "P2-D FUP-04: the raw capture (not a normalized copy) is what is handed out");
    TEST_CHECK(wi_cpp.find("if (out_raw_rescue_text) {\n        out_raw_rescue_text->clear();\n    }") !=
                   std::string::npos,
               "P2-D FUP-04: the carrier is cleared at entry (strict opt-in contract)");
    // Zero feature loss: the Enter seam's exclusion message and the abort-order
    // invariant stay exactly where they were.
    TEST_CHECK(wi_cpp.find("CopySelectedText/005: editor/IDE/terminal app excluded") != std::string::npos,
               "P2-D: the /005 exclusion message is untouched");

    // ---- the hook's hotkey plumbing ---------------------------------------
    TEST_CHECK(hook_hpp.find("    void SetSelectionReplaceCallback(std::function<void(HWND)> cb) {") !=
                   std::string::npos,
               "P2-D L4-C: KeyboardHook exposes the selection-replace setter (HWND-carrying)");
    TEST_CHECK(hook_hpp.find("    static constexpr HotkeySpec kDefaultSelectionHotkey{") != std::string::npos &&
                   hook_hpp.find("    static HotkeySpec ResolveSelectionFromConfig(std::string_view raw) {") !=
                       std::string::npos,
               "P2-D L4-C: the compiled default + pure resolver live in the header (ONE definition)");
    TEST_CHECK(hook_hpp.find("    HotkeySpec selection_spec_{};") != std::string::npos &&
                   hook_hpp.find("    std::function<void(HWND)> selection_replace_cb_;") != std::string::npos,
               "P2-D L4-C: the compiled spec + callback members are declared");
    TEST_CHECK(hook_cpp.find("selection_spec_ = ResolveSelectionFromConfig(config_.hotkey_selection_translate);") !=
                   std::string::npos,
               "P2-D L4-C: Start() compiles the spec from config.hotkey_selection_translate");
    {
        const size_t match = hook_cpp.find(
            "HotkeyMatches(s_instance->selection_spec_, kbd->vkCode, ctrl, shift, alt, win)");
        const size_t bound_table = hook_cpp.find("ResolveBoundHotkeyAction(");
        const size_t alt_win_funnel = hook_cpp.find("if (alt || win) {");
        TEST_CHECK(match != std::string::npos && bound_table != std::string::npos &&
                       alt_win_funnel != std::string::npos && bound_table < match &&
                       match < alt_win_funnel,
                   "P2-D L4-C: the match block sits AFTER the REQ-022 bound rows and BEFORE the Alt/Win funnel");
        // V10F-R1 F-05: gated on IsActive() FIRST - an inactive hook swallows
        // nothing, so the combo keeps its normal app meaning while translation
        // is off.
        TEST_CHECK(hook_cpp.find("if (s_instance->IsActive() && s_instance->selection_replace_cb_ &&") !=
                       std::string::npos,
                   "P2-D L4-C + F-05: the combo is swallowed only while ACTIVE and a callback is registered");
        TEST_CHECK(hook_cpp.find("self->DispatchHotkeyFx([self, selection_target] {") != std::string::npos &&
                       hook_cpp.find("self->selection_replace_cb_(selection_target);") != std::string::npos,
                   "P2-D L4-C: the user callback runs off the hook thread via DispatchHotkeyFx");
        TEST_CHECK(hook_cpp.find("const HWND selection_target = ::GetForegroundWindow();") != std::string::npos,
                   "P2-D L4-C: the target foreground is captured on the hook thread at dispatch time");
    }
    // P1-U1 zero feature loss: the epoch-guarded K reset inventory is untouched.
    TEST_CHECK(P2DCountOcc(hook_cpp, "ResetShiftEnterKEpoched(s_instance->shift_enter_k_") == 8,
               "P2-D: the 8 FUP-07 reset sites are untouched");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-D win32_input/hook structural pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-D win32_input/hook structural pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// Single registration entry (the run_tests.cpp end-of-file pattern): the
// integrator adds `#include "p2_d_tests.inc"` to the include cluster and
// `TestP2DV10FWorkerPipeline();` inside main().
void TestP2DV10FWorkerPipeline() {
    TestP2DSelectionHotkeyConfig();
    TestP2DSelectionPlanPredicates();
    TestP2DWorkerStructurePins();
    TestP2DInputAndHookPins();
}
