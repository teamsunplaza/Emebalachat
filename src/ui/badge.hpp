#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include "layered_renderer.hpp"
#include "theme.hpp"

namespace emebalachat {

enum class BadgeStatus {
    Active,       // Emerald #10B981
    Translating,  // Amber #F59E0B
    Disabled      // Gray #6B7280
};

// 261122_0001 A4 (F9): the badge's {c}/{t} token fill for the localized
// progress label (StringId::BadgeTranslatingProgress). The Translating arm of
// FloatingBadge::Render calls exactly this function, so Render() output is
// unchanged; it is declared here (not file-local) because it is a PURE string
// transform - no Direct2D, no window - and was otherwise unreachable from the
// unit suite, which left a {c}/{t} swap uncaught (only the arm precedence and
// the per-locale token presence were pinned). index/total are the 1-based
// chunk counter and the plan total; unknown tokens are left untouched.
std::wstring BadgeFillProgressTokens(std::wstring text, std::size_t index, std::size_t total);

// ---------------------------------------------------------------------------
// V10F (261122_0002 P3-C2, UX-3 Option A): the badge pill carries three hit
// zones, so the single-click deferral only ever applies to the medallion:
//   * left medallion - the legacy single click (cancel in flight / toggle
//                      active), still armed on kTimerSingleClick so the OS
//                      double-click window can never lose a click to a cancel
//                      (V10F BUG-01 stays fixed).
//   * center label   - SRC / arrow / TGT each own one slice and dispatch on
//                      WM_LBUTTONUP with no deferral (the audit's "eliminate
//                      the 500 ms lag on the language-pair zone"). A
//                      double-click that STARTS on a language slice skips the
//                      legacy double-click swap because the slice already ran
//                      its action on the first click.
//   * right mini-tag - engine tag text ("G"/"L"/"AI"/"U") plus an optional
//                      auto-send marker; its own click callback.
// Right-click menu, drag move, hover restore and the 5 s idle fade are all
// untouched. The boundaries are produced by Render() from the measured label
// runs; the pure planner below is the ONLY place they are interpreted, which
// is what the unit suite pins (no Win32, constexpr).
enum class BadgeZone : int {
    None = -1,      // outside the pill
    Medallion = 0,  // left medallion + the gap up to the label (legacy toggle)
    Source = 1,     // SRC run of the language pair
    Swap = 2,       // the arrow between SRC and TGT
    Target = 3,     // TGT run of the language pair
    Tag = 4         // right engine mini-tag chip
};

// Zone boundaries in DIP - the badge's authored coordinate space, the same one
// current_width_ and the DirectWrite layout metrics live in. `*_begin` are
// inclusive left edges; a value <= 0 means "not laid out yet" and every point
// falls back to the medallion (so a badge that never rendered keeps the legacy
// whole-pill toggle). tag_begin == -1 means no chip is shown.
struct BadgeZoneLayout {
    int source_begin = 0;
    int swap_begin = 0;
    int target_begin = 0;
    int tag_begin = -1;
};

// PURE zone planner. Right-to-left precedence turns the four boundaries into a
// TOTAL partition of the pill, so every in-pill x maps to exactly one zone and
// no click is ever swallowed by a gap between runs:
//   [tag_begin, width)      -> Tag       (when a chip is shown)
//   [target_begin, ...)     -> Target
//   [swap_begin, ...)       -> Swap
//   [source_begin, ...)     -> Source
//   [0, source_begin)       -> Medallion (legacy single click)
// Out-of-pill x (negative, >= width, or a non-positive width) -> None.
constexpr BadgeZone BadgeZoneForPoint(int x, int width, const BadgeZoneLayout& layout) noexcept {
    if (width <= 0 || x < 0 || x >= width) {
        return BadgeZone::None;
    }
    if (layout.tag_begin >= 0 && layout.tag_begin < width && x >= layout.tag_begin) {
        return BadgeZone::Tag;
    }
    if (layout.target_begin > 0 && layout.target_begin < width && x >= layout.target_begin) {
        return BadgeZone::Target;
    }
    if (layout.swap_begin > 0 && layout.swap_begin < width && x >= layout.swap_begin) {
        return BadgeZone::Swap;
    }
    if (layout.source_begin > 0 && layout.source_begin < width && x >= layout.source_begin) {
        return BadgeZone::Source;
    }
    return BadgeZone::Medallion;
}

// The int the zone-language callback receives: 0 = source, 1 = swap,
// 2 = target, -1 for the zones that are not part of the language pair.
constexpr int BadgeZoneLanguageIndex(BadgeZone zone) noexcept {
    switch (zone) {
        case BadgeZone::Source: return 0;
        case BadgeZone::Swap:   return 1;
        case BadgeZone::Target: return 2;
        case BadgeZone::Medallion:
        case BadgeZone::Tag:
        case BadgeZone::None:
        default:                return -1;
    }
}

// True for the zones that dispatch on WM_LBUTTONUP WITHOUT the single-click
// deferral (the language slices and the engine tag). The medallion keeps the
// deferral; WM_LBUTTONDBLCLK consults this too, so a double-click on an
// immediate zone never runs a second action.
constexpr bool BadgeZoneIsImmediate(BadgeZone zone) noexcept {
    return zone == BadgeZone::Source || zone == BadgeZone::Swap ||
           zone == BadgeZone::Target || zone == BadgeZone::Tag;
}

class FloatingBadge {
public:
    using ClickCallback = std::function<void()>;

