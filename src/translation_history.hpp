#pragma once

// V10F L2-B (261122_0002 P2-G, v0.10.10 phased fixes): the in-memory
// translation HISTORY ring. Every completed translation pushes one Entry so
// the tooltip's [edit/retranslate] surface (unit P2-C) can reopen the result
// in the translation editor window (unit P2-G,
// src/ui/translation_editor_window.*) for correction and re-translation.
//
// Contract:
//   * IN-MEMORY ONLY. This module never touches disk, the registry, or the
//     diag log: the history holds up to 10 full user texts (original,
//     forward, back-translation), which are exactly the content class the
//     privacy model forbids persisting or logging (diag_log_content=false
//     is the default). Do NOT add persistence here without a user decision.
//   * Mutex-guarded: Push may be called from the pipeline worker thread and
//     Recent from the GUI thread (the editor window), so both serialize on
//     one internal mutex.
//   * Newest first: index 0 of Recent() is the most recent entry; the ring
//     keeps at most kMaxHistoryEntries and silently drops the oldest.

#include <cstddef>
#include <string>
#include <vector>

#include <windows.h>

namespace emebalachat {
namespace translation_history {

// The ring capacity (L2-B brief). Pinned by tests/p2_g_tests.inc.
inline constexpr std::size_t kMaxHistoryEntries = 10;

struct Entry {
    std::wstring original;
    std::wstring forward;   // translated text that replaced the original
    std::wstring back;      // back-translation shown for verification
    std::wstring src_code;  // normalized source language code ("KO", "EN", ...)
    std::wstring tgt_code;  // normalized target language code
    HWND target_hwnd = nullptr; // the window the paste landed in (0 = unknown)
    // V10F-R1 F-09 / V10F-R2 F-27: the PID that owned target_hwnd at Push
    // time. HWNDs are recycled by the OS, so the editor's
    // [ApplyToOriginalWindow] verifies the handle against this PID before
    // AND after the activation wait. The ring records it centrally at Push
    // for every entry (the ring is in-memory only, so every entry is written
    // by this build); 0 with a non-null target_hwnd means the handle was
    // already dead/recycled at capture, and the editor REFUSES the apply —
    // there is no legitimate unverifiable entry left to skip the check for.
    DWORD target_pid = 0;
};

// Pushes `entry` at the FRONT of the ring, dropping the oldest entry when the
// ring is full. The evicted entry's text buffers are zeroized before free
// (V10F-R1 F-09: the ring holds full user content), and the entry's
// target_pid is filled in here when the producer left it 0 and carried a
// target_hwnd (the editor's HWND-recycling verification reads it; see the
// Entry contract — an unrecordable PID refuses the apply).
// Mutex-guarded; safe from any thread. In-memory only.
void Push(Entry entry);

// V10F-R1 F-09: zeroizes every buffered text in the ring and empties it (the
// shutdown-sweep teardown path). Mutex-guarded; safe from any thread.
void Clear();

// A snapshot copy of the ring, newest first. Mutex-guarded; the returned
// vector is detached from the internal state, so mutating it never affects
// the ring.
std::vector<Entry> Recent();

} // namespace translation_history
} // namespace emebalachat
