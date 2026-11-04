// ---------------------------------------------------------------------------
// ggml_translate_worker — REQ-043 (M6 T3, design §1.2, plan §V2-3): the
// Emebala.Engine.ggml-translate.exe worker process (WIN32 subsystem; unified
// family name since session 260930_0003).
//
// Second frozen contract (난부 계약, §V2-3): the orchestrator creates the
// PRIVATE named pipe (server side) and spawns this exe with the pipe name on
// the command line and the boot-scoped token in the child ENVIRONMENT
// (EMEBALA_WORKER_TOKEN, review round 1 F-11; an old orchestrator still passes
// --token, which remains the fallback); this process connects as a CLIENT.
// Wire frames are the §4.3 [u32 LE][UTF-8 JSON] reused unmodified (1 MiB cap,
// one frame = one pipe message); the message vocabulary lives in
// worker_protocol.hpp (header-only, unit-tested without any process).
//
// Lifecycle (design §1.2):
//   1. wWinMain: SetDllDirectoryW(L""), diag::Init, EnsureVulkanGuard,
//      single-instance mutex Local\EmebalaEngine_Worker_ggml_translate.
//   2. Parse argv: --pipe <name> [--mutex <family>] (the legacy --token argv
//      is still parsed as the old-host fallback; the 0.10.5+ host passes the
//      token via the environment instead). Missing/garbled -> exit 2
//      (the orchestrator ReaperLoop judges the exit code).
//   3. Connect to the pipe (user-only SD enforced by the kernel — the
//      orchestrator built the pipe with the BuildUserOnlySd pattern; this
//      process just CreateFileWs it), send announce (worker.manifest §3.4).
//   4. Frame loop, SPLIT (Backend P0-1, v0.10.5): a dedicated READER thread
//      owns the pipe; the wWinMain thread is the INFERENCE executor (job ->
//      LocalInferenceEngine::Translate, per-request sampling profile;
//      cancel_flag address stability contract from translation_common.hpp —
//      one process-lifetime atomic, D-2 chain b->c) -> event final | error.
//      abort -> cancel_flag store (unwinds a running decode within ms) or a
//      queued-job marker. close -> closed. heartbeats every 5 s while idle.
//      shutdown -> cancel_flag store -> unload model -> shutdown_ack ->
//      exit 0. Pipe breakage cancels the in-flight decode too.
//   5. M-2 (tech gate): DisconnectNamedPipe purges unconsumed frames (v1
//      measured, host_main.cpp:363-372), so the final exchange of a dying
//      session is protected by ORDERING: every WriteFrame here completes
//      synchronously (GetOverlappedResult wait) before returning, the
//      shutdown_ack is the LAST write before exit, and the ORCHESTRATOR
//      (host_v2_worker_manager) waits for shutdown_ack / pipe EOF BEFORE
//      CloseHandle — no frame can be purged unseen. This process never calls
//      DisconnectNamedPipe.
//
// Security (design §10): no config file, no token file (the token lives only
// in THIS process environment / on the command line of a legacy spawn; the
// user-only pipe ACL is enforced at the kernel boundary), shape-only
// ENGINEHOST/worker/NNN diagnostics (stderr mirror only; the file sink stays
// off — diag defaults), user text never logged from this layer.
// Delay-load rules identical to the host: CUDA (+Vulkan when built) DLLs are
// delay-loaded by CMake; on a driverless machine EnsureVulkanGuard + the
// EnsureLoaded CUDA->CPU retry keep inference alive.
// RT-C (260926_0003, task RT-C "D안-개선"): the EnsureLoaded VRAM auto gate
// (see translation_common.hpp / engine_core_helpers.hpp) reads the manual
// override EMEBALA_MT_GPU=0/1 from THIS process environment. The
// orchestrator spawns us with lpEnvironment=nullptr
// (host_v2_worker_manager.cpp LaunchWorkerProcess), so the variable inherits
// from the host process / user environment — set it user- or machine-wide
// (or on the host) to pin the MT worker to GPU or CPU on a given box. No
// manifest or command-line change: the wire contract is untouched.
// ---------------------------------------------------------------------------

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "worker_protocol.hpp"       // second frozen contract (pure helpers)
#include "translation_common.hpp"    // LocalInferenceEngine (worker low body)
#include "engine.hpp"                // kPinnedModelFilename (engine_core re-export)
#include "config.hpp"                // NormalizeLanguageCode / FindLanguageByCode (prompt parity)
#include "diag_logger.hpp"           // shape-only diagnostics
#include "unicode_utils.hpp"         // ToUtf8 / ToUtf16
#include "vulkan_guard.hpp"          // P5-F1 driverless-machine guard
// REQ-044 (P4-2): shared engine-host path constants (kModelsDirRel) —
// replaces the inline literal that used to be at the model-path site below.
#include "engine_host_paths.hpp"     // REQ-044: shared path constants
// REQ-045 P4-4 (item 3a-1, design §A 안 i): the worker resolves a client
// model_id against registry.json itself (bare-filename files[] resolve
// against the fixed Common\models dir). The parser is already linked via
// Emebalachat_core — only the call is new (tech gate c4).
#include "engine_host_registry.hpp"  // REQ-045: registry.json (model_id -> files[])

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h> // SHGetKnownFolderPath

#pragma comment(lib, "shell32.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable> // Backend P0-1: the reader<->inference command queue
#include <cstdint>
#include <cwchar>
#include <memory>
#include <mutex> // Backend P0-1: g_pipe_write_mu serializes the two threads' frames
#include <string>
#include <thread> // Backend P0-1: the dedicated pipe reader thread
#include <vector>

