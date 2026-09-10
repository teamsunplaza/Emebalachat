#include "tooltip.hpp"
#include "diag_logger.hpp"
#include "asset_loader.hpp"
#include "dpi.hpp"
#include "dwrite_helpers.hpp"  // REF-3.7 (session 260910_0006 T6): shared IsPointInRect / CloneFormatWithLocale
#include "../config.hpp"
#include "../i18n.hpp"
#include "../unicode_utils.hpp"
#include "../win32_input.hpp"
#include "../bidi_utils.hpp"  // P4 Batch B-2: IsRtlLanguageCode / DirectionForLocale

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <shellapi.h> // P4 Batch-3 (REQ-C-002 / D7): ShellExecuteW ms-settings:speech deep link

#include <cctype>

namespace emebalachat {

namespace {
const wchar_t kTooltipClassName[] = L"Emebalachat_TooltipClass";

// REF-3.7 (session 260910_0006 T6, verification §10a/§10c): the file-local
// IsPointInRect and CloneFormatWithLocale (with its B-2 creation-only-locale
// rationale comment) moved verbatim to the shared src/ui/dwrite_helpers.hpp
// after being verified byte-identical against about_window.cpp's copies.

// REF-3.1 (session 260910_0006 T2): the former file-local WcsIEqualsAscii was
// an exact transcription of the shared EqualsIgnoreCaseAscii<wchar_t> template
// in unicode_utils.hpp (same +32 A-Z fold, verbatim for every other code
// unit) and has been deleted; call sites below (DWrite locale-tag equality
// checks) now use the template directly. DWrite canonicalizes locale tags on
// readback ("zh-CN" is stored and echoed lowercased — B-2 probe datum), so
// tag equality checks must case-fold or every show would churn a swap.
std::string GetTokenName(ISpObjectToken* pToken) {
    if (!pToken) return "";
    ISpDataKey* pAttrKey = nullptr;
    if (SUCCEEDED(pToken->OpenKey(L"Attributes", &pAttrKey)) && pAttrKey) {
        LPWSTR pName = nullptr;
        if (SUCCEEDED(pAttrKey->GetStringValue(L"Name", &pName)) && pName) {
            std::string name = ToUtf8(pName);
            ::CoTaskMemFree(pName);
            pAttrKey->Release();
            return name;
        }
        pAttrKey->Release();
    }
    LPWSTR pDefaultVal = nullptr;
    if (SUCCEEDED(pToken->GetStringValue(nullptr, &pDefaultVal)) && pDefaultVal) {
        std::string name = ToUtf8(pDefaultVal);
        ::CoTaskMemFree(pDefaultVal);
        return name;
    }
    return "";
}

std::string GetTokenLanguage(ISpObjectToken* pToken) {
    if (!pToken) return "";
    ISpDataKey* pAttrKey = nullptr;
    if (SUCCEEDED(pToken->OpenKey(L"Attributes", &pAttrKey)) && pAttrKey) {
        LPWSTR pLang = nullptr;
        if (SUCCEEDED(pAttrKey->GetStringValue(L"Language", &pLang)) && pLang) {
            std::string lang = ToUtf8(pLang);
            ::CoTaskMemFree(pLang);
            pAttrKey->Release();
            return lang;
        }
        pAttrKey->Release();
    }
    return "";
}

int MatchTokenLanguage(ISpObjectToken* pToken, DWORD target_lcid) {
    if (!pToken || target_lcid == 0) return 0;

    int best_match = 0;
    WORD target_primary = PRIMARYLANGID(static_cast<WORD>(target_lcid));

    ISpDataKey* pAttrKey = nullptr;
    if (SUCCEEDED(pToken->OpenKey(L"Attributes", &pAttrKey)) && pAttrKey) {
        LPWSTR pLangStr = nullptr;
        if (SUCCEEDED(pAttrKey->GetStringValue(L"Language", &pLangStr)) && pLangStr) {
            std::wstring str = pLangStr;
            ::CoTaskMemFree(pLangStr);

            size_t start = 0;
            while (start < str.size()) {
                while (start < str.size() && (str[start] == L';' || str[start] == L',' || str[start] == L' ')) {
                    start++;
                }
                if (start >= str.size()) break;
                size_t end = start;
                while (end < str.size() && str[end] != L';' && str[end] != L',' && str[end] != L' ') {
                    end++;
                }
                std::wstring token_hex = str.substr(start, end - start);
                start = end;

                try {
                    DWORD token_lcid = static_cast<DWORD>(std::stoul(token_hex, nullptr, 16));
                    if (token_lcid == target_lcid) {
                        best_match = (std::max)(best_match, 2);
                    } else if (PRIMARYLANGID(static_cast<WORD>(token_lcid)) == target_primary) {
                        best_match = (std::max)(best_match, 1);
                    }
                } catch (...) {}
            }
        }
        pAttrKey->Release();
    }

    if (best_match > 0) return best_match;

    wchar_t hex_buf[16];
    swprintf_s(hex_buf, L"%x", target_lcid);
    std::wstring query = L"Language=";
    query += hex_buf;

    BOOL bMatch = FALSE;
    if (SUCCEEDED(pToken->MatchesAttributes(query.c_str(), &bMatch)) && bMatch) {
        return 2;
    }

    return 0;
}

} // namespace

DWORD GetLcidForLanguage(std::string_view target_lang_name_or_code) {
    if (target_lang_name_or_code.empty()) return 0;

    // Direct hex prefix check (e.g. "0x409", "0x0409")
    if (target_lang_name_or_code.rfind("0x", 0) == 0 || target_lang_name_or_code.rfind("0X", 0) == 0) {
        try {
            return static_cast<DWORD>(std::stoul(std::string(target_lang_name_or_code.substr(2)), nullptr, 16));
        } catch (...) {}
    }

    // Try canonical resolution via config.hpp's NormalizeLanguageCode
    std::string code = NormalizeLanguageCode(target_lang_name_or_code);
    if (code == "AUTO") {
        size_t sep = target_lang_name_or_code.find_first_of("-_");
        if (sep != std::string_view::npos) {
            std::string sub = NormalizeLanguageCode(target_lang_name_or_code.substr(0, sep));
            if (sub != "AUTO") {
                code = sub;
            }
        }
    }

    if (code == "EN") return 0x0409;
    if (code == "KO") return 0x0412;
    if (code == "JA") return 0x0411;
    if (code == "ZH-CN" || code == "ZH") return 0x0804;
    if (code == "ZH-TW") return 0x0404;
    if (code == "ES") return 0x040A;
    if (code == "FR") return 0x040C;
    if (code == "DE") return 0x0407;
    if (code == "RU") return 0x0419;
    if (code == "VI") return 0x042A;
    if (code == "PT") return 0x0416;
    if (code == "IT") return 0x0410;
    if (code == "TH") return 0x041E;
    if (code == "AR") return 0x0401;
    if (code == "ID") return 0x0421;
    if (code == "MS") return 0x043E;
    if (code == "FIL") return 0x0464;
    if (code == "KM") return 0x0453;
    if (code == "LO") return 0x0454;
    if (code == "HI") return 0x0439;
    if (code == "BN") return 0x0445;
    if (code == "TR") return 0x041F;
    if (code == "PL") return 0x0415;
    if (code == "NL") return 0x0413;
    if (code == "UK") return 0x0422;
    if (code == "FA") return 0x0429;
    if (code == "UR") return 0x0420;
    if (code == "HE") return 0x040D;
    if (code == "CS") return 0x0405;
    if (code == "HU") return 0x040E;
    if (code == "SV") return 0x041D;
    if (code == "EL") return 0x0408;
    if (code == "RO") return 0x0418;
    if (code == "DA") return 0x0406;
    if (code == "FI") return 0x040B;
    if (code == "NO") return 0x0414;
    if (code == "MY") return 0x0455;

    // Fallback: Check if string is 3 to 6 hex characters (e.g. "409", "0409", "412")
    bool all_hex = true;
    for (char c : target_lang_name_or_code) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            all_hex = false;
            break;
        }
    }
    if (all_hex && target_lang_name_or_code.size() >= 3 && target_lang_name_or_code.size() <= 6) {
        try {
            return static_cast<DWORD>(std::stoul(std::string(target_lang_name_or_code), nullptr, 16));
        } catch (...) {}
    }

    return 0;
}

TooltipWindow::TooltipWindow() = default;

TooltipWindow::~TooltipWindow() {
    Destroy();
}

