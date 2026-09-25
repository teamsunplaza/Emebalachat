#ifndef EMEBALACHAT_HOST_V2_ASR_RELAY_HPP
#define EMEBALACHAT_HOST_V2_ASR_RELAY_HPP

// ---------------------------------------------------------------------------
// host_v2_asr_relay — P2-1 stabilization (session 260925_0001, live-test
// defects 2/3): the pure single-slot CLAIM state behind the ggml-asr relay in
// host_main.cpp.
//
// The ggml-asr family serves ONE streaming session at a time (registry
// max_sessions=1 parity). The orchestrator mirrors that with one claim, and
// the live test found two ways it used to wedge:
//   * defect 2 (zombie claim): a connection could exit RunSessionV2 through
//     a protocol-error `return` while holding the claim; with an idle asr
//     session the relay thread never observed the death and the slot stayed
//     unavailable forever. The fix on the host side funnels EVERY connection
//     end (normal close, abnormal disconnect, protocol-error exit) through
//     ONE teardown plan (host_main.cpp converts the fatal `return`s to the
//     single loop-exit cleanup).
//   * defect 3 (mid-stream reopen): the relay thread releasing the claim as
//     its last act used to drop the owner pointer too, so the owner's later
//     teardown could not close the SessionTable record that OUTLIVES the
//     claim. ReleaseByRelay therefore PRESERVES the owner fields; only the
//     owner's own teardown (PlanTeardown/Disown) clears them.
//
// Threading: this struct is PURE (no mutex, no thread). host_main.cpp holds
// AsrRelay::mu around every call; run_tests drives the sequences directly.
// The relay thread's release is always its LAST mutex acquisition, so owner
// joins can never deadlock.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace emebalachat {
namespace host_v2 {

struct AsrRelayClaim {
    bool active = false;             // a session claim is held
    bool stop = false;               // external teardown request
    std::uint64_t wire_session = 0;  // the CLIENT-chosen session id (worker id)
    std::uint64_t table_id = 0;      // the SessionTable record id (host-global)
    const void* conn = nullptr;      // owning connection (opaque; never dereferenced)

    // Try to claim the single slot for `owner`. False when occupied — the
    // racing second asr open fails fast with unavailable (max_sessions=1
    // parity). A successful claim REPLACES any previous owner fields, so a
    // stale preserved record can never leak into a new session.
    bool TryClaim(const void* owner, std::uint64_t session, std::uint64_t table) {
        if (active) return false;
        active = true;
        stop = false;
        wire_session = session;
        table_id = table;
        conn = owner;
        return true;
    }

    // The relay's LAST act: the worker stream ended (terminal frame relayed)
    // or the pipe died — free the slot so the next open can claim it. The
    // owner fields STAY: the owning connection's teardown may still need to
    // close the SessionTable record that outlives the claim (defect 3).
    void ReleaseByRelay() { active = false; }

    // The open-attempt unwind: the worker never answered opened (or the
    // owner vanished mid-open). Full clear — the caller closes the table
    // record itself, so no teardown recovery may ever see it.
    bool ReleaseByOwner(const void* owner, std::uint64_t session) {
        if (!active || conn != owner || wire_session != session) return false;
        active = false;
        stop = false;
        wire_session = 0;
        table_id = 0;
        conn = nullptr;
        return true;
    }

    // Message-loop gates: this connection's session must be live.
    bool IsLiveFor(const void* owner) const {
        return active && conn == owner;
    }
    bool IsLiveFor(const void* owner, std::uint64_t session) const {
        return active && conn == owner && wire_session == session;
    }

    // What a connection teardown must do. Runs under the relay mutex; the
    // caller performs the plan's I/O (worker close, relay join, table close)
    // OUTSIDE the lock, then calls Disown().
    struct TeardownPlan {
        bool ours = false;                // this connection owns the slot record
        bool order_worker_close = false;  // send close{wire_session} to the worker
        std::uint64_t wire_session = 0;
        std::uint64_t table_id = 0;
    };

    // Connection end (normal close AND abnormal disconnect funnel here).
    // Idempotent: a second call for the same owner finds nothing left.
    //   * claim still held  -> order the worker close (model unload) and stop
    //     the relay; the caller joins the relay thread;
    //   * relay already self-released -> nothing to order, but the table
    //     record still belongs to us: close it (defect 3 recovery).
    TeardownPlan PlanTeardown(const void* owner) {
        TeardownPlan p;
        if (conn != owner) return p;
        p.ours = true;
        p.wire_session = wire_session;
        p.table_id = table_id;
        if (active) {
            stop = true;
            p.order_worker_close = true;
        }
        return p;
    }

    // The client's session_close for `session`: only THIS connection's live
    // session matches (a foreign or stale session id falls through to the
    // generic translate path, unchanged).
    TeardownPlan PlanSessionEnd(const void* owner, std::uint64_t session) {
        TeardownPlan p;
        if (!IsLiveFor(owner, session)) return p;
        p.ours = true;
        p.wire_session = wire_session;
        p.table_id = table_id;
        stop = true; // bounded relay exit if the close write fails
        p.order_worker_close = true;
        return p;
    }

    // Called by the teardown owner after the plan's I/O completed: the slot
    // record (if any remained after a relay self-release) is fully handed
    // off. A later teardown of a REUSED connection object must not recover
    // stale ids. GUARDED: a racing NEW owner's claim must survive — only
    // clear when WE still own the record (live race: the relay's
    // self-release frees the slot, a new connection claims it BEFORE this
    // owner's Disown runs — an unconditional Disown would wipe the new
    // owner's fields).
    void DisownIfOwner(const void* owner) {
        if (conn != owner) return;
        conn = nullptr;
        table_id = 0;
        wire_session = 0;
    }
    void Disown() { DisownIfOwner(conn); }
};

} // namespace host_v2
} // namespace emebalachat

#endif // EMEBALACHAT_HOST_V2_ASR_RELAY_HPP
