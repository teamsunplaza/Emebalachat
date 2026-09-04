#include "engine.hpp"
#include "config.hpp"
#include "google_translate.hpp"
#include "unicode_utils.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#if defined(HAVE_LLAMA_CPP) || __has_include("llama.h")
#ifndef HAVE_LLAMA_CPP
#define HAVE_LLAMA_CPP 1
#endif
#include "llama.h"
#endif

namespace emebalachat {

namespace {

// Lower-case ASCII characters for case-insensitive path comparisons (Windows
// paths are case-insensitive; this project targets Windows only).
std::string LowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

} // namespace

// M3 (security): fail-closed validation of a GGUF model path before it is handed
// to the llama.cpp loader. Rejections log an ENGINE/IsValidModelPath/NNN code to
// stderr (never to a persisted log). The check is purely lexical + regular-file
// based, so it is unit-testable without loading any model.
bool IsValidModelPath(std::string_view path, std::string_view base_dir) {
    if (path.empty()) {
        fprintf(stderr, "ENGINE/IsValidModelPath/001: empty model path rejected\n");
        return false;
    }

    const std::filesystem::path p{std::string(path)};

    // Extension must be exactly ".gguf" (case-insensitive) so the GGUF parser
    // never touches arbitrary files chosen via a tampered config.json.
    if (LowerAscii(p.extension().string()) != ".gguf") {
        fprintf(stderr, "ENGINE/IsValidModelPath/003: non-.gguf model path rejected: %s\n",
                std::string(path).c_str());
        return false;
    }

    // For relative paths, resolve against base_dir (default: current working
    // directory) and collapse '.'/'..' components BEFORE any filesystem access,
    // then verify the resolved target stayed inside base_dir (path-traversal
    // check). Absolute paths define their own location; containment does not
    // apply to them. Existence is checked on the resolved target so the loader
    // (which resolves against the process cwd, i.e. the default base_dir) and
    // this validation agree on which file is being loaded.
    std::filesystem::path target = p;
    if (p.is_relative()) {
        std::error_code ec;
        std::filesystem::path base;
        if (base_dir.empty()) {
            base = std::filesystem::current_path(ec);
            if (ec) {
                fprintf(stderr, "ENGINE/IsValidModelPath/004: cannot resolve base directory; relative model path rejected: %s\n",
                        std::string(path).c_str());
                return false;
            }
        } else {
            base = std::filesystem::path{std::string(base_dir)};
        }

        std::filesystem::path joined = (base / p).lexically_normal();
        std::filesystem::path norm_base = base.lexically_normal();
        std::string j = LowerAscii(joined.generic_string());
        std::string b = LowerAscii(norm_base.generic_string());
        while (!b.empty() && b.back() == '/') {
            b.pop_back();
        }
        const bool contained =
            (j == b) ||
            (j.size() > b.size() && j.compare(0, b.size(), b) == 0 && j[b.size()] == '/');
        if (!contained) {
            fprintf(stderr, "ENGINE/IsValidModelPath/004: relative model path escapes base directory via '..' (path traversal) rejected: %s\n",
                    std::string(path).c_str());
            return false;
        }
        target = joined;
    }

    // Must exist as a regular file (not a directory, device, or missing entry).
    std::error_code ec;
    if (!std::filesystem::is_regular_file(target, ec) || ec) {
        fprintf(stderr, "ENGINE/IsValidModelPath/002: model file does not exist or is not a regular file: %s\n",
                std::string(path).c_str());
        return false;
    }

    return true;
}

#ifdef HAVE_LLAMA_CPP

struct TranslationManager::LlamaEngine {
    llama_model* model = nullptr;
    llama_context* ctx = nullptr;
    const llama_vocab* vocab = nullptr;
    std::string loaded_path;

    LlamaEngine() {
        llama_log_set([](ggml_log_level level, const char* text, void* /*user_data*/) {
            if (level >= GGML_LOG_LEVEL_WARN) {
                fprintf(stderr, "%s", text);
            }
        }, nullptr);
        llama_backend_init();
    }

    ~LlamaEngine() {
        Unload();
        llama_backend_free();
    }

    void Unload() {
        if (ctx) {
            llama_free(ctx);
            ctx = nullptr;
        }
        if (model) {
            llama_model_free(model);
            model = nullptr;
        }
        vocab = nullptr;
        loaded_path.clear();
    }

