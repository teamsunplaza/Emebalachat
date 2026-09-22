#pragma once

// ---------------------------------------------------------------------------
// translation_common — REQ-043 (plan §5.1-1): the translation-only core of the
// local Hy-MT2 engine, extracted from src/engine.cpp's private
// TranslationManager::LlamaEngine so that BOTH the shared inference host
// (Emebala.Engine.exe, src/host_main.cpp) and the M6-removed in-app path ran
// BYTE-IDENTICAL inference logic with zero duplication.
// REQ-043 (M6 T1, design §4.2/D-1): this pair now lives in src/engine_core/
// inside the Emebalachat_engine_core static library (physical move only).
// REQ-043 (M6 fold2): the in-app embedded inference path has since been
// REMOVED (no-llama app exe; the shared host is the only local serving
// source, plan §V2-8.6) — this header documents the extraction move only.
//
// What moved here VERBATIM from engine.cpp (bodies unchanged, comments kept):
//   * kPenaltyLastN + its compile-time pin
//   * the REQ-R01 context-budget static_asserts
//   * LlamaAbortIfCanceled / LlamaLoadProgress (REQ-R16 abort/progress seams)
//   * the P7-F2 split_mode enum-layout static_assert
//   * SetGpuOffloadParams (definition; declaration stays in engine.hpp)
//   * TranslationManager::LlamaEngine (both the HAVE_LLAMA_CPP implementation
//     and the no-llama stub), renamed LocalInferenceEngine
//
// Deliberately NOT moved (still shared through Emebalachat_core): the pure
// helpers declared in engine.hpp — TruncateHeadTailWindow, ScrubControlTokenTexts,
// CollectControlTokenTexts, IsValidModelPath, VerifyModelSha256,
// kExpectedModelSha256 — plus BuildPrompt (config.cpp) and NormalizeNFC /
// ToUtf8 / ToUtf16 (unicode_utils.cpp). The host links Emebalachat_core, so it
// resolves the same objects; nothing is duplicated.
//
// Cancellation contract (unchanged from the pre-extraction code): the engine
// reads *cancel_flag between decode steps and from llama.cpp's abort callback
// (CPU chunks). REQ-051 U-1 FIX 2 splits the MODEL-LOAD leg onto its own
// load_cancel_flag (default: aliases cancel_flag, so legacy callers are
// byte-identical): the worker points it at a never-set atomic so a per-job
// abort unwinds the DECODE but never discards a multi-minute load (the
// never-converging load spiral). Callers serialize inference; both flag
// addresses must stay stable for the engine's lifetime (TranslationManager
// wires its own latch; the host wires one fixed per-request atomic — see
// host_main.cpp).
//
// REQ-051 U-1 FIX 1: the decode loop additionally carries a wall-clock budget
// (kDecodeWallClockBudgetMs) and an input-scaled generation cap
// (ScaledMaxGenTokens): a degenerate loop on a user GGUF stops sampling at the
// budget and reports exhaustion through decode_wall_clock_exhausted() so the
// worker can answer the frozen "timeout" code (transient -> client retry).
//
// At extraction time the in-app path's runtime behavior was UNCHANGED by the
// move: TranslationManager::LlamaEngine was a zero-member derived class
// (engine.cpp), forwarding every call to this implementation verbatim. That
// in-app class was later deleted outright in M6 — only the worker-side copy
// remains live now.
// ---------------------------------------------------------------------------

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

#if defined(HAVE_LLAMA_CPP) || __has_include("llama.h")
#ifndef HAVE_LLAMA_CPP
#define HAVE_LLAMA_CPP 1
#endif
#include "llama.h"
#endif

// REQ-051 U-1 FIX 1: the input-scaled generation cap clamps against
// kLlamaGenReserve (the REQ-R01 budget constant lives in this library's
// llama-independent helper header; re-exported through engine.hpp for the
// existing consumers).
#include "engine_core_helpers.hpp"

