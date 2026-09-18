#ifndef EMEBALACHAT_HOST_V2_HEALTH_HPP
#define EMEBALACHAT_HOST_V2_HEALTH_HPP

// ---------------------------------------------------------------------------
// host_v2_health — REQ-043 (M6 T4, design §1.7, REQ-008): local health metrics
// for the v2 orchestrator (EmebalaEngine target).
//
// Scope (task T4 item 5): session/job success, failure and fallback counters
// aggregated locally in atomics; the v2 welcome exposes
// health{session_success_ratio, fallback_ratio}. The v1 welcome gains NO
// fields (frozen §4.4 bytes stay untouched).
//
// Telemetry-free by construction (design §10 row "telemetry 금지"): the
// counters live in this process only, no network code exists in this module,
// and the only egress is the welcome frame the host writes to a local pipe
// client. No user text is ever recorded — outcomes are counted, not stored.
//
// Definitions (design §9 R-5 option (b) adopted):
//   * fallback  = a job terminated by busy | unavailable | timeout (the
//                 situations where a client would fall back), as observed by
//                 the HOST. Client-side cloud fallback is invisible here and
//                 deliberately not estimated.
//   * failure   = a job terminated by engine_failed | model_missing.
//   * success   = a job answered status=ok.
//   * session_success_ratio = successful sessions / all session attempts;
//                 0 attempts -> 1.0 (vacuous truth: nothing has failed yet —
//                 a fresh boot must not look unhealthy).
//   * fallback_ratio        = fallback jobs / all jobs;
//                 0 jobs -> 0.0.
// All counters are monotonic for the process lifetime (no reset — the ratios
// converge toward long-run values, which is the honest signal).
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstdint>

namespace emebalachat {
namespace host_v2 {

// Outcome bucket for one finished job (translate request either profile).
enum class HealthOutcome : unsigned char {
    Success,   // status=ok
    Failure,   // engine_failed | model_missing
    Fallback,  // busy | unavailable | timeout
};

class HealthCounters {
public:
    HealthCounters() = default;

    // One job reached a terminal answer (either profile, both pipe sets).
    void RecordJob(HealthOutcome o);

    // One session reached a terminal lifecycle state. A refused/aborted/
    // error-open session counts as failure (a fallback is not observable for
    // sessions in M6 — sessions are only opened for the deployed family).
    void RecordSession(bool success);

    // Successful sessions / all session attempts; 0 attempts -> 1.0.
    double SessionSuccessRatio() const;

    // Fallback jobs / all jobs; 0 jobs -> 0.0.
    double FallbackRatio() const;

    // Shape-only counters for diagnostics/tests (never contain user text).
    std::int64_t JobsTotal() const;
    std::int64_t JobsFallback() const;
    std::int64_t SessionsTotal() const;
    std::int64_t SessionsOk() const;

private:
    std::atomic<std::int64_t> job_ok_{0};
    std::atomic<std::int64_t> job_fail_{0};
    std::atomic<std::int64_t> job_fallback_{0};
    std::atomic<std::int64_t> session_ok_{0};
    std::atomic<std::int64_t> session_fail_{0};
};

} // namespace host_v2
} // namespace emebalachat

#endif // EMEBALACHAT_HOST_V2_HEALTH_HPP
