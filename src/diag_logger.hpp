#pragma once

// ---------------------------------------------------------------------------
// diag_logger — comprehensive file-based diagnostic logging (session 260905).
//
// Purpose: give the user a complete forensic trail of every keystroke, every
// Enter-path gate decision, every pipeline stage and every UI transition so
// the live bugs can be reproduced and attributed from a single log file.
// The VP/user explicitly authorized keystroke CONTENT logging for this
// diagnostic build (overrides the previous shape-only rule for this feature).
//
// Design contract (must hold — see the .cpp for the implementation):
//   * ONE file per app run:
//       %LOCALAPPDATA%\Emebalachat\logs\emebalachat_yymmddhhmmss.log
//     (local time in the FILENAME per the user's requested format; collision
//     suffix -2/-3 if two runs start in the same second). Directory is
//     created on demand. On any failure the logger falls back to
//     <exe-dir>\logs, and if that fails too it disables itself gracefully —
//     it NEVER crashes the host application.
//   * Line format:
//       yyyy-mm-dd hh:mm:ss.mmm [tid] TAG/message
//     Timestamp and thread id are captured at ENQUEUE time, so hook / worker /
//     GUI thread activity is distinguishable and ordered by true occurrence.
//   * Thread safety: internal std::mutex + std::deque + dedicated flush
//     thread. Callers (including the WH_KEYBOARD_LL hook thread) only format
//     and enqueue — microseconds, no file I/O, no waits. Bounded queue:
//     overflow drops OLDEST lines and records a dropped-count note.
//   * Never throws. All Win32/STL failures degrade to disabled/ignored.
//   * diag::SetEnabled(false) silences the FILE sink (runtime toggle, default
//     ON for this diagnostic build; nothing persisted to config).
//
// Two families of entry points:
//   DIAG_LOG("TAG", fmt, ...)  — new, explicit-tag lines (keystrokes, gates,
//                                pipeline stages, UI events, session records).
//   DIAG_F(fmt, ...)           — drop-in replacement for the existing
//                                fprintf(stderr, "MODULE/site/NNN: ...\n")
//                                sites: ONE implementation, TWO sinks. The
//                                stderr output stays byte-identical for
//                                compatibility; the same formatted text is
//                                mirrored to the file with the MODULE/site/NNN
//                                code parsed out as the TAG. When the logger
//                                is uninitialized/disabled, DIAG_F still
//                                behaves exactly like fprintf(stderr, ...).
// ---------------------------------------------------------------------------

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace diag {

// Initializes the logger: resolves the log directory (dir_override is only
// used by the headless tests), creates <dir>\emebalachat_yymmddhhmmss.log
// (binary append, collision-suffixed), and starts the flush thread.
// Returns true when the file sink is live. Idempotent: calling Init() again
// while already initialized is a no-op returning the previous result.
// Never throws; on total failure returns false and every Log call no-ops.
bool Init(const std::filesystem::path& dir_override = {});

// Stops accepting lines, drains + flushes the queue, joins the flush thread
// and closes the file. Safe to call when uninitialized (no-op) and safe to
// call twice. After Shutdown(), Init() may be called again (tests rely on
// this re-init cycle).
void Shutdown();

// Runtime toggle for the FILE sink (default: enabled once Init() succeeded).
// Does not affect DIAG_F's stderr output. Not persisted anywhere.
void SetEnabled(bool enabled);
bool IsEnabled();

// True between a successful Init() and Shutdown().
bool IsInitialized();

// Full path of the active log file (empty when uninitialized).
std::wstring CurrentLogPath();

// Number of lines dropped so far because the bounded queue overflowed.
uint64_t DroppedCount();

// Blocks until every enqueued line is written AND flushed to disk. Used by
// tests and by app shutdown. Returns immediately when uninitialized.
void Flush();

// printf-style, explicit TAG. Safe from any thread including the LL hook
// (formats + enqueues only). Silently no-ops when disabled/uninitialized.
void Printf(std::string_view tag, const char* fmt, ...);

// Dual-sink mirror of fprintf(stderr, ...): always writes to stderr, and —
// when the logger is live and enabled — enqueues the same text to the file
// with the leading "MODULE/site/NNN:" token parsed out as the TAG.
void MirrorF(const char* fmt, ...);

} // namespace diag

#define DIAG_LOG(tag, ...) ::diag::Printf((tag), __VA_ARGS__)
#define DIAG_F(...) ::diag::MirrorF(__VA_ARGS__)