    bool EnsureLoaded(const std::string& path) {
        if (model && ctx && loaded_path == path) {
            return true;
        }

        Unload();

        // M3 (security): validate the path (non-empty, regular file, .gguf
        // extension, no '..' traversal for relative paths) BEFORE passing it to
        // the GGUF loader. Fail-closed with an ENGINE/IsValidModelPath/NNN code
        // on stderr; the worker treats a false return like any load failure.
        if (!IsValidModelPath(path)) {
            return false;
        }

        llama_model_params mparams = llama_model_default_params();
        mparams.n_gpu_layers = 99; // Offload layers to RTX 2070 Turing GPU (sm_75)

        model = llama_model_load_from_file(path.c_str(), mparams);
        if (!model) {
            // Fallback to CPU-only load if CUDA load encounters an issue
            mparams.n_gpu_layers = 0;
            model = llama_model_load_from_file(path.c_str(), mparams);
            if (!model) {
                return false;
            }
        }

        vocab = llama_model_get_vocab(model);

        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx = 2048;
        cparams.n_batch = 2048;
        cparams.n_ubatch = 512;
        unsigned int hw_threads = std::thread::hardware_concurrency();
        cparams.n_threads = hw_threads > 0 ? static_cast<int32_t>(hw_threads) : 4;
        cparams.n_threads_batch = cparams.n_threads;
        cparams.flash_attn = true;

        ctx = llama_init_from_model(model, cparams);
        if (!ctx && cparams.flash_attn) {
            cparams.flash_attn = false;
            ctx = llama_init_from_model(model, cparams);
        }
        if (!ctx) {
            llama_model_free(model);
            model = nullptr;
            vocab = nullptr;
            return false;
        }

        loaded_path = path;
        return true;
    }

    std::wstring Translate(
        std::wstring_view text,
        std::string_view tgt_name,
        const std::string& path,
        float temperature = 0.7f,
        float top_p = 0.6f,
        int top_k = 20,
        float rep_pen = 1.05f
    ) {
        if (!EnsureLoaded(path)) {
            return {};
        }

        std::string src_u8 = ToUtf8(text);
        if (src_u8.empty()) {
            return {};
        }

        // Tencent Hy-MT2 instruction format
        std::string prompt = BuildPrompt(src_u8, tgt_name);

        // Check if model GGUF provides a custom chat template
        const char* tmpl = llama_model_chat_template(model, nullptr);
        if (tmpl) {
            llama_chat_message msg{"user", prompt.c_str()};
            int32_t needed = llama_chat_apply_template(tmpl, &msg, 1, true, nullptr, 0);
            if (needed > 0) {
                std::vector<char> formatted(needed + 1);
                int32_t written = llama_chat_apply_template(tmpl, &msg, 1, true, formatted.data(), static_cast<int32_t>(formatted.size()));
                if (written > 0) {
                    prompt.assign(formatted.data(), written);
                }
            }
        }

        // Tokenize prompt
        int32_t n_tokens_alloc = -llama_tokenize(vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()), nullptr, 0, true, true);
        if (n_tokens_alloc <= 0) {
            n_tokens_alloc = static_cast<int32_t>(prompt.size()) + 16;
        }
        std::vector<llama_token> prompt_tokens(n_tokens_alloc);
        int32_t n_prompt_tokens = llama_tokenize(
            vocab,
            prompt.c_str(),
            static_cast<int32_t>(prompt.size()),
            prompt_tokens.data(),
            static_cast<int32_t>(prompt_tokens.size()),
            true,
            true
        );
        if (n_prompt_tokens <= 0) {
            return {};
        }
        prompt_tokens.resize(n_prompt_tokens);

        // Clear KV memory for clean inference sequence
        llama_memory_clear(llama_get_memory(ctx), true);

        // Process prompt tokens
        llama_batch batch = llama_batch_get_one(prompt_tokens.data(), static_cast<int32_t>(prompt_tokens.size()));
        if (llama_decode(ctx, batch) != 0) {
            return {};
        }

