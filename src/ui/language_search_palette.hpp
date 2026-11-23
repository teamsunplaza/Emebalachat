#pragma once

// 261116_0003 P5 (language-menu redesign, plan §3 D3 + §4 "Search" + §5 P5):
// the shared searchable language picker ("언어 퀵픽"). ONE popup serves every
// language-selection surface; P6 prepends the "검색…" row to the six flat
// menus and routes the pick here.
//
// The module is split at a hard seam, and the seam is the testability contract:
//
//   SECTION A — namespace language_palette: the PURE logic. Header-inline, zero
//     Win32 calls, no window, no message loop, no global UI state. Everything
//     tests/p5_language_picker_tests.inc exercises lives here: the engine-view
//     filter (SearchLanguages over GetLanguagesForEngine), the best-match
//     preselection, ↑↓ wraparound, Enter/double-click resolution, Esc, the Tab
//     source↔target flip (including the view swap), the empty-query state and
//     the AUTO row's role asymmetry. 261119_0001 U2 (UI-language-89 phase 2)
//     adds the UI-language MODE to this same layer: ONE surface value
//     (PickerSurface::TrayUiLanguage) swaps the engine view for the authored
//     UI-locale pool and turns the Tab flip into a no-op; the keyboard
//     contract is pinned headlessly by tests/p6_ui_language_picker_tests.inc.
//
//   SECTION B — the Win32 window: WS_POPUP + cue-banner EDIT + LISTBOX (standard
//     controls, shell UI font scaled per monitor), its own modal loop, and the
//     REQ-047 D3 deferred-open entry the P6 tray/tooltip code calls. The only
//     window-creating call in the module lives here, behind
//     ShowLanguageSearchPalette — a test never reaches it, and no build step
//     runs it.
//
// Design record (designer report §4.2/§4.3/§4.5, docs/261116_0002_session_
// language-menu-redesign): live filter on EN_CHANGE with the best match
// preselected; ↑↓ (wraps); Enter/double-click commit; Esc cancels; Tab /
// Shift+Tab flips the surface context (출발언어 ↔ 도착언어) and re-seats the
// selection on that context's current language. The committed pick is
// dispatched by P6 through the EXISTING language-change callbacks with the
// registry row's name_en bytes unchanged (LanguagePickerResult::name_en).

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include "config.hpp"         // LanguageEngine / LanguageInfo / GetLanguagesForEngine
#include "i18n.hpp"           // GetSupportedUiLocalePickerPool (the UI-language mode's pool)
#include "language_alias.hpp" // SearchLanguages / LanguageSearchHit

