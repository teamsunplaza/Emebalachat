#include "badge.hpp"
#include "../i18n.hpp"

#include <cmath>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace emebalachat {

namespace {
const wchar_t kWindowClassName[] = L"Emebalachat_FloatingBadgeClass";
}

FloatingBadge::FloatingBadge() = default;

FloatingBadge::~FloatingBadge() {
    Destroy();
}

bool FloatingBadge::Create(HINSTANCE hInstance, std::wstring_view src_code, std::wstring_view tgt_code, int initial_x, int initial_y) {
    hInstance_ = hInstance;
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        src_code_ = src_code;
        tgt_code_ = tgt_code;
    }

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = FloatingBadge::WndProc;
    wc.hInstance = hInstance_;
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = kWindowClassName;
    ::RegisterClassExW(&wc);

    current_width_ = kDefaultWidth;
    int posX = 0;
    int posY = 0;

    if (initial_x >= 0 && initial_y >= 0) {
        posX = initial_x;
        posY = initial_y;
        // Multi-monitor aware bounds clamping: find the nearest active monitor
        POINT pt = { posX, posY };
        HMONITOR hMon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(MONITORINFO);
        if (::GetMonitorInfoW(hMon, &mi)) {
            const RECT& monWork = mi.rcWork;
            if (posX < monWork.left) posX = monWork.left;
            if (posY < monWork.top) posY = monWork.top;
            if (posX + current_width_ > monWork.right) posX = monWork.right - current_width_;
            if (posY + kHeight > monWork.bottom) posY = monWork.bottom - kHeight;
        }
    } else {
        RECT workArea = {};
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        // Initial default position: bottom right of primary monitor, above taskbar
        posX = workArea.right - current_width_ - 30;
        posY = workArea.bottom - kHeight - 30;
    }

    // Create layered, topmost, toolwindow (no taskbar button)
    hwnd_ = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kWindowClassName,
        L"Emebalachat Badge",
        WS_POPUP,
        posX, posY, current_width_, kHeight,
        nullptr, nullptr, hInstance_, this
    );

    if (!hwnd_) {
        return false;
    }

    // Initialize DIB section and memory DC for UpdateLayeredWindow
    ReallocateBuffer(current_width_, kHeight);

    // Initialize Direct2D & DirectWrite
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

    RECT rc = { 0, 0, current_width_, kHeight };
    dc_render_target_->BindDC(hMemDC_, &rc);

    if (FAILED(::DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&dwrite_factory_)
    ))) {
        return false;
    }

    // Prefer Segoe UI Variable Text (Windows 11 Fluent font), fallback to Segoe UI
    const wchar_t* fontName = L"Segoe UI Variable Text";
    IDWriteFontCollection* sysFonts = nullptr;
    if (SUCCEEDED(dwrite_factory_->GetSystemFontCollection(&sysFonts)) && sysFonts) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (FAILED(sysFonts->FindFamilyName(fontName, &index, &exists)) || !exists) {
            fontName = L"Segoe UI";
        }
        sysFonts->Release();
    }

    dwrite_factory_->CreateTextFormat(
        fontName,
        nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        12.5f,
        L"",
        &text_format_
    );

    if (text_format_) {
        text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // Initial render and show
    Render();
    ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    ResetIdleTimer();

    return true;
}

