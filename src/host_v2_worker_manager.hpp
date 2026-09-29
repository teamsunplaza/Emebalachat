#pragma once

// ---------------------------------------------------------------------------
// host_v2_worker_manager — REQ-043 (M6 T3, design §1.1, plan §V2-3): the
// orchestrator-side worker lifecycle manager (EmebalaEngine target).
//
// Responsibilities (design §1.1 "워커 매니저"):
//   * EnsureSpawned  — spawn the family worker exe (hidden, same user, boot-
//                      scoped token on the command line — NO token file),
//                      create the private pipe (server end, user-only SD),
//                      and complete the announce handshake (family/abi/
//                      protocol-range validation; a mismatch marks the family
//                      unavailable WITHOUT respawning — design §3.4 note).
//   * ReaperLoop     — 250 ms done_event poll; a crashed worker ends the
//                      family's in-flight jobs (engine_failed/unavailable),
//                      resets the state machine and schedules an exponential
//                      backoff respawn (1s -> 2s -> 4s ... cap 30s; R-4
//                      adopted: hardcoded, design §9).
//   * GracefulStop   — M-2 ORDER (tech gate): send shutdown frame -> wait for
//                      shutdown_ack OR pipe EOF (the worker's last frame) ->
//                      ONLY THEN CloseHandle. DisconnectNamedPipe purges
//                      unconsumed frames (v1 measured, host_main.cpp:363-372),
//                      so closing before the ack can lose the final event.
//   * Orphan guard   — ShutdownAll kills any surviving child so the
//                      orchestrator can never orphan a worker (design §1.1
//                      rule: "오케스트레이터 종료 시 워커도 종료 보장").
//
// T3 scope note: host_main.cpp is NOT touched (T4 owns the InferenceLoop
// replacement). This module compiles + unit-tests standalone: the pure state
// machine is split from the Win32 plumbing so run_tests can drive spawn
// decisions, backoff math and crash transitions WITHOUT any process.
//
// Security (design §10): pipe = user-only SD (same SDDL builder pattern as
// host_main.cpp BuildUserOnlySd), token = boot-scoped random hex32 passed on
// the command line only, shape-only ENGINEHOST/wmgr/NNN diagnostics, no user
// text, no config file, loopback TCP forbidden (named pipe only).
// ---------------------------------------------------------------------------

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "worker_protocol.hpp" // second frozen contract (pure helpers)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace emebalachat {
namespace host_v2 {

namespace wp = emebalachat::workerproto;

// ---- worker state machine (design §1.1 WorkerHandle/WorkerState) -----------
enum class WorkerState : unsigned char {
    Spawning,   // spawn issued, handshake in flight
    Ready,      // announce validated, accepts dispatch
    Busy,       // a job is in flight (T4 dispatcher sets/clears this)
    Crashed,    // child died abnormally; backoff respawn pending
    Stopped,    // graceful stop completed (or never spawned)
    Unavailable, // handshake rejected (abi/family mismatch) — NO respawn
};

inline std::string_view WorkerStateToString(WorkerState s) {
    switch (s) {
        case WorkerState::Spawning:    return "spawning";
        case WorkerState::Ready:       return "ready";
        case WorkerState::Busy:        return "busy";
        case WorkerState::Crashed:     return "crashed";
        case WorkerState::Stopped:     return "stopped";
        case WorkerState::Unavailable: return "unavailable";
    }
    return {};
}

// Pure backoff math (unit-pinned): 1s -> 2s -> 4s -> ... capped at 30s (R-4).
inline int BackoffDelayMs(int consecutive_failures) {
    if (consecutive_failures <= 0) return 0;
    int delay = wp::kWorkerBackoffInitialMs;
    for (int i = 1; i < consecutive_failures && delay < wp::kWorkerBackoffMaxMs; ++i) {
        delay *= 2;
    }
    return delay > wp::kWorkerBackoffMaxMs ? wp::kWorkerBackoffMaxMs : delay;
}

// P2-1 stabilization (session 260925_0001, live-test defect 1): the asr
// session_open cold-start wait classifier. EnsureSpawned can answer nullptr
// FAST while the family is still coming up — the Crashed backoff gate defers
// the respawn, and a slow cold spawn can outrun the fixed handshake window —
// and the first open used to convert that fast nullptr straight into
// "unavailable". The open instead RETRIES EnsureSpawned inside its deadline
// (kAsrOpenTimeoutMs) until the worker is spawned+connected. Unavailable is
// permanent (missing exe / announce mismatch: a retry cannot fix a wrong
// deployment), so it alone fails fast; past the deadline anything stops.
// P2-1 stabilization (session 260925_0001, live defect: persistent
// `unavailable` under sustained load): a worker-pipe write failure is either
// BROKEN (peer dead, immediate error) or STALLED (peer alive but not draining
// for the full bounded write window — e.g. blocked inside an inference call).
// A STALLED worker is useless to a real-time stream and must be KILLED: its
// per-family single-instance mutex rejects every duplicate spawn while the
// stuck process holds it, so without the kill the family wedges in
// backoff-respawn churn (the live `unavailable` loop). Pure decision,
// unit-pinned.
enum class FrameWriteResult : unsigned char { Ok, Broken, Stalled };
inline bool WorkerWriteFailureNeedsKill(FrameWriteResult r) {
    return r == FrameWriteResult::Stalled;
}

inline bool AsrSpawnRetryWarranted(WorkerState s, int64_t now_ms, int64_t deadline_ms) {
    if (now_ms >= deadline_ms) return false;
    return s == WorkerState::Stopped || s == WorkerState::Spawning ||
           s == WorkerState::Crashed || s == WorkerState::Ready ||
           s == WorkerState::Busy;
}

// ---- injectable spawn seam (unit tests substitute a fake launcher) ---------
// The real launcher spawns "Emebala.Engine.<family>.exe" hidden with
// --pipe/--token. Tests inject a process handle + fake exit behavior so the
// state machine runs process-free.
struct SpawnRequest {
    std::wstring exe_path;      // absolute path to the worker exe
    std::wstring pipe_name;     // \\.\pipe\emebala-engine-worker-<pid>-<rand>
    std::string token;          // 32 hex chars
    // Plan-B (REQ-B004) B-T3, technical gate A-4: per-spawn environment block
    // override. Empty -> the child inherits the parent's environment verbatim
    // (the pre-B-T3 behavior, lpEnvironment=nullptr). Non-empty -> a
    // double-null-terminated Unicode environment block built by
    // BuildTranslateSpawnEnvironment (parent env + the boot-scoped overrides),
    // passed to CreateProcessW as lpEnvironment.
    std::vector<wchar_t> environment_block;
    // B3 fix (session 260928_0001): the worker family this spawn serves. The
    // launcher forwards it to the child as `--mutex <family>` so the worker
    // derives a PER-FAMILY single-instance mutex (wp::MutexNameForFamily)
    // instead of the legacy family-agnostic constant — the B-T4 pool spawns
    // one worker per family and the second family used to collide on the
    // shared mutex name. Empty (old call sites / arg-absent worker) -> the
    // worker falls back to the exact legacy constant (backward compat).
    std::wstring family;
};

struct SpawnResult {
    bool ok = false;
    HANDLE process = nullptr;   // caller owns; nullptr when !ok
    HANDLE done_event = nullptr;// process handle itself (signaled on exit)
    DWORD last_error = 0;
};

using SpawnFn = SpawnResult (*)(const SpawnRequest&, void* user);
using CloseProcessFn = void (*)(HANDLE process, void* user);

// ---- Plan-B (REQ-B004) B-T3: injectable VRAM-gate dependencies -------------
// host_v2 stays dependency-free (technical gate A-5): the registry vram lookup
// lives in Emebalachat_core (engine_host_registry), the DXGI free-VRAM probe in
// Emebalachat_engine_core (engine_core_helpers) — host_v2 links NEITHER. Each
// is injected once at boot from host_main.cpp (B-T4 wiring) via a setter; unit
// tests inject fakes so the whole gate runs with no DXGI and no registry.
//
//   VramBytesResolver: model_id -> its reserved VRAM in BYTES (the registry
//     vram_mb converted at the call site). A null/empty return -> the model is
//     absent from the boot registry snapshot -> the conservative default
//     (design §4.3 step 2).
//   FreeVramProbe: (out_free_bytes) -> query_ok. False -> the gate's fail-closed
//     rule (query_ok=false -> never allow GPU, vramgate::DecideSpawnVram).
using VramBytesResolver = std::function<unsigned long long(const std::string& model_id)>;
using FreeVramProbe = std::function<bool(unsigned long long& out_free_bytes)>;

// ---- per-family handle (design §1.1 WorkerHandle) --------------------------
struct WorkerHandle {
    std::wstring family;
    WorkerState state = WorkerState::Stopped;
    HANDLE process = nullptr;          // SYNCHRONIZE | PROCESS_TERMINATE
    HANDLE pipe = nullptr;             // orchestrator-side server end
    std::wstring pipe_name;
    std::string token;
    std::wstring exe_path;
    int spawn_failures = 0;            // consecutive crash counter (backoff)
    int64_t next_spawn_allowed_ms = 0; // backoff gate
    wp::WorkerManifest manifest;       // the VALIDATED announce
    int64_t last_heartbeat_ms = 0;
    // Diagnostics counters (shape-only; surfaced in the T4 health block).
    int64_t jobs_failed_on_crash = 0;
};

// ---- per-model translate pool entry (Plan-B §3.2 / REQ-B003) ----------------
// One entry per distinct resolved model_id (the output domain of
// RelayPinForClient). The WorkerManager owns the pool; the dispatchers only
// ever call TranslatePoolEnsure() and use the returned family with the
// existing WorkerManager API. Relay state lives HERE — this is the single
// source of truth that eliminates the duplicated per-dispatcher-loop
// relayed_model_id / relayed_worker_process locals (§3.5).
//
// In B-T1 the pool is present but UNUSED: the dispatchers still hardcode
// kWorkerFamilyTranslate; the boot "" entry references that same family. The
// switchover lands in B-T4 (two-stage merge, §12.1 mitigation 4).
struct TranslatePoolEntry {
    std::string model_id;           // "" = pinned default. Pool map key.
    std::wstring family;            // derived: kWorkerFamilyTranslate for "",
                                    //   kWorkerFamilyTranslate + L"-" + widened
                                    //   model_id otherwise (§3.3).
    std::string relayed_model_id;   // last model_id the worker accepted (REQ-055)
    HANDLE relayed_worker_process = nullptr;  // REQ-058 process-handle snapshot
    bool registered = false;        // RegisterFamily() has been called
    // ---- B-T3 (REQ-B004 §4.3) ----
    // This entry's contribution to translate_pool_reserved_vram_bytes_, recorded
    // when its spawn succeeds (gate allow_gpu == spawn with the GPU offload
    // environment; the model_vram_bytes the gate charged). 0 while not running.
    // The reaper subtracts EXACTLY this on exit, so the tracker can never
    // drift from the pool's own accounting (design §12.4).
    unsigned long long reserved_vram_bytes = 0;
    // Set when the gate decided !allow_gpu (CPU leg): the spawn passes
    // EMEBALA_MT_GPU=0 in the child environment (technical gate A-4). Cleared
    // on a GPU-leg decision. Only consulted at SPAWN time (the gate decides AT
    // SPAWN, design §4.2).
    //
    // P5a.5 C2 (195700_code-reviewer-planb-p5a5.md F-2): this flag is the
    // PERSISTENT per-entry record of the family's spawn-env leg. It is applied
    // on EVERY LaunchWorkerProcess for the family — initial spawn, backoff
    // crash-respawn, and graceful-restart respawn alike (the handle state is
    // NOT consulted; the old `state == Stopped` gate silently skipped the
    // Crashed->respawn path). A respawned CPU-leg worker must never silently
    // take the GPU leg (REQ-B004 honesty on the designed crash-respawn path).
    bool spawn_gpu_env_override = false; // true -> child env carries EMEBALA_MT_GPU=0
};

// ---- job-wait frame classifier (REQ-049) -----------------------------------
// REQ-049: one shared definition of how a job-wait loop classifies an inbound
// worker frame. The worker legitimately interleaves heartbeat frames (§1.2
// cadence) with events; a heartbeat must never fail a job (the root cause of
// the intermittent 'first request after idle -> engine_failed' defect).
enum class JobWaitFrame : unsigned char {
    Final,     // event kind=final -> terminal success
    Error,     // event kind=error -> terminal failure (code mapped by caller)
    Progress,  // event kind=partial|token|eos -> consume, keep waiting
    Heartbeat, // op=heartbeat -> consume, keep waiting
    Malformed, // anything else -> fail-closed protocol violation
};

// Fills `ev` only for the event classes; returns the classification.
inline JobWaitFrame ClassifyJobWaitFrame(std::string_view json, workerproto::EventMsg& ev) {
    // Cheap op check first: the idle cadence makes a heartbeat the most common
    // stray frame on a pipe the dispatcher left idle.
    enginehost::JsonPairs p;
    if (enginehost::JsonParseObject(json, p)) {
        const auto* op = enginehost::detail::FindField(p, "op");
        if (op && op->is_string && op->text == "heartbeat") return JobWaitFrame::Heartbeat;
    }
    if (!wp::ParseEvent(json, ev)) return JobWaitFrame::Malformed;
    switch (ev.kind) {
        case wp::EventKind::Final:            return JobWaitFrame::Final;
        case wp::EventKind::Error:            return JobWaitFrame::Error;
        case wp::EventKind::Partial:
        case wp::EventKind::Token:
        case wp::EventKind::Eos:              return JobWaitFrame::Progress;
    }
    return JobWaitFrame::Malformed; // unreachable (kind is a closed enum)
}

// ---- the manager (one instance per orchestrator; families registered) ------
class WorkerManager {
public:
    // injectors: launch/close seams for unit tests (default = real Win32).
    WorkerManager(SpawnFn spawn = nullptr, CloseProcessFn close = nullptr,
                  void* user = nullptr);
    ~WorkerManager();

