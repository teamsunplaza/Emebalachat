#include "config.hpp"

#include "diag_logger.hpp" // R1-13: shape-only line when a malformed config is preserved
#include "i18n.hpp"
#include "unicode_utils.hpp"
// 261116_0003 P9a (plan §3 D5): stage 1 of the user-language code ladder is the
// generated alias matrix's exact-match search (SearchLanguages/FoldLanguageAliasText).
#include "language_alias.hpp"
// 261116_0003 P9a (plan §3 D5/D6): the script vocabulary (kScriptMask* +
// ScriptMaskOfText/ScriptMaskForLanguageCode) is REUSED here rather than copied
// a third time. win32_input.hpp is STL+windows.h+smart_bypass only (no config
// dependency, no link dependency: smart_bypass/unicode_utils are already in
// Emebalachat_core), and its masks are the app's single script-bit definition
// that the ws2 suite pins against engine_core.
#include "win32_input.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio> // 261116_0003 P9b-3: std::snprintf for the probe's ISO-8601 UTC stamp
#include <deque>  // 261116_0003 P9c-1a: the append-only filtered-view cache
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
// Complete 90-language table (AUTO + 89 translation targets; 261116_0003:
// 38 -> 47 in P2, 47 -> 90 in P3 when tranche A+B landed).
// bcp47 (P4 Batch B-2, design §3 B-2 note): canonical BCP-47 tag fed to
// DWrite CreateTextFormat(localeName) for script-appropriate font fallback
// (REQ-038/REQ-037 font lever, §2-Q5 verdict A). Language subtags per
// ISO 639-1 (fil = Filipino, he = Hebrew, my = Burmese, no = Norwegian);
// zh carries the explicit script region (zh-CN/zh-TW, mirroring the config
// code); YUE carries the STATIC "zh-HK" tag — the 261116_0003 alias probe
// recorded the GetLocaleInfoEx(L"yue") placeholder trap (success code with a
// placeholder name), so no runtime locale probe may ever feed this column.
// AUTO = "en" pivot. All 47 pre-P3 values verified accepted by DWrite on
// SDK 10.0.26100 (B-2 probe: tools_tmp_b2_dwrite_probe.cpp, probe_b2_run.log);
// the 43 P3 tranche tags are standard lowercase ISO-639 subtags of the same
// shape, covered by the B-2 ASCII-hygiene pin (TestReq038B2RegistryBcp47).
// NOTE: the UI-chrome counterpart is B-3's i18n LocaleMapping.bcp47_full
// (UiLocale-keyed); do not merge the two tables (two owners, two purposes).
//
// engine_mask (261116_0003 P2, plan §4 / D7): per-engine offerability bits
// (kLangHyMt2/kLangGoogle/kLangOpenAi/kLangUserGguf, config.hpp). Row sets:
//   * the 29 pre-existing Hy-MT2-official rows + the 9 new rows GU/TE/MR/TA/
//     BO/KK/MN/UG/YUE carry kLangHyMt2 (Tencent's official 38-target table,
//     model card fetched 2026-11-16, plan §2);
//   * LO/HU/SV/EL/RO/DA/FI/NO stay registered as CLOUD-ONLY rows (they were
//     never in Hy-MT2's official set) — kLangGoogle|kLangOpenAi|kLangUserGguf;
//   * BO (Tibetan) carries NO kLangGoogle bit: Google's published language
//     table (docs.cloud.google.com/translate/docs/languages, page updated
//     2025-12-15 UTC, fetched 2026-11-16) lists no Tibetan in either the
//     Translation-LLM or the NMT list — the per-code determination is
//     recorded in the P2 coder report (re-verified by the P3 fresh fetch:
//     still absent; see the P3 coder report);
//   * the 43 tranche A+B rows P3 appends after YUE are CLOUD-ONLY with
//     kLangGoogle|kLangOpenAi|kLangUserGguf — none is in Hy-MT2's official
//     set, and every one of the 43 codes is present in Google's published
//     NMT table (docs.cloud.google.com/translate/docs/languages, page updated
//     2025-12-15 UTC, fetched 2026-11-16 — per-code
//     determination in the P3 coder report), so unlike BO they all keep
//     the kLangGoogle bit and MapLanguageCode needs ZERO new exceptions
//     (every new code is ASCII-lowercase-equal to its Google form);
//   * AUTO carries all four bits by design (source-only marker).
// ROW ORDER IS CYCLE-PIN LOAD-BEARING (Ctrl+F9 and the unit tests pin the
// sequence): the 9 P2 rows are APPENDED AFTER "MY" and the 43 P3 rows
// AFTER "YUE" — never reorder.
constexpr std::uint32_t kMaskLocalAndCloud =
    kLangHyMt2 | kLangGoogle | kLangOpenAi | kLangUserGguf;
constexpr std::uint32_t kMaskCloudOnly = kLangGoogle | kLangOpenAi | kLangUserGguf;
const std::vector<LanguageInfo> kAllLanguages = {
    {"AUTO",  "Auto Detect",          "자동 감지",         "AUTO",  "en",       kMaskLocalAndCloud},
    {"EN",    "English",              "English",           "EN",    "en",       kMaskLocalAndCloud},
    {"KO",    "Korean",               "한국어",            "KO",    "ko",       kMaskLocalAndCloud},
    {"VI",    "Vietnamese",           "Tiếng Việt",        "VI",    "vi",       kMaskLocalAndCloud},
    {"ZH-CN", "Chinese Simplified",   "简体中文",          "ZH-CN", "zh-CN",    kMaskLocalAndCloud},
    {"ZH-TW", "Chinese Traditional",  "繁體中文",          "ZH-TW", "zh-TW",    kMaskLocalAndCloud},
    {"JA",    "Japanese",             "日本語",            "JA",    "ja",       kMaskLocalAndCloud},
    {"ES",    "Spanish",              "Español",           "ES",    "es",       kMaskLocalAndCloud},
    {"FR",    "French",               "Français",          "FR",    "fr",       kMaskLocalAndCloud},
    {"DE",    "German",               "Deutsch",           "DE",    "de",       kMaskLocalAndCloud},
    {"RU",    "Russian",              "Русский",           "RU",    "ru",       kMaskLocalAndCloud},
    {"TH",    "Thai",                 "ไทย",               "TH",    "th",       kMaskLocalAndCloud},
    {"AR",    "Arabic",               "العربية",           "AR",    "ar",       kMaskLocalAndCloud},
    {"PT",    "Portuguese",           "Português",         "PT",    "pt",       kMaskLocalAndCloud},
    {"IT",    "Italian",              "Italiano",          "IT",    "it",       kMaskLocalAndCloud},
    {"ID",    "Indonesian",           "Bahasa Indonesia",  "ID",    "id",       kMaskLocalAndCloud},
    {"MS",    "Malay",                "Bahasa Melayu",     "MS",    "ms",       kMaskLocalAndCloud},
    {"FIL",   "Filipino",             "Filipino",          "FIL",   "fil",      kMaskLocalAndCloud},
    {"KM",    "Khmer",                "ភាសាខ្មែរ",          "KM",    "km",       kMaskLocalAndCloud},
    {"LO",    "Lao",                  "ພາສາລາວ",          "LO",    "lo",       kMaskCloudOnly},
    {"HI",    "Hindi",                "हिन्दी",             "HI",    "hi",       kMaskLocalAndCloud},
    {"BN",    "Bengali",              "বাংলা",              "BN",    "bn",       kMaskLocalAndCloud},
    {"TR",    "Turkish",              "Türkçe",            "TR",    "tr",       kMaskLocalAndCloud},
    {"PL",    "Polish",               "Polski",            "PL",    "pl",       kMaskLocalAndCloud},
    {"NL",    "Dutch",                "Nederlands",        "NL",    "nl",       kMaskLocalAndCloud},
    {"UK",    "Ukrainian",            "Українська",        "UK",    "uk",       kMaskLocalAndCloud},
    {"FA",    "Persian",              "فارسی",             "FA",    "fa",       kMaskLocalAndCloud},
    {"UR",    "Urdu",                 "اردو",              "UR",    "ur",       kMaskLocalAndCloud},
    {"HE",    "Hebrew",               "עברית",             "HE",    "he",       kMaskLocalAndCloud},
    {"CS",    "Czech",                "Čeština",           "CS",    "cs",       kMaskLocalAndCloud},
    {"HU",    "Hungarian",            "Magyar",            "HU",    "hu",       kMaskCloudOnly},
    {"SV",    "Swedish",              "Svenska",           "SV",    "sv",       kMaskCloudOnly},
    {"EL",    "Greek",                "Ελληνικά",          "EL",    "el",       kMaskCloudOnly},
    {"RO",    "Romanian",             "Română",            "RO",    "ro",       kMaskCloudOnly},
    {"DA",    "Danish",               "Dansk",             "DA",    "da",       kMaskCloudOnly},
    {"FI",    "Finnish",              "Suomi",             "FI",    "fi",       kMaskCloudOnly},
    {"NO",    "Norwegian",            "Norsk",             "NO",    "no",       kMaskCloudOnly},
    {"MY",    "Burmese",              "မြန်မာစာ",          "MY",    "my",       kMaskLocalAndCloud},
    // 261116_0003 P2: the 9 Hy-MT2-official rows the app was missing
    // (research-report §2: "Hy-MT2-only"). Appended after MY — the row order
    // above is cycle-pin load-bearing. KK is KAZAKH (ISO 639-1 kk; ky =
    // Kyrgyz lands in P3 tranche A per plan D2 — the research report, the
    // architect reports and the Hy-MT2 card all agree; see the P2 coder
    // report). name_native values are authored in each language's own
    // script (R1-1 native-name-script agreement pin, the missing_script /
    // native_script_mismatch drift-guard block in run_tests.cpp; anchored
    // by check name, not bare line numbers - those drifted twice and the
    // previous citation :18540-18558 landed on the HasSuffixRepetition
    // block. 261116_0003 P2 fix round 2b); BO's native is བོད་ཡིག (the same
    // sample the ws2 grid pins); UG's is Arabic-script (RTL —
    // bidi_utils kRtlLanguageCodes).
    {"GU",    "Gujarati",             "ગુજરાતી",           "GU",    "gu",       kMaskLocalAndCloud},
    {"TE",    "Telugu",               "తెలుగు",            "TE",    "te",       kMaskLocalAndCloud},
    {"MR",    "Marathi",              "मराठी",              "MR",    "mr",       kMaskLocalAndCloud},
    {"TA",    "Tamil",                "தமிழ்",              "TA",    "ta",       kMaskLocalAndCloud},
    {"BO",    "Tibetan",              "བོད་ཡིག",            "BO",    "bo",       kLangHyMt2 | kLangOpenAi | kLangUserGguf},
    {"KK",    "Kazakh",               "Қазақ тілі",        "KK",    "kk",       kMaskLocalAndCloud},
    {"MN",    "Mongolian",            "Монгол",            "MN",    "mn",       kMaskLocalAndCloud},
    {"UG",    "Uyghur",               "ئۇيغۇرچە",          "UG",    "ug",       kMaskLocalAndCloud},
    {"YUE",   "Cantonese",            "廣東話",            "YUE",   "zh-HK",    kMaskLocalAndCloud},
    // 261116_0003 P3 (plan §5 P3 / decision D2): tranche A+B, the 43 cloud-
    // only languages appended AFTER YUE (row order is cycle-pin load-bearing;
    // the single-argument Ctrl+F9 cycle and the unit pins traverse the full
    // 89-target list). Mask for every row: kMaskCloudOnly — none of the 43 is
    // in Hy-MT2's official 38-target set (D7: the built-in engine never sees
    // them; auto routes them to cloud through the revived LocalPairReliable),
    // and all 43 codes verified present in Google's published NMT table on the
    // 261116_0003 fetch (docs.cloud.google.com/translate/docs/languages, page
    // updated 2025-12-15 UTC, fetched 2026-11-16) — every code is
    // ASCII-lowercase-equal to its Google form, so MapLanguageCode gains ZERO
    // new exceptions. Order:
    // tranche A (26 Latin/Cyrillic reuses, D2 order) then tranche B (17: the
    // 8-script-bit languages then the reuse rows; SA/MAI/GOM = Sanskrit/
    // Maithili/Goan Konkani are the three Devanagari reuse additions, the
    // identity the engine's kLanguageScriptTable already staged). name_native
    // values are authored in each language's own script (R1-1 native-name-
    // script agreement pin). SR and UZ are dual-script rows (Cyrillic|Latin,
    // the JA Han|Kana precedent) — Serbian and Uzbek are written in both.
    // PS (پښتو) and SD (سنڌي) are Arabic-script and join kRtlLanguageCodes
    // (bidi_utils.cpp) in this unit — the UG precedent.
    // Tranche A — Latin/Cyrillic script reuse (26):
    {"BS",    "Bosnian",              "Bosanski",          "BS",    "bs",       kMaskCloudOnly},
    {"SR",    "Serbian",              "Српски",            "SR",    "sr",       kMaskCloudOnly},
    {"HR",    "Croatian",             "Hrvatski",          "HR",    "hr",       kMaskCloudOnly},
    {"SL",    "Slovenian",            "Slovenščina",       "SL",    "sl",       kMaskCloudOnly},
    {"SK",    "Slovak",               "Slovenčina",        "SK",    "sk",       kMaskCloudOnly},
    {"BG",    "Bulgarian",            "Български",         "BG",    "bg",       kMaskCloudOnly},
    {"LT",    "Lithuanian",           "Lietuvių",          "LT",    "lt",       kMaskCloudOnly},
    {"LV",    "Latvian",              "Latviešu",          "LV",    "lv",       kMaskCloudOnly},
    {"ET",    "Estonian",             "Eesti",             "ET",    "et",       kMaskCloudOnly},
    {"AF",    "Afrikaans",            "Afrikaans",         "AF",    "af",       kMaskCloudOnly},
    {"CA",    "Catalan",              "Català",            "CA",    "ca",       kMaskCloudOnly},
    {"IS",    "Icelandic",            "Íslenska",          "IS",    "is",       kMaskCloudOnly},
    {"SQ",    "Albanian",             "Shqip",             "SQ",    "sq",       kMaskCloudOnly},
    {"MK",    "Macedonian",           "Македонски",        "MK",    "mk",       kMaskCloudOnly},
    {"CY",    "Welsh",                "Cymraeg",           "CY",    "cy",       kMaskCloudOnly},
    {"GA",    "Irish",                "Gaeilge",           "GA",    "ga",       kMaskCloudOnly},
    {"EU",    "Basque",               "Euskara",           "EU",    "eu",       kMaskCloudOnly},
    {"GL",    "Galician",             "Galego",            "GL",    "gl",       kMaskCloudOnly},
    {"SW",    "Swahili",              "Kiswahili",         "SW",    "sw",       kMaskCloudOnly},
    {"HT",    "Haitian Creole",       "Kreyòl Ayisyen",    "HT",    "ht",       kMaskCloudOnly},
    {"UZ",    "Uzbek",                "Oʻzbekcha",         "UZ",    "uz",       kMaskCloudOnly},
    {"AZ",    "Azerbaijani",          "Azərbaycanca",      "AZ",    "az",       kMaskCloudOnly},
    {"BE",    "Belarusian",           "Беларуская",        "BE",    "be",       kMaskCloudOnly},
    {"KY",    "Kyrgyz",               "Кыргызча",          "KY",    "ky",       kMaskCloudOnly},
    {"TG",    "Tajik",                "Тоҷикӣ",            "TG",    "tg",       kMaskCloudOnly},
    {"EO",    "Esperanto",            "Esperanto",         "EO",    "eo",       kMaskCloudOnly},
    // Tranche B — 8 new script bits + reuse rows (17):
    {"HY",    "Armenian",             "Հայերեն",           "HY",    "hy",       kMaskCloudOnly},
    {"KA",    "Georgian",             "ქართული",           "KA",    "ka",       kMaskCloudOnly},
    {"SI",    "Sinhala",              "සිංහල",             "SI",    "si",       kMaskCloudOnly},
    {"ML",    "Malayalam",            "മലയാളം",            "ML",    "ml",       kMaskCloudOnly},
    {"KN",    "Kannada",              "ಕನ್ನಡ",             "KN",    "kn",       kMaskCloudOnly},
    {"PA",    "Punjabi",              "ਪੰਜਾਬੀ",             "PA",    "pa",       kMaskCloudOnly},
    {"OR",    "Odia",                 "ଓଡ଼ିଆ",             "OR",    "or",       kMaskCloudOnly},
    {"AM",    "Amharic",              "አማርኛ",             "AM",    "am",       kMaskCloudOnly},
    {"TI",    "Tigrinya",             "ትግርኛ",             "TI",    "ti",       kMaskCloudOnly},
    {"PS",    "Pashto",               "پښتو",              "PS",    "ps",       kMaskCloudOnly},
    {"SD",    "Sindhi",               "سنڌي",              "SD",    "sd",       kMaskCloudOnly},
    {"NE",    "Nepali",               "नेपाली",             "NE",    "ne",       kMaskCloudOnly},
    {"AS",    "Assamese",             "অসমীয়া",            "AS",    "as",       kMaskCloudOnly},
    {"DZ",    "Dzongkha",             "རྫོང་ཁ",             "DZ",    "dz",       kMaskCloudOnly},
    {"SA",    "Sanskrit",             "संस्कृतम्",          "SA",    "sa",       kMaskCloudOnly},
    {"MAI",   "Maithili",             "मैथिली",             "MAI",   "mai",      kMaskCloudOnly},
    {"GOM",   "Goan Konkani",         "कोंकणी",             "GOM",   "gom",      kMaskCloudOnly}
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
        // REQ-051-1: Strip a leading UTF-8 BOM (EF BB BF) before the opening
        // brace. Windows editors (Notepad, PowerShell Set-Content/Out-File)
        // prepend a BOM when saving UTF-8 text; without this skip the parser
        // saw byte 0xEF instead of '{' and rejected — then the startup path
        // overwrote the user's config with defaults, resetting every setting.
        // The app itself never writes a BOM (SaveToFileLocked emits raw UTF-8),
        // but external edits must not brick the config.
        if (src_.size() >= 3 &&
            static_cast<unsigned char>(src_[0]) == 0xEF &&
            static_cast<unsigned char>(src_[1]) == 0xBB &&
            static_cast<unsigned char>(src_[2]) == 0xBF) {
            pos_ = 3;
        }
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

            if (key == "user_languages") {
                // 261116_0003 P9a fix PR9-04: the block's value is captured RAW
                // (its exact source bytes, never escape-decoded). The
                // forward-compat carrier re-emits these bytes verbatim on the
                // next save; decoding a string value and re-emitting it
                // unquoted was the sibling-key fabrication path.
                if (!CaptureRawValue(value)) return false;
            } else if (src_[pos_] == '\"') {
                if (!ParseString(value)) return false;
            } else if (src_[pos_] == 't' || src_[pos_] == 'f') {
                if (!ParseBool(value)) return false;
            } else if (src_[pos_] == '{' || src_[pos_] == '[') {
                // 261116_0003 P9a fix4 PR12-01: nested object/array value
                // (the "engine_host" block). Its raw text is captured by the
                // shared comment-aware CaptureRawValue below; the caller
                // re-parses that substring with a second SimpleJsonReader.
                // 261116_0003 P9a fix3 PR11-01: the capture goes through the
                // comment-aware CaptureRawValue below (the same scanner the
                // user_languages block uses). The former dedicated group scan
                // was comment-blind at the group level, so a "// }" note
                // inside an object/array value ended the scan early and the
                // remaining text bound as top-level sibling keys
                // (probe-proven: an object-typed user_languages_version or
                // engine_host value flipped cloud_fallback_enabled and
                // redirected openai.base_url, laundered by the next save).
                // The shared capture also enforces the PR10-03 separator
                // check for these values.
                if (!CaptureRawValue(value)) return false;
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
            // 261116_0003 P9a fix3 PR11-03: a value must be followed by a
            // separator. The pre-fix loop consumed a comma only when present,
            // so a missing comma after a string/bool/primitive value
            // ("\"ui_language\": \"ko\" \"cloud_fallback_enabled\": true")
            // bound the next key as a sibling and laundered the edit on the
            // next save. Object/array values already fail closed inside
            // CaptureRawValue (PR10-03).
            if (pos_ >= src_.size()) return false; // unterminated object
            if (src_[pos_] == ',') {
                pos_++;
            } else if (src_[pos_] != '}') {
                return false; // no separator: fail closed, bind nothing
            }
        }
        return false;
    }

