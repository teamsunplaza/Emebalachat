#include "about_window.hpp"
#include "diag_logger.hpp"
#include "asset_loader.hpp"
#include "dpi.hpp"
#include "dwrite_helpers.hpp"  // REF-3.7 (session 260910_0006 T6): shared IsPointInRect / CloneFormatWithLocale
#include "../bidi_utils.hpp"  // P4 Batch B-5: DirectionForLocale / TextDirection
#include "../i18n.hpp"
#include "../unicode_utils.hpp"  // REF-3.1 (session 260910_0006 T2): EqualsIgnoreCaseAscii
#include "../version.hpp"

#include <memory>
#include <shellapi.h>
#include <string>
#include <string_view>

namespace emebalachat {

namespace {
const wchar_t kAboutClassName[] = L"Emebalachat_AboutClass";

// REF-3.1 (session 260910_0006 T2): the former file-local WcsIEqualsAscii was
// an exact transcription of the shared EqualsIgnoreCaseAscii<wchar_t> template
// in unicode_utils.hpp (same +32 A-Z fold, verbatim for every other code
// unit) and has been deleted; ApplyFormatLocale below now calls the template
// directly. DWrite canonicalizes locale tags on readback (B-2 probe datum),
// so tag equality checks must case-fold or every refresh would churn the COM
// object.
// REF-3.7 (session 260910_0006 T6, verification §10a): the former file-local
// CloneFormatWithLocale (with its B-5 "deliberate local replication" rationale
// comment) moved verbatim to the shared src/ui/dwrite_helpers.hpp after being
// verified byte-identical against tooltip.cpp's copy; the B-5 file-disjoint
// wave constraint that forced the duplication is void since both files are now
// edited together. The B-5 design §2-Q5 verdict A rationale (CREATION-ONLY
// localeName → clone-swap) travels with the helper in the shared header.

// Set (clone-swap) a live format's localeName to `tag` iff it differs. The
// swap only lands when the cloned object VERIFIABLY carries the tag
// (GetLocaleName readback) — DWrite accepts any syntactically valid tag, but
// a rejected/mangled one must never replace the working format (fail-safe,
// same contract as tooltip.cpp's B-2 in-place swap). Returns true when the
// slot ends up carrying the requested tag.
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

// Representative full BCP-47 tag for the CURRENT UI locale, from the B-3
// LocaleMapping table (design §2.1.3: bcp47_full is the DWrite localeName
// column; i18n.hpp's header comment cross-references LanguageInfo.bcp47 —
// UI-chrome vs content, two owners, no drift). Auto/unmapped resolves to the
// English pivot tag, mirroring GetLocaleCode()'s explicit "en" fallback.
std::wstring LocaleTagForUi(UiLocale locale) {
    for (const LocaleMapping& m : GetLocaleMappings()) {
        if (m.locale == locale && m.bcp47_full && m.bcp47_full[0]) {
            return std::wstring(m.bcp47_full);
        }
    }
    return L"en-US";
}

// ---- R6 Phase 5 (plan §5.2, user decision "About 다국어화"): the About body
// is now fully localized. The former hardcoded English constants (tagline,
// 3 features, etymology, link labels, contact lines) moved into the i18n
// LocalizedStrings tables (StringId::AboutTagline .. AboutContactLead) and
// are read through I18n::Get on every Render, so a live locale switch
// repaints in the new language. The English locale keeps the original REQ-005
// copy verbatim. ----

// Link URLs are factual brand data, never translated. Slot order matches the
// label StringIds (Website / Contact / Reddit). R6 Phase 5 (user decision):
// slot 2 replaced the GitHub-releases Download link with the public Reddit
// community; both emebala.org links are kept.
const wchar_t* const kLinkUrls[AboutWindow::kNumLinks] = {
    L"https://www.emebala.org/emebalachat",
    L"https://www.emebala.org/contact",
    L"https://www.reddit.com/r/emebala/",
};

// Hover index encoding for hovered_link_ (link slots 0..2, close = 3,
// reset = kHoverReset in the class header - Phase 4, REQ-020).
constexpr int kHoverClose = 3;
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

