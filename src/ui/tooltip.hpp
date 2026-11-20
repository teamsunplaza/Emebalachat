#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint> // R6 Phase 2 (B1-H1): uint64_t generation ids
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <sapi.h>

#include "layered_renderer.hpp"
#include "dpi.hpp" // 261108_0002 (WS3): constexpr ClampWindowOrigin for the pure card placement math
#include "ui/theme.hpp"
#include "config.hpp" // 261116_0003 P6b: LanguageEngine for the engine-view resolver seam

namespace emebalachat {

// Returns the Windows LCID for a given language code or name. Returns 0 if unknown.
DWORD GetLcidForLanguage(std::string_view target_lang_name_or_code);

// 261111_0001 (v0.10.7) fix-list round 2, G-06: the tooltip card's
// show/dismiss publication state. ONE mutex makes {visible, dismissal-owed,
// card generation} a single indivisible unit, so an off-thread Dismiss() can
// never be observed half-applied while a fresh Show* publishes a card in
// between.
//
// The defect this replaces: Dismiss() cleared the atomic visible_ and THEN
// stored a separate `dismiss_teardown_pending_` stamp. A GUI-side Show*
// landing in that window published a FRESH card and cleared the stamp; the
// worker's late store then set it again and the queued kDismissMessage tore
// the fresh card down (a card the user never dismissed vanished).
//
// The generation names the CARD: every Show* publication bumps it, Dismiss
// captures the generation of the card it began on, and the posted
// kDismissMessage completes only while that generation is still current. A
// card published after the dismissal began therefore can never be torn down
// by it, and a duplicate/stale message is a pure no-op.
//
// Pure (no Win32), so every interleaving is unit-testable headlessly
// (tests/v107_drag_tooltip_tests.inc drives the adversarial sequence); the
// live TooltipWindow holds one instance and every read/write goes through it.
struct TooltipCardVisibility {
    // Sentinel for "no card generation". Generations start at 1, so 0 can
    // never match a published card (same rule as TooltipWindow::kGenNone).
    static constexpr uint64_t kNoGeneration = 0;

    // GUI thread (Show*): publish a fresh card. Bumps the card generation, so
    // every dismissal begun against an older card is stale from here on.
    uint64_t PublishShow() {
        std::lock_guard<std::mutex> lk(mu_);
        visible_ = true;
        teardown_owed_ = false;
        return ++generation_;
    }

    // ANY thread (Dismiss): begin a dismissal as ONE unit - clear visibility,
    // owe the GUI teardown, and capture the generation of the card being
    // dismissed. Returns false when nothing is visible (the caller's early
    // return, unchanged from the pre-fix contract).
    bool TryBeginDismiss(uint64_t& out_generation) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!visible_) {
            return false;
        }
        visible_ = false;
        teardown_owed_ = true;
        out_generation = generation_;
        return true;
    }

    // GUI thread (kDismissMessage handler): true only when THIS dismissal is
    // still current (no newer card was published) AND a teardown is owed;
    // consumes the owed stamp so a second message cannot double-teardown.
    bool TryCompleteDismiss(uint64_t generation) {
        std::lock_guard<std::mutex> lk(mu_);
        if (generation == kNoGeneration || generation != generation_ || !teardown_owed_) {
            return false;
        }
        teardown_owed_ = false;
        return true;
    }

    // GUI thread (TeardownCard): the card is off screen. The owed stamp is
    // irrelevant once the teardown has run; the caller clears it explicitly.
    void MarkHidden() {
        std::lock_guard<std::mutex> lk(mu_);
        visible_ = false;
    }

    // ANY thread (R1-G restore): a failed dismissal post restores the coherent
    // pre-post state - card shown, nothing owed - so a later dismissal path
    // retries instead of leaving a ghost card. Restores ONLY while this very
    // dismissal is still the current one: a fresh card published meanwhile has
    // its own visibility state, and a stale restore would resurrect a card the
    // user just dismissed (or strip a newer dismissal of its owed teardown).
    bool RestoreShown(uint64_t dismiss_generation) {
        std::lock_guard<std::mutex> lk(mu_);
        if (dismiss_generation == kNoGeneration || dismiss_generation != generation_ ||
            !teardown_owed_) {
            return false;
        }
        visible_ = true;
        teardown_owed_ = false;
        return true;
    }

    // ANY thread: drop a pending teardown stamp (TeardownCard's idempotence
    // step; a duplicate message then finds nothing to do).
    void ClearDismissalOwed() {
        std::lock_guard<std::mutex> lk(mu_);
        teardown_owed_ = false;
    }

    // ANY thread (IsVisible() is read from the keyboard/mouse hook threads).
    // A short mutex acquisition, never a Win32 call inside the lock.
    bool IsVisible() const {
        std::lock_guard<std::mutex> lk(mu_);
        return visible_;
    }

    // Test/diagnostic seams (assert the coherent triple, never a torn pair).
    bool DismissalOwed() const {
        std::lock_guard<std::mutex> lk(mu_);
        return teardown_owed_;
    }
    uint64_t Generation() const {
        std::lock_guard<std::mutex> lk(mu_);
        return generation_;
    }

private:
    mutable std::mutex mu_;
    bool visible_ = false;
    bool teardown_owed_ = false;
    uint64_t generation_ = kNoGeneration;
};

// ---- 261116_0003 P9b-2: the dropdowns' "내 언어" user-row command band -----
//
// The tooltip's source/target dropdowns carry the same user-language group the
// tray's four menus do (plan §3 D5; the data is BuildLanguageMenuView's merge),
// in the TOOLTIP's OWN command space. The id spaces are NOT numerically
// disjoint from the tray's user band (2609..2864): this module's P6b special
// rows are 2701..2704, numerically inside that band (a raw tray-decode of
// 2701..2704 would read them as user rows). What the code pins instead is
// separation by construction: (1) the HMENU boundary: each dropdown is its
// own popup, appended and dispatched by the tooltip window alone
// (TrackPopupMenuEx + TPM_RETURNCMD), so an id is interpreted only by the
// dispatcher of the menu that appended it; (2) exact-match arms first: the
// tooltip dispatcher matches its 2701..2704 rows by exact comparison BEFORE
// the user-band range check, and its own decoder rejects them from that band
// (DecodeTooltipUserLangCommand accepts only [2900, 3028)), so the numeric
// overlap is resolved inside the only dispatcher that can ever see it;
// (3) cross-band rejection: DecodeTrayUserLangCommand accepts only
// [2609, 2865), so no tooltip user id (2900..) can ever decode as a tray row.
// The user rows start at 2900 with one 64-id slot per role (target directly
// after source); tooltip.cpp pins that layout with static_asserts, and the
// cross-surface non-collision is a runtime pin in the test suite. Both
// headers are visible together there AND in tooltip.cpp, which includes
// tray.hpp for the shared pin-to-row resolution (fix R14-02).
enum class TooltipUserLangRole : unsigned char {
    Source = 0, // the header source dropdown (include_auto = true)
    Target = 1, // the language button dropdown (include_auto = false)
};

inline constexpr UINT kTooltipCmdUserSrcBase = 2900;
inline constexpr UINT kTooltipCmdUserTgtBase =
    kTooltipCmdUserSrcBase + static_cast<UINT>(kMaxUserLanguages);
inline constexpr UINT kTooltipCmdUserBandEnd =
    kTooltipCmdUserTgtBase + static_cast<UINT>(kMaxUserLanguages); // exclusive

// Command id of one user row. Callers keep user_index < kMaxUserLanguages (the
// P9a cap the merge stops at).
inline constexpr UINT TooltipUserLangCommand(TooltipUserLangRole role, std::size_t user_index) {
    return (role == TooltipUserLangRole::Target ? kTooltipCmdUserTgtBase
                                                : kTooltipCmdUserSrcBase) +
           static_cast<UINT>(user_index);
}

struct TooltipUserLangCommandRef {
    TooltipUserLangRole role = TooltipUserLangRole::Source;
    std::size_t user_index = 0;
    bool decoded = false;
};

// Decodes a dispatch command back to (role, user index); decoded == false for
// every id outside the two slots, so no registry row (i + 1) or P6b special row
// can ever be read as a user row.
inline TooltipUserLangCommandRef DecodeTooltipUserLangCommand(UINT cmd) {
    TooltipUserLangCommandRef ref;
    if (cmd >= kTooltipCmdUserSrcBase && cmd < kTooltipCmdUserTgtBase) {
        ref.role = TooltipUserLangRole::Source;
        ref.user_index = static_cast<std::size_t>(cmd - kTooltipCmdUserSrcBase);
        ref.decoded = true;
    } else if (cmd >= kTooltipCmdUserTgtBase && cmd < kTooltipCmdUserBandEnd) {
        ref.role = TooltipUserLangRole::Target;
        ref.user_index = static_cast<std::size_t>(cmd - kTooltipCmdUserTgtBase);
        ref.decoded = true;
    }
    return ref;
}

class TooltipWindow {
public:
    using LanguageChangeCallback = std::function<void(std::string_view new_target_lang)>;

    // F8 (ADR-A1-4): source-language dropdown callback. Passes the picked
    // source as name_en ("Auto Detect" for the AUTO entry) - the same payload
    // form the target menu uses, so main.cpp routes both through the single
    // ApplyLanguageChange coordinator (persist -> tray check-marks -> tooltip).
    using SourceLanguageChangeCallback = std::function<void(std::string_view new_source_lang)>;

    TooltipWindow();
    ~TooltipWindow();

    bool Create(HINSTANCE hInstance);
    void Destroy();

    // Displays the tooltip with translated text at screen coordinates (x, y).
    // MAIN GUI THREAD ONLY: it drives the single-threaded D2D render target
    // (audit §3.4 / REQ-R10). Use ShowTranslationThreadSafe() from any other
    // thread (keyboard/mouse hook threads, the REQ-R06 double-Ctrl+C worker).
    //
    // R6 Phase 2 (B1-H1): `generation` carries the request sequence id stamped
    // by BeginTranslationRequest() at trigger time. A payload whose generation
    // is older than the latest-requested generation is DROPPED (never rendered)
    // so a slow, superseded translate thread can no longer overwrite a newer
    // result (the last-writer-wins stale-tooltip race). kGenNone (0) means
    // "unmanaged show" (synchronous GUI-thread callers, notices, tests): always
    // renders, keeping every pre-existing call site behaviorally unchanged.
    void ShowTranslation(
        int x, int y,
        std::wstring_view source_text,
        std::string_view source_lang_code,
        std::string_view target_lang,
        std::wstring_view translated_text,
        uint64_t generation = kGenNone
    );

    void Dismiss();
    // G-06 (round 2): visibility is owned by card_visibility_, whose
    // {visible, dismissal-owed, card generation} triple is one mutex-guarded
    // unit. Still callable from ANY thread (the keyboard/mouse hook threads
    // read it); the acquisition is a few loads, no Win32 call inside the lock.
    bool IsVisible() const { return card_visibility_.IsVisible(); }
    HWND GetHwnd() const { return hwnd_; }

    // ---- REQ-R08 (audit §3.2): transient message-bubble mode ----
    // Lightweight "state changed" notice (used for the Win+F9 toggle feedback):
    // compact D2D card with a header line and a body line; no copy/TTS/language
    // buttons. Auto-hides after kMessageAutohideMs unless dismissed sooner.
    // Same thread affinity as ShowTranslation (GUI thread only).
    // R6 Phase 2 (B1-H1): same generation guard as ShowTranslation - a failure
    // notice from a superseded translate request must not replace newer content.
    void ShowMessage(int x, int y, std::wstring_view header, std::wstring_view body,
                     uint64_t generation = kGenNone);

