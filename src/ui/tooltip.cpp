#include "tooltip.hpp"
#include "asset_loader.hpp"
#include "../config.hpp"
#include "../i18n.hpp"
#include "../unicode_utils.hpp"
#include "../win32_input.hpp"

#include <algorithm>
#include <cmath>

#include <cctype>

namespace emebalachat {

namespace {
const wchar_t kTooltipClassName[] = L"Emebalachat_TooltipClass";

bool IsPointInRect(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

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
        L"Emebalachat Translation Tooltip",
        WS_POPUP,
        -1000, -1000, current_width_, current_height_,
        nullptr, nullptr, hInstance_, this
    );

    if (!hwnd_) {
        return false;
    }

    ReallocateBuffer(current_width_, current_height_);

    // Direct2D & DirectWrite Factories
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

    RECT rc = { 0, 0, current_width_, current_height_ };
    dc_render_target_->BindDC(hMemDC_, &rc);

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
        if (default_voice_token_) {
            voice_->SetVoice(default_voice_token_);
            current_voice_name_ = GetTokenName(default_voice_token_);
            current_voice_lang_ = GetTokenLanguage(default_voice_token_);
        }
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
                    int base_score = (cat_idx == 0) ? 80 : 40;
                    int score = base_score + ((match == 2) ? 20 : 0);

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
        return true;
    }

    // Fallback safely to default voice
    if (default_voice_token_) {
        voice_->SetVoice(default_voice_token_);
        current_voice_name_ = GetTokenName(default_voice_token_);
        current_voice_lang_ = GetTokenLanguage(default_voice_token_);
    }
    return false;
}

void TooltipWindow::ReallocateBuffer(int width, int height) {
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

void TooltipWindow::ShowTranslation(
    int x, int y,
    std::wstring_view source_text,
    std::string_view source_lang_code,
    std::string_view target_lang,
    std::wstring_view translated_text
) {
    if (!hwnd_) return;

    // REQ-R10 (audit §3.4): thread affinity guard. Hook/worker threads are
    // marshaled to the owning GUI thread instead of touching D2D cross-thread
    // (D2DERR_WRONG_THREAD -> invisible tooltip). Same-thread callers (badge
    // click copy path, tests, the WndProc-marshaled re-entry) run directly.
    if (::GetCurrentThreadId() != gui_thread_id_) {
        ShowTranslationThreadSafe(x, y, source_text, source_lang_code, target_lang, translated_text);
        return;
    }

    is_message_mode_ = false;
    ::KillTimer(hwnd_, kTimerMessageAutohide);

    source_text_ = source_text;
    source_lang_code_ = source_lang_code;
    target_lang_ = target_lang;
    translated_text_ = translated_text;
    copied_feedback_ = false;
    hovered_btn_ = 0;

    // Measure body text layout height
    float max_text_width = static_cast<float>(current_width_ - 32);
    IDWriteTextLayout* layout = nullptr;
    float text_height = 40.0f;
    if (dwrite_factory_ && body_format_) {
        dwrite_factory_->CreateTextLayout(
            translated_text_.c_str(),
            static_cast<UINT32>(translated_text_.size()),
            body_format_,
            max_text_width,
            1000.0f,
            &layout
        );
        if (layout) {
            DWRITE_TEXT_METRICS m = {};
            layout->GetMetrics(&m);
            text_height = (std::max)(36.0f, m.height);
            layout->Release();
        }
    }

    // Calculate dynamic window height: header (38) + pad (12) + body + pad (16) + footer (36)
    int calculated_height = static_cast<int>(std::ceil(38.0f + 12.0f + text_height + 16.0f + 36.0f));
    current_height_ = (std::max)(140, (std::min)(calculated_height, 480));

    ReallocateBuffer(current_width_, current_height_);

    // Multi-monitor aware bounds clamping
    POINT pt = { x, y };
    HMONITOR hMon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(MONITORINFO);
    if (::GetMonitorInfoW(hMon, &mi)) {
        if (x + current_width_ > mi.rcWork.right) x = mi.rcWork.right - current_width_ - 10;
        if (y + current_height_ > mi.rcWork.bottom) y = mi.rcWork.bottom - current_height_ - 10;
        if (x < mi.rcWork.left) x = mi.rcWork.left + 10;
        if (y < mi.rcWork.top) y = mi.rcWork.top + 10;
    }

    ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, current_width_, current_height_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;

    Render();
    UpdateLayered();
}

void TooltipWindow::ShowMessage(int x, int y, std::wstring_view header, std::wstring_view body) {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() != gui_thread_id_) {
        ShowMessageThreadSafe(x, y, header, body);
        return;
    }

    // REQ-R08 (audit §3.2): transient state-change notice. Compact fixed-size
    // card; translated_text_ carries the body line, message_header_ the title.
    is_message_mode_ = true;
    message_header_ = header;
    translated_text_ = body;
    source_text_.clear();
    source_lang_code_.clear();
    target_lang_.clear();
    copied_feedback_ = false;
    hovered_btn_ = 0;

    current_width_ = 320;
    current_height_ = 84;
    ReallocateBuffer(current_width_, current_height_);

    // Multi-monitor aware bounds clamping (same policy as ShowTranslation).
    POINT pt = { x, y };
    HMONITOR hMon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(MONITORINFO);
    if (::GetMonitorInfoW(hMon, &mi)) {
        if (x + current_width_ > mi.rcWork.right) x = mi.rcWork.right - current_width_ - 10;
        if (y + current_height_ > mi.rcWork.bottom) y = mi.rcWork.bottom - current_height_ - 10;
        if (x < mi.rcWork.left) x = mi.rcWork.left + 10;
        if (y < mi.rcWork.top) y = mi.rcWork.top + 10;
    }

    ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, current_width_, current_height_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;

    Render();
    UpdateLayered();

    ::KillTimer(hwnd_, kTimerMessageAutohide);
    ::SetTimer(hwnd_, kTimerMessageAutohide, kMessageAutohideMs, nullptr);
}

