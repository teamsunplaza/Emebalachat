// tools/temp_probe.cpp
// Standalone measurement probe for the Local-translation temperature study
// (session 260913_0002). MEASUREMENT ONLY: it links the same llama.cpp b6099
// static libs the app uses, loads the GGUF directly, and replicates the
// production inference path from src/engine.cpp:
//   * EnsureLoaded context config   (L704-809):  n_ctx 4096, n_batch 4096,
//     n_ubatch 512, hw threads, flash_attn true (false-retry), GPU leg
//     n_gpu_layers=99 + SPLIT_MODE_NONE + main_gpu=0 with the CPU
//     (n_gpu_layers=0 + SPLIT_MODE_LAYER) fallback leg.
//   * BuildPrompt English branch     (src/config.cpp L588-597):  "Translate
//     the following <src_en> segment into <tgt_en>, without additional
//     explanation.\n\n<text>"  + llama_chat_apply_template(add_gen_prompt)
//     (engine.cpp L845-862).
//   * Tokenizer call                  (L893-916): add_special=true,
//     parse_special=true, +16 safety margin.
//   * Sampler chain                  (L1002-1020): temp<=0.001 -> greedy;
//     else penalties(64, rep_pen) -> top_k -> top_p -> temp -> dist.
//     DELIBERATE DIVERGENCE: production dist uses LLAMA_DEFAULT_SEED
//     (random). The probe pins the seed so 0.1 vs 0.3 is reproducible.
//   * Generation loop + EOS double-check (L1028-1078): max 2048 tokens,
//     window guard prompt+i+1 >= 4096, is_eog || token==eos break.
//   * Output trim + outer-quote strip  (L1082-1099).
// NO control-token scrub and NO shrink loop are replicated: this tool is fed
// only vetted corpus texts far below the 2032-token prompt budget, and the
// probe FAILS LOUD (writes BUDGET_EXCEEDED) instead of truncating, so a run
// is never silently measured on a different prompt than production would send.
#include "llama.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ---- production constants mirrored from src/engine.hpp / engine.cpp ----
static std::string slurp_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "probe: FATAL cannot read %s\n", path.c_str());
        std::exit(2);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    // Strip a UTF-8 BOM if an editor added one (corpus text must stay pure UTF-8).
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
    return s;
}
static constexpr int kNCtx = 4096;             // kLlamaNCtx
static constexpr int kGenReserve = 2048;       // kLlamaGenReserve
static constexpr int kSafetyMargin = 16;       // kLlamaTokenSafetyMargin
static constexpr int kPromptBudget = kNCtx - kGenReserve - kSafetyMargin; // 2032
static constexpr int kPenaltyLastN = 64;       // kPenaltyLastN

struct Run {
    std::string id;
    std::string pair;    // display only, e.g. "FR->AR"
    std::string src_en;  // English name pinned in the instruction ("" = no hint)
    std::string tgt_en;  // English name pinned in the instruction
    int sent = 0;        // source sentence count (display only)
    float temp = 0.3f;
    // Per-run sampler overrides (session 260913_0002 grid). Defaults are the
    // shipped production values; a RUN header may set top_p=/top_k=/rep_pen=
    // to probe other cells. top_k=0 disables the top_k stage (engine parity:
    // engine.cpp L1009 only adds top_k when >0).
    float top_p = 0.6f;
    int top_k = 20;
    float rep_pen = 1.05f;
    std::string text;
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

// Manifest format (UTF-8, no BOM):
//   @@@RUN id=L1 pair=EN->AR src=English tgt=Arabic sent=10 temp=0.0
//          [top_p=0.6] [top_k=20] [rep_pen=1.05]      (optional per-run
//          sampler overrides; omitted fields fall back to the CLI values,
//          which themselves default to the shipped production values)
//   <arbitrary text lines>
//   @@@END
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
        hs >> head;  // "@@@RUN"
        std::string tok;
        while (hs >> tok) {
            std::string v;
            if (!(v = field_of(tok, "id")).empty()) r.id = v;
            else if (!(v = field_of(tok, "pair")).empty()) r.pair = v;
            else if (!(v = field_of(tok, "src")).empty()) r.src_en = v;
            else if (!(v = field_of(tok, "tgt")).empty()) r.tgt_en = v;
            else if (!(v = field_of(tok, "temp")).empty()) r.temp = std::strtof(v.c_str(), nullptr);
            else if (!(v = field_of(tok, "sent")).empty()) r.sent = std::atoi(v.c_str());
            else if (!(v = field_of(tok, "top_p")).empty()) r.top_p = std::strtof(v.c_str(), nullptr);
            else if (!(v = field_of(tok, "top_k")).empty()) r.top_k = std::atoi(v.c_str());
            else if (!(v = field_of(tok, "rep_pen")).empty()) r.rep_pen = std::strtof(v.c_str(), nullptr);
        }
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line == "@@@END") break;
            if (!r.text.empty()) r.text.push_back('\n');
            r.text += line;
        }
        runs.push_back(std::move(r));
    }
    return runs;
}

