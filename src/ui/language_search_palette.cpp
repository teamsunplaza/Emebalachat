// 261116_0003 P5 (language-menu redesign, plan §3 D3 + §4 "Search" + §5 P5):
// the Win32 half of the shared language search picker. SECTION A (the pure
// logic) is header-inline in language_search_palette.hpp; this TU owns the ONLY
// window-creating code in the module.
//
// Mechanism (plan D3): WS_POPUP + cue-banner EDIT + LISTBOX — standard controls
// the OS draws and the shell font, NOT a Direct2D card. Both existing language
// pickers are TrackPopupMenuEx HMENUs (tooltip.cpp:2881-2995, tray.cpp:752-805),
// and a Win32 popup menu cannot host an edit control, so one shared small window
// is the only mechanism that serves the tray and the tooltip identically
// (designer report §2).
//
// The modal loop is deliberately hand-rolled rather than DialogBoxIndirectParamW:
// the picker needs ↑↓ to move the LISTBOX selection while the EDIT keeps the
// caret (a real dialog would need a subclass to intercept them), and Tab has to
// mean "flip role", never "move focus". Pre-translating those four keys in the
// loop keeps both controls stock. Everything else (typing, IME composition,
// Backspace, Home/End, mouse) is the EDIT's and the LISTBOX's own behaviour.
//
// Headless discipline (AGENTS.md §10): nothing here runs unless a user opens the
// picker — no static initializer, no timer, no window at module load. The unit
// suite includes only SECTION A.

#include "ui/language_search_palette.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "diag_logger.hpp"
#include "dpi.hpp"
#include "i18n.hpp"
#include "unicode_utils.hpp"

// REQ-050 (cue banners): EM_SETCUEBANNER (0x1501, ECM_FIRST + 1) ships in the
// Windows SDK, but the OpenAI settings dialog already guards the #define the
// same way — kept for SDK-version independence (and so the grep for the cue
// idiom finds both call sites).
#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

namespace emebalachat {

namespace {

// ---- REQ-047 D3 deferred open: the value-only latch ------------------------
// 261116_0003 P6c (PR6-32): the latch is PER-SURFACE — one fixed slot per
// PickerSurface value — so no surface can starve another. Root basis: with the
// old module-global single slot, the FIRST "검색…" click of a process latched
// permanently (nothing consumed it until P6c), and even with a consumer a
// tray click followed by a tooltip click LOST the tooltip's request (its post
// coalesced against the tray's pending slot). Each slot keeps the original
// single-instance discipline: a second post on the SAME surface while one is
// pending still coalesces (first one wins, designer §4.2 "reopening
// replaces").
// GUI-THREAD-ONLY state, the same reasoning as main.cpp's
// s_openai_dialog_pending (design §C.2 안 C-1 권장 저장소 (a)): the producers
// are the tray/tooltip dispatch and the consumers are the controller/tooltip
// WndProcs, all pumping on the ONE GUI thread, so a plain bool + value slot
// per surface is race-free without a mutex.
// The slot index IS the enum value, bounds-checked at both entry points
// (PickerSlotFor): an out-of-range surface can neither alias a real slot nor
// leak a request into one. The array is value-only (PickerInit holds std::string
// members, so slots reset to PickerInit{} release any held bytes; nothing
// pointer-shaped ever lives here).
// 261119_0001 U3 (UI-language-89 phase 2): FOUR surfaces now — the UI-language
// mode's PickerSurface::TrayUiLanguage takes slot 3. The slot index IS the enum
// value, so the new enum value needs the new slot; PickerSlotFor's bounds check
// (idx >= kPickerSlotCount) is unchanged and still closes the array.
constexpr std::size_t kPickerSlotCount = 4; // PickerSurface: TrayTyping / TrayTooltip / TooltipPill / TrayUiLanguage
bool s_open_pending[kPickerSlotCount] = {};
language_palette::PickerInit s_pending_init[kPickerSlotCount];

// 261116_0003 P6c fix round (PR6-42): the single-instance guard for the modal
// picker, hoisted from a function-local static inside ShowLanguageSearchPalette
// to module scope so LanguagePickerShowing() can expose it. Same
// GUI-THREAD-ONLY discipline as the latch arrays above for the WRITE side
// (the modal loop, both deferred-open arms).
// V10F FUP-02: the flag is now std::atomic<bool> — main.cpp's ESC and
// dismissal callbacks on the keyboard/mouse HOOK threads read it while the
// picker is modal (acquire/release; the state is a lone bool, no payload
// travels with the ordering). One picker process-wide is the design (the tray
// and the tooltip share the module).
std::atomic<bool> s_palette_open{false};

// Defensive surface->slot mapping. The enum is closed (four values, zero
// holes) and every producer passes a literal, so the false branch is
// unreachable in practice; it exists so a corrupted/unknown tag can never
// index past the array (CWE-129-style guard on a by-value enum crossing a
// module boundary).
bool PickerSlotFor(language_palette::PickerSurface surface, std::size_t* slot) {
    const auto idx = static_cast<unsigned int>(surface);
    if (idx >= kPickerSlotCount) {
        return false;
    }
    *slot = static_cast<std::size_t>(idx);
    return true;
}

// ---- window class --------------------------------------------------------
// V10F FUP-02: the class string itself lives in SECTION A of the header (one
// shared definition, consumed by the pure IsLanguagePaletteWindowClass
// predicate); this alias keeps the three Win32 use sites below unchanged.
constexpr const wchar_t* kPaletteClassName =
    language_palette::kLanguagePaletteWindowClass;

// WM_COMMAND / child control ids (module-local: the picker owns its own window,
// so it must not collide with the tray's 2001..2600+ menu band or the settings
// dialogs' IDC_* ranges).
enum : int {
    kIdCue = 4001,     // the cue-banner EDIT
    kIdList = 4002,    // the LISTBOX
    kIdNoMatch = 4003, // the "no matching language" STATIC (shown only when empty)
    kIdHeader = 4004,  // the role header STATIC ("출발 언어 (입력 언어)")
    kIdChip = 4005,    // 261116_0003 P7: the engine chip + count STATIC
};

// DIP layout (designer report §3: 372 DIP wide, 34 px result rows, 38 px
// search field, the same scale tooltip.cpp already uses).
constexpr int kWidthDips = 372;
constexpr int kPadDips = 10;
constexpr int kHeaderDips = 16;
// 261116_0003 P7: the engine chip + count line sits UNDER the role label in
// the same header band. It gets its own 16 DIP row instead of sharing the role
// line: the localized engine names run long (Finnish "Sisäänrakennettu
// paikallinen moottori" is 37 chars, and MenuSourceLang/MenuTargetLang already
// carry a parenthetical), and 352 DIP of inner width holds one long line, not
// two. A clipped chip would defeat the line's purpose (naming the list's
// engine), so the band grows by one row and every height consumer below adds
// kHeaderChipDips alongside kHeaderDips.
constexpr int kHeaderChipDips = 16;
constexpr int kHeaderGapDips = 4;
constexpr int kCueDips = 38;
constexpr int kGapDips = 8;
constexpr int kRowDips = 34;     // fallback row height (see RowHeightPx)
constexpr int kRowsVisible = 9;  // ~9.5 rows before the list scrolls

// One open popup's whole state. Heap-allocated per invocation, published
// through GWLP_USERDATA (the about_window/tooltip idiom) and deleted by the
// entry point after the modal loop returns — so a posted message that outlives
// the window can never reach a dangling pointer via the WndProc.
struct PaletteWindow {
    explicit PaletteWindow(const language_palette::PickerInit& init) : state(init) {}