void TooltipWindow::ShowMessageThreadSafe(int x, int y, std::wstring_view header, std::wstring_view body) {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() == gui_thread_id_) {
        ShowMessage(x, y, header, body);
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
    std::wstring_view translated_text
) {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() == gui_thread_id_) {
        ShowTranslation(x, y, source_text, source_lang_code, target_lang, translated_text);
        return;
    }
    auto payload = std::make_unique<TranslationPayload>();
    payload->x = x;
    payload->y = y;
    payload->source_text = source_text;
    payload->source_lang_code = source_lang_code;
    payload->target_lang = target_lang;
    payload->translated_text = translated_text;
    const LPARAM lparam = reinterpret_cast<LPARAM>(payload.release());
    if (::PostMessageW(hwnd_, kShowTranslationMessage, 0, lparam) == FALSE) {
        delete reinterpret_cast<TranslationPayload*>(lparam);
    }
}

void TooltipWindow::DismissThreadSafe() {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() == gui_thread_id_) {
        Dismiss();
        return;
    }
    ::PostMessageW(hwnd_, kDismissMessage, 0, 0);
}

void TooltipWindow::Dismiss() {
    if (!hwnd_) return;
    // REQ-R10 companion: the ESC (keyboard hook) and outside-click (mouse hook)
    // dismissal paths call Dismiss() off the GUI thread. ShowWindow/SAPI on
    // another thread's window can block until that pump is free (e.g. mid
    // translation), which would stall LowLevelHooksTimeout. Marshal instead.
    // WndProc re-enters on the GUI thread, so this guard never recurses.
    if (::GetCurrentThreadId() != gui_thread_id_) {
        ::PostMessageW(hwnd_, kDismissMessage, 0, 0);
        return;
    }
    if (!visible_.load(std::memory_order_relaxed)) return;
    StopTTS();
    ::KillTimer(hwnd_, kTimerMessageAutohide);
    visible_ = false;
    hovered_btn_ = 0;
    copied_feedback_ = false;
    is_message_mode_ = false;
    ::ShowWindow(hwnd_, SW_HIDE);
}

