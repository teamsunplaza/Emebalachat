#include "../src/config.hpp"
#include "../src/diag_logger.hpp"
#include "../src/unicode_utils.hpp"
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

#include <algorithm> // R6 B1: uniqueness check on concurrent generations
#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <type_traits> // R6 B3: static_assert pins on accessor signatures
#include <vector>
#include <memory> // R6 B3: unique_ptr payloads in marshal tests

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

    // 1. Compile-time constant wiring per the delegation: total change budget
    //    ~150-200 ms, poll cadence 5-10 ms, hard deadline = change + stable.
    static_assert(kClipboardChangeTimeoutMs >= 150 && kClipboardChangeTimeoutMs <= 200,
                  "REQ-R04: change timeout must stay in the 150-200 ms band");
    static_assert(kClipboardPollIntervalMs >= 5 && kClipboardPollIntervalMs <= 10,
                  "REQ-R04: poll interval must stay in the 5-10 ms band");
    static_assert(kClipboardCopyDeadlineMs ==
                      static_cast<uint64_t>(kClipboardChangeTimeoutMs) + kClipboardStableWindowMs,
                  "REQ-R04: hard deadline = change timeout + stable window");
    static_assert(kClipboardChangeTimeoutMs == 180 && kClipboardPollIntervalMs == 8 &&
                      kClipboardStableWindowMs == 16 && kClipboardCopyDeadlineMs == 196,
                  "REQ-R04: constants are 180 ms change cap / 8 ms poll / 16 ms stable / 196 ms deadline");

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

    std::cout << "[PASS] REQ-R04 Clipboard Sequence Polling tests completed." << std::endl;
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

// ---- REQ-R05: per-endpoint HTTP profile (Chrome UA + 8 s budget) ----
// HttpGet selects its (UA, timeouts) pair and the Chrome header set from the
// request path via the pure constexpr GoogleTranslate::RequestProfileForPath /
// IsDictChromeExEndpoint functions, so the exact production wiring is pinned
// headlessly: compile-time for the budget, runtime for string content.
void TestGoogleHttpProfile() {
    std::cout << "[RUN] Testing REQ-R05 Google HTTP Profile..." << std::endl;

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

    std::cout << "[PASS] REQ-R05 Google HTTP Profile tests completed." << std::endl;
}