namespace {

using emebalachat::LocalInferenceEngine;
namespace wp = emebalachat::workerproto;
namespace enginehost = emebalachat::enginehost;

constexpr DWORD kPipeBufSize = (1u << 20) + 4; // fits the max legal frame

// Backend FIX 4 (review round 1, F-11): the boot-scoped worker token arrives
// as this ENVIRONMENT variable — the 0.10.5+ orchestrator never puts it on
// the command line (same-user processes can read a child argv). This literal
// is the frozen cross-process contract with host_v2_worker_manager.cpp
// (kWorkerTokenEnvName there, same spelling); keep the two in sync.
constexpr wchar_t kWorkerTokenEnvName[] = L"EMEBALA_WORKER_TOKEN";

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- command line (§1.2 handshake: pipe name [+ legacy boot token]) --------
struct WorkerArgs {
    std::wstring pipe_name;
    std::string token; // LEGACY argv token (old orchestrators); the 0.10.5+
                       // host passes EMEBALA_WORKER_TOKEN in the environment
                       // instead (review round 1 F-11).
    // B3 fix (session 260928_0001): the orchestrator passes the family it is
    // spawning this worker for as --mutex <family>; the worker derives its
    // PER-FAMILY single-instance mutex from it (wp::MutexNameForFamily). Empty
    // (arg absent — an OLD orchestrator) -> the exact legacy constant, so a
    // new worker under an old host keeps single-instance protection for the
    // default translate family.
    std::wstring mutex_family;
};

// Minimal argv walker: --pipe <value> / --token <value> / --mutex <value>.
// The orchestrator is the only legitimate spawner; malformed input -> empty
// -> exit 2.
WorkerArgs ParseArgs(const wchar_t* cmd) {
    WorkerArgs out;
    if (!cmd) return out;
    const wchar_t* p = cmd;
    while (*p) {
        while (*p == L' ') ++p;
        if (wcsncmp(p, L"--pipe", 6) == 0 && (p[6] == L' ' || p[6] == L'=')) {
            p += 6;
            if (*p == L'=') ++p;
            while (*p == L' ') ++p;
            const wchar_t* start = p;
            while (*p && *p != L' ') ++p;
            out.pipe_name.assign(start, static_cast<size_t>(p - start));
        } else if (wcsncmp(p, L"--token", 7) == 0 && (p[7] == L' ' || p[7] == L'=')) {
            p += 7;
            if (*p == L'=') ++p;
            while (*p == L' ') ++p;
            const wchar_t* start = p;
            while (*p && *p != L' ') ++p;
            // hex32 chars are ASCII: explicit per-char narrow (no narrowing
            // conversion warning under /W4 — a range-assign would warn C4244).
            out.token.reserve(static_cast<size_t>(p - start));
            for (const wchar_t* q = start; q != p; ++q) {
                out.token.push_back(static_cast<char>(*q));
            }
        } else if (wcsncmp(p, L"--mutex", 7) == 0 && (p[7] == L' ' || p[7] == L'=')) {
            p += 7;
            if (*p == L'=') ++p;
            while (*p == L' ') ++p;
            const wchar_t* start = p;
            while (*p && *p != L' ') ++p;
            out.mutex_family.assign(start, static_cast<size_t>(p - start));
        } else {
            while (*p && *p != L' ') ++p; // skip unknown arg (forward compat)
        }
    }
    return out;
}

// REQ-045 P4-4 (item 3a-1, design §A 안 i): pure model_id -> model-file
// resolution against the registry. `model_id` comes from the session_open
// relay (or "" for the pinned default). Returns the pinned filename when
// model_id is empty or cannot be resolved; an empty string when a non-empty
// model_id is validly referenced but its file is absent (the caller maps
// that to the honest model_missing path). `models_dir` is the resolved
// Common\models directory (empty when LOCALAPPDATA is unavailable).
//
// Policy notes folded in from the P3 tech gate:
//  * (c2) The worker holds ONE model at a time — "single active model" is the
//    standing assumption (interleaved multi-model v2 sessions are out of
//    scope for P3; the dispatcher relays the latest session_open and any
//    mid-stream swap pays one EnsureLoaded Unload+reload).
//  * (c3) registry.json absent/unparseable + a non-empty model_id degrades to
//    the pinned path — the caller surfaces a one-time notice (the honest
//    fallback, pinned 경로 + 1회 로그 공지).
//  * The registry parser ALREADY enforces bare filenames in files[]
//    (IsBareFilename rejects separators/escapes), so no path-injection escape
//    is possible here — we only concatenate onto the fixed models dir.
//  * v0.10.4 R1-3: `resolved_model_id` reports the registry id of the entry
//    the returned path came from. Both pinned branches resolve through
//    ResolveBundledModelId (family-aware) and report that id, so the REQ-057
//    served-model echo reads the id from THIS resolution instead of scanning
//    the registry a second time (single source of truth — the pre-fix echo
//    re-scanned and named the first bundled entry, an ASR model).
std::string ResolveModelFile(const std::string& models_dir,
                             const emebalachat::engine_host_registry::Registry& registry,
                             const std::string& model_id,
                             bool* resolved /* out: false -> pinned fallback */,
                             std::string* resolved_model_id /* out: entry id served */) {
    if (resolved) *resolved = true;
    if (resolved_model_id) resolved_model_id->clear();
    // REQ-048 R2: the pinned branches must return the SAME ABSOLUTE shape as
    // the registry branch below. The bare filename used to make the load
    // resolve against the worker's cwd (the engine dir, via the inherited
    // CreateProcessW lpCurrentDirectory=nullptr), so a present bundled model
    // answered model_missing on every translate - the user-device "local
    // engine never serves" root cause.
    const std::string pinned_name(emebalachat::kPinnedModelFilename.begin(),
                                  emebalachat::kPinnedModelFilename.end());
    auto pinned_abs = [&]() -> std::string {
        if (models_dir.empty()) return std::string{};
        return models_dir + "\\" + pinned_name;
    };
    if (model_id.empty()) {
        // R1-3: the pinned default IS the bundled translate entry; report the
        // id of the same family-aware scan the app's expectation uses.
        if (resolved_model_id) {
            *resolved_model_id =
                emebalachat::engine_host_registry::ResolveBundledModelId(registry);
        }
        return pinned_abs();
    }
    const emebalachat::engine_host_registry::ModelEntry* entry = registry.FindModel(model_id);
    if (!entry || entry->files.empty()) {
        // Unregistered id, or a registry we could not load: fall back to the
        // pinned model and flag it so the caller can log the one-time notice.
        if (resolved) *resolved = false;
        if (resolved_model_id) {
            *resolved_model_id =
                emebalachat::engine_host_registry::ResolveBundledModelId(registry);
        }
        return pinned_abs();
    }
    // R1-3: the entry RESOLVED (its id is what this call serves, whether or
    // not the file check below passes) — the echo reads this, not a rescan.
    if (resolved_model_id) *resolved_model_id = entry->id;
    if (models_dir.empty()) return std::string{};
    const std::wstring w = emebalachat::ToUtf16(models_dir) + L"\\" +
        emebalachat::ToUtf16(entry->files[0]);
    const DWORD attrs = ::GetFileAttributesW(w.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return std::string{}; // referenced but absent -> model_missing
    }
    return emebalachat::ToUtf8(w);
}

// ---- pipe client (§1.2: the worker is the CLIENT) ---------------------------
bool OpenWorkerPipe(const std::wstring& name, HANDLE& out) {
    for (int attempt = 0; attempt < 60; ++attempt) { // ~3 s worst case (spawn race)
        out = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE,
                            0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (out != INVALID_HANDLE_VALUE) return true;
        const DWORD err = ::GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            (void)::WaitNamedPipeW(name.c_str(), 2000);
            continue;
        }
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ||
            err == ERROR_ACCESS_DENIED) {
            // The pipe instance may not exist yet right after spawn; pace it.
            ::Sleep(50);
            continue;
        }
        return false; // anything else is fatal
    }
    return false;
}

