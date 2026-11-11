#pragma once

#include <atomic>
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
    static constexpr int kBackTransCardMinHeightDip = 96;
    static constexpr int kBackTransCardMaxHeightDip = 340;
    // 13pt SemiBold body in the 332 DIP text column (360 - 2*14 padding).
    // Deliberately a character-count estimate, not a DirectWrite measurement:
    // the GUI-thread show path refines the height with a real layout when a
    // DWrite factory exists, and this estimate is the headless floor.
    static constexpr int kBackTransCharsPerLine = 44;
    static constexpr int kBackTransLineHeightDip = 20;
    // Header row (38) + top/bottom padding (22).
    static constexpr int kBackTransChromeDip = 60;
    static constexpr int kBackTransRefGapDip = 6;
    static constexpr int kBackTransRefLineDip = 16;

    struct BackTransCardSize {
        int width;
        int height;
    };

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
    static constexpr BackTransCardSize ComputeBackTransCardSize(int back_text_len,
                                                                int ref_text_len,
                                                                int header_extra_h_dip = 0) {
        const int back_len = back_text_len > 0 ? back_text_len : 0;
        const int ref_len = ref_text_len > 0 ? ref_text_len : 0;
        int lines = (back_len + kBackTransCharsPerLine - 1) / kBackTransCharsPerLine;
        if (lines < 1) lines = 1;
        int h = kBackTransChromeDip + lines * kBackTransLineHeightDip;
        if (ref_len > 0) {
            int ref_lines = (ref_len + kBackTransCharsPerLine - 1) / kBackTransCharsPerLine;
            if (ref_lines < 1) ref_lines = 1;
            h += kBackTransRefGapDip + ref_lines * kBackTransRefLineDip;
        }
        if (header_extra_h_dip > 0) h += header_extra_h_dip;
        if (h < kBackTransCardMinHeightDip) h = kBackTransCardMinHeightDip;
        if (h > kBackTransCardMaxHeightDip) h = kBackTransCardMaxHeightDip;
        return {kBackTransCardWidthDip, h};
    }

    // ---- hover pause / auto-dismiss state machine ----
    // Spec: 4500 ms auto-dismiss, PAUSED while the pointer is over the card,
    // resumed with a 1500 ms grace timer when it leaves. Expressed as a pure
    // planner so the WndProc branch is a single dispatch and the whole
    // contract is unit-testable without a window.
    static constexpr DWORD kBackTransAutohideMs = 4500;
    static constexpr DWORD kBackTransHoverGraceMs = 1500;

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
                                                           bool hover_paused) {
        switch (ev) {
            case BackTransTimerEvent::Shown:
                return {BackTransTimerAction::ArmAutohide, kBackTransAutohideMs};
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
    // Muted reference line, e.g. "↳ 원문: AAAAA". No i18n key exists for it
    // (the 261108_0002 i18n unit added only the header + two pills), so the
    // label is a plain formatted literal per the spec.
    static std::wstring FormatBackTransReference(const std::wstring& original_text);
    // The reference line is shown only when the round trip actually changed
    // the text (AAAAA' != AAAAA); an exact round trip would just repeat the
    // body and read as noise.
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
    const std::wstring& GetBackTransReferenceLine() const { return backtrans_ref_line_; }
    bool IsBackTransHoverPaused() const { return backtrans_hover_paused_; }
    bool IsBackTransMatched() const { return backtrans_matched_; }
    float ContentHeightForTest() const { return content_height_dip_; }

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
    // 261108_0002 (WS3, v0.10.6): floating back-translation card formats
    // (13pt SemiBold body + 11pt muted reference line). Created in Create(),
    // released in Destroy; never created per render.
    IDWriteTextFormat* backtrans_body_format_ = nullptr;
    IDWriteTextFormat* backtrans_ref_format_ = nullptr;
    IDWriteTextFormat* backtrans_header_format_ = nullptr;

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
    std::wstring backtrans_original_;   // AAAAA  (reference line source)
    std::wstring backtrans_text_;       // AAAAA' (card body, 13pt SemiBold)
    std::wstring backtrans_header_;     // pre-formatted "🔄 ... (EN → KO)"
    std::wstring backtrans_ref_line_;   // pre-formatted "↳ 원문: AAAAA" (empty = hidden)
    bool backtrans_matched_ = false;    // caller's pill verdict
    // 261108_0002 (FIX-WS3B): the resolved header-row plan for the CURRENT
    // card (computed once in ShowBackTranslation, consumed by Render). Storing
    // it keeps measure and render in lockstep by construction - the same
    // single-source-of-truth discipline the C2 measured-layout cache uses.
    BackTransHeaderPlan backtrans_header_plan_ =
        {BackTransHeaderLayout::SingleRow, 0.0f, 0.0f, 0.0f, 0.0f, false};
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
static_assert(TooltipWindow::ComputeBackTransCardSize(0, 0).width == 360 &&
                  TooltipWindow::ComputeBackTransCardSize(0, 0).height ==
                      TooltipWindow::kBackTransCardMinHeightDip,
              "WS3: an empty payload floors at the minimum card height");
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

// Hover pause / auto-dismiss state machine (spec: 4500 ms autohide, pause on
// hover, 1500 ms grace on leave).
static_assert(TooltipWindow::kBackTransAutohideMs == 4500,
              "WS3: the back-translation card auto-dismisses after 4500 ms");
static_assert(TooltipWindow::kBackTransHoverGraceMs == 1500,
              "WS3: leaving the card resumes with a 1500 ms grace window");
static_assert(TooltipWindow::PlanBackTransTimer(TooltipWindow::BackTransTimerEvent::Shown, false).action ==
                      TooltipWindow::BackTransTimerAction::ArmAutohide &&
                  TooltipWindow::PlanBackTransTimer(TooltipWindow::BackTransTimerEvent::Shown, false).delay_ms == 4500,
              "WS3: showing the card arms the 4500 ms autohide");
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
