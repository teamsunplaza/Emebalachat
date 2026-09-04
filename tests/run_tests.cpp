#include "../src/config.hpp"
#include "../src/unicode_utils.hpp"
#include "../src/smart_bypass.hpp"
#include "../src/sound.hpp"
#include "../src/win32_input.hpp"
#include "../src/google_translate.hpp"
#include "../src/engine.hpp"
#include "../src/i18n.hpp"
#include "../src/ui/badge.hpp"
#include "../src/ui/drag_icon.hpp"
#include "../src/ui/tooltip.hpp"
#include "../src/ui/asset_loader.hpp"
#include "../src/mouse_hook.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
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

void TestConfigModule() {
    std::cout << "[RUN] Testing Config & Languages..." << std::endl;

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

    std::string prompt_zh_cn = BuildPrompt("Hello", "ZH-CN");
    TEST_CHECK(prompt_zh_cn == "将以下文本翻译为ZH-CN，注意只需要输出翻译后的结果，不要额外解释：\n\nHello",
               "Prompt formatting matches specification (ZH-CN branch)");

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

    std::cout << "[PASS] Config & Languages tests completed." << std::endl;
}

void TestUnicodeModule() {
    std::cout << "[RUN] Testing Unicode & Normalization..." << std::endl;

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

    std::cout << "[PASS] Unicode & Normalization tests completed." << std::endl;
}

void TestSmartBypassModule() {
    std::cout << "[RUN] Testing Smart Bypass..." << std::endl;

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
    TEST_CHECK(DetectLanguage(L"Hello world") == "English", "Detect English");
    TEST_CHECK(DetectLanguage(L"") == "Unknown", "Empty text detected as Unknown");
    TEST_CHECK(DetectLanguage(L"123456") == "Unknown", "Digits detected as Unknown");

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

    // Same language bypass
    TEST_CHECK(!ShouldTranslate(L"Hello world, have a good day", "English"), "English targeting English bypassed");
    TEST_CHECK(!ShouldTranslate(L"안녕하세요 만나서 반갑습니다", "Korean"), "Korean targeting Korean bypassed");
    TEST_CHECK(!ShouldTranslate(L"Xin chào bạn nhé", "Vietnamese"), "Vietnamese targeting Vietnamese bypassed");
    TEST_CHECK(!ShouldTranslate(L"こんにちは、元気ですか？", "Japanese"), "Japanese targeting Japanese bypassed");

    // Non-linguistic bypass
    std::vector<std::wstring> non_ling = {
        L"123456", L"999.99", L"https://discord.com", L":) ;) :(", L":-)", L"^_^", L"👍🔥🎉💯", L"", L"   \n\t  ", L"--- ... ---"
    };
    for (const auto& item : non_ling) {
        TEST_CHECK(!ShouldTranslate(item, "Korean"), "Non-linguistic input bypassed for Korean");
        TEST_CHECK(!ShouldTranslate(item, "English"), "Non-linguistic input bypassed for English");
        TEST_CHECK(!ShouldTranslate(item, "Vietnamese"), "Non-linguistic input bypassed for Vietnamese");
    }

    std::cout << "[PASS] Smart Bypass tests completed." << std::endl;
}

void TestSoundModule() {
    std::cout << "[RUN] Testing Sound Feedback..." << std::endl;

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
    std::cout << "[PASS] Sound Feedback tests completed." << std::endl;
}

void TestWin32InputModule() {
    std::cout << "[RUN] Testing Win32 Input & Clipboard Safety..." << std::endl;

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

    // No captured target -> always allow (CopySelectedLine path has no HWND).
    TEST_CHECK(IsSameWindowForInjection(nullptr, nullptr), "Guard allows when no target captured");
    TEST_CHECK(IsSameWindowForInjection(nullptr, fake_a), "Guard allows when target null regardless of foreground");

    // Target captured but foreground lost -> refuse to inject.
    TEST_CHECK(!IsSameWindowForInjection(fake_a, nullptr), "Guard refuses when target captured but foreground null");

    // Identical handle -> allow.
    TEST_CHECK(IsSameWindowForInjection(fake_a, fake_a), "Guard allows identical foreground/target handle");

    // Different window roots -> refuse (focus moved to another application).
    TEST_CHECK(!IsSameWindowForInjection(fake_a, fake_b), "Guard refuses different foreground window root");

    std::cout << "[PASS] Win32 Input & Clipboard Safety tests completed." << std::endl;
}

void TestGoogleTranslateModule() {
    std::cout << "[RUN] Testing Google Translate Engine..." << std::endl;

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

    std::cout << "[PASS] Google Translate Engine tests completed." << std::endl;
}

