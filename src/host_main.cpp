// ---------------------------------------------------------------------------
// Emebala.Engine.exe — REQ-043 shared inference host (plan §4, §5.1, §6, §8),
// evolved into the v2 ORCHESTRATOR (M6 T4, plan §V2-4, design §1.1/§1.4).
//
// A Win32 GUI-subsystem background process (no console, no tray, no window)
// that routes every Emebala app's inference requests to family worker
// processes (Emebalachat.Engine.ggml-translate.exe — the second frozen
// contract) over local named pipes. See
// plans/emebala-engine-host-shared-inference.md — the v1 wire contract is
// FROZEN and still served byte-identically (§V2-4.7).
//
//   * DUAL PIPES (REQ-002, §V2-4.1): the canonical versionless
//     \\.\pipe\emebala-engine (4 instances) AND the transition alias
//     \\.\pipe\emebala-engine-v1 (4 instances — the v1 §4.1 frozen name,
//     enginehost::kPipeName, UNMODIFIED) run concurrently, 8 ConnectionLoops
//     total sharing ONE g_active_connections counter (idle-exit accuracy).
//     hello.protocol branches: 1 -> RunSessionV1 (§4.4 verbatim), 2 -> the
//     v2 profile (sessions/scheduler/health); any other -> version_mismatch
//     (the v1 rule, frozen).
//   * Boot-rotating 128-bit token hex(32) in
//     %LOCALAPPDATA%\Emebala\Common\engine\token, user-only ACL, deleted at
//     exit (RAII); hello mismatch -> {"op":"error","code":"unauthorized"} + close (§4.2).
//   * Frames [u32 LE][UTF-8 JSON], 1 MiB cap; over-cap / unknown op ->
//     bad_request + close (§4.3).
//   * v1 profile: the FROZEN global FIFO job queue, depth > 8 -> immediate
//     busy (§4.4/§4.5). The queue now FEEDS THE WORKER PROCESS instead of an
//     in-process llama call (T4: InferenceLoop deleted; the ggml-translate
//     worker owns LocalInferenceEngine — design §1.4). Response semantics are
//     unchanged: ok / engine_failed / model_missing / timeout / busy, cancel
//     op -> worker abort frame -> status=timeout (§4.4, D-2 chain).
//   * v2 profile (protocol 2): host_v2_session (§V2-4.3), host_v2_scheduler
//     (§V2-4.6 priority/drop_eligible/deadline), welcome v2 with models[] +
//     health (REQ-008, §V2-4.7 example — unknown fields ignored by receivers).
//   * cancel op -> abort frame to the worker -> the worker's cancel_flag
//     (address-stable atomic) unwinds llama via the REQ-R16 abort callback;
//     the job answers status=timeout (§4.4). A watchdog thread enforces the
//     per-request deadline the same way.
//   * Model: %LOCALAPPDATA%\Emebala\Common\models\Hy-MT2-1.8B-Q8_0.gguf —
//     the FILE PRESENCE probe stays here for the v1 fast path (§8
//     model_missing without queueing); load/verify happens inside the worker
//     (.sha256ok marker cache + CUDA->CPU fallback moved with the engine).
//   * Idle exit (§4.4): 0 active connections + no scheduler backlog + no open
//     session + workers idle, after idle_exit_ms (default 600000; hidden
//     --idle-exit-ms N test hook) -> workers GracefulStop (M-2 order) THEN
//     delete the token, exit 0; the next client request respawns it. No
//     worker is ever orphaned (design §1.1 rule 4 / §10).
//   * Shape-only logging through the existing diag_logger (OFF by default —
//     the host has no config file and never enables the file sink; DIAG_F's
//     stderr mirror is unconditional). User text NEVER appears in any log.
//   * Startup mirrors main.cpp: SetDllDirectoryW(L""), single-instance mutex,
//     diag::Init, EnsureVulkanGuard (delay-load SEH guard).
// ---------------------------------------------------------------------------

// NOMINMAX must precede EVERY include in this TU: <windows.h> arrives via
// engine.hpp first, and without the macro std::min/std::max below would be
// preprocessed into garbage by the min/max macros (config.cpp precedent uses
// ternaries instead; a single-TU guard is cleaner for the new host code).
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "engine.hpp"               // kExpectedModelSha256 / kPinnedModelFilename
#include "engine_host_protocol.hpp" // frozen wire protocol (pure helpers)
#include "config.hpp"               // NormalizeLanguageCode / FindLanguageByCode (prompt parity)
#include "diag_logger.hpp"          // shape-only diagnostics
#include "unicode_utils.hpp"        // ToUtf8 / ToUtf16
#include "vulkan_guard.hpp"         // P5-F1 driverless-machine Vulkan guard
// REQ-043 (M6 T4): the v2 orchestrator modules (design §1.1/§1.3). The
// in-process engine include above is GONE — inference moved to the worker
// process (T4: InferenceLoop deleted; LocalInferenceEngine lives in
// ggml_translate_worker.cpp now).
#include "host_v2_session.hpp"          // §V2-4.3 session table
#include "host_v2_scheduler.hpp"        // §V2-4.6 scheduler queue
#include "host_v2_health.hpp"           // REQ-008 health counters
#include "host_v2_worker_manager.hpp"   // §V2-3 worker lifecycle (T3)
#include "worker_protocol.hpp"          // second frozen contract (frames)
#include "engine_host_registry.hpp"     // registry.json (model_id/profile resolution)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h> // BCryptGenRandom (boot token)
#include <sddl.h>   // SDDL descriptor builder (pipe + token ACLs)
#include <shlobj.h> // SHGetKnownFolderPath

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>   // _wtoll
#include <cwchar>    // wcsstr / wcslen
#include <deque>
#include <filesystem>
#include <functional> // std::ref (DispatcherLoop/DispatcherV2Loop thread args)
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace emebalachat {
namespace host {

// The boot token (§4.2): written to the token file before any connection
// thread starts, read-only afterwards. Compared against every hello.
std::string g_token;

namespace {

// ---- constants --------------------------------------------------------------
constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\EmebalaEngine_Host_SingleInstance";
constexpr wchar_t kEngineDirRel[] = L"Emebala\\Common\\engine";
constexpr wchar_t kModelDirRel[] = L"Emebala\\Common\\models";
constexpr wchar_t kTokenFilename[] = L"token";
constexpr size_t kPipeInstanceCount = 4; // §4.1 minimum (PER pipe name)
constexpr int kIdlePollMs = 250;
constexpr int64_t kJobWatchdogPollMs = 50;

// REQ-043 (M6 T4, REQ-002, plan §V2-4.1): the canonical VERSIONLESS pipe
// name. The v1 frozen name stays in enginehost::kPipeName (UNTOUCHED — this
// orchestrator keeps serving the alias simultaneously). Only THIS new
// constant was added; no existing constant changed.
constexpr const char* kPipeNameVersionless = "\\\\.\\pipe\\emebala-engine";

// REQ-043 (M6 T4): the worker family this orchestrator routes to (T3
// contract; registry.json entries resolve the same family string).
constexpr wchar_t kWorkerFamilyTranslate[] = L"ggml-translate";

// REQ-043 (M6 T4): how long a v1 job waits for its worker event before the
// dispatcher itself declares timeout (safety net BEYOND the client's
// timeout_ms watchdog — a hung worker pipe must not pin a queue slot past
// the watchdog's own abort; 5 s of grace covers pipe turnaround).
constexpr int kWorkerAnswerGraceMs = 5000;

// ---- activity / shutdown ----------------------------------------------------
std::atomic<int64_t> g_last_activity_ms{0};
// REQ-043 (M6 T4, REQ-002): ONE counter shared by BOTH pipe sets — the
// idle-exit judgment sums every live connection regardless of the pipe it
// arrived on (design §1.1: "양 세트가 하나의 g_active_connections를 공유").
std::atomic<int> g_active_connections{0};
std::atomic<bool> g_shutdown{false};
HANDLE g_stop_event = nullptr; // manual-reset: wakes connection threads for exit

// ---- v2 orchestrator singletons (M6 T4, design §1.1) ------------------------
// Health (REQ-008): local atomics only; exposed via the v2 welcome.
host_v2::HealthCounters g_health;
// Session table (§V2-4.3): M6 serves "translate" only — anything else fails
// to open (unavailable, §V2-4.5).
host_v2::SessionTable g_sessions{std::vector<std::string>{"translate"}};
// Scheduler (§V2-4.6): the v1 profile path does NOT use it (frozen
// immediate-busy queue above); protocol-2 requests enqueue here.
host_v2::Scheduler g_scheduler;
// The registry (§3.1) resolves model_id/profile at boot; absent file = the
// v1 hardcoded-path default (backward compatible, task item 4).
engine_host_registry::Registry g_registry;
bool g_registry_loaded = false;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void TouchActivity() { g_last_activity_ms.store(NowMs(), std::memory_order_release); }

// ---- paths ------------------------------------------------------------------
std::wstring LocalAppDataDir() {
    PWSTR known = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known)) && known) {
        std::wstring out(known);
        ::CoTaskMemFree(known);
        return out;
    }
    return {};
}

