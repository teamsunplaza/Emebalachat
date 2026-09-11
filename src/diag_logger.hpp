#pragma once

// ---------------------------------------------------------------------------
// diag_logger — comprehensive file-based diagnostic logging (session 260905).
//
// Purpose: give the user a complete forensic trail of every keystroke, every
// Enter-path gate decision, every pipeline stage and every UI transition so
// the live bugs can be reproduced and attributed from a single log file.
// RELEASE POSTURE (REQ-003, session 260909): user-CONTENT logging (typed
// chars, foreground-window titles, captured bodies, translation output) is
// OFF by default and strictly opt-in via the AppConfig field
// "diag_log_content" (config.json, applied once at startup through
// SetContentLogging below). With the flag off, the PII call sites record
// shape-only metadata (key codes, modifier flags, window class, lengths,
// timings) and omit every content field. Enabling it is a deliberate,
// user-initiated troubleshooting action that re-exposes content in the log
// file; restart is required after changing the field.
// REQ-201/202 (session 260911_0002): the FILE sink itself is now OFF by
// default and strictly opt-in via the AppConfig field "diag_log_enabled"
// (config.json, applied once at startup through SetEnabled below). Init() no
// longer opens (or even creates) the log file: it only resolves the directory
// and starts the flush thread. The FIRST SetEnabled(true) transition opens
// the file lazily and injects the two SESSION header lines; an open failure
// leaves lines cheaply queued and the flush worker retries per batch. An
// opted-out user gets zero log files: not one .log is ever created, no
// SESSION header, no rotation churn (the empty logs DIRECTORY may exist
// from an earlier enabled run — it holds no files this run). When enabled,
// every pre-existing file-sink behavior is intact (REQ-202).
//
// Design contract (must hold — see the .cpp for the implementation):
//   * ONE file per app run, opened lazily on the first enabled log line:
//       %LOCALAPPDATA%\Emebalachat\logs\emebalachat_yymmddhhmmss.log
//     (local time in the FILENAME per the user's requested format; collision
//     suffix -2/-3 if two runs start in the same second). Directory is
//     created on demand. On any failure the logger falls back to
//     <exe-dir>\logs, and if that fails too it disables itself gracefully —
//     it NEVER crashes the host application.
//   * 200MB cap (REQ-203): PruneLogs() runs ONCE at app start (before any
//     file is opened, regardless of the opt-in state) and deletes the oldest
//     emebalachat_*.log files until the directory total is at or below
//     kLogDirCapBytes. Deletion failures are swallowed per file (OneDrive/
//     antivirus locks must never block startup).
//   * Line format:
//       yyyy-mm-dd hh:mm:ss.mmm [tid] TAG/message
//     Timestamp and thread id are captured at ENQUEUE time, so hook / worker /
//     GUI thread activity is distinguishable and ordered by true occurrence.
//   * Thread safety: internal std::mutex + std::deque + dedicated flush
//     thread. Callers (including the WH_KEYBOARD_LL hook thread) only format
//     and enqueue — microseconds, no file I/O, no waits. Bounded queue:
//     overflow drops OLDEST lines and records a dropped-count note.
//   * Never throws. All Win32/STL failures degrade to disabled/ignored.
//   * diag::SetEnabled(false) silences the FILE sink (runtime toggle;
//     default OFF since REQ-201, persisted as "diag_log_enabled").
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

// Total on-disk cap for the whole logs directory (REQ-203: "로그회전/상한은
// 200MB"). Enforced by PruneLogs() at startup and again right before every
// lazy file open (belt-and-suspenders: a single enabled run can outlive the
// cap without ever restarting).
inline constexpr uint64_t kLogDirCapBytes = 200ull * 1024ull * 1024ull;

// Initializes the logger: resolves the log directory (dir_override is only
// used by the headless tests) and starts the flush thread. Does NOT open or
// create the log file (REQ-201 lazy open): that happens at the first log
// line enqueued while the sink is enabled (see SetEnabled). Returns true
// when the directory resolved and the worker is live — false only when even
// the fallback dir is unusable. Idempotent: calling Init() again while
// already initialized is a no-op returning the previous result. Never
// throws; on total failure every Log call no-ops.
bool Init(const std::filesystem::path& dir_override = {});

// Stops accepting lines, drains + flushes the queue, joins the flush thread
// and closes the file (if it was ever opened). Safe to call when
// uninitialized (no-op) and safe to call twice. After Shutdown(), Init() may
// be called again (tests rely on this re-init cycle).
void Shutdown();

// Runtime toggle for the FILE sink. Default FALSE (REQ-201): main.cpp applies
// AppConfig::diag_log_enabled exactly once here, right after the config load
// and before any hook/worker thread exists — the same startup-only pattern
// as SetContentLogging below; a config change takes effect on restart.
// The FIRST SetEnabled(true) transition opens the log file lazily (pruning
// the directory to kLogDirCapBytes first, then writing the SESSION header
// lines). An open failure degrades to "sink enabled, file never created":
// lines stay cheaply queued and the next SetEnabled(true) transition retries.
// Does not affect DIAG_F's stderr output (unconditional debug channel).
void SetEnabled(bool enabled);
bool IsEnabled();

// Removes the oldest emebalachat_*.log files in `dir` until the directory
// total is at or below cap_bytes (default kLogDirCapBytes). Best-effort: a
// file that cannot be stat'ed or deleted (locked, vanished) is skipped, and
// the function never throws. Returns the number of files deleted. main.cpp
// calls it once at startup against DefaultLogDir() REGARDLESS of the opt-in
// state (REQ-203: "enabled와 무관하게 항상 실행"); the lazy file-open calls
// it again with the same default. Exposed for the headless prune unit test.
uint64_t PruneLogs(const std::filesystem::path& dir,
                   uint64_t cap_bytes = kLogDirCapBytes);

// Directory the logger writes to (same resolution as Init, including its
// create-if-missing semantics): %LOCALAPPDATA%\Emebalachat\logs with an
// <exe-dir>\logs fallback. main.cpp prunes this directory at startup
// regardless of the opt-in state (an empty/absent dir prune is a no-op).
// Empty only when neither LOCALAPPDATA nor the module path resolve
// (practically never).
std::filesystem::path DefaultLogDir();

// REQ-003 (session 260909): opt-in gate for USER-CONTENT fields in diagnostic
// lines (typed characters, foreground-window titles, captured text bodies,
// translation output, local prompt bodies). Default FALSE. main.cpp applies
// AppConfig::diag_log_content exactly once here: after the config load and
// before any hook/worker thread exists (runtime UI toggling is out of scope;
// a config change takes effect on restart). Call sites branch on
// ContentLoggingEnabled() and MUST keep every non-PII field (shape-only
// rule): vk/scan/modifiers/ime/composing/class/fg handle, len=, out_len=,
// duration_ms=, status= etc. Lock-free std::atomic<bool>, safe on the
// WH_KEYBOARD_LL thread. Independent of Init()/Shutdown() lifecycles.
void SetContentLogging(bool enabled);
bool ContentLoggingEnabled();

// True between a successful Init() and Shutdown().
bool IsInitialized();

// Full path of the active log file. Empty when uninitialized OR initialized
// but still lazily un-opened (sink disabled / open not yet triggered) —
// REQ-201: an empty path here means NO file exists on disk yet.
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
