// ---------------------------------------------------------------------------
// ggml_translate_worker — REQ-043 (M6 T3, design §1.2, plan §V2-3): the
// Emebalachat.Engine.ggml-translate.exe worker process (WIN32 subsystem).
//
// Second frozen contract (난부 계약, §V2-3): the orchestrator creates the
// PRIVATE named pipe (server side) and spawns this exe with the pipe name +
// boot-scoped token on the command line; this process connects as a CLIENT.
// Wire frames are the §4.3 [u32 LE][UTF-8 JSON] reused unmodified (1 MiB cap,
// one frame = one pipe message); the message vocabulary lives in
// worker_protocol.hpp (header-only, unit-tested without any process).
//
// Lifecycle (design §1.2):
//   1. wWinMain: SetDllDirectoryW(L""), diag::Init, EnsureVulkanGuard,
//      single-instance mutex Local\EmebalaEngine_Worker_ggml_translate.
//   2. Parse argv: --pipe <name> --token <hex32>. Missing/garbled -> exit 2
//      (the orchestrator ReaperLoop judges the exit code).
//   3. Connect to the pipe (user-only SD enforced by the kernel — the
//      orchestrator built the pipe with the BuildUserOnlySd pattern; this
//      process just CreateFileWs it), send announce (worker.manifest §3.4).
//   4. Frame loop (single inference thread): job -> LocalInferenceEngine::
//      Translate (per-request sampling profile; cancel_flag address stability
//      contract from translation_common.hpp — one process-lifetime atomic,
//      D-2 chain b->c) -> event final | error. abort -> cancel_flag store.
//      close -> closed. heartbeats every 5 s while idle. shutdown -> unload
//      model -> shutdown_ack -> exit 0.
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
// on the command line of this short-lived child; the user-only pipe ACL is
// enforced at the kernel boundary), shape-only ENGINEHOST/worker/NNN
// diagnostics (stderr mirror only; the file sink stays off — diag defaults),
// user text never logged from this layer.
// Delay-load rules identical to the host: CUDA (+Vulkan when built) DLLs are
// delay-loaded by CMake; on a driverless machine EnsureVulkanGuard + the
// EnsureLoaded CUDA->CPU retry keep inference alive.
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
#include <cstdint>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

