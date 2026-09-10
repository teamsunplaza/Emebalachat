#include "layered_renderer.hpp"

namespace emebalachat {

LayeredD2DRenderer::~LayeredD2DRenderer() {
    ReleaseTarget();
    FreeBuffer();
}

bool LayeredD2DRenderer::CreateTarget(ID2D1Factory* factory,
                                      ID2D1DCRenderTarget** out_target) {
    ReleaseTarget(out_target);
    if (!factory) {
        return false;
    }
    // Byte-identical properties at all four original sites (REF-3.6 §6).
    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    ID2D1DCRenderTarget* target = nullptr;
    if (FAILED(factory->CreateDCRenderTarget(&rtProps, &target))) {
        if (out_target) {
            *out_target = nullptr;
        }
        return false;
    }
    target_ = target;
    if (out_target) {
        *out_target = target;
    }
    return true;
}

void LayeredD2DRenderer::ReleaseTarget(ID2D1DCRenderTarget** out_target) {
    if (target_) {
        target_->Release();
        target_ = nullptr;
    }
    if (out_target) {
        *out_target = nullptr;
    }
}

void LayeredD2DRenderer::ReallocateBuffer(int width, int height, UINT dpi, bool bind) {
    FreeBuffer();

    HDC hScreenDC = ::GetDC(nullptr);
    mem_dc_ = ::CreateCompatibleDC(hScreenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    bitmap_ = ::CreateDIBSection(mem_dc_, &bmi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    old_bitmap_ = static_cast<HBITMAP>(::SelectObject(mem_dc_, bitmap_));
    ::ReleaseDC(nullptr, hScreenDC);

    if (bind && target_) {
        // REQ-R15: keep the D2D DPI transform in sync with the monitor DPI
        // whenever the physical buffer is re-allocated (initial create and
        // cross-DPI moves both land here).
        target_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
        RECT rc = { 0, 0, width, height };
        target_->BindDC(mem_dc_, &rc);
    }
}

void LayeredD2DRenderer::FreeBuffer() {
    if (mem_dc_) {
        if (old_bitmap_) {
            ::SelectObject(mem_dc_, old_bitmap_);
            old_bitmap_ = nullptr;
        }
        if (bitmap_) {
            ::DeleteObject(bitmap_);
            bitmap_ = nullptr;
        }
        ::DeleteDC(mem_dc_);
        mem_dc_ = nullptr;
        bits_ = nullptr;
    }
}

void LayeredD2DRenderer::Rebind(int width, int height, UINT dpi, bool set_dpi) {
    if (!target_ || !mem_dc_) {
        return;
    }
    if (set_dpi) {
        target_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
    }
    const RECT rc = { 0, 0, width, height };
    target_->BindDC(mem_dc_, &rc);
}

void LayeredD2DRenderer::Present(HWND hwnd, int width, int height, BYTE alpha) {
    if (!hwnd || !mem_dc_) {
        return;
    }

    POINT ptSrc = { 0, 0 };
    SIZE sz = { width, height }; // physical blit size (matches the DIB)
    POINT ptDst = {};
    RECT rcWindow = {};
    ::GetWindowRect(hwnd, &rcWindow);
    ptDst.x = rcWindow.left;
    ptDst.y = rcWindow.top;

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = alpha;
    blend.AlphaFormat = AC_SRC_ALPHA;

    HDC hScreenDC = ::GetDC(nullptr);
    ::UpdateLayeredWindow(
        hwnd,
        hScreenDC,
        &ptDst,
        &sz,
        mem_dc_,
        &ptSrc,
        0,
        &blend,
        ULW_ALPHA
    );
    ::ReleaseDC(nullptr, hScreenDC);
}

} // namespace emebalachat
