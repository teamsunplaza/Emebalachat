#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace emebalachat {

// Synthetic input marker placed in dwExtraInfo to prevent recursive hook
// re-entry. L2 fix: this is now a per-process random value chosen at process
// startup instead of the old compile-time constant 0x1337BEEF, which any local
// process could hardcode to make our hooks silently ignore spoofed synthetic
// events. Note (accepted-risk documentation): dwExtraInfo filtering remains a
// heuristic dedupe hint, NOT an integrity boundary - a determined local
// attacker with read access to this process's memory can still observe the
// value. Raising the bar from "trivially guessable constant" to "per-process
// secret" closes the casual-spoofing path with zero behavior change, because
// every producer and consumer resolves the same symbol within this process.
extern const DWORD EXTRA_INFO_MARKER;

// Snapshot of clipboard state across translation swaps
struct ClipboardBackup {
    std::optional<std::wstring> text;
    std::vector<std::pair<UINT, std::vector<uint8_t>>> formats;
};

// Returns true if format is a GDI handle (CF_BITMAP, CF_PALETTE, CF_METAFILEPICT, CF_ENHMETAFILE)
// which cannot be duplicated via GlobalAlloc/GlobalLock.
bool IsGdiClipboardFormat(UINT format);

// Flushes Korean/CJK IME composition buffer by sending synthetic VK_RIGHT with EXTRA_INFO_MARKER + 10ms delay.
void FlushIme();

// Multi-line block fix: selects the message block from the start of the text
// flow to the cursor (Shift+Home then Ctrl+Shift+Home, extending the selection
// from line start back to the beginning of the input's text). Replaces the old
// line-only boundary, which captured ONLY the last physical line of a multi-line
// typed/pasted block, so Enter-translate replaced just that last line in every
// non-whitelisted app (browser chat boxes, editors) and left the preceding
// lines untranslated. Works identically for every script/language because the
// selection primitive is pure keyboard geometry, not text inspection.
bool SelectMessageBlock();

// Sends Ctrl+C to copy current selection to clipboard.
bool CopySelection();

// Sends Ctrl+V to paste clipboard contents into target control.
bool PasteSelection();

// Backs up clipboard contents safely skipping GDI objects.
bool BackupClipboard(ClipboardBackup& out, DWORD timeout_ms = 100);

// Restores previously backed up clipboard formats.
bool RestoreClipboard(const ClipboardBackup& in, DWORD timeout_ms = 100);

// Retrieves CF_UNICODETEXT from Windows clipboard.
std::wstring GetClipboardText(DWORD timeout_ms = 100);

// Places Unicode text onto Windows clipboard as CF_UNICODETEXT.
bool SetClipboardText(std::wstring_view text, DWORD timeout_ms = 100);

// Phase 5 (REQ-011, amended by F4/A2 for session 260908_0002): keyboard-
// pipeline app category. CategoryA = chat/command apps where Enter means
// "send/execute" (KakaoTalk, Slack, Discord, WhatsApp, terminals-as-command-
// pipelines, etc.): the whole input is selected (Ctrl+A), replaced, and the
// synthetic Enter sends it. CategoryB = editor-type apps (Notepad, IDE text
// editors, browser text areas) where the caret-line/block context is replaced
// and a real newline is always injected afterwards (REQ-012/023).
// Classification is a pure static exe-name table lookup; anything not in the
// CategoryA table is CategoryB (fail-open to the editor path, which is the
// pre-Phase-5 default for all non-chat apps).
//
// F4/A2 (REQ-011 reversal): the VS Code family (Code.exe / Code - Insiders.exe
// / Cursor.exe / Windsurf.exe / VSCodium.exe) and the AI CLI editors
// (opencode.exe / claude.exe / codex.exe) are NO LONGER CategoryA. They are
// IDE/editor apps whose bare Enter must insert a native newline - never select
// and replace the whole source document (V3 verify: 2076-char overwrite).
// They are classified by IsEditorExeNameForEnterExclusion and excluded from
// the ENTER translate pipeline at the hook gate (pass-through) and inside
// CopySelectedText (empty capture), while drag-to-translate stays untouched.
// The capture-size guard (kMaxEnterTranslateChars / kMaxEnterTranslateNewlines)
// is the category-independent safety net for any residual misclassification.
enum class AppCategory : unsigned char { CategoryA, CategoryB };

// Phase 5 (REQ-011): classifies the process owning hwnd into the keyboard
// pipeline category. Same process-name resolution as the old
// IsChatApplicationWindow (QueryFullProcessImageNameW -> basename ->
// case-insensitive compare), but returns the category instead of a bool.
// Fails closed to CategoryB on any resolution failure (null hwnd, bad pid,
// OpenProcess/Query failure): the editor path is the safe default because it
// never sends the message on the user's behalf beyond a newline.
// F4/A2 diagnostics: every fail-open stage and the final table miss log an
// attributed reason (WIN32_INPUT/ClassifyAppWindow/001-005) so a runtime
// log can pinpoint where a window fell to CategoryB.
AppCategory ClassifyAppWindow(HWND hwnd);

// F4 (A2 W1 결정 2/3): true when the process owning hwnd is an editor/IDE
// excluded from the ENTER translate pipeline (kEditorApps: VS Code family +
// AI CLI editors). Same process-name resolution and case-insensitive compare
// as ClassifyAppWindow, but the FAIL-OPEN polarity is inverted: any
// resolution failure returns FALSE (not excluded), so the app flows through
// normal classification where the capture-size guard (EnterCaptureWithinGuard)
// remains the last line of defense. Never consulted by the drag path.
bool IsEnterTranslateExcludedApp(HWND hwnd);

// Pure basename matchers over the classifier tables, exposed for the headless
// unit tests (same single-definition discipline as CopyChordRetryWarranted /
// EnterCaptureWithinGuard). Case-insensitive.
bool IsChatAppExeNameForEnterTranslation(std::wstring_view basename);
bool IsEditorExeNameForEnterExclusion(std::wstring_view basename);

