#include "badge.hpp"
#include "diag_logger.hpp"
#include "asset_loader.hpp"
#include "../i18n.hpp"
#include "../unicode_utils.hpp"
#include "dpi.hpp"

#include <cmath>
#include <vector>

#include <dwrite_1.h>  // v0.10.4 design sweep: IDWriteTextLayout1 tracking

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
    // REQ-052 P3: capture the single-click delay once at init so the pause
    // toggle always outlives the OS double-click window (GetDoubleClickTime,
    // default 500ms) instead of the old fixed 220ms that raced ahead of a
    // slow double-click.
    single_click_delay_ms_ = ::GetDoubleClickTime();
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

    // REQ-R15 (audit §5 latent item 3): the badge is sized in DIPs by Render()
    // (DirectWrite metrics are DPI-independent), but the window, the DIB and
    // UpdateLayeredWindow must live in PHYSICAL pixels of the monitor it sits
    // on. The persisted position and the LL-hook coordinates are physical, so
    // all comparisons below convert the DIP extents via the target DPI.
    if (initial_x >= 0 && initial_y >= 0) {
        posX = initial_x;
        posY = initial_y;
        // Multi-monitor aware bounds clamping: find the nearest active monitor
        POINT pt = { posX, posY };
        dpi_ = emebalachat::ui::MonitorDpiAtPoint(pt);
        HMONITOR hMon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(MONITORINFO);
        const int pw = PhysW();
        const int ph = PhysH();
        if (::GetMonitorInfoW(hMon, &mi)) {
            const POINT clamped = emebalachat::ui::ClampWindowOrigin(posX, posY, pw, ph, 0, mi.rcWork);
            posX = clamped.x;
            posY = clamped.y;
        }
    } else {
        RECT workArea = {};
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        dpi_ = emebalachat::ui::MonitorDpiAtPoint(POINT{ workArea.left + 1, workArea.top + 1 });
        // Initial default position: bottom right of primary monitor, above taskbar
        posX = workArea.right - PhysW() - 30;
        posY = workArea.bottom - PhysH() - 30;
    }

    // Create layered, topmost, toolwindow (no taskbar button)
    hwnd_ = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kWindowClassName,
        L"Emebala Chat Badge",
        WS_POPUP,
        posX, posY, PhysW(), PhysH(),
        nullptr, nullptr, hInstance_, this
    );

    if (!hwnd_) {
        return false;
    }

    // Initialize Direct2D & DirectWrite (REQ-R15: the DIB + render-target
    // binding happens once, after the target exists, below - ReallocateBuffer
    // both allocates the physical-pixel buffer and binds + DPI-sets the target)
    if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory_))) {
        return false;
    }

    // REF-3.6: target creation + lifetime owned by renderer_; dc_render_target_
    // stays a non-owning alias so the render code below is untouched.
    if (!renderer_.CreateTarget(d2d_factory_, &dc_render_target_)) {
        return false;
    }

    // REQ-R15: render-target DPI maps the DIP-authored layout onto the
    // physical-pixel buffer (crisp text at 125%/150%/200%); ReallocateBuffer
    // above ran before the target existed, so rebind + set DPI now.
    ReallocateBuffer(PhysW(), PhysH());

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

    LoadLogoBitmap();

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
        ::KillTimer(hwnd_, kTimerDisplayChange);
        ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    if (logo_bitmap_) { logo_bitmap_->Release(); logo_bitmap_ = nullptr; }
    if (text_format_) { text_format_->Release(); text_format_ = nullptr; }
    if (dwrite_factory_) { dwrite_factory_->Release(); dwrite_factory_ = nullptr; }
    renderer_.ReleaseTarget(&dc_render_target_);
    if (d2d_factory_) { d2d_factory_->Release(); d2d_factory_ = nullptr; }

    renderer_.FreeBuffer();
}

