#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace emebalachat {

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

private:
    void RefreshActiveEngine();

    mutable std::mutex mutex_;
    EngineType preferred_type_ = EngineType::Auto;
    std::string model_path_;
    EngineType active_type_ = EngineType::GoogleTranslate;
    std::string active_name_ = "Google Translate";
    bool local_model_available_ = false;

    struct LlamaEngine;
    std::unique_ptr<LlamaEngine> llama_engine_;
};

} // namespace emebalachat