    WorkerManager(const WorkerManager&) = delete;
    WorkerManager& operator=(const WorkerManager&) = delete;

    // Registers a family: exe path resolution happens NOW (exe may be absent
    // -> EnsureSpawned answers unavailable until deployed; design §V2-8).
    // family "ggml-translate" is the M6 registration.
    void RegisterFamily(const std::wstring& family, const std::wstring& exe_path);

    // Ensures the family has a Ready worker: spawns when Stopped/Crashed
    // (respecting the backoff gate), runs the announce handshake, validates
    // abi/protocol ranges. Returns nullptr when the family cannot serve
    // (missing exe, spawn failure, handshake mismatch -> Unavailable).
    // pure-logic hook: ValidateHandshake is exposed separately for tests.
    WorkerHandle* EnsureSpawned(const std::wstring& family);

    // Marks a job as started/finished on the family (Busy <-> Ready).
    void SetBusy(const std::wstring& family, bool busy);

    // Dispatch entry for the T4 scheduler: writes one frame to the worker
    // pipe (thread-safe). False = the pipe is dead (caller treats the job as
    // engine_failed; the ReaperLoop will judge the respawn).
    bool SendToWorker(const std::wstring& family, std::string_view json);

    // M6 T4 (orchestrator dispatch): read ONE frame from the family worker's
    // pipe with a bounded wait. Ok -> json; Timeout -> alive, nothing yet;
    // IoError -> the pipe is dead (caller answers engine_failed; the reaper
    // judges the respawn). Thread-safe: serialized against SendToWorker and
    // the graceful stop by the manager mutex.
    enum class WorkerRead : unsigned char { Ok, Timeout, IoError };
    WorkerRead ReadFromWorker(const std::wstring& family, std::string& json,
                              int timeout_ms);

