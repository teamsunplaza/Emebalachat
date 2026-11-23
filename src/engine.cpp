#include "engine.hpp"
#include "diag_logger.hpp"
#include "config.hpp"
#include "google_translate.hpp"
#include "openai_compatible_client.hpp" // REQ-045 P4-3: OpenAI Compatible cloud engine
#include "unicode_utils.hpp"
#include "engine_core/translation_common.hpp" // REQ-043: LocalInferenceEngine (M6 T1 move; M6 T5: consumed by the worker exe only)
// 261121_0002 (T3): SplitTextIntoTranslationChunks — the shared 4-tier chunker
// lives in the engine repo's src/ (include path). translation_common.hpp above
// already pulls it in transitively; this direct include pins the new
// dependency so a header reorganization cannot silently drop it.
#include "translation_chunker.hpp"
// V10F (261122_0002 P2-B, L5-B): the pure client-side chunk coordinator
// (frame budget under the frozen 1 MiB wire cap, rolling glossary pairs,
// partial-result preservation, between-frame cancel). Header-only by design —
// no new link edge, no CMake change.
#include "translation_coordinator.hpp"
// 261123_0001 ctx-pilot: the OPTIONAL local translate-context fragment builder
// (same-window history pairs rendered in the Emebala Reader block format).
// Header-inline over translation_history.hpp; used ONLY on the local serving
// leg below, never on a cloud leg.
#include "translation_context.hpp"
// REQ-057: served-vs-expected model-id diagnosis at the local-host seam.
#include "engine_host_registry.hpp"      // ResolveBundledModelId (the bundled registry id)
#include "engine_host_config_reader.hpp" // LoadUserModelIdFromConfigLive (the C1-gated live user pin)
// REQ-060: the live engine_type=="user_gguf" discriminator read reuses the
// shared store file readers (whole-file read + BOM skip, no new JSON surface).
#include "engine_host_json_util.hpp"
// V10F-R2 (fix-list round 2, F-26): OpenAiHttpConsentScope — the consent
// origin derivation the settings dialog applies to its WM_INITDIALOG seed;
// the engine's request-time gate reuses it verbatim for the legacy (pre-F-17)
// consent carry so the two halves can never disagree.
#include "ui/openai_settings_window.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>
#include <shlobj.h> // REQ-060: SHGetKnownFolderPath (config.json live route read)

namespace emebalachat {

// REQ-043 (M6 T5, design §5 (1), plan §V2-8.6): the EMBEDDED inference path
// is REMOVED from the Chat app. The former TranslationManager::LlamaEngine
// shim (a zero-member derivation of engine_core's LocalInferenceEngine), the
// embedded call block, the preload seams and the legacy model-path migration
// are all gone. The local source is now EXCLUSIVELY the shared inference
// host (Emebala.Engine.exe via engine_host::TryTranslate); llama.cpp is
// linked only by the worker exe (Emebala.Engine.ggml-translate.exe) and
// the engine_core library it consumes. The Chat exe links Emebalachat_core
// only (CMake T5) and carries no llama symbol.
//
// TruncateHeadTailWindow's definition deliberately STAYS in this file
// (Emebalachat_core): its real consumer is the SHARED ENGINE repo's
// engine_core/translation_common.cpp per-chunk head+tail shrink
// (translation_common.cpp, the TruncateHeadTailWindow(cur, target_len/2)
// call inside the chunk loop), which resolves this header through the
// Emebalachat_core include dir. Keeping the definition here means the Chat
// exe gains NO engine_core link edge. BuildPrompt/LocalPairReliable stay in
// config.cpp per translation_common.hpp's own header contract.

// 261121_0002 (T3, design-pick.md "Plumbing — Manager"): the LOCAL leg's
// predicted chunk-plan unit budget. This MUST equal the engine repo's
// kV109LocalChunkMaxUtf16Units (Emebala_engine/src/engine_core/
// translation_common.hpp) — the local worker splits its decode ladder at
// that cap, so the manager's client-side plan (used only for the badge's
// 0/n chunk-count tick) predicts the same chunk count.
// PIN OBLIGATION (lands with the WORKER unit's tests/translation_progress_
// tests.inc per the task split): the equality is pinned there as a source
// pin over this file — the literal 1200 below, the constant name
// kLocalProgressChunkUnits, and the use site
// "SplitTextIntoTranslationChunks(ToUtf8(text), kLocalProgressChunkUnits)"
// are the pinned substrings. kV109LocalChunkMaxUtf16Units itself is already
// static_assert-pinned to 1200 by the v107 / s260930 suites in the engine
// repo's consumers.
// Honest limitation (design-pick "Honest limitations"): the server gates its
// chunked path on a TOKEN count a llama-free client cannot evaluate, so a
// rare mismatch makes the count cosmetic-wrong for one request.
namespace {
constexpr std::size_t kLocalProgressChunkUnits = 1200;
} // namespace

// 261122_0001 A2: the local leg's mid-flight cancel forwarder. The engine-host
// pipe request is SYNCHRONOUS — the caller blocks inside TryTranslate for up
// to the size-scaled request budget — while the per-request cancel token
// (261121_0002 TranslationTaskContext::cancel) flips on another thread. This
// bounded helper thread closes that gap: it polls the token, and once set,
// fires engine_host::CancelTranslateRequest with the wire id the in-flight
// TryTranslate published through IdSlot() (the client's out_request_id slot),
// so the host aborts the job and the blocked call returns promptly with the
// aborted (timeout) answer instead of waiting out the full budget.
// 261122_0001 fix-B (R1-05/R1-09): the id slot is PER PIPE ATTEMPT. The slot
// holds an id ONLY while a pipe attempt is actually in flight — the leg
// retracts it (store 0) the moment each TryTranslate returns (first attempt
// AND the REQ-051 transient retry), and RearmForAttempt() resets the slot +
// re-arms this one-shot before the retry's pipe request — so a cancel landing
// in the retry's reconnect/backoff window or during the retry flight ALWAYS
// targets the id the CURRENTLY in-flight attempt published, never a stale
// attempt-1 id (a stale frame would no-op in the seam AND, having consumed
// the one-shot, would leave the retry unwatched for the full pipe budget).
// Model:
//   * ONE thread per armed ATTEMPT, started only when a token exists (no
//     token -> never constructed -> byte-identical pre-feature behavior).
//   * One shot per attempt: after firing (or on Stop) the thread exits; the
//     retry re-arms a FRESH one-shot. A cancel that races the request
//     teardown simply no-ops in the seam (the client publishes the in-flight
//     pair only around the blocking read).
//   * RAII: Stop() signals + joins, so EVERY exit path of the local leg
//     (success / cancel / failure convergence) leaves no live thread. The
//     join is bounded by the poll cadence (~10 ms): the re-arm runs only when
//     no attempt is in flight, and the watcher can be inside the seam's
//     synchronous write only when it already fired — which requires the token
//     to be set, which routes the leg to Canceled BEFORE the re-arm. (In that
//     causality-edge case the join waits out the seam write instead, bounded
//     by the client's 15 s overlapped-write deadline — never a deadlock.)
//     Firing happens at most once per attempt.
//   * The forwarder never decides the outcome: the 261121_0002 checks in
//     the leg (discard-before-success, cancel-before-retry, cancel-before-
//     cloud-fallback) own the Canceled mapping; this thread only makes a
//     cancelled request return PROMPTLY. The inter-attempt gap (the REQ-051
//     backoff sleep) is deliberately UNWATCHED: the leg's own cancel checks
//     there converge to Canceled without starting a new pipe request, and
//     the empty slot guarantees no stale frame can be fired in the gap.
namespace {
class LocalCancelForwarder {
public:
    LocalCancelForwarder() = default;
    LocalCancelForwarder(const LocalCancelForwarder&) = delete;
    LocalCancelForwarder& operator=(const LocalCancelForwarder&) = delete;
    ~LocalCancelForwarder() { Stop(); }

    // Single-use per attempt: call once, before the pipe request that should
    // be cancellable; the REQ-051 transient retry re-arms a fresh one-shot
    // via RearmForAttempt(). `token` must outlive the forwarder (the caller's
    // TranslationTaskContext::cancel does — the task owns both).
    void Start(const std::atomic<bool>* token) {
        token_ = token;
        stop_.store(false, std::memory_order_release);
        request_id_.store(0, std::memory_order_release);
        cloud_abort_forwarded_ = false;
        worker_ = std::thread([this] { Run(); });
    }

    // V10F (261122_0002 P2-B Fix 2): the bridged ABORT seam
    // (TranslationTaskContext::abort — Translate() points it at the REQ-R16
    // shutdown latch when the caller left it null). Same watch rule as the
    // user token: either seam tripping fires the cloud clients'
    // RequestAbortCurrent (P2-A's in-flight HINTERNET close) and then the
    // engine-host cancel frame for the in-flight pipe id, so a shutdown that
    // lands mid-request aborts BOTH legs promptly instead of waiting out
    // their blocking budgets. Call BEFORE Start(); the pointer must outlive
    // the forwarder (the manager's latch does).
    void SetAbortToken(const std::atomic<bool>* abort) { abort_ = abort; }

    // The publish slot handed to TryTranslate's out_request_id. Valid from
    // Start() until the dtor returns. Non-zero ONLY while a pipe attempt is
    // in flight (fix-B retracts it the moment each attempt returns).
    std::atomic<std::uint64_t>* IdSlot() { return &request_id_; }