namespace emebalachat {
namespace language_palette {

// ===========================================================================
// SECTION A — pure logic (headless; no Win32 call, no window)
// ===========================================================================

// Which of the surface's two roles the popup is currently editing. Tab flips
// it (designer §4.5). The role decides BOTH the engine view — the source role
// keeps the AUTO row, the target role never offers it (AUTO can never be a
// translation target) — and which callback P6 dispatches to.
enum class PickerRole : unsigned char {
    Source = 0,
    Target = 1,
};

// The menu surface that opened the popup. Plan §1 counts six language-selection
// surfaces; they collapse to four surfaces - the first three x two roles - so
// P6 maps the (surface, role) pair onto the existing callbacks:
//   TrayTyping  + Source/Target -> on_select_source_lang / on_select_target_lang
//   TrayTooltip + Source/Target -> on_select_drag_source_lang / on_select_drag_target_lang
//   TooltipPill + Source/Target -> the floating tooltip's src/lang callbacks
//   TrayUiLanguage (261119_0001 U2) + role UNUSED -> the tray's UI-language
//     submenu. A UI-chrome locale has no translation direction, so this surface
//     has one flat pool and the role is dead weight; that is exactly why the
//     surface value doubles as the MODE discriminator: this ONE value swaps the
//     pool to GetSupportedUiLocalePickerPool() and refuses the Tab flip, so no
//     second mode field can ever disagree with it.
// The picker itself never dispatches - it only reports the pair.
enum class PickerSurface : unsigned char {
    TrayTyping = 0,
    TrayTooltip = 1,
    TooltipPill = 2,
    TrayUiLanguage = 3,
};

// One invocation's inputs, latched at open time (the REQ-047 D3 deferred open
// carries this by value: no pointer ever crosses PostMessageW).
struct PickerInit {
    PickerSurface surface = PickerSurface::TrayTyping;
    PickerRole role = PickerRole::Source;
    LanguageEngine engine = LanguageEngine::Auto;
    // The role's CURRENT value exactly as the config holds it: a registry code
    // ("KO") or an English name ("Korean"); "" means unset. The tray passes
    // name_en, the tooltip passes its display code — both resolve through
    // FindLanguageByCode/FindLanguageByName below.
    std::string source_code;
    std::string target_code;
};

// Pure state of one picker invocation: query -> ranked hits -> selection.
// Constructed once per open; the window layer owns one instance and forwards
// every user event here. Copyable and cheap (the hit vector only).

// Every user action the popup understands, expressed as DATA so the whole
// key/mouse contract is testable headlessly: the window layer translates
// WM_KEYDOWN / LBN_DBLCLK into these commands and touches nothing else. Query
// text arrives through PickerState::SetQuery (a string cannot ride an enum).
enum class PickerCommand : unsigned char {
    MoveUp = 0,      // ↑ (wraps)
    MoveDown = 1,    // ↓ (wraps)
    Commit = 2,      // Enter — commits the selected row
    Cancel = 3,      // Esc / click-outside — ends the invocation, no commit
    FlipRole = 4,    // Tab / Shift+Tab — source↔target, view swap
    ActivateRow = 5, // double-click — commits the row that was clicked
};

// What one command did. `committed` carries the row P6 must dispatch;
// `cancelled` means "close, dispatch nothing"; `changed` means the visible
// list/selection moved (the window repaints the LISTBOX from the state).
struct PickerResolution {
    bool committed = false;
    bool cancelled = false;
    bool changed = false;
    const LanguageInfo* row = nullptr;
};

class PickerState {
public:
    explicit PickerState(const PickerInit& init) : init_(init), role_(init.role) {
        RefreshHits();
        // Empty query = the unfiltered pool in registry order (the matcher's
        // documented contract); the starting selection is the role's CURRENT
        // language (designer §4.3 "현재 선택" / §4.5 Tab rule), or row 0 when it
        // is not in this engine's list (a cloud-only pin under the local
        // engine) — never -1 while rows exist.
        ReselectRoleCurrent();
    }

    // ---- query (EN_CHANGE) -------------------------------------------------
    // Live filter. The selection RESETS to the best match: SearchLanguages
    // already returns the hits ranked (tier, match kind, registry order), so
    // "row 0" IS the best match — no UI-level re-sort may be added (the
    // CEO's 한국어/korean/koreanisch/한글/KO pins depend on that order).
    // Returns true when at least one row is offered.
    bool SetQuery(std::string_view utf8) {
        query_.assign(utf8.data(), utf8.size());
        RefreshHits();
        selection_ = hits_.empty() ? -1 : 0;
        return !hits_.empty();
    }

    // Back to the invocation's starting state (empty query, whole pool,
    // selection on the role's current language).
    void ClearQuery() {
        query_.clear();
        RefreshHits();
        ReselectRoleCurrent();
    }

    const std::string& query() const noexcept { return query_; }

    // True when the query actually filters. A whitespace-only query is the
    // unfiltered state: FoldLanguageAliasText collapses it to empty, which is
    // exactly what SearchLanguages treats as "no filter".
    bool Filtering() const { return !FoldLanguageAliasText(query_).empty(); }

    // ---- result list -------------------------------------------------------
    const std::vector<LanguageSearchHit>& hits() const noexcept { return hits_; }
    bool HasHits() const noexcept { return !hits_.empty(); }

    // The engine-filtered view this invocation searches (never a copy: pointers
    // into the static registry, stable for the process lifetime).
    const std::vector<const LanguageInfo*>& pool() const noexcept { return *pool_; }

    // ---- selection ---------------------------------------------------------
    int selection() const noexcept { return selection_; }

    // The row under the selection, or nullptr (empty result list).
    const LanguageInfo* selected() const noexcept {
        if (selection_ < 0 || static_cast<std::size_t>(selection_) >= hits_.size()) {
            return nullptr;
        }
        return hits_[static_cast<std::size_t>(selection_)].language;
    }