// F4 (A2 W2 결정 4): Enter-pipeline capture-size guard. Language-neutral
// thresholds (UTF-16 char count + newline-char count - no script/language
// weighting). A whole-block capture above these is document-sized (V3:
// 2076-char overwrite), never a chat message: the Enter translate must abort
// to an empty capture. The EM tail path is exempt by construction (its
// geometry is bounded by the previous translation end, so it never trips the
// guard - see CopySelectedText).
// F3 RAISE (session 260908_0003, verify 220750 §5-2): the old 512/16 limits
// conflicted with the 예시1 contract - on the non-EM fallback the capture is
// the WHOLE [0..caret) accumulation, so 6+ translated sentences (each ~100
// chars + terminator) exceeded 512 and the guard aborted the translation of
// the CURRENT block together with it. 4096 covers a comfortably long
// accumulation (≈40 sentences of 100 chars) while still rejecting pasted
// documents; 64 newlines is the abuse ceiling aligned with the hook's
// Shift+Enter counter cap (ShiftEnterKNext clamps K to this constant, so the
// block slice can never ask for more than 65 lines). Whole-document captures
// above these limits are still rejected outright; captures UNDER these
// limits but spanning multiple blocks are now handled safely by the F3
// block-slice (worker.hpp FindCurrentBlockStart), which is what the raise
// makes reachable.
inline constexpr size_t kMaxEnterTranslateChars = 4096;
inline constexpr size_t kMaxEnterTranslateNewlines = 64;

// Pure guard predicate (single shared definition for CopySelectedText and the
// unit tests - same discipline as CopyChordRetryWarranted). True when the
// capture shape is inside the Enter-translate limits.
constexpr bool EnterCaptureWithinGuard(size_t chars, size_t newlines) {
    return chars <= kMaxEnterTranslateChars &&
           newlines <= kMaxEnterTranslateNewlines;
}

// ---------------------------------------------------------------------------
// Session 260913_0001 (Reddit long-post capture fix, debug report 022121 §7
// Step 2): FindCurrentBlockStart was RELOCATED here from worker.hpp so the
// capture seam (CopySelectedText slice-before-guard below) and the worker's
// F3 block slice share the ONE definition (pure-predicate discipline).
// worker.hpp includes this header, so worker.cpp and run_tests.cpp keep the
// name visible and compile unchanged - this was a pure move, no semantics.
// ---------------------------------------------------------------------------
// F3 (session 260908_0003, verify 220750 §6 "수정-Ananke" adopted design):
// block-slice-from-whole-capture. The non-EM CategoryB fallback selection is
// the WHOLE [0..caret) accumulation; the CURRENT block is the last K+1
// logical lines, K = the Shift+Enter passthroughs the hook counted since the
// last bare-Enter capture. Everything before the slice point is earlier
// (already-translated) content and MUST be preserved verbatim - the worker
// recomposes [prefix][translation of block] with the SAME machinery as the
// REQ-F2 PrefixWithTail branch, so 예시1/2/3 hold even when the ledger chain
// broke (the R2/R4 whole-document destruction path of verify 220750 §2).
//
// Pure helper (no Win32 calls, unit-testable headlessly). Input should be
// CRLF-normalized (ExecuteTask normalizes at the capture seam) but lone-LF /
// lone-CR separators are tolerated: a \r\n PAIR is one logical newline, and
// a lone \r or \n is one too. Two ADJACENT terminators ("AAA\r\n\r\nBBB")
// are TWO boundaries with an empty logical line between them - exactly the
// split() semantics the "last K+1 LOGICAL lines" contract needs (merging
// them would under-count Shift+Enters and reach back into translated
// content). Returns the START INDEX of the current block within `capture`:
//   - empty capture, negative K, or fewer than K+1 terminators in the text
//     -> 0 (whole capture is the block: first-block geometry, legacy safe);
//   - otherwise: the index immediately after the (K+1)-th terminator
//     counted from the END. A capture ending in a terminator (caret on a
//     fresh empty line) with K=0 therefore yields index == capture.size() -
//     the "empty tail" the worker turns into a plain send-through (041)
//     instead of re-translating the prefix;
//   - K over-count (fewer terminators than K+1) clamps to 0, never out of
//     bounds. Word-wrap is irrelevant BY DESIGN: the clipboard capture only
//     ever contains LOGICAL newlines (the app's own line breaks), never the
//     display-line folds - exactly why the rejected Shift+Up x K geometry
//     (verify 220750 §6-(i)) is not needed here.
// Surrogate safety: 0x0D/0x0A never participate in UTF-16 surrogate pairs,
// and the returned index always sits immediately after a terminator.
constexpr size_t FindCurrentBlockStart(std::wstring_view capture, int shift_enter_count) {
    if (capture.empty() || shift_enter_count < 0) {
        return 0;
    }
    const int want = shift_enter_count + 1; // terminators to cross from the end
    int crossed = 0;
    size_t i = capture.size();
    while (i > 0) {
        const wchar_t c = capture[i - 1];
        if (c != L'\r' && c != L'\n') {
            --i;
            continue;
        }
        // One logical terminator ends at `i`: a \r\n pair (two units) or a
        // lone \r / \n (one unit). A lone \r BEFORE another \r (or end-of-
        // scan) is its own line break; the \n of a pair swallows its \r.
        size_t term_start = i - 1;
        if (c == L'\n' && term_start > 0 && capture[term_start - 1] == L'\r') {
            --term_start;
        }
        ++crossed;
        if (crossed == want) {
            // The block starts right after this terminator (`i` is the index
            // one past its end). When the capture ENDS in a terminator,
            // i == capture.size(): the empty-tail shape the worker trims to
            // a send-through.
            return i;
        }
        i = term_start;
    }
    return 0; // fewer than K+1 terminators: the whole capture is the block
}

// Session 260913_0001 (Phase B diagnostics, debug report §8-1): the shape-only
// verdict of the Enter capture seam. CopySelectedText reports it through the
// optional out-param so the worker's capture log can distinguish WHY a
// capture came back empty (guard_abort vs copy_chord_failed vs
// empty_selection vs empty_tail) or why it differs from the raw selection
// (block_sliced). The old logs collapsed all of these into one
// `capture len=0` line, which made the Reddit long-post incident (debug
// report §3, H-A vs H-B indistinguishable) undiagnosable from the user's
// description alone. ENUMS AND LENGTHS ONLY - never user content, so every
// consumer stays compatible with the diag_log_content=false default.
enum class EnterCaptureResult {
    Ok,              // non-empty capture returned unchanged (within guard)
    BlockSliced,     // whole capture over guard; guard re-vetted against the current
                     // block and the WHOLE capture returned (worker F3 owns the split)
    GuardAbort,      // single block itself over guard; empty returned (V3 abuse defense)
    EmptyTail,       // current block empty (capture ends at a separator); empty returned
    CopyChordFailed, // Ctrl+C not confirmed within the retry budget; empty returned
    EmptySelection,  // copy confirmed but the clipboard text was empty; empty returned
    EditorExcluded,  // F4 editor/IDE exclusion backstop (/005); no selection attempted
};