bool TooltipWindow::Create(HINSTANCE hInstance) {
    hInstance_ = hInstance;
    // REQ-R10 (audit §3.4): remember the thread that owns the single-threaded
    // D2D factory/render target. Create() runs on the main GUI thread in
    // wWinMain (and on the test main thread in run_tests); direct render calls
    // from any other thread are marshaled to WndProc via PostMessageW.
    gui_thread_id_ = ::GetCurrentThreadId();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = TooltipWindow::WndProc;
    wc.hInstance = hInstance_;
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = kTooltipClassName;
    ::RegisterClassExW(&wc);

    hwnd_ = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kTooltipClassName,
        L"Emebala Chat Translation Tooltip",
        WS_POPUP,
        -1000, -1000, PhysW(), PhysH(),
        nullptr, nullptr, hInstance_, this
    );

    if (!hwnd_) {
        return false;
    }

    // Direct2D & DirectWrite Factories
    if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory_))) {
        return false;
    }

    // Discovered defect fix (Batch 2, reported): the DC render target creation
    // call was missing here (badge.cpp/drag_icon.cpp both have it), leaving
    // dc_render_target_ null forever -> Render() early-returns and every
    // UpdateLayeredWindow blit pushed an empty DIB: the invisible-tooltip
    // symptom of REQ-001 (trigger logic untouched; this is D2D init). Without
    // it no Batch 2 rendering (scrolling included) could ever be verified.
    // REF-3.6: target creation + lifetime owned by renderer_; dc_render_target_
    // stays a non-owning alias so the render code below is untouched.
    if (!renderer_.CreateTarget(d2d_factory_, &dc_render_target_)) {
        return false;
    }

    // C1: the persistent scratch brush lives with the target (created here and
    // in RecreateAfterDeviceLost, never at render entry).
    EnsureScratchBrush();

    // REQ-R15: single buffer allocation AFTER the render target exists so the
    // D2D DPI transform is set in the same step (ReallocateBuffer no-ops its
    // bind when the target is null).
    ReallocateBuffer(PhysW(), PhysH());

    LoadLogoBitmap();

    if (FAILED(::DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&dwrite_factory_)
    ))) {
        return false;
    }

    // DirectWrite Typography
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
        12.0f,
        L"",
        &header_format_
    );
    if (header_format_) {
        header_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        header_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    dwrite_factory_->CreateTextFormat(
        fontName,
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        13.5f,
        L"",
        &body_format_
    );
    if (body_format_) {
        body_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        body_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    dwrite_factory_->CreateTextFormat(
        fontName,
        nullptr,
        DWRITE_FONT_WEIGHT_MEDIUM,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        11.5f,
        L"",
        &button_format_
    );
    if (button_format_) {
        button_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        button_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    dwrite_factory_->CreateTextFormat(
        fontName,
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        11.0f,
        L"",
        &small_format_
    );

    InitSapi();
    return true;
}

void TooltipWindow::Destroy() {
    StopTTS();
    CleanupSapi();

    if (hwnd_) {
        // R6 Phase 3 (audit items 6+8): free marshal payloads still sitting in
        // the thread queue. DestroyWindow purges the queue WITHOUT running any
        // destructor for LPARAM heap pointers, so a shutdown with posted-but-
        // undelivered Show/Refresh requests leaked each TranslationPayload /
        // MessagePayload / TargetLangPayload. GUI-thread-only (queue scope).
        if (::GetCurrentThreadId() == gui_thread_id_) {
            DrainMarshalQueue();
        }
        ::KillTimer(hwnd_, kTimerCopiedFeedback);
        ::KillTimer(hwnd_, kTimerMessageAutohide);
        ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    if (logo_bitmap_) { logo_bitmap_->Release(); logo_bitmap_ = nullptr; }
    if (small_format_) { small_format_->Release(); small_format_ = nullptr; }
    if (button_format_) { button_format_->Release(); button_format_ = nullptr; }
    if (body_format_) { body_format_->Release(); body_format_ = nullptr; }
    if (header_format_) { header_format_->Release(); header_format_ = nullptr; }
    if (dwrite_factory_) { dwrite_factory_->Release(); dwrite_factory_ = nullptr; }
    ReleaseScratchBrush(); // C1: brush released while its target is still alive
    renderer_.ReleaseTarget(&dc_render_target_);
    if (d2d_factory_) { d2d_factory_->Release(); d2d_factory_ = nullptr; }

    renderer_.FreeBuffer();
}

void TooltipWindow::LoadLogoBitmap() {
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

// C1 (session 260910_0007 hygiene): the card is drawn through ONE persistent
// ID2D1SolidColorBrush. Aliasing audit result: the old Render created up to 17
// brushes per pass but NEVER held two of them alive across each other's color
// use - every fill/stroke/text consumed its brush immediately, and the only
// "ternary brush" sites selected between colors at draw time. SetColor-
// just-before-use discipline is therefore sufficient; a second scratch brush
// is not needed. The scratch brush is created at Create() and device-lost
// recovery (NEVER at render entry) and released in RecreateAfterDeviceLost
// (before the old target is dropped: a device-lost target kept alive by a
// leaked brush reference would resurrect the broken device) and in Destroy().
// Null target -> null brush; every draw site keeps the old `if (brush)`
// discipline via the single early bail in Render().
void TooltipWindow::EnsureScratchBrush() {
    if (!scratch_brush_ && dc_render_target_) {
        dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f),
                                                 &scratch_brush_);
    }
}

void TooltipWindow::ReleaseScratchBrush() {
    if (scratch_brush_) {
        scratch_brush_->Release();
        scratch_brush_ = nullptr;
    }
}

// C2 (session 260910_0007): measured-layout cache. Catalog audit verified the
// src-tag pill width [was inline at Render] and the two footer button widths
// [was inline at Render] as the ONLY genuinely per-frame CreateTextLayout
// measurement sites; the ShowTranslation body measures (content path, once
// per translation) were mislabeled and stay untouched. The label inputs
// change on content set / language pick / UI-locale string switch only, so
// the cache keys are the exact label strings; a key match skips all
// shaping. DirectWrite metrics are DPI-independent DIPs, so a DPI crossing
// needs no recompute (verified: the old blocks used constant 512 DIP extent
// constraints with no per-frame width drift). The hit-test rects
// (src_btn_rect_, copy_btn_rect_, tts_btn_rect_) are rebuilt every Render
// FROM these cached widths in the same pass, so measure and hit-test cannot
// disagree (the VP's stale-cache click-misroute hazard is closed by the
// single source of truth). If measurement is unavailable (no format /
// factory / layout failure), today's degradation is frozen: widths keep
// their floors (== the old hardcoded fallback sizes).
void TooltipWindow::EnsureMeasuredLayouts() {
    if (!dwrite_factory_) return;

    // --- site 3: source-language pill width (header_format_) ---
    const std::wstring src_tag_text = source_lang_code_.empty()
                                          ? I18n::Get(StringId::AutoDetect)
                                          : ToUtf16(source_lang_code_);
    const std::wstring src_tag = src_tag_text + L" ▾";
    if (header_format_ && src_tag != src_tag_key_) {
        float width = 44.0f; // floor == the pre-F8 fixed-size box (fallback parity)
        IDWriteTextLayout* src_layout = nullptr;
        if (SUCCEEDED(dwrite_factory_->CreateTextLayout(
                src_tag.c_str(), static_cast<UINT32>(src_tag.size()), header_format_,
                512.0f, 22.0f, &src_layout)) && src_layout) {
            DWRITE_TEXT_METRICS src_metrics = {};
            if (SUCCEEDED(src_layout->GetMetrics(&src_metrics))) {
                const float measured = src_metrics.width + 16.0f;
                if (measured > width) width = measured;
            }
            src_layout->Release();
        }
        src_tag_width_ = width;
        src_tag_key_ = src_tag;
    }

    // --- sites 4-6: footer pill widths (button_format_) ---
    if (button_format_) {
        const std::wstring copy_label = I18n::Get(StringId::TooltipButtonCopy);
        const std::wstring copied_label = I18n::Get(StringId::TooltipCopied);
        const std::wstring tts_label = I18n::Get(StringId::TooltipButtonTts);
        const std::wstring labels_key = copy_label + L'\0' + copied_label + L'\0' + tts_label;
        if (labels_key != footer_labels_key_) {
            float copy_w = 88.0f; // floors == the pre-DESIGN-260910 hardcoded boxes
            float tts_w = 82.0f;
            IDWriteTextLayout* lbl_layout = nullptr;
            DWRITE_TEXT_METRICS lbl_metrics = {};
            if (SUCCEEDED(dwrite_factory_->CreateTextLayout(
                    copy_label.c_str(), static_cast<UINT32>(copy_label.size()),
                    button_format_, 512.0f, 24.0f, &lbl_layout)) && lbl_layout) {
                if (SUCCEEDED(lbl_layout->GetMetrics(&lbl_metrics)) && lbl_metrics.width > 0.0f) {
                    copy_w = lbl_metrics.width + 20.0f;
                    if (copy_w < 88.0f) copy_w = 88.0f;
                    if (copy_w > 180.0f) copy_w = 180.0f;
                }
                lbl_layout->Release();
            }
            lbl_layout = nullptr;
            if (SUCCEEDED(dwrite_factory_->CreateTextLayout(
                    copied_label.c_str(), static_cast<UINT32>(copied_label.size()),
                    button_format_, 512.0f, 24.0f, &lbl_layout)) && lbl_layout) {
                if (SUCCEEDED(lbl_layout->GetMetrics(&lbl_metrics)) && lbl_metrics.width > 0.0f) {
                    const float w_feedback = lbl_metrics.width + 20.0f;
                    if (w_feedback > copy_w) copy_w = w_feedback;
                    if (copy_w > 180.0f) copy_w = 180.0f;
                }
                lbl_layout->Release();
            }
            lbl_layout = nullptr;
            if (SUCCEEDED(dwrite_factory_->CreateTextLayout(
                    tts_label.c_str(), static_cast<UINT32>(tts_label.size()),
                    button_format_, 512.0f, 24.0f, &lbl_layout)) && lbl_layout) {
                if (SUCCEEDED(lbl_layout->GetMetrics(&lbl_metrics)) && lbl_metrics.width > 0.0f) {
                    tts_w = lbl_metrics.width + 20.0f;
                    if (tts_w < 82.0f) tts_w = 82.0f;
                    if (tts_w > 150.0f) tts_w = 150.0f;
                }
                lbl_layout->Release();
            }
            copy_btn_w_ = copy_w;
            tts_btn_w_ = tts_w;
            footer_labels_key_ = labels_key;
        }
    }
}

void TooltipWindow::InitSapi() {
    if (!voice_) {
        HRESULT hr = ::CoCreateInstance(
            CLSID_SpVoice,
            nullptr,
            CLSCTX_ALL,
            IID_ISpVoice,
            reinterpret_cast<void**>(&voice_)
        );
        if (SUCCEEDED(hr) && voice_) {
            if (!default_voice_token_) {
                voice_->GetVoice(&default_voice_token_);
                if (default_voice_token_) {
                    current_voice_name_ = GetTokenName(default_voice_token_);
                    current_voice_lang_ = GetTokenLanguage(default_voice_token_);
                }
            }
        }
    }
}

void TooltipWindow::CleanupSapi() {
    if (voice_) {
        voice_->Speak(nullptr, SPF_PURGEBEFORESPEAK | SPF_ASYNC, nullptr);
        voice_->Release();
        voice_ = nullptr;
    }
    if (default_voice_token_) {
        default_voice_token_->Release();
        default_voice_token_ = nullptr;
    }
    current_voice_name_.clear();
    current_voice_lang_.clear();
}

bool TooltipWindow::SelectVoiceForLanguage(std::string_view target_lang_name_or_code) {
    InitSapi();
    if (!voice_) return false;

    DWORD target_lcid = GetLcidForLanguage(target_lang_name_or_code);
    if (target_lcid == 0) {
        // P4 Batch-3 (REQ-C-002 / D8): the forced default_voice_token_ fallback
        // that used to live here is REMOVED — an unknown language must not
        // silently read the text in the system default voice. Voice state
        // stays untouched; the caller decides the UX (ShowNoVoiceNotice).
        return false;
    }

    // Check if current voice already matches target LCID
    if (!current_voice_lang_.empty()) {
        try {
            DWORD curr_lcid = static_cast<DWORD>(std::stoul(current_voice_lang_, nullptr, 16));
            if (curr_lcid == target_lcid) {
                return true;
            }
        } catch (...) {}
    }

    struct VoiceCandidate {
        ISpObjectToken* token = nullptr;
        int score = 0;
        std::string name;
        std::string lang;
    };

    std::vector<VoiceCandidate> candidates;

    const wchar_t* kCategories[] = {
        SPCAT_VOICES,
        L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices"
    };

    for (size_t cat_idx = 0; cat_idx < 2; ++cat_idx) {
        ISpObjectTokenCategory* pCategory = nullptr;
        HRESULT hr = ::CoCreateInstance(
            CLSID_SpObjectTokenCategory,
            nullptr,
            CLSCTX_ALL,
            IID_ISpObjectTokenCategory,
            reinterpret_cast<void**>(&pCategory)
        );
        if (FAILED(hr) || !pCategory) continue;

        if (FAILED(pCategory->SetId(kCategories[cat_idx], FALSE))) {
            pCategory->Release();
            continue;
        }

        IEnumSpObjectTokens* pEnum = nullptr;
        if (FAILED(pCategory->EnumTokens(nullptr, nullptr, &pEnum)) || !pEnum) {
            pCategory->Release();
            continue;
        }

        ULONG count = 0;
        pEnum->GetCount(&count);
        for (ULONG i = 0; i < count; ++i) {
            ISpObjectToken* pToken = nullptr;
            if (SUCCEEDED(pEnum->Item(i, &pToken)) && pToken) {
                int match = MatchTokenLanguage(pToken, target_lcid);
                if (match > 0) {
                    // P4 Batch-3 (REQ-C-001 / D6): OneCore 100 / SAPI 50 base +
                    // the preserved +20 exact-LCID bonus, via the constexpr
                    // lattice helper whose static_assert proof lives in
                    // tooltip.hpp. Replaces the old inverted inline math
                    // ((cat_idx == 0) ? 80 : 40), which ranked legacy SAPI
                    // voices above modern OneCore voices for the same language.
                    int score = VoiceTotalScore(static_cast<int>(cat_idx), match);

                    VoiceCandidate c;
                    c.token = pToken; // Keep AddRef
                    c.score = score;
                    c.name = GetTokenName(pToken);
                    c.lang = GetTokenLanguage(pToken);
                    candidates.push_back(c);
                } else {
                    pToken->Release();
                }
            }
        }

        pEnum->Release();
        pCategory->Release();
    }

    std::stable_sort(candidates.begin(), candidates.end(), [](const VoiceCandidate& a, const VoiceCandidate& b) {
        return a.score > b.score;
    });

    bool voice_switched = false;
    for (auto& cand : candidates) {
        if (!voice_switched) {
            HRESULT hr = voice_->SetVoice(cand.token);
            if (SUCCEEDED(hr)) {
                voice_switched = true;
                current_voice_name_ = cand.name;
                current_voice_lang_ = cand.lang;
            }
        }
        cand.token->Release();
    }
    candidates.clear();

    if (voice_switched) {
        // Manual-QA trace (P3 §3 Step 3 "OneCore name in log"): the winning
        // voice is identifiable by name ("... Online (Natural)" / OneCore
        // voices vs legacy "... Desktop" SAPI voices).
        DIAG_LOG("UI", "tts_voice_selected name=%s lang=%s",
                 current_voice_name_.c_str(), current_voice_lang_.c_str());
        return true;
    }

    // P4 Batch-3 (REQ-C-002 / D8): no candidates matched the target language.
    // The forced default_voice_token_ fallback that used to live here is
    // REMOVED — it made SAPI read the text in the system default language
    // (English/Korean) with zero indication to the user. Voice state stays
    // untouched; the caller surfaces ShowNoVoiceNotice() and speaks nothing.
    DIAG_LOG("UI", "tts_voice_none target_lcid=0x%04lx", target_lcid);
    return false;
}

int TooltipWindow::PhysW() const {
    return emebalachat::ui::ScaleDipsToPixels(current_width_, dpi_);
}

int TooltipWindow::PhysH() const {
    return emebalachat::ui::ScaleDipsToPixels(current_height_, dpi_);
}

void TooltipWindow::RebindRenderTarget() {
    renderer_.Rebind(PhysW(), PhysH(), dpi_, /*set_dpi=*/false);
}

// REF-3.6: DIB (re)creation + SetDpi/BindDC tail owned by the shared renderer
// (REQ-R15: D2D DPI tracks the monitor so DIP layout rasterizes 1:1).
void TooltipWindow::ReallocateBuffer(int width, int height) {
    renderer_.ReallocateBuffer(width, height, dpi_);
}

void TooltipWindow::ShowTranslation(
    int x, int y,
    std::wstring_view source_text,
    std::string_view source_lang_code,
    std::string_view target_lang,
    std::wstring_view translated_text,
    uint64_t generation
) {
    if (!hwnd_) return;

    // REQ-R10 (audit §3.4): thread affinity guard. Hook/worker threads are
    // marshaled to the owning GUI thread instead of touching D2D cross-thread
    // (D2DERR_WRONG_THREAD -> invisible tooltip). Same-thread callers (badge
    // click copy path, tests, the WndProc-marshaled re-entry) run directly.
    if (::GetCurrentThreadId() != gui_thread_id_) {
        ShowTranslationThreadSafe(x, y, source_text, source_lang_code, target_lang, translated_text,
                                  generation);
        return;
    }

    // R6 Phase 2 (B1-H1): generation guard, evaluated on the GUI thread AFTER
    // any marshal reordering. A translate result whose request generation is
    // older than the latest-REQUESTED generation is a superseded (stale)
    // delivery - the user already triggered a newer selection - and must not
    // touch the model or repaint. Newest-wins regardless of which thread
    // finished first or in which order the payloads were dequeued.
    if (!ShouldRenderForGeneration(latest_request_gen_.load(std::memory_order_relaxed),
                                   generation)) {
        ++dropped_stale_shows_;
        DIAG_LOG("UI", "tooltip_marshal_drop kind=translation gen=%llu latest=%llu total_drops=%llu",
                 static_cast<unsigned long long>(generation),
                 static_cast<unsigned long long>(latest_request_gen_.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(dropped_stale_shows_));
        return;
    }
    DIAG_LOG("UI", "tooltip_show kind=translation gen=%llu x=%d y=%d src_len=%zu out_len=%zu",
             static_cast<unsigned long long>(generation), x, y,
             source_text.size(), translated_text.size());

    is_message_mode_ = false;
    no_voice_notice_active_ = false; // P4 Batch-3 (D7): card replaced by translation content
    ::KillTimer(hwnd_, kTimerMessageAutohide);

    source_text_ = source_text;
    source_lang_code_ = source_lang_code;
    target_lang_ = target_lang;
    translated_text_ = translated_text;
    copied_feedback_ = false;
    hovered_btn_ = 0;

    // P4 Batch B-2 (REQ-038, design §2-Q2 verdict A / §2.2.2 item 1): the
    // SINGLE body-direction mutation point — right after translated_text_ is
    // stored, before measure (CreateTextLayout below + the gutter re-measure)
    // and Render's DrawText consume body_format_. Measure and render can then
    // never disagree (risk R1 eliminated by construction). Direction follows
    // the CONTENT's target language, never the UI locale; header/button/
    // close/scrollbar chrome stays LTR (design constraint 2). GUI thread only
    // (§2-Q2 threading rule): this body runs after the marshal re-entry, on
    // the same thread that creates layouts and draws.
    if (body_format_) {
        const bool body_rtl = IsRtlLanguageCode(target_lang_);
        // Design §2-Q5 verdict A: body_format_'s localeName carries the
        // target's BCP-47 tag so DWrite's font fallback resolves script-
        // appropriate faces (Myanmar Text, Leelawadee UI/Nirmala UI, Segoe UI
        // Historic...). localeName is CREATION-ONLY in DWrite (no
        // SetLocaleName exists — B-2 SDK header audit + headless probe), so a
        // tag change is a clone-swap; an unchanged tag (case-folded, DWrite
        // lowercases on readback) never churns the COM object. Unresolvable
        // or empty targets normalize to AUTO, whose tag is the "en" pivot.
        const std::string norm_code = NormalizeLanguageCode(target_lang_);
        const LanguageInfo* tag_info = FindLanguageByCode(norm_code);
        if (tag_info && tag_info->bcp47 && tag_info->bcp47[0]) {
            const std::wstring tag = ToUtf16(tag_info->bcp47);
            wchar_t cur_locale[64] = {};
            const bool same_locale =
                SUCCEEDED(body_format_->GetLocaleName(cur_locale, 64)) &&
                EqualsIgnoreCaseAscii<wchar_t>(cur_locale, tag);
            if (!same_locale) {
                IDWriteTextFormat* swapped =
                    CloneFormatWithLocale(dwrite_factory_, body_format_, tag.c_str());
                if (swapped) {
                    body_format_->Release();
                    body_format_ = swapped;
                } else {
                    // Fail-safe: keep the previous format (L"" fallback locale
                    // still resolves glyphs via the system chain; only the
                    // script-first-face hint is lost). Direction below still
                    // applies to the surviving format.
                    DIAG_LOG("UI",
                             "tooltip_b2/ShowTranslation/001 body_locale_swap_fail tag=%ls",
                             tag.c_str());
                }
            }
        }
        body_format_->SetReadingDirection(
            body_rtl ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT
                     : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);
        // Grep-able proof the per-content direction decision ran (design §4.2c
        // — the E2E-RTL-1 log assertion; same DIAG discipline as tooltip_show).
        // target= carries the CANONICAL code per the design's `target=AR`
        // example, not the raw name_en payload ("Arabic").
        DIAG_LOG("UI", "tooltip body_dir=%s target=%s",
                 body_rtl ? "rtl" : "ltr", norm_code.c_str());
    }

    // Measure body text layout height (DIP; DirectWrite metrics are DPI
    // independent). Layout width matches the painted body rect exactly
    // (w - 28 when not scrollable) so measurement cannot disagree with render.
    float text_height = 40.0f;
    UINT32 text_line_count = 0;
    if (dwrite_factory_ && body_format_) {
        IDWriteTextLayout* layout = nullptr;
        if (SUCCEEDED(dwrite_factory_->CreateTextLayout(
                translated_text_.c_str(),
                static_cast<UINT32>(translated_text_.size()),
                body_format_,
                static_cast<float>(current_width_ - 28),
                100000.0f,
                &layout)) && layout) {
            DWRITE_TEXT_METRICS m = {};
            layout->GetMetrics(&m);
            text_height = (std::max)(36.0f, m.height);
            text_line_count = m.lineCount;
            layout->Release();
        }
    }

    // Calculate dynamic window height: header (38) + pad (12) + body + pad (16) + footer (36)
    // (DIP layout units - the DirectWrite metrics above are DPI-independent).
    // REQ-002 (plan §2.1): the 480 cap becomes 520; content taller than the
    // capped body viewport scrolls instead of being clipped.
    int calculated_height = static_cast<int>(std::ceil(38.0f + 12.0f + text_height + 16.0f + 36.0f));
    current_height_ = (std::max)(140, (std::min)(calculated_height, kMaxWindowHeightDip));

    // REQ-002: a fresh translation always starts unscrolled (plan edge case 4).
    scroll_offset_dip_ = 0.0f;
    dragging_thumb_ = false;
    thumb_hover_ = false;
    content_height_dip_ = text_height;
    line_height_dip_ = (text_line_count > 0)
        ? text_height / static_cast<float>(text_line_count)
        : 18.0f;
    scrollable_ = false;
    const float viewport_h = BodyViewportHeightDip(current_height_);
    // Strict comparison: content exactly equal to the viewport fits without
    // clipping (plan §2.1 edge case 1); 1 DIP more must scroll, not clip.
    if (text_height > viewport_h) {
        scrollable_ = true;
        // Re-measure with the extra 4 DIP gutter the renderer reserves for
        // the custom scrollbar (width w - 32): a narrower wrap can only grow
        // the content extent, so adopt the larger measurement when it differs.
        if (dwrite_factory_ && body_format_) {
            IDWriteTextLayout* gutter_layout = nullptr;
            if (SUCCEEDED(dwrite_factory_->CreateTextLayout(
                    translated_text_.c_str(),
                    static_cast<UINT32>(translated_text_.size()),
                    body_format_,
                    static_cast<float>(current_width_ - 32),
                    100000.0f,
                    &gutter_layout)) && gutter_layout) {
                DWRITE_TEXT_METRICS m2 = {};
                gutter_layout->GetMetrics(&m2);
                if (m2.height > content_height_dip_) {
                    content_height_dip_ = m2.height;
                    if (m2.lineCount > 0) {
                        line_height_dip_ = m2.height / static_cast<float>(m2.lineCount);
                    }
                }
                gutter_layout->Release();
            }
        }
    }

    // REQ-R15 (audit §5 latent item 3): capture the target monitor's DPI from
    // the (physical) cursor coordinates the caller passed, then allocate the
    // physical buffer and clamp in the same physical units.
    dpi_ = emebalachat::ui::MonitorDpiAtPoint(POINT{ x, y });
    ReallocateBuffer(PhysW(), PhysH());

    // Multi-monitor aware bounds clamping
    POINT pt = { x, y };
    HMONITOR hMon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(MONITORINFO);
    if (::GetMonitorInfoW(hMon, &mi)) {
        const POINT clamped = emebalachat::ui::ClampWindowOrigin(x, y, PhysW(), PhysH(), 10, mi.rcWork);
        x = clamped.x;
        y = clamped.y;
    }

    ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, PhysW(), PhysH(), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;

    Render();
    UpdateLayered();
}

void TooltipWindow::ShowMessage(int x, int y, std::wstring_view header, std::wstring_view body,
                                uint64_t generation) {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() != gui_thread_id_) {
        ShowMessageThreadSafe(x, y, header, body, generation);
        return;
    }

    // R6 Phase 2 (B1-H1): a failure notice from a superseded translate request
    // (e.g. the older drag thread's "copy failed") must not replace a newer
    // result either. Same pure drop rule as ShowTranslation.
    if (!ShouldRenderForGeneration(latest_request_gen_.load(std::memory_order_relaxed),
                                   generation)) {
        ++dropped_stale_shows_;
        DIAG_LOG("UI", "tooltip_marshal_drop kind=message gen=%llu latest=%llu total_drops=%llu",
                 static_cast<unsigned long long>(generation),
                 static_cast<unsigned long long>(latest_request_gen_.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(dropped_stale_shows_));
        return;
    }
    DIAG_LOG("UI", "tooltip_show kind=message gen=%llu x=%d y=%d",
             static_cast<unsigned long long>(generation), x, y);

    // REQ-R08 (audit §3.2): transient state-change notice. Compact fixed-size
    // card; translated_text_ carries the body line, message_header_ the title.
    is_message_mode_ = true;
    // P4 Batch-3 (D7): any generic message resets the no-voice click arming —
    // only ShowNoVoiceNotice re-arms it, AFTER this function returns.
    no_voice_notice_active_ = false;
    message_header_ = header;
    translated_text_ = body;
    source_text_.clear();
    source_lang_code_.clear();
    target_lang_.clear();
    copied_feedback_ = false;
    hovered_btn_ = 0;

    // P4 Batch B-2 (REQ-038, design §2.2.2 items 2+3): message bodies are
    // app-authored notices in the UI locale — direction follows the UI
    // locale, NOT the content's script (per-region rule: an English notice
    // under an RTL UI reads RTL like every other UI-locale string; the
    // first-strong heuristic is the documented future fallback). small_format_
    // is the exclusive message-body format (header_format_ renders the title
    // as LTR chrome — never touched). Single mutation point before Render.
    if (small_format_) {
        const TextDirection msg_dir = DirectionForLocale(I18n::GetCurrentLocale());
        // Design §1.3.2 font binding for the UI-locale-authored content:
        // small_format_'s localeName carries the current UI-locale tag (the
        // eight GetLocaleCode() outputs are all probe-verified BCP-47 tags).
        // Creation-only param => clone-swap on change (see ShowTranslation).
        const std::wstring ui_tag = ToUtf16(I18n::GetLocaleCode());
        if (!ui_tag.empty()) {
            wchar_t cur_locale[64] = {};
            const bool same_locale =
                SUCCEEDED(small_format_->GetLocaleName(cur_locale, 64)) &&
                EqualsIgnoreCaseAscii<wchar_t>(cur_locale, ui_tag);
            if (!same_locale) {
                IDWriteTextFormat* swapped =
                    CloneFormatWithLocale(dwrite_factory_, small_format_, ui_tag.c_str());
                if (swapped) {
                    small_format_->Release();
                    small_format_ = swapped;
                } else {
                    DIAG_LOG("UI",
                             "tooltip_b2/ShowMessage/002 body_locale_swap_fail tag=%ls",
                             ui_tag.c_str());
                }
            }
        }
        small_format_->SetReadingDirection(msg_dir == TextDirection::RTL
                                               ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT
                                               : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);
        DIAG_LOG("UI", "tooltip msg_dir=%s locale=%s",
                 msg_dir == TextDirection::RTL ? "rtl" : "ltr",
                 std::string(I18n::GetLocaleCode()).c_str());
    }

    // REQ-002: the compact notice card is fixed-height and never scrolls.
    scroll_offset_dip_ = 0.0f;
    scrollable_ = false;
    content_height_dip_ = 0.0f;
    dragging_thumb_ = false;
    thumb_hover_ = false;

    current_width_ = 320;
    current_height_ = 84;
    // REQ-R15: same physical-buffer policy as ShowTranslation (see there).
    dpi_ = emebalachat::ui::MonitorDpiAtPoint(POINT{ x, y });
    ReallocateBuffer(PhysW(), PhysH());

    // Multi-monitor aware bounds clamping (same policy as ShowTranslation).
    POINT pt = { x, y };
    HMONITOR hMon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(MONITORINFO);
    if (::GetMonitorInfoW(hMon, &mi)) {
        const POINT clamped = emebalachat::ui::ClampWindowOrigin(x, y, PhysW(), PhysH(), 10, mi.rcWork);
        x = clamped.x;
        y = clamped.y;
    }

    ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, PhysW(), PhysH(), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;

    Render();
    UpdateLayered();

    ::KillTimer(hwnd_, kTimerMessageAutohide);
    ::SetTimer(hwnd_, kTimerMessageAutohide, kMessageAutohideMs, nullptr);
}

void TooltipWindow::ShowMessageThreadSafe(int x, int y, std::wstring_view header, std::wstring_view body,
                                          uint64_t generation) {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() == gui_thread_id_) {
        ShowMessage(x, y, header, body, generation);
        return;
    }
    // Heap payload, ownership transferred to WndProc through LPARAM
    // (documented in tooltip.hpp). Post failure deletes locally: the payload
    // can never leak in either branch.
    auto payload = std::make_unique<MessagePayload>();
    payload->x = x;
    payload->y = y;
    payload->header = header;
    payload->body = body;
    payload->generation = generation; // R6 B1-H1: travels INSIDE the payload
    const LPARAM lparam = reinterpret_cast<LPARAM>(payload.release());
    if (::PostMessageW(hwnd_, kShowMessageMessage, 0, lparam) == FALSE) {
        delete reinterpret_cast<MessagePayload*>(lparam);
    }
}

void TooltipWindow::ShowTranslationThreadSafe(
    int x, int y,
    std::wstring_view source_text,
    std::string_view source_lang_code,
    std::string_view target_lang,
    std::wstring_view translated_text,
    uint64_t generation
) {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() == gui_thread_id_) {
        ShowTranslation(x, y, source_text, source_lang_code, target_lang, translated_text,
                        generation);
        return;
    }
    auto payload = std::make_unique<TranslationPayload>();
    payload->x = x;
    payload->y = y;
    payload->source_text = source_text;
    payload->source_lang_code = source_lang_code;
    payload->target_lang = target_lang;
    payload->translated_text = translated_text;
    payload->generation = generation; // R6 B1-H1
    const LPARAM lparam = reinterpret_cast<LPARAM>(payload.release());
    if (::PostMessageW(hwnd_, kShowTranslationMessage, 0, lparam) == FALSE) {
        delete reinterpret_cast<TranslationPayload*>(lparam);
    }
}

// R6 Phase 2 (B1-H1): stamps a new translate request and hands back its
// monotonic generation id. Plain fetch_add on a 64-bit counter: wraps only
// after ~2^64 requests (never, in practice), and the >= comparison in
// ShouldRenderForGeneration stays correct for any realistic in-flight window.
// Callable from ANY thread (atomic) - trigger sites live on the GUI thread
// (drag-icon click), the REQ-R06 double-Ctrl+C worker, and hook threads.
uint64_t TooltipWindow::BeginTranslationRequest() {
    const uint64_t gen = latest_request_gen_.fetch_add(1, std::memory_order_relaxed) + 1;
    DIAG_LOG("UI", "tooltip_request_begin gen=%llu", static_cast<unsigned long long>(gen));
    return gen;
}

void TooltipWindow::DismissThreadSafe() {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() == gui_thread_id_) {
        Dismiss();
        return;
    }
    ::PostMessageW(hwnd_, kDismissMessage, 0, 0);
}

