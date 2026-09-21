// ---------------------------------------------------------------------------
// REQ-051 (session 260921, symptom A): the strengthened copy-chord retry
// policy - ONE shared schedule for the drag and Enter paths, the drag
// path's 3-attempt cycle, the CopyChordDropped seam verdict, and the Enter
// path's dropped-chord streak policy. Staged as an .inc next to this runner;
// registered near the end of main() (after TestReq050HfAddDialog).
//
// Coverage:
//   (a) The unified pure schedule CopyChordRetryAttemptTimeoutMs(attempt,
//       total_attempts): attempt 0 = single-shot 80 ms, the LAST attempt of
//       a multi-attempt cycle = extended 120 ms, middle attempts = 80 ms,
//       out-of-range = single-shot fallback. Includes byte-equivalence pins
//       against the REQ-001-tested CopyAttemptTimeoutMs profile (2 attempts)
//       so the shipped Enter-path behavior cannot drift.
//   (b) The drag-path constants: kDragCopyChordAttempts = 3 with the
//       {80, 80, 120} budget table, kDragCopyRetryBackoffMs = 70 (moved from
//       main.cpp), and the worst-case cycle bound (static_assert in the
//       header; mirrored here at runtime).
//   (c) The per-attempt baseline re-read invariant, expressed as pure logic:
//       CopyChordWithSettledRetry must NOT read the clipboard sequence (or
//       the clipboard text) itself - the REQ-R04 baseline re-read lives
//       inside EVERY CopySelectionWithSequenceWait attempt - and inside that
//       wait the sequence read must precede the Ctrl+C keystroke. Pinned by
//       source inspection (same technique as TestReq048CloudFailureSignaling;
//       the invariant is inside a Win32-coupled function and cannot run
//       hermetically).
//   (d) EnterCaptureResult::CopyChordDropped: appended AFTER the existing
//       values (ordinal stability for the log-analysis scripts), a distinct
//       shape-only label, and the DroppedChordFailureStreakSurfaces policy
//       (surface exactly when the consecutive streak reaches 1 - the
//       main.cpp:748 latch algebra; persistence suppresses, recovery clears).
// ---------------------------------------------------------------------------

