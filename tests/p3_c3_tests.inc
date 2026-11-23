// ---------------------------------------------------------------------------
// V10F (v0.10.10 phased fixes, unit P3-C3): the four-tab settings hub (UX-2)
// and the user-language manager CRUD pair (UX-4).
//
// Covered:
//   (a) UX-2 tab strip mapping — HubTabCaptionId pins the four landed Phase-3
//       ids in strip order (SettingsTabGeneral .. SettingsTabAdvanced) and
//       degrades out-of-range reads to the first tab (no blank caption).
//   (b) UX-2 page ownership — HubControlTab maps every control band to exactly
//       one page, chrome (kIdTab / IDOK / IDCANCEL) maps to -1, and every id
//       in kHubPageControlIds resolves to a page (a template control forgotten
//       in the table can never be switched to).
//   (c) UX-2 warmup absorption — HubWarmupComboIndex / HubComboIndexWarmup are
//       the REQ-060 round trip (the hub combo and the still-live
//       engine_host_settings_window cannot disagree).
//   (d) UX-2 Advanced parsing — ParseHubFloat / ParseHubInt clamp into the
//       pinned ranges and refuse empty / partial / non-finite text;
//       FormatHubFloat renders the round-trip-stable spelling.
//   (e) UX-2 hotkey rows — MakeHubHotkeyField routes through the hook's own
//       parser and persists only the canonical spelling.
//   (f) UX-2 save plan — FoldHubDraft: identical draft -> ok + no change;
//       edited values -> change; unparsable field -> !ok + per-field refusal.
//   (g) UX-2 draft seed — MakeHubDraft reads the live config through the
//       documented seams (Snapshot / locked engine-host read / registry probe).
//   (h) UX-2 display folds — ComposePairSummary / ComposeUserLanguageSummary /
//       ComposeDeclaredLanguagesSummary / DescribeUiLanguageValue.
//   (i) UX-4 removal guard — PlanUserLanguageRemoval: carrier gate, empty
//       name, found -> index, stale name -> not_found.
//   (j) Template shapes — BuildSettingsHubTemplate emits exactly kHubItemCount
//       items with the shared WS_VISIBLE dialog style;
//       BuildUserLanguageManagerTemplate still emits kDialogItemCount == 10
//       items (the UX-4 runtime CRUD row never touched the template bytes).
//   (k) UX-4 manager CRUD wiring (source pins): the WM_COMMAND notification
//       code seam, the runtime button creation, the SAME add entry point
//       main.cpp uses, the shared removal guard, and the locked verb call.
// ---------------------------------------------------------------------------

#include <fstream>
#include <string>

#include "../src/ui/settings_hub_window.hpp"
#include "../src/ui/user_language_manager_dialog.hpp"
#include "../src/ui/user_language_dialog.hpp"

namespace p3_c3_tests {

// Resolve + read a production source, normalized to LF (needles are ASCII).
void P3C3ReadSrc(const wchar_t* rel, std::string& out) {
    out.clear();
    const std::filesystem::path p = ResolveRepoFile(rel);
    if (p.empty()) return;
    std::ifstream in(p, std::ios::binary);
    if (!in) return;
    out.assign((std::istreambuf_iterator<char>(in)),
               std::istreambuf_iterator<char>());
    std::string norm;
    norm.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\r' && i + 1 < out.size() && out[i + 1] == '\n') continue;
        norm.push_back(out[i]);
    }
    out.swap(norm);
}

size_t P3C3Count(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    size_t n = 0;
    size_t pos = hay.find(needle);
    while (pos != std::string::npos) {
        ++n;
        pos = hay.find(needle, pos + needle.size());
    }
    return n;
}

// ---- (a)+(b): tab captions and page ownership -----------------------------