// R6 Phase 1 (B3, plan §2.4 step 6/7 companion): view-only sync of the target
// language label after ANY surface mutated the persisted pair (tray menu,
// swap, Ctrl+F9 cycle, config reload). Best-effort by design: hidden or
// message-mode notices have no language button to update, so those cases
// no-op. When visible in translation mode, the button label (Render reads
// target_lang_) is refreshed WITHOUT re-translating - the body stays as shown
// until the next request, per the plan's "view sync, not content churn".
// Cross-thread calls marshal through kRefreshTargetLangMessage (REQ-R10):
// heap payload ownership transfers to the WndProc, deleted locally on post
// failure, same contract as the Show*ThreadSafe seams above.
void TooltipWindow::RefreshTargetLanguageFromConfig(std::string_view new_target) {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() != gui_thread_id_) {
        auto payload = std::make_unique<TargetLangPayload>();
        payload->target_lang = new_target;
        const LPARAM lparam = reinterpret_cast<LPARAM>(payload.release());
        if (::PostMessageW(hwnd_, kRefreshTargetLangMessage, 0, lparam) == FALSE) {
            delete reinterpret_cast<TargetLangPayload*>(lparam);
        }
        return;
    }
    if (!visible_.load(std::memory_order_relaxed) || is_message_mode_) {
        return;
    }
    if (target_lang_ == new_target) {
        return; // already in sync (tooltip-initiated change re-shows below)
    }
    target_lang_ = new_target;
    Render();
    UpdateLayered();
}

