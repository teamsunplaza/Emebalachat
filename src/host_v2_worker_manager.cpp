// host_v2_worker_manager implementation — REQ-043 (M6 T3, design §1.1).
// The Win32 plumbing behind the header's seam surface: real spawn (hidden,
// same user, token on the command line), private pipe creation with the
// user-only SD (the host_main.cpp BuildUserOnlySd pattern, restated here so
// the orchestrator does not depend on the frozen v1 TU), the M-2 ordered
// graceful stop, and the 250 ms reaper thread with exponential backoff.

#include "host_v2_worker_manager.hpp"

#include <bcrypt.h>   // BCryptGenRandom (boot-scoped worker token)
#include <sddl.h>
#include <shlobj.h>

#include "diag_logger.hpp" // DIAG_LOG / DIAG_F (shape-only diagnostics)

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwchar>

namespace emebalachat {
namespace host_v2 {

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- user-only security descriptor (§4.1 pattern; design §10 row 1) --------
struct SecurityDescriptorHolder {
    SecurityDescriptorHolder() = default;
    PSECURITY_DESCRIPTOR sd = nullptr;
    ~SecurityDescriptorHolder() { if (sd) ::LocalFree(sd); }
    SecurityDescriptorHolder(const SecurityDescriptorHolder&) = delete;
    SecurityDescriptorHolder& operator=(const SecurityDescriptorHolder&) = delete;
};

bool BuildUserOnlySd(SecurityDescriptorHolder& holder) {
    HANDLE hToken = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &hToken)) return false;
    DWORD len = 0;
    ::GetTokenInformation(hToken, TokenUser, nullptr, 0, &len);
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) {
        ::CloseHandle(hToken);
        return false;
    }
    std::vector<BYTE> buf(len);
    const BOOL okInfo = ::GetTokenInformation(hToken, TokenUser, buf.data(), len, &len);
    ::CloseHandle(hToken);
    if (!okInfo) return false;
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buf.data());
    LPWSTR sidStr = nullptr;
    if (!::ConvertSidToStringSidW(user->User.Sid, &sidStr) || !sidStr) return false;
    const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sidStr + L")";
    ::LocalFree(sidStr);
    return ::ConvertStringSecurityDescriptorToSecurityDescriptorW(
               sddl.c_str(), SDDL_REVISION_1, &holder.sd, nullptr) != FALSE;
}

// Boot-scoped token (§4.2 pattern): 128-bit random hex32, lives on the child
// command line only — NO token file, nothing to clean up (design §10 row 2).
bool GenerateTokenHex32(std::string& out) {
    BYTE bytes[16] = {};
    const NTSTATUS st = ::BCryptGenRandom(nullptr, bytes, sizeof(bytes),
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st < 0) return false;
    static const char kHex[] = "0123456789abcdef";
    out.clear();
    out.reserve(32);
    for (BYTE b : bytes) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return true;
}

// Private worker pipe name: \\.\pipe\emebala-engine-worker-<pid>-<rand>
// (design §1.2 transport row; unique per spawn attempt).
std::wstring MakeWorkerPipeName() {
    wchar_t buf[128];
    swprintf(buf, 128, L"\\\\.\\pipe\\emebala-engine-worker-%lu-%u",
             static_cast<unsigned long>(::GetCurrentProcessId()),
             static_cast<unsigned>(::GetTickCount()));
    return buf;
}

constexpr DWORD kPipeBufSize = (1u << 20) + 4;

// Hard kill for a child that cannot be ordered down (handshake failure /
// orphan guard). Best effort: a race-lost kill still gets closed by the
// caller's CloseProcess (the reaper never waits on a zombie handle).
void TerminateProcessHandle(HANDLE process) {
    if (process) ::TerminateProcess(process, 1);
}

