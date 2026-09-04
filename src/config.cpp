#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <windows.h>

namespace emebalachat {

namespace {

// Helper: ASCII case-insensitive comparison
bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Complete 38-language table (AUTO + 37 translation targets)
const std::vector<LanguageInfo> kAllLanguages = {
    {"AUTO",  "Auto Detect",          "자동 감지",         "AUTO"},
    {"EN",    "English",              "English",           "EN"},
    {"KO",    "Korean",               "한국어",            "KO"},
    {"VI",    "Vietnamese",           "Tiếng Việt",        "VI"},
    {"ZH-CN", "Chinese Simplified",   "简体中文",          "ZH-CN"},
    {"ZH-TW", "Chinese Traditional",  "繁體中文",          "ZH-TW"},
    {"JA",    "Japanese",             "日本語",            "JA"},
    {"ES",    "Spanish",              "Español",           "ES"},
    {"FR",    "French",               "Français",          "FR"},
    {"DE",    "German",               "Deutsch",           "DE"},
    {"RU",    "Russian",              "Русский",           "RU"},
    {"TH",    "Thai",                 "ไทย",               "TH"},
    {"AR",    "Arabic",               "العربية",           "AR"},
    {"PT",    "Portuguese",           "Português",         "PT"},
    {"IT",    "Italian",              "Italiano",          "IT"},
    {"ID",    "Indonesian",           "Bahasa Indonesia",  "ID"},
    {"MS",    "Malay",                "Bahasa Melayu",     "MS"},
    {"FIL",   "Filipino",             "Filipino",          "FIL"},
    {"KM",    "Khmer",                "ភាសាខ្មែរ",          "KM"},
    {"LO",    "Lao",                  "ພາສາລາວ",          "LO"},
    {"HI",    "Hindi",                "हिन्दी",             "HI"},
    {"BN",    "Bengali",              "বাংলা",              "BN"},
    {"TR",    "Turkish",              "Türkçe",            "TR"},
    {"PL",    "Polish",               "Polski",            "PL"},
    {"NL",    "Dutch",                "Nederlands",        "NL"},
    {"UK",    "Ukrainian",            "Українська",        "UK"},
    {"FA",    "Persian",              "فارسی",             "FA"},
    {"UR",    "Urdu",                 "اردو",              "UR"},
    {"HE",    "Hebrew",               "עברית",             "HE"},
    {"CS",    "Czech",                "Čeština",           "CS"},
    {"HU",    "Hungarian",            "Magyar",            "HU"},
    {"SV",    "Swedish",              "Svenska",           "SV"},
    {"EL",    "Greek",                "Ελληνικά",          "EL"},
    {"RO",    "Romanian",             "Română",            "RO"},
    {"DA",    "Danish",               "Dansk",             "DA"},
    {"FI",    "Finnish",              "Suomi",             "FI"},
    {"NO",    "Norwegian",            "Norsk",             "NO"},
    {"MY",    "Burmese",              "မြန်မာစာ",          "MY"}
};

// Target languages only (skipping AUTO)
std::vector<LanguageInfo> InitTargetLanguages() {
    std::vector<LanguageInfo> targets;
    targets.reserve(kAllLanguages.size() - 1);
    for (size_t i = 1; i < kAllLanguages.size(); ++i) {
        targets.push_back(kAllLanguages[i]);
    }
    return targets;
}

// Fast JSON string escaping
std::string EscapeJsonString(std::string_view str) {
    std::string out;
    out.reserve(str.size() + 16);
    for (char c : str) {
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

// Minimal robust JSON parser for key-value dictionary
class SimpleJsonReader {
public:
    explicit SimpleJsonReader(std::string_view src) : src_(src), pos_(0) {}

    bool ParseObject(std::vector<std::pair<std::string, std::string>>& out_pairs) {
        SkipWhitespace();
        if (pos_ >= src_.size() || src_[pos_] != '{') return false;
        pos_++; // skip '{'

        while (pos_ < src_.size()) {
            SkipWhitespace();
            if (pos_ >= src_.size()) break;
            if (src_[pos_] == '}') {
                pos_++;
                return true;
            }

            // Parse key
            std::string key;
            if (!ParseString(key)) return false;

            SkipWhitespace();
            if (pos_ >= src_.size() || src_[pos_] != ':') return false;
            pos_++; // skip ':'

            SkipWhitespace();
            std::string value;
            if (pos_ >= src_.size()) return false;

            if (src_[pos_] == '\"') {
                if (!ParseString(value)) return false;
            } else if (src_[pos_] == 't' || src_[pos_] == 'f') {
                if (!ParseBool(value)) return false;
            } else {
                // Read raw primitive (number/null/etc) until comma or brace
                size_t start = pos_;
                while (pos_ < src_.size() && src_[pos_] != ',' && src_[pos_] != '}' && !std::isspace(static_cast<unsigned char>(src_[pos_]))) {
                    pos_++;
                }
                value = std::string(src_.substr(start, pos_ - start));
            }

            out_pairs.emplace_back(std::move(key), std::move(value));

            SkipWhitespace();
            if (pos_ < src_.size() && src_[pos_] == ',') {
                pos_++;
            }
        }
        return false;
    }

private:
    void SkipWhitespace() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (std::isspace(static_cast<unsigned char>(c))) {
                pos_++;
            } else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                // Line comment support
                pos_ += 2;
                while (pos_ < src_.size() && src_[pos_] != '\n') pos_++;
            } else {
                break;
            }
        }
    }