void TestP3C3TabStripAndPageMapping() {
    std::cout << "[RUN] Testing P3-C3 hub tab strip + page mapping (V10F UX-2)..."
              << std::endl;
    const int failures_before = g_failed_count;

    TEST_CHECK(settings_hub::HubTabCaptionId(0) == StringId::SettingsTabGeneral,
               "P3-C3 UX-2: tab 0 caption is SettingsTabGeneral");
    TEST_CHECK(settings_hub::HubTabCaptionId(1) == StringId::SettingsTabEngines,
               "P3-C3 UX-2: tab 1 caption is SettingsTabEngines");
    TEST_CHECK(settings_hub::HubTabCaptionId(2) == StringId::SettingsTabLanguages,
               "P3-C3 UX-2: tab 2 caption is SettingsTabLanguages");
    TEST_CHECK(settings_hub::HubTabCaptionId(3) == StringId::SettingsTabAdvanced,
               "P3-C3 UX-2: tab 3 caption is SettingsTabAdvanced");
    // Out-of-range reads the FIRST tab's caption (no blank tab can render).
    TEST_CHECK(settings_hub::HubTabCaptionId(-1) == StringId::SettingsTabGeneral &&
                   settings_hub::HubTabCaptionId(99) == StringId::SettingsTabGeneral,
               "P3-C3 UX-2: out-of-range tab indexes degrade to the first caption");

    // Page band boundaries (the enum is contiguous per band).
    TEST_CHECK(settings_hub::HubControlTab(settings_hub::kIdAutoSend) == 0 &&
                   settings_hub::HubControlTab(settings_hub::kIdHotkeySelection) == 0,
               "P3-C3 UX-2: the General band (4801..4824) maps to page 0");
    TEST_CHECK(settings_hub::HubControlTab(settings_hub::kIdWarmupLabel) == 1 &&
                   settings_hub::HubControlTab(settings_hub::kIdDeclaredSummary) == 1,
               "P3-C3 UX-2: the Engines band (4830..4837) maps to page 1");
    TEST_CHECK(settings_hub::HubControlTab(settings_hub::kIdTypingPairLabel) == 2 &&
                   settings_hub::HubControlTab(settings_hub::kIdUserLangManage) == 2,
               "P3-C3 UX-2: the Languages band (4840..4849) maps to page 2");
    TEST_CHECK(settings_hub::HubControlTab(settings_hub::kIdDiagLogEnabled) == 3 &&
                   settings_hub::HubControlTab(settings_hub::kIdCheatSheet) == 3,
               "P3-C3 UX-2: the Advanced band (4852..4866) maps to page 3");
    // Chrome the page switch never touches.
    TEST_CHECK(settings_hub::HubControlTab(settings_hub::kIdTab) == -1 &&
                   settings_hub::HubControlTab(IDOK) == -1 &&
                   settings_hub::HubControlTab(IDCANCEL) == -1,
               "P3-C3 UX-2: the tab strip and the footer buttons map to -1");

    // The table the ShowPage loop walks: 57 page controls, every one owned.
    TEST_CHECK(settings_hub::kHubPageControlCount == 57,
               "P3-C3 UX-2: kHubPageControlIds carries exactly 57 page controls");
    bool all_mapped = true;
    for (std::size_t i = 0; i < settings_hub::kHubPageControlCount; ++i) {
        if (settings_hub::HubControlTab(settings_hub::kHubPageControlIds[i]) < 0) {
            all_mapped = false;
            break;
        }
    }
    TEST_CHECK(all_mapped,
               "P3-C3 UX-2: every kHubPageControlIds entry belongs to a page");

    // Hotkey edit slot mapping (the EN_KILLFOCUS normalizer's only lookup).
    TEST_CHECK(settings_hub::HubHotkeySlot(settings_hub::kIdHotkeyToggle) == 0 &&
                   settings_hub::HubHotkeySlot(settings_hub::kIdHotkeyLang) == 1 &&
                   settings_hub::HubHotkeySlot(settings_hub::kIdHotkeyMode) == 2 &&
                   settings_hub::HubHotkeySlot(settings_hub::kIdHotkeySelection) == 3 &&
                   settings_hub::HubHotkeySlot(settings_hub::kIdAutoSend) == -1,
               "P3-C3 UX-2: the four hotkey edits map to slots 0..3, others -1");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C3 tab strip + page mapping passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C3 tab strip + page mapping: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}

