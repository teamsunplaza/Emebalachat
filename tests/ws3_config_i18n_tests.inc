// ---------------------------------------------------------------------------
// 261108_0002 (WS3) - v0.10.6 floating back-translation verification UI
// foundations: the config field, the source-language ladder, the four i18n
// keys and the tray toggle wiring.
//
// User intent (verbatim): "신규 기능 #3: 번역 결과(BBBBB)를 원문 언어로 자동
// 역번역(AAAAA')하여 플로팅 카드로 보여주는 역번역 플로팅 확인 UI (Floating
// Back-Translation Verification) 및 트레이/설정 On/Off 토글 구현".
//
// This suite pins the four foundations the UI layer consumes:
//   (a) AppConfig::back_translation_enabled - default false, JSON round trip
//       (true / false / absent) through the same public ToJsonString /
//       FromJsonString seam the other toggles use, plus the Snapshot copy the
//       pipeline worker reads.
//   (b) ResolveBackTranslationTarget - the four-tier ladder (pinned type
//       source -> detection -> OS language -> ADR-A1-7 pivot), including the
//       "a candidate equal to the forward target is skipped at EVERY tier"
//       rule and the AUTO-detected-source shape the worker actually produces.
//   (c) I18n::Get non-empty for the four new StringIds across all 37
//       GetSupportedUiLocales() entries, the Korean/English literal pins, and
//       the StringId ordering invariant (the four ids sit below EnumCount).
//   (d) Tray structural pins: the ID band (2035, no collision), the callback
//       member, the setter, the AppendMenuW row below auto-send, and the
//       command dispatch - all read from src/ui/tray.{hpp,cpp} as text
//       because ShowContextMenu needs a live HWND/message loop.
//
// Self-contained: no main(), and no include beyond what tests/run_tests.cpp
// already provides (config.hpp, i18n.hpp, ui/tray.hpp, <fstream>, <string>,
// <vector>). The orchestrator wires the #include and the call.
// ---------------------------------------------------------------------------

