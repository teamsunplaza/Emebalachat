// ---------------------------------------------------------------------------
// v0.10.7 (session 261111_0001) E1/E2/E9 + ISSUE-013: the Shift+Enter line-
// break accounting fix, the raised long-form capture ceilings, and the
// system-metric drag threshold. Staged as an .inc next to this runner.
//
// Register near the end of main() (the end-of-file pattern):
//     int v107_failures = 0;
//     RunV107LineBreakTests(v107_failures);
//     ... add v107_failures to the suite failure total ...
// The entry also feeds the shared TEST_CHECK counters (g_failed_count /
// g_test_count), so a plain `RunV107LineBreakTests(some_int)` wiring keeps
// both books consistent; the return value is the number of failed checks in
// THIS suite.
//
// Headers: relies on run_tests.cpp's existing includes (hook.hpp,
// mouse_hook.hpp, win32_input.hpp, worker.hpp chain - same discipline as
// req052_ui_fixes_tests.inc).
//
// Coverage:
//   (a) E1 - Shift+Enter ALWAYS advances K, including mid-IME-composition
//       (the old ShiftEnterKNext freeze starved Korean multi-line messages:
//       jamo/space/period all arrive as VK_PROCESSKEY, so the mirror stayed
//       true for the whole sentence and K never left 0).
//   (b) E2 - Backspace/Delete on a NON-EMPTY line never touches K; on an
//       EMPTY line it decrements K (merging with the previous line). Typo
//       correction on line 3 of a 3-line message leaves K=2.
//   (c) The LineBreakTracker contract: per-line counter saturation, K clamp
//       at the guard ceiling, Reset, and the IsTextProducingKey matrix.
//   (d) E9 - the capture ceilings are 16384 chars / 256 newlines everywhere
//       they are enforced (EnterCaptureWithinGuard, the K clamp), a 1-2 page
//       Shift+Enter message now passes, and the F3 block-slice
//       (FindCurrentBlockStart / SliceWholeCaptureToBlock) is what keeps
//       multi-block captures safe under the raise.
//   (e) ISSUE-013 - the drag threshold derives from SM_CXDRAG/SM_CYDRAG for
//       the press monitor's DPI (pure combine + source pins on the Win32
//       seam); the hard-coded 15 px is gone.
// ---------------------------------------------------------------------------

