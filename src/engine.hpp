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

enum class EngineType {
    Auto,           // Auto-detect: Local llama if model exists on disk, otherwise seamless Google Translate fallback
    GoogleTranslate,// 100% Free, zero-install, zero-API-key Google Translate via native WinHTTP
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

    // Privacy consent gate (H2 fix): when false, no text is transmitted to the
    // Google Translate cloud — neither as a post-local-failure fallback nor as
    // the auto engine when no local model exists; Translate() returns empty.
    // An explicit engine_type=google choice still uses the cloud (deliberate).
    void SetCloudFallbackEnabled(bool enabled);
    bool IsCloudFallbackEnabled() const;

    // Translates input text from source language to target language.
    // Thread-safe: internal mutex guards concurrent requests.
    std::wstring Translate(
        std::wstring_view text,
        std::string_view src_code_or_name,
        std::string_view tgt_code_or_name
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
