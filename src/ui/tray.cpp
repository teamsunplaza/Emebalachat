#include "tray.hpp"
#include "../diag_logger.hpp"
#include "asset_loader.hpp"
#include "../config.hpp"
#include "../i18n.hpp"
#include "../unicode_utils.hpp"

#include <cmath>
#include <vector>
#include <wincodec.h>

namespace emebalachat {

namespace {

enum TrayMenuId : UINT {
    ID_TRAY_STATUS = 2001,
    ID_TRAY_ENGINE_GOOGLE = 2010,
    ID_TRAY_ENGINE_LOCAL = 2011,
    ID_TRAY_SWAP = 2020,
    ID_TRAY_AUTOSEND = 2030,
    ID_TRAY_SOUND = 2040,
    ID_TRAY_BADGE = 2050,
    ID_TRAY_START_WITH_WINDOWS = 2055,
    ID_TRAY_CHEATSHEET = 2060,
    ID_TRAY_ABOUT = 2065,
    ID_TRAY_EXIT = 2070,
    ID_TRAY_SRC_BASE = 2100, // 2100..2137
    ID_TRAY_TGT_BASE = 2200, // 2200..2236
    // R6 Phase 6 (plan §5.4): UI-language selector submenu (Auto + 7 locales)
    ID_TRAY_UILANG_AUTO = 2300,
    ID_TRAY_UILANG_BASE = 2301 // 2301..2307
};

} // namespace

SystemTray::SystemTray() = default;

SystemTray::~SystemTray() {
    Destroy();
}

HICON SystemTray::CreateStatusIcon(bool active) {
    constexpr int size = 32;
    HDC hScreenDC = ::GetDC(nullptr);
    HDC hMemDC = ::CreateCompatibleDC(hScreenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    uint32_t* pixels = nullptr;
    HBITMAP hColorBmp = ::CreateDIBSection(hMemDC, &bmi, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
    HBITMAP hOldBmp = static_cast<HBITMAP>(::SelectObject(hMemDC, hColorBmp));

    // Clear transparent
    memset(pixels, 0, size * size * sizeof(uint32_t));

    bool loaded_icon = false;
    std::wstring iconPath = FindAppIconPath();
    if (!iconPath.empty()) {
        IWICImagingFactory* pFactory = nullptr;
        if (SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory))) && pFactory) {
            IWICBitmapDecoder* pDecoder = nullptr;
            if (SUCCEEDED(pFactory->CreateDecoderFromFilename(iconPath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &pDecoder)) && pDecoder) {
                IWICBitmapFrameDecode* pFrame = nullptr;
                if (SUCCEEDED(pDecoder->GetFrame(0, &pFrame)) && pFrame) {
                    IWICBitmapScaler* pScaler = nullptr;
                    if (SUCCEEDED(pFactory->CreateBitmapScaler(&pScaler)) && pScaler) {
                        if (SUCCEEDED(pScaler->Initialize(pFrame, size, size, WICBitmapInterpolationModeHighQualityCubic))) {
                            IWICFormatConverter* pConverter = nullptr;
                            if (SUCCEEDED(pFactory->CreateFormatConverter(&pConverter)) && pConverter) {
                                if (SUCCEEDED(pConverter->Initialize(pScaler, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeMedianCut))) {
                                    const UINT stride = size * sizeof(uint32_t);
                                    const UINT bufferSize = stride * size;
                                    if (SUCCEEDED(pConverter->CopyPixels(nullptr, stride, bufferSize, reinterpret_cast<BYTE*>(pixels)))) {
                                        loaded_icon = true;
                                    }
                                }
                                pConverter->Release();
                            }
                        }
                        pScaler->Release();
                    }
                    pFrame->Release();
                }
                pDecoder->Release();
            }
            pFactory->Release();
        }
    }