    // ---- REQ-R10 companions: thread-safe marshaling seams ----
    // Cross-thread ShowWindow()/D2D calls are unsafe from hook threads
    // (D2DERR_WRONG_THREAD, and ShowWindow can block on the GUI thread's pump
    // while it is mid-translation). These seams post the work to the tooltip's
    // own window (main GUI thread) via PostMessageW, which is non-blocking for
    // the caller. SEC-ADJ (release readiness 260911_0002, Blocker-5 class):
    // the transport is NO LONGER a heap pointer in LPARAM (CWE-822 shatter —
    // any same-session process could post an attacker-chosen address that the
    // WndProc dereferenced and freed). Payloads travel BY VALUE through the
    // mutex-guarded deques in tooltip.cpp; the posted WM_APP message is a pure
    // wake-up notification (wParam = lParam = 0, both ignored by WndProc).
    // On a failed post the entry stays queued and is consumed by the next
    // wake-up or by the DrainMarshalQueue() teardown sweep — nothing leaks and
    // nothing double-applies.
    // R6 Phase 2 (B1-H1): the generation travels INSIDE the payload; the drop
    // decision runs on the GUI thread in ShowTranslation/ShowMessage against
    // latest_request_gen_, so it is immune to both producer-thread races and
    // marshal-queue reordering (a stale payload that overtakes a fresh one is
    // still dropped on arrival).
    void ShowMessageThreadSafe(int x, int y, std::wstring_view header, std::wstring_view body,
                               uint64_t generation = kGenNone);
    void ShowTranslationThreadSafe(
        int x, int y,
        std::wstring_view source_text,
        std::string_view source_lang_code,
        std::string_view target_lang,
        std::wstring_view translated_text,
        uint64_t generation = kGenNone
    );
    void DismissThreadSafe();

    // ---- 261108_0002 (WS3, v0.10.6): floating back-translation card ----
    // User intent (verbatim): "역번역 플로팅 확인 UI: 번역 결과(BBBBB)를 원문
    // 언어로 자동 역번역(AAAAA')하여 플로팅 카드로 보여주기".
    //
    // The card is a THIRD mode on the same layered window (translation /
    // REQ-R08 notice / back-translation): showing it replaces whatever the
    // card was displaying, and the next ShowTranslation or ShowMessage
    // replaces it back. It never changes the existing translation card's
    // behavior beyond (a) the 360 DIP width reset in ShowTranslation and
    // (b) the dismiss-on-new-translation rule.
    //
    // GUI thread only (drives the single-threaded D2D target); the worker
    // thread uses ShowBackTranslationThreadSafe below.
    void ShowBackTranslation(const std::wstring& original_text,
                             const std::wstring& back_text,
                             const std::wstring& source_lang_label,
                             const std::wstring& target_lang_label,
                             bool meaning_matched,
                             HWND target_hwnd, HWND badge_hwnd);

    // Thread-safe entry: callable from the back-translation worker. Payload
    // travels BY VALUE through g_tt_backtrans_queue; the posted
    // kShowBackTranslationMessage is a pure (0, 0) wake-up (SEC-ADJ
    // Blocker-5 contract, identical to the seams above). A failed post leaves
    // the entry queued for the next wake-up or the teardown drain.
    void ShowBackTranslationThreadSafe(const std::wstring& original_text,
                                       const std::wstring& back_text,
                                       const std::wstring& source_lang_label,
                                       const std::wstring& target_lang_label,
                                       bool meaning_matched,
                                       HWND target_hwnd, HWND badge_hwnd);

    // ---- R6 Phase 2 (B1-H1): translation-request generation guard ----
    // Sentinel 0 = "generation-unmanaged show" (renders unconditionally).
    static constexpr uint64_t kGenNone = 0;

    // Stamp a NEW translate request and return its monotonic generation id.
    // Call at TRIGGER time (before any thread spawn / clipboard work) on the
    // thread that observed the user action, then pass the returned id to the
    // Show*ThreadSafe seam when the result lands. Thread-safe (atomic); never
    // returns kGenNone.
    uint64_t BeginTranslationRequest();

    // Pure staleness decision (headless-testable planner per plan §7.2):
    // render only when the payload is the newest request (or unmanaged).
    // newest-wins: payload_gen >= latest_requested -> show; older -> drop.
    static constexpr bool ShouldRenderForGeneration(uint64_t latest_requested,
                                                    uint64_t payload_gen) {
        if (payload_gen == kGenNone || latest_requested == kGenNone) {
            return true; // unmanaged show always renders
        }
        return payload_gen >= latest_requested;
    }

    // Test/inspection seams (GUI-thread-written values; the marshal tests pump
    // messages on the owning thread before reading).
    uint64_t LatestRequestGenerationForTest() const {
        return latest_request_gen_.load(std::memory_order_relaxed);
    }
    uint64_t DroppedStaleShowsForTest() const { return dropped_stale_shows_; }

    bool IsMessageMode() const { return is_message_mode_; }
    const std::wstring& GetMessageHeader() const { return message_header_; }

    // Marshaled message IDs. WM_APP+0x200 block: distinct from the drag icon's
    // WM_APP+0x100 block (src/ui/drag_icon.hpp) and the badge's WM_USER block.
    static constexpr UINT kShowTranslationMessage = WM_APP + 0x201;
    static constexpr UINT kShowMessageMessage     = WM_APP + 0x202;
    static constexpr UINT kDismissMessage         = WM_APP + 0x203;
    // REQ-002 (plan §2.1): wheel-forwarded scroll request. WPARAM unused,
    // LPARAM = raw WM_MOUSEWHEEL delta as signed int (WHEEL_DELTA multiples,
    // possibly fractional-notch values from precision touchpads). Posted from
    // the LL mouse hook callback (hook thread) — consumed on the GUI thread
    // (REQ-R10 marshaling discipline; the tooltip is WS_EX_NOACTIVATE so the
    // OS routes real WM_MOUSEWHEEL to the focused window, never to it).
    static constexpr UINT kScrollMessage          = WM_APP + 0x204;
    // R6 Phase 1 (B3): marshaled target-language view refresh posted by
    // RefreshTargetLanguageFromConfig() when called off the GUI thread.
    static constexpr UINT kRefreshTargetLangMessage = WM_APP + 0x205;
    // 261108_0002 (WS3, v0.10.6): marshaled floating back-translation card
    // show, posted by ShowBackTranslationThreadSafe from the back-translation
    // worker thread. Same by-value queue + parameter-less wake-up contract.
    static constexpr UINT kShowBackTranslationMessage = WM_APP + 0x206;
    // 261116_0003 P6b: the two language dropdowns' "검색…" row latches a
    // language-picker request and wakes THIS window with the picker module's
    // OWN id — language_palette's kMsgOpenLanguagePalette (WM_APP + 0x503,
    // language_search_palette.hpp), posted by PostDeferredLanguagePaletteOpen
    // to this window as its own controller (Option B; the tray's four surfaces
    // keep posting the same id to main.cpp's controller). The id is owned by
    // the picker module and is deliberately NOT re-declared in this band: a
    // same-value alias here could drift from the posted id with nothing to
    // catch it. The next FREE value in this band therefore remains +0x207.

    // ---- REQ-002 scrollable tooltip (architect plan §2.1) ----
    // GUI-thread entry for kScrollMessage (and direct WM_MOUSEWHEEL). Converts
    // the raw wheel delta to a DIP offset step, clamps against the
    // content/viewport extents, and re-renders only when the offset actually
    // moved. Marshals via PostMessageW off-thread (REQ-R10). No-op in message
    // mode (fixed-height REQ-R08 card) and when the body fits (not scrollable_).
    void ScrollByDipWheel(int wheel_delta);

    // ---- pure, headless-testable scroll math (all DIP, plan REQ-002/REQ-R15) ----
    // Max tooltip window height after this batch (was 480; body viewport is
    // kMaxWindowHeightDip - header - footer reserves, see ShowTranslation).
    static constexpr int kMaxWindowHeightDip = 520;
    // Body viewport top / footer reserve matching the Render layout constants
    // (body text rect ends at current_height_ - 44, see tooltip.cpp Render).
    static constexpr float kBodyTopDip = 48.0f;
    static constexpr float kFooterReserveDip = 44.0f;

    // Viewport height available for body text at a given window height (DIP).
    static constexpr float BodyViewportHeightDip(int window_height_dip) {
        const float vh = static_cast<float>(window_height_dip) - kBodyTopDip - kFooterReserveDip;
        return vh > 0.0f ? vh : 0.0f;
    }

    // Clamping scroll-offset rule: nothing to scroll (content fits the
    // viewport) pins the offset at 0; otherwise clamp into [0, content-viewport].
    static constexpr float ClampScrollOffset(float offset, float content_h, float viewport_h) {
        if (content_h <= viewport_h) return 0.0f;
        if (offset < 0.0f) return 0.0f;
        const float max_offset = content_h - viewport_h;
        return offset > max_offset ? max_offset : offset;
    }

    // Wheel delta -> offset step in DIP. One full notch (WHEEL_DELTA=120)
    // scrolls 3 body lines (Windows SPI_GETWHEELSCROLLLINES default); negative
    // for wheel-down (offset grows downward). Fractional deltas from precision
    // touchpads scale proportionally.
    static constexpr float WheelDeltaToOffsetStepDip(int wheel_delta, float line_height_dip) {
        return -static_cast<float>(wheel_delta) * (3.0f * line_height_dip / static_cast<float>(WHEEL_DELTA));
    }

    // Scrollbar thumb extent: viewport/content proportional, min 24 DIP,
    // never taller than the track.
    static constexpr float ScrollbarThumbHeightDip(float track_h, float viewport_h, float content_h) {
        if (content_h <= 0.0f || viewport_h <= 0.0f) return track_h;
        if (viewport_h >= content_h) return track_h;
        float h = track_h * viewport_h / content_h;
        if (h < 24.0f) h = 24.0f;
        return h > track_h ? track_h : h;
    }

    // Scrollbar thumb top: linear map of offset in [0, content-viewport] to
    // [track_top, track_top + track_h - thumb_h]. Degenerate ranges pin to the
    // track top (no jitter at the boundaries, plan §2.1 edge case 2).
    static constexpr float ScrollbarThumbTopDip(float track_top, float track_h, float thumb_h,
                                                float offset, float content_h, float viewport_h) {
        const float range = track_h - thumb_h;
        if (range <= 0.0f) return track_top;
        const float scroll_range = content_h - viewport_h;
        if (scroll_range <= 0.0f) return track_top;
        float frac = offset / scroll_range;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        return track_top + frac * range;
    }

    // ---- REQ-052 (audit P1): header target-button geometry planner ----
    // Pure/constexpr, headless-testable (same pattern as the scroll math
    // above). The target-language button starts after the source tag + arrow
    // run (28 DIP past the tag's left edge) but must NEVER reach the close
    // button: its right edge keeps kHeaderCloseGapDip clear of the close
    // button's left edge (card_width - 32), and its width is the measured
    // label width clamped to [kHeaderTargetMinWidthDip, max_width]. When even
    // the minimum width would not fit at the natural start, the start itself
    // is pulled left to close_left - (min + gap), which guarantees
    // max_width >= min (the button stays clickable on very wide source tags).
    static constexpr float kHeaderCloseGapDip = 8.0f;
    static constexpr float kHeaderTargetMinWidthDip = 40.0f;
    struct HeaderTargetButtonPlan {
        float x;
        float width;
    };
    static constexpr HeaderTargetButtonPlan PlanHeaderTargetButton(
        float src_tag_x, float src_tag_width, float card_width, float desired_width) {
        const float close_left = card_width - 32.0f;
        float x = src_tag_x + src_tag_width + 28.0f;
        const float x_limit = close_left - (kHeaderTargetMinWidthDip + kHeaderCloseGapDip);
        if (x > x_limit) x = x_limit;
        const float max_width = close_left - kHeaderCloseGapDip - x;
        float w = desired_width;
        if (w < kHeaderTargetMinWidthDip) w = kHeaderTargetMinWidthDip;
        if (w > max_width) w = max_width;
        return {x, w};
    }

