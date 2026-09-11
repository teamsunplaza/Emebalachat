// ---------------------------------------------------------------------------
// diag_logger.cpp — see diag_logger.hpp for the contract. This file implements
// the enqueue-only producer path (hook-thread safe) + a dedicated flush thread
// that owns ALL file I/O. Never throws (every public entry is try/catch
// guarded); degrades to disabled on any init failure.
//
// REQ-201 (session 260911_0002): the file is opened LAZILY. Init() only
// resolves the directory and starts the worker; the first SetEnabled(true)
// transition performs the open (prune-to-cap first), and the flush worker
// retries the open per batch if that attempt failed. The producer path never
// touches file I/O, so the WH_KEYBOARD_LL thread contract is unchanged.
// ---------------------------------------------------------------------------

#include "diag_logger.hpp"
#include "unicode_utils.hpp"
#include "version.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <deque>
#include <algorithm>   // REQ-203 prune: std::sort by mtime
#include <fcntl.h>     // _O_BINARY / _O_APPEND / _O_CREAT open flags
#include <filesystem>  // REQ-203 prune: directory enumeration
#include <io.h>        // _close / _fdopen
#include <share.h>     // _wsopen_s / _SH_DENYNO
#include <sys/stat.h>  // _S_IREAD / _S_IWRITE
#include <mutex>
#include <condition_variable>
#include <string>
#include <thread>
#include <vector>

namespace diag {
namespace {

// Bounded queue: overflow drops OLDEST lines (counted, surfaced as a note
// line in the log). 16k pending lines is far beyond any burst a human typing
// + one translation pipeline can generate while the flush thread runs.
constexpr size_t kQueueMaxLines = 16384;

struct State {
    std::mutex mtx;
    std::condition_variable cv_wake;    // worker sleeps on this
    std::condition_variable cv_drained; // Flush() waits on this
    std::deque<std::string> queue;
    bool worker_writing = false;        // batch swapped out, not yet flushed
    bool stop_requested = false;
    std::thread worker;
    std::atomic<bool> initialized{false};
    // REQ-201: default OFF. Init() used to force this true ("default ON for
    // the diagnostic build"); the release posture inverts it and main.cpp
    // applies diag_log_enabled via SetEnabled after the config load.
    std::atomic<bool> enabled{false};
    std::atomic<uint64_t> dropped{0};
    // REQ-201 lazy open. dir/path/file live under their own mutex: they are
    // touched by the SetEnabled(true) transition (main thread, before any
    // hook thread exists in the app), by the flush worker (write + open
    // retry), and by Shutdown's close after join — never by producers on the
    // WH_KEYBOARD_LL thread. Lock order is file_mtx -> mtx (OpenLogFileLocked
    // takes mtx while the caller holds file_mtx); never acquire mtx first and
    // then file_mtx.
    std::mutex file_mtx;
    std::wstring dir;    // resolved by Init(); empty = file can never open
    FILE* file = nullptr;
    std::wstring path;
};

State g;

// REQ-003 (session 260909): process-wide opt-in gate for USER-CONTENT fields
// (typed characters, window titles, captured bodies, translation output,
// prompt bodies). Deliberately NOT part of State: the value is applied once
// by main.cpp after the config load and must survive Init/Shutdown cycles
// (the tests re-Init, and startup order is Init -> config load -> apply).
// Lock-free atomic load; safe from the WH_KEYBOARD_LL hook thread per key.
std::atomic<bool> g_content_logging{false};

// "yyyy-mm-dd hh:mm:ss.mmm" in LOCAL time, captured at enqueue time so the
// stamp reflects true occurrence, not flush time.
std::string TimestampNow() {
    SYSTEMTIME st = {};
    ::GetLocalTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                  static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth),
                  static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond),
                  static_cast<unsigned>(st.wMilliseconds));
    return buf;
}

// Appends msg with control characters escaped so ONE record is ONE physical
// line (grep-friendly). Fast path: pure printable runs append verbatim.
void AppendEscaped(std::string& out, std::string_view msg) {
    size_t run = 0;
    for (size_t i = 0; i < msg.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(msg[i]);
        if (c >= 0x20 && c != 0x7F) {
            continue;
        }
        if (i > run) {
            out.append(msg.data() + run, i - run);
        }
        run = i + 1;
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: {
                char hex[8];
                std::snprintf(hex, sizeof(hex), "\\x%02X", c);
                out += hex;
            } break;
        }
    }
    if (run < msg.size()) {
        out.append(msg.data() + run, msg.size() - run);
    }
}

