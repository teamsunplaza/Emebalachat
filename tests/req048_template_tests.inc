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
// ---------------------------------------------------------------------------

namespace req048_detail {

struct ItemExpectation {
    WORD id;
    WORD ordinal;
    bool has_title;
    std::wstring title;
    short x, y, cx, cy; // REQ-050: pin the production geometry too
};

// Production constants mirrored from openai_settings_window.cpp (enum starts
// at IDC_STATIC_BASE = 100; system-class ordinals 0x0080..0x0085).
constexpr WORD kStaticCls = 0x0082;
constexpr WORD kEditCls   = 0x0081;
constexpr WORD kBtnCls    = 0x0080;
constexpr WORD kComboCls  = 0x0085;

// REQ-050: geometry matches BuildOpenAiTemplate exactly (262x130 dialog;
// labels w=56; edits x=68 w=186; combo x=68 w=118; fetch x=190 w=64;
// masked label w=240; REQ-051 delete x=8 y=70 w=64 h=13; OK x=110 w=64;
// Cancel x=182 w=64 — all h/y per spec).
const ItemExpectation kExpected[] = {
    {100, kStaticCls, false, L"",   8,   6, 56,  9},  // IDC_STATIC_BASE
    {101, kEditCls,   true,  L"",   68,  5, 186, 12}, // IDC_BASE_URL
    {102, kStaticCls, false, L"",   8,   22, 56,  9}, // IDC_STATIC_KEY
    {103, kEditCls,   true,  L"",   68,  21, 186, 12},// IDC_API_KEY
    {104, kStaticCls, false, L"",   8,   38, 56,  9}, // IDC_STATIC_MODEL
    {105, kComboCls,  true,  L"",   68,  37, 118, 64},// IDC_MODEL_COMBO
    {106, kBtnCls,    false, L"",   190, 37, 64,  12},// IDC_FETCH_BTN (i18n title)
    {107, kStaticCls, true,  L"",   8,   54, 240, 9}, // IDC_MASKED_LABEL
    {108, kBtnCls,    false, L"",   8,   70, 64,  13},// REQ-051 IDC_DELETE_BTN (i18n title)
    {IDOK,     kBtnCls, true, L"OK",     110, 70, 64, 13},
    {IDCANCEL, kBtnCls, true, L"Cancel", 182, 70, 64, 13},
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

} // namespace req048_detail

void TestReq048OpenAiTemplateSerialization() {
    std::cout << "[RUN] Testing REQ-048 P2 OpenAI template serialization..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- (a) Header offset pin -------------------------------------------
    {
        TemplateBuilder tb;
        // REQ-050: probe carries the production geometry (262 x 130).
        tb.Begin(L"probe", 262, 130, /*itemCount=*/7);
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
            TEST_CHECK(req048_detail::ReadWord(base, 16) == 130,
                       "REQ-048 P2 (a): byte offset 16 == cy (130)");
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
        // REQ-050: production dialog is 262 x 130 DLU.
        // REQ-051: 11 items — the IDC_DELETE_BTN button (REQ-051 Symptom D)
        // joined the 10-control REQ-048 P2 template.
        tb.Begin(L"production", 262, 130, /*itemCount=*/11);
        BuildOpenAiTemplate(tb);
        const BYTE* base = reinterpret_cast<const BYTE*>(tb.Get());
        const size_t tpl_base = 0; // buffer origin == template origin for a fresh builder
        TEST_CHECK(base != nullptr, "REQ-048 P2 (b): production template buffer non-null");
        if (!base) return;

        // Header: style, exstyle, cdit, x, y, cx, cy (18 bytes).
        size_t off = 18;
        const WORD cdit = ReadWord(base, 8);
        TEST_CHECK(cdit == 11, "REQ-048 P2 (b): header cdit == 11 (9 controls + OK + Cancel)");
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
            TEST_CHECK(i < 11, "REQ-048 P2 (b): item index within expectation table");
            if (i >= 11) break;
            const ItemExpectation& exp = kExpected[i];
            TEST_CHECK(id == exp.id, "REQ-048 P2 (b): item id matches production constant");
            // REQ-050: geometry pin — x/y/cx/cy must match the production
            // AddItem coordinates exactly (labels w=56, edits x=68 w=186,
            // fetch x=190 w=64, REQ-051 delete x=8 y=70, OK x=110 w=64,
            // Cancel x=182 w=64).
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
    I18n::SetLocale(req050_initial_locale); // REQ-050: restore pre-test locale

    std::cout << "[DONE] REQ-048 P2 checks: " << (g_test_count - failures_before)
              << ", failures: " << (g_failed_count - failures_before) << std::endl;
}