// One frame write as ONE pipe message (§4.3). Synchronous-with-overlap: the
// call returns only after the frame is fully accepted — the worker half of
// the M-2 flush guarantee.
bool WriteFrame(HANDLE pipe, std::string_view json) {
    std::string frame;
    frame.reserve(4 + json.size());
    const uint32_t len = static_cast<uint32_t>(json.size());
    for (unsigned i = 0; i < 4; ++i) {
        frame.push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
    }
    frame.append(json);
    OVERLAPPED ol = {};
    ol.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ol.hEvent) return false;
    DWORD written = 0;
    BOOL ok = ::WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, &ol);
    if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
        ok = ::GetOverlappedResult(pipe, &ol, &written, TRUE);
    }
    const bool success = ok && written == frame.size();
    ::CloseHandle(ol.hEvent);
    return success;
}

// One message-mode read. Outcomes:
//   ReadOk        -> json holds a complete frame
//   ReadTimeout   -> nothing arrived within timeout_ms (connection alive)
//   ReadIoError   -> peer gone / cap violation / bad header (tear down)
enum class ReadOutcome { Ok, Timeout, IoError };

ReadOutcome ReadFrame(HANDLE pipe, std::string& json, DWORD timeout_ms) {
    static thread_local std::vector<char> buf;
    if (buf.size() < kPipeBufSize) buf.resize(kPipeBufSize);
    OVERLAPPED ol = {};
    ol.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ol.hEvent) return ReadOutcome::IoError;
    DWORD read = 0;
    BOOL ok = ::ReadFile(pipe, buf.data(), kPipeBufSize, &read, &ol);
    if (!ok && ::GetLastError() == ERROR_IO_PENDING) {
        const DWORD w = ::WaitForSingleObject(ol.hEvent, timeout_ms);
        if (w == WAIT_TIMEOUT) {
            // Cancel ONLY this pending read; the connection stays usable.
            ::CancelIoEx(pipe, &ol);
            ::GetOverlappedResult(pipe, &ol, &read, TRUE); // drain the cancelled IO
            ::CloseHandle(ol.hEvent);
            return ReadOutcome::Timeout;
        }
        if (w != WAIT_OBJECT_0) {
            ::CancelIoEx(pipe, &ol);
            // Backend P1-1: drain the kernel cancel-ack BEFORE the stack
            // OVERLAPPED/buffer go out of scope (the overlapped_io_util
            // contract — REQ-L02).
            DWORD cancelled = 0;
            (void)::GetOverlappedResult(pipe, &ol, &cancelled, TRUE);
            ::CloseHandle(ol.hEvent);
            return ReadOutcome::IoError;
        }
        ok = ::GetOverlappedResult(pipe, &ol, &read, TRUE);
    }
    const DWORD readErr = ::GetLastError();
    ::CloseHandle(ol.hEvent);
    if (!ok) {
        if (readErr == ERROR_MORE_DATA) {
            DIAG_F("ENGINEHOST/worker/010: frame exceeds the 1 MiB cap; disconnecting\n");
        }
        return ReadOutcome::IoError; // ERROR_BROKEN_PIPE = orchestrator gone
    }
    if (read < enginehost::kFrameHeaderSize) return ReadOutcome::IoError;
    uint32_t len = 0;
    enginehost::FrameReadLengthPrefix(buf.data(), len);
    if (len > enginehost::kMaxFrameBytes ||
        static_cast<size_t>(len) + enginehost::kFrameHeaderSize != read) {
        DIAG_F("ENGINEHOST/worker/011: bad frame header (len=%u read=%lu)\n", len, read);
        return ReadOutcome::IoError;
    }
    json.assign(buf.data() + enginehost::kFrameHeaderSize, static_cast<size_t>(len));
    return ReadOutcome::Ok;
}

// ---- process-lifetime cancel flag (D-2 chain b: address stability) ---------
// ONE atomic for the whole worker lifetime; its ADDRESS is handed to the
// engine's llama abort/progress callbacks at context creation and stays
// stable (translation_common.hpp contract). Values toggle per job — exactly
// the host_main.cpp g_abort pattern, relocated into the worker process.
std::atomic<bool> g_cancel{false};