    // ---- 261108_0002 (WS3, v0.10.6): pure back-translation card math ----
    // Same headless-testable planner idiom as the scroll math and
    // PlanHeaderTargetButton above: every decision that can be expressed
    // without an HWND lives here (constexpr where possible, so the header's
    // static_asserts prove the contract at compile time); only the anchor
    // TIER resolution stays Win32-bound in tooltip.cpp.
    static constexpr int kBackTransCardWidthDip = 360;
    // Legacy floor. The TWO-PANEL furniture added in v0.10.9 (labels + panel
    // padding, kBackTransContentTopDip and the constants below) is already
    // tall enough that this floor never binds on a real payload; it stays as
    // the documented degenerate-case clamp (and the empty-payload pin).
    static constexpr int kBackTransCardMinHeightDip = 96;
    // v0.10.9 (U-B, report §4 defect 3-2): 340 -> 480. The old cap left
    // 228..258 DIP of body space (~10-12 lines) for header + reference line +
    // padding; a slightly long back-translation was clipped with no way to
    // reach the rest. 480 matches the translation card's REQ-002 ceiling.
    static constexpr int kBackTransCardMaxHeightDip = 480;
    // 13pt SemiBold body in the 332 DIP text column (360 - 2*14 padding).
    // Deliberately a character-count estimate, not a DirectWrite measurement:
    // the GUI-thread show path refines the height with a real layout when a
    // DWrite factory exists, and this estimate is the headless floor.
    static constexpr int kBackTransCharsPerLine = 44;
    static constexpr int kBackTransLineHeightDip = 20;   // 13pt SemiBold body
    static constexpr int kBackTransRefLineDip = 16;      // 11.5pt [원문] body

    // ---- v0.10.9 (U-B, report §4 Option 2): two-panel vertical geometry ----
    // [ header row ][ divider ][ [역번역] label + body viewport ]
    //                          [ [원문] sub-panel: label + body viewport ]
    // EVERY value below is a whole DIP count held in an int, so the constexpr
    // planner is exact: the header's static_asserts (and the unit tests) can
    // compare whole numbers, and no float rounding can put a panel one DIP
    // over the card's bottom edge.
    //
    // kBackTransContentTopDip is where the [역번역] label band starts: the
    // header row (kBackTransRowTopDip 9 + kBackTransRowHeightDip 24 = 33), the
    // header divider at row_bottom + 4 = 37, and the 7 DIP gap the pre-v0.10.9
    // renderer already used before the body.
    static constexpr int kBackTransContentTopDip = 44;
    static constexpr int kBackTransSectionLabelDip = 18;  // label band
    static constexpr int kBackTransSectionGapDip = 2;     // label -> its body
    static constexpr int kBackTransPanelTopGapDip = 8;    // back body -> [원문] panel
    static constexpr int kBackTransPanelPadDip = 6;       // [원문] panel inner pad
    static constexpr int kBackTransPanelBottomPadDip = 6;
    static constexpr int kBackTransCardBottomPadDip = 12;
    // Degenerate-clamp basis, NOT a per-panel allocation any more (v0.10.9 fix
    // round 3): the overflow split reserves the ORIGINAL's measured height
    // first (see kBackTransRefSharePct below), so no floor is imposed on
    // either panel. What survives is the guard for a body area smaller than
    // two of these (only reachable through a pathologically tall wrapped pill
    // row); a real payload always leaves the natural height in charge.
    static constexpr int kBackTransMinPanelBodyDip = 24;
    // v0.10.9 fix round 3 (H-1, QA finding F3): the [원문] panel's share cap.
    // The original reserves its MEASURED height, so a short original is always
    // visible whole without scrolling; 45% of the card body is the most it may
    // reserve while the back-translation still needs room (the remainder is
    // >= 55%, so the long side always keeps the larger viewport). On a
    // full-height card that is 364 * 45 / 100 = 163 DIP of original text,
    // ~10 back-translation lines' worth, next to a 201 DIP body viewport.
    static constexpr int kBackTransRefSharePct = 45;
    // Shared scrollbar: one thumb for the ONE shared scroll offset, drawn on
    // the card's right edge across both panel viewports.
    static constexpr int kBackTransScrollbarWDip = 6;
    static constexpr int kBackTransScrollbarGutterDip = 10; // track + gap (text column)

    // The card's fixed vertical furniture: everything in the column that never
    // carries body text (both label bands, the [원문] panel's padding, the
    // card's bottom padding).
    static constexpr int kBackTransFurnitureDip =
        kBackTransSectionLabelDip + kBackTransSectionGapDip +  // [역번역] band
        kBackTransPanelTopGapDip +
        kBackTransPanelPadDip +
        kBackTransSectionLabelDip + kBackTransSectionGapDip +  // [원문] band
        kBackTransPanelBottomPadDip +
        kBackTransCardBottomPadDip;

    struct BackTransCardSize {
        int width;
        int height;
    };

    // The two-panel layout, computed ONCE in ShowBackTranslation from the
    // DirectWrite-measured content heights and consumed verbatim by Render
    // (measure/render lockstep by construction - the same single-source-of-
    // truth discipline as BackTransHeaderPlan). The two viewports are the
    // clipped regions; the shared scroll offset is applied to BOTH, and each
    // panel additionally clamps to its own extent, so the shorter panel pins
    // at its end instead of scrolling into blank space.
    struct BackTransPanelPlan {
        int card_height_dip = kBackTransCardMinHeightDip;
        int content_top_dip = kBackTransContentTopDip;
        int back_label_top = kBackTransContentTopDip;
        int back_view_top = kBackTransContentTopDip + kBackTransSectionLabelDip +
                            kBackTransSectionGapDip;
        int back_view_h = 0;
        int ref_panel_top = 0;
        int ref_label_top = 0;
        int ref_view_top = 0;
        int ref_view_h = 0;
        int back_content_h = 0;    // measured body extent (may exceed the viewport)
        int ref_content_h = 0;
        int body_area_dip = 0;     // vertical body budget the split divided
        int shared_viewport_h = 0; // max(view_h) - the shared scroll clamp basis
        int max_scroll_offset = 0; // max(content - view) over both panels
        bool scrollable = false;
    };

    // ---- v0.10.9 (U-B): the pure two-panel planner ----
    // Inputs are the MEASURED content heights (rounded up to whole DIP by the
    // caller) and the wrapped-pill row height of the header plan. Output: the
    // clamped card height plus both viewports, which by construction satisfy
    //   ref_view_top + ref_view_h + kBackTransPanelBottomPadDip
    //       + kBackTransCardBottomPadDip == card_height_dip
    // (the panels ALWAYS fit the card; overflow becomes scroll, never clip).
    //
    // Overflow split (v0.10.9 fix round 3, H-1 / QA F3): the [원문] panel
    // reserves min(its measured height, kBackTransRefSharePct% of the body
    // area) FIRST and the back-translation takes the remainder, so a 3-line
    // original beside a 20-line back-translation is visible whole at offset 0
    // instead of being reachable only by scrolling. When the back-translation
    // needs less than that remainder, the surplus flows back to the original
    // (so a very long original still wins the space when IT is the long side),
    // and neither panel is ever given more than its measured height. Integer
    // arithmetic throughout: the two viewports add up to exactly
    // body_area_dip, so the panels close the card by construction.
    static constexpr BackTransPanelPlan PlanBackTransPanels(int back_content_h_dip,
                                                            int ref_content_h_dip,
                                                            int header_extra_h_dip = 0) {
        BackTransPanelPlan p;
        const int back_c = back_content_h_dip > 0 ? back_content_h_dip : 0;
        const int ref_c = ref_content_h_dip > 0 ? ref_content_h_dip : 0;
        const int extra = header_extra_h_dip > 0 ? header_extra_h_dip : 0;
        p.content_top_dip = kBackTransContentTopDip + extra;
        p.back_content_h = back_c;
        p.ref_content_h = ref_c;

        const int natural = p.content_top_dip + kBackTransFurnitureDip + back_c + ref_c;
        int height = natural < kBackTransCardMinHeightDip ? kBackTransCardMinHeightDip : natural;
        if (height > kBackTransCardMaxHeightDip) height = kBackTransCardMaxHeightDip;
        p.card_height_dip = height;

        int body_area = height - p.content_top_dip - kBackTransFurnitureDip;
        if (body_area < 2 * kBackTransMinPanelBodyDip) {
            body_area = 2 * kBackTransMinPanelBodyDip;
        }
        p.body_area_dip = body_area;

        int back_view = 0;
        int ref_view = 0;
        if (back_c + ref_c <= body_area) {
            // Everything fits at this card height: no scrolling, natural sizes.
            back_view = back_c;
            ref_view = ref_c;
        } else {
            // H-1: the original reserves its measured height up to the cap; the
            // back-translation takes the remainder (never more than it needs).
            // `body_area` can only be clamped up in the FITS branch above (a
            // real overflow always grows the card past funding both bodies),
            // so the two viewports below always sum to the real body area and
            // the panels close the card exactly.
            p.scrollable = true;
            const int ref_cap = body_area * kBackTransRefSharePct / 100;
            ref_view = ref_c < ref_cap ? ref_c : ref_cap;
            back_view = body_area - ref_view;
            if (back_view > back_c) {
                // The back-translation is satisfied by less than its remainder:
                // the surplus returns to the original, which is now the only
                // panel that can still overflow.
                back_view = back_c;
                ref_view = body_area - back_view;
            }
        }
        p.back_view_h = back_view;
        p.ref_view_h = ref_view;
        p.back_label_top = p.content_top_dip; // the wrapped-pill row pushes it down
        p.back_view_top = p.back_label_top + kBackTransSectionLabelDip + kBackTransSectionGapDip;
        p.ref_panel_top = p.back_view_top + back_view + kBackTransPanelTopGapDip;
        p.ref_label_top = p.ref_panel_top + kBackTransPanelPadDip;
        p.ref_view_top = p.ref_label_top + kBackTransSectionLabelDip + kBackTransSectionGapDip;
        p.shared_viewport_h = back_view > ref_view ? back_view : ref_view;
        const int over_back = back_c - back_view;
        const int over_ref = ref_c - ref_view;
        const int over = over_back > over_ref ? over_back : over_ref;
        p.max_scroll_offset = over > 0 ? over : 0;
        return p;
    }

    // ---- 261108_0002 (FIX-WS3B, v0.10.6): header-row layout planner ----
    // The card row is [header text] [verdict pill] [✕] inside a FIXED 360 DIP
    // width. In long-string locales (Italian "Verifica retrotraduzione" +
    // "Controlla la sfumatura", German "Rückübersetzungs-Prüfung" + "Nuance
    // prüfen") the header and the pill overlapped on the shipped v0.10.6
    // card. The rule below is the fix, expressed as pure geometry so it is
    // headless-testable:
    //   1. both fit on one row          -> SingleRow
    //   2. they do not                  -> PillOnOwnRow (pill left-aligned
    //                                      under the header, same styling;
    //                                      the ✕ stays on the header row)
    //   3. the header alone still does
    //      not fit on its row           -> EllipsizeHeader (also a pill row:
    //                                      the pill never shares a row with a
    //                                      clipped header)
    // The pill is NEVER clipped: its box is the measured/estimated width.
    static constexpr float kBackTransPadDip = 14.0f;       // card side padding
    static constexpr float kBackTransCloseW = 20.0f;       // ✕ box width
    static constexpr float kBackTransCloseRightDip = 12.0f; // ✕ to card right edge
    static constexpr float kBackTransCloseGapDip = 8.0f;   // pill -> ✕ gap
    static constexpr float kBackTransHeaderGapDip = 8.0f;  // header -> pill gap
    static constexpr float kBackTransRowTopDip = 9.0f;
    static constexpr float kBackTransRowHeightDip = 24.0f;
    static constexpr float kBackTransPillPadDip = 16.0f;   // pill horizontal padding
    static constexpr float kBackTransPillMinWidthDip = 96.0f;
    static constexpr float kBackTransPillRowGapDip = 6.0f; // header row -> pill row
    // Per-character width estimates (DIP) used ONLY by the character-count
    // entry point below. The live show path prefers a real DirectWrite
    // measurement and falls back to these; they are a lower bound for CJK
    // (wide glyphs), which is safe: an under-estimate can only keep a row
    // single when the measurement would also keep it single.
    static constexpr float kBackTransHeaderCharDip = 6.6f; // 12pt SemiBold
    static constexpr float kBackTransPillCharDip = 6.4f;   // 11.5pt Medium

    enum class BackTransHeaderLayout { SingleRow, PillOnOwnRow, EllipsizeHeader };

    struct BackTransHeaderPlan {
        BackTransHeaderLayout layout;
        float header_width_dip; // width available to the header text
        float pill_width_dip;   // pill box width (never clipped)
        float header_row_h;     // header row height (constant)
        float pill_row_h;       // EXTRA height the wrapped pill row adds (0 single-row)
        bool ellipsize;         // the header must be trimmed to header_width_dip
    };