// Core producer: stamp + format + enqueue. Lock hold time is a single
// push_back (+ rare pop_front drop); no I/O, no allocation beyond the line
// string built before the lock. Called from any thread incl. WH_KEYBOARD_LL.
void Enqueue(std::string_view tag, std::string_view msg) {
    if (!g.initialized.load(std::memory_order_acquire) ||
        !g.enabled.load(std::memory_order_relaxed)) {
        return; // cheap exit before any work for the disabled path
    }
    std::string line;
    line.reserve(64 + tag.size() + msg.size());
    line += TimestampNow();
    line += " [";
    char tid[16];
    std::snprintf(tid, sizeof(tid), "%lu", static_cast<unsigned long>(::GetCurrentThreadId()));
    line += tid;
    line += "] ";
    line += tag;
    line += '/';
    AppendEscaped(line, msg);
    line += '\n';
    try {
        std::lock_guard<std::mutex> lk(g.mtx);
        if (!g.initialized.load(std::memory_order_relaxed)) {
            return; // racing Shutdown()
        }
        if (g.queue.size() >= kQueueMaxLines) {
            g.queue.pop_front(); // drop OLDEST per contract
            g.dropped.fetch_add(1, std::memory_order_relaxed);
        }
        g.queue.push_back(std::move(line));
    } catch (...) {
        // Allocation failure only: logging must never crash the host.
        return;
    }
    g.cv_wake.notify_one();
}

// Format a printf call into a fresh string (stack fast path, heap for long
// translation payloads). Returns false on formatting error.
bool FormatVa(std::string& out, const char* fmt, va_list args) {
    char stackbuf[512];
    va_list args2;
    va_copy(args2, args);
    const int need = std::vsnprintf(stackbuf, sizeof(stackbuf), fmt, args2);
    va_end(args2);
    if (need < 0) {
        return false;
    }
    if (static_cast<size_t>(need) < sizeof(stackbuf)) {
        out.assign(stackbuf, static_cast<size_t>(need));
        return true;
    }
    out.resize(static_cast<size_t>(need) + 1);
    const int again = std::vsnprintf(out.data(), out.size(), fmt, args);
    if (again < 0) {
        out.clear();
        return false;
    }
    out.resize(static_cast<size_t>(again));
    return true;
}

// Strip one trailing newline (the fprintf convention "\n" terminators) before
// the line is escaped/enqueued; AppendEscaped would otherwise emit "\\n".
std::string_view TrimTrailingNewline(std::string_view s) {
    if (!s.empty() && s.back() == '\n') {
        s.remove_suffix(1);
    }
    if (!s.empty() && s.back() == '\r') {
        s.remove_suffix(1);
    }
    return s;
}

// Parse the existing "MODULE/site/NNN: body" stderr convention out of a
// mirrored fprintf payload so the file line gets a meaningful TAG. Falls back
// to "STDERR" for free-form payloads (llama.cpp log passthrough etc.).
void SplitMirrorTag(std::string_view text, std::string& tag, std::string& body) {
    const size_t colon = text.find(':');
    if (colon != std::string_view::npos && colon > 0 && colon <= 64) {
        bool ok = true;
        for (size_t i = 0; i < colon; ++i) {
            const char c = text[i];
            const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                               (c >= '0' && c <= '9');
            if (!alnum && c != '_' && c != '/' && c != '.' && c != '-') {
                ok = false;
                break;
            }
        }
        if (ok) {
            tag = std::string(text.substr(0, colon));
            body = std::string(text.substr(colon + 1));
            if (!body.empty() && body.front() == ' ') {
                body.erase(body.begin());
            }
            return;
        }
    }
    tag = "STDERR";
    body = std::string(text);
}

// Defined below (with the other directory helpers); needed by the lazy open.
std::wstring BuildLogName(const std::wstring& dir);