// ---- default Win32 seams ----------------------------------------------------
SpawnResult LaunchWorkerProcess(const SpawnRequest& req, void* /*user*/) {
    SpawnResult r;
    if (req.exe_path.empty() || req.pipe_name.empty() || req.token.size() != 32) {
        r.last_error = ERROR_INVALID_PARAMETER;
        return r;
    }
    // hex32 chars are ASCII: explicit per-char widen (no /W4 C4244 warning).
    std::wstring token_w;
    token_w.reserve(req.token.size());
    for (char c : req.token) token_w.push_back(static_cast<wchar_t>(c));
    std::wstring cmd = L"\"" + req.exe_path + L"\" --pipe " + req.pipe_name +
                       L" --token " + token_w;
    // P2-1 stabilization observability (temporary diagnostic): when THIS
    // host's own stderr is a real handle (a logging run started with
    // redirection), hand the child exactly that handle through
    // STARTF_USESTDHANDLES. bInheritHandles stays FALSE, so the child gets
    // ONLY the two standard handles named here (no other host handle leaks
    // in, pipe EOF semantics unchanged). Do NOT combine with
    // PROC_THREAD_ATTRIBUTE_HANDLE_LIST: std handles are implicitly
    // inherited with STARTF_USESTDHANDLES and listing them too fails the
    // create with ERROR_INVALID_PARAMETER (measured). In a normal GUI spawn
    // stderr is null/invalid and this is a no-op.
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; // design §1.1 rule 1: hidden background child
    HANDLE stderr_handle = ::GetStdHandle(STD_ERROR_HANDLE);
    if (stderr_handle != nullptr && stderr_handle != INVALID_HANDLE_VALUE) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdError = stderr_handle;
        si.hStdOutput = stderr_handle;
    }
    PROCESS_INFORMATION pi = {};
    // bInheritHandles=TRUE is required for STARTF_USESTDHANDLES delivery;
    // it leaks nothing here because NO host handle is created inheritable
    // (every CreateEventW/CreateNamedPipeW/CreateFileW uses a non-inheritable
    // default or an explicit FALSE), so the child receives exactly the two
    // standard handles named above.
    const BOOL ok = ::CreateProcessW(req.exe_path.c_str(), cmd.data(), nullptr, nullptr,
                                     TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) {
        r.last_error = ::GetLastError();
        return r;
    }
    ::CloseHandle(pi.hThread);
    // The process handle doubles as the done_event (signaled on child exit) —
    // the ReaperLoop's 250 ms WaitForSingleObject target (design §1.1 rule 3).
    r.ok = true;
    r.process = pi.hProcess;
    r.done_event = pi.hProcess;
    return r;
}

void CloseWorkerProcess(HANDLE process, void* /*user*/) {
    if (process) ::CloseHandle(process);
}

// One message write on the orchestrator side. Ok -> the frame is queued;
// Broken -> the pipe is dead (peer gone, immediate error); Stalled -> the
// peer stopped draining and the bounded wait timed out (the pending write is
// cancelled — message-mode atomicity discards the whole frame).
FrameWriteResult WritePipeFrame(HANDLE pipe, std::string_view json) {
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) return FrameWriteResult::Broken;
    std::string frame;
    frame.reserve(4 + json.size());
    const uint32_t len = static_cast<uint32_t>(json.size());
    for (unsigned i = 0; i < 4; ++i) {
        frame.push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
    }
    frame.append(json);
    OVERLAPPED ol = {};
    ol.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ol.hEvent) return FrameWriteResult::Broken;
    DWORD written = 0;
    BOOL ok = ::WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, &ol);
    if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
        // Bounded: a wedged peer must not pin the caller (the reaper judges
        // the process separately).
        const DWORD w = ::WaitForSingleObject(ol.hEvent, 15000);
        if (w == WAIT_OBJECT_0) {
            ok = ::GetOverlappedResult(pipe, &ol, &written, FALSE);
        } else {
            // P2-1 stabilization observability: the peer stopped draining for
            // the full window (a busy/stuck worker backs the pipe up). Shape-
            // only: byte count only, never content.
            DIAG_F("ENGINEHOST/wmgr/017: frame write stalled 15s, cancelling (bytes=%zu)\n",
                   json.size());
            // Cancel window race (same class as overlapped_io_util.hpp's fix):
            // the write may COMPLETE between the wait timeout and CancelIoEx.
            // The cancel-approve result decides: a completed write DELIVERED
            // the frame — report Ok so the caller and the pipe agree.
            // Reporting Stalled for a delivered frame leaves a dangling armed
            // state downstream (live worker/031: a "failed" feed META that
            // actually armed the worker, then the next session_open hit the
            // arm -> "expected binary PCM, got JSON" -> exit 3).
            ::CancelIoEx(pipe, &ol);
            DWORD completed = 0;
            if (::GetOverlappedResult(pipe, &ol, &completed, TRUE)) {
                written = completed;
                ok = TRUE;
            } else {
                ::CloseHandle(ol.hEvent);
                return FrameWriteResult::Stalled;
            }
        }
    }
    ::CloseHandle(ol.hEvent);
    if (!ok || written != frame.size()) return FrameWriteResult::Broken;
    return FrameWriteResult::Ok;
}