    FloatingBadge();
    ~FloatingBadge();

    // Creates the layered Direct2D pill badge window with optional initial position
    bool Create(HINSTANCE hInstance, std::wstring_view src_code = L"Auto", std::wstring_view tgt_code = L"EN", int initial_x = -1, int initial_y = -1);

    // Closes and destroys the badge window
    void Destroy();

    // Updates status and triggers immediate redraw (thread-safe)
    void SetStatus(BadgeStatus status);

    // 261121_0002 (T4 badge progress): multi-chunk translation progress.
    // Both marshal to the GUI thread exactly like SetStatus above. SetProgress
    // stores (index, total); a (0,0) payload clears the numbers so the next
    // render falls back to the plain "Translating" label. SetCancellingThreadSafe
    // drives the "Cancelling…" overlay shown while a cancel is being honored.
    void SetProgress(std::size_t index, std::size_t total);
    void SetCancellingThreadSafe(bool cancelling);

    // Updates source and target language codes (thread-safe)
    void SetLanguages(std::wstring_view src_code, std::wstring_view tgt_code);

    // Shows or hides the badge window
    void SetVisible(bool visible);
    bool IsVisible() const;

    // Repositions badge on desktop
    void SetPosition(int x, int y);

    using ActionCallback = std::function<void()>;
    using PositionCallback = std::function<void(int x, int y)>;

    // Sets callbacks for user interaction
    void SetClickCallback(ActionCallback cb);
    void SetDoubleClickCallback(ActionCallback cb);
    void SetRightClickCallback(ActionCallback cb);
    void SetPositionCallback(PositionCallback cb);

    // V10F (261122_0002 P3-C2, UX-3 Option A): 3-zone interaction. Additive
    // only - every callback above keeps its exact pre-existing contract (the
    // medallion click still arrives through the kTimerSingleClick deferral,
    // the double-click still swaps languages, the right click still opens the
    // menu, and a drag still only reports its new position).
    using ZoneClickCallback = std::function<void(int zone)>;
    // SRC (0) / swap (1) / TGT (2) slice click, fired on WM_LBUTTONUP with no
    // deferral. See BadgeZoneLanguageIndex for the numbering.
    void SetZoneLanguageClickCallback(ZoneClickCallback cb);
    // Center-arrow executor. When set it is the ONLY swap executor: the zone
    // language callback is NOT also given zone 1, so wiring both cannot make
    // main.cpp swap twice.
    void SetSwapRequestCallback(ActionCallback cb);
    // Engine mini-tag click (the third zone).
    void SetTagClickCallback(ActionCallback cb);
    // Right mini-tag content: a short engine badge ("G", "L", "AI", "U", ...).
    // nullptr or L"" hides the chip unless SetAutoSendIndicator(true) is on.
    // Thread-safe: the value is stored under data_mutex_ and a GUI marshal
    // only re-renders (the SetLanguages discipline), so no reorder hazard.
    void SetEngineTag(const wchar_t* tag);
    // Appends the auto-send marker to the mini-tag while auto-send (Enter) is
    // enabled. Thread-safe, same marshaling as SetEngineTag.
    void SetAutoSendIndicator(bool on);
    // Snapshot of the last rendered boundaries (DIP) and a physical-pixel
    // client-x hit test through the same pure planner the window uses, so a
    // caller (main.cpp zone tooltips) cannot drift from the real hit boxes.
    BadgeZoneLayout GetZoneLayout() const;
    BadgeZone HitTestZone(int client_x) const;

    HWND GetHwnd() const { return hwnd_; }
    int GetCurrentWidth() const { return current_width_; }
    void Invalidate() { Render(); }
    void RefreshTheme() { Render(); }
    // 260922_0001 A4 (fix plan §4.1-A): after an EndDraw device-lost, Render()
    // recreates the target and re-renders exactly once. reentry_allowed caps
    // the recursion depth at 2 (top pass true -> recovery pass false); the
    // recovery pass is a plain call on the same (GUI) thread, so the only
    // re-lock it performs is render_mutex_ — hence the recursive mutex below.
    void Render(bool reentry_allowed = true);