    // REF-3.6: target creation + lifetime owned by renderer_; dc_render_target_
    // stays a non-owning alias so the render code below is untouched.
    if (!renderer_.CreateTarget(d2d_factory_, &dc_render_target_)) {
        return false;
    }

    // C1: the persistent scratch brush lives with the target (created here and
    // in RecreateAfterDeviceLost, never at render entry).
    EnsureScratchBrush();

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

    // P4 Batch B-5 (design §2.2.3): bind the freshly created formats to the
    // STARTUP locale once — reading direction on the three UI-prose formats
    // and the Q5-A font-fallback localeName on the five localized-copy
    // formats (see ApplyLocaleFormatting). Without this call an auto-detected
    // RTL OS locale would render the first About show LTR until the first
    // RequestLocaleRefresh. Formats are created with localeName L""; this is
    // the single swap pass that moves them onto the real tag.
    ApplyLocaleFormatting();

    return true;
}

void AboutWindow::Destroy() {
    if (hwnd_) {
        // SEC-ADJ (Blocker-5 class fix): the old DrainMarshalQueue PeekMessageW
        // sweep here deleted heap ShowPayload pointers that the DestroyWindow
        // queue purge would otherwise have leaked. kShowMessage now carries its
        // two-int payload losslessly in WPARAM/LPARAM — no heap, no pointer,
        // nothing to free — so a still-queued show notification at teardown is
        // harmless (the sweep and the whole helper are deleted with the seam).
        // Phase 4 (REQ-020, plan §2.6): explicit reset-feedback timer teardown
        // before the window goes away. DestroyWindow would drop hwnd-scoped
        // timers anyway, but killing it here keeps the cleanup contract local
        // and auditable (Debug review focus point: no timer leak).
        ::KillTimer(hwnd_, kResetFeedbackTimerId);
        reset_feedback_until_ = 0;
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
    ReleaseScratchBrush(); // C1: brush released while its target is still alive
    renderer_.ReleaseTarget(&dc_render_target_);
    if (d2d_factory_) { d2d_factory_->Release(); d2d_factory_ = nullptr; }

    renderer_.FreeBuffer(); // also nulls the bits pointer (this site's pBits_ = nullptr)
}

// C1 (session 260910_0007 hygiene): mirror of TooltipWindow::EnsureScratchBrush
// - the About card draws through ONE persistent ID2D1SolidColorBrush created at
// Create()/device-lost recovery instead of 13 create/Release churns per Render.
void AboutWindow::EnsureScratchBrush() {
    if (!scratch_brush_ && dc_render_target_) {
        dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f),
                                                 &scratch_brush_);
    }
}

void AboutWindow::ReleaseScratchBrush() {
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

    // Plan §2.2 item 1: branded app icon via the existing asset loader.
    std::wstring path = FindAppIconPath();
    if (!path.empty()) {
        LoadWicBitmap(dc_render_target_, path, &logo_bitmap_);
    }
}

// REF-3.6: DIB (re)creation + SetDpi/BindDC tail owned by the shared renderer
// (REQ-R15: D2D DPI tracks the monitor so DIP layout rasterizes 1:1).
void AboutWindow::ReallocateBuffer(int width, int height) {
    renderer_.ReallocateBuffer(width, height, dpi_);
}

void AboutWindow::RebindRenderTarget() {
    renderer_.Rebind(PhysW(), PhysH(), dpi_, /*set_dpi=*/false);
}

// SEC-ADJ (Blocker-5 class fix): the old AboutWindow::DrainMarshalQueue()
// PeekMessageW sweep that deleted queued heap ShowPayload pointers is deleted
// with the pointer transport below — there is no heap payload left to free.

