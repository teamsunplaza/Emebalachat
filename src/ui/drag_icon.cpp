#include "drag_icon.hpp"
#include "asset_loader.hpp"

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
        L"Emebalachat Drag Icon",
        WS_POPUP,
        -100, -100, kSize, kSize,
        nullptr, nullptr, hInstance_, this
    );

    if (!hwnd_) {
        return false;
    }

    // Allocate 32-bit DIB Section & Memory DC
    HDC hScreenDC = ::GetDC(nullptr);
    hMemDC_ = ::CreateCompatibleDC(hScreenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = kSize;
    bmi.bmiHeader.biHeight = -kSize; // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    hBitmap_ = ::CreateDIBSection(hMemDC_, &bmi, DIB_RGB_COLORS, &pBits_, nullptr, 0);
    hOldBitmap_ = static_cast<HBITMAP>(::SelectObject(hMemDC_, hBitmap_));
    ::ReleaseDC(nullptr, hScreenDC);

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

    RECT rc = { 0, 0, kSize, kSize };
    dc_render_target_->BindDC(hMemDC_, &rc);

    LoadLogoBitmap();

    return true;
}

void DragIconWindow::LoadLogoBitmap() {
    if (!dc_render_target_) return;
    if (logo_bitmap_) {
        logo_bitmap_->Release();
        logo_bitmap_ = nullptr;
    }

    std::wstring logoPath = FindLogoPath();
    if (!logoPath.empty()) {
        LoadWicBitmap(dc_render_target_, logoPath, &logo_bitmap_);
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

    // Multi-monitor aware bounds clamping
    POINT pt = { x, y };
    HMONITOR hMon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(MONITORINFO);
    if (::GetMonitorInfoW(hMon, &mi)) {
        if (x + kSize > mi.rcWork.right) x = mi.rcWork.right - kSize - 4;
        if (y + kSize > mi.rcWork.bottom) y = mi.rcWork.bottom - kSize - 4;
        if (x < mi.rcWork.left) x = mi.rcWork.left + 4;
        if (y < mi.rcWork.top) y = mi.rcWork.top + 4;
    }

    is_hovered_ = false;
    alpha_ = 230;

    ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, kSize, kSize, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;

    Render();
    UpdateLayered();
    StartFadeoutTimer();
}

void DragIconWindow::Hide() {
    if (!hwnd_ || !visible_) return;
    StopFadeoutTimer();
    visible_ = false;
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

    RECT rc = { 0, 0, kSize, kSize };
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

    // Centered Emebala Logo inside 32x32 pill
    // Aspect ratio 427:271 (~1.576). Target size: 24 x 15.2
    D2D1_RECT_F logoRect = D2D1::RectF(4.0f, 8.4f, 28.0f, 23.6f);

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
    SIZE sz = { kSize, kSize };
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