    // Geometry core, in DIP. Both public entry points funnel through here, so
    // the measured live path and the character-count test path can never
    // disagree about the RULE - only about the inputs.
    static constexpr BackTransHeaderPlan PlanBackTransHeaderRowDip(float header_w_dip,
                                                                  float pill_w_dip,
                                                                  float row_width_dip) {
        const float close_left = row_width_dip - kBackTransCloseRightDip - kBackTransCloseW;
        // Pill box: its measured width, floored, and never wider than the
        // card's content column (a pill wider than the card cannot be shown
        // without clipping, which the contract forbids).
        float pill_w = pill_w_dip;
        if (pill_w < kBackTransPillMinWidthDip) pill_w = kBackTransPillMinWidthDip;
        const float content_w = row_width_dip - 2.0f * kBackTransPadDip;
        if (pill_w > content_w) pill_w = content_w;

        const float single_row_header_w =
            close_left - kBackTransCloseGapDip - pill_w - kBackTransHeaderGapDip - kBackTransPadDip;
        const float own_row_header_w =
            close_left - kBackTransCloseGapDip - kBackTransPadDip;

        if (header_w_dip <= single_row_header_w) {
            return {BackTransHeaderLayout::SingleRow, single_row_header_w, pill_w,
                    kBackTransRowHeightDip, 0.0f, false};
        }
        const float extra = kBackTransPillRowGapDip + kBackTransRowHeightDip;
        if (header_w_dip <= own_row_header_w) {
            return {BackTransHeaderLayout::PillOnOwnRow, own_row_header_w, pill_w,
                    kBackTransRowHeightDip, extra, false};
        }
        return {BackTransHeaderLayout::EllipsizeHeader, own_row_header_w, pill_w,
                kBackTransRowHeightDip, extra, true};
    }

    // Character-count entry point (the headless/testable form the unit tests
    // drive): converts both strings to an estimated DIP width first.
    static constexpr BackTransHeaderPlan PlanBackTransHeaderRow(int header_cp, int pill_cp,
                                                               int row_width_dip) {
        const int hcp = header_cp > 0 ? header_cp : 0;
        const int pcp = pill_cp > 0 ? pill_cp : 0;
        return PlanBackTransHeaderRowDip(static_cast<float>(hcp) * kBackTransHeaderCharDip,
                                         static_cast<float>(pcp) * kBackTransPillCharDip +
                                             kBackTransPillPadDip,
                                         static_cast<float>(row_width_dip));
    }

    // Card extent from the two text lengths (character counts, not DIP).
    // Monotonic non-decreasing in BOTH lengths by construction: each extra
    // wrapped line adds exactly one line height, and both are clamped to
    // [min, max] so a pathological body cannot grow the card off-screen.
    // `header_extra_h_dip` is the wrapped-pill row height from
    // PlanBackTransHeaderRow (0 keeps the single-row card byte-identical).
    //
    // v0.10.9 (U-B): this estimate now funnels through the SAME two-panel
    // planner the measured show path uses, so the headless floor and the live
    // card can never disagree about the layout rule - only about the input
    // heights (character-count lines here, DirectWrite metrics there).
    static constexpr BackTransCardSize ComputeBackTransCardSize(int back_text_len,
                                                                int ref_text_len,
                                                                int header_extra_h_dip = 0) {
        const int back_len = back_text_len > 0 ? back_text_len : 0;
        const int ref_len = ref_text_len > 0 ? ref_text_len : 0;
        int back_lines = (back_len + kBackTransCharsPerLine - 1) / kBackTransCharsPerLine;
        if (back_lines < 1) back_lines = 1;
        int ref_lines = (ref_len + kBackTransCharsPerLine - 1) / kBackTransCharsPerLine;
        if (ref_lines < 1) ref_lines = 1;
        const BackTransPanelPlan plan =
            PlanBackTransPanels(back_lines * kBackTransLineHeightDip,
                                ref_lines * kBackTransRefLineDip, header_extra_h_dip);
        return {kBackTransCardWidthDip, plan.card_height_dip};
    }

    // ---- hover pause / auto-dismiss state machine ----
    // Spec (261108_0002): auto-dismiss, PAUSED while the pointer is over the
    // card, resumed with a grace timer when it leaves. Expressed as a pure
    // planner so the WndProc branch is a single dispatch and the whole
    // contract is unit-testable without a window.
    //
    // v0.10.9 (U-B, report §4 defect 3-5c): the fixed 4500 ms was shorter than
    // the time a long comparison card takes to read, so the card closed while
    // the user was still reading it. The delay is now ADAPTIVE - 4500 ms for a
    // short card, +10 ms per displayed character beyond kBackTransAutohideFreeChars,
    // saturating at 12000 ms (750 extra characters / 870 displayed) - and any
    // wheel or drag scroll re-arms it (RearmBackTransAutohideForScroll).
    // kBackTransAutohideMs stays the FLOOR and the planner's default, so every
    // pre-existing call site keeps its exact short-card timing.
    static constexpr DWORD kBackTransAutohideMs = 4500;
    static constexpr DWORD kBackTransAutohideMaxMs = 12000;
    static constexpr std::size_t kBackTransAutohideFreeChars = 120;
    static constexpr DWORD kBackTransAutohideMsPerChar = 10;
    static constexpr DWORD kBackTransHoverGraceMs = 1500;

    // Pure, constexpr, overflow-free: the per-character ramp saturates BEFORE
    // the multiply, so no displayed-character count can wrap the delay.
    static constexpr DWORD BackTransAutohideMsForContent(std::size_t displayed_chars) {
        const std::size_t extra = displayed_chars > kBackTransAutohideFreeChars
                                      ? displayed_chars - kBackTransAutohideFreeChars
                                      : 0;
        const std::size_t ramp_chars =
            (kBackTransAutohideMaxMs - kBackTransAutohideMs) / kBackTransAutohideMsPerChar;
        if (extra >= ramp_chars) return kBackTransAutohideMaxMs;
        return static_cast<DWORD>(kBackTransAutohideMs +
                                  static_cast<DWORD>(extra) * kBackTransAutohideMsPerChar);
    }

    enum class BackTransTimerEvent {
        Shown,        // the card just appeared
        PointerMoved, // WM_MOUSEMOVE over the card
        PointerLeft,  // WM_MOUSELEAVE
        Dismissed,    // close button / ESC / next translation
    };
    enum class BackTransTimerAction {
        None,          // nothing to do (already paused, or already dismissed)
        ArmAutohide,   // SetTimer(kTimerBackTranslateAutohide, kBackTransAutohideMs)
        PauseAutohide, // KillTimer(kTimerBackTranslateAutohide)
        ArmGrace,      // SetTimer(kTimerBackTranslateAutohide, kBackTransHoverGraceMs)
        KillTimers,    // KillTimer on the back-translation timer
    };
    struct BackTransTimerPlan {
        BackTransTimerAction action;
        DWORD delay_ms;
    };

    static constexpr BackTransTimerPlan PlanBackTransTimer(BackTransTimerEvent ev,
                                                           bool hover_paused,
                                                           DWORD autohide_ms = kBackTransAutohideMs) {
        switch (ev) {
            case BackTransTimerEvent::Shown:
                // The caller passes BackTransAutohideMsForContent(displayed
                // chars); the default keeps the short-card floor.
                return {BackTransTimerAction::ArmAutohide, autohide_ms};
            case BackTransTimerEvent::PointerMoved:
                // The first move over the card pauses the countdown; further
                // moves are no-ops (no timer churn per mouse pixel).
                return hover_paused ? BackTransTimerPlan{BackTransTimerAction::None, 0}
                                    : BackTransTimerPlan{BackTransTimerAction::PauseAutohide, 0};
            case BackTransTimerEvent::PointerLeft:
                // Always the grace floor: a hover that already ran past the
                // autohide window must not extend the card indefinitely.
                return {BackTransTimerAction::ArmGrace, kBackTransHoverGraceMs};
            case BackTransTimerEvent::Dismissed:
                return {BackTransTimerAction::KillTimers, 0};
        }
        return {BackTransTimerAction::None, 0};
    }

    // ---- pure placement math (PHYSICAL px, the ClampWindowOrigin contract) ----
    // Centres the card on the anchor's horizontal midpoint, `gap_px` above (or
    // below) it, flips to the other side when the preferred side would leave
    // the work area, then applies the shared 10 px edge clamp. Every anchor
    // tier funnels through this one function, so the three tiers can never
    // disagree about geometry.
    static constexpr POINT PlaceBackTransCard(const RECT& anchor, bool above, int gap_px,
                                              int card_w_px, int card_h_px,
                                              const RECT& work) {
        const int anchor_w = static_cast<int>(anchor.right - anchor.left);
        int x = static_cast<int>(anchor.left) + (anchor_w - card_w_px) / 2;
        int y = above ? static_cast<int>(anchor.top) - gap_px - card_h_px
                      : static_cast<int>(anchor.bottom) + gap_px;
        if (above && y < work.top) {
            y = static_cast<int>(anchor.bottom) + gap_px; // no room above: drop below
        } else if (!above && y + card_h_px > work.bottom) {
            y = static_cast<int>(anchor.top) - gap_px - card_h_px; // mirror flip
        }
        return ui::ClampWindowOrigin(x, y, card_w_px, card_h_px, 10, work);
    }

    // ---- pure label formatting (string work, so .cpp-defined) ----
    // Header line: localized title + " (SRC → TGT)", e.g.
    // "🔄 역번역 확인 (EN → KO)". Empty labels are dropped rather than
    // rendered as "()" so a caller that could not resolve a language still
    // gets a clean title.
    static std::wstring FormatBackTransHeader(const std::wstring& title,
                                              const std::wstring& source_label,
                                              const std::wstring& target_label);
    // Policy seam: does the card need an [원문] (original) panel body at all?
    // (The pre-v0.10.9 "↳ 원문: AAAAA" reference LINE - and its
    // FormatBackTransReference formatter - is gone: the original now lives in
    // its own labelled [원문] sub-panel fed by StringId::BackTransSectionOriginal,
    // so prefixing the text with a second localized label would say the same
    // thing twice. This predicate is what gates that panel's body.)
    //
    // ---- v0.10.9 (U-B, report §4 defect 3-5a): the original is ALWAYS shown ----
    // The pre-v0.10.9 rule hid the reference line whenever the round trip was
    // exact (AAAAA' == AAAAA) to avoid repeating the body. That reasoning is
    // void: the input box already REPLACED the user's text with the
    // translation, so the card is the only place the original still exists -
    // hiding it exactly when the round trip was perfect removed the
    // comparison the card is FOR. The policy is now "show it whenever there is
    // an original to show"; `back_text` is kept in the signature (and
    // deliberately ignored) so callers and the pinned seam keep one shape.
    static bool BackTransNeedsReferenceLine(const std::wstring& original_text,
                                           const std::wstring& back_text);

    // Test/inspection seams for the REQ-002 scroll state (values are written
    // only on the GUI thread; tests pump messages on the owning thread).
    bool IsScrollableForTest() const { return scrollable_; }
    float ScrollOffsetForTest() const { return scroll_offset_dip_; }
    // 261108_0002 (WS3): back-translation card inspection seams (GUI-thread
    // written; the marshal tests pump on the owning thread before reading).
    bool IsBackTranslationMode() const { return is_backtrans_mode_; }
    const std::wstring& GetBackTransText() const { return backtrans_text_; }
    const std::wstring& GetBackTransHeader() const { return backtrans_header_; }
    // v0.10.9 (U-B): the [원문] panel BODY - the raw original text the card
    // shows (empty only when the caller passed an empty original). The
    // pre-v0.10.9 "↳ 원문: " inline prefix is gone: the panel carries its own
    // localized [원문] section header.
    const std::wstring& GetBackTransReferenceLine() const { return backtrans_ref_line_; }
    bool IsBackTransHoverPaused() const { return backtrans_hover_paused_; }
    bool IsBackTransMatched() const { return backtrans_matched_; }
    float ContentHeightForTest() const { return content_height_dip_; }
    // v0.10.9 (U-B): the two-panel layout actually measured for the CURRENT
    // card (render consumes this verbatim) and the adaptive autohide delay
    // armed for it.
    const BackTransPanelPlan& GetBackTransPanelPlanForTest() const {
        return backtrans_panel_plan_;
    }
    DWORD GetBackTransAutohideMsForTest() const { return backtrans_autohide_ms_; }