std::wstring EngineDir() {
    const std::wstring lad = LocalAppDataDir();
    return lad.empty() ? std::wstring{} : lad + L"\\" + kEngineDirRel;
}

std::wstring TokenPath() {
    const std::wstring dir = EngineDir();
    return dir.empty() ? std::wstring{} : dir + L"\\" + kTokenFilename;
}

std::wstring ModelPathW() {
    const std::wstring lad = LocalAppDataDir();
    if (lad.empty()) return {};
    return lad + L"\\" + kModelDirRel + L"\\" +
           std::wstring(kPinnedModelFilename.begin(), kPinnedModelFilename.end());
}

std::string ModelPathUtf8() {
    const std::wstring w = ModelPathW();
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n) - 1, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

bool ModelFileExists() {
    const std::wstring w = ModelPathW();
    if (w.empty()) return false;
    const DWORD attrs = ::GetFileAttributesW(w.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// ---- user-only security descriptor (§4.1 pipe ACL / §4.2 token file ACL) ----
// Builds "D:P(A;;GA;;;<owner-sid>)" — the P flag is SE_DACL_PROTECTED (inheritance
// broken), the only ACE grants GENERIC_ALL to the creating user. Every other
// principal (other users, services) is refused by the kernel before any of
// our code runs.
struct SecurityDescriptorHolder {
    PSECURITY_DESCRIPTOR sd = nullptr;
    SecurityDescriptorHolder() = default;
    ~SecurityDescriptorHolder() { if (sd) ::LocalFree(sd); }
    SecurityDescriptorHolder(const SecurityDescriptorHolder&) = delete;
    SecurityDescriptorHolder& operator=(const SecurityDescriptorHolder&) = delete;
};

bool BuildUserOnlySd(SecurityDescriptorHolder& holder) {
    HANDLE hToken = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return false;
    }
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
    if (!::ConvertSidToStringSidW(user->User.Sid, &sidStr) || !sidStr) {
        return false;
    }
    const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sidStr + L")";
    ::LocalFree(sidStr);
    return ::ConvertStringSecurityDescriptorToSecurityDescriptorW(
               sddl.c_str(), SDDL_REVISION_1, &holder.sd, nullptr) != FALSE;
}

// ---- boot token (§4.2) ------------------------------------------------------
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

// Writes (replacing) the token file with the user-only ACL. §4.2: a stale
// file from a crashed run is replaced at boot.
bool WriteTokenFile(const std::wstring& path, const std::string& token) {
    SecurityDescriptorHolder sd;
    if (!BuildUserOnlySd(sd)) return false;
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd.sd;
    sa.bInheritHandle = FALSE;
    const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = ::WriteFile(h, token.data(), static_cast<DWORD>(token.size()), &written, nullptr);
    ::CloseHandle(h);
    return ok && written == token.size();
}

// REQ-043 (§4.2): the token file dies with the process — RAII so every exit
// path (idle shutdown, startup failure after this point) cleans up.
struct TokenFileGuard {
    std::wstring path;
    TokenFileGuard() = default;
    explicit TokenFileGuard(std::wstring p) : path(std::move(p)) {}
    ~TokenFileGuard() {
        if (!path.empty()) {
            ::DeleteFileW(path.c_str()); // best effort; a stale file is replaced next boot
        }
    }
    TokenFileGuard(const TokenFileGuard&) = delete;
    TokenFileGuard& operator=(const TokenFileGuard&) = delete;
};

// ---- job queue (§4.5) -------------------------------------------------------
struct Connection; // fwd

struct Job {
    uint64_t id = 0;
    std::string src;
    std::string tgt;
    std::string text;
    int timeout_ms = enginehost::kDefaultTranslateTimeoutMs;
    Connection* conn = nullptr;
};

class JobQueue {
public:
    // §4.4: depth > 8 -> refuse WITHOUT enqueueing (the caller answers busy).
    bool Push(const Job& job) {
        std::lock_guard<std::mutex> lk(mu_);
        if (q_.size() > enginehost::kMaxQueueDepth) return false;
        q_.push_back(job);
        cv_.notify_one();
        return true;
    }
    // Blocks until a job is available or shutdown. False = stop.
    bool Pop(Job& out) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
        if (stop_) return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }
    void StopAll() {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> q_;
    bool stop_ = false;
};

JobQueue g_queue;

// ---- in-flight job bookkeeping (cancel §4.4 + per-job deadline) -------------
struct CurrentJob {
    std::mutex mu;
    std::condition_variable cv;
    uint64_t id = 0;
    bool done = true;
    int64_t deadline_ms = 0;
};
CurrentJob g_current;
// The abort flag's ADDRESS is handed to llama's abort/progress callbacks at
// context creation and stays stable for the engine's lifetime; values toggle
// per job (REQ-R16 seam, reused for the §4.4 cancel op).
std::atomic<bool> g_abort{false};

// ---- connection -------------------------------------------------------------
struct Connection {
    HANDLE pipe = nullptr;
    // REQ-043: duplicated server end used EXCLUSIVELY for frame writes. The
    // inference worker answers translate while the connection loop sits in a
    // pending ReadFile; a write on the SAME handle serializes behind that
    // read and deadlocks (result never delivered). Separate handle = the
    // classic full-duplex named-pipe fix. Created per accepted session.
    HANDLE write_pipe = nullptr;
    int index = 0;
    OVERLAPPED accept_ol = {};
    // Writer state: results are pushed by the inference worker and the
    // session thread; one mutex serializes frame writes per connection.
    std::mutex write_mu;

    bool WriteFrame(std::string_view json) {
        std::string frame;
        frame.reserve(4 + json.size());
        const uint32_t len = static_cast<uint32_t>(json.size());
        for (unsigned i = 0; i < 4; ++i) {
            frame.push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
        }
        frame.append(json);
        std::lock_guard<std::mutex> lk(write_mu);
        OVERLAPPED ol = {};
        ol.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ol.hEvent) return false;
        DWORD written = 0;
        // REQ-043: write through the duplicated handle (never `pipe` — see the
        // Connection member note); fall back only if the duplication failed.
        HANDLE w = write_pipe ? write_pipe : pipe;
        BOOL ok = ::WriteFile(w, frame.data(), static_cast<DWORD>(frame.size()), &written, &ol);
        if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
            // REQ-043: the completion wait MUST target the SAME handle the IO
            // was issued on — GetOverlappedResult matches pending IO per handle.
            ok = ::GetOverlappedResult(w, &ol, &written, TRUE);
        }
        ::CloseHandle(ol.hEvent);
        return ok && written == frame.size();
    }

    void SendResult(uint64_t id, enginehost::HostStatus status, std::string text = {}) {
        enginehost::ResultMsg m;
        m.id = id;
        m.status = status;
        m.text = std::move(text);
        if (!WriteFrame(enginehost::BuildResult(m))) {
            DIAG_LOG("ENGINEHOST", "conn/%03d: result write failed (client gone)", index);
        }
    }
    void SendError(std::string_view code) {
        if (!WriteFrame(enginehost::BuildError(code))) {
            DIAG_LOG("ENGINEHOST", "conn/%03d: error write failed (client gone)", index);
        }
    }
    // REQ-043 terminal-error path (§4.2/§4.3 handshake rejections and protocol
    // violations): the error frame MUST reach the client before the
    // disconnect. DisconnectNamedPipe purges frames the client app has not
    // consumed yet (measured: even a 250 ms drain window loses the race), so
    // the close is CLIENT-DRIVEN instead: after the error we wait (bounded)
    // for the client to close, which the contract clients do immediately
    // after reading a terminal error. Only then does ConnectionLoop run
    // DisconnectNamedPipe, when the pipe is already broken and nothing can be
    // lost. A non-closing peer parks this connection thread for the window,
    // which §4.1's 4-instance pool absorbs.
    void SendErrorThenClose(std::string_view code) {
        SendError(code);
        WaitForClientClose(10000);
    }
    // Bounded wait for the peer to close: one overlapped read that completes
    // (with ERROR_BROKEN_PIPE) once the client closes. Returns immediately on
    // broken pipe; gives up after timeout_ms (cancelling the pending read) so
    // a rude peer cannot pin the thread forever.
    void WaitForClientClose(DWORD timeout_ms) {
        char scratch[64];
        OVERLAPPED ol = {};
        ol.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ol.hEvent) return;
        DWORD read = 0;
        BOOL ok = ::ReadFile(pipe, scratch, sizeof(scratch), &read, &ol);
        if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
            if (::WaitForSingleObject(ol.hEvent, timeout_ms) == WAIT_TIMEOUT) {
                ::CancelIoEx(pipe, &ol);
                ::CloseHandle(ol.hEvent);
                return;
            }
            ok = ::GetOverlappedResult(pipe, &ol, &read, FALSE);
        }
        // Any outcome (broken pipe = client closed; stray data = rude client)
        // leads to the caller returning and ConnectionLoop disconnecting.
        ::CloseHandle(ol.hEvent);
    }
    void SendWelcome() {
        enginehost::WelcomeMsg m;
        m.protocol = enginehost::kProtocolVersion;
        m.model = std::string(enginehost::kModelDisplayName);
        // The pin comes from engine.hpp (single source of truth; the
        // self-contained client carries a verified-in-sync copy).
        m.model_sha256 = std::string(kExpectedModelSha256);
        m.capabilities = {"translate"};
        if (!WriteFrame(enginehost::BuildWelcome(m))) {
            DIAG_LOG("ENGINEHOST", "conn/%03d: welcome write failed (client gone)", index);
        }
    }

    // REQ-043 (M6 T4, REQ-008, plan §V2-4.7 example): the v2 welcome. Fields
    // receivers do not know are IGNORED (§V2-12-2), so the extended shape is
    // additive; the v1 welcome above stays byte-compatible (NO fields added
    // there — the frozen contract).
    // health (R-5 option b): "호스트가 관측한" ratios only — no client-side
    // fallback is estimated, no network egress (local counters, design §10).
    void SendWelcomeV2() {
        std::string models = "[";
        if (g_registry_loaded && !g_registry.empty()) {
            bool first_model = true;
            for (const auto& m : g_registry.models) {
                const bool serves_translate =
                    std::any_of(m.capabilities.begin(), m.capabilities.end(),
                                [](const std::string& c) { return c == "translate"; });
                if (!serves_translate) continue;
                if (!first_model) models += ',';
                first_model = false;
                models += std::string("{\"id\":\"") + enginehost::JsonEscape(m.id) +
                          "\",\"family\":\"" + enginehost::JsonEscape(m.family) +
                          "\",\"capabilities\":[\"translate\"]";
                // "default": the first registry translate model wins (the
                // bundle ships exactly one; §V2-4.7 example shape).
                models += ",\"default\":";
                models += (m.id == g_registry.models.front().id) ? "true" : "false";
                models += "}";
            }
        }
        models += ']';
        char ratio_buf[32];
        snprintf(ratio_buf, sizeof(ratio_buf), "%.3f", g_health.SessionSuccessRatio());
        std::string health = std::string("{\"session_success_ratio\":") + ratio_buf;
        snprintf(ratio_buf, sizeof(ratio_buf), "%.3f", g_health.FallbackRatio());
        health += std::string(",\"fallback_ratio\":") + ratio_buf + "}";
        const std::string welcome =
            std::string("{\"op\":\"welcome\",\"protocol\":2") +
            ",\"models\":" + models +
            ",\"capabilities\":[\"translate\"]" +
            ",\"health\":" + health + "}";
        if (!WriteFrame(welcome)) {
            DIAG_LOG("ENGINEHOST", "conn/%03d: welcome v2 write failed (client gone)", index);
        }
    }
};

