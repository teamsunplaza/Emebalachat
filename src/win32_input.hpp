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
// REQ-027 (Phase A §A-2): 표준 EDIT/RichEdit 컨트롤 한정 "직전 번역 지점
// 오프셋" 추적기. CategoryB 에디터에서 Enter 시 SelectMessageBlock(전체~캐럿)
// 대신 EM_SETSEL(last_offset, caret)로 새로 입력한 부분만 선택한다.
// 비표준 컨트롤(브라우저/Electron/WinUI/VSCode)은 EM_* 미처리이므로 false를
// 반환하고 호출자가 SelectMessageBlock으로 폴백한다.

// hwnd: 포그라운드 최상위 창 (hook이 캡처한 target_hwnd).
// 반환: true = 오프셋 선택 성공(EM_SETSEL 적용됨, 이후 Ctrl+C 진행),
//       false = 폴백 필요(비표준/교착/실패).
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
// NotifyReplacement: focus candidate resolution + standard EDIT/RichEdit
// class + EM_GETSEL within the 100ms deadlock budget. Returns
// kEditCaretUnknown when any gate fails. Used by the worker to capture the
// pre-newline caret for the settle comparison below.
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

// High-level pipeline helper:
// Classifies the window via ClassifyAppWindow(hwnd) and selects text with
// SelectAll() (CategoryA), the REQ-027 EditCaretTracker EM_SETSEL path
// (CategoryB + standard EDIT/RichEdit classes only), or SelectMessageBlock()
// (CategoryB fallback for everything else), then runs the REQ-R04
// sequence-number copy-settle wait and retrieves clipboard text.
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

