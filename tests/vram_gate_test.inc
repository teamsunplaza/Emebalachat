// Plan-B (REQ-B004) B-T2: host-side per-spawn VRAM gate pins.
// Included directly by tests/run_tests.cpp (the staged session .inc contract;
// uses the TEST_CHECK harness + the std:: facilities included there).
//
//   * Threshold-equality pin: vramgate::kSpawnVramThresholdBytes is byte-equal
//     to engine_core's kMtGpuFreeVramThresholdBytes (the constant this module
//     deliberately restates rather than includes — see the header).
//   * The 7 governing pins of 235700_architect-planB-design-v2-amendments.md
//     §A2 "AFTER" list (threshold = 1.5 GiB; free/reserved/model in bytes).
//   * Supplementary pins: exact headroom boundary (== threshold), unsigned
//     underflow (reserved > free), and reason-string stability.
//
// This suite is PURE LOGIC: no DXGI, no env reads, no worker spawns.

void TestVramGate() {
    std::cout << "[RUN] Testing VRAM spawn gate (Plan-B B-T2)..." << std::endl;
    const int failures_before = g_failed_count;
    namespace vg = emebalachat::enginehost::vramgate;

    using vg::DecideSpawnVram;
    using vg::SpawnVramReason;
    using vg::SpawnVramReasonString;

    const unsigned long long kGiB = 1024ull * 1024 * 1024;
    const unsigned long long kThreshold = vg::kSpawnVramThresholdBytes; // 1.5 GiB
    const unsigned long long kModel2GiB = 2 * kGiB;

    // ---- Threshold-equality pin (dependency-direction decision) -------------
    // engine_core_helpers.hpp IS includable here (run_tests links engine_core)
    // even though the gate TU itself must not. This proves the restated
    // constant stays byte-equal to the RT-C floor it extends. Fully qualified
    // so the pin holds even if the runner's `using namespace emebalachat` moves.
    TEST_CHECK(emebalachat::kMtGpuFreeVramThresholdBytes == 1536ull * 1024 * 1024,
               "vram-gate: engine_core floor is 1.5 GiB (sanity)");
    TEST_CHECK(kThreshold == emebalachat::kMtGpuFreeVramThresholdBytes,
               "vram-gate: kSpawnVramThresholdBytes == kMtGpuFreeVramThresholdBytes");
    TEST_CHECK(kThreshold == 1610612736ull, "vram-gate: 1.5 GiB == 1610612736 bytes");

    // ---- Pin 1: free=8GiB, reserved=0, model=2GiB -> gpu+cpu, reason ok -----
    {
        auto d = DecideSpawnVram(-1, true, 8 * kGiB, 0, kModel2GiB);
        TEST_CHECK(d.allow_gpu, "vram-gate pin1: 8GiB free, 2GiB model -> allow_gpu");
        TEST_CHECK(d.allow_cpu, "vram-gate pin1: allow_cpu true");
        TEST_CHECK(d.reason == SpawnVramReason::Ok, "vram-gate pin1: reason ok");
        TEST_CHECK(d.reserved_bytes == 0, "vram-gate pin1: reserved echoed == 0");
        TEST_CHECK(d.estimated_free_bytes == 8 * kGiB, "vram-gate pin1: free echoed");
    }

    // ---- Pin 2: free=2GiB, reserved=1.5GiB, model=2GiB -> !gpu+cpu ----------
    // (2 - 1.5 - 2) GiB = -1.5 GiB -> headroom 0 < 1.5 GiB.
    {
        auto d = DecideSpawnVram(-1, true, 2 * kGiB, kThreshold, kModel2GiB);
        TEST_CHECK(!d.allow_gpu, "vram-gate pin2: negative headroom -> !allow_gpu");
        TEST_CHECK(d.allow_cpu, "vram-gate pin2: allow_cpu true (CPU leg host-RAM)");
        TEST_CHECK(d.reason == SpawnVramReason::BelowThresholdGpu,
                   "vram-gate pin2: reason below_threshold_gpu");
    }

    // ---- Pin 3: free=0.5GiB, reserved=1.5GiB, model=2GiB -> !gpu+cpu --------
    // Underflow shape: reserved (1.5GiB) alone already exceeds free (0.5GiB).
    {
        auto d = DecideSpawnVram(-1, true, kGiB / 2, kThreshold, kModel2GiB);
        TEST_CHECK(!d.allow_gpu, "vram-gate pin3: reserved>free underflow -> !allow_gpu");
        TEST_CHECK(d.allow_cpu, "vram-gate pin3: allow_cpu true (never refuses)");
        TEST_CHECK(d.reason == SpawnVramReason::BelowThresholdGpu,
                   "vram-gate pin3: reason below_threshold_gpu");
    }

    // ---- Pin 4: query_ok=false -> allow_gpu=false (fail-closed) -------------
    {
        auto d = DecideSpawnVram(-1, false, 8 * kGiB, 0, kModel2GiB);
        TEST_CHECK(!d.allow_gpu, "vram-gate pin4: probe failure -> !allow_gpu");
        TEST_CHECK(d.allow_cpu, "vram-gate pin4: allow_cpu true");
        TEST_CHECK(d.reason == SpawnVramReason::QueryFailed,
                   "vram-gate pin4: reason query_failed");
    }

    // ---- Pin 5: override=1, gpu_ok=false -> forced gpu + reason -------------
    {
        auto d = DecideSpawnVram(1, true, 2 * kGiB, kThreshold, kModel2GiB);
        TEST_CHECK(d.allow_gpu, "vram-gate pin5: override=1 forces GPU below threshold");
        TEST_CHECK(d.allow_cpu, "vram-gate pin5: allow_cpu true");
        TEST_CHECK(d.reason == SpawnVramReason::OverrideForcedBelowThreshold,
                   "vram-gate pin5: reason override_forced_below_threshold");
    }

    // ---- Pin 6: override=0 -> never gpu ------------------------------------
    {
        auto d = DecideSpawnVram(0, true, 8 * kGiB, 0, kModel2GiB);
        TEST_CHECK(!d.allow_gpu, "vram-gate pin6: override=0 -> !allow_gpu despite ample VRAM");
        TEST_CHECK(d.allow_cpu, "vram-gate pin6: allow_cpu true");
    }

    // ---- Pin 7: aggregate accounting across co-resident workers -------------
    // Governing example (v2 §A2): free=8GiB, model=2GiB. First spawn (reserved=0):
    // headroom = 8 - 0 - 2 = 6 GiB >= 1.5 -> gpu_ok. Second spawn once the first
    // worker is running (reserved=2GiB): headroom = 8 - 2 - 2 = 4 GiB >= 1.5 ->
    // gpu_ok STILL true (8 GiB is ample for two 2 GiB workers) — and crucially
    // the decision now accounts for the 2 GiB reservation (reserved_bytes echoes
    // it), which is the aggregate-accounting property this pin exists to prove.
    //
    // Boundary (still free=8GiB, model=2GiB): the running reservation at which
    // the same model exactly hits the floor is reserved = free - model - threshold
    // = 8 - 2 - 1.5 = 4.5 GiB. headroom = 8 - 4.5 - 2 = 1.5 GiB == threshold
    // (sufficient, >=); 4.5 GiB + 1 byte pushes headroom to threshold - 1 byte.
    {
        auto first = DecideSpawnVram(-1, true, 8 * kGiB, 0, kModel2GiB);
        TEST_CHECK(first.allow_gpu && first.reason == SpawnVramReason::Ok,
                   "vram-gate pin7: first worker (reserved=0) fits");
        TEST_CHECK(first.reserved_bytes == 0, "vram-gate pin7: first decision reserved==0");

        auto second = DecideSpawnVram(-1, true, 8 * kGiB, 2 * kGiB, kModel2GiB);
        TEST_CHECK(second.allow_gpu,
                   "vram-gate pin7: second 2GiB worker still fits in 8GiB (aggregate accounting)");
        TEST_CHECK(second.allow_cpu, "vram-gate pin7: second worker allow_cpu true");
        TEST_CHECK(second.reason == SpawnVramReason::Ok,
                   "vram-gate pin7: second worker reason ok");
        TEST_CHECK(second.reserved_bytes == 2 * kGiB,
                   "vram-gate pin7: reserved_bytes echoes the 2GiB running total");

        // Exact boundary: reserved = 4.5 GiB -> headroom == threshold (sufficient).
        const unsigned long long kReservedBoundary = 4 * kGiB + kGiB / 2;
        auto boundary = DecideSpawnVram(-1, true, 8 * kGiB, kReservedBoundary, kModel2GiB);
        TEST_CHECK(boundary.allow_gpu,
                   "vram-gate pin7: headroom == threshold exactly is sufficient (>=)");
        TEST_CHECK(boundary.reason == SpawnVramReason::Ok,
                   "vram-gate pin7: boundary reason ok");

        // One byte more reservation -> headroom = threshold - 1 -> below floor.
        auto over = DecideSpawnVram(-1, true, 8 * kGiB, kReservedBoundary + 1, kModel2GiB);
        TEST_CHECK(!over.allow_gpu,
                   "vram-gate pin7: one byte past the boundary -> !allow_gpu");
        TEST_CHECK(over.reason == SpawnVramReason::BelowThresholdGpu,
                   "vram-gate pin7: one-byte-over reason below_threshold_gpu");
    }

    // ---- Supplementary: exact headroom boundary (== threshold) --------------
    // Fixed 4 GiB budget, model chosen so headroom = free - model == threshold
    // exactly: model = free - threshold = 4 - 1.5 = 2.5 GiB -> gpu_ok true.
    // One byte more model -> headroom = threshold - 1 -> below floor.
    {
        const unsigned long long kFree = 4 * kGiB;
        const unsigned long long kModelBoundary = kFree - kThreshold; // 2.5 GiB
        auto d = DecideSpawnVram(-1, true, kFree, 0, kModelBoundary);
        TEST_CHECK(d.allow_gpu, "vram-gate: headroom == threshold is sufficient (>=)");
        TEST_CHECK(d.reason == SpawnVramReason::Ok, "vram-gate: boundary reason ok");

        auto e = DecideSpawnVram(-1, true, kFree, 0, kModelBoundary + 1);
        TEST_CHECK(!e.allow_gpu, "vram-gate: headroom == threshold-1 byte is insufficient");
        TEST_CHECK(e.reason == SpawnVramReason::BelowThresholdGpu,
                   "vram-gate: one-byte-short reason below_threshold_gpu");
    }

    // ---- Supplementary: override=1 does NOT bypass a failed probe -----------
    // A forced GPU on an UNKNOWN budget would defeat the RT-C fail-closed
    // contract, so query_ok still gates the forced path.
    {
        auto d = DecideSpawnVram(1, false, 8 * kGiB, 0, kModel2GiB);
        TEST_CHECK(!d.allow_gpu, "vram-gate: override=1 cannot force GPU on a failed probe");
        TEST_CHECK(d.reason == SpawnVramReason::QueryFailed,
                   "vram-gate: forced+probe-fail reports query_failed");
    }

    // ---- Supplementary: reason strings are stable (shape-only logging) ------
    TEST_CHECK(std::string(SpawnVramReasonString(SpawnVramReason::Ok)) == "ok",
               "vram-gate: reason string 'ok'");
    TEST_CHECK(std::string(SpawnVramReasonString(SpawnVramReason::BelowThresholdGpu)) ==
                   "below_threshold_gpu",
               "vram-gate: reason string 'below_threshold_gpu'");
    TEST_CHECK(std::string(SpawnVramReasonString(SpawnVramReason::QueryFailed)) == "query_failed",
               "vram-gate: reason string 'query_failed'");
    TEST_CHECK(std::string(SpawnVramReasonString(SpawnVramReason::OverrideForcedBelowThreshold)) ==
                   "override_forced_below_threshold",
               "vram-gate: reason string 'override_forced_below_threshold'");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] VRAM spawn gate tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] VRAM spawn gate tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