void TooltipWindow::Dismiss() {
    if (!hwnd_) return;
    // REQ-R10 companion: the ESC (keyboard hook) and outside-click (mouse hook)
    // dismissal paths call Dismiss() off the GUI thread. ShowWindow/SAPI on
    // another thread's window can block until that pump is free (e.g. mid
    // translation), which would stall LowLevelHooksTimeout. Marshal instead.
    // WndProc re-enters on the GUI thread, so this guard never recurses.
    if (::GetCurrentThreadId() != gui_thread_id_) {
        DIAG_LOG("UI", "tooltip_dismiss requested off-thread (marshaled)");
        ::PostMessageW(hwnd_, kDismissMessage, 0, 0);
        return;
    }
    if (!visible_.load(std::memory_order_relaxed)) return;
    DIAG_LOG("UI", "tooltip_dismiss executed gen=%llu",
             static_cast<unsigned long long>(latest_request_gen_.load(std::memory_order_relaxed)));
    StopTTS();
    ::KillTimer(hwnd_, kTimerMessageAutohide);
    visible_ = false;
    hovered_btn_ = 0;
    copied_feedback_ = false;
    is_message_mode_ = false;
    no_voice_notice_active_ = false; // P4 Batch-3 (D7): notice released on every dismissal path
    // R6 Phase 2 (B1-H2, plan §1 B1-H2 fix direction): clear the content
    // buffers on dismissal. WM_DPICHANGED re-renders the CURRENT model while
    // visible, and any future re-show path that skips a fresh ShowTranslation
    // would otherwise repaint cycle N-1's leftovers. Clearing here removes the
    // stale-repaint vector entirely (the generation guard above stays the
    // primary protection for the producer-side race).
    source_text_.clear();
    source_lang_code_.clear();
    target_lang_.clear();
    translated_text_.clear();
    message_header_.clear();
    // REQ-002 (plan §2.1): scroll state resets on every dismissal.
    scroll_offset_dip_ = 0.0f;
    scrollable_ = false;
    content_height_dip_ = 0.0f;
    dragging_thumb_ = false;
    thumb_hover_ = false;
    ::ShowWindow(hwnd_, SW_HIDE);
}