// ---- watchdog: per-job deadline -> abort -> status=timeout (§4.4) -----------
void WatchdogLoop() {
    for (;;) {
        std::unique_lock<std::mutex> lk(g_current.mu);
        if (g_shutdown.load(std::memory_order_acquire)) return;
        if (g_current.done) {
            g_current.cv.wait(lk, [] {
                return g_shutdown.load(std::memory_order_acquire) || !g_current.done;
            });
            continue;
        }
        const int64_t now = NowMs();
        if (now >= g_current.deadline_ms) {
            // The worker observes the flag between decode steps / inside the
            // abort callback and answers status=timeout.
            g_abort.store(true, std::memory_order_release);
        }
        const int64_t wait_ms =
            std::max<int64_t>(1, std::min<int64_t>(kJobWatchdogPollMs, g_current.deadline_ms - now));
        g_current.cv.wait_for(lk, std::chrono::milliseconds(wait_ms));
    }
}

// ---- dispatcher: the FROZEN v1 queue now feeds the ggml-translate WORKER
// PROCESS (M6 T4, design §1.4; the old in-process InferenceLoop is DELETED).
// Response semantics are byte-identical to the old loop (§4.4):
//   aborted/timeout -> status=timeout, load failure -> model_missing,
//   empty decode -> engine_failed, success -> status=ok + text.
// One dispatcher thread: pops the queue, sends a worker job frame, waits for
// the worker's single terminal event (final|error), answers the client,
// marks the manager not-busy. The §4.5 single-context serialization contract
// is preserved: ONE job in flight at a time (registry max_sessions=1 for
// ggml-translate, enforced by SetBusy Ready<->Busy on the single family).
void DispatcherLoop(host_v2::WorkerManager& wmgr) {
    const std::wstring family(kWorkerFamilyTranslate);
    namespace wp = emebalachat::workerproto;
    for (;;) {
        Job job;
        if (!g_queue.Pop(job)) break; // shutdown

        // v1 §8 fast-path parity: the file-presence probe stays in the host,
        // so a missing model answers model_missing WITHOUT a worker round
        // trip (the worker would answer the same; the fast path keeps the
        // pre-T4 response latency for a fresh machine).
        if (!ModelFileExists()) {
            // §8: the process stays alive and answers model_missing.
            job.conn->SendResult(job.id, enginehost::HostStatus::ModelMissing);
            g_health.RecordJob(host_v2::HealthOutcome::Failure);
            TouchActivity();
            continue;
        }

        // EnsureSpawned covers spawn + announce handshake + backoff gating
        // (T3). Failure = the family cannot serve (missing exe, spawn fail,
        // handshake mismatch, backoff). The frozen v1 §4.4 status set has no
        // "unavailable"; the frozen nearest equivalent for a deployment that
        // cannot serve the MODEL is model_missing (a 0.10.1 v1 client falls
        // back exactly as it does on model_missing today — §V2-4.7 keeps v1
        // semantics intact; the v2 profile surfaces proper unavailable via
        // the scheduler path below).
        host_v2::WorkerHandle* w = wmgr.EnsureSpawned(family);
        if (!w) {
            job.conn->SendResult(job.id, enginehost::HostStatus::ModelMissing);
            g_health.RecordJob(host_v2::HealthOutcome::Fallback);
            TouchActivity();
            continue;
        }

        // Clear the per-job abort flag BEFORE publishing the job as current,
        // so a cancel that lands mid-swap can never abort the next job (the
        // v1 InferenceLoop discipline, now across the process boundary).
        g_abort.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(g_current.mu);
            g_current.id = job.id;
            g_current.done = false;
            g_current.deadline_ms = NowMs() + job.timeout_ms;
        }
        g_current.cv.notify_all();

        // Prompt parity lives INSIDE the worker (it resolves name_en +
        // sampling defaults from the same helpers). The host forwards the
        // raw request fields only — the §V2-3 job frame contract. The v1
        // request id IS the worker job id (1:1, id-matching preserved).
        wp::JobMsg jm;
        jm.job = job.id;
        jm.session = 0; // v1 profile jobs are sessionless (§1.2 one-shot shortcut)
        jm.src = job.src;
        jm.tgt = job.tgt;
        jm.text = job.text;
        // No sampling member: the worker's shipped defaults (0.0/0.6/20/1.05)
        // apply — protocol v1 has no tuning channel (unchanged, frozen).
        wmgr.SetBusy(family, true);
        const bool sent = wmgr.SendToWorker(family, wp::BuildJob(jm));
        if (!sent) {
            // Dead pipe: the manager already marked the family Crashed; the
            // reaper owns the respawn. Answer NOW — never leave a v1 id
            // unmatched (§4.4: every request gets exactly one result).
            job.conn->SendResult(job.id, enginehost::HostStatus::EngineFailed);
            g_health.RecordJob(host_v2::HealthOutcome::Failure);
            wmgr.SetBusy(family, false);
            {
                std::lock_guard<std::mutex> lk(g_current.mu);
                g_current.done = true;
                g_current.id = 0;
            }
            g_current.cv.notify_all();
            TouchActivity();
            continue;
        }

        // ---- await the worker's terminal event (final | error) ----
        // The worker answers exactly ONE event per job (T3 contract). The
        // wait is bounded by the client deadline + grace; the watchdog sets
        // g_abort on expiry and the cancel op sets it too — both map to the
        // frozen status=timeout. A worker crash mid-job surfaces as a pipe
        // IO error (EngineFailed) or the manager losing the family.
        enginehost::HostStatus status = enginehost::HostStatus::EngineFailed;
        std::string out_text;
        bool answered = false;
        const int64_t give_up_ms = g_current.deadline_ms + kWorkerAnswerGraceMs;
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(g_current.mu);
                if (g_current.done) break;
            }
            if (g_shutdown.load(std::memory_order_acquire)) break;
            if (!wmgr.EnsureSpawned(family)) break; // family died, cannot serve
            std::string json;
            const auto rc = wmgr.ReadFromWorker(family, json, 250);
            if (rc == host_v2::WorkerManager::WorkerRead::Ok) {
                wp::EventMsg ev;
                if (!wp::ParseEvent(json, ev)) {
                    // Malformed frame on the 난부 pipe: protocol violation ->
                    // job fails (fail-closed, §V2-12-2).
                    status = enginehost::HostStatus::EngineFailed;
                    answered = true;
                    break;
                }
                if (ev.kind == wp::EventKind::Final) {
                    status = enginehost::HostStatus::Ok;
                    out_text = ev.text;
                    answered = true;
                    break;
                }
                if (ev.kind == wp::EventKind::Error) {
                    // §4.4 mapping (T3 contract, unchanged):
                    //   timeout       -> status=timeout (cancel/watchdog)
                    //   model_missing -> status=model_missing
                    //   engine_failed -> status=engine_failed
                    if (ev.code == "timeout") {
                        status = enginehost::HostStatus::Timeout;
                    } else if (ev.code == "model_missing") {
                        status = enginehost::HostStatus::ModelMissing;
                    } else {
                        status = enginehost::HostStatus::EngineFailed;
                    }
                    answered = true;
                    break;
                }
                // partial/token/eos: not emitted by the one-shot translate
                // worker; consume and keep waiting.
                continue;
            }
            if (rc == host_v2::WorkerManager::WorkerRead::IoError) {
                status = enginehost::HostStatus::EngineFailed; // worker gone
                answered = true;
                break;
            }
            // 250 ms read timeout: re-check abort/watchdog/deadline.
            const bool aborted = g_abort.load(std::memory_order_acquire);
            const bool past_deadline = NowMs() >= give_up_ms;
            if (aborted || past_deadline) {
                // D-2 chain across the process boundary: order the worker to
                // unwind (abort frame). Its event, if it still arrives, is a
                // stray the NEXT job's read loop never sees (this pipe is
                // quiesced by the crash/respawn or GracefulStop paths).
                (void)wmgr.SendToWorker(family, wp::BuildAbort(0));
                status = enginehost::HostStatus::Timeout;
                answered = true;
                break;
            }
        }
        if (answered || !g_shutdown.load(std::memory_order_acquire)) {
            job.conn->SendResult(job.id, status, out_text);
            // Health (REQ-008): the host-observable outcome bucket.
            if (status == enginehost::HostStatus::Ok) {
                g_health.RecordJob(host_v2::HealthOutcome::Success);
            } else if (status == enginehost::HostStatus::Timeout ||
                       status == enginehost::HostStatus::Busy) {
                g_health.RecordJob(host_v2::HealthOutcome::Fallback);
            } else {
                g_health.RecordJob(host_v2::HealthOutcome::Failure);
            }
        }
        wmgr.SetBusy(family, false);
        {
            std::lock_guard<std::mutex> lk(g_current.mu);
            g_current.done = true;
            g_current.id = 0;
        }
        g_current.cv.notify_all();
        TouchActivity();
    }
}