void FloatingBadge::Destroy() {
    if (hwnd_) {
        ::KillTimer(hwnd_, kTimerIdle);
        ::KillTimer(hwnd_, kTimerSingleClick);
        ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    if (text_format_) { text_format_->Release(); text_format_ = nullptr; }
    if (dwrite_factory_) { dwrite_factory_->Release(); dwrite_factory_ = nullptr; }
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

void FloatingBadge::SetStatus(BadgeStatus status) {
    if (!hwnd_) return;
    DWORD winThreadId = ::GetWindowThreadProcessId(hwnd_, nullptr);
    if (::GetCurrentThreadId() == winThreadId) {
        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            status_ = status;
            current_alpha_ = 217;
        }
        Render();
        ResetIdleTimer();
    } else {
        ::PostMessageW(hwnd_, WM_BADGE_SET_STATUS, static_cast<WPARAM>(status), 0);
    }
}

void FloatingBadge::SetLanguages(std::wstring_view src_code, std::wstring_view tgt_code) {
    if (!hwnd_) return;
    DWORD winThreadId = ::GetWindowThreadProcessId(hwnd_, nullptr);
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        src_code_ = src_code;
        tgt_code_ = tgt_code;
    }
    if (::GetCurrentThreadId() == winThreadId) {
        Render();
    } else {
        ::PostMessageW(hwnd_, WM_BADGE_SET_LANGS, 0, 0);
    }
}

void FloatingBadge::SetVisible(bool visible) {
    visible_ = visible;
    if (hwnd_) {
        ::ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (visible) {
            Render();
            ResetIdleTimer();
        }
    }
}

bool FloatingBadge::IsVisible() const {
    return visible_;
}

void FloatingBadge::SetPosition(int x, int y) {
    if (hwnd_) {
        ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, current_width_, kHeight, SWP_NOACTIVATE | SWP_NOSIZE);
    }
}

void FloatingBadge::SetClickCallback(ActionCallback cb) {
    click_callback_ = std::move(cb);
}

void FloatingBadge::SetDoubleClickCallback(ActionCallback cb) {
    double_click_callback_ = std::move(cb);
}

void FloatingBadge::SetRightClickCallback(ActionCallback cb) {
    right_click_callback_ = std::move(cb);
}

void FloatingBadge::SetPositionCallback(PositionCallback cb) {
    position_callback_ = std::move(cb);
}

void FloatingBadge::ResetIdleTimer() {
    if (hwnd_) {
        ::KillTimer(hwnd_, kTimerIdle);
        ::SetTimer(hwnd_, kTimerIdle, kIdleTimeoutMs, nullptr);
    }
}

void FloatingBadge::ReallocateBuffer(int width, int height) {
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
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    hBitmap_ = ::CreateDIBSection(hMemDC_, &bmi, DIB_RGB_COLORS, &pBits_, nullptr, 0);
    hOldBitmap_ = static_cast<HBITMAP>(::SelectObject(hMemDC_, hBitmap_));
    ::ReleaseDC(nullptr, hScreenDC);

    if (dc_render_target_) {
        RECT rc = { 0, 0, width, height };
        dc_render_target_->BindDC(hMemDC_, &rc);
    }
}

