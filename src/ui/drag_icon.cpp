#include "drag_icon.hpp"
#include "asset_loader.hpp"
#include "dpi.hpp"

#include <cmath>

namespace emebalachat {

namespace {
const wchar_t kDragIconClassName[] = L"Emebalachat_DragIconClass";
}

DragIconWindow::DragIconWindow() = default;

DragIconWindow::~DragIconWindow() {
    Destroy();
}

bool DragIconWindow::Create(HINSTANCE hInstance) {
    hInstance_ = hInstance;
    // REQ-R10 (audit §3.4): capture the owning GUI thread. Create() runs on the
    // main thread in wWinMain (and the test main thread in run_tests). ShowAt /
    // Hide called from any other thread (mouse hook, delayed-click worker) are
    // marshaled to WndProc via PostMessageW instead of touching the
    // single-threaded D2D render target cross-thread (D2DERR_WRONG_THREAD).
    gui_thread_id_ = ::GetCurrentThreadId();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DragIconWindow::WndProc;
    wc.hInstance = hInstance_;
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32649));
    wc.lpszClassName = kDragIconClassName;
    ::RegisterClassExW(&wc);

    hwnd_ = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kDragIconClassName,
        L"Emebala Chat Drag Icon",
        WS_POPUP,
        -100, -100, kSize, kSize,
        nullptr, nullptr, hInstance_, this
    );

    if (!hwnd_) {
        return false;
    }

    // REQ-R15: initial DIB + render-target binding at the default (96 dpi)
    // geometry; ShowAt() re-scales to the monitor the icon actually lands on.
    EnsureBuffer(dpi_, phys_size_);

    // Initialize Direct2D
    if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory_))) {
        return false;
    }

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );

    if (FAILED(d2d_factory_->CreateDCRenderTarget(&rtProps, &dc_render_target_))) {
        return false;
    }

    RebindRenderTarget();

    LoadLogoBitmap();

    return true;
}