// One message read with a deadline. Ok -> json; Timeout -> empty, alive;
// IoError -> peer gone/cap violation.
enum class PipeRead { Ok, Timeout, IoError };

PipeRead ReadPipeFrame(HANDLE pipe, std::string& json, DWORD timeout_ms) {
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) return PipeRead::IoError;
    static thread_local std::vector<char> buf;
    if (buf.size() < kPipeBufSize) buf.resize(kPipeBufSize);
    OVERLAPPED ol = {};
    ol.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ol.hEvent) return PipeRead::IoError;
    DWORD read = 0;
    BOOL ok = ::ReadFile(pipe, buf.data(), kPipeBufSize, &read, &ol);
    if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
        const DWORD w = ::WaitForSingleObject(ol.hEvent, timeout_ms);
        if (w == WAIT_TIMEOUT) {
            // Cancel window race: the read may COMPLETE between the wait
            // timeout and CancelIoEx. Honor the completion — discarding a
            // completed frame LOSES it (consumed from the pipe), e.g. a
            // worker `closed` answer vanishing into a bogus Timeout.
            ::CancelIoEx(pipe, &ol);
            DWORD completed = 0;
            if (::GetOverlappedResult(pipe, &ol, &completed, TRUE) && completed > 0) {
                read = completed;
                ok = TRUE;
            } else {
                ::CloseHandle(ol.hEvent);
                return PipeRead::Timeout;
            }
        } else if (w != WAIT_OBJECT_0) {
            ::CancelIoEx(pipe, &ol);
            ::CloseHandle(ol.hEvent);
            return PipeRead::IoError;
        } else {
            ok = ::GetOverlappedResult(pipe, &ol, &read, TRUE);
        }
    }
    ::CloseHandle(ol.hEvent);
    if (!ok) return PipeRead::IoError; // ERROR_BROKEN_PIPE = worker closed
    if (read < enginehost::kFrameHeaderSize) return PipeRead::IoError;
    uint32_t len = 0;
    enginehost::FrameReadLengthPrefix(buf.data(), len);
    if (len > enginehost::kMaxFrameBytes ||
        static_cast<size_t>(len) + enginehost::kFrameHeaderSize != read) {
        return PipeRead::IoError;
    }
    json.assign(buf.data() + enginehost::kFrameHeaderSize, static_cast<size_t>(len));
    return PipeRead::Ok;
}

} // namespace

// ---- Impl -------------------------------------------------------------------
struct WorkerManager::Impl {
    std::mutex mu;
    std::vector<WorkerHandle> workers;
    SpawnFn spawn_fn = nullptr;
    CloseProcessFn close_fn = nullptr;
    void* user = nullptr;

    std::thread reaper_thread;
    std::atomic<bool> reaper_stop{false};
    HANDLE reaper_wake = nullptr; // auto-reset event: StopReaper beats the 250 ms poll

    Impl(SpawnFn s, CloseProcessFn c, void* u)
        : spawn_fn(s ? s : &LaunchWorkerProcess),
          close_fn(c ? c : &CloseWorkerProcess),
          user(u) {
        reaper_wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ~Impl() {
        if (reaper_wake) ::CloseHandle(reaper_wake);
    }

    WorkerHandle* FindLocked(const std::wstring& family) {
        for (auto& w : workers) {
            if (w.family == family) return &w;
        }
        return nullptr;
    }

    // Tear a dead/broken pipe (process handle stays — the reaper still
    // watches it for the crash transition).
    void ClosePipeLocked(WorkerHandle& w) {
        if (w.pipe && w.pipe != INVALID_HANDLE_VALUE) {
            ::DisconnectNamedPipe(w.pipe);
            ::CloseHandle(w.pipe);
        }
        w.pipe = nullptr;
    }

    void CloseProcessLocked(WorkerHandle& w) {
        if (w.process) close_fn(w.process, user);
        w.process = nullptr;
    }
};

// ---- lifecycle --------------------------------------------------------------
WorkerManager::WorkerManager(SpawnFn spawn, CloseProcessFn close, void* user)
    : impl_(new Impl(spawn, close, user)) {}

WorkerManager::~WorkerManager() {
    StopReaper();
    ShutdownAll();
    delete impl_;
}

void WorkerManager::RegisterFamily(const std::wstring& family, const std::wstring& exe_path) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->FindLocked(family)) return; // idempotent
    WorkerHandle w;
    w.family = family;
    w.exe_path = exe_path;
    w.state = WorkerState::Stopped;
    impl_->workers.push_back(std::move(w));
    DIAG_LOG("ENGINEHOST", "wmgr/001: family registered (family=%ws exe_present=%d)",
             family.c_str(),
             (!exe_path.empty() &&
              ::GetFileAttributesW(exe_path.c_str()) != INVALID_FILE_ATTRIBUTES) ? 1 : 0);
}