void TooltipWindow::ScrollByDipWheel(int wheel_delta) {
    if (!hwnd_) return;
    // Message mode (REQ-R08 compact notice) never scrolls; neither does a body
    // that fits its viewport (short text keeps today's exact look, REQ-003).
    if (is_message_mode_ || !scrollable_) return;
    // REQ-R10: the LL mouse hook callback runs on the hook thread. Marshal the
    // wheel event instead of touching the GUI-thread D2D target directly.
    // WndProc re-enters here already on the GUI thread, so no recursion.
    if (::GetCurrentThreadId() != gui_thread_id_) {
        ::PostMessageW(hwnd_, kScrollMessage, 0, static_cast<LPARAM>(wheel_delta));
        return;
    }
    const float step = WheelDeltaToOffsetStepDip(wheel_delta, line_height_dip_);
    if (step == 0.0f) return;
    const float viewport_h = BodyViewportHeightDip(current_height_);
    const float next = ClampScrollOffset(scroll_offset_dip_ + step, content_height_dip_, viewport_h);
    if (next == scroll_offset_dip_) return; // clamped at an edge: no repaint jitter
    scroll_offset_dip_ = next;
    Render();
    UpdateLayered();
}

void TooltipWindow::SpeakCurrentText() {
    if (translated_text_.empty()) return;
    InitSapi();
    if (voice_) {
        // P4 Batch-3 (REQ-C-002 / D7+D8): the SelectVoiceForLanguage return
        // value is now load-bearing. false = no voice installed for
        // target_lang_ — block the utterance (the old code spoke in the
        // fallback voice anyway, the silent wrong-language defect) and show
        // the transient speech-settings notice instead.
        if (!SelectVoiceForLanguage(target_lang_)) {
            ShowNoVoiceNotice();
            return;
        }
        voice_->Speak(translated_text_.c_str(), SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
    }
}

void TooltipWindow::StopTTS() {
    if (voice_) {
        voice_->Speak(nullptr, SPF_PURGEBEFORESPEAK | SPF_ASYNC, nullptr);
    }
}

void TooltipWindow::ShowNoVoiceNotice() {
    // P4 Batch-3 (REQ-C-002 / D7): REQ-R08 message-mode reuse — brand-
    // consistent header (TooltipTitle, the localized bare brand) + localized
    // guidance body (TooltipNoTtsVoice, Batch-1 37-table string). 3s autohide,
    // generation guard and the compact card all come from ShowMessage itself
    // (kGenNone = unmanaged show: user-triggered feedback, must always
    // render). Anchored at the card's current screen position so the notice
    // replaces the translation panel in place. While it is up, the WM_LBUTTONDOWN
    // early-check opens ms-settings:speech on a click anywhere on the card.
    if (!hwnd_) return;
    RECT rc = {};
    int x = 0;
    int y = 0;
    if (::GetWindowRect(hwnd_, &rc)) {
        x = rc.left;
        y = rc.top;
    }
    ShowMessage(x, y, I18n::Get(StringId::TooltipTitle),
                I18n::Get(StringId::TooltipNoTtsVoice));
    // Arm AFTER the show: ShowMessage clears the flag for every generic
    // notice; only this one routes clicks to the speech settings page.
    no_voice_notice_active_ = true;
}

void TooltipWindow::CopyToClipboard() {
    if (translated_text_.empty()) return;
    SetClipboardText(translated_text_);
    copied_feedback_ = true;
    if (hwnd_) {
        ::SetTimer(hwnd_, kTimerCopiedFeedback, 1500, nullptr);
    }
    Render();
    UpdateLayered();
}

void TooltipWindow::Render() {
    if (!dc_render_target_) return;

    // REQ-R15: DIP layout authored below; BindDC rect is the physical buffer.
    RebindRenderTarget();

    // C1: the single persistent scratch brush is owned by Create()/device-lost
    // recovery, never churned here. The null-branch retry only fires after a
    // failed init (a one-shot self-heal, not per-frame work - once alive the
    // check is a single predictable branch). Still null == no brush on a live
    // target: skip the frame (old code could only offer a cleared empty card).
    if (!scratch_brush_) {
        EnsureScratchBrush();
        if (!scratch_brush_) return;
    }

    // C2: refresh the measured pill widths only when a label string changed;
    // zero CreateTextLayout calls on steady-state renders (scroll/hover bursts).
    EnsureMeasuredLayouts();

    dc_render_target_->BeginDraw();
    dc_render_target_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    // Outer acrylic card container with rounded corners
    D2D1_ROUNDED_RECT card = D2D1::RoundedRect(
        D2D1::RectF(0.5f, 0.5f, static_cast<float>(current_width_) - 0.5f, static_cast<float>(current_height_) - 0.5f),
        10.0f, 10.0f
    );

    // DESIGN-260910 brand pass (session 260910_0005): mirrors about_window.cpp
    // — palette re-anchored to the Emebala brand DNA (lapis blue / antique
    // gold / warm sandstone from assets/). Same slot mapping: slate family ->
    // lapis-tinted equivalents, emerald accent -> brand gold. Message-body
    // #CBD5E1 (last pass's P1 contrast fix) becomes the sand-tinted
    // #D6DCEA at the same WCAG-safe lightness (>= 11:1 on #0C1830). Hover
    // fills, close #EF4444 semantics, and all geometry are untouched.
    // C1 (session 260910_0007): these palette entries are now SetColor
    // arguments on the single scratch_brush_ instead of 6 per-frame
    // CreateSolidColorBrush calls - identical ColorF values, identical draw
    // order. SetColor-just-before-use is safe because the old code consumed
    // every brush immediately at its draw call (aliasing audit: no site held
    // two different-color brushes across each other's use).
    scratch_brush_->SetColor(D2D1::ColorF(0x0C1830, 0.96f));  // poster navy (lapis shadow)
    dc_render_target_->FillRoundedRectangle(card, scratch_brush_);
    scratch_brush_->SetColor(D2D1::ColorF(0x33507E, 0.85f));  // lapis mid
    dc_render_target_->DrawRoundedRectangle(card, scratch_brush_, 1.0f);

    // DESIGN-260910 (P2 glass depth): 1px warm-white rim light inset along the
    // top edge - a Fluent-style specular cue that separates the card from
    // bright / low-contrast host backgrounds. The DIB-sized layered window
    // cannot host a spread outer glow (it would clip at the buffer edge), so
    // the rim is the clipped-safe equivalent of elevation shading. Shared by
    // both the translation card and the message-mode notice below.
    {
        scratch_brush_->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f));
        dc_render_target_->DrawLine(
            D2D1::Point2F(9.0f, 1.5f),
            D2D1::Point2F(static_cast<float>(current_width_) - 9.0f, 1.5f),
            scratch_brush_, 1.0f);
    }

    // --- REQ-R08 message mode: compact header + body notice, no action buttons ---
    if (is_message_mode_) {
        D2D1_RECT_F msgHeaderRect = D2D1::RectF(14.0f, 10.0f, static_cast<float>(current_width_) - 14.0f, 36.0f);
        if (header_format_) {
            scratch_brush_->SetColor(D2D1::ColorF(0xF2ECDC, 1.0f));  // warm sand-white
            dc_render_target_->DrawText(
                message_header_.c_str(), static_cast<UINT32>(message_header_.size()),
                header_format_, msgHeaderRect, scratch_brush_);
        }
        D2D1_RECT_F msgBodyRect = D2D1::RectF(14.0f, 38.0f, static_cast<float>(current_width_) - 14.0f,
                                              static_cast<float>(current_height_) - 10.0f);
        // DESIGN-260910 (P1 readability): the notice body IS the primary
        // content of a message-mode card, but slate-400 #94A3B8 muted it a
        // full step below the header hierarchy on the small 11px format.
        // Slate-300 #CBD5E1 keeps the hierarchy and clears WCAG AA at that
        // size on the #0F172A card.
        if (small_format_) {
            scratch_brush_->SetColor(D2D1::ColorF(0xD6DCEA, 1.0f));  // brand pass: lapis-tinted sand body
            dc_render_target_->DrawText(
                translated_text_.c_str(), static_cast<UINT32>(translated_text_.size()),
                small_format_, msgBodyRect, scratch_brush_);
        }
        // R6 Phase 3 (audit item 4, plan §3.1 A3): the unchecked EndDraw was a
        // latent device-lost gap - after a GPU driver reset D2DERR_RECREATE_TARGET
        // made every later BeginDraw/EndDraw silently fail and the tooltip stayed
        // permanently blank (reads as "stale/empty tooltip"). Recreate the
        // render target + device-dependent logo bitmap so the NEXT show repaints.
        const HRESULT hr_message = dc_render_target_->EndDraw();
        if (IsRecoverableDeviceLost(hr_message)) {
            RecreateAfterDeviceLost();
        }
        return;
    }

    // --- Header Area ---
    // 1. Emebala Brand Logo (Far Left, 1:1 round/squircle frame without horizontal stretching)
    D2D1_ROUNDED_RECT logoFrame = D2D1::RoundedRect(D2D1::RectF(14.0f, 8.0f, 36.0f, 30.0f), 6.0f, 6.0f);
    scratch_brush_->SetColor(D2D1::ColorF(0x1E293B, 0.9f));
    dc_render_target_->FillRoundedRectangle(logoFrame, scratch_brush_);
    scratch_brush_->SetColor(D2D1::ColorF(0xD4AF37, 0.85f));
    dc_render_target_->DrawRoundedRectangle(logoFrame, scratch_brush_, 1.0f);

    D2D1_RECT_F logoRect = D2D1::RectF(16.0f, 10.0f, 34.0f, 28.0f);
    if (logo_bitmap_) {
        dc_render_target_->DrawBitmap(
            logo_bitmap_,
            logoRect,
            1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
        );
    } else {
        DrawTabletLogoVector(dc_render_target_, logoRect, false);
    }

    // C1: logo-frame brushes were released here; the scratch brush persists.

    // 2. Source language dropdown button (clean layout following 1:1 logo
    // frame). F8 (ADR-A1-4): the source tag is now interactive exactly like
    // the target button - a "▾" label, a MEMBER rect (src_btn_rect_) so the
    // hit tests can reach it, and a hover highlight (hover id 5).
    // R6 Phase 5 (plan §5.3): the empty-code fallback was a hardcoded L"Auto";
    // it now uses the localized AutoDetect string.
    float src_tag_x = 44.0f;
    std::wstring src_tag_text = source_lang_code_.empty() ? I18n::Get(StringId::AutoDetect)
                                                          : ToUtf16(source_lang_code_);
    std::wstring src_tag = src_tag_text + L" ▾";
    // F8 (ADR-A1-4): DYNAMIC width - the hardcoded 44.0f box clipped localized
    // Auto-Detect strings and wider codes (e.g. "ZH-CN"). C2 (session
    // 260910_0007): the shaping moved to EnsureMeasuredLayouts (keyed on this
    // exact label string); Render consumes the cached DIP width. 44.0f stays
    // the floor there so measurement failure degrades identically.
    const float src_tag_width = src_tag_width_;
    // F8: persist the button rect as a member for hover/click hit tests.
    src_btn_rect_ = D2D1::RectF(src_tag_x, 8.0f, src_tag_x + src_tag_width, 30.0f);
    D2D1_ROUNDED_RECT srcTagRect = D2D1::RoundedRect(src_btn_rect_, 4.0f, 4.0f);
    scratch_brush_->SetColor((hovered_btn_ == 5) ? D2D1::ColorF(0x334155, 1.0f)
                                                 : D2D1::ColorF(0x1E293B, 0.9f));
    dc_render_target_->FillRoundedRectangle(srcTagRect, scratch_brush_);
    // F8: hover highlight mirrors the target button's hovered_btn_==3
    // pattern - accent border when hovered (tooltip.cpp:1269-1275 model).
    scratch_brush_->SetColor((hovered_btn_ == 5) ? D2D1::ColorF(0xD9B45A, 1.0f)
                                                 : D2D1::ColorF(0x33507E, 0.85f));
    dc_render_target_->DrawRoundedRectangle(srcTagRect, scratch_brush_, 1.0f);
    if (header_format_) {
        scratch_brush_->SetColor(D2D1::ColorF(0x93A3C7, 1.0f));  // lapis-gray
        dc_render_target_->DrawText(src_tag.c_str(), static_cast<UINT32>(src_tag.size()), header_format_, srcTagRect.rect, scratch_brush_);
    }

    // 3. Arrow indicator
    if (header_format_) {
        D2D1_RECT_F arrowRect = D2D1::RectF(src_tag_x + src_tag_width + 4.0f, 8.0f, src_tag_x + src_tag_width + 24.0f, 30.0f);
        scratch_brush_->SetColor(D2D1::ColorF(0x93A3C7, 1.0f));  // lapis-gray
        dc_render_target_->DrawText(L"→", 1, header_format_, arrowRect, scratch_brush_);
    }

    // 4. Target language switcher dropdown button
    // R6 Phase 5 (plan §5.3): the empty-target fallback was a hardcoded
    // L"English"; it now resolves through the localized language registry.
    std::wstring tgt_name = target_lang_.empty() ? I18n::GetLanguageDisplayName("English")
                                                 : ToUtf16(target_lang_);
    std::wstring tgt_label = tgt_name + L" ▾";
    float tgt_btn_x = src_tag_x + src_tag_width + 28.0f;
    float tgt_btn_width = 90.0f;
    lang_btn_rect_ = D2D1::RectF(tgt_btn_x, 8.0f, tgt_btn_x + tgt_btn_width, 30.0f);
    D2D1_ROUNDED_RECT tgtTagRect = D2D1::RoundedRect(lang_btn_rect_, 4.0f, 4.0f);

    scratch_brush_->SetColor((hovered_btn_ == 3) ? D2D1::ColorF(0x334155, 1.0f)
                                                 : D2D1::ColorF(0x1E293B, 0.9f));
    dc_render_target_->FillRoundedRectangle(tgtTagRect, scratch_brush_);
    scratch_brush_->SetColor(D2D1::ColorF(0xD9B45A, 1.0f));  // antique gold accent
    dc_render_target_->DrawRoundedRectangle(tgtTagRect, scratch_brush_, 1.0f);
    if (button_format_) {
        scratch_brush_->SetColor(D2D1::ColorF(0xF2ECDC, 1.0f));  // warm sand-white
        dc_render_target_->DrawText(tgt_label.c_str(), static_cast<UINT32>(tgt_label.size()), button_format_, lang_btn_rect_, scratch_brush_);
    }

    // Close button (top right)
    close_btn_rect_ = D2D1::RectF(static_cast<float>(current_width_) - 32.0f, 8.0f, static_cast<float>(current_width_) - 12.0f, 28.0f);
    if (header_format_) {
        scratch_brush_->SetColor((hovered_btn_ == 4) ? D2D1::ColorF(0xEF4444, 1.0f)
                                                     : D2D1::ColorF(0x94A3B8, 0.8f));
        dc_render_target_->DrawText(L"✕", 1, header_format_, close_btn_rect_, scratch_brush_);
    }

    // Header divider line
    scratch_brush_->SetColor(D2D1::ColorF(0x33507E, 0.5f));
    dc_render_target_->DrawLine(
        D2D1::Point2F(14.0f, 37.0f),
        D2D1::Point2F(static_cast<float>(current_width_) - 14.0f, 37.0f),
        scratch_brush_,
        1.0f
    );

    // --- Body Area: Translated Text (REQ-002: scrollable viewport) ---
    float body_y = kBodyTopDip;
    // When scrolling, reserve an 8 DIP track + 4 DIP gutter at the right edge.
    float max_body_width = static_cast<float>(current_width_ - (scrollable_ ? 32 : 28));
    float body_height = static_cast<float>(current_height_ - 44) - body_y;
    if (body_height < 0.0f) body_height = 0.0f;

    body_viewport_rect_ = D2D1::RectF(14.0f, body_y, 14.0f + max_body_width, body_y + body_height);

    if (!translated_text_.empty() && body_format_) {
        scratch_brush_->SetColor(D2D1::ColorF(0xF2ECDC, 1.0f));  // warm sand-white
        // Clip to the viewport and draw the FULL layout (huge layout extent,
        // never trimmed) shifted up by the scroll offset: overflow becomes
        // reachable via scrolling instead of silently clipped (static
        // re-check: replaces the pre-Batch-2 truncating behavior).
        dc_render_target_->PushAxisAlignedClip(body_viewport_rect_, D2D1_ANTIALIAS_MODE_ALIASED);
        D2D1_RECT_F bodyRect = D2D1::RectF(
            14.0f,
            body_y - scroll_offset_dip_,
            14.0f + max_body_width,
            body_y - scroll_offset_dip_ + (std::max)(content_height_dip_, body_height) + body_height);
        dc_render_target_->DrawText(
            translated_text_.c_str(),
            static_cast<UINT32>(translated_text_.size()),
            body_format_,
            bodyRect,
            scratch_brush_
        );
        dc_render_target_->PopAxisAlignedClip();
    }

    // --- Custom D2D scrollbar (plan §2.1 Option A-lite: native WS_VSCROLL is
    // incompatible with UpdateLayeredWindow; 8 DIP track at the body's right
    // edge, proportional thumb, min 24 DIP) ---
    if (scrollable_) {
        const float track_left = static_cast<float>(current_width_) - 12.0f;
        scrollbar_track_rect_ = D2D1::RectF(track_left, body_y, track_left + 8.0f, body_y + body_height);

        const float thumb_h = ScrollbarThumbHeightDip(body_height, body_height, content_height_dip_);
        const float thumb_top = ScrollbarThumbTopDip(
            body_y, body_height, thumb_h, scroll_offset_dip_, content_height_dip_, body_height);
        scrollbar_thumb_rect_ = D2D1::RectF(track_left, thumb_top, track_left + 8.0f, thumb_top + thumb_h);

        const bool thumb_active = thumb_hover_ || dragging_thumb_;
        // C1: track + thumb brushes used to be alive simultaneously (both
        // created, then each filled once). One scratch brush with SetColor
        // interleaved keeps the identical draw order (track first, thumb on
        // top), so the compositing result is unchanged.
        scratch_brush_->SetColor(D2D1::ColorF(0x1E293B, 0.7f));
        dc_render_target_->FillRoundedRectangle(
            D2D1::RoundedRect(scrollbar_track_rect_, 4.0f, 4.0f), scratch_brush_);
        scratch_brush_->SetColor(thumb_active ? D2D1::ColorF(0xF8FAFC, 1.0f)
                                              : D2D1::ColorF(0x94A3B8, 1.0f));
        dc_render_target_->FillRoundedRectangle(
            D2D1::RoundedRect(scrollbar_thumb_rect_, 4.0f, 4.0f), scratch_brush_);
    } else {
        scrollbar_track_rect_ = {};
        scrollbar_thumb_rect_ = {};
    }

    // Footer divider line
    float footer_div_y = static_cast<float>(current_height_) - 40.0f;
    scratch_brush_->SetColor(D2D1::ColorF(0x33507E, 0.5f));
    dc_render_target_->DrawLine(
        D2D1::Point2F(14.0f, footer_div_y),
        D2D1::Point2F(static_cast<float>(current_width_) - 14.0f, footer_div_y),
        scratch_brush_,
        1.0f
    );

    // --- Footer Action Buttons ---
    // DESIGN-260910 (P1 fit): footer pills size to their LOCALIZED labels
    // (37-locale i18n) measured with the exact button_format_ they are drawn
    // in, so no locale's Copy / Read-aloud label can clip against the old
    // hardcoded 88/82 DIP boxes. Floors keep the previous geometry when
    // measurement fails; caps keep the row inside the 360 DIP card (worst case
    // right edge: 14 + 180 + 8 + 150 = 352 < 360). The copied-feedback label
    // is measured too, so the success state can never outgrow its pill.
    // C2 (session 260910_0007): the three CreateTextLayout measurements moved
    // to EnsureMeasuredLayouts (keyed on the concatenated label strings, so a
    // locale switch re-measures exactly once); Render consumes the cached
    // DIP widths. copy_btn_rect_/tts_btn_rect_ hit-test rects are rebuilt
    // here from the same cached values - measure and hit-test share one
    // source of truth by construction.
    const float copy_btn_w = copy_btn_w_;
    const float tts_btn_w = tts_btn_w_;
    // [📋 Copy] button
    copy_btn_rect_ = D2D1::RectF(14.0f, footer_div_y + 7.0f, 14.0f + copy_btn_w, footer_div_y + 31.0f);
    D2D1_ROUNDED_RECT copyBtnRect = D2D1::RoundedRect(copy_btn_rect_, 4.0f, 4.0f);

    if (copied_feedback_) {
        scratch_brush_->SetColor(D2D1::ColorF(0x064E3B, 0.95f));
        dc_render_target_->FillRoundedRectangle(copyBtnRect, scratch_brush_);
        scratch_brush_->SetColor(D2D1::ColorF(0xD9B45A, 1.0f));  // antique gold accent
        dc_render_target_->DrawRoundedRectangle(copyBtnRect, scratch_brush_, 1.2f);
        if (button_format_) {
            // R6 Phase 5 (plan §5.3): L"✓ Copied!" literal -> StringId.
            const std::wstring feedback = I18n::Get(StringId::TooltipCopied);
            scratch_brush_->SetColor(D2D1::ColorF(0x34D399, 1.0f));
            dc_render_target_->DrawText(feedback.c_str(), static_cast<UINT32>(feedback.size()), button_format_, copy_btn_rect_, scratch_brush_);
        }
    } else {
        scratch_brush_->SetColor((hovered_btn_ == 1) ? D2D1::ColorF(0x334155, 1.0f)
                                                     : D2D1::ColorF(0x1E293B, 0.9f));
        dc_render_target_->FillRoundedRectangle(copyBtnRect, scratch_brush_);
        // DESIGN-260910 (P3 affordance): a hovered pill gains the accent
        // border, matching the header language buttons' hover so
        // interactivity reads identically across the card.
        scratch_brush_->SetColor((hovered_btn_ == 1) ? D2D1::ColorF(0xD9B45A, 1.0f)
                                                     : D2D1::ColorF(0x33507E, 0.85f));
        dc_render_target_->DrawRoundedRectangle(copyBtnRect, scratch_brush_, 1.0f);
        if (button_format_) {
            // R6 Phase 5 (plan §5.3): footer button labels -> StringIds.
            const std::wstring label = I18n::Get(StringId::TooltipButtonCopy);
            scratch_brush_->SetColor(D2D1::ColorF(0xF2ECDC, 1.0f));  // warm sand-white
            dc_render_target_->DrawText(label.c_str(), static_cast<UINT32>(label.size()), button_format_, copy_btn_rect_, scratch_brush_);
        }
    }

    // [🔊 TTS] button
    const float tts_left = 14.0f + copy_btn_w + 8.0f;
    tts_btn_rect_ = D2D1::RectF(tts_left, footer_div_y + 7.0f, tts_left + tts_btn_w, footer_div_y + 31.0f);
    D2D1_ROUNDED_RECT ttsBtnRect = D2D1::RoundedRect(tts_btn_rect_, 4.0f, 4.0f);

    scratch_brush_->SetColor((hovered_btn_ == 2) ? D2D1::ColorF(0x334155, 1.0f)
                                                 : D2D1::ColorF(0x1E293B, 0.9f));
    dc_render_target_->FillRoundedRectangle(ttsBtnRect, scratch_brush_);
    // DESIGN-260910 (P3 affordance): same accent-on-hover as the Copy pill.
    scratch_brush_->SetColor((hovered_btn_ == 2) ? D2D1::ColorF(0xD9B45A, 1.0f)
                                                 : D2D1::ColorF(0x33507E, 0.85f));
    dc_render_target_->DrawRoundedRectangle(ttsBtnRect, scratch_brush_, 1.0f);
    if (button_format_) {
        // R6 Phase 5 (plan §5.3): TTS button label -> StringId.
        const std::wstring label = I18n::Get(StringId::TooltipButtonTts);
        scratch_brush_->SetColor(D2D1::ColorF(0xF2ECDC, 1.0f));  // warm sand-white
        dc_render_target_->DrawText(label.c_str(), static_cast<UINT32>(label.size()), button_format_, tts_btn_rect_, scratch_brush_);
    }

    // C1: the six palette brushes used to be released here; the persistent
    // scratch_brush_ survives the frame (released at Destroy/device-lost).

    // R6 Phase 3 (audit item 4, plan §3.1 A3): see the message-mode note above
    // - identical device-lost recovery on the normal render path.
    const HRESULT hr_normal = dc_render_target_->EndDraw();
    if (IsRecoverableDeviceLost(hr_normal)) {
        RecreateAfterDeviceLost();
    }
}

