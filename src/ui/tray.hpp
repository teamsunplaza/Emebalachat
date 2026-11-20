#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>
#include <shellapi.h>

#include "config.hpp" // 261116_0003 P6: LanguageEngine for TrayLanguageEngineFor
#include "ui/theme.hpp"

namespace emebalachat {

// REQ-054/REQ-056: the engine submenu's 4-way check-mark index (0 = Google/
// auto, 1 = Local, 2 = OpenAI, 3 = 사용자 선택(.gguf)) derived from the
// persisted config engine_type string. Single definition consumed by
// main.cpp's engine-menu resolver (REQ-056 derive-at-open). Pre-REQ-054 the
// hook paths passed a legacy bool "(type != local)" as this index, mapping
// user_gguf -> Local (the "F9 reverts my engine" report: the menu tick, not
// routing).
inline int TrayPreferredEngineIndex(std::string_view engine_type) {
    return (engine_type == "local") ? 1
         : (engine_type == "openai") ? 2
         : (engine_type == "user_gguf") ? 3
         : 0;
}

// 261116_0003 P6 (language-menu redesign, plan §5 P6 + D3): the 4-way
// preferred-engine index mapped onto the registry's per-engine language views
// (config.hpp GetLanguagesForEngine). The tray resolves the ACTIVE engine at
// menu-build time through EngineMenuView::preferred_engine (REQ-056
// derive-at-open), and the four language menus plus the deferred picker
// request must all read the SAME view, so the mapping lives next to the index
// it decodes.
//
// IMPORTANT — do NOT cast the index to engine.hpp's EngineType: the two enums
// number their values differently (EngineType::Auto is 0, the tray's 0 is the
// Google row), and engine.hpp has no user_gguf value at all (that route is a
// config-string read inside engine.cpp; its view is LanguageEngine::UserGguf).
//
// Index 0 covers BOTH persisted engine_type "google" AND "auto" (the engine
// submenu's single Google row is the checked state for both, REQ-054), so the
// language menus show the GOOGLE view for both. Deliberate: the checked engine
// row and the language list must never disagree. The view choice is exact for
// PICKS: every Google-view pick is also servable under auto (the revived D7
// router serves local-capable pairs from the local engine regardless; verified
// engine.cpp:379-399).
//
// The pinned-absent ghost (261116_0003 P6a fix round, PR6-01) is deliberately
// NARROWER than that view, because routing is not the same under the two
// tokens: a persisted "google" latches active_type_ = GoogleTranslate inside
// RefreshActiveEngine (engine.cpp:313-315), and Translate() consults
// PlanTranslationRouting only under active == LocalLlama (engine.cpp:685-691),
// so that token NEVER reaches the local leg. Only a persisted "auto" may treat
// a HyMt2-only row (e.g. BO/Tibetan, no kLangGoogle bit) as locally servable,
// so EngineMenuView::is_auto carries that distinction (main.cpp's resolver
// derives it from the raw config string; the collapsed index cannot).
// TRUE: the tray composes the gate with the ROUTER'S OWN pair predicate,
// LocalPairReliable (config.cpp:1280-1307) — the same pairwise gate
// PlanTranslationRouting consults under auto — AND the router's own
// availability predicate HostCouldServeFor (engine.cpp:288-290 =
// engine_host.enabled && IsHostBinaryPresent(), carried here as
// EngineMenuView::local_host_available, r4 PR6-19). Suppressing the ghost
// additionally requires the local host leg to be live (enabled + binary
// present), the pair predicate above being the other half. Both halves are
// read FRESH at menu-build time, but they are not the same KIND of read: the
// availability half pairs a config flag with a LIVE existence probe of the
// host binary (engine_host::IsHostBinaryPresent() is a plain file-existence
// check, the engine repo's src/engine_host_client.cpp:783-786), while the
// router's local leg is the LATCHED active_type_, written only inside
// RefreshActiveEngine (engine.cpp:304) and only ever called from the
// constructor and the five setters (engine.cpp:111, 126, 140, 214, 224, 234).
// The gate therefore matches the router in every STEADY state and diverges in
// exactly one direction: a host binary that APPEARS after the last refresh
// makes the probe TRUE while the latched route is still GoogleTranslate, so
// Auto keeps sending traffic to Google (which cannot serve a HyMt2-only pin)
// and the ghost stays suppressed in that window, i.e. the PR6-19 silent
// degradation displaced onto the inverse transition. The opposite direction
// (host removed after the last refresh) is conservative: the probe reads
// FALSE and the truthful ghost SHOWS. Sourcing this conjunct from the engine's
// LATCHED local-leg state instead of a live probe needs an engine-side
// accessor; still open (P6c landed the resolver wiring, not this accessor).
// The Zero-Install case, stated explicitly: under auto with the
// host absent at the last refresh, RefreshActiveEngine (engine.cpp:357-359)
// latches Google "Google Translate (Zero-Install)" and Translate() consults
// PlanTranslationRouting ONLY when active == LocalLlama (engine.cpp:685-691),
// so the router is never consulted, Google serves everything, and a HyMt2-only
// pin (e.g. BO/Tibetan, config.cpp:133, no kLangGoogle bit) is honored by NO
// leg — Google cannot serve BO — and the truthful single-view ghost SHOWS.
// R3 PR6-10: the earlier rounds re-probed the pinned role's HyMt2-view
// membership instead, which is pair-blind — source HU (cloud-only,
// config.cpp:108) + target BO (HyMt2-only, config.cpp:133) finds BO in the
// HyMt2 view and suppresses the ghost, yet LocalPairReliable("HU","BO") is
// false, the router falls back to Google, which cannot serve BO, and the user
// got no signal. FALSE (explicit "google", auto with the host leg not live,
// or a tray with no resolver, the default): the truthful single-view
// PinnedAbsentLanguage() ghost shows, since the router would not serve the
// pin either. Concrete engines (1/2/3, pref != 0) keep the single-view
// check. R2 PR6-07: the first round gated this on `pref == 0` alone, which
// silently hid that truthful ghost row under explicit google.
//
// Honesty corner (R3 PR6-11): the token is the RAW persisted string. The
// app's own writers emit only the five canonical values, but config loads
// engine_type unvalidated, so a
// hand-edited non-canonical token routes as Auto at startup
// (main.cpp:1430-1445 maps everything unrecognized to Auto) while this
// token stays false — the conservative single-view ghost shows even though
// routing would honor a reliable pair. Mirroring the router exactly needs
// the P9 config-normalization pass; deferred there by name.
//
// The HyMt2-only rows stay reachable by switching the engine submenu to
// Local, and a persisted pick the view does not offer is never dropped and
// stays honored raw (menus filter, validation stays registry-global);
// whether it renders as the P6 pinned-absent ghost row depends on the gate
// described above — under auto with a pair LocalPairReliable honors, the
// ghost is suppressed entirely and the pick renders as no row at all
// (R3 PR6-14).
//
// 261116_0003 P6b fix round (PR6-28): the four-conjunct composition above is
// NO LONGER re-derived inline in tray.cpp — it lives in ONE shared pure
// predicate, language_palette::SuppressPinnedAbsentGhost
// (language_search_palette.hpp), which BOTH this surface and the floating
// tooltip's two dropdowns call, over per-surface INPUTS: main.cpp wires THIS
// surface's resolver (the EngineMenuView fields below, main.cpp:3160-3227) and
// the tooltip's SetEngineResolver (main.cpp:3248-3262) from the SAME sources,
// so the two surfaces evaluate the same decision on the same live router view
// and agree on the ghost row in practice (PR6-38/PR6-35); the contract stays
// shared-DECISION / unshared-INPUTS, so each surface remains responsible for
// feeding its own live values at its own menu-open time, never a cached copy.
// P6c landing the tooltip's SetEngineResolver wiring was the completing step.
// The pre-P6c interim — the tooltip's resolver not yet wired, conservative
// defaults, a truthful ghost where this surface suppressed — is gone; that
// direction was safe because it over-informed rather than wrongfully
// suppressed, and the P6a defect class was wrongful SUPPRESSION.
// This header's EngineMenuView fields remain the tray's INPUTS to that
// predicate; nothing about the resolver contract or the defaults changes.
inline LanguageEngine TrayLanguageEngineFor(int preferred_engine) {
    switch (preferred_engine) {
        case 1: return LanguageEngine::HyMt2;    // "local"
        case 2: return LanguageEngine::OpenAi;   // "openai"
        case 3: return LanguageEngine::UserGguf; // "user_gguf"
        default: return LanguageEngine::Google;  // 0 = "google" / "auto" row
    }
}

// ---- 261116_0003 P9b-2: the "내 언어" user-row command band ----------------
//
// The four language submenus (TYPE source/target, DRAG source/target) carry the
// config's user-defined languages as a "내 언어" group above the registry rows
// (plan §3 D5 + §4 Phase 3). The data is BuildLanguageMenuView's merge (P9a),
// which also removes every registry row a user entry claimed, so a language is
// rendered exactly once per menu.
//
// The registry bands cannot absorb an unbounded user list, and the P6 block
// (2601..2608 search/ghost) recorded 2609+ as the space the P9 redesign owns.
// The group therefore gets a FLAT computed band: base + surface *
// kMaxUserLanguages + user index, one 64-id slot per surface. P9a's
// kMaxUserLanguages cap is the only bound the merge can produce
// (BuildLanguageMenuView stops at that count), so four slots of exactly that
// size hold every row every surface can ever render. tray.cpp's static_asserts
// pin the arithmetic, the contiguity after the P6 ghost block and
// non-collision with every other tray band; growing kMaxUserLanguages is a
// re-pin obligation there in the same change (the registry-band literals'
// documented duty).
enum class TrayUserLangSurface : unsigned char {
    TypingSource = 0, // keyboard-typing source menu (ID_TRAY_SRC_BASE rows)
    TypingTarget = 1, // keyboard-typing target menu (ID_TRAY_TGT_BASE rows)
    DragSource = 2,   // 번역툴팁 source menu (ID_TRAY_DRAG_SRC_BASE rows)
    DragTarget = 3,   // 번역툴팁 target menu (ID_TRAY_DRAG_TGT_BASE rows)
};

inline constexpr UINT kTrayUserLangBase = 2609;
inline constexpr UINT kTrayUserLangSurfaceCount = 4;
inline constexpr UINT kTrayUserLangStride = static_cast<UINT>(kMaxUserLanguages);
inline constexpr UINT kTrayUserLangBandEnd =
    kTrayUserLangBase + kTrayUserLangSurfaceCount * kTrayUserLangStride; // exclusive

// Command id of one user row. Callers keep user_index < kMaxUserLanguages; the
// menu build applies the same cap the merge does.
inline constexpr UINT TrayUserLangCommand(TrayUserLangSurface surface, std::size_t user_index) {
    return kTrayUserLangBase + static_cast<UINT>(surface) * kTrayUserLangStride +
           static_cast<UINT>(user_index);
}

struct TrayUserLangCommandRef {
    TrayUserLangSurface surface = TrayUserLangSurface::TypingSource;
    std::size_t user_index = 0;
    bool decoded = false;
};

// Decodes a dispatch command back to (surface, user index). decoded == false
// for every id outside [kTrayUserLangBase, kTrayUserLangBandEnd), so no other
// menu command can ever be read as a user row.
inline TrayUserLangCommandRef DecodeTrayUserLangCommand(UINT cmd) {
    TrayUserLangCommandRef ref;
    if (cmd < kTrayUserLangBase || cmd >= kTrayUserLangBandEnd) {
        return ref;
    }
    const UINT offset = cmd - kTrayUserLangBase;
    ref.surface = static_cast<TrayUserLangSurface>(offset / kTrayUserLangStride);
    ref.user_index = static_cast<std::size_t>(offset % kTrayUserLangStride);
    ref.decoded = true;
    return ref;
}

// ---- 261116_0003 P9b-2: the group's row form + its ghost half -------------
// Row text of one "내 언어" row: the user's NATIVE name, plus the English name
// in parentheses when the entry carries one. Deliberately NOT the registry
// rows' "CODE - display name" form: a user entry has no code prefix, which is
// what makes the group visually distinguishable (a claimed registry language
// appears once, in this group, so the two forms never describe the same row).
// The tooltip's two dropdowns share this ONE definition; their registry rows
// read "English (Native)" while the user rows stay native-first - no code
// prefix is the shared marker, not a second convention.
std::string FormatUserLanguageMenuLabel(const UserLanguage& language);

// 261116_0003 P9b-2 fix R14-02: ONE shared resolution of "which RENDERED
// "내 언어" row does the persisted pin name", called by all six menu surfaces
// (the tray's four submenus and the tooltip's two dropdowns). nullptr when the
// pin names no entry this view draws.
//
// Two match arms, over merged.user_group, in this order:
//   1. the FindUserLanguage spelling set (stored name, native name, English
//      name, resolved code; ASCII-case-insensitive, trimmed), so every pin the
//      P9a authority already resolves keeps the row it names. It runs across
//      the WHOLE group before arm 2, so an entry whose own spelling the pin
//      carries always wins over another entry's code that resolves from the
//      same text.
//   2. the pin's RESOLVED registry code (NormalizeLanguageCode), for a
//      code-carrying entry. This arm is what makes the pin round-trip the
//      planner: CanonicalPlannerToken is REGISTRY-FIRST (config.cpp), so a
//      pick of a native-only entry (native "한국어", code KO, no English name)
//      persists the REGISTRY spelling "Korean". That spelling appears on the
//      entry nowhere, and the merge has moved the claimed KO registry row out
//      of the registry section, so a spelling-only lookup lights ZERO rows;
//      the code arm resolves "Korean" back to KO and lights the entry's row.
// The search runs over the RENDERED group, never the whole registry: the mark
// must land on a row the menu draws (the merge filters a code-less entry out
// of the Google view, so it can never be the checked row). The result is
// compared by pointer identity against the rows the caller is about to append,
// which is why the caller must pass the SAME view it renders.
const UserLanguage* CurrentUserLanguageRow(const LanguageMenuView& merged,
                                           std::string_view pinned_raw);

// 261116_0003 P9b-2: the registry rows' check-mark rule, shared by both menu
// surfaces. `current_normalized` is NormalizeLanguageCode(current_raw), and that
// normalization maps EVERY unresolvable token to "AUTO" (config.cpp). Since
// P9b-2 a code-less user pin is exactly such a token ("Twi" -> "AUTO"), so the
// normalized comparison alone would light the AUTO row for a pin that is not
// AUTO at all (and the same pin also lights its own "내 언어" row - two check
// marks for one setting). The RAW token therefore decides the AUTO row; every
// other row keeps the plain normalized comparison, because registry codes are
// the canonical form. A real AUTO pin stores "Auto Detect" (or an empty source
// display fallback), so the accepted spellings are the two the app itself
// writes.
//
// 261116_0003 P9b-2 fix R14-02 (exactly one check mark per setting):
// `user_row_checked` carries the fact that a RENDERED "내 언어" row is already
// checked for this same pin (CurrentUserLanguageRow answered non-null), and the
// AUTO arm then withdraws its mark because the user row owns the setting. The
// colliding pin is reachable: a user entry stored as "Auto Detect" (or "AUTO")
// spell-matches the raw token the AUTO arm accepts as genuine, so without this
// fact one pin lights both the entry's row and the AUTO registry row. The
// default keeps every 3-argument caller and its pins behaviorally identical;
// the flag can only ever REMOVE an AUTO mark, never add one, and concrete rows
// ignore it.
inline bool IsRegistryMenuRowCurrent(std::string_view row_code,
                                     std::string_view current_normalized,
                                     std::string_view current_raw,
                                     bool user_row_checked = false) {
    if (row_code != current_normalized) {
        return false;
    }
    if (row_code != "AUTO") {
        return true;
    }
    if (user_row_checked) {
        return false;
    }
    return current_raw.empty() || current_raw == "AUTO" || current_raw == "Auto Detect";
}

// The USER half of the pinned-absent ghost (the "설정 유지됨" contract). The
// registry half stays with language_palette::PickerState::PinnedAbsentLanguage
// (the shared predicate, never re-implemented here) and already covers every
// user entry whose code resolved: the pin is the stored name,
// NormalizeLanguageCode maps it back to the registry code, and the predicate
// answers from the engine view. The CALLER runs this half only when the
// registry half found NO ghost BEFORE the shared
// language_palette::SuppressPinnedAbsentGhost gate decides (fix R14-01): a
// code-carrying pin whose ghost the gate suppresses (AUTO + live host + a
// locally reliable pair, e.g. a Hy-MT2-only code such as BO) stays silent,
// exactly like an identical registry pin; the helper itself never consults the
// gate, so SuppressPinnedAbsentGhost stays the ONE suppression predicate.
// Within that gate, the helper answers the remaining question from the
// AUTHORITATIVE MERGE instead of re-deriving capability: the pinned name
// resolves to a registered entry (FindUserLanguage, the P9a authority) AND the
// merged view offers it neither as a user row nor through its resolved
// registry code. "Offered as a user row" matches by identity OR by resolved
// code (fix R14-04): the merge's code-level dedupe keeps only the FIRST entry
// of a duplicate code, so the pinned entry can be the dropped twin while the
// language is still offered as the surviving entry's row. No engine mask is
// read here; BuildLanguageMenuView stays the only filtering authority.
// `merged` and `user_languages` must be the SAME vector the view was built from
// (the user-row pointers are compared by identity).
const UserLanguage* PinnedAbsentUserLanguage(const LanguageMenuView& merged,
                                             const std::vector<UserLanguage>& user_languages,
                                             std::string_view pinned_name);

// ---- 261116_0003 P9b-3 U4a: the ONE shared probe badge on "내 언어" rows ----
// Row-label SUFFIX of one "내 언어" row under the caller's (engine, model)
// probe-cache key (D6: one cached verdict per engine + model + language), the
// SINGLE badge composition every menu surface appends after
// FormatUserLanguageMenuLabel's &-escaped label (R14-05 stays the one escape
// point; the badge copy carries no '&'). The suffix includes the separating
// " - " (the manager dialog's kListBadgeSeparator) and is EMPTY when there is
// nothing to show, so the caller composes label + suffix verbatim.
//   probe_engine_id  UserLanguageEngineId(resolved engine); EMPTY = the
//                    no-probe context (Google is publisher-declared, the local
//                    engine and Auto have no probe row) -> NO suffix at all.
//   probe_model      the OpenAiConfig model / the user-.gguf registry id — the
//                    SAME model half the U3 runner writes its slots under, so
//                    a row and its verdict can never disagree about the key.
//   in_flight        the U3 IsInFlight answer for THIS entry's stored name
//                    (runtime-only Checking); false when no probe runs.
// The five display states mirror the manager dialog's row fold exactly
// (user_language_manager_dialog::FoldUserLanguageManagerRow), composed from
// that module's OWN public landmarks (kBadgeSupportedPrefix /
// kBadgeUnsupportedPrefix / kBadgeMarkerSeparator / kListBadgeSeparator) so
// the menu rows and the manager list can never drift apart:
//   in flight       -> " - 확인 중"            (runtime-only; never the slot)
//   Supported       -> " - ✓ 지원"
//   Unsupported     -> " - ✗ 미지원 의심"
//   UserOverridden  -> " - ✓/✗ <direction> · 직접 설정함"  (the marker)
//   Checking slot   -> " - 확인 중"            (defensive, as the manager fold)
//   Unknown / no cache record -> ""            (nothing judged yet, D6)
// Display-only: adds no dispatch, creates no second suppression path (the
// R14-01 ghost contract and the R14-02 check-mark rule stay untouched).
std::wstring UserLanguageMenuBadgeSuffix(const UserLanguage& language,
                                         std::string_view probe_engine_id,
                                         std::string_view probe_model,
                                         bool in_flight);

// ---- W1 (R1-22, session 261029_0001): hover-tip / status-row composition ----
//
// NOTIFYICONDATAW::szTip is a FIXED wchar_t buffer INCLUDING the terminator.
// Shell_NotifyIcon silently drops an over-long tip and wcsncpy_s(_TRUNCATE)
// cuts mid-word, so the composed tip is capped here - derived from the struct
// itself so the number can never drift from the SDK.
inline constexpr size_t kTrayTipMaxChars =
    sizeof(NOTIFYICONDATAW{}.szTip) / sizeof(wchar_t) - 1;

// The localized engine rows (MenuEngineGoogle / MenuEngineLocal / ...) are
// full sentences of 26-44 characters and would eat the whole tip budget
// (research §8 residual risk 1: "prefer engine short code over full localized
// engine sentence"). This maps BOTH push shapes onto one short product/model
// token: the GUI path passes TranslationManager::GetActiveEngineName()'s
// display stem ("Hy-MT2-1.8B (Shared Host)"), the hook path passes the raw
// persisted engine_type ("local"). Empty input stays empty (no engine segment).
std::string TrayEngineLabel(std::string_view active_engine);

// The pieces of the hover tip, already localized by the caller. Keeping the
// composition pure (no I18n call inside) is what makes the szTip budget and
// the drop order unit-testable headlessly.
struct TrayTipParts {
    std::wstring_view title;
    std::wstring_view state;          // BadgeActive / BadgePaused
    std::wstring_view engine;         // TrayEngineLabel result (may be empty)
    std::wstring_view src_display;    // I18n::GetLanguageDisplayName(...)
    std::wstring_view tgt_display;
    std::wstring_view auto_send_label; // MenuAutoSend
    bool auto_send = false;
};

// "Title (State) - Engine · SRC → TGT[ · AutoSend]", never longer than
// kTrayTipMaxChars. Overflow drops the auto-send tail FIRST (it is the only
// optional segment), then ellipsizes - a visible "…" instead of the silent
// mid-word cut wcsncpy_s would produce.
std::wstring ComposeTrayTip(const TrayTipParts& parts);

// "State · SRC → TGT": appended to the tray's top status row (the localized
// MenuStatus* sentence already carries the "상태:" prefix, so the row reads
// "상태: 활성 (F9: 일시 정지) · 자동 감지 → 영어" - the research's W1 shape).
std::wstring ComposeTrayStatusLabel(std::wstring_view state_label,
                                    std::wstring_view src_display,
                                    std::wstring_view tgt_display);

// ---- W2 (R1-22): pure swap planner for BOTH language pairs ----
//
// The tray Swap row / badge double-click (TYPE pair) and the new 번역툴팁 Swap
// row (DRAG pair) share ONE policy definition, so a product change to the
// AUTO-pivot rule can never land on only one of the two surfaces:
//   * source is AUTO Detect -> the concrete old target becomes the source and
//     the new target falls back to the OS-native language (English when the
//     OS language IS the old target, so the swap always reverses something);
//   * otherwise the two concrete languages swap directly.
// Result strings are name_en (the coordinator's stored form). Pure: the caller
// passes the pair and the OS language code, so both arms and every OS-language
// edge (including the unsupported -> "KO" fallback) are unit-testable.
struct LanguageSwapPlan {
    std::string source;
    std::string target;
};
LanguageSwapPlan PlanLanguageSwap(std::string_view cur_source,
                                  std::string_view cur_target,
                                  std::string_view system_language_code);

class SystemTray {
public:
    using ThemeSelectCallback = std::function<void(const std::string&)>;

