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

// Phase 5 (REQ-011): keyboard-pipeline app category. CategoryA = chat/command
// apps where Enter means "send/execute" (KakaoTalk, Slack, Discord, VSCode,
// terminals-as-command-pipelines, etc.): the whole input is selected (Ctrl+A),
// replaced, and the synthetic Enter sends it. CategoryB = editor-type apps
// (Notepad, IDE text editors, browser text areas) where the caret-line/block
// context is replaced and a real newline is always injected afterwards
// (REQ-012/023). Classification is a pure static exe-name table lookup;
// anything not in the CategoryA table is CategoryB (fail-open to the editor
// path, which is the pre-Phase-5 default for all non-chat apps).
enum class AppCategory : unsigned char { CategoryA, CategoryB };

// Phase 5 (REQ-011): classifies the process owning hwnd into the keyboard
// pipeline category. Same process-name resolution as the old
// IsChatApplicationWindow (QueryFullProcessImageNameW -> basename ->
// case-insensitive compare), but returns the category instead of a bool.
// Fails closed to CategoryB on any resolution failure (null hwnd, bad pid,
// OpenProcess/Query failure): the editor path is the safe default because it
// never sends the message on the user's behalf beyond a newline.
AppCategory ClassifyAppWindow(HWND hwnd);

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
inline constexpr uint32_t kClipboardChangeTimeoutMs = 180;
// The sequence must hold this long after the last observed change before reading.
inline constexpr uint32_t kClipboardStableWindowMs = 16;
// Poll cadence for the real-clipboard driver (delegation: 5-10 ms).
inline constexpr uint32_t kClipboardPollIntervalMs = 8;
// Hard wall-clock deadline for the whole wait (change detection + settling).
inline constexpr uint64_t kClipboardCopyDeadlineMs =
    kClipboardChangeTimeoutMs + kClipboardStableWindowMs;

enum class ClipboardCopyOutcome { Pending, Confirmed, Failed };

// Pure, time-parameterized state machine behind CopySelectionWithSequenceWait().
// Deliberately free of Win32 calls so the complete timeline matrix (including
// the two-step EmptyClipboard->SetClipboardData race) is unit-testable
// headlessly with synthetic timestamps; see TestClipboardSequencePolling().
class ClipboardCopyWatcher {
public:
    ClipboardCopyWatcher(uint32_t pre_copy_seq, uint64_t start_ms)
        : pre_seq_(pre_copy_seq), start_ms_(start_ms), last_seq_(pre_copy_seq) {}

    // Feed the clipboard sequence number observed at now_ms (same clock epoch
    // as start_ms; now_ms must be monotonically non-decreasing).
    ClipboardCopyOutcome Update(uint32_t current_seq, uint64_t now_ms);

    ClipboardCopyOutcome Outcome() const { return outcome_; }

private:
    uint32_t pre_seq_;
    uint64_t start_ms_;
    uint32_t last_seq_;
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
bool CopySelectionWithSequenceWait();

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
// must push the effective selection start forward by exactly 2 UTF-16 units.
// Pure seam: no Win32 contact, unit-testable headlessly.
inline constexpr size_t EditCaretTracker_CountLeadingCrlfPairs(std::wstring_view captured) {
    size_t n = 0;
    while (captured.size() >= 2 * (n + 1) &&
           captured[2 * n] == L'\r' && captured[2 * n + 1] == L'\n') {
        ++n;
    }
    return n;
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

// REQ-036 surgical FIX-2 data-driven compensation: after an EM-path capture
// that begins with N leading CRLF pairs (N >= 1), the stored start was N
// newlines behind the true block boundary. Instead of re-selecting from 0
// (which grabs every PRECEDING already-translated block - the whole-document
// retranslation defect of REQ-036) or re-selecting and re-copying at all
// (extra clipboard round-trip whose re-copy can only reproduce bytes we
// already hold), this advances the stored offset by exactly 2*N UTF-16
// units: the measured, structural size of the N block terminators the
// capture itself proves exist. The caller then strips those leading pairs
// from the capture text (the newline now sits OUTSIDE the replacement, so
// the block boundary survives) and the pipeline proceeds with the corrected
// text directly. Same gate discipline as TrySelfCorrectReSelect (focus
// resolution + capability probe + stored-entry-is-ahead check); returns
// false (changing nothing) when the hwnd is unusable, the entry is
// missing/already 0/staler than the caret, or the EM_SETSEL-less design
// needs the caller to fall back to conservative behavior.
bool EditCaretTracker_CompensateLeadingNewlines(HWND hwnd, size_t pair_count);

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
// Returns empty when the copy could not be confirmed (never stale data).
std::wstring CopySelectedText(HWND hwnd);

// High-level pipeline helper:
// Sets translated text to clipboard, sends Ctrl+V, sleeps the minimal paste settle
// delay (kPasteSettleDelayMs, currently 120ms - see M1 note in win32_input.cpp),
// then restores the original clipboard as soon as the target app has read it.
// If expected_target is non-null, re-verifies the foreground window immediately before
// injecting Ctrl+V and aborts (returns false, no paste) when focus has shifted to a
// different window root. Prevents translated text leaking into the wrong application.
bool PasteAndRestore(std::wstring_view text, const ClipboardBackup& backup, HWND expected_target = nullptr);

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

} // namespace emebalachat