constexpr const char* EnterCaptureResultName(EnterCaptureResult result) {
    switch (result) {
        case EnterCaptureResult::Ok:              return "ok";
        case EnterCaptureResult::BlockSliced:     return "block_sliced";
        case EnterCaptureResult::GuardAbort:      return "guard_abort";
        case EnterCaptureResult::EmptyTail:       return "empty_tail";
        case EnterCaptureResult::CopyChordFailed: return "copy_chord_failed";
        case EnterCaptureResult::EmptySelection:  return "empty_selection";
        case EnterCaptureResult::EditorExcluded:  return "editor_excluded";
    }
    return "unknown"; // unreachable; keeps the function total for constexpr use
}

// Session 260913_0001 root fix (debug report §7 Step 3): slice-before-guard for
// the NON-EM whole-capture geometry. SelectMessageBlock() selects [0..caret)
// - the whole accumulation of an editing session - because keyboard geometry
// cannot address a mid-document block on surfaces without EM_ support
// (Chrome contenteditable, e.g. the Reddit composer). The capture-size guard
// was written to vet a BLOCK, not an accumulation, so a long post crossed
// 4096 chars at roughly the 5th-6th Enter and the guard aborted the CURRENT
// block's translation together with the accumulation (symptom 2). This pure
// function re-bounds what the guard measures: under the guard the capture
// passes through byte-identical (result Ok - the slice NEVER engages, so every
// currently-working flow is untouched); over it, the current block (last K+1
// logical lines, leading separator run stripped by the same rule as the
// worker's F3 slice) is re-vetted instead. A single block that is itself
// over-guard is still a document-sized abuse shape and keeps the abort
// semantics (result GuardAbort). NOTE (P5 review 260913_0001, corrected):
// the slice NEVER changes the live selection in the target app - it stays
// [0..caret), the whole accumulation - so the seam MUST return the WHOLE
// capture (block-scoped re-vetting only); returning the block alone would
// make the worker paste a block-only replacement over the whole live
// selection and DESTROY the already-translated prefix (the worker can only
// recompose a prefix it actually received). The worker's F3 slice
// (FindCurrentBlockStart on the whole capture, its native input shape) then
// owns the prefix/block split, the engine sees only the block, and the paste
// recomposes [verbatim prefix][translated block] over the whole selection -
// byte-identical to the established sub-guard whole-capture flow.
struct WholeCaptureSlice {
    EnterCaptureResult result; // Ok | BlockSliced | EmptyTail | GuardAbort
    std::wstring_view block;   // meaningful when result is Ok or BlockSliced
};

constexpr WholeCaptureSlice SliceWholeCaptureToBlock(std::wstring_view capture,
                                                     int shift_enter_count) {
    size_t nl = 0;
    for (wchar_t c : capture) { if (c == L'\n' || c == L'\r') ++nl; }
    if (EnterCaptureWithinGuard(capture.size(), nl)) {
        return {EnterCaptureResult::Ok, capture}; // sub-limit: byte-identical passthrough
    }
    size_t block_start = FindCurrentBlockStart(capture, shift_enter_count);
    // Move a leading separator run into the verbatim prefix (same rule as the
    // worker's F3 slice, worker.cpp ExecuteTask: the engine must never see a
    // leading bare newline, and an all-separator tail is the empty block).
    while (block_start < capture.size() &&
           (capture[block_start] == L'\r' || capture[block_start] == L'\n')) {
        ++block_start;
    }
    if (block_start >= capture.size()) {
        return {EnterCaptureResult::EmptyTail, {}};
    }
    const std::wstring_view block = capture.substr(block_start);
    size_t block_nl = 0;
    for (wchar_t c : block) { if (c == L'\n' || c == L'\r') ++block_nl; }
    if (EnterCaptureWithinGuard(block.size(), block_nl)) {
        return {EnterCaptureResult::BlockSliced, block};
    }
    return {EnterCaptureResult::GuardAbort, {}};
}

// Phase 8 Batch 1 (REQ-005, plan 225900 §1.5/§4.1): true when hwnd belongs to
// a console/terminal surface where a SYNTHETIC Ctrl+C is interpreted as
// SIGINT (process interrupt) instead of "copy selection". Detection is by
// window class name - ConsoleWindowClass (conhost), CASCADIA_HOSTING_WINDOW_CLASS
// (Windows Terminal hosting frame), PseudoConsoleWindow (OpenConsole/pseudo
// console, also what GetConsoleWindow() returns under WT) - checked on the
// window itself AND its GA_ROOT ancestor, because the foreground hwnd may be
// the top-level frame or a nested console child. Class-name matching is more
// precise than exe-name matching: the selected-text hwnd in a terminal tab is
// owned by conhost/WT, not by the shell exe running inside it.
// Fails OPEN (returns false = keep existing behavior) on null/invalid hwnd
// or any class-query failure, so every non-terminal app retains the exact
// pre-Phase-8 clipboard capture path (regression blocker by construction).
// This gate is for the DRAG CAPTURE path only (run_drag_translate); the
// double-Ctrl+C path is deliberately not gated (user-initiated keystrokes).
bool IsConsoleCaptureUnsafe(HWND hwnd);

// Sends Ctrl+A to select all text in the active control.
bool SelectAll();

// ---- REQ-R04: clipboard copy-settle polling (Electron IPC robustness) ----
//
// Replaces the old fixed Sleep(35) after Ctrl+C. BackupClipboard() never calls
// EmptyClipboard(), so reading the clipboard before the target app's copy
// handler has written returns STALE text (audit 2.3: smart bypass then treats
// the stale English text as "already in target language" and skips
// translation - Electron apps like Discord/Slack commit the clipboard over IPC
// with unbounded latency). GetClipboardSequenceNumber() is a per-desktop
// counter incremented on every clipboard write (EmptyClipboard/SetClipboardData),
// so we wait until the number provably leaves its pre-Ctrl+C baseline and then
// until it holds stable. The stability window is required because handlers that
// write EmptyClipboard() first and SetClipboardData() second bump the sequence
// twice; reading on the first bump would observe an EMPTY clipboard.

