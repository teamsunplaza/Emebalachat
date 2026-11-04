#include "tray.hpp"
#include "ui/theme.hpp"
#include "../diag_logger.hpp"
#include "asset_loader.hpp"
#include "../config.hpp"
#include "../i18n.hpp"
#include "../unicode_utils.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace emebalachat {

namespace {

enum TrayMenuId : UINT {
    ID_TRAY_STATUS = 2001,
    ID_TRAY_ENGINE_GOOGLE = 2010,
    ID_TRAY_ENGINE_LOCAL = 2011,
    // REQ-045 P4-3 (design §3b): OpenAI Compatible engine. Uses the next
    // free slot in the engine band (2010/2011/2012) — no collision with the
    // 2020+ control band.
    ID_TRAY_ENGINE_OPENAI = 2012,
    // REQ-045 P4-5 (item 3a-2) + REQ-046 P4-2 (Rev2 §B-3): index 3 of the
    // engine submenu — flat checkable "사용자 선택(.gguf)" entry (the nested
    // POPUP is gone; ID unchanged). Sits in the 2010-band (2013), far below
    // the 2020 control band and the 2400 DRAG band — no collision.
    ID_TRAY_ENGINE_USER_GGUF = 2013,
    // REQ-045 P4-5 / REQ-046 P4-2: the always-active file-picker row that
    // used to follow the checkable user-model entry. REQ-050: that row is
    // MERGED into the single manager row below (2015) — the constant stays
    // defined (and never renumbered) so the frozen engine ID band 2010~2014
    // remains intact; nothing appends it to a menu anymore.
    ID_TRAY_BROWSE_GGUF = 2014,
    // REQ-048 R2-D + REQ-050: the single user-model manager row ("모델 관리…")
    // — the ONE entry point for everything user-model: add-from-file,
    // add-from-Hugging-Face, rename, delete. PLAIN item — deliberately NOT
    // part of the frozen 4-way radio contract (2010~2014) and below the 2020
    // control band, so the radio IDs and the band layout are untouched.
    // Clicking it defers to the GUI thread via WM_APP+0x501 (same modality
    // contract as the OpenAI settings' WM_APP+0x500, REQ-047 D3) instead of a
    // tray callback.
    ID_TRAY_MANAGE_GGUF = 2015,
    // REQ-060 (session 261009_0001, engine-idle-eviction-warmup): the engine
    // submenu's "번역 모델 로드 설정…" entry row, appended BELOW the manager
    // row (2015) per mockup-tray-menu.png. PLAIN item (opens the load-mode
    // dialog; no check geometry) — the frozen 4-way radio band 2010~2014 is
    // untouched and 2016 stays clear of the 2020 control band. Dispatch routes
    // through on_open_engine_load_settings; main.cpp owns the deferred modal
    // open (the REQ-047 D3 contract, kMsgOpenEngineLoadSettings = WM_APP+0x502).
    ID_TRAY_ENGINE_LOAD_SETTINGS = 2016,
    ID_TRAY_SWAP = 2020,
    ID_TRAY_AUTOSEND = 2030,
    ID_TRAY_SOUND = 2040,
    ID_TRAY_BADGE = 2050,
    ID_TRAY_START_WITH_WINDOWS = 2055,
    ID_TRAY_CHEATSHEET = 2060,
    ID_TRAY_ABOUT = 2065,
    ID_TRAY_EXIT = 2070,
    ID_TRAY_SRC_BASE = 2100, // 2100..2137  (REQ-025: TYPE pair - unchanged)
    ID_TRAY_TGT_BASE = 2200, // 2200..2236  (REQ-025: TYPE pair - unchanged)
    // R6 Phase 6 (plan §5.4), expanded by REQ-037 (P4 B-4, design §2-Q3):
    // UI-language selector submenu = Auto (2300) + the 37 GetSupportedUiLocales
    // entries (2301..2337, endonym-only labels, single column + OS auto-scroll
    // per the §2-Q3 verdict A). Both the build loop and the dispatch below are
    // size-dynamic, so only the band capacity is a code property: the next
    // band (DRAG_SRC) starts at 2400 -> highest current ID 2337, 62 IDs of
    // headroom, no collision (design §2-Q3 verified).
    ID_TRAY_UILANG_AUTO = 2300,
    ID_TRAY_UILANG_BASE = 2301, // 2301..2337 (Auto + 37 locales = 38 items)
    // UI Theme selector submenu (Fluent / Editorial / Monolith = 2381..2383).
    ID_TRAY_THEME_FLUENT = SystemTray::kCmdThemeFluent,
    ID_TRAY_THEME_EDITORIAL = SystemTray::kCmdThemeEditorial,
    ID_TRAY_THEME_MONOLITH = SystemTray::kCmdThemeMonolith,
    // REQ-025 (Phase A §2.1.A3-25): DRAG ("번역툴팁") pair submenu IDs. The
    // existing 2100/2200 bands stay TYPE-only (regression guard); the drag
    // pair gets fresh non-overlapping bands. 2400..2437 / 2500..2536.
    ID_TRAY_DRAG_SRC_BASE = 2400,
    ID_TRAY_DRAG_TGT_BASE = 2500,
    // W2 (R1-22, session 261029_0001): the 번역툴팁 group's Swap row — a plain
    // item BELOW the two drag pickers, in a fresh ≥2600 band (the 2500 band
    // ends at 2536, so 2600 keeps a 63-ID gap for future target languages).
    // Label reuses MenuSwapLangs (zero new i18n keys); the dispatch mirrors the
    // type-swap AUTO-pivot policy through PlanLanguageSwap and applies it to
    // the DRAG pair (ApplyLanguageChange(LanguageContext::Drag, ...)).
    ID_TRAY_DRAG_SWAP = 2600
};

// REQ-037/B-4 compile-time guard for the band layout asserted above: the
// 38-item UILANG submenu (Auto + 37 selector codes => 2300..2337) must stay
// strictly below the DRAG band floor. If the selector ever grows past 99
// locales, widen the bands here FIRST - the AppendMenu loop would otherwise
// hand submenu IDs straight into DRAG_SRC territory.
static_assert(ID_TRAY_UILANG_BASE + 37 <= ID_TRAY_DRAG_SRC_BASE,
              "tray UI-language ID band must not collide with the drag band");

// W2 (R1-22): the same guard for the two new neighbours — the drag TARGET
// picker must stay strictly below the DRAG_SWAP row, and that row must sit in
// the ≥2600 band the W2 research assigned it. Widen the band here FIRST if a
// target language is ever added past the 63-ID headroom.
static_assert(ID_TRAY_DRAG_TGT_BASE + 37 <= ID_TRAY_DRAG_SWAP,
              "tray drag-target ID band must not collide with the drag-swap row");
static_assert(ID_TRAY_DRAG_SWAP >= 2600,
              "the drag-pair Swap row belongs to the >=2600 band (W2/R1-22)");

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

