#include "tray.hpp"
#include "ui/theme.hpp"
#include "../diag_logger.hpp"
#include "asset_loader.hpp"
#include "../config.hpp"
#include "../i18n.hpp"
#include "../unicode_utils.hpp"
#include "language_search_palette.hpp" // 261116_0003 P6: the deferred picker open + ghost predicate
// 261116_0003 P9b-3 U4a: the badge fold composes from the manager dialog's OWN
// public landmarks (kBadgeSupportedPrefix / kBadgeUnsupportedPrefix /
// kBadgeMarkerSeparator / kListBadgeSeparator) so the menu rows and the
// manager list can never drift apart.
#include "user_language_manager_dialog.hpp"

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
    // 261108_0002 (WS3, v0.10.6): the "역번역 플로팅 표시 (번역 검증)" check row,
    // appended directly BELOW the auto-send row in the toggles group. 2035 is
    // the next free slot in the 2020~2054 control band (2030 auto-send, 2040
    // sound, 2050 badge, 2055 start-with-Windows) — no existing id is
    // renumbered and the 2010~2016 engine band stays untouched.
    ID_TRAY_BACK_TRANSLATE = 2035,
    ID_TRAY_SOUND = 2040,
    ID_TRAY_BADGE = 2050,
    ID_TRAY_START_WITH_WINDOWS = 2055,
    ID_TRAY_CHEATSHEET = 2060,
    ID_TRAY_ABOUT = 2065,
    ID_TRAY_EXIT = 2070,
    // 261116_0003 P3 fix round 3 (PR3-04 — the PR2-06 obligation from
    // fix-list-round2.md, re-pinned here against the 90-row registry): the
    // bands are sized by the LIVE registry, 90 source rows (AUTO + 89 targets,
    // GetSupportedLanguages()) and 89 target rows (GetTargetLanguages()). The
    // range comment names the last ID used; the static_asserts below check the
    // exclusive bound BASE + count. (REQ-025: TYPE pair - unchanged IDs.)
    ID_TRAY_SRC_BASE = 2100, // 2100..2189 (90 rows; exclusive end 2190)
    ID_TRAY_TGT_BASE = 2200, // 2200..2288 (89 targets; exclusive end 2289)
    // R6 Phase 6 (plan §5.4), expanded by REQ-037 (P4 B-4, design §2-Q3),
    // RESHAPED by 261119_0001 U4 (UI-language-89 phase 2, architect-pick.md
    // "Search-row command ID"): the UI-language submenu is a TWO-ROW launcher
    // now, not a flat endonym band -
    //   Auto   (2300)  inline muscle-memory row; its check mark tracks the
    //                  PERSISTED config value, not the resolved locale;
    //   검색…  (2301)  opens the shared searchable picker, which carries the
    //                  UI-locale pool (89 rows wide at phase 3, so a flat
    //                  band of endonyms can never represent it).
    // Both rows are static: no loop and no size-dynamic ID arithmetic here.
    // The retired flat band would have been 2301..2389 for 89 locales, and
    // 2381..2383 are the UI-theme commands (SystemTray::kCmdTheme* below) -
    // that collision is RECORDED here and pinned by the assert block below,
    // which binds this row to the theme band as its real neighbour.
    ID_TRAY_UILANG_AUTO = 2300,
    ID_TRAY_UILANG_SEARCH = 2301, // the retired band's historical first slot
    // UI Theme selector submenu (Fluent / Editorial / Monolith = 2381..2383).
    ID_TRAY_THEME_FLUENT = SystemTray::kCmdThemeFluent,
    ID_TRAY_THEME_EDITORIAL = SystemTray::kCmdThemeEditorial,
    ID_TRAY_THEME_MONOLITH = SystemTray::kCmdThemeMonolith,
    // REQ-025 (Phase A §2.1.A3-25): DRAG ("번역툴팁") pair submenu IDs. The
    // existing 2100/2200 bands stay TYPE-only (regression guard); the drag
    // pair gets fresh non-overlapping bands with the same 90/89 split:
    // 2400..2489 / 2500..2588 (exclusive ends 2490 / 2589).
    ID_TRAY_DRAG_SRC_BASE = 2400,
    ID_TRAY_DRAG_TGT_BASE = 2500,
    // W2 (R1-22, session 261029_0001): the 번역툴팁 group's Swap row — a plain
    // item BELOW the two drag pickers, in a fresh ≥2600 band (the 2500 band
    // ends at 2588, so 2600 kept an 11-ID gap — IDs 2589..2599 — for future
    // target languages; it was 63 IDs before the P3 registry growth; the gap
    // BELOW 2600 is still what the drag-target static_assert guards, since
    // 89 future targets must fit before 2600).
    // Label reuses MenuSwapLangs (zero new i18n keys); the dispatch mirrors the
    // type-swap AUTO-pivot policy through PlanLanguageSwap and applies it to
    // the DRAG pair (ApplyLanguageChange(LanguageContext::Drag, ...)).
    ID_TRAY_DRAG_SWAP = 2600,
    // 261116_0003 P6 (language-menu redesign, plan §5 P6 + D3): the "검색…"
    // search row and the pinned-absent ghost row, ONE PAIR PER LANGUAGE MENU
    // (four menus: TYPE source/target, DRAG source/target), in the ≥2601 band
    // directly above the Swap row. The search row opens the shared searchable
    // language picker (src/ui/language_search_palette.hpp) through the REQ-047
    // D3 deferred open posted in the dispatch below; the ghost row renders the
    // pinned language the ACTIVE engine does not offer (greyed + checked —
    // the "설정 유지됨" contract: the config value is preserved and still
    // honored raw, it is just not offered here). The language rows above/
    // below keep their UNCHANGED BASE+i IDs — these special rows are prepended
    // in menu ORDER, they never shift the language bands.
    ID_TRAY_SEARCH_SRC = 2601,
    ID_TRAY_SEARCH_TGT = 2602,
    ID_TRAY_SEARCH_DRAG_SRC = 2603,
    ID_TRAY_SEARCH_DRAG_TGT = 2604,
    ID_TRAY_GHOST_SRC = 2605,
    ID_TRAY_GHOST_TGT = 2606,
    ID_TRAY_GHOST_DRAG_SRC = 2607,
    // 2609..2864 is the P9b-2 "내 언어" user-row band (kMaxUserLanguages per
    // surface, four surfaces; the band asserts below pin the layout).
    ID_TRAY_GHOST_DRAG_TGT = 2608,
    // 261116_0003 P9b-3 U4a: the main menu's "내 언어 추가…" / "내 언어…"
    // rows. The two ids DIRECTLY above the P9b-2 user band — 2865 is exactly
    // kTrayUserLangBandEnd, so the rows stay contiguous with the band whose
    // entries they manage, inside the 16-bit WM_COMMAND space and below the
    // tooltip's own user band (2900, tooltip.hpp kTooltipCmdUserSrcBase; the
    // assert blocks on BOTH surfaces pin the two directions, the same
    // cross-surface discipline as the P9b-2 band). Both rows open modal
    // dialogs, so their dispatch arms DEFER the open to the controller
    // (WM_APP + 0x504 / + 0x505 — the kMsgOpenGgufManager house contract).
    ID_TRAY_USER_LANG_ADD = kTrayUserLangBandEnd,        // 2865
    ID_TRAY_USER_LANG_MANAGE = kTrayUserLangBandEnd + 1, // 2866
};

// REQ-037/B-4 compile-time guard for the band layout asserted above.
// 261119_0001 U4 retired the flat UILANG band (Auto + 37 selector codes =>
// 2300..2337), so the 37 - an I18N count, never a registry count - is gone
// from this file. Two facts still need a guard:
//   1. ADJACENCY: the 검색… row sits directly above Auto, so the group reads
//      as one unit in the menu and in the dispatch ladder.
//   2. THE THEME BAND: 2381..2383 are SystemTray::kCmdThemeFluent /
//      Editorial / Monolith - the band a re-growth of this row group hits
//      FIRST. The retired 89-locale layout (2301..2389) would have swallowed
//      them whole; the old assert compared against the DRAG band (2400), which
//      guarded nothing this side of 2381. This binds the real neighbour.
static_assert(ID_TRAY_UILANG_SEARCH == ID_TRAY_UILANG_AUTO + 1 &&
                  ID_TRAY_UILANG_SEARCH + 1 <= SystemTray::kCmdThemeFluent,
              "tray UI-language rows must be contiguous and stay below the theme band");