void TooltipWindow::SpeakCurrentText() {
    if (translated_text_.empty()) return;
    InitSapi();
    if (voice_) {
        SelectVoiceForLanguage(target_lang_);
        voice_->Speak(translated_text_.c_str(), SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
    }
}

void TooltipWindow::StopTTS() {
    if (voice_) {
        voice_->Speak(nullptr, SPF_PURGEBEFORESPEAK | SPF_ASYNC, nullptr);
    }
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

    RECT rc = { 0, 0, current_width_, current_height_ };
    dc_render_target_->BindDC(hMemDC_, &rc);

    dc_render_target_->BeginDraw();
    dc_render_target_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    // Outer acrylic card container with rounded corners
    D2D1_ROUNDED_RECT card = D2D1::RoundedRect(
        D2D1::RectF(0.5f, 0.5f, static_cast<float>(current_width_) - 0.5f, static_cast<float>(current_height_) - 0.5f),
        10.0f, 10.0f
    );

    ID2D1SolidColorBrush* bgBrush = nullptr;
    ID2D1SolidColorBrush* borderBrush = nullptr;
    ID2D1SolidColorBrush* textBrush = nullptr;
    ID2D1SolidColorBrush* subTextBrush = nullptr;
    ID2D1SolidColorBrush* dividerBrush = nullptr;
    ID2D1SolidColorBrush* accentBrush = nullptr;
    ID2D1SolidColorBrush* btnBgBrush = nullptr;

    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x0F172A, 0.96f), &bgBrush);
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x334155, 0.85f), &borderBrush);
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0xF8FAFC, 1.0f), &textBrush);
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x94A3B8, 1.0f), &subTextBrush);
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x334155, 0.5f), &dividerBrush);
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x10B981, 1.0f), &accentBrush);

    if (bgBrush) dc_render_target_->FillRoundedRectangle(card, bgBrush);
    if (borderBrush) dc_render_target_->DrawRoundedRectangle(card, borderBrush, 1.0f);

    // --- REQ-R08 message mode: compact header + body notice, no action buttons ---
    if (is_message_mode_) {
        D2D1_RECT_F msgHeaderRect = D2D1::RectF(14.0f, 10.0f, static_cast<float>(current_width_) - 14.0f, 36.0f);
        if (header_format_ && textBrush) {
            dc_render_target_->DrawText(
                message_header_.c_str(), static_cast<UINT32>(message_header_.size()),
                header_format_, msgHeaderRect, textBrush);
        }
        D2D1_RECT_F msgBodyRect = D2D1::RectF(14.0f, 38.0f, static_cast<float>(current_width_) - 14.0f,
                                              static_cast<float>(current_height_) - 10.0f);
        if (small_format_ && subTextBrush) {
            dc_render_target_->DrawText(
                translated_text_.c_str(), static_cast<UINT32>(translated_text_.size()),
                small_format_, msgBodyRect, subTextBrush);
        }
        if (accentBrush) accentBrush->Release();
        if (dividerBrush) dividerBrush->Release();
        if (subTextBrush) subTextBrush->Release();
        if (textBrush) textBrush->Release();
        if (borderBrush) borderBrush->Release();
        if (bgBrush) bgBrush->Release();
        dc_render_target_->EndDraw();
        return;
    }

    // --- Header Area ---
    // 1. Emebala Brand Logo (Far Left)
    D2D1_ROUNDED_RECT logoFrame = D2D1::RoundedRect(D2D1::RectF(14.0f, 8.0f, 44.0f, 30.0f), 4.0f, 4.0f);
    ID2D1SolidColorBrush* logoBgBrush = nullptr;
    ID2D1SolidColorBrush* goldBorderBrush = nullptr;
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x1E293B, 0.9f), &logoBgBrush);
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0xD4AF37, 0.85f), &goldBorderBrush);

    if (logoBgBrush) {
        dc_render_target_->FillRoundedRectangle(logoFrame, logoBgBrush);
    }
    if (goldBorderBrush) {
        dc_render_target_->DrawRoundedRectangle(logoFrame, goldBorderBrush, 1.0f);
    }

    D2D1_RECT_F logoRect = D2D1::RectF(16.0f, 10.75f, 42.0f, 27.25f);
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

    if (goldBorderBrush) goldBorderBrush->Release();
    if (logoBgBrush) logoBgBrush->Release();

    // 2. Source language badge (shifted to accommodate logo)
    float src_tag_x = 52.0f;
    std::wstring src_tag = source_lang_code_.empty() ? L"Auto" : ToUtf16(source_lang_code_);
    float src_tag_width = 44.0f;
    D2D1_ROUNDED_RECT srcTagRect = D2D1::RoundedRect(D2D1::RectF(src_tag_x, 8.0f, src_tag_x + src_tag_width, 30.0f), 4.0f, 4.0f);
    dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x1E293B, 0.9f), &btnBgBrush);
    if (btnBgBrush) {
        dc_render_target_->FillRoundedRectangle(srcTagRect, btnBgBrush);
        dc_render_target_->DrawRoundedRectangle(srcTagRect, borderBrush, 1.0f);
        btnBgBrush->Release();
        btnBgBrush = nullptr;
    }
    if (header_format_ && subTextBrush) {
        dc_render_target_->DrawText(src_tag.c_str(), static_cast<UINT32>(src_tag.size()), header_format_, srcTagRect.rect, subTextBrush);
    }

    // 3. Arrow indicator
    if (header_format_ && subTextBrush) {
        D2D1_RECT_F arrowRect = D2D1::RectF(src_tag_x + src_tag_width + 4.0f, 8.0f, src_tag_x + src_tag_width + 24.0f, 30.0f);
        dc_render_target_->DrawText(L"→", 1, header_format_, arrowRect, subTextBrush);
    }

    // 4. Target language switcher dropdown button
    std::wstring tgt_name = target_lang_.empty() ? L"English" : ToUtf16(target_lang_);
    std::wstring tgt_label = tgt_name + L" ▾";
    float tgt_btn_x = src_tag_x + src_tag_width + 28.0f;
    float tgt_btn_width = 90.0f;
    lang_btn_rect_ = D2D1::RectF(tgt_btn_x, 8.0f, tgt_btn_x + tgt_btn_width, 30.0f);
    D2D1_ROUNDED_RECT tgtTagRect = D2D1::RoundedRect(lang_btn_rect_, 4.0f, 4.0f);

    dc_render_target_->CreateSolidColorBrush(
        (hovered_btn_ == 3) ? D2D1::ColorF(0x334155, 1.0f) : D2D1::ColorF(0x1E293B, 0.9f),
        &btnBgBrush
    );
    if (btnBgBrush) {
        dc_render_target_->FillRoundedRectangle(tgtTagRect, btnBgBrush);
        dc_render_target_->DrawRoundedRectangle(tgtTagRect, accentBrush, 1.0f);
        btnBgBrush->Release();
        btnBgBrush = nullptr;
    }
    if (button_format_ && textBrush) {
        dc_render_target_->DrawText(tgt_label.c_str(), static_cast<UINT32>(tgt_label.size()), button_format_, lang_btn_rect_, textBrush);
    }

    // Close button (top right)
    close_btn_rect_ = D2D1::RectF(static_cast<float>(current_width_) - 32.0f, 8.0f, static_cast<float>(current_width_) - 12.0f, 28.0f);
    if (header_format_) {
        ID2D1SolidColorBrush* closeBrush = nullptr;
        dc_render_target_->CreateSolidColorBrush(
            (hovered_btn_ == 4) ? D2D1::ColorF(0xEF4444, 1.0f) : D2D1::ColorF(0x94A3B8, 0.8f),
            &closeBrush
        );
        if (closeBrush) {
            dc_render_target_->DrawText(L"✕", 1, header_format_, close_btn_rect_, closeBrush);
            closeBrush->Release();
        }
    }

    // Header divider line
    if (dividerBrush) {
        dc_render_target_->DrawLine(
            D2D1::Point2F(14.0f, 37.0f),
            D2D1::Point2F(static_cast<float>(current_width_) - 14.0f, 37.0f),
            dividerBrush,
            1.0f
        );
    }

    // --- Body Area: Translated Text ---
    float body_y = 48.0f;
    float max_body_width = static_cast<float>(current_width_ - 28);
    float body_height = static_cast<float>(current_height_ - 44) - body_y;

    if (!translated_text_.empty() && body_format_ && textBrush) {
        D2D1_RECT_F bodyRect = D2D1::RectF(14.0f, body_y, 14.0f + max_body_width, body_y + body_height);
        dc_render_target_->DrawText(
            translated_text_.c_str(),
            static_cast<UINT32>(translated_text_.size()),
            body_format_,
            bodyRect,
            textBrush
        );
    }

    // Footer divider line
    float footer_div_y = static_cast<float>(current_height_) - 40.0f;
    if (dividerBrush) {
        dc_render_target_->DrawLine(
            D2D1::Point2F(14.0f, footer_div_y),
            D2D1::Point2F(static_cast<float>(current_width_) - 14.0f, footer_div_y),
            dividerBrush,
            1.0f
        );
    }

    // --- Footer Action Buttons ---
    // [📋 Copy] button
    copy_btn_rect_ = D2D1::RectF(14.0f, footer_div_y + 7.0f, 102.0f, footer_div_y + 31.0f);
    D2D1_ROUNDED_RECT copyBtnRect = D2D1::RoundedRect(copy_btn_rect_, 4.0f, 4.0f);

    if (copied_feedback_) {
        dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x064E3B, 0.95f), &btnBgBrush);
        if (btnBgBrush) {
            dc_render_target_->FillRoundedRectangle(copyBtnRect, btnBgBrush);
            dc_render_target_->DrawRoundedRectangle(copyBtnRect, accentBrush, 1.2f);
            btnBgBrush->Release();
            btnBgBrush = nullptr;
        }
        if (button_format_) {
            ID2D1SolidColorBrush* feedbackBrush = nullptr;
            dc_render_target_->CreateSolidColorBrush(D2D1::ColorF(0x34D399, 1.0f), &feedbackBrush);
            if (feedbackBrush) {
                std::wstring feedback = L"✓ Copied!";
                dc_render_target_->DrawText(feedback.c_str(), static_cast<UINT32>(feedback.size()), button_format_, copy_btn_rect_, feedbackBrush);
                feedbackBrush->Release();
            }
        }
    } else {
        dc_render_target_->CreateSolidColorBrush(
            (hovered_btn_ == 1) ? D2D1::ColorF(0x334155, 1.0f) : D2D1::ColorF(0x1E293B, 0.9f),
            &btnBgBrush
        );
        if (btnBgBrush) {
            dc_render_target_->FillRoundedRectangle(copyBtnRect, btnBgBrush);
            dc_render_target_->DrawRoundedRectangle(copyBtnRect, borderBrush, 1.0f);
            btnBgBrush->Release();
            btnBgBrush = nullptr;
        }
        if (button_format_ && textBrush) {
            std::wstring label = L"📋 Copy";
            dc_render_target_->DrawText(label.c_str(), static_cast<UINT32>(label.size()), button_format_, copy_btn_rect_, textBrush);
        }
    }

    // [🔊 TTS] button
    tts_btn_rect_ = D2D1::RectF(110.0f, footer_div_y + 7.0f, 192.0f, footer_div_y + 31.0f);
    D2D1_ROUNDED_RECT ttsBtnRect = D2D1::RoundedRect(tts_btn_rect_, 4.0f, 4.0f);

    dc_render_target_->CreateSolidColorBrush(
        (hovered_btn_ == 2) ? D2D1::ColorF(0x334155, 1.0f) : D2D1::ColorF(0x1E293B, 0.9f),
        &btnBgBrush
    );
    if (btnBgBrush) {
        dc_render_target_->FillRoundedRectangle(ttsBtnRect, btnBgBrush);
        dc_render_target_->DrawRoundedRectangle(ttsBtnRect, borderBrush, 1.0f);
        btnBgBrush->Release();
        btnBgBrush = nullptr;
    }
    if (button_format_ && textBrush) {
        std::wstring label = L"🔊 TTS";
        dc_render_target_->DrawText(label.c_str(), static_cast<UINT32>(label.size()), button_format_, tts_btn_rect_, textBrush);
    }

    if (accentBrush) accentBrush->Release();
    if (dividerBrush) dividerBrush->Release();
    if (subTextBrush) subTextBrush->Release();
    if (textBrush) textBrush->Release();
    if (borderBrush) borderBrush->Release();
    if (bgBrush) bgBrush->Release();

    dc_render_target_->EndDraw();
}