// REQ-051 U-1 FIX 2: the LOAD-side cancellation flag. The engine's
// load_cancel_flag is pointed at THIS never-set atomic: a per-job abort
// (g_cancel) unwinds the in-flight DECODE, but a multi-minute model load —
// once started — always runs to completion. The old single-flag behavior
// discarded the load on abort, so the next request repaid the full load and
// a busy user's retry spiral never converged.
std::atomic<bool> g_never_cancel_load{false};

// The job id currently in flight (0 = none). M6 ggml-translate serves ONE
// context at a time (registry max_sessions=1), so a single slot suffices.
std::atomic<std::uint64_t> g_current_job{0};

// Backend P0-1 (v0.10.5): abort frames are consumed by the READER thread, so
// an abort targeting a job still sitting in the single-slot queue (not yet
// started) is remembered here; the inference thread exchanges the session id
// at job start and answers "timeout" without ever running the work. The
// wire contract carries only the session (worker_protocol.hpp BuildAbort),
// and JobMsg.session is the matching handle.
std::atomic<std::uint64_t> g_pending_abort_session{0};

// Backend P0-1: both threads write frames now (the reader: heartbeats and
// control replies; the inference thread: terminal events). WriteFrame itself
// is not thread-safe; this mutex serializes the writes so two frames can
// never interleave on the pipe.
std::mutex g_pipe_write_mu;

bool WriteFrameThreadSafe(HANDLE pipe, std::string_view json) {
    std::lock_guard<std::mutex> lk(g_pipe_write_mu);
    return WriteFrame(pipe, json);
}

// Per-family single-instance mutex handle (design §4.2).
HANDLE g_mutex = nullptr;

// The announce manifest (embedded §3.4 constants; one source for both the
// deployed worker.manifest and the announce frame).
wp::WorkerManifest g_manifest;