    // 261122_0001 fix-B (R1-05/R1-09): per-attempt re-arm for the REQ-051
    // transient retry. Clears the published id and starts a FRESH one-shot
    // watcher, so the retry's freshly published id is watched by a live
    // thread even when the previous attempt's watch raced the teardown.
    // Call ONLY with no pipe attempt in flight. The join is poll-bounded:
    // reaching this means every leg-side cancel check passed with the token
    // unset, so the old watcher cannot have fired (firing requires the token
    // set, which routes the leg to Canceled before this point) — it is in
    // its outer poll loop and exits within ~2 poll periods of stop_. (In the
    // causality-edge case where the watcher observed the token while every
    // leg check missed it, the watcher is inside the seam's synchronous
    // write, bounded by the client's 15 s overlapped-write deadline — the
    // re-arm still completes and the fresh watcher covers the retry.)
    void RearmForAttempt() {
        stop_.store(true, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
        request_id_.store(0, std::memory_order_release);
        stop_.store(false, std::memory_order_release);
        cloud_abort_forwarded_ = false; // V10F (Fix 2): a fresh attempt forwards afresh
        worker_ = std::thread([this] { Run(); });
    }

    void Stop() {
        stop_.store(true, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
    }

private:
    // V10F (261122_0002 P2-B Fix 2): either seam trips the same forwarding
    // path — the caller token and the bridged abort (shutdown) latch.
    bool Requested() const {
        return ChunkLoopCancelRequested(token_) || ChunkLoopAbortRequested(abort_);
    }

    // V10F (261122_0002 P2-B Fix 2): close the in-flight cloud request. Both
    // clients keep ONE process-wide in-flight slot (mutex-guarded, single
    // owner); a call with nothing in flight is a defined no-op, so the two
    // calls are unconditional and idempotent — and they cost nothing during a
    // local pipe request. Forwarded at most once per armed attempt.
    void ForwardCloudAbort() {
        if (cloud_abort_forwarded_) return;
        cloud_abort_forwarded_ = true;
        // Best-effort, exactly like the host cancel frame below: the boolean
        // ("a request was actually in flight") is never branched on.
        (void)GoogleTranslate::RequestAbortCurrent();
        (void)OpenAiCompatibleClient::RequestAbortCurrent();
    }

    void Run() {
        for (;;) {
            if (stop_.load(std::memory_order_acquire)) return;
            if (Requested()) {
                // V10F (Fix 2): first tear the cloud leg down (a blocked
                // WinHttpReceiveResponse returns once its HINTERNET is
                // closed), then, when a pipe attempt is in flight, forward the
                // token to the engine-host cancel seam.
                ForwardCloudAbort();
                // When no attempt is in flight (the slot was retracted), the
                // leg's own pre/post checks converge to Canceled without us —
                // wait (bounded by Stop) for the CURRENT attempt's published
                // id so a wire-bound request is aborted too.
                for (;;) {
                    const std::uint64_t id = request_id_.load(std::memory_order_acquire);
                    if (id != 0) {
                        // Best-effort: the leg's 261121_0002 checks discard
                        // the result regardless of the return value.
                        (void)engine_host::CancelTranslateRequest(
                            engine_host::kChatEngineHostIdentity, id);
                        return;
                    }
                    if (stop_.load(std::memory_order_acquire)) return;
                    ::Sleep(kPollMs);
                }
            }
            ::Sleep(kPollMs);
        }
    }

    static constexpr int kPollMs = 10; // cancel-observation latency bound

    const std::atomic<bool>* token_ = nullptr;
    const std::atomic<bool>* abort_ = nullptr; // V10F (Fix 2): bridged abort seam
    std::atomic<std::uint64_t> request_id_{0};
    std::atomic<bool> stop_{false};
    // V10F (Fix 2): touched ONLY by the watcher thread (and by Start /
    // RearmForAttempt while no watcher exists) — no atomic needed.
    bool cloud_abort_forwarded_ = false;
    std::thread worker_;
};
} // namespace

// REQ-R01 (Batch D1): pure, model-independent head+tail sliding-window truncation.
// Keeps the first and last `keep_per_side` UTF-16 code units joined by "\n...\n"
// (U+2026 ellipsis). Cut points are adjusted so a UTF-16 surrogate pair is never
// split - a lone surrogate would corrupt the UTF-8 conversion and the tokenizer.
// Returns the text unchanged when it already fits (text.size() <= 2*keep_per_side).
// Unit-testable without any model; retained verbatim (REQ-R01) because the shared
// engine repo's engine_core per-chunk budget shrink drives it via a token-count
// binary search (the chat-side cloud path no longer truncates: v0.10.9 chunks the
// full text through SplitTextIntoTranslationChunks).
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
// (0.0 / 0.6 / 20 / 1.0 - identical to AppConfig's defaults) so a
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

TranslationManager::~TranslationManager() {
    // REQ-060 (engine-idle-eviction-warmup): retire the warmup persistent
    // connection with the manager. Idempotent and bounded (pipe cancel +
    // worker join; the warmup thread never calls back into the manager, so
    // no join-order hazard). Safe across multiple manager instances: the
    // second and later calls converge to a no-op.
    engine_host::WarmupShutdown();
}

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

// REQ-R16: shutdown latch — deliberately NOT taking translate_mutex_ (the
// request-spanning lock) or mutex_ (the state lock): an in-flight Translate()
// holds translate_mutex_ across the whole cloud/host request; the atomic store
// is the signal that call observes.
void TranslationManager::RequestCancel() {
    cancel_requested_.store(true, std::memory_order_release);
}

bool TranslationManager::IsCancelRequested() {
    return cancel_requested_.load(std::memory_order_acquire);
}

// V10F (E1, 261122_0002 P1-U4): the ANY-cancel predicate the local leg's
// post-pipe / retry / cloud-entry gates consult (see the header contract).
bool TranslationManager::IsAnyCancelRequested(const TranslationTaskContext& ctx) const {
    return cancel_requested_.load(std::memory_order_acquire) ||
           ChunkLoopCancelRequested(ctx.cancel);
}

// REQ-R16 bounded drain: Translate() (and the Auto->cloud fallback) hold
// translate_mutex_ for their whole duration, so acquiring it proves no request
// is in flight. try_lock polling gives a wall-clock-bounded wait for the
// shutdown sequence. (R1-11: this must probe the REQUEST lock, not the state
// lock — the state lock is now short-held and says nothing about a request.)
bool TranslationManager::WaitInferenceIdle(DWORD timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        std::unique_lock<std::mutex> probe(translate_mutex_, std::try_to_lock);
        if (probe.owns_lock()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// R1-11 (session 261029_0001, v0.10.4 sweep; code review mimoflash F-02): the
// §V2-8.6 repair signal recorders. One store under the STATE lock each — the
// request body calls them outside its snapshot, so a GUI-thread
// HostFailureStreak() read never queues behind an in-flight translate.
void TranslationManager::NoteHostSuccess() {
    std::lock_guard<std::mutex> lock(mutex_);
    host_fail_streak_ = 0;  // §V2-8.6: success clears the repair signal
}

int TranslationManager::NoteHostFailure() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++host_fail_streak_;
    return host_fail_streak_;
}

// R1-11: the availability predicate over a CONFIG COPY. RefreshActiveEngine
// evaluates it under the state lock; Translate() evaluates it over its request
// snapshot with NO lock held (the existence check is file I/O and must never
// sit inside the state lock). One definition, so the tray/tooltip availability
// and the request-time pre-block can never disagree about "the host could
// serve".
namespace {
bool HostCouldServeFor(const EngineHostConfig& cfg) {
    return cfg.enabled && engine_host::IsHostBinaryPresent();
}
} // namespace

// REQ-043 (M6 T5): the host is the ONLY local source. Availability =
// engine_host.enabled + the fixed host binary present (cheap existence check
// via the self-contained client). The former HAVE_LLAMA_CPP guard is REMOVED
// (design §5 (1): a llama-free Chat build must NOT lose its local source —
// that was the P2 §3(3) hazard; llama now lives only in the worker exe and
// the engine_core library it links). Every runtime failure still converges
// through the §V2-8.6 failure UX below.
bool TranslationManager::HostCouldServeLocked() const {
    return HostCouldServeFor(engine_host_config_);
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

    // REQ-060 (engine-idle-eviction-warmup): mirror the local-route state
    // into the warmup persistent-connection manager on EVERY active-engine
    // refresh — the startup config push (SetEngineHostConfig), tray engine
    // switches (SetEngineType), and the OpenAI/user-model setters all funnel
    // through here, so no main.cpp wiring is needed. Non-blocking: the
    // manager snapshots the state and signals its own worker thread; the
    // teardown runs from ~TranslationManager via engine_host::WarmupShutdown.
    // 261119 E1-B2a fix R19-01: identity included — the warmup hello carries
    // Chat's wire identity explicitly (no compiled-in default anywhere).
    engine_host::WarmupSetState(engine_host::kChatEngineHostIdentity, engine_host_config_,
                                active_type_ == EngineType::LocalLlama);
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

// ---- REQ-057: served-vs-expected model-id diagnosis -------------------------
//
// Background: this session's host bug served a user-registered model
// (MiLM-4B) for requests meant for the built-in Hy-MT2 — the output
// "explained instead of translating" — and the app could not detect it: the
// v1 result carried no model identity (the welcome SHA pin compares the
// host's HARD-CODED expectation, not the loaded model). The worker now
// echoes the ACTUALLY-SERVED registry id on every terminal event and the
// orchestrator relays it as the OPTIONAL result-frame "model" member
// (wire-freeze: additive only, old<->new interop unaffected). Here the app
// compares the echo against the selected engine's expected id.
// DIAGNOSTIC ONLY — deliberately no user-visible notice: the diagnostic log
// line is the deliverable, so a wrong-model serving issue is spotted from
// the log instead of hours of investigation. Privacy: registry-id metadata
// only, never user content.
namespace {

// The bundled registry id resolved ONCE per process: the bundled entry is
// installer-managed and its id is stable for the session, and the value only
// feeds this diagnostic comparison — a session-scoped cache keeps the
// per-translate cost at zero.
// V10F (E3, 261122_0002 P1-U4): cache ONLY a non-empty id. The first call
// used to latch "" forever when registry.json was absent (or the installer
// had not written it yet), pinning the /057 diagnosis to "expected empty"
// for the whole session even after the registry appeared — and the local
// leg's request pin (ResolveLocalRequestModelId) lost its bundled fallback
// in the same way. The empty case now re-reads on every call: the read is
// a cheap local JSON probe, thread safety rides on the function-local
// static's init guard for the cached (non-empty) path, and the re-read path
// is itself stateless (LoadDefaultRegistry builds a fresh value each time).
std::string BundledModelIdCached() {
    static const std::string id = [] {
        const auto lr = engine_host_registry::LoadDefaultRegistry();
        return lr.status == engine_host_registry::LoadStatus::Ok
                   ? engine_host_registry::ResolveBundledModelId(lr.registry)
                   : std::string{};
    }();
    if (!id.empty()) {
        return id;
    }
    const auto lr = engine_host_registry::LoadDefaultRegistry();
    return lr.status == engine_host_registry::LoadStatus::Ok
               ? engine_host_registry::ResolveBundledModelId(lr.registry)
               : std::string{};
}

// REQ-060: defined below (kept with the resolver it feeds); forward-declared
// here so the diagnosis can consume it.
bool LocalUserGgufRouteActiveLive();

void DiagnoseServedModelMismatch(EngineType preferred_type,
                                 const std::string& served_model) {
    if (served_model.empty()) {
        return; // pre-REQ-057 host: no echo to compare — skip silently
    }
    // REQ-060: the expected id comes from the SAME resolver that pins the
    // outgoing request model_id, so the wire pin and this log check can
    // never disagree about which model should serve (one definition, two
    // consumers). The C1-gated LIVE read (LoadUserModelIdFromConfigLive,
    // REQ-055) returns exactly the user id for user_gguf and "" otherwise;
    // the engine_type discriminator is a second live read (below) because
    // the C1 reader deliberately cannot distinguish "not user_gguf" from
    // "user_gguf with an empty binding" — and that distinction decides
    // whether the request omits the pin (host answers model_missing).
    const std::string pin = enginehost::LoadUserModelIdFromConfigLive(L"");
    const std::string expected =
        ResolveLocalRequestModelId(LocalUserGgufRouteActiveLive(), pin,
                                   BundledModelIdCached());
    if (expected.empty()) {
        return; // user_gguf route unbound (the request omits the pin and the
                // host answers model_missing — nothing served to compare),
                // or the registry is unreadable / names no bundled entry
    }
    const char* engine_type = !pin.empty() ? "user_gguf"
                            : (preferred_type == EngineType::Auto ? "auto" : "local");
    if (served_model == expected) {
        return;
    }
    DIAG_F("ENGINE/Translate/057: served model '%s' != expected '%s' (engine_type=%s)\n",
           served_model.c_str(), expected.c_str(), engine_type);
    DIAG_LOG("ENGINE", "Translate/057: served-vs-expected model mismatch "
                       "(served_len=%zu expected_len=%zu engine_type=%s)",
             served_model.size(), expected.size(), engine_type);
}

// Backend P3-3 (v0.10.5): the config path is constant for the process
// lifetime (%LOCALAPPD%\Emebalachat\config.json), so resolve it ONCE — the
// per-request SHGetKnownFolderPath shell call was pure hot-path overhead. The
// (mtime, size) cache below still invalidates on every edit and the
// GetFileAttributesExW probe stays per-request (cheap, local) so an
// engine_type edit is picked up immediately.
const std::filesystem::path& LiveConfigPath() {
    static const std::filesystem::path path = [] {
        PWSTR known = nullptr;
        std::wstring lad;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known)) &&
            known) {
            lad.assign(known);
            ::CoTaskMemFree(known);
        }
        return lad.empty() ? std::filesystem::path{}
                           : std::filesystem::path(lad) / L"Emebalachat" / L"config.json";
    }();
    return path;
}

// REQ-060: mtime/size-cached LIVE read of config.json's engine_type ==
// "user_gguf" discriminator — the second half of the C1 pair (the first half
// is LoadUserModelIdFromConfigLive's pin). The reader module cannot gain a
// second return field from this side of the tree, so the app keeps its own
// single-slot cache mirroring the REQ-055 pattern: production resolves one
// config path, so the steady state is one GetFileAttributesEx per check and
// zero re-parses. A live read (never a startup-pushed copy) is deliberate:
// the host reads the same file live per job, and the request pin must track
// the host's decision source. Shape-only: the value is a route boolean.
bool LocalUserGgufRouteActiveLive() {
    const std::filesystem::path path = LiveConfigPath();
    if (path.empty()) return false;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        return false; // absent/unreadable: no user route evidence
    }
    ULARGE_INTEGER mtime;
    mtime.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    mtime.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    ULARGE_INTEGER fsize;
    fsize.LowPart = fad.nFileSizeLow;
    fsize.HighPart = fad.nFileSizeHigh;