void AboutWindow::Show(int x, int y) {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() == gui_thread_id_) {
        ShowAt(x, y);
        return;
    }
    // REQ-R10 marshaling, SEC-ADJ: the two-int payload travels LOSSLESSLY in
    // the message parameters (x in WPARAM, y in LPARAM, sign-extended ints —
    // the drag_icon RequestShowAt contract; MAKELPARAM's 16-bit truncation was
    // rejected for the same multi-monitor negative-coordinate reason, see
    // about_window.hpp). No heap allocation, no pointer crosses the seam, so
    // nothing can be dereferenced or freed by a forged post; a failed post is
    // simply "the About card did not show" (nothing to clean up locally).
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
    // Phase 4 (REQ-020, plan §2.6): a reset-click "done" label must not leak
    // into the next Show. Kill the feedback timer (idempotent no-op when the
    // label is not counting down) BEFORE hiding, so a WM_TIMER can never race
    // in after this point. The next ShowAt renders the resting label.
    ::KillTimer(hwnd_, kResetFeedbackTimerId);
    reset_feedback_until_ = 0;
    visible_ = false;
    hovered_link_ = -1;
    ::ShowWindow(hwnd_, SW_HIDE);
}

// R6 Phase 6 (plan §5.2/§5.4): UI-language switch. Marshal like Dismiss (no
// heap payload). Re-apply the localized caption; repaint the body only while
// visible (a hidden window reads fresh strings on its next ShowAt -> Render).
void AboutWindow::RequestLocaleRefresh() {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() != gui_thread_id_) {
        ::PostMessageW(hwnd_, kLocaleRefreshMessage, 0, 0);
        return;
    }
    ::SetWindowTextW(hwnd_, I18n::Get(StringId::AboutTitle).c_str());
    // P4 Batch B-5 (design §2.2.3): re-derive body/tagline/etymology reading
    // direction + the UI-locale font tags BEFORE deciding whether to repaint,
    // so a HIDDEN window's next ShowAt -> Render already carries the new
    // direction (the caption repaint below only covers the visible case).
    // Runs on the GUI thread — same single-threaded DWrite affinity as Render.
    ApplyLocaleFormatting();
    if (visible_.load(std::memory_order_relaxed)) {
        Render();
        UpdateLayered();
    }
}