    static constexpr UINT WM_BADGE_SET_STATUS = WM_USER + 201;
    static constexpr UINT WM_BADGE_SET_LANGS  = WM_USER + 202;
    // 261121_0002 (T4 badge progress): WM_USER+203/204 verified free across
    // src/ (grep WM_USER + 20x hits only the two ids above; main.cpp has none).
    // SET_PROGRESS carries MAKELONG(index, total); a (0,0) payload clears the
    // numbers. SET_CANCELLING carries the bool in wParam (0/1).
    static constexpr UINT WM_BADGE_SET_PROGRESS   = WM_USER + 203;
    static constexpr UINT WM_BADGE_SET_CANCELLING = WM_USER + 204;

    // V10F P3-C2 (UX-3 Option A): WM_USER+205/206 verified free across src/
    // (only 201-204 above are claimed; no other WM_USER + 20x site exists).
    // Both are PURE re-render notifications: the setter already stored the
    // value under data_mutex_ before posting (the SetLanguages discipline), so
    // a reordered message can only ever cause an extra render, never a stale
    // value.
    static constexpr UINT WM_BADGE_SET_TAG      = WM_USER + 205;
    static constexpr UINT WM_BADGE_SET_AUTOSEND = WM_USER + 206;

    // V10F P3-C2 (UX-3 Option A): the 5 s idle fade floor moves 20% -> 45%.
    // At 51/255 the pill was barely legible over a dark editor, which made the
    // new zone affordances undiscoverable; 115/255 keeps the pill clearly
    // "resting" without hiding them. kFullAlpha (85%) is the hover/activity
    // level every Set* path restores.
    static constexpr BYTE kIdleAlpha = 115;
    static constexpr BYTE kFullAlpha = 217;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void UpdateAlpha(BYTE alpha);
    void ResetIdleTimer();
    // 261029_0001 U-I (R1-21): re-run the Create/drag clamp against the live
    // monitor union after the display topology changed (monitor unplugged /
    // undocked / resolution changed). No-op when no monitor is attached.
    void ReclampToVisibleMonitors();
    void ReallocateBuffer(int width, int height); // physical px buffer
    // REQ-R15 (audit §5 latent item 3): the layout (current_width_, kHeight)
    // is authored in DIPs; the window, DIB buffer, and UpdateLayeredWindow
    // live in PHYSICAL pixels of the monitor the badge sits on. PhysW/PhysH
    // perform the conversion with the current dpi_.
    int PhysW() const;
    int PhysH() const;
    void RebindRenderTarget();
    // R6 Phase 3 (audit item 4, plan §3.1 A3): recreate the DC render target
    // (+ device-dependent logo bitmap) after EndDraw returns
    // D2DERR_RECREATE_TARGET, so a driver reset cannot leave the badge a
    // permanently blank pill.
    // 260922_0001 A4: returns true only when the target was recreated and
    // rebound — Render() uses it to decide between the one-shot recovery
    // re-render and skipping the UpdateAlpha commit of stale/blank pixels.
    bool RecreateAfterDeviceLost();
    void LoadLogoBitmap();
    // V10F P3-C2 (UX-3 Option A): run the callback for an already-latched
    // zone. GUI thread only (called from WndProc / WM_TIMER), like the state
    // fields below.
    void DispatchZoneClick(BadgeZone zone);

    mutable std::mutex data_mutex_;
    // 260922_0001 A4: std::mutex -> std::recursive_mutex. The one-shot
    // device-lost recovery pass runs as Render(false) from inside the locked
    // Render() body; a NON-recursive lock_guard would self-deadlock on that
    // re-entry (std::mutex is UB/deadlock when locked by its owning thread).
    // Cross-thread semantics are unchanged: only the GUI message thread ever
    // calls Render (worker-side SetStatus/SetLanguages marshal via
    // PostMessage — see the REQ-R10 seam comments), and the recursion is
    // depth-capped at 1 by reentry_allowed.
    mutable std::recursive_mutex render_mutex_;

    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    BadgeStatus status_ = BadgeStatus::Active;
    // 261121_0002 (T4 badge progress): chunk progress numbers + cancel
    // overlay flag. Guarded by data_mutex_ like status_/src_code_/tgt_code_;
    // read by Render() for the Translating arm only. (0,0) means "no progress
    // to show" and keeps short/single-chunk labels byte-identical.
    std::size_t progress_index_ = 0;
    std::size_t progress_total_ = 0;
    bool cancelling_ = false;
    std::wstring src_code_ = L"Auto";
    std::wstring tgt_code_ = L"EN";
    // V10F P3-C2 (UX-3 Option A): right mini-tag content. Guarded by
    // data_mutex_ (SetEngineTag / SetAutoSendIndicator may be called from the
    // engine/worker threads and marshal exactly like SetLanguages).
    std::wstring engine_tag_;
    bool auto_send_indicator_ = false;
    bool visible_ = true;
    BYTE current_alpha_ = kFullAlpha; // 85% opacity default
    bool is_hovered_ = false;
    // V10F P3-C2: zone interaction state. GUI-thread-only, the same ownership
    // is_hovered_ above has (written in WndProc, read by Render).
    BadgeZone press_zone_ = BadgeZone::Medallion;       // zone under the press
    BadgeZone hovered_zone_ = BadgeZone::None;          // drives hover wash/cursor
    BadgeZoneLayout zone_layout_;                       // written by Render()
    int current_width_ = 240; // DIP layout width (recomputed by Render())
    UINT dpi_ = 96;           // REQ-R15: DPI of the monitor hosting the badge

