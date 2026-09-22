// translation_common implementation — REQ-043 (plan §5.1-1). Every function in
// this file is a VERBATIM move from src/engine.cpp (session history: I5, P2,
// REQ-R01, R6 Phase 4, SEC-B2, M3, F3, REQ-R16, P7-F2, REQ-003); see
// translation_common.hpp for the extraction rationale. The only edits are the
// class rename (TranslationManager::LlamaEngine -> LocalInferenceEngine) and
// the unconditional 4-parameter Translate signature on the no-llama stub
// (the old stub omitted src_name and could not have compiled with 8-arg
// callers; behavior in HAVE_LLAMA_CPP builds is untouched).

#include "translation_common.hpp"

#include "config.hpp"        // BuildPrompt (R6 Phase 4 / Tencent SFT template)
#include "diag_logger.hpp"   // DIAG_F (shape-only diagnostics)
#include "engine.hpp"        // TruncateHeadTailWindow / ScrubControlTokenTexts /
                             // CollectControlTokenTexts / IsValidModelPath /
                             // VerifyModelSha256 / kLlama* budget constants
#include "unicode_utils.hpp" // ToUtf8 / ToUtf16

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

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
// P2 (session 260910_0001): re-pinned to the official Hy-MT2 model card plan -
// n_ctx 4096, generation reserve 2048 (see src/engine.hpp for the KV-cache
// memory justification and the max_tokens=4096 trade-off analysis).
static_assert(kLlamaNCtx == 4096, "REQ-R01/P2: n_ctx is 4096 (EnsureLoaded must configure the same)");
static_assert(kLlamaGenReserve == 2048, "REQ-R01/P2: generation reserve equals max_gen_tokens");
static_assert(kLlamaPromptTokenBudget == kLlamaNCtx - kLlamaGenReserve - kLlamaTokenSafetyMargin,
              "REQ-R01: prompt budget = n_ctx - gen reserve - safety margin");
static_assert(kLlamaPromptTokenBudget == 2032, "REQ-R01/P2: prompt token budget is 4096-2048-16 = 2032");

} // namespace

#ifdef HAVE_LLAMA_CPP

namespace {

// REQ-059: the fixed end-of-conversation marker table, shared by the
// mid-output strip (StripEndOfConversationMarkers) and the bare-special
// acceptance predicate. Session 260922_0002 live evidence: llama's BUILT-IN
// default template (ChatML) spells its turn ends as <|im_end|>/<|im_start|>,
// which the Gemma-family native specials do NOT cover (the MiLM vocab
// declares eot id 106 whose text is the NATIVE form, while the model spelled
// the CHATML form out of NORMAL tokens). Neither source alone catches both
// shapes, so the table stays fixed alongside the declared-special texts.
constexpr const char* kEndOfConversationMarkers[] = {
    "<|im_end|>", "<|im_start|>", "<end_of_turn>", "<start_of_turn>",
};

// REQ-059 perf: rung-1 early-echo-abort cadence — every N generated tokens
// the accumulated output is compared against the source prefix (exact
// compare). 16 tokens of exact source tracking is already conclusive (a
// real translation never tracks the source for that long), and the probe
// stays negligible against a 4B decode.
constexpr int kEchoPrefixProbeTokenCadence = 16;

// REQ-R16 (audit §5 latent item 4): llama.cpp abort callback. ggml calls this
// between tensor-evaluation chunks of an in-flight llama_decode(); returning
// true aborts the compute. user_data is the caller's cancellation atomic
// (registered once at engine creation; TranslationManager passes its shutdown
// latch, the engine host passes its per-request flag — the address stays
// stable for the engine's lifetime). Documented llama.cpp limitation: CPU
// execution only - the per-token stop check in the decode loop below covers
// the GPU path (one forward pass per token, milliseconds each).
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

// P7-F2 compile-time pin: the device-pinning logic in EnsureLoaded relies on
// the exact b6099 split_mode enum layout (llama.h L184-187: NONE=0, LAYER=1).
// If a llama.cpp upgrade renumbers the enum, the build fails here instead of
// silently mis-pinning model devices.
static_assert(LLAMA_SPLIT_MODE_NONE == 0 && LLAMA_SPLIT_MODE_LAYER == 1,
              "P7-F2: llama.cpp split_mode enum layout changed; re-verify the "
              "NONE+main_gpu device pin and the CPU-fallback restore in EnsureLoaded");

// R6 Phase 4 (B2, architect plan §4.1 item 2): opt-in local-prompt observability.
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

} // namespace