WorkerState WorkerManager::State(const std::wstring& family) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    const WorkerHandle* w = impl_->FindLocked(family);
    return w ? w->state : WorkerState::Unavailable;
}

WorkerHandle* WorkerManager::Find(const std::wstring& family) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->FindLocked(family);
}

void WorkerManager::SetBusy(const std::wstring& family, bool busy) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    WorkerHandle* w = impl_->FindLocked(family);
    if (!w) return;
    if (busy && w->state == WorkerState::Ready) w->state = WorkerState::Busy;
    else if (!busy && w->state == WorkerState::Busy) w->state = WorkerState::Ready;
}

bool WorkerManager::SendToWorker(const std::wstring& family, std::string_view json) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    WorkerHandle* w = impl_->FindLocked(family);
    if (!w || (w->state != WorkerState::Ready && w->state != WorkerState::Busy)) return false;
    const FrameWriteResult rc = WritePipeFrame(w->pipe, json);
    if (rc == FrameWriteResult::Ok) return true;
    if (WorkerWriteFailureNeedsKill(rc)) {
        // P2-1 stabilization (live wedge): the worker is alive but not
        // draining. Kill it NOW — otherwise its per-family single-instance
        // mutex rejects every duplicate spawn while the stuck process holds
        // it, and the family wedges in backoff-respawn churn (persistent
        // `unavailable`). We finalize the crash right here (the reaper only
        // watches Ready/Busy/Spawning, and state is Crashed from here on);
        // EnsureSpawned's backoff gate paces the respawn.
        DIAG_F("ENGINEHOST/wmgr/019: stalled worker terminated (family=%ws)\n",
               family.c_str());
        TerminateProcessHandle(w->process);
        impl_->ClosePipeLocked(*w);
        impl_->CloseProcessLocked(*w);
        w->state = WorkerState::Crashed;
        w->spawn_failures++;
        w->next_spawn_allowed_ms = NowMs() + BackoffDelayMs(w->spawn_failures);
        return false;
    }
    // A dead pipe is a crashed worker in waiting: mark it so the reaper's
    // next pass finalizes the transition (no respawn from this thread).
    w->state = WorkerState::Crashed;
    DIAG_F("ENGINEHOST/wmgr/002: frame write failed; family marked crashed (family=%ws)\n",
           family.c_str());
    return false;
}

// M6 T4 (orchestrator dispatch): bounded single-frame read on the family
// pipe. Ok -> json; Timeout -> alive, nothing yet; IoError -> the pipe is
// dead (caller answers engine_failed; the reaper judges the respawn).
//
// P2-1 stabilization R4 (session 260925_0001-GPU-R4, live wedge): SPLIT
// PHASE — the bounded kernel wait runs WITHOUT the manager mutex. The relay
// polls at kAsrRelayPollMs while holding nothing, so the feed path
// (SendToWorker) and the translate dispatch never serialize behind the poll
// (the ":369 convoy" the original comment deferred; measured relay ceiling
// ~4.3 chunks/s vs the 6.25 chunks/s feed). Phase 1 validates and captures
// the pipe under the lock; phase 3 re-validates the SAME pipe + serving
// state under the lock — a teardown/replace/stall-kill that ran mid-read
// returns IoError (the frame is discarded; the reader's cleanup owns the
// outcome). Single-reader-per-family by design (the asr claim / one
// dispatcher owns each pipe), unchanged. Closing the handle with a pending
// overlapped read completes that read with an error, so the GracefulStop /
// reaper close paths stay race-free.
WorkerManager::WorkerRead WorkerManager::ReadFromWorker(const std::wstring& family,
                                                         std::string& json,
                                                         int timeout_ms) {
    HANDLE pipe = nullptr;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        WorkerHandle* w = impl_->FindLocked(family);
        if (!w || (w->state != WorkerState::Ready && w->state != WorkerState::Busy)) {
            return WorkerRead::IoError;
        }
        pipe = w->pipe;
    }
    const PipeRead rc = ReadPipeFrame(pipe, json, static_cast<DWORD>(timeout_ms));
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        WorkerHandle* w = impl_->FindLocked(family);
        if (!w || w->pipe != pipe ||
            (w->state != WorkerState::Ready && w->state != WorkerState::Busy)) {
            json.clear();
            return WorkerRead::IoError;
        }
    }
    switch (rc) {
        case PipeRead::Ok:      return WorkerRead::Ok;
        case PipeRead::Timeout: return WorkerRead::Timeout;
        case PipeRead::IoError: return WorkerRead::IoError;
    }
    return WorkerRead::IoError;
}

