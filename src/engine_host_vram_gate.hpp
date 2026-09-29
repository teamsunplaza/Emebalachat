// Plan-B (REQ-B004) B-T2: host-side per-spawn VRAM gate — PURE decision core.
//
// Design: 234900_architect-planB-design.md §4.2, SUPERSEDED by
// 235700_architect-planB-design-v2-amendments.md §A2 (the governing contract).
// The v2 amendment removed the `model_cpu_estimate` term and restated the CPU
// leg numerically: cpu_ok is ALWAYS true (the CPU-offload worker allocates
// host RAM, not adapter VRAM), so the host-side contract guards ONLY the GPU
// leg. The worker's own RT-C gate (MtGpuOffloadGateDecision) remains the
// second independent check.
//
// This module is PURE LOGIC: no DXGI calls, no environment reads, no logging.
// `override_value` is the already-parsed EMEBALA_MT_GPU host value (B-T3 wires
// the env parse); `query_ok` is the already-probed DXGI success flag (B-T3
// calls QueryMtAdapterFreeVramBytes). Nothing here touches I/O.
//
// Dependency direction (091500_debug-p3-technical-gate-planb.md): this TU is a
// member of Emebalachat_host_v2, which must stay free of Emebalachat_engine_core
// (else the DXGI/llama decls in engine_core_helpers.hpp leak into every host_v2
// consumer). We therefore CANNOT include engine_core_helpers.hpp to reuse
// kMtGpuFreeVramThresholdBytes. Instead the 1.5 GiB threshold is restated here
// as a namespaced constexpr and pinned byte-equal to the engine_core constant
// by tests/vram_gate_test.inc (a TEST_CHECK, since a cross-namespace
// static_assert needs the engine_core header we must not include). If either
// side ever changes, that pin fails loudly.

#pragma once

namespace emebalachat {
namespace enginehost {
namespace vramgate {

// 1.5 GiB free-VRAM floor. Byte-identical to
// kMtGpuFreeVramThresholdBytes (engine_core_helpers.hpp:184) = 1536 MiB.
inline constexpr unsigned long long kSpawnVramThresholdBytes = 1536ull * 1024 * 1024;

// Why the gate reached its answer. Governing enum per the v2 amendment §A2
// (which superseded the baseline §4.2 list). `below_threshold_cpu` and
// `no_registry_entry` from the baseline design are GONE: under the cpu_ok=true
// rule there is no reachable CPU-refusal path, and "no registry entry" is a
// host-side lookup concern (the conservative 2048 MiB default), not a gate
// input — model_vram_bytes is always a concrete number by the time the gate
// is called.
enum class SpawnVramReason {
    Ok,                        // GPU leg satisfied the threshold on its own.
    BelowThresholdGpu,         // gpu_ok false (aggregate free-after-reserve < model + floor).
    QueryFailed,               // DXGI probe failed -> fail-closed (never allow GPU).
    OverrideForcedBelowThreshold, // override==1 forced GPU past a failed gpu_ok.
};

// Stable, shape-only string for decision logging at the B-T3 call site. Never
// user-facing; invariant #5 (shape-only logs, no user text).
const char* SpawnVramReasonString(SpawnVramReason reason);

struct SpawnVramDecision {
    bool allow_gpu;          // true: spawn with GPU offload permitted.
    bool allow_cpu;          // true: spawn on the CPU leg. ALWAYS true under v2.
    unsigned long long estimated_free_bytes;  // shape-only, for logging (== free_bytes).
    unsigned long long reserved_bytes;        // sum of running translate workers' vram (echo).
    SpawnVramReason reason;
};

// Pure decision core (unit-testable, no I/O). Self-contained per v2 §A2:
// every input is a parameter; there is NO model_cpu_estimate term.
//   gpu_ok = (free - reserved - model) >= threshold
//   cpu_ok = true   // CPU leg always permitted (host RAM, not adapter VRAM)
//   if override == 1: allow_gpu = query_ok && gpu_ok   (force GPU, honest about risk)
//   if override == 0: allow_gpu = false
//   if override == other (auto/-1): allow_gpu = query_ok && gpu_ok
// In every case allow_cpu = true and reserved_bytes is echoed verbatim.
//
// override_value uses the kMtGpuOverride* states: 1 == force GPU, 0 == force
// CPU, anything else (incl. -1 auto) == no override. A forced GPU (override==1)
// still requires query_ok: a failed DXGI probe means the adapter budget is
// unknown, and forcing GPU blind would defeat the RT-C fail-closed contract.
SpawnVramDecision DecideSpawnVram(int override_value, bool query_ok,
                                  unsigned long long free_bytes,
                                  unsigned long long reserved_bytes,
                                  unsigned long long model_vram_bytes);

} // namespace vramgate
} // namespace enginehost
} // namespace emebalachat
