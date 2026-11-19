// ---------------------------------------------------------------------------
// REQ-048 P2: OpenAI settings dialog runtime template serialization — byte-walk
// simulation of the Windows DLGTEMPLATE/DLGITEMTEMPLATE parser contract
// (GUI-free decisive proof for the "title + X only, zero controls" device
// failure). Filled by the REQ-048 P2 implementation wave; staged as an .inc
// next to this runner; registered near the end of main().
//
// Three layers:
//   (a) header-offset pin — Begin() must emit style, dwExtendedStyle, cdit,
//       x, y, cx, cy (the pre-P2 order put x..cy before cdit, so the loader
//       read cx as the item count and cdit as the height);
//   (b) production byte-walk — BuildOpenAiTemplate() serialized and walked
//       exactly the way the Windows dialog parser consumes it: 4-byte item
//       alignment, 18-byte fixed part, {0xFFFF, ordinal} class array, title
//       string, creation-data count WORD, clean consumption of exactly cdit
//       items ending at builder size();
//   (c) compatibility — the REQ-046 style-bit suite pins dt->cdit == 0 for
//       itemCount=0; the new layout must keep that true.
//
// REQ-060 (session 261009_0001): a fourth section byte-walks
// BuildEngineHostSettingsTemplate() — the translation-model load-mode dialog —
// with the same parser contract: 5 items, the CBS_DROPDOWNLIST combo style
// pin, and the OK/Cancel pair.
// ---------------------------------------------------------------------------

#include "../src/ui/engine_host_settings_window.hpp" // REQ-060: BuildEngineHostSettingsTemplate

namespace req048_detail {

struct ItemExpectation {
    WORD id;
    WORD ordinal;
    bool has_title;
    std::wstring title;
    short x, y, cx, cy; // REQ-050: pin the production geometry too
};

// Production constants mirrored from openai_settings_window.cpp (enum starts
// at IDC_STATIC_BASE = 100; system-class ordinals 0x0080..0x0085 + the
// 261116_0003 P9c-1b LISTBOX 0x0083).
constexpr WORD kStaticCls = 0x0082;
constexpr WORD kEditCls   = 0x0081;
constexpr WORD kBtnCls    = 0x0080;
constexpr WORD kComboCls  = 0x0085;
constexpr WORD kListboxCls = 0x0083;

// REQ-050: geometry matches BuildOpenAiTemplate exactly (262x130 dialog pre-
// P9c-1b; 262x174 after); labels w=56; edits x=68 w=186; combo x=68 w=118;
// fetch x=190 w=64; masked label w=240; REQ-051 delete x=8 y=70->154 (P9c-1b
// moved the action row) w=64 h=13; OK x=110 w=64; Cancel x=182 w=64 — all
// h/y per spec. P9c-1b appends the declaration checklist: title y=66 h=9,
// list y=77 h=52, hint y=132 h=18 (all x=8 w=240).
const ItemExpectation kExpected[] = {
    {100, kStaticCls, false, L"",   8,   6, 56,  9},  // IDC_STATIC_BASE
    {101, kEditCls,   true,  L"",   68,  5, 186, 12}, // IDC_BASE_URL
    {102, kStaticCls, false, L"",   8,   22, 56,  9}, // IDC_STATIC_KEY
    {103, kEditCls,   true,  L"",   68,  21, 186, 12},// IDC_API_KEY
    {104, kStaticCls, false, L"",   8,   38, 56,  9}, // IDC_STATIC_MODEL
    {105, kComboCls,  true,  L"",   68,  37, 118, 64},// IDC_MODEL_COMBO
    {106, kBtnCls,    false, L"",   190, 37, 64,  12},// IDC_FETCH_BTN (i18n title)
    {107, kStaticCls, true,  L"",   8,   54, 240, 9}, // IDC_MASKED_LABEL
    {109, kStaticCls, false, L"",   8,   66, 240, 9}, // P9c-1b IDC_DECLARE_TITLE (i18n title)
    {110, kListboxCls,true,  L"",   8,   77, 240, 52},// P9c-1b IDC_DECLARE_LIST (empty title)
    {111, kStaticCls, false, L"",   8,   132, 240, 18},// P9c-1b IDC_DECLARE_HINT (i18n title)
    {108, kBtnCls,    false, L"",   8,   154, 64, 13},// REQ-051 IDC_DELETE_BTN (i18n title)
    {IDOK,     kBtnCls, true, L"OK",     110, 154, 64, 13},
    {IDCANCEL, kBtnCls, true, L"Cancel", 182, 154, 64, 13},
};

// Little-endian WORD/DWORD readers over the raw template buffer.
WORD ReadWord(const BYTE* p, size_t off) {
    return static_cast<WORD>(p[off] | (static_cast<WORD>(p[off + 1]) << 8));
}
DWORD ReadDword(const BYTE* p, size_t off) {
    return static_cast<DWORD>(ReadWord(p, off)) |
           (static_cast<DWORD>(ReadWord(p, off + 2)) << 16);
}
// Advances *off past a null-terminated wide string; returns the string.
std::wstring ReadString(const BYTE* p, size_t* off) {
    std::wstring s;
    for (;;) {
        const wchar_t ch = static_cast<wchar_t>(ReadWord(p, *off));
        *off += 2;
        if (ch == L'\0') break;
        s += ch;
    }
    return s;
}

// 261116_0003 P9c-1b: the dialog's pure declaration fold helpers, defined at
// namespace scope in src/ui/openai_settings_window.cpp. Declared here (the
// suite rides inside run_tests.cpp, which links Emebalachat_core) because
// openai_settings_window.hpp is outside this unit's write boundary - the
// suite drives the exact helpers the dialog proc calls.
} // namespace req048_detail

namespace emebalachat {
std::vector<std::string> OpenAiDeclarationRowCodes();
std::vector<int> OpenAiDeclarationSelection(const std::vector<std::string>& declared,
                                            const std::vector<std::string>& rows);
std::vector<std::string> OpenAiDeclarationFromSelection(
    const std::vector<int>& selected, const std::vector<std::string>& rows);
} // namespace emebalachat

