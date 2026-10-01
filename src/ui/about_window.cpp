#include "about_window.hpp"
#include "diag_logger.hpp"
#include "asset_loader.hpp"
#include "dpi.hpp"
#include "dwrite_helpers.hpp"  // REF-3.7: shared IsPointInRect / CloneFormatWithLocale
#include "../bidi_utils.hpp"  // DirectionForLocale / TextDirection
#include "../i18n.hpp"
#include "../unicode_utils.hpp"
#include "../version.hpp"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <mutex>
#include <optional>
#include <shellapi.h>
#include <string>
#include <string_view>
#include <thread>

namespace emebalachat {

namespace {
const wchar_t kAboutClassName[] = L"Emebalachat_AboutClass";

// REF-3.7: ApplyFormatLocale calls the shared EqualsIgnoreCaseAscii template
// directly (the former file-local WcsIEqualsAscii replication was deleted).
bool ApplyFormatLocale(IDWriteFactory* factory, IDWriteTextFormat** slot,
                       const std::wstring& tag) {
    if (!factory || !slot || !*slot || tag.empty()) return true; // vacuous
    wchar_t cur[64] = {};
    if (SUCCEEDED((*slot)->GetLocaleName(cur, 64)) && EqualsIgnoreCaseAscii<wchar_t>(cur, tag)) {
        return true; // unchanged tag: never churn the COM object (B-2 rule)
    }
    IDWriteTextFormat* swapped = CloneFormatWithLocale(factory, *slot, tag.c_str());
    if (!swapped) return false;
    wchar_t read[64] = {};
    if (FAILED(swapped->GetLocaleName(read, 64)) || !EqualsIgnoreCaseAscii<wchar_t>(read, tag)) {
        swapped->Release();
        return false;
    }
    (*slot)->Release();
    *slot = swapped;
    return true;
}

// Representative full BCP-47 tag for the CURRENT UI locale (B-3
// LocaleMapping.bcp47_full). Auto/unmapped resolves to the English pivot tag.
std::wstring LocaleTagForUi(UiLocale locale) {
    for (const LocaleMapping& m : GetLocaleMappings()) {
        if (m.locale == locale && m.bcp47_full && m.bcp47_full[0]) {
            return std::wstring(m.bcp47_full);
        }
    }
    return L"en-US";
}

// ---- REQ-UC (260930_0004, design spec §2): the exact palette -------------
const D2D1_COLOR_F kColCard      = D2D1::ColorF(0x0C1830, 0.96f); // navy-0
const D2D1_COLOR_F kColDialog    = D2D1::ColorF(0x101F3E, 1.0f);  // navy-1
const D2D1_COLOR_F kColLogoTile  = D2D1::ColorF(0x0F1E3D, 1.0f);  // navy-2
const D2D1_COLOR_F kColLapis     = D2D1::ColorF(0x33507E, 1.0f);
const D2D1_COLOR_F kColLapisHi   = D2D1::ColorF(0x4A6CA3, 1.0f);
const D2D1_COLOR_F kColGold      = D2D1::ColorF(0xD4AF37, 1.0f);
const D2D1_COLOR_F kColGoldHi    = D2D1::ColorF(0xE4C86B, 1.0f);
const D2D1_COLOR_F kColInk       = D2D1::ColorF(0xFFFFFF, 1.0f);
const D2D1_COLOR_F kColMut       = D2D1::ColorF(0xA9B6CF, 1.0f);
const D2D1_COLOR_F kColMut2      = D2D1::ColorF(0x8FA0C0, 1.0f);
const D2D1_COLOR_F kColFaint     = D2D1::ColorF(0x8093B5, 1.0f);
const D2D1_COLOR_F kColDanger    = D2D1::ColorF(0xE5645E, 1.0f);
const D2D1_COLOR_F kColDangerInk = D2D1::ColorF(0xF2C1BD, 1.0f);
const D2D1_COLOR_F kColTrack     = D2D1::ColorF(0x182A50, 1.0f);
const D2D1_COLOR_F kColScrim     = D2D1::ColorF(0x040812, 0.62f);
const D2D1_COLOR_F kColCardEdge  = D2D1::ColorF(0x5D7CB6, 0.38f);
const D2D1_COLOR_F kColOnGold    = D2D1::ColorF(0x0A1428, 1.0f); // button text
const D2D1_COLOR_F kColOnLapis   = D2D1::ColorF(0xDCE5F5, 1.0f); // secondary text

// Button variants (design spec §5).
enum UcButtonVariant { kUcPrimary, kUcSecondary, kUcDanger, kUcText };

// REQ-UC: card + region geometry (design spec §1 + restored intro, DIP).
const float kCardW = 440.0f;
const float kCardH = 625.0f;
// The transient update UI swaps the middle content region (below the seal,
// above the footer) and centers inside this band.
const float kZoneTop = 191.0f;
const float kZoneBottom = 570.0f;
const float kZoneCy = (kZoneTop + kZoneBottom) / 2.0f;
// 260930_0004 merge: the restored intro keeps its ORIGINAL internal geometry
// (byte-faithful, incl. the 260922_0001 A2 dynamic-height planner); it starts
// 29 DIP lower than the historical card because the REQ-UC brand chrome
// (80 DIP logo tile + 16 pt name + version line + seal hairlines) occupies
// 0..181 instead of the old 0..152.
const float kIntroShift = 29.0f;

// Copyright: universal factual brand data (never translated), two lines.
const wchar_t kCopyrightText[] = L"\x00A9 2026 Emebala Project\nMIT License";

// REQ-UC: quiet status glyph ids (hand-drawn vectors, no emoji per spec).
enum UcGlyph { kGlyphCheck, kGlyphClock, kGlyphCross, kGlyphWarn };

// Link URLs are factual brand data, never translated. Slot order matches the
// label StringIds (Website / Contact / Reddit).
const wchar_t* const kLinkUrls[AboutWindow::kNumLinks] = {
    L"https://www.emebala.org/emebalachat",
    L"https://www.emebala.org/contact",
    L"https://www.reddit.com/r/emebala/",
};
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
    gui_thread_id_ = ::GetCurrentThreadId();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = AboutWindow::WndProc;
    wc.hInstance = hInstance_;
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = kAboutClassName;
    ::RegisterClassExW(&wc);

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

    if (!renderer_.CreateTarget(d2d_factory_, &dc_render_target_)) {
        return false;
    }

    // C1: the persistent scratch brush + the REQ-UC gold progress gradient
    // live with the target (created here and at device-lost recovery).
    EnsureScratchBrush();

    // REQ-UC: the 270-degree spinner arc geometry (device-independent; one
    // geometry for the lifetime of the factory). Rotated per frame via a
    // transform at draw time.
    ID2D1GeometrySink* sink = nullptr;
    if (SUCCEEDED(d2d_factory_->CreatePathGeometry(&spinner_arc_)) && spinner_arc_ &&
        SUCCEEDED(spinner_arc_->Open(&sink)) && sink) {
        sink->BeginFigure(D2D1::Point2F(10.0f, 0.0f), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(0.0f, 10.0f), D2D1::SizeF(10.0f, 10.0f),
                                      0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL)); // 0..90 deg
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(-10.0f, 0.0f), D2D1::SizeF(10.0f, 10.0f),
                                      0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL)); // 90..180
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(0.0f, -10.0f), D2D1::SizeF(10.0f, 10.0f),
                                      0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                      D2D1_ARC_SIZE_SMALL)); // 180..270
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        sink->Release();
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

    // REQ-UC brand chrome (design spec §3) + restored intro formats
    // (byte-faithful pre-rework typography) + update-zone stack.
    makeFormat(DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, 16.0f,
               DWRITE_TEXT_ALIGNMENT_CENTER, false, &title_format_);      // app name
    makeFormat(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, 11.0f,
               DWRITE_TEXT_ALIGNMENT_CENTER, false, &version_format_);    // version line
    makeFormat(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, 12.0f,
               DWRITE_TEXT_ALIGNMENT_CENTER, true, &tagline_format_);     // intro tagline
    makeFormat(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, 12.0f,
               DWRITE_TEXT_ALIGNMENT_LEADING, true, &body_format_);       // intro features
    makeFormat(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC, 11.0f,
               DWRITE_TEXT_ALIGNMENT_CENTER, true, &etymology_format_);   // intro etymology
    makeFormat(DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL, 12.0f,
               DWRITE_TEXT_ALIGNMENT_CENTER, false, &link_format_);       // intro link pills
    makeFormat(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, 10.5f,
               DWRITE_TEXT_ALIGNMENT_CENTER, false, &small_format_);      // intro contacts
    makeFormat(DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, 12.0f,
               DWRITE_TEXT_ALIGNMENT_CENTER, false, &header_format_);     // close glyph
    makeFormat(DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, 12.5f,
               DWRITE_TEXT_ALIGNMENT_CENTER, true, &uz_head_format_);     // state headline
    makeFormat(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, 10.5f,
               DWRITE_TEXT_ALIGNMENT_CENTER, true, &uz_sub_format_);      // sub / note lines
    makeFormat(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, 10.5f,
               DWRITE_TEXT_ALIGNMENT_LEADING, true, &uz_label_format_);   // dialog labels/meta
    makeFormat(DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, 12.5f,
               DWRITE_TEXT_ALIGNMENT_CENTER, false, &uz_button_format_);  // button labels
    makeFormat(DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL, 11.0f,
               DWRITE_TEXT_ALIGNMENT_CENTER, false, &uz_textbtn_format_); // text buttons
    makeFormat(DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, 13.0f,
               DWRITE_TEXT_ALIGNMENT_CENTER, false, &uz_dlgtitle_format_);// dialog title
    makeFormat(DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, 12.0f,
               DWRITE_TEXT_ALIGNMENT_LEADING, true, &uz_value_format_);   // dialog values
    makeFormat(DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, 13.0f,
               DWRITE_TEXT_ALIGNMENT_TRAILING, false, &uz_pct_format_);   // progress percent
    makeFormat(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, 10.0f,
               DWRITE_TEXT_ALIGNMENT_LEADING, true, &copyright_format_);  // footer copyright

    // Footer reset pill: a WRAPPING variant of the link-pill typography — a
    // long localized label (Arabic) wraps to a second line inside a taller
    // pill instead of overflowing (trimming is NOT used here: a trimming
    // sign makes DWrite break lines at the layout width even for short
    // clamped pills — see the unified footer block below).
    makeFormat(DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL, 12.0f,
               DWRITE_TEXT_ALIGNMENT_CENTER, true, &reset_footer_format_);

    // REQ-UC §11: zone button labels ellipsis at word boundaries (the buttons
    // are content-sized, so trimming never engages wrap in practice). NOTE:
    // never set trimming on link_format_/reset_footer_format_ — a trimming
    // sign makes DWrite break lines at the layout width, which turned the
    // width-clamped Arabic reset label into a 3-line overflow inside its
    // pill (blind-QA defect).
    auto set_word_trimming = [&](IDWriteTextFormat* fmt) {
        if (!fmt) return;
        IDWriteInlineObject* sign = nullptr;
        if (SUCCEEDED(dwrite_factory_->CreateEllipsisTrimmingSign(fmt, &sign))) {
            const DWRITE_TRIMMING trimming =
                { DWRITE_TRIMMING_GRANULARITY_WORD, 0, 0 };
            fmt->SetTrimming(&trimming, sign);
            if (sign) sign->Release();
        }
    };
    set_word_trimming(uz_button_format_);
    if (header_format_) {
        header_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    ApplyLocaleFormatting();

    return true;
}

