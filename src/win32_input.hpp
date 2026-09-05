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

// Selects the current line from cursor back to beginning of line (Shift+Home).
bool SelectCurrentLine();

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

// Returns true if process owning hwnd is a known chat application (KakaoTalk, Discord, Slack, etc.)
bool IsChatApplicationWindow(HWND hwnd);

// Sends Ctrl+A to select all text in the active control.
bool SelectAll();

// Process-aware text selection: Ctrl+A for chat apps, Shift+Home for document editors.
bool SelectTextForTranslation(HWND hwnd);

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

// High-level pipeline helper:
// Selects text via SelectTextForTranslation(hwnd), then runs the REQ-R04
// sequence-number copy-settle wait and retrieves clipboard text.
// Returns empty when the copy could not be confirmed (never stale data).
std::wstring CopySelectedText(HWND hwnd);

// High-level pipeline helper:
// Selects line (Shift+Home) and runs the same REQ-R04 copy-settle as
// CopySelectedText.
std::wstring CopySelectedLine();

// High-level pipeline helper:
// Sets translated text to clipboard, sends Ctrl+V, sleeps the minimal paste settle
// delay (kPasteSettleDelayMs, currently 120ms - see M1 note in win32_input.cpp),
// then restores the original clipboard as soon as the target app has read it.
// If expected_target is non-null, re-verifies the foreground window immediately before
// injecting Ctrl+V and aborts (returns false, no paste) when focus has shifted to a
// different window root. Prevents translated text leaking into the wrong application.
bool PasteAndRestore(std::wstring_view text, const ClipboardBackup& backup, HWND expected_target = nullptr);

// Pure foreground-equivalence check for injection gating (H1 wrong-window fix).
// - expected_target == nullptr  -> always true (no target captured, e.g. CopySelectedLine path)
// - current_foreground == nullptr -> false (cannot verify -> refuse to inject)
// - true when handles are equal OR share the same GA_ROOTOWNER (survives re-nested
//   child-window focus within the same top-level window).
bool IsSameWindowForInjection(HWND expected_target, HWND current_foreground);

// Sends synthetic Enter key event with modifier release and 35ms hold time.
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