// Max wait for the sequence number to move off the pre-Ctrl+C baseline.
// REQ-001 (session 260910_0003, Issue A): shrunk 180 -> 80 ms. The 180 ms
// budget was tuned for slow Electron clipboard commits, but it also taxes
// every confirmed-empty copy (Discord empty input: Ctrl+C over an empty
// selection changes nothing, so the WHOLE chord budget burns before the
// worker's has_text=0 send-through - the reported 1.55 s freeze). 80 ms
// stays generous for real commits (field logs: a legit copy moves the
// sequence within <50 ms) while capping the empty-input penalty. The
// single-shot callers sharing this default (drag path, double-Ctrl+C
// handler) accept the same 80 ms timeline (user-approved).
inline constexpr uint32_t kClipboardChangeTimeoutMs = 80;
// The sequence must hold this long after the last observed change before reading.
inline constexpr uint32_t kClipboardStableWindowMs = 16;
// Poll cadence for the real-clipboard driver (delegation: 5-10 ms).
inline constexpr uint32_t kClipboardPollIntervalMs = 8;
// Hard wall-clock deadline for the whole wait (change detection + settling).
inline constexpr uint64_t kClipboardCopyDeadlineMs =
    kClipboardChangeTimeoutMs + kClipboardStableWindowMs;

// ---- REQ-039 (chat-window Enter capture: Chromium dropped-chord) ----------
//
// Electron/Chromium targets intermittently drop a synthetic Ctrl+C chord:
// the selection commands were delivered, SendInput reports success, but the
// renderer-side clipboard write never lands (GetClipboardSequenceNumber
// stays on the pre-chord baseline), so CopySelectionWithSequenceWait's
// REQ-R04 stale-read refusal fires on a chord the app silently discarded
// (user log emebalachat_260907204046 L1782/L1860/L1939 - Discord, class
// Chrome_WidgetWin_1; same signature as PowerToys issue #46485). One such
// drop on the bare-Enter path means an EMPTY capture, which the worker's R5
// hold turns into a swallowed Enter. The fix re-runs the full
// selection+copy cycle a bounded number of times: every selection primitive
// (SelectAll / keyboard geometry / EM_SETSEL(last, caret)) is idempotent,
// and the sequence-wait re-baselines each attempt, so a late commit from an
// earlier chord is read as a confirmed copy rather than stale text.
inline constexpr int kClipboardCopyChordAttempts = 2;
// Settle between chord attempts so the target's input pipeline can drain -
// a half-processed chord is one drop hypothesis this gap addresses.
inline constexpr uint32_t kClipboardCopyChordRetryGapMs = 30;

// Backoff schedule (REQ-001, session 260910_0003 Issue A; SUPERSEDES the
// Option D {180, 400, 800} schedule from session 260910_0001): on a
// non-EM control with an EMPTY input (Discord et al., CategoryA), Ctrl+C
// legitimately changes nothing, the sequence never moves, and the
// stale-read refusal burns EVERY attempt at its full timeout - the old
// schedule stacked 180+400+800 ms (+ Sleep/gap overhead) into a ~1.5 s
// freeze before the worker's has_text=0 send-through, during which the
// user's impatient second Enter passed the hook (worker_busy) and the
// worker's late send-through then delivered BOTH Enters (duplicate Enter).
// The fix shrinks the whole cycle: attempt 1 waits the (now 80 ms)
// single-shot budget, attempt 2 waits 120 ms, and the schedule sums to
// <=200 ms. Real text-bearing copies move the sequence within ~50 ms
// (field logs: Option D showed attempt-2 confirming 400 ms writes, but
// those commits were still no-change-at-180 cases on EMPTY-selection
// targets; an actual text commit is confirmed in the sub-80 ms fast path).
// The stale-read refusal semantics are unchanged: a sequence that never
// moves is still Failed - only the patience shrinks.
inline constexpr uint32_t kClipboardCopyAttemptTimeoutMs[kClipboardCopyChordAttempts] =
    {80, 120};
static_assert(kClipboardCopyAttemptTimeoutMs[0] == kClipboardChangeTimeoutMs,
              "REQ-001: attempt 1 must equal the single-shot 80 ms budget");
static_assert(kClipboardCopyAttemptTimeoutMs[0] + kClipboardCopyAttemptTimeoutMs[1] <= 200,
              "REQ-001: the chord retry schedule must stay within the 200 ms cap");

// 0-based attempt index -> per-attempt change timeout. Out-of-range indices
// fall back to the attempt-1 budget (the safe, established timeline); the
// retry loop only ever passes in-range values under the CopyChordRetryWarranted
// budget. Single shared definition for CopySelectedText and the unit tests.
constexpr uint32_t CopyAttemptTimeoutMs(int attempt) {
    return (attempt >= 0 && attempt < kClipboardCopyChordAttempts)
               ? kClipboardCopyAttemptTimeoutMs[attempt]
               : kClipboardChangeTimeoutMs;
}

// Pure retry-warrant predicate (single definition shared by
// CopySelectedText and the unit tests; same discipline as
// SelectionReleaseRequired / EmptyCaptureNeedsHold). attempt_index is
// 0-based. False when the attempt budget is exhausted or when the
// established selection is PROVABLY EMPTY - the REQ-034 F3-B paste-window
// geometry (EM_SETSEL(last, last): Ctrl+C over an empty selection
// legitimately changes nothing, so re-sending the chord can only add
// latency before the worker's silent send-through).
constexpr bool CopyChordRetryWarranted(int attempt_index, bool selection_provably_empty) {
    return (attempt_index + 1 < kClipboardCopyChordAttempts) && !selection_provably_empty;
}

enum class ClipboardCopyOutcome { Pending, Confirmed, Failed };

// Pure, time-parameterized state machine behind CopySelectionWithSequenceWait().
// Deliberately free of Win32 calls so the complete timeline matrix (including
// the two-step EmptyClipboard->SetClipboardData race) is unit-testable
// headlessly with synthetic timestamps; see TestClipboardSequencePolling().
class ClipboardCopyWatcher {
public:
    // change_timeout_ms: per-attempt change budget (backoff schedule above).
    // The hard wall-clock deadline is derived as change_timeout_ms +
    // kClipboardStableWindowMs. The default keeps every single-shot caller on
    // the single-shot 80 ms / 96 ms REQ-R04 timeline (REQ-001 shrink).
    ClipboardCopyWatcher(uint32_t pre_copy_seq, uint64_t start_ms,
                         uint32_t change_timeout_ms = kClipboardChangeTimeoutMs)
        : pre_seq_(pre_copy_seq), start_ms_(start_ms), last_seq_(pre_copy_seq),
          change_timeout_ms_(change_timeout_ms) {}

    // Feed the clipboard sequence number observed at now_ms (same clock epoch
    // as start_ms; now_ms must be monotonically non-decreasing).
    ClipboardCopyOutcome Update(uint32_t current_seq, uint64_t now_ms);