    static constexpr UINT kCmdThemeFluent    = 2381;
    static constexpr UINT kCmdThemeEditorial = 2382;
    static constexpr UINT kCmdThemeMonolith  = 2383;

    struct Callbacks {
        std::function<void()> on_toggle_active;
        // REQ-045 P4-3 + REQ-046 P4-2 (Rev2 §B-3): engine_idx 0 = Google
        // Translate, 1 = Local LLM, 2 = OpenAI Compatible, 3 = 사용자 선택
        // (.gguf). Appended (not renumbered) so existing main.cpp call sites
        // keep their meaning.
        std::function<void(int engine_idx)> on_select_engine;
        std::function<void(std::string_view code)> on_select_source_lang;
        std::function<void(std::string_view code)> on_select_target_lang;
        // REQ-025 (Phase A §2.1.A3-25): "번역툴팁"(drag) language pair selectors,
        // separate from the "키보드타이핑"(type) pair above. Same string_view
        // contract (name_en passed); main.cpp routes to ApplyLanguageChange
        // (LanguageContext::Drag).
        std::function<void(std::string_view code)> on_select_drag_source_lang;
        std::function<void(std::string_view code)> on_select_drag_target_lang;
        std::function<void()> on_swap_languages;
        // W2 (R1-22, session 261029_0001): the 번역툴팁 group's own Swap row.
        // Same policy as on_swap_languages (PlanLanguageSwap) but applied to the
        // DRAG pair through ApplyLanguageChange(LanguageContext::Drag, ...).
        // Appended (not reordered) so every existing wiring keeps its meaning.
        std::function<void()> on_swap_drag_languages;
        std::function<void()> on_toggle_auto_send;
        std::function<void()> on_toggle_sound;
        std::function<void()> on_toggle_badge;
        std::function<void()> on_toggle_start_with_windows;
        std::function<void()> on_show_cheat_sheet;
        std::function<void()> on_show_about; // Opens the About window (wired in main.cpp)
        std::function<void()> on_exit;
        // R6 Phase 6 (plan §5.4), REQ-037/B-4 expansion (design §2-Q3),
        // 261119_0001 U4 (UI-language-89 phase 2): the UI-language submenu is
        // Auto + "검색…" now, so this callback fires from the Auto row and from
        // the shared picker's commit (TrayUiLanguage surface) - never from a
        // flat endonym band, which is retired. Argument is the canonical
        // persisted code - "auto" or the config_code of any entry in
        // I18n::GetSupportedUiLocales() as emitted by I18n::LocaleToString
        // ("ko", "ja", "zh-CN", "zh-TW", "vi", "es", ... ; the canonical
        // spellings live in i18n.cpp kLocaleMappings, 89 rows once phase 3
        // authors them all). main.cpp validates/persists via
        // PlanUiLocaleChange.
        std::function<void(std::string_view code)> on_select_ui_language;
        // REQ-050: REMOVED — the engine submenu's file-picker row is merged
        // into the single manager row (ID_TRAY_MANAGE_GGUF). The manager's
        // [파일에서 추가…] button consumes main.cpp's registration pipeline
        // through the callback passed to ShowGgufModelManagerDialog instead.
        // REQ-060 (session 261009_0001, engine-idle-eviction-warmup): the
        // engine submenu's "번역 모델 로드 설정…" entry row (below the model
        // manager row). Entry row, NOT a check target — it opens the
        // translation-model load-mode dialog. main.cpp defers the modal open
        // to the controller window past the TrackPopupMenuEx teardown (the
        // REQ-047 D3 contract shared with the manager/OpenAI rows).
        std::function<void()> on_open_engine_load_settings;
        ThemeSelectCallback on_select_theme;
        // 261108_0002 (WS3, v0.10.6): the "역번역 플로팅 표시 (번역 검증)" check
        // row (ID_TRAY_BACK_TRANSLATE). Appended (not reordered) so every
        // existing main.cpp wiring keeps its meaning. main.cpp flips
        // AppConfig::back_translation_enabled, persists it, and refreshes the
        // check mark through SetBackTranslationEnabled.
        std::function<void()> on_toggle_back_translate;
    };

