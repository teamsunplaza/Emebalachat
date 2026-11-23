// ---------------------------------------------------------------------------
// V10F (261122_0002 P3-C1, Phase 3 wave A): UX-1 Option B — the slim tray.
//
// The 18+-row, three-deep tray menu collapses to ten top rows:
//   F0  status + pair summary, F1 키보드타이핑 pair ▾ (출발/도착 ▾, ⇄,
//       최근 사용 언어 block, 검색…), F2 번역툴팁 pair ▾ (same shape),
//   F3  engine ▾ (the 4-way radio is the INSTANT switch now; the OpenAI
//       settings dialog moved to its own plain row), F4/F5 the two 1-level
//       quick toggles (자동 전송 / 역번역), F6 환경설정 ▾ (the settings hub
//       row + every legacy settings command: UI 언어 ▾, 테마 ▾, 알림음,
//       배지, 자동실행, 내 언어 추가…/내 언어…), F7 사용법, F8 소개, F9 종료.
// Every pre-V10F TrayMenuId survives verbatim; only the DEPTH of the legacy
// settings rows changed. The two new rows take the free 2017..2018 window
// (ID_TRAY_OPENAI_SETTINGS / ID_TRAY_SETTINGS_HUB) and the recent-language
// command band fills 2870..2885 (one kTrayRecentLanguageCapacity slot per
// tray pair), above the P9b-3 rows (2865/2866) and below the tooltip user
// band (2900) — all pinned by tray.cpp's own static_assert block.
//
// Coverage:
//   (a) TestP3C1RecentLanguageMru      - the pure MRU recorder
//                                        (RememberRecentLanguage): spelling
//                                        normalization through
//                                        NormalizeLanguageCode, AUTO/unknown
//                                        drop, move-to-front dedupe, cap.
//   (b) TestP3C1RecentLanguageRows     - the pure row builder
//                                        (BuildRecentLanguageRows): only rows
//                                        the live target view offers, dedupe,
//                                        capacity, code/payload form.
//   (c) TestP3C1RecentCommandBand      - the 2870..2885 command band:
//                                        constexpr values, encode/decode
//                                        round trip, out-of-band rejection,
//                                        disjointness from the neighbouring
//                                        bands (user rows 2609..2864, the
//                                        P9b-3 rows 2865/2866, tooltip 2900).
//   (d) TestP3C1TraySlimSourcePins     - structural pins on tray.cpp/.hpp:
//                                        the additive ids + band asserts, the
//                                        ten-row top level in the F0..F9
//                                        order, the engine radio/settings
//                                        split, the settings container holding
//                                        every legacy settings command, both
//                                        recent-language blocks, the MRU
//                                        dispatch recording, and zero feature
//                                        loss (every pre-V10F id literal).
//   (e) TestP3C1TrayRecentSeam         - the public seams on a non-Created
//                                        tray (the TipTextForTest precedent):
//                                        NoteRecentLanguage feeds
//                                        RecentLanguagesForTest through the
//                                        same rules, SetOpenSettingsHubCallback
//                                        stores harmlessly.
//
// Staged as an .inc next to this runner (p3_c2 precedent); registered from
// main() per the end-of-file pattern. Source pins read the production files
// through ResolveRepoFile with a CRLF-normalized raw read; every needle is
// ASCII-only.
// ---------------------------------------------------------------------------

#include "../src/ui/tray.hpp"

namespace {

// Resolve + read a production source for the P3-C1 structural pins (same
// candidate-path precedent as the P3-C2 reader; CRLF-normalized so
// line-anchored needles never depend on the checkout's autocrlf).
void P3C1ReadSrc(const wchar_t* rel, std::string& out) {
    out.clear();
    const std::filesystem::path p = ResolveRepoFile(rel);
    if (p.empty()) return;
    std::ifstream in(p, std::ios::binary);
    if (!in) return;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string norm;
    norm.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\r' && i + 1 < out.size() && out[i + 1] == '\n') continue;
        norm.push_back(out[i]);
    }
    out.swap(norm);
}

