#include "engine.hpp"
#include "diag_logger.hpp"
#include "config.hpp"
#include "google_translate.hpp"
#include "unicode_utils.hpp"

#include <algorithm>
#include <chrono>
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

// I5 fix (was hardcoded 64 at the sampler-chain call site): repetition-penalty
// look-back window in tokens, per Tencent Hy-MT2 lab specification. Changing
// the tuning requires no other edit; behavior is identical to the previous
// literal 64.
constexpr int kPenaltyLastN = 64;
// I5 proof: compile-time assertion pins the lab-spec value (the constant lives
// in this TU's anonymous namespace, so tests/run_tests.cpp cannot reference it
// directly; a wrong value now fails the build instead of drifting silently).
static_assert(kPenaltyLastN == 64, "Hy-MT2 lab spec: repetition penalty last-N window is 64 tokens");

// REQ-R01 proof: the llama context budget must agree with the values EnsureLoaded
// configures and the arithmetic the unit tests rely on. Wrong values fail the build.
static_assert(kLlamaNCtx == 2048, "REQ-R01: n_ctx is 2048 (EnsureLoaded must configure the same)");
static_assert(kLlamaGenReserve == 512, "REQ-R01: generation reserve equals max_gen_tokens");
static_assert(kLlamaPromptTokenBudget == kLlamaNCtx - kLlamaGenReserve - kLlamaTokenSafetyMargin,
              "REQ-R01: prompt budget = n_ctx - gen reserve - safety margin");

// R6 Phase 4 (B2, architect plan §4.2(b)): opt-in local-prompt observability.
// The user-machine JA->ZH confirmation needs proof of the EXACT prompt sent to
// Hy-MT2 (the 040 line only names the routed target). Launch Emebala_chat.exe
// with EMEBALA_DEBUG_PROMPT=1 in the environment to log every built local
// prompt (first 240 bytes) to stderr. Off by default: the prompt embeds user
// text, so capture is strictly user-initiated and stays on local stderr.
// Cached in a function-local static (thread-safe init since C++11): read once,
// never races with SetEnvironmentVariable mid-run.
bool DebugPromptEnabled() {
    static const bool enabled = [] {
        wchar_t buf[8] = {0};
        const DWORD n = ::GetEnvironmentVariableW(L"EMEBALA_DEBUG_PROMPT", buf, 8);
        return n > 0 && n < 8;
    }();
    return enabled;
}
static_assert(kLlamaPromptTokenBudget == 1520, "REQ-R01: prompt token budget is 1520");

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

// REQ-R01 (Batch D1): pure, model-independent head+tail sliding-window truncation.
// Keeps the first and last `keep_per_side` UTF-16 code units joined by "\n...\n"
// (U+2026 ellipsis). Cut points are adjusted so a UTF-16 surrogate pair is never
// split - a lone surrogate would corrupt the UTF-8 conversion and the tokenizer.
// Returns the text unchanged when it already fits (text.size() <= 2*keep_per_side).
// Unit-testable without any model; the llama path drives it with a bounded
// proportional shrink loop (see LlamaEngine::Translate) so llama_decode NEVER
// receives a prompt larger than the context window (audit §2.1 root cause).
std::wstring TruncateHeadTailWindow(std::wstring_view text, size_t keep_per_side) {
    if (text.empty()) {
        return {};
    }
    if (text.size() <= 2 * keep_per_side) {
        return std::wstring(text);
    }

    // UTF-16 surrogate ranges (wchar_t is UTF-16 on Windows).
    auto is_high_surrogate = [](wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; };
    auto is_low_surrogate = [](wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; };

    size_t head = keep_per_side;
    // If the last retained head unit is a HIGH surrogate, its low partner falls
    // into the cut region - drop the high surrogate instead of emitting a lone one.
    if (head > 0 && is_high_surrogate(text[head - 1])) {
        --head;
    }

    size_t tail_start = text.size() - keep_per_side;
    // If the first retained tail unit is a LOW surrogate, its high partner was
    // cut away - shift the tail start inward past the orphaned low surrogate.
    // keep_per_side == 0 (degenerate: both sides empty) must not index text[size()].
    if (keep_per_side > 0 && is_low_surrogate(text[tail_start])) {
        ++tail_start;
    }

    static const std::wstring kEllipsisMarker = L"\n\u2026\n";
    std::wstring out;
    out.reserve(head + kEllipsisMarker.size() + (text.size() - tail_start));
    out.append(text, 0, head);
    out.append(kEllipsisMarker);
    out.append(text, tail_start, text.size() - tail_start);
    return out;
}

