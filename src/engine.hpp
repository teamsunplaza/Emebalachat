#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector> // SEC-B2: ScrubControlTokenTexts / CollectControlTokenTexts signatures
#include <windows.h> // DWORD for WaitInferenceIdle

// REQ-043: EngineHostConfig (the persisted "engine_host" block) and the
// engine_host::TryTranslate client seam. The header is self-contained
// (Win32 + STL only) so the other Emebala workspaces can copy it verbatim.
#include "engine_host_client.hpp"

// REQ-045 P4-3 (design §3b, item 3b): the persisted OpenAI Compatible block
// (OpenAiConfig) referenced by SetOpenAiConfig / the openai_config_ member.
#include "openai_compatible_client.hpp"

// REQ-043 (M6 T1, design §4.2/D-1): the llama-independent pure helpers and
// pins (IsValidModelPath, ComputeFileSha256/VerifyModelSha256 + marker cache
// contract, kExpectedModelSha256/kPinnedModelFilename, kLlama* budget
// constants, Scrub/CollectControlTokenTexts, SetGpuOffloadParams) moved
// VERBATIM into the Emebalachat_engine_core static library (physical move,
// no logic change). engine.hpp re-exports the header so every existing
// consumer keeps resolving the names unchanged.
#include "engine_core/engine_core_helpers.hpp"

// REQ-043 (M6 T5, plan §V2-8.6): the direct llama.h dependency is RETIRED
// with the embedded engine. llama.h/HAVE_LLAMA_CPP still reach consumers
// through the re-exported engine_core header below (its own guarded
// include), so the engine_core-linked targets (host / worker / test runner)
// keep resolving the P7-F2 seam; the Chat exe (post-T5: no engine_core
// link, CMake T5) compiles llama-free.

namespace emebalachat {

// REQ-R01: pure, model-independent head+tail sliding-window truncation of source
// text. Returns `text` unchanged when it already fits (2 * keep_per_side >= size).
// Otherwise keeps the first and last `keep_per_side` UTF-16 code units joined by
// "\n…\n". Cut points are shifted inward so a UTF-16 surrogate pair is never
// split (a lone surrogate would corrupt the UTF-8 conversion and the tokenizer).
// Exposed for unit testing; the engine drives it via a token-count binary search.
// REQ-043 (M6 T1): the DEFINITION stays in engine.cpp (Emebalachat_core) —
// google_translate.cpp (a permanent core member, Chat exe cloud path) calls it,
// so moving it into engine_core would pin the post-T5 Chat exe to engine_core.
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
    // REQ-029-B / REQ-043 (M6 T5): preferred=LocalLlama but NO local source
    // can serve (the shared host is absent or disabled; the embedded model
    // was removed), so translation never even started. The cause is distinct from
    // CloudConsentBlocked (the H2 consent gate refusing an allowed cloud
    // fallback), so logs/UI/sound can tell the user precisely "model
    // missing" instead of the old masquerade that reported Google/cloud and
    // then failed silently. Appended AFTER Canceled: existing values keep
    // their integer order (0..4), so log-based diagnostic scripts that
    // static_cast<int>(status) stay compatible.
    LocalModelMissing,
};