    // ↑ / ↓ with wraparound (designer §4.5 "Move selection (wraps)"): +1 from
    // the last row lands on row 0, -1 from row 0 lands on the last row.
    // `delta` is any signed step, normalized modulo the hit count. Returns
    // false — leaving the selection at -1 — when nothing is offered.
    // 261116_0003 PR5-04(d): the intermediate arithmetic is widened to long
    // long — `base + delta` computed in int is signed-overflow UB for an
    // extreme delta (production callers pass ±1, but the documented contract
    // is any signed step); the in-range result is cast back to int.
    bool MoveSelection(int delta) {
        const long long n = static_cast<long long>(hits_.size());
        if (n <= 0) {
            selection_ = -1;
            return false;
        }
        const long long base =
            (selection_ < 0) ? -1 : static_cast<long long>(selection_);
        selection_ = static_cast<int>(((base + delta) % n + n) % n);
        return true;
    }
    bool MoveUp() { return MoveSelection(-1); }
    bool MoveDown() { return MoveSelection(1); }

    // A click / hover landing on row `index` (LB_GETCURSEL sync). Out-of-range
    // input is refused and leaves the selection untouched — a click can never
    // fabricate a row that is not in the result list.
    bool SelectRow(int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= hits_.size()) {
            return false;
        }
        selection_ = index;
        return true;
    }

    // ---- resolution --------------------------------------------------------
    // Enter commits the selected row. A double-click commits the row it landed
    // on. Both return nullptr when there is nothing to commit, which is the
    // "Enter is a no-op on an empty state" rule (designer §4.5).
    const LanguageInfo* Commit() const noexcept { return selected(); }
    const LanguageInfo* CommitRow(int index) const noexcept {
        if (index < 0 || static_cast<std::size_t>(index) >= hits_.size()) {
            return nullptr;
        }
        return hits_[static_cast<std::size_t>(index)].language;
    }

    // The dispatch payload: the registry row's own name_en bytes, untouched.
    // This is the flat menu's callback contract (tray.cpp passes
    // allLangs[idx].name_en), so a pick made in the picker is byte-identical
    // to the same pick made in the menu — no second dispatch path exists.
    static std::string_view PayloadOf(const LanguageInfo* row) noexcept {
        return row == nullptr ? std::string_view{} : std::string_view{row->name_en};
    }

    // ---- the single command entry (the window layer's ONLY mutation path) ---
    // See PickerCommand / PickerResolution below. `row_index` is used by
    // ActivateRow only (the index the LISTBOX reported). Enter on an empty
    // result list commits nothing and does NOT close (designer §4.5: Enter is
    // a no-op there).
    PickerResolution Apply(PickerCommand command, int row_index = -1) {
        PickerResolution out;
        switch (command) {
            case PickerCommand::MoveUp:
                out.changed = MoveUp();
                return out;
            case PickerCommand::MoveDown:
                out.changed = MoveDown();
                return out;
            case PickerCommand::Commit:
                out.row = Commit();
                out.committed = (out.row != nullptr);
                return out;
            case PickerCommand::Cancel:
                out.cancelled = true;
                return out;
            case PickerCommand::FlipRole:
                // 261119_0001 U2: the resolution reports whether the flip
                // actually moved anything. Every translation surface always
                // flips (FlipRole() returns true, so this stays byte-identical
                // for them); the UI-language mode refuses the flip outright, so
                // the window layer can skip a dead re-seat/repaint.
                out.changed = FlipRole();
                return out;
            case PickerCommand::ActivateRow:
                out.row = CommitRow(row_index);
                out.committed = (out.row != nullptr);
                return out;
        }
        return out; // unreachable: the enum is closed
    }

    // ---- roles (Tab / Shift+Tab) ------------------------------------------
    PickerRole role() const noexcept { return role_; }
    PickerSurface surface() const noexcept { return init_.surface; }
    LanguageEngine engine() const noexcept { return init_.engine; }
    const PickerInit& init() const noexcept { return init_; }

    // The current value of `role` exactly as the caller seeded it.
    std::string_view CodeForRole(PickerRole role) const noexcept {
        return role == PickerRole::Source ? std::string_view{init_.source_code}
                                          : std::string_view{init_.target_code};
    }
    std::string_view CurrentCode() const noexcept { return CodeForRole(role_); }