private:
    // 261116_0003 P9a fix PR9-04: captures ONE value's raw source text,
    // whatever its type (string, number, object, array), string-, bracket-,
    // and comment-aware (PR9-08: a commented "}" must not end the scan early,
    // matching the inner array scanner) so the terminating top-level ',' or
    // '}' is found correctly. Returns false when the value never terminates
    // (unbalanced brackets or an unterminated string), so a malformed block
    // fails the whole parse like every other value routed through this
    // shared capture (261116_0003 P9a fix4 PR12-01).
    // 261116_0003 P9a fix2 PR10-03: the value's OWN syntactic end is located
    // first (the closing quote of a string, the matching bracket of an
    // object/array, or a primitive's delimiter) and the separator check runs
    // against that end. The pre-fix scan ran to the next top-level ',' the
    // document happened to contain, so a missing comma after the value
    // ("[...] \"ui_language\": \"ko\"") re-entered string mode and absorbed the
    // following keys into the value: the parse SUCCEEDED and the next save
    // persisted the loss. A value not followed by ',' or '}' now fails the
    // parse closed instead.
    bool CaptureRawValue(std::string& out) {
        SkipWhitespace(); // leading trivia (comments included) is not the value
        const std::size_t start = pos_;
        if (pos_ >= src_.size()) {
            return false;
        }
        const char first = src_[pos_];
        if (first == '\"') {
            // String value: raw bytes through its closing quote (escapes kept).
            ++pos_;
            bool closed = false;
            while (pos_ < src_.size()) {
                const char c = src_[pos_];
                if (c == '\\') {
                    if (pos_ + 1 >= src_.size()) {
                        return false; // truncated escape
                    }
                    pos_ += 2;
                    continue;
                }
                ++pos_;
                if (c == '\"') {
                    closed = true;
                    break;
                }
            }
            if (!closed) {
                return false; // unterminated string: the document is malformed
            }
        } else if (first == '{' || first == '[') {
            // Object/array value: balanced scan that stops just past the
            // matching close, so the enclosing object's '}' can never be
            // mistaken for the value's end.
            // 261116_0003 P9a fix3 PR11-02: the unmatched openers live on a
            // stack so a closer must match its OWN group. The former single
            // depth counter accepted a mismatched closer ("[1}", "[{\"a\":1]")
            // as the value's end. The top-level reader routes every
            // object/array value through this capture (PR11-01), so this
            // scan is also the outer group's strictness gate.
            std::vector<char> openers;
            bool in_string = false;
            bool in_line_comment = false;
            bool in_block_comment = false;
            bool closed = false;
            while (pos_ < src_.size()) {
                const char c = src_[pos_];
                if (in_line_comment) {
                    if (c == '\n') {
                        in_line_comment = false;
                    }
                    ++pos_;
                    continue;
                }
                if (in_block_comment) {
                    if (c == '*' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                        in_block_comment = false;
                        pos_ += 2;
                        continue;
                    }
                    ++pos_;
                    continue;
                }
                if (in_string) {
                    // 261116_0003 P9a fix4 PR12-05: the same pos+1 < size
                    // bounds guard the sibling scanners below carry. A
                    // trailing backslash now steps to end of input instead
                    // of overshooting it; the final in_string check still
                    // fails the value closed either way.
                    if (c == '\\' && pos_ + 1 < src_.size()) {
                        pos_ += 2; // skip the escaped character
                        continue;
                    }
                    if (c == '\"') {
                        in_string = false;
                    }
                    ++pos_;
                    continue;
                }
                if (c == '\"') {
                    in_string = true;
                } else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                    in_line_comment = true;
                    pos_ += 2;
                    continue;
                } else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '*') {
                    in_block_comment = true;
                    pos_ += 2;
                    continue;
                } else if (c == '{' || c == '[') {
                    openers.push_back(c);
                } else if (c == '}' || c == ']') {
                    if (openers.empty() ||
                        (openers.back() == '{' && c != '}') ||
                        (openers.back() == '[' && c != ']')) {
                        return false; // a closer with the wrong opener: malformed
                    }
                    openers.pop_back();
                    if (openers.empty()) {
                        ++pos_;
                        closed = true;
                        break;
                    }
                }
                ++pos_;
            }
            if (!closed || in_string || in_block_comment) {
                return false; // unterminated value: the document is malformed
            }
        } else {
            // Primitive (number/null/...): runs to its delimiter.
            while (pos_ < src_.size() && src_[pos_] != ',' && src_[pos_] != '}' &&
                   !std::isspace(static_cast<unsigned char>(src_[pos_]))) {
                ++pos_;
            }
        }
        // The captured raw text stays "value start through the enclosing
        // separator", the pre-fix shape: trivia between the value and the ','
        // or '}' is skipped by both consumers of this text (the block splitter
        // and the carrier's bracket scan), and keeping it lets the carrier
        // prove it trims a trailing note (PR10-04).
        SkipWhitespace();
        if (pos_ >= src_.size() || (src_[pos_] != ',' && src_[pos_] != '}')) {
            return false; // the value is not terminated by a separator
        }
        out = std::string(src_.substr(start, pos_ - start));
        return true;
    }

    void SkipWhitespace() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (std::isspace(static_cast<unsigned char>(c))) {
                pos_++;
            } else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                // Line comment support
                pos_ += 2;
                while (pos_ < src_.size() && src_[pos_] != '\n') pos_++;
            } else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '*') {
                // Backend P2-6 (v0.10.5 handoff section 4.4 item 10): block
                // comment support ("/* ... */") alongside the line comments.
                pos_ += 2;
                while (pos_ + 1 < src_.size() && !(src_[pos_] == '*' && src_[pos_ + 1] == '/')) {
                    pos_++;
                }
                if (pos_ + 1 < src_.size()) {
                    pos_ += 2; // consume the closing marker
                } else {
                    pos_ = src_.size(); // unterminated block comment swallows the rest
                }
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
                        // REF-3.5 (session 260910_0006 T4): the former inline
                        // read_hex4 + I2-surrogate-defense + UTF-8 encoder block
                        // (the reference implementation, behavior unchanged) now
                        // lives in unicode_utils::AppendJsonUnicodeEscape, shared
                        // with google_translate.cpp. The helper's contract is a
                        // line-for-line move of this case: false on truncated /
                        // non-hex input (propagated to string-parse failure
                        // exactly as before), lone surrogates -> U+FFFD, valid
                        // pairs -> astral code point, failed lookahead rolls
                        // pos_ back so the next escape re-parses normally.
                        if (!AppendJsonUnicodeEscape(out, src_, pos_)) return false;
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

// REQ-043: parse the nested "engine_host" object captured verbatim by the
// reader above. Existing-field convention applies: absent keys keep the
// current (default-initialized) values; a malformed block keeps ALL defaults;
// idle_exit_ms is validated as an integer — 0/negative restore the default
// policy, values above the 24h ceiling clamp, unparseable resets to default
// (the top_k clamp/reset precedent).
// REQ-060: worker_idle_ms follows the same shape — 0/negative restore the
// 90000 default policy, values below the 5 s floor clamp up, values above
// the 24h ceiling clamp down, unparseable resets to default. warmup is
// strict true-only (any non-"true" literal -> false, the enabled/spawn
// discipline).
void ParseEngineHostBlock(const std::string& raw, EngineHostConfig& out) {
    SimpleJsonReader reader(raw);
    std::vector<std::pair<std::string, std::string>> pairs;
    if (!reader.ParseObject(pairs)) {
        return; // malformed block: keep defaults
    }
    for (const auto& [k, v] : pairs) {
        if (k == "enabled") {
            out.enabled = (v == "true");
        } else if (k == "spawn") {
            out.spawn = (v == "true");
        } else if (k == "idle_exit_ms") {
            try {
                const long long ms = std::stoll(v);
                if (ms <= 0) {
                    out.idle_exit_ms = 600000; // 0 means "use the default policy"
                } else if (ms > 86400000LL) {
                    out.idle_exit_ms = 86400000; // 24 h ceiling
                } else {
                    out.idle_exit_ms = static_cast<int>(ms);
                }
            } catch (...) {
                out.idle_exit_ms = 600000; // unparseable -> default
            }
        } else if (k == "worker_idle_ms") {
            // REQ-060: translation-worker idle eviction budget.
            try {
                const long long ms = std::stoll(v);
                if (ms <= 0) {
                    out.worker_idle_ms = 90000; // 0 means "use the default policy"
                } else if (ms < 5000) {
                    out.worker_idle_ms = 5000; // 5 s floor
                } else if (ms > 86400000LL) {
                    out.worker_idle_ms = 86400000; // 24 h ceiling
                } else {
                    out.worker_idle_ms = static_cast<int>(ms);
                }
            } catch (...) {
                out.worker_idle_ms = 90000; // unparseable -> default
            }
        } else if (k == "warmup") {
            // REQ-060: strict true-only, same discipline as enabled/spawn.
            out.warmup = (v == "true");
        }
    }
}

// ---- 261122_0001 fix-A1 (R1-03): the OpenAI block/profile FIELD CAPS --------
//
// ApplyOpenAiConfigPair below is the shared parse choke point for BOTH the
// active "openai" block and every "openai_profiles" entry (the B-U1 contract:
// an entry body IS the block shape plus "name"), so capping here bounds every
// load-time copy of these fields at once. The caps sit far above any honest
// value (a base URL, a model id, a base64 DPAPI blob — real-world values are
// tens to low-hundreds of bytes) yet keep a hostile hand-edited config from
// feeding multi-MB strings x kMaxOpenAiProfiles into load-time copies and
// per-chunk request bodies (the R1-03 finding). Truncation (not drop) at a
// UTF-8 code-point boundary, the kMaxUserLanguage* cap precedent; honest
// configs never reach the caps, so no behavior changes for them.
constexpr std::size_t kMaxOpenAiBaseUrlConfigBytes = 2048;
constexpr std::size_t kMaxOpenAiModelConfigBytes = 512;
constexpr std::size_t kMaxOpenAiApiKeyDpapiConfigBytes = 16384;

// Byte cap that never splits a UTF-8 code point (CapUserLanguageField's rule
// restated beside this choke point so this anonymous namespace stays
// independent of the P9a one defined further below).
std::string CapOpenAiConfigField(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return std::string(text);
    }
    std::size_t pos = 0;
    std::size_t last_boundary = 0;
    while (pos < text.size()) {
        std::size_t consumed = 1;
        language_alias_detail::DecodeUtf8CodePoint(text, pos, consumed);
        pos += consumed;
        if (pos > max_bytes) {
            break;
        }
        last_boundary = pos;
    }
    return std::string(text.substr(0, last_boundary));
}

// 261122_0001 fix-A1 (R1-03): the integrity digest's only trustworthy wire
// shape — exactly 64 lowercase hex chars. Anything else (wrong length,
// uppercase, non-hex, empty) fails CLOSED: the digest is dropped TOGETHER
// with the key material it vouches for (see ApplyOpenAiConfigPair).
bool IsOpenAiSha256DigestWellFormed(std::string_view digest) {
    if (digest.size() != 64) {
        return false;
    }
    for (const char c : digest) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) {
            return false;
        }
    }
    return true;
}

// REQ-045 P4-3 (design §3b): applies ONE parsed key/value pair of the OpenAI
// block to `out`. Shared by ParseOpenAiBlock (the active "openai" block) and
// the 261122_0001 B-U1 profile-entry parser: an openai_profiles entry body IS
// the openai block shape plus "name", so the two parsers must agree on the
// exact field semantics (lenient absent-key posture, the reasoning_effort
// clamp, the 261122_0001 fix-A1 field caps + digest shape check) and this
// function is that single source of truth.
void ApplyOpenAiConfigPair(const std::string& k, const std::string& v, OpenAiConfig& out) {
    if (k == "base_url") {
        // 261122_0001 fix-A1 (R1-03): cap at the shared choke point.
        out.base_url = CapOpenAiConfigField(v, kMaxOpenAiBaseUrlConfigBytes);
    } else if (k == "model") {
        out.model = CapOpenAiConfigField(v, kMaxOpenAiModelConfigBytes);
    } else if (k == "api_key_dpapi") {
        out.api_key_dpapi = CapOpenAiConfigField(v, kMaxOpenAiApiKeyDpapiConfigBytes);
    } else if (k == "api_key_sha256") {
        // 261122_0001 fix-A1 (R1-03, fail-closed): the digest must be exactly
        // 64 lowercase hex chars; ANYTHING else means the pair is untrustworthy
        // and both key fields clear together (a half-pair is the refuse-
        // forever shape, REQ-050 b6f98da / REQ-051 pair atomicity — never
        // leave a blob whose integrity claim failed, and never a digest
        // vouching for a blob that is not there). Honest saves always write a
        // well-formed pair, so this only bites hand-edited files. A dpapi
        // value listed AFTER a malformed digest lands without a digest, the
        // same state omitting the digest key entirely already produces
        // (WithUnprotectedKey skips the check for an empty sha256), so no new
        // runtime capability is introduced.
        if (IsOpenAiSha256DigestWellFormed(v)) {
            out.api_key_sha256 = v;
        } else {
            out.api_key_sha256.clear();
            out.api_key_dpapi.clear();
        }
    } else if (k == "http_consent_given") {
        out.http_consent_given = (v == "true");
    } else if (k == "reasoning_effort") {
        // 261121_0001 U-H (contract C2): the reasoning effort sent to the
        // OpenAI-compatible endpoint. "" (absent key, empty value) means
        // "send no reasoning_effort at all" - the mandatory default,
        // because a non-reasoning model answers HTTP 400 to that key. Any
        // other value is kept, bounded by the clamp below: which efforts a
        // gateway accepts is only known from its own /v1/models metadata,
        // so a whitelist here could reject a value the server itself
        // advertised. The value is JSON-escaped on the write side, so it
        // can never break the file.
        //
        // G-2 (261121_0001 fix round 2, SEC-F02): clamp the READ to
        // kMaxOpenAiReasoningEffortConfigChars. Legitimate tokens are
        // <= 8 chars (none|minimal|low|medium|high|xhigh); the settings
        // combo caps its items at 32. A hand-edited (hostile) config.json
        // could otherwise carry a multi-MB value that rides verbatim into
        // EVERY per-chunk request body and every config rewrite. 64 is
        // far above any real token yet bounded, and the truncation (not
        // drop) keeps a long-but-real token usable in diagnosis.
        constexpr std::size_t kMaxOpenAiReasoningEffortConfigChars = 64;
        out.reasoning_effort = (v.size() <= kMaxOpenAiReasoningEffortConfigChars)
                                   ? v
                                   : v.substr(0, kMaxOpenAiReasoningEffortConfigChars);
    }
}

// REQ-045 P4-3 (design §3b): parse the nested "openai" object captured
// verbatim by the reader (same convention as ParseEngineHostBlock above).
// Absent keys keep the default-initialized values; a malformed block keeps
// ALL defaults. The key is stored DPAPI-protected (never cleartext); the
// digest is expected to be exactly 64 lowercase hex chars when present.
void ParseOpenAiBlock(const std::string& raw, OpenAiConfig& out) {
    SimpleJsonReader reader(raw);
    std::vector<std::pair<std::string, std::string>> pairs;
    if (!reader.ParseObject(pairs)) {
        return; // malformed block: keep defaults
    }
    for (const auto& [k, v] : pairs) {
        ApplyOpenAiConfigPair(k, v, out);
    }
}

// ---- 261116_0003 P9c-1a: the OpenAI declared-language VIEW FILTER ----------
//
// One piece of process state, published by exactly two callers in this TU (the
// load path and AppConfig::SetOpenAiDeclaredLanguages) through
// PublishOpenAiDeclaredLanguages, and read by ONE authority:
// GetLanguagesForEngine. Every menu surface reaches the filter through that
// function (the tray's four language menus and the tooltip's two dropdowns
// through BuildLanguageMenuView, the search picker's pool directly, and the
// Ctrl+F9 cycle set through CycleTargetLanguage), so no surface can re-filter
// and the six surfaces plus the picker cannot drift.
//
// Threading: `mutex` guards the whole state. The FILTERED VIEWS are cached
// append-only in a deque and keyed by (generation, include_auto); a deque never
// invalidates references to its elements on push_back, so a caller that already
// holds a view reference (a menu built before a declaration change) keeps a
// valid row list for the process lifetime. Old generations are never freed on
// purpose: the alternative (replacing the cached vectors in place) would free a
// list some other thread may still be reading.
struct OpenAiDeclaredLanguageState {
    struct CachedView {
        std::uint64_t generation = 0;
        bool include_auto = false;
        std::vector<const LanguageInfo*> rows;
    };
    std::mutex mutex;
    std::uint64_t generation = 0;
    std::vector<std::string> codes; // canonical, registry order (publish sanitizes)
    std::deque<CachedView> views;   // append-only; see the threading note above
};

OpenAiDeclaredLanguageState& OpenAiDeclaredLanguageSnapshot() {
    static OpenAiDeclaredLanguageState state;
    return state;
}

bool IsOpenAiLanguageDeclaredLocked(const OpenAiDeclaredLanguageState& state,
                                    std::string_view code) {
    for (const std::string& declared : state.codes) {
        if (EqualsIgnoreCaseAscii<char>(declared, code)) {
            return true;
        }
    }
    return false;
}

// The single filter point. An EMPTY declaration returns `base` itself, so the
// pre-P9c behavior is not merely equivalent but pointer-identical. AUTO is
// always kept: it is a source-only marker, not a language the endpoint can be
// asked to serve, so a declaration never takes auto-detect out of a source menu.
const std::vector<const LanguageInfo*>& FilterOpenAiViewForDeclaration(
    const std::vector<const LanguageInfo*>& base, bool include_auto) {
    OpenAiDeclaredLanguageState& state = OpenAiDeclaredLanguageSnapshot();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.codes.empty()) {
        return base; // NOT DECLARED: the full generous set, unchanged
    }
    for (auto it = state.views.rbegin(); it != state.views.rend(); ++it) {
        if (it->generation == state.generation && it->include_auto == include_auto) {
            return it->rows;
        }
    }
    OpenAiDeclaredLanguageState::CachedView view;
    view.generation = state.generation;
    view.include_auto = include_auto;
    view.rows.reserve(base.size());
    for (const LanguageInfo* row : base) {
        if (row == nullptr) {
            continue;
        }
        if (row->code == "AUTO" || IsOpenAiLanguageDeclaredLocked(state, row->code)) {
            view.rows.push_back(row);
        }
    }
    state.views.push_back(std::move(view));
    return state.views.back().rows;
}

// Skips JSON trivia (whitespace plus the // and /* */ comments this reader
// supports everywhere else), starting at `pos`.
std::size_t SkipDeclaredLanguagesTrivia(std::string_view text, std::size_t pos) {
    while (pos < text.size()) {
        const char c = text[pos];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++pos;
        } else if (c == '/' && pos + 1 < text.size() && text[pos + 1] == '/') {
            pos += 2;
            while (pos < text.size() && text[pos] != '\n') {
                ++pos;
            }
        } else if (c == '/' && pos + 1 < text.size() && text[pos + 1] == '*') {
            pos += 2;
            while (pos + 1 < text.size() && !(text[pos] == '*' && text[pos + 1] == '/')) {
                ++pos;
            }
            pos = (pos + 1 < text.size()) ? pos + 2 : text.size();
        } else {
            break;
        }
    }
    return pos;
}

// Parses the raw captured value of "openai_declared_languages" - a JSON array
// of strings - into the RAW tokens the sanitizer then canonicalizes. Nothing is
// trusted: a non-array value, a non-string element, a truncated escape or an
// unterminated group stops the walk and the tokens collected so far are kept.
// A malformed declaration therefore reads as "not declared" (or as its valid
// prefix) instead of failing the whole load, the same lenient posture
// ParseOpenAiBlock takes with a malformed nested block. Never logs content.
std::vector<std::string> ParseOpenAiDeclaredLanguagesArray(std::string_view raw) {
    std::vector<std::string> tokens;
    std::size_t pos = SkipDeclaredLanguagesTrivia(raw, 0);
    if (pos >= raw.size() || raw[pos] != '[') {
        return tokens; // not an array: nothing declared
    }
    ++pos;
    bool needs_comma = false;
    while (true) {
        pos = SkipDeclaredLanguagesTrivia(raw, pos);
        if (pos >= raw.size()) {
            return tokens; // unterminated array
        }
        if (raw[pos] == ']') {
            return tokens;
        }
        if (needs_comma) {
            if (raw[pos] != ',') {
                return tokens; // missing separator: stop, keep the prefix
            }
            ++pos;
            pos = SkipDeclaredLanguagesTrivia(raw, pos);
            if (pos < raw.size() && raw[pos] == ']') {
                return tokens; // trailing comma
            }
        }
        if (pos >= raw.size() || raw[pos] != '\"') {
            return tokens; // non-string element
        }
        ++pos;
        std::string token;
        bool closed = false;
        while (pos < raw.size()) {
            const char c = raw[pos++];
            if (c == '\"') {
                closed = true;
                break;
            }
            if (c != '\\') {
                token += c;
                continue;
            }
            if (pos >= raw.size()) {
                return tokens; // truncated escape
            }
            const char esc = raw[pos++];
            switch (esc) {
                case '\"': token += '\"'; break;
                case '\\': token += '\\'; break;
                case '/': token += '/'; break;
                case 'b': token += '\b'; break;
                case 'f': token += '\f'; break;
                case 'n': token += '\n'; break;
                case 'r': token += '\r'; break;
                case 't': token += '\t'; break;
                case 'u': {
                    // Same escape decoder the object reader uses (lone
                    // surrogates -> U+FFFD, truncated/non-hex fails).
                    if (!AppendJsonUnicodeEscape(token, raw, pos)) {
                        return tokens;
                    }
                    break;
                }
                default: token += esc; break;
            }
        }
        if (!closed) {
            return tokens; // unterminated string
        }
        tokens.push_back(std::move(token));
        needs_comma = true;
    }
}

} // namespace

const std::vector<LanguageInfo>& GetSupportedLanguages() {
    return kAllLanguages;
}

const std::vector<LanguageInfo>& GetTargetLanguages() {
    static const std::vector<LanguageInfo> kTargetLanguages = InitTargetLanguages();
    return kTargetLanguages;
}

