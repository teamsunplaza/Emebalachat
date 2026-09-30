#pragma once

// ---------------------------------------------------------------------------
// engine_core_helpers — REQ-043 (M6 T1, design §4.2/D-1): the llama-independent
// pure helpers + pins moved VERBATIM out of src/engine.hpp (bodies unchanged,
// comments kept) into the Emebalachat_engine_core static library, so the
// worker exe (T3), the host (Emebala.Engine.exe) and the test runner resolve
// them from ONE library. This is a physical move only — no logic change.
//
// Moved from engine.hpp (declarations; definitions live in
// engine_core_helpers.cpp, itself a verbatim move from engine.cpp):
//   * IsValidModelPath (M3) / ComputeFileSha256 / VerifyModelSha256 (F3)
//   * kExpectedModelSha256 / kPinnedModelFilename (F3 compile-time pin)
//   * kLlamaNCtx / kLlamaGenReserve / kLlamaTokenSafetyMargin /
//     kLlamaPromptTokenBudget (REQ-R01 budget constants)
//   * ScrubControlTokenTexts / CollectControlTokenTexts (SEC-B2)
//   * SetGpuOffloadParams (P7-F2)
// Deliberately NOT moved (kept in Emebalachat_core — conservative boundary,
// see the M6 T1 report "Issues Discovered"):
//   * TruncateHeadTailWindow — still defined in engine.cpp: google_translate.cpp
//     (a permanent Emebalachat_core member serving the Chat exe's cloud path)
//     calls it, and design §4.2 pins the post-T5 Chat exe to
//     Emebalachat_core only. Moving it would force the Chat exe to keep
//     linking Emebalachat_engine_core after T5.
//   * BuildPrompt / LocalPairReliable (config.cpp) — translation_common.hpp's
//     own header contract ("Deliberately NOT moved ... plus BuildPrompt
//     (config.cpp)") and the P2 code-surface §3 (2) keep-list pin them to
//     config.cpp; moving them would drag i18n/config link surface into
//     engine_core for zero benefit (the T3 worker links both libraries).
//
// engine.hpp re-exports this header so every existing consumer (main.cpp,
// host_main.cpp, tests/run_tests.cpp) keeps resolving these names unchanged.
// ---------------------------------------------------------------------------

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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
// kLlamaNCtx must stay in sync with cparams.n_ctx in EnsureLoaded()
// (translation_common.cpp).
//
// P2 (session 260910_0001): n_ctx raised 2048 -> 4096 per the official Tencent
// Hy-MT2-1.8B model card (recommended max_tokens=4096), which also forces the
// generation reserve to be re-tuned 512 -> 2048.
//
// 260930_0003 (가, CEO decision session 260930_0003): n_ctx raised 4096 -> 8192
// and the prompt budget 2032 -> 6128. WHY (user report): user-supplied GGUF
// models (config engine_type "user_gguf") tokenize CJK far less compactly than
// the CJK-optimized Hy-MT2 vocab, so 1-3 paragraphs that fit Hy-MT2's budget
// blew past 2032 on MiLM/Gemma-class models and had their MIDDLE discarded by
// the old head+tail shrink. The arithmetic: prompt budget = n_ctx - generation
// reserve - safety margin = 8192 - 2048 - 16 = 6128 (>= 6000 per the decision).
// llama.cpp b6099 does NOT clamp an explicit n_ctx to model metadata (warns
// only, llama-context.cpp), so the 8192 window loads on every model.
//
// KV-cache cost (MEASURED from build/models/Hy-MT2-1.8B-Q8_0.gguf in the F10
// audit, docs/260908_0002 report: hunyuan-dense, block_count=32, GQA
// head_count_kv=4, head_dim=128, f16 KV):
//   2 (K+V) x 32 layers x 4 kv_heads x 128 dims x 2 B = 64 KiB/token
//   n_ctx 8192 -> 512 MiB KV total (+256 MiB vs the old 4096 window).
//   With flash_attn enabled (EnsureLoaded) GPU residency is lower still.
//   Safe for the Q8_0 1.8B (~2.0 GB weights) on typical 8 GB+ machines.
//
// Generation split trade-off: the card's max_tokens=4096 API ceiling is the
// model's recommended single-response output size, not the context size. At
// the current n_ctx 8192 a 4096-token generation fits comfortably (prompt
// budget 6128 leaves 2048+ tokens of room even at full prompt), so the old
// "physically impossible inside a 4096-token window" argument only described
// the pre-260930_0003 window. kLlamaGenReserve=2048 stays the named ceiling
// for the input-scaled generation cap (ScaledMaxGenTokens); with n_ctx 8192
// the prompt budget (6128) is now 3x the reserve, so long-input translations
// keep a full 2048-token output room. RT-C (260926_0003): the worker VRAM
// auto gate keys off the FREE local VRAM measured at worker start
// (kMtGpuFreeVramThresholdBytes below), NOT off any context-size constant, so
// the n_ctx doubling changes no gate arithmetic - a tighter card simply keeps
// routing to the CPU leg as before.
inline constexpr int kLlamaNCtx = 8192;
inline constexpr int kLlamaGenReserve = 2048;   // tokens reserved for generation (max_gen_tokens)
inline constexpr int kLlamaTokenSafetyMargin = 16;
// 260930_0003 (가): 8192 - 2048 - 16 = 6128 (>= 6000 prompt tokens fit).
inline constexpr int kLlamaPromptTokenBudget =
    kLlamaNCtx - kLlamaGenReserve - kLlamaTokenSafetyMargin; // 6128

