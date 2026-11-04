#include "drag_icon.hpp"
#include "diag_logger.hpp"
#include "asset_loader.hpp"
#include "dpi.hpp"
#include "theme.hpp"

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

    // REF-3.6: target creation + lifetime owned by renderer_; dc_render_target_
    // stays a non-owning alias so the render code below is untouched.
    if (!renderer_.CreateTarget(d2d_factory_, &dc_render_target_)) {
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
    if (renderer_.has_bitmap() && dpi == dpi_ && phys_edge == phys_size_) {
        return;
    }
    dpi_ = dpi;
    phys_size_ = phys_edge;

    // REF-3.6: DIB (re)creation owned by the shared renderer. bind=false keeps
    // this site's original ordering: the buffer is freed/recreated first, then
    // RebindRenderTarget() re-attaches the target (BindDC + SetDpi) at the new
    // square geometry.
    renderer_.ReallocateBuffer(phys_size_, phys_size_, dpi_, /*bind=*/false);
    RebindRenderTarget();
}

void DragIconWindow::RebindRenderTarget() {
    // Render target DPI = monitor DPI: all DIP-authored geometry in Render()
    // then rasterizes 1:1 into the physical-pixel DIB (crisp at 150%/200%).
    // (The original ran SetDpi after BindDC; both are pure state setters, so
    // the shared Rebind's order is not observable.)
    renderer_.Rebind(phys_size_, phys_size_, dpi_, /*set_dpi=*/true);
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
    renderer_.ReleaseTarget(&dc_render_target_);
    if (d2d_factory_) { d2d_factory_->Release(); d2d_factory_ = nullptr; }

    renderer_.FreeBuffer();
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
    renderer_.Rebind(phys_size_, phys_size_, dpi_, /*set_dpi=*/false);

    dc_render_target_->BeginDraw();
    dc_render_target_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    const auto& theme = ui::GetActiveTheme();
    const float pill_radius = theme.badge_radius + 1.0f;

    // Outer rounded pill container (32x32 rounded icon pill)
    D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
        D2D1::RectF(1.0f, 1.0f, static_cast<float>(kSize) - 1.0f, static_cast<float>(kSize) - 1.0f),
        pill_radius, pill_radius
    );

    ID2D1SolidColorBrush* bgBrush = nullptr;
    ID2D1SolidColorBrush* borderBrush = nullptr;

    // DESIGN-261029 (session 261029_0001): brand navy family instead of the
    // generic slate pill, so the drag icon reads as the same product as the
    // tooltip card; gold border + new warm-white top rim light (the shared
    // "seal" motif).
    if (is_hovered_) {
        dc_render_target_->CreateSolidColorBrush(ui::ColorFromHex(theme.surface_hover, 0.98f), &bgBrush);
        dc_render_target_->CreateSolidColorBrush(ui::ColorFromHex(theme.border_hover, theme.border_hover_alpha), &borderBrush);
    } else {
        dc_render_target_->CreateSolidColorBrush(ui::ColorFromHex(theme.surface_canvas, theme.surface_canvas_alpha), &bgBrush);
        dc_render_target_->CreateSolidColorBrush(ui::ColorFromHex(theme.border_outer, theme.border_outer_alpha), &borderBrush);
    }

    if (bgBrush) {
        dc_render_target_->FillRoundedRectangle(pill, bgBrush);
    }
    if (borderBrush) {
        dc_render_target_->DrawRoundedRectangle(pill, borderBrush, is_hovered_ ? 1.8f : 1.2f);
    }
    // R2-12c (FIX-B2): the top rim light moved AFTER the gold border and down
    // to y = 2.4f. Drawn first at y = 1.5f it was half-covered by the 1.2 DIP
    // idle border and COMPLETELY erased by the 1.8 DIP hover border (which
    // spans y = 0.1..1.9), and its x = 7.0f start reached 2 DIP into the
    // r = 8.0f corner arc. At y = 2.4f (line body 1.9..2.9) it sits just
    // inside the border in both states, and x = 9.0f is past the arc
    // (x ~= 4.5 at that height).
    if (bgBrush) {
        const float inner_r = (std::max)(1.0f, pill_radius - 1.0f);
        const D2D1_ROUNDED_RECT inner_pill = D2D1::RoundedRect(
            D2D1::RectF(2.0f, 2.0f, static_cast<float>(kSize) - 2.0f, static_cast<float>(kSize) - 2.0f),
            inner_r, inner_r
        );
        bgBrush->SetColor(ui::ColorFromHex(theme.border_inner, theme.border_inner_alpha));
        dc_render_target_->DrawRoundedRectangle(inner_pill, bgBrush, 1.0f);




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

    // R6 Phase 3 (audit item 4, plan §3.1 A3): device-lost recovery - an
    // unrecreated target after D2DERR_RECREATE_TARGET would keep EndDraw
    // failing and leave the drag icon permanently invisible after a driver
    // reset (the next ShowAt repaints on the recreated target).
    const HRESULT hr = dc_render_target_->EndDraw();
    if (IsRecoverableDeviceLost(hr)) {
        RecreateAfterDeviceLost();
    }
}

// R6 Phase 3 (audit item 4): recreate the single-threaded DC render target
// after a device-lost. RebindRenderTarget re-binds the existing DIB + DPI on
// the fresh target; the logo bitmap was created on the lost device and must
// be rebuilt.
void DragIconWindow::RecreateAfterDeviceLost() {
    DIAG_F("DRAG_ICON/DeviceLost/001: D2DERR_RECREATE_TARGET; recreating render target\n");
    renderer_.ReleaseTarget(&dc_render_target_);
    if (!d2d_factory_) {
        return; // Create() never finished; all render paths null-guard already
    }
    if (!renderer_.CreateTarget(d2d_factory_, &dc_render_target_)) {
        DIAG_F("DRAG_ICON/DeviceLost/002: render-target recreation failed; icon stays stale until next Create()\n");
        return;
    }
    RebindRenderTarget();
    LoadLogoBitmap();
}

void DragIconWindow::UpdateLayered() {
    // REQ-R15: blit the PHYSICAL buffer size (matches the DIB and window);
    // renderer_ performs the GetWindowRect-based ptDst + GetDC(nullptr) ULW
    // sequence identical to the previous inline code.
    renderer_.Present(hwnd_, phys_size_, phys_size_, alpha_);
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

        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            ::BeginPaint(hwnd, &ps);
            ::EndPaint(hwnd, &ps);
            if (pThis->IsVisible()) {
                pThis->Render();
                pThis->UpdateLayered();
            }
            return 0;
        }

        case WM_DESTROY:
            return 0;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace emebalachat