// 261116_0003 P2 (plan §4): engine-filtered registry views. The eight views
// (4 engines x with/without AUTO) are built ONCE from the immutable global
// registry and hold POINTERS at its rows (stable for the process lifetime —
// kAllLanguages is const after init, so no view can dangle). LanguageEngine::
// Auto resolves to the HyMt2 view: auto prefers local (plan §4 "Routing").
const std::vector<const LanguageInfo*>& GetLanguagesForEngine(LanguageEngine engine,
                                                              bool include_auto) {
    auto build = [](std::uint32_t bit, bool with_auto) {
        std::vector<const LanguageInfo*> out;
        out.reserve(kAllLanguages.size());
        for (const LanguageInfo& lang : kAllLanguages) {
            if ((lang.engine_mask & bit) == 0) {
                continue;
            }
            if (!with_auto && lang.code == "AUTO") {
                continue;
            }
            out.push_back(&lang);
        }
        return out;
    };
    static const std::vector<const LanguageInfo*> kHyMt2Auto = build(kLangHyMt2, true);
    static const std::vector<const LanguageInfo*> kHyMt2Targets = build(kLangHyMt2, false);
    static const std::vector<const LanguageInfo*> kGoogleAuto = build(kLangGoogle, true);
    static const std::vector<const LanguageInfo*> kGoogleTargets = build(kLangGoogle, false);
    static const std::vector<const LanguageInfo*> kOpenAiAuto = build(kLangOpenAi, true);
    static const std::vector<const LanguageInfo*> kOpenAiTargets = build(kLangOpenAi, false);
    static const std::vector<const LanguageInfo*> kUserGgufAuto = build(kLangUserGguf, true);
    static const std::vector<const LanguageInfo*> kUserGgufTargets = build(kLangUserGguf, false);
    switch (engine) {
        case LanguageEngine::HyMt2:
        case LanguageEngine::Auto:
            return include_auto ? kHyMt2Auto : kHyMt2Targets;
        case LanguageEngine::Google:
            return include_auto ? kGoogleAuto : kGoogleTargets;
        case LanguageEngine::OpenAi:
            // 261116_0003 P9c-1a: the ONE place the declared-language set
            // filters a registry view. Every menu surface, the search picker's
            // pool and the Ctrl+F9 cycle set reach this function, so the
            // declaration is honoured once and cannot drift per surface. An
            // empty (absent) declaration returns the static base view itself,
            // i.e. the pre-P9c byte-for-byte behavior.
            return FilterOpenAiViewForDeclaration(include_auto ? kOpenAiAuto : kOpenAiTargets,
                                                  include_auto);
        case LanguageEngine::UserGguf:
            return include_auto ? kUserGgufAuto : kUserGgufTargets;
    }
    return kHyMt2Targets; // unreachable: the enum is closed
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

// 261108_0002 (WS3, v0.10.6): back-translation source ladder. See config.hpp
// for the contract; the four tiers are the same precedence the rest of the
// language surface uses (pinned source -> detection -> OS language -> the
// ADR-A1-7 neutral pivot), each one skipping the forward target so the card
// never asks for a src==tgt round trip.
std::string ResolveBackTranslationTarget(std::string_view type_source_language,
                                         std::string_view detected_source_language,
                                         std::string_view forward_target_language,
                                         std::string_view system_language_code) {
    const std::string fwd_tgt = NormalizeLanguageCode(forward_target_language);

    // Returns the name_en of `candidate` when it is a CONCRETE language that
    // differs from the forward target, else an empty string (tier skipped).
    // NormalizeLanguageCode maps "", "Auto Detect", "AUTO" and every
    // unresolvable token to "AUTO", so one test covers both the concrete-ness
    // and the case-insensitivity requirements.
    const auto concrete_other = [&fwd_tgt](std::string_view candidate) -> std::string {
        const std::string code = NormalizeLanguageCode(candidate);
        if (code == "AUTO" || code == fwd_tgt) {
            return {};
        }
        const LanguageInfo* info = FindLanguageByCode(code);
        return info ? info->name_en : std::string();
    };

    // 1. The user's PINNED typing source is ground truth (F3/ADR-A1-2).
    if (std::string tier = concrete_other(type_source_language); !tier.empty()) {
        return tier;
    }
    // 2. Per-request detection, while the pinned source is AUTO.
    if (std::string tier = concrete_other(detected_source_language); !tier.empty()) {
        return tier;
    }
    // 3. OS UI language (the ADR-A1-7 rank-1 pivot input).
    if (std::string tier = concrete_other(system_language_code); !tier.empty()) {
        return tier;
    }
    // 4. Everything AUTO: the ADR-A1-7 neutral pivot, the fallback the spec
    //    names. ResolveEffectiveTarget("Auto Detect", fwd, false) canonicalizes
    //    the source to AUTO, so tier 3 already consumed its rank-1 outcome
    //    (the OS language, when concrete and != fwd). What remains is its
    //    rank-2/3 tail, replayed here so the ladder has a defined last rung:
    //      * rank 2 - English, when the forward target is not English
    //        (the ADR-A1-7 neutral pivot VALUE is unchanged; the routing
    //        behind it is mask-driven since 261116_0003 P2: EN carries
    //        kLangHyMt2, so an official source still pivots to a locally
    //        servable language, while a cloud-only source is routed by
    //        LocalPairReliable's D7 engine_mask);
    //      * rank 3 - nullopt (forward target IS English and the OS language
    //        is English/unknown). The user already reads that language, so the
    //        forward target itself is the honest answer: the card then shows
    //        the same text, an explicit "nothing to verify" signal instead of
    //        a wrong-language card. Returning nullopt's meaning here is why
    //        this branch answers with the forward target rather than "".
    if (fwd_tgt != "EN") {
        return std::string("English");
    }
    if (const LanguageInfo* info = FindLanguageByCode(fwd_tgt)) {
        return info->name_en;
    }
    return std::string("English"); // unreachable for a registered target
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

namespace {
// 261116_0003 P2: the shared Ctrl+F9 cycle step over an arbitrary registry
// view (pointer-stable rows). Matching precedence and the not-found restart
// are exactly the historical single-argument semantics.
std::string CycleTargetInView(std::string_view current_code_or_name,
                              const std::vector<const LanguageInfo*>& view) {
    if (view.empty()) {
        return {}; // unreachable for every live view (>= 38 targets)
    }
    size_t current_idx = 0;
    bool found = false;

    for (size_t i = 0; i < view.size(); ++i) {
        if (EqualsIgnoreCaseAscii<char>(view[i]->code, current_code_or_name) ||
            EqualsIgnoreCaseAscii<char>(view[i]->name_en, current_code_or_name) ||
            EqualsIgnoreCaseAscii<char>(view[i]->name_native, current_code_or_name)) {
            current_idx = i;
            found = true;
            break;
        }
    }

    size_t next_idx = found ? ((current_idx + 1) % view.size()) : 0;
    // Prefer returning English name for configuration and display consistency
    return view[next_idx]->name_en;
}

// 261116_0003 P2: the full-registry target view (kAllLanguages minus AUTO),
// in table order — the single-argument cycle form keeps cycling THIS list.
const std::vector<const LanguageInfo*>& AllTargetView() {
    static const std::vector<const LanguageInfo*> kAllTargets = [] {
        std::vector<const LanguageInfo*> out;
        out.reserve(kAllLanguages.size() - 1);
        for (size_t i = 1; i < kAllLanguages.size(); ++i) {
            out.push_back(&kAllLanguages[i]);
        }
        return out;
    }();
    return kAllTargets;
}
} // namespace

std::string CycleTargetLanguage(std::string_view current_code_or_name) {
    return CycleTargetInView(current_code_or_name, AllTargetView());
}

// 261116_0003 P2 (plan §4 "Routing"): engine-filtered cycle — the cycle set
// is the given engine's registry view (same matching/wrapping semantics as
// the single-argument form).
// 261116_0003 P2 fix round 2 (PR2-02 item 12, k3d1agent P3-1): DEFERRED to P6.
// 261116_0003 P8b: the deferral is CLOSED. Both Ctrl+F9 call sites now use
// this 2-arg overload: src/main.cpp's ApplyLanguageChange cycle_target branch
// and src/hook.cpp's KeyboardHook::CycleTargetLanguage unwired fallback, each
// deriving the active engine with the tray-menu idiom
// TrayLanguageEngineFor(TrayPreferredEngineIndex(snap.engine_type)) recorded
// on the config.hpp declaration. The hotkey therefore cycles exactly the view
// the language menus render. Pinned headlessly by TestP8bCtrlF9CycleWiring.
std::string CycleTargetLanguage(std::string_view current_code_or_name,
                                LanguageEngine engine) {
    return CycleTargetInView(current_code_or_name,
                             GetLanguagesForEngine(engine, false));
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
//
// 261116_0003 P9a: the planner body moved into the anonymous-namespace Impl so
// the registry-only overloads (nullptr) and the user-language-aware overload
// share ONE rule set. nullptr means "no user languages", which is exactly the
// pre-P9a behavior - the allowance cannot leak into an existing caller.
namespace {

// One planner token -> its canonical stored form, or an empty string when the
// token resolves to nothing this config may persist. Registry first (so AUTO
// and every registry spelling keep their meaning), then the registered user
// languages (P9a allowance, scoped to the passed vector).
std::string CanonicalPlannerToken(std::string_view token,
                                  const std::vector<UserLanguage>* user_languages) {
    if (const LanguageInfo* info = ResolveLanguageInfo(token)) {
        return info->name_en;
    }
    if (user_languages != nullptr) {
        if (const UserLanguage* user = FindUserLanguage(*user_languages, token)) {
            return UserLanguageStoredName(*user);
        }
    }
    return {};
}

LanguageSyncPlan PlanLanguageSyncImpl(LanguageContext /*ctx*/,
                                      std::string_view cur_source,
                                      std::string_view cur_target,
                                      std::string_view new_source,
                                      std::string_view new_target,
                                      const std::vector<UserLanguage>* user_languages) {
    LanguageSyncPlan plan; // valid=false, changed=false, no surfaces

    // Canonicalize the CURRENT pair. An unresolvable persisted value (e.g. a
    // hand-edited garbage config) is carried through RAW instead of poisoning
    // the whole mutation - the surfaces normalize for display via
    // NormalizeLanguageCode() anyway, and a later valid write self-heals it.
    // P9a: a persisted USER-language pin canonicalizes through the same lookup,
    // so a no-op mutation on it reports changed=false and never churns disk.
    const std::string canonical_src = CanonicalPlannerToken(cur_source, user_languages);
    const std::string canonical_tgt = CanonicalPlannerToken(cur_target, user_languages);
    std::string canon_src = canonical_src.empty() ? std::string(cur_source) : canonical_src;
    std::string canon_tgt = canonical_tgt.empty() ? std::string(cur_target) : canonical_tgt;

    // Empty request = keep the (canonicalized) current value. A non-empty
    // request MUST resolve: all-or-nothing, so a half-applied swap or a typo'd
    // menu name can never leave config and views disagreeing (INV-1 guard).
    // On refusal the CURRENT pair is echoed back untouched (valid=false tells
    // the coordinator to persist nothing and refresh nothing).
    if (!new_source.empty()) {
        const std::string requested = CanonicalPlannerToken(new_source, user_languages);
        if (requested.empty()) {
            plan.source_language = std::string(cur_source);
            plan.target_language = std::string(cur_target);
            return plan; // invalid: persist nothing, refresh nothing
        }
        canon_src = requested; // source accepts AUTO ("Auto Detect")
    }
    if (!new_target.empty()) {
        const std::string requested = CanonicalPlannerToken(new_target, user_languages);
        if (requested.empty() || requested == "Auto Detect") {
            // AUTO is never a translation TARGET (the tooltip/tray menus only
            // offer GetTargetLanguages()); refuse instead of storing a no-op.
            // The registry lookup runs FIRST, so a user entry spelled like the
            // AUTO row cannot smuggle a target through.
            plan.source_language = std::string(cur_source);
            plan.target_language = std::string(cur_target);
            return plan;
        }
        canon_tgt = requested;
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

} // namespace

LanguageSyncPlan PlanLanguageSync(LanguageContext ctx,
                                  std::string_view cur_source,
                                  std::string_view cur_target,
                                  std::string_view new_source,
                                  std::string_view new_target) {
    return PlanLanguageSyncImpl(ctx, cur_source, cur_target, new_source, new_target, nullptr);
}

LanguageSyncPlan PlanLanguageSync(LanguageContext ctx,
                                  std::string_view cur_source,
                                  std::string_view cur_target,
                                  std::string_view new_source,
                                  std::string_view new_target,
                                  const std::vector<UserLanguage>& user_languages) {
    return PlanLanguageSyncImpl(ctx, cur_source, cur_target, new_source, new_target,
                                &user_languages);
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

// REQ-CP T4 (design §8.2 / §8.3): render the two NEW prompt templates. Reuses
// the SAME language-resolution prologue as BuildPrompt (ResolvePromptLanguage:
// Chinese branch keeps name_native on both sides, English branch name_en on
// both sides; AUTO/unresolvable target -> raw-token injection; AUTO/empty/
// unresolvable source -> NO source hint token, the byte-stable degenerate
// form). The template bodies below are copied VERBATIM from design §8 — the
// exact {source}/{target}/{text} substitution pattern mirrors hymt2-official.
// `template_ref` is assumed already validated to one of the two new refs by
// the caller (BuildPrompt dispatches only known refs here; unknown/empty fall
// back to hymt2-official before reaching this function).
static std::string BuildTemplatedPrompt(std::string_view source_text,
                                        std::string_view target_lang,
                                        std::string_view source_lang,
                                        std::string_view template_ref) {
    std::string tgt_code;
    const LanguageInfo* tgt_info = ResolvePromptLanguage(target_lang, tgt_code);
    std::string tgt_native;
    std::string tgt_en;
    if (tgt_info != nullptr && tgt_code != "AUTO") {
        tgt_native = tgt_info->name_native;
        tgt_en     = tgt_info->name_en;
    } else {
        // AUTO / unresolvable target: raw-token injection (BuildPrompt parity).
        tgt_code.clear();
        tgt_native = std::string(target_lang);
        tgt_en     = tgt_native;
    }

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

    // Chinese-target branch, same sniff as hymt2-official (canonical ZH codes,
    // else the legacy raw-token Chinese sniff keeps historical forms).
    const bool zh_instruction =
        (tgt_code == "ZH-CN" || tgt_code == "ZH-TW") ||
        (tgt_code.empty() && IsChineseLanguage(tgt_native));

    std::string prompt;
    if (template_ref == kPromptTemplateSubtitleRealtime) {
        if (zh_instruction) {
            // Design §8.2 Chinese-target form (verbatim). The {source_native}
            // hint token is dropped exactly as hymt2-official's src_en-empty
            // path does, giving the byte-stable degenerate "将以下字幕翻译为…".
            prompt = "将以下";
            if (!src_native.empty()) {
                prompt.append(src_native);
            }
            prompt.append("字幕翻译为");
            prompt.append(tgt_native);
            prompt.append("。只输出译文一行，不要解释，不要引号，不要标签，不要添加标点。尽量与原文同样简短；若句子不完整，只翻译已有内容，不要补全。若内容无法辨认，原样输出原文。\n\n");
            prompt.append(source_text);
            return prompt;
        }
        // Design §8.2 English-source canonical form (verbatim).
        prompt = "Translate this subtitle line from ";
        if (!src_en.empty()) {
            prompt.append(src_en);
            prompt.push_back(' ');
        }
        prompt.append("into ");
        prompt.append(tgt_en);
        prompt.append(". Output ONLY the translation: one short line, no explanations, no quotation marks, no labels, no added punctuation. Keep it as brief as the original; if the line is cut off mid-sentence, translate only what is there and do not complete it. If the input is not intelligible ");
        if (!src_en.empty()) {
            prompt.append(src_en);
        }
        prompt.append(", output the input unchanged.\n\n");
        prompt.append(source_text);
        return prompt;
    }

    // kPromptTemplateLiteraryFlow (design §8.3, verbatim).
    if (zh_instruction) {
        prompt = "将以下";
        if (!src_native.empty()) {
            prompt.append(src_native);
        }
        prompt.append("文本翻译为");
        prompt.append(tgt_native);
        prompt.append("，供屏幕阅读使用。保持原文的语气与段落结构。只输出译文，不要任何解释或注释。\n\n");
        prompt.append(source_text);
        return prompt;
    }
    prompt = "Translate the following ";
    if (!src_en.empty()) {
        prompt.append(src_en);
        prompt.push_back(' ');
    }
    prompt.append("text into ");
    prompt.append(tgt_en);
    prompt.append(" for on-screen reading. Preserve the tone, register, and paragraph structure of the original. Output only the translation, without explanations or annotations.\n\n");
    prompt.append(source_text);
    return prompt;
}

std::string BuildPrompt(std::string_view source_text,
                        std::string_view target_lang,
                        std::string_view source_lang,
                        std::string_view template_ref) {
    // REQ-CP T4 (design §8 + tech-gate A2): dispatch on the template ref. The
    // registry is FAIL-CLOSED — an unknown ref or the empty ref selects
    // kPromptTemplateHymt2Official, so a bogus/absent ref can never reach a
    // non-sanctioned body. The hymt2-official branch below is the EXISTING
    // code path, byte-identical (the ref param only routes here); the two new
    // templates are rendered by BuildTemplatedPrompt. The completion-retry
    // (BuildCompletionPrompt) is template-independent per A2 and untouched.
    if (template_ref != kPromptTemplateHymt2Official && !template_ref.empty()) {
        if (template_ref == kPromptTemplateSubtitleRealtime ||
            template_ref == kPromptTemplateLiteraryFlow) {
            return BuildTemplatedPrompt(source_text, target_lang, source_lang, template_ref);
        }
        // Unknown ref: fall through to hymt2-official (fail-closed).
    }

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
        // Plan §4.1 template + Condition C: 将以下[<source>文本]翻译为<target-native>，…
        // Tencent 100% Official Full SFT template (official Chinese SFT template).
        std::string prompt = "将以下";
        if (!src_native.empty()) {
            prompt.append(src_native);
        }
        prompt.append("文本翻译为");
        prompt.append(tgt_native);
        if (!src_native.empty()) {
            // Pinned source: the historical form, byte-identical (REQ-CP pins).
            prompt.append("，注意只需要输出翻译后的结果，不要额外解释：\n\n");
            prompt.append(source_text);
            return prompt;
        }
        // RC-3 (v0.10.5 handoff section 2.4) -> WS1 (261108_0002): the AUTO
        // source now uses the SAME clean SFT form as the pinned branch, minus
        // the source-language name. RC-3 wrapped the text in
        // <<<EMEBALA_SOURCE_BEGIN/END>>> fences plus a literal-text rule; on a
        // 1-token input the ~60-token instruction made the 1.8B model attend
        // to the marker tokens and translate/transliterate them instead of the
        // text, and the rule's own "command / question / conversation" tokens
        // primed numbered quiz-style answers. The clean form carries no brand
        // and no meta vocabulary, so nothing but the text itself can be echoed.
        prompt.append("，注意只需要输出翻译后的结果，不要额外解释：\n\n");
        prompt.append(source_text);
        return prompt;
    }

    // Task 1 (session 260910_0004): the English instruction carries the
    // ENGLISH name (name_en) on both sides.
    // Condition C: Align with Tencent 100% Official Full SFT template
    // ("text into ... . Note that you should only output the translated result without any additional explanation:\n\n").
    std::string prompt = "Translate the following ";
    if (!src_en.empty()) {
        prompt.append(src_en);
        prompt.push_back(' ');
    }
    prompt.append("text into ");
    prompt.append(tgt_en);
    if (!src_en.empty()) {
        // Pinned source: the historical form, byte-identical (REQ-CP pins).
        prompt.append(". Note that you should only output the translated result without any additional explanation:\n\n");
        prompt.append(source_text);
        return prompt;
    }
    // RC-3 (v0.10.5 handoff section 2.4) -> WS1 (261108_0002): the AUTO
    // source now uses the SAME clean SFT form as the pinned branch, minus the
    // source-language name. RC-3 wrapped the text in
    // <<<EMEBALA_SOURCE_BEGIN/END>>> fences plus a literal-text rule; on a
    // 1-token input (cyber / astrology / clue / ...) the ~60-token
    // instruction made the 1.8B-4B model attend to the marker tokens and
    // translate/transliterate them instead of the text ("EMEBALA_SOURCE" ->
    // "Emebala의 출처" / "이메발라" / subword-primed "이메일"), and the rule's
    // own "command, question ... answer it" tokens primed numbered quiz-style
    // answers. The clean form carries no brand and no meta vocabulary, so
    // nothing but the text itself can be echoed.
    prompt.append(". Note that you should only output the translated result without any additional explanation:\n\n");
    prompt.append(source_text);
    return prompt;
}

// REQ-059: the completion-form prompt for user GGUF models whose GGUF chat
// template is trivial (rung 2 of the fixed ladder in translation_common.cpp;
// see the header for the two pinned forms and the fidelity-probe evidence).
// Language resolution mirrors BuildPrompt above: name_en on both sides,
// AUTO/unresolvable target -> raw-token injection, AUTO/absent/unresolvable
// source -> the target-side-only form.
std::string BuildCompletionPrompt(std::string_view source_text,
                                  std::string_view target_lang,
                                  std::string_view source_lang) {
    std::string tgt_code;
    const LanguageInfo* tgt_info = ResolvePromptLanguage(target_lang, tgt_code);
    std::string tgt_en;
    if (tgt_info != nullptr && tgt_code != "AUTO") {
        tgt_en = tgt_info->name_en;
    } else {
        // AUTO or unresolvable token: keep the historical raw injection so
        // the prompt still names whatever the caller passed.
        tgt_code.clear();
        tgt_en = std::string(target_lang);
    }

    std::string src_code;
    std::string src_en;
    if (!source_lang.empty()) {
        const LanguageInfo* src_info = ResolvePromptLanguage(source_lang, src_code);
        if (src_info != nullptr && src_code != "AUTO") {
            src_en = src_info->name_en;
        }
    }

    std::string prompt;
    if (!src_en.empty()) {
        // Official two-sided completion form (fidelity probe: PERFECT both
        // directions on the MiLM family when the bare chat path decodes
        // empty or echoes).
        prompt.append("Translate this from ");
        prompt.append(src_en);
        prompt.append(" to ");
        prompt.append(tgt_en);
        prompt.append(":\n");
        prompt.append(src_en);
        prompt.append(": ");
        prompt.append(source_text);
        prompt.push_back('\n');
        prompt.append(tgt_en);
        prompt.push_back(':');
    } else {
        // AUTO / absent / unresolvable source: naming a source language the
        // model must detect itself ("from Auto Detect") is nonsense, so the
        // form carries the target side only.
        prompt.append("Translate into ");
        prompt.append(tgt_en);
        prompt.append(":\n");
        prompt.append(source_text);
        prompt.push_back('\n');
        prompt.append(tgt_en);
        prompt.push_back(':');
    }
    return prompt;
}

// R6 Phase 4 (B2, architect plan §4.1 item 3): supported-pair policy for the
// LOCAL Hy-MT2 engine (see config.hpp doc).
//
// F1 companion fix (session 260908_0003, verify 220010 §5 item 4): an AUTO
// SOURCE is reliable to every OFFICIAL target. Under src=Auto the local Hy-MT2
// model decides the source language with its own built-in language ID - the
// exact mechanism the Latin-script AUTO pass-through feeds it (DetectLanguage
// returns "Auto Detect" for Latin instead of a forced language). Without this
// clause, Latin Auto -> non-EN targets (e.g. Portuguese -> Korean) would
// normalize AUTO, fail the EN-side gate, and be flagged "outside the reliable
// set": the 041 branch would ship the text to Google (privacy regression +
// wrong "VI -> KO" pair churn).
//
// 261116_0003 P2 (plan §4 "Routing", decision D7): REVIVED capability gate.
// The F5 blanket-true made this function dead weight (research-report §8
// item 13: "true for every real target -> the auto->cloud branch is dead
// code") — harmless while the registry equaled the local model's set, wrong
// the moment the registry grew past it. The rule now reads the SAME
// LanguageInfo::engine_mask data the menus filter by (single source of
// truth, no second hardcoded list):
//
//   target carries kLangHyMt2 (Hy-MT2's official 38-target set) AND
//   (source is AUTO - the F1 guard, now a SOURCE-side relaxation only: the
//    model's built-in language ID decides for auto-detected text, so
//    AUTO -> an official target stays local - OR the source ALSO carries
//    kLangHyMt2).
//
// 261116_0003 P2 fix round 2 (PR2-04, SEC-P2-03): the target check runs FIRST.
// The F1 clause used to short-circuit ahead of it, so the DEFAULT source kept
// the degraded local route for the 8 cloud-only rows - plan §4's formula was
// simply not implemented for the most common source value. The order is now
// the formula's order (AND is commutative; the short-circuit is not).
//
// Effect on PlanTranslationRouting (src/engine.cpp:379-399), the seam the
// R6P4 suite pins headlessly:
//   * engine_type=auto + non-official pair (e.g. KO->HU or AUTO->HU): the pair
//     is unreliable -> the auto->cloud branch (engine.cpp:390-393) routes it to
//     Google — choosing auto IS the consent, so no cloud_fallback gate.
//   * explicit local pin + non-official pair: stays on-device unless the
//     user granted cloud consent (the H2 strict-local semantics win).
//   * AUTO source + OFFICIAL target: never flagged unreliable (F1) — no
//     privacy regression for auto-detected text, and no false "VI -> KO"
//     style pair churn. AUTO source + cloud-only target stays unreliable,
//     because the target side is not the language ID's decision to make.
//
// The F5 history this supersedes (kept for the audit trail): the EN-side
// conservative gate pushed pinned non-EN pairs (VI->KO, JA->ZH-CN, ...) onto
// the 041 cloud-leak / 042 degraded route, the user SCOPE DIRECTIVE demanded
// 100% of all Hy-MT2 pairs, and F5 generalized every pinned real pair to
// reliable — including identity pairs (on-device echo). Identity pairs among
// Hy-MT2-official rows stay reliable under the revived rule (both sides
// carry the bit); identity pairs on cloud-only rows (HU->HU) are no longer
// "reliable", but the typing path never reaches routing for them
// (ShouldTranslate bypasses identity first), so the F5 privacy note is
// preserved by the bypass, not by this gate.
//
// Excluded verdicts (unchanged): empty/AUTO TARGET is never reliable
// (auto-detect is never a translation target), and unresolvable source
// tokens normalize to AUTO via NormalizeLanguageCode, keeping the F1 guard.
bool LocalPairReliable(std::string_view src_code, std::string_view tgt_code) {
    const std::string src = NormalizeLanguageCode(src_code);
    const std::string tgt = NormalizeLanguageCode(tgt_code);
    if (tgt.empty() || tgt == "AUTO") {
        return false; // auto-detect is never a translation target
    }
    // 261116_0003 P2 (D7): the pair must live inside Hy-MT2's official set —
    // both sides carry kLangHyMt2 in the registry's engine_mask column.
    // 261116_0003 P2 fix round 2 (PR2-04, SEC-P2-03): the TARGET check comes
    // FIRST so the verdict matches plan §4's formula. The F1 AUTO-source guard
    // below is a SOURCE-side relaxation only: the model's built-in language ID
    // decides the SOURCE, it cannot make an out-of-set TARGET servable. Before
    // the reorder the AUTO early return ran first, so the DEFAULT source kept
    // the degraded local route for the 8 cloud-only rows instead of D7's
    // cloud route.
    const LanguageInfo* tgt_info = FindLanguageByCode(tgt);
    if (tgt_info == nullptr || (tgt_info->engine_mask & kLangHyMt2) == 0) {
        return false; // target outside the official set: the local model was never trained for it
    }
    if (src == "AUTO") {
        return true; // model's built-in language ID handles the SOURCE (F1)
    }
    const LanguageInfo* src_info = FindLanguageByCode(src);
    if (src_info == nullptr || (src_info->engine_mask & kLangHyMt2) == 0) {
        return false; // pinned source outside the official set (cloud-only row)
    }
    return true; // official pair (identity included, on-device echo): reliable
}

// ===========================================================================
// 261116_0003 P9a (plan §3 D5/D6, §5 P9): user-defined languages
//
// Three pure-ish layers, all headless-testable:
//   1. code resolution  - alias matrix (exact) -> installed locale names -> ""
//   2. script inference - from the user-typed native name (fail-open)
//   3. probe evaluation - the three D6 checks over ONE engine call, whose
//                         transport the caller owns (no network in this file)
// plus the config schema parser/serializer (AppConfig, further below) and the
// D5/D7 menu-view merge (data only).
// ===========================================================================
namespace {

// ASCII whitespace trim: the probe checks and the stored user fields are
// trimmed so padding ("  안녕하세요 ") or a stray trailing space cannot flip a
// verdict, and a hand-edited config's " Twi " still resolves.
std::string_view TrimAsciiSpaceView(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

// 261116_0003 P9a fix PR9-03: strips the characters that must never reach a
// stored user-language field: C0/C1 controls + DEL (interior newlines, device
// controls) and the bidi override/isolate marks (U+202A-202E direction
// overrides, U+2066-2069 isolates, U+200E/200F embedding marks) that could
// reorder a menu row or a raw prompt token invisibly. The shared non-validating
// UTF-8 decoder passes malformed bytes through as single raw bytes, so a broken
// sequence (< 0x80, inside the C0 range only when it IS a control) still lands
// in the strip set; valid text passes through unchanged.
// 261116_0003 P9a fix2 PR10-08: the strip set also covers the invisible
// separators a stored name must never carry: U+061C (Arabic letter mark),
// U+200B-U+200D (zero-width space/non-joiner/joiner), U+2028/U+2029 (line and
// paragraph separators), U+FEFF (zero-width no-break space / BOM) and U+00AD
// (soft hyphen). Defense in depth: the enumerated injection set was already
// closed, these are the remaining invisible characters.
std::string SanitizeUserLanguageField(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t start = pos;
        std::size_t consumed = 1;
        const char32_t cp = language_alias_detail::DecodeUtf8CodePoint(text, pos, consumed);
        pos += consumed;
        const bool strip = cp < 0x20u || (cp >= 0x7Fu && cp <= 0x9Fu) ||
                           (cp >= 0x202Au && cp <= 0x202Eu) ||
                           (cp >= 0x2066u && cp <= 0x2069u) ||
                           cp == 0x200Eu || cp == 0x200Fu ||
                           cp == 0x061Cu ||
                           (cp >= 0x200Bu && cp <= 0x200Du) ||
                           cp == 0x2028u || cp == 0x2029u ||
                           cp == 0xFEFFu || cp == 0x00ADu;
        if (!strip) {
            out.append(text.substr(start, consumed));
        }
    }
    return out;
}

// 261116_0003 P9a fix PR9-03: byte cap that never splits a UTF-8 code point (a
// truncated trailing sequence would be invalid text). Walks code points and
// keeps the longest complete prefix within the cap.
std::string CapUserLanguageField(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return std::string(text);
    }
    std::size_t pos = 0;
    std::size_t last_boundary = 0;
    while (pos < text.size()) {
        std::size_t consumed = 1;
        language_alias_detail::DecodeUtf8CodePoint(text, pos, consumed);
        pos += consumed;
        if (pos > max_bytes) {
            break;
        }
        last_boundary = pos;
    }
    return std::string(text.substr(0, last_boundary));
}

// Sanitize-then-cap, the single choke-point cleaning both entry paths
// (ParseUserLanguagesBlock and MakeUserLanguage) apply to a stored field.
std::string CleanUserLanguageField(std::string_view text, std::size_t max_bytes) {
    return CapUserLanguageField(SanitizeUserLanguageField(text), max_bytes);
}

// The script vocabulary: mask bit <-> config token. Order = ascending bit,
// which is also the composite-token ("han+kana") emission order. The BITS come
// from win32_input.hpp and are pinned against engine_core's ScriptClass
// ordinals by tests/ws2_capture_tests.inc, so this table only names them; it
// never renumbers them.
struct ScriptTokenEntry {
    std::uint32_t bit;
    const char* token;
};
constexpr ScriptTokenEntry kScriptTokens[] = {
    {kScriptMaskLatin, "latin"},
    {kScriptMaskHangul, "hangul"},
    {kScriptMaskHan, "han"},
    {kScriptMaskKana, "kana"},
    {kScriptMaskCyrillic, "cyrillic"},
    {kScriptMaskThai, "thai"},
    {kScriptMaskArabic, "arabic"},
    {kScriptMaskDevanagari, "devanagari"},
    {kScriptMaskBengali, "bengali"},
    {kScriptMaskKhmer, "khmer"},
    {kScriptMaskLao, "lao"},
    {kScriptMaskHebrew, "hebrew"},
    {kScriptMaskGreek, "greek"},
    {kScriptMaskMyanmar, "myanmar"},
    {kScriptMaskTamil, "tamil"},
    {kScriptMaskTelugu, "telugu"},
    {kScriptMaskGujarati, "gujarati"},
    {kScriptMaskTibetan, "tibetan"},
    {kScriptMaskArmenian, "armenian"},
    {kScriptMaskGeorgian, "georgian"},
    {kScriptMaskSinhala, "sinhala"},
    {kScriptMaskMalayalam, "malayalam"},
    {kScriptMaskKannada, "kannada"},
    {kScriptMaskGurmukhi, "gurmukhi"},
    {kScriptMaskOdia, "odia"},
    {kScriptMaskEthiopic, "ethiopic"},
};

// Splits a JSON array's text into its top-level OBJECT spans. String-,
// escape-, and comment-aware and brace-balanced, so a nested "probes" array
// belongs to its owning entry and a brace inside a string or a note can never
// truncate a span. Tolerant by design: malformed text yields whatever balanced
// spans exist, and the caller gates each span through the strict reader (a bad
// element is skipped, never a hard load failure - a hand-edited block must not
// brick the config).
// 261116_0003 P9a fix PR9-08: comment-aware. A hand-edited block may carry
// // line or /* */ block notes; a brace inside a note must not perturb the
// depth scan (the live finding: one commented "}" swallowed every following
// entry and the next save persisted the loss). Comments are skipped while
// bracket-scanning, and the strict span reader is comment-aware too
// (SkipWhitespace skips // and /* */ notes between tokens), so a noted entry
// parses normally instead of being dropped: 261116_0003 P9a fix2 PR10-09
// corrects the earlier claim that such an entry fails the span reader.
std::vector<std::string_view> SplitTopLevelJsonObjects(std::string_view array_text) {
    std::vector<std::string_view> objects;
    std::size_t pos = 0;
    std::size_t start = 0;
    int depth = 0;
    bool in_string = false;
    bool in_line_comment = false;
    bool in_block_comment = false;
    while (pos < array_text.size()) {
        const char c = array_text[pos];
        if (in_line_comment) {
            if (c == '\n') {
                in_line_comment = false;
            }
            ++pos;
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && pos + 1 < array_text.size() && array_text[pos + 1] == '/') {
                in_block_comment = false;
                pos += 2;
                continue;
            }
            ++pos;
            continue;
        }
        if (in_string) {
            if (c == '\\' && pos + 1 < array_text.size()) {
                pos += 2; // skip the escaped character (may be an escaped quote)
                continue;
            }
            if (c == '\"') {
                in_string = false;
            }
            ++pos;
            continue;
        }
        if (c == '\"') {
            in_string = true;
        } else if (c == '/' && pos + 1 < array_text.size() && array_text[pos + 1] == '/') {
            in_line_comment = true;
            pos += 2;
            continue;
        } else if (c == '/' && pos + 1 < array_text.size() && array_text[pos + 1] == '*') {
            in_block_comment = true;
            pos += 2;
            continue;
        } else if (c == '{') {
            if (depth == 0) {
                start = pos;
            }
            ++depth;
        } else if (c == '}') {
            if (depth > 0) {
                --depth;
                if (depth == 0) {
                    objects.push_back(array_text.substr(start, pos + 1 - start));
                }
            }
        }
        ++pos;
    }
    return objects;
}

// One object span through the strict reader. The reader keeps a VIEW of the
// text it parses, so the storage is owned here and outlives the call.
bool ParseJsonObjectSpan(std::string_view span,
                         std::vector<std::pair<std::string, std::string>>& out) {
    const std::string text(span);
    SimpleJsonReader reader(text);
    return reader.ParseObject(out);
}

// Probe-status token <-> stored enum. "checking" and "user_overridden" are
// runtime/display states with no persisted direction: both read back as
// Unknown so a crashed run or a hand-edit cannot leave a language stuck.
UserLanguageProbeStatus ParseProbeStatusToken(std::string_view token) {
    if (EqualsIgnoreCaseAscii<char>(token, "supported")) {
        return UserLanguageProbeStatus::Supported;
    }
    if (EqualsIgnoreCaseAscii<char>(token, "unsupported")) {
        return UserLanguageProbeStatus::Unsupported;
    }
    return UserLanguageProbeStatus::Unknown;
}

const char* ProbeStatusToken(UserLanguageProbeStatus status) {
    switch (status) {
        case UserLanguageProbeStatus::Supported: return "supported";
        case UserLanguageProbeStatus::Unsupported: return "unsupported";
        case UserLanguageProbeStatus::Checking:
        case UserLanguageProbeStatus::UserOverridden:
        case UserLanguageProbeStatus::Unknown:
            break;
    }
    return "unknown";
}

// A registry code token, canonicalized. "" for anything the registry does not
// hold (D5's "free-form engines only") and for AUTO (a user language is a real
// target language; auto-detect is a source marker).
std::string NormalizeRegisteredUserLanguageCode(std::string_view token) {
    const LanguageInfo* row = FindLanguageByCode(TrimAsciiSpaceView(token));
    if (row == nullptr || row->code == "AUTO") {
        return {};
    }
    return row->code;
}

// D5 stage 2 helper: the registry code implied by a locale name's primary
// language subtag ("ko-KR" -> "KO"). Region/script variants whose primary
// subtag has no row ("zh-Hans-CN" -> "zh") return "", but those names are
// already reachable through the alias matrix (stage 1), so the ladder never
// loses them.
std::string RegistryCodeFromLocaleName(std::string_view locale) {
    std::string primary;
    primary.reserve(locale.size());
    for (char c : locale) {
        if (c == '-' || c == '_') {
            break;
        }
        primary.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    if (primary.empty()) {
        return {};
    }
    const LanguageInfo* row = FindLanguageByCode(primary);
    if (row == nullptr || row->code == "AUTO") {
        return {};
    }
    return row->code;
}

// GetLocaleInfoEx into UTF-8. Returns "" when the query fails or yields an
// empty name (a locale Windows cannot name is simply not a name-matching
// candidate).
std::string LocaleInfoName(const wchar_t* locale_name, LCTYPE info_type) {
    const int needed = ::GetLocaleInfoEx(locale_name, info_type, nullptr, 0);
    if (needed <= 1) {
        return {};
    }
    std::wstring buffer(static_cast<std::size_t>(needed), L'\0');
    const int written = ::GetLocaleInfoEx(locale_name, info_type, buffer.data(), needed);
    if (written <= 0) {
        return {};
    }
    buffer.resize(static_cast<std::size_t>(written - 1)); // the reported size includes the NUL
    return ToUtf8(buffer);
}

inline constexpr std::size_t kMaxEnumeratedLocales = 2048;

BOOL CALLBACK CollectLocaleLanguageName(LPWSTR locale_name, DWORD /*flags*/, LPARAM lparam) {
    auto* entries = reinterpret_cast<std::vector<LocaleLanguageName>*>(lparam);
    if (entries == nullptr || locale_name == nullptr) {
        return FALSE; // stop: nothing sane to collect into
    }
    if (entries->size() >= kMaxEnumeratedLocales) {
        return FALSE; // bound reached (defensive: a real install has ~200 locales)
    }
    LocaleLanguageName entry;
    entry.locale = ToUtf8(std::wstring_view(locale_name));
    entry.english = LocaleInfoName(locale_name, LOCALE_SENGLISHLANGUAGENAME);
    entry.native = LocaleInfoName(locale_name, LOCALE_SNATIVELANGUAGENAME);
    entries->push_back(std::move(entry));
    return TRUE;
}

// The probe-cache slot for (engine, model), created on demand. nullptr when the
// entry is already at kMaxUserLanguageProbes and the key is new.
// 261116_0003 P9b-3 PR10-07: the runtime write seam caps its own key fields
// (the load path caps at parse; nothing capped a live call). The cap runs
// BEFORE the comparison, so an over-long model hits its own truncated slot
// instead of splitting one logical key across two entries.
UserLanguageProbe* UpsertProbe(UserLanguage& language,
                               std::string_view engine,
                               std::string_view model) {
    const UserLanguageProbeKey key = MakeUserLanguageProbeKey(engine, model);
    if (key.engine.empty()) {
        return nullptr; // the engine id IS the cache key
    }
    for (UserLanguageProbe& probe : language.probes) {
        if (probe.engine == key.engine && probe.model == key.model) {
            return &probe;
        }
    }
    if (language.probes.size() >= kMaxUserLanguageProbes) {
        return nullptr;
    }
    UserLanguageProbe probe;
    probe.engine = key.engine;
    probe.model = key.model;
    language.probes.push_back(std::move(probe));
    return &language.probes.back();
}

// The per-entry "probes" cache array (defined below the entry parser).
void ParseUserLanguageProbesBlock(const std::string& raw, std::vector<UserLanguageProbe>& out);

// One "user_languages" array element. A nullopt-style absence of the derived
// keys (code / script) is re-derived, which keeps a hand-written entry usable;
// an EXPLICIT code (including "") and a recognized NON-ZERO script token are
// trusted verbatim, because a recorded decision must not silently change under
// the user. A zero/unknown script token is re-inferred (fail-open recovery).
// 261116_0003 P9a fix PR9-03: the stored names go through the sanitizer/cap
// (controls + bidi marks stripped, byte-capped) so a hand-edited file cannot
// smuggle prompt-reordering characters or unbounded text into the block.
void ParseUserLanguagesBlock(const std::string& raw, std::vector<UserLanguage>& out) {
    for (const std::string_view span : SplitTopLevelJsonObjects(raw)) {
        if (out.size() >= kMaxUserLanguages) {
            break;
        }
        std::vector<std::pair<std::string, std::string>> pairs;
        if (!ParseJsonObjectSpan(span, pairs)) {
            continue;
        }
        UserLanguage language;
        bool has_code_key = false;
        bool has_script_key = false;
        for (const auto& [k, v] : pairs) {
            if (k == "native_name") {
                language.native_name =
                    CleanUserLanguageField(TrimAsciiSpaceView(v), kMaxUserLanguageNameBytes);
            } else if (k == "english_name") {
                language.english_name =
                    CleanUserLanguageField(TrimAsciiSpaceView(v), kMaxUserLanguageNameBytes);
            } else if (k == "code") {
                language.code = NormalizeRegisteredUserLanguageCode(v);
                has_code_key = true;
            } else if (k == "script") {
                language.script_mask = UserLanguageScriptMaskFromToken(v);
                has_script_key = true;
            } else if (k == "probes") {
                ParseUserLanguageProbesBlock(v, language.probes);
            }
        }
        if (language.native_name.empty()) {
            continue; // the one required field: an unnamed entry cannot be offered
        }
        if (!has_code_key) {
            language.code =
                ResolveUserLanguageCode(language.native_name, language.english_name).code;
        }
        if (!has_script_key || language.script_mask == 0) {
            language.script_mask = InferUserLanguageScript(language.native_name);
            if (language.script_mask == 0 && !language.code.empty()) {
                language.script_mask = ScriptMaskForLanguageCode(language.code);
            }
        }
        out.push_back(std::move(language));
    }
}

void ParseUserLanguageProbesBlock(const std::string& raw, std::vector<UserLanguageProbe>& out) {
    for (const std::string_view span : SplitTopLevelJsonObjects(raw)) {
        if (out.size() >= kMaxUserLanguageProbes) {
            break;
        }
        std::vector<std::pair<std::string, std::string>> pairs;
        if (!ParseJsonObjectSpan(span, pairs)) {
            continue;
        }
        UserLanguageProbe probe;
        for (const auto& [k, v] : pairs) {
            // 261116_0003 P9a fix PR9-05: every string field is byte-capped at
            // the parse choke point (engine is the stable cache key token,
            // model/evidence/checked_at are cache metadata).
            if (k == "engine") {
                probe.engine = CapUserLanguageField(TrimAsciiSpaceView(v),
                                                    kMaxUserLanguageProbeEngineBytes);
            } else if (k == "model") {
                probe.model = CapUserLanguageField(v, kMaxUserLanguageProbeModelBytes);
            } else if (k == "status") {
                probe.status = ParseProbeStatusToken(v);
            } else if (k == "override") {
                probe.override_status = ParseProbeStatusToken(v);
            } else if (k == "checked_at") {
                probe.checked_at = CapUserLanguageField(TrimAsciiSpaceView(v),
                                                        kMaxUserLanguageProbeCheckedAtBytes);
            } else if (k == "evidence") {
                probe.evidence = CapUserLanguageField(v, kMaxUserLanguageProbeEvidenceBytes);
            }
        }
        if (probe.engine.empty()) {
            continue; // not addressable without its cache key
        }
        // 261116_0003 P9a fix PR9-05: duplicate (engine, model) keys within one
        // entry keep the FIRST occurrence (a shadowed verdict must not flip the
        // cached answer, and duplicates must not exhaust the slot cap).
        bool duplicate_key = false;
        for (const UserLanguageProbe& existing : out) {
            if (existing.engine == probe.engine && existing.model == probe.model) {
                duplicate_key = true;
                break;
            }
        }
        if (duplicate_key) {
            continue;
        }
        out.push_back(std::move(probe));
    }
}

// 261122_0001 B-U1 (design-pick §1/§2): one "openai_profiles" array entry.
// The entry body is the openai block shape plus "name": every non-name key is
// routed through ApplyOpenAiConfigPair verbatim (identical semantics to the
// active block, key material included — the DPAPI blob and digest round-trip
// as opaque strings and are never logged). The name is byte-capped on a UTF-8
// code-point boundary at the parse choke point; control/bidi sanitizing is the
// dialog's save-time duty (design-pick §7), NOT the load path's.
void ParseOpenAiProfileEntry(const std::vector<std::pair<std::string, std::string>>& pairs,
                             OpenAiProfile& out) {
    for (const auto& [k, v] : pairs) {
        if (k == "name") {
            out.name = CapUserLanguageField(v, kMaxOpenAiProfileNameBytes);
        } else {
            ApplyOpenAiConfigPair(k, v, out.cfg);
        }
    }
}

// 261122_0001 B-U1 (design-pick §1/§2): the "openai_profiles" raw array
// (captured verbatim by the top-level reader, the openai_declared_languages
// posture). Lenient by contract: a non-array value reads as an EMPTY library
// (the '[' gate below — an object-typed value must not synthesize entries out
// of its braces); a malformed single entry span is SKIPPED, never a hard load
// failure (a hand-edited block must not brick the config, the
// ParseUserLanguagesBlock precedent); entries past kMaxOpenAiProfiles are
// dropped. Never logs content — key fields are opaque material.
void ParseOpenAiProfilesBlock(const std::string& raw, std::vector<OpenAiProfile>& out) {
    const std::string_view trimmed = TrimAsciiSpaceView(raw);
    if (trimmed.empty() || trimmed.front() != '[') {
        return; // not an array: empty library
    }
    for (const std::string_view span : SplitTopLevelJsonObjects(trimmed)) {
        if (out.size() >= kMaxOpenAiProfiles) {
            break;
        }
        std::vector<std::pair<std::string, std::string>> pairs;
        if (!ParseJsonObjectSpan(span, pairs)) {
            continue; // malformed entry: skip it, keep the rest
        }
        OpenAiProfile profile;
        ParseOpenAiProfileEntry(pairs, profile);
        out.push_back(std::move(profile));
    }
}

// 261116_0003 P9a fix2 PR10-04: skips ASCII whitespace plus // line and
// /* */ block notes from `pos`. Returns false when a block note is never
// terminated (the caller then treats the text as malformed).
bool SkipJsonTrivia(std::string_view text, std::size_t& pos) {
    while (pos < text.size()) {
        const char c = text[pos];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++pos;
        } else if (c == '/' && pos + 1 < text.size() && text[pos + 1] == '/') {
            pos += 2;
            while (pos < text.size() && text[pos] != '\n') {
                ++pos;
            }
        } else if (c == '/' && pos + 1 < text.size() && text[pos + 1] == '*') {
            pos += 2;
            while (pos + 1 < text.size() && !(text[pos] == '*' && text[pos + 1] == '/')) {
                ++pos;
            }
            if (pos + 1 >= text.size()) {
                pos = text.size();
                return false; // unterminated block note
            }
            pos += 2;
        } else {
            break;
        }
    }
    return true;
}

// 261116_0003 P9a fix2 PR10-04: the index just past the matching ']' of the
// array group that STARTS the text, 0 when there is none. String- and
// comment-aware (the same discipline as the value capture) so a "]" inside a
// stored name or a note cannot close the group early.
std::size_t JsonArrayGroupEnd(std::string_view text) {
    if (text.empty() || text.front() != '[') {
        return 0;
    }
    // 261116_0003 P9a fix3 PR11-02: the unmatched openers live on a stack so
    // a closer must match its OWN group. The former single depth counter
    // accepted a mismatched closer ("[1}", "[{\"a\":1]") as this group's end,
    // and the carrier round-tripped strict-JSON-invalid text verbatim.
    std::vector<char> openers;
    bool in_string = false;
    bool in_line_comment = false;
    bool in_block_comment = false;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const char c = text[pos];
        if (in_line_comment) {
            if (c == '\n') {
                in_line_comment = false;
            }
            ++pos;
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && pos + 1 < text.size() && text[pos + 1] == '/') {
                in_block_comment = false;
                pos += 2;
                continue;
            }
            ++pos;
            continue;
        }
        if (in_string) {
            if (c == '\\' && pos + 1 < text.size()) {
                pos += 2; // skip the escaped character
                continue;
            }
            if (c == '\"') {
                in_string = false;
            }
            ++pos;
            continue;
        }
        if (c == '\"') {
            in_string = true;
        } else if (c == '/' && pos + 1 < text.size() && text[pos + 1] == '/') {
            in_line_comment = true;
            pos += 2;
            continue;
        } else if (c == '/' && pos + 1 < text.size() && text[pos + 1] == '*') {
            in_block_comment = true;
            pos += 2;
            continue;
        } else if (c == '{' || c == '[') {
            openers.push_back(c);
        } else if (c == '}' || c == ']') {
            if (openers.empty() ||
                (openers.back() == '{' && c != '}') ||
                (openers.back() == '[' && c != ']')) {
                return 0; // a closer with the wrong opener: malformed
            }
            openers.pop_back();
            if (openers.empty()) {
                return pos + 1; // the outermost '[' just closed: the group ends
            }
        }
        ++pos;
    }
    return 0;
}

// 261116_0003 P9a fix2 PR10-04: carrier eligibility is bracket-BALANCE based,
// not a front/back character test. Two hand-edit shapes defeated that test: a
// trailing note after the array ("[1] // written by a newer build") left the
// span's last byte inside the note, so the future block was silently dropped,
// and a multi-group span ("[1] [2]") was accepted as if it were one array.
// The scan finds the first group's matching ']' and accepts only when nothing
// but trivia follows it; `out_span` is that balanced group's exact bytes (the
// trailing note is NOT part of the preserved value, so the re-emitted line
// stays valid JSON).
bool IsSingleJsonArraySpan(std::string_view text, std::string& out_span) {
    const std::size_t end = JsonArrayGroupEnd(text);
    if (end == 0) {
        return false;
    }
    std::size_t tail = end;
    if (!SkipJsonTrivia(text, tail) || tail != text.size()) {
        return false; // a second group or trailing junk: not one array value
    }
    out_span = std::string(text.substr(0, end));
    return true;
}

// Serializes the block: an empty array as "[]", otherwise the writer's exact
// emission shape (schema doc in config.hpp). A newer-schema block is returned
// verbatim (forward-compat preservation).
std::string SerializeUserLanguages(const std::vector<UserLanguage>& languages,
                                   const std::string& forward_compat_json) {
    if (!forward_compat_json.empty()) {
        return forward_compat_json;
    }
    if (languages.empty()) {
        return "[]";
    }
    std::ostringstream ss;
    ss << "[\n";
    for (std::size_t i = 0; i < languages.size(); ++i) {
        const UserLanguage& language = languages[i];
        ss << "    {\n";
        ss << "      \"native_name\": \"" << EscapeJsonString(language.native_name) << "\",\n";
        ss << "      \"english_name\": \"" << EscapeJsonString(language.english_name) << "\",\n";
        ss << "      \"code\": \"" << EscapeJsonString(language.code) << "\",\n";
        ss << "      \"script\": \"" << EscapeJsonString(UserLanguageScriptToken(language.script_mask))
           << "\",\n";
        ss << "      \"probes\": ";
        if (language.probes.empty()) {
            ss << "[]";
        } else {
            ss << "[\n";
            for (std::size_t p = 0; p < language.probes.size(); ++p) {
                const UserLanguageProbe& probe = language.probes[p];
                ss << "        {\"engine\": \"" << EscapeJsonString(probe.engine)
                   << "\", \"model\": \"" << EscapeJsonString(probe.model)
                   << "\", \"status\": \"" << ProbeStatusToken(probe.status)
                   << "\", \"override\": \"" << ProbeStatusToken(probe.override_status)
                   << "\", \"checked_at\": \"" << EscapeJsonString(probe.checked_at)
                   << "\", \"evidence\": \"" << EscapeJsonString(probe.evidence) << "\"}"
                   << (p + 1 < language.probes.size() ? "," : "") << "\n";
            }
            ss << "      ]";
        }
        ss << "\n    }" << (i + 1 < languages.size() ? "," : "") << "\n";
    }
    ss << "  ]";
    return ss.str();
}

// 261122_0001 B-U1 (design-pick §1): the "openai_profiles" writer for a
// NON-EMPTY library; the caller gates on that (an empty-but-touched library is
// emitted by the caller as an explicit [], 261122_0001 fix-A1 R1-12, never
// through this function). The entry shape is name + the 6-field openai block
// in the block's own field order, so an entry differs from the active block
// only by the extra name line. Key material is emitted as the same opaque
// dpapi/sha256 pair as the active block, never cleartext and never any other
// field.
std::string SerializeOpenAiProfiles(const std::vector<OpenAiProfile>& profiles) {
    std::ostringstream ss;
    ss << "[\n";
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        const OpenAiProfile& profile = profiles[i];
        ss << "    {\n";
        ss << "      \"name\": \"" << EscapeJsonString(profile.name) << "\",\n";
        ss << "      \"base_url\": \"" << EscapeJsonString(profile.cfg.base_url) << "\",\n";
        ss << "      \"model\": \"" << EscapeJsonString(profile.cfg.model) << "\",\n";
        ss << "      \"reasoning_effort\": \"" << EscapeJsonString(profile.cfg.reasoning_effort)
           << "\",\n";
        ss << "      \"api_key_dpapi\": \"" << EscapeJsonString(profile.cfg.api_key_dpapi)
           << "\",\n";
        ss << "      \"api_key_sha256\": \"" << EscapeJsonString(profile.cfg.api_key_sha256)
           << "\",\n";
        ss << "      \"http_consent_given\": "
           << (profile.cfg.http_consent_given ? "true" : "false") << "\n";
        ss << "    }" << (i + 1 < profiles.size() ? "," : "") << "\n";
    }
    ss << "  ]";
    return ss.str();
}

} // namespace

