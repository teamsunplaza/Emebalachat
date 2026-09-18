// tools/fidelity_probe.cpp
// Offline inference probe for Hy-MT2-1.8B-Q8_0 fidelity experiment.
// Reads manifest where each @@@RUN block contains the verbatim prompt.
// Decodes with greedy sampling (temp=0.0) replicating engine.cpp.

#include "llama.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static std::string slurp_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "probe: FATAL cannot read %s\n", path.c_str());
        std::exit(2);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
    return s;
}

static constexpr int kNCtx = 4096;
static constexpr int kGenReserve = 2048;
static constexpr int kSafetyMargin = 16;
static constexpr int kPromptBudget = kNCtx - kGenReserve - kSafetyMargin; // 2032

struct Run {
    std::string id;
    std::string prompt;
};

static std::string trim_ascii(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\n' || s[a] == '\r' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\n' || s[b - 1] == '\r' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

static std::string field_of(const std::string& token, const std::string& key) {
    const std::string prefix = key + "=";
    if (token.rfind(prefix, 0) != 0) return {};
    return token.substr(prefix.size());
}

static std::vector<Run> parse_manifest(const std::string& raw) {
    std::vector<Run> runs;
    std::istringstream iss(raw);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("@@@RUN ", 0) != 0) continue;
        Run r;
        std::istringstream hs(line);
        std::string head;
        hs >> head;
        std::string tok;
        while (hs >> tok) {
            std::string v;
            if (!(v = field_of(tok, "id")).empty()) r.id = v;
        }
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line == "@@@END") break;
            if (!r.prompt.empty()) r.prompt.push_back('\n');
            r.prompt += line;
        }
        runs.push_back(std::move(r));
    }
    return runs;
}

