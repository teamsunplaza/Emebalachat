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

// High-level pipeline helper:
// Selects text via SelectTextForTranslation(hwnd), sends Ctrl+C, sleeps 35ms, retrieves clipboard text.
std::wstring CopySelectedText(HWND hwnd);

// High-level pipeline helper:
// Selects line (Shift+Home), sends Ctrl+C, sleeps 35ms copy settle delay, retrieves clipboard text.
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

} // namespace emebalachat