    // Tab (and Shift+Tab — there are exactly two roles, so both land here).
    // The flip changes the engine VIEW (source keeps AUTO, target never offers
    // it) and re-seats the selection on the new role's current language
    // (designer §4.5). The QUERY IS KEPT on purpose: "type the name once, Tab,
    // pick the other role" is the flow the flip exists for.
    //
    // 261119_0001 U2: the tray's UI-language mode has ONE role - there is no
    // translation direction to flip to - so Tab is a pure no-op there. It
    // returns false and touches NOTHING (role, pool, query, hits, selection),
    // which is what makes the refusal observable to the window layer through
    // PickerResolution::changed. Every other surface keeps the flip verbatim.
    bool FlipRole() {
        if (init_.surface == PickerSurface::TrayUiLanguage) {
            return false;
        }
        role_ = (role_ == PickerRole::Source) ? PickerRole::Target : PickerRole::Source;
        RefreshHits();
        ReselectRoleCurrent();
        return true;
    }

    // ---- ghost row (plan §4 "Ghost row") -----------------------------------
    // The ACTIVE role's pinned language, returned ONLY when the active engine
    // does not offer it (e.g. a cloud-only pin while the local engine is
    // selected). nullptr when it is offered, unknown to the registry, empty, or
    // AUTO (AUTO is a source-only marker, never a "pinned language").
    //
    // P5 owns the PREDICATE only. The ghost ROW itself — the greyed, checked
    // row pinned above the list with its warning outline — is P6 menu scope:
    // a plain LISTBOX cannot express that visual without owner-draw, and the
    // P5 unit must not pre-empt P6's row model. See the phase note.
    //
    // 261119_0001 U2: the UI-language mode has no engine view and no engine
    // capability gate, so "pinned but not offered" cannot exist there - the
    // concept is a translation-engine one. nullptr, always, before any lookup.
    const LanguageInfo* PinnedAbsentLanguage() const {
        if (init_.surface == PickerSurface::TrayUiLanguage) {
            return nullptr;
        }
        const std::string_view wanted = CurrentCode();
        if (wanted.empty()) {
            return nullptr;
        }
        const LanguageInfo* row = FindLanguageByCode(wanted);
        if (row == nullptr) {
            row = FindLanguageByName(wanted);
        }
        if (row == nullptr || row->code == "AUTO") {
            return nullptr;
        }
        // Capability check through the SAME view machinery the list uses, so
        // the predicate can never drift from what the picker offers (Auto
        // resolves to the HyMt2 view inside the registry accessor).
        for (const LanguageInfo* offered : GetLanguagesForEngine(init_.engine, /*include_auto=*/true)) {
            if (offered == row) {
                return nullptr;
            }
        }
        return row;
    }

private:
    // Re-derives the view + the ranked hit list from the current (role, query).
    void RefreshHits() {
        // 261119_0001 U2 (UI-language-89 phase 2, architect-pick.md "Mode
        // discriminator"): the tray's UI-language mode is the ONE surface that
        // does not search an engine view. Its pool is the authored-gated
        // UI-locale pool (AUTO first, then one registry row per authored
        // selector entry), engine-mask independent and role independent - a UI
        // chrome locale is offerable whatever the translation engine can do.
        if (init_.surface == PickerSurface::TrayUiLanguage) {
            pool_ = &GetSupportedUiLocalePickerPool();
        } else {
            // include_auto: the source role offers AUTO (first-class source
            // setting), the target role never does — the same asymmetry the tray
            // menus already ship (GetSupportedLanguages vs GetTargetLanguages).
            pool_ = &GetLanguagesForEngine(init_.engine, role_ == PickerRole::Source);
        }
        hits_ = SearchLanguages(query_, *pool_);
    }

    // Selection := the role's current language when the current result list
    // contains it, else row 0 (or -1 when the list is empty).
    void ReselectRoleCurrent() {
        selection_ = -1;
        if (hits_.empty()) {
            return;
        }
        const std::string_view wanted = CurrentCode();
        if (!wanted.empty()) {
            const LanguageInfo* row = FindLanguageByCode(wanted);
            if (row == nullptr) {
                row = FindLanguageByName(wanted);
            }
            if (row != nullptr) {
                for (std::size_t i = 0; i < hits_.size(); ++i) {
                    if (hits_[i].language == row) {
                        selection_ = static_cast<int>(i);
                        return;
                    }
                }
            }
        }
        selection_ = 0;
    }