void FloatingBadge::UpdateAlpha(BYTE alpha) {
    current_alpha_ = alpha;
    if (!hwnd_ || !hMemDC_) return;

    POINT ptSrc = { 0, 0 };
    SIZE sz = { current_width_, kHeight };
    POINT ptDst = {};
    RECT rcWindow = {};
    ::GetWindowRect(hwnd_, &rcWindow);
    ptDst.x = rcWindow.left;
    ptDst.y = rcWindow.top;

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = current_alpha_;
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

void FloatingBadge::Render() {
    if (!hwnd_) return;
    std::lock_guard<std::mutex> render_lock(render_mutex_);

    BadgeStatus status;
    std::wstring src_code;
    std::wstring tgt_code;
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        status = status_;
        src_code = src_code_;
        tgt_code = tgt_code_;
    }

    // Status colors and typography accents matching Windows 11 Fluent / Dynamic Island theme
    D2D1_COLOR_F statusColor;
    D2D1_COLOR_F statusTextColor;
    std::wstring statusText;
    switch (status) {
        case BadgeStatus::Active:
            statusColor = D2D1::ColorF(0x10B981);       // Vibrant Emerald (#10B981)
            statusTextColor = D2D1::ColorF(0x34D399);   // Glowing Mint (#34D399)
            statusText = I18n::Get(StringId::BadgeActive);
            break;
        case BadgeStatus::Translating:
            statusColor = D2D1::ColorF(0xF59E0B);       // Warm Amber (#F59E0B)
            statusTextColor = D2D1::ColorF(0xFBBF24);   // Glowing Gold/Amber (#FBBF24)
            statusText = I18n::Get(StringId::BadgeTranslating);
            break;
        case BadgeStatus::Disabled:
        default:
            statusColor = D2D1::ColorF(0x9CA3AF);       // Slate (#9CA3AF)
            statusTextColor = D2D1::ColorF(0x9CA3AF);   // Slate (#9CA3AF)
            statusText = I18n::Get(StringId::BadgePaused);
            break;
    }

    std::wstring display_src = (src_code == L"AUTO" || src_code == L"Auto" || src_code == L"Auto Detect")
        ? I18n::Get(StringId::AutoDetect) : src_code;
    std::wstring lang_part = display_src + L" \u2192 " + tgt_code;
    std::wstring div_part = L"  |  ";
    std::wstring status_part = statusText;
    std::wstring display_label = lang_part + div_part + status_part;

    // Measure exact text dimensions using DirectWrite TextLayout
    IDWriteTextLayout* textLayout = nullptr;
    DWRITE_TEXT_METRICS metrics = {};
    if (dwrite_factory_ && text_format_) {
        dwrite_factory_->CreateTextLayout(
            display_label.c_str(),
            static_cast<UINT32>(display_label.size()),
            text_format_,
            2000.0f,
            static_cast<float>(kHeight),
            &textLayout
        );
        if (textLayout) {
            textLayout->GetMetrics(&metrics);
        }
    }

    // Mathematical symmetrical centering calculation:
    // Outer ambient glow halo radius: 6.0f (diameter: 12.0f)
    // Inner vibrant status dot radius: 3.5f (diameter: 7.0f)
    float haloRadius = 6.0f;
    float haloDiameter = haloRadius * 2.0f;
    float dotRadius = 3.5f;
    float gap = 8.5f;
    float textWidth = metrics.widthIncludingTrailingWhitespace;
    float totalContentWidth = haloDiameter + gap + textWidth;
    float sidePadding = 20.0f; // pill curvature breathing room on each side

    int needed_width = static_cast<int>(std::ceil(totalContentWidth + (sidePadding * 2.0f)));
    int dynamic_width = (std::max)(180, needed_width);

    // If width changed or buffer unallocated, reallocate buffer and resize window
    if (dynamic_width != current_width_ || !hBitmap_) {
        current_width_ = dynamic_width;
        ReallocateBuffer(current_width_, kHeight);
        if (hwnd_) {
            ::SetWindowPos(hwnd_, nullptr, 0, 0, current_width_, kHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    if (!dc_render_target_) {
        if (textLayout) textLayout->Release();
        return;
    }

    // Exact horizontal and vertical dead-center coordinates
    // Mathematical symmetry: Left margin to halo == Right margin from text end to pill edge
    float startX = (static_cast<float>(current_width_) - totalContentWidth) / 2.0f;
    float dotCenterX = startX + haloRadius;
    float dotCenterY = static_cast<float>(kHeight) / 2.0f;

    float textX = startX + haloDiameter + gap;

    // ROOT CAUSE FIX:
    // text_format_ has SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER).
    // When textLayout is created with maxHeight = kHeight (38.0f), DirectWrite
    // automatically centers the text line vertically inside [0, kHeight].
    // Passing any non-zero Y to DrawTextLayout causes double-centering (pushing
    // the text downward). Drawing at Y = 0.0f guarantees 100% dead-center alignment!
    float textY = 0.0f;

    RECT rc = { 0, 0, current_width_, kHeight };
    dc_render_target_->BindDC(hMemDC_, &rc);
    dc_render_target_->BeginDraw();
    dc_render_target_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    // 1. Pill background: deep obsidian glass with subtle top-to-bottom illumination
    ID2D1GradientStopCollection* pBgStops = nullptr;
    D2D1_GRADIENT_STOP bgStops[2];
    bgStops[0].position = 0.0f;
    bgStops[0].color = D2D1::ColorF(0x181A20, 0.95f); // Subtle top specular illumination
    bgStops[1].position = 1.0f;
    bgStops[1].color = D2D1::ColorF(0x0E1014, 0.96f); // Deep obsidian base

    ID2D1LinearGradientBrush* bgGradient = nullptr;
    if (SUCCEEDED(dc_render_target_->CreateGradientStopCollection(bgStops, 2, &pBgStops)) && pBgStops) {
        dc_render_target_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(0.0f, 0.0f),
                D2D1::Point2F(0.0f, static_cast<float>(kHeight))
            ),
            pBgStops,
            &bgGradient
        );
        pBgStops->Release();
    }

    D2D1_ROUNDED_RECT fillRect = D2D1::RoundedRect(
        D2D1::RectF(0.0f, 0.0f, static_cast<float>(current_width_), static_cast<float>(kHeight)),
        kRadius,
        kRadius
    );

    if (bgGradient) {
        dc_render_target_->FillRoundedRectangle(&fillRect, bgGradient);
        bgGradient->Release();
    } else {
        ID2D1SolidColorBrush* solidBg = nullptr;
        dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x121418, 0.95f), &solidBg);
        if (solidBg) {
            dc_render_target_->FillRoundedRectangle(&fillRect, solidBg);
            solidBg->Release();
        }
    }

    // 2. Refined 1px dual-tone Fluent border (soft top rim light + status accent tint)
    ID2D1GradientStopCollection* pBorderStops = nullptr;
    D2D1_GRADIENT_STOP borderStops[2];
    borderStops[0].position = 0.0f;
    borderStops[0].color = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f); // Top rim light
    borderStops[1].position = 1.0f;
    borderStops[1].color = D2D1::ColorF(statusColor.r, statusColor.g, statusColor.b, 0.38f); // Status tint

    ID2D1LinearGradientBrush* borderGradient = nullptr;
    if (SUCCEEDED(dc_render_target_->CreateGradientStopCollection(borderStops, 2, &pBorderStops)) && pBorderStops) {
        dc_render_target_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(0.0f, 0.0f),
                D2D1::Point2F(0.0f, static_cast<float>(kHeight))
            ),
            pBorderStops,
            &borderGradient
        );
        pBorderStops->Release();
    }

    D2D1_ROUNDED_RECT strokeRect = D2D1::RoundedRect(
        D2D1::RectF(0.5f, 0.5f, static_cast<float>(current_width_) - 0.5f, static_cast<float>(kHeight) - 0.5f),
        kRadius - 0.5f,
        kRadius - 0.5f
    );

    if (borderGradient) {
        dc_render_target_->DrawRoundedRectangle(&strokeRect, borderGradient, 1.0f);
        borderGradient->Release();
    } else {
        ID2D1SolidColorBrush* solidBorder = nullptr;
        dc_render_target_->CreateSolidColorBrush(
            D2D1::ColorF(statusColor.r, statusColor.g, statusColor.b, 0.35f),
            &solidBorder
        );
        if (solidBorder) {
            dc_render_target_->DrawRoundedRectangle(&strokeRect, solidBorder, 1.0f);
            solidBorder->Release();
        }
    }

    // 3. Glowing status indicator (ambient halo + vibrant core)
    // Ambient glow halo (22% opacity)
    ID2D1SolidColorBrush* haloBrush = nullptr;
    float haloAlpha = (status_ == BadgeStatus::Disabled) ? 0.15f : 0.24f;
    dc_render_target_->CreateSolidColorBrush(
        D2D1::ColorF(statusColor.r, statusColor.g, statusColor.b, haloAlpha),
        &haloBrush
    );
    if (haloBrush) {
        D2D1_ELLIPSE halo = D2D1::Ellipse(D2D1::Point2F(dotCenterX, dotCenterY), haloRadius, haloRadius);
        dc_render_target_->FillEllipse(&halo, haloBrush);
        haloBrush->Release();
    }

    // Vibrant core dot (100% opacity)
    ID2D1SolidColorBrush* coreBrush = nullptr;
    dc_render_target_->CreateSolidColorBrush(statusColor, &coreBrush);
    if (coreBrush) {
        D2D1_ELLIPSE core = D2D1::Ellipse(D2D1::Point2F(dotCenterX, dotCenterY), dotRadius, dotRadius);
        dc_render_target_->FillEllipse(&core, coreBrush);
        coreBrush->Release();
    }

    // 4. Label text (multi-tone typography & mathematically centered)
    ID2D1SolidColorBrush* langBrush = nullptr;
    ID2D1SolidColorBrush* divBrush = nullptr;
    ID2D1SolidColorBrush* statusTextBrush = nullptr;

    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0xF9FAFB, 1.0f), &langBrush);
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x6B7280, 0.70f), &divBrush);
    dc_render_target_->CreateSolidColorBrush(statusTextColor, &statusTextBrush);

    if (textLayout) {
        if (langBrush) {
            DWRITE_TEXT_RANGE langRange = { 0, static_cast<UINT32>(lang_part.length()) };
            textLayout->SetDrawingEffect(langBrush, langRange);
            textLayout->SetFontWeight(DWRITE_FONT_WEIGHT_MEDIUM, langRange);
        }
        if (divBrush) {
            DWRITE_TEXT_RANGE divRange = {
                static_cast<UINT32>(lang_part.length()),
                static_cast<UINT32>(div_part.length())
            };
            textLayout->SetDrawingEffect(divBrush, divRange);
            textLayout->SetFontWeight(DWRITE_FONT_WEIGHT_REGULAR, divRange);
        }
        if (statusTextBrush) {
            DWRITE_TEXT_RANGE statusRange = {
                static_cast<UINT32>(lang_part.length() + div_part.length()),
                static_cast<UINT32>(status_part.length())
            };
            textLayout->SetDrawingEffect(statusTextBrush, statusRange);
            textLayout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, statusRange);
        }

        dc_render_target_->DrawTextLayout(
            D2D1::Point2F(textX, textY),
            textLayout,
            langBrush ? langBrush : statusTextBrush
        );
    }

    if (langBrush) langBrush->Release();
    if (divBrush) divBrush->Release();
    if (statusTextBrush) statusTextBrush->Release();
    if (textLayout) textLayout->Release();

    dc_render_target_->EndDraw();

    // Commit pixels to layered window
    UpdateAlpha(current_alpha_);
}