    // W6/C3 (session 260910_0007): decode through asset_loader's single WIC
    // pixel pipeline. The old hand-rolled decoder here grabbed GetFrame(0) of
    // Emebala_Chat_Appicon.ico - the 16x16 LOWEST frame of the 7-frame DP-1
    // container (16/24/32/48/64/128/256, verified against the shipped asset) -
    // and upscaled it 2x to 32x32, so the tray rendered a blurry blown-up icon.
    // LoadWicIconPixels applies the same largest-frame selection the D2D
    // LoadWicBitmap path already used, then HighQualityCubic-scales the largest
    // frame DOWN to the 32x32 premultiplied-BGRA build buffer below. The
    // state-dot overlay and inactive-dim loops are untouched: they operate on
    // the 32x32 buffer, whose coordinate space is unchanged (the dot still
    // addresses the bottom-right corner in 32px space; Shell_NotifyIcon's
    // SM_CXSMICON downscale semantics are unchanged since the HICON stays
    // 32x32).
    bool loaded_icon = false;
    std::wstring iconPath = FindAppIconPath();
    if (!iconPath.empty()) {
        std::vector<uint32_t> basePixels;
        if (SUCCEEDED(LoadWicIconPixels(iconPath, static_cast<UINT>(size), basePixels)) &&
            basePixels.size() == static_cast<size_t>(size) * size) {
            memcpy(pixels, basePixels.data(), basePixels.size() * sizeof(uint32_t));
            loaded_icon = true;
        }
    }

    const auto& theme = ui::GetActiveTheme();
    auto smoothstep_coverage = [](float edge, float dist, float feather) noexcept -> float {
        const float half_f = feather * 0.5f;
        if (dist <= edge - half_f) return 1.0f;
        if (dist >= edge + half_f) return 0.0f;
        const float t = (dist - (edge - half_f)) / feather;
        return 1.0f - (t * t * (3.0f - 2.0f * t));
    };

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

        // Status badge overlay at bottom-right corner with sub-pixel smoothstep coverage
        // (1.2f feathering ramp at both inner_r and outer_r) and active theme colors.
        const float dot_cx = 24.5f;
        const float dot_cy = 24.5f;
        const float outer_r = 5.5f;
        const float inner_r = 4.0f;
        const float feather = 1.2f;
        const uint32_t status_rgb = active ? theme.state_ready : theme.state_off;
        (void)theme.state_busy;
        const uint32_t border_rgb = theme.surface_canvas;

        const float sr = static_cast<float>((status_rgb >> 16) & 0xFF);
        const float sg = static_cast<float>((status_rgb >> 8) & 0xFF);
        const float sb = static_cast<float>(status_rgb & 0xFF);
        const float br = static_cast<float>((border_rgb >> 16) & 0xFF);
        const float bg = static_cast<float>((border_rgb >> 8) & 0xFF);
        const float bb = static_cast<float>(border_rgb & 0xFF);

        for (int y = 18; y < size; ++y) {
            for (int x = 18; x < size; ++x) {
                const float dx = static_cast<float>(x) - dot_cx;
                const float dy = static_cast<float>(y) - dot_cy;
                float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));

                const float outer_cov = smoothstep_coverage(outer_r, dist, feather);
                if (outer_cov <= 0.0f) {
                    continue;
                }
                const float inner_cov = smoothstep_coverage(inner_r, dist, feather);

                // Mix inner status fill with outer border rim using inner_cov
                const float fg_r = sr * inner_cov + br * (1.0f - inner_cov);
                const float fg_g = sg * inner_cov + bg * (1.0f - inner_cov);
                const float fg_b = sb * inner_cov + bb * (1.0f - inner_cov);

                // Alpha-composite over existing premultiplied-BGRA icon pixel
                const uint32_t dst = pixels[y * size + x];
                const float dst_a = static_cast<float>((dst >> 24) & 0xFF) / 255.0f;
                const float dst_r = static_cast<float>((dst >> 16) & 0xFF);
                const float dst_g = static_cast<float>((dst >> 8) & 0xFF);
                const float dst_b = static_cast<float>(dst & 0xFF);

