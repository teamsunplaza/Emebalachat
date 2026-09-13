#ifndef EMEBALACHAT_UI_BADGE_TRANSIENT_HPP
#define EMEBALACHAT_UI_BADGE_TRANSIENT_HPP

#include "badge.hpp"
#include "../config.hpp"
#include "../diag_logger.hpp"
#include "../unicode_utils.hpp"

#include <utility>

namespace emebalachat {

// ---- REQ-013 (session 260913_0002, user ruling 22:02): transient badge
// language-pair flip during drag-translate ----
//
// User-approved semantics: while a DRAG-context translation runs, the floating
// badge label briefly shows the DRAG pair; when the translation flow ends
// (success, failure, early return, cancellation - ALL exit paths), the label
// restores to the TYPE (typing) pair the badge normally displays.
//
// Contract (display-only):
//  * NO config value is written. The translation engine keeps receiving the
//    pair its caller resolved (persisted drag pair + per-request pivot) - this
//    guard only touches the badge surface.
//  * Restore reads the LIVE type pair (config.GetSnapshot()) at DESTRUCTION
//    time, not a snapshot taken at construction. That is the re-entrancy
//    decision: if the user cycles the type pair (Ctrl+F9) while a drag
//    translation is in flight, the restore lands on the NEW typing pair, and
//    a second drag starting before the first restores is naturally idempotent
//    (its restore also re-reads live state). A stale-snapshot restore would
//    re-display an outdated pair here.
//
// Exit-funnel discipline: callers construct the guard AFTER all early-return
// gates, so the remaining function body has a single return and the destructor
// is the only restore site - no duplicated restore per return.

class TransientDragPairBadge {
public:
    // Flips the badge label to (drag_src -> drag_tgt). Safe to call from any
    // thread: FloatingBadge::SetLanguages marshals the repaint off the GUI
    // thread (REQ-R10 seam) and AppConfig::GetSnapshot takes its mutex (I4
    // discipline for worker-thread reads). `badge` may be a null-window badge
    // (headless / not yet Create()d) - SetLanguages no-ops there.
    TransientDragPairBadge(AppConfig& config, FloatingBadge& badge,
                           std::wstring_view drag_src, std::wstring_view drag_tgt)
        : config_(config), badge_(badge) {
        badge_.SetLanguages(drag_src, drag_tgt);
        DIAG_LOG("UI", "badge_transient show drag pair");
    }

    // Non-copyable, movable (scope-guard idiom).
    TransientDragPairBadge(const TransientDragPairBadge&) = delete;
    TransientDragPairBadge& operator=(const TransientDragPairBadge&) = delete;

    TransientDragPairBadge(TransientDragPairBadge&& other) noexcept
        : config_(other.config_), badge_(other.badge_),
          armed_(std::exchange(other.armed_, false)) {}

    TransientDragPairBadge& operator=(TransientDragPairBadge&&) = delete;

    ~TransientDragPairBadge() {
        if (!armed_) return;
        armed_ = false;
        // LIVE type-pair read (see class comment): the restored label is
        // whatever the user's typing pair is RIGHT NOW - a Ctrl+F9 cycle
        // during the translation is honored at restore time.
        const AppConfig::Snapshot snap = config_.GetSnapshot();
        badge_.SetLanguages(ToUtf16(snap.type_source_language),
                            ToUtf16(snap.type_target_language));
        DIAG_LOG("UI", "badge_transient restore type pair");
    }

private:
    AppConfig& config_;
    FloatingBadge& badge_;
    bool armed_ = true;
};

} // namespace emebalachat

#endif // EMEBALACHAT_UI_BADGE_TRANSIENT_HPP