// ---- v2 dispatch (M6 T4, §V2-4.6): the SchedulerLoop thread ------------------
// Pops the §V2-4.6 queue (priority ASC, enqueue ASC; expired items are
// answered timeout on pop) and proxies each item to the ggml-translate
// worker exactly like the v1 dispatcher, answering the REQUESTER's
// connection. One thread = the single worker-context serialization the
// registry max_sessions=1 profile prescribes (§V2-5.2).
void DispatcherV2Loop(host_v2::WorkerManager& wmgr) {
    namespace wp = emebalachat::workerproto;
    const std::wstring family(kWorkerFamilyTranslate);
    for (;;) {
        host_v2::SchedItem item;
        host_v2::SchedItem expired;
        bool had_expired = false;
        if (!g_scheduler.Pop(item, expired, had_expired)) {
            if (g_shutdown.load(std::memory_order_acquire)) break;
            ::Sleep(kIdlePollMs); // bounded idle wait (queue has no blocking pop)
            continue;
        }
        if (had_expired) {
            // §V2-4.6 deadline: queued-too-long requests answer timeout.
            if (expired.user) {
                static_cast<Connection*>(expired.user)->SendResult(
                    0, enginehost::HostStatus::Timeout);
            }
            g_health.RecordJob(host_v2::HealthOutcome::Fallback);
            continue;
        }
        Connection* requester = static_cast<Connection*>(item.user);
        if (!requester) continue;
        if (!ModelFileExists()) {
            requester->SendResult(0, enginehost::HostStatus::ModelMissing);
            g_health.RecordJob(host_v2::HealthOutcome::Failure);
            continue;
        }
        host_v2::WorkerHandle* w = wmgr.EnsureSpawned(family);
        if (!w) {
            requester->SendResult(0, enginehost::HostStatus::Busy);
            g_health.RecordJob(host_v2::HealthOutcome::Fallback);
            continue;
        }
        // The v2 profile forwards the request with a synthetic in-flight id
        // (the client's id space stays untouched — results here answer the
        // session/request that enqueued, not an id echo; §V2-4.3 sessions
        // own the matching). The worker needs SOME job id; the scheduler
        // enqueue_seq (>= 1) is unique per host.
        wp::JobMsg jm;
        jm.job = item.enqueue_seq;
        jm.session = item.session;
        wmgr.SetBusy(family, true);
        const bool sent = wmgr.SendToWorker(family, wp::BuildJob(jm));
        if (!sent) {
            requester->SendResult(0, enginehost::HostStatus::EngineFailed);
            g_health.RecordJob(host_v2::HealthOutcome::Failure);
            wmgr.SetBusy(family, false);
            continue;
        }
        enginehost::HostStatus status = enginehost::HostStatus::EngineFailed;
        std::string out_text;
        const int64_t give_up_ms =
            (item.deadline_ms != 0 ? item.deadline_ms : NowMs() + 30000) + kWorkerAnswerGraceMs;
        for (;;) {
            if (g_shutdown.load(std::memory_order_acquire)) break;
            host_v2::WorkerHandle* cur = wmgr.EnsureSpawned(family);
            if (!cur) break;
            std::string json;
            const auto rc = wmgr.ReadFromWorker(family, json, 250);
            if (rc == host_v2::WorkerManager::WorkerRead::Ok) {
                wp::EventMsg ev;
                if (!wp::ParseEvent(json, ev)) {
                    status = enginehost::HostStatus::EngineFailed;
                    break;
                }
                if (ev.kind == wp::EventKind::Final) {
                    status = enginehost::HostStatus::Ok;
                    out_text = ev.text;
                    break;
                }
                if (ev.kind == wp::EventKind::Error) {
                    if (ev.code == "timeout") status = enginehost::HostStatus::Timeout;
                    else if (ev.code == "model_missing") status = enginehost::HostStatus::ModelMissing;
                    else status = enginehost::HostStatus::EngineFailed;
                    break;
                }
                continue;
            }
            if (rc == host_v2::WorkerManager::WorkerRead::IoError) {
                status = enginehost::HostStatus::EngineFailed;
                break;
            }
            if (NowMs() >= give_up_ms) {
                (void)wmgr.SendToWorker(family, wp::BuildAbort(0));
                status = enginehost::HostStatus::Timeout;
                break;
            }
        }
        requester->SendResult(0, status, out_text);
        if (status == enginehost::HostStatus::Ok) {
            g_health.RecordJob(host_v2::HealthOutcome::Success);
        } else if (status == enginehost::HostStatus::Timeout ||
                   status == enginehost::HostStatus::Busy) {
            g_health.RecordJob(host_v2::HealthOutcome::Fallback);
        } else {
            g_health.RecordJob(host_v2::HealthOutcome::Failure);
        }
        wmgr.SetBusy(family, false);
        TouchActivity();
    }
}

