#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <windows.h> // DWORD for WaitInferenceIdle

// P7-F2: llama.h is an optional dependency here exactly as in engine.cpp
// (L21-26). When present, the SetGpuOffloadParams seam below compiles and is
// unit-tested; when absent (no-llama configuration), both legs fall back to
// the existing HAVE_LLAMA_CPP-guarded code paths and the seam is not built.
#if defined(HAVE_LLAMA_CPP) || __has_include("llama.h")
#ifndef HAVE_LLAMA_CPP
#define HAVE_LLAMA_CPP 1
#endif
#include "llama.h"
#endif

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

// F3 (security, session 260909_0002, audit §F3 MEDIUM): runtime content-hash
// pin for the shipped Hy-MT2 model. The installer (installer/setup.iss) pins
// this SHA-256 at download time; the app never re-checked the FILE CONTENTS
// before handing it to the llama.cpp GGUF parser. VerifyModelSha256() closes
// that gap: it is called by LlamaEngine::EnsureLoaded immediately BEFORE
// llama_model_load_from_file, fail-closed (a false return blocks the load).
//   001 path is empty
//   002 path does not exist as a regular file
//   003 file read error while hashing
//   004 hash does not match kExpectedModelSha256
// Semantics for user-configured model_path values (config.json lets a user
// point at ANY .gguf):
//   * file name == kPinnedModelFilename ("Hy-MT2-1.8B-Q8_0.gguf", the model
//     the installer downloads and the hash below was computed for) -> strict
//     fail-closed. Anything else named the same is corrupt or tampered.
//   * any OTHER file name -> the user deliberately selected a different model
//     (e.g. a Q4 quantization they downloaded themselves). The hash cannot
//     match by design, so we DIAG_F a visible warning (ENGINE/VerifyModelSha256/005)
//     and ALLOW the load: consent basis = the user explicitly set this path
//     in their own config file (on-device action, opt-in by construction).
//     This difference is deliberate and documented in EnsureLoaded's call site.
// kExpectedModelSha256 is the lowercase hex SHA-256 of
// https://huggingface.co/tencent/Hy-MT2-1.8B-GGUF/resolve/main/Hy-MT2-1.8B-Q8_0.gguf
// (certutil -hashfile ... SHA256, session 260909_0002; identical to
// EXPECTED_MODEL_SHA256 in installer/setup.iss - keep the two in sync when
// the model is ever rotated).
inline constexpr char kExpectedModelSha256[] =
    "5c3fe0b1408a5ceb0143184ef247b11b579c525f4b02b060e6c851bb76fef1a4";
inline constexpr std::string_view kPinnedModelFilename = "Hy-MT2-1.8B-Q8_0.gguf";

// Streams the whole file through Windows CNG (bcrypt.dll BCrypt* SHA-256, no
// third-party dependency) and writes 64 lowercase hex chars to out_hex.
// Returns false on any read/provider failure. Exposed for unit testing.
bool ComputeFileSha256(const std::filesystem::path& file, std::string& out_hex);

// Full F3 decision INCLUDING the marker cache (see VerifyModelIntegrity docs
// in engine.cpp for the <model>.sha256ok cache semantics). Returns true when
// the model load may proceed. marker_dir selects where the cache marker is
// stored (tests pass a temp dir; production uses the model's own directory).
bool VerifyModelSha256(const std::filesystem::path& model_path,
                       const std::filesystem::path& marker_dir);