    // REQ-049: non-blocking drain of frames queued on the family pipe while
    // the dispatcher was idle (worker heartbeats, or a stray final left by a
    // timed-out previous job). Called right before publishing a new job so a
    // stale frame can never be misattributed to the new job (events carry no
    // job id). Returns the number of drained frames; bounded by max_frames.
    int DrainWorkerPipe(const std::wstring& family, int max_frames = 128);

    // 260927_0003 (MT audit Q1 fix A): bounded blocking SETTLE after a give-up.
    // The REQ-049 drain is a non-blocking poll, so by its own contract it only
    // covers strays that arrived while the dispatcher was IDLE. A worker that
    // is still decoding a given-up job publishes that job's terminal event
    // only AFTER the abort unwinds the decode (between tokens, seconds late
    // under CPU contention) — a back-to-back next job then classifies it by
    // kind only (worker events carry no job id) and re-stamps it with its own
    // id: a persistent off-by-one pairing skew for the worker's lifetime.
    // The settle closes that hole: BEFORE the timeout answer leaves and BEFORE
    // the next job frame is published, read frames until the given-up job's
    // terminal event arrives and DISCARD it. The worker answers exactly ONE
    // terminal event per job (T3 contract — after an abort it is the
    // error/timeout unwind event or a final that outran the abort), so the
    // first terminal frame read after the abort is the given-up job's by
    // construction — and that invariant is FAMILY-WIDE, not per-dispatcher:
    // 260927_0003 F-01 (review round 2). The owner holds the family Busy from
    // the TrySetBusy acquire (prolog, BEFORE any family-pipe I/O: drain,
    // relay, publish) through the whole job and this settle until the
    // post-answer release, and BOTH dispatchers publish only via TrySetBusy,
    // which fails fast while the family is Busy. The sibling therefore
    // answers HostStatus::Busy instead of publishing mid-settle — its frames
    // can never race the settle's discard, and the settle can never swallow
    // the sibling job's only terminal. Heartbeat/progress frames interleaved
    // are consumed; a malformed frame is consumed too — the job is already
    // given up, so there is nothing left to fail-closed.
    // Bounded by timeout_ms (the audit's <= 10 s budget) AND max_frames;
    // `cancel` (host shutdown) ends the settle early. Lock discipline mirrors
    // ReadFromWorker: the handle is snapshotted under the mutex, frames are
    // read WITHOUT holding it (a seconds-long blocking read must not stall
    // SendToWorker or the reaper), and a pipe IO error ends the settle.
    enum class SettleOutcome : unsigned char {
        TerminalDiscarded, // the given-up job's terminal frame was read + dropped
        DeadlineExpired,   // the budget (time or max_frames) ran out first
        PipeIoError,       // the family pipe died mid-settle (the reaper judges)
        Cancelled,         // `cancel` fired (host shutdown)
    };
    SettleOutcome SettleWorkerPipe(const std::wstring& family, int timeout_ms,
                                   const std::atomic<bool>* cancel = nullptr,
                                   int max_frames = 512);