// ---- session handling -------------------------------------------------------
// Read exactly one pipe message (frame) into `json`. False on IO error or a
// sender-side cap violation (§4.1: > 1 MiB -> bad_request + close).
bool ReadMessage(Connection& conn, std::string& json) {
    constexpr DWORD kBufSize = (1u << 20) + 4;
    static thread_local std::vector<char> buf;
    if (buf.size() < kBufSize) buf.resize(kBufSize);

    OVERLAPPED ol = {};
    ol.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ol.hEvent) return false;
    DWORD read = 0;
    BOOL ok = ::ReadFile(conn.pipe, buf.data(), kBufSize, &read, &ol);
    if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
        // Wait on the stop event too: a client that connected in the idle-exit
        // window must not hold the final thread join (and the process exit)
        // hostage until it disconnects on its own.
        const HANDLE waits[2] = {ol.hEvent, g_stop_event};
        const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0) {
            ::CancelIoEx(conn.pipe, &ol);
            ::CloseHandle(ol.hEvent);
            return false;
        }
        ok = ::GetOverlappedResult(conn.pipe, &ol, &read, TRUE);
    }
    const DWORD readErr = ::GetLastError();
    ::CloseHandle(ol.hEvent);
    if (!ok) {
        if (readErr == ERROR_MORE_DATA) {
            // Frame exceeds the read buffer: answer bad_request; the caller
            // disconnects and the undrained body dies with the connection.
            conn.SendError(enginehost::kErrBadRequest);
        }
        return false;
    }
    if (read < enginehost::kFrameHeaderSize) return false;
    uint32_t len = 0;
    enginehost::FrameReadLengthPrefix(buf.data(), len);
    if (len > enginehost::kMaxFrameBytes ||
        static_cast<size_t>(len) + enginehost::kFrameHeaderSize != read) {
        conn.SendError(enginehost::kErrBadRequest);
        return false;
    }
    json.assign(buf.data() + enginehost::kFrameHeaderSize, static_cast<size_t>(len));
    return true;
}

// REQ-043 (M6 T4, §V2-4.7): hello.protocol BRANCHES here.
//   protocol == 1 -> RunSessionV1: the FROZEN §4.4 flow, byte-for-byte the
//     pre-T4 RunSession body (translate/cancel/unknown-op rules unchanged,
//     queue = the frozen JobQueue with immediate-busy depth > 8). A v1 client
//     receives IDENTICAL behavior no matter WHICH pipe (canonical or alias)
//     it connected to.
//   protocol == 2 -> RunSessionV2: the v2 profile (§V2-4.3 sessions,
//     §V2-4.6 scheduler, welcome v2). M6's real consumer is translate routed
//     through the scheduler; the session lifecycle frames follow §V2-4.4.
//   anything else -> version_mismatch (the v1 rule, frozen — unchanged).
// The token check stays BEFORE the protocol check (frozen §4.2 order).
void RunSessionV1(Connection& conn, const enginehost::HelloMsg& hello) {
    DIAG_LOG("ENGINEHOST", "conn/%03d: hello ok v1 (client=%s version=%s)",
             conn.index, hello.client.c_str(), hello.client_version.c_str());
    conn.SendWelcome();

    // ---- request loop (the FROZEN §4.4 body, unchanged) ----
    std::string frame;
    for (;;) {
        if (!ReadMessage(conn, frame)) return; // client gone / cap violation
        TouchActivity();

        enginehost::JsonPairs fields;
        if (!enginehost::JsonParseObject(frame, fields)) {
            conn.SendErrorThenClose(enginehost::kErrBadRequest);
            return;
        }
        const auto* opField = enginehost::detail::FindField(fields, "op");
        const std::string op = (opField && opField->is_string) ? opField->text : "";

        if (op == "translate") {
            enginehost::TranslateMsg msg;
            if (!enginehost::ParseTranslate(frame, msg)) {
                // §4.4: every answer carries the request id when one exists;
                // an unparseable id is a protocol-level bad_request.
                const auto* idField = enginehost::detail::FindField(fields, "id");
                uint64_t id = 0;
                if (idField && enginehost::detail::ParseUInt64(idField->text, id)) {
                    conn.SendResult(id, enginehost::HostStatus::BadRequest);
                    continue;
                }
                conn.SendErrorThenClose(enginehost::kErrBadRequest);
                return;
            }
            if (msg.src.empty() || msg.tgt.empty()) {
                conn.SendResult(msg.id, enginehost::HostStatus::BadRequest);
                continue;
            }
            // Sanity-clamp the deadline into the supported band (the frozen
            // contract is silent on out-of-range values; documented minimal
            // interpretation — the request still runs, bounded).
            msg.timeout_ms = std::max(enginehost::kMinTranslateTimeoutMs,
                                      std::min(enginehost::kMaxTranslateTimeoutMs, msg.timeout_ms));
            // §8 fast path: no model on disk -> model_missing without queueing.
            if (!ModelFileExists()) {
                conn.SendResult(msg.id, enginehost::HostStatus::ModelMissing);
                continue;
            }
            Job job;
            job.id = msg.id;
            job.src = std::move(msg.src);
            job.tgt = std::move(msg.tgt);
            job.text = std::move(msg.text);
            job.timeout_ms = msg.timeout_ms;
            job.conn = &conn;
            if (!g_queue.Push(job)) {
                conn.SendResult(job.id, enginehost::HostStatus::Busy); // §4.4: depth > 8
                continue;
            }
        } else if (op == "cancel") {
            enginehost::CancelMsg msg;
            if (!enginehost::ParseCancel(frame, msg)) {
                conn.SendErrorThenClose(enginehost::kErrBadRequest);
                return;
            }
            // §4.4: only the in-flight job is aborted; anything else is ignored.
            std::lock_guard<std::mutex> lk(g_current.mu);
            if (!g_current.done && g_current.id == msg.id) {
                g_abort.store(true, std::memory_order_release);
            }
        } else if (op == "hello") {
            // A second hello on one connection is a protocol violation.
            conn.SendErrorThenClose(enginehost::kErrBadRequest);
            return;
        } else {
            // §4.3: unknown ops converge to bad_request + close.
            conn.SendErrorThenClose(enginehost::kErrBadRequest);
            return;
        }
    }
}