                const float inv_a = 1.0f - outer_cov;
                const float out_a = outer_cov + dst_a * inv_a;
                const uint32_t out_a8 = static_cast<uint32_t>(std::clamp(out_a * 255.0f + 0.5f, 0.0f, 255.0f));
                const uint32_t out_r8 = static_cast<uint32_t>(std::clamp(fg_r * outer_cov + dst_r * inv_a + 0.5f, 0.0f, 255.0f));
                const uint32_t out_g8 = static_cast<uint32_t>(std::clamp(fg_g * outer_cov + dst_g * inv_a + 0.5f, 0.0f, 255.0f));
                const uint32_t out_b8 = static_cast<uint32_t>(std::clamp(fg_b * outer_cov + dst_b * inv_a + 0.5f, 0.0f, 255.0f));

                pixels[y * size + x] = (out_a8 << 24) | (out_r8 << 16) | (out_g8 << 8) | out_b8;
            }
        }
    } else {
        // Fallback: geometric status ring if app icon asset cannot be found
        const uint32_t ring_rgb = active ? theme.state_ready : theme.state_off;
        const uint32_t inner_rgb = theme.surface_canvas;

        const float rr = static_cast<float>((ring_rgb >> 16) & 0xFF);
        const float rg = static_cast<float>((ring_rgb >> 8) & 0xFF);
        const float rb = static_cast<float>(ring_rgb & 0xFF);
        const float ir = static_cast<float>((inner_rgb >> 16) & 0xFF);
        const float ig = static_cast<float>((inner_rgb >> 8) & 0xFF);
        const float ib = static_cast<float>(inner_rgb & 0xFF);

        const float cx = (size - 1) / 2.0f;
        const float cy = (size - 1) / 2.0f;
        const float outerR = 13.0f;
        const float innerR = 8.5f;
        const float feather = 1.2f;

        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const float dx = static_cast<float>(x) - cx;
                const float dy = static_cast<float>(y) - cy;
                float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));

                const float outer_cov = smoothstep_coverage(outerR, dist, feather);
                if (outer_cov <= 0.0f) {
                    continue;
                }
                const float inner_cov = smoothstep_coverage(innerR, dist, feather);
                const float fg_r = ir * inner_cov + rr * (1.0f - inner_cov);
                const float fg_g = ig * inner_cov + rg * (1.0f - inner_cov);
                const float fg_b = ib * inner_cov + rb * (1.0f - inner_cov);

                const uint32_t out_a8 = static_cast<uint32_t>(std::clamp(outer_cov * 255.0f + 0.5f, 0.0f, 255.0f));
                const uint32_t out_r8 = static_cast<uint32_t>(std::clamp(fg_r * outer_cov + 0.5f, 0.0f, 255.0f));
                const uint32_t out_g8 = static_cast<uint32_t>(std::clamp(fg_g * outer_cov + 0.5f, 0.0f, 255.0f));
                const uint32_t out_b8 = static_cast<uint32_t>(std::clamp(fg_b * outer_cov + 0.5f, 0.0f, 255.0f));
                pixels[y * size + x] = (out_a8 << 24) | (out_r8 << 16) | (out_g8 << 8) | out_b8;
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

