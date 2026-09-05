#include "about_window.hpp"
#include "asset_loader.hpp"
#include "dpi.hpp"
#include "../i18n.hpp"
#include "../version.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <memory>
#include <shellapi.h>
#include <string>

namespace emebalachat {

namespace {
const wchar_t kAboutClassName[] = L"Emebalachat_AboutClass";

// ---- REQ-005 content constants (architect plan §2.2, verbatim copy) ----
// Body content stays English (marketing copy) per the plan's deliberate i18n
// decision; only the menu label and window title are localized (Batch 1).
constexpr wchar_t kTagline[] =
    L"Never copy-paste again. Type naturally in your native tongue \u2014 "
    L"translations replace your keystrokes in real time inside any Windows application.";

constexpr wchar_t kFeature0[] =
    L"\u26A1 Drag-to-Translate \u2014 select text in any app, the floating icon translates instantly.";
constexpr wchar_t kFeature1[] =
    L"\U0001F50A Neural TTS \u2014 KO/EN/JA/DE pronunciation.";
constexpr wchar_t kFeature2[] =
    L"\U0001F512 100% on-device & private \u2014 active only while the shortcut is held; clipboard untouched.";

constexpr wchar_t kEtymology[] =
    L"In 2000 BCE, Mesopotamian scribes called \u201CEme-bala\u201D \u2014 those who turn language to bridge worlds.";

const wchar_t* const kLinkLabels[AboutWindow::kNumLinks] = { L"Website", L"Contact", L"Download" };
const wchar_t* const kLinkUrls[AboutWindow::kNumLinks] = {
    L"https://www.emebala.org/emebalachat",
    L"https://www.emebala.org/contact",
    L"https://github.com/teamsunplaza/Emebalachat/releases/latest",
};

constexpr wchar_t kContact0[] =
    L"Team Sunplaza \u00B7 Seoul Yeongdeungpo (Room 219, 65 Yeongjung-ro)";
constexpr wchar_t kContact1[] =
    L"+82 2 575 0414 \u00B7 Office hours 10:00\u201319:00 KST";
constexpr wchar_t kContact2[] =
    L"Lead Architect: Yongtai Kim";

// Hover index encoding for hovered_link_ (link slots 0..2, close = 3).
constexpr int kHoverClose = 3;

bool IsPointInRect(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}
} // namespace

AboutWindow::AboutWindow() = default;

AboutWindow::~AboutWindow() {
    Destroy();
}

int AboutWindow::PhysW() const {
    return emebalachat::ui::ScaleDipsToPixels(current_width_, dpi_);
}

int AboutWindow::PhysH() const {
    return emebalachat::ui::ScaleDipsToPixels(current_height_, dpi_);
}

bool AboutWindow::Create(HINSTANCE hInstance) {
    hInstance_ = hInstance;
    // REQ-R10 pattern (see tooltip.cpp): remember the owning GUI thread; Show
    // from any other thread marshals via PostMessageW instead of touching the
    // single-threaded D2D target cross-thread (D2DERR_WRONG_THREAD).
    gui_thread_id_ = ::GetCurrentThreadId();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = AboutWindow::WndProc;
    wc.hInstance = hInstance_;
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = kAboutClassName;
    ::RegisterClassExW(&wc);

    // Activatable by design (plan §2.2): NO WS_EX_NOACTIVATE so keyboard focus
    // reaches WM_KEYDOWN/ESC. Layered + topmost + toolwindow keeps the
    // tooltip's popup look without a taskbar entry.
    hwnd_ = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kAboutClassName,
        I18n::Get(StringId::AboutTitle).c_str(),
        WS_POPUP,
        -1000, -1000, PhysW(), PhysH(),
        nullptr, nullptr, hInstance_, this
    );

    if (!hwnd_) {
        return false;
    }

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

    ReallocateBuffer(PhysW(), PhysH());
    LoadLogoBitmap();