// Open (or re-open) the per-run log file. CALLER CONTRACT: hold file_mtx,
// do NOT hold mtx (this function takes mtx internally for the SESSION-header
// push_front; lock order file_mtx -> mtx). Used by the SetEnabled(true)
// transition and by the worker as an open-retry fallback. Best-effort by
// design (REQ-201/202): a failed open leaves file/path null, lines stay
// cheaply queued, and the next transition or worker batch retries. The
// SESSION header is injected into the FRONT of the queue here, so it is
// always the first content physically present in the file — whether the open
// happened on the enable transition or on a later retry. Returns the current
// path (empty when no file is open).
std::wstring OpenLogFileLocked() {
    if (!g.dir.empty() && !g.file) {
        // REQ-203: enforce the cap BEFORE the new file is created (design
        // 144800 §2.2 step 5: the current run's file trivially cannot be
        // pruned because it does not exist yet at prune time). Normally a
        // no-op — main.cpp already pruned at startup — but a long-running
        // enabled session that crossed the cap without restarting gets
        // bounded at its next open instead of never.
        PruneLogs(g.dir, kLogDirCapBytes);
        const std::wstring path = BuildLogName(g.dir);
        // Open with FULL sharing (_SH_DENYNO). The CRT _wfopen default is
        // deny-all, which (a) made the headless mid-run readbacks fail in
        // this feature's own tests and (b) would stop the user from opening/
        // tailing the log in Notepad WHILE the app reproduces the bug - the
        // exact scenario this feature exists for. _wsopen_s + _fdopen keeps
        // plain stdio append semantics underneath.
        FILE* fp = nullptr;
        int fd = -1;
        if (_wsopen_s(&fd, path.c_str(), _O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY,
                      _SH_DENYNO, _S_IREAD | _S_IWRITE) == 0 && fd != -1) {
            fp = _fdopen(fd, "ab");
            if (!fp) {
                _close(fd);
            }
        }
        if (fp) {
            g.file = fp;
            g.path = path;
            // SESSION header, enqueued at the FRONT so it precedes every
            // line that queued during the disabled window (they stay: a
            // forensic run's early events are exactly what debugging wants).
            wchar_t exe[MAX_PATH * 2] = {};
            ::GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
            // Generous buffers: ToUtf8 of a MAX_PATH*2 wide exe path can
            // reach ~4 bytes per wchar (CJK/emoji install dirs). 64/256 would
            // silently truncate the forensic header lines.
            char ver[MAX_PATH * 8 + 128];
            std::snprintf(ver, sizeof(ver),
                          "==== Emebalachat v%s session start (pid=%lu, exe=%s) ====",
                          std::string(emebalachat::kAppVersionA).c_str(),
                          static_cast<unsigned long>(::GetCurrentProcessId()),
                          emebalachat::ToUtf8(exe).c_str());
            char loc[MAX_PATH * 8 + 32];
            std::snprintf(loc, sizeof(loc), "log_file=%s",
                          emebalachat::ToUtf8(path).c_str());
            const std::string ts = TimestampNow();
            const std::string tid = " [" + std::to_string(::GetCurrentThreadId()) + "] ";
            std::lock_guard<std::mutex> lk(g.mtx);
            if (g.initialized.load(std::memory_order_relaxed) &&
                !g.stop_requested) {
                // The worker fwrites queue entries VERBATIM (Enqueue is the
                // one that appends '\n'), so the injected lines must carry
                // their own terminator — otherwise the header concatenates
                // with the next record on disk.
                g.queue.push_front(ts + tid + "SESSION/" + std::string(loc) + "\n");
                g.queue.push_front(ts + tid + "SESSION/" + std::string(ver) + "\n");
            }
        }
    }
    return g.path;
}