// 261116_0003 P3 fix round 3 (PR3-04, the PR2-06 obligation): the language-band
// guards are pinned to the LIVE registry size — 90 source rows (AUTO + 89
// targets) and 89 target rows (both pinned by run_tests.cpp's size checks).
// They stay literals because kAllLanguages is a runtime vector, so EVERY
// registry growth must re-pin them here in the same change:
//   2100 + 90 = 2190 <= 2200   (TYPE source stays inside its 100-ID slot)
//   2200 + 89 = 2289 <= 2300   (TYPE target stays below the UILANG row)
//   2400 + 90 = 2490 <= 2500   (drag source stays inside its slot)
//   2500 + 89 = 2589 <= 2600   (drag target keeps an 11-ID gap to the Swap row)
// P6/P9 OBLIGATION — recorded, never left silent: P9's user-defined languages
// join these SAME menus, so the per-row ID band must be re-designed (widened,
// or replaced by a dynamic command map) BEFORE that growth; static literals
// cannot absorb an unbounded user registry.
static_assert(ID_TRAY_SRC_BASE + 90 <= ID_TRAY_TGT_BASE,
              "tray TYPE source band (AUTO + 89 rows) must stay below the target band");
static_assert(ID_TRAY_TGT_BASE + 89 <= ID_TRAY_UILANG_AUTO,
              "tray TYPE target band (89 targets) must stay below the UI-language row");
static_assert(ID_TRAY_DRAG_SRC_BASE + 90 <= ID_TRAY_DRAG_TGT_BASE,
              "tray drag source band (AUTO + 89 rows) must stay below the drag-target band");
static_assert(ID_TRAY_DRAG_TGT_BASE + 89 <= ID_TRAY_DRAG_SWAP,
              "tray drag-target band (89 targets) must not collide with the drag-swap row");
static_assert(ID_TRAY_DRAG_SWAP >= 2600,
              "the drag-pair Swap row belongs to the >=2600 band (W2/R1-22)");

// 261116_0003 P6: the search/ghost band sits contiguous 2601..2608, directly
// above the Swap row — a later renumber must never wander a special row into a
// language band or the engine/control bands. (The P9 band-redesign obligation
// recorded above still owns any growth BEYOND this block.)
static_assert(ID_TRAY_SEARCH_SRC == ID_TRAY_DRAG_SWAP + 1 &&
                  ID_TRAY_SEARCH_TGT == ID_TRAY_SEARCH_SRC + 1 &&
                  ID_TRAY_SEARCH_DRAG_SRC == ID_TRAY_SEARCH_TGT + 1 &&
                  ID_TRAY_SEARCH_DRAG_TGT == ID_TRAY_SEARCH_DRAG_SRC + 1 &&
                  ID_TRAY_GHOST_SRC == ID_TRAY_SEARCH_DRAG_TGT + 1 &&
                  ID_TRAY_GHOST_TGT == ID_TRAY_GHOST_SRC + 1 &&
                  ID_TRAY_GHOST_DRAG_SRC == ID_TRAY_GHOST_TGT + 1 &&
                  ID_TRAY_GHOST_DRAG_TGT == ID_TRAY_GHOST_DRAG_SRC + 1,
              "the P6 search/ghost rows must stay the contiguous 2601..2608 band above the Swap row");

// 261116_0003 P9b-2 (plan §3 D5 + §3d.5 shared-surface discipline): the
// "내 언어" user-row band, the redesign the P6 block above recorded as an
// obligation (ID_TRAY_GHOST_DRAG_TGT's "2609+ stays free until the P9
// redesign"). The band is the flat computed layout from tray.hpp:
//   base 2609 + surface * kMaxUserLanguages + user index, four surfaces.
// The asserts below state the three properties the dispatcher relies on:
//   1. CONTIGUITY: the base is exactly the first free id after the P6 block,
//      so no gap can hide an id another band later claims.
//   2. DISJOINTNESS from every other tray band, by literal facts rather than a
//      range sweep: each assert names the highest id that band can reach, and
//      every one of them is strictly below 2609.
//   3. SLOT SEPARATION + headroom: each surface's last id stays below the next
//      surface's first id, and the exclusive end stays inside the 16-bit
//      WM_COMMAND space the menus are addressed in.
// Growing kMaxUserLanguages is a re-pin duty HERE, exactly like the registry
// band literals above (the merge caps the group at that constant, so 4 slots of
// that size always suffice).
static_assert(kTrayUserLangBase == ID_TRAY_GHOST_DRAG_TGT + 1,
              "the P9b-2 user-row band must start directly above the P6 ghost block (2609)");
static_assert(kTrayUserLangBase > ID_TRAY_SRC_BASE + 90 &&
                  kTrayUserLangBase > ID_TRAY_TGT_BASE + 89 &&
                  kTrayUserLangBase > ID_TRAY_UILANG_SEARCH &&
                  kTrayUserLangBase > SystemTray::kCmdThemeMonolith &&
                  kTrayUserLangBase > ID_TRAY_DRAG_SRC_BASE + 90 &&
                  kTrayUserLangBase > ID_TRAY_DRAG_TGT_BASE + 89 &&
                  kTrayUserLangBase > ID_TRAY_DRAG_SWAP &&
                  kTrayUserLangBase > ID_TRAY_EXIT,
              "the P9b-2 user-row band must stay above every existing tray band");
static_assert(kTrayUserLangStride == static_cast<UINT>(kMaxUserLanguages),
              "one user-row slot per surface must mirror the P9a registry cap");
static_assert(TrayUserLangCommand(TrayUserLangSurface::TypingSource, kMaxUserLanguages - 1) <
                      TrayUserLangCommand(TrayUserLangSurface::TypingTarget, 0) &&
                  TrayUserLangCommand(TrayUserLangSurface::TypingTarget, kMaxUserLanguages - 1) <
                      TrayUserLangCommand(TrayUserLangSurface::DragSource, 0) &&
                  TrayUserLangCommand(TrayUserLangSurface::DragSource, kMaxUserLanguages - 1) <
                      TrayUserLangCommand(TrayUserLangSurface::DragTarget, 0) &&
                  TrayUserLangCommand(TrayUserLangSurface::DragTarget, kMaxUserLanguages - 1) + 1 ==
                      kTrayUserLangBandEnd,
              "the four P9b-2 user-row slots must be contiguous and pairwise disjoint");
static_assert(kTrayUserLangBandEnd <= 0x10000,
              "the P9b-2 user-row band must stay inside the 16-bit menu-command space");

// 261116_0003 P9b-3 U4a: the two user-language dialog rows own the two ids
// directly above the P9b-2 band. The asserts state the three properties the
// layout relies on, in the band block's own idiom:
//   1. CONTIGUITY: the add row is exactly the band's exclusive end and the
//      manage row follows it — no gap another band can later claim.
//   2. HEADROOM TO THE TOOLTIP BAND: the tooltip's own user band starts at
//      2900 (tooltip.hpp kTooltipCmdUserSrcBase, pinned there by that header's
//      constants and tooltip.cpp's assert block, which pins the opposite
//      direction against kTrayUserLangBandEnd) — the rows must stay below it
//      so the two surfaces can never address one another's commands.
//   3. The 16-bit WM_COMMAND bound.
static_assert(ID_TRAY_USER_LANG_ADD == kTrayUserLangBandEnd &&
                  ID_TRAY_USER_LANG_MANAGE == ID_TRAY_USER_LANG_ADD + 1,
              "the P9b-3 user-language dialog rows must sit directly above the P9b-2 user band");
static_assert(ID_TRAY_USER_LANG_MANAGE < 2900,
              "the P9b-3 user-language dialog rows must stay below the tooltip user band "
              "(2900, tooltip.hpp kTooltipCmdUserSrcBase)");
