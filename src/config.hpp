#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace emebalachat {

// Represents a supported translation language with ISO code and localized names.
struct LanguageInfo {
    std::string code;           // e.g. "AUTO", "KO", "EN", "VI", "ZH-CN"
    std::string name_en;        // e.g. "Auto Detect", "Korean", "English", "Vietnamese"
    std::string name_native;    // e.g. "자동 감지", "한국어", "English", "Tiếng Việt"
    std::string display_short;  // e.g. "AUTO", "KO", "EN", "VI", "ZH-CN"
};

// Returns the full list of 38 language entries (AUTO + 37 supported languages).
const std::vector<LanguageInfo>& GetSupportedLanguages();

// Returns the list of 37 target languages (excluding AUTO).
const std::vector<LanguageInfo>& GetTargetLanguages();

// Looks up a language by its code (case-insensitive, e.g. "ko", "KO", "ZH-CN").
const LanguageInfo* FindLanguageByCode(std::string_view code);

// Looks up a language by English or native name (case-insensitive).
const LanguageInfo* FindLanguageByName(std::string_view name);

// Resolves a code or name into a canonical uppercase language code (e.g. "korean" -> "KO").
std::string NormalizeLanguageCode(std::string_view code_or_name);

// Cycles to the next target language given current code or name, wrapping around.
std::string CycleTargetLanguage(std::string_view current_code_or_name);

// Formats translation prompt for Hy-MT2 model.
std::string BuildPrompt(std::string_view source_text, std::string_view target_lang);

// Application configuration backed by JSON with zero external dependencies.
struct AppConfig {
    std::string ui_language = "auto";
    std::string engine_type = "auto";
    std::string model_path = "models/Hy-MT2-1.8B-Q8_0.gguf";
    std::string source_language = "Auto Detect";
    std::string target_language = "English";
    bool auto_send = false;
    bool sound_enabled = true;
    bool drag_to_translate = true;
    std::string drag_hotkey = "double_ctrl_c";
    int badge_x = -1;
    int badge_y = -1;
    std::string hotkey_toggle = "F9";
    std::string hotkey_lang = "Ctrl+F9";
    std::string hotkey_mode = "Ctrl+Shift+Enter";
    float temperature = 0.7f;
    float top_p = 0.6f;
    int top_k = 20;
    float repetition_penalty = 1.05f;

    void SetBadgePosition(int x, int y);

    // Returns standard default config path: config.json next to executable or working dir.
    static std::filesystem::path GetDefaultConfigPath();

    // Loads configuration from file. If file does not exist, saves defaults to disk.
    // If JSON is invalid, retains existing defaults without throwing.
    bool LoadFromFile(const std::filesystem::path& path = "");

    // Saves current configuration to JSON file.
    bool SaveToFile(const std::filesystem::path& path = "") const;

    // Serializes config to a formatted JSON string.
    std::string ToJsonString() const;

    // Deserializes config from a JSON string.
    bool FromJsonString(std::string_view json);

    // Advances target_language to the next supported language and updates state.
    std::string CycleLanguage();
};

} // namespace emebalachat
