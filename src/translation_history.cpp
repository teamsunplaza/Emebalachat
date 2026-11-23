// V10F L2-B (261122_0002 P2-G, v0.10.10 phased fixes): the in-memory
// translation history ring. See translation_history.hpp for the contract;
// this TU is deliberately tiny and holds NO I/O of any kind (in-memory only
// is the privacy contract, pinned by tests/p2_g_tests.inc).

#include "translation_history.hpp"

#include <deque>
#include <mutex>
#include <utility>

namespace emebalachat {
namespace translation_history {

namespace {

// Function-local statics keep the ring and its mutex a single instance per
// process, initialized on first use (thread-safe statics, C++11 and later).
std::mutex& HistoryMutex() {
    static std::mutex m;
    return m;
}

std::deque<Entry>& History() {
    static std::deque<Entry> h;
    return h;
}

// V10F-R1 F-09: overwrite an entry's text buffers before they leave the ring.
// wstring storage is contiguous, so a byte-span wipe covers the whole buffer;
// the const_cast only writes the elements in place (no resize/realloc), which
// is the same SecureZeroMemory discipline win32_input uses on string copies.
void ZeroizeEntry(Entry& e) {
    auto wipe = [](std::wstring& s) {
        if (!s.empty()) {
            ::SecureZeroMemory(const_cast<wchar_t*>(s.data()), s.size() * sizeof(wchar_t));
        }
    };
    wipe(e.original);
    wipe(e.forward);
    wipe(e.back);
    wipe(e.src_code);
    wipe(e.tgt_code);
}

} // namespace

void Push(Entry entry) {
    // V10F-R1 F-09: record the PID that owned target_hwnd at Push time. The
    // ring is the single choke point every producer passes, so centralizing
    // the query here means no Push site can forget it. GetWindowThreadProcessId
    // answers 0 for a dead/already-recycled handle — the editor refuses the
    // apply for such an entry (V10F-R2 F-27: 0 with a live-looking handle
    // means the capture raced a close/recycle; the liveness probe alone
    // cannot distinguish a stranger's window). (A producer-set PID is kept
    // verbatim.)
    if (entry.target_hwnd != nullptr && entry.target_pid == 0) {
        ::GetWindowThreadProcessId(entry.target_hwnd, &entry.target_pid);
    }
    std::lock_guard<std::mutex> lk(HistoryMutex());
    std::deque<Entry>& h = History();
    h.push_front(std::move(entry));
    while (h.size() > kMaxHistoryEntries) {
        ZeroizeEntry(h.back()); // V10F-R1 F-09: zeroize before the eviction frees
        h.pop_back(); // drop the oldest; the ring never exceeds the cap
    }
}

void Clear() {
    std::lock_guard<std::mutex> lk(HistoryMutex());
    std::deque<Entry>& h = History();
    for (Entry& e : h) {
        ZeroizeEntry(e);
    }
    h.clear();
}

std::vector<Entry> Recent() {
    std::lock_guard<std::mutex> lk(HistoryMutex());
    const std::deque<Entry>& h = History();
    return std::vector<Entry>(h.begin(), h.end()); // copy: newest first
}

} // namespace translation_history
} // namespace emebalachat