    if (FAILED(::DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&dwrite_factory_)
    ))) {
        return false;
    }

    // Same font policy as the tooltip: Segoe UI Variable Text when installed.
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

    auto makeFormat = [&](DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style,
                          FLOAT size, DWRITE_TEXT_ALIGNMENT align,
                          bool wrap, IDWriteTextFormat** out) {
        if (FAILED(dwrite_factory_->CreateTextFormat(
                fontName, nullptr, weight, style, DWRITE_FONT_STRETCH_NORMAL,
                size, L"", out))) {
            return;
        }
        (*out)->SetTextAlignment(align);
        (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        if (wrap) {
            (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        }
    };

    makeFormat(DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, 22.0f,
               DWRITE_TEXT_ALIGNMENT_CENTER, false, &title_format_);
    makeFormat(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, 11.0f,
               DWRITE_TEXT_ALIGNMENT_CENTER, false, &version_format_);
    makeFormat(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, 12.0f,
               DWRITE_TEXT_ALIGNMENT_CENTER, true, &tagline_format_);
    makeFormat(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, 12.0f,
               DWRITE_TEXT_ALIGNMENT_LEADING, true, &body_format_);
    makeFormat(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC, 11.0f,
               DWRITE_TEXT_ALIGNMENT_CENTER, true, &etymology_format_);
    makeFormat(DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL, 12.0f,
               DWRITE_TEXT_ALIGNMENT_CENTER, false, &link_format_);
    makeFormat(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, 10.5f,
               DWRITE_TEXT_ALIGNMENT_CENTER, false, &small_format_);
    makeFormat(DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, 12.0f,
               DWRITE_TEXT_ALIGNMENT_CENTER, false, &header_format_);
    if (header_format_) {
        header_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    return true;
}

void AboutWindow::Destroy() {
    if (hwnd_) {
        ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    visible_ = false;

    if (logo_bitmap_) { logo_bitmap_->Release(); logo_bitmap_ = nullptr; }
    if (header_format_) { header_format_->Release(); header_format_ = nullptr; }
    if (small_format_) { small_format_->Release(); small_format_ = nullptr; }
    if (link_format_) { link_format_->Release(); link_format_ = nullptr; }
    if (etymology_format_) { etymology_format_->Release(); etymology_format_ = nullptr; }
    if (body_format_) { body_format_->Release(); body_format_ = nullptr; }
    if (tagline_format_) { tagline_format_->Release(); tagline_format_ = nullptr; }
    if (version_format_) { version_format_->Release(); version_format_ = nullptr; }
    if (title_format_) { title_format_->Release(); title_format_ = nullptr; }
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
        pBits_ = nullptr;
    }
}

void AboutWindow::LoadLogoBitmap() {
    if (!dc_render_target_) return;
    if (logo_bitmap_) {
        logo_bitmap_->Release();
        logo_bitmap_ = nullptr;
    }

    // Plan §2.2 item 1: branded app icon via the existing asset loader.
    std::wstring path = FindAppIconPath();
    if (!path.empty()) {
        LoadWicBitmap(dc_render_target_, path, &logo_bitmap_);
    }
}

void AboutWindow::ReallocateBuffer(int width, int height) {
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
        // REQ-R15: D2D DPI tracks the monitor so DIP layout rasterizes 1:1.
        dc_render_target_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
        RECT rc = { 0, 0, width, height };
        dc_render_target_->BindDC(hMemDC_, &rc);
    }
}

void AboutWindow::RebindRenderTarget() {
    if (!dc_render_target_ || !hMemDC_) {
        return;
    }
    const RECT rc = { 0, 0, PhysW(), PhysH() };
    dc_render_target_->BindDC(hMemDC_, &rc);
}

void AboutWindow::Show(int x, int y) {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() == gui_thread_id_) {
        ShowAt(x, y);
        return;
    }
    // REQ-R10 marshaling: heap payload through LPARAM, ownership transferred
    // to WndProc; a failed post frees locally (tooltip.cpp seam contract).
    auto payload = std::make_unique<ShowPayload>();
    payload->x = x;
    payload->y = y;
    const LPARAM lparam = reinterpret_cast<LPARAM>(payload.release());
    if (::PostMessageW(hwnd_, kShowMessage, 0, lparam) == FALSE) {
        delete reinterpret_cast<ShowPayload*>(lparam);
    }
}

