#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include "layered_renderer.hpp"

namespace emebalachat {

// REQ-005 (architect plan §2.2): branded About popup opened from the tray
// context menu ("About Emebala Chat…"). Self-contained Direct2D layered popup
// cloning TooltipWindow's proven architecture: own WndProc, own single-threaded
// D2D factory + DC render target + DWrite factory, own DIB mem-DC,
// UpdateLayeredWindow blit. Same palette as the tooltip card (bg 0x0F172A,
// border 0x334155, text 0xF8FAFC, subtext 0x94A3B8, accent 0x10B981, gold
// 0xD4AF37) — no visual redesign, brand consistency only.
//
// Unlike the tooltip this window IS activatable (no WS_EX_NOACTIVATE): it is a
// deliberate user action and must receive keyboard focus so ESC works. It
// dismisses on the ✕ button, ESC (WM_KEYDOWN in its own WndProc), focus loss
// (WM_KILLFOCUS), or outside click (main.cpp's mouse-hook dismissal list).
//
// Threading: Create/Show/Dismiss/Destroy are GUI-thread operations. Show()
// marshals via PostMessageW when called from another thread (REQ-R10 pattern
// shared with TooltipWindow), so tray callbacks can invoke it directly.
class AboutWindow {
public:
    AboutWindow();
    ~AboutWindow();

    bool Create(HINSTANCE hInstance);
    void Destroy();

    // Shows the window centered on the work area of the monitor containing
    // (x, y) (typically the cursor position when the tray item was clicked).
    void Show(int x, int y);
    void Dismiss();

    bool IsVisible() const { return visible_.load(std::memory_order_relaxed); }
    HWND GetHwnd() const { return hwnd_; }

    // Number of link buttons (Website / Contact / Reddit - R6 Phase 5 replaced
    // the GitHub-releases Download link with the public Reddit community per
    // the user's decision; URLs live in about_window.cpp).
    static constexpr int kNumLinks = 3;

    // Marshaled message IDs — WM_APP+0x300 block: distinct from the tooltip's
    // 0x200 block and the drag icon's 0x100 block (see tooltip.hpp comments).
    // SEC-ADJ (release readiness 260911_0002, Blocker-5 class): kShowMessage
    // used to carry a heap ShowPayload* in LPARAM, which WndProc cast back and
    // freed — a shatter primitive (CWE-822) on this top-level, guessable-class
    // window. The two-int payload now travels losslessly in the message
    // parameters themselves (see PackShowX/PackShowY below): no pointer ever
    // crosses the seam, so attacker-posted values can at most move the card
    // off-screen (ShowAt clamps it back to the monitor work area anyway);
    // there is nothing to dereference or free.
    static constexpr UINT kShowMessage   = WM_APP + 0x301;
    static constexpr UINT kDismissMessage = WM_APP + 0x302;
    // R6 Phase 6 (plan §5.2): locale-change re-render request (no heap payload,
    // same ownership contract as kDismissMessage).
    static constexpr UINT kLocaleRefreshMessage = WM_APP + 0x303;

    // Coordinate packing for kShowMessage (SEC-ADJ): x travels in WPARAM, y in
    // LPARAM, as sign-extended int values — the exact DragIconWindow::RequestShowAt
    // contract (drag_icon.hpp). MAKELPARAM was deliberately NOT used: it
    // truncates each coordinate to 16 bits, corrupting large negative
    // virtual-screen offsets on multi-monitor rigs (Show() is fed cursor
    // positions, which go below INT16_MIN on left/top secondary monitors);
    // the pointer-sized parameters round-trip a full 32-bit int on both x64
    // and x86, keeping today's exact show/move semantics.
    static constexpr WPARAM PackShowX(int x) {
        return static_cast<WPARAM>(static_cast<INT_PTR>(x));
    }
    static constexpr LPARAM PackShowY(int y) {
        return static_cast<LPARAM>(static_cast<INT_PTR>(y));
    }
    static constexpr int ShowXFromWParam(WPARAM w) {
        return static_cast<int>(static_cast<INT_PTR>(w));
    }
    static constexpr int ShowYFromLParam(LPARAM l) {
        return static_cast<int>(static_cast<INT_PTR>(l));
    }

    // R6 Phase 6: refresh caption + localized body after a UI-language switch.
    // Thread-safe (marshals like Show/Dismiss). Re-renders only while visible;
    // a hidden window picks the new strings up on its next Show (Render reads
    // I18n::Get fresh every time — nothing is cached at Create).
    void RequestLocaleRefresh();

    // Phase 4 (REQ-020, plan §1.4): wires the "Reset to system defaults"
    // button. The window stays a pure view: it owns NO config knowledge; on
    // click it invokes this callback (GUI thread — the WndProc runs there) and
    // shows the brief "done" feedback regardless (optimistic: the
    // coordinator's Set*+SaveToFile cannot fail softly; a SaveToFile failure
    // is already DIAG-logged inside config). Set once from main.cpp after
    // construction (same wiring pattern as the tooltip's language callback).
    void SetResetCallback(std::function<void()> cb) { reset_callback_ = std::move(cb); }

    // (SEC-ADJ: the old heap `struct ShowPayload { int x; int y; }` that
    // travelled as a raw LPARAM pointer is deleted with the pointer transport —
    // the coordinates now travel losslessly in the message parameters
    // (PackShowX/PackShowY above). No test or product code references it.)