namespace {

using emebalachat::LocalInferenceEngine;
namespace wp = emebalachat::workerproto;
namespace enginehost = emebalachat::enginehost;

constexpr DWORD kPipeBufSize = (1u << 20) + 4; // fits the max legal frame

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- command line (§1.2 handshake: pipe name + boot-scoped token) ----------
struct WorkerArgs {
    std::wstring pipe_name;
    std::string token;
};

// Minimal argv walker: --pipe <value> / --token <value>. The orchestrator is
// the only legitimate spawner; malformed input -> empty -> exit 2.
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
std::string ResolveModelFile(const std::string& models_dir,
                             const emebalachat::engine_host_registry::Registry& registry,
                             const std::string& model_id,
                             bool* resolved /* out: false -> pinned fallback */) {
    if (resolved) *resolved = true;
    if (model_id.empty()) {
        return std::string(emebalachat::kPinnedModelFilename.begin(),
                           emebalachat::kPinnedModelFilename.end());
    }
    const emebalachat::engine_host_registry::ModelEntry* entry = registry.FindModel(model_id);
    if (!entry || entry->files.empty()) {
        // Unregistered id, or a registry we could not load: fall back to the
        // pinned model and flag it so the caller can log the one-time notice.
        if (resolved) *resolved = false;
        return std::string(emebalachat::kPinnedModelFilename.begin(),
                           emebalachat::kPinnedModelFilename.end());
    }
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

// The job id currently in flight (0 = none). M6 ggml-translate serves ONE
// context at a time (registry max_sessions=1), so a single slot suffices.
std::atomic<std::uint64_t> g_current_job{0};

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
    // stay single-threaded (v1 InferenceLoop discipline, §4.5).
    auto engine = std::make_unique<LocalInferenceEngine>();
    engine->cancel_flag = &g_cancel; // address-stability contract (D-2)

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
    auto resolve_active_model = [&]() -> std::string {
        bool resolved = true;
        const std::string path = ResolveModelFile(models_dir, registry, active_model_id, &resolved);
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

    std::uint64_t last_heartbeat_ms = static_cast<std::uint64_t>(NowMs());
    std::uint64_t event_seq = 0;

    for (;;) {
        // Bounded read so the heartbeat cadence holds with no inbound traffic.
        std::string json;
        const ReadOutcome rc = ReadFrame(pipe, json, 250);
        if (rc == ReadOutcome::IoError) {
            DIAG_F("ENGINEHOST/worker/012: pipe read failed; exiting (err=%lu)\n",
                   ::GetLastError());
            return 3;
        }
        if (rc == ReadOutcome::Timeout) {
            const std::uint64_t now = static_cast<std::uint64_t>(NowMs());
            if (now - last_heartbeat_ms >= wp::kWorkerHeartbeatIntervalMs) {
                if (!WriteFrame(pipe, wp::BuildHeartbeat(now))) {
                    DIAG_F("ENGINEHOST/worker/022: heartbeat write failed (orchestrator gone)\n");
                    return 3;
                }
                last_heartbeat_ms = now;
            }
            continue;
        }
        last_heartbeat_ms = NowMs(); // inbound traffic also proves liveness

        enginehost::JsonPairs fields;
        if (!enginehost::JsonParseObject(json, fields)) {
            (void)WriteFrame(pipe, wp::BuildError(enginehost::kErrBadRequest));
            return 3; // protocol violation -> the orchestrator tears the session
        }
        const auto* opField = enginehost::detail::FindField(fields, "op");
        const std::string op = (opField && opField->is_string) ? opField->text : "";

        if (op == "job") {
            wp::JobMsg job;
            if (!wp::ParseJob(json, job)) {
                (void)WriteFrame(pipe, wp::BuildError(enginehost::kErrBadRequest));
                return 3;
            }
            g_current_job.store(job.job, std::memory_order_release);
            // Clear the cancel flag BEFORE the work starts so a stale abort
            // can never kill the next job (v1 InferenceLoop discipline).
            g_cancel.store(false, std::memory_order_release);

            // REQ-045 P4-4: resolve the model for THIS job from the latest
            // session_open model_id ("" -> pinned; unresolvable -> pinned with
            // the one-time notice; referenced-but-absent -> model_missing).
            const std::string model_path = resolve_active_model();
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
                    out = engine->Translate(
                        emebalachat::ToUtf16(job.text), tgt_name, job.src, model_path,
                        job.sampling.temperature, job.sampling.top_p,
                        job.sampling.top_k, job.sampling.rep_pen);
                }
            }
            const bool aborted = g_cancel.load(std::memory_order_acquire);
            g_current_job.store(0, std::memory_order_release);

            wp::EventMsg ev;
            ev.session = job.session;
            ev.seq = ++event_seq;
            if (aborted) {
                // Cancel contract: the orchestrator maps an aborted job to
                // status=timeout toward the client (v1 §4.4 behavior).
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
            if (!WriteFrame(pipe, wp::BuildEvent(ev))) {
                DIAG_F("ENGINEHOST/worker/023: event write failed (orchestrator gone)\n");
                return 3;
            }
            DIAG_LOG("ENGINEHOST", "worker/002: job %llu answered (kind=%s seq=%llu)",
                     static_cast<unsigned long long>(job.job),
                     ev.kind == wp::EventKind::Final ? "final" : "error",
                     static_cast<unsigned long long>(ev.seq));
        } else if (op == "session_open") {
            wp::SessionOpenMsg msg;
            if (!wp::ParseSessionOpen(json, msg)) {
                (void)WriteFrame(pipe, wp::BuildError(enginehost::kErrBadRequest));
                return 3;
            }
            if (msg.capability != "translate") {
                // Not our capability: inline refusal (fail-closed, no tear-down
                // — the orchestrator routes capability mismatches).
                (void)WriteFrame(pipe, wp::BuildError("unavailable"));
                continue;
            }
            // REQ-045 P4-4: the session opens unconditionally still, but the
            // relayed model_id now BECOMES the active model for subsequent
            // jobs (resolved per job against the registry inside the job
            // branch — see resolve_active_model). An empty model_id keeps the
            // pinned default (the pre-REQ-045 behavior, byte-identical).
            active_model_id = msg.model_id;
            if (!WriteFrame(pipe, wp::BuildOpened(msg.session))) {
                return 3;
            }
        } else if (op == "close") {
            std::uint64_t session = 0;
            if (!wp::ParseClose(json, session)) {
                (void)WriteFrame(pipe, wp::BuildError(enginehost::kErrBadRequest));
                return 3;
            }
            if (!WriteFrame(pipe, wp::BuildClosed(session))) {
                return 3;
            }
        } else if (op == "abort") {
            wp::AbortMsg msg;
            if (!wp::ParseAbort(json, msg)) {
                (void)WriteFrame(pipe, wp::BuildError(enginehost::kErrBadRequest));
                return 3;
            }
            // D-2 chain b: store the flag; the llama abort/progress callbacks
            // (already wired to THIS atomic's address) unwind the decode/load.
            // An abort for a job that is not in flight is a no-op (v1 rule:
            // only the in-flight job is aborted).
            if (g_current_job.load(std::memory_order_acquire) != 0) {
                g_cancel.store(true, std::memory_order_release);
            }
        } else if (op == "heartbeat") {
            wp::HeartbeatMsg hb;
            if (!wp::ParseHeartbeat(json, hb)) {
                (void)WriteFrame(pipe, wp::BuildError(enginehost::kErrBadRequest));
                return 3;
            }
            // Liveness marker only; nothing to answer (§1.2 one-way ping).
        } else if (op == "shutdown") {
            if (!wp::ParseShutdown(json)) {
                (void)WriteFrame(pipe, wp::BuildError(enginehost::kErrBadRequest));
                return 3;
            }
            // M-2 order (worker half): every prior event was flushed
            // synchronously by WriteFrame. Now: abort any in-flight decode
            // (so Unload is not blocked by a multi-second llama call), unload
            // the model, send shutdown_ack LAST, exit 0. The orchestrator
            // waits for this ack (or EOF) BEFORE closing the pipe.
            DIAG_LOG("ENGINEHOST", "worker/024: shutdown requested; aborting + unloading model");
            g_cancel.store(true, std::memory_order_release);
            engine->Unload();
            if (!WriteFrame(pipe, wp::BuildShutdownAck())) {
                DIAG_F("ENGINEHOST/worker/025: shutdown_ack write failed (orchestrator gone)\n");
                return 0; // the pipe is already dead; graceful-enough exit
            }
            break; // FrameLoop returns 0 -> wWinMain exits 0
        } else {
            // §V2-12-2: unknown ops converge to error + close (fail-closed).
            DIAG_F("ENGINEHOST/worker/026: unknown op rejected (len=%zu)\n", json.size());
            (void)WriteFrame(pipe, wp::BuildError(enginehost::kErrBadRequest));
            return 3;
        }
    }
    return 0;
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

    // Single instance per family (design §4.2): a duplicate spawn exits
    // quietly; the orchestrator's respawn path retries on the next dispatch.
    g_mutex = ::CreateMutexW(nullptr, TRUE, wp::kWorkerSingleInstanceMutexName);
    if (!g_mutex || ::GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_mutex) ::CloseHandle(g_mutex);
        diag::Shutdown();
        return 0;
    }

    // ---- handshake arguments (§1.2) ----
    const WorkerArgs args = ParseArgs(pCmdLine);
    const bool token_ok = args.token.size() == 32 &&
        std::all_of(args.token.begin(), args.token.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    if (args.pipe_name.empty() || args.pipe_name.size() > 200 || !token_ok) {
        DIAG_F("ENGINEHOST/worker/002: bad command line (pipe=%zu token=%zu)\n",
               args.pipe_name.size(), args.token.size());
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

    const int exit_code = FrameLoop(pipe, args.token);
    ::CloseHandle(pipe);
    DIAG_LOG("ENGINEHOST", "worker/027: exiting (code=%d)", exit_code);
    diag::Shutdown();
    ::ReleaseMutex(g_mutex);
    ::CloseHandle(g_mutex);
    return exit_code;
}