void AboutWindow::ShowAt(int x, int y) {
    if (!hwnd_) return;

    // REQ-R15: capture the target monitor's DPI from the (physical) caller
    // point, size the DIB in physical px, center in the work area, clamp.
    dpi_ = emebalachat::ui::MonitorDpiAtPoint(POINT{ x, y });
    ReallocateBuffer(PhysW(), PhysH());

    POINT pt = { x, y };
    HMONITOR hMon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(MONITORINFO);
    if (::GetMonitorInfoW(hMon, &mi)) {
        const int work_w = mi.rcWork.right - mi.rcWork.left;
        const int work_h = mi.rcWork.bottom - mi.rcWork.top;
        x = mi.rcWork.left + (work_w - PhysW()) / 2;
        y = mi.rcWork.top + (work_h - PhysH()) / 2;
        const POINT clamped = emebalachat::ui::ClampWindowOrigin(x, y, PhysW(), PhysH(), 10, mi.rcWork);
        x = clamped.x;
        y = clamped.y;
    }

    hovered_link_ = -1;
    ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, PhysW(), PhysH(), SWP_SHOWWINDOW);
    ::SetForegroundWindow(hwnd_); // activatable: keyboard focus for ESC
    ::SetFocus(hwnd_);
    visible_ = true;

    Render();
    UpdateLayered();
}

void AboutWindow::Dismiss() {
    if (!hwnd_) return;
    // The mouse-hook outside-click path runs on the hook thread: marshal.
    if (::GetCurrentThreadId() != gui_thread_id_) {
        ::PostMessageW(hwnd_, kDismissMessage, 0, 0);
        return;
    }
    if (!visible_.load(std::memory_order_relaxed)) return;
    visible_ = false;
    hovered_link_ = -1;
    ::ShowWindow(hwnd_, SW_HIDE);
}

void AboutWindow::OpenLink(int index) {
    if (index < 0 || index >= kNumLinks) return;
    HINSTANCE hRes = ::ShellExecuteW(hwnd_, L"open", kLinkUrls[index], nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(hRes) <= 32) {
        // Traceable error code per project convention; the URL was not opened.
        fprintf(stderr, "ABOUT/OpenLink/001: ShellExecuteW failed (INT_PTR %lld) for %ls\n",
                static_cast<long long>(reinterpret_cast<INT_PTR>(hRes)), kLinkUrls[index]);
    }
}