    SystemTray();
    ~SystemTray();

    // Initializes system tray notification icon
    bool Create(HWND hOwner, HINSTANCE hInstance, const Callbacks& callbacks);

    // Removes icon from system notification area
    void Destroy();

    // Updates tray icon tooltip, status color, and checked state.
    // REQ-025: src/tgt are the TYPE pair (tip text + type submenu checks);
    // drag_src/drag_tgt drive ONLY the drag submenu check marks - the tip
    // always keeps displaying the type pair (Phase A §2.1.A3-25 design).
    // active_engine is display-only: W1 (R1-22) puts its SHORT token in the
    // tip (TrayEngineLabel) and still logs it through tray_update; the Engine
    // submenu check mark never reads it (REQ-056).
    //
    // R2-5 (FIX-B2): an EMPTY active_engine means "no resolved display stem"
    // and PRESERVES the stored one. The GUI thread's refresh_tray() is the
    // single authority (engine.GetActiveEngineName()); the hook-thread
    // refreshes pass empty so an F9 / Ctrl+Shift+Enter / Ctrl+F9 can no
    // longer clobber "Hy-MT2-1.8B" with the raw engine_type token.
    //
    // W1 (R1-22): the tip is now composed by ComposeTrayTip - state, engine,
    // type pair and (when on) the auto-send tail, capped at kTrayTipMaxChars.
    //
    // REQ-056 (derive-at-open): the Engine submenu's two config-derived
    // values — the 4-way preferred-engine check index and the registered
    // user-model stem — are NO LONGER pushed through UpdateStatus and cached.
    // Every tray bug of the 260922 session (F9 stem clobber REQ-054, check-
    // mark formula REQ-054) came from those push paths. ShowContextMenu now
    // asks main.cpp's resolver (SetEngineMenuResolver) at menu build time;
    // UpdateStatus carries only runtime state.
    void UpdateStatus(
        bool active,
        std::string_view active_engine,
        std::string_view src_code,
        std::string_view tgt_code,
        std::string_view drag_src_code,
        std::string_view drag_tgt_code,
        bool auto_send,
        bool sound_enabled,
        bool badge_visible
    );

