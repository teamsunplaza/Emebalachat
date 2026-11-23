#include "tooltip.hpp"
#include "ui/theme.hpp"
#include "diag_logger.hpp"
#include "asset_loader.hpp"
#include "dpi.hpp"
#include "dwrite_helpers.hpp"  // REF-3.7 (session 260910_0006 T6): shared IsPointInRect / CloneFormatWithLocale
#include "../config.hpp"
#include "../i18n.hpp"
#include "../unicode_utils.hpp"
#include "../win32_input.hpp"
#include "../bidi_utils.hpp"  // P4 Batch B-2: IsRtlLanguageCode / DirectionForLocale
#include "language_search_palette.hpp" // 261116_0003 P6b: the deferred picker open + ghost predicate
// 261116_0003 P9b-2: the "내 언어" menu-row form and its ghost half are ONE
// definition shared with the tray's four menus (tray.hpp). The include carries
// only those two pure helpers plus the command-band constants; the tooltip's
// own band lives in tooltip.hpp and the pin proving the two spaces never
// collide runs in the test suite. Nothing here calls the tray's engine mapper
// (the P6b no-duplication pins stay green).
#include "tray.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>   // SEC-ADJ: value-queue marshal transport
#include <mutex>   // SEC-ADJ: guards the marshal deques
#include <optional> // 261116_0003 P6b: the deferred picker latch consumer
#include <shellapi.h> // P4 Batch-3 (REQ-C-002 / D7): ShellExecuteW ms-settings:speech deep link

#include <cctype>