static_assert(ID_TRAY_USER_LANG_MANAGE <= 0xFFFF,
              "the P9b-3 user-language dialog rows must stay inside the 16-bit menu-command space");

} // namespace

// 261116_0003 P9b-2: one "내 언어" row label. Native name first, the English
// name in parentheses only when the entry carries one; no code prefix (that is
// what distinguishes a user row from the registry rows' "CODE - display" form).
std::string FormatUserLanguageMenuLabel(const UserLanguage& language) {
    std::string label = language.native_name;
    if (!language.english_name.empty()) {
        label += " (";
        label += language.english_name;
        label += ")";
    }
    // 261116_0003 P9b-2 fix R14-05: every consumer passes this label to
    // AppendMenuW, which consumes '&' as the mnemonic marker. A user-typed
    // name containing '&' must render the literal character, else the row
    // reads textually different from the sanitized stored name (a crafted
    // name could even shadow a registry row's text while dispatching its
    // own payload). Double the marker for the menu API; the stored name and
    // the UserLanguageStoredName dispatch payload stay byte-identical.
    std::string menu_label;
    menu_label.reserve(label.size());
    for (const char ch : label) {
        menu_label.push_back(ch);
        if (ch == '&') {
            menu_label.push_back('&');
        }
    }
    return menu_label;
}

// 261116_0003 P9b-3 U4a: the ONE shared "내 언어" row badge fold — the contract
// lives on the declaration (tray.hpp). This is the ONLY body; all six menu
// surfaces (the tray's four submenus through build_language_menu and the
// tooltip's two dropdowns) compose label + suffix through it. The display
// states mirror the manager dialog's row fold exactly, from that module's OWN
// public landmarks, so the menu rows and the manager list can never drift.
std::wstring UserLanguageMenuBadgeSuffix(const UserLanguage& language,
                                         std::string_view probe_engine_id,
                                         std::string_view probe_model,
                                         bool in_flight) {
    namespace uld = user_language_manager_dialog;
    // No-probe engine context (D6: Google is publisher-declared; D7 + Auto:
    // UserLanguageEngineId returns "") — the row carries no verdict suffix.
    if (probe_engine_id.empty()) {
        return {};
    }
    // Runtime-only Checking (U3's in-flight set; the persisted slot never
    // holds Checking — ParseProbeStatusToken maps it to Unknown).
    if (in_flight) {
        return std::wstring(uld::kListBadgeSeparator) + I18n::Get(StringId::UserLangProbeChecking);
    }
    // The SAME key construction the U3 runner writes with: a different model
    // half here would address a DIFFERENT cache slot on purpose (D6 caches
    // per engine + model + language).
    const UserLanguageProbe* probe =
        FindUserLanguageProbe(language, probe_engine_id, probe_model);
    if (probe == nullptr) {
        return {}; // Unknown: nothing judged yet (D6 draws no badge)
    }
    switch (DisplayUserLanguageProbeStatus(*probe)) {
        case UserLanguageProbeStatus::Supported:
            return std::wstring(uld::kListBadgeSeparator) + uld::kBadgeSupportedPrefix +
                   I18n::Get(StringId::UserLangProbeSupported);
        case UserLanguageProbeStatus::Unsupported:
            return std::wstring(uld::kListBadgeSeparator) + uld::kBadgeUnsupportedPrefix +
                   I18n::Get(StringId::UserLangProbeUnsupported);
        case UserLanguageProbeStatus::Checking:
            // A persisted slot can still READ Checking (only the LOAD path maps
            // it to Unknown, config.cpp ParseProbeStatusToken); render it like
            // the runtime state, exactly as the manager fold does — never a
            // sixth look.
            return std::wstring(uld::kListBadgeSeparator) + I18n::Get(StringId::UserLangProbeChecking);
        case UserLanguageProbeStatus::UserOverridden: {
            // DisplayUserLanguageProbeStatus returns UserOverridden only when
            // override_status is a real answer (config.cpp), so the direction
            // is always one of the two verdicts; the marker rides the marker
            // separator (D6: the user is the final authority).
            const bool supported =
                probe->override_status == UserLanguageProbeStatus::Supported;
            return std::wstring(uld::kListBadgeSeparator) +
                   (supported ? uld::kBadgeSupportedPrefix : uld::kBadgeUnsupportedPrefix) +
                   I18n::Get(supported ? StringId::UserLangProbeSupported
                                       : StringId::UserLangProbeUnsupported) +
                   uld::kBadgeMarkerSeparator + I18n::Get(StringId::UserLangOverrideActive);
        }
        case UserLanguageProbeStatus::Unknown:
        default:
            return {};
    }
}

// 261116_0003 P9b-2 fix R14-02: the shared pin-to-row resolution. The contract
// lives on the declaration (tray.hpp); this is the ONE body every surface
// calls, so no surface can re-derive the match.
const UserLanguage* CurrentUserLanguageRow(const LanguageMenuView& merged,
                                           std::string_view pinned_raw) {
    if (pinned_raw.empty()) {
        return nullptr;
    }
    // Arm 1 (spelling) across the whole rendered group first: a pin that
    // literally carries one of an entry's own spellings names that entry, even
    // when another entry's code happens to resolve from the same text. The
    // single-entry vector keeps FindUserLanguage the only definition of the
    // spelling set (the suite pins the equivalence on an all-entries view).
    for (const LanguageMenuRow& row : merged.user_group) {
        const UserLanguage* entry = row.user;
        if (entry == nullptr || entry->native_name.empty()) {
            continue;
        }
        if (FindUserLanguage(std::vector<UserLanguage>{*entry}, pinned_raw) != nullptr) {
            return entry;
        }
    }
    // Arm 2 (resolved code): the planner's registry-first canonicalization
    // rewrites a pick to the registry spelling, so this is the arm that makes
    // "Korean" light a native-only KO entry's row. An unresolvable pin
    // normalizes to AUTO, which no stored entry may carry (config.cpp rejects
    // it at parse), so the guard also keeps that sentinel out of the match.
    const std::string resolved_code = NormalizeLanguageCode(pinned_raw);
    if (resolved_code.empty() || resolved_code == "AUTO") {
        return nullptr;
    }
    for (const LanguageMenuRow& row : merged.user_group) {
        const UserLanguage* entry = row.user;
        if (entry == nullptr || entry->code.empty()) {
            continue;
        }
        if (entry->code == resolved_code) {
            return entry;
        }
    }
    return nullptr;
}

// 261116_0003 P9b-2: the user half of the pinned-absent ghost. The registry
// half stays the shared palette predicate; the caller gates this half on the
// registry half's PRE-SUPPRESSION null (fix R14-01, at the menu build), and
// the helper itself answers one question from the authoritative merge: is the
// pinned entry offered anywhere in this view at all.
const UserLanguage* PinnedAbsentUserLanguage(const LanguageMenuView& merged,
                                             const std::vector<UserLanguage>& user_languages,
                                             std::string_view pinned_name) {
    const UserLanguage* pinned = FindUserLanguage(user_languages, pinned_name);
    if (pinned == nullptr) {
        return nullptr; // the pin names no registered entry: not a user ghost
    }
    const bool has_code = !pinned->code.empty();
    for (const LanguageMenuRow& row : merged.user_group) {
        if (row.user == pinned) {
            return nullptr; // offered as a "내 언어" row: not absent
        }
        // 261116_0003 P9b-2 fix R14-04: the merge dedupes the group by resolved
        // code and keeps only the FIRST entry of a duplicate code, so the
        // pinned entry can be the dropped twin while the language is still
        // offered as the surviving entry's row. A group row with the same code
        // is the same language: not absent.
        if (has_code && row.user != nullptr && row.user->code == pinned->code) {
            return nullptr; // offered as another entry's "내 언어" row
        }
    }
    if (has_code) {
        for (const LanguageInfo* row : merged.registry) {
            if (row != nullptr && row->code == pinned->code) {
                return nullptr; // offered through its resolved registry row
            }
        }
    }
    return pinned;
}

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