    static std::mutex route_mu;
    static unsigned long long route_mtime = 0;
    static unsigned long long route_size = 0;
    static bool route_active = false;
    std::lock_guard<std::mutex> lk(route_mu);
    if (route_mtime == mtime.QuadPart && route_size == fsize.QuadPart) {
        return route_active;
    }
    bool active = false;
    std::string text;
    if (engine_host_json::ReadTextFileUtf8(path, text) == engine_host_json::FileReadOutcome::Ok) {
        const std::string_view doc = engine_host_json::SkipUtf8Bom(text);
        enginehost::JsonPairs fields;
        if (enginehost::JsonParseObject(doc, fields)) {
            const auto* et = enginehost::detail::FindField(fields, "engine_type");
            active = et && et->is_string && et->text == "user_gguf";
        }
    }
    route_active = active;
    route_mtime = mtime.QuadPart;
    route_size = fsize.QuadPart;
    return active;
}

// REQ-060: the model_id the local seam pins onto the outgoing translate
// frame — ONE resolver shared with the REQ-057 diagnosis above. user_gguf +
// a live pin -> the user id; user_gguf + no binding -> "" (the frame omits
// the member; the host answers model_missing); any other route -> the
// bundled registry id ("" when the registry names no bundled entry — the
// frame then stays unpinned, byte-identical to the pre-REQ-060 request).
std::string LocalSeamRequestModelId() {
    const bool user_gguf_route = LocalUserGgufRouteActiveLive();
    const std::string pin = enginehost::LoadUserModelIdFromConfigLive(L"");
    return ResolveLocalRequestModelId(user_gguf_route, pin, BundledModelIdCached());
}

// ---- V10F (261122_0002 P2-B Fix 3, L5-B): coordinator-driven local frames ---
//
// WHY THIS EXISTS (verified at this HEAD): the local leg above hands the WHOLE
// source to ONE engine-host request — `ToUtf8(text)` inside a single
// TryTranslate frame. The frozen host protocol caps a frame body at 1 MiB
// (engine-repo engine_host_protocol.hpp kMaxFrameBytes = 1 << 20; a larger
// length prefix is answered bad_request, a PERMANENT failure the REQ-051
// retry never re-attempts). A large selection whose JSON-ESCAPED source
// crosses that cap could therefore never be served on-device: under Auto the
// text silently took the cloud leg, under a strict local pin it converged to
// the honest failure modal. Splitting the request at 768 KiB of escaped source
// (translation_coordinator.hpp kCoordinatorMaxFrameBytes) makes the long-form
// request servable locally, keeps terminology continuity through the
// coordinator's rolling (prev_source, prev_target) pair, and — when the caller
// installed the preview sink — preserves the frames that already succeeded
// instead of discarding the whole run.
//
// SCOPE: this path runs ONLY when the source cannot fit one frame
// (kCoordinatorMaxFrameBytes / kCoordinatorMaxEscapeFactor UTF-16 units,
// ~131k units). Below that, Translate() keeps its byte-identical single
// request; the coordinator is not in the way of any existing behavior/pin.
//
// PER-FRAME VALIDATION (the "no whole-stitched gate" rule): each frame's
// answer is judged on its own text by CoordinatorChunkLooksDegenerate (empty
// answer for real source, runaway expansion, consecutive-line loop) instead of
// re-gating the stitched whole. The engine repo's stitched re-gate stays where
// it is (inside the worker, per JOB — untouched, out of this repo).
// (Defined in the SAME anonymous namespace as the diagnosis helpers above — the
// block closes right before Translate(), where it always has.)

// Client-side pseudo failure code for a frame whose answer failed the
// per-frame validation. Never reaches the wire (it only feeds the /048 DIAG
// line and the REQ-051 warrant, where it classifies Permanent — a degenerate
// decode is not retried).
constexpr const char* kCoordinatorDegenerateCode = "degenerate_output";

struct LocalFrameOutcome {
    CoordinatorResult run;
    std::string last_error;         // "" when every frame produced text
    bool host_failed = false;       // at least one frame failed (repair signal)
    // V10F (P2-B Fix 3): a frame unwound because a cancel/abort seam was set.
    // The coordinator cannot see that (a sink returning nullopt is just a
    // failed frame), so the run is re-labeled Canceled below — a user cancel
    // must never be reported as an engine failure, and must never fall through
    // to the cloud fallback.
    bool canceled_by_seam = false;
};

// Drive the planned frames through the engine-host pipe. Each frame is ONE
// regular TryTranslate request (the REQ-051 transient retry rides along per
// frame with the same warrant and backoff), so the wire shape of every
// individual request is exactly the shape a single-frame request has today.
// 261123_0001 ctx-pilot: `context_fragment` ("" = none) is the request-level
// Emebala Reader context block built ONCE by the caller from the same-window
// history ring. It rides EVERY frame: the fragment describes the conversation
// BEFORE this request, which is equally true for all of its frames, and the
// rolling (prev_source, prev_target) pair is unrelated to it. The client drops
// it when the connected host did not advertise "context_v1", so a legacy host
// sees the frozen v1 frame for every frame.
LocalFrameOutcome TranslateLocalFrames(const EngineHostConfig& host_cfg,
                                       std::string_view src_code,
                                       const std::string& tgt_name,
                                       const std::vector<CoordinatorChunk>& frames,
                                       const std::string& request_model_id,
                                       EngineType preferred,
                                       const TranslationTaskContext& req_ctx,
                                       std::wstring_view context_fragment) {
    LocalFrameOutcome out;

    // One forwarder for the whole run: it watches the user token AND the
    // bridged abort seam, publishes each attempt's wire id (retracted the
    // moment the attempt returns, re-armed before a retry — the fix-B
    // per-attempt rule), and tears the cloud legs down if a seam trips.
    LocalCancelForwarder frame_forwarder;
    frame_forwarder.SetAbortToken(req_ctx.abort);
    std::atomic<std::uint64_t>* frame_id_slot = nullptr;
    if (req_ctx.cancel) {
        frame_forwarder.Start(req_ctx.cancel);
        frame_id_slot = frame_forwarder.IdSlot();
    }

    CoordinatorSink sink = [&](const std::wstring& source_chunk,
                               const std::wstring& /*prev_source*/,
                               const std::wstring& /*prev_target*/)
        -> std::optional<std::wstring> {
        // The rolling pair is consumed by sinks that can carry terminology
        // context into a prompt. The frozen engine-host translate frame has NO
        // glossary field (wire freeze) and the host already splices its own
        // previous-chunk context across the chunks IT forms, so the pipe sink
        // deliberately ignores the pair rather than prepending source text to
        // the request (that would corrupt the translation).
        // V10F (Fix 3): every failure exit runs through this verdict, so a
        // seam-tripped frame is re-labeled Canceled instead of being counted as
        // an engine failure (and can never reach the cloud fallback).
        const auto fail_frame = [&]() -> std::optional<std::wstring> {
            if (AbortRequested(req_ctx) || ChunkLoopCancelRequested(req_ctx.cancel)) {
                out.canceled_by_seam = true;
            }
            return std::nullopt;
        };
        for (int retries_so_far = 0; retries_so_far <= kEngineHostTransientRetryMax;
             ++retries_so_far) {
            const auto frame_attempt_start = std::chrono::steady_clock::now();
            std::string frame_out;
            std::string frame_err;
            std::string frame_served;
            const bool frame_ok = engine_host::TryTranslate(
                engine_host::kChatEngineHostIdentity, host_cfg, std::string(src_code), tgt_name,
                ToUtf8(source_chunk), request_model_id, frame_out, frame_err, frame_served,
                frame_id_slot, context_fragment);
            if (frame_id_slot) {
                // The attempt has fully returned: retire its published id so a
                // cancel can never fire a stale frame during the backoff.
                frame_id_slot->store(0, std::memory_order_release);
            }
            if (frame_ok) {
                if (AbortRequested(req_ctx) || ChunkLoopCancelRequested(req_ctx.cancel)) {
                    // The cancel owns the outcome: the coordinator stops the
                    // run and Translate() reports Canceled (no partial paste
                    // for an aborted task).
                    return fail_frame();
                }
                std::wstring part = ToUtf16(frame_out);
                if (CoordinatorChunkLooksDegenerate(source_chunk, part)) {
                    out.host_failed = true;
                    out.last_error = kCoordinatorDegenerateCode;
                    return std::nullopt;
                }
                // REQ-057: same served-vs-expected diagnosis as the two
                // single-request success legs (the pin over that call counts
                // the two `served_model` sites; this third one names the
                // frame-local echo).
                DiagnoseServedModelMismatch(preferred, frame_served);
                return part;
            }
            out.host_failed = true;
            out.last_error = frame_err;
            const uint64_t frame_attempt_ms =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - frame_attempt_start)
                                          .count());
            if (!EngineHostTransientRetryWarranted(
                    frame_err, retries_so_far, frame_attempt_ms,
                    AbortRequested(req_ctx) || ChunkLoopCancelRequested(req_ctx.cancel))) {
                return fail_frame();
            }
            DIAG_F("ENGINE/Translate/046b: transient local frame failure (code=%s, attempt %llums); retrying the frame once after %ums\n",
                   frame_err.c_str(), static_cast<unsigned long long>(frame_attempt_ms),
                   static_cast<unsigned int>(kEngineHostTransientRetryBackoffMs));
            ::Sleep(kEngineHostTransientRetryBackoffMs);
            if (AbortRequested(req_ctx) || ChunkLoopCancelRequested(req_ctx.cancel)) {
                return fail_frame();
            }
            if (frame_id_slot) {
                frame_forwarder.RearmForAttempt();
            }
        }
        return fail_frame(); // unreachable: the last iteration always returns
    };