    PickerInit init_;
    PickerRole role_ = PickerRole::Source;
    const std::vector<const LanguageInfo*>* pool_ = nullptr;
    std::string query_;
    std::vector<LanguageSearchHit> hits_;
    int selection_ = -1;
};

// ---- 261116_0003 P6b fix round (PR6-28), re-anchored to the landed P6c
// state (PR6-43 sweep): the shared ghost-suppression gate --------------------
// TRUE when the pinned-absent ghost row must be SUPPRESSED for this request.
// ONE DECISION, consumed by the tray's four language menus (tray.cpp) AND the
// tooltip's two dropdowns (tooltip.cpp) — but the INPUTS are NOT shared: each
// surface resolves its own is_auto / local_host_available view at its own
// menu-open time. Since P6c BOTH halves are wired (PR6-35): main.cpp's
// controller wires the tray's resolver AND the tooltip's SetEngineResolver
// (one call site, deriving all three values from the same persisted config
// snapshot through the tray's own engine mapper). That shared source is why
// the two surfaces now agree in practice; the contract is still
// shared-DECISION / unshared-INPUTS, so each surface stays responsible for
// feeding the live values, never a cached copy. The suppression itself is the
// ROUTER-AWARE decision carried over from the tray's P6a gate (hardened
// across fix rounds r1-r5): only a persisted "auto" whose local host leg is
// live AND whose pair the router would serve locally may treat a pin the
// Google view does not offer as servable. Every other state keeps the
// truthful single-view ghost (the P6a r1-r5 defect class was WRONGFUL
// SUPPRESSION, so a missing/unresolved input must never suppress).
// `preferred_engine` is the tray's collapsed engine index (0 = the Google row
// covering both "google" and "auto"); the tooltip maps its LanguageEngine
// onto the same zero-vs-nonzero axis. Pure: LocalPairReliable (config.hpp)
// is reachable through this header's config.hpp include.
inline bool SuppressPinnedAbsentGhost(const PickerInit& req,
                                      int preferred_engine,
                                      bool is_auto,
                                      bool local_host_available) {
    if (preferred_engine != 0 || !is_auto || !local_host_available) {
        return false;
    }
    return LocalPairReliable(req.source_code, req.target_code);
}

// V10F FUP-02: the picker's window class, defined HERE (SECTION A, one shared
// definition) because the pure exemption predicate below and the Win32
// registration in the .cpp must never drift apart. main.cpp's global-input
// callbacks (mouse-down / drag-release / ESC on the hook threads) exempt our
// own picker window from outside-click dismissal / drag-icon summon / overlay
// Esc through IsLanguagePaletteWindowClass + a same-pid check at the call site.
inline constexpr wchar_t kLanguagePaletteWindowClass[] = L"Emebalachat_LanguageSearchPalette";

// Pure half of the FUP-02 exemption: does this window class name belong to our
// language picker? (The caller owns the WindowFromPoint/ancestor walk and the
// GetWindowThreadProcessId same-pid check; the class compare itself is pure so
// the decision is unit-testable headlessly.)
inline bool IsLanguagePaletteWindowClass(std::wstring_view class_name) noexcept {
    return class_name == kLanguagePaletteWindowClass;
}

} // namespace language_palette

// ===========================================================================
// SECTION B — the Win32 window (never entered by a test, never by a build)
// ===========================================================================

// One invocation's outcome. `committed == false` means Esc / click-outside /
// no row to commit (nothing may be dispatched). `surface`/`role` are echoed
// back so P6 can pick the callback without re-deriving the invocation state,
// and `name_en` is the registry row's name_en (byte-identical payload).
struct LanguagePickerResult {
    bool committed = false;
    language_palette::PickerSurface surface = language_palette::PickerSurface::TrayTyping;
    language_palette::PickerRole role = language_palette::PickerRole::Source;
    std::string code;    // committed registry code ("KO")
    std::string name_en; // committed registry name_en ("Korean")
};