void TooltipWindow::UpdateLayered() {
    if (!hwnd_ || !hMemDC_) return;

    POINT ptSrc = { 0, 0 };
    SIZE sz = { current_width_, current_height_ };
    POINT ptDst = {};
    RECT rcWindow = {};
    ::GetWindowRect(hwnd_, &rcWindow);
    ptDst.x = rcWindow.left;
    ptDst.y = rcWindow.top;

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 245; // ~96% opacity
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
                pThis->ShowTranslation(p->x, p->y, p->source_text,
                                       p->source_lang_code, p->target_lang, p->translated_text);
            }
            return 0;
        }

        case kShowMessageMessage: {
            const std::unique_ptr<MessagePayload> p(
                reinterpret_cast<MessagePayload*>(lParam));
            if (p) {
                pThis->ShowMessage(p->x, p->y, p->header, p->body);
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
            float x = static_cast<float>(pt.x);
            float y = static_cast<float>(pt.y);

            if (pThis->is_message_mode_) {
                ::SetCursor(::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
                return TRUE;
            }

            if (IsPointInRect(pThis->copy_btn_rect_, x, y) ||
                IsPointInRect(pThis->tts_btn_rect_, x, y) ||
                IsPointInRect(pThis->lang_btn_rect_, x, y) ||
                IsPointInRect(pThis->close_btn_rect_, x, y)) {
                ::SetCursor(::LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
                return TRUE;
            }
            ::SetCursor(::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
            return TRUE;
        }

        case WM_MOUSEMOVE: {
            float x = static_cast<float>((short)LOWORD(lParam));
            float y = static_cast<float>((short)HIWORD(lParam));

            int new_hover = 0;
            if (IsPointInRect(pThis->copy_btn_rect_, x, y)) new_hover = 1;
            else if (IsPointInRect(pThis->tts_btn_rect_, x, y)) new_hover = 2;
            else if (IsPointInRect(pThis->lang_btn_rect_, x, y)) new_hover = 3;
            else if (IsPointInRect(pThis->close_btn_rect_, x, y)) new_hover = 4;

            if (new_hover != pThis->hovered_btn_) {
                pThis->hovered_btn_ = new_hover;
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
            if (pThis->hovered_btn_ != 0) {
                pThis->hovered_btn_ = 0;
                pThis->Render();
                pThis->UpdateLayered();
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            // Message-mode notice has no action buttons: any click dismisses.
            if (pThis->is_message_mode_) {
                pThis->Dismiss();
                return 0;
            }

            float x = static_cast<float>((short)LOWORD(lParam));
            float y = static_cast<float>((short)HIWORD(lParam));

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

                POINT pt = { static_cast<int>(pThis->lang_btn_rect_.left), static_cast<int>(pThis->lang_btn_rect_.bottom) };
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