    ~PaletteWindow() {
        if (font) {
            ::DeleteObject(font);
        }
    }

    HWND hwnd = nullptr;
    HWND header = nullptr;
    HWND chip = nullptr;
    HWND cue = nullptr;
    HWND list = nullptr;
    HWND no_match = nullptr;
    HFONT font = nullptr;
    UINT dpi = ui::kDpiBase;
    language_palette::PickerState state;
    bool done = false;              // the modal loop's exit flag
    bool deactivate_dismiss = false; // armed once the popup has been ACTIVE
    // The committed row (empty while nothing was committed).
    std::string committed_code;
    std::string committed_name_en;
    bool committed = false;
};

PaletteWindow* FromWindow(HWND hwnd) {
    return reinterpret_cast<PaletteWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// The shell UI font (Segoe UI 9pt on Win10/11), scaled to the monitor's DPI.
// Using NONCLIENTMETRICS instead of a hardcoded family keeps the picker visually
// identical to every other native dialog on the machine and to the tray menu;
// the height is a DIP->px scale of the system-provided character height, so a
// 144 dpi monitor gets 1.5x text instead of a DWM-stretched bitmap.
// Returns nullptr when the metric is unavailable — the caller then keeps its
// CURRENT font (the system default on the first apply, the previous scale's
// font on a DPI change) rather than inventing a size.
HFONT CreatePaletteFont(UINT dpi) {
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    if (!::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        return nullptr;
    }
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -ui::ScaleDipsToPixels(-lf.lfHeight, dpi);
    lf.lfQuality = CLEARTYPE_QUALITY;
    return ::CreateFontIndirectW(&lf);
}

// Row text: the flat menus' own shape ("KO - 한국어 (Korean)"), so a row the
// user already recognises in the menu is the same row here. Reuses
// GetLanguageDisplayName — zero new i18n keys, and the native/English order
// follows the existing per-locale convention inside it.
//
// 261119_0001 U3 (UI-language mode): the UI pool's row 0 is the registry AUTO
// row, whose registry display name is the TRANSLATION auto-detect label —
// wrong chrome for a UI-locale list. That ONE row renders the selector's own
// MenuUiLanguageAuto label instead; every other row, in every mode, keeps the
// existing shape verbatim.
std::wstring RowText(const LanguageInfo& language, bool ui_mode) {
    if (ui_mode && language.code == "AUTO") {
        return I18n::Get(StringId::MenuUiLanguageAuto);
    }
    return ToUtf16(language.code) + L" - " + I18n::GetLanguageDisplayName(language.code);
}

// 261116_0003 P7: the engine name for the header chip. REUSES the four
// engine-name StringIds the tray's engine submenu already shows (no new
// engine-name strings): a user who picked the engine in the menu reads the same
// words in the picker's header. The four CONCRETE engines keep that mapping.
// AUTO is the P9b-3 U5 exception (CEO decision 1.b, decisions.md:540): mapping
// Auto to the LOCAL name rendered "내장 로컬 엔진 · N개 언어", a half-truth that
// hid AUTO's cloud leg, so Auto now carries its own label
// (StringId::LanguageSearchAutoChip) and no count (see UpdateHeader).
//
// The descriptive parenthetical is dropped at this call site the same way the
// tray's own szTip budget drops it (TrayEngineLabel): the full stems are 40+
// chars ("Google 번역 (무료 / 무설치 / 실시간)"), which cannot fit a 352-DIP
// chip row, and the parenthetical is a menu-row selling point rather than news
// once the engine is already chosen.
std::wstring EngineChipName(LanguageEngine engine) {
    // 261116_0003 P7 fix PR7-12: no default arm on purpose. The id initializer
    // below is the actual corrupted-value guard (an unmatched enum value keeps
    // the Local name); compile-time flagging of a future enumerator is
    // toolchain-dependent (MSVC /W4 does not enable the C4061/C4062
    // switch-coverage warnings).
    StringId id = StringId::MenuEngineLocal;
    switch (engine) {
        case LanguageEngine::Google:
            id = StringId::MenuEngineGoogle;
            break;
        case LanguageEngine::OpenAi:
            id = StringId::MenuEngineOpenAi;
            break;
        case LanguageEngine::UserGguf:
            id = StringId::MenuEngineUserGguf;
            break;
        case LanguageEngine::HyMt2:
            id = StringId::MenuEngineLocal;
            break;
        case LanguageEngine::Auto: // 261116_0003 P9b-3 U5 (CEO decision 1.b)
            id = StringId::LanguageSearchAutoChip;
            break;
    }
    std::wstring name = I18n::Get(id);
    const std::size_t paren = name.find(L" (");
    // 261116_0003 P7 fix PR7-10: npos or a leading " (" keeps the full string
    // as the chip name (no empty chip from a degenerate engine label).
    if (paren != std::wstring::npos && paren > 0) {
        name.resize(paren);
    }
    return name;
}

// 261116_0003 P7: the localized count phrase with its number substituted.
// i18n.cpp has no printf/format layer (and its "{s}" tokens are replaced at
// each call site), so LanguageSearchCount carries the number as "{n}" and the
// swap happens here. A locale that somehow lost the token still shows a count
// (number appended) rather than silently dropping it; the P7 test pins the
// token in all 37 locales, so that branch is a defensive one.
std::wstring CountPhrase(std::size_t count) {
    const std::wstring number = std::to_wstring(count);
    std::wstring phrase = I18n::Get(StringId::LanguageSearchCount);
    const std::wstring token = L"{n}";
    const std::size_t at = phrase.find(token);
    if (at == std::wstring::npos) {
        return phrase + L" " + number;
    }
    phrase.replace(at, token.size(), number);
    return phrase;
}

void UpdateHeader(PaletteWindow& w) {
    // 261119_0001 U3 (UI-language mode): the header names the SURFACE ("UI
    // 언어"), not a translation direction, and the engine chip is meaningless
    // here — no translation engine exists in this mode — so the chip is HIDDEN
    // (SW_HIDE) rather than relabeled with a fabricated count. The translation
    // surfaces fall through to the pinned role/engine arms below, byte-
    // identical; this branch returns before any of them runs.
    if (w.state.surface() == language_palette::PickerSurface::TrayUiLanguage) {
        ::SetWindowTextW(w.header, I18n::Get(StringId::MenuUiLanguage).c_str());
        ::ShowWindow(w.chip, SW_HIDE);
        return;
    }

    const StringId id = (w.state.role() == language_palette::PickerRole::Source)
                            ? StringId::MenuSourceLang
                            : StringId::MenuTargetLang;
    ::SetWindowTextW(w.header, I18n::Get(id).c_str());

    // 261116_0003 P7: the chip names the engine whose VIEW this list is and
    // how many languages that view offers, so the user can never mistake the
    // local shape (39 entries) for the cloud one (89 sources / 88 targets;
    // BO lacks kLangGoogle). The count is the POOL
    // size, never the filtered hit count: typing a query must not make the
    // header claim the engine suddenly offers fewer languages.
    // 261116_0003 P9b-3 U5 (CEO decision 1.b, decisions.md:540): the AUTO
    // view's chip is the dedicated auto label ALONE — the label already says
    // "local for supported languages, cloud for the rest", so counting the
    // local view's size beside it would re-introduce the half-truth. The
    // four concrete engine chips keep the engine name + " · " + count shape.
    const std::wstring chip =
        (w.state.engine() == LanguageEngine::Auto)
            ? EngineChipName(w.state.engine())
            : EngineChipName(w.state.engine()) + L" · " +
                  CountPhrase(w.state.pool().size());
    ::SetWindowTextW(w.chip, chip.c_str());
}

// Repopulates the LISTBOX from the state's hit list and mirrors the state's
// selection into it (LB_SETCURSEL — the list owns no selection state of its
// own). The empty-result case swaps the list for the no-match STATIC.
void SyncList(PaletteWindow& w) {
    ::SendMessageW(w.list, LB_RESETCONTENT, 0, 0);
    const auto& hits = w.state.hits();
    // 261119_0001 U3: the UI-language mode is the ONE surface whose AUTO pool
    // row renders the selector's own label (see RowText); every other surface
    // keeps the row shape it has always had.
    const bool ui_mode =
        w.state.surface() == language_palette::PickerSurface::TrayUiLanguage;
    for (const LanguageSearchHit& hit : hits) {
        const std::wstring row = RowText(*hit.language, ui_mode);
        ::SendMessageW(w.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
    }
    const int sel = w.state.selection();
    ::SendMessageW(w.list, LB_SETCURSEL, sel < 0 ? static_cast<WPARAM>(-1) : static_cast<WPARAM>(sel), 0);
    if (sel >= 0) {
        // Scroll-follow: only touch the top index when the selection left the
        // visible window (an unconditional LB_SETTOPINDEX would fight the
        // user's own scrolling).
        const int top = static_cast<int>(::SendMessageW(w.list, LB_GETTOPINDEX, 0, 0));
        if (sel < top) {
            ::SendMessageW(w.list, LB_SETTOPINDEX, static_cast<WPARAM>(sel), 0);
        } else if (sel >= top + kRowsVisible) {
            ::SendMessageW(w.list, LB_SETTOPINDEX, static_cast<WPARAM>(sel - kRowsVisible + 1), 0);
        }
    }
    ::ShowWindow(w.list, hits.empty() ? SW_HIDE : SW_SHOW);
    ::ShowWindow(w.no_match, hits.empty() ? SW_SHOW : SW_HIDE);
}

// EN_CHANGE: EDIT (UTF-16) -> UTF-8 -> the pure filter. The query itself is
// NEVER logged (privacy: the picker is the one place a user types freely) —
// 261116_0003 PR5-S1: nothing on this path reaches ANY log (no shape record,
// no length record). 261116_0003 PR5-10: the diag sites ELSEWHERE carry the
// audited shape fields only — result=/reason= enum tokens, surface=/role=,
// engine=, pool size, dpi=, gle=, and the committed row's registry code
// (code=, a static identifier such as "KO") — never the query text, its
// length, or any byte the user typed.
void OnQueryChanged(PaletteWindow& w) {
    std::wstring text;
    const int len = ::GetWindowTextLengthW(w.cue);
    if (len > 0) {
        text.resize(static_cast<std::size_t>(len) + 1, L'\0');
        const int copied = ::GetWindowTextW(w.cue, text.data(), static_cast<int>(text.size()));
        text.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0);
    }
    w.state.SetQuery(ToUtf8(text));
    SyncList(w);
}

// Row height in physical px: the LISTBOX reports the height it actually derived
// from the (DPI-scaled) shell font, so the visible-row count holds at every DPI
// instead of assuming the designer's 34 DIP. kRowDips is the fallback for a
// control that reports none (LB_ERR / 0).
int RowHeightPx(PaletteWindow& w) {
    const int measured = static_cast<int>(::SendMessageW(w.list, LB_GETITEMHEIGHT, 0, 0));
    return measured > 0 ? measured : ui::ScaleDipsToPixels(kRowDips, w.dpi);
}

// Whole-window height in physical px for the current DPI/font: the window is
// always exactly the DIP chrome plus kRowsVisible measured rows.
int PaletteHeightPx(PaletteWindow& w) {
    const UINT dpi = w.dpi;
    return 2 * ui::ScaleDipsToPixels(kPadDips, dpi) +
           ui::ScaleDipsToPixels(kHeaderDips, dpi) +
           ui::ScaleDipsToPixels(kHeaderChipDips, dpi) +
           ui::ScaleDipsToPixels(kHeaderGapDips, dpi) +
           ui::ScaleDipsToPixels(kCueDips, dpi) +
           ui::ScaleDipsToPixels(kGapDips, dpi) +
           RowHeightPx(w) * kRowsVisible;
}

void Layout(PaletteWindow& w) {
    const UINT dpi = w.dpi;
    const int pad = ui::ScaleDipsToPixels(kPadDips, dpi);
    const int inner = ui::ScaleDipsToPixels(kWidthDips, dpi) - 2 * pad;
    int y = pad;
    const int header_h = ui::ScaleDipsToPixels(kHeaderDips, dpi);
    ::MoveWindow(w.header, pad, y, inner, header_h, TRUE);
    y += header_h;
    // 261116_0003 P7: the engine chip line occupies its own row directly under
    // the role label (see kHeaderChipDips); the gap below it stays the single
    // kHeaderGapDips the search field always had.
    const int chip_h = ui::ScaleDipsToPixels(kHeaderChipDips, dpi);
    ::MoveWindow(w.chip, pad, y, inner, chip_h, TRUE);
    y += chip_h + ui::ScaleDipsToPixels(kHeaderGapDips, dpi);
    const int cue_h = ui::ScaleDipsToPixels(kCueDips, dpi);
    ::MoveWindow(w.cue, pad, y, inner, cue_h, TRUE);
    y += cue_h + ui::ScaleDipsToPixels(kGapDips, dpi);
    const int list_h = RowHeightPx(w) * kRowsVisible;
    ::MoveWindow(w.list, pad, y, inner, list_h, TRUE);
    ::MoveWindow(w.no_match, pad, y, inner, list_h, TRUE);
}

// 261116_0003 PR5-04(b)+PR5-S3: the new font is created BEFORE the old one is
// deleted, and the children are rebound to the NEW font BEFORE the old handle
// is deleted. Deleting first left the window fontless when the create then
// failed (the DPI-change path would silently drop to the system default), and
// a delete-before-rebind window keeps drawing through a stale HFONT between
// the two steps (the latent GDI use-after-free shape).
void ApplyFont(PaletteWindow& w) {
    const HFONT fresh = CreatePaletteFont(w.dpi);
    if (fresh == nullptr) {
        return; // keep the current font (the system default on first apply)
    }
    const HFONT previous = w.font;
    w.font = fresh;
    for (HWND child : {w.header, w.chip, w.cue, w.list, w.no_match}) {
        ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(w.font), TRUE);
    }
    if (previous) {
        ::DeleteObject(previous);
    }
}

bool CreateChildren(PaletteWindow& w) {
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    const int pad = ui::ScaleDipsToPixels(kPadDips, w.dpi);
    const int inner = ui::ScaleDipsToPixels(kWidthDips, w.dpi) - 2 * pad;

    w.header = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 pad, pad, inner, 1, w.hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdHeader)),
                                 instance, nullptr);
    // 261116_0003 P7: the engine chip + count line. Same standard SS_LEFT
    // STATIC as the role label (the OS draws it and the shell font applies
    // through ApplyFont); its text is set by UpdateHeader, never a literal
    // here. One line, clipped at the inner width by the control itself, so it
    // can never reach past the 372-DIP popup edge.
    w.chip = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                               pad, pad, inner, 1, w.hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdChip)),
                               instance, nullptr);
    w.cue = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
                              pad, pad, inner, 1, w.hwnd,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCue)),
                              instance, nullptr);
    w.list = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                               WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
                                   LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                               pad, pad, inner, 1, w.hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdList)),
                               instance, nullptr);
    if (w.header == nullptr || w.chip == nullptr || w.cue == nullptr || w.list == nullptr) {
        return false;
    }

    // The no-match STATIC is created AFTER the list so it sits above it in the
    // child z-order; the two share a rect and SyncList shows exactly one.
    w.no_match = ::CreateWindowExW(0, L"STATIC", I18n::Get(StringId::LanguageSearchNoMatch).c_str(),
                                   WS_CHILD | SS_CENTER | SS_CENTERIMAGE,
                                   pad, pad, inner, 1, w.hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdNoMatch)),
                                   instance, nullptr);
    if (w.no_match == nullptr) {
        return false;
    }

    // 261116_0003 PR5-04(e): cap the query text at 256 chars — a mega-paste
    // would otherwise churn the live filter (and the whole list rebuild) on
    // every EN_CHANGE of an edit that never needs to hold a language name
    // anywhere near that long.
    ::SendMessageW(w.cue, EM_SETLIMITTEXT, 256, 0);
    // Cue banner (REQ-050's EM_SETCUEBANNER idiom — the gray placeholder the
    // OpenAI settings edits already use). MSVC's SDK header defines it; the
    // local #define keeps the file independent of the SDK version.
    ::SendMessageW(w.cue, EM_SETCUEBANNER, TRUE,
                   reinterpret_cast<LPARAM>(I18n::Get(StringId::LanguageSearchCue).c_str()));
    return true;
}