void FloatingBadge::SetStatus(BadgeStatus status) {
    if (!hwnd_) return;
    // 260905 diagnostics: badge pill state transitions (recorded BEFORE the
    // marshal decision so the log shows both the request and its routing).
    DIAG_LOG("UI", "badge_status -> %s",
             status == BadgeStatus::Active      ? "Active"
             : status == BadgeStatus::Disabled  ? "Disabled"
             : status == BadgeStatus::Translating ? "Translating"
                                                 : "Unknown");
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
    // 260905 diagnostics: language label change (old→new visible across
    // successive lines; wstrings here are short ASCII-ish codes).
    DIAG_LOG("UI", "badge_langs \"%s\" -> \"%s\"", ToUtf8(src_code).c_str(), ToUtf8(tgt_code).c_str());
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
            // 261029_0001 U-I (R1-21): re-showing is the user's recovery gesture
            // after "the badge disappeared" (tray double-click / tray toggle),
            // and the env-robustness report notes that this gesture used to show
            // the pill wherever it was left. If the topology changed while the
            // badge was hidden - or the OS broadcast never reached this window -
            // pull it back onto a live monitor BEFORE drawing, so the recovery
            // gesture cannot present an off-screen pill. No-op when the current
            // position is already inside a monitor (the common case).
            ReclampToVisibleMonitors();
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
        ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, PhysW(), PhysH(), SWP_NOACTIVATE | SWP_NOSIZE);
    }
}

// 261029_0001 U-I (R1-21, env research F4): undocking a laptop or unplugging
// the display that hosted the badge left this layered window at absolute
// coordinates on a monitor that no longer exists, so the pill vanished until
// the app restarted - Create() and the drag path were the only clamps. This
// re-runs that exact policy against the CURRENT monitor union: pick the nearest
// live monitor from the enumerated work areas, refresh that monitor's DPI
// (physical extents depend on it), then ClampWindowOrigin - one clamp
// implementation shared with Create/drag, no second copy of the edge math.
//
// The persisted badge_x/badge_y are deliberately NOT rewritten: they are the
// user's intent (restored on the next launch, when the display may be back),
// and main.cpp's position callback is the only writer that persists - drag is
// the only gesture the design ever re-persisted.
void FloatingBadge::ReclampToVisibleMonitors() {
    if (!hwnd_) {
        return;
    }
    if (is_dragging_) {
        // The drag path owns the position and re-clamps per WM_MOUSEMOVE
        // against the monitor under the cursor; moving the window underneath
        // the pointer mid-gesture would fight it.
        return;
    }

    size_t primary_index = emebalachat::ui::kNoWorkArea;
    const std::vector<RECT> work_areas = emebalachat::ui::MonitorWorkAreas(&primary_index);
    if (work_areas.empty()) {
        // Headless / RDP console session: no monitor exists to clamp into.
        // Skipping is the documented degradation - inventing an origin would be
        // worse than leaving the window where the OS put it.
        DIAG_F("BADGE/Reclamp/003: no monitor enumerated; display-change re-clamp skipped\n");
        return;
    }

    RECT rc = {};
    if (!::GetWindowRect(hwnd_, &rc)) {
        return;
    }
    const int old_x = rc.left;
    const int old_y = rc.top;

    const size_t index = emebalachat::ui::NearestWorkAreaIndex(
        old_x, old_y, work_areas.data(), work_areas.size(), primary_index);
    if (index == emebalachat::ui::kNoWorkArea) {
        return;
    }
    const RECT& work = work_areas[index];

    // Refresh the DPI BEFORE measuring PhysW/PhysH: an undocked 150% laptop
    // panel and a 100% external monitor give different physical extents, and
    // clamping with the stale scale would leave the pill over the new edge.
    // The 1x1 clamp turns the chosen work rect into a point inside itself, so
    // MonitorDpiAtPoint cannot fall back to a neighbouring display.
    const POINT probe = emebalachat::ui::ClampWindowOrigin(old_x, old_y, 1, 1, 0, work);
    const UINT target_dpi = emebalachat::ui::MonitorDpiAtPoint(probe);

    bool resized = false;
    if (target_dpi != dpi_) {
        dpi_ = target_dpi;
        ReallocateBuffer(PhysW(), PhysH());
        resized = true;
    }

    const POINT clamped = emebalachat::ui::ClampWindowOrigin(
        old_x, old_y, PhysW(), PhysH(), 0, work);

    if (clamped.x == old_x && clamped.y == old_y && !resized) {
        return; // already on a live monitor at the right scale: no jitter
    }

    DIAG_F("BADGE/Reclamp/001: display change; badge %d,%d -> %d,%d (monitor %llu/%llu, dpi %u)\n",
           old_x, old_y, clamped.x, clamped.y,
           static_cast<unsigned long long>(index),
           static_cast<unsigned long long>(work_areas.size()), target_dpi);

    // Size is only re-applied when the DPI change reallocated the DIB at a new
    // physical extent; SWP_NOSIZE otherwise (same contract as the drag path).
    ::SetWindowPos(hwnd_, nullptr, clamped.x, clamped.y, PhysW(), PhysH(),
                   SWP_NOZORDER | SWP_NOACTIVATE | (resized ? 0 : SWP_NOSIZE));
    if (resized) {
        // ReallocateBuffer destroyed the DIB, so its contents must be redrawn;
        // Render() terminates in UpdateAlpha(), i.e. the re-blit happens inside
        // this call (mirrors the WM_DPICHANGED house pattern below).
        Render();
    }
}