namespace emebalachat {

// ---- SEC-ADJ (Blocker-5 class shatter fix, release readiness 260911_0002) ---
// The three cross-thread marshal seams (ShowTranslationThreadSafe,
// ShowMessageThreadSafe, RefreshTargetLanguageFromConfig) used to heap-
// allocate a payload and hand the raw pointer to WndProc through
// PostMessageW's LPARAM. The tooltip is a top-level WS_POPUP with a shipped,
// guessable class name (kTooltipClassName below) and WM_APP-based message
// ids, so ANY same-session process could post an attacker-chosen address that
// the handler then dereferenced (wstring/std::string member reads) and freed
// (CWE-822; see 173000_verify-sec-adjacent-lparam-seams.md). Transport is now
// the pattern proven in src/main.cpp for kMsgApplyLanguageSync (commit
// 46016d3): payloads travel BY VALUE through these mutex-guarded deques (one
// per message id, each with its own mutex so wake-ups never inter-block), and
// the window message is a PURE NOTIFICATION (wParam = lParam = 0, both
// ignored by the handler). Producers enqueue then post; a failed post leaves
// the entry queued (consumed by the next wake-up or the Destroy() drain) —
// rolling it back would race with a consumer that another producer's
// successful post already woke. Sole consumers: the GUI-thread WndProc drain
// loops and the discard-teardown sweep (DrainMarshalQueue) in Destroy().
// Queue-scope note: these are file-static, shared across TooltipWindow
// instances. Destroy() drains all three queues on the GUI thread before
// DestroyWindow (old purge point), so no entry survives its own window's
// lifetime in the sequential app/test lifecycle; a producer that races a
// teardown keeps its entry queued until the next drain instead of being
// deleted locally — same accepted trade-off as main.cpp's SEC-B5 queue.
namespace {

std::mutex g_tt_translation_mu;
std::deque<TooltipWindow::TranslationPayload> g_tt_translation_queue;

std::mutex g_tt_message_mu;
std::deque<TooltipWindow::MessagePayload> g_tt_message_queue;

std::mutex g_tt_targetlang_mu;
std::deque<TooltipWindow::TargetLangPayload> g_tt_targetlang_queue;

// 261108_0002 (WS3, v0.10.6): the floating back-translation card's value
// queue. Own mutex (never inter-blocks with the three above), same SEC-ADJ
// by-value transport: the posted kShowBackTranslationMessage is a pure
// (0, 0) wake-up notification.
std::mutex g_tt_backtrans_mu;
std::deque<TooltipWindow::BackTranslationPayload> g_tt_backtrans_queue;

// V10F L3-B: the progressive-preview value queue (Begin / Append / End). Own
// mutex, same SEC-ADJ by-value transport + parameter-less wake-up contract as
// the four above; the entry carries the cumulative text by value.
std::mutex g_tt_preview_mu;
std::deque<TooltipWindow::ChunkPreviewPayload> g_tt_preview_queue;

} // namespace

namespace {
const wchar_t kTooltipClassName[] = L"Emebalachat_TooltipClass";

// ---- 261116_0003 P6b: the two language dropdowns' dedicated row ids --------
// The language rows keep their UNCHANGED i + 1 ids (1..N over the engine
// view; N = 90 registry rows today) — the frozen dispatch scheme. The search
// row and the pinned-absent ghost row get DEDICATED ids in this band, far
// above any plausible registry size (the tray's band comments plan for ~200
// future rows; 2701 leaves ~26 hundred rows of headroom), so they can never
// collide with i + 1 for any list size. The band is the tooltip's OWN menu-id
// space (each dropdown is its own HMENU; nothing is shared with the tray's
// 2601..2608 band, which lives in tray.cpp's menus only). The static_assert
// kills a renumber of the band down into the language rows' i + 1 space.
constexpr UINT kTooltipCmdSearchSrc = 2701;
constexpr UINT kTooltipCmdSearchTgt = 2702;
constexpr UINT kTooltipCmdGhostSrc  = 2703;
constexpr UINT kTooltipCmdGhostTgt  = 2704;
// The static_assert proves the four ids are pairwise DISTINCT (the failure
// mode the old >=2701-only form could not see: two names collapsed onto the
// same value would both pass a band-floor check yet hand one cmd id to two
// different dispatch arms) AND all above the language rows' i + 1 horizon —
// the tray band asserts' idiom (tray.cpp's contiguous-band chain), applied
// to the tooltip's own band.
static_assert(kTooltipCmdSearchSrc >= 2701 && kTooltipCmdSearchTgt >= 2701 &&
                  kTooltipCmdGhostSrc >= 2701 && kTooltipCmdGhostTgt >= 2701 &&
                  kTooltipCmdSearchSrc != kTooltipCmdSearchTgt &&
                  kTooltipCmdSearchSrc != kTooltipCmdGhostSrc &&
                  kTooltipCmdSearchSrc != kTooltipCmdGhostTgt &&
                  kTooltipCmdSearchTgt != kTooltipCmdGhostSrc &&
                  kTooltipCmdSearchTgt != kTooltipCmdGhostTgt &&
                  kTooltipCmdGhostSrc != kTooltipCmdGhostTgt,
              "P6b: the four dedicated ids are pairwise distinct and stay in "
              "their >=2701 band, never inside the language rows' i + 1 space");

// 261116_0003 P9b-2: the "내 언어" user-row slots (tooltip.hpp). Two roles x
// kMaxUserLanguages ids, starting above every id this module already owns and
// above the tray's whole user band (2609..2864), so the two surfaces can never
// address one another's rows even if a future change ever shared a dispatcher.
// The layout is the same flat base + role * kMaxUserLanguages + index scheme
// the tray uses; the slot separation assert kills an off-by-one between the two
// roles, and the 16-bit bound keeps the ids inside the WM_COMMAND space.
static_assert(kTooltipCmdUserSrcBase > kTooltipCmdGhostTgt,
              "P9b-2: the user-row band must start above the P6b special rows");
static_assert(kTooltipCmdUserTgtBase ==
                  kTooltipCmdUserSrcBase + static_cast<UINT>(kMaxUserLanguages),
              "P9b-2: the target slot must start right after the source slot");
static_assert(kTooltipCmdUserBandEnd ==
                  kTooltipCmdUserTgtBase + static_cast<UINT>(kMaxUserLanguages),
              "P9b-2: two role slots of kMaxUserLanguages ids each");
static_assert(TooltipUserLangCommand(TooltipUserLangRole::Source, kMaxUserLanguages - 1) <
                  TooltipUserLangCommand(TooltipUserLangRole::Target, 0) &&
                  TooltipUserLangCommand(TooltipUserLangRole::Target, kMaxUserLanguages - 1) + 1 ==
                      kTooltipCmdUserBandEnd,
              "P9b-2: the source and target user-row slots must be disjoint and contiguous");
static_assert(kTooltipCmdUserBandEnd <= 0x10000,
              "P9b-2: the user-row band must stay inside the 16-bit menu-command space");
// 261116_0003 P9b-3 U4a: the tray claimed the two ids directly above its user
// band for the "내 언어 추가…" / "내 언어…" dialog rows (tray.cpp
// ID_TRAY_USER_LANG_ADD / _MANAGE = kTrayUserLangBandEnd / +1 = 2865/2866,
// pinned there by tray.cpp's own assert block). The tooltip user band must
// stay strictly above them so the two surfaces can never address one
// another's commands, exactly like the P9b-2 cross-band discipline.
static_assert(kTooltipCmdUserSrcBase > kTrayUserLangBandEnd + 1,
              "P9b-3 U4a: the tooltip user band must start above the tray's user-language "
              "dialog rows (2865/2866)");

// Strip LEADING emoji/symbol pseudo-icons (📋 \xD83D\xDCCB, 🔊 \xD83D\xDD0A, ✓ \x2713, ✔ \x2714)
// and any run of leading separators (checkmarks, VS16, spaces) so Copy/Copied/
// Speak labels pair cleanly with vector icons. F-10 (fix-list round 1): the
// old comment claimed TRAILING-space stripping, which this front-anchored
// loop never performs - only the front of the string is ever erased.
std::wstring StripLeadingEmojiIcon(std::wstring s) {
    while (!s.empty()) {
        if (s.size() >= 2 && s[0] >= 0xD800 && s[0] <= 0xDBFF &&
            s[1] >= 0xDC00 && s[1] <= 0xDFFF) {
            s.erase(0, 2);
        } else if (s[0] == L'\x2713' || s[0] == L'\x2714' ||
                   s[0] == L'\xFE0F' || s[0] == L' ') {
            s.erase(0, 1);
        } else {
            break;
        }
    }
    return s;
}

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

// V10F FUP-06: the production side of the injectable ResolveLcidForLanguage
// seam (tooltip.hpp). The SPECIFIC locale is asked for FIRST: with
// LOCALE_ALLOW_NEUTRAL_NAMES alone a bare language name answers the NEUTRAL
// LCID ("en" -> 0x0009, "ko" -> 0x0012) instead of the shipped 0x0409/0x0412,
// which silently dropped MatchTokenLanguage's exact-LCID bonus for every one of
// the 36 legacy languages (261122_0002 Wave-C regression, caught by the
// TestTtsVoiceSelectionModule LCID pins). Neutral-name resolution stays as the
// FALLBACK for script-only / neutral-only tags that a specific lookup rejects.
DWORD LocaleNameToLcidAllowNeutral(const wchar_t* bcp47_name) {
    if (!bcp47_name || !bcp47_name[0]) return 0;
    const DWORD specific = ::LocaleNameToLCID(bcp47_name, 0);
    if (specific != 0) return specific;
    return ::LocaleNameToLCID(bcp47_name, LOCALE_ALLOW_NEUTRAL_NAMES);
}

// V10F L3-B: {c}/{t}-style token fill for the preview header. Deliberately a
// file-local 6-liner instead of reaching into badge.hpp: the badge module owns
// its own copy for the badge's label, and this module must stay linkable
// without the badge window (the tooltip is created by main.cpp and by
// run_tests, the badge only by the app).
void ReplaceAllTokens(std::wstring& text, std::wstring_view token, const std::wstring& value) {
    if (token.empty()) return;
    std::size_t pos = 0;
    while ((pos = text.find(token, pos)) != std::wstring::npos) {
        text.replace(pos, token.size(), value);
        pos += value.size();
    }
}

// V10F L1-B: physical WORK-AREA height of the monitor under `pt` (taskbar and
// docked app bars excluded). Feeds MaxCardHeightDipForWorkArea, which is the
// pure planner the unit suite drives; this is only the Win32 read.
int WorkAreaHeightPixelsAtPoint(POINT pt) {
    HMONITOR mon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(MONITORINFO);
    if (::GetMonitorInfoW(mon, &mi)) {
        return static_cast<int>(mi.rcWork.bottom - mi.rcWork.top);
    }
    // Defensive fallback: the whole virtual screen is still bounded, and the
    // card's own 520 DIP cap applies on top, so a failed query can only make
    // the usable height LARGER than reality (never an unusable card).
    return static_cast<int>(::GetSystemMetrics(SM_CYVIRTUALSCREEN));
}

} // namespace

DWORD GetLcidForLanguageWithResolver(std::string_view target_lang_name_or_code,
                                     LocaleNameToLcidFn resolve) {
    if (target_lang_name_or_code.empty()) return 0;

    // Direct hex prefix check (e.g. "0x409", "0x0409") - unchanged from the
    // pre-V10F resolver, and it stays first so an explicit hex id always wins.
    if (target_lang_name_or_code.rfind("0x", 0) == 0 || target_lang_name_or_code.rfind("0X", 0) == 0) {
        try {
            return static_cast<DWORD>(std::stoul(std::string(target_lang_name_or_code.substr(2)), nullptr, 16));
        } catch (...) {}
    }

    // Canonical resolution via config.hpp's NormalizeLanguageCode (codes,
    // English names, native names and the alias matrix).
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

    if (code != "AUTO" && !code.empty()) {
        // V10F FUP-06 step 1: the explicit overrides (YUE/FIL/ZH-CN/ZH-TW) win
        // over the registry tag, because those four rows' Windows mapping is
        // not what bcp47 alone yields.
        DWORD override_lcid = 0;
        if (TryLcidOverride(code, override_lcid)) {
            return override_lcid;
        }
        // Step 2: the row's OWN bcp47 column -> LocaleNameToLCID. ONE table
        // feeds DWrite font fallback (P4 B-2) and voice matching, so the two
        // can never drift, and every future registry row resolves with no edit
        // here (the pre-V10F if-chain could not: 53 of the 89 rows answered 0).
        if (const LanguageInfo* info = FindLanguageByCode(code)) {
            if (info->bcp47 && info->bcp47[0] && resolve) {
                const std::wstring tag = ToUtf16(info->bcp47);
                if (!tag.empty()) {
                    const DWORD lcid = resolve(tag.c_str());
                    if (lcid != 0) return lcid;
                }
            }
        }
    }

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

DWORD GetLcidForLanguage(std::string_view target_lang_name_or_code) {
    return GetLcidForLanguageWithResolver(target_lang_name_or_code,
                                          &LocaleNameToLcidAllowNeutral);
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

    // 261108_0002 (WS3, v0.10.6): the floating back-translation card's two
    // formats. backtrans_body_format_ is the 13pt SEMI-BOLD card body the spec
    // calls for (body_format_ is 13.5pt regular - reusing it would silently
    // drop the weight); backtrans_ref_format_ is the muted 11.5pt [원문]
    // sub-panel body (v0.10.9 U-B: 11 -> 11.5, the report's comparison-card
    // size). Both are created here, never per render (C1 discipline) and
    // released in Destroy.
    dwrite_factory_->CreateTextFormat(
        fontName,
        nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        13.0f,
        L"",
        &backtrans_body_format_
    );
    if (backtrans_body_format_) {
        backtrans_body_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        backtrans_body_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    dwrite_factory_->CreateTextFormat(
        fontName,
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        11.5f,
        L"",
        &backtrans_ref_format_
    );
    if (backtrans_ref_format_) {
        backtrans_ref_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        backtrans_ref_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // v0.10.9 (U-B, report §4 Option 2): the two [역번역] / [원문] SECTION
    // labels. A dedicated format (not backtrans_body_format_) so the section
    // headers keep NO_WRAP and their own vertical centring inside the label
    // band: a wrapped or body-aligned label would drift out of its 18 DIP band
    // and over the body text the clip rect protects.
    dwrite_factory_->CreateTextFormat(
        fontName,
        nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        13.0f,
        L"",
        &backtrans_section_format_
    );
    if (backtrans_section_format_) {
        backtrans_section_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        backtrans_section_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        backtrans_section_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // Header row: LEFT-aligned (the shared header_format_ is CENTER-aligned
    // for the translation card's pills, which would centre the card title).
    dwrite_factory_->CreateTextFormat(
        fontName,
        nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        12.0f,
        L"",
        &backtrans_header_format_
    );
    if (backtrans_header_format_) {
        backtrans_header_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        backtrans_header_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        backtrans_header_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    InitSapi();
    return true;
}

void TooltipWindow::Destroy() {
    StopTTS();
    CleanupSapi();

    if (hwnd_) {
        // R6 Phase 3 (audit items 6+8) / SEC-ADJ: discard marshal payloads
        // still queued when the window goes away. The old purge point freed the
        // heap payloads DestroyWindow's queue purge would have leaked; with the
        // by-value deques the same call releases their string memory and keeps
        // the discard-without-apply semantics. Stray wake-up notifications
        // left in the OS queue are harmless (they carry no payload and find
        // empty deques). GUI-thread-only (queue scope).
        if (::GetCurrentThreadId() == gui_thread_id_) {
            DrainMarshalQueue();
        }
        ::KillTimer(hwnd_, kTimerCopiedFeedback);
        ::KillTimer(hwnd_, kTimerMessageAutohide);
        ::KillTimer(hwnd_, kTimerBackTranslateAutohide);
        ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    if (logo_bitmap_) { logo_bitmap_->Release(); logo_bitmap_ = nullptr; }
    // V10F FUP-08: the cached body layout holds the (about to be released)
    // DirectWrite factory, so it must go before the factory does.
    ReleaseBodyTextLayout();
    if (backtrans_section_format_) { backtrans_section_format_->Release(); backtrans_section_format_ = nullptr; }
    if (backtrans_header_format_) { backtrans_header_format_->Release(); backtrans_header_format_ = nullptr; }
    if (backtrans_ref_format_) { backtrans_ref_format_->Release(); backtrans_ref_format_ = nullptr; }
    if (backtrans_body_format_) { backtrans_body_format_->Release(); backtrans_body_format_ = nullptr; }
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
// src-tag pill width [was inline at Render], the two footer button widths
// [was inline at Render] and (REQ-052) the target pill width [was the
// hardcoded 90 DIP box at Render] as the ONLY genuinely per-frame
// CreateTextLayout measurement sites; the ShowTranslation body measures
// (content path, once per translation) were mislabeled and stay untouched. The label inputs
// change on content set / language pick / UI-locale string switch only, so
// the cache keys are the exact label strings; a key match skips all
// shaping. DirectWrite metrics are DPI-independent DIPs, so a DPI crossing
// needs no recompute (verified: the old blocks used constant 512 DIP extent
// constraints with no per-frame width drift). The hit-test rects
// (src_btn_rect_, copy_btn_rect_, tts_btn_rect_, lang_btn_rect_) are rebuilt
// every Render
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
        const std::wstring copy_label = StripLeadingEmojiIcon(I18n::Get(StringId::TooltipButtonCopy));
        const std::wstring copied_label = StripLeadingEmojiIcon(I18n::Get(StringId::TooltipCopied));
        const std::wstring tts_label = StripLeadingEmojiIcon(I18n::Get(StringId::TooltipButtonTts));
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
                    copy_w = lbl_metrics.width + 36.0f;
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
                    const float w_feedback = lbl_metrics.width + 36.0f;
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
                    tts_w = lbl_metrics.width + 36.0f;
                    if (tts_w < 82.0f) tts_w = 82.0f;
                    if (tts_w > 150.0f) tts_w = 150.0f;
                }
                lbl_layout->Release();
            }
            copy_btn_w_ = copy_w;
            tts_btn_w_ = tts_w;
            footer_labels_key_ = labels_key;
        }

        // --- site 7 (REQ-052): target-language pill width (button_format_) ---
        // The old Render box was a hardcoded 90 DIP regardless of the label,
        // so wide localized target names (e.g. "Portuguese (Brazil) ▾")
        // silently clipped inside it - and on a wide source tag the box ran
        // into the close ✕ (audit P1). Same pattern as site 3 (+16 DIP
        // padding, floor == the old box); Render clamps the result against
        // the close button through PlanHeaderTargetButton.
        const std::wstring tgt_name = target_lang_.empty()
                                          ? I18n::GetLanguageDisplayName("English")
                                          : ToUtf16(target_lang_);
        const std::wstring tgt_label = tgt_name + L" ▾";
        if (tgt_label != tgt_label_key_) {
            float tgt_w = 90.0f; // floor == the pre-REQ-052 hardcoded box (fallback parity)
            IDWriteTextLayout* tgt_layout = nullptr;
            if (SUCCEEDED(dwrite_factory_->CreateTextLayout(
                    tgt_label.c_str(), static_cast<UINT32>(tgt_label.size()),
                    button_format_, 512.0f, 24.0f, &tgt_layout)) && tgt_layout) {
                DWRITE_TEXT_METRICS tgt_metrics = {};
                if (SUCCEEDED(tgt_layout->GetMetrics(&tgt_metrics)) && tgt_metrics.width > 0.0f) {
                    tgt_w = tgt_metrics.width + 16.0f;
                    if (tgt_w < 90.0f) tgt_w = 90.0f;
                }
                tgt_layout->Release();
            }
            tgt_btn_w_ = tgt_w;
            tgt_label_key_ = tgt_label;
        }

        // --- site 8 (V10F L2): header action labels -------------------------
        // The three right-anchored header buttons (pin / edit / open) size from
        // their localized labels. Measured here, once per label change, so the
        // per-frame Render only does arithmetic (C2 discipline). The pin's
        // width is the WIDER of the two state labels (Pin/Unpin) so toggling
        // never reflows the header cluster under the user's pointer.
        const std::wstring pin_label = I18n::Get(StringId::PinTooltipCard);
        const std::wstring unpin_label = I18n::Get(StringId::UnpinTooltipCard);
        const std::wstring edit_label = I18n::Get(StringId::EditRetranslate);
        const std::wstring open_label = I18n::Get(StringId::OpenInWindow);
        const std::wstring action_key =
            pin_label + L'\0' + unpin_label + L'\0' + edit_label + L'\0' + open_label;
        if (action_key != header_action_key_) {
            auto measure_label = [&](const std::wstring& label) -> float {
                if (label.empty()) return 0.0f;
                IDWriteTextLayout* layout = nullptr;
                float width = 0.0f;
                if (SUCCEEDED(dwrite_factory_->CreateTextLayout(
                        label.c_str(), static_cast<UINT32>(label.size()),
                        button_format_, 512.0f, 24.0f, &layout)) && layout) {
                    DWRITE_TEXT_METRICS metrics = {};
                    if (SUCCEEDED(layout->GetMetrics(&metrics)) && metrics.width > 0.0f) {
                        width = metrics.width;
                    }
                    layout->Release();
                }
                return width;
            };
            float pin_w = measure_label(pin_label);
            const float unpin_w = measure_label(unpin_label);
            if (unpin_w > pin_w) pin_w = unpin_w;
            pin_btn_label_w_ = pin_w;
            edit_btn_label_w_ = measure_label(edit_label);
            open_btn_label_w_ = measure_label(open_label);
            header_action_key_ = action_key;
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

// ---- V10F FUP-08: cached body text layout --------------------------------
// Render() used to call DrawText on every repaint, i.e. DirectWrite re-ran the
// full shaping + line-breaking pipeline for the whole body on every wheel
// notch, hover change and DPI crossing. The layout is content-shaped exactly
// once per (content, column width, direction) change now.
//
// KEY: content pointer + length + column width + reading direction, plus a
// content hash. Pointer+length alone would be unsound - std::wstring reuses its
// SSO/allocated buffer address when a same-length string is assigned over it,
// so "show A, show B (same length)" would silently keep A's layout. The hash
// closes that (an O(n) FNV-1a pass at show time, next to a DirectWrite shaping
// pass that costs orders of magnitude more).
namespace {
std::uint64_t BodyLayoutContentHash(const std::wstring& text) {
    std::uint64_t hash = 1469598103934665603ULL; // FNV-1a 64-bit offset basis
    for (wchar_t c : text) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}
} // namespace

void TooltipWindow::ReleaseBodyTextLayout() {
    if (body_text_layout_) {
        body_text_layout_->Release();
        body_text_layout_ = nullptr;
    }
    body_layout_text_ptr_ = nullptr;
    body_layout_text_len_ = 0;
    body_layout_width_dip_ = 0;
    body_layout_direction_ = 0;
    body_layout_content_hash_ = 0;
}

void TooltipWindow::EnsureBodyTextLayout(int width_dip) {
    if (width_dip < 0) width_dip = 0;
    const int direction =
        (body_format_ &&
         body_format_->GetReadingDirection() == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT)
            ? 1
            : 0;
    const wchar_t* text_ptr = translated_text_.c_str();
    const std::size_t text_len = translated_text_.size();
    const std::uint64_t content_hash = BodyLayoutContentHash(translated_text_);
    if (body_text_layout_ && body_layout_text_ptr_ == text_ptr &&
        body_layout_text_len_ == text_len && body_layout_width_dip_ == width_dip &&
        body_layout_direction_ == direction && body_layout_content_hash_ == content_hash) {
        return; // unchanged key: the cached layout IS this frame's layout
    }
    ReleaseBodyTextLayout();
    if (!dwrite_factory_ || !body_format_ || width_dip <= 0 || translated_text_.empty()) {
        return; // Render falls back to its DrawText path
    }
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dwrite_factory_->CreateTextLayout(
            text_ptr, static_cast<UINT32>(text_len), body_format_,
            static_cast<float>(width_dip), 100000.0f, &layout)) || !layout) {
        DIAG_LOG("UI", "tooltip_body_layout_fail width=%d len=%zu", width_dip, text_len);
        return;
    }
    body_text_layout_ = layout;
    body_layout_text_ptr_ = text_ptr;
    body_layout_text_len_ = text_len;
    body_layout_width_dip_ = width_dip;
    body_layout_direction_ = direction;
    body_layout_content_hash_ = content_hash;
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
    // 261108_0002 (WS3, v0.10.6): "the next translation dismisses the
    // back-translation card". Hiding here (before the model is rewritten)
    // means the card is gone even if this show is later superseded, and the
    // translation card takes the window straight back over below.
    HideBackTranslationCard();
    // 261108_0002 (WS3, v0.10.6): width reset. ShowMessage narrows the card to
    // 320 DIP and the back-translation card is 360 DIP wide; without this
    // reset a translation shown after either of them kept the narrower width
    // (the body measured against current_width_ - 28 and the header pill
    // planner both read it), clipping the header and mis-wrapping the body.
    current_width_ = kBackTransCardWidthDip;
    // V10F L1-B: content-scaled width. The reset above stays the FLOOR (the
    // narrow tier IS 360 DIP), so this line can only widen the card: a long
    // translation earns 480/620 DIP instead of wrapping into a narrow ribbon.
    // Applied BEFORE the body measurement below, which reads current_width_ -
    // the same ordering rule the 360 reset has always followed.
    current_width_ = CardWidthDipForContentChars(translated_text.size());

    // REQ-R15 + V10F L1-B: resolve the target monitor's DPI here (the caller's
    // physical x/y is the monitor the card lands on) so the vertical cap below
    // can be expressed against that monitor's WORK AREA. The pre-V10F code
    // resolved it after the height math, which is why the cap had to be the
    // fixed 520 DIP constant for every display.
    dpi_ = emebalachat::ui::MonitorDpiAtPoint(POINT{ x, y });
    const int max_height_dip = MaxCardHeightDipForWorkArea(
        WorkAreaHeightPixelsAtPoint(POINT{ x, y }), dpi_);

    source_text_ = source_text;
    source_lang_code_ = source_lang_code;
    target_lang_ = target_lang;
    translated_text_ = translated_text;
    copied_feedback_ = false;
    hovered_btn_ = 0;
    // V10F L1-B + L3-B: a fresh card is UNPINNED and is not a live preview.
    // Both are per-card state, so "unpinned + no progress line" is the exact
    // pre-V10F behavior for every existing caller.
    pinned_.store(false, std::memory_order_relaxed);
    preview_phase_ = ChunkPreviewPhase::Inactive;
    preview_text_.clear();
    preview_index_ = 0;
    preview_total_ = 0;

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
    // V10F L1-B: the cap is now the TIGHTER of the fixed 520 DIP ceiling and
    // 70% of the anchoring monitor's work area, so the card can never cover a
    // short screen (the pre-V10F fixed cap did on a 1366x768 / 125% display).
    int calculated_height = static_cast<int>(std::ceil(38.0f + 12.0f + text_height + 16.0f + 36.0f));
    int height_cap = (std::min)(kMaxWindowHeightDip, max_height_dip);
    if (height_cap < kCardHeightMinDip) height_cap = kCardHeightMinDip;
    current_height_ = (std::max)(kCardHeightMinDip, (std::min)(calculated_height, height_cap));

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

    // V10F FUP-08: build the cached render layout at the FINAL column width
    // (w - 32 when the custom scrollbar is reserved, w - 28 otherwise) and with
    // the direction the show path just wrote into body_format_. Render() then
    // draws this layout instead of re-shaping the string on every repaint.
    EnsureBodyTextLayout(scrollable_ ? current_width_ - 32 : current_width_ - 28);

    // REQ-R15 (audit §5 latent item 3): the target monitor's DPI was captured
    // above (before the work-area height cap); allocate the physical buffer and
    // clamp in the same physical units.
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
    // 261111_0001 U1 (v0.10.7, D1a) + fix-list round 2 G-06: publishing this
    // fresh card is ONE locked transition (visible, no teardown owed, NEW card
    // generation), so a dismissal still queued for a previous card - or one
    // beginning concurrently on another thread - can never clear this card's
    // visibility or tear it down when its message is finally drained.
    card_visibility_.PublishShow();

    Render();
    UpdateLayered();
}

void TooltipWindow::ShowMessage(int x, int y, std::wstring_view header, std::wstring_view body,
                                uint64_t generation, DWORD autohide_ms) {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() != gui_thread_id_) {
        ShowMessageThreadSafe(x, y, header, body, generation, autohide_ms);
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
    // 261108_0002 (WS3, v0.10.6): a notice also replaces the back-translation
    // card (one display mode at a time on the shared layered window).
    HideBackTranslationCard();
    message_header_ = header;
    translated_text_ = body;
    source_text_.clear();
    source_lang_code_.clear();
    target_lang_.clear();
    copied_feedback_ = false;
    hovered_btn_ = 0;
    // V10F: a notice is a fresh card - unpinned, never a preview.
    pinned_.store(false, std::memory_order_relaxed);
    preview_phase_ = ChunkPreviewPhase::Inactive;

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
    // 261111_0001 U1 (v0.10.7, D1a) + fix-list round 2 G-06: publishing this
    // fresh card is ONE locked transition (visible, no teardown owed, NEW card
    // generation), so a dismissal still queued for a previous card - or one
    // beginning concurrently on another thread - can never clear this card's
    // visibility or tear it down when its message is finally drained.
    card_visibility_.PublishShow();

    Render();
    UpdateLayered();

    ::KillTimer(hwnd_, kTimerMessageAutohide);
    // V10F-R1 F-06: the caller's per-notice budget (kMessageAutohideMs for
    // every legacy notice; kTypingErrorAutohideMs for the typing-error card).
    ::SetTimer(hwnd_, kTimerMessageAutohide, autohide_ms, nullptr);
}

void TooltipWindow::ShowMessageThreadSafe(int x, int y, std::wstring_view header, std::wstring_view body,
                                          uint64_t generation, DWORD autohide_ms) {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() == gui_thread_id_) {
        ShowMessage(x, y, header, body, generation, autohide_ms);
        return;
    }
    // SEC-ADJ: payload travels BY VALUE through g_tt_message_queue; the posted
    // message is a pure wake-up notification (0, 0) that the handler reads
    // neither. On post failure the entry stays queued (drain-safe): the next
    // wake-up or the Destroy() drain pops it exactly once — see the queue-
    // scope note at the top of this file.
    MessagePayload payload;
    payload.x = x;
    payload.y = y;
    payload.header = header;
    payload.body = body;
    payload.generation = generation; // R6 B1-H1: travels INSIDE the payload
    payload.autohide_ms = autohide_ms; // V10F-R1 F-06: the budget rides along
    {
        std::lock_guard<std::mutex> lk(g_tt_message_mu);
        g_tt_message_queue.push_back(std::move(payload));
    }
    if (::PostMessageW(hwnd_, kShowMessageMessage, 0, 0) == FALSE) {
        DIAG_F("TT/Marshal/001: PostMessage kShowMessage failed (GLE %lu); payload left queued\n",
               ::GetLastError());
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
    // SEC-ADJ: by-value transport through g_tt_translation_queue; the posted
    // message is a pure (0, 0) wake-up. Failed post leaves the entry queued
    // (drain-safe) — same contract as ShowMessageThreadSafe above.
    TranslationPayload payload;
    payload.x = x;
    payload.y = y;
    payload.source_text = source_text;
    payload.source_lang_code = source_lang_code;
    payload.target_lang = target_lang;
    payload.translated_text = translated_text;
    payload.generation = generation; // R6 B1-H1
    {
        std::lock_guard<std::mutex> lk(g_tt_translation_mu);
        g_tt_translation_queue.push_back(std::move(payload));
    }
    if (::PostMessageW(hwnd_, kShowTranslationMessage, 0, 0) == FALSE) {
        DIAG_F("TT/Marshal/002: PostMessage kShowTranslation failed (GLE %lu); payload left queued\n",
               ::GetLastError());
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

// ---- V10F L3-B: progressive preview seams --------------------------------
// The transport mirrors the four Show* seams above (SEC-ADJ Blocker-5 class):
// the payload travels BY VALUE through g_tt_preview_queue and the posted
// kChunkPreviewMessage is a pure (0, 0) wake-up. The queue is LATEST-WINS with
// ONE merge exception (V10F-R1 F-08, see CoalesceChunkPreviewPayload): every
// Append carries the FULL cumulative state, so an older pending entry has no
// information the newest one lacks — EXCEPT an un-applied Begin, which is the
// only carrier of total_chunks and must survive until the drain so the
// Appends posted behind it are not dropped as "preview inactive". An End
// carries no cumulative of its own; the merge rule adopts the newest pending
// Append's text into it (V10F-R2 F-29) so the drain's End arm always lands
// the latest body.
namespace {

bool PostChunkPreviewPayload(HWND hwnd, TooltipWindow::ChunkPreviewPayload payload) {
    if (!hwnd) return false;
    {
        std::lock_guard<std::mutex> lk(g_tt_preview_mu);
        TooltipWindow::CoalesceChunkPreviewPayload(g_tt_preview_queue, std::move(payload));
    }
    if (::PostMessageW(hwnd, TooltipWindow::kChunkPreviewMessage, 0, 0) == FALSE) {
        DIAG_F("TT/Marshal/005: PostMessage kChunkPreview failed (GLE %lu); payload left queued\n",
               ::GetLastError());
        return false;
    }
    return true;
}
} // namespace

// V10F-R1 F-08: the production merge rule (declared in the header so the unit
// suite drives it headlessly). A Begin REPLACES everything queued before it (a
// new run supersedes the old one). An Append/End MERGES with a pending Begin
// instead of coalescing it away: the un-applied Begin (the only carrier of
// total_chunks) stays at the front and the newest Append/End rides behind it,
// so the drain applies Begin FIRST and the Appends are not dropped as
// "preview inactive". Older pending Appends/Ends are dropped (latest-wins
// within the run); the queue never holds more than one Begin + one op.
// V10F-R2 (fix-list round 2, F-29): an End payload carries no cumulative of
// its own, so before the latest-wins erase the End ADOPTS the newest pending
// Append's cumulative — otherwise a run whose last Append is still queued
// when End arrives would erase that final text and the drain's End arm would
// freeze the card on the (empty) Begin body.
void TooltipWindow::CoalesceChunkPreviewPayload(std::deque<ChunkPreviewPayload>& queue,
                                                ChunkPreviewPayload incoming) {
    if (incoming.op == ChunkPreviewOp::Begin) {
        // A new run supersedes whatever is still queued (pending Begin included).
        queue.clear();
        queue.push_back(std::move(incoming));
        return;
    }
    // V10F-R2 (F-29): carry the latest cumulative into the End payload.
    if (incoming.op == ChunkPreviewOp::End && incoming.cumulative.empty()) {
        for (auto it = queue.rbegin(); it != queue.rend(); ++it) {
            if (it->op == ChunkPreviewOp::Append && !it->cumulative.empty()) {
                incoming.cumulative = it->cumulative;
                break;
            }
        }
    }
    // Append / End: keep one pending Begin (it carries total_chunks into the
    // drain), drop every older pending Append/End (latest-wins within the
    // run), then ride behind the Begin.
    for (auto it = queue.begin(); it != queue.end();) {
        if (it->op == ChunkPreviewOp::Begin) {
            ++it;
        } else {
            it = queue.erase(it);
        }
    }
    queue.push_back(std::move(incoming));
}

void TooltipWindow::BeginChunkPreview(int total_chunks) {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() != gui_thread_id_) {
        ChunkPreviewPayload payload;
        payload.op = ChunkPreviewOp::Begin;
        payload.total_chunks = total_chunks;
        PostChunkPreviewPayload(hwnd_, std::move(payload));
        return;
    }
    ApplyChunkPreviewOp(ChunkPreviewOp::Begin, total_chunks, std::wstring(), false);
}

void TooltipWindow::AppendChunkPreviewText(const std::wstring& cumulative) {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() != gui_thread_id_) {
        ChunkPreviewPayload payload;
        payload.op = ChunkPreviewOp::Append;
        payload.cumulative = cumulative;
        PostChunkPreviewPayload(hwnd_, std::move(payload));
        return;
    }
    ApplyChunkPreviewOp(ChunkPreviewOp::Append, 0, cumulative, false);
}

void TooltipWindow::EndChunkPreview(bool success) {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() != gui_thread_id_) {
        ChunkPreviewPayload payload;
        payload.op = ChunkPreviewOp::End;
        payload.success = success;
        PostChunkPreviewPayload(hwnd_, std::move(payload));
        return;
    }
    ApplyChunkPreviewOp(ChunkPreviewOp::End, 0, std::wstring(), success);
}

void TooltipWindow::ApplyChunkPreviewOp(ChunkPreviewOp op, int total_chunks,
                                        const std::wstring& cumulative, bool success) {
    if (!hwnd_) return;
    switch (op) {
        case ChunkPreviewOp::Begin: {
            is_message_mode_ = false;
            no_voice_notice_active_ = false;
            ::KillTimer(hwnd_, kTimerMessageAutohide);
            HideBackTranslationCard();
            // Per-card state: a live preview is never pinned (the pin would
            // outlive the run and hold a stale partial text on screen).
            pinned_.store(false, std::memory_order_relaxed);
            preview_phase_ = ChunkPreviewPhase::Running;
            preview_total_ = total_chunks > 1 ? total_chunks : 1;
            preview_index_ = preview_total_ > 1 ? 0 : 1;
            preview_text_ = cumulative;
            POINT cursor = {};
            if (!::GetCursorPos(&cursor)) {
                cursor = POINT{0, 0};
            }
            // Same cursor-relative anchoring main.cpp uses for its notices;
            // the card then clamps itself inside the monitor's work area.
            preview_x_ = cursor.x + 16;
            preview_y_ = cursor.y + 16;
            DIAG_LOG("UI", "tooltip_preview_begin total=%d", preview_total_);
            LayoutChunkPreviewCard();
            break;
        }
        case ChunkPreviewOp::Append: {
            // A late emission from a superseded request must never resurrect a
            // card the user (or End) already retired.
            if (preview_phase_ == ChunkPreviewPhase::Inactive) {
                DIAG_LOG("UI", "tooltip_preview_append_ignored");
                return;
            }
            preview_text_ = cumulative;
            if (preview_index_ < preview_total_) ++preview_index_;
            preview_phase_ = ChunkPreviewPhase::Running;
            LayoutChunkPreviewCard();
            break;
        }
        case ChunkPreviewOp::End: {
            if (preview_phase_ == ChunkPreviewPhase::Inactive) return;
            if (!success) {
                DIAG_LOG("UI", "tooltip_preview_end success=0");
                DismissExplicit();
                return;
            }
            if (!cumulative.empty()) preview_text_ = cumulative;
            preview_index_ = preview_total_;
            preview_phase_ = ChunkPreviewPhase::Finished;
            DIAG_LOG("UI", "tooltip_preview_end success=1 len=%zu", preview_text_.size());
            LayoutChunkPreviewCard();
            break;
        }
    }
}

void TooltipWindow::LayoutChunkPreviewCard() {
    if (!hwnd_) return;
    // The body model is the SAME translated_text_ the translation card paints,
    // so the clip/scroll/hit-test state is shared (no fourth scroll
    // implementation).
    translated_text_ = preview_text_;
    source_text_.clear();
    source_lang_code_.clear();
    target_lang_.clear();
    message_header_.clear();
    is_message_mode_ = false;
    is_backtrans_mode_ = false;
    hovered_btn_ = 0;
    // Direction follows the text itself: the preview seam carries no target
    // language, and partial text may not be reliably attributable yet, so the
    // first-strong heuristic (bidi_utils) is the honest source.
    if (body_format_) {
        const bool rtl = GuessBaseDirection(translated_text_) == TextDirection::RTL;
        body_format_->SetReadingDirection(rtl ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT
                                              : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);
    }
    current_width_ = CardWidthDipForContentChars(translated_text_.size());

    float text_height = 40.0f;
    UINT32 text_line_count = 0;
    if (dwrite_factory_ && body_format_ && !translated_text_.empty()) {
        IDWriteTextLayout* layout = nullptr;
        if (SUCCEEDED(dwrite_factory_->CreateTextLayout(
                translated_text_.c_str(), static_cast<UINT32>(translated_text_.size()),
                body_format_, static_cast<float>(current_width_ - 28), 100000.0f,
                &layout)) && layout) {
            DWRITE_TEXT_METRICS m = {};
            layout->GetMetrics(&m);
            text_height = (std::max)(20.0f, m.height);
            text_line_count = m.lineCount;
            layout->Release();
        }
    }
    dpi_ = emebalachat::ui::MonitorDpiAtPoint(POINT{ preview_x_, preview_y_ });
    const int max_height_dip = MaxCardHeightDipForWorkArea(
        WorkAreaHeightPixelsAtPoint(POINT{ preview_x_, preview_y_ }), dpi_);
    int height_cap = (std::min)(kMaxWindowHeightDip, max_height_dip);
    if (height_cap < kCardHeightMinDip) height_cap = kCardHeightMinDip;
    const int wanted = static_cast<int>(
        std::ceil(kBodyTopDip + text_height + kPreviewFooterReserveDip));
    current_height_ = (std::max)(kCardHeightMinDip, (std::min)(wanted, height_cap));

    content_height_dip_ = text_height;
    line_height_dip_ = (text_line_count > 0) ? text_height / static_cast<float>(text_line_count)
                                             : 18.0f;
    const float viewport_h = static_cast<float>(current_height_) - kBodyTopDip -
                             kPreviewFooterReserveDip;
    scrollable_ = text_height > viewport_h;
    scroll_offset_dip_ = ClampScrollOffset(scroll_offset_dip_, content_height_dip_,
                                           viewport_h > 0.0f ? viewport_h : 0.0f);
    if (!scrollable_) scroll_offset_dip_ = 0.0f;
    scrollbar_track_rect_ = {};
    scrollbar_thumb_rect_ = {};

    EnsureBodyTextLayout(scrollable_ ? current_width_ - 32 : current_width_ - 28);
    ReallocateBuffer(PhysW(), PhysH());

    POINT origin = { preview_x_, preview_y_ };
    HMONITOR hMon = ::MonitorFromPoint(origin, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(MONITORINFO);
    if (::GetMonitorInfoW(hMon, &mi)) {
        origin = emebalachat::ui::ClampWindowOrigin(origin.x, origin.y, PhysW(), PhysH(), 10,
                                                    mi.rcWork);
    }
    ::SetWindowPos(hwnd_, HWND_TOPMOST, origin.x, origin.y, PhysW(), PhysH(),
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    card_visibility_.PublishShow();
    Render();
    UpdateLayered();
}

// ---- V10F L3-B: the live preview card ------------------------------------
// A fourth display mode on the same layered window, deliberately minimal: a
// progress header (the badge's own localized "Translating… {c}/{t}" string,
// token-filled in place), the ✕, the cumulative body through the SAME cached
// layout path the translation card uses, and a thin progress line in the footer
// band. No new strings, no new fonts, no new window.
void TooltipWindow::RenderChunkPreview() {
    if (!dc_render_target_ || !scratch_brush_) return;
    const auto& theme = ui::GetActiveTheme();
    const float w = static_cast<float>(current_width_);
    const float h = static_cast<float>(current_height_);

    // Header label. A single-chunk run keeps the plain "Translating…" copy
    // (the progress line still moves), which is what the pre-feature badge
    // showed for non-multi-chunk work.
    std::wstring title;
    if (preview_total_ > 1) {
        title = I18n::Get(StringId::BadgeTranslatingProgress);
        ReplaceAllTokens(title, L"{c}", std::to_wstring(preview_index_));
        ReplaceAllTokens(title, L"{t}", std::to_wstring(preview_total_));
    } else {
        title = I18n::Get(StringId::BadgeTranslating);
    }
    IDWriteTextFormat* title_format =
        backtrans_header_format_ ? backtrans_header_format_ : header_format_;
    if (title_format && !title.empty()) {
        const D2D1_RECT_F title_rect = D2D1::RectF(14.0f, 10.0f, w - 44.0f, 36.0f);
        scratch_brush_->SetColor(ui::ColorFromHex(theme.text_primary, 1.0f));
        dc_render_target_->DrawText(title.c_str(), static_cast<UINT32>(title.size()),
                                    title_format, title_rect, scratch_brush_);
    }

    close_btn_rect_ = D2D1::RectF(w - 32.0f, 8.0f, w - 12.0f, 28.0f);
    if (header_format_) {
        scratch_brush_->SetColor((hovered_btn_ == 4) ? ui::ColorFromHex(theme.state_danger, 1.0f)
                                                     : ui::ColorFromHex(theme.text_muted, 0.85f));
        dc_render_target_->DrawText(L"✕", 1, header_format_, close_btn_rect_, scratch_brush_);
    }
    // No footer buttons and no header tag/pill on this card: clear every rect a
    // previous mode left behind so no stale zone can answer a click.
    src_btn_rect_ = {};
    lang_btn_rect_ = {};
    copy_btn_rect_ = {};
    tts_btn_rect_ = {};
    pin_btn_rect_ = {};
    edit_btn_rect_ = {};
    open_btn_rect_ = {};

    // Header divider, same rule the other two cards draw.
    scratch_brush_->SetColor(ui::ColorFromHex(theme.border_subtle, theme.border_subtle_alpha));
    dc_render_target_->DrawLine(D2D1::Point2F(14.0f, 37.0f), D2D1::Point2F(w - 14.0f, 37.0f),
                                scratch_brush_, 1.0f);

    // Body: the measured column (w - 32 while the scrollbar gutter is reserved,
    // w - 28 otherwise) with the show path's cached layout.
    const float body_top = kBodyTopDip;
    const float bar_bottom = h - 10.0f;
    const float bar_top = bar_bottom - kPreviewBarHeightDip;
    const float body_bottom = bar_top - 8.0f;
    const float col_left = 14.0f;
    const float col_right = w - static_cast<float>(scrollable_ ? 32 : 28);
    body_viewport_rect_ = D2D1::RectF(col_left, body_top, col_right,
                                      body_bottom > body_top ? body_bottom : body_top);
    if (!translated_text_.empty() && body_format_ && body_bottom > body_top) {
        dc_render_target_->PushAxisAlignedClip(body_viewport_rect_,
                                              D2D1_ANTIALIAS_MODE_ALIASED);
        scratch_brush_->SetColor(ui::ColorFromHex(theme.text_primary, 1.0f));
        if (body_text_layout_) {
            dc_render_target_->DrawTextLayout(
                D2D1::Point2F(col_left, body_top - scroll_offset_dip_), body_text_layout_,
                scratch_brush_);
        } else {
            const D2D1_RECT_F body_rect =
                D2D1::RectF(col_left, body_top - scroll_offset_dip_, col_right,
                            body_top - scroll_offset_dip_ + content_height_dip_ + 40.0f);
            dc_render_target_->DrawText(translated_text_.c_str(),
                                        static_cast<UINT32>(translated_text_.size()),
                                        body_format_, body_rect, scratch_brush_);
        }
        dc_render_target_->PopAxisAlignedClip();
    }

    // Thin progress line: track + accent fill, proportional to the accepted
    // chunk count. The finished card pins the fill at 100% instead of hiding it
    // (the card is a result surface until the caller's ShowTranslation
    // replaces it).
    preview_progress_track_rect_ = D2D1::RectF(col_left, bar_top, w - 14.0f, bar_bottom);
    float fraction = 0.0f;
    if (preview_total_ > 0) {
        const int clamped_index =
            preview_index_ < 0 ? 0 : (preview_index_ > preview_total_ ? preview_total_
                                                                      : preview_index_);
        fraction = static_cast<float>(clamped_index) / static_cast<float>(preview_total_);
    }
    if (preview_phase_ == ChunkPreviewPhase::Finished) fraction = 1.0f;
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;
    preview_progress_fill_rect_ = D2D1::RectF(
        col_left, bar_top, col_left + (w - 14.0f - col_left) * fraction, bar_bottom);
    scratch_brush_->SetColor(ui::ColorFromHex(theme.surface_elevated, 0.85f));
    dc_render_target_->FillRoundedRectangle(
        D2D1::RoundedRect(preview_progress_track_rect_, 1.5f, 1.5f), scratch_brush_);
    if (preview_progress_fill_rect_.right > preview_progress_fill_rect_.left) {
        scratch_brush_->SetColor(ui::ColorFromHex(theme.accent_primary, 1.0f));
        dc_render_target_->FillRoundedRectangle(
            D2D1::RoundedRect(preview_progress_fill_rect_, 1.5f, 1.5f), scratch_brush_);
    }

    // Scrollbar: the preview body can overflow on a long run, and the shared
    // REQ-002 thumb math already runs against content_height_dip_.
    if (scrollable_) {
        const float track_left = w - 12.0f;
        scrollbar_track_rect_ = D2D1::RectF(track_left, body_top, track_left + 8.0f, body_bottom);
        const float track_h = body_bottom - body_top;
        const float thumb_h = ScrollbarThumbHeightDip(track_h, track_h, content_height_dip_);
        const float thumb_top = ScrollbarThumbTopDip(body_top, track_h, thumb_h,
                                                     scroll_offset_dip_, content_height_dip_,
                                                     track_h);
        scrollbar_thumb_rect_ = D2D1::RectF(track_left, thumb_top, track_left + 8.0f,
                                            thumb_top + thumb_h);
        scratch_brush_->SetColor(ui::ColorFromHex(theme.surface_elevated, 0.75f));
        dc_render_target_->FillRoundedRectangle(
            D2D1::RoundedRect(scrollbar_track_rect_, 4.0f, 4.0f), scratch_brush_);
        scratch_brush_->SetColor(ui::ColorFromHex(theme.text_muted, 1.0f));
        dc_render_target_->FillRoundedRectangle(
            D2D1::RoundedRect(scrollbar_thumb_rect_, 4.0f, 4.0f), scratch_brush_);
    } else {
        scrollbar_track_rect_ = {};
        scrollbar_thumb_rect_ = {};
    }
}

void TooltipWindow::DismissThreadSafe() {
    if (!hwnd_) return;
    // 261111_0001 U1 (v0.10.7, audit D1a/D1b) + G-06 round 2: delegate to
    // Dismiss() for BOTH thread flavors. The old body posted kDismissMessage
    // directly on the off-thread path (visibility stayed set until the GUI
    // thread drained it); Dismiss() now performs the whole show/dismiss
    // transition through TooltipCardVisibility before returning on any thread
    // (visibility cleared, teardown owed, card generation captured) and the
    // message carries that generation for the exactly-once GUI teardown.
    //
    // V10F L1-B: this seam is the PROGRAMMATIC dismissal (main.cpp's tray
    // "back-translation off" row, the shutdown paths and the marshal tests) -
    // not a user click outside the card. A pinned card must not outlive the
    // feature that spawned it, so the pin is released first; an outside click /
    // ESC still reaches the pin-gated Dismiss() directly.
    pinned_.store(false, std::memory_order_relaxed);
    Dismiss();
}

// ===========================================================================
// 261108_0002 (WS3, v0.10.6): floating back-translation verification card
// ===========================================================================
// User intent (verbatim): "역번역 플로팅 확인 UI: 번역 결과(BBBBB)를 원문
// 언어로 자동 역번역(AAAAA')하여 플로팅 카드로 보여주기".
//
// The card is a THIRD display mode on the SAME layered window (translation /
// REQ-R08 notice / back-translation). Reusing the existing window is
// deliberate: one WS_EX_LAYERED | TOPMOST | TOOLWINDOW | NOACTIVATE popup,
// one D2D target, one marshal-queue family, one timer band. Showing the card
// replaces whatever was displayed; the next ShowTranslation or ShowMessage
// replaces it back.
//
// Threading: the back-translation worker calls ShowBackTranslationThreadSafe,
// which enqueues a value payload and posts a parameter-less wake-up (SEC-ADJ
// Blocker-5 contract). Everything below the GUI-thread entry runs on the
// thread that owns the D2D target.

// ---- pure label helpers ---------------------------------------------------
// Header: "<localized title> (SRC → TGT)". Empty labels are dropped instead of
// rendering an empty "()" - a caller that could not resolve a language still
// gets a clean title line.
std::wstring TooltipWindow::FormatBackTransHeader(const std::wstring& title,
                                                  const std::wstring& source_label,
                                                  const std::wstring& target_label) {
    std::wstring out = title;
    if (source_label.empty() && target_label.empty()) {
        return out;
    }
    out += L" (";
    out += source_label;
    if (!source_label.empty() && !target_label.empty()) {
        out += L" → ";
    }
    out += target_label;
    out += L")";
    return out;
}

// Reference line: "<localized label>AAAAA", e.g. "↳ 원문: AAAAA" in Korean and
// "↳ Original: AAAAA" in English.
// 261108_0002 (FIX-WS3B, v0.10.6): the label used to be a hardcoded Korean
// literal here, so "원문:" leaked into all 37 UI languages while the header and
// the two pills were localized.
// v0.10.9 (U-B): FormatBackTransReference is GONE. The original now lives in
// its own [원문] sub-panel whose localized section header comes from
// StringId::BackTransSectionOriginal, so a second inline "↳ 원문: " label would
// duplicate the panel's own heading. The pre-v0.10.9 inline form is therefore
// dead code, not a fallback.

// v0.10.9 (U-B, report §4 defect 3-5a): the original is ALWAYS shown.
// The pre-v0.10.9 rule dropped the reference line whenever the round trip was
// textually exact (AAAAA' == AAAAA). That premise is void: the input box
// already REPLACED the user's text with the translation, so the comparison
// card is the only place the original still exists - and the exact-round-trip
// case is precisely the one the user wants to verify. The policy is now
// "whenever there is an original", and `back_text` is kept (and ignored) so
// the seam's shape and its pins stay stable.
bool TooltipWindow::BackTransNeedsReferenceLine(const std::wstring& original_text,
                                                const std::wstring& back_text) {
    (void)back_text;
    return !original_text.empty();
}

// ---- timer plan application ----------------------------------------------
void TooltipWindow::ApplyBackTransTimerPlan(BackTransTimerEvent ev) {
    if (!hwnd_) return;
    // V10F L1-B: a PINNED card ignores the autohide timers outright. Both
    // "start the countdown" events (Shown / the hover grace) are dropped; the
    // pause/kill events still run so a pin taken while the timer is armed
    // cancels it.
    if (pinned_.load(std::memory_order_relaxed) &&
        (ev == BackTransTimerEvent::Shown || ev == BackTransTimerEvent::PointerLeft)) {
        ::KillTimer(hwnd_, kTimerBackTranslateAutohide);
        return;
    }
    // v0.10.9 (U-B, defect 3-5c): the arming delay is the ADAPTIVE one measured
    // for THIS card (BackTransAutohideMsForContent), not the fixed floor.
    const BackTransTimerPlan plan =
        PlanBackTransTimer(ev, backtrans_hover_paused_, backtrans_autohide_ms_);
    switch (plan.action) {
        case BackTransTimerAction::ArmAutohide:
            ::KillTimer(hwnd_, kTimerBackTranslateAutohide);
            ::SetTimer(hwnd_, kTimerBackTranslateAutohide, plan.delay_ms, nullptr);
            break;
        case BackTransTimerAction::ArmGrace:
            // The pointer is no longer over the card, so the pause flag is
            // released here: re-entering during the grace window must pause
            // the countdown again (spec: "pause on hover"), and without this
            // reset the planner would treat the re-entry as a repeated move
            // and let the grace timer run out under the pointer.
            ::KillTimer(hwnd_, kTimerBackTranslateAutohide);
            ::SetTimer(hwnd_, kTimerBackTranslateAutohide, plan.delay_ms, nullptr);
            backtrans_hover_paused_ = false;
            break;
        case BackTransTimerAction::PauseAutohide:
            ::KillTimer(hwnd_, kTimerBackTranslateAutohide);
            backtrans_hover_paused_ = true;
            break;
        case BackTransTimerAction::KillTimers:
            ::KillTimer(hwnd_, kTimerBackTranslateAutohide);
            backtrans_hover_paused_ = false;
            break;
        case BackTransTimerAction::None:
            break;
    }
}

// v0.10.9 (U-B, defect 3-5c): a wheel notch or a thumb drag is a user signal
// that the card is still being read, so it re-arms the adaptive countdown.
// While the pointer hovers the card the countdown is ALREADY paused (hover
// pause) - arming there would close the card under the reader's pointer, which
// is exactly the defect this fix removes. So: re-arm only when the countdown
// is actually running.
void TooltipWindow::RearmBackTransAutohideForScroll() {
    if (!is_backtrans_mode_ || !hwnd_) return;
    if (backtrans_hover_paused_) return; // hover pause already holds the card open
    ApplyBackTransTimerPlan(BackTransTimerEvent::Shown);
}

// Hide the card WITHOUT touching the translation model. Used by the
// dismiss-on-new-translation rule (which re-shows the card immediately with
// fresh content) and by the mode-switch paths in ShowTranslation/ShowMessage.
void TooltipWindow::HideBackTranslationCard() {
    ::KillTimer(hwnd_, kTimerBackTranslateAutohide);
    backtrans_hover_paused_ = false;
    backtrans_close_hover_ = false;
    backtrans_close_rect_ = {};
    // v0.10.9 (U-B): a drag in flight when the card is replaced must not leave
    // the mouse captured by a window that is no longer showing this card. No
    // repaint here: the caller is about to show a fresh card over this one.
    if (dragging_thumb_) {
        dragging_thumb_ = false;
        thumb_hover_ = false;
        ::ReleaseCapture();
    }
    if (!is_backtrans_mode_) {
        return; // nothing displayed in this mode: leave the other modes alone
    }
    is_backtrans_mode_ = false;
    backtrans_original_.clear();
    backtrans_text_.clear();
    backtrans_header_.clear();
    backtrans_ref_line_.clear();
    backtrans_matched_ = false;
    backtrans_panel_plan_ = BackTransPanelPlan{};
    backtrans_autohide_ms_ = kBackTransAutohideMs;
    scrollable_ = false;
    scroll_offset_dip_ = 0.0f;
    content_height_dip_ = 0.0f;
    thumb_hover_ = false;
    scrollbar_track_rect_ = {};
    scrollbar_thumb_rect_ = {};
    // V10F FUP-05: the pane/view state belongs to the card that is going away.
    backtrans_view_ = BackTransView::Split;
    backtrans_scroll_pane_ = BackTransScrollPane::Back;
    backtrans_ref_offset_dip_ = 0.0f;
    backtrans_ref_track_rect_ = {};
    backtrans_ref_thumb_rect_ = {};
    backtrans_back_label_rect_ = {};
    backtrans_ref_label_rect_ = {};
    backtrans_back_label_hover_ = false;
    backtrans_ref_label_hover_ = false;
    backtrans_pin_rect_ = {};
    backtrans_pin_hover_ = false;
    backtrans_measured_back_h_ = 0;
    backtrans_measured_ref_h_ = 0;
    backtrans_header_extra_h_ = 0;
}

// ---- V10F FUP-05: RTL + locale for one comparison-card body format --------
// Same clone-swap discipline as ShowTranslation's body_format_ block (DWrite's
// localeName is creation-only, so a tag change is a clone; an unchanged tag -
// case-folded, DWrite lowercases on readback - never churns the COM object).
// Direction and alignment are applied to whichever format survives, so a failed
// clone degrades to "correct direction, previous font hint" instead of to no
// RTL at all.
//
// ALIGNMENT note (deliberate deviation from the brief's "alignment trailing"):
// DWRITE_TEXT_ALIGNMENT_LEADING/TRAILING are READING-DIRECTION-RELATIVE, so
// LEADING already hugs the RIGHT edge once the direction is RTL, and TRAILING
// would flip the paragraph back to the left. The assignment below therefore
// pins LEADING explicitly for both directions (the RTL hug is produced by the
// direction, exactly as DirectWrite intends); the test suite pins the resulting
// reading direction, which is the observable that matters.
void TooltipWindow::ApplyBackTransBodyFormat(IDWriteTextFormat*& slot, bool rtl,
                                            const std::string& norm_code) {
    if (!slot) return;
    const LanguageInfo* tag_info = FindLanguageByCode(norm_code);
    if (dwrite_factory_ && tag_info && tag_info->bcp47 && tag_info->bcp47[0]) {
        const std::wstring tag = ToUtf16(tag_info->bcp47);
        wchar_t cur_locale[64] = {};
        const bool same_locale =
            SUCCEEDED(slot->GetLocaleName(cur_locale, 64)) &&
            EqualsIgnoreCaseAscii<wchar_t>(cur_locale, tag);
        if (!tag.empty() && !same_locale) {
            IDWriteTextFormat* swapped = CloneFormatWithLocale(dwrite_factory_, slot, tag.c_str());
            if (swapped) {
                slot->Release();
                slot = swapped;
            } else {
                DIAG_LOG("UI", "tooltip_backtrans_locale_swap_fail tag=%ls", tag.c_str());
            }
        }
    }
    slot->SetReadingDirection(rtl ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT
                                  : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);
    slot->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
}

// ---- V10F FUP-08: the DPI of the monitor the card will appear on -----------
UINT TooltipWindow::AnchorMonitorDpi(HWND target_hwnd, HWND badge_hwnd) const {
    if (target_hwnd && ::IsWindow(target_hwnd)) {
        return emebalachat::ui::WindowDpi(target_hwnd);
    }
    if (badge_hwnd && ::IsWindow(badge_hwnd)) {
        return emebalachat::ui::WindowDpi(badge_hwnd);
    }
    POINT cursor = {};
    if (!::GetCursorPos(&cursor)) {
        cursor = POINT{0, 0};
    }
    return emebalachat::ui::MonitorDpiAtPoint(cursor);
}

// ---- V10F FUP-05: the split <-> solo panel toggle -------------------------
// Re-plans from the SHOW-TIME measurements (backtrans_measured_*_h_), so one
// click costs an integer planner call, not a DirectWrite pass. Everything the
// renderer and the hit tests read is rebuilt here (plan, card height, scroll
// extents, pane clamps, window size), which is why the toggle cannot desync
// measure from render: the plan remains the single source of truth.
void TooltipWindow::ApplyBackTransView(BackTransView view) {
    if (!hwnd_ || !is_backtrans_mode_) return;
    if (::GetCurrentThreadId() != gui_thread_id_) return; // GUI-thread only
    backtrans_view_ = view;
    const BackTransPanelPlan plan = PlanBackTransViewPanels(
        backtrans_view_, backtrans_measured_back_h_, backtrans_measured_ref_h_,
        backtrans_header_extra_h_);
    backtrans_panel_plan_ = plan;
    current_width_ = kBackTransCardWidthDip;
    current_height_ = plan.card_height_dip;
    scrollable_ = plan.scrollable;
    content_height_dip_ = static_cast<float>(plan.max_scroll_offset + plan.shared_viewport_h);
    // V10F FUP-05: both pane offsets survive the toggle but are re-clamped
    // against the NEW viewports (a pane that just gained height may have lost
    // all of its scroll range).
    scroll_offset_dip_ = ClampScrollOffset(scroll_offset_dip_,
                                           static_cast<float>(plan.back_content_h),
                                           static_cast<float>(plan.back_view_h));
    backtrans_ref_offset_dip_ = ClampScrollOffset(backtrans_ref_offset_dip_,
                                                  static_cast<float>(plan.ref_content_h),
                                                  static_cast<float>(plan.ref_view_h));
    backtrans_scroll_pane_ = backtrans_view_ == BackTransView::RefOnly
                                 ? BackTransScrollPane::Ref
                                 : BackTransScrollPane::Back;
    if (dragging_thumb_) {
        dragging_thumb_ = false;
        ::ReleaseCapture();
    }
    DIAG_LOG("UI", "tooltip backtrans_view=%d height=%d scrollable=%d",
             static_cast<int>(view), current_height_, plan.scrollable ? 1 : 0);
    ReallocateBuffer(PhysW(), PhysH());
    RECT wr = {};
    if (::GetWindowRect(hwnd_, &wr)) {
        // Keep the card's top-left origin (the reader's eye is there) and only
        // re-clamp: a taller card must not run off the monitor's work area.
        POINT probe = {static_cast<LONG>(wr.left), static_cast<LONG>(wr.top)};
        HMONITOR mon = ::MonitorFromPoint(probe, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(MONITORINFO);
        POINT origin = {wr.left, wr.top};
        if (::GetMonitorInfoW(mon, &mi)) {
            origin = emebalachat::ui::ClampWindowOrigin(wr.left, wr.top, PhysW(), PhysH(), 10,
                                                        mi.rcWork);
        }
        ::SetWindowPos(hwnd_, HWND_TOPMOST, origin.x, origin.y, PhysW(), PhysH(),
                       SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    Render();
    UpdateLayered();
    // A view switch is a "still reading" signal for the adaptive autohide.
    RearmBackTransAutohideForScroll();
}


// ---- 3-tier anchor resolution --------------------------------------------
// Tier 1 (caret): GetGUIThreadInfo on the TARGET's thread. The caret rect is in
//   the focus window's client coordinates, so it is mapped through that window
//   (ClientToScreen) - not through target_hwnd, which may be an ancestor. When
//   the caret is empty/absent, the focus window itself is used, but only while
//   it is a compact control (<= 180 px tall: an edit box, not a whole app
//   window whose top edge would put the card in the title bar).
// Tier 2 (badge): the floating badge's window rect, when it is visible.
// Tier 3 (fallback): the target window's bottom-left inner area, or the mouse
//   cursor when no usable target window exists.
// Every tier funnels through the pure PlaceBackTransCard, so the geometry and
// the work-area clamp are shared and unit-tested.
POINT TooltipWindow::ResolveBackTranslationAnchor(HWND target_hwnd, HWND badge_hwnd,
                                                  int card_w_phys, int card_h_phys) const {
    if (card_w_phys <= 0) card_w_phys = emebalachat::ui::ScaleDipsToPixels(kBackTransCardWidthDip, dpi_);
    if (card_h_phys <= 0) card_h_phys = emebalachat::ui::ScaleDipsToPixels(kBackTransCardMinHeightDip, dpi_);
    const int gap_px = emebalachat::ui::ScaleDipsToPixels(12, dpi_);

    // Work area of the monitor the anchor lives on; resolved per tier below.
    const auto work_area_for = [](POINT pt) -> RECT {
        HMONITOR hMon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(MONITORINFO);
        if (::GetMonitorInfoW(hMon, &mi)) {
            return mi.rcWork;
        }
        RECT fallback = {0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN)};
        return fallback;
    };

    // ---- Tier 1: caret of the target's GUI thread ----
    if (target_hwnd && ::IsWindow(target_hwnd)) {
        const DWORD tid = ::GetWindowThreadProcessId(target_hwnd, nullptr);
        GUITHREADINFO gti = {};
        gti.cbSize = sizeof(GUITHREADINFO);
        if (tid != 0 && ::GetGUIThreadInfo(tid, &gti)) {
            RECT anchor = {};
            bool have_anchor = false;
            if (gti.hwndCaret && (gti.rcCaret.right > gti.rcCaret.left ||
                                  gti.rcCaret.bottom > gti.rcCaret.top)) {
                // rcCaret is in hwndCaret's CLIENT coordinates.
                POINT tl = {gti.rcCaret.left, gti.rcCaret.top};
                POINT br = {gti.rcCaret.right, gti.rcCaret.bottom};
                if (::ClientToScreen(gti.hwndCaret, &tl) && ::ClientToScreen(gti.hwndCaret, &br)) {
                    anchor = RECT{tl.x, tl.y, br.x, br.y};
                    have_anchor = true;
                }
            }
            if (!have_anchor && gti.hwndFocus && ::IsWindow(gti.hwndFocus)) {
                RECT fr = {};
                if (::GetWindowRect(gti.hwndFocus, &fr)) {
                    const int focus_h = static_cast<int>(fr.bottom - fr.top);
                    // Compact control only: a whole-app focus window would
                    // anchor the card against the title bar.
                    if (focus_h > 0 && focus_h <= emebalachat::ui::ScaleDipsToPixels(180, dpi_)) {
                        anchor = fr;
                        have_anchor = true;
                    }
                }
            }
            if (have_anchor) {
                const POINT probe = {static_cast<LONG>(anchor.left), static_cast<LONG>(anchor.top)};
                return PlaceBackTransCard(anchor, /*above=*/true, gap_px,
                                          card_w_phys, card_h_phys, work_area_for(probe));
            }
        }
    }

    // ---- Tier 2: the floating badge ----
    if (badge_hwnd && ::IsWindow(badge_hwnd) && ::IsWindowVisible(badge_hwnd)) {
        RECT br = {};
        if (::GetWindowRect(badge_hwnd, &br)) {
            const POINT probe = {static_cast<LONG>((br.left + br.right) / 2),
                                 static_cast<LONG>((br.top + br.bottom) / 2)};
            return PlaceBackTransCard(br, /*above=*/true, gap_px,
                                      card_w_phys, card_h_phys, work_area_for(probe));
        }
    }

    // ---- Tier 3: bottom-left inner area of the target, else the cursor ----
    if (target_hwnd && ::IsWindow(target_hwnd)) {
        RECT tr = {};
        if (::GetWindowRect(target_hwnd, &tr)) {
            // A thin synthetic anchor at the window's inner bottom-left: the
            // card is placed ABOVE it, i.e. inside the lower part of the
            // window rather than hanging off the taskbar.
            const int inset = emebalachat::ui::ScaleDipsToPixels(24, dpi_);
            const int bar_h = emebalachat::ui::ScaleDipsToPixels(8, dpi_);
            RECT anchor = {tr.left + inset, tr.bottom - inset - bar_h,
                           tr.left + inset + emebalachat::ui::ScaleDipsToPixels(2, dpi_),
                           tr.bottom - inset};
            const POINT probe = {static_cast<LONG>(anchor.left), static_cast<LONG>(anchor.top)};
            return PlaceBackTransCard(anchor, /*above=*/true, gap_px,
                                      card_w_phys, card_h_phys, work_area_for(probe));
        }
    }

    POINT cursor = {};
    if (!::GetCursorPos(&cursor)) {
        cursor = POINT{0, 0};
    }
    RECT anchor = {cursor.x, cursor.y, cursor.x + 1, cursor.y + 1};
    return PlaceBackTransCard(anchor, /*above=*/false, gap_px,
                              card_w_phys, card_h_phys, work_area_for(cursor));
}

// ---- v0.10.9 (U-B, defect 3-4): real multi-line measurement ---------------
// The pre-v0.10.9 card never measured the original at all (it reserved a fixed
// 16 DIP single line), so any original longer than ~40 characters was drawn
// below the card and lost. Both panels now measure through DirectWrite at the
// exact painted column width; DWRITE_TEXT_METRICS.height is DPI-independent
// DIP, the same unit the layout code below uses.
//
// Degenerate paths keep a usable card instead of a zero-height panel:
//   * no DWrite factory / no format -> character-count estimate at
//     `fallback_line_h` per wrapped line;
//   * empty text -> zero height (the planner then gives that panel no body).
// `out_line_height_dip` (optional) receives the measured per-line height so
// the wheel step moves REAL lines of this card (one notch = three lines).
float TooltipWindow::MeasureBackTransTextHeight(const std::wstring& text,
                                               IDWriteTextFormat* format,
                                               float column_w,
                                               float fallback_line_h,
                                               float* out_line_height_dip) const {
    if (out_line_height_dip) *out_line_height_dip = fallback_line_h;
    if (text.empty()) return 0.0f;
    if (column_w <= 0.0f) column_w = static_cast<float>(kBackTransCardWidthDip - 28);
    if (dwrite_factory_ && format) {
        IDWriteTextLayout* layout = nullptr;
        if (SUCCEEDED(dwrite_factory_->CreateTextLayout(
                text.c_str(), static_cast<UINT32>(text.size()), format,
                column_w, 100000.0f, &layout)) && layout) {
            DWRITE_TEXT_METRICS m = {};
            const HRESULT hr = layout->GetMetrics(&m);
            layout->Release();
            if (SUCCEEDED(hr) && m.height > 0.0f) {
                if (out_line_height_dip && m.lineCount > 0) {
                    *out_line_height_dip = m.height / static_cast<float>(m.lineCount);
                }
                return m.height;
            }
        }
    }
    int lines = (static_cast<int>(text.size()) + kBackTransCharsPerLine - 1) /
                kBackTransCharsPerLine;
    if (lines < 1) lines = 1;
    return static_cast<float>(lines) * fallback_line_h;
}

// ---- GUI-thread show ------------------------------------------------------
void TooltipWindow::ShowBackTranslation(const std::wstring& original_text,
                                        const std::wstring& back_text,
                                        const std::wstring& source_lang_label,
                                        const std::wstring& target_lang_label,
                                        bool meaning_matched,
                                        HWND target_hwnd, HWND badge_hwnd) {
    if (!hwnd_) return;
    // REQ-R10 thread-affinity guard: any off-thread caller is marshaled.
    if (::GetCurrentThreadId() != gui_thread_id_) {
        ShowBackTranslationThreadSafe(original_text, back_text, source_lang_label,
                                      target_lang_label, meaning_matched,
                                      target_hwnd, badge_hwnd);
        return;
    }

    DIAG_LOG("UI", "tooltip_show kind=backtrans src_len=%zu back_len=%zu matched=%d",
             original_text.size(), back_text.size(), meaning_matched ? 1 : 0);

    // The card owns the window while it is up: the translation card and the
    // REQ-R08 notice are both replaced (one display mode at a time).
    is_message_mode_ = false;
    no_voice_notice_active_ = false;
    ::KillTimer(hwnd_, kTimerMessageAutohide);
    copied_feedback_ = false;
    hovered_btn_ = 0;
    // The translation model is released with the card: the footer buttons and
    // the language dropdowns are not rendered in this mode, and leaving the
    // old body behind would let a WM_DPICHANGED repaint resurrect it.
    source_text_.clear();
    source_lang_code_.clear();
    target_lang_.clear();
    translated_text_.clear();
    message_header_.clear();

    is_backtrans_mode_ = true;
    backtrans_original_ = original_text;
    backtrans_text_ = back_text;
    backtrans_matched_ = meaning_matched;
    backtrans_header_ = FormatBackTransHeader(I18n::Get(StringId::BackTranslateHeader),
                                              source_lang_label, target_lang_label);
    // v0.10.9 (U-B, defect 3-5a): the [원문] panel body is the RAW original;
    // the panel carries its own localized section header, so the pre-v0.10.9
    // inline "↳ 원문: " prefix is not built any more. The never-hide policy
    // lives in BackTransNeedsReferenceLine (always true for a non-empty
    // original, including the exact-round-trip case).
    backtrans_ref_line_ = BackTransNeedsReferenceLine(original_text, back_text)
                              ? original_text
                              : std::wstring();

    // ---- V10F FUP-05: reading direction + locale for the comparison card ---
    // The back-translation and the original are BOTH in the SOURCE language
    // (that is what makes the round trip readable), so the pair's direction is
    // the source language's — resolved from the localized source label through
    // the registry (labels are name_en / native / code, all of which
    // NormalizeLanguageCode resolves). The first-strong heuristic is the
    // documented fallback for a card whose label could not be resolved (e.g. a
    // caller that passed an empty label), matching bidi_utils' per-string
    // contract for unknown-language content.
    // Direction is set BEFORE the measurement below: DWrite lays an RTL
    // paragraph out from the right edge, so a measurement taken under LTR would
    // not be the layout the renderer paints.
    {
        const std::string src_code = NormalizeLanguageCode(ToUtf8(source_lang_label));
        bool backtrans_rtl = IsRtlLanguageCode(src_code);
        if (!backtrans_rtl) {
            backtrans_rtl = GuessBaseDirection(backtrans_text_) == TextDirection::RTL;
        }
        ApplyBackTransBodyFormat(backtrans_body_format_, backtrans_rtl, src_code);
        ApplyBackTransBodyFormat(backtrans_ref_format_, backtrans_rtl, src_code);
        DIAG_LOG("UI", "tooltip backtrans_dir=%s src=%s",
                 backtrans_rtl ? "rtl" : "ltr", src_code.c_str());
    }

    // A fresh card always starts unscrolled (same rule the translation card
    // follows). scrollable_ is NOT reset here: it is assigned unconditionally
    // from the measured panel plan below, which is the whole point of defect
    // 3-1 (the pre-v0.10.9 code pinned it to false at this point and killed
    // every wheel/drag path on this card).
    // A drag still in flight from the PREVIOUS card must not keep the mouse
    // captured by a card that has already been replaced.
    if (dragging_thumb_) {
        ::ReleaseCapture();
    }
    scroll_offset_dip_ = 0.0f;
    content_height_dip_ = 0.0f;
    dragging_thumb_ = false;
    thumb_hover_ = false;
    scrollbar_track_rect_ = {};
    scrollbar_thumb_rect_ = {};
    backtrans_hover_paused_ = false;
    backtrans_close_hover_ = false;
    // V10F FUP-05: every fresh card starts in the SPLIT view with BOTH pane
    // offsets at 0 (the pre-V10F card had exactly one offset, so this is the
    // pre-V10F behavior for an unpinned card).
    backtrans_view_ = BackTransView::Split;
    backtrans_scroll_pane_ = BackTransScrollPane::Back;
    backtrans_ref_offset_dip_ = 0.0f;
    backtrans_ref_track_rect_ = {};
    backtrans_ref_thumb_rect_ = {};
    backtrans_back_label_rect_ = {};
    backtrans_ref_label_rect_ = {};
    backtrans_back_label_hover_ = false;
    backtrans_ref_label_hover_ = false;
    backtrans_pin_rect_ = {};
    backtrans_pin_hover_ = false;
    // V10F L1-B + L3-B: per-card state, same rule as ShowTranslation.
    pinned_.store(false, std::memory_order_relaxed);
    preview_phase_ = ChunkPreviewPhase::Inactive;
    preview_text_.clear();
    preview_index_ = 0;
    preview_total_ = 0;

    // Size: the pure character-count plan is the floor; a real DirectWrite
    // measurement refines both panels' heights and the header row when the
    // factory is alive (same measure-then-lay-out discipline as
    // ShowTranslation, but the card width is FIXED at 360 DIP so the estimate
    // stays honest).
    //
    // 261108_0002 (FIX-WS3B, v0.10.6): the header row is planned BEFORE the
    // height is computed, because a wrapped pill adds a row. The plan is
    // measured here once and stored in backtrans_header_plan_ so Render draws
    // exactly the geometry that was measured (measure/render lockstep by
    // construction).
    const int text_column = kBackTransCardWidthDip - 28;
    const std::wstring pill_text = I18n::Get(backtrans_matched_ ? StringId::BackTranslateMatchExact
                                                               : StringId::BackTranslateMatchCheck);
    float header_w = static_cast<float>(backtrans_header_.size()) * kBackTransHeaderCharDip;
    float pill_w = static_cast<float>(pill_text.size()) * kBackTransPillCharDip + kBackTransPillPadDip;
    if (dwrite_factory_) {
        if (backtrans_header_format_ && !backtrans_header_.empty()) {
            IDWriteTextLayout* hl = nullptr;
            if (SUCCEEDED(dwrite_factory_->CreateTextLayout(
                    backtrans_header_.c_str(), static_cast<UINT32>(backtrans_header_.size()),
                    backtrans_header_format_, 4096.0f, 24.0f, &hl)) && hl) {
                DWRITE_TEXT_METRICS hm = {};
                if (SUCCEEDED(hl->GetMetrics(&hm)) && hm.width > 0.0f) {
                    header_w = hm.width;
                }
                hl->Release();
            }
        }
        if (button_format_ && !pill_text.empty()) {
            IDWriteTextLayout* pl = nullptr;
            if (SUCCEEDED(dwrite_factory_->CreateTextLayout(
                    pill_text.c_str(), static_cast<UINT32>(pill_text.size()),
                    button_format_, 4096.0f, 24.0f, &pl)) && pl) {
                DWRITE_TEXT_METRICS pm = {};
                if (SUCCEEDED(pl->GetMetrics(&pm)) && pm.width > 0.0f) {
                    pill_w = pm.width + kBackTransPillPadDip;
                }
                pl->Release();
            }
        }
    }
    backtrans_header_plan_ =
        // V10F L1-B: the pin box owns the slot between the ✕ and the verdict
        // pill, so the row planner is fed the card width MINUS that slot - the
        // pill and the header text therefore never reach the pin by
        // construction (the same "give the pure planner a narrower row"
        // technique PlanHeaderActionCluster uses on the translation card).
        PlanBackTransHeaderRowDip(header_w, pill_w,
                                  static_cast<float>(kBackTransCardWidthDip - kHeaderPinSlotDip));
    const int header_extra_h = static_cast<int>(backtrans_header_plan_.pill_row_h);
    backtrans_header_extra_h_ = header_extra_h; // V10F FUP-05: kept for the view toggle

    // ---- v0.10.9 (U-B, defects 3-3/3-4): MEASURE BOTH panels ----
    // The original used to be a fixed 6+16 DIP one-line box appended to the
    // body height; anything past ~40 characters was drawn below the card and
    // lost. Both texts now go through a real multi-line DirectWrite
    // measurement at the exact painted column width, and the two-panel planner
    // turns those heights into clipped viewports.
    float measured_line_height_dip = static_cast<float>(kBackTransLineHeightDip);
    auto measure_panels = [&](int column_w) -> BackTransPanelPlan {
        float back_line_h = static_cast<float>(kBackTransLineHeightDip);
        const int back_h = static_cast<int>(
            std::ceil(MeasureBackTransTextHeight(
                backtrans_text_, backtrans_body_format_, static_cast<float>(column_w),
                static_cast<float>(kBackTransLineHeightDip), &back_line_h)));
        measured_line_height_dip = back_line_h;
        const int ref_h = static_cast<int>(
            std::ceil(MeasureBackTransTextHeight(
                backtrans_ref_line_, backtrans_ref_format_, static_cast<float>(column_w),
                static_cast<float>(kBackTransRefLineDip))));
        // V10F FUP-05: remember the two measurements so the view toggle can
        // re-plan without touching DirectWrite again (a re-measure at the
        // toggled column width would be a second measurement pass per click).
        backtrans_measured_back_h_ = back_h;
        backtrans_measured_ref_h_ = ref_h;
        return PlanBackTransViewPanels(backtrans_view_, back_h, ref_h, header_extra_h);
    };
    BackTransPanelPlan panel_plan = measure_panels(text_column);
    if (panel_plan.scrollable) {
        // The shared scrollbar takes kBackTransScrollbarGutterDip off the text
        // column; a narrower wrap can only grow both content extents, so
        // re-measure once and adopt the result (same gutter-remeasure rule the
        // REQ-002 body path uses).
        panel_plan = measure_panels(text_column - kBackTransScrollbarGutterDip);
    }
    backtrans_panel_plan_ = panel_plan;
    current_width_ = kBackTransCardWidthDip;
    current_height_ = panel_plan.card_height_dip;

    // ---- v0.10.9 (U-B, defects 3-1/3-3): the SHARED scroll state ----
    // One offset drives both clipped panels. content_height_dip_ is expressed
    // against the taller viewport so the EXISTING ClampScrollOffset /
    // ScrollbarThumb* math (wheel, thumb drag, thumb geometry) needs no
    // special case: content - viewport == panel_plan.max_scroll_offset.
    scrollable_ = panel_plan.scrollable;
    content_height_dip_ = static_cast<float>(panel_plan.max_scroll_offset +
                                             panel_plan.shared_viewport_h);
    // Wheel step unit: the measured 13pt body line height, so one notch moves
    // three real lines of THIS card (fallback: the documented 20 DIP).
    line_height_dip_ = measured_line_height_dip > 0.0f
                           ? measured_line_height_dip
                           : static_cast<float>(kBackTransLineHeightDip);
    // v0.10.9 (U-B, defect 3-5c): the adaptive auto-hide window for THIS card.
    backtrans_autohide_ms_ = BackTransAutohideMsForContent(
        backtrans_text_.size() + backtrans_original_.size());

    // DPI + physical buffer, then the anchor (which needs the physical card
    // extents to clamp correctly).
    //
    // V10F FUP-08: the DPI comes from the ANCHOR WINDOW's monitor, not from the
    // cursor. The card is placed next to the caret/badge/target window, which
    // routinely sits on a DIFFERENT monitor than the pointer (the reported
    // symptom: a back-translation card over a secondary 150% display rendered
    // at the primary's 100% scale). AnchorMonitorDpi falls back to the
    // pre-V10F cursor probe only when neither window handle is usable.
    dpi_ = AnchorMonitorDpi(target_hwnd, badge_hwnd);
    ReallocateBuffer(PhysW(), PhysH());

    const POINT origin = ResolveBackTranslationAnchor(target_hwnd, badge_hwnd, PhysW(), PhysH());
    ::SetWindowPos(hwnd_, HWND_TOPMOST, origin.x, origin.y, PhysW(), PhysH(),
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    // 261111_0001 U1 (v0.10.7, D1a) + fix-list round 2 G-06: publishing this
    // fresh card is ONE locked transition (visible, no teardown owed, NEW card
    // generation), so a dismissal still queued for a previous card - or one
    // beginning concurrently on another thread - can never clear this card's
    // visibility or tear it down when its message is finally drained.
    card_visibility_.PublishShow();

    Render();
    UpdateLayered();

    // Auto-dismiss: the ADAPTIVE window measured for this card (4500..12000 ms,
    // defect 3-5c), paused on hover - PlanBackTransTimer owns the decision and
    // the WndProc only dispatches events into it.
    ApplyBackTransTimerPlan(BackTransTimerEvent::Shown);
}

void TooltipWindow::ShowBackTranslationThreadSafe(const std::wstring& original_text,
                                                  const std::wstring& back_text,
                                                  const std::wstring& source_lang_label,
                                                  const std::wstring& target_lang_label,
                                                  bool meaning_matched,
                                                  HWND target_hwnd, HWND badge_hwnd) {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() == gui_thread_id_) {
        ShowBackTranslation(original_text, back_text, source_lang_label, target_lang_label,
                            meaning_matched, target_hwnd, badge_hwnd);
        return;
    }
    // SEC-ADJ: by-value transport through g_tt_backtrans_queue; the posted
    // message is a pure (0, 0) wake-up. A failed post leaves the entry queued
    // (drain-safe) - same contract as the other three seams.
    BackTranslationPayload payload;
    payload.original_text = original_text;
    payload.back_text = back_text;
    payload.source_lang_label = source_lang_label;
    payload.target_lang_label = target_lang_label;
    payload.meaning_matched = meaning_matched;
    payload.target_hwnd = target_hwnd;
    payload.badge_hwnd = badge_hwnd;
    {
        std::lock_guard<std::mutex> lk(g_tt_backtrans_mu);
        // 261108_0002 (FIX-R1, v0.10.6): LATEST-WINS bound. A deque that grows
        // per request retains full original + back-translated strings whenever
        // the posted wake-up is dropped (queue full / window gone) or the GUI
        // pump stalls, and none of those retention paths is user-reversible.
        // The card can only ever show one card, so an older pending payload has
        // no value: clear then push keeps exactly the newest one, matching the
        // SingleSlotWorker latest-wins producer. The WndProc drain loop stays
        // correct unchanged - it applies whatever entries exist (0 or 1 now)
        // and therefore coalesces to the newest by construction.
        g_tt_backtrans_queue.clear();
        g_tt_backtrans_queue.push_back(std::move(payload));
    }
    if (::PostMessageW(hwnd_, kShowBackTranslationMessage, 0, 0) == FALSE) {
        DIAG_F("TT/Marshal/004: PostMessage kShowBackTranslation failed (GLE %lu); payload left queued\n",
               ::GetLastError());
    }
}

// R6 Phase 1 (B3, plan §2.4 step 6/7 companion): view-only sync of the target
// language label after ANY surface mutated the persisted pair (tray menu,
// swap, Ctrl+F9 cycle, config reload). Best-effort by design: hidden or
// message-mode notices have no language button to update, so those cases
// no-op. When visible in translation mode, the button label (Render reads
// target_lang_) is refreshed WITHOUT re-translating - the body stays as shown
// until the next request, per the plan's "view sync, not content churn".
// Cross-thread calls marshal through kRefreshTargetLangMessage (REQ-R10):
// SEC-ADJ — payload travels BY VALUE through g_tt_targetlang_queue; the posted
// message is a pure (0, 0) wake-up, same contract as the Show*ThreadSafe seams
// above. Failed post leaves the entry queued (drain-safe).
void TooltipWindow::RefreshTargetLanguageFromConfig(std::string_view new_target) {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() != gui_thread_id_) {
        TargetLangPayload payload;
        payload.target_lang = new_target;
        {
            std::lock_guard<std::mutex> lk(g_tt_targetlang_mu);
            g_tt_targetlang_queue.push_back(std::move(payload));
        }
        if (::PostMessageW(hwnd_, kRefreshTargetLangMessage, 0, 0) == FALSE) {
            DIAG_F("TT/Marshal/003: PostMessage kRefreshTargetLang failed (GLE %lu); payload left queued\n",
                   ::GetLastError());
        }
        return;
    }
    if (!card_visibility_.IsVisible() || is_message_mode_) {
        return;
    }
    if (target_lang_ == new_target) {
        return; // already in sync (tooltip-initiated change re-shows below)
    }
    target_lang_ = new_target;
    Render();
    UpdateLayered();
}

void TooltipWindow::RefreshTheme() {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() != gui_thread_id_) {
        return;
    }
    if (card_visibility_.IsVisible()) {
        Render();
        UpdateLayered();
    }
}

void TooltipWindow::Dismiss() {
    if (!hwnd_) return;
    // V10F L1-B: a PINNED card ignores the outside-click dismissal (the mouse
    // hook's WindowFromPoint path in main.cpp calls this) and the autohide
    // timers, which is the whole point of the pin. The card's own ✕ goes
    // through DismissExplicit() below and is NOT gated, so the pin can never
    // trap the user. `pinned_` is atomic: this runs on the hook threads too.
    if (pinned_.load(std::memory_order_relaxed)) {
        DIAG_LOG("UI", "tooltip_dismiss ignored reason=pinned");
        return;
    }
    // 261108_0002 (WS3, v0.10.6): a dismissal also discards back-translation
    // payloads still queued for this window. Without it, a show enqueued a
    // moment before the user pressed ✕/ESC (or before the autohide fired)
    // would resurrect the card on the next wake-up - the "I closed it and it
    // came back" race. Only this unit's queue is touched; the translation /
    // message / target-lang queues keep their existing semantics.
    {
        std::lock_guard<std::mutex> lk(g_tt_backtrans_mu);
        g_tt_backtrans_queue.clear();
    }
    // V10F L3-B: same rule for the preview queue - a Begin/Append posted a
    // moment before the dismissal must not resurrect the card it closed.
    {
        std::lock_guard<std::mutex> lk(g_tt_preview_mu);
        g_tt_preview_queue.clear();
    }
    // REQ-R10 companion: the ESC (keyboard hook) and outside-click (mouse hook)
    // dismissal paths call Dismiss() off the GUI thread. ShowWindow/SAPI on
    // another thread's window can block until that pump is free (e.g. mid
    // translation), which would stall LowLevelHooksTimeout. Marshal instead.
    // WndProc re-enters on the GUI thread, so this guard never recurses.
    //
    // 261111_0001 U1 (v0.10.7, audit D1a/D1b) + fix-list round 2 G-06:
    // visibility is cleared BEFORE this call returns on the hook/worker
    // thread, so a fast re-drag can never observe a stale IsVisible() gate
    // (the 40-100 ms dead-gesture drop that suppressed drag-to-translate
    // while a card was parked). The posted message still performs the
    // GUI-side teardown; the dismissal is stamped with the generation of the
    // card it began on, so the kDismissMessage handler runs TeardownCard()
    // exactly once for THAT card - a fresh card published meanwhile has a
    // newer generation and the stale message is dropped instead of tearing
    // it down. Clearing visibility, stamping the owed teardown and capturing
    // the generation all happen inside ONE TooltipCardVisibility section, so
    // a concurrent Show* can never observe (or produce) a half-applied
    // dismissal.
    if (::GetCurrentThreadId() != gui_thread_id_) {
        DIAG_LOG("UI", "tooltip_dismiss requested off-thread (marshaled)");
        uint64_t dismiss_generation = TooltipCardVisibility::kNoGeneration;
        if (!card_visibility_.TryBeginDismiss(dismiss_generation)) {
            return; // nothing visible: same early-return as the on-thread path
        }
        // R1-G (v0.10.7, 261111_0001 fix-list round 1): the post used to be
        // UNCHECKED. A failed PostMessageW (target queue full / pump stalled)
        // stranded a SHOWN window with visibility already false and no
        // teardown owed - a ghost card no later dismissal path would retry.
        // Retry a bounded number of times; on persistent failure restore the
        // coherent pre-post state (shown, nothing owed) so the next ESC /
        // outside-click / autohide path re-enters Dismiss and retries the
        // post. Never leave shown-window + hidden-model + no pending teardown.
        bool posted = false;
        for (int attempt = 0; attempt < 3 && !posted; ++attempt) {
            if (::PostMessageW(hwnd_, kDismissMessage,
                               static_cast<WPARAM>(dismiss_generation), 0) == TRUE) {
                posted = true;
                break;
            }
            DIAG_LOG("UI", "tooltip_dismiss post failed (attempt %d, GLE %lu); retrying",
                     attempt, static_cast<unsigned long>(::GetLastError()));
            // G-12 (round 2): YIELD, never sleep. This runs on the keyboard/
            // mouse hook thread (ESC / outside click), where any Sleep eats the
            // LowLevelHooksTimeout budget (mouse_hook.cpp:213) and can get the
            // hook silently dropped by the OS. SwitchToThread hands the CPU to
            // another ready thread and returns at once when none is ready.
            ::SwitchToThread();
        }
        if (!posted) {
            // Restore ONLY while this dismissal still owns the state; a fresh
            // card published during the retries keeps its own visibility.
            const bool restored = card_visibility_.RestoreShown(dismiss_generation);
            DIAG_LOG("UI", "tooltip_dismiss post failed after retries; %s",
                     restored ? "state restored (shown, no teardown owed) - a later path will retry"
                              : "a newer card owns the state; nothing restored");
        }
        return;
    }
    if (!card_visibility_.IsVisible()) return;
    TeardownCard();
}

// V10F L1-B: the ✕ buttons' dismissal. Structurally the on-thread Dismiss()
// tail (same visibility proof, same TeardownCard) with ONE difference: the pin
// does not apply, because clicking the card's own close affordance is an
// explicit, deliberate dismissal. A pinned card therefore always has a way out
// that needs no second click on the pin.
void TooltipWindow::DismissExplicit() {
    if (!hwnd_) return;
    if (::GetCurrentThreadId() != gui_thread_id_) {
        // Defensive: the ✕ hit test runs on the GUI thread, so this branch is
        // unreachable today. An off-thread caller that wants a pin-proof close
        // must marshal its own GUI-thread work (the generic Dismiss() is
        // swallowed by the pin by design); log and drop instead of tearing a
        // card down from a thread that does not own the D2D target.
        DIAG_LOG("UI", "tooltip_dismiss_explicit ignored off-thread");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_tt_backtrans_mu);
        g_tt_backtrans_queue.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_tt_preview_mu);
        g_tt_preview_queue.clear();
    }
    if (!card_visibility_.IsVisible()) return;
    pinned_.store(false, std::memory_order_relaxed);
    TeardownCard();
}

void TooltipWindow::TeardownCard() {
    // 261111_0001 U1 (v0.10.7, audit D1a/D1b): GUI-side teardown half of
    // Dismiss(), shared by the on-thread Dismiss() path and the kDismissMessage
    // handler. Idempotent by construction - every step below is a no-op when
    // run a second time (KillTimer on dead timers, clearing empty buffers,
    // SW_HIDE on a hidden window) - and drops the owed stamp first so a
    // second invocation can never double-teardown.
    card_visibility_.ClearDismissalOwed();
    DIAG_LOG("UI", "tooltip_dismiss executed gen=%llu",
             static_cast<unsigned long long>(latest_request_gen_.load(std::memory_order_relaxed)));
    StopTTS();
    ::KillTimer(hwnd_, kTimerMessageAutohide);
    ::KillTimer(hwnd_, kTimerBackTranslateAutohide);
    card_visibility_.MarkHidden();
    hovered_btn_ = 0;
    copied_feedback_ = false;
    is_message_mode_ = false;
    no_voice_notice_active_ = false; // P4 Batch-3 (D7): notice released on every dismissal path
    // 261108_0002 (WS3, v0.10.6): the back-translation card is released by the
    // same dismissal (✕ / ESC / outside click / autohide timer).
    is_backtrans_mode_ = false;
    backtrans_hover_paused_ = false;
    backtrans_close_hover_ = false;
    backtrans_close_rect_ = {};
    backtrans_original_.clear();
    backtrans_text_.clear();
    backtrans_header_.clear();
    backtrans_ref_line_.clear();
    backtrans_matched_ = false;
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
    // v0.10.9 (U-B): + the two-panel plan, the adaptive delay and the shared
    // thumb rect (a stale thumb must not answer the next card's hit test), and
    // any capture held by an in-flight thumb drag.
    if (dragging_thumb_) {
        ::ReleaseCapture();
    }
    scroll_offset_dip_ = 0.0f;
    scrollable_ = false;
    content_height_dip_ = 0.0f;
    dragging_thumb_ = false;
    thumb_hover_ = false;
    scrollbar_track_rect_ = {};
    scrollbar_thumb_rect_ = {};
    backtrans_panel_plan_ = BackTransPanelPlan{};
    backtrans_autohide_ms_ = kBackTransAutohideMs;
    // V10F FUP-05: the pane/view state dies with the card (a stale solo view or
    // a stale pane offset must not leak into the next card).
    backtrans_view_ = BackTransView::Split;
    backtrans_scroll_pane_ = BackTransScrollPane::Back;
    backtrans_ref_offset_dip_ = 0.0f;
    backtrans_ref_track_rect_ = {};
    backtrans_ref_thumb_rect_ = {};
    backtrans_back_label_rect_ = {};
    backtrans_ref_label_rect_ = {};
    backtrans_back_label_hover_ = false;
    backtrans_ref_label_hover_ = false;
    backtrans_pin_rect_ = {};
    backtrans_pin_hover_ = false;
    backtrans_measured_back_h_ = 0;
    backtrans_measured_ref_h_ = 0;
    backtrans_header_extra_h_ = 0;
    // V10F L1-B + L3-B + FUP-08: per-card state released with the card. The
    // pin clears so the NEXT card starts unpinned; the cached body layout is
    // content-shaped for the text that is being dropped, so it goes too.
    pinned_.store(false, std::memory_order_relaxed);
    preview_phase_ = ChunkPreviewPhase::Inactive;
    preview_index_ = 0;
    preview_total_ = 0;
    preview_text_.clear();
    preview_progress_track_rect_ = {};
    preview_progress_fill_rect_ = {};
    ReleaseBodyTextLayout();
    ::ShowWindow(hwnd_, SW_HIDE);
}

// V10F FUP-02: the picker-session source snapshot (see the hpp contract).
// Latch captures the RAW source_lang_code_ member ("" exactly when the card is
// showing AUTO), byte-identical to what GetSourceLangCode() would return for
// the live state, so the retranslate carry semantics are unchanged.
void TooltipWindow::LatchLanguagePickerSourceSnapshot() {
    lang_picker_snapshot_valid_ = true;
    lang_picker_snapshot_text_ = source_text_;
    lang_picker_snapshot_code_ = source_lang_code_;
}

TooltipWindow::LanguagePickerSourceSnapshot
TooltipWindow::GetLanguagePickerSourceSnapshot() const {
    LanguagePickerSourceSnapshot out;
    out.valid = lang_picker_snapshot_valid_;
    if (out.valid) {
        out.source_text = lang_picker_snapshot_text_;
        out.source_lang_code = lang_picker_snapshot_code_;
    }
    return out;
}

void TooltipWindow::ClearLanguagePickerSourceSnapshot() {
    lang_picker_snapshot_valid_ = false;
    lang_picker_snapshot_text_.clear();
    lang_picker_snapshot_code_.clear();
}

void TooltipWindow::ScrollByDipWheel(int wheel_delta) {
    if (!hwnd_) return;
    // Message mode (REQ-R08 compact notice) never scrolls; neither does a body
    // that fits its viewport (short text keeps today's exact look, REQ-003).
    // v0.10.9 (U-B, defect 3-1): the back-translation card no longer hardcodes
    // scrollable_ = false - ShowBackTranslation computes it from the measured
    // content heights exactly like the normal mode, so this gate now lets its
    // wheel through.
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
    // v0.10.9 (U-B): the clamp basis is the ACTIVE mode's viewport.
    // V10F FUP-05: on the comparison card the wheel addresses the PANE UNDER
    // THE POINTER (each panel now has its own offset, so "scroll both together"
    // is gone). The pointer read is a GUI-thread Win32 call; when it fails the
    // pane resolver's default (the view's soloing pane, else the
    // back-translation) applies.
    if (is_backtrans_mode_) {
        float y_dip = -1.0f;
        POINT cursor = {};
        if (::GetCursorPos(&cursor)) {
            ::ScreenToClient(hwnd_, &cursor);
            y_dip = static_cast<float>(emebalachat::ui::ScalePixelsToDips(cursor.y, dpi_));
        }
        backtrans_scroll_pane_ = TargetBackTransPaneForPoint(y_dip);
    }
    const float viewport_h = ActiveScrollViewportDip();
    const float content_h = is_backtrans_mode_
                                ? BackTransPaneContentDip(backtrans_scroll_pane_)
                                : content_height_dip_;
    float& offset = is_backtrans_mode_ ? BackTransPaneOffset(backtrans_scroll_pane_)
                                       : scroll_offset_dip_;
    const float next = ClampScrollOffset(offset + step, content_h, viewport_h);
    if (next == offset) return; // clamped at an edge: no repaint jitter
    offset = next;
    // v0.10.9 (U-B, defect 3-5c): a scroll is a "still reading" signal.
    RearmBackTransAutohideForScroll();
    Render();
    UpdateLayered();
}

// ---- v0.10.9 (U-B): shared scrollbar thumb drag ----
// The REQ-002 drag math, factored out so the back-translation card reaches the
// SAME tested code path instead of a second copy (its WM_LBUTTONDOWN branch
// used to swallow every press, which is why drag-scroll never worked there).
bool TooltipWindow::BeginScrollThumbDrag(float x_dip, float y_dip) {
    if (!scrollable_) return false;
    // V10F FUP-05: the comparison card draws ONE THUMB PER overflowing pane
    // (each pane has its own offset), so the press resolves which pane's thumb
    // it landed on. Every other mode keeps the single REQ-002 thumb.
    bool ref_pane = false;
    if (is_backtrans_mode_ && IsPointInRect(backtrans_ref_thumb_rect_, x_dip, y_dip)) {
        ref_pane = true;
    } else if (!IsPointInRect(scrollbar_thumb_rect_, x_dip, y_dip)) {
        return false;
    }
    dragging_thumb_ = true;
    thumb_hover_ = true;
    if (is_backtrans_mode_) {
        backtrans_scroll_pane_ =
            ref_pane ? BackTransScrollPane::Ref : BackTransScrollPane::Back;
    }
    drag_start_offset_dip_ = is_backtrans_mode_ && ref_pane ? backtrans_ref_offset_dip_
                                                           : scroll_offset_dip_;
    drag_start_y_dip_ = y_dip;
    ::SetCapture(hwnd_);
    Render();
    UpdateLayered();
    return true;
}

bool TooltipWindow::UpdateScrollThumbDrag(float y_dip) {
    if (!dragging_thumb_) return false;
    // V10F FUP-05: the drag moves the pane whose thumb was grabbed. The rects,
    // the extents and the offset slot are therefore all pane-scoped; the
    // non-backtrans card keeps the pre-V10F body math unchanged.
    const bool ref_pane = is_backtrans_mode_ &&
                          backtrans_scroll_pane_ == BackTransScrollPane::Ref;
    const D2D1_RECT_F& track = ref_pane ? backtrans_ref_track_rect_ : scrollbar_track_rect_;
    const D2D1_RECT_F& thumb = ref_pane ? backtrans_ref_thumb_rect_ : scrollbar_thumb_rect_;
    const float track_h = track.bottom - track.top;
    const float thumb_h = thumb.bottom - thumb.top;
    const float viewport_h = ActiveScrollViewportDip();
    const float content_h = is_backtrans_mode_
                                ? BackTransPaneContentDip(backtrans_scroll_pane_)
                                : content_height_dip_;
    const float range = track_h - thumb_h;
    const float scroll_range = content_h - viewport_h;
    if (range <= 0.0f || scroll_range <= 0.0f) return false;
    float& offset = is_backtrans_mode_ ? BackTransPaneOffset(backtrans_scroll_pane_)
                                       : scroll_offset_dip_;
    const float dy = y_dip - drag_start_y_dip_;
    const float next = ClampScrollOffset(drag_start_offset_dip_ + dy * scroll_range / range,
                                         content_h, viewport_h);
    if (next == offset) return false;
    offset = next;
    RearmBackTransAutohideForScroll();
    return true;
}

void TooltipWindow::EndScrollThumbDrag() {
    dragging_thumb_ = false;
    ::ReleaseCapture();
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
    // FIX-B1 (R2-2/R2-4, 261029_0001): this is a USER-intent write - the user
    // pressed the card's copy button. It must NOT be attributed to a translate
    // task that happens to be in flight (the pre-fix SetClipboardText stamped
    // the shared process slot, so a concurrent task's restore saw the user's
    // copy as "our own write" and replayed the pre-task snapshot over it).
    // SetClipboardTextAsUserCopy writes the identical clip (text + the three
    // Clipboard-History exclusion formats) and declares the USER as the owner.
    SetClipboardTextAsUserCopy(translated_text_);
    copied_feedback_ = true;
    if (hwnd_) {
        ::SetTimer(hwnd_, kTimerCopiedFeedback, 1500, nullptr);
    }
    Render();
    UpdateLayered();
}

// 261108_0002 (WS3, v0.10.6): the floating back-translation card body.
// Called from Render() after the shared elevated-card chrome, with the D2D
// target already begun. Layout (all DIP, card width 360), v0.10.9 (U-B) the
// report's §4 Option 2 two-panel comparison card:
//   row 1  y  9..33   header title (12pt SemiBold) + match pill + ✕
//   [역번역]  label band (StringId::BackTransSectionBack, 13pt SemiBold)
//            body viewport  AAAAA' (13pt SemiBold, text_primary)  <- CLIPPED
//   [원문]    sub-panel (subtle contrast fill + border): label band
//            (StringId::BackTransSectionOriginal) + body viewport
//            AAAAA (11.5pt, text_secondary)                      <- CLIPPED
//   right edge: ONE scrollbar thumb for the ONE shared scroll offset
// The pill is the caller's verdict: state_ready green for "✓ 원문 일치",
// accent_primary for "⚡ 표현 확인" (the theme has no `status_active` token -
// state_ready is its equivalent semantic slot).
//
// 261108_0002 (FIX-WS3B, v0.10.6): the header row now follows
// backtrans_header_plan_ (planned in ShowBackTranslation from real DirectWrite
// measurements). In long-string locales the pill drops to its OWN row below
// the header instead of overlapping it; the ✕ stays top-right on the header
// row, and the body starts below whichever rows are in use.
//
// v0.10.9 (U-B, defects 3-3/3-4/3-1): the report's "역번역 글씨가 하단 원문
// 위를 덮어씀" defect is gone by construction - every panel draws inside a
// PushAxisAlignedClip window taken from the SAME plan the show path measured,
// so a long back-translation can only scroll inside its own viewport (it can
// neither reach the [원문] panel nor run past the card edge). The original is
// no longer a fixed 16 DIP line: it has its own measured, scrollable viewport.
void TooltipWindow::RenderBackTranslation() {
    if (!dc_render_target_ || !scratch_brush_) return;
    const auto& theme = ui::GetActiveTheme();
    const float w = static_cast<float>(current_width_);
    const BackTransHeaderPlan& plan = backtrans_header_plan_;
    const BackTransPanelPlan& panels = backtrans_panel_plan_;
    const float h = static_cast<float>(current_height_);

    // ---- header row: title + (maybe) pill + pin + close ----
    const float row_top = kBackTransRowTopDip;
    const float row_bottom = row_top + kBackTransRowHeightDip;
    close_btn_rect_ = D2D1::RectF(w - kBackTransCloseRightDip - kBackTransCloseW, row_top,
                                  w - kBackTransCloseRightDip, row_bottom);
    backtrans_close_rect_ = close_btn_rect_;
    // V10F L1-B: the pin toggle. One 20 DIP box between the ✕ and the verdict
    // pill; the header-row planner was fed the card width MINUS
    // kHeaderPinSlotDip (ShowBackTranslation), so the pill's box can never
    // reach it. Same right-anchoring rule as the translation card's cluster.
    backtrans_pin_rect_ = D2D1::RectF(
        close_btn_rect_.left - kHeaderCloseGapDip - kHeaderActionIconDip, row_top,
        close_btn_rect_.left - kHeaderCloseGapDip, row_bottom);

    const std::wstring pill_text = I18n::Get(backtrans_matched_ ? StringId::BackTranslateMatchExact
                                                               : StringId::BackTranslateMatchCheck);
    const float pill_w = plan.pill_width_dip;
    // Single row: the pill sits immediately left of the pin box. Wrapped: it
    // starts at the card's left padding, under the header.
    // V10F L1-B: the single-row basis is the planner's EFFECTIVE close edge
    // (card width minus the pin slot), so the pill stops short of the pin
    // exactly where PlanBackTransHeaderRowDip assumed it would.
    const float row_close_left =
        w - static_cast<float>(kHeaderPinSlotDip) - kBackTransCloseRightDip - kBackTransCloseW;
    const float pill_left = (plan.layout == BackTransHeaderLayout::SingleRow)
                                ? (row_close_left - kBackTransCloseGapDip - pill_w)
                                : kBackTransPadDip;
    const float pill_top = (plan.layout == BackTransHeaderLayout::SingleRow)
                               ? row_top
                               : row_bottom + kBackTransPillRowGapDip;
    const D2D1_RECT_F pill_rect = D2D1::RectF(pill_left, pill_top, pill_left + pill_w,
                                              pill_top + kBackTransRowHeightDip);
    const D2D1_ROUNDED_RECT pill_rr =
        D2D1::RoundedRect(pill_rect, theme.control_radius, theme.control_radius);
    const UINT32 pill_color = backtrans_matched_ ? theme.state_ready : theme.accent_primary;

    scratch_brush_->SetColor(ui::ColorFromHex(pill_color, 0.16f));
    dc_render_target_->FillRoundedRectangle(pill_rr, scratch_brush_);
    scratch_brush_->SetColor(ui::ColorFromHex(pill_color, 0.55f));
    dc_render_target_->DrawRoundedRectangle(pill_rr, scratch_brush_, 1.0f);
    if (button_format_) {
        scratch_brush_->SetColor(ui::ColorFromHex(pill_color, 1.0f));
        dc_render_target_->DrawText(pill_text.c_str(), static_cast<UINT32>(pill_text.size()),
                                    button_format_, pill_rect, scratch_brush_);
    }

    // V10F L1-B: the pin toggle's glyph. Accent while pinned (the card will
    // ignore outside clicks and autohide), muted otherwise; the hover read is
    // the usual text_primary lift. A 20 DIP box cannot hold the localized
    // "고정 해제" / "Unpin" label in any shipped locale, so this card shows the
    // glyph and encodes the state in the color; the translation card's 480/620
    // DIP tiers DO draw the label (PlanHeaderActionCluster decides).
    if (header_format_) {
        scratch_brush_->SetColor(
            backtrans_pin_hover_
                ? ui::ColorFromHex(theme.text_primary, 1.0f)
                : (pinned_.load(std::memory_order_relaxed)
                       ? ui::ColorFromHex(theme.accent_primary, 1.0f)
                       : ui::ColorFromHex(theme.text_muted, 0.85f)));
        const std::wstring pin_icon = L"\xD83D\xDCCC"; // U+1F4CC pushpin
        dc_render_target_->DrawText(pin_icon.c_str(), static_cast<UINT32>(pin_icon.size()),
                                    header_format_, backtrans_pin_rect_, scratch_brush_);
    }

    // Title: bounded by the plan's header budget, which already excludes the
    // pill and the ✕. When the plan says the header cannot fit even alone, the
    // box simply ends at that budget and DirectWrite trims with an ellipsis
    // (NO_WRAP + a bounded rect) - the documented last-resort branch.
    if (backtrans_header_format_) {
        const D2D1_RECT_F title_rect = D2D1::RectF(
            kBackTransPadDip, row_top, kBackTransPadDip + plan.header_width_dip, row_bottom);
        scratch_brush_->SetColor(ui::ColorFromHex(theme.text_primary, 1.0f));
        dc_render_target_->DrawText(backtrans_header_.c_str(),
                                    static_cast<UINT32>(backtrans_header_.size()),
                                    backtrans_header_format_, title_rect, scratch_brush_);
    }

    // Close ✕ (hover turns it danger-red, matching the translation card).
    if (header_format_) {
        scratch_brush_->SetColor(backtrans_close_hover_
                                     ? ui::ColorFromHex(theme.state_danger, 1.0f)
                                     : ui::ColorFromHex(theme.text_muted, 0.85f));
        dc_render_target_->DrawText(L"✕", 1, header_format_, close_btn_rect_, scratch_brush_);
    }

    // Header divider: below whichever rows the plan used, so a wrapped pill
    // never straddles the rule.
    const float divider_y = row_bottom + plan.pill_row_h + 4.0f;
    scratch_brush_->SetColor(ui::ColorFromHex(theme.border_subtle, theme.border_subtle_alpha));
    dc_render_target_->DrawLine(D2D1::Point2F(kBackTransPadDip, divider_y),
                                D2D1::Point2F(w - kBackTransPadDip, divider_y),
                                scratch_brush_, 1.0f);

    // ---- [역번역] section: label band + clipped body viewport ----
    // The label comes from the i18n table (contract C5). NOTE (fix round 1,
    // F-5): BackTransSectionBack carries NO brackets — the strings are
    // "역번역" / "Back-translation"; the bracketed forms elsewhere in this
    // file's comments are shorthand for "the back-translation section", not
    // text the C5 table contains. The body is drawn into a rect as tall as the
    // MEASURED content and then cut by the clip, so DirectWrite always lays
    // out every line (no bottom truncation) while the viewport decides what is
    // visible. V10F FUP-05: each panel clamps ITS OWN offset (the shared one is
    // gone), and the label band doubles as the split/solo view toggle.
    const std::wstring back_label = I18n::Get(StringId::BackTransSectionBack);
    const std::wstring ref_label = I18n::Get(StringId::BackTransSectionOriginal);
    const float scrollbar_gutter =
        scrollable_ ? static_cast<float>(kBackTransScrollbarGutterDip) : 0.0f;
    const float col_left = kBackTransPadDip;
    const float col_right = w - kBackTransPadDip - scrollbar_gutter;
    const float label_h = static_cast<float>(kBackTransSectionLabelDip);

    // The two toggle hit zones are rebuilt every frame from the plan, so the
    // WndProc hit test and the painted band can never disagree.
    backtrans_back_label_rect_ = D2D1::RectF(
        col_left, static_cast<float>(panels.back_label_top),
        col_right, static_cast<float>(panels.back_label_top) + label_h);
    backtrans_ref_label_rect_ = D2D1::RectF(
        col_left, static_cast<float>(panels.ref_label_top),
        col_right, static_cast<float>(panels.ref_label_top) + label_h);

    if (backtrans_section_format_ && !back_label.empty()) {
        const bool collapsed =
            panels.back_view_h <= 0 && !backtrans_text_.empty(); // solo [원문] view
        scratch_brush_->SetColor(
            collapsed
                ? ui::ColorFromHex(backtrans_back_label_hover_ ? theme.text_primary
                                                               : theme.text_muted,
                                   1.0f)
                : ui::ColorFromHex(theme.accent_primary, 0.95f));
        dc_render_target_->DrawText(back_label.c_str(), static_cast<UINT32>(back_label.size()),
                                    backtrans_section_format_, backtrans_back_label_rect_,
                                    scratch_brush_);
    }

    if (backtrans_body_format_ && !backtrans_text_.empty() && panels.back_view_h > 0) {
        const float view_top = static_cast<float>(panels.back_view_top);
        const float view_bottom = view_top + static_cast<float>(panels.back_view_h);
        const float draw_off = ClampScrollOffset(scroll_offset_dip_,
                                                 static_cast<float>(panels.back_content_h),
                                                 static_cast<float>(panels.back_view_h));
        const D2D1_RECT_F clip_rect =
            D2D1::RectF(col_left, view_top, col_right, view_bottom);
        dc_render_target_->PushAxisAlignedClip(clip_rect, D2D1_ANTIALIAS_MODE_ALIASED);
        scratch_brush_->SetColor(ui::ColorFromHex(theme.text_primary, 1.0f));
        // Full-content height + one line of slack: the clip, not the layout
        // rect, is what limits the visible rows.
        const D2D1_RECT_F body_rect = D2D1::RectF(
            col_left, view_top - draw_off, col_right,
            view_top - draw_off + static_cast<float>(panels.back_content_h) + label_h);
        dc_render_target_->DrawText(backtrans_text_.c_str(),
                                    static_cast<UINT32>(backtrans_text_.size()),
                                    backtrans_body_format_, body_rect, scratch_brush_);
        dc_render_target_->PopAxisAlignedClip();
    }

    // ---- [원문] sub-panel: subtle contrast + its own clipped viewport ----
    // The panel's background is what visually separates the comparison from
    // the back-translation above it (report §4 Option 2: "미세한 배경 대비"),
    // and it ends exactly where the planner ended the card's body, so the
    // original can never overdraw the card edge.
    const D2D1_RECT_F panel_rect = D2D1::RectF(
        kBackTransPadDip - kBackTransPanelPadDip, static_cast<float>(panels.ref_panel_top),
        w - kBackTransPadDip + kBackTransPanelPadDip,
        h - kBackTransCardBottomPadDip);
    if (panel_rect.bottom > panel_rect.top) {
        const D2D1_ROUNDED_RECT panel_rr =
            D2D1::RoundedRect(panel_rect, theme.control_radius, theme.control_radius);
        scratch_brush_->SetColor(ui::ColorFromHex(theme.surface_elevated, 0.55f));
        dc_render_target_->FillRoundedRectangle(panel_rr, scratch_brush_);
        scratch_brush_->SetColor(
            ui::ColorFromHex(theme.border_subtle, theme.border_subtle_alpha * 0.6f));
        dc_render_target_->DrawRoundedRectangle(panel_rr, scratch_brush_, 1.0f);
    }

    if (backtrans_section_format_ && !ref_label.empty()) {
        const bool collapsed =
            panels.ref_view_h <= 0 && !backtrans_ref_line_.empty(); // solo [역번역] view
        scratch_brush_->SetColor(
            collapsed
                ? ui::ColorFromHex(backtrans_ref_label_hover_ ? theme.text_primary
                                                              : theme.text_muted,
                                   1.0f)
                : ui::ColorFromHex(theme.text_muted, 1.0f));
        dc_render_target_->DrawText(ref_label.c_str(), static_cast<UINT32>(ref_label.size()),
                                    backtrans_section_format_, backtrans_ref_label_rect_,
                                    scratch_brush_);
    }

    if (backtrans_ref_format_ && !backtrans_ref_line_.empty() && panels.ref_view_h > 0) {
        const float view_top = static_cast<float>(panels.ref_view_top);
        const float view_bottom = view_top + static_cast<float>(panels.ref_view_h);
        // V10F FUP-05: the ORIGINAL pane scrolls on its OWN offset
        // (backtrans_ref_offset_dip_), so scrolling it no longer drags the
        // back-translation along.
        const float draw_off = ClampScrollOffset(backtrans_ref_offset_dip_,
                                                 static_cast<float>(panels.ref_content_h),
                                                 static_cast<float>(panels.ref_view_h));
        const D2D1_RECT_F clip_rect =
            D2D1::RectF(col_left, view_top, col_right, view_bottom);
        dc_render_target_->PushAxisAlignedClip(clip_rect, D2D1_ANTIALIAS_MODE_ALIASED);
        scratch_brush_->SetColor(ui::ColorFromHex(theme.text_secondary, 1.0f));
        const D2D1_RECT_F ref_rect = D2D1::RectF(
            col_left, view_top - draw_off, col_right,
            view_top - draw_off + static_cast<float>(panels.ref_content_h) + label_h);
        dc_render_target_->DrawText(backtrans_ref_line_.c_str(),
                                    static_cast<UINT32>(backtrans_ref_line_.size()),
                                    backtrans_ref_format_, ref_rect, scratch_brush_);
        dc_render_target_->PopAxisAlignedClip();
    }

    // ---- V10F FUP-05: ONE THUMB PER PANE ----
    // The pre-V10F card drew a single thumb across both viewports because both
    // panels shared ONE offset. With independent offsets that thumb would lie
    // about at least one of them, so each overflowing pane gets its own thumb
    // over its own viewport depth, and each maps its OWN offset. The rects are
    // stashed for the WM_SETCURSOR / WM_LBUTTONDOWN / WM_MOUSEMOVE hit tests
    // (the drag then moves the pane whose thumb was grabbed).
    const float track_left = w - kBackTransPadDip - static_cast<float>(kBackTransScrollbarWDip);
    const float track_w = static_cast<float>(kBackTransScrollbarWDip);
    const bool thumb_active = thumb_hover_ || dragging_thumb_;
    const auto draw_pane_thumb = [&](D2D1_RECT_F& track_rect, D2D1_RECT_F& thumb_rect,
                                     int view_top_dip, int view_h_dip, int content_dip,
                                     float offset_dip) {
        const float track_top = static_cast<float>(view_top_dip);
        const float track_h = static_cast<float>(view_h_dip);
        if (track_h <= 0.0f) {
            track_rect = {};
            thumb_rect = {};
            return;
        }
        track_rect = D2D1::RectF(track_left, track_top, track_left + track_w,
                                 track_top + track_h);
        const float viewport_h = track_h;
        const float content_h = static_cast<float>(content_dip);
        const float thumb_h = ScrollbarThumbHeightDip(track_h, viewport_h, content_h);
        const float thumb_top = ScrollbarThumbTopDip(track_top, track_h, thumb_h, offset_dip,
                                                     content_h, viewport_h);
        thumb_rect = D2D1::RectF(track_left, thumb_top, track_left + track_w,
                                 thumb_top + thumb_h);
        scratch_brush_->SetColor(ui::ColorFromHex(theme.surface_elevated, 0.75f));
        dc_render_target_->FillRoundedRectangle(D2D1::RoundedRect(track_rect, 3.0f, 3.0f),
                                               scratch_brush_);
        scratch_brush_->SetColor(thumb_active ? ui::ColorFromHex(theme.text_primary, 1.0f)
                                              : ui::ColorFromHex(theme.text_muted, 1.0f));
        dc_render_target_->FillRoundedRectangle(D2D1::RoundedRect(thumb_rect, 3.0f, 3.0f),
                                               scratch_brush_);
    };
    if (scrollable_ && panels.back_view_h > 0 &&
        panels.back_content_h > panels.back_view_h) {
        draw_pane_thumb(scrollbar_track_rect_, scrollbar_thumb_rect_, panels.back_view_top,
                        panels.back_view_h, panels.back_content_h, scroll_offset_dip_);
    } else {
        scrollbar_track_rect_ = {};
        scrollbar_thumb_rect_ = {};
    }
    if (scrollable_ && panels.ref_view_h > 0 &&
        panels.ref_content_h > panels.ref_view_h) {
        draw_pane_thumb(backtrans_ref_track_rect_, backtrans_ref_thumb_rect_,
                        panels.ref_view_top, panels.ref_view_h, panels.ref_content_h,
                        backtrans_ref_offset_dip_);
    } else {
        backtrans_ref_track_rect_ = {};
        backtrans_ref_thumb_rect_ = {};
    }
    if (!scrollable_) {
        // Never leave a stale thumb rect behind: a non-scrolling card must not
        // answer the drag hit test (the normal-mode renderer keeps the same
        // invariant).
        scrollbar_track_rect_ = {};
        scrollbar_thumb_rect_ = {};
    }
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

    const auto& theme = ui::GetActiveTheme();

    // Outer elevated card container with theme surface + outer/inner borders
    const D2D1_RECT_F card_rect = D2D1::RectF(
        0.5f, 0.5f,
        static_cast<float>(current_width_) - 0.5f,
        static_cast<float>(current_height_) - 0.5f);
    ui::DrawElevatedCard(dc_render_target_, card_rect, theme.card_radius, 1.0f, theme);

    // --- 261108_0002 (WS3, v0.10.6): floating back-translation card ---
    // Dispatched here (after the shared elevated-card chrome) so the card
    // shares the exact surface/border treatment of the other two modes.
    if (is_backtrans_mode_) {
        RenderBackTranslation();
        const HRESULT hr_back = dc_render_target_->EndDraw();
        if (IsRecoverableDeviceLost(hr_back)) {
            RecreateAfterDeviceLost();
        }
        return;
    }

    // --- V10F L3-B: progressive preview card (live, chunk by chunk) ---
    if (preview_phase_ != ChunkPreviewPhase::Inactive) {
        RenderChunkPreview();
        const HRESULT hr_preview = dc_render_target_->EndDraw();
        if (IsRecoverableDeviceLost(hr_preview)) {
            RecreateAfterDeviceLost();
        }
        return;
    }

    // --- REQ-R08 message mode: compact header + body notice, no action buttons ---
    if (is_message_mode_) {
        D2D1_RECT_F msgHeaderRect = D2D1::RectF(14.0f, 10.0f, static_cast<float>(current_width_) - 14.0f, 36.0f);
        if (header_format_) {
            scratch_brush_->SetColor(ui::ColorFromHex(theme.text_primary, 1.0f));
            dc_render_target_->DrawText(
                message_header_.c_str(), static_cast<UINT32>(message_header_.size()),
                header_format_, msgHeaderRect, scratch_brush_);
        }
        D2D1_RECT_F msgBodyRect = D2D1::RectF(14.0f, 38.0f, static_cast<float>(current_width_) - 14.0f,
                                              static_cast<float>(current_height_) - 10.0f);
        if (small_format_) {
            scratch_brush_->SetColor(ui::ColorFromHex(theme.text_secondary, 1.0f));
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
    D2D1_ROUNDED_RECT logoFrame = D2D1::RoundedRect(D2D1::RectF(14.0f, 8.0f, 36.0f, 30.0f), theme.control_radius, theme.control_radius);
    scratch_brush_->SetColor(ui::ColorFromHex(theme.surface_elevated, 0.95f));
    dc_render_target_->FillRoundedRectangle(logoFrame, scratch_brush_);
    scratch_brush_->SetColor(ui::ColorFromHex(theme.border_subtle, theme.border_subtle_alpha));
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
    const float src_tag_width = src_tag_width_;
    src_btn_rect_ = D2D1::RectF(src_tag_x, 8.0f, src_tag_x + src_tag_width, 30.0f);
    D2D1_ROUNDED_RECT srcTagRect = D2D1::RoundedRect(src_btn_rect_, theme.control_radius, theme.control_radius);
    scratch_brush_->SetColor((hovered_btn_ == 5) ? ui::ColorFromHex(theme.surface_hover, 1.0f)
                                                 : ui::ColorFromHex(theme.surface_elevated, 0.95f));
    dc_render_target_->FillRoundedRectangle(srcTagRect, scratch_brush_);
    scratch_brush_->SetColor((hovered_btn_ == 5) ? ui::ColorFromHex(theme.border_hover, theme.border_hover_alpha)
                                                 : ui::ColorFromHex(theme.border_subtle, theme.border_subtle_alpha));
    dc_render_target_->DrawRoundedRectangle(srcTagRect, scratch_brush_, 1.0f);
    if (header_format_) {
        scratch_brush_->SetColor((hovered_btn_ == 5) ? ui::ColorFromHex(theme.text_primary, 1.0f)
                                                     : ui::ColorFromHex(theme.text_secondary, 1.0f));
        dc_render_target_->DrawText(src_tag.c_str(), static_cast<UINT32>(src_tag.size()), header_format_, srcTagRect.rect, scratch_brush_);
    }

    // 3. Arrow indicator
    if (header_format_) {
        D2D1_RECT_F arrowRect = D2D1::RectF(src_tag_x + src_tag_width + 4.0f, 8.0f, src_tag_x + src_tag_width + 24.0f, 30.0f);
        scratch_brush_->SetColor(ui::ColorFromHex(theme.text_muted, 1.0f));
        dc_render_target_->DrawText(L"→", 1, header_format_, arrowRect, scratch_brush_);
    }

    // 4. Target language switcher dropdown button
    std::wstring tgt_name = target_lang_.empty() ? I18n::GetLanguageDisplayName("English")
                                                 : ToUtf16(target_lang_);
    std::wstring tgt_label = tgt_name + L" ▾";
    // ---- V10F L2/L1-B: the right-anchored header action cluster -----------
    // [pin][edit][open][✕], right to left. PlanHeaderActionCluster decides
    // between labelled pills and 20 DIP icon boxes from the room left of the
    // target pill's minimum position, and the TARGET planner is then fed an
    // effective card width whose close edge IS the cluster's left edge - so the
    // pill can never run under the cluster, on any width tier or locale.
    const float header_close_left = static_cast<float>(current_width_) - 32.0f;
    const float header_pill_min_left =
        src_tag_x + src_tag_width + 28.0f + kHeaderTargetMinWidthDip;
    header_action_plan_ = PlanHeaderActionCluster(
        header_close_left, header_pill_min_left, pin_btn_label_w_, edit_btn_label_w_,
        open_btn_label_w_);
    pin_btn_rect_ = D2D1::RectF(header_action_plan_.left, 8.0f,
                                header_action_plan_.left + header_action_plan_.pin_w, 30.0f);
    edit_btn_rect_ = D2D1::RectF(pin_btn_rect_.right + kHeaderActionGapDip, 8.0f,
                                 pin_btn_rect_.right + kHeaderActionGapDip +
                                     header_action_plan_.edit_w,
                                 30.0f);
    open_btn_rect_ = D2D1::RectF(edit_btn_rect_.right + kHeaderActionGapDip, 8.0f,
                                 edit_btn_rect_.right + kHeaderActionGapDip +
                                     header_action_plan_.open_w,
                                 30.0f);
    const HeaderTargetButtonPlan tgt_plan = PlanHeaderTargetButton(
        src_tag_x, src_tag_width,
        header_action_plan_.left + 32.0f, // effective width: close edge == cluster left
        tgt_btn_w_);
    lang_btn_rect_ = D2D1::RectF(tgt_plan.x, 8.0f, tgt_plan.x + tgt_plan.width, 30.0f);
    D2D1_ROUNDED_RECT tgtTagRect = D2D1::RoundedRect(lang_btn_rect_, theme.control_radius, theme.control_radius);

    // Fix Anti-Pattern 5: both Source and Target pills use surface_elevated and
    // border_subtle at rest, transitioning to surface_hover and border_hover on
    // hover, with accent_primary text on Target so hierarchy is clean without
    // permanent gold borders at rest.
    scratch_brush_->SetColor((hovered_btn_ == 3) ? ui::ColorFromHex(theme.surface_hover, 1.0f)
                                                 : ui::ColorFromHex(theme.surface_elevated, 0.95f));
    dc_render_target_->FillRoundedRectangle(tgtTagRect, scratch_brush_);
    scratch_brush_->SetColor((hovered_btn_ == 3) ? ui::ColorFromHex(theme.border_hover, theme.border_hover_alpha)
                                                 : ui::ColorFromHex(theme.border_subtle, theme.border_subtle_alpha));
    dc_render_target_->DrawRoundedRectangle(tgtTagRect, scratch_brush_, 1.0f);
    if (button_format_) {
        scratch_brush_->SetColor(ui::ColorFromHex(theme.accent_primary, 1.0f));
        dc_render_target_->DrawText(tgt_label.c_str(), static_cast<UINT32>(tgt_label.size()), button_format_, lang_btn_rect_, scratch_brush_);
    }

    // ---- V10F L2/L1-B: the three header action buttons --------------------
    // One draw block for all three so the fill/border/hover treatment matches
    // the source and target pills exactly. `labels` comes from the cluster
    // plan: on the 360 DIP tier the boxes are 20 DIP icon squares (no shipped
    // locale fits "수정 · 재번역" there), on 480/620 they carry the localized
    // label. A callback that was never registered renders the button dimmed
    // instead of disappearing, so the header geometry never reflows when P2-E
    // wires it. (261122_0002 Wave-C: the wording avoids the P6c sweep's
    // banned needle word, which guards the retired pre-P6c claims.)
    const bool pinned_now = pinned_.load(std::memory_order_relaxed);
    const auto draw_header_action = [&](const D2D1_RECT_F& rect, int hover_id,
                                        const wchar_t* glyph, const std::wstring& label,
                                        bool wired, UINT32 resting_color) {
        const bool hovered = hovered_btn_ == hover_id && wired;
        const D2D1_ROUNDED_RECT rr =
            D2D1::RoundedRect(rect, theme.control_radius, theme.control_radius);
        scratch_brush_->SetColor(hovered ? ui::ColorFromHex(theme.surface_hover, 1.0f)
                                         : ui::ColorFromHex(theme.surface_elevated, 0.95f));
        dc_render_target_->FillRoundedRectangle(rr, scratch_brush_);
        scratch_brush_->SetColor(hovered
                                     ? ui::ColorFromHex(theme.border_hover, theme.border_hover_alpha)
                                     : ui::ColorFromHex(theme.border_subtle,
                                                        theme.border_subtle_alpha));
        dc_render_target_->DrawRoundedRectangle(rr, scratch_brush_, 1.0f);
        if (!header_format_ || !button_format_) return;
        scratch_brush_->SetColor(hovered ? ui::ColorFromHex(theme.text_primary, 1.0f)
                                         : ui::ColorFromHex(resting_color, wired ? 1.0f : 0.45f));
        if (header_action_plan_.labels && !label.empty()) {
            dc_render_target_->DrawText(label.c_str(), static_cast<UINT32>(label.size()),
                                        button_format_, rect, scratch_brush_);
        } else {
            const std::wstring glyph_text(glyph ? glyph : L"");
            dc_render_target_->DrawText(glyph_text.c_str(),
                                        static_cast<UINT32>(glyph_text.size()),
                                        header_format_, rect, scratch_brush_);
        }
    };
    draw_header_action(
        pin_btn_rect_, 6, L"\xD83D\xDCCC",
        I18n::Get(pinned_now ? StringId::UnpinTooltipCard : StringId::PinTooltipCard), true,
        pinned_now ? theme.accent_primary : theme.text_secondary);
    draw_header_action(edit_btn_rect_, 7, L"\x270E",
                       I18n::Get(StringId::EditRetranslate),
                       static_cast<bool>(edit_request_cb_), theme.text_secondary);
    draw_header_action(open_btn_rect_, 8, L"\x29C9",
                       I18n::Get(StringId::OpenInWindow),
                       static_cast<bool>(open_in_window_cb_), theme.text_secondary);

    // Close button (top right)
    close_btn_rect_ = D2D1::RectF(static_cast<float>(current_width_) - 32.0f, 8.0f, static_cast<float>(current_width_) - 12.0f, 28.0f);
    if (header_format_) {
        scratch_brush_->SetColor((hovered_btn_ == 4) ? ui::ColorFromHex(theme.state_danger, 1.0f)
                                                     : ui::ColorFromHex(theme.text_muted, 0.85f));
        dc_render_target_->DrawText(L"✕", 1, header_format_, close_btn_rect_, scratch_brush_);
    }

    // Header divider line
    scratch_brush_->SetColor(ui::ColorFromHex(theme.border_subtle, theme.border_subtle_alpha));
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
        scratch_brush_->SetColor(ui::ColorFromHex(theme.text_primary, 1.0f));
        dc_render_target_->PushAxisAlignedClip(body_viewport_rect_, D2D1_ANTIALIAS_MODE_ALIASED);
        // V10F FUP-08: paint the layout the show path already shaped. The
        // DrawText fallback is the pre-V10F path, kept for the degenerate case
        // where CreateTextLayout failed (no factory, OOM) - a failed cache must
        // never blank the body.
        if (body_text_layout_) {
            dc_render_target_->DrawTextLayout(
                D2D1::Point2F(14.0f, body_y - scroll_offset_dip_), body_text_layout_,
                scratch_brush_);
        } else {
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
        }
        dc_render_target_->PopAxisAlignedClip();
    }

    // --- Custom D2D scrollbar (plan §2.1 Option A-lite) ---
    if (scrollable_) {
        const float track_left = static_cast<float>(current_width_) - 12.0f;
        scrollbar_track_rect_ = D2D1::RectF(track_left, body_y, track_left + 8.0f, body_y + body_height);

        const float thumb_h = ScrollbarThumbHeightDip(body_height, body_height, content_height_dip_);
        const float thumb_top = ScrollbarThumbTopDip(
            body_y, body_height, thumb_h, scroll_offset_dip_, content_height_dip_, body_height);
        scrollbar_thumb_rect_ = D2D1::RectF(track_left, thumb_top, track_left + 8.0f, thumb_top + thumb_h);

        const bool thumb_active = thumb_hover_ || dragging_thumb_;
        scratch_brush_->SetColor(ui::ColorFromHex(theme.surface_elevated, 0.75f));
        dc_render_target_->FillRoundedRectangle(
            D2D1::RoundedRect(scrollbar_track_rect_, 4.0f, 4.0f), scratch_brush_);
        scratch_brush_->SetColor(thumb_active ? ui::ColorFromHex(theme.text_primary, 1.0f)
                                              : ui::ColorFromHex(theme.text_muted, 1.0f));
        dc_render_target_->FillRoundedRectangle(
            D2D1::RoundedRect(scrollbar_thumb_rect_, 4.0f, 4.0f), scratch_brush_);
    } else {
        scrollbar_track_rect_ = {};
        scrollbar_thumb_rect_ = {};
    }

    // Footer divider line
    float footer_div_y = static_cast<float>(current_height_) - 40.0f;
    scratch_brush_->SetColor(ui::ColorFromHex(theme.border_subtle, theme.border_subtle_alpha));
    dc_render_target_->DrawLine(
        D2D1::Point2F(14.0f, footer_div_y),
        D2D1::Point2F(static_cast<float>(current_width_) - 14.0f, footer_div_y),
        scratch_brush_,
        1.0f
    );

    // --- Footer Action Buttons ---
    const float copy_btn_w = copy_btn_w_;
    const float tts_btn_w = tts_btn_w_;
    const float btn_top = footer_div_y + 7.0f;
    const float btn_bottom = footer_div_y + 31.0f;
    const float icon_cy = (btn_top + btn_bottom) * 0.5f;

    // Copy button (with 12x12 DIP vector icon on left, no emoji prefix)
    copy_btn_rect_ = D2D1::RectF(14.0f, btn_top, 14.0f + copy_btn_w, btn_bottom);
    D2D1_ROUNDED_RECT copyBtnRect = D2D1::RoundedRect(copy_btn_rect_, theme.control_radius, theme.control_radius);
    const D2D1_RECT_F copyIconRect = D2D1::RectF(
        copy_btn_rect_.left + 10.0f, icon_cy - 6.0f,
        copy_btn_rect_.left + 22.0f, icon_cy + 6.0f);
    const D2D1_RECT_F copyTextRect = D2D1::RectF(
        copy_btn_rect_.left + 24.0f, copy_btn_rect_.top,
        copy_btn_rect_.right - 6.0f, copy_btn_rect_.bottom);

    if (copied_feedback_) {
        // Fix Anti-Pattern 1: cohesive state_ready tint/border/text + DrawCheckIcon
        scratch_brush_->SetColor(ui::ColorFromHex(theme.state_ready, 0.15f));
        dc_render_target_->FillRoundedRectangle(copyBtnRect, scratch_brush_);
        scratch_brush_->SetColor(ui::ColorFromHex(theme.state_ready, 0.65f));
        dc_render_target_->DrawRoundedRectangle(copyBtnRect, scratch_brush_, 1.0f);
        scratch_brush_->SetColor(ui::ColorFromHex(theme.state_ready, 1.0f));
        ui::DrawCheckIcon(dc_render_target_, copyIconRect, scratch_brush_, 1.5f);
        if (button_format_) {
            const std::wstring feedback = StripLeadingEmojiIcon(I18n::Get(StringId::TooltipCopied));
            dc_render_target_->DrawText(feedback.c_str(), static_cast<UINT32>(feedback.size()), button_format_, copyTextRect, scratch_brush_);
        }
    } else {
        scratch_brush_->SetColor((hovered_btn_ == 1) ? ui::ColorFromHex(theme.surface_hover, 1.0f)
                                                     : ui::ColorFromHex(theme.surface_elevated, 0.95f));
        dc_render_target_->FillRoundedRectangle(copyBtnRect, scratch_brush_);
        scratch_brush_->SetColor((hovered_btn_ == 1) ? ui::ColorFromHex(theme.border_hover, theme.border_hover_alpha)
                                                     : ui::ColorFromHex(theme.border_subtle, theme.border_subtle_alpha));
        dc_render_target_->DrawRoundedRectangle(copyBtnRect, scratch_brush_, 1.0f);
        scratch_brush_->SetColor((hovered_btn_ == 1) ? ui::ColorFromHex(theme.text_primary, 1.0f)
                                                     : ui::ColorFromHex(theme.text_secondary, 1.0f));
        ui::DrawCopyIcon(dc_render_target_, copyIconRect, scratch_brush_, 1.4f);
        if (button_format_) {
            const std::wstring label = StripLeadingEmojiIcon(I18n::Get(StringId::TooltipButtonCopy));
            dc_render_target_->DrawText(label.c_str(), static_cast<UINT32>(label.size()), button_format_, copyTextRect, scratch_brush_);
        }
    }

    // TTS / Speak button (with 12x12 DIP vector speaker icon on left, no emoji prefix)
    const float tts_left = 14.0f + copy_btn_w + 8.0f;
    tts_btn_rect_ = D2D1::RectF(tts_left, btn_top, tts_left + tts_btn_w, btn_bottom);
    D2D1_ROUNDED_RECT ttsBtnRect = D2D1::RoundedRect(tts_btn_rect_, theme.control_radius, theme.control_radius);
    const D2D1_RECT_F ttsIconRect = D2D1::RectF(
        tts_btn_rect_.left + 10.0f, icon_cy - 6.0f,
        tts_btn_rect_.left + 22.0f, icon_cy + 6.0f);
    const D2D1_RECT_F ttsTextRect = D2D1::RectF(
        tts_btn_rect_.left + 24.0f, tts_btn_rect_.top,
        tts_btn_rect_.right - 6.0f, tts_btn_rect_.bottom);

    scratch_brush_->SetColor((hovered_btn_ == 2) ? ui::ColorFromHex(theme.surface_hover, 1.0f)
                                                 : ui::ColorFromHex(theme.surface_elevated, 0.95f));
    dc_render_target_->FillRoundedRectangle(ttsBtnRect, scratch_brush_);
    scratch_brush_->SetColor((hovered_btn_ == 2) ? ui::ColorFromHex(theme.border_hover, theme.border_hover_alpha)
                                                 : ui::ColorFromHex(theme.border_subtle, theme.border_subtle_alpha));
    dc_render_target_->DrawRoundedRectangle(ttsBtnRect, scratch_brush_, 1.0f);
    scratch_brush_->SetColor((hovered_btn_ == 2) ? ui::ColorFromHex(theme.text_primary, 1.0f)
                                                 : ui::ColorFromHex(theme.text_secondary, 1.0f));
    ui::DrawSpeakerIcon(dc_render_target_, ttsIconRect, scratch_brush_, 1.4f, true);
    if (button_format_) {
        const std::wstring label = StripLeadingEmojiIcon(I18n::Get(StringId::TooltipButtonTts));
        dc_render_target_->DrawText(label.c_str(), static_cast<UINT32>(label.size()), button_format_, ttsTextRect, scratch_brush_);
    }

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

// R6 Phase 3 (audit items 6+8) / SEC-ADJ (Blocker-5 class fix): discard every
// marshal payload still queued when the window goes away, mirroring the old
// PeekMessageW purge semantics exactly — REMOVE without applying (the old code
// deleted the heap payloads; clearing the by-value deques releases the same
// string memory and keeps the "drained payloads never render" observable).
// The remaining parameter-less wake-up notifications can still sit in the OS
// queue at this point: consuming them finds empty deques and is a no-op, so
// no apply-after-destroy path exists. Everything else (kDismissMessage,
// kScrollMessage, timers, input) was and stays untouched for the normal
// teardown path.
int TooltipWindow::DrainMarshalQueue() {
    if (!hwnd_) return 0;
    int drained = 0;
    {
        std::lock_guard<std::mutex> lk(g_tt_translation_mu);
        drained += static_cast<int>(g_tt_translation_queue.size());
        g_tt_translation_queue.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_tt_message_mu);
        drained += static_cast<int>(g_tt_message_queue.size());
        g_tt_message_queue.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_tt_targetlang_mu);
        drained += static_cast<int>(g_tt_targetlang_queue.size());
        g_tt_targetlang_queue.clear();
    }
    // 261108_0002 (WS3, v0.10.6): the back-translation queue joins the
    // teardown sweep (same discard-without-apply semantics).
    {
        std::lock_guard<std::mutex> lk(g_tt_backtrans_mu);
        drained += static_cast<int>(g_tt_backtrans_queue.size());
        g_tt_backtrans_queue.clear();
    }
    return drained;
}

// Test/inspection seam (SEC-ADJ): enqueue a caller-provided payload VALUE into
// the queue for `msg` and post the same parameter-less wake-up the real seams
// use. Ownership never transfers (the entry is copied); unsupported message
// ids return false WITHOUT posting, matching the old "caller keeps the
// pointer, deletes locally" test contract at the call sites.
bool TooltipWindow::PostPayloadForTest(HWND hwnd, UINT msg, const void* payload) {
    if (!hwnd || !payload) {
        return false;
    }
    switch (msg) {
        case kShowTranslationMessage: {
            std::lock_guard<std::mutex> lk(g_tt_translation_mu);
            g_tt_translation_queue.push_back(
                *static_cast<const TranslationPayload*>(payload));
            break;
        }
        case kShowMessageMessage: {
            std::lock_guard<std::mutex> lk(g_tt_message_mu);
            g_tt_message_queue.push_back(
                *static_cast<const MessagePayload*>(payload));
            break;
        }
        case kRefreshTargetLangMessage: {
            std::lock_guard<std::mutex> lk(g_tt_targetlang_mu);
            g_tt_targetlang_queue.push_back(
                *static_cast<const TargetLangPayload*>(payload));
            break;
        }
        case kShowBackTranslationMessage: {
            std::lock_guard<std::mutex> lk(g_tt_backtrans_mu);
            g_tt_backtrans_queue.push_back(
                *static_cast<const BackTranslationPayload*>(payload));
            break;
        }
        default:
            return false;
    }
    return ::PostMessageW(hwnd, msg, 0, 0) == TRUE;
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
        // These run on the GUI thread that owns the D2D target. SEC-ADJ
        // (Blocker-5 class shatter fix): wParam/LPARAM are IGNORED entirely —
        // the wake-up message carries no payload (producers always post 0/0;
        // any value an external process posts is untrusted by design). The
        // handler drains its value deque one entry at a time under a per-item
        // lock, so entries enqueued while an earlier one is being applied are
        // still consumed by this same wake-up. R6 B1-H1 generation guard
        // semantics unchanged: ShowTranslation/ShowMessage drop superseded
        // payloads on the GUI thread.
        case kShowTranslationMessage: {
            for (;;) {
                TranslationPayload p;
                {
                    std::lock_guard<std::mutex> lk(g_tt_translation_mu);
                    if (g_tt_translation_queue.empty()) {
                        break;
                    }
                    p = std::move(g_tt_translation_queue.front());
                    g_tt_translation_queue.pop_front();
                }
                pThis->ShowTranslation(p.x, p.y, p.source_text,
                                       p.source_lang_code, p.target_lang, p.translated_text,
                                       p.generation);
            }
            return 0;
        }

        case kShowMessageMessage: {
            for (;;) {
                MessagePayload p;
                {
                    std::lock_guard<std::mutex> lk(g_tt_message_mu);
                    if (g_tt_message_queue.empty()) {
                        break;
                    }
                    p = std::move(g_tt_message_queue.front());
                    g_tt_message_queue.pop_front();
                }
                pThis->ShowMessage(p.x, p.y, p.header, p.body, p.generation, p.autohide_ms);
            }
            return 0;
        }

        case kDismissMessage: {
            // 261111_0001 U1 (v0.10.7, audit D1a/D1b) + fix-list round 2 G-06:
            // idempotent. An off-thread Dismiss()/DismissThreadSafe() clears
            // visibility, owes the teardown and captures the card generation
            // of the card it began on before posting, so the GUI-side teardown
            // MUST run even though the model already reads hidden (the window
            // is still on screen until TeardownCard's SW_HIDE). The message
            // carries that generation in WPARAM: a message whose card was
            // replaced by a fresher Show* (newer generation) is STALE and a
            // pure no-op - it must never tear down a card the user did not
            // dismiss. A duplicate message for the same card finds the owed
            // stamp already consumed and is a no-op too (no double teardown,
            // no crash).
            const uint64_t dismiss_generation = static_cast<uint64_t>(wParam);
            if (pThis->card_visibility_.TryCompleteDismiss(dismiss_generation)) {
                pThis->TeardownCard();
            } else {
                DIAG_LOG("UI", "tooltip_dismiss_dropped_stale card_gen=%llu current=%llu",
                         static_cast<unsigned long long>(dismiss_generation),
                         static_cast<unsigned long long>(
                             pThis->card_visibility_.Generation()));
            }
            return 0;
        }

        // 261108_0002 (WS3, v0.10.6): marshaled floating back-translation
        // card. Same SEC-ADJ value-queue drain as the show handlers above; the
        // HWNDs travel inside the payload and are re-validated with IsWindow
        // by ResolveBackTranslationAnchor, so a stale/foreign handle degrades
        // to the next anchor tier instead of being dereferenced.
        case kShowBackTranslationMessage: {
            for (;;) {
                BackTranslationPayload p;
                {
                    std::lock_guard<std::mutex> lk(g_tt_backtrans_mu);
                    if (g_tt_backtrans_queue.empty()) {
                        break;
                    }
                    p = std::move(g_tt_backtrans_queue.front());
                    g_tt_backtrans_queue.pop_front();
                }
                pThis->ShowBackTranslation(p.original_text, p.back_text,
                                           p.source_lang_label, p.target_lang_label,
                                           p.meaning_matched, p.target_hwnd, p.badge_hwnd);
            }
            return 0;
        }

        // V10F L3-B: the marshaled progressive-preview operations. Same SEC-ADJ
        // value-queue drain as the handlers above; the cumulative text travels
        // inside the payload, so the wake-up stays a pure (0, 0) notification.
        case kChunkPreviewMessage: {
            for (;;) {
                ChunkPreviewPayload p;
                {
                    std::lock_guard<std::mutex> lk(g_tt_preview_mu);
                    if (g_tt_preview_queue.empty()) {
                        break;
                    }
                    p = std::move(g_tt_preview_queue.front());
                    g_tt_preview_queue.pop_front();
                }
                pThis->ApplyChunkPreviewOp(p.op, p.total_chunks, p.cumulative, p.success);
            }
            return 0;
        }

        // R6 Phase 1 (B3): marshaled target-language view refresh. Same
        // SEC-ADJ value-queue drain as the show handlers above. Running
        // RefreshTargetLanguageFromConfig here is on the GUI thread, so it
        // takes the inline path (no re-post, no recursion).
        case kRefreshTargetLangMessage: {
            for (;;) {
                TargetLangPayload p;
                {
                    std::lock_guard<std::mutex> lk(g_tt_targetlang_mu);
                    if (g_tt_targetlang_queue.empty()) {
                        break;
                    }
                    p = std::move(g_tt_targetlang_queue.front());
                    g_tt_targetlang_queue.pop_front();
                }
                pThis->RefreshTargetLanguageFromConfig(p.target_lang);
            }
            return 0;
        }

        // ---- 261116_0003 P6b: the deferred language-picker open (Option B) --
        // The two dropdowns' "검색…" rows latch a PickerInit through
        // PostDeferredLanguagePaletteOpen(hwnd, init) — hwnd is THIS window:
        // the tooltip is its own controller for its two picker surfaces (the
        // tray's four surfaces keep posting to main.cpp's controller) — and
        // the picker module posts its kMsgOpenLanguagePalette wake-up
        // (WM_APP + 0x503, language_search_palette.hpp) here.
        //
        // SEC-ADJ discipline, identical to the four marshal seams above:
        // wParam and lParam are IGNORED entirely (the wake-up carries no
        // payload; any value an external process posts is untrusted by
        // design) and the request travels through the picker module's
        // PER-SURFACE latch (PR6-32: one slot per PickerSurface, so this
        // window's bare take consumes exactly the pill slot and can never
        // swallow a tray request). The optional is guarded with has_value()
        // and is NEVER unwrapped through the throwing accessor: an EMPTY
        // latch (an orphan wake-up — a previous consumer already took the
        // request, or a foreign process posted the id) is logged once and
        // dropped, never opened with default/empty codes. The arm returns 0
        // on every path (P6a security-review obligations, carried over).
        case kMsgOpenLanguagePalette: {
            // 261116_0003 P6c fix round (PR6-42): same nested-delivery guard
            // as main.cpp's controller arm. A wake-up delivered while a picker
            // is already open arrives NESTED through that picker's own modal
            // pump; taking the latch here would consume a request the
            // single-instance guard then refuses to show. The nested delivery
            // leaves its request LATCHED and exits; the arm whose show() owns
            // the running loop drains the latch after its popup closes (the
            // loop below) and serves the deferred request then. Stated
            // without overstatement: the guarantee is the drain pass of the
            // arm that owns the open picker's show() — for a pill request
            // that is THIS arm, so the request is served right after that
            // picker closes.
            if (LanguagePickerShowing()) {
                DIAG_LOG("UI", "tooltip_picker_wake result=deferred_while_open");
                return 0;
            }
            bool first_pass = true;
            for (;;) {
                std::optional<language_palette::PickerInit> init =
                    TakeDeferredLanguagePaletteOpen();
                if (!init.has_value()) {
                    if (first_pass) {
                        // Empty latch on a real delivery: an orphan wake-up.
                        // Later passes exit silently (the normal drain end).
                        DIAG_LOG("UI", "tooltip_picker_wake result=orphan_latch_empty");
                    }
                    break;
                }
                first_pass = false;
                // 261116_0003 P6b fix round (PR6-29, SEC-ADJ), re-anchored to
                // the landed P6c state (PR6-43 sweep): the per-surface slots
                // (PR6-32) already make a foreign request impossible by
                // construction — this window's bare take reads ONLY the pill
                // slot, and the tray's TrayTyping/TrayTooltip requests live in
                // slots only main.cpp's controller arm consumes. The
                // ownership check below stays as the belt-and-braces twin of
                // the controller arm's PR6-32 check: if either consumer's
                // take is ever regressed to a cross-surface take, the
                // wrong-pair dispatch is still refused here — one diagnostic
                // line, no picker, no dispatch, return 0.
                if (init->surface != language_palette::PickerSurface::TooltipPill) {
                    DIAG_LOG("UI", "tooltip_picker_wake result=surface_mismatch surface=%d",
                             static_cast<int>(init->surface));
                    return 0;
                }
                // The dropdown's screen anchor, re-derived at handler time from
                // the REQUEST role's button rect (the same DIP -> px +
                // ClientToScreen conversion the menu blocks use): the card may
                // have moved between the post and this pump, so the fresh rect is
                // the honest anchor. Re-derived per drain pass for the same
                // reason.
                POINT anchor =
                    (init->role == language_palette::PickerRole::Source)
                        ? POINT{emebalachat::ui::ScaleDipsToPixels(
                                    static_cast<int>(pThis->src_btn_rect_.left), pThis->dpi_),
                                emebalachat::ui::ScaleDipsToPixels(
                                    static_cast<int>(pThis->src_btn_rect_.bottom), pThis->dpi_)}
                        : POINT{emebalachat::ui::ScaleDipsToPixels(
                                    static_cast<int>(pThis->lang_btn_rect_.left), pThis->dpi_),
                                emebalachat::ui::ScaleDipsToPixels(
                                    static_cast<int>(pThis->lang_btn_rect_.bottom), pThis->dpi_)};
                ::ClientToScreen(hwnd, &anchor);
                // V10F FUP-02: latch the card's source state BEFORE the modal
                // picker runs. Every dismissal path empties source_text_/
                // source_lang_code_ (TeardownCard above); without the latch, a
                // mid-session dismissal would hand the pick's retranslate
                // emptied state (retranslate_skip_empty). The arm clears the
                // latch right after the dispatch below, on every outcome.
                pThis->LatchLanguagePickerSourceSnapshot();
                // 260922_0001 A5 / PR5-03: the same focus choreography the two
                // TrackPopupMenuEx paths run — back up the foreground owner, take
                // it for the modal surface, restore after it closes.
                HWND hwndPrev = ::GetForegroundWindow();
                ::SetForegroundWindow(hwnd);
                const LanguagePickerResult picked =
                    ShowLanguageSearchPalette(hwnd, *init, &anchor);
                if (hwndPrev && hwndPrev != hwnd && ::IsWindow(hwndPrev)) {
                    ::SetForegroundWindow(hwndPrev);
                }
                if (picked.committed) {
                    // The role is the picker's COMMIT-TIME echo (a Tab flip before
                    // committing moves the pick to the flipped role), so the pick
                    // always lands on the pair the user actually edited. The
                    // payload is the registry row's own name_en — byte-identical
                    // to what the same pick made in the flat menu would dispatch
                    // (PickerState::PayloadOf's contract).
                    if (picked.role == language_palette::PickerRole::Source) {
                        if (pThis->src_lang_change_cb_) {
                            pThis->src_lang_change_cb_(picked.name_en);
                        }
                    } else {
                        if (pThis->lang_change_cb_) {
                            pThis->lang_change_cb_(picked.name_en);
                        }
                    }
                }
                // V10F FUP-02: the snapshot is per-picker-session; release it on
                // every outcome (commit, cancel, click-outside) so a later menu
                // pick can never read a stale latch.
                pThis->ClearLanguagePickerSourceSnapshot();
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

            // 261108_0002 (WS3): the back-translation card's interactive
            // surfaces are the ✕ and (v0.10.9 U-B) the shared scrollbar thumb:
            // hand cursor over both, arrow elsewhere. The thumb is only
            // interactive while the card actually overflows.
            if (pThis->is_backtrans_mode_) {
                if (IsPointInRect(pThis->backtrans_close_rect_, x, y) ||
                    IsPointInRect(pThis->backtrans_pin_rect_, x, y) ||
                    IsPointInRect(pThis->backtrans_back_label_rect_, x, y) ||
                    IsPointInRect(pThis->backtrans_ref_label_rect_, x, y) ||
                    (pThis->scrollable_ &&
                     IsPointInRect(pThis->scrollbar_thumb_rect_, x, y)) ||
                    // V10F FUP-05: the original pane's own thumb.
                    (pThis->scrollable_ &&
                     IsPointInRect(pThis->backtrans_ref_thumb_rect_, x, y))) {
                    ::SetCursor(::LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
                } else {
                    ::SetCursor(::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
                }
                return TRUE;
            }

            if (IsPointInRect(pThis->copy_btn_rect_, x, y) ||
                IsPointInRect(pThis->tts_btn_rect_, x, y) ||
                // F8 (ADR-A1-4): the source button is interactive now.
                IsPointInRect(pThis->src_btn_rect_, x, y) ||
                IsPointInRect(pThis->lang_btn_rect_, x, y) ||
                IsPointInRect(pThis->close_btn_rect_, x, y) ||
                // V10F L2/L1-B: the header action cluster (pin/edit/open).
                IsPointInRect(pThis->pin_btn_rect_, x, y) ||
                IsPointInRect(pThis->edit_btn_rect_, x, y) ||
                IsPointInRect(pThis->open_btn_rect_, x, y) ||
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

            // 261108_0002 (WS3, v0.10.6): hovering the back-translation card
            // PAUSES the auto-dismiss (spec). The planner makes the first move
            // the only timer action; the close-button/thumb hovers are the only
            // repaint triggers (the card is otherwise static).
            //
            // v0.10.9 (U-B, defect 3-1): the branch ALSO drives the shared
            // scrollbar thumb drag. It previously returned before the REQ-002
            // drag code, so a backtrans card could never be dragged even if a
            // drag had started; the drag update now runs first, through the
            // same helper the translation card uses.
            if (pThis->is_backtrans_mode_) {
                if (pThis->UpdateScrollThumbDrag(y)) {
                    repaint = true;
                }
                pThis->ApplyBackTransTimerPlan(TooltipWindow::BackTransTimerEvent::PointerMoved);
                // V10F: the pin click must NOT pause/close the card; the hover
                // read stays the plain pause + repaint.
                const bool close_hover = IsPointInRect(pThis->backtrans_close_rect_, x, y);
                if (close_hover != pThis->backtrans_close_hover_) {
                    pThis->backtrans_close_hover_ = close_hover;
                    repaint = true;
                }
                const bool pin_hover = IsPointInRect(pThis->backtrans_pin_rect_, x, y);
                if (pin_hover != pThis->backtrans_pin_hover_) {
                    pThis->backtrans_pin_hover_ = pin_hover;
                    repaint = true;
                }
                // V10F FUP-05: the two section-label bands are the view toggle,
                // so they read as interactive (hover lift + hand cursor).
                const bool back_label_hover =
                    IsPointInRect(pThis->backtrans_back_label_rect_, x, y) &&
                    !pThis->backtrans_text_.empty();
                if (back_label_hover != pThis->backtrans_back_label_hover_) {
                    pThis->backtrans_back_label_hover_ = back_label_hover;
                    repaint = true;
                }
                const bool ref_label_hover =
                    IsPointInRect(pThis->backtrans_ref_label_rect_, x, y) &&
                    !pThis->backtrans_ref_line_.empty();
                if (ref_label_hover != pThis->backtrans_ref_label_hover_) {
                    pThis->backtrans_ref_label_hover_ = ref_label_hover;
                    repaint = true;
                }
                const bool new_thumb_hover =
                    pThis->scrollable_ && !pThis->dragging_thumb_ &&
                    (IsPointInRect(pThis->scrollbar_thumb_rect_, x, y) ||
                     IsPointInRect(pThis->backtrans_ref_thumb_rect_, x, y));
                if (new_thumb_hover != pThis->thumb_hover_) {
                    pThis->thumb_hover_ = new_thumb_hover;
                    repaint = true;
                }
                if (repaint) {
                    pThis->Render();
                    pThis->UpdateLayered();
                }
                // TrackMouseEvent so WM_MOUSELEAVE arrives and the grace timer
                // can resume the countdown.
                TRACKMOUSEEVENT tme_back = {};
                tme_back.cbSize = sizeof(TRACKMOUSEEVENT);
                tme_back.dwFlags = TME_LEAVE;
                tme_back.hwndTrack = hwnd;
                ::TrackMouseEvent(&tme_back);
                return 0;
            }

            // REQ-002: thumb drag (SetCapture held since WM_LBUTTONDOWN).
            // Relative mapping keeps the grab point under the cursor: pointer
            // travel across the track scales to the scroll range (plan §2.1;
            // overshoot clamps, no jitter, edge cases 2/3). v0.10.9 (U-B):
            // factored into UpdateScrollThumbDrag so the back-translation card
            // runs the identical math against its own shared viewport.
            if (pThis->UpdateScrollThumbDrag(y)) {
                repaint = true;
            }

            int new_hover = 0;
            if (IsPointInRect(pThis->copy_btn_rect_, x, y)) new_hover = 1;
            else if (IsPointInRect(pThis->tts_btn_rect_, x, y)) new_hover = 2;
            // F8 (ADR-A1-4): source button hover (id 5) before the target
            // button so the leftmost header control wins on any overlap.
            else if (IsPointInRect(pThis->src_btn_rect_, x, y)) new_hover = 5;
            // REQ-052 (audit P1): close (id 4) BEFORE target (id 3) - on any
            // residual overlap the dismiss control wins, matching the
            // WM_LBUTTONUP click order (close is checked there before the
            // language menu). The planner clamp keeps an 8 DIP gap between
            // the rects, so this order is defensive-only.
            else if (IsPointInRect(pThis->close_btn_rect_, x, y)) new_hover = 4;
            // V10F L2/L1-B: the header action cluster. Ids 6/7/8 continue the
            // frozen 1..5 numbering (6 was reserved-in-comment by the
            // back-translation card, which keeps its own close-hover flag).
            else if (IsPointInRect(pThis->pin_btn_rect_, x, y)) new_hover = 6;
            else if (IsPointInRect(pThis->edit_btn_rect_, x, y)) new_hover = 7;
            else if (IsPointInRect(pThis->open_btn_rect_, x, y)) new_hover = 8;
            else if (IsPointInRect(pThis->lang_btn_rect_, x, y)) new_hover = 3;

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
            // v0.10.9 (U-B): checked BEFORE the back-translation branch too, so
            // dragging that card's shared thumb cannot arm the grace countdown
            // and close the card under the pointer mid-drag.
            if (pThis->dragging_thumb_) {
                return 0;
            }
            // 261108_0002 (WS3, v0.10.6): leaving the back-translation card
            // resumes the countdown with the 1500 ms grace window.
            if (pThis->is_backtrans_mode_) {
                if (pThis->backtrans_close_hover_ || pThis->thumb_hover_ ||
                    pThis->backtrans_pin_hover_ || pThis->backtrans_back_label_hover_ ||
                    pThis->backtrans_ref_label_hover_) {
                    pThis->backtrans_close_hover_ = false;
                    pThis->thumb_hover_ = false;
                    pThis->backtrans_pin_hover_ = false;
                    pThis->backtrans_back_label_hover_ = false;
                    pThis->backtrans_ref_label_hover_ = false;
                    pThis->Render();
                    pThis->UpdateLayered();
                }
                pThis->ApplyBackTransTimerPlan(TooltipWindow::BackTransTimerEvent::PointerLeft);
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
            // V10F L3-B: the live preview card's only affordance is the ✕
            // (dispatched on UP). A press is inert, so no stale footer/header
            // rect from the previous card can fire under the live run.
            if (pThis->preview_phase_ != TooltipWindow::ChunkPreviewPhase::Inactive) {
                return 0;
            }
            // 261108_0002 (WS3, v0.10.6): the back-translation card's only
            // BUTTON is ✕; a press anywhere else is inert. Dismiss on UP so the
            // press/release pair stays honest.
            //
            // v0.10.9 (U-B, defect 3-1): a press ON THE SHARED SCROLLBAR THUMB
            // is not inert - it starts the same drag the translation card runs,
            // which is what makes "휠/드래그 스크롤" work on this card. Every
            // other press is still swallowed.
            if (pThis->is_backtrans_mode_) {
                const float bx = static_cast<float>(emebalachat::ui::ScalePixelsToDips(
                    static_cast<int>(static_cast<short>(LOWORD(lParam))), pThis->dpi_));
                const float by = static_cast<float>(emebalachat::ui::ScalePixelsToDips(
                    static_cast<int>(static_cast<short>(HIWORD(lParam))), pThis->dpi_));
                pThis->BeginScrollThumbDrag(bx, by);
                return 0;
            }
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
            // v0.10.9 (U-B): the drag state lives in the shared
            // BeginScrollThumbDrag helper, which the back-translation card's
            // WM_LBUTTONDOWN branch above uses too (one implementation, one
            // pair of tests).
            if (!pThis->is_message_mode_ &&
                pThis->BeginScrollThumbDrag(
                    static_cast<float>(emebalachat::ui::ScalePixelsToDips(
                        static_cast<int>(static_cast<short>(LOWORD(lParam))), pThis->dpi_)),
                    static_cast<float>(emebalachat::ui::ScalePixelsToDips(
                        static_cast<int>(static_cast<short>(HIWORD(lParam))), pThis->dpi_)))) {
                return 0;
            }
            break;
        }

        case WM_LBUTTONUP: {
            // 261108_0002 (WS3, v0.10.6): ✕ dismisses the back-translation
            // card immediately (spec). Checked before every other control so a
            // stale footer rect from the previous mode can never fire.
            if (pThis->is_backtrans_mode_) {
                // v0.10.9 (U-B): a shared-thumb drag ends here and swallows the
                // release, exactly like the translation card's path - otherwise
                // releasing over the ✕ would dismiss a card the user was only
                // scrolling.
                if (pThis->dragging_thumb_) {
                    pThis->EndScrollThumbDrag();
                    return 0;
                }
                float bx = static_cast<float>(emebalachat::ui::ScalePixelsToDips(
                    static_cast<int>(static_cast<short>(LOWORD(lParam))), pThis->dpi_));
                float by = static_cast<float>(emebalachat::ui::ScalePixelsToDips(
                    static_cast<int>(static_cast<short>(HIWORD(lParam))), pThis->dpi_));
                // V10F L1-B: the pin toggle (card-level: outside-click and
                // autohide are ignored while it is set).
                if (IsPointInRect(pThis->backtrans_pin_rect_, bx, by)) {
                    const bool next = !pThis->pinned_.load(std::memory_order_relaxed);
                    pThis->pinned_.store(next, std::memory_order_relaxed);
                    DIAG_LOG("UI", "tooltip_pin pinned=%d mode=backtrans", next ? 1 : 0);
                    if (!next && pThis->backtrans_autohide_ms_ > 0) {
                        // Re-arm the countdown the pin suspended, so an
                        // unpinned card behaves exactly like a fresh one.
                        pThis->ApplyBackTransTimerPlan(
                            TooltipWindow::BackTransTimerEvent::Shown);
                    } else {
                        ::KillTimer(hwnd, TooltipWindow::kTimerBackTranslateAutohide);
                    }
                    pThis->Render();
                    pThis->UpdateLayered();
                    return 0;
                }
                // V10F FUP-05: a section label is the split <-> solo toggle.
                if (IsPointInRect(pThis->backtrans_back_label_rect_, bx, by) ||
                    IsPointInRect(pThis->backtrans_ref_label_rect_, bx, by)) {
                    const bool ref_label =
                        IsPointInRect(pThis->backtrans_ref_label_rect_, bx, by);
                    pThis->ApplyBackTransView(pThis->NextBackTransViewForLabel(
                        ref_label ? TooltipWindow::BackTransLabelTarget::Ref
                                  : TooltipWindow::BackTransLabelTarget::Back));
                    return 0;
                }
                if (IsPointInRect(pThis->backtrans_close_rect_, bx, by)) {
                    DIAG_LOG("UI", "tooltip_backtrans_dismiss reason=close_button");
                    pThis->DismissExplicit();
                }
                return 0;
            }
            // REQ-002: end a thumb drag first; swallow the release so a drag
            // that ends over a footer button does not also fire its action.
            if (pThis->dragging_thumb_) {
                pThis->EndScrollThumbDrag();
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

            // ---- V10F L2/L1-B: the header action cluster ----------------
            // Pin: flips the card-level pin (no dismissal, no timer churn -
            // Dismiss() and the autohide arms read the flag).
            if (IsPointInRect(pThis->pin_btn_rect_, x, y)) {
                const bool next = !pThis->pinned_.load(std::memory_order_relaxed);
                pThis->pinned_.store(next, std::memory_order_relaxed);
                DIAG_LOG("UI", "tooltip_pin pinned=%d mode=translation", next ? 1 : 0);
                pThis->Render();
                pThis->UpdateLayered();
                return 0;
            }
            // Edit / retranslate: the P2-G editor hand-off. Guarded null: a
            // tooltip whose callback was never registered (run_tests, a build
            // without the editor) is a no-op instead of a crash.
            if (IsPointInRect(pThis->edit_btn_rect_, x, y)) {
                if (pThis->edit_request_cb_) {
                    DIAG_LOG("UI", "tooltip_edit_request");
                    pThis->edit_request_cb_();
                } else {
                    DIAG_LOG("UI", "tooltip_edit_request ignored reason=no_callback");
                }
                return 0;
            }
            // Open in window: the P2-G editor's standalone surface.
            if (IsPointInRect(pThis->open_btn_rect_, x, y)) {
                if (pThis->open_in_window_cb_) {
                    DIAG_LOG("UI", "tooltip_open_in_window_request");
                    pThis->open_in_window_cb_();
                } else {
                    DIAG_LOG("UI", "tooltip_open_in_window ignored reason=no_callback");
                }
                return 0;
            }

            if (IsPointInRect(pThis->close_btn_rect_, x, y)) {
                // V10F L1-B: the ✕ is an explicit dismissal - it bypasses the
                // pin (DismissExplicit), so a pinned card is never a trap.
                pThis->DismissExplicit();
                return 0;
            }

            // F8 (ADR-A1-4): source-language dropdown. Picking AUTO
            // re-records the persisted drag_source_language as "Auto Detect"
            // via the main.cpp callback (re-record contract, user decision ④ /
            // ADR-A1-4).
            //
            // 261116_0003 P6b: the menu builds from the ACTIVE ENGINE's
            // registry view — GetLanguagesForEngine(engine, include_auto=true)
            // — instead of the global GetSupportedLanguages() list it shipped
            // until P6b (the pre-P6b comment's "menu stays global until P6
            // wires the engine views" is now honored; the P2/P3 registry-growth
            // history lives in the tray's band comments and the test pins).
            // The tray's four language menus already show the engine view
            // (261116_0003 P6); the tooltip must agree with it. The engine is
            // resolved ONCE, at menu-build time, through the resolver seam
            // (derive-at-open, the REQ-056 pattern): the resolver is wired in
            // main.cpp since P6c (PR6-35), and the defensive no-resolver
            // fallback still returns LanguageEngine::Google — the conservative
            // cloud set — mirroring tray.cpp's no-resolver default. AUTO stays
            // a first-class source setting (include_auto = true; "기본값은
            // 자동감지").
            if (IsPointInRect(pThis->src_btn_rect_, x, y)) {
                const MenuEngineResolution menu_engine = pThis->ResolveMenuEngine();
                // 261116_0003 P9b-2: the D5/D7 merge is the dropdown's only
                // data source now - registry rows PLUS the "내 언어" group (empty
                // under the local Hy-MT2 engine, D7; under Google only entries
                // whose code is publisher-declared), with every registry row a
                // user entry claimed removed. The merge is authoritative: this
                // surface adds no filtering of its own. The view local is the
                // build-time AND dispatch-time payload, alive across
                // TrackPopupMenuEx because the group's rows point into
                // menu_engine.user_languages.
                const LanguageMenuView src_menu =
                    BuildLanguageMenuView(menu_engine.engine, /*include_auto=*/true,
                                          menu_engine.user_languages);
                const auto& src_langs = src_menu.registry;
                DIAG_LOG("UI", "tooltip src_menu_open engine=%d count=%zu user=%zu",
                         static_cast<int>(menu_engine.engine), src_langs.size(),
                         src_menu.user_group.size());
                HMENU hMenu = ::CreatePopupMenu();
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
                const std::string norm_tgt = NormalizeLanguageCode(pThis->target_lang_);
                // 261116_0003 P6b: the (surface, role) picker request, latched
                // at menu-build time. It drives BOTH the ghost probe below AND
                // the search row's deferred open in the dispatch — the menu is
                // modal, so the dispatch path cannot re-derive the values (the
                // tray's PR6-03d build-time snapshot contract, mirrored).
                language_palette::PickerInit picker_req;
                picker_req.surface = language_palette::PickerSurface::TooltipPill;
                picker_req.role = language_palette::PickerRole::Source;
                picker_req.engine = menu_engine.engine;
                picker_req.source_code = norm_src;
                picker_req.target_code = norm_tgt;
                // Menu shape per the tray's build_language_menu (261116_0003
                // P6): the "검색…" row first, a separator, then — only when the
                // resolved engine does NOT offer the menu's current source —
                // the pinned-absent GHOST row, then the engine-view rows with
                // their UNCHANGED i + 1 ids.
                ::AppendMenuW(hMenu, MF_STRING, kTooltipCmdSearchSrc,
                              I18n::Get(StringId::MenuSearchLangs).c_str());
                ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                // 261116_0003 P6b fix round (PR6-28), re-anchored to the
                // landed P6c state (PR6-43 sweep): the pinned-absent ghost
                // row — greyed + checked, informational, non-selectable
                // ("설정 유지됨": the config value is preserved verbatim and
                // still honored raw by the engine adapter, it is just not
                // offered by this engine). The capability probe is the
                // picker's OWN tested check (PickerState::PinnedAbsentLanguage)
                // — reused, never re-implemented — and the SUPPRESSION decision
                // is the SHARED router-aware predicate
                // language_palette::SuppressPinnedAbsentGhost: ONE definition
                // in language_search_palette.hpp, consumed by BOTH this
                // surface and the tray's four menus. What the two surfaces
                // share is the DECISION, not the inputs: each side resolves
                // its own is_auto / local_host_available at its own menu-open
                // time, and since P6c (PR6-35) BOTH resolver halves are wired
                // from main.cpp — the tray's and this tooltip's
                // SetEngineResolver — deriving all three values from the same
                // persisted config snapshot. That shared derivation source is
                // why the two surfaces agree in practice; the inputs stay
                // unshared by contract, so each surface keeps feeding its own
                // live view rather than a cached copy.
                // The predicate's preferred_engine is the tray's collapsed index,
                // mapped here zero-vs-nonzero from the resolved
                // LanguageEngine — only the Google view (index 0, covering
                // both "google" and "auto") can ever suppress.
                // The label keeps the tooltip's own row form (name_en
                // (name_native)), matching the language rows below —
                // deliberately NOT the tray's "CODE - display" form.
                const LanguageInfo* ghost =
                    language_palette::PickerState{picker_req}.PinnedAbsentLanguage();
                // 261116_0003 P9b-2 fix R14-01: capture the registry half's
                // PRE-SUPPRESSION answer; the user half below runs only on
                // this null (the same contract as the tray's four menus).
                const bool registry_ghost_found = ghost != nullptr;
                const int pref_equiv = (menu_engine.engine == LanguageEngine::Google) ? 0 : 1;
                if (ghost != nullptr &&
                    language_palette::SuppressPinnedAbsentGhost(
                        picker_req, pref_equiv, menu_engine.is_auto,
                        menu_engine.local_host_available)) {
                    // Same predicate as the tray's four menus (PR6-28); the
                    // inputs here are this surface's own resolver view
                    // (wired in P6c, PR6-35), resolved live at menu-open
                    // time like the tray's: one decision, unshared inputs.
                    ghost = nullptr;
                }
                if (ghost != nullptr) {
                    const std::wstring ghost_text =
                        ToUtf16(ghost->name_en) + L" (" + ToUtf16(ghost->name_native) + L")";
                    ::AppendMenuW(hMenu, MF_STRING | MF_GRAYED | MF_CHECKED, kTooltipCmdGhostSrc,
                                  ghost_text.c_str());
                } else if (!registry_ghost_found) {
                    // 261116_0003 P9b-2: the USER half of the same ghost (see
                    // tray.cpp): same row slot, same command id, same
                    // greyed+checked contract, and the same R14-01 gate:
                    // reached only when the registry half found no ghost
                    // BEFORE the shared suppression gate decides. The helper
                    // answers from the merge, and the label is the user row
                    // form (no code prefix) so it reads as the pinned entry.
                    if (const UserLanguage* user_ghost =
                            PinnedAbsentUserLanguage(src_menu, menu_engine.user_languages,
                                                     pThis->source_lang_code_)) {
                        ::AppendMenuW(hMenu, MF_STRING | MF_GRAYED | MF_CHECKED,
                                      kTooltipCmdGhostSrc,
                                      ToUtf16(FormatUserLanguageMenuLabel(*user_ghost)).c_str());
                    }
                }
                // 261116_0003 P9b-2 (plan §3 D5): the "내 언어" group, below the
                // search/ghost rows and above the registry rows, rendered only
                // when the merge offers user entries. DISABLED id-0 header (the
                // P9b-1 string, not a command); rows carry this dropdown's own
                // user band. The check mark comes from the same ONE shared
                // pin-to-row resolution the tray's four menus call (fix R14-02,
                // tray.hpp CurrentUserLanguageRow), over this dropdown's own
                // merged group, and the answer feeds the registry rule below as
                // `user_row_checked`, so a setting never lights two rows.
                const UserLanguage* current_user =
                    CurrentUserLanguageRow(src_menu, pThis->source_lang_code_);
                const bool user_row_checked = current_user != nullptr;
                if (!src_menu.user_group.empty()) {
                    ::AppendMenuW(hMenu, MF_STRING | MF_DISABLED, 0,
                                  I18n::Get(StringId::UserLangsGroup).c_str());
                    const size_t user_count = (std::min)(
                        src_menu.user_group.size(), static_cast<size_t>(kMaxUserLanguages));
                    for (size_t i = 0; i < user_count; ++i) {
                        const UserLanguage* entry = src_menu.user_group[i].user;
                        if (entry == nullptr) {
                            continue;
                        }
                        UINT flags = MF_STRING;
                        if (entry == current_user) {
                            flags |= MF_CHECKED;
                        }
                        // 261116_0003 P9b-3 U4a: the probe badge rides the row
                        // through the ONE shared fold (tray.hpp), keyed by
                        // this dropdown's own resolver view — the same
                        // derive-at-open (engine, model) key the tray's four
                        // submenus read, so one verdict renders identically on
                        // all six surfaces. The in-flight seam is the
                        // resolver's per-entry predicate (EMPTY = no resolver
                        // -> not in flight). Display-only: the check mark
                        // above and the registry rule below are untouched.
                        const bool probe_in_flight =
                            menu_engine.user_lang_probe_in_flight &&
                            menu_engine.user_lang_probe_in_flight(*entry);
                        const std::wstring item_text =
                            ToUtf16(FormatUserLanguageMenuLabel(*entry)) +
                            UserLanguageMenuBadgeSuffix(*entry,
                                                        menu_engine.user_lang_probe_engine,
                                                        menu_engine.user_lang_probe_model,
                                                        probe_in_flight);
                        ::AppendMenuW(hMenu, flags,
                                      TooltipUserLangCommand(TooltipUserLangRole::Source, i),
                                      item_text.c_str());
                    }
                    ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                }
                for (size_t i = 0; i < src_langs.size(); ++i) {
                    std::wstring item = ToUtf16(src_langs[i]->name_en) + L" (" + ToUtf16(src_langs[i]->name_native) + L")";
                    UINT flags = MF_STRING;
                    // 261116_0003 P9b-2: the shared check-mark rule (an
                    // unresolvable pin, e.g. a code-less user language,
                    // normalizes to AUTO and must not light the AUTO row as
                    // well as its own "내 언어" row; fix R14-02 passes the
                    // user-row answer so an AUTO-spelled entry cannot either).
                    if (IsRegistryMenuRowCurrent(src_langs[i]->code, norm_src,
                                                 pThis->source_lang_code_,
                                                 user_row_checked)) {
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
                // 260922_0001 A5: back up the window that owned the foreground
                // before the menu takes it, so focus can be restored after the
                // modal TrackPopupMenuEx loop (focus loss on language change).
                HWND hwndPrev = ::GetForegroundWindow();
                ::SetForegroundWindow(hwnd);

                int cmd = ::TrackPopupMenuEx(hMenu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, hwnd, nullptr);
                ::DestroyMenu(hMenu);

                // 260922_0001 A5: best-effort focus restore (return value ignored -
                // the tooltip is WS_EX_NOACTIVATE, so a failure degrades to the
                // previous behaviour). IsWindow guards a window destroyed while the
                // modal loop was running; a prev == hwnd case needs no restore.
                if (hwndPrev && hwndPrev != hwnd && ::IsWindow(hwndPrev)) {
                    ::SetForegroundWindow(hwndPrev);
                }

                // 261116_0003 P6b: the dedicated arms come FIRST, so the
                // search/ghost ids can never reach the language range check
                // below (their ids live in the >=2701 band, outside i + 1 by
                // construction; the ordering makes that structural).
                if (cmd == kTooltipCmdSearchSrc) {
                    // 261116_0003 P6b (D3 + REQ-047 D3): the "검색…" row defers
                    // the picker open — the latched request is posted to THIS
                    // window (the tooltip is its own controller for its two
                    // picker surfaces) and the WndProc's kMsgOpenLanguagePalette
                    // arm opens the modal picker after the menu tear-down
                    // settles. Same PR6-08 GLE discipline as the tray's search
                    // arm: clear the stale error first; a false return is
                    // cause-neutral (coalesced / rejected / genuinely failed —
                    // only the GLE branch is decidable).
                    // PR6-32, re-anchored to the landed P6c state (PR6-43
                    // sweep): the latch is PER-SURFACE and this post targets
                    // the pill slot, so a "coalesced" false can only mean THIS
                    // surface's own earlier request is still pending (first
                    // one wins — the popup is single-instance per surface by
                    // design); it can no longer mean an unconsumed tray
                    // request, because the tray's requests live in the two
                    // slots only main.cpp's controller arm consumes (wired in
                    // P6c). The picker module logs the specific reason either
                    // way.
                    ::SetLastError(ERROR_SUCCESS);
                    if (!PostDeferredLanguagePaletteOpen(hwnd, picker_req)) {
                        const DWORD post_gle = ::GetLastError();
                        if (post_gle != ERROR_SUCCESS) {
                            DIAG_F("TT/Picker/001: deferred language-picker open post failed (GLE %lu)\n",
                                   post_gle);
                        } else {
                            DIAG_F("TT/Picker/001: deferred language-picker open not posted "
                                   "(coalesced or rejected; the picker module logs the specific reason)\n");
                        }
                    }
                } else if (cmd == kTooltipCmdGhostSrc) {
                    // 261116_0003 P6b: documented NO-OP arm (the tray's
                    // ID_TRAY_GHOST_* contract). The ghost row is appended
                    // MF_GRAYED, so the OS never dispatches it; this arm exists
                    // so the id is not silently unhandled if a future change
                    // ever makes it selectable (designer §4.6 floats "choosing
                    // the ghost row = explicit keep-as-is confirmation"; that
                    // decision is NOT made here). No config write, no
                    // callback, no picker open.
                } else if (cmd >= kTooltipCmdUserSrcBase && cmd < kTooltipCmdUserTgtBase) {
                    // 261116_0003 P9b-2: the "내 언어" row. The command decodes
                    // to (role, user index); the index selects the row from the
                    // SAME merged view the menu was built from, and the payload
                    // is the entry's STORED name (the form PlanLanguageSync
                    // canonicalizes to), i.e. the identical shape a registry
                    // pick sends. A stale/out-of-range index dispatches nothing.
                    const TooltipUserLangCommandRef user_cmd =
                        DecodeTooltipUserLangCommand(cmd);
                    if (user_cmd.role == TooltipUserLangRole::Source &&
                        user_cmd.user_index < src_menu.user_group.size() &&
                        src_menu.user_group[user_cmd.user_index].user != nullptr) {
                        const std::string new_src = UserLanguageStoredName(
                            *src_menu.user_group[user_cmd.user_index].user);
                        // 261116_0003 P9b-2 fix R14-06: the user-typed stored
                        // name is content, so REQ-003 records it only behind
                        // diag::ContentLoggingEnabled() (default off); the
                        // else branch keeps the shape-only len= field.
                        if (diag::ContentLoggingEnabled()) {
                            DIAG_LOG("UI", "tooltip src_menu_select_user lang=%s",
                                     new_src.c_str());
                        } else {
                            DIAG_LOG("UI", "tooltip src_menu_select_user len=%zu",
                                     new_src.size());
                        }
                        if (pThis->src_lang_change_cb_) {
                            pThis->src_lang_change_cb_(new_src);
                        }
                    }
                } else if (cmd > 0 && static_cast<size_t>(cmd - 1) < src_langs.size()) {
                    // name_en payload (the AUTO entry yields "Auto Detect") -
                    // unified with the target menu's payload form (ADR-A1-4).
                    std::string new_src = src_langs[cmd - 1]->name_en;
                    DIAG_LOG("UI", "tooltip src_menu_select lang=%s",
                             new_src.c_str());
                    if (pThis->src_lang_change_cb_) {
                        pThis->src_lang_change_cb_(new_src);
                    }
                }
                return 0;
            }

            if (IsPointInRect(pThis->lang_btn_rect_, x, y)) {
                // Language switcher dropdown menu.
                //
                // 261116_0003 P6b: the same engine-view build as the source
                // menu above — GetLanguagesForEngine(menu_engine.engine, include_auto=
                // false) (AUTO can never be a translation target) instead of
                // the global GetTargetLanguages() list. The engine is resolved
                // ONCE, at menu-build time, through the resolver seam (wired
                // in main.cpp since P6c, PR6-35; the MenuEngineResolution{}
                // fallback below is the defensive no-resolver default).
                // 261116_0003 P9b-2 fix R14-03: the check-mark rule is the
                // SOURCE menu's rule, the shared IsRegistryMenuRowCurrent over
                // the normalized code (the legacy raw `name_en == pin || code
                // == pin` comparison is gone: it missed a native, alias or
                // case-variant pin that the tray's drag-target menu lights).
                HMENU hMenu = ::CreatePopupMenu();
                const MenuEngineResolution menu_engine = pThis->ResolveMenuEngine();
                // 261116_0003 P9b-2: the same D5/D7 merge as the source menu
                // above (include_auto = false: AUTO can never be a target), and
                // the same build-time AND dispatch-time payload discipline.
                const LanguageMenuView tgt_menu =
                    BuildLanguageMenuView(menu_engine.engine, /*include_auto=*/false,
                                          menu_engine.user_languages);
                const auto& target_langs = tgt_menu.registry;
                DIAG_LOG("UI", "tooltip tgt_menu_open engine=%d count=%zu user=%zu",
                         static_cast<int>(menu_engine.engine), target_langs.size(),
                         tgt_menu.user_group.size());
                const std::string norm_src =
                    pThis->source_lang_code_.empty()
                        ? std::string("AUTO")
                        : NormalizeLanguageCode(pThis->source_lang_code_);
                const std::string norm_tgt = NormalizeLanguageCode(pThis->target_lang_);
                // 261116_0003 P6b: the (surface, role) picker request, latched
                // at menu-build time — same snapshot contract as the source
                // menu (PR6-03d). Role = Target: the probe checks the target
                // code, and the dispatch routes a committed pick through the
                // target callback.
                language_palette::PickerInit picker_req;
                picker_req.surface = language_palette::PickerSurface::TooltipPill;
                picker_req.role = language_palette::PickerRole::Target;
                picker_req.engine = menu_engine.engine;
                picker_req.source_code = norm_src;
                picker_req.target_code = norm_tgt;
                // Menu shape per the tray's build_language_menu: the "검색…"
                // row first, a separator, then the optional pinned-absent
                // ghost row (same "설정 유지됨" contract, same reused
                // PinnedAbsentLanguage capability probe, same SHARED
                // SuppressPinnedAbsentGhost suppression gate and tooltip row
                // form as the source menu's ghost — see the comment there:
                // ONE predicate definition consumed by both surfaces, each
                // feeding its own resolver view — wired in P6c, PR6-35),
                // then the engine-view rows with their UNCHANGED i + 1 ids.
                ::AppendMenuW(hMenu, MF_STRING, kTooltipCmdSearchTgt,
                              I18n::Get(StringId::MenuSearchLangs).c_str());
                ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                const LanguageInfo* ghost =
                    language_palette::PickerState{picker_req}.PinnedAbsentLanguage();
                // 261116_0003 P9b-2 fix R14-01: capture the registry half's
                // PRE-SUPPRESSION answer; the user half below runs only on
                // this null (the same contract as the source menu).
                const bool registry_ghost_found = ghost != nullptr;
                const int pref_equiv = (menu_engine.engine == LanguageEngine::Google) ? 0 : 1;
                if (ghost != nullptr &&
                    language_palette::SuppressPinnedAbsentGhost(
                        picker_req, pref_equiv, menu_engine.is_auto,
                        menu_engine.local_host_available)) {
                    // Same shared gate as the source menu and the tray
                    // (PR6-28); this surface's resolver view is wired in P6c
                    // (PR6-35).
                    ghost = nullptr;
                }
                if (ghost != nullptr) {
                    const std::wstring ghost_text =
                        ToUtf16(ghost->name_en) + L" (" + ToUtf16(ghost->name_native) + L")";
                    ::AppendMenuW(hMenu, MF_STRING | MF_GRAYED | MF_CHECKED, kTooltipCmdGhostTgt,
                                  ghost_text.c_str());
                } else if (!registry_ghost_found) {
                    // 261116_0003 P9b-2: the USER half of the same ghost (see
                    // the source menu above and tray.cpp), under the same
                    // R14-01 gate: reached only when the registry half found
                    // no ghost BEFORE the shared suppression gate decides.
                    if (const UserLanguage* user_ghost =
                            PinnedAbsentUserLanguage(tgt_menu, menu_engine.user_languages,
                                                     pThis->target_lang_)) {
                        ::AppendMenuW(hMenu, MF_STRING | MF_GRAYED | MF_CHECKED,
                                      kTooltipCmdGhostTgt,
                                      ToUtf16(FormatUserLanguageMenuLabel(*user_ghost)).c_str());
                    }
                }
                // 261116_0003 P9b-2: the "내 언어" group, same shape and
                // position as the source menu's (header, rows, separator, then
                // the registry rows). Fix R14-02: the check mark comes from the
                // same shared pin-to-row resolution over THIS dropdown's merged
                // group, and its answer feeds the registry rule below.
                const UserLanguage* current_user =
                    CurrentUserLanguageRow(tgt_menu, pThis->target_lang_);
                const bool user_row_checked = current_user != nullptr;
                if (!tgt_menu.user_group.empty()) {
                    ::AppendMenuW(hMenu, MF_STRING | MF_DISABLED, 0,
                                  I18n::Get(StringId::UserLangsGroup).c_str());
                    const size_t user_count = (std::min)(
                        tgt_menu.user_group.size(), static_cast<size_t>(kMaxUserLanguages));
                    for (size_t i = 0; i < user_count; ++i) {
                        const UserLanguage* entry = tgt_menu.user_group[i].user;
                        if (entry == nullptr) {
                            continue;
                        }
                        UINT flags = MF_STRING;
                        if (entry == current_user) {
                            flags |= MF_CHECKED;
                        }
                        // 261116_0003 P9b-3 U4a: the same ONE shared badge fold
                        // as the source dropdown above and the tray's four
                        // submenus — same resolver key, same display-only
                        // contract (see the source menu's comment).
                        const bool probe_in_flight =
                            menu_engine.user_lang_probe_in_flight &&
                            menu_engine.user_lang_probe_in_flight(*entry);
                        const std::wstring item_text =
                            ToUtf16(FormatUserLanguageMenuLabel(*entry)) +
                            UserLanguageMenuBadgeSuffix(*entry,
                                                        menu_engine.user_lang_probe_engine,
                                                        menu_engine.user_lang_probe_model,
                                                        probe_in_flight);
                        ::AppendMenuW(hMenu, flags,
                                      TooltipUserLangCommand(TooltipUserLangRole::Target, i),
                                      item_text.c_str());
                    }
                    ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                }
                for (size_t i = 0; i < target_langs.size(); ++i) {
                    std::wstring item = ToUtf16(target_langs[i]->name_en) + L" (" + ToUtf16(target_langs[i]->name_native) + L")";
                    UINT flags = MF_STRING;
                    // 261116_0003 P9b-2 fix R14-03: the SAME shared rule and
                    // the same arguments as the source loop above and the
                    // tray's drag-target menu (normalized code + raw token for
                    // the AUTO arm + the user-row fact), so one pin spelling
                    // shows one identical check state on both surfaces. The
                    // legacy raw comparison this replaces missed every native,
                    // alias or case-variant spelling the normalized lookup
                    // resolves.
                    if (IsRegistryMenuRowCurrent(target_langs[i]->code, norm_tgt,
                                                 pThis->target_lang_,
                                                 user_row_checked)) {
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
                // 260922_0001 A5: back up the window that owned the foreground
                // before the menu takes it, so focus can be restored after the
                // modal TrackPopupMenuEx loop (focus loss on language change).
                HWND hwndPrev = ::GetForegroundWindow();
                ::SetForegroundWindow(hwnd);

                int cmd = ::TrackPopupMenuEx(hMenu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, hwnd, nullptr);
                ::DestroyMenu(hMenu);

                // 260922_0001 A5: best-effort focus restore (return value ignored -
                // the tooltip is WS_EX_NOACTIVATE, so a failure degrades to the
                // previous behaviour). IsWindow guards a window destroyed while the
                // modal loop was running; a prev == hwnd case needs no restore.
                if (hwndPrev && hwndPrev != hwnd && ::IsWindow(hwndPrev)) {
                    ::SetForegroundWindow(hwndPrev);
                }

                // 261116_0003 P6b: the dedicated arms come FIRST, before the
                // language range arm (same ordering contract as the source
                // menu above).
                if (cmd == kTooltipCmdSearchTgt) {
                    // Same deferred open as the source menu (see that comment):
                    // the request is posted to THIS window; the WndProc's
                    // kMsgOpenLanguagePalette arm consumes it. Same PR6-08 GLE
                    // discipline (clear first, cause-neutral false label).
                    // PR6-32 honesty re-anchored (PR6-43 sweep): the latch is
                    // per-surface, so a coalesced false can only mean THIS
                    // surface's own pending request (first one wins); the
                    // tray's slots are the controller arm's alone since P6c.
                    ::SetLastError(ERROR_SUCCESS);
                    if (!PostDeferredLanguagePaletteOpen(hwnd, picker_req)) {
                        const DWORD post_gle = ::GetLastError();
                        if (post_gle != ERROR_SUCCESS) {
                            DIAG_F("TT/Picker/002: deferred language-picker open post failed (GLE %lu)\n",
                                   post_gle);
                        } else {
                            DIAG_F("TT/Picker/002: deferred language-picker open not posted "
                                   "(coalesced or rejected; the picker module logs the specific reason)\n");
                        }
                    }
                } else if (cmd == kTooltipCmdGhostTgt) {
                    // 261116_0003 P6b: documented NO-OP arm (the tray's
                    // ID_TRAY_GHOST_* contract; see the source menu's arm).
                } else if (cmd >= kTooltipCmdUserTgtBase && cmd < kTooltipCmdUserBandEnd) {
                    // 261116_0003 P9b-2: the target "내 언어" row. Same decode
                    // and stored-name payload as the source menu's arm; the
                    // coordinator (ApplyLanguageChange) is the single apply
                    // authority and now validates against the user registry.
                    const TooltipUserLangCommandRef user_cmd =
                        DecodeTooltipUserLangCommand(cmd);
                    if (user_cmd.role == TooltipUserLangRole::Target &&
                        user_cmd.user_index < tgt_menu.user_group.size() &&
                        tgt_menu.user_group[user_cmd.user_index].user != nullptr) {
                        const std::string new_lang = UserLanguageStoredName(
                            *tgt_menu.user_group[user_cmd.user_index].user);
                        // 261116_0003 P9b-2 fix R14-06: same REQ-003 content
                        // gate as the source arm above (shape-only len= when
                        // the gate is off).
                        if (diag::ContentLoggingEnabled()) {
                            DIAG_LOG("UI", "tooltip tgt_menu_select_user lang=%s",
                                     new_lang.c_str());
                        } else {
                            DIAG_LOG("UI", "tooltip tgt_menu_select_user len=%zu",
                                     new_lang.size());
                        }
                        if (pThis->lang_change_cb_) {
                            pThis->lang_change_cb_(new_lang);
                        }
                    }
                } else if (cmd > 0 && static_cast<size_t>(cmd - 1) < target_langs.size()) {
                    std::string new_lang = target_langs[cmd - 1]->name_en;
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

        // 261108_0002 (WS3, v0.10.6): ESC dismisses the back-translation card
        // immediately. The window is WS_EX_NOACTIVATE so it never holds focus;
        // the global ESC path (main.cpp's hook esc_cb_ -> Dismiss) also covers
        // this mode, and this handler is the in-window belt-and-braces case
        // (e.g. a future activating variant).
        case WM_KEYDOWN: {
            if (pThis->is_backtrans_mode_ && wParam == VK_ESCAPE) {
                DIAG_LOG("UI", "tooltip_backtrans_dismiss reason=esc");
                pThis->Dismiss();
                return 0;
            }
            break;
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
            // 261108_0002 (WS3, v0.10.6): auto-dismiss or the hover grace
            // expiry - both land here; the card is hidden either way. A timer
            // that fires after the card was replaced is harmless: the mode flag
            // is already false and Dismiss is a no-op for an invisible window.
            // v0.10.9 (U-B, defect 3-5c): the armed delay is the ADAPTIVE one
            // (4500..12000 ms by content length) and every wheel/drag scroll
            // re-arms it, so this handler's job is unchanged.
            if (wParam == kTimerBackTranslateAutohide) {
                ::KillTimer(hwnd, kTimerBackTranslateAutohide);
                if (pThis->pinned_.load(std::memory_order_relaxed)) {
                    // V10F L1-B: a pinned card is immune to autohide. The
                    // timer is already dead above, so the card simply stays.
                    DIAG_LOG("UI", "tooltip_backtrans_autohide ignored reason=pinned");
                    return 0;
                }
                if (pThis->is_backtrans_mode_) {
                    DIAG_LOG("UI", "tooltip_backtrans_dismiss reason=autohide");
                    pThis->Dismiss();
                }
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