    if (loaded_icon) {
        // If inactive, desaturate / dim icon slightly so paused state is immediately clear
        if (!active) {
            for (int i = 0; i < size * size; ++i) {
                uint32_t px = pixels[i];
                uint32_t a = (px >> 24) & 0xFF;
                if (a == 0) continue;
                uint32_t r = (px >> 16) & 0xFF;
                uint32_t g = (px >> 8) & 0xFF;
                uint32_t b = px & 0xFF;
                uint32_t gray = (r * 299 + g * 587 + b * 114) / 1000;
                uint32_t out_r = (r + gray * 2) / 3;
                uint32_t out_g = (g + gray * 2) / 3;
                uint32_t out_b = (b + gray * 2) / 3;
                uint32_t out_a = (a * 190) / 255;
                pixels[i] = (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
            }
        }

        // Status badge overlay at bottom-right corner
        const float dot_cx = 24.5f;
        const float dot_cy = 24.5f;
        const float outer_r = 5.5f;
        const float inner_r = 4.0f;
        const uint32_t status_color = active ? 0xFF10B981 : 0xFF64748B; // Active Emerald vs Inactive Slate Gray
        const uint32_t border_color = 0xFF0F172A; // Dark contrast rim

        for (int y = 18; y < size; ++y) {
            for (int x = 18; x < size; ++x) {
                float dx = x - dot_cx;
                float dy = y - dot_cy;
                float d = sqrtf(dx * dx + dy * dy);
                if (d <= inner_r) {
                    pixels[y * size + x] = status_color;
                } else if (d <= outer_r) {
                    pixels[y * size + x] = border_color;
                }
            }
        }
    } else {
        // Fallback: geometric status ring if app icon asset cannot be found
        uint32_t ringColor = active ? 0xFF10B981 : 0xFF6B7280;
        uint32_t innerColor = 0xFF1E2025;

        float cx = (size - 1) / 2.0f;
        float cy = (size - 1) / 2.0f;
        float outerR = 13.0f;
        float innerR = 8.5f;

        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                float dx = x - cx;
                float dy = y - cy;
                float dist = sqrtf(dx * dx + dy * dy);

                if (dist <= outerR && dist >= innerR) {
                    pixels[y * size + x] = ringColor;
                } else if (dist < innerR) {
                    pixels[y * size + x] = innerColor;
                }
            }
        }
    }

    ::SelectObject(hMemDC, hOldBmp);
    ::DeleteDC(hMemDC);
    ::ReleaseDC(nullptr, hScreenDC);

    // Create 1-bit mask bitmap for icon
    HBITMAP hMaskBmp = ::CreateBitmap(size, size, 1, 1, nullptr);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = hMaskBmp;
    ii.hbmColor = hColorBmp;

    HICON hIcon = ::CreateIconIndirect(&ii);

    ::DeleteObject(hColorBmp);
    ::DeleteObject(hMaskBmp);

    return hIcon;
}

bool SystemTray::Create(HWND hOwner, HINSTANCE hInstance, const Callbacks& callbacks) {
    hOwner_ = hOwner;
    hInstance_ = hInstance;
    callbacks_ = callbacks;

    hCurrentIcon_ = CreateStatusIcon(is_active_);

    memset(&nid_, 0, sizeof(NOTIFYICONDATAW));
    nid_.cbSize = sizeof(NOTIFYICONDATAW);
    nid_.hWnd = hOwner_;
    nid_.uID = 1;
    nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid_.uCallbackMessage = WM_TRAYICON;
    nid_.hIcon = hCurrentIcon_;

    // R6 Phase 5 sweep (plan §5.3): the initial tip was a hardcoded English
    // literal ("Emebala Chat (Active) - KO -> EN"). Build it from the same
    // localized pieces UpdateStatus uses, against the initial state members,
    // so the very first hover already honors the active UI locale. (Create()
    // runs after I18n::Initialize in wWinMain.)
    std::wstring tip = I18n::Get(StringId::TooltipTitle) + L" (" + I18n::Get(StringId::BadgeActive) + L") - "
        + I18n::GetLanguageDisplayName(src_code_) + L" -> " + I18n::GetLanguageDisplayName(tgt_code_);
    wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);

    return ::Shell_NotifyIconW(NIM_ADD, &nid_) == TRUE;
}