// REQ-R15: physical window size helpers. current_width_/kHeight stay DIP.
int FloatingBadge::PhysW() const {
    return emebalachat::ui::ScaleDipsToPixels(current_width_, dpi_);
}

int FloatingBadge::PhysH() const {
    return emebalachat::ui::ScaleDipsToPixels(kHeight, dpi_);
}

void FloatingBadge::RebindRenderTarget() {
    renderer_.Rebind(PhysW(), PhysH(), dpi_, /*set_dpi=*/false);
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

// REF-3.6: DIB (re)creation + SetDpi/BindDC tail owned by the shared
// renderer (REQ-R15: DPI tracks the monitor on every reallocation - initial
// create and cross-DPI drags both land here).
void FloatingBadge::ReallocateBuffer(int width, int height) {
    renderer_.ReallocateBuffer(width, height, dpi_);
}

void FloatingBadge::LoadLogoBitmap() {
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

void FloatingBadge::UpdateAlpha(BYTE alpha) {
    current_alpha_ = alpha;
    // REQ-R15: blit at the physical buffer size; renderer_ performs the
    // GetWindowRect-based ptDst + GetDC(nullptr) ULW sequence identical to the
    // previous inline code.
    renderer_.Present(hwnd_, PhysW(), PhysH(), current_alpha_);
}

// 260922_0001 A4: reentry_allowed caps the device-lost recovery re-render at
// one nested pass (top-level true -> recovery pass false). render_mutex_ is a
// recursive mutex so the nested lock on the same GUI thread is safe.
void FloatingBadge::Render(bool reentry_allowed) {
    if (!hwnd_) return;
    std::lock_guard<std::recursive_mutex> render_lock(render_mutex_);

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
            // v0.10.4 design sweep (session 261029_0001): +0.3 DIP character
            // tracking across the whole label — the badge is the brand's most
            // visible surface and the extra air reads premium rather than
            // utilitarian. IDWriteTextLayout1 is Win8+; the QI simply fails on
            // anything older and the label renders untracked (safe no-op).
            // Metrics are read AFTER the spacing is applied, so the
            // centering math below and the dynamic pill width stay exact.
            IDWriteTextLayout1* layout1 = nullptr;
            if (SUCCEEDED(textLayout->QueryInterface(
                    __uuidof(IDWriteTextLayout1),
                    reinterpret_cast<void**>(&layout1))) && layout1) {
                layout1->SetCharacterSpacing(
                    0.3f, 0.3f, 0.0f,
                    DWRITE_TEXT_RANGE{0, static_cast<UINT32>(display_label.size())});
                layout1->Release();
            }
            textLayout->GetMetrics(&metrics);
        }
    }

    // Mathematical symmetrical centering calculation:
    // 18x18 mini circular medallion with status glow ring
    float medallionSize = 18.0f;
    float medallionRadius = medallionSize * 0.5f; // 9.0f
    float gap = 8.5f;
    // R2-12b (FIX-B2): the gold seal ring is drawn OUTSIDE medallionSize. The
    // layout used to size the medallion at 18 DIP while the ring's outer edge
    // reached 22.4 DIP (r=13.0 + 0.4 stroke), so it ate 4.4 DIP of the gap
    // before the label (8.5 -> 4.1 DIP) and shifted the group 2.2 DIP left.
    // kRingPad is the ring's reserved half-width: the ring now sits at
    // medallionRadius + 2.5f (= 11.5, outer edge 11.95) inside the 12.0 pad.
    const float kRingPad = 3.0f;
    const float medallionVisualW = medallionSize + kRingPad * 2.0f; // 24.0f
    float textWidth = metrics.widthIncludingTrailingWhitespace;
    float totalContentWidth = medallionVisualW + gap + textWidth;
    float sidePadding = 18.0f; // pill curvature breathing room on each side

    int needed_width = static_cast<int>(std::ceil(totalContentWidth + (sidePadding * 2.0f)));
    int dynamic_width = (std::max)(180, needed_width);

    // If width changed or buffer unallocated, reallocate buffer and resize window
    if (dynamic_width != current_width_ || !renderer_.has_bitmap()) {
        current_width_ = dynamic_width;
        // REQ-R15: DIP layout width -> physical buffer & window width.
        ReallocateBuffer(PhysW(), PhysH());
        if (hwnd_) {
            ::SetWindowPos(hwnd_, nullptr, 0, 0, PhysW(), PhysH(), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    if (!dc_render_target_) {
        if (textLayout) textLayout->Release();
        return;
    }

    // Exact horizontal and vertical dead-center coordinates
    // Mathematical symmetry: Left margin to medallion == Right margin from text end to pill edge
    // R2-12b (FIX-B2): the VISUAL medallion (ring included) is the layout box,
    // so the ring's outer edge and the label's start are what get balanced.
    float startX = (static_cast<float>(current_width_) - totalContentWidth) / 2.0f;
    float dotCenterX = startX + medallionVisualW * 0.5f;
    float dotCenterY = static_cast<float>(kHeight) / 2.0f;

    float textX = startX + medallionVisualW + gap;

    // REQ-R15 note: everything above is authored in DIPs; the render target
    // DPI converts to physical at rasterization. The DirectWrite layout was
    // measured against the same DIP coordinate space, so centering math is
    // scale-correct on any monitor.

    // ROOT CAUSE FIX:
    // text_format_ has SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER).
    // When textLayout is created with maxHeight = kHeight (38.0f), DirectWrite
    // automatically centers the text line vertically inside [0, kHeight].
    // Passing any non-zero Y to DrawTextLayout causes double-centering (pushing
    // the text downward). Drawing at Y = 0.0f guarantees 100% dead-center alignment!
    float textY = 0.0f;

    RebindRenderTarget();
    dc_render_target_->BeginDraw();
    dc_render_target_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    // 1. Pill background: Emebala navy (brand palette, session 261029_0001) —
    //    a subtle top-to-bottom illumination over the poster navy, matching
    //    the tooltip card / About window family instead of the old generic
    //    obsidian AI-pill gradient.
    ID2D1GradientStopCollection* pBgStops = nullptr;
    D2D1_GRADIENT_STOP bgStops[2];
    bgStops[0].position = 0.0f;
    bgStops[0].color = D2D1::ColorF(0x101F3E, 0.95f); // navy-1 top illumination
    bgStops[1].position = 1.0f;
    bgStops[1].color = D2D1::ColorF(0x0C1830, 0.97f); // navy-0 poster base

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
        dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x0C1830, 0.96f), &solidBg);
        if (solidBg) {
            dc_render_target_->FillRoundedRectangle(&fillRect, solidBg);
            solidBg->Release();
        }
    }

    // 2. Hairline border: 1px lapis + warm-white top rim light (the "seal"
    //    motif shared with the tooltip card and the About window; replaces
    //    the old white→status gradient rim that read as a generic glow pill).
    //    R2-12e (FIX-B2): the hairline was #33507E @0.85, which measures only
    //    1.96:1 against a dark-navy IDE backdrop (#0B1324) and let the pill's
    //    silhouette dissolve. #4A6CA3 (lapis-hi) @0.90 keeps the same brand
    //    hue while the edge stays readable over dark apps.
    ID2D1SolidColorBrush* borderBrush = nullptr;
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x4A6CA3, 0.90f), &borderBrush);

    D2D1_ROUNDED_RECT strokeRect = D2D1::RoundedRect(
        D2D1::RectF(0.5f, 0.5f, static_cast<float>(current_width_) - 0.5f, static_cast<float>(kHeight) - 0.5f),
        kRadius - 0.5f,
        kRadius - 0.5f
    );

    if (borderBrush) {
        dc_render_target_->DrawRoundedRectangle(&strokeRect, borderBrush, 1.0f);
        borderBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f));
        // R2-12a (FIX-B2): the rim light must stay inside the capsule's flat
        // top tangent segment. At y = 1.5f the border curve sits at
        // x = kRadius - sqrt((kRadius-0.5)^2 - (kRadius-1.5)^2) ~= 13.0 DIP,
        // so the old 0.6 x kRadius start (11.4) poked ~1.6 DIP of white line
        // outside the rounded corners (the "whisker" visible at 144 DPI).
        dc_render_target_->DrawLine(
            D2D1::Point2F(kRadius, 1.5f),
            D2D1::Point2F(static_cast<float>(current_width_) - kRadius, 1.5f),
            borderBrush, 1.0f);
        borderBrush->Release();
    }

    // 3. Logo medallion (18x18) — "signet" treatment (session 261029_0001):
    //    the status disc stays saturated for glanceability, ringed by its own
    //    crisp status ring and a thin antique-gold outer ring echoing the About
    //    window's twin-gold seal.
    //    R2-12b (FIX-B2): the intermediate status-tinted outerRing that used to
    //    sit at r + 2.4f is GONE. With the gold ring near it the two warm rings
    //    read as one fuzzy "onion ring" in the Translating state (amber #F59E0B
    //    @0.38 and gold #D4AF37 were only 0.7 px apart), so the medallion is now
    //    a clean 2-ring signet: saturated status disc ring + gold seal ring.
    D2D1_ELLIPSE medalCircle = D2D1::Ellipse(D2D1::Point2F(dotCenterX, dotCenterY), medallionRadius, medallionRadius);

    // Medallion base (REQ-024: circular background tinted with the status
    // color — Active=emerald, Translating=amber, Disabled=slate — instead of
    // the previous fixed dark fill. statusColor is already computed by the
    // switch above; halo/ring/text/alpha logic untouched, same 0.95f alpha.)
    ID2D1SolidColorBrush* medalBgBrush = nullptr;
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(statusColor.r, statusColor.g, statusColor.b, 0.95f), &medalBgBrush);
    if (medalBgBrush) {
        dc_render_target_->FillEllipse(&medalCircle, medalBgBrush);
        medalBgBrush->Release();
    }

    // Medallion icon / logo content (centered 13x13 within the 18x18 medallion)
    if (logo_bitmap_) {
        D2D1_RECT_F logoRect = D2D1::RectF(dotCenterX - 6.5f, dotCenterY - 6.5f, dotCenterX + 6.5f, dotCenterY + 6.5f);
        dc_render_target_->DrawBitmap(
            logo_bitmap_,
            logoRect,
            1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
        );
    } else {
        // Fallback: core status dot if no bitmap is loaded
        ID2D1SolidColorBrush* coreBrush = nullptr;
        dc_render_target_->CreateSolidColorBrush(statusColor, &coreBrush);
        if (coreBrush) {
            D2D1_ELLIPSE core = D2D1::Ellipse(D2D1::Point2F(dotCenterX, dotCenterY), 4.0f, 4.0f);
            dc_render_target_->FillEllipse(&core, coreBrush);
            coreBrush->Release();
        }
    }

    // Status ring stroke around the medallion
    ID2D1SolidColorBrush* statusRingBrush = nullptr;
    dc_render_target_->CreateSolidColorBrush(statusColor, &statusRingBrush);
    if (statusRingBrush) {
        dc_render_target_->DrawEllipse(&medalCircle, statusRingBrush, 1.2f);
        statusRingBrush->Release();
    }

    // Gold signet ring — the outermost, thinnest ring (About-window seal cue).
    // R2-12b (FIX-B2): r = medallionRadius + 2.5f with a 0.9 stroke puts the
    // ring's outer edge at 11.95 DIP, exactly inside the kRingPad (12.0) the
    // layout above reserved; 0.60 alpha keeps it a whisper over the navy.
    {
        ID2D1SolidColorBrush* goldRingBrush = nullptr;
        dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0xD4AF37, 0.60f), &goldRingBrush);
        if (goldRingBrush) {
            D2D1_ELLIPSE goldRing = D2D1::Ellipse(D2D1::Point2F(dotCenterX, dotCenterY),
                                                  medallionRadius + 2.5f, medallionRadius + 2.5f);
            dc_render_target_->DrawEllipse(&goldRing, goldRingBrush, 0.9f);
            goldRingBrush->Release();
        }
    }

    // 4. Label text (multi-tone typography & mathematically centered;
    //    brand sand-white + lapis-gray divider per the 261029_0001 palette)
    ID2D1SolidColorBrush* langBrush = nullptr;
    ID2D1SolidColorBrush* divBrush = nullptr;
    ID2D1SolidColorBrush* statusTextBrush = nullptr;

    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0xF2ECDC, 1.0f), &langBrush);
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x93A3C7, 0.75f), &divBrush);
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

    // R6 Phase 3 (audit item 4, plan §3.1 A3): device-lost recovery. A driver
    // reset fails EndDraw with D2DERR_RECREATE_TARGET; without recreation the
    // badge stays a permanently blank pill.
    // 260922_0001 A4 (fix plan §4.1-A): the previous pass committed the blank
    // DIB via UpdateAlpha immediately after recreation, so the badge showed an
    // empty pill until the next unrelated state change. Now a successful
    // recovery re-renders once (reentry_allowed=false caps the recursion at
    // depth 2) and returns through that pass's own UpdateAlpha commit; a
    // failed recovery (or the already-recovering pass) SKIPS the commit so no
    // stale/blank bitmap ever reaches the layered window.
    const HRESULT hr = dc_render_target_->EndDraw();
    if (IsRecoverableDeviceLost(hr)) {
        if (reentry_allowed && RecreateAfterDeviceLost()) {
            Render(false);
            return;
        }
        return; // commit skipped: nothing valid to blit this pass
    }

    // Commit pixels to layered window
    UpdateAlpha(current_alpha_);
}