void AboutWindow::Destroy() {
    // REQ-UC: cancel any in-flight worker. The detached thread only holds a
    // shared_ptr<UpdateJob> + an HWND value (never `this`), so it cannot race
    // this teardown: it aborts at the next cancel poll, its PostMessageW
    // against the dying HWND fails harmless, and the job dies with it. SEC
    // Finding 8: if the process exits while a worker is blocked in a WinHTTP
    // read, ExitProcess terminates the thread before its OutFile guard runs,
    // leaving an orphaned partial in %TEMP% — never executed (launch requires
    // full download + verified hash) and truncated by the next attempt's
    // CREATE_ALWAYS. Accepted + documented (joining here would stall shutdown
    // for up to the 30 s receive timeout).
    if (job_) {
        job_->cancel.store(true, std::memory_order_relaxed);
        job_.reset();
    }

    if (hwnd_) {
        ::KillTimer(hwnd_, kResetFeedbackTimerId);
        ::KillTimer(hwnd_, kUpdateAnimTimerId);
        ::KillTimer(hwnd_, kUpToDateReturnTimerId);
        reset_feedback_until_ = 0;
        ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    visible_ = false;

    if (spinner_arc_) { spinner_arc_->Release(); spinner_arc_ = nullptr; }
    if (logo_bitmap_) { logo_bitmap_->Release(); logo_bitmap_ = nullptr; }
    if (copyright_format_) { copyright_format_->Release(); copyright_format_ = nullptr; }
    if (uz_pct_format_) { uz_pct_format_->Release(); uz_pct_format_ = nullptr; }
    if (uz_value_format_) { uz_value_format_->Release(); uz_value_format_ = nullptr; }
    if (uz_dlgtitle_format_) { uz_dlgtitle_format_->Release(); uz_dlgtitle_format_ = nullptr; }
    if (uz_textbtn_format_) { uz_textbtn_format_->Release(); uz_textbtn_format_ = nullptr; }
    if (uz_button_format_) { uz_button_format_->Release(); uz_button_format_ = nullptr; }
    if (uz_label_format_) { uz_label_format_->Release(); uz_label_format_ = nullptr; }
    if (uz_sub_format_) { uz_sub_format_->Release(); uz_sub_format_ = nullptr; }
    if (uz_head_format_) { uz_head_format_->Release(); uz_head_format_ = nullptr; }
    if (header_format_) { header_format_->Release(); header_format_ = nullptr; }
    if (small_format_) { small_format_->Release(); small_format_ = nullptr; }
    if (link_format_) { link_format_->Release(); link_format_ = nullptr; }
    if (reset_footer_format_) { reset_footer_format_->Release(); reset_footer_format_ = nullptr; }
    if (etymology_format_) { etymology_format_->Release(); etymology_format_ = nullptr; }
    if (body_format_) { body_format_->Release(); body_format_ = nullptr; }
    if (tagline_format_) { tagline_format_->Release(); tagline_format_ = nullptr; }
    if (version_format_) { version_format_->Release(); version_format_ = nullptr; }
    if (title_format_) { title_format_->Release(); title_format_ = nullptr; }
    if (dwrite_factory_) { dwrite_factory_->Release(); dwrite_factory_ = nullptr; }
    ReleaseScratchBrush(); // C1: brush released while its target is still alive
    renderer_.ReleaseTarget(&dc_render_target_);
    if (d2d_factory_) { d2d_factory_->Release(); d2d_factory_ = nullptr; }

    renderer_.FreeBuffer();
}

// C1: the single persistent scratch brush (SetColor just before use) plus the
// REQ-UC gold progress gradient, both rebuilt on the live target.
void AboutWindow::EnsureScratchBrush() {
    if (!scratch_brush_ && dc_render_target_) {
        dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f),
                                                 &scratch_brush_);
    }
    if (!progress_brush_ && dc_render_target_) {
        const D2D1_GRADIENT_STOP stops[4] = {
            { 0.00f, D2D1::ColorF(0xB8962C, 1.0f) }, // gold-lo
            { 0.45f, D2D1::ColorF(0xD4AF37, 1.0f) }, // gold
            { 0.72f, D2D1::ColorF(0xE4C86B, 1.0f) }, // gold-hi
            { 1.00f, D2D1::ColorF(0xD4AF37, 1.0f) }, // gold
        };
        ID2D1GradientStopCollection* collection = nullptr;
        if (SUCCEEDED(dc_render_target_->CreateGradientStopCollection(
                stops, 4, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &collection)) && collection) {
            dc_render_target_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f),
                                                    D2D1::Point2F(1.0f, 0.0f)),
                collection, &progress_brush_);
            collection->Release();
        }
    }
}

void AboutWindow::ReleaseScratchBrush() {
    if (progress_brush_) {
        progress_brush_->Release();
        progress_brush_ = nullptr;
    }
    if (scratch_brush_) {
        scratch_brush_->Release();
        scratch_brush_ = nullptr;
    }
}

void AboutWindow::LoadLogoBitmap() {
    if (!dc_render_target_) return;
    if (logo_bitmap_) {
        logo_bitmap_->Release();
        logo_bitmap_ = nullptr;
    }
    std::wstring path = FindAppIconPath();
    if (!path.empty()) {
        LoadWicBitmap(dc_render_target_, path, &logo_bitmap_);
    }
}

void AboutWindow::ReallocateBuffer(int width, int height) {
    renderer_.ReallocateBuffer(width, height, dpi_);
}

void AboutWindow::RebindRenderTarget() {
    renderer_.Rebind(PhysW(), PhysH(), dpi_, /*set_dpi=*/false);
}

void AboutWindow::Show(int x, int y) {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() == gui_thread_id_) {
        ShowAt(x, y);
        return;
    }
    // REQ-R10 marshaling, SEC-ADJ: two-int payload in the message parameters.
    if (::PostMessageW(hwnd_, kShowMessage, PackShowX(x), PackShowY(y)) == FALSE) {
        DIAG_F("ABOUT/Show/001: PostMessage kShowMessage failed (GLE %lu); About not shown\n",
               ::GetLastError());
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

    hovered_ctl_ = -1;
    ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, PhysW(), PhysH(), SWP_SHOWWINDOW);
    ::SetForegroundWindow(hwnd_); // activatable: keyboard focus for ESC
    ::SetFocus(hwnd_);
    visible_ = true;

    Render();
    UpdateLayered();
}

void AboutWindow::Dismiss() {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() != gui_thread_id_) {
        ::PostMessageW(hwnd_, kDismissMessage, 0, 0);
        return;
    }
    if (!visible_.load(std::memory_order_relaxed)) return;
    ::KillTimer(hwnd_, kResetFeedbackTimerId);
    reset_feedback_until_ = 0;
    visible_ = false;
    hovered_ctl_ = -1;
    ::ShowWindow(hwnd_, SW_HIDE);
}

void AboutWindow::RequestLocaleRefresh() {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() != gui_thread_id_) {
        ::PostMessageW(hwnd_, kLocaleRefreshMessage, 0, 0);
        return;
    }
    ::SetWindowTextW(hwnd_, I18n::Get(StringId::AboutTitle).c_str());
    ApplyLocaleFormatting();
    if (visible_.load(std::memory_order_relaxed)) {
        Render();
        UpdateLayered();
    }
}

// P4 Batch B-5 (design §2.2.3): bind the UI-locale-dependent text formats to
// the active locale. The three intro prose formats (body/tagline/etymology)
// take the RTL/LTR reading direction; every format that paints localized copy
// gets the Q5-A font-fallback localeName (clone-swap, verified readback).
// Chrome formats (version/percent/close/copyright) stay LTR/neutral-locale.
void AboutWindow::ApplyLocaleFormatting() {
    const UiLocale locale = I18n::GetCurrentLocale();
    const TextDirection text_dir = DirectionForLocale(locale);
    const DWRITE_READING_DIRECTION dir = (text_dir == TextDirection::RTL)
                                             ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT
                                             : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;

    if (body_format_) body_format_->SetReadingDirection(dir);
    if (tagline_format_) tagline_format_->SetReadingDirection(dir);
    if (etymology_format_) etymology_format_->SetReadingDirection(dir);
    if (uz_head_format_) uz_head_format_->SetReadingDirection(dir);
    if (uz_sub_format_) uz_sub_format_->SetReadingDirection(dir);
    if (uz_label_format_) uz_label_format_->SetReadingDirection(dir);
    if (uz_dlgtitle_format_) uz_dlgtitle_format_->SetReadingDirection(dir);
    if (uz_value_format_) uz_value_format_->SetReadingDirection(dir);

    const std::wstring tag = LocaleTagForUi(locale);
    bool swap_failed = false;
    if (!ApplyFormatLocale(dwrite_factory_, &title_format_, tag)) swap_failed = true;
    if (!ApplyFormatLocale(dwrite_factory_, &tagline_format_, tag)) swap_failed = true;
    if (!ApplyFormatLocale(dwrite_factory_, &body_format_, tag)) swap_failed = true;
    if (!ApplyFormatLocale(dwrite_factory_, &etymology_format_, tag)) swap_failed = true;
    if (!ApplyFormatLocale(dwrite_factory_, &link_format_, tag)) swap_failed = true;
    if (!ApplyFormatLocale(dwrite_factory_, &reset_footer_format_, tag)) swap_failed = true;
    if (!ApplyFormatLocale(dwrite_factory_, &small_format_, tag)) swap_failed = true;
    if (!ApplyFormatLocale(dwrite_factory_, &uz_head_format_, tag)) swap_failed = true;
    if (!ApplyFormatLocale(dwrite_factory_, &uz_sub_format_, tag)) swap_failed = true;
    if (!ApplyFormatLocale(dwrite_factory_, &uz_label_format_, tag)) swap_failed = true;
    if (!ApplyFormatLocale(dwrite_factory_, &uz_button_format_, tag)) swap_failed = true;
    if (!ApplyFormatLocale(dwrite_factory_, &uz_textbtn_format_, tag)) swap_failed = true;
    if (!ApplyFormatLocale(dwrite_factory_, &uz_dlgtitle_format_, tag)) swap_failed = true;
    if (!ApplyFormatLocale(dwrite_factory_, &uz_value_format_, tag)) swap_failed = true;
    if (swap_failed) {
        DIAG_F("ABOUT/ApplyLocaleFormatting/001: localeName clone-swap failed for tag '%ls'; keeping previous formats\n",
               tag.c_str());
    }
    DIAG_LOG("UI", "about locale_dir=%s locale=%s",
             text_dir == TextDirection::RTL ? "rtl" : "ltr",
             std::string(I18n::GetLocaleCode()).c_str());
}

AboutWindow::LocalizedContent AboutWindow::BuildLocalizedContent() {
    LocalizedContent c;
    c.title = I18n::Get(StringId::AboutTitle);
    c.tagline = I18n::Get(StringId::AboutTagline);
    c.features[0] = I18n::Get(StringId::AboutFeature0);
    c.features[1] = I18n::Get(StringId::AboutFeature1);
    c.features[2] = I18n::Get(StringId::AboutFeature2);
    c.etymology = I18n::Get(StringId::AboutEtymology);
    c.link_labels[0] = I18n::Get(StringId::AboutLinkWebsite);
    c.link_labels[1] = I18n::Get(StringId::AboutLinkContact);
    c.link_labels[2] = I18n::Get(StringId::AboutLinkReddit);
    c.contacts[0] = I18n::Get(StringId::AboutContactOrg);
    c.contacts[1] = I18n::Get(StringId::AboutContactPhone);
    c.contacts[2] = I18n::Get(StringId::AboutContactLead);
    c.reset_label = I18n::Get(StringId::AboutResetButton);
    return c;
}