// ---- the frame loop (design §1.2 items 3-4) --------------------------------
// Returns the process exit code. 0 = graceful (shutdown acknowledged);
// 3 = protocol/IO break (the orchestrator ReaperLoop judges respawn).
// `token` is the boot-scoped value the orchestrator passed on our command
// line; it is echoed in the announce frame for orchestrator re-validation
// (security audit finding 2, M6 T6) — never logged (shape-only discipline).
int FrameLoop(HANDLE pipe, std::string_view token) {
    // Engine created ON this thread: llama_backend_init + the model lifecycle
    // stay single-threaded (v1 InferenceLoop discipline, §4.5). Backend P0-1
    // (v0.10.5): this thread is the INFERENCE executor; the dedicated reader
    // thread below owns the pipe, so an abort/shutdown frame (or a pipe break)
    // is consumed WHILE engine->Translate() runs instead of sitting unread in
    // the kernel buffer for the whole decode budget. The pre-fix single-
    // threaded loop answered aborts only after Translate returned, making
    // host cancellation a no-op mid-inference and stranding the worker's
    // ~2 GiB of VRAM for up to 30 s after a host give-up.
    auto engine = std::make_unique<LocalInferenceEngine>();
    engine->cancel_flag = &g_cancel; // address-stability contract (D-2)
    // REQ-051 U-1 FIX 2: the load leg never cancels on a per-job abort — a
    // started load always finishes (see g_never_cancel_load above). Without
    // this wiring load_cancel_flag aliases cancel_flag (the legacy behavior).
    engine->load_cancel_flag = &g_never_cancel_load;

    // REQ-045 P4-4 (item 3a-1): the models directory + registry.json are
    // resolved ONCE at frame-loop start (same common location as the host;
    // the worker owns EnsureLoaded incl. the SHA pin + marker cache +
    // CUDA->CPU retry). The per-job model PATH is decided dynamically from
    // the latest session_open model_id — see the job branch below.
    PWSTR known = nullptr;
    std::wstring lad;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known)) && known) {
        lad = known;
        ::CoTaskMemFree(known);
    }
    // REQ-044 (P4-2): the inline L"Emebala\\Common\\models\\" literal is
    // now paths::kModelsDirRel (engine_host_paths.hpp).
    const std::wstring models_dir_w =
        lad.empty() ? std::wstring{}
                    : lad + L"\\" + emebalachat::enginehost::paths::kModelsDirRel;
    const std::string models_dir = models_dir_w.empty() ? std::string{}
                                                        : emebalachat::ToUtf8(models_dir_w);
    if (models_dir.empty()) {
        DIAG_F("ENGINEHOST/worker/020: cannot resolve the common model path; jobs answer model_missing\n");
    }
    // REQ-045 (c3): a missing/unparseable registry is NOT fatal — a set
    // model_id then degrades to the pinned model (the resolver flags it and
    // we surface the one-time notice below).
    emebalachat::engine_host_registry::Registry registry;
    bool registry_loaded = false;
    {
        const auto lr = emebalachat::engine_host_registry::LoadDefaultRegistry();
        if (lr.status == emebalachat::engine_host_registry::LoadStatus::Ok) {
            registry = std::move(lr.registry);
            registry_loaded = true;
        }
    }
    // Latest model_id relayed by the orchestrator's session_open ("" until
    // the first one — the pinned default). Single-slot: one worker process
    // serves ONE model at a time (tech gate c2 — interleaved multi-model v2
    // sessions are out of scope for P3).
    std::string active_model_id;
    // Lazily-reported fallback notice (c3): set on the first unresolved
    // model_id, logged once, then cleared.
    bool fallback_notice_pending = false;
    // REQ-057 + v0.10.4 R1-3: `resolved_model_id` is the served-model echo's
    // SINGLE source of truth — the registry id of the entry the resolved path
    // came from (both pinned branches report the family-aware bundled id).
    // Reported by ResolveModelFile; the caller must NOT re-scan the registry
    // (the pre-fix job branch did, and named the first bundled entry — an ASR
    // model — while the pinned Hy-MT2 served).
    auto resolve_active_model = [&](std::string* resolved_model_id) -> std::string {
        bool resolved = true;
        const std::string path =
            ResolveModelFile(models_dir, registry, active_model_id, &resolved, resolved_model_id);
        if (!resolved && !fallback_notice_pending) {
            fallback_notice_pending = true; // one-time pinned-path notice (c3)
            DIAG_LOG("ENGINEHOST",
                     "worker/030: model_id '%s' unresolved (registry_loaded=%d); "
                     "falling back to the pinned model",
                     active_model_id.c_str(), registry_loaded ? 1 : 0);
        }
        return path;
    };

    // ---- announce (§3.4): worker.manifest content + the echoed token as the
    // FIRST frame (audit finding 2: the orchestrator re-validates the token).
    if (!WriteFrame(pipe, wp::BuildAnnounce(g_manifest, token))) {
        DIAG_F("ENGINEHOST/worker/021: announce write failed (orchestrator gone)\n");
        return 3;
    }
    DIAG_LOG("ENGINEHOST", "worker/001: announced (family=%s abi=%d proto=%d-%d token_len=%zu)",
             g_manifest.family.c_str(), g_manifest.abi_version,
             g_manifest.protocol_min, g_manifest.protocol_max, token.size());

    // ---- Backend P0-1: the reader thread and the single-slot command queue --
    // Ordered commands (job / session_open / close) flow reader -> inference;
    // control frames (abort / shutdown / heartbeat) are acted on by the reader
    // the moment they arrive, so they are never hostage to a running decode.
    struct Command {
        std::string op;
        std::string json;
        bool shutdown = false;
    };
    struct CommandQueue {
        std::mutex mu;
        std::condition_variable cv;
        Command cmd;       // one pending ordered command; empty json = none
        bool stop = false; // the reader decided the process must exit ...
        int exit_code = 0; // ...with this code (3 = protocol/IO break)
    } queue;
    std::atomic<bool> shutdown_ordered{false};
    // Backend FIX 3 (review round 1, F-08): set by the inference thread on
    // EVERY exit path (error breaks and the normal shutdown alike) so the
    // reader leaves its 250 ms read slice and reader.join() returns promptly
    // even when the peer holds its pipe end open. Previously the join relied
    // on the peer closing the pipe.
    std::atomic<bool> stop_reader{false};

    std::thread reader([&] {
        std::uint64_t last_heartbeat_ms = static_cast<std::uint64_t>(NowMs());
        const auto die = [&](int code) {
            // Unwind any in-flight decode/load promptly (pipe break or
            // protocol error) and hand the exit decision to the inference
            // thread, which owns the model teardown.
            g_cancel.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lk(queue.mu);
            queue.stop = true;
            queue.exit_code = code;
            queue.cv.notify_one();
        };
        for (;;) {
            if (shutdown_ordered.load(std::memory_order_acquire) ||
                stop_reader.load(std::memory_order_acquire)) {
                return; // shutdown ack below, or the inference thread is done
            }
            // Bounded read so the heartbeat cadence holds with no inbound
            // traffic.
            std::string json;
            const ReadOutcome rc = ReadFrame(pipe, json, 250);
            if (rc == ReadOutcome::IoError) {
                DIAG_F("ENGINEHOST/worker/012: pipe read failed; exiting (err=%lu)\n",
                       ::GetLastError());
                die(3);
                return;
            }
            if (rc == ReadOutcome::Timeout) {
                const std::uint64_t now = static_cast<std::uint64_t>(NowMs());
                if (now - last_heartbeat_ms >= wp::kWorkerHeartbeatIntervalMs) {
                    if (!WriteFrameThreadSafe(pipe, wp::BuildHeartbeat(now))) {
                        DIAG_F("ENGINEHOST/worker/022: heartbeat write failed (orchestrator gone)\n");
                        die(3);
                        return;
                    }
                    last_heartbeat_ms = now;
                }
                continue;
            }
            last_heartbeat_ms = NowMs(); // inbound traffic also proves liveness

            enginehost::JsonPairs fields;
            if (!enginehost::JsonParseObject(json, fields)) {
                (void)WriteFrameThreadSafe(pipe, wp::BuildError(enginehost::kErrBadRequest));
                die(3);
                return; // protocol violation -> the orchestrator tears the session
            }
            const auto* opField = enginehost::detail::FindField(fields, "op");
            const std::string op = (opField && opField->is_string) ? opField->text : "";

            if (op == "abort") {
                wp::AbortMsg msg;
                if (!wp::ParseAbort(json, msg)) {
                    (void)WriteFrameThreadSafe(pipe, wp::BuildError(enginehost::kErrBadRequest));
                    die(3);
                    return;
                }
                // D-2 chain b: g_cancel's address is wired into the llama
                // abort/progress callbacks, so this unwinds a RUNNING decode
                // or load between tokens within milliseconds. A job still
                // QUEUED behind the running one is remembered by id; the
                // inference thread answers it "timeout" without starting the
                // work. An abort for an unknown id stays a no-op (the v1
                // rule: only the in-flight job is aborted).
                if (g_current_job.load(std::memory_order_acquire) != 0) {
                    g_cancel.store(true, std::memory_order_release);
                }
                g_pending_abort_session.store(msg.session, std::memory_order_release);
                continue;
            }
            if (op == "heartbeat") {
                wp::HeartbeatMsg hb;
                if (!wp::ParseHeartbeat(json, hb)) {
                    (void)WriteFrameThreadSafe(pipe, wp::BuildError(enginehost::kErrBadRequest));
                    die(3);
                    return;
                }
                continue; // liveness marker only; nothing to answer (§1.2)
            }
            if (op == "shutdown") {
                if (!wp::ParseShutdown(json)) {
                    (void)WriteFrameThreadSafe(pipe, wp::BuildError(enginehost::kErrBadRequest));
                    die(3);
                    return;
                }
                // M-2 order kept: the reader only unwinds the in-flight decode
                // (g_cancel) so the inference thread's Unload below is not
                // blocked by a multi-second llama call; unload + shutdown_ack
                // (the LAST write) + exit 0 run on the inference thread.
                DIAG_LOG("ENGINEHOST", "worker/024: shutdown requested; aborting + unloading model");
                g_cancel.store(true, std::memory_order_release);
                shutdown_ordered.store(true, std::memory_order_release);
                {
                    std::lock_guard<std::mutex> lk(queue.mu);
                    queue.cmd = Command{std::string(), std::move(json), /*shutdown=*/true};
                    queue.cv.notify_one();
                }
                return; // nothing further is read after a shutdown
            }
            // Ordered commands (job / session_open / close): the single slot.
            // The orchestrator contract is ONE job in flight, so the slot is
            // almost always empty; a superseded unstarted frame is dropped
            // (newest wins) and logged shape-only.
            {
                std::lock_guard<std::mutex> lk(queue.mu);
                if (!queue.cmd.json.empty()) {
                    DIAG_F("ENGINEHOST/worker/031: an unstarted command was superseded (op=%s len=%zu)\n",
                           queue.cmd.op.c_str(), queue.cmd.json.size());
                }
                queue.cmd = Command{op, std::move(json), /*shutdown=*/false};
                queue.cv.notify_one();
            }
        }
    });

    // ---- the inference loop (this thread owns the engine) ----
    std::uint64_t event_seq = 0;
    int exit_code = 0;
    for (;;) {
        Command cmd;
        {
            std::unique_lock<std::mutex> lk(queue.mu);
            queue.cv.wait(lk, [&] { return queue.stop || !queue.cmd.json.empty(); });
            if (!queue.cmd.json.empty()) {
                cmd = std::move(queue.cmd);
                queue.cmd = Command{};
            } else {
                exit_code = queue.exit_code;
                break; // reader stop with no further command
            }
        }

        if (cmd.shutdown) {
            // M-2 order (worker half): every prior event was flushed
            // synchronously (WriteFrameThreadSafe) before this point. Unload
            // the model, send shutdown_ack LAST, exit 0. The orchestrator
            // waits for this ack (or EOF) BEFORE closing the pipe.
            engine->Unload();
            if (!WriteFrameThreadSafe(pipe, wp::BuildShutdownAck())) {
                DIAG_F("ENGINEHOST/worker/025: shutdown_ack write failed (orchestrator gone)\n");
                exit_code = 0; // the pipe is already dead; graceful-enough exit
                break;
            }
            exit_code = 0;
            break; // FrameLoop returns 0 -> wWinMain exits 0
        }

        const std::string& op = cmd.op;
        if (op == "job") {
            wp::JobMsg job;
            if (!wp::ParseJob(cmd.json, job)) {
                (void)WriteFrameThreadSafe(pipe, wp::BuildError(enginehost::kErrBadRequest));
                exit_code = 3;
                break;
            }
            g_current_job.store(job.job, std::memory_order_release);
            // Clear the cancel flag BEFORE the work starts so a stale abort
            // can never kill the next job (v1 InferenceLoop discipline).
            g_cancel.store(false, std::memory_order_release);
            // Backend P0-1: an abort that landed while this job sat in the
            // queue is honored here — the work never starts and the job gets
            // the same "timeout" answer as a mid-decode abort.
            if (g_pending_abort_session.exchange(0, std::memory_order_acq_rel) == job.session) {
                g_current_job.store(0, std::memory_order_release);
                wp::EventMsg ev;
                ev.session = job.session;
                ev.seq = ++event_seq;
                ev.kind = wp::EventKind::Error;
                ev.code = "timeout";
                if (!WriteFrameThreadSafe(pipe, wp::BuildEvent(ev))) {
                    DIAG_F("ENGINEHOST/worker/023: event write failed (orchestrator gone)\n");
                    exit_code = 3;
                    break;
                }
                continue;
            }

            // REQ-045 P4-4: resolve the model for THIS job from the latest
            // session_open model_id ("" -> pinned; unresolvable -> pinned with
            // the one-time notice; referenced-but-absent -> model_missing).
            // REQ-057 + v0.10.4 R1-3: the ACTUALLY-SERVED model id echo
            // (privacy-safe: registry id metadata, never user content) is
            // reported by the SAME resolution that produced the path — no
            // second registry scan. A resolved non-empty id names itself; the
            // pinned branches report the bundled TRANSLATE-family entry (the
            // same family-aware id the app's expected-id check uses). Empty
            // when the registry names no bundled entry -> pre-REQ-057 shape.
            std::string served_model_id;
            const std::string model_path = resolve_active_model(&served_model_id);
            const bool model_ok = !model_path.empty();
            bool loaded = false;
            std::wstring out;
            if (model_ok) {
                // Load BEFORE inferencing: a missing/corrupt/undecodable GGUF
                // maps to model_missing (plan §8); a decode-time failure after
                // a SUCCESSFUL load stays engine_failed. A cancel-aborted load
                // surfaces through the abort check below (timeout precedence).
                //
                // REQ-045 (tech gate c1): a mid-stream model SWAP is validated
                // HERE, before the job is answered. EnsureLoaded unloads the
                // previous model and reloads the resolved one; when that
                // reload FAILS the worker stays alive on its previous model
                // (loaded_path is left pointing at the last good load) and
                // this job alone answers the honest model_missing — no
                // half-loaded state is acked. If the reload CRASHES the
                // process instead, the orchestrator maps the pipe IO error to
                // EngineFailed + respawn backoff (the existing §V2-8.6 chain);
                // the next request re-attempts the load. Non-HyMT models run
                // through the fixed hymt2-official prompt either way (§A.4:
                // 번역 품질 미보장), so a bad swap degrades honestly.
                loaded = engine->EnsureLoaded(model_path);
                if (loaded) {
                    // Prompt parity with the embedded/host path: the target
                    // token resolves to name_en, the source passes through
                    // raw; per-request sampling profile (plan §V2-5.2) with
                    // the shipped defaults when the job carried none.
                    const std::string norm_tgt = emebalachat::NormalizeLanguageCode(job.tgt);
                    const emebalachat::LanguageInfo* tgt_info =
                        emebalachat::FindLanguageByCode(norm_tgt);
                    const std::string tgt_name = tgt_info ? tgt_info->name_en : job.tgt;
                    // REQ-CP T4+T5 (design §9 rows 6-7 + tech-gate A2): the
                    // prompt template ref threads to the rung-1 prompt builder.
                    // T5 connected the wire: the job frame's OPTIONAL
                    // prompt_template ref (absent -> "" = hymt2-official, the
                    // fail-closed default the template registry resolves) is
                    // forwarded here; a non-empty ref selects subtitle-
                    // realtime-v1 / literary-flow-v1, an unknown ref falls back
                    // to hymt2-official.
                    out = engine->Translate(
                        emebalachat::ToUtf16(job.text), tgt_name, job.src, model_path,
                        job.sampling.temperature, job.sampling.top_p,
                        job.sampling.top_k, job.sampling.rep_pen,
                        /*template_ref=*/job.prompt_template);
                }
            }
            const bool aborted = g_cancel.load(std::memory_order_acquire);
            // REQ-051 U-1 FIX 1: a decode that exhausted its wall-clock budget
            // answers the SAME "timeout" code as a cancel-aborted job — the
            // wire vocabulary is unchanged (no new frame shape) and the frozen
            // client classifies "timeout" as TRANSIENT, so the app's one-shot
            // retry fires instead of the classification-as-Permanent
            // engine_failed no-retry dead end.
            // 260930_0003 (다): over-budget inputs are chunked inside
            // LocalInferenceEngine::Translate (paragraph boundaries, stitched
            // with the original separators); each chunk decodes under its own
            // FRESH wall-clock budget, so the budget below is per chunk and a
            // chunked job cannot multiply a single decode's timeout risk.
            const bool decode_wall_clock_exhausted = engine->decode_wall_clock_exhausted();
            g_current_job.store(0, std::memory_order_release);

            wp::EventMsg ev;
            ev.session = job.session;
            ev.seq = ++event_seq;
            if (aborted || decode_wall_clock_exhausted) {
                // Cancel contract: the orchestrator maps an aborted job to
                // status=timeout toward the client (v1 §4.4 behavior). The
                // wall-clock-exhausted leg shares the exact same answer.
                ev.kind = wp::EventKind::Error;
                ev.code = "timeout";
            } else if (!model_ok || !loaded) {
                ev.kind = wp::EventKind::Error;
                ev.code = "model_missing";
            } else if (out.empty()) {
                ev.kind = wp::EventKind::Error;
                ev.code = "engine_failed";
            } else {
                ev.kind = wp::EventKind::Final;
                ev.text = emebalachat::ToUtf8(out);
            }
            // REQ-057: echo the served id on EVERY terminal event of the job
            // (final AND error kinds) — diagnosability on failures matters.
            ev.model = served_model_id;
            if (!WriteFrameThreadSafe(pipe, wp::BuildEvent(ev))) {
                DIAG_F("ENGINEHOST/worker/023: event write failed (orchestrator gone)\n");
                exit_code = 3;
                break;
            }
            DIAG_LOG("ENGINEHOST", "worker/002: job %llu answered (kind=%s seq=%llu)",
                     static_cast<unsigned long long>(job.job),
                     ev.kind == wp::EventKind::Final ? "final" : "error",
                     static_cast<unsigned long long>(ev.seq));
        } else if (op == "session_open") {
            wp::SessionOpenMsg msg;
            if (!wp::ParseSessionOpen(cmd.json, msg)) {
                (void)WriteFrameThreadSafe(pipe, wp::BuildError(enginehost::kErrBadRequest));
                exit_code = 3;
                break;
            }
            if (msg.capability != "translate") {
                // Not our capability: inline refusal (fail-closed, no tear-down
                // — the orchestrator routes capability mismatches).
                (void)WriteFrameThreadSafe(pipe, wp::BuildError("unavailable"));
                continue;
            }
            // REQ-045 P4-4: the session opens unconditionally still, but the
            // relayed model_id now BECOMES the active model for subsequent
            // jobs (resolved per job against the registry inside the job
            // branch — see resolve_active_model). An empty model_id keeps the
            // pinned default (the pre-REQ-045 behavior, byte-identical).
            active_model_id = msg.model_id;
            // REQ-060: warmup "미리 로드" — an explicit preload asks the worker
            // to resolve the just-relayed model_id and EnsureLoaded it NOW, so
            // the model sits resident in VRAM before the opened ack (the first
            // real job then skips the multi-second load; the worker holds one
            // model without unload, so post-load residency is automatic).
            // The load runs on the inference thread — the engine's owning
            // thread — and blocks for the whole load, which is acceptable by
            // design (the orchestrator awaits opened before dispatching
            // anyway, and Backend P0-1 keeps the pipe reader independent, so
            // abort/shutdown stay responsive for the whole preload window).
            // Failure is NON-fatal: one shape-only diag line and the session
            // still opens normally — the first job re-attempts EnsureLoaded
            // through the ordinary model_missing path.
            if (msg.preload) {
                const std::string preload_path = resolve_active_model(nullptr);
                if (!preload_path.empty() && engine->EnsureLoaded(preload_path)) {
                    // Shape-only (host/004 contract): the id's length, never
                    // its content, goes to the diag log.
                    DIAG_LOG("ENGINEHOST",
                             "worker/028: preload kept model resident (id_len=%zu)",
                             active_model_id.size());
                } else {
                    DIAG_LOG("ENGINEHOST",
                             "worker/029: preload load failed; session opens unloaded "
                             "(first job retries EnsureLoaded)");
                }
            }
            if (!WriteFrameThreadSafe(pipe, wp::BuildOpened(msg.session))) {
                exit_code = 3;
                break;
            }
        } else if (op == "close") {
            std::uint64_t session = 0;
            if (!wp::ParseClose(cmd.json, session)) {
                (void)WriteFrameThreadSafe(pipe, wp::BuildError(enginehost::kErrBadRequest));
                exit_code = 3;
                break;
            }
            if (!WriteFrameThreadSafe(pipe, wp::BuildClosed(session))) {
                exit_code = 3;
                break;
            }
        } else if (op == "abort" || op == "heartbeat") {
            // Backend P0-1: reader-owned control frames. Unreachable here (the
            // reader intercepts them before queueing); kept harmless if a
            // future change ever routes one into the queue.
            continue;
        } else {
            // §V2-12-2: unknown ops converge to error + close (fail-closed).
            DIAG_F("ENGINEHOST/worker/026: unknown op rejected (len=%zu)\n", cmd.json.size());
            (void)WriteFrameThreadSafe(pipe, wp::BuildError(enginehost::kErrBadRequest));
            exit_code = 3;
            break;
        }
    }
    // Backend FIX 3 (review round 1, F-08): reached from EVERY inference exit
    // path (all `exit_code = 3; break;` error branches, the shutdown branch
    // and the reader-stop branch alike) — signal the reader so it leaves its
    // read slice and the join below returns promptly even when the peer holds
    // its pipe end open.
    stop_reader.store(true, std::memory_order_release);
    // Join the reader before returning: no thread may touch the pipe after
    // wWinMain closes it, and a reader blocked in its 250 ms read slice must
    // not outlive the engine teardown.
    if (reader.joinable()) reader.join();
    return exit_code;
}

} // namespace