// ---- (c): the REQ-060 warmup round trip -----------------------------------

void TestP3C3WarmupComboMapping() {
    std::cout << "[RUN] Testing P3-C3 warmup combo mapping (V10F UX-2)..." << std::endl;
    const int failures_before = g_failed_count;

    TEST_CHECK(settings_hub::HubWarmupComboIndex(false) == 0 &&
                   settings_hub::HubWarmupComboIndex(true) == 1,
               "P3-C3 UX-2: warmup false -> combo index 0, true -> 1");
    TEST_CHECK(!settings_hub::HubComboIndexWarmup(0) &&
                   settings_hub::HubComboIndexWarmup(1),
               "P3-C3 UX-2: combo index 0 -> load-when-used, 1 -> preload");
    // Any index outside the fixed two-option list reads the engine default
    // (load when used), so a corrupt selection can never arm preload.
    TEST_CHECK(!settings_hub::HubComboIndexWarmup(-1) &&
                   !settings_hub::HubComboIndexWarmup(2),
               "P3-C3 UX-2: out-of-range combo indexes read the default policy");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C3 warmup combo mapping passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C3 warmup combo mapping: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}

// ---- (d): Advanced number parsing ------------------------------------------

void TestP3C3NumberParsing() {
    std::cout << "[RUN] Testing P3-C3 hub number parsing (V10F UX-2)..." << std::endl;
    const int failures_before = g_failed_count;

    float f = 0.0f;
    TEST_CHECK(settings_hub::ParseHubFloat("0.6", settings_hub::kHubTemperatureMin,
                                           settings_hub::kHubTemperatureMax, f) &&
                   f == 0.6f,
               "P3-C3 UX-2: ParseHubFloat parses 0.6");
    TEST_CHECK(settings_hub::ParseHubFloat(" 1.05 ", 0.0f, 2.0f, f) && f == 1.05f,
               "P3-C3 UX-2: ParseHubFloat trims ASCII whitespace");
    TEST_CHECK(settings_hub::ParseHubFloat("5", 0.0f, 2.0f, f) && f == 2.0f,
               "P3-C3 UX-2: ParseHubFloat clamps high into range");
    TEST_CHECK(settings_hub::ParseHubFloat("-1", 0.0f, 2.0f, f) && f == 0.0f,
               "P3-C3 UX-2: ParseHubFloat clamps low into range");
    TEST_CHECK(!settings_hub::ParseHubFloat("", 0.0f, 2.0f, f),
               "P3-C3 UX-2: ParseHubFloat refuses empty text");
    TEST_CHECK(!settings_hub::ParseHubFloat("1x", 0.0f, 2.0f, f),
               "P3-C3 UX-2: ParseHubFloat refuses a partial parse");
    TEST_CHECK(!settings_hub::ParseHubFloat("0.6.1", 0.0f, 2.0f, f),
               "P3-C3 UX-2: ParseHubFloat refuses a doubled separator");
    TEST_CHECK(!settings_hub::ParseHubFloat("nan", 0.0f, 2.0f, f) &&
                   !settings_hub::ParseHubFloat("inf", 0.0f, 2.0f, f),
               "P3-C3 UX-2: ParseHubFloat refuses non-finite values");

    int k = 0;
    TEST_CHECK(settings_hub::ParseHubInt("20", settings_hub::kHubTopKMin,
                                         settings_hub::kHubTopKMax, k) &&
                   k == 20,
               "P3-C3 UX-2: ParseHubInt parses 20");
    TEST_CHECK(settings_hub::ParseHubInt("99999", 0, 1000, k) && k == 1000,
               "P3-C3 UX-2: ParseHubInt clamps high into range");
    TEST_CHECK(settings_hub::ParseHubInt("-5", 0, 1000, k) && k == 0,
               "P3-C3 UX-2: ParseHubInt clamps low into range");
    TEST_CHECK(settings_hub::ParseHubInt("  42  ", 0, 1000, k) && k == 42,
               "P3-C3 UX-2: ParseHubInt trims ASCII whitespace");
    TEST_CHECK(!settings_hub::ParseHubInt("12x", 0, 1000, k) &&
                   !settings_hub::ParseHubInt("", 0, 1000, k),
               "P3-C3 UX-2: ParseHubInt refuses partial / empty text");

    // Round-trip-stable rendering (<= 3 decimals survive byte-exactly).
    TEST_CHECK(settings_hub::FormatHubFloat(0.6f) == L"0.6",
               "P3-C3 UX-2: FormatHubFloat(0.6) == \"0.6\"");
    TEST_CHECK(settings_hub::FormatHubFloat(1.0f) == L"1",
               "P3-C3 UX-2: FormatHubFloat(1) == \"1\"");
    TEST_CHECK(settings_hub::FormatHubFloat(1.05f) == L"1.05",
               "P3-C3 UX-2: FormatHubFloat(1.05) == \"1.05\"");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C3 number parsing passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C3 number parsing: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}