// REQ-049: non-blocking drain of frames the worker queued on the family pipe
// while the dispatcher was idle (heartbeat cadence, or a stray final left by a
// timed-out previous job). The manager mutex keeps the handle lifecycle
// serialized exactly like ReadFromWorker; timeout 0 makes each ReadPipeFrame a
// pure poll — PipeRead::Timeout means the pipe is empty and the drain is done.
// No per-frame log: heartbeats are routine traffic.
int WorkerManager::DrainWorkerPipe(const std::wstring& family, int max_frames) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    WorkerHandle* w = impl_->FindLocked(family);
    if (!w || (w->state != WorkerState::Ready && w->state != WorkerState::Busy) ||
        !w->pipe || w->pipe == INVALID_HANDLE_VALUE) {
        return 0;
    }
    int drained = 0;
    std::string json; // discarded: only the count matters to the caller
    while (drained < max_frames && ReadPipeFrame(w->pipe, json, 0) == PipeRead::Ok) {
        ++drained;
    }
    return drained;
}

// ---- spawn + handshake (design §1.1 rules 1-2 / §3.4) -----------------------
// REQ-044 (P4-3, item 5): the blocking ConnectNamedPipe wait + announce
// ReadPipeFrame below run while impl_->mu is held. This is a deliberate
// lock convoy, not a self-deadlock (std::mutex is non-recursive and no
// path from here re-enters mu: FindLocked / ClosePipeLocked /
// CloseProcessLocked are Impl:: "Locked"-suffix helpers that assume the
// caller already holds mu, and none of them take a lock_guard). It is
// safe because handles in Spawning state are not reachable concurrently:
// SendToWorker / ReadFromWorker early-return on
// state != Ready/Busy, and ReaperPass skips states other
// than Ready/Busy/Spawning, so w->pipe is only ever touched by
// this thread while the lock is held. The convoy only matters if M7 adds
// parallel multi-family worker spawns - revisit then with an explicit
// state-machine design before narrowing the lock scope.
WorkerHandle* WorkerManager::EnsureSpawned(const std::wstring& family) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    WorkerHandle* w = impl_->FindLocked(family);
    if (!w) return nullptr;

    if (w->state == WorkerState::Ready || w->state == WorkerState::Busy) return w;
    if (w->state == WorkerState::Unavailable) return nullptr; // wrong deployment: NO respawn

    // Backoff gate (R-4): a recent crash defers the respawn.
    const int64_t now = NowMs();
    if (w->state == WorkerState::Crashed && now < w->next_spawn_allowed_ms) {
        return nullptr;
    }

    if (w->exe_path.empty() ||
        ::GetFileAttributesW(w->exe_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Not deployed yet: unavailable WITHOUT counting a spawn failure
        // (the bootstrapper/installer owns deployment; design §V2-8).
        w->state = WorkerState::Unavailable;
        DIAG_F("ENGINEHOST/wmgr/003: worker exe missing; family unavailable (family=%ws)\n",
               family.c_str());
        return nullptr;
    }

    // Fresh per-spawn pipe + token (boot-scoped; nothing persisted).
    impl_->ClosePipeLocked(*w);
    w->pipe_name = MakeWorkerPipeName();
    if (!GenerateTokenHex32(w->token)) {
        DIAG_F("ENGINEHOST/wmgr/004: token generation failed (family=%ws)\n", family.c_str());
        w->state = WorkerState::Crashed;
        w->spawn_failures++;
        w->next_spawn_allowed_ms = NowMs() + BackoffDelayMs(w->spawn_failures);
        return nullptr;
    }

    SecurityDescriptorHolder sd;
    if (!BuildUserOnlySd(sd)) {
        DIAG_F("ENGINEHOST/wmgr/005: user-only SD failed (err=%lu)\n", ::GetLastError());
        w->state = WorkerState::Crashed;
        w->spawn_failures++;
        w->next_spawn_allowed_ms = NowMs() + BackoffDelayMs(w->spawn_failures);
        return nullptr;
    }
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd.sd;
    sa.bInheritHandle = FALSE;
    w->pipe = ::CreateNamedPipeW(
        w->pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,                      // exactly one worker client per pipe (§1.2)
        kPipeBufSize, kPipeBufSize, 0, &sa);
    if (w->pipe == INVALID_HANDLE_VALUE) {
        w->pipe = nullptr;
        DIAG_F("ENGINEHOST/wmgr/006: CreateNamedPipe failed (err=%lu)\n", ::GetLastError());
        w->state = WorkerState::Crashed;
        w->spawn_failures++;
        w->next_spawn_allowed_ms = NowMs() + BackoffDelayMs(w->spawn_failures);
        return nullptr;
    }

    // Spawn (hidden, same user, token on the command line — no file).
    impl_->CloseProcessLocked(*w);
    SpawnRequest req{w->exe_path, w->pipe_name, w->token};
    SpawnResult sr = impl_->spawn_fn(req, impl_->user);
    if (!sr.ok) {
        DIAG_F("ENGINEHOST/wmgr/007: spawn failed (family=%ws err=%lu)\n",
               family.c_str(), sr.last_error);
        impl_->ClosePipeLocked(*w);
        w->state = WorkerState::Crashed;
        w->spawn_failures++;
        w->next_spawn_allowed_ms = NowMs() + BackoffDelayMs(w->spawn_failures);
        return nullptr;
    }
    w->process = sr.process;
    w->state = WorkerState::Spawning;
    w->last_heartbeat_ms = NowMs();

    // ---- handshake phase 1: the worker connects to our pipe instance ----
    {
        OVERLAPPED ol = {};
        ol.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool connected = false;
        if (ol.hEvent) {
            const BOOL ok = ::ConnectNamedPipe(w->pipe, &ol);
            const DWORD err = ::GetLastError();
            if (!ok && err == ERROR_IO_PENDING) {
                DWORD dummy = 0;
                const DWORD wr = ::WaitForSingleObject(ol.hEvent,
                                                       wp::kWorkerSpawnConnectTimeoutMs);
                if (wr == WAIT_OBJECT_0) {
                    connected = ::GetOverlappedResult(w->pipe, &ol, &dummy, FALSE) != FALSE;
                } else {
                    ::CancelIoEx(w->pipe, &ol);
                }
            } else if (ok || err == ERROR_PIPE_CONNECTED) {
                connected = true; // the client already connected (spawn race)
            }
            ::CloseHandle(ol.hEvent);
        }
        if (!connected) {
            DIAG_F("ENGINEHOST/wmgr/008: worker never connected (family=%ws)\n", family.c_str());
            impl_->ClosePipeLocked(*w);
            TerminateProcessHandle(w->process);
            impl_->CloseProcessLocked(*w);
            w->state = WorkerState::Crashed;
            w->spawn_failures++;
            w->next_spawn_allowed_ms = NowMs() + BackoffDelayMs(w->spawn_failures);
            return nullptr;
        }
    }

    // ---- handshake phase 2: announce + token re-validation + family/abi/
    // protocol validation. Token re-validation (security audit finding 2,
    // M6 T6): the announce frame must echo the exact boot-scoped token we
    // issued on the command line, compared in constant time. A missing or
    // mismatched token means the connected party is NOT the worker we spawned
    // (same-user pipe spoofing) — tear the session; the family goes
    // Unavailable (a spoofed peer is a wrong deployment; no respawn).
    bool announced_ok = false;
    wp::WorkerManifest announced;
    {
        std::string json;
        const PipeRead rc = ReadPipeFrame(w->pipe, json, wp::kWorkerSpawnConnectTimeoutMs);
        wp::AnnounceMsg am;
        if (rc == PipeRead::Ok && wp::ParseAnnounce(json, am)) {
            if (!wp::AnnounceTokenMatches(am.token, w->token)) {
                DIAG_F("ENGINEHOST/wmgr/013: announce token mismatch (family=%ws); tearing the session (audit finding 2)\n",
                       family.c_str());
                w->state = WorkerState::Unavailable;
                impl_->ClosePipeLocked(*w);
                TerminateProcessHandle(w->process);
                impl_->CloseProcessLocked(*w);
                return nullptr;
            }
            announced = am.manifest;
            const wp::AnnounceStatus st = ValidateHandshake(announced);
            if (st == wp::AnnounceStatus::Ok) {
                announced_ok = true;
            } else {
                // §3.4: a mismatch is a WRONG DEPLOYMENT — unavailable + log,
                // NO respawn (respawning cannot fix a bad binary).
                DIAG_F("ENGINEHOST/wmgr/009: announce rejected (%s announced_family=%s)\n",
                       wp::AnnounceStatusToString(st).data(), announced.family.c_str());
                w->state = WorkerState::Unavailable;
                impl_->ClosePipeLocked(*w);
                TerminateProcessHandle(w->process);
                impl_->CloseProcessLocked(*w);
                return nullptr;
            }
        } else {
            DIAG_F("ENGINEHOST/wmgr/010: announce unreadable (rc=%d family=%ws)\n",
                   static_cast<int>(rc), family.c_str());
        }
    }
    if (!announced_ok) {
        impl_->ClosePipeLocked(*w);
        impl_->CloseProcessLocked(*w);
        w->state = WorkerState::Crashed;
        w->spawn_failures++;
        w->next_spawn_allowed_ms = NowMs() + BackoffDelayMs(w->spawn_failures);
        return nullptr;
    }

    w->manifest = announced;
    w->spawn_failures = 0; // a clean handshake resets the crash streak
    w->state = WorkerState::Ready;
    DIAG_LOG("ENGINEHOST", "wmgr/011: worker ready (family=%ws engine=%s/%s abi=%d proto=%d-%d)",
             family.c_str(), w->manifest.engine.c_str(), w->manifest.engine_version.c_str(),
             w->manifest.abi_version, w->manifest.protocol_min, w->manifest.protocol_max);
    return w;
}

