// Plan-B (REQ-B004) B-T2: host-side per-spawn VRAM gate — PURE decision core.
// See engine_host_vram_gate.hpp for the contract and dependency rationale.

#include "engine_host_vram_gate.hpp"

namespace emebalachat {
namespace enginehost {
namespace vramgate {

const char* SpawnVramReasonString(SpawnVramReason reason) {
    switch (reason) {
        case SpawnVramReason::Ok:
            return "ok";
        case SpawnVramReason::BelowThresholdGpu:
            return "below_threshold_gpu";
        case SpawnVramReason::QueryFailed:
            return "query_failed";
        case SpawnVramReason::OverrideForcedBelowThreshold:
            return "override_forced_below_threshold";
    }
    return "ok"; // exhaustive switch; unreachable
}

SpawnVramDecision DecideSpawnVram(int override_value, bool query_ok,
                                  unsigned long long free_bytes,
                                  unsigned long long reserved_bytes,
                                  unsigned long long model_vram_bytes) {
    SpawnVramDecision d;
    d.allow_cpu = true; // v2 §A2: CPU leg always permitted (host RAM, not VRAM).
    d.estimated_free_bytes = free_bytes;
    d.reserved_bytes = reserved_bytes;

    // Aggregate accounting: what actually remains after the already-running
    // translate workers' reservations, minus what this spawn would take.
    const unsigned long long available =
        (free_bytes >= reserved_bytes) ? (free_bytes - reserved_bytes) : 0ull;
    const unsigned long long headroom =
        (available >= model_vram_bytes) ? (available - model_vram_bytes) : 0ull;
    const bool gpu_ok = headroom >= kSpawnVramThresholdBytes;

    if (!query_ok) {
        // Fail-closed: a failed DXGI probe means the adapter budget is unknown.
        // Never allow GPU on an unknown budget, even under a forced override.
        d.allow_gpu = false;
        d.reason = SpawnVramReason::QueryFailed;
        return d;
    }

    if (override_value == 1) {
        // Force GPU past a KNOWN shortfall; honest about the risk (the worker's
        // own RT-C gate sees the same override and forces GPU too).
        d.allow_gpu = true;
        d.reason = gpu_ok ? SpawnVramReason::Ok
                          : SpawnVramReason::OverrideForcedBelowThreshold;
        return d;
    }

    if (override_value == 0) {
        // Force CPU: never GPU, regardless of gpu_ok.
        d.allow_gpu = false;
        d.reason = SpawnVramReason::Ok;
        return d;
    }

    // Auto (no override): honest gate on the aggregate accounting.
    d.allow_gpu = gpu_ok;
    d.reason = gpu_ok ? SpawnVramReason::Ok : SpawnVramReason::BelowThresholdGpu;
    return d;
}

} // namespace vramgate
} // namespace enginehost
} // namespace emebalachat