// ---- (e): hotkey rows through the hook's own parser ------------------------

void TestP3C3HotkeyFields() {
    std::cout << "[RUN] Testing P3-C3 hotkey field folds (V10F UX-2)..." << std::endl;
    const int failures_before = g_failed_count;

    const settings_hub::HubHotkeyField ctrl_f9 =
        settings_hub::MakeHubHotkeyField("Ctrl+F9");
    TEST_CHECK(ctrl_f9.valid && ctrl_f9.canonical == "Ctrl+F9" &&
                   ctrl_f9.raw == "Ctrl+F9",
               "P3-C3 UX-2: Ctrl+F9 is valid and stays canonical");
    const settings_hub::HubHotkeyField lower =
        settings_hub::MakeHubHotkeyField("ctrl+f9");
    TEST_CHECK(lower.valid && lower.canonical == "Ctrl+F9",
               "P3-C3 UX-2: lowercase input normalizes to the canonical spelling");
    const settings_hub::HubHotkeyField bare =
        settings_hub::MakeHubHotkeyField("F9");
    TEST_CHECK(bare.valid && bare.canonical == "F9",
               "P3-C3 UX-2: a bare key is valid");
    const settings_hub::HubHotkeyField garbage =
        settings_hub::MakeHubHotkeyField("notakey");
    TEST_CHECK(!garbage.valid && garbage.canonical.empty() &&
                   garbage.raw == "notakey",
               "P3-C3 UX-2: an unparsable combo keeps its raw text, no canonical");
    const settings_hub::HubHotkeyField empty =
        settings_hub::MakeHubHotkeyField("   ");
    TEST_CHECK(!empty.valid && empty.raw.empty(),
               "P3-C3 UX-2: whitespace-only input is invalid and trims to empty");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C3 hotkey field folds passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C3 hotkey field folds: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}

// ---- (f)+(g): the draft seed and the save plan -----------------------------