    // SEC-ADJ (Blocker-5 class fix): these payloads NO LONGER travel as heap
    // pointers in LPARAM. They are value types enqueued by the posting seams
    // into the mutex-guarded deques in tooltip.cpp (one deque per payload
    // type, each with its own mutex so a wake-up on one message id never
    // blocks on another), and popped one-at-a-time by the WndProc handlers
    // (per-item lock: entries enqueued while an earlier one is being applied
    // are still consumed by the same wake-up loop). The struct definitions
    // are unchanged so every producer/test call site keeps compiling.
    // R6 Phase 2 (B1-H1): both carry the originating request generation
    // (kGenNone default = unmanaged, always renders - existing producers and
    // test payloads that never set it keep their exact behavior).
    struct TranslationPayload {
        int x;
        int y;
        std::wstring source_text;
        std::string source_lang_code;
        std::string target_lang;
        std::wstring translated_text;
        uint64_t generation = kGenNone;
    };
    struct MessagePayload {
        int x;
        int y;
        std::wstring header;
        std::wstring body;
        uint64_t generation = kGenNone;
    };
    // R6 B3: marshaled RefreshTargetLanguageFromConfig payload (value-queue
    // transport, same SEC-ADJ contract as the seams above).
    struct TargetLangPayload {
        std::string target_lang;
    };
    // 261108_0002 (WS3, v0.10.6): marshaled floating back-translation card
    // payload. Same value-queue transport + parameter-less wake-up contract as
    // the three above; HWNDs travel inside the value (never as an LPARAM
    // pointer), and the GUI-thread handler re-validates them with IsWindow
    // before anchoring.
    struct BackTranslationPayload {
        std::wstring original_text;
        std::wstring back_text;
        std::wstring source_lang_label;
        std::wstring target_lang_label;
        bool meaning_matched = false;
        HWND target_hwnd = nullptr;
        HWND badge_hwnd = nullptr;
    };
    // Test/inspection seam: injects a caller-provided payload VALUE into the
    // same value queue the hook/worker threads use for `msg`, then posts the
    // identical parameter-less wake-up notification. The WndProc drains and
    // applies it on the next pump. `payload` must point at a live Translation/
    // Message/TargetLangPayload matching `msg` (the seam copies it by value;
    // ownership never transfers). Returns false on null hwnd, unsupported msg,
    // or PostMessage failure.
    static bool PostPayloadForTest(HWND hwnd, UINT msg, const void* payload);

    // R6 Phase 3 (audit items 6+8) / SEC-ADJ: drains the three value queues
    // (Translation/Message/TargetLang), discarding still-queued payloads.
    // Destroy() calls it on the GUI thread right before DestroyWindow, and it
    // replicates the old PeekMessage purge semantics exactly: messages are
    // REMOVED without running the apply path (discard, not apply). With the
    // by-value deques there is no heap to free; clearing the deque is what
    // releases the string memory the old `delete` did. Returns the number of
    // payloads removed (test seam for the drain invariant).
    int DrainMarshalQueue();

    void SetLanguageChangeCallback(LanguageChangeCallback cb) { lang_change_cb_ = std::move(cb); }

    // F8 (ADR-A1-4): register the source-language dropdown pick handler.
    void SetSourceLanguageChangeCallback(SourceLanguageChangeCallback cb) { src_lang_change_cb_ = std::move(cb); }

    // ---- 261116_0003 P6b: engine-view resolver seam (the tray's REQ-056
    // derive-at-open pattern, narrowed to what the two dropdowns need) ----
    // The floating tooltip's source/target language menus build from the
    // ACTIVE engine's registry view (config.hpp GetLanguagesForEngine) instead
    // of the global lists, so a user on the local Hy-MT2 engine no longer sees
    // cloud-only rows and a cloud user no longer sees Hy-MT2-only rows — the
    // tooltip must agree with the tray's engine-filtered menus. main.cpp wires
    // the resolver in P6c by mapping the persisted engine_type through the
    // tray's engine mapper (tray.hpp) into a LanguageEngine, resolved AT
    // MENU-BUILD TIME (derive-at-open: no cached engine state can go stale,
    // mirroring tray.cpp ShowContextMenu's resolver invocation). The tooltip
    // itself NEVER calls the tray's mapper — the resolver hands it the values
    // DIRECTLY (no tray index crosses this seam), and the P6b pins grep-prove
    // the absence of both tray mapper names in tooltip.*.
    //
    // 261116_0003 P6b fix round (PR6-28), re-anchored to the landed P6c
    // state (PR6-43 sweep): the seam is WIDENED from a bare
    // LanguageEngine to MenuEngineResolution — the engine PLUS the two
    // router-aware inputs the tray's ghost gate composes (is_auto = the RAW
    // persisted "auto" token, PR6-07; local_host_available = the router's own
    // HostCouldServeFor components, PR6-19). Both dropdowns feed all three
    // into the SHARED ghost predicate
    // (language_palette::SuppressPinnedAbsentGhost, ONE definition consumed by
    // BOTH surfaces). The DECISION is shared; the INPUTS are not — each
    // surface resolves its own view at its own menu-open time, and since P6c
    // (PR6-35) BOTH resolver halves are wired from main.cpp (the tray's and
    // this SetEngineResolver), deriving all three values from the same
    // persisted config snapshot. That shared derivation source is why the two
    // surfaces agree in practice; the contract stays
    // shared-DECISION / unshared-INPUTS, so each surface keeps feeding its own
    // live view rather than a cached copy.
    //
    // NO-RESOLVER FALLBACK (the defensive default, not a runtime state since
    // P6c): MenuEngineResolution{} — LanguageEngine::Google, is_auto = false,
    // local_host_available = false, all three the CONSERVATIVE values. The
    // fallback view is the GOOGLE list: a SUPERSET of the pre-P6b global list
    // for the cloud rows, but it OMITS the Hy-MT2-only rows — BO (Tibetan)
    // carries no kLangGoogle (config.cpp:133), so a no-resolver tooltip offers
    // strictly FEWER languages for those rows than the pre-P6b global list did
    // (P6b F-05's known narrowing, stated here rather than papered over,
    // PR6-34). The shared predicate's default ("do not suppress") keeps the
    // truthful single-view ghost. This mirrors tray.cpp's no-resolver
    // documentation (a no-resolver tray renders the Google engine row checked
    // and the single-view ghost). The comment is part of the contract: the
    // fallback defaults are load-bearing (main.cpp wires the real resolver in
    // P6c, and they are pinned by the P6b source-scan tests).
    struct MenuEngineResolution {
        LanguageEngine engine = LanguageEngine::Google;
        bool is_auto = false;              // RAW persisted "auto" token (PR6-07)
        bool local_host_available = false; // HostCouldServeFor components (PR6-19)
        // 261116_0003 P9b-2: the config's registered user languages, COPIED by
        // value at dropdown-build time (the same derive-at-open contract as the
        // three fields above; config.hpp documents the member as GUI-thread
        // only, and this resolver runs on the GUI thread). The dropdown builds
        // hand this vector to BuildLanguageMenuView and keep it alive through
        // the menu's dispatch, so the "내 언어" rows' `const UserLanguage*`
        // pointers stay valid for the whole TrackPopupMenuEx call. Empty on a
        // tooltip with no resolver wired: no group, no user ghost, and the
        // dropdowns are exactly the pre-P9b-2 shape.
        std::vector<UserLanguage> user_languages;
        // 261116_0003 P9b-3 U4a: the badge key + the runtime in-flight seam —
        // the SAME derivation the tray's EngineMenuView carries (shared
        // decision, unshared inputs: this surface derives its own view from
        // the same persisted sources). Engine half = UserLanguageEngineId
        // ("" = no-probe context, no badge), model half = the OpenAiConfig
        // model / the user-.gguf registry id the U3 runner writes under. Both
        // stay empty on a tooltip whose resolver does not populate them (no
        // suffix; the conservative default). The in-flight seam answers per
        // entry (the stored-name keying lives inside the seam, main.cpp);
        // EMPTY = no resolver -> no row is in flight.
        std::string user_lang_probe_engine;
        std::string user_lang_probe_model;
        std::function<bool(const UserLanguage&)> user_lang_probe_in_flight;
    };
    using EngineResolver = std::function<MenuEngineResolution()>;
    void SetEngineResolver(EngineResolver resolver) {
        engine_resolver_ = std::move(resolver);
    }

    // ---- R6 Phase 1 (B3): single-source-of-truth language sync (plan §2.4) ----
    // View-refresh seam called by the main.cpp ApplyLanguageChange coordinator
    // whenever ANY surface (tray menu, swap, cycle, config load) mutates the
    // persisted target language. Best-effort by design: a no-op while hidden
    // or in REQ-R08 message mode (the compact notice has no language button);
    // otherwise it re-labels the target button and re-renders WITHOUT touching
    // the translated body (re-translation on external change is out of scope -
    // the plan calls for view sync, not content churn). Thread-safe: posts to
    // the GUI-thread WndProc through kRefreshTargetLangMessage like the other
    // REQ-R10 seams, so hook/worker-thread coordinators can call it directly.
    void RefreshTargetLanguageFromConfig(std::string_view new_target);

    // Repaints the tooltip immediately when the active UI theme changes.
    void RefreshTheme();

    // Audio & Clipboard actions
    void SpeakCurrentText();
    void StopTTS();
    void CopyToClipboard();

    // Voice selection for multi-language TTS
    //
    // P4 Batch-3 (REQ-C-002, design §1.2.2 / decision D8) return contract:
    // true  = a language-matching voice was selected;
    // false = no voice for the language (unresolvable LCID or zero installed
    // matches); voice state unchanged - the forced default-voice fallback is
    // REMOVED (that was the silent wrong-language reading defect) and the
    // caller decides the UX (SpeakCurrentText shows ShowNoVoiceNotice and
    // speaks nothing).
    bool SelectVoiceForLanguage(std::string_view target_lang_name_or_code);
    const std::string& GetCurrentVoiceName() const { return current_voice_name_; }
    const std::string& GetCurrentVoiceLanguage() const { return current_voice_lang_; }

    // ---- P4 Batch-3 (REQ-C-001, design §1.2.1 / decision D6): voice score
    // lattice. Pure + constexpr, compile-time-proven below (same pattern as
    // the scroll math section). cat_idx is the SelectVoiceForLanguage
    // kCategories enumeration order: 0 = SPCAT_VOICES (legacy SAPI desktop
    // voices), 1 = Speech_OneCore (modern high-quality Windows voices).
    // Historical defect: SAPI base 80 outranked OneCore base 40, so a legacy
    // mechanical voice won over an installed OneCore voice for the same
    // language. Fixed lattice: OneCore 100 / SAPI 50 base, the exact-LCID
    // bonus (+20, match == 2 per MatchTokenLanguage) preserved, so a SAPI
    // voice can still win only when OneCore has no language match at all.
    static constexpr int kVoiceCategorySapi = 0;
    static constexpr int kVoiceCategoryOneCore = 1;
    static constexpr int kVoiceBaseScoreSapi = 50;
    static constexpr int kVoiceBaseScoreOneCore = 100;
    static constexpr int kVoiceExactMatchBonus = 20;

    static constexpr int VoiceBaseScore(int cat_idx) {
        return cat_idx == kVoiceCategoryOneCore ? kVoiceBaseScoreOneCore : kVoiceBaseScoreSapi;
    }

    // match: 2 = exact LCID, 1 = primary-language match (MatchTokenLanguage).
    static constexpr int VoiceTotalScore(int cat_idx, int match) {
        return VoiceBaseScore(cat_idx) + (match == 2 ? kVoiceExactMatchBonus : 0);
    }
    // NOTE: the compile-time lattice proofs (static_assert 120/100/70/50) live
    // at namespace scope right after the class - an in-class static_assert
    // that CALLS a member of the same class is evaluated while the class is
    // still incomplete and is rejected by MSVC (C2131). Same reason the
    // scroll-math asserts sit in completed-type scope (run_tests.cpp:3142).