// P4 Batch B-5 (session 260907_0002, design §2.2.3 + §5.2 debug focus 4):
// the About window's locale-refresh formatting seam. Called from Create()
// (startup, incl. an auto-detected RTL OS locale) and from every
// RequestLocaleRefresh on the GUI thread, so measure/paint can never observe
// a stale direction or stale font tag — no reuse-across-switch hazard.
//
// Per-region rule (design §2.2.3, user verbatim "each window operates
// properly according to its own rules"): ONLY the three UI-locale prose
// formats (body/tagline/etymology) take the RTL/LTR reading direction.
// title_format_ / version_format_ render brand + factual data (localized
// brand name, "vX.Y.Z"), header_format_ renders the "✕" glyph, and the reset
// button + link labels (link_format_/small_format_) stay LTR chrome per the
// batch contract — text inside the buttons is centered, so an RTL label under
// an LTR base direction still shapes correctly glyph-run-wise (DWrite UAX #9).
// REQ-B-005 note (design §1.1.7): the title is now the localized brand; for
// RTL-script brands (AR/HE) the centered single-line text still shapes
// correctly under the LTR base direction, so title_format_ keeps LTR chrome.
void AboutWindow::ApplyLocaleFormatting() {
    const UiLocale locale = I18n::GetCurrentLocale();
    const TextDirection text_dir = DirectionForLocale(locale);
    const DWRITE_READING_DIRECTION dir = (text_dir == TextDirection::RTL)
                                             ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT
                                             : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;

    // Direction FIRST, then the locale clone-swap: CloneFormatWithLocale
    // carries Get/SetReadingDirection over, so the swapped objects inherit
    // whatever direction the live formats hold at swap time. Mutating the
    // surviving formats afterwards would be equivalent, but this order makes
    // a partially-failed swap (old format kept) keep the NEW direction too —
    // direction correctness never depends on the font-fallback bonus.
    if (body_format_) body_format_->SetReadingDirection(dir);
    if (tagline_format_) tagline_format_->SetReadingDirection(dir);
    if (etymology_format_) etymology_format_->SetReadingDirection(dir);

    // Q5-A font fallback (design §1.3.2/§2-Q5): localeName L"" -> the active
    // UI locale's representative BCP-47 tag (B-3 LocaleMapping.bcp47_full), so
    // IDWriteFontFallback::MapCharacters picks script-appropriate faces
    // (Nirmala UI, Leelawadee UI, Myanmar Text, Segoe UI Historic...). The
    // six formats that paint UI-locale COPY get the tag: the three prose
    // formats plus link_format_ (localized link labels + localized reset/"
    // done" labels), small_format_ (contact lines carry localized labels
    // like "ساعات العمل") and title_format_ (D4, session 260909_0001: the
    // title is now I18n::Get(StringId::AppName), a per-locale brand that can
    // be Hangul/Kana/CJK/Cyrillic/Thai/Arabic/Hebrew/... and needs the same
    // script-appropriate font fallback). JUDGMENT CALL, documented per
    // delegation: the localeName here is a font-resolution hint only — none
    // of these formats receives RTL reading direction (chrome stays LTR
    // above).
    // version_format_/header_format_ keep L"": ASCII "vX.Y.Z" + a single
    // dingbat glyph, no script-fallback need.
    const std::wstring tag = LocaleTagForUi(locale);
    bool swap_failed = false;
    if (!ApplyFormatLocale(dwrite_factory_, &body_format_, tag)) swap_failed = true;
    if (!ApplyFormatLocale(dwrite_factory_, &tagline_format_, tag)) swap_failed = true;
    if (!ApplyFormatLocale(dwrite_factory_, &etymology_format_, tag)) swap_failed = true;
    if (!ApplyFormatLocale(dwrite_factory_, &link_format_, tag)) swap_failed = true;
    if (!ApplyFormatLocale(dwrite_factory_, &small_format_, tag)) swap_failed = true;
    if (!ApplyFormatLocale(dwrite_factory_, &title_format_, tag)) swap_failed = true;
    if (swap_failed) {
        // Fail-safe path: the working formats survive with their previous tag
        // (L"" system-chain fallback still resolves glyphs; only the
        // script-first-face hint is lost). Direction above still applied.
        DIAG_F("ABOUT/ApplyLocaleFormatting/001: localeName clone-swap failed for tag '%ls'; keeping previous formats\n",
               tag.c_str());
    }
    // Grep-able proof the refresh-time direction decision ran (design §4.2c
    // DIAG discipline, manual-QA M4 correlation anchor).
    DIAG_LOG("UI", "about locale_dir=%s locale=%s",
             text_dir == TextDirection::RTL ? "rtl" : "ltr",
             std::string(I18n::GetLocaleCode()).c_str());
}

// R6 Phase 5 pure seam (plan §7.2): every text the card paints, resolved for
// the CURRENT locale. Render() and the unit tests share this single source.
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
    // Phase 4 (REQ-020, plan §2.5): resting label of the reset button. The
    // transient "done" label is a momentary state, read directly by Render.
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