// ---- v2 profile session (M6 T4, §V2-4.3/§V2-4.4) ----------------------------
// hello.protocol=2. M6's real consumer is translate, but the session model
// and event frames already follow §V2-4.3/§V2-4.4: session_open ->
// {"op":"opened","session":N}; translate rides the §V2-4.6 scheduler with
// priority/drop_eligible/deadline extension fields (unknown-field rule makes
// them OPTIONAL for the receiver); close/cancel end the session. The
// dispatcher thread (the same single worker-context contract, §V2-4.6
// max_sessions=1) drains the scheduler queue and proxies to the ggml worker
// exactly like the v1 path.
void RunSessionV2(Connection& conn, const enginehost::HelloMsg& hello) {
    namespace wp = emebalachat::workerproto;
    DIAG_LOG("ENGINEHOST", "conn/%03d: hello ok v2 (client=%s version=%s)",
             conn.index, hello.client.c_str(), hello.client_version.c_str());
    conn.SendWelcomeV2();

    std::string frame;
    for (;;) {
        if (!ReadMessage(conn, frame)) break; // client gone / cap violation
        TouchActivity();

        enginehost::JsonPairs fields;
        if (!enginehost::JsonParseObject(frame, fields)) {
            conn.SendErrorThenClose(enginehost::kErrBadRequest);
            return;
        }
        const auto* opField = enginehost::detail::FindField(fields, "op");
        const std::string op = (opField && opField->is_string) ? opField->text : "";

        if (op == "session_open") {
            // §V2-4.3: capability-gated (M6 serves "translate" only —
            // anything else is unavailable, §V2-4.5).
            host_v2::SessionRecord req;
            if (const auto* cap = enginehost::detail::FindField(fields, "capability")) {
                if (cap->is_string) req.capability = cap->text;
            }
            if (const auto* mi = enginehost::detail::FindField(fields, "model_id")) {
                if (mi->is_string) req.model_id = mi->text;
            }
            if (const auto* pr = enginehost::detail::FindField(fields, "profile")) {
                if (pr->is_string) req.profile = pr->text;
            }
            // §V2-4.6 optional scheduling fields (clamped/validated).
            if (const auto* p = enginehost::detail::FindField(fields, "priority")) {
                int v = 0;
                if (enginehost::detail::ParseInt(p->text, v)) req.priority = v;
            }
            if (const auto* de = enginehost::detail::FindField(fields, "drop_eligible")) {
                req.drop_eligible = (de->text == "true");
            }
            req.connection = &conn;
            host_v2::SessionRecord opened;
            const auto r = g_sessions.Open(req, opened);
            if (r != host_v2::SessionOpenResult::Opened) {
                conn.SendResult(0, enginehost::HostStatus::Busy);
                continue;
            }
            g_health.RecordSession(true);
            // {"op":"opened","session":N}
            if (!conn.WriteFrame(std::string("{\"op\":\"opened\",\"session\":") +
                                 std::to_string(opened.id) + "}")) {
                g_sessions.Close(opened.id);
            }
        } else if (op == "translate") {
            // §V2-4.4 translate: the v1 frozen message shape plus OPTIONAL
            // model/profile/sampling/priority fields (§V2-4.4 table). The
            // unknown-field rule makes the extension additive; parsing the
            // frozen core reuses the frozen parser (fail-closed identical).
            enginehost::TranslateMsg msg;
            if (!enginehost::ParseTranslate(frame, msg)) {
                const auto* idField = enginehost::detail::FindField(fields, "id");
                uint64_t id = 0;
                if (idField && enginehost::detail::ParseUInt64(idField->text, id)) {
                    conn.SendResult(id, enginehost::HostStatus::BadRequest);
                    continue;
                }
                conn.SendErrorThenClose(enginehost::kErrBadRequest);
                return;
            }
            if (msg.src.empty() || msg.tgt.empty()) {
                conn.SendResult(msg.id, enginehost::HostStatus::BadRequest);
                continue;
            }
            msg.timeout_ms = std::max(enginehost::kMinTranslateTimeoutMs,
                                      std::min(enginehost::kMaxTranslateTimeoutMs, msg.timeout_ms));
            // §8 fast path stays identical (fresh machine answers
            // model_missing without queueing, both profiles).
            if (!ModelFileExists()) {
                conn.SendResult(msg.id, enginehost::HostStatus::ModelMissing);
                continue;
            }
            host_v2::SchedItem item;
            if (const auto* p = enginehost::detail::FindField(fields, "priority")) {
                int v = 0;
                if (enginehost::detail::ParseInt(p->text, v)) item.priority = v;
            }
            if (const auto* de = enginehost::detail::FindField(fields, "drop_eligible")) {
                item.drop_eligible = (de->text == "true");
            }
            item.deadline_ms = NowMs() + msg.timeout_ms; // §V2-4.6 deadline
            item.user = &conn;
            host_v2::SchedItem evicted;
            const auto r = g_scheduler.Enqueue(item, evicted);
            if (r == host_v2::EnqueueResult::BusyDroppedOld) {
                // §V2-4.6: the OLDEST eligible was discarded — answer busy to
                // THAT request's connection (it carried a different id).
                if (evicted.user) {
                    static_cast<Connection*>(evicted.user)->SendResult(
                        0, enginehost::HostStatus::Busy);
                }
            } else if (r == host_v2::EnqueueResult::BusyOverflow) {
                conn.SendResult(msg.id, enginehost::HostStatus::Busy);
            }
            // Enqueued: the v2 dispatch thread answers result when done.
        } else if (op == "session_close" || op == "close") {
            std::uint64_t session = 0;
            if (const auto* s = enginehost::detail::FindField(fields, "session")) {
                if (!enginehost::detail::ParseUInt64(s->text, session)) {
                    conn.SendErrorThenClose(enginehost::kErrBadRequest);
                    return;
                }
            }
            std::vector<host_v2::SchedItem> dropped;
            (void)g_scheduler.DropSession(session, dropped);
            for (const auto& d : dropped) {
                if (d.user) {
                    static_cast<Connection*>(d.user)->SendResult(
                        0, enginehost::HostStatus::Busy);
                }
            }
            (void)g_sessions.Close(session);
            if (!conn.WriteFrame(std::string("{\"op\":\"closed\",\"session\":") +
                                 std::to_string(session) + "}")) {
                break;
            }
        } else if (op == "cancel") {
            enginehost::CancelMsg msg;
            if (!enginehost::ParseCancel(frame, msg)) {
                conn.SendErrorThenClose(enginehost::kErrBadRequest);
                return;
            }
            // §4.4 cancel rule carried into v2: only the in-flight job is
            // aborted (the dispatcher's g_current), everything else ignored.
            std::lock_guard<std::mutex> lk(g_current.mu);
            if (!g_current.done && g_current.id == msg.id) {
                g_abort.store(true, std::memory_order_release);
            }
        } else if (op == "list_models") {
            // §V2-4.4: registry enumeration (no user text).
            std::string models = "[";
            if (g_registry_loaded) {
                bool first = true;
                for (const auto& m : g_registry.models) {
                    if (!first) models += ',';
                    first = false;
                    models += std::string("{\"id\":\"") + enginehost::JsonEscape(m.id) +
                              "\",\"family\":\"" + enginehost::JsonEscape(m.family) + "\"}";
                }
            }
            models += ']';
            if (!conn.WriteFrame(std::string("{\"op\":\"models\",\"models\":") + models + "}")) {
                break;
            }
        } else if (op == "unload_model") {
            // §V2-4.4: a RESIDENCY HINT only (the scheduler decides); M6's
            // sticky translate model acknowledges and keeps serving.
            if (!conn.WriteFrame(std::string("{\"op\":\"unloaded\"}"))) {
                break;
            }
        } else if (op == "hello") {
            // A second hello on one connection is a protocol violation (the
            // v1 rule carries over).
            conn.SendErrorThenClose(enginehost::kErrBadRequest);
            return;
        } else {
            // §V2-12-2: unknown ops converge to bad_request + close.
            conn.SendErrorThenClose(enginehost::kErrBadRequest);
            return;
        }
    }
    // Connection teardown: auto-close the sessions this connection owns
    // (task item 2: "연결 종료 시 소유 세션 자동 정리").
    std::vector<std::uint64_t> owned;
    (void)g_sessions.CloseOwnedByConnection(&conn, owned);
    for (const auto sid : owned) {
        std::vector<host_v2::SchedItem> dropped;
        (void)g_scheduler.DropSession(sid, dropped);
    }
}