// SEC-B2 (session 260911_0002, verify 233020): pure control-token scrub.
// Removes EVERY occurrence of every token text in `tokens` from `text`,
// re-scanning from the beginning after each removal until a full pass finds
// no match (bounded at 32 iterations for pathologically adversarial input).
// The restart guarantees convergence against split-token reassembly: deleting
// one marker can splice surrounding fragments into a NEW marker
// (e.g. "<｜hy_<｜hy_User｜>User｜>" collapses to "<｜hy_User｜>" after one
// naive erase), and a single linear pass would miss it.
// `tokens` is consumed verbatim: exact, case-sensitive substring matches only;
// empty token strings are ignored. The function cannot fail except on
// allocation failure (std::bad_alloc, which unwinds the translation request):
// there is no fallback-to-unsanitized path by design (verify report §6 - a
// scrub failure must never restore the injection channel).
// The engine wires it to the ACTIVE model's vocab via
// CollectControlTokenTexts below (never a hardcoded list, so a user-selected
// alternative GGUF is protected by its own vocabulary).
std::wstring ScrubControlTokenTexts(std::wstring_view text, const std::vector<std::wstring>& tokens);

// RT-C (260926_0003, task RT-C "D안-개선"): translation-worker VRAM auto gate.
// WHY: the shared engine host serves BOTH the ASR family (Listener-owned,
// VRAM-hungry) and this MT family from one GPU. Live defect (RTX 2070 8 GB
// class, the "개발도상국 수준 데스크탑" mission target): the translate
// worker's unconditional GPU offload (~2.8 GB) evicted the ASR worker into
// its CPU fallback (~12x slower captions). The gate decides the FIRST
// EnsureLoaded leg from the FREE local VRAM left at worker start instead of
// hard-pinning either backend:
//   free local VRAM >= kMtGpuFreeVramThresholdBytes -> GPU offload (unchanged)
//   below threshold, or not measurable               -> CPU leg directly
//     (no CUDA context is initialized, so the ASR budget is never touched)
// Manual override env var EMEBALA_MT_GPU: "1" forces GPU, "0" forces CPU,
// unset/empty/anything else -> auto. The orchestrator spawns the worker with
// lpEnvironment=nullptr (host_v2_worker_manager.cpp LaunchWorkerProcess), so
// the variable inherits from the host process / user environment.
// VRAM measurement: DXGI IDXGIAdapter3::QueryVideoMemoryInfo (WDDM local
// memory Budget - CurrentUsage = how much more THIS process can still
// commit; reflects every other process's usage, vendor-agnostic CUDA/Vulkan,
// and never initializes a CUDA context as a side effect). dxgi.lib is
// already on every engine_core consumer's link line (Emebalachat_core PUBLIC
// set, CMakeLists "Win32 system dependencies") — NO new dependency.
// Driverless machines: only the software basic render driver enumerates, so
// the query finds no candidate adapter and reports failure -> CPU, exactly
// the historical CPU-fallback contract (P5-F1 / EnsureLoaded retry parity).
// Win10 builds older than 1607 (DXGI < 1.4) likewise fail the
// IDXGIAdapter3 QueryInterface -> CPU; RTX-class mission machines are all
// 1607+. The gate is evaluated ONCE per process (worker start = first
// EnsureLoaded) and cached.
inline constexpr unsigned long long kMtGpuFreeVramThresholdBytes = 1536ull * 1024 * 1024; // 1.5 GiB floor