std::string UserLanguageStoredName(const UserLanguage& language) {
    return language.english_name.empty() ? language.native_name : language.english_name;
}

const UserLanguage* FindUserLanguage(const std::vector<UserLanguage>& languages,
                                     std::string_view token) {
    const std::string_view trimmed = TrimAsciiSpaceView(token);
    if (trimmed.empty()) {
        return nullptr;
    }
    for (const UserLanguage& language : languages) {
        if (language.native_name.empty()) {
            continue;
        }
        if (EqualsIgnoreCaseAscii<char>(UserLanguageStoredName(language), trimmed) ||
            EqualsIgnoreCaseAscii<char>(language.native_name, trimmed) ||
            EqualsIgnoreCaseAscii<char>(language.english_name, trimmed) ||
            (!language.code.empty() && EqualsIgnoreCaseAscii<char>(language.code, trimmed))) {
            return &language;
        }
    }
    return nullptr;
}

std::string UserLanguageEngineId(LanguageEngine engine) {
    switch (engine) {
        case LanguageEngine::Google: return "google";
        case LanguageEngine::OpenAi: return "openai";
        case LanguageEngine::UserGguf: return "user_gguf";
        case LanguageEngine::HyMt2:
        case LanguageEngine::Auto:
            return {}; // D7: the local built-in engine has no custom row and no probe
    }
    return {};
}