// R6 Phase 6: mirror of the persisted config.ui_language for the submenu
// check mark. The context menu itself rebuilds on every ShowContextMenu, so
// no invalidation is required here.
void SystemTray::SetUiLanguage(std::string_view persisted_code) {
    ui_language_ = persisted_code;
}

void SystemTray::Destroy() {
    if (nid_.hWnd) {
        ::Shell_NotifyIconW(NIM_DELETE, &nid_);
        nid_.hWnd = nullptr;
    }
    if (hCurrentIcon_) {
        ::DestroyIcon(hCurrentIcon_);
        hCurrentIcon_ = nullptr;
    }
}

void SystemTray::UpdateStatus(
    bool active,
    std::string_view active_engine,
    std::string_view src_code,
    std::string_view tgt_code,
    bool auto_send,
    bool sound_enabled,
    bool badge_visible
) {
    bool iconChanged = (is_active_ != active);
    is_active_ = active;
    active_engine_ = active_engine;
    src_code_ = src_code;
    tgt_code_ = tgt_code;
    auto_send_ = auto_send;
    sound_enabled_ = sound_enabled;
    badge_visible_ = badge_visible;

    if (iconChanged) {
        if (hCurrentIcon_) {
            ::DestroyIcon(hCurrentIcon_);
        }
        hCurrentIcon_ = CreateStatusIcon(is_active_);
        nid_.hIcon = hCurrentIcon_;
    }

    std::wstring tip = I18n::Get(StringId::TooltipTitle) + L" (" + (is_active_ ? I18n::Get(StringId::BadgeActive) : I18n::Get(StringId::BadgePaused)) + L") - "
        + I18n::GetLanguageDisplayName(src_code_) + L" -> " + I18n::GetLanguageDisplayName(tgt_code_);
    wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);

    ::Shell_NotifyIconW(NIM_MODIFY, &nid_);
    // 260905 diagnostics: every tray status refresh (language changes old→new
    // are visible through successive lines of this tag).
    DIAG_LOG("UI", "tray_update active=%d engine=%s src=%s tgt=%s auto_send=%d icon_changed=%d",
             active ? 1 : 0, std::string(active_engine).c_str(),
             std::string(src_code).c_str(), std::string(tgt_code).c_str(),
             auto_send ? 1 : 0, iconChanged ? 1 : 0);
}