int main(int argc, char** argv) {
    std::string model_path, manifest_path, out_path;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--model") model_path = next("--model");
        else if (a == "--manifest") manifest_path = next("--manifest");
        else if (a == "--out") out_path = next("--out");
        else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return 2;
        }
    }
    if (model_path.empty() || manifest_path.empty() || out_path.empty()) {
        std::fprintf(stderr, "usage: fidelity_probe --model F.gguf --manifest M.txt --out O.txt\n");
        return 2;
    }

    std::vector<Run> runs = parse_manifest(slurp_file(manifest_path));
    if (runs.empty()) {
        std::fprintf(stderr, "manifest contains no @@@RUN records\n");
        return 2;
    }
    std::fprintf(stderr, "probe: %zu runs, model=%s\n", runs.size(), model_path.c_str());

    llama_backend_init();

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 99;
    mparams.split_mode = LLAMA_SPLIT_MODE_NONE;
    mparams.main_gpu = 0;
    mparams.devices = nullptr;
    mparams.tensor_split = nullptr;
    llama_model* model = llama_model_load_from_file(model_path.c_str(), mparams);
    if (!model) {
        std::fprintf(stderr, "probe: GPU load failed, retrying CPU leg\n");
        mparams = llama_model_default_params();
        mparams.n_gpu_layers = 0;
        mparams.split_mode = LLAMA_SPLIT_MODE_LAYER;
        model = llama_model_load_from_file(model_path.c_str(), mparams);
        if (!model) {
            std::fprintf(stderr, "probe: FATAL model load failed\n");
            llama_backend_free();
            return 1;
        }
    }

    const llama_vocab* vocab = llama_model_get_vocab(model);
    const char* chat_tmpl = llama_model_chat_template(model, nullptr);

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = kNCtx;
    cparams.n_batch = kNCtx;
    cparams.n_ubatch = 512;
    unsigned int hw_threads = std::thread::hardware_concurrency();
    cparams.n_threads = hw_threads > 0 ? static_cast<int32_t>(hw_threads) : 4;
    cparams.n_threads_batch = cparams.n_threads;
    cparams.flash_attn = true;
    llama_context* ctx = llama_init_from_model(model, cparams);
    if (!ctx && cparams.flash_attn) {
        cparams.flash_attn = false;
        ctx = llama_init_from_model(model, cparams);
    }
    if (!ctx) {
        std::fprintf(stderr, "probe: FATAL context init failed\n");
        llama_model_free(model);
        llama_backend_free();
        return 1;
    }

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "probe: FATAL cannot open --out %s\n", out_path.c_str());
        llama_free(ctx);
        llama_model_free(model);
        llama_backend_free();
        return 1;
    }

    std::fprintf(stderr, "llama_backend_init done, starting inference\n");

    for (size_t idx = 0; idx < runs.size(); ++idx) {
        const Run& r = runs[idx];
        if (idx % 20 == 0 || idx == runs.size() - 1) {
            std::fprintf(stderr, "probe: progress %zu/%zu (run %s)\n", idx + 1, runs.size(), r.id.c_str());
        }

        std::string prompt = r.prompt;
        if (chat_tmpl) {
            llama_chat_message msg{"user", prompt.c_str()};
            int32_t needed = llama_chat_apply_template(chat_tmpl, &msg, 1, true, nullptr, 0);
            if (needed > 0) {
                std::vector<char> formatted(static_cast<size_t>(needed) + 1);
                int32_t written = llama_chat_apply_template(chat_tmpl, &msg, 1, true,
                                                            formatted.data(), static_cast<int32_t>(formatted.size()));
                if (written > 0) {
                    prompt.assign(formatted.data(), static_cast<size_t>(written));
                }
            }
        }

        std::vector<llama_token> prompt_tokens;
        {
            int32_t n_alloc = -llama_tokenize(vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()),
                                              nullptr, 0, true, true);
            if (n_alloc <= 0) {
                n_alloc = static_cast<int32_t>(prompt.size()) + kSafetyMargin;
            }
            prompt_tokens.resize(static_cast<size_t>(n_alloc) + kSafetyMargin);
            int32_t n = llama_tokenize(vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()),
                                       prompt_tokens.data(), static_cast<int32_t>(prompt_tokens.size()),
                                       true, true);
            if (n <= 0) {
                out << "@@@RESULT id=" << r.id << "\nERROR: tokenizer failed\n@@@END\n";
                continue;
            }
            prompt_tokens.resize(static_cast<size_t>(n));
        }

        if (static_cast<int>(prompt_tokens.size()) > kPromptBudget) {
            out << "@@@RESULT id=" << r.id << "\nBUDGET_EXCEEDED\n@@@END\n";
            continue;
        }

        llama_memory_clear(llama_get_memory(ctx), true);
        llama_batch batch = llama_batch_get_one(prompt_tokens.data(), static_cast<int32_t>(prompt_tokens.size()));
        if (llama_decode(ctx, batch) != 0) {
            out << "@@@RESULT id=" << r.id << "\nERROR: decode failed\n@@@END\n";
            continue;
        }

        llama_sampler* smpl = llama_sampler_init_greedy();

        std::string output_u8;
        int gen_tokens = 0;
        for (int i = 0; i < kGenReserve; ++i) {
            if (static_cast<int>(prompt_tokens.size()) + i + 1 >= kNCtx) {
                break;
            }
            llama_token token = llama_sampler_sample(smpl, ctx, -1);
            llama_sampler_accept(smpl, token);
            if (llama_vocab_is_eog(vocab, token) || token == llama_vocab_eos(vocab)) {
                break;
            }
            ++gen_tokens;
            char piece[256] = {};
            int n_piece = llama_token_to_piece(vocab, token, piece, sizeof(piece), 0, false);
            if (n_piece > 0) {
                output_u8.append(piece, static_cast<size_t>(n_piece));
            } else if (n_piece < 0) {
                int needed = -n_piece;
                std::vector<char> big_piece(static_cast<size_t>(needed));
                int written = llama_token_to_piece(vocab, token, big_piece.data(), needed, 0, false);
                if (written > 0) {
                    output_u8.append(big_piece.data(), static_cast<size_t>(written));
                }
            }
            batch = llama_batch_get_one(&token, 1);
            if (llama_decode(ctx, batch) != 0) {
                break;
            }
        }
        llama_sampler_free(smpl);

        std::string trimmed = trim_ascii(output_u8);
        if (trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"' &&
            !r.prompt.empty() && r.prompt.front() != '"' && r.prompt.back() != '"') {
            trimmed = trimmed.substr(1, trimmed.size() - 2);
        }

        out << "@@@RESULT id=" << r.id << "\n";
        out << trimmed << "\n";
        out << "@@@END\n";
        out.flush();
    }

    out.close();
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    std::fprintf(stderr, "probe: all runs completed successfully\n");
    return 0;
}