void AboutWindow::Render() {
    if (!dc_render_target_) return;

    // REQ-R15: DIP layout authored below; BindDC rect is the physical buffer.
    RebindRenderTarget();

    // C1: the single persistent scratch brush is owned by Create()/device-lost
    // recovery, never churned here. The null-branch retry only fires after a
    // failed init (one-shot self-heal, not per-frame work). Still null: skip
    // the frame (old code could only offer a cleared empty card in that case).
    if (!scratch_brush_) {
        EnsureScratchBrush();
        if (!scratch_brush_) return;
    }

    dc_render_target_->BeginDraw();
    dc_render_target_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    const float w = static_cast<float>(current_width_);
    const float h = static_cast<float>(current_height_);

    // Outer acrylic card, same container geometry as the tooltip.
    D2D1_ROUNDED_RECT card = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), 10.0f, 10.0f);

    // DESIGN-260910 brand pass (session 260910_0005): palette re-anchored to
    // the Emebala brand DNA sampled from assets/ (Brand Logo + Chat Logo +
    // poster): lapis-lazuli royal blue (#2E4E9E medallion ground / #0C1830
    // poster navy), antique gold (#D4AF37 star + arrow, already the logo-frame
    // border), warm sandstone (#E8D5A8). The former neutral slate family and
    // the emerald #10B981 accent (present in NO brand asset) are replaced:
    // slate -> lapis-tinted equivalents, emerald accent -> brand gold.
    // Contrast is preserved or improved (WCAG on #0C1830): sand-white body
    // 14.8:1, lapis-gray subtext 6.9:1, gold accent 8.8:1 (was 15.5 / 7.0 /
    // 7.0). Close-hover keeps #EF4444 (cross-surface close semantics, same as
    // tooltip.cpp); the logo bitmap itself is untouched brand artwork.
    // C1 (session 260910_0007): the 12 per-frame CreateSolidColorBrush calls
    // became SetColor arguments on the single persistent scratch_brush_ -
    // identical ColorF values, identical draw order. Aliasing audit: the old
    // code's brushes were each consumed immediately at their draw call
    // (including the features loop where accent/text alternate per iteration),
    // so SetColor-just-before-use reproduces every draw byte-for-byte; one
    // brush is sufficient (no second scratch needed).
    scratch_brush_->SetColor(D2D1::ColorF(0x0C1830, 0.96f));  // poster navy (lapis shadow)
    dc_render_target_->FillRoundedRectangle(card, scratch_brush_);
    scratch_brush_->SetColor(D2D1::ColorF(0x33507E, 0.85f));  // lapis mid
    dc_render_target_->DrawRoundedRectangle(card, scratch_brush_, 1.0f);

    // DESIGN-260910 (P2 consistency): 1px warm-white top rim light, identical
    // to the tooltip card's (src/ui/tooltip.cpp Render) so both windows read
    // as the same glass material over any host background.
    {
        scratch_brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f));
        dc_render_target_->DrawLine(
            D2D1::Point2F(9.0f, 1.5f), D2D1::Point2F(w - 9.0f, 1.5f),
            scratch_brush_, 1.0f);
    }

    // 1. Logo: 64x64 squircle, gold border (plan §2.2 layout item 1).
    const D2D1_RECT_F logoFrame = D2D1::RectF((w - 64.0f) / 2.0f, 28.0f, (w + 64.0f) / 2.0f, 92.0f);
    scratch_brush_->SetColor(D2D1::ColorF(0x14243F, 0.9f));  // lapis-deep logo bg
    dc_render_target_->FillRoundedRectangle(D2D1::RoundedRect(logoFrame, 14.0f, 14.0f), scratch_brush_);
    scratch_brush_->SetColor(D2D1::ColorF(0xD4AF37, 0.85f));  // antique gold border
    dc_render_target_->DrawRoundedRectangle(D2D1::RoundedRect(logoFrame, 14.0f, 14.0f), scratch_brush_, 1.2f);
    const D2D1_RECT_F logoRect = D2D1::RectF(logoFrame.left + 6.0f, logoFrame.top + 6.0f,
                                             logoFrame.right - 6.0f, logoFrame.bottom - 6.0f);
    if (logo_bitmap_) {
        dc_render_target_->DrawBitmap(logo_bitmap_, logoRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        DrawTabletLogoVector(dc_render_target_, logoRect, false);
    }

    // 2. Title + version (version from the single source of truth, REQ-006).
    // REQ-B-005 (design §1.1.7): localized brand title (previously the fixed
    // English display-name constant); resolved per Render through the i18n seam.
    const std::wstring titleText = I18n::Get(StringId::AppName);
    if (title_format_) {
        scratch_brush_->SetColor(D2D1::ColorF(0xF2ECDC, 1.0f));  // warm sand-white
        dc_render_target_->DrawText(titleText.c_str(), static_cast<UINT32>(titleText.size()),
                                    title_format_, D2D1::RectF(24.0f, 100.0f, w - 24.0f, 132.0f), scratch_brush_);
    }
    const std::wstring versionText = L"v" + std::wstring(kAppVersionW);
    if (version_format_) {
        scratch_brush_->SetColor(D2D1::ColorF(0x93A3C7, 1.0f));  // lapis-gray subtext
        dc_render_target_->DrawText(versionText.c_str(), static_cast<UINT32>(versionText.size()),
                                    version_format_, D2D1::RectF(24.0f, 134.0f, w - 24.0f, 152.0f), scratch_brush_);
    }

    // R6 Phase 5: resolve every body string for the CURRENT locale once per
    // Render (the pure seam the unit tests assert against).
    const LocalizedContent content = BuildLocalizedContent();

    // 3. Tagline (body 12, wrap, centered).
    if (tagline_format_) {
        scratch_brush_->SetColor(D2D1::ColorF(0xF2ECDC, 1.0f));  // warm sand-white
        dc_render_target_->DrawText(content.tagline.c_str(), static_cast<UINT32>(content.tagline.size()),
                                    tagline_format_, D2D1::RectF(24.0f, 162.0f, w - 24.0f, 208.0f), scratch_brush_);
    }

    // Divider 1
    scratch_brush_->SetColor(D2D1::ColorF(0x33507E, 0.5f));  // lapis divider
    dc_render_target_->DrawLine(D2D1::Point2F(24.0f, 218.0f), D2D1::Point2F(w - 24.0f, 218.0f),
                                scratch_brush_, 1.0f);

    // 4. Features: 3 blocks of 42 DIP. DESIGN-260910 (P2 hierarchy): each
    // block gains a 5 DIP emerald marker dot so the section scans as three
    // distinct capability statements instead of one gray paragraph wall.
    // The marker mirrors with the UI locale's reading direction (body_format_
    // carries RTL for RTL locales; leading alignment starts at the rect's
    // reading edge), so RTL locales get the dot on the right.
    const bool ui_rtl = DirectionForLocale(I18n::GetCurrentLocale()) == TextDirection::RTL;
    if (body_format_) {
        for (int i = 0; i < 3; ++i) {
            const float top = 230.0f + static_cast<float>(i) * 42.0f;
            const D2D1_ROUNDED_RECT dot = D2D1::RoundedRect(
                ui_rtl ? D2D1::RectF(w - 33.0f, top + 7.0f, w - 28.0f, top + 12.0f)
                       : D2D1::RectF(28.0f, top + 7.0f, 33.0f, top + 12.0f),
                2.5f, 2.5f);
            scratch_brush_->SetColor(D2D1::ColorF(0xD9B45A, 1.0f));  // antique gold marker
            dc_render_target_->FillRoundedRectangle(dot, scratch_brush_);
            scratch_brush_->SetColor(D2D1::ColorF(0xF2ECDC, 1.0f));  // warm sand-white
            dc_render_target_->DrawText(content.features[i].c_str(),
                                        static_cast<UINT32>(content.features[i].size()),
                                        body_format_,
                                        ui_rtl ? D2D1::RectF(28.0f, top, w - 40.0f, top + 40.0f)
                                               : D2D1::RectF(40.0f, top, w - 28.0f, top + 40.0f),
                                        scratch_brush_);
        }
    }

    // 5. Etymology (italic 11, subtext).
    if (etymology_format_) {
        scratch_brush_->SetColor(D2D1::ColorF(0x93A3C7, 1.0f));  // lapis-gray subtext
        dc_render_target_->DrawText(content.etymology.c_str(), static_cast<UINT32>(content.etymology.size()),
                                    etymology_format_, D2D1::RectF(24.0f, 362.0f, w - 24.0f, 402.0f), scratch_brush_);
    }

    // Divider 2
    scratch_brush_->SetColor(D2D1::ColorF(0x33507E, 0.5f));  // lapis divider
    dc_render_target_->DrawLine(D2D1::Point2F(24.0f, 414.0f), D2D1::Point2F(w - 24.0f, 414.0f),
                                scratch_brush_, 1.0f);

    // 6. Links row: 3 pill buttons centered (Website / Contact / Reddit).
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
        scratch_brush_->SetColor(hover ? D2D1::ColorF(0x22406B, 1.0f)     // pill hover
                                       : D2D1::ColorF(0x14243F, 0.9f));   // lapis-deep pill
        dc_render_target_->FillRoundedRectangle(pill, scratch_brush_);
        scratch_brush_->SetColor(D2D1::ColorF(0xD9B45A, 1.0f));  // antique gold accent
        dc_render_target_->DrawRoundedRectangle(pill, scratch_brush_, hover ? 1.4f : 1.0f);
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
            const float top = 472.0f + static_cast<float>(i) * 22.0f;
            dc_render_target_->DrawText(content.contacts[i].c_str(),
                                        static_cast<UINT32>(content.contacts[i].size()),
                                        small_format_,
                                        D2D1::RectF(24.0f, top, w - 24.0f, top + 20.0f),
                                        scratch_brush_);
        }
    }

    // 7b. "Reset to system defaults" action button (Phase 4, REQ-020, plan
    // §2.2): full card width minus the 24 DIP insets, y 546..578, made
    // possible by the 560 -> 596 DIP card extension. Same pill palette as
    // the link buttons (pillBg / pillBgHover / accent border, 4 DIP radius,
    // link_format_ centered text) but wide, visually marking it as an
    // action. During the 1.6 s post-click feedback window the label reads
    // StringId::AboutResetDone instead of the localized resting label.
    reset_rect_ = D2D1::RectF(24.0f, 546.0f, w - 24.0f, 578.0f);
    {
        const bool hover = (hovered_link_ == kHoverReset);
        const bool feedback = (reset_feedback_until_ != 0 &&
                               ::GetTickCount64() < reset_feedback_until_);
        const std::wstring reset_text =
            feedback ? I18n::Get(StringId::AboutResetDone) : content.reset_label;
        const D2D1_ROUNDED_RECT btn = D2D1::RoundedRect(reset_rect_, 4.0f, 4.0f);
        scratch_brush_->SetColor(hover ? D2D1::ColorF(0x22406B, 1.0f)     // pill hover
                                       : D2D1::ColorF(0x14243F, 0.9f));   // lapis-deep pill
        dc_render_target_->FillRoundedRectangle(btn, scratch_brush_);
        scratch_brush_->SetColor(D2D1::ColorF(0xD9B45A, 1.0f));  // antique gold accent
        dc_render_target_->DrawRoundedRectangle(btn, scratch_brush_, hover ? 1.4f : 1.0f);
        if (link_format_) {
            scratch_brush_->SetColor(D2D1::ColorF(0xF2ECDC, 1.0f));  // warm sand-white
            dc_render_target_->DrawText(reset_text.c_str(),
                                        static_cast<UINT32>(reset_text.size()),
                                        link_format_, reset_rect_, scratch_brush_);
        }
    }

    // 8. Close button top-right (same rect math as the tooltip's).
    close_btn_rect_ = D2D1::RectF(w - 32.0f, 12.0f, w - 12.0f, 32.0f);
    if (header_format_) {
        scratch_brush_->SetColor((hovered_link_ == kHoverClose)
                                     ? D2D1::ColorF(0xEF4444, 1.0f)
                                     : D2D1::ColorF(0x93A3C7, 0.8f));
        dc_render_target_->DrawText(L"\u2715", 1, header_format_, close_btn_rect_, scratch_brush_);
    }

    // C1: the twelve palette brushes used to be released here; the persistent
    // scratch_brush_ survives the frame (released at Destroy/device-lost).

    // R6 Phase 3 (audit item 4, plan §3.1 A3): device-lost recovery. Without
    // it a driver reset makes every later EndDraw fail and the About card
    // stays permanently blank while the window is shown.
    const HRESULT hr = dc_render_target_->EndDraw();
    if (IsRecoverableDeviceLost(hr)) {
        RecreateAfterDeviceLost();
    }
}

