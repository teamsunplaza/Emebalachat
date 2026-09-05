// ---------------------------------------------------------------------------
// diag_logger.cpp — see diag_logger.hpp for the contract. This file implements
// the enqueue-only producer path (hook-thread safe) + a dedicated flush thread
// that owns ALL file I/O. Never throws (every public entry is try/catch
// guarded); degrades to disabled on any init failure.
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
#include <fcntl.h>     // _O_BINARY / _O_APPEND / _O_CREAT open flags
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
    std::atomic<bool> enabled{false};
    std::atomic<uint64_t> dropped{0};
    // file is ONLY touched by the flush thread between worker start/join;
    // Init assigns before the thread starts, Shutdown closes after join.
    FILE* file = nullptr;
    std::wstring path;
};

State g;

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

// The flush thread: the ONLY code that touches the FILE. Sleeps on cv_wake,
// swaps whole batches out under the lock, writes + fflushes OUTSIDE the lock
// (producers never wait on disk), then signals drained for Flush().
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
                std::fflush(g.file); // record durability per batch: a later
                                     // hard crash keeps everything flushed
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

bool Init(const std::filesystem::path& dir_override) {
    try {
        if (g.initialized.load(std::memory_order_acquire)) {
            return true; // idempotent
        }
        const std::wstring dir = ResolveLogDir(dir_override);
        if (dir.empty()) {
            return false; // graceful disable, never crash
        }
        const std::wstring path = BuildLogName(dir);
        // Open with FULL sharing (_SH_DENYNO). The CRT _wfopen default is
        // deny-all, which (a) made the headless mid-run readbacks fail in
        // this feature's own tests and (b) would stop the user from opening/
        // tailing the log in Notepad WHILE the app reproduces the bug - the
        // exact scenario this feature exists for. _wsopen_s + _fdopen keeps
        // plain stdio append semantics underneath.
        FILE* fp = nullptr;
        {
            int fd = -1;
            if (_wsopen_s(&fd, path.c_str(), _O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY,
                          _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0 || fd == -1) {
                return false;
            }
            fp = _fdopen(fd, "ab");
            if (!fp) {
                _close(fd);
                return false;
            }
        }
        {
            std::lock_guard<std::mutex> lk(g.mtx);
            g.file = fp;
            g.path = path;
            g.stop_requested = false;
            g.worker_writing = false;
            g.dropped.store(0, std::memory_order_relaxed);
        }
        g.worker = std::thread(WorkerMain); // could throw on thread-starved
                                            // systems; caught below
        g.initialized.store(true, std::memory_order_release);
        g.enabled.store(true, std::memory_order_relaxed); // default ON for the
                                                          // diagnostic build
        wchar_t exe[MAX_PATH * 2] = {};
        ::GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        Printf("SESSION", "==== Emebalachat v%s session start (pid=%lu, exe=%s) ====",
               std::string(emebalachat::kAppVersionA).c_str(),
               static_cast<unsigned long>(::GetCurrentProcessId()),
               emebalachat::ToUtf8(exe).c_str());
        Printf("SESSION", "log_file=%s", emebalachat::ToUtf8(path).c_str());
        return true;
    } catch (...) {
        // std::thread allocation failure etc. Keep the app alive, disabled.
        g.initialized.store(false);
        if (g.worker.joinable()) {
            g.worker.join();
        }
        if (g.file) {
            std::fclose(g.file);
            g.file = nullptr;
        }
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
    std::lock_guard<std::mutex> lk(g.mtx);
    if (g.file) {
        std::fflush(g.file);
        std::fclose(g.file);
        g.file = nullptr;
    }
    g.path.clear();
    g.enabled.store(false, std::memory_order_relaxed);
    g.stop_requested = false;
}

void SetEnabled(bool enabled) {
    g.enabled.store(enabled, std::memory_order_relaxed);
}

bool IsEnabled() {
    return g.enabled.load(std::memory_order_relaxed);
}

bool IsInitialized() {
    return g.initialized.load(std::memory_order_acquire);
}

std::wstring CurrentLogPath() {
    std::lock_guard<std::mutex> lk(g.mtx);
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
