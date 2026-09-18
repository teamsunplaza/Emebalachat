// host_v2_session implementation — REQ-043 (M6 T4, design §1.1, §V2-4.3/4.4).
// The .hpp holds the contract comments; this file is the mechanics.

#include "host_v2_session.hpp"

#include <algorithm>

namespace emebalachat {
namespace host_v2 {

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
    {
        std::lock_guard<std::mutex> lk(mu_);
        sessions_.push_back(rec);
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

} // namespace host_v2
} // namespace emebalachat