// The flush thread: the ONLY code that writes the FILE (the SetEnabled
// transition opens it under file_mtx). Lock discipline: the global order is
// file_mtx -> mtx, never the reverse; OpenLogFileLocked takes mtx internally
// for the SESSION-header push_front, so the caller must NOT hold mtx when
// entering it. The queue swap stays on mtx alone, so producers never wait on
// disk: the fwrite/fflush block below runs outside mtx.
void WorkerMain() {
    std::deque<std::string> batch;
    uint64_t dropped_reported = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g.mtx);
            g.cv_wake.wait(lk, [] { return g.stop_requested || !g.queue.empty(); });
            if (!g.queue.empty()) {
                g.worker_writing = true;
                batch.clear();
                batch.swap(g.queue);
            } else if (g.stop_requested) {
                return;
            }
        }
        if (!batch.empty()) {
            {
                std::lock_guard<std::mutex> flk(g.file_mtx);
                if (!g.file) {
                    // Open-retry fallback: the enable transition failed
                    // (disk locked at startup, transient sharing violation,
                    // OneDrive sync lock). One attempt per batch; the SESSION
                    // header re-injection inside handles itself.
                    OpenLogFileLocked();
                }
                if (g.file) {
                    for (const std::string& line : batch) {
                        std::fwrite(line.data(), 1, line.size(), g.file);
                    }
                    const uint64_t d = g.dropped.load(std::memory_order_relaxed);
                    if (d > dropped_reported) {
                        dropped_reported = d;
                        std::string note;
                        note += TimestampNow();
                        note += " [diag] DIAG/queue NOTE: ";
                        note += std::to_string(d);
                        note += " oldest lines dropped (queue overflow, cap ";
                        note += std::to_string(kQueueMaxLines);
                        note += ") since last note\n";
                        std::fwrite(note.data(), 1, note.size(), g.file);
                    }
                    std::fflush(g.file); // record durability per batch: a
                                          // later hard crash keeps everything
                                          // flushed
                }
            }
            batch.clear();
            std::lock_guard<std::mutex> lk(g.mtx);
            g.worker_writing = false;
            g.cv_drained.notify_all();
        }
    }
}

// Resolve + create the log directory: LOCALAPPDATA first, then exe-dir
// fallback (VP spec). Returns empty when neither could be prepared.
std::wstring ResolveLogDir(const std::filesystem::path& dir_override) {
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates;
    if (!dir_override.empty()) {
        candidates.push_back(dir_override);
    } else {
        size_t len = 0;
        if (_wgetenv_s(&len, nullptr, 0, L"LOCALAPPDATA") == 0 && len > 1) {
            std::wstring v(len, L'\0');
            if (_wgetenv_s(&len, &v[0], len, L"LOCALAPPDATA") == 0) {
                while (!v.empty() && v.back() == L'\0') {
                    v.pop_back();
                }
                if (!v.empty()) {
                    candidates.push_back(fs::path(v) / L"Emebalachat" / L"logs");
                }
            }
        }
        wchar_t exe[MAX_PATH * 2] = {};
        if (::GetModuleFileNameW(nullptr, exe, MAX_PATH * 2) > 0) {
            candidates.push_back(fs::path(exe).parent_path() / L"logs");
        }
    }
    for (const fs::path& dir : candidates) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (!ec && fs::is_directory(dir, ec) && !ec) {
            return dir.wstring();
        }
    }
    return {};
}

// emebalachat_yymmddhhmmss.log (LOCAL time in the name, user-specified
// format). One file per run: an already-existing name (two runs inside the
// same second) gets a "-N" collision suffix.
std::wstring BuildLogName(const std::wstring& dir) {
    SYSTEMTIME st = {};
    ::GetLocalTime(&st);
    wchar_t base[64] = {};
    swprintf_s(base, L"emebalachat_%02u%02u%02u%02u%02u%02u",
               static_cast<unsigned>(st.wYear % 100), static_cast<unsigned>(st.wMonth),
               static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
               static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));
    namespace fs = std::filesystem;
    for (unsigned n = 0; n < 100; ++n) {
        std::wstring stem = base;
        if (n > 0) {
            wchar_t suffix[16] = {};
            swprintf_s(suffix, L"-%u", n);
            stem += suffix;
        }
        fs::path cand = fs::path(dir) / (stem + L".log");
        std::error_code ec;
        if (!fs::exists(cand, ec)) {
            return cand.wstring();
        }
    }
    // Absurd collision case (100 runs within one second): reuse the base name
    // anyway — "ab" append keeps both runs' records instead of losing logs.
    return (fs::path(dir) / (std::wstring(base) + L".log")).wstring();
}

} // namespace