void TestEngineModule() {
    std::cout << "[RUN] Testing Translation Manager..." << std::endl;

    TranslationManager mgr(EngineType::Auto, "D:\\non_existent_model.gguf");
    TEST_CHECK(mgr.GetEngineType() == EngineType::Auto, "Preferred engine is Auto");
    TEST_CHECK(!mgr.IsLocalModelAvailable(), "Non-existent model path reported as not available");
    TEST_CHECK(mgr.GetActiveEngineName().find("Google Translate") != std::string::npos,
               "Active engine automatically falls back to Google Translate");

    // H2 gate: cloud_fallback_enabled defaults to FALSE, so the silent auto->cloud
    // path must NOT transmit; Translate returns empty instead of leaking text.
    TEST_CHECK(!mgr.IsCloudFallbackEnabled(), "Cloud fallback gate defaults to disabled (H2)");
    std::wstring gated_res = mgr.Translate(L"감사합니다", "KO", "EN");
    TEST_CHECK(gated_res.empty(), "H2: auto->cloud returns empty when consent gate is disabled (no silent leak)");

    // Enable the gate -> the same auto->cloud fallback now performs the live call.
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

    const std::string local_model_path = "D:\\OneDrive\\Documents\\models\\tsmodel\\Hy-MT2-1.8B-Q8_0.gguf";
    if (std::filesystem::exists(local_model_path)) {
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
    } else {
        std::cout << "  [LOCAL LLM SKIP] Model file not present at: " << local_model_path << std::endl;
    }

    // H2 explicit-choice exception: a user who deliberately selects engine_type=google
    // has explicitly consented to the cloud, so Translate works even with the gate off.
    TranslationManager google_mgr(EngineType::GoogleTranslate, "");
    TEST_CHECK(!google_mgr.IsCloudFallbackEnabled(), "Explicit-google manager also defaults gate off");
    std::wstring google_res = google_mgr.Translate(L"안녕하세요", "KO", "EN");
    TEST_CHECK(!google_res.empty(), "H2: explicit engine_type=google bypasses gate (deliberate consent)");
    std::cout << "  [EXPLICIT GOOGLE RESULT]: '안녕하세요' -> '" << ToUtf8(google_res) << "'" << std::endl;

    std::cout << "[PASS] Translation Manager tests completed." << std::endl;
}

// M3 (security): pure-logic validation of IsValidModelPath - no real GGUF model
// is loaded; fixtures are tiny temp files created/cleaned here.
void TestModelPathValidation() {
    std::cout << "[RUN] Testing M3 Model Path Validation..." << std::endl;

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

    std::cout << "[PASS] M3 Model Path Validation tests completed." << std::endl;
}

void TestBadgeDynamicSizing() {
    std::cout << "[RUN] Testing Floating Badge Dynamic Sizing..." << std::endl;

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

    std::cout << "[PASS] Floating Badge Dynamic Sizing tests completed." << std::endl;
}

void TestI18nModule() {
    std::cout << "[RUN] Testing Universal i18n Localization..." << std::endl;

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

    // 6. Default target pairing
    TEST_CHECK(I18n::GetDefaultTargetLanguage(UiLocale::Korean) == "English", "Korean defaults to English target");
    TEST_CHECK(I18n::GetDefaultTargetLanguage(UiLocale::English) == "Korean", "English defaults to Korean target");

    // 7. Autostart registry status inspection without throwing
    bool autostart = I18n::IsStartWithWindowsEnabled();
    (void)autostart;
    TEST_CHECK(true, "IsStartWithWindowsEnabled executes safely");

    // Reset back to OS locale
    I18n::Initialize("auto");
    std::cout << "[PASS] Universal i18n Localization tests completed." << std::endl;
}

void TestDragToTranslateComponents() {
    std::cout << "[RUN] Testing Drag-to-Translate Components..." << std::endl;

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
    TEST_CHECK(!IsChatApplicationWindow(nullptr), "IsChatApplicationWindow returns false for null HWND");

    // 5. Emebala Tablet Relief Assets & 32x32 Pill Specifications
    int icon_size = DragIconWindow::kSize;
    TEST_CHECK(icon_size == 32, "DragIconWindow size is 32px rounded icon pill");
    std::wstring resolvedLogo = FindLogoPath();
    TEST_CHECK(!resolvedLogo.empty(), "FindLogoPath resolves logo.png successfully");
    TEST_CHECK(std::filesystem::exists(resolvedLogo), "Resolved logo.png exists on disk");
    TEST_CHECK(std::filesystem::exists("D:\\OneDrive\\Projects\\Emebalachat\\assets\\logo.svg"), "Emebalachat assets/logo.svg exists");
    TEST_CHECK(std::filesystem::exists("D:\\OneDrive\\Projects\\Emebala\\assets\\logo.svg"), "Emebala assets/logo.svg exists");

    std::cout << "[PASS] Drag-to-Translate Components tests completed." << std::endl;
}

void TestTtsVoiceSelectionModule() {
    std::cout << "[RUN] Testing Multi-Language Native TTS Voice Selection..." << std::endl;

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
    std::cout << "[PASS] Multi-Language Native TTS Voice Selection tests completed." << std::endl;
}

int main() {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::cout << "========================================" << std::endl;
    std::cout << "  Emebalachat C++20 Core Test Suite     " << std::endl;
    std::cout << "========================================" << std::endl;

    TestConfigModule();
    TestUnicodeModule();
    TestSmartBypassModule();
    TestSoundModule();
    TestWin32InputModule();
    TestGoogleTranslateModule();
    TestEngineModule();
    TestModelPathValidation();
    TestBadgeDynamicSizing();
    TestI18nModule();
    TestDragToTranslateComponents();
    TestTtsVoiceSelectionModule();

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