std::string UserLanguageScriptToken(std::uint32_t script_mask) {
    std::string token;
    for (const ScriptTokenEntry& entry : kScriptTokens) {
        if ((script_mask & entry.bit) == 0) {
            continue;
        }
        if (!token.empty()) {
            token.push_back('+');
        }
        token += entry.token;
    }
    return token;
}

std::uint32_t UserLanguageScriptMaskFromToken(std::string_view token) {
    std::uint32_t mask = 0;
    std::string_view rest = TrimAsciiSpaceView(token);
    while (!rest.empty()) {
        const std::size_t plus = rest.find('+');
        const std::string_view part = TrimAsciiSpaceView(rest.substr(0, plus));
        for (const ScriptTokenEntry& entry : kScriptTokens) {
            if (EqualsIgnoreCaseAscii<char>(part, entry.token)) {
                mask |= entry.bit;
                break;
            }
        }
        if (plus == std::string_view::npos) {
            break;
        }
        rest = TrimAsciiSpaceView(rest.substr(plus + 1));
    }
    return mask;
}

std::uint32_t InferUserLanguageScript(std::string_view native_name) {
    const std::string_view trimmed = TrimAsciiSpaceView(native_name);
    if (trimmed.empty()) {
        return 0; // fail-open: nothing to classify
    }
    // Reuses the app's single script classifier (smart_bypass predicates via
    // win32_input.hpp's mask), so a name is classified exactly like captured
    // text. Mixed names OR their classes instead of guessing one; a name with
    // no script character at all (digits/punctuation) yields 0.
    return ScriptMaskOfText(ToUtf16(trimmed));
}

std::vector<LocaleLanguageName> EnumerateSystemLocaleLanguages() {
    std::vector<LocaleLanguageName> entries;
    entries.reserve(256);
    ::EnumSystemLocalesEx(&CollectLocaleLanguageName, LOCALE_ALL,
                          reinterpret_cast<LPARAM>(&entries), nullptr);
    return entries;
}

std::string ResolveCodeFromLocaleLanguages(std::string_view native_name,
                                           std::string_view english_name,
                                           const std::vector<LocaleLanguageName>& entries) {
    const std::string_view native_trimmed = TrimAsciiSpaceView(native_name);
    const std::string_view english_trimmed = TrimAsciiSpaceView(english_name);
    if (native_trimmed.empty() && english_trimmed.empty()) {
        return {}; // blank never matches a blank/unnamed locale entry
    }
    for (const LocaleLanguageName& entry : entries) {
        const std::string_view entry_english = TrimAsciiSpaceView(entry.english);
        const std::string_view entry_native = TrimAsciiSpaceView(entry.native);
        const bool matches =
            (!native_trimmed.empty() &&
             (EqualsIgnoreCaseAscii<char>(entry_native, native_trimmed) ||
              EqualsIgnoreCaseAscii<char>(entry_english, native_trimmed))) ||
            (!english_trimmed.empty() &&
             (EqualsIgnoreCaseAscii<char>(entry_native, english_trimmed) ||
              EqualsIgnoreCaseAscii<char>(entry_english, english_trimmed)));
        if (!matches) {
            continue;
        }
        const std::string code = RegistryCodeFromLocaleName(entry.locale);
        if (!code.empty()) {
            return code; // a name-matched locale whose primary subtag is a registry row
        }
    }
    return {};
}

UserLanguageResolution ResolveUserLanguageCode(std::string_view native_name,
                                              std::string_view english_name,
                                              LocaleLanguageSource locale_source) {
    UserLanguageResolution resolution;
    // Stage 1 (D5): the alias matrix, EXACT matches only. Prefix/substring are
    // the picker's search affordance; resolving a typo to a language the user
    // did not name would be worse than leaving the entry free-form.
    const std::string_view candidates[] = {TrimAsciiSpaceView(native_name),
                                           TrimAsciiSpaceView(english_name)};
    const std::vector<const LanguageInfo*>& pool = AllTargetView();
    for (const std::string_view candidate : candidates) {
        if (candidate.empty()) {
            continue;
        }
        const std::vector<LanguageSearchHit> hits = SearchLanguages(candidate, pool);
        for (const LanguageSearchHit& hit : hits) {
            // 261116_0003 P9a fix PR9-02: AliasTier::None is the empty-query
            // "unfiltered pool" row, not a match. A candidate that FOLDS to
            // empty (ZWSP-only U+200B, NBSP-only U+00A0) lands in that branch
            // and would otherwise resolve to the first pool row; the picker's
            // no-filter affordance stays untouched (SearchLanguages itself is
            // not changed).
            if (hit.match != AliasMatchKind::Exact || hit.tier == AliasTier::None ||
                hit.language == nullptr) {
                continue;
            }
            resolution.code = hit.language->code;
            resolution.english_name = hit.language->name_en;
            resolution.from_alias = true;
            return resolution;
        }
    }
    // Stage 2 (D5): the installed system locale names.
    if (locale_source != nullptr) {
        const std::string code = ResolveCodeFromLocaleLanguages(native_name, english_name,
                                                                locale_source());
        if (!code.empty()) {
            const LanguageInfo* row = FindLanguageByCode(code);
            resolution.code = code;
            resolution.english_name = row != nullptr ? row->name_en : std::string();
            resolution.from_locale = true;
            return resolution;
        }
    }
    // Stage 3: no code. Legal D5 outcome: the free-form engines can still serve
    // the language, Google cannot (BuildLanguageMenuView enforces that).
    return resolution;
}

UserLanguageResolution ResolveUserLanguageCode(std::string_view native_name,
                                              std::string_view english_name) {
    return ResolveUserLanguageCode(native_name, english_name, &EnumerateSystemLocaleLanguages);
}

// 261116_0003 P9a fix2 PR10-05: the D5 ladder's locale stage is injectable (the
// same seam ResolveUserLanguageCode already exposes), so the PR9-07 English
// back-fill pin resolves a stage-2-only name deterministically instead of
// depending on which locales this machine happens to have installed.
UserLanguage MakeUserLanguage(std::string_view native_name, std::string_view english_name) {
    return MakeUserLanguage(native_name, english_name, &EnumerateSystemLocaleLanguages);
}

UserLanguage MakeUserLanguage(std::string_view native_name,
                              std::string_view english_name,
                              LocaleLanguageSource locale_source) {
    UserLanguage language;
    // 261116_0003 P9a fix PR9-03: the typed fields are sanitized + capped at
    // the build choke point, exactly like the parse path (both paths feed the
    // stored form the prompt injects).
    language.native_name =
        CleanUserLanguageField(TrimAsciiSpaceView(native_name), kMaxUserLanguageNameBytes);
    language.english_name =
        CleanUserLanguageField(TrimAsciiSpaceView(english_name), kMaxUserLanguageNameBytes);
    if (language.native_name.empty()) {
        return language; // callers reject it (no required field)
    }
    const UserLanguageResolution resolution =
        ResolveUserLanguageCode(language.native_name, language.english_name, locale_source);
    language.code = resolution.code;
    if (!language.code.empty() && language.english_name.empty() &&
        !resolution.english_name.empty()) {
        // The stored form is what the prompt injects; the English template must
        // receive an English name (Task 1: native-script names caused
        // code-switching in the small local model). Filling the resolved
        // registry name keeps that guarantee without asking the user for it.
        // 261116_0003 P9a fix PR9-07: the back-fill covers the locale-fallback
        // stage too, not only the alias matrix (a native-script name that
        // resolves via a system locale kept its native stored name before).
        language.english_name = resolution.english_name;
    }
    language.script_mask = InferUserLanguageScript(language.native_name);
    if (language.script_mask == 0 && !language.code.empty()) {
        language.script_mask = ScriptMaskForLanguageCode(language.code);
    }
    return language;
}