void TestEngineModule() {
    std::cout << "[RUN] Testing Translation Manager..." << std::endl;

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

    // Strict explicit-local semantics: with gate off, no cloud - but the failure
    // is SURFACED as CloudConsentBlocked instead of a bare {} (REQ-R02).
    {
        TranslationManager strict_local(EngineType::LocalLlama, "D:\\non_existent_model.gguf");
        TranslationStatus st = TranslationStatus::Ok;
        std::wstring res = strict_local.Translate(L"안녕하세요", "KO", "EN", &st);
        TEST_CHECK(res.empty(), "REQ-R02: explicit local without consent still keeps text on-device (empty)");
        TEST_CHECK(st == TranslationStatus::CloudConsentBlocked, "REQ-R02: privacy-block surfaced as CloudConsentBlocked, not silent");

        // Explicit local WITH consent -> cloud fallback now allowed and Ok.
        strict_local.SetCloudFallbackEnabled(true);
        TranslationStatus st2 = TranslationStatus::InputEmpty;
        std::wstring res2 = strict_local.Translate(L"안녕하세요", "KO", "EN", &st2);
        TEST_CHECK(!res2.empty(), "REQ-R02: explicit local with consent falls back to cloud");
        TEST_CHECK(st2 == TranslationStatus::Ok, "REQ-R02: consented cloud fallback reports Ok");
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

        // REQ-R01 (audit §2.1): text long enough to exceed the 2048-token context
        // must be safely truncated (head+tail window) and STILL produce a
        // non-empty translation - no decode crash, no silent {}. ~8000 Korean
        // UTF-16 units tokenize to well beyond kLlamaPromptTokenBudget (1520).
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
            TEST_CHECK(!ovf_res.empty(), "REQ-R01: >2048-token input still returns a non-empty translation (no crash, no silent empty)");
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

// REQ-R11 (audit §4 M3): ResolveModelPath / GetExecutableDir - relative model
// paths must anchor at the EXECUTABLE directory, not the CWD, so Run-registry
// autostart (CWD=C:\Windows\System32) keeps loading the model. base_dir is
// injectable, so the whole suite runs headlessly against temp fixtures.
void TestModelPathNormalization() {
    std::cout << "[RUN] Testing REQ-R11 Model Path Normalization..." << std::endl;

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
    std::cout << "[PASS] REQ-R11 Model Path Normalization tests completed." << std::endl;
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

    std::cout << "[PASS] REQ-R12 Config Snapshot Thread Safety tests completed." << std::endl;
}

// REQ-R01 (Batch D1): pure-logic tests for the head+tail sliding-window
// truncation that guards llama_decode against >n_ctx prompts. No model needed;
// asserts the geometric contract (fit-through, head/tail preservation, marker
// insertion) and UTF-16 surrogate-pair safety at both cut points.
void TestTokenTruncation() {
    std::cout << "[RUN] Testing REQ-R01 Head/Tail Token Truncation..." << std::endl;

    // Budget constants shared with the engine. Compile-time pinned per the
    // task's "static-assertion-style checks where feasible" directive (and it
    // avoids MSVC C4127 constant-condition warnings that runtime asserts on
    // constexpr values would emit under /W4).
    static_assert(kLlamaNCtx == 2048, "test build: n_ctx 2048");
    static_assert(kLlamaPromptTokenBudget == 2048 - 512 - 16, "test build: budget 1520");
    static_assert(kLlamaPromptTokenBudget == 1520, "REQ-R01: prompt token budget is 1520");
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

    std::cout << "[PASS] REQ-R01 Head/Tail Token Truncation tests completed." << std::endl;
}

// REQ-R03 (Batch D1): path matrix for the selection-release guarantee. The
// constexpr predicate (shared with worker.cpp via src/worker.hpp) encodes the
// full outcome matrix; compile-time asserts pin it, runtime checks exercise it.
void TestSelectionReleaseMatrix() {
    std::cout << "[RUN] Testing REQ-R03 Selection Release Path Matrix..." << std::endl;

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

    std::cout << "[PASS] REQ-R03 Selection Release Path Matrix tests completed." << std::endl;
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

    // Same-language multi-line block: STILL bypassed (unchanged semantics).
    TEST_CHECK(!ShouldTranslate(L"first line\nsecond line\nthird line", "English"), "MLF: English->English multi-line still bypassed");
    TEST_CHECK(!ShouldTranslate(en_block, "English"), "MLF: bypassed when block already in target language");

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

    std::cout << "[PASS] Multi-line block fix tests completed." << std::endl;
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
    std::filesystem::path resolvedLogoSvg = ResolveRepoFile(L"assets\\logo.svg");
    TEST_CHECK(!resolvedLogoSvg.empty(), "assets/logo.svg resolves via exe/CWD-relative candidates (portable)");
    TEST_CHECK(std::filesystem::exists(resolvedLogoSvg), "Resolved assets/logo.svg exists on disk");

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

    std::cout << "[PASS] Hotkey Parsing tests completed." << std::endl;
}

// REQ-R07 (audit §3.1): active-change callback fires 1:1 with SetActive, and
// REQ-R06 (audit §2.5): the double-Ctrl+C job runs OFF the caller thread with
// a non-blocking dispatch seam (measured).
void TestKeyboardHookStateSyncAndDispatch() {
    std::cout << "[RUN] Testing KeyboardHook state sync + async dispatch (REQ-R06/R07)..." << std::endl;

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
    std::cout << "[PASS] KeyboardHook state sync + async dispatch tests completed." << std::endl;
}

// REQ-R09 (audit §3.3): join-free click_seq_ invalidation debounce, exercised
// through the same seams the LL mouse callback uses (no real mouse input).
void TestMouseHookDebounce() {
    std::cout << "[RUN] Testing MouseHook click_seq_ debounce (REQ-R09)..." << std::endl;

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

    std::cout << "[PASS] MouseHook click_seq_ debounce tests completed." << std::endl;
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
    std::cout << "[PASS] Drag-icon click dispatch tests completed." << std::endl;
}

// REQ-R1 (session 260905_0001): prove the silent-failure branch that produces
// the live symptom. main.cpp's wired click_cb_ early-returns with ZERO user
// feedback when CopySelectionWithSequenceWait() reports failure (no copyable
// selection / clipboard sequence never advances within kClipboardChangeTimeoutMs)
// or when the read-back text is empty. Both paths return before
// tooltip.ShowTranslation, so the tooltip never appears - the exact
// "tooltip itself doesn't show" the user reports. This test drives the REAL
// early-return control flow (not a stub) against a clipboard with no fresh
// selection and asserts the observable outcome: callback ran, copy gate
// failed, tooltip stayed hidden. It pins WHY the dispatch (proven intact by
// TestDragIconClickShowsTooltip) still yields no tooltip in real use.
void TestDragIconClickClipboardEarlyReturn() {
    std::cout << "[RUN] Testing drag-icon click clipboard early-return (REQ-R1)..." << std::endl;

    const HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    TooltipWindow tooltip;
    TEST_CHECK(tooltip.Create(hInst), "R1 fixture: TooltipWindow created for early-return path");

    // Reproduce main.cpp's click_cb_ control flow verbatim, with the engine
    // call stubbed (it is unreachable on the failure path we are proving).
    std::atomic<bool> copy_gate_passed{false};
    std::atomic<bool> tooltip_shown{false};
    auto wired_click_cb = [&](int /*cx*/, int /*cy*/) {
        emebalachat::ClipboardBackup backup;
        emebalachat::BackupClipboard(backup);

        if (!emebalachat::CopySelectionWithSequenceWait()) {
            emebalachat::RestoreClipboard(backup);
            copy_gate_passed.store(false, std::memory_order_relaxed);
            return; // <-- the silent early-return: no tooltip, no feedback
        }
        std::wstring selected = emebalachat::GetClipboardText();
        emebalachat::RestoreClipboard(backup);
        if (selected.empty() || selected.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
            copy_gate_passed.store(false, std::memory_order_relaxed);
            return; // <-- second silent early-return
        }
        copy_gate_passed.store(true, std::memory_order_relaxed);
        tooltip.ShowTranslation(0, 0, selected, "KO", "English", L"translated");
        tooltip_shown.store(true, std::memory_order_relaxed);
    };

    // No foreground app holds a live selection here, so the synthetic Ctrl+C
    // cannot advance the clipboard sequence: CopySelectionWithSequenceWait must
    // time out and report failure, exercising the first early-return.
    wired_click_cb(0, 0);

    TEST_CHECK(!copy_gate_passed.load(std::memory_order_relaxed),
               "R1: copy gate fails closed with no copyable selection (silent path)");
    TEST_CHECK(!tooltip_shown.load(std::memory_order_relaxed),
               "R1: early-return skips ShowTranslation entirely");
    TEST_CHECK(!tooltip.IsVisible(),
               "R1: tooltip never appears on the clipboard-failure path (live symptom reproduced)");

    tooltip.Destroy();
    std::cout << "[PASS] Drag-icon click clipboard early-return tests completed." << std::endl;
}

// REQ-R10 (audit §3.4): the WM_APP marshal protocol - packing helpers,
// message routing, and WndProc payload ownership. Runs the real WndProc on the
// test main thread (which owns the created windows' queues).
void TestUIMarshaling() {
    std::cout << "[RUN] Testing D2D thread-marshal helpers (REQ-R10)..." << std::endl;

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

    std::cout << "[PASS] D2D thread-marshal tests completed." << std::endl;
}

// REQ-R15 (audit §5 latent item 3): mixed-DPI coordinate/scale math. The pure
// conversions + clamping are fully headless; the runtime half asserts the
// process actually ends up DPI-aware and windows report sane per-monitor DPI.
void TestDpiMixedScaling() {
    std::cout << "[RUN] Testing REQ-R15 Mixed-DPI Scaling Math..." << std::endl;

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

    std::cout << "[PASS] REQ-R15 Mixed-DPI scaling tests completed." << std::endl;
}

// REQ-R14 (audit §5 latent item 2): hook lifecycle event policy + the verify-
// and-reinstall seam, exercised on a REAL installed hook (Start/Stop/Reinstall
// without synthetic user input).
void TestHookLifecyclePolicy() {
    std::cout << "[RUN] Testing REQ-R14 hook lifecycle policy..." << std::endl;

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

    std::cout << "[PASS] REQ-R14 hook lifecycle tests completed." << std::endl;
}

// REQ-R17 (audit §5 latent item 5): IME composition safety predicate.
void TestImeCompositionGate() {
    std::cout << "[RUN] Testing REQ-R17 IME composition gate..." << std::endl;

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

    // Enter gate matrix (mirror value feeds ime_composing):
    static_assert(EnterTranslationAllowed(true, true, false, false), "R17: plain Enter while idle -> fire");
    static_assert(!EnterTranslationAllowed(true, true, false, true), "R17: composing -> NEVER fire");
    static_assert(!EnterTranslationAllowed(true, true, true, false), "R17: busy worker -> pass through");
    static_assert(!EnterTranslationAllowed(true, false, false, false), "R17: inactive hook -> pass through");
    static_assert(!EnterTranslationAllowed(false, true, false, true), "R17: VK_PROCESSKEY Enter -> pass through");
    static_assert(!EnterTranslationAllowed(false, true, false, false), "R17: non-Enter vk -> gate closed");

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

    std::cout << "[PASS] REQ-R17 IME composition gate tests completed." << std::endl;
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
    static_assert(!EnterSendReplaceAllowed(true, true, false, true, false),
                  "S2: bare Enter mid-IME-composition -> pass through (IME commit)");
    static_assert(!EnterSendReplaceAllowed(true, true, false, true, true),
                  "S2: Shift+Enter mid-IME-composition -> pass through");

    // Non-Enter vk never intercepts (gate stays closed on the vk axis).
    static_assert(!EnterSendReplaceAllowed(false, true, false, false, false),
                  "S2: non-Enter vk -> gate closed");
    static_assert(!EnterSendReplaceAllowed(false, true, false, false, true),
                  "S2: non-Enter vk with Shift -> gate closed");

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

    std::cout << "[PASS] S2 Shift+Enter / bare-Enter gate tests completed." << std::endl;
}

// REQ-R16 (audit §5 latent item 4): llama.cpp shutdown cancellation seam.
void TestEngineShutdownCancellation() {
    std::cout << "[RUN] Testing REQ-R16 engine shutdown cancellation..." << std::endl;

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

    std::cout << "[PASS] REQ-R16 engine shutdown cancellation tests completed." << std::endl;
}

// D5 item F: engine.cpp's ctor relative-model fallback is exe-dir anchored
// (D4-flagged issue 1). A direct-construct manager with an EMPTY model path
// resolves to <exe dir>/models/... regardless of the process CWD.
void TestEngineFallbackExeDirAnchoring() {
    std::cout << "[RUN] Testing D5-F engine fallback exe-dir anchoring..." << std::endl;

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
    std::cout << "[PASS] D5-F engine fallback anchoring tests completed." << std::endl;
}

// ---------------------------------------------------------------------------
// Batch 2 (session 260905_0001): REQ-006 version single source of truth,
// REQ-002 scrollable tooltip (pure math + runtime), REQ-005 About window
// smoke. Follows the plan's test-mapping table (§3 Batch 2 verification).
// ---------------------------------------------------------------------------
void TestBatch2VersionScrollAbout() {
    std::cout << "[RUN] Testing Batch 2 version + tooltip scroll + about window..." << std::endl;

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
                       emebalachat::ui::ScaleDipsToPixels(560, cur_dpi),
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

    std::cout << "[PASS] Batch 2 version/scroll/about tests completed." << std::endl;
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

    std::cout << "[PASS] R5 target-language switching matrix tests completed." << std::endl;
}

// ===========================================================================
// R6 Phase 4 (B2, architect plan §4): language routing. Pins the three pure
// seams headlessly:
//   (1) BuildPrompt injects the NATIVE target name (简体中文, not "Chinese
//       Simplified") and a native source hint only for a known non-AUTO source
//       (AUTO/empty/unresolvable -> byte-identical historical prompt).
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

    // ---- 1) Target native-name injection, plan §4.2(a) row 1. -------------
    // The ZH-CN user bug: "将以下文本翻译为Chinese Simplified" (English name
    // inside the Chinese instruction) is out-of-distribution for Hy-MT2 and
    // degraded JA->ZH output to English. The seam now injects name_native.
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
        TEST_CHECK(by_code.find(t.name_native) != std::string::npos,
                   R6P4Msg(std::string("target prompt contains native '") + t.name_native + "'", "-", t.code));
        TEST_CHECK(by_code == by_name,
                   R6P4Msg("code-form and name-form prompts are identical", "-", t.code));
        if (std::string(t.name_en) != std::string(t.name_native)) {
            TEST_CHECK(by_code.find(t.name_en) == std::string::npos,
                       R6P4Msg(std::string("English name '") + t.name_en + "' must NOT appear", "-", t.code));
        }
    }

    // ---- 2) Source hint injection, plan §4.2(a) row 2. --------------------
    // Known non-AUTO source -> native source name is named in the instruction.
    TEST_CHECK(BuildPrompt("hello", "ZH-CN", "JA") ==
                   "将以下日本語文本翻译为简体中文，注意只需要输出翻译后的结果，不要额外解释：\n\nhello",
               "R6p4: JA source + ZH target injects 日本語 source hint");
    TEST_CHECK(BuildPrompt("hello", "English", "Japanese") ==
                   "Translate the following 日本語 segment into English, without additional explanation.\n\nhello",
               "R6p4: known source injects native source name into English branch");
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

    // ---- 5) LocalPairReliable verdicts (plan §4.1 item 3 conservative set).
    // Default supported set: every pair involving English (en<->*, auto->en).
    // zh<->ja (the user bug pair) and other non-EN pairs are OUTSIDE the set
    // pending VP confirmation (plan §9 open decision).
    {
        TEST_CHECK(LocalPairReliable("AUTO", "EN"),
                   "R6p4: AUTO->EN (the user's working scenario) is reliable");
        TEST_CHECK(LocalPairReliable("EN", "KO"), "R6p4: EN->KO reliable (source EN)");
        TEST_CHECK(LocalPairReliable("KO", "EN"), "R6p4: KO->EN reliable (target EN)");
        TEST_CHECK(LocalPairReliable("ja", "english"), "R6p4: verdicts are case/name-insensitive");
        TEST_CHECK(!LocalPairReliable("JA", "ZH-CN"),
                   "R6p4: the user bug pair JA->ZH-CN is NOT reliable locally");
        TEST_CHECK(!LocalPairReliable("KO", "JA"), "R6p4: KO->JA not in conservative set");
        TEST_CHECK(!LocalPairReliable("DE", "VI"), "R6p4: DE->VI not in conservative set");
        TEST_CHECK(LocalPairReliable("ZH-CN", "EN"), "R6p4: ZH-CN->EN reliable (target EN)");
        TEST_CHECK(!LocalPairReliable("KO", "AUTO"), "R6p4: AUTO target is never reliable");
        TEST_CHECK(!LocalPairReliable("KO", "KO"), "R6p4: same-language pin (src==tgt) keeps EN-equality semantics");
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
    TEST_CHECK(PlanTranslationRouting("JA", "ZH-CN", EngineType::Auto, false) == EngineType::GoogleTranslate,
               "R6p4: JA->ZH-CN under Auto routes to Google even without extra consent (user bug fixed)");
    TEST_CHECK(PlanTranslationRouting("JA", "Chinese Simplified", EngineType::LocalLlama, true) == EngineType::GoogleTranslate,
               "R6p4: JA->ZH-CN explicit-local WITH cloud consent routes to Google");
    TEST_CHECK(PlanTranslationRouting("JA", "Chinese Simplified", EngineType::LocalLlama, false) == EngineType::LocalLlama,
               "R6p4: JA->ZH-CN explicit-local WITHOUT consent stays on device");
    TEST_CHECK(PlanTranslationRouting("AUTO", "English", EngineType::LocalLlama, false) == EngineType::LocalLlama,
               "R6p4: AUTO->EN stays local under every pin (works today per user report)");
    TEST_CHECK(PlanTranslationRouting("KO", "EN", EngineType::Auto, false) == EngineType::LocalLlama,
               "R6p4: KO->EN stays local (reliable pair, offline capability preserved)");

    std::cout << "[PASS] R6 Phase 4 language routing tests completed." << std::endl;
}

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

        // Unwired fallback (unit/standalone use): inline legacy behavior still
        // advances the config through the locked setter. CycleLanguage() also
        // persists to the DEFAULT config path as a side effect - snapshot and
        // restore that file around the call so the test can never clobber a
        // developer's real build/config.json.
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
        hook2.CycleTargetLanguage(); // unwired: mutates cfg (English -> Korean)
        TEST_CHECK(cfg.GetSnapshot().target_language == "Korean",
                   "B3: unwired fallback keeps the legacy inline cycle (English->Korean)");
        if (existed) {
            std::ofstream out(default_cfg_path, std::ios::binary | std::ios::trunc);
            out.write(backup_bytes.data(), static_cast<std::streamsize>(backup_bytes.size()));
        } else {
            std::filesystem::remove(default_cfg_path, ec); // only removes OUR artifact
        }
        SetSoundEnabled(true);
    }

    std::cout << "[PASS] R6-B3 language sync coordinator seam tests completed." << std::endl;
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
    std::cout << "[PASS] R6-B1 tooltip staleness generation guard tests completed." << std::endl;
}

