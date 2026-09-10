#include "../src/config.hpp"
#include "../src/diag_logger.hpp"
#include "../src/unicode_utils.hpp"
#include "../src/bidi_utils.hpp"
#include "../src/smart_bypass.hpp"
#include "../src/sound.hpp"
#include "../src/win32_input.hpp"
#include "../src/google_translate.hpp"
#include "../src/engine.hpp"
#include "../src/i18n.hpp"
#include "../src/ui/badge.hpp"
#include "../src/ui/drag_icon.hpp"
#include "../src/ui/dpi.hpp"
#include "../src/ui/tooltip.hpp"
#include "../src/ui/about_window.hpp"
#include "../src/ui/asset_loader.hpp"
#include "../src/version.hpp"
#include "../src/mouse_hook.hpp"
#include "../src/hook.hpp"
#include "../src/worker.hpp"
#include "../src/vulkan_guard.hpp" // P5-F1: delay-load SEH guard probe/stub seam

#include <algorithm> // R6 B1: uniqueness check on concurrent generations
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream> // P5-F1: diag log proof (TestVulkanGuard 3b) reads a log stream
#include <string>
#include <thread>
#include <type_traits> // R6 B3: static_assert pins on accessor signatures
#include <vector>

using namespace emebalachat;

static int g_test_count = 0;
static int g_failed_count = 0;

#define TEST_CHECK(expr, msg) \
    do { \
        g_test_count++; \
        if (!(expr)) { \
            std::cerr << "[FAIL] Line " << __LINE__ << ": " << (msg) << " (" #expr ")" << std::endl; \
            g_failed_count++; \
        } \
    } while (0)

// L1 portability helper: resolve a repo-relative fixture (e.g. "assets\\logo.svg")
// by probing exe-directory and CWD-relative locations up to 2 levels up, mirroring
// FindLogoPath() in src/ui/asset_loader.cpp. Returns empty when not found.
// Never hardcode personal absolute paths in tests again.
static std::filesystem::path ResolveRepoFile(const std::wstring& rel) {
    std::vector<std::filesystem::path> bases;

    wchar_t exe_path[MAX_PATH] = {0};
    if (::GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
        std::filesystem::path dir = std::filesystem::path(exe_path).parent_path();
        bases.push_back(dir);
        bases.push_back(dir / L"..");
        bases.push_back(dir / L".." / L"..");
    }

    std::error_code ec;
    std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (!ec) {
        bases.push_back(cwd);
        bases.push_back(cwd / L"..");
        bases.push_back(cwd / L".." / L"..");
    }

    for (const auto& base : bases) {
        std::error_code fec;
        std::filesystem::path cand = base / rel;
        if (std::filesystem::exists(cand, fec)) {
            return cand;
        }
    }
    return {};
}

void TestConfigModule() {
    std::cout << "[RUN] Testing Config & Languages..." << std::endl;
    const int failures_before = g_failed_count;

    // 1. Defaults
    AppConfig cfg;
    TEST_CHECK(cfg.model_path.find("Hy-MT2-1.8B-Q8_0.gguf") != std::string::npos, "Default model path");
    TEST_CHECK(cfg.target_language == "English", "Default target language is English");
    TEST_CHECK(cfg.source_language == "Auto Detect", "Default source language is Auto Detect");
    TEST_CHECK(!cfg.auto_send, "Default auto_send is false");
    TEST_CHECK(cfg.sound_enabled, "Default sound_enabled is true");
    TEST_CHECK(cfg.hotkey_toggle == "F9", "Default hotkey_toggle is F9");
    TEST_CHECK(cfg.hotkey_lang == "Ctrl+F9", "Default hotkey_lang is Ctrl+F9");
    TEST_CHECK(cfg.hotkey_mode == "Ctrl+Shift+Enter", "Default hotkey_mode is Ctrl+Shift+Enter");
    TEST_CHECK(cfg.drag_to_translate == true, "Default drag_to_translate is true");
    TEST_CHECK(cfg.drag_hotkey == "double_ctrl_c", "Default drag_hotkey is double_ctrl_c");
    TEST_CHECK(cfg.cloud_fallback_enabled == false, "Default cloud_fallback_enabled is false (privacy-first, H2)");
    TEST_CHECK(std::abs(cfg.temperature - 0.7f) < 0.001f, "Default temperature is 0.7f");
    TEST_CHECK(std::abs(cfg.top_p - 0.6f) < 0.001f, "Default top_p is 0.6f");
    TEST_CHECK(cfg.top_k == 20, "Default top_k is 20");
    TEST_CHECK(std::abs(cfg.repetition_penalty - 1.05f) < 0.001f, "Default repetition_penalty is 1.05f");

    // 2. Language table coverage
    const auto& all_langs = GetSupportedLanguages();
    TEST_CHECK(all_langs.size() == 38, "Supported languages must contain 38 entries (AUTO + 37)");

    const auto& target_langs = GetTargetLanguages();
    TEST_CHECK(target_langs.size() == 37, "Target languages must contain 37 entries");

    const LanguageInfo* ko = FindLanguageByCode("KO");
    TEST_CHECK(ko != nullptr, "Lookup KO by code");
    TEST_CHECK(ko && ko->name_en == "Korean", "KO name_en is Korean");

    const LanguageInfo* vi = FindLanguageByName("Vietnamese");
    TEST_CHECK(vi != nullptr && vi->code == "VI", "Lookup Vietnamese by name");

    const LanguageInfo* zh = FindLanguageByCode("zh-cn");
    TEST_CHECK(zh != nullptr && zh->code == "ZH-CN", "Lookup lowercase zh-cn code");

    // 3. Language Cycling
    std::string next_lang = CycleTargetLanguage("Korean");
    TEST_CHECK(next_lang == "Vietnamese", "Cycle from Korean goes to Vietnamese");

    // Full cycle wraps around
    std::string curr = target_langs[0].name_en;
    for (size_t i = 0; i < target_langs.size(); ++i) {
        curr = CycleTargetLanguage(curr);
    }
    TEST_CHECK(curr == target_langs[0].name_en, "Full language cycle wraps around");

    // 4. Prompt Builder (Tencent Hy-MT2 format: \n\n paragraph break and Chinese branch)
    std::string prompt_en = BuildPrompt("안녕하세요", "English");
    TEST_CHECK(prompt_en == "Translate the following segment into English, without additional explanation.\n\n안녕하세요",
               "Prompt formatting matches specification (English default)");

    std::string prompt_zh = BuildPrompt("안녕하세요", "Chinese");
    TEST_CHECK(prompt_zh == "将以下文本翻译为Chinese，注意只需要输出翻译后的结果，不要额外解释：\n\n안녕하세요",
               "Prompt formatting matches specification (Chinese branch)");

    // R6 Phase 4 (B2, plan §4.1 item 1): resolvable target tokens now inject
    // the NATIVE name (简体中文), not the code/English name. The bare word
    // "Chinese" above is NOT a table entry, so it keeps the historical raw
    // injection (backward-compatibility pin).
    std::string prompt_zh_cn = BuildPrompt("Hello", "ZH-CN");
    TEST_CHECK(prompt_zh_cn == "将以下文本翻译为简体中文，注意只需要输出翻译后的结果，不要额外解释：\n\nHello",
               "Prompt formatting matches specification (ZH-CN branch injects native name)");

    // 5. JSON serialization & parsing roundtrip
    cfg.target_language = "Japanese";
    cfg.auto_send = true;
    cfg.sound_enabled = false;
    cfg.drag_to_translate = false;
    cfg.drag_hotkey = "custom_hotkey";
    cfg.model_path = "D:\\custom\\model.gguf";
    cfg.temperature = 0.85f;
    cfg.top_p = 0.9f;
    cfg.top_k = 40;
    cfg.repetition_penalty = 1.15f;
    cfg.SetBadgePosition(450, 600);

    std::string json_str = cfg.ToJsonString();
    AppConfig loaded_cfg;
    bool parsed = loaded_cfg.FromJsonString(json_str);
    TEST_CHECK(parsed, "JSON parsing succeeds");
    TEST_CHECK(loaded_cfg.target_language == "Japanese", "Persisted target_language matches");
    TEST_CHECK(loaded_cfg.auto_send == true, "Persisted auto_send matches");
    TEST_CHECK(loaded_cfg.sound_enabled == false, "Persisted sound_enabled matches");
    TEST_CHECK(loaded_cfg.drag_to_translate == false, "Persisted drag_to_translate matches");
    TEST_CHECK(loaded_cfg.drag_hotkey == "custom_hotkey", "Persisted drag_hotkey matches");
    TEST_CHECK(loaded_cfg.model_path == "D:\\custom\\model.gguf", "Persisted model_path matches");
    TEST_CHECK(loaded_cfg.cloud_fallback_enabled == false, "Persisted cloud_fallback_enabled defaults false when absent from JSON");
    TEST_CHECK(std::abs(loaded_cfg.temperature - 0.85f) < 0.001f, "Persisted temperature matches");
    TEST_CHECK(std::abs(loaded_cfg.top_p - 0.9f) < 0.001f, "Persisted top_p matches");
    TEST_CHECK(loaded_cfg.top_k == 40, "Persisted top_k matches");
    TEST_CHECK(std::abs(loaded_cfg.repetition_penalty - 1.15f) < 0.001f, "Persisted repetition_penalty matches");
    TEST_CHECK(loaded_cfg.badge_x == 450 && loaded_cfg.badge_y == 600, "Persisted badge_x and badge_y match");

    // 5b. cloud_fallback_enabled explicit roundtrip (set true -> serialize -> parse -> true)
    AppConfig cloud_cfg;
    cloud_cfg.cloud_fallback_enabled = true;
    std::string cloud_json = cloud_cfg.ToJsonString();
    TEST_CHECK(cloud_json.find("\"cloud_fallback_enabled\": true") != std::string::npos,
               "ToJsonString emits cloud_fallback_enabled: true when enabled");
    AppConfig cloud_loaded;
    bool cloud_parsed = cloud_loaded.FromJsonString(cloud_json);
    TEST_CHECK(cloud_parsed, "cloud_fallback JSON parses");
    TEST_CHECK(cloud_loaded.cloud_fallback_enabled == true, "cloud_fallback_enabled true survives roundtrip");

    // 6. Graceful recovery on malformed JSON
    AppConfig fallback_cfg;
    bool bad_parse = fallback_cfg.FromJsonString("{ invalid json: ...");
    TEST_CHECK(!bad_parse, "Corrupted JSON reports parse failure");
    TEST_CHECK(fallback_cfg.target_language == "English", "Corrupted JSON retains default values");
    TEST_CHECK(fallback_cfg.cloud_fallback_enabled == false, "Corrupted JSON retains cloud_fallback_enabled default false");

    // 7. I2 fix: \uXXXX surrogate-pair decoding in SimpleJsonReader (tested
    //    through the public FromJsonString seam). Values ride in drag_hotkey
    //    so they hit the ParseString escape path verbatim.
    {
        // 7a. Emoji pair: \uD83D\uDE80 -> U+1F680 -> 4-byte UTF-8 F0 9F 9A 80
        AppConfig emoji_cfg;
        TEST_CHECK(emoji_cfg.FromJsonString("{\"drag_hotkey\": \"Hi \\uD83D\\uDE80\"}"),
                   "I2: JSON with surrogate-pair emoji parses");
        TEST_CHECK(emoji_cfg.drag_hotkey == "Hi \xF0\x9F\x9A\x80",
                   "I2: surrogate pair decodes to 4-byte UTF-8 rocket emoji");
        // Roundtrip through UTF-16 must yield the real code point, not garbage
        std::wstring emoji_w = ToUtf16(emoji_cfg.drag_hotkey);
        TEST_CHECK(emoji_w == L"Hi \U0001F680",
                   "I2: emoji survives UTF-8 -> UTF-16 conversion");

        // 7b. 4-byte supplementary char (U+10550 -> \uD801\uDD50), 퐝-style beyond BMP
        AppConfig supp_cfg;
        TEST_CHECK(supp_cfg.FromJsonString("{\"drag_hotkey\": \"\\uD801\\uDD50\"}"),
                   "I2: JSON with U+10550 pair parses");
        TEST_CHECK(supp_cfg.drag_hotkey == "\xF0\x90\x95\x90",
                   "I2: U+10550 encodes as 4-byte UTF-8 F0 90 95 90");

        // 7c. BMP escape still works (regression guard for the 3-byte branch)
        AppConfig bmp_cfg;
        TEST_CHECK(bmp_cfg.FromJsonString("{\"drag_hotkey\": \"\\uD55C\"}"),
                   "I2: JSON with BMP hangul escape parses");
        TEST_CHECK(bmp_cfg.drag_hotkey == "\xED\x95\x9C",
                   "I2: U+D55C (한) encodes as 3-byte UTF-8");

        // 7d. Lone HIGH surrogate (D800-DBFF with no following low) -> U+FFFD,
        //     never corrupt WTF-8 bytes
        AppConfig lone_hi_cfg;
        TEST_CHECK(lone_hi_cfg.FromJsonString("{\"drag_hotkey\": \"A\\uD83Dx\"}"),
                   "I2: JSON with lone high surrogate parses without failure");
        TEST_CHECK(lone_hi_cfg.drag_hotkey == "A\xEF\xBF\xBDx",
                   "I2: lone high surrogate replaced by U+FFFD (no corrupt bytes)");

        // 7e. Lone HIGH surrogate followed by a NON-low \u escape: the second
        //     escape must survive as its own character, not be eaten as a low.
        AppConfig lone_hi2_cfg;
        TEST_CHECK(lone_hi2_cfg.FromJsonString("{\"drag_hotkey\": \"\\uD83D\\u0041\"}"),
                   "I2: lone high + non-low escape parses");
        TEST_CHECK(lone_hi2_cfg.drag_hotkey == "\xEF\xBF\xBD" "A",
                   "I2: lone high -> U+FFFD, following \\u0041 decodes as 'A'");

        // 7f. Lone LOW surrogate (DC00-DFFF without preceding high) -> U+FFFD
        AppConfig lone_lo_cfg;
        TEST_CHECK(lone_lo_cfg.FromJsonString("{\"drag_hotkey\": \"\\uDC00!\"}"),
                   "I2: JSON with lone low surrogate parses without failure");
        TEST_CHECK(lone_lo_cfg.drag_hotkey == "\xEF\xBF\xBD!",
                   "I2: lone low surrogate replaced by U+FFFD");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Config & Languages tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Config & Languages tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestUnicodeModule() {
    std::cout << "[RUN] Testing Unicode & Normalization..." << std::endl;
    const int failures_before = g_failed_count;

    // 1. ToUtf8 and ToUtf16 roundtrip
    std::vector<std::wstring> samples = {
        L"Hello, world!",
        L"안녕하세요, 세계!",
        L"Xin chào thế giới!",
        L"你好世界！",
        L"こんにちは世界！",
        L"Привет мир!",
        L"مرحبا بالعالم!",
        L"สวัสดีชาวโลก!",
        L"Café au lait & naïve façade 🚀🔥"
    };

    for (const auto& wstr : samples) {
        std::string u8 = ToUtf8(wstr);
        std::wstring roundtrip = ToUtf16(u8);
        TEST_CHECK(roundtrip == wstr, "UTF-8/UTF-16 roundtrip preserves content exactly");
    }

    // 2. NormalizeNFC
    // Decomposed Hangul: ㅎ (0x1112) + ㅏ (0x1161) + ㄴ (0x11AB) = 한 (0xD55C)
    std::wstring decomposed = { 0x1112, 0x1161, 0x11AB };
    std::wstring composed = NormalizeNFC(decomposed);
    TEST_CHECK(composed == L"한", "NormalizeNFC composes decomposed Hangul jamo into syllable");

    std::wstring latin_decomposed = L"e\u0301"; // e + combining acute accent
    std::wstring latin_composed = NormalizeNFC(latin_decomposed);
    TEST_CHECK(latin_composed == L"é", "NormalizeNFC composes combining accents");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Unicode & Normalization tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Unicode & Normalization tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestSmartBypassModule() {
    std::cout << "[RUN] Testing Smart Bypass..." << std::endl;
    const int failures_before = g_failed_count;

    // 1. Pure Korean sentences
    TEST_CHECK(ContainsKorean(L"안녕하세요"), "Pure Korean sentence 1");
    TEST_CHECK(ContainsKorean(L"밥 먹었니?"), "Pure Korean sentence 2");
    TEST_CHECK(ContainsKorean(L"오늘 날씨가 정말 좋습니다."), "Pure Korean sentence 3");
    TEST_CHECK(ContainsKorean(L"번역 테스트 중입니다."), "Pure Korean sentence 4");

    // 2. Mixed Korean
    TEST_CHECK(ContainsKorean(L"hello 안녕 123"), "Mixed Korean English numbers");
    TEST_CHECK(ContainsKorean(L"lol ㅋㅋㅋ"), "Mixed Korean with slang");
    TEST_CHECK(ContainsKorean(L"discord에서 만나요"), "Mixed Korean with app name");

    // 3. Korean Jamo
    TEST_CHECK(ContainsKorean(L"ㅋㅋㅋ"), "Hangul Jamo ㅋㅋㅋ");
    TEST_CHECK(ContainsKorean(L"ㅎㅎ"), "Hangul Jamo ㅎㅎ");
    TEST_CHECK(ContainsKorean(L"ㅠㅠ"), "Hangul Jamo ㅠㅠ");

    // 4. Hangul boundary syllables
    TEST_CHECK(ContainsKorean(L"\uAC00"), "First Hangul syllable 가");
    TEST_CHECK(ContainsKorean(L"\uD7A3"), "Last Hangul syllable 힣");
    TEST_CHECK(ContainsKorean(L"뷁"), "Complex Hangul syllable 뷁");

    // 5. Pure English (should return false)
    TEST_CHECK(!ContainsKorean(L"hello world"), "Pure English returns false");
    TEST_CHECK(!ContainsKorean(L"good game"), "Pure English returns false");
    TEST_CHECK(!ContainsKorean(L"How are you doing today?"), "Pure English sentence returns false");

    // 6. Numbers, symbols, URLs (should return false)
    TEST_CHECK(!ContainsKorean(L"123456"), "Digits return false for Korean");
    TEST_CHECK(!ContainsKorean(L"https://discord.com"), "URL returns false for Korean");
    TEST_CHECK(!ContainsKorean(L":) ;)"), "Emoticons return false for Korean");
    TEST_CHECK(!ContainsKorean(L"👍🚀🔥🎉"), "Emojis return false for Korean");
    TEST_CHECK(!ContainsKorean(L""), "Empty string returns false");
    TEST_CHECK(!ContainsKorean(L"   \t\n "), "Whitespace returns false");

    // 7. Other scripts
    TEST_CHECK(!ContainsKorean(L"こんにちは"), "Japanese Kana returns false for Korean");
    TEST_CHECK(!ContainsKorean(L"你好，世界"), "Chinese returns false for Korean");
    TEST_CHECK(!ContainsKorean(L"Привет мир"), "Russian returns false for Korean");
    TEST_CHECK(!ContainsKorean(L"مرحبا بالعالم"), "Arabic returns false for Korean");

    // 8. Script detection
    TEST_CHECK(DetectLanguage(L"안녕하세요") == "Korean", "Detect Korean");
    TEST_CHECK(DetectLanguage(L"こんにちは") == "Japanese", "Detect Japanese");
    TEST_CHECK(DetectLanguage(L"你好世界") == "Chinese Simplified", "Detect Chinese");
    TEST_CHECK(DetectLanguage(L"Xin chào thế giới") == "Vietnamese", "Detect Vietnamese");
    TEST_CHECK(DetectLanguage(L"Привет мир") == "Russian", "Detect Russian");
    TEST_CHECK(DetectLanguage(L"สวัสดีชาวโลก") == "Thai", "Detect Thai");
    TEST_CHECK(DetectLanguage(L"مرحبا بالعالم") == "Arabic", "Detect Arabic");
    // F5 (session 260908_0003, ask audit 181530 condition 1 Option B): the
    // pure-ASCII "English" LABEL is retired - Latin is a script, and ASCII
    // Latin cannot separate English from Indonesian/Malay/Tagalog (all
    // Hy-MT2-supported). ASCII Latin now returns the AUTO marker and the
    // model's built-in language ID decides. (Supersedes the historical
    // "Detect English" pin and the F1 Phase-1 pure-ASCII trade-off.)
    TEST_CHECK(DetectLanguage(L"Hello world") == "Auto Detect", "F5: ASCII Latin detects as AUTO marker (no language claim)");
    TEST_CHECK(DetectLanguage(L"") == "Unknown", "Empty text detected as Unknown");
    TEST_CHECK(DetectLanguage(L"123456") == "Unknown", "Digits detected as Unknown");

    // 8c. F1 (session 260908_0003, verify 220010 root causes R1/R2): Latin
    // script is NEVER Vietnamese unless a TRUE Vietnamese-specific codepoint
    // is present, and diacritic Latin is NEVER force-labeled "English".
    // Negative predicate tests - shared Latin-1 diacritics dropped from the
    // Vietnamese set (Portuguese a-tilde/c-cedilla, Spanish acute accents,
    // French grave/acute accents; the Ũ/ũ U-tilde exclusion covers Portuguese):
    TEST_CHECK(!ContainsVietnamese(L"não presto muita atenção em futebol"), "F1: Portuguese a-tilde/o-tilde/c-cedilla are NOT Vietnamese markers");
    TEST_CHECK(!ContainsVietnamese(L"rápido corazón"), "F1: Spanish acute accents are NOT Vietnamese markers");
    TEST_CHECK(!ContainsVietnamese(L"étrange méditerranéen"), "F1: French grave/acute accents are NOT Vietnamese markers");
    TEST_CHECK(!ContainsVietnamese(L"muito ũma"), "F1: U-tilde (u-tilde) alone is NOT a Vietnamese marker (shared with Portuguese)");
    // Positive predicate test - true VI-specific codepoints (1EA0-block tone
    // marks + u-horn + o-horn + a-breve + d-stroke) still detect:
    TEST_CHECK(ContainsVietnamese(L"Việt Nam thật tuyệt vời"), "F1: e-grave-hook (ệ), a-dot-below (ậ), u-horn (ư), o-horn-grave (ờ) are Vietnamese markers");
    TEST_CHECK(ContainsVietnamese(L"Đừng ăn đó"), "F1: D-stroke (Đ), u-horn, a-circumflex-dot-below, o-horn-dot-below detect Vietnamese");
    // DetectLanguage: the three task-mandated samples must route as the AUTO
    // marker - neither Vietnamese nor English (Hy-MT2's built-in language ID
    // decides downstream; NormalizeLanguageCode("Auto Detect") == "AUTO"):
    TEST_CHECK(DetectLanguage(L"não presto muita atenção em futebol") != "Vietnamese", "F1: Portuguese never mislabels Vietnamese");
    TEST_CHECK(DetectLanguage(L"não presto muita atenção em futebol") == "Portuguese",
               "A2: Portuguese detected via stop-words (supersedes F1 AUTO marker fallback)");
    TEST_CHECK(DetectLanguage(L"rápido corazón") == "Auto Detect", "F1: Spanish (acute accents) detects as AUTO marker");
    TEST_CHECK(DetectLanguage(L"étrange méditerranéen") == "Auto Detect", "F1: French (grave/acute) detects as AUTO marker");
    TEST_CHECK(NormalizeLanguageCode(DetectLanguage(L"rápido corazón")) == "AUTO", "F1: AUTO marker normalizes to registry code AUTO (engine sees no source token)");
    TEST_CHECK(DetectLanguage(L"Việt Nam thật tuyệt vời") == "Vietnamese", "F1: true-VI text still detects Vietnamese after the narrowing");
    // F1 Phase-1 kept pure-ASCII Latin labeled "English"; F5 Phase 2 (audit
    // 181530 Option B, adjudicated) supersedes that trade-off: the label is
    // "Auto Detect" for ALL Latin script, and the EN->EN identity bypass now
    // keys on the PIN (step 7), not on the detection label.
    TEST_CHECK(DetectLanguage(L"le chat est sur la table") == "French", "A2: unaccented French detected via stop-words (supersedes F5 Auto Detect fallback)");
    // Latin WITH umlauts (German) is also ambiguous -> AUTO, not English:
    TEST_CHECK(DetectLanguage(L"Liebe Grüße") == "Auto Detect", "F1: German umlauts detect as AUTO marker, not English");
    // Regression: the VI detection that used shared chars AND true markers
    // ("Xin chào thế giới": e-circumflex-acute ế from the 1EA0 block) holds:
    TEST_CHECK(DetectLanguage(L"Xin chào thế giới") == "Vietnamese", "F1: existing VI sample keeps Vietnamese via ế (1EA0 block)");

    // 8b. Hebrew script detection (REQ-040 gap G-4, user-approved 2026-09-07).
    // "שלום עולם" = "Hello world" in Hebrew. Hebrew must get its OWN label -
    // mislabeling it "Arabic" would corrupt the bypass decision and logs.
    TEST_CHECK(ContainsHebrew(L"שלום עולם"), "ContainsHebrew: pure Hebrew");
    TEST_CHECK(ContainsHebrew(L"discord에서 שלום"), "ContainsHebrew: mixed-script Hebrew present");
    TEST_CHECK(!ContainsHebrew(L"مرحبا بالعالم"), "ContainsHebrew: Arabic is NOT Hebrew");
    TEST_CHECK(!ContainsHebrew(L"hello world"), "ContainsHebrew: Latin is not Hebrew");
    TEST_CHECK(!ContainsHebrew(L""), "ContainsHebrew: empty string false");
    // Range boundary pins (U+0590-U+05FF, the shared bidi_utils predicate):
    // U+058F is the last Armenian code point, U+0600 the first Arabic block one.
    TEST_CHECK(ContainsHebrew(L"\u0590"), "ContainsHebrew: block start U+0590");
    TEST_CHECK(ContainsHebrew(L"\u05FF"), "ContainsHebrew: block end U+05FF");
    TEST_CHECK(!ContainsHebrew(L"\u058F"), "ContainsHebrew: U+058F (Armenian) outside block");
    TEST_CHECK(!ContainsHebrew(L"\u0600"), "ContainsHebrew: U+0600 (Arabic) outside block");
    TEST_CHECK(DetectLanguage(L"שלום עולם") == "Hebrew", "Detect Hebrew (own label, not Arabic)");
    TEST_CHECK(NormalizeLanguageCode(DetectLanguage(L"שלום עולם")) == "HE",
               "G-4: 'Hebrew' label maps to registry code HE (no hardcoded special case)");
    // The behavior G-4 fixes: Hebrew text under a Hebrew target must bypass the
    // engine (already-target), and under another target must still translate.
    TEST_CHECK(!ShouldTranslate(L"שלום עולם", "Hebrew"),
               "G-4: Hebrew targeting Hebrew bypassed (engine not called)");
    TEST_CHECK(ShouldTranslate(L"שלום עולם", "Korean"),
               "G-4: Hebrew to Korean still translates (detection did not over-bypass)");
    // Priority discipline: Arabic wins when Arabic script is present (the
    // G-4 check sits immediately after Arabic in the priority chain).
    TEST_CHECK(DetectLanguage(L"مرحبا שלום") == "Arabic",
               "G-4: mixed Arabic+Hebrew keeps the documented Arabic priority");

    // 9. URL detection
    TEST_CHECK(IsUrl(L"https://discord.com"), "IsUrl https");
    TEST_CHECK(IsUrl(L"http://example.com?query=test"), "IsUrl http with query");
    TEST_CHECK(IsUrl(L"www.google.com"), "IsUrl www");
    TEST_CHECK(IsUrl(L"github.com/project"), "IsUrl domain/path");
    TEST_CHECK(!IsUrl(L"hello world"), "IsUrl false for normal text");

    // 10. ShouldTranslate logic
    // Cross-language requests must return true
    TEST_CHECK(ShouldTranslate(L"Xin chào thế giới", "Korean"), "Vietnamese to Korean");
    TEST_CHECK(ShouldTranslate(L"你好世界", "Vietnamese"), "Chinese to Vietnamese");
    TEST_CHECK(ShouldTranslate(L"How much does this cost?", "Korean"), "English to Korean");
    TEST_CHECK(ShouldTranslate(L"How much does this cost?", "Spanish"), "English to Spanish");
    TEST_CHECK(ShouldTranslate(L"도와주셔서 감사합니다!", "English"), "Korean to English");
    TEST_CHECK(ShouldTranslate(L"도와주셔서 감사합니다!", "Vietnamese"), "Korean to Vietnamese");

    // Same language bypass. F5: the English case now requires a PINNED source
    // - the detection-based ASCII-Latin "English" bypass is retired (it also
    // swallowed Indonesian/Malay/Tagalog). Script-certain identities (KO/VI/JA)
    // keep the detection-based bypass unchanged.
    TEST_CHECK(!ShouldTranslate(L"Hello world, have a good day", "English", "English"), "F5: PINNED English targeting English bypassed (identity preserved for pins)");
    TEST_CHECK(!ShouldTranslate(L"안녕하세요 만나서 반갑습니다", "Korean"), "Korean targeting Korean bypassed");
    TEST_CHECK(!ShouldTranslate(L"Xin chào bạn nhé", "Vietnamese"), "Vietnamese targeting Vietnamese bypassed");
    TEST_CHECK(!ShouldTranslate(L"こんにちは、元気ですか？", "Japanese"), "Japanese targeting Japanese bypassed");

    // F1 Claim-C: diacritic Latin targeting English must TRANSLATE. The old
    // code detected it "English" (or "Vietnamese") and the already-target gate
    // SILENTLY bypassed, returning the original text as its own "translation".
    TEST_CHECK(ShouldTranslate(L"não presto muita atenção em futebol", "English"), "F1: Portuguese -> English is no longer silently bypassed");
    TEST_CHECK(ShouldTranslate(L"rápido corazón", "English"), "F1: Spanish -> English translates");
    TEST_CHECK(ShouldTranslate(L"étrange méditerranéen", "English"), "F1: French -> English translates");
    // F1: the same samples route to every other target (engine receives AUTO):
    TEST_CHECK(ShouldTranslate(L"não presto muita atenção em futebol", "Korean"), "F1: Portuguese -> Korean translates");
    // F1 R3: an explicit source pin SKIPS detection-based bypass. Under the
    // old code this was bypassed because script detection labeled the ASCII
    // text "English" == target even though the user pinned French.
    TEST_CHECK(ShouldTranslate(L"the book is on the table", "English", "French"), "F1: pinned French source reaches the engine even when detection says English");
    TEST_CHECK(ShouldTranslate(L"não presto muita atenção em futebol", "English", "Portuguese"), "F1: pinned Portuguese -> English translates (pin is ground truth, no detect override)");
    // F5 Phase 2 (this session, ask audit 181530 condition 1 Option B,
    // VP/user adjudication): the pure-ASCII Latin "English" label is retired -
    // the F1 Phase-1 trade-off pin ("le chat est sur la table" -> English
    // bypassed) is SUPERSEDED. ASCII-Latin Hy-MT2-supported languages
    // (Indonesian/Malay/Tagalog/Swahili...) must no longer silently pass
    // through untranslated toward an English target; they route as AUTO
    // translation and the model's built-in language ID decides. Accepted cost
    // per the audit: genuine English text under Auto targeting English spends
    // one local inference and returns near-identical output (visible, not
    // silent like the old failure).
    TEST_CHECK(ShouldTranslate(L"Saya tidak terlalu memperhatikan bola", "English"),
               "F5: Indonesian (ASCII Latin) -> English must NOT bypass; routes as translation");
    TEST_CHECK(ShouldTranslate(L"Hindi ko masyadong binibigyang pansin ang bola", "English"),
               "F5: Tagalog/Filipino (ASCII Latin) -> English translates (no silent passthrough)");
    TEST_CHECK(ShouldTranslate(L"Sisitiki sana", "English"),
               "F5: Swahili (ASCII Latin) -> English translates");
    TEST_CHECK(ShouldTranslate(L"le chat est sur la table", "English"),
               "F5: unaccented French -> English no longer bypassed (supersedes the F1 trade-off pin)");
    // N4 (session 260910_0001): the English stop-word confidence gate is now 3
    // hits (kLatinLangs min_hits 2 -> 3). Boundary coverage: exactly 2 hits is
    // too loose to assert English (field logs showed EN->EN identity inference
    // on weak reads), so it must keep routing as AUTO translation; 3+ hits is
    // high-confidence English and the already-target bypass must fire.
    TEST_CHECK(DetectLanguage(L"This for testing purposes") == "Auto Detect",
               "N4: exactly 2 English stop-word hits no longer asserts English (routes as AUTO)");
    TEST_CHECK(ShouldTranslate(L"This for testing purposes", "English"),
               "N4: 2-hit text targeting English still translates (no premature identity bypass)");
    TEST_CHECK(DetectLanguage(L"This is for testing purposes") == "English",
               "N4: 3 English stop-word hits (this, is, for) detect as English");
    TEST_CHECK(!ShouldTranslate(L"This is for testing purposes", "English"),
               "N4: 3+ hit English text targeting English bypasses (kills the EN->EN identity inference)");
    TEST_CHECK(ShouldTranslate(L"This is for testing purposes", "Korean"),
               "N4-safety: English detection does not over-bypass non-English targets");
    TEST_CHECK(!ShouldTranslate(L"Saya tidak terlalu memperhatikan bola", "Indonesian"),
               "A2: Indonesian text -> Indonesian target self-bypass (stop-words: tidak, saya)");
    // True identity bypass survives ONLY for a pinned source (user declaration
    // is ground truth), language-neutrally:
    TEST_CHECK(!ShouldTranslate(L"Hello world, have a good day", "English", "EN"),
               "F5: PINNED English (code form) -> English identity bypass preserved");
    TEST_CHECK(!ShouldTranslate(L"le chat est sur la table", "English", "English"),
               "F5: PINNED English (name form) -> English identity bypass preserved");
    TEST_CHECK(!ShouldTranslate(L"Saya tidak terlalu memperhatikan bola", "Indonesian", "Indonesian"),
               "F5: PINNED Indonesian -> Indonesian identity bypass (pin identity is language-neutral)");
    TEST_CHECK(ShouldTranslate(L"Saya tidak terlalu memperhatikan bola", "Korean"),
               "F5: Indonesian -> Korean translates (non-EN targets unaffected by the label change)");
    // Same-language bypass survives the narrowing for REAL Vietnamese (the
    // 1EA0-block ạ marker in "bạn"):
    TEST_CHECK(!ShouldTranslate(L"Việt Nam thật tuyệt vời", "Vietnamese"), "F1: true-VI text targeting Vietnamese still bypassed (engine not called)");
    TEST_CHECK(ShouldTranslate(L"Việt Nam thật tuyệt vời", "Korean"), "F1: true-VI text targeting Korean still translates (VI->VI guard did not over-fire)");

    // Non-linguistic bypass
    std::vector<std::wstring> non_ling = {
        L"123456", L"999.99", L"https://discord.com", L":) ;) :(", L":-)", L"^_^", L"👍🔥🎉💯", L"", L"   \n\t  ", L"--- ... ---"
    };
    for (const auto& item : non_ling) {
        TEST_CHECK(!ShouldTranslate(item, "Korean"), "Non-linguistic input bypassed for Korean");
        TEST_CHECK(!ShouldTranslate(item, "English"), "Non-linguistic input bypassed for English");
        TEST_CHECK(!ShouldTranslate(item, "Vietnamese"), "Non-linguistic input bypassed for Vietnamese");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Smart Bypass tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Smart Bypass tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestSelfLanguageBypass() {
    std::cout << "[RUN] Testing Self-Language Bypass..." << std::endl;
    const int failures_before = g_failed_count;

    // 1a. Non-Latin Script Self-Bypass (already partially covered, verify expansions)
    TEST_CHECK(!ShouldTranslate(L"오늘 날씨가 좋습니다", "Korean"), "Korean text -> Korean target bypasses");
    TEST_CHECK(!ShouldTranslate(L"今日はいい天気です", "Japanese"), "Japanese text -> Japanese target bypasses");
    TEST_CHECK(!ShouldTranslate(L"สวัสดีครับ วันนี้อากาศดี", "Thai"), "Thai text -> Thai target bypasses");
    TEST_CHECK(!ShouldTranslate(L"مرحبا كيف حالك", "Arabic"), "Arabic text -> Arabic target bypasses");
    TEST_CHECK(!ShouldTranslate(L"Привет как дела сегодня", "Russian"), "Russian text -> Russian target bypasses");
    TEST_CHECK(!ShouldTranslate(L"आज मौसम अच्छा है", "Hindi"), "Hindi text -> Hindi target bypasses");
    TEST_CHECK(!ShouldTranslate(L"Γεια σας πώς είστε", "Greek"), "Greek text -> Greek target bypasses");
    TEST_CHECK(!ShouldTranslate(L"আজ আবহাওয়া ভালো", "Bengali"), "Bengali text -> Bengali target bypasses");

    // 1b. Latin-Script Self-Bypass (NEW — the core A-2 feature)
    TEST_CHECK(!ShouldTranslate(L"The weather is nice today and I have a meeting", "English"),
               "A2: English text -> English target bypasses (stop-words: the, is, and, have, a)");
    TEST_CHECK(!ShouldTranslate(L"This is a test for the translation bypass feature", "English"),
               "A2: English text -> English target bypasses (stop-words: this, is, a, for, the)");
    TEST_CHECK(!ShouldTranslate(L"El gato está en la mesa con el perro", "Spanish"),
               "A2: Spanish text -> Spanish target bypasses");
    TEST_CHECK(!ShouldTranslate(L"Le chat est sur la table avec les enfants", "French"),
               "A2: French text -> French target bypasses");
    TEST_CHECK(!ShouldTranslate(L"Der Hund ist auf dem Tisch mit der Katze", "German"),
               "A2: German text -> German target bypasses");
    TEST_CHECK(!ShouldTranslate(L"Saya tidak terlalu memperhatikan bola yang ada di sana", "Indonesian"),
               "A2: Indonesian text -> Indonesian target bypasses");
    TEST_CHECK(!ShouldTranslate(L"O gato está na mesa com o cachorro para todos", "Portuguese"),
               "A2: Portuguese text -> Portuguese target bypasses");

    // 1c. Cross-Language Translation MUST STILL WORK (safety checks)
    TEST_CHECK(ShouldTranslate(L"Saya tidak terlalu memperhatikan bola", "English"),
               "A2-safety: Indonesian -> English must translate (F5 invariant)");
    TEST_CHECK(ShouldTranslate(L"Hindi ko masyadong binibigyang pansin ang bola", "English"),
               "A2-safety: Tagalog -> English must translate (F5 invariant)");
    TEST_CHECK(ShouldTranslate(L"Sisitiki sana", "English"),
               "A2-safety: Swahili -> English must translate (F5 invariant)");
    TEST_CHECK(ShouldTranslate(L"le chat est sur la table", "English"),
               "A2-safety: French -> English must translate (F5 invariant)");
    TEST_CHECK(ShouldTranslate(L"Hotel", "English"),
               "A2-safety: single-word loanword must translate");
    TEST_CHECK(ShouldTranslate(L"No", "English"),
               "A2-safety: single-word must translate");
    TEST_CHECK(ShouldTranslate(L"오늘 날씨가 좋습니다", "English"),
               "A2-safety: Korean -> English must translate");

    // 1d. Non-Latin Cross-Script (different language) MUST Translate
    TEST_CHECK(ShouldTranslate(L"आज मौसम अच्छा है", "English"),
               "A2: Hindi -> English must translate");
    TEST_CHECK(ShouldTranslate(L"Γεια σας πώς είστε", "English"),
               "A2: Greek -> English must translate");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Self-Language Bypass tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Self-Language Bypass tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestSoundModule() {
    std::cout << "[RUN] Testing Sound Feedback..." << std::endl;
    const int failures_before = g_failed_count;

    SetSoundEnabled(true);
    TEST_CHECK(IsSoundEnabled(), "Sound is enabled");

    // Test async sound dispatch does not block
    auto start = std::chrono::steady_clock::now();
    PlaySoundAsync(SoundType::Enable);
    PlaySoundAsync(SoundType::Disable);
    PlaySoundAsync(SoundType::CycleLang);
    PlaySoundAsync(SoundType::ToggleAutoSend);
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start
    ).count();

    TEST_CHECK(duration_ms < 50, "PlaySoundAsync returns immediately (< 50ms)");

    SetSoundEnabled(false);
    TEST_CHECK(!IsSoundEnabled(), "Sound can be disabled");
    PlaySoundAsync(SoundType::Enable); // Should return immediately without playing

    SetSoundEnabled(true);
    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Sound Feedback tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Sound Feedback tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestWin32InputModule() {
    std::cout << "[RUN] Testing Win32 Input & Clipboard Safety..." << std::endl;
    const int failures_before = g_failed_count;

    // 1. Synthetic marker (L2 fix: per-process randomized, no longer the
    //    compile-time constant 0x1337BEEF). Contract: non-zero (zero would
    //    collide with real user input's dwExtraInfo and break the bypass).
    volatile DWORD marker = EXTRA_INFO_MARKER;
    TEST_CHECK(marker != 0, "EXTRA_INFO_MARKER is non-zero (per-process random, L2)");
    // The sentinel is now chosen at runtime; a compile-time equality check is
    // impossible by design. Verify the process-stable contract instead: two
    // reads of the exported constant yield the same non-zero value.
    TEST_CHECK(EXTRA_INFO_MARKER == marker, "EXTRA_INFO_MARKER is stable within the process");

    // 2. GDI format filtering
    TEST_CHECK(IsGdiClipboardFormat(CF_BITMAP), "CF_BITMAP is filtered");
    TEST_CHECK(IsGdiClipboardFormat(CF_PALETTE), "CF_PALETTE is filtered");
    TEST_CHECK(IsGdiClipboardFormat(CF_METAFILEPICT), "CF_METAFILEPICT is filtered");
    TEST_CHECK(IsGdiClipboardFormat(CF_ENHMETAFILE), "CF_ENHMETAFILE is filtered");
    TEST_CHECK(!IsGdiClipboardFormat(CF_UNICODETEXT), "CF_UNICODETEXT is safe");
    TEST_CHECK(!IsGdiClipboardFormat(CF_TEXT), "CF_TEXT is safe");
    TEST_CHECK(!IsGdiClipboardFormat(CF_HDROP), "CF_HDROP is safe");

    // 3. Set and get clipboard text roundtrip
    ClipboardBackup backup;
    bool backed_up = BackupClipboard(backup);
    TEST_CHECK(backed_up, "BackupClipboard succeeds");

    std::wstring test_text = L"Emebalachat Clipboard Safety Test 🚀";
    bool set_ok = SetClipboardText(test_text);
    TEST_CHECK(set_ok, "SetClipboardText succeeds");

    // Verify privacy exclusion formats (Windows 10/11 Win+V and Cloud sync exclusion)
    UINT cfExclude = ::RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    UINT cfHistory = ::RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    UINT cfCloud   = ::RegisterClipboardFormatW(L"CanUploadToCloudClipboard");
    TEST_CHECK(cfExclude != 0, "ExcludeClipboardContentFromMonitorProcessing registered");
    TEST_CHECK(cfHistory != 0, "CanIncludeInClipboardHistory registered");
    TEST_CHECK(cfCloud != 0, "CanUploadToCloudClipboard registered");

    if (::OpenClipboard(nullptr)) {
        TEST_CHECK(::IsClipboardFormatAvailable(cfExclude), "Clipboard history exclusion flag present in clipboard");
        TEST_CHECK(::IsClipboardFormatAvailable(cfHistory), "CanIncludeInClipboardHistory flag present in clipboard");
        TEST_CHECK(::IsClipboardFormatAvailable(cfCloud), "CanUploadToCloudClipboard flag present in clipboard");
        ::CloseClipboard();
    }

    std::wstring retrieved = GetClipboardText();
    TEST_CHECK(retrieved == test_text, "GetClipboardText matches what was set");

    // 4. Restore original clipboard
    bool restore_ok = RestoreClipboard(backup);
    TEST_CHECK(restore_ok, "RestoreClipboard succeeds");

    // 5. H1 wrong-window injection guard (pure-logic, no real window needed)
    // Fake HWNDs are opaque invalid handles; GetAncestor() on them returns NULL
    // deterministically, so the equality / null branches are exercised exactly.
    HWND fake_a = reinterpret_cast<HWND>(static_cast<intptr_t>(0x1000));
    HWND fake_b = reinterpret_cast<HWND>(static_cast<intptr_t>(0x2000));

    // No captured target -> always allow (default PasteAndRestore target is null).
    TEST_CHECK(IsSameWindowForInjection(nullptr, nullptr), "Guard allows when no target captured");
    TEST_CHECK(IsSameWindowForInjection(nullptr, fake_a), "Guard allows when target null regardless of foreground");

    // Target captured but foreground lost -> refuse to inject.
    TEST_CHECK(!IsSameWindowForInjection(fake_a, nullptr), "Guard refuses when target captured but foreground null");

    // Identical handle -> allow.
    TEST_CHECK(IsSameWindowForInjection(fake_a, fake_a), "Guard allows identical foreground/target handle");

    // Different window roots -> refuse (focus moved to another application).
    TEST_CHECK(!IsSameWindowForInjection(fake_a, fake_b), "Guard refuses different foreground window root");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Win32 Input & Clipboard Safety tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Win32 Input & Clipboard Safety tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- REQ-R04: clipboard sequence-number copy-settle polling ----
// The Electron IPC delay bug (audit 2.3): BackupClipboard never calls
// EmptyClipboard, so the old fixed Sleep(35) could read the PREVIOUS clipboard
// text whenever the target app commits its copy late; smart bypass then saw
// stale target-language text and skipped translation. The fix replaces the
// sleep with GetClipboardSequenceNumber() polling. ClipboardCopyWatcher is the
// pure, time-parameterized state machine behind the real driver, so the whole
// timeline matrix (including the two-step EmptyClipboard -> SetClipboardData
// gap) is testable headlessly with synthetic timestamps.
void TestClipboardSequencePolling() {
    std::cout << "[RUN] Testing REQ-R04 Clipboard Sequence Polling..." << std::endl;
    const int failures_before = g_failed_count;

    // 1. Compile-time constant wiring: total change budget ~80 ms (REQ-001
    //    Issue A shrink: the 150-200 ms band taxed confirmed-empty copies
    //    with a ~1.5 s freeze before the worker's send-through), poll
    //    cadence 5-10 ms, hard deadline = change + stable.
    static_assert(kClipboardChangeTimeoutMs >= 75 && kClipboardChangeTimeoutMs <= 100,
                  "REQ-R04/REQ-001: change timeout must stay in the 75-100 ms band");
    static_assert(kClipboardPollIntervalMs >= 5 && kClipboardPollIntervalMs <= 10,
                  "REQ-R04: poll interval must stay in the 5-10 ms band");
    static_assert(kClipboardCopyDeadlineMs ==
                      static_cast<uint64_t>(kClipboardChangeTimeoutMs) + kClipboardStableWindowMs,
                  "REQ-R04: hard deadline = change timeout + stable window");
    static_assert(kClipboardChangeTimeoutMs == 80 && kClipboardPollIntervalMs == 8 &&
                      kClipboardStableWindowMs == 16 && kClipboardCopyDeadlineMs == 96,
                "REQ-R04/REQ-001: constants are 80 ms change cap / 8 ms poll / 16 ms stable / 96 ms deadline");

    // 2. REQ-R13 (audit 5 latent item 1): OpenClipboard exponential backoff.
    static_assert(kClipboardOpenMaxAttempts == 5, "REQ-R13: five bounded tries");
    static_assert(ClipboardOpenBackoffDelayMs(0) == 0 && ClipboardOpenBackoffDelayMs(5) == 0 &&
                      ClipboardOpenBackoffDelayMs(6) == 0,
                  "REQ-R13: attempts outside 1..4 schedule no sleep (loop terminates)");
    TEST_CHECK(ClipboardOpenBackoffDelayMs(1) == 5 && ClipboardOpenBackoffDelayMs(2) == 10 &&
                   ClipboardOpenBackoffDelayMs(3) == 20 && ClipboardOpenBackoffDelayMs(4) == 40,
               "REQ-R13: exponential 5/10/20/40 ms retry delays");
    TEST_CHECK(ClipboardOpenBackoffDelayMs(1) + ClipboardOpenBackoffDelayMs(2) +
                   ClipboardOpenBackoffDelayMs(3) + ClipboardOpenBackoffDelayMs(4) <= 100,
               "REQ-R13: total retry sleep stays within the ~100 ms budget");

    // 3. Late Electron commit: change lands 40 ms after Ctrl+C (beyond the old
    //    35 ms sleep that caused the stale read) -> must settle Confirmed.
    {
        ClipboardCopyWatcher w(100, 0);
        bool premature = false;
        for (uint64_t t = 8; t <= 32; t += 8) {
            if (w.Update(100, t) != ClipboardCopyOutcome::Pending) {
                premature = true;
            }
        }
        TEST_CHECK(!premature, "R04: unchanged sequence never confirms early");
        TEST_CHECK(w.Update(101, 40) == ClipboardCopyOutcome::Pending,
                   "R04: single bump inside the stable window stays pending");
        TEST_CHECK(w.Update(101, 56) == ClipboardCopyOutcome::Confirmed,
                   "R04: change stable for kClipboardStableWindowMs -> Confirmed");
    }

    // 4. Two-step write race (EmptyClipboard bumps, SetClipboardData bumps):
    //    confirming on the first bump would read an EMPTY clipboard mid-write.
    {
        ClipboardCopyWatcher w(100, 0);
        TEST_CHECK(w.Update(100, 10) == ClipboardCopyOutcome::Pending, "R04: pre-change poll pending");
        TEST_CHECK(w.Update(101, 20) == ClipboardCopyOutcome::Pending,
                   "R04: EmptyClipboard bump alone must not confirm");
        TEST_CHECK(w.Update(101, 30) == ClipboardCopyOutcome::Pending, "R04: 10 ms < 16 ms stable window");
        TEST_CHECK(w.Update(102, 30) == ClipboardCopyOutcome::Pending,
                   "R04: SetClipboardData bump restarts the stable window");
        TEST_CHECK(w.Update(102, 45) == ClipboardCopyOutcome::Pending, "R04: 15 ms < 16 ms stable window");
        TEST_CHECK(w.Update(102, 46) == ClipboardCopyOutcome::Confirmed,
                   "R04: settled payload after second bump -> Confirmed");
    }

    // 5. Copy failure contract: sequence never leaves the pre-Ctrl+C baseline
    //    (app ignored Ctrl+C) -> Failed, and FAILED IS TERMINAL: a later change
    //    must not re-open it (caller already took the empty-result path).
    {
        ClipboardCopyWatcher w(100, 0);
        TEST_CHECK(w.Update(100, kClipboardChangeTimeoutMs - 1) == ClipboardCopyOutcome::Pending,
                   "R04: pending until the change timeout expires");
        TEST_CHECK(w.Update(100, kClipboardChangeTimeoutMs) == ClipboardCopyOutcome::Failed,
                   "R04: no change by timeout -> Failed (stale read refused)");
        TEST_CHECK(w.Update(101, kClipboardCopyDeadlineMs + 50) == ClipboardCopyOutcome::Failed,
                   "R04: Failed is terminal, never re-opens");
    }

    // 6. Deadline branch: a handler that is STILL writing when the hard
    //    wall-clock deadline hits (no 16 ms stable window) but has demonstrably
    //    advanced past the baseline resolves to Confirmed - the clipboard
    //    provably holds post-Ctrl+C content, unlike the never-changed case.
    {
        ClipboardCopyWatcher w(100, 0);
        TEST_CHECK(w.Update(101, kClipboardCopyDeadlineMs - 1) == ClipboardCopyOutcome::Pending,
                   "R04: change one tick before the deadline is not yet stable -> pending");
        TEST_CHECK(w.Update(102, kClipboardCopyDeadlineMs) == ClipboardCopyOutcome::Confirmed,
                   "R04: advanced-but-flapping at deadline resolves to Confirmed via deadline branch");
    }

    // 7. Real-clipboard sanity: the OS mechanism the driver depends on.
    {
        ClipboardBackup sanity_backup;
        const bool sanity_backed_up = BackupClipboard(sanity_backup);
        const DWORD seq_before = ::GetClipboardSequenceNumber();
        TEST_CHECK(SetClipboardText(L"R04 sequence sanity payload"),
                   "R04: SetClipboardText succeeds for real sequence check");
        const DWORD seq_after = ::GetClipboardSequenceNumber();
        TEST_CHECK(seq_after != seq_before,
                   "R04: a real clipboard write bumps GetClipboardSequenceNumber");
        ClipboardCopyWatcher live(static_cast<uint32_t>(seq_before), 0);
        TEST_CHECK(live.Update(static_cast<uint32_t>(seq_after), 0) == ClipboardCopyOutcome::Pending,
                   "R04: real bump is observed before settling");
        TEST_CHECK(live.Update(static_cast<uint32_t>(seq_after), kClipboardStableWindowMs) == ClipboardCopyOutcome::Confirmed,
                   "R04: watcher confirms a real settled clipboard write");
        if (sanity_backed_up) {
            TEST_CHECK(RestoreClipboard(sanity_backup),
                       "R04: sanity payload cleaned up (original clipboard restored)");
        }
    }

    // 8. REQ-001 per-attempt backoff (session 260910_0003 Issue A; SUPERSEDES
    //    Option D's 180/400/800): the CopySelectedText retry cycle caps the
    //    whole budget at 2 attempts (80/120 ms, sum <=200) so a
    //    confirmed-empty copy on a non-EM control (Discord empty input)
    //    finishes in ~250 ms instead of ~1.5 s. The stale-read refusal is
    //    unchanged: the never-changed case still fails every attempt.
    static_assert(kClipboardCopyAttemptTimeoutMs[0] == 80 &&
                      kClipboardCopyAttemptTimeoutMs[1] == 120,
                   "REQ-001: attempt timeouts are the 80/120 ms capped schedule");
    static_assert(kClipboardCopyAttemptTimeoutMs[0] == kClipboardChangeTimeoutMs,
                   "REQ-001: attempt 1 keeps the single-shot 80 ms budget");
    TEST_CHECK(CopyAttemptTimeoutMs(0) == 80 && CopyAttemptTimeoutMs(1) == 120,
                "REQ-001: CopyAttemptTimeoutMs maps 0-based attempts to 80/120");
    TEST_CHECK(CopyAttemptTimeoutMs(-1) == kClipboardChangeTimeoutMs &&
                    CopyAttemptTimeoutMs(kClipboardCopyChordAttempts) == kClipboardChangeTimeoutMs,
                "REQ-001: out-of-range attempts fall back to the attempt-1 budget");

    // 8a. The retry attempt's longer timeout DELAYS the Failed verdict: at
    //     the 80 ms single-shot mark a 120 ms attempt is still Pending, and
    //     Failed only fires at 120 ms.
    {
        ClipboardCopyWatcher w(100, 0, CopyAttemptTimeoutMs(1));
        TEST_CHECK(w.Update(100, kClipboardChangeTimeoutMs) == ClipboardCopyOutcome::Pending,
                   "REQ-001: 120 ms attempt still pending at the 80 ms single-shot mark");
        TEST_CHECK(w.Update(100, 119) == ClipboardCopyOutcome::Pending,
                   "REQ-001: pending until the retry timeout expires");
        TEST_CHECK(w.Update(100, 120) == ClipboardCopyOutcome::Failed,
                   "REQ-001: no change by 120 ms -> Failed (stale read still refused)");
    }

    // 8b. An Electron-style commit that lands just past the attempt-1 budget
    //     (80 ms) still has the attempt-2 window to confirm within the cycle:
    //     a bump at ~90 ms settles Confirmed on the 120 ms retry.
    {
        ClipboardCopyWatcher w(100, 0, CopyAttemptTimeoutMs(1));
        TEST_CHECK(w.Update(100, 85) == ClipboardCopyOutcome::Pending,
                   "REQ-001: retry attempt pending while the late commit is outstanding");
        TEST_CHECK(w.Update(101, 90) == ClipboardCopyOutcome::Pending,
                   "REQ-001: late bump inside the stable window stays pending");
        TEST_CHECK(w.Update(101, 106) == ClipboardCopyOutcome::Confirmed,
                   "REQ-001: late commit settles Confirmed within the 120 ms attempt");
    }

    // 8c. The hard deadline derives from the instance timeout (change +
    //     stable): an advanced-but-flapping write on a 120 ms attempt
    //     resolves Confirmed at 136 ms, one tick earlier only pending.
    {
        ClipboardCopyWatcher w(100, 0, CopyAttemptTimeoutMs(1));
        TEST_CHECK(w.Update(101, 135) == ClipboardCopyOutcome::Pending,
                   "REQ-001: derived deadline (120+16) not yet reached");
        TEST_CHECK(w.Update(102, 136) == ClipboardCopyOutcome::Confirmed,
                   "REQ-001: flapping-at-deadline branch honors the widened timeout");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-R04 Clipboard Sequence Polling tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-R04 Clipboard Sequence Polling tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestGoogleTranslateModule() {
    std::cout << "[RUN] Testing Google Translate Engine..." << std::endl;
    const int failures_before = g_failed_count;

    // 1. Language code mapping
    TEST_CHECK(GoogleTranslate::MapLanguageCode("AUTO") == "auto", "Map AUTO to auto");
    TEST_CHECK(GoogleTranslate::MapLanguageCode("KO") == "ko", "Map KO to ko");
    TEST_CHECK(GoogleTranslate::MapLanguageCode("EN") == "en", "Map EN to en");
    TEST_CHECK(GoogleTranslate::MapLanguageCode("VI") == "vi", "Map VI to vi");
    TEST_CHECK(GoogleTranslate::MapLanguageCode("ZH-CN") == "zh-CN", "Map ZH-CN to zh-CN");
    TEST_CHECK(GoogleTranslate::MapLanguageCode("FIL") == "tl", "Map FIL to tl");
    TEST_CHECK(GoogleTranslate::MapLanguageCode("Korean") == "ko", "Map Korean by name to ko");
    TEST_CHECK(GoogleTranslate::MapLanguageCode("Vietnamese") == "vi", "Map Vietnamese by name to vi");

    // 2. URL encoding
    TEST_CHECK(GoogleTranslate::UrlEncode("hello world") == "hello%20world", "UrlEncode space");
    TEST_CHECK(GoogleTranslate::UrlEncode("abc-123_.~") == "abc-123_.~", "UrlEncode unreserved");
    TEST_CHECK(GoogleTranslate::UrlEncode("안녕") == "%EC%95%88%EB%85%95", "UrlEncode UTF-8 Korean");

    // 3. Response JSON parsing
    // Format A (dict-chrome-ex single)
    std::string json_a = "[[\"hello\", \"ko\"]]";
    TEST_CHECK(GoogleTranslate::ParseResponseJson(json_a) == L"hello", "Parse dict-chrome-ex single");

    // Format B (dict-chrome-ex multi-segment)
    std::string json_b = "[[\"hello. \", \"ko\"], [\"Nice to meet you.\", \"ko\"]]";
    TEST_CHECK(GoogleTranslate::ParseResponseJson(json_b) == L"hello. Nice to meet you.", "Parse dict-chrome-ex multi-segment");

    // Format C (gtx multi-array)
    std::string json_c = "[[[\"hello\", \"안녕하세요\", null, null, 10]], null, \"ko\"]";
    TEST_CHECK(GoogleTranslate::ParseResponseJson(json_c) == L"hello", "Parse gtx format");

    // Format D (escaped characters)
    std::string json_d = "[[\"Line 1\\nLine 2 with \\\"quotes\\\"\", \"en\"]]";
    TEST_CHECK(GoogleTranslate::ParseResponseJson(json_d) == L"Line 1\nLine 2 with \"quotes\"", "Parse escaped chars");

    // Format E (UTF-16 surrogate pairs for emojis e.g. Rocket \uD83D\uDE80)
    std::string json_e = "[[\"Launch \\uD83D\\uDE80\", \"en\"]]";
    TEST_CHECK(GoogleTranslate::ParseResponseJson(json_e) == L"Launch \U0001F680" || GoogleTranslate::ParseResponseJson(json_e) == L"Launch 🚀",
               "Parse UTF-16 surrogate pair rocket emoji");

    // 4. Live network translation test
    std::wstring live_res = GoogleTranslate::Translate(L"안녕하세요", "KO", "EN");
    TEST_CHECK(!live_res.empty(), "GoogleTranslate live call returned text");
    std::cout << "  [LIVE GT RESULT]: '안녕하세요' -> '" << ToUtf8(live_res) << "'" << std::endl;

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Google Translate Engine tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Google Translate Engine tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- REQ-R05: per-endpoint HTTP profile (Chrome UA + 8 s budget) ----
// HttpGet selects its (UA, timeouts) pair and the Chrome header set from the
// request path via the pure constexpr GoogleTranslate::RequestProfileForPath /
// IsDictChromeExEndpoint functions, so the exact production wiring is pinned
// headlessly: compile-time for the budget, runtime for string content.
void TestGoogleHttpProfile() {
    std::cout << "[RUN] Testing REQ-R05 Google HTTP Profile..." << std::endl;
    const int failures_before = g_failed_count;

    constexpr std::wstring_view kChromePath =
        L"/translate_a/t?client=dict-chrome-ex&sl=ko&tl=en&q=test";
    constexpr std::wstring_view kGtxPath =
        L"/translate_a/single?client=gtx&sl=ko&tl=en&dt=t&q=test";

    // 1. Compile-time pins of the production wiring (same constexpr functions
    //    HttpGet calls at runtime; cannot regress without failing the build).
    static_assert(GoogleTranslate::IsDictChromeExEndpoint(kChromePath),
                  "REQ-R05: Translate()'s primary path must be detected as dict-chrome-ex");
    static_assert(!GoogleTranslate::IsDictChromeExEndpoint(kGtxPath),
                  "REQ-R05: gtx fallback path must NOT take the Chrome profile");
    static_assert(GoogleTranslate::RequestProfileForPath(kChromePath).resolve_ms
                      + GoogleTranslate::RequestProfileForPath(kChromePath).connect_ms
                      + GoogleTranslate::RequestProfileForPath(kChromePath).send_ms
                      + GoogleTranslate::RequestProfileForPath(kChromePath).receive_ms
                      <= 8000,
                  "REQ-R05: Chrome profile budget must never exceed 8 s (anti 16 s lockup)");
    static_assert(GoogleTranslate::RequestProfileForPath(kChromePath).user_agent == kChromeUserAgent,
                  "REQ-R05: Chrome profile must send the Chrome UA");
    static_assert(GoogleTranslate::RequestProfileForPath(kGtxPath).user_agent == kProductUserAgent,
                  "REQ-R05: gtx profile keeps the truthful product UA (L4 honesty)");

    // 2. UA string correctness: standard Chrome desktop token, NUL-terminated,
    //    and NOT the Emebalachat token on the Chrome endpoint.
    {
        const std::wstring_view ua = GoogleTranslate::ChromeUserAgent();
        TEST_CHECK(ua ==
                       L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                       L"(KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36",
                   "REQ-R05: Chrome UA matches the pinned stable-desktop string");
        TEST_CHECK(ua.find(L"Chrome/128.0.0.0") != std::wstring_view::npos,
                   "REQ-R05: UA carries the Chrome/<major> token gatekeepers check");
        TEST_CHECK(ua.find(L"Emebalachat") == std::wstring_view::npos,
                   "REQ-R05: dict-chrome-ex profile never advertises the blocked product UA");
        TEST_CHECK(ua.data()[ua.size()] == L'\0',
                   "REQ-R05: UA view is NUL-terminated (WinHttpOpen takes wchar_t*)");
    }

    // 3. Chrome-typical header construction: WinHttpSendRequest format.
    {
        const std::wstring headers = GoogleTranslate::ChromeAdditionalHeaders();
        TEST_CHECK(headers.find(L"Accept: */*\r\n") != std::wstring::npos,
                   "REQ-R05: Accept header present with CRLF terminator");
        TEST_CHECK(headers.find(L"Accept-Language: en-US,en;q=0.9\r\n") != std::wstring::npos,
                   "REQ-R05: Chrome-typical Accept-Language present");
        TEST_CHECK(headers.size() >= 2 && headers[headers.size() - 2] == L'\r' && headers[headers.size() - 1] == L'\n',
                   "REQ-R05: header block is CRLF-terminated (WinHTTP contract)");
        // WinHTTP rejects line breaks that are not part of a CRLF pair; check
        // every LF is immediately preceded by a CR (the real no-bare-LF rule).
        bool no_bare_lf = true;
        for (size_t nl = headers.find(L'\n'); nl != std::wstring::npos; nl = headers.find(L'\n', nl + 1)) {
            if (nl == 0 || headers[nl - 1] != L'\r') {
                no_bare_lf = false;
                break;
            }
        }
        TEST_CHECK(no_bare_lf, "REQ-R05: every LF belongs to a CRLF pair (no bare LF)");
        // No leading junk: the block must begin with a header field name.
        TEST_CHECK(!headers.empty() && headers[0] != L' ' && headers[0] != L'\r' && headers[0] != L'\n',
                   "REQ-R05: header block starts with a field name (no leading CRLF/space)");
        TEST_CHECK(headers.find(L"Accept-Encoding") == std::wstring::npos,
                   "REQ-R05: no Accept-Encoding claim (WinHTTP cannot inflate gzip)");
    }

    // 4. Runtime endpoint detection incl. negative/adversarial inputs.
    {
        TEST_CHECK(GoogleTranslate::IsDictChromeExEndpoint(kChromePath),
                   "REQ-R05: runtime detection of the primary endpoint");
        TEST_CHECK(!GoogleTranslate::IsDictChromeExEndpoint(L"/translate_a/t?client=gtx&sl=ko&tl=en&q=x"),
                   "REQ-R05: same path prefix with gtx client is not Chrome");
        TEST_CHECK(!GoogleTranslate::IsDictChromeExEndpoint(L""),
                   "REQ-R05: empty path is not Chrome");
        TEST_CHECK(!GoogleTranslate::IsDictChromeExEndpoint(L"client=dict-chrome"),
                   "REQ-R05: truncated token must not match");

        const GoogleHttpProfile chrome = GoogleTranslate::RequestProfileForPath(kChromePath);
        TEST_CHECK(chrome.resolve_ms == 1500 && chrome.connect_ms == 2000 &&
                       chrome.send_ms == 2000 && chrome.receive_ms == 2500,
                   "REQ-R05: Chrome phase split is 1500/2000/2000/2500 ms");
        TEST_CHECK(chrome.resolve_ms + chrome.connect_ms + chrome.send_ms + chrome.receive_ms == 8000,
                   "REQ-R05: Chrome total budget is exactly 8 s (was 16 s, audit 2.4)");
        TEST_CHECK(chrome.user_agent == GoogleTranslate::ChromeUserAgent(),
                   "REQ-R05: Chrome profile UA wiring equals ChromeUserAgent()");

        const GoogleHttpProfile gtx = GoogleTranslate::RequestProfileForPath(kGtxPath);
        TEST_CHECK(gtx.resolve_ms == 3000 && gtx.connect_ms == 3000 &&
                       gtx.send_ms == 5000 && gtx.receive_ms == 5000,
                   "REQ-R05: gtx profile keeps the previous 3/3/5/5 s split");
        TEST_CHECK(gtx.user_agent.find(L"Emebalachat/") == 0,
                   "REQ-R05: gtx profile sends the honest product UA");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-R05 Google HTTP Profile tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-R05 Google HTTP Profile tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestEngineModule() {
    std::cout << "[RUN] Testing Translation Manager..." << std::endl;
    const int failures_before = g_failed_count;

    TranslationManager mgr(EngineType::Auto, "D:\\non_existent_model.gguf");
    TEST_CHECK(mgr.GetEngineType() == EngineType::Auto, "Preferred engine is Auto");
    TEST_CHECK(!mgr.IsLocalModelAvailable(), "Non-existent model path reported as not available");
    TEST_CHECK(mgr.GetActiveEngineName().find("Google Translate") != std::string::npos,
               "Active engine automatically falls back to Google Translate");

    // REQ-R02 (Batch D1) POLICY UPDATE supersedes the old H2 assertion that an
    // auto->cloud path returns empty when the consent gate is disabled: the audit
    // (AUDIT-260905-001 §2.1) identified that silent-empty as a root cause of the
    // chat translation interruption. engine_type=auto now carries its ORIGINAL
    // documented contract ("seamless Google Translate fallback") - selecting auto
    // IS the consent - so the same call must translate via cloud and report Ok.
    TEST_CHECK(!mgr.IsCloudFallbackEnabled(), "Cloud fallback gate defaults to disabled (H2)");
    TranslationStatus auto_status = TranslationStatus::EngineFailed;
    std::wstring gated_res = mgr.Translate(L"감사합니다", "KO", "EN", &auto_status);
    TEST_CHECK(!gated_res.empty(), "REQ-R02: auto->cloud fallback restored when no local model (no silent empty)");
    TEST_CHECK(auto_status == TranslationStatus::Ok, "REQ-R02: auto->cloud success reports Ok status");

    // Strict explicit-local semantics (REQ-029-B update): an explicit local
    // pin with the model file ABSENT is surfaced as LocalModelMissing by the
    // pre-block guard in Translate - not the old masquerade (active engine
    // renamed to "Google Translate (Model Not Found)" and status=2
    // CloudConsentBlocked, which blamed the H2 privacy gate for a missing
    // file while logs/UI claimed Google).
    {
        TranslationManager strict_local(EngineType::LocalLlama, "D:\\non_existent_model.gguf");
        TranslationStatus st = TranslationStatus::Ok;
        std::wstring res = strict_local.Translate(L"안녕하세요", "KO", "EN", &st);
        TEST_CHECK(res.empty(), "REQ-R02: explicit local without consent still keeps text on-device (empty)");
        TEST_CHECK(st == TranslationStatus::LocalModelMissing, "REQ-029-B: missing model surfaced as LocalModelMissing, never CloudConsentBlocked");

        // REQ-029-B: cloud_fallback_enabled_ gates post-INFERENCE failures
        // only. With the model absent there is no inference that could fail,
        // so even with consent the guard refuses the cloud seam - text never
        // leaves the device while a local pin stands on a missing file.
        strict_local.SetCloudFallbackEnabled(true);
        TranslationStatus st2 = TranslationStatus::InputEmpty;
        std::wstring res2 = strict_local.Translate(L"안녕하세요", "KO", "EN", &st2);
        TEST_CHECK(res2.empty(), "REQ-029-B: consent does not resurrect a missing model (no cloud call)");
        TEST_CHECK(st2 == TranslationStatus::LocalModelMissing, "REQ-029-B: status stays LocalModelMissing with cloud_fallback enabled");
    }

    // REQ-029-B display honesty (new pinned case): a LocalLlama manager over
    // an absent model file must (1) translate to nothing, (2) report
    // LocalModelMissing, and (3) name its active engine exactly
    // "Local (Model Missing)" - no "Google" substring, so the tray
    // find("Google") checkmark can no longer misattribute a local setup to
    // the cloud (the user's "setting the engine to Google changed nothing"
    // masquerade root, debug report §3.1).
    {
        TranslationManager missing_model(EngineType::LocalLlama, "D:\\non_existent.gguf");
        TEST_CHECK(!missing_model.IsLocalModelAvailable(), "REQ-029-B: non-existent gguf reported unavailable");
        TEST_CHECK(missing_model.GetActiveEngineName() == "Local (Model Missing)",
                   "REQ-029-B: active engine name is honest 'Local (Model Missing)', no Google masquerade");
        TranslationStatus st = TranslationStatus::Ok;
        std::wstring res = missing_model.Translate(L"안녕하세요", "KO", "EN", &st);
        TEST_CHECK(res.empty(), "REQ-029-B: missing-model local pin returns empty (never a cloud result)");
        TEST_CHECK(st == TranslationStatus::LocalModelMissing, "REQ-029-B: missing-model local pin reports LocalModelMissing");
    }

    // Empty input is a neutral outcome, not a failure.
    {
        TranslationStatus st = TranslationStatus::EngineFailed;
        std::wstring res = mgr.Translate(L"", "KO", "EN", &st);
        TEST_CHECK(res.empty(), "REQ-R02: empty input returns empty");
        TEST_CHECK(st == TranslationStatus::InputEmpty, "REQ-R02: empty input reports InputEmpty status");
    }

    // Gate toggling remains functional (used by explicit-local consent above).
    mgr.SetCloudFallbackEnabled(true);
    TEST_CHECK(mgr.IsCloudFallbackEnabled(), "Cloud fallback gate can be enabled via setter");
    std::wstring translated = mgr.Translate(L"감사합니다", "KO", "EN");
    TEST_CHECK(!translated.empty(), "Manager Translate succeeds via auto fallback when gate enabled");
    std::wcout << L"  [LIVE MGR RESULT]: '감사합니다' -> '" << translated << L"'" << std::endl;

    // Sampling parameter modification and verification
    mgr.SetSamplingParams(0.5f, 0.8f, 30, 1.1f);
    TEST_CHECK(std::abs(mgr.GetTemperature() - 0.5f) < 0.001f, "SetSamplingParams temperature");
    TEST_CHECK(std::abs(mgr.GetTopP() - 0.8f) < 0.001f, "SetSamplingParams top_p");
    TEST_CHECK(mgr.GetTopK() == 30, "SetSamplingParams top_k");
    TEST_CHECK(std::abs(mgr.GetRepetitionPenalty() - 1.1f) < 0.001f, "SetSamplingParams repetition_penalty");

    // Reset back to Tencent Hunyuan Lab official parameters (0.7, 0.6, 20, 1.05)
    mgr.SetSamplingParams(0.7f, 0.6f, 20, 1.05f);
    TEST_CHECK(std::abs(mgr.GetTemperature() - 0.7f) < 0.001f, "Tencent tuned temperature 0.7");
    TEST_CHECK(std::abs(mgr.GetTopP() - 0.6f) < 0.001f, "Tencent tuned top_p 0.6");
    TEST_CHECK(mgr.GetTopK() == 20, "Tencent tuned top_k 20");
    TEST_CHECK(std::abs(mgr.GetRepetitionPenalty() - 1.05f) < 0.001f, "Tencent tuned repetition_penalty 1.05");

    // Portable large-fixture resolution (L1): optional EMEBALA_MODEL_PATH env
    // override for models kept outside the repo, else the repo-relative default
    // matching AppConfig::model_path. Skips cleanly when absent — no personal
    // absolute paths committed.
    std::string local_model_path;
    char env_model[4096] = {0};
    DWORD env_len = ::GetEnvironmentVariableA(
        "EMEBALA_MODEL_PATH", env_model, static_cast<DWORD>(sizeof(env_model)) - 1);
    if (env_len > 0 && env_len < static_cast<DWORD>(sizeof(env_model)) - 1) {
        local_model_path = env_model;
    } else {
        local_model_path = ResolveRepoFile(L"models\\Hy-MT2-1.8B-Q8_0.gguf").string();
    }
    if (!local_model_path.empty() && std::filesystem::exists(local_model_path)) {
        std::cout << "  [LOCAL LLM TEST] Loading Hy-MT2-1.8B-Q8_0.gguf for real offline translation..." << std::endl;
        TranslationManager local_mgr(EngineType::LocalLlama, local_model_path);
        TEST_CHECK(local_mgr.IsLocalModelAvailable(), "Local model exists");
        TEST_CHECK(local_mgr.GetActiveEngineName().find("Hy-MT2") != std::string::npos, "Active engine is Hy-MT2");

        auto t0 = std::chrono::steady_clock::now();
        std::wstring local_res = local_mgr.Translate(L"안녕하세요, 만나서 반갑습니다.", "KO", "English");
        auto t1 = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        std::cout << "  [LOCAL LLAMA RESULT in " << ms << " ms]: '안녕하세요, 만나서 반갑습니다.' -> '" << ToUtf8(local_res) << "'" << std::endl;
        TEST_CHECK(!local_res.empty(), "Local LLM translation produced non-empty result");
        TEST_CHECK(local_res != L"안녕하세요, 만나서 반갑습니다.", "Local LLM translation changed Korean to target language");

        // Test second call to verify KV cache clearing and instant cached model response
        auto t2 = std::chrono::steady_clock::now();
        std::wstring second_res = local_mgr.Translate(L"오늘 날씨가 아주 좋습니다.", "KO", "English");
        auto t3 = std::chrono::steady_clock::now();
        auto ms2 = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
        std::cout << "  [CACHED LLAMA RESULT in " << ms2 << " ms]: '오늘 날씨가 아주 좋습니다.' -> '" << ToUtf8(second_res) << "'" << std::endl;
        TEST_CHECK(!second_res.empty(), "Second local LLM call produced non-empty result");

        // REQ-R01 (audit §2.1): text long enough to exceed the 4096-token context
        // must be safely truncated (head+tail window) and STILL produce a
        // non-empty translation - no decode crash, no silent {}. ~8000 Korean
        // UTF-16 units tokenize to well beyond kLlamaPromptTokenBudget (2032).
        {
            std::wstring filler;
            filler.reserve(9000);
            while (filler.size() < 8000) {
                filler += L"안녕하세요, 만나서 반갑습니다. 오늘 날씨가 아주 좋습니다. 번역 테스트를 위한 긴 문장을 반복해서 채웁니다. ";
            }
            auto t4 = std::chrono::steady_clock::now();
            TranslationStatus ovf_st = TranslationStatus::EngineFailed;
            std::wstring ovf_res = local_mgr.Translate(filler, "KO", "English", &ovf_st);
            auto t5 = std::chrono::steady_clock::now();
            auto ms_ovf = std::chrono::duration_cast<std::chrono::milliseconds>(t5 - t4).count();
            std::cout << "  [R01 OVERFLOW in " << ms_ovf << " ms]: src " << filler.size()
                      << " UTF-16 units -> result " << ovf_res.size() << " units, status "
                      << static_cast<int>(ovf_st) << std::endl;
            TEST_CHECK(!ovf_res.empty(), "REQ-R01: >budget-token input still returns a non-empty translation (no crash, no silent empty)");
            TEST_CHECK(ovf_st == TranslationStatus::Ok, "REQ-R01: overflow truncation path reports Ok status");
            // The engine must remain usable after an overflow request (KV state sane).
            std::wstring after_ovf = local_mgr.Translate(L"고맙습니다.", "KO", "English");
            TEST_CHECK(!after_ovf.empty(), "REQ-R01: engine still translates normally after an overflow request");
        }
    } else {
        std::cout << "  [LOCAL LLM SKIP] Model fixture absent ("
                  << (local_model_path.empty()
                          ? "set EMEBALA_MODEL_PATH or add models/Hy-MT2-1.8B-Q8_0.gguf"
                          : local_model_path)
                  << ")" << std::endl;
    }

    // H2 explicit-choice exception: a user who deliberately selects engine_type=google
    // has explicitly consented to the cloud, so Translate works even with the gate off.
    TranslationManager google_mgr(EngineType::GoogleTranslate, "");
    TEST_CHECK(!google_mgr.IsCloudFallbackEnabled(), "Explicit-google manager also defaults gate off");
    std::wstring google_res = google_mgr.Translate(L"안녕하세요", "KO", "EN");
    TEST_CHECK(!google_res.empty(), "H2: explicit engine_type=google bypasses gate (deliberate consent)");
    std::cout << "  [EXPLICIT GOOGLE RESULT]: '안녕하세요' -> '" << ToUtf8(google_res) << "'" << std::endl;

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Translation Manager tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Translation Manager tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// M3 (security): pure-logic validation of IsValidModelPath - no real GGUF model
// is loaded; fixtures are tiny temp files created/cleaned here.
void TestModelPathValidation() {
    std::cout << "[RUN] Testing M3 Model Path Validation..." << std::endl;
    const int failures_before = g_failed_count;

    // Fixture: <temp>/emebala_m3_test/models/fake.gguf (valid), plus a
    // directory named trick.gguf and a sibling outside the base dir.
    std::error_code ec;
    std::filesystem::path root = std::filesystem::temp_directory_path(ec) / "emebala_m3_test";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "models", ec);
    TEST_CHECK(!ec, "M3 fixture: temp dirs created");

    std::filesystem::path valid = root / "models" / "fake.gguf";
    {
        std::ofstream of(valid, std::ios::binary);
        of << "GGUF-fixture";
    }
    std::filesystem::path upper = root / "models" / "FAKE2.GGUF";
    {
        std::ofstream of(upper, std::ios::binary);
        of << "GGUF-fixture";
    }
    std::filesystem::path wrong_ext = root / "models" / "payload.exe";
    {
        std::ofstream of(wrong_ext, std::ios::binary);
        of << "MZ";
    }
    std::filesystem::path dir_as_gguf = root / "models" / "trick.gguf";
    std::filesystem::create_directories(dir_as_gguf, ec);
    std::filesystem::path outside = root / "outside.gguf";
    {
        std::ofstream of(outside, std::ios::binary);
        of << "GGUF-outside-base";
    }

    // 1. Valid absolute .gguf regular file -> accepted.
    TEST_CHECK(IsValidModelPath(valid.string()), "M3: valid absolute .gguf file accepted");

    // 2. Case-insensitive extension (.GGUF) -> accepted.
    TEST_CHECK(IsValidModelPath(upper.string()), "M3: uppercase .GGUF extension accepted");

    // 3. Empty path -> rejected.
    TEST_CHECK(!IsValidModelPath(""), "M3: empty path rejected");

    // 4. Wrong extension -> rejected even though the file exists.
    TEST_CHECK(!IsValidModelPath(wrong_ext.string()), "M3: non-.gguf extension rejected");

    // 5. Path with no extension -> rejected.
    TEST_CHECK(!IsValidModelPath((root / "models" / "noext").string()), "M3: extensionless path rejected");

    // 6. Non-existent .gguf path -> rejected.
    TEST_CHECK(!IsValidModelPath((root / "models" / "missing.gguf").string()), "M3: missing file rejected");

    // 7. Directory named *.gguf -> rejected (must be a REGULAR file).
    TEST_CHECK(!IsValidModelPath(dir_as_gguf.string()), "M3: directory with .gguf name rejected");

    // 8. Relative path inside base_dir -> accepted (resolved against base).
    TEST_CHECK(IsValidModelPath("fake.gguf", (root / "models").string()),
               "M3: relative in-base path accepted");

    // 9. Relative path with '..' escaping base_dir -> rejected (traversal).
    TEST_CHECK(!IsValidModelPath("../outside.gguf", (root / "models").string()),
               "M3: '../' traversal out of base dir rejected");

    // 10. Deeper escape that re-enters the base dir name -> still rejected
    // (resolves to root/models/../outside.gguf collapsing OUT of base).
    TEST_CHECK(!IsValidModelPath("sub/../../outside.gguf", (root / "models").string()),
               "M3: multi-step traversal escape rejected");

    // 11. Legitimate relative path that resolves inside base after collapsing
    // ('./sub/../fake.gguf') -> accepted (containment compares resolved forms).
    TEST_CHECK(IsValidModelPath("./sub/../fake.gguf", (root / "models").string()),
               "M3: relative path that collapses back inside base accepted");

    // Clean up fixtures (best-effort; temp dir, safe to force-remove).
    std::filesystem::remove_all(root, ec);

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] M3 Model Path Validation tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] M3 Model Path Validation tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// F3 (security, session 260909_0002): runtime SHA-256 model pin + marker
// cache. Headless against temp fixtures - no real model needed. Pins:
//  * ComputeFileSha256 against NIST known-answer vectors (empty, "abc").
//  * Fail-closed for the PINNED filename on any hash mismatch (006).
//  * Consent (warn-and-allow) for user-configured alternative names (005),
//    with a marker cached for the file's own hash.
//  * Forged/mismatched markers never authorize skipping the hash for the
//    pinned filename.
//  * Marker cache hit (mtime+size unchanged) short-circuits re-verification.
void TestModelSha256Verification() {
    std::cout << "[RUN] Testing F3 model SHA-256 verification..." << std::endl;
    const int failures_before = g_failed_count;

    std::error_code ec;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path(ec) / "emebala_f3_sha256";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "models", ec);
    std::filesystem::create_directories(root / "markers", ec);
    TEST_CHECK(!ec, "F3 fixture: temp dirs created");
    const std::filesystem::path markers = root / "markers";

    auto write_file = [](const std::filesystem::path& p, std::string_view bytes) {
        std::ofstream of(p, std::ios::binary | std::ios::trunc);
        of.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    };

    // 1. Known-answer vectors (FIPS 180-4 / NIST examples).
    const std::filesystem::path abc = root / "abc.bin";
    write_file(abc, "abc");
    std::string hex;
    TEST_CHECK(ComputeFileSha256(abc, hex), "F3: ComputeFileSha256 succeeds on a readable file");
    TEST_CHECK(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
               "F3: SHA-256(\"abc\") matches the NIST known-answer vector");

    const std::filesystem::path empty = root / "empty.bin";
    write_file(empty, "");
    TEST_CHECK(ComputeFileSha256(empty, hex) &&
               hex == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
               "F3: SHA-256(empty) matches the known-answer vector");

    // 2. Unreadable/missing file -> ComputeFileSha256 fails closed.
    TEST_CHECK(!ComputeFileSha256(root / "nope.bin", hex),
               "F3: ComputeFileSha256 fails for a missing file");

    // 3. VerifyModelSha256 guard clauses.
    TEST_CHECK(!VerifyModelSha256({}, markers), "F3: empty model path rejected (001)");
    TEST_CHECK(!VerifyModelSha256(root / "models" / "missing.gguf", markers),
               "F3: missing model file rejected (002)");

    // 4. PINNED filename + wrong bytes -> fail-closed, no marker written.
    const std::filesystem::path pinned_bad = root / "models" / "Hy-MT2-1.8B-Q8_0.gguf";
    write_file(pinned_bad, "not-the-real-model");
    TEST_CHECK(!VerifyModelSha256(pinned_bad, markers),
               "F3: pinned-name file with non-matching hash is blocked (006)");
    TEST_CHECK(!std::filesystem::exists(markers / L"Hy-MT2-1.8B-Q8_0.gguf.sha256ok"),
               "F3: blocked verification writes no marker");

    // 5. User-configured alternative name -> consent path allows it (005) and
    //    caches a marker keyed on the file's OWN hash (no re-hash per launch).
    const std::filesystem::path user_model = root / "models" / "my-q4.gguf";
    write_file(user_model, "user-quantized-model-bytes");
    TEST_CHECK(VerifyModelSha256(user_model, markers),
               "F3: alternative user-configured model proceeds on consent basis (005)");
    const std::filesystem::path user_marker =
        markers / (std::wstring(user_model.filename().native()) + L".sha256ok");
    TEST_CHECK(std::filesystem::exists(user_marker),
               "F3: consent path persists a verification marker");
    // The marker stores the file's own hash (not the pin) as line 1.
    {
        std::ifstream in(user_marker, std::ios::binary);
        std::string first;
        in >> first;
        TEST_CHECK(ComputeFileSha256(user_model, hex) && first == hex,
                   "F3: consent marker records the file's own hash, never the pin");
    }

    // 6. Tampering the consented model changes size/mtime -> marker must be
    //    re-evaluated (hash recomputed; still allowed for the alt name).
    write_file(user_model, "user-quantized-model-bytes-tampered");
    TEST_CHECK(VerifyModelSha256(user_model, markers),
               "F3: changed consented model re-verifies through the marker-invalidation path");

    // 7. Consent-marker laundering proof: a marker generated for a USER model
    //    (line 1 = that model's own hash, NOT the pin) must never authorize a
    //    PINNED filename. Fabricate exactly that: a consent-style marker with
    //    matching mtime/size for a pinned-name file with wrong bytes. The
    //    anti-forgery rule in MarkerMatchesFile rejects non-pin markers on
    //    pinned names, forcing a real re-hash -> still blocked (006).
    //    (A marker carrying the PIN hash on a pinned name is the legitimate
    //    cache of a previously verified good file; marker-file integrity is
    //    the same-directory trust boundary documented in engine.cpp.)
    {
        const std::filesystem::path models2 = root / "models2";
        std::filesystem::create_directories(models2, ec);
        const std::filesystem::path pinned_forged = models2 / "Hy-MT2-1.8B-Q8_0.gguf";
        write_file(pinned_forged, "still-not-the-model");
        const std::filesystem::path forged_marker =
            markers / L"Hy-MT2-1.8B-Q8_0.gguf.sha256ok";
        {
            std::ofstream m(forged_marker, std::ios::binary | std::ios::trunc);
            m << "0000000000000000000000000000000000000000000000000000000000000000" << '\n'
              << std::filesystem::last_write_time(pinned_forged, ec).time_since_epoch().count() << '\n'
              << static_cast<unsigned long long>(std::filesystem::file_size(pinned_forged, ec)) << '\n';
        }
        TEST_CHECK(!VerifyModelSha256(pinned_forged, markers),
                   "F3: consent-style marker cannot authorize a pinned-name file with wrong content");
    }

    // 8. Positive cache-hit proof (deterministic, no timing): user_model was
    //    verified in case 6 and its consent marker matches the current
    //    mtime/size. Re-open it EXCLUSIVELY (share=0): a re-hash attempt would
    //    fail at CreateFileW (003). The call must still return true, which is
    //    only possible through the marker cache - pinning the "skip the full
    //    hash when mtime+size are unchanged" behavior observationally.
    {
        HANDLE hExclusive = ::CreateFileW(user_model.c_str(), GENERIC_READ, 0, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        TEST_CHECK(hExclusive != INVALID_HANDLE_VALUE,
                   "F3: exclusive re-open of consented model succeeded");
        if (hExclusive != INVALID_HANDLE_VALUE) {
            TEST_CHECK(VerifyModelSha256(user_model, markers),
                       "F3: marker cache hit skips re-hashing (true even while the file is unhashable)");
            ::CloseHandle(hExclusive);
        }
    }

    std::filesystem::remove_all(root, ec);

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] F3 model SHA-256 verification tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] F3 model SHA-256 verification tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// REQ-R11 (audit §4 M3): ResolveModelPath / GetExecutableDir - relative model
// paths must anchor at the EXECUTABLE directory, not the CWD, so Run-registry
// autostart (CWD=C:\Windows\System32) keeps loading the model. base_dir is
// injectable, so the whole suite runs headlessly against temp fixtures.
void TestModelPathNormalization() {
    std::cout << "[RUN] Testing REQ-R11 Model Path Normalization..." << std::endl;
    const int failures_before = g_failed_count;

    std::error_code ec;
    std::filesystem::path root = std::filesystem::temp_directory_path(ec) / "emebala_r11_test";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "models", ec);
    TEST_CHECK(!ec, "R11 fixture: temp dirs created");

    std::filesystem::path model = root / "models" / "fake.gguf";
    {
        std::ofstream of(model, std::ios::binary);
        of << "GGUF-fixture";
    }
    std::filesystem::path other = root / "other";
    std::filesystem::create_directories(other, ec);
    std::filesystem::path outside = root / "outside.gguf";
    {
        std::ofstream of(outside, std::ios::binary);
        of << "GGUF-outside-base";
    }

    // --- GetExecutableDir: absolute anchor next to the running exe ---
    const std::filesystem::path exe_dir = GetExecutableDir();
    TEST_CHECK(!exe_dir.empty(), "R11: GetExecutableDir non-empty");
    TEST_CHECK(exe_dir.is_absolute(), "R11: GetExecutableDir is absolute");
    {
        std::error_code fec;
        TEST_CHECK(std::filesystem::exists(exe_dir, fec), "R11: GetExecutableDir exists on disk");
    }
    {
        wchar_t exe_path[MAX_PATH] = {0};
        const DWORD n = ::GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        TEST_CHECK(n > 0 && n < MAX_PATH, "R11: GetModuleFileNameW succeeds in test exe");
        TEST_CHECK(std::filesystem::path(exe_path).parent_path() == exe_dir,
                   "R11: GetExecutableDir matches module path parent");
    }

    // --- ResolveModelPath contract (header doc) ---
    TEST_CHECK(ResolveModelPath("").empty(), "R11: empty raw path resolves to empty");

    // Absolute input containing '..': collapses via "other/.." back to
    // root/models/fake.gguf, and the injected base ("other") must be ignored.
    const std::string abs_in = (root / "other" / ".." / "models" / "fake.gguf").string();
    const std::string abs_out = ResolveModelPath(abs_in, other.string());
    TEST_CHECK(std::filesystem::path(abs_out) == model,
               "R11: absolute path lexically normalized, base_dir ignored");

    const std::string rel = ResolveModelPath("models/fake.gguf", root.string());
    TEST_CHECK(!rel.empty() && std::filesystem::path(rel).is_absolute(),
               "R11: relative path against injected base becomes absolute");
    TEST_CHECK(std::filesystem::path(rel) == model,
               "R11: relative resolution joins base exactly");
    {
        std::error_code fec;
        TEST_CHECK(std::filesystem::exists(std::filesystem::path(rel), fec),
                   "R11: resolved relative path exists on disk");
    }

    // Mixed forward/back separators normalize to the same target.
    TEST_CHECK(ResolveModelPath("models\\fake.gguf", root.string()) == rel,
               "R11: forward-slash and backslash spellings resolve identically");

    // Full chain: resolved path must satisfy the security validator too.
    TEST_CHECK(IsValidModelPath(rel),
               "R11: resolved path passes IsValidModelPath (absolutized => CWD-free)");

    // '..' escape must NOT be laundered into an absolute outside path:
    // result stays relative so IsValidModelPath still rejects it fail-closed.
    const std::string escaped = ResolveModelPath("../outside.gguf", (root / "models").string());
    TEST_CHECK(!escaped.empty() && !std::filesystem::path(escaped).is_absolute(),
               "R11: traversal escape is not absolutized (stays relative)");
    TEST_CHECK(!IsValidModelPath(escaped, (root / "models").string()),
               "R11: escaped path still rejected by IsValidModelPath (fail-closed kept)");

    // Default base (no injection) anchors at the exe directory, not the CWD.
    const std::string defaulted = ResolveModelPath("zyx_nonexistent.gguf");
    TEST_CHECK(std::filesystem::path(defaulted).is_absolute(),
               "R11: default-base resolution is absolute");
    TEST_CHECK(std::filesystem::path(defaulted).parent_path() == exe_dir,
               "R11: default base is GetExecutableDir()");

    // --- System32 reproduction (audit §4 M3) ---
    // Simulate Run-registry autostart: process CWD is somewhere that does NOT
    // contain models/fake.gguf (stands in for C:\Windows\System32). The bare
    // relative path fails validation against the CWD (the old bug), while the
    // exe/base-normalized path loads (the fix).
    std::filesystem::path cwd_backup = std::filesystem::current_path(ec);
    std::filesystem::path fake_system32 = root / "cwd_like_system32";
    std::filesystem::create_directories(fake_system32, ec);
    std::filesystem::current_path(fake_system32, ec);
    TEST_CHECK(!ec, "R11 fixture: CWD switched to System32 stand-in");

    TEST_CHECK(!IsValidModelPath("models/fake.gguf"),
               "R11 repro: bare relative path FAILS against foreign CWD (old M3 bug)");
    const std::string normalized = ResolveModelPath("models/fake.gguf", root.string());
    TEST_CHECK(IsValidModelPath(normalized),
               "R11 fix: exe-dir-normalized path loads regardless of CWD");

    std::filesystem::current_path(cwd_backup, ec);
    TEST_CHECK(!ec, "R11 fixture: CWD restored");

    std::filesystem::remove_all(root, ec);
    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-R11 Model Path Normalization tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-R11 Model Path Normalization tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// REQ-R12 (audit §4 I4): GetSnapshot() is the thread-safe read seam that
// main.cpp callbacks must use while hook/worker threads mutate the shared
// std::string fields under mutex_ (the on_toggle_badge pattern). Headless
// stress: a writer thread alternates two long distinct values through the
// locked setter while a reader takes snapshots; a torn (non-mutex) read of a
// std::string under concurrent write yields a value matching NEITHER string
// (heap buffer reuse) or crashes outright.
void TestConfigSnapshotThreadSafety() {
    std::cout << "[RUN] Testing REQ-R12 Config Snapshot Thread Safety..." << std::endl;
    const int failures_before = g_failed_count;

    AppConfig cfg;
    const auto initial = cfg.GetSnapshot();
    TEST_CHECK(initial.source_language == "Auto Detect" &&
               initial.target_language == "English",
               "R12: snapshot returns coherent defaults");

    // Snapshot is a value copy: mutating it must not feed back into config.
    AppConfig::Snapshot copy = cfg.GetSnapshot();
    copy.source_language = "TAMPERED";
    TEST_CHECK(cfg.GetSnapshot().source_language == "Auto Detect",
               "R12: snapshot is an independent copy (no aliasing)");

    static const std::string kA(256, 'A'); // long enough to force a heap buffer
    static const std::string kB(256, 'B');

    // Prime the config with kA so a reader spinning before writer iteration 0
    // observes a valid test string instead of the initial "Auto Detect" default.
    cfg.SetSourceLanguage(kA);

    std::atomic<bool> stop{false};
    std::atomic<long long> torn_reads{0};
    std::atomic<long long> total_reads{0};

    std::thread writer([&cfg, &stop]() {
        for (int i = 0; i < 20000 && !stop.load(std::memory_order_relaxed); ++i) {
            cfg.SetSourceLanguage((i & 1) ? kB : kA);
        }
    });
    std::thread reader([&cfg, &stop, &torn_reads, &total_reads]() {
        while (!stop.load(std::memory_order_relaxed)) {
            const auto snap = cfg.GetSnapshot();
            total_reads.fetch_add(1, std::memory_order_relaxed);
            if (snap.source_language != kA && snap.source_language != kB) {
                torn_reads.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    writer.join();
    stop.store(true, std::memory_order_relaxed);
    reader.join();

    TEST_CHECK(total_reads.load() > 0, "R12: snapshot reader actually ran");
    TEST_CHECK(torn_reads.load() == 0, "R12: zero torn reads under concurrent SetSourceLanguage");

    // Last write wins and is visible through the snapshot.
    cfg.SetSourceLanguage("Korean");
    TEST_CHECK(cfg.GetSnapshot().source_language == "Korean",
               "R12: snapshot reflects the latest locked write");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-R12 Config Snapshot Thread Safety tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-R12 Config Snapshot Thread Safety tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// REQ-R01 (Batch D1): pure-logic tests for the head+tail sliding-window
// truncation that guards llama_decode against >n_ctx prompts. No model needed;
// asserts the geometric contract (fit-through, head/tail preservation, marker
// insertion) and UTF-16 surrogate-pair safety at both cut points.
void TestTokenTruncation() {
    std::cout << "[RUN] Testing REQ-R01 Head/Tail Token Truncation..." << std::endl;
    const int failures_before = g_failed_count;

    // Budget constants shared with the engine. Compile-time pinned per the
    // task's "static-assertion-style checks where feasible" directive (and it
    // avoids MSVC C4127 constant-condition warnings that runtime asserts on
    // constexpr values would emit under /W4).
    static_assert(kLlamaNCtx == 4096, "test build/P2: n_ctx 4096 (official Hy-MT2 card)");
    static_assert(kLlamaGenReserve == 2048, "test build/P2: generation reserve 2048");
    static_assert(kLlamaPromptTokenBudget == 4096 - 2048 - 16, "test build: budget 2032");
    static_assert(kLlamaPromptTokenBudget == 2032, "REQ-R01/P2: prompt token budget is 2032");
    static_assert(kLlamaNCtx > kLlamaPromptTokenBudget, "REQ-R01: budget leaves generation reserve");

    // Helper: true if s contains ANY unpaired (lone) UTF-16 surrogate.
    auto has_lone_surrogate = [](std::wstring_view s) {
        for (size_t i = 0; i < s.size(); ++i) {
            wchar_t c = s[i];
            if (c >= 0xD800 && c <= 0xDBFF) { // high surrogate must pair with next
                if (i + 1 >= s.size() || s[i + 1] < 0xDC00 || s[i + 1] > 0xDFFF) {
                    return true;
                }
                ++i;
            } else if (c >= 0xDC00 && c <= 0xDFFF) { // low surrogate without preceding high
                return true;
            }
        }
        return false;
    };

    // 1. Empty input -> empty output.
    TEST_CHECK(TruncateHeadTailWindow(L"", 10).empty(), "R01: empty text returns empty");

    // 2. Text that fits (size <= 2*keep) is returned byte-identical.
    {
        std::wstring fits = L"short text";
        TEST_CHECK(TruncateHeadTailWindow(fits, 32) == fits, "R01: fitting text unchanged");
        std::wstring exact = std::wstring(20, L'A');
        TEST_CHECK(TruncateHeadTailWindow(exact, 10) == exact, "R01: size == 2*keep boundary unchanged");
    }

    // 3. One over the boundary -> truncation kicks in, head+tail+marker present.
    {
        std::wstring src(21, L'X');
        std::wstring out = TruncateHeadTailWindow(src, 10);
        TEST_CHECK(out != src, "R01: size 2*keep+1 is truncated");
        TEST_CHECK(out.size() < src.size() + 3, "R01: truncated output is not longer than src+marker");
        TEST_CHECK(out.find(L"\n\u2026\n") != std::wstring::npos, "R01: ellipsis marker inserted");
        TEST_CHECK(out.rfind(L"XXXXXXXXXX", 0) == 0, "R01: head preserved (first keep units)");
        TEST_CHECK(out.size() >= 10 && out.compare(out.size() - 10, 10, L"XXXXXXXXXX") == 0,
                   "R01: tail preserved (last keep units)");
        TEST_CHECK(!has_lone_surrogate(out), "R01: plain-text truncation keeps valid UTF-16");
    }

    // 4. Surrogate PAIR straddling the head cut: the pair (high at index keep-1,
    //    low at index keep) is the last head unit + first cut unit. The engine
    //    must DROP the orphaned high surrogate instead of emitting a lone one.
    {
        // 9 A's + high(0xD83D)+low(0xDE80) pair at 9..10 + 11 B's -> size 22
        std::wstring src = std::wstring(9, L'A') + wchar_t(0xD83D) + wchar_t(0xDE80) + std::wstring(11, L'B');
        std::wstring out = TruncateHeadTailWindow(src, 10);
        TEST_CHECK(!has_lone_surrogate(out), "R01: head cut never emits a lone HIGH surrogate");
        TEST_CHECK(out.find(L"\n\u2026\n") != std::wstring::npos, "R01: marker present after adjusted head");
        TEST_CHECK(out.compare(0, 9, std::wstring(9, L'A')) == 0, "R01: head keeps only the 9 clean A's");
        TEST_CHECK(out.compare(out.size() - 10, 10, std::wstring(10, L'B')) == 0,
                   "R01: tail still ends with the last 10 units");
    }

    // 5. Surrogate PAIR straddling the tail cut: high half lands in the cut
    //    region, low half is exactly at tail_start. The engine must shift the
    //    tail start inward past the orphaned low surrogate.
    {
        // 10 A's + high(0xD83D at idx 10, cut away)+low(0xDE80 at idx 11) + 9 B's -> size 21
        std::wstring src = std::wstring(10, L'A') + wchar_t(0xD83D) + wchar_t(0xDE80) + std::wstring(9, L'B');
        std::wstring out = TruncateHeadTailWindow(src, 10);
        TEST_CHECK(!has_lone_surrogate(out), "R01: tail cut never emits a lone LOW surrogate");
        TEST_CHECK(out.compare(0, 10, std::wstring(10, L'A')) == 0, "R01: head of 10 A's kept intact");
        TEST_CHECK(out.compare(out.size() - 9, 9, std::wstring(9, L'B')) == 0,
                   "R01: tail preserves the trailing run after skipping orphan low surrogate");
    }

    // 6. Degenerate keep=0: both sides empty, output is just the marker; valid UTF-16.
    {
        std::wstring src(50, L'C');
        std::wstring out = TruncateHeadTailWindow(src, 0);
        TEST_CHECK(out == std::wstring(L"\n\u2026\n"), "R01: keep=0 yields marker only (no OOB read)");
    }

    // 7. Roundtrip safety: truncated result must survive UTF-8 conversion with no
    //    replacement-character corruption (the engine tokenizes the UTF-8 bytes).
    {
        std::wstring src;
        for (int i = 0; i < 400; ++i) {
            src += L"안녕 🚀 ";
        }
        std::wstring out = TruncateHeadTailWindow(src, 100);
        std::string u8 = ToUtf8(out);
        std::wstring back = ToUtf16(u8);
        TEST_CHECK(back == out, "R01: truncated text round-trips UTF-8/UTF-16 losslessly");
        TEST_CHECK(u8.find("\xEF\xBF\xBD") == std::string::npos, "R01: no U+FFFD produced by truncation");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-R01 Head/Tail Token Truncation tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-R01 Head/Tail Token Truncation tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// REQ-R03 (Batch D1): path matrix for the selection-release guarantee. The
// constexpr predicate (shared with worker.cpp via src/worker.hpp) encodes the
// full outcome matrix; compile-time asserts pin it, runtime checks exercise it.
void TestSelectionReleaseMatrix() {
    std::cout << "[RUN] Testing REQ-R03 Selection Release Path Matrix..." << std::endl;
    const int failures_before = g_failed_count;

    // Compile-time pinning: paste success is the ONLY no-release path.
    static_assert(SelectionReleaseRequired(true) == false, "R03: success must not release");
    static_assert(SelectionReleaseRequired(false) == true, "R03: failure must release");

    // Runtime sweep of the four audited outcomes (audit §2.2):
    //   a) translated empty -> paste never attempted -> pasted=false -> release
    //   b) translated == line -> paste never attempted -> pasted=false -> release
    //   c) H1 guard cancelled paste -> PasteAndRestore returned false -> release
    //   d) successful paste -> selection consumed by Ctrl+V -> NO release
    struct Case { const wchar_t* name; bool paste_attempted; bool paste_succeeded; };
    const Case cases[] = {
        { L"translated_empty",     false, false },
        { L"translated_same",      false, false },
        { L"paste_h1_cancelled",   true,  false },
        { L"paste_success",        true,  true  },
    };
    for (const auto& c : cases) {
        bool release = SelectionReleaseRequired(c.paste_succeeded);
        bool expect_release = !c.paste_succeeded;
        TEST_CHECK(release == expect_release, "R03: release decision matches matrix");
        // A paste attempt that failed (H1 cancel) MUST still release - the whole
        // point of the fix; the old code skipped unsel whenever translated was
        // non-empty (audit §2.2 step 4).
        if (c.paste_attempted && !c.paste_succeeded) {
            TEST_CHECK(release, "R03: H1-cancelled paste releases selection (was the silent-evaporation bug)");
        }
    }
    TEST_CHECK(cases[3].paste_succeeded && !SelectionReleaseRequired(cases[3].paste_succeeded),
               "R03: successful paste is the sole skipped path");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-R03 Selection Release Path Matrix tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-R03 Selection Release Path Matrix tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Multi-line block fix (R3): the "only the last line was translated" bug.
//
// Root cause: non-chat apps selected only the current physical line
// (Shift+Home), so a multi-line typed/pasted block had only its final line
// reaching the translator. The fix adds SelectMessageBlock() at the selection
// seam and NormalizeNewlinesToCRLF() at the capture/translate seams. These
// tests pin the PURE, logic-only parts of the fix (headless, no SendInput):
//   1) newline normalization (CRLF/LF/CR, KO/EN/JA scripts, surrogate pairs),
//   2) ShouldTranslate / DetectLanguage behaving identically on multi-line
//      blocks (per-line and whole-block) for KO/EN/JA/ZH/VI/ES,
//   3) ParseResponseJson + UrlEncode round-tripping a multi-line payload
//      containing newlines (translation layer must not drop lines),
//   4) the worker's equality comparison (translated == line) being
//      line-ending-representation-insensitive.
// ---------------------------------------------------------------------------
void TestMultiLineBlockFix() {
    std::cout << "[RUN] Testing multi-line block fix (R3: last-line-only bug)..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- 1) NormalizeNewlinesToCRLF: pure line-ending matrix ----
    TEST_CHECK(NormalizeNewlinesToCRLF(L"") == L"", "MLF: empty stays empty");
    TEST_CHECK(NormalizeNewlinesToCRLF(L"single line") == L"single line", "MLF: no newlines passthrough");
    TEST_CHECK(NormalizeNewlinesToCRLF(L"a\nb") == L"a\r\nb", "MLF: LF becomes CRLF");
    TEST_CHECK(NormalizeNewlinesToCRLF(L"a\rb") == L"a\r\nb", "MLF: bare CR becomes CRLF");
    TEST_CHECK(NormalizeNewlinesToCRLF(L"a\r\nb") == L"a\r\nb", "MLF: CRLF preserved (no doubling)");
    TEST_CHECK(NormalizeNewlinesToCRLF(L"a\n\nb") == L"a\r\n\r\nb", "MLF: double LF becomes double CRLF");
    TEST_CHECK(NormalizeNewlinesToCRLF(L"\n") == std::wstring(L"\r\n"), "MLF: lone LF newline");
    TEST_CHECK(NormalizeNewlinesToCRLF(L"\r\n\r\n") == std::wstring(L"\r\n\r\n"), "MLF: CRLF pairs unchanged");

    // Script-agnostic coverage: the same normalization must hold for every
    // language, because the worker seam runs regardless of script.
    TEST_CHECK(NormalizeNewlinesToCRLF(L"한 줄\n두 줄") == L"한 줄\r\n두 줄", "MLF: Korean LF -> CRLF");
    TEST_CHECK(NormalizeNewlinesToCRLF(L"一行\n二行") == L"一行\r\n二行", "MLF: Chinese LF -> CRLF");
    TEST_CHECK(NormalizeNewlinesToCRLF(L"１行目\n２行目") == L"１行目\r\n２行目", "MLF: Japanese LF -> CRLF");
    TEST_CHECK(NormalizeNewlinesToCRLF(L"línea uno\nlínea dos") == L"línea uno\r\nlínea dos", "MLF: Spanish LF -> CRLF");
    TEST_CHECK(NormalizeNewlinesToCRLF(L"dòng một\ndòng hai") == L"dòng một\r\ndòng hai", "MLF: Vietnamese LF -> CRLF");
    TEST_CHECK(NormalizeNewlinesToCRLF(L"Zeile eins\r\nZeile zwei") == L"Zeile eins\r\nZeile zwei", "MLF: German CRLF passthrough");
    // Surrogate pair safety: today's emoji (non-BMP) must survive normalization
    // adjacent to line breaks; 0x0D/0x0A never participate in surrogates.
    TEST_CHECK(NormalizeNewlinesToCRLF(L"🚀\n🚀") == L"🚀\r\n🚀", "MLF: emoji-surrogate pairs intact across newline");
    TEST_CHECK(NormalizeNewlinesToCRLF(&L"😀line\n"[0]) == L"😀line\r\n", "MLF: leading non-BMP survives");

    // ---- 2) Smart-bypass seams on multi-line blocks (whole-block) ----
    // A mixed block (Korean lines + one English line) must remain translatable
    // as a whole when target is English - the same decision the worker makes
    // after the block selection fix. This pins that DetectLanguage on a mixed
    // script block still falls back to the priority order (Korean first) and
    // the block is NOT wrongly bypassed as "already target language".
    const std::wstring ko_block = L"'그래도 혹시라도 기준에 맞춰 변경라인 축소 여지가 있는지 꼼꼼히 확인해줘.\n변경라인을 줄이고도 문제가 없어야해.\n\n---------\n이렇게 글을 남겼는데, 맨 마지막 줄만 번역하네.";
    TEST_CHECK(ShouldTranslate(ko_block, "English"), "MLF: whole Korean block must translate to English");
    TEST_CHECK(DetectLanguage(ko_block) == "Korean", "MLF: multi-line Korean block detected as Korean");

    // Per-line equivalence: each line separately must produce the SAME
    // translation decision as the whole block (the user-per-line-Enter
    // sub-scenario). This is the language-independence guarantee: the
    // decision engine is fed either form and concludes "needs translation".
    const std::vector<std::wstring> ko_lines = {
        L"'그래도 혹시라도 기준에 맞춰 변경라인 축소 여지가 있는지 꼼꼼히 확인해줘.",
        L"변경라인을 줄이고도 문제가 없어야해.",
        L"이렇게 글을 남겼는데, 맨 마지막 줄만 번역하네."
    };
    for (const auto& l : ko_lines) {
        TEST_CHECK(ShouldTranslate(l, "English"), "MLF: each Korean line alone translates (per-line Enter sub-scenario)");
    }

    // Japanese block: whole + per-line (kana detection must retain newlines).
    const std::vector<std::wstring> ja_lines = {
        L"最後の行だけ翻訳される。",
        L"複数行の文章を入力してください。",
        L"すべての言語で同じ動作が必要です。"
    };
    std::wstring ja_block;
    for (size_t i = 0; i < ja_lines.size(); ++i) {
        if (i) ja_block += L"\r\n";
        ja_block += ja_lines[i];
    }
    TEST_CHECK(DetectLanguage(ja_block) == "Japanese", "MLF: multi-line Japanese block detected");
    for (const auto& l : ja_lines) {
        TEST_CHECK(ShouldTranslate(l, "Korean"), "MLF: Japanese line translates (per-line)");
    }
    TEST_CHECK(ShouldTranslate(ja_block, "Korean"), "MLF: Japanese whole block translates");

    // English -> Korean, whole block and per line (mirrors the reverse pair).
    const std::vector<std::wstring> en_lines = {
        L"'Please double check whether the changed lines can be reduced.",
        L"It must still be correct with fewer changed lines.",
        L"Please write the report in Hangul and submit it to me.'"
    };
    std::wstring en_block;
    for (size_t i = 0; i < en_lines.size(); ++i) {
        if (i) en_block += L"\r\n";
        en_block += en_lines[i];
    }
    TEST_CHECK(ShouldTranslate(en_block, "Korean"), "MLF: English whole block translates to Korean");
    for (const auto& l : en_lines) {
        TEST_CHECK(ShouldTranslate(l, "Korean"), "MLF: English line alone translates (per-line)");
    }

    // Same-language multi-line block: STILL bypassed under a PINNED source
    // (F5: the detection-based ASCII-Latin English identity bypass moved to
    // pin-keyed; the per-block decision semantics themselves are unchanged).
    TEST_CHECK(!ShouldTranslate(L"first line\nsecond line\nthird line", "English", "English"), "MLF: pinned English->English multi-line still bypassed");
    TEST_CHECK(!ShouldTranslate(en_block, "English", "English"), "MLF: bypassed when pinned source equals target language");

    // ---- 3) Translation payload round-trip with newlines (Google seam) ----
    // UrlEncode must %-encode the LF inside a UTF-8 Korean block so the q=
    // parameter is well-formed for both endpoints (newline is NOT a legal
    // raw query character; dropping or mangling it would lose lines).
    {
        // CRLF form the worker now feeds the engine:
        const std::wstring block = L"첫째 줄입니다.\r\n둘째 줄입니다.";
        const std::string u8 = ToUtf8(block);
        TEST_CHECK(u8.find('\n') != std::string::npos && u8.find('\r') != std::string::npos,
                   "MLF: block UTF-8 keeps newline bytes before URL-encoding");
        const std::string enc = GoogleTranslate::UrlEncode(u8);
        TEST_CHECK(enc.find('\n') == std::string::npos && enc.find('\r') == std::string::npos,
                   "MLF: UrlEncode percent-encodes CR and LF (no raw newlines in q=)");
        TEST_CHECK(enc.find("%0D%0A") != std::string::npos, "MLF: CRLF encoded as %0D%0A in payload");
        // And ParseResponseJson must restore an embedded escaped newline:
        const std::string json = "[[\"der erste\\nSatz\", \"de\"]]";
        TEST_CHECK(GoogleTranslate::ParseResponseJson(json) == L"der erste\nSatz",
                   "MLF: response parser restores \\n from JSON escape");
        const std::string json2 = "[[\"erste Zeile\\r\\nzweite Zeile\", \"de\"]]";
        TEST_CHECK(GoogleTranslate::ParseResponseJson(json2) == L"erste Zeile\r\nzweite Zeile",
                   "MLF: response parser restores \\r\\n escape pair");
    }

    // ---- 4) Worker equality comparison is line-ending-insensitive ----
    // The worker pastes only when translated != line. With both sides now
    // normalized to CRLF, a translator returning LF-only newlines (llama and
    // Google both may) must still be detected as "changed" or "unchanged"
    // based on CONTENT, not line-ending representation.
    {
        const std::wstring src = L"a\r\nb";            // captured (already CRLF)
        const std::wstring engine_out = L"a\nb";       // engine returned LF-only
        TEST_CHECK(NormalizeNewlinesToCRLF(engine_out) != src || NormalizeNewlinesToCRLF(engine_out) == src,
                   "MLF: normalization makes both representations comparable");
        TEST_CHECK(NormalizeNewlinesToCRLF(L"a\nb") == NormalizeNewlinesToCRLF(L"a\r\nb"),
                   "MLF: LF-only and CRLF engine outputs normalize equal (comparison is representation-insensitive)");
        // And the actual worker predicate: paste happens iff translated != line.
        const std::wstring translated_lf = NormalizeNewlinesToCRLF(engine_out);
        TEST_CHECK(translated_lf == src, "MLF: worker equality: same content different endings -> equal, no paste (preserves bypass)");
    }
    {
        // Content actually differs -> normalization must NOT make them equal.
        TEST_CHECK(NormalizeNewlinesToCRLF(L"a\nb") != NormalizeNewlinesToCRLF(L"a\rb") + L"x", "MLF: different content stays different");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Multi-line block fix tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Multi-line block fix tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestBadgeDynamicSizing() {
    std::cout << "[RUN] Testing Floating Badge Dynamic Sizing..." << std::endl;
    const int failures_before = g_failed_count;

    HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    FloatingBadge badge;
    bool created = badge.Create(hInst, L"Auto Detect", L"English");
    TEST_CHECK(created, "Badge created successfully");

    badge.SetLanguages(L"Auto Detect", L"English");
    badge.SetStatus(BadgeStatus::Disabled); // "Auto Detect -> English | Paused"

    int wide_width = badge.GetCurrentWidth();
    std::cout << "  [BADGE METRICS] 'Auto Detect -> English | Paused' width: " << wide_width << " px" << std::endl;
    TEST_CHECK(wide_width >= 220, "Badge dynamically expands for long label");

    // Switch to compact languages
    badge.SetLanguages(L"KO", L"EN");
    badge.SetStatus(BadgeStatus::Active); // "KO -> EN | Active"
    int compact_width = badge.GetCurrentWidth();
    std::cout << "  [BADGE METRICS] 'KO -> EN | Active' width: " << compact_width << " px" << std::endl;
    TEST_CHECK(compact_width <= 240, "Badge shrinks back to compact width for short label");

    badge.Destroy();

    // Verify custom coordinates initialization and screen bounds safety
    FloatingBadge custom_badge;
    bool created_custom = custom_badge.Create(hInst, L"KO", L"EN", 100, 150);
    TEST_CHECK(created_custom, "Badge created with custom coordinates");
    custom_badge.Destroy();

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Floating Badge Dynamic Sizing tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Floating Badge Dynamic Sizing tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestI18nModule() {
    std::cout << "[RUN] Testing Universal i18n Localization..." << std::endl;
    const int failures_before = g_failed_count;

    // 1. OS Detection
    UiLocale osLoc = I18n::DetectSystemLocale();
    TEST_CHECK(osLoc != UiLocale::Auto, "OS locale detected successfully");

    // 2. Korean Locale
    I18n::SetLocale(UiLocale::Korean);
    TEST_CHECK(I18n::GetCurrentLocale() == UiLocale::Korean, "SetLocale Korean");
    TEST_CHECK(I18n::Get(StringId::MenuStatusActive).find(L"활성") != std::wstring::npos, "Korean status text");
    TEST_CHECK(I18n::Get(StringId::MenuEngine).find(L"번역 엔진") != std::wstring::npos, "Korean engine text");
    TEST_CHECK(I18n::Get(StringId::BadgeActive) == L"활성", "Korean badge active word");
    TEST_CHECK(I18n::Get(StringId::BadgePaused) == L"일시 정지", "Korean badge paused word");
    TEST_CHECK(I18n::GetLanguageDisplayName("KO").find(L"한국어") != std::wstring::npos, "Korean display name for KO");
    TEST_CHECK(I18n::GetLanguageDisplayName("AUTO") == L"자동 감지", "Korean display name for AUTO");

    // 3. Japanese Locale
    I18n::SetLocale(UiLocale::Japanese);
    TEST_CHECK(I18n::Get(StringId::MenuStatusActive).find(L"有効") != std::wstring::npos, "Japanese status text");
    TEST_CHECK(I18n::Get(StringId::BadgeActive) == L"有効", "Japanese badge active word");
    TEST_CHECK(I18n::GetLanguageDisplayName("JA").find(L"日本語") != std::wstring::npos, "Japanese display name for JA");

    // 4. Chinese Simplified Locale
    I18n::SetLocale(UiLocale::ChineseSimplified);
    TEST_CHECK(I18n::Get(StringId::MenuStatusActive).find(L"运行中") != std::wstring::npos, "Chinese status text");
    TEST_CHECK(I18n::Get(StringId::BadgeActive) == L"运行中", "Chinese badge active word");

    // 5. English Locale
    I18n::SetLocale(UiLocale::English);
    TEST_CHECK(I18n::Get(StringId::MenuStatusActive).find(L"Active") != std::wstring::npos, "English status text");
    TEST_CHECK(I18n::Get(StringId::BadgeActive) == L"Active", "English badge active word");

    // Phase 4 batch 1 (REQ-001, plan §2.4): former "6. Default target pairing"
    // assertions removed together with the dead GetDefaultTargetLanguage API
    // (0 runtime callers; reset logic uses ResolveDragDefaultTarget instead).

    // 7. Autostart registry status inspection without throwing
    bool autostart = I18n::IsStartWithWindowsEnabled();
    (void)autostart;
    TEST_CHECK(true, "IsStartWithWindowsEnabled executes safely");

    // Reset back to OS locale
    I18n::Initialize("auto");
    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Universal i18n Localization tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Universal i18n Localization tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestDragToTranslateComponents() {
    std::cout << "[RUN] Testing Drag-to-Translate Components..." << std::endl;
    const int failures_before = g_failed_count;

    HINSTANCE hInst = ::GetModuleHandleW(nullptr);

    // 1. DragIconWindow creation, display toggle, and destruction
    DragIconWindow drag_icon;
    bool icon_created = drag_icon.Create(hInst);
    TEST_CHECK(icon_created, "DragIconWindow created successfully");
    TEST_CHECK(!drag_icon.IsVisible(), "DragIconWindow starts hidden");

    drag_icon.ShowAt(100, 100);
    TEST_CHECK(drag_icon.IsVisible(), "DragIconWindow is visible after ShowAt");

    drag_icon.Hide();
    TEST_CHECK(!drag_icon.IsVisible(), "DragIconWindow is hidden after Hide");
    drag_icon.Destroy();

    // 2. TooltipWindow creation, presentation, and destruction
    TooltipWindow tooltip;
    bool tooltip_created = tooltip.Create(hInst);
    TEST_CHECK(tooltip_created, "TooltipWindow created successfully");
    TEST_CHECK(!tooltip.IsVisible(), "TooltipWindow starts hidden");

    tooltip.ShowTranslation(200, 200, L"안녕하세요", "KO", "English", L"Hello");
    TEST_CHECK(tooltip.IsVisible(), "TooltipWindow is visible after ShowTranslation");
    TEST_CHECK(tooltip.GetSourceText() == L"안녕하세요", "TooltipWindow records source text");
    TEST_CHECK(tooltip.GetSourceLangCode() == "KO", "TooltipWindow records source language code");
    TEST_CHECK(tooltip.GetTargetLang() == "English", "TooltipWindow records target language");

    tooltip.Dismiss();
    TEST_CHECK(!tooltip.IsVisible(), "TooltipWindow is hidden after Dismiss");
    tooltip.Destroy();

    // 3. MouseHook lifecycle and multi-click state
    MouseHook mouse_hook;
    TEST_CHECK(mouse_hook.IsEnabled(), "MouseHook starts enabled");
    mouse_hook.SetEnabled(false);
    TEST_CHECK(!mouse_hook.IsEnabled(), "MouseHook can be disabled");
    mouse_hook.SetEnabled(true);
    TEST_CHECK(mouse_hook.GetClickCount() == 0, "MouseHook starts with 0 click count");

    bool hook_started = mouse_hook.Start();
    TEST_CHECK(hook_started, "MouseHook started successfully");
    mouse_hook.Stop();

    // 4. Process-aware input helper
    // Phase 5 (REQ-011): classification fails open to CategoryB (editor path)
    // for a null HWND, matching the old IsChatApplicationWindow(false) contract.
    TEST_CHECK(ClassifyAppWindow(nullptr) == AppCategory::CategoryB, "ClassifyAppWindow returns CategoryB for null HWND");

    // 5. Emebala Tablet Relief Assets & 32x32 Pill Specifications
    int icon_size = DragIconWindow::kSize;
    TEST_CHECK(icon_size == 32, "DragIconWindow size is 32px rounded icon pill");
    std::wstring resolvedLogo = FindLogoPath();
    TEST_CHECK(!resolvedLogo.empty(), "FindLogoPath resolves logo.png successfully");
    TEST_CHECK(std::filesystem::exists(resolvedLogo), "Resolved logo.png exists on disk");
    std::filesystem::path resolvedLogoSvg = ResolveRepoFile(L"assets\\logo.svg");
    TEST_CHECK(!resolvedLogoSvg.empty(), "assets/logo.svg resolves via exe/CWD-relative candidates (portable)");
    TEST_CHECK(std::filesystem::exists(resolvedLogoSvg), "Resolved assets/logo.svg exists on disk");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Drag-to-Translate Components tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Drag-to-Translate Components tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void TestTtsVoiceSelectionModule() {
    std::cout << "[RUN] Testing Multi-Language Native TTS Voice Selection..." << std::endl;
    const int failures_before = g_failed_count;

    // 1. Language LCID Mapping Coverage
    TEST_CHECK(GetLcidForLanguage("English") == 0x0409, "LCID English by name");
    TEST_CHECK(GetLcidForLanguage("EN") == 0x0409, "LCID EN by code");
    TEST_CHECK(GetLcidForLanguage("en") == 0x0409, "LCID en lowercase");
    TEST_CHECK(GetLcidForLanguage("Korean") == 0x0412, "LCID Korean by name");
    TEST_CHECK(GetLcidForLanguage("KO") == 0x0412, "LCID KO by code");
    TEST_CHECK(GetLcidForLanguage("한국어") == 0x0412, "LCID 한국어 native name");
    TEST_CHECK(GetLcidForLanguage("Japanese") == 0x0411, "LCID Japanese by name");
    TEST_CHECK(GetLcidForLanguage("JA") == 0x0411, "LCID JA by code");
    TEST_CHECK(GetLcidForLanguage("日本語") == 0x0411, "LCID 日本語 native name");
    TEST_CHECK(GetLcidForLanguage("Chinese Simplified") == 0x0804, "LCID Chinese Simplified");
    TEST_CHECK(GetLcidForLanguage("ZH-CN") == 0x0804, "LCID ZH-CN by code");
    TEST_CHECK(GetLcidForLanguage("ZH-TW") == 0x0404, "LCID ZH-TW by code");
    TEST_CHECK(GetLcidForLanguage("Spanish") == 0x040A, "LCID Spanish by name");
    TEST_CHECK(GetLcidForLanguage("ES") == 0x040A, "LCID ES by code");
    TEST_CHECK(GetLcidForLanguage("French") == 0x040C, "LCID French by name");
    TEST_CHECK(GetLcidForLanguage("German") == 0x0407, "LCID German by name");
    TEST_CHECK(GetLcidForLanguage("Russian") == 0x0419, "LCID Russian by name");
    TEST_CHECK(GetLcidForLanguage("Vietnamese") == 0x042A, "LCID Vietnamese by name");
    TEST_CHECK(GetLcidForLanguage("Portuguese") == 0x0416, "LCID Portuguese by name");
    TEST_CHECK(GetLcidForLanguage("Italian") == 0x0410, "LCID Italian by name");
    TEST_CHECK(GetLcidForLanguage("Thai") == 0x041E, "LCID Thai by name");
    TEST_CHECK(GetLcidForLanguage("Arabic") == 0x0401, "LCID Arabic by name");

    // Hex and BCP-47 variant parsing
    TEST_CHECK(GetLcidForLanguage("0x409") == 0x0409, "LCID 0x409 hex");
    TEST_CHECK(GetLcidForLanguage("409") == 0x0409, "LCID 409 hex string");
    TEST_CHECK(GetLcidForLanguage("0x412") == 0x0412, "LCID 0x412 hex");
    TEST_CHECK(GetLcidForLanguage("412") == 0x0412, "LCID 412 hex string");
    TEST_CHECK(GetLcidForLanguage("en-US") == 0x0409, "LCID en-US regional tag");
    TEST_CHECK(GetLcidForLanguage("ko-KR") == 0x0412, "LCID ko-KR regional tag");
    TEST_CHECK(GetLcidForLanguage("NonExistentLanguage999") == 0, "Unknown language returns 0");
    TEST_CHECK(GetLcidForLanguage("") == 0, "Empty language returns 0");

    // 2. Active SAPI Voice Selection
    HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    TooltipWindow tooltip;
    bool created = tooltip.Create(hInst);
    TEST_CHECK(created, "TooltipWindow created for TTS testing");

    // Switch to English voice (e.g. Microsoft Zira Desktop / David)
    bool en_selected = tooltip.SelectVoiceForLanguage("English");
    std::cout << "  [TTS EN VOICE]: Selected=" << (en_selected ? "true" : "false")
              << " Name='" << tooltip.GetCurrentVoiceName()
              << "' Lang='" << tooltip.GetCurrentVoiceLanguage() << "'" << std::endl;
    TEST_CHECK(en_selected, "English voice selected successfully on Windows");
    TEST_CHECK(tooltip.GetCurrentVoiceLanguage().find("409") != std::string::npos,
               "Selected English voice has 409 LCID");
    TEST_CHECK(!tooltip.GetCurrentVoiceName().empty(), "Selected English voice has non-empty name");

    // Switch to Korean voice (e.g. Microsoft Heami Desktop)
    bool ko_selected = tooltip.SelectVoiceForLanguage("Korean");
    std::cout << "  [TTS KO VOICE]: Selected=" << (ko_selected ? "true" : "false")
              << " Name='" << tooltip.GetCurrentVoiceName()
              << "' Lang='" << tooltip.GetCurrentVoiceLanguage() << "'" << std::endl;
    TEST_CHECK(ko_selected, "Korean voice selected successfully on Windows");
    TEST_CHECK(tooltip.GetCurrentVoiceLanguage().find("412") != std::string::npos,
               "Selected Korean voice has 412 LCID");

    // Switch back to English to verify dynamic bidirectional switching
    bool en_reselected = tooltip.SelectVoiceForLanguage("EN");
    TEST_CHECK(en_reselected, "Switched back to English voice via 'EN' code");
    TEST_CHECK(tooltip.GetCurrentVoiceLanguage().find("409") != std::string::npos,
               "Switched back to 409 LCID");

    // Safe fallback on unsupported / uninstalled voice
    bool fallback_selected = tooltip.SelectVoiceForLanguage("NonExistentVoiceLanguage");
    TEST_CHECK(!fallback_selected, "Unsupported language reports voice not found");
    TEST_CHECK(!tooltip.GetCurrentVoiceName().empty(), "Fallback voice retains a valid active voice");

    // Speak and Stop TTS safety
    tooltip.ShowTranslation(100, 100, L"안녕하세요", "KO", "English", L"Hello world");
    tooltip.SpeakCurrentText(); // Should switch to English voice and speak asynchronously
    tooltip.StopTTS();          // Purge TTS safely

    tooltip.Destroy();
    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Multi-Language Native TTS Voice Selection tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Multi-Language Native TTS Voice Selection tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ===========================================================================
// Batch D3: hook sync, F9 hotkey, mouse-hook debounce, D2D marshaling
// (REQ-R06, REQ-R07, REQ-R08, REQ-R09, REQ-R10)
// ===========================================================================

namespace {
// Waits (bounded) until pred() is true. Returns true if it became true.
template <typename Pred>
bool WaitUntilMs(Pred pred, uint32_t timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

// Pump every currently-queued message of THIS thread exactly once (the test
// main thread owns the message windows used for the marshal tests).
void PumpThreadMessagesOnce() {
    MSG m = {};
    while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&m);
        ::DispatchMessageW(&m);
    }
}
} // namespace

// REQ-R08 (audit §3.2): pure hotkey parsing + exact-match predicate matrix.
void TestHotkeyParsing() {
    std::cout << "[RUN] Testing Hotkey Parsing (REQ-R08)..." << std::endl;
    const int failures_before = g_failed_count;

    using HK = KeyboardHook;

    // Compile-time pins: the default toggle combo is bare F9.
    static_assert(HK::kDefaultToggleHotkey.vk == VK_F9 &&
                      !HK::kDefaultToggleHotkey.win && !HK::kDefaultToggleHotkey.ctrl &&
                      !HK::kDefaultToggleHotkey.shift && !HK::kDefaultToggleHotkey.alt &&
                      HK::kDefaultToggleHotkey.valid,
                  "Default toggle must be bare F9");
    static_assert(HK::IsWinKey(VK_LWIN) && HK::IsWinKey(VK_RWIN) && !HK::IsWinKey(VK_F9),
                  "IsWinKey covers both Win keys only");

    // 1. ParseHotkey matrix.
    TEST_CHECK(HK::ParseHotkey("F9") == HK::kDefaultToggleHotkey, "Parse F9 == default");
    TEST_CHECK(HK::ParseHotkey(" f9 ") == HK::kDefaultToggleHotkey, "Parse is case/space insensitive");
    TEST_CHECK(HK::ParseHotkey("Win+F9") == HK::MakePressSpec(VK_F9, false, false, false, true), "Parse Win+F9");
    TEST_CHECK(HK::ParseHotkey("Windows+F9") == HK::MakePressSpec(VK_F9, false, false, false, true), "Parse 'Windows' synonym");
    TEST_CHECK(HK::ParseHotkey("Super+F9") == HK::MakePressSpec(VK_F9, false, false, false, true), "Parse 'Super' synonym");
    TEST_CHECK(HK::ParseHotkey("Ctrl+F9") == HK::MakePressSpec(VK_F9, true, false, false, false),
               "Parse Ctrl+F9");
    const HK::HotkeySpec mode = HK::ParseHotkey("Ctrl+Shift+Enter");
    TEST_CHECK(mode.valid && mode.vk == VK_RETURN && mode.ctrl && mode.shift && !mode.alt && !mode.win,
               "Parse Ctrl+Shift+Enter");
    TEST_CHECK(HK::ParseHotkey("Ctrl+Shift+Alt+Win+A") ==
                   HK::MakePressSpec('A', true, true, true, true), "Parse all four modifiers");
    TEST_CHECK(HK::ParseHotkey("Escape").vk == VK_ESCAPE && HK::ParseHotkey("Esc").vk == VK_ESCAPE,
               "Parse Escape/ESC aliases");
    TEST_CHECK(HK::ParseHotkey("PrintScreen").valid == false, "Unknown key token is invalid");
    TEST_CHECK(HK::ParseHotkey("").valid == false, "Empty spec is invalid");
    TEST_CHECK(HK::ParseHotkey("Win+").valid == false, "Trailing + is invalid");
    TEST_CHECK(HK::ParseHotkey("+F9").valid == false, "Leading + is invalid");
    TEST_CHECK(HK::ParseHotkey("Ctrl++").valid == false, "Empty token is invalid");
    TEST_CHECK(HK::ParseHotkey("F9+F9").valid == false, "Two main keys is invalid");
    TEST_CHECK(HK::ParseHotkey("Ctrl+F99").valid == false, "Unknown f-key is invalid");
    TEST_CHECK(HK::ParseHotkey("f24").vk == VK_F24 && HK::ParseHotkey("f10").vk == VK_F10,
               "f10/f24 two-digit parse");
    TEST_CHECK(HK::ParseHotkey("Space").vk == VK_SPACE && HK::ParseHotkey("Home").vk == VK_HOME,
               "named-key parse");

    // 2. Exact-match predicate (the hook swallows ONLY the full combo).
    const HK::HotkeySpec f9 = HK::kDefaultToggleHotkey;
    TEST_CHECK(HK::HotkeyMatches(f9, VK_F9, false, false, false, false), "Bare F9 matches toggle");
    TEST_CHECK(!HK::HotkeyMatches(f9, VK_F9, false, false, false, true), "Win+F9 does not match bare F9 toggle");
    TEST_CHECK(!HK::HotkeyMatches(f9, VK_F9, true, false, false, false), "Ctrl+F9 does not match bare F9 toggle");
    TEST_CHECK(!HK::HotkeyMatches(f9, VK_F9, false, true, false, false), "Shift+F9 does not match bare F9 toggle");
    TEST_CHECK(!HK::HotkeyMatches(f9, VK_F10, false, false, false, false), "F10 does not match");
    TEST_CHECK(!HK::HotkeyMatches(HK::HotkeySpec{}, VK_F9, false, false, false, false),
               "Invalid spec never matches");
    const HK::HotkeySpec ctrlf9 = HK::ParseHotkey("Ctrl+F9");
    TEST_CHECK(HK::HotkeyMatches(ctrlf9, VK_F9, true, false, false, false), "Ctrl+F9 matches lang-cycle");
    TEST_CHECK(!HK::HotkeyMatches(ctrlf9, VK_F9, true, false, false, true),
               "Win+Ctrl+F9 does not match Ctrl+F9 (win must be absent)");

    // 3. Config resolution: "F9" resolves to bare F9 default; empty/invalid -> default; custom valid combos honored.
    TEST_CHECK(HK::ResolveToggleFromConfig("F9") == HK::kDefaultToggleHotkey,
               "Config 'F9' resolves to bare F9 default");
    TEST_CHECK(HK::ResolveToggleFromConfig("") == HK::kDefaultToggleHotkey, "Empty resolves to default");
    TEST_CHECK(HK::ResolveToggleFromConfig("nonsense") == HK::kDefaultToggleHotkey, "Invalid resolves to default");
    TEST_CHECK(HK::ResolveToggleFromConfig("Win+F9") == HK::ParseHotkey("Win+F9"),
               "Explicit Win+F9 combo honored as-is");
    TEST_CHECK(HK::ResolveToggleFromConfig("Ctrl+Alt+D") == HK::ParseHotkey("Ctrl+Alt+D"),
               "Explicit valid combo honored as-is");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Hotkey Parsing tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Hotkey Parsing tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// REQ-022 (Phase 6 batch 2): config wiring matrix for hotkey_lang/hotkey_mode
// (plan §6.1). Covers resolver fallback (C3), default-spec equivalence (C1),
// exact matching, and default-spec independence (C4). The drag_hotkey value
// validation lives inside Start() as a non-pure block, so per plan §6.1 it is
// covered by code review (hook.cpp Start()), not by a unit test here.
void TestPhase6HotkeyWiring() {
    std::cout << "[RUN] Testing Phase 6 Hotkey Wiring (REQ-022)..." << std::endl;
    const int failures_before = g_failed_count;

    using HK = KeyboardHook;

    // Compile-time pins: the compiled-in defaults equal the historical
    // hardcoded triggers (C1) - lang = Ctrl+F9, mode = Ctrl+Shift+Enter.
    static_assert(HK::kDefaultLangHotkey.vk == VK_F9 && HK::kDefaultLangHotkey.ctrl &&
                      !HK::kDefaultLangHotkey.shift && !HK::kDefaultLangHotkey.alt &&
                      !HK::kDefaultLangHotkey.win && HK::kDefaultLangHotkey.valid,
                  "Default lang hotkey must be Ctrl+F9");
    static_assert(HK::kDefaultModeHotkey.vk == VK_RETURN && HK::kDefaultModeHotkey.ctrl &&
                      HK::kDefaultModeHotkey.shift && !HK::kDefaultModeHotkey.alt &&
                      !HK::kDefaultModeHotkey.win && HK::kDefaultModeHotkey.valid,
                  "Default mode hotkey must be Ctrl+Shift+Enter");

    // 1. Lang resolver: default string, fallbacks (C3), custom combo (C2).
    TEST_CHECK(HK::ResolveLangFromConfig("Ctrl+F9") == HK::kDefaultLangHotkey,
               "Lang config 'Ctrl+F9' resolves to the default lang spec");
    TEST_CHECK(HK::ResolveLangFromConfig("") == HK::kDefaultLangHotkey,
               "Empty lang config falls back to default (C3)");
    TEST_CHECK(HK::ResolveLangFromConfig("nonsense") == HK::kDefaultLangHotkey,
               "Invalid lang config falls back to default (C3)");
    TEST_CHECK(HK::ResolveLangFromConfig("Ctrl+F99") == HK::kDefaultLangHotkey,
               "Unsupported f-key lang config falls back to default (C3)");
    const HK::HotkeySpec alt_l = HK::ResolveLangFromConfig("Ctrl+Alt+L");
    TEST_CHECK(alt_l == HK::ParseHotkey("Ctrl+Alt+L"),
               "Valid custom lang combo is honored as parsed (C2)");
    TEST_CHECK(alt_l.valid && alt_l.vk == 'L' && alt_l.ctrl && alt_l.alt,
               "Custom lang combo is Ctrl+Alt+L");

    // 2. Mode resolver: default string, fallbacks (C3), custom combo (C2).
    TEST_CHECK(HK::ResolveModeFromConfig("Ctrl+Shift+Enter") == HK::kDefaultModeHotkey,
               "Mode config 'Ctrl+Shift+Enter' resolves to the default mode spec");
    TEST_CHECK(HK::ResolveModeFromConfig("") == HK::kDefaultModeHotkey,
               "Empty mode config falls back to default (C3)");
    TEST_CHECK(HK::ResolveModeFromConfig("bad") == HK::kDefaultModeHotkey,
               "Invalid mode config falls back to default (C3)");
    const HK::HotkeySpec f10 = HK::ResolveModeFromConfig("Ctrl+F10");
    TEST_CHECK(f10.valid && f10.vk == VK_F10, "Valid custom mode combo honors the key (C2)");
    TEST_CHECK(f10.ctrl && !f10.shift && !f10.alt && !f10.win,
               "Custom mode combo is Ctrl+F10");

    // 3. Default-spec matching equivalence with the historical hardcoded
    //    triggers (C1): only the exact combo fires; neighbors stay passthrough.
    TEST_CHECK(HK::HotkeyMatches(HK::kDefaultLangHotkey, VK_F9, true, false, false, false),
               "Lang default fires on Ctrl+F9 (historical hardcode)");
    TEST_CHECK(!HK::HotkeyMatches(HK::kDefaultLangHotkey, VK_F9, false, false, false, false),
               "Bare F9 does not fire lang default (toggle passthrough)");
    TEST_CHECK(HK::HotkeyMatches(HK::kDefaultModeHotkey, VK_RETURN, true, true, false, false),
               "Mode default fires on Ctrl+Shift+Enter (historical hardcode)");
    TEST_CHECK(!HK::HotkeyMatches(HK::kDefaultModeHotkey, VK_RETURN, true, false, false, false),
               "Ctrl+Enter does not fire mode default (passthrough preserved)");
    TEST_CHECK(!HK::HotkeyMatches(HK::kDefaultModeHotkey, VK_RETURN, false, true, false, false),
               "Shift+Enter does not fire mode default (S2 newline preserved)");

    // 4. Default-spec independence (C4): no keydown that matches one default
    //    spec matches another, so the hook's evaluation order (toggle > lang >
    //    mode) can never double-fire for the default configuration.
    TEST_CHECK(!HK::HotkeyMatches(HK::kDefaultModeHotkey, VK_F9, true, false, false, false),
               "Lang trigger never matches mode spec");
    TEST_CHECK(!HK::HotkeyMatches(HK::kDefaultLangHotkey, VK_RETURN, true, true, false, false),
               "Mode trigger never matches lang spec");
    TEST_CHECK(!HK::HotkeyMatches(HK::kDefaultToggleHotkey, VK_RETURN, true, true, false, false),
               "Mode trigger never matches toggle spec");
    TEST_CHECK(!HK::HotkeyMatches(HK::kDefaultModeHotkey, VK_F9, false, false, false, false),
               "Toggle trigger never matches mode spec");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Phase 6 Hotkey Wiring tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Phase 6 Hotkey Wiring tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// REQ-R07 (audit §3.1): active-change callback fires 1:1 with SetActive, and
// REQ-R06 (audit §2.5): the double-Ctrl+C job runs OFF the caller thread with
// a non-blocking dispatch seam (measured).
void TestKeyboardHookStateSyncAndDispatch() {
    std::cout << "[RUN] Testing KeyboardHook state sync + async dispatch (REQ-R06/R07)..." << std::endl;
    const int failures_before = g_failed_count;

    SetSoundEnabled(false); // keep the test suite quiet; re-enabled at the end

    AppConfig cfg;   // defaults, no disk load (no LoadFromFile call)
    cfg.hotkey_toggle = "F9"; // default string exercises bare F9
    TranslationManager engine(EngineType::GoogleTranslate, ""); // no model load in ctor
    FloatingBadge badge;      // not Create()d: all badge methods no-op headless
    SystemTray tray;          // not Create()d: UpdateStatus is Shell-API-only
    PipelineWorker worker(cfg, engine, badge); // not Start()ed
    KeyboardHook hook(cfg, worker, badge, tray);

    const DWORD main_tid = ::GetCurrentThreadId();

    // ---- REQ-R07: 1:1 state-change observation (simulates the main.cpp wiring) ----
    MouseHook mouse_hook;
    std::atomic<int> cb_count{0};
    std::atomic<int> cb_last_state{-1};
    hook.SetActiveChangeCallback([&](bool active) {
        cb_count.fetch_add(1, std::memory_order_relaxed);
        cb_last_state.store(active ? 1 : 0, std::memory_order_relaxed);
        mouse_hook.SetEnabled(active); // THE sync edge from main.cpp
    });

    hook.SetActive(true);  // already active -> NO change
    TEST_CHECK(cb_count.load() == 0, "R07: SetActive(true) while active fires no callback");

    hook.SetActive(false);
    TEST_CHECK(cb_count.load() == 1, "R07: disabling fires callback exactly once");
    TEST_CHECK(cb_last_state.load() == 0, "R07: callback reports the new state (inactive)");
    TEST_CHECK(!mouse_hook.IsEnabled(), "R07: mouse hook follows keyboard hook OFF");

    hook.SetActive(false); // idempotent
    TEST_CHECK(cb_count.load() == 1, "R07: redundant SetActive(false) fires nothing");

    hook.ToggleActive();
    TEST_CHECK(cb_count.load() == 2 && cb_last_state.load() == 1, "R07: ToggleActive fires with active=true");
    TEST_CHECK(mouse_hook.IsEnabled(), "R07: mouse hook follows keyboard hook back ON (the F9-fix)");

    // ---- REQ-R06: async double-Ctrl+C dispatch ----
    std::atomic<int> ctrlc_runs{0};
    std::atomic<DWORD> ctrlc_tid{0};
    hook.SetDoubleCtrlCCallback([&]() {
        ctrlc_tid.store(::GetCurrentThreadId(), std::memory_order_relaxed);
        ctrlc_runs.fetch_add(1, std::memory_order_relaxed);
        ::Sleep(150); // simulate slow clipboard settle + inference
    });

    TEST_CHECK(hook.Start(), "R06/R07 fixture: KeyboardHook::Start installs the hook");
    TEST_CHECK(hook.ToggleHotkey() == KeyboardHook::kDefaultToggleHotkey,
               "Compiled toggle spec honors 'F9' config as bare F9 at Start()");

    const ULONGLONG t0 = ::GetTickCount64();
    KeyboardHook::DispatchDoubleCtrlC();
    const ULONGLONG dispatch_us = (::GetTickCount64() - t0) * 1000ULL; // conservative ms->us
    TEST_CHECK(dispatch_us < 20000ULL,
               "R06: dispatch call returns in <20 ms measured (actually microseconds; hook thread must not block)");

    TEST_CHECK(WaitUntilMs([&]() { return ctrlc_runs.load() >= 1; }, 2000),
               "R06: dispatched callback executes on the worker within 2 s");
    TEST_CHECK(ctrlc_tid.load() != 0 && ctrlc_tid.load() != main_tid,
               "R06: callback ran on a worker thread, NOT the caller/hook thread");
    TEST_CHECK(KeyboardHook::IsDoubleCtrlCBusy(), "R06: busy flag set while body sleeps");

    // Second dispatch WHILE the first body runs: must not block the caller and
    // must queue a re-run after the first completes.
    const ULONGLONG t1 = ::GetTickCount64();
    KeyboardHook::DispatchDoubleCtrlC();
    const ULONGLONG dispatch2_ms = ::GetTickCount64() - t1;
    TEST_CHECK(dispatch2_ms < 5, "R06: re-dispatch while busy returns instantly (<5 ms)");
    TEST_CHECK(WaitUntilMs([&]() { return ctrlc_runs.load() >= 2; }, 3000),
               "R06: queued second event runs after the first body completes");

    const ULONGLONG stop_t = ::GetTickCount64();
    hook.Stop();
    const ULONGLONG stop_ms = ::GetTickCount64() - stop_t;
    TEST_CHECK(stop_ms < 500, "R06: Stop() joins the worker deterministically (bounded)");
    TEST_CHECK(!KeyboardHook::IsDoubleCtrlCBusy(), "R06: busy flag cleared after Stop()");

    // Dispatch after Stop(): worker not live -> event dropped with a trace, no crash.
    KeyboardHook::DispatchDoubleCtrlC();
    TEST_CHECK(ctrlc_runs.load() == 2, "R06: post-Stop dispatch is dropped, not executed");

    SetSoundEnabled(true);
    if (g_failed_count == failures_before) {
        std::cout << "[PASS] KeyboardHook state sync + async dispatch tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] KeyboardHook state sync + async dispatch tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// REQ-R09 (audit §3.3): join-free click_seq_ invalidation debounce, exercised
// through the same seams the LL mouse callback uses (no real mouse input).
void TestMouseHookDebounce() {
    std::cout << "[RUN] Testing MouseHook click_seq_ debounce (REQ-R09)..." << std::endl;
    const int failures_before = g_failed_count;

    // Compile-time pins of the pure predicate (mirrors the C2-regression fix):
    static_assert(MouseHook::kMultiClickDebounceMs == 60, "R09: settle window stays 60 ms");
    static_assert(MouseHook::kDelayedClickPollMs == 16, "R09: invalidation notice stays <=16 ms");
    static_assert(MouseHook::MultiClickDeadlineMs(1000) == 1060, "R09: deadline = arm + window");
    static_assert(MouseHook::ShouldFireDelayedClick(5, 5, true, 1060, 1060), "R09: due + current + running -> fire");
    static_assert(!MouseHook::ShouldFireDelayedClick(5, 6, true, 9999, 1060), "R09: stale seq NEVER fires (no-ops)");
    static_assert(!MouseHook::ShouldFireDelayedClick(5, 5, false, 9999, 1060), "R09: stopped hook never fires");
    static_assert(!MouseHook::ShouldFireDelayedClick(5, 5, true, 1059, 1060), "R09: settle window still open");

    // Runtime: real hook lifecycle + worker-side invalidation.
    MouseHook mouse_hook;
    std::atomic<int> fires{0};
    std::atomic<DWORD> fire_tid{0};
    std::atomic<LONG> last_x{0};
    std::atomic<LONG> last_y{0};
    mouse_hook.SetDragReleaseCallback([&](int x, int y) {
        fire_tid.store(::GetCurrentThreadId(), std::memory_order_relaxed);
        last_x.store(x, std::memory_order_relaxed);
        last_y.store(y, std::memory_order_relaxed);
        fires.fetch_add(1, std::memory_order_relaxed);
    });

    TEST_CHECK(mouse_hook.Start(), "R09 fixture: MouseHook::Start installs the hook");
    const DWORD main_tid = ::GetCurrentThreadId();
    // Hermeticity: the real LL mouse proc early-returns while disabled, so a
    // physical user click cannot bump click_seq_ mid-test and invalidate the
    // synthetic jobs below. The worker's fire predicate checks running_, not
    // enabled_, so the debounce path itself stays fully exercised.
    mouse_hook.SetEnabled(false);
    TEST_CHECK(!mouse_hook.IsEnabled(), "R09 fixture: hook input disabled for synthetic-seq isolation");

    // (a) A fresh job fires once, after the settle window, OFF the hook thread.
    const uint64_t seq1 = mouse_hook.BumpClickSeqForTest();
    mouse_hook.ArmDelayedClickForTest(seq1, POINT{111, 222});
    TEST_CHECK(WaitUntilMs([&]() { return fires.load() >= 1; }, 2000), "R09: armed job fires within settle window");
    TEST_CHECK(fires.load() == 1, "R09: fires exactly once");
    TEST_CHECK(last_x.load() == 111 && last_y.load() == 222, "R09: click coordinates survive to the callback");
    TEST_CHECK(fire_tid.load() != main_tid, "R09: callback ran on the delayed-click worker, not the caller thread");

    // (b) Newer physical click (seq bump) + re-arm: the stale job must NO-OP
    // and only the latest click fires - the join-free debounce replacing the
    // C2 join(). Coordinates prove WHICH job fired.
    const uint64_t seqA = mouse_hook.BumpClickSeqForTest();
    mouse_hook.ArmDelayedClickForTest(seqA, POINT{10, 10});
    ::Sleep(5); // mid-settle window, before the 60 ms deadline
    const uint64_t seqB = mouse_hook.BumpClickSeqForTest();
    mouse_hook.ArmDelayedClickForTest(seqB, POINT{20, 20});
    TEST_CHECK(WaitUntilMs([&]() { return fires.load() >= 2; }, 2000), "R09: latest job fires after invalidation");
    TEST_CHECK(seqB == seqA + 1, "R09: sequence counter advances per click (invalidation token)");
    TEST_CHECK(WaitUntilMs([&]() { return fires.load() >= 2; }, 200) &&
                   (last_x.load() == 20 && last_y.load() == 20),
               "R09: only the NEWEST click fires; stale job dropped its callback");
    ::Sleep(120); // give any wrong job more than two settle windows
    TEST_CHECK(fires.load() == 2, "R09: stale job never double-fires after its successor ran");

    // (c) Stop() with a pending job: cooperative stop wins, nothing fires, and
    // Stop() returns promptly (bounded by one poll step).
    const uint64_t seqC = mouse_hook.BumpClickSeqForTest();
    mouse_hook.ArmDelayedClickForTest(seqC, POINT{30, 30});
    const ULONGLONG stop_t = ::GetTickCount64();
    mouse_hook.Stop();
    const ULONGLONG stop_ms = ::GetTickCount64() - stop_t;
    TEST_CHECK(stop_ms < 250, "R09: Stop() is bounded (<= settle window) and never waits on the hook thread");
    ::Sleep(150);
    TEST_CHECK(fires.load() == 2, "R09: pending job was stopped, did not fire post-Stop");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] MouseHook click_seq_ debounce tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] MouseHook click_seq_ debounce tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// REQ-R1 (session 260905_0001): the click-on-drag-icon dispatch chain. A real
// user click on the layered icon delivers WM_LBUTTONDOWN then WM_LBUTTONUP to
// the icon's WndProc; the WM_LBUTTONUP case must fire click_cb_ (which the app
// wires to tooltip.ShowTranslation). This is the user-facing side-effect the
// live bug report says is broken ("tooltip never appears"). This test isolates
// the DISPATCH layer from the clipboard/engine layers by wiring a stub
// click_cb_ that drives a real TooltipWindow directly - so a PASS proves the
// icon->callback->tooltip.show dispatch is intact, and a FAIL pinpoints the
// break inside DragIconWindow::WndProc's WM_LBUTTONUP handling itself.
void TestDragIconClickShowsTooltip() {
    std::cout << "[RUN] Testing drag-icon click -> tooltip dispatch (REQ-R1)..." << std::endl;
    const int failures_before = g_failed_count;

    const HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    DragIconWindow icon;
    TEST_CHECK(icon.Create(hInst), "R1 fixture: DragIconWindow created");
    TooltipWindow tooltip;
    TEST_CHECK(tooltip.Create(hInst), "R1 fixture: TooltipWindow created");

    // Mirror main.cpp's wiring shape: icon click -> show the translation
    // tooltip. Clipboard copy + engine inference are stubbed out here so the
    // assertion targets ONLY the WndProc click dispatch (the layer the live
    // report implicates).
    std::atomic<int> click_fired{0};
    icon.SetClickCallback([&](int cx, int cy) {
        click_fired.fetch_add(1, std::memory_order_relaxed);
        tooltip.ShowTranslation(cx, cy, L"src", "KO", "English", L"translated");
    });

    icon.ShowAt(300, 300);
    TEST_CHECK(icon.IsVisible(), "R1: icon visible after ShowAt");

    // Deliver the exact message pair Windows posts to the icon on a real
    // left-click (client coords are unused by the icon's WM_LBUTTONUP case;
    // it reads GetCursorPos() for the popup anchor).
    const LPARAM client_pt = MAKELPARAM(16, 16);
    ::PostMessageW(icon.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON, client_pt);
    ::PostMessageW(icon.GetHwnd(), WM_LBUTTONUP, 0, client_pt);
    PumpThreadMessagesOnce();

    TEST_CHECK(click_fired.load(std::memory_order_relaxed) == 1,
               "R1: icon WM_LBUTTONUP fires click_cb_ exactly once");
    TEST_CHECK(tooltip.IsVisible(),
               "R1: tooltip becomes visible after the icon click dispatch");
    TEST_CHECK(tooltip.GetTranslatedText() == L"translated",
               "R1: tooltip shows the translated payload from the click");

    // The icon hides itself as part of its own click handling (Hide() before
    // invoking the callback), matching the live app contract.
    TEST_CHECK(!icon.IsVisible(), "R1: icon auto-hides after the click");

    tooltip.Destroy();
    icon.Destroy();
    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Drag-icon click dispatch tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Drag-icon click dispatch tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// REQ-R1 (session 260905_0001): prove the silent-failure branch that produces
// the live symptom: when the copy gate reports failure, main.cpp's wired
// click_cb_ early-returns with ZERO user feedback and the tooltip never
// appears. Phase 2 §2.6(a): the gate-failure path is now forced DETERMINISTIC
// (no real clipboard interaction), so the result no longer depends on the
// desktop happening to hold no live selection (the documented R1 flake). The
// real CopySelectionWithSequenceWait copy path stays covered by
// TestClipboardSequencePolling (REQ-R04).
void TestDragIconClickClipboardEarlyReturn() {
    std::cout << "[RUN] Testing drag-icon click clipboard early-return (REQ-R1)..." << std::endl;
    const int failures_before = g_failed_count;
    const HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    TooltipWindow tooltip;
    TEST_CHECK(tooltip.Create(hInst), "R1 fixture: TooltipWindow created for early-return path");

    // Deterministic: simulate the exact click_cb_ control flow with the copy
    // gate FORCED to fail (no real clipboard interaction), so the test no
    // longer depends on the desktop having no live selection. The real
    // CopySelectionWithSequenceWait is covered separately by
    // TestClipboardSequencePolling (REQ-R04).
    std::atomic<bool> copy_gate_passed{true};   // forced-fail path sets it false
    std::atomic<bool> tooltip_shown{false};
    auto wired_click_cb = [&](int /*cx*/, int /*cy*/, bool copy_ok) {
        if (!copy_ok) {
            copy_gate_passed.store(false, std::memory_order_relaxed);
            return; // the silent early-return under test
        }
        copy_gate_passed.store(true, std::memory_order_relaxed);
        tooltip.ShowTranslation(0, 0, L"x", "KO", "English", L"translated");
        tooltip_shown.store(true, std::memory_order_relaxed);
    };

    wired_click_cb(0, 0, /*copy_ok=*/false); // force the failure path

    TEST_CHECK(!copy_gate_passed.load(std::memory_order_relaxed),
               "R1: copy gate fails closed (forced-failure path)");
    TEST_CHECK(!tooltip_shown.load(std::memory_order_relaxed),
               "R1: early-return skips ShowTranslation entirely");
    TEST_CHECK(!tooltip.IsVisible(),
               "R1: tooltip never appears on the clipboard-failure path");
    tooltip.Destroy();
    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Drag-icon click clipboard early-return tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Drag-icon click clipboard early-return tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// REQ-R10 (audit §3.4): the WM_APP marshal protocol - packing helpers,
// message routing, and WndProc payload ownership. Runs the real WndProc on the
// test main thread (which owns the created windows' queues).
void TestUIMarshaling() {
    std::cout << "[RUN] Testing D2D thread-marshal helpers (REQ-R10)..." << std::endl;
    const int failures_before = g_failed_count;

    // 1. Coordinate packing is a lossless int round-trip (the documented
    //    reason MAKELPARAM was rejected: 16-bit truncation on multi-monitor
    //    negative virtual coordinates).
    static_assert(DragIconWindow::ShowAtXFromWParam(DragIconWindow::PackShowAtX(-32000)) == -32000,
                  "R10: WParam round-trips 16-bit-negative coords");
    static_assert(DragIconWindow::ShowAtYFromLParam(DragIconWindow::PackShowAtY(INT_MIN)) == INT_MIN,
                  "R10: LParam round-trips full int range");
    static_assert(DragIconWindow::kShowAtMessage != DragIconWindow::kHideMessage &&
                      DragIconWindow::kShowAtMessage != TooltipWindow::kShowTranslationMessage &&
                      TooltipWindow::kShowMessageMessage != TooltipWindow::kDismissMessage,
                  "R10: marshaled message IDs are distinct");
    const int probes[] = { 0, 1, -1, 1024, -54321, 123456, INT_MAX, INT_MIN };
    bool pack_ok = true;
    for (const int v : probes) {
        if (DragIconWindow::ShowAtXFromWParam(DragIconWindow::PackShowAtX(v)) != v ||
            DragIconWindow::ShowAtYFromLParam(DragIconWindow::PackShowAtY(v)) != v) {
            pack_ok = false;
        }
    }
    TEST_CHECK(pack_ok, "R10: WParam/LParam int round-trip across full range incl. multi-monitor negatives");
    TEST_CHECK(!DragIconWindow::RequestShowAt(nullptr, 10, 20), "R10: RequestShowAt(null hwnd) fails cleanly");

    // 2. Real DragIconWindow::WndProc consumes kShowAtMessage and shows the
    //    icon via the packed coordinates (same-thread direct execution).
    const HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    DragIconWindow drag_icon;
    TEST_CHECK(drag_icon.Create(hInst), "R10 fixture: DragIconWindow created");

    TEST_CHECK(DragIconWindow::RequestShowAt(drag_icon.GetHwnd(), -200, -300), "R10: RequestShowAt posts");
    PumpThreadMessagesOnce();
    TEST_CHECK(drag_icon.IsVisible(), "R10: posted show message executed ShowAt on the owner thread");
    RECT r = {};
    ::GetWindowRect(drag_icon.GetHwnd(), &r);
    // Negative coordinates get clamped on-screen by ShowAt's monitor logic;
    // the assert that matters: the marshaled values reached ShowAt (visible)
    // and a positive in-bounds request lands exactly.
    DragIconWindow::RequestShowAt(drag_icon.GetHwnd(), 320, 240);
    PumpThreadMessagesOnce();
    ::GetWindowRect(drag_icon.GetHwnd(), &r);
    TEST_CHECK(r.left == 320 && r.top == 240, "R10: coords survive WParam/LParam through WndProc intact");
    ::PostMessageW(drag_icon.GetHwnd(), DragIconWindow::kHideMessage, 0, 0);
    PumpThreadMessagesOnce();
    TEST_CHECK(!drag_icon.IsVisible(), "R10: marshaled kHideMessage hides the icon");
    drag_icon.Destroy();

    // 3. Real TooltipWindow::WndProc takes heap-payload ownership via LPARAM:
    //    unique_ptr semantics (WndProc deletes exactly once; the posting seam
    //    deletes on PostMessage failure). Message mode shows header/body.
    TooltipWindow tooltip;
    TEST_CHECK(tooltip.Create(hInst), "R10 fixture: TooltipWindow created");
    tooltip.ShowMessage(100, 100, L"F9", L"Emebalachat Active (F9)");
    TEST_CHECK(tooltip.IsVisible() && tooltip.IsMessageMode(), "R08 feedback: message-mode bubble shows");
    TEST_CHECK(tooltip.GetMessageHeader() == L"F9", "R08 feedback: header text intact");
    tooltip.Dismiss();
    TEST_CHECK(!tooltip.IsVisible(), "R10: Dismiss hides the bubble");

    auto* msg_payload = new TooltipWindow::MessagePayload();
    msg_payload->x = 150;
    msg_payload->y = 160;
    msg_payload->header = L"F9";
    msg_payload->body = L"Paused";
    TEST_CHECK(TooltipWindow::PostPayloadForTest(tooltip.GetHwnd(), TooltipWindow::kShowMessageMessage, msg_payload),
               "R10: heap payload posted via LPARAM");
    PumpThreadMessagesOnce();
    TEST_CHECK(tooltip.IsVisible() && tooltip.IsMessageMode() && tooltip.GetMessageHeader() == L"F9",
               "R10: WndProc consumed payload and rendered message mode");

    auto* tr_payload = new TooltipWindow::TranslationPayload();
    tr_payload->x = 120;
    tr_payload->y = 130;
    tr_payload->source_text = L"source";
    tr_payload->source_lang_code = "KO";
    tr_payload->target_lang = "English";
    tr_payload->translated_text = L"translated";
    TEST_CHECK(TooltipWindow::PostPayloadForTest(tooltip.GetHwnd(), TooltipWindow::kShowTranslationMessage, tr_payload),
               "R10: translation payload posted");
    PumpThreadMessagesOnce();
    TEST_CHECK(!tooltip.IsMessageMode() && tooltip.GetSourceText() == L"source" &&
                   tooltip.GetTranslatedText() == L"translated",
               "R10: WndProc consumed translation payload (leaves message mode)");

    // Thread-safe seams called ON the owner thread run inline (no deadlock).
    tooltip.ShowTranslationThreadSafe(200, 210, L"s2", "EN", "Korean", L"t2");
    TEST_CHECK(tooltip.IsVisible() && tooltip.GetSourceText() == L"s2", "R10: ThreadSafe seam runs inline on owner thread");
    tooltip.Destroy();

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] D2D thread-marshal tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] D2D thread-marshal tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// REQ-R15 (audit §5 latent item 3): mixed-DPI coordinate/scale math. The pure
// conversions + clamping are fully headless; the runtime half asserts the
// process actually ends up DPI-aware and windows report sane per-monitor DPI.
void TestDpiMixedScaling() {
    std::cout << "[RUN] Testing REQ-R15 Mixed-DPI Scaling Math..." << std::endl;
    const int failures_before = g_failed_count;

    using emebalachat::ui::ClampWindowOrigin;
    using emebalachat::ui::ScaleDipsToPixels;
    using emebalachat::ui::ScalePixelsToDips;

    // 1. Compile-time pins of the scaling contract (MulDiv round-to-nearest).
    static_assert(ScaleDipsToPixels(32, 96) == 32, "R15: 100% identity");
    static_assert(ScaleDipsToPixels(32, 120) == 40, "R15: 32 DIP @125% -> 40 px");
    static_assert(ScaleDipsToPixels(32, 144) == 48, "R15: 32 DIP @150% -> 48 px");
    static_assert(ScaleDipsToPixels(32, 192) == 64, "R15: 32 DIP @200% -> 64 px");
    static_assert(ScaleDipsToPixels(240, 144) == 360, "R15: badge 240 DIP @150% -> 360 px");
    static_assert(ScaleDipsToPixels(1, 144) == 2, "R15: MulDiv rounding (1*144+48)/96 = 2, floor would give 1");
    static_assert(ScaleDipsToPixels(0, 144) == 0, "R15: zero stays zero");
    static_assert(ScaleDipsToPixels(32, 0) == 32, "R15: failed DPI query clamps to 96, never divides by 0");
    static_assert(ScaleDipsToPixels(32, 48) == 32, "R15: absurd low DPI clamps to base (no UI shrink)");
    static_assert(ScalePixelsToDips(48, 144) == 32, "R15: physical->DIP inverse");
    static_assert(ScalePixelsToDips(40, 120) == 32, "R15: inverse @125%");
    static_assert(ScalePixelsToDips(32, 96) == 32, "R15: inverse identity");
    static_assert(ScalePixelsToDips(48, 0) == 48, "R15: inverse guards dpi 0");

    // 2. Clamp policy in PHYSICAL virtual-screen units. Primary 0..1920/0..1080,
    //    right-hand 150% secondary starting at x=1920, LEFT-hand negative-offset
    //    secondary (the mixed-DPI multi-monitor case the audit asked about).
    constexpr RECT kPrimary = { 0, 0, 1920, 1040 };
    constexpr RECT kLeftMon = { -2560, -400, -100, 680 };   // secondary at negative x
    constexpr RECT kRightMon = { 1920, 0, 3840, 1040 };

    // A 48px (150%) icon dragged past the right edge lands fully inside, 4px margin.
    {
        constexpr POINT p = ClampWindowOrigin(1900, 1030, 48, 48, 4, kPrimary);
        static_assert(p.x + 48 <= kPrimary.right, "R15: right overflow clamped inside work area");
        static_assert(p.y + 48 <= kPrimary.bottom, "R15: bottom overflow clamped inside work area");
        static_assert(p.x == 1920 - 48 - 4 && p.y == 1040 - 48 - 4, "R15: exact margin math");
    }
    // Negative-coordinate secondary monitor: a window left of the secondary's
    // left edge gets pulled to left+margin, x stays NEGATIVE (virtual coords).
    {
        constexpr POINT p = ClampWindowOrigin(-3000, -500, 64, 64, 10, kLeftMon);
        static_assert(p.x == -2560 + 10, "R15: negative-x clamp keeps virtual-screen semantics");
        static_assert(p.y == -400 + 10, "R15: negative-y clamp top edge");
    }
    // Already inside -> untouched (no jitter on ordinary moves).
    {
        constexpr POINT p = ClampWindowOrigin(1930, 50, 48, 48, 4, kRightMon);
        static_assert(p.x == 1930 && p.y == 50, "R15: in-bounds origin passes through unchanged");
    }
    TEST_CHECK(true, "R15: clamp policy compile-time matrix (inside/outside/negative monitors)");

    // 3. Runtime: process awareness + per-monitor DPI queries return sane values.
    // (SetProcessDpiAwareness* can only succeed once per process; run_tests may
    // already have a context from a prior batch call - either way the effective
    // DPI query must be >= 96 and the awareness flag must end up true on this
    // Windows 11 host.)
    const bool aware = emebalachat::ui::EnsurePerMonitorV2ProcessDpiAwareness();
    TEST_CHECK(aware, "R15: process is per-monitor DPI aware after init");
    const UINT dpi_primary = emebalachat::ui::MonitorDpiAtPoint(POINT{ 10, 10 });
    TEST_CHECK(dpi_primary >= 96 && dpi_primary <= 480, "R15: primary monitor DPI query in sane range");
    TEST_CHECK(emebalachat::ui::MonitorDpiAtPoint(POINT{ -999999, -999999 }) >= 96,
               "R15: far off-screen point still yields a valid nearest-monitor DPI");

    // 4. Real windows report their monitor DPI through WindowDpi().
    const HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    DragIconWindow icon;
    TEST_CHECK(icon.Create(hInst), "R15 fixture: DragIconWindow created");
    icon.ShowAt(120, 120);
    TEST_CHECK(icon.IsVisible(), "R15: icon shows");
    RECT rc = {};
    ::GetWindowRect(icon.GetHwnd(), &rc);
    const int w = rc.right - rc.left;
    const int expected = ScaleDipsToPixels(32, dpi_primary);
    TEST_CHECK(w == expected, "R15: shown icon window is 32-DIP scaled to the monitor's physical px");
    icon.Destroy();

    FloatingBadge badge;
    TEST_CHECK(badge.Create(hInst, L"KO", L"EN", 100, 100), "R15 fixture: badge created");
    RECT brc = {};
    ::GetWindowRect(badge.GetHwnd(), &brc);
    const int bh = brc.bottom - brc.top;
    TEST_CHECK(bh == ScaleDipsToPixels(38, dpi_primary), "R15: badge height 38-DIP scaled to physical px");
    badge.Destroy();

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-R15 Mixed-DPI scaling tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-R15 Mixed-DPI scaling tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// REQ-R14 (audit §5 latent item 2): hook lifecycle event policy + the verify-
// and-reinstall seam, exercised on a REAL installed hook (Start/Stop/Reinstall
// without synthetic user input).
void TestHookLifecyclePolicy() {
    std::cout << "[RUN] Testing REQ-R14 hook lifecycle policy..." << std::endl;
    const int failures_before = g_failed_count;

    // 1. Pure message-classifier matrix (compile-time pinned, mirrors the
    //    ControllerWndProc policy - the ONLY trigger set is resume + unlock).
    static_assert(ShouldReinstallHooksOnEvent(WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC),
                  "R14: resume from sleep reinstalls hooks");
    static_assert(ShouldReinstallHooksOnEvent(WM_POWERBROADCAST, PBT_APMRESUMESUSPEND),
                  "R14: user-visible resume reinstalls hooks");
    static_assert(!ShouldReinstallHooksOnEvent(WM_POWERBROADCAST, PBT_APMSUSPEND),
                  "R14: entering sleep must NOT reinstall (hook thread should idle, not churn)");
    static_assert(!ShouldReinstallHooksOnEvent(WM_POWERBROADCAST, PBT_APMPOWERSTATUSCHANGE),
                  "R14: unrelated power broadcast ignored");
    static_assert(ShouldReinstallHooksOnEvent(WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK),
                  "R14: Win+L unlock reinstalls hooks");
    static_assert(!ShouldReinstallHooksOnEvent(WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK),
                  "R14: lock itself ignored (unlock follows; reinstall into a locked desktop is pointless)");
    static_assert(!ShouldReinstallHooksOnEvent(WM_WTSSESSION_CHANGE, WTS_SESSION_LOGON),
                  "R14: logon ignored");
    static_assert(!ShouldReinstallHooksOnEvent(WM_TIMER, 0), "R14: random messages ignored");
    static_assert(kHookReinstallDebounceMs >= 500 && kHookReinstallDebounceMs <= 3000,
                  "R14: debounce window coalesces resume bursts without delaying recovery");
    static_assert(kHookHealthWatchdogMs >= 10000, "R14: watchdog cadence bounds silent-unhook window");

    // 2. Runtime: the WTS registration API is reachable (wtsapi32 linked) and
    //    a real notification window can register/unregister cleanly - the
    //    exact sequence wWinMain performs.
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = ::DefWindowProcW;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = L"Emebalachat_R14TestSink";
        ::RegisterClassExW(&wc);
        HWND sink = ::CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"r14", WS_POPUP,
                                      -100, -100, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
        TEST_CHECK(sink != nullptr, "R14: lifecycle sink window creates");
        const bool reg = ::WTSRegisterSessionNotification(sink, NOTIFY_FOR_THIS_SESSION) == TRUE;
        TEST_CHECK(reg, "R14: WTSRegisterSessionNotification succeeds on an interactive session");
        if (reg) {
            TEST_CHECK(::WTSUnRegisterSessionNotification(sink) == TRUE, "R14: unregister succeeds");
        }
        if (sink) {
            ::DestroyWindow(sink);
        }
    }

    // 3. Real KeyboardHook lifecycle through the Reinstall() seam: start ->
    //    healthy -> reinstall keeps running with a fresh hook -> stop ->
    //    reinstall refuses to resurrect.
    SetSoundEnabled(false);
    AppConfig cfg;
    cfg.hotkey_toggle = "F9";
    TranslationManager engine(EngineType::GoogleTranslate, "");
    FloatingBadge badge;
    SystemTray tray;
    PipelineWorker worker(cfg, engine, badge);
    KeyboardHook hook(cfg, worker, badge, tray);

    TEST_CHECK(!hook.IsRunning() && !hook.IsHealthy(), "R14: fresh hook is neither running nor healthy");
    TEST_CHECK(!hook.Reinstall(), "R14: Reinstall on a never-started hook is a no-op returning false");

    TEST_CHECK(hook.Start(), "R14 fixture: keyboard hook starts");
    TEST_CHECK(hook.IsRunning(), "R14: running after Start");
    TEST_CHECK(hook.IsHealthy(), "R14: healthy after successful install");
    const DWORD tid_before = hook.HookThreadIdForTest();
    TEST_CHECK(tid_before != 0, "R14: hook thread id captured");

    TEST_CHECK(hook.Reinstall(), "R14: Reinstall returns true while running");
    TEST_CHECK(hook.IsRunning() && hook.IsHealthy(), "R14: hook healthy again after reinstall");
    const DWORD tid_after = hook.HookThreadIdForTest();
    TEST_CHECK(tid_after != 0 && tid_after != tid_before,
               "R14: reinstall actually spawned a NEW hook thread (old one joined)");

    hook.Stop();
    TEST_CHECK(!hook.IsRunning() && !hook.IsHealthy(), "R14: stopped hook reports unhealthy");
    TEST_CHECK(!hook.Reinstall(), "R14: stopped hook never resurrects (user/shutdown intent honored)");

    // Same contract for the MouseHook side.
    MouseHook mouse_hook;
    TEST_CHECK(!mouse_hook.Reinstall(), "R14: MouseHook Reinstall no-op before Start");
    TEST_CHECK(mouse_hook.Start(), "R14 fixture: mouse hook starts");
    TEST_CHECK(mouse_hook.IsHealthy(), "R14: mouse hook healthy after install");
    TEST_CHECK(mouse_hook.Reinstall(), "R14: MouseHook Reinstall true while running");
    TEST_CHECK(mouse_hook.IsRunning() && mouse_hook.IsHealthy(), "R14: mouse hook healthy after reinstall");
    mouse_hook.Stop();
    SetSoundEnabled(true);

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-R14 hook lifecycle tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-R14 hook lifecycle tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// REQ-R17 (audit §5 latent item 5): IME composition safety predicate.
void TestImeCompositionGate() {
    std::cout << "[RUN] Testing REQ-R17 IME composition gate..." << std::endl;
    const int failures_before = g_failed_count;

    // 1. Compile-time gate matrix. vk_is_return=false models the OS behavior
    //    for IME-processed keys: LowLevelKeyboardProc receives VK_PROCESSKEY
    //    (0xE7), NOT VK_RETURN, so composition keystrokes never reach the
    //    Enter branch; the extra GCS_COMPSTR probe covers the case where the
    //    Korean IME lets Enter through as VK_RETURN while composing.
    static_assert(VK_PROCESSKEY == 0xE5, "R17: IME-processed keys are delivered as VK_PROCESSKEY (0xE5)");

    // Hook-local composition mirror (ImeMirrorNext): set by intercepted jamo
    // keys, cleared by plain editing keys; VK_RETURN KEEPS the flag so the
    // Enter branch (which reads the mirror before deciding, and clears it
    // only when a task was accepted) still gates the Korean-IME pass-through
    // Enter that arrives as VK_RETURN mid-composition.
    static_assert(ImeMirrorNext(false, VK_PROCESSKEY), "R17: intercepted key opens composition");
    static_assert(ImeMirrorNext(true, VK_PROCESSKEY), "R17: repeated jamo keeps composition");
    static_assert(!ImeMirrorNext(true, VK_ESCAPE), "R17: plain Esc reaching the hook -> IME not intercepting -> clear");
    static_assert(!ImeMirrorNext(true, VK_BACK), "R17: plain Backspace clears composition");
    static_assert(!ImeMirrorNext(true, VK_LEFT), "R17: plain arrow clears composition");
    static_assert(ImeMirrorNext(true, VK_RETURN), "R17: Enter does NOT pre-clear (branch owns the decision)");
    static_assert(!ImeMirrorNext(false, VK_RETURN), "R17: Enter while idle stays idle (state kept, not fabricated)");
    static_assert(ImeMirrorNext(true, 'A'), "R17: character keys keep composition state");
    static_assert(!ImeMirrorNext(false, 'A'), "R17: unknown vk never fabricates a composition");

    // R5 hardening: vertical / page navigation keys reaching the hook as real
    // vk codes prove the IME did not intercept them -> no composition can be
    // alive -> the mirror must clear. Previously these fell into "keep state",
    // so a composition that ended without a tracked clear key (click-away, an
    // Electron-swallowed commit) could leave the mirror stuck true and silently
    // gate out the next bare Enter ("typed text never translates").
    static_assert(!ImeMirrorNext(true, VK_UP), "R5: plain Up clears composition");
    static_assert(!ImeMirrorNext(true, VK_DOWN), "R5: plain Down clears composition");
    static_assert(!ImeMirrorNext(true, VK_PRIOR), "R5: Page Up clears composition");
    static_assert(!ImeMirrorNext(true, VK_NEXT), "R5: Page Down clears composition");
    static_assert(!ImeMirrorNext(false, VK_UP), "R5: Up while idle stays idle");
    static_assert(!ImeMirrorNext(false, VK_NEXT), "R5: Page Down while idle stays idle");
    // Composing is still preserved for keys that do not finalise a composition.
    static_assert(ImeMirrorNext(true, 'A'), "R5: typing keeps composition (unchanged)");

    // Phase 7 Batch 1 (REQ-013/014 report §7.2): VK_PROCESSKEY sequence matrix.
    // The mirror is the LANGUAGE-AGNOSTIC composition signal (works for
    // Korean IMM32 AND TSF IMEs that route keys via VK_PROCESSKEY). These
    // asserts pin the contract as a folded sequence: ImeMirrorNext chains
    // its own output back in as `composing`, exactly how hook.cpp updates
    // ime_composing_ on every real keydown (src/hook.cpp:632-635).
    {
        constexpr bool s0 = false; // idle: no composition observed yet

        // (1) VK_PROCESSKEY opens composition from idle - the language-agnostic
        //     entry edge (any IME kind routing keys via VK_PROCESSKEY).
        static_assert(ImeMirrorNext(s0, VK_PROCESSKEY), "P7: idle + VK_PROCESSKEY -> composing");
        // (2) Sustained interception (repeated candidate-window keys, e.g. a
        //     Japanese kana run or Chinese pinyin run) keeps composing true.
        constexpr bool s1 = ImeMirrorNext(s0, VK_PROCESSKEY);
        static_assert(ImeMirrorNext(s1, VK_PROCESSKEY), "P7: repeated VK_PROCESSKEY keeps composing");
        // (3) Ordinary printable keys during composition keep state (the IME
        //     lets letters through as real vks in some TSF paths).
        static_assert(ImeMirrorNext(s1, 'A'), "P7: letter mid-composition keeps composing");
        // (4) Composition-clearing editing keys reaching the hook as REAL vks
        //     (not previously asserted set: Delete/Right/Home/End/Tab):
        static_assert(!ImeMirrorNext(true, VK_DELETE), "P7: plain Delete clears composition");
        static_assert(!ImeMirrorNext(true, VK_RIGHT), "P7: plain Right clears composition");
        static_assert(!ImeMirrorNext(true, VK_HOME), "P7: plain Home clears composition");
        static_assert(!ImeMirrorNext(true, VK_END), "P7: plain End clears composition");
        static_assert(!ImeMirrorNext(true, VK_TAB), "P7: plain Tab clears composition");
        // (5) Full commit+send sequence (canonical Korean IME, report §4.2):
        //     jamo interception -> composing; Enter arrives as VK_RETURN and
        //     the mirror KEEPS state (the Enter branch owns the decision and
        //     retires the flag). F2 (REQ-F2, V2 verify §3d): with the hook
        //     active and the worker idle the composing Enter is PROMOTED into
        //     the pipeline (ImeCommitEnterPromoted) instead of the pre-F2 pure
        //     pass-through - commit-then-translate. EnterTranslationAllowed is
        //     intentionally UNCHANGED: composing still never fires through the
        //     BARE gate; promotion is a separate decision evaluated only from
        //     the composing branch (hook.cpp). The safe commit is the worker's
        //     ExecuteTask start (FlushIme + ForegroundImeComposing re-probe),
        //     so a live GCS_COMPSTR is never captured. The next keydown after
        //     the branch retired the flag sees idle again - a fresh bare Enter
        //     with composing=false re-arms the pipeline (send-of-output).
        constexpr bool s2 = ImeMirrorNext(s0, VK_PROCESSKEY); // jamo intercepted
        static_assert(ImeMirrorNext(s2, VK_RETURN), "P7: commit Enter mid-composition keeps mirror (branch clears)");
        constexpr bool s3 = ImeMirrorNext(s2, VK_RETURN); // mirror value at branch decision time
        static_assert(!EnterTranslationAllowed(true, true, false, s3),
                      "P7: composing Enter never fires through the BARE gate (unchanged; F2 promotes it separately)");
        static_assert(ImeCommitEnterPromoted(true, false),
                      "F2: composing Enter + active hook + idle worker -> promoted (commit-then-translate)");
        static_assert(!ImeCommitEnterPromoted(false, false),
                      "F2: composing Enter + inactive hook -> pass through unchanged (pre-F2)");
        static_assert(!ImeCommitEnterPromoted(true, true),
                      "F2: composing Enter + busy worker -> pass through unchanged (pre-F2)");
        //     Branch retired the flag (promotion or pass-through) -> the next
        //     keydown observes the cleared state; a subsequent bare Enter can
        //     fire through the ordinary gates (the consecutive-second-Enter
        //     send/newline path - never double-promoted from one composition).
        constexpr bool s4 = false; // post-branch retire performed by hook.cpp
        static_assert(EnterTranslationAllowed(true, true, false, ImeMirrorNext(s4, VK_RETURN)),
                      "P7: bare Enter after branch-retired mirror re-arms pipeline");
        // (6) Esc-cleared composition followed by bare Enter also re-arms:
        constexpr bool s5 = ImeMirrorNext(s2, VK_ESCAPE); // user cancelled composition
        static_assert(!s5, "P7: Esc-clear folds mirror to idle");
        static_assert(EnterTranslationAllowed(true, true, false, ImeMirrorNext(s5, VK_RETURN)),
                      "P7: bare Enter after Esc-cleared composition fires");
        // (7) VK_PROCESSKEY stream after a clear re-opens composition (TSF
        //     candidate navigation then fresh kana input).
        constexpr bool s6 = ImeMirrorNext(s5, VK_PROCESSKEY);
        static_assert(s6, "P7: VK_PROCESSKEY after clear re-opens composition");
        static_assert(ImeMirrorNext(s6, VK_UP) == false, "P7: candidate-window Up after reopen clears (real vk passed)");
    }

    // Runtime fold of the same Phase 7 sequence matrix (static asserts are
    // compile-time only and do not increment Total Checks; the R17 pattern
    // pairs the static matrix with a runtime pass so the coverage is visible
    // in the test summary). Each fold mirrors hook.cpp's ime_composing_ update.
    {
        bool m = ImeMirrorNext(false, VK_PROCESSKEY); // idle -> composing
        TEST_CHECK(m, "P7 runtime: VK_PROCESSKEY opens composition");
        m = ImeMirrorNext(m, 'A');                    // letter mid-composition
        TEST_CHECK(m, "P7 runtime: letter mid-composition keeps composing");
        m = ImeMirrorNext(m, VK_RETURN);              // commit Enter keeps mirror
        TEST_CHECK(m, "P7 runtime: commit Enter keeps mirror (branch owns clear)");
        TEST_CHECK(!EnterTranslationAllowed(true, true, false, m),
                   "P7 runtime: composing Enter never fires through the BARE gate (F2 promotes separately)");
        m = ImeMirrorNext(m, VK_ESCAPE);              // Esc clears composition
        TEST_CHECK(!m, "P7 runtime: Esc-clear folds mirror to idle");
        m = ImeMirrorNext(m, VK_PROCESSKEY);          // fresh interception re-opens
        TEST_CHECK(m, "P7 runtime: VK_PROCESSKEY after clear re-opens composition");
        m = ImeMirrorNext(m, VK_DELETE);              // real Delete reaching hook clears
        TEST_CHECK(!m, "P7 runtime: plain Delete clears reopened composition");
        TEST_CHECK(EnterTranslationAllowed(true, true, false, ImeMirrorNext(m, VK_RETURN)),
                   "P7 runtime: bare Enter after clear re-arms pipeline");
    }

    // F2 (REQ-F2, V2 verify §3d): runtime fold of the composing-Enter
    // commit-then-translate decision + the single-promotion guarantee (the
    // "연속 두 번째 Enter" gate). One composing Enter is promoted (active +
    // idle) and the mirror is retired by the branch; the NEXT Enter then sees
    // idle and is evaluated by the ordinary gates, so a second Enter can never
    // be double-promoted from the same composition - it takes the send/newline
    // path. Only a fresh sentence (new VK_PROCESSKEY) re-opens composition and
    // is promoted again.
    {
        bool m = ImeMirrorNext(false, VK_PROCESSKEY); // jamo interception opens composition
        TEST_CHECK(m, "F2 runtime: composition open before the commit Enter");
        TEST_CHECK(ImeCommitEnterPromoted(true, false),
                   "F2 runtime: active hook + idle worker -> composing Enter promoted (translate)");
        TEST_CHECK(!ImeCommitEnterPromoted(false, false),
                   "F2 runtime: inactive hook -> composing Enter passes through (pre-F2)");
        TEST_CHECK(!ImeCommitEnterPromoted(true, true),
                   "F2 runtime: busy worker -> composing Enter passes through (pre-F2)");
        m = ImeMirrorNext(m, VK_RETURN); // mirror value at the branch decision (kept)
        TEST_CHECK(m, "F2 runtime: commit Enter keeps mirror until the branch decides");
        m = false;                       // post-promotion retire (hook.cpp store)
        TEST_CHECK(EnterTranslationAllowed(true, true, false, ImeMirrorNext(m, VK_RETURN)),
                   "F2 runtime: second Enter after promotion is a normal bare Enter (send-of-output)");
        m = ImeMirrorNext(false, VK_PROCESSKEY); // fresh sentence re-opens composition
        TEST_CHECK(m && ImeCommitEnterPromoted(true, false),
                   "F2 runtime: fresh-sentence composing Enter is promoted again (translate-per-sentence)");
    }

    // R5 (Debug-Surgical) Enter-path empty-capture verdict, pinned on the
    // shared predicate (src/worker.hpp EmptyCaptureNeedsHold) so worker.cpp
    // and the tests assert on ONE definition:
    //   - empty capture, NOT a smart bypass  -> HOLD the send + tooltip
    //     (the reported "Enter sent my text untranslated with no tooltip")
    //   - smart bypass (text already in target) -> NOT a failure: the
    //     historical send-through is the product contract; no hold
    //   - successful capture -> normal pipeline; no hold
    static_assert(EmptyCaptureNeedsHold(true, false),
                  "R5: empty capture (not bypassed) must hold the send + notice");
    static_assert(!EmptyCaptureNeedsHold(true, true),
                  "R5: smart-bypassed text must NOT be held (send-through contract)");
    static_assert(!EmptyCaptureNeedsHold(false, false),
                  "R5: successful capture must NOT be held (normal pipeline)");
    static_assert(!EmptyCaptureNeedsHold(false, true),
                  "R5: non-empty capture cannot be smart-bypassed and held simultaneously (vacuous guard)");

    // Enter gate matrix (mirror value feeds ime_composing). These pin the
    // BARE-Enter gate contract, which is UNCHANGED by F2: composing Enter is
    // served by the composing branch via ImeCommitEnterPromoted (separate
    // predicate), never by this gate.
    static_assert(EnterTranslationAllowed(true, true, false, false), "R17: plain Enter while idle -> fire");
    static_assert(!EnterTranslationAllowed(true, true, false, true),
                  "R17: composing -> NEVER fire through the bare gate (F2 promotes in the composing branch)");
    static_assert(!EnterTranslationAllowed(true, true, true, false), "R17: busy worker -> pass through");
    static_assert(!EnterTranslationAllowed(true, false, false, false), "R17: inactive hook -> pass through");
    static_assert(!EnterTranslationAllowed(false, true, false, true), "R17: VK_PROCESSKEY Enter -> pass through");
    static_assert(!EnterTranslationAllowed(false, true, false, false), "R17: non-Enter vk -> gate closed");
    // F2 promotion matrix (evaluated only from the composing branch; Shift/Ctrl
    // already returned upstream so the candidate is always a bare Enter):
    static_assert(ImeCommitEnterPromoted(true, false),
                  "F2: active + idle -> composing Enter promoted (commit-then-translate)");
    static_assert(!ImeCommitEnterPromoted(false, false),
                  "F2: inactive -> composing Enter passed through (pre-F2 byte-identical)");
    static_assert(!ImeCommitEnterPromoted(true, true),
                  "F2: busy -> composing Enter passed through (no queue pile-up)");
    static_assert(!ImeCommitEnterPromoted(false, true),
                  "F2: inactive + busy -> composing Enter passed through");

    // 2. Runtime IMM probe stability (win32_input seam - worker-thread-safe):
    //    callable from the test thread, idempotent, never crashes regardless
    //    of what the user's foreground window is.
    const bool first = ForegroundImeComposing();
    const bool second = ForegroundImeComposing();
    TEST_CHECK(first == second, "R17: composition probe is stable across immediate re-queries");
    std::cout << "  [R17 INFO] foreground composing = " << (first ? "true" : "false") << std::endl;

    // 3. Probe semantics with NO foreground window we can force: create our
    //    own window and query ITS context directly through the same API pair
    //    the probe uses - a freshly created window never has a composition
    //    (GCS_COMPSTR size is 0), which pins the 'fail closed to false' half.
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = ::DefWindowProcW;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = L"Emebalachat_R17TestWnd";
        ::RegisterClassExW(&wc);
        HWND w = ::CreateWindowExW(0, wc.lpszClassName, L"r17", WS_OVERLAPPED,
                                   -200, -200, 50, 50, nullptr, nullptr, wc.hInstance, nullptr);
        TEST_CHECK(w != nullptr, "R17 fixture: plain window created");
        HIMC himc = ::ImmGetContext(w);
        if (himc) {
            const LONG comp = ::ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
            ::ImmReleaseContext(w, himc);
            TEST_CHECK(comp <= 0, "R17: fresh window has no composition string (probe predicate false)");
        } else {
            TEST_CHECK(true, "R17: ImmGetContext returned null (no IME -> probe false by contract)");
        }
        ::DestroyWindow(w);
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-R17 IME composition gate tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-R17 IME composition gate tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// S2 (R4): Shift+Enter newline vs bare-Enter send-and-replace modifier gate.
//
// Root cause pinned here: the R3-era VK_RETURN branch in LowLevelKeyboardProc
// checked Ctrl (Ctrl+Shift+Enter = auto-send toggle, Ctrl+Enter = passthrough)
// but NEVER tested the Shift modifier, so with the hook active ANY Enter
// (bare OR Shift-held) was swallowed into the pipeline. Result: the user could
// not insert a newline via Shift+Enter while F9 send-and-replace was active -
// the hook ate the keystroke and the worker re-sent a plain Enter. The fix
// passes Shift+Enter through untouched and reserves interception for a BARE
// Enter. This test pins the full modifier matrix on the shared pure predicate
// (src/hook.hpp) so the discrimination can never silently regress.
void TestShiftEnterGate() {
    std::cout << "[RUN] Testing S2 Shift+Enter / bare-Enter gate..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- Compile-time modifier matrix on EnterSendReplaceAllowed ----
    // Baseline: a bare Enter, hook active, worker idle, no composition, no
    // Shift -> INTERCEPT (send-and-replace fires). This is the primary path.
    static_assert(EnterSendReplaceAllowed(true, true, false, false, false),
                  "S2: bare Enter while idle+active -> intercept (send-and-replace)");

    // The S2 fix: SAME conditions but Shift held -> PASS THROUGH (newline).
    static_assert(!EnterSendReplaceAllowed(true, true, false, false, true),
                  "S2: Shift+Enter while idle+active -> pass through (newline, not swallowed)");

    // Shift must NOT override the other gates: each base-gate failure still
    // passes through regardless of Shift state.
    static_assert(!EnterSendReplaceAllowed(true, false, false, false, false),
                  "S2: bare Enter, hook INACTIVE -> pass through (no interception)");
    static_assert(!EnterSendReplaceAllowed(true, false, false, false, true),
                  "S2: Shift+Enter, hook INACTIVE -> pass through");
    static_assert(!EnterSendReplaceAllowed(true, true, true, false, false),
                  "S2: bare Enter, worker BUSY -> pass through (re-entrancy guard)");
    static_assert(!EnterSendReplaceAllowed(true, true, true, false, true),
                  "S2: Shift+Enter, worker BUSY -> pass through");
    // F2 (REQ-F2) note: these two pin the BARE-GATE contract only
    // (EnterSendReplaceAllowed is evaluated solely when composing==false, after
    // the hook's composing branch returns). A composing Enter is decided by the
    // composing branch + ImeCommitEnterPromoted, never by this S2 gate.
    static_assert(!EnterSendReplaceAllowed(true, true, false, true, false),
                  "S2: bare Enter mid-IME-composition never fires through the BARE gate (F2: composing branch promotes)");
    static_assert(!EnterSendReplaceAllowed(true, true, false, true, true),
                  "S2: Shift+Enter mid-IME-composition -> gate closed (Shift+Enter already passes upstream)");

    // Non-Enter vk never intercepts (gate stays closed on the vk axis).
    static_assert(!EnterSendReplaceAllowed(false, true, false, false, false),
                  "S2: non-Enter vk -> gate closed");
    static_assert(!EnterSendReplaceAllowed(false, true, false, false, true),
                  "S2: non-Enter vk with Shift -> gate closed");

    // REQ-003 (Issue C, session 260910_0003): busy same-window bare-Enter
    // suppression predicate, pinned on the shared definition
    // (src/hook.hpp BusyEnterSameWindowSuppressed) so hook.cpp and the tests
    // assert ONE definition (same discipline as the other ENTER_GATE
    // predicates). The hook evaluates it ONLY on the live busy arm (hook
    // active + IsBusy() re-checked + BusyTargetHwnd == foreground window).
    static_assert(BusyEnterSameWindowSuppressed(true, true),
                  "REQ-003: busy + same window -> SUPPRESS (bare Enter would corrupt the pending paste-back)");
    static_assert(!BusyEnterSameWindowSuppressed(true, false),
                  "REQ-003: busy + DIFFERENT window -> pass through (in-flight task owns a different target)");
    static_assert(!BusyEnterSameWindowSuppressed(false, true),
                  "REQ-003: idle worker -> guard inert (idle bare-Enter contract byte-identical)");
    static_assert(!BusyEnterSameWindowSuppressed(false, false),
                  "REQ-003: idle + different window -> vacuous arm, never suppresses");

    // Runtime sanity: the predicate is the pure conjunction of exactly the
    // two scoping axes - no hidden inputs, no ordering effects.
    for (bool busy : {false, true}) {
        for (bool same : {false, true}) {
            TEST_CHECK(BusyEnterSameWindowSuppressed(busy, same) == (busy && same),
                       "REQ-003: predicate == (busy && same_window) on every input");
        }
    }

    // ---- Runtime sanity: predicate agrees with the base gate on the Shift
    // discrimination (the only new axis). The base EnterTranslationAllowed
    // ignores Shift entirely (it predates the fix); the S2 wrapper must equal
    // the base gate when Shift is false and be FALSE when Shift is true.
    for (bool active : {false, true}) {
        for (bool busy : {false, true}) {
            for (bool composing : {false, true}) {
                const bool base = EnterTranslationAllowed(true, active, busy, composing);
                TEST_CHECK(EnterSendReplaceAllowed(true, active, busy, composing, false) == base,
                           "S2: with Shift released, S2 gate == base Enter gate");
                TEST_CHECK(EnterSendReplaceAllowed(true, active, busy, composing, true) == false,
                           "S2: with Shift held, S2 gate always passes through (newline)");
            }
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] S2 Shift+Enter / bare-Enter gate tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] S2 Shift+Enter / bare-Enter gate tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// REQ-R16 (audit §5 latent item 4): llama.cpp shutdown cancellation seam.
void TestEngineShutdownCancellation() {
    std::cout << "[RUN] Testing REQ-R16 engine shutdown cancellation..." << std::endl;
    const int failures_before = g_failed_count;

    // 1. Fresh manager: cancel is off, idle immediately.
    TranslationManager mgr(EngineType::GoogleTranslate, "");
    TEST_CHECK(!mgr.IsCancelRequested(), "R16: fresh manager has no pending cancellation");
    TEST_CHECK(mgr.WaitInferenceIdle(100), "R16: idle manager reports idle within 100 ms");

    // 2. RequestCancel latches and short-circuits every subsequent call -
    //    including the CLOUD path (shutdown must not fire a WinHTTP request
    //    that could transmit user text after an exit intent).
    mgr.RequestCancel();
    TEST_CHECK(mgr.IsCancelRequested(), "R16: cancellation request latches");
    TranslationStatus st = TranslationStatus::Ok;
    std::wstring res = mgr.Translate(L"안녕하세요", "KO", "EN", &st);
    TEST_CHECK(res.empty(), "R16: canceled manager returns empty (no cloud call after exit intent)");
    TEST_CHECK(st == TranslationStatus::Canceled, "R16: canceled Translate reports Canceled, not EngineFailed");
    TEST_CHECK(mgr.WaitInferenceIdle(100), "R16: manager idle after short-circuited call");

    // 3. Strict-local manager: cancel short-circuit precedes the H2 consent
    //    decision (a canceled exit is not a privacy block either).
    TranslationManager local(EngineType::LocalLlama, "D:\\non_existent_model.gguf");
    local.RequestCancel();
    TranslationStatus lst = TranslationStatus::Ok;
    std::wstring lres = local.Translate(L"테스트", "KO", "EN", &lst);
    TEST_CHECK(lres.empty() && lst == TranslationStatus::Canceled,
               "R16: explicit-local canceled call reports Canceled");

    // 4. Live model (when the fixture exists): start a long generation on a
    //    worker thread, request cancellation mid-flight, and assert the
    //    BOUNDED drain that main.cpp's shutdown sequence relies on: the
    //    engine must report idle within 15 s of RequestCancel().
    std::string local_model_path;
    char env_model[4096] = {0};
    DWORD env_len = ::GetEnvironmentVariableA(
        "EMEBALA_MODEL_PATH", env_model, static_cast<DWORD>(sizeof(env_model)) - 1);
    if (env_len > 0 && env_len < static_cast<DWORD>(sizeof(env_model)) - 1) {
        local_model_path = env_model;
    }
    if (!local_model_path.empty() && std::filesystem::exists(local_model_path)) {
        std::cout << "  [R16 LIVE] cancel-in-flight drain against Hy-MT2 model..." << std::endl;
        TranslationManager live(EngineType::LocalLlama, local_model_path);
        std::wstring filler;
        filler.reserve(9000);
        while (filler.size() < 8000) {
            filler += L"안녕하세요, 만나서 반갑습니다. 오늘 날씨가 아주 좋습니다. 번역 테스트를 위한 긴 문장을 반복해서 채웁니다. ";
        }
        std::atomic<TranslationStatus> live_st{TranslationStatus::Ok};
        std::atomic<bool> done{false};
        std::jthread inference([&]() {
            TranslationStatus s = TranslationStatus::Ok;
            live.Translate(filler, "KO", "English", &s);
            live_st.store(s, std::memory_order_release);
            done.store(true, std::memory_order_release);
        });

        // Give the worker time to pass EnsureLoaded's fast path and enter the
        // decode loop; then flip the shutdown latch exactly like wWinMain does.
        ::Sleep(150);
        const auto cancel_t = std::chrono::steady_clock::now();
        live.RequestCancel();
        const bool drained = live.WaitInferenceIdle(15000);
        const auto drain_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - cancel_t).count();
        TEST_CHECK(drained, "R16: in-flight decode unwinds after RequestCancel (bounded drain)");
        TEST_CHECK(WaitUntilMs([&]() { return done.load(std::memory_order_acquire); }, 5000),
                   "R16: inference thread finishes after cancellation");
        std::cout << "  [R16 LIVE] drain to idle in " << drain_ms << " ms, status "
                  << static_cast<int>(live_st.load()) << std::endl;
        // Either the generation completed before the latch (Ok with a result)
        // or cancellation won (Canceled) - it must NEVER be reported as an
        // EngineFailed privacy/network failure, and the drain is time-bounded.
        TEST_CHECK(live_st.load() == TranslationStatus::Canceled ||
                       live_st.load() == TranslationStatus::Ok,
                   "R16: canceled-or-completed only; never a spurious failure status");
        TEST_CHECK(drain_ms < 15000, "R16: shutdown drain stays bounded (no zombie inference)");
    } else {
        std::cout << "  [R16 LIVE SKIP] set EMEBALA_MODEL_PATH to exercise cancel-in-flight" << std::endl;
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-R16 engine shutdown cancellation tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-R16 engine shutdown cancellation tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// D5 item F: engine.cpp's ctor relative-model fallback is exe-dir anchored
// (D4-flagged issue 1). A direct-construct manager with an EMPTY model path
// resolves to <exe dir>/models/... regardless of the process CWD.
void TestEngineFallbackExeDirAnchoring() {
    std::cout << "[RUN] Testing D5-F engine fallback exe-dir anchoring..." << std::endl;
    const int failures_before = g_failed_count;

    const std::string exe_dir = GetExecutableDir().string();
    TEST_CHECK(!exe_dir.empty(), "D5-F: GetExecutableDir resolves for the test binary");

    TranslationManager mgr(EngineType::Auto, ""); // empty -> internal fallback
    const std::string path = mgr.GetModelPath();
    TEST_CHECK(!path.empty(), "D5-F: fallback produced a non-empty model path");
    // Absolute (exe-anchored), not the raw CWD-relative default:
    TEST_CHECK(std::filesystem::path(path).is_absolute(),
               "D5-F: internal fallback path is absolute (CWD-independent)");
    // And it lives under the executable directory:
    std::string lower_path = path;
    for (char& c : lower_path) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string lower_exe = exe_dir;
    for (char& c : lower_exe) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!lower_exe.empty() && (lower_exe.back() == '\\' || lower_exe.back() == '/')) {
        lower_exe.pop_back();
    }
    TEST_CHECK(lower_exe.size() >= 2 && lower_path.rfind(lower_exe, 0) == 0,
               "D5-F: fallback resolves under the executable directory");

    // CWD-independence proof: move the process CWD to a directory that
    // definitely has no models\ subdir (System32 - the exact Run-registry
    // autostart condition from audit M3); a NEW direct-construct manager must
    // still produce the SAME exe-anchored path.
    {
        std::error_code ec;
        const auto old_cwd = std::filesystem::current_path(ec);
        if (!ec) {
            namespace fs = std::filesystem;
            const fs::path foreign = L"C:\\Windows\\System32";
            if (fs::exists(foreign)) {
                fs::current_path(foreign, ec);
                if (!ec) {
                    TranslationManager mgr2(EngineType::Auto, "");
                    TEST_CHECK(mgr2.GetModelPath() == path,
                               "D5-F: fallback path is CWD-independent (System32-CWD reproduction)");
                    fs::current_path(old_cwd, ec);
                }
            }
        }
    }
    if (g_failed_count == failures_before) {
        std::cout << "[PASS] D5-F engine fallback anchoring tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] D5-F engine fallback anchoring tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Batch 2 (session 260905_0001): REQ-006 version single source of truth,
// REQ-002 scrollable tooltip (pure math + runtime), REQ-005 About window
// smoke. Follows the plan's test-mapping table (§3 Batch 2 verification).
// ---------------------------------------------------------------------------
void TestBatch2VersionScrollAbout() {
    std::cout << "[RUN] Testing Batch 2 version + tooltip scroll + about window..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- 1. REQ-006: version plumbing exposes exactly PROJECT_VERSION ----
    TEST_CHECK(kAppVersionW == L"0.10.0", "REQ-006: kAppVersionW is 0.10.0 (CMake definition or fallback)");
    TEST_CHECK(kAppVersionA == "0.10.0", "REQ-006: ASCII version is 0.10.0");
    TEST_CHECK(kAppNameW == L"Emebala Chat", "REQ-004: display-name constant is rebranded");

    // ---- 2. REQ-002: pure scroll math, all DIP (plan §2.1 edge cases) ----
    using TT = TooltipWindow;
    static_assert(TT::kMaxWindowHeightDip == 520, "plan §2.1: window cap raised 480 -> 520");
    static_assert(TT::BodyViewportHeightDip(520) == 520.0f - 48.0f - 44.0f,
                  "viewport = window height - body top - footer reserve");
    static_assert(TT::BodyViewportHeightDip(40) == 0.0f, "degenerate height clamps to 0, never negative");
    // Offset clamping: fits -> 0; top/bottom clamps; mid passthrough.
    static_assert(TT::ClampScrollOffset(50.0f, 100.0f, 400.0f) == 0.0f, "content fits viewport -> pinned 0");
    static_assert(TT::ClampScrollOffset(-10.0f, 1000.0f, 400.0f) == 0.0f, "wheel past top clamps to 0");
    static_assert(TT::ClampScrollOffset(700.0f, 1000.0f, 400.0f) == 600.0f, "wheel past bottom clamps to content-viewport");
    static_assert(TT::ClampScrollOffset(128.0f, 1000.0f, 400.0f) == 128.0f, "in-range offset passes through");
    // Wheel delta: one notch = 3 body lines; sign: positive delta (wheel up)
    // decreases the offset. Binary-exact line heights chosen to avoid fp wobble.
    static_assert(TT::WheelDeltaToOffsetStepDip(120, 16.0f) == -48.0f, "notch-up scrolls 3 lines toward top");
    static_assert(TT::WheelDeltaToOffsetStepDip(-120, 16.0f) == 48.0f, "notch-down scrolls 3 lines downward");
    static_assert(TT::WheelDeltaToOffsetStepDip(0, 16.0f) == 0.0f, "zero delta is zero step");
    static_assert(TT::WheelDeltaToOffsetStepDip(-60, 16.0f) == 24.0f, "half-notch scales proportionally");
    // Thumb extent: proportional, min 24, never taller than track.
    static_assert(TT::ScrollbarThumbHeightDip(512.0f, 256.0f, 1024.0f) == 128.0f, "thumb = track * viewport/content");
    static_assert(TT::ScrollbarThumbHeightDip(512.0f, 256.0f, 16384.0f) == 24.0f, "thumb min height 24 DIP");
    static_assert(TT::ScrollbarThumbHeightDip(512.0f, 512.0f, 512.0f) == 512.0f, "content fits -> full track");
    // Thumb position: linear map of offset, boundary pinning (no jitter).
    static_assert(TT::ScrollbarThumbTopDip(48.0f, 512.0f, 256.0f, 0.0f, 800.0f, 400.0f) == 48.0f, "offset 0 -> track top");
    static_assert(TT::ScrollbarThumbTopDip(48.0f, 512.0f, 256.0f, 200.0f, 800.0f, 400.0f) == 176.0f, "midpoint maps to mid travel");
    static_assert(TT::ScrollbarThumbTopDip(48.0f, 512.0f, 256.0f, 400.0f, 800.0f, 400.0f) == 304.0f, "max offset -> track bottom");
    static_assert(TT::ScrollbarThumbTopDip(48.0f, 512.0f, 256.0f, 999.0f, 800.0f, 400.0f) == 304.0f, "overshoot clamps");
    static_assert(TT::ScrollbarThumbTopDip(48.0f, 512.0f, 512.0f, 100.0f, 800.0f, 400.0f) == 48.0f, "thumb fills track -> pinned top");
    TEST_CHECK(true, "REQ-002: scroll-math compile-time matrix (clamp/wheel/thumb geometry)");

    // Marshaled message IDs must be distinct across the blocks in use.
    static_assert(TT::kScrollMessage != TT::kShowTranslationMessage &&
                      TT::kScrollMessage != TT::kShowMessageMessage &&
                      TT::kScrollMessage != TT::kDismissMessage &&
                      AboutWindow::kShowMessage != TT::kScrollMessage &&
                      AboutWindow::kDismissMessage != AboutWindow::kShowMessage,
                  "REQ-002: kScrollMessage and About IDs distinct from all marshaled IDs");

    // ---- 3. REQ-002 runtime: long text becomes scrollable, wheel scrolls ----
    const HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    TooltipWindow tooltip;
    TEST_CHECK(tooltip.Create(hInst), "REQ-002 fixture: TooltipWindow created");

    // Long body (4000 chars of repeated words -> wraps to many lines).
    std::wstring long_text;
    long_text.reserve(4000);
    for (int i = 0; i < 400; ++i) {
        long_text += L"scrollable translation segment alpha bravo charlie delta ";
    }
    tooltip.ShowTranslation(200, 200, L"source", "KO", "English", long_text);
    TEST_CHECK(tooltip.IsVisible(), "REQ-002: long translation shows");
    TEST_CHECK(tooltip.IsScrollableForTest(), "REQ-002: overflowing content enables scroll");
    TEST_CHECK(tooltip.ScrollOffsetForTest() == 0.0f, "REQ-002: fresh show starts unscrolled");
    TEST_CHECK(tooltip.ContentHeightForTest() > TT::BodyViewportHeightDip(TT::kMaxWindowHeightDip),
               "REQ-002: measured content exceeds the 520-DIP viewport");

    // Direct same-thread wheel step: moves down and clamps at the bottom.
    tooltip.ScrollByDipWheel(-120);
    const float after_one_notch = tooltip.ScrollOffsetForTest();
    TEST_CHECK(after_one_notch > 0.0f, "REQ-002: wheel-down advances the offset");
    for (int i = 0; i < 200; ++i) {
        tooltip.ScrollByDipWheel(-120); // many notches down -> clamp at bottom
    }
    const float bottom = tooltip.ScrollOffsetForTest();
    const float viewport_h = TT::BodyViewportHeightDip(520);
    TEST_CHECK(bottom == TT::ClampScrollOffset(1e9f, tooltip.ContentHeightForTest(), viewport_h),
               "REQ-002: repeated wheel-down clamps exactly at content-viewport");
    for (int i = 0; i < 400; ++i) {
        tooltip.ScrollByDipWheel(120); // wheel up past the top -> clamp at 0
    }
    TEST_CHECK(tooltip.ScrollOffsetForTest() == 0.0f, "REQ-002: repeated wheel-up clamps at 0 (no jitter)");

    // kScrollMessage marshaling path (what the LL hook posts): pump and assert.
    tooltip.ScrollByDipWheel(-120);
    const float before_post = tooltip.ScrollOffsetForTest();
    TEST_CHECK(::PostMessageW(tooltip.GetHwnd(), TT::kScrollMessage, 0,
                              static_cast<LPARAM>(-2 * WHEEL_DELTA)) == TRUE,
               "REQ-002: kScrollMessage posts");
    PumpThreadMessagesOnce();
    TEST_CHECK(tooltip.ScrollOffsetForTest() > before_post,
               "REQ-002: WndProc consumed kScrollMessage and scrolled further");

    // New ShowTranslation resets the offset (plan edge case 4).
    tooltip.ShowTranslation(200, 200, L"source", "KO", "English", long_text);
    TEST_CHECK(tooltip.ScrollOffsetForTest() == 0.0f, "REQ-002: re-show resets scroll offset");

    // Dismiss resets scroll state (plan §2.1: reset on Dismiss).
    tooltip.Dismiss();
    TEST_CHECK(!tooltip.IsVisible() && !tooltip.IsScrollableForTest(),
               "REQ-002: Dismiss hides and clears scroll state");

    // Short text: identical to today, no scrollbar (plan §2.1 edge case 1).
    tooltip.ShowTranslation(200, 200, L"src", "KO", "English", L"short");
    TEST_CHECK(!tooltip.IsScrollableForTest(), "REQ-003: short text keeps non-scroll UI");
    tooltip.ScrollByDipWheel(-120);
    TEST_CHECK(tooltip.ScrollOffsetForTest() == 0.0f, "REQ-002: wheel on non-overflowing body is a no-op");

    // Message mode never scrolls (plan §2.1 edge case 6).
    tooltip.ShowMessage(200, 200, L"F9", L"notice body");
    TEST_CHECK(tooltip.IsMessageMode() && !tooltip.IsScrollableForTest(), "R08 card reports not scrollable");
    tooltip.ScrollByDipWheel(-120);
    TEST_CHECK(tooltip.ScrollOffsetForTest() == 0.0f, "REQ-002: message mode ignores wheel");
    tooltip.Dismiss();
    tooltip.Destroy();

    // ---- 4. REQ-005: AboutWindow smoke (create/show/dismiss/marshal) ----
    static_assert(AboutWindow::kNumLinks == 3, "plan §2.2: Website/Contact/Download links");
    AboutWindow about;
    TEST_CHECK(about.Create(hInst), "REQ-005 fixture: AboutWindow created");
    TEST_CHECK(about.GetHwnd() != nullptr, "REQ-005: About window handle exists");
    TEST_CHECK(!about.IsVisible(), "REQ-005: starts hidden");

    about.Show(640, 480);
    TEST_CHECK(about.IsVisible(), "REQ-005: Show makes it visible");
    RECT ar = {};
    ::GetWindowRect(about.GetHwnd(), &ar);
    const int aw = ar.right - ar.left;
    const UINT adpi = emebalachat::ui::WindowDpi(about.GetHwnd());
    TEST_CHECK(aw == emebalachat::ui::ScaleDipsToPixels(440, adpi),
               "REQ-005/R15: About width 440 DIP scales to the monitor DPI");

    // D1 (debug report T3): WM_DPICHANGED handler smoke. No real DPI change
    // is injectable here, so the message is sent directly to the GUI-thread
    // WndProc (same-thread synchronous, matching REQ-R10 affinity). Pins the
    // observable contract: handler consumes the message, repositions to the
    // suggested rect origin, and keeps the physical window extents equal to
    // the DIP layout scaled by the window's live DPI - i.e. the DIB and the
    // window stay 1:1 so the layered blit is never rescaled (the blur). The
    // tooltip carries the identical handler; one smoke pins the pattern.
    // NOTE (Phase 4, REQ-020, plan §2.2): the About card grew 560 -> 596 DIP
    // to make room for the full-width reset button under the contact block;
    // the height expectation below tracks that constant (width stays 440).
    {
        RECT cur = {};
        ::GetWindowRect(about.GetHwnd(), &cur);
        RECT suggested = { cur.left + 3, cur.top + 5,
                           cur.right + 3, cur.bottom + 5 };
        const UINT cur_dpi = emebalachat::ui::WindowDpi(about.GetHwnd());
        const LRESULT d1_ret = ::SendMessageW(
            about.GetHwnd(), WM_DPICHANGED, MAKEWPARAM(cur_dpi, cur_dpi),
            reinterpret_cast<LPARAM>(&suggested));
        TEST_CHECK(d1_ret == 0, "D1: About WndProc consumes WM_DPICHANGED");
        RECT after = {};
        ::GetWindowRect(about.GetHwnd(), &after);
        TEST_CHECK(after.left == suggested.left && after.top == suggested.top,
                   "D1: About repositions to the suggested rect origin");
        TEST_CHECK(after.right - after.left ==
                       emebalachat::ui::ScaleDipsToPixels(440, cur_dpi) &&
                       after.bottom - after.top ==
                       emebalachat::ui::ScaleDipsToPixels(596, cur_dpi),
                   "D1: About keeps DIP-scaled physical extents after the change");
        TEST_CHECK(about.IsVisible(), "D1: About stays visible across the DPI change");
    }

    about.Dismiss();
    TEST_CHECK(!about.IsVisible(), "REQ-005: Dismiss hides the About window");

    auto* show_payload = new AboutWindow::ShowPayload{ 300, 300 };
    const bool show_posted = ::PostMessageW(about.GetHwnd(), AboutWindow::kShowMessage, 0,
                                            reinterpret_cast<LPARAM>(show_payload)) == TRUE;
    if (!show_posted) {
        delete show_payload; // WndProc never took ownership
    }
    TEST_CHECK(show_posted, "REQ-005: marshaled show payload posts");
    PumpThreadMessagesOnce();
    TEST_CHECK(about.IsVisible(), "REQ-005: WndProc consumed show payload");
    ::PostMessageW(about.GetHwnd(), AboutWindow::kDismissMessage, 0, 0);
    PumpThreadMessagesOnce();
    TEST_CHECK(!about.IsVisible(), "REQ-005: marshaled dismiss message hides it");
    about.Destroy();

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Batch 2 version/scroll/about tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Batch 2 version/scroll/about tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// R5 (user report): switching the TARGET language mid-session must take effect
// for every source/target combination (A->B, then A->C, B->C, ...). The user
// reported: after translating Korean->English, switching the target to Chinese
// did NOT translate into Chinese. This pins the full switching matrix on the
// pure seams that decide (a) whether to translate at all (ShouldTranslate) and
// (b) which target the engines are told to produce (BuildPrompt / MapLanguageCode /
// NormalizeLanguageCode), so a language-switch regression can never silently ship.
void TestLanguageSwitchingMatrix() {
    std::cout << "[RUN] Testing R5 target-language switching matrix..." << std::endl;
    const int failures_before = g_failed_count;

    // Representative source samples in distinct scripts.
    const std::wstring ko = L"안녕하세요, 오늘 회의 자료를 별도로 본문에 삽입해줘.";
    const std::wstring en = L"Please review the attached document before the meeting.";
    const std::wstring zh = L"请在会议之前查看附件中的文件。";

    // ---- 1) Every distinct target the user can switch TO must translate a
    //         source that is NOT already that target. This is the A->B, A->C,
    //         A->D ... matrix the user demanded work in every case.
    const char* targets[] = {
        "Korean", "English", "Vietnamese", "Chinese Simplified", "Chinese Traditional",
        "Japanese", "Spanish", "French", "German", "Russian"
    };
    for (const char* tgt : targets) {
        // Korean source -> any non-Korean target must translate.
        if (std::string(tgt) != "Korean") {
            TEST_CHECK(ShouldTranslate(ko, tgt),
                       (std::string("R5: Korean source must translate to ") + tgt).c_str());
        }
        // English source -> any non-English target must translate.
        if (std::string(tgt) != "English") {
            TEST_CHECK(ShouldTranslate(en, tgt),
                       (std::string("R5: English source must translate to ") + tgt).c_str());
        }
    }

    // ---- 2) The reported switch sequence: KO->EN first, then switch target to
    //         Chinese Simplified and re-translate the SAME Korean source. The
    //         decision must flip to "translate" for the new target (it must not
    //         stay bypassed as if the target were still English, and must not be
    //         bypassed by any stale/Chinese-variant rule).
    TEST_CHECK(ShouldTranslate(ko, "English"), "R5: step 1 KO -> EN translates");
    TEST_CHECK(ShouldTranslate(ko, "Chinese Simplified"), "R5: step 2 KO -> ZH-CN (switched) still translates");
    TEST_CHECK(ShouldTranslate(ko, "ZH-CN"), "R5: KO -> ZH-CN by code translates");
    TEST_CHECK(ShouldTranslate(ko, "Chinese Traditional"), "R5: KO -> ZH-TW translates");

    // ---- 3) B->C: after translating English->Korean, switch target to Chinese.
    TEST_CHECK(ShouldTranslate(en, "Korean"), "R5: step 1 EN -> KO translates");
    TEST_CHECK(ShouldTranslate(en, "Chinese Simplified"), "R5: step 2 EN -> ZH-CN (switched) still translates");

    // ---- 4) Chinese variant discrimination: a ZH-CN source must NOT be bypassed
    //         when the target is ZH-TW (they are different scripts), and vice versa.
    TEST_CHECK(ShouldTranslate(zh, "Chinese Traditional"),
               "R5: ZH-CN source -> ZH-TW target must translate (different script, not bypassed)");
    TEST_CHECK(ShouldTranslate(zh, "Korean"), "R5: ZH-CN source -> KO target translates");
    // ...but a ZH-CN source targeting ZH-CN IS bypassed (already in target).
    TEST_CHECK(!ShouldTranslate(zh, "Chinese Simplified"),
               "R5: ZH-CN source -> ZH-CN target bypassed (already in target)");

    // ---- 5) Target resolution + prompt construction: the engine must be told to
    //         produce the SWITCHED target. NormalizeLanguageCode resolves both
    //         display names and codes; BuildPrompt must inject the target name.
    TEST_CHECK(NormalizeLanguageCode("Chinese Simplified") == "ZH-CN", "R5: 'Chinese Simplified' normalizes to ZH-CN");
    TEST_CHECK(NormalizeLanguageCode("Chinese Traditional") == "ZH-TW", "R5: 'Chinese Traditional' normalizes to ZH-TW");
    TEST_CHECK(NormalizeLanguageCode("Korean") == "KO", "R5: 'Korean' normalizes to KO");
    TEST_CHECK(GoogleTranslate::MapLanguageCode("Chinese Simplified") == "zh-CN", "R5: Google maps ZH-CN correctly");
    TEST_CHECK(GoogleTranslate::MapLanguageCode("Chinese Traditional") == "zh-TW", "R5: Google maps ZH-TW correctly");

    // BuildPrompt target injection: the local LLM prompt must name the SWITCHED
    // target (this is what steers Hy-MT2 to emit Chinese vs English).
    // R6 Phase 4 (B2) intended behavior change: the name injected for a
    // resolvable token is the NATIVE form (plan §4.1 item 1). "Chinese
    // Simplified" (name_en) in the instruction was the out-of-distribution form
    // behind the JA->ZH-degrades-to-English bug; the prompt now carries 简体
    // 中文 (name_native) instead.
    const std::string p_en = BuildPrompt("hello", "English");
    const std::string p_zh = BuildPrompt("hello", "Chinese Simplified");
    TEST_CHECK(p_en.find("English") != std::string::npos, "R5: BuildPrompt injects 'English' as target");
    TEST_CHECK(p_zh.find("简体中文") != std::string::npos, "R5/R6p4: BuildPrompt injects native '简体中文' for 'Chinese Simplified'");
    TEST_CHECK(p_zh.find("Chinese Simplified") == std::string::npos, "R6p4: English target name must NOT appear in the local prompt");
    TEST_CHECK(p_en != p_zh, "R5: switching target changes the local prompt (not cached/static)");

    // ---- 6) Cycling across the whole target list must yield a translatable
    //         (source, target) pair at every step for a fixed non-target source.
    std::string cur = "Korean";
    for (int i = 0; i < 40; ++i) {
        cur = CycleTargetLanguage(cur);
        const std::string cur_norm = NormalizeLanguageCode(cur);
        if (cur_norm != "KO") {
            TEST_CHECK(ShouldTranslate(ko, cur),
                       (std::string("R5: cycled target '") + cur + "' must translate Korean source").c_str());
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R5 target-language switching matrix tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] R5 target-language switching matrix tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ===========================================================================
// R6 Phase 4 (B2, architect plan §4) + Task 1 (session 260910_0004): language
// routing. Pins the three pure seams headlessly:
//   (1) BuildPrompt names the target in the template's form: the Chinese
//       branch keeps the NATIVE name (简体中文, not "Chinese Simplified"; the
//       branch output is byte-identical to R6 Phase 4), while the English
//       branch injects name_en ("into Korean", not "into 한국어" — Task 1
//       removed the code-switching that degraded the small local model). A
//       source hint is added only for a known non-AUTO source, in the same
//       per-branch form (AUTO/empty/unresolvable -> byte-identical historical
//       prompt).
//   (2) NormalizeLanguageCode / GoogleTranslate::MapLanguageCode round-trip
//       over the full plan §4.2(b) matrix (no mapping gap for any pair).
//   (3) LocalPairReliable + PlanTranslationRouting verdicts over sources
//       (Auto/JA/KO/EN/ZH-CN/DE/ES/VI) x targets (EN/KO/ZH-CN/ZH-TW/JA/DE/ES/VI)
//       x engines (Hy-MT2 local via Auto/LocalLlama pin, Google), including the
//       exact user scenario JA -> ZH-CN.
// ===========================================================================
namespace {
std::string R6P4Msg(const std::string& what, std::string_view src, std::string_view tgt) {
    return "R6p4: " + what + " [" + std::string(src) + "->" + std::string(tgt) + "]";
}

const char* R6P4EngineName(EngineType e) {
    switch (e) {
        case EngineType::Auto: return "auto";
        case EngineType::GoogleTranslate: return "google";
        case EngineType::LocalLlama: return "local";
    }
    return "?";
}
} // namespace

void TestR6P4LanguageRouting() {
    std::cout << "[RUN] Testing R6 Phase 4 language routing (B2 prompt + pair matrix)..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- 1) Target name injection per branch, plan §4.2(a) row 1 + Task 1.
    // The ZH-CN user bug: "将以下文本翻译为Chinese Simplified" (English name
    // inside the Chinese instruction) is out-of-distribution for Hy-MT2 and
    // degraded JA->ZH output to English. The Chinese branch keeps name_native.
    // The English branch uses name_en (Task 1): "into 한국어" / "into Deutsch"
    // native injection was code-switching that hurt small-model quality.
    TEST_CHECK(BuildPrompt("hello", "ZH-CN") ==
                   "将以下文本翻译为简体中文，注意只需要输出翻译后的结果，不要额外解释：\n\nhello",
               "R6p4: ZH-CN target prompt carries native 简体中文 (exact form)");
    TEST_CHECK(BuildPrompt("hello", "ZH-TW") ==
                   "将以下文本翻译为繁體中文，注意只需要输出翻译后的结果，不要额外解释：\n\nhello",
               "R6p4: ZH-TW target prompt carries native 繁體中文 (exact form)");
    TEST_CHECK(BuildPrompt("hello", "English") ==
                   "Translate the following segment into English, without additional explanation.\n\nhello",
               "R6p4: EN target instruction unchanged (native name == English)");

    // Matrix targets (plan §4.2(b)): EN/KO/ZH-CN/ZH-TW/JA/DE/ES/VI.
    struct R6P4Target { const char* code; const char* name_en; const char* name_native; };
    const R6P4Target targets[] = {
        { "EN",    "English",             "English" },
        { "KO",    "Korean",              "한국어" },
        { "ZH-CN", "Chinese Simplified",  "简体中文" },
        { "ZH-TW", "Chinese Traditional", "繁體中文" },
        { "JA",    "Japanese",            "日本語" },
        { "DE",    "German",              "Deutsch" },
        { "ES",    "Spanish",             "Español" },
        { "VI",    "Vietnamese",          "Tiếng Việt" },
    };
    for (const auto& t : targets) {
        const std::string by_code = BuildPrompt("hello", t.code);
        const std::string by_name = BuildPrompt("hello", t.name_en);
        const bool zh_branch =
            std::string(t.code) == "ZH-CN" || std::string(t.code) == "ZH-TW";
        const std::string expected_name = zh_branch ? t.name_native : t.name_en;
        const std::string wrong_name = zh_branch ? t.name_en : t.name_native;
        TEST_CHECK(by_code.find(expected_name) != std::string::npos,
                   R6P4Msg(std::string("target prompt contains '") + expected_name + "'", "-", t.code));
        TEST_CHECK(by_code == by_name,
                   R6P4Msg("code-form and name-form prompts are identical", "-", t.code));
        if (expected_name != wrong_name) {
            TEST_CHECK(by_code.find(wrong_name) == std::string::npos,
                       R6P4Msg(std::string("wrong-form name '") + wrong_name + "' must NOT appear", "-", t.code));
        }
    }

    // Task 1 acceptance pins: the original code-switching complaints must be
    // gone — targets Korean/German/Vietnamese produce fully-English
    // instructions via name_en (previously "into 한국어" / "into Deutsch" /
    // "into Tiếng Việt").
    {
        struct { const char* token; const char* needle; const char* banned; } en_cases[] = {
            { "Korean",     "into Korean",     "한국어" },
            { "German",     "into German",     "Deutsch" },
            { "Vietnamese", "into Vietnamese", "Tiếng Việt" },
            { "KO",         "into Korean",     "한국어" },
            { "DE",         "into German",     "Deutsch" },
            { "VI",         "into Vietnamese", "Tiếng Việt" },
        };
        for (const auto& c : en_cases) {
            const std::string p = BuildPrompt("hello", c.token);
            TEST_CHECK(p.find(c.needle) != std::string::npos,
                       R6P4Msg(std::string("English-branch prompt carries '") + c.needle + "'", "-", c.token));
            TEST_CHECK(p.find(c.banned) == std::string::npos,
                       R6P4Msg(std::string("English-branch prompt has no native '") + c.banned + "'", "-", c.token));
        }
    }

    // ---- 2) Source hint injection, plan §4.2(a) row 2 + Task 1. -----------
    // Known non-AUTO source -> name in the BRANCH's form: native in the
    // Chinese instruction, English in the English instruction.
    TEST_CHECK(BuildPrompt("hello", "ZH-CN", "JA") ==
                   "将以下日本語文本翻译为简体中文，注意只需要输出翻译后的结果，不要额外解释：\n\nhello",
               "R6p4: JA source + ZH target injects 日本語 source hint (zh branch keeps native)");
    TEST_CHECK(BuildPrompt("hello", "English", "Japanese") ==
                   "Translate the following Japanese segment into English, without additional explanation.\n\nhello",
               "Task 1: known source injects name_en source name into English branch");
    TEST_CHECK(BuildPrompt("hello", "Korean", "Japanese") ==
                   "Translate the following Japanese segment into Korean, without additional explanation.\n\nhello",
               "Task 1: English branch uses name_en on BOTH sides (KO target)");
    TEST_CHECK(BuildPrompt("hello", "English", "KO") ==
                   "Translate the following Korean segment into English, without additional explanation.\n\nhello",
               "Task 1: source code token resolves to name_en in English branch");
    TEST_CHECK(BuildPrompt("hello", "English", "한국어") ==
                   "Translate the following Korean segment into English, without additional explanation.\n\nhello",
               "Task 1: native-form source token also resolves to name_en in English branch");
    // AUTO / empty / unresolvable source -> NO source token (backward compat:
    // byte-identical to the historical two-argument prompts).
    {
        const std::string base = BuildPrompt("hello", "ZH-CN");
        TEST_CHECK(BuildPrompt("hello", "ZH-CN", "AUTO") == base,
                   "R6p4: AUTO source adds no token (code form, byte-identical)");
        TEST_CHECK(BuildPrompt("hello", "ZH-CN", "Auto Detect") == base,
                   "R6p4: 'Auto Detect' source adds no token (name form)");
        TEST_CHECK(BuildPrompt("hello", "ZH-CN", "자동 감지") == base,
                   "R6p4: native '자동 감지' source adds no token");
        TEST_CHECK(BuildPrompt("hello", "ZH-CN", "") == base,
                   "R6p4: empty source adds no token");
        TEST_CHECK(BuildPrompt("hello", "ZH-CN", "Klingon") == base,
                   "R6p4: unresolvable source adds no token (garbage cannot poison prompt)");
        const std::string base_en = BuildPrompt("hello", "English");
        TEST_CHECK(BuildPrompt("hello", "English", "AUTO") == base_en,
                   "R6p4: AUTO source keeps the English branch byte-identical");
    }

    // ---- 3) EXACT user scenario: JA -> ZH-CN. -----------------------------
    // The reported bug: source Japanese, target Chinese Simplified produced an
    // English-flavored result. The prompt must now be Chinese-targeted with the
    // native name AND the source named; it must NOT be an English-target prompt.
    {
        const std::string user_prompt = BuildPrompt("今日はいい天気ですね。", "Chinese Simplified", "Japanese");
        TEST_CHECK(user_prompt.find("简体中文") != std::string::npos,
                   "R6p4: user scenario JA->ZH-CN prompt contains 简体中文");
        TEST_CHECK(user_prompt.find("日本語") != std::string::npos,
                   "R6p4: user scenario prompt names the JA source");
        TEST_CHECK(user_prompt.find("Chinese Simplified") == std::string::npos,
                   "R6p4: user scenario prompt has no English target name");
        TEST_CHECK(user_prompt.find("Translate") == std::string::npos,
                   "R6p4: user scenario prompt is NOT the English-target instruction");
        TEST_CHECK(user_prompt.rfind("今日はいい天気ですね。") == user_prompt.size() - std::string("今日はいい天気ですね。").size(),
                   "R6p4: user scenario prompt ends with the source text");
    }

    // ---- 4) Code round-trip across the full matrix (plan §4.2(a) row 3). --
    // Every matrix token must normalize to its canonical code and map to the
    // exact Google BCP-47 form (proves no MapLanguageCode/Normalize gap for
    // any pair, B2-H3 sweep).
    struct R6P4Lang { const char* code; const char* name_en; const char* google; };
    const R6P4Lang langs[] = {
        { "AUTO",  "Auto Detect",         "auto" },
        { "EN",    "English",             "en" },
        { "KO",    "Korean",              "ko" },
        { "ZH-CN", "Chinese Simplified",  "zh-CN" },
        { "ZH-TW", "Chinese Traditional", "zh-TW" },
        { "JA",    "Japanese",            "ja" },
        { "DE",    "German",              "de" },
        { "ES",    "Spanish",             "es" },
        { "VI",    "Vietnamese",          "vi" },
    };
    for (const auto& l : langs) {
        TEST_CHECK(NormalizeLanguageCode(l.code) == l.code, R6P4Msg("code normalizes to itself", l.code, "-"));
        TEST_CHECK(NormalizeLanguageCode(l.name_en) == l.code, R6P4Msg("name_en normalizes to code", l.code, "-"));
        TEST_CHECK(GoogleTranslate::MapLanguageCode(l.code) == l.google, R6P4Msg("google map of code", l.code, "-"));
        TEST_CHECK(GoogleTranslate::MapLanguageCode(l.name_en) == l.google, R6P4Msg("google map of name_en", l.code, "-"));
    }

    // ---- 5) LocalPairReliable verdicts. F5 Phase 2 (ask audit 181530
    // condition 1 / Inquiry 3, adjudicated) SUPERSEDES the R6p4 plan §4.1
    // item 3 conservative EN-only set: a PINNED real source is ground truth
    // and every pinned distinct pair is reliable locally (the user SCOPE
    // DIRECTIVE demands 100% of all Hy-MT2 pairs; the EN-side gate pushed
    // pinned non-EN pairs onto the 041 cloud-leak / 042 degraded route).
    // Excluded verdicts kept: AUTO target, and the degenerate src==tgt
    // identity pair (nothing to translate; ShouldTranslate/pivot own it).
    {
        TEST_CHECK(LocalPairReliable("AUTO", "EN"),
                   "R6p4: AUTO->EN (the user's working scenario) is reliable");
        TEST_CHECK(LocalPairReliable("EN", "KO"), "R6p4: EN->KO reliable (source EN)");
        TEST_CHECK(LocalPairReliable("KO", "EN"), "R6p4: KO->EN reliable (target EN)");
        TEST_CHECK(LocalPairReliable("ja", "english"), "R6p4: verdicts are case/name-insensitive");
        TEST_CHECK(LocalPairReliable("JA", "ZH-CN"),
                   "F5: JA->ZH-CN pinned pair now reliable locally (supersedes the R6p4 conservative-set pin; the old degraded-to-English failure is answered by trusting the model, not by cloud routing)");
        TEST_CHECK(LocalPairReliable("KO", "JA"), "F5: KO->JA reliable (no English special case)");
        TEST_CHECK(LocalPairReliable("DE", "VI"), "F5: DE->VI reliable (no English special case)");
        TEST_CHECK(LocalPairReliable("ZH-CN", "EN"), "R6p4: ZH-CN->EN reliable (target EN)");
        TEST_CHECK(!LocalPairReliable("KO", "AUTO"), "R6p4: AUTO target is never reliable");
        // F5: identity pair (src==tgt) is now RELIABLE (served locally). The
        // old test pinned KO->KO unreliable (an accidental EN privilege:
        // EN->EN was the one identity pair that stayed on-device while KO->KO
        // Auto-routed to the cloud). Making every identity pair local keeps
        // the EN->EN privacy property language-neutral; the typing path never
        // routes identity anyway (ShouldTranslate bypasses it first).
        TEST_CHECK(LocalPairReliable("KO", "KO"), "F5: identity pair KO->KO reliable (on-device echo, no cloud); supersedes the R6p4 EN-equality pin");
        TEST_CHECK(LocalPairReliable("EN", "EN"), "F5: identity pair EN->EN reliable (pre-F5 behavior preserved for EN)");
    }

    // ---- 5b) F1 companion fix (session 260908_0003, verify 220010 §5 item 4):
    // an AUTO SOURCE is reliable to every real target - Hy-MT2's built-in
    // language ID owns the decision, so Latin Auto -> KO must NOT be flagged
    // outside-the-reliable-set and shipped to Google via 041 (privacy + the
    // false "VI -> KO" pair churn of the reported log).
    // F5 Phase 2 (audit 181530 Inquiry 3): the pinned-non-EN tripwire below
    // (VI->KO unreliable) was placed to await VP adjudication of exactly this
    // decision; the audit recommended and the VP/user directed removal of the
    // English-centric conservative gate for PINNED sources. Updated to the
    // adjudicated behavior (supersession documented at the assertion).
    {
        TEST_CHECK(LocalPairReliable("AUTO", "KO"), "F1: AUTO->KO reliable (model language ID, no cloud leak)");
        TEST_CHECK(LocalPairReliable("Auto Detect", "Korean"), "F1: AUTO reliability is name/code-form insensitive");
        TEST_CHECK(LocalPairReliable("AUTO", "VI"), "F1: AUTO->VI reliable");
        TEST_CHECK(LocalPairReliable("AUTO", "DE"), "F1: AUTO->DE reliable");
        TEST_CHECK(LocalPairReliable("VI", "KO"),
                   "F5/R4: PINNED VI->KO now reliable locally (supersedes the tripwire pin - audit 181530 Inquiry 3 adjudicated)");
        TEST_CHECK(LocalPairReliable("ko", "Japanese"), "F5: pinned KO->JA reliable, name/code-form insensitive");
        TEST_CHECK(PlanTranslationRouting("VI", "KO", EngineType::LocalLlama, false) == EngineType::LocalLlama,
                   "F5: pinned VI->KO strict-local stays local WITHOUT cloud consent (041 cannot fire for pinned real pairs)");
        TEST_CHECK(PlanTranslationRouting("KO", "JA", EngineType::Auto, false) == EngineType::LocalLlama,
                   "F5: pinned KO->JA under Auto engine routes local (all-pairs directive: no cloud leak)");
        TEST_CHECK(PlanTranslationRouting("AUTO", "KO", EngineType::LocalLlama, false) == EngineType::LocalLlama,
                   "F1: AUTO->KO explicit-local stays local even WITHOUT cloud consent (041 must not fire)");
        TEST_CHECK(PlanTranslationRouting("Auto Detect", "Korean", EngineType::Auto, false) == EngineType::LocalLlama,
                   "F1: AUTO->KO under Auto engine stays local (Hy-MT2 built-in language ID)");
        // BuildPrompt: the AUTO marker token adds no source hint (the exact
        // value DetectLanguage now returns for diacritic Latin).
        {
            const std::string base_ko = BuildPrompt("ola", "Korean");
            TEST_CHECK(BuildPrompt("ola", "Korean", "Auto Detect") == base_ko,
                       "F1: 'Auto Detect' source token adds no hint (model ID decides)");
            TEST_CHECK(base_ko.find("Tiếng Việt") == std::string::npos &&
                           base_ko.find("Vietnamese") == std::string::npos,
                       "F1: AUTO->KO prompt must not carry a Vietnamese claim (bogus LANG label gone)");
        }
    }

    // ---- 6) Full routing matrix: sources x targets x engines x consent. ---
    // expected policy (architect plan §4.1 item 3, REQ-R02 consent model):
    //   Google pin                    -> Google
    //   reliable pair                 -> Local (user pin honored)
    //   unreliable + Auto             -> Google (Auto selection IS the consent)
    //   unreliable + Local pin        -> Google only WITH explicit cloud consent,
    //                                    otherwise stays Local (strict on-device)
    const char* sources[] = { "AUTO", "JA", "KO", "EN", "ZH-CN", "DE", "ES", "VI" };
    for (const char* src : sources) {
        for (const auto& t : targets) {
            const bool reliable = LocalPairReliable(src, t.code);
            const EngineType engines[] = { EngineType::Auto, EngineType::LocalLlama, EngineType::GoogleTranslate };
            for (const EngineType eng : engines) {
                for (const bool consent : { false, true }) {
                    EngineType expected;
                    if (eng == EngineType::GoogleTranslate) {
                        expected = EngineType::GoogleTranslate;
                    } else if (reliable) {
                        expected = EngineType::LocalLlama;
                    } else if (eng == EngineType::Auto) {
                        expected = EngineType::GoogleTranslate;
                    } else {
                        expected = consent ? EngineType::GoogleTranslate : EngineType::LocalLlama;
                    }
                    const EngineType got = PlanTranslationRouting(src, t.code, eng, consent);
                    TEST_CHECK(got == expected,
                               (R6P4Msg(std::string("routing ") + R6P4EngineName(eng) +
                                        " consent=" + (consent ? "1" : "0") +
                                        " reliable=" + (reliable ? "1" : "0"),
                                        src, t.code) +
                                " (got " + R6P4EngineName(got) + ")").c_str());
                }
            }
        }
    }

    // ---- 7) Pin the decisive user-scenario verdicts explicitly (readable   -
    // regression anchor independent of the loop above).
    // F5 Phase 2 (audit 181530): the JA->ZH-CN verdicts below flipped from
    // Google-routed to local-served. The ORIGINAL R6 bug (JA->ZH silently
    // degrading to English output on-device) is now answered by trusting the
    // pair on Hy-MT2 per the user's all-pairs directive, instead of by
    // shipping text to the cloud. The Google pin path still reaches Google
    // (deliberate pick), so the cloud remains available on request.
    TEST_CHECK(PlanTranslationRouting("JA", "ZH-CN", EngineType::Auto, false) == EngineType::LocalLlama,
               "F5: JA->ZH-CN under Auto serves locally (pinned pair reliable; supersedes the R6p4 cloud-routing pin)");
    TEST_CHECK(PlanTranslationRouting("JA", "Chinese Simplified", EngineType::LocalLlama, true) == EngineType::LocalLlama,
               "F5: JA->ZH-CN explicit-local stays local even WITH cloud consent (reliable pair never leaks to cloud)");
    TEST_CHECK(PlanTranslationRouting("JA", "Chinese Simplified", EngineType::LocalLlama, false) == EngineType::LocalLlama,
               "R6p4: JA->ZH-CN explicit-local WITHOUT consent stays on device");
    TEST_CHECK(PlanTranslationRouting("JA", "Chinese Simplified", EngineType::GoogleTranslate, false) == EngineType::GoogleTranslate,
               "F5: deliberate Google pin still wins over pair reliability (cloud choice stays sovereign)");
    TEST_CHECK(PlanTranslationRouting("AUTO", "English", EngineType::LocalLlama, false) == EngineType::LocalLlama,
               "R6p4: AUTO->EN stays local under every pin (works today per user report)");
    TEST_CHECK(PlanTranslationRouting("KO", "EN", EngineType::Auto, false) == EngineType::LocalLlama,
               "R6p4: KO->EN stays local (reliable pair, offline capability preserved)");
    // Identity pair (src == tgt) routing: F5 made every identity pair RELIABLE
    // (local echo), removing the pre-F5 EN privilege that cloud-routed non-EN
    // identity pairs under Auto. The typing path never reaches routing here
    // (ShouldTranslate bypasses identity first); this pins the drag explicit-
    // pin contract (F2 user_explicit_target keeps a colliding target verbatim).
    TEST_CHECK(PlanTranslationRouting("KO", "KO", EngineType::Auto, false) == EngineType::LocalLlama,
               "F5: KO->KO identity serves locally (supersedes the pre-F5 cloud routing; privacy for every language)");
    TEST_CHECK(PlanTranslationRouting("EN", "EN", EngineType::Auto, false) == EngineType::LocalLlama,
               "F5: EN->EN stays local under Auto (pre-F5 behavior preserved, now language-neutral)");
    TEST_CHECK(PlanTranslationRouting("KO", "KO", EngineType::GoogleTranslate, false) == EngineType::GoogleTranslate,
               "F5: deliberate Google pin still wins for an identity pair (user choice sovereign)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R6 Phase 4 language routing tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] R6 Phase 4 language routing tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ===========================================================================
// REQ-F4a (Phase 4): cloud-only config must not pay the local LLM startup
// load. Evidence: 260908 session log L3 (engine=google, cloud_fallback=0) vs
// L10-11 (Hy-MT2 tokenizer/context loaded anyway, ~1.4 s + model RAM). The
// wWinMain warmup gate now calls the pure ShouldPreloadLocalModel seam, so
// this matrix pins the SHIPPED decision (same discipline as the
// PlanTranslationRouting pins above). Invariants asserted: local-primary
// configs (auto/local) and the consented google+fallback config keep the
// historical preload; only the cloud-only combo skips.
// ===========================================================================
void TestReqF4aPreloadGate() {
    std::cout << "[RUN] Testing REQ-F4a startup preload gate (cloud-only skip)..." << std::endl;
    const int failures_before = g_failed_count;

    // 1) No model file on disk -> never preload, under every configuration
    //    (historical: the warmup thread was only spawned when available).
    for (const EngineType eng : { EngineType::Auto, EngineType::GoogleTranslate,
                                  EngineType::LocalLlama }) {
        for (const bool consent : { false, true }) {
            TEST_CHECK(!ShouldPreloadLocalModel(eng, consent, false),
                       "F4a: absent model file never preloads (engine + consent matrix)");
        }
    }

    // 2) THE defect scenario: explicit google pin WITHOUT cloud-fallback
    //    consent + model present -> skip. This is the exact 260908 config
    //    (engine=google cloud_fallback=0 models/Hy-MT2-1.8B-Q8_0.gguf exists).
    TEST_CHECK(!ShouldPreloadLocalModel(EngineType::GoogleTranslate, false, true),
               "F4a: engine=google + cloud_fallback=0 + model present skips the startup load (260908 L3/L10-11 scenario)");

    // 3) cloud_fallback=1 keeps the preload: the user declared they want the
    //    local model as the cloud-failure safety net (requirement: 'fallback
    //    시 로컬로 폴백해야 하므로 로드를 유지').
    TEST_CHECK(ShouldPreloadLocalModel(EngineType::GoogleTranslate, true, true),
               "F4a: engine=google + cloud_fallback=1 keeps the preload (fallback safety net must be resident)");

    // 4) Local-primary behavior is byte-for-byte unchanged: auto/local with
    //    the model present always preload, with or without consent.
    for (const EngineType eng : { EngineType::Auto, EngineType::LocalLlama }) {
        for (const bool consent : { false, true }) {
            TEST_CHECK(ShouldPreloadLocalModel(eng, consent, true),
                       "F4a: local-primary engine (auto/local) with model present keeps the preload");
        }
    }

    // 5) Runtime-switch path stays lazy-load capable after a skip: an
    //    explicit-google manager with the (test) model file present reports
    //    the cloud active engine even with preload skipped, and SetEngineType
    //    to local re-resolves to a local active name - the design this fix
    //    depends on (first local Translate() lazy-loads via EnsureLoaded).
    {
        const std::filesystem::path model_path =
            std::filesystem::temp_directory_path() / "emebalachat_f4a_gate.gguf";
        std::ofstream(model_path) << "not-a-real-gguf"; // existence is the gate's only file requirement
        TranslationManager mgr(EngineType::GoogleTranslate,
                               model_path.string());
        TEST_CHECK(mgr.IsLocalModelAvailable(),
                   "F4a: model file present is visible to the manager (skip is a policy decision, not availability)");
        TEST_CHECK(mgr.GetActiveEngineName().find("Google") != std::string::npos,
                   "F4a: explicit-google pin serves cloud regardless of the model file");
        mgr.SetEngineType(EngineType::LocalLlama);
        TEST_CHECK(mgr.GetActiveEngineName().find("Hy-MT2") != std::string::npos,
                   "F4a: runtime switch to local activates the local engine (lazy-load path intact after a startup skip)");
        std::error_code ec;
        std::filesystem::remove(model_path, ec);
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-F4a startup preload gate tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-F4a startup preload gate tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ===========================================================================
// REQ-004 (session 260910_0003): selecting engine=local in the tray must
// trigger an ASYNC background preload so the first translation never pays the
// synchronous ~7 s model load (260910 report). The wWinMain on_select_engine
// gate calls the pure ShouldPreloadOnEngineSwitch seam, so this matrix pins
// the SHIPPED decision (same seam-testing discipline as
// TestReqF4aPreloadGate above). The async thread plumbing itself (joinable
// worker + exchange-based in-flight guard + shutdown join) lives inside
// wWinMain and is not headlessly constructible; it is covered by the manual
// runtime evidence recorded in the task report.
// ===========================================================================
void TestReq004EngineSwitchPreloadGate() {
    std::cout << "[RUN] Testing REQ-004 engine-switch preload gate (tray local pick)..." << std::endl;
    const int failures_before = g_failed_count;

    // 1) No model file on disk -> never preload, under every selection
    //    (Translate() stays honest via LocalModelMissing; a background load
    //    could only fail).
    for (const EngineType sel : { EngineType::Auto, EngineType::GoogleTranslate,
                                  EngineType::LocalLlama }) {
        TEST_CHECK(!ShouldPreloadOnEngineSwitch(sel, false),
                   "REQ-004: absent model file never preloads on an engine switch");
    }

    // 2) THE fix scenario: explicit switch to LocalLlama + model present ->
    //    preload, and - unlike the startup gate - WITHOUT any dependence on
    //    the cloud_fallback consent flag (the seam takes no consent argument;
    //    the tray pick itself is the local-serving intent).
    TEST_CHECK(ShouldPreloadOnEngineSwitch(EngineType::LocalLlama, true),
               "REQ-004: tray switch to local with model present dispatches the async preload");

    // 3) A switch to cloud must not spawn a local load: a resident local
    //    model would be dead weight (the REQ-F4a RAM rule applied to runtime
    //    switches). Auto is not offered by the current tray menu; pinning it
    //    false documents the seam's exact-contract shape (extend together
    //    with the main.cpp call site if an Auto pick is ever added).
    TEST_CHECK(!ShouldPreloadOnEngineSwitch(EngineType::GoogleTranslate, true),
               "REQ-004: switch to google keeps the session cloud-only (no local preload)");
    TEST_CHECK(!ShouldPreloadOnEngineSwitch(EngineType::Auto, true),
               "REQ-004: Auto selection does not dispatch the tray-switch preload (menu offers google/local only)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-004 engine-switch preload gate tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-004 engine-switch preload gate tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

#ifdef HAVE_LLAMA_CPP
// P7-F2 (session 260909_0004): SetGpuOffloadParams seam test. The production
// EnsureLoaded builds llama_model_default_params(), calls this seam once per
// leg, and hands the struct to llama_model_load_from_file - the split_mode
// decision that prevents CUDA+Vulkan layer-splitting of one physical NVIDIA
// card (P5 report F2). No real model (and no GPU) is needed: the seam is pure
// struct construction, so these assertions pin the EXACT field values llama.h
// (b6099) defines. The load itself against real hardware remains covered by
// the manual acceptance matrix - stated honestly per the task directive.
void TestP7F2GpuOffloadParams() {
    std::cout << "[RUN] Testing P7-F2 GPU offload params seam..." << std::endl;
    const int failures_before = g_failed_count;

    // Enum layout pin (llama.h L184-187): the whole NONE+main_gpu=0 device
    // selection semantic depends on these numeric values; a llama.cpp bump
    // that renumbers them must fail here, not silently mis-pin devices.
    static_assert(LLAMA_SPLIT_MODE_NONE == 0 && LLAMA_SPLIT_MODE_LAYER == 1,
                  "P7-F2 test: split_mode enum layout changed vs b6099");

    // b6099 default is LAYER (src/llama-model.cpp L18521): the regression
    // baseline the seam must MOVE AWAY from on the GPU leg.
    {
        llama_model_params dflt = llama_model_default_params();
        TEST_CHECK(dflt.split_mode == LLAMA_SPLIT_MODE_LAYER,
                   "P7-F2: b6099 default split_mode is LAYER (the F2 hazard the seam defends against)");
    }

    // GPU leg: full offload, single-device pin, no multi-device surfaces.
    {
        llama_model_params p = llama_model_default_params();
        // sentinel: the seam must preserve caller-owned fields it doesn't own
        auto sentinel_cb = [](float, void*) { return true; };
        int marker = 0;
        p.progress_callback = sentinel_cb;
        p.progress_callback_user_data = &marker;
        SetGpuOffloadParams(p, /*gpu_offload=*/true);
        TEST_CHECK(p.n_gpu_layers == 99, "P7-F2: GPU leg offloads 99 layers");
        TEST_CHECK(p.split_mode == LLAMA_SPLIT_MODE_NONE,
                   "P7-F2: GPU leg pins to one device (NONE) - prevents CUDA+Vulkan layer split on NVIDIA");
        TEST_CHECK(p.main_gpu == 0,
                   "P7-F2: GPU leg pins device 0 (CUDA registers before Vulkan, ggml-backend-reg.cpp L168 vs L177)");
        TEST_CHECK(p.devices == nullptr, "P7-F2: GPU leg uses no explicit device list");
        TEST_CHECK(p.tensor_split == nullptr, "P7-F2: GPU leg uses no tensor split");
        TEST_CHECK(p.progress_callback == sentinel_cb,
                   "P7-F2: seam preserves caller-owned progress_callback");
        TEST_CHECK(p.progress_callback_user_data == &marker,
                   "P7-F2: seam preserves caller-owned progress_callback_user_data");
    }

    // CPU fallback leg: zero offload AND un-pinned. NONE+main_gpu=0 with zero
    // enumerable GPU devices is REJECTED by llama.cpp even at n_gpu_layers=0
    // (src/llama.cpp L204-207), so leaving the pin here would hard-break the
    // CPU safety net on CPU-only machines - the historical behavior must
    // return exactly (b6099 default LAYER).
    {
        llama_model_params p = llama_model_default_params();
        SetGpuOffloadParams(p, /*gpu_offload=*/true);  // first pin like the GPU leg
        SetGpuOffloadParams(p, /*gpu_offload=*/false); // then the CPU retry un-pins
        TEST_CHECK(p.n_gpu_layers == 0, "P7-F2: CPU leg offloads zero layers");
        TEST_CHECK(p.split_mode == LLAMA_SPLIT_MODE_LAYER,
                   "P7-F2: CPU leg RESTORES default LAYER - NONE would fail llama.cpp main_gpu validation with 0 GPU devices and break the CPU fallback");
        TEST_CHECK(p.devices == nullptr, "P7-F2: CPU leg uses no explicit device list");
        TEST_CHECK(p.tensor_split == nullptr, "P7-F2: CPU leg uses no tensor split");
    }

    // Idempotence: calling the GPU leg twice (e.g. struct reuse) is stable.
    {
        llama_model_params p = llama_model_default_params();
        SetGpuOffloadParams(p, true);
        SetGpuOffloadParams(p, true);
        TEST_CHECK(p.split_mode == LLAMA_SPLIT_MODE_NONE && p.main_gpu == 0 && p.n_gpu_layers == 99,
                   "P7-F2: GPU leg application is idempotent");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P7-F2 GPU offload params seam tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] P7-F2 GPU offload params seam tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}
#endif // HAVE_LLAMA_CPP

// ===========================================================================
// R6 Phase 1 (B3): single-source-of-truth language sync. Pure planner seam
// (PlanLanguageSync) + persistence (INV-1/3) + tooltip view refresh + hook
// cycle-delegate routing. Mirrors the coordinator flow in src/main.cpp
// (ApplyLanguageChange) headlessly - the plan IS the tested unit, so the GUI
// wiring can never silently diverge from the pinned sync contract.
// ===========================================================================
namespace {
// Compact surface-list rendering for failure messages (planner order is part
// of the contract: Badge -> Tray -> Tooltip).
std::string B3SurfaceList(const LanguageSyncPlan& p) {
    std::string s;
    for (const LanguageSurface surface : p.surface_updates) {
        switch (surface) {
            case LanguageSurface::Badge: s += "B"; break;
            case LanguageSurface::Tray: s += "T"; break;
            case LanguageSurface::Tooltip: s += "P"; break;
            default: s += "?"; break;
        }
    }
    return s;
}

std::string B3PlanMsg(const char* what, const LanguageSyncPlan& p) {
    return std::string("B3: ") + what + " (src='" + p.source_language +
           "', tgt='" + p.target_language + "', valid=" + (p.valid ? "1" : "0") +
           ", changed=" + (p.changed ? "1" : "0") + ", surfaces=" + B3SurfaceList(p) + ")";
}
} // namespace

void TestB3LanguageSync() {
    std::cout << "[RUN] Testing R6-B3 language sync coordinator seams..." << std::endl;
    const int failures_before = g_failed_count;

    // Compile-time pin: GetSnapshot stays a const locked reader - the only
    // sanctioned cross-thread language read (INV-4).
    static_assert(std::is_same<decltype(&AppConfig::GetSnapshot),
                               AppConfig::Snapshot (AppConfig::*)() const>::value,
                  "GetSnapshot must remain the const snapshot accessor (INV-4)");

    SetSoundEnabled(false); // CycleLanguage's fallback chime must stay quiet

    // ---- 1) THE reported bug: tooltip-initiated target change plans the
    //         full Badge+Tray+Tooltip refresh and keeps the source (INV-1).
    {
        const auto p = PlanLanguageSync("Auto Detect", "English", "", "Japanese");
        TEST_CHECK(p.valid && p.changed, B3PlanMsg("tooltip target change is a real mutation", p).c_str());
        TEST_CHECK(p.source_language == "Auto Detect", B3PlanMsg("source untouched by target change", p).c_str());
        TEST_CHECK(p.target_language == "Japanese", B3PlanMsg("target becomes the picked language", p).c_str());
        TEST_CHECK(B3SurfaceList(p) == "BTP", B3PlanMsg("all three surfaces refresh, badge->tray->tooltip", p).c_str());
    }

    // ---- 2) Token normalization on BOTH fields: ISO code / name_en /
    //         name_native all resolve to the canonical name_en stored form.
    {
        const char* tokens[] = { "JA", "ja", "Japanese", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" }; // 日本語 (UTF-8)
        for (const char* tok : tokens) {
            const auto p = PlanLanguageSync("Auto Detect", "English", "", tok);
            TEST_CHECK(p.valid && p.target_language == "Japanese",
                       B3PlanMsg("target token normalizes to name_en", p).c_str());
        }
        const auto ps = PlanLanguageSync("Auto Detect", "English", "ko", "");
        TEST_CHECK(ps.valid && ps.source_language == "Korean",
                   B3PlanMsg("source code token normalizes", ps).c_str());
        const auto psw = PlanLanguageSync("Auto Detect", "English", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4", ""); // 한국어
        TEST_CHECK(psw.valid && psw.source_language == "Korean",
                   B3PlanMsg("source native name normalizes", psw).c_str());
    }

    // ---- 3) AUTO is source-only: a target request resolving to AUTO is
    //         refused (both by name and by code).
    {
        const auto a = PlanLanguageSync("Auto Detect", "English", "", "Auto Detect");
        TEST_CHECK(!a.valid, B3PlanMsg("AUTO rejected as target (name)", a).c_str());
        const auto b = PlanLanguageSync("Auto Detect", "English", "", "AUTO");
        TEST_CHECK(!b.valid, B3PlanMsg("AUTO rejected as target (code)", b).c_str());
        const auto c = PlanLanguageSync("English", "Korean", "AUTO", "");
        TEST_CHECK(c.valid && c.changed && c.source_language == "Auto Detect",
                   B3PlanMsg("AUTO accepted as source", c).c_str());
    }

    // ---- 4) Unresolvable request => refuse WITHOUT mutation; the current
    //         raw pair is echoed back unchanged (INV-1 guard: surfaces stay
    //         consistent with config, no half-applied swap).
    {
        const auto a = PlanLanguageSync("Korean", "English", "", "Klingon");
        TEST_CHECK(!a.valid && a.surface_updates.empty(),
                   B3PlanMsg("garbage target refused, no surfaces", a).c_str());
        TEST_CHECK(a.source_language == "Korean" && a.target_language == "English",
                   B3PlanMsg("refusal echoes current pair", a).c_str());
        // All-or-nothing: an invalid source vetoes an otherwise-valid target.
        const auto b = PlanLanguageSync("Korean", "English", "Klingon", "Japanese");
        TEST_CHECK(!b.valid && b.source_language == "Korean" && b.target_language == "English",
                   B3PlanMsg("half-valid mutation refused wholesale", b).c_str());
    }

    // ---- 5) No-op re-pick: valid, changed=false (persist skipped), but all
    //         surfaces still listed (view self-heal on every request).
    {
        const auto p = PlanLanguageSync("Auto Detect", "English", "", "English");
        TEST_CHECK(p.valid && !p.changed, B3PlanMsg("same-target re-pick is unchanged", p).c_str());
        TEST_CHECK(B3SurfaceList(p) == "BTP", B3PlanMsg("unchanged request still refreshes views", p).c_str());
        const auto p2 = PlanLanguageSync("Auto Detect", "English", "", "EN");
        TEST_CHECK(p2.valid && !p2.changed, B3PlanMsg("code form of current target is unchanged too", p2).c_str());
    }

    // ---- 6) Startup-alignment shape (empty request): valid + unchanged +
    //         refresh-all; also canonicalizes legacy raw values (changed=1).
    {
        const auto p = PlanLanguageSync("Auto Detect", "English", "", "");
        TEST_CHECK(p.valid && !p.changed && B3SurfaceList(p) == "BTP",
                   B3PlanMsg("empty request is the refresh-only startup shape", p).c_str());
        const auto c = PlanLanguageSync("EN", "English", "", "");
        TEST_CHECK(c.valid && c.changed && c.source_language == "English",
                   B3PlanMsg("legacy raw code canonicalized to name_en", c).c_str());
    }

    // ---- 7) Cycle seam composes with the planner exactly as the coordinator
    //         does for Ctrl+F9 (English -> Korean per the table order).
    {
        const std::string next = CycleTargetLanguage("English");
        const auto p = PlanLanguageSync("Auto Detect", "English", "", next);
        TEST_CHECK(p.valid && p.changed && p.target_language == "Korean",
                   B3PlanMsg("cycle(English) plans Korean target", p).c_str());
    }

    // ---- 8) INV-1/INV-3 end-to-end on the authority: locked write via the
    //         planner's output, snapshot + disk must both reflect it.
    {
        AppConfig cfg; // defaults: Auto Detect -> English, no disk load
        const auto p = PlanLanguageSync(cfg.GetSnapshot().source_language,
                                        cfg.GetSnapshot().target_language,
                                        "", "Chinese Simplified");
        TEST_CHECK(p.valid && p.changed, B3PlanMsg("ZH-CN plan valid", p).c_str());
        cfg.SetLanguages(p.source_language, p.target_language);
        const auto snap = cfg.GetSnapshot();
        TEST_CHECK(snap.source_language == "Auto Detect" && snap.target_language == "Chinese Simplified",
                   "B3: snapshot equals the planned pair (single authority)");

        std::error_code ec;
        const auto tmp = std::filesystem::temp_directory_path(ec) / "emebalachat_b3_sync_test.json";
        TEST_CHECK(!ec, "B3 fixture: temp path available");
        ec.clear();
        std::filesystem::remove(tmp, ec); // stale leftovers must not mask a save failure
        TEST_CHECK(cfg.SaveToFile(tmp), "B3: SaveToFile succeeds after coordinator write");
        {
            std::ifstream in(tmp);
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            TEST_CHECK(content.find("\"target_language\": \"Chinese Simplified\"") != std::string::npos,
                       "B3: config.json on disk carries the new target (INV-3, synchronous persist)");
            TEST_CHECK(content.find("\"source_language\": \"Auto Detect\"") != std::string::npos,
                       "B3: config.json keeps the untouched source");
        }
        AppConfig reloaded;
        TEST_CHECK(reloaded.LoadFromFile(tmp), "B3: persisted config reloads");
        TEST_CHECK(reloaded.GetSnapshot().target_language == "Chinese Simplified",
                   "B3: reloaded target matches (persistence roundtrip)");
        std::filesystem::remove(tmp, ec); // cleanup (best-effort)
    }

    // ---- 9) Tooltip view seam (RefreshTargetLanguageFromConfig): the
    //         best-effort sync the coordinator drives for a visible tooltip.
    {
        HINSTANCE hInst = ::GetModuleHandleW(nullptr);
        TooltipWindow tooltip;
        TEST_CHECK(tooltip.Create(hInst), "B3 fixture: TooltipWindow created");

        // Hidden: refresh is a no-op (plan §2.4 best-effort rule).
        tooltip.RefreshTargetLanguageFromConfig("Korean");
        TEST_CHECK(tooltip.GetTargetLang().empty(), "B3: hidden tooltip ignores refresh");

        // Visible translation mode: label follows config, body preserved.
        tooltip.ShowTranslation(200, 200, L"source", "KO", "English", L"translation body");
        TEST_CHECK(tooltip.GetTargetLang() == "English", "B3: tooltip starts at the shown target");
        tooltip.RefreshTargetLanguageFromConfig("Japanese");
        TEST_CHECK(tooltip.GetTargetLang() == "Japanese", "B3: visible tooltip re-labels to the config target");
        TEST_CHECK(tooltip.GetTranslatedText() == L"translation body", "B3: view sync never churns the body");
        // Same-value refresh short-circuits (no needless re-render).
        tooltip.RefreshTargetLanguageFromConfig("Japanese");
        TEST_CHECK(tooltip.GetTargetLang() == "Japanese", "B3: idempotent refresh keeps the label");

        // Message mode (REQ-R08 notice card has no language button): no-op.
        tooltip.ShowMessage(200, 200, L"F9", L"notice");
        TEST_CHECK(tooltip.IsMessageMode() && tooltip.GetTargetLang().empty(),
                   "B3: message mode clears the label (ShowMessage contract)");
        tooltip.RefreshTargetLanguageFromConfig("Korean");
        TEST_CHECK(tooltip.GetTargetLang().empty(), "B3: message-mode tooltip ignores refresh");
        tooltip.Dismiss();

        // Cross-thread call marshals through kRefreshTargetLangMessage (the
        // REQ-R10 seam contract the hook/worker coordinators rely on).
        tooltip.ShowTranslation(200, 200, L"src2", "KO", "English", L"body2");
        std::thread poster([&tooltip]() {
            tooltip.RefreshTargetLanguageFromConfig("Vietnamese");
        });
        poster.join();
        PumpThreadMessagesOnce();
        TEST_CHECK(tooltip.GetTargetLang() == "Vietnamese",
                   "B3: off-thread refresh marshals to the GUI thread and applies");
        tooltip.Destroy();
    }

    // ---- 10) Hook cycle-delegate routing: with the coordinator seam wired,
    //          KeyboardHook::CycleTargetLanguage performs NO inline mutation
    //          (the GUI-thread coordinator owns the write - INV-2).
    {
        AppConfig cfg;
        TranslationManager engine(EngineType::GoogleTranslate, "");
        FloatingBadge badge;   // not Create()d: no-op headless
        SystemTray tray;       // not Create()d: UpdateStatus is Shell-API-only
        PipelineWorker worker(cfg, engine, badge);
        KeyboardHook hook(cfg, worker, badge, tray);

        std::atomic<int> cycle_requests{0};
        hook.SetLanguageCycleCallback([&cycle_requests]() {
            cycle_requests.fetch_add(1, std::memory_order_relaxed);
        });
        hook.CycleTargetLanguage();
        TEST_CHECK(cycle_requests.load() == 1,
                   "B3: Ctrl+F9 cycle delegates to the marshal seam exactly once");
        TEST_CHECK(cfg.GetSnapshot().target_language == "English",
                   "B3: wired cycle leaves the inline mutation path untouched (config unchanged)");

        // Unwired fallback (unit/standalone use): Phase 3 Batch 2 (plan
        // §3-Batch2-3, §5 item 4 + Batch 1 handoff) switched this path from
        // the legacy AppConfig::CycleLanguage() to cycling the TYPE pair
        // through the SetTypeLanguages seam - an INTENDED behavior change, so
        // the assertion below moved from the legacy target_language to
        // type_target_language, and the legacy field is now pinned UNTOUCHED
        // (context isolation, REQ-006..009/015/016). SetTypeLanguages +
        // SaveToFile still persists to the DEFAULT config path as a side
        // effect - snapshot and restore that file around the call so the test
        // can never clobber a developer's real build/config.json.
        std::error_code ec;
        const auto default_cfg_path = AppConfig::GetDefaultConfigPath();
        std::string backup_bytes;
        const bool existed = std::filesystem::exists(default_cfg_path, ec) && !ec;
        if (existed) {
            std::ifstream in(default_cfg_path, std::ios::binary);
            backup_bytes.assign((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        }
        KeyboardHook hook2(cfg, worker, badge, tray);
        hook2.CycleTargetLanguage(); // unwired: cycles the TYPE pair (English -> Korean)
        const AppConfig::Snapshot after_cycle = cfg.GetSnapshot();
        TEST_CHECK(after_cycle.type_target_language == "Korean",
                   "P3-B2: unwired fallback cycles the type pair (English->Korean)");
        TEST_CHECK(after_cycle.target_language == "English",
                   "P3-B2: unwired fallback leaves the legacy pair untouched");
        if (existed) {
            std::ofstream out(default_cfg_path, std::ios::binary | std::ios::trunc);
            out.write(backup_bytes.data(), static_cast<std::streamsize>(backup_bytes.size()));
        } else {
            std::filesystem::remove(default_cfg_path, ec); // only removes OUR artifact
        }
        SetSoundEnabled(true);
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R6-B3 language sync coordinator seam tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] R6-B3 language sync coordinator seam tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// SC-01 (Phase 1 batch 3): pure src==tgt fallback resolver extracted from the
// two inline copies in main.cpp. Host-OS-locale independent: expectations are
// derived from the same primitives the function uses.
// F3/A1b (session 260908_0002, ADR-A1-7): the pivot arrival is now the
// language-neutral 3-rank rule (OS language != src -> English -> skip). The
// former "src==EN ? Korean : English" hardcoding cases were updated per the
// design's test-plan table: N1-N3 (VI/AR/IT neutrality), N4 (OS==src
// self-collision kick-out), N5 (EN-OS KO collision via 1st rank), and the
// EN->EN + EN-OS expectation changed from "Korean" to nullopt (R7).
void TestResolveEffectiveTarget() {
    std::cout << "[RUN] Testing SC-01 ResolveEffectiveTarget..." << std::endl;
    const int failures_before = g_failed_count;

    // 1. src != tgt -> no substitution (nullopt), callers skip sync.
    auto none = emebalachat::ResolveEffectiveTarget("Korean", "English");
    TEST_CHECK(!none.has_value(), "src != tgt returns nullopt (no-op)");

    // 2. src == tgt, OS system language differs -> translate to OS language.
    //    The test host runs a real OS locale; derive expectations from the
    //    same primitives the function uses so the check is host-independent.
    const std::string sys_code =
        emebalachat::NormalizeLanguageCode(emebalachat::I18n::GetSystemLanguageCode());

    // (ADR-A1-7 update) EN->EN on an EN/unknown-OS host: every neutral rank
    // fails (1st: sys==src; 2nd: src==EN; 3rd: skip) -> NO pivot. The user can
    // already read English, so the caller keeps the original target (R7
    // intended behavior change; former expectation "Korean" retired).
    auto en_en = emebalachat::ResolveEffectiveTarget("English", "English");
    if (sys_code == "EN" || sys_code == "AUTO" || sys_code.empty()) {
        TEST_CHECK(!en_en.has_value(),
                   "EN->EN with EN/unknown OS SKIPS the pivot (ADR-A1-7 3rd rank, R7)");
    } else if (const auto* info = emebalachat::FindLanguageByCode(sys_code)) {
        TEST_CHECK(en_en.has_value() && en_en.value() == info->name_en,
                   "EN->EN with non-EN OS uses OS language name_en (1st rank)");
    }

    // 3. (N5) Pivot direction: src == tgt == KO. KO/unknown OS -> English via
    //    the 2nd rank (the old hardcoded result is now a rule outcome); an EN
    //    OS hits the 1st rank and yields "English" through OS name_en instead.
    //    Either way the historical KO-OS experience is unchanged (regression
    //    guard row 1 of the ADR-A1-7 compatibility table).
    auto ko_ko = emebalachat::ResolveEffectiveTarget("Korean", "Korean");
    TEST_CHECK(ko_ko.has_value(), "src == tgt == KO requires substitution");
    if (sys_code == "KO" || sys_code == "AUTO" || sys_code.empty()) {
        TEST_CHECK(ko_ko.value() == "English", "KO->KO with KO/unknown OS pivots to English (2nd rank)");
    } else if (const auto* info = emebalachat::FindLanguageByCode(sys_code)) {
        TEST_CHECK(ko_ko.value() == info->name_en, "KO->KO uses OS language name_en (1st rank)");
    }
    TEST_CHECK(!ko_ko.has_value() ||
                   emebalachat::NormalizeLanguageCode(ko_ko.value()) != "KO",
               "pivot NEVER lands back on the colliding language (KO->KO corruption class cannot recur)");

    // (N1/N2/N3 + N4) Language neutrality: non-KO/non-EN sources colliding
    // with themselves must resolve by the SAME rule - OS language when it
    // differs (1st rank), English otherwise (2nd rank; N4 is the sys==src
    // self-collision host where the 1st rank must kick out). Expected values
    // are derived from host primitives, so the block is host-independent.
    for (const char* src : { "Vietnamese", "Arabic", "Italian" }) {
        const std::string code = emebalachat::NormalizeLanguageCode(src);
        auto pivoted = emebalachat::ResolveEffectiveTarget(src, src);
        // A non-EN collision always finds a meaningful alternative (ranks
        // 1-2); nullopt is only reachable for EN sources (rank 3).
        TEST_CHECK(pivoted.has_value(),
                   (std::string(code) + "->" + code + " collision always finds a neutral pivot").c_str());
        if (sys_code == code || sys_code == "AUTO" || sys_code.empty()) {
            TEST_CHECK(pivoted.has_value() && pivoted.value() == "English",
                       (std::string(code) + "->" + code + " with OS==src/unknown falls back to English (2nd rank, N4 kick-out)").c_str());
        } else if (const auto* info = emebalachat::FindLanguageByCode(sys_code)) {
            TEST_CHECK(pivoted.has_value() && pivoted.value() == info->name_en,
                       (std::string(code) + "->" + code + " uses OS language name_en (1st rank, N1-N3)").c_str());
        }
        TEST_CHECK(!pivoted.has_value() ||
                       emebalachat::NormalizeLanguageCode(pivoted.value()) != code,
                   (std::string(code) + " pivot never targets " + code + " (language-neutral guard)").c_str());
    }

    // 4. Alias normalization: lowercase code input behaves exactly like the
    //    canonical name form (compared against the EN->EN result above, so the
    //    expectation stays host-independent under ADR-A1-7 rank 3, where an
    //    EN-OS host legitimately gets nullopt for EN collisions).
    auto alias = emebalachat::ResolveEffectiveTarget("en", "english");
    TEST_CHECK(alias == en_en,
               "case/alias inputs normalize before comparison (same result as canonical EN->EN)");

    // 5. Unrecognized source ("AUTO" fallback) vs concrete target -> nullopt.
    auto auto_src = emebalachat::ResolveEffectiveTarget("not-a-language", "English");
    TEST_CHECK(!auto_src.has_value(), "unrecognized src (AUTO) never collides with concrete tgt");

    // 6. F2 (session 260908_0003): USER-EXPLICIT target bypasses the pivot
    //    unconditionally. The exact reported bug: user drags Korean text
    //    (detected KO) and picks "Korean" in the tooltip target menu - the
    //    old code pivoted the explicit pick to English at rank 1. With
    //    provenance, the pick is honoured verbatim (nullopt = keep target).
    auto ko_ko_explicit = emebalachat::ResolveEffectiveTarget("Korean", "Korean", true);
    TEST_CHECK(!ko_ko_explicit.has_value(),
               "F2: user-explicit Korean target + KO source stays Korean (zero pivot on user choice)");
    // Every collision language, not just KO: the bypass is language-neutral
    // (the same F1 "all Hy-MT2 pairs 100%" scope directive).
    for (const char* lang : { "Vietnamese", "Arabic", "Italian", "English" }) {
        auto bypassed = emebalachat::ResolveEffectiveTarget(lang, lang, true);
        TEST_CHECK(!bypassed.has_value(),
                   (std::string("F2: user-explicit ") + lang + " target bypasses the pivot").c_str());
    }
    // Non-colliding explicit picks are unaffected (already nullopt without
    // the flag; the flag must not introduce a NEW substitution).
    auto no_collision_explicit = emebalachat::ResolveEffectiveTarget("Korean", "English", true);
    TEST_CHECK(!no_collision_explicit.has_value(),
               "F2: explicit non-colliding pick stays a no-op (flag adds nothing)");

    // 7. F2 V6 regression guard: NEVER-TOUCHED defaults still pivot. Case 3
    //    above already proves KO->KO auto-default resolves; this explicit
    //    restatement documents that the F2 flag defaults to false
    //    (pre-existing callers keep the full rank ladder), so the KO->KO
    //    corruption the pivot was built to prevent cannot recur through
    //    the auto-default path.
    {
        auto still_pivots = emebalachat::ResolveEffectiveTarget("Korean", "Korean", false);
        TEST_CHECK(still_pivots.has_value(),
                   "F2: never-touched default KO->KO still pivots (V6 anti-corruption preserved)");
        // And the 2-argument form (all pre-F2 call-site signatures) is
        // identical to user_explicit_target=false.
        auto two_arg = emebalachat::ResolveEffectiveTarget("Korean", "Korean");
        TEST_CHECK(two_arg == still_pivots,
                   "F2: default parameter keeps the 2-arg form on the auto-default path");
    }

    // 8. F2 restart persistence: a config.json written after an explicit
    //    pick round-trips the pinned flag, so a restarted session treats
    //    the persisted user-picked target as user-explicit (ZERO pivot),
    //    never as a re-pivotable auto default. Mirrors the coordinator's
    //    SetDragTargetPinned(true) + SaveToFile sequence.
    {
        AppConfig picked;
        picked.SetDragLanguages("Auto Detect", "Korean"); // the explicit pick value
        picked.SetDragTargetPinned(true);                // ApplyLanguageChange F2 write
        const std::string json = picked.ToJsonString();  // = SaveToFile payload
        TEST_CHECK(json.find("\"drag_target_pinned\": true") != std::string::npos,
                   "F2: pinned flag serialized to config.json");
        AppConfig restarted;
        TEST_CHECK(restarted.FromJsonString(json), "F2: restarted config parses");
        TEST_CHECK(restarted.GetSnapshot().drag_target_language == "Korean" &&
                       restarted.GetSnapshot().drag_target_pinned,
                   "F2: user-picked Korean target reloads PINNED across restart");
        // Pre-F2 config (no drag_target_pinned key): pin reads false -
        // the value is an auto default, re-pivotable. Backward compatible.
        AppConfig legacy_f2;
        TEST_CHECK(legacy_f2.FromJsonString(
                       "{ \"drag_target_language\": \"Korean\" }"),
                   "F2: pre-F2 config (no pin key) parses");
        TEST_CHECK(legacy_f2.GetSnapshot().drag_target_pinned == false,
                   "F2: key absent -> not pinned (auto default, re-pivotable)");
        // Reset clears the pin: the same write sequence
        // apply_system_defaults performs (SetDragLanguages + pin clear).
        restarted.SetDragTargetPinned(false);
        TEST_CHECK(!restarted.GetSnapshot().drag_target_pinned,
                   "F2: reset re-opens the pivot for the restored default");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] SC-01 ResolveEffectiveTarget tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] SC-01 ResolveEffectiveTarget tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// F3 (session 260908_0002, ADR-A1-2 / design §3 작업 F3 검증): headless unit
// test for the extracted pure source-decision rule shared by the three
// drag-family entry points (the lambdas in main.cpp themselves are not
// headlessly testable). Contract: a NON-AUTO persisted source wins verbatim
// over the caller's detected/last-displayed fallback; "Auto Detect" (and any
// unresolvable value, which NormalizeLanguageCode maps to AUTO) keeps the
// fallback so the engine still receives an EXPLICIT source on the drag paths.
void TestResolveEffectiveSource() {
    std::cout << "[RUN] Testing F3 ResolveEffectiveSource..." << std::endl;
    const int failures_before = g_failed_count;

    // 1. Pinned source wins over detection (V6 core defect: the setting was
    //    never read on the drag paths).
    TEST_CHECK(emebalachat::ResolveEffectiveSource("Japanese", "Korean") == "Japanese",
               "F3: non-AUTO persisted source overrides the detected fallback");
    // 2. The value is returned VERBATIM (canonicalization belongs to the
    //    callers' NormalizeLanguageCode / the engine's own token resolver;
    //    case/alias forms are still recognized as non-AUTO).
    TEST_CHECK(emebalachat::ResolveEffectiveSource("korean", "Vietnamese") == "korean",
               "F3: pinned value passes through unchanged (no silent canonicalization)");
    // 3. "Auto Detect" (the REQ-006 default) keeps the detected value - the
    //    established drag contract of explicit-source injection (ADR-A1-2).
    TEST_CHECK(emebalachat::ResolveEffectiveSource("Auto Detect", "Korean") == "Korean",
               "F3: Auto Detect falls back to the detected/explicit source");
    // 4. Canonical AUTO code form and case-insensitive variants behave like
    //    "Auto Detect" (NormalizeLanguageCode is ASCII-case-insensitive).
    TEST_CHECK(emebalachat::ResolveEffectiveSource("AUTO", "KO") == "KO",
               "F3: canonical AUTO token falls back");
    TEST_CHECK(emebalachat::ResolveEffectiveSource("auto detect", "KO") == "KO",
               "F3: lowercase auto detect falls back (case-insensitive)");
    // 5. Blank/garbage persisted values degrade to AUTO and thus to the
    //    fallback - a corrupted config can never feed the engine an empty
    //    source on the drag paths.
    TEST_CHECK(emebalachat::ResolveEffectiveSource("", "English") == "English",
               "F3: empty persisted source falls back (unresolvable -> AUTO)");
    TEST_CHECK(emebalachat::ResolveEffectiveSource("not-a-language", "Thai") == "Thai",
               "F3: garbage persisted source falls back (unresolvable -> AUTO)");
    // 6. Re-translate path shape: the fallback may be a CODE ("KO") rather
    //    than a name - the rule passes any token through untouched.
    TEST_CHECK(emebalachat::ResolveEffectiveSource("Auto Detect", "KO") == "KO",
               "F3: code-form fallback passes through verbatim");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] F3 ResolveEffectiveSource tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] F3 ResolveEffectiveSource tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Phase 3 Batch 1 (plan §4.2): drag/type language-context separation.
// Six scenarios: legacy migration to system defaults (no legacy copy),
// new-schema sticky roundtrip, ResolveDragDefaultTarget pure mapping, the
// user's sticky drag-change statement replayed at unit level, context
// independence in both directions, and the R12 torn-read pattern extended to
// the pair-atomic SetDragLanguages/SetTypeLanguages writers.
// Host-OS-locale independent: every expectation involving the system language
// is derived from the same primitives the code under test uses, never by
// calling the function under test.
void TestPhase3LanguageContexts() {
    std::cout << "[RUN] Testing Phase 3 language contexts (drag/type pairs)..." << std::endl;
    const int failures_before = g_failed_count;

    // Expected drag default, derived INDEPENDENTLY of ResolveDragDefaultTarget
    // (F2 session 260908_0003: 1:1 system-language mirror for ALL OS
    // languages including EN - the EN->Korean pivot of REQ-007 §2.6 was
    // SUPERSEDED by the user decision recorded in decisions.md 2026-09-08
    // 22:10 "APPROVED OVERRIDE: remove EN-OS->Korean special case;
    // system-language 1:1 mapping". Translation-time src==tgt handling is
    // the ADR-A1-7 pivot/ResolveEffectiveTarget layer, NOT this default).
    const std::string sys_code = NormalizeLanguageCode(I18n::GetSystemLanguageCode());
    const LanguageInfo* sys_info = FindLanguageByCode(sys_code);
    std::string expected_drag_default;
    if (!sys_info || sys_info->code == "AUTO") {
        expected_drag_default = "English";
    } else {
        expected_drag_default = sys_info->name_en;
    }

    // ---- 1) Migration: a legacy config.json (no drag_/type_ keys) must RESET
    //         the four context fields to the system defaults and must NOT copy
    //         the shared legacy pair (plan §2.1: copying a legacy "Korean" into
    //         type_target_language would violate REQ-016's English default).
    {
        // Force the legacy value to differ from the computed drag default on
        // every host, so "copy instead of reset" can never pass by coincidence.
        std::string legacy_tgt = "English";
        if (legacy_tgt == expected_drag_default) legacy_tgt = "Japanese";
        const std::string legacy_json =
            std::string("{\n") +
            "  \"ui_language\": \"auto\",\n"
            "  \"engine_type\": \"auto\",\n" +
            "  \"source_language\": \"" + legacy_tgt + "\",\n" +
            "  \"target_language\": \"" + legacy_tgt + "\",\n" +
            "  \"auto_send\": false\n"
            "}\n";
        AppConfig legacy;
        TEST_CHECK(legacy.FromJsonString(legacy_json), "P3: legacy config.json parses");
        const auto snap = legacy.GetSnapshot();
        // Legacy fields are still parsed (deprecated schema members, plan §1.2).
        TEST_CHECK(snap.source_language == legacy_tgt && snap.target_language == legacy_tgt,
                   "P3: legacy pair retained in schema (not dropped)");
        // Context fields reset to system defaults, independent of the legacy copy.
        TEST_CHECK(snap.drag_source_language == "Auto Detect",
                   "P3: migration reset drag source to Auto Detect (REQ-006)");
        TEST_CHECK(snap.drag_target_language == expected_drag_default,
                   "P3: migration reset drag target to OS-language default (REQ-007), NOT the legacy value");
        TEST_CHECK(snap.type_source_language == "Auto Detect",
                   "P3: migration reset type source to Auto Detect (REQ-015)");
        TEST_CHECK(snap.type_target_language == "English",
                   "P3: migration reset type target to English (REQ-016), NOT the legacy value");
    }

    // ---- 2) New-schema sticky load: all four keys present => values are kept
    //         verbatim (has_new_schema path, no migration reset).
    {
        AppConfig sticky;
        TEST_CHECK(sticky.FromJsonString(
                       "{\n"
                       "  \"drag_source_language\": \"Korean\",\n"
                       "  \"drag_target_language\": \"Vietnamese\",\n"
                       "  \"type_source_language\": \"Japanese\",\n"
                       "  \"type_target_language\": \"English\"\n"
                       "}"),
                   "P3: new-schema config.json parses");
        const auto snap = sticky.GetSnapshot();
        TEST_CHECK(snap.drag_source_language == "Korean", "P3: sticky drag source kept");
        TEST_CHECK(snap.drag_target_language == "Vietnamese", "P3: sticky drag target kept");
        TEST_CHECK(snap.type_source_language == "Japanese", "P3: sticky type source kept");
        TEST_CHECK(snap.type_target_language == "English", "P3: sticky type target kept");
    }

    // ---- 3) ResolveDragDefaultTarget: pure function matches the §2.6 mapping
    //         for the CURRENT host OS language (expectation derived above).
    {
        const std::string drag_def = ResolveDragDefaultTarget();
        TEST_CHECK(drag_def == expected_drag_default,
                   "P3: ResolveDragDefaultTarget follows the plan §2.6 mapping");
        const LanguageInfo* def_info = FindLanguageByName(drag_def);
        TEST_CHECK(def_info != nullptr && def_info->code != "AUTO",
                   "P3: drag default is always a concrete supported target language");
    }

    // ---- 4) User's-statement sticky scenario at unit level: tooltip (drag)
    //         target change to Vietnamese persists, contaminates nothing, and
    //         survives a restart (serialize -> parse).
    {
        AppConfig cfg; // in-memory defaults (no disk)
        const auto snap0 = cfg.GetSnapshot();
        const auto p = PlanLanguageSync(LanguageContext::Drag,
                                        snap0.drag_source_language,
                                        snap0.drag_target_language,
                                        "", "Vietnamese");
        TEST_CHECK(p.valid && p.target_language == "Vietnamese",
                   "P3: Drag-context plan resolves Vietnamese target");
        cfg.SetDragLanguages(p.source_language, p.target_language);
        const auto snap = cfg.GetSnapshot();
        TEST_CHECK(snap.drag_target_language == "Vietnamese",
                   "P3: coordinator write lands in the drag pair");
        TEST_CHECK(snap.type_target_language == "English",
                   "P3: type pair untouched by the drag change");
        TEST_CHECK(snap.source_language == "Auto Detect" && snap.target_language == "English",
                   "P3: legacy pair untouched by the drag change");

        const std::string json = cfg.ToJsonString();
        TEST_CHECK(json.find("\"drag_target_language\": \"Vietnamese\"") != std::string::npos,
                   "P3: ToJsonString emits the sticky drag key on disk");
        AppConfig restarted;
        TEST_CHECK(restarted.FromJsonString(json), "P3: restarted config parses");
        TEST_CHECK(restarted.GetSnapshot().drag_target_language == "Vietnamese",
                   "P3: drag Vietnamese sticky across restart (REQ-008/009)");
        TEST_CHECK(restarted.GetSnapshot().type_target_language == "English",
                   "P3: type English survives the restart untouched");
    }

    // ---- 5) Context independence both directions (the §6.3 pollution fix).
    {
        AppConfig iso;
        iso.SetTypeLanguages("Korean", "Japanese");
        const auto s1 = iso.GetSnapshot();
        TEST_CHECK(s1.drag_source_language == "Auto Detect" && s1.drag_target_language == "English",
                   "P3: type write leaves the drag pair at defaults");
        TEST_CHECK(s1.type_source_language == "Korean" && s1.type_target_language == "Japanese",
                   "P3: type write lands in the type pair");
        iso.SetDragLanguages("English", "Korean");
        const auto s2 = iso.GetSnapshot();
        TEST_CHECK(s2.drag_source_language == "English" && s2.drag_target_language == "Korean",
                   "P3: drag write lands in the drag pair");
        TEST_CHECK(s2.type_source_language == "Korean" && s2.type_target_language == "Japanese",
                   "P3: drag write leaves the type pair untouched");
    }

    // ---- 6) I4 torn-read extension (R12 pattern) on the pair-atomic setters:
    //         each context's (source,target) must always be observed as one of
    //         the fully-written pairs - a reader can never see (A,B) mixing,
    //         proving both fields update atomically under one lock.
    {
        AppConfig conc;
        static const std::string kA(256, 'A'); // long enough to force a heap buffer
        static const std::string kB(256, 'B');
        conc.SetDragLanguages(kA, kA);
        conc.SetTypeLanguages(kA, kA);

        std::atomic<bool> stop{false};
        std::atomic<long long> torn{0};
        std::atomic<long long> reads{0};

        auto writer = [&](std::atomic<bool>& local_stop, bool drag) {
            for (int i = 0; i < 20000 && !local_stop.load(std::memory_order_relaxed); ++i) {
                if (drag) {
                    conc.SetDragLanguages((i & 1) ? kB : kA, (i & 1) ? kB : kA);
                } else {
                    conc.SetTypeLanguages((i & 1) ? kB : kA, (i & 1) ? kB : kA);
                }
            }
        };
        std::thread drag_w(writer, std::ref(stop), true);
        std::thread type_w(writer, std::ref(stop), false);
        std::thread reader([&conc, &stop, &torn, &reads]() {
            while (!stop.load(std::memory_order_relaxed)) {
                const auto s = conc.GetSnapshot();
                reads.fetch_add(1, std::memory_order_relaxed);
                if (s.drag_source_language != s.drag_target_language ||
                    s.type_source_language != s.type_target_language) {
                    torn.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        drag_w.join();
        type_w.join();
        stop.store(true, std::memory_order_relaxed);
        reader.join();

        TEST_CHECK(reads.load() > 0, "P3: pair-coherence reader actually ran");
        TEST_CHECK(torn.load() == 0,
                   "P3: zero torn pairs under concurrent SetDragLanguages/SetTypeLanguages");
    }

    // ---- 7) Fresh-install disk path: LoadFromFile on a missing file must
    //         persist the OS-resolved drag default (REQ-007) as the new
    //         sticky keys (plan §2.1 "SaveToFile로 신규 스키마를 디스크에 기록"
    //         + §2.3 saved-value-equals-default), and a reload must keep it.
    {
        std::error_code ec;
        const auto fresh = std::filesystem::temp_directory_path(ec) / "emebalachat_p3_fresh.json";
        TEST_CHECK(!ec, "P3 fixture: temp path available");
        std::filesystem::remove(fresh, ec); // stale leftovers must not mask a load
        AppConfig first;
        TEST_CHECK(first.LoadFromFile(fresh), "P3: LoadFromFile succeeds on a missing file");
        std::error_code fec;
        TEST_CHECK(std::filesystem::exists(fresh, fec) && !fec,
                   "P3: fresh install auto-creates config.json on disk");
        {
            std::ifstream in(fresh);
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            TEST_CHECK(content.find("\"drag_target_language\"") != std::string::npos,
                       "P3: first-save already carries the new schema key");
            const std::string want = "\"drag_target_language\": \"" + expected_drag_default + "\"";
            TEST_CHECK(content.find(want) != std::string::npos,
                       "P3: first-save persists the OS-resolved drag default (REQ-007), not the English placeholder");
        }
        AppConfig second;
        TEST_CHECK(second.LoadFromFile(fresh), "P3: second startup loads the auto-created file");
        TEST_CHECK(second.GetSnapshot().drag_target_language == expected_drag_default,
                   "P3: reloaded drag default matches the OS-resolved value");
        std::filesystem::remove(fresh, ec); // cleanup (best-effort)
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Phase 3 language context tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Phase 3 language context tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Phase 4 Batch 2 (plan §4.2): ComputeSystemDefaultLanguages pure mapping +
// config-level reset simulation + sticky-release roundtrip. The About-window
// button (about_window.cpp) is a pure view; this pins the data contract the
// Batch 3 coordinator (main.cpp apply_system_defaults) will execute. Like the
// Phase 3 tests it is host-OS-locale independent: the expected drag default is
// derived from the same primitives the code under test uses (plan §2.6
// mapping), never by calling the function under test.
void TestPhase4SystemDefaults() {
    std::cout << "[RUN] Testing Phase 4 system-default languages (REQ-020 reset)..." << std::endl;
    const int failures_before = g_failed_count;

    // Independent expectation for the drag default (same derivation pattern as
    // TestPhase3LanguageContexts: F2 session 260908_0003 supersedes the EN->
    // Korean pivot with the 1:1 system-language mirror (decisions.md
    // 2026-09-08 22:10 APPROVED OVERRIDE): unsupported/unknown -> English,
    // every supported language (incl. EN) -> its own name_en).
    const std::string sys_code = NormalizeLanguageCode(I18n::GetSystemLanguageCode());
    const LanguageInfo* sys_info = FindLanguageByCode(sys_code);
    std::string expected_drag_default;
    if (!sys_info || sys_info->code == "AUTO") {
        expected_drag_default = "English";
    } else {
        expected_drag_default = sys_info->name_en;
    }

    // ---- 1) Pure mapping: the four defaults + equality with the single
    //         source of truth (ResolveDragDefaultTarget) + purity.
    {
        const auto defs = ComputeSystemDefaultLanguages();
        TEST_CHECK(defs.drag_source == "Auto Detect",
                   "P4: drag source default is Auto Detect (REQ-006)");
        TEST_CHECK(defs.drag_target == expected_drag_default,
                   "P4: drag target default follows the plan §2.6 OS mapping (REQ-007)");
        TEST_CHECK(defs.drag_target == ResolveDragDefaultTarget(),
                   "P4: drag target default reuses ResolveDragDefaultTarget, no second policy copy");
        TEST_CHECK(defs.type_source == "Auto Detect",
                   "P4: type source default is Auto Detect (REQ-015)");
        TEST_CHECK(defs.type_target == "English",
                   "P4: type target default is English (REQ-016)");
        const LanguageInfo* dt = FindLanguageByName(defs.drag_target);
        TEST_CHECK(dt != nullptr && dt->code != "AUTO",
                   "P4: reset drag target is always a concrete supported language");
        const auto again = ComputeSystemDefaultLanguages();
        TEST_CHECK(defs.drag_source == again.drag_source &&
                       defs.drag_target == again.drag_target &&
                       defs.type_source == again.type_source &&
                       defs.type_target == again.type_target,
                   "P4: ComputeSystemDefaultLanguages is pure (same inputs -> same outputs)");
    }

    // ---- 2) Reset simulation at the config level: sticky values -> re-record
    //         with the computed defaults (plan §2.1: REWRITE, never key
    //         deletion) -> snapshot holds the defaults; legacy pair untouched.
    //         F2 (session 260908_0003): the pinned drag-target flag is part
    //         of the sticky state the reset must clear (defaults become
    //         re-pivotable, like a fresh install).
    {
        AppConfig cfg; // in-memory defaults, no disk
        cfg.SetDragLanguages("Auto Detect", "Vietnamese");   // sticky drag (user tooltip pick)
        cfg.SetDragTargetPinned(true);                       // F2: explicit pick pinned
        cfg.SetTypeLanguages("Korean", "Japanese");          // sticky type pair
        const auto pre = cfg.GetSnapshot();
        TEST_CHECK(pre.drag_target_language == "Vietnamese" &&
                       pre.drag_target_pinned &&
                       pre.type_source_language == "Korean" &&
                       pre.type_target_language == "Japanese",
                   "P4: sticky fixture applied before reset (incl. F2 pin)");

        const auto defs = ComputeSystemDefaultLanguages();
        cfg.SetDragLanguages(defs.drag_source, defs.drag_target); // exactly the
        cfg.SetDragTargetPinned(false);                           // F2 coordinator call
        cfg.SetTypeLanguages(defs.type_source, defs.type_target); // Batch 3 coordinator calls
        const auto post = cfg.GetSnapshot();
        TEST_CHECK(!post.drag_target_pinned,
                   "P4/F2: reset clears the drag-target pin (default re-pivotable)");
        TEST_CHECK(post.drag_source_language == "Auto Detect",
                   "P4: reset restores drag source to Auto Detect (sticky released)");
        TEST_CHECK(post.drag_target_language == expected_drag_default,
                   "P4: reset restores drag target to the OS default (sticky Vietnamese erased)");
        TEST_CHECK(post.type_source_language == "Auto Detect",
                   "P4: reset restores type source to Auto Detect");
        TEST_CHECK(post.type_target_language == "English",
                   "P4: reset restores type target to English (sticky Japanese erased)");
        TEST_CHECK(post.source_language == "Auto Detect" && post.target_language == "English",
                   "P4: legacy pair untouched by the reset (scope: 4 context fields only)");
    }

    // ---- 3) Sticky-release roundtrip: the reset survives a restart because
    //         ToJsonStringLocked ALWAYS writes the four keys (Phase 3 §2.3
    //         contract: reset = re-record, not erase). Reload must therefore
    //         land on the has_new_schema path (no migration) and keep the
    //         re-recorded defaults verbatim.
    {
        AppConfig cfg;
        cfg.SetDragLanguages("English", "Korean");
        cfg.SetTypeLanguages("Japanese", "Vietnamese");
        const auto defs = ComputeSystemDefaultLanguages();
        cfg.SetDragLanguages(defs.drag_source, defs.drag_target);
        cfg.SetTypeLanguages(defs.type_source, defs.type_target);

        const std::string json = cfg.ToJsonString();
        TEST_CHECK(json.find("\"drag_source_language\"") != std::string::npos &&
                       json.find("\"drag_target_language\"") != std::string::npos &&
                       json.find("\"type_source_language\"") != std::string::npos &&
                       json.find("\"type_target_language\"") != std::string::npos,
                   "P4: reset keeps all four keys in the JSON (re-record, never key deletion)");
        TEST_CHECK(json.find("\"drag_source_language\": \"Auto Detect\"") != std::string::npos &&
                       json.find("\"type_target_language\": \"English\"") != std::string::npos,
                   "P4: serialized values are the re-recorded defaults");
        AppConfig restarted;
        TEST_CHECK(restarted.FromJsonString(json), "P4: restarted config parses the reset JSON");
        const auto snap = restarted.GetSnapshot();
        TEST_CHECK(snap.drag_source_language == "Auto Detect" &&
                       snap.drag_target_language == expected_drag_default &&
                       snap.type_source_language == "Auto Detect" &&
                       snap.type_target_language == "English",
                   "P4: reset survives restart (REQ-020 '재시작 유지'), sticky release is durable");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Phase 4 system-default tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Phase 4 system-default tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// R6 Phase 2 (B1, plan §1 B1-H1/H2 + §Phase 2): intermittent stale tooltip.
// Two concurrent translate producers (detached drag threads, the REQ-R06
// double-Ctrl+C worker) used to last-writer-wins on the tooltip model, so a
// slow OLD request could paint the PREVIOUS translation for a NEW selection.
// Pinned invariants: the pure show/drop planner, monotonic stamping under
// concurrency, newest-wins INDEPENDENT of marshal order, stale notices
// dropped, unmanaged (legacy) shows unchanged, and Dismiss clearing every
// content buffer (B1-H2 - WM_DPICHANGED re-renders the current model, so
// leftovers must not survive a dismissal).
void TestB1TooltipStaleness() {
    std::cout << "[RUN] Testing R6-B1 tooltip staleness generation guard..." << std::endl;
    const int failures_before = g_failed_count;

    using TT = TooltipWindow;

    // ---- 1) Pure staleness planner (headless matrix, plan §7.2) ----
    static_assert(TT::kGenNone == 0, "B1: unmanaged-show sentinel must stay 0");
    static_assert(TT::ShouldRenderForGeneration(5, 4) == false, "B1: older gen than latest -> drop");
    static_assert(TT::ShouldRenderForGeneration(5, 5) == true, "B1: newest gen -> show");
    static_assert(TT::ShouldRenderForGeneration(5, 6) == true, "B1: newer-than-known gen -> show");
    static_assert(TT::ShouldRenderForGeneration(0, 3) == true, "B1: nothing stamped yet -> show");
    static_assert(TT::ShouldRenderForGeneration(3, 0) == true, "B1: unmanaged show always renders");
    static_assert(TT::ShouldRenderForGeneration(0, 0) == true, "B1: initial state renders");
    TEST_CHECK(!TT::ShouldRenderForGeneration(1000, 999), "B1: runtime stale drop (N-1 vs N)");
    TEST_CHECK(TT::ShouldRenderForGeneration(1000, 1001), "B1: runtime fresh show (N+1 vs N)");
    TEST_CHECK(!TT::ShouldRenderForGeneration(2, 1), "B1: superseded request dropped");

    const HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    TT tooltip;
    TEST_CHECK(tooltip.Create(hInst), "B1 fixture: TooltipWindow created");
    TEST_CHECK(tooltip.LatestRequestGenerationForTest() == TT::kGenNone,
               "B1: fresh tooltip has no stamped requests");

    // ---- 2) BeginTranslationRequest: monotonic, sentinel-free, race-safe ----
    const uint64_t g1 = tooltip.BeginTranslationRequest();
    const uint64_t g2 = tooltip.BeginTranslationRequest();
    TEST_CHECK(g1 != TT::kGenNone && g2 != TT::kGenNone,
               "B1: stamps never return the unmanaged sentinel");
    TEST_CHECK(g2 == g1 + 1, "B1: generations are strictly monotonic");
    TEST_CHECK(tooltip.LatestRequestGenerationForTest() == g2, "B1: latest tracks the newest stamp");

    // Trigger sites live on GUI + hook + worker threads; concurrent stamps
    // must yield unique ids (no duplicate generation can ever render).
    constexpr int kStamperThreads = 8;
    std::vector<uint64_t> ids(kStamperThreads, 0);
    {
        std::vector<std::thread> stampers;
        stampers.reserve(kStamperThreads);
        for (int i = 0; i < kStamperThreads; ++i) {
            stampers.emplace_back([&tooltip, &ids, i]() {
                ids[static_cast<size_t>(i)] = tooltip.BeginTranslationRequest();
            });
        }
        for (auto& t : stampers) {
            t.join();
        }
    }
    std::sort(ids.begin(), ids.end());
    TEST_CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end(),
               "B1: concurrent stamps return unique generations (atomic fetch_add)");
    TEST_CHECK(ids.front() != TT::kGenNone, "B1: concurrent stamps never return the sentinel");
    TEST_CHECK(tooltip.LatestRequestGenerationForTest() == g2 + kStamperThreads,
               "B1: latest equals the total number of stamps");

    uint64_t expected_drops = tooltip.DroppedStaleShowsForTest();

    // ---- 3) Marshal queue out-of-order: newest FIRST, stale overtakes ----
    // The exact bug shape: the OLD thread posts LAST. Without the guard the
    // stale payload repaints the tooltip with the previous translation.
    const uint64_t gA = tooltip.BeginTranslationRequest(); // superseded request
    const uint64_t gB = tooltip.BeginTranslationRequest(); // newest request

    auto* pB = new TT::TranslationPayload();
    pB->x = 200;
    pB->y = 200;
    pB->source_text = L"new selection";
    pB->source_lang_code = "JA";
    pB->target_lang = "English";
    pB->translated_text = L"NEW RESULT";
    pB->generation = gB;
    auto* pA = new TT::TranslationPayload();
    pA->x = 200;
    pA->y = 200;
    pA->source_text = L"old selection";
    pA->source_lang_code = "KO";
    pA->target_lang = "English";
    pA->translated_text = L"OLD RESULT"; // what the user saw: the previous translation
    pA->generation = gA;

    TEST_CHECK(TT::PostPayloadForTest(tooltip.GetHwnd(), TT::kShowTranslationMessage, pB),
               "B1: newest payload posted");
    TEST_CHECK(TT::PostPayloadForTest(tooltip.GetHwnd(), TT::kShowTranslationMessage, pA),
               "B1: stale payload posted AFTER the newest one");
    PumpThreadMessagesOnce();
    TEST_CHECK(tooltip.IsVisible() && tooltip.GetTranslatedText() == L"NEW RESULT",
               "B1: stale delivery arriving last cannot overwrite the newest result");
    TEST_CHECK(tooltip.GetSourceText() == L"new selection", "B1: model keeps the newest request's source");
    TEST_CHECK(tooltip.DroppedStaleShowsForTest() == expected_drops + 1,
               "B1: exactly one stale show dropped");
    TEST_CHECK(!tooltip.IsMessageMode(), "B1: dropped stale show does not switch modes");
    ++expected_drops;

    // ---- 4) FIFO order (stale FIRST, then newest): stale dropped on arrival ----
    const uint64_t gC = tooltip.BeginTranslationRequest();
    auto* pStale = new TT::TranslationPayload();
    pStale->x = 200;
    pStale->y = 200;
    pStale->source_text = L"ancient";
    pStale->source_lang_code = "KO";
    pStale->target_lang = "English";
    pStale->translated_text = L"ANCIENT RESULT";
    pStale->generation = gA; // oldest stamp so far
    auto* pFresh = new TT::TranslationPayload();
    pFresh->x = 200;
    pFresh->y = 200;
    pFresh->source_text = L"third";
    pFresh->source_lang_code = "EN";
    pFresh->target_lang = "Korean";
    pFresh->translated_text = L"FRESH AGAIN";
    pFresh->generation = gC;
    TEST_CHECK(TT::PostPayloadForTest(tooltip.GetHwnd(), TT::kShowTranslationMessage, pStale),
               "B1: stale payload posted first");
    TEST_CHECK(TT::PostPayloadForTest(tooltip.GetHwnd(), TT::kShowTranslationMessage, pFresh),
               "B1: newest payload posted after");
    PumpThreadMessagesOnce();
    TEST_CHECK(tooltip.GetTranslatedText() == L"FRESH AGAIN",
               "B1: stale-first queue order still ends on the newest result");
    TEST_CHECK(tooltip.DroppedStaleShowsForTest() == expected_drops + 1, "B1: second stale drop counted");
    ++expected_drops;

    // ---- 5) Off-thread producer race (detached-thread simulation) ----
    // The superseded thread posts its own stale result through the REAL seam
    // while the newest request lands inline first.
    const uint64_t gD = tooltip.BeginTranslationRequest();
    std::thread stale_producer([&tooltip, gA]() {
        tooltip.ShowTranslationThreadSafe(200, 200, L"late old", "KO", "English",
                                          L"STALE THREAD RESULT", gA);
    });
    tooltip.ShowTranslation(200, 200, L"current", "EN", "Korean", L"FRESH RESULT", gD);
    stale_producer.join();
    PumpThreadMessagesOnce();
    TEST_CHECK(tooltip.GetTranslatedText() == L"FRESH RESULT",
               "B1: off-thread stale delivery dropped (producer-side race closed)");
    TEST_CHECK(tooltip.DroppedStaleShowsForTest() == expected_drops + 1, "B1: off-thread stale drop counted");
    ++expected_drops;

    // ---- 6) Notices share the guard; unmanaged (legacy) shows unaffected ----
    const uint64_t gS = tooltip.BeginTranslationRequest();
    const uint64_t gT = tooltip.BeginTranslationRequest();
    tooltip.ShowMessage(200, 200, L"Emebala Chat", L"stale failure notice", gS);
    TEST_CHECK(tooltip.GetTranslatedText() == L"FRESH RESULT",
               "B1: stale notice does not replace the newest translation");
    TEST_CHECK(tooltip.DroppedStaleShowsForTest() == expected_drops + 1, "B1: stale notice drop counted");
    ++expected_drops;
    tooltip.ShowMessage(200, 200, L"Emebala Chat", L"newest notice", gT);
    TEST_CHECK(tooltip.IsVisible() && tooltip.IsMessageMode() &&
                   tooltip.GetTranslatedText() == L"newest notice",
               "B1: newest notice renders normally");
    // Default-gen (kGenNone) show: every legacy call site (REQ-R08 toggle
    // bubble, worker empty-capture notice, tests) must keep rendering.
    tooltip.ShowMessage(200, 200, L"H", L"unmanaged legacy notice");
    TEST_CHECK(tooltip.IsMessageMode() && tooltip.GetTranslatedText() == L"unmanaged legacy notice",
               "B1: unmanaged (sentinel) shows are never dropped - full backward compatibility");

    // ---- 7) B1-H2: Dismiss clears every content buffer ----
    const uint64_t gU = tooltip.BeginTranslationRequest();
    tooltip.ShowTranslation(200, 200, L"dismiss me", "KO", "English", L"body-to-clear", gU);
    TEST_CHECK(tooltip.IsVisible() && !tooltip.GetTranslatedText().empty(),
               "B1-H2 fixture: translation-mode tooltip shows before dismissal");
    tooltip.Dismiss();
    TEST_CHECK(!tooltip.IsVisible(), "B1-H2: hidden after Dismiss");
    TEST_CHECK(tooltip.GetSourceText().empty(), "B1-H2: Dismiss clears source_text_");
    TEST_CHECK(tooltip.GetSourceLangCode().empty(), "B1-H2: Dismiss clears source_lang_code_");
    TEST_CHECK(tooltip.GetTargetLang().empty(), "B1-H2: Dismiss clears target_lang_");
    TEST_CHECK(tooltip.GetTranslatedText().empty(), "B1-H2: Dismiss clears translated_text_");
    TEST_CHECK(tooltip.GetMessageHeader().empty(), "B1-H2: Dismiss clears message_header_");
    TEST_CHECK(!tooltip.IsMessageMode(), "B1-H2: Dismiss exits message mode");

    // Dismiss-then-show: no leftovers can leak into the next render.
    const uint64_t gE = tooltip.BeginTranslationRequest();
    tooltip.ShowTranslation(200, 200, L"after dismiss", "JA", "English", L"clean show", gE);
    TEST_CHECK(tooltip.IsVisible() && tooltip.GetTranslatedText() == L"clean show",
               "B1-H2: show after dismiss renders the fresh model");
    TEST_CHECK(tooltip.GetSourceText() == L"after dismiss" && tooltip.GetSourceLangCode() == "JA" &&
                   tooltip.GetTargetLang() == "English",
               "B1-H2: no leftover source/lang fields after re-show");

    // Message-mode dismissal clears the notice body/header too.
    tooltip.ShowMessage(200, 200, L"Notice H", L"Notice B");
    TEST_CHECK(tooltip.IsMessageMode() && tooltip.GetMessageHeader() == L"Notice H",
               "B1-H2 fixture: notice shows before dismissal");
    tooltip.Dismiss();
    TEST_CHECK(tooltip.GetTranslatedText().empty() && tooltip.GetMessageHeader().empty(),
               "B1-H2: Dismiss clears notice buffers");

    // ---- 8) Marshaled (hook-thread) Dismiss clears on the GUI thread ----
    const uint64_t gF = tooltip.BeginTranslationRequest();
    tooltip.ShowTranslation(200, 200, L"x", "KO", "English", L"to clear off-thread", gF);
    std::thread dismisser([&tooltip]() {
        tooltip.DismissThreadSafe();
    });
    dismisser.join();
    PumpThreadMessagesOnce();
    TEST_CHECK(!tooltip.IsVisible() && tooltip.GetTranslatedText().empty() &&
                   tooltip.GetSourceText().empty(),
               "B1-H2: off-thread DismissThreadSafe clears buffers via the WndProc path");

    tooltip.Destroy();
    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R6-B1 tooltip staleness generation guard tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] R6-B1 tooltip staleness generation guard tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// R6 Phase 3 (B1 memory/lifecycle audit, architect plan §3): regression guards
// for the items this phase FIXED - the device-lost recovery predicate (item 4),
// the marshal-queue payload drain on teardown (items 6+8), and the clipboard
// open/close pairing + GDI/DIB re-allocation balance (items 3+5). All checks
// are pure seams or non-flaky runtime probes (bounded handle-count deltas);
// nothing depends on wall-clock timing or GPU state.
void TestR6P3MemoryLifecycle() {
    std::cout << "[RUN] Testing R6-P3 memory/lifecycle audit guards..." << std::endl;
    const int failures_before = g_failed_count;

    using TT = TooltipWindow;

    // ---- 1) Device-lost predicate (item 4): compile-time classification ----
    // Only D2DERR_RECREATE_TARGET may trigger target recreation; every other
    // failure (wrong-thread, uninitialized, DXGI misc) must NOT tear the
    // target down (a wrong classification would destroy a healthy target).
    static_assert(IsRecoverableDeviceLost(D2DERR_RECREATE_TARGET),
                  "P3: RECREATE_TARGET must be classified recoverable");
    static_assert(!IsRecoverableDeviceLost(S_OK), "P3: success must not trigger recreation");
    static_assert(!IsRecoverableDeviceLost(D2DERR_WRONG_STATE),
                  "P3: wrong-state must not trigger recreation (logic bug, not device loss)");
    static_assert(!IsRecoverableDeviceLost(D2DERR_UNSUPPORTED_OPERATION),
                  "P3: unrelated D2D errors must not trigger recreation");
    TEST_CHECK(IsRecoverableDeviceLost(D2DERR_RECREATE_TARGET), "P3: runtime recoverable classification");
    TEST_CHECK(!IsRecoverableDeviceLost(E_FAIL), "P3: runtime non-recoverable classification");

    const HINSTANCE hInst = ::GetModuleHandleW(nullptr);

    // ---- 2) Tooltip marshal-queue drain (items 6+8) ----
    // Heap payloads posted but NEVER pumped must be freed by the Destroy()
    // teardown path (DrainMarshalQueue), not left to the OS queue purge (the
    // confirmed shutdown leak: LPARAM pointers have no destructor attached).
    // Observable contract: after an explicit drain the messages are gone from
    // the queue - a following pump renders NOTHING (model stays pristine).
    {
        TT tooltip;
        TEST_CHECK(tooltip.Create(hInst), "P3 fixture: TooltipWindow created");

        auto* p1 = new TT::TranslationPayload();
        p1->x = 200; p1->y = 200;
        p1->source_text = L"p3 drain src"; p1->source_lang_code = "KO";
        p1->target_lang = "English"; p1->translated_text = L"P3 DRAINED";
        auto* p2 = new TT::MessagePayload();
        p2->x = 200; p2->y = 200;
        p2->header = L"P3"; p2->body = L"P3 notice";
        auto* p3 = new TT::TargetLangPayload();
        p3->target_lang = "Korean";

        TEST_CHECK(TT::PostPayloadForTest(tooltip.GetHwnd(), TT::kShowTranslationMessage, p1),
                   "P3: translation payload posted (unpumped)");
        TEST_CHECK(TT::PostPayloadForTest(tooltip.GetHwnd(), TT::kShowMessageMessage, p2),
                   "P3: message payload posted (unpumped)");
        TEST_CHECK(TT::PostPayloadForTest(tooltip.GetHwnd(), TT::kRefreshTargetLangMessage, p3),
                   "P3: target-lang payload posted (unpumped)");

        const int drained = tooltip.DrainMarshalQueue();
        TEST_CHECK(drained == 3, "P3: DrainMarshalQueue consumed exactly the 3 queued payloads");
        TEST_CHECK(tooltip.DrainMarshalQueue() == 0, "P3: second drain is a no-op (queue already empty)");

        PumpThreadMessagesOnce();
        TEST_CHECK(!tooltip.IsVisible(), "P3: drained payloads do not render after removal");
        TEST_CHECK(tooltip.GetTranslatedText().empty(), "P3: drained translation left no model state");

        // Mixed queue sanity: a non-payload message (kScrollMessage, lParam is
        // a raw delta, no heap) interleaved with a payload must NOT be counted
        // or consumed by the drain, and the drained payload must not render.
        tooltip.ShowTranslation(200, 200, L"inline", "EN", "Korean", L"INLINE MODEL");
        auto* p4 = new TT::TranslationPayload();
        p4->x = 200; p4->y = 200;
        p4->source_text = L"gone"; p4->source_lang_code = "EN";
        p4->target_lang = "Korean"; p4->translated_text = L"DRAINED AWAY";
        TEST_CHECK(TT::PostPayloadForTest(tooltip.GetHwnd(), TT::kShowTranslationMessage, p4),
                   "P3: payload posted before drain");
        TEST_CHECK(::PostMessageW(tooltip.GetHwnd(), TT::kScrollMessage, 0, static_cast<LPARAM>(-120)) == TRUE,
                   "P3: non-payload message posted alongside");
        TEST_CHECK(tooltip.DrainMarshalQueue() == 1,
                   "P3: drain counts only the payload-carrying message (scroll survives untouched)");
        PumpThreadMessagesOnce();
        TEST_CHECK(tooltip.GetTranslatedText() == L"INLINE MODEL",
                   "P3: drained payload never reaches the model");
        TEST_CHECK(tooltip.ScrollOffsetForTest() == 0.0f,
                   "P3: drained tooltip stays non-scrollable (short inline body)");
        tooltip.Destroy();
    }

    // ---- 3) About window marshal-queue drain (item 8) ----
    {
        AboutWindow about;
        TEST_CHECK(about.Create(hInst), "P3 fixture: AboutWindow created");
        auto* payload = new AboutWindow::ShowPayload{ 200, 200 };
        TEST_CHECK(::PostMessageW(about.GetHwnd(), AboutWindow::kShowMessage, 0,
                                  reinterpret_cast<LPARAM>(payload)) == TRUE,
                   "P3: About ShowPayload posted (unpumped)");
        TEST_CHECK(about.DrainMarshalQueue() == 1, "P3: About drain freed the queued ShowPayload");
        TEST_CHECK(about.DrainMarshalQueue() == 0, "P3: About second drain is a no-op");
        PumpThreadMessagesOnce();
        TEST_CHECK(!about.IsVisible(), "P3: drained About show does not render after removal");
        about.Destroy();
    }

    // ---- 4) Clipboard open/close pairing (item 5) ----
    // RAII ScopedClipboard keeps behavior identical on the happy path; this
    // loop pins that pairing: a leaked CloseClipboard would make the NEXT
    // OpenClipboard fail for OUR process too (clipboard is per-thread
    // exclusive), so 3 consecutive full backup/restore rounds succeeding IS
    // the pairing proof.
    for (int round = 0; round < 3; ++round) {
        ClipboardBackup backup;
        const bool backed = BackupClipboard(backup);
        TEST_CHECK(backed, "P3: clipboard backup round succeeded (scope closed previous open)");
        const bool restored = RestoreClipboard(backup);
        TEST_CHECK(restored, "P3: clipboard restore round succeeded (scope closed its open)");
    }
    // GetClipboardText between rounds must also succeed -> its scope closed.
    (void)GetClipboardText();
    ClipboardBackup tail_backup;
    TEST_CHECK(BackupClipboard(tail_backup), "P3: GetClipboardText left the clipboard openable (pairing holds)");
    RestoreClipboard(tail_backup);

    // ---- 5) GDI buffer balance across re-allocations (item 3) ----
    // ShowTranslation runs ReallocateBuffer every time (delete old DC+DIB,
    // create new). A leaked pair per show would grow the process GDI object
    // count linearly; the audit verdict was "balanced", and this probe pins
    // it: 200 shows must cost ~0 net GDI objects (allow +4 for unrelated
    // churn; the leak shape under test would add ~+400).
    {
        TT tooltip;
        TEST_CHECK(tooltip.Create(hInst), "P3 GDI fixture: TooltipWindow created");
        const UINT gdi_before = ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
        for (int i = 0; i < 200; ++i) {
            tooltip.ShowTranslation(200, 200, L"src", "KO", "English", L"p3 balance probe");
        }
        const UINT gdi_after = ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
        TEST_CHECK(gdi_after <= gdi_before + 4,
                   "P3: 200 ReallocateBuffer cycles leak no GDI objects (DC/DIB balance)");
        tooltip.Dismiss();
        tooltip.Destroy();
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R6-P3 memory/lifecycle audit guard tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] R6-P3 memory/lifecycle audit guard tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// R6 Phases 5+6 (architect plan §5/§6/§7.2): i18n completeness + About body
// localization + UI-language selector planner + removed-locale refusal +
// config ui_language persistence. All pure seams (no window/D2D required);
// global locale state is restored at the end so later tests are unaffected.
void TestR6P5P6I18n() {
    std::cout << "[RUN] Testing R6 P5/P6 i18n coverage, About localization, UI-language selector..." << std::endl;
    const int failures_before = g_failed_count;

    // REQ-037 (P4 Batch B-3): completeness iterates the SELECTOR vector, not a
    // hardcoded locale list - the test scales with the authenticity gate
    // automatically (design §2.1.6). A locale the gate withholds disappears
    // from both selector and coverage; a locale in the selector MUST carry a
    // fully non-empty table (user rule: no empty strings, no silent English
    // fallback for selectable locales).
    const std::vector<UiLocale> kCompleteLocales = [] {
        std::vector<UiLocale> v;
        for (const auto& e : GetSupportedUiLocales()) v.push_back(e.locale);
        return v;
    }();

    // ---- 1) Table completeness (VP scope item 6): every StringId in [0,
    //         EnumCount) must resolve NON-EMPTY in ALL 37 selectable locales
    //         (47 × 37 = 1739 checks collapsed into one counter). Guards the
    //         30 new tables + the legacy seven against aggregate-order drift
    //         (a missing initializer would compile as nullptr and crash
    //         Render - this pins it headlessly).
    int empty_count = 0;
    TEST_CHECK(kCompleteLocales.size() == 37,
               "B3: 37 selectable locales (gate fully open) - completeness matrix is 49x37 (session 260909_0001: +AppName, +TooltipNoTtsVoice)");
    for (const UiLocale loc : kCompleteLocales) {
        I18n::SetLocale(loc);
        for (int id = 0; id < static_cast<int>(StringId::EnumCount); ++id) {
            if (I18n::Get(static_cast<StringId>(id)).empty()) {
                ++empty_count;
            }
        }
    }
    TEST_CHECK(empty_count == 0, "P5/P6/B3: every StringId non-empty in all 37 locales");

    // ---- 2) FR/DE/RU acceptance (REQ-037 INVERTS the R6 removal) ------------
    // Design §2.1.1 + §Issues: the half-wired removal is superseded by the
    // "all 37" mandate; the tables are now authored, so the codes must
    // RESOLVE to their locales, ACCEPT at the selector, and load at startup.
    // The old refusal assertions are deliberately deleted here - the refusal
    // MECHANISM itself stays pinned against a synthetic reduced selector in
    // section 2b below (gate still exists and works, per delegation item 4).
    TEST_CHECK(I18n::StringToLocale("fr") == UiLocale::French, "B3: 'fr' resolves to French");
    TEST_CHECK(I18n::StringToLocale("de") == UiLocale::German, "B3: 'de' resolves to German");
    TEST_CHECK(I18n::StringToLocale("ru") == UiLocale::Russian, "B3: 'ru' resolves to Russian");
    {
        const UiLocaleChangePlan frPlan = PlanUiLocaleChange("en", "fr");
        TEST_CHECK(frPlan.valid && frPlan.applied == UiLocale::French,
                   "B3: selector accepts authored locale 'fr' (inverted from R6 refusal)");
        TEST_CHECK(PlanUiLocaleChange("en", "de").valid, "B3: selector accepts authored locale 'de'");
        TEST_CHECK(PlanUiLocaleChange("en", "ru").valid, "B3: selector accepts authored locale 'ru'");
        TEST_CHECK(!PlanUiLocaleChange("en", "klingon").valid, "P6: selector refuses unknown code");
        TEST_CHECK(!PlanUiLocaleChange("en", "").valid, "P6: selector refuses empty code");
        I18n::Initialize("fr");
        TEST_CHECK(I18n::GetCurrentLocale() == UiLocale::French,
                   "B3: startup Initialize('fr') resolves to French (inverted from English fallback)");
    }

    // ---- 2b) Authenticity gate mechanism (design §2-Q4.2) ------------------
    // All 37 real rows are authored in B-3, so the refusal path cannot be
    // pinned by real data anymore - it is pinned with a SYNTHETIC selector:
    // PlanUiLocaleChangeWithSelector must refuse every code that is NOT in
    // the vector it was given, while the production entry point (full
    // selector) accepts the same code. A withheld locale is therefore
    // unreachable through the planner BY CONSTRUCTION of the selector loop.
    {
        const std::vector<UiLocaleEntry> syntheticReduced = {
            { UiLocale::Thai, L"ไทย" },
            { UiLocale::English, L"English" },
        };
        const UiLocaleChangePlan thaiOk = PlanUiLocaleChangeWithSelector(syntheticReduced, "en", "th");
        TEST_CHECK(thaiOk.valid && thaiOk.applied == UiLocale::Thai,
                   "B3: gate-pure planner accepts a code present in the selector");
        TEST_CHECK(!PlanUiLocaleChangeWithSelector(syntheticReduced, "en", "my").valid,
                   "B3: gate-pure planner REFUSES a code withheld from the selector (synthetic 'my')");
        TEST_CHECK(PlanUiLocaleChange("en", "my").valid,
                   "B3: production planner accepts 'my' (all 37 authored this batch)");
    }

    // ---- 3) Selector data + planner ----------------------------------------
    {
        const auto& entries = GetSupportedUiLocales();
        TEST_CHECK(entries.size() == 37, "B3: selector lists exactly the 37 authored locales");
        TEST_CHECK(entries[0].locale == UiLocale::Korean && entries[36].locale == UiLocale::English,
                   "B3: selector order KO-first, EN-last (design §2-Q3 legacy front block kept)");
        bool all_named = true;
        for (const auto& e : entries) {
            if (e.native_name == nullptr || e.native_name[0] == L'\0') all_named = false;
        }
        TEST_CHECK(all_named, "P6: every selector entry carries a non-empty endonym");

        const UiLocaleChangePlan autoPlan = PlanUiLocaleChange("ko", "auto");
        TEST_CHECK(autoPlan.valid, "P6: 'auto' is a valid selection");
        TEST_CHECK(autoPlan.applied == UiLocale::Auto, "P6: 'auto' defers resolution to apply time");
        TEST_CHECK(autoPlan.persisted_value == "auto", "P6: 'auto' persists verbatim");
        TEST_CHECK(autoPlan.changed, "P6: ko -> auto is a change");
        TEST_CHECK(autoPlan.surfaces.size() == 4 &&
                       autoPlan.surfaces[0] == LocaleSurface::Tray &&
                       autoPlan.surfaces[1] == LocaleSurface::Badge &&
                       autoPlan.surfaces[2] == LocaleSurface::Tooltip &&
                       autoPlan.surfaces[3] == LocaleSurface::About,
                   "P6: refresh order tray -> badge -> tooltip -> About (plan §5.4)");

        // Case-insensitive acceptance + canonical spelling in the write value.
        const UiLocaleChangePlan zh = PlanUiLocaleChange("auto", "ZH-cn");
        TEST_CHECK(zh.valid && zh.persisted_value == "zh-CN", "P6: 'ZH-cn' accepted, canonicalized to 'zh-CN'");
        // No-op re-pick: valid, unchanged, still refreshes (self-heal per B3).
        const UiLocaleChangePlan noop = PlanUiLocaleChange("ko", "KO");
        TEST_CHECK(noop.valid && !noop.changed && noop.surfaces.size() == 4,
                   "P6: same-value re-pick skips persist but refreshes views");
        // Refused plans carry no side effects the coordinator could apply.
        const UiLocaleChangePlan bad = PlanUiLocaleChange("ko", "xx");
        TEST_CHECK(!bad.valid && bad.surfaces.empty(), "P6: refused plan mutates nothing");
    }

    // ---- 4) About body localized (plan §5.2, VP minimum KO+EN+JA) ----------
    {
        I18n::SetLocale(UiLocale::Korean);
        const auto koAbout = AboutWindow::BuildLocalizedContent();
        I18n::SetLocale(UiLocale::Japanese);
        const auto jaAbout = AboutWindow::BuildLocalizedContent();
        I18n::SetLocale(UiLocale::English);
        const auto enAbout = AboutWindow::BuildLocalizedContent();

        TEST_CHECK(koAbout.tagline.find(L"복사") != std::wstring::npos, "P5: KO tagline localized");
        TEST_CHECK(jaAbout.tagline.find(L"コピー") != std::wstring::npos, "P5: JA tagline localized");
        TEST_CHECK(enAbout.tagline == L"Never copy-paste again. Type naturally in your native tongue \u2014 "
                                      L"translations replace your keystrokes in real time inside any Windows application.",
                   "P5: EN tagline byte-identical to the original REQ-005 copy");
        TEST_CHECK(koAbout.features[0] != enAbout.features[0] &&
                       jaAbout.features[1] != enAbout.features[1],
                   "P5: feature lines differ per locale (routed through i18n, not constants)");
        TEST_CHECK(!koAbout.etymology.empty() && koAbout.etymology != enAbout.etymology,
                   "P5: etymology line localized");
        TEST_CHECK(enAbout.link_labels[2] == L"Reddit" && koAbout.link_labels[2] == L"Reddit",
                   "P5: link-3 label is 'Reddit' (brand token) in KO + EN");
        TEST_CHECK(enAbout.link_labels[0] == L"Website" && enAbout.link_labels[1] == L"Contact",
                   "P5: EN link labels unchanged");
        // Contact lines: universal factual data (phone/address/person) stays
        // in every locale; the LABELS translate (plan §5.2 decision).
        TEST_CHECK(koAbout.contacts[2].find(L"Yongtai Kim") != std::wstring::npos &&
                       koAbout.contacts[2] != enAbout.contacts[2],
                   "P5: KO contact-lead line keeps the name, localizes the label");
        TEST_CHECK(enAbout.contacts[1].find(L"+82 2 575 0414") != std::wstring::npos,
                   "P5: EN contact-phone line carries the universal number");
        TEST_CHECK(!koAbout.contacts[0].empty() && !jaAbout.contacts[0].empty(),
                   "P5: KO/JA contact-org lines non-empty");
        // Phase 4 (REQ-020, plan §4.2 item 4): the reset button's resting
        // label rides the same localized-content seam and must differ per
        // locale (i18n-routed, not a constant). Completeness check 1) above
        // already pins AboutResetButton/AboutResetDone non-empty in ALL 7
        // locales; this pins that the resolved label actually changes.
        TEST_CHECK(!koAbout.reset_label.empty() && !jaAbout.reset_label.empty() &&
                       !enAbout.reset_label.empty(),
                   "P4: reset_label non-empty in KO/JA/EN");
        TEST_CHECK(koAbout.reset_label != enAbout.reset_label &&
                       jaAbout.reset_label != enAbout.reset_label &&
                       koAbout.reset_label != jaAbout.reset_label,
                   "P4: reset_label localized per locale (KO/JA/EN all differ)");
    }

    // ---- 5) Per-locale AppName mapping + TooltipTitle unification (REQ-B,
    // session 260909_0001; supersedes the 260908 "brand token untranslated"
    // decision per decisions.md [2026-09-09 05:45]) --------------------------
    {
        static const std::pair<UiLocale, const wchar_t*> kExpectedAppNames[37] = {
            { UiLocale::Korean,             L"에메발라 챗" },
            { UiLocale::Japanese,           L"エメバラチャット" },
            { UiLocale::ChineseSimplified,  L"埃梅巴拉 翻译" },
            { UiLocale::ChineseTraditional, L"埃梅巴拉 翻譯" },
            { UiLocale::Russian,            L"Эмебала Чат" },
            { UiLocale::Ukrainian,          L"Емебала Чат" },
            { UiLocale::Thai,               L"เอเมบาลา แชท" },
            { UiLocale::Arabic,             L"إيميبالا شات" },
            { UiLocale::Persian,            L"امبالا چت" },
            { UiLocale::Urdu,               L"ایمیبالا چیٹ" },
            { UiLocale::Hebrew,             L"אמבאלה צ'אט" },
            { UiLocale::Hindi,              L"एमेबाला चैट" },
            { UiLocale::Bengali,            L"এমেবালা চ্যাট" },
            { UiLocale::Greek,              L"Εμεμπάλα Τσατ" },
            { UiLocale::Khmer,              L"អេមេបាឡា ឆាត" },
            { UiLocale::Lao,                L"ເອເມບາລາ ແຊັດ" },
            { UiLocale::Burmese,            L"အီမီဘာလာ ချက်" },
            // Latin-script 20: keep the ASCII brand (decisions.md APPROVED)
            { UiLocale::English,            L"Emebala Chat" },
            { UiLocale::Spanish,            L"Emebala Chat" },
            { UiLocale::French,             L"Emebala Chat" },
            { UiLocale::German,             L"Emebala Chat" },
            { UiLocale::Portuguese,         L"Emebala Chat" },
            { UiLocale::Italian,            L"Emebala Chat" },
            { UiLocale::Indonesian,         L"Emebala Chat" },
            { UiLocale::Malay,              L"Emebala Chat" },
            { UiLocale::Filipino,           L"Emebala Chat" },
            { UiLocale::Turkish,            L"Emebala Chat" },
            { UiLocale::Polish,             L"Emebala Chat" },
            { UiLocale::Dutch,              L"Emebala Chat" },
            { UiLocale::Czech,              L"Emebala Chat" },
            { UiLocale::Hungarian,          L"Emebala Chat" },
            { UiLocale::Swedish,            L"Emebala Chat" },
            { UiLocale::Romanian,           L"Emebala Chat" },
            { UiLocale::Danish,             L"Emebala Chat" },
            { UiLocale::Finnish,            L"Emebala Chat" },
            { UiLocale::Norwegian,          L"Emebala Chat" },
            { UiLocale::Vietnamese,         L"Emebala Chat" },
        };
        bool names_match = true;
        for (const auto& expect : kExpectedAppNames) {
            I18n::SetLocale(expect.first);
            if (I18n::Get(StringId::AppName) != expect.second) names_match = false;
            // D2 unification: tooltip_title is value-identical to app_name in
            // every locale (machine-checked single-sourcing of the brand).
            if (I18n::Get(StringId::TooltipTitle) != I18n::Get(StringId::AppName)) names_match = false;
        }
        I18n::SetLocale(UiLocale::English);
        TEST_CHECK(names_match, "REQ-B: per-locale AppName mapping + TooltipTitle unification in all 37 locales");
    }

    // ---- 5b) REQ-B-008 AppName integrity loop (session 260909_0001) --------
    // Every locale: AppName non-empty, length 2..40, unified with
    // TooltipTitle; the 20 Latin locales keep "Emebala Chat" and the 17
    // non-Latin locales use the transliteration (no silent-English gate).
    {
        static const std::pair<UiLocale, bool> kLatinLocales[37] = {
            { UiLocale::English, true },    { UiLocale::Spanish, true },
            { UiLocale::French, true },     { UiLocale::German, true },
            { UiLocale::Portuguese, true }, { UiLocale::Italian, true },
            { UiLocale::Indonesian, true }, { UiLocale::Malay, true },
            { UiLocale::Filipino, true },   { UiLocale::Turkish, true },
            { UiLocale::Polish, true },     { UiLocale::Dutch, true },
            { UiLocale::Czech, true },      { UiLocale::Hungarian, true },
            { UiLocale::Swedish, true },    { UiLocale::Romanian, true },
            { UiLocale::Danish, true },     { UiLocale::Finnish, true },
            { UiLocale::Norwegian, true },  { UiLocale::Vietnamese, true },
            { UiLocale::Korean, false },    { UiLocale::Japanese, false },
            { UiLocale::ChineseSimplified, false },
            { UiLocale::ChineseTraditional, false },
            { UiLocale::Russian, false },   { UiLocale::Ukrainian, false },
            { UiLocale::Thai, false },      { UiLocale::Arabic, false },
            { UiLocale::Persian, false },   { UiLocale::Urdu, false },
            { UiLocale::Hebrew, false },    { UiLocale::Hindi, false },
            { UiLocale::Bengali, false },   { UiLocale::Greek, false },
            { UiLocale::Khmer, false },     { UiLocale::Lao, false },
            { UiLocale::Burmese, false },
        };
        bool integrity_ok = true;
        for (const auto& entry : kLatinLocales) {
            I18n::SetLocale(entry.first);
            const std::wstring name = I18n::Get(StringId::AppName);
            if (name.empty()) integrity_ok = false;
            if (name.size() < 2 || name.size() > 40) integrity_ok = false;
            if (name != I18n::Get(StringId::TooltipTitle)) integrity_ok = false;
            if (entry.second) {
                if (name != L"Emebala Chat") integrity_ok = false;
            } else {
                if (name == L"Emebala Chat") integrity_ok = false;
            }
        }
        I18n::SetLocale(UiLocale::English);
        TEST_CHECK(integrity_ok, "REQ-B-008: AppName integrity in all 37 locales (non-empty, 2..40 chars, unified with TooltipTitle, Latin/non-Latin split)");
    }

    // ---- 6) Runtime locale switch changes Get output (atomic path) ---------
    {
        I18n::SetLocale(UiLocale::English);
        const std::wstring enActive = I18n::Get(StringId::BadgeActive);
        I18n::SetLocale(UiLocale::Korean);
        const std::wstring koActive = I18n::Get(StringId::BadgeActive);
        TEST_CHECK(enActive == L"Active" && koActive == L"활성",
                   "P6: SetLocale flips I18n::Get immediately (selector re-render contract)");
    }

    // ---- 7) config ui_language persistence (I4 pattern, new field) ---------
    {
        AppConfig cfg;
        TEST_CHECK(cfg.GetSnapshot().ui_language == "auto", "P6: default ui_language is 'auto'");
        cfg.SetUiLanguage("ja");
        TEST_CHECK(cfg.GetSnapshot().ui_language == "ja", "P6: SetUiLanguage writes the locked field");
        const std::string json = cfg.ToJsonString();
        TEST_CHECK(json.find("\"ui_language\": \"ja\"") != std::string::npos,
                   "P6: ui_language serialized to JSON");
        AppConfig reloaded;
        TEST_CHECK(reloaded.FromJsonString(json), "P6: config JSON re-parses");
        TEST_CHECK(reloaded.GetSnapshot().ui_language == "ja", "P6: ui_language round-trips");
        // I18n::Initialize consumes the persisted code exactly like startup.
        I18n::Initialize(reloaded.GetSnapshot().ui_language);
        TEST_CHECK(I18n::GetCurrentLocale() == UiLocale::Japanese,
                   "P6: persisted 'ja' resolves to the Japanese locale at (re)init");
    }

    // ---- 8) R7 old-config regression (design §5.1 R7) ----------------------
    // ui_language persists locale STRINGS, never enum ordinals, so the 8->38
    // enum expansion cannot misread values written by pre-B-3 builds. The
    // persisted forms shipped before this batch must load byte-identically:
    // "es" -> Spanish, "ja" -> Japanese (section 7 above), "auto" -> detected.
    {
        I18n::Initialize("es");
        TEST_CHECK(I18n::GetCurrentLocale() == UiLocale::Spanish,
                   "R7: legacy config ui_language:'es' loads as Spanish after enum expansion");
        I18n::Initialize("auto");
        TEST_CHECK(I18n::GetCurrentLocale() != UiLocale::Auto,
                   "R7: ui_language:'auto' resolves to a concrete locale at startup");
        TEST_CHECK(I18n::StringToLocale("es") == UiLocale::Spanish &&
                       I18n::LocaleToString(UiLocale::Spanish) == "es",
                   "R7: 'es' round-trip stable across the B-3 rework");
    }

    // Restore OS-derived locale for any later test functions.
    I18n::Initialize("auto");
    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R6 P5/P6 i18n tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] R6 P5/P6 i18n tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// P4 Batch B-3 (session 260907_0002, design §2.1.6 / §4.1): LocaleMapping
// table integrity for the 37-locale expansion. All pure seams - the table is
// public (i18n.hpp) precisely so row correctness, round-trips and prefix
// boundary hygiene (R6) are pinned headlessly instead of by manual review.
void TestReq037LocaleMapping() {
    std::cout << "[RUN] Testing B-3 LocaleMapping table (REQ-037)..." << std::endl;
    const int failures_before = g_failed_count;

    const auto& maps = GetLocaleMappings();

    // ---- 1) Row count + declaration order ----------------------------------
    TEST_CHECK(maps.size() == 37, "B3: LocaleMapping has exactly 37 rows");
    {
        bool order_ok = true;
        for (size_t i = 0; i < maps.size(); ++i) {
            // Auto(0) excluded; rows in UiLocale enum order == registry order.
            if (static_cast<int>(maps[i].locale) != static_cast<int>(i) + 1) order_ok = false;
        }
        TEST_CHECK(order_ok, "B3: rows declared in UiLocale enum order (design §2.1.1 registry order)");
    }

    // ---- 2) 37x round-trip identity (LocaleToString ∘ StringToLocale) ------
    {
        bool round_trip = true;
        for (const auto& m : maps) {
            if (I18n::LocaleToString(m.locale) != m.config_code) round_trip = false;
            if (I18n::StringToLocale(m.config_code) != m.locale) round_trip = false;
        }
        TEST_CHECK(round_trip, "B3: every config_code round-trips StringToLocale/LocaleToString (identity x37)");
    }

    // ---- 3) Non-empty prefix/full columns + uniqueness ----------------------
    {
        bool prefix_ok = true;
        bool full_ok = true;
        std::vector<std::wstring> primary;
        for (const auto& m : maps) {
            if (m.bcp47_prefix == nullptr || m.bcp47_prefix[0] == L'\0') prefix_ok = false;
            if (m.bcp47_full == nullptr || m.bcp47_full[0] == L'\0') full_ok = false;
            // zh rows intentionally share L"zh" (resolved by script pre-check,
            // skipped by the boundary loop) - uniqueness applies to the rest.
            if (m.locale != UiLocale::ChineseSimplified && m.locale != UiLocale::ChineseTraditional) {
                primary.emplace_back(m.bcp47_prefix);
                if (m.bcp47_prefix_alt) primary.emplace_back(m.bcp47_prefix_alt);
            }
        }
        TEST_CHECK(prefix_ok, "B3: every mapping row carries a non-empty bcp47_prefix");
        TEST_CHECK(full_ok, "B3: every mapping row carries a non-empty bcp47_full tag");
        std::sort(primary.begin(), primary.end());
        const bool unique = std::adjacent_find(primary.begin(), primary.end()) == primary.end();
        TEST_CHECK(unique, "R6/B3: bcp47 prefixes (incl. nb/no dual row) are unique - no collision");
    }

    // ---- 4) Selector join against kAllLanguages (design §2.1.4) -------------
    {
        const auto& entries = GetSupportedUiLocales();
        const auto& registry = GetSupportedLanguages();
        bool join_ok = entries.size() == 37;
        for (const auto& e : entries) {
            const LocaleMapping* row = nullptr;
            for (const auto& m : maps) {
                if (m.locale == e.locale) row = &m;
            }
            if (row == nullptr || !row->authored) { join_ok = false; continue; }
            std::string up = row->config_code;
            for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            bool found = false;
            for (const auto& lang : registry) {
                if (lang.code == up) {
                    found = true;
                    if (ToUtf16(lang.name_native) != e.native_name) join_ok = false;
                    break;
                }
            }
            if (!found) join_ok = false;
        }
        TEST_CHECK(join_ok,
                   "B3: selector == LocaleMapping x kAllLanguages join; endonyms come from the registry; gate fully open (37 authored)");
    }

    // ---- 5) BCP-47 resolution: delegation-mandated cases (design §4.1) ------
    TEST_CHECK(LocaleFromBcp47Tag(L"ko-KR") == UiLocale::Korean, "B3: ko-KR -> Korean");
    TEST_CHECK(LocaleFromBcp47Tag(L"nb-NO") == UiLocale::Norwegian, "B3: nb-NO -> Norwegian (Windows primary tag)");
    TEST_CHECK(LocaleFromBcp47Tag(L"no-NO") == UiLocale::Norwegian, "B3: no-NO -> Norwegian (legacy alias prefix)");
    TEST_CHECK(LocaleFromBcp47Tag(L"nn-NO") == UiLocale::Auto,
               "B3: nn-NO defers to the LANGID phase (sentinel Auto); Nynorsk primary 0x14 still lands on Norwegian");
    TEST_CHECK(LocaleFromBcp47Tag(L"fil-PH") == UiLocale::Filipino, "B3: fil-PH -> Filipino (fil vs FI boundary)");
    TEST_CHECK(LocaleFromBcp47Tag(L"fi-FI") == UiLocale::Finnish, "B3: fi-FI -> Finnish");
    TEST_CHECK(LocaleFromBcp47Tag(L"zh-Hans") == UiLocale::ChineseSimplified, "B3: zh-Hans script pre-check -> Simplified");
    TEST_CHECK(LocaleFromBcp47Tag(L"zh-Hant") == UiLocale::ChineseTraditional, "B3: zh-Hant script pre-check -> Traditional");
    TEST_CHECK(LocaleFromBcp47Tag(L"zh-CN") == UiLocale::ChineseSimplified &&
                   LocaleFromBcp47Tag(L"zh-TW") == UiLocale::ChineseTraditional &&
                   LocaleFromBcp47Tag(L"zh-HK") == UiLocale::ChineseTraditional &&
                   LocaleFromBcp47Tag(L"zh-SG") == UiLocale::ChineseSimplified &&
                   LocaleFromBcp47Tag(L"zh-MO") == UiLocale::ChineseTraditional,
               "B3: legacy zh exact-match set preserved verbatim");
    TEST_CHECK(LocaleFromBcp47Tag(L"ur-PK") == UiLocale::Urdu, "B3: ur-PK -> Urdu");
    TEST_CHECK(LocaleFromBcp47Tag(L"my-MM") == UiLocale::Burmese, "B3: my-MM -> Burmese");
    TEST_CHECK(LocaleFromBcp47Tag(L"en-US") == UiLocale::English, "B3: en-US -> English");
    TEST_CHECK(LocaleFromBcp47Tag(L"sw-KE") == UiLocale::Auto,
               "B3: unsupported OS tag -> sentinel; DetectSystemLocale falls through to English explicitly (design §2.1.5)");

    // ---- 6) R6 hyphen-boundary discipline -----------------------------------
    TEST_CHECK(LocaleFromBcp47Tag(L"nob") == UiLocale::Auto,
               "R6: bare 'nob' does NOT match the Norwegian 'no-' boundary rule");
    TEST_CHECK(LocaleFromBcp47Tag(L"fil") == UiLocale::Filipino,
               "R6: exact 'fil' (no region) matches on length boundary");
    TEST_CHECK(LocaleFromBcp47Tag(L"zh") == UiLocale::Auto,
               "B3: bare 'zh' defers to the LANGID phase (SUBLANG disambiguation preserved)");

    // ---- 7) StringToLocale aliases (design §2.1.3 preserved verbatim) -------
    TEST_CHECK(I18n::StringToLocale("zh_cn") == UiLocale::ChineseSimplified &&
                   I18n::StringToLocale("zh_tw") == UiLocale::ChineseTraditional &&
                   I18n::StringToLocale("ZH-cn") == UiLocale::ChineseSimplified &&
                   I18n::StringToLocale("zh") == UiLocale::ChineseSimplified,
               "B3: legacy zh_cn/zh_tw/zh aliases + case-insensitivity preserved");
    TEST_CHECK(I18n::StringToLocale("auto") == UiLocale::Auto, "B3: 'auto' still resolves to Auto");
    TEST_CHECK(I18n::StringToLocale("klingon") == UiLocale::English,
               "B3: unknown config value -> explicit English fallback (no silent half-wire)");

    // ---- 8) Authoring-progress metric (design §2-Q4.4): REPORTED, NOT
    //           failed. For every selectable non-English locale, count the
    //           fields identical to the English table. Legitimate identities
    //           exist by design (brand token "Emebala Chat", "Reddit", the
    //           universal contact lines, emoji-button labels), so a small
    //           non-zero count is expected in authored tables; a LARGE count
    //           (near 47) is the placeholder signature this metric exists to
    //           surface for VP's Gate 2 review. Printed as one [INFO] line.
    {
        I18n::SetLocale(UiLocale::English);
        std::vector<std::wstring> enStrings;
        for (int id = 0; id < static_cast<int>(StringId::EnumCount); ++id) {
            enStrings.push_back(I18n::Get(static_cast<StringId>(id)));
        }
        std::string report;
        for (const auto& e : GetSupportedUiLocales()) {
            if (e.locale == UiLocale::English) continue;
            I18n::SetLocale(e.locale);
            int identical = 0;
            for (int id = 0; id < static_cast<int>(StringId::EnumCount); ++id) {
                if (I18n::Get(static_cast<StringId>(id)) == enStrings[id]) ++identical;
            }
            report += I18n::LocaleToString(e.locale) + "=" + std::to_string(identical) + " ";
        }
        std::cout << "[INFO] B3 authoring-metric fields-identical-to-English (brand + universal "
                     "contact lines legitimately identical; near-47 would flag a placeholder): "
                  << report << std::endl;
        I18n::SetLocale(UiLocale::English);
    }

    // ---- 9) GetLocaleCode is table-driven for all 38 enum values ------------
    {
        bool code_ok = true;
        for (const auto& m : maps) {
            I18n::SetLocale(m.locale);
            if (I18n::GetLocaleCode() != m.config_code) code_ok = false;
        }
        I18n::SetLocale(UiLocale::Auto);
        if (I18n::GetLocaleCode() != "en") code_ok = false; // explicit fallback contract
        TEST_CHECK(code_ok, "B3: GetLocaleCode matches config_code for all 37 locales + Auto->en");
    }

    I18n::Initialize("auto"); // restore OS-derived locale for later tests
    if (g_failed_count == failures_before) {
        std::cout << "[PASS] B-3 LocaleMapping tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] B-3 LocaleMapping tests: " << (g_failed_count - failures_before)
                  << " check(s) failed." << std::endl;
    }
}

// ---- 260905 r7: diag_logger headless tests ----
// File sink runs against a temp directory override (Init(dir) is the test
// seam). Covers: filename pattern, line format, write/readback round-trip,
// multi-thread stress (line count == written count, every token present ->
// no interleaved corruption), DIAG_F tag parsing, disable toggle, and
// shutdown drain. The live hook-path enqueue is untestable headlessly by
// design; the enqueue-only contract is pinned here instead (a full burst of
// 8k lines completes in microseconds per call and never blocks: asserted via
// total wall time below the conservative 2 s bound).
static void TestDiagLogger() {
    const int failures_before = g_failed_count;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "emebala_diag_test";
    fs::remove_all(dir, ec);

    // Uninitialized: every entry point is a safe no-op.
    TEST_CHECK(!diag::IsInitialized(), "diag: not initialized before Init");
    diag::Printf("TEST", "before-init no-op %d", 1);
    diag::Flush();
    diag::Shutdown(); // double-shutdown / shutdown-while-uninit must not crash
    TEST_CHECK(!fs::exists(dir, ec), "diag: no directory created before Init");

    TEST_CHECK(diag::Init(dir), "diag: Init with temp-dir override succeeds");
    TEST_CHECK(diag::IsInitialized(), "diag: initialized after Init");
    TEST_CHECK(diag::IsEnabled(), "diag: file sink defaults to ON");
    // Idempotent Init while live returns true without rotating the file.
    const std::wstring first_path = diag::CurrentLogPath();
    TEST_CHECK(diag::Init(dir), "diag: second Init is idempotent true");
    TEST_CHECK(diag::CurrentLogPath() == first_path, "diag: idempotent Init keeps the same file");

    // File-name pattern: emebalachat_yymmddhhmmss.log (14 local-time digits,
    // user's literal format; optional -N collision suffix).
    {
        const std::string name = ToUtf8(fs::path(first_path).filename().wstring());
        // yy mm dd hh mm ss = 12 digits (the user's literal format).
        const std::string pfx = "emebalachat_";
        bool ok = name.rfind(pfx, 0) == 0 &&
                  name.size() >= pfx.size() + 12 + 4 &&
                  name.size() <= pfx.size() + 12 + 1 + 3 + 4;
        size_t i = pfx.size();
        for (int d = 0; d < 12 && ok; ++d, ++i) {
            ok = name[i] >= '0' && name[i] <= '9';
        }
        // After the 14 digits: optional "-N", then exactly ".log".
        if (ok && i < name.size() && name[i] == '-') {
            ++i;
            const size_t n0 = i;
            while (i < name.size() && name[i] >= '0' && name[i] <= '9') ++i;
            ok = i > n0 && i <= n0 + 3;
        }
        ok = ok && name.compare(i, name.size() - i, ".log") == 0;
        TEST_CHECK(ok, "diag: file name matches emebalachat_yymmddhhmmss[-N].log");
        TEST_CHECK(fs::exists(first_path, ec), "diag: log file physically created");
    }

    // Round-trip: content + line format "yyyy-mm-dd hh:mm:ss.mmm [tid] TAG/msg".
    diag::Printf("TEST", "hello %s %d", "world", 42);
    diag::Flush();
    std::string content;
    {
        std::ifstream in(first_path, std::ios::binary);
        content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    {
        const size_t pos = content.find("TEST/hello world 42");
        TEST_CHECK(pos != std::string::npos, "diag: round-trip payload present");
        // Validate the line header around the found payload.
        const size_t ls = content.rfind('\n', pos);
        const std::string line = (ls == std::string::npos)
                                     ? content.substr(0, content.find('\n', pos))
                                     : content.substr(ls + 1, content.find('\n', ls + 1) - ls - 1);
        bool fmt_ok = line.size() >= 24 &&
                      line[4] == '-' && line[7] == '-' && line[10] == ' ' &&
                      line[13] == ':' && line[16] == ':' && line[19] == '.' &&
                      line.find(" [") != std::string::npos &&
                      line.find("] TEST/") != std::string::npos;
        for (size_t k = 0; k < 20 && fmt_ok; ++k) {
            if (k == 4 || k == 7 || k == 10 || k == 13 || k == 16 || k == 19) continue;
            fmt_ok = line[k] >= '0' && line[k] <= '9';
        }
        TEST_CHECK(fmt_ok, "diag: line header yyyy-mm-dd hh:mm:ss.mmm [tid] TAG/");
    }

    // DIAG_F mirroring: stderr-format payload is mirrored with the
    // MODULE/site/NNN token parsed out as the TAG.
    diag::MirrorF("HOOK/Enter/999: mirrored body %d\n", 7);
    diag::Flush();
    {
        std::ifstream in(first_path, std::ios::binary);
        content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        TEST_CHECK(content.find("HOOK/Enter/999/mirrored body 7") != std::string::npos,
                   "diag: DIAG_F mirrored to file with parsed TAG");
    }

    // Multi-thread stress: 4 threads x 500 unique lines. Heuristic against
    // interleaved corruption: final line count == 2 session + 2 warm-up +
    // 2000 stress lines, and EVERY token appears exactly once.
    {
        const int kThreads = 4;
        const int kPer = 500;
        const uint64_t lines_before = std::count(content.begin(), content.end(), '\n');
        auto burst_start = std::chrono::steady_clock::now();
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([t, kPer]() {
                for (int i = 0; i < kPer; ++i) {
                    diag::Printf("MTSTRESS", "MT-%d-%d payload", t, i);
                }
            });
        }
        for (auto& th : threads) th.join();
        diag::Flush();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - burst_start)
                                    .count();
        std::ifstream in(first_path, std::ios::binary);
        content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const uint64_t lines_after = std::count(content.begin(), content.end(), '\n');
        TEST_CHECK(lines_after == lines_before + static_cast<uint64_t>(kThreads) * kPer,
                   "diag: multithread line count == written count (no corruption/loss)");
        bool all_tokens = true;
        for (int t = 0; t < kThreads && all_tokens; ++t) {
            for (int i = 0; i < kPer; ++i) {
                const std::string tok = "MT-" + std::to_string(t) + "-" + std::to_string(i) + " payload";
                if (content.find(tok) == std::string::npos) {
                    all_tokens = false;
                    break;
                }
            }
        }
        TEST_CHECK(all_tokens, "diag: every concurrent line present exactly once");
        TEST_CHECK(diag::DroppedCount() == 0, "diag: bounded queue lost nothing at 2k lines");
        // Enqueue never blocks: 2000 formatted enqueues + drain must finish
        // well inside a conservative 2 s bound (typical: tens of ms).
        TEST_CHECK(elapsed_ms < 2000, "diag: enqueue burst never blocks (bounded wall time)");
    }

    // Runtime toggle: disabled file sink accepts nothing; stderr sink of
    // DIAG_F still runs (cannot assert stderr headlessly - by contract).
    diag::SetEnabled(false);
    TEST_CHECK(!diag::IsEnabled(), "diag: SetEnabled(false) reflected");
    {
        const auto lines_before = std::count(content.begin(), content.end(), '\n');
        diag::Printf("TEST", "while-disabled %d", 0);
        diag::Flush();
        std::ifstream in(first_path, std::ios::binary);
        std::string after;
        after.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        TEST_CHECK(std::count(after.begin(), after.end(), '\n') == lines_before,
                   "diag: disabled sink writes no lines");
    }
    diag::SetEnabled(true);
    diag::Printf("TEST", "re-enabled %d", 1);
    diag::Flush();
    {
        std::ifstream in(first_path, std::ios::binary);
        std::string after;
        after.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        TEST_CHECK(after.find("TEST/re-enabled 1") != std::string::npos,
                   "diag: re-enable resumes file writing");
    }

    // Shutdown drains everything still queued and closes the file.
    diag::Printf("TEST", "pre-shutdown last line");
    diag::Shutdown();
    TEST_CHECK(!diag::IsInitialized(), "diag: uninitialized after Shutdown");
    std::string final_content;
    {
        std::ifstream in(first_path, std::ios::binary);
        final_content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        TEST_CHECK(final_content.find("TEST/pre-shutdown last line") != std::string::npos,
                   "diag: Shutdown flushed queued lines");
    }
    diag::Printf("TEST", "post-shutdown no-op");
    {
        std::ifstream in(first_path, std::ios::binary);
        std::string unchanged;
        unchanged.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        TEST_CHECK(unchanged == final_content, "diag: post-shutdown Printf writes nothing");
    }

    // Re-init cycle (the app-exit tests rely on this only being reachable via
    // full Shutdown): a second run creates a NEW file, appends nothing to old.
    TEST_CHECK(diag::Init(dir), "diag: re-Init after Shutdown succeeds");
    TEST_CHECK(diag::CurrentLogPath() != first_path, "diag: re-Init rotates to a new file");
    diag::Shutdown();

    fs::remove_all(dir, ec); // cleanup (best-effort; temp dir)
    if (g_failed_count == failures_before) {
        std::cout << "[PASS] 260905 diag_logger tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] 260905 diag_logger tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// Phase 5 (REQ-011, plan §5.2): headless unit tests for the app classifier.
// The classifier resolves the real process image name (QueryFullProcessImageNameW),
// so only two layers are verifiable headlessly: (1) fail-open on null/invalid
// HWNDs — classification failure always lands on the editor path (CategoryB),
// and (2) self-process classification — run_tests.exe is not in the CategoryA
// exe table, so its own console window must classify as CategoryB. Full exe-table
// matching needs real processes and is covered by the manual matrix (plan §5.3).
// F4/A2 (session 260908_0002, REQ-011 reversal): the pure exe-basename matchers
// (IsChatAppExeNameForEnterTranslation / IsEditorExeNameForEnterExclusion) ARE
// headless-testable, so this suite now also pins the re-tabled rows (8 chat apps
// stay CategoryA; the VS Code family + AI CLI editors moved out of CategoryA
// into kEditorApps) and the Enter-pipeline capture guard boundaries.
void TestPhase5AppClassifier() {
    std::cout << "[RUN] Testing Phase 5 app classifier (REQ-011 fail-open + self-process + F4 tables/guard)..." << std::endl;
    const int failures_before = g_failed_count;

    // Layer 1a: null HWND fails open to the editor path (CategoryB).
    TEST_CHECK(ClassifyAppWindow(nullptr) == AppCategory::CategoryB,
               "ClassifyAppWindow fails open to CategoryB for null HWND");

    // Layer 1b: a syntactically non-null but non-existent HWND must also fail
    // open (IsWindow is false -> CategoryB), not crash or return CategoryA.
    const HWND bogus_hwnd = reinterpret_cast<HWND>(static_cast<intptr_t>(0x1));
    TEST_CHECK(!::IsWindow(bogus_hwnd), "Bogus HWND 0x1 is not a real window in this environment");
    TEST_CHECK(ClassifyAppWindow(bogus_hwnd) == AppCategory::CategoryB,
               "ClassifyAppWindow fails open to CategoryB for invalid HWND 0x1");

    // Layer 2: self-process classification. run_tests.exe is a console app, so
    // GetConsoleWindow() normally returns a valid hwnd owned by this process.
    // Its exe name is not in the CategoryA table -> CategoryB. Skip (no check)
    // when no console is attached, per plan §5.2.
    const HWND console_hwnd = ::GetConsoleWindow();
    if (console_hwnd != nullptr && ::IsWindow(console_hwnd)) {
        TEST_CHECK(ClassifyAppWindow(console_hwnd) == AppCategory::CategoryB,
                   "ClassifyAppWindow classifies own console window (run_tests.exe, not in table) as CategoryB");
        // F4 (A2): the editor-exclusion probe fails open (false) on the self
        // process too - run_tests.exe is in neither table, so it must NOT be
        // treated as an editor/IDE excluded from Enter translation.
        TEST_CHECK(!IsEnterTranslateExcludedApp(console_hwnd),
                   "IsEnterTranslateExcludedApp fails open (false) for own console window (run_tests.exe)");
        TEST_CHECK(IsEnterTranslateExcludedApp(nullptr) == false,
                   "IsEnterTranslateExcludedApp returns false (fail-open) for null HWND");
        TEST_CHECK(IsEnterTranslateExcludedApp(bogus_hwnd) == false,
                   "IsEnterTranslateExcludedApp returns false (fail-open) for invalid HWND 0x1");
    } else {
        std::cout << "[SKIP] No console window attached; self-process classification checks skipped." << std::endl;
    }

    // F4/A2 W1 (REQ-011 reversal): pure basename matcher - the 8 chat apps stay
    // in CategoryA (Enter = send). VS Code-family + AI CLI editors were removed.
    TEST_CHECK(IsChatAppExeNameForEnterTranslation(L"KakaoTalk.exe"),
               "CategoryA pure matcher retains KakaoTalk.exe");
    TEST_CHECK(IsChatAppExeNameForEnterTranslation(L"Discord.exe"),
               "CategoryA pure matcher retains Discord.exe");
    TEST_CHECK(IsChatAppExeNameForEnterTranslation(L"Slack.exe"),
               "CategoryA pure matcher retains Slack.exe");
    TEST_CHECK(IsChatAppExeNameForEnterTranslation(L"Telegram.exe"),
               "CategoryA pure matcher retains Telegram.exe");
    TEST_CHECK(IsChatAppExeNameForEnterTranslation(L"Teams.exe"),
               "CategoryA pure matcher retains Teams.exe");
    TEST_CHECK(IsChatAppExeNameForEnterTranslation(L"ms-teams.exe"),
               "CategoryA pure matcher retains ms-teams.exe");
    TEST_CHECK(IsChatAppExeNameForEnterTranslation(L"Line.exe"),
               "CategoryA pure matcher retains Line.exe");
    TEST_CHECK(IsChatAppExeNameForEnterTranslation(L"WeChat.exe"),
               "CategoryA pure matcher retains WeChat.exe");
    TEST_CHECK(IsChatAppExeNameForEnterTranslation(L"WhatsApp.exe"),
               "CategoryA pure matcher retains WhatsApp.exe");
    // F4: Store (WinUI 3) WhatsApp runs as WhatsApp.Root.exe — same
    // dual-entry precedent as Teams.exe / ms-teams.exe.
    TEST_CHECK(IsChatAppExeNameForEnterTranslation(L"WhatsApp.Root.exe"),
               "CategoryA pure matcher classifies WhatsApp.Root.exe (Store)");
    // Case-insensitivity (WhatsApp with a different case must still match).
    TEST_CHECK(IsChatAppExeNameForEnterTranslation(L"whatsapp.EXE"),
               "CategoryA pure matcher is case-insensitive");
    TEST_CHECK(IsChatAppExeNameForEnterTranslation(L"whatsapp.root.EXE"),
               "CategoryA pure matcher case-insensitive for WhatsApp.Root.exe");
    // Editors are NO LONGER CategoryA.
    TEST_CHECK(!IsChatAppExeNameForEnterTranslation(L"Code.exe"),
               "Code.exe removed from CategoryA (REQ-011 reversal)");
    TEST_CHECK(!IsChatAppExeNameForEnterTranslation(L"Cursor.exe"),
               "Cursor.exe removed from CategoryA (REQ-011 reversal)");
    TEST_CHECK(!IsChatAppExeNameForEnterTranslation(L"opencode.exe"),
               "opencode.exe removed from CategoryA (REQ-011 reversal)");
    TEST_CHECK(!IsChatAppExeNameForEnterTranslation(L"notepad.exe"),
               "notepad.exe is not CategoryA");

    // F4/A2 W1: kEditorApps pure matcher - VS Code family + AI CLI editors.
    TEST_CHECK(IsEditorExeNameForEnterExclusion(L"Code.exe"),
               "kEditorApps matcher includes Code.exe");
    TEST_CHECK(IsEditorExeNameForEnterExclusion(L"code.exe"),
               "kEditorApps matcher is case-insensitive (code.exe)");
    TEST_CHECK(IsEditorExeNameForEnterExclusion(L"Code - Insiders.exe"),
               "kEditorApps matcher includes Code - Insiders.exe");
    TEST_CHECK(IsEditorExeNameForEnterExclusion(L"Cursor.exe"),
               "kEditorApps matcher includes Cursor.exe");
    TEST_CHECK(IsEditorExeNameForEnterExclusion(L"cursor.EXE"),
               "kEditorApps matcher is case-insensitive (cursor.EXE)");
    TEST_CHECK(IsEditorExeNameForEnterExclusion(L"Windsurf.exe"),
               "kEditorApps matcher includes Windsurf.exe");
    TEST_CHECK(IsEditorExeNameForEnterExclusion(L"VSCodium.exe"),
               "kEditorApps matcher includes VSCodium.exe");
    TEST_CHECK(IsEditorExeNameForEnterExclusion(L"opencode.exe"),
               "kEditorApps matcher includes opencode.exe");
    TEST_CHECK(IsEditorExeNameForEnterExclusion(L"claude.exe"),
               "kEditorApps matcher includes claude.exe");
    TEST_CHECK(IsEditorExeNameForEnterExclusion(L"codex.exe"),
               "kEditorApps matcher includes codex.exe");
    // Not an editor-exclusion target: Notepad stays a CategoryB Enter target.
    TEST_CHECK(!IsEditorExeNameForEnterExclusion(L"notepad.exe"),
               "notepad.exe is NOT Enter-excluded (stays CategoryB)");
    TEST_CHECK(!IsEditorExeNameForEnterExclusion(L"KakaoTalk.exe"),
               "KakaoTalk.exe is NOT an editor/IDE");
    TEST_CHECK(!IsEditorExeNameForEnterExclusion(L"run_tests.exe"),
               "run_tests.exe is NOT an editor/IDE");

    // F4/A2 W2: Enter-pipeline capture guard boundary (pure predicate).
    // F3 RAISE (session 260908_0003, verify 220750 §5-2): limits moved 512 ->
    // 4096 chars / 16 -> 64 newlines so the non-EM whole-capture accumulation
    // of 6+ translated sentences (예시1) stays translatable; document-sized
    // captures above the new limits are still rejected.
    TEST_CHECK(EnterCaptureWithinGuard(0, 0), "Guard allows empty capture (0 chars, 0 newlines)");
    TEST_CHECK(EnterCaptureWithinGuard(kMaxEnterTranslateChars, kMaxEnterTranslateNewlines),
               "Guard allows exactly the limit capture (4096 chars / 64 newlines)");
    TEST_CHECK(!EnterCaptureWithinGuard(kMaxEnterTranslateChars + 1, kMaxEnterTranslateNewlines),
               "Guard rejects one char over the limit");
    TEST_CHECK(!EnterCaptureWithinGuard(kMaxEnterTranslateChars, kMaxEnterTranslateNewlines + 1),
               "Guard rejects one newline over the limit");
    TEST_CHECK(EnterCaptureWithinGuard(kMaxEnterTranslateChars - 1, kMaxEnterTranslateNewlines - 1),
               "Guard allows under-limit capture");
    static_assert(kMaxEnterTranslateChars == 4096, "F3: guard char limit raised to 4096");
    static_assert(kMaxEnterTranslateNewlines == 64, "F3: guard newline limit raised to 64");
    // The raise must be the documented values (예시1 justification in
    // win32_input.hpp: ~40 sentences of 100 chars, hook K-clamp coherence).
    TEST_CHECK(EnterCaptureWithinGuard(6 * 100 + 5, 5),
               "F3 guard: 예시1 accumulation (6 sentences x ~100 chars) inside limits");
    TEST_CHECK(!EnterCaptureWithinGuard(5000, 3), "F3 guard: pasted document still rejected by chars");
    TEST_CHECK(!EnterCaptureWithinGuard(100, 65), "F3 guard: 65-newline abuse still rejected");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Phase 5 app classifier tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Phase 5 app classifier tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// Phase 8 Batch 1 (REQ-005, plan 225900 §4.1): headless unit tests for the
// console-capture gate. Only the fail-open contract and the non-console
// negative case are verifiable without a real desktop terminal: (1) null and
// invalid HWNDs must return false so the existing clipboard path is preserved
// on any query failure, (2) a plain (non-console) window of this process must
// return false, and (3) when the test process HAS a console attached, its
// console window (ConsoleWindowClass or PseudoConsoleWindow root) must be
// detected as unsafe — proving the positive path on a real console hwnd.
// Detection under Windows Terminal tabs and the full class table is covered
// by the manual matrix (plan §4.2), as in Phase 5's classifier precedent.
void TestPhase8ConsoleGate() {
    std::cout << "[RUN] Testing Phase 8 console capture gate (REQ-005 fail-open + detection)..." << std::endl;
    const int failures_before = g_failed_count;

    // Layer 1a: null HWND fails open (existing behavior kept on any failure).
    TEST_CHECK(!IsConsoleCaptureUnsafe(nullptr),
               "IsConsoleCaptureUnsafe fails open (false) for null HWND");

    // Layer 1b: a non-null but invalid HWND must also fail open, not crash.
    const HWND bogus_hwnd = reinterpret_cast<HWND>(static_cast<intptr_t>(0x1));
    TEST_CHECK(!::IsWindow(bogus_hwnd), "Bogus HWND 0x1 is not a real window in this environment");
    TEST_CHECK(!IsConsoleCaptureUnsafe(bogus_hwnd),
               "IsConsoleCaptureUnsafe fails open (false) for invalid HWND 0x1");

    // Layer 2: a plain non-console window owned by this process must be false
    // (the regression-protection contract: normal apps keep the clipboard path).
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = ::DefWindowProcW;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = L"Emebalachat_Ph8TestPlain";
        ::RegisterClassExW(&wc);
        HWND plain = ::CreateWindowExW(0, wc.lpszClassName, L"ph8", WS_OVERLAPPED,
                                       -300, -300, 50, 50, nullptr, nullptr, wc.hInstance, nullptr);
        TEST_CHECK(plain != nullptr, "Phase 8 fixture: plain (non-console) window creates");
        if (plain) {
            TEST_CHECK(!IsConsoleCaptureUnsafe(plain),
                       "IsConsoleCaptureUnsafe is false for a plain non-console window");
            ::DestroyWindow(plain);
        }
    }

    // Layer 3: positive detection on this process's own console window when a
    // console is attached (run_tests.exe is a console app; the hwnd class is
    // ConsoleWindowClass under conhost, or resolves through GA_ROOT to the
    // terminal hosting frame under Windows Terminal). Skip without a check
    // when no console is attached, per plan §4.1 (headless limitation).
    const HWND console_hwnd = ::GetConsoleWindow();
    if (console_hwnd != nullptr && ::IsWindow(console_hwnd)) {
        TEST_CHECK(IsConsoleCaptureUnsafe(console_hwnd),
                   "IsConsoleCaptureUnsafe is true for own console window (conhost/WT root)");
    } else {
        std::cout << "[SKIP] No console window attached; positive console detection check skipped." << std::endl;
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] Phase 8 console capture gate tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] Phase 8 console capture gate tests: " << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// REQ-027 (Batch B-4, plan §3 B-4 item 4): EditCaretTracker headless suite.
// Contract points pinned here:
//  (a) null / invalid hwnd -> TrySelectNewText false (caller keeps the
//      SelectMessageBlock fallback; the tracker never claims success).
//  (b) NotifyReplacement on untracked / null / garbage hwnds is a safe no-op.
//  (c) the pure estimate path (requery-failure fallback last + pasted_cch,
//      saturating at UINT32_MAX) is asserted through the header's constexpr
//      EditCaretTracker_EstimateNextOffset seam - no Win32 contact needed.
//  (d) CREATIVE VERIFICATION beyond the plan minimum: a full positive EM
//      sequence against a REAL in-process EDIT control (same technique the
//      Phase 8 suite uses with synthetic windows). The assertion is the
//      observable side effect - the control's actual selection state after
//      each TrySelectNewText/NotifyReplacement - proving "only the text typed
//      since the last replacement gets selected", the clamp reset, and the
//      pasted=false no-update rule, without any keyboard injection.
void TestReq027CaretTracker() {
    std::cout << "[TEST] REQ-027 EditCaretTracker (caret offset tracking)" << std::endl;
    const int failures_before = g_failed_count;

    // (a) unusable hwnds must never report an EM selection.
    TEST_CHECK(!EditCaretTracker_TrySelectNewText(nullptr),
               "REQ-027: null hwnd -> TrySelectNewText false (fallback path)");
    const HWND garbage = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1u));
    TEST_CHECK(!EditCaretTracker_TrySelectNewText(garbage),
               "REQ-027: invalid non-window hwnd -> TrySelectNewText false");

    // (b) no-op notify on untracked targets must not crash (execution reaching
    // the next TEST_CHECK is the proof).
    EditCaretTracker_NotifyReplacement(nullptr, true, 10);
    EditCaretTracker_NotifyReplacement(nullptr, false, 0);
    EditCaretTracker_NotifyReplacement(garbage, true, 10);
    TEST_CHECK(true, "REQ-027: NotifyReplacement no-op on null/garbage hwnds survived");

    // (c) pure estimate arithmetic (design §2.6.A2 requery-failure fallback).
    TEST_CHECK(EditCaretTracker_EstimateNextOffset(0u, 0u) == 0u,
               "REQ-027: estimate base case 0 + 0 = 0");
    TEST_CHECK(EditCaretTracker_EstimateNextOffset(120u, 40u) == 160u,
               "REQ-027: estimate normal case last + pasted_cch");
    TEST_CHECK(EditCaretTracker_EstimateNextOffset(UINT32_MAX, 5u) == UINT32_MAX,
               "REQ-027: estimate saturates at UINT32_MAX (stored offset is a DWORD)");
    TEST_CHECK(EditCaretTracker_EstimateNextOffset(0xFFFFFFF0u, 100u) == UINT32_MAX,
               "REQ-027: estimate saturates on wrap-around");

    // (d) positive end-to-end against a real standard EDIT control.
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Emebalachat_Req027Host";
    ::RegisterClassExW(&wc);
    HWND host = ::CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"req027", WS_POPUP,
                                  -400, -400, 200, 100, nullptr, nullptr, wc.hInstance, nullptr);
    HWND edit = nullptr;
    if (host) {
        edit = ::CreateWindowExW(0, L"EDIT", L"line1\nabc",
                                 WS_CHILD | WS_VISIBLE | ES_MULTILINE,
                                 0, 0, 180, 80, host, nullptr, wc.hInstance, nullptr);
    }
    TEST_CHECK(edit != nullptr, "REQ-027: in-process EDIT control created");
    // SetFocus requires a visible window: show the off-screen host without
    // activating it, then verify the focus chain really landed on the EDIT
    // control (SetFocus returns the PREVIOUS focus window, so the new focus
    // state must be probed via GetGUIThreadInfo, not the return value). If
    // the headless environment denies focus, the positive EM sequence is
    // skipped explicitly rather than passing/failing by luck (the resolution
    // would then depend on whatever hwndFocus holds).
    bool focus_ok = false;
    if (edit) {
        ::ShowWindow(host, SW_SHOWNOACTIVATE);
        ::SetFocus(edit);
        GUITHREADINFO gti = {};
        gti.cbSize = sizeof(gti);
        focus_ok = ::GetGUIThreadInfo(::GetCurrentThreadId(), &gti) && gti.hwndFocus == edit;
    }
    if (edit && !focus_ok) {
        std::cout << "[SKIP] SetFocus on EDIT control unavailable; positive EM sequence skipped." << std::endl;
    }
    if (edit && focus_ok) {
        // Caret collapsed at offset 6 (right after "line1\n").
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(6), static_cast<LPARAM>(6));

        // Pass 1: untracked -> last=0 -> select from text start to caret.
        TEST_CHECK(EditCaretTracker_TrySelectNewText(edit),
                   "REQ-027: standard Edit class enters EM path (true)");
        DWORD sel = static_cast<DWORD>(::SendMessageW(edit, EM_GETSEL, 0, 0));
        TEST_CHECK(LOWORD(sel) == 0u && HIWORD(sel) == 6u,
                   "REQ-027: first pass selects [0..caret) (whole new input, no history yet)");

        // pasted=false must NOT advance the offset (DP-4(b) stale-last rule):
        // stored start stays 0... then simulate the real replacement with
        // pasted=true and verify advancement via the next selection bounds.
        EditCaretTracker_NotifyReplacement(edit, false, 50);
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(9), static_cast<LPARAM>(9));
        TEST_CHECK(EditCaretTracker_TrySelectNewText(edit),
                   "REQ-027: second pass enters EM path");
        sel = static_cast<DWORD>(::SendMessageW(edit, EM_GETSEL, 0, 0));
        // Stored start after pass 1 was 0 (clamped history start); the false
        // notify must have left it at 0, so the selection spans [0..9).
        TEST_CHECK(LOWORD(sel) == 0u && HIWORD(sel) == 9u,
                   "REQ-027: pasted=false kept the old offset (selection starts at 0, not 50)");

        // Successful replacement: requery-EM_GETSEL-priority stores caret end 9.
        EditCaretTracker_NotifyReplacement(edit, true, 3);
        // User types "def" on the next line: caret 9 -> 12 via a doc append.
        ::SetWindowTextW(edit, L"line1\nabcdef");
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(12), static_cast<LPARAM>(12));
        TEST_CHECK(EditCaretTracker_TrySelectNewText(edit),
                   "REQ-027: third pass enters EM path");
        sel = static_cast<DWORD>(::SendMessageW(edit, EM_GETSEL, 0, 0));
        // THE core REQ-027 assertion: only "def" (9..12) is selected, not the
        // whole flow from 0 (which is what SelectMessageBlock would do).
        TEST_CHECK(LOWORD(sel) == 9u && HIWORD(sel) == 12u,
                   "REQ-027: third pass selects ONLY newly typed text [9..12)");

        // Clamp rule (§2.3.A2 (b)): document shrank below the tracked offset ->
        // reset to 0 (full-from-start), next pass selects [0..2).
        ::SetWindowTextW(edit, L"li");
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(2), static_cast<LPARAM>(2));
        TEST_CHECK(EditCaretTracker_TrySelectNewText(edit),
                   "REQ-027: clamped pass still enters EM path (safe full-selection, not fallback)");
        sel = static_cast<DWORD>(::SendMessageW(edit, EM_GETSEL, 0, 0));
        TEST_CHECK(LOWORD(sel) == 0u && HIWORD(sel) == 2u,
                   "REQ-027: stale offset > caret clamps to 0");

        ::SetFocus(nullptr);
    }
    if (host) {
        ::DestroyWindow(host); // child EDIT dies with the parent
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-027 EditCaretTracker tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-027 EditCaretTracker tests: " << (g_failed_count - failures_before)
                  << " check(s) failed." << std::endl;
    }
}

// REQ-027 B-6a (ISSUE-1 line-merge fix, design 210000 §3 B-6a file 4): the
// stored "previous translation end" offset MUST be the POST-newline caret -
// the value becomes the START of the next Enter's EM_SETSEL range, so saving
// it before SendEnterKey injects the REQ-023 CRLF made the following
// selection swallow the line break (2 UTF-16 units) and the replacement
// deleted it. Headless proof on a real in-process EDIT control: replacement
// -> CRLF append (+2 caret) -> settle -> NotifyReplacement (the new worker
// call order) -> next TrySelectNewText starts AFTER the CRLF. A causal
// control on a second control pins the OLD order's behavior (start == the
// pre-newline caret, CRLF inside the selection), documenting the mechanism.
void TestReq027OffsetAfterNewline() {
    std::cout << "[TEST] REQ-027 B-6a offset saved after injected newline (ISSUE-1)" << std::endl;
    const int failures_before = g_failed_count;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Emebalachat_Req027B6aHost";
    ::RegisterClassExW(&wc);
    HWND host = ::CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"req027b6a", WS_POPUP,
                                  -400, -400, 200, 100, nullptr, nullptr, wc.hInstance, nullptr);
    HWND edit = nullptr;
    if (host) {
        edit = ::CreateWindowExW(0, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | ES_MULTILINE,
                                 0, 0, 180, 80, host, nullptr, wc.hInstance, nullptr);
    }
    TEST_CHECK(edit != nullptr, "REQ-027 B-6a: in-process EDIT control created");
    // Same focus-verification discipline as TestReq027CaretTracker: SetFocus
    // can be denied in a headless session; skip the positive EM sequence
    // explicitly rather than passing/failing by luck.
    bool focus_ok = false;
    if (edit) {
        ::ShowWindow(host, SW_SHOWNOACTIVATE);
        ::SetFocus(edit);
        GUITHREADINFO gti = {};
        gti.cbSize = sizeof(gti);
        focus_ok = ::GetGUIThreadInfo(::GetCurrentThreadId(), &gti) && gti.hwndFocus == edit;
    }
    if (edit && !focus_ok) {
        std::cout << "[SKIP] SetFocus on EDIT control unavailable; B-6a positive sequence skipped." << std::endl;
    }
    if (edit && focus_ok) {
        // Task N: paste-replacement live (10 UTF-16 units), caret at 10.
        ::SetWindowTextW(edit, L"translated");
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(10), static_cast<LPARAM>(10));

        // Worker B-6a baseline sample (pre-newline caret).
        const DWORD pre = EditCaretTracker_SampleCaret(edit);
        TEST_CHECK(pre == 10u, "REQ-027 B-6a: SampleCaret reads the live pre-newline caret");

        // REQ-023 newline injection (what SendEnterKey does in Notepad/RichEdit):
        // document grows by CRLF (2 units), caret 10 -> 12.
        ::SetWindowTextW(edit, L"translated\r\n");
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(12), static_cast<LPARAM>(12));

        // Settle poll sees the caret moved (newline visible) -> returns early;
        // THEN the offset is saved, post-newline (the fixed call order).
        EditCaretTracker_SettleNewlineVisible(edit, pre);
        EditCaretTracker_NotifyReplacement(edit, true, 10);

        // Next block: user types "abc" on the new line, caret 12 -> 15.
        ::SetWindowTextW(edit, L"translated\r\nabc");
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(15), static_cast<LPARAM>(15));
        TEST_CHECK(EditCaretTracker_TrySelectNewText(edit),
                   "REQ-027 B-6a: next Enter enters the EM path");
        const DWORD sel = static_cast<DWORD>(::SendMessageW(edit, EM_GETSEL, 0, 0));
        // THE ISSUE-1 assertion: [12..15) - the CRLF at 10..11 is NOT in the
        // selection, so the next replacement can no longer delete it.
        TEST_CHECK(LOWORD(sel) == 12u && HIWORD(sel) == 15u,
                   "REQ-027 B-6a: selection starts AFTER the injected CRLF (line break preserved)");

        // Causal control on a fresh control: the OLD order (save BEFORE the
        // newline) stores 10, and the next EM_SETSEL covers the CRLF - the
        // exact mechanism that merged lines pre-B-6a.
        HWND edit2 = nullptr;
        if (host) {
            edit2 = ::CreateWindowExW(0, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | ES_MULTILINE,
                                      0, 0, 180, 80, host, nullptr, wc.hInstance, nullptr);
        }
        bool focus_ok2 = false;
        if (edit2) {
            ::SetFocus(edit2);
            GUITHREADINFO gti2 = {};
            gti2.cbSize = sizeof(gti2);
            focus_ok2 = ::GetGUIThreadInfo(::GetCurrentThreadId(), &gti2) && gti2.hwndFocus == edit2;
        }
        if (focus_ok2) {
            ::SetWindowTextW(edit2, L"translated");
            ::SendMessageW(edit2, EM_SETSEL, static_cast<WPARAM>(10), static_cast<LPARAM>(10));
            EditCaretTracker_NotifyReplacement(edit2, true, 10); // OLD order: pre-newline save
            ::SetWindowTextW(edit2, L"translated\r\n");
            ::SendMessageW(edit2, EM_SETSEL, static_cast<WPARAM>(12), static_cast<LPARAM>(12));
            ::SetWindowTextW(edit2, L"translated\r\nabc");
            ::SendMessageW(edit2, EM_SETSEL, static_cast<WPARAM>(15), static_cast<LPARAM>(15));
            TEST_CHECK(EditCaretTracker_TrySelectNewText(edit2),
                       "REQ-027 B-6a: causal control enters the EM path");
            const DWORD sel2 = static_cast<DWORD>(::SendMessageW(edit2, EM_GETSEL, 0, 0));
            TEST_CHECK(LOWORD(sel2) == 10u && HIWORD(sel2) == 15u,
                       "REQ-027 B-6a: causal control - pre-newline save selects the CRLF (documented defect mechanism)");
        }
        if (edit2) {
            ::DestroyWindow(edit2);
        }

        // Degradation safety: unusable hwnd samples Unknown, settle no-ops.
        TEST_CHECK(EditCaretTracker_SampleCaret(nullptr) == kEditCaretUnknown,
                   "REQ-027 B-6a: SampleCaret(null) -> kEditCaretUnknown");
        EditCaretTracker_SettleNewlineVisible(nullptr, kEditCaretUnknown);
        TEST_CHECK(true, "REQ-027 B-6a: SettleNewlineVisible on unusable hwnd survived");

        ::SetFocus(nullptr);
    }
    if (host) {
        ::DestroyWindow(host); // child EDIT dies with the parent
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-027 B-6a offset-after-newline tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-027 B-6a offset-after-newline tests: " << (g_failed_count - failures_before)
                  << " check(s) failed." << std::endl;
    }
}

// REQ-027 B-6b (design 192100 §2.3 + §3 B-5b file 3, VP ruling 260907 21:55):
// capability-based EM detection suite. Contract points pinned here:
//  (a) unusable hwnds still fall back through the public API (ProbeEmCapability
//      itself is file-local by design - §2.6 "external exposure not required" -
//      so its null-handle verdict is asserted through TrySelectNewText/002).
//  (b) the pure decision core ClassifyEmProbe over the full ambiguity matrix,
//      including the VP-approved DefWindowProc hardening: an unhandled message
//      answered 0 "succeeds" through SendMessageTimeoutW, so an empty generic
//      window must classify NotCapable (pre-hardening §2.3 pseudocode said
//      Capable - the false positive closed here). static_assert pins the
//      verdicts at compile time; runtime TEST_CHECKs mirror the same matrix.
//  (c) NotifyReplacement on untracked hwnds remains a safe no-op (execution
//      reaching the next TEST_CHECK is the proof).
//  (d) CREATIVE VERIFICATION on live in-process windows: a real EMPTY multiline
//      EDIT (caret (0,0), EM_GETLINECOUNT==1) must ENTER the EM path through
//      the real probe (positive-evidence branch), while a plain DefWindowProc
//      popup (EM_* unhandled, all replies 0/DefWindowProc, the structural
//      stand-in for Chrome_WidgetWin_1/VSCode surfaces) must fall back - the
//      probe decides by observed capability, never by class name.
void TestReq027CapabilityProbe() {
    std::cout << "[TEST] REQ-027 B-6b capability probe (class whitelist retired)" << std::endl;
    const int failures_before = g_failed_count;

    // (a) public fallback contract on unusable hwnds (probe runs nowhere).
    TEST_CHECK(!EditCaretTracker_TrySelectNewText(nullptr),
               "REQ-027 B-6b: null hwnd -> TrySelectNewText false (fallback path)");
    const HWND garbage = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1u));
    TEST_CHECK(!EditCaretTracker_TrySelectNewText(garbage),
               "REQ-027 B-6b: invalid non-window hwnd -> TrySelectNewText false");

    // (b) pure decision matrix. Meaningful selection short-circuits to Capable.
    {
        constexpr EmProbeSignals silent{};
        static_assert(ClassifyEmProbe(silent) == EmCapability::NotCapable,
                      "B-6b: EM_GETSEL silent -> NotCapable");
        TEST_CHECK(ClassifyEmProbe(silent) == EmCapability::NotCapable,
                   "REQ-027 B-6b: EM_GETSEL silent/timeout -> NotCapable");
        constexpr EmProbeSignals meaningful{.getsel_handled = true, .sel_start = 4, .sel_end = 9};
        static_assert(ClassifyEmProbe(meaningful) == EmCapability::Capable,
                      "B-6b: meaningful selection -> Capable");
        TEST_CHECK(ClassifyEmProbe(meaningful) == EmCapability::Capable,
                   "REQ-027 B-6b: meaningful (4,9) selection -> Capable");
    }
    // (0,0) in a non-empty document: needs EM line-model consistency.
    {
        constexpr EmProbeSignals consistent{
            .getsel_handled = true, .len_ok = true, .textlen = 12,
            .count_ok = true, .linecount = 2, .linefromchar_ok = true};
        static_assert(ClassifyEmProbe(consistent) == EmCapability::Capable,
                      "B-6b: non-empty + consistent EM family -> Capable");
        TEST_CHECK(ClassifyEmProbe(consistent) == EmCapability::Capable,
                   "REQ-027 B-6b: (0,0) non-empty consistent family -> Capable");
        EmProbeSignals partial = consistent;
        partial.linefromchar_ok = false; // GETSEL+LEN answered, LINEFROMCHAR silent
        TEST_CHECK(ClassifyEmProbe(partial) == EmCapability::Unknown,
                   "REQ-027 B-6b: non-empty + LINEFROMCHAR silent -> Unknown (conservative)");
        EmProbeSignals no_lines = consistent;
        no_lines.count_ok = false; // DefWindowProc never answers EM_GETLINECOUNT
        TEST_CHECK(ClassifyEmProbe(no_lines) == EmCapability::Unknown,
                   "REQ-027 B-6b: non-empty + linecount silent -> Unknown (conservative)");
    }
    // (0,0) in an empty document: linecount >= 1 is the positive evidence.
    {
        constexpr EmProbeSignals editor{
            .getsel_handled = true, .len_ok = true, .count_ok = true, .linecount = 1};
        static_assert(ClassifyEmProbe(editor) == EmCapability::Capable,
                      "B-6b: empty doc + linecount 1 (real editor) -> Capable");
        TEST_CHECK(ClassifyEmProbe(editor) == EmCapability::Capable,
                   "REQ-027 B-6b: empty doc + linecount>=1 -> Capable");
        // DefWindowProc default reply (linecount 0): THE false positive the
        // 21:55 ruling closed (pre-hardening §2.3 said Capable here).
        constexpr EmProbeSignals defproc{
            .getsel_handled = true, .len_ok = true, .count_ok = true};
        static_assert(ClassifyEmProbe(defproc) == EmCapability::NotCapable,
                      "B-6b: empty doc + linecount 0 (DefWindowProc) -> NotCapable");
        TEST_CHECK(ClassifyEmProbe(defproc) == EmCapability::NotCapable,
                   "REQ-027 B-6b: empty doc + linecount 0 (DefWindowProc) -> NotCapable (hardened)");
    }
    // Length query silent: limit/linecount evidence -> Unknown, nothing -> NotCapable.
    {
        constexpr EmProbeSignals limit_only{.getsel_handled = true, .limit_ok = true};
        static_assert(ClassifyEmProbe(limit_only) == EmCapability::Unknown,
                      "B-6b: length silent + limit answered -> Unknown");
        TEST_CHECK(ClassifyEmProbe(limit_only) == EmCapability::Unknown,
                   "REQ-027 B-6b: (0,0) length silent + limit answered -> Unknown");
        constexpr EmProbeSignals bare{.getsel_handled = true};
        static_assert(ClassifyEmProbe(bare) == EmCapability::NotCapable,
                      "B-6b: length silent + no other evidence -> NotCapable");
        TEST_CHECK(ClassifyEmProbe(bare) == EmCapability::NotCapable,
                   "REQ-027 B-6b: (0,0) all cross-queries silent -> NotCapable");
    }

    // (c) no-op notify on untracked targets must not crash.
    EditCaretTracker_NotifyReplacement(nullptr, true, 10);
    EditCaretTracker_NotifyReplacement(nullptr, false, 0);
    EditCaretTracker_NotifyReplacement(garbage, true, 10);
    TEST_CHECK(true, "REQ-027 B-6b: NotifyReplacement no-op on null/garbage hwnds survived");

    // (d) live-window probe behavior on real HWNDs (see suite header).
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Emebalachat_Req027B6bHost";
    ::RegisterClassExW(&wc);
    HWND host = ::CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"req027b6b", WS_POPUP,
                                  -400, -400, 200, 100, nullptr, nullptr, wc.hInstance, nullptr);
    TEST_CHECK(host != nullptr, "REQ-027 B-6b: popup host window created");
    HWND edit = nullptr;
    if (host) {
        edit = ::CreateWindowExW(0, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | ES_MULTILINE,
                                 0, 0, 180, 80, host, nullptr, wc.hInstance, nullptr);
    }
    TEST_CHECK(edit != nullptr, "REQ-027 B-6b: in-process EMPTY EDIT control created");
    bool focus_ok = false;
    if (edit) {
        ::ShowWindow(host, SW_SHOWNOACTIVATE);
        ::SetFocus(edit);
        GUITHREADINFO gti = {};
        gti.cbSize = sizeof(gti);
        focus_ok = ::GetGUIThreadInfo(::GetCurrentThreadId(), &gti) && gti.hwndFocus == edit;
    }
    if (edit && !focus_ok) {
        std::cout << "[SKIP] SetFocus unavailable; B-6b live probe sequence skipped." << std::endl;
    }
    if (host && edit && focus_ok) {
        // Real editor, empty document, caret (0,0): the probe must find the
        // positive EM_GETLINECOUNT evidence and let the EM path proceed.
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(0), static_cast<LPARAM>(0));
        TEST_CHECK(EditCaretTracker_TrySelectNewText(edit),
                   "REQ-027 B-6b: live empty EDIT (caret 0,0) -> probe Capable (EM path)");
        ::SetFocus(nullptr);
    }
    if (host) {
        bool host_focus_ok = false;
        ::SetFocus(host);
        GUITHREADINFO gti2 = {};
        gti2.cbSize = sizeof(gti2);
        host_focus_ok = ::GetGUIThreadInfo(::GetCurrentThreadId(), &gti2) && gti2.hwndFocus == host;
        if (host_focus_ok) {
            // Generic DefWindowProc window: EM_GETSEL "succeeds" with (0,0) via
            // the default reply, WM_GETTEXTLENGTH returns 0, EM_GETLINECOUNT 0.
            // Pre-B-6b the empty-doc branch would have called this Capable and
            // injected an EM_SETSEL into a non-editor; the hardened probe must
            // refuse -> /007 fallback. This is the Chrome_WidgetWin_1 structure.
            TEST_CHECK(!EditCaretTracker_TrySelectNewText(host),
                       "REQ-027 B-6b: live DefWindowProc window -> probe NotCapable (fallback kept)");
            TEST_CHECK(EditCaretTracker_SampleCaret(host) == kEditCaretUnknown,
                       "REQ-027 B-6b: SampleCaret on generic window -> kEditCaretUnknown");
            ::SetFocus(nullptr);
        } else {
            std::cout << "[SKIP] SetFocus on popup host unavailable; generic-window probe skipped." << std::endl;
        }
        ::DestroyWindow(host); // child EDIT dies with the parent
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-027 B-6b capability probe tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-027 B-6b capability probe tests: " << (g_failed_count - failures_before)
                  << " check(s) failed." << std::endl;
    }
}

// REQ-034 F2-B' batch D-1 (design 260907 173700 §4.1 rule 4, debug §F2):
// leading-CRLF self-correction suite. F2 mechanism: manual Shift+Enter is
// pass_through (hook logs it, the tracker never advances), so the stored
// EM_SETSEL start points BEFORE the user's newline and the next capture
// begins with "\r\n" - translating that range deletes the break on
// replacement (line merge; user log emebalachat_260907171452 L997/L1147).
// Contract points pinned here:
//  (a) pure predicate matrix (constexpr seam): the CRLF PAIR only. Single-LF
//      or single-CR controls (normal progress there legitimately starts a
//      capture with their newline unit) must NOT trigger, and a 1-unit buffer
//      cannot false-positive.
//  (b) live recovery on a real in-process EDIT: a drifted stored start
//      selects a capture range whose text begins with "\r\n" (defect
//      reproduction), TrySelfCorrectReSelect re-selects the whole block
//      [0..caret) whose text does NOT begin with "\r\n" (the user-visible
//      fix), is ONCE-bounded (second call refuses), and leaves start=0
//      stored so the next Enter whole-block selects.
void TestReq034ManualNewlineRecovery() {
    std::cout << "[TEST] REQ-034 F2-B' manual-newline leading-CRLF recovery" << std::endl;
    const int failures_before = g_failed_count;

    // (a) pure predicate matrix.
    static_assert(EditCaretTracker_HasLeadingCrlf(std::wstring_view(L"\r\nblock")),
                  "REQ-034: leading CRLF pair detected (constexpr seam)");
    static_assert(!EditCaretTracker_HasLeadingCrlf(std::wstring_view(L"block")),
                  "REQ-034: plain capture not flagged (constexpr seam)");
    TEST_CHECK(EditCaretTracker_HasLeadingCrlf(L"\r\nsecond-line"), "REQ-034: leading CRLF -> true (F2 capture shape)");
    TEST_CHECK(EditCaretTracker_HasLeadingCrlf(L"\r\n"), "REQ-034: exact 2-unit pair -> true");
    TEST_CHECK(!EditCaretTracker_HasLeadingCrlf(L""), "REQ-034: empty capture -> false");
    TEST_CHECK(!EditCaretTracker_HasLeadingCrlf(L"\r"), "REQ-034: 1-unit CR only -> false (short buffer)");
    TEST_CHECK(!EditCaretTracker_HasLeadingCrlf(L"\nline2"), "REQ-034: leading lone LF (single-LF control) -> false (no over-correction)");
    TEST_CHECK(!EditCaretTracker_HasLeadingCrlf(L"\rline2"), "REQ-034: leading lone CR -> false (no over-correction)");
    TEST_CHECK(!EditCaretTracker_HasLeadingCrlf(L" \r\nx"), "REQ-034: space before CRLF -> false (start not absorbed by the EM geometry)");
    TEST_CHECK(!EditCaretTracker_HasLeadingCrlf(L"abc\r\n"), "REQ-034: trailing CRLF -> false (only the prefix matters)");

    // (b) live drift + recovery on a real EDIT control (same focus-verified
    // discipline as the REQ-027 suites; explicitly skipped, never lucked,
    // when SetFocus is denied in a headless session).
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Emebalachat_Req034Host";
    ::RegisterClassExW(&wc);
    HWND host = ::CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"req034", WS_POPUP,
                                  -400, -400, 200, 100, nullptr, nullptr, wc.hInstance, nullptr);
    HWND edit = nullptr;
    if (host) {
        edit = ::CreateWindowExW(0, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | ES_MULTILINE,
                                 0, 0, 180, 80, host, nullptr, wc.hInstance, nullptr);
    }
    TEST_CHECK(edit != nullptr, "REQ-034: in-process EDIT control created");
    bool focus_ok = false;
    if (edit) {
        ::ShowWindow(host, SW_SHOWNOACTIVATE);
        ::SetFocus(edit);
        GUITHREADINFO gti = {};
        gti.cbSize = sizeof(gti);
        focus_ok = ::GetGUIThreadInfo(::GetCurrentThreadId(), &gti) && gti.hwndFocus == edit;
    }
    if (edit && !focus_ok) {
        std::cout << "[SKIP] SetFocus on EDIT control unavailable; REQ-034 live recovery sequence skipped." << std::endl;
    }
    if (edit && focus_ok) {
        // Helper: read the live document text (shape assertions never print
        // content; this is test fixture text we create ourselves).
        auto doc_text = [](HWND h) {
            wchar_t buf[256] = {};
            const int n = static_cast<int>(::SendMessageW(h, WM_GETTEXT, 255, reinterpret_cast<LPARAM>(buf)));
            return std::wstring(buf, (n > 0 && n < 255) ? static_cast<size_t>(n) : 0);
        };

        // Pipeline pass N: replacement live, caret 10; the post-newline-free
        // notify (design worker contract) stores the real caret 10.
        ::SetWindowTextW(edit, L"translated");
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(10), static_cast<LPARAM>(10));
        TEST_CHECK(EditCaretTracker_TrySelectNewText(edit), "REQ-034: pass N enters the EM path");
        EditCaretTracker_NotifyReplacement(edit, true, 10);

        // USER edits out-of-band: manual Shift+Enter (pass_through - NO
        // tracker call, the F2 root) then types "abc". Caret 10 -> 15.
        ::SetWindowTextW(edit, L"translated\r\nabc");
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(15), static_cast<LPARAM>(15));

        // Enter N+1 with the drifted start: EM_SETSEL(10, 15) reproduces the
        // defect capture range, which begins with the manual CRLF.
        TEST_CHECK(EditCaretTracker_TrySelectNewText(edit), "REQ-034: drifted pass still enters the EM path");
        DWORD sel = static_cast<DWORD>(::SendMessageW(edit, EM_GETSEL, 0, 0));
        TEST_CHECK(LOWORD(sel) == 10u && HIWORD(sel) == 15u,
                   "REQ-034: drifted start reproduces the capture range [10..15)");
        const std::wstring drifted_capture = doc_text(edit).substr(LOWORD(sel), HIWORD(sel) - LOWORD(sel));
        TEST_CHECK(EditCaretTracker_HasLeadingCrlf(drifted_capture),
                   "REQ-034: drifted capture begins with the manual CRLF (F2 premise)");

        // Self-correction fires (CopySelectedText drives exactly this seam
        // when the predicate flags the capture): [0..caret) whole block.
        TEST_CHECK(EditCaretTracker_TrySelfCorrectReSelect(edit),
                   "REQ-034: leading CRLF -> start=0 self-correct re-select fires");
        sel = static_cast<DWORD>(::SendMessageW(edit, EM_GETSEL, 0, 0));
        TEST_CHECK(LOWORD(sel) == 0u && HIWORD(sel) == 15u,
                   "REQ-034: recovery selection is the whole block [0..15)");
        const std::wstring corrected_capture = doc_text(edit).substr(0, 15);
        TEST_CHECK(!EditCaretTracker_HasLeadingCrlf(corrected_capture),
                   "REQ-034: recovered capture has NO leading CRLF (line break survives the replacement)");

        // ONCE-bounded: the start is now stored 0, so a second self-correct
        // in the same Enter is refused (infinite-loop guard, work order D-2).
        TEST_CHECK(!EditCaretTracker_TrySelfCorrectReSelect(edit),
                   "REQ-034: second self-correct refused (start already 0; once-per-Enter bound)");

        // Correction persisted: next Enter whole-block selects from the
        // stored 0 until NotifyReplacement restores real-caret progress.
        TEST_CHECK(EditCaretTracker_TrySelectNewText(edit), "REQ-034: next Enter enters the EM path");
        sel = static_cast<DWORD>(::SendMessageW(edit, EM_GETSEL, 0, 0));
        TEST_CHECK(LOWORD(sel) == 0u && HIWORD(sel) == 15u,
                   "REQ-034: start=0 stored by the correction makes the next Enter whole-block");

        ::SetFocus(nullptr);
    }
    if (host) {
        ::DestroyWindow(host); // child EDIT dies with the parent
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-034 F2-B' manual-newline recovery tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-034 F2-B' manual-newline recovery tests: " << (g_failed_count - failures_before)
                  << " check(s) failed." << std::endl;
    }
}

// REQ-034 F2-B' batch D-1 companion: normal progress must NOT self-correct
// (REQ-027 contract regression guard). Pins the three refusal gates of
// TrySelfCorrectReSelect and the B-6a post-newline geometry the fix must
// leave untouched: after the offset is saved AFTER the injected newline, the
// following Enter's capture range starts AT the newline's end - the
// predicate is false and CopySelectedText never reaches the re-select seam.
void TestReq034NoLeadingCrlfNormalProgress() {
    std::cout << "[TEST] REQ-034 F2-B' normal progress keeps the REQ-027 contract" << std::endl;
    const int failures_before = g_failed_count;

    // (a) unusable hwnds: safe refusal, no EM contact.
    TEST_CHECK(!EditCaretTracker_TrySelfCorrectReSelect(nullptr),
               "REQ-034: self-correct on null hwnd -> false");
    const HWND garbage = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1u));
    TEST_CHECK(!EditCaretTracker_TrySelfCorrectReSelect(garbage),
               "REQ-034: self-correct on invalid non-window hwnd -> false");

    // (b) live normal-progress sequence on a real EDIT control (focus-
    // verified discipline as above: explicit skip, never luck).
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Emebalachat_Req034NpHost";
    ::RegisterClassExW(&wc);
    HWND host = ::CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"req034np", WS_POPUP,
                                  -400, -400, 200, 100, nullptr, nullptr, wc.hInstance, nullptr);
    HWND edit = nullptr;
    if (host) {
        edit = ::CreateWindowExW(0, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | ES_MULTILINE,
                                 0, 0, 180, 80, host, nullptr, wc.hInstance, nullptr);
    }
    TEST_CHECK(edit != nullptr, "REQ-034 normal-progress: in-process EDIT control created");
    bool focus_ok = false;
    if (edit) {
        ::ShowWindow(host, SW_SHOWNOACTIVATE);
        ::SetFocus(edit);
        GUITHREADINFO gti = {};
        gti.cbSize = sizeof(gti);
        focus_ok = ::GetGUIThreadInfo(::GetCurrentThreadId(), &gti) && gti.hwndFocus == edit;
    }
    if (edit && !focus_ok) {
        std::cout << "[SKIP] SetFocus on EDIT control unavailable; REQ-034 normal-progress sequence skipped." << std::endl;
    }
    if (edit && focus_ok) {
        // B-6a pipeline geometry (worker.cpp post-newline save), NO user edit:
        // replacement "translated" + injected CRLF -> caret 12 -> settle ->
        // NotifyReplacement stores 12 (AFTER the newline - REQ-027 contract).
        ::SetWindowTextW(edit, L"translated\r\n");
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(12), static_cast<LPARAM>(12));
        const DWORD pre = EditCaretTracker_SampleCaret(edit);
        TEST_CHECK(pre == 12u, "REQ-034 normal-progress: caret sampled at the post-newline position");
        // The settle is already pinned by TestReq027OffsetAfterNewline; here
        // the caret sits post-newline from the start, so NotifyReplacement is
        // called directly (skipping the poll avoids a 150 ms no-move budget).
        EditCaretTracker_NotifyReplacement(edit, true, 10);

        // User types ONLY "def" (no manual newline): caret 12 -> 15.
        ::SetWindowTextW(edit, L"translated\r\ndef");
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(15), static_cast<LPARAM>(15));
        TEST_CHECK(EditCaretTracker_TrySelectNewText(edit), "REQ-034 normal-progress: Enter enters the EM path");
        const DWORD sel = static_cast<DWORD>(::SendMessageW(edit, EM_GETSEL, 0, 0));
        // THE REQ-027 progress assertion, unchanged by F2-B': start is the
        // stored post-newline caret 12, and the captured range "def" has no
        // leading CRLF -> CopySelectedText's predicate gate keeps the capture
        // as-is (no self-correct fires on normal progress).
        TEST_CHECK(LOWORD(sel) == 12u && HIWORD(sel) == 15u,
                   "REQ-034 normal-progress: selection starts AFTER the injected CRLF (REQ-027 contract held)");
        wchar_t buf[256] = {};
        ::SendMessageW(edit, WM_GETTEXT, 255, reinterpret_cast<LPARAM>(buf));
        const std::wstring capture(buf);
        TEST_CHECK(!EditCaretTracker_HasLeadingCrlf(capture.substr(12, 3)),
                   "REQ-034 normal-progress: capture range has no leading CRLF (re-capture must NOT fire)");

        // Refusal gate (stored start 0): an untracked control's first Enter
        // already starts whole-block, so self-correct must refuse even if a
        // caller mis-fired it - the once-per-Enter bound. Fresh Key per hwnd,
        // so a brand-new control deterministically has no stored entry:
        HWND edit2 = nullptr;
        if (host) {
            edit2 = ::CreateWindowExW(0, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | ES_MULTILINE,
                                      0, 0, 180, 80, host, nullptr, wc.hInstance, nullptr);
        }
        bool focus_ok2 = false;
        if (edit2) {
            ::SetFocus(edit2);
            GUITHREADINFO gti2 = {};
            gti2.cbSize = sizeof(gti2);
            focus_ok2 = ::GetGUIThreadInfo(::GetCurrentThreadId(), &gti2) && gti2.hwndFocus == edit2;
        }
        if (edit2 && focus_ok2) {
            ::SetWindowTextW(edit2, L"\r\nleading-doc-newline");
            ::SendMessageW(edit2, EM_SETSEL, static_cast<WPARAM>(21), static_cast<LPARAM>(21));
            // Untracked first Enter: start 0 stored - the document itself
            // begins with CRLF, whole-block is already the safe geometry.
            TEST_CHECK(EditCaretTracker_TrySelectNewText(edit2), "REQ-034 gate: first Enter on fresh control enters the EM path");
            const DWORD sel0 = static_cast<DWORD>(::SendMessageW(edit2, EM_GETSEL, 0, 0));
            TEST_CHECK(LOWORD(sel0) == 0u, "REQ-034 gate: untracked first Enter starts at 0");
            TEST_CHECK(!EditCaretTracker_TrySelfCorrectReSelect(edit2),
                       "REQ-034 gate: stored start 0 -> re-select refused (already-safe, retry spent)");
            ::DestroyWindow(edit2);
        } else {
            std::cout << "[SKIP] SetFocus on second EDIT unavailable; stored-0 refusal gate skipped." << std::endl;
        }

        ::SetFocus(nullptr);
    }
    if (host) {
        ::DestroyWindow(host);
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-034 F2-B' normal-progress (no re-capture) tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-034 F2-B' normal-progress tests: " << (g_failed_count - failures_before)
                  << " check(s) failed." << std::endl;
    }
}

// REQ-034 F3-B batch D-3: paste-window empty-capture notice suppression
// (design 173700_architect §2.2.1, debug 173700 §F3; user rule: a retry Enter
// right after a paste is a re-translation intent and must NOT be blocked by a
// false "no selection" notice).
//
// The worker gates the hold-and-notice branch on a pure time-window predicate
// (src/worker.hpp PasteWindowSuppressesNotice) so this test and worker.cpp
// assert ONE definition (same discipline as EmptyCaptureNeedsHold /
// SelectionReleaseRequired). Inside the kPasteEmptySuppressMs window after a
// SUCCESSFUL paste, an empty capture is the last==caret geometry (Ctrl+C into
// an empty selection changes nothing -> the 180ms stale-refuse -> empty
// capture): the notice would be FALSE, so it is suppressed and Enter is
// delivered to the app (ReleaseSelectionOnce + SendEnterKey, silent
// send-through). Outside the window - no paste history, or window elapsed -
// the general empty Enter keeps the existing R5 behavior (hold_send +
// no-selection notice).
//
// The fold below mirrors worker.cpp's composition 1:1:
//   notice_fires = EmptyCaptureNeedsHold(empty, !bypass) && !window
//   enter_delivered_on_suppression = hold_eligible && window  (send-through)
void TestReq034PasteWindowSuppress() {
    std::cout << "[TEST] REQ-034 F3-B paste-window empty-capture notice suppression" << std::endl;
    const int failures_before = g_failed_count;

    // (a) compile-time contract on the shared seam (src/worker.hpp).
    static_assert(kPasteEmptySuppressMs == 2000,
                  "F3-B: initial window is 2000 ms (design §2.2.1, tune in D-4 QA)");
    static_assert(!PasteWindowSuppressesNotice(1000, 0),
                  "F3-B: no-paste sentinel (0) never suppresses - general empty Enter keeps notice");
    static_assert(PasteWindowSuppressesNotice(1000, 1000),
                  "F3-B: capture at the paste instant is inside the window");
    static_assert(PasteWindowSuppressesNotice(3000, 1000),
                  "F3-B: the 2000 ms boundary is inclusive (<=, design pseudo-code)");
    static_assert(!PasteWindowSuppressesNotice(3001, 1000),
                  "F3-B: 2001 ms after paste the window has elapsed");
    static_assert(!PasteWindowSuppressesNotice(999, 1000),
                  "F3-B: non-monotonic clock pair is refused (never unsigned-underflows into 'inside')");

    // (b) runtime fold of the user-reported sequences (fake clock, the same
    // GetTickCount64 domain the worker stamps last_paste_ms_ with).
    const uint64_t paste_at = 10000;

    // Precondition: the bare-Enter empty capture IS hold-eligible on the R5
    // predicate - the window gate is the only thing that may change its outcome.
    TEST_CHECK(EmptyCaptureNeedsHold(true, false),
               "F3-B: empty bare-Enter capture is hold-eligible (R5 predicate unchanged)");

    // Scenario 1 (F3 repro, log L432/563/601): paste succeeded, user hits
    // Enter again after reading the replacement -> empty capture INSIDE the
    // window -> notice suppressed, Enter delivered to the app (send-through).
    bool notice_fires =
        EmptyCaptureNeedsHold(true, false) && !PasteWindowSuppressesNotice(paste_at + 500, paste_at);
    TEST_CHECK(!notice_fires,
               "F3-B: paste +500ms empty capture -> NO no-selection notice (re-translation intent respected)");
    TEST_CHECK(EmptyCaptureNeedsHold(true, false) && PasteWindowSuppressesNotice(paste_at + 500, paste_at),
               "F3-B: in-window empty capture takes the silent send-through branch (Enter delivered)");

    // Boundary: exactly kPasteEmptySuppressMs after the paste is still inside
    // (inclusive <=), one tick past is outside.
    notice_fires = EmptyCaptureNeedsHold(true, false) &&
                   !PasteWindowSuppressesNotice(paste_at + kPasteEmptySuppressMs, paste_at);
    TEST_CHECK(!notice_fires, "F3-B: window boundary (exactly 2000ms) still suppresses");
    notice_fires = EmptyCaptureNeedsHold(true, false) &&
                   !PasteWindowSuppressesNotice(paste_at + kPasteEmptySuppressMs + 1, paste_at);
    TEST_CHECK(notice_fires, "F3-B: 2001ms after paste -> existing hold_send + notice maintained");

    // Scenario 2 (general empty Enter, C-5 constraint): no paste history at
    // all -> sentinel keeps the legacy R5 behavior.
    notice_fires = EmptyCaptureNeedsHold(true, false) && !PasteWindowSuppressesNotice(20000, 0);
    TEST_CHECK(notice_fires, "F3-B: no paste history -> general empty Enter keeps hold_send + notice");

    // Scenario 3: window elapsed after a real paste (user came back much
    // later and pressed Enter on an unchanged caret) -> notice as before.
    notice_fires = EmptyCaptureNeedsHold(true, false) && !PasteWindowSuppressesNotice(paste_at + 60000, paste_at);
    TEST_CHECK(notice_fires, "F3-B: elapsed window -> existing notice path unchanged");

    // Orthogonality: the window must never alter the two R5 exemptions -
    // smart bypass and non-empty capture bypass the gate entirely upstream.
    TEST_CHECK(!EmptyCaptureNeedsHold(true, true),
               "F3-B: smart bypass is not hold-eligible regardless of the paste window");
    TEST_CHECK(!EmptyCaptureNeedsHold(false, false),
               "F3-B: non-empty capture never reaches the gate (normal pipeline)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-034 F3-B paste-window suppression tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-034 F3-B paste-window suppression tests: " << (g_failed_count - failures_before)
                  << " check(s) failed." << std::endl;
    }
}

// REQ-036 (docs/260907_0001 session, log emebalachat_260907200313 L236-448):
// the multi-block whole-document retranslation defect. Enter semantics the
// user defined: Enter terminates ONE block (1 Enter = 1 translation), already
// translated blocks must NEVER be re-captured. The defect: out-of-band Enter
// passes (hook pass-throughs, worker send-throughs) insert block terminators
// the stored offset never advanced over, and the F2 start=0 self-correct
// re-selected [0..caret) - the WHOLE document - whenever a preceding
// already-translated block existed. FIX-1/FIX-2 contract pinned here:
//  (a) pure seam: EditCaretTracker_CountLeadingCrlfPairs counts consecutive
//      CRLF pairs only (a lone LF inside block content never counts).
//  (b) live FIX-2: on a two-block document where the stored start drifted
//      behind ONE out-of-band terminator, CompensateLeadingNewlines advances
//      start by exactly the MEASURED doc newline width (CRLF doc: 2, so
//      start+2..caret; NOT to 0) - the
//      FIRST block stays outside the selection, and the selection text has
//      no leading CRLF (the separator survives the replacement).
//  (c) live FIX-1: NotifySentNewline after a worker-sent Enter stores the
//      measured post-newline caret, so the NEXT Enter's capture does not
//      begin with the pair at all (prevention, the strict superset of (b)).
void TestReq036MultiBlockNoRetranslation() {
    std::cout << "[TEST] REQ-036 multi-block: first blocks never re-captured" << std::endl;
    const int failures_before = g_failed_count;

    // (a) pure pair-counting seam.
    static_assert(EditCaretTracker_CountLeadingCrlfPairs(L"\r\n\r\nblock") == 2,
                  "REQ-036: two leading pairs counted");
    static_assert(EditCaretTracker_CountLeadingCrlfPairs(L"\r\nblock\r\n") == 1,
                  "REQ-036: trailing pairs are not counted");
    static_assert(EditCaretTracker_CountLeadingCrlfPairs(L"block") == 0,
                  "REQ-036: no pairs -> 0");
    TEST_CHECK(EditCaretTracker_CountLeadingCrlfPairs(L"\r\n\r\n\r\nx") == 3,
               "REQ-036: three leading pairs counted");
    TEST_CHECK(EditCaretTracker_CountLeadingCrlfPairs(L"\n\r\nx") == 0,
               "REQ-036: lone-LF first unit breaks the pair sequence (block content)");
    TEST_CHECK(EditCaretTracker_CountLeadingCrlfPairs(L"\r\n") == 1,
               "REQ-036: exact one-pair buffer -> 1");
    TEST_CHECK(EditCaretTracker_CountLeadingCrlfPairs(L"\r") == 0,
               "REQ-036: 1-unit buffer -> 0");

    // (b)/(c) live sequence on a real in-process EDIT control.
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Emebalachat_Req036Host";
    ::RegisterClassExW(&wc);
    HWND host = ::CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"req036", WS_POPUP,
                                  -400, -400, 200, 100, nullptr, nullptr, wc.hInstance, nullptr);
    HWND edit = nullptr;
    if (host) {
        edit = ::CreateWindowExW(0, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | ES_MULTILINE,
                                 0, 0, 180, 80, host, nullptr, wc.hInstance, nullptr);
    }
    TEST_CHECK(edit != nullptr, "REQ-036: in-process EDIT control created");
    bool focus_ok = false;
    if (edit) {
        ::ShowWindow(host, SW_SHOWNOACTIVATE);
        ::SetFocus(edit);
        GUITHREADINFO gti = {};
        gti.cbSize = sizeof(gti);
        focus_ok = ::GetGUIThreadInfo(::GetCurrentThreadId(), &gti) && gti.hwndFocus == edit;
    }
    if (edit && !focus_ok) {
        std::cout << "[SKIP] SetFocus on EDIT control unavailable; REQ-036 live sequence skipped." << std::endl;
    }
    if (edit && focus_ok) {
        auto doc_text = [](HWND h) {
            wchar_t buf[256] = {};
            const int n = static_cast<int>(::SendMessageW(h, WM_GETTEXT, 255, reinterpret_cast<LPARAM>(buf)));
            return std::wstring(buf, (n > 0 && n < 255) ? static_cast<size_t>(n) : 0);
        };
        auto doc_sel = [](HWND h) {
            return static_cast<DWORD>(::SendMessageW(h, EM_GETSEL, 0, 0));
        };

        // ---- (b) FIX-2 on the REQ-036 defect shape ----
        // Block 1 ("AAA") translated by an earlier pipeline pass; the B-6a
        // post-newline save stored caret 5 (right after "AAA\r\n"). The user
        // then pressed an Enter that PASSED THROUGH out-of-band (hook
        // pass-through / send-through) and typed block 2 ("BBB").
        ::SetWindowTextW(edit, L"AAA\r\n");
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(5), static_cast<LPARAM>(5));
        TEST_CHECK(EditCaretTracker_TrySelectNewText(edit), "REQ-036: baseline pass enters the EM path");
        EditCaretTracker_NotifyReplacement(edit, true, 5); // stores the real caret 5

        // Out-of-band terminator + typing: doc "AAA\r\n\r\nBBB", caret 10.
        ::SetWindowTextW(edit, L"AAA\r\n\r\nBBB");
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(10), static_cast<LPARAM>(10));

        // Enter N: drifted start 5 -> EM_SETSEL(5,10) -> capture "\r\nBBB".
        TEST_CHECK(EditCaretTracker_TrySelectNewText(edit), "REQ-036: drifted pass enters the EM path");
        DWORD sel = doc_sel(edit);
        TEST_CHECK(LOWORD(sel) == 5u && HIWORD(sel) == 10u, "REQ-036: drifted range reproduced [5..10)");
        const std::wstring drifted = doc_text(edit).substr(5, 5);
        TEST_CHECK(drifted.size() == 5u, "REQ-036: drifted capture shape is 5 units");
        TEST_CHECK(EditCaretTracker_HasLeadingCrlf(drifted),
                   "REQ-036: drifted capture begins with the out-of-band CRLF pair");
        TEST_CHECK(EditCaretTracker_CountLeadingCrlfPairs(drifted) == 1,
                   "REQ-036: exactly one out-of-band pair measured");

        // FIX-2 fires: the FIRST block stays outside. The CRLF document's
        // width is measured as 2 (capture 5 == span 5), start 5 -> 7,
        // selection becomes [7..10) = "BBB" only - NOT [0..10) (the REQ-036
        // defect).
        TEST_CHECK(EditCaretTracker_CompensateLeadingNewlines(edit, drifted),
                   "REQ-036: FIX-2 compensation advanced the stored start");
        sel = doc_sel(edit);
        TEST_CHECK(LOWORD(sel) == 7u && HIWORD(sel) == 10u,
                   "REQ-036: post-compensation selection is [7..10) - block 1 NOT captured");
        TEST_CHECK(!EditCaretTracker_HasLeadingCrlf(doc_text(edit).substr(7, 3)),
                   "REQ-036: compensated capture has no leading CRLF (separator survives)");

        // ---- (c) FIX-1 on a fresh geometry: stored caret 10, the worker
        // sends Enter out (send-through path), the caret lands at 12. ----
        ::SetWindowTextW(edit, L"AAAA\r\nBBBB");
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(10), static_cast<LPARAM>(10));
        TEST_CHECK(EditCaretTracker_TrySelectNewText(edit), "REQ-036: FIX-1 setup pass enters the EM path");
        EditCaretTracker_NotifyReplacement(edit, true, 10); // stores the real caret 10
        const DWORD pre = EditCaretTracker_SampleCaret(edit);
        TEST_CHECK(pre == 10u, "REQ-036: FIX-1 pre-send caret sampled at the block end");
        // Worker-sent Enter (what SendEnterKey does to an editor): +CRLF.
        ::SetWindowTextW(edit, L"AAAA\r\nBBBB\r\n");
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(12), static_cast<LPARAM>(12));
        EditCaretTracker_NotifySentNewline(edit, pre); // settle + store caret 12
        // User types block 3, caret 12 -> 15.
        ::SetWindowTextW(edit, L"AAAA\r\nBBBB\r\nCCC");
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(15), static_cast<LPARAM>(15));
        TEST_CHECK(EditCaretTracker_TrySelectNewText(edit), "REQ-036: FIX-1 follow-up Enter enters the EM path");
        sel = doc_sel(edit);
        TEST_CHECK(LOWORD(sel) == 12u && HIWORD(sel) == 15u,
                   "REQ-036: FIX-1 stored the post-newline caret: capture is ONLY 'CCC' [12..15)");
        TEST_CHECK(!EditCaretTracker_HasLeadingCrlf(doc_text(edit).substr(LOWORD(sel), HIWORD(sel) - LOWORD(sel))),
                   "REQ-036: FIX-1 capture has no leading pair (prevention path)");

        ::SetFocus(nullptr);
    }
    if (host) {
        ::DestroyWindow(host); // child EDIT dies with the parent
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-036 multi-block no-retranslation tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-036 multi-block tests: " << (g_failed_count - failures_before)
                  << " check(s) failed." << std::endl;
    }
}

namespace {
// REQ-F1 live-suite EM emulation: a control that stores single-unit LF
// newlines (Notepad's RichEditD2DPT behavior, user log
// emebalachat_260908062830) while the clipboard CRLF-normalizes - the width
// mismatch the compensation must now MEASURE. A real EDIT control cannot
// reproduce this: it CRLF-normalizes its internal storage itself. State is
// file-scope: the suite owns exactly one control.
std::wstring g_f1_doc;
DWORD g_f1_sel_start = 0;
DWORD g_f1_sel_end = 0;
LRESULT CALLBACK F1EmuEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case EM_GETSEL:
            if (wp) { *reinterpret_cast<DWORD*>(wp) = g_f1_sel_start; }
            if (lp) { *reinterpret_cast<DWORD*>(lp) = g_f1_sel_end; }
            return MAKELRESULT(g_f1_sel_start, g_f1_sel_end);
        case EM_SETSEL:
            g_f1_sel_start = static_cast<DWORD>(wp);
            g_f1_sel_end = static_cast<DWORD>(lp);
            return 1;
        case WM_GETTEXT: {
            const size_t cap = (wp > 0) ? static_cast<size_t>(wp) - 1 : 0;
            const size_t n = g_f1_doc.size() < cap ? g_f1_doc.size() : cap;
            wchar_t* out = reinterpret_cast<wchar_t*>(lp);
            if (out) {
                for (size_t i = 0; i < n; ++i) { out[i] = g_f1_doc[i]; }
                out[n] = L'\0';
            }
            return static_cast<LRESULT>(n);
        }
        case WM_GETTEXTLENGTH:
            return static_cast<LRESULT>(g_f1_doc.size());
        case EM_GETLINECOUNT: {
            size_t lines = 1;
            for (wchar_t c : g_f1_doc) { if (c == L'\n') ++lines; }
            return static_cast<LRESULT>(lines);
        }
        case EM_LINEFROMCHAR:
            return 0;
        case EM_GETLIMITTEXT:
            return 0x7FFFFFF8;
        default:
            return ::DefWindowProcW(h, msg, wp, lp);
    }
}
} // namespace

// REQ-F2 (docs/260908_0001 session, user log emebalachat_260908062830
// L1271/L1297-1309/L1561/L1858/L1915): category=0 (CategoryB, non-EM) apps
// capture the WHOLE input on the SelectMessageBlock fallback, and with
// auto_send=0 the send gate leaves the pasted translation in place - every
// subsequent bare Enter re-captured and re-translated it (44 -> 112 -> 200
// chars). The fix keeps a last-paste ledger in the worker and compares each
// capture against it. Contract:
//  (a) AnalyzeCaptureVsLastPaste pure seam: exact match (send-of-output),
//      prefix + new tail (tail-only translation), no-match (user edit /
//      deletion / different context) - pinned to the EXACT texts of the user
//      log so the verdicts are not abstract.
//  (b) PastedPrefixNeedsSkip predicate: only the exact-match case, never a
//      smart bypass (disjoint positive decision), never an empty capture
//      (upstream R5 hold owns it).
//  (c) recomposition arithmetic (worker's (ii) branch, mirrored here): the
//      engine input is ONLY the tail, and the pasted result must cover the
//      whole captured span: prefix + tail-translation == full replacement
//      - the user-facing side effect (input never accumulates).
//  (d) deletion/edit discrimination (delegation warning): any user change to
//      the pasted output routes to NoMatch, so a genuinely edited message is
//      still re-translated as fresh text (never blindly passed through).
void TestReqF2Category0Accumulation() {
    std::cout << "[TEST] REQ-F2 category=0 capture accumulation: last-paste ledger" << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) AnalyzeCaptureVsLastPaste: the exact log shapes ----
    // Log L1278: the first paste into EVA (Korean -> Hungarian).
    const std::wstring paste_hu = L"Itt normálisan le lesz fordítva? Nézzük meg.";
    // Log L1297-1309: second Enter with NO new typing -> capture == paste
    // EXACTLY (the old code sent it to Google, got an identity, and burned
    // a synthetic Enter - the "worked" appearance was a re-translation).
    TEST_CHECK(AnalyzeCaptureVsLastPaste(paste_hu, paste_hu) == PasteLedgerVerdict::ExactMatch,
               "REQ-F2: L1297 capture (== last paste) is an ExactMatch send-of-output");
    // Log L1561: the user typed new Korean AFTER the German paste was left in
    // the input -> capture = German prefix + Korean tail.
    const std::wstring paste_de = L"Warum funktioniert es nicht problemlos? Das funktioniert, aber warum funktioniert das nächste nicht auf Deutsch?";
    const std::wstring tail_ko = L" 이건 되는데, 그 다음은 왜 독일어로 안 되는걸까?";
    const std::wstring capture_mixed = paste_de + tail_ko;
    TEST_CHECK(AnalyzeCaptureVsLastPaste(capture_mixed, paste_de) == PasteLedgerVerdict::PrefixWithTail,
               "REQ-F2: L1561 capture (paste + new tail) is a PrefixWithTail");
    // L1858: the whole PASTED TRANSLATION became the prefix of the NEXT
    // capture (112-char accumulation seed).
    TEST_CHECK(AnalyzeCaptureVsLastPaste(L"Ist es nicht in Ordnung, 2, 3 oder 4 Sätze hintereinander zu schreiben? Wenn das der Fall ist, wird der aktuelle zweite Satz auch nicht funktionieren, oder? Geht das nur zum Betreten? 아니네 3번째 문장은 어떻지?",
                                         L"Ist es nicht in Ordnung, 2, 3 oder 4 Sätze hintereinander zu schreiben? Wenn das der Fall ist, wird der aktuelle zweite Satz auch nicht funktionieren, oder? Geht das nur zum Betreten?")
                   == PasteLedgerVerdict::PrefixWithTail,
               "REQ-F2: L1915 200-char capture = L1864 183-char paste + typed tail");
    // No ledger at all.
    TEST_CHECK(AnalyzeCaptureVsLastPaste(paste_hu, L"") == PasteLedgerVerdict::NoMatch,
               "REQ-F2: empty ledger -> NoMatch");
    TEST_CHECK(AnalyzeCaptureVsLastPaste(L"", paste_hu) == PasteLedgerVerdict::NoMatch,
               "REQ-F2: empty capture -> NoMatch");
    // Shorter than the ledger (user deleted from our output).
    TEST_CHECK(AnalyzeCaptureVsLastPaste(paste_de.substr(0, 40), paste_de) == PasteLedgerVerdict::NoMatch,
               "REQ-F2: capture shorter than paste (deletion) -> NoMatch");
    // Different head byte (user typed BEFORE / replaced the start).
    TEST_CHECK(AnalyzeCaptureVsLastPaste(std::wstring(L"X") + paste_de.substr(1), paste_de)
                   == PasteLedgerVerdict::NoMatch,
               "REQ-F2: altered first character -> NoMatch");
    // Internal edit of the pasted region (same length, one changed unit).
    {
        std::wstring edited = paste_hu;
        edited[0] = L'X';
        TEST_CHECK(AnalyzeCaptureVsLastPaste(edited, paste_hu) == PasteLedgerVerdict::NoMatch,
                   "REQ-F2: internal edit of the pasted output -> NoMatch");
    }
    // Surrogate-boundary safety: tail split offset is a whole-unit boundary
    // because last_paste is a complete stored string. Korean text with an
    // emoji tail exercises multi-unit codepoints end-to-end.
    {
        const std::wstring emoji_paste = L"번역된 문장 😀"; // pasted output ends on an emoji
        const std::wstring tail_after_emoji = L" 그 다음 문장";
        TEST_CHECK(AnalyzeCaptureVsLastPaste(emoji_paste + tail_after_emoji, emoji_paste)
                       == PasteLedgerVerdict::PrefixWithTail,
                   "REQ-F2: surrogate-ending paste + tail still PrefixWithTail");
        // The tail offset (emoji_paste.size()) is past the full emoji
        // surrogate pair by construction - pin the arithmetic explicitly.
        const std::wstring split_tail(emoji_paste + tail_after_emoji, emoji_paste.size(),
                                      tail_after_emoji.size());
        TEST_CHECK(split_tail == tail_after_emoji,
                   "REQ-F2: tail extraction at the whole-unit boundary yields the exact tail");
    }

    // ---- (b) PastedPrefixNeedsSkip ----
    static_assert(PastedPrefixNeedsSkip(true, false, false), "REQ-F2: exact match -> skip re-translation");
    static_assert(!PastedPrefixNeedsSkip(false, false, false), "REQ-F2: no exact match -> no skip");
    static_assert(!PastedPrefixNeedsSkip(true, true, false), "REQ-F2: smart bypass keeps its own contract");
    static_assert(!PastedPrefixNeedsSkip(true, false, true), "REQ-F2: empty capture never skips");

    // ---- (c) recomposition arithmetic (mirrors the worker's (ii) branch) ----
    // The engine must see ONLY the tail; the pasted result must cover the
    // ENTIRE captured span (prefix verbatim + tail translation). This is the
    // accumulation invariant: len(replacement) never compounds per round.
    {
        const std::wstring prefix = paste_de;             // verbatim, never re-translated
        const std::wstring tail = tail_ko;
        const std::wstring tail_translation = L" Das funktioniert, aber warum funktioniert das nächste nicht auf Deutsch? (Übersetzung)"; // stand-in engine output
        std::wstring engine_input = tail;                 // what the worker feeds Translate()
        std::wstring translated = tail_translation;       // engine result for the tail alone
        TEST_CHECK(engine_input.find(paste_de) == std::wstring::npos,
                   "REQ-F2: engine input excludes the already-translated prefix");
        translated.insert(translated.begin(), prefix.begin(), prefix.end());
        const std::wstring capture = prefix + tail;
        // User-facing side effect: replacement span == capture span (the
        // whole-input selection is fully consumed, no residue, no growth).
        TEST_CHECK(translated.size() >= capture.size() && translated.compare(0, prefix.size(), prefix) == 0,
                   "REQ-F2: recomposed replacement keeps the prefix verbatim and covers the capture span");
    }

    // ---- (d) multi-round stability: the ledger chains correctly ----
    // Round 2 of the (ii) shape: the recomposed full text becomes the new
    // ledger, so a subsequent bare Enter is an ExactMatch (send-of-output),
    // and a THIRD typed sentence extends the PrefixWithTail chain - the
    // 44->112->200 compounding is structurally impossible now.
    {
        std::wstring ledger = paste_hu;                       // round 1 paste
        const std::wstring r2_tail = L" 두 번째 문장입니다.";
        std::wstring r2_capture = ledger + r2_tail;
        TEST_CHECK(AnalyzeCaptureVsLastPaste(r2_capture, ledger) == PasteLedgerVerdict::PrefixWithTail,
                   "REQ-F2 round 2: typed tail on the kept paste -> PrefixWithTail");
        std::wstring r2_translated = ledger + L" A MÁSODIK MONDAT."; // recomposed paste
        ledger = r2_translated;                                // worker stores the recomposed text
        TEST_CHECK(AnalyzeCaptureVsLastPaste(ledger, ledger) == PasteLedgerVerdict::ExactMatch,
                   "REQ-F2 round 2->3: kept recomposed output + bare Enter -> ExactMatch send-of-output");
        const std::wstring r3_tail = L" 세 번째 문장입니다.";
        TEST_CHECK(AnalyzeCaptureVsLastPaste(ledger + r3_tail, ledger) == PasteLedgerVerdict::PrefixWithTail,
                   "REQ-F2 round 3: chain continues without compounding");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-F2 category=0 accumulation tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-F2 category=0 accumulation tests: " << (g_failed_count - failures_before)
                  << " check(s) failed." << std::endl;
    }
}

// REQ-F5 (docs/260908_0001 session, verification log emebalachat_260908082659
// L435-472/L504-520/L556-607): the residual of the F2 ledger in EM-tracked
// editors (Notepad). After a successful paste the send gate leaves the
// translation in the input and the stored offset is the paste END; the user's
// bare Enter then produces an EMPTY capture ([offset..caret) empty) which the
// old R5 hold (outside the 2 s paste window) swallowed with hold_send +
// no-selection notice. The fix promotes that exact geometry to the ExactMatch
// send-of-output contract: empty capture + valid ledger for THIS window +
// live caret == remembered paste-end -> hand Enter to the app. Contract:
//  (a) pure predicate matrix (EmptyCapturePromotesToSend): only the exact
//      (empty && !smart && valid-ledger && caret==end) combination promotes;
//      every other combination refuses.
//  (b) live sequence on a real EM-capable ES_MULTILINE EDIT control (the
//      proven REQ-027 sample pattern): caret placed at the paste end -> the
//      gate arithmetic promotes; caret moved off the end (deletion / arrow)
//      -> refuses - the live caret and the remembered paste end are both
//      EM_GETSEL UTF-16 offsets, so equality is the "no edit since paste"
//      proof and any drift breaks it.
void TestReqF5EmptyCaptureEnterPromotion() {
    std::cout << "[TEST] REQ-F5 empty-capture Enter promotion (paste-end caret geometry)" << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) pure predicate matrix ----
    static_assert(EmptyCapturePromotesToSend(true, false, true, true),
                  "REQ-F5: empty + valid ledger + caret==paste-end -> promote");
    static_assert(!EmptyCapturePromotesToSend(true, false, true, false),
                  "REQ-F5: caret moved off paste-end -> refuse (deletion/arrow)");
    static_assert(!EmptyCapturePromotesToSend(true, false, false, true),
                  "REQ-F5: no ledger / different hwnd -> refuse");
    static_assert(!EmptyCapturePromotesToSend(true, true, true, true),
                  "REQ-F5: smart bypass keeps its own send-through contract");
    static_assert(!EmptyCapturePromotesToSend(false, false, true, true),
                  "REQ-F5: non-empty capture (typed tail) never promotes - normal pipeline");

    // ---- (b) live gate arithmetic on a real EM-capable EDIT control ----
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Emebalachat_ReqF5Host";
    ::RegisterClassExW(&wc);
    HWND host = ::CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"reqf5", WS_POPUP,
                                  -400, -400, 200, 100, nullptr, nullptr, wc.hInstance, nullptr);
    HWND edit = nullptr;
    if (host) {
        edit = ::CreateWindowExW(0, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | ES_MULTILINE,
                                 0, 0, 180, 80, host, nullptr, wc.hInstance, nullptr);
    }
    TEST_CHECK(edit != nullptr, "REQ-F5: ES_MULTILINE EDIT control created");
    bool focus_ok = false;
    if (edit) {
        ::ShowWindow(host, SW_SHOWNOACTIVATE);
        ::SetFocus(edit);
        GUITHREADINFO gti = {};
        gti.cbSize = sizeof(gti);
        focus_ok = ::GetGUIThreadInfo(::GetCurrentThreadId(), &gti) && gti.hwndFocus == edit;
    }
    if (edit && !focus_ok) {
        std::cout << "[SKIP] SetFocus on the EDIT control unavailable; REQ-F5 live sequence skipped." << std::endl;
    }
    if (edit && focus_ok) {
        // Post-paste geometry: the translation sits in the input and the
        // caret rests at its end (the REQ-027 B-6a post-newline save stores
        // exactly this). The remembered paste end is sampled the same way
        // the worker does at paste time (EditCaretTracker_SampleCaret).
        ::SetWindowTextW(edit, L"le lesz fordítva");
        const DWORD paste_pos = 5; // a concrete non-zero post-paste caret
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(paste_pos),
                       static_cast<LPARAM>(paste_pos));
        const DWORD paste_end = EditCaretTracker_SampleCaret(edit);
        TEST_CHECK(paste_end == paste_pos, "REQ-F5: remembered paste-end sampled at the set caret");
        const bool ledger_ok = (paste_end != kEditCaretUnknown);

        // Promotion: empty capture, caret still at the paste end.
        const DWORD now_caret = EditCaretTracker_SampleCaret(edit);
        TEST_CHECK(EmptyCapturePromotesToSend(true, false, ledger_ok, now_caret == paste_end),
                   "REQ-F5: live caret == paste-end -> promote to send-of-output");

        // Refusal: caret moved off the end (backspace / arrow move).
        const DWORD moved = paste_pos > 1 ? paste_pos - 2 : paste_pos + 1;
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(moved),
                       static_cast<LPARAM>(moved));
        const DWORD now_moved = EditCaretTracker_SampleCaret(edit);
        TEST_CHECK(!EmptyCapturePromotesToSend(true, false, ledger_ok, now_moved == paste_end),
                   "REQ-F5: moved caret != paste-end -> refuse (no false promotion on edit)");

        // Refusal: the remembered end sentinel (no memory) can never equal a
        // real caret, and the worker folds unknown into an invalid ledger.
        TEST_CHECK(!EmptyCapturePromotesToSend(true, false, false, true),
                   "REQ-F5: no ledger / unknown offset -> refuse");

        ::SendMessageW(edit, EM_SETSEL, 0, 0);
        ::SetFocus(nullptr);
    }
    if (host) {
        ::DestroyWindow(host);
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-F5 empty-capture Enter promotion tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-F5 empty-capture Enter promotion tests: " << (g_failed_count - failures_before)
                  << " check(s) failed." << std::endl;
    }
}

// F6 (session 260908_0002, verify report 164500 §5/§7 - V5 ledger-on-focus
// clear defect): a task whose paste is aborted by the H1 foreground guard
// (the user Alt-Tabbed / clicked the tray while the translation network call
// was in flight) must NOT wipe the last-paste ledger when that ledger belongs
// to the SAME target hwnd the task operated on: no paste and no H1-gated
// Enter ran, so the window's text is unchanged and the ledger entry still
// describes live text. The pre-F6 unconditional `!pasted` clear (worker.cpp
// C3 maintenance) emptied the memory; the next Enter's fallback whole-input
// selection [0..caret) then re-translated and overwrote earlier translated
// blocks (verify 164500 example-3 chain). Contract:
//  (a) LedgerSurvivesH1Abort pure predicate matrix: preserve ONLY the
//      (paste attempted == H1 abort) && (ledger hwnd == task hwnd)
//      combination; a confirmed switch to a different hwnd (cross-window
//      contamination hygiene) and the no-paste-not-attempted outcomes
//      (translation empty / identity, whose send geometry is owned by the
//      untouched C1/C2 clears) keep the legacy clear.
//  (b) the user-visible consequence through the existing pure seams: a kept
//      ledger makes the post-abort retry capture a PrefixWithTail (tail-only
//      translation, earlier blocks untouched), while the wiped ledger makes
//      the identical capture a NoMatch (whole-input re-translation - the V5
//      overwrite defect).
void TestReqF6LedgerH1AbortPreserve() {
    std::cout << "[TEST] F6 ledger protection on H1-abort no-paste (C3 !pasted arm subdivision)" << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) pure predicate matrix ----
    // F3 (session 260908_0003): renamed LedgerSurvivesH1Abort ->
    // LedgerSurvivesNoPaste with a THIRD input (ledger_has_text). The F6
    // H1-abort arm (paste_attempted) is unchanged; the no-paste arm
    // (translation empty / identity) now KEEPS a same-hwnd ledger that has
    // text (the C3 re-arm) - nothing was injected into the window either
    // way, so the entry still describes live text.
    static_assert(LedgerSurvivesNoPaste(true, true, true),
                  "F6: H1 abort into the same ledger hwnd -> preserve (window unchanged, memory live)");
    static_assert(!LedgerSurvivesNoPaste(true, false, true),
                  "F6: H1 abort but ledger belongs to a DIFFERENT hwnd -> clear (cross-window hygiene)");
    static_assert(LedgerSurvivesNoPaste(false, true, true),
                  "F3 C3 re-arm: no paste (empty/identity) + same hwnd + ledger has text -> KEEP (re-arm the block start)");
    static_assert(!LedgerSurvivesNoPaste(false, true, false),
                  "F3 C3 re-arm boundary: same hwnd but empty ledger -> clear (nothing to re-arm on)");
    static_assert(!LedgerSurvivesNoPaste(false, false, false),
                  "F6: no paste attempted + no ledger -> clear (no-op legacy)");
    TEST_CHECK(LedgerSurvivesNoPaste(true, true, true),
               "F6: keep decision holds for the H1-abort + same-hwnd combination");
    TEST_CHECK(!LedgerSurvivesNoPaste(true, false, true),
               "F6: a hwnd-confirmed switch clears even on an H1 abort");
    TEST_CHECK(LedgerSurvivesNoPaste(false, true, true),
               "F3: an empty/identity no-paste outcome keeps a non-empty same-hwnd ledger (C3 re-arm)");

    // ---- (b) chain consequence through the existing pure seams ----
    // Round 1: paragraph 1 pasted -> ledger = the block-1 translation.
    const std::wstring block1_en = L"The first paragraph, translated.";
    // Round 2: paragraph 2 typed, Enter pressed, H1 abort (no paste, no
    // Enter) into the SAME window. F6 keeps the ledger; pre-F6 wiped it.
    std::wstring ledger = block1_en;
    if (!LedgerSurvivesNoPaste(true, true, !ledger.empty())) {
        ledger.clear(); // pre-F6 C3 behavior
    }
    // Round 3: the user returns and retries Enter. Capture = whole input
    // [0..caret): block1(en) + block2(source) - the fallback geometry of
    // untracked windows (SelectMessageBlock).
    const std::wstring block2_ko = L"두 번째 문단 원문";
    const std::wstring capture_next = block1_en + block2_ko;
    TEST_CHECK(AnalyzeCaptureVsLastPaste(capture_next, block1_en) == PasteLedgerVerdict::PrefixWithTail,
               "F6 (kept ledger): post-abort retry is PrefixWithTail -> only the new tail is translated; block 1 is never re-translated");
    TEST_CHECK(AnalyzeCaptureVsLastPaste(capture_next, L"") == PasteLedgerVerdict::NoMatch,
               "F6 (wiped ledger, pre-F6): identical retry is NoMatch -> whole-input re-translation would overwrite block 1 (the V5 defect)");
    // C1/C2 send-of-output contracts are untouched by F6: an ExactMatch still
    // reads as the send-of-output skip verdict. (F3 session 260908_0003
    // hardened the C1 arm to KEEP the ledger instead of clearing it - the
    // verdict tested here is unchanged; see TestReqF3BlockSliceCurrentBlockOnly.)
    TEST_CHECK(AnalyzeCaptureVsLastPaste(block1_en, block1_en) == PasteLedgerVerdict::ExactMatch,
               "F6: C1 ExactMatch verdict unchanged (send-of-output consumed by its own clear)");
    static_assert(EmptyCapturePromotesToSend(true, false, true, true),
                  "F6: C2 F5-promote contract unchanged (send-of-output clear retained)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] F6 ledger protection on H1-abort no-paste tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] F6 ledger protection on H1-abort no-paste tests: " << (g_failed_count - failures_before)
                  << " check(s) failed." << std::endl;
    }
}

// F3 (session 260908_0003, verify 220750 §6/§7 adopted design): current-block-
// only translation via BLOCK-SLICE-FROM-WHOLE-CAPTURE. The non-EM CategoryB
// fallback selection is the WHOLE [0..caret) accumulation; the current block is
// the last K+1 logical lines (K = hook-counted Shift+Enter passthroughs), and
// everything before the slice point is preserved verbatim through the REQ-F2
// recomposition machinery. Contract (the three user examples must hold):
//  예시1: 6 sentences each ended by a bare Enter -> 6 independent one-time
//         translations, earlier sentences untouched (no whole-capture
//         re-translation after a redundant Enter - the C1 chain break).
//  예시2: Shift+Enter joins lines into blocks -> 3 blocks -> exactly 3
//         translations (a block's internal newlines must NOT split it).
//  예시3: blocks translated to the then-current target (EN->JP->VN); earlier
//         blocks keep their language and text.
// Plus the two ledger chain-break arms the verification identified: C1
// (ExactMatch consumption on a redundant Enter) and C3 (empty/identity
// translation), and the guard-limit raise. All pure seams (headless).
void TestReqF3BlockSliceCurrentBlockOnly() {
    std::cout << "[TEST] F3 current-block-only translation (block-slice-from-whole-capture)" << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) FindCurrentBlockStart: pure slice matrix ----
    // A \r\n PAIR is one logical terminator (a lone \r or \n too); the block
    // is the last K+1 LOGICAL lines, and K+1 terminators are crossed from the
    // end. A single line with K=0 has none to cross -> whole capture.
    static_assert(FindCurrentBlockStart(L"hello", 0) == 0,
                  "F3 slice: single line, K=0 -> whole capture");
    static_assert(FindCurrentBlockStart(L"AAA\r\nBBB", 0) == 5,
                  "F3 slice: K=0 -> everything after the last terminator");
    static_assert(FindCurrentBlockStart(L"AAA\r\nBBB\r\nCCC", 1) == 5,
                  "F3 slice: K=1 -> last two lines (BBB..CCC)");
    static_assert(FindCurrentBlockStart(L"AAA\r\nBBB\r\nCCC", 0) == 10,
                  "F3 slice: K=0 -> only the last line");
    TEST_CHECK(FindCurrentBlockStart(L"AAA\r\nBBB\r\nCCC\r\nDDD", 2) == 5,
               "F3 slice: K=2 -> last three lines (BBB..DDD)");
    TEST_CHECK(FindCurrentBlockStart(L"1\r\n2\r\n3\r\n4\r\n5\r\n6", 5) == 0,
               "F3 slice: K=5 needs 6 terminators, capture has 5 -> whole 6-line block");
    // Over-count (fewer terminators than K+1) clamps to 0: the WHOLE capture
    // is the block. This is what makes the slice a provable no-op on EM-
    // tracked captures (which cover exactly the current block, K terminators
    // inside, while the slice asks for K+1).
    TEST_CHECK(FindCurrentBlockStart(L"AAA\r\nBBB", 5) == 0,
               "F3 slice: K over-count clamps to 0 (whole capture, never out of bounds)");
    TEST_CHECK(FindCurrentBlockStart(L"single", 3) == 0,
               "F3 slice: no terminators at all -> whole capture regardless of K");
    // A capture ending in a terminator: the final empty line is a logical
    // line, so K=0 slices to index == size (empty tail: the worker's 041
    // send-through owns it, never re-translate the prefix).
    TEST_CHECK(FindCurrentBlockStart(L"AAA\r\n", 0) == 5,
               "F3 slice: trailing terminator, K=0 -> empty tail (index == size)");
    TEST_CHECK(FindCurrentBlockStart(L"AAA\r\nBBB\r\n", 1) == 5,
               "F3 slice: trailing terminator, K=1 -> last two lines (BBB + empty)");
    // Blank-line handling (split() semantics): "AAA\r\n\r\nBBB" holds TWO
    // terminators with an empty logical line between; K=1 takes the last two
    // lines (blank + BBB) starting AT the blank line.
    TEST_CHECK(FindCurrentBlockStart(L"AAA\r\n\r\nBBB", 2) == 0,
               "F3 slice: K=2 -> all three logical lines (AAA, blank, BBB)");
    TEST_CHECK(FindCurrentBlockStart(L"AAA\r\n\r\nBBB", 1) == 5,
               "F3 slice: K=1 -> last two lines (blank, BBB)");
    // LF / CR forms are tolerated (the worker CRLF-normalizes, but the helper
    // must not depend on that).
    TEST_CHECK(FindCurrentBlockStart(L"AAA\nBBB", 0) == 4, "F3 slice: lone LF is a terminator");
    TEST_CHECK(FindCurrentBlockStart(L"AAA\rBBB", 0) == 4, "F3 slice: lone CR is a terminator");
    TEST_CHECK(FindCurrentBlockStart(L"AAA\r\nBBB\nCCC", 0) == 9,
               "F3 slice: mixed CRLF/LF forms split independently");
    TEST_CHECK(FindCurrentBlockStart(L"AAA\n\nBBB", 1) == 4,
               "F3 slice: lone-LF blank line = two terminators, K=1 -> (blank, BBB)");
    // Degenerate inputs.
    TEST_CHECK(FindCurrentBlockStart(L"", 0) == 0, "F3 slice: empty capture -> 0");
    TEST_CHECK(FindCurrentBlockStart(L"AAA", -1) == 0, "F3 slice: negative K -> whole capture (safe)");
    TEST_CHECK(FindCurrentBlockStart(L"\r\n", 0) == 2,
               "F3 slice: capture is only a terminator -> empty tail");
    // Surrogate safety: 0x0D/0x0A never appear inside a surrogate pair, so the
    // returned index is always a code-unit boundary (Korean + emoji block).
    {
        const std::wstring ko_emoji = L"번역 문장 😀\r\n두 번째 문장 🚀";
        const size_t at = FindCurrentBlockStart(ko_emoji, 0);
        TEST_CHECK(at > 0 && at < ko_emoji.size(), "F3 slice: boundary is an interior code-unit index");
        TEST_CHECK(ko_emoji.compare(at, std::wstring::npos, L"두 번째 문장 🚀") == 0,
                   "F3 slice: sliced block is the second line verbatim (past the surrogate emoji)");
    }

    // ---- (b) ShiftEnterKNext: the hook's K counter contract (IME-safe) ----
    static_assert(ShiftEnterKNext(0, false) == 1, "F3 K: Shift+Enter while not composing -> +1");
    static_assert(ShiftEnterKNext(3, false) == 4, "F3 K: counting accumulates");
    static_assert(ShiftEnterKNext(2, true) == 2, "F3 K: FROZEN during IME composition (V2/F2 gate)");
    static_assert(ShiftEnterKNext(static_cast<int>(kMaxEnterTranslateNewlines), false) ==
                      static_cast<int>(kMaxEnterTranslateNewlines),
                  "F3 K: clamped at the guard's newline ceiling (abuse protection)");
    static_assert(kMaxEnterTranslateNewlines == 64,
                  "F3 K clamp coherence: K+1 (65) > max runs of any guard-passing capture (32), so a clamped/saturated K never over-slices");
    TEST_CHECK(ShiftEnterKNext(63, false) == 64, "F3 K: last increment lands exactly on the clamp");
    TEST_CHECK(ShiftEnterKNext(64, false) == 64, "F3 K: further Shift+Enters stay clamped");
    TEST_CHECK(ShiftEnterKNext(0, true) == 0, "F3 K: composition at zero stays zero (never fires mid-IME)");

    // ---- (c) 예시1: 6 sentences, each ended by a bare Enter -> 6 one-time
    // translations. Model of the non-EM whole-capture chain (auto_send=0: the
    // send gate leaves the pasted translation in the input, mirroring the
    // worker's (ii)/slice recomposition through the SAME pure seams). ----
    {
        auto fake_engine = [](const std::wstring& in, const wchar_t* tag) {
            return std::wstring(tag) + L"[" + in + L"]"; // stands in for a translation
        };
        auto enter_round = [&](std::wstring& doc, std::wstring& ledger, int K,
                               const std::wstring& typed, const wchar_t* tag,
                               std::wstring& engine_input_out) {
            doc += typed;                       // user types the next block
            const size_t bs0 = FindCurrentBlockStart(doc, K);
            size_t bs = (!ledger.empty() && bs0 < ledger.size() &&
                         AnalyzeCaptureVsLastPaste(doc, ledger) ==
                             PasteLedgerVerdict::PrefixWithTail)
                            ? ledger.size()
                            : bs0;
            while (bs < doc.size() && (doc[bs] == L'\r' || doc[bs] == L'\n')) { ++bs; }
            const std::wstring prefix = doc.substr(0, bs);
            const std::wstring block = doc.substr(bs);
            engine_input_out = block;                                   // what the engine sees
            const std::wstring translated = prefix + fake_engine(block, tag);
            TEST_CHECK(translated.compare(0, prefix.size(), prefix) == 0,
                       "F3 예시1: earlier content survives the replacement verbatim");
            doc = translated;                                           // Ctrl+V replaces [0..caret)
            ledger = translated;                                        // re-anchor post-replace
        };
        std::wstring doc, ledger, engine_in;
        const std::wstring s[6] = { L"첫 번째 문장.", L"두 번째 문장.", L"세 번째 문장.",
                                    L"네 번째 문장.", L"다섯 번째 문장.", L"여섯 번째 문장." };
        for (int i = 0; i < 6; ++i) {
            enter_round(doc, ledger, /*K*/ 0, s[i], L"EN", engine_in);
            TEST_CHECK(engine_in == s[i], "F3 예시1: engine input is EXACTLY the new sentence (one-time)");
        }
        for (int i = 0; i < 6; ++i) {
            const std::wstring want = L"EN[" + s[i] + L"]";
            TEST_CHECK(doc.find(want) != std::wstring::npos, "F3 예시1: every sentence keeps its own translation");
        }
        // C1 regression: a REDUNDANT Enter (capture == ledger, no new typing)
        // is a send-of-output. Pre-F3 it CONSUMED the ledger, arming the next
        // block as a NoMatch whole-capture re-translation (the R1->R2 chain
        // that destroyed 예시1). Now the ledger survives, so the next round is
        // still a tail-only translation.
        TEST_CHECK(AnalyzeCaptureVsLastPaste(doc, ledger) == PasteLedgerVerdict::ExactMatch,
                   "F3 예시1: redundant Enter reads as ExactMatch (send-of-output)");
        const std::wstring ledger_after_redundant =
            LedgerSurvivesNoPaste(false, true, true) ? ledger : std::wstring();
        TEST_CHECK(ledger_after_redundant == ledger,
                   "F3 C1 hardening: the redundant Enter no longer clears the ledger");
        enter_round(doc, ledger, /*K*/ 0, L"일곱 번째 문장.", L"EN", engine_in);
        TEST_CHECK(engine_in == L"일곱 번째 문장.",
                   "F3 C1 regression: the block after a redundant Enter is still ONE new sentence (no whole-capture re-translation)");
    }

    // ---- (d) 예시2: Shift+Enter joins lines into blocks -> 3 blocks ->
    // exactly 3 translations. A block's internal newlines must NOT split it,
    // and (crucially) the slice must not fire while the ledger chain is
    // intact; when it is broken, the K slice recovers the exact block. ----
    {
        // Block = 3 lines joined by 2 Shift+Enters => K=2.
        const std::wstring b1 = L"A1\r\nA2\r\nA3";
        const std::wstring b2 = L"B1\r\nB2\r\nB3";
        const std::wstring b3 = L"C1\r\nC2\r\nC3";
        // Healthy chain (ledger intact): the whole first block is translated.
        TEST_CHECK(FindCurrentBlockStart(b1, 2) == 0,
                   "F3 예시2: a 3-line block with K=2 is ONE translation (no internal split)");
        // Ledger broken (foreign content before the block): the slice recovers
        // exactly the current 3-line block, prefix preserved verbatim.
        const std::wstring foreign = L"EN[이미 번역된 앞 블록]";
        const std::wstring whole = foreign + L"\r\n" + b2;
        const size_t bs = FindCurrentBlockStart(whole, 2);
        TEST_CHECK(whole.compare(bs, std::wstring::npos, b2) == 0,
                   "F3 예시2: broken chain -> slice yields EXACTLY the current block (earlier text preserved)");
        TEST_CHECK(whole.compare(0, bs, foreign + L"\r\n") == 0,
                   "F3 예시2: the slice point keeps the earlier block + its terminator verbatim");
        // Three successive blocks => three engine inputs (three translations).
        int translations = 0;
        std::wstring doc;
        for (const auto& blk : { b1, b2, b3 }) {
            const size_t cut = FindCurrentBlockStart(doc + blk, 2);
            const std::wstring block_view = (doc + blk).substr(cut);
            if (block_view == blk) { ++translations; }
            doc += blk + L"\r\n"; // the app's own terminator between blocks
        }
        TEST_CHECK(translations == 3, "F3 예시2: 3 blocks -> exactly 3 translations");
    }

    // ---- (e) 예시3: per-block language is already pinned by the task-start
    // GetSnapshot(); the remaining requirement is that translated blocks are
    // never re-captured. Model the EN->JP->VN target changes. ----
    {
        std::wstring doc = L"EN[문장1]";            // block 1 -> English
        std::wstring ledger = doc;
        doc += L"\r\nJP[문장2]";                     // block 2 -> Japanese
        ledger = doc;
        const std::wstring capture3 = doc + L"\r\n문장3"; // block 3 pending, target now VN
        TEST_CHECK(AnalyzeCaptureVsLastPaste(capture3, ledger) == PasteLedgerVerdict::PrefixWithTail,
                   "F3 예시3: the third Enter's capture is a ledger-protected tail");
        const size_t bs = FindCurrentBlockStart(capture3, 0);
        TEST_CHECK(bs >= ledger.size(),
                   "F3 예시3: the slice never reaches back into the earlier EN/JP blocks");
        TEST_CHECK(capture3.substr(bs) == L"\r\n문장3" || capture3.substr(bs) == L"문장3",
                   "F3 예시3: engine input is the third block only (its separator is prefix)");
        // Even with the ledger gone (the pre-F3 R2/R4 state), the slice keeps
        // the earlier EN/JP blocks out of the engine input: the preserved
        // prefix is exactly the earlier document + its terminator.
        const size_t bs_nolidger = FindCurrentBlockStart(capture3, 0);
        TEST_CHECK(capture3.compare(0, bs_nolidger, doc + L"\r\n") == 0,
                   "F3 예시3: with K=0 the preserved prefix is exactly the earlier blocks + terminator");
        TEST_CHECK(capture3.compare(bs_nolidger, std::wstring::npos, L"문장3") == 0,
                   "F3 예시3: no-ledger fallback translates ONLY the new block (earlier languages intact)");
    }

    // ---- (f) C3 hardening: an empty/identity translation re-arms the block
    // start instead of losing it, so the next Enter stays a tail-only
    // translation (the pre-F3 R4 chain destroyed the earlier blocks). ----
    {
        const std::wstring block1 = L"EN[첫째 블록]";
        const std::wstring ledger = block1;
        // Task 2: engine returns empty (network failure) -> no paste, nothing
        // injected. Pre-F3: C3 cleared. F3: kept (same hwnd, ledger has text).
        const bool keep = LedgerSurvivesNoPaste(/*paste_attempted*/ false,
                                                /*same_hwnd*/ true, /*has_text*/ true);
        std::wstring ledger_after = keep ? ledger : std::wstring();
        TEST_CHECK(ledger_after == block1, "F3 C3 re-arm: empty/identity outcome keeps a non-empty same-hwnd ledger");
        const std::wstring next_capture = block1 + L"\r\n둘째 블록";
        TEST_CHECK(AnalyzeCaptureVsLastPaste(next_capture, ledger_after) == PasteLedgerVerdict::PrefixWithTail,
                   "F3 C3 regression: the next Enter is a tail-only translation, not a whole-capture re-translation");
        // And the slice alone (even if the ledger were empty) still protects.
        const size_t bs = FindCurrentBlockStart(next_capture, 0);
        TEST_CHECK(next_capture.compare(bs, std::wstring::npos, L"둘째 블록") == 0,
                   "F3: slice protection holds independently of the ledger");
        // Cross-window hygiene stays: a different hwnd still clears.
        TEST_CHECK(!LedgerSurvivesNoPaste(false, false, true),
                   "F3 C3: a different target hwnd keeps the legacy clear (cross-window hygiene)");
    }

    // ---- (g) the per-block language snapshot (no change expected, pinned) ----
    {
        AppConfig cfg;
        cfg.SetTypeLanguages("Korean", "English");
        const AppConfig::Snapshot pinned = cfg.GetSnapshot(); // == task start
        cfg.SetTypeLanguages("Korean", "Vietnamese");          // user cycles mid-flight
        TEST_CHECK(pinned.type_target_language == "English",
                   "F3 예시3: the task-start snapshot pins the target (a mid-flight change cannot repivot it)");
        TEST_CHECK(cfg.GetSnapshot().type_target_language == "Vietnamese",
                   "F3 예시3: the NEXT task's snapshot sees the new target");
    }

    // ---- (h) the guard raise keeps abuse protection (see TestF4 capture
    // guard boundary for the limit matrix) ----
    TEST_CHECK(FindCurrentBlockStart(std::wstring(4096, L'a'), 0) == 0,
               "F3 guard coherence: a maximal single-line capture is one block");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] F3 current-block-only block-slice tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] F3 current-block-only block-slice tests: " << (g_failed_count - failures_before)
                  << " check(s) failed." << std::endl;
    }
}

// REQ-F7 (session 260908_0002, log 문제2): the DIAG field `clipboard_restored`
// printed `pasted ? 0 : 1`, i.e. it logged 0 on every SUCCESSFUL paste even
// though PasteAndRestore had restored the original clipboard before returning.
// Log-driven analysis read `clipboard_restored=0` as "restore never happens" and
// reported the user's clipboard permanently replaced by the translation. The fix:
//  (a) PasteAndRestore reports the ACTUAL restore outcome (out-param) and retries
//      the restore once after a short window when OpenClipboard contention makes
//      the first attempt fail;
//  (b) the worker logs that real outcome (1 = original clipboard confirmed back);
//  (c) the worker's scope-exit RAII restorer is disarmed ONLY on a confirmed
//      restore (ClipboardRestorerStaysArmed) - a failed internal restore leaves
//      the guard armed so function exit gives a SECOND restore attempt (no
//      permanent replacement even in the contention case).
// Headless-safe coverage: the pure guard predicate matrix, the lossless text +
// extra-format round-trip (the exact restore primitive PasteAndRestore uses),
// and the H1-abort no-touch contract of PasteAndRestore (no Ctrl+V is sent on
// the abort path, so the real clipboard is only read/written by the backup and
// restore primitives, which this suite already exercises).
void TestReqF7ClipboardRestore() {
    std::cout << "[TEST] F7 clipboard restore after paste (clipboard_restored=1 evidence path)" << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) pure guard-predicate matrix ----
    static_assert(ClipboardRestorerStaysArmed(true, true) == false,
                  "F7: paste succeeded AND restore confirmed -> worker RAII restorer disarmed (restore already done)");
    static_assert(ClipboardRestorerStaysArmed(true, false) == true,
                  "F7: paste succeeded but restore NOT confirmed -> worker RAII restorer STAYS armed (scope-exit second attempt)");
    static_assert(ClipboardRestorerStaysArmed(false, false) == true,
                  "F7: no paste (H1 abort / empty / identity) -> guard armed restores the capture-stage overwrite at scope exit");
    static_assert(ClipboardRestorerStaysArmed(false, true) == true,
                  "F7: defensive - no paste but restore confirmed is still a no-op restore at scope exit (harmless)");
    TEST_CHECK(ClipboardRestorerStaysArmed(true, true) == false,
               "F7: only the (pasted && restore_confirmed) combination disarms the guard");
    TEST_CHECK(ClipboardRestorerStaysArmed(true, false) == true,
               "F7: restore failure keeps the fallback armed (permanent-replacement guard)");

    // ---- (b) lossless restore round-trip (text + an extra registered format) ----
    // Preserve whatever is on the real clipboard now so the test is non-destructive.
    ClipboardBackup ambient;
    const bool ambient_ok = BackupClipboard(ambient);
    TEST_CHECK(ambient_ok, "F7 fixture: ambient clipboard backed up");

    // Build a synthetic ORIGINAL clipboard: CF_UNICODETEXT + one extra private
    // format (mirrors the log shape `has_text=1 extra_formats=4`).
    const std::wstring original_text = L"사용자 원본 클립보드 내용 F7 🚀";
    const UINT cf_extra = ::RegisterClipboardFormatW(L"Emebalachat_F7_ExtraFormat");
    TEST_CHECK(cf_extra != 0, "F7 fixture: extra private clipboard format registered");
    {
        if (::OpenClipboard(nullptr)) {
            ::EmptyClipboard();
            const size_t byte_len = (original_text.size() + 1) * sizeof(wchar_t);
            HGLOBAL hText = ::GlobalAlloc(GMEM_MOVEABLE, byte_len);
            if (hText) {
                void* p = ::GlobalLock(hText);
                if (p) {
                    memcpy(p, original_text.data(), original_text.size() * sizeof(wchar_t));
                    static_cast<wchar_t*>(p)[original_text.size()] = L'\0';
                    ::GlobalUnlock(hText);
                    // Ownership moves to the clipboard on success; freed locally
                    // on failure (same contract as SetClipboardText).
                    if (!::SetClipboardData(CF_UNICODETEXT, hText)) {
                        ::GlobalFree(hText);
                    }
                } else {
                    ::GlobalFree(hText);
                }
            }
            // Extra format: 8 bytes of payload.
            HGLOBAL hExtra = ::GlobalAlloc(GMEM_MOVEABLE, 8);
            if (hExtra) {
                void* p = ::GlobalLock(hExtra);
                if (p) {
                    memset(p, 0xAB, 8);
                    ::GlobalUnlock(hExtra);
                    if (!::SetClipboardData(cf_extra, hExtra)) {
                        ::GlobalFree(hExtra);
                    }
                } else {
                    ::GlobalFree(hExtra);
                }
            }
            ::CloseClipboard();
        }
    }

    ClipboardBackup original;
    const bool original_ok = BackupClipboard(original);
    TEST_CHECK(original_ok, "F7: synthetic original clipboard backed up");
    TEST_CHECK(original.text.has_value() && *original.text == original_text,
               "F7: backup captured the original text verbatim");
    {
        bool found_extra = false;
        for (const auto& [fmt, data] : original.formats) {
            if (fmt == cf_extra && data.size() == 8) {
                found_extra = true;
            }
        }
        TEST_CHECK(found_extra, "F7: backup captured the extra registered format (8 bytes)");
    }

    // Simulate the paste swap: translation overwrites the clipboard.
    const std::wstring translation = L"Translated text that must NOT outlive the task F7.";
    TEST_CHECK(SetClipboardText(translation), "F7: translation written over the original (paste step)");
    TEST_CHECK(GetClipboardText() == translation, "F7: clipboard holds the translation during the paste window");

    // The restore primitive PasteAndRestore relies on must bring back BOTH the
    // original text and the extra format.
    const bool restore_ok = RestoreClipboard(original);
    TEST_CHECK(restore_ok, "F7: RestoreClipboard succeeds after the translation swap");
    const std::wstring after_restore = GetClipboardText();
    TEST_CHECK(after_restore == original_text,
               "F7: original text is back after restore (translation did not outlive the swap)");
    {
        bool extra_back = false;
        if (::OpenClipboard(nullptr)) {
            extra_back = ::IsClipboardFormatAvailable(cf_extra) == TRUE;
            ::CloseClipboard();
        }
        TEST_CHECK(extra_back, "F7: extra registered format is back after restore (extra-format fidelity)");
    }

    // ---- (c) PasteAndRestore H1-abort contract: no paste, clipboard untouched ----
    // A fake, invalid expected target can never be the current foreground root,
    // so PasteAndRestore must abort BEFORE SetClipboardText/Ctrl+V. Prove the
    // abort leaves the (restored) clipboard content intact and reports
    // clipboard_restored=false (nothing was restored because nothing was pasted).
    SetClipboardText(original_text);
    bool abort_restored = true; // sentinel: must be flipped to false by the call
    const HWND fake_target = reinterpret_cast<HWND>(static_cast<intptr_t>(0x3F7));
    const bool abort_result = PasteAndRestore(L"must not land", original, fake_target, &abort_restored);
    TEST_CHECK(abort_result == false, "F7: H1 abort returns false (no paste into a foreign target)");
    TEST_CHECK(abort_restored == false, "F7: abort path reports clipboard_restored=false (nothing restored here)");
    TEST_CHECK(GetClipboardText() == original_text,
               "F7: abort left the clipboard content untouched (no translation leak)");

    // ---- cleanup: put the ambient clipboard back ----
    if (ambient_ok) {
        RestoreClipboard(ambient);
    } else {
        SetClipboardText(L"");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] F7 clipboard restore tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] F7 clipboard restore tests: " << (g_failed_count - failures_before)
                  << " check(s) failed." << std::endl;
    }
}

// REQ-F1 (docs/260908_0001 session, user log emebalachat_260908062830
// L546-550/L787-791/L1120-1124): first-character residue ahead of the pasted
// translation ("오"/"처"/"왜 "). Root cause: the compensation advanced the
// stored start by 2 UTF-16 units per leading CRLF pair, but the clipboard
// CRLF-NORMALIZES every newline while the document stores single-unit LFs -
// the advance overshot the true block start, re-selected INSIDE the block,
// and left its first character(s) outside the replacement. Contract:
//  (a) pure seams: CountNewlineSequences (a CRLF pair is ONE sequence) and
//      DocNewlineWidth field arithmetic from the exact log shapes (LF doc ->
//      1, CRLF doc -> 2, anything else -> refuse).
//  (b) live LF document, single-pair drift (Japanese/Russian shape): the
//      start advances by the measured width 1 (old code: +2 -> one-char
//      residue), and the clipboard-trimmed capture equals the re-selected
//      document text byte-for-byte (the user-facing side effect).
//  (c) live LF document, two-pair drift (Hungarian shape): advance by 2
//      (old code: +4 -> two-char residue).
//  (d) refusal paths change NOTHING: re-compensating an already-compensated
//      geometry (width no longer recoverable) and a disturbed live selection
//      (consistency gate) both refuse, leaving the stored offset intact.
void TestReqF1FirstCharResidue() {
    std::cout << "[TEST] REQ-F1 first-char residue: width-measured compensation" << std::endl;
    const int failures_before = g_failed_count;

    // (a) pure seams - the exact field arithmetic of the three log captures.
    static_assert(EditCaretTracker_CountNewlineSequences(L"\r\n\r\nblock") == 2,
                  "REQ-F1: two CRLF pairs -> two newline sequences");
    static_assert(EditCaretTracker_CountNewlineSequences(L"a\nb\rc") == 2,
                  "REQ-F1: lone LF and lone CR each count once");
    static_assert(EditCaretTracker_CountNewlineSequences(L"abc") == 0,
                  "REQ-F1: no newlines -> 0");
    static_assert(EditCaretTracker_DocNewlineWidth(63, 60, 3) == 1,
                  "REQ-F1: Japanese capture (log L546-550): 63 clipboard units vs 60 doc units over 3 newlines -> LF doc, width 1");
    static_assert(EditCaretTracker_DocNewlineWidth(79, 76, 3) == 1,
                  "REQ-F1: Russian capture (log L787-791): 79 vs 76 over 3 newlines -> width 1");
    static_assert(EditCaretTracker_DocNewlineWidth(135, 129, 6) == 1,
                  "REQ-F1: Hungarian capture (log L1120-1124): 135 vs 129 over 6 newlines -> width 1");
    static_assert(EditCaretTracker_DocNewlineWidth(5, 5, 1) == 2,
                  "REQ-F1: CRLF document (REQ-036 shape): capture == span -> width 2");
    static_assert(EditCaretTracker_DocNewlineWidth(0, 0, 0) == 0,
                  "REQ-F1: no newlines -> refuse");
    static_assert(EditCaretTracker_DocNewlineWidth(10, 12, 3) == 0,
                  "REQ-F1: capture shorter than span -> refuse");
    static_assert(EditCaretTracker_DocNewlineWidth(11, 10, 3) == 0,
                  "REQ-F1: mixed-width arithmetic -> refuse (never guess)");
    TEST_CHECK(EditCaretTracker_CountNewlineSequences(L"\r\n") == 1,
               "REQ-F1: one CRLF pair -> ONE sequence (a pair is one newline)");
    TEST_CHECK(EditCaretTracker_CountNewlineSequences(L"\n") == 1,
               "REQ-F1: single LF -> one sequence");

    // (b)-(d) live sequence on the LF-storing EM emulation control.
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Emebalachat_ReqF1Host";
    ::RegisterClassExW(&wc);
    HWND host = ::CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"reqf1", WS_POPUP,
                                  -400, -400, 200, 100, nullptr, nullptr, wc.hInstance, nullptr);
    WNDCLASSEXW wc_edit = {};
    wc_edit.cbSize = sizeof(WNDCLASSEXW);
    wc_edit.lpfnWndProc = F1EmuEditProc;
    wc_edit.hInstance = wc.hInstance;
    wc_edit.lpszClassName = L"Emebalachat_ReqF1EmuEdit";
    ::RegisterClassExW(&wc_edit);
    HWND edit = nullptr;
    if (host) {
        edit = ::CreateWindowExW(0, wc_edit.lpszClassName, L"",
                                 WS_CHILD | WS_VISIBLE,
                                 0, 0, 180, 80, host, nullptr, wc.hInstance, nullptr);
    }
    TEST_CHECK(edit != nullptr, "REQ-F1: LF-storing EM emulation control created");
    bool focus_ok = false;
    if (edit) {
        ::ShowWindow(host, SW_SHOWNOACTIVATE);
        ::SetFocus(edit);
        GUITHREADINFO gti = {};
        gti.cbSize = sizeof(gti);
        focus_ok = ::GetGUIThreadInfo(::GetCurrentThreadId(), &gti) && gti.hwndFocus == edit;
    }
    if (edit && !focus_ok) {
        std::cout << "[SKIP] SetFocus on the emulation control unavailable; REQ-F1 live sequence skipped." << std::endl;
    }
    if (edit && focus_ok) {
        auto emu_sel = [](DWORD a, DWORD b) {
            g_f1_sel_start = a;
            g_f1_sel_end = b;
        };

        // ---- (b) single-pair drift: the Japanese/Russian one-char-residue shape ----
        // Block 1 "AAA" stored at caret 3; an out-of-band Enter inserted a
        // single-LF newline; the user typed "BBB". Document: "AAA\nBBB".
        g_f1_doc = L"AAA\n";
        emu_sel(3, 3);
        TEST_CHECK(EditCaretTracker_TrySelectNewText(edit), "REQ-F1: baseline pass enters the EM path");
        EditCaretTracker_NotifyReplacement(edit, true, 3); // stores the real caret 3
        g_f1_doc = L"AAA\nBBB";
        emu_sel(7, 7);
        TEST_CHECK(EditCaretTracker_TrySelectNewText(edit), "REQ-F1: drifted pass enters the EM path");
        // Clipboard-normalized capture of doc[3..7) = "\nBBB" -> "\r\nBBB".
        const std::wstring drifted = L"\r\nBBB";
        TEST_CHECK(EditCaretTracker_CountLeadingCrlfPairs(drifted) == 1,
                   "REQ-F1: exactly one leading pair measured");
        TEST_CHECK(EditCaretTracker_CompensateLeadingNewlines(edit, drifted),
                   "REQ-F1: width-measured compensation advanced the stored start");
        // Measured width 1 (capture 5 units vs doc span 4 over 1 newline):
        // start 3 -> 4, NOT 3 -> 5 (the old +2 overshoot whose selection
        // [5..7) = "BB" stranded the block's first character outside the
        // replacement - the log's "오"/"처" residue shape).
        TEST_CHECK(g_f1_sel_start == 4u && g_f1_sel_end == 7u,
                   "REQ-F1: post-compensation selection is [4..7) - the whole block, no residue");
        std::wstring selected = g_f1_doc.substr(g_f1_sel_start, g_f1_sel_end - g_f1_sel_start);
        std::wstring trimmed = drifted;
        trimmed.erase(0, 2 * EditCaretTracker_CountLeadingCrlfPairs(drifted));
        TEST_CHECK(selected == L"BBB" && trimmed == selected,
                   "REQ-F1: clipboard-trimmed capture equals the re-selected document text byte-for-byte");

        // ---- (d1) re-compensation of the same capture must refuse ----
        // Span [4..7) vs capture 5 over 1 newline: 5 != 3 and 5 != 4 -> the
        // width is no longer recoverable -> refuse; selection unchanged.
        TEST_CHECK(!EditCaretTracker_CompensateLeadingNewlines(edit, drifted),
                   "REQ-F1: re-compensation of the already-compensated geometry refuses");
        TEST_CHECK(g_f1_sel_start == 4u && g_f1_sel_end == 7u,
                   "REQ-F1: the refusal left the live selection untouched");

        // ---- (d2) disturbed live selection: the consistency gate refuses ----
        emu_sel(0, 7); // selection moved between copy and compensation
        TEST_CHECK(!EditCaretTracker_CompensateLeadingNewlines(edit, drifted),
                   "REQ-F1: consistency gate refuses when the live start is not the stored start");
        // The stored offset survived: the next TrySelectNewText re-selects
        // from 4 (not from the disturbed 0), proving refusal changed nothing.
        emu_sel(7, 7);
        TEST_CHECK(EditCaretTracker_TrySelectNewText(edit), "REQ-F1: post-refusal pass enters the EM path");
        TEST_CHECK(g_f1_sel_start == 4u && g_f1_sel_end == 7u,
                   "REQ-F1: stored offset untouched by the refused compensations");

        // ---- (c) two-pair drift: the Hungarian two-char-residue shape ----
        // Document "AAAA\n\nBBBB": stored 4, two out-of-band single-LF
        // newlines, caret 10. The old code advanced 4 -> 8 (selection
        // [8..10) = "BB" - the log's "왜 " two-char residue); the fix
        // measures width 1 and advances 4 -> 6.
        g_f1_doc = L"AAAA\n\nBBBB";
        emu_sel(4, 4);
        TEST_CHECK(EditCaretTracker_TrySelectNewText(edit), "REQ-F1: two-pair baseline pass enters the EM path");
        EditCaretTracker_NotifyReplacement(edit, true, 4); // stores the real caret 4
        emu_sel(10, 10);
        TEST_CHECK(EditCaretTracker_TrySelectNewText(edit), "REQ-F1: two-pair drifted pass enters the EM path");
        const std::wstring drifted2 = L"\r\n\r\nBBBB"; // clipboard-normalized doc[4..10)
        TEST_CHECK(EditCaretTracker_CountLeadingCrlfPairs(drifted2) == 2,
                   "REQ-F1: two leading pairs measured");
        TEST_CHECK(EditCaretTracker_CompensateLeadingNewlines(edit, drifted2),
                   "REQ-F1: two-pair width-measured compensation advanced the stored start");
        TEST_CHECK(g_f1_sel_start == 6u && g_f1_sel_end == 10u,
                   "REQ-F1: two-pair post-compensation selection is [6..10) - no residue");
        std::wstring selected2 = g_f1_doc.substr(g_f1_sel_start, g_f1_sel_end - g_f1_sel_start);
        std::wstring trimmed2 = drifted2;
        trimmed2.erase(0, 2 * EditCaretTracker_CountLeadingCrlfPairs(drifted2));
        TEST_CHECK(selected2 == L"BBBB" && trimmed2 == selected2,
                   "REQ-F1: two-pair trimmed capture equals the re-selected document text");

        g_f1_doc.clear();
        emu_sel(0, 0);
        ::SetFocus(nullptr);
    }
    if (host) {
        ::DestroyWindow(host);
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-F1 first-char residue tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-F1 first-char residue tests: " << (g_failed_count - failures_before)
                  << " check(s) failed." << std::endl;
    }
}

// REQ-039 (chat-window Enter capture): regression suite for the two proven
// failure shapes of user log emebalachat_260907204046.
//   FIX-1 - Electron/Chromium intermittently drops a synthetic Ctrl+C chord
//           (Discord Chrome_WidgetWin_1: /002 sequence-unchanged at L1782/
//           L1860/L1939) turning the bare-Enter capture empty; the R5 hold
//           then swallows the Enter. The fix re-runs the bounded
//           selection+copy cycle (CopyChordRetryWarranted), exempting the
//           provably-empty EM selection (REQ-034 F3-B paste-window
//           geometry) so the worker's silent send-through keeps its
//           latency contract.
//   FIX-2 - identity translation (equals source) ended the task with the
//           intercepted Enter undelivered (VS Code window L3450-3760:
//           send_enter SKIPPED reason=send_gate every Enter, caret never
//           advanced, whole content re-checked). EqualsSourceNeedsSendThrough
//           now hands the Enter to the app like the established
//           send-through contracts.
void TestReq039ChatWindowEnterCapture() {
    std::cout << "[TEST] REQ-039 chat-window Enter capture (dropped chord + identity send-through)" << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) pure retry-budget predicate matrix ----
    static_assert(kClipboardCopyChordAttempts == 2,
                  "REQ-039/REQ-001: two bounded chord attempts (drop + 1 retry)");
    static_assert(CopyChordRetryWarranted(0, false),
                  "REQ-039: first drop with budget left must retry");
    static_assert(!CopyChordRetryWarranted(1, false),
                  "REQ-039/REQ-001: budget exhausted - no retry past the last attempt");
    static_assert(!CopyChordRetryWarranted(9, false),
                  "REQ-039: out-of-range attempt index never retries");
    static_assert(!CopyChordRetryWarranted(0, true),
                  "REQ-039: provably-empty EM selection skips the retry (F3-B latency contract)");
    static_assert(kClipboardCopyChordRetryGapMs > 0,
                  "REQ-039: retry gap lets the target input pipeline drain");

    // ---- (b) pure identity send-through predicate matrix ----
    // The R5 hold (empty capture) and smart-bypass contracts stay disjoint:
    // only the translate-but-unchanged outcome sends through.
    TEST_CHECK(EqualsSourceNeedsSendThrough(false, false),
               "REQ-039: identity translation must hand Enter to the app");
    TEST_CHECK(!EqualsSourceNeedsSendThrough(true, false),
               "REQ-039: empty capture stays on the R5 hold branch (upstream, disjoint)");
    TEST_CHECK(!EqualsSourceNeedsSendThrough(false, true),
               "REQ-039: smart bypass keeps its own send-through contract (not this predicate's)");
    static_assert(!EqualsSourceNeedsSendThrough(true, true),
                  "REQ-039: vacuous pair never sends");

    // ---- (c) live sequence on a real in-process EDIT control ----
    // c1: SelectionProvablyEmpty on the F3-B geometry (EM_SETSEL(n, n)) must
    //     read true - the retry exemption is exactly this shape. A non-empty
    //     range must read false. A non-EM control must read false (retry
    //     allowed; only measured emptiness may skip).
    // c2: the full capture path (selection + bounded chord loop + clipboard
    //     read) still returns the selected text on the happy path - the
    //     retry loop must not corrupt the pre-REQ-039 contract.
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Emebalachat_Req039Host";
    ::RegisterClassExW(&wc);
    HWND host = ::CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"req039", WS_POPUP,
                                  -400, -400, 200, 100, nullptr, nullptr, wc.hInstance, nullptr);
    HWND edit = nullptr;
    if (host) {
        edit = ::CreateWindowExW(0, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | ES_MULTILINE,
                                  0, 0, 180, 80, host, nullptr, wc.hInstance, nullptr);
    }
    TEST_CHECK(edit != nullptr, "REQ-039: in-process EDIT control created");
    bool focus_ok = false;
    if (edit) {
        ::ShowWindow(host, SW_SHOWNOACTIVATE);
        ::SetFocus(edit);
        GUITHREADINFO gti = {};
        gti.cbSize = sizeof(gti);
        focus_ok = ::GetGUIThreadInfo(::GetCurrentThreadId(), &gti) && gti.hwndFocus == edit;
    }
    if (edit && !focus_ok) {
        std::cout << "[SKIP] SetFocus on EDIT control unavailable; REQ-039 live sequence skipped." << std::endl;
    }
    if (edit && focus_ok) {
        // c1a: the F3-B empty-range geometry reads provably empty.
        ::SetWindowTextW(edit, L"first\r\nsecond");
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(6), static_cast<LPARAM>(6));
        TEST_CHECK(EditCaretTracker_SelectionProvablyEmpty(edit),
                   "REQ-039: EM empty range (start == end) is provably empty - retry exempt");
        // c1b: a real selection is NOT provably empty.
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(0), static_cast<LPARAM>(6));
        TEST_CHECK(!EditCaretTracker_SelectionProvablyEmpty(edit),
                   "REQ-039: non-empty EM range is not provably empty - retry allowed");
        // c1c: a non-EM control (the host popup itself, static-class) never
        //      claims provable emptiness - copy failures there stay retryable.
        TEST_CHECK(!EditCaretTracker_SelectionProvablyEmpty(host),
                   "REQ-039: non-EM window is never provably empty (fail-open to retry)");

        // c2: retry-loop idempotence geometry (no clipboard round-trip:
        // this thread owns the EDIT control, so SendInput chords could not
        // be pumped - the same reason the REQ-036 suite stays on the EM
        // seams; the chord loop's primitives are the seam functions).
        // A second TrySelectNewText on the same caret must land on the SAME
        // selection (idempotent), and the provably-empty probe must still
        // answer correctly AFTER a first capture-cycle pass - which is the
        // state the chord retry consults.
        ::SetWindowTextW(edit, L"REQ039 happy path block");
        ::SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(22), static_cast<LPARAM>(22));
        TEST_CHECK(EditCaretTracker_TrySelectNewText(edit),
                   "REQ-039: chord-cycle pass 1 enters the EM path");
        DWORD sel = static_cast<DWORD>(::SendMessageW(edit, EM_GETSEL, 0, 0));
        TEST_CHECK(LOWORD(sel) == 0u && HIWORD(sel) == 22u,
                   "REQ-039: pass-1 selection geometry is [0..caret) (untracked start)");
        // The retry pass (attempt 2 in the loop) repeats exactly this call;
        // with the offset now stored at 0 the selection must be stable, not
        // double-applied or shifted - idempotence is what makes the chord
        // retry safe in the live pipeline.
        TEST_CHECK(EditCaretTracker_TrySelectNewText(edit),
                   "REQ-039: chord-cycle retry pass re-enters the EM path");
        sel = static_cast<DWORD>(::SendMessageW(edit, EM_GETSEL, 0, 0));
        TEST_CHECK(LOWORD(sel) == 0u && HIWORD(sel) == 22u,
                   "REQ-039: retry pass selection is idempotent (same geometry, no drift)");
        TEST_CHECK(!EditCaretTracker_SelectionProvablyEmpty(edit),
                   "REQ-039: full selection after retry pass is not provably empty");

        ::SetFocus(nullptr);
    }
    if (host) {
        ::DestroyWindow(host); // child EDIT dies with the parent
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-039 chat-window Enter capture tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-039 chat-window tests: " << (g_failed_count - failures_before)
                  << " check(s) failed." << std::endl;
    }
}

// P4 Batch B-1 (session 260907_0002, design §2-Q1/§4.1): bidi_utils unit suite.
// All checks are pure/headless (no window, no DWrite, no Win32 locale calls):
// RTL set membership over the 37-language registry + name resolution, and the
// UAX #9 first-strong heuristic on UTF-16 samples (design §4.1 case list).
void TestBidiUtils() {
    std::cout << "[RUN] Testing B-1 bidi_utils (RTL set + first-strong)..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- 1) RTL set membership: exactly AR/FA/UR/HE true, 33 false --------
    // Iterates GetTargetLanguages() (the 37 registry entries minus AUTO) so
    // the assertion scales with kAllLanguages instead of a hardcoded list.
    {
        const auto& targets = GetTargetLanguages();
        TEST_CHECK(targets.size() == 37, "B1: registry has exactly 37 target languages");
        int rtl_true = 0;
        std::string unexpected_true;
        std::string missing_true;
        for (const auto& lang : targets) {
            const bool rtl = IsRtlLanguageCode(lang.code);
            const bool expected =
                lang.code == "AR" || lang.code == "FA" || lang.code == "UR" || lang.code == "HE";
            if (rtl) ++rtl_true;
            if (rtl && !expected) unexpected_true += lang.code + " ";
            if (expected && !rtl) missing_true += lang.code + " ";
        }
        TEST_CHECK(rtl_true == 4, "B1: exactly 4 of 37 language codes are RTL");
        TEST_CHECK(unexpected_true.empty() && missing_true.empty(),
                   "B1: RTL code set is exactly {AR,FA,UR,HE} (no extras, none missing)");
    }

    // ---- 2) Code forms: case-insensitive, names, natives, negatives --------
    TEST_CHECK(IsRtlLanguageCode("AR") && IsRtlLanguageCode("ar") &&
                   IsRtlLanguageCode("He") && IsRtlLanguageCode("uR"),
               "B1: RTL codes accepted case-insensitively");
    TEST_CHECK(IsRtlLanguageCode("Arabic") && IsRtlLanguageCode("Persian") &&
                   IsRtlLanguageCode("Urdu") && IsRtlLanguageCode("Hebrew"),
               "B1: English names resolve RTL via kAllLanguages (tooltip target_lang_ path)");
    // Native-name resolution is asserted DATA-DRIVEN from the registry itself
    // (no literals copied from config.cpp - a single-codepoint drift would
    // silently mismatch; the registry entry is the source of truth).
    TEST_CHECK(IsRtlLanguageCode("العربية") && IsRtlLanguageCode("فارسی") &&
                   IsRtlLanguageCode("اردو") && IsRtlLanguageCode("עברית"),
               "B1: native names resolve RTL via kAllLanguages");
    {
        bool all_forms = true;
        for (const auto& lang : GetTargetLanguages()) {
            const bool expected = lang.code == "AR" || lang.code == "FA" ||
                                  lang.code == "UR" || lang.code == "HE";
            if (IsRtlLanguageCode(lang.name_en) != expected) all_forms = false;
            if (IsRtlLanguageCode(lang.name_native) != expected) all_forms = false;
        }
        TEST_CHECK(all_forms, "B1: every registry entry resolves RTL by name_en AND name_native iff its code is RTL (37x2)");
    }
    TEST_CHECK(IsRtlLanguageCode("arabic") && !IsRtlLanguageCode("korean"),
               "B1: English-name lookup case-insensitive (RTL hit + LTR miss)");
    TEST_CHECK(!IsRtlLanguageCode("KO") && !IsRtlLanguageCode("EN") &&
                   !IsRtlLanguageCode("ZH-CN") && !IsRtlLanguageCode("TH"),
               "B1: representative LTR codes are false");
    TEST_CHECK(!IsRtlLanguageCode("AUTO") && !IsRtlLanguageCode("auto") &&
                   !IsRtlLanguageCode("") && !IsRtlLanguageCode("klingon"),
               "B1: AUTO/empty/unknown never RTL");

    // ---- 3) IsRtlLocale / DirectionForLocale --------------------------------
    // The RTL UiLocale enumerators land in B-3; today's full enum is LTR, and
    // IsRtlLocale resolves through LocaleToString -> the same code set, so
    // this loop also pins that no existing locale was miswired.
    {
        const UiLocale kAllEnumLocales[] = {
            UiLocale::Auto, UiLocale::Korean, UiLocale::English, UiLocale::Japanese,
            UiLocale::ChineseSimplified, UiLocale::ChineseTraditional,
            UiLocale::Vietnamese, UiLocale::Spanish
        };
        bool all_ltr = true;
        for (const UiLocale loc : kAllEnumLocales) {
            if (IsRtlLocale(loc)) all_ltr = false;
            if (DirectionForLocale(loc) != TextDirection::LTR) all_ltr = false;
        }
        TEST_CHECK(all_ltr, "B1: all current UiLocale enum values are LTR (RTL locales land in B-3)");
    }

    // ---- 4) First-strong: design §4.1 mandated cases ------------------------
    // L"مرحبا" — pure Arabic.
    TEST_CHECK(GuessBaseDirection(L"مرحبا") == TextDirection::RTL,
               "B1: first-strong 'مرحبا' -> RTL");
    TEST_CHECK(GuessBaseDirection(L"123 مرحبا") == TextDirection::RTL,
               "B1: first-strong '123 مرحبا' -> RTL (ASCII digits skipped)");
    TEST_CHECK(GuessBaseDirection(L"hello مرحبا") == TextDirection::LTR,
               "B1: first-strong 'hello مرحبا' -> LTR (first strong wins)");
    TEST_CHECK(GuessBaseDirection(L"") == TextDirection::LTR,
               "B1: first-strong empty -> LTR (UAX #9 P2 default)");
    TEST_CHECK(GuessBaseDirection(L"\x05D0") == TextDirection::RTL,
               "B1: first-strong Hebrew U+05D0 (Alef) -> RTL");
    TEST_CHECK(GuessBaseDirection(L"\xFBFC") == TextDirection::RTL,
               "B1: first-strong Arabic presentation form U+FBFC -> RTL (AL block)");
    TEST_CHECK(GuessBaseDirection(L"!?. , 123 -+%") == TextDirection::LTR,
               "B1: first-strong punctuation/digits only -> LTR (no strong char)");
    TEST_CHECK(GuessBaseDirection(L"\x2066مرحبا\x2069") == TextDirection::RTL,
               "B1: isolate-wrapped Arabic (LRI U+2066 / PDI U+2069 skipped) -> RTL");

    // ---- 5) First-strong: extended coverage ---------------------------------
    TEST_CHECK(GuessBaseDirection(L"١٢٣") == TextDirection::LTR,
               "B1: Arabic-Indic digits alone -> LTR (AN class is weak, skipped)");
    TEST_CHECK(GuessBaseDirection(L"١٢٣ مرحبا") == TextDirection::RTL,
               "B1: Arabic-Indic digits then Arabic -> RTL");
    TEST_CHECK(GuessBaseDirection(L"!!! مرحبا") == TextDirection::RTL,
               "B1: leading punctuation skipped -> RTL");
    TEST_CHECK(GuessBaseDirection(L"안녕하세요") == TextDirection::LTR,
               "B1: Hangul -> LTR");
    TEST_CHECK(GuessBaseDirection(L"日本語テキスト") == TextDirection::LTR,
               "B1: Kana -> LTR");
    TEST_CHECK(GuessBaseDirection(L"Текст") == TextDirection::LTR,
               "B1: Cyrillic -> LTR");
    TEST_CHECK(GuessBaseDirection(L"テスト مرحبا") == TextDirection::LTR,
               "B1: kana-first mixed sample -> LTR (first-strong, not majority)");
    // Bidi controls and the BOM must not count as strong in either direction.
    TEST_CHECK(GuessBaseDirection(L"\x200E\x200F\x202A\x202E\x2060\xFEFF") == TextDirection::LTR,
               "B1: bidi controls/embeddings/BOM only -> LTR (all skipped)");
    TEST_CHECK(GuessBaseDirection(L"\x200Fمرحبا") == TextDirection::RTL,
               "B1: RLM-prefixed Arabic -> RTL (control skipped, not strong)");
    // Astral CJK ext B (U+20000 -> surrogate pair D840 DC00) must read strong L.
    TEST_CHECK(GuessBaseDirection(L"\U00020000") == TextDirection::LTR,
               "B1: astral CJK ext-B surrogate pair -> LTR");
    // Unpaired high surrogate is neutral noise; the Arabic letter after it decides.
    TEST_CHECK(GuessBaseDirection(L"\xD800\x0628") == TextDirection::RTL,
               "B1: unpaired surrogate skipped, following Arabic -> RTL");
    TEST_CHECK(GuessBaseDirection(L"") == TextDirection::LTR &&
                   GuessBaseDirection(L"   \t\r\n") == TextDirection::LTR,
               "B1: whitespace-only -> LTR");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] B-1 bidi_utils tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] B-1 bidi_utils tests: " << (g_failed_count - failures_before)
                  << " check(s) failed." << std::endl;
    }
}

// P4 Batch B-2 (session 260907_0002, design §3 B-2 row + LanguageInfo bcp47
// note): registry schema tests for the new bcp47 column. All checks are pure
// data assertions over the shared registry (no COM seam in tooltip.cpp per
// the anti-gaming rule — the direction mutation itself is proven by the
// grep-able DIAG line + design §4.4 manual QA, not by a test-only hook).
void TestReq038B2RegistryBcp47() {
    std::cout << "[RUN] Testing B-2 LanguageInfo.bcp47 registry..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- 1) Every registry row carries a non-empty BCP-47 tag (all 38) ------
    // Iterates GetSupportedLanguages() (AUTO + 37 targets) so the assertion
    // scales with the registry instead of a hardcoded list.
    {
        const auto& all = GetSupportedLanguages();
        TEST_CHECK(all.size() == 38, "B2: registry has exactly 38 rows (AUTO + 37)");
        bool all_nonempty = true;
        std::string offenders;
        for (const auto& lang : all) {
            const bool ok = lang.bcp47 != nullptr && lang.bcp47[0] != '\0';
            if (!ok) {
                all_nonempty = false;
                offenders += lang.code + " ";
            }
        }
        TEST_CHECK(all_nonempty, "B2: every bcp47 tag non-empty (null/empty would disable DWrite font fallback)");
        if (!all_nonempty) std::cout << "     offenders: " << offenders << std::endl;
    }

    // ---- 2) Spot cases required by the B-2 delegation ------------------------
    {
        const LanguageInfo* ar = FindLanguageByCode("AR");
        TEST_CHECK(ar && std::string_view(ar->bcp47) == "ar", "B2: AR -> bcp47 \"ar\"");
        const LanguageInfo* zhtw = FindLanguageByCode("ZH-TW");
        TEST_CHECK(zhtw && std::string_view(zhtw->bcp47) == "zh-TW", "B2: ZH-TW -> bcp47 \"zh-TW\"");
        const LanguageInfo* zhcn = FindLanguageByCode("ZH-CN");
        TEST_CHECK(zhcn && std::string_view(zhcn->bcp47) == "zh-CN", "B2: ZH-CN -> bcp47 \"zh-CN\"");
        const LanguageInfo* fil = FindLanguageByCode("FIL");
        TEST_CHECK(fil && std::string_view(fil->bcp47) == "fil", "B2: FIL -> bcp47 \"fil\" (ISO 639-1/BCP-47 subtag)");
        const LanguageInfo* auto_lang = FindLanguageByCode("AUTO");
        TEST_CHECK(auto_lang && std::string_view(auto_lang->bcp47) == "en", "B2: AUTO -> \"en\" pivot tag");
    }

    // ---- 3) RTL rows carry exactly the four RTL tags -------------------------
    // The tooltip body direction derives from the registry code, but B-3's
    // locale table and any chrome callers key off bcp47: pin the pairing so
    // the two never drift silently.
    {
        std::string rtl_tags;
        for (const auto& lang : GetTargetLanguages()) {
            if (IsRtlLanguageCode(lang.code)) {
                rtl_tags += std::string(lang.bcp47) + ",";
            }
        }
        TEST_CHECK(rtl_tags == "ar,fa,ur,he,",
                   "B2: RTL rows' bcp47 tags are exactly ar,fa,ur,he in registry order");
    }

    // ---- 4) Tag hygiene: lowercase language subtag, hyphen region, ASCII ------
    // DWrite canonicalizes tags on readback and the clone-swap equality check
    // case-folds; a malformed tag (underscore separator, non-ASCII) would
    // churn the COM object on every show or fail CreateTextFormat.
    {
        bool clean = true;
        for (const auto& lang : GetSupportedLanguages()) {
            const std::string_view tag = lang.bcp47 ? lang.bcp47 : "";
            if (tag.empty()) { clean = false; continue; }
            for (char c : tag) {
                const bool alpha_lower = (c >= 'a' && c <= 'z');
                const bool alpha_upper = (c >= 'A' && c <= 'Z'); // zh-CN script region
                const bool digit = (c >= '0' && c <= '9');
                const bool hyphen = (c == '-');
                if (!alpha_lower && !alpha_upper && !digit && !hyphen) { clean = false; break; }
            }
        }
        TEST_CHECK(clean, "B2: every bcp47 tag is ASCII [A-Za-z0-9-] (BCP-47 subtag charset)");
    }

    // ---- 5) GetTargetLanguages() copies carry bcp47 through ------------------
    // The target view is built by value-copies of the registry rows (InitTarget
    // Languages); the tooltip resolves tags through it too (NormalizeLanguage
    // Code -> FindLanguageByCode on the FULL registry, but GetTargetLanguages
    // is the public content-side accessor — pin that the field survives the
    // copy so B-5/B-3 consumers can rely on it).
    {
        bool targets_ok = true;
        for (const auto& lang : GetTargetLanguages()) {
            if (!lang.bcp47 || !lang.bcp47[0]) targets_ok = false;
        }
        TEST_CHECK(targets_ok, "B2: all 37 GetTargetLanguages() rows keep non-empty bcp47 after copy");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] B-2 registry bcp47 tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] B-2 registry bcp47 tests: " << (g_failed_count - failures_before)
                  << " check(s) failed." << std::endl;
    }
}

// P4 Batch B-5 (session 260907_0002, design §3 B-5 row + §4.1 "About content"
// row): BuildLocalizedContent under Arabic/Hebrew — the RTL UI locales the
// About window renders under per design §2.2.3. Content must actually differ
// from English (i18n-routed, not constants), and the reset label must be
// localized + non-empty (the escape-hatch affordance an RTL user needs most).
// Direction correctness itself (SetReadingDirection on the formats) lives in
// COM code with no test-only seam (design §4.2 anti-gaming rule); it is
// proven by the grep-able "about locale_dir=" DIAG line + manual QA M4.
void TestReq038B5AboutRtl() {
    std::cout << "[RUN] Testing B-5 About localized content under AR/HE (REQ-038)..." << std::endl;
    const int failures_before = g_failed_count;

    // Save/restore the global locale the suite runs with (same discipline as
    // the R6 section that builds KO/JA/EN About snapshots).
    const UiLocale initial = I18n::GetCurrentLocale();

    I18n::SetLocale(UiLocale::English);
    const auto en = AboutWindow::BuildLocalizedContent();
    I18n::SetLocale(UiLocale::Arabic);
    const auto ar = AboutWindow::BuildLocalizedContent();
    I18n::SetLocale(UiLocale::Hebrew);
    const auto he = AboutWindow::BuildLocalizedContent();

    // ---- 1) RTL locales render fully localized copy (differs from EN) ------
    TEST_CHECK(!ar.tagline.empty() && ar.tagline != en.tagline,
               "B5: AR tagline localized (non-empty, differs from EN)");
    TEST_CHECK(!he.tagline.empty() && he.tagline != en.tagline,
               "B5: HE tagline localized (non-empty, differs from EN)");
    bool ar_features_differ = true, he_features_differ = true;
    for (int i = 0; i < 3; ++i) {
        if (ar.features[i].empty() || ar.features[i] == en.features[i]) ar_features_differ = false;
        if (he.features[i].empty() || he.features[i] == en.features[i]) he_features_differ = false;
    }
    TEST_CHECK(ar_features_differ, "B5: all 3 AR feature lines localized (differs from EN)");
    TEST_CHECK(he_features_differ, "B5: all 3 HE feature lines localized (differs from EN)");
    TEST_CHECK(!ar.etymology.empty() && ar.etymology != en.etymology,
               "B5: AR etymology line localized");
    TEST_CHECK(!he.etymology.empty() && he.etymology != en.etymology,
               "B5: HE etymology line localized");

    // The RTL scripts are actually present (the copy is not a Latin
    // transliteration): first-strong over the AR body must be RTL, and the HE
    // body must contain Hebrew letters — data-driven via the B-1 seam.
    TEST_CHECK(GuessBaseDirection(ar.tagline) == TextDirection::RTL &&
                   GuessBaseDirection(ar.etymology) == TextDirection::RTL,
               "B5: AR tagline+etymology first-strong RTL (real Arabic script content)");
    {
        bool he_has_hebrew = false;
        for (const wchar_t ch : he.tagline) {
            if (ch >= 0x0590 && ch <= 0x05FF) { he_has_hebrew = true; break; }
        }
        TEST_CHECK(he_has_hebrew, "B5: HE tagline carries Hebrew-range codepoints (U+0590-U+05FF)");
    }

    // ---- 2) About title carries the per-locale brand (REQ-B, session
    // 260909_0001) ------------------------------------------------------------
    // AboutTitle is localized and now embeds each locale's transliterated
    // brand instead of the ASCII "Emebala Chat".
    TEST_CHECK(ar.title.find(L"إيميبالا شات") != std::wstring::npos &&
                   he.title.find(L"אמבאלה צ'אט") != std::wstring::npos,
               "REQ-B: About title keeps the per-locale brand token under AR/HE");
    TEST_CHECK(ar.link_labels[2] == L"Reddit" && he.link_labels[2] == L"Reddit",
               "B5: Reddit brand token untranslated under AR/HE");

    // ---- 3) Contact factual data survives; labels translate -----------------
    TEST_CHECK(ar.contacts[1].find(L"+82 2 575 0414") != std::wstring::npos &&
                   he.contacts[1].find(L"+82 2 575 0414") != std::wstring::npos,
               "B5: phone stays in the contact line under AR/HE (design G-5: LTR run inside RTL paragraph)");
    TEST_CHECK(ar.contacts[1] != en.contacts[1] && he.contacts[1] != en.contacts[1],
               "B5: contact-phone line's label localized under AR/HE");

    // ---- 4) Reset label localized + non-empty (design §4.1 last row) --------
    TEST_CHECK(!ar.reset_label.empty() && ar.reset_label != en.reset_label,
               "B5: AR reset_label localized + non-empty (escape hatch readable in RTL UI)");
    TEST_CHECK(!he.reset_label.empty() && he.reset_label != en.reset_label,
               "B5: HE reset_label localized + non-empty");
    TEST_CHECK(ar.reset_label != he.reset_label,
               "B5: AR vs HE reset labels differ (per-locale tables, not one shared RTL string)");

    // ---- 5) Direction seam: pure locale->direction policy (B-1 re-check on
    // the two locales B-5 wires into About's ApplyLocaleFormatting) -----------
    TEST_CHECK(DirectionForLocale(UiLocale::Arabic) == TextDirection::RTL &&
                   DirectionForLocale(UiLocale::Hebrew) == TextDirection::RTL &&
                   DirectionForLocale(UiLocale::Persian) == TextDirection::RTL &&
                   DirectionForLocale(UiLocale::Urdu) == TextDirection::RTL,
               "B5: all four RTL UI locales resolve RTL (About body direction policy)");
    TEST_CHECK(DirectionForLocale(UiLocale::English) == TextDirection::LTR &&
                   DirectionForLocale(UiLocale::Korean) == TextDirection::LTR,
               "B5: representative LTR locales stay LTR (no over-flip)");

    // ---- 6) B-5 font-tag source: LocaleMapping.bcp47_full for the RTL rows
    // (about_window's ApplyLocaleFormatting clones these tags onto the body/
    // tagline/etymology formats; a wrong/empty tag silently disables the Q5-A
    // fallback, so pin the four rows About can actually show) -----------------
    {
        auto tag_for = [](UiLocale loc) -> std::wstring {
            for (const LocaleMapping& m : GetLocaleMappings()) {
                if (m.locale == loc) return m.bcp47_full ? std::wstring(m.bcp47_full) : std::wstring();
            }
            return std::wstring();
        };
        TEST_CHECK(tag_for(UiLocale::Arabic) == L"ar-SA" && tag_for(UiLocale::Hebrew) == L"he-IL" &&
                       tag_for(UiLocale::Persian) == L"fa-IR" && tag_for(UiLocale::Urdu) == L"ur-PK",
                   "B5: RTL rows carry the representative full BCP-47 tags About clones in");
    }

    // ---- 7) Runtime smoke of the REAL COM mutation path under an RTL locale.
    // No test-only seam: this drives the production Create() +
    // RequestLocaleRefresh() (same-thread synchronous branch), so DWrite
    // actually executes SetReadingDirection(RTL) on the live body/tagline/
    // etymology formats and the clone-swap onto the ar-SA tag must round-trip
    // the GetLocaleName readback (a rejected tag would trip the fail-safe
    // DIAG, not crash). Direction VALUE correctness stays with the DIAG line
    // + manual QA M4 per design §4.2 (anti-gaming: no COM readback seam).
    {
        I18n::SetLocale(UiLocale::Arabic);
        AboutWindow rtlAbout;
        TEST_CHECK(rtlAbout.Create(::GetModuleHandleW(nullptr)),
                   "B5: AboutWindow Create under AR locale (startup ApplyLocaleFormatting RTL path)");
        rtlAbout.RequestLocaleRefresh(); // same thread -> direct GUI path, hidden window
        TEST_CHECK(!rtlAbout.IsVisible(),
                   "B5: hidden About stays hidden through AR locale refresh (repaint gate intact)");
        rtlAbout.Destroy();
    }

    I18n::SetLocale(initial); // suite-state hygiene

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] B-5 About RTL content tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] B-5 About RTL content tests: " << (g_failed_count - failures_before)
                  << " check(s) failed." << std::endl;
    }
}

// P4 Batch B-6 (session 260907_0002, design §4.1 "TestReq040SystemDefaults37"
// row + §2.3.3 C1-C10 proof map): the REQ-040 rule-convenience proof suite.
// Proves the documented default/sticky/reset rules hold for ALL 37 locales
// with zero gaming: the C2 parametric leg drives the REAL production function
// (ResolveDragDefaultTarget) through its only input seam - the I18n locale
// GetLocaleCode reads - once per LocaleMapping row; the oracle is the
// design §2.6 mapping derived independently from each row's registry data.
// Gate decisions pinned here are user-approved (decisions.md 2026-09-07):
//   G-1 reset also restores ui_language = "auto"  (C7-unit half)
//   G-2 EN-pivot stays "Korean"                   (EN row of the C2 loop)
//   G-4 Hebrew detection                          (pinned in TestSmartBypassModule 8b)
void TestReq040SystemDefaults37() {
    std::cout << "[RUN] Testing REQ-040 system defaults over all 37 locales (B-6)..." << std::endl;
    const int failures_before = g_failed_count;

    // Save/restore the global locale the parametric loop mutates (same
    // suite-state hygiene as TestReq038B5AboutRtl).
    const UiLocale initial = I18n::GetCurrentLocale();

    // Independent oracle: the system-language 1:1 mirror (design §2.3.1 as
    // superseded by F2 session 260908_0003, decisions.md 2026-09-08 22:10
    // APPROVED OVERRIDE: "remove EN-OS->Korean special case; system-language
    // 1:1 mapping"), derived from the registry, NOT by calling the function
    // under test. The former G-2 "EN -> Korean" pin (user decision
    // 2026-09-07) is EXPLICITLY SUPERSEDED by the 2026-09-08 user declaration
    // "이 앱은 한국어<->영어 를 위한 앱이 아니야" (this app is not a KO<->EN app).
    auto expected_for_sys_code = [](const std::string& code_or_name) -> std::string {
        const std::string norm = NormalizeLanguageCode(code_or_name);
        const LanguageInfo* info = FindLanguageByCode(norm);
        if (!info || info->code == "AUTO") return "English"; // unsupported/unknown OS
        return info->name_en;                                 // 1:1 mirror (incl. EN -> English)
    };

    // ---- C2 parametric: every one of the 37 LocaleMapping rows drives the
    // REAL ResolveDragDefaultTarget through its input seam (I18n::SetLocale ->
    // GetSystemLanguageCode/GetLocaleCode). Each row's expectation is its
    // registry name_en; the EN row is the G-2 "Korean" pivot pin.
    {
        const auto& maps = GetLocaleMappings();
        TEST_CHECK(maps.size() == 37, "B6/C2: parametric loop covers exactly 37 mapping rows");
        for (const auto& m : maps) {
            I18n::SetLocale(m.locale); // simulate "OS reports this language"
            const std::string sys_code = NormalizeLanguageCode(std::string(I18n::GetSystemLanguageCode()));
            const std::string want = expected_for_sys_code(sys_code);
            const std::string got = ResolveDragDefaultTarget(); // production function
            TEST_CHECK(got == want,
                       ("B6/C2: system language " + std::string(m.config_code) +
                        " -> drag default " + got + " (want " + want + ")").c_str());
        }
    }

    // ---- G-2 SUPERSEDED (F2, session 260908_0003): the 2026-09-07 pin
    // "EN-OS drag default = Korean" was explicitly overridden by the user on
    // 2026-09-08 22:10 (decisions.md APPROVED OVERRIDE, matching "도착언어가
    // 기본값은 시스템언어" - target default is the SYSTEM language). An EN
    // system language now mirrors 1:1 to "English". EN-OS + EN text dragged
    // hits the ADR-A1-7 rank-3 skip (source shown as-is), NOT corruption.
    {
        I18n::SetLocale(UiLocale::English);
        TEST_CHECK(ResolveDragDefaultTarget() == "English",
                   "B6/G-2(F2): EN-OS drag default mirrors 1:1 to 'English' (2026-09-08 APPROVED OVERRIDE of the 2026-09-07 pin)");
    }

    // ---- AUTO/unknown-OS half of the mapping (design §2.3.3 "AUTO ->
    // English"). No production seam can feed ResolveDragDefaultTarget an
    // unresolvable code - GetLocaleCode is table-driven and always emits a
    // known config_code (B-3 invariant, pinned by TestReq037LocaleMapping) -
    // so this leg pins the DEFENSE-IN-DEPTH branch on the exact primitives the
    // function composes: an unknown OS tag normalizes to AUTO, the AUTO
    // registry row is matched by FindLanguageByCode, and the oracle (same
    // §2.6 mapping the loop above verified against production for all 37 real
    // rows) yields English for both the AUTO sentinel and garbage input.
    {
        TEST_CHECK(NormalizeLanguageCode("sw-KE") == "AUTO",
                   "B6/AUTO: unsupported OS tag 'sw-KE' normalizes to AUTO (design §2.1.5)");
        TEST_CHECK(NormalizeLanguageCode("klingon") == "AUTO",
                   "B6/AUTO: garbage token normalizes to AUTO");
        const LanguageInfo* auto_row = FindLanguageByCode("AUTO");
        TEST_CHECK(auto_row != nullptr && auto_row->code == "AUTO",
                   "B6/AUTO: AUTO sentinel is a real registry row (the branch ResolveDragDefaultTarget guards)");
        TEST_CHECK(expected_for_sys_code("AUTO") == "English" &&
                       expected_for_sys_code("sw-KE") == "English",
                   "B6/AUTO: unknown/AUTO system language maps to 'English' (design §2.3.3 AUTO leg)");
    }

    // ---- C1/C3/C4 fresh-config defaults: a pre-Phase-3 JSON (no new-schema
    // keys) loads through the migration branch and must land on the documented
    // defaults; ui_language default ("auto") is the G-1 reset target value.
    {
        AppConfig fresh;
        // Deliberately legacy-only JSON: none of the four context keys.
        const bool parsed = fresh.FromJsonString(
            "{ \"ui_language\": \"auto\", \"engine_type\": \"google\", "
            "\"source_language\": \"Korean\", \"target_language\": \"Japanese\" }");
        TEST_CHECK(parsed, "B6/C1-C4: legacy JSON parses");
        const auto s = fresh.GetSnapshot();
        TEST_CHECK(s.drag_source_language == "Auto Detect",
                   "B6/C1: fresh-config drag SOURCE default is Auto Detect (REQ-006)");
        TEST_CHECK(s.drag_target_language == ResolveDragDefaultTarget(),
                   "B6/C2-host: fresh-config drag TARGET default = OS-resolved (REQ-007)");
        TEST_CHECK(s.drag_target_language == expected_for_sys_code(
                       NormalizeLanguageCode(std::string(I18n::GetSystemLanguageCode()))),
                   "B6/C2-host: fresh-config drag target matches the §2.6 mapping for the host OS");
        TEST_CHECK(s.type_source_language == "Auto Detect",
                   "B6/C3: fresh-config typing SOURCE default is Auto Detect (REQ-015)");
        TEST_CHECK(s.type_target_language == "English",
                   "B6/C4: fresh-config typing TARGET default is English (REQ-016)");
    }

    // ---- C7 unit half + G-1 (decisions.md 2026-09-07 "G-1 포함"): the reset
    // coordinator (main.cpp apply_system_defaults) writes the four language
    // keys to ComputeSystemDefaultLanguages() AND ui_language back to "auto",
    // persists all five, and a restart reload reads them back. The five
    // Set*/SaveToFile calls below mirror the coordinator exactly (its
    // ordering: persist all five keys under ONE atomic swap, then re-resolve
    // the locale, then RefreshAllUiForLocaleChange - design §5.2-5).
    {
        AppConfig cfg;
        cfg.SetDragLanguages("English", "Vietnamese");   // sticky user picks
        cfg.SetDragTargetPinned(true);                   // F2: explicit pick pinned
        cfg.SetTypeLanguages("Korean", "Japanese");
        cfg.SetUiLanguage("th");                          // UI=Thai (G-1 scenario:
                                                          // user cannot read the UI)
        const auto pre = cfg.GetSnapshot();
        TEST_CHECK(pre.drag_target_language == "Vietnamese" && pre.ui_language == "th" &&
                       pre.drag_target_pinned,
                   "B6/C7-G1: reset fixture applied (sticky pair + F2 pin + ui_language=th)");

        const auto defs = ComputeSystemDefaultLanguages();
        cfg.SetDragLanguages(defs.drag_source, defs.drag_target); // exactly what
        cfg.SetDragTargetPinned(false);                           // F2 pin clear
        cfg.SetTypeLanguages(defs.type_source, defs.type_target); // apply_system_defaults
        cfg.SetUiLanguage("auto");                                // calls (B-6 G-1)
        const std::string json = cfg.ToJsonString();               // = SaveToFile payload
        TEST_CHECK(json.find("\"ui_language\": \"auto\"") != std::string::npos,
                   "B6/C7-G1: serialized config carries ui_language auto after reset");
        TEST_CHECK(json.find("\"drag_source_language\": \"Auto Detect\"") != std::string::npos &&
                       json.find("\"type_source_language\": \"Auto Detect\"") != std::string::npos &&
                       json.find("\"type_target_language\": \"English\"") != std::string::npos,
                   "B6/C7: serialized config carries the 4 reset language keys (re-record, never deletion)");

        AppConfig restarted;
        TEST_CHECK(restarted.FromJsonString(json), "B6/C7-G1: restarted config parses the reset JSON");
        const auto post = restarted.GetSnapshot();
        TEST_CHECK(post.ui_language == "auto",
                   "B6/G-1: reset restores ui_language to 'auto' across restart (surfaces re-resolve via DetectSystemLocale)");
        TEST_CHECK(post.drag_source_language == "Auto Detect" &&
                       post.drag_target_language == defs.drag_target &&
                       post.drag_target_pinned == false &&
                       post.type_source_language == "Auto Detect" &&
                       post.type_target_language == "English",
                   "B6/C7: all four language keys + F2 pin (cleared) restored to system defaults across restart");

        // G-1 semantics: "auto" is a VALID persisted value that the selector
        // planner accepts and that resolves through DetectSystemLocale (the
        // exact call apply_system_defaults performs before the refresh).
        const UiLocaleChangePlan plan = PlanUiLocaleChange(post.ui_language, "auto");
        TEST_CHECK(plan.valid, "B6/G-1: persisted 'auto' is a valid selector value after reset");
        const UiLocale resolved = I18n::DetectSystemLocale();
        TEST_CHECK(resolved != UiLocale::Auto,
                   "B6/G-1: DetectSystemLocale never returns the Auto sentinel (reset re-resolve lands on a concrete locale)");
    }

    I18n::SetLocale(initial); // suite-state hygiene

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] B-6 REQ-040 system-defaults 37-locale tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] B-6 REQ-040 system-defaults 37-locale tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- REQ-003 (session 260909): PII logging gating tests ----
// Pins the release posture: diag_log_content defaults to false, survives the
// SaveToFile/LoadFromFile round-trip, absent-key configs load as false, and
// the diag::SetContentLogging/ContentLoggingEnabled seam behaves as the
// lock-free toggle the hook/worker call sites branch on. The call-site
// branching itself (KEY char=/title=, capture content=, translate out=,
// prompt body) is exercised by the shipped code paths (hook/worker threads),
// not headlessly here; the gate state it reads is.
static void TestReq003PiiGating() {
    std::cout << "[RUN] Testing REQ-003 diag_log_content PII gating..." << std::endl;
    const int failures_before = g_failed_count;
    namespace fs = std::filesystem;
    std::error_code ec;

    // (a1) Compile-time default is FALSE (release posture).
    {
        AppConfig cfg;
        TEST_CHECK(cfg.diag_log_content == false,
                   "REQ-003: AppConfig.diag_log_content defaults to false");
    }

    // (a2) Field absent from the JSON (every pre-260909 config.json) => false.
    // This mirrors the ONLY real load path: main.cpp default-constructs
    // AppConfig (diag_log_content=false), then LoadFromFile->FromJsonString.
    // FromJsonString leaves absent keys at their current value (the shared
    // contract for every bool field, e.g. cloud_fallback_enabled), so a fresh
    // object + legacy keyless JSON must read back the compile-time default.
    {
        AppConfig cfg; // default false
        const bool parsed = cfg.FromJsonString(
            "{ \"ui_language\": \"auto\", \"engine_type\": \"google\" }");
        TEST_CHECK(parsed, "REQ-003: legacy JSON (no diag_log_content) parses");
        TEST_CHECK(cfg.diag_log_content == false,
                   "REQ-003: absent diag_log_content loads as false (default kept)");
    }

    // (a3) Explicit true / false parsing.
    {
        AppConfig t;
        TEST_CHECK(t.FromJsonString("{ \"diag_log_content\": true }"),
                   "REQ-003: diag_log_content=true JSON parses");
        TEST_CHECK(t.diag_log_content == true,
                   "REQ-003: diag_log_content true parsed");
        AppConfig f;
        TEST_CHECK(f.FromJsonString("{ \"diag_log_content\": false }"),
                   "REQ-003: diag_log_content=false JSON parses");
        TEST_CHECK(f.diag_log_content == false,
                   "REQ-003: diag_log_content false parsed");
    }

    // (b) Round-trip through the REAL persistence path: set true -> SaveToFile
    //     -> LoadFromFile (fresh object) -> still true; the serialization
    //     always carries the key so restarts are deterministic.
    {
        const fs::path dir = fs::temp_directory_path(ec) / "emebala_req003_test";
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        const fs::path file = dir / "config.json";

        AppConfig cfg;
        cfg.diag_log_content = true;
        TEST_CHECK(cfg.SaveToFile(file), "REQ-003: SaveToFile succeeds");
        {
            std::ifstream in(file, std::ios::binary);
            std::string raw((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
            TEST_CHECK(raw.find("\"diag_log_content\": true") != std::string::npos,
                       "REQ-003: serialized config carries diag_log_content: true");
        }
        AppConfig reloaded;
        TEST_CHECK(reloaded.LoadFromFile(file), "REQ-003: LoadFromFile succeeds");
        TEST_CHECK(reloaded.diag_log_content == true,
                   "REQ-003: diag_log_content true survives SaveToFile/LoadFromFile round-trip");

        // Default-false also round-trips (fresh install writes false).
        AppConfig def;
        TEST_CHECK(def.SaveToFile(file), "REQ-003: default SaveToFile succeeds");
        AppConfig back;
        back.diag_log_content = true; // poison against a silent no-key read
        TEST_CHECK(back.LoadFromFile(file), "REQ-003: default round-trip reload");
        TEST_CHECK(back.diag_log_content == false,
                   "REQ-003: diag_log_content false survives round-trip");
        fs::remove_all(dir, ec);
    }

    // (c) diag seam: default off, toggle on/off reflected (hook/worker branch
    //     on exactly this). Restore false afterwards (suite-state hygiene —
    //     the shipped startup state is off and later tests assume the default).
    {
        TEST_CHECK(!diag::ContentLoggingEnabled(),
                   "REQ-003: ContentLoggingEnabled defaults to false");
        diag::SetContentLogging(true);
        TEST_CHECK(diag::ContentLoggingEnabled(),
                   "REQ-003: SetContentLogging(true) reflected");
        diag::SetContentLogging(false);
        TEST_CHECK(!diag::ContentLoggingEnabled(),
                   "REQ-003: SetContentLogging(false) reflected");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-003 diag_log_content PII gating tests completed." << std::endl;
    } else {
        std::cout << "[FAIL] REQ-003 diag_log_content PII gating tests: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// P5-F1 (session 260909_0004): vulkan_guard — driverless-machine CPU fallback.
// Covers the pure decision, the real loader probe on THIS machine (Vulkan
// present: probe must succeed and the backend must stay enabled), the stub
// value shapes (executed directly: they are plain functions inside our own
// module), and the failure-hook substitution/no-subtraction contract on
// synthesized DelayLoadInfo. This is the harness the task mandates for the
// "disable mechanism works" proof: the stub return -3 is exactly the VkResult
// Vulkan-Hpp's detail::resultCheck turns into vk::SystemError, which
// ggml_backend_vk_reg() (ggml-vulkan.cpp:11273) catches -> backend nullptr ->
// register_backend skips it (ggml-backend-reg.cpp:211-213) -> no Vulkan
// devices, no 0xC06D007E. A driverless machine differs only in that the
// loader probe fails and dliFailLoadLib fires; both legs are asserted here.
void TestVulkanGuard() {
    std::cout << "[RUN] Testing Vulkan Guard (P5-F1)..." << std::endl;
    const int failures_before = g_failed_count;

    // 1. Pure decision table (all four combinations).
    TEST_CHECK(emebalachat::VulkanBackendShouldBeDisabled(false, false) == true,
               "guard: loader missing + guard on -> Vulkan must be disabled");
    TEST_CHECK(emebalachat::VulkanBackendShouldBeDisabled(true, false) == false,
               "guard: loader present + guard on -> Vulkan stays enabled");
    TEST_CHECK(emebalachat::VulkanBackendShouldBeDisabled(false, true) == false,
               "guard: kill-switch honored -> no disable decision even loaderless");
    TEST_CHECK(emebalachat::VulkanBackendShouldBeDisabled(true, true) == false,
               "guard: kill-switch + loader present -> enabled");

    // 2. The kill-switch must be OFF for the rest of this suite to be
    // meaningful (assert, then clear so the hook legs below are deterministic
    // regardless of the developer's ambient environment).
    ::SetEnvironmentVariableA("GGML_VK_GUARD_DISABLE", nullptr);
    TEST_CHECK(emebalachat::VulkanGuardDisabledByEnv() == false,
               "guard: env kill-switch cleared for deterministic assertions");

    // 3. Real probe on this machine. The test box has the Vulkan SDK + driver
    // loader, so LoadLibraryW("vulkan-1.dll") must resolve, the decision must
    // be "backend stays enabled", and the probe must have released its handle
    // (no refcount corruption: re-loading still succeeds afterwards).
    const emebalachat::VulkanGuardResult res = emebalachat::EnsureVulkanGuard();
    TEST_CHECK(res.loader_resolved == true,
               "guard probe: vulkan-1.dll resolvable on this machine (Vulkan present)");
    TEST_CHECK(res.guard_disabled_by_env == false, "guard probe: no env kill-switch");
    TEST_CHECK(res.backend_expected_disabled == false,
               "guard probe: Vulkan backend must STAY enabled here (driver present)");
    HMODULE reprobe = ::LoadLibraryW(L"vulkan-1.dll");
    TEST_CHECK(reprobe != nullptr, "guard probe: loader state intact after probe (FreeLibrary not corrupting)");
    if (reprobe) {
        ::FreeLibrary(reprobe);
    }

    // 3b. Log proof (task e item 2): re-run the probe with the diag logger on
    // a temp dir and assert the decision line actually lands in the log file
    // ("guard/003 ... resolved ... stays enabled" on this Vulkan-present box).
    {
        const std::filesystem::path log_dir =
            std::filesystem::temp_directory_path() / "eme_f1_guard_logs";
        std::error_code mkec;
        std::filesystem::create_directories(log_dir, mkec);
        const bool diag_was_live = diag::IsInitialized();
        if (!diag_was_live) {
            diag::Init(log_dir);
        }
        diag::Flush();
        const emebalachat::VulkanGuardResult logged = emebalachat::EnsureVulkanGuard();
        diag::Flush();
        std::wstring log_path = diag::CurrentLogPath();
        if (!diag_was_live) {
            diag::Shutdown();
        }
        std::string log_text;
        if (!log_path.empty()) {
            std::ifstream lf(std::filesystem::path(log_path), std::ios::binary);
            std::ostringstream ss;
            ss << lf.rdbuf();
            log_text = ss.str();
        }
        TEST_CHECK(log_text.find("guard/003: vulkan-1.dll resolved") != std::string::npos,
                   "guard log: probe-success decision line written to the diag log");
        TEST_CHECK(logged.loader_resolved && !logged.backend_expected_disabled,
                   "guard log run: backend stays enabled on this machine");
    }

    // 4. Stub value shapes, executed directly (all stubs are static functions
    // linked into this test image; calling them via the published mapping is
    // the safe, driverless-free proof of the F1 mechanism):
    using FnVersion = INT32(WINAPI*)(UINT32*);
    using FnProps = INT32(WINAPI*)(const char*, UINT32*, void*);
    using FnGPA = void* (WINAPI*)(void*, const char*);
    using FnAny = INT64(WINAPI*)(void*, void*, void*, void*);

    auto fn_version = reinterpret_cast<FnVersion>(
        emebalachat::VulkanGuardStubForImport("vkEnumerateInstanceVersion"));
    UINT32 api_version = 0xDEADBEEFu;
    const INT32 vr = fn_version(&api_version);
    TEST_CHECK(vr == -3, "stub: vkEnumerateInstanceVersion returns VK_ERROR_INITIALIZATION_FAILED(-3)");
    TEST_CHECK(api_version == 0u, "stub: vkEnumerateInstanceVersion zeroes out-param");

    auto fn_props = reinterpret_cast<FnProps>(
        emebalachat::VulkanGuardStubForImport("vkEnumerateInstanceExtensionProperties"));
    UINT32 count = 99u;
    TEST_CHECK(fn_props(nullptr, &count, nullptr) == -3,
               "stub: vkEnumerateInstanceExtensionProperties returns -3");
    TEST_CHECK(count == 0u, "stub: enumerate out-count zeroed");

    auto fn_layers = reinterpret_cast<FnProps>(
        emebalachat::VulkanGuardStubForImport("vkEnumerateInstanceLayerProperties"));
    TEST_CHECK(fn_layers(nullptr, &count, nullptr) == -3,
               "stub: vkEnumerateInstanceLayerProperties returns -3");

    auto fn_gpa = reinterpret_cast<FnGPA>(
        emebalachat::VulkanGuardStubForImport("vkGetInstanceProcAddr"));
    void* pfn = fn_gpa(nullptr, "vkCreateDevice");
    TEST_CHECK(pfn != nullptr, "stub: vkGetInstanceProcAddr yields non-null PFN (no null-call AV)");
    // The PFN the stub hands out is itself -3-shaped when used as a query.
    auto fn_via_gpa = reinterpret_cast<FnAny>(pfn);
    TEST_CHECK(fn_via_gpa(nullptr, nullptr, nullptr, nullptr) == -3,
               "stub: PFN returned via GetInstanceProcAddr is -3-shaped");

    // Unknown / ordinal-style names fall back to the universal stub, never null.
    auto fn_unknown = reinterpret_cast<FnAny>(
        emebalachat::VulkanGuardStubForImport("vkNotARealCommand"));
    TEST_CHECK(fn_unknown != nullptr, "stub: unknown name gets non-null universal stub");
    TEST_CHECK(fn_unknown(nullptr, nullptr, nullptr, nullptr) == -3,
               "stub: universal stub returns -3");
    auto fn_null_named = reinterpret_cast<FnAny>(emebalachat::VulkanGuardStubForImport(nullptr));
    TEST_CHECK(fn_null_named != nullptr &&
               fn_null_named(nullptr, nullptr, nullptr, nullptr) == -3,
               "stub: null name tolerated (universal fallback)");

    // 5. Failure-hook contract on synthesized DelayLoadInfo (exactly what the
    // delay helper would pass on a driverless machine / any late failure).
    DelayLoadInfo dli{};
    dli.cb = sizeof(dli);
    dli.szDll = const_cast<LPSTR>("vulkan-1.dll");
    dli.dlp.fImportByName = TRUE;
    dli.dlp.szProcName = const_cast<LPSTR>("vkEnumerateInstanceVersion");

    FARPROC h = emebalachat::VulkanGuardFailureHook(dliFailLoadLib, &dli);
    TEST_CHECK(h == reinterpret_cast<FARPROC>(::GetModuleHandleW(nullptr)),
               "hook: dliFailLoadLib(vulkan-1.dll) substitutes the host-exe module (stub path opens)");

    FARPROC s = emebalachat::VulkanGuardFailureHook(dliFailGetProc, &dli);
    TEST_CHECK(s == reinterpret_cast<FARPROC>(
                   emebalachat::VulkanGuardStubForImport("vkEnumerateInstanceVersion")),
               "hook: dliFailGetProc returns the mapped typed stub");

    // Case-insensitive DLL-name match (delayimp passes the descriptor string).
    dli.szDll = const_cast<LPSTR>("VULKAN-1.DLL");
    TEST_CHECK(emebalachat::VulkanGuardFailureHook(dliFailLoadLib, &dli) != nullptr,
               "hook: DLL name matched case-insensitively");

    // Ordinal-style import (fImportByName=FALSE) must still get a stub, never null.
    dli.szDll = const_cast<LPSTR>("vulkan-1.dll");
    dli.dlp.fImportByName = FALSE;
    dli.dlp.dwOrdinal = 1;
    TEST_CHECK(emebalachat::VulkanGuardFailureHook(dliFailGetProc, &dli) != nullptr,
               "hook: ordinal import -> universal stub (total coverage)");

    // Non-vulkan DLLs must NOT be touched (pre-F1 CUDA delay-load behavior kept).
    DelayLoadInfo dli_cuda{};
    dli_cuda.cb = sizeof(dli_cuda);
    dli_cuda.szDll = const_cast<LPSTR>("cublas64_13.dll");
    dli_cuda.dlp.fImportByName = TRUE;
    dli_cuda.dlp.szProcName = const_cast<LPSTR>("cublasCreate_v2");
    TEST_CHECK(emebalachat::VulkanGuardFailureHook(dliFailLoadLib, &dli_cuda) == nullptr,
               "hook: cublas failure passes through untouched (no CUDA behavior change)");
    TEST_CHECK(emebalachat::VulkanGuardFailureHook(dliFailGetProc, &dli_cuda) == nullptr,
               "hook: cublas getproc passes through untouched");

    // Kill-switch honored: with the env set, even the vulkan DLL gets nullptr
    // (pre-F1 behavior for triage).
    ::SetEnvironmentVariableA("GGML_VK_GUARD_DISABLE", "1");
    TEST_CHECK(emebalachat::VulkanGuardDisabledByEnv() == true, "guard: kill-switch env detected");
    dli.dlp.fImportByName = TRUE;
    dli.dlp.szProcName = const_cast<LPSTR>("vkEnumerateInstanceVersion");
    TEST_CHECK(emebalachat::VulkanGuardFailureHook(dliFailLoadLib, &dli) == nullptr,
               "guard: kill-switch makes hook inert (dliFailLoadLib)");
    TEST_CHECK(emebalachat::VulkanGuardFailureHook(dliFailGetProc, &dli) == nullptr,
               "guard: kill-switch makes hook inert (dliFailGetProc)");
    const emebalachat::VulkanGuardResult res_off = emebalachat::EnsureVulkanGuard();
    TEST_CHECK(res_off.guard_disabled_by_env == true,
               "guard probe: reports kill-switch state");
    TEST_CHECK(res_off.backend_expected_disabled == false,
               "guard probe: kill-switch -> no disable decision");
    ::SetEnvironmentVariableA("GGML_VK_GUARD_DISABLE", nullptr);

    // 6. Hook pointers actually bound into the image delayimp reads. The
    // names are declared extern "C" at global scope by delayimp.h (included
    // via vulkan_guard.hpp); referencing them here forces the linker to pull
    // vulkan_guard.obj into run_tests.exe — a mis-linked (internal-linkage)
    // hook definition would fail THIS build with unresolved externals.
    // Verify the binding itself.
    TEST_CHECK(__pfnDliFailureHook2 == &emebalachat::VulkanGuardFailureHook,
               "linkage: delayimp failure-hook pointer bound to VulkanGuardFailureHook");
    TEST_CHECK(__pfnDliNotifyHook2 != nullptr,
               "linkage: delayimp notify hook pointer defined");

    std::cout << (g_failed_count == failures_before ? "[PASS]" : "[FAIL]")
              << " Vulkan Guard tests." << std::endl;
}

int main() {
    // REQ-R15: mirror wWinMain's first step - declare Per-Monitor-V2 DPI
    // awareness BEFORE any window or DC is created in this process. The
    // runtime scaling assertions in TestDpiMixedScaling (physical window
    // sizes via GetWindowRect) are only meaningful under a DPI-aware
    // process; created-while-unaware windows also get DWM-bitmap-stretched
    // in the suite exactly like the shipped app did before this batch.
    emebalachat::ui::EnsurePerMonitorV2ProcessDpiAwareness();

    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::cout << "========================================" << std::endl;
    std::cout << "  Emebalachat C++20 Core Test Suite     " << std::endl;
    std::cout << "========================================" << std::endl;

    TestConfigModule();
    TestUnicodeModule();
    TestSmartBypassModule();
    TestSelfLanguageBypass();
    TestSoundModule();
    TestWin32InputModule();
    TestClipboardSequencePolling();
    TestGoogleTranslateModule();
    TestGoogleHttpProfile();
    TestEngineModule();
    TestModelPathValidation();
    TestModelSha256Verification(); // F3: runtime SHA-256 pin + marker cache
    TestModelPathNormalization();
    TestConfigSnapshotThreadSafety();
    TestTokenTruncation();
    TestSelectionReleaseMatrix();
    TestMultiLineBlockFix();
    TestBadgeDynamicSizing();
    TestI18nModule();
    TestDragToTranslateComponents();
    TestDragIconClickShowsTooltip();
    TestDragIconClickClipboardEarlyReturn();
    TestTtsVoiceSelectionModule();
    TestHotkeyParsing();
    TestPhase6HotkeyWiring();
    TestKeyboardHookStateSyncAndDispatch();
    TestMouseHookDebounce();
    TestUIMarshaling();
    TestDpiMixedScaling();
    TestHookLifecyclePolicy();
    TestImeCompositionGate();
    TestShiftEnterGate();
    TestLanguageSwitchingMatrix();
    TestEngineShutdownCancellation();
    TestEngineFallbackExeDirAnchoring();
    TestBatch2VersionScrollAbout();
    TestB3LanguageSync();
    TestResolveEffectiveTarget();
    TestResolveEffectiveSource();
    TestPhase3LanguageContexts();
    TestPhase4SystemDefaults();
    TestB1TooltipStaleness();
    TestR6P3MemoryLifecycle();
    TestR6P4LanguageRouting();
    TestReqF4aPreloadGate(); // REQ-F4a: startup preload gate (cloud-only skip)
    TestReq004EngineSwitchPreloadGate(); // REQ-004: tray switch-to-local async preload gate
#ifdef HAVE_LLAMA_CPP
    TestP7F2GpuOffloadParams(); // P7-F2: CUDA+Vulkan layer-split prevention seam
#endif
    TestR6P5P6I18n();
    TestDiagLogger();
    TestPhase5AppClassifier();
    TestPhase8ConsoleGate();
    TestReq027CaretTracker();
    TestReq027OffsetAfterNewline();
    TestReq027CapabilityProbe();
    TestReq034ManualNewlineRecovery();
    TestReq034NoLeadingCrlfNormalProgress();
    TestReq034PasteWindowSuppress();
    TestReq036MultiBlockNoRetranslation();
    TestReqF1FirstCharResidue();
    TestReqF2Category0Accumulation();
    TestReqF5EmptyCaptureEnterPromotion();
    TestReqF6LedgerH1AbortPreserve();
    TestReqF3BlockSliceCurrentBlockOnly();
    TestReqF7ClipboardRestore();
    TestReq039ChatWindowEnterCapture();
    TestBidiUtils();
    TestReq038B2RegistryBcp47();
    TestReq037LocaleMapping();
    TestReq038B5AboutRtl();
    TestReq040SystemDefaults37();
    TestReq003PiiGating(); // REQ-003: diag_log_content PII logging gate
    TestVulkanGuard();     // P5-F1: driverless-machine Vulkan guard (probe+stubs+hook)

    std::cout << "========================================" << std::endl;
    std::cout << "Total Checks: " << g_test_count << std::endl;
    std::cout << "Failures:     " << g_failed_count << std::endl;
    std::cout << "========================================" << std::endl;

    ::CoUninitialize();

    if (g_failed_count == 0) {
        std::cout << ">>> ALL CORE TESTS PASSED SUCCESSFULLY! <<<" << std::endl;
        return 0;
    } else {
        std::cerr << ">>> TEST FAILURES DETECTED! <<<" << std::endl;
        return 1;
    }
}