// REQ-R15: (re)allocate the 32-bit top-down DIB for `phys_edge` physical
// pixels and remember the monitor DPI so Render() can set the matching D2D
// DPI transform. Cheap no-op while geometry is unchanged (the common case:
// rapid ShowAt moves on one monitor).
void DragIconWindow::EnsureBuffer(UINT dpi, int phys_edge) {
    if (hBitmap_ && dpi == dpi_ && phys_edge == phys_size_) {
        return;
    }
    dpi_ = dpi;
    phys_size_ = phys_edge;

    if (hMemDC_) {
        if (hOldBitmap_) {
            ::SelectObject(hMemDC_, hOldBitmap_);
            hOldBitmap_ = nullptr;
        }
        if (hBitmap_) {
            ::DeleteObject(hBitmap_);
            hBitmap_ = nullptr;
        }
        ::DeleteDC(hMemDC_);
        hMemDC_ = nullptr;
    }

    HDC hScreenDC = ::GetDC(nullptr);
    hMemDC_ = ::CreateCompatibleDC(hScreenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = phys_size_;
    bmi.bmiHeader.biHeight = -phys_size_; // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    hBitmap_ = ::CreateDIBSection(hMemDC_, &bmi, DIB_RGB_COLORS, &pBits_, nullptr, 0);
    hOldBitmap_ = static_cast<HBITMAP>(::SelectObject(hMemDC_, hBitmap_));
    ::ReleaseDC(nullptr, hScreenDC);

    RebindRenderTarget();
}

void DragIconWindow::RebindRenderTarget() {
    if (!dc_render_target_ || !hMemDC_) {
        return;
    }
    const RECT rc = { 0, 0, phys_size_, phys_size_ };
    dc_render_target_->BindDC(hMemDC_, &rc);
    // Render target DPI = monitor DPI: all DIP-authored geometry in Render()
    // then rasterizes 1:1 into the physical-pixel DIB (crisp at 150%/200%).
    dc_render_target_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
}

void DragIconWindow::LoadLogoBitmap() {
    if (!dc_render_target_) return;
    if (logo_bitmap_) {
        logo_bitmap_->Release();
        logo_bitmap_ = nullptr;
    }

    std::wstring iconPath = FindAppIconPath();
    if (iconPath.empty()) {
        iconPath = FindLogoPath();
    }
    if (!iconPath.empty()) {
        LoadWicBitmap(dc_render_target_, iconPath, &logo_bitmap_);
    }
}

void DragIconWindow::Destroy() {
    if (hwnd_) {
        StopFadeoutTimer();
        ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    if (logo_bitmap_) { logo_bitmap_->Release(); logo_bitmap_ = nullptr; }
    if (dc_render_target_) { dc_render_target_->Release(); dc_render_target_ = nullptr; }
    if (d2d_factory_) { d2d_factory_->Release(); d2d_factory_ = nullptr; }

    if (hMemDC_) {
        if (hOldBitmap_) {
            ::SelectObject(hMemDC_, hOldBitmap_);
            hOldBitmap_ = nullptr;
        }
        if (hBitmap_) {
            ::DeleteObject(hBitmap_);
            hBitmap_ = nullptr;
        }
        ::DeleteDC(hMemDC_);
        hMemDC_ = nullptr;
    }
}

void DragIconWindow::ShowAt(int x, int y) {
    if (!hwnd_) return;

    // REQ-R10 (audit §3.4) thread-affinity guard: off the owning GUI thread,
    // marshal through the icon's own window queue (PostMessage, non-blocking for
    // the hook/worker caller). WndProc re-enters this method on the GUI thread,
    // where GetCurrentThreadId()==gui_thread_id_, so the draw runs inline with
    // no recursion.
    if (::GetCurrentThreadId() != gui_thread_id_) {
        RequestShowAt(hwnd_, x, y);
        return;
    }

    // REQ-R15 (audit §5 latent item 3): the LL mouse coordinates arriving here
    // are PHYSICAL pixels of the monitor under the cursor. Query that
    // monitor's effective DPI, scale the 32-DIP logical pill to its physical
    // size, resize the DIB + D2D target DPI accordingly (no-op while the icon
    // stays on one monitor), and clamp in the SAME physical units - the old
    // code mixed a DIP-sized constant (kSize) with physical monitor rects,
    // which offsets the icon by (scale-1)*size/2 on 150%/200% secondary
    // monitors and renders it blurry (DWM stretch) on unaware processes.
    const POINT pt_cursor = { x, y };
    const UINT dpi = emebalachat::ui::MonitorDpiAtPoint(pt_cursor);
    const int phys = emebalachat::ui::ScaleDipsToPixels(kSize, dpi);
    EnsureBuffer(dpi, phys);

    // Multi-monitor aware bounds clamping (physical px vs physical work area)
    POINT pt = { x, y };
    HMONITOR hMon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(MONITORINFO);
    if (::GetMonitorInfoW(hMon, &mi)) {
        const POINT clamped = emebalachat::ui::ClampWindowOrigin(x, y, phys_size_, phys_size_, 4, mi.rcWork);
        x = clamped.x;
        y = clamped.y;
    }

    is_hovered_ = false;
    alpha_ = 230;

    ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, phys_size_, phys_size_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_.store(true, std::memory_order_release);

    Render();
    UpdateLayered();
    StartFadeoutTimer();
}

void DragIconWindow::Hide() {
    if (!hwnd_) return;
    // Same marshal rule as ShowAt: the ESC/outside-click dismiss paths call this
    // from the keyboard/mouse hook threads. SetWindowPos/ShowWindow/D2D must run
    // on the GUI thread. WndProc re-enters on the GUI thread (no recursion).
    if (::GetCurrentThreadId() != gui_thread_id_) {
        ::PostMessageW(hwnd_, kHideMessage, 0, 0);
        return;
    }
    if (!visible_.load(std::memory_order_acquire)) return;
    StopFadeoutTimer();
    visible_.store(false, std::memory_order_release);
    is_hovered_ = false;
    ::ShowWindow(hwnd_, SW_HIDE);
}

void DragIconWindow::StartFadeoutTimer() {
    if (hwnd_) {
        ::KillTimer(hwnd_, kTimerFadeout);
        ::SetTimer(hwnd_, kTimerFadeout, kFadeoutTimeoutMs, nullptr);
    }
}

void DragIconWindow::StopFadeoutTimer() {
    if (hwnd_) {
        ::KillTimer(hwnd_, kTimerFadeout);
    }
}

void DragIconWindow::Render() {
    if (!dc_render_target_) return;

    // REQ-R15: geometry stays authored in DIPs (kSize = 32 logical); the
    // render target DPI (set in EnsureBuffer/RebindRenderTarget) maps DIP ->
    // physical px. BindDC's rect must match the physical buffer.
    RECT rc = { 0, 0, phys_size_, phys_size_ };
    dc_render_target_->BindDC(hMemDC_, &rc);

    dc_render_target_->BeginDraw();
    dc_render_target_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    // Outer rounded pill container (32x32 rounded icon pill)
    D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
        D2D1::RectF(1.0f, 1.0f, static_cast<float>(kSize) - 1.0f, static_cast<float>(kSize) - 1.0f),
        8.0f, 8.0f
    );

    ID2D1SolidColorBrush* bgBrush = nullptr;
    ID2D1SolidColorBrush* borderBrush = nullptr;

    if (is_hovered_) {
        dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x1E293B, 0.98f), &bgBrush);
        dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0xE5C158, 1.0f), &borderBrush);
    } else {
        dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x0F172A, 0.92f), &bgBrush);
        dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0xD4AF37, 0.90f), &borderBrush);
    }

    if (bgBrush) {
        dc_render_target_->FillRoundedRectangle(pill, bgBrush);
    }
    if (borderBrush) {
        dc_render_target_->DrawRoundedRectangle(pill, borderBrush, is_hovered_ ? 1.8f : 1.2f);
    }

    // Centered 1:1 squircle appicon (24x24 DIP centered in 32x32 container)
    D2D1_RECT_F logoRect = D2D1::RectF(4.0f, 4.0f, 28.0f, 28.0f);

    if (logo_bitmap_) {
        dc_render_target_->DrawBitmap(
            logo_bitmap_,
            logoRect,
            1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
        );
    } else {
        DrawTabletLogoVector(dc_render_target_, logoRect, is_hovered_);
    }

    if (borderBrush) borderBrush->Release();
    if (bgBrush) bgBrush->Release();

    dc_render_target_->EndDraw();
}