    // W1 (R1-22) test seam: the tip text UpdateStatus last composed and copied
    // into szTip. run_tests exercises UpdateStatus on a non-Created tray (the
    // Shell_NotifyIcon call is a harmless no-op), so this lets the suite prove
    // the REAL surface honours the szTip budget, not just the pure helper.
    std::wstring_view TipTextForTest() const noexcept { return nid_.szTip; }

    // REQ-056: derive-at-open view of the engine submenu's config-derived
    // values. Invoked ON THE GUI THREAD from ShowContextMenu at menu build
    // time — main.cpp is the single authority (registry + config resolution);
    // the tray no longer caches them, so no push path can go stale.
    struct EngineMenuView {
        int preferred_engine = 0;      // TrayPreferredEngineIndex(engine_type)
        // 261116_0003 P6a fix round r2 (PR6-07): the collapsed index maps BOTH
        // "google" and "auto" to 0, so it cannot answer "may the router serve a
        // HyMt2-only pin?". main.cpp's resolver sets this from the RAW persisted
        // engine_type; the false default keeps an unwired tray truthful (google
        // view, single-view ghost).
        bool is_auto = false;
        // 261116_0003 P6a fix round r4 (PR6-19): the ghost gate's host-
        // availability conjunct. HostCouldServeFor (engine.cpp:288-290) =
        // engine_host.enabled && engine_host::IsHostBinaryPresent() is the
        // router's OWN availability half of its local-serve predicate:
        // under auto with the host leg dead (Zero-Install) RefreshActiveEngine
        // picks Google (engine.cpp:357-359) and Translate() never consults
        // PlanTranslationRouting, so a HyMt2-only pin is honored by NO leg and
        // the truthful ghost must SHOW. Its binary half is a LIVE probe, so it
        // can read TRUE while the latched active_type_ is still Google; that
        // single divergence direction is scoped in the rationale above.
        // main.cpp's resolver populates this from those same two components;
        // the FALSE default keeps an unwired tray (or any future consumer that
        // forgets the field) in the conservative direction — the ghost shows.
        bool local_host_available = false;
        std::string user_model_stem;   // "" = no user model registered
        // 261116_0003 P9b-2: the config's registered user languages, COPIED by
        // value at menu-build time (the same derive-at-open contract as every
        // other field here; config.hpp documents the member as GUI-thread-only,
        // and this resolver runs on the GUI thread). The menu build hands this
        // vector to BuildLanguageMenuView and keeps it alive through the menu's
        // dispatch, so the "내 언어" rows' `const UserLanguage*` pointers stay
        // valid for the whole TrackPopupMenuEx call. Empty on a tray whose
        // resolver does not populate it: no group, no user ghost, and the
        // registry menus are exactly the pre-P9b-2 shape.
        std::vector<UserLanguage> user_languages;
        // 261116_0003 P9b-3 U4a: the badge key + the runtime in-flight seam,
        // resolved at menu-open time from the SAME persisted snapshot (the
        // identical derive-at-open contract as every field above). The engine
        // half is UserLanguageEngineId(resolved engine) — "" names the
        // no-probe context and renders NO badge; the model half is the
        // OpenAiConfig model / the user-.gguf registry id (the runner writes
        // its verdict slots under the same key, D6). BOTH stay empty on a
        // tray whose resolver does not populate them, and the badge fold then
        // renders no suffix (the conservative default). The in-flight seam
        // answers per entry from U3's GUI-thread-only in-flight set — the
        // stored-name keying lives inside the seam (main.cpp), so this
        // surface never re-derives it; EMPTY = no resolver -> no row is in
        // flight.
        std::string user_lang_probe_engine;
        std::string user_lang_probe_model;
        std::function<bool(const UserLanguage&)> user_lang_probe_in_flight;
    };
    void SetEngineMenuResolver(std::function<EngineMenuView()> resolver);