int WINAPI wWinMain(HINSTANCE /*hInstance*/, HINSTANCE, PWSTR pCmdLine, int) {
    using namespace emebalachat;

    // L3 contract, identical to main.cpp/host_main.cpp: remove the CWD from
    // the DLL search order (ggml DLLs resolve via app dir + PATH only).
    if (!::SetDllDirectoryW(L"")) {
        DIAG_F("ENGINEHOST/worker/SetDllDirectory/001: SetDllDirectoryW(L\"\") failed (err=%lu)\n",
               ::GetLastError());
    }

    // Shape-only logging; the file sink stays OFF (no config file in a worker;
    // the DIAG_F stderr mirror still reports hard errors).
    (void)diag::Init();

    // P5-F1: driverless machines must CPU-fall back, never SEH 0xC06D007E.
    (void)EnsureVulkanGuard();

    // ---- handshake arguments (§1.2) ----
    // B3 fix (session 260928_0001): parsed BEFORE the single-instance gate so
    // the mutex name can be derived PER FAMILY (wp::MutexNameForFamily). The
    // B-T4 pool spawns one worker per family; under the old family-agnostic
    // constant the second family's worker collided here, exited 0 pre-connect,
    // and wedged the host in a 15 s ConnectNamedPipe timeout + model_missing.
    const WorkerArgs args = ParseArgs(pCmdLine);

    // Single instance per family (design §4.2): the mutex name is derived
    // from the family arg (--mutex); an arg-absent spawn (old orchestrator)
    // falls back to the exact legacy constant. A duplicate same-family spawn
    // exits quietly; the orchestrator's respawn path retries on the next
    // dispatch. Distinct families derive distinct names, so the per-family
    // pool coexists.
    const std::wstring mutex_name = wp::MutexNameForFamily(args.mutex_family);
    g_mutex = ::CreateMutexW(nullptr, TRUE, mutex_name.c_str());
    if (!g_mutex || ::GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_mutex) ::CloseHandle(g_mutex);
        diag::Shutdown();
        return 0;
    }
    // Backend FIX 4 (review round 1, F-11): the boot token arrives via the
    // child ENVIRONMENT (the 0.10.5+ orchestrator never puts it on the
    // command line — same-user processes can read a child argv). An OLD
    // orchestrator still passes --token, so the argv value remains the
    // fallback. Shape-only discipline: the token is never logged.
    std::string token;
    {
        // Query size first: the return value counts the terminating NUL, so
        // the read buffer must hold `need` characters (capacity need, then
        // resize down to the returned length).
        const DWORD need = ::GetEnvironmentVariableW(kWorkerTokenEnvName, nullptr, 0);
        if (need > 1) {
            std::wstring buf(need, L'\0');
            if (::GetEnvironmentVariableW(kWorkerTokenEnvName, buf.data(), need) == need - 1) {
                buf.resize(need - 1);
                token.reserve(buf.size());
                for (const wchar_t c : buf) {
                    if (c < 0x80) token.push_back(static_cast<char>(c));
                }
            }
        }
    }
    if (token.empty()) token = args.token; // old-host --token fallback

    const bool token_ok = token.size() == 32 &&
        std::all_of(token.begin(), token.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    if (args.pipe_name.empty() || args.pipe_name.size() > 200 || !token_ok) {
        DIAG_F("ENGINEHOST/worker/002: bad command line (pipe=%zu token=%zu)\n",
               args.pipe_name.size(), token.size());
        ::ReleaseMutex(g_mutex);
        ::CloseHandle(g_mutex);
        diag::Shutdown();
        return 2;
    }

    // The embedded manifest (§3.4).
    g_manifest = wp::EmbeddedWorkerManifest();

    // ---- connect (the orchestrator owns the server end) ----
    HANDLE pipe = nullptr;
    if (!OpenWorkerPipe(args.pipe_name, pipe)) {
        DIAG_F("ENGINEHOST/worker/003: cannot connect to the worker pipe (err=%lu)\n",
               ::GetLastError());
        ::ReleaseMutex(g_mutex);
        ::CloseHandle(g_mutex);
        diag::Shutdown();
        return 3;
    }

    // Message mode on the client end (the server created PIPE_TYPE_MESSAGE).
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!::SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
        DIAG_F("ENGINEHOST/worker/004: SetNamedPipeHandleState failed (err=%lu)\n",
               ::GetLastError());
        ::CloseHandle(pipe);
        ::ReleaseMutex(g_mutex);
        ::CloseHandle(g_mutex);
        diag::Shutdown();
        return 3;
    }

    const int exit_code = FrameLoop(pipe, token);
    ::CloseHandle(pipe);
    DIAG_LOG("ENGINEHOST", "worker/027: exiting (code=%d)", exit_code);
    diag::Shutdown();
    ::ReleaseMutex(g_mutex);
    ::CloseHandle(g_mutex);
    return exit_code;
}