    bool ParseString(std::string& out) {
        SkipWhitespace();
        if (pos_ >= src_.size() || src_[pos_] != '\"') return false;
        pos_++; // skip opening quote
        out.clear();

        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '\"') {
                return true;
            }
            if (c == '\\') {
                if (pos_ >= src_.size()) return false;
                char esc = src_[pos_++];
                switch (esc) {
                    case '\"': out += '\"'; break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        if (pos_ + 4 > src_.size()) return false;
                        std::string hex(src_.substr(pos_, 4));
                        pos_ += 4;
                        try {
                            uint32_t code = std::stoul(hex, nullptr, 16);
                            if (code < 0x80) {
                                out += static_cast<char>(code);
                            } else if (code < 0x800) {
                                out += static_cast<char>(0xC0 | (code >> 6));
                                out += static_cast<char>(0x80 | (code & 0x3F));
                            } else {
                                out += static_cast<char>(0xE0 | (code >> 12));
                                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                                out += static_cast<char>(0x80 | (code & 0x3F));
                            }
                        } catch (...) {
                            return false;
                        }
                        break;
                    }
                    default:
                        out += esc;
                        break;
                }
            } else {
                out += c;
            }
        }
        return false;
    }

    bool ParseBool(std::string& out) {
        if (src_.substr(pos_, 4) == "true") {
            out = "true";
            pos_ += 4;
            return true;
        }
        if (src_.substr(pos_, 5) == "false") {
            out = "false";
            pos_ += 5;
            return true;
        }
        return false;
    }

    std::string_view src_;
    size_t pos_;
};

} // namespace

const std::vector<LanguageInfo>& GetSupportedLanguages() {
    return kAllLanguages;
}

const std::vector<LanguageInfo>& GetTargetLanguages() {
    static const std::vector<LanguageInfo> kTargetLanguages = InitTargetLanguages();
    return kTargetLanguages;
}

const LanguageInfo* FindLanguageByCode(std::string_view code) {
    for (const auto& lang : kAllLanguages) {
        if (EqualsIgnoreCase(lang.code, code)) {
            return &lang;
        }
    }
    return nullptr;
}