// Repo-file reader for the source pins below. std::string candidates (NOT
// the const char* temporary pattern): every pointer must outlive the loop.
static std::string V107ReadRepoFile(const char* name) {
    const std::string candidates[] = {std::string(name), std::string("../") + name,
                                      std::string("../../") + name};
    for (const std::string& cand : candidates) {
        std::ifstream in(cand, std::ios::binary);
        if (in) {
            return std::string((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        }
    }
    return {};
}

int RunV107LineBreakTests(int& failures) {
    std::cout << "[TEST] v0.10.7 E1/E2/E9 + ISSUE-013 (line-break tracker, capture ceilings, system drag threshold)" << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) E1: Shift+Enter advances K even while composing -------------
    // The freeze (ShiftEnterKNext's old ime_composing early-return) is gone;
    // the parameter is retained for source compatibility and ignored.
    static_assert(ShiftEnterKNext(0, /*ime_composing*/ true) == 1,
                  "E1: Shift+Enter while composing -> K advances (freeze removed)");
    static_assert(ShiftEnterKNext(2, true) == 3,
                  "E1: a second composing Shift+Enter accumulates");
    static_assert(ShiftEnterKNext(0, false) == 1,
                  "E1: Shift+Enter while not composing -> +1 (unchanged)");
    static_assert(ShiftEnterKNext(3, false) == 4,
                  "E1: counting accumulates");
    TEST_CHECK(ShiftEnterKNext(0, true) == 1,
               "E1: composing Shift+Enter no longer frozen (runtime twin)");
    {
        // Korean sentence simulation: jamo/space/period keys arrive as
        // VK_PROCESSKEY (the hook's composition signal), Shift+Enter still
        // passes a real newline to the app and must be counted.
        LineBreakTracker t;
        for (int i = 0; i < 12; ++i) {
            t.OnTextInput(); // "안녕 하세요." jamo + space + period
        }
        TEST_CHECK(t.line_input == 12 && t.k == 0,
                   "E1 fixture: composition keys accumulate on the current line");
        t.OnShiftEnter();
        TEST_CHECK(t.k == 1 && t.line_input == 0,
                   "E1: Shift+Enter during composition increments K and resets the line");
        for (int i = 0; i < 5; ++i) {
            t.OnTextInput();
        }
        t.OnShiftEnter();
        TEST_CHECK(t.k == 2,
                   "E1: a second mid-composition Shift+Enter accumulates (K=2)");
        // The composing-Enter promotion consumes the block with the K the
        // composition actually accumulated (no starvation).
        TEST_CHECK(ShiftEnterKNext(t.k, true) == 3,
                   "E1: the K carried into the composing-Enter promotion is the real count");
    }

    // ---- (b) E2: Backspace/Delete line accounting ------------------------
    {
        // Success criterion: typo correction on line 3 of a 3-line message
        // leaves K=2.
        LineBreakTracker t;
        for (int i = 0; i < 8; ++i) t.OnTextInput();  // line 1 typed
        t.OnShiftEnter();                             // K=1
        for (int i = 0; i < 8; ++i) t.OnTextInput();  // line 2 typed
        t.OnShiftEnter();                             // K=2
        for (int i = 0; i < 9; ++i) t.OnTextInput();  // line 3 typed (9 keys)
        TEST_CHECK(t.k == 2 && t.line_input == 9, "E2 fixture: 3-line message, K=2");
        t.OnBackspace(); // typo fix inside line 3
        TEST_CHECK(t.k == 2 && t.line_input == 8,
                   "E2: Backspace on a NON-EMPTY line does NOT decrement K");
        t.OnTextInput(); // corrected character retyped
        TEST_CHECK(t.k == 2 && t.line_input == 9,
                   "E2: typo correction on line 3 of a 3-line message leaves K=2");
        // Delete behaves like Backspace (same tracker method).
        LineBreakTracker d;
        d.OnShiftEnter();
        d.OnTextInput();
        d.OnTextInput();
        d.OnBackspace(); // stands in for VK_DELETE (the hook feeds both here)
        TEST_CHECK(d.k == 1 && d.line_input == 1,
                   "E2: a delete-style key on a non-empty line never touches K");
    }
    {
        // Backspace on an EMPTY line merges it with the previous one: K
        // decrements, and never below zero.
        LineBreakTracker t;
        t.OnShiftEnter();
        t.OnShiftEnter();
        TEST_CHECK(t.k == 2 && t.line_input == 0, "E2 fixture: two separators, empty line");
        t.OnBackspace();
        TEST_CHECK(t.k == 1,
                   "E2: Backspace on an empty line decrements K (merge with the previous line)");
        t.OnBackspace();
        TEST_CHECK(t.k == 0, "E2: a second empty-line Backspace decrements K again");
        t.OnBackspace();
        TEST_CHECK(t.k == 0,
                   "E2: Backspace on the first line (K=0) never drives K negative");
    }
    {
        // The full E2 story: clearing line 3 entirely, then one more press
        // merges the emptied line into line 2.
        LineBreakTracker t;
        t.OnShiftEnter();
        for (int i = 0; i < 4; ++i) t.OnTextInput();
        t.OnShiftEnter();
        for (int i = 0; i < 4; ++i) t.OnTextInput();
        t.OnShiftEnter();
        for (int i = 0; i < 4; ++i) t.OnTextInput();
        TEST_CHECK(t.k == 3 && t.line_input == 4, "E2 fixture: 3 separators, line 3 non-empty");
        for (int i = 0; i < 4; ++i) t.OnBackspace(); // clear line 3
        TEST_CHECK(t.k == 3 && t.line_input == 0,
                   "E2: clearing a line's text never touches K");
        t.OnBackspace(); // empty line -> merge
        TEST_CHECK(t.k == 2,
                   "E2: the merge press after clearing line 3 decrements K to 2");
    }

    // ---- (c) tracker contract: saturation, clamp, Reset, key classifier --
    {
        LineBreakTracker t;
        for (int i = 0; i < 1000; ++i) {
            t.OnTextInput();
        }
        TEST_CHECK(t.line_input == LineBreakTracker::kLineInputCap && t.k == 0,
                   "tracker: the per-line counter saturates at kLineInputCap (K untouched)");
        for (int i = 0; i < 1000; ++i) {
            t.OnShiftEnter();
        }
        TEST_CHECK(t.k == static_cast<int>(kMaxEnterTranslateNewlines),
                   "tracker: K clamps at the guard's newline ceiling (256 after E9)");
        TEST_CHECK(ShiftEnterKNext(static_cast<int>(kMaxEnterTranslateNewlines), false) ==
                       static_cast<int>(kMaxEnterTranslateNewlines),
                   "K clamp: a saturated K stays clamped");
        TEST_CHECK(ShiftEnterKNext(static_cast<int>(kMaxEnterTranslateNewlines) - 1, false) ==
                       static_cast<int>(kMaxEnterTranslateNewlines),
                   "K clamp: the last increment lands exactly on the ceiling");
        t.Reset();
        TEST_CHECK(t.k == 0 && t.line_input == 0, "tracker: Reset zeroes both fields");
    }
    {
        // IsTextProducingKey matrix (the hook's per-line accounting input).
        TEST_CHECK(IsTextProducingKey(VK_PROCESSKEY, false, false, false),
                   "IME composition keys (VK_PROCESSKEY) count as text");
        TEST_CHECK(IsTextProducingKey(VK_SPACE, false, false, false),
                   "space counts as text");
        TEST_CHECK(IsTextProducingKey('A', false, false, false) &&
                       IsTextProducingKey('5', false, false, false) &&
                       IsTextProducingKey(VK_OEM_2, false, false, false) &&
                       IsTextProducingKey(VK_NUMPAD0, false, false, false),
                   "letters / digits / OEM punctuation / numpad count as text");
        TEST_CHECK(!IsTextProducingKey(VK_BACK, false, false, false) &&
                       !IsTextProducingKey(VK_DELETE, false, false, false) &&
                       !IsTextProducingKey(VK_RETURN, false, false, false) &&
                       !IsTextProducingKey(VK_TAB, false, false, false) &&
                       !IsTextProducingKey(VK_ESCAPE, false, false, false) &&
                       !IsTextProducingKey(VK_LEFT, false, false, false) &&
                       !IsTextProducingKey(VK_UP, false, false, false) &&
                       !IsTextProducingKey(VK_HOME, false, false, false) &&
                       !IsTextProducingKey(VK_NEXT, false, false, false) &&
                       !IsTextProducingKey(VK_F9, false, false, false),
                   "editing / navigation / function keys are NOT text");
        TEST_CHECK(!IsTextProducingKey('A', /*ctrl*/ true, false, false) &&
                       !IsTextProducingKey('V', true, false, false) &&
                       !IsTextProducingKey(VK_SPACE, false, /*alt*/ true, false) &&
                       !IsTextProducingKey('E', false, false, /*win*/ true),
                   "Ctrl/Alt/Win chords are commands, not text (Ctrl+V is counted at its own hook site)");
        // The classifier is the ONLY input to the line counter, so the E2
        // invariant "Backspace never fires on a text key" holds by construction.
        TEST_CHECK(!IsTextProducingKey(VK_BACK, false, false, false) &&
                       !IsTextProducingKey(VK_DELETE, false, false, false),
                   "E2: Backspace/Delete can never advance the line counter");
    }

    // ---- (d) E9: the raised capture ceilings ------------------------------
    static_assert(kMaxEnterTranslateChars == 16384,
                  "E9: guard char limit raised 4096 -> 16384");
    static_assert(kMaxEnterTranslateNewlines == 256,
                  "E9: guard newline limit raised 64 -> 256");
    TEST_CHECK(EnterCaptureWithinGuard(kMaxEnterTranslateChars, kMaxEnterTranslateNewlines),
               "E9: the guard allows exactly the new limit (16384 chars / 256 newlines)");
    TEST_CHECK(!EnterCaptureWithinGuard(kMaxEnterTranslateChars + 1, kMaxEnterTranslateNewlines),
               "E9: one char over the new limit is still rejected");
    TEST_CHECK(!EnterCaptureWithinGuard(kMaxEnterTranslateChars, kMaxEnterTranslateNewlines + 1),
               "E9: one newline over the new limit is still rejected");
    TEST_CHECK(EnterCaptureWithinGuard(5000, 3),
               "E9: a 5000-char single-line capture now passes (aborted under the old 4096)");
    TEST_CHECK(!EnterCaptureWithinGuard(20000, 3),
               "E9: a document-sized single block is still rejected (V3 abuse defense)");
    {
        // 1-2 pages of Shift+Enter text: 120 logical lines of ~100 chars.
        std::wstring page;
        for (int line = 0; line < 120; ++line) {
            page += std::wstring(100, L'a');
            if (line + 1 < 120) {
                page += L"\r\n";
            }
        }
        size_t nl = 0;
        for (wchar_t c : page) {
            if (c == L'\n' || c == L'\r') {
                ++nl;
            }
        }
        TEST_CHECK(page.size() == 12000 + 119 * 2 && nl == 119 * 2,
                   "E9 fixture: a 120-line page is built (12238 chars / 238 newline chars)");
        TEST_CHECK(EnterCaptureWithinGuard(page.size(), nl),
                   "E9: a 120-line (~1.5 page) Shift+Enter message passes the guard");
        // The F3 block-slice still addresses the current block inside it:
        // K=2 -> the last 3 logical lines.
        const size_t expect_start = page.size() - (3 * 100 + 2 * 2);
        TEST_CHECK(FindCurrentBlockStart(page, 2) == expect_start,
                   "E9: FindCurrentBlockStart slices the current block out of the long capture");
    }
    {
        // What makes the raise safe: an OVER-guard whole accumulation is
        // re-bounded to the CURRENT block (the last K+1 logical lines)
        // instead of aborting - the F3 slice-before-guard contract.
        const std::wstring doc = std::wstring(17000, L'a') + L"\r\n" +
                                 L"fresh line one\r\nfresh line two\r\nfresh line three";
        size_t nl = 0;
        for (wchar_t c : doc) {
            if (c == L'\n' || c == L'\r') {
                ++nl;
            }
        }
        TEST_CHECK(!EnterCaptureWithinGuard(doc.size(), nl),
                   "E9 fixture: the accumulation is over the new guard");
        const WholeCaptureSlice s = SliceWholeCaptureToBlock(doc, 2);
        TEST_CHECK(s.result == EnterCaptureResult::BlockSliced,
                   "E9: over-guard accumulation slices to the current block (no abort, no document overwrite)");
        TEST_CHECK(s.block == std::wstring_view(L"fresh line one\r\nfresh line two\r\nfresh line three"),
                   "E9: the sliced block is the last K+1 logical lines (FindCurrentBlockStart)");
        // A single block that is itself over-guard keeps the abort.
        const std::wstring one_block(17000, L'x');
        TEST_CHECK(SliceWholeCaptureToBlock(one_block, 0).result == EnterCaptureResult::GuardAbort,
                   "E9: a document-sized single block still aborts (abuse defense intact)");
    }

    // ---- (e) ISSUE-013: system-metric drag threshold ----------------------
    TEST_CHECK(MouseHook::DragThresholdFromMetrics(4, 4) == 4,
               "ISSUE-013: equal drag metrics -> that threshold");
    TEST_CHECK(MouseHook::DragThresholdFromMetrics(2, 8) == 8 &&
                   MouseHook::DragThresholdFromMetrics(8, 2) == 8,
               "ISSUE-013: the larger axis wins (the gate is one Euclidean radius)");
    TEST_CHECK(MouseHook::DragThresholdFromMetrics(0, 7) == 7 &&
                   MouseHook::DragThresholdFromMetrics(7, 0) == 7,
               "ISSUE-013: a zero on one axis does not disable the threshold");
    TEST_CHECK(MouseHook::DragThresholdFromMetrics(0, 0) == 3 &&
                   MouseHook::DragThresholdFromMetrics(-1, -1) == 3,
               "ISSUE-013: registry-set 0 metrics fall back to the 3 px floor (REQ-052 P2)");

    // ---- (f) wiring source pins (properties no headless run can observe) --
    {
        const std::string hook_hpp_src = V107ReadRepoFile("src/hook.hpp");
        const std::string hook_src = V107ReadRepoFile("src/hook.cpp");
        const std::string mouse_src = V107ReadRepoFile("src/mouse_hook.cpp");
        if (hook_hpp_src.empty() || hook_src.empty() || mouse_src.empty()) {
            std::cout << "[SKIP] v0.10.7 wiring source pins skipped (repo files not resolvable "
                         "from the test CWD)." << std::endl;
        } else {
            // E1/E2: the pure tracker exists and the hook owns exactly one.
            TEST_CHECK(hook_hpp_src.find("struct LineBreakTracker") != std::string::npos,
                       "E1/E2: hook.hpp defines the pure LineBreakTracker");
            TEST_CHECK(hook_hpp_src.find("LineBreakTracker line_break_;") != std::string::npos,
                       "E1/E2: KeyboardHook owns one tracker instance");
            TEST_CHECK(hook_hpp_src.find("[[maybe_unused]] bool ime_composing") != std::string::npos,
                       "E1: ShiftEnterKNext's ime_composing freeze is retired (parameter retained, ignored)");
            TEST_CHECK(hook_hpp_src.find("constexpr bool IsTextProducingKey(") != std::string::npos,
                       "E1/E2: the text-producing-key classifier is a shared pure definition");
            // E1/E2: the hook routes through the tracker, not ad-hoc arithmetic.
            TEST_CHECK(hook_src.find("line_break_.OnShiftEnter()") != std::string::npos,
                       "E1: Shift+Enter advances K through the tracker");
            TEST_CHECK(hook_src.find("line_break_.OnBackspace()") != std::string::npos,
                       "E2: Backspace/Delete go through the tracker");
            TEST_CHECK(hook_src.find("store(k - 1") == std::string::npos,
                       "E2: the unconditional K decrement on Backspace is gone");
            TEST_CHECK(hook_src.find("IsTextProducingKey(kbd->vkCode") != std::string::npos,
                       "E1/E2: text-producing keys feed the per-line counter");
            TEST_CHECK(hook_src.find("line_break_.k =") != std::string::npos,
                       "E1/E2: the tracker re-syncs its K from the shared atomic every keydown");
            // ISSUE-013: the drag threshold derives from the system metrics.
            TEST_CHECK(mouse_src.find("GetSystemMetricsForDpi") != std::string::npos &&
                           mouse_src.find("SM_CXDRAG") != std::string::npos &&
                           mouse_src.find("SM_CYDRAG") != std::string::npos,
                       "ISSUE-013: the drag threshold reads SM_CXDRAG/SM_CYDRAG for the press DPI");
            TEST_CHECK(mouse_src.find("DragThresholdFromMetrics(") != std::string::npos,
                       "ISSUE-013: the Win32 seam composes through the pure combine helper");
            TEST_CHECK(mouse_src.find("dist_sq >= threshold_sq") != std::string::npos,
                       "ISSUE-013: the drag gate compares against the derived threshold");
            TEST_CHECK(mouse_src.find("15 * 15") == std::string::npos,
                       "ISSUE-013: the hard-coded 15 px drag threshold is gone");
        }
    }

    const int delta = g_failed_count - failures_before;
    failures += delta;
    if (delta == 0) {
        std::cout << "[PASS] v0.10.7 line-break / ceiling / drag-threshold tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] v0.10.7 line-break / ceiling / drag-threshold tests: "
                  << delta << " check(s) failed." << std::endl;
    }
    return delta;
}