void AboutWindow::Render() {
    if (!dc_render_target_) return;

    // REQ-R15: DIP layout authored below; BindDC rect is the physical buffer.
    RebindRenderTarget();

    dc_render_target_->BeginDraw();
    dc_render_target_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    const float w = static_cast<float>(current_width_);
    const float h = static_cast<float>(current_height_);

    // Outer acrylic card, same container geometry as the tooltip.
    D2D1_ROUNDED_RECT card = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), 10.0f, 10.0f);

    ID2D1SolidColorBrush* bgBrush = nullptr;
    ID2D1SolidColorBrush* borderBrush = nullptr;
    ID2D1SolidColorBrush* textBrush = nullptr;
    ID2D1SolidColorBrush* subTextBrush = nullptr;
    ID2D1SolidColorBrush* dividerBrush = nullptr;
    ID2D1SolidColorBrush* accentBrush = nullptr;
    ID2D1SolidColorBrush* pillBgBrush = nullptr;
    ID2D1SolidColorBrush* pillBgHoverBrush = nullptr;
    ID2D1SolidColorBrush* goldBorderBrush = nullptr;
    ID2D1SolidColorBrush* logoBgBrush = nullptr;
    ID2D1SolidColorBrush* closeBrush = nullptr;
    ID2D1SolidColorBrush* closeHoverBrush = nullptr;

    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x0F172A, 0.96f), &bgBrush);
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x334155, 0.85f), &borderBrush);
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0xF8FAFC, 1.0f), &textBrush);
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x94A3B8, 1.0f), &subTextBrush);
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x334155, 0.5f), &dividerBrush);
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x10B981, 1.0f), &accentBrush);
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x1E293B, 0.9f), &pillBgBrush);
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x334155, 1.0f), &pillBgHoverBrush);
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0xD4AF37, 0.85f), &goldBorderBrush);
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x1E293B, 0.9f), &logoBgBrush);
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x94A3B8, 0.8f), &closeBrush);
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0xEF4444, 1.0f), &closeHoverBrush);

    if (bgBrush) dc_render_target_->FillRoundedRectangle(card, bgBrush);
    if (borderBrush) dc_render_target_->DrawRoundedRectangle(card, borderBrush, 1.0f);

    // 1. Logo: 64x64 squircle, gold border (plan §2.2 layout item 1).
    const D2D1_RECT_F logoFrame = D2D1::RectF((w - 64.0f) / 2.0f, 28.0f, (w + 64.0f) / 2.0f, 92.0f);
    if (logoBgBrush) {
        dc_render_target_->FillRoundedRectangle(D2D1::RoundedRect(logoFrame, 14.0f, 14.0f), logoBgBrush);
    }
    if (goldBorderBrush) {
        dc_render_target_->DrawRoundedRectangle(D2D1::RoundedRect(logoFrame, 14.0f, 14.0f), goldBorderBrush, 1.2f);
    }
    const D2D1_RECT_F logoRect = D2D1::RectF(logoFrame.left + 6.0f, logoFrame.top + 6.0f,
                                             logoFrame.right - 6.0f, logoFrame.bottom - 6.0f);
    if (logo_bitmap_) {
        dc_render_target_->DrawBitmap(logo_bitmap_, logoRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        DrawTabletLogoVector(dc_render_target_, logoRect, false);
    }

    // 2. Title + version (version from the single source of truth, REQ-006).
    const std::wstring titleText(kAppNameW);
    if (title_format_ && textBrush) {
        dc_render_target_->DrawText(titleText.c_str(), static_cast<UINT32>(titleText.size()),
                                    title_format_, D2D1::RectF(24.0f, 100.0f, w - 24.0f, 132.0f), textBrush);
    }
    const std::wstring versionText = L"v" + std::wstring(kAppVersionW);
    if (version_format_ && subTextBrush) {
        dc_render_target_->DrawText(versionText.c_str(), static_cast<UINT32>(versionText.size()),
                                    version_format_, D2D1::RectF(24.0f, 134.0f, w - 24.0f, 152.0f), subTextBrush);
    }

    // 3. Tagline (body 12, wrap, centered).
    if (tagline_format_ && textBrush) {
        dc_render_target_->DrawText(kTagline, static_cast<UINT32>(std::size(kTagline) - 1),
                                    tagline_format_, D2D1::RectF(24.0f, 162.0f, w - 24.0f, 208.0f), textBrush);
    }

    // Divider 1
    if (dividerBrush) {
        dc_render_target_->DrawLine(D2D1::Point2F(24.0f, 218.0f), D2D1::Point2F(w - 24.0f, 218.0f),
                                    dividerBrush, 1.0f);
    }

    // 4. Features: 3 blocks of 42 DIP, wrapped leading text.
    const wchar_t* const features[3] = { kFeature0, kFeature1, kFeature2 };
    if (body_format_ && textBrush) {
        for (int i = 0; i < 3; ++i) {
            const float top = 230.0f + static_cast<float>(i) * 42.0f;
            dc_render_target_->DrawText(features[i],
                                        static_cast<UINT32>(std::wcslen(features[i])),
                                        body_format_,
                                        D2D1::RectF(28.0f, top, w - 28.0f, top + 40.0f),
                                        textBrush);
        }
    }

    // 5. Etymology (italic 11, subtext).
    if (etymology_format_ && subTextBrush) {
        dc_render_target_->DrawText(kEtymology, static_cast<UINT32>(std::size(kEtymology) - 1),
                                    etymology_format_, D2D1::RectF(24.0f, 362.0f, w - 24.0f, 402.0f), subTextBrush);
    }

    // Divider 2
    if (dividerBrush) {
        dc_render_target_->DrawLine(D2D1::Point2F(24.0f, 414.0f), D2D1::Point2F(w - 24.0f, 414.0f),
                                    dividerBrush, 1.0f);
    }

    // 6. Links row: 3 pill buttons centered (Website / Contact / Download).
    const float pill_w = 100.0f;
    const float pill_gap = 10.0f;
    const float pills_total = pill_w * kNumLinks + pill_gap * (kNumLinks - 1);
    const float pills_x = (w - pills_total) / 2.0f;
    for (int i = 0; i < kNumLinks; ++i) {
        link_rects_[i] = D2D1::RectF(pills_x + static_cast<float>(i) * (pill_w + pill_gap),
                                     428.0f,
                                     pills_x + static_cast<float>(i) * (pill_w + pill_gap) + pill_w,
                                     456.0f);
        const bool hover = (hovered_link_ == i);
        const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(link_rects_[i], 4.0f, 4.0f);
        if (hover && pillBgHoverBrush) {
            dc_render_target_->FillRoundedRectangle(pill, pillBgHoverBrush);
        } else if (pillBgBrush) {
            dc_render_target_->FillRoundedRectangle(pill, pillBgBrush);
        }
        if (accentBrush) {
            dc_render_target_->DrawRoundedRectangle(pill, accentBrush, hover ? 1.4f : 1.0f);
        }
        if (link_format_ && textBrush) {
            dc_render_target_->DrawText(kLinkLabels[i],
                                        static_cast<UINT32>(std::wcslen(kLinkLabels[i])),
                                        link_format_, link_rects_[i], textBrush);
        }
    }

    // 7. Contact block (small 10.5, subtext, centered).
    const wchar_t* const contacts[3] = { kContact0, kContact1, kContact2 };
    if (small_format_ && subTextBrush) {
        for (int i = 0; i < 3; ++i) {
            const float top = 472.0f + static_cast<float>(i) * 22.0f;
            dc_render_target_->DrawText(contacts[i],
                                        static_cast<UINT32>(std::wcslen(contacts[i])),
                                        small_format_,
                                        D2D1::RectF(24.0f, top, w - 24.0f, top + 20.0f),
                                        subTextBrush);
        }
    }

    // 8. Close button top-right (same rect math as the tooltip's).
    close_btn_rect_ = D2D1::RectF(w - 32.0f, 12.0f, w - 12.0f, 32.0f);
    if (header_format_) {
        ID2D1SolidColorBrush* brush = (hovered_link_ == kHoverClose && closeHoverBrush)
                                          ? closeHoverBrush : closeBrush;
        if (brush) {
            dc_render_target_->DrawText(L"\u2715", 1, header_format_, close_btn_rect_, brush);
        }
    }

    if (closeHoverBrush) closeHoverBrush->Release();
    if (closeBrush) closeBrush->Release();
    if (logoBgBrush) logoBgBrush->Release();
    if (goldBorderBrush) goldBorderBrush->Release();
    if (pillBgHoverBrush) pillBgHoverBrush->Release();
    if (pillBgBrush) pillBgBrush->Release();
    if (accentBrush) accentBrush->Release();
    if (dividerBrush) dividerBrush->Release();
    if (subTextBrush) subTextBrush->Release();
    if (textBrush) textBrush->Release();
    if (borderBrush) borderBrush->Release();
    if (bgBrush) bgBrush->Release();

    dc_render_target_->EndDraw();
}