    // 260927_0003 F-01 (review round 2): single-flight family acquire. Under
    // ONE mutex acquisition: succeeds only when the family is Ready and flips
    // it to Busy atomically; ANY other state fails fast (Busy = a sibling
    // dispatcher owns the pipe across its whole job + give-up settle window;
    // Crashed/Spawning/Unavailable/Stopped = cannot serve right now). The
    // loser answers HostStatus::Busy (an existing status on both profiles;
    // the client retries) instead of interleaving its publish/drain/relay
    // with the owner's frames — the cross-dispatcher hole the settle's
    // invariant needs closed (the v1 and v2 dispatcher threads run
    // concurrently in one host over the same translate-worker family).
    // Replaces the prolog SetBusy(true), which was a no-op when the family
    // was already Busy and so never enforced single-flight.
    // DEADLOCK-FREE BY CONSTRUCTION: mu is held only for the compare+store,
    // never across a wait or an I/O; the loser never blocks on the owner
    // thread (no condition variable, no cross-thread wait, no lock ordering
    // between the two dispatchers — the only lock is mu, and no thread ever
    // waits on another thread while holding it). The winner's later pipe
    // I/O keeps the established ReadFromWorker snapshot discipline (mu only
    // for handle snapshots), so the sibling's fail-fast path can never
    // convoy behind a blocking read either.
    bool TrySetBusy(const std::wstring& family);