// R6 Phase 3 (audit item 4, plan §3.1 A3): device-lost recovery. A DC render
// target survives BindDC churn, but D2DERR_RECREATE_TARGET means the D2D
// device is gone (driver reset, desktop transition): the target AND any
// bitmap created from it must be re-created. Deliberately does NOT re-run
// Render() here (a failing second EndDraw would risk unbounded recursion);
// the next natural repaint (Show*, scroll, hover) draws on the new target,
// so the recovery is app-safe degradation, not a silent-permanent-blank.
void TooltipWindow::RecreateAfterDeviceLost() {
    DIAG_F("TOOLTIP/DeviceLost/001: D2DERR_RECREATE_TARGET; recreating render target\n");
    // C1: the scratch brush is device-dependent (bound to the lost target).
    // Release it BEFORE ReleaseTarget so no brush keeps the dead device alive
    // (the leaked per-frame brushes of the old code were only ever reclaimed at
    // Destroy; this is the one behavior delta of the conversion - the old
    // in-frame Release churn on a device-lost target is gone by design).
    ReleaseScratchBrush();
    renderer_.ReleaseTarget(&dc_render_target_);
    if (!d2d_factory_) {
        return; // nothing to rebuild from; all render paths null-guard already
    }
    if (!renderer_.CreateTarget(d2d_factory_, &dc_render_target_)) {
        DIAG_F("TOOLTIP/DeviceLost/002: render-target recreation failed; tooltip stays blank until next Create()\n");
        return;
    }
    // ReallocateBuffer re-binds SetDpi + BindDC on the fresh target; the logo
    // bitmap was created on the lost device and must be rebuilt too.
    ReallocateBuffer(PhysW(), PhysH());
    LoadLogoBitmap();
    EnsureScratchBrush(); // C1: rebuild the scratch brush on the fresh target
}