namespace {

// Reads a repo-relative source file the same way the other source-pin suites
// do (exe dir, then one and two levels up - the test binary lives in build/).
std::string Ws3ReadSrc(const char* rel) {
    std::string out;
    const char* prefixes[] = {"", "../", "../../"};
    for (const char* pre : prefixes) {
        std::ifstream in((std::string(pre) + rel).c_str(), std::ios::binary);
        if (in) {
            out.assign((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
            break;
        }
    }
    return out;
}

std::size_t Ws3CountOcc(const std::string& hay, const std::string& needle) {
    std::size_t n = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

} // namespace

// ---------------------------------------------------------------------------
// (a) config field + JSON round trip
// ---------------------------------------------------------------------------
void TestWs3BackTranslationConfig() {
    std::cout << "[RUN] Testing WS3 back_translation_enabled config round trip..." << std::endl;
    const int failures_before = g_failed_count;

    // Default: OFF. The card costs a second engine round trip, so a fresh
    // install (and every pre-261108 config.json, where the key is absent)
    // must not pay for it.
    {
        AppConfig cfg;
        TEST_CHECK(cfg.back_translation_enabled.load() == false,
                   "WS3: back_translation_enabled defaults to false");
        TEST_CHECK(cfg.GetSnapshot().back_translation_enabled == false,
                   "WS3: the Snapshot default is false too");
    }

    // true -> serialize -> parse -> true, with the key present in the JSON.
    {
        AppConfig cfg;
        cfg.back_translation_enabled.store(true);
        const std::string json = cfg.ToJsonString();
        TEST_CHECK(json.find("\"back_translation_enabled\": true") != std::string::npos,
                   "WS3: ToJsonString emits back_translation_enabled: true when enabled");
        AppConfig loaded;
        TEST_CHECK(loaded.FromJsonString(json), "WS3: the serialized config parses");
        TEST_CHECK(loaded.back_translation_enabled.load() == true,
                   "WS3: back_translation_enabled true survives the round trip");
        TEST_CHECK(loaded.GetSnapshot().back_translation_enabled == true,
                   "WS3: the enabled state reaches the worker through GetSnapshot");
    }

    // false -> serialize -> parse -> false (an explicit opt-out is preserved,
    // not silently re-defaulted by the parse).
    {
        AppConfig cfg;
        cfg.back_translation_enabled.store(false);
        const std::string json = cfg.ToJsonString();
        TEST_CHECK(json.find("\"back_translation_enabled\": false") != std::string::npos,
                   "WS3: ToJsonString emits back_translation_enabled: false when disabled");
        AppConfig loaded;
        TEST_CHECK(loaded.FromJsonString(json), "WS3: the disabled config parses");
        TEST_CHECK(loaded.back_translation_enabled.load() == false,
                   "WS3: back_translation_enabled false survives the round trip");
    }

    // Absent key (every pre-261108 config.json): the field keeps its
    // compile-time default false. This is the SAME convention every optional
    // boolean follows (cloud_fallback_enabled / diag_log_enabled): the parse is
    // a per-key chain, so a missing key touches nothing - a fresh AppConfig
    // stays false, which is exactly the backward-compatible reading.
    {
        AppConfig cfg;
        TEST_CHECK(cfg.FromJsonString("{ \"engine_type\": \"google\" }"),
                   "WS3: a key-absent config parses");
        TEST_CHECK(cfg.back_translation_enabled.load() == false,
                   "WS3: an absent key leaves the compile-time default false");
        // ...and the absent key does NOT overwrite an in-memory value, matching
        // the established per-key chain (nothing to reset, nothing to clobber).
        AppConfig warm;
        warm.back_translation_enabled.store(true);
        TEST_CHECK(warm.FromJsonString("{ \"engine_type\": \"google\" }"),
                   "WS3: a key-absent config parses over an existing value");
        TEST_CHECK(warm.back_translation_enabled.load() == true,
                   "WS3: an absent key does not clobber the in-memory value "
                   "(same per-key-chain convention as the other optional booleans)");
    }

    // A neighbouring toggle must not be disturbed by the new key (the parse is
    // a per-key chain; a mis-ordered else-if would swallow the next key).
    {
        AppConfig cfg;
        TEST_CHECK(cfg.FromJsonString(
                       "{ \"back_translation_enabled\": true, \"auto_send\": true, "
                       "\"sound_enabled\": false }"),
                   "WS3: a config carrying the new key plus its neighbours parses");
        TEST_CHECK(cfg.back_translation_enabled.load() == true &&
                       cfg.auto_send.load() == true &&
                       cfg.sound_enabled.load() == false,
                   "WS3: the new key does not disturb auto_send / sound_enabled");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] WS3 back_translation_enabled config round trip passed." << std::endl;
    } else {
        std::cout << "[FAIL] WS3 back_translation_enabled config round trip: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// (b) ResolveBackTranslationTarget ladder
// ---------------------------------------------------------------------------
void TestWs3BackTranslationTarget() {
    std::cout << "[RUN] Testing WS3 ResolveBackTranslationTarget ladder..." << std::endl;
    const int failures_before = g_failed_count;

    // Tier 1: a PINNED typing source wins over detection and the OS language.
    TEST_CHECK(ResolveBackTranslationTarget("Korean", "Japanese", "English", "Spanish") == "Korean",
               "WS3 ladder T1: the pinned type source wins over detection and OS language");
    // Tier 1 accepts every surface form (code / English name / native name) and
    // canonicalizes to name_en.
    TEST_CHECK(ResolveBackTranslationTarget("ko", "Japanese", "English", "Spanish") == "Korean",
               "WS3 ladder T1: an ISO code resolves to its name_en");
    TEST_CHECK(ResolveBackTranslationTarget("한국어", "Japanese", "English", "Spanish") == "Korean",
               "WS3 ladder T1: a native name resolves to its name_en");

    // Tier 2: AUTO pinned source -> the per-request detection wins.
    TEST_CHECK(ResolveBackTranslationTarget("Auto Detect", "Japanese", "English", "Spanish") ==
                   "Japanese",
               "WS3 ladder T2: AUTO type source -> the detected source wins");
    TEST_CHECK(ResolveBackTranslationTarget("AUTO", "ja", "English", "Spanish") == "Japanese",
               "WS3 ladder T2: an explicit AUTO token is not concrete (case-insensitive)");
    TEST_CHECK(ResolveBackTranslationTarget("", "Japanese", "English", "Spanish") == "Japanese",
               "WS3 ladder T2: an empty type source is not concrete");

    // Tier 3: both sources AUTO -> the OS language wins.
    TEST_CHECK(ResolveBackTranslationTarget("Auto Detect", "Auto Detect", "English", "Spanish") ==
                   "Spanish",
               "WS3 ladder T3: both sources AUTO -> the OS language wins");
    TEST_CHECK(ResolveBackTranslationTarget("Auto Detect", "AUTO", "Japanese", "vi") == "Vietnamese",
               "WS3 ladder T3: the OS language is canonicalized to name_en");

    // Tier 4: everything AUTO -> the ADR-A1-7 neutral pivot.
    //   * forward target != English -> English (the reliable pair side);
    TEST_CHECK(ResolveBackTranslationTarget("Auto Detect", "Auto Detect", "Korean", "Auto Detect") ==
                   "English",
               "WS3 ladder T4: all AUTO + non-English forward target -> English pivot");
    //   * forward target == English and the OS language is English/unknown ->
    //     the forward target itself (nothing better exists; the card then
    //     shows the same text as an explicit "nothing to verify" signal).
    TEST_CHECK(ResolveBackTranslationTarget("Auto Detect", "Auto Detect", "English", "Auto Detect") ==
                   "English",
               "WS3 ladder T4: all AUTO + English forward target -> English (no pivot)");
    TEST_CHECK(ResolveBackTranslationTarget("Auto Detect", "Auto Detect", "English", "en") ==
                   "English",
               "WS3 ladder T4: an English OS language is not a usable pivot");

    // The skip rule at EVERY tier: a candidate equal to the forward target is
    // never returned while a later tier can answer.
    TEST_CHECK(ResolveBackTranslationTarget("English", "Japanese", "English", "Spanish") ==
                   "Japanese",
               "WS3 skip T1: a pinned source equal to the forward target is skipped");
    TEST_CHECK(ResolveBackTranslationTarget("Auto Detect", "English", "English", "Spanish") ==
                   "Spanish",
               "WS3 skip T2: a detected source equal to the forward target is skipped");
    TEST_CHECK(ResolveBackTranslationTarget("Auto Detect", "Auto Detect", "Spanish", "Spanish") ==
                   "English",
               "WS3 skip T3: an OS language equal to the forward target is skipped");
    // ...and the skip is case-insensitive on both sides.
    TEST_CHECK(ResolveBackTranslationTarget("EN", "Japanese", "en", "Spanish") == "Japanese",
               "WS3 skip: the comparison is case-insensitive (EN vs en)");

    // Every tier skipping leaves tier 4: the ladder never returns the forward
    // target when a pivot exists.
    TEST_CHECK(ResolveBackTranslationTarget("Auto Detect", "Auto Detect", "Korean", "Korean") ==
                   "English",
               "WS3 skip: all candidates equal to the forward target -> the pivot answers");

    // The result is always the name_en spelling (the stored form every
    // language surface uses), never a raw code.
    {
        const std::string r = ResolveBackTranslationTarget("ZH-CN", "Auto Detect", "English", "ko");
        TEST_CHECK(r == "Chinese Simplified",
                   "WS3: the result is the registry name_en, not a raw ISO code");
        TEST_CHECK(NormalizeLanguageCode(r) == "ZH-CN",
                   "WS3: the returned name_en round-trips back to the same language");
    }

    // Garbage in every slot must not crash or return an empty string (the card
    // renders this value verbatim).
    {
        const std::string r =
            ResolveBackTranslationTarget("???", "!!!", "not-a-language", "###");
        TEST_CHECK(!r.empty(), "WS3: unresolvable input still yields a usable language name");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] WS3 ResolveBackTranslationTarget ladder passed." << std::endl;
    } else {
        std::cout << "[FAIL] WS3 ResolveBackTranslationTarget ladder: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// (c) i18n: the four keys, all 37 locales, ordering invariant
// ---------------------------------------------------------------------------
void TestWs3BackTranslationI18n() {
    std::cout << "[RUN] Testing WS3 back-translation i18n keys..." << std::endl;
    const int failures_before = g_failed_count;
    const UiLocale initial = I18n::GetCurrentLocale();

    const StringId kNewIds[] = {
        StringId::MenuBackTranslation,
        StringId::BackTranslateHeader,
        StringId::BackTranslateMatchExact,
        StringId::BackTranslateMatchCheck,
        // 261108_0002 (FIX-WS3B, v0.10.6): the card's reference-line label,
        // which used to be a hardcoded Korean literal in tooltip.cpp.
        StringId::BackTranslateReference,
    };

    // Ordering invariant: the four ids were appended AFTER the theme block and
    // BEFORE the sentinel, so the EnumCount completeness loop covers them. The
    // values ride through volatile ints so the comparisons stay genuine runtime
    // checks (a bare constant expression trips C4127 under /W4).
    volatile int first_new = static_cast<int>(StringId::MenuBackTranslation);
    volatile int last_new = static_cast<int>(StringId::BackTranslateReference);
    volatile int theme_tail = static_cast<int>(StringId::ThemeMonolith);
    volatile int sentinel = static_cast<int>(StringId::EnumCount);
    TEST_CHECK(first_new < sentinel && first_new > theme_tail,
               "WS3 i18n: MenuBackTranslation sits between ThemeMonolith and EnumCount");
    TEST_CHECK(last_new == sentinel - 1,
               "WS3 i18n: BackTranslateReference is the last id before EnumCount");
    TEST_CHECK(last_new - first_new == 4,
               "WS3 i18n: the five new ids are contiguous");

    // Korean literal pins (the user-facing strings the design names verbatim).
    I18n::SetLocale(UiLocale::Korean);
    TEST_CHECK(I18n::Get(StringId::MenuBackTranslation) == L"역번역 플로팅 표시 (번역 검증)",
               "WS3 i18n ko: MenuBackTranslation literal");
    TEST_CHECK(I18n::Get(StringId::BackTranslateHeader) == L"🔄 역번역 확인",
               "WS3 i18n ko: BackTranslateHeader literal");
    TEST_CHECK(I18n::Get(StringId::BackTranslateMatchExact) == L"✓ 원문 일치",
               "WS3 i18n ko: BackTranslateMatchExact literal");
    TEST_CHECK(I18n::Get(StringId::BackTranslateMatchCheck) == L"⚡ 표현 확인",
               "WS3 i18n ko: BackTranslateMatchCheck literal");
    TEST_CHECK(I18n::Get(StringId::BackTranslateReference) == L"↳ 원문: ",
               "WS3 i18n ko: BackTranslateReference literal (FIX-WS3B)");

    // English literals (the struct defaults; the card's fallback language).
    I18n::SetLocale(UiLocale::English);
    TEST_CHECK(I18n::Get(StringId::MenuBackTranslation) == L"Floating Back-Translation",
               "WS3 i18n en: MenuBackTranslation literal");
    TEST_CHECK(I18n::Get(StringId::BackTranslateHeader) == L"🔄 Back-Translation",
               "WS3 i18n en: BackTranslateHeader literal");
    TEST_CHECK(I18n::Get(StringId::BackTranslateMatchExact) == L"✓ Meaning Matched",
               "WS3 i18n en: BackTranslateMatchExact literal");
    TEST_CHECK(I18n::Get(StringId::BackTranslateMatchCheck) == L"⚡ Check Nuance",
               "WS3 i18n en: BackTranslateMatchCheck literal");
    TEST_CHECK(I18n::Get(StringId::BackTranslateReference) == L"↳ Original: ",
               "WS3 i18n en: BackTranslateReference literal (FIX-WS3B)");

    // All 37 selectable locales: every new id non-empty. The generic
    // EnumCount loop in TestR6P5P6I18n covers this too; the per-key count here
    // names the failing key instead of collapsing to one counter.
    const auto& locales = GetSupportedUiLocales();
    TEST_CHECK(locales.size() == 37,
               "WS3 i18n: 37 selectable locales cover the four new keys");
    for (StringId id : kNewIds) {
        int non_empty = 0;
        for (const auto& entry : locales) {
            I18n::SetLocale(entry.locale);
            if (!I18n::Get(id).empty()) ++non_empty;
        }
        TEST_CHECK(non_empty == static_cast<int>(locales.size()),
                   "WS3 i18n: the new StringId is non-empty in all 37 locales");
    }

    // Structural pins on src/i18n.cpp: the trailing-field pattern the theme
    // batch established (defaults in the struct, designators in the 36
    // non-English tables, positional values in kStringsEnglish).
    const std::string i18n_src = Ws3ReadSrc("src/i18n.cpp");
    if (i18n_src.empty()) {
        std::cout << "[SKIP] src/i18n.cpp not resolvable from the test CWD; "
                     "WS3 i18n structural pins skipped." << std::endl;
    } else {
        TEST_CHECK(i18n_src.find("const wchar_t* menu_back_translation = L\"Floating Back-Translation\";") !=
                       std::string::npos,
                   "WS3 i18n: LocalizedStrings declares the trailing defaulted field");
        TEST_CHECK(Ws3CountOcc(i18n_src, ".menu_back_translation = ") == 36,
                   "WS3 i18n: 36 non-English tables name .menu_back_translation");
        TEST_CHECK(Ws3CountOcc(i18n_src, ".back_translate_header = ") == 36 &&
                       Ws3CountOcc(i18n_src, ".back_translate_match_exact = ") == 36 &&
                       Ws3CountOcc(i18n_src, ".back_translate_match_check = ") == 36,
                   "WS3 i18n: the other three designators appear in the same 36 tables");
        // 261108_0002 (FIX-WS3B): the fifth field follows the same pattern.
        TEST_CHECK(i18n_src.find("const wchar_t* back_translate_reference = L\"↳ Original: \";") !=
                       std::string::npos,
                   "WS3 i18n: LocalizedStrings declares the back_translate_reference default");
        TEST_CHECK(Ws3CountOcc(i18n_src, ".back_translate_reference = ") == 36,
                   "WS3 i18n: 36 non-English tables name .back_translate_reference");
        TEST_CHECK(i18n_src.find("case StringId::BackTranslateReference:") != std::string::npos,
                   "WS3 i18n: I18n::Get maps BackTranslateReference");
        TEST_CHECK(i18n_src.find("case StringId::MenuBackTranslation:") != std::string::npos &&
                       i18n_src.find("case StringId::BackTranslateHeader:") != std::string::npos &&
                       i18n_src.find("case StringId::BackTranslateMatchExact:") != std::string::npos &&
                       i18n_src.find("case StringId::BackTranslateMatchCheck:") != std::string::npos,
                   "WS3 i18n: I18n::Get maps all four new ids");
        // The X-macro list and its generated count must stay untouched: the
        // new fields are TRAILING defaults, not X-macro fields.
        TEST_CHECK(i18n_src.find("X(menu_back_translation)") == std::string::npos,
                   "WS3 i18n: the new fields are NOT X-macro fields (count stays 157)");
        TEST_CHECK(i18n_src.find("static_assert(kLocalizedStringsFieldCount == 157,") !=
                       std::string::npos,
                   "WS3 i18n: kLocalizedStringsFieldCount is still pinned at 157");
    }

    I18n::SetLocale(initial); // suite-state hygiene

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] WS3 back-translation i18n keys passed." << std::endl;
    } else {
        std::cout << "[FAIL] WS3 back-translation i18n keys: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// (d) tray wiring
// ---------------------------------------------------------------------------
void TestWs3BackTranslationTray() {
    std::cout << "[RUN] Testing WS3 back-translation tray wiring..." << std::endl;
    const int failures_before = g_failed_count;

    // Runtime half: the setter stores the check state (the menu itself needs a
    // live HWND, so the AppendMenuW call is pinned structurally below).
    {
        SystemTray tray;
        tray.SetBackTranslationEnabled(true);
        tray.SetBackTranslationEnabled(false);
        // 261108_0002 (FIX-R1, v0.10.6): this used to be TEST_CHECK(true, ...),
        // which can never fail and so asserted nothing. The contract the line
        // intended to document is "the setter is SAFE on a non-Created tray and
        // only stores the flag": this tray object never ran Create(), so any
        // HWND/Shell_NotifyIcon touch in the setter would fault at runtime. With
        // no accessor by design (the theme/ui-language setters have none
        // either), the body is pinned instead - the compile+link proof is the
        // two calls above, and the body must be a plain state store.
        const std::string tray_src = Ws3ReadSrc("src/ui/tray.cpp");
        if (tray_src.empty()) {
            std::cout << "[SKIP] src/ui/tray.cpp not resolvable from the test CWD; "
                         "setter-body pin skipped." << std::endl;
        } else {
            const std::string setter_sig =
                "void SystemTray::SetBackTranslationEnabled(bool enabled) {";
            const size_t setter_at = tray_src.find(setter_sig);
            TEST_CHECK(setter_at != std::string::npos,
                       "WS3 tray: SetBackTranslationEnabled is defined in tray.cpp");
            if (setter_at != std::string::npos) {
                const size_t setter_end = tray_src.find("\n}", setter_at);
                const std::string body = tray_src.substr(
                    setter_at, setter_end == std::string::npos ? std::string::npos
                                                               : setter_end - setter_at);
                TEST_CHECK(body.find("back_translation_enabled_ = enabled;") != std::string::npos,
                           "WS3 tray: the setter stores the check state");
                TEST_CHECK(body.find("Shell_NotifyIconW") == std::string::npos &&
                               body.find("nid_") == std::string::npos &&
                               body.find("AppendMenu") == std::string::npos,
                           "WS3 tray: the setter does no HWND/shell work, so it is safe on a "
                           "non-Created tray (the calls above ran on one)");
            }
        }
    }

    const std::string tray_hpp = Ws3ReadSrc("src/ui/tray.hpp");
    const std::string tray_cpp = Ws3ReadSrc("src/ui/tray.cpp");
    if (tray_hpp.empty() || tray_cpp.empty()) {
        std::cout << "[SKIP] src/ui/tray.{hpp,cpp} not resolvable from the test CWD; "
                     "WS3 tray structural pins skipped." << std::endl;
    } else {
        // ---- header: callback + setter + state ----
        TEST_CHECK(tray_hpp.find("std::function<void()> on_toggle_back_translate;") !=
                       std::string::npos,
                   "WS3 tray: Callbacks carries on_toggle_back_translate");
        TEST_CHECK(tray_hpp.find("void SetBackTranslationEnabled(bool enabled);") !=
                       std::string::npos,
                   "WS3 tray: SystemTray exposes SetBackTranslationEnabled");
        TEST_CHECK(tray_hpp.find("bool back_translation_enabled_ = false;") !=
                       std::string::npos,
                   "WS3 tray: the check state defaults to false");

        // ---- cpp: id band, no renumbering, menu row, dispatch ----
        TEST_CHECK(tray_cpp.find("ID_TRAY_BACK_TRANSLATE = 2035,") != std::string::npos,
                   "WS3 tray: ID_TRAY_BACK_TRANSLATE is 2035");
        // The pre-existing ids keep their values (no renumbering).
        TEST_CHECK(tray_cpp.find("ID_TRAY_AUTOSEND = 2030,") != std::string::npos &&
                       tray_cpp.find("ID_TRAY_SOUND = 2040,") != std::string::npos &&
                       tray_cpp.find("ID_TRAY_BADGE = 2050,") != std::string::npos &&
                       tray_cpp.find("ID_TRAY_SWAP = 2020,") != std::string::npos,
                   "WS3 tray: the neighbouring ids keep their pre-WS3 values");
        // The menu row sits directly BELOW the auto-send row and carries the
        // check state + the new i18n label.
        const std::size_t autosend_row = tray_cpp.find("ID_TRAY_AUTOSEND, I18n::Get(StringId::MenuAutoSend)");
        const std::size_t back_row = tray_cpp.find("ID_TRAY_BACK_TRANSLATE, I18n::Get(StringId::MenuBackTranslation)");
        TEST_CHECK(autosend_row != std::string::npos && back_row != std::string::npos,
                   "WS3 tray: both toggle rows are appended");
        TEST_CHECK(autosend_row != std::string::npos && back_row != std::string::npos &&
                       autosend_row < back_row,
                   "WS3 tray: the back-translation row is appended BELOW the auto-send row");
        TEST_CHECK(tray_cpp.find("back_translation_enabled_ ? MF_CHECKED : MF_UNCHECKED") !=
                       std::string::npos,
                   "WS3 tray: the row renders the MF_CHECKED/MF_UNCHECKED state");
        // Dispatch: the command reaches the callback.
        TEST_CHECK(tray_cpp.find("cmd == ID_TRAY_BACK_TRANSLATE && callbacks_.on_toggle_back_translate") !=
                       std::string::npos,
                   "WS3 tray: the command dispatch calls on_toggle_back_translate");
        TEST_CHECK(tray_cpp.find("callbacks_.on_toggle_back_translate();") != std::string::npos,
                   "WS3 tray: the dispatch body invokes the callback");
        // The setter body stores the value.
        TEST_CHECK(tray_cpp.find("void SystemTray::SetBackTranslationEnabled(bool enabled)") !=
                       std::string::npos &&
                       tray_cpp.find("back_translation_enabled_ = enabled;") != std::string::npos,
                   "WS3 tray: SetBackTranslationEnabled stores the state");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] WS3 back-translation tray wiring passed." << std::endl;
    } else {
        std::cout << "[FAIL] WS3 back-translation tray wiring: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// Orchestrator entry point (wired from tests/run_tests.cpp).
void TestWs3ConfigI18n() {
    TestWs3BackTranslationConfig();
    TestWs3BackTranslationTarget();
    TestWs3BackTranslationI18n();
    TestWs3BackTranslationTray();
}