    // ---- per-model translate pool (Plan-B §3, REQ-B003; B-T1 machinery) ------
    // A-3 (technical gate): the translate worker exe path is a main()-scope
    // local today (host_main.cpp). It is handed to the manager ONCE at boot
    // so TranslatePoolEnsure can reuse the SAME exe path for runtime
    // registrations of new pool families (the worker is model-agnostic at
    // spawn; the model is resolved INSIDE the worker via session_open).
    // Returns false when no translate exe path has been recorded.
    bool SetTranslateExePath(const std::wstring& exe_path);
    std::wstring TranslateExePath() const;

    // Boot registration of the pinned-default ("") pool entry. Called ONCE at
    // the boot site, immediately after RegisterFamily(kWorkerFamilyTranslate,
    // ...): the "" entry references that SAME family — no new RegisterFamily
    // is issued for it (B-T1 scope; dispatchers keep the hardcoded family).
    // idempotent: a second call is a no-op when the entry already exists.
    bool RegisterTranslatePoolBootEntry();

    // Plan-B §3.4 on-demand registration + B-T3 (REQ-B004) VRAM gate. Returns
    // the pool entry for `model_id`, creating + registering it when absent.
    // For "" this returns the boot entry (no duplicate registration). When the
    // family derivation yields an empty string (invalid id chars) or the
    // translate exe path is unset, returns nullptr (the dispatcher answers
    // model_missing).
    //
    // B-T3 gate (runs ONLY for a NEW entry — the gate decides AT SPAWN, design
    // §4.2; an already-registered entry takes the fast path with no re-run):
    //   free   <- the injected FreeVramProbe (default nullptr -> query_ok=false)
    //   model  <- the injected VramBytesResolver (default -> 2048 MiB, §4.3 step 2)
    //   override <- the injected override value (default = absent/auto)
    //   reserved <- translate_pool_reserved_vram_bytes_ (the running tracker)
    //   decision <- vramgate::DecideSpawnVram(...)
    // Branch per §4.4 / v2 §A2 (the CPU leg is always permitted):
    //   allow_gpu  -> register + spawn with the GPU offload environment as-is
    //                 (child inherits; unchanged behavior).
    //   !allow_gpu -> register + spawn with EMEBALA_MT_GPU=0 in the child env
    //                 (technical gate A-4). The CPU leg is never refused, so a
    //                 nullptr return here means only "invalid id / no exe".
    // Shape-only decision log (invariant #5): family / reason enum / numbers.
    //
    // LOCK DISCIPLINE (technical gate A-2): pool mu is held ONLY for the map
    // lookup/insert; RegisterFamily / EnsureSpawned run OUTSIDE pool mu — never
    // hold translate_pool_mu_ across a spawn (30 s convoy risk).
    TranslatePoolEntry* TranslatePoolEnsure(const std::string& model_id);