namespace emebalachat {

// REQ-051 U-1 FIX 1 (live bug U-1, user-GGUF serving intermittency): the
// decode wall-clock budget. A healthy Hy-MT2 translation finishes in
// ~78-171 ms warm / <= ~2 s worst case; a degenerate non-Hy-MT2 loop under
// VRAM contention burned the ENTIRE 2048-token generation reserve at
// ~14 ms/token (~28.5 s), colliding with the frozen 30 s client budget.
// 12 s is generous for every legitimate translation (even a 10x-thrashed
// decode gets ONE honest timeout + the client's one-shot transient retry
// instead of a 30 s freeze): 12 s + 400 ms backoff + 12 s ~= 25 s stays
// inside the frozen 30 s budget.
inline constexpr int kDecodeWallClockBudgetMs = 12000;

// REQ-059 perf (live: MiLM long paragraphs slow/failing): input-scaled
// decode wall-clock budget. The flat 12 s REQ-051 budget was sized for
// Hy-MT2-1.8B (healthy <= ~2 s); MiLM-class 4B user models decode ~2x+
// slower per token, so a long LEGITIMATE translation could exceed 12 s,
// be discarded -> "timeout" -> the one-shot retry exhausts -> the user
// sees a failure on long paragraphs. The budget now scales with the
// generation cap at a worst-case-assumed 50 tok/s (20 ms/token) —
// clamp(max_gen_tokens * 20, kDecodeWallClockBudgetMs /*12 s floor, this
// named constant is UNTOUCHED*/, kDecodeWallClockBudgetMaxMs). The 12 s
// floor holds while max_gen_tokens <= 600 (prompt n <= 118); long
// translations (both models) scale up to 27 s, staying under the frozen
// 30 s v1 client budget including the one-shot retry headroom. The
// REQ-051 U-1 latch semantics (timeout wire code on exhaust) are
// unchanged.
inline constexpr int kDecodeMsPerAssumedToken = 20;
inline constexpr int kDecodeWallClockBudgetMaxMs = 27000;

// REQ-059 perf: budget_ms = clamp(max_gen_tokens * kDecodeMsPerAssumedToken,
// kDecodeWallClockBudgetMs, kDecodeWallClockBudgetMaxMs). Pure/constexpr so
// the unit suite pins the clamp headlessly (mirrors ScaledMaxGenTokens).
constexpr int ScaledDecodeWallClockBudgetMs(int max_gen_token_count) {
    const long long scaled =
        static_cast<long long>(max_gen_token_count) * kDecodeMsPerAssumedToken;
    if (scaled < kDecodeWallClockBudgetMs) {
        return kDecodeWallClockBudgetMs;
    }
    if (scaled > kDecodeWallClockBudgetMaxMs) {
        return kDecodeWallClockBudgetMaxMs;
    }
    return static_cast<int>(scaled);
}

// REQ-051 U-1 FIX 1: input-scaled generation cap constants. Translation
// outputs beyond (4x input + 128) tokens do not legitimately exist; the cap
// is floored at 256 (a one-token prompt still deserves a full sentence) and
// clamped at kLlamaGenReserve (2048 stays the named ceiling constant — the
// unit-test pins and the REQ-R01 static_asserts are untouched).
inline constexpr int kScaledGenFloorTokens = 256;
inline constexpr int kScaledGenPerInputToken = 4;
inline constexpr int kScaledGenFlatTokens = 128;

// REQ-051 U-1 FIX 1: max_gen_tokens = min(kLlamaGenReserve, max(256, 4n+128)).
// Pure/constexpr so the unit suite can pin the whole matrix headlessly.
// Overflow-safe: the multiply happens only after the ceiling crossover
// short-circuit, so adversarial token counts can never reach it.
constexpr int ScaledMaxGenTokens(int input_token_count) {
    if (input_token_count <= 0) {
        return kScaledGenFloorTokens;
    }
    constexpr int kCeilingCrossover =
        (kLlamaGenReserve - kScaledGenFlatTokens) / kScaledGenPerInputToken; // 480
    if (input_token_count >= kCeilingCrossover) {
        return kLlamaGenReserve;
    }
    const int scaled = input_token_count * kScaledGenPerInputToken + kScaledGenFlatTokens;
    return scaled < kScaledGenFloorTokens ? kScaledGenFloorTokens : scaled;
}

// REQ-043: headless local inference engine (see file header for provenance).
// All members are public exactly as in the original struct; the host drives it
// from its single inference worker thread, the app from TranslationManager
// under its mutex — the caller-serialized discipline the original code
// documented (control_token_texts needs no separate lock) is preserved by
// both call sites.
class LocalInferenceEngine {
public:
#ifdef HAVE_LLAMA_CPP
    llama_model* model = nullptr;
    llama_context* ctx = nullptr;
    const llama_vocab* vocab = nullptr;
#else
    // REQ-043: no-llama stub keeps the SAME member layout surface the
    // TranslationManager leg touches (loaded_path/cancel_flag) so the derived
    // shim in engine.cpp compiles in both configurations.
    void* model = nullptr;
    void* ctx = nullptr;
    void* vocab = nullptr;
#endif
    std::string loaded_path;
    // REQ-R16: cancellation flag owned by the caller (null when the engine was
    // created without one, e.g. in isolation tests - then cancellation is
    // simply unavailable and behavior is the old full-run).
    const std::atomic<bool>* cancel_flag = nullptr;
    // REQ-051 U-1 FIX 2: the model-load cancellation flag. NULL (the default)
    // makes the load leg ALIAS cancel_flag — byte-identical to the historical
    // single-flag behavior. A caller that must never discard an in-flight
    // load (the ggml-translate worker: a per-job abort unwinds the decode via
    // cancel_flag but the multi-minute load, once started, runs to completion
    // so the next job never repays it) points this at its own never-set
    // atomic. EnsureLoaded's progress callback and its cancel checks read
    // THIS flag (see LoadCancelRequested); the decode loop keeps cancel_flag.
    const std::atomic<bool>* load_cancel_flag = nullptr;
    // REQ-051 U-1 FIX 1: set when a Translate() decode loop exhausts
    // kDecodeWallClockBudgetMs (the worker maps it to the frozen "timeout"
    // wire code); cleared at every Translate() entry. Plain bool — Translate()
    // is caller-serialized, exactly like control_texts_built below.
    bool decode_wall_clock_exhausted_ = false;