    // R6 Phase 5 (plan §5.2): pure localized-content snapshot resolved from the
    // i18n tables for the CURRENT locale. Headless test seam (TestR6P5AboutI18n
    // asserts KO/EN/JA rendering); Render() consumes the same I18n::Get ids.
    struct LocalizedContent {
        std::wstring title;
        std::wstring tagline;
        std::wstring features[3];
        std::wstring etymology;
        std::wstring link_labels[kNumLinks];
        std::wstring contacts[3];
        // Phase 4 (REQ-020, plan §2.5): the reset button's resting label. The
        // transient post-click "done" label is NOT part of this snapshot —
        // content is the static string set at Show/Render time; Render reads
        // StringId::AboutResetDone directly while the feedback window is open.
        std::wstring reset_label;
    };
    static LocalizedContent BuildLocalizedContent();

    // (Removed with the SEC-ADJ fix: the old DrainMarshalQueue PeekMessageW
    // sweep existed solely to delete heap ShowPayload pointers the DestroyWindow
    // queue purge would have leaked. kShowMessage now carries the coordinates
    // losslessly in WPARAM/LPARAM (PackShowX/PackShowY), so there is nothing
    // to free; a still-queued show notification at teardown is harmless.)

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void ShowAt(int x, int y); // real show path, GUI thread only
    void Render();
    void UpdateLayered();
    void ReallocateBuffer(int width, int height); // physical px buffer
    int PhysW() const;
    int PhysH() const;
    void RebindRenderTarget();
    // R6 Phase 3 (audit item 4, plan §3.1 A3): recreate the DC render target
    // (+ device-dependent logo bitmap) after EndDraw returns
    // D2DERR_RECREATE_TARGET, so a driver reset cannot leave the About card
    // permanently blank.
    void RecreateAfterDeviceLost();
    void LoadLogoBitmap();
    // C1 (session 260910_0007): the single persistent scratch brush the whole
    // Render path draws through (SetColor just before every Fill/Draw/DrawText
    // call). Created at Create() and device-lost recovery - NEVER at render
    // entry - and released in RecreateAfterDeviceLost (before the old target
    // is dropped) and Destroy. Null when target creation failed; Render bails
    // on a null brush exactly like the old per-brush null guards.
    void EnsureScratchBrush();
    void ReleaseScratchBrush();
    // P4 Batch B-5 (session 260907_0002, design §2.2.3): align the
    // UI-locale-dependent text formats with the active I18n locale —
    // body/tagline/etymology reading direction (RTL for AR/FA/UR/HE) plus the
    // Q5-A font-fallback localeName (clone-swap; localeName is creation-only
    // in DWrite, B-2 SDK audit) on all five localized-copy formats. Chrome
    // formats (title/version/header) stay untouched LTR/neutral-locale.
    // Called from Create() (startup locale incl. auto-detected RTL OS) and
    // from RequestLocaleRefresh on every UI-language switch. GUI thread only
    // (owns the DWrite formats exactly like Render).
    void ApplyLocaleFormatting();
    void OpenLink(int index); // 0=website 1=contact 2=reddit (ShellExecuteW)

    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    DWORD gui_thread_id_ = 0; // thread that ran Create(); WndProc dispatch owner
    std::atomic<bool> visible_{false};

    int current_width_ = 440;  // DIP (plan §2.2 fixed size)
    // Phase 4 (REQ-020, plan §2.2): 560 -> 596 DIP. The contact block ends at
    // y=536 and the card had no room left, so the reset button (y 546..578)
    // required the +36 DIP expansion. ClampWindowOrigin in ShowAt already
    // keeps the taller card inside the work area.
    int current_height_ = 596; // DIP
    UINT dpi_ = 96;            // REQ-R15: DPI of the monitor showing the window

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
    IDWriteTextFormat* title_format_ = nullptr;     // 22 SemiBold, centered
    IDWriteTextFormat* version_format_ = nullptr;   // 11, centered, subtext
    IDWriteTextFormat* tagline_format_ = nullptr;   // 12, wrapped, centered
    IDWriteTextFormat* body_format_ = nullptr;      // 12, wrapped, leading
    IDWriteTextFormat* etymology_format_ = nullptr; // 11 italic, centered
    IDWriteTextFormat* link_format_ = nullptr;      // 12 medium, centered
    IDWriteTextFormat* small_format_ = nullptr;     // 10.5, centered (contact)
    IDWriteTextFormat* header_format_ = nullptr;    // 12 centered (close ✕)

    // Interactive rectangles, DIP window coords (recomputed every Render).
    D2D1_RECT_F close_btn_rect_ = {};
    D2D1_RECT_F link_rects_[kNumLinks] = {};
    int hovered_link_ = -1; // 0..2 link, 3 = close, 4 = reset, -1 none

    // Phase 4 (REQ-020, plan §2.2): the full-width "Reset to system defaults"
    // action button under the contact block, its transient feedback deadline
    // (GetTickCount64 ms; 0 = inactive), and the coordinator callback.
    static constexpr int kHoverReset = 4;
    static constexpr UINT_PTR kResetFeedbackTimerId = 0xB1B1; // unique on this hwnd
    static constexpr UINT kResetFeedbackMs = 1600;            // plan §2.2: 1.6 s
    D2D1_RECT_F reset_rect_ = {};
    ULONGLONG reset_feedback_until_ = 0;
    std::function<void()> reset_callback_;
};

} // namespace emebalachat