void TestP3C3DraftAndSavePlan() {
    std::cout << "[RUN] Testing P3-C3 draft seed + save plan (V10F UX-2)..." << std::endl;
    const int failures_before = g_failed_count;

    AppConfig cfg;
    HubRuntimeState runtime;
    runtime.badge_visible = false; // the one value AppConfig does not carry
    const settings_hub::HubDraft seed = settings_hub::MakeHubDraft(cfg, runtime);
    TEST_CHECK(!seed.auto_send && seed.sound && !seed.back_translation,
               "P3-C3 UX-2: the draft seeds the three atomic toggles from config");
    TEST_CHECK(!seed.badge_visible,
               "P3-C3 UX-2: the draft carries the injected runtime badge state");
    TEST_CHECK(seed.drag_to_translate && !seed.cloud_fallback,
               "P3-C3 UX-2: the draft seeds drag-to-translate / cloud fallback");
    TEST_CHECK(seed.ui_theme == "fluent" && seed.warmup_combo_index == 0,
               "P3-C3 UX-2: the default theme is fluent and the default load "
               "policy is load-when-used (combo 0)");
    TEST_CHECK(seed.top_k == "20" && seed.temperature == "0" &&
                   seed.top_p == "0.6" && seed.repetition_penalty == "1",
               "P3-C3 UX-2: the sampling edits seed from the config defaults");
    TEST_CHECK(seed.hotkey[0].valid && seed.hotkey[0].canonical == "F9" &&
                   seed.hotkey[1].canonical == "Ctrl+F9" &&
                   seed.hotkey[2].canonical == "Ctrl+Shift+Enter" &&
                   seed.hotkey[3].canonical == "Ctrl+Alt+T",
               "P3-C3 UX-2: the four hotkey rows seed through the resolve helpers");

    // Identical draft -> ok and NO change (an untouched hub never writes).
    settings_hub::HubApplyPlan plan = settings_hub::FoldHubDraft(seed, seed);
    TEST_CHECK(plan.ok && !plan.any_change,
               "P3-C3 UX-2: an unedited draft folds to ok + no change");

    // Edited values -> change.
    settings_hub::HubDraft edited = seed;
    edited.temperature = "0.5";
    plan = settings_hub::FoldHubDraft(edited, seed);
    TEST_CHECK(plan.ok && plan.any_change && plan.extra.temperature == 0.5f,
               "P3-C3 UX-2: a temperature edit folds to a persisted change");

    // Clamp: a parseable out-of-range number is clamped, never refused.
    edited = seed;
    edited.temperature = "99";
    plan = settings_hub::FoldHubDraft(edited, seed);
    TEST_CHECK(plan.ok && plan.extra.temperature == settings_hub::kHubTemperatureMax,
               "P3-C3 UX-2: an out-of-range temperature clamps to the max");

    // Warmup flip -> change.
    edited = seed;
    edited.warmup_combo_index = 1;
    plan = settings_hub::FoldHubDraft(edited, seed);
    TEST_CHECK(plan.ok && plan.warmup && plan.any_change,
               "P3-C3 UX-2: selecting preload folds to a warmup change");

    // Unparsable field -> !ok + the per-field refusal gate (dialog stays open).
    edited = seed;
    edited.hotkey[0].valid = false;
    plan = settings_hub::FoldHubDraft(edited, seed);
    TEST_CHECK(!plan.ok && plan.errors.hotkey_toggle,
               "P3-C3 UX-2: an invalid hotkey refuses the whole apply");
    edited = seed;
    edited.top_k = "abc";
    plan = settings_hub::FoldHubDraft(edited, seed);
    TEST_CHECK(!plan.ok && plan.errors.top_k,
               "P3-C3 UX-2: an unparsable top-k refuses the whole apply");

    // The lock-free toggles ride the extra bundle.
    edited = seed;
    edited.cloud_fallback = true;
    plan = settings_hub::FoldHubDraft(edited, seed);
    TEST_CHECK(plan.ok && plan.extra.cloud_fallback_enabled && plan.any_change,
               "P3-C3 UX-2: the cloud-fallback toggle travels in HubExtraSettings");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C3 draft seed + save plan passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C3 draft seed + save plan: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}

// ---- (h): display folds -----------------------------------------------------

