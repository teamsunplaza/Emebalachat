#include "config.hpp"

#include "i18n.hpp"
#include "unicode_utils.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <windows.h>
// REQ-029-B: SHGetKnownFolderPath(FOLDERID_LocalAppData) for the single-source
// config path. objbase.h for CoTaskMemFree (shell32/ole32 already linked in
// CMakeLists for Emebalachat_core).
#include <shlobj.h>
#include <objbase.h>

namespace emebalachat {

namespace {

// REF-3.1 (session 260910_0006 T2): the former file-local EqualsIgnoreCase
// (std::tolower(static_cast<unsigned char>(c)) idiom) is replaced at every
// call site below by the shared EqualsIgnoreCaseAscii<char> template in
// unicode_utils.hpp. Equivalent under this project's default "C" CRT locale
// (no setlocale call exists in src/): tolower mapped only 'A'-'Z' there,
// exactly the template's +32 fold; high bytes (>= 0x80, e.g. the UTF-8
// name_native values in the table below) pass through both unchanged and
// compare byte-for-byte.
// Complete 38-language table (AUTO + 37 translation targets).
// bcp47 (P4 Batch B-2, design §3 B-2 note): canonical BCP-47 tag fed to
// DWrite CreateTextFormat(localeName) for script-appropriate font fallback
// (REQ-038/REQ-037 font lever, §2-Q5 verdict A). Language subtags per
// ISO 639-1 (fil = Filipino, he = Hebrew, my = Burmese, no = Norwegian);
// zh carries the explicit script region (zh-CN/zh-TW, mirroring the config
// code). AUTO = "en" pivot. All 38 values verified accepted by DWrite on
// SDK 10.0.26100 (B-2 probe: tools_tmp_b2_dwrite_probe.cpp, probe_b2_run.log).
// NOTE: the UI-chrome counterpart is B-3's i18n LocaleMapping.bcp47_full
// (UiLocale-keyed); do not merge the two tables (two owners, two purposes).
const std::vector<LanguageInfo> kAllLanguages = {
    {"AUTO",  "Auto Detect",          "자동 감지",         "AUTO",  "en"},
    {"EN",    "English",              "English",           "EN",    "en"},
    {"KO",    "Korean",               "한국어",            "KO",    "ko"},
    {"VI",    "Vietnamese",           "Tiếng Việt",        "VI",    "vi"},
    {"ZH-CN", "Chinese Simplified",   "简体中文",          "ZH-CN", "zh-CN"},
    {"ZH-TW", "Chinese Traditional",  "繁體中文",          "ZH-TW", "zh-TW"},
    {"JA",    "Japanese",             "日本語",            "JA",    "ja"},
    {"ES",    "Spanish",              "Español",           "ES",    "es"},
    {"FR",    "French",               "Français",          "FR",    "fr"},
    {"DE",    "German",               "Deutsch",           "DE",    "de"},
    {"RU",    "Russian",              "Русский",           "RU",    "ru"},
    {"TH",    "Thai",                 "ไทย",               "TH",    "th"},
    {"AR",    "Arabic",               "العربية",           "AR",    "ar"},
    {"PT",    "Portuguese",           "Português",         "PT",    "pt"},
    {"IT",    "Italian",              "Italiano",          "IT",    "it"},
    {"ID",    "Indonesian",           "Bahasa Indonesia",  "ID",    "id"},
    {"MS",    "Malay",                "Bahasa Melayu",     "MS",    "ms"},
    {"FIL",   "Filipino",             "Filipino",          "FIL",   "fil"},
    {"KM",    "Khmer",                "ភាសាខ្មែរ",          "KM",    "km"},
    {"LO",    "Lao",                  "ພາສາລາວ",          "LO",    "lo"},
    {"HI",    "Hindi",                "हिन्दी",             "HI",    "hi"},
    {"BN",    "Bengali",              "বাংলা",              "BN",    "bn"},
    {"TR",    "Turkish",              "Türkçe",            "TR",    "tr"},
    {"PL",    "Polish",               "Polski",            "PL",    "pl"},
    {"NL",    "Dutch",                "Nederlands",        "NL",    "nl"},
    {"UK",    "Ukrainian",            "Українська",        "UK",    "uk"},
    {"FA",    "Persian",              "فارسی",             "FA",    "fa"},
    {"UR",    "Urdu",                 "اردو",              "UR",    "ur"},
    {"HE",    "Hebrew",               "עברית",             "HE",    "he"},
    {"CS",    "Czech",                "Čeština",           "CS",    "cs"},
    {"HU",    "Hungarian",            "Magyar",            "HU",    "hu"},
    {"SV",    "Swedish",              "Svenska",           "SV",    "sv"},
    {"EL",    "Greek",                "Ελληνικά",          "EL",    "el"},
    {"RO",    "Romanian",             "Română",            "RO",    "ro"},
    {"DA",    "Danish",               "Dansk",             "DA",    "da"},
    {"FI",    "Finnish",              "Suomi",             "FI",    "fi"},
    {"NO",    "Norwegian",            "Norsk",             "NO",    "no"},
    {"MY",    "Burmese",              "မြန်မာစာ",          "MY",    "my"}
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
                        // Reads the next 4 hex digits into out_code; false on
                        // truncation or non-hex input.
                        auto read_hex4 = [this](uint32_t& out_code) -> bool {
                            if (pos_ + 4 > src_.size()) return false;
                            const std::string hex(src_.substr(pos_, 4));
                            pos_ += 4;
                            try {
                                out_code = std::stoul(hex, nullptr, 16);
                            } catch (...) {
                                return false;
                            }
                            return true;
                        };

                        uint32_t code = 0;
                        if (!read_hex4(code)) return false;

                        // I2 fix: decode UTF-16 surrogate pairs (high D800-DBFF
                        // followed by low DC00-DFFF) into the real code point,
                        // matching google_translate.cpp. A LONE surrogate is not
                        // valid scalar Unicode; encoding it would emit corrupt
                        // WTF-8 bytes. Replace lone surrogates with U+FFFD so the
                        // output is always well-formed UTF-8.
                        if (code >= 0xD800 && code <= 0xDBFF) {
                            bool paired = false;
                            if (pos_ + 6 <= src_.size() && src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                                const size_t save_pos = pos_;
                                pos_ += 2; // step over "\u"
                                uint32_t low = 0;
                                if (read_hex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                                    paired = true;
                                } else {
                                    pos_ = save_pos; // not a valid pair; re-parse next escape normally
                                }
                            }
                            if (!paired) {
                                code = 0xFFFD;
                            }
                        } else if (code >= 0xDC00 && code <= 0xDFFF) {
                            code = 0xFFFD; // lone low surrogate
                        }

                        if (code < 0x80) {
                            out += static_cast<char>(code);
                        } else if (code < 0x800) {
                            out += static_cast<char>(0xC0 | (code >> 6));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        } else if (code < 0x10000) {
                            out += static_cast<char>(0xE0 | (code >> 12));
                            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        } else {
                            // 4-byte UTF-8 for supplementary-plane code points
                            out += static_cast<char>(0xF0 | (code >> 18));
                            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (code & 0x3F));
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
        if (EqualsIgnoreCaseAscii<char>(lang.code, code)) {
            return &lang;
        }
    }
    return nullptr;
}

const LanguageInfo* FindLanguageByName(std::string_view name) {
    for (const auto& lang : kAllLanguages) {
        if (EqualsIgnoreCaseAscii<char>(lang.name_en, name) ||
            EqualsIgnoreCaseAscii<char>(lang.name_native, name)) {
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

std::optional<std::string> ResolveEffectiveTarget(std::string_view detected_src,
                                                   std::string_view current_tgt,
                                                   bool user_explicit_target) {
    const std::string src_code = NormalizeLanguageCode(detected_src);
    const std::string tgt_code = NormalizeLanguageCode(current_tgt);
    if (tgt_code != src_code) {
        return std::nullopt;
    }
    // F2 (session 260908_0003, verify 220310 option (b)): a USER-EXPLICIT
    // target bypasses the pivot entirely. The ADR-A1-7 rank ladder exists to
    // stop stale AUTO defaults from producing a meaningless same-language
    // request (V6 KO->KO corruption); it must never overwrite a value the
    // user deliberately chose. Returning nullopt here means "keep the
    // original target" - the caller then runs the engine with src==tgt
    // (Google passthrough / local empty result), the exact semantics the
    // ADR-A1-7 rank-3 skip already established for EN-OS hosts.
    if (user_explicit_target) {
        return std::nullopt;
    }
    // ADR-A1-7 (session 260908_0002): language-neutral pivot. The former
    // `src_code == "EN" ? "Korean" : "English"` hardcoding privileged the
    // KO<->EN pair for every collision (a Vietnamese user dragging Vietnamese
    // text was always pivoted to English regardless of their OS language).
    // The neutral rule: (1) OS UI language when supported AND != src,
    // (2) English when src != EN, (3) no pivot (nullopt -> caller keeps the
    // original target). KO<->EN hosts keep their historical outcomes (KO OS +
    // KO src: step 1 skipped by sys==src, step 2 -> English == old behavior;
    // KO OS + EN src: step 1 -> Korean == old behavior). See ADR-A1-7
    // compatibility table; the only intended change is R7 (EN OS + EN src==tgt
    // now skips instead of pivoting to Korean).
    const std::string sys_code =
        NormalizeLanguageCode(I18n::GetSystemLanguageCode());
    if (!sys_code.empty() && sys_code != "AUTO" && sys_code != src_code) {
        if (const LanguageInfo* info = FindLanguageByCode(sys_code)) {
            return std::string(info->name_en); // 1st: OS language (!= src)
        }
    }
    if (src_code != "EN") {
        return std::string("English"); // 2nd: EN fallback (reliable pair side)
    }
    return std::nullopt; // 3rd: no meaningful alternative -> keep original tgt
}

// F3 (session 260908_0002, ADR-A1-2): single source-decision rule for the
// drag-family entry points. See config.hpp for the contract.
// NormalizeLanguageCode maps "Auto Detect"/"AUTO"/any unresolvable token to
// "AUTO", so a blank or garbage persisted value safely degrades to the
// caller's fallback instead of feeding the engine a meaningless source string.
std::string ResolveEffectiveSource(std::string_view persisted_src,
                                   std::string_view detected_or_fallback) {
    if (NormalizeLanguageCode(persisted_src) != "AUTO") {
        return std::string(persisted_src); // user-pinned: detection is ignored
    }
    return std::string(detected_or_fallback); // AUTO: explicit detected source
}

// Phase 3 (REQ-007, plan §1.4/§2.6): drag-context default target. Queries the
// OS system language through the same read-only I18n seam as
// ResolveEffectiveTarget, canonicalizes it, and maps:
//   * unsupported/unknown OS language (NormalizeLanguageCode -> "AUTO") =>
//     "English" (plan §2.6: reasonable default when "system language" is
//     impossible);
//   * OS language == English => "Korean" (EN->EN drag translation is
//     meaningless: EN<->KO pivot, matching the ResolveEffectiveTarget policy);
//   * any other supported language => its canonical name_en.
// Pure: no config mutation, no Win32 message traffic, safe from any thread.
std::string ResolveDragDefaultTarget() {
    const std::string sys_code =
        NormalizeLanguageCode(I18n::GetSystemLanguageCode());
    const LanguageInfo* info = FindLanguageByCode(sys_code);
    if (!info || info->code == "AUTO") {
        return "English"; // unsupported/unknown OS language
    }
    return info->name_en;
}

// Phase 4 (REQ-020, plan §1.4): the four system-default language values, as a
// pure function of the OS locale. Reuses ResolveDragDefaultTarget (Phase 3
// §2.1: the drag default lives in exactly one place) plus the fixed defaults
// of REQ-006/015/016. The About-window reset coordinator (main.cpp, Batch 3)
// rewrites the config fields with this result; the reset NEVER deletes keys
// (Phase 3 §2.3 contract: ToJsonStringLocked always writes the four keys, so
// a reset must be a re-record, not an erase).
SystemDefaultLanguages ComputeSystemDefaultLanguages() {
    SystemDefaultLanguages defs;
    defs.drag_source = "Auto Detect";            // REQ-006
    defs.drag_target = ResolveDragDefaultTarget(); // REQ-007 (OS lang)
    defs.type_source = "Auto Detect";            // REQ-015
    defs.type_target = "English";                // REQ-016
    return defs;
}

std::string CycleTargetLanguage(std::string_view current_code_or_name) {
    const auto& targets = GetTargetLanguages();
    size_t current_idx = 0;
    bool found = false;

    for (size_t i = 0; i < targets.size(); ++i) {
        if (EqualsIgnoreCaseAscii<char>(targets[i].code, current_code_or_name) ||
            EqualsIgnoreCaseAscii<char>(targets[i].name_en, current_code_or_name) ||
            EqualsIgnoreCaseAscii<char>(targets[i].name_native, current_code_or_name)) {
            current_idx = i;
            found = true;
            break;
        }
    }

    size_t next_idx = found ? ((current_idx + 1) % targets.size()) : 0;
    // Prefer returning English name for configuration and display consistency
    return targets[next_idx].name_en;
}

// ---- R6 Phase 1 (B3): pure language-sync planner (architect plan §2.4/§2.5) ----
namespace {
// Resolve a language token exactly the way the config layer already does for
// CycleTargetLanguage: canonical code first (case-insensitive), then English
// or native name. nullptr when the token matches nothing.
const LanguageInfo* ResolveLanguageInfo(std::string_view token) {
    if (const auto* by_code = FindLanguageByCode(token)) {
        return by_code;
    }
    return FindLanguageByName(token);
}
} // namespace

// Phase 3 (plan §2.4/§3-Batch1): the context overload resolves a PAIR and is
// deliberately context-AGNOSTIC - the planner output drives whichever pair the
// coordinator persists, and surface_updates filtering per context happens in
// src/main.cpp ApplyLanguageSync coordinator (Batch 2). The pre-Phase-3
// 4-argument form is an inline wrapper in config.hpp delegating to Type, so
// existing callers (main.cpp, TestB3LanguageSync) keep identical behavior.
LanguageSyncPlan PlanLanguageSync(LanguageContext /*ctx*/,
                                  std::string_view cur_source,
                                  std::string_view cur_target,
                                  std::string_view new_source,
                                  std::string_view new_target) {
    LanguageSyncPlan plan; // valid=false, changed=false, no surfaces

    // Canonicalize the CURRENT pair. An unresolvable persisted value (e.g. a
    // hand-edited garbage config) is carried through RAW instead of poisoning
    // the whole mutation - the surfaces normalize for display via
    // NormalizeLanguageCode() anyway, and a later valid write self-heals it.
    const LanguageInfo* cur_src_info = ResolveLanguageInfo(cur_source);
    const LanguageInfo* cur_tgt_info = ResolveLanguageInfo(cur_target);
    std::string canon_src = cur_src_info ? cur_src_info->name_en : std::string(cur_source);
    std::string canon_tgt = cur_tgt_info ? cur_tgt_info->name_en : std::string(cur_target);

    // Empty request = keep the (canonicalized) current value. A non-empty
    // request MUST resolve: all-or-nothing, so a half-applied swap or a typo'd
    // menu name can never leave config and views disagreeing (INV-1 guard).
    // On refusal the CURRENT pair is echoed back untouched (valid=false tells
    // the coordinator to persist nothing and refresh nothing).
    if (!new_source.empty()) {
        const LanguageInfo* req = ResolveLanguageInfo(new_source);
        if (!req) {
            plan.source_language = std::string(cur_source);
            plan.target_language = std::string(cur_target);
            return plan; // invalid: persist nothing, refresh nothing
        }
        canon_src = req->name_en; // source accepts AUTO ("Auto Detect")
    }
    if (!new_target.empty()) {
        const LanguageInfo* req = ResolveLanguageInfo(new_target);
        if (!req || req->code == "AUTO") {
            // AUTO is never a translation TARGET (the tooltip/tray menus only
            // offer GetTargetLanguages()); refuse instead of storing a no-op.
            plan.source_language = std::string(cur_source);
            plan.target_language = std::string(cur_target);
            return plan;
        }
        canon_tgt = req->name_en;
    }

    plan.source_language = std::move(canon_src);
    plan.target_language = std::move(canon_tgt);
    plan.valid = true;
    // changed compares the canonical result against the RAW persisted strings,
    // so a persisted "EN"/"korean"-style value is rewritten into the canonical
    // name_en form the rest of the app stores (CycleTargetLanguage precedent).
    plan.changed = (plan.source_language != std::string(cur_source) ||
                    plan.target_language != std::string(cur_target));
    // Every valid mutation refreshes all three views (plan §2.4): the tooltip
    // seam itself no-ops when hidden, so listing it unconditionally is safe
    // and keeps the sync path single (no caller-side branching to drift).
    plan.surface_updates = { LanguageSurface::Badge, LanguageSurface::Tray,
                             LanguageSurface::Tooltip };
    return plan;
}

namespace {
bool IsChineseLanguage(std::string_view lang) {
    if (EqualsIgnoreCaseAscii<char>(lang, "ZH") ||
        EqualsIgnoreCaseAscii<char>(lang, "ZH-CN") ||
        EqualsIgnoreCaseAscii<char>(lang, "ZH-TW")) {
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

// R6 Phase 4 (B2, architect plan §4.1 item 1) + Task 1 (session 260910_0004):
// resolve a language token (ISO code / English name / native name) to its
// registry entry. BuildPrompt picks the name FORM per instruction template:
// the Chinese branch keeps name_native (B2-H1: an English name inside the
// localized 将以下… instruction is out-of-distribution and degraded non-EN
// targets), while the English branch injects name_en (native names there were
// code-switching: "into 한국어" / "into Deutsch", which degraded the small
// local model's translation quality). Returns nullptr for unresolvable tokens
// with an empty out_code so historical prompts (e.g. the bare word "Chinese")
// stay byte-identical via raw injection.
const LanguageInfo* ResolvePromptLanguage(std::string_view token, std::string& out_code) {
    if (const auto* info = FindLanguageByCode(token)) {
        out_code = info->code;
        return info;
    }
    if (const auto* info = FindLanguageByName(token)) {
        out_code = info->code;
        return info;
    }
    out_code.clear();
    return nullptr;
}
} // namespace

std::string BuildPrompt(std::string_view source_text,
                        std::string_view target_lang,
                        std::string_view source_lang) {
    std::string tgt_code;
    const LanguageInfo* tgt_info = ResolvePromptLanguage(target_lang, tgt_code);
    std::string tgt_native;
    std::string tgt_en;
    if (tgt_info != nullptr && tgt_code != "AUTO") {
        tgt_native = tgt_info->name_native;
        tgt_en     = tgt_info->name_en;
    } else {
        // AUTO or unresolvable token: keep the historical raw injection.
        // AUTO is never a translation target (its native "자동 감지" must not
        // leak into the instruction), and unresolvable tokens (e.g. the bare
        // word "Chinese") preserve their byte-identical historical prompts.
        tgt_code.clear();
        tgt_native = std::string(target_lang);
        tgt_en     = tgt_native;
    }

    // R6 Phase 4 (B2-H2, plan §4.1 item 2): explicit source hint ONLY when the
    // token resolves to a real (non-AUTO) language. Empty / AUTO / unresolvable
    // sources add no token, so the AUTO prompt stays byte-identical to the
    // historical form (plan §4.2 backward-compatibility requirement).
    std::string src_code;
    std::string src_native;
    std::string src_en;
    if (!source_lang.empty()) {
        const LanguageInfo* src_info = ResolvePromptLanguage(source_lang, src_code);
        if (src_info != nullptr && src_code != "AUTO") {
            src_native = src_info->name_native;
            src_en     = src_info->name_en;
        }
    }

    // Chinese-target instruction branch: canonical codes when the token
    // resolved, else the legacy raw-token sniff keeps historical forms (e.g.
    // BuildPrompt(text, "Chinese") -> untranslated "Chinese" injection).
    const bool zh_instruction =
        (tgt_code == "ZH-CN" || tgt_code == "ZH-TW") ||
        (tgt_code.empty() && IsChineseLanguage(tgt_native));

    if (zh_instruction) {
        // Plan §4.1 template: 将以下[<source>文本]翻译为<target-native>，…
        // Task 1 (session 260910_0004): this branch KEEPS the native name
        // forms — its output stays byte-identical to the R6 Phase 4 policy.
        std::string prompt = "将以下";
        if (!src_native.empty()) {
            prompt.append(src_native);
        }
        prompt.append("文本翻译为");
        prompt.append(tgt_native);
        prompt.append("，注意只需要输出翻译后的结果，不要额外解释：\n\n");
        prompt.append(source_text);
        return prompt;
    }

    // Task 1 (session 260910_0004): the English instruction carries the
    // ENGLISH name (name_en) on both sides. Injecting name_native here caused
    // code-switching ("Translate the following segment into 한국어"), which
    // degraded the small (1.8B) local model's translation quality.
    std::string prompt = "Translate the following ";
    if (!src_en.empty()) {
        prompt.append(src_en);
        prompt.push_back(' ');
    }
    prompt.append("segment into ");
    prompt.append(tgt_en);
    prompt.append(", without additional explanation.\n\n");
    prompt.append(source_text);
    return prompt;
}

// R6 Phase 4 (B2, architect plan §4.1 item 3): supported-pair policy for the
// LOCAL Hy-MT2 engine (see config.hpp doc).
//
// F1 companion fix (session 260908_0003, verify 220010 §5 item 4): an AUTO
// SOURCE is reliable to EVERY target. Under src=Auto the local Hy-MT2 model
// decides the source language with its own built-in language ID - the exact
// mechanism the Latin-script AUTO pass-through feeds it (DetectLanguage
// returns "Auto Detect" for Latin instead of a forced language). Without this
// clause, Latin Auto -> non-EN targets (e.g. Portuguese -> Korean) would
// normalize AUTO, fail the EN-side gate, and be flagged "outside the reliable
// set": the 041 branch would ship the text to Google (privacy regression +
// wrong "VI -> KO" pair churn).
//
// F5 Phase 2 (session 260908_0003, ask audit 181530 Inquiry 3 adjudicated):
// the EN-side conservative rule for PINNED sources is REMOVED. A pinned
// source is ground truth (the user declared it), so pair reliability is
// evaluated on the pair itself: Hy-MT2 is a multilingual model whose registry
// covers all 37 languages as source/target names (BuildPrompt names both sides
// in the template's form: name_native in the Chinese branch, name_en in the
// English branch — Task 1, session 260910_0004), and the user SCOPE DIRECTIVE ("Hy-MT2에서
// 지원하는 모든 언어쌍을 정확하게 100% 지원") forbids an English-centric gate
// that pushed explicitly pinned non-EN pairs (VI->KO, JA->ZH-CN, KO->JA...)
// onto the 041 cloud-leak path or the 042 degraded-local route. Result: every
// pair with a REAL target is reliable locally. The identity pair src == tgt
// included - served on-device (echo): the pre-F5 rule made EN->EN the only
// identity pair that never left the device, and generalizing that privacy
// property to all 37 languages removes the last English privilege (the
// typing path never routes identity anyway; ShouldTranslate bypasses it
// first, so the echo only serves pinned-target drag requests). The only
// remaining false verdicts are meaningless targets: empty or AUTO (auto-detect
// is never a translation target). Unresolvable source tokens normalize to
// AUTO via NormalizeLanguageCode, so they keep the F1 AUTO-source guard
// (always local, model's built-in language ID decides).
bool LocalPairReliable(std::string_view src_code, std::string_view tgt_code) {
    const std::string src = NormalizeLanguageCode(src_code);
    const std::string tgt = NormalizeLanguageCode(tgt_code);
    if (tgt.empty() || tgt == "AUTO") {
        return false; // auto-detect is never a translation target
    }
    if (src == "AUTO") {
        return true; // model's built-in language ID handles any real target (F1)
    }
    return true; // pinned real pair (identity included, on-device echo): reliable (F5)
}

namespace {

// Lowercase an ASCII-only byte sequence for path-containment comparison
// (mirrors LowerAscii in src/engine.cpp; non-ASCII bytes are passed through so
// UTF-8 stays byte-compatible for the prefix test).
std::string LowerPathAscii(std::string s) {
    for (char& c : s) {
        if (static_cast<unsigned char>(c) < 0x80) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return s;
}

// True when `joined` (already lexically normalised) stays inside `base`
// (already normalised). Same containment rule IsValidModelPath applies at
// validation time, so absolutizing a relative path here cannot silently widen
// the path-traversal guard that commit 46ff978 (M3 security) introduced.
bool PathInsideBase(const std::filesystem::path& joined,
                    const std::filesystem::path& base) {
    std::string j = LowerPathAscii(joined.generic_string());
    std::string b = LowerPathAscii(base.generic_string());
    while (!b.empty() && b.back() == '/') {
        b.pop_back();
    }
    return (j == b) ||
           (j.size() > b.size() && j.compare(0, b.size(), b) == 0 && j[b.size()] == '/');
}

} // namespace

// REQ-R11 (audit §4 M3): directory of the running executable. Shared seam for
// GetDefaultConfigPath() and ResolveModelPath(): anything launched with a CWD
// that is NOT the install directory (Run-registry autostart uses
// C:\Windows\System32) must anchor relative paths here, never at the CWD.
std::filesystem::path GetExecutableDir() {
    wchar_t exe_path[MAX_PATH] = {0};
    const DWORD n = ::GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::filesystem::path dir = std::filesystem::path(exe_path).parent_path();
        if (!dir.empty()) {
            return dir;
        }
    }
    // Fallback only when the module-path query itself fails (or truncates):
    // the previous CWD-based behavior. current_path() can also fail; return an
    // empty path rather than throwing so callers can detect "no anchor".
    std::error_code ec;
    std::filesystem::path cwd = std::filesystem::current_path(ec);
    return ec ? std::filesystem::path{} : cwd;
}

// REQ-R11 (audit §4 M3): pure path arithmetic, no disk access (see header doc).
// UTF-8 in, UTF-8 out: the raw strings are widened via the repo's CP_UTF8
// ToUtf16 seam, joined in the wide domain (where the exe directory is already
// lossless from GetModuleFileNameW), and folded back with ToUtf8. This matches
// how TranslationManager/llama.cpp consume the path (UTF-8 std::string) and
// never reintroduces a lossy ANSI-code-page conversion.
std::string ResolveModelPath(std::string_view raw_path,
                             std::string_view base_dir) {
    if (raw_path.empty()) {
        return {};
    }

    const std::filesystem::path raw{ToUtf16(raw_path)};
    if (raw.is_absolute()) {
        // Absolute paths define their own location; only collapse '.'/'..'.
        return ToUtf8(raw.lexically_normal().native());
    }

    std::filesystem::path base;
    if (!base_dir.empty()) {
        base = std::filesystem::path{ToUtf16(base_dir)}.lexically_normal();
    } else {
        base = GetExecutableDir().lexically_normal();
    }
    if (base.empty()) {
        // No anchor available (module path AND CWD both unresolvable): keep the
        // legacy CWD-relative meaning instead of inventing a wrong directory.
        return ToUtf8(raw.lexically_normal().native());
    }

    const std::filesystem::path joined = (base / raw).lexically_normal();
    if (!PathInsideBase(joined, base)) {
        // '..' escape: DO NOT hand back an absolute path outside the install
        // directory (that would launder a traversal past IsValidModelPath's
        // containment rule). Return the collapsed relative path so validation
        // still sees a relative path and rejects it fail-closed.
        return ToUtf8(raw.lexically_normal().native());
    }
    return ToUtf8(joined.native());
}

// REQ-029-B: config 단일 진실 경로. 설치본(Program Files, BUILTIN\Users:RX)에서는
// exe-dir 쓰기가 불가하므로 %LOCALAPPDATA%\Emebalachat\config.json으로 통일한다.
// 실패 시 empty 반환 → 호출자(GetDefaultConfigPath)가 exe-dir 폐백.
std::filesystem::path AppConfig::GetLocalAppDataConfigPath() {
    PWSTR known = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known))) {
        result = std::filesystem::path(known) / "Emebalachat" / "config.json";
        ::CoTaskMemFree(known);
    }
    return result;  // 실패 시 empty → 호출자가 exe-dir 폐백
}

std::filesystem::path AppConfig::GetDefaultConfigPath() {
    // REQ-029-B (설계서 §2.1 3-b): %LOCALAPPDATA% 우선, empty면 exe-dir 폐백,
    // 그마저 empty면 current_path 폐백(기존 동작 호환).
    const std::filesystem::path lad = GetLocalAppDataConfigPath();
    if (!lad.empty()) {
        return lad;
    }
    const std::filesystem::path exe_dir = GetExecutableDir();
    if (!exe_dir.empty()) {
        return exe_dir / "config.json";
    }
    return std::filesystem::current_path() / "config.json";
}

bool AppConfig::LoadFromFile(const std::filesystem::path& path) {
    std::filesystem::path target_path = path.empty() ? GetDefaultConfigPath() : path;

    // REQ-029-B 마이그레이션: 신규 경로(%LOCALAPPDATA%)에 config가 없고
    // 레거시 exe-dir에만 있으면, 레거시를 읽어 신규 경로로 이관한다.
    // - 신규 경로가 이미 있으면 레거시는 무시(신규가 진실).
    // - 명시적 path 인자가 주어진 테스트/특수 호출은 마이그레이션 대상 아님.
    // - 레거시는 삭제하지 않고 읽기 전용으로 잔존시켜 롤백 여지 보존.
    // 프로세스 시작 시 main.cpp의 인자 없는 LoadFromFile() 호출 1회만 이 분기를
    // 타므로(설계서 §2.5: 스레드 생성 전, 1회, 원자적), 부작용 창은 없다.
    if (path.empty()) {  // 기본 경로 사용 시에만 마이그레이션 판단
        const std::filesystem::path exe_dir = GetExecutableDir();
        if (!exe_dir.empty()) {  // exe-dir 해석 실패 시 판단 보류(설계서 가드 강화)
            const std::filesystem::path legacy = exe_dir / "config.json";
            std::error_code ec_new, ec_legacy;
            const bool new_exists = std::filesystem::exists(target_path, ec_new);
            const bool legacy_exists = std::filesystem::exists(legacy, ec_legacy);
            const bool same_file = !legacy.empty() && !target_path.empty() &&
                                   std::filesystem::equivalent(legacy, target_path, ec_new) && !ec_new;
            if (!new_exists && legacy_exists && !same_file) {
                // 1) 레거시를 메모리로 로드
                std::ifstream lf(legacy, std::ios::in | std::ios::binary);
                if (lf.is_open()) {
                    std::stringstream buf; buf << lf.rdbuf();
                    if (FromJsonString(buf.str())) {
                        // 2) 신규 경로로 저장 (디렉터리 자동 생성은 SaveToFileLocked가 담당)
                        // 3) 레거시는 삭제하지 않고 읽기 전용으로 잔존시켜 롤백 여지 보존
                        SaveToFile(target_path);
                    }
                }
            }
        }
    }

    if (!std::filesystem::exists(target_path)) {
        // Auto-create config file with defaults.
        // Phase 3 (REQ-007, plan §1.4/§2.3): the fresh-install drag target must
        // already be the OS-resolved default, not the compile-time "English"
        // placeholder, BEFORE the save below writes the sticky keys - plan §2.3
        // pins that "the saved value equals the default", and the default is
        // defined as (auto / OS language). Without this, first launch would
        // persist "English" as sticky and REQ-007 could never take effect.
        // The type pair's compile-time defaults already equal the system
        // defaults (REQ-015/016), so only the drag pair needs resolution.
        SetDragLanguages("Auto Detect", ResolveDragDefaultTarget());
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
    std::lock_guard<std::mutex> lock(mutex_); // I4: serialize saves + field reads
    return SaveToFileLocked(target_path);
}

bool AppConfig::SaveToFileLocked(const std::filesystem::path& target_path) const {
    // Caller MUST hold mutex_ (SaveToFile). Serializing under the lock also fixes
    // the old race where two concurrent saves wrote the same shared ".tmp" file.
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

            std::string json_data = ToJsonStringLocked();
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

// I4: thread-safe snapshot of the fields shared across UI/hook/worker threads.
AppConfig::Snapshot AppConfig::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot s;
    s.engine_type = engine_type;
    s.source_language = source_language;
    s.target_language = target_language;
    s.drag_source_language = drag_source_language; // Phase 3 (I4: same lock)
    s.drag_target_language = drag_target_language;
    s.drag_target_pinned = drag_target_pinned;     // F2: provenance
    s.type_source_language = type_source_language;
    s.type_target_language = type_target_language;
    s.ui_language = ui_language; // R6 Phase 6: selector read-back
    s.auto_send = auto_send.load(std::memory_order_relaxed);
    s.sound_enabled = sound_enabled.load(std::memory_order_relaxed);
    s.drag_to_translate = drag_to_translate;
    return s;
}

// I4: locked mutators for shared strings.
void AppConfig::SetEngineTypeName(std::string value) {
    std::lock_guard<std::mutex> lock(mutex_);
    engine_type = std::move(value);
}

// R6 Phase 6: ui_language is runtime-mutable via the tray selector; same I4
// locking discipline as the engine/language setters above.
void AppConfig::SetUiLanguage(std::string value) {
    std::lock_guard<std::mutex> lock(mutex_);
    ui_language = std::move(value);
}

void AppConfig::SetSourceLanguage(std::string value) {
    std::lock_guard<std::mutex> lock(mutex_);
    source_language = std::move(value);
}

void AppConfig::SetTargetLanguage(std::string value) {
    std::lock_guard<std::mutex> lock(mutex_);
    target_language = std::move(value);
}

void AppConfig::SetLanguages(std::string source, std::string target) {
    std::lock_guard<std::mutex> lock(mutex_);
    source_language = std::move(source);
    target_language = std::move(target);
}

// Phase 3 (plan §2.3): locked pair mutators - both fields of one context
// update atomically under mutex_ so a snapshot never sees a half-applied pair.
void AppConfig::SetDragLanguages(std::string source, std::string target) {
    std::lock_guard<std::mutex> lock(mutex_);
    drag_source_language = std::move(source);
    drag_target_language = std::move(target);
}

void AppConfig::SetTypeLanguages(std::string source, std::string target) {
    std::lock_guard<std::mutex> lock(mutex_);
    type_source_language = std::move(source);
    type_target_language = std::move(target);
}

// F2 (session 260908_0003): plain bool under mutex_ (not std::atomic) - the
// same I4 discipline as the drag/type string fields it accompanies.
void AppConfig::SetDragTargetPinned(bool pinned) {
    std::lock_guard<std::mutex> lock(mutex_);
    drag_target_pinned = pinned;
}

std::string AppConfig::ToJsonString() const {
    std::lock_guard<std::mutex> lock(mutex_); // I4
    return ToJsonStringLocked();
}

std::string AppConfig::ToJsonStringLocked() const {
    // Caller MUST hold mutex_ (ToJsonString / SaveToFileLocked).
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"ui_language\": \"" << EscapeJsonString(ui_language) << "\",\n";
    ss << "  \"engine_type\": \"" << EscapeJsonString(engine_type) << "\",\n";
    ss << "  \"model_path\": \"" << EscapeJsonString(model_path) << "\",\n";
    ss << "  \"source_language\": \"" << EscapeJsonString(source_language) << "\",\n";
    ss << "  \"target_language\": \"" << EscapeJsonString(target_language) << "\",\n";
    // Phase 3 (plan §2.3): the four context keys are ALWAYS serialized; the
    // sticky distinction lives in FromJsonString's key-presence check, and the
    // defaults-vs-user values are observationally identical until mutated.
    ss << "  \"drag_source_language\": \"" << EscapeJsonString(drag_source_language) << "\",\n";
    ss << "  \"drag_target_language\": \"" << EscapeJsonString(drag_target_language) << "\",\n";
    ss << "  \"drag_target_pinned\": " << (drag_target_pinned ? "true" : "false") << ",\n";
    ss << "  \"type_source_language\": \"" << EscapeJsonString(type_source_language) << "\",\n";
    ss << "  \"type_target_language\": \"" << EscapeJsonString(type_target_language) << "\",\n";
    ss << "  \"auto_send\": " << (auto_send.load(std::memory_order_relaxed) ? "true" : "false") << ",\n";
    ss << "  \"sound_enabled\": " << (sound_enabled.load(std::memory_order_relaxed) ? "true" : "false") << ",\n";
    ss << "  \"drag_to_translate\": " << (drag_to_translate ? "true" : "false") << ",\n";
    ss << "  \"cloud_fallback_enabled\": " << (cloud_fallback_enabled ? "true" : "false") << ",\n";
    ss << "  \"diag_log_content\": " << (diag_log_content ? "true" : "false") << ",\n";
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

    const std::lock_guard<std::mutex> lock(mutex_); // I4: writes are visible to all reader threads
    bool has_new_schema = false; // Phase 3: drag_target_language key present?
    for (const auto& [k, v] : pairs) {
        if (k == "ui_language") {
            ui_language = v;
        } else if (k == "engine_type") {
            engine_type = v;
        } else if (k == "model_path") {
            model_path = v;
        } else if (k == "source_language") {
            source_language = v;      // legacy
        } else if (k == "target_language") {
            target_language = v;      // legacy
        } else if (k == "drag_source_language") {
            drag_source_language = v;
        } else if (k == "drag_target_language") {
            drag_target_language = v;
            has_new_schema = true;
        } else if (k == "drag_target_pinned") {
            // F2: user-picked target provenance. Key absent on every pre-F2
            // config => pinned stays false (AUTO default, re-pivotable),
            // which is the correct backward-compatible reading.
            drag_target_pinned = (v == "true");
        } else if (k == "type_source_language") {
            type_source_language = v;
        } else if (k == "type_target_language") {
            type_target_language = v;
        } else if (k == "auto_send") {
            auto_send.store(v == "true", std::memory_order_relaxed);
        } else if (k == "sound_enabled") {
            sound_enabled.store(v == "true", std::memory_order_relaxed);
        } else if (k == "drag_to_translate") {
            drag_to_translate = (v == "true");
        } else if (k == "cloud_fallback_enabled") {
            cloud_fallback_enabled = (v == "true");
        } else if (k == "diag_log_content") {
            // REQ-003: key absent on every pre-260909 config.json => the field
            // keeps its compile-time default false (opt-in content logging).
            diag_log_content = (v == "true");
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
    // Phase 3 migration (plan §2.1): no new-schema keys => this is a pre-Phase-3
    // config.json. RESET the four context fields to the system defaults instead
    // of copying the legacy shared pair (REQ-020 alignment: copying e.g. a
    // legacy "Korean" into type_target_language would violate REQ-016's English
    // default). The legacy source_language/target_language keys stay parsed
    // (deprecated schema members, Batch 2 removes the last runtime reads).
    // The next SaveToFile persists the new keys, so from then on the values are
    // sticky (REQ-008/009). ResolveDragDefaultTarget() locks nothing - mutex_ is
    // non-recursive but the call is safe (pure read-only locale query).
    if (!has_new_schema) {
        drag_source_language = "Auto Detect";              // REQ-006
        drag_target_language = ResolveDragDefaultTarget(); // REQ-007 (OS lang)
        drag_target_pinned = false;                         // F2: never-touched default
        type_source_language = "Auto Detect";              // REQ-015
        type_target_language = "English";                  // REQ-016
    }
    return true;
}

void AppConfig::SetBadgePosition(int x, int y) {
    std::lock_guard<std::mutex> lock(mutex_); // I4: read by badge Create on other paths
    badge_x = x;
    badge_y = y;
}

std::string AppConfig::CycleLanguage() {
    std::string next;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        next = CycleTargetLanguage(target_language);
        target_language = next;
    }
    SaveToFile(); // SaveToFile takes the lock itself; do not hold it across (non-recursive mutex)
    return next;
}

} // namespace emebalachat