void TestReq048OpenAiTemplateSerialization() {
    std::cout << "[RUN] Testing REQ-048 P2 OpenAI template serialization..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) Header offset pin -------------------------------------------
    {
        TemplateBuilder tb;
        // P9c-1b: probe carries the production geometry (262 x 174).
        tb.Begin(L"probe", 262, 174, /*itemCount=*/7);
        const auto* base = reinterpret_cast<const BYTE*>(tb.Get());
        TEST_CHECK(base != nullptr, "REQ-048 P2 (a): Begin() returns a template");
        if (base) {
            TEST_CHECK(req048_detail::ReadDword(base, 0) ==
                           (WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT |
                            DS_MODALFRAME | WS_VISIBLE | DS_CENTER | DS_SETFOREGROUND),
                       "REQ-048 P2 (a): offset 0..3 == style DWORD");
            TEST_CHECK(req048_detail::ReadDword(base, 4) == 0,
                       "REQ-048 P2 (a): offset 4..7 == dwExtendedStyle");
            TEST_CHECK(req048_detail::ReadWord(base, 8) == 7,
                       "REQ-048 P2 (a): byte offset 8 == cdit (7)");
            TEST_CHECK(req048_detail::ReadWord(base, 10) == 0,
                       "REQ-048 P2 (a): byte offset 10 == x (0)");
            TEST_CHECK(req048_detail::ReadWord(base, 12) == 0,
                       "REQ-048 P2 (a): byte offset 12 == y (0)");
            TEST_CHECK(req048_detail::ReadWord(base, 14) == 262,
                       "REQ-048 P2 (a): byte offset 14 == cx (262)");
            TEST_CHECK(req048_detail::ReadWord(base, 16) == 174,
                       "REQ-048 P2 (a): byte offset 16 == cy (174)");
        }
    }

    // ---- (c) Compatibility with TestReq046OpenAiTemplateStyle ------------
    // That suite pins dt->cdit == 0 after Begin(..., 0); the new field order
    // must keep the struct-view contract identical.
    {
        TemplateBuilder tb;
        tb.Begin(L"probe", 262, 130, /*itemCount=*/0);
        const auto* dt = tb.Get();
        TEST_CHECK(dt != nullptr && dt->cdit == 0,
                   "REQ-048 P2 (c): itemCount=0 still round-trips to cdit==0 "
                   "(TestReq046OpenAiTemplateStyle stays green)");
    }

    // ---- (b) Production byte-walk ----------------------------------------
    // REQ-050: the OK/Cancel button titles are i18n strings now
    // (StringId::DialogOk/DialogCancel), so pin the locale to English for the
    // title comparison and restore afterwards (suite-state hygiene).
    const UiLocale req050_initial_locale = I18n::GetCurrentLocale();
    I18n::SetLocale(UiLocale::English);
    {
        using namespace req048_detail;
        TemplateBuilder tb;
        // REQ-050: production dialog is 262 wide; REQ-051: 14 items — the
        // REQ-051 IDC_DELETE_BTN button joined the 10-control REQ-048 P2
        // template, and 261116_0003 P9c-1b added the 3-control declaration
        // checklist section (title + list + hint), shell 262x130 -> 262x174.
        tb.Begin(L"production", 262, 174, /*itemCount=*/14);
        BuildOpenAiTemplate(tb);
        const BYTE* base = reinterpret_cast<const BYTE*>(tb.Get());
        const size_t tpl_base = 0; // buffer origin == template origin for a fresh builder
        TEST_CHECK(base != nullptr, "REQ-048 P2 (b): production template buffer non-null");
        if (!base) return;

        // Header: style, exstyle, cdit, x, y, cx, cy (18 bytes).
        size_t off = 18;
        const WORD cdit = ReadWord(base, 8);
        TEST_CHECK(cdit == 14,
                   "REQ-048 P2 (b): header cdit == 14 (9 controls + 3 declaration "
                   "checklist controls + OK + Cancel)");
        // menu (WORD 0), windowClass (WORD 0), title string, pointsize, font.
        TEST_CHECK(ReadWord(base, off) == 0, "REQ-048 P2 (b): menu == 0 (none)");
        off += 2;
        TEST_CHECK(ReadWord(base, off) == 0, "REQ-048 P2 (b): windowClass == 0 (none)");
        off += 2;
        const std::wstring dlg_title = ReadString(base, &off);
        TEST_CHECK(dlg_title == L"production",
                   "REQ-048 P2 (b): dialog title string round-trips");
        const WORD pointsize = ReadWord(base, off);
        off += 2;
        TEST_CHECK(pointsize == 9, "REQ-048 P2 (b): pointsize == 9");
        const std::wstring font = ReadString(base, &off);
        TEST_CHECK(font == L"Segoe UI", "REQ-048 P2 (b): font face == L\"Segoe UI\"");

        bool ok_seen = false;
        bool cancel_seen = false;
        for (WORD i = 0; i < cdit; ++i) {
            // The builder pads each item start to a DWORD boundary relative to
            // the DLGTEMPLATE (AddItem's while-loop). The Windows parser lands
            // on the next aligned offset after the previous item's variable
            // data, so the walk must skip those pad bytes (0..3, always whole
            // WORDs here) before reading the next item.
            off += (4 - (off - tpl_base) % 4) % 4;
            // Item starts must be 4-byte aligned relative to the DLGTEMPLATE.
            TEST_CHECK((off - tpl_base) % 4 == 0,
                       "REQ-048 P2 (b): item start is 4-byte aligned");
            // Fixed part: style(4) exstyle(4) x(2) y(2) cx(2) cy(2) id(2) = 18.
            const WORD id = ReadWord(base, off + 16);
            TEST_CHECK(i < 14, "REQ-048 P2 (b): item index within expectation table");
            if (i >= 14) break;
            const ItemExpectation& exp = kExpected[i];
            TEST_CHECK(id == exp.id, "REQ-048 P2 (b): item id matches production constant");
            // REQ-050: geometry pin — x/y/cx/cy must match the production
            // AddItem coordinates exactly (labels w=56, edits x=68 w=186,
            // fetch x=190 w=64, REQ-051 delete x=8 y=154, P9c-1b checklist
            // x=8 w=240, OK x=110 w=64, Cancel x=182 w=64).
            // Fixed part: style(4) exstyle(4) x(2) y(2) cx(2) cy(2) id(2) = 18,
            // so x/y/cx/cy live at off+8..off+15 (id is off+16).
            TEST_CHECK(ReadWord(base, off + 8) == static_cast<WORD>(exp.x) &&
                       ReadWord(base, off + 10) == static_cast<WORD>(exp.y) &&
                       ReadWord(base, off + 12) == static_cast<WORD>(exp.cx) &&
                       ReadWord(base, off + 14) == static_cast<WORD>(exp.cy),
                       "REQ-050: item x/y/cx/cy match the new dialog geometry");
            // REQ-050 (single-line edit limits): the two EDIT items must
            // carry ES_AUTOHSCROLL (0x0080) — a single-line EDIT without it
            // rejects input beyond its visible width (device-confirmed on the
            // HF add dialog's URL edit, same latent hazard here). Styles:
            // WS_CHILD|WS_VISIBLE|WS_BORDER|WS_TABSTOP = 0x50810000, +0x0080 =
            // 0x50810080, +ES_PASSWORD (0x0020) = 0x508100A0.
            if (exp.id == 101 || exp.id == 103) {
                TEST_CHECK(ReadDword(base, off) ==
                               (exp.id == 101 ? 0x50810080u : 0x508100A0u),
                           "REQ-050: single-line edits carry ES_AUTOHSCROLL "
                           "(IDC_BASE_URL/IDC_API_KEY)");
            }
            // 261116_0003 P9c-1b: the declaration LISTBOX (110) must be the
            // multi-select idiom and must NOT sort: the listbox index IS the
            // row index of the registry-order fold. Styles:
            // WS_CHILD|WS_VISIBLE|WS_BORDER|WS_VSCROLL|WS_TABSTOP = 0x50A10000,
            // +LBS_HASSTRINGS(0x40) +LBS_MULTIPLESEL(0x08) +LBS_NOTIFY(0x01) =
            // 0x50A10049 (LBS_SORT 0x0002 absent).
            if (exp.id == 110) {
                TEST_CHECK(ReadDword(base, off) == 0x50A10049u,
                           "P9c-1b: the declaration listbox carries LBS_MULTIPLESEL "
                           "and no LBS_SORT (the index==row fold)");
            }
            off += 18;
            // Class array: 0xFFFF prefix WORD + ordinal WORD.
            TEST_CHECK(ReadWord(base, off) == 0xFFFF,
                       "REQ-048 P2 (b): class array carries the 0xFFFF ordinal prefix");
            TEST_CHECK(ReadWord(base, off + 2) == exp.ordinal,
                       "REQ-048 P2 (b): class ordinal matches production constant");
            off += 4;
            // Title string (empty for edits/combo/label; OK/Cancel pinned).
            const std::wstring title = ReadString(base, &off);
            if (exp.has_title) {
                TEST_CHECK(title == exp.title,
                           "REQ-048 P2 (b): item title matches production constant");
            }
            // Creation data: count WORD == 0.
            TEST_CHECK(ReadWord(base, off) == 0,
                       "REQ-048 P2 (b): creation-data count WORD == 0");
            off += 2;
            if (id == IDOK) ok_seen = true;
            if (id == IDCANCEL) cancel_seen = true;
        }
        TEST_CHECK(ok_seen && cancel_seen,
                   "REQ-048 P2 (b): IDOK(1) and IDCANCEL(2) present in template");
        TEST_CHECK(off == tb.size(),
                   "REQ-048 P2 (b): parser consumed exactly cdit items and ends at size()");
    }
    // ---- REQ-060: engine-host load-mode settings dialog template ----------
    // Same parser-contract walk as (b) for the translation-model load-mode
    // dialog: 5 items (title label + mode combo + description static + OK +
    // Cancel) on the 240x90 shell, with the CBS_DROPDOWNLIST style pin (a
    // fixed two-option list — no free-text leg, unlike the OpenAI model
    // combo's CBS_DROPDOWN).
    {
        const UiLocale req060_initial_locale = I18n::GetCurrentLocale();
        I18n::SetLocale(UiLocale::Korean); // title/combo labels
        using namespace req048_detail;
        TemplateBuilder tb;
        tb.Begin(L"production", 240, 90, /*itemCount=*/5);
        BuildEngineHostSettingsTemplate(tb);
        const BYTE* base = reinterpret_cast<const BYTE*>(tb.Get());
        TEST_CHECK(base != nullptr,
                   "REQ-060: engine-host settings template buffer non-null");
        if (base) {
            const WORD cdit = ReadWord(base, 8);
            TEST_CHECK(cdit == 5,
                       "REQ-060: template emits 5 items (title + combo + desc + OK + Cancel)");
            TEST_CHECK(ReadWord(base, 14) == 240 && ReadWord(base, 16) == 90,
                       "REQ-060: dialog geometry 240x90 DLU");
            struct Exp060 { WORD id; WORD cls; short x, y, cx, cy; std::wstring title; };
            const Exp060 kExp060[] = {
                {4400, kStaticCls, 8, 6, 224, 9,
                 I18n::Get(StringId::EngineLoadSettingsTitle)},
                {4401, kComboCls, 8, 20, 224, 64, L""},
                {4402, kStaticCls, 8, 40, 224, 22, L""},
                {IDOK, kBtnCls, 104, 72, 60, 13, I18n::Get(StringId::DialogOk)},
                {IDCANCEL, kBtnCls, 172, 72, 60, 13,
                 I18n::Get(StringId::DialogCancel)},
            };
            size_t off = 18; // header
            off += 2;        // menu
            off += 2;        // windowClass
            off += 2 * (1 + std::wstring(L"production").size()); // title + NUL
            off += 2;        // pointsize
            off += 2 * (1 + std::wstring(L"Segoe UI").size());   // font + NUL
            bool walk060_ok = true;
            for (WORD iItem = 0; iItem < cdit && iItem < 5; ++iItem) {
                off += (4 - off % 4) % 4; // DWORD-align relative to the template
                const Exp060& e = kExp060[iItem];
                const WORD id = ReadWord(base, off + 16);
                const WORD cls = ReadWord(base, off + 20);
                const short x = static_cast<short>(ReadWord(base, off + 8));
                const short y = static_cast<short>(ReadWord(base, off + 10));
                const short cx = static_cast<short>(ReadWord(base, off + 12));
                const short cy = static_cast<short>(ReadWord(base, off + 14));
                size_t toff = off + 22; // fixed part 18 + class array 4
                const std::wstring title = ReadString(base, &toff);
                if (id != e.id || cls != e.cls || x != e.x || y != e.y ||
                    cx != e.cx || cy != e.cy || title != e.title) {
                    walk060_ok = false;
                    break;
                }
                // REQ-060: the mode combo must be CBS_DROPDOWNLIST — the two
                // load-mode options are a fixed pick list. Styles:
                // WS_CHILD|WS_VISIBLE|WS_BORDER|WS_TABSTOP = 0x50810000,
                // +CBS_DROPDOWNLIST (0x0003) = 0x50810003.
                if (e.id == 4401) {
                    TEST_CHECK(ReadDword(base, off) == 0x50810003u,
                               "REQ-060: mode combo carries CBS_DROPDOWNLIST "
                               "(fixed option list, no free-text leg)");
                }
                off = toff + 2; // creation-data count WORD
            }
            TEST_CHECK(walk060_ok,
                       "REQ-060: engine-host settings template items 1:1 — "
                       "title/combo/desc geometry + i18n captions");
            TEST_CHECK(off == tb.size(),
                       "REQ-060: engine-host settings template walk consumed exactly size() bytes");
        }
        I18n::SetLocale(req060_initial_locale); // suite-state hygiene
    }
    // ---- 261116_0003 P9c-1b: the OpenAI declaration checklist --------------
    // Pinned here, headlessly (no window is created - AGENTS.md §10; the modal
    // loop stays a manual-QA badge item, the same split P9b-3 U2 recorded):
    //   (i)   the template wiring of the 3 new controls (captions on the two
    //         landed StringIds, the multi-select/no-sort listbox idiom);
    //   (ii)  the i18n landing of OpenAiDeclaredTitle/Hint (ko/en literals,
    //         37-locale non-empty, em-dash-free, 36 designators, the Get()
    //         case, the English positional adjacency, the enum placement);
    //   (iii) the declaration fold round-trip through the REAL helpers the
    //         dialog proc calls (selection <-> declaration, both directions,
    //         including the canonicalization and the fail-open);
    //   (iv)  the wiring: IDOK folds the listbox selection and applies the
    //         declaration through P9c-1a's LOCKED VERB (never the member,
    //         never the bypass publish), exactly one call site;
    //   (v)   the UNFILTERED draw: the row set comes from the registry +
    //         the kLangOpenAi capability bit, and the whole settings TU
    //         never reads GetLanguagesForEngine (the P9c-1a handoff hazard:
    //         that view is declaration-filtered, so drawing from it would
    //         hide every unchecked language forever).
    {
        const UiLocale p9c1b_initial_locale = I18n::GetCurrentLocale();
        const auto eq_s = [](std::size_t lhs, std::size_t rhs) { return lhs == rhs; };
        const auto count_of_str = [](const std::string& haystack, const std::string& needle) {
            std::size_t count = 0;
            for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
                 pos = haystack.find(needle, pos + needle.size())) {
                ++count;
            }
            return count;
        };
        const auto read_src = [](const std::filesystem::path& path, std::string& out) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return false;
            std::ostringstream ss;
            ss << in.rdbuf();
            out = ss.str();
            return true;
        };
        const std::filesystem::path osw_cpp =
            ResolveRepoFile(L"src\\ui\\openai_settings_window.cpp");
        const std::filesystem::path i18n_cpp = ResolveRepoFile(L"src\\i18n.cpp");
        std::string osw_src;
        std::string i18n_src;
        const bool sources_ok = !osw_cpp.empty() && !i18n_cpp.empty() &&
                                read_src(osw_cpp, osw_src) && read_src(i18n_cpp, i18n_src);
        TEST_CHECK(sources_ok,
                   "P9c-1b: the settings + i18n sources resolve for the scan pins");

        // -- (i) the template wiring of the 3 new controls -------------------
        if (!osw_src.empty()) {
            TEST_CHECK(osw_src.find("IDC_DECLARE_TITLE") != std::string::npos &&
                           osw_src.find("IDC_DECLARE_LIST") != std::string::npos &&
                           osw_src.find("IDC_DECLARE_HINT") != std::string::npos,
                       "P9c-1b (i): the template carries the 3 declaration controls");
            TEST_CHECK(osw_src.find("I18n::Get(StringId::OpenAiDeclaredTitle)") !=
                           std::string::npos &&
                           osw_src.find("I18n::Get(StringId::OpenAiDeclaredHint)") !=
                               std::string::npos,
                       "P9c-1b (i): the section title + hint render the landed StringIds");
            TEST_CHECK(osw_src.find("LBS_MULTIPLESEL") != std::string::npos &&
                           osw_src.find("LBS_SORT") == std::string::npos,
                       "P9c-1b (i): the checklist listbox is multi-select and never sorted "
                       "(the listbox index IS the fold's row index)");
        }

        // -- (ii) the i18n landing -------------------------------------------
        I18n::SetLocale(UiLocale::English);
        TEST_CHECK(I18n::Get(StringId::OpenAiDeclaredTitle) ==
                       L"Languages this model supports",
                   "P9c-1b (ii): the English section title matches the authored copy");
        TEST_CHECK(I18n::Get(StringId::OpenAiDeclaredHint) ==
                       L"Only the languages you select are shown in the language "
                       L"menus. When none are selected, all languages are shown.",
                   "P9c-1b (ii): the English fail-open hint matches the authored copy");
        I18n::SetLocale(UiLocale::Korean);
        TEST_CHECK(I18n::Get(StringId::OpenAiDeclaredTitle) == L"이 모델이 지원하는 언어",
                   "P9c-1b (ii): the Korean section title matches the authored copy verbatim");
        TEST_CHECK(I18n::Get(StringId::OpenAiDeclaredHint) ==
                       L"선택한 언어만 언어 목록에 표시됩니다. 아무것도 선택하지 않으면 "
                       L"모든 언어가 표시됩니다.",
                   "P9c-1b (ii): the Korean fail-open hint matches the authored copy verbatim");
        {
            const auto& locales = GetSupportedUiLocales();
            int non_empty = 0;
            int with_emdash = 0;
            for (const UiLocaleEntry& entry : locales) {
                I18n::SetLocale(entry.locale);
                if (!I18n::Get(StringId::OpenAiDeclaredTitle).empty() &&
                    !I18n::Get(StringId::OpenAiDeclaredHint).empty()) {
                    ++non_empty;
                }
                if (I18n::Get(StringId::OpenAiDeclaredTitle).find(L"\u2014") !=
                        std::wstring::npos ||
                    I18n::Get(StringId::OpenAiDeclaredHint).find(L"\u2014") !=
                        std::wstring::npos) {
                    ++with_emdash;
                }
            }
            TEST_CHECK(eq_s(non_empty, locales.size()) && eq_s(locales.size(), 37),
                       "P9c-1b (ii): both strings resolve non-empty in all 37 locales "
                       "(the English placeholder is allowed where no translation was authored)");
            TEST_CHECK(with_emdash == 0,
                       "P9c-1b (ii): no locale carries an em-dash (R14-10 hyphen separator)");
        }
        if (!i18n_src.empty()) {
            TEST_CHECK(count_of_str(i18n_src, "    .openai_declared_title = ") == 36 &&
                           count_of_str(i18n_src, "    .openai_declared_hint = ") == 36,
                       "P9c-1b (ii): both fields are designated in exactly 36 tables (the "
                       "37th is the positional English one)");
            const std::string title_case = "case StringId::OpenAiDeclaredTitle:";
            const std::string hint_case = "case StringId::OpenAiDeclaredHint:";
            const std::size_t title_at = i18n_src.find(title_case);
            const std::size_t hint_at = i18n_src.find(hint_case);
            const std::size_t title_end = i18n_src.find('\n', title_at);
            const std::size_t hint_end = i18n_src.find('\n', hint_at);
            TEST_CHECK(title_at != std::string::npos && hint_at != std::string::npos &&
                           i18n_src.find("return s.openai_declared_title;", title_at) <
                               title_end &&
                           i18n_src.find("return s.openai_declared_hint;", hint_at) <
                               hint_end,
                       "P9c-1b (ii): the Get() switch maps both ids to their members");
            TEST_CHECK(i18n_src.find(
                           "    L\"Languages this model supports\",\n"
                           "    L\"Only the languages you select are shown in the language "
                           "menus. When none are selected, all languages are shown.\",\n"
                           "    L\"Auto - local for supported languages, cloud for the rest\",\n"
                           "};") != std::string::npos,
                       "P9c-1b (ii): the English positional table carries the two literals "
                       "immediately before the AUTO chip line (which stays its last "
                       "initializer per the P9b-3 U5 tail pin)");
        }
        // Enum placement: the two ids close the pre-MenuSearchLangs block
        // (the P5 sentinel pin owns the enum tail), same rule as P9b-1/U5.
        {
            volatile int auto_chip = static_cast<int>(StringId::LanguageSearchAutoChip);
            volatile int title_id = static_cast<int>(StringId::OpenAiDeclaredTitle);
            volatile int hint_id = static_cast<int>(StringId::OpenAiDeclaredHint);
            volatile int menu_search = static_cast<int>(StringId::MenuSearchLangs);
            TEST_CHECK(auto_chip + 1 == title_id && title_id + 1 == hint_id &&
                           hint_id + 1 == menu_search,
                       "P9c-1b (ii): the two ids sit contiguously between "
                       "LanguageSearchAutoChip and MenuSearchLangs");
        }

        // -- (iii) the declaration fold round-trip ----------------------------
        {
            using emebalachat::OpenAiDeclarationFromSelection;
            using emebalachat::OpenAiDeclarationRowCodes;
            using emebalachat::OpenAiDeclarationSelection;
            const std::vector<std::string> rows = OpenAiDeclarationRowCodes();
            // The expectation is derived from the REGISTRY (the independent
            // source), never from the helper under test: every kLangOpenAi
            // row but AUTO, in registry order.
            std::vector<std::string> expected_rows;
            for (const LanguageInfo& row : GetSupportedLanguages()) {
                if (row.code == "AUTO") continue;
                if ((row.engine_mask & kLangOpenAi) == 0) continue;
                expected_rows.push_back(row.code);
            }
            TEST_CHECK(rows == expected_rows,
                        "P9c-1b (iii): the checklist rows are every kLangOpenAi registry row "
                        "but AUTO, in registry order (derived from the registry)");
            TEST_CHECK(eq_s(rows.size(), 89) &&
                           std::find(rows.begin(), rows.end(), "AUTO") == rows.end(),
                       "P9c-1b (iii): 89 unfiltered rows, AUTO excluded (the OpenAI "
                       "target-view size)");
            const auto index_of = [&rows](const std::string& code) -> int {
                for (std::size_t i = 0; i < rows.size(); ++i) {
                    if (rows[i] == code) return static_cast<int>(i);
                }
                return -1;
            };
            // Declaration -> indices -> declaration (the round-trip fold).
            // The indices follow the ROW (registry) order - which is EN, KO,
            // ..., JA, not the declaration's order - so the expectation is
            // the three codes' row indices sorted ascending, derived from the
            // rows themselves, never from a hardcoded order.
            std::vector<int> expect_sel = {index_of("KO"), index_of("JA"),
                                           index_of("EN")};
            std::sort(expect_sel.begin(), expect_sel.end());
            const std::vector<int> sel =
                OpenAiDeclarationSelection({"KO", "JA", "EN"}, rows);
            TEST_CHECK(sel == expect_sel,
                       "P9c-1b (iii): a declaration maps to its rows' selected indices, "
                       "in registry (row) order");
            std::vector<std::string> expect_codes;
            for (const int idx : expect_sel) {
                expect_codes.push_back(rows[static_cast<std::size_t>(idx)]);
            }
            TEST_CHECK(OpenAiDeclarationFromSelection(sel, rows) == expect_codes,
                       "P9c-1b (iii): the selected indices fold back to the declaration");
            // Declaration ORDER is irrelevant: the same set in a different
            // order selects identically and folds to the identical codes
            // (the canonical registry order).
            const std::vector<int> fwd = OpenAiDeclarationSelection({"KO", "JA"}, rows);
            const std::vector<int> rev = OpenAiDeclarationSelection({"JA", "KO"}, rows);
            TEST_CHECK(!fwd.empty() && fwd == rev,
                       "P9c-1b (iii): declaration order is irrelevant - the selection is "
                       "always the row order");
            TEST_CHECK(OpenAiDeclarationFromSelection(rev, rows) ==
                           OpenAiDeclarationFromSelection(fwd, rows),
                       "P9c-1b (iii): the fold canonicalizes to registry order");
            // Unknown codes select nothing; duplicates collapse.
            TEST_CHECK(eq_s(OpenAiDeclarationSelection({"KO", "XX-UNKNOWN"}, rows).size(), 1),
                       "P9c-1b (iii): an unknown code selects nothing");
            TEST_CHECK(eq_s(OpenAiDeclarationSelection({"KO", "KO", "ko"}, rows).size(), 1),
                       "P9c-1b (iii): duplicate spellings collapse to one selection");
            // Invalid and duplicate indices drop on the way back.
            const int ko_idx = index_of("KO");
            TEST_CHECK(OpenAiDeclarationFromSelection({ko_idx, ko_idx, -5, 99999}, rows) ==
                           std::vector<std::string>({"KO"}),
                       "P9c-1b (iii): invalid and duplicate indices drop from the fold");
            // Empty selection = empty declaration = the documented fail-open.
            TEST_CHECK(OpenAiDeclarationSelection({}, rows).empty() &&
                           OpenAiDeclarationFromSelection({}, rows).empty(),
                       "P9c-1b (iii): an empty selection folds to an empty declaration "
                       "(not declared - the views offer the full set)");
            // Full round-trip over every row, both directions.
            std::vector<int> all_indices;
            all_indices.reserve(rows.size());
            for (std::size_t i = 0; i < rows.size(); ++i) {
                all_indices.push_back(static_cast<int>(i));
            }
            TEST_CHECK(OpenAiDeclarationFromSelection(all_indices, rows) == rows,
                       "P9c-1b (iii): selecting every row folds to the full registry-ordered "
                       "set");
            TEST_CHECK(OpenAiDeclarationSelection(rows, rows) == all_indices,
                       "P9c-1b (iii): declaring every row selects every index");
        }

        // -- (iv) the wiring: IDOK -> the declaration fold --------------------
        // 261116_0003 P9c-1c retarget: P9c-1b pinned IDOK applying the fold
        // through the locked verb on a SECOND AppConfig instance (LoadFromFile
        // -> verb -> SaveToFile). That write was reverted by the owning
        // instance's own save one line later (the P9c-1b §5 handoff), so the
        // apply moved to the owner and the dialog does no config file I/O for
        // the declaration at all. The two checks below now pin the corrected
        // contract instead of the removed one; everything else is unchanged.
        if (!osw_src.empty()) {
            const std::size_t ok_at = osw_src.find("case IDOK");
            const std::size_t cancel_at = osw_src.find("case IDCANCEL");
            const bool window_ok = ok_at != std::string::npos &&
                                   cancel_at != std::string::npos && ok_at < cancel_at;
            TEST_CHECK(window_ok, "P9c-1b (iv): the IDOK window is locatable");
            if (window_ok) {
                const std::string ok_window =
                    osw_src.substr(ok_at, cancel_at - ok_at);
                TEST_CHECK(ok_window.find("OpenAiDeclarationFromSelection(") !=
                               std::string::npos &&
                               ok_window.find("st->declaration_edited = true;") !=
                                   std::string::npos,
                           "P9c-1b (iv) + P9c-1c: IDOK folds the listbox selection through the "
                           "shared helper and flags the fold for the declaration-aware entry");
                TEST_CHECK(ok_window.find("AppConfig") == std::string::npos &&
                               ok_window.find("LoadFromFile()") == std::string::npos &&
                               ok_window.find("SaveToFile()") == std::string::npos &&
                               ok_window.find("SetOpenAiDeclaredLanguages(") ==
                                   std::string::npos,
                           "P9c-1c (a): IDOK performs NO config file I/O for the declaration - the "
                           "P9c-1b second-instance read-modify-write is gone (the owning config "
                           "applies the fold before its own save)");
            }
            TEST_CHECK(count_of_str(osw_src, "SetOpenAiDeclaredLanguages(") == 0,
                       "P9c-1c (a): the settings TU has ZERO locked-verb call sites - the dialog "
                       "never writes the declaration, the owning config is the single apply path");
            TEST_CHECK(osw_src.find("PublishOpenAiDeclaredLanguages") ==
                           std::string::npos,
                       "P9c-1b (iv): the bypass publish is never called from the UI "
                       "(the verb is the single publication path)");
            TEST_CHECK(osw_src.find(".openai_declared_languages") ==
                               std::string::npos &&
                           osw_src.find("->openai_declared_languages") ==
                               std::string::npos &&
                           osw_src.find("openai_declared_languages =") ==
                               std::string::npos,
                       "P9c-1b (iv): the member is never touched directly (a direct write "
                       "would skip the publish)");
        }

        // -- (v) the UNFILTERED draw ------------------------------------------
        if (!osw_src.empty()) {
            const std::string rows_sig =
                "std::vector<std::string> OpenAiDeclarationRowCodes()";
            const std::size_t rows_at = osw_src.find(rows_sig);
            TEST_CHECK(rows_at != std::string::npos,
                       "P9c-1b (v): the row-set helper is locatable");
            if (rows_at != std::string::npos) {
                const std::size_t rows_end = osw_src.find("\n}\n", rows_at);
                const std::string rows_body = osw_src.substr(
                    rows_at, rows_end == std::string::npos ? std::string::npos
                                                           : rows_end - rows_at);
                TEST_CHECK(rows_body.find("GetSupportedLanguages()") !=
                                   std::string::npos &&
                               rows_body.find("kLangOpenAi") != std::string::npos,
                           "P9c-1b (v): the row set is built from the UNFILTERED registry "
                           "plus the kLangOpenAi capability bit");
                TEST_CHECK(rows_body.find("GetLanguagesForEngine") ==
                               std::string::npos,
                           "P9c-1b (v): the row-set helper never reads the filtered view");
            }
            TEST_CHECK(count_of_str(osw_src, "GetLanguagesForEngine") == 0,
                       "P9c-1b (v): the settings TU NEVER calls GetLanguagesForEngine - "
                       "that view is declaration-filtered since P9c-1a, so drawing from "
                       "it would hide every unchecked language (the handoff hazard)");
            const std::size_t init_at = osw_src.find("case WM_INITDIALOG");
            const std::size_t cmd_at = osw_src.find("case WM_COMMAND");
            const bool init_ok = init_at != std::string::npos &&
                                 cmd_at != std::string::npos && init_at < cmd_at;
            TEST_CHECK(init_ok, "P9c-1b (v): the INITDIALOG window is locatable");
            if (init_ok) {
                const std::string init_window =
                    osw_src.substr(init_at, cmd_at - init_at);
                TEST_CHECK(init_window.find("OpenAiDeclarationRowCodes()") !=
                               std::string::npos &&
                               init_window.find("LB_ADDSTRING") != std::string::npos &&
                               init_window.find("LB_SETSEL") != std::string::npos &&
                               init_window.find("OpenAiLanguageDeclared") !=
                                   std::string::npos,
                           "P9c-1b (v): INITDIALOG populates the listbox from the "
                           "unfiltered row set and pre-selects the declared codes via "
                           "the published-snapshot read seam");
            }
        }

        // ---- 261116_0003 P9c-1c: the declaration reaches the LIVE config -----
        // P9c-1b left the dialog persisting the declaration through a SECOND
        // AppConfig instance, whose write the owning instance's own save then
        // reverted (that unit's §5 handoff). P9c-1c carries the fold out through
        // the declaration-aware entry and applies it on the OWNING instance in
        // main.cpp BEFORE that instance's save. Pinned here:
        //   (a) the split of duties: the dialog TU applies nothing and main.cpp
        //       applies the fold exactly once, inside g_apply_openai_settings,
        //       ahead of config.SaveToFile(); Cancel passes no fold at all;
        //   (b) the fold IS the set the locked verb stores (canonical registry
        //       order both ways, so the dialog cannot drift from the file);
        //   (c) the emission rule: a cleared declaration emits no key at all;
        //   (d) the end-to-end chain the user runs - declare -> OK -> the
        //       owner's verb -> SaveToFile -> LoadFromFile -> the OpenAI view
        //       offers exactly the declared set - plus the clear direction.
        {
            const std::filesystem::path main_cpp = ResolveRepoFile(L"src\\main.cpp");
            std::string main_src;
            const bool main_ok = !main_cpp.empty() && read_src(main_cpp, main_src);
            TEST_CHECK(main_ok, "P9c-1c: src/main.cpp resolves for the apply-path pins");

            // The suite mutates the process-wide declaration snapshot below;
            // capture the pre-test state so the surrounding suites (which
            // assume the fail-open view unless they arm it themselves) are
            // unaffected.
            std::vector<std::string> snapshot_before;
            for (const LanguageInfo& row : GetSupportedLanguages()) {
                if (row.code != "AUTO" && OpenAiLanguageDeclared(row.code)) {
                    snapshot_before.push_back(row.code);
                }
            }

            // -- (a) the split of duties -------------------------------------
            if (!osw_src.empty()) {
                TEST_CHECK(osw_src.find("OpenAiDeclaredLanguagesFold& declaration)") !=
                                   std::string::npos &&
                               osw_src.find("declaration.edited = st.declaration_edited;") !=
                                   std::string::npos &&
                               osw_src.find("declaration.declared_languages = st.declared_out;") !=
                                   std::string::npos,
                           "P9c-1c (a): the declaration-aware entry carries the IDOK fold out "
                           "(the edited flag plus the folded set, straight from the dialog state)");
                TEST_CHECK(osw_src.find("declaration.edited = false;") != std::string::npos &&
                               osw_src.find("declaration.declared_languages.clear();") !=
                                   std::string::npos,
                           "P9c-1c (a): the entry clears the out-struct first, so Cancel and [삭제] "
                           "report 'nothing to adopt' instead of an empty set");
                TEST_CHECK(osw_src.find("ShowOpenAiSettingsDialogEx(parent, cfg) == OpenAiSettingsOutcome::kSaved") !=
                                   std::string::npos &&
                               osw_src.find("return ShowOpenAiSettingsDialogEx(parent, cfg, declaration);") !=
                                   std::string::npos,
                           "P9c-1c (a): the outcome-only overloads stay intact (the REQ-051 bool "
                           "wrapper keeps its exact contract and forwards through the new entry)");
            }
            if (main_ok) {
                const std::size_t apply_at = main_src.find("g_apply_openai_settings = [&]");
                const std::size_t apply_end =
                    main_src.find("g_apply_openai_deleted = [&]", apply_at);
                const bool apply_ok = apply_at != std::string::npos &&
                                      apply_end != std::string::npos && apply_end > apply_at;
                TEST_CHECK(apply_ok, "P9c-1c (a): the apply lambda is locatable");
                if (apply_ok) {
                    const std::string body = main_src.substr(apply_at, apply_end - apply_at);
                    const std::size_t verb_at =
                        body.find("config.SetOpenAiDeclaredLanguages(*declared);");
                    const std::size_t save_at = body.find("config.SaveToFile();");
                    TEST_CHECK(verb_at != std::string::npos && save_at != std::string::npos &&
                                   verb_at < save_at,
                               "P9c-1c (a): g_apply_openai_settings applies the folded declaration on "
                               "the LIVE config BEFORE its save (so the emitted config.json follows "
                               "the dialog and no later in-memory save can revert it)");
                    TEST_CHECK(body.find("if (declared) {") != std::string::npos,
                               "P9c-1c (a): the apply is gated on the fold pointer - a Cancel call "
                               "(nullptr) never touches a live declaration");
                }
                TEST_CHECK(count_of_str(main_src, "SetOpenAiDeclaredLanguages(") == 1,
                           "P9c-1c (a): exactly ONE declaration apply call site exists in main.cpp "
                           "(the single apply path)");
                TEST_CHECK(main_src.find(
                               "declaration.edited ? &declaration.declared_languages : nullptr") !=
                                   std::string::npos &&
                               main_src.find(
                                   "/*parent=*/emebalachat::g_hControllerWnd, edited, declaration)") !=
                                   std::string::npos,
                           "P9c-1c (a): the WndProc passes the folded set into the seam on the Save "
                           "outcome (and only when the dialog actually folded it)");
                TEST_CHECK(main_src.find(
                               "g_apply_openai_settings(edited, /*saved=*/false, nullptr);") !=
                               std::string::npos,
                           "P9c-1c (a): Cancel passes nullptr - a cancelled dialog never adopts a "
                           "declaration, so the user's live set survives");
            }

            // -- (b) the fold is the set the verb stores ----------------------
            const std::vector<std::string> rows = OpenAiDeclarationRowCodes();
            const auto p9c1c_row_idx = [&rows](const std::string& code) -> int {
                for (std::size_t i = 0; i < rows.size(); ++i) {
                    if (rows[i] == code) return static_cast<int>(i);
                }
                return -1;
            };
            std::vector<int> p9c1c_sel = {p9c1c_row_idx("KO"), p9c1c_row_idx("JA"),
                                          p9c1c_row_idx("EN")};
            std::sort(p9c1c_sel.begin(), p9c1c_sel.end());
            const std::vector<std::string> p9c1c_folded =
                OpenAiDeclarationFromSelection(p9c1c_sel, rows);
            {
                AppConfig fold_cfg;
                const std::vector<std::string> stored =
                    fold_cfg.SetOpenAiDeclaredLanguages(p9c1c_folded);
                TEST_CHECK(!p9c1c_folded.empty() && stored == p9c1c_folded &&
                               fold_cfg.openai_declared_languages == p9c1c_folded &&
                               SanitizeOpenAiDeclaredLanguages(p9c1c_folded) == p9c1c_folded,
                           "P9c-1c (b): the dialog's folded set IS the set the locked verb stores "
                           "(already canonical, so the apply cannot reorder or drop it)");
                // The pre-selection source is the published snapshot, which is
                // always canonical (the verb's output), so the round trip the
                // dialog actually performs is canonical -> rows -> canonical.
                AppConfig preselect_cfg;
                const std::vector<std::string> canonical_form =
                    preselect_cfg.SetOpenAiDeclaredLanguages({"Korean", "ja"});
                const std::vector<std::string> refolded = OpenAiDeclarationFromSelection(
                    OpenAiDeclarationSelection(canonical_form, rows), rows);
                TEST_CHECK(refolded == canonical_form &&
                               canonical_form == std::vector<std::string>({"KO", "JA"}),
                           "P9c-1c (b): the verb's canonical set pre-selects its rows and folds back "
                           "unchanged (declaration -> selection -> declaration is an identity for "
                           "the mixed-spelling input Korean/ja)");
            }

            // -- (c) the emission rule ----------------------------------------
            {
                AppConfig emitted;
                emitted.SetOpenAiDeclaredLanguages({});
                const std::string cleared_json = emitted.ToJsonString();
                TEST_CHECK(cleared_json.find("openai_declared_languages") ==
                               std::string::npos,
                           "P9c-1c (c): a CLEARED declaration emits no key at all (absent = not "
                           "declared = the fail-open full menu), so the clear is visible in the file");
                emitted.SetOpenAiDeclaredLanguages({"Korean", "ja"});
                const std::string armed_json = emitted.ToJsonString();
                TEST_CHECK(armed_json.find("\"openai_declared_languages\": [\"KO\", \"JA\"],") !=
                               std::string::npos,
                           "P9c-1c (c): a declared set emits the canonical array in registry order");
            }

            // -- (d) the end-to-end chain -------------------------------------
            {
                const std::filesystem::path chain_path =
                    std::filesystem::temp_directory_path() /
                    L"emebalachat_p9c1c_chain.json";
                std::error_code chain_ec;
                std::filesystem::remove(chain_path, chain_ec); // stale leftovers
                // `owner` stands in for the single AppConfig main.cpp owns.
                AppConfig owner;
                owner.SetOpenAiDeclaredLanguages({});
                const bool armed_saved = owner.SaveToFile(chain_path);
                std::string armed_text;
                const bool armed_read = read_src(chain_path, armed_text);
                TEST_CHECK(armed_saved && armed_read &&
                               armed_text.find("openai_declared_languages") == std::string::npos,
                           "P9c-1c (d): the undeclared baseline file carries no declaration key");
                // The user ticks the checklist and presses OK: the owning
                // instance applies the fold (the exact call the lambda makes)
                // and then saves.
                owner.SetOpenAiDeclaredLanguages(p9c1c_folded);
                const bool saved = owner.SaveToFile(chain_path);
                std::string text;
                const bool read_ok = read_src(chain_path, text);
                TEST_CHECK(saved && read_ok &&
                               count_of_str(text, "openai_declared_languages") == 1 &&
                               text.find("\"openai_declared_languages\": [\"EN\", \"KO\", \"JA\"]") !=
                                   std::string::npos,
                           "P9c-1c (d): declare -> OK -> owner apply -> save writes the declaration "
                           "into config.json (one key, canonical array)");
                AppConfig reloaded;
                const bool reload_ok = reloaded.LoadFromFile(chain_path);
                std::string view_codes;
                for (const LanguageInfo* row :
                     GetLanguagesForEngine(LanguageEngine::OpenAi, /*include_auto=*/false)) {
                    view_codes += row->code;
                    view_codes += ",";
                }
                TEST_CHECK(reload_ok && reloaded.openai_declared_languages == p9c1c_folded &&
                               view_codes == "EN,KO,JA,",
                           "P9c-1c (d): ... and after a reload the OpenAI view offers exactly the "
                           "declared set (the declaration survives the restart)");
                // The clear direction through the same path.
                owner.SetOpenAiDeclaredLanguages({});
                const bool cleared_saved = owner.SaveToFile(chain_path);
                std::string cleared_text;
                const bool cleared_read = read_src(chain_path, cleared_text);
                TEST_CHECK(cleared_saved && cleared_read &&
                               cleared_text.find("openai_declared_languages") == std::string::npos,
                           "P9c-1c (d): unticking everything and saving removes the key again, so "
                           "the file returns to the pre-P9c shape");
                std::filesystem::remove(chain_path, chain_ec); // our own artifact
            }

            {
                AppConfig restore;
                restore.SetOpenAiDeclaredLanguages(snapshot_before);
                TEST_CHECK(OpenAiLanguagesDeclared() == !snapshot_before.empty(),
                           "P9c-1c: the process-wide declaration snapshot is restored to its "
                           "pre-test state (suite-state hygiene)");
            }
        }

        I18n::SetLocale(p9c1b_initial_locale); // suite-state hygiene
    }
    I18n::SetLocale(req050_initial_locale); // REQ-050: restore pre-test locale

    std::cout << "[DONE] REQ-048 P2 checks: " << (g_test_count - failures_before)
              << ", failures: " << (g_failed_count - failures_before) << std::endl;
}