void TestP3C3DisplayFolds() {
    std::cout << "[RUN] Testing P3-C3 display folds (V10F UX-2)..." << std::endl;
    const int failures_before = g_failed_count;

    // Pair summary: empty pair -> nothing; a user-language name passes through
    // verbatim (the registry resolves real codes and passes unknown tokens).
    TEST_CHECK(settings_hub::ComposePairSummary("", "").empty(),
               "P3-C3 UX-2: an empty pair composes to an empty summary");
    const std::wstring passthrough =
        settings_hub::ComposePairSummary("MyLang", "");
    TEST_CHECK(passthrough == L"MyLang \u2192 ",
               "P3-C3 UX-2: an unknown stored name renders verbatim with the arrow");

    // User-language summary: join + the overflow marker.
    auto entry_with_name = [](const char* name) {
        UserLanguage e;
        e.native_name = name;
        return e;
    };
    TEST_CHECK(settings_hub::ComposeUserLanguageSummary({}).empty(),
               "P3-C3 UX-2: an empty registry composes to an empty summary");
    std::vector<UserLanguage> four;
    four.push_back(entry_with_name("A"));
    four.push_back(entry_with_name("B"));
    four.push_back(entry_with_name("C"));
    four.push_back(entry_with_name("D"));
    TEST_CHECK(settings_hub::ComposeUserLanguageSummary(four) == L"A, B, C, D",
               "P3-C3 UX-2: up to four names join with \", \"");
    four.push_back(entry_with_name("E"));
    TEST_CHECK(settings_hub::ComposeUserLanguageSummary(four) == L"A, B, C, D \u2026",
               "P3-C3 UX-2: a fifth name collapses into the ellipsis");

    // Declared-language count: empty -> "", otherwise the {n} token is replaced.
    TEST_CHECK(settings_hub::ComposeDeclaredLanguagesSummary({}).empty(),
               "P3-C3 UX-2: nothing declared composes to an empty summary");
    const std::wstring declared =
        settings_hub::ComposeDeclaredLanguagesSummary({"ko", "ja"});
    TEST_CHECK(!declared.empty() && declared.find(L"{n}") == std::wstring::npos &&
                   declared.find(L"2") != std::wstring::npos,
               "P3-C3 UX-2: the declared count substitutes the {n} token once");

    // UI-language value cell.
    TEST_CHECK(settings_hub::DescribeUiLanguageValue("auto") ==
                       I18n::Get(StringId::MenuUiLanguageAuto) &&
                   settings_hub::DescribeUiLanguageValue(" AUTO ") ==
                       I18n::Get(StringId::MenuUiLanguageAuto),
               "P3-C3 UX-2: the auto token (any case, trimmed) shows the Auto row");
    TEST_CHECK(settings_hub::DescribeUiLanguageValue("ko") == L"KO" &&
                   settings_hub::DescribeUiLanguageValue("EN") == L"EN",
               "P3-C3 UX-2: a persisted code renders uppercased");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C3 display folds passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C3 display folds: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}

// ---- (i): the shared user-language removal guard ----------------------------

void TestP3C3RemovalGuard() {
    std::cout << "[RUN] Testing P3-C3 user-language removal guard (V10F UX-4)..."
              << std::endl;
    const int failures_before = g_failed_count;

    UserLanguage a;
    a.native_name = "Alpha";
    UserLanguage b;
    b.native_name = "Beta";
    b.english_name = "Betaish"; // the STORED name is the english half
    const std::vector<UserLanguage> entries{a, b};

    // Carrier armed (PR9-12): every write verb refuses, so the delete is
    // disabled without even looking at the selection.
    const settings_hub::UserLanguageRemovalPlan blocked =
        settings_hub::PlanUserLanguageRemoval(entries, "Betaish", false);
    TEST_CHECK(!blocked.enabled && blocked.carrier_blocks && !blocked.not_found,
               "P3-C3 UX-4: the forward-compat carrier blocks the delete");

    // No selection -> disabled, no index.
    const settings_hub::UserLanguageRemovalPlan none =
        settings_hub::PlanUserLanguageRemoval(entries, "", true);
    TEST_CHECK(!none.enabled && !none.carrier_blocks && !none.not_found,
               "P3-C3 UX-4: an empty selection disables the delete");

    // Stored-name resolution (english half wins) -> the config index.
    const settings_hub::UserLanguageRemovalPlan hit =
        settings_hub::PlanUserLanguageRemoval(entries, "Betaish", true);
    TEST_CHECK(hit.enabled && hit.index == 1 && !hit.carrier_blocks,
               "P3-C3 UX-4: the selected stored name resolves to its config index");
    const settings_hub::UserLanguageRemovalPlan first =
        settings_hub::PlanUserLanguageRemoval(entries, "Alpha", true);
    TEST_CHECK(first.enabled && first.index == 0,
               "P3-C3 UX-4: a native-only entry resolves by its native name");

    // Stale selection (row refreshed away) -> not_found, still disabled.
    const settings_hub::UserLanguageRemovalPlan stale =
        settings_hub::PlanUserLanguageRemoval(entries, "Gone", true);
    TEST_CHECK(!stale.enabled && stale.not_found && !stale.carrier_blocks,
               "P3-C3 UX-4: a name no longer in the registry refuses as not_found");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C3 removal guard passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C3 removal guard: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}