// ---- M-2 graceful stop (tech gate order) ------------------------------------
bool WorkerManager::GracefulStop(WorkerHandle& w, int timeout_ms) {
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!w.pipe) {
            // Nothing to order down: just finalize the process side.
            impl_->CloseProcessLocked(w);
            w.state = WorkerState::Stopped;
            return true;
        }
        w.state = WorkerState::Stopped; // refuse new dispatch immediately
    }
    // M-2 step 1: send shutdown (one synchronous frame write).
    const bool sent = WritePipeFrame(w.pipe, wp::BuildShutdown()) == FrameWriteResult::Ok;
    if (!sent) {
        // Pipe already dead: the worker cannot ack; fall through to the wait
        // (the done_event fires on the process exit we are waiting for).
        DIAG_F("ENGINEHOST/wmgr/012: shutdown write failed; waiting for exit (family=%ws)\n",
               w.family.c_str());
    }
    // M-2 step 2: wait for shutdown_ack OR pipe EOF (worker's last frame) OR
    // the process exiting — ALL prove no frame is in flight anymore.
    const int64_t deadline = NowMs() + timeout_ms;
    bool acked = false;
    while (NowMs() < deadline) {
        std::string json;
        const PipeRead rc = ReadPipeFrame(w.pipe, json, 250);
        if (rc == PipeRead::Ok) {
            if (wp::ParseShutdownAck(json)) { acked = true; break; }
            continue; // an in-flight event frame (e.g. a final) — consume it
        }
        if (rc == PipeRead::IoError) break;      // EOF: worker is done/exiting
        if (!sent) break; // the pipe was already dead when we started
        // Timeout: keep polling until the deadline (a slow Unload is legal).
    }
    // M-2 step 3: ONLY NOW close/disconnect the pipe (nothing in flight).
    // If the process still lives after the window, escalate to kill so an
    // unresponsive worker cannot outlive the orchestrator.
    bool killed = false;
    if (w.process) {
        const DWORD wr = ::WaitForSingleObject(w.process, 2000);
        if (wr != WAIT_OBJECT_0) {
            DIAG_F("ENGINEHOST/wmgr/013: worker did not exit after shutdown; terminating (family=%ws acked=%d)\n",
                   w.family.c_str(), acked ? 1 : 0);
            TerminateProcessHandle(w.process);
            killed = true;
        }
        impl_->close_fn(w.process, impl_->user);
        w.process = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->ClosePipeLocked(w);
    }
    DIAG_LOG("ENGINEHOST", "wmgr/014: family stopped (family=%ws acked=%d killed=%d)",
             w.family.c_str(), acked ? 1 : 0, killed ? 1 : 0);
    return acked || killed;
}