    ClipboardCopyOutcome Outcome() const { return outcome_; }

private:
    uint32_t pre_seq_;
    uint64_t start_ms_;
    uint32_t last_seq_;
    uint32_t change_timeout_ms_;
    uint64_t last_change_ms_ = 0;
    bool advanced_ = false;
    ClipboardCopyOutcome outcome_ = ClipboardCopyOutcome::Pending;
};

// Sends Ctrl+C via CopySelection() and waits for the clipboard write using
// GetClipboardSequenceNumber() polling with the constants above. The baseline
// sequence number is captured IMMEDIATELY before the keystroke, so sequence
// jumps caused by the worker's earlier BackupClipboard()/RestoreClipboard()
// writes are absorbed by this fresh read (REQ-R04 directive).
// Returns true only when the clipboard provably changed (fresh data is safe to
// read). Returns false on SendInput failure or timeout: callers MUST NOT read
// the clipboard afterwards - it would still hold stale content.
// change_timeout_ms is the per-attempt change budget (see the backoff
// constants above); the default keeps single-shot callers (main.cpp
// double-Ctrl+C handler, drag path) on the same 80 ms timeline as attempt 1.
bool CopySelectionWithSequenceWait(uint32_t change_timeout_ms = kClipboardChangeTimeoutMs);

// ---- REQ-R13 (overlaps this batch): OpenClipboard contention backoff ----
inline constexpr int kClipboardOpenMaxAttempts = 5;
inline constexpr uint32_t kClipboardOpenBaseDelayMs = 5;
// Exponential retry schedule: attempts 1..4 yield 5, 10, 20, 40 ms sleeps
// (total 75 ms, inside the ~100 ms budget); 0 means "stop retrying".
constexpr uint32_t ClipboardOpenBackoffDelayMs(int attempt) {
    if (attempt < 1 || attempt >= kClipboardOpenMaxAttempts) {
        return 0;
    }
    return kClipboardOpenBaseDelayMs << (attempt - 1);
}

// ---- REQ-027 (Phase A §A-2): editor caret-offset tracker ----
//
// REQ-027 (Phase A §A-2): EM_*를 실제 처리하는 컨트롤 한정 "직전 번역 지점
// 오프셋" 추적기. CategoryB 에디터에서 Enter 시 SelectMessageBlock(전체~캐럿)
// 대신 EM_SETSEL(last_offset, caret)로 새로 입력한 부분만 선택한다.
// B-6b (design 192100 §2.3): 진입 판정은 클래스명 화이트리스트가 아니라
// 능력 프로브(ProbeEmCapability/ClassifyEmProbe)다. EM_*를 처리하지 않는
// 컨트롤(브라우저/Electron/WinUI/VSCode)은 false를 반환하고 호출자가
// SelectMessageBlock으로 폴백한다. 클래스명은 DIAG 속성 로그로만 기록되며
// 판정에는 절대 사용되지 않는다(미래의 신형 EM 처리 컨트롤 자동 포용).

// ---- REQ-027 B-6b: capability-based EM detection (design 192100 §2.3) ----
//
// PROBE CONTRACT: ProbeEmCapability is READ-ONLY (EM_GETSEL / EM_GETLIMITTEXT /
// WM_GETTEXTLENGTH / EM_GETLINECOUNT / EM_LINEFROMCHAR only - EM_SETSEL is used
// exclusively by the real selection step, never the probe) and its verdict is
// decided WITHOUT window class names. Every message goes through the SendEm
// wrapper (SendMessageTimeoutW, 100 ms cap, SMTO_ABORTIFHUNG) so a hung target
// cannot stall the worker (design §2.5.A2 deadlock budget).
enum class EmCapability { Capable, NotCapable, Unknown };

// Raw observations of one ProbeEmCapability run. Plain data so the decision
// core below is pure (zero Win32 contact) and unit-testable headlessly -
// the same seam pattern as EditCaretTracker_EstimateNextOffset.
struct EmProbeSignals {
    bool getsel_handled = false; // EM_GETSEL answered within the timeout
    DWORD sel_start = 0;         // LOWORD of the EM_GETSEL reply
    DWORD sel_end = 0;           // HIWORD of the EM_GETSEL reply
    bool limit_ok = false;       // EM_GETLIMITTEXT answered
    ULONG_PTR limittext = 0;
    bool len_ok = false;         // WM_GETTEXTLENGTH answered
    ULONG_PTR textlen = 0;
    bool count_ok = false;       // EM_GETLINECOUNT answered
    ULONG_PTR linecount = 0;
    bool linefromchar_ok = false; // EM_LINEFROMCHAR(-1) answered
};

// Pure decision core behind ProbeEmCapability: design 192100 §2.3 skeleton +
// VP-approved DefWindowProc false-positive hardening (ruling 260907 21:55).
// SendMessageTimeoutW reports SUCCESS even when DefWindowProc answers 0 to an
// UNHANDLED message, so call success alone is not evidence of EM_* handling.
// In the ambiguous (0,0) EM_GETSEL case, Capable requires positive evidence
// from the EM line model: a real EDIT/RichEdit reports linecount >= 1 even
// for an empty document, while a generic window leaves EM_GETLINECOUNT to
// DefWindowProc (reply 0) and is structurally NotCapable. Unknown = partial
// or inconclusive evidence; both Unknown and NotCapable make callers fall
// back to SelectMessageBlock (only the DIAG code differs: /006 vs /007).
constexpr EmCapability ClassifyEmProbe(const EmProbeSignals& s) {
    if (!s.getsel_handled) {
        return EmCapability::NotCapable; // EM_GETSEL timed out: EM path unusable
    }
    if (s.sel_start != 0 || s.sel_end != 0) {
        return EmCapability::Capable;    // meaningful selection/caret: EM_GETSEL handled
    }
    const bool em_line_model = s.count_ok && s.linecount >= 1;
    if (s.len_ok && s.textlen > 0) {
        // Caret at document start in a NON-empty document (e.g. Home pressed):
        // proceed only when the rest of the EM family answers consistently.
        return (em_line_model && s.linefromchar_ok) ? EmCapability::Capable
                                                    : EmCapability::Unknown;
    }
    if (s.len_ok) {
        // Empty document: (0,0) is the only valid caret. A real empty editor
        // has linecount >= 1; a generic text-less window answers 0/DefWindowProc
        // -> the false positive the 21:55 ruling closed (was Capable in §2.3).
        return em_line_model ? EmCapability::Capable : EmCapability::NotCapable;
    }
    // Length query itself silent: at most partial evidence -> conservative fallback.
    return (em_line_model || s.limit_ok) ? EmCapability::Unknown : EmCapability::NotCapable;
}