namespace {

// 261116_0003 P9b-3 PR9-11: the spellings the app itself uses for its AUTO row
// are reserved. Creation refuses them because such an entry is offered by the
// free-form menus but refused by the planner (offers=1 / valid=0 - a dead menu
// row). The table is the AUTO registry row's two names plus its code word, so a
// rename of that row must be mirrored here; the P9b-3 suite pins the equality
// against kAllLanguages.
constexpr const char* kReservedUserLanguageNames[] = {
    "Auto Detect",  // LanguageInfo::name_en of the AUTO row
    "AUTO",         // LanguageInfo::code of the AUTO row
    "자동 감지",     // LanguageInfo::name_native of the AUTO row
};

bool IsReservedUserLanguageName(std::string_view candidate) {
    const std::string_view trimmed = TrimAsciiSpaceView(candidate);
    if (trimmed.empty()) {
        return false;
    }
    for (const char* reserved : kReservedUserLanguageNames) {
        if (EqualsIgnoreCaseAscii<char>(trimmed, reserved)) {
            return true;
        }
    }
    return false;
}

// 261116_0003 P9b-3: the write seam's own cleaning, identical to what
// MakeUserLanguage applies to the two typed fields (sanitize + trim + byte
// cap). Applied by every locked mutator, so a hand-built UserLanguage cannot
// bypass the storage-side strip/cap the dialog path already inherits.
void CleanUserLanguageNamesAtWrite(UserLanguage& language) {
    language.native_name =
        CleanUserLanguageField(TrimAsciiSpaceView(language.native_name),
                               kMaxUserLanguageNameBytes);
    language.english_name =
        CleanUserLanguageField(TrimAsciiSpaceView(language.english_name),
                               kMaxUserLanguageNameBytes);
}

} // namespace

// 261116_0003 P9b-3 PR9-11 (creation-time refusal; contract in config.hpp):
// reserved AUTO spellings first, then registry/user duplicates. The order is
// load-bearing - "AUTO" is also the AUTO registry row's code, so a duplicate
// check first would report the reserved spelling as an ordinary duplicate and
// the dialog would render the wrong reason.
UserLanguageNameVerdict ValidateUserLanguageName(std::string_view native_name,
                                                 std::string_view english_name,
                                                 const std::vector<UserLanguage>& existing,
                                                 std::size_t ignore_index) {
    // The same folding the sanitizer/storage path applies, so a name that only
    // becomes a reserved spelling after the invisible/control strip (or after
    // trimming) is refused too.
    UserLanguage candidate;
    candidate.native_name = CleanUserLanguageField(TrimAsciiSpaceView(native_name),
                                                  kMaxUserLanguageNameBytes);
    candidate.english_name = CleanUserLanguageField(TrimAsciiSpaceView(english_name),
                                                    kMaxUserLanguageNameBytes);
    const std::string_view spellings[] = {candidate.native_name, candidate.english_name};

    for (const std::string_view spelling : spellings) {
        if (IsReservedUserLanguageName(spelling)) {
            return UserLanguageNameVerdict::Reserved;
        }
    }
    for (const std::string_view spelling : spellings) {
        if (spelling.empty()) {
            continue;
        }
        // User side: the same four-spelling lookup the menus dispatch with. A
        // hit that IS the entry being updated is that entry's own current name
        // (a no-op rename), not a duplicate; the FIRST hit wins otherwise,
        // exactly like every other user-language lookup in the app.
        const UserLanguage* hit = FindUserLanguage(existing, spelling);
        if (hit != nullptr &&
            (ignore_index >= existing.size() || hit != &existing[ignore_index])) {
            return UserLanguageNameVerdict::Duplicate;
        }
        // Registry side: the stored (English) name, the native name and the
        // code row - the three spellings BuildLanguageMenuView resolves a user
        // entry against. An unmatched name is NOT an AUTO fallback here
        // (NormalizeLanguageCode would map it to AUTO and refuse everything),
        // hence the two specific lookups.
        if (FindLanguageByName(spelling) != nullptr ||
            FindLanguageByCode(spelling) != nullptr) {
            return UserLanguageNameVerdict::Duplicate;
        }
    }
    return UserLanguageNameVerdict::Accepted;
}

ProbeEvaluation EvaluateProbeResult(std::string_view input_sentence,
                                   std::string_view output_text,
                                   std::uint32_t expected_script_mask) {
    ProbeEvaluation evaluation;
    const std::string_view input_trimmed = TrimAsciiSpaceView(input_sentence);
    const std::string_view output_trimmed = TrimAsciiSpaceView(output_text);

    evaluation.non_empty = !output_trimmed.empty();
    evaluation.differs = !input_trimmed.empty() && output_trimmed != input_trimmed;
    if (evaluation.non_empty) {
        evaluation.detected_mask = ScriptMaskOfText(ToUtf16(output_trimmed));
    }
    // Check 3 fails OPEN when the expected script is unknown (inference is
    // best-effort by D5): the two content checks still decide, and the evidence
    // records "script=na" so the UI can say the check was unavailable.
    evaluation.script_ok =
        (expected_script_mask == 0) || ((evaluation.detected_mask & expected_script_mask) != 0);

    // Shape-only evidence: check bits plus script NAMES, never the probe text or
    // the model output (no user content, no PII, no prompt echo in the log line
    // P9b may surface).
    std::string evidence = "nonempty=";
    evidence += evaluation.non_empty ? "1" : "0";
    evidence += " differs=";
    evidence += evaluation.differs ? "1" : "0";
    evidence += " script=";
    if (expected_script_mask == 0) {
        evidence += "na";
    } else {
        evidence += evaluation.script_ok ? "1" : "0";
    }
    const std::string expected_token = UserLanguageScriptToken(expected_script_mask);
    const std::string detected_token = UserLanguageScriptToken(evaluation.detected_mask);
    evidence += " expected=";
    evidence += expected_token.empty() ? "-" : expected_token;
    evidence += " detected=";
    evidence += detected_token.empty() ? "-" : detected_token;

    if (input_trimmed.empty()) {
        // Internal-invariant breach (the probe sentence is a constant): report
        // it instead of pretending the output differed from "nothing".
        evaluation.status = UserLanguageProbeStatus::Unsupported;
        evidence += " reason=noinput";
    } else if (!evaluation.non_empty) {
        evaluation.status = UserLanguageProbeStatus::Unsupported;
        evidence += " reason=empty";
    } else if (!evaluation.differs) {
        evaluation.status = UserLanguageProbeStatus::Unsupported;
        evidence += " reason=echo";
    } else if (!evaluation.script_ok) {
        evaluation.status = UserLanguageProbeStatus::Unsupported;
        evidence += " reason=script";
    } else {
        evaluation.status = UserLanguageProbeStatus::Supported;
    }
    evaluation.evidence = std::move(evidence);
    return evaluation;
}

// 261116_0003 P9b-3 PR10-07: the single canonicalisation point for a probe
// cache key. Both fields are cleaned exactly like the load path
// (CapUserLanguageField; the engine id is also trimmed, as on load), so the
// free record verbs, the AppConfig write verbs and the parser all agree on the
// same slot for the same logical key.
UserLanguageProbeKey MakeUserLanguageProbeKey(std::string_view engine,
                                              std::string_view model) {
    UserLanguageProbeKey key;
    key.engine =
        CapUserLanguageField(TrimAsciiSpaceView(engine), kMaxUserLanguageProbeEngineBytes);
    key.model = CapUserLanguageField(model, kMaxUserLanguageProbeModelBytes);
    return key;
}

// 261116_0003 P9b-3 F-1 (R15-10): the READ seam canonicalises too. PR10-07 made
// every WRITE address the capped slot, but this reader still compared the RAW
// arguments, and the model half reaches every caller uncapped (openai.model at
// config.cpp:665 and the user-.gguf registry id at :3437 parse without a cap).
// An over-cap model therefore recorded its verdict in a slot no lookup could
// find: the first-use probe re-fired on every pick (one engine call per pick
// against the user's quota) and no badge/manager row could show the verdict.
// Reading through the SAME key builder as the writers closes that split. This is
// the single accessor behind all three read surfaces (tray badge, manager
// dialog, runner trigger) and behind the write verbs' own capacity checks, so no
// caller needs to know the cap.
const UserLanguageProbe* FindUserLanguageProbe(const UserLanguage& language,
                                               std::string_view engine,
                                               std::string_view model) {
    // Idempotent by construction: CapUserLanguageField returns an in-cap string
    // unchanged and TrimAsciiSpaceView trims to a fixed point, so the write
    // verbs' already-canonical calls (config.cpp:3236/3268 -> :3241/:3273) still
    // address the very slot they just canonicalised.
    const UserLanguageProbeKey key = MakeUserLanguageProbeKey(engine, model);
    for (const UserLanguageProbe& probe : language.probes) {
        if (probe.engine == key.engine && probe.model == key.model) {
            return &probe;
        }
    }
    return nullptr;
}

UserLanguageProbeStatus EffectiveUserLanguageProbeStatus(const UserLanguageProbe& probe) {
    if (probe.override_status == UserLanguageProbeStatus::Supported ||
        probe.override_status == UserLanguageProbeStatus::Unsupported) {
        return probe.override_status; // D6: the user is the final authority
    }
    return probe.status;
}

UserLanguageProbeStatus DisplayUserLanguageProbeStatus(const UserLanguageProbe& probe) {
    if (probe.override_status == UserLanguageProbeStatus::Supported ||
        probe.override_status == UserLanguageProbeStatus::Unsupported) {
        return UserLanguageProbeStatus::UserOverridden;
    }
    if (probe.status == UserLanguageProbeStatus::Checking) {
        return UserLanguageProbeStatus::Checking;
    }
    return probe.status;
}

void RecordUserLanguageProbeResult(UserLanguage& language,
                                   std::string_view engine,
                                   std::string_view model,
                                   const ProbeEvaluation& evaluation,
                                   std::string_view checked_at) {
    UserLanguageProbe* probe = UpsertProbe(language, engine, model);
    if (probe == nullptr) {
        return;
    }
    probe->status = (evaluation.status == UserLanguageProbeStatus::Supported)
                        ? UserLanguageProbeStatus::Supported
                        : UserLanguageProbeStatus::Unsupported;
    // 261116_0003 P9b-3 PR10-07: the runtime record seam caps what it stores.
    // The load path already caps at parse (PR9-05); nothing capped a live call,
    // so a caller-supplied evidence string or stamp could grow the block past
    // the field budget the schema documents.
    probe->evidence =
        CapUserLanguageField(evaluation.evidence, kMaxUserLanguageProbeEvidenceBytes);
    probe->checked_at = CapUserLanguageField(TrimAsciiSpaceView(checked_at),
                                             kMaxUserLanguageProbeCheckedAtBytes);
    // 261116_0003 P9a fix PR9-06: a probe run never retires the user's answer.
    // The override is the final authority (D6); the automatic first-use pass
    // fires only when no verdict AND no override exists, so reaching this
    // point with an override set is an explicit re-test whose verdict the
    // user may still overrule. Only a stale non-override entry takes the
    // fresh verdict's implicit retirement (already Unknown -> no-op here).
    if (probe->override_status != UserLanguageProbeStatus::Supported &&
        probe->override_status != UserLanguageProbeStatus::Unsupported) {
        probe->override_status = UserLanguageProbeStatus::Unknown;
    }
}

void RecordUserLanguageProbeOverride(UserLanguage& language,
                                     std::string_view engine,
                                     std::string_view model,
                                     UserLanguageProbeStatus status,
                                     std::string_view checked_at) {
    if (status != UserLanguageProbeStatus::Supported &&
        status != UserLanguageProbeStatus::Unsupported) {
        return; // an override is an ANSWER, not a state
    }
    UserLanguageProbe* probe = UpsertProbe(language, engine, model);
    if (probe == nullptr) {
        return;
    }
    probe->override_status = status;
    // 261116_0003 P9b-3 PR10-07: same write-seam cap as the result verb.
    probe->checked_at = CapUserLanguageField(TrimAsciiSpaceView(checked_at),
                                             kMaxUserLanguageProbeCheckedAtBytes);
}

void ClearUserLanguageProbeOverride(UserLanguage& language,
                                    std::string_view engine,
                                    std::string_view model) {
    for (UserLanguageProbe& probe : language.probes) {
        if (probe.engine == engine && probe.model == model) {
            probe.override_status = UserLanguageProbeStatus::Unknown;
            return;
        }
    }
}

// 261116_0003 P9b-3: ISO-8601 UTC in the documented wire form
// ("YYYY-MM-DDTHH:MM:SSZ"). GetSystemTime is already UTC, so no timezone or
// DST conversion is involved; the buffer is fixed and the result is 20 bytes,
// inside kMaxUserLanguageProbeCheckedAtBytes by construction.
std::string UserLanguageProbeTimestampNowUtc() {
    SYSTEMTIME st = {};
    ::GetSystemTime(&st);
    char buffer[24] = {};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02uZ",
                  static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth),
                  static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));
    return std::string(buffer);
}

// ---- 261116_0003 P9c-1a: declared-language canonical form + the publish seam ----
// (contract at the declaration block above GetLanguagesForEngine in config.hpp)

std::string CanonicalOpenAiDeclaredLanguage(std::string_view token) {
    // The INPUT token is a human spelling (a code, an English name such as
    // "Chinese Traditional" or a native name), so it is bounded generously -
    // just enough that a hand-edited multi-megabyte string never reaches the
    // name matcher. The PERSISTED form is a registry code and is separately
    // held to kMaxOpenAiDeclaredLanguageCodeBytes below.
    constexpr std::size_t kMaxDeclarationTokenBytes = 64;
    const std::string_view trimmed = TrimAsciiSpaceView(token);
    if (trimmed.empty() || trimmed.size() > kMaxDeclarationTokenBytes) {
        return std::string();
    }
    const LanguageInfo* row = FindLanguageByCode(trimmed);
    if (row == nullptr) {
        row = FindLanguageByName(trimmed);
    }
    if (row == nullptr || row->code == "AUTO") {
        return std::string(); // unknown, or the source-only marker: not declarable
    }
    if (row->code.size() > kMaxOpenAiDeclaredLanguageCodeBytes) {
        return std::string(); // defensive: no registry code is that long
    }
    return row->code;
}

std::vector<std::string> SanitizeOpenAiDeclaredLanguages(
    const std::vector<std::string>& declared) {
    std::vector<std::string> wanted; // canonical, declaration order, first-wins
    wanted.reserve(declared.size());
    for (const std::string& token : declared) {
        std::string code = CanonicalOpenAiDeclaredLanguage(token);
        if (code.empty()) {
            continue; // unknown / AUTO / over-long: dropped, never an error
        }
        if (std::find(wanted.begin(), wanted.end(), code) == wanted.end()) {
            wanted.push_back(std::move(code));
        }
    }
    // REGISTRY order, not declaration order: the persisted bytes and the menus
    // are a function of the SET alone, so a hand-reordered config.json or a
    // re-saved untouched declaration produces identical output. AUTO is skipped
    // by the loop below (the view filter keeps it separately).
    std::vector<std::string> canonical;
    canonical.reserve(wanted.size());
    for (const LanguageInfo& row : kAllLanguages) {
        if (row.code == "AUTO") {
            continue;
        }
        if (std::find(wanted.begin(), wanted.end(), row.code) != wanted.end()) {
            if (canonical.size() >= kMaxOpenAiDeclaredLanguages) {
                break; // defensive cap; the registry itself is the real bound
            }
            canonical.push_back(row.code);
        }
    }
    return canonical;
}

std::vector<std::string> PublishOpenAiDeclaredLanguages(
    const std::vector<std::string>& declared) {
    std::vector<std::string> canonical = SanitizeOpenAiDeclaredLanguages(declared);
    OpenAiDeclaredLanguageState& state = OpenAiDeclaredLanguageSnapshot();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (canonical == state.codes) {
        return canonical; // same set: the cached views are already correct
    }
    state.codes = canonical;
    ++state.generation;
    return canonical;
}

bool OpenAiLanguagesDeclared() {
    OpenAiDeclaredLanguageState& state = OpenAiDeclaredLanguageSnapshot();
    std::lock_guard<std::mutex> lock(state.mutex);
    return !state.codes.empty();
}

bool OpenAiLanguageDeclared(std::string_view code) {
    OpenAiDeclaredLanguageState& state = OpenAiDeclaredLanguageSnapshot();
    std::lock_guard<std::mutex> lock(state.mutex);
    return IsOpenAiLanguageDeclaredLocked(state, code);
}

// ---- 261122_0001 B-U1: the pure profile helpers (design-pick §7) -----------
// i18n-independent by contract: the dialog unit derives the name template from
// StringId::OpenAiProfileNameTemplate at SAVE time, while the LOAD path's
// migration synthesis uses the fixed kOpenAiMigrationNameTemplate below. These
// helpers take base_url / model as DATA: control/bidi stripping is the
// dialog's sanitize-before-save duty, not done here.

namespace {

// ASCII fold for the DNS-label abbreviation (hostnames are case-insensitive).
// IP literals and punycode carry no uppercase by construction and pass
// through; a non-ASCII byte passes through untouched (UTF-8 continuation
// bytes are >= 0x80, outside the 'A'-'Z' range).
std::string AsciiLowerOpenAiLabel(std::string_view label) {
    std::string out(label);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool IsAllAsciiDigits(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

// IPv4 heuristic: only digits and dots with at least one of each. The exact
// quad validator is unnecessary — a number-and-dots hostname is displayed
// whole either way, which is the IP branch's whole purpose (the second-to-
// last label of "192.168.0.10" would be the meaningless "0").
bool LooksLikeIPv4OpenAiHost(std::string_view host) {
    bool has_digit = false;
    for (const char c : host) {
        if (c >= '0' && c <= '9') {
            has_digit = true;
        } else if (c != '.') {
            return false;
        }
    }
    return has_digit && host.find('.') != std::string_view::npos;
}

// The FIXED migration template (the en form of the OpenAiProfileNameTemplate
// i18n string). The load-path synthesis must be DETERMINISTIC — the same
// input file always yields the same library — so it cannot follow the UI
// locale the way the dialog's save-time derivation does (design-pick §7).
constexpr std::string_view kOpenAiMigrationNameTemplate = "{h} / {m}";

} // namespace

std::string OpenAiHostAbbrev(std::string_view base_url) {
    // 1. Authority extraction: scheme://[userinfo@]host[:port][/path][?query].
    std::string_view rest = base_url;
    const std::size_t scheme = rest.find("://");
    if (scheme != std::string_view::npos) {
        rest.remove_prefix(scheme + 3);
    }
    // A relative reference ("/v1/...", "?x=1", empty) carries no host at all.
    if (rest.empty() || rest.front() == '/' || rest.front() == '?') {
        return {};
    }
    const std::size_t delim = rest.find_first_of("/?");
    std::string_view authority = rest.substr(0, delim);
    const std::size_t at = authority.rfind('@');
    if (at != std::string_view::npos) {
        authority.remove_prefix(at + 1);
    }
    if (authority.empty()) {
        return {};
    }
    // 2. Host extraction: an [v6] literal keeps its brackets (the design-pick
    // table pins "[::1]"); a single colon followed by all digits is a port;
    // multiple colons are an unbracketed v6 tail that cannot be split
    // reliably, so the whole thing is kept.
    std::string host;
    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) {
            return {}; // malformed literal: no host
        }
        host = std::string(authority.substr(0, close + 1));
    } else {
        std::string_view candidate = authority;
        const std::size_t colon = candidate.rfind(':');
        if (colon != std::string_view::npos && candidate.find(':') == colon &&
            IsAllAsciiDigits(candidate.substr(colon + 1))) {
            candidate = candidate.substr(0, colon);
        }
        host = std::string(candidate);
    }
    if (host.empty()) {
        return {};
    }
    if (host.size() > 1 && host.back() == '.') {
        host.pop_back(); // one trailing root dot is trivia, not a label
    }
    // 3. Abbreviation: a multi-label DNS hostname yields its SECOND-TO-LAST
    // label (the design-pick host token: api.openai.com -> "openai"); a
    // single label or an IP literal yields the whole host.
    std::string abbrev;
    const std::size_t last_dot = host.rfind('.');
    if (last_dot != std::string::npos && last_dot > 0 &&
        !LooksLikeIPv4OpenAiHost(host)) {
        const std::size_t prev_dot = host.rfind('.', last_dot - 1);
        const std::size_t label_start = (prev_dot == std::string::npos) ? 0 : prev_dot + 1;
        abbrev = AsciiLowerOpenAiLabel(host.substr(label_start, last_dot - label_start));
    } else {
        abbrev = AsciiLowerOpenAiLabel(host);
    }
    // 4. Cap: 24 UTF-8 bytes on a code-point boundary; the "…" suffix counts
    // toward the cap, so a truncated form is at most 24 bytes total.
    constexpr std::size_t kMaxOpenAiHostAbbrevBytes = 24;
    if (abbrev.size() <= kMaxOpenAiHostAbbrevBytes) {
        return abbrev;
    }
    constexpr std::string_view kEllipsis = "\xE2\x80\xA6"; // U+2026
    return CapUserLanguageField(abbrev, kMaxOpenAiHostAbbrevBytes - kEllipsis.size()) +
           std::string(kEllipsis);
}