// ---- reaper (design §1.1 rule 3) --------------------------------------------
int WorkerManager::ReaperPass(int64_t now_ms) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    int crashes = 0;
    for (auto& w : impl_->workers) {
        if (w.state != WorkerState::Ready && w.state != WorkerState::Busy &&
            w.state != WorkerState::Spawning) {
            continue; // Stopped/Crashed/Unavailable have nothing to watch
        }
        if (!w.process) continue; // seam-injected state without a handle

        const DWORD wr = ::WaitForSingleObject(w.process, 0);
        if (wr != WAIT_OBJECT_0) {
            // Heartbeat staleness judge (§1.2: 15 s timeout) — only when the
            // pipe is ALREADY gone (a silent-but-piped worker is handled by
            // the orchestrator's ordered GracefulStop, not by the reaper).
            if (!w.pipe &&
                now_ms - w.last_heartbeat_ms > wp::kWorkerHeartbeatTimeoutMs) {
                w.state = WorkerState::Crashed;
                w.spawn_failures++;
                w.next_spawn_allowed_ms = now_ms + BackoffDelayMs(w.spawn_failures);
                ++crashes;
                DIAG_F("ENGINEHOST/wmgr/015: heartbeat timeout (family=%ws)\n",
                       w.family.c_str());
            }
            continue;
        }
        // ---- crash finalization ----
        DWORD exit_code = 0;
        ::GetExitCodeProcess(w.process, &exit_code);
        impl_->ClosePipeLocked(w);
        impl_->CloseProcessLocked(w);
        w.state = WorkerState::Crashed;
        w.spawn_failures++;
        w.next_spawn_allowed_ms = now_ms + BackoffDelayMs(w.spawn_failures);
        ++crashes;
        // 장애 격리 (§V2-3): this family's in-flight jobs are ended here —
        // T4's dispatcher observes state==Crashed and answers engine_failed/
        // unavailable. Other families and other connections are untouched.
        w.jobs_failed_on_crash++;
        DIAG_F("ENGINEHOST/wmgr/016: worker crashed (family=%ws exit=%lu streak=%d backoff_ms=%d)\n",
               w.family.c_str(), static_cast<unsigned long>(exit_code),
               w.spawn_failures, BackoffDelayMs(w.spawn_failures));
    }
    return crashes;
}