    // ---- B-T3 injectable VRAM-gate seams (technical gate A-5) ----------------
    // Boot wiring (host_main.cpp, B-T4) calls each setter ONCE before the
    // dispatchers start; unit tests inject fakes. host_v2 stays free of
    // engine_core / registry includes.
    //   resolver: model_id -> reserved bytes. Unset/empty -> 2048 MiB default.
    //   probe:    (out_free) -> query_ok. Unset -> query_ok=false (fail-closed).
    //   override: the EMEBALA_MT_GPU value (1 force GPU / 0 force CPU / other
    //             = auto). Boot reads the env ONCE behind this setter.
    void SetVramBytesResolver(VramBytesResolver resolver);
    void SetFreeVramProbe(FreeVramProbe probe);
    void SetVramOverrideValue(int override_value);
    // The last gate decision inputs+outputs, for the unit pins (never null).
    struct VramGateSnapshot {
        int override_value;
        bool query_ok;
        unsigned long long free_bytes;
        unsigned long long reserved_bytes;
        unsigned long long model_vram_bytes;
        bool allow_gpu;
        bool allow_cpu;
        const char* reason; // vramgate::SpawnVramReasonString, shape-only
    };
    // The gate ran only when the snapshot's model_vram_bytes is non-zero
    // (0 marks "no decision yet"); B-T3 tests assert on the last decision.
    VramGateSnapshot TranslatePoolLastGateDecision() const;

    // Plan-B §3.5 relay consolidation. Byte-identical comparison semantics to
    // the removed per-loop locals: relayed_model_id != model_id ||
    // relayed_worker_process != current_process. Accessed under the per-family
    // Busy lock (TrySetBusy) — the F-01 single-flight contract provides the
    // mutation discipline; pool mu guards only the O(1) map lookup.
    bool TranslatePoolNeedsRelay(const std::string& model_id, HANDLE current_process);

    // Mark relay accepted (after EnsureWorkerModelRelayed succeeds).
    void TranslatePoolMarkRelayed(const std::string& model_id, HANDLE process);

    // Pool size — for B-T1 verification pins only.
    size_t TranslatePoolSize() const;

    // B-T3: the running-bytes tracker (design §4.3) — the sum of the RUNNING
    // translate families' model bytes. Incremented when a translate-family
    // spawn reaches Ready (+entry->reserved_vram_bytes, which the gate set);
    // decremented by the reaper when a translate worker exits. ASR families
    // are NEVER counted (§4.5). Test-only accessor.
    unsigned long long TranslatePoolReservedVramBytes() const;

    // M-2 graceful stop of ONE family: shutdown -> wait shutdown_ack/EOF ->
    // close. Returns after the pipe is closed. Safe on a dead worker.
    bool GracefulStop(WorkerHandle& w, int timeout_ms = 10000);

    // Orchestrator exit path: graceful stop for every family, then kill any
    // survivor (orphan guard), close all handles, join the reaper.
    void ShutdownAll();