    // REF-3.6: GDI DIB + memory DC + DC render-target lifetime moved to the
    // shared RAII owner. dc_render_target_ below is a NON-OWNING alias into
    // renderer_.target(), kept in sync at the three points where the target
    // changes (Create / RecreateAfterDeviceLost / Destroy) so the render code
    // referencing it stays untouched.
    LayeredD2DRenderer renderer_;

    // Direct2D & DirectWrite COM pointers
    ID2D1Factory* d2d_factory_ = nullptr;
    ID2D1DCRenderTarget* dc_render_target_ = nullptr; // alias of renderer_.target()
    ID2D1Bitmap* logo_bitmap_ = nullptr;
    IDWriteFactory* dwrite_factory_ = nullptr;
    IDWriteTextFormat* text_format_ = nullptr;
    IDWriteTextFormat* code_format_ = nullptr;

    ActionCallback click_callback_;
    ActionCallback double_click_callback_;
    ActionCallback right_click_callback_;
    PositionCallback position_callback_;
    // V10F P3-C2 (UX-3 Option A): zone callbacks. swap_request_callback_ is
    // the dedicated arrow executor; the zone callback still receives 1 when no
    // swap executor is wired (exactly one of them runs per arrow click).
    ZoneClickCallback zone_click_callback_;
    ActionCallback swap_request_callback_;
    ActionCallback tag_click_callback_;

    // V10F P3-C2: engine mini-tag geometry (DIP). kMaxEngineTagChars keeps the
    // chip a tag rather than a second label ("AI" + the auto-send marker fits
    // the 4-char budget); longer values are truncated at the setter.
    static constexpr std::size_t kMaxEngineTagChars = 4;
    static constexpr float kTagFontSize = 9.5f;
    static constexpr float kTagPadX = 5.0f;
    static constexpr float kTagMinWidth = 22.0f;
    static constexpr float kTagHeight = 18.0f;
    static constexpr float kTagRadius = 4.0f;
    static constexpr float kTagGap = 7.0f;
    // Used only when DirectWrite refuses the tag layout: a rough Consolas
    // advance keeps the chip placed (the label still renders exactly).
    static constexpr float kTagFallbackAdvance = 6.0f;

    static constexpr int kDefaultWidth = 240;
    static constexpr int kHeight = 38;
    static constexpr float kRadius = 19.0f;
    static constexpr UINT_PTR kTimerIdle = 1001;
    static constexpr UINT_PTR kTimerSingleClick = 1002;
    // 261029_0001 U-I (R1-21): one topology change arrives as a BURST of
    // WM_DISPLAYCHANGE messages (mode set, then the DPI/arrangement settle), so
    // the re-clamp is armed on this timer and re-armed by each further message:
    // SetTimer with an existing id restarts the countdown, collapsing the burst
    // into one reposition. Same SetTimer/WM_TIMER idiom as kTimerIdle above.
    static constexpr UINT_PTR kTimerDisplayChange = 1003;
    static constexpr DWORD kDisplayChangeDebounceMs = 250;
    static constexpr DWORD kIdleTimeoutMs = 5000;

    bool is_mouse_down_ = false;
    // REQ-052 P3: single-click timer delay, captured once at Create() from
    // GetDoubleClickTime() (the OS double-click window, default 500ms). The
    // old fixed 220ms fired the pause-toggle action before a 250-350ms
    // double-click arrived and could cancel it via WM_LBUTTONDBLCLK.
    UINT single_click_delay_ms_ = 220; // overwritten in Create()
    bool is_dragging_ = false;
    POINT drag_start_cursor_ = {};
    POINT drag_start_window_ = {};
};

} // namespace emebalachat