// ---- W1 (R1-22, session 261029_0001): pure tip / status-row composition ----
namespace {

// Caps a composed string at max_chars, backing off a dangling high surrogate
// (a cut must never split a surrogate pair) and marking the cut with "…".
// Used only as the last resort: ComposeTrayTip drops whole optional segments
// before it falls back to this.
std::wstring TruncateWithEllipsis(std::wstring text, size_t max_chars) {
    if (text.size() <= max_chars) {
        return text;
    }
    size_t keep = max_chars ? max_chars - 1 : 0; // room for the ellipsis
    if (keep > 0 && text[keep - 1] >= 0xD800 && text[keep - 1] <= 0xDBFF) {
        --keep; // the next code unit was the low half of that pair
    }
    text.resize(keep);
    text += L'\u2026';
    return text;
}

// Narrow ASCII view of the engine stem for the token comparisons below.
std::string AsciiUpper(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

} // namespace

std::string TrayEngineLabel(std::string_view active_engine) {
    std::string stem(active_engine);
    // Strip the descriptive parenthetical ("Hy-MT2-1.8B (Shared Host)" ->
    // "Hy-MT2-1.8B") - the szTip budget cannot carry the full stem.
    const size_t paren = stem.find(" (");
    if (paren != std::string::npos) {
        stem.resize(paren);
    }
    // R2-5 (261029_0001): the hook-thread refreshes push an EMPTY view (the
    // engine segment is authoritative on the GUI thread), so this mapping is
    // NOT a hook-path token translator any more - it serves the legacy/GUI
    // stems that still arrive as a raw token, and passes a display stem
    // through unchanged.
    const std::string token = AsciiUpper(stem);
    if (token == "LOCAL") return "Local";
    if (token == "GOOGLE" || token == "AUTO") return "Google Translate";
    if (token == "OPENAI") return "OpenAI Compatible";
    if (token == "USER_GGUF") return "User model";
    return stem; // already a display stem (or empty: no engine segment)
}

std::wstring ComposeTrayTip(const TrayTipParts& parts) {
    std::wstring core(parts.title);
    if (!parts.state.empty()) {
        core += L" (";
        core += parts.state;
        core += L")";
    }
    if (!parts.engine.empty()) {
        core += L" - ";
        core += parts.engine;
    }
    if (!parts.src_display.empty() || !parts.tgt_display.empty()) {
        core += L" \u00B7 "; // " · "
        core += parts.src_display;
        core += L" \u2192 "; // " → "
        core += parts.tgt_display;
    }
    if (parts.auto_send && !parts.auto_send_label.empty()) {
        std::wstring full = core;
        full += L" \u00B7 ";
        full += parts.auto_send_label;
        if (full.size() <= kTrayTipMaxChars) {
            return full;
        }
        // Overflow: the auto-send tail is the only optional segment, so it is
        // the first thing dropped (documented order, pinned by the tests).
    }
    if (core.size() <= kTrayTipMaxChars) {
        return core;
    }
    DIAG_LOG("UI", "tray_tip_truncated len=%zu cap=%zu",
             core.size(), kTrayTipMaxChars);
    return TruncateWithEllipsis(std::move(core), kTrayTipMaxChars);
}

std::wstring ComposeTrayStatusLabel(std::wstring_view state_label,
                                    std::wstring_view src_display,
                                    std::wstring_view tgt_display) {
    std::wstring out(state_label);
    if (src_display.empty() && tgt_display.empty()) {
        return out;
    }
    out += L" \u00B7 ";
    out += src_display;
    out += L" \u2192 ";
    out += tgt_display;
    return out;
}

// ---- W2 (R1-22): one swap policy for both language pairs ----
LanguageSwapPlan PlanLanguageSwap(std::string_view cur_source,
                                  std::string_view cur_target,
                                  std::string_view system_language_code) {
    const std::string cur_src_norm = NormalizeLanguageCode(cur_source);
    const std::string cur_tgt_norm = NormalizeLanguageCode(cur_target);

    LanguageSwapPlan plan;
    if (cur_src_norm == "AUTO") {
        // Auto Detect -> English becomes English -> <OS native>: the concrete
        // old target moves to the source, and the target becomes the user's OS
        // language (or the other of EN/KO so the swap always reverses).
        const LanguageInfo* const p_tgt = FindLanguageByCode(cur_tgt_norm);
        plan.source = p_tgt ? p_tgt->name_en : std::string(cur_target);

        std::string sys_norm = NormalizeLanguageCode(system_language_code);
        if (sys_norm == "AUTO" || sys_norm.empty()) {
            sys_norm = "KO";
        }
        if (cur_tgt_norm == sys_norm) {
            plan.target = (sys_norm == "EN") ? "Korean" : "English";
        } else {
            const LanguageInfo* const p_sys = FindLanguageByCode(sys_norm);
            plan.target = p_sys ? p_sys->name_en : "Korean";
        }
    } else {
        // Two concrete languages swap directly.
        plan.source = std::string(cur_target);
        plan.target = std::string(cur_source);
    }
    return plan;
}

bool SystemTray::Create(HWND hOwner, HINSTANCE hInstance, const Callbacks& callbacks) {
    hOwner_ = hOwner;
    hInstance_ = hInstance;
    callbacks_ = callbacks;
    if (callbacks_.on_select_theme && !on_theme_select_) {
        on_theme_select_ = callbacks_.on_select_theme;
    }

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
    // W1 (R1-22): same composer, so the first hover already carries the engine
    // + auto-send segments the later UpdateStatus tips carry.
    const std::wstring engine_label = ToUtf16(TrayEngineLabel(active_engine_));
    const std::wstring src_label = I18n::GetLanguageDisplayName(src_code_);
    const std::wstring tgt_label = I18n::GetLanguageDisplayName(tgt_code_);
    const std::wstring auto_send_label = I18n::Get(StringId::MenuAutoSend);
    const std::wstring tip = ComposeTrayTip(TrayTipParts{
        I18n::Get(StringId::TooltipTitle),
        I18n::Get(StringId::BadgeActive),
        engine_label,
        src_label,
        tgt_label,
        auto_send_label,
        auto_send_});
    wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);

    return ::Shell_NotifyIconW(NIM_ADD, &nid_) == TRUE;
}

// R6 Phase 6: mirror of the persisted config.ui_language for the submenu
// check mark. The context menu itself rebuilds on every ShowContextMenu, so
// no invalidation is required here.
void SystemTray::SetUiLanguage(std::string_view persisted_code) {
    ui_language_ = persisted_code;
}

void SystemTray::SetOnThemeSelectCallback(ThemeSelectCallback cb) {
    on_theme_select_ = std::move(cb);
    callbacks_.on_select_theme = on_theme_select_;
}

void SystemTray::SetUiTheme(std::string_view theme_key) {
    ui_theme_ = ui::ThemeIdToKey(ui::ParseThemeId(theme_key));
}