const LanguageInfo* FindLanguageByName(std::string_view name) {
    for (const auto& lang : kAllLanguages) {
        if (EqualsIgnoreCase(lang.name_en, name) || EqualsIgnoreCase(lang.name_native, name)) {
            return &lang;
        }
    }
    return nullptr;
}

std::string NormalizeLanguageCode(std::string_view code_or_name) {
    if (const auto* by_code = FindLanguageByCode(code_or_name)) {
        return by_code->code;
    }
    if (const auto* by_name = FindLanguageByName(code_or_name)) {
        return by_name->code;
    }
    return "AUTO";
}

std::string CycleTargetLanguage(std::string_view current_code_or_name) {
    const auto& targets = GetTargetLanguages();
    size_t current_idx = 0;
    bool found = false;

    for (size_t i = 0; i < targets.size(); ++i) {
        if (EqualsIgnoreCase(targets[i].code, current_code_or_name) ||
            EqualsIgnoreCase(targets[i].name_en, current_code_or_name) ||
            EqualsIgnoreCase(targets[i].name_native, current_code_or_name)) {
            current_idx = i;
            found = true;
            break;
        }
    }

    size_t next_idx = found ? ((current_idx + 1) % targets.size()) : 0;
    // Prefer returning English name for configuration and display consistency
    return targets[next_idx].name_en;
}

namespace {
bool IsChineseLanguage(std::string_view lang) {
    if (EqualsIgnoreCase(lang, "ZH") || EqualsIgnoreCase(lang, "ZH-CN") || EqualsIgnoreCase(lang, "ZH-TW")) {
        return true;
    }
    std::string lower;
    lower.reserve(lang.size());
    for (char c : lang) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lower.find("chinese") != std::string::npos || lower.rfind("zh", 0) == 0) {
        return true;
    }
    if (lang.find("中文") != std::string_view::npos) {
        return true;
    }
    return false;
}
} // namespace

std::string BuildPrompt(std::string_view source_text, std::string_view target_lang) {
    if (IsChineseLanguage(target_lang)) {
        std::string prompt = "将以下文本翻译为";
        prompt.append(target_lang);
        prompt.append("，注意只需要输出翻译后的结果，不要额外解释：\n\n");
        prompt.append(source_text);
        return prompt;
    }

    std::string prompt = "Translate the following segment into ";
    prompt.append(target_lang);
    prompt.append(", without additional explanation.\n\n");
    prompt.append(source_text);
    return prompt;
}

std::filesystem::path AppConfig::GetDefaultConfigPath() {
    wchar_t exe_path[MAX_PATH] = {0};
    if (::GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
        std::filesystem::path path(exe_path);
        return path.parent_path() / "config.json";
    }
    return std::filesystem::current_path() / "config.json";
}

