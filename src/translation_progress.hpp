// Progress + cancel seam for multi-chunk translations (session 261121_0002).
// Pure STL header: no Win32, no network, no llama — includable from the cloud
// clients, the engine router, and the tests without new link edges. A
// default-constructed context reproduces the pre-feature behavior exactly.
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>

namespace emebalachat {

// Per-request translation task context. Owned by the CALLER (the pipeline
// worker); alive for the whole Translate() call; read-only inside the
// clients.
// - cancel: when non-null and true, the chunk loops unwind at the next
//   between-chunk check and the manager reports TranslationStatus::Canceled.
// - on_chunk: invoked on the calling (worker) thread as (index_1based,
//   total) immediately BEFORE each network/pipe-bearing chunk request;
//   total > 1 is the multi-chunk signal. The caller marshals to the GUI
//   thread itself.
// V10F (261122_0002 P2-B Fix 1): three ADDITIVE members. Every pre-existing
// caller keeps compiling and behaving unchanged (a default-constructed
// context still carries all three in their "absent" state), so the trailing
// aggregate-initialization order stays source-compatible for the positions
// that existed before.
struct TranslationTaskContext {
    const std::atomic<bool>* cancel = nullptr;

    // V10F (261122_0002 P2-B Fix 2): the request ABORT seam.
    //   * Read by the cloud chunk loops between chunks exactly like `cancel`
    //     (a set flag unwinds with {} and the manager attributes Canceled),
    //     and by the cloud clients' in-flight HINTERNET close-on-cancel slot
    //     (P2-A) so a blocked WinHTTP receive returns promptly.
    //   * TranslationManager::Translate() BRIDGES a null value to the
    //     manager's own shutdown latch (&cancel_requested_) at request entry,
    //     which is what makes the REQ-R16 exit intent reach the cloud legs
    //     mid-flight. LIFETIME: the bridged pointer names a
    //     TranslationManager member, which outlives every Translate() call;
    //     the manager's local copy of this struct is what the clients see, so
    //     the caller's own context is never mutated.
    //   * Latch semantics (never cleared during a request), so the
    //     between-chunk checks may read it freely.
    const std::atomic<bool>* abort = nullptr;

    // V10F (261122_0002 P2-B Fix 1, RISK-03 carrier): when non-null, the
    // engine/client writes the request's bounded, sanitized failure cause
    // through it (the OpenAiCompatibleClient per-request detail form; empty
    // on success and on a user cancel). TranslationManager::Translate()
    // clears the slot at request entry so a caller can never read a stale
    // cause. LIFETIME: must outlive the Translate() call; written on the
    // calling (worker) thread only.
    std::string* out_error_detail = nullptr;

    // V10F (261122_0002 P2-B Fix 1/4, L3-B preview): invoked with the
    // CUMULATIVE stitched translation after each completed chunk (all
    // engines). "Cumulative" is the contract: the receiver REPLACES its
    // preview text, never appends, because a client may emit a final
    // whole-request tick after its own streaming deltas.
    //   * Called on the calling (worker) thread, synchronously, inside the
    //     request. MARSHAL-FREE: the receiver must be thread-safe (or
    //     PostMessage itself) and must not block — the pipeline worker is the
    //     thread that is not pumping messages.
    //   * Its PRESENCE is also the partial-result opt-in: with the callback
    //     installed, a multi-frame run returns the stitched prefix a failure
    //     already produced instead of all-or-nothing (see
    //     translation_coordinator.hpp / PartialResultsOptIn below).
    //   * Empty (the default) = no preview and no partial opt-in: the
    //     pre-V10F behavior byte-for-byte.
    std::function<void(const std::wstring&)> on_chunk_text;

    std::function<void(std::size_t index_1based, std::size_t total)> on_chunk;

    // 261123_0001 ctx-pilot: the window THIS request's text belongs to (the
    // paste target the pipeline captured at interception time). Opened as a
    // void* so this header keeps its documented "pure STL, no Win32" contract
    // (the engine_host_client.hpp pipe-handle precedent): producers store an
    // HWND, and the engine casts it back for the per-window context gate
    // (src/translation_context.hpp BuildFragment). nullptr (the default) means
    // "window unknown", and an unknown window NEVER gets context.
    void* target_hwnd = nullptr;

    // 261123_0001 ctx-pilot: AppConfig::Snapshot::local_context_enabled, read
    // ONCE by the pipeline worker (the I4 locked snapshot the task already
    // carries) and shipped here so the router needs no config read, no global
    // and no cross-thread flag. false is both the default and the shipped
    // config default, and the whole pilot is gated on it: with false the
    // fragment is never built and the local frame keeps the frozen v1 shape.
    bool local_context_enabled = false;
};

// Progress numbers are only shown for genuine multi-chunk runs; a single
// chunk (or an empty plan) keeps the badge byte-identical to the
// pre-feature build.
constexpr bool ShowChunkProgress(std::size_t total) { return total > 1; }

// A set cancel flag always wins over the loop-budget check at each boundary.
constexpr bool ChunkLoopCancelRequested(const std::atomic<bool>* f) {
    return f != nullptr && f->load(std::memory_order_acquire);
}

// V10F (261122_0002 P2-B Fix 2): the abort seam's boundary predicate — the
// same shape, polarity and latch rule as ChunkLoopCancelRequested, so every
// between-chunk check can consult either seam with ONE rule (null never
// aborts, set always does). Deliberately NOT constexpr: unlike the pre-V10F
// token helper this is called from paths that also build state, and keeping
// it a plain inline function avoids relying on the constexpr-atomic
// relaxation.
inline bool ChunkLoopAbortRequested(const std::atomic<bool>* f) {
    return f != nullptr && f->load(std::memory_order_acquire);
}

// The request-level abort form: the caller's abort seam OR (once Translate()
// has bridged it) the manager's shutdown latch. A default context never
// aborts.
inline bool AbortRequested(const TranslationTaskContext& ctx) {
    return ChunkLoopAbortRequested(ctx.abort);
}

// V10F (261122_0002 P2-B Fix 3): the partial-result opt-in rule in ONE place
// — the presence of the preview sink IS the opt-in (a caller that renders
// partial text is exactly the caller that wants the partial result).
inline bool PartialResultsOptIn(const TranslationTaskContext& ctx) {
    return static_cast<bool>(ctx.on_chunk_text);
}

}  // namespace emebalachat