// R6 Phase 3 (audit item 4): recreate the single-threaded DC render target
// after a device-lost. ReallocateBuffer re-binds SetDpi + BindDC on the fresh
// target; the logo bitmap was created on the lost device and must be rebuilt.
// 260922_0001 A4: returns true only when the target exists again (create
// succeeded); false on the factory-null / CreateTarget failure paths so the
// caller skips the pixel commit and retries recovery on the next Render.
bool FloatingBadge::RecreateAfterDeviceLost() {
    DIAG_F("BADGE/DeviceLost/001: D2DERR_RECREATE_TARGET; recreating render target\n");
    renderer_.ReleaseTarget(&dc_render_target_);
    if (!d2d_factory_) {
        return false; // Create() never finished; all render paths null-guard already
    }
    if (!renderer_.CreateTarget(d2d_factory_, &dc_render_target_)) {
        DIAG_F("BADGE/DeviceLost/002: render-target recreation failed; badge stays stale until next Create()\n");
        return false;
    }
    ReallocateBuffer(PhysW(), PhysH());
    LoadLogoBitmap();
    return true;
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
                    // REQ-052 P2: use the system drag threshold. A
                    // registry-set metric of 0 would make abs(d) >= 0 always
                    // true and turn mere click tremor into a drag, silently
                    // swallowing every click - so fall back to the historical
                    // 3px when the metric is unavailable.
                    int dragX = ::GetSystemMetrics(SM_CXDRAG);
                    int dragY = ::GetSystemMetrics(SM_CYDRAG);
                    if (dragX <= 0) dragX = 3;
                    if (dragY <= 0) dragY = 3;
                    if (abs(dx) >= dragX || abs(dy) >= dragY) {
                        self->is_dragging_ = true;
                        ::KillTimer(hwnd, kTimerSingleClick);
                    }
                }

                if (self->is_dragging_) {
                    int newX = self->drag_start_window_.x + dx;
                    int newY = self->drag_start_window_.y + dy;

                    // Multi-monitor aware bounds clamping: find nearest monitor to badge center.
                    // REQ-R15: cursor deltas and monitor rects are PHYSICAL px; the badge
                    // extents must be converted from DIP before they mix. Re-check the
                    // monitor DPI each move so a drag onto a differently-scaled monitor
                    // keeps the pill visually the same physical footprint once it lands.
                    POINT ptCenter = { newX + self->PhysW() / 2, newY + self->PhysH() / 2 };
                    const UINT targetDpi = emebalachat::ui::MonitorDpiAtPoint(ptCenter);
                    bool dpiChanged = false;
                    if (targetDpi != self->dpi_) {
                        self->dpi_ = targetDpi;
                        // REQ-052 P1: ReallocateBuffer destroys the DIB, so the
                        // contents must be re-rendered below; ReallocateBuffer
                        // alone left the badge transparent until the next state
                        // change.
                        self->ReallocateBuffer(self->PhysW(), self->PhysH());
                        dpiChanged = true;
                    }
                    HMONITOR hMon = ::MonitorFromPoint(ptCenter, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO mi = {};
                    mi.cbSize = sizeof(MONITORINFO);
                    if (::GetMonitorInfoW(hMon, &mi)) {
                        const POINT clamped = emebalachat::ui::ClampWindowOrigin(
                            newX, newY, self->PhysW(), self->PhysH(), 0, mi.rcWork);
                        newX = clamped.x;
                        newY = clamped.y;
                    }

                    // REQ-052 P1: when the DPI change above reallocated the DIB
                    // at a new physical size, the size must actually be applied -
                    // SWP_NOSIZE would leave the window at the old scale and the
                    // blit would be rescaled (ghost pill).
                    ::SetWindowPos(hwnd, HWND_TOPMOST, newX, newY,
                                   self->PhysW(), self->PhysH(),
                                   SWP_NOACTIVATE | (dpiChanged ? 0 : SWP_NOSIZE));
                    if (dpiChanged) {
                        // Full render path restores the destroyed DIB contents;
                        // Render() terminates in UpdateAlpha(current_alpha_), so
                        // the re-blit happens in the same call. Mirrors the
                        // WM_DPICHANGED house pattern in tooltip.cpp /
                        // about_window.cpp (resize -> reallocate -> re-render).
                        self->Render();
                    }
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
                // Click in place: start single-click timer (sized to the OS
                // double-click window captured at Create) to allow double-click
                // detection
                ::SetTimer(hwnd, kTimerSingleClick, self->single_click_delay_ms_, nullptr);
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
            } else if (wParam == kTimerDisplayChange) {
                // 261029_0001 U-I (R1-21): the topology burst has settled -
                // re-clamp once, through the same routine Create/drag use.
                ::KillTimer(hwnd, kTimerDisplayChange);
                self->ReclampToVisibleMonitors();
            }
            return 0;
        }

        case WM_NCDESTROY: {
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        // 261029_0001 U-I (R1-21, env research F4): monitor topology change
        // (undock / unplug / resolution change). Windows broadcasts
        // WM_DISPLAYCHANGE to every top-level window, and this badge is a
        // WS_POPUP top-level window, so it hears the event directly - no
        // main.cpp routing needed. A single topology change arrives as a burst
        // (mode set, then the arrangement/DPI settle), so the handler only
        // arms a short timer and each further message restarts it (SetTimer on
        // an existing id resets the countdown). WM_TIMER then re-clamps once.
        case WM_DISPLAYCHANGE: {
            ::SetTimer(hwnd, kTimerDisplayChange, kDisplayChangeDebounceMs, nullptr);
            return 0;
        }

        // 260922_0001 A3 (fix plan §5 A3): live per-monitor DPI change. The
        // badge used to re-scale only during cross-DPI drags (WM_MOUSEMOVE
        // path) or at the next state change, so a display-scaling change on a
        // stationary badge left the physical DIB stale and the DWM compositor
        // upscaled the UpdateLayeredWindow blit — blurry pill text. Ports the
        // 5-step house pattern from tooltip.cpp WM_DPICHANGED (and
        // about_window.cpp): suggested origin + dpi refresh + own-extent
        // resize + buffer reallocation + full re-render.
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            self->dpi_ = emebalachat::ui::WindowDpi(hwnd);
            // Keep our own PhysW()/PhysH() extents (ScaleDipsToPixels
            // rounding) instead of the suggested size: DIB and window rect
            // must match exactly or the blit is rescaled — the very defect
            // being fixed (tooltip.cpp:2174-2176 contract).
            ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                           self->PhysW(), self->PhysH(),
                           SWP_NOZORDER | SWP_NOACTIVATE);
            self->ReallocateBuffer(self->PhysW(), self->PhysH());
            // Render() terminates in UpdateAlpha(current_alpha_), so the
            // re-blit happens inside this call (no separate Present needed);
            // the A4 reentry_allowed guard applies through it too.
            self->Render();
            return 0;
        }

        case WM_DESTROY: {
            return 0;
        }

        default:
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace emebalachat