void TestReq051CaptureRetryPolicy() {
    std::cout << "[RUN] Testing REQ-051 capture retry policy..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) the unified schedule + the REQ-001 equivalence pins ----------
    TEST_CHECK(CopyChordRetryAttemptTimeoutMs(0, 1) == kClipboardChangeTimeoutMs &&
                   CopyChordRetryAttemptTimeoutMs(0, 2) == kClipboardChangeTimeoutMs &&
                   CopyChordRetryAttemptTimeoutMs(0, 3) == kClipboardChangeTimeoutMs,
               "REQ-051: attempt 0 always waits the single-shot 80 ms budget");
    TEST_CHECK(CopyChordRetryAttemptTimeoutMs(1, 2) == 120,
               "REQ-051: the LAST attempt of a 2-attempt cycle waits 120 ms");
    TEST_CHECK(CopyChordRetryAttemptTimeoutMs(1, 3) == kClipboardChangeTimeoutMs,
               "REQ-051: a MIDDLE attempt (1 of 3) waits the single-shot budget");
    TEST_CHECK(CopyChordRetryAttemptTimeoutMs(2, 3) == 120,
               "REQ-051: the LAST attempt of a 3-attempt cycle waits 120 ms");
    TEST_CHECK(CopyChordRetryAttemptTimeoutMs(-1, 2) == kClipboardChangeTimeoutMs &&
                   CopyChordRetryAttemptTimeoutMs(5, 2) == kClipboardChangeTimeoutMs &&
                   CopyChordRetryAttemptTimeoutMs(9, 3) == kClipboardChangeTimeoutMs,
               "REQ-051: out-of-range attempts fall back to the single-shot budget");
    // Byte-equivalence with the REQ-001-tested Enter-path profile (2 attempts):
    // every value the REQ-001 suite pins must be identical through the
    // unified function.
    TEST_CHECK(CopyAttemptTimeoutMs(0) == 80 && CopyAttemptTimeoutMs(1) == 120,
               "REQ-051: CopyAttemptTimeoutMs (Enter profile) keeps 80/120");
    TEST_CHECK(CopyAttemptTimeoutMs(-1) == kClipboardChangeTimeoutMs &&
                   CopyAttemptTimeoutMs(kClipboardCopyChordAttempts) == kClipboardChangeTimeoutMs,
               "REQ-051: CopyAttemptTimeoutMs out-of-range fallback unchanged");
    TEST_CHECK(CopyAttemptTimeoutMs(0) == CopyChordRetryAttemptTimeoutMs(0, kClipboardCopyChordAttempts) &&
                   CopyAttemptTimeoutMs(1) == CopyChordRetryAttemptTimeoutMs(1, kClipboardCopyChordAttempts),
               "REQ-051: CopyAttemptTimeoutMs IS the 2-attempt profile of the unified schedule");

    // ---- (b) the drag-path strengthened cycle ------------------------------
    volatile int drag_attempts = kDragCopyChordAttempts;
    TEST_CHECK(drag_attempts == 3,
               "REQ-051: the drag copy cycle runs 3 attempts (was 2)");
    TEST_CHECK(kDragCopyAttemptTimeoutMs[0] == kClipboardChangeTimeoutMs &&
                   kDragCopyAttemptTimeoutMs[1] == kClipboardChangeTimeoutMs &&
                   kDragCopyAttemptTimeoutMs[2] == 120,
               "REQ-051: drag attempt budgets are {80, 80, 120}");
    volatile uint32_t drag_backoff = kDragCopyRetryBackoffMs;
    TEST_CHECK(drag_backoff == 70,
               "REQ-051: kDragCopyRetryBackoffMs keeps the REQ-005 70 ms value");
    volatile uint32_t drag_worst_case = kDragCopyAttemptTimeoutMs[0] + kDragCopyAttemptTimeoutMs[1] +
                                        kDragCopyAttemptTimeoutMs[2] +
                                        kDragCopyRetryBackoffMs * (kDragCopyChordAttempts - 1);
    TEST_CHECK(drag_worst_case <= 500,
               "REQ-051: worst-case 3-attempt drag cycle stays <= 500 ms");
    // The schedule function must agree with the drag table for every
    // in-range index (the helper consumes the function, not the table).
    bool drag_table_agrees = true;
    for (int a = 0; a < kDragCopyChordAttempts; ++a) {
        if (CopyChordRetryAttemptTimeoutMs(a, kDragCopyChordAttempts) != kDragCopyAttemptTimeoutMs[a]) {
            drag_table_agrees = false;
        }
    }
    TEST_CHECK(drag_table_agrees,
               "REQ-051: the unified schedule reproduces the drag budget table");

    // ---- (c) the per-attempt baseline re-read invariant (structural) -------
    const std::filesystem::path wi_path = ResolveRepoFile(L"src\\win32_input.cpp");
    TEST_CHECK(!wi_path.empty(),
               "REQ-051: src/win32_input.cpp located via ResolveRepoFile");
    if (!wi_path.empty()) {
        std::ifstream wi_in(wi_path, std::ios::binary);
        TEST_CHECK(static_cast<bool>(wi_in),
                   "REQ-051: src/win32_input.cpp opened for reading");
        if (wi_in) {
            const std::string wi_src((std::istreambuf_iterator<char>(wi_in)),
                                     std::istreambuf_iterator<char>());
            TEST_CHECK(!wi_src.empty(), "REQ-051: src/win32_input.cpp non-empty after read");

            // (c1) inside CopySelectionWithSequenceWait, the clipboard
            // sequence baseline is read BEFORE the Ctrl+C keystroke - the
            // stale-read prevention the whole retry design must never
            // regress.
            const size_t seq_wait = wi_src.find(
                "bool CopySelectionWithSequenceWait(uint32_t change_timeout_ms)");
            const size_t seq_wait_end = wi_src.find("\nbool PasteSelection", seq_wait);
            TEST_CHECK(seq_wait != std::string::npos && seq_wait_end != std::string::npos &&
                           seq_wait_end > seq_wait,
                       "REQ-051 pin: CopySelectionWithSequenceWait body is locatable");
            if (seq_wait != std::string::npos && seq_wait_end != std::string::npos &&
                seq_wait_end > seq_wait) {
                const std::string wait_body =
                    wi_src.substr(seq_wait, seq_wait_end - seq_wait);
                const size_t baseline_pos = wait_body.find("GetClipboardSequenceNumber");
                const size_t chord_pos = wait_body.find("CopySelection()");
                TEST_CHECK(baseline_pos != std::string::npos &&
                               chord_pos != std::string::npos &&
                               baseline_pos < chord_pos,
                           "REQ-051 pin: the sequence baseline is re-read BEFORE the "
                           "Ctrl+C keystroke (stale-read invariant)");
            }

            // (c2) the shared retry driver NEVER reads the clipboard itself:
            // no sequence read, no clipboard text read - every attempt's
            // baseline comes from inside its own
            // CopySelectionWithSequenceWait call. This is the per-attempt
            // re-read guarantee expressed structurally.
            const size_t driver_pos = wi_src.find("bool CopyChordWithSettledRetry(");
            const size_t driver_end = wi_src.find("std::wstring GetClipboardText(",
                                                  driver_pos);
            TEST_CHECK(driver_pos != std::string::npos && driver_end != std::string::npos &&
                           driver_end > driver_pos,
                       "REQ-051 pin: CopyChordWithSettledRetry body is locatable");
            if (driver_pos != std::string::npos && driver_end != std::string::npos &&
                driver_end > driver_pos) {
                const std::string driver_body =
                    wi_src.substr(driver_pos, driver_end - driver_pos);
                TEST_CHECK(driver_body.find("CopySelectionWithSequenceWait") != std::string::npos,
                           "REQ-051 pin: the retry driver drives the sequence wait");
                TEST_CHECK(driver_body.find("CopyChordRetryAttemptTimeoutMs(attempt, total_attempts)") !=
                               std::string::npos,
                           "REQ-051 pin: the retry driver consumes the unified schedule");
                TEST_CHECK(driver_body.find("GetClipboardSequenceNumber") == std::string::npos,
                           "REQ-051 pin: the retry driver holds NO sequence baseline of "
                           "its own (every attempt re-reads inside the wait)");
                TEST_CHECK(driver_body.find("GetClipboardText") == std::string::npos,
                           "REQ-051 pin: the retry driver never reads clipboard text "
                           "(no stale-read exposure at the driver level)");
            }

            // (c3) the main.cpp drag path routes through the shared driver
            // (the old inline 2-attempt block is gone).
            const std::filesystem::path main_path = ResolveRepoFile(L"src\\main.cpp");
            if (!main_path.empty()) {
                std::ifstream main_in(main_path, std::ios::binary);
                if (main_in) {
                    const std::string main_src((std::istreambuf_iterator<char>(main_in)),
                                               std::istreambuf_iterator<char>());
                    TEST_CHECK(main_src.find("CopyChordWithSettledRetry") != std::string::npos,
                               "REQ-051 pin: the drag path uses the shared retry driver");
                    TEST_CHECK(main_src.find("MAIN/DragIconClick/004") == std::string::npos,
                               "REQ-051 pin: the old inline 2-attempt drag retry block "
                               "is fully replaced (no stray /004 tokens)");
                }
            }
        }
    }

    // ---- (d) the CopyChordDropped verdict + the streak policy --------------
    // Read through volatiles: the ordinals/distinctness are compile-time
    // constants, and a direct TEST_CHECK on them trips C4127 under /W4 (the
    // established pattern in this suite and the repo).
    volatile int editor_excluded_ord = static_cast<int>(EnterCaptureResult::EditorExcluded);
    volatile int copy_chord_dropped_ord = static_cast<int>(EnterCaptureResult::CopyChordDropped);
    TEST_CHECK(editor_excluded_ord == 6 && copy_chord_dropped_ord == 7,
               "REQ-051: CopyChordDropped is appended after the existing verdicts "
               "(log-analysis ordinal stability)");
    volatile bool dropped_distinct =
        EnterCaptureResult::CopyChordDropped != EnterCaptureResult::CopyChordFailed;
    TEST_CHECK(dropped_distinct,
               "REQ-051: the dropped-chord verdict is distinct from the benign "
               "paste-window verdict");
    TEST_CHECK(std::string_view(EnterCaptureResultName(EnterCaptureResult::CopyChordDropped)) ==
                   "copy_chord_dropped",
               "REQ-051: CopyChordDropped carries its own shape-only label");
    TEST_CHECK(std::string_view(EnterCaptureResultName(EnterCaptureResult::CopyChordFailed)) ==
                   "copy_chord_failed",
               "REQ-051: the benign CopyChordFailed label is unchanged");
    volatile int drop_threshold = kDroppedChordNoticeStreakThreshold;
    TEST_CHECK(drop_threshold == 1,
               "REQ-051: the dropped-chord notice threshold is 1 (first of a streak)");
    TEST_CHECK(!DroppedChordFailureStreakSurfaces(0),
               "REQ-051: no notice before any drop");
    TEST_CHECK(DroppedChordFailureStreakSurfaces(1),
               "REQ-051: the notice surfaces on the streak ENTRY (once)");
    TEST_CHECK(!DroppedChordFailureStreakSurfaces(2) && !DroppedChordFailureStreakSurfaces(5),
               "REQ-051: persistence suppresses the re-arm (the latch algebra)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-051 capture retry policy passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-051 capture retry policy: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