// hwnd: 포그라운드 최상위 창 (hook이 캡처한 target_hwnd).
// 반환: true = 오프셋 선택 성공(EM_SETSEL 적용됨, 이후 Ctrl+C 진행),
//       false = 폴백 필요(능력 없음 Unknown/NotCapable·교착/실패).
bool EditCaretTracker_TrySelectNewText(HWND hwnd);

// 치환 성공 후 호출 — 직전 번역 지점 오프셋을 갱신한다.
// pasted=true이고 hwnd가 추적 중이면 EM_GETSEL 재조회 우선, 실패 시
// last + pasted_cch 추정치로 갱신. pasted=false면 갱신하지 않는다.
//
// B-6a 호출 시점 계약 (design 210000_architect §2.2 수정안 (a)): 개행이 주입되는
// 경로(CategoryB/inject_enter && h1_ok)에서는 반드시 SendEnterKey + 아래 settle
// 폴 이후에 호출해야 한다. 저장되는 오프셋은 다음 Enter의 EM_SETSEL 시작점이라,
// 개행 전 캐럿을 저장하면 선행 CRLF(UTF-16 2유닛)가 선택에 흡수되고 치환으로
// 소멸한다(ISSUE-1 줄병합). 개행 미주입 경로는 치환 직후 호출이 정확하다(캐럿이
// 이미 치환 끝).
void EditCaretTracker_NotifyReplacement(HWND hwnd, bool pasted, size_t pasted_cch);

// ---- REQ-027 B-6a: post-newline settle (ISSUE-1 line-merge fix) -----------
// SendEnterKey is an async SendInput (down + 35ms hold + up); the target may
// not have written the newline when the worker re-samples the caret. Spec
// (design §2.2 mandatory companion): fixed kNewlineSettleMs wait, then an
// EM_GETSEL visibility poll at kNewlineSettlePollMs intervals, max
// kNewlineSettlePollMax times (poll budget stays inside the §2.5.A2 deadlock
// bound). Exhaustion only logs EditCaretTracker/008 and proceeds: NO +2
// compensation (that is rejected variant (b) - single-LF controls would be
// over-stored); the next Enter's clamp (EditCaretTracker/004) is the last
// safety net ("settle failure allowed", design §2.2).
inline constexpr uint32_t kNewlineSettleMs = 50;
inline constexpr uint32_t kNewlineSettlePollMs = 25;
inline constexpr int kNewlineSettlePollMax = 4;

// Sentinel for a caret sample that could not be taken. EM_GETSEL saturates at
// 65535 (WORD packing), so UINT32_MAX can never collide with a real offset.
inline constexpr DWORD kEditCaretUnknown = UINT32_MAX;

// Read-only caret (selection-end) probe through the same gates as
// NotifyReplacement: focus candidate resolution + EM_* capability probe
// (B-6b: ClassifyEmProbe verdict, class name irrelevant) + EM_GETSEL within
// the 100ms deadlock budget. Returns kEditCaretUnknown when any gate fails.
// Used by the worker to capture the pre-newline caret for the settle
// comparison below.
DWORD EditCaretTracker_SampleCaret(HWND hwnd);

// Block (bounded) until the caret on hwnd moves off pre_newline_caret - proof
// the injected newline is visible to EM_GETSEL - or the settle budget is
// spent. No-op besides the fixed wait when pre_newline_caret is
// kEditCaretUnknown. Call between SendEnterKey and NotifyReplacement.
void EditCaretTracker_SettleNewlineVisible(HWND hwnd, DWORD pre_newline_caret);

// REQ-027 headless test seam (design §3 B-4 item 4): the pure part of the
// NotifyReplacement offset-update rule - the EM_GETSEL-requery-failure
// estimate "last + pasted_cch", saturating at UINT32_MAX (the state map
// stores UTF-16 code-unit offsets in a DWORD). No Win32 contact, so the
// arithmetic is unit-testable without a live editor (TestReq027CaretTracker).
inline constexpr uint32_t EditCaretTracker_EstimateNextOffset(uint32_t last, size_t pasted_cch) {
    const uint64_t sum = static_cast<uint64_t>(last) + pasted_cch;
    return sum > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(sum);
}

// ---- REQ-034 F2-B': leading-CRLF self-correction (design 260907 173700 §4.1) ----
//
// Root cause (debug 260907 173700 §F2): the stored EM_SETSEL start is only
// ever advanced by the pipeline, so out-of-band user edits (manual Shift+
// Enter, paste, backspace) drift it: the next Enter's capture begins with
// the "\r\n" the user typed at the saved boundary, and the replacement
// swallows the line break (line merge - user log emebalachat_260907171452
// L997/L1147). The primary verdict is measured, not inferred: when the
// EM-path CAPTURE starts with a leading CRLF, the stored start pointed at a
// newline - self-correct by re-selecting from 0.
//
// Pure seam (same pattern as ClassifyEmProbe/EstimateNextOffset): narrow by
// design - the CRLF pair only. A lone '\r'/'\n' at index 0 is normal block
// content in single-LF controls and must NOT trigger a re-capture (that is
// the REQ-027 normal-progress contract; TestReq034NoLeadingCrlfNormalProgress
// pins it). Widening is deferred to the D-4 per-app matrix measurement.
inline constexpr bool EditCaretTracker_HasLeadingCrlf(std::wstring_view captured) {
    return captured.size() >= 2 && captured[0] == L'\r' && captured[1] == L'\n';
}

// REQ-036 (docs/260907_0001 session, log emebalachat_260907200313 L247-264/
// L322-326/L437-448): count the CRLF pairs that prefix an EM-path capture.
// A pair proves a hook pass-through Enter inserted a block terminator the
// stored offset never advanced over (out-of-band newline). Unlike the
// HasLeadingCrlf predicate above (single pair, reselect trigger), this counts
// EVERY consecutive pair: each pass-through Enter that terminated a block
// between the stored offset and the caret contributed one pair, and each one
// must push the effective selection start forward by that newline's
// DOCUMENT width - 1 or 2 UTF-16 units, MEASURED via DocNewlineWidth below
// (REQ-F1: the clipboard's 2-unit pair is a normalization artifact, never
// the document's storage width).
// Pure seam: no Win32 contact, unit-testable headlessly.
inline constexpr size_t EditCaretTracker_CountLeadingCrlfPairs(std::wstring_view captured) {
    size_t n = 0;
    while (captured.size() >= 2 * (n + 1) &&
           captured[2 * n] == L'\r' && captured[2 * n + 1] == L'\n') {
        ++n;
    }
    return n;
}