void SystemTray::ShowContextMenu() {
    HMENU hMenu = ::CreatePopupMenu();
    if (!hMenu) return;

    // 1. Status toggle item
    std::wstring statusLabel = is_active_ ? I18n::Get(StringId::MenuStatusActive) : I18n::Get(StringId::MenuStatusPaused);
    ::AppendMenuW(hMenu, MF_STRING | (is_active_ ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_STATUS, statusLabel.c_str());
    ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // 2. Engine submenu
    HMENU hEngineMenu = ::CreatePopupMenu();
    bool isGoogle = (active_engine_.find("Google") != std::string::npos);
    ::AppendMenuW(hEngineMenu, MF_STRING | (isGoogle ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_ENGINE_GOOGLE, I18n::Get(StringId::MenuEngineGoogle).c_str());
    ::AppendMenuW(hEngineMenu, MF_STRING | (!isGoogle ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_ENGINE_LOCAL, I18n::Get(StringId::MenuEngineLocal).c_str());
    ::AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hEngineMenu), I18n::Get(StringId::MenuEngine).c_str());

    // 2b. R6 Phase 6 (plan §5.4): UI-language selector submenu, next to the
    // Engine submenu. Entries show ENDONYMS (each language in its own script)
    // plus a localized Auto entry. Check mark tracks the PERSISTED config
    // value (ui_language_), not the resolved locale, so "auto" stays visibly
    // selected while it follows the OS language.
    HMENU hUiLangMenu = ::CreatePopupMenu();
    {
        const bool autoChecked = (ui_language_ == "auto" || ui_language_.empty());
        ::AppendMenuW(hUiLangMenu, MF_STRING | (autoChecked ? MF_CHECKED : MF_UNCHECKED),
                      ID_TRAY_UILANG_AUTO, I18n::Get(StringId::MenuUiLanguageAuto).c_str());
        ::AppendMenuW(hUiLangMenu, MF_SEPARATOR, 0, nullptr);
        const auto& uiLocales = GetSupportedUiLocales();
        for (size_t i = 0; i < uiLocales.size(); ++i) {
            const std::string code = I18n::LocaleToString(uiLocales[i].locale);
            const bool checked = (!autoChecked && ui_language_ == code);
            ::AppendMenuW(hUiLangMenu, MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED),
                          ID_TRAY_UILANG_BASE + static_cast<UINT>(i), uiLocales[i].native_name);
        }
    }
    ::AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hUiLangMenu), I18n::Get(StringId::MenuUiLanguage).c_str());

    // 3. Source language submenu (AUTO + 37 languages)
    HMENU hSrcMenu = ::CreatePopupMenu();
    const auto& allLangs = GetSupportedLanguages();
    std::string normSrc = NormalizeLanguageCode(src_code_);
    for (size_t i = 0; i < allLangs.size(); ++i) {
        std::wstring itemText = ToUtf16(allLangs[i].code) + L" - " + I18n::GetLanguageDisplayName(allLangs[i].code);
        bool isCurrent = (normSrc == allLangs[i].code);
        ::AppendMenuW(hSrcMenu, MF_STRING | (isCurrent ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_SRC_BASE + static_cast<UINT>(i), itemText.c_str());
    }
    ::AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hSrcMenu), I18n::Get(StringId::MenuSourceLang).c_str());

    // 4. Target language submenu (37 languages)
    HMENU hTgtMenu = ::CreatePopupMenu();
    const auto& tgtLangs = GetTargetLanguages();
    std::string normTgt = NormalizeLanguageCode(tgt_code_);
    for (size_t i = 0; i < tgtLangs.size(); ++i) {
        std::wstring itemText = ToUtf16(tgtLangs[i].code) + L" - " + I18n::GetLanguageDisplayName(tgtLangs[i].code);
        bool isCurrent = (normTgt == tgtLangs[i].code);
        ::AppendMenuW(hTgtMenu, MF_STRING | (isCurrent ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_TGT_BASE + static_cast<UINT>(i), itemText.c_str());
    }
    ::AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hTgtMenu), I18n::Get(StringId::MenuTargetLang).c_str());

    // 5. Swap languages
    ::AppendMenuW(hMenu, MF_STRING, ID_TRAY_SWAP, I18n::Get(StringId::MenuSwapLangs).c_str());
    ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // 6. Modes and toggles
    ::AppendMenuW(hMenu, MF_STRING | (auto_send_ ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_AUTOSEND, I18n::Get(StringId::MenuAutoSend).c_str());
    ::AppendMenuW(hMenu, MF_STRING | (sound_enabled_ ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_SOUND, I18n::Get(StringId::MenuSoundFeedback).c_str());
    ::AppendMenuW(hMenu, MF_STRING | (badge_visible_ ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_BADGE, I18n::Get(StringId::MenuShowBadge).c_str());

    // 7. Start with Windows
    bool startWithWin = I18n::IsStartWithWindowsEnabled();
    ::AppendMenuW(hMenu, MF_STRING | (startWithWin ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_START_WITH_WINDOWS, I18n::Get(StringId::MenuStartWithWindows).c_str());
    ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // 8. About, Cheat Sheet and Exit
    ::AppendMenuW(hMenu, MF_STRING, ID_TRAY_ABOUT, I18n::Get(StringId::MenuAbout).c_str());
    ::AppendMenuW(hMenu, MF_STRING, ID_TRAY_CHEATSHEET, I18n::Get(StringId::MenuCheatSheet).c_str());
    ::AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, I18n::Get(StringId::MenuExit).c_str());

    POINT pt = {};
    ::GetCursorPos(&pt);
    ::SetForegroundWindow(hOwner_);

    UINT cmd = ::TrackPopupMenuEx(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, hOwner_, nullptr);
    ::PostMessageW(hOwner_, WM_NULL, 0, 0);

    // Dispatch selection
    if (cmd == ID_TRAY_STATUS && callbacks_.on_toggle_active) {
        callbacks_.on_toggle_active();
    } else if (cmd == ID_TRAY_ENGINE_GOOGLE && callbacks_.on_select_engine) {
        callbacks_.on_select_engine(0);
    } else if (cmd == ID_TRAY_ENGINE_LOCAL && callbacks_.on_select_engine) {
        callbacks_.on_select_engine(1);
    } else if (cmd == ID_TRAY_SWAP && callbacks_.on_swap_languages) {
        callbacks_.on_swap_languages();
    } else if (cmd == ID_TRAY_AUTOSEND && callbacks_.on_toggle_auto_send) {
        callbacks_.on_toggle_auto_send();
    } else if (cmd == ID_TRAY_SOUND && callbacks_.on_toggle_sound) {
        callbacks_.on_toggle_sound();
    } else if (cmd == ID_TRAY_BADGE && callbacks_.on_toggle_badge) {
        callbacks_.on_toggle_badge();
    } else if (cmd == ID_TRAY_START_WITH_WINDOWS) {
        I18n::SetStartWithWindows(!startWithWin);
        if (callbacks_.on_toggle_start_with_windows) {
            callbacks_.on_toggle_start_with_windows();
        }
    } else if (cmd == ID_TRAY_CHEATSHEET && callbacks_.on_show_cheat_sheet) {
        callbacks_.on_show_cheat_sheet();
    } else if (cmd == ID_TRAY_ABOUT && callbacks_.on_show_about) {
        callbacks_.on_show_about();
    } else if (cmd == ID_TRAY_EXIT && callbacks_.on_exit) {
        callbacks_.on_exit();
    } else if (cmd >= ID_TRAY_SRC_BASE && cmd < ID_TRAY_SRC_BASE + allLangs.size()) {
        size_t idx = cmd - ID_TRAY_SRC_BASE;
        if (callbacks_.on_select_source_lang) {
            callbacks_.on_select_source_lang(allLangs[idx].name_en);
        }
    } else if (cmd >= ID_TRAY_TGT_BASE && cmd < ID_TRAY_TGT_BASE + tgtLangs.size()) {
        size_t idx = cmd - ID_TRAY_TGT_BASE;
        if (callbacks_.on_select_target_lang) {
            callbacks_.on_select_target_lang(tgtLangs[idx].name_en);
        }
    } else if (cmd == ID_TRAY_UILANG_AUTO && callbacks_.on_select_ui_language) {
        callbacks_.on_select_ui_language("auto");
    } else if (cmd >= ID_TRAY_UILANG_BASE &&
               cmd < ID_TRAY_UILANG_BASE + static_cast<UINT>(GetSupportedUiLocales().size()) &&
               callbacks_.on_select_ui_language) {
        size_t idx = cmd - ID_TRAY_UILANG_BASE;
        callbacks_.on_select_ui_language(I18n::LocaleToString(GetSupportedUiLocales()[idx].locale));
    }

    ::DestroyMenu(hMenu);
}

} // namespace emebalachat
