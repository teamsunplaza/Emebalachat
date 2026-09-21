// ---------------------------------------------------------------------------
// REQ-051 U-1 FIX 1 + FIX 2 (session 260922, live bug U-1 — user-model serving
// intermittency): the decode wall-clock budget, the input-scaled generation
// cap, and the load/decode cancellation split. Staged as an .inc next to this
// runner; registered near the end of main() after TestReq051EngineRetryPolicy.
//
// Root cause pinned by evidence: a user GGUF looping on the fixed Hy-MT2
// prompt burned the ENTIRE 2048-token generation reserve under VRAM
// contention (~14 ms/token -> ~28.5 s), colliding with the frozen 30 s client
// budget; a per-job abort mid-load discarded the multi-minute load so the
// next request repaid it (never-converging spiral).
//
// Coverage:
//   (a) ScaledMaxGenTokens: the pure cap min(kLlamaGenReserve, max(256,
//       4n+128)) over a size matrix (0/1/small/large inputs, the 256 floor,
//       the 2048 ceiling at the 480-token crossover, overflow-proof huge
//       inputs) plus a full-sweep equivalence proof against the reference
//       formula. kLlamaGenReserve stays the named 2048 ceiling constant.
//   (b) Constants: the 12000 ms wall-clock budget pin.
//   (c) Runtime engine semantics: decode_wall_clock_exhausted() default-false
//       on a fresh engine; the load_cancel_flag split — NULL aliases
//       cancel_flag (legacy byte-identical), a wired flag overrides it, and
//       re-NULLing restores the alias.
//   (d) Structural pins (ResolveRepoFile + raw-byte read, the
//       TestReq048CloudFailureSignaling precedent; every needle ASCII-only):
//       translation_common.cpp clears the latch at BOTH Translate() entries
//       (llama + no-llama stub parity), sets it only on wall-clock expiry,
//       builds the deadline from kDecodeWallClockBudgetMs, wires the scaled
//       cap at the token loop, and routes EnsureLoaded's cancel checks +
//       progress callback through the LOAD flag; ggml_translate_worker.cpp
//       answers the EXISTING "timeout" code for an exhausted decode (the
//       abort arm unchanged) and pins the engine's load_cancel_flag to the
//       never-set atomic.
// ---------------------------------------------------------------------------

#include "../src/engine_core/translation_common.hpp" // REQ-051 U-1: LocalInferenceEngine + ScaledMaxGenTokens (mid-TU include, pragma-once safe)

// Compile-time pins (the header constants are inline constexpr — a drifted
// value fails the BUILD here instead of silently shipping).
static_assert(emebalachat::kDecodeWallClockBudgetMs == 12000,
              "REQ-051 U-1 FIX 1: the decode wall-clock budget is 12 s");
static_assert(emebalachat::ScaledMaxGenTokens(0) == emebalachat::kScaledGenFloorTokens,
              "REQ-051 U-1 FIX 1: a zero-token input gets the 256-token floor");
static_assert(emebalachat::ScaledMaxGenTokens(32) == emebalachat::kScaledGenFloorTokens,
              "REQ-051 U-1 FIX 1: 4*32+128 == 256 (the exact floor boundary)");
static_assert(emebalachat::ScaledMaxGenTokens(480) == emebalachat::kLlamaGenReserve,
              "REQ-051 U-1 FIX 1: the ceiling crossover lands on kLlamaGenReserve");