void AboutWindow::UpdateLayered() {
    if (!hwnd_ || !hMemDC_) return;

    POINT ptSrc = { 0, 0 };
    SIZE sz = { PhysW(), PhysH() }; // REQ-R15: physical blit size
    POINT ptDst = {};
    RECT rcWindow = {};
    ::GetWindowRect(hwnd_, &rcWindow);
    ptDst.x = rcWindow.left;
    ptDst.y = rcWindow.top;

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 245; // ~96% opacity, matches the tooltip card
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

LRESULT CALLBACK AboutWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* pThis = reinterpret_cast<AboutWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = static_cast<AboutWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    if (!pThis) {
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
        // ---- REQ-R10 marshaled show/dismiss requests (GUI thread runs them) ----
        case kShowMessage: {
            const std::unique_ptr<ShowPayload> p(reinterpret_cast<ShowPayload*>(lParam));
            if (p) {
                pThis->ShowAt(p->x, p->y);
            }
            return 0;
        }

        case kDismissMessage: {
            pThis->Dismiss();
            return 0;
        }

        case WM_SETCURSOR: {
            POINT pt = {};
            ::GetCursorPos(&pt);
            ::ScreenToClient(hwnd, &pt);
            // REQ-R15: client coords are physical px; rects are DIP.
            pt.x = emebalachat::ui::ScalePixelsToDips(pt.x, pThis->dpi_);
            pt.y = emebalachat::ui::ScalePixelsToDips(pt.y, pThis->dpi_);
            const float x = static_cast<float>(pt.x);
            const float y = static_cast<float>(pt.y);

            bool interactive = IsPointInRect(pThis->close_btn_rect_, x, y);
            for (int i = 0; !interactive && i < kNumLinks; ++i) {
                interactive = IsPointInRect(pThis->link_rects_[i], x, y);
            }
            ::SetCursor(::LoadCursorW(nullptr,
                interactive ? MAKEINTRESOURCEW(32649) /* hand */ : MAKEINTRESOURCEW(32512) /* arrow */));
            return TRUE;
        }

        case WM_MOUSEMOVE: {
            // REQ-R15: physical client px -> DIP (tooltip's convention).
            const float x = static_cast<float>(emebalachat::ui::ScalePixelsToDips(
                static_cast<int>(static_cast<short>(LOWORD(lParam))), pThis->dpi_));
            const float y = static_cast<float>(emebalachat::ui::ScalePixelsToDips(
                static_cast<int>(static_cast<short>(HIWORD(lParam))), pThis->dpi_));

            int hover = -1;
            for (int i = 0; i < kNumLinks; ++i) {
                if (IsPointInRect(pThis->link_rects_[i], x, y)) {
                    hover = i;
                    break;
                }
            }
            if (hover < 0 && IsPointInRect(pThis->close_btn_rect_, x, y)) {
                hover = kHoverClose;
            }

            if (hover != pThis->hovered_link_) {
                pThis->hovered_link_ = hover;
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
            if (pThis->hovered_link_ != -1) {
                pThis->hovered_link_ = -1;
                pThis->Render();
                pThis->UpdateLayered();
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            const float x = static_cast<float>(emebalachat::ui::ScalePixelsToDips(
                static_cast<int>(static_cast<short>(LOWORD(lParam))), pThis->dpi_));
            const float y = static_cast<float>(emebalachat::ui::ScalePixelsToDips(
                static_cast<int>(static_cast<short>(HIWORD(lParam))), pThis->dpi_));

            if (IsPointInRect(pThis->close_btn_rect_, x, y)) {
                pThis->Dismiss();
                return 0;
            }
            for (int i = 0; i < kNumLinks; ++i) {
                if (IsPointInRect(pThis->link_rects_[i], x, y)) {
                    pThis->OpenLink(i);
                    // Focus will leave for the browser; WM_KILLFOCUS dismisses.
                    return 0;
                }
            }
            return 0;
        }

        case WM_KEYDOWN: {
            if (wParam == VK_ESCAPE) {
                pThis->Dismiss();
                return 0;
            }
            break;
        }

        case WM_KILLFOCUS: {
            // Plan §2.2: closes on focus loss (Alt+Tab, app switch, browser
            // handoff after a link click).
            pThis->Dismiss();
            return 0;
        }

        case WM_DESTROY:
            return 0;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace emebalachat