// 261108_0002 (WS3): mirror of AppConfig::back_translation_enabled for the
// "역번역 플로팅 표시" check mark (same lazy-rebuild contract as
// SetUiLanguage - ShowContextMenu re-reads this on every open).
void SystemTray::SetBackTranslationEnabled(bool enabled) {
    back_translation_enabled_ = enabled;
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
    // 261116_0003 P9b-3 F-1 (R15-12): the src/tgt/drag codes can BE
    // user-typed stored names (a user-language pick stores its name as the
    // planner token), so the pair fields are content: REQ-003 records them
    // only behind diag::ContentLoggingEnabled() (default off) — the same
    // gate R14-06 gave menu_select_user; the else branch keeps the
    // shape-only len= fields.
    if (diag::ContentLoggingEnabled()) {
        DIAG_LOG("UI", "tray_update active=%d engine=%s src=%s tgt=%s drag_src=%s drag_tgt=%s auto_send=%d icon_changed=%d",
                 active ? 1 : 0, active_engine_.c_str(),
                 std::string(src_code).c_str(), std::string(tgt_code).c_str(),
                 std::string(drag_src_code).c_str(), std::string(drag_tgt_code).c_str(),
                 auto_send ? 1 : 0, iconChanged ? 1 : 0);
    } else {
        DIAG_LOG("UI", "tray_update active=%d engine=%s src_len=%zu tgt_len=%zu drag_src_len=%zu drag_tgt_len=%zu auto_send=%d icon_changed=%d",
                 active ? 1 : 0, active_engine_.c_str(),
                 src_code.size(), tgt_code.size(),
                 drag_src_code.size(), drag_tgt_code.size(),
                 auto_send ? 1 : 0, iconChanged ? 1 : 0);
    }
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
    // REQ-056 (derive-at-open): the submenu's config-derived values are
    // resolved NOW, at menu build time, from main.cpp (the single authority)
    // — the tray keeps no cached copies, so no push path can go stale (the
    // 260922 session's F9 stem-clobber and check-mark bugs both lived in those
    // caches). Without a resolver the defaults render Google checked, keep the
    // single-view ghost (is_auto false) and show no user-model entry (engine
    // submenu degenerates to built-ins + manager).
    const EngineMenuView view = resolver_ ? resolver_() : EngineMenuView{};
    const int pref = view.preferred_engine;
    // 261116_0003 P6a fix round r2 (PR6-07): index 0 covers BOTH "google" and
    // "auto", but only an explicitly persisted "auto" lets the router serve a
    // HyMt2-only pin (a persisted "google" latches active_type_ = Google in
    // RefreshActiveEngine, engine.cpp:313-315, and Translate() consults the
    // plan only under active == LocalLlama, engine.cpp:685-691). The token
    // therefore scopes the composed ghost gate below to auto.
    const bool is_auto = view.is_auto;
    // 261116_0003 P6a fix round r4 (PR6-19): the availability half, the
    // router's OWN HostCouldServeFor (engine.cpp:288-290) = enabled + the host
    // binary present. Its binary half is a LIVE probe while the router's local
    // leg is the LATCHED active_type_ (refreshed only in RefreshActiveEngine,
    // engine.cpp:304), so a binary that appears after the last refresh reads
    // TRUE here while Auto still routes Google and the ghost stays suppressed
    // in that window (the one divergence direction, scoped in tray.hpp's
    // rationale). Under auto with the host absent at the last refresh the
    // router picks Google (engine.cpp:357-359) and never consults the routing
    // plan, so a HyMt2-only pin is honored by NO leg and the truthful ghost
    // must SHOW.
    // (Copied into a local because the language-menu lambda's own `view`
    // parameter — the registry language view — shadows this EngineMenuView.)
    const bool local_host_available = view.local_host_available;
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
    // the manager row below. While registered, the " - <stem>" suffix rides
    // the label REGARDLESS of the checked engine: the old code appended the
    // suffix only at pref==3, so switching to another engine made the entry
    // read like nothing was registered. The frozen REQ-046 radio contract is
    // untouched (same ID, same 4-way check mark, conditional append), and the
    // localized "(미등록)" placeholder is gone together with the always-append
    // entry.
    if (!user_model_stem.empty()) {
        std::wstring user_gguf_label = I18n::Get(StringId::MenuEngineUserGguf);
        user_gguf_label += L" - " + ToUtf16(user_model_stem);
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

    // 2b. R6 Phase 6 (plan §5.4) + 261119_0001 U4 (UI-language-89 phase 2,
    // architect-pick.md "Search-row command ID" + "Current-locale signal in
    // closed menu"): the UI-language submenu is a TWO-ROW launcher now — the
    // flat endonym band is retired (89 locales can never fit it, and its
    // would-be 2301..2389 range collided with the theme band 2381..2383):
    //   [✓] Auto        ID_TRAY_UILANG_AUTO, logic UNCHANGED: the check mark
    //                   tracks the PERSISTED config value (ui_language_), not
    //                   the resolved locale, so "auto" stays visibly selected
    //                   while it follows the OS language;
    //   ─── separator ───
    //   검색…           ID_TRAY_UILANG_SEARCH, the SAME label idiom as the four
    //                   translation "검색…" rows (StringId::MenuSearchLangs, so
    //                   zero new i18n keys): it opens the shared searchable
    //                   picker through the REQ-047 D3 deferred open dispatched
    //                   below. The UI-locale pool, its search, its AUTO row and
    //                   its ordering all live in the picker.
    // The submenu TITLE carries the active locale's endonym — "UI 언어 (한국어)"
    // while a concrete selector locale is active, and the plain title for
    // auto/empty and for any unknown persisted value (never a row the selector
    // does not offer). The endonym is DATA (UiLocaleEntry::native_name), so no
    // new StringId is involved, and the lookup reuses the selector list plus
    // the exact equality test the retired per-row check marks used.
    HMENU hUiLangMenu = ::CreatePopupMenu();
    const bool autoChecked = (ui_language_ == "auto" || ui_language_.empty());
    std::wstring uiLangTitle = I18n::Get(StringId::MenuUiLanguage);
    for (const UiLocaleEntry& entry : GetSupportedUiLocales()) {
        if (!autoChecked && ui_language_ == I18n::LocaleToString(entry.locale)) {
            uiLangTitle += L" (";
            uiLangTitle += entry.native_name;
            uiLangTitle += L")";
            break;
        }
    }
    {
        ::AppendMenuW(hUiLangMenu, MF_STRING | (autoChecked ? MF_CHECKED : MF_UNCHECKED),
                      ID_TRAY_UILANG_AUTO, I18n::Get(StringId::MenuUiLanguageAuto).c_str());
        ::AppendMenuW(hUiLangMenu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(hUiLangMenu, MF_STRING, ID_TRAY_UILANG_SEARCH,
                      I18n::Get(StringId::MenuSearchLangs).c_str());
    }
    ::AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hUiLangMenu), uiLangTitle.c_str());

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
    // 261116_0003 P6 (language-menu redesign, plan §5 P6 + D3): the four
    // language menus build from the ACTIVE ENGINE's registry view instead of
    // the global lists — GetLanguagesForEngine(TrayLanguageEngineFor(pref),
    // include_auto), include_auto=true on the two SOURCE menus (AUTO stays a
    // first-class source setting), false on the two TARGET menus (AUTO can
    // never be a translation target). P9b-2 (below) layers the "내 언어" merge
    // on the same views: BuildLanguageMenuView wraps that call, so the view
    // locals are the build-time AND dispatch-time payload. Their row POINTERS
    // reference config.cpp's immutable statics; the wrapping vectors are
    // locals, and the dispatch range-checks and name lookups index the SAME
    // locals the menu was built from (never re-fetch a different list between
    // build and dispatch).
    //
    // Menu shape per designer §4.1 + plan §4: the "검색…" row (MenuSearchLangs,
    // >=2601 band) first, a separator, then — only when the active engine does
    // NOT offer the menu's current value — the pinned-absent GHOST row
    // (greyed + checked: the "설정 유지됨" contract — the config value is
    // preserved verbatim and still honored raw by the engine adapter, it is
    // just not offered by this engine), then the engine-view rows with their
    // UNCHANGED BASE+i IDs (the special rows are prepended in menu order and
    // never shift the language bands). The ghost predicate is the picker's OWN
    // tested capability check (PickerState::PinnedAbsentLanguage —
    // phase-note-p5 §5: reuse it, never re-implement the capability check),
    // driven by the SAME PickerInit the search row posts to the deferred open,
    // so the menu and the picker can never disagree about (engine, role,
    // current codes).
    const LanguageEngine menu_engine = TrayLanguageEngineFor(pref);
    // 261116_0003 P9b-2 (plan §3 D5 + §4 Phase 3): the D5/D7 merge is the only
    // menu data source now. BuildLanguageMenuView returns the engine's registry
    // rows PLUS the "내 언어" group (the local Hy-MT2 view never carries one,
    // D7; Google carries only entries whose code is Google-servable; the
    // free-form engines carry every entry), and removes every registry row a
    // user entry claimed, so a language renders once per menu. The merge is
    // authoritative: this surface adds no filtering of its own.
    // The views are LOCALS alive across TrackPopupMenuEx because the group's
    // rows point into `users` (the resolver's copy) and every dispatch lookup
    // indexes these SAME vectors the menu was built from (the P6 build-time
    // payload discipline: never re-fetch a different list between build and
    // dispatch).
    const std::vector<UserLanguage>& users = view.user_languages;
    const LanguageMenuView srcMenu =
        BuildLanguageMenuView(menu_engine, /*include_auto=*/true, users);
    const LanguageMenuView tgtMenu =
        BuildLanguageMenuView(menu_engine, /*include_auto=*/false, users);

    std::string normSrc = NormalizeLanguageCode(src_code_);
    std::string normTgt = NormalizeLanguageCode(tgt_code_);
    std::string normDragSrc = NormalizeLanguageCode(drag_src_code_);
    std::string normDragTgt = NormalizeLanguageCode(drag_tgt_code_);

    // The four (surface, role) picker requests, latched at menu-build time.
    // They drive BOTH the ghost probes below AND the REQ-047 D3 deferred open
    // in the dispatch. The request INTENTIONALLY SNAPSHOTS the build-time
    // pair: the menu is modal, so the dispatch path cannot re-derive the
    // values (one request object per menu) — and while the hook thread's
    // tray_.UpdateStatus CAN change src_code_/drag_src_code_ mid-menu, that
    // only affects the NEXT build; it never rewrites this in-flight snapshot
    // (261116_0003 P6a fix round, PR6-03d — the earlier "nothing can change
    // the pair" claim was too absolute).
    const auto make_picker_request = [&](language_palette::PickerSurface surface,
                                         language_palette::PickerRole role,
                                         const std::string& src,
                                         const std::string& tgt) {
        language_palette::PickerInit req;
        req.surface = surface;
        req.role = role;
        req.engine = menu_engine;
        req.source_code = src;
        req.target_code = tgt;
        return req;
    };
    const language_palette::PickerInit typing_src_req =
        make_picker_request(language_palette::PickerSurface::TrayTyping,
                            language_palette::PickerRole::Source, normSrc, normTgt);
    const language_palette::PickerInit typing_tgt_req =
        make_picker_request(language_palette::PickerSurface::TrayTyping,
                            language_palette::PickerRole::Target, normSrc, normTgt);
    const language_palette::PickerInit drag_src_req =
        make_picker_request(language_palette::PickerSurface::TrayTooltip,
                            language_palette::PickerRole::Source, normDragSrc, normDragTgt);
    const language_palette::PickerInit drag_tgt_req =
        make_picker_request(language_palette::PickerSurface::TrayTooltip,
                            language_palette::PickerRole::Target, normDragSrc, normDragTgt);

    // 261119_0001 U4 (UI-language-89 phase 2): the UI-language submenu's ONE
    // picker request, latched here beside the four translation requests and by
    // the same build-time snapshot discipline (the menu is modal, so the
    // dispatch path cannot re-derive the value). Its shape is deliberately NOT
    // make_picker_request's:
    //   surface  TrayUiLanguage — the picker's mode discriminator: flat
    //            authored-only UI pool, Tab flip refused, engine chip hidden;
    //   role     Source — this surface ignores the role (a UI-chrome locale
    //            has no translation direction); the enum has no "none" value;
    //   engine   Auto — INERT on this surface (the UI pool is engine-mask
    //            independent), named explicitly so it can never be mistaken for
    //            the active translation engine the other four requests carry;
    //   source   the canonical config-code spelling of the persisted locale:
    //            LocaleToString(StringToLocale(x)) canonicalizes case and the
    //            legacy aliases ("zh_cn", "zh_tw", "zh") to "ko"/"zh-CN"/...;
    //            "auto" lands on StringToLocale's own auto branch. The feed is
    //            the submenu's autoChecked PREDICATE, not the raw string, so an
    //            empty persisted value preselects the pool's AUTO row exactly
    //            like the Auto check mark reads it - StringToLocale("") would
    //            otherwise fall through to its English fallback and preselect
    //            EN under a checked Auto row. One predicate, one meaning.
    //   target   empty — there is no second slot to preselect.
    language_palette::PickerInit ui_lang_req;
    ui_lang_req.surface = language_palette::PickerSurface::TrayUiLanguage;
    ui_lang_req.role = language_palette::PickerRole::Source;
    ui_lang_req.engine = LanguageEngine::Auto;
    ui_lang_req.source_code =
        I18n::LocaleToString(I18n::StringToLocale(autoChecked ? "auto" : ui_language_));
    ui_lang_req.target_code.clear();

    // One builder for all four language submenus (261116_0003 P6): the
    // "검색…" row, a separator, the optional pinned-absent ghost row, then the
    // "내 언어" group (P9b-2, non-empty only where the merge offers user
    // entries), then the engine-view rows with their UNCHANGED BASE+i IDs.
    const auto build_language_menu = [&](HMENU menu,
                                         UINT search_cmd,
                                         UINT ghost_cmd,
                                         UINT row_base,
                                         TrayUserLangSurface surface,
                                         const LanguageMenuView& merged,
                                         const std::string& current,
                                         const std::string& current_raw,
                                         const language_palette::PickerInit& ghost_probe) {
        const UINT user_base = TrayUserLangCommand(surface, 0);
        ::AppendMenuW(menu, MF_STRING, search_cmd,
                      I18n::Get(StringId::MenuSearchLangs).c_str());
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        // 261116_0003 P6a fix round (PR6-01) + r2 (PR6-07) + r3 (PR6-10) +
        // r4 (PR6-19), composed gate now the SHARED predicate (P6b fix round,
        // PR6-28): ghost the pinned row only when no engine leg the ROUTER
        // can pick would serve it. The Google view is what renders under BOTH
        // "google" and "auto", but the legs differ: the auto router serves
        // local-capable pairs from the local engine (PlanTranslationRouting /
        // LocalPairReliable), while an explicit "google" pin latches
        // active_type_ = Google (engine.cpp:313-315) and never reaches the
        // plan (engine.cpp:685-691). The shared predicate composes the four
        // conjuncts hardened across the P6a rounds, each with one job:
        //   preferred_engine == 0  the Google view; concrete engines (1/2/3)
        //                          keep the single-view ghost (R2 PR6-07)
        //   is_auto                the RAW config token, which the collapsed
        //                          index cannot convey — only a persisted
        //                          "auto" may route a pair locally at all
        //   local_host_available   the router's OWN HostCouldServeFor
        //                          (engine.cpp:288-290), a LIVE binary check
        //                          rather than the router's latched local leg:
        //                          under auto with the host leg dead
        //                          (Zero-Install) the router picks Google
        //                          (engine.cpp:357-359) and never consults
        //                          PlanTranslationRouting, so a HyMt2-only pin
        //                          is honored by NO leg — Google cannot serve
        //                          it — and the truthful ghost must SHOW
        //                          (the tray's resolver view carries this
        //                          token, unlike this lambda's `merged` param,
        //                          which is the merged language view)
        //   LocalPairReliable(...) the router's OWN pair predicate, PAIR-WISE
        //                          (config.cpp:1280-1307 — the TARGET must
        //                          carry kLangHyMt2 and the source must be
        //                          AUTO or also carry kLangHyMt2), so it
        //                          answers exactly what the router answers
        //                          under auto
        // The r2 round's pair-blind view-membership probe defect and the r4
        // Zero-Install window are documented where the predicate lives
        // (language_search_palette.hpp) — ONE DECISION shared by BOTH this
        // lambda and the tooltip's two dropdowns, over per-surface INPUTS
        // (PR6-38): main.cpp wires THIS surface's resolver
        // (main.cpp:2989-3018) and the tooltip's SetEngineResolver
        // (main.cpp:3171-3184) from the SAME sources, so is_auto /
        // local_host_available are the live router view on both surfaces and
        // the two surfaces now agree on the ghost row. The pre-P6c
        // interim (tooltip unwired, conservative defaults, truthful ghost
        // where the tray suppressed) is gone; the safe direction it relied on
        // was over-information, never wrongful suppression — the P6a defect
        // class was wrongful SUPPRESSION. ghost_probe carries
        // the normalized codes (normSrc/normTgt above) and LocalPairReliable
        // normalizes internally anyway. FALSE (explicit "google", auto with
        // the host leg not live, or a tray with no resolver, the default) and
        // concrete engines (1/2/3, pref != 0) keep the truthful single-view
        // ghost.
        const LanguageInfo* ghost =
            language_palette::PickerState{ghost_probe}.PinnedAbsentLanguage();
        // 261116_0003 P9b-2 fix R14-01: capture "the registry half found a
        // ghost" BEFORE the shared gate below decides. The user half (the else
        // branch) runs only on this PRE-SUPPRESSION null, so a pin whose ghost
        // the shared gate suppresses (AUTO + live host + a locally reliable
        // pair, e.g. a Hy-MT2-only code such as BO) renders NOTHING, exactly
        // like an identical registry pin, instead of the user half
        // re-rendering the greyed row the one shared suppression decision
        // just rejected.
        const bool registry_ghost_found = ghost != nullptr;
        if (ghost != nullptr &&
            language_palette::SuppressPinnedAbsentGhost(ghost_probe, pref, is_auto,
                                                        local_host_available)) {
            ghost = nullptr;
        }
        if (ghost != nullptr) {
            const std::wstring ghost_text =
                ToUtf16(ghost->code) + L" - " + I18n::GetLanguageDisplayName(ghost->code);
            ::AppendMenuW(menu, MF_STRING | MF_GRAYED | MF_CHECKED, ghost_cmd,
                          ghost_text.c_str());
        } else if (!registry_ghost_found) {
            // 261116_0003 P9b-2: the USER half of the same ghost contract, in
            // the SAME row slot and with the SAME command id (so the
            // documented no-op dispatch arm covers it, and no new id is
            // spent). Reached only when the registry half found nothing BEFORE
            // the shared suppression gate decides (fix R14-01): either the pin
            // is not a registry language at all, or its row is offered here;
            // a code-carrying pin whose ghost the gate suppressed never
            // reaches this branch. The helper answers from the merge, not
            // from a capability rule: a pinned entry the authoritative view
            // does not offer is exactly as "설정 유지됨" as a registry pin the
            // engine cannot serve, and it must not disappear silently after
            // an engine switch. Greyed + checked, exactly like the registry
            // ghost row above; the label is the user row form (no code
            // prefix), so it reads as the pinned entry, not as a registry row.
            if (const UserLanguage* user_ghost =
                    PinnedAbsentUserLanguage(merged, users, current_raw)) {
                const std::wstring ghost_text = ToUtf16(FormatUserLanguageMenuLabel(*user_ghost));
                ::AppendMenuW(menu, MF_STRING | MF_GRAYED | MF_CHECKED, ghost_cmd,
                              ghost_text.c_str());
            }
        }
        // 261116_0003 P9b-2 (plan §3 D5 "내 언어 group atop cloud menus"): the
        // group renders only where the merge offers user entries - never under
        // the local Hy-MT2 engine (D7), and under Google only for entries whose
        // code is publisher-declared. Position: BELOW the search row, the
        // separator and the optional ghost row (all three keep their P6
        // positions), ABOVE the registry rows. The header is a DISABLED item
        // (non-selectable, id 0: a header is not a command) using the P9b-1
        // string; the rows carry the computed user band, one slot per surface.
        // The check mark comes from the ONE shared pin-to-row resolution
        // (CurrentUserLanguageRow, fix R14-02), never a per-surface spelling
        // lookup: it answers over the RENDERED group (so the marked row is
        // always a row this menu draws) and it closes the planner's
        // registry-first round trip (a native-only entry persists as the
        // registry spelling and is found again through its resolved code).
        // `user_row_checked` threads that answer into the registry rule below,
        // so the AUTO row cannot double mark a setting the user row already
        // owns: one setting, exactly one check mark. The count is capped at the
        // same kMaxUserLanguages the merge caps at, which keeps row ids inside
        // their surface's slot (the band static_asserts).
        const UserLanguage* current_user = CurrentUserLanguageRow(merged, current_raw);
        const bool user_row_checked = current_user != nullptr;
        if (!merged.user_group.empty()) {
            ::AppendMenuW(menu, MF_STRING | MF_DISABLED, 0,
                          I18n::Get(StringId::UserLangsGroup).c_str());
            const size_t user_count = (std::min)(merged.user_group.size(),
                                                 static_cast<size_t>(kMaxUserLanguages));
            for (size_t i = 0; i < user_count; ++i) {
                const UserLanguage* entry = merged.user_group[i].user;
                if (entry == nullptr) {
                    continue;
                }
                // 261116_0003 P9b-3 U4a: the probe badge rides the row through
                // the ONE shared fold (tray.hpp), keyed by this menu build's
                // resolver view — the same derive-at-open (engine, model) key
                // the tooltip's dropdowns read, so one verdict renders
                // identically on all six surfaces. The in-flight seam is the
                // resolver's per-entry predicate (the stored-name keying
                // lives inside the seam, main.cpp); an EMPTY seam (no
                // resolver) means simply not in flight. Display-only: the
                // check mark above and the registry rule below are untouched.
                const bool probe_in_flight =
                    view.user_lang_probe_in_flight &&
                    view.user_lang_probe_in_flight(*entry);
                const std::wstring item_text =
                    ToUtf16(FormatUserLanguageMenuLabel(*entry)) +
                    UserLanguageMenuBadgeSuffix(*entry, view.user_lang_probe_engine,
                                                view.user_lang_probe_model, probe_in_flight);
                const bool is_current = (entry == current_user);
                ::AppendMenuW(menu, MF_STRING | (is_current ? MF_CHECKED : MF_UNCHECKED),
                              user_base + static_cast<UINT>(i), item_text.c_str());
            }
            ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }
        for (size_t i = 0; i < merged.registry.size(); ++i) {
            std::wstring item_text =
                ToUtf16(merged.registry[i]->code) + L" - " + I18n::GetLanguageDisplayName(merged.registry[i]->code);
            // 261116_0003 P9b-2 + fix R14-02: the shared check-mark rule, so a
            // code-less user pin (which normalizes to AUTO) or an AUTO-spelled
            // user entry cannot also light the AUTO row while its own "내 언어"
            // row is checked.
            const bool is_current = IsRegistryMenuRowCurrent(
                merged.registry[i]->code, current, current_raw, user_row_checked);
            ::AppendMenuW(menu, MF_STRING | (is_current ? MF_CHECKED : MF_UNCHECKED),
                          row_base + static_cast<UINT>(i), item_text.c_str());
        }
    };

    HMENU hSrcMenu = ::CreatePopupMenu();
    build_language_menu(hSrcMenu, ID_TRAY_SEARCH_SRC, ID_TRAY_GHOST_SRC,
                        ID_TRAY_SRC_BASE, TrayUserLangSurface::TypingSource,
                        srcMenu, normSrc, src_code_, typing_src_req);

    HMENU hTgtMenu = ::CreatePopupMenu();
    build_language_menu(hTgtMenu, ID_TRAY_SEARCH_TGT, ID_TRAY_GHOST_TGT,
                        ID_TRAY_TGT_BASE, TrayUserLangSurface::TypingTarget,
                        tgtMenu, normTgt, tgt_code_, typing_tgt_req);

    HMENU hTypingGroup = ::CreatePopupMenu();
    ::AppendMenuW(hTypingGroup, MF_POPUP, reinterpret_cast<UINT_PTR>(hSrcMenu), I18n::Get(StringId::MenuSourceLang).c_str());
    ::AppendMenuW(hTypingGroup, MF_POPUP, reinterpret_cast<UINT_PTR>(hTgtMenu), I18n::Get(StringId::MenuTargetLang).c_str());
    ::AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hTypingGroup), I18n::Get(StringId::MenuTypingGroup).c_str());

    // 4b. Drag ("번역툴팁") pair — check marks follow drag_src_code_/drag_tgt_code_.
    // 261116_0003 P6: same engine-view + 검색…/ghost shape as the type pair
    // above, surface TrayTooltip (the picker's phase-note §3 mapping).
    HMENU hDragSrcMenu = ::CreatePopupMenu();
    build_language_menu(hDragSrcMenu, ID_TRAY_SEARCH_DRAG_SRC, ID_TRAY_GHOST_DRAG_SRC,
                        ID_TRAY_DRAG_SRC_BASE, TrayUserLangSurface::DragSource,
                        srcMenu, normDragSrc, drag_src_code_, drag_src_req);

    HMENU hDragTgtMenu = ::CreatePopupMenu();
    build_language_menu(hDragTgtMenu, ID_TRAY_SEARCH_DRAG_TGT, ID_TRAY_GHOST_DRAG_TGT,
                        ID_TRAY_DRAG_TGT_BASE, TrayUserLangSurface::DragTarget,
                        tgtMenu, normDragTgt, drag_tgt_code_, drag_tgt_req);

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

    // 4c. 261116_0003 P9b-3 U4a: the "내 언어" management rows, directly below
    // the two language submenu groups they manage (pinned position: above the
    // Swap row). BOTH open modal dialogs, so both are PLAIN items (no check
    // geometry) and BOTH defer their open to the controller in the dispatch
    // below (the kMsgOpenGgufManager REQ-047 D3 contract). The add row reuses
    // the P9b-1 dialog title as its label (the copy the add dialog itself
    // shows); the manage row reuses the group's own name — the manager dialog
    // it opens carries that same title (U4b's recorded copy gap: no dedicated
    // "관리…" id landed in P9b-1, and this unit adds no StringIds), so the
    // affordance reads as "open my languages". The trailing ellipsis marks a
    // dialog-opening row (the codebase's menu convention; the P9b-1 strings
    // carry no ellipsis of their own, so it is appended here).
    ::AppendMenuW(hMenu, MF_STRING, ID_TRAY_USER_LANG_ADD,
                  (I18n::Get(StringId::UserLangAddTitle) + L"\u2026").c_str());
    ::AppendMenuW(hMenu, MF_STRING, ID_TRAY_USER_LANG_MANAGE,
                  (I18n::Get(StringId::UserLangsGroup) + L"\u2026").c_str());

    // 5. Swap languages
    ::AppendMenuW(hMenu, MF_STRING, ID_TRAY_SWAP, I18n::Get(StringId::MenuSwapLangs).c_str());
    ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // 6. Modes and toggles
    ::AppendMenuW(hMenu, MF_STRING | (auto_send_ ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_AUTOSEND, I18n::Get(StringId::MenuAutoSend).c_str());
    // 261108_0002 (WS3): floating back-translation verification card toggle,
    // directly below the auto-send row (its natural neighbour in the toggles
    // group). Checkable row; the card itself is drawn by the UI layer.
    ::AppendMenuW(hMenu, MF_STRING | (back_translation_enabled_ ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_BACK_TRANSLATE, I18n::Get(StringId::MenuBackTranslation).c_str());
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
    } else if (cmd == ID_TRAY_USER_LANG_ADD) {
        // 261116_0003 P9b-3 U4a: "내 언어 추가…" — open the add-language dialog
        // (U2's ShowUserLanguageDialogEx). The modal open is DEFERRED to the
        // controller window's GUI thread (kMsgOpenUserLangAdd = WM_APP + 0x504,
        // main.cpp) — the IDENTICAL REQ-047 D3 contract as the manager row
        // above: entering a modal synchronously here, while the
        // TrackPopupMenuEx modality tear-down may still be in flight, is the
        // "minimized window, no input" root cause. Pure (0,0) wake-up, no
        // payload crosses PostMessageW; main.cpp owns the dialog, the
        // persist-on-kAdded and the tray refresh. No coalescing latch is
        // needed: a second dispatch cannot run while the modal dialog blocks
        // this GUI thread's message handling.
        if (::PostMessageW(hOwner_, WM_APP + 0x504, 0, 0) == FALSE) {
            DIAG_F("UI/Tray/003: PostMessage user-language add open failed (GLE %lu)\n",
                   ::GetLastError());
        }
    } else if (cmd == ID_TRAY_USER_LANG_MANAGE) {
        // 261116_0003 P9b-3 U4a: "내 언어…" — open the manager dialog
        // (U4b's ShowUserLanguageManagerDialogEx with the current engine +
        // model resolution). The same deferred-open contract as the add row
        // (kMsgOpenUserLangManager = WM_APP + 0x505, main.cpp).
        if (::PostMessageW(hOwner_, WM_APP + 0x505, 0, 0) == FALSE) {
            DIAG_F("UI/Tray/004: PostMessage user-language manager open failed (GLE %lu)\n",
                   ::GetLastError());
        }
    } else if (cmd == ID_TRAY_SWAP && callbacks_.on_swap_languages) {
        callbacks_.on_swap_languages();
    } else if (cmd == ID_TRAY_AUTOSEND && callbacks_.on_toggle_auto_send) {
        callbacks_.on_toggle_auto_send();
    } else if (cmd == ID_TRAY_BACK_TRANSLATE && callbacks_.on_toggle_back_translate) {
        // 261108_0002 (WS3): flip AppConfig::back_translation_enabled. main.cpp
        // owns the persist + the SetBackTranslationEnabled refresh.
        callbacks_.on_toggle_back_translate();
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
    } else if (cmd >= ID_TRAY_SRC_BASE && cmd < ID_TRAY_SRC_BASE + srcMenu.registry.size()) {
        // 261116_0003 P6: the range resolves through the SAME engine view the
        // menu was built from (srcMenu, captured above — never a re-fetched
        // list), so the name_en payload stays byte-identical to the row shown.
        size_t idx = cmd - ID_TRAY_SRC_BASE;
        if (callbacks_.on_select_source_lang) {
            callbacks_.on_select_source_lang(srcMenu.registry[idx]->name_en);
        }
    } else if (cmd >= ID_TRAY_TGT_BASE && cmd < ID_TRAY_TGT_BASE + tgtMenu.registry.size()) {
        size_t idx = cmd - ID_TRAY_TGT_BASE;
        if (callbacks_.on_select_target_lang) {
            callbacks_.on_select_target_lang(tgtMenu.registry[idx]->name_en);
        }
    } else if (cmd >= ID_TRAY_DRAG_SRC_BASE && cmd < ID_TRAY_DRAG_SRC_BASE + srcMenu.registry.size()) {
        // REQ-025: drag ("번역툴팁") pair picks route to the drag callbacks.
        size_t idx = cmd - ID_TRAY_DRAG_SRC_BASE;
        if (callbacks_.on_select_drag_source_lang) {
            callbacks_.on_select_drag_source_lang(srcMenu.registry[idx]->name_en);
        }
    } else if (cmd >= ID_TRAY_DRAG_TGT_BASE && cmd < ID_TRAY_DRAG_TGT_BASE + tgtMenu.registry.size()) {
        size_t idx = cmd - ID_TRAY_DRAG_TGT_BASE;
        if (callbacks_.on_select_drag_target_lang) {
            callbacks_.on_select_drag_target_lang(tgtMenu.registry[idx]->name_en);
        }
    } else if (cmd == ID_TRAY_SEARCH_SRC || cmd == ID_TRAY_SEARCH_TGT ||
               cmd == ID_TRAY_SEARCH_DRAG_SRC || cmd == ID_TRAY_SEARCH_DRAG_TGT) {
        // 261116_0003 P6 (D3 + REQ-047 D3): the "검색…" row latches a request
        // for the shared searchable language picker and posts it through the
        // DEFERRED open — the post lands on the controller window immediately
        // AFTER TrackPopupMenuEx returned, the same modality contract as the
        // manager row above (entering the picker's modal loop synchronously
        // while the menu modality tear-down may still be in flight is the
        // REQ-047 D3 "minimized window, no input" defect). The request latched
        // at menu-build time is posted verbatim: (surface, role) name the
        // callback pair main.cpp's ControllerWndProc handler routes to
        // (main.cpp:1006-1101), engine is the active engine, and the codes are
        // the persisted current values exactly as the ghost probe saw them.
        // That consumer landed in P6c: ControllerWndProc's
        // kMsgOpenLanguagePalette arm takes this surface's latch slot, shows
        // the picker, and dispatches the committed pair through
        // g_apply_language_change with LanguageContext::Type (TrayTyping) or
        // ::Drag (TrayTooltip). The latch is per-surface (PR6-32), so no other
        // surface can starve this one; a same-surface second post while one is
        // pending still coalesces, and a request that arrives while a picker
        // is already open stays latched for the next delivery instead of
        // being dropped (PR6-42). main.cpp owns the picker + the existing
        // language-change callbacks (the picker writes no config itself).
        const language_palette::PickerInit& req =
            (cmd == ID_TRAY_SEARCH_SRC) ? typing_src_req
            : (cmd == ID_TRAY_SEARCH_TGT) ? typing_tgt_req
            : (cmd == ID_TRAY_SEARCH_DRAG_SRC) ? drag_src_req
            : drag_tgt_req;
        // 261116_0003 P6a fix round (PR6-03e) + r2 (PR6-08) + r3 (PR6-13): a
        // COALESCED request never calls PostMessageW inside the picker module,
        // so a bare GetLastError() here would log a stale error left by an
        // unrelated earlier call. Clear it first so the read below sees only
        // what happened since (the picker separately logs result=coalesced /
        // result=post_failed gle=... with the fresh value, so a stale GLE is
        // never duplicated here).
        // A false return does NOT by itself name the cause: the picker returns
        // false for THREE cases, (1) controller == nullptr (defensive, GLE
        // untouched, language_search_palette.cpp:507-510), (2) an open is
        // already latched (coalesced), (3) a genuinely failed post (GLE set).
        // Only (3) is decidable from the GLE, hence the cause-neutral label in
        // the else arm below.
        ::SetLastError(ERROR_SUCCESS);
        if (!PostDeferredLanguagePaletteOpen(hOwner_, req)) {
            const DWORD post_gle = ::GetLastError();
            if (post_gle != ERROR_SUCCESS) {
                DIAG_F("UI/Tray/002: deferred language-picker open post failed (GLE %lu)\n",
                       post_gle);
            } else {
                DIAG_F("UI/Tray/002: deferred language-picker open not posted "
                       "(coalesced or rejected; the picker module logs the specific reason)\n");
            }
        }
    } else if (cmd == ID_TRAY_GHOST_SRC || cmd == ID_TRAY_GHOST_TGT ||
               cmd == ID_TRAY_GHOST_DRAG_SRC || cmd == ID_TRAY_GHOST_DRAG_TGT) {
        // 261116_0003 P6: documented NO-OP arm. The ghost rows are appended
        // MF_GRAYED, so the OS never dispatches them — this arm exists so the
        // four ghost IDs are not silently unhandled if a future change ever
        // makes them selectable (designer §4.6 floats "choosing the ghost row
        // = explicit keep-as-is confirmation"; that decision is NOT made
        // here). No config write, no callback, no picker open.
    } else if (cmd >= kTrayUserLangBase && cmd < kTrayUserLangBandEnd) {
        // 261116_0003 P9b-2: ONE dispatch mapping for all four "내 언어" slots.
        // The command decodes back to (surface, user index), the index selects
        // the row from the SAME merged view the menu was built from, and the
        // payload is the entry's STORED name (UserLanguageStoredName: the
        // English name when present, else the native name) - the exact form
        // PlanLanguageSync canonicalizes to and every other surface already
        // sends. By role, not by row content: a source pick never carries a
        // target and vice versa, so main.cpp's coordinator sees the identical
        // request shape it sees from a registry row. The bounds check is
        // defensive (the ids are only appended for existing rows); a stale or
        // out-of-range id writes nothing.
        const TrayUserLangCommandRef user_cmd = DecodeTrayUserLangCommand(cmd);
        const LanguageMenuView& user_view =
            (user_cmd.surface == TrayUserLangSurface::TypingSource ||
             user_cmd.surface == TrayUserLangSurface::DragSource)
                ? srcMenu
                : tgtMenu;
        if (user_cmd.decoded && user_cmd.user_index < user_view.user_group.size() &&
            user_view.user_group[user_cmd.user_index].user != nullptr) {
            const std::string name = UserLanguageStoredName(
                *user_view.user_group[user_cmd.user_index].user);
            switch (user_cmd.surface) {
                case TrayUserLangSurface::TypingSource:
                    if (callbacks_.on_select_source_lang) {
                        callbacks_.on_select_source_lang(name);
                    }
                    break;
                case TrayUserLangSurface::TypingTarget:
                    if (callbacks_.on_select_target_lang) {
                        callbacks_.on_select_target_lang(name);
                    }
                    break;
                case TrayUserLangSurface::DragSource:
                    if (callbacks_.on_select_drag_source_lang) {
                        callbacks_.on_select_drag_source_lang(name);
                    }
                    break;
                case TrayUserLangSurface::DragTarget:
                    if (callbacks_.on_select_drag_target_lang) {
                        callbacks_.on_select_drag_target_lang(name);
                    }
                    break;
            }
        }
    } else if (cmd == ID_TRAY_UILANG_AUTO && callbacks_.on_select_ui_language) {
        callbacks_.on_select_ui_language("auto");
    } else if (cmd == ID_TRAY_UILANG_SEARCH) {
        // 261119_0001 U4 (UI-language-89 phase 2): the submenu's "검색…" row
        // mirrors the P6 search-row arm above EXACTLY — the same REQ-047 D3
        // deferred open (the post lands on the controller window immediately
        // after TrackPopupMenuEx returned; entering the picker's modal loop
        // while the menu modality tear-down is in flight is the REQ-047 D3
        // "minimized window, no input" defect), the same PR6-03e SetLastError
        // hygiene, the same cause-neutral diagnostic. The request latched at
        // menu-build time (ui_lang_req) is posted verbatim: no pointer crosses
        // PostMessageW, and its source code is already the canonical config
        // spelling.
        // The consumer is U5 (main.cpp's controller arm takes the
        // TrayUiLanguage latch slot and commits the pick through the existing
        // on_select_ui_language path). The arm is wired: its drain polls this
        // slot third (kMsgOpenLanguagePalette), so a posted request stays
        // latched only until that drain takes it.
        ::SetLastError(ERROR_SUCCESS);
        if (!PostDeferredLanguagePaletteOpen(hOwner_, ui_lang_req)) {
            const DWORD post_gle = ::GetLastError();
            if (post_gle != ERROR_SUCCESS) {
                DIAG_F("UI/Tray/002: deferred language-picker open post failed (GLE %lu)\n",
                       post_gle);
            } else {
                DIAG_F("UI/Tray/002: deferred language-picker open not posted "
                       "(coalesced or rejected; the picker module logs the specific reason)\n");
            }
        }
    } else if (cmd == ID_TRAY_DRAG_SWAP && callbacks_.on_swap_drag_languages) {
        // W2 (R1-22): reverse the 번역툴팁 pair. The callback resolves the pair
        // through the shared PlanLanguageSwap planner and applies it via
        // ApplyLanguageChange(LanguageContext::Drag, ...), so a drag swap pins
        // the target exactly like an explicit drag-target pick (F2 contract).
        callbacks_.on_swap_drag_languages();
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