        // Initialize sampler according to Tencent Hy-MT2 official specifications
        llama_sampler* smpl = nullptr;
        if (temperature <= 0.001f) {
            smpl = llama_sampler_init_greedy();
        } else {
            llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
            smpl = llama_sampler_chain_init(sparams);
            if (rep_pen > 1.0f) {
                llama_sampler_chain_add(smpl, llama_sampler_init_penalties(64, rep_pen, 0.0f, 0.0f));
            }
            if (top_k > 0) {
                llama_sampler_chain_add(smpl, llama_sampler_init_top_k(top_k));
            }
            if (top_p > 0.0f && top_p < 1.0f) {
                llama_sampler_chain_add(smpl, llama_sampler_init_top_p(top_p, 1));
            }
            llama_sampler_chain_add(smpl, llama_sampler_init_temp(temperature));
            llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
        }
        if (!smpl) {
            return {};
        }

        std::string output_u8;
        constexpr int max_gen_tokens = 512;

        for (int i = 0; i < max_gen_tokens; ++i) {
            // Guard against context overflow
            if (static_cast<int>(prompt_tokens.size()) + i + 1 >= 2048) {
                break;
            }

            llama_token token = llama_sampler_sample(smpl, ctx, -1);
            llama_sampler_accept(smpl, token);

            if (llama_vocab_is_eog(vocab, token)) {
                break;
            }

            char piece[256] = {};
            int n_piece = llama_token_to_piece(vocab, token, piece, sizeof(piece), 0, false);
            if (n_piece > 0) {
                output_u8.append(piece, n_piece);
            } else if (n_piece < 0) {
                int needed = -n_piece;
                std::vector<char> big_piece(needed);
                int written = llama_token_to_piece(vocab, token, big_piece.data(), needed, 0, false);
                if (written > 0) {
                    output_u8.append(big_piece.data(), written);
                }
            }

            batch = llama_batch_get_one(&token, 1);
            if (llama_decode(ctx, batch) != 0) {
                break;
            }
        }

        llama_sampler_free(smpl);

        // Trim leading and trailing whitespace / newlines
        size_t start = 0;
        while (start < output_u8.size() && (output_u8[start] == ' ' || output_u8[start] == '\n' || output_u8[start] == '\r' || output_u8[start] == '\t')) {
            start++;
        }
        size_t end = output_u8.size();
        while (end > start && (output_u8[end - 1] == ' ' || output_u8[end - 1] == '\n' || output_u8[end - 1] == '\r' || output_u8[end - 1] == '\t')) {
            end--;
        }

        std::string trimmed_u8 = output_u8.substr(start, end - start);

        // Strip matching outer quotes if model wrapped translation in quotes but source text was not quoted
        if (trimmed_u8.size() >= 2 && trimmed_u8.front() == '\"' && trimmed_u8.back() == '\"') {
            if (src_u8.empty() || (src_u8.front() != '\"' && src_u8.back() != '\"')) {
                trimmed_u8 = trimmed_u8.substr(1, trimmed_u8.size() - 2);
            }
        }

        return ToUtf16(trimmed_u8);
    }
};

#else

struct TranslationManager::LlamaEngine {
    bool EnsureLoaded(const std::string&) { return false; }
    void Unload() {}
    std::wstring Translate(std::wstring_view, std::string_view, const std::string&, float = 0.7f, float = 0.6f, int = 20, float = 1.05f) { return {}; }
};

#endif

TranslationManager::TranslationManager(EngineType preferred_type, std::string model_path)
    : preferred_type_(preferred_type), model_path_(std::move(model_path)) {
    AppConfig cfg;
    cfg.LoadFromFile();
    if (model_path_.empty()) {
        model_path_ = cfg.model_path;
    }
    temperature_ = cfg.temperature;
    top_p_ = cfg.top_p;
    top_k_ = cfg.top_k;
    repetition_penalty_ = cfg.repetition_penalty;
    RefreshActiveEngine();
}

TranslationManager::~TranslationManager() = default;

void TranslationManager::SetEngineType(EngineType type) {
    std::lock_guard<std::mutex> lock(mutex_);
    preferred_type_ = type;
    RefreshActiveEngine();
}

EngineType TranslationManager::GetEngineType() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return preferred_type_;
}

void TranslationManager::SetModelPath(std::string_view path) {
    std::lock_guard<std::mutex> lock(mutex_);
    model_path_ = std::string(path);
    RefreshActiveEngine();
}

std::string TranslationManager::GetModelPath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return model_path_;
}

void TranslationManager::SetSamplingParams(float temp, float top_p, int top_k, float rep_pen) {
    std::lock_guard<std::mutex> lock(mutex_);
    temperature_ = temp;
    top_p_ = top_p;
    top_k_ = top_k;
    repetition_penalty_ = rep_pen;
}