// REQ-047 D3 (plan §4 "Search"): the deferred-open wake-up the P6 tray code
// posts. Distinct WM_APP band: 0x503 verified unused (main.cpp owns 0x400
// engine-modal, 0x500 OpenAI settings, 0x501 gguf manager, 0x502 load-settings;
// tooltip 0x2xx, drag_icon 0x1xx, about 0x3xx, language_sync 0x300).
inline constexpr UINT kMsgOpenLanguagePalette = WM_APP + 0x503;

// Latches one pending open and posts a PURE (0,0) wake-up to `controller`
// (no pointer ever crosses PostMessageW — the SEC-ADJ house contract). MUST be
// called from the GUI thread, after the TrackPopupMenuEx dispatch decided the
// user picked the "검색…" row: entering a modal loop synchronously while the
// menu modality tear-down is still in flight is the REQ-047 D3 "minimized
// window, no input" defect. 261116_0003 P6c (PR6-32): the latch is PER-SURFACE
// — `init.surface` selects the slot — so a second post on the SAME surface
// while one is pending is still coalesced (the popup is single-instance per
// surface by design) but can never starve another surface. Returns false when
// the request was coalesced, the post failed (the slot is rolled back,
// nothing consumed it), or the surface tag is out of range (defensive; the
// enum is closed and every producer passes a literal).
bool PostDeferredLanguagePaletteOpen(HWND controller,
                                     const language_palette::PickerInit& init);

// Consumes the latched request for ONE surface (PR6-32; called by the two
// P6c consumers — main.cpp's controller arm for TrayTyping/TrayTooltip, the
// tooltip WndProc for TooltipPill). Returns nullopt when that surface's slot
// is empty. GUI thread only.
//
// The DEFAULT argument is PickerSurface::TooltipPill because the tooltip's
// WndProc arm is the historical no-arg caller: its window is the picker's
// controller for the pill surfaces, and a bare Take() must keep consuming the
// pill slot. main.cpp's controller arm owns the two TRAY surfaces and ALWAYS
// passes its surface explicitly — a defaulted take can never swallow a tray
// slot, and each consumer naming its own surface is what keeps a foreign
// request from ever being dispatched to the wrong pair (defence-in-depth on
// top of the per-surface slots themselves).
std::optional<language_palette::PickerInit> TakeDeferredLanguagePaletteOpen(
    language_palette::PickerSurface surface = language_palette::PickerSurface::TooltipPill);

// Shows the modal picker and runs its OWN message loop, then returns the
// outcome. GUI thread only; the call blocks until the popup closes.
//   owner   - owner HWND for z-order/activation (may be null)
//   init    - the invocation inputs (see PickerInit)
//   anchor  - physical-pixel screen point the popup is clamped around;
//             nullptr = the current cursor position (the tray/REQ-047 D3 path)
LanguagePickerResult ShowLanguageSearchPalette(HWND owner,
                                               const language_palette::PickerInit& init,
                                               const POINT* anchor = nullptr);

// 261116_0003 P6c fix round (PR6-42): TRUE while a language picker's modal
// loop is running (the module's single-instance guard state).
// V10F FUP-02: the flag is a std::atomic<bool> (acquire loads / release
// stores) — it is still written only from the GUI thread (ShowLanguageSearch
// Palette's entry + its OpenGuard), but it is now read from the keyboard/mouse
// HOOK threads too (main.cpp's ESC/dismissal exemptions), so the read side
// needs the atomic + acquire to stay data-race-free. The latch/take module
// state stays GUI-thread-only, same discipline as before. The deferred-open
// arms' contract: an arm that takes the deferred latch MUST consult this
// BEFORE taking — a
// wake-up delivered while a picker is already open arrives NESTED through that
// picker's own hand-rolled pump (GetMessageW(nullptr) drains every message on
// the GUI thread), and a take at that moment would consume a request the
// single-instance guard inside ShowLanguageSearchPalette then refuses to show
// (the PR6-42 silent drop). The nested delivery therefore leaves its request
// latched and exits; the arm whose show() owns the running loop re-takes the
// latched requests after its popup closes and serves them (the drain pass).
bool LanguagePickerShowing();

} // namespace emebalachat