    // Reaper (design §1.1 rule 3): polls done_events, finalizes crashes,
    // schedules backoff respawns. Runs on its own thread after StartReaper();
    // T4 calls StartReaper once after registration.
    void StartReaper();
    void StopReaper();

    // One reaper pass (public for unit tests): poll every family's
    // done_event, transition Crashed, terminate in-flight jobs' state.
    // Returns the number of crashes finalized.
    int ReaperPass(int64_t now_ms);

    // Pure handshake validation (unit-pinned): announce vs expectations.
    static wp::AnnounceStatus ValidateHandshake(const wp::WorkerManifest& a) {
        return wp::AnnounceCheck::Validate(a, wp::kWorkerAbiVersion,
                                           wp::kWorkerProtocolMin,
                                           wp::kWorkerProtocolMax);
    }

    // State inspection (thread-safe snapshot; the T4 scheduler + health read).
    WorkerState State(const std::wstring& family);
    WorkerHandle* Find(const std::wstring& family); // mutex-internal use only

private:
    // NOTE (header): Impl's member layout is completed in the .cpp. The pool
    // containers + translate exe path + B-T3 gate seams live in Impl there:
    //   std::unordered_map<std::string, TranslatePoolEntry> translate_pool_;
    //   std::mutex translate_pool_mu_;
    //   std::wstring translate_exe_path_;
    //   unsigned long long translate_pool_reserved_vram_bytes_; (§4.3, + its mu)
    //   VramBytesResolver / FreeVramProbe / override / last-gate snapshot
    //
    // P5a.5 C1 (195700 F-1) MAP-MUTEX CONTRACT: EVERY access to
    // `translate_pool_` (find / emplace / iterate / entry-field read+write)
    // runs under translate_pool_mu_ — reads AND writes alike. Callers copy the
    // needed entry values out (or make the brief field update) and release —
    // and NEVER hold translate_pool_mu_ across a spawn / RegisterFamily
    // (technical gate A-2: pool mu is never held across those long ops).
    // Do NOT read this as "copy-and-release, pool mu stays a leaf" (P5a.5
    // re-review NEW-3/NEW-4 wording fix). The ACTUAL safe nesting is
    // impl_->mu -> translate_pool_mu_ -> translate_pool_vram_mu_ (one global
    // order, no reverse path anywhere; pool mu is therefore NOT leaf-level —
    // the tracker bump nests vram mu inside pool mu at the Ready-charge and
    // reaper-subtract sites). Brief OS reads under pool mu are accepted
    // fast-path-map-consistency choices: the GPU-env block snapshot
    // (GetEnvironmentStringsW inside BuildTranslateSpawnEnvironment) at the
    // spawn site and the one registry model-bytes resolve (vram_resolver_) at
    // the Ready-charge site — the same calls already ran under impl_->mu
    // pre-fix; neither takes impl_->mu or pool mu, so no deadlock path exists.
    // Holding only impl_->mu (or no lock) while touching the map is a data
    // race (UB) — the exact class C1 closed (Ready-charge / respawn-env /
    // reaper were the escaped sites).
    struct Impl;
    Impl* impl_;
};

// ---- default Win32 spawn/close seams ---------------------------------------
// SpawnResult LaunchWorkerProcess(const SpawnRequest& req, void* /*user*/);
// void CloseWorkerProcess(HANDLE process, void* /*user*/);
// (Defined in the .cpp; the manager defaults to them when ctor seams are null.)

// ---- B-T3 (REQ-B004, technical gate A-4): per-spawn environment helper -------
// Pure + testable (no CreateProcess): builds a double-null-terminated Unicode
// environment block = a copy of the PARENT environment (GetEnvironmentStringsW)
// with `assignments` applied/overwritten. The GPU-permitted leg passes an empty
// `assignments` and the CALLER forwards an empty result as lpEnvironment=nullptr
// (inherit verbatim — unchanged pre-B-T3 behavior); the CPU leg passes
// {L"EMEBALA_MT_GPU", L"0"}. Returns false only when the parent block cannot be
// read (CreateProcessW then runs with lpEnvironment=nullptr — the inherit path,
// never a failed spawn because of the env block).
bool BuildTranslateSpawnEnvironment(const std::vector<std::pair<std::wstring, std::wstring>>& assignments,
                                    std::vector<wchar_t>& out_block);

} // namespace host_v2
} // namespace emebalachat