bool AppConfig::LoadFromFile(const std::filesystem::path& path) {
    std::filesystem::path target_path = path.empty() ? GetDefaultConfigPath() : path;

    if (!std::filesystem::exists(target_path)) {
        // Auto-create config file with defaults
        SaveToFile(target_path);
        return true;
    }

    std::ifstream file(target_path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return FromJsonString(buffer.str());
}

bool AppConfig::SaveToFile(const std::filesystem::path& path) const {
    std::filesystem::path target_path = path.empty() ? GetDefaultConfigPath() : path;

    try {
        if (target_path.has_parent_path()) {
            std::filesystem::create_directories(target_path.parent_path());
        }

        std::filesystem::path tmp_path = target_path;
        tmp_path += ".tmp";

        {
            std::ofstream file(tmp_path, std::ios::out | std::ios::trunc | std::ios::binary);
            if (!file.is_open()) {
                return false;
            }

            std::string json_data = ToJsonString();
            file.write(json_data.data(), json_data.size());
            file.flush();
            if (!file.good()) {
                return false;
            }
        }

        // Atomic swap replacing target file safely
        if (::MoveFileExW(tmp_path.c_str(), target_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }

        // Fallback standard rename
        std::error_code ec;
        std::filesystem::rename(tmp_path, target_path, ec);
        return !ec;
    } catch (...) {
        return false;
    }
}

std::string AppConfig::ToJsonString() const {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"ui_language\": \"" << EscapeJsonString(ui_language) << "\",\n";
    ss << "  \"engine_type\": \"" << EscapeJsonString(engine_type) << "\",\n";
    ss << "  \"model_path\": \"" << EscapeJsonString(model_path) << "\",\n";
    ss << "  \"source_language\": \"" << EscapeJsonString(source_language) << "\",\n";
    ss << "  \"target_language\": \"" << EscapeJsonString(target_language) << "\",\n";
    ss << "  \"auto_send\": " << (auto_send ? "true" : "false") << ",\n";
    ss << "  \"sound_enabled\": " << (sound_enabled ? "true" : "false") << ",\n";
    ss << "  \"drag_to_translate\": " << (drag_to_translate ? "true" : "false") << ",\n";
    ss << "  \"drag_hotkey\": \"" << EscapeJsonString(drag_hotkey) << "\",\n";
    ss << "  \"hotkey_toggle\": \"" << EscapeJsonString(hotkey_toggle) << "\",\n";
    ss << "  \"hotkey_lang\": \"" << EscapeJsonString(hotkey_lang) << "\",\n";
    ss << "  \"hotkey_mode\": \"" << EscapeJsonString(hotkey_mode) << "\",\n";
    ss << "  \"temperature\": " << temperature << ",\n";
    ss << "  \"top_p\": " << top_p << ",\n";
    ss << "  \"top_k\": " << top_k << ",\n";
    ss << "  \"repetition_penalty\": " << repetition_penalty << ",\n";
    ss << "  \"badge_x\": " << badge_x << ",\n";
    ss << "  \"badge_y\": " << badge_y << "\n";
    ss << "}\n";
    return ss.str();
}

bool AppConfig::FromJsonString(std::string_view json) {
    SimpleJsonReader reader(json);
    std::vector<std::pair<std::string, std::string>> pairs;
    if (!reader.ParseObject(pairs)) {
        return false; // Retain defaults on parse failure
    }

    for (const auto& [k, v] : pairs) {
        if (k == "ui_language") {
            ui_language = v;
        } else if (k == "engine_type") {
            engine_type = v;
        } else if (k == "model_path") {
            model_path = v;
        } else if (k == "source_language") {
            source_language = v;
        } else if (k == "target_language") {
            target_language = v;
        } else if (k == "auto_send") {
            auto_send = (v == "true");
        } else if (k == "sound_enabled") {
            sound_enabled = (v == "true");
        } else if (k == "drag_to_translate") {
            drag_to_translate = (v == "true");
        } else if (k == "drag_hotkey") {
            drag_hotkey = v;
        } else if (k == "badge_x") {
            try { badge_x = std::stoi(v); } catch (...) {}
        } else if (k == "badge_y") {
            try { badge_y = std::stoi(v); } catch (...) {}
        } else if (k == "hotkey_toggle") {
            hotkey_toggle = v;
        } else if (k == "hotkey_lang") {
            hotkey_lang = v;
        } else if (k == "hotkey_mode") {
            hotkey_mode = v;
        } else if (k == "temperature") {
            try { temperature = std::stof(v); } catch (...) {}
        } else if (k == "top_p") {
            try { top_p = std::stof(v); } catch (...) {}
        } else if (k == "top_k") {
            try { top_k = std::stoi(v); } catch (...) {}
        } else if (k == "repetition_penalty") {
            try { repetition_penalty = std::stof(v); } catch (...) {}
        }
    }
    return true;
}

void AppConfig::SetBadgePosition(int x, int y) {
    badge_x = x;
    badge_y = y;
}

std::string AppConfig::CycleLanguage() {
    target_language = CycleTargetLanguage(target_language);
    SaveToFile();
    return target_language;
}

} // namespace emebalachat