// R6 Phase 3 (audit items 6+8): free every heap payload still queued for this
// window. PeekMessageW's [min,max] filter removes ONLY the three payload-
// carrying marshal messages; everything else (kDismissMessage, kScrollMessage
// - no heap, timers, input) is left untouched for the normal teardown path.
int TooltipWindow::DrainMarshalQueue() {
    if (!hwnd_) return 0;
    int drained = 0;
    const UINT ids[] = { kShowTranslationMessage, kShowMessageMessage, kRefreshTargetLangMessage };
    for (const UINT id : ids) {
        MSG m = {};
        while (::PeekMessageW(&m, hwnd_, id, id, PM_REMOVE)) {
            switch (id) {
                case kShowTranslationMessage:
                    delete reinterpret_cast<TranslationPayload*>(m.lParam);
                    break;
                case kShowMessageMessage:
                    delete reinterpret_cast<MessagePayload*>(m.lParam);
                    break;
                case kRefreshTargetLangMessage:
                    delete reinterpret_cast<TargetLangPayload*>(m.lParam);
                    break;
                default:
                    break;
            }
            ++drained;
        }
    }
    return drained;
}

void TooltipWindow::UpdateLayered() {
    // REF-3.6: renderer_ performs the GetWindowRect-based ptDst +
    // GetDC(nullptr) ULW sequence identical to the previous inline code.
    // REQ-R15: physical blit size; alpha fixed 245 (~96% opacity) per §6.
    renderer_.Present(hwnd_, PhysW(), PhysH(), 245);
}