// ASCII-only squeeze for count pins that must survive reflow (same precedent
// as the P6b fix-round reader): only whitespace is normalized away.
std::string P3C1Squeeze(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            out.push_back(ch);
        }
    }
    return out;
}

} // namespace

// ---- (a) the pure recent-language MRU recorder ------------------------------

void TestP3C1RecentLanguageMru() {
    std::cout << "[RUN] Testing P3-C1 recent-language MRU recorder (pure)..." << std::endl;
    const int failures_before = g_failed_count;

    using emebalachat::RememberRecentLanguage;
    using emebalachat::kTrayRecentLanguageCapacity;

    // The registry's own spelling authority collapses every accepted spelling
    // of a language onto its code, so the MRU stores codes - never names.
    std::vector<std::string> list;
    RememberRecentLanguage(list, "Korean");
    TEST_CHECK(list.size() == 1 && list[0] == "KO",
               "P3-C1: an English-name pick records the registry code");

    // A code spelling of the same language must not add a second entry.
    RememberRecentLanguage(list, "KO");
    TEST_CHECK(list.size() == 1,
               "P3-C1: 'KO' and 'Korean' are one MRU entry (NormalizeLanguageCode dedupe)");

    RememberRecentLanguage(list, "English");
    TEST_CHECK(list.size() == 2 && list[0] == "EN" && list[1] == "KO",
               "P3-C1: a new language lands at the front, the rest shift down");

    // Move-to-front: re-using a language promotes it instead of duplicating.
    RememberRecentLanguage(list, "ko");
    TEST_CHECK(list.size() == 2 && list[0] == "KO" && list[1] == "EN",
               "P3-C1: a repeat pick moves to the front without a duplicate");

    // AUTO is a source-only setting and never a target row: every token the
    // registry cannot resolve (AUTO spellings, empty, unknown names) maps to
    // AUTO through NormalizeLanguageCode and is dropped, never recorded.
    const std::size_t before = list.size();
    RememberRecentLanguage(list, "Auto Detect");
    RememberRecentLanguage(list, "AUTO");
    RememberRecentLanguage(list, "");
    RememberRecentLanguage(list, "not-a-language-xyz");
    TEST_CHECK(list.size() == before,
               "P3-C1: AUTO / empty / unresolvable tokens are dropped, never recorded");

    // The cap is a property, not an example: 12 distinct real languages leave
    // exactly kTrayRecentLanguageCapacity rows, newest first.
    std::vector<std::string> fresh;
    const char* kCodes[] = {"EN", "KO", "JA", "ZH-CN", "FR", "DE",
                            "ES", "PT", "RU", "IT", "VI", "TH"};
    for (const char* code : kCodes) {
        RememberRecentLanguage(fresh, code);
    }
    TEST_CHECK(fresh.size() == kTrayRecentLanguageCapacity,
               "P3-C1: the MRU caps at kTrayRecentLanguageCapacity");
    TEST_CHECK(fresh[0] == "TH" && fresh[1] == "VI" && fresh[kTrayRecentLanguageCapacity - 1] == "FR",
               "P3-C1: after the cap, the order is newest-first and the tail is trimmed");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C1 recent-language MRU recorder passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C1 recent-language MRU recorder: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (b) the pure recent-language row builder --------------------------------

void TestP3C1RecentLanguageRows() {
    std::cout << "[RUN] Testing P3-C1 recent-language row builder (pure)..." << std::endl;
    const int failures_before = g_failed_count;

    using emebalachat::BuildRecentLanguageRows;
    using emebalachat::TrayRecentLanguageRow;

    const emebalachat::LanguageInfo* ko = emebalachat::FindLanguageByCode("KO");
    const emebalachat::LanguageInfo* en = emebalachat::FindLanguageByCode("EN");
    const emebalachat::LanguageInfo* ja = emebalachat::FindLanguageByCode("JA");
    TEST_CHECK(ko != nullptr && en != nullptr && ja != nullptr,
               "P3-C1: the three fixture languages resolve from the registry");

    // The live target view the 도착 언어 submenu rendered: a recent row may
    // only exist where the active engine offers the language as a target.
    const std::vector<const emebalachat::LanguageInfo*> view{ko, en, ja};

    TEST_CHECK(BuildRecentLanguageRows({}, view).empty(),
               "P3-C1: an empty MRU renders no rows (and no orphan header)");

    // Order follows the MRU (newest first); the payload is the registry
    // name_en - the exact payload every language command dispatches.
    const std::vector<TrayRecentLanguageRow> rows =
        BuildRecentLanguageRows({"JA", "KO"}, view);
    TEST_CHECK(rows.size() == 2 &&
                   rows[0].code == "JA" && rows[0].payload == "Japanese" &&
                   rows[1].code == "KO" && rows[1].payload == "Korean",
               "P3-C1: rows carry the registry code + name_en in MRU order");

    // A language the live view does not offer is skipped - never a dead row -
    // without disturbing the rows that survive.
    const std::vector<TrayRecentLanguageRow> filtered =
        BuildRecentLanguageRows({"FR", "KO"}, view);
    TEST_CHECK(filtered.size() == 1 && filtered[0].code == "KO",
               "P3-C1: a code outside the live target view is filtered out");

    // Duplicated codes collapse to their first (newest) occurrence.
    const std::vector<TrayRecentLanguageRow> deduped =
        BuildRecentLanguageRows({"KO", "EN", "ko"}, view);
    TEST_CHECK(deduped.size() == 2 && deduped[0].code == "KO" && deduped[1].code == "EN",
               "P3-C1: duplicate codes render one row, at the newest position");

    // AUTO entries (a source-only pick that slipped into the list) are rows
    // nobody can dispatch: skipped.
    const std::vector<TrayRecentLanguageRow> no_auto =
        BuildRecentLanguageRows({"AUTO", "EN"}, view);
    TEST_CHECK(no_auto.size() == 1 && no_auto[0].code == "EN",
               "P3-C1: AUTO never becomes a recent row");

    // The capacity argument bounds the block even when the MRU is deeper.
    const std::vector<TrayRecentLanguageRow> capped =
        BuildRecentLanguageRows({"JA", "EN", "KO"}, view, 2);
    TEST_CHECK(capped.size() == 2 && capped[0].code == "JA" && capped[1].code == "EN",
               "P3-C1: the capacity argument bounds the rendered block");
    TEST_CHECK(BuildRecentLanguageRows({"JA"}, view, 0).empty(),
               "P3-C1: a zero capacity renders no rows");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C1 recent-language row builder passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C1 recent-language row builder: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (c) the recent-row command band -----------------------------------------

void TestP3C1RecentCommandBand() {
    std::cout << "[RUN] Testing P3-C1 recent-row command band..." << std::endl;
    const int failures_before = g_failed_count;

    using emebalachat::DecodeTrayRecentLangCommand;
    using emebalachat::TrayRecentLangCommand;
    using emebalachat::TrayRecentPair;

    // The band facts are pinned at compile time in tray.cpp too; these mirror
    // them at runtime so a drifted constant fails in BOTH places.
    static_assert(emebalachat::kTrayRecentLanguageCapacity == 8,
                  "P3-C1: the MRU cap is 8 rows per the UX-1 brief (5..8 recent languages)");
    static_assert(emebalachat::kTrayRecentLangBase == 2870,
                  "P3-C1: the recent band starts directly above the P9b-3 rows");
    static_assert(emebalachat::kTrayRecentLangBandEnd == 2886,
                  "P3-C1: 2870 + 2 pairs * 8 rows = 2886 (exclusive)");
    static_assert(TrayRecentLangCommand(TrayRecentPair::Typing, 0) == 2870 &&
                      TrayRecentLangCommand(TrayRecentPair::Typing, 7) == 2877 &&
                      TrayRecentLangCommand(TrayRecentPair::Drag, 0) == 2878 &&
                      TrayRecentLangCommand(TrayRecentPair::Drag, 7) == 2885,
                  "P3-C1: one contiguous 8-id slot per tray pair");
    TEST_CHECK(true, "P3-C1: the command-band constants are constexpr");

    // Encode/decode round trip over every slot of both pairs.
    bool round_trip_ok = true;
    for (int pair = 0; pair < 2 && round_trip_ok; ++pair) {
        for (std::size_t i = 0; i < emebalachat::kTrayRecentLanguageCapacity; ++i) {
            const UINT cmd = TrayRecentLangCommand(static_cast<TrayRecentPair>(pair), i);
            const emebalachat::TrayRecentLangCommandRef ref = DecodeTrayRecentLangCommand(cmd);
            if (!ref.decoded || static_cast<int>(ref.pair) != pair || ref.index != i) {
                round_trip_ok = false;
                break;
            }
        }
    }
    TEST_CHECK(round_trip_ok,
               "P3-C1: every band id decodes back to its (pair, index)");

    // Out-of-band ids can never be misread as a recent row: one below, the
    // exclusive end, and the neighbouring bands on both sides.
    const emebalachat::TrayRecentLangCommandRef low = DecodeTrayRecentLangCommand(2869);
    const emebalachat::TrayRecentLangCommandRef high = DecodeTrayRecentLangCommand(2886);
    const emebalachat::TrayRecentLangCommandRef user_row = DecodeTrayRecentLangCommand(2609);
    const emebalachat::TrayRecentLangCommandRef dialog_row = DecodeTrayRecentLangCommand(2866);
    const emebalachat::TrayRecentLangCommandRef tooltip_row = DecodeTrayRecentLangCommand(2900);
    TEST_CHECK(!low.decoded && !high.decoded,
               "P3-C1: the band is decodable exactly on [2870, 2886)");
    TEST_CHECK(!user_row.decoded && !dialog_row.decoded && !tooltip_row.decoded,
               "P3-C1: the user band (2609..2864), the P9b-3 rows (2865/2866) and the "
               "tooltip band (2900) never decode as recent rows");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C1 recent-row command band passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C1 recent-row command band: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (d) structural pins on the slim menu ------------------------------------

void TestP3C1TraySlimSourcePins() {
    std::cout << "[RUN] Testing P3-C1 slim-tray structural pins..." << std::endl;
    const int failures_before = g_failed_count;

    std::string tray_cpp;
    std::string tray_hpp;
    P3C1ReadSrc(L"src\\ui\\tray.cpp", tray_cpp);
    P3C1ReadSrc(L"src\\ui\\tray.hpp", tray_hpp);
    if (tray_cpp.empty() || tray_hpp.empty()) {
        std::cout << "[SKIP] tray.cpp/.hpp not resolvable; P3-C1 pins skipped." << std::endl;
        return;
    }
    const std::string tray_sq = P3C1Squeeze(tray_cpp);
    const auto count_of = [](const std::string& hay, const char* needle) {
        size_t n = 0;
        for (size_t at = hay.find(needle); at != std::string::npos;
             at = hay.find(needle, at + 1)) {
            ++n;
        }
        return n;
    };

    // --- the additive command ids: the free 2017..2018 window, never a
    //     renumber of the frozen engine (2010..2016) or control (2020+)
    //     bands; the recent band fills 2870..2885.
    TEST_CHECK(tray_cpp.find("ID_TRAY_OPENAI_SETTINGS = 2017") != std::string::npos &&
                   tray_cpp.find("ID_TRAY_SETTINGS_HUB = 2018") != std::string::npos,
               "P3-C1 pin: the two additive rows own 2017/2018 (the free window)");
    TEST_CHECK(tray_cpp.find("ID_TRAY_OPENAI_SETTINGS > ID_TRAY_ENGINE_LOAD_SETTINGS") !=
                       std::string::npos &&
                   tray_cpp.find("ID_TRAY_SETTINGS_HUB < ID_TRAY_SWAP") != std::string::npos,
               "P3-C1 pin: the band asserts pin the rows into the 2017..2019 window");
    TEST_CHECK(tray_cpp.find("kTrayRecentLangBase == ID_TRAY_USER_LANG_MANAGE + 4") !=
                       std::string::npos &&
                   tray_cpp.find("kTrayRecentLangBandEnd <= 2900") != std::string::npos,
               "P3-C1 pin: the recent band asserts pin 2870..2885 between the P9b-3 "
               "rows and the tooltip user band");

    // --- the header contract: the two wave-B seams + the MRU helpers.
    TEST_CHECK(tray_hpp.find("void SetOpenSettingsHubCallback(std::function<void()> cb);") !=
                       std::string::npos &&
                   tray_hpp.find("void NoteRecentLanguage(const std::string& code);") !=
                       std::string::npos &&
                   tray_hpp.find("RecentLanguagesForTest()") != std::string::npos,
               "P3-C1 pin: the settings-hub seam, the external MRU seam and the test "
               "seam are declared");
    TEST_CHECK(tray_hpp.find("on_open_settings_hub;") != std::string::npos &&
                   tray_hpp.find("on_open_openai_settings;") != std::string::npos,
               "P3-C1 pin: the two additive Callbacks members are appended, not reordered");
    TEST_CHECK(tray_hpp.find("void RememberRecentLanguage(std::vector<std::string>& list,") !=
                       std::string::npos &&
                   tray_hpp.find("BuildRecentLanguageRows(") != std::string::npos,
               "P3-C1 pin: the pure MRU helpers are declared in tray.hpp");

    // --- the slim top level: exactly ten command rows in the F0..F9 order,
    //     four separators. The 18+-row flat menu cannot come back silently.
    const size_t top_rows = count_of(tray_cpp, "::AppendMenuW(hMenu,");
    const size_t top_seps = count_of(tray_cpp, "hMenu, MF_SEPARATOR");
    TEST_CHECK(top_rows == 14 && top_seps == 4,
               "P3-C1 pin: the top level renders exactly 10 command rows + 4 separators");

    const size_t pos_status = tray_cpp.find("ID_TRAY_STATUS, statusLabel.c_str()");
    const size_t pos_typing = tray_cpp.find("hTypingGroup), I18n::Get(StringId::MenuTypingGroup)");
    const size_t pos_tooltip = tray_cpp.find("hTooltipGroup), I18n::Get(StringId::MenuTooltipGroup)");
    const size_t pos_engine = tray_cpp.find("hEngineMenu), I18n::Get(StringId::MenuEngine)");
    const size_t pos_autosend = tray_cpp.find("ID_TRAY_AUTOSEND, I18n::Get(StringId::MenuAutoSend)");
    const size_t pos_back = tray_cpp.find("ID_TRAY_BACK_TRANSLATE, I18n::Get(StringId::MenuBackTranslation)");
    const size_t pos_settings = tray_cpp.find("hSettingsMenu), settingsLabel.c_str()");
    const size_t pos_cheatsheet = tray_cpp.find("ID_TRAY_CHEATSHEET, I18n::Get(StringId::MenuCheatSheet)");
    const size_t pos_about = tray_cpp.find("ID_TRAY_ABOUT, I18n::Get(StringId::MenuAbout)");
    const size_t pos_exit = tray_cpp.find("ID_TRAY_EXIT, I18n::Get(StringId::MenuExit)");
    TEST_CHECK(pos_status != std::string::npos && pos_typing != std::string::npos &&
                   pos_tooltip != std::string::npos && pos_engine != std::string::npos &&
                   pos_autosend != std::string::npos && pos_back != std::string::npos &&
                   pos_settings != std::string::npos && pos_cheatsheet != std::string::npos &&
                   pos_about != std::string::npos && pos_exit != std::string::npos,
               "P3-C1 pin: all ten top rows are appended from hMenu");
    TEST_CHECK(pos_status < pos_typing && pos_typing < pos_tooltip &&
                   pos_tooltip < pos_engine && pos_engine < pos_autosend &&
                   pos_autosend < pos_back && pos_back < pos_settings &&
                   pos_settings < pos_cheatsheet && pos_cheatsheet < pos_about &&
                   pos_about < pos_exit,
               "P3-C1 pin: the F0..F9 row order (status, typing, drag, engine, toggles, "
               "settings, help, about, exit) holds");

    // --- the settings container is the new home of every legacy settings
    //     command: the hub row first, then UI 언어 ▾ + 테마 ▾ + the three
    //     toggles + the two "내 언어" rows. Every command id survives; only
    //     the depth changed.
    TEST_CHECK(count_of(tray_cpp, "hSettingsMenu, MF_POPUP") == 2,
               "P3-C1 pin: the settings container hosts exactly the UI-language and "
               "theme submenus");
    TEST_CHECK(tray_cpp.find("hSettingsMenu, MF_STRING, ID_TRAY_SETTINGS_HUB") !=
                       std::string::npos &&
                   tray_cpp.find("hSettingsMenu, MF_STRING | (sound_enabled_ ? MF_CHECKED : "
                                 "MF_UNCHECKED), ID_TRAY_SOUND") != std::string::npos &&
                   tray_cpp.find("hSettingsMenu, MF_STRING | (badge_visible_ ? MF_CHECKED : "
                                 "MF_UNCHECKED), ID_TRAY_BADGE") != std::string::npos &&
                   tray_cpp.find("hSettingsMenu, MF_STRING | (startWithWin ? MF_CHECKED : "
                                 "MF_UNCHECKED), ID_TRAY_START_WITH_WINDOWS") !=
                       std::string::npos &&
                   tray_cpp.find("hSettingsMenu, MF_STRING, ID_TRAY_USER_LANG_ADD") !=
                       std::string::npos &&
                   tray_cpp.find("hSettingsMenu, MF_STRING, ID_TRAY_USER_LANG_MANAGE") !=
                       std::string::npos,
               "P3-C1 pin: the hub row and all six legacy settings commands live in the "
               "settings container");
    TEST_CHECK(count_of(tray_cpp, "ID_TRAY_USER_LANG_ADD,") == 1 &&
                   count_of(tray_cpp, "ID_TRAY_USER_LANG_MANAGE,") == 1,
               "P3-C1 pin: the two user-language rows are still appended exactly once each");

    // --- the engine submenu split: the 4-way radio is the instant switch
    //     (the frozen REQ-046 ids/check marks untouched) and the OpenAI
    //     settings dialog owns its own plain row.
    TEST_CHECK(tray_cpp.find("ID_TRAY_OPENAI_SETTINGS,") != std::string::npos &&
                   tray_cpp.find("StringId::OpenAiSettingsAction") != std::string::npos,
               "P3-C1 pin: the OpenAI settings row reuses OpenAiSettingsAction (no new "
               "StringId) and carries no check geometry");
    TEST_CHECK(count_of(tray_sq, "callbacks_.on_select_engine(2);") == 2,
               "P3-C1 pin: the OpenAI radio arm and the settings row's unset fallback "
               "are the only on_select_engine(2) dispatches");
    TEST_CHECK(tray_cpp.find("cmd == ID_TRAY_OPENAI_SETTINGS") != std::string::npos &&
                   tray_cpp.find("callbacks_.on_open_openai_settings") != std::string::npos,
               "P3-C1 pin: the settings row dispatches the additive callback first and "
               "falls back to the radio behavior");
    TEST_CHECK(tray_cpp.find("cmd == ID_TRAY_SETTINGS_HUB") != std::string::npos &&
                   tray_cpp.find("open_settings_hub_()") != std::string::npos &&
                   tray_cpp.find("callbacks_.on_open_settings_hub") != std::string::npos,
               "P3-C1 pin: the hub row invokes the stored seam first, the Callbacks "
               "member second (the SetOnThemeSelectCallback precedence)");

    // --- the two pair submenus carry the recent block between the Swap row
    //     and the 검색… escape hatch.
    TEST_CHECK(count_of(tray_cpp, "StringId::RecentLanguagesHeader") == 2,
               "P3-C1 pin: exactly the two pair submenus render the recent-languages "
               "header");
    TEST_CHECK(tray_cpp.find("TrayRecentLangCommand(TrayRecentPair::Typing, i)") !=
                       std::string::npos &&
                   tray_cpp.find("TrayRecentLangCommand(TrayRecentPair::Drag, i)") !=
                       std::string::npos,
               "P3-C1 pin: both pair slots render their recent rows from the shared build");
    const size_t swap_typing = tray_cpp.find("ID_TRAY_SWAP, I18n::Get(StringId::MenuSwapLangs)");
    const size_t recent_typing = tray_cpp.find("TrayRecentLangCommand(TrayRecentPair::Typing, i)");
    const size_t search_typing = tray_cpp.find("ID_TRAY_SEARCH_TGT, I18n::Get(StringId::MenuSearchLangs)");
    TEST_CHECK(swap_typing != std::string::npos && recent_typing != std::string::npos &&
                   search_typing != std::string::npos && swap_typing < recent_typing &&
                   recent_typing < search_typing,
               "P3-C1 pin: the typing pair's order is Swap, recent block, then the "
               "검색… escape hatch");

    // --- MRU recording at dispatch: the recent-row arm re-promotes its pick,
    //     and every target pick through this surface (registry, drag, user
    //     rows) feeds the one shared list. ONE definition + the
    //     NoteRecentLanguage wrapper + five dispatch sites = 7 mentions.
    TEST_CHECK(count_of(tray_cpp, "RememberRecentLanguage(") == 7,
               "P3-C1 pin: the MRU recorder has one definition, one wrapper seam and "
               "five dispatch recording sites - no surface can silently skip it");
    TEST_CHECK(tray_cpp.find("cmd >= kTrayRecentLangBase && cmd < kTrayRecentLangBandEnd") !=
                       std::string::npos &&
                   tray_cpp.find("DecodeTrayRecentLangCommand(cmd)") != std::string::npos,
               "P3-C1 pin: one range arm decodes both pair slots against the build-time rows");

    // --- zero feature loss: every pre-V10F command id literal survives.
    const char* kLegacyIds[] = {
        "ID_TRAY_STATUS = 2001",  "ID_TRAY_ENGINE_GOOGLE = 2010",
        "ID_TRAY_ENGINE_LOCAL = 2011", "ID_TRAY_ENGINE_OPENAI = 2012",
        "ID_TRAY_ENGINE_USER_GGUF = 2013", "ID_TRAY_BROWSE_GGUF = 2014",
        "ID_TRAY_MANAGE_GGUF = 2015", "ID_TRAY_ENGINE_LOAD_SETTINGS = 2016",
        "ID_TRAY_SWAP = 2020", "ID_TRAY_AUTOSEND = 2030",
        "ID_TRAY_BACK_TRANSLATE = 2035", "ID_TRAY_SOUND = 2040",
        "ID_TRAY_BADGE = 2050", "ID_TRAY_START_WITH_WINDOWS = 2055",
        "ID_TRAY_CHEATSHEET = 2060", "ID_TRAY_ABOUT = 2065",
        "ID_TRAY_EXIT = 2070", "ID_TRAY_SRC_BASE = 2100",
        "ID_TRAY_TGT_BASE = 2200", "ID_TRAY_UILANG_AUTO = 2300",
        "ID_TRAY_UILANG_SEARCH = 2301", "ID_TRAY_DRAG_SRC_BASE = 2400",
        "ID_TRAY_DRAG_TGT_BASE = 2500", "ID_TRAY_DRAG_SWAP = 2600",
        "ID_TRAY_SEARCH_SRC = 2601", "ID_TRAY_SEARCH_TGT = 2602",
        "ID_TRAY_SEARCH_DRAG_SRC = 2603", "ID_TRAY_SEARCH_DRAG_TGT = 2604",
        "ID_TRAY_GHOST_SRC = 2605", "ID_TRAY_GHOST_TGT = 2606",
        "ID_TRAY_GHOST_DRAG_SRC = 2607", "ID_TRAY_GHOST_DRAG_TGT = 2608",
        "ID_TRAY_USER_LANG_ADD = kTrayUserLangBandEnd",
        "ID_TRAY_USER_LANG_MANAGE = kTrayUserLangBandEnd + 1",
    };
    bool all_ids = true;
    for (const char* id : kLegacyIds) {
        if (tray_cpp.find(id) == std::string::npos) {
            all_ids = false;
            break;
        }
    }
    TEST_CHECK(all_ids,
               "P3-C1 pin: zero feature loss - every pre-V10F TrayMenuId literal is "
               "untouched (the enum only grew)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C1 slim-tray structural pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C1 slim-tray structural pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (e) the public MRU seams on a non-Created tray ---------------------------

void TestP3C1TrayRecentSeam() {
    std::cout << "[RUN] Testing P3-C1 tray MRU seams (non-Created tray)..." << std::endl;
    const int failures_before = g_failed_count;

    // The pure-state precedent (TipTextForTest): no HWND, no shell call.
    emebalachat::SystemTray tray;

    tray.NoteRecentLanguage("Korean");
    TEST_CHECK(tray.RecentLanguagesForTest().size() == 1 &&
                   tray.RecentLanguagesForTest()[0] == "KO",
               "P3-C1: NoteRecentLanguage normalizes through the registry authority");

    tray.NoteRecentLanguage("KO");
    TEST_CHECK(tray.RecentLanguagesForTest().size() == 1,
               "P3-C1: the external seam dedupes like the tray's own dispatch");

    tray.NoteRecentLanguage("English");
    TEST_CHECK(tray.RecentLanguagesForTest().size() == 2 &&
                   tray.RecentLanguagesForTest()[0] == "EN",
               "P3-C1: a new language lands at the front");

    const std::size_t before = tray.RecentLanguagesForTest().size();
    tray.NoteRecentLanguage("Auto Detect");
    tray.NoteRecentLanguage("");
    tray.NoteRecentLanguage("not-a-language-xyz");
    TEST_CHECK(tray.RecentLanguagesForTest().size() == before,
               "P3-C1: the external seam drops AUTO / empty / unresolvable tokens");

    const char* kCodes[] = {"JA", "ZH-CN", "FR", "DE", "ES", "PT", "RU", "IT", "VI", "TH"};
    for (const char* code : kCodes) {
        tray.NoteRecentLanguage(code);
    }
    TEST_CHECK(tray.RecentLanguagesForTest().size() ==
                       emebalachat::kTrayRecentLanguageCapacity &&
                   tray.RecentLanguagesForTest()[0] == "TH",
               "P3-C1: the external seam honours the same cap, newest first");

    // The hub seam stores a callback on a tray that was never Created: the
    // store is pure state (the row it feeds is built only on menu open).
    bool invoked = false;
    tray.SetOpenSettingsHubCallback([&invoked]() { invoked = true; });
    TEST_CHECK(!invoked,
               "P3-C1: SetOpenSettingsHubCallback only stores (nothing fires without "
               "a menu dispatch)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C1 tray MRU seams passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C1 tray MRU seams: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- registration ---------------------------------------------------------

namespace p3_c1_tests {

void P3C1RegisterTests() {
    TestP3C1RecentLanguageMru();
    TestP3C1RecentLanguageRows();
    TestP3C1RecentCommandBand();
    TestP3C1TraySlimSourcePins();
    TestP3C1TrayRecentSeam();
}

} // namespace p3_c1_tests