void AboutWindow::OpenLink(int index) {
    if (index < 0 || index >= kNumLinks) return;
    HINSTANCE hRes = ::ShellExecuteW(hwnd_, L"open", kLinkUrls[index], nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(hRes) <= 32) {
        // Traceable error code per project convention; the URL was not opened.
        DIAG_F("ABOUT/OpenLink/001: ShellExecuteW failed (INT_PTR %lld) for %ls\n",
                static_cast<long long>(reinterpret_cast<INT_PTR>(hRes)), kLinkUrls[index]);
    }
}

std::wstring AboutWindow::ReplaceToken(std::wstring text, std::wstring_view token,
                                       std::wstring_view value) {
    if (token.empty()) return text;
    size_t pos = 0;
    while ((pos = text.find(token, pos)) != std::wstring::npos) {
        text.replace(pos, token.size(), value);
        pos += value.size();
    }
    return text;
}

float AboutWindow::MeasureTextWidth(IDWriteTextFormat* format, const std::wstring& text) {
    if (!format || !dwrite_factory_ || text.empty()) return 0.0f;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dwrite_factory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                                 format, 1000.0f, 100.0f, &layout)) ||
        !layout) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS m = {};
    const HRESULT hr = layout->GetMetrics(&m);
    layout->Release();
    return SUCCEEDED(hr) ? m.width : 0.0f;
}

// REQ-UC: run the whole zone-state transition on the GUI thread (timers +
// repaint). Callers: the WndProc handlers + the click handlers.
void AboutWindow::SetZoneState(UpdateZoneState state) {
    zone_state_ = state;
    if (state == UpdateZoneState::Checking) {
        checking_started_ = ::GetTickCount64();
    }
    if (state == UpdateZoneState::Downloading) {
        displayed_pct_ = 0.0f;
    }
    if (state == UpdateZoneState::Available) {
        dlg_focus_ = 1; // Enter == Download (design spec §8)
    }
    if (hwnd_) {
        const bool animate = (state == UpdateZoneState::Checking ||
                              state == UpdateZoneState::Downloading);
        if (animate) {
            ::SetTimer(hwnd_, kUpdateAnimTimerId, 50, nullptr);
        } else {
            ::KillTimer(hwnd_, kUpdateAnimTimerId);
        }
        if (state == UpdateZoneState::UpToDate) {
            ::SetTimer(hwnd_, kUpToDateReturnTimerId, kUpToDateReturnMs, nullptr);
        } else {
            ::KillTimer(hwnd_, kUpToDateReturnTimerId);
        }
    }
    if (visible_.load(std::memory_order_relaxed)) {
        Render();
        UpdateLayered();
    }
}

// REQ-UC: detached worker runs CheckForUpdate (blocking WinHTTP, never on the
// GUI thread) and wakes the WndProc with a pure (0,0) message.
void AboutWindow::StartUpdateCheck() {
    if (!hwnd_) return;
    if (zone_state_ == UpdateZoneState::Checking ||
        zone_state_ == UpdateZoneState::Downloading) {
        return; // one update operation at a time
    }
    auto job = std::make_shared<UpdateJob>();
    job_ = job;
    const HWND hwnd = hwnd_;
    std::thread([hwnd, job]() {
        UpdateCheckOutcome result = CheckForUpdate(job->cancel);
        {
            std::lock_guard<std::mutex> lk(job->m);
            job->check_status = result.status;
            job->release = std::move(result.release);
            job->check_done = true;
        }
        ::PostMessageW(hwnd, AboutWindow::kUpdateCheckDoneMessage, 0, 0);
    }).detach();
    SetZoneState(UpdateZoneState::Checking);
}

void AboutWindow::HandleUpdateCheckDone() {
    const std::shared_ptr<UpdateJob> job = job_;
    if (!job) return;
    UpdateCheckStatus status = UpdateCheckStatus::Offline;
    updatelogic::ReleaseInfo info;
    {
        std::lock_guard<std::mutex> lk(job->m);
        if (!job->check_done) return;
        status = job->check_status;
        info = std::move(job->release);
    }
    switch (status) {
        case UpdateCheckStatus::UpToDate:
            SetZoneState(UpdateZoneState::UpToDate);
            break;
        case UpdateCheckStatus::UpdateAvailable:
            pending_release_ = std::move(info);
            SetZoneState(UpdateZoneState::Available);
            break;
        case UpdateCheckStatus::RateLimited:
            SetZoneState(UpdateZoneState::ErrRate);
            break;
        default:
            SetZoneState(UpdateZoneState::ErrOffline);
            break;
    }
}

// REQ-UC: download the picked asset to %TEMP% + SHA-256 verify (fail-closed
// against the release-notes hash line). Partial files never survive.
void AboutWindow::StartDownload() {
    const updatelogic::ReleaseAsset* asset = PickInstallerAsset(pending_release_);
    wchar_t temp[MAX_PATH] = {};
    if (!asset || !::GetTempPathW(MAX_PATH, temp)) {
        SetZoneState(UpdateZoneState::ErrOffline); // nothing pickable: quiet retry state
        return;
    }
    const std::wstring dest = updatelogic::BuildDownloadTargetPath(temp, pending_release_.tag);
    const std::optional<std::string> expected =
        updatelogic::ExtractSha256FromNotes(pending_release_.body);

    auto job = std::make_shared<UpdateJob>();
    job_ = job;
    const HWND hwnd = hwnd_;
    const std::string url = asset->download_url;
    const std::uint64_t advertised_size = asset->size; // SEC cap cross-check input
    std::thread([hwnd, job, url, dest, expected, advertised_size]() {
        const UpdateDownloadStatus status = DownloadUpdate(
            url, dest, advertised_size, expected,
            [job](std::uint64_t received, std::uint64_t total) {
                job->received.store(received, std::memory_order_relaxed);
                job->total.store(total, std::memory_order_relaxed);
            },
            job->cancel);
        {
            std::lock_guard<std::mutex> lk(job->m);
            job->download_status = status;
            job->download_path = dest;
            job->download_done = true;
        }
        ::PostMessageW(hwnd, AboutWindow::kUpdateDownloadDoneMessage, 0, 0);
    }).detach();
    SetZoneState(UpdateZoneState::Downloading);
}

void AboutWindow::HandleUpdateDownloadDone() {
    if (zone_state_ != UpdateZoneState::Downloading) return; // stale wake-up
    const std::shared_ptr<UpdateJob> job = job_;
    if (!job) return;
    UpdateDownloadStatus status = UpdateDownloadStatus::Failed;
    std::wstring path;
    {
        std::lock_guard<std::mutex> lk(job->m);
        if (!job->download_done) return;
        status = job->download_status;
        path = std::move(job->download_path);
    }
    switch (status) {
        case UpdateDownloadStatus::Ok:
            ready_path_ = std::move(path);
            SetZoneState(UpdateZoneState::Ready);
            break;
        case UpdateDownloadStatus::HashMismatch:
            SetZoneState(UpdateZoneState::ErrHash); // the only red state
            break;
        case UpdateDownloadStatus::Cancelled:
            SetZoneState(UpdateZoneState::Cancelled);
            break;
        default:
            SetZoneState(UpdateZoneState::ErrOffline); // mid-download failure: quiet retry
            break;
    }
}

// REQ-UC: launch the verified installer, then exit the app (the installer
// handles the running app). Launch failure keeps the Ready state so the user
// can retry.
void AboutWindow::EnterInstall() {
    if (ready_path_.empty()) return;
    if (!LaunchInstaller(ready_path_)) return;
    ::PostQuitMessage(0);
}

