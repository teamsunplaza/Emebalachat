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
#include <mutex>
#include <string>
#include <thread>
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

// ---- injectable spawn seam (unit tests substitute a fake launcher) ---------
// The real launcher spawns "Emebala.Engine.<family>.exe" hidden with
// --pipe/--token. Tests inject a process handle + fake exit behavior so the
// state machine runs process-free.
struct SpawnRequest {
    std::wstring exe_path;      // absolute path to the worker exe
    std::wstring pipe_name;     // \\.\pipe\emebala-engine-worker-<pid>-<rand>
    std::string token;          // 32 hex chars
};

struct SpawnResult {
    bool ok = false;
    HANDLE process = nullptr;   // caller owns; nullptr when !ok
    HANDLE done_event = nullptr;// process handle itself (signaled on exit)
    DWORD last_error = 0;
};

using SpawnFn = SpawnResult (*)(const SpawnRequest&, void* user);
using CloseProcessFn = void (*)(HANDLE process, void* user);

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
    struct Impl;
    Impl* impl_;
};

// ---- default Win32 spawn/close seams ---------------------------------------
// SpawnResult LaunchWorkerProcess(const SpawnRequest& req, void* /*user*/);
// void CloseWorkerProcess(HANDLE process, void* /*user*/);
// (Defined in the .cpp; the manager defaults to them when ctor seams are null.)

} // namespace host_v2
} // namespace emebalachat