// The protocol BRANCH (§V2-4.7): handshake once, then dispatch by profile.
void RunSession(Connection& conn) {
    // ---- handshake (§4.4): hello is mandatory, exactly once ----
    std::string frame;
    if (!ReadMessage(conn, frame)) return;
    enginehost::HelloMsg hello;
    if (!enginehost::ParseHello(frame, hello)) {
        conn.SendErrorThenClose(enginehost::kErrBadRequest);
        return;
    }
    // Token check runs BEFORE the protocol check (an outsider learns nothing
    // about the protocol version without the boot token) — frozen order.
    if (hello.token.size() != 32 || hello.token != g_token) {
        conn.SendErrorThenClose(enginehost::kErrUnauthorized);
        return;
    }
    if (hello.protocol == enginehost::kProtocolVersion) {
        RunSessionV1(conn, hello);
        return;
    }
    if (hello.protocol == 2) {
        RunSessionV2(conn, hello);
        return;
    }
    // Any other protocol: version_mismatch (the v1 rule, frozen).
    conn.SendErrorThenClose(enginehost::kErrVersionMismatch);
}

void ConnectionLoop(Connection* conn) {
    HANDLE acceptEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!acceptEvent) {
        DIAG_F("ENGINEHOST/Conn/%03d: accept-event creation failed\n", conn->index);
        return;
    }
    for (;;) {
        if (g_shutdown.load(std::memory_order_acquire)) break;
        ::ResetEvent(acceptEvent);
        ::ZeroMemory(&conn->accept_ol, sizeof(conn->accept_ol));
        conn->accept_ol.hEvent = acceptEvent;
        BOOL ok = ::ConnectNamedPipe(conn->pipe, &conn->accept_ol);
        const DWORD err = ::GetLastError();
        if (!ok && err == ERROR_IO_PENDING) {
            const HANDLE waits[2] = {acceptEvent, g_stop_event};
            const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w != WAIT_OBJECT_0) break; // stop signaled (or handle churn)
            DWORD dummy = 0;
            if (!::GetOverlappedResult(conn->pipe, &conn->accept_ol, &dummy, TRUE)) break;
        } else if (!ok && err != ERROR_PIPE_CONNECTED) {
            DIAG_F("ENGINEHOST/Conn/%03d: ConnectNamedPipe failed (err=%lu)\n", conn->index, err);
            ::Sleep(500);
            continue;
        }
        g_active_connections.fetch_add(1, std::memory_order_acq_rel);
        TouchActivity();
        // REQ-043: duplicate the server end for writes BEFORE the session:
        // worker-thread result frames must not serialize behind this loop's
        // pending ReadFile on the same handle (observed deadlock — the
        // translate completed in 62 ms yet the client never got the frame).
        if (!::DuplicateHandle(::GetCurrentProcess(), conn->pipe,
                               ::GetCurrentProcess(), &conn->write_pipe, 0,
                               FALSE, DUPLICATE_SAME_ACCESS)) {
            DIAG_F("ENGINEHOST/Conn/%03d: DuplicateHandle(write_pipe) failed (err=%lu); worker writes fall back to the shared handle\n",
                   conn->index, ::GetLastError());
            conn->write_pipe = nullptr;
        }
        RunSession(*conn);
        g_active_connections.fetch_sub(1, std::memory_order_acq_rel);
        TouchActivity();
        if (conn->write_pipe) {
            ::CloseHandle(conn->write_pipe);
            conn->write_pipe = nullptr;
        }
        ::DisconnectNamedPipe(conn->pipe);
    }
    ::CloseHandle(acceptEvent);
}

} // namespace
} // namespace host
} // namespace emebalachat