LRESULT CALLBACK FloatingBadge::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    FloatingBadge* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<FloatingBadge*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<FloatingBadge*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self) {
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
        case WM_LBUTTONDOWN: {
            self->is_mouse_down_ = true;
            self->is_dragging_ = false;
            ::GetCursorPos(&self->drag_start_cursor_);
            RECT rc = {};
            ::GetWindowRect(hwnd, &rc);
            self->drag_start_window_ = { rc.left, rc.top };
            ::SetCapture(hwnd);

            self->UpdateAlpha(217);
            self->ResetIdleTimer();
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (self->is_mouse_down_) {
                POINT ptNow = {};
                ::GetCursorPos(&ptNow);
                int dx = ptNow.x - self->drag_start_cursor_.x;
                int dy = ptNow.y - self->drag_start_cursor_.y;

                if (!self->is_dragging_) {
                    int dragX = ::GetSystemMetrics(SM_CXDRAG);
                    int dragY = ::GetSystemMetrics(SM_CYDRAG);
                    if (abs(dx) >= dragX || abs(dy) >= dragY) {
                        self->is_dragging_ = true;
                        ::KillTimer(hwnd, kTimerSingleClick);
                    }
                }

                if (self->is_dragging_) {
                    int newX = self->drag_start_window_.x + dx;
                    int newY = self->drag_start_window_.y + dy;

                    // Multi-monitor aware bounds clamping: find nearest monitor to badge center
                    POINT ptCenter = { newX + self->current_width_ / 2, newY + kHeight / 2 };
                    HMONITOR hMon = ::MonitorFromPoint(ptCenter, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO mi = {};
                    mi.cbSize = sizeof(MONITORINFO);
                    if (::GetMonitorInfoW(hMon, &mi)) {
                        const RECT& monWork = mi.rcWork;
                        if (newX < monWork.left) newX = monWork.left;
                        if (newY < monWork.top) newY = monWork.top;
                        if (newX + self->current_width_ > monWork.right) newX = monWork.right - self->current_width_;
                        if (newY + kHeight > monWork.bottom) newY = monWork.bottom - kHeight;
                    }

                    ::SetWindowPos(hwnd, HWND_TOPMOST, newX, newY, self->current_width_, kHeight, SWP_NOSIZE | SWP_NOACTIVATE);
                }
            } else {
                if (!self->is_hovered_) {
                    self->is_hovered_ = true;
                    self->UpdateAlpha(217); // Restore full opacity on hover

                    TRACKMOUSEEVENT tme = {};
                    tme.cbSize = sizeof(TRACKMOUSEEVENT);
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = hwnd;
                    ::TrackMouseEvent(&tme);
                }
                self->ResetIdleTimer();
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (::GetCapture() == hwnd) {
                ::ReleaseCapture();
            }

            if (self->is_dragging_) {
                self->is_dragging_ = false;
                self->is_mouse_down_ = false;
                if (self->position_callback_) {
                    RECT rcWnd = {};
                    ::GetWindowRect(hwnd, &rcWnd);
                    self->position_callback_(rcWnd.left, rcWnd.top);
                }
            } else if (self->is_mouse_down_) {
                self->is_mouse_down_ = false;
                // Click in place: start short single-click timer to allow double-click detection
                ::SetTimer(hwnd, kTimerSingleClick, kSingleClickDelayMs, nullptr);
            }
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            if (::GetCapture() == hwnd) {
                ::ReleaseCapture();
            }
            self->is_mouse_down_ = false;
            self->is_dragging_ = false;
            ::KillTimer(hwnd, kTimerSingleClick); // Cancel pending single-click toggle!

            if (self->double_click_callback_) {
                self->double_click_callback_();
            }
            return 0;
        }

        case WM_RBUTTONUP: {
            if (self->right_click_callback_) {
                self->right_click_callback_();
            }
            return 0;
        }

        case WM_BADGE_SET_STATUS: {
            {
                std::lock_guard<std::mutex> lock(self->data_mutex_);
                self->status_ = static_cast<BadgeStatus>(wParam);
                self->current_alpha_ = 217;
            }
            self->Render();
            self->ResetIdleTimer();
            return 0;
        }

        case WM_BADGE_SET_LANGS: {
            self->Render();
            return 0;
        }

        case WM_MOUSELEAVE: {
            self->is_hovered_ = false;
            self->ResetIdleTimer();
            return 0;
        }

        case WM_TIMER: {
            if (wParam == kTimerSingleClick) {
                ::KillTimer(hwnd, kTimerSingleClick);
                if (self->click_callback_) {
                    self->click_callback_();
                }
            } else if (wParam == kTimerIdle) {
                // If idle for 5 seconds, not hovered, and not mouse-down, fade to 20% opacity
                if (!self->is_hovered_ && !self->is_mouse_down_ && self->current_alpha_ > 51) {
                    self->UpdateAlpha(51);
                }
            }
            return 0;
        }

        case WM_NCDESTROY: {
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        case WM_DESTROY: {
            return 0;
        }

        default:
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace emebalachat