// RT-C: parsed EMEBALA_MT_GPU states. Anything outside {"0","1"} (unset,
// empty, typo) degrades to Auto — a malformed override must never hard-lock
// a machine into the "wrong" backend silently.
inline constexpr int kMtGpuOverrideAuto = -1;
inline constexpr int kMtGpuOverrideForceCpu = 0;
inline constexpr int kMtGpuOverrideForceGpu = 1;

// RT-C: pure EMEBALA_MT_GPU parse. Exact match only ("0"/"1"); every other
// shape ("" included) -> kMtGpuOverrideAuto. Unit-pinned by
// TestRtCMtGpuVramGate (tests/run_tests.cpp).
constexpr int ParseMtGpuOverride(std::wstring_view value) {
    if (value == L"0") {
        return kMtGpuOverrideForceCpu;
    }
    if (value == L"1") {
        return kMtGpuOverrideForceGpu;
    }
    return kMtGpuOverrideAuto;
}

// RT-C: pure offload decision (unit-pinned by TestRtCMtGpuVramGate).
// Precedence: explicit override wins over everything (forcing GPU on a
// driverless machine still lands on CPU through the existing
// CUDA-load-failure -> CPU retry, so the override can stay unconditional);
// then auto: unmeasurable VRAM -> CPU (historical fallback contract), free
// >= threshold -> GPU, free < threshold -> CPU.
constexpr bool DecideMtGpuOffload(int override_value, bool vram_query_ok,
                                  unsigned long long free_vram_bytes) {
    if (override_value == kMtGpuOverrideForceCpu) {
        return false;
    }
    if (override_value == kMtGpuOverrideForceGpu) {
        return true;
    }
    if (!vram_query_ok) {
        return false;
    }
    return free_vram_bytes >= kMtGpuFreeVramThresholdBytes;
}

// RT-C: DXGI-backed free-local-VRAM probe for the adapter the inference
// backend would actually use (max DedicatedVideoMemory among non-software
// adapters — the discrete card, i.e. ggml device 0 in practice). Returns
// true only when a real hardware adapter reported its WDDM local-memory
// budget; out_free_bytes is then Budget - CurrentUsage (clamped at 0).
// Failure shapes (no factory, no hardware adapter, DXGI < 1.4,
// QueryVideoMemoryInfo error) all return false -> the caller gates to CPU.
// Deliberately quiet on failure: a driverless machine is the NORMAL CPU
// path, not an error. Shape-only decision logging lives at the caller.
bool QueryMtAdapterFreeVramBytes(unsigned long long& out_free_bytes);

#ifdef HAVE_LLAMA_CPP
// SEC-B2: enumerate the scrub set from a loaded llama.cpp vocab. Returns the
// text of every token whose attr has CONTROL or USER_DEFINED or UNKNOWN set -
// exactly the class llama-vocab's tokenizer_st_partition substring-matches
// when parse_special=true (build/_deps/llama_cpp-src/src/llama-vocab.cpp
// L2396-2411 cache_special_tokens build; b6099 public APIs llama_vocab_n_tokens
// L496 / llama_vocab_get_attr L1003 / llama_vocab_get_text L999). UNKNOWN is
// included because llama.cpp partitions those too.
std::vector<std::wstring> CollectControlTokenTexts(const llama_vocab* vocab);
#endif

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

} // namespace emebalachat