// R6 Phase 3 (audit item 4): recreate the single-threaded DC render target
// after a device-lost. ReallocateBuffer re-binds SetDpi + BindDC on the fresh
// target; the logo bitmap was created on the lost device and must be rebuilt.
void AboutWindow::RecreateAfterDeviceLost() {
    DIAG_F("ABOUT/DeviceLost/001: D2DERR_RECREATE_TARGET; recreating render target\n");
    // C1: the scratch brush is device-dependent (bound to the lost target).
    // Release it BEFORE ReleaseTarget so no brush reference keeps the dead
    // device alive.
    ReleaseScratchBrush();
    renderer_.ReleaseTarget(&dc_render_target_);
    if (!d2d_factory_) {
        return; // Create() never finished; all render paths null-guard already
    }
    if (!renderer_.CreateTarget(d2d_factory_, &dc_render_target_)) {
        DIAG_F("ABOUT/DeviceLost/002: render-target recreation failed; About stays stale until next Create()\n");
        return;
    }
    ReallocateBuffer(PhysW(), PhysH());
    LoadLogoBitmap();
    EnsureScratchBrush(); // C1: rebuild the scratch brush on the fresh target
}

void AboutWindow::UpdateLayered() {
    // REF-3.6: renderer_ performs the GetWindowRect-based ptDst +
    // GetDC(nullptr) ULW sequence identical to the previous inline code.
    // REQ-R15: physical blit size; alpha fixed 245 (~96% opacity, matches the
    // tooltip card) per §6.
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
        // ---- REQ-R10 marshaled show/dismiss requests (GUI thread runs them) ----
        // SEC-ADJ (Blocker-5 class fix): kShowMessage unpacks its two-int
        // payload from the message PARAMETERS (plain sign-extended ints —
        // never cast to a pointer, so a forged post from another process can
        // at most choose a position, which ShowAt clamps back to the monitor
        // work area; no dereference, no free, no memory unsafety).
        case kShowMessage: {
            pThis->ShowAt(ShowXFromWParam(wParam), ShowYFromLParam(lParam));
            return 0;
        }

        case kDismissMessage: {
            pThis->Dismiss();
            return 0;
        }

        // R6 Phase 6: marshaled locale-refresh request (no heap payload).
        case kLocaleRefreshMessage: {
            pThis->RequestLocaleRefresh();
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
            // Phase 4 (REQ-020): the reset button is interactive too.
            interactive = interactive || IsPointInRect(pThis->reset_rect_, x, y);
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
            // Phase 4 (REQ-020): kHoverReset extends the hover encoding.
            if (hover < 0 && IsPointInRect(pThis->reset_rect_, x, y)) {
                hover = kHoverReset;
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
            // Phase 4 (REQ-020, plan §1.3): reset button click. The window is
            // a pure view - it invokes the coordinator callback (main.cpp,
            // GUI thread, runs the 4-field default rewrite + save + surface
            // refresh synchronously here), then shows the optimistic 1.6 s
            // "done" feedback regardless of the outcome. No confirm dialog
            // (plan §2.2: non-destructive operation).
            if (IsPointInRect(pThis->reset_rect_, x, y)) {
                if (pThis->reset_callback_) {
                    pThis->reset_callback_();
                }
                // Re-arm (not just start): a second click inside the feedback
                // window extends it; SetTimer with the same id replaces the
                // existing timer, so exactly one timer is ever active.
                pThis->reset_feedback_until_ = ::GetTickCount64() + kResetFeedbackMs;
                ::SetTimer(hwnd, kResetFeedbackTimerId, kResetFeedbackMs, nullptr);
                pThis->Render();
                pThis->UpdateLayered();
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

        // Phase 4 (REQ-020, plan §2.2): the "done" label window expired -
        // revert to the resting label and tear the timer down. KillTimer
        // first so a re-entrant render path can never leave it running.
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
            break;
        }

        case WM_KILLFOCUS: {
            // Plan §2.2: closes on focus loss (Alt+Tab, app switch, browser
            // handoff after a link click).
            pThis->Dismiss();
            return 0;
        }

        // D1 (debug report T3): live per-monitor DPI change while visible.
        // DPI is otherwise captured only in ShowAt (MonitorDpiAtPoint), so a
        // scaling change on the showing monitor left a stale physical DIB and
        // a blurry blit until the next open. Same re-allocation discipline as
        // badge.cpp / the tooltip: update dpi_ through the ui/dpi helper,
        // reposition + resize at the new scale, ReallocateBuffer (re-binds
        // SetDpi + BindDC), re-render the fixed DIP layout, re-blit.
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            pThis->dpi_ = emebalachat::ui::WindowDpi(hwnd);
            // Keep our own PhysW()/PhysH() extents (ScaleDipsToPixels
            // rounding): DIB and window rect must match exactly or the blit
            // is rescaled - the very defect being fixed.
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