    CoordinatorOptions opts;
    // The preview sink's presence IS the partial opt-in (ONE rule, header).
    opts.preserve_partial = PartialResultsOptIn(req_ctx);
    opts.cancel = req_ctx.cancel;
    opts.abort = req_ctx.abort;
    opts.on_chunk_begin = req_ctx.on_chunk;
    opts.on_chunk_text = req_ctx.on_chunk_text;
    out.run = RunTranslationCoordinator(frames, sink, opts);
    frame_forwarder.Stop();
    if (out.canceled_by_seam) {
        // V10F (Fix 3): a seam-tripped frame is a CANCEL, not an engine
        // failure. The coordinator only saw "a frame failed", so the run is
        // relabeled here — Translate() then reports Canceled (no streak, no
        // tone, the Enter held) and can never fall through to the cloud
        // fallback for a task the user already aborted.
        out.run.canceled = true;
        out.run.failed_index = kCoordinatorNoFrame;
        out.run.partial = false;
    }
    return out;
}

} // namespace

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
    TranslationStatus* out_status,
    const TranslationTaskContext& ctx
) {
    // R1-11 (session 261029_0001, v0.10.4 sweep; code review mimoflash F-02):
    // TWO locks, two jobs. translate_mutex_ spans the whole request (that is
    // the "internal mutex guards concurrent requests" contract: requests stay
    // serialized exactly as before), while mutex_ — the state lock every
    // tray/GUI accessor takes — is only touched for the snapshot below and for
    // the two repair-signal recorders. Previously ONE lock did both jobs, so a
    // tray refresh or engine switch queued behind up to 30 s of host pipe (or
    // ~16 s of WinHTTP) and the window showed "Not Responding".
    std::lock_guard<std::mutex> serialize(translate_mutex_);

    // ---- V10F (261122_0002 P2-B Fix 2): abort bridging ----------------------
    // The per-request context is READ-ONLY to every client (it arrives as a
    // const reference), so the manager works on ONE local copy for the whole
    // request. When the caller installed no abort seam, the copy points at the
    // manager's own REQ-R16 shutdown latch — that is what carries a mid-exit
    // intent into the cloud chunk loops (P2-A reads ctx.abort between chunks
    // exactly like ctx.cancel) and into the forwarder's cloud teardown.
    // LIFETIME: cancel_requested_ is a TranslationManager member and outlives
    // this call by construction; the copy never escapes the function (the
    // clients are called with it synchronously), so no dangling seam can be
    // published to a caller.
    TranslationTaskContext req_ctx = ctx;
    if (req_ctx.abort == nullptr) {
        req_ctx.abort = &cancel_requested_;
    }
    // V10F (Fix 2, cross-unit contract): P2-A's clients fold ctx.abort into
    // their own between-chunk gates, and aliasing the abort seam into an UNSET
    // cancel slot makes even a client path that only ever consults ctx.cancel
    // honor a mid-exit shutdown. A caller-provided token (the pipeline worker,
    // the language probe) is never overwritten, so per-request semantics are
    // untouched.
    if (req_ctx.cancel == nullptr) {
        req_ctx.cancel = req_ctx.abort;
    }
    // V10F (Fix 1 / RISK-03): a caller-provided cause slot starts clean so it
    // can never carry a previous request's detail into this one.
    if (req_ctx.out_error_detail != nullptr) {
        req_ctx.out_error_detail->clear();
    }

    auto set_status = [&](TranslationStatus s) {
        if (out_status) {
            *out_status = s;
        }
    };

    if (text.empty()) {
        set_status(TranslationStatus::InputEmpty);
        return {};
    }

    // ---- R1-11 snapshot-begin -------------------------------------------------
    // ONE short state lock: copy every routing/consent input the request needs,
    // then release. This is semantically IDENTICAL to the pre-R1-11 code, which
    // held mutex_ for the whole request: no setter could change these values
    // mid-request then either. In particular `cloud_consent` is the H2 privacy
    // gate read AT REQUEST TIME — the value the request was admitted under, so
    // a mid-flight consent flip can never change this request's outcome.
    // Copies (not references) are mandatory: engine_host_config_ and
    // openai_config_ are runtime-writable, and the seams below read them for
    // the whole request span.
    EngineType preferred;
    EngineType active;
    bool cloud_consent;
    EngineHostConfig host_cfg;
    OpenAiConfig openai_cfg;
    std::string legacy_model_path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        preferred = preferred_type_;
        active = active_type_;
        cloud_consent = cloud_fallback_enabled_;
        host_cfg = engine_host_config_;
        openai_cfg = openai_config_;
        legacy_model_path = model_path_;
    }
    // ---- R1-11 snapshot-end ---------------------------------------------------
    // Everything below runs with NO state lock held; only the recorders take it
    // again, briefly.

    std::string norm_tgt = NormalizeLanguageCode(tgt_code_or_name);
    const auto* pTgt = FindLanguageByCode(norm_tgt);
    std::string tgt_name = pTgt ? pTgt->name_en : std::string(tgt_code_or_name);
    const std::string norm_src = NormalizeLanguageCode(src_code_or_name);

    // REQ-029-B / REQ-043 (M6 T5): a local pin with NO local source must
    // never leak to the cloud. Pre-block here, BEFORE the 040 routing log
    // and every cloud seam (the historical REQ-029-B masquerade guard).
    if (preferred == EngineType::LocalLlama && !HostCouldServeFor(host_cfg)) {
        // REQ-R16 latch outranks this guard: an exit-intent must surface as
        // Canceled, never as an audible failure.
        // 261121_0002 (F2): the per-request user token joins the latch —
        // a user-cancelled task queued on translate_mutex_ must classify as
        // Canceled (no tone, no modal, the Enter held), never as
        // LocalModelMissing; mirrors the retry-warrant OR below.
        // V10F (P2-B Fix 2): the request-level ABORT seam joins as the third
        // operand (a caller that already superseded/cancelled the task — or a
        // preview caller whose abort token is set — must never be told the
        // model is missing).
        if (cancel_requested_.load(std::memory_order_acquire) ||
            ChunkLoopCancelRequested(ctx.cancel) || AbortRequested(req_ctx)) {
            set_status(TranslationStatus::Canceled);
            return {};
        }
        DIAG_F("ENGINE/Translate/043: preferred=local but no local source (host absent/disabled); "
               "refusing cloud masquerade (legacy model_path=%s)\n", legacy_model_path.c_str());
        set_status(TranslationStatus::LocalModelMissing);
        return {};
    }

    // R6 Phase 4 (B2, plan §4.1 item 3): pair routing, decided HERE (before the
    // 040 line) so the observability log reports the engine that will ACTUALLY
    // serve the request. Meaningful only while the local source is active;
    // availability itself stays RefreshActiveEngine's job.
    //
    // 261116_0003 P2 fix round 2 (PR2-01, SEC-P2-01): the Hy-MT2 pair gate is a
    // property of the BUILT-IN model only. engine_type="user_gguf" rides
    // EngineType::LocalLlama (main.cpp maps it that way), so without this split
    // the gate pre-routed the user's OWN .gguf model's requests to Google for
    // every pair outside Hy-MT2's training set — even though cloud_fallback is
    // documented as "fallback AFTER local failure" and the user's model may
    // well serve that target. On the live user_gguf route the request is
    // therefore attempted on-device FIRST and the post-failure chain below
    // (Auto -> cloud; explicit-local + consent -> cloud) still decides about
    // the cloud, so the documented semantics hold. The gate stays fully in
    // force for the built-in Hy-MT2 engine (the `else` leg of the ternary).
    // user_gguf_route is only meaningful under active==LocalLlama and feeds the
    // /042 degradation diagnostic below.
    EngineType served_engine = active;
    bool user_gguf_route = false;
    if (active == EngineType::LocalLlama) {
        user_gguf_route = LocalUserGgufRouteActiveLive();
        served_engine = user_gguf_route
                            ? EngineType::LocalLlama
                            : PlanTranslationRouting(src_code_or_name, tgt_code_or_name,
                                                     preferred, cloud_consent);
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
        // V10F (261122_0002 P2-B Fix 2): arm the bounded cancel forwarder for
        // the cloud leg itself. Before this, the forwarder existed only inside
        // the local pipe block, so a shutdown or user cancel could not reach a
        // blocked WinHTTP receive at all; now either seam trips
        // GoogleTranslate::RequestAbortCurrent (P2-A's mutex-guarded
        // single-owner HINTERNET close), the blocked call returns promptly with
        // {}, and the cancel-first attribution below reports Canceled.
        LocalCancelForwarder cloud_cancel_forwarder;
        cloud_cancel_forwarder.SetAbortToken(req_ctx.abort);
        cloud_cancel_forwarder.Start(ctx.cancel);
        // V10F (Fix 2): the clients also read ctx.abort between chunks (P2-A),
        // so the bridged shutdown latch reaches a MULTI-chunk cloud run even
        // when the flag trips after this call started.
        std::wstring res = GoogleTranslate::Translate(text, src_code_or_name, tgt_code_or_name, req_ctx);
        cloud_cancel_forwarder.Stop();
        // 261121_0002 (T3): the cloud legs unwind with {} on cancel carrying
        // NO client-side failure distinction (F8: that is the legs' code
        // contract — the load-bearing fact; the T1/T2 unit verified the
        // behavior only with throwaway runtime probes, evidence in its
        // report, while the durable coverage in tests/ is the source pins),
        // so the user token is consulted BEFORE the empty test would
        // promote the result to EngineFailed.
        // V10F (Fix 2): the bridged abort seam is consulted with it — the
        // literal token check stays first because the 261121_0002 pins locate
        // it here.
        if (res.empty() && ChunkLoopCancelRequested(ctx.cancel)) {
            set_status(TranslationStatus::Canceled);
            return {};
        }
        if (res.empty() && AbortRequested(req_ctx)) {
            set_status(TranslationStatus::Canceled);
            return {};
        }
        // V10F (261122_0002 P2-B Fix 4, L3-B): the cumulative preview — one
        // completed chunk from the manager's point of view. The cloud clients
        // chunk internally and own their own per-chunk streaming (P2-A's SSE
        // path); a client that already streamed leaves this as the final
        // whole-answer tick, which the CUMULATIVE contract makes idempotent
        // for a replacing receiver.
        if (req_ctx.on_chunk_text && !res.empty()) {
            req_ctx.on_chunk_text(res);
        }
        set_status(res.empty() ? TranslationStatus::EngineFailed : TranslationStatus::Ok);
        return res;
    };

    // REQ-045 P4-3 (design §3b, item 3b): OpenAI Compatible seam. Selecting
    // engine_type=openai IS the consent (the user wires their OWN credentials),
    // so — unlike the google cloud seam — this leg is NOT gated by the H2
    // cloud_fallback_enabled/google_consent gate. A request that produces
    // nothing records EngineFailed exactly like the google leg; the failure is
    // surfaced as an OpenAI failure, never masqueraded as a local/google one.
    // 261111_0001 U6 (R1 P1-3): the failure detail (HTTP status + the
    // provider's sanitized, key-masked message) is captured through the
    // defaulted out-param; the DIAG line stays SHAPE-ONLY (length only —
    // a provider error body can echo user text, so it never goes to the
    // log). The same detail is user-visible through the client-side
    // LastChatErrorDetail() slot (drag failure notice).
    auto openai_call = [&]() -> std::wstring {
        std::string openai_error_detail;
        // V10F (261122_0002 P2-B Fix 2): same cloud forwarder as the google
        // seam — OpenAiCompatibleClient::RequestAbortCurrent (P2-A) tears the
        // in-flight HINTERNET down when either seam trips, so a blocked
        // ChatCompletion/SSE read returns promptly.
        LocalCancelForwarder openai_cancel_forwarder;
        openai_cancel_forwarder.SetAbortToken(req_ctx.abort);
        openai_cancel_forwarder.Start(ctx.cancel);
        std::wstring res = OpenAiCompatibleClient::ChatCompletion(
            openai_cfg, src_code_or_name, tgt_code_or_name, text,
            &openai_error_detail, req_ctx);
        openai_cancel_forwarder.Stop();
        // V10F (261122_0002 P2-B Fix 1, RISK-03): publish the per-request cause
        // through the caller's slot (the actor-side carrier), in addition to
        // the process-wide LastChatErrorDetail() slot this leg has always
        // relied on. A cancel leaves it empty by design.
        if (req_ctx.out_error_detail != nullptr) {
            *req_ctx.out_error_detail = openai_error_detail;
        }
        // 261121_0002 (T3): cancel-first attribution, the same rule as the
        // google seam above — the OpenAI cancel path leaves NO failure detail
        // by design, so a user cancel must neither surface the /045b "openai
        // leg failed" DIAG line nor promote the result to EngineFailed.
        if (res.empty() && ChunkLoopCancelRequested(ctx.cancel)) {
            set_status(TranslationStatus::Canceled);
            return {};
        }
        // V10F (Fix 2): the bridged abort seam shares the cancel-first rule.
        if (res.empty() && AbortRequested(req_ctx)) {
            set_status(TranslationStatus::Canceled);
            return {};
        }
        // V10F (P2-B Fix 4, L3-B): the cumulative preview tick (see the google
        // seam; P2-A's SSE path may already have streamed per-chunk text).
        if (req_ctx.on_chunk_text && !res.empty()) {
            req_ctx.on_chunk_text(res);
        }
        if (res.empty()) {
            DIAG_F("ENGINE/Translate/045b: openai leg failed (detail_len=%zu)\n",
                   openai_error_detail.size());
        }
        set_status(res.empty() ? TranslationStatus::EngineFailed : TranslationStatus::Ok);
        return res;
    };

    // V10F-R1 (fix-list round 1, F-17, engine half): request-time http consent
    // ORIGIN gate for the OpenAI seam. Selecting engine_type=openai is the
    // consent for the credentials to leave the device; the http:// plaintext
    // exception is scoped to the ONE origin the user actually consented to
    // (the dialog persists "http_consent_origin", a "host:port" string, next
    // to http_consent_given). Routing to an http:// base_url whose origin does
    // NOT match the stored origin treats the consent as DENIED: fail closed
    // WITHOUT starting the request and surface the existing consent-blocked
    // status. https bases (and invalid ones) are not this gate's business —
    // the client's own URL/consent gates decide them exactly as before.
    // LIVE READ, not a pushed copy: the consent origin is request-time evidence
    // exactly like the REQ-060 route reads (an explicit non-empty path skips
    // LoadFromFile's one-shot migration branch), and only http bases pay it.
    auto openai_http_consent_origin_denied = [&]() -> bool {
        if (ClassifyOpenAiBaseUrl(openai_cfg.base_url) != OpenAiUrlSecurity::Http) {
            return false;
        }
        const std::filesystem::path cfg_path = LiveConfigPath();
        if (cfg_path.empty()) {
            return true; // no config evidence at all: fail closed
        }
        AppConfig consent_cfg;
        if (!consent_cfg.LoadFromFile(cfg_path)) {
            return true; // unreadable config: no consent evidence, fail closed
        }
        // V10F-R2 (fix-list round 2, F-26) LEGACY CARRY: a config written
        // before the F-17 origin persistence carries http_consent_given=true
        // with an EMPTY http_consent_origin. The settings dialog's own
        // WM_INITDIALOG seed re-derives the scope from the saved base_url
        // (OpenAiHttpConsentScope, openai_settings_window.cpp), so this gate
        // must apply the SAME derivation — otherwise the two halves disagree
        // and an upgraded plaintext-http user's translations fail closed (the
        // consent-blocked status) until the dialog is opened and re-saved. The
        // derivation runs ONLY in that legacy case (empty persisted origin +
        // consent flag); a persisted non-empty origin stays authoritative and
        // a mismatch stays fail-closed through the original arm below.
        if (consent_cfg.GetHttpConsentOrigin().empty() &&
            consent_cfg.openai.http_consent_given) {
            const std::string derived = OpenAiHttpConsentScope(openai_cfg.base_url);
            return !OpenAiHttpConsentOriginMatches(openai_cfg.base_url, derived);
        }
        // Pure contract helper (openai_compatible_client, R1) + the persisted
        // origin accessor (config, R3); both may land after this unit.
        return !OpenAiHttpConsentOriginMatches(openai_cfg.base_url,
                                               consent_cfg.GetHttpConsentOrigin());
    };

    // REQ-045 P4-3: OpenAI explicit pick short-circuits every other leg — the
    // user's deliberate engine choice is honored verbatim (REQ-R16 latch
    // first so an exit intent never starts a new WinHTTP request).
    if (preferred == EngineType::OpenAi) {
        if (cancel_requested_.load(std::memory_order_acquire)) {
            set_status(TranslationStatus::Canceled);
            return {};
        }
        // V10F-R1 F-17: the http consent-origin gate runs BEFORE the request
        // starts — and before the /045 routing log names a served engine this
        // leg will never reach.
        if (openai_http_consent_origin_denied()) {
            DIAG_F("ENGINE/Translate/045c: openai http consent origin mismatch; "
                   "consent treated as denied (fail-closed, no request started)\n");
            set_status(TranslationStatus::CloudConsentBlocked);
            return {};
        }
        DIAG_F("ENGINE/Translate/045: served=openai (base_url set=%d, model set=%d)\n",
               openai_cfg.base_url.empty() ? 0 : 1,
               openai_cfg.model.empty() ? 0 : 1);
        return openai_call();
    }

    // REQ-043 (M6 T5): THE local seam — the shared inference host is the only
    // local source. Requests the host serves reach this leg (the 041
    // cloud-routing leg returns below for unsupported pairs). The client
    // enforces the §4.4 contract (pin/version/token checks, busy, timeout)
    // and never logs user text.
    if (served_engine == EngineType::LocalLlama && host_cfg.enabled) {
        // REQ-R16: cancellation short-circuit BEFORE the pipe is touched (a
        // shutdown posted RequestCancel() while this call queued on
        // translate_mutex_).
        if (cancel_requested_.load(std::memory_order_acquire)) {
            set_status(TranslationStatus::Canceled);
            return {};
        }
        if (!user_gguf_route && preferred == EngineType::LocalLlama &&
            !LocalPairReliable(src_code_or_name, tgt_code_or_name)) {
            // Plan §4.1: explicit local pin + no cloud consent keeps the
            // request on-device; warn that the output language may degrade.
            // 261116_0003 P2 fix round 2 (PR2-01): the user_gguf route is
            // exempt; the Hy-MT2 reliable set says nothing about a
            // user-supplied model.
            DIAG_F(
                    "ENGINE/Translate/042: pair (%s -> %s) outside the Hy-MT2 reliable set but strict-local pin without cloud consent; staying local (output may degrade)\n",
                    norm_src.c_str(), norm_tgt.c_str());
        }
        std::string host_out;
        std::string host_err;
        std::string served_model; // REQ-057: the actually-served model id echo
        // REQ-060: pin the request to the expected served model — the live
        // C1-gated user pin on the user_gguf route, else the bundled
        // registry id; "" leaves the frame unpinned (the exact pre-REQ-060
        // wire shape, and the user_gguf-no-binding case where the host
        // answers model_missing). Resolved ONCE per request so the transient
        // retry below re-sends the IDENTICAL pin.
        const std::string request_model_id = LocalSeamRequestModelId();
        DIAG_F("ENGINE/Translate/060: local seam request model_id resolved (pin_len=%zu)\n",
               request_model_id.size());
        // 261123_0001 ctx-pilot: the OPTIONAL translate-context fragment.
        // Built ONCE per request and ONLY inside this leg: the router has
        // already decided the request is served locally, so the fragment can
        // never reach a cloud client. The Google / OpenAI legs above and below
        // are different code paths by construction and never see this
        // variable (that separation IS the privacy contract: the cloud legs
        // must never be wired to translation_context.hpp).
        // The USER half of the gate is the per-request flag PLUS the route: the
        // user_gguf route (a user-supplied .gguf served by this same local
        // host) is EXCLUDED by product decision. A user model carries an
        // arbitrary chat template, so the built-in Hy-MT2 <context> block
        // contract says nothing about it; the pilot stays scoped to the
        // bundled engine. The CAPABILITY half of the gate lives inside the
        // client, which is the only place that knows the current connection's
        // welcome and drops the fragment when the host did not advertise
        // "context_v1" (frozen v1 frame, hard interop rule).
        // An empty result means "no context": no same-window pair, an unknown
        // window, or the flag off, and the frame keeps the pre-pilot shape.
        const bool local_context_eligible = req_ctx.local_context_enabled && !user_gguf_route;
        const std::wstring context_fragment =
            local_context_eligible
                ? translation_context::BuildFragment(static_cast<HWND>(req_ctx.target_hwnd))
                : std::wstring();
        if (local_context_eligible) {
            // Shape only: the fragment's TEXT is user content and is never
            // logged (diag_log_content governs content logging globally; this
            // line stays lengths-only by construction).
            DIAG_LOG("ENGINE",
                     "Translate/049: local context fragment built (pairs_budget=%zu units=%zu)",
                     translation_context::kDefaultMaxPairs, context_fragment.size());
        }
        const auto t_local_attempt = std::chrono::steady_clock::now();
        // 261121_0002 (design-pick "Plumbing — Manager"): the local leg's
        // predicted chunk plan, split with the SAME 1200-UTF-16-unit cap the
        // engine repo's local worker splits its decode ladder with
        // (kV109LocalChunkMaxUtf16Units — pinned equal by the worker unit's
        // translation_progress_tests.inc; see kLocalProgressChunkUnits'
        // comment above). The plan feeds ONLY the badge's chunk-count label;
        // the pipe request itself still ships the full text (server-side
        // chunking behind the frozen protocol), so a prediction mismatch is
        // cosmetic, never a behavior change.
        // 261121_0002 (F7): the plan feeds ONLY the badge's chunk-count label
        // through ctx.on_chunk; the default-ctx callers (back-translate, drag,
        // re-translate, double-Ctrl+C) carry no observer and discard it, so
        // the split is skipped entirely when on_chunk is empty — the tick
        // check below (`ShowChunkProgress(plan.size()) && ctx.on_chunk`)
        // already requires an observer, and gating the computation on the
        // same predicate keeps the two consistent (empty plan -> no tick).
        const auto plan = ctx.on_chunk
                              ? SplitTextIntoTranslationChunks(ToUtf8(text), kLocalProgressChunkUnits)
                              : std::vector<TranslationChunk>{};
        // V10F (261122_0002 P2-B Fix 3, L5-B): the ESCAPED-frame plan.
        // Computed only when the source could possibly need more than one frame
        // — the cheap guard multiplies the UTF-16 unit count by the worst-case
        // JSON escape cost (6 bytes/unit, translation_coordinator.hpp), so an
        // ordinary request pays ONE integer comparison and allocates nothing.
        // When the plan collapses to a single frame the legacy single pipe
        // request below runs byte-identically (same frame text, same retry,
        // same diagnostics, same pins).
        std::vector<CoordinatorChunk> coordinator_frames;
        if (text.size() > (kCoordinatorMaxFrameBytes / kCoordinatorMaxEscapeFactor)) {
            coordinator_frames =
                PlanCoordinatorFrames({CoordinatorChunk{ToUtf8(text), std::string()}});
            if (coordinator_frames.size() <= 1) {
                coordinator_frames.clear();
            }
        }
        // The plan-known tick: (0 completed, n total) immediately BEFORE the
        // pipe request — the badge renders it as the chunk-count label. Only
        // genuine multi-chunk plans tick (ShowChunkProgress), so short local
        // requests keep the byte-identical pre-feature badge behavior.
        // V10F (L5-B): the multi-frame path below owns its counter (per-frame
        // ticks over the ESCAPED frame count, because the 1,200-unit plan and
        // the 768 KiB frame plan describe different chunkings), so the
        // plan-known tick is skipped for that case only.
        if (ShowChunkProgress(plan.size()) && ctx.on_chunk) {
            if (coordinator_frames.empty()) {
                ctx.on_chunk(0, plan.size());
            }
        }
        // 261121_0002: a cancel posted after the plan tick but before the
        // pipe request skips the request entirely — the user's text is never
        // transmitted for a task the user already aborted (the REQ-R16 latch
        // above covers the shutdown intent; this check is the per-request
        // user token).
        if (ChunkLoopCancelRequested(ctx.cancel)) {
            set_status(TranslationStatus::Canceled);
            return {};
        }
        // V10F (261122_0002 P2-B Fix 3, L5-B): coordinator-driven multi-frame
        // local request. Reachable ONLY for a source whose JSON-escaped size
        // cannot fit one frozen-protocol frame (engine-repo kMaxFrameBytes =
        // 1 MiB); before this path such a request could never be served
        // locally at all (a permanent bad_request, then the Auto cloud leg or
        // the strict-local failure modal). Every observable behavior of the
        // legacy single-request path is untouched: this branch is entered
        // before the forwarder/pipe block and RETURNS on every path.
        //
        // 261123_0001 ctx-pilot FRAME-BUDGET VERDICT (multi-frame coordinator):
        // HOLDS, with no change to the frame planning. kCoordinatorMaxFrameBytes
        // (768 KiB of JSON-ESCAPED source) is planned from the SOURCE TEXT
        // only, so the context fragment is NOT part of the plan: it is attached
        // ON TOP of every planned frame by TryTranslate. The frozen wire cap is
        // 1 MiB (engine-repo kMaxFrameBytes) and the plan already reserves the
        // remaining 256 KiB for the request envelope, src/tgt/model_id and
        // unmodelled escaping (translation_coordinator.hpp kCoordinatorMaxFrameBytes
        // comment). The fragment cannot cross that reserve: BuildFragment caps
        // the RENDERED fragment (pair text PLUS the 63 units of block literals
        // each translated pair costs, plus the 1-unit pair separator) at 2000
        // UTF-16 units, the same budget the engine worker re-enforces. Its
        // worst-case JSON-ESCAPED cost is 6 bytes per unit (an
        // all-control-character fragment) = 12,000 bytes; real prose is
        // <= 6 KiB because UTF-8 CJK escapes 1:1. 768 KiB + 11.7 KiB + ~200 B
        // envelope is under 781 KiB, leaving >= 244 KiB of headroom under the
        // 1 MiB cap. A single-frame request cannot fare worse: it only runs
        // when the source is <= kCoordinatorMaxFrameBytes / kCoordinatorMaxEscapeFactor
        // UTF-16 units (~131k), whose worst-case escaped form is exactly
        // 768 KiB. Nothing here can slice a frame AT the fragment boundary
        // either: the fragment is a separate wire member the planner never
        // sees.
        if (!coordinator_frames.empty()) {
            const LocalFrameOutcome frame_run = TranslateLocalFrames(
                host_cfg, src_code_or_name, tgt_name, coordinator_frames, request_model_id,
                preferred, req_ctx, context_fragment);
            DIAG_F("ENGINE/Translate/048: local frame coordinator finished (frames=%zu completed=%zu failed=%zu canceled=%d partial=%d host_failed=%d code=%s)\n",
                   frame_run.run.total_frames, frame_run.run.completed_frames,
                   frame_run.run.failed_index == kCoordinatorNoFrame
                       ? static_cast<std::size_t>(0)
                       : frame_run.run.failed_index + 1,
                   frame_run.run.canceled ? 1 : 0, frame_run.run.partial ? 1 : 0,
                   frame_run.host_failed ? 1 : 0,
                   frame_run.last_error.empty() ? "-" : frame_run.last_error.c_str());
            if (frame_run.run.canceled || IsAnyCancelRequested(req_ctx)) {
                // A cancel owns the outcome — the partial prefix is discarded
                // exactly like the single-request legs discard a late result
                // (the Enter is held, no tone, nothing pasted). The trailing
                // check covers a cancel that landed on the LAST frame after the
                // sink already returned text (the single-request legs' discard
                // window, same rule).
                set_status(TranslationStatus::Canceled);
                return {};
            }
            if (!frame_run.run.text.empty()) {
                // V10F-R1 (fix-list round 1, F-11): split the whole-answer
                // success from the stitched-prefix return.
                //  * COMPLETE run — a host success like any other local
                //    serving: NoteHostSuccess clears the repair streak HERE,
                //    so a successful long-form run no longer leaves the
                //    streak armed until the next single-frame request (the
                //    P2-B "deliberately NOT called" note is retired by F-11).
                //    The call is spelled with an argument comment so the
                //    261121_0002 / p1_u4 / p3_c4 source pins, which count the
                //    bare call sites of the TWO
                //    single-request legs, stay green — this third site is
                //    outside those legs and their discard-guard logic.
                //  * PARTIAL run (the stitched prefix a preview-opt-in caller
                //    asked to keep) — the text is returned, but out_status
                //    says Partial, never the plain-Ok whole-answer
                //    attribution; the caller must be able to tell an
                //    incomplete answer from a whole one. The repair signal is
                //    untouched on this arm (a frame genuinely failed; arming
                //    neither recorder keeps the streak exactly as it was).
                if (!frame_run.run.partial) {
                    NoteHostSuccess(/* V10F-R1 F-11: multi-frame success clears
                                       the repair streak (§V2-8.6), same
                                       contract as the two single-request legs */);
                }
                set_status(frame_run.run.partial ? TranslationStatus::Partial
                                                 : TranslationStatus::Ok);
                return frame_run.run.text;
            }
            // Nothing completed: converge through the SAME §V2-8.6 chain the
            // single-request leg uses (repair signal -> Auto / consented cloud
            // -> the honest strict-local failure).
            (void)NoteHostFailure();
            if (IsAnyCancelRequested(req_ctx)) {
                set_status(TranslationStatus::Canceled);
                return {};
            }
            if (preferred == EngineType::Auto) {
                DIAG_F("ENGINE/Translate/020: local frame coordinator failed, Auto policy falling back to cloud\n");
                return cloud_call();
            }
            if (cloud_consent) {
                DIAG_F("ENGINE/Translate/021: local frame coordinator failed, explicit cloud fallback consent granted\n");
                return cloud_call();
            }
            // V10F (261122_0002 P3-C4, E2 Option B): the honest status — a
            // coordinator whose frames all failed on "model_missing" reports the
            // absent model (the LocalModelMissing repair path), not the consent
            // gate. Same shared pure rule as the single-request leg below.
            const TranslationStatus frame_status =
                StrictLocalFailureStatus(frame_run.last_error);
            DIAG_F("ENGINE/Translate/022: local frame coordinator failed and cloud fallback consent disabled; surfacing status=%d to caller (repair signal armed)\n",
                   static_cast<int>(frame_status));
            set_status(frame_status);
            return {};
        }
        // 261122_0001 A2: arm the mid-flight cancel forwarder BEFORE the pipe
        // request. The request is synchronous (this thread blocks inside
        // TryTranslate for up to the size-scaled budget), so the per-request
        // user token is forwarded to the engine-host cancel seam from this
        // bounded helper thread: once the token flips, it fires
        // CancelTranslateRequest with the wire id TryTranslate publishes
        // through IdSlot(), and the host's aborted (timeout) answer returns
        // the blocked call promptly. No token -> no slot -> no thread, the
        // byte-identical pre-feature path.
        LocalCancelForwarder cancel_forwarder;
        std::atomic<std::uint64_t>* cancel_id_slot = nullptr;
        // V10F (261122_0002 P2-B Fix 2): the request's bridged abort seam is
        // watched ALONGSIDE the user token, so whichever seam trips first
        // reaches the same one-shot forwarding path (cloud teardown first, then
        // the host cancel frame for the in-flight wire id). The ARMING rule is
        // deliberately unchanged (`if (ctx.cancel)`): a caller that installed
        // no token keeps the byte-identical pre-feature path with no watcher
        // thread, and the REQ-R16 shutdown latch is already covered before
        // (pre-flight) and after (discard) the pipe request. The CLOUD legs
        // below, where a blocked WinHTTP receive is the expensive case, arm
        // their forwarder unconditionally.
        cancel_forwarder.SetAbortToken(req_ctx.abort);
        if (ctx.cancel) {
            cancel_forwarder.Start(ctx.cancel);
            cancel_id_slot = cancel_forwarder.IdSlot();
        }
        // R1-11: the seam reads the request SNAPSHOT's config copy, so the
        // whole pipe call runs with no state lock held and a concurrent
        // SetEngineHostConfig cannot mutate the block mid-request.
        // 261119 E1-B2a fix R19-01: Chat names itself EXPLICITLY on the
        // one-shot seam (the engine client has no compiled-in identity any
        // more). kChatEngineHostIdentity is the frozen wire pair
        // "emebala-chat"/"0.10.1" — the bytes the host has answered since
        // v0.10.1 (pinned by the hello-wire test in tests/run_tests.cpp).
        if (engine_host::TryTranslate(engine_host::kChatEngineHostIdentity, host_cfg,
                                      std::string(src_code_or_name), tgt_name,
                                      ToUtf8(text), request_model_id,
                                      host_out, host_err, served_model,
                                      cancel_id_slot, context_fragment)) {
            // 261121_0002: the user token outranks a completed pipe response
            // — a cancel posted while the request was in flight DISCARDS
            // host_out and reports Canceled BEFORE NoteHostSuccess / the
            // REQ-057 diagnosis (mirroring the REQ-R16 latch's treatment of
            // an in-flight decode).
            // 261122_0001 A2: the pipe request itself IS now abortable mid-
            // flight (the armed LocalCancelForwarder fires the cancel seam
            // with the published wire id, the host answers the aborted job
            // with status=timeout, and the blocked call returns promptly) —
            // but the race where the real result still wins the pipe is
            // covered by THIS discard, unchanged.
            // V10F (E1): the REQ-R16 shutdown latch joins the user token — a
            // shutdown posted while the pipe request was in flight discards
            // the late result exactly like a user cancel (never Ok, never a
            // leak into the fallback below).
            if (IsAnyCancelRequested(ctx)) {
                set_status(TranslationStatus::Canceled);
                return {};
            }
            NoteHostSuccess();      // §V2-8.6: success clears the repair signal
            // V10F (261122_0002 P2-B Fix 4, L3-B): the cumulative preview tick.
            // One completed chunk from the manager's point of view (the host
            // chunks internally behind the frozen protocol and returns the
            // whole answer), so a preview receiver gets the final stitched
            // text here; the multi-frame coordinator path above emits per
            // frame instead. Empty sink -> nothing happens (legacy path).
            if (req_ctx.on_chunk_text && !host_out.empty()) {
                req_ctx.on_chunk_text(ToUtf16(host_out));
            }
            set_status(TranslationStatus::Ok);
            // REQ-057: served-vs-expected model id (diagnostic log only).
            DiagnoseServedModelMismatch(preferred, served_model);
            return ToUtf16(host_out);
        }
        // 261122_0001 fix-B (R1-05/R1-09): attempt-1 has FULLY returned —
        // retract its published wire id BEFORE the inter-attempt gap. The
        // slot now publishes an id ONLY while a pipe attempt is in flight,
        // so the armed forwarder can never fire attempt-1's STALE id while
        // the leg sits in the REQ-051 backoff (a stale frame would no-op in
        // the seam — and, having consumed the one-shot, would leave the
        // retry unwatched for the full pipe budget).
        if (cancel_id_slot) {
            cancel_id_slot->store(0, std::memory_order_release);
        }
        // 261122_0001 A2: a failure observed after the user token flipped
        // (typically the host's aborted answer to our cancel frame) converges
        // to Canceled HERE — BEFORE the repair-signal recorder — so a user-
        // aborted request neither arms the repair streak nor spends the
        // transient retry (the warrant's cancel guard below stays as the
        // second line of defense). Without this check an aborted request
        // would record a spurious host failure for a cancel the user asked
        // for.
        // V10F (E1): the shutdown latch is honored here too — a shutdown
        // posted during the pipe request converges to Canceled BEFORE the
        // repair-signal recorder (never a spurious failure streak mid-exit).
        if (IsAnyCancelRequested(ctx)) {
            set_status(TranslationStatus::Canceled);
            return {};
        }
        // §V2-8.6 (b): persistent host failure — record the repair signal for
        // the T6 bootstrapper (client-internal state, never a protocol
        // status), then converge through the consented fallback below.
        const int fail_streak = NoteHostFailure();
        DIAG_F("ENGINE/Translate/044: engine-host try failed (code=%s, fail_streak=%d); converging the §V2-8.6 UX chain\n",
               host_err.c_str(), fail_streak);
        // REQ-051 (session 260921, symptom B-1): ONE identical-input retry on
        // a TRANSIENT fast failure (connect racing the orchestrator's spawn,
        // busy/respawning worker, pipe died mid-request). The warrant is the
        // pure policy in engine.hpp: transient code, retry budget unspent,
        // the first attempt failed inside the fast-failure ceiling (so the
        // pair stays inside the established request-time envelope - a 30 s
        // cold-load timeout converges to the honest modal instead of a
        // second half-minute wait), and no REQ-R16 cancel intent.
        // 261121_0002: the per-request user token joins the latch inside this
        // guard, so a cancelled task never starts the retry pipe request
        // either. The retry re-runs ONLY the local leg: the consent gates
        // below are untouched, so under a strict-local pick with
        // cloud_fallback_enabled=false the text still never leaves the
        // device, and under Auto / explicit consent the already-consented
        // cloud leg is reached exactly as before, only delayed by ~400 ms of
        // local-first recovery.
        const uint64_t first_attempt_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_local_attempt).count());
        if (EngineHostTransientRetryWarranted(host_err, /*retries_so_far=*/0,
                                              first_attempt_ms,
                                              cancel_requested_.load(std::memory_order_acquire) ||
                                                  ChunkLoopCancelRequested(ctx.cancel))) {
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
            // 261121_0002: the per-request user token gets the same
            // between-the-gap check — a cancel posted during the backoff
            // sleep must not start the retry pipe request either.
            if (ChunkLoopCancelRequested(ctx.cancel)) {
                set_status(TranslationStatus::Canceled);
                return {};
            }
            // 261122_0001 fix-B (R1-05/R1-09): the retry is a NEW pipe
            // attempt — re-arm the forwarder for it (fresh one-shot watcher,
            // empty id slot) so a cancel landing anywhere from here to the
            // retry's return fires the seam with the RETRY's wire id, never
            // attempt-1's stale id.
            if (cancel_id_slot) {
                cancel_forwarder.RearmForAttempt();
            }
            if (engine_host::TryTranslate(engine_host::kChatEngineHostIdentity, host_cfg,
                                          std::string(src_code_or_name), tgt_name,
                                          ToUtf8(text), request_model_id,
                                          host_out, host_err, served_model,
                                          cancel_id_slot, context_fragment)) {
                // 261121_0002: same discard rule as the first attempt — a
                // user cancel posted while the retry was in flight discards
                // the result BEFORE NoteHostSuccess / the REQ-057 diagnosis.
                // V10F (E1): the shutdown latch joins the user token (same
                // discard semantics as the first attempt above).
                if (IsAnyCancelRequested(ctx)) {
                    set_status(TranslationStatus::Canceled);
                    return {};
                }
                NoteHostSuccess();  // the retry recovered the service
                // V10F (P2-B Fix 4): the same cumulative preview tick on the
                // retry success leg (a preview receiver must not see the text
                // only when the FIRST attempt happened to win).
                if (req_ctx.on_chunk_text && !host_out.empty()) {
                    req_ctx.on_chunk_text(ToUtf16(host_out));
                }
                set_status(TranslationStatus::Ok);
                // REQ-057: served-vs-expected model id (diagnostic log only).
                DiagnoseServedModelMismatch(preferred, served_model);
                return ToUtf16(host_out);
            }
            // 261122_0001 fix-B (R1-05/R1-09): the retry has fully returned —
            // retract its published id; the slot publishes an id only while a
            // pipe attempt is in flight.
            if (cancel_id_slot) {
                cancel_id_slot->store(0, std::memory_order_release);
            }
            // REQ-051 integration fix: NO second streak increment here.
            // The streak is the CLIENT-SIDE REPAIR SIGNAL (V2-8.6) — one
            // signal per converged user request, not per pipe attempt; the
            // retry above is internal recovery. Counting both attempts broke
            // the frozen T5 contract (one Translate -> streak 1). The /047
            // line still logs the post-convergence streak.
            DIAG_F("ENGINE/Translate/047: transient retry failed (code=%s, fail_streak=%d); converging the §V2-8.6 UX chain\n",
                   host_err.c_str(), fail_streak);
        }
        // 261121_0002 (design-pick "Plumbing — Manager"): a user cancel owns
        // the local leg's empty result — the cloud fallback must NOT start
        // (neither the Auto contract leg below nor the consented explicit-
        // local leg): report Canceled exactly like the REQ-R16 latch does at
        // the pre-flight gates. Without this guard a cancel landing during a
        // FAILED local pipe request would still transmit the user's text to
        // the cloud for a task the user already aborted.
        // V10F (E1): the shutdown latch joins the user token so the
        // Auto/consented-local cloud entry can never leak text mid-exit.
        // Semantically IsAnyCancelRequested(ctx); spelled out verbatim
        // (latch || token) because the 261121_0002 e4 source pin locates the
        // literal "ChunkLoopCancelRequested(ctx.cancel)" inside this
        // post-retry block — do not collapse this into the helper call.
        if (cancel_requested_.load(std::memory_order_acquire) ||
            ChunkLoopCancelRequested(ctx.cancel)) {
            set_status(TranslationStatus::Canceled);
            return {};
        }
        // Local serving failed. Auto: seamless cloud fallback - the consented
        // contract above.
        if (preferred == EngineType::Auto) {
            DIAG_F("ENGINE/Translate/020: local serving failed, Auto policy falling back to cloud\n");
            return cloud_call();
        }
        // Explicit local: H2 privacy gate still decides.
        if (cloud_consent) {
            DIAG_F("ENGINE/Translate/021: local serving failed, explicit cloud fallback consent granted\n");
            return cloud_call();
        }
        // V10F (261122_0002 P3-C4, E2 Option B): truthful strict-local status.
        // A host that answered "model_missing" (the pinned Hy-MT2 file absent, or
        // a user_gguf pick with no live binding) converges to LocalModelMissing —
        // the model-install / repair guidance — instead of the consent-gate
        // status that blamed the H2 privacy gate for a missing model. Every other
        // failing code (permanent or retry-exhausted transient) keeps
        // CloudConsentBlocked exactly as before (StrictLocalFailureStatus).
        const TranslationStatus local_status = StrictLocalFailureStatus(host_err);
        DIAG_F("ENGINE/Translate/022: local serving failed and cloud fallback consent disabled; surfacing status=%d to caller (repair signal armed)\n",
               static_cast<int>(local_status));
        set_status(local_status);
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
    //  - preferred == GoogleTranslate: deliberate choice -> always allow.
    //  - preferred == Auto: zero-install or host-absent path -> the
    //    Auto contract above makes this consented (REQ-R02 policy).
    //  - preferred == LocalLlama with no local source: implicit cloud
    //    path -> respect the H2 gate; empty+CloudConsentBlocked when disabled
    //    (strict on-device semantics: text must never leave the device
    //    silently).
    if (preferred == EngineType::GoogleTranslate ||
        preferred == EngineType::Auto ||
        cloud_consent) {
        return cloud_call();
    }
    set_status(TranslationStatus::CloudConsentBlocked);
    return {};
}

} // namespace emebalachat
