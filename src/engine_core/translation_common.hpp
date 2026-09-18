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
// (CPU chunks) and the model-load progress callback. Callers serialize
// inference; the flag's address must stay stable for the engine's lifetime
// (TranslationManager wires its own latch; the host wires one fixed
// per-request atomic — see host_main.cpp).
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

namespace emebalachat {

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
    // SEC-B2 (session 260911_0002, verify 233020): vocab-derived control-token
    // scrub set, built lazily on the first Translate() after a (re)load and
    // invalidated by Unload(). Translation requests are caller-serialized, so
    // this cache needs no separate lock (original contract, kept verbatim).
    std::vector<std::wstring> control_token_texts;
    bool control_texts_built = false;

    bool CancelRequested() const {
        return cancel_flag && cancel_flag->load(std::memory_order_acquire);
    }

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