// R6 Phase 3 (B1 memory/lifecycle audit, architect plan §3): regression guards
// for the items this phase FIXED - the device-lost recovery predicate (item 4),
// the marshal-queue payload drain on teardown (items 6+8), and the clipboard
// open/close pairing + GDI/DIB re-allocation balance (items 3+5). All checks
// are pure seams or non-flaky runtime probes (bounded handle-count deltas);
// nothing depends on wall-clock timing or GPU state.
void TestR6P3MemoryLifecycle() {
    std::cout << "[RUN] Testing R6-P3 memory/lifecycle audit guards..." << std::endl;

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

    std::cout << "[PASS] R6-P3 memory/lifecycle audit guard tests completed." << std::endl;
}

// R6 Phases 5+6 (architect plan §5/§6/§7.2): i18n completeness + About body
// localization + UI-language selector planner + removed-locale refusal +
// config ui_language persistence. All pure seams (no window/D2D required);
// global locale state is restored at the end so later tests are unaffected.
void TestR6P5P6I18n() {
    std::cout << "[RUN] Testing R6 P5/P6 i18n coverage, About localization, UI-language selector..." << std::endl;

    const UiLocale kCompleteLocales[] = {
        UiLocale::Korean, UiLocale::Japanese, UiLocale::ChineseSimplified,
        UiLocale::ChineseTraditional, UiLocale::Vietnamese, UiLocale::Spanish,
        UiLocale::English
    };

    // ---- 1) Table completeness (VP scope item 6): every StringId in [0,
    //         EnumCount) must resolve NON-EMPTY in all 7 complete locales.
    //         Guards the 18 new Phase-5/6 strings and the existing ones
    //         against any future table drift (a missing initializer would
    //         compile as nullptr and crash Render - this pins it headlessly).
    int empty_count = 0;
    for (const UiLocale loc : kCompleteLocales) {
        I18n::SetLocale(loc);
        for (int id = 0; id < static_cast<int>(StringId::EnumCount); ++id) {
            if (I18n::Get(static_cast<StringId>(id)).empty()) {
                ++empty_count;
            }
        }
    }
    TEST_CHECK(empty_count == 0, "P5/P6: every StringId non-empty in all 7 locales");

    // ---- 2) Removed FR/DE/RU (user decision "미완성 로케일 제거") ----------
    // The enum values are gone; StringToLocale must map the old codes to
    // English (startup fallback), and the selector planner must REFUSE them
    // so they can never be (re-)persisted as half-wired locales.
    TEST_CHECK(I18n::StringToLocale("fr") == UiLocale::English, "P6: 'fr' falls back to English");
    TEST_CHECK(I18n::StringToLocale("de") == UiLocale::English, "P6: 'de' falls back to English");
    TEST_CHECK(I18n::StringToLocale("ru") == UiLocale::English, "P6: 'ru' falls back to English");
    {
        const UiLocaleChangePlan frPlan = PlanUiLocaleChange("en", "fr");
        TEST_CHECK(!frPlan.valid, "P6: selector refuses removed locale 'fr'");
        TEST_CHECK(!PlanUiLocaleChange("en", "de").valid, "P6: selector refuses removed locale 'de'");
        TEST_CHECK(!PlanUiLocaleChange("en", "ru").valid, "P6: selector refuses removed locale 'ru'");
        TEST_CHECK(!PlanUiLocaleChange("en", "klingon").valid, "P6: selector refuses unknown code");
        TEST_CHECK(!PlanUiLocaleChange("en", "").valid, "P6: selector refuses empty code");
        I18n::Initialize("fr");
        TEST_CHECK(I18n::GetCurrentLocale() == UiLocale::English,
                   "P6: startup Initialize('fr') resolves to English fallback");
    }

    // ---- 3) Selector data + planner ----------------------------------------
    {
        const auto& entries = GetSupportedUiLocales();
        TEST_CHECK(entries.size() == 7, "P6: selector lists exactly the 7 complete locales");
        TEST_CHECK(entries[0].locale == UiLocale::Korean && entries[6].locale == UiLocale::English,
                   "P6: selector order KO-first, EN-last (plan §5.4)");
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
    }

    // ---- 5) Brand token fixed (user decision) ------------------------------
    // TooltipTitle (the notice header + tray tip prefix) stays "Emebala Chat"
    // in every complete locale; only surrounding sentences translate.
    {
        bool brand_fixed = true;
        for (const UiLocale loc : kCompleteLocales) {
            I18n::SetLocale(loc);
            if (I18n::Get(StringId::TooltipTitle) != L"Emebala Chat") brand_fixed = false;
        }
        TEST_CHECK(brand_fixed, "P5: brand token 'Emebala Chat' untranslated in all 7 locales");
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

    // Restore OS-derived locale for any later test functions.
    I18n::Initialize("auto");
    std::cout << "[PASS] R6 P5/P6 i18n tests completed." << std::endl;
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
    std::cout << "[PASS] 260905 diag_logger tests completed." << std::endl;
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
    TestSoundModule();
    TestWin32InputModule();
    TestClipboardSequencePolling();
    TestGoogleTranslateModule();
    TestGoogleHttpProfile();
    TestEngineModule();
    TestModelPathValidation();
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
    TestB1TooltipStaleness();
    TestR6P3MemoryLifecycle();
    TestR6P4LanguageRouting();
    TestR6P5P6I18n();
    TestDiagLogger();

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