// ---- REQ-F1 (docs/260908_0001 session): clipboard-vs-document newline width ----
//
// Root cause (user log emebalachat_260908062830 L546-550/L787-791/L1120-1124):
// the compensation below advanced the stored start by 2 UTF-16 units per
// leading CRLF pair, but the clipboard NORMALIZES every newline to "\r\n"
// while the document may store each newline as a single LF unit (Notepad's
// RichEditD2DPT: field arithmetic 63 captured vs 60 selected units over 3
// newlines -> 1 unit per document newline). The 2-units-per-pair advance
// overshot the true block start, the re-selection began INSIDE the block,
// and the block's first character(s) stayed outside the replacement - the
// "first char residue" ahead of the pasted translation ("오"/"처"/"왜 ").
// The width must be MEASURED from the capture-vs-selection arithmetic,
// never assumed.
//
// Pure seam: count the newline SEQUENCES in a capture. A "\r\n" pair, a
// lone "\n", and a lone "\r" each count as ONE sequence. No Win32 contact.
inline constexpr size_t EditCaretTracker_CountNewlineSequences(std::wstring_view captured) {
    size_t n = 0;
    for (size_t i = 0; i < captured.size(); ++i) {
        if (captured[i] == L'\n') {
            ++n;
        } else if (captured[i] == L'\r' &&
                   (i + 1 >= captured.size() || captured[i + 1] != L'\n')) {
            ++n;
        }
    }
    return n;
}

// REQ-F1 pure seam: back-compute the DOCUMENT's UTF-16 newline width from
// the capture-vs-selection arithmetic. The capture is the CLIPBOARD text
// (every newline CRLF-normalized, 2 units); the selection span is the same
// range's DOCUMENT unit count. For a uniform-width document:
//   captured == sel_span            -> every document newline is CRLF (width 2)
//   captured == sel_span + sequences-> every document newline is LF (width 1)
// Any other relation is a mixed-width document or a non-normalized capture
// whose leading-newline widths arithmetic cannot recover: return 0 and the
// caller must keep the original capture (refuse - never guess a width; the
// hardcoded-width guess is exactly the F1 defect).
inline constexpr size_t EditCaretTracker_DocNewlineWidth(size_t captured_units,
                                                         size_t sel_span_units,
                                                         size_t newline_sequences) {
    if (newline_sequences == 0 || captured_units < sel_span_units) {
        return 0;
    }
    if (captured_units == sel_span_units) {
        return 2;
    }
    if (captured_units == sel_span_units + newline_sequences) {
        return 1;
    }
    return 0;
}

// REQ-036 surgical FIX-1 worker-sent-newline notification: call AFTER the
// worker's own SendEnterKey when the task ends WITHOUT a paste (IME backstop,
// paste-window send-through, empty/bypass send-through - the full enumeration
// is in the debug-surgical report). The Enter this process just sent inserted
// the CURRENT block's terminator in the document; the stored offset must
// advance past it or the NEXT Enter's capture begins with the CRLF pair that
// splits the two out-of-band block boundaries. pre_newline_caret is sampled
// by the caller BEFORE SendEnterKey (the B-6a pre-sample pattern). Settles
// (same budget as the paste path), then re-queries EM_GETSEL and stores the
// measured caret - measurement, not a +2 assumption, so single-LF controls
// store correctly too. No-op when the hwnd is untracked or any EM gate fails
// (the stored offset is then left alone; FIX-2 compensates at capture time).
void EditCaretTracker_NotifySentNewline(HWND hwnd, DWORD pre_newline_caret);

// REQ-036 surgical FIX-2 data-driven compensation, width-corrected by
// REQ-F1: after an EM-path capture that begins with N leading CRLF pairs
// (N >= 1), the stored start was N newlines behind the true block boundary.
// Instead of re-selecting from 0 (which grabs every PRECEDING already-
// translated block - the whole-document retranslation defect of REQ-036)
// or re-selecting and re-copying at all (extra clipboard round-trip whose
// re-copy can only reproduce bytes we already hold), this advances the
// stored offset by exactly N * width units, where width is the document's
// MEASURED UTF-16 newline width back-computed from the capture text vs the
// live selection span (DocNewlineWidth above - the clipboard normalizes
// newlines to CRLF, but the document may store single-unit LFs; the old
// hardcoded 2-units-per-pair advance overshot LF documents and left the
// block's first character(s) outside the replacement - the F1 residue).
// The capture text is the second argument: the function recounts the
// leading pairs from it, so caller and callee can never disagree. When
// the width is not recoverable (mixed-width document, non-normalized
// capture, or capture shorter than the selection span) the function
// refuses and changes NOTHING: the caller keeps the original capture and
// its fallback budget. The caller then strips the leading pairs from the
// capture text by their 2*N CLIPBOARD units (the newline now sits OUTSIDE
// the replacement, so the block boundary survives) and the pipeline
// proceeds with the corrected text directly. Same gate discipline as
// TrySelfCorrectReSelect (focus resolution + capability probe + stored-
// entry-ahead check + stale clamp); returns false when the hwnd is
// unusable, the entry is missing/already 0, the width is unrecoverable, or
// the advanced start would exceed the measured caret.
bool EditCaretTracker_CompensateLeadingNewlines(HWND hwnd, std::wstring_view captured);

// REQ-039: read-only probe of the CURRENT EM selection range on the focus
// candidate resolved for hwnd. True only when the control is EM-capable AND
// the probe successfully reads an exactly-empty range (start == end >= 0) -
// the REQ-034 F3-B paste-window geometry where a subsequent Ctrl+C
// legitimately leaves the clipboard sequence untouched. All gates fail to
// false (untracked / non-EM / probe timeout), which simply means "retry is
// allowed": a non-EM control's copy failure is not provably legitimate, so
// the chord re-send proceeds and only the attempt budget bounds it. Pure
// read: EM_GETSEL never mutates state (same primitive ProbeEmCapability
// uses, same SendEm 100 ms deadlock bound).
bool EditCaretTracker_SelectionProvablyEmpty(HWND hwnd);

