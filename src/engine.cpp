#include "engine.hpp"
#include "diag_logger.hpp"
#include "config.hpp"
#include "google_translate.hpp"
#include "openai_compatible_client.hpp" // REQ-045 P4-3: OpenAI Compatible cloud engine
#include "unicode_utils.hpp"
#include "engine_core/translation_common.hpp" // REQ-043: LocalInferenceEngine (M6 T1 move; M6 T5: consumed by the worker exe only)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>

namespace emebalachat {

// REQ-043 (M6 T5, design §5 (1), plan §V2-8.6): the EMBEDDED inference path
// is REMOVED from the Chat app. The former TranslationManager::LlamaEngine
// shim (a zero-member derivation of engine_core's LocalInferenceEngine), the
// embedded call block, the preload seams and the legacy model-path migration
// are all gone. The local source is now EXCLUSIVELY the shared inference
// host (Emebala.Engine.exe via engine_host::TryTranslate); llama.cpp is
// linked only by the worker exe (Emebalachat.Engine.ggml-translate.exe) and
// the engine_core library it consumes. The Chat exe links Emebalachat_core
// only (CMake T5) and carries no llama symbol.
//
// TruncateHeadTailWindow's definition deliberately STAYS in this file
// (Emebalachat_core): google_translate.cpp (a permanent core member serving
// the Chat exe's cloud path) calls it, so moving it would pin the Chat exe
// to engine_core. BuildPrompt/LocalPairReliable stay in config.cpp per
// translation_common.hpp's own header contract.

// REQ-R01 (Batch D1): pure, model-independent head+tail sliding-window truncation.
// Keeps the first and last `keep_per_side` UTF-16 code units joined by "\n...\n"
// (U+2026 ellipsis). Cut points are adjusted so a UTF-16 surrogate pair is never
// split - a lone surrogate would corrupt the UTF-8 conversion and the tokenizer.
// Returns the text unchanged when it already fits (text.size() <= 2*keep_per_side).
// Unit-testable without any model; retained verbatim (REQ-R01) so the cloud
// path and the config-side budget seams keep their proven helper.
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

// I3 fix: the constructor no longer performs its own config.json disk load.
// Previously it created a shadow AppConfig and re-read the file, so every start
// parsed the config twice and a locked/malformed file could give the engine
// different values than the caller (single-source-of-truth violation).
// main.cpp owns the one LoadFromFile() call and pushes the loaded values in via
// the existing public setters (SetSamplingParams / SetCloudFallbackEnabled)
// AFTER construction; the constructor here keeps pure in-class defaults
// (0.0 / 0.6 / 20 / 1.05 - identical to AppConfig's defaults) so a
// stand-alone constructed manager still behaves as before for tests.
//
// REQ-043 (M6 T5): the model_path_ field is retained as a legacy config
// passthrough (GetModelPath callers) but drives NO serving decision anymore —
// the local source is the shared host, not a file. The former migration of a
// legacy exe-relative default to the common location is removed with the
// embedded engine: the model path is owned by the installer (T7) at the fixed
// common location.
TranslationManager::TranslationManager(EngineType preferred_type, std::string model_path)
    : preferred_type_(preferred_type), model_path_(std::move(model_path)) {
    if (model_path_.empty()) {
        // D5 item F (historical hardening, kept): anchor an empty default to
        // the EXECUTABLE directory instead of the process CWD (Run-registry
        // autostart -> System32). Pure path arithmetic, still no file I/O.
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
    // REQ-043 (M6 T5): legacy config passthrough only (no migration, no
    // serving decision). RefreshActiveEngine stays a no-op for the path, but
    // the call is kept so future path-aware behavior has one entry point.
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
    // REQ-043 (M6 T5) SEMANTIC SHIFT: "a local source can serve" = the shared
    // host is enabled + deployed. The embedded model file no longer exists.
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

// REQ-043 (M6 T5): consecutive engine-host failures — the T6 bootstrapper's
// one-click-repair signal. Client-internal only (see the header contract).
int TranslationManager::HostFailureStreak() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return host_fail_streak_;
}

// REQ-043: main.cpp pushes the persisted engine_host block through this
// setter right after LoadFromFile (the I3 single-source-of-truth pattern used
// by SetSamplingParams / SetCloudFallbackEnabled above).
void TranslationManager::SetEngineHostConfig(const EngineHostConfig& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    engine_host_config_ = cfg;
    // REQ-043: enabled is a routing input (HostCouldServeLocked) — re-evaluate
    // the active engine so toggling the block flips availability immediately
    // instead of after an unrelated refresh.
    RefreshActiveEngine();
}

// REQ-045 P4-3 (design §3b, item 3b): pushes the persisted OpenAI Compatible
// block into the manager. Startup-only write (main.cpp applies it after
// LoadFromFile); re-evaluates the active engine so selecting OpenAi flips the
// displayed engine name immediately.
void TranslationManager::SetOpenAiConfig(const OpenAiConfig& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    openai_config_ = cfg;
    RefreshActiveEngine();
}

// REQ-051 U-2: pushes the user-model (.gguf) display stem ("" clears the
// route back to the pinned Hy-MT2 naming). Same GUI-thread-only startup/
// runtime discipline as SetEngineType; re-evaluates the active name so the
// tooltip/log surface flips immediately.
void TranslationManager::SetUserModelDisplayStem(std::string_view stem) {
    std::lock_guard<std::mutex> lock(mutex_);
    user_model_stem_ = std::string(stem);
    RefreshActiveEngine();
}

// REQ-R16: shutdown latch — deliberately NOT taking mutex_: an in-flight
// Translate() (cloud WinHTTP or engine-host pipe) holds it across the whole
// request; the atomic store is the signal that call observes.
void TranslationManager::RequestCancel() {
    cancel_requested_.store(true, std::memory_order_release);
}

bool TranslationManager::IsCancelRequested() {
    return cancel_requested_.load(std::memory_order_acquire);
}

// REQ-R16 bounded drain: Translate() (and the Auto->cloud fallback) hold mutex_
// for their whole duration, so acquiring it proves no request is in flight.
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

// REQ-043 (M6 T5): the host is the ONLY local source. Availability =
// engine_host.enabled + the fixed host binary present (cheap existence check
// via the self-contained client). The former HAVE_LLAMA_CPP guard is REMOVED
// (design §5 (1): a llama-free Chat build must NOT lose its local source —
// that was the P2 §3(3) hazard; llama now lives only in the worker exe and
// the engine_core library it links). Every runtime failure still converges
// through the §V2-8.6 failure UX below.
bool TranslationManager::HostCouldServeLocked() const {
    return engine_host_config_.enabled && engine_host::IsHostBinaryPresent();
}

void TranslationManager::RefreshActiveEngine() {
    std::error_code ec;
    // REQ-043 (M6 T5) SEMANTIC SHIFT: local_model_available_ no longer probes
    // a model FILE — it mirrors HostCouldServeLocked() ("the local source,
    // the shared host, can serve"). RefreshActiveEngine's file-existence
    // condition is replaced by the host gate (design §5 (3) row 2).
    local_model_available_ = HostCouldServeLocked();
    const bool host_available = local_model_available_;  // REQ-043 (M6 T5)

    if (preferred_type_ == EngineType::GoogleTranslate) {
        active_type_ = EngineType::GoogleTranslate;
        active_name_ = "Google Translate (Cloud)";
    } else if (preferred_type_ == EngineType::OpenAi) {
        // REQ-045 P4-3 (design §3b): OpenAI Compatible cloud engine. Always
        // active (the request itself may still fail — surfaced via
        // EngineFailed, never a silent masquerade).
        active_type_ = EngineType::OpenAi;
        active_name_ = "OpenAI Compatible (Cloud)";
    } else if (preferred_type_ == EngineType::LocalLlama) {
        // REQ-043 (M6 T5): "local" = the shared inference host. No embedded
        // model file exists, so the former (model present -> embedded / host)
        // split collapses to a single host-serving leg.
        if (host_available) {
            active_type_ = EngineType::LocalLlama;
            // REQ-051 U-2: the user-model (.gguf) route gets a DISTINCT,
            // config-derived display name (the model file stem — active_name_
            // is UI-visible through the tooltip and the pipeline/translate
            // diagnostic lines, so the pinned Hy-MT2 name must not masquerade
            // as the user's model). No new hardcoded English + no i18n
            // strings: the bare stem reads correctly in every locale.
            active_name_ = user_model_stem_.empty() ? "Hy-MT2-1.8B (Shared Host)"
                                                    : user_model_stem_;
        } else {
            // REQ-029-B honesty preserved: a strict-local pick with NO local
            // source stays honest — active_type_ remains LocalLlama
            // (Translate() pre-blocks with LocalModelMissing before any cloud
            // seam) and the display name carries no "Google" substring, so
            // the tray checkmark side-effect (find("Google") on this string)
            // stays eliminated.
            active_type_ = EngineType::LocalLlama;
            active_name_ = "Local (Model Missing)";
        }
    } else {
        // EngineType::Auto
        if (host_available) {
            // REQ-043: under Auto a deployed host keeps local-first routing
            // — otherwise every translation would silently go to Google.
            // The pinned Hy-MT2 name stays UNCONDITIONAL here: the host's C1
            // gate serves the pinned model for engine_type=auto, so the
            // user-model stem (a LocalLlama-route display input) must not
            // leak into the Auto leg.
            active_type_ = EngineType::LocalLlama;
            active_name_ = "Hy-MT2-1.8B (Shared Host)";
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
// tested one. REQ-043 (M6 T5): the enum VALUES are unchanged — LocalLlama
// now means "serve through the shared host" — so this seam is untouched.
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

// REQ-R02 (Batch D1, audit §2.1 / §5-C4): every path that produces no
// translation reports a TranslationStatus the worker can react to (error
// tone / tooltip), and the Auto policy is restored to its documented
// semantics.
//
// What Auto means NOW (REQ-043, M6 T5):
//   engine_type=auto = "use the shared inference host when it can serve;
//   seamlessly fall back to Google Translate when the host is absent OR a
//   host serving attempt fails". Selecting auto in config IS the user's
//   consent to that documented cloud fallback, so cloud_fallback_enabled_
//   does NOT gate the Auto->cloud paths.
//   engine_type=local stays strictly on-device: after a host failure the
//   cloud is used only when the user explicitly enabled cloud_fallback_enabled_.
//
// §V2-8.6 FAILURE UX (convergence chain, no silent failure, no crash):
//   (a) the engine-host client's spawn leg runs first (cfg.spawn);
//   (b) on persistent failure the manager records the repair signal
//       (host_fail_streak_ — consumed by the T6 bootstrapper UI);
//   (c) cloud path when consented (Auto contract, or explicit-local with the
//       H2 gate enabled);
//   (d) no local source + no consent = the honest strict-local failure
//       (LocalModelMissing / CloudConsentBlocked) — the "feature disabled"
//       guidance state.
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

    // REQ-029-B / REQ-043 (M6 T5): a local pin with NO local source must
    // never leak to the cloud. Pre-block here, BEFORE the 040 routing log
    // and every cloud seam (the historical REQ-029-B masquerade guard).
    if (preferred_type_ == EngineType::LocalLlama && !HostCouldServeLocked()) {
        // REQ-R16 latch outranks this guard: an exit-intent must surface as
        // Canceled, never as an audible failure.
        if (cancel_requested_.load(std::memory_order_acquire)) {
            set_status(TranslationStatus::Canceled);
            return {};
        }
        DIAG_F("ENGINE/Translate/043: preferred=local but no local source (host absent/disabled); "
               "refusing cloud masquerade (legacy model_path=%s)\n", model_path_.c_str());
        set_status(TranslationStatus::LocalModelMissing);
        return {};
    }

    // R6 Phase 4 (B2, plan §4.1 item 3): pair routing, decided HERE (before the
    // 040 line) so the observability log reports the engine that will ACTUALLY
    // serve the request. Meaningful only while the local source is active;
    // availability itself stays RefreshActiveEngine's job.
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

    // REQ-045 P4-3 (design §3b, item 3b): OpenAI Compatible seam. Selecting
    // engine_type=openai IS the consent (the user wires their OWN credentials),
    // so — unlike the google cloud seam — this leg is NOT gated by the H2
    // cloud_fallback_enabled/google_consent gate. A request that produces
    // nothing records EngineFailed exactly like the google leg; the failure is
    // surfaced as an OpenAI failure, never masqueraded as a local/google one.
    auto openai_call = [&]() -> std::wstring {
        std::wstring res = OpenAiCompatibleClient::ChatCompletion(
            openai_config_, src_code_or_name, tgt_code_or_name, text);
        set_status(res.empty() ? TranslationStatus::EngineFailed : TranslationStatus::Ok);
        return res;
    };

    // REQ-045 P4-3: OpenAI explicit pick short-circuits every other leg — the
    // user's deliberate engine choice is honored verbatim (REQ-R16 latch
    // first so an exit intent never starts a new WinHTTP request).
    if (preferred_type_ == EngineType::OpenAi) {
        if (cancel_requested_.load(std::memory_order_acquire)) {
            set_status(TranslationStatus::Canceled);
            return {};
        }
        DIAG_F("ENGINE/Translate/045: served=openai (base_url set=%d, model set=%d)\n",
               openai_config_.base_url.empty() ? 0 : 1,
               openai_config_.model.empty() ? 0 : 1);
        return openai_call();
    }

    // REQ-043 (M6 T5): THE local seam — the shared inference host is the only
    // local source. Requests the host serves reach this leg (the 041
    // cloud-routing leg returns below for unsupported pairs). The client
    // enforces the §4.4 contract (pin/version/token checks, busy, timeout)
    // and never logs user text.
    if (served_engine == EngineType::LocalLlama && engine_host_config_.enabled) {
        // REQ-R16: cancellation short-circuit BEFORE the pipe is touched (a
        // shutdown posted RequestCancel() while this call queued on mutex_).
        if (cancel_requested_.load(std::memory_order_acquire)) {
            set_status(TranslationStatus::Canceled);
            return {};
        }
        if (preferred_type_ == EngineType::LocalLlama &&
            !LocalPairReliable(src_code_or_name, tgt_code_or_name)) {
            // Plan §4.1: explicit local pin + no cloud consent keeps the
            // request on-device; warn that the output language may degrade.
            DIAG_F(
                    "ENGINE/Translate/042: pair (%s -> %s) outside the Hy-MT2 reliable set but strict-local pin without cloud consent; staying local (output may degrade)\n",
                    norm_src.c_str(), norm_tgt.c_str());
        }
        std::string host_out;
        std::string host_err;
        const auto t_local_attempt = std::chrono::steady_clock::now();
        if (engine_host::TryTranslate(engine_host_config_,
                                      std::string(src_code_or_name), tgt_name,
                                      ToUtf8(text), host_out, host_err)) {
            host_fail_streak_ = 0;  // §V2-8.6: success clears the repair signal
            set_status(TranslationStatus::Ok);
            return ToUtf16(host_out);
        }
        // §V2-8.6 (b): persistent host failure — record the repair signal for
        // the T6 bootstrapper (client-internal state, never a protocol
        // status), then converge through the consented fallback below.
        ++host_fail_streak_;
        DIAG_F("ENGINE/Translate/044: engine-host try failed (code=%s, fail_streak=%d); converging the §V2-8.6 UX chain\n",
               host_err.c_str(), host_fail_streak_);
        // REQ-051 (session 260921, symptom B-1): ONE identical-input retry on
        // a TRANSIENT fast failure (connect racing the orchestrator's spawn,
        // busy/respawning worker, pipe died mid-request). The warrant is the
        // pure policy in engine.hpp: transient code, retry budget unspent,
        // the first attempt failed inside the fast-failure ceiling (so the
        // pair stays inside the established request-time envelope - a 30 s
        // cold-load timeout converges to the honest modal instead of a
        // second half-minute wait), and no REQ-R16 cancel intent. The retry
        // re-runs ONLY the local leg: the consent gates below are untouched,
        // so under a strict-local pick with cloud_fallback_enabled=false the
        // text still never leaves the device, and under Auto / explicit
        // consent the already-consented cloud leg is reached exactly as
        // before, only delayed by ~400 ms of local-first recovery.
        const uint64_t first_attempt_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_local_attempt).count());
        if (EngineHostTransientRetryWarranted(host_err, /*retries_so_far=*/0,
                                              first_attempt_ms,
                                              cancel_requested_.load(std::memory_order_acquire))) {
            DIAG_F("ENGINE/Translate/046: transient local failure (code=%s, first attempt %llums); retrying the identical input once after %ums\n",
                   host_err.c_str(), static_cast<unsigned long long>(first_attempt_ms),
                   static_cast<unsigned int>(kEngineHostTransientRetryBackoffMs));
            ::Sleep(kEngineHostTransientRetryBackoffMs);
            if (cancel_requested_.load(std::memory_order_acquire)) {
                // REQ-R16: a cancel posted while we slept — never start the
                // second pipe request (it could transmit user text after an
                // exit intent and could never deliver its result).
                set_status(TranslationStatus::Canceled);
                return {};
            }
            if (engine_host::TryTranslate(engine_host_config_,
                                          std::string(src_code_or_name), tgt_name,
                                          ToUtf8(text), host_out, host_err)) {
                host_fail_streak_ = 0;  // the retry recovered the service
                set_status(TranslationStatus::Ok);
                return ToUtf16(host_out);
            }
            // REQ-051 integration fix: NO second ++host_fail_streak_ here.
            // The streak is the CLIENT-SIDE REPAIR SIGNAL (V2-8.6) — one
            // signal per converged user request, not per pipe attempt; the
            // retry above is internal recovery. Counting both attempts broke
            // the frozen T5 contract (one Translate -> streak 1). The /047
            // line still logs the post-convergence streak.
            DIAG_F("ENGINE/Translate/047: transient retry failed (code=%s, fail_streak=%d); converging the §V2-8.6 UX chain\n",
                   host_err.c_str(), host_fail_streak_);
        }
        // Local serving failed. Auto: seamless cloud fallback - the consented
        // contract above.
        if (preferred_type_ == EngineType::Auto) {
            DIAG_F("ENGINE/Translate/020: local serving failed, Auto policy falling back to cloud\n");
            return cloud_call();
        }
        // Explicit local: H2 privacy gate still decides.
        if (cloud_fallback_enabled_) {
            DIAG_F("ENGINE/Translate/021: local serving failed, explicit cloud fallback consent granted\n");
            return cloud_call();
        }
        DIAG_F("ENGINE/Translate/022: local serving failed and cloud fallback consent disabled; surfacing failure to caller (repair signal armed)\n");
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
    //  - preferred_type_ == Auto: zero-install or host-absent path -> the
    //    Auto contract above makes this consented (REQ-R02 policy).
    //  - preferred_type_ == LocalLlama with no local source: implicit cloud
    //    path -> respect the H2 gate; empty+CloudConsentBlocked when disabled
    //    (strict on-device semantics: text must never leave the device
    //    silently).
    if (preferred_type_ == EngineType::GoogleTranslate ||
        preferred_type_ == EngineType::Auto ||
        cloud_fallback_enabled_) {
        return cloud_call();
    }
    set_status(TranslationStatus::CloudConsentBlocked);
    return {};
}

} // namespace emebalachat