float TranslationManager::GetTemperature() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return temperature_;
}

float TranslationManager::GetTopP() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return top_p_;
}

int TranslationManager::GetTopK() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return top_k_;
}

float TranslationManager::GetRepetitionPenalty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return repetition_penalty_;
}

std::string TranslationManager::GetActiveEngineName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_name_;
}

bool TranslationManager::IsLocalModelAvailable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return local_model_available_;
}

void TranslationManager::SetCloudFallbackEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    cloud_fallback_enabled_ = enabled;
}

bool TranslationManager::IsCloudFallbackEnabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cloud_fallback_enabled_;
}

bool TranslationManager::PreloadLocalModel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!llama_engine_) {
        llama_engine_ = std::make_unique<LlamaEngine>();
    }
    return llama_engine_->EnsureLoaded(model_path_);
}

void TranslationManager::UnloadLocalModel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (llama_engine_) {
        llama_engine_->Unload();
    }
}

void TranslationManager::RefreshActiveEngine() {
    std::error_code ec;
    local_model_available_ = !model_path_.empty() && std::filesystem::exists(model_path_, ec);

    if (preferred_type_ == EngineType::GoogleTranslate) {
        active_type_ = EngineType::GoogleTranslate;
        active_name_ = "Google Translate (Cloud)";
    } else if (preferred_type_ == EngineType::LocalLlama) {
        if (local_model_available_) {
#ifdef HAVE_LLAMA_CPP
            active_type_ = EngineType::LocalLlama;
            active_name_ = "Hy-MT2-1.8B (Local)";
            if (!llama_engine_) {
                llama_engine_ = std::make_unique<LlamaEngine>();
            }
#else
            active_type_ = EngineType::GoogleTranslate;
            active_name_ = "Google Translate (Llama Native Not Linked)";
#endif
        } else {
            active_type_ = EngineType::GoogleTranslate;
            active_name_ = "Google Translate (Model Not Found)";
        }
    } else {
        // EngineType::Auto
        if (local_model_available_) {
#ifdef HAVE_LLAMA_CPP
            active_type_ = EngineType::LocalLlama;
            active_name_ = "Hy-MT2-1.8B (CUDA/CPU)";
            if (!llama_engine_) {
                llama_engine_ = std::make_unique<LlamaEngine>();
            }
#else
            active_type_ = EngineType::GoogleTranslate;
            active_name_ = "Google Translate (Cloud Free)";
#endif
        } else {
            active_type_ = EngineType::GoogleTranslate;
            active_name_ = "Google Translate (Zero-Install)";
        }
    }
}

std::wstring TranslationManager::Translate(
    std::wstring_view text,
    std::string_view src_code_or_name,
    std::string_view tgt_code_or_name
) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (text.empty()) {
        return {};
    }

    std::string norm_tgt = NormalizeLanguageCode(tgt_code_or_name);
    const auto* pTgt = FindLanguageByCode(norm_tgt);
    std::string tgt_name = pTgt ? pTgt->name_en : std::string(tgt_code_or_name);

    if (active_type_ == EngineType::LocalLlama && llama_engine_) {
        std::wstring res = llama_engine_->Translate(
            text, tgt_name, model_path_,
            temperature_, top_p_, top_k_, repetition_penalty_
        );
        if (!res.empty()) {
            return res;
        }
        // H2 gate: only fall back to the Google cloud when the user has explicitly
        // consented via cloud_fallback_enabled. Otherwise return empty (the worker
        // handles empty gracefully) so typed text never leaks off-device silently.
        if (cloud_fallback_enabled_) {
            return GoogleTranslate::Translate(text, src_code_or_name, tgt_code_or_name);
        }
        return {};
    }

    // Non-local active engine. Distinguish two cases:
    //  - preferred_type_ == GoogleTranslate: the user DELIBERATELY chose the cloud
    //    engine (explicit consent) -> always allow.
    //  - otherwise (auto/local with no local model available): this is an implicit
    //    cloud path -> respect the H2 consent gate; return empty when disabled.
    if (preferred_type_ == EngineType::GoogleTranslate || cloud_fallback_enabled_) {
        return GoogleTranslate::Translate(text, src_code_or_name, tgt_code_or_name);
    }
    return {};
}

} // namespace emebalachat