int WINAPI wWinMain(HINSTANCE /*hInstance*/, HINSTANCE, PWSTR pCmdLine, int) {
    using namespace emebalachat;
    using namespace emebalachat::host;
    using namespace emebalachat::enginehost;

    // L3 contract, identical to src/main.cpp: remove the CWD from the DLL
    // search order (NULL would restore it; L"" is the actual hardening).
    if (!::SetDllDirectoryW(L"")) {
        DIAG_F("ENGINEHOST/SetDllDirectory/001: SetDllDirectoryW(L\"\") failed (err=%lu)\n",
               ::GetLastError());
    }

    // Shape-only logging; the file sink stays OFF (no config file, release
    // posture — the DIAG_F stderr mirror still reports hard errors).
    (void)diag::Init();

    // P5-F1: driverless machines must CPU-fall back, never SEH 0xC06D007E.
    (void)EnsureVulkanGuard();

    // Single instance (§5.1-4): a duplicate spawn exits quietly; the client
    // then retries against the already-running host.
    HANDLE hMutex = ::CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
    if (!hMutex || ::GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) ::CloseHandle(hMutex);
        diag::Shutdown();
        return 0;
    }

    // Hidden test hook for the idle-exit rule (§4.4): --idle-exit-ms <n>.
    long long idle_exit_ms = kDefaultIdleExitMs;
    if (pCmdLine) {
        const wchar_t* key = wcsstr(pCmdLine, L"--idle-exit-ms");
        if (key) {
            const wchar_t* num = key + wcslen(L"--idle-exit-ms");
            if (*num == L'=') ++num;
            while (*num == L' ') ++num;
            const long long v = _wtoll(num);
            if (v > 0) idle_exit_ms = v;
        }
    }

    // ---- boot token (§4.2) ----
    const std::wstring engine_dir = EngineDir();
    const std::wstring token_path = TokenPath();
    std::string token;
    std::error_code dir_ec;
    if (!engine_dir.empty()) {
        std::filesystem::create_directories(engine_dir, dir_ec);
    }
    if (engine_dir.empty() || token_path.empty() || dir_ec ||
        !GenerateTokenHex32(token) || !WriteTokenFile(token_path, token)) {
        DIAG_F("ENGINEHOST/Token/001: token file setup failed (dir=%ls err=%lu)\n",
               engine_dir.empty() ? L"<none>" : engine_dir.c_str(), ::GetLastError());
        ::ReleaseMutex(hMutex);
        ::CloseHandle(hMutex);
        diag::Shutdown();
        return 1;
    }
    g_token = token;
    TokenFileGuard token_guard{token_path};

    // ---- pipe instances (§4.1) ----
    SecurityDescriptorHolder pipe_sd;
    if (!BuildUserOnlySd(pipe_sd)) {
        DIAG_F("ENGINEHOST/Pipe/001: user-only security descriptor failed (err=%lu)\n",
               ::GetLastError());
        ::ReleaseMutex(hMutex);
        ::CloseHandle(hMutex);
        diag::Shutdown();
        return 1;
    }
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = pipe_sd.sd;
    sa.bInheritHandle = FALSE;

    // REQ-043 (M6 T4, task item 4): registry.json resolves model_id/profile
    // when present; ABSENT file -> the v1 hardcoded-path default (backward
    // compatible — the pinned model location IS the default).
    {
        const auto lr = engine_host_registry::LoadDefaultRegistry();
        if (lr.status == engine_host_registry::LoadStatus::Ok) {
            g_registry = std::move(lr.registry);
            g_registry_loaded = true;
        }
        DIAG_LOG("ENGINEHOST", "host/003: registry status=%d models=%zu",
                 static_cast<int>(lr.status), g_registry.models.size());
    }

    // ---- the worker manager (M6 T3) + the ggml-translate family ----
    // The worker exe sits next to the orchestrator in the build tree and at
    // %LOCALAPPDATA%\Emebala\Common\engine in the installed layout — both
    // are the orchestrator's own directory, so the exe-adjacent lookup
    // covers both (T7 may adjust the installed name only).
    wchar_t self_path[MAX_PATH] = {0};
    std::wstring worker_exe;
    if (::GetModuleFileNameW(nullptr, self_path, MAX_PATH) > 0) {
        std::filesystem::path dir = std::filesystem::path(self_path).parent_path();
        worker_exe = (dir / L"Emebalachat.Engine.ggml-translate.exe").wstring();
    }
    host_v2::WorkerManager wmgr;
    wmgr.RegisterFamily(kWorkerFamilyTranslate, worker_exe);
    wmgr.StartReaper(); // 250 ms done_event poll (design §1.1 rule 3)

    std::vector<std::unique_ptr<Connection>> connections;
    std::vector<std::thread> threads;
    // REQ-043 (M6 T4, REQ-002): DUAL pipe sets — the frozen alias
    // enginehost::kPipeName (\\.\pipe\emebala-engine-v1, the v1 §4.1 name,
    // 4 instances — the v1 client availability is NOT reduced) plus the new
    // canonical versionless \\.\pipe\emebala-engine (4 instances). 8
    // ConnectionLoops total share ONE g_active_connections.
    const std::string alias_name_a(enginehost::kPipeName);
    const std::wstring alias_name(alias_name_a.begin(), alias_name_a.end());
    const std::string canon_name_a(kPipeNameVersionless);
    const std::wstring canon_name(canon_name_a.begin(), canon_name_a.end());
    const std::wstring* pipe_names[2] = {&alias_name, &canon_name};
    int next_index = 0;
    for (const std::wstring* name : pipe_names) {
        for (size_t i = 0; i < kPipeInstanceCount; ++i) {
            auto conn = std::make_unique<Connection>();
            conn->index = next_index++;
            conn->pipe = ::CreateNamedPipeW(
                name->c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                static_cast<DWORD>(kPipeInstanceCount),
                (1u << 20) + 4, // outbound buffer fits the max legal frame
                (1u << 20) + 4, // inbound buffer
                0, &sa);
            if (conn->pipe == INVALID_HANDLE_VALUE) {
                DIAG_F("ENGINEHOST/Pipe/002: CreateNamedPipe failed (err=%lu)\n", ::GetLastError());
                wmgr.ShutdownAll();
                ::ReleaseMutex(hMutex);
                ::CloseHandle(hMutex);
                diag::Shutdown();
                return 1;
            }
            connections.push_back(std::move(conn));
        }
    }

    g_stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (size_t i = 0; i < connections.size(); ++i) {
        threads.emplace_back(ConnectionLoop, connections[i].get());
    }
    std::thread watchdog(WatchdogLoop);
    std::thread dispatcher(DispatcherLoop, std::ref(wmgr));
    std::thread dispatcher_v2(DispatcherV2Loop, std::ref(wmgr));

    DIAG_LOG("ENGINEHOST",
             "host/001: Emebala.Engine up (pipes=%s + %s instances=%zu total=%zu idle_exit_ms=%lld)",
             enginehost::kPipeName.data(), kPipeNameVersionless,
             kPipeInstanceCount, connections.size(), idle_exit_ms);

    // ---- idle-exit rule (§4.4 + T4 update): 0 active connections AND no
    // scheduler backlog AND no open session, idle budget elapsed. The worker
    // idle state is the manager's own business (its Reaper keeps the family
    // warm only while jobs flow; GracefulStop below reclaims it at exit).
    TouchActivity();
    for (;;) {
        ::Sleep(kIdlePollMs);
        if (g_active_connections.load(std::memory_order_acquire) > 0) continue;
        if (g_scheduler.Size() > 0) continue;
        if (g_sessions.OpenCount() > 0) continue;
        const int64_t idle_for = NowMs() - g_last_activity_ms.load(std::memory_order_acquire);
        if (idle_for >= idle_exit_ms) break;
    }

    DIAG_LOG("ENGINEHOST", "host/002: idle exit after %lld ms with no connections", idle_exit_ms);

    // Graceful shutdown: stop the queue, drain the dispatcher, M-2
    // GracefulStop every worker family BEFORE any pipe teardown (design
    // §1.1 rule 4: no orphaned worker), wake the connection threads, delete
    // the token file, then exit. token_guard deletes the token on scope exit.
    g_shutdown.store(true, std::memory_order_release);
    ::SetEvent(g_stop_event);
    g_queue.StopAll();
    g_current.cv.notify_all();
    if (dispatcher.joinable()) dispatcher.join();
    if (dispatcher_v2.joinable()) dispatcher_v2.join();
    if (watchdog.joinable()) watchdog.join();
    // M-2 order: workers are stopped and awaited BEFORE the orchestrator's
    // own handles close (wmgr.ShutdownAll blocks on each GracefulStop).
    wmgr.ShutdownAll();
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
    for (auto& c : connections) {
        if (c->pipe && c->pipe != INVALID_HANDLE_VALUE) ::CloseHandle(c->pipe);
    }
    if (g_stop_event) ::CloseHandle(g_stop_event);
    diag::Shutdown();
    ::ReleaseMutex(hMutex);
    ::CloseHandle(hMutex);
    return 0;
}