std::string MakeOpenAiProfileName(std::string_view host_abbrev,
                                  std::string_view model,
                                  std::string_view name_template) {
    // The empty-side rules collapse the template entirely (design-pick §7):
    // no connector fragment (" / ") may survive a missing side.
    if (host_abbrev.empty() && model.empty()) {
        return {};
    }
    std::string out;
    if (host_abbrev.empty()) {
        out.assign(model);
    } else if (model.empty()) {
        out.assign(host_abbrev);
    } else {
        out.reserve(name_template.size() + host_abbrev.size() + model.size());
        for (std::size_t i = 0; i < name_template.size();) {
            if (name_template.compare(i, 3, "{h}") == 0) {
                out.append(host_abbrev);
                i += 3;
            } else if (name_template.compare(i, 3, "{m}") == 0) {
                out.append(model);
                i += 3;
            } else {
                out.push_back(name_template[i]);
                ++i;
            }
        }
    }
    return CapUserLanguageField(out, kMaxOpenAiProfileNameBytes);
}

std::string MakeUniqueOpenAiProfileName(const std::vector<OpenAiProfile>& existing,
                                        std::string_view base_name) {
    const auto name_taken = [&existing](std::string_view name) {
        for (const OpenAiProfile& profile : existing) {
            if (profile.name == name) {
                return true;
            }
        }
        return false;
    };
    // The capped base itself is the first candidate (a base within the cap
    // returns unchanged when free).
    std::string candidate = CapUserLanguageField(base_name, kMaxOpenAiProfileNameBytes);
    if (!name_taken(candidate)) {
        return candidate;
    }
    // " (2)", " (3)" … : the suffix is always kept whole — the base is
    // truncated to cap-minus-suffix on a code-point boundary first, so the
    // result can never exceed kMaxOpenAiProfileNameBytes.
    for (int n = 2;; ++n) {
        const std::string suffix = " (" + std::to_string(n) + ")";
        candidate = CapUserLanguageField(base_name, kMaxOpenAiProfileNameBytes - suffix.size());
        candidate.append(suffix);
        if (!name_taken(candidate)) {
            return candidate;
        }
    }
}

std::size_t PlanOpenAiProfileUpsert(const std::vector<OpenAiProfile>& existing,
                                    const OpenAiConfig& candidate_identity) {
    // Storage identity (design-pick item 3): base_url ASCII case-INSENSITIVE
    // (hosts are case-insensitive; the scheme/host case of a hand-typed URL
    // must not fork one logical endpoint into two profiles), model case-
    // SENSITIVE (model ids are opaque tokens — "GPT" and "gpt" are different
    // models to a server).
    for (std::size_t i = 0; i < existing.size(); ++i) {
        if (EqualsIgnoreCaseAscii<char>(existing[i].cfg.base_url,
                                        candidate_identity.base_url) &&
            existing[i].cfg.model == candidate_identity.model) {
            return i;
        }
    }
    return existing.size(); // no match: the caller appends
}

LanguageMenuView BuildLanguageMenuView(LanguageEngine engine,
                                      bool include_auto,
                                      const std::vector<UserLanguage>& user_languages) {
    LanguageMenuView view;
    // D5 "내 언어 group atop CLOUD menus": under engine_type=auto the menus
    // render the Google list (P8b tray idiom: TrayLanguageEngineFor maps the
    // auto row to Google), so the MENU view resolves auto to Google.
    // Deliberately different from GetLanguagesForEngine(Auto) == HyMt2, which
    // is the ROUTING view: menus and routing are two views with two purposes.
    const LanguageEngine menu_engine =
        (engine == LanguageEngine::Auto) ? LanguageEngine::Google : engine;
    const std::vector<const LanguageInfo*>& base = GetLanguagesForEngine(menu_engine, include_auto);
    view.registry.assign(base.begin(), base.end());
    if (menu_engine == LanguageEngine::HyMt2) {
        return view; // D7: official set only - no custom row, no probe
    }
    std::uint32_t required_bit = kLangUserGguf;
    if (menu_engine == LanguageEngine::Google) {
        required_bit = kLangGoogle;
    } else if (menu_engine == LanguageEngine::OpenAi) {
        required_bit = kLangOpenAi;
    }
    for (const UserLanguage& language : user_languages) {
        if (view.user_group.size() >= kMaxUserLanguages) {
            break;
        }
        const std::string stored_name = UserLanguageStoredName(language);
        const std::string_view stored_trimmed = TrimAsciiSpaceView(stored_name);
        if (stored_trimmed.empty()) {
            continue; // unnamed entry: nothing to render
        }
        // 261116_0003 P9a fix PR9-01: the user group dedupes by resolved CODE
        // (the asserted contract: "first occurrence of a code wins"); a
        // code-less (free-form) entry falls back to name-level dedupe. The old
        // stored-name comparison let two same-code entries with different
        // names both render.
        const bool has_code = !language.code.empty();
        bool duplicate = false;
        for (const LanguageMenuRow& existing : view.user_group) {
            if (existing.user == nullptr) {
                continue;
            }
            if (has_code) {
                if (!existing.user->code.empty() &&
                    EqualsIgnoreCaseAscii<char>(existing.user->code, language.code)) {
                    duplicate = true;
                    break;
                }
            } else if (existing.user->code.empty() &&
                       EqualsIgnoreCaseAscii<char>(
                           TrimAsciiSpaceView(UserLanguageStoredName(*existing.user)),
                           stored_trimmed)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue; // first occurrence of a language wins (config order)
        }
        const LanguageInfo* row =
            language.code.empty() ? nullptr : FindLanguageByCode(language.code);
        if (row != nullptr && row->code == "AUTO") {
            row = nullptr; // auto-detect is never a target language
        }
        if (menu_engine == LanguageEngine::Google &&
            (row == nullptr || (row->engine_mask & required_bit) == 0)) {
            // D5/D6: Google serves only publisher-declared languages. A
            // free-form entry (no code) or a row without the Google bit cannot
            // be offered there - it stays in the free-form engines' menus.
            continue;
        }
        LanguageMenuRow menu_row;
        menu_row.registry = row;
        menu_row.user = &language;
        menu_row.group = LanguageMenuGroup::UserLanguage;
        if (row != nullptr) {
            // A language appears once per menu: the claimed registry row leaves
            // the registry section and moves into "내 언어".
            for (auto it = view.registry.begin(); it != view.registry.end(); ++it) {
                if (*it == row) {
                    view.registry.erase(it);
                    break;
                }
            }
        }
        view.user_group.push_back(menu_row);
    }
    return view;
}

// REQ-208/SEC-1 (session 260911_0002 T3, design 144800 §2.6 option (i)): the
// first-run privacy gate is exactly the negation of the persisted ack record.
// main.cpp fires the blocking MessageBoxW when this returns true and only then
// constructs the TranslationManager / starts worker+hook threads, so translation
// serving is gated on the notice by construction (light-gate 165600 Q4:
// "disclosure BEFORE any translation"). The predicate lives here, decoupled
// from the MessageBox, so TestReq208PrivacyNoticeGate pins it headlessly.
bool PrivacyNoticeOutstanding(bool privacy_notice_shown) {
    return !privacy_notice_shown;
}

namespace {

// True when `joined` (already lexically normalised) stays inside `base`
// (already normalised). Same containment rule IsValidModelPath applies at
// validation time, so absolutizing a relative path here cannot silently widen
// the path-traversal guard that commit 46ff978 (M3 security) introduced.
//
// A6/W3 (session 260910_0007): the former file-local LowerPathAscii mirror of
// engine.cpp's LowerAscii is removed; the containment expression (itself a
// byte-identical twin of IsValidModelPath's) now lives once in
// IsPathContainedIgnoreCaseAscii (unicode_utils.hpp). Fold-equivalence:
// LowerPathAscii ran std::tolower under this project's default "C" CRT locale
// (no setlocale call exists anywhere in src/), where tolower maps only
// 'A'-'Z' with +32 and passes high bytes (>= 0x80, e.g. UTF-8 lead/continuation
// bytes) through unchanged — exactly the shared template's fold set, so the
// comparison result is unchanged for every input (REF-3.1 precedent).
bool PathInsideBase(const std::filesystem::path& joined,
                    const std::filesystem::path& base) {
    const std::string j = joined.generic_string();
    const std::string b = base.generic_string();
    return IsPathContainedIgnoreCaseAscii<char>(j, b);
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

namespace {

// R1-13 (code review mimoflash F-04): preserve a config.json the parser
// REJECTED, before anything can overwrite it. FromJsonString leaves every field
// at its default when it fails, and wWinMain's unconditional exit save then
// atomically replaces the user's document with those defaults, so a hand-edited
// setting would be unrecoverable. Copy the ORIGINAL bytes once, as a sibling:
//   config.json -> config.json.bak
// Rules: an existing .bak is never overwritten (an older rescue copy is never
// clobbered by a newer malformation), and a healthy load never reaches this
// function, so a normal config.json never grows a sibling. The diag line is
// shape-only: byte count + copy outcome, never the document content.
void PreserveMalformedConfigBackup(const std::filesystem::path& target_path,
                                   std::size_t original_bytes) {
    std::filesystem::path backup_path = target_path;
    backup_path += ".bak";
    std::error_code ec;
    if (std::filesystem::exists(backup_path, ec)) {
        DIAG_F("CONFIG/001: config.json failed to parse (len=%zu); an existing .bak was kept "
               "unchanged\n", original_bytes);
        return;
    }
    const bool copied = std::filesystem::copy_file(
        target_path, backup_path, std::filesystem::copy_options::skip_existing, ec);
    DIAG_F("CONFIG/002: config.json failed to parse (len=%zu); original copied to .bak "
           "(copied=%d ec=%d)\n", original_bytes, copied ? 1 : 0, ec.value());
}

} // namespace

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
    std::string document = buffer.str();
    file.close();
    if (FromJsonString(document)) {
        return true;
    }
    // R1-13: the document did not parse, so every field is still at its
    // default and the next save (wWinMain's unconditional exit save, or any
    // settings change made in this session) would atomically replace the
    // user's file with those defaults. Preserve the exact bytes FIRST; a
    // healthy load never reaches this line, so a normal config.json round-trip
    // never produces a .bak.
    PreserveMalformedConfigBackup(target_path, document.size());
    return false;
}

bool AppConfig::SaveToFile(const std::filesystem::path& path) const {
    std::filesystem::path target_path = path.empty() ? GetDefaultConfigPath() : path;
    std::lock_guard<std::mutex> lock(mutex_); // I4: serialize saves + field reads
    return SaveToFileLocked(target_path);
}

bool AppConfig::SaveToFileLocked(const std::filesystem::path& target_path) const {
    // Caller MUST hold mutex_ (SaveToFile). The lock serializes INTRA-PROCESS
    // writers; the unique pid+counter temp name (below) covers cross-process
    // writers and stale leftovers from crashed runs.
    try {
        if (target_path.has_parent_path()) {
            std::filesystem::create_directories(target_path.parent_path());
        }

        std::filesystem::path tmp_path = target_path;
        // F-07 (v0.10.5 review round 1): a UNIQUE temp name (pid + a
        // process-local counter). The caller-held mutex serializes
        // INTRA-PROCESS saves only; the old fixed ".tmp" sibling could
        // collide with a concurrent writer in another process (or a
        // leftover from a crashed run) and corrupt the config.
        static std::atomic<unsigned long long> save_tmp_seq{0};
        tmp_path += L".tmp." + std::to_wstring(::GetCurrentProcessId()) + L"." +
                     std::to_wstring(save_tmp_seq.fetch_add(1, std::memory_order_relaxed));

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
        // Backend P2-6 (v0.10.5 handoff section 4.4 item 10): short backoff
        // retry. A transient sharing/lock violation (OneDrive sync, AV scan,
        // an editor holding the file) used to lose the whole save on the
        // first attempt; retry transient errors up to 3 times with a
        // growing backoff before the rename fallback.
        for (int move_attempt = 0; move_attempt < 3; ++move_attempt) {
            if (::MoveFileExW(tmp_path.c_str(), target_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                return true;
            }
            const DWORD move_err = ::GetLastError();
            if (move_err != ERROR_SHARING_VIOLATION && move_err != ERROR_LOCK_VIOLATION &&
                move_err != ERROR_ACCESS_DENIED) {
                break; // non-transient: straight to the rename fallback
            }
            ::Sleep(static_cast<DWORD>(100 * (move_attempt + 1))); // 100, 200 ms
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
    s.ui_theme = ui_theme;
    s.auto_send = auto_send.load(std::memory_order_relaxed);
    s.sound_enabled = sound_enabled.load(std::memory_order_relaxed);
    s.back_translation_enabled = back_translation_enabled.load(std::memory_order_relaxed);
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

std::string AppConfig::NormalizeUiTheme(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    if (EqualsIgnoreCaseAscii<char>(value, "editorial") || EqualsIgnoreCaseAscii<char>(value, "b")) {
        return "editorial";
    }
    if (EqualsIgnoreCaseAscii<char>(value, "monolith") || EqualsIgnoreCaseAscii<char>(value, "c")) {
        return "monolith";
    }
    return "fluent";
}

void AppConfig::SetUiTheme(std::string value) {
    std::lock_guard<std::mutex> lock(mutex_);
    ui_theme = NormalizeUiTheme(value);

}

// REQ-045 P4-5 (item 3a-2): locked write of the third-party .gguf registry id
// (same I4 discipline as SetUiLanguage; the field is serialized by
// ToJsonStringLocked under the same mutex).
void AppConfig::SetUserModelId(std::string value) {
    std::lock_guard<std::mutex> lock(mutex_);
    user_model_id = std::move(value);
}

// 261116_0003 P9c-1a: locked write of the OpenAI declared-language set (the
// member contract next to `openai`). Sanitization + publication happen INSIDE
// the lock, so the stored member and the engine-view snapshot can never hold
// different sets, and a concurrent SaveToFile() serialising the config cannot
// observe a half-applied edit. Lock order is mutex_ -> the snapshot mutex, the
// same order the load path uses; no path takes them the other way round.
std::vector<std::string> AppConfig::SetOpenAiDeclaredLanguages(
    std::vector<std::string> declared) {
    std::lock_guard<std::mutex> lock(mutex_);
    openai_declared_languages = PublishOpenAiDeclaredLanguages(declared);
    return openai_declared_languages;
}

// ---- 261122_0001 fix-A1 R1-02 BEGIN openai_profiles write surface ---------
// The locked write surface for the profile library (member contract next to
// the member). The lock is the SAME mutex_ ToJsonStringLocked()/SaveToFile()
// hold while they iterate the vector, so a GUI-thread fold edit can no longer
// interleave with an off-thread save (the R1-02 race: GUI-thread move-assign
// vs SaveToFile() iteration). openai_profiles_set is marked on EVERY call,
// including an empty fold: "the user deleted every profile" must survive the
// next save (the writer emits an explicit []) and the reload after it (R1-12).
// The B-U5 write-surface pin in tests/openai_profile_tests.inc scans this
// region; the FromJsonString load path (already under mutex_) is the one
// documented writer outside it.
void AppConfig::SetOpenAiProfiles(std::vector<OpenAiProfile> profiles) {
    std::lock_guard<std::mutex> lock(mutex_);
    openai_profiles = std::move(profiles);
    openai_profiles_set = true;
}
// ---- 261122_0001 fix-A1 R1-02 END openai_profiles write surface -----------

// ---- 261122_0001 fix-E R2-N2 BEGIN openai block write surface -------------
// The locked write surface for the active "openai" block (member contract
// next to the member). The lock is the SAME mutex_ ToJsonStringLocked()/
// SaveToFile() hold while they serialise the block, so the GUI-thread
// settings-dialog write can no longer interleave with an off-thread save
// (the round-2 N2 race: a direct member assignment vs SaveToFile()
// iteration — the same PR9-12 class fix-A1 closed for openai_profiles, on
// this older REQ-045/047/051-era member). The fix-E write-surface pin in
// tests/openai_profile_tests.inc scans this region; the FromJsonString load
// path (ParseOpenAiBlock -> ApplyOpenAiConfigPair, already under mutex_)
// is the one documented writer outside it.
void AppConfig::SetOpenAiConfig(OpenAiConfig block) {
    std::lock_guard<std::mutex> lock(mutex_);
    openai = std::move(block);
}
// ---- 261122_0001 fix-E R2-N2 END openai block write surface ---------------

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

// ---- 261116_0003 P9b-3 PR9-12 BEGIN user_languages write surface --------
// The ONLY place this file mutates the user-language registry outside the load
// path (FromJsonString, which already holds mutex_). Every verb below takes the
// same mutex_ the whole-config serialiser takes, so a GUI edit can no longer
// interleave with a hook-thread / fx-worker SaveToFile() iteration. The P9b-3
// suite scans this region (markers included) to prove no other writer exists.
// Order of checks is deliberate: carrier (the edit would be discarded on save),
// then the entry exists, then the PR9-11 name verdict, then capacity.
bool AppConfig::HasUserLanguagesForwardCompatCarrierLocked() const {
    return !user_languages_forward_compat_json_.empty();
}

bool AppConfig::HasUserLanguagesForwardCompatCarrier() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return HasUserLanguagesForwardCompatCarrierLocked();
}

UserLanguageWriteResult AppConfig::AddUserLanguage(UserLanguage language) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (HasUserLanguagesForwardCompatCarrierLocked()) {
        return UserLanguageWriteResult::RefusedForwardCompatCarrier;
    }
    CleanUserLanguageNamesAtWrite(language);
    if (language.native_name.empty()) {
        return UserLanguageWriteResult::RejectedEmptyName; // the one required field
    }
    switch (ValidateUserLanguageName(language.native_name, language.english_name,
                                     user_languages)) {
        case UserLanguageNameVerdict::Reserved:
            return UserLanguageWriteResult::RejectedReservedName;
        case UserLanguageNameVerdict::Duplicate:
            return UserLanguageWriteResult::RejectedDuplicateName;
        case UserLanguageNameVerdict::Accepted:
            break;
    }
    if (user_languages.size() >= kMaxUserLanguages) {
        return UserLanguageWriteResult::RejectedCapacity;
    }
    user_languages.push_back(std::move(language));
    return UserLanguageWriteResult::Applied;
}

UserLanguageWriteResult AppConfig::UpdateUserLanguage(std::size_t index,
                                                      UserLanguage language) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (HasUserLanguagesForwardCompatCarrierLocked()) {
        return UserLanguageWriteResult::RefusedForwardCompatCarrier;
    }
    if (index >= user_languages.size()) {
        return UserLanguageWriteResult::RejectedNotFound;
    }
    CleanUserLanguageNamesAtWrite(language);
    if (language.native_name.empty()) {
        return UserLanguageWriteResult::RejectedEmptyName;
    }
    switch (ValidateUserLanguageName(language.native_name, language.english_name,
                                     user_languages, index)) {
        case UserLanguageNameVerdict::Reserved:
            return UserLanguageWriteResult::RejectedReservedName;
        case UserLanguageNameVerdict::Duplicate:
            return UserLanguageWriteResult::RejectedDuplicateName;
        case UserLanguageNameVerdict::Accepted:
            break;
    }
    user_languages[index] = std::move(language);
    return UserLanguageWriteResult::Applied;
}

UserLanguageWriteResult AppConfig::RemoveUserLanguage(std::size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (HasUserLanguagesForwardCompatCarrierLocked()) {
        return UserLanguageWriteResult::RefusedForwardCompatCarrier;
    }
    if (index >= user_languages.size()) {
        return UserLanguageWriteResult::RejectedNotFound;
    }
    user_languages.erase(user_languages.begin() + static_cast<std::ptrdiff_t>(index));
    return UserLanguageWriteResult::Applied;
}

// The stored-name address form (UserLanguageStoredName = the menu dispatch
// payload). FindUserLanguage is the app's single lookup; the offset back to a
// mutable entry keeps the const-correct public signature untouched.
namespace {

UserLanguage* FindMutableUserLanguage(std::vector<UserLanguage>& languages,
                                      std::string_view stored_name) {
    const UserLanguage* found = FindUserLanguage(languages, stored_name);
    if (found == nullptr) {
        return nullptr;
    }
    return &languages[static_cast<std::size_t>(found - languages.data())];
}

} // namespace

UserLanguageWriteResult AppConfig::RecordUserLanguageProbeResultFor(
    std::string_view stored_name,
    std::string_view engine,
    std::string_view model,
    const ProbeEvaluation& evaluation,
    std::string_view checked_at) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (HasUserLanguagesForwardCompatCarrierLocked()) {
        return UserLanguageWriteResult::RefusedForwardCompatCarrier;
    }
    UserLanguage* target = FindMutableUserLanguage(user_languages, stored_name);
    if (target == nullptr) {
        return UserLanguageWriteResult::RejectedNotFound;
    }
    const UserLanguageProbeKey key = MakeUserLanguageProbeKey(engine, model);
    if (key.engine.empty()) {
        return UserLanguageWriteResult::RejectedMissingProbeKey;
    }
    if (target->probes.size() >= kMaxUserLanguageProbes &&
        FindUserLanguageProbe(*target, key.engine, key.model) == nullptr) {
        // UpsertProbe would silently drop the new key at the cap; report it so
        // a re-test on a full cache is visible instead of a no-op.
        return UserLanguageWriteResult::RejectedCapacity;
    }
    RecordUserLanguageProbeResult(*target, key.engine, key.model, evaluation, checked_at);
    return UserLanguageWriteResult::Applied;
}

UserLanguageWriteResult AppConfig::RecordUserLanguageProbeOverrideFor(
    std::string_view stored_name,
    std::string_view engine,
    std::string_view model,
    UserLanguageProbeStatus status,
    std::string_view checked_at) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (HasUserLanguagesForwardCompatCarrierLocked()) {
        return UserLanguageWriteResult::RefusedForwardCompatCarrier;
    }
    if (status != UserLanguageProbeStatus::Supported &&
        status != UserLanguageProbeStatus::Unsupported) {
        return UserLanguageWriteResult::RejectedInvalidStatus; // an answer, not a state
    }
    UserLanguage* target = FindMutableUserLanguage(user_languages, stored_name);
    if (target == nullptr) {
        return UserLanguageWriteResult::RejectedNotFound;
    }
    const UserLanguageProbeKey key = MakeUserLanguageProbeKey(engine, model);
    if (key.engine.empty()) {
        return UserLanguageWriteResult::RejectedMissingProbeKey;
    }
    if (target->probes.size() >= kMaxUserLanguageProbes &&
        FindUserLanguageProbe(*target, key.engine, key.model) == nullptr) {
        return UserLanguageWriteResult::RejectedCapacity;
    }
    RecordUserLanguageProbeOverride(*target, key.engine, key.model, status, checked_at);
    return UserLanguageWriteResult::Applied;
}

UserLanguageWriteResult AppConfig::ClearUserLanguageProbeOverrideFor(
    std::string_view stored_name,
    std::string_view engine,
    std::string_view model) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (HasUserLanguagesForwardCompatCarrierLocked()) {
        return UserLanguageWriteResult::RefusedForwardCompatCarrier;
    }
    UserLanguage* target = FindMutableUserLanguage(user_languages, stored_name);
    if (target == nullptr) {
        return UserLanguageWriteResult::RejectedNotFound;
    }
    // Idempotent by design: clearing an override that is not set is the
    // requested end state, not an error.
    const UserLanguageProbeKey key = MakeUserLanguageProbeKey(engine, model);
    ClearUserLanguageProbeOverride(*target, key.engine, key.model);
    return UserLanguageWriteResult::Applied;
}