void WorkerManager::StartReaper() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->reaper_thread.joinable()) return;
    impl_->reaper_stop.store(false, std::memory_order_release);
    impl_->reaper_thread = std::thread([this] {
        for (;;) {
            const DWORD wr = ::WaitForSingleObject(impl_->reaper_wake,
                                                   wp::kWorkerReaperPollMs);
            if (impl_->reaper_stop.load(std::memory_order_acquire)) return;
            (void)wr;
            // One pass per 250 ms tick (design §1.1: done_event 250 ms poll).
            (void)ReaperPass(NowMs());
        }
    });
}

void WorkerManager::StopReaper() {
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!impl_->reaper_stop.exchange(true, std::memory_order_acq_rel)) {
            if (impl_->reaper_wake) ::SetEvent(impl_->reaper_wake);
        }
    }
    // Join OUTSIDE the lock (the reaper thread takes the same lock per pass).
    if (impl_->reaper_thread.joinable()) impl_->reaper_thread.join();
}

void WorkerManager::ShutdownAll() {
    StopReaper();
    std::vector<std::wstring> targets;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        for (auto& w : impl_->workers) {
            if (w.state == WorkerState::Ready || w.state == WorkerState::Busy ||
                w.state == WorkerState::Spawning) {
                targets.push_back(w.family);
            }
        }
    }
    // REQ-043 (M6 fix, session 260918_0001): GracefulStop must run WITHOUT
    // the manager mutex — it takes the same lock in its step-1 scope and
    // again in its step-3 scope. With the pre-fix code the mutex was already
    // held here, so the GracefulStop lock acquisitions self-deadlocked; the
    // process then hit the CRT's deadlock timeout fail-fast (exit 0xc0000409,
    // observed 4/5 reproduction runs after a real worker-spawned translate)
    // instead of the designed exit 0. Each GracefulStop call now fetches its
    // own handle under a brief lock, stops the child (blocking <= its
    // timeout), then the orphan guard re-locks for the final sweep (design
    // §1.1 rule 5 unchanged).
    for (const auto& family : targets) {
        WorkerHandle* w = nullptr;
        {
            std::lock_guard<std::mutex> lk(impl_->mu);
            w = impl_->FindLocked(family);
        }
        if (w) (void)GracefulStop(*w);
    }
    // Orphan guard: anything still standing is killed (design §1.1 rule 5).
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        for (auto& w : impl_->workers) {
            impl_->ClosePipeLocked(w);
            if (w.process) {
                TerminateProcessHandle(w.process);
                impl_->close_fn(w.process, impl_->user);
                w.process = nullptr;
            }
            w.state = WorkerState::Stopped;
        }
    }
}

} // namespace host_v2
} // namespace emebalachat