LRESULT CALLBACK TooltipWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* pThis = reinterpret_cast<TooltipWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = static_cast<TooltipWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    if (!pThis) {
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
        // ---- REQ-R10 (audit §3.4) marshaled render requests ----
        // These run on the GUI thread that owns the D2D target. The unique_ptr
        // claims the heap payload posted via LPARAM; unconsumed payloads were
        // already freed by the posting seam when PostMessage failed.
        case kShowTranslationMessage: {
            const std::unique_ptr<TranslationPayload> p(
                reinterpret_cast<TranslationPayload*>(lParam));
            if (p) {
                // R6 B1-H1: the payload carries the originating generation;
                // ShowTranslation drops it on the GUI thread if superseded.
                pThis->ShowTranslation(p->x, p->y, p->source_text,
                                       p->source_lang_code, p->target_lang, p->translated_text,
                                       p->generation);
            }
            return 0;
        }

        case kShowMessageMessage: {
            const std::unique_ptr<MessagePayload> p(
                reinterpret_cast<MessagePayload*>(lParam));
            if (p) {
                pThis->ShowMessage(p->x, p->y, p->header, p->body, p->generation);
            }
            return 0;
        }

        case kDismissMessage: {
            pThis->Dismiss();
            return 0;
        }

        // R6 Phase 1 (B3): marshaled target-language view refresh. The unique_ptr
        // claims the heap payload posted by RefreshTargetLanguageFromConfig() from
        // a non-GUI thread; unconsumed payloads were freed by the posting seam.
        case kRefreshTargetLangMessage: {
            const std::unique_ptr<TargetLangPayload> p(
                reinterpret_cast<TargetLangPayload*>(lParam));
            if (p) {
                pThis->RefreshTargetLanguageFromConfig(p->target_lang);
            }
            return 0;
        }

        // ---- REQ-002 scroll input ----
        // Primary path: kScrollMessage posted by the LL mouse hook callback
        // (WS_EX_NOACTIVATE popups never receive routed WM_MOUSEWHEEL because
        // Windows sends the wheel to the FOCUSED window, not the hovered one).
        case kScrollMessage: {
            pThis->ScrollByDipWheel(static_cast<int>(lParam));
            return 0;
        }

        // Defensive secondary path: if the OS ever routes a real wheel message
        // here (e.g. focus quirks), honor it instead of dropping it.
        case WM_MOUSEWHEEL: {
            pThis->ScrollByDipWheel(static_cast<int>(GET_WHEEL_DELTA_WPARAM(wParam)));
            return 0;
        }

        case WM_SETCURSOR: {
            POINT pt = {};
            ::GetCursorPos(&pt);
            ::ScreenToClient(hwnd, &pt);
            // REQ-R15: client coordinates are PHYSICAL px; the button rects
            // are DIP layout. Convert before hit-testing, or on 150%/200%
            // monitors the hover/click zones sit left/above the painted
            // buttons (the audited "coordinate offset" symptom).
            pt.x = emebalachat::ui::ScalePixelsToDips(pt.x, pThis->dpi_);
            pt.y = emebalachat::ui::ScalePixelsToDips(pt.y, pThis->dpi_);
            float x = static_cast<float>(pt.x);
            float y = static_cast<float>(pt.y);

            if (pThis->is_message_mode_) {
                ::SetCursor(::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
                return TRUE;
            }

            if (IsPointInRect(pThis->copy_btn_rect_, x, y) ||
                IsPointInRect(pThis->tts_btn_rect_, x, y) ||
                // F8 (ADR-A1-4): the source button is interactive now.
                IsPointInRect(pThis->src_btn_rect_, x, y) ||
                IsPointInRect(pThis->lang_btn_rect_, x, y) ||
                IsPointInRect(pThis->close_btn_rect_, x, y) ||
                (pThis->scrollable_ && IsPointInRect(pThis->scrollbar_thumb_rect_, x, y))) {
                ::SetCursor(::LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
                return TRUE;
            }
            ::SetCursor(::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
            return TRUE;
        }

        case WM_MOUSEMOVE: {
            // REQ-R15: lParam client coords are physical px -> DIP for the
            // hit-test (same conversion + rationale as WM_SETCURSOR).
            float x = static_cast<float>(emebalachat::ui::ScalePixelsToDips(
                static_cast<int>(static_cast<short>(LOWORD(lParam))), pThis->dpi_));
            float y = static_cast<float>(emebalachat::ui::ScalePixelsToDips(
                static_cast<int>(static_cast<short>(HIWORD(lParam))), pThis->dpi_));

            bool repaint = false;

            // REQ-002: thumb drag (SetCapture held since WM_LBUTTONDOWN).
            // Relative mapping keeps the grab point under the cursor:
            // pointer travel across the track scales to the scroll range
            // (plan §2.1; overshoot clamps, no jitter, edge cases 2/3).
            if (pThis->dragging_thumb_) {
                const float track_h = pThis->scrollbar_track_rect_.bottom - pThis->scrollbar_track_rect_.top;
                const float thumb_h = pThis->scrollbar_thumb_rect_.bottom - pThis->scrollbar_thumb_rect_.top;
                const float viewport_h = TooltipWindow::BodyViewportHeightDip(pThis->current_height_);
                const float range = track_h - thumb_h;
                const float scroll_range = pThis->content_height_dip_ - viewport_h;
                if (range > 0.0f && scroll_range > 0.0f) {
                    const float dy = y - pThis->drag_start_y_dip_;
                    const float next = TooltipWindow::ClampScrollOffset(
                        pThis->drag_start_offset_dip_ + dy * scroll_range / range,
                        pThis->content_height_dip_, viewport_h);
                    if (next != pThis->scroll_offset_dip_) {
                        pThis->scroll_offset_dip_ = next;
                        repaint = true;
                    }
                }
            }

            int new_hover = 0;
            if (IsPointInRect(pThis->copy_btn_rect_, x, y)) new_hover = 1;
            else if (IsPointInRect(pThis->tts_btn_rect_, x, y)) new_hover = 2;
            // F8 (ADR-A1-4): source button hover (id 5) before the target
            // button so the leftmost header control wins on any overlap.
            else if (IsPointInRect(pThis->src_btn_rect_, x, y)) new_hover = 5;
            else if (IsPointInRect(pThis->lang_btn_rect_, x, y)) new_hover = 3;
            else if (IsPointInRect(pThis->close_btn_rect_, x, y)) new_hover = 4;

            const bool new_thumb_hover =
                pThis->scrollable_ && !pThis->dragging_thumb_ &&
                IsPointInRect(pThis->scrollbar_thumb_rect_, x, y);

            if (new_hover != pThis->hovered_btn_ || new_thumb_hover != pThis->thumb_hover_) {
                pThis->hovered_btn_ = new_hover;
                pThis->thumb_hover_ = new_thumb_hover;
                repaint = true;
            }

            if (repaint) {
                pThis->Render();
                pThis->UpdateLayered();
            }

            if (new_hover != 0 || new_thumb_hover) {
                TRACKMOUSEEVENT tme = {};
                tme.cbSize = sizeof(TRACKMOUSEEVENT);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                ::TrackMouseEvent(&tme);
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            // During a thumb drag the capture keeps mouse events coming even
            // when the pointer leaves; ignore leave until the drag ends.
            if (pThis->dragging_thumb_) {
                return 0;
            }
            if (pThis->hovered_btn_ != 0 || pThis->thumb_hover_) {
                pThis->hovered_btn_ = 0;
                pThis->thumb_hover_ = false;
                pThis->Render();
                pThis->UpdateLayered();
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            // P4 Batch-3 (REQ-C-002 / D7): while the no-voice notice is up, a
            // click ANYWHERE on the card opens the official Windows speech
            // settings page (free voice-packs store, ms-settings:speech deep
            // link). Single-shot: the flag is consumed BEFORE the ShellExecute
            // so a double-fire cannot open two settings pages. Failure keeps a
            // traceable code, same pattern as about_window.cpp OpenLink
            // (INT_PTR <= 32 == error). The click is consumed here only — the
            // notice dismissal rides the EXISTING tested message-mode
            // WM_LBUTTONUP path (is_message_mode_ -> Dismiss), keeping the
            // card's click lifecycle unchanged (no UP fall-through onto stale
            // footer-button rects after an early Dismiss).
            if (pThis->no_voice_notice_active_) {
                pThis->no_voice_notice_active_ = false;
                HINSTANCE hRes = ::ShellExecuteW(nullptr, L"open", L"ms-settings:speech",
                                                 nullptr, nullptr, SW_SHOWNORMAL);
                if (reinterpret_cast<INT_PTR>(hRes) <= 32) {
                    DIAG_F("TTS/NoVoice/001: ShellExecuteW ms-settings:speech failed (INT_PTR %lld)\n",
                           static_cast<long long>(reinterpret_cast<INT_PTR>(hRes)));
                }
                return 0;
            }
            // REQ-002: begin a scrollbar thumb drag (plan §2.1). SetCapture so
            // moves outside the layered popup keep feeding the drag math.
            if (!pThis->is_message_mode_ && pThis->scrollable_ &&
                IsPointInRect(pThis->scrollbar_thumb_rect_,
                              static_cast<float>(emebalachat::ui::ScalePixelsToDips(
                                  static_cast<int>(static_cast<short>(LOWORD(lParam))), pThis->dpi_)),
                              static_cast<float>(emebalachat::ui::ScalePixelsToDips(
                                  static_cast<int>(static_cast<short>(HIWORD(lParam))), pThis->dpi_)))) {
                pThis->dragging_thumb_ = true;
                pThis->thumb_hover_ = true;
                pThis->drag_start_offset_dip_ = pThis->scroll_offset_dip_;
                pThis->drag_start_y_dip_ = static_cast<float>(emebalachat::ui::ScalePixelsToDips(
                    static_cast<int>(static_cast<short>(HIWORD(lParam))), pThis->dpi_));
                ::SetCapture(hwnd);
                pThis->Render();
                pThis->UpdateLayered();
                return 0;
            }
            break;
        }

        case WM_LBUTTONUP: {
            // REQ-002: end a thumb drag first; swallow the release so a drag
            // that ends over a footer button does not also fire its action.
            if (pThis->dragging_thumb_) {
                pThis->dragging_thumb_ = false;
                ::ReleaseCapture();
                pThis->Render();
                pThis->UpdateLayered();
                return 0;
            }

            // Message-mode notice has no action buttons: any click dismisses.
            if (pThis->is_message_mode_) {
                pThis->Dismiss();
                return 0;
            }

            // REQ-R15: physical client px -> DIP (see WM_MOUSEMOVE note).
            float x = static_cast<float>(emebalachat::ui::ScalePixelsToDips(
                static_cast<int>(static_cast<short>(LOWORD(lParam))), pThis->dpi_));
            float y = static_cast<float>(emebalachat::ui::ScalePixelsToDips(
                static_cast<int>(static_cast<short>(HIWORD(lParam))), pThis->dpi_));

            if (IsPointInRect(pThis->copy_btn_rect_, x, y)) {
                pThis->CopyToClipboard();
                return 0;
            }

            if (IsPointInRect(pThis->tts_btn_rect_, x, y)) {
                pThis->SpeakCurrentText();
                return 0;
            }

            if (IsPointInRect(pThis->close_btn_rect_, x, y)) {
                pThis->Dismiss();
                return 0;
            }

            // F8 (ADR-A1-4): source-language dropdown. The source menu uses
            // GetSupportedLanguages() (AUTO + 37 = 38 entries) - NOT the
            // target list (AUTO is excluded there because it can never be a
            // translation target, but it is a first-class source setting:
            // "기본값은 자동감지"). Picking AUTO re-records the persisted
            // drag_source_language as "Auto Detect" via the main.cpp callback
            // (re-record contract, user decision ④ / ADR-A1-4).
            if (IsPointInRect(pThis->src_btn_rect_, x, y)) {
                DIAG_LOG("UI", "tooltip src_menu_open count=%zu",
                         GetSupportedLanguages().size());
                HMENU hMenu = ::CreatePopupMenu();
                const auto& src_langs = GetSupportedLanguages();
                // Check mark follows the tag's EFFECTIVE source (the displayed
                // value): the AUTO entry when the tag is showing the localized
                // Auto-Detect fallback (empty code), otherwise the entry whose
                // code matches the normalized effective code. This is the
                // same effective-value convention the target menu already uses
                // (ADR-A1-4 display doctrine - the tag shows the effective
                // source, so the check reflects what the user sees).
                const std::string norm_src =
                    pThis->source_lang_code_.empty()
                        ? std::string("AUTO")
                        : NormalizeLanguageCode(pThis->source_lang_code_);
                for (size_t i = 0; i < src_langs.size(); ++i) {
                    std::wstring item = ToUtf16(src_langs[i].name_en) + L" (" + ToUtf16(src_langs[i].name_native) + L")";
                    UINT flags = MF_STRING;
                    if (norm_src == src_langs[i].code) {
                        flags |= MF_CHECKED;
                    }
                    ::AppendMenuW(hMenu, flags, static_cast<UINT_PTR>(i + 1), item.c_str());
                }

                // REQ-R15: the dropdown anchor is DIP layout coords; the
                // window-to-screen conversion works in physical px (same as
                // the target menu below).
                POINT pt = {
                    emebalachat::ui::ScaleDipsToPixels(static_cast<int>(pThis->src_btn_rect_.left), pThis->dpi_),
                    emebalachat::ui::ScaleDipsToPixels(static_cast<int>(pThis->src_btn_rect_.bottom), pThis->dpi_)
                };
                ::ClientToScreen(hwnd, &pt);
                ::SetForegroundWindow(hwnd);

                int cmd = ::TrackPopupMenuEx(hMenu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, hwnd, nullptr);
                ::DestroyMenu(hMenu);

                if (cmd > 0 && static_cast<size_t>(cmd - 1) < src_langs.size()) {
                    // name_en payload (the AUTO entry yields "Auto Detect") -
                    // unified with the target menu's payload form (ADR-A1-4).
                    std::string new_src = src_langs[cmd - 1].name_en;
                    DIAG_LOG("UI", "tooltip src_menu_select lang=%s",
                             new_src.c_str());
                    if (pThis->src_lang_change_cb_) {
                        pThis->src_lang_change_cb_(new_src);
                    }
                }
                return 0;
            }

            if (IsPointInRect(pThis->lang_btn_rect_, x, y)) {
                // Language switcher dropdown menu
                HMENU hMenu = ::CreatePopupMenu();
                const auto& target_langs = GetTargetLanguages();
                for (size_t i = 0; i < target_langs.size(); ++i) {
                    std::wstring item = ToUtf16(target_langs[i].name_en) + L" (" + ToUtf16(target_langs[i].name_native) + L")";
                    UINT flags = MF_STRING;
                    if (target_langs[i].name_en == pThis->target_lang_ || target_langs[i].code == pThis->target_lang_) {
                        flags |= MF_CHECKED;
                    }
                    ::AppendMenuW(hMenu, flags, static_cast<UINT_PTR>(i + 1), item.c_str());
                }

                // REQ-R15: the dropdown anchor is DIP layout coords; the
                // window-to-screen conversion works in physical px.
                POINT pt = {
                    emebalachat::ui::ScaleDipsToPixels(static_cast<int>(pThis->lang_btn_rect_.left), pThis->dpi_),
                    emebalachat::ui::ScaleDipsToPixels(static_cast<int>(pThis->lang_btn_rect_.bottom), pThis->dpi_)
                };
                ::ClientToScreen(hwnd, &pt);
                ::SetForegroundWindow(hwnd);

                int cmd = ::TrackPopupMenuEx(hMenu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, hwnd, nullptr);
                ::DestroyMenu(hMenu);

                if (cmd > 0 && static_cast<size_t>(cmd - 1) < target_langs.size()) {
                    std::string new_lang = target_langs[cmd - 1].name_en;
                    if (pThis->lang_change_cb_) {
                        pThis->lang_change_cb_(new_lang);
                    }
                }
                return 0;
            }

            return 0;
        }

        // D1 (debug report T3): live per-monitor DPI change while visible.
        // Without this handler the physical DIB captured at show time (see
        // ShowTranslation's MonitorDpiAtPoint) goes stale and the compositor
        // scales the UpdateLayeredWindow blit - a soft tooltip until the next
        // show. Mirrors badge.cpp's cross-DPI re-allocation discipline: update
        // dpi_ through the ui/dpi helper, resize/reposition to the new scale,
        // reallocate the physical buffer (which re-binds SetDpi + BindDC),
        // then re-render the DIP layout at the correct scale.
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            pThis->dpi_ = emebalachat::ui::WindowDpi(hwnd);
            // Keep our own PhysW()/PhysH() extents (ScaleDipsToPixels rounding)
            // instead of the suggested size: DIB and window rect must match
            // exactly or the blit is rescaled - the very defect being fixed.
            ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                           pThis->PhysW(), pThis->PhysH(),
                           SWP_NOZORDER | SWP_NOACTIVATE);
            pThis->ReallocateBuffer(pThis->PhysW(), pThis->PhysH());
            pThis->Render();
            pThis->UpdateLayered();
            return 0;
        }

        case WM_TIMER: {
            if (wParam == kTimerCopiedFeedback) {
                ::KillTimer(hwnd, kTimerCopiedFeedback);
                pThis->copied_feedback_ = false;
                pThis->Render();
                pThis->UpdateLayered();
                return 0;
            }
            if (wParam == kTimerMessageAutohide) {
                pThis->Dismiss();
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