// REQ-R01 (Batch D1): llama.cpp context-window sizing constants, centralized so
// the decode-time budget and the unit tests agree on one source of truth.
// kLlamaNCtx must stay in sync with cparams.n_ctx in engine.cpp EnsureLoaded().
//
// P2 (session 260910_0001): n_ctx raised 2048 -> 4096 per the official Tencent
// Hy-MT2-1.8B model card (recommended max_tokens=4096), which also forces the
// generation reserve to be re-tuned 512 -> 2048.
//
// KV-cache cost (MEASURED from build/models/Hy-MT2-1.8B-Q8_0.gguf in the F10
// audit, docs/260908_0002 report: hunyuan-dense, block_count=32, GQA
// head_count_kv=4, head_dim=128, f16 KV):
//   2 (K+V) x 32 layers x 4 kv_heads x 128 dims x 2 B = 64 KiB/token
//   n_ctx 4096 -> 256 MiB KV total (+128 MiB vs the old 2048 window).
//   With flash_attn enabled (EnsureLoaded L683) GPU residency is lower still.
//   Safe for the Q8_0 1.8B (~2.0 GB weights) on typical 8 GB+ machines.
//
// Generation split trade-off: the card's max_tokens=4096 is an API ceiling that
// is physically impossible inside a 4096-token window - a non-empty prompt
// (instruction wrapper + chat-template specials alone are ~40-650 tokens) would
// leave zero room for output. kLlamaGenReserve=2048 is the best balance for
// translation workloads: output length tracks input length, so a reserve (2048)
// roughly equal to the prompt budget (2032) maximizes both sides of the window
// symmetrically. A future n_ctx=8192 (+512 MiB KV) would fully honor the 4096
// output recommendation; deliberately NOT taken here to keep the 8 GB-class
// memory envelope of this decision.
inline constexpr int kLlamaNCtx = 4096;
inline constexpr int kLlamaGenReserve = 2048;   // tokens reserved for generation (max_gen_tokens)
inline constexpr int kLlamaTokenSafetyMargin = 16;
inline constexpr int kLlamaPromptTokenBudget =
    kLlamaNCtx - kLlamaGenReserve - kLlamaTokenSafetyMargin; // 2032

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
    // REQ-R16: local inference unwound because RequestCancel() was called
    // (app shutdown draining an in-flight decode). Distinct from
    // EngineFailed so callers do not surface a spurious error tone and,
    // critically, so the Auto policy does NOT fall through to a cloud
    // request that would transmit the user's text mid-exit.
    Canceled,
    // REQ-029-B: preferred=LocalLlama but the model file is absent on disk,
    // so translation never even started. The cause is distinct from
    // CloudConsentBlocked (the H2 consent gate refusing an allowed cloud
    // fallback), so logs/UI/sound can tell the user precisely "model
    // missing" instead of the old masquerade that reported Google/cloud and
    // then failed silently. Appended AFTER Canceled: existing values keep
    // their integer order (0..4), so log-based diagnostic scripts that
    // static_cast<int>(status) stay compatible.
    LocalModelMissing,
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

// R6 Phase 4 (B2, architect plan §4.1 item 3): pure routing seam for
// TranslationManager::Translate, unit-testable headlessly
// (TestR6P4LanguageRouting). PRECONDITION: the local Hy-MT2 engine is ACTIVE
// (model loaded) - engine availability itself stays in RefreshActiveEngine.
// Given the request pair and the user's configuration, returns the engine that
// will ACTUALLY serve the request:
//   * engine_type GoogleTranslate            -> GoogleTranslate (deliberate pick).
//   * LocalPairReliable(src, tgt)            -> LocalLlama (supported pair;
//                                                explicit pins are honored).
//   * unsupported pair, engine_type Auto     -> GoogleTranslate. Selecting
//                                                Auto is the REQ-R02 consent to
//                                                the documented cloud fallback,
//                                                so google_consent does not gate
//                                                this case (same rule as the
//                                                post-inference Auto fallback).
//   * unsupported pair, explicit LocalLlama  -> GoogleTranslate ONLY when the
//                                                user granted cloud consent
//                                                (google_consent); without it
//                                                the pin is respected and the
//                                                request stays local (caller
//                                                logs a routing warning).
EngineType PlanTranslationRouting(std::string_view src_code,
                                  std::string_view tgt_code,
                                  EngineType engine_type,
                                  bool google_consent);