// Byte-identical replica of BuildPrompt's English branch (config.cpp L588-597)
// for a pinned non-Chinese target: "Translate the following [src ]segment
// into tgt, without additional explanation.\n\n" + text.
static std::string build_prompt_like_app(const Run& r) {
    std::string p = "Translate the following ";
    if (!r.src_en.empty()) {
        p.append(r.src_en);
        p.push_back(' ');
    }
    p.append("segment into ");
    p.append(r.tgt_en);
    p.append(", without additional explanation.\n\n");
    p.append(r.text);
    return p;
}

int main(int argc, char** argv) {
    std::string model_path, manifest_path, out_path;
    int32_t seed = 42;
    float top_p = 0.6f;   // shipped config
    int top_k = 20;       // shipped config
    float rep_pen = 1.05f; // shipped config

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
        else if (a == "--seed") seed = std::atoi(next("--seed").c_str());
        else if (a == "--top-p") top_p = std::strtof(next("--top-p").c_str(), nullptr);
        else if (a == "--top-k") top_k = std::atoi(next("--top-k").c_str());
        else if (a == "--rep-pen") rep_pen = std::strtof(next("--rep-pen").c_str(), nullptr);
        else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return 2;
        }
    }
    if (model_path.empty() || manifest_path.empty() || out_path.empty()) {
        std::fprintf(stderr, "usage: temp_probe --model F.gguf --manifest M.txt --out O.txt [--seed N]\n");
        return 2;
    }

    std::vector<Run> runs = parse_manifest(slurp_file(manifest_path));
    if (runs.empty()) {
        std::fprintf(stderr, "manifest contains no @@@RUN records\n");
        return 2;
    }
    std::fprintf(stderr, "probe: %zu runs, model=%s seed=%d top_p=%g top_k=%d rep_pen=%g\n",
                 runs.size(), model_path.c_str(), seed, top_p, top_k, rep_pen);

    llama_backend_init();

    // ---- EnsureLoaded GPU leg (engine.cpp L746-752 + SetGpuOffloadParams L617-649) ----
    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 99;
    mparams.split_mode = LLAMA_SPLIT_MODE_NONE;
    mparams.main_gpu = 0;
    mparams.devices = nullptr;
    mparams.tensor_split = nullptr;
    llama_model* model = llama_model_load_from_file(model_path.c_str(), mparams);
    if (!model) {
        std::fprintf(stderr, "probe: GPU load failed, retrying CPU leg (n_gpu_layers=0 + SPLIT_LAYER)\n");
        mparams = llama_model_default_params();
        mparams.n_gpu_layers = 0;
        mparams.split_mode = LLAMA_SPLIT_MODE_LAYER;
        mparams.devices = nullptr;
        mparams.tensor_split = nullptr;
        model = llama_model_load_from_file(model_path.c_str(), mparams);
        if (!model) {
            std::fprintf(stderr, "probe: FATAL model load failed\n");
            llama_backend_free();
            return 1;
        }
    }

    const llama_vocab* vocab = llama_model_get_vocab(model);
    const char* chat_tmpl = llama_model_chat_template(model, nullptr);
    std::fprintf(stderr, "probe: chat template %s\n", chat_tmpl ? "present" : "absent");

    // ---- EnsureLoaded context config (engine.cpp L782-799) ----
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
    std::fprintf(stderr, "llama_backend_init done\n");

    for (const Run& r : runs) {
        std::fprintf(stderr, "probe: run %s pair=%s temp=%g\n", r.id.c_str(), r.pair.c_str(), r.temp);

        std::string header = "=== temp=";
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.1f", r.temp);
            header += buf;
        }
        header += " seed=" + std::to_string(seed);
        header += " pair=" + r.pair;
        header += " sent=" + std::to_string(r.sent);
        header += " id=" + r.id;
        if (r.temp > 0.001f) {
            char pbuf[8];
            header += " top_p=";
            std::snprintf(pbuf, sizeof(pbuf), "%.2f", r.top_p);
            header += pbuf;
            header += " top_k=" + std::to_string(r.top_k);
            header += " rep_pen=";
            std::snprintf(pbuf, sizeof(pbuf), "%.2f", r.rep_pen);
            header += pbuf;
        }
        header += " ===\n";
        out << header;

        // prompt = BuildPrompt replica + GGUF chat template (engine.cpp L846-862)
        std::string prompt = build_prompt_like_app(r);
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

        // tokenize with production flags: add_special=true, parse_special=true (L893-916)
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
                out << "ERROR: tokenizer rejected the prompt (code PROBE/tokenize/001)\n\n";
                std::fprintf(stderr, "probe: tokenize failed for %s\n", r.id.c_str());
                continue;
            }
            prompt_tokens.resize(static_cast<size_t>(n));
        }

        // Loud budget guard instead of the production shrink loop (see header note).
        if (static_cast<int>(prompt_tokens.size()) > kPromptBudget) {
            out << "BUDGET_EXCEEDED prompt_tokens=" << prompt_tokens.size() << " > " << kPromptBudget << "\n\n";
            std::fprintf(stderr, "probe: BUDGET EXCEEDED for %s (%d tokens)\n", r.id.c_str(),
                         static_cast<int>(prompt_tokens.size()));
            continue;
        }

        llama_memory_clear(llama_get_memory(ctx), true);
        llama_batch batch = llama_batch_get_one(prompt_tokens.data(), static_cast<int32_t>(prompt_tokens.size()));
        if (llama_decode(ctx, batch) != 0) {
            out << "ERROR: prompt llama_decode failed (code PROBE/decode/002)\n\n";
            std::fprintf(stderr, "probe: prompt decode failed for %s\n", r.id.c_str());
            continue;
        }

        // sampler chain identical to engine.cpp L1002-1020, except the fixed
        // dist seed. Per-run top_p/top_k/rep_pen override the CLI defaults.
        llama_sampler* smpl = nullptr;
        if (r.temp <= 0.001f) {
            smpl = llama_sampler_init_greedy();
        } else {
            llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
            smpl = llama_sampler_chain_init(sparams);
            if (r.rep_pen > 1.0f) {
                llama_sampler_chain_add(smpl, llama_sampler_init_penalties(kPenaltyLastN, r.rep_pen, 0.0f, 0.0f));
            }
            if (r.top_k > 0) {
                llama_sampler_chain_add(smpl, llama_sampler_init_top_k(r.top_k));
            }
            if (r.top_p > 0.0f && r.top_p < 1.0f) {
                llama_sampler_chain_add(smpl, llama_sampler_init_top_p(r.top_p, 1));
            }
            llama_sampler_chain_add(smpl, llama_sampler_init_temp(r.temp));
            llama_sampler_chain_add(smpl, llama_sampler_init_dist(seed));
        }

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

        // trim + outer-quote strip (engine.cpp L1082-1099; probe source is never quoted)
        std::string trimmed = trim_ascii(output_u8);
        if (trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"' &&
            !r.text.empty() && r.text.front() != '"' && r.text.back() != '"') {
            trimmed = trimmed.substr(1, trimmed.size() - 2);
        }

        out << trimmed << "\n";
        out << "--- gen_tokens=" << gen_tokens
            << " prompt_tokens=" << prompt_tokens.size()
            << " out_chars=" << trimmed.size() << "\n\n";
        out.flush();
    }

    out.close();
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    std::fprintf(stderr, "probe: done\n");
    return 0;
}