void TestReq051ScaledGenCap() {
    std::cout << "[RUN] Testing REQ-051 U-1 input-scaled generation cap..." << std::endl;
    const int failures_before = g_failed_count;
    using emebalachat::kLlamaGenReserve;
    using emebalachat::kScaledGenFloorTokens;
    using emebalachat::ScaledMaxGenTokens;

    // ---- the size matrix ---------------------------------------------------
    TEST_CHECK(ScaledMaxGenTokens(0) == 256,
               "REQ-051 U-1: 0 input tokens -> the 256 floor");
    TEST_CHECK(ScaledMaxGenTokens(1) == 256,
               "REQ-051 U-1: 1 input token (4+128=132) -> the 256 floor");
    TEST_CHECK(ScaledMaxGenTokens(-7) == 256,
               "REQ-051 U-1: a negative count is degenerate -> the 256 floor");
    TEST_CHECK(ScaledMaxGenTokens(31) == 256,
               "REQ-051 U-1: 31 tokens (4n+128=252) still floors at 256");
    TEST_CHECK(ScaledMaxGenTokens(32) == 256,
               "REQ-051 U-1: 32 tokens (4n+128=256) is the exact floor");
    TEST_CHECK(ScaledMaxGenTokens(33) == 260,
               "REQ-051 U-1: 33 tokens scales to 260");
    TEST_CHECK(ScaledMaxGenTokens(100) == 528,
               "REQ-051 U-1: 100 tokens scales to 528");
    TEST_CHECK(ScaledMaxGenTokens(479) == 2044,
               "REQ-051 U-1: 479 tokens scales to 2044 (just under the ceiling)");
    TEST_CHECK(ScaledMaxGenTokens(480) == kLlamaGenReserve,
               "REQ-051 U-1: the 480-token crossover clamps at kLlamaGenReserve (2048)");
    TEST_CHECK(ScaledMaxGenTokens(481) == kLlamaGenReserve,
               "REQ-051 U-1: past the crossover the cap stays 2048");
    TEST_CHECK(ScaledMaxGenTokens(100000) == kLlamaGenReserve,
               "REQ-051 U-1: a huge input clamps at 2048 (no overflow)");
    TEST_CHECK(ScaledMaxGenTokens(2147483647) == kLlamaGenReserve,
               "REQ-051 U-1: INT_MAX input clamps at 2048 (multiply short-circuited)");
    volatile int ceiling = kLlamaGenReserve;
    TEST_CHECK(ceiling == 2048,
               "REQ-051 U-1: kLlamaGenReserve stays the named 2048 ceiling constant");

    // ---- full-sweep equivalence with the reference formula -----------------
    // min(2048, max(256, 4n+128)) computed in 64-bit; the sweep covers the
    // floor ramp, the crossover, and past it.
    int mismatches = 0;
    for (long long n = -64; n <= 600; ++n) {
        const long long scaled = 4 * n + 128;
        const long long floored = scaled < 256 ? 256 : scaled;
        const int ref = static_cast<int>(floored > 2048 ? 2048 : floored);
        if (ScaledMaxGenTokens(static_cast<int>(n)) != ref) {
            ++mismatches;
        }
    }
    TEST_CHECK(mismatches == 0,
               "REQ-051 U-1: ScaledMaxGenTokens matches min(2048, max(256, 4n+128)) across the whole sweep");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-051 U-1 scaled generation cap passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-051 U-1 scaled generation cap: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestReq051DecodeWallClockSemantics() {
    std::cout << "[RUN] Testing REQ-051 U-1 decode budget + load-cancel semantics..." << std::endl;
    const int failures_before = g_failed_count;
    using emebalachat::kDecodeWallClockBudgetMs;
    using emebalachat::LocalInferenceEngine;

    // ---- (b) the wall-clock constant ---------------------------------------
    volatile int budget = kDecodeWallClockBudgetMs;
    TEST_CHECK(budget == 12000,
               "REQ-051 U-1 FIX 1: the decode wall-clock budget is 12000 ms");

    // ---- (c) runtime engine semantics --------------------------------------
    // A fresh engine: the exhaustion latch starts clear (default-false).
    LocalInferenceEngine engine;
    TEST_CHECK(!engine.decode_wall_clock_exhausted(),
               "REQ-051 U-1 FIX 1: decode_wall_clock_exhausted() is false on a fresh engine");

    // REQ-051 U-1 FIX 2 split: NULL load_cancel_flag ALIASES cancel_flag (the
    // legacy byte-identical behavior); a wired flag overrides the alias; a
    // re-NULL restores it.
    std::atomic<bool> cancel_true{true};
    std::atomic<bool> never_set{false};
    engine.cancel_flag = &cancel_true;
    engine.load_cancel_flag = nullptr;
    TEST_CHECK(engine.CancelRequested() && engine.LoadCancelRequested(),
               "REQ-051 U-1 FIX 2: with no load flag wired, the load leg reads cancel_flag (alias)");
    TEST_CHECK(engine.EffectiveLoadCancelFlag() == &cancel_true,
               "REQ-051 U-1 FIX 2: the effective load flag aliases cancel_flag by default");
    engine.load_cancel_flag = &never_set;
    TEST_CHECK(!engine.LoadCancelRequested(),
               "REQ-051 U-1 FIX 2: a wired never-set load flag overrides a set cancel_flag");
    TEST_CHECK(engine.CancelRequested(),
               "REQ-051 U-1 FIX 2: the decode leg still sees cancel_flag while the load leg is split");
    TEST_CHECK(engine.EffectiveLoadCancelFlag() == &never_set,
               "REQ-051 U-1 FIX 2: the effective load flag follows the wired pointer");
    engine.load_cancel_flag = nullptr;
    TEST_CHECK(engine.LoadCancelRequested(),
               "REQ-051 U-1 FIX 2: clearing the load flag restores the cancel_flag alias");
    engine.cancel_flag = nullptr;
    TEST_CHECK(!engine.LoadCancelRequested(),
               "REQ-051 U-1 FIX 2: no flag at all -> no load cancellation (legacy isolation)");

    // ---- (d) structural pins: translation_common.cpp ------------------------
    const std::filesystem::path tc_path =
        ResolveRepoFile(L"src\\engine_core\\translation_common.cpp");
    TEST_CHECK(!tc_path.empty(),
               "REQ-051 U-1: src/engine_core/translation_common.cpp located via ResolveRepoFile");
    if (!tc_path.empty()) {
        std::ifstream tc_in(tc_path, std::ios::binary);
        TEST_CHECK(static_cast<bool>(tc_in),
                   "REQ-051 U-1: translation_common.cpp opened for reading");
        if (tc_in) {
            const std::string tc_src((std::istreambuf_iterator<char>(tc_in)),
                                     std::istreambuf_iterator<char>());
            TEST_CHECK(!tc_src.empty(), "REQ-051 U-1: translation_common.cpp non-empty after read");

            // The latch is cleared at EVERY Translate() entry: the llama leg
            // AND the no-llama stub (parity across build configs).
            std::size_t clears = 0;
            for (std::size_t pos = 0;
                 (pos = tc_src.find("decode_wall_clock_exhausted_ = false;", pos)) !=
                 std::string::npos;
                 pos += 37) {
                ++clears;
            }
            TEST_CHECK(clears == 2,
                       "REQ-051 U-1 pin: the latch is cleared at both Translate() entries (llama + stub)");

            // Set in exactly one place: the wall-clock expiry arm of the loop.
            std::size_t sets = 0;
            for (std::size_t pos = 0;
                 (pos = tc_src.find("decode_wall_clock_exhausted_ = true;", pos)) !=
                 std::string::npos;
                 pos += 36) {
                ++sets;
            }
            TEST_CHECK(sets == 1,
                       "REQ-051 U-1 pin: the latch is set only by the wall-clock expiry arm");

            // The deadline is built from the pinned budget constant ...
            TEST_CHECK(tc_src.find(
                           "decode_deadline = decode_start + "
                           "std::chrono::milliseconds(kDecodeWallClockBudgetMs)") !=
                           std::string::npos,
                       "REQ-051 U-1 pin: the decode deadline derives from kDecodeWallClockBudgetMs");
            // ... the cap is wired at the token loop ...
            TEST_CHECK(tc_src.find("const int max_gen_tokens = ScaledMaxGenTokens(n_prompt_tokens);") !=
                           std::string::npos,
                       "REQ-051 U-1 pin: the token loop uses the input-scaled cap");
            // ... the expiry answers empty through the timeout-shaped arm
            // (shape-only diagnostic, no content) ...
            TEST_CHECK(tc_src.find("ENGINE/Translate/033") != std::string::npos,
                       "REQ-051 U-1 pin: the expiry logs the shape-only ENGINE/Translate/033 line");

            // FIX 2: the load leg reads the LOAD flag — EnsureLoaded's two
            // cancel checks and the progress-callback user_data both route
            // through it, while the decode loop keeps the REQ-R16 cancel_flag.
            std::size_t load_checks = 0;
            for (std::size_t pos = 0;
                 (pos = tc_src.find("LoadCancelRequested()", pos)) != std::string::npos;
                 pos += 21) {
                ++load_checks;
            }
            TEST_CHECK(load_checks >= 2,
                       "REQ-051 U-1 pin: EnsureLoaded consults the load flag (pre-load + post-failure)");
            TEST_CHECK(tc_src.find("static_cast<const void*>(EffectiveLoadCancelFlag())") !=
                           std::string::npos,
                       "REQ-051 U-1 pin: the load-progress callback is wired to the effective load flag");
            TEST_CHECK(tc_src.find("ENGINE/Translate/032") != std::string::npos,
                       "REQ-051 U-1 pin: the decode loop still honors the REQ-R16 cancel flag");
        }
    }

    // ---- (d) structural pins: ggml_translate_worker.cpp ---------------------
    const std::filesystem::path worker_path = ResolveRepoFile(L"src\\ggml_translate_worker.cpp");
    TEST_CHECK(!worker_path.empty(),
               "REQ-051 U-1: src/ggml_translate_worker.cpp located via ResolveRepoFile");
    if (!worker_path.empty()) {
        std::ifstream w_in(worker_path, std::ios::binary);
        TEST_CHECK(static_cast<bool>(w_in),
                   "REQ-051 U-1: ggml_translate_worker.cpp opened for reading");
        if (w_in) {
            const std::string w_src((std::istreambuf_iterator<char>(w_in)),
                                    std::istreambuf_iterator<char>());
            TEST_CHECK(!w_src.empty(), "REQ-051 U-1: ggml_translate_worker.cpp non-empty after read");

            // The abort path is byte-stable ...
            TEST_CHECK(w_src.find("const bool aborted = g_cancel.load(std::memory_order_acquire);") !=
                           std::string::npos,
                       "REQ-051 U-1 pin: the abort arm still reads g_cancel (unchanged)");
            // ... the wall-clock latch is consumed next to it ...
            TEST_CHECK(w_src.find("engine->decode_wall_clock_exhausted()") != std::string::npos,
                       "REQ-051 U-1 pin: the worker reads the engine's wall-clock latch");
            const size_t route_pos = w_src.find("aborted || decode_wall_clock_exhausted");
            TEST_CHECK(route_pos != std::string::npos,
                       "REQ-051 U-1 pin: the exhausted decode joins the abort arm");
            // ... and the shared arm answers the EXISTING wire code "timeout"
            // (no new frame shape; the frozen client classifies it transient).
            if (route_pos != std::string::npos) {
                const size_t arm_end = w_src.find("model_missing", route_pos);
                const std::string arm = w_src.substr(
                    route_pos, arm_end == std::string::npos ? std::string::npos : arm_end - route_pos);
                TEST_CHECK(arm.find("ev.code = \"timeout\";") != std::string::npos,
                           "REQ-051 U-1 pin: the joined arm answers the existing \"timeout\" code");
            }

            // FIX 2: the worker pins the engine's load flag to the never-set
            // atomic so a started load always finishes.
            TEST_CHECK(w_src.find("std::atomic<bool> g_never_cancel_load{false};") !=
                           std::string::npos,
                       "REQ-051 U-1 pin: the never-set load atomic exists");
            TEST_CHECK(w_src.find("engine->load_cancel_flag = &g_never_cancel_load;") !=
                           std::string::npos,
                       "REQ-051 U-1 pin: the worker wires load_cancel_flag to the never-set atomic");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-051 U-1 decode budget + load-cancel semantics passed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-051 U-1 decode budget + load-cancel semantics: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
