#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

namespace emebalachat {

class DragIconWindow {
public:
    using ClickCallback = std::function<void(int click_x, int click_y)>;

    DragIconWindow();
    ~DragIconWindow();

    bool Create(HINSTANCE hInstance);
    void Destroy();

    // ShowAt/Hide are safe to call from ANY thread: when invoked off the GUI
    // thread that created the window (mouse-hook thread, delayed-click worker),
    // they marshal themselves via PostMessageW to WndProc (REQ-R10, audit §3.4).
    // Same-thread callers run inline.
    void ShowAt(int x, int y);
    void Hide();
    bool IsVisible() const { return visible_.load(std::memory_order_relaxed); }

    HWND GetHwnd() const { return hwnd_; }
    void SetClickCallback(ClickCallback cb) { click_cb_ = std::move(cb); }

    // REQ-R15 (audit §5 latent item 3): logical pill size in DIPs. The window,
    // the DIB buffer, and UpdateLayeredWindow are sized in PHYSICAL pixels of
    // the monitor the icon is shown on: phys_size_ = ScaleDipsToPixels(kSize,
    // dpi_) with dpi_ from ui::MonitorDpiAtPoint at ShowAt time, and the D2D
    // render target DPI is set to dpi_ so all DIP-authored geometry below
    // (rounded pill, logo rect) rasterizes crisply at 1:1 physical scale.
    static constexpr int kSize = 32;

    // ---- REQ-R10 (audit §3.4): D2D thread marshal for ShowAt ----
    //
    // The D2D factory and DC render target are created SINGLE_THREADED on the
    // main GUI thread (Create() runs in wWinMain). Calling ShowAt() from the
    // mouse-hook thread makes BeginDraw/EndDraw return D2DERR_WRONG_THREAD and
    // the icon renders invisible. RequestShowAt() is the thread-safe seam: it
    // posts kShowAtMessage to the icon's own window, so WndProc - dispatched by
    // the main GUI thread's message pump - performs the actual ShowAt(). Safe
    // from ANY thread including LowLevelMouseProc (PostMessageW never blocks on
    // the target queue; it only enqueues and returns).
    //
    // Coordinate packing choice: x travels in WPARAM, y in LPARAM, as
    // sign-extended int values. MAKELPARAM(x, y) was deliberately NOT used:
    // it truncates each coordinate to 16 bits, which corrupts large negative
    // virtual-screen offsets on multi-monitor rigs (primary spans can push
    // coordinates past INT16). WPARAM/LPARAM are pointer-sized (64-bit on x64,
    // 32-bit on x86, int is 32-bit on both), so the int round-trip is lossless.
    // No heap-allocated struct either: nothing to leak or double-free, and
    // rapid drag releases coalesce naturally in the queue (latest wins).
    static constexpr UINT kShowAtMessage = WM_APP + 0x101;
    // Hide marshaled the same way (ESC/outside-click dismiss paths run on the
    // keyboard/mouse hook threads).
    static constexpr UINT kHideMessage = WM_APP + 0x102;

    static constexpr WPARAM PackShowAtX(int x) {
        return static_cast<WPARAM>(static_cast<INT_PTR>(x));
    }
    static constexpr LPARAM PackShowAtY(int y) {
        return static_cast<LPARAM>(static_cast<INT_PTR>(y));
    }
    static constexpr int ShowAtXFromWParam(WPARAM w) {
        return static_cast<int>(static_cast<INT_PTR>(w));
    }
    static constexpr int ShowAtYFromLParam(LPARAM l) {
        return static_cast<int>(static_cast<INT_PTR>(l));
    }

    // Returns false when hwnd is null (icon Create() failed or was Destroy()d)
    // or when PostMessage failed; callers may treat false as "no icon shown".
    static bool RequestShowAt(HWND hwnd, int x, int y) {
        if (!hwnd) {
            return false;
        }
        return ::PostMessageW(hwnd, kShowAtMessage, PackShowAtX(x), PackShowAtY(y)) == TRUE;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void Render();
    void UpdateLayered();
    void StartFadeoutTimer();
    void StopFadeoutTimer();
    void LoadLogoBitmap();
    // R6 Phase 3 (audit item 4, plan §3.1 A3): recreate the DC render target
    // (+ device-dependent logo bitmap) after EndDraw returns
    // D2DERR_RECREATE_TARGET, so a driver reset cannot leave the icon a
    // permanently invisible pill.
    void RecreateAfterDeviceLost();
    // REQ-R15: (re)allocate the DIB + bind the DC render target at the given
    // physical edge size / DPI. No-op while geometry is unchanged.
    void EnsureBuffer(UINT dpi, int phys_edge);
    void RebindRenderTarget();

    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    DWORD gui_thread_id_ = 0; // thread that ran Create(); owns all D2D/GUI calls
    std::atomic<bool> visible_{false}; // read cross-thread via IsVisible()
    bool is_hovered_ = false;          // WndProc (GUI thread) only
    BYTE alpha_ = 235;

    HDC hMemDC_ = nullptr;
    HBITMAP hBitmap_ = nullptr;
    HBITMAP hOldBitmap_ = nullptr;
    void* pBits_ = nullptr;

    // REQ-R15: physical buffer geometry for the monitor the icon last showed on.
    UINT dpi_ = 96;
    int phys_size_ = kSize; // physical px edge length of window/DIB

    ID2D1Factory* d2d_factory_ = nullptr;
    ID2D1DCRenderTarget* dc_render_target_ = nullptr;
    ID2D1Bitmap* logo_bitmap_ = nullptr;

    ClickCallback click_cb_;

    static constexpr UINT_PTR kTimerFadeout = 3001;
    static constexpr DWORD kFadeoutTimeoutMs = 2500;
};

} // namespace emebalachat