// M3 (security): fail-closed validation of a GGUF model path before it is handed
// to the llama.cpp loader. Rejections log an ENGINE/IsValidModelPath/NNN code to
// stderr (never to a persisted log). The check is purely lexical + regular-file
// based, so it is unit-testable without loading any model.
bool IsValidModelPath(std::string_view path, std::string_view base_dir) {
    if (path.empty()) {
        DIAG_F("ENGINE/IsValidModelPath/001: empty model path rejected\n");
        return false;
    }

    const std::filesystem::path p{std::string(path)};

    // Extension must be exactly ".gguf" (case-insensitive) so the GGUF parser
    // never touches arbitrary files chosen via a tampered config.json.
    if (LowerAscii(p.extension().string()) != ".gguf") {
        DIAG_F("ENGINE/IsValidModelPath/003: non-.gguf model path rejected: %s\n",
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
                DIAG_F("ENGINE/IsValidModelPath/004: cannot resolve base directory; relative model path rejected: %s\n",
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
            DIAG_F("ENGINE/IsValidModelPath/004: relative model path escapes base directory via '..' (path traversal) rejected: %s\n",
                    std::string(path).c_str());
            return false;
        }
        target = joined;
    }

    // Must exist as a regular file (not a directory, device, or missing entry).
    std::error_code ec;
    if (!std::filesystem::is_regular_file(target, ec) || ec) {
        DIAG_F("ENGINE/IsValidModelPath/002: model file does not exist or is not a regular file: %s\n",
                std::string(path).c_str());
        return false;
    }

    return true;
}

#ifdef HAVE_LLAMA_CPP

namespace {

// REQ-R16 (audit §5 latent item 4): llama.cpp abort callback. ggml calls this
// between tensor-evaluation chunks of an in-flight llama_decode(); returning
// true aborts the compute. user_data is the TranslationManager's
// cancel_requested_ atomic (registered once at engine creation, lives as long
// as the manager which owns this engine). Documented llama.cpp limitation:
// CPU execution only - the per-token stop check in the decode loop below
// covers the GPU path (one forward pass per token, milliseconds each).
bool LlamaAbortIfCanceled(void* user_data) {
    if (!user_data) {
        return false;
    }
    const auto* flag = static_cast<const std::atomic<bool>*>(user_data);
    return flag->load(std::memory_order_acquire);
}

// REQ-R16: model-load cancellation. llama_progress_callback returns TRUE to
// CONTINUE loading; returning false aborts llama_model_load_from_file(). This
// unwinds the startup warmup thread (PreloadLocalModel) during app exit
// instead of the shutdown path waiting out a multi-second VRAM load - the
// other half of the "no zombie thread at exit" requirement.
bool LlamaLoadProgress(float /*progress*/, void* user_data) {
    return !LlamaAbortIfCanceled(user_data);
}

} // namespace

struct TranslationManager::LlamaEngine {
    llama_model* model = nullptr;
    llama_context* ctx = nullptr;
    const llama_vocab* vocab = nullptr;
    std::string loaded_path;
    // REQ-R16: cancellation flag owned by TranslationManager (null when the
    // engine was created without a manager, e.g. in isolation tests - then
    // cancellation is simply unavailable and behavior is the old full-run).
    const std::atomic<bool>* cancel_flag = nullptr;

    bool CancelRequested() const {
        return cancel_flag && cancel_flag->load(std::memory_order_acquire);
    }

    LlamaEngine() {
        llama_log_set([](ggml_log_level level, const char* text, void* /*user_data*/) {
            if (level >= GGML_LOG_LEVEL_WARN) {
                DIAG_F("%s", text);
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

        // REQ-R16: if a shutdown cancellation was requested while we were
        // queued behind mutex_, do not even start a model load.
        if (CancelRequested()) {
            DIAG_F("ENGINE/EnsureLoaded/030: load skipped, shutdown cancellation pending\n");
            return false;
        }

        llama_model_params mparams = llama_model_default_params();
        mparams.n_gpu_layers = 99; // Offload layers to RTX 2070 Turing GPU (sm_75)
        // REQ-R16: abort an in-progress model load when shutdown is requested.
        mparams.progress_callback = LlamaLoadProgress;
        mparams.progress_callback_user_data =
            const_cast<void*>(static_cast<const void*>(cancel_flag));

        model = llama_model_load_from_file(path.c_str(), mparams);
        if (!model) {
            // REQ-R16: a cancel-aborted load is not a CUDA failure; do not
            // spend another full load attempt on the CPU path afterwards.
            if (CancelRequested()) {
                DIAG_F("ENGINE/EnsureLoaded/031: model load aborted by shutdown cancellation\n");
                return false;
            }
            // Fallback to CPU-only load if CUDA load encounters an issue
            mparams.n_gpu_layers = 0;
            model = llama_model_load_from_file(path.c_str(), mparams);
            if (!model) {
                return false;
            }
        }

        vocab = llama_model_get_vocab(model);

        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx = kLlamaNCtx;             // REQ-R01: budget constants shared with Translate()
        cparams.n_batch = kLlamaNCtx;
        cparams.n_ubatch = 512;
        unsigned int hw_threads = std::thread::hardware_concurrency();
        cparams.n_threads = hw_threads > 0 ? static_cast<int32_t>(hw_threads) : 4;
        cparams.n_threads_batch = cparams.n_threads;
        cparams.flash_attn = true;
        // REQ-R16: per-chunk abort inside llama_decode (CPU execution path).
        cparams.abort_callback = LlamaAbortIfCanceled;
        cparams.abort_callback_data =
            const_cast<void*>(static_cast<const void*>(cancel_flag));

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

    // R6 Phase 4 (B2, plan §4.1 item 2): src_name is the resolved SOURCE token
    // for the prompt hint (""/AUTO = no hint, historical behavior).
    std::wstring Translate(
        std::wstring_view text,
        std::string_view tgt_name,
        std::string_view src_name,
        const std::string& path,
        float temperature = 0.7f,
        float top_p = 0.6f,
        int top_k = 20,
        float rep_pen = 1.05f
    ) {
        // REQ-R16: a canceled manager short-circuits before touching llama.
        if (CancelRequested()) {
            return {};
        }
        if (!EnsureLoaded(path)) {
            return {};
        }

        // Tencent Hy-MT2 instruction format + optional GGUF chat template. Both are
        // rebuilt inside the REQ-R01 shrink loop, so they live in one lambda.
        const char* chat_tmpl = llama_model_chat_template(model, nullptr);
        auto build_final_prompt = [&](std::wstring_view s) -> std::string {
            std::string u8 = ToUtf8(s);
            if (u8.empty()) {
                return {};
            }
            std::string p = BuildPrompt(u8, tgt_name, src_name);
            if (chat_tmpl) {
                llama_chat_message msg{"user", p.c_str()};
                int32_t needed = llama_chat_apply_template(chat_tmpl, &msg, 1, true, nullptr, 0);
                if (needed > 0) {
                    std::vector<char> formatted(needed + 1);
                    int32_t written = llama_chat_apply_template(chat_tmpl, &msg, 1, true, formatted.data(), static_cast<int32_t>(formatted.size()));
                    if (written > 0) {
                        p.assign(formatted.data(), written);
                    }
                }
            }
            if (DebugPromptEnabled()) {
                DIAG_F(
                        "ENGINE/BuildPrompt/050: local prompt target=\"%.*s\" source=\"%.*s\" bytes=%zu:\n%.240s\n---\n",
                        static_cast<int>(tgt_name.size()), tgt_name.data(),
                        static_cast<int>(src_name.size()), src_name.data(),
                        p.size(), p.c_str());
            }
            return p;
        };

        // Tokenize helper (REQ-R01): fills `out` and returns the token count, or -1
        // on tokenizer failure. Allocation mirrors the original probe-then-size.
        auto tokenize_prompt = [&](const std::string& p, std::vector<llama_token>& out) -> int32_t {
            if (p.empty()) {
                return -1;
            }
            int32_t n_alloc = -llama_tokenize(vocab, p.c_str(), static_cast<int32_t>(p.size()), nullptr, 0, true, true);
            if (n_alloc <= 0) {
                n_alloc = static_cast<int32_t>(p.size()) + kLlamaTokenSafetyMargin;
            }
            out.resize(static_cast<size_t>(n_alloc) + static_cast<size_t>(kLlamaTokenSafetyMargin));
            int32_t n = llama_tokenize(
                vocab,
                p.c_str(),
                static_cast<int32_t>(p.size()),
                out.data(),
                static_cast<int32_t>(out.size()),
                true,
                true
            );
            if (n <= 0) {
                return -1;
            }
            out.resize(static_cast<size_t>(n));
            return n;
        };

        std::wstring src_w(text);
        std::string prompt = build_final_prompt(src_w);
        std::vector<llama_token> prompt_tokens;
        int32_t n_prompt_tokens = tokenize_prompt(prompt, prompt_tokens);
        if (n_prompt_tokens < 0) {
            return {};
        }

        // REQ-R01 (audit §2.1): count prompt tokens BEFORE llama_decode. When they
        // exceed the budget (n_ctx - generation reserve - safety margin), shrink
        // the SOURCE text with a head+tail sliding window and re-tokenize. Each
        // iteration targets a proportional size minus 25% headroom, so the loop
        // makes geometric progress and terminates quickly even when the character
        // -> token compression ratio differs between iterations.
        if (n_prompt_tokens > kLlamaPromptTokenBudget) {
            const int32_t overflow_n = n_prompt_tokens;
            int shrink_iters = 0;
            while (n_prompt_tokens > kLlamaPromptTokenBudget && shrink_iters < 16 && src_w.size() > 64) {
                const double ratio = static_cast<double>(kLlamaPromptTokenBudget) / static_cast<double>(n_prompt_tokens);
                size_t target_len = static_cast<size_t>(static_cast<double>(src_w.size()) * ratio * 0.75);
                if (target_len < 64) {
                    target_len = 64;
                }
                if (target_len >= src_w.size()) {
                    target_len = src_w.size() - 1; // shrink at least one unit per iteration
                }
                src_w = TruncateHeadTailWindow(src_w, target_len / 2);
                prompt = build_final_prompt(src_w);
                const int32_t n2 = tokenize_prompt(prompt, prompt_tokens);
                if (n2 < 0) {
                    DIAG_F("ENGINE/Translate/013: tokenizer rejected the truncated prompt\n");
                    return {};
                }
                n_prompt_tokens = n2;
                ++shrink_iters;
            }
            DIAG_F("ENGINE/Translate/010: prompt tokens %d exceeded budget %d; source truncated to %zu UTF-16 units -> %d tokens after %d shrink iterations\n",
                    overflow_n, kLlamaPromptTokenBudget, src_w.size(), n_prompt_tokens, shrink_iters);
        }

        // Last-resort hard cap: if the shrink loop still could not reach the budget
        // (pathological template overhead or iteration cap), clip the TOKEN vector
        // head+tail so llama_decode can never fail on length. Preserves the BOS +
        // instruction prefix (head) and the sentence-final tokens (tail).
        if (n_prompt_tokens > kLlamaPromptTokenBudget) {
            const size_t head_n = static_cast<size_t>(kLlamaPromptTokenBudget) / 2;
            const size_t tail_n = static_cast<size_t>(kLlamaPromptTokenBudget) - head_n;
            std::vector<llama_token> kept;
            kept.reserve(prompt_tokens.size());
            kept.insert(kept.end(), prompt_tokens.begin(), prompt_tokens.begin() + head_n);
            kept.insert(kept.end(), prompt_tokens.end() - tail_n, prompt_tokens.end());
            prompt_tokens.swap(kept);
            n_prompt_tokens = static_cast<int32_t>(prompt_tokens.size());
            DIAG_F("ENGINE/Translate/012: hard token-window cap applied, prompt clipped to %d tokens\n", n_prompt_tokens);
        }

        // The post-generation quote-strip heuristic below compares against the
        // ORIGINAL source quoting; recompute UTF-8 from the (possibly truncated)
        // working copy so the comparison reflects what was actually sent.
        std::string src_u8 = ToUtf8(src_w);

        // Clear KV memory for clean inference sequence
        llama_memory_clear(llama_get_memory(ctx), true);

        // Process prompt tokens
        llama_batch batch = llama_batch_get_one(prompt_tokens.data(), static_cast<int32_t>(prompt_tokens.size()));
        if (llama_decode(ctx, batch) != 0) {
            DIAG_F("ENGINE/Translate/011: llama_decode failed (%d prompt tokens, budget %d)\n",
                    n_prompt_tokens, kLlamaPromptTokenBudget);
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
                llama_sampler_chain_add(smpl, llama_sampler_init_penalties(kPenaltyLastN, rep_pen, 0.0f, 0.0f));
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
        constexpr int max_gen_tokens = kLlamaGenReserve; // REQ-R01: reserve mirrored from the budget constant

        for (int i = 0; i < max_gen_tokens; ++i) {
            // REQ-R16: cancellation check BETWEEN DECODE STEPS - the token
            // loop is the app-level seam: shutdown posts the flag, and at
            // most one more sampled token (+ its batched decode, the
            // abort_callback unwinds that from inside) runs before we exit
            // the loop with the partial output discarded as empty.
            if (CancelRequested()) {
                DIAG_F("ENGINE/Translate/032: local decode canceled at token %d (shutdown)\n", i);
                llama_sampler_free(smpl);
                return {};
            }

            // Guard against context overflow (REQ-R01: constant now shared with the
            // budget computed before decode, instead of a second hardcoded 2048).
            if (static_cast<int>(prompt_tokens.size()) + i + 1 >= kLlamaNCtx) {
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
    const std::atomic<bool>* cancel_flag = nullptr;
    bool CancelRequested() const { return false; }
    bool EnsureLoaded(const std::string&) { return false; }
    void Unload() {}
    std::wstring Translate(std::wstring_view, std::string_view, const std::string&, float = 0.7f, float = 0.6f, int = 20, float = 1.05f) { return {}; }
};

#endif

// I3 fix: the constructor no longer performs its own config.json disk load.
// Previously it created a shadow AppConfig and re-read the file, so every start
// parsed the config twice and a locked/malformed file could give the engine
// different values than the caller (single-source-of-truth violation).
// main.cpp owns the one LoadFromFile() call and pushes the loaded values in via
// the existing public setters (SetSamplingParams / SetCloudFallbackEnabled)
// AFTER construction; the constructor here keeps pure in-class defaults
// (0.7 / 0.6 / 20 / 1.05 - identical to AppConfig's defaults) so a
// stand-alone constructed manager still behaves as before for tests.
TranslationManager::TranslationManager(EngineType preferred_type, std::string model_path)
    : preferred_type_(preferred_type), model_path_(std::move(model_path)) {
    if (model_path_.empty()) {
        // D5 item F (hardens the D4-flagged issue 1): the in-class AppConfig
        // default "models/Hy-MT2-1.8B-Q8_0.gguf" is RELATIVE, and the old
        // fallback left it resolved against the process CWD by downstream
        // checks (std::filesystem::exists here, IsValidModelPath's default
        // base, and llama_model_load_from_file inside the engine) - so a
        // direct-construct caller (tests, future embedding) under a foreign
        // CWD (Run-registry autostart -> System32) silently lost the local
        // model. ResolveModelPath anchors the default to the EXECUTABLE
        // directory: pure path arithmetic, still no file I/O in this
        // constructor, and identical semantics to the main.cpp REQ-R11
        // handoff. Explicit absolute model_path arguments keep their
        // pass-through behavior (ResolveModelPath ignores base for them,
        // but we only touch the empty-default branch anyway).
        model_path_ = ResolveModelPath(AppConfig{}.model_path);
    }
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
    EnsureLlamaEngineLocked();
    return llama_engine_->EnsureLoaded(model_path_);
}

// REQ-R16: single creation point wiring the cancellation flag into the llama
// engine (caller holds mutex_). The engine reads cancel_requested_ lock-free
// between decode steps and llama.cpp reads it from the abort callback.
void TranslationManager::EnsureLlamaEngineLocked() {
    if (!llama_engine_) {
        llama_engine_ = std::make_unique<LlamaEngine>();
        llama_engine_->cancel_flag = &cancel_requested_;
    }
}

void TranslationManager::RequestCancel() {
    // Deliberately NOT taking mutex_: the inference thread holds it across the
    // whole decode; the atomic store is the signal that thread observes.
    cancel_requested_.store(true, std::memory_order_release);
}

bool TranslationManager::IsCancelRequested() {
    return cancel_requested_.load(std::memory_order_acquire);
}

// REQ-R16 bounded drain: Translate() (and the Auto->cloud fallback) hold mutex_
// for their whole duration, so acquiring it proves no inference is in flight.
// try_lock polling gives a wall-clock-bounded wait for the shutdown sequence.
bool TranslationManager::WaitInferenceIdle(DWORD timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        std::unique_lock<std::mutex> probe(mutex_, std::try_to_lock);
        if (probe.owns_lock()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
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
                EnsureLlamaEngineLocked();
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
                EnsureLlamaEngineLocked();
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

// R6 Phase 4 (B2, architect plan §4.1 item 3): pure routing seam - the header
// (src/engine.hpp) carries the full contract. No state, no I/O: the whole
// (source x target x engine x consent) matrix is pinned headlessly by
// TestR6P4LanguageRouting, so the shipped decision can never drift from the
// tested one.
EngineType PlanTranslationRouting(std::string_view src_code,
                                  std::string_view tgt_code,
                                  EngineType engine_type,
                                  bool google_consent) {
    if (engine_type == EngineType::GoogleTranslate) {
        return EngineType::GoogleTranslate; // deliberate cloud pick always wins
    }
    if (LocalPairReliable(src_code, tgt_code)) {
        return EngineType::LocalLlama; // supported pair: the pin is honored
    }
    // Unsupported pair for Hy-MT2 (user bug: JA -> ZH-CN degraded to English).
    if (engine_type == EngineType::Auto) {
        // REQ-R02 Auto contract: choosing auto IS the consent to the seamless
        // cloud fallback, so google_consent does not gate this path (plan §4.1).
        return EngineType::GoogleTranslate;
    }
    // Explicit LocalLlama pin + unsupported pair: route to cloud ONLY with the
    // user's explicit cloud consent; without it the strict on-device semantics
    // win and the request stays local (Translate logs the degradation warning).
    return google_consent ? EngineType::GoogleTranslate : EngineType::LocalLlama;
}

// 3-arg compatibility form for existing callers (main.cpp tooltip/drag paths).
// Discards the REQ-R02 status; callers that must react to failure use the
// status-aware overload below.
std::wstring TranslationManager::Translate(
    std::wstring_view text,
    std::string_view src_code_or_name,
    std::string_view tgt_code_or_name
) {
    return Translate(text, src_code_or_name, tgt_code_or_name, nullptr);
}

// REQ-R02 (Batch D1, audit §2.1 / §5-C4): the silent bare-{} failure is gone.
// Every path that produces no translation now reports a TranslationStatus the
// worker can react to (error tone / tooltip), and the Auto policy is restored
// to its documented semantics.
//
// What Auto means NOW (documented per task directive):
//   engine_type=auto = "use the local Hy-MT2 model when available; seamlessly
//   fall back to Google Translate when the model is absent OR a local inference
//   attempt fails". Selecting auto in config IS the user's consent to that
//   documented cloud fallback, so cloud_fallback_enabled_ does NOT gate the
//   Auto->cloud paths (it never did per the original EngineType::Auto contract;
//   H2 had over-tightened it into a silent-failure regression).
//   engine_type=local stays strictly on-device: after a local failure the cloud
//   is used only when the user explicitly enabled cloud_fallback_enabled_.
std::wstring TranslationManager::Translate(
    std::wstring_view text,
    std::string_view src_code_or_name,
    std::string_view tgt_code_or_name,
    TranslationStatus* out_status
) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto set_status = [&](TranslationStatus s) {
        if (out_status) {
            *out_status = s;
        }
    };

    if (text.empty()) {
        set_status(TranslationStatus::InputEmpty);
        return {};
    }

    std::string norm_tgt = NormalizeLanguageCode(tgt_code_or_name);
    const auto* pTgt = FindLanguageByCode(norm_tgt);
    std::string tgt_name = pTgt ? pTgt->name_en : std::string(tgt_code_or_name);
    const std::string norm_src = NormalizeLanguageCode(src_code_or_name);

    // R6 Phase 4 (B2, plan §4.1 item 3): pair routing, decided HERE (before the
    // 040 line) so the observability log reports the engine that will ACTUALLY
    // serve the request. Meaningful only while the local engine is active;
    // model availability itself stays RefreshActiveEngine's job.
    EngineType served_engine = active_type_;
    if (active_type_ == EngineType::LocalLlama) {
        served_engine = PlanTranslationRouting(src_code_or_name, tgt_code_or_name,
                                               preferred_type_, cloud_fallback_enabled_);
    }

    // R5 observability (R6-p4: now also names the resolved source and reflects
    // the pair-routing decision) so a "didn't translate to the switched target"
    // can be attributed (engine routing vs upstream).
    DIAG_F(
            "ENGINE/Translate/040: target routed (src=\"%.*s\" -> norm=%s, input=\"%.*s\" -> norm=%s name=%s, engine=%s)\n",
            static_cast<int>(src_code_or_name.size()), src_code_or_name.data(),
            norm_src.c_str(),
            static_cast<int>(tgt_code_or_name.size()), tgt_code_or_name.data(),
            norm_tgt.c_str(), tgt_name.c_str(),
            served_engine == EngineType::LocalLlama ? "local" : "cloud");

    // Single cloud seam: records EngineFailed when the request itself produced
    // nothing (network error, 403, malformed response), Ok otherwise.
    auto cloud_call = [&]() -> std::wstring {
        std::wstring res = GoogleTranslate::Translate(text, src_code_or_name, tgt_code_or_name);
        set_status(res.empty() ? TranslationStatus::EngineFailed : TranslationStatus::Ok);
        return res;
    };

    if (active_type_ == EngineType::LocalLlama && llama_engine_) {
        // REQ-R16: cancellation short-circuit BEFORE llama is touched (a
        // shutdown posted RequestCancel() while this call queued on mutex_).
        if (cancel_requested_.load(std::memory_order_acquire)) {
            set_status(TranslationStatus::Canceled);
            return {};
        }
        // R6 Phase 4 (B2, plan §4.1 item 3): unsupported pair policy BEFORE
        // llama. An outside-the-reliable-set pair never wastes an inference
        // that degrades to English output; under the Auto consent (or an
        // explicit cloud fallback consent) it goes straight to Google.
        if (served_engine != EngineType::LocalLlama) {
            DIAG_F(
                    "ENGINE/Translate/041: pair (%s -> %s) outside the Hy-MT2 reliable set; routed to cloud (engine_type=%s, cloud_consent=%d)\n",
                    norm_src.c_str(), norm_tgt.c_str(),
                    preferred_type_ == EngineType::Auto ? "auto" : "local",
                    cloud_fallback_enabled_ ? 1 : 0);
            return cloud_call();
        }
        if (preferred_type_ == EngineType::LocalLlama &&
            !LocalPairReliable(src_code_or_name, tgt_code_or_name)) {
            // Plan §4.1: explicit local pin + no cloud consent keeps the
            // request on-device; warn that the output language may degrade.
            DIAG_F(
                    "ENGINE/Translate/042: pair (%s -> %s) outside the Hy-MT2 reliable set but strict-local pin without cloud consent; staying local (output may degrade)\n",
                    norm_src.c_str(), norm_tgt.c_str());
        }
        std::wstring res = llama_engine_->Translate(
            text, tgt_name, src_code_or_name, model_path_,
            temperature_, top_p_, top_k_, repetition_penalty_
        );
        if (!res.empty()) {
            set_status(TranslationStatus::Ok);
            return res;
        }
        // REQ-R16: an empty result right after a cancel request is the
        // shutdown unwind, NOT an engine failure. It must not fall through to
        // the cloud (would transmit user text mid-exit) and must not surface
        // a spurious error tone.
        if (cancel_requested_.load(std::memory_order_acquire)) {
            set_status(TranslationStatus::Canceled);
            return {};
        }
        // Local inference failed (load/decode/tokenizer/empty output).
        // Auto: seamless cloud fallback - this is the consented contract above.
        if (preferred_type_ == EngineType::Auto) {
            DIAG_F("ENGINE/Translate/020: local inference failed, Auto policy falling back to cloud\n");
            return cloud_call();
        }
        // Explicit local: H2 privacy gate still decides.
        if (cloud_fallback_enabled_) {
            DIAG_F("ENGINE/Translate/021: local inference failed, explicit cloud fallback consent granted\n");
            return cloud_call();
        }
        DIAG_F("ENGINE/Translate/022: local inference failed and cloud fallback consent disabled; surfacing failure to caller\n");
        set_status(TranslationStatus::CloudConsentBlocked);
        return {};
    }

    // Non-local active engine. REQ-R16: shutdown cancellation also latches
    // the pure cloud path - the caller already decided the process is
    // exiting; starting a WinHTTP request now could never deliver its
    // result and would transmit user text after an exit intent.
    if (cancel_requested_.load(std::memory_order_acquire)) {
        set_status(TranslationStatus::Canceled);
        return {};
    }
    // Cloud paths in order of consent strength:
    //  - preferred_type_ == GoogleTranslate: deliberate choice -> always allow.
    //  - preferred_type_ == Auto: zero-install or model-not-found path -> the
    //    Auto contract above makes this consented (REQ-R02 policy).
    //  - preferred_type_ == LocalLlama with no local model: implicit cloud path ->
    //    respect the H2 gate; empty+CloudConsentBlocked when disabled (strict
    //    on-device semantics: text must never leave the device silently).
    if (preferred_type_ == EngineType::GoogleTranslate ||
        preferred_type_ == EngineType::Auto ||
        cloud_fallback_enabled_) {
        return cloud_call();
    }
    set_status(TranslationStatus::CloudConsentBlocked);
    return {};
}

} // namespace emebalachat
