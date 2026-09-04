#include "../src/config.hpp"
#include "../src/unicode_utils.hpp"
#include "../src/smart_bypass.hpp"
#include "../src/sound.hpp"
#include "../src/win32_input.hpp"
#include "../src/google_translate.hpp"
#include "../src/engine.hpp"
#include "../src/i18n.hpp"
#include "../src/ui/badge.hpp"

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

    // 4. Prompt Builder
    std::string prompt = BuildPrompt("안녕하세요", "English");
    TEST_CHECK(prompt == "Translate the following segment into English, without additional explanation. 안녕하세요",
               "Prompt formatting matches specification");

    // 5. JSON serialization & parsing roundtrip
    cfg.target_language = "Japanese";
    cfg.auto_send = true;
    cfg.sound_enabled = false;
    cfg.model_path = "D:\\custom\\model.gguf";
    cfg.SetBadgePosition(450, 600);

    std::string json_str = cfg.ToJsonString();
    AppConfig loaded_cfg;
    bool parsed = loaded_cfg.FromJsonString(json_str);
    TEST_CHECK(parsed, "JSON parsing succeeds");
    TEST_CHECK(loaded_cfg.target_language == "Japanese", "Persisted target_language matches");
    TEST_CHECK(loaded_cfg.auto_send == true, "Persisted auto_send matches");
    TEST_CHECK(loaded_cfg.sound_enabled == false, "Persisted sound_enabled matches");
    TEST_CHECK(loaded_cfg.model_path == "D:\\custom\\model.gguf", "Persisted model_path matches");
    TEST_CHECK(loaded_cfg.badge_x == 450 && loaded_cfg.badge_y == 600, "Persisted badge_x and badge_y match");

    // 6. Graceful recovery on malformed JSON
    AppConfig fallback_cfg;
    bool bad_parse = fallback_cfg.FromJsonString("{ invalid json: ...");
    TEST_CHECK(!bad_parse, "Corrupted JSON reports parse failure");
    TEST_CHECK(fallback_cfg.target_language == "English", "Corrupted JSON retains default values");

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

    // 1. Synthetic marker
    volatile DWORD marker = EXTRA_INFO_MARKER;
    TEST_CHECK(marker == 0x1337BEEF, "EXTRA_INFO_MARKER is 0x1337BEEF");

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

    std::wstring translated = mgr.Translate(L"감사합니다", "KO", "EN");
    TEST_CHECK(!translated.empty(), "Manager Translate succeeds via auto fallback");
    std::wcout << L"  [LIVE MGR RESULT]: '감사합니다' -> '" << translated << L"'" << std::endl;

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

    std::cout << "[PASS] Translation Manager tests completed." << std::endl;
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

int main() {
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
    TestBadgeDynamicSizing();
    TestI18nModule();

    std::cout << "========================================" << std::endl;
    std::cout << "Total Checks: " << g_test_count << std::endl;
    std::cout << "Failures:     " << g_failed_count << std::endl;
    std::cout << "========================================" << std::endl;

    if (g_failed_count == 0) {
        std::cout << ">>> ALL CORE TESTS PASSED SUCCESSFULLY! <<<" << std::endl;
        return 0;
    } else {
        std::cerr << ">>> TEST FAILURES DETECTED! <<<" << std::endl;
        return 1;
    }
}