// Ends the invocation. The window is destroyed here; the entry point's modal
// loop observes `done` on its next iteration and returns.
void ClosePalette(PaletteWindow& w) {
    if (!w.done) {
        w.done = true;
        if (w.hwnd && ::IsWindow(w.hwnd)) {
            ::DestroyWindow(w.hwnd);
        }
    }
}

// Commits `row` (Enter on the selection, or a double-click on a row) and closes.
// An empty result list has nothing to commit: Enter is a documented no-op
// (designer §4.5), so the popup stays open.
void CommitRow(PaletteWindow& w, const LanguageInfo* row) {
    if (row == nullptr) {
        return;
    }
    w.committed = true;
    w.committed_code = row->code;
    w.committed_name_en = std::string(language_palette::PickerState::PayloadOf(row));
    DIAG_LOG("UI", "lang_palette_commit surface=%d role=%d code=%s",
             static_cast<int>(w.state.surface()), static_cast<int>(w.state.role()),
             w.committed_code.c_str());
    ClosePalette(w);
}

// Pre-translate hook of the modal loop: the four navigation keys never reach
// the EDIT/LISTBOX controls (Tab must flip the ROLE, never move focus; ↑↓ must
// move the LISTBOX selection while the caret stays in the EDIT). Everything
// else falls through to TranslateMessage/DispatchMessage untouched — including
// every WM_SYSKEYDOWN (261116_0003 PR5-02(a): Alt+key and F10 must keep their
// system semantics; swallowing SYSKEYDOWN here made Alt+Tab flip the picker
// role, Alt+Enter commit and Alt+Esc cancel instead of leaving the popup).
bool HandleNavigationKey(PaletteWindow& w, const MSG& msg) {
    // Only the popup's own window tree — the modal loop also pumps messages for
    // the rest of the app (the same scope a modal dialog has), and a key aimed
    // at another window must never be swallowed by the picker.
    if (msg.hwnd != w.hwnd && msg.hwnd != w.cue && msg.hwnd != w.list) {
        return false;
    }
    if (msg.message != WM_KEYDOWN) { // PR5-02(a): WM_SYSKEYDOWN is NOT handled
        return false;
    }
    using language_palette::PickerCommand;
    switch (msg.wParam) {
        case VK_UP:
            if (w.state.Apply(PickerCommand::MoveUp).changed) {
                SyncList(w);
            }
            return true;
        case VK_DOWN:
            if (w.state.Apply(PickerCommand::MoveDown).changed) {
                SyncList(w);
            }
            return true;
        case VK_RETURN:
        case VK_SELECT: {
            // The focused control never sees Enter (no default button exists in
            // a non-dialog window), so there is no beep to suppress. An empty
            // result list commits nothing and leaves the popup open.
            const language_palette::PickerResolution resolution =
                w.state.Apply(PickerCommand::Commit);
            if (resolution.committed) {
                CommitRow(w, resolution.row);
            }
            return true;
        }
        case VK_ESCAPE: {
            w.state.Apply(PickerCommand::Cancel);
            DIAG_LOG("UI", "lang_palette_cancel surface=%d role=%d reason=esc",
                     static_cast<int>(w.state.surface()),
                     static_cast<int>(w.state.role()));
            ClosePalette(w);
            return true;
        }
        case VK_TAB: {
            // Shift+Tab lands here too: two roles, so both directions flip.
            // 261119_0001 U3: consume the resolution and repaint only when the
            // flip actually moved something. In the UI-language mode FlipRole
            // is a pure no-op (one role, one pool — see PickerState::FlipRole),
            // and re-seating/repainting there would churn the list for
            // nothing; every translation surface reports changed == true and
            // keeps the exact pre-U3 repaint behavior.
            const language_palette::PickerResolution resolution =
                w.state.Apply(PickerCommand::FlipRole);
            if (resolution.changed) {
                UpdateHeader(w);
                SyncList(w);
            }
            return true;
        }
        default:
            return false;
    }
}