// REQ-F4a: pure decision seam for the STARTUP local-model warmup in wWinMain
// (src/main.cpp). Returns true when the caller must spawn the PreloadLocalModel
// thread. Evidence (260908 session log): L3 shows a cloud-only config
// (engine=google cloud_fallback=0) yet L10-11 show the Hy-MT2 tokenizer and
// context loading anyway - ~1.4 s of dead startup time plus the model's RAM,
// because the old gate was file-existence-only. Contract:
//   * no model file on disk                                  -> false (historical: no warmup thread)
//   * explicit GoogleTranslate + NO cloud_fallback consent   -> false (cloud-only session: Translate()
//       can never route local - RefreshActiveEngine pins active_type_ to GoogleTranslate - and a later
//       tray switch to local lazy-loads via LlamaEngine::EnsureLoaded, the existing design)
//   * GoogleTranslate + cloud_fallback consent               -> true  (the user declared they want the
//       local model resident as the fallback safety net; historical preload preserved)
//   * Auto / LocalLlama with model available                 -> true  (local is the primary engine under
//       documented Auto semantics; startup behavior unchanged)
bool ShouldPreloadLocalModel(EngineType engine_type, bool cloud_fallback_enabled,
                             bool model_available);

// REQ-004 (session 260910_0003): pure decision seam for the RUNTIME tray
// switch-to-local warmup in wWinMain's on_select_engine (src/main.cpp). The
// 260910 report: selecting engine=local from the tray left the model
// unloaded, so the first translation paid the synchronous load stall (~7 s).
// Distinct from the startup gate ShouldPreloadLocalModel above: an explicit
// tray pick of "local" IS the user's local-serving intent, so the
// cloud_fallback consent plays no role in this decision (the switched-to
// engine serves regardless of the consent flag). Contract:
//   * no model file on disk             -> false (nothing to preload; the
//       historical no-model behavior stays: RefreshActiveEngine reports
//       "Local (Model Missing)" and Translate() pre-blocks with
//       LocalModelMissing - a background load could only fail)
//   * selected == LocalLlama + present  -> true  (spawn the async preload)
//   * any other selection (Google/Auto) -> false (a switch to cloud must not
//       pay the local model's RAM - the same dead-weight rule REQ-F4a applies
//       at startup. The current tray menu offers exactly google/local; if a
//       runtime Auto pick is ever added, extend this seam, its in-flight
//       guard, and TestReq004EngineSwitchPreloadGate together.)
bool ShouldPreloadOnEngineSwitch(EngineType selected, bool model_available);