// ---- 261116_0003 P9b-3 PR9-12 END user_languages write surface ---------

std::string AppConfig::ToJsonString() const {
    std::lock_guard<std::mutex> lock(mutex_); // I4
    return ToJsonStringLocked();
}

std::string AppConfig::ToJsonStringLocked() const {
    // Caller MUST hold mutex_ (ToJsonString / SaveToFileLocked).
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"ui_language\": \"" << EscapeJsonString(ui_language) << "\",\n";
    ss << "  \"ui_theme\": \"" << EscapeJsonString(NormalizeUiTheme(ui_theme)) << "\",\n";
    ss << "  \"engine_type\": \"" << EscapeJsonString(engine_type) << "\",\n";
    ss << "  \"model_path\": \"" << EscapeJsonString(model_path) << "\",\n";
    // REQ-045 P4-5 (item 3a-2): ALWAYS serialized so a round trip preserves
    // the user's third-party model pick (same sticky-key policy as model_path).
    ss << "  \"user_model_id\": \"" << EscapeJsonString(user_model_id) << "\",\n";
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
    // 261116_0003 P9a (plan §3 D5/D6): the user-language block and its schema
    // version are ALWAYS serialized (an empty registry as "[]"), same sticky-key
    // policy as the other blocks - the version stays visible and a round trip
    // cannot lose it. A block written by a NEWER schema version is re-emitted
    // verbatim together with ITS declared version (kUserLanguagesSchemaVersion),
    // so the next load keeps preserving it instead of reinterpreting it.
    const bool preserving_forward_compat = !user_languages_forward_compat_json_.empty();
    // 261116_0003 P9a fix PR9-09: a parsed newer version round-trips as its
    // integer; an unparseable one ("abc", "2junk") round-trips as its ORIGINAL
    // text, never as a number this build never saw.
    // 261116_0003 P9a fix2 PR10-01: that preserved text is emitted as a QUOTED,
    // escaped JSON string. The text is kept RAW in memory (never interpreted
    // as a version), so re-emitting it unquoted let a crafted
    // `"user_languages_version": "2\", \"cloud_fallback_enabled\": true"`
    // fabricate sibling config keys on the next save (the sibling-key class
    // PR9-04 closed for the block value). The read side DOES decode the
    // emission: the reader's ParseString unescapes the quoted form back to
    // the same text, so the carrier still round-trips.
    // 261116_0003 P9a fix2 PR10-02: an empty or whitespace-only version text
    // takes the same quoted path (it emits `""`), so the carrier is never armed
    // with the 0 numeric sentinel whose next load reads as "current version"
    // and half-parses the preserved newer block as v1.
    const std::string emitted_user_languages_version =
        preserving_forward_compat
            ? (user_languages_forward_compat_version_text_
                   ? "\"" + EscapeJsonString(user_languages_forward_compat_version_raw_) + "\""
                   : std::to_string(user_languages_forward_compat_version_))
            : std::to_string(kUserLanguagesSchemaVersion);
    ss << "  \"user_languages_version\": " << emitted_user_languages_version << ",\n";
    ss << "  \"user_languages\": "
       << SerializeUserLanguages(user_languages, user_languages_forward_compat_json_) << ",\n";
    ss << "  \"auto_send\": " << (auto_send.load(std::memory_order_relaxed) ? "true" : "false") << ",\n";
    ss << "  \"sound_enabled\": " << (sound_enabled.load(std::memory_order_relaxed) ? "true" : "false") << ",\n";
    // 261108_0002 (WS3): ALWAYS serialized (same sticky-key policy as the
    // other toggles) so a round trip preserves the user's opt-in; the key is
    // absent on every pre-261108 config.json and then reads as false.
    ss << "  \"back_translation_enabled\": " << (back_translation_enabled.load(std::memory_order_relaxed) ? "true" : "false") << ",\n";
    ss << "  \"drag_to_translate\": " << (drag_to_translate ? "true" : "false") << ",\n";
    ss << "  \"cloud_fallback_enabled\": " << (cloud_fallback_enabled ? "true" : "false") << ",\n";
    // REQ-043 (plan §5.1-2): the engine_host block is ALWAYS serialized so a
    // round trip preserves the values (same sticky-key policy as the Phase 3
    // language keys).
    ss << "  \"engine_host\": {\n";
    ss << "    \"enabled\": " << (engine_host.enabled ? "true" : "false") << ",\n";
    ss << "    \"spawn\": " << (engine_host.spawn ? "true" : "false") << ",\n";
    ss << "    \"idle_exit_ms\": " << engine_host.idle_exit_ms << ",\n";
    // REQ-060: the two new keys are ALWAYS serialized too (same sticky-key
    // policy), so an untouched round trip writes back the defaults.
    ss << "    \"worker_idle_ms\": " << engine_host.worker_idle_ms << ",\n";
    ss << "    \"warmup\": " << (engine_host.warmup ? "true" : "false") << "\n";
    ss << "  },\n";
    // REQ-045 P4-3 (design §3b): the openai block is ALWAYS serialized so a
    // round trip preserves the values (same sticky-key policy as engine_host).
    // The key is written DPAPI-protected, never in cleartext.
    ss << "  \"openai\": {\n";
    ss << "    \"base_url\": \"" << EscapeJsonString(openai.base_url) << "\",\n";
    ss << "    \"model\": \"" << EscapeJsonString(openai.model) << "\",\n";
    // 261121_0001 U-H (contract C2): ALWAYS serialized like every other key of
    // this block (the sticky-key policy above), so a round trip cannot lose
    // the user's pick and a config.json written by this build always shows the
    // setting. The encoded value "" means UNSET: the request body then omits
    // the reasoning_effort key entirely (a non-reasoning model rejects it with
    // HTTP 400), which is why "" is the default and can never be confused with
    // the standard "none" effort value.
    ss << "    \"reasoning_effort\": \"" << EscapeJsonString(openai.reasoning_effort) << "\",\n";
    ss << "    \"api_key_dpapi\": \"" << EscapeJsonString(openai.api_key_dpapi) << "\",\n";
    ss << "    \"api_key_sha256\": \"" << EscapeJsonString(openai.api_key_sha256) << "\",\n";
    ss << "    \"http_consent_given\": " << (openai.http_consent_given ? "true" : "false") << "\n";
    ss << "  },\n";
    // 261122_0001 B-U1 + fix-A1 (R1-12): the library is emitted while
    // non-empty; an EMPTY library emits an explicit [] once the fold has been
    // touched (openai_profiles_set — a delete-all is a real edit the next
    // load must not migrate away). The key stays omitted only while never
    // set, keeping every pre-261122_0001 config.json byte-identical (the
    // openai_declared_languages precedent).
    if (!openai_profiles.empty()) {
        ss << "  \"openai_profiles\": " << SerializeOpenAiProfiles(openai_profiles) << ",\n";
    } else if (openai_profiles_set) {
        ss << "  \"openai_profiles\": [],\n";
    }
    // 261116_0003 P9c-1a: the OpenAI declared-language set, emitted ONLY while
    // non-empty. Unlike the openai block above (sticky keys, so an untouched
    // round trip rewrites the defaults), an EMPTY set carries no information:
    // "key absent" already means "not declared", so omitting it keeps every
    // pre-P9c config.json byte-identical and adds no noise to new ones. The
    // member holds the canonical form (registry codes, registry order), so the
    // emitted order is stable for any input order.
    if (!openai_declared_languages.empty()) {
        ss << "  \"openai_declared_languages\": [";
        for (std::size_t i = 0; i < openai_declared_languages.size(); ++i) {
            if (i != 0) {
                ss << ", ";
            }
            ss << "\"" << EscapeJsonString(openai_declared_languages[i]) << "\"";
        }
        ss << "],\n";
    }
    ss << "  \"diag_log_enabled\": " << (diag_log_enabled ? "true" : "false") << ",\n";
    ss << "  \"diag_log_content\": " << (diag_log_content ? "true" : "false") << ",\n";
    ss << "  \"privacy_notice_shown\": " << (privacy_notice_shown ? "true" : "false") << ",\n";
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
    // 261116_0003 P9a: the user-language block is DEFERRED to after the loop.
    // The version key may appear after the array in a hand-edited file, and the
    // version decides whether the array is parsed or preserved verbatim.
    // 261116_0003 P9a fix PR9-04/PR9-09: the block value is the RAW source
    // text (the reader captured it undecoded), and the version text is kept
    // for the strict digit-only validation below.
    std::string raw_user_languages;
    bool has_user_languages_key = false;
    std::string raw_user_languages_version;
    bool has_user_languages_version_key = false;
    // 261116_0003 P9c-1a: the declared-language array is captured RAW too (an
    // array value always is) and parsed after the loop.
    std::string raw_openai_declared_languages;
    bool has_openai_declared_languages_key = false;
    // 261122_0001 B-U1: the profile library array is captured RAW (the same
    // posture) and parsed after the loop, next to the migration check.
    std::string raw_openai_profiles;
    bool has_openai_profiles_key = false;
    for (const auto& [k, v] : pairs) {
        if (k == "ui_language") {
            ui_language = v;
        } else if (k == "ui_theme") {
            ui_theme = NormalizeUiTheme(v);
        } else if (k == "engine_type") {
            engine_type = v;
        } else if (k == "model_path") {
            model_path = v;
        } else if (k == "user_model_id") {
            // REQ-045 P4-5 (item 3a-2): key absent on every pre-REQ-045
            // config.json => the field keeps its compile-time default ""
            // (the pinned Hy-MT2 model keeps serving, AC-4).
            user_model_id = v;
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
        } else if (k == "back_translation_enabled") {
            // 261108_0002 (WS3): key absent on every pre-261108 config.json =>
            // the field keeps its compile-time default false (opt-in card).
            back_translation_enabled.store(v == "true", std::memory_order_relaxed);
        } else if (k == "drag_to_translate") {
            drag_to_translate = (v == "true");
        } else if (k == "cloud_fallback_enabled") {
            cloud_fallback_enabled = (v == "true");
        } else if (k == "engine_host") {
            // REQ-043: nested block captured verbatim by the reader; absent
            // on every pre-REQ-043 config.json => the defaults survive.
            ParseEngineHostBlock(v, engine_host);
        } else if (k == "openai") {
            // REQ-045 P4-3: nested block captured verbatim by the reader;
            // absent on every pre-REQ-045 config.json => the defaults survive.
            ParseOpenAiBlock(v, openai);
        } else if (k == "openai_declared_languages") {
            // 261116_0003 P9c-1a: an ARRAY value is captured RAW by the reader
            // (the same path engine_host takes for its object), so the text is
            // parsed after the loop, next to the publish that mirrors it into
            // the engine-view filter.
            raw_openai_declared_languages = v;
            has_openai_declared_languages_key = true;
        } else if (k == "openai_profiles") {
            // 261122_0001 B-U1: an ARRAY value is captured RAW by the reader,
            // parsed after the loop next to the migration check (the key's
            // PRESENCE suppresses the synthesis, so both need their flags).
            raw_openai_profiles = v;
            has_openai_profiles_key = true;
        } else if (k == "user_languages_version") {
            // 261116_0003 P9a fix PR9-09: the text is validated STRICTLY
            // (digit-only) in the deferred handling below; a non-numeric or
            // newer version is never guessed at as the current version.
            raw_user_languages_version = v;
            has_user_languages_version_key = true;
        } else if (k == "user_languages") {
            // 261116_0003 P9a: deferred (see the declaration above).
            raw_user_languages = v;
            has_user_languages_key = true;
        } else if (k == "diag_log_enabled") {
            // REQ-201: key absent on every pre-260911 config.json => the field
            // keeps its compile-time default false (opt-in log FILE sink — the
            // release posture ships with logging OFF).
            diag_log_enabled = (v == "true");
        } else if (k == "diag_log_content") {
            // REQ-003: key absent on every pre-260909 config.json => the field
            // keeps its compile-time default false (opt-in content logging).
            diag_log_content = (v == "true");
        } else if (k == "privacy_notice_shown") {
            // REQ-208/SEC-1: key absent on every pre-260911 config.json => the
            // field keeps its compile-time default false, so a fresh install
            // fires the first-run privacy popup (the blocking serving gate —
            // design 144800 §2.3/§2.6 option (i)).
            privacy_notice_shown = (v == "true");
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
            // SEC Batch B (Medium 3 hardening, accepted per 235914 §5): clamp at
            // the single JSON choke point. std::stof accepts quoted "nan"/"inf"
            // strings, so std::isfinite + range tests run here; a non-finite or
            // out-of-range value falls back to the field default (config.hpp
            // sampler note: 0.0f/0.6f/20/1.0f since v0.10.7 U4, Batch 2 grid 260913_0002).
            // Upper-edge temperature saturates to 2.0 (report spec); 0 stays
            // meaningful (engine greedy branch, engine.cpp L1004).
            try { float t = std::stof(v);
                  // (t > 2.0f ? 2.0f : t) saturation instead of std::min:
                  // <windows.h> above is included without NOMINMAX, so the
                  // min macro would corrupt the std::min qualified call.
                  temperature = (std::isfinite(t) && t >= 0.0f) ? (t > 2.0f ? 2.0f : t) : 0.0f; } catch (...) {}
        } else if (k == "top_p") {
            try { float p = std::stof(v);
                  top_p = (std::isfinite(p) && p > 0.0f && p <= 1.0f) ? p : 0.6f; } catch (...) {}
        } else if (k == "top_k") {
            try { int k_ = std::stoi(v);
                  top_k = (k_ >= 0 && k_ <= 1000) ? k_ : 20; } catch (...) {}
        } else if (k == "repetition_penalty") {
            try { float r = std::stof(v);
                  repetition_penalty = (std::isfinite(r) && r >= 1.0f && r <= 2.0f) ? r : 1.0f; } catch (...) {}
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
    if (has_user_languages_key) {
        // 261116_0003 P9a fix PR9-09: STRICT digit-only version parse; an
        // ABSENT key is version 1. Anything else, an unparseable text
        // ("abc", "2junk") or a parsed version newer than this build, is never
        // guessed at: the block is preserved verbatim (forward compat,
        // kUserLanguagesSchemaVersion) so a downgrade cannot half-read it.
        int declared_version = kUserLanguagesSchemaVersion;
        bool version_is_current = true;
        bool version_digits_only = false;
        std::string version_text;
        if (has_user_languages_version_key) {
            version_text = std::string(TrimAsciiSpaceView(raw_user_languages_version));
            version_digits_only = !version_text.empty() &&
                std::all_of(version_text.begin(), version_text.end(),
                            [](char c) { return c >= '0' && c <= '9'; });
            if (version_digits_only) {
                try {
                    declared_version = std::stoi(version_text);
                } catch (...) {
                    // A digit run outside int range: the raw text is the only
                    // faithful form, so it takes the verbatim round trip too.
                    version_digits_only = false;
                    declared_version = kUserLanguagesSchemaVersion + 1;
                }
            }
            version_is_current =
                version_digits_only && declared_version <= kUserLanguagesSchemaVersion;
        }
        // The block is authoritative when PRESENT (it replaces the in-memory
        // registry, so a save-after-load round trip is lossless); when the key
        // is ABSENT the field keeps its current value, exactly like every other
        // key in this reader.
        if (!version_is_current) {
            // 261116_0003 P9a fix PR9-04: the carrier preserves ONLY a
            // bracket-balanced ARRAY span, re-emitted byte-identically (the
            // raw capture was never decoded, so a string value keeps its
            // quotes and escapes and can never be re-emitted unquoted to
            // fabricate sibling config keys). A non-array value FAILS CLOSED:
            // no carrier, the registry reads empty and the next save writes a
            // clean current-version block.
            // 261116_0003 P9a fix2 PR10-04: eligibility scans to the matching
            // ']' instead of testing the first/last byte, and the stored span
            // is that balanced group alone (a trailing "// next-version note"
            // is trivia, not part of the array value).
            const std::string_view trimmed_block = TrimAsciiSpaceView(raw_user_languages);
            std::string carrier_block;
            if (IsSingleJsonArraySpan(trimmed_block, carrier_block)) {
                user_languages_forward_compat_json_ = std::move(carrier_block);
                if (version_digits_only) {
                    user_languages_forward_compat_version_ = declared_version;
                    user_languages_forward_compat_version_raw_.clear();
                    user_languages_forward_compat_version_text_ = false;
                } else {
                    user_languages_forward_compat_version_ = 0;
                    user_languages_forward_compat_version_raw_ = version_text;
                    // 261116_0003 P9a fix2 PR10-02: text-typed version,
                    // including the EMPTY text an empty/whitespace-only
                    // `user_languages_version` yields. The save emits it
                    // quoted, so the next load preserves the block again
                    // instead of reading the old 0 sentinel as "current".
                    user_languages_forward_compat_version_text_ = true;
                }
            } else {
                user_languages_forward_compat_json_.clear();
                user_languages_forward_compat_version_ = 0;
                user_languages_forward_compat_version_raw_.clear();
                user_languages_forward_compat_version_text_ = false;
            }
            user_languages.clear();
        } else {
            user_languages_forward_compat_json_.clear();
            user_languages_forward_compat_version_ = 0;
            user_languages_forward_compat_version_raw_.clear();
            user_languages_forward_compat_version_text_ = false;
            std::vector<UserLanguage> parsed;
            ParseUserLanguagesBlock(raw_user_languages, parsed);
            user_languages = std::move(parsed);
        }
    }
    // 261116_0003 P9c-1a: mirror the declaration into the engine-view filter.
    // A PRESENT key replaces the set (the block is authoritative, the same rule
    // the user_languages block follows); an ABSENT key keeps the member's
    // current value, exactly like every other key in this reader. The publish
    // therefore runs on BOTH branches: the snapshot must always mirror the
    // member the menus are documented to render (a stale snapshot would filter
    // the OpenAI views with a set this document never named). The publish
    // sanitizes, so the stored member and the filtered views hold the same
    // canonical set; an unknown code is dropped and the load still succeeds.
    if (has_openai_declared_languages_key) {
        openai_declared_languages = PublishOpenAiDeclaredLanguages(
            ParseOpenAiDeclaredLanguagesArray(raw_openai_declared_languages));
    } else {
        PublishOpenAiDeclaredLanguages(openai_declared_languages);
    }
    // 261122_0001 B-U1 (design-pick §2 "마이그레이션"): a PRESENT key is
    // authoritative — it replaces the in-memory library, exactly like the
    // user_languages block. An ABSENT key never erases: when the "openai"
    // block is non-empty (any of the four identity/key fields set — consent
    // alone is not "configured"), ONE profile is synthesized from it with a
    // name derived through the pure helpers under the FIXED migration
    // template, so the same input file always yields the same library. The
    // "openai" block itself is copied BYTE-IDENTICALLY (never moved, never
    // rewritten); when it is empty the member keeps its current value, the
    // reader-wide absent-key convention.
    // 261122_0001 fix-A1 (R1-03): the synthesized profile inherits the
    // field caps and the fail-closed digest check for free — `openai` above
    // was parsed through the shared capped ApplyOpenAiConfigPair choke point,
    // so its four identity/key fields are already capped/shape-checked when
    // they are copied here.
    if (has_openai_profiles_key) {
        std::vector<OpenAiProfile> parsed_profiles;
        ParseOpenAiProfilesBlock(raw_openai_profiles, parsed_profiles);
        openai_profiles = std::move(parsed_profiles);
        // 261122_0001 fix-A1 (R1-12): a PRESENT key marks the fold touched, so
        // a re-save keeps emitting [] and the migration below cannot re-run on
        // the round trip ("deleted all profiles" survives reload + save).
        openai_profiles_set = true;
    } else if (!openai.base_url.empty() || !openai.model.empty() ||
               !openai.api_key_dpapi.empty() || !openai.api_key_sha256.empty()) {
        OpenAiProfile synthesized;
        synthesized.name = MakeOpenAiProfileName(
            OpenAiHostAbbrev(openai.base_url), openai.model,
            kOpenAiMigrationNameTemplate);
        synthesized.cfg = openai;
        openai_profiles.clear();
        openai_profiles.push_back(std::move(synthesized));
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