// P7-F2: the SetGpuOffloadParams DEFINITION moved to
// engine_core_helpers.cpp (REQ-043 M6 T1 — design §4.2 lists it among the
// llama-independent seams of engine_core; keeping it here too produced
// LNK2005 within the same library). The declaration is re-exported via
// engine.hpp -> engine_core_helpers.hpp; EnsureLoaded below still calls it
// by its unqualified name.

LocalInferenceEngine::LocalInferenceEngine() {
    llama_log_set([](ggml_log_level level, const char* text, void* /*user_data*/) {
        if (level >= GGML_LOG_LEVEL_WARN) {
            DIAG_F("%s", text);
        }
    }, nullptr);
    llama_backend_init();
}

LocalInferenceEngine::~LocalInferenceEngine() {
    Unload();
    llama_backend_free();
}

void LocalInferenceEngine::Unload() {
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
    // SEC-B2: the scrub set belongs to the unloaded vocab - force a rebuild
    // for whatever model loads next (CollectControlTokenTexts is vocab-
    // derived, never hardcoded, so an alternative user GGUF is covered by
    // its own vocabulary).
    control_token_texts.clear();
    control_texts_built = false;
}

bool LocalInferenceEngine::EnsureLoaded(const std::string& path) {
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

    // F3 (security, session 260909_0002, audit §F3): content-hash pin at
    // runtime. The installer verifies the SHA-256 only at download time,
    // so a GGUF tampered AFTER installation (or restored from backup)
    // used to reach the llama.cpp parser unchecked. VerifyModelSha256()
    // streams the file through Windows CNG once and caches the result in
    // a "<model>.sha256ok" marker keyed on mtime+size, so the 1.91 GB
    // hash costs at most one full pass per changed file. Fail-closed for
    // the pinned filename; user-configured alternative names proceed on
    // explicit-config consent basis (see engine.hpp contract). A false
    // return here behaves like any load failure: Auto falls back to the
    // cloud, strict-local surfaces EngineFailed/LocalModelMissing to the
    // worker, and every rejection carries an ENGINE/VerifyModelSha256/NNN
    // diagnostic on stderr (DIAG_F mirrors to the diagnostic log).
    // This also completes the F2 story: an installer run where the user
    // answered "No" to the mismatch dialog leaves the unverified file in
    // place, but the app refuses to load it now.
    if (!VerifyModelSha256(std::filesystem::path{path}, {})) {
        return false;
    }

    // REQ-R16 + REQ-051 U-1 FIX 2: if a load-cancellation was requested while
    // we were queued behind the caller's lock, do not even start a model load.
    // The LOAD leg reads LoadCancelRequested() (load_cancel_flag, defaulting
    // to the cancel_flag alias) — a decode-time abort never reaches it.
    if (LoadCancelRequested()) {
        DIAG_F("ENGINE/EnsureLoaded/030: load skipped, shutdown cancellation pending\n");
        return false;
    }

    llama_model_params mparams = llama_model_default_params();
    // P7-F2 (universal GPU, session 260909_0004): GPU-offload leg pins the
    // WHOLE model to device 0 (= CUDA on NVIDIA dual-backend boxes) to stop
    // the CUDA+Vulkan layer split of one physical card. Full contract +
    // evidence: SetGpuOffloadParams in src/engine.hpp. Seam-tested
    // headlessly by TestP7F2GpuOffloadParams (tests/run_tests.cpp).
    SetGpuOffloadParams(mparams, /*gpu_offload=*/true);
    // REQ-R16: abort an in-progress model load when shutdown is requested.
    // REQ-051 U-1 FIX 2: the progress callback reads the LOAD flag (never the
    // decode flag), so a per-job abort mid-load cannot discard the load — the
    // default load_cancel_flag alias keeps the legacy behavior byte-identical.
    mparams.progress_callback = LlamaLoadProgress;
    mparams.progress_callback_user_data =
        const_cast<void*>(static_cast<const void*>(EffectiveLoadCancelFlag()));

    model = llama_model_load_from_file(path.c_str(), mparams);
    if (!model) {
        // REQ-R16: a cancel-aborted load is not a CUDA failure; do not
        // spend another full load attempt on the CPU path afterwards.
        // REQ-051 U-1 FIX 2: same load-flag split as the pre-load check.
        if (LoadCancelRequested()) {
            DIAG_F("ENGINE/EnsureLoaded/031: model load aborted by shutdown cancellation\n");
            return false;
        }
        // Fallback to CPU-only load if CUDA load encounters an issue
        // P7-F2: this leg MUST un-pin (n_gpu_layers=0 + default
        // LLAMA_SPLIT_MODE_LAYER, per the seam contract): llama.cpp
        // validates split_mode/main_gpu against the GPU-device list even
        // at n_gpu_layers=0 (src/llama.cpp L200-213), so NONE +
        // main_gpu=0 would fail this retry too on machines with zero
        // enumerable GPU devices and hard-break the CPU fallback.
        SetGpuOffloadParams(mparams, /*gpu_offload=*/false);
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

std::wstring LocalInferenceEngine::Translate(
    std::wstring_view text,
    std::string_view tgt_name,
    std::string_view src_name,
    const std::string& path,
    float temperature,
    float top_p,
    int top_k,
    float rep_pen
) {
    // REQ-R16: a canceled engine short-circuits before touching llama.
    // REQ-051 U-1 FIX 1: every Translate() entry clears the wall-clock
    // exhaustion latch so a previous timeout can never leak into this result.
    decode_wall_clock_exhausted_ = false;
    if (CancelRequested()) {
        return {};
    }
    if (!EnsureLoaded(path)) {
        return {};
    }

    // SEC-B2: build the scrub set from the now-loaded vocab (once per model
    // load). The only way the security guard could silently disappear is an
    // EMPTY set on a non-null vocab (degenerate/corrupt vocab) - surface
    // that loudly instead of pretending the scrub is active.
    if (!control_texts_built) {
        control_token_texts = CollectControlTokenTexts(vocab);
        control_texts_built = true;
        if (vocab && control_token_texts.empty()) {
            DIAG_F("ENGINE/ScrubControlTokens/001: vocab exposes no control-class tokens; scrub set is empty\n");
        }
    }

    // Tencent Hy-MT2 instruction format + optional GGUF chat template. Both are
    // rebuilt inside the REQ-R01 shrink loop, so they live in one lambda.
    // REQ-059 rung 1: the template is probed ONCE per call — if applying it to
    // a single user message returns the content VERBATIM (adds no structure;
    // MiLM ships the trivial {% for message in messages %}{{ message.content
    // %}{% endfor %} concat template) the bare fixed instruction is out-of-
    // distribution (live evidence: immediate-EOS empty decode). A trivial (or
    // absent) template therefore falls back to llama's BUILT-IN default
    // template (ChatML) instead; non-trivial templates (Hy-MT2) keep the
    // existing path byte-identically.
    const char* chat_tmpl = llama_model_chat_template(model, nullptr);
    bool chat_tmpl_is_trivial = true;
    if (chat_tmpl) {
        static constexpr const char* kTmplProbe = "REQ059_TEMPLATE_PROBE";
        llama_chat_message probe{"user", kTmplProbe};
        const int32_t probe_needed =
            llama_chat_apply_template(chat_tmpl, &probe, 1, true, nullptr, 0);
        if (probe_needed > 0) {
            std::vector<char> formatted(static_cast<size_t>(probe_needed) + 1);
            const int32_t probe_written = llama_chat_apply_template(
                chat_tmpl, &probe, 1, true, formatted.data(),
                static_cast<int32_t>(formatted.size()));
            chat_tmpl_is_trivial =
                probe_written > 0 &&
                std::string(formatted.data(), static_cast<size_t>(probe_written)) == kTmplProbe;
        }
    }
    // REQ-059: two fixed prompt forms, selected per attempt (rung 1 = chat,
    // rung 2 = completion); rung 2 exists only for trivial-template user
    // models — the completion form is never applied to the Hy-MT2 path's
    // non-trivial template (attempt 1 only follows an empty/echo rung 1).
    auto build_final_prompt = [&](std::wstring_view s, bool completion_form) -> std::string {
        std::string u8 = ToUtf8(s);
        if (u8.empty()) {
            return {};
        }
        std::string p = completion_form
            ? BuildCompletionPrompt(u8, tgt_name, src_name)
            : BuildPrompt(u8, tgt_name, src_name);
        if (!completion_form) {
            if (chat_tmpl && !chat_tmpl_is_trivial) {
                llama_chat_message msg{"user", p.c_str()};
                int32_t needed = llama_chat_apply_template(chat_tmpl, &msg, 1, true, nullptr, 0);
                if (needed > 0) {
                    std::vector<char> formatted(needed + 1);
                    int32_t written = llama_chat_apply_template(chat_tmpl, &msg, 1, true, formatted.data(), static_cast<int32_t>(formatted.size()));
                    if (written > 0) {
                        p.assign(formatted.data(), written);
                    }
                }
            } else {
                // REQ-059 rung 1: trivial (concat-only) or absent template —
                // wrap with llama's BUILT-IN default template (ChatML) so the
                // fixed instruction is in-distribution for Gemma-style user
                // models. (Hy-MT2 never reaches this branch: its real
                // template is non-trivial and stays byte-identical.)
                llama_chat_message msg{"user", p.c_str()};
                int32_t needed = llama_chat_apply_template(nullptr, &msg, 1, true, nullptr, 0);
                if (needed > 0) {
                    std::vector<char> formatted(needed + 1);
                    int32_t written = llama_chat_apply_template(nullptr, &msg, 1, true, formatted.data(), static_cast<int32_t>(formatted.size()));
                    if (written > 0) {
                        p.assign(formatted.data(), written);
                    }
                }
            }
        }
        // REQ-003 (session 260909) exhaustive-content audit: the built
        // prompt embeds the user's source text (first 240 bytes printed
        // below), so it needs BOTH gates - the pre-existing opt-in
        // EMEBALA_DEBUG_PROMPT env var AND diag_log_content (default off).
        // With either off, only the byte count is recorded.
        if (DebugPromptEnabled()) {
            if (diag::ContentLoggingEnabled()) {
                DIAG_F(
                        "ENGINE/BuildPrompt/050: local prompt target=\"%.*s\" source=\"%.*s\" bytes=%zu:\n%.240s\n---\n",
                        static_cast<int>(tgt_name.size()), tgt_name.data(),
                        static_cast<int>(src_name.size()), src_name.data(),
                        p.size(), p.c_str());
            } else {
                DIAG_F(
                        "ENGINE/BuildPrompt/050: local prompt target=\"%.*s\" source=\"%.*s\" bytes=%zu (content logging disabled)\n",
                        static_cast<int>(tgt_name.size()), tgt_name.data(),
                        static_cast<int>(src_name.size()), src_name.data(),
                        p.size());
            }
        }
        return p;
    };

    // Tokenize helper (REQ-R01): fills `out` and returns the token count, or -1
    // on tokenizer failure. Allocation mirrors the original probe-then-size.
    // SEC-B2 (session 260911_0002, verify 233020 §5): parse_special=true is
    // load-bearing and MUST stay - it is how the chat-template markers the
    // app itself spliced in above (begin_of_sentence / hy_User / hy_Assistant)
    // tokenize as genuine control tokens. The injection channel it opens for
    // USER text is closed upstream by ScrubControlTokenTexts, not here.
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
    // SEC-B2 (OWASP LLM01, verify 233020 §6): with parse_special=true the
    // tokenizer substring-matches EVERY CONTROL/USER_DEFINED/UNKNOWN vocab
    // text (llama-vocab.cpp L2396-2402, L2603-2635), so a user text
    // containing "<｜hy_User｜>" (id 120006), "<｜hy_Assistant｜>"
    // (120007) or the metadata-EOS "<｜hy_place▁holder▁no▁2｜>" (120020)
    // would be emitted as a GENUINE role-boundary control token -> prompt
    // injection. Scrub the user-controlled text here - after capture,
    // BEFORE BuildPrompt's verbatim append and the chat-template wrap, so
    // the app's own template markers (added inside build_final_prompt
    // after this point) stay intact. The shrink loop below only re-slices
    // this already-scrubbed copy, so every rebuild inherits the scrub.
    src_w = ScrubControlTokenTexts(src_w, control_token_texts);
    // REQ-059: the prompt build + tokenize + decode body runs per attempt
    // (attempt 0 = rung-1 chat form; attempt 1 = rung-2 completion form,
    // only after an empty/echo rung 1). The shrink loop, hard cap, sampler,
    // cancel/wall-clock checks and the REQ-051 U-1 latch semantics are
    // identical per attempt; the decode wall-clock budget is FRESH per
    // attempt. Hy-MT2 never degrades: its rung-1 output is never empty/echo.
    // REQ-059 live-verified: a rung-1 output can also be a BARE SPECIAL
    // MARKER — the live MiLM host spelled "<|im_end|>" out of SIX NORMAL
    // tokens (the eot id 106 was never sampled, so the stop disjuncts and
    // the control-piece scrub — NORMAL pieces by definition — could not
    // catch it). Such an output is rejected exactly like empty/echo.
    // REQ-059: the full end-of-conversation marker text set, built ONCE per
    // call: the fixed ChatML/native table above PLUS the texts of every
    // special id the vocab declares (eos/eot/bos/sep/pad; same no-op-safety
    // argument as the eos disjunct for undeclared ids (-1)). Shared by the
    // mid-output strip and the bare-special acceptance predicate.
    std::vector<std::string> eoc_markers;
    for (const char* marker : kEndOfConversationMarkers) {
        eoc_markers.emplace_back(marker);
    }
    {
        const llama_token specials[] = {
            llama_vocab_eos(vocab), llama_vocab_eot(vocab), llama_vocab_bos(vocab),
            llama_vocab_sep(vocab), llama_vocab_pad(vocab),
        };
        for (const llama_token id : specials) {
            if (id < 0) {
                continue;
            }
            const char* special_text = llama_vocab_get_text(vocab, id);
            if (special_text && *special_text) {
                eoc_markers.emplace_back(special_text);
            }
        }
    }
    auto output_is_bare_special = [&](const std::string& out) {
        if (out.empty()) {
            return false;
        }
        return std::find(eoc_markers.begin(), eoc_markers.end(), out) != eoc_markers.end();
    };
    // REQ-059: strip end-of-conversation markers embedded in an otherwise
    // real output (live user case: rung-1 answered with the source ECHO +
    // a spelled-out "<|im_end|>" tail, which the EOG break and the
    // control-piece scrub both miss because the marker is spelled from
    // NORMAL tokens). Source-guard: a marker the SOURCE text itself
    // contains is NEVER stripped, so legitimate user text about chat
    // formats survives untouched.
    auto StripEndOfConversationMarkers = [&](std::string& out, std::string_view source_u8) {
        for (const std::string& marker : eoc_markers) {
            if (source_u8.find(marker) != std::string_view::npos) {
                continue; // source-guard (see above)
            }
            size_t pos = 0;
            while ((pos = out.find(marker, pos)) != std::string::npos) {
                out.erase(pos, marker.size());
            }
        }
    };
    std::string trimmed_u8;
    bool degraded_to_completion = false;
    // REQ-059 perf: set when the rung-1 early-echo probe aborts the decode —
    // the acceptance test below treats it exactly like an echo (the output
    // is only a source PREFIX at that point, so the full-equality echo
    // compare alone could not catch it).
    bool rung1_aborted_echo = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
    const bool completion_form = (attempt == 1);
    std::string prompt = build_final_prompt(src_w, completion_form);
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
            prompt = build_final_prompt(src_w, completion_form);
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
    // REQ-051 U-1 FIX 1: input-scaled generation cap (min(kLlamaGenReserve,
    // max(256, 4n+128))) replaces the flat 2048 mirror — kLlamaGenReserve stays
    // the named ceiling constant (untouched REQ-R01 pin), but a degenerate
    // non-Hy-MT2 loop now deterministically dies at 4x the prompt size + 128
    // tokens instead of pasting 2048 tokens of garbage. The wall-clock budget
    // below is the second, VRAM-contention-proof backstop: whichever trips
    // first wins.
    const int max_gen_tokens = ScaledMaxGenTokens(n_prompt_tokens);
    // REQ-051 U-1 FIX 1: the decode wall-clock budget starts HERE (after the
    // prompt decode + sampler setup) and covers token GENERATION only — a
    // model load is governed by the REQ-051 U-1 FIX 2 finish-once-started
    // rule, not by this clock.
    // REQ-059 perf: the budget is input-scaled
    // (ScaledDecodeWallClockBudgetMs(max_gen_tokens)): the 12 s REQ-051
    // floor for typical inputs, scaling to 27 s for long translations on
    // slow 4B user models. REQ-051 U-1 latch semantics unchanged.
    const auto decode_start = std::chrono::steady_clock::now();
    const auto decode_deadline = decode_start +
        std::chrono::milliseconds(ScaledDecodeWallClockBudgetMs(max_gen_tokens));

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

        // REQ-051 U-1 FIX 1: wall-clock budget check BETWEEN DECODE STEPS
        // (same seam as the cancellation check — at most one more sampled
        // token runs before we exit). On expiry the partial output is
        // discarded as empty and the exhaustion latch is set; the worker
        // maps it to the frozen "timeout" wire code (transient -> the
        // client's one-shot retry). Shape-only line: ms + token index, no
        // content.
        const auto now = std::chrono::steady_clock::now();
        if (now >= decode_deadline) {
            const long long elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - decode_start).count();
            decode_wall_clock_exhausted_ = true;
            DIAG_F("ENGINE/Translate/033: decode wall-clock budget exhausted at token %d (%lld ms); answering timeout\n",
                   i, static_cast<long long>(elapsed_ms));
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

        // REQ-006: EOS double-check. Hy-MT2 GGUF metadata ships with
        // special_eos_id missing from the eog set, so llama_vocab_is_eog
        // alone can let generation run past the model's stop token. The
        // is_eog test stays FIRST (fast path unchanged for well-formed
        // vocabs); the eos disjunct only catches the malformed-metadata
        // case. llama_vocab_eos returns -1 for EOS-less vocabs, which
        // never equals a valid sampled token, so this is a safe no-op
        // there.
        // REQ-059: third disjunct — llama_vocab_eot (end-of-turn). Gemma-
        // family GGUFs (user models) declare an eot id that is NOT eos and
        // NOT in the eog set consulted above, so a model emitting its eot
        // directly would leak the piece into the output (live MiLM host,
        // session 260922_0002). NOTE (live-traced): this vocab's actual
        // failure spelled "<|im_end|>" out of SIX NORMAL tokens instead —
        // caught by the bare-special acceptance test below, not this
        // disjunct; both guards stay (a direct eot sample remains possible
        // on other GGUFs). Mirroring the eos no-op-safety argument:
        // llama_vocab_eot returns -1 for eot-less vocabs, never equal to a
        // valid sampled token.
        if (llama_vocab_is_eog(vocab, token) || token == llama_vocab_eos(vocab) ||
            token == llama_vocab_eot(vocab)) {
            break;
        }

        char piece[256] = {};
        int n_piece = llama_token_to_piece(vocab, token, piece, sizeof(piece), 0, false);
        if (n_piece > 0) {
            // REQ-059: output-side control-piece scrub (defense-in-depth).
            // The eot break above handles termination; this stops any other
            // stray control-class piece (mid-output specials) from reaching
            // the caller's text. The scrub set is the same per-load
            // control_token_texts built for the SEC-B2 input scrub — cheap:
            // it holds only control-class token texts.
            const std::wstring piece_w =
                ToUtf16(std::string_view(piece, static_cast<size_t>(n_piece)));
            if (std::find(control_token_texts.begin(), control_token_texts.end(), piece_w) ==
                control_token_texts.end()) {
                output_u8.append(piece, n_piece);
            }
        } else if (n_piece < 0) {
            int needed = -n_piece;
            std::vector<char> big_piece(needed);
            int written = llama_token_to_piece(vocab, token, big_piece.data(), needed, 0, false);
            if (written > 0) {
                // REQ-059: same control-piece scrub on the oversized-piece path.
                const std::wstring piece_w =
                    ToUtf16(std::string_view(big_piece.data(), static_cast<size_t>(written)));
                if (std::find(control_token_texts.begin(), control_token_texts.end(), piece_w) ==
                    control_token_texts.end()) {
                    output_u8.append(big_piece.data(), written);
                }
            }
        }

        // REQ-059 perf: early echo abort (RUNG 1 ONLY). A rung-1 echo on a
        // long paragraph otherwise generates the ENTIRE source (hundreds of
        // tokens, seconds on a 4B model) before the end-of-generation echo
        // check can reject it — roughly doubling the bill before rung 2
        // re-decodes from scratch. Every kEchoPrefixProbeTokenCadence
        // tokens, compare the accumulated output against the source prefix:
        // an EXACT-prefix match means the model is echoing — bail out now
        // (shape-only 037 line) and let the attempt loop degrade to the
        // completion form. Exact compare only, no fuzzy heuristic: Hy-MT2
        // translations never track the source for 16+ tokens, and even a
        // false fire still answers correctly via rung 2.
        if (!completion_form && (i + 1) % kEchoPrefixProbeTokenCadence == 0 &&
            !output_u8.empty() && output_u8.size() <= src_u8.size() &&
            output_u8.compare(0, output_u8.size(), src_u8, 0, output_u8.size()) == 0) {
            DIAG_F("ENGINE/Translate/037: rung-1 output tracks the source prefix at token %d (out=%zu src=%zu); aborting to the completion form\n",
                   i + 1, output_u8.size(), src_u8.size());
            rung1_aborted_echo = true;
            break;
        }

        batch = llama_batch_get_one(&token, 1);
        if (llama_decode(ctx, batch) != 0) {
            // REQ-049: a mid-generation decode failure used to fall through
            // with the partial output masquerading as a successful
            // translation; answer honestly (empty -> engine_failed) like the
            // prompt-decode failure above.
            DIAG_F("ENGINE/Translate/014: llama_decode failed mid-generation at token %d; answering engine_failed\n", i);
            llama_sampler_free(smpl);
            return {};
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

    trimmed_u8 = output_u8.substr(start, end - start);

    // REQ-059: strip embedded end-of-conversation markers FIRST — the
    // ladder's acceptance checks (echo compare + bare-special predicate)
    // and the colon/quote cleanup all operate on the STRIPPED string, so
    // "source + marker" reduces to exactly the source and is correctly
    // judged an ECHO (rung 2).
    StripEndOfConversationMarkers(trimmed_u8, src_u8);

    // REQ-059: leading-colon cleanup — Gemma-style models answering through
    // the ChatML wrap start with a stray ':' token; drop the colon (and any
    // spaces after it) when the source itself did not start with one.
    if (!trimmed_u8.empty() && trimmed_u8.front() == ':' &&
        (src_u8.empty() || src_u8.front() != ':')) {
        size_t colon_drop = 1;
        while (colon_drop < trimmed_u8.size() && trimmed_u8[colon_drop] == ' ') {
            ++colon_drop;
        }
        trimmed_u8.erase(0, colon_drop);
    }

    // Strip matching outer quotes if model wrapped translation in quotes but source text was not quoted
    if (trimmed_u8.size() >= 2 && trimmed_u8.front() == '\"' && trimmed_u8.back() == '\"') {
        if (src_u8.empty() || (src_u8.front() != '\"' && src_u8.back() != '\"')) {
            trimmed_u8 = trimmed_u8.substr(1, trimmed_u8.size() - 2);
        }
    }

    // REQ-059 rung acceptance: an EMPTY rung-1 decode (immediate EOS, or a
    // bare marker stripped to empty just above), an ECHO (trimmed output
    // equals the trimmed source — the live MiLM KO->EN behavior), or a
    // bare special marker degrades to the completion form for ONE more
    // decode with a fresh wall-clock budget. Hy-MT2 never trips this (its
    // rung-1 output is a real translation), so its path stays byte-identical
    // end to end.
    if (attempt == 0) {
        std::string src_cmp = src_u8;
        size_t cs = 0;
        while (cs < src_cmp.size() &&
               (src_cmp[cs] == ' ' || src_cmp[cs] == '\n' || src_cmp[cs] == '\r' || src_cmp[cs] == '\t')) {
            ++cs;
        }
        size_t ce = src_cmp.size();
        while (ce > cs &&
               (src_cmp[ce - 1] == ' ' || src_cmp[ce - 1] == '\n' ||
                src_cmp[ce - 1] == '\r' || src_cmp[ce - 1] == '\t')) {
            --ce;
        }
        src_cmp = src_cmp.substr(cs, ce - cs);
        const bool rung1_empty = trimmed_u8.empty();
        const bool rung1_echo = !rung1_empty && !src_cmp.empty() && trimmed_u8 == src_cmp;
        const bool rung1_bare_special = output_is_bare_special(trimmed_u8);
        if (!rung1_empty && !rung1_echo && !rung1_bare_special && !rung1_aborted_echo) {
            break; // acceptable rung-1 output — no completion-form retry
        }
        degraded_to_completion = true;
        DIAG_F("ENGINE/Translate/034: rung-1 chat form produced %s; retrying with the completion form (shape-only)\n",
               rung1_aborted_echo ? "a source-prefix echo (early abort)"
                                  : (rung1_empty ? "an empty decode"
                                                 : (rung1_echo ? "an echo of the source"
                                                               : "a bare special marker")));
    }
    } // REQ-059 attempt loop (rung 1 chat -> rung 2 completion)

    // REQ-059: the completion form can emit the same bare marker (defense:
    // the rung-1 check above only guards the retry decision) — answer
    // honestly with empty (-> engine_failed) instead of handing the caller
    // a control-token string.
    if (output_is_bare_special(trimmed_u8)) {
        DIAG_F("ENGINE/Translate/036: output is a bare special-token marker; answering engine_failed\n");
        return {};
    }

    if (degraded_to_completion) {
        DIAG_F("ENGINE/Translate/035: returning the completion-form result (shape-only)\n");
    }
    return ToUtf16(trimmed_u8);
}

#else // !HAVE_LLAMA_CPP

LocalInferenceEngine::LocalInferenceEngine() = default;
LocalInferenceEngine::~LocalInferenceEngine() = default;
void LocalInferenceEngine::Unload() {}
bool LocalInferenceEngine::EnsureLoaded(const std::string&) { return false; }
std::wstring LocalInferenceEngine::Translate(std::wstring_view, std::string_view,
                                             std::string_view, const std::string&,
                                             float, float, int, float) {
    // REQ-051 U-1 FIX 1: parity with the llama build — the exhaustion latch
    // is cleared at every Translate() entry even though the stub can never
    // exhaust the budget.
    decode_wall_clock_exhausted_ = false;
    return {};
}

#endif // HAVE_LLAMA_CPP

} // namespace emebalachat