void AboutWindow::Render() {
    if (!dc_render_target_) return;

    RebindRenderTarget();

    if (!scratch_brush_) {
        EnsureScratchBrush();
        if (!scratch_brush_) return;
    }

    dc_render_target_->BeginDraw();
    dc_render_target_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    const float w = static_cast<float>(current_width_);
    const float h = static_cast<float>(current_height_);
    const bool ui_rtl = DirectionForLocale(I18n::GetCurrentLocale()) == TextDirection::RTL;
    const float cx = w / 2.0f;

    const LocalizedContent content = BuildLocalizedContent();

    // ------------------------------------------------------------------
    // (1) Card fill + border + top rim light (design spec §13).
    // ------------------------------------------------------------------
    D2D1_ROUNDED_RECT card = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f),
                                               10.0f, 10.0f);
    scratch_brush_->SetColor(kColCard);
    dc_render_target_->FillRoundedRectangle(card, scratch_brush_);
    scratch_brush_->SetColor(kColCardEdge);
    dc_render_target_->DrawRoundedRectangle(card, scratch_brush_, 1.0f);
    scratch_brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f));
    dc_render_target_->DrawLine(D2D1::Point2F(9.0f, 1.5f), D2D1::Point2F(w - 9.0f, 1.5f),
                                scratch_brush_, 1.0f);

    // ------------------------------------------------------------------
    // (2) REQ-UC brand chrome: logo tile (80 DIP, r16, 1.5 px gold border),
    // 16 pt name, 11 pt version, signature twin-gold-hairline seal.
    // ------------------------------------------------------------------
    const D2D1_RECT_F logoTile = D2D1::RectF(cx - 40.0f, 26.0f, cx + 40.0f, 106.0f);
    scratch_brush_->SetColor(kColLogoTile);
    dc_render_target_->FillRoundedRectangle(D2D1::RoundedRect(logoTile, 16.0f, 16.0f),
                                            scratch_brush_);
    scratch_brush_->SetColor(D2D1::ColorF(0xD4AF37, 0.9f));
    dc_render_target_->DrawRoundedRectangle(D2D1::RoundedRect(logoTile, 16.0f, 16.0f),
                                            scratch_brush_, 1.5f);
    const D2D1_RECT_F logoRect = D2D1::RectF(logoTile.left + 8.0f, logoTile.top + 8.0f,
                                             logoTile.right - 8.0f, logoTile.bottom - 8.0f);
    if (logo_bitmap_) {
        dc_render_target_->DrawBitmap(logo_bitmap_, logoRect, 1.0f,
                                      D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        DrawTabletLogoVector(dc_render_target_, logoRect, false);
    }

    const std::wstring titleText = I18n::Get(StringId::AppName);
    if (title_format_) {
        scratch_brush_->SetColor(kColInk);
        dc_render_target_->DrawText(titleText.c_str(), static_cast<UINT32>(titleText.size()),
                                    title_format_, D2D1::RectF(24.0f, 118.0f, w - 24.0f, 138.0f),
                                    scratch_brush_);
    }
    const std::wstring versionText = L"v" + std::wstring(kAppVersionW);
    if (version_format_) {
        scratch_brush_->SetColor(kColMut2);
        dc_render_target_->DrawText(versionText.c_str(), static_cast<UINT32>(versionText.size()),
                                    version_format_, D2D1::RectF(24.0f, 142.0f, w - 24.0f, 157.0f),
                                    scratch_brush_);
    }

    // Signature "twin gold hairline" seal (spec §4/§0): two 150 DIP lines,
    // 4 px apart, at y=176.5 / 181.5, gold 55% + 22%.
    scratch_brush_->SetColor(D2D1::ColorF(0xD4AF37, 0.55f));
    dc_render_target_->DrawLine(D2D1::Point2F(cx - 75.0f, 176.5f),
                                D2D1::Point2F(cx + 75.0f, 176.5f), scratch_brush_, 1.0f);
    scratch_brush_->SetColor(D2D1::ColorF(0xD4AF37, 0.22f));
    dc_render_target_->DrawLine(D2D1::Point2F(cx - 75.0f, 181.5f),
                                D2D1::Point2F(cx + 75.0f, 181.5f), scratch_brush_, 1.0f);

    // ------------------------------------------------------------------
    // Shared helpers for the middle region + footer.
    // ------------------------------------------------------------------
    const int hover = hovered_ctl_;

    auto measure = [&](IDWriteTextFormat* format, const std::wstring& text) -> float {
        return MeasureTextWidth(format, text);
    };

    // Button painter (design spec §5).
    auto draw_button = [&](D2D1_RECT_F rect, const std::wstring& label, UcButtonVariant variant,
                           bool hovered) {
        if (variant == kUcText) {
            scratch_brush_->SetColor(hovered ? kColGoldHi : kColMut);
            if (uz_textbtn_format_) {
                dc_render_target_->DrawText(label.c_str(), static_cast<UINT32>(label.size()),
                                            uz_textbtn_format_, rect, scratch_brush_);
            }
            return;
        }
        const D2D1_ROUNDED_RECT btn = D2D1::RoundedRect(rect, 6.0f, 6.0f);
        switch (variant) {
            case kUcPrimary:
                scratch_brush_->SetColor(hovered ? kColGoldHi : kColGold);
                dc_render_target_->FillRoundedRectangle(btn, scratch_brush_);
                scratch_brush_->SetColor(D2D1::ColorF(0xD4AF37, 0.0f));
                dc_render_target_->DrawRoundedRectangle(btn, scratch_brush_, 1.0f);
                scratch_brush_->SetColor(kColOnGold);
                break;
            case kUcDanger:
                scratch_brush_->SetColor(D2D1::ColorF(0xE5645E, hovered ? 0.22f : 0.10f));
                dc_render_target_->FillRoundedRectangle(btn, scratch_brush_);
                scratch_brush_->SetColor(kColDanger);
                dc_render_target_->DrawRoundedRectangle(btn, scratch_brush_, 1.0f);
                scratch_brush_->SetColor(kColDangerInk);
                break;
            default: // kUcSecondary
                scratch_brush_->SetColor(D2D1::ColorF(0x33507E, hovered ? 0.42f : 0.28f));
                dc_render_target_->FillRoundedRectangle(btn, scratch_brush_);
                scratch_brush_->SetColor(kColLapisHi);
                dc_render_target_->DrawRoundedRectangle(btn, scratch_brush_, 1.0f);
                scratch_brush_->SetColor(kColOnLapis);
                break;
        }
        if (uz_button_format_) {
            dc_render_target_->DrawText(label.c_str(), static_cast<UINT32>(label.size()),
                                        uz_button_format_, rect, scratch_brush_);
        }
    };

    // Content-sized secondary button rect: max(96, text + 40), 32 tall.
    auto plan_button = [&](const std::wstring& label, float center_x, float top) -> D2D1_RECT_F {
        const float tw = measure(uz_button_format_, label);
        const float bw = tw + 40.0f > 96.0f ? tw + 40.0f : 96.0f;
        return D2D1::RectF(center_x - bw / 2.0f, top, center_x + bw / 2.0f, top + 32.0f);
    };

    // Hand-drawn status glyphs (spec §9 "quiet": no icon over 16 DIP).
    auto draw_glyph = [&](float x, float y, UcGlyph glyph, const D2D1_COLOR_F& color) {
        scratch_brush_->SetColor(color);
        switch (glyph) {
            case kGlyphCheck: {
                dc_render_target_->DrawLine(D2D1::Point2F(x + 3.5f, y + 8.5f),
                                            D2D1::Point2F(x + 6.5f, y + 11.5f),
                                            scratch_brush_, 1.4f);
                dc_render_target_->DrawLine(D2D1::Point2F(x + 6.5f, y + 11.5f),
                                            D2D1::Point2F(x + 12.5f, y + 4.5f),
                                            scratch_brush_, 1.4f);
                break;
            }
            case kGlyphClock: {
                const D2D1_RECT_F r = D2D1::RectF(x + 1.0f, y + 1.0f, x + 15.0f, y + 15.0f);
                dc_render_target_->DrawEllipse(D2D1::Ellipse(
                    D2D1::Point2F((r.left + r.right) / 2.0f, (r.top + r.bottom) / 2.0f),
                    7.0f, 7.0f), scratch_brush_, 1.2f);
                const D2D1_POINT_2F c = D2D1::Point2F((r.left + r.right) / 2.0f,
                                                      (r.top + r.bottom) / 2.0f);
                dc_render_target_->DrawLine(c, D2D1::Point2F(c.x, c.y - 4.0f),
                                            scratch_brush_, 1.2f);
                dc_render_target_->DrawLine(c, D2D1::Point2F(c.x + 3.0f, c.y),
                                            scratch_brush_, 1.2f);
                break;
            }
            case kGlyphCross: {
                dc_render_target_->DrawLine(D2D1::Point2F(x + 4.5f, y + 4.5f),
                                            D2D1::Point2F(x + 11.5f, y + 11.5f),
                                            scratch_brush_, 1.4f);
                dc_render_target_->DrawLine(D2D1::Point2F(x + 11.5f, y + 4.5f),
                                            D2D1::Point2F(x + 4.5f, y + 11.5f),
                                            scratch_brush_, 1.4f);
                break;
            }
            case kGlyphWarn: {
                const D2D1_POINT_2F c = D2D1::Point2F(x + 8.0f, y + 8.0f);
                dc_render_target_->DrawEllipse(D2D1::Ellipse(c, 7.0f, 7.0f),
                                               scratch_brush_, 1.2f);
                scratch_brush_->SetColor(color);
                dc_render_target_->DrawLine(D2D1::Point2F(c.x, c.y - 3.5f),
                                            D2D1::Point2F(c.x, c.y + 1.5f),
                                            scratch_brush_, 1.4f);
                const D2D1_RECT_F dot = D2D1::RectF(c.x - 0.9f, c.y + 3.6f,
                                                    c.x + 0.9f, c.y + 5.4f);
                dc_render_target_->FillRectangle(dot, scratch_brush_);
                break;
            }
        }
    };

    // Quiet status line: glyph (inline-start) + headline + optional sub line,
    // centered AS ONE GROUP. The headline/sub rects are sized to the MEASURED
    // text width so the CENTER-aligned formats center ON the text — a wide
    // clip rect would visually drift the group right (CEO screenshot 03).
    auto draw_status_line = [&](UcGlyph glyph, const D2D1_COLOR_F& glyph_color,
                                const std::wstring& headline, const D2D1_COLOR_F& head_color,
                                const std::wstring& sub_line, float top, float max_text_w,
                                IDWriteTextFormat* head_format) {
        float head_w = measure(head_format, headline);
        if (head_w > max_text_w) head_w = max_text_w;
        float sub_w = sub_line.empty() ? 0.0f : measure(uz_sub_format_, sub_line);
        if (sub_w > max_text_w) sub_w = max_text_w;
        const float text_w = head_w > sub_w ? head_w : sub_w;
        const float group_w = 16.0f + 9.0f + text_w;
        const float gx = cx - group_w / 2.0f;
        const float glyph_x = ui_rtl ? gx + group_w - 16.0f : gx;
        draw_glyph(glyph_x, top + 1.0f, glyph, glyph_color);
        const float text_x = ui_rtl ? gx : gx + 25.0f;
        scratch_brush_->SetColor(head_color);
        dc_render_target_->DrawText(headline.c_str(), static_cast<UINT32>(headline.size()),
                                    head_format,
                                    D2D1::RectF(text_x, top, text_x + head_w, top + 36.0f),
                                    scratch_brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (!sub_line.empty() && uz_sub_format_) {
            scratch_brush_->SetColor(kColMut2);
            dc_render_target_->DrawText(sub_line.c_str(), static_cast<UINT32>(sub_line.size()),
                                        uz_sub_format_,
                                        D2D1::RectF(text_x, top + 19.0f,
                                                    text_x + sub_w, top + 36.0f),
                                        scratch_brush_);
        }
    };

    // Re-center helper: the block top for a block of the given height.
    auto block_top = [&](float block_h) { return kZoneCy - block_h / 2.0f; };

    const std::wstring check_label = I18n::Get(StringId::UpdateCheck);

    // Feature measurement + pure planner run in EVERY state: Idle renders the
    // blocks; the footer anchors (base-derived) stay put while a transient
    // update state swaps the middle region.
    AboutFeatureMetrics feature_metrics[3] = {};
    if (body_format_ && dwrite_factory_) {
        for (int i = 0; i < 3; ++i) {
            IDWriteTextLayout* layout = nullptr;
            if (SUCCEEDED(dwrite_factory_->CreateTextLayout(
                    content.features[i].c_str(),
                    static_cast<UINT32>(content.features[i].size()),
                    body_format_, w - 68.0f, 1000.0f, &layout)) && layout) {
                DWRITE_TEXT_METRICS m = {};
                if (SUCCEEDED(layout->GetMetrics(&m))) {
                    feature_metrics[i].height = m.height;
                }
                layout->Release();
            }
            // Failure: leave height 0 -> planner floors the block at 40 DIP.
        }
    }
    const AboutFeaturePlan plan = PlanAboutFeatureLayout(feature_metrics, ui_rtl, w);
    const float base = plan.next_free_y + kIntroShift;

    // ------------------------------------------------------------------
    // (3) MIDDLE REGION: Idle renders the restored intro; any non-Idle
    // state swaps it for the update-zone content (CEO regression fix:
    // the intro is the star again, the flow is transient).
    // ------------------------------------------------------------------
    if (zone_state_ == UpdateZoneState::Idle) {
        // ---- Restored intro (byte-faithful to the pre-rework rendering;
        // every Y is the historical value + kIntroShift for the new chrome).
        const float iy = kIntroShift;

        // 3. Tagline (body 12, wrap, centered).
        if (tagline_format_) {
            scratch_brush_->SetColor(D2D1::ColorF(0xF2ECDC, 1.0f));  // warm sand-white
            dc_render_target_->DrawText(content.tagline.c_str(),
                                        static_cast<UINT32>(content.tagline.size()),
                                        tagline_format_,
                                        D2D1::RectF(24.0f, 162.0f + iy, w - 24.0f, 208.0f + iy),
                                        scratch_brush_);
        }

        // Divider 1
        scratch_brush_->SetColor(D2D1::ColorF(0x33507E, 0.5f));  // lapis divider
        dc_render_target_->DrawLine(D2D1::Point2F(24.0f, 218.0f + iy),
                                    D2D1::Point2F(w - 24.0f, 218.0f + iy),
                                    scratch_brush_, 1.0f);

        // 4. Features: dynamic-height blocks (260922_0001 A2). REQ-052's
        // per-block CLIP keeps a too-tall block from bleeding into the next
        // one (the historical no-clip-on-long-languages fix: the old fixed
        // 230+i*42 grid silently cut the third line of de/ru/es/vi).
        if (body_format_) {
            for (int i = 0; i < 3; ++i) {
                const float top = plan.block_top[i] + iy;
                const float bottom = top + plan.block_h[i];
                const D2D1_ROUNDED_RECT dot = D2D1::RoundedRect(
                    D2D1::RectF(plan.marker_x[i], top + 7.0f,
                                plan.marker_x[i] + 5.0f, top + 12.0f),
                    2.5f, 2.5f);
                scratch_brush_->SetColor(D2D1::ColorF(0xD9B45A, 1.0f));  // antique gold marker
                dc_render_target_->FillRoundedRectangle(dot, scratch_brush_);
                scratch_brush_->SetColor(D2D1::ColorF(0xF2ECDC, 1.0f));  // warm sand-white
                dc_render_target_->DrawText(content.features[i].c_str(),
                                            static_cast<UINT32>(content.features[i].size()),
                                            body_format_,
                                            ui_rtl ? D2D1::RectF(28.0f, top, w - 40.0f, bottom)
                                                   : D2D1::RectF(40.0f, top, w - 28.0f, bottom),
                                            scratch_brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }

        // 5. Etymology (italic 11, subtext).
        if (etymology_format_) {
            scratch_brush_->SetColor(D2D1::ColorF(0x93A3C7, 1.0f));  // lapis-gray subtext
            dc_render_target_->DrawText(content.etymology.c_str(),
                                        static_cast<UINT32>(content.etymology.size()),
                                        etymology_format_,
                                        D2D1::RectF(24.0f, base + 8.0f, w - 24.0f, base + 48.0f),
                                        scratch_brush_);
        }

        // Divider 2
        scratch_brush_->SetColor(D2D1::ColorF(0x33507E, 0.5f));  // lapis divider
        dc_render_target_->DrawLine(D2D1::Point2F(24.0f, base + 60.0f),
                                    D2D1::Point2F(w - 24.0f, base + 60.0f),
                                    scratch_brush_, 1.0f);

        // 6. Links row: 3 pill buttons centered (Website / Contact / Reddit).
        const float pill_w = 100.0f;
        const float pill_gap = 10.0f;
        const float pills_total = pill_w * kNumLinks + pill_gap * (kNumLinks - 1);
        const float pills_x = (w - pills_total) / 2.0f;
        for (int i = 0; i < kNumLinks; ++i) {
            link_rects_[i] = D2D1::RectF(pills_x + static_cast<float>(i) * (pill_w + pill_gap),
                                         base + 74.0f,
                                         pills_x + static_cast<float>(i) * (pill_w + pill_gap) + pill_w,
                                         base + 102.0f);
            const bool link_hover = (hover == kHoverLink0 + i);
            const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(link_rects_[i], 4.0f, 4.0f);
            scratch_brush_->SetColor(link_hover ? D2D1::ColorF(0x22406B, 1.0f)     // pill hover
                                                : D2D1::ColorF(0x14243F, 0.9f));   // lapis-deep pill
            dc_render_target_->FillRoundedRectangle(pill, scratch_brush_);
            scratch_brush_->SetColor(D2D1::ColorF(0xD9B45A, 1.0f));  // antique gold accent
            dc_render_target_->DrawRoundedRectangle(pill, scratch_brush_,
                                                    link_hover ? 1.4f : 1.0f);
            if (link_format_) {
                scratch_brush_->SetColor(D2D1::ColorF(0xF2ECDC, 1.0f));  // warm sand-white
                dc_render_target_->DrawText(content.link_labels[i].c_str(),
                                            static_cast<UINT32>(content.link_labels[i].size()),
                                            link_format_, link_rects_[i], scratch_brush_);
            }
        }

        // 7. Contact block (small 10.5, subtext, centered).
        if (small_format_) {
            scratch_brush_->SetColor(D2D1::ColorF(0x93A3C7, 1.0f));  // lapis-gray subtext
            for (int i = 0; i < 3; ++i) {
                const float top = base + 118.0f + static_cast<float>(i) * 22.0f;
                dc_render_target_->DrawText(content.contacts[i].c_str(),
                                            static_cast<UINT32>(content.contacts[i].size()),
                                            small_format_,
                                            D2D1::RectF(24.0f, top, w - 24.0f, top + 20.0f),
                                            scratch_brush_);
            }
        }
    } else {
        // ---- Transient update-zone content (swaps the intro region only).
        switch (zone_state_) {
        case UpdateZoneState::Checking: {
            // Spinner + label stack + quiet hint; the card stays interactive
            // (spec §7: nothing is disabled, no modal).
            const std::wstring head = I18n::Get(StringId::UpdateChecking);
            const float head_w = measure(uz_head_format_, head);
            const float group_w = 20.0f + 10.0f + head_w;
            const float gx = cx - group_w / 2.0f;
            const float row_top = block_top(62.0f);
            const D2D1_POINT_2F spin_c = D2D1::Point2F(gx + 10.0f, row_top + 10.0f);
            scratch_brush_->SetColor(D2D1::ColorF(0xD4AF37, 0.22f));
            dc_render_target_->DrawEllipse(D2D1::Ellipse(spin_c, 10.0f, 10.0f),
                                           scratch_brush_, 2.0f);
            if (spinner_arc_) {
                const float angle = static_cast<float>(
                    ((::GetTickCount64() - checking_started_) % 900) * 360 / 900);
                // The arc GEOMETRY is origin-centered (radius 10 around (0,0)).
                // Blind-QA fix: rotate around the ORIGIN first, then translate
                // to the spinner center — Rotation(angle, spin_c) alone rotated
                // the origin-centered arc IN PLACE around the spinner center,
                // so the gold arc never reached the spinner (visible only as a
                // flash at the card's top-left at angle 0).
                dc_render_target_->SetTransform(
                    D2D1::Matrix3x2F::Rotation(angle) *
                    D2D1::Matrix3x2F::Translation(spin_c.x, spin_c.y));
                scratch_brush_->SetColor(kColGold);
                dc_render_target_->DrawGeometry(spinner_arc_, scratch_brush_, 2.0f);
                dc_render_target_->SetTransform(D2D1::Matrix3x2F::Identity());
            }
            scratch_brush_->SetColor(kColInk);
            dc_render_target_->DrawText(head.c_str(), static_cast<UINT32>(head.size()),
                                        uz_head_format_,
                                        D2D1::RectF(gx + 30.0f, row_top, gx + 40.0f + head_w,
                                                    row_top + 20.0f),
                                        scratch_brush_);
            const std::wstring hint = I18n::Get(StringId::UpdateCheckingHint);
            if (uz_sub_format_) {
                scratch_brush_->SetColor(kColMut);
                dc_render_target_->DrawText(hint.c_str(), static_cast<UINT32>(hint.size()),
                                            uz_sub_format_,
                                            D2D1::RectF(cx - 170.0f, row_top + 40.0f,
                                                        cx + 170.0f, row_top + 74.0f),
                                            scratch_brush_);
            }
            break;
        }
        case UpdateZoneState::UpToDate: {
            // Gold-outlined check + headline + bare "vX.Y.Z" subline, centered
            // as one group (no leading middle dot); auto-returns to Idle on a
            // timer or a click inside the ZONE band only.
            const std::wstring head = I18n::Get(StringId::UpdateUptodate);
            const std::wstring ver = L"v" + std::wstring(kAppVersionW);
            draw_status_line(kGlyphCheck, kColGold, head, kColInk, ver,
                             block_top(36.0f), 300.0f, uz_head_format_);
            break;
        }
        case UpdateZoneState::Available: {
            // The zone itself is dimmed behind the scrim; the consent dialog
            // renders in the layer below.
            break;
        }
        case UpdateZoneState::Downloading: {
            // Progress UI (spec §6): label+percent, track+fill, meta, cancel.
            // The head/meta rows mirror in RTL (label inline-start); the fill
            // itself stays physically LTR like Windows progress bars.
            const std::shared_ptr<UpdateJob> job = job_;
            const std::uint64_t received = job ? job->received.load(std::memory_order_relaxed) : 0;
            const std::uint64_t total = job ? job->total.load(std::memory_order_relaxed) : 0;
            const float top = block_top(89.0f);
            const float x0 = cx - 158.0f; // 316 DIP wrap, centered
            const std::wstring label = I18n::Get(StringId::UpdateDlProgress);
            const float label_left = ui_rtl ? x0 + 56.0f : x0;
            const float pct_left = ui_rtl ? x0 : x0 + 316.0f - 48.0f;
            scratch_brush_->SetColor(kColInk);
            dc_render_target_->DrawText(label.c_str(), static_cast<UINT32>(label.size()),
                                        uz_value_format_,
                                        D2D1::RectF(label_left, top, x0 + 316.0f, top + 18.0f),
                                        scratch_brush_);
            wchar_t pct_buf[16] = {};
            std::swprintf(pct_buf, 16, L"%d%%", static_cast<int>(displayed_pct_));
            scratch_brush_->SetColor(kColGoldHi);
            dc_render_target_->DrawText(pct_buf, static_cast<UINT32>(std::wcslen(pct_buf)),
                                        uz_pct_format_,
                                        D2D1::RectF(pct_left, top, pct_left + 48.0f, top + 18.0f),
                                        scratch_brush_);
            const D2D1_RECT_F track = D2D1::RectF(x0, top + 26.0f, x0 + 316.0f, top + 34.0f);
            scratch_brush_->SetColor(kColTrack);
            dc_render_target_->FillRoundedRectangle(D2D1::RoundedRect(track, 4.0f, 4.0f),
                                                    scratch_brush_);
            scratch_brush_->SetColor(D2D1::ColorF(0x4A6CA3, 0.55f));
            dc_render_target_->DrawRoundedRectangle(D2D1::RoundedRect(track, 4.0f, 4.0f),
                                                    scratch_brush_, 1.0f);
            if (progress_brush_) {
                const float fill_w = (displayed_pct_ / 100.0f) * 314.0f;
                if (fill_w > 0.0f) {
                    progress_brush_->SetStartPoint(D2D1::Point2F(x0 + 1.0f, 0.0f));
                    progress_brush_->SetEndPoint(D2D1::Point2F(x0 + 315.0f, 0.0f));
                    dc_render_target_->FillRoundedRectangle(
                        D2D1::RoundedRect(
                            D2D1::RectF(x0 + 1.0f, top + 27.0f, x0 + 1.0f + fill_w,
                                        top + 33.0f),
                            3.0f, 3.0f),
                        progress_brush_);
                }
            }
            const std::wstring meta = updatelogic::FormatProgressMb(received, total);
            const float meta_left_x = ui_rtl ? x0 + 116.0f : x0;
            const float meta_right_x = ui_rtl ? x0 : x0 + 116.0f;
            scratch_brush_->SetColor(kColMut2);
            dc_render_target_->DrawText(meta.c_str(), static_cast<UINT32>(meta.size()),
                                        uz_label_format_,
                                        D2D1::RectF(meta_left_x, top + 42.0f,
                                                    meta_left_x + 200.0f, top + 57.0f),
                                        scratch_brush_);
            scratch_brush_->SetColor(kColMut2);
            dc_render_target_->DrawText(label.c_str(), static_cast<UINT32>(label.size()),
                                        uz_label_format_,
                                        D2D1::RectF(meta_right_x, top + 42.0f,
                                                    meta_right_x + 200.0f, top + 57.0f),
                                        scratch_brush_);
            const std::wstring cancel = I18n::Get(StringId::UpdateDlCancel);
            const float cw = measure(uz_textbtn_format_, cancel) + 12.0f;
            dl_cancel_rect_ = D2D1::RectF(cx - cw / 2.0f - 4.0f, top + 66.0f - 4.0f,
                                          cx + cw / 2.0f + 4.0f, top + 82.0f + 4.0f);
            scratch_brush_->SetColor(hover == kHoverDlCancel ? kColGoldHi : kColMut);
            if (uz_textbtn_format_) {
                dc_render_target_->DrawText(cancel.c_str(), static_cast<UINT32>(cancel.size()),
                                            uz_textbtn_format_,
                                            D2D1::RectF(cx - cw / 2.0f, top + 66.0f,
                                                        cx + cw / 2.0f, top + 82.0f),
                                            scratch_brush_);
            }
            break;
        }
        case UpdateZoneState::Ready: {
            // Twin-ring gold seal + check + headline + warn + [Later]/[Install now].
            const float top = block_top(140.0f);
            const D2D1_POINT_2F seal_c = D2D1::Point2F(cx, top + 22.0f);
            scratch_brush_->SetColor(kColGold);
            dc_render_target_->DrawEllipse(D2D1::Ellipse(seal_c, 22.0f, 22.0f),
                                           scratch_brush_, 1.5f);
            scratch_brush_->SetColor(D2D1::ColorF(0xD4AF37, 0.28f));
            dc_render_target_->DrawEllipse(D2D1::Ellipse(seal_c, 18.0f, 18.0f),
                                           scratch_brush_, 1.0f);
            scratch_brush_->SetColor(kColGold);
            dc_render_target_->DrawLine(D2D1::Point2F(seal_c.x - 6.0f, seal_c.y),
                                        D2D1::Point2F(seal_c.x - 1.5f, seal_c.y + 4.5f),
                                        scratch_brush_, 1.6f);
            dc_render_target_->DrawLine(D2D1::Point2F(seal_c.x - 1.5f, seal_c.y + 4.5f),
                                        D2D1::Point2F(seal_c.x + 6.5f, seal_c.y - 5.0f),
                                        scratch_brush_, 1.6f);
            const std::wstring head = I18n::Get(StringId::UpdateReadyTitle);
            scratch_brush_->SetColor(kColInk);
            dc_render_target_->DrawText(head.c_str(), static_cast<UINT32>(head.size()),
                                        uz_head_format_,
                                        D2D1::RectF(24.0f, top + 52.0f, w - 24.0f, top + 70.0f),
                                        scratch_brush_);
            const std::wstring warn = I18n::Get(StringId::UpdateReadyWarn);
            if (uz_sub_format_) {
                scratch_brush_->SetColor(kColMut2);
                dc_render_target_->DrawText(warn.c_str(), static_cast<UINT32>(warn.size()),
                                            uz_sub_format_,
                                            D2D1::RectF(24.0f, top + 73.0f, w - 24.0f,
                                                        top + 89.0f),
                                            scratch_brush_);
            }
            const std::wstring later = I18n::Get(StringId::UpdateReadyLater);
            const std::wstring install = I18n::Get(StringId::UpdateReadyInstall);
            const float lw = measure(uz_button_format_, later) + 40.0f;
            const float iw = measure(uz_button_format_, install) + 40.0f;
            const float pair_w = lw + 8.0f + iw;
            const float by = top + 108.0f;
            // LTR: [Later][Install] centered; RTL mirrors the order (primary
            // ends up leftmost, spec §10).
            const float pair_x = cx - pair_w / 2.0f;
            const float first_x = ui_rtl ? pair_x + iw + 8.0f : pair_x;
            const float second_x = ui_rtl ? pair_x : pair_x + lw + 8.0f;
            ready_later_rect_ = D2D1::RectF(first_x, by, first_x + lw, by + 32.0f);
            ready_install_rect_ = D2D1::RectF(second_x, by, second_x + iw, by + 32.0f);
            draw_button(ready_later_rect_, later, kUcSecondary, hover == kHoverReadyLater);
            draw_button(ready_install_rect_, install, kUcPrimary, hover == kHoverReadyInstall);
            break;
        }
        case UpdateZoneState::Snoozed:
        case UpdateZoneState::Cancelled: {
            const bool snoozed = (zone_state_ == UpdateZoneState::Snoozed);
            const std::wstring line =
                I18n::Get(snoozed ? StringId::UpdateSnoozedLine : StringId::UpdateDlCancelled);
            draw_status_line(snoozed ? kGlyphClock : kGlyphCross, kColMut, line, kColMut,
                             L"", block_top(70.0f), 316.0f, uz_head_format_);
            const float top = block_top(70.0f);
            update_check_rect_ = plan_button(check_label, cx, top + 38.0f);
            draw_button(update_check_rect_, check_label, kUcSecondary,
                        hover == kHoverUpdateCheck);
            break;
        }
        case UpdateZoneState::ErrOffline:
        case UpdateZoneState::ErrRate: {
            const std::wstring line = I18n::Get(zone_state_ == UpdateZoneState::ErrRate
                                                    ? StringId::UpdateErrRate
                                                    : StringId::UpdateErrOffline);
            draw_status_line(kGlyphWarn, kColLapisHi, line, kColMut, L"",
                             block_top(70.0f), 316.0f, uz_head_format_);
            const float top = block_top(70.0f);
            update_check_rect_ = plan_button(check_label, cx, top + 38.0f);
            draw_button(update_check_rect_, check_label, kUcSecondary,
                        hover == kHoverUpdateCheck);
            break;
        }
        case UpdateZoneState::ErrHash: {
            // The ONLY red state (spec §9.10).
            const std::wstring line = I18n::Get(StringId::UpdateErrHash);
            draw_status_line(kGlyphWarn, kColDanger, line, kColDangerInk, L"",
                             block_top(70.0f), 316.0f, uz_head_format_);
            const std::wstring retry = I18n::Get(StringId::UpdateErrRetry);
            const float top = block_top(70.0f);
            retry_rect_ = plan_button(retry, cx, top + 38.0f);
            draw_button(retry_rect_, retry, kUcDanger, hover == kHoverRetry);
            break;
        }
        default:
            break;
        }
    }

    // ------------------------------------------------------------------
    // Footer: copyright at the inline-start edge, quiet update affordance
    // + reset pill toward inline-end, MIRRORED under RTL (QA §10). The row
    // is width-budgeted: the copyright gets its measured reservation, the
    // update pill its natural width, and the reset pill takes the remaining
    // space (word-ellipsis trimming keeps a long Arabic label inside its
    // slot — no overlap ever).
    // ------------------------------------------------------------------
    {
        const bool feedback = (reset_feedback_until_ != 0 &&
                               ::GetTickCount64() < reset_feedback_until_);
        const std::wstring reset_text =
            feedback ? I18n::Get(StringId::AboutResetDone) : content.reset_label;

        // Copyright reservation: the wider of its two lines, padded.
        const float copy_w = measure(copyright_format_, L"\x00A9 2026 Emebalachat Project") + 8.0f;
        const float copy_res = copy_w < 120.0f ? 120.0f : (copy_w > 180.0f ? 180.0f : copy_w);
        const float upd_w = (zone_state_ == UpdateZoneState::Idle)
                                ? measure(uz_textbtn_format_, check_label) + 28.0f
                                : 0.0f;
        const float gap = 8.0f;
        const float margin = 24.0f;
        const float reset_natural = measure(link_format_, reset_text) + 40.0f;
        float reset_w = reset_natural;
        const float max_reset = w - margin * 2.0f - copy_res - gap - (upd_w > 0.0f ? upd_w + gap : 0.0f);
        if (reset_w > max_reset) reset_w = max_reset;
        if (reset_w < 96.0f) reset_w = 96.0f;

        // Positions (LTR; mirrored by swapping edge anchors under RTL). The
        // reset pill BOTTOM stays anchored at base+224 whether it is a
        // one-line (32 DIP) or two-line (46 DIP) pill.
        float copy_x, upd_x, reset_x;
        if (!ui_rtl) {
            copy_x = margin;
            const float end_x = w - margin;
            reset_x = end_x - reset_w;
            upd_x = (upd_w > 0.0f) ? reset_x - gap - upd_w : reset_x;
        } else {
            copy_x = w - margin - copy_res;
            reset_x = margin;
            upd_x = (upd_w > 0.0f) ? reset_x + reset_w + gap : reset_x;
        }
        // Two-line pill when the localized label cannot fit one line in the
        // clamped width (Arabic reset label). Drawn with the WRAPPING
        // reset_footer_format_ (link_format_ is no-wrap for the intro pills).
        const bool reset_two_line =
            reset_natural - 40.0f > reset_w - 16.0f && reset_w < reset_natural - 6.0f;
        const float reset_h = reset_two_line ? 46.0f : 32.0f;
        const float footer_bottom = base + 224.0f;
        reset_rect_ = D2D1::RectF(reset_x, footer_bottom - reset_h,
                                  reset_x + reset_w, footer_bottom);
        // D-02: own update_check_rect_ ONLY in Idle (the footer pill). In a
        // zone state the rect was set by the zone switch for its retry/check
        // button — zeroing it here left that button unclickable and the
        // state stuck across hide/show. Leave the zone rect untouched.
        if (upd_w > 0.0f) {
            update_check_rect_ = D2D1::RectF(upd_x, reset_rect_.top + (reset_h - 28.0f) / 2.0f,
                                             upd_x + upd_w,
                                             reset_rect_.top + (reset_h + 28.0f) / 2.0f);
        }

        if (upd_w > 0.0f) {
            const D2D1_ROUNDED_RECT upd_btn =
                D2D1::RoundedRect(update_check_rect_, 6.0f, 6.0f);
            scratch_brush_->SetColor(D2D1::ColorF(0x33507E, hover == kHoverUpdateCheck ? 0.28f : 0.16f));
            dc_render_target_->FillRoundedRectangle(upd_btn, scratch_brush_);
            scratch_brush_->SetColor(kColLapisHi);
            dc_render_target_->DrawRoundedRectangle(upd_btn, scratch_brush_, 1.0f);
            if (uz_textbtn_format_) {
                scratch_brush_->SetColor(hover == kHoverUpdateCheck ? kColGoldHi : kColMut);
                dc_render_target_->DrawText(check_label.c_str(),
                                            static_cast<UINT32>(check_label.size()),
                                            uz_textbtn_format_, update_check_rect_, scratch_brush_);
            }
        }
        // Reset pill (original REQ-020 styling; wraps to two lines when the
        // localized label needs it).
        {
            const D2D1_ROUNDED_RECT btn = D2D1::RoundedRect(reset_rect_, 4.0f, 4.0f);
            scratch_brush_->SetColor(hover == kHoverReset
                                         ? D2D1::ColorF(0x22406B, 1.0f)     // pill hover
                                         : D2D1::ColorF(0x14243F, 0.9f));   // lapis-deep pill
            dc_render_target_->FillRoundedRectangle(btn, scratch_brush_);
            scratch_brush_->SetColor(D2D1::ColorF(0xD9B45A, 1.0f));  // antique gold accent
            dc_render_target_->DrawRoundedRectangle(btn, scratch_brush_,
                                                    hover == kHoverReset ? 1.4f : 1.0f);
            if (reset_footer_format_) {
                scratch_brush_->SetColor(D2D1::ColorF(0xF2ECDC, 1.0f));  // warm sand-white
                dc_render_target_->DrawText(reset_text.c_str(),
                                            static_cast<UINT32>(reset_text.size()),
                                            reset_footer_format_,
                                            D2D1::RectF(reset_rect_.left + 6.0f, reset_rect_.top,
                                                        reset_rect_.right - 6.0f, reset_rect_.bottom),
                                            scratch_brush_);
            }
        }
        if (copyright_format_) {
            scratch_brush_->SetColor(kColFaint);
            dc_render_target_->DrawText(kCopyrightText,
                                        static_cast<UINT32>(std::wcslen(kCopyrightText)),
                                        copyright_format_,
                                        D2D1::RectF(copy_x, reset_rect_.top + (reset_h - 28.0f) / 2.0f,
                                                    copy_x + copy_res,
                                                    reset_rect_.top + (reset_h + 28.0f) / 2.0f),
                                        scratch_brush_);
        }
    }

    // Close button top-right.
    close_btn_rect_ = D2D1::RectF(w - 32.0f, 12.0f, w - 12.0f, 32.0f);
    if (header_format_) {
        scratch_brush_->SetColor((hover == kHoverClose)
                                     ? D2D1::ColorF(0xEF4444, 1.0f)
                                     : D2D1::ColorF(0x93A3C7, 0.8f));
        dc_render_target_->DrawText(L"\u2715", 1, header_format_, close_btn_rect_,
                                    scratch_brush_);
    }

    // ------------------------------------------------------------------
    // (4+5) Consent dialog (spec §8): scrim over the WHOLE card, then the
    // raised panel with the twin hairline, rows, privacy note, buttons.
    // ------------------------------------------------------------------
    if (zone_state_ == UpdateZoneState::Available) {
        scratch_brush_->SetColor(kColScrim);
        dc_render_target_->FillRectangle(D2D1::RectF(0.0f, 0.0f, w, h), scratch_brush_);

        const float dlg_w = 372.0f;
        const float dlg_x = cx - dlg_w / 2.0f;

        const updatelogic::ReleaseAsset* asset = PickInstallerAsset(pending_release_);
        const std::wstring size_text = updatelogic::FormatSizeMb(asset ? asset->size : 0);
        const std::wstring note_text = I18n::Get(StringId::UpdateDlgPrivacy);

        // Note panel: fixed 2-line budget, auto-grows downward (spec §11).
        IDWriteTextLayout* note_layout = nullptr;
        float note_text_h = 32.0f;
        if (SUCCEEDED(dwrite_factory_->CreateTextLayout(
                note_text.c_str(), static_cast<UINT32>(note_text.size()),
                uz_label_format_, 293.0f, 200.0f, &note_layout)) && note_layout) {
            DWRITE_TEXT_METRICS nm = {};
            if (SUCCEEDED(note_layout->GetMetrics(&nm))) {
                note_text_h = nm.height;
            }
        }
        const float note_h = (note_text_h + 16.0f) > 48.0f ? note_text_h + 16.0f : 48.0f;

        const float dlg_h = 177.0f + note_h;
        const float dlg_y = (h - dlg_h) / 2.0f;

        const D2D1_ROUNDED_RECT dlg = D2D1::RoundedRect(
            D2D1::RectF(dlg_x, dlg_y, dlg_x + dlg_w, dlg_y + dlg_h), 10.0f, 10.0f);
        scratch_brush_->SetColor(kColDialog);
        dc_render_target_->FillRoundedRectangle(dlg, scratch_brush_);
        scratch_brush_->SetColor(D2D1::ColorF(0x5D7CB6, 0.5f));
        dc_render_target_->DrawRoundedRectangle(dlg, scratch_brush_, 1.0f);

        // Top twin hairline (the signature motif, 4 px apart).
        scratch_brush_->SetColor(D2D1::ColorF(0xD4AF37, 0.55f));
        dc_render_target_->DrawLine(D2D1::Point2F(dlg_x + 20.0f, dlg_y + 7.5f),
                                    D2D1::Point2F(dlg_x + dlg_w - 20.0f, dlg_y + 7.5f),
                                    scratch_brush_, 1.0f);
        scratch_brush_->SetColor(D2D1::ColorF(0xD4AF37, 0.22f));
        dc_render_target_->DrawLine(D2D1::Point2F(dlg_x + 20.0f, dlg_y + 11.5f),
                                    D2D1::Point2F(dlg_x + dlg_w - 20.0f, dlg_y + 11.5f),
                                    scratch_brush_, 1.0f);

        const std::wstring dlg_title = I18n::Get(StringId::UpdateDlgTitle);
        scratch_brush_->SetColor(kColInk);
        dc_render_target_->DrawText(dlg_title.c_str(), static_cast<UINT32>(dlg_title.size()),
                                    uz_dlgtitle_format_,
                                    D2D1::RectF(dlg_x, dlg_y + 22.0f, dlg_x + dlg_w,
                                                dlg_y + 40.0f),
                                    scratch_brush_);

        // Two rows: 58 DIP label column + value (12/600) with a 10.5 mut-2
        // parenthetical. Columns mirror in RTL (spec §10); the value column
        // width is geometry-fixed, never label-text-derived (spec §11).
        const float row1_y = dlg_y + 54.0f;
        const float row2_y = row1_y + 27.0f;
        const float pad_x = dlg_x + 20.0f;
        const float label_w = 58.0f;
        const float value_x = ui_rtl ? pad_x : pad_x + label_w + 8.0f;
        const float label_x = ui_rtl ? pad_x + dlg_w - 40.0f - label_w : pad_x;
        const float value_w = dlg_w - 40.0f - label_w - 8.0f;

        auto draw_row = [&](float row_y, const std::wstring& label, const std::wstring& main,
                            const std::wstring& paren) {
            scratch_brush_->SetColor(kColMut2);
            dc_render_target_->DrawText(label.c_str(), static_cast<UINT32>(label.size()),
                                        uz_label_format_,
                                        D2D1::RectF(label_x, row_y, label_x + label_w,
                                                    row_y + 16.0f),
                                        scratch_brush_);
            const float main_w = measure(uz_value_format_, main);
            const float paren_w = paren.empty() ? 0.0f : measure(uz_label_format_, paren);
            const bool paren_below = !paren.empty() &&
                                     main_w + 4.0f + paren_w > value_w;
            scratch_brush_->SetColor(kColInk);
            dc_render_target_->DrawText(main.c_str(), static_cast<UINT32>(main.size()),
                                        uz_value_format_,
                                        D2D1::RectF(value_x, row_y, value_x + value_w,
                                                    row_y + 36.0f),
                                        scratch_brush_);
            if (!paren.empty()) {
                scratch_brush_->SetColor(kColMut2);
                dc_render_target_->DrawText(
                    paren.c_str(), static_cast<UINT32>(paren.size()), uz_label_format_,
                    D2D1::RectF(value_x, row_y + (paren_below ? 18.0f : 1.0f),
                                value_x + value_w, row_y + 34.0f),
                    scratch_brush_);
            }
        };

        const std::wstring ver_label = I18n::Get(StringId::UpdateDlgVersion);
        const std::wstring ver_paren = ReplaceToken(
            I18n::Get(StringId::UpdateDlgVersionCur), L"{v}",
            L"v" + std::wstring(kAppVersionW));
        draw_row(row1_y, ver_label, ToUtf16(pending_release_.tag), ver_paren);

        const std::wstring size_label = I18n::Get(StringId::UpdateDlgSize);
        const std::wstring size_main = ReplaceToken(I18n::Get(StringId::UpdateDlgSizeApprox),
                                                    L"{s}", size_text);
        draw_row(row2_y, size_label, size_main, L"");

        // Privacy note panel (the GitHub notice — the consent surface).
        const float note_y = row2_y + 32.0f;
        const D2D1_ROUNDED_RECT note = D2D1::RoundedRect(
            D2D1::RectF(pad_x, note_y, pad_x + dlg_w - 40.0f, note_y + note_h), 6.0f, 6.0f);
        scratch_brush_->SetColor(D2D1::ColorF(0x33507E, 0.14f));
        dc_render_target_->FillRoundedRectangle(note, scratch_brush_);
        scratch_brush_->SetColor(D2D1::ColorF(0x33507E, 0.35f));
        dc_render_target_->DrawRoundedRectangle(note, scratch_brush_, 1.0f);
        // Flask glyph (inline-start), hand-drawn, 12 DIP, lapis-hi.
        {
            const float fx = pad_x + 10.0f;
            const float fy = note_y + 9.0f;
            scratch_brush_->SetColor(kColLapisHi);
            dc_render_target_->DrawLine(D2D1::Point2F(fx + 4.0f, fy),
                                        D2D1::Point2F(fx + 4.0f, fy + 3.0f),
                                        scratch_brush_, 1.2f);
            dc_render_target_->DrawLine(D2D1::Point2F(fx + 8.0f, fy),
                                        D2D1::Point2F(fx + 8.0f, fy + 3.0f),
                                        scratch_brush_, 1.2f);
            dc_render_target_->DrawLine(D2D1::Point2F(fx + 4.0f, fy + 3.0f),
                                        D2D1::Point2F(fx + 1.5f, fy + 10.0f),
                                        scratch_brush_, 1.2f);
            dc_render_target_->DrawLine(D2D1::Point2F(fx + 8.0f, fy + 3.0f),
                                        D2D1::Point2F(fx + 10.5f, fy + 10.0f),
                                        scratch_brush_, 1.2f);
            dc_render_target_->DrawLine(D2D1::Point2F(fx + 1.5f, fy + 10.0f),
                                        D2D1::Point2F(fx + 10.5f, fy + 10.0f),
                                        scratch_brush_, 1.2f);
        }
        if (note_layout) {
            scratch_brush_->SetColor(kColMut2);
            dc_render_target_->DrawTextLayout(
                D2D1::Point2F(pad_x + 10.0f + 12.0f + 7.0f, note_y + 8.0f), note_layout,
                scratch_brush_);
            note_layout->Release();
        }

        // Footer buttons: [Later] secondary + [Download] primary, end-aligned
        // with an 8 DIP gap; the order mirrors in RTL (spec §10).
        const std::wstring later = I18n::Get(StringId::UpdateDlgLater);
        const std::wstring download = I18n::Get(StringId::UpdateDlgDownload);
        const float lw = measure(uz_button_format_, later) + 40.0f;
        const float dw = measure(uz_button_format_, download) + 40.0f;
        const float btn_y = note_y + note_h + 16.0f;
        const float end_x = pad_x + dlg_w - 40.0f;
        // LTR: Later then Download right-to-left; RTL: Download leftmost.
        const float later_x = ui_rtl ? end_x - (lw + 8.0f + dw) : end_x - lw;
        const float download_x = ui_rtl ? end_x - dw : end_x - (lw + 8.0f + dw);
        dlg_later_rect_ = D2D1::RectF(later_x, btn_y, later_x + lw, btn_y + 32.0f);
        dlg_download_rect_ = D2D1::RectF(download_x, btn_y, download_x + dw, btn_y + 32.0f);
        draw_button(dlg_later_rect_, later, kUcSecondary, hover == kHoverDlgLater);
        draw_button(dlg_download_rect_, download, kUcPrimary, hover == kHoverDlgDownload);

        // (6) Focus ring on top of everything (spec §5): 2 px gold-hi, 2 DIP
        // offset around the focused dialog button.
        const D2D1_RECT_F& focus_rect = (dlg_focus_ == 0) ? dlg_later_rect_ : dlg_download_rect_;
        scratch_brush_->SetColor(kColGoldHi);
        dc_render_target_->DrawRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(focus_rect.left - 2.0f, focus_rect.top - 2.0f,
                            focus_rect.right + 2.0f, focus_rect.bottom + 2.0f),
                8.0f, 8.0f),
            scratch_brush_, 2.0f);
    }

    const HRESULT hr = dc_render_target_->EndDraw();
    if (IsRecoverableDeviceLost(hr)) {
        RecreateAfterDeviceLost();
    }
}

void AboutWindow::RecreateAfterDeviceLost() {
    DIAG_F("ABOUT/DeviceLost/001: D2DERR_RECREATE_TARGET; recreating render target\n");
    ReleaseScratchBrush(); // brush + gradient are device-dependent
    renderer_.ReleaseTarget(&dc_render_target_);
    if (!d2d_factory_) {
        return;
    }
    if (!renderer_.CreateTarget(d2d_factory_, &dc_render_target_)) {
        DIAG_F("ABOUT/DeviceLost/002: render-target recreation failed; About stays stale until next Create()\n");
        return;
    }
    ReallocateBuffer(PhysW(), PhysH());
    LoadLogoBitmap();
    EnsureScratchBrush(); // rebuild the scratch brush + gradient on the fresh target
}

void AboutWindow::UpdateLayered() {
    // REQ-R15: physical blit size; alpha fixed 245 (~96% opacity).
    renderer_.Present(hwnd_, PhysW(), PhysH(), 245);
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
        case kShowMessage: {
            pThis->ShowAt(ShowXFromWParam(wParam), ShowYFromLParam(lParam));
            return 0;
        }

        case kDismissMessage: {
            // REQ-052: while the reset-confirmation MessageBox is up, a click
            // on the box itself is an outside-click for this card.
            if (pThis->confirm_pending_) {
                return 0;
            }
            pThis->Dismiss();
            return 0;
        }

        case kLocaleRefreshMessage: {
            pThis->RequestLocaleRefresh();
            return 0;
        }

        // REQ-UC: pure (0,0) worker wake-ups; outcomes ride the UpdateJob slot.
        case kUpdateCheckDoneMessage: {
            pThis->HandleUpdateCheckDone();
            return 0;
        }
        case kUpdateDownloadDoneMessage: {
            pThis->HandleUpdateDownloadDone();
            return 0;
        }

        case WM_SETCURSOR: {
            POINT pt = {};
            ::GetCursorPos(&pt);
            ::ScreenToClient(hwnd, &pt);
            const float x = static_cast<float>(emebalachat::ui::ScalePixelsToDips(pt.x, pThis->dpi_));
            const float y = static_cast<float>(emebalachat::ui::ScalePixelsToDips(pt.y, pThis->dpi_));
            bool interactive = IsPointInRect(pThis->close_btn_rect_, x, y) ||
                               IsPointInRect(pThis->reset_rect_, x, y);
            if (!interactive && pThis->zone_state_ == UpdateZoneState::Idle) {
                for (int i = 0; i < kNumLinks; ++i) {
                    if (IsPointInRect(pThis->link_rects_[i], x, y)) {
                        interactive = true;
                        break;
                    }
                }
                interactive = interactive ||
                              IsPointInRect(pThis->update_check_rect_, x, y);
            }
            if (!interactive) {
                switch (pThis->zone_state_) {
                    case UpdateZoneState::Available:
                        interactive = IsPointInRect(pThis->dlg_later_rect_, x, y) ||
                                      IsPointInRect(pThis->dlg_download_rect_, x, y);
                        break;
                    case UpdateZoneState::Downloading:
                        interactive = IsPointInRect(pThis->dl_cancel_rect_, x, y);
                        break;
                    case UpdateZoneState::Ready:
                        interactive = IsPointInRect(pThis->ready_later_rect_, x, y) ||
                                      IsPointInRect(pThis->ready_install_rect_, x, y);
                        break;
                    case UpdateZoneState::ErrHash:
                        interactive = IsPointInRect(pThis->retry_rect_, x, y);
                        break;
                    default:
                        break;
                }
            }
            ::SetCursor(::LoadCursorW(nullptr,
                interactive ? MAKEINTRESOURCEW(32649) /* hand */ : MAKEINTRESOURCEW(32512) /* arrow */));
            return TRUE;
        }

        case WM_MOUSEMOVE: {
            const float x = static_cast<float>(emebalachat::ui::ScalePixelsToDips(
                static_cast<int>(static_cast<short>(LOWORD(lParam))), pThis->dpi_));
            const float y = static_cast<float>(emebalachat::ui::ScalePixelsToDips(
                static_cast<int>(static_cast<short>(HIWORD(lParam))), pThis->dpi_));

            int hover = -1;
            if (IsPointInRect(pThis->close_btn_rect_, x, y)) {
                hover = kHoverClose;
            } else if (IsPointInRect(pThis->reset_rect_, x, y)) {
                hover = kHoverReset;
            } else if (pThis->zone_state_ == UpdateZoneState::Idle &&
                       IsPointInRect(pThis->update_check_rect_, x, y)) {
                hover = kHoverUpdateCheck;
            } else if (pThis->zone_state_ == UpdateZoneState::Idle) {
                for (int i = 0; i < kNumLinks; ++i) {
                    if (IsPointInRect(pThis->link_rects_[i], x, y)) {
                        hover = kHoverLink0 + i;
                        break;
                    }
                }
            } else {
                switch (pThis->zone_state_) {
                    case UpdateZoneState::Available:
                        if (IsPointInRect(pThis->dlg_download_rect_, x, y)) hover = kHoverDlgDownload;
                        else if (IsPointInRect(pThis->dlg_later_rect_, x, y)) hover = kHoverDlgLater;
                        break;
                    case UpdateZoneState::Downloading:
                        if (IsPointInRect(pThis->dl_cancel_rect_, x, y)) hover = kHoverDlCancel;
                        break;
                    case UpdateZoneState::Ready:
                        if (IsPointInRect(pThis->ready_install_rect_, x, y)) hover = kHoverReadyInstall;
                        else if (IsPointInRect(pThis->ready_later_rect_, x, y)) hover = kHoverReadyLater;
                        break;
                    case UpdateZoneState::ErrHash:
                        if (IsPointInRect(pThis->retry_rect_, x, y)) hover = kHoverRetry;
                        break;
                    default:
                        break;
                }
            }

            if (hover != pThis->hovered_ctl_) {
                pThis->hovered_ctl_ = hover;
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
            if (pThis->hovered_ctl_ != -1) {
                pThis->hovered_ctl_ = -1;
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
            // Intro link pills (Idle only — they are not rendered in flow
            // states, and the zone swap owns the middle region then).
            if (pThis->zone_state_ == UpdateZoneState::Idle) {
                for (int i = 0; i < kNumLinks; ++i) {
                    if (IsPointInRect(pThis->link_rects_[i], x, y)) {
                        pThis->OpenLink(i);
                        return 0; // WM_KILLFOCUS dismisses
                    }
                }
                if (IsPointInRect(pThis->update_check_rect_, x, y)) {
                    pThis->StartUpdateCheck();
                    return 0;
                }
            }
            // Footer reset pill: same REQ-020 callback + REQ-052 confirm gate
            // as the historical button.
            if (IsPointInRect(pThis->reset_rect_, x, y)) {
                std::wstring confirm_body = I18n::Get(StringId::AboutResetButton);
                confirm_body += L'?';
                pThis->confirm_pending_ = true;
                const int confirm = ::MessageBoxW(
                    hwnd, confirm_body.c_str(), I18n::Get(StringId::AppName).c_str(),
                    MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND);
                pThis->confirm_pending_ = false;
                if (confirm != IDYES) {
                    return 0; // declined: state untouched, no "done" feedback
                }
                if (pThis->reset_callback_) {
                    pThis->reset_callback_();
                }
                pThis->reset_feedback_until_ = ::GetTickCount64() + kResetFeedbackMs;
                ::SetTimer(hwnd, kResetFeedbackTimerId, kResetFeedbackMs, nullptr);
                pThis->Render();
                pThis->UpdateLayered();
                return 0;
            }

            switch (pThis->zone_state_) {
                case UpdateZoneState::Snoozed:
                case UpdateZoneState::Cancelled:
                case UpdateZoneState::ErrOffline:
                case UpdateZoneState::ErrRate:
                    if (IsPointInRect(pThis->update_check_rect_, x, y)) {
                        pThis->StartUpdateCheck();
                    }
                    break;
                case UpdateZoneState::UpToDate:
                    // Bounded to the ZONE BAND only: a click in the swapped
                    // middle region returns to Idle, but footer clicks (reset,
                    // copyright) must not be swallowed.
                    if (y >= kZoneTop && y <= kZoneBottom) {
                        ::KillTimer(hwnd, pThis->kUpToDateReturnTimerId);
                        pThis->SetZoneState(UpdateZoneState::Idle);
                    }
                    break;
                case UpdateZoneState::Available:
                    if (IsPointInRect(pThis->dlg_download_rect_, x, y)) {
                        pThis->StartDownload();
                    } else if (IsPointInRect(pThis->dlg_later_rect_, x, y)) {
                        pThis->SetZoneState(UpdateZoneState::Snoozed);
                    }
                    break;
                case UpdateZoneState::Downloading:
                    if (IsPointInRect(pThis->dl_cancel_rect_, x, y) && pThis->job_) {
                        pThis->job_->cancel.store(true, std::memory_order_relaxed);
                    }
                    break;
                case UpdateZoneState::Ready:
                    if (IsPointInRect(pThis->ready_install_rect_, x, y)) {
                        pThis->EnterInstall();
                    } else if (IsPointInRect(pThis->ready_later_rect_, x, y)) {
                        pThis->SetZoneState(UpdateZoneState::Snoozed);
                    }
                    break;
                case UpdateZoneState::ErrHash:
                    if (IsPointInRect(pThis->retry_rect_, x, y)) {
                        pThis->StartDownload(); // re-runs the download from 0
                    }
                    break;
                default:
                    break;
            }
            return 0;
        }

        case WM_KEYDOWN: {
            if (pThis->zone_state_ == UpdateZoneState::Available) {
                // Spec §8: the dialog is modal within the card — Esc == Later,
                // Enter == Download, Tab cycles the two buttons.
                if (wParam == VK_ESCAPE) {
                    pThis->SetZoneState(UpdateZoneState::Snoozed);
                    return 0;
                }
                if (wParam == VK_RETURN) {
                    pThis->StartDownload();
                    return 0;
                }
                if (wParam == VK_TAB) {
                    pThis->dlg_focus_ = (pThis->dlg_focus_ == 0) ? 1 : 0;
                    pThis->Render();
                    pThis->UpdateLayered();
                    return 0;
                }
                return 0; // swallow other keys while the dialog is up
            }
            if (wParam == VK_ESCAPE) {
                pThis->Dismiss();
                return 0;
            }
            break;
        }

        case WM_TIMER: {
            if (wParam == kResetFeedbackTimerId) {
                ::KillTimer(hwnd, kResetFeedbackTimerId);
                pThis->reset_feedback_until_ = 0;
                if (pThis->visible_.load(std::memory_order_relaxed)) {
                    pThis->Render();
                    pThis->UpdateLayered();
                }
                return 0;
            }
            if (wParam == kUpdateAnimTimerId) {
                // Spinner rotation (checking) + progress chase (downloading).
                if (pThis->zone_state_ == UpdateZoneState::Downloading && pThis->job_) {
                    const std::uint64_t total = pThis->job_->total.load(std::memory_order_relaxed);
                    const std::uint64_t received =
                        pThis->job_->received.load(std::memory_order_relaxed);
                    const float target = total > 0
                        ? (static_cast<float>(received) * 100.0f / static_cast<float>(total))
                        : pThis->displayed_pct_;
                    // 150-200 ms linear chase (spec §6).
                    pThis->displayed_pct_ += (target - pThis->displayed_pct_) * 0.3f;
                    if (pThis->displayed_pct_ > 100.0f) pThis->displayed_pct_ = 100.0f;
                }
                if (pThis->visible_.load(std::memory_order_relaxed)) {
                    pThis->Render();
                    pThis->UpdateLayered();
                }
                return 0;
            }
            if (wParam == kUpToDateReturnTimerId) {
                ::KillTimer(hwnd, kUpToDateReturnTimerId);
                if (pThis->zone_state_ == UpdateZoneState::UpToDate) {
                    pThis->SetZoneState(UpdateZoneState::Idle);
                }
                return 0;
            }
            break;
        }

        case WM_KILLFOCUS: {
            if (pThis->confirm_pending_) {
                return 0; // the reset-confirm modal owns focus for now
            }
            pThis->Dismiss();
            return 0;
        }

        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            pThis->dpi_ = emebalachat::ui::WindowDpi(hwnd);
            ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                           pThis->PhysW(), pThis->PhysH(),
                           SWP_NOZORDER | SWP_NOACTIVATE);
            pThis->ReallocateBuffer(pThis->PhysW(), pThis->PhysH());
            pThis->Render();
            pThis->UpdateLayered();
            return 0;
        }

        case WM_DESTROY:
            return 0;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace emebalachat
