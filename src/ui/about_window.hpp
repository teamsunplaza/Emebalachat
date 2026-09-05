#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

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

    // Number of link buttons (Website / Contact / Download).
    static constexpr int kNumLinks = 3;

    // Marshaled message IDs — WM_APP+0x300 block: distinct from the tooltip's
    // 0x200 block and the drag icon's 0x100 block (see tooltip.hpp comments).
    static constexpr UINT kShowMessage   = WM_APP + 0x301;
    static constexpr UINT kDismissMessage = WM_APP + 0x302;

    struct ShowPayload {
        int x;
        int y;
    };

    // R6 Phase 3 (audit item 8): drains THIS window's marshal queue before
    // DestroyWindow, freeing every still-queued heap ShowPayload (the OS queue
    // purge at window destruction would otherwise drop the LPARAM pointers
    // without running any destructor). GUI-thread-only (PeekMessage is
    // thread-queue scoped). Returns the number of payloads freed (test seam).
    int DrainMarshalQueue();

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
    void OpenLink(int index); // 0=website 1=contact 2=download (ShellExecuteW)

    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    DWORD gui_thread_id_ = 0; // thread that ran Create(); WndProc dispatch owner
    std::atomic<bool> visible_{false};

    int current_width_ = 440;  // DIP (plan §2.2 fixed size)
    int current_height_ = 560; // DIP
    UINT dpi_ = 96;            // REQ-R15: DPI of the monitor showing the window

    // GDI Memory DC & DIB Section
    HDC hMemDC_ = nullptr;
    HBITMAP hBitmap_ = nullptr;
    HBITMAP hOldBitmap_ = nullptr;
    void* pBits_ = nullptr;

    // Direct2D & DirectWrite
    ID2D1Factory* d2d_factory_ = nullptr;
    ID2D1DCRenderTarget* dc_render_target_ = nullptr;
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
    int hovered_link_ = -1; // 0..2 link, 3 = close, -1 none
};

} // namespace emebalachat
