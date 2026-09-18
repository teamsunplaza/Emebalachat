#ifndef EMEBALACHAT_HOST_V2_SESSION_HPP
#define EMEBALACHAT_HOST_V2_SESSION_HPP

// ---------------------------------------------------------------------------
// host_v2_session — REQ-043 (M6 T4, design §1.1 "세션 테이블", plan §V2-4.3):
// the v2 session table (EmebalaEngine target).
//
// Session model (§V2-4.3): session_open (capability, model/profile, options)
// -> bidirectional frames (client -> feed / host -> event) -> session_close /
// cancel. M6's only real consumer is translate (one-shot jobs dispatched by
// the scheduler), but the FRAME format already follows §V2-4.4: events carry
// {"op":"event","session":N,"kind":...,"seq":M,...} and seq counts per
// session from 1.
//
// Identity rules:
//   * The session id is a HOST-GLOBAL increment starting at 1 (design §1.1:
//     "호스트 전역 증분") — independent of the per-connection request ids.
//   * A session records its OWNING CONNECTION; when the connection ends, all
//     sessions it owns are closed and reported to the caller (auto-cleanup,
//     task item 2: "연결 종료 시 소유 세션 자동 정리").
//
// Unallocated capability: a session_open naming a capability the M6
// deployment does not serve (e.g. "asr") FAILS to open (unavailable) — the
// table accepts only registered capabilities, fail-closed (§V2-4.5).
//
// Privacy: sessions store identity/lifecycle fields only (ids, strings named
// by the client's open request — capability/model/profile names, not user
// text). No logging here; the host owns shape-only diagnostics.
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace emebalachat {
namespace host_v2 {

// Lifecycle of one session (design §1.1 Session sketch + §V2-4.3).
enum class SessionState : unsigned char {
    Open,     // opened, accepting jobs/events
    Closed,   // closed by the client or the host teardown
    Aborted,  // cancelled (abort) — terminal, same cleanup as Closed
};

inline std::string_view SessionStateToString(SessionState s) {
    switch (s) {
        case SessionState::Open:    return "open";
        case SessionState::Closed:  return "closed";
        case SessionState::Aborted: return "aborted";
    }
    return {};
}

// One session record (design §1.1 sketch, host-side ownership fields kept).
struct SessionRecord {
    std::uint64_t id = 0;             // host-global increment (1-based)
    void* connection = nullptr;       // owning connection (opaque to this module)
    std::string capability;           // "translate" | "asr" | ... (M6: translate)
    std::string model_id;             // registry reference ("" = family default)
    std::string profile;              // registry profile key ("default")
    int priority = 5;                 // §V2-4.6 0-9
    bool drop_eligible = false;       // backpressure permission
    std::int64_t deadline_ms = 0;     // per-request deadline for jobs (0 = none)
    SessionState state = SessionState::Open;
    std::uint64_t next_event_seq = 1; // §V2-4.4 event seq, per session, from 1
};

// Open outcomes (the caller maps failures to §V2-4.5 status codes).
enum class SessionOpenResult : unsigned char {
    Opened,          // `out.id` valid
    Unavailable,     // capability not served by this deployment
    BadArguments,    // empty capability / protocol misuse
};

class SessionTable {
public:
    // `served_capabilities`: the capabilities THIS deployment can open (M6:
    // {"translate"}). Fail-closed when empty (nothing can open).
    explicit SessionTable(std::vector<std::string> served_capabilities)
        : served_(std::move(served_capabilities)) {}

    // Opens a session with a host-global incrementing id (1-based). The
    // capability must be in the served set (unavailable otherwise).
    SessionOpenResult Open(const SessionRecord& request, SessionRecord& out);

    // Terminal transitions. Idempotent: an already-terminal id is a no-op
    // returning false. Aborting marks Aborted; closing marks Closed.
    bool Close(std::uint64_t session);
    bool Abort(std::uint64_t session);

    // Event-sequence allocator (§V2-4.4: per-session seq from 1). Returns 0
    // when the session is not open (the caller must not emit events).
    std::uint64_t NextEventSeq(std::uint64_t session);

    // Closes every OPEN session owned by `connection` (connection teardown);
    // the closed ids are appended to `closed` so the caller can notify the
    // worker/scheduler. Returns the count closed.
    std::size_t CloseOwnedByConnection(void* connection, std::vector<std::uint64_t>& closed);

    // Lookup (nullptr when absent/closed is still returned — callers check
    // state). Thread-safe snapshot copy for the dispatcher.
    bool Find(std::uint64_t session, SessionRecord& out) const;

    // Number of sessions currently OPEN (idle-exit input).
    std::size_t OpenCount() const;

private:
    mutable std::mutex mu_;
    std::vector<std::string> served_;
    std::vector<SessionRecord> sessions_; // all records, terminal ones kept for id stability
    std::atomic<std::uint64_t> next_id_{1}; // host-global increment, 1-based
};

} // namespace host_v2
} // namespace emebalachat

#endif // EMEBALACHAT_HOST_V2_SESSION_HPP
