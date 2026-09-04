#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace emebalachat {

// M3 (security): Validates a GGUF model file path before it reaches the llama.cpp
// loader. Fail-closed: returns false and logs a traceable
// ENGINE/IsValidModelPath/NNN message to stderr when:
//   001 path is empty
//   002 path does not exist as a regular file
//   003 extension is not ".gguf" (case-insensitive)
//   004 relative path resolves (via '..' components) OUTSIDE base_dir
// base_dir defaults to the current working directory when empty. Absolute paths
// are not subject to the containment rule (they define their own location).
// Pure filesystem logic - unit-testable without loading any model.
bool IsValidModelPath(std::string_view path, std::string_view base_dir = {});

// REQ-R01 (Batch D1): llama.cpp context-window sizing constants, centralized so
// the decode-time budget and the unit tests agree on one source of truth.
// kLlamaNCtx must stay in sync with cparams.n_ctx in engine.cpp EnsureLoaded().
inline constexpr int kLlamaNCtx = 2048;
inline constexpr int kLlamaGenReserve = 512;    // tokens reserved for generation (max_gen_tokens)
inline constexpr int kLlamaTokenSafetyMargin = 16;
inline constexpr int kLlamaPromptTokenBudget =
    kLlamaNCtx - kLlamaGenReserve - kLlamaTokenSafetyMargin; // 1520

// REQ-R01: pure, model-independent head+tail sliding-window truncation of source
// text. Returns `text` unchanged when it already fits (2 * keep_per_side >= size).
// Otherwise keeps the first and last `keep_per_side` UTF-16 code units joined by
// "\n…\n". Cut points are shifted inward so a UTF-16 surrogate pair is never
// split (a lone surrogate would corrupt the UTF-8 conversion and the tokenizer).
// Exposed for unit testing; the engine drives it via a token-count binary search.
std::wstring TruncateHeadTailWindow(std::wstring_view text, size_t keep_per_side);

// REQ-R02 (Batch D1): explicit outcome of TranslationManager::Translate.
// The bare-empty-wstring silent failure (H2 regression, audit §2.1) is replaced
// by a surfaced status so the worker can give user-audible feedback instead of
// dropping the translation without a trace.
enum class TranslationStatus {
    Ok,                  // non-empty translation produced (local, cloud, or fallback)
    InputEmpty,          // caller passed empty text; nothing to do, not a failure
    CloudConsentBlocked, // local engine failed/unavailable and the H2 privacy gate
                         // refused the cloud path (explicit engine_type=local, no consent)
    EngineFailed,        // selected engine (local without fallback, or cloud) returned
                         // empty after all allowed attempts (network/decode failure)
};

enum class EngineType {
    // Auto (REQ-R02 policy, documented semantics restored): local llama if the model
    // exists on disk; otherwise, AND when a local inference attempt fails, the cloud
    // Google Translate fallback is used even if cloud_fallback_enabled is false.
    // Choosing engine_type=auto IS the consent to the documented "seamless Google
    // Translate fallback" contract below; cloud_fallback_enabled no longer gates it.
    Auto,
    GoogleTranslate,// 100% Free, zero-install, zero-API-key Google Translate via native WinHTTP
    // Strict local: typed text stays on-device. Cloud is used ONLY as fallback after
    // a local failure when the user explicitly enabled cloud_fallback_enabled (H2 gate).
    LocalLlama      // Local GGUF model via llama.cpp (CUDA 13.3 / CPU fallback)
};

class TranslationManager {
public:
    explicit TranslationManager(
        EngineType preferred_type = EngineType::Auto,
        std::string model_path = ""
    );
    ~TranslationManager();

    // Sets preferred engine type and refreshes active engine state
    void SetEngineType(EngineType type);
    EngineType GetEngineType() const;

    // Sets model file path and verifies existence on disk
    void SetModelPath(std::string_view path);
    std::string GetModelPath() const;

    // Returns user-facing name of the currently active engine
    std::string GetActiveEngineName() const;

    // Returns true if local GGUF model file exists on disk
    bool IsLocalModelAvailable() const;

    // Privacy consent gate (H2 fix, REQ-R02 policy update): when false, post-local-
    // failure cloud fallback is refused for an explicit engine_type=local choice
    // (strict on-device semantics preserved). It does NOT gate engine_type=auto:
    // Auto's documented contract ("seamless Google Translate fallback") is the
    // consent. An explicit engine_type=google choice always uses the cloud.
    // Whenever no translation is produced, Translate() reports why via
    // TranslationStatus instead of failing silently.
    void SetCloudFallbackEnabled(bool enabled);
    bool IsCloudFallbackEnabled() const;

    // Translates input text from source language to target language.
    // Thread-safe: internal mutex guards concurrent requests.
    // The 3-arg form is kept for existing callers; it discards the status.
    std::wstring Translate(
        std::wstring_view text,
        std::string_view src_code_or_name,
        std::string_view tgt_code_or_name
    );

    // REQ-R02: status-aware form. When the returned string is empty, *out_status
    // (if non-null) tells the caller WHY (privacy gate vs. engine failure) so the
    // worker can surface audible/visible feedback. When non-empty, status is Ok.
    std::wstring Translate(
        std::wstring_view text,
        std::string_view src_code_or_name,
        std::string_view tgt_code_or_name,
        TranslationStatus* out_status
    );

    // Preloads local GGUF model into memory/VRAM. Returns true on success.
    bool PreloadLocalModel();

    // Releases local model from memory/VRAM.
    void UnloadLocalModel();

    // Sets sampling parameters for local LLM generation
    void SetSamplingParams(float temp, float top_p, int top_k, float rep_pen);
    float GetTemperature() const;
    float GetTopP() const;
    int GetTopK() const;
    float GetRepetitionPenalty() const;

private:
    void RefreshActiveEngine();

    mutable std::mutex mutex_;
    EngineType preferred_type_ = EngineType::Auto;
    std::string model_path_;
    EngineType active_type_ = EngineType::GoogleTranslate;
    std::string active_name_ = "Google Translate";
    bool local_model_available_ = false;
    bool cloud_fallback_enabled_ = false;

    float temperature_ = 0.7f;
    float top_p_ = 0.6f;
    int top_k_ = 20;
    float repetition_penalty_ = 1.05f;

    struct LlamaEngine;
    std::unique_ptr<LlamaEngine> llama_engine_;
};

} // namespace emebalachat