// ---- (j): template shapes ---------------------------------------------------

void TestP3C3TemplateShapes() {
    std::cout << "[RUN] Testing P3-C3 template shapes (V10F UX-2/UX-4)..." << std::endl;
    const int failures_before = g_failed_count;

    // The hub: exactly kHubItemCount items with the shared dialog style. The
    // buffer begins empty so the DLGTEMPLATE sits at offset 0 and cdit is the
    // little-endian WORD at byte offset 8 (the REQ-048 P2 layout contract).
    TemplateBuilder hub_tb;
    hub_tb.Begin(L"hub", settings_hub::kHubWidthDlu, settings_hub::kHubHeightDlu,
                 settings_hub::kHubItemCount);
    BuildSettingsHubTemplate(hub_tb);
    const unsigned char* hub_bytes =
        reinterpret_cast<const unsigned char*>(hub_tb.Get());
    TEST_CHECK(hub_bytes != nullptr, "P3-C3 UX-2: the hub template builds");
    if (hub_bytes != nullptr) {
        const unsigned int style =
            static_cast<unsigned int>(hub_bytes[0]) |
            (static_cast<unsigned int>(hub_bytes[1]) << 8) |
            (static_cast<unsigned int>(hub_bytes[2]) << 16) |
            (static_cast<unsigned int>(hub_bytes[3]) << 24);
        TEST_CHECK((style & WS_VISIBLE) != 0,
                   "P3-C3 UX-2: the hub dialog style carries WS_VISIBLE");
        const unsigned int cdit = static_cast<unsigned int>(hub_bytes[8]) |
                                  (static_cast<unsigned int>(hub_bytes[9]) << 8);
        TEST_CHECK(cdit == settings_hub::kHubItemCount && cdit == 59,
                   "P3-C3 UX-2: the hub template emits exactly 59 items "
                   "(57 page controls + OK + Cancel)");
    }

    // The manager dialog: the UX-4 CRUD pair is a RUNTIME row, so the template
    // still carries exactly kDialogItemCount == 10 items (the p9b3 byte-walk
    // and the count pin stay green untouched).
    TemplateBuilder mgr_tb;
    mgr_tb.Begin(L"mgr", user_language_manager_dialog::kDialogWidthDlu,
                 user_language_manager_dialog::kDialogHeightDlu,
                 user_language_manager_dialog::kDialogItemCount);
    user_language_manager_dialog::BuildUserLanguageManagerTemplate(mgr_tb);
    const unsigned char* mgr_bytes =
        reinterpret_cast<const unsigned char*>(mgr_tb.Get());
    TEST_CHECK(mgr_bytes != nullptr, "P3-C3 UX-4: the manager template builds");
    if (mgr_bytes != nullptr) {
        const unsigned int cdit = static_cast<unsigned int>(mgr_bytes[8]) |
                                  (static_cast<unsigned int>(mgr_bytes[9]) << 8);
        TEST_CHECK(cdit == user_language_manager_dialog::kDialogItemCount &&
                       cdit == 10,
                   "P3-C3 UX-4: the manager template stays byte-shaped at 10 "
                   "items (CRUD row is runtime-created)");
    }

    // The 4800 control band sits past every other dialog's band and past the
    // manager's WM_TIMER id, so no control and no timer can alias.
    TEST_CHECK(settings_hub::kIdTab == 4800 &&
                   settings_hub::kIdCheatSheet < 4870 &&
                   user_language_manager_dialog::kInFlightTimerId == 4710,
               "P3-C3: the hub 4800 band and the manager timer id leave no alias");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C3 template shapes passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C3 template shapes: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}