// Self-correction re-selection for the once-per-Enter retry (CopySelectedText
// drives predicate -> re-select -> re-copy). Same gates as TrySelectNewText
// (focus resolution + EM capability + SendEm 100 ms budget), then
// EM_SETSEL(0, caret) and stores start 0 + the REQ-034 baseline_textlen
// auxiliary signal. Returns false - changing nothing - when the hwnd is
// unusable/NotCapable/Unknown, any EM call fails, or the stored start is
// already 0: that is the already-safe whole-block geometry (the document
// itself begins with a newline), so re-capturing identical bytes would only
// add a clipboard round trip and a repeat-retry risk. On true the next Enter
// starts from 0 (safe whole-block) until the following NotifyReplacement
// restores real-caret progress.
bool EditCaretTracker_TrySelfCorrectReSelect(HWND hwnd);

// High-level pipeline helper:
// Classifies the window via ClassifyAppWindow(hwnd) and selects text with
// SelectAll() (CategoryA), the REQ-027 EditCaretTracker EM_SETSEL path
// (CategoryB + standard EDIT/RichEdit classes only), or SelectMessageBlock()
// (CategoryB fallback for everything else), then runs the REQ-R04
// sequence-number copy-settle wait and retrieves clipboard text.
// REQ-036 FIX-2: when the EM-path capture begins with N leading CRLF pairs,
// the stored start lagged N out-of-band block terminators behind - advance
// the stored offset AND the live selection by exactly 2*N units, trim the
// same units from the capture text, and hand the corrected text to the
// pipeline (no re-copy; data-driven from the measured capture). The
// offset-saving contracts (worker post-newline NotifyReplacement /
// NotifySentNewline) are the FIX-1 producers and stay untouched here.
// REQ-039 FIX-1: a sequence-wait failure no longer terminates the capture
// immediately. Electron/Chromium targets intermittently drop synthetic
// Ctrl+C chords (renderer-side commit never lands), so the full
// selection+copy cycle is re-run under the CopyChordRetryWarranted budget:
// all selection primitives are idempotent and each attempt re-baselines
// the sequence wait, so a late commit from an earlier chord reads as a
// confirmed copy, never stale text. The provably-empty EM selection (the
// REQ-034 F3-B paste-window geometry) is exempt (no retry, no latency).
// Returns empty when the copy could not be confirmed (never stale data).
// shift_enter_count (session 260913_0001, Reddit long-post fix): K = the
// hook-counted Shift+Enter depth of the CURRENT composition block, exactly
// the value the worker's F3 slice uses. It is consulted ONLY by the non-EM
// whole-capture slice-before-guard (SliceWholeCaptureToBlock): when the
// [0..caret) accumulation crosses the capture-size guard, the guard re-vets
// the last K+1 logical lines instead of aborting the whole capture. Default
// 0 keeps any caller without block context source-compatible (and behaves
// identically for sub-guard captures, where the slice never engages).
// capture_result (Phase B diagnostics, debug report 022121 §8-1): optional
// out-param reporting the SHAPE-ONLY verdict of the capture seam
// (EnterCaptureResult: ok / block_sliced / guard_abort / empty_tail /
// copy_chord_failed / empty_selection / editor_excluded) so the worker log
// can distinguish empty-capture causes. Never carries user content.
std::wstring CopySelectedText(HWND hwnd, int shift_enter_count = 0,
                              EnterCaptureResult* capture_result = nullptr);

// High-level pipeline helper:
// Sets translated text to clipboard, sends Ctrl+V, sleeps the minimal paste settle
// delay (kPasteSettleDelayMs, currently 120ms - see M1 note in win32_input.cpp),
// then restores the original clipboard as soon as the target app has read it.
// If expected_target is non-null, re-verifies the foreground window immediately before
// injecting Ctrl+V and aborts (returns false, no paste) when focus has shifted to a
// different window root. Prevents translated text leaking into the wrong application.
//
// F7 (session 260908_0002, log 문제2 clipboard_restored=0): when clipboard_restored
// is non-null it receives whether RestoreClipboard actually confirmed putting the
// ORIGINAL backup (text + extra formats) back before returning. A caller that
// disables its own scope-exit RAII restorer on paste success MUST gate that on this
// flag: disarming after a silently-failed restore leaves the translated text
// permanently on the user's clipboard (backup collected, then discarded).
bool PasteAndRestore(std::wstring_view text, const ClipboardBackup& backup,
                     HWND expected_target = nullptr, bool* clipboard_restored = nullptr);

// Pure foreground-equivalence check for injection gating (H1 wrong-window fix).
// - expected_target == nullptr  -> always true (no target captured; PasteAndRestore default)
// - current_foreground == nullptr -> false (cannot verify -> refuse to inject)
// - true when handles are equal OR share the same GA_ROOTOWNER (survives re-nested
//   child-window focus within the same top-level window).
bool IsSameWindowForInjection(HWND expected_target, HWND current_foreground);

// Sends synthetic Enter key event with modifier release and 35ms hold time.
// release_shift is accepted for call-site clarity (workers pass
// task.is_shift_enter) but intentionally unused: all modifiers are probed
// via GetAsyncKeyState and released unconditionally. Reserved for the
// Phase 5 editor-mode redesign (REQ-023 newline injection).
void SendEnterKey(bool release_shift = false);

// ---- REQ-R17 (audit §5 latent item 5): IME composition state probe ----
//
// True when the CURRENT foreground window has an active, non-empty IME
// composition (Korean jamo being assembled, Japanese conversion candidate
// open). ImmGetContext() SendMessage's the target window's thread, so this
// is WORKER-THREAD / UI-THREAD ONLY: calling it from a low-level hook would
// reintroduce the REQ-R06 LowLevelHooksTimeout stall class (the hook uses the
// O(1) local mirror in hook.hpp instead). Fails closed to false when there
// is no window / no IME / the query cannot tell. ExecuteTask consults it as
// the race-window backstop before touching the clipboard pipeline.
bool ForegroundImeComposing();

// ---- W1 (session 260910_0007) internal test seams --------------------------
//
// Not application API: headless verification hooks for the edit_caret state
// map's dead-entry purge (W1/A3 erase_if refactor). Declared here so
// run_tests.cpp can drive PurgeDeadEntriesLocked directly; the production
// callers keep their mutex-held internal use. Same discipline as the REQ-027
// pure seams above (test-only, documented, zero app call sites).
namespace edit_caret {

// Insert (or overwrite) one map entry under g_mutex. Test helper ONLY.
bool TestInsertEntry(HWND focus_hwnd, DWORD pid, DWORD offset);
// True when the exact composite key {focus_hwnd, pid} is present. Test only.
bool TestHasEntry(HWND focus_hwnd, DWORD pid);
// Lock g_mutex and run the production purge (std::erase_if path). Test only.
void TestPurgeDeadEntries();

} // namespace edit_caret

} // namespace emebalachat