    // R6 Phase 6: mirrors the persisted config.ui_language value ("auto" or a
    // locale code). 261119_0001 U4: it now drives the submenu's Auto check mark
    // and the submenu title's endonym suffix (the current-locale signal that
    // replaced the retired flat band's per-row check marks). Called by main.cpp
    // at startup and from RefreshAllUiForLocaleChange (menu itself rebuilds
    // lazily on every ShowContextMenu, so no explicit rebuild nudge is needed).
    void SetUiLanguage(std::string_view persisted_code);
    void SetOnThemeSelectCallback(ThemeSelectCallback cb);
    void SetUiTheme(std::string_view theme_key);
    void RefreshTheme();
    // 261108_0002 (WS3, v0.10.6): mirrors AppConfig::back_translation_enabled
    // for the "역번역 플로팅 표시" check mark. The context menu rebuilds on
    // every ShowContextMenu, so this only has to store the state (same
    // discipline as SetUiLanguage); main.cpp calls it at startup and from the
    // toggle callback's refresh.
    void SetBackTranslationEnabled(bool enabled);

    // Displays popup context menu at current cursor coordinates
    void ShowContextMenu();

    static constexpr UINT WM_TRAYICON = WM_USER + 100;

private:
    HICON CreateStatusIcon(bool active);

    HWND hOwner_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    NOTIFYICONDATAW nid_ = {};
    HICON hCurrentIcon_ = nullptr;
    Callbacks callbacks_;
    ThemeSelectCallback on_theme_select_;

    bool is_active_ = true;
    // REQ-029-B: display-only engine name (tooltip + tray_update log). The
    // Engine submenu check mark NO LONGER reads this string.
    std::string active_engine_ = "Google Translate";
    // REQ-056: derive-at-open — the engine submenu's config-derived values
    // (check index + user-model stem) are resolved from main.cpp at menu
    // build time; nothing is cached here.
    std::function<EngineMenuView()> resolver_;
    std::string src_code_ = "AUTO";
    std::string tgt_code_ = "EN";
    // REQ-025: drag-pair check-mark state for the "번역툴팁" submenus.
    std::string drag_src_code_ = "AUTO";
    std::string drag_tgt_code_ = "EN";
    bool auto_send_ = false;
    bool sound_enabled_ = true;
    bool badge_visible_ = true;
    bool back_translation_enabled_ = false; // 261108_0002 (WS3): check state
    std::string ui_language_ = "auto"; // R6 Phase 6: Auto check state + title suffix (U4)
    std::string ui_theme_ = "fluent";
};

} // namespace emebalachat