    // Retrieves current translation content
    const std::wstring& GetSourceText() const { return source_text_; }
    const std::string& GetSourceLangCode() const { return source_lang_code_; }
    const std::string& GetTargetLang() const { return target_lang_; }
    // Body text (translation, or the notice body in REQ-R08 message mode). Read
    // by the marshaling unit tests to assert a posted payload reached the model.
    const std::wstring& GetTranslatedText() const { return translated_text_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    // 261111_0001 U1 (v0.10.7, audit D1a/D1b): the GUI-side teardown half of
    // Dismiss() (StopTTS + timer kills + model clears + SW_HIDE), extracted so
    // the kDismissMessage handler can run it exactly once even when an
    // off-thread Dismiss() already cleared visibility before posting (G-06
    // round 2: the handler first proves the dismissal is still current via
    // TooltipCardVisibility::TryCompleteDismiss). Idempotent by construction
    // (every step is a no-op on the second run).
    void TeardownCard();
    void Render();
    void UpdateLayered();
    void ReallocateBuffer(int width, int height); // physical px buffer
    // REQ-R15 (audit §5 latent item 3): current_width_/current_height_ are
    // DIP layout extents; the window/DIB are physical px of the target
    // monitor. PhysW/PhysH convert with the DPI captured at show time.
    int PhysW() const;
    int PhysH() const;
    void RebindRenderTarget();
    // R6 Phase 3 (audit item 4, plan §3.1 A3): device-lost recovery. Called
    // when EndDraw returns D2DERR_RECREATE_TARGET; recreates the single-
    // threaded DC render target + the device-dependent logo bitmap on the new
    // target. Without it a driver reset leaves the surface permanently blank.
    void RecreateAfterDeviceLost();
    void LoadLogoBitmap();
    // C1 (session 260910_0007): the single persistent scratch brush the whole
    // Render path draws through (SetColor just before every Fill/Draw/DrawText
    // call). Created at Create() and device-lost recovery - NEVER at render
    // entry - and released in RecreateAfterDeviceLost (before the old target
    // is dropped) and Destroy. Null when target creation failed; every draw
    // site null-guards exactly like the old per-brush `if (brush)` checks.
    void EnsureScratchBrush();
    void ReleaseScratchBrush();
    // C2 (session 260910_0007): measured-layout cache seam. Recomputes
    // src_tag_width_ / copy_btn_w_ / tts_btn_w_ - plus tgt_btn_w_ (REQ-052) -
    // ONLY when the label strings feeding them change (content set, UI-locale
    // string switch - detected by
    // the cached-key comparison; DirectWrite metrics are DPI-independent DIPs
    // so a DPI crossing needs no recompute). Render consumes the cached
    // floats; the hit-test rects derive from the same values (lockstep by
    // construction).
    void EnsureMeasuredLayouts();
    void InitSapi();
    void CleanupSapi();
    // P4 Batch-3 (REQ-C-002, design §1.2.2 / decision D7): transient "no
    // Windows voice installed for this language" notice. Reuses the REQ-R08
    // message-mode state machine through ShowMessage (compact card, header +
    // body, 1.8s autohide via kMessageAutohideMs, generation guard) - no new
    // window type, no new message ids, no new timers. While it is showing,
    // no_voice_notice_active_ routes a card click to the Windows speech
    // settings page (ms-settings:speech deep link, decision D7).
    void ShowNoVoiceNotice();

    // ---- 261108_0002 (WS3, v0.10.6): floating back-translation card ----
    // 3-tier anchor resolution (caret -> badge -> target window/cursor),
    // returning the PHYSICAL-pixel window origin. Card extents are passed in
    // physical px because every tier clamps against a physical work rect.
    POINT ResolveBackTranslationAnchor(HWND target_hwnd, HWND badge_hwnd,
                                       int card_w_phys, int card_h_phys) const;
    // GUI-thread renderer for the back-translation mode (called from Render).
    void RenderBackTranslation();
    // Applies the PlanBackTransTimer decision to the real timer.
    void ApplyBackTransTimerPlan(BackTransTimerEvent ev);
    // Hides the card and kills its timers WITHOUT touching the translation
    // model (used by the dismiss-on-new-translation rule, which immediately
    // re-shows the card with fresh content).
    void HideBackTranslationCard();

    // ---- v0.10.9 (U-B, report §4 Option 2): two-panel scroll plumbing ----
    // Measured height (DIP) of `text` laid out at `column_w` with `format`.
    // Returns 0 when DWrite or the format is unavailable; the caller then uses
    // the character-count estimate (`fallback_line_h` per wrapped line) so the
    // card still lays out on a factory-less target.
    float MeasureBackTransTextHeight(const std::wstring& text, IDWriteTextFormat* format,
                                     float column_w, float fallback_line_h,
                                     float* out_line_height_dip = nullptr) const;
    // The viewport the SINGLE shared scroll offset clamps against: backtrans
    // mode clamps against the taller of its two panel viewports (one offset,
    // two clips); every other mode keeps the REQ-002 body formula.
    float ActiveScrollViewportDip() const {
        return is_backtrans_mode_
                   ? static_cast<float>(backtrans_panel_plan_.shared_viewport_h)
                   : BodyViewportHeightDip(current_height_);
    }
    // Shared scrollbar thumb drag (REQ-002 behaviour, now also reachable in
    // backtrans mode). Returns true when a drag actually started.
    bool BeginScrollThumbDrag(float x_dip, float y_dip);
    // Pointer-driven thumb update; returns true when the offset moved.
    bool UpdateScrollThumbDrag(float y_dip);
    void EndScrollThumbDrag();
    // v0.10.9 (U-B, defect 3-5c): re-arm the adaptive autohide countdown after
    // a wheel/drag scroll. No-op unless the back-translation card is up AND the
    // countdown is actually running - while the pointer hovers the card the
    // timer is deliberately paused (hover pause), so there is nothing to
    // re-arm and the card cannot close under the reader.
    void RearmBackTransAutohideForScroll();

    // 261116_0003 P6b: resolve the ACTIVE engine at menu-build time (called
    // once at the top of each of the two language-dropdown builders). Returns
    // MenuEngineResolution{} when no resolver is wired — the conservative
    // {Google, is_auto=false, local_host_available=false} defaults, per the
    // no-resolver fallback contract on the public seam above (main.cpp wires
    // the real resolver since P6c, PR6-35).
    MenuEngineResolution ResolveMenuEngine() const {
        return engine_resolver_ ? engine_resolver_() : MenuEngineResolution{};
    }

    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    DWORD gui_thread_id_ = 0; // thread that ran Create(); WndProc dispatch owner
    // 261111_0001 U1 (v0.10.7, audit D1a/D1b) + fix-list round 2 G-06: the
    // card's show/dismiss publication state. An off-thread Dismiss()/
    // DismissThreadSafe() clears visibility IMMEDIATELY (so IsVisible() is
    // false the moment the call returns on any thread), owes the GUI teardown,
    // and captures the generation of the card it is dismissing - all three in
    // ONE mutex section (TooltipCardVisibility above), so a fresh Show* can
    // never slip between them. The kDismissMessage handler completes the
    // dismissal only while that generation is still current, and every Show*
    // path publishes through the same type (fresh card, no teardown owed, new
    // generation), so a queued/duplicate dismissal of an older card is a pure
    // no-op instead of tearing a fresher card down.
    TooltipCardVisibility card_visibility_;

    std::wstring source_text_;
    std::string source_lang_code_;
    std::string target_lang_;
    std::wstring translated_text_;

    // ---- R6 Phase 2 (B1-H1): generation-guard state ----
    // latest_request_gen_: bumped by BeginTranslationRequest() on ANY thread
    // (trigger sites run on the GUI thread, the REQ-R06 double-Ctrl+C worker,
    // and the detached drag threads). Read ONLY inside the GUI-thread
    // ShowTranslation/ShowMessage bodies to drop superseded deliveries.
    // dropped_stale_shows_: GUI-thread-only diagnostic counter (test seam).
    std::atomic<uint64_t> latest_request_gen_{kGenNone};
    uint64_t dropped_stale_shows_ = 0;

    int current_width_ = 360;  // DIP
    int current_height_ = 160; // DIP
    UINT dpi_ = 96;            // REQ-R15: DPI of the monitor showing the tooltip

    // REF-3.6: GDI DIB + memory DC + DC render-target lifetime moved to the
    // shared RAII owner. dc_render_target_ below is a NON-OWNING alias into
    // renderer_.target(), kept in sync at the three points where the target
    // changes (Create / RecreateAfterDeviceLost / Destroy).
    LayeredD2DRenderer renderer_;

    // Direct2D & DirectWrite
    ID2D1Factory* d2d_factory_ = nullptr;
    ID2D1DCRenderTarget* dc_render_target_ = nullptr; // alias of renderer_.target()
    ID2D1SolidColorBrush* scratch_brush_ = nullptr;   // C1: persistent single brush
    ID2D1Bitmap* logo_bitmap_ = nullptr;
    IDWriteFactory* dwrite_factory_ = nullptr;
    IDWriteTextFormat* header_format_ = nullptr;
    IDWriteTextFormat* body_format_ = nullptr;
    IDWriteTextFormat* button_format_ = nullptr;
    IDWriteTextFormat* small_format_ = nullptr;
    // 261108_0002 (WS3, v0.10.6): floating back-translation card formats.
    // v0.10.9 (U-B): + backtrans_section_format_ (the two [역번역]/[원문]
    // section headers) and backtrans_ref_format_ retuned to 11.5pt for the
    // [원문] sub-panel body. Created in Create(), released in Destroy; never
    // created per render (C1 discipline).
    IDWriteTextFormat* backtrans_body_format_ = nullptr;      // 13pt SemiBold body
    IDWriteTextFormat* backtrans_ref_format_ = nullptr;       // 11.5pt [원문] body
    IDWriteTextFormat* backtrans_header_format_ = nullptr;    // 12pt SemiBold title
    IDWriteTextFormat* backtrans_section_format_ = nullptr;   // 13pt SemiBold labels

    // Interactive Button Rectangles (local window coordinates)
    D2D1_RECT_F copy_btn_rect_ = {};
    D2D1_RECT_F tts_btn_rect_ = {};
    D2D1_RECT_F lang_btn_rect_ = {};
    D2D1_RECT_F close_btn_rect_ = {};
    // F8 (ADR-A1-4): source-language dropdown button rect (target-pattern
    // member so the WM_MOUSEMOVE/WM_LBUTTONUP hit tests can reach it).
    D2D1_RECT_F src_btn_rect_ = {};

    // ---- REQ-002 scroll state (DIP layout space, plan §1.3) ----
    float scroll_offset_dip_ = 0.0f;    // current scroll position (>= 0)
    float content_height_dip_ = 0.0f;   // measured body text height
    float line_height_dip_ = 18.0f;     // measured line spacing (wheel step unit)
    bool scrollable_ = false;           // content overflows the viewport
    bool dragging_thumb_ = false;       // L-button capture held on the thumb
    bool thumb_hover_ = false;          // brighter thumb while pointer is over it
    float drag_start_offset_dip_ = 0.0f;
    float drag_start_y_dip_ = 0.0f;
    D2D1_RECT_F body_viewport_rect_ = {};
    D2D1_RECT_F scrollbar_track_rect_ = {};
    D2D1_RECT_F scrollbar_thumb_rect_ = {};

    // ---- C2 measured-layout cache (DIP widths; keys are the exact label
    // strings the old per-Render measurement blocks consumed) ----
    float src_tag_width_ = 44.0f;      // floor == old fixed-size fallback
    std::wstring src_tag_key_;         // label that produced src_tag_width_
    float copy_btn_w_ = 88.0f;         // floors == old hardcoded boxes
    float tts_btn_w_ = 82.0f;
    std::wstring footer_labels_key_;   // concat of the 3 footer labels measured
    // REQ-052: target pill natural width (floor == the pre-REQ-052 hardcoded
    // 90 DIP box, fallback parity) and its label key. Render clamps it
    // against the close button via PlanHeaderTargetButton; the cache holds
    // only the measured natural width, so a card/DPI change needs no
    // re-measure (DirectWrite metrics are DIP-independent).
    float tgt_btn_w_ = 90.0f;
    std::wstring tgt_label_key_;

    int hovered_btn_ = 0; // 0=none, 1=copy, 2=tts, 3=lang, 4=close, 5=src
    bool copied_feedback_ = false;
    bool is_message_mode_ = false;
    std::wstring message_header_;
    // P4 Batch-3 (REQ-C-002 / D7): true only while the CURRENT message-mode
    // card is the no-voice notice. Cleared by every show/dismiss path that
    // can replace or hide the card (ShowTranslation / ShowMessage / Dismiss)
    // and consumed by the WM_LBUTTONDOWN click router.
    bool no_voice_notice_active_ = false;

    // ---- 261108_0002 (WS3, v0.10.6): floating back-translation card state ----
    // The card is a THIRD display mode on the same layered window. GUI-thread
    // written, GUI-thread read (Render / WndProc); the worker thread only ever
    // touches the marshal queue.
    bool is_backtrans_mode_ = false;
    std::wstring backtrans_original_;   // AAAAA  (verbatim original, [원문] panel)
    std::wstring backtrans_text_;       // AAAAA' (card body, 13pt SemiBold)
    std::wstring backtrans_header_;     // pre-formatted "🔄 ... (EN → KO)"
    std::wstring backtrans_ref_line_;   // [원문] panel body (raw AAAAA; empty = no original)
    bool backtrans_matched_ = false;    // caller's pill verdict
    // 261108_0002 (FIX-WS3B): the resolved header-row plan for the CURRENT
    // card (computed once in ShowBackTranslation, consumed by Render). Storing
    // it keeps measure and render in lockstep by construction - the same
    // single-source-of-truth discipline the C2 measured-layout cache uses.
    BackTransHeaderPlan backtrans_header_plan_ =
        {BackTransHeaderLayout::SingleRow, 0.0f, 0.0f, 0.0f, 0.0f, false};
    // v0.10.9 (U-B): the measured TWO-PANEL layout for the CURRENT card -
    // both clipped viewports, both content extents, the shared offset clamp,
    // and the card height. Computed once in ShowBackTranslation (DWrite), read
    // verbatim by Render and by the WndProc hit tests.
    BackTransPanelPlan backtrans_panel_plan_ = {};
    // v0.10.9 (U-B, defect 3-5c): the adaptive autohide delay armed for the
    // CURRENT card (BackTransAutohideMsForContent over the displayed text).
    DWORD backtrans_autohide_ms_ = kBackTransAutohideMs;
    // Hover pause state: true between WM_MOUSEMOVE-over-card and
    // WM_MOUSELEAVE (the autohide countdown is paused while it is true).
    bool backtrans_hover_paused_ = false;
    // Close-button hover (id 6 in the shared hovered_btn_ slot would collide
    // with the footer ids, so the card keeps its own flag).
    bool backtrans_close_hover_ = false;
    D2D1_RECT_F backtrans_close_rect_ = {};

    // SAPI TTS Voice COM interface
    ISpVoice* voice_ = nullptr;
    ISpObjectToken* default_voice_token_ = nullptr;
    std::string current_voice_name_;
    std::string current_voice_lang_;

    LanguageChangeCallback lang_change_cb_;
    // F8 (ADR-A1-4): source-language dropdown handler (optional; hidden when
    // unset, exactly like the target callback).
    SourceLanguageChangeCallback src_lang_change_cb_;
    // 261116_0003 P6b: the engine-view resolver (SetEngineResolver). Wired by
    // main.cpp since P6c (PR6-35); ResolveMenuEngine() falls back to
    // MenuEngineResolution{} when no resolver is set (the defensive
    // conservative Google / false / false defaults).
    EngineResolver engine_resolver_;

    static constexpr UINT_PTR kTimerCopiedFeedback = 4001;
    static constexpr UINT_PTR kTimerMessageAutohide = 4002;
    static constexpr DWORD kMessageAutohideMs = 1800; // auto-hide delay for message-mode cards (F-17/R5/toggle notices)
    // 261108_0002 (WS3, v0.10.6): floating back-translation card auto-dismiss.
    // Reused for the hover GRACE countdown too (one timer, two delays - the
    // PlanBackTransTimer planner owns which).
    static constexpr UINT_PTR kTimerBackTranslateAutohide = 4003;
};

// P4 Batch-3 (REQ-C-001 / D6): compile-time proof of the voice score lattice
// (§1.2.1). Class is complete here, so the constexpr member calls evaluate.
// The resulting order is OneCore exact 120 > OneCore partial 100 >
// SAPI exact 70 > SAPI partial 50 - OneCore always outranks legacy SAPI for
// the same language match level, and a SAPI voice can win only when OneCore
// has no language match at all.
static_assert(TooltipWindow::VoiceTotalScore(TooltipWindow::kVoiceCategoryOneCore, 2) == 120,
              "REQ-C-001: OneCore exact-match lattice point is 120");
static_assert(TooltipWindow::VoiceTotalScore(TooltipWindow::kVoiceCategoryOneCore, 1) == 100,
              "REQ-C-001: OneCore partial-match lattice point is 100");
static_assert(TooltipWindow::VoiceTotalScore(TooltipWindow::kVoiceCategorySapi, 2) == 70,
              "REQ-C-001: SAPI exact-match lattice point is 70");
static_assert(TooltipWindow::VoiceTotalScore(TooltipWindow::kVoiceCategorySapi, 1) == 50,
              "REQ-C-001: SAPI partial-match lattice point is 50");

// REQ-052 (audit P1 header overlap fix): compile-time proof of the header
// target-button clamp planner. All operands are small integers, exact in
// binary floating point. On the 360 DIP card the close button's left edge
// sits at 328 DIP.
static_assert(TooltipWindow::PlanHeaderTargetButton(44.0f, 60.0f, 360.0f, 90.0f).x == 132.0f &&
                  TooltipWindow::PlanHeaderTargetButton(44.0f, 60.0f, 360.0f, 90.0f).width == 90.0f,
              "REQ-052: nominal source tag keeps the natural start and the old 90 DIP width");
// German "Automatische Erkennung ▾" (~165 DIP source tag): the desired 90 DIP
// width clamps to 83 DIP so the right edge lands exactly 8 DIP clear of the
// close button (was ~327 vs 328 DIP - the overlap that put the language menu
// on top of the ✕).
static_assert(TooltipWindow::PlanHeaderTargetButton(44.0f, 165.0f, 360.0f, 90.0f).x == 237.0f &&
                  TooltipWindow::PlanHeaderTargetButton(44.0f, 165.0f, 360.0f, 90.0f).width == 83.0f,
              "REQ-052: wide source tag clamps the target width to the 8 DIP gap");
// Extreme source tag: the start itself is pulled left to close_left - 48 so
// the button keeps the 40 DIP minimum - always clickable, never under the ✕.
static_assert(TooltipWindow::PlanHeaderTargetButton(44.0f, 250.0f, 360.0f, 90.0f).x == 280.0f &&
                  TooltipWindow::PlanHeaderTargetButton(44.0f, 250.0f, 360.0f, 90.0f).width == 40.0f,
              "REQ-052: extreme source tag pulls the start left, width floors at 40 DIP");
// A sub-minimum desired width (measurement glitch) floors at the minimum.
static_assert(TooltipWindow::PlanHeaderTargetButton(44.0f, 60.0f, 360.0f, 20.0f).width == 40.0f,
              "REQ-052: desired width below the 40 DIP minimum is floored");

// 261108_0002 (WS3, v0.10.6): compile-time proof of the floating
// back-translation card math. The card is always 360 DIP wide (the same width
// ShowTranslation resets to, so the two modes never leave each other a stale
// width), and the height is monotonic non-decreasing in both text lengths.
static_assert(TooltipWindow::kBackTransCardWidthDip == 360,
              "WS3: the back-translation card is 360 DIP wide");
// v0.10.9 (U-B): the empty payload is the two-panel furniture plus one body
// line (20) and one [원문] line (16): 44 + 72 + 20 + 16 = 152 DIP.
static_assert(TooltipWindow::ComputeBackTransCardSize(0, 0).width == 360 &&
                  TooltipWindow::ComputeBackTransCardSize(0, 0).height == 152,
              "WS3: an empty payload lays out the two-panel furniture + one line each");
static_assert(TooltipWindow::ComputeBackTransCardSize(1, 0).height <=
                  TooltipWindow::ComputeBackTransCardSize(45, 0).height,
              "WS3: one wrapped line more is never shorter (monotonic in body length)");
static_assert(TooltipWindow::ComputeBackTransCardSize(200, 0).height <=
                  TooltipWindow::ComputeBackTransCardSize(200, 40).height,
              "WS3: the reference line never shrinks the card (monotonic in ref length)");
static_assert(TooltipWindow::ComputeBackTransCardSize(100000, 100000).height ==
                  TooltipWindow::kBackTransCardMaxHeightDip,
              "WS3: a pathological body clamps at the maximum card height");
static_assert(TooltipWindow::ComputeBackTransCardSize(100000, 100000).width == 360,
              "WS3: the clamp never widens the card");

// ---- v0.10.9 (U-B, report §4 Option 2): two-panel layout proofs ----
// The overflow case the bug report describes: a 20-line back-translation next
// to a 3-line original (20*20 = 400 DIP and 3*16 = 48 DIP of content). Both
// panels must stay inside the 480 DIP card, and the original must not be
// starved by the long body.
//
// v0.10.9 fix round 3 (H-1 / QA finding F3): the original is no longer starved
// either - it reserves min(measured, 45% of the body) FIRST, so this exact
// case shows all 3 original lines at offset 0 without scrolling.
namespace ws3_panel_probe {
inline constexpr TooltipWindow::BackTransPanelPlan kOverflow =
    TooltipWindow::PlanBackTransPanels(400, 48);
inline constexpr TooltipWindow::BackTransPanelPlan kFits =
    TooltipWindow::PlanBackTransPanels(40, 16);
// A pathological original (30 * 16 = 480 DIP) beside the same 20-line body:
// both sides overflow, so the original gets exactly the 45% cap.
inline constexpr TooltipWindow::BackTransPanelPlan kTallOriginal =
    TooltipWindow::PlanBackTransPanels(400, 30 * TooltipWindow::kBackTransRefLineDip);
} // namespace ws3_panel_probe
static_assert(ws3_panel_probe::kOverflow.scrollable,
              "WS3 (v0.10.9): a 20-line body + 3-line original scrolls");
static_assert(ws3_panel_probe::kOverflow.card_height_dip ==
                  TooltipWindow::kBackTransCardMaxHeightDip,
              "WS3 (v0.10.9): the overflow card uses the full 480 DIP height");
// The invariant Render depends on: both panels + furniture close the card
// exactly, so no viewport can reach past the card's bottom edge.
static_assert(ws3_panel_probe::kOverflow.ref_view_top +
                      ws3_panel_probe::kOverflow.ref_view_h +
                      TooltipWindow::kBackTransPanelBottomPadDip +
                      TooltipWindow::kBackTransCardBottomPadDip ==
                  ws3_panel_probe::kOverflow.card_height_dip,
              "WS3 (v0.10.9): the two panels always fit the card exactly");
static_assert(ws3_panel_probe::kOverflow.back_view_top +
                      ws3_panel_probe::kOverflow.back_view_h <=
                  ws3_panel_probe::kOverflow.ref_panel_top,
              "WS3 (v0.10.9): the back viewport never overlaps the [원문] panel");
static_assert(ws3_panel_probe::kOverflow.back_view_h >= TooltipWindow::kBackTransMinPanelBodyDip &&
                  ws3_panel_probe::kOverflow.ref_view_h >= TooltipWindow::kBackTransMinPanelBodyDip,
              "WS3 (v0.10.9): each panel keeps its minimum body height");
static_assert(ws3_panel_probe::kOverflow.back_view_h >
                  ws3_panel_probe::kOverflow.ref_view_h,
              "WS3 (v0.10.9): the long panel gets the larger share");
static_assert(ws3_panel_probe::kOverflow.back_view_h +
                      ws3_panel_probe::kOverflow.ref_view_h == 480 -
                      TooltipWindow::kBackTransContentTopDip -
                      TooltipWindow::kBackTransFurnitureDip,
              "WS3 (v0.10.9): the two viewports use the whole body area");
// H-1 (fix round 3): the 3-line original reserves its WHOLE measured height,
// so the comparison is visible at once even beside the 20-line body.
static_assert(ws3_panel_probe::kOverflow.ref_view_h ==
                  ws3_panel_probe::kOverflow.ref_content_h,
              "WS3 (H-1): a short original is fully visible without scrolling");
static_assert(ws3_panel_probe::kOverflow.ref_view_h ==
                  3 * TooltipWindow::kBackTransRefLineDip,
              "WS3 (H-1): the reservation is exactly the measured 3 original lines");
static_assert(ws3_panel_probe::kOverflow.ref_view_h <=
                  ws3_panel_probe::kOverflow.body_area_dip *
                      TooltipWindow::kBackTransRefSharePct / 100,
              "WS3 (H-1): the original never reserves more than the 45% cap");
// ... and a pathological original is capped at that 45% share instead of
// eating the body, so the long side still keeps the larger viewport.
static_assert(ws3_panel_probe::kTallOriginal.ref_view_h ==
                  ws3_panel_probe::kTallOriginal.body_area_dip *
                      TooltipWindow::kBackTransRefSharePct / 100,
              "WS3 (H-1): an overflowing original is capped at the 45% share");
static_assert(ws3_panel_probe::kTallOriginal.ref_view_h <
                  ws3_panel_probe::kTallOriginal.ref_content_h,
              "WS3 (H-1): an original taller than the cap still scrolls");
static_assert(ws3_panel_probe::kTallOriginal.back_view_h ==
                  ws3_panel_probe::kTallOriginal.body_area_dip -
                      ws3_panel_probe::kTallOriginal.ref_view_h,
              "WS3 (H-1): the back-translation takes the remainder of the body");
// The shared offset now clamps to the back-translation's own overflow (the
// original's is zero): 400 - 316 = 84 DIP.
static_assert(ws3_panel_probe::kOverflow.max_scroll_offset == 84,
              "WS3 (v0.10.9): the shared offset clamps to the largest panel overflow");
static_assert(!ws3_panel_probe::kFits.scrollable &&
                  ws3_panel_probe::kFits.card_height_dip == 172,
              "WS3 (v0.10.9): a fitting payload shrinks the card and never scrolls");
// H-1 must not disturb content that already fits: this is the pre-fix
// (v0.10.9 U-B) geometry, value for value.
static_assert(ws3_panel_probe::kFits.back_view_top == 64 &&
                  ws3_panel_probe::kFits.ref_panel_top == 112 &&
                  ws3_panel_probe::kFits.ref_label_top == 118 &&
                  ws3_panel_probe::kFits.ref_view_top == 138 &&
                  ws3_panel_probe::kFits.ref_view_h == 16 &&
                  ws3_panel_probe::kFits.back_view_h == 40,
              "WS3 (H-1): a payload that fits keeps the unchanged layout");
static_assert(TooltipWindow::PlanBackTransPanels(0, 0).card_height_dip > 0 &&
                  TooltipWindow::PlanBackTransPanels(0, 0).back_view_h == 0 &&
                  TooltipWindow::PlanBackTransPanels(0, 0).ref_view_h == 0 &&
                  !TooltipWindow::PlanBackTransPanels(0, 0).scrollable,
              "WS3 (v0.10.9): an empty payload is a non-scrolling, viewport-less card");
static_assert(TooltipWindow::PlanBackTransPanels(-40, -16).card_height_dip ==
                  TooltipWindow::PlanBackTransPanels(0, 0).card_height_dip,
              "WS3 (v0.10.9): negative measurements clamp to zero (defensive)");

// 261108_0002 (FIX-WS3B, v0.10.6): compile-time proof of the header-row
// planner. On the 360 DIP card the ✕'s left edge sits at 328 DIP, so:
//   single-row header budget = 328 - 8 (close gap) - pill - 8 (header gap) - 14 (pad)
//   own-row header budget    = 328 - 8 (close gap) - 14 (pad) = 306
using Ws3bPlan = TooltipWindow::BackTransHeaderPlan;
using Ws3bLayout = TooltipWindow::BackTransHeaderLayout;
static_assert(TooltipWindow::PlanBackTransHeaderRowDip(120.0f, 90.0f, 360.0f).layout ==
                  Ws3bLayout::SingleRow,
              "FIX-WS3B: a short header + short pill stay on one row");
static_assert(TooltipWindow::PlanBackTransHeaderRowDip(120.0f, 90.0f, 360.0f).pill_row_h == 0.0f,
              "FIX-WS3B: the single-row plan adds no height");
static_assert(TooltipWindow::PlanBackTransHeaderRowDip(200.0f, 120.0f, 360.0f).layout ==
                  Ws3bLayout::PillOnOwnRow,
              "FIX-WS3B: a long header + long pill wrap the pill to its own row");
static_assert(TooltipWindow::PlanBackTransHeaderRowDip(200.0f, 120.0f, 360.0f).pill_row_h > 0.0f,
              "FIX-WS3B: the wrapped pill row adds height");
static_assert(TooltipWindow::PlanBackTransHeaderRowDip(200.0f, 120.0f, 360.0f).ellipsize == false,
              "FIX-WS3B: the pill wraps BEFORE the header is ever clipped");
static_assert(TooltipWindow::PlanBackTransHeaderRowDip(400.0f, 120.0f, 360.0f).layout ==
                  Ws3bLayout::EllipsizeHeader,
              "FIX-WS3B: only a header that cannot fit alone is ellipsized");
static_assert(TooltipWindow::PlanBackTransHeaderRowDip(400.0f, 120.0f, 360.0f).ellipsize,
              "FIX-WS3B: the ellipsize flag is set only in that case");
// The pill is never clipped: its box is exactly what the planner reports, and
// it never exceeds the card's content column.
static_assert(TooltipWindow::PlanBackTransHeaderRowDip(120.0f, 90.0f, 360.0f).pill_width_dip ==
                  TooltipWindow::kBackTransPillMinWidthDip,
              "FIX-WS3B: a sub-minimum pill measurement floors at 96 DIP");
static_assert(TooltipWindow::PlanBackTransHeaderRowDip(120.0f, 900.0f, 360.0f).pill_width_dip ==
                  360.0f - 2.0f * TooltipWindow::kBackTransPadDip,
              "FIX-WS3B: an oversized pill clamps to the content column, never past the card");
// Every plan keeps the header clear of the ✕ and the pill clear of the ✕.
static_assert(TooltipWindow::PlanBackTransHeaderRowDip(400.0f, 120.0f, 360.0f).header_width_dip +
                      TooltipWindow::kBackTransPadDip + TooltipWindow::kBackTransCloseGapDip <=
                  360.0f - TooltipWindow::kBackTransCloseRightDip - TooltipWindow::kBackTransCloseW,
              "FIX-WS3B: the header budget never reaches the close button");
// The wrapped pill row grows the card by exactly the plan's extra height.
static_assert(TooltipWindow::ComputeBackTransCardSize(80, 20, 30).height ==
                  TooltipWindow::ComputeBackTransCardSize(80, 20, 0).height + 30,
              "FIX-WS3B: the card height includes the wrapped pill row");
static_assert(TooltipWindow::ComputeBackTransCardSize(80, 20, 0).height ==
                  TooltipWindow::ComputeBackTransCardSize(80, 20).height,
              "FIX-WS3B: the default argument keeps the single-row card byte-identical");

// Hover pause / auto-dismiss state machine (4500 ms autohide FLOOR since
// v0.10.9 - the live delay is content-scaled up to 12000 ms - pause on hover,
// 1500 ms grace on leave).
static_assert(TooltipWindow::kBackTransAutohideMs == 4500,
              "WS3: the back-translation card's autohide floor is 4500 ms");
static_assert(TooltipWindow::kBackTransHoverGraceMs == 1500,
              "WS3: leaving the card resumes with a 1500 ms grace window");
// v0.10.9 (U-B, defect 3-5c): the adaptive autohide ramp. Short cards keep the
// 4500 ms floor, the delay grows 10 ms per displayed character beyond the free
// 120, and it saturates at 12000 ms (750 extra chars / 870 displayed).
static_assert(TooltipWindow::BackTransAutohideMsForContent(0) == 4500 &&
                  TooltipWindow::BackTransAutohideMsForContent(120) == 4500,
              "WS3 (v0.10.9): a short card keeps the 4500 ms floor");
static_assert(TooltipWindow::BackTransAutohideMsForContent(121) == 4510,
              "WS3 (v0.10.9): the ramp adds 10 ms per character beyond the free 120");
static_assert(TooltipWindow::BackTransAutohideMsForContent(400) == 4500 + 280 * 10,
              "WS3 (v0.10.9): a mid-length card scales linearly");
static_assert(TooltipWindow::BackTransAutohideMsForContent(870) ==
                  TooltipWindow::kBackTransAutohideMaxMs &&
                  TooltipWindow::BackTransAutohideMsForContent(1000000) ==
                      TooltipWindow::kBackTransAutohideMaxMs,
              "WS3 (v0.10.9): the delay saturates at 12000 ms and never overflows");
static_assert(TooltipWindow::PlanBackTransTimer(TooltipWindow::BackTransTimerEvent::Shown, false, 9000)
                      .delay_ms == 9000,
              "WS3 (v0.10.9): the adaptive delay flows through the timer planner");
static_assert(TooltipWindow::PlanBackTransTimer(TooltipWindow::BackTransTimerEvent::Shown, false).action ==
                      TooltipWindow::BackTransTimerAction::ArmAutohide &&
                  TooltipWindow::PlanBackTransTimer(TooltipWindow::BackTransTimerEvent::Shown, false).delay_ms == 4500,
              "WS3: showing the card arms the autohide at its 4500 ms floor by default");
static_assert(TooltipWindow::PlanBackTransTimer(TooltipWindow::BackTransTimerEvent::PointerMoved, false).action ==
                      TooltipWindow::BackTransTimerAction::PauseAutohide,
              "WS3: the first pointer move over the card pauses the countdown");
static_assert(TooltipWindow::PlanBackTransTimer(TooltipWindow::BackTransTimerEvent::PointerMoved, true).action ==
                      TooltipWindow::BackTransTimerAction::None,
              "WS3: repeated pointer moves do not churn the timer");
static_assert(TooltipWindow::PlanBackTransTimer(TooltipWindow::BackTransTimerEvent::PointerLeft, true).action ==
                      TooltipWindow::BackTransTimerAction::ArmGrace &&
                  TooltipWindow::PlanBackTransTimer(TooltipWindow::BackTransTimerEvent::PointerLeft, true).delay_ms == 1500,
              "WS3: leaving the card re-arms the timer with the 1500 ms grace");
static_assert(TooltipWindow::PlanBackTransTimer(TooltipWindow::BackTransTimerEvent::Dismissed, false).action ==
                      TooltipWindow::BackTransTimerAction::KillTimers,
              "WS3: an explicit dismissal kills the timer outright");

// Placement math: a caret with room above puts the card's bottom 12 px above
// it, horizontally centred on the caret; without room it flips below.
namespace ws3_card_place_probe {
inline constexpr RECT kAnchor = {500, 400, 502, 420};   // 2 px wide caret
inline constexpr RECT kWork   = {0, 0, 1920, 1080};
inline constexpr POINT kAbove = TooltipWindow::PlaceBackTransCard(kAnchor, true, 12, 360, 120, kWork);
inline constexpr POINT kBelow = TooltipWindow::PlaceBackTransCard(RECT{500, 10, 502, 30}, true, 12, 360, 120,
                                                                  RECT{0, 0, 1920, 1080});
} // namespace ws3_card_place_probe
static_assert(ws3_card_place_probe::kAbove.x == 321 && ws3_card_place_probe::kAbove.y == 268,
              "WS3: the card is centred on the anchor and sits gap+height above it");
static_assert(ws3_card_place_probe::kBelow.y == 42,
              "WS3: no room above flips the card below the anchor");

} // namespace emebalachat