enum class EngineType {
    // Auto (REQ-R02 policy, documented semantics restored): the local source
    // (REQ-043 M6 T5: the shared inference host) when it can serve;
    // otherwise, AND when a local serving attempt fails, the cloud Google
    // Translate fallback is used even if cloud_fallback_enabled is false.
    // Choosing engine_type=auto IS the consent to the documented "seamless
    // Google Translate fallback" contract below; cloud_fallback_enabled no
    // longer gates it.
    Auto,
    GoogleTranslate,// 100% Free, zero-install, zero-API-key Google Translate via native WinHTTP
    // Strict local: typed text stays on-device. Cloud is used ONLY as fallback after
    // a local failure when the user explicitly enabled cloud_fallback_enabled (H2 gate).
    // REQ-043 (M6 T5) SEMANTIC SHIFT, enum value kept for config.json
    // compatibility: "local" now means "serve through the shared inference
    // host (Emebala.Engine.exe)" — the embedded llama.cpp engine was removed
    // from the Chat app (plan §V2-8.6). The config string
    // engine_type="local" is unchanged; only the serving source moved.
    LocalLlama,
    // REQ-045 P4-3 (design §3b, item 3b): OpenAI Compatible cloud engine.
    // User-supplied base URL + API key; deliberately picking "openai" IS the
    // consent (the user wires their OWN credentials), distinct from the
    // google_consent/cloud_fallback H2 gate. Appended AFTER LocalLlama so the
    // existing values keep their integer order (0..2).
    OpenAi
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

// ---- REQ-051 (session 260921, symptom B-1): local transient-failure retry --
//
// The user-reported 'sometimes works, sometimes shows the 로컬번역을 사용할 수
// 없습니다 modal' class: a TRANSIENT engine-host failure (pipe connect racing
// the orchestrator's spawn, worker busy/respawning) surfaced through
// CloudConsentBlocked with NO retry. Classification and the retry warrant
// are ONE pure definition (shared by engine.cpp and the unit tests):
//   * Transient = "connect" (orchestrator still booting / spawn in flight),
//     "busy" (the worker manager refused a job while loading), "io" (the
//     pipe died mid-request; the NEXT call respawns), "timeout" (worker-side
//     decode deadline). These are the codes the frozen client's failure
//     contract (engine_host_client.hpp) documents as recoverable-by-retry.
//   * Everything else is Permanent: "disabled", "no_host_binary",
//     "unauthorized", "version_mismatch", "pin_mismatch", "bad_request",
//     "model_missing" and "engine_failed" (a decode that actually ran -
//     retrying a deterministic decode failure only reloads the model).
//     UNKNOWN / empty codes fail PERMANENT (fail-safe: never spend a retry
//     on a code the policy does not recognize).
enum class EngineHostFailureClass { Transient, Permanent };
constexpr EngineHostFailureClass ClassifyEngineHostFailure(std::string_view err_code) {
    return (err_code == "connect" || err_code == "busy" || err_code == "io" ||
            err_code == "timeout")
               ? EngineHostFailureClass::Transient
               : EngineHostFailureClass::Permanent;
}

// One identical-input retry on a transient local failure, inside the existing
// request machinery: ~400 ms backoff (the handoff's 300-500 ms band), bounded
// by kEngineHostTransientRetryMax (ONE retry - never a loop), and gated on
// the FIRST attempt having failed FAST (<= the ceiling): the second attempt
// rides the client's own 30 s budget, so a retry is only warranted when the
// pair stays inside the established request-time envelope - a failure that
// already burned the full 30 s (e.g. a cold model load on the user's 2.8 GB
// user_gguf) converges to the honest modal instead of a second 30 s wait.
// The REQ-R16 latch outranks everything: a cancel intent never retries and
// never starts a second pipe request.
inline constexpr int kEngineHostTransientRetryMax = 1;
inline constexpr uint32_t kEngineHostTransientRetryBackoffMs = 400;
inline constexpr uint64_t kEngineHostRetryFirstAttemptCeilingMs = 10000;
constexpr bool EngineHostTransientRetryWarranted(std::string_view err_code,
                                                 int retries_so_far,
                                                 uint64_t first_attempt_elapsed_ms,
                                                 bool cancel_requested) {
    return !cancel_requested &&
           ClassifyEngineHostFailure(err_code) == EngineHostFailureClass::Transient &&
           retries_so_far < kEngineHostTransientRetryMax &&
           first_attempt_elapsed_ms <= kEngineHostRetryFirstAttemptCeilingMs;
}

// REQ-051 (symptom B-1) ABSOLUTE SECURITY BOUNDARY: the retry re-runs ONLY
// the local engine-host leg (engine_host::TryTranslate). The cloud/openai
// call lambdas below are never invoked by the retry path itself; under
// engine_type=local / user_gguf with cloud_fallback_enabled=false an
// exhausted retry still converges to CloudConsentBlocked with the text
// staying on-device - the policy can delay an already-consented cloud leg
// (Auto / explicit fallback) but can NEVER newly trigger one. The unit tests
// pin both this predicate matrix and the engine.cpp source structure.

// REQ-043 (M6 T5, design §5 (1)): the ShouldPreloadLocalModel /
// ShouldPreloadOnEngineSwitch preload seams and the
// MigrateLegacyDefaultModelPath migration helper are REMOVED with the
// embedded engine. There is no resident model to warm up (startup or tray
// switch — the shared host spawns its worker on demand), and the model path
// is a legacy config field: the local source lives at the installer-owned
// fixed common location, so nothing is "followed" anymore.

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

    // REQ-043 (plan §5.1-2; M6 T5): pushes the persisted engine_host config
    // block into the manager (main.cpp applies it right after LoadFromFile,
    // the I3 pattern). Translate() consults it at the local-serving seam:
    // when enabled, the shared inference host IS the local engine; on host
    // failure the §V2-8.6 UX chain applies (cloud when consented, honest
    // failure otherwise — there is no embedded fallback anymore).
    void SetEngineHostConfig(const EngineHostConfig& cfg);

    // REQ-045 P4-3 (design §3b, item 3b): pushes the persisted OpenAI
    // Compatible block (base_url / model / DPAPI key blob + integrity digest /
    // http consent) into the manager. main.cpp applies it right after
    // LoadFromFile (the I3 pattern, same as SetEngineHostConfig). Translate()
    // consults it when preferred_type_ == EngineType::OpenAi; the user
    // selecting "openai" IS the consent (their own credentials), distinct
    // from the google_consent/cloud_fallback H2 gate.
    void SetOpenAiConfig(const OpenAiConfig& cfg);