// ---- (k): the manager CRUD wiring (source pins) -----------------------------

void TestP3C3ManagerCrudWiring() {
    std::cout << "[RUN] Testing P3-C3 manager CRUD wiring (V10F UX-4)..." << std::endl;
    const int failures_before = g_failed_count;

    std::string mgr;
    P3C3ReadSrc(L"src\\ui\\user_language_manager_dialog.cpp", mgr);
    if (mgr.empty()) {
        std::cout << "[SKIP] user_language_manager_dialog.cpp not resolvable; "
                     "P3-C3 UX-4 pins skipped." << std::endl;
        return;
    }

    // The WM_COMMAND notification-code seam (the UX-4 handlers read BN_CLICKED
    // through the named local, not a bare literal).
    TEST_CHECK(mgr.find("const WORD code = HIWORD(wp);") != std::string::npos,
               "P3-C3 UX-4: WM_COMMAND names the notification-code local");

    // The runtime CRUD row (the template stays byte-identical).
    TEST_CHECK(mgr.find("void CreateCrudButtons(HWND dlg)") != std::string::npos &&
                   P3C3Count(mgr, "CreateCrudButtons(dlg);") >= 1,
               "P3-C3 UX-4: the CRUD pair is created at runtime in WM_INITDIALOG");
    TEST_CHECK(mgr.find("kIdAdd = 4711") != std::string::npos &&
                   mgr.find("kIdDelete = 4712") != std::string::npos,
               "P3-C3 UX-4: the CRUD ids sit past the timer id (4711/4712)");

    // [추가…] reuses the SAME entry point the tray row uses (main.cpp's call
    // site is user_language_dialog::ShowUserLanguageDialogEx) and only a
    // kAdded outcome marks the dialog changed.
    TEST_CHECK(mgr.find("user_language_dialog::ShowUserLanguageDialogEx(dlg, "
                        "*st->config);") != std::string::npos,
               "P3-C3 UX-4: [추가…] runs the tray's own add-dialog entry point");
    TEST_CHECK(mgr.find("outcome == UserLanguageDialogOutcome::kAdded") !=
                   std::string::npos,
               "P3-C3 UX-4: only the kAdded outcome marks the manager changed");

    // [삭제] goes through the ONE shared guard and the locked verb.
    TEST_CHECK(P3C3Count(mgr, "settings_hub::PlanUserLanguageRemoval(") >= 2,
               "P3-C3 UX-4: the removal guard gates both the button and the verb");
    TEST_CHECK(mgr.find("st->config->RemoveUserLanguage(plan.index);") !=
                   std::string::npos,
               "P3-C3 UX-4: [삭제] applies through AppConfig::RemoveUserLanguage");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C3 manager CRUD wiring passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C3 manager CRUD wiring: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}

void P3C3RegisterTests() {
    TestP3C3TabStripAndPageMapping();
    TestP3C3WarmupComboMapping();
    TestP3C3NumberParsing();
    TestP3C3HotkeyFields();
    TestP3C3DraftAndSavePlan();
    TestP3C3DisplayFolds();
    TestP3C3RemovalGuard();
    TestP3C3TemplateShapes();
    TestP3C3ManagerCrudWiring();
}

} // namespace p3_c3_tests