    bool CancelRequested() const {
        return cancel_flag && cancel_flag->load(std::memory_order_acquire);
    }

    // REQ-051 U-1 FIX 2: the effective load-cancellation flag (load_cancel_flag
    // when wired, cancel_flag otherwise). EnsureLoaded consults this so the
    // default configuration keeps the legacy single-flag semantics.
    const std::atomic<bool>* EffectiveLoadCancelFlag() const {
        return load_cancel_flag ? load_cancel_flag : cancel_flag;
    }
    bool LoadCancelRequested() const {
        const std::atomic<bool>* flag = EffectiveLoadCancelFlag();
        return flag && flag->load(std::memory_order_acquire);
    }
    bool decode_wall_clock_exhausted() const {
        return decode_wall_clock_exhausted_;
    }
    // SEC-B2 (session 260911_0002, verify 233020): vocab-derived control-token
    // scrub set, built lazily on the first Translate() after a (re)load and
    // invalidated by Unload(). Translation requests are caller-serialized, so
    // this cache needs no separate lock (original contract, kept verbatim).
    std::vector<std::wstring> control_token_texts;
    bool control_texts_built = false;

    LocalInferenceEngine();
    ~LocalInferenceEngine();

    void Unload();
    bool EnsureLoaded(const std::string& path);

    // R6 Phase 4 (B2, plan §4.1 item 2): src_name is the resolved SOURCE token
    // for the prompt hint (""/AUTO = no hint, historical behavior).
    std::wstring Translate(
        std::wstring_view text,
        std::string_view tgt_name,
        std::string_view src_name,
        const std::string& path,
        float temperature = 0.0f,
        float top_p = 0.6f,
        int top_k = 20,
        float rep_pen = 1.05f
    );
};

} // namespace emebalachat