void SystemTray::RefreshTheme() {
    ui_theme_ = ui::ThemeIdToKey(ui::GetActiveThemeId());
    if (hCurrentIcon_) {
        ::DestroyIcon(hCurrentIcon_);
    }
    hCurrentIcon_ = CreateStatusIcon(is_active_);
    nid_.hIcon = hCurrentIcon_;
    if (nid_.hWnd) {
        ::Shell_NotifyIconW(NIM_MODIFY, &nid_);
    }
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

void SystemTray::SetEngineMenuResolver(std::function<EngineMenuView()> resolver) {
    resolver_ = std::move(resolver);
}

void SystemTray::UpdateStatus(
    bool active,
    std::string_view active_engine,
    std::string_view src_code,
    std::string_view tgt_code,
    std::string_view drag_src_code,
    std::string_view drag_tgt_code,
    bool auto_send,
    bool sound_enabled,
    bool badge_visible
) {
    bool iconChanged = (is_active_ != active);
    is_active_ = active;
    // REQ-029-B: display-only capture (tooltip + tray_update log below); the
    // Engine submenu check mark is derived at open (REQ-056), never from
    // this string.
    //
    // R2-5 (FIX-B2): an EMPTY view means "the caller has no resolved display
    // stem" - the hook-thread refreshes (F9 pause/resume, Ctrl+Shift+Enter
    // auto-send, Ctrl+F9 cycle). Those paths only hold the persisted
    // engine_type TOKEN, which cannot answer "which model is serving": under
    // "auto"/"local" the real answer is the shared host's Hy-MT2-1.8B, and
    // under "user_gguf" it is the user's model stem. Pushing the token here
    // overwrote the GUI-resolved stem in the hover tip ("Hy-MT2-1.8B" flipped
    // to "Google Translate", "Local" or "User model" after every F9). The
    // single authority for that string is the GUI thread's refresh_tray()
    // (engine.GetActiveEngineName()), so an empty push preserves it - and
    // leaves the non-atomic std::string untouched from the hook thread.
    if (!active_engine.empty()) {
        active_engine_ = active_engine;
    }
    src_code_ = src_code;
    tgt_code_ = tgt_code;
    // REQ-025: drag pair feeds ONLY the "번역툴팁" submenu check marks; the
    // hover tip below intentionally keeps displaying the type pair.
    drag_src_code_ = drag_src_code;
    drag_tgt_code_ = drag_tgt_code;
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

    // W1 (R1-22): the hover tip now answers "which engine / which pair / is
    // auto-send on?" without opening any submenu. Composition (and the szTip
    // 128-wchar budget) lives in the pure ComposeTrayTip helper so both the
    // drop order and the cap are unit-tested headlessly; the engine segment
    // uses the SHORT token because the localized MenuEngine* rows are full
    // sentences (research §8 residual risk 1).
    const std::wstring state_label = is_active_ ? I18n::Get(StringId::BadgeActive)
                                                : I18n::Get(StringId::BadgePaused);
    const std::wstring engine_label = ToUtf16(TrayEngineLabel(active_engine_));
    const std::wstring src_label = I18n::GetLanguageDisplayName(src_code_);
    const std::wstring tgt_label = I18n::GetLanguageDisplayName(tgt_code_);
    const std::wstring auto_send_label = I18n::Get(StringId::MenuAutoSend);
    const std::wstring tip = ComposeTrayTip(TrayTipParts{
        I18n::Get(StringId::TooltipTitle), state_label, engine_label,
        src_label, tgt_label, auto_send_label, auto_send});
    // Defensive: ComposeTrayTip already caps at kTrayTipMaxChars, _TRUNCATE
    // keeps the copy safe even if that invariant is ever broken.
    wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);

    ::Shell_NotifyIconW(NIM_MODIFY, &nid_);
    // 260905 diagnostics: every tray status refresh (language changes old→new
    // are visible through successive lines of this tag). REQ-025 adds the drag
    // pair so the DP-2 check (drag change must NOT alter src/tgt = type tip)
    // is directly readable from this log line.
    DIAG_LOG("UI", "tray_update active=%d engine=%s src=%s tgt=%s drag_src=%s drag_tgt=%s auto_send=%d icon_changed=%d",
             active ? 1 : 0, active_engine_.c_str(),
             std::string(src_code).c_str(), std::string(tgt_code).c_str(),
             std::string(drag_src_code).c_str(), std::string(drag_tgt_code).c_str(),
             auto_send ? 1 : 0, iconChanged ? 1 : 0);
}