void DragIconWindow::UpdateLayered() {
    if (!hwnd_ || !hMemDC_) return;

    POINT ptSrc = { 0, 0 };
    // REQ-R15: blit the PHYSICAL buffer size (matches the DIB and window).
    SIZE sz = { phys_size_, phys_size_ };
    POINT ptDst = {};
    RECT rcWindow = {};
    ::GetWindowRect(hwnd_, &rcWindow);
    ptDst.x = rcWindow.left;
    ptDst.y = rcWindow.top;

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = alpha_;
    blend.AlphaFormat = AC_SRC_ALPHA;

    HDC hScreenDC = ::GetDC(nullptr);
    ::UpdateLayeredWindow(
        hwnd_,
        hScreenDC,
        &ptDst,
        &sz,
        hMemDC_,
        &ptSrc,
        0,
        &blend,
        ULW_ALPHA
    );
    ::ReleaseDC(nullptr, hScreenDC);
}

LRESULT CALLBACK DragIconWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* pThis = reinterpret_cast<DragIconWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = static_cast<DragIconWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    if (!pThis) {
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
        // REQ-R10 (audit §3.4): ShowAt marshaled from the mouse-hook thread via
        // RequestShowAt()/PostMessageW. This case runs on the GUI thread that
        // owns the single-threaded D2D render target, curing D2DERR_WRONG_THREAD.
        // Unpack with the lossless int round-trip documented in drag_icon.hpp.
        case DragIconWindow::kShowAtMessage: {
            const int x = DragIconWindow::ShowAtXFromWParam(wParam);
            const int y = DragIconWindow::ShowAtYFromLParam(lParam);
            pThis->ShowAt(x, y);
            return 0;
        }

        case DragIconWindow::kHideMessage: {
            pThis->Hide(); // already on the GUI thread here
            return 0;
        }

        case WM_MOUSEACTIVATE:
            // Return MA_NOACTIVATE so clicking the drag icon does not take focus
            // from the target window where text is currently selected.
            return MA_NOACTIVATE;

        case WM_SETCURSOR:
            ::SetCursor(::LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
            return TRUE;

        case WM_MOUSEMOVE: {
            if (!pThis->is_hovered_) {
                pThis->is_hovered_ = true;
                pThis->StopFadeoutTimer();
                pThis->Render();
                pThis->UpdateLayered();

                TRACKMOUSEEVENT tme = {};
                tme.cbSize = sizeof(TRACKMOUSEEVENT);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                ::TrackMouseEvent(&tme);
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            pThis->is_hovered_ = false;
            pThis->StartFadeoutTimer();
            pThis->Render();
            pThis->UpdateLayered();
            return 0;
        }

        case WM_LBUTTONUP: {
            POINT pt = {};
            ::GetCursorPos(&pt);
            auto cb = pThis->click_cb_;
            pThis->Hide();
            if (cb) {
                cb(pt.x, pt.y);
            }
            return 0;
        }

        case WM_TIMER: {
            if (wParam == kTimerFadeout) {
                pThis->Hide();
                return 0;
            }
            break;
        }

        case WM_DESTROY:
            return 0;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace emebalachat
