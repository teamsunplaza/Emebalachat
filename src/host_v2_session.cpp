// host_v2_session implementation — REQ-043 (M6 T4, design §1.1, §V2-4.3/4.4).
// The .hpp holds the contract comments; this file is the mechanics.

#include "host_v2_session.hpp"

#include <algorithm>

#include "diag_logger.hpp" // R1-10: the one shape-only oversize signal

namespace emebalachat {
namespace host_v2 {

// R1-10 (v0.10.4 sweep, session 261029_0001): the host process is persistent
// (REQ-060 idle eviction stops WORKERS, never the host) while Open() runs once
// per translate job, and Close/Abort only flip `state`. Without a prune the
// table grows ~120 B per job forever and every lookup pays an O(n) scan under
// mu_. The prune is therefore opportunistic and bounded:
//
//   * it runs only from Open(), under the same mutex that guards every other
//     accessor, so no reader ever observes a half-shrunk vector;
//   * it drops TERMINAL records only (Closed/Aborted) — a live session is
//     never a candidate, so OpenCount() and event sequencing are untouched;
//   * it keeps the newest kPruneKeepTerminal terminal records, so an id a
//     caller just closed (a dispatcher lookup racing the close) still
//     resolves for that many subsequent opens;
//   * ids are monotonic and records are appended in id order, so the vector
//     order equals id order and the retention cutoff is a single id.
void SessionTable::PruneLocked() {
    if (sessions_.size() <= kPruneSlack) return; // small table: nothing to do
    const std::size_t open = static_cast<std::size_t>(
        std::count_if(sessions_.begin(), sessions_.end(),
                      [](const SessionRecord& r) { return r.state == SessionState::Open; }));
    if (sessions_.size() <= kPruneOpenFactor * open + kPruneSlack) return; // within bound
    // Newest-first walk: the id of the first terminal record BEYOND the
    // retention window is the cutoff (that record and every older terminal
    // one are droppable).
    std::size_t terminal_seen = 0;
    std::uint64_t cutoff = 0;
    for (auto it = sessions_.rbegin(); it != sessions_.rend(); ++it) {
        if (it->state == SessionState::Open) continue;
        if (++terminal_seen > kPruneKeepTerminal) {
            cutoff = it->id;
            break;
        }
    }
    if (cutoff == 0) return; // <= kPruneKeepTerminal terminal records
    sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                   [cutoff](const SessionRecord& r) {
                                       return r.state != SessionState::Open && r.id <= cutoff;
                                   }),
                    sessions_.end());
}

SessionOpenResult SessionTable::Open(const SessionRecord& request, SessionRecord& out) {
    if (request.capability.empty()) return SessionOpenResult::BadArguments;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const bool served = std::any_of(served_.begin(), served_.end(),
            [&](const std::string& c) { return c == request.capability; });
        if (!served) return SessionOpenResult::Unavailable;
    }
    SessionRecord rec = request;
    rec.id = next_id_.fetch_add(1, std::memory_order_relaxed); // host-global, 1-based
    rec.state = SessionState::Open;
    rec.next_event_seq = 1;
    std::size_t retained = 0;
    std::size_t open_now = 0;
    bool signal_oversize = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        PruneLocked(); // R1-10: keep the table bounded over host uptime
        sessions_.push_back(rec);
        retained = sessions_.size();
        // R1-10 cap check: reaching this after a prune means the bound above
        // is broken (~2435 simultaneous open sessions), not that the host is
        // busy. One line per table instance (the host has one), counts only,
        // no session content.
        if (retained > kOversizeSignal && !oversize_reported_) {
            oversize_reported_ = true;
            signal_oversize = true;
            open_now = static_cast<std::size_t>(
                std::count_if(sessions_.begin(), sessions_.end(),
                              [](const SessionRecord& r) {
                                  return r.state == SessionState::Open;
                              }));
        }
    }
    // Logged OUTSIDE the table mutex: the flag above is latched under it, so
    // the line is still exactly-once per table instance.
    if (signal_oversize) {
        DIAG_F("ENGINEHOST/session/001: session table oversize after prune "
               "(retained=%zu open=%zu cap=%zu); prune bound violated\n",
               retained, open_now, kOversizeSignal);
    }
    out = std::move(rec);
    return SessionOpenResult::Opened;
}

bool SessionTable::Close(std::uint64_t session) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = std::find_if(sessions_.begin(), sessions_.end(),
        [session](const SessionRecord& r) { return r.id == session; });
    if (it == sessions_.end() || it->state != SessionState::Open) return false;
    it->state = SessionState::Closed;
    return true;
}

bool SessionTable::Abort(std::uint64_t session) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = std::find_if(sessions_.begin(), sessions_.end(),
        [session](const SessionRecord& r) { return r.id == session; });
    if (it == sessions_.end() || it->state != SessionState::Open) return false;
    it->state = SessionState::Aborted;
    return true;
}

std::uint64_t SessionTable::NextEventSeq(std::uint64_t session) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = std::find_if(sessions_.begin(), sessions_.end(),
        [session](const SessionRecord& r) { return r.id == session; });
    if (it == sessions_.end() || it->state != SessionState::Open) return 0;
    return it->next_event_seq++;
}

std::size_t SessionTable::CloseOwnedByConnection(void* connection,
                                                 std::vector<std::uint64_t>& closed) {
    std::size_t n = 0;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& r : sessions_) {
        if (r.connection == connection && r.state == SessionState::Open) {
            r.state = SessionState::Closed;
            closed.push_back(r.id);
            ++n;
        }
    }
    return n;
}

bool SessionTable::Find(std::uint64_t session, SessionRecord& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = std::find_if(sessions_.begin(), sessions_.end(),
        [session](const SessionRecord& r) { return r.id == session; });
    if (it == sessions_.end()) return false;
    out = *it;
    return true;
}

std::size_t SessionTable::OpenCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::size_t n = 0;
    for (const auto& r : sessions_) {
        if (r.state == SessionState::Open) ++n;
    }
    return n;
}

// R1-10: the retained-record footprint (open + terminal retention tail). The
// prune bound in the header is stated in these units, so the pins can assert
// "bounded" against the real accessor instead of a private field.
std::size_t SessionTable::TotalCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sessions_.size();
}

} // namespace host_v2
} // namespace emebalachat