// Cap-enforcing cleanup of a logs directory (REQ-203). Public entry: the
// declaration + contract live in the header; this external-linkage definition
// matches it. Also called by the lazy-open path above (declared earlier by
// the header include). Never throws: every filesystem call uses the
// error_code overloads, and per-file failures (OneDrive/AV locks, files
// vanishing mid-enumeration) are skipped, never fatal — pruning is startup
// hygiene, not a startup blocker.
uint64_t PruneLogs(const std::filesystem::path& dir, uint64_t cap_bytes) {
    namespace fs = std::filesystem;
    uint64_t deleted = 0;
    try {
        std::error_code ec;
        if (!fs::is_directory(dir, ec) || ec) {
            return 0; // nothing to prune (also the opted-out user's common case)
        }
        struct Entry {
            fs::path p;
            uintmax_t size = 0;
            fs::file_time_type mtime{};
        };
        std::vector<Entry> entries;
        uintmax_t total = 0;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code fec;
            if (!it->is_regular_file(fec) || fec) {
                continue;
            }
            const std::wstring name = it->path().filename().wstring();
            const std::wstring kPrefix = L"emebalachat_";
            const std::wstring kSuffix = L".log";
            if (name.size() < kPrefix.size() + kSuffix.size() ||
                name.compare(0, kPrefix.size(), kPrefix) != 0 ||
                name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
                continue; // foreign files in the dir are not ours to touch
            }
            Entry e;
            e.p = it->path();
            e.size = static_cast<uintmax_t>(fs::file_size(e.p, fec));
            if (fec) {
                continue; // vanished or unreadable metadata: skip
            }
            e.mtime = fs::last_write_time(e.p, fec);
            if (fec) {
                e.mtime = fs::file_time_type{}; // oldest possible: pruned first
            }
            total += e.size;
            entries.push_back(std::move(e));
        }
        if (total <= cap_bytes || entries.empty()) {
            return 0;
        }
        // Oldest-first (mtime ascending): the rotation cap drops the stalest
        // sessions first, keeping the most recent history.
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& a, const Entry& b) { return a.mtime < b.mtime; });
        for (const Entry& e : entries) {
            if (total <= cap_bytes) {
                break;
            }
            std::error_code rec;
            fs::remove(e.p, rec); // locked/vanished files are skipped by contract
            if (!rec) {
                total -= e.size;
                ++deleted;
            }
        }
        return deleted;
    } catch (...) {
        return deleted; // logging must never crash or block the host
    }
}

std::filesystem::path DefaultLogDir() {
    // ResolveLogDir lives in the anonymous namespace above; anonymous-namespace
    // names are visible unqualified from the enclosing namespace diag.
    return std::filesystem::path(ResolveLogDir({}));
}

bool Init(const std::filesystem::path& dir_override) {
    try {
        if (g.initialized.load(std::memory_order_acquire)) {
            return true; // idempotent
        }
        const std::wstring dir = ResolveLogDir(dir_override);
        if (dir.empty()) {
            return false; // graceful disable, never crash
        }
        {
            std::lock_guard<std::mutex> flk(g.file_mtx);
            g.dir = dir; // REQ-201: directory only — the FILE opens lazily
        }
        {
            std::lock_guard<std::mutex> lk(g.mtx);
            g.stop_requested = false;
            g.worker_writing = false;
            g.dropped.store(0, std::memory_order_relaxed);
        }
        g.worker = std::thread(WorkerMain); // could throw on thread-starved
                                            // systems; caught below
        g.initialized.store(true, std::memory_order_release);
        // REQ-201: the file sink starts DISABLED (was hardcoded ON for the
        // 260905 diagnostic build). main.cpp applies diag_log_enabled via
        // SetEnabled right after the config load; until then nothing is
        // enqueued and no file exists. The SESSION header now lives in
        // OpenLogFileLocked() (see SetEnabled) because Init no longer has a
        // file to write it into.
        return true;
    } catch (...) {
        // std::thread allocation failure etc. Keep the app alive, disabled.
        g.initialized.store(false);
        if (g.worker.joinable()) {
            g.worker.join();
        }
        std::lock_guard<std::mutex> flk(g.file_mtx);
        if (g.file) {
            std::fclose(g.file);
            g.file = nullptr;
        }
        g.path.clear();
        g.dir.clear();
        return false;
    }
}

void Shutdown() {
    if (!g.initialized.exchange(false)) {
        return; // also claims the teardown slot (no double Shutdown)
    }
    {
        std::lock_guard<std::mutex> lk(g.mtx);
        g.stop_requested = true;
    }
    g.cv_wake.notify_all();
    if (g.worker.joinable()) {
        g.worker.join(); // drains everything still queued (WorkerMain exits
                         // only on stop_requested && queue empty)
    }
    // The worker is joined: no concurrent file access is possible anymore,
    // but the SetEnabled transition may have opened the file on the main
    // thread, so close under file_mtx and never while holding mtx.
    std::lock_guard<std::mutex> flk(g.file_mtx);
    if (g.file) {
        std::fflush(g.file);
        std::fclose(g.file);
        g.file = nullptr;
    }
    g.path.clear();
    g.dir.clear();
    g.enabled.store(false, std::memory_order_relaxed);
    g.stop_requested = false;
}