void SystemTray::ShowContextMenu() {
    HMENU hMenu = ::CreatePopupMenu();
    if (!hMenu) return;

    // 1. Status toggle item
    // W1 (R1-22): the top row now carries the TYPE pair as well, so the pair
    // is readable without hovering the icon or walking the 3-level language
    // menus: "상태: 활성 (F9: 일시 정지) · 자동 감지 → 영어" (research shape
    // "상태: … · KO → EN"; display names via GetLanguageDisplayName, zero new
    // i18n keys).
    const std::wstring statusLabel = ComposeTrayStatusLabel(
        is_active_ ? I18n::Get(StringId::MenuStatusActive)
                   : I18n::Get(StringId::MenuStatusPaused),
        I18n::GetLanguageDisplayName(src_code_),
        I18n::GetLanguageDisplayName(tgt_code_));
    ::AppendMenuW(hMenu, MF_STRING | (is_active_ ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_STATUS, statusLabel.c_str());
    ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // 2. Engine submenu
    // REQ-056 (derive-at-open): the submenu's two config-derived values are
    // resolved NOW, at menu build time, from main.cpp (the single authority)
    // — the tray keeps no cached copies, so no push path can go stale (the
    // 260922 session's F9 stem-clobber and check-mark bugs both lived in those
    // caches). Without a resolver the defaults render Google checked and no
    // user-model entry (engine submenu degenerates to built-ins + manager).
    const EngineMenuView view = resolver_ ? resolver_() : EngineMenuView{};
    const int pref = view.preferred_engine;
    const std::string user_model_stem = view.user_model_stem;
    HMENU hEngineMenu = ::CreatePopupMenu();
    // REQ-045 P4-3 + REQ-046 P4-2 (Rev2 §B-3): flat 4-way check (0 = Google,
    // 1 = Local, 2 = OpenAI, 3 = 사용자 선택(.gguf)). The nested POPUP from
    // REQ-045 P4-5 is replaced by a CHECKABLE engine entry (the state the
    // user actually selects) plus a separate always-active file-picker row.
    ::AppendMenuW(hEngineMenu, MF_STRING | (pref == 0 ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_ENGINE_GOOGLE, I18n::Get(StringId::MenuEngineGoogle).c_str());
    ::AppendMenuW(hEngineMenu, MF_STRING | (pref == 1 ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_ENGINE_LOCAL, I18n::Get(StringId::MenuEngineLocal).c_str());
    ::AppendMenuW(hEngineMenu, MF_STRING | (pref == 2 ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_ENGINE_OPENAI, I18n::Get(StringId::MenuEngineOpenAi).c_str());

    // REQ-047 U1 (designer 164500 §1.2 Layer 3, decision #1): visual boundary
    // between the "built-in engines" group (Google / Built-in Local / OpenAI)
    // and the "user-provided model" group below. A separator is NOT a menu
    // item — the 4-way radio contract (REQ-046) and the 2010~2014 ID band
    // are untouched.
    ::AppendMenuW(hEngineMenu, MF_SEPARATOR, 0, nullptr);

    // REQ-050 (user items 3-1 + 3-2): the checkable 사용자 선택(.gguf) entry
    // is appended ONLY while a user model is registered — the stem the
    // resolver just derived (REQ-056; main.cpp resolves it from the registry
    // at this moment) non-empty. Nothing registered -> the submenu shows just
    // the manager row below. While registered, the " — <stem>" suffix rides
    // the label REGARDLESS of the checked engine: the old code appended the
    // suffix only at pref==3, so switching to another engine made the entry
    // read like nothing was registered. The frozen REQ-046 radio contract is
    // untouched (same ID, same 4-way check mark, conditional append), and the
    // localized "(미등록)" placeholder is gone together with the always-append
    // entry.
    if (!user_model_stem.empty()) {
        std::wstring user_gguf_label = I18n::Get(StringId::MenuEngineUserGguf);
        user_gguf_label += L" — " + ToUtf16(user_model_stem);
        ::AppendMenuW(hEngineMenu, MF_STRING | (pref == 3 ? MF_CHECKED : MF_UNCHECKED),
                      ID_TRAY_ENGINE_USER_GGUF, user_gguf_label.c_str());
    }
    // REQ-050: the former two plain rows — "다른 .gguf 모델 등록…"
    // (ID_TRAY_BROWSE_GGUF) and "모델 관리…" (ID_TRAY_MANAGE_GGUF) — are merged
    // into ONE manager row. The manager window offers both add methods
    // ([파일에서 추가…] routes into the exact file-picker registration
    // pipeline; [Hugging Face에서 추가…] downloads from a resolve URL) above
    // the registered-model list (rename/delete). PLAIN item (it opens a
    // dialog; no check geometry), sitting below the separator in the
    // user-model group. With no model registered this row is the ONLY
    // user-model entry in the submenu (REQ-050 3-1); main.cpp's INV-B3 guard
    // keeps the deferred manager open as a defensive path for a config/stem
    // desync (deferred via WM_APP+0x501 like this row's dispatch below).
    ::AppendMenuW(hEngineMenu, MF_STRING, ID_TRAY_MANAGE_GGUF,
                  I18n::Get(StringId::MenuManageGgufModels).c_str());
    // REQ-060: the load-mode settings row sits BELOW the manager row (the
    // mockup-tray-menu.png order). PLAIN entry row — no check mark by design
    // (it opens a dialog; the 4-way radio contract is untouched).
    ::AppendMenuW(hEngineMenu, MF_STRING, ID_TRAY_ENGINE_LOAD_SETTINGS,
                  I18n::Get(StringId::EngineLoadSettingsMenuRow).c_str());
    ::AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hEngineMenu), I18n::Get(StringId::MenuEngine).c_str());

    // 2b. R6 Phase 6 (plan §5.4): UI-language selector submenu, next to the
    // Engine submenu. Entries show ENDONYMS (each language in its own script)
    // plus a localized Auto entry. Check mark tracks the PERSISTED config
    // value (ui_language_), not the resolved locale, so "auto" stays visibly
    // selected while it follows the OS language.
    //
    // REQ-037/B-4 (design §2-Q3 verdict A): Auto + all 37 GetSupportedUiLocales
    // entries (38 items) as a SINGLE column - Windows auto-adds scroll
    // chevrons past screen height, the identical pattern the three 38-item
    // SRC/TGT pickers below already ship (no MF_MENUBARBREAK columns, no
    // owner-draw). Entry label = UiLocaleEntry.native_name (endonym only);
    // display order and Auto-first placement are owned by
    // GetSupportedUiLocales() (§2-Q3: KO, JA, zh-CN, zh-TW, VI, ES front
    // block, remaining locales in registry order, EN last). The loop is
    // size-dynamic (selector 37 => IDs 2301..2337, band guarded by the
    // static_assert above); the check mark is per-entry dynamic too.
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

    // 2c. UI Theme selector submenu (Fluent Craft / Warm Editorial / Refined Monolith)
    const I18nStrings i18n_ = I18n::GetThemeStrings();
    HMENU hThemeMenu = ::CreatePopupMenu();
    {
        const ui::ThemeId activeThemeId = ui::GetActiveThemeId();
        ::AppendMenuW(hThemeMenu,
                      MF_STRING | (activeThemeId == ui::ThemeId::Fluent ? MF_CHECKED : MF_UNCHECKED),
                      kCmdThemeFluent, i18n_.theme_fluent.c_str());
        ::AppendMenuW(hThemeMenu,
                      MF_STRING | (activeThemeId == ui::ThemeId::Editorial ? MF_CHECKED : MF_UNCHECKED),
                      kCmdThemeEditorial, i18n_.theme_editorial.c_str());
        ::AppendMenuW(hThemeMenu,
                      MF_STRING | (activeThemeId == ui::ThemeId::Monolith ? MF_CHECKED : MF_UNCHECKED),
                      kCmdThemeMonolith, i18n_.theme_monolith.c_str());
    }
    ::AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hThemeMenu), i18n_.menu_ui_theme.c_str());

    // 3+4. REQ-025 (Phase A §2.1.A3-25): the language pickers are grouped into
    // TWO contexts so typing-translation and drag-tooltip translations can use
    // different source/target pairs:
    //   키보드타이핑 (type)  > 출발언어 / 도착언어  — IDs 2100/2200 (UNCHANGED)
    //   번역툴팁   (drag)   > 출발언어 / 도착언어  — IDs 2400/2500 (NEW)
    // Submenu entry texts reuse MenuSourceLang/MenuTargetLang per the plan.
    const auto& allLangs = GetSupportedLanguages();
    const auto& tgtLangs = GetTargetLanguages();

    HMENU hSrcMenu = ::CreatePopupMenu();
    std::string normSrc = NormalizeLanguageCode(src_code_);
    for (size_t i = 0; i < allLangs.size(); ++i) {
        std::wstring itemText = ToUtf16(allLangs[i].code) + L" - " + I18n::GetLanguageDisplayName(allLangs[i].code);
        bool isCurrent = (normSrc == allLangs[i].code);
        ::AppendMenuW(hSrcMenu, MF_STRING | (isCurrent ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_SRC_BASE + static_cast<UINT>(i), itemText.c_str());
    }

    HMENU hTgtMenu = ::CreatePopupMenu();
    std::string normTgt = NormalizeLanguageCode(tgt_code_);
    for (size_t i = 0; i < tgtLangs.size(); ++i) {
        std::wstring itemText = ToUtf16(tgtLangs[i].code) + L" - " + I18n::GetLanguageDisplayName(tgtLangs[i].code);
        bool isCurrent = (normTgt == tgtLangs[i].code);
        ::AppendMenuW(hTgtMenu, MF_STRING | (isCurrent ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_TGT_BASE + static_cast<UINT>(i), itemText.c_str());
    }

    HMENU hTypingGroup = ::CreatePopupMenu();
    ::AppendMenuW(hTypingGroup, MF_POPUP, reinterpret_cast<UINT_PTR>(hSrcMenu), I18n::Get(StringId::MenuSourceLang).c_str());
    ::AppendMenuW(hTypingGroup, MF_POPUP, reinterpret_cast<UINT_PTR>(hTgtMenu), I18n::Get(StringId::MenuTargetLang).c_str());
    ::AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hTypingGroup), I18n::Get(StringId::MenuTypingGroup).c_str());

    // 4b. Drag ("번역툴팁") pair — check marks follow drag_src_code_/drag_tgt_code_.
    HMENU hDragSrcMenu = ::CreatePopupMenu();
    std::string normDragSrc = NormalizeLanguageCode(drag_src_code_);
    for (size_t i = 0; i < allLangs.size(); ++i) {
        std::wstring itemText = ToUtf16(allLangs[i].code) + L" - " + I18n::GetLanguageDisplayName(allLangs[i].code);
        bool isCurrent = (normDragSrc == allLangs[i].code);
        ::AppendMenuW(hDragSrcMenu, MF_STRING | (isCurrent ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_DRAG_SRC_BASE + static_cast<UINT>(i), itemText.c_str());
    }

    HMENU hDragTgtMenu = ::CreatePopupMenu();
    std::string normDragTgt = NormalizeLanguageCode(drag_tgt_code_);
    for (size_t i = 0; i < tgtLangs.size(); ++i) {
        std::wstring itemText = ToUtf16(tgtLangs[i].code) + L" - " + I18n::GetLanguageDisplayName(tgtLangs[i].code);
        bool isCurrent = (normDragTgt == tgtLangs[i].code);
        ::AppendMenuW(hDragTgtMenu, MF_STRING | (isCurrent ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_DRAG_TGT_BASE + static_cast<UINT>(i), itemText.c_str());
    }

    HMENU hTooltipGroup = ::CreatePopupMenu();
    ::AppendMenuW(hTooltipGroup, MF_POPUP, reinterpret_cast<UINT_PTR>(hDragSrcMenu), I18n::Get(StringId::MenuSourceLang).c_str());
    ::AppendMenuW(hTooltipGroup, MF_POPUP, reinterpret_cast<UINT_PTR>(hDragTgtMenu), I18n::Get(StringId::MenuTargetLang).c_str());
    // W2 (R1-22): the drag pair gets its OWN Swap row inside the 번역툴팁 group
    // (the type pair's Swap stays on the top level). PLAIN item, ID in the
    // >=2600 band, label reused from MenuSwapLangs - zero new i18n keys. The
    // dispatch applies the same AUTO-pivot policy as the type swap through the
    // shared PlanLanguageSwap planner, then routes through
    // ApplyLanguageChange(LanguageContext::Drag, ...) so persistence, the
    // drag-pair check marks, the tooltip label and the pin write all ride the
    // single language authority (INV-2/INV-3).
    ::AppendMenuW(hTooltipGroup, MF_STRING, ID_TRAY_DRAG_SWAP, I18n::Get(StringId::MenuSwapLangs).c_str());
    ::AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hTooltipGroup), I18n::Get(StringId::MenuTooltipGroup).c_str());

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
    } else if (cmd == ID_TRAY_ENGINE_OPENAI && callbacks_.on_select_engine) {
        // REQ-045 P4-3: index 2 = OpenAI Compatible.
        callbacks_.on_select_engine(2);
    } else if (cmd == ID_TRAY_ENGINE_USER_GGUF && callbacks_.on_select_engine) {
        // REQ-046 P4-2 (Rev2 §B-3): index 3 = 사용자 선택(.gguf). main.cpp
        // handles the no-model-registered case by routing into the merged
        // model manager (REQ-050).
        callbacks_.on_select_engine(3);
    } else if (cmd == ID_TRAY_MANAGE_GGUF) {
        // REQ-048 R2-D + REQ-050: open the merged user-model manager
        // (add-from-file / add-from-Hugging-Face / rename / delete). The
        // dialog open is DEFERRED to the controller window's GUI thread —
        // entering a modal synchronously here, while the TrackPopupMenuEx
        // modality tear-down may still be in flight, is the exact "minimized
        // window, no input" root cause REQ-047 D3 fixed for the OpenAI
        // settings (kMsgOpenOpenAiSettings = WM_APP+0x500). Same house
        // contract: a pure (0,0) wake-up, no payload crosses PostMessageW;
        // main.cpp owns the dialog + tray refresh. No coalescing latch is
        // needed: a second dispatch cannot run while the modal dialog blocks
        // this GUI thread's message handling.
        if (::PostMessageW(hOwner_, WM_APP + 0x501, 0, 0) == FALSE) {
            DIAG_F("UI/Tray/001: PostMessage gguf-manager open failed (GLE %lu)\n",
                   ::GetLastError());
        }
    } else if (cmd == ID_TRAY_ENGINE_LOAD_SETTINGS && callbacks_.on_open_engine_load_settings) {
        // REQ-060: open the load-mode settings dialog. The modal open itself
        // is DEFERRED by main.cpp (the on_open_engine_load_settings callback
        // posts kMsgOpenEngineLoadSettings to the controller window) — the
        // same modality contract as the manager row above: entering a modal
        // synchronously here, while the TrackPopupMenuEx modality tear-down
        // may still be in flight, is the REQ-047 D3 root cause.
        callbacks_.on_open_engine_load_settings();
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
    } else if (cmd >= ID_TRAY_DRAG_SRC_BASE && cmd < ID_TRAY_DRAG_SRC_BASE + allLangs.size()) {
        // REQ-025: drag ("번역툴팁") pair picks route to the drag callbacks.
        size_t idx = cmd - ID_TRAY_DRAG_SRC_BASE;
        if (callbacks_.on_select_drag_source_lang) {
            callbacks_.on_select_drag_source_lang(allLangs[idx].name_en);
        }
    } else if (cmd >= ID_TRAY_DRAG_TGT_BASE && cmd < ID_TRAY_DRAG_TGT_BASE + tgtLangs.size()) {
        size_t idx = cmd - ID_TRAY_DRAG_TGT_BASE;
        if (callbacks_.on_select_drag_target_lang) {
            callbacks_.on_select_drag_target_lang(tgtLangs[idx].name_en);
        }
    } else if (cmd == ID_TRAY_UILANG_AUTO && callbacks_.on_select_ui_language) {
        callbacks_.on_select_ui_language("auto");
    } else if (cmd == ID_TRAY_DRAG_SWAP && callbacks_.on_swap_drag_languages) {
        // W2 (R1-22): reverse the 번역툴팁 pair. The callback resolves the pair
        // through the shared PlanLanguageSwap planner and applies it via
        // ApplyLanguageChange(LanguageContext::Drag, ...), so a drag swap pins
        // the target exactly like an explicit drag-target pick (F2 contract).
        callbacks_.on_swap_drag_languages();
    } else if (cmd >= ID_TRAY_UILANG_BASE &&
               cmd < ID_TRAY_UILANG_BASE + static_cast<UINT>(GetSupportedUiLocales().size()) &&
               callbacks_.on_select_ui_language) {
        size_t idx = cmd - ID_TRAY_UILANG_BASE;
        callbacks_.on_select_ui_language(I18n::LocaleToString(GetSupportedUiLocales()[idx].locale));
    } else if (cmd == kCmdThemeFluent || cmd == kCmdThemeEditorial || cmd == kCmdThemeMonolith) {
        const std::string selected_theme =
            (cmd == kCmdThemeEditorial) ? "editorial"
            : (cmd == kCmdThemeMonolith) ? "monolith"
            : "fluent";
        ui_theme_ = selected_theme;
        if (on_theme_select_) {
            on_theme_select_(selected_theme);
        } else if (callbacks_.on_select_theme) {
            callbacks_.on_select_theme(selected_theme);
        }
    }

    ::DestroyMenu(hMenu);
}

} // namespace emebalachat