    // Returns user-facing name of the currently active engine
    std::string GetActiveEngineName() const;

    // REQ-043 (M6 T5) SEMANTIC SHIFT: returns true when a local source can
    // serve — i.e. the shared inference host is deployed and enabled. The
    // name and signature are kept for callers/config compatibility; there is
    // no embedded model file to check anymore.
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

    // REQ-043 (M6 T5, plan §V2-8.6 failure UX): CLIENT-INTERNAL repair
    // signal for the T6 bootstrapper UI — the number of consecutive
    // engine-host failures observed by Translate(). Zeroed on every host
    // success. Deliberately NOT a TranslationStatus and NEVER a protocol
    // status: the wire status enum is frozen (§V2-4.5) and must not grow
    // client-UI states. The spawn leg of the UX chain lives inside the
    // engine-host client (cfg.spawn); this counter covers the "failure
    // persists" leg that the one-click repair flow consumes.
    int HostFailureStreak() const;

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

    // REQ-043 (M6 T5): PreloadLocalModel/UnloadLocalModel are REMOVED with
    // the embedded engine (nothing resident to load/unload — the shared
    // host's worker process owns the model lifecycle).

    // ---- REQ-R16 (audit §5 latent item 4): shutdown latch ----
    //
    // Requests cancellation of any in-flight or queued Translate(). REQ-043
    // (M6 T5): with the embedded engine gone there is no llama decode to
    // unwind — the latch now guards the surviving engines: an in-flight or
    // queued cloud WinHTTP request and an in-flight engine-host pipe request
    // short-circuit to Canceled instead of starting work that could transmit
    // user text after an exit intent. Safe to call at any time from any
    // thread. SHUTDOWN LATCH SEMANTICS: once requested, every subsequent
    // Translate() also short-circuits (the flag is only cleared when a
    // NEW manager is constructed) - this is deliberate: the seam exists to
    // drain requests before process exit, not to pause/resume translation.
    void RequestCancel();

    // True while a cancellation request is pending.
    // Exposed for the shutdown watchdog and the headless seam tests.
    bool IsCancelRequested();

    // REQ-R16 watchdog helper: returns true once the engine mutex is free
    // again, i.e. every in-flight Translate() (cloud WinHTTP or engine-host
    // pipe — the mutex is held across the whole call) has unwound. Call
    // after RequestCancel() to get a BOUNDED, observable confirmation that
    // no request is still in flight before the process exits.
    bool WaitInferenceIdle(DWORD timeout_ms);

    // Sets sampling parameters for local LLM generation
    void SetSamplingParams(float temp, float top_p, int top_k, float rep_pen);
    float GetTemperature() const;
    float GetTopP() const;
    int GetTopK() const;
    float GetRepetitionPenalty() const;

private:
    void RefreshActiveEngine();
    // REQ-043 (plan §5.1-2; M6 T5): the shared inference host is THE local
    // serving source (the embedded engine was removed). Availability =
    // engine_host.enabled + the fixed host binary present (cheap existence
    // check via the self-contained client). Caller holds mutex_. The former
    // HAVE_LLAMA_CPP guard is REMOVED (design §5 (1): closes the P2 §3(3)
    // hazard where a llama-free Chat build lost its local source entirely —
    // a no-llama Chat build now keeps host routing; only the worker carries
    // llama).
    bool HostCouldServeLocked() const;

    mutable std::mutex mutex_;
    EngineType preferred_type_ = EngineType::Auto;
    std::string model_path_;
    EngineType active_type_ = EngineType::GoogleTranslate;
    std::string active_name_ = "Google Translate";
    bool local_model_available_ = false;
    bool cloud_fallback_enabled_ = false;
    // REQ-043: engine_host.enabled gates the host-first routing inside
    // Translate(); the block is a startup-only write (like
    // cloud_fallback_enabled_) so it needs no Snapshot entry.
    EngineHostConfig engine_host_config_;

    // REQ-045 P4-3 (design §3b): the persisted OpenAI Compatible block that
    // drives the OpenAi serving leg of Translate(). Startup-only write (same
    // discipline as engine_host_config_), so it needs no Snapshot entry.
    OpenAiConfig openai_config_;

    float temperature_ = 0.0f;
    float top_p_ = 0.6f;
    int top_k_ = 20;
    float repetition_penalty_ = 1.05f;

    // REQ-043 (M6 T5): consecutive engine-host failures (the repair signal
    // getter above); guarded by mutex_ like the rest of the manager state.
    // The TranslationManager::LlamaEngine member is REMOVED with the
    // embedded engine.
    int host_fail_streak_ = 0;

    // REQ-R16: shutdown latch. Written via RequestCancel() without taking
    // mutex_ (the point is to signal a thread that IS holding mutex_ inside
    // a request), read by every Translate() leg - std::atomic keeps all
    // sides race-free.
    std::atomic<bool> cancel_requested_{false};
};

} // namespace emebalachat