void SetEnabled(bool enabled) {
    g.enabled.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        return; // silencing the sink never closes the file: re-enabling
                // within the same run appends to it (runtime toggle semantics
                // preserved from the pre-REQ-201 suite)
    }
    // REQ-201 lazy open: the first enable transition creates the file. If
    // Init() never succeeded (no dir) or the open fails, the worker's
    // per-batch retry picks it up later — best-effort, never fatal.
    if (!g.initialized.load(std::memory_order_acquire)) {
        return;
    }
    try {
        std::lock_guard<std::mutex> flk(g.file_mtx);
        OpenLogFileLocked();
    } catch (...) {
        // Allocation etc. on the open path: fall back to worker retry.
    }
}

bool IsEnabled() {
    return g.enabled.load(std::memory_order_relaxed);
}

void SetContentLogging(bool enabled) {
    g_content_logging.store(enabled, std::memory_order_relaxed);
}

bool ContentLoggingEnabled() {
    return g_content_logging.load(std::memory_order_relaxed);
}

bool IsInitialized() {
    return g.initialized.load(std::memory_order_acquire);
}

std::wstring CurrentLogPath() {
    // REQ-201: path/dir/file moved under file_mtx (the SetEnabled transition
    // can now open on the main thread while hook/worker threads are alive).
    std::lock_guard<std::mutex> lk(g.file_mtx);
    return g.path;
}

uint64_t DroppedCount() {
    return g.dropped.load(std::memory_order_relaxed);
}

void Flush() {
    if (!g.initialized.load(std::memory_order_acquire)) {
        return;
    }
    std::unique_lock<std::mutex> lk(g.mtx);
    // Wait until every enqueued line has been written AND fflushed: the
    // worker only clears worker_writing after its fflush returns. Total wait
    // is BOUNDED (kFlushGiveUpMs): a wedged disk (OneDrive lock, full media)
    // must never hang the caller — diagnostics must not create a new hang
    // class. On give-up, lines already handed to the OS stay queued in the
    // FILE stream buffer and are flushed at normal process teardown anyway.
    constexpr int kFlushGiveUpMs = 5000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kFlushGiveUpMs);
    while (!g.queue.empty() || g.worker_writing) {
        g.cv_wake.notify_one();
        if (g.cv_drained.wait_until(lk, deadline) == std::cv_status::timeout) {
            return;
        }
        if (!g.initialized.load(std::memory_order_relaxed)) {
            return; // racing Shutdown: its join already drains the queue
        }
    }
}

void Printf(std::string_view tag, const char* fmt, ...) {
    if (!fmt || !g.initialized.load(std::memory_order_acquire) ||
        !g.enabled.load(std::memory_order_relaxed)) {
        return;
    }
    try {
        std::string msg;
        va_list args;
        va_start(args, fmt);
        const bool ok = FormatVa(msg, fmt, args);
        va_end(args);
        if (ok) {
            Enqueue(tag, TrimTrailingNewline(msg));
        }
    } catch (...) {
    }
}

void MirrorF(const char* fmt, ...) {
    if (!fmt) {
        return;
    }
    try {
        std::string text;
        va_list args;
        va_start(args, fmt);
        const bool ok = FormatVa(text, fmt, args);
        va_end(args);
        if (!ok) {
            return;
        }
        // Sink 1: stderr, byte-identical to the original fprintf(stderr, ...)
        // call sites (compatibility kept per VP directive).
        std::fwrite(text.data(), 1, text.size(), stderr);
        std::fflush(stderr);
        // Sink 2: the diag file with the MODULE/site/NNN token as TAG.
        if (g.initialized.load(std::memory_order_acquire) &&
            g.enabled.load(std::memory_order_relaxed)) {
            std::string tag;
            std::string body;
            SplitMirrorTag(TrimTrailingNewline(text), tag, body);
            Enqueue(tag, body);
        }
    } catch (...) {
    }
}

} // namespace diag