#ifdef HAVE_LLAMA_CPP
// P7-F2 (universal GPU, session 260909_0004): pure params-construction seam
// for the two model-load legs of LlamaEngine::EnsureLoaded. The production
// call sites pass a llama_model_default_params() struct; this function sets
// ONLY the three offload-relevant fields and touches nothing else
// (progress_callback etc. stay caller-owned).
//   gpu_offload == true  (GPU leg, called BEFORE llama_model_load_from_file):
//     n_gpu_layers = 99; split_mode = LLAMA_SPLIT_MODE_NONE; main_gpu = 0.
//     WHY: with GGML_CUDA and GGML_VULKAN both statically linked (REQ-101), a
//     single NVIDIA card is exposed as TWO devices ("CUDA0" + "Vulkan0") -
//     llama.cpp adds every GPU device from every backend with no dedup
//     (build_gputest/_deps/llama_cpp-src/src/llama.cpp L183-190) and b6099's
//     default split_mode is LLAMA_SPLIT_MODE_LAYER (src/llama-model.cpp
//     L18521), which interleaves ~half the model's layers onto the typically
//     slower Vulkan half of the SAME physical card (P5 report F2). NONE makes
//     llama.cpp keep only devices[main_gpu] (src/llama.cpp L200-213); ggml
//     registers CUDA before Vulkan (ggml/src/ggml-backend-reg.cpp L168-169 vs
//     L177-178) and the device list preserves that enumeration order
//     (src/llama.cpp L175-192), so devices[0] is the CUDA device on NVIDIA
//     machines - the whole model runs on the fast backend. On AMD/Intel
//     Vulkan-only boxes exactly one GPU device exists, so the pin is a no-op.
//   gpu_offload == false (CPU fallback leg, n_gpu_layers=0 retry):
//     n_gpu_layers = 0; split_mode = LLAMA_SPLIT_MODE_LAYER (b6099 default);
//     main_gpu unchanged. MUST un-pin: llama.cpp validates split_mode/
//     main_gpu against the GPU-device list EVEN at n_gpu_layers = 0
//     (src/llama.cpp L200-213) - NONE + main_gpu = 0 with zero enumerable GPU
//     devices fails the load outright (LLAMA_SPLIT... "invalid value for
//     main_gpu", L204-207), which would hard-break the historical CPU
//     fallback on CPU-only machines. The CPU leg's behavior is therefore
//     byte-identical to the pre-F2 code.
//   Invariants (pinned by TestP7F2GpuOffloadParams): devices and
//   tensor_split stay nullptr on both legs - the whole point of F2 is that
//   the app never engages multi-device splitting; no RPC/multi-GPU feature
//   is used.
void SetGpuOffloadParams(llama_model_params& params, bool gpu_offload);
#endif


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

    // ---- REQ-R16 (audit §5 latent item 4): llama.cpp shutdown safety ----
    //
    // Requests cancellation of any in-flight local inference. The decode loop
    // checks the stop flag between every generated token, llama.cpp's own
    // abort_callback fires between CPU tensor-evaluation chunks, and the
    // model-load progress_callback unwinds a loading warmup, so an ongoing
    // llama_decode/llama_model_load sequence returns within milliseconds
    // instead of running to the full generation cap while app shutdown waits.
    // Safe to call at any time from any thread; a no-op when llama.cpp is not
    // linked. SHUTDOWN LATCH SEMANTICS: once requested, every subsequent
    // local Translate() also short-circuits (the flag is only cleared when a
    // NEW manager is constructed) - this is deliberate: the seam exists to
    // drain inference before process exit, not to pause/resume translation.
    void RequestCancel();

    // True while a cancellation request is pending for the local engine.
    // Exposed for the shutdown watchdog and the headless seam tests.
    bool IsCancelRequested();

    // REQ-R16 watchdog helper: returns true once the engine mutex is free
    // again, i.e. every in-flight Translate() has observed the cancellation
    // and unwound (the mutex is held across the whole decode). Call after
    // RequestCancel() to get a BOUNDED, observable confirmation that no
    // inference thread is still inside llama.cpp before the process exits.
    bool WaitInferenceIdle(DWORD timeout_ms);

    // Sets sampling parameters for local LLM generation
    void SetSamplingParams(float temp, float top_p, int top_k, float rep_pen);
    float GetTemperature() const;
    float GetTopP() const;
    int GetTopK() const;
    float GetRepetitionPenalty() const;

private:
    void RefreshActiveEngine();
    // REQ-R16: single creation point for the llama engine so the cancellation
    // flag pointer is wired exactly once per instance (caller holds mutex_).
    void EnsureLlamaEngineLocked();

    mutable std::mutex mutex_;
    EngineType preferred_type_ = EngineType::Auto;
    std::string model_path_;
    EngineType active_type_ = EngineType::GoogleTranslate;
    std::string active_name_ = "Google Translate";
    bool local_model_available_ = false;
    bool cloud_fallback_enabled_ = false;

    float temperature_ = 0.3f;
    float top_p_ = 0.6f;
    int top_k_ = 20;
    float repetition_penalty_ = 1.05f;

    struct LlamaEngine;
    std::unique_ptr<LlamaEngine> llama_engine_;

    // REQ-R16: cancellation flag shared with the LlamaEngine decode loop and
    // llama.cpp's abort/progress callbacks. Written via RequestCancel()
    // without taking mutex_ (the point is to signal a thread that IS holding
    // mutex_ inside a decode), read from the inference thread and from the
    // abort callback - std::atomic keeps all sides race-free.
    std::atomic<bool> cancel_requested_{false};
};

} // namespace emebalachat