LRESULT CALLBACK PaletteWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PaletteWindow* w = FromWindow(hwnd);
    if (w == nullptr) {
        // WM_NCCREATE / WM_CREATE before GWLP_USERDATA is published (and any
        // message to a half-created window).
        if (msg == WM_NCCREATE) {
            const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            if (id == kIdCue && code == EN_CHANGE) {
                OnQueryChanged(*w);
                return 0;
            }
            if (id == kIdList && code == LBN_SETFOCUS) {
                // 261116_0003 PR5-06: the LBN_SELCHANGE arm below fires on
                // selection CHANGE only — clicking the ALREADY-selected row or
                // the list's blank area still gives the LISTBOX focus with no
                // notification, and the next keystroke is eaten by listbox
                // type-ahead instead of reaching the cue EDIT. Refocus on the
                // list's focus GAIN (the listbox carries LBS_NOTIFY, so the
                // notification IS delivered), covering every path that hands
                // the list focus; handing the focus straight back sends the
                // list LBN_KILLFOCUS, so no notification loop is possible.
                ::SetFocus(w->cue);
                return 0;
            }
            if (id == kIdList && code == LBN_SELCHANGE) {
                // Mouse/keyboard click inside the list: adopt it as the state's
                // selection so a following ↑↓ continues from the clicked row.
                w->state.SelectRow(
                    static_cast<int>(::SendMessageW(w->list, LB_GETCURSEL, 0, 0)));
                // 261116_0003 PR5-02(b): the click handed focus to the LISTBOX;
                // give it straight back to the cue EDIT, or the next keystrokes
                // hit listbox type-ahead and the live filter stops.
                ::SetFocus(w->cue);
                return 0;
            }
            if (id == kIdList && code == LBN_DBLCLK) {
                // Double-click commits the row it landed on (designer §4.5).
                const int index = static_cast<int>(::SendMessageW(w->list, LB_GETCURSEL, 0, 0));
                const language_palette::PickerResolution resolution =
                    w->state.Apply(language_palette::PickerCommand::ActivateRow, index);
                if (resolution.committed) {
                    CommitRow(*w, resolution.row);
                }
                return 0;
            }
            return 0;
        }

        case WM_ACTIVATE: {
            // Click-outside dismissal. Armed only AFTER the popup has been
            // active once, so the WA_INACTIVE some shells deliver around
            // ShowWindow/SetForegroundWindow cannot close the popup on open.
            // The close is POSTED rather than performed inline: destroying a
            // window from inside its own WM_ACTIVATE handler re-enters the
            // activation machinery.
            if (LOWORD(wParam) == WA_INACTIVE) {
                if (w->deactivate_dismiss) {
                    DIAG_LOG("UI", "lang_palette_cancel surface=%d role=%d reason=deactivate",
                             static_cast<int>(w->state.surface()),
                             static_cast<int>(w->state.role()));
                    ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
                }
            } else {
                w->deactivate_dismiss = true;
            }
            return 0;
        }

        case WM_DPICHANGED: {
            // REQ-R15 house pattern (tooltip.cpp / badge.cpp / about_window.cpp):
            // adopt the new monitor DPI, take the suggested rect, re-lay out in
            // DIPs and rebuild the font at the new scale. The height is
            // re-measured because the row height follows the font.
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            w->dpi = ui::WindowDpi(hwnd);
            ApplyFont(*w);
            Layout(*w);
            const int width = ui::ScaleDipsToPixels(kWidthDips, w->dpi);
            const int height = PaletteHeightPx(*w);
            if (suggested != nullptr) {
                ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, width, height,
                               SWP_NOZORDER | SWP_NOACTIVATE);
            } else {
                ::SetWindowPos(hwnd, nullptr, 0, 0, width, height,
                               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }

        case WM_CLOSE:
            ClosePalette(*w);
            return 0;

        case WM_DESTROY:
            w->done = true;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

bool PostDeferredLanguagePaletteOpen(HWND controller,
                                     const language_palette::PickerInit& init) {
    if (controller == nullptr) {
        DIAG_LOG("UI", "lang_palette_deferred_post result=no_controller");
        return false;
    }
    std::size_t slot = 0;
    if (!PickerSlotFor(init.surface, &slot)) {
        DIAG_LOG("UI", "lang_palette_deferred_post result=bad_surface surface=%d",
                 static_cast<int>(init.surface));
        return false;
    }
    if (s_open_pending[slot]) {
        // Single instance PER SURFACE: the already-latched request on THIS
        // surface wins (designer §4.2); another surface's slot is untouched.
        DIAG_LOG("UI", "lang_palette_deferred_post result=coalesced surface=%d",
                 static_cast<int>(init.surface));
        return false;
    }
    s_pending_init[slot] = init;
    s_open_pending[slot] = true;
    if (::PostMessageW(controller, kMsgOpenLanguagePalette, 0, 0) == FALSE) {
        // Nothing consumed the latch (the post is the only wake-up), so rolling
        // THIS SLOT back is safe here — unlike the language-sync queue, where a
        // second producer could already be racing a successful wake-up. Only
        // this surface's slot is cleared; a pending request on another surface
        // survives a failed post here (PR6-32).
        s_open_pending[slot] = false;
        s_pending_init[slot] = language_palette::PickerInit{};
        DIAG_LOG("UI", "lang_palette_deferred_post result=post_failed gle=%lu surface=%d",
                 ::GetLastError(), static_cast<int>(init.surface));
        return false;
    }
    return true;
}

std::optional<language_palette::PickerInit> TakeDeferredLanguagePaletteOpen(
    language_palette::PickerSurface surface) {
    std::size_t slot = 0;
    if (!PickerSlotFor(surface, &slot)) {
        DIAG_LOG("UI", "lang_palette_take result=bad_surface surface=%d",
                 static_cast<int>(surface));
        return std::nullopt;
    }
    if (!s_open_pending[slot]) {
        return std::nullopt;
    }
    s_open_pending[slot] = false;
    language_palette::PickerInit init = std::move(s_pending_init[slot]);
    s_pending_init[slot] = language_palette::PickerInit{};
    return init;
}

bool LanguagePickerShowing() {
    // V10F FUP-02: acquire load — readers on the hook threads (main.cpp's ESC /
    // dismissal exemptions) must see the flag set by the GUI thread's release
    // store before ShowLanguageSearchPalette entered its modal loop.
    return s_palette_open.load(std::memory_order_acquire);
}

LanguagePickerResult ShowLanguageSearchPalette(HWND owner,
                                               const language_palette::PickerInit& init,
                                               const POINT* anchor) {
    LanguagePickerResult result;
    result.surface = init.surface;
    result.role = init.role;

    // Single instance: a picker already open swallows a second open (the
    // deferred latch normally prevents this; a direct call can still race a
    // modal loop pumped from the same thread). The flag is module-scope since
    // the PR6-42 fix round (LanguagePickerShowing reads it); V10F FUP-02 made
    // it atomic so the hook-thread readers join the acquire/release pairing.
    if (s_palette_open.load(std::memory_order_acquire)) {
        DIAG_LOG("UI", "lang_palette_open result=already_open");
        return result;
    }
    // V10F FUP-02: release store — publishes "the modal loop is running" to
    // the hook-thread readers before any picker state exists.
    s_palette_open.store(true, std::memory_order_release);
    struct OpenGuard {
        std::atomic<bool>* flag;
        ~OpenGuard() { flag->store(false, std::memory_order_release); }
    } open_guard{&s_palette_open};

    POINT pt = {};
    if (anchor != nullptr) {
        pt = *anchor;
    } else {
        ::GetCursorPos(&pt);
    }

    auto window = std::make_unique<PaletteWindow>(init);
    PaletteWindow& w = *window;
    w.dpi = ui::MonitorDpiAtPoint(pt);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = PaletteWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    // Classic dialog face: the picker IS a standard-control surface, so it
    // takes the OS's own dialog background instead of inventing a themed one
    // (the Direct2D cards own the branded surfaces; this one is a utility).
    wc.hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = kPaletteClassName;
    // 261116_0003 PR5-04(c): the return IS checked. ERROR_CLASS_ALREADY_EXISTS
    // is tolerated — idempotent re-registration is the design, the class lives
    // for the process once the first popup registered it. Any other failure
    // must not reach CreateWindowExW with an unregistered class.
    if (::RegisterClassExW(&wc) == 0) {
        const DWORD register_error = ::GetLastError();
        if (register_error != ERROR_CLASS_ALREADY_EXISTS) {
            DIAG_LOG("UI", "lang_palette_open result=register_failed gle=%lu",
                     register_error);
            return result;
        }
        // 261116_0003 PR5-S2: the class name is taken — proceed ONLY when the
        // pre-existing class IS this module's. A foreign WndProc registered
        // under our name would route the popup's messages elsewhere; verify
        // and bail (still uncommitted) on mismatch or query failure.
        WNDCLASSEXW existing = {};
        existing.cbSize = sizeof(existing);
        // 261116_0003 PR5-09: the two failure modes log SEPARATELY because only
        // one of them owns a live GetLastError(). The query-failure branch does
        // (the call just failed); the WndProc-mismatch branch does NOT — its
        // GetClassInfoExW SUCCEEDED, so the gle there is stale (the earlier
        // ERROR_CLASS_ALREADY_EXISTS, 1410) and printing it read as "benign
        // re-registration" to triage. Both still log and bail BEFORE
        // CreateWindowExW: the fail-closed behavior is unchanged.
        if (::GetClassInfoExW(wc.hInstance, kPaletteClassName, &existing) == 0) {
            DIAG_LOG("UI", "lang_palette_open result=class_name_query_failed gle=%lu",
                     ::GetLastError());
            return result;
        }
        if (existing.lpfnWndProc != PaletteWndProc) {
            DIAG_LOG("UI", "lang_palette_open result=class_name_conflict reason=wndproc_mismatch");
            return result;
        }
    }

    const int width = ui::ScaleDipsToPixels(kWidthDips, w.dpi);
    // Created at the DIP-derived height, then resized to the MEASURED height
    // (kRowsVisible rows of the scaled shell font) before it is shown — the
    // measurement needs the controls, the controls need the window.
    const int initial_height = ui::ScaleDipsToPixels(kRowDips, w.dpi) * kRowsVisible +
                                  2 * ui::ScaleDipsToPixels(kPadDips, w.dpi) +
                                  ui::ScaleDipsToPixels(kHeaderDips, w.dpi) +
                                  ui::ScaleDipsToPixels(kHeaderChipDips, w.dpi) +
                                  ui::ScaleDipsToPixels(kHeaderGapDips, w.dpi) +
                                  ui::ScaleDipsToPixels(kCueDips, w.dpi) +
                                  ui::ScaleDipsToPixels(kGapDips, w.dpi);

    // The window text is never drawn (a WS_POPUP has no caption); it exists for
    // accessibility tooling, so it carries the localized app name rather than a
    // hardcoded literal.
    w.hwnd = ::CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPaletteClassName,
        I18n::Get(StringId::AppName).c_str(), WS_POPUP | WS_CLIPCHILDREN, pt.x, pt.y, width,
        initial_height, owner, nullptr, ::GetModuleHandleW(nullptr), &w);
    if (w.hwnd == nullptr) {
        DIAG_LOG("UI", "lang_palette_open result=create_failed gle=%lu", ::GetLastError());
        return result;
    }

    // 261116_0003 PR5-04(f): every exit path from here must destroy the HWND
    // before the PaletteWindow struct dies. The struct is heap-held by a
    // unique_ptr and published through GWLP_USERDATA, so an exception escaping
    // the setup or the modal loop (a std::bad_alloc from any handler would do)
    // would otherwise free a live window's state. The catch destroys the
    // window first, logs, and rethrows; the IsWindow/DestroyWindow after the
    // loop stays the normal path.
    try {
        if (!CreateChildren(w)) {
            DIAG_LOG("UI", "lang_palette_open result=children_failed gle=%lu", ::GetLastError());
            ::DestroyWindow(w.hwnd);
            return result;
        }
        ApplyFont(w);
        UpdateHeader(w);
        SyncList(w);
        Layout(w);
        const int height = PaletteHeightPx(w);

        // Clamp into the work area of the monitor the anchor is on, so the popup
        // never opens under the taskbar or off-screen (the dpi.hpp contract).
        RECT work = {0, 0, 0, 0};
        if (HMONITOR monitor = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST)) {
            MONITORINFO info = {};
            info.cbSize = sizeof(info);
            if (::GetMonitorInfoW(monitor, &info)) {
                work = info.rcWork;
            }
        }
        const POINT origin = ui::ClampWindowOrigin(
            pt.x, pt.y, width, height, ui::ScaleDipsToPixels(kPadDips, w.dpi), work);
        ::SetWindowPos(w.hwnd, HWND_TOPMOST, origin.x, origin.y, width, height,
                       SWP_SHOWWINDOW);
        ::SetForegroundWindow(w.hwnd);
        ::SetFocus(w.cue);

        DIAG_LOG("UI", "lang_palette_open surface=%d role=%d engine=%d pool=%zu dpi=%u",
                 static_cast<int>(init.surface), static_cast<int>(init.role),
                 static_cast<int>(init.engine), w.state.pool().size(), w.dpi);

        // ---- the modal loop ---------------------------------------------------
        // Pumps this thread's messages (the same scope DialogBoxIndirectParamW
        // gives the settings dialogs), exits when the picker closes itself. A
        // WM_QUIT is re-posted so the outer application loop still terminates.
        while (!w.done) {
            MSG msg = {};
            const BOOL got = ::GetMessageW(&msg, nullptr, 0, 0);
            if (got == 0) {
                ::PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            if (got == -1) {
                break;
            }
            if (HandleNavigationKey(w, msg)) {
                continue;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }

        if (::IsWindow(w.hwnd)) {
            ::DestroyWindow(w.hwnd);
        }
    } catch (...) {
        // PR5-04(f): unwind path — the window dies before the struct can.
        if (::IsWindow(w.hwnd)) {
            ::DestroyWindow(w.hwnd);
        }
        DIAG_LOG("UI", "lang_palette_open result=modal_loop_exception");
        throw;
    }
    if (w.committed) {
        result.committed = true;
        // The role/surface AT COMMIT TIME, not the request's: if the user
        // pressed Tab before committing, the pick belongs to the FLIPPED role
        // (dispatching the request's role would fire the wrong callback).
        result.surface = w.state.surface();
        result.role = w.state.role();
        result.code = w.committed_code;
        result.name_en = w.committed_name_en;
    }
    return result;
}

} // namespace emebalachat
