// host_v2_health implementation — REQ-043 (M6 T4, design §1.7): local-only
// counters. No network, no file IO, no user text — see the header contract.

#include "host_v2_health.hpp"

namespace emebalachat {
namespace host_v2 {

void HealthCounters::RecordJob(HealthOutcome o) {
    switch (o) {
        case HealthOutcome::Success:   job_ok_.fetch_add(1, std::memory_order_relaxed); break;
        case HealthOutcome::Failure:   job_fail_.fetch_add(1, std::memory_order_relaxed); break;
        case HealthOutcome::Fallback:  job_fallback_.fetch_add(1, std::memory_order_relaxed); break;
    }
}

void HealthCounters::RecordSession(bool success) {
    (success ? session_ok_ : session_fail_).fetch_add(1, std::memory_order_relaxed);
}

double HealthCounters::SessionSuccessRatio() const {
    const std::int64_t ok = session_ok_.load(std::memory_order_relaxed);
    const std::int64_t total = ok + session_fail_.load(std::memory_order_relaxed);
    return total == 0 ? 1.0 : static_cast<double>(ok) / static_cast<double>(total);
}

double HealthCounters::FallbackRatio() const {
    const std::int64_t fb = job_fallback_.load(std::memory_order_relaxed);
    const std::int64_t total = fb + job_ok_.load(std::memory_order_relaxed) +
                               job_fail_.load(std::memory_order_relaxed);
    return total == 0 ? 0.0 : static_cast<double>(fb) / static_cast<double>(total);
}

std::int64_t HealthCounters::JobsTotal() const {
    return job_ok_.load(std::memory_order_relaxed) +
           job_fail_.load(std::memory_order_relaxed) +
           job_fallback_.load(std::memory_order_relaxed);
}

std::int64_t HealthCounters::JobsFallback() const {
    return job_fallback_.load(std::memory_order_relaxed);
}

std::int64_t HealthCounters::SessionsTotal() const {
    return session_ok_.load(std::memory_order_relaxed) +
           session_fail_.load(std::memory_order_relaxed);
}

std::int64_t HealthCounters::SessionsOk() const {
    return session_ok_.load(std::memory_order_relaxed);
}

} // namespace host_v2
} // namespace emebalachat
