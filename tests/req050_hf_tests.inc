// ---------------------------------------------------------------------------
// REQ-050 (user feedback items 2-1 + 2-2): the Hugging Face add-dialog follow-up.
//   2-1  URL auto-conversion — NormalizeHfUrl must accept the three shapes a
//        user pastes from huggingface.co (canonical /resolve/, /blob/ link,
//        model page with ?show_file_info=<file>) and normalize each to the
//        canonical resolve form before any network I/O; everything else
//        (wrong host, http, /tree/, undeterminable file, duplicate/extra
//        query params, malformed percent-encoding, path traversal) fails
//        closed with an empty out-parameter.
//   2-2  Layout fix — the user reported the URL edit looked covered
//        ("네모난 칸이 가려져있는 것 같아"): the runtime progress bar was created
//        from raw DLU numbers via CreateWindowExW (which takes PIXELS), so it
//        landed across the edit. The fix pins every template control rect
//        inside the 262x92 client area with >= 6 DLU margins on all sides
//        (byte-walk, the TestReq048R2GgufManager (c2) precedent, extended by
//        the margin/order asserts + the kHfProgressRectDlu runtime rect).
// Staged as an .inc next to this runner; registered in main() right after
// TestReq050HfUrlGate (the canonical-gate matrix this suite complements).
// REQ-052 extends this file with TestReq052HfUiFixes (the HF add-dialog
// defect bundle: registered-flag refresh, progress-decode signedness,
// cancel abort-before-join, 64-bit Content-Length, orphan-file registration,
// localized rename captions) — register it right after TestReq050HfAddDialog.
// ---------------------------------------------------------------------------

#include "../src/ui/gguf_model_manager_window.hpp" // NormalizeHfUrl + BuildHfAddTemplate + kHfProgressRectDlu

void TestReq050HfAddDialog() {
    std::cout << "[RUN] Testing REQ-050 (2-1) HF URL auto-conversion + "
                 "(2-2) HF dialog layout..." << std::endl;
    const int failures_before = g_failed_count;

    // ==================== (2-1) NormalizeHfUrl matrix =======================

    // ---- accept: canonical resolve URL passes through VERBATIM -------------
    {
        std::string out;
        TEST_CHECK(NormalizeHfUrl("https://huggingface.co/a/b/resolve/main/m.gguf", &out) &&
                       out == "https://huggingface.co/a/b/resolve/main/m.gguf",
                   "REQ-050 2-1: canonical resolve URL passes through byte-identical");
        TEST_CHECK(NormalizeHfUrl("https://huggingface.co/user/repo/resolve/v1.0/"
                                  "sub/Model Q4_K_M.gguf", &out) &&
                       out == "https://huggingface.co/user/repo/resolve/v1.0/"
                              "sub/Model Q4_K_M.gguf",
                   "REQ-050 2-1: nested repo + tagged revision resolve URL passes through");
    }

    // ---- accept: /blob/<rev>/<file> -> /resolve/<rev>/<file> ---------------
    {
        std::string out;
        TEST_CHECK(NormalizeHfUrl("https://huggingface.co/a/b/blob/main/m.gguf", &out) &&
                       out == "https://huggingface.co/a/b/resolve/main/m.gguf",
                   "REQ-050 2-1: blob link normalized to resolve/main");
        TEST_CHECK(NormalizeHfUrl("https://huggingface.co/user/repo/blob/v1.0/"
                                  "sub/dir/m.gguf", &out) &&
                       out == "https://huggingface.co/user/repo/resolve/v1.0/"
                              "sub/dir/m.gguf",
                   "REQ-050 2-1: blob link with tagged revision + nested file normalized");
        TEST_CHECK(NormalizeHfUrl("https://huggingface.co/a/blob/main/m.gguf", &out) &&
                       out == "https://huggingface.co/a/resolve/main/m.gguf",
                   "REQ-050 2-1: single-segment repo blob link normalized");
    }

    // ---- accept: model page ?show_file_info=<file> -> /resolve/main/<file> --
    {
        std::string out;
        TEST_CHECK(NormalizeHfUrl("https://huggingface.co/a/b?show_file_info=m.gguf",
                                  &out) &&
                       out == "https://huggingface.co/a/b/resolve/main/m.gguf",
                   "REQ-050 2-1: model page URL with show_file_info normalized to resolve/main");
        TEST_CHECK(NormalizeHfUrl("https://huggingface.co/a?show_file_info=m.gguf",
                                  &out) &&
                       out == "https://huggingface.co/a/resolve/main/m.gguf",
                   "REQ-050 2-1: single-segment repo page URL normalized");
        // Percent-decoding of the file parameter.
        TEST_CHECK(NormalizeHfUrl(
                       "https://huggingface.co/a/b?show_file_info=sub%2Fdir%2Fm.gguf",
                       &out) &&
                       out == "https://huggingface.co/a/b/resolve/main/sub/dir/m.gguf",
                   "REQ-050 2-1: show_file_info percent-decoded (%2F -> subdir)");
        TEST_CHECK(NormalizeHfUrl(
                       "https://huggingface.co/a/b?show_file_info=Model%20Q4_K_M.gguf",
                       &out) &&
                       out == "https://huggingface.co/a/b/resolve/main/Model Q4_K_M.gguf",
                   "REQ-050 2-1: show_file_info percent-decoded (%20 -> space)");
        TEST_CHECK(NormalizeHfUrl(
                       "https://huggingface.co/a/b?show_file_info=%EB%AA%A8%EB%8D%B8.gguf",
                       &out) &&
                       out == "https://huggingface.co/a/b/resolve/main/\xEB\xAA\xA8\xEB\x8D\xB8.gguf",
                   "REQ-050 2-1: show_file_info percent-decoded UTF-8 filename preserved");
    }

    // ---- reject: every hostile/ambiguous shape fails closed ----------------
    {
        const char* kReject[] = {
            // transport/host pins (unchanged from the canonical gate)
            "http://huggingface.co/a/b/resolve/main/m.gguf",
            "https://evil.com/huggingface.co/a/b/resolve/main/m.gguf",
            "https://huggingface.co.evil.com/a/b/blob/main/m.gguf",
            "https://HuggingFace.co/a/b?show_file_info=m.gguf",
            // /tree/ browser paths
            "https://huggingface.co/a/b/tree/main/m.gguf",
            "https://huggingface.co/a/b/tree/main?show_file_info=m.gguf",
            // page URL whose file cannot be determined
            "https://huggingface.co/a/b",
            "https://huggingface.co/a/b?",
            "https://huggingface.co/a/b?foo=bar",
            "https://huggingface.co/a/b?show_file_info",      // bare key, no '='
            "https://huggingface.co/a/b?show_file_info=",     // empty value
            // query-parameter discipline
            "https://huggingface.co/a/b?show_file_info=m.gguf&foo=bar",
            "https://huggingface.co/a/b?foo=bar&show_file_info=m.gguf",
            "https://huggingface.co/a/b?show_file_info=a.gguf&show_file_info=b.gguf",
            // malformed percent-encoding
            "https://huggingface.co/a/b?show_file_info=%2",
            "https://huggingface.co/a/b?show_file_info=%zz",
            // path traversal, raw and encoded
            "https://huggingface.co/a/b?show_file_info=../m.gguf",
            "https://huggingface.co/a/b?show_file_info=..%2Fm.gguf",
            "https://huggingface.co/a/b?show_file_info=%2E%2E%2Fm.gguf",
            "https://huggingface.co/a/b?show_file_info=%2e%2e",
            "https://huggingface.co/a/b?show_file_info=.%2Fm.gguf",
            "https://huggingface.co/a/b?show_file_info=%2Fm.gguf",   // leading slash
            // ambiguous/invalid raw bytes in the value
            "https://huggingface.co/a/b?show_file_info=m+o.gguf",    // '+' space ambiguity
            "https://huggingface.co/a/b?show_file_info=m o.gguf",    // raw space
            "https://huggingface.co/a/b?show_file_info=m%5Co.gguf",  // decoded backslash
            // resolve URLs keep rejecting query/fragment tricks
            "https://huggingface.co/a/b/resolve/main/m.gguf?download=true",
            "https://huggingface.co/a/b/resolve/main/m.gguf#frag",
            "https://huggingface.co/a/b?show_file_info=m.gguf#frag",
            // blob shapes that cannot determine a file
            "https://huggingface.co/a/b/blob/main",
            "https://huggingface.co/a/b/blob//m.gguf",
            "https://huggingface.co/a/b/blob/main/",
            "https://huggingface.co/a/b/blob/main/../m.gguf",
            "https://huggingface.co/a/b/blob/main/m.gguf?x=y",       // query on a blob URL
            "https://huggingface.co/a/tree/b/blob/main/m.gguf",      // reserved marker in repo
            // page path carrying a reserved marker
            "https://huggingface.co/a/b/blob/main/m.gguf?show_file_info=x.gguf",
            "https://huggingface.co/a/b/resolve/main/m.gguf?show_file_info=x.gguf",
            // canonical-gate leftovers
            "https://huggingface.co/a/b/resolve//m.gguf",
            "https://huggingface.co/a/b/resolve/main/../m.gguf",
            "https://huggingface.co//resolve/main/m.gguf",
            "https://huggingface.co/",
            "",
        };
        for (const char* u : kReject) {
            std::string out = "sentinel";
            const bool ok = NormalizeHfUrl(u, &out);
            TEST_CHECK(!ok && out.empty(),
                       "REQ-050 2-1: rejected shape fails closed with an empty out");
            if (ok || !out.empty()) {
                std::cout << "      (offending URL: " << u << ")" << std::endl;
            }
        }
        // Null out-parameter is a safe no-op false (defensive contract).
        TEST_CHECK(!NormalizeHfUrl("https://huggingface.co/a/b?show_file_info=m.gguf",
                                   nullptr),
                   "REQ-050 2-1: null out-parameter rejected without writing");
    }

    // ---- normalize -> filename derivation is the dialog-facing contract ----
    {
        std::string out, name;
        TEST_CHECK(NormalizeHfUrl("https://huggingface.co/a/b?show_file_info=m.gguf",
                                  &out) &&
                       HfResolveFilename(out, &name) && name == "m.gguf",
                   "REQ-050 2-1: normalized page URL derives the show_file_info filename");
        TEST_CHECK(NormalizeHfUrl("https://huggingface.co/a/b/blob/v1/m.gguf", &out) &&
                       HfResolveFilename(out, &name) && name == "m.gguf",
                   "REQ-050 2-1: normalized blob URL derives the last-segment filename");
        // Shape validation and filename validation stay SEPARATE gates: a
        // percent-encoded Windows-invalid character normalizes (valid URL
        // shape) but HfResolveFilename refuses it, so the dialog still shows
        // hf_invalid_url before any network I/O.
        TEST_CHECK(NormalizeHfUrl(
                       "https://huggingface.co/a/b?show_file_info=m%3Co%3E.gguf", &out) &&
                       out == "https://huggingface.co/a/b/resolve/main/m<o>.gguf",
                   "REQ-050 2-1: Windows-invalid chars still normalize (URL shape is valid)");
        TEST_CHECK(!HfResolveFilename(out, &name),
                   "REQ-050 2-1: filename gate refuses the decoded Windows-invalid chars");
    }

    // ==================== (2-2) template layout byte-walk ===================
    const UiLocale hf_initial_locale = I18n::GetCurrentLocale();
    I18n::SetLocale(UiLocale::English); // DialogOk/Cancel reference
    {
        using namespace req048_detail;
        constexpr short kDlgW = 262, kDlgH = 92; // production client size (DLU)
        TemplateBuilder tb;
        tb.Begin(L"probe", kDlgW, kDlgH, /*itemCount=*/5);
        BuildHfAddTemplate(tb);
        const auto* dt = tb.Get();
        TEST_CHECK(dt != nullptr && dt->cdit == 5,
                   "REQ-050 2-2: HF add template emits 5 items (URL label + URL edit "
                   "+ status + Download(IDOK) + Cancel(IDCANCEL))");
        TEST_CHECK(dt != nullptr && dt->cx == kDlgW && dt->cy == kDlgH,
                   "REQ-050 2-2: production dialog is 262x92 DLU");
        const BYTE* base = reinterpret_cast<const BYTE*>(tb.Get());
        if (base) {
            const WORD kHfStaticCls = 0x0082;
            const WORD kHfEditCls = 0x0081;
            const WORD kHfBtnCls = 0x0080;
            struct Exp { WORD id; WORD cls; short x, y, cx, cy; std::wstring title; };
            const Exp kExp[] = {
                {4300, kHfStaticCls, 8,   6,  140, 9,
                 I18n::Get(StringId::HfUrlLabel)},
                {4301, kHfEditCls,   8,   17, 246, 12, L""},
                {4302, kHfStaticCls, 8,   52, 246, 9,  L""},
                {IDOK, kHfBtnCls,    118, 68, 64,  13,
                 I18n::Get(StringId::DialogOk)},
                {IDCANCEL, kHfBtnCls, 190, 68, 64, 13,
                 I18n::Get(StringId::DialogCancel)},
            };
            size_t off = 18 + 2 + 2;
            off += 2 * (1 + std::wstring(L"probe").size());
            off += 2;
            off += 2 * (1 + std::wstring(L"Segoe UI").size());
            bool walk_ok = true;
            bool margins_ok = true;
            bool scroll_ok = true;
            short prev_top = 0;
            for (WORD iItem = 0; iItem < 5; ++iItem) {
                off += (4 - off % 4) % 4;
                const Exp& e = kExp[iItem];
                // REQ-050 (single-line edit limits): the URL EDIT (4301) must
                // carry ES_AUTOHSCROLL (0x0080) — without it a single-line
                // EDIT rejects input beyond its visible width (device-
                // confirmed: a 110-char URL truncated at ~76 on paste).
                // EDT style DWORD = 0x50810000 (+0x0080 -> 0x50810080).
                if (e.id == 4301) {
                    scroll_ok = ReadDword(base, off) == 0x50810080u;
                }
                const short x = static_cast<short>(ReadWord(base, off + 8));
                const short y = static_cast<short>(ReadWord(base, off + 10));
                const short cx = static_cast<short>(ReadWord(base, off + 12));
                const short cy = static_cast<short>(ReadWord(base, off + 14));
                if (ReadWord(base, off + 16) != e.id ||
                    ReadWord(base, off + 20) != e.cls ||
                    x != e.x || y != e.y || cx != e.cx || cy != e.cy) {
                    walk_ok = false;
                    break;
                }
                size_t toff = off + 22;
                if (ReadString(base, &toff) != e.title) {
                    walk_ok = false;
                    break;
                }
                // REQ-050 (2-2): >= 6 DLU margins on ALL sides of the client rect.
                if (x < 6 || y < 6 ||
                    static_cast<int>(x) + cx > kDlgW - 6 ||
                    static_cast<int>(y) + cy > kDlgH - 6) {
                    margins_ok = false;
                }
                // y-order must never invert (label, edit, progress, status,
                // button row — same-row controls share a top, so compare tops).
                if (iItem > 0 && y < prev_top) {
                    margins_ok = false;
                }
                prev_top = y;
                off = toff + 2;
            }
            TEST_CHECK(walk_ok,
                       "REQ-050 2-2: HF template items 1:1 — label(8,6,140,9) "
                       "edit(8,17,246,12) status(8,52,246,9) buttons(118/190,68,64,13)");
            TEST_CHECK(scroll_ok,
                       "REQ-050 2-2: the URL edit carries ES_AUTOHSCROLL (0x50810080) "
                       "so long model URLs paste in full");
            TEST_CHECK(margins_ok,
                       "REQ-050 2-2: every template control sits inside the client rect "
                       "with >= 6 DLU margins on all sides, in y-order");
            TEST_CHECK(off == tb.size(),
                       "REQ-050 2-2: HF template walk consumed exactly size() bytes");

            // The runtime progress bar (not a template item): its DLU rect must
            // honor the same margins and sit between the edit and the status.
            const RECT& pr = kHfProgressRectDlu; // {left, top, right, bottom}
            TEST_CHECK(pr.left >= 6 && pr.top >= 6 &&
                           pr.right <= kDlgW - 6 && pr.bottom <= kDlgH - 6,
                       "REQ-050 2-2: progress-bar DLU rect inside the client rect "
                       "with >= 6 DLU margins");
            TEST_CHECK(pr.top >= 17 + 12 && pr.bottom <= 52,
                       "REQ-050 2-2: progress bar sits between the URL edit (bottom 29) "
                       "and the status line (top 52)");
        }
        I18n::SetLocale(hf_initial_locale); // suite-state hygiene
    }

    // ---- structural pins: the fix is wired in the production source ---------
    const std::filesystem::path hf_cpp =
        ResolveRepoFile(L"src\\ui\\gguf_model_manager_window.cpp");
    if (hf_cpp.empty()) {
        std::cout << "[SKIP] src/ui/gguf_model_manager_window.cpp not resolvable; "
                     "REQ-050 2-2 source pins skipped." << std::endl;
    } else {
        std::ifstream in(hf_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        // 2-2: the progress bar is mapped DLU -> pixels before creation (the
        // raw-DLU-as-pixel bug that covered the URL edit).
        TEST_CHECK(src.find("MapDialogRect") != std::string::npos &&
                       src.find("kHfProgressRectDlu") != std::string::npos,
                   "REQ-050 2-2: the progress bar is created through MapDialogRect "
                   "from kHfProgressRectDlu");
        // 2-2 (edit limits): the URL edit passes EDT | ES_AUTOHSCROLL — a
        // single-line EDIT without it rejects input beyond its visible width
        // (device-confirmed: a 110-char URL truncated at ~76 on paste).
        TEST_CHECK(src.find("EDT | ES_AUTOHSCROLL") != std::string::npos,
                   "REQ-050 2-2: the HF URL edit passes EDT | ES_AUTOHSCROLL");
        TEST_CHECK(src.find("tb.Begin(title, 262, 92") != std::string::npos,
                   "REQ-050 2-2: ShowHfAddDialog builds the 262x92 template");
        // 2-1: the download path normalizes before WinHTTP.
        TEST_CHECK(src.find("NormalizeHfUrl") != std::string::npos &&
                       src.find("st->worker_url = ToUtf16(resolve_url)") !=
                           std::string::npos,
                   "REQ-050 2-1: the IDOK path normalizes the pasted URL and hands "
                   "the canonical form to the download worker");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-050 (2-1/2-2) HF add-dialog checks passed."
                  << std::endl;
    } else {
        std::cout << "[FAIL] REQ-050 (2-1/2-2) HF add-dialog checks: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}

// ---------------------------------------------------------------------------
// REQ-052: the HF model-manager/downloader defect bundle —
//   1 manager list not refreshed after a successful fresh HF add
//     (HfRegisterDownloaded's fresh path now flips st->registered like both
//     reuse paths; the caller gates the reload on it)
//   2 '(65535%)' signedness — HfDecodeProgressPercent((WORD)-1) == -1 so the
//     marquee branch runs again (the pure helper lives in the window hpp)
//   3 cancel freeze — the worker publishes its connect/request handles and
//     IDCANCEL WinHttpCloseHandles them BEFORE joining (the documented
//     cross-thread abort), instead of waiting out the 60s receive timeout
//   4 4GB+ Content-Length — the header is queried as TEXT and parsed to a
//     64-bit value (no WINHTTP_QUERY_FLAG_NUMBER on it anymore), percent
//     math stays 64-bit with the divide-by-zero guard
//   5 orphan-file re-download deadlock — an unregistered file on disk is
//     registered through the same origin:"user" path a completed download
//     takes (shared HfDeriveFreshIdentity) with an honest HfDone status,
//     instead of a bare HfFailed refusal
//   6 rename-dialog captions — the hardcoded L"OK"/L"Cancel" became the
//     generic DialogOk/DialogCancel pair (no new i18n strings)
// ---------------------------------------------------------------------------
void TestReq052HfUiFixes() {
    std::cout << "[RUN] Testing REQ-052 HF add-dialog defect fixes..." << std::endl;
    const int failures_before = g_failed_count;

    // ---- (2) pure: the progress-percent decode helper (header-inline) -------
    TEST_CHECK(HfDecodeProgressPercent(static_cast<WPARAM>(static_cast<WORD>(-1))) == -1,
               "REQ-052: (WORD)-1 marquee sentinel decodes to -1 (not 65535)");
    TEST_CHECK(HfDecodeProgressPercent(0) == 0,
               "REQ-052: 0% decodes to 0");
    TEST_CHECK(HfDecodeProgressPercent(50) == 50,
               "REQ-052: 50% decodes to 50");
    TEST_CHECK(HfDecodeProgressPercent(100) == 100,
               "REQ-052: 100% decodes to 100");
    // The wire value 65535 IS the sentinel — the pre-fix cast decoded it to
    // +65535 and the status read "(65535%)".
    TEST_CHECK(HfDecodeProgressPercent(static_cast<WPARAM>(65535)) == -1,
               "REQ-052: the wire value 65535 decodes to -1 (marquee)");

    // ---- (1/3/4/5/6) structural pins on the production source ---------------
    const std::filesystem::path hf_req052_cpp =
        ResolveRepoFile(L"src\\ui\\gguf_model_manager_window.cpp");
    if (hf_req052_cpp.empty()) {
        std::cout << "[SKIP] src/ui/gguf_model_manager_window.cpp not resolvable; "
                     "REQ-052 source pins skipped." << std::endl;
    } else {
        std::ifstream in(hf_req052_cpp, std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        const auto count_occ = [&src](const std::string& needle) {
            size_t n = 0;
            size_t pos = 0;
            while ((pos = src.find(needle, pos)) != std::string::npos) {
                ++n;
                pos += needle.size();
            }
            return n;
        };

        // (1) every success path flips `registered` — both reuse paths, the
        // orphan-file path AND the fresh path (the pre-fix fresh path forgot,
        // so the caller's manager-list reload gate never ran).
        TEST_CHECK(count_occ("st->registered = true") >= 3,
                   "REQ-052: st->registered = true on every registration path "
                   "(reuse x2 + orphan + fresh)");
        // (2) the handler decodes through the helper, not a raw WORD->int cast.
        TEST_CHECK(src.find("const int percent = HfDecodeProgressPercent(wp);") !=
                       std::string::npos &&
                       src.find("static_cast<int>(static_cast<WORD>(wp))") ==
                           std::string::npos,
                   "REQ-052: kHfMsgProgress decodes wParam through "
                   "HfDecodeProgressPercent (the raw WORD->int cast is gone)");
        // (3) the worker publishes the request handle so the cancel path has
        // something to close.
        TEST_CHECK(src.find("st->hf_request = request.get();") != std::string::npos &&
                       src.find("st->hf_connect = connect.get();") != std::string::npos,
                   "REQ-052: the worker publishes its connect/request handles "
                   "into the dialog state");
        // (3) cancel: close BEFORE join (source order inside the IDCANCEL arm —
        // the cancel flag store is the only st->cancel.store(true) in the file).
        {
            const size_t pos_cancel = src.find("st->cancel.store(true)");
            const size_t pos_close = src.find("WinHttpCloseHandle(abort_request)");
            const size_t pos_join =
                pos_cancel == std::string::npos
                    ? std::string::npos
                    : src.find("HfJoinWorker(st);", pos_cancel);
            TEST_CHECK(pos_cancel != std::string::npos &&
                           pos_close != std::string::npos &&
                           pos_join != std::string::npos &&
                           pos_cancel < pos_close && pos_close < pos_join,
                       "REQ-052: IDCANCEL closes the published handles before "
                       "joining the worker (abort, not a 60s freeze)");
        }
        // (4) 64-bit Content-Length: no FLAG_NUMBER on the content-length
        // query (the status-code query legitimately keeps its own), and the
        // strict decimal text parse to unsigned long long is present.
        TEST_CHECK(src.find("WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER") ==
                       std::string::npos,
                   "REQ-052: Content-Length is no longer queried with "
                   "WINHTTP_QUERY_FLAG_NUMBER (the DWORD 4GB overflow)");
        TEST_CHECK(src.find("WINHTTP_QUERY_CONTENT_LENGTH,") != std::string::npos &&
                       src.find("unsigned long long parsed") != std::string::npos &&
                       src.find("parsed * 10ull") != std::string::npos,
                   "REQ-052: Content-Length is queried as text and parsed to a "
                   "64-bit value");
        // (5) orphan file: registered through the shared identity helper with
        // an honest HfDone status (HfDone is also the done-handler notice, so
        // >= 2 uses).
        TEST_CHECK(src.find("orphan file registered") != std::string::npos &&
                       count_occ("HfDeriveFreshIdentity(") >= 3,
                   "REQ-052: the orphan-file path registers the existing file "
                   "through the shared HfDeriveFreshIdentity helper");
        TEST_CHECK(count_occ("I18n::Get(StringId::HfDone)") >= 2,
                   "REQ-052: the orphan path surfaces the honest HfDone status "
                   "(reused string, no new i18n field)");
        // (6) rename captions localized through the generic pair.
        TEST_CHECK(src.find("L\"OK\"") == std::string::npos &&
                       src.find("L\"Cancel\"") == std::string::npos,
                   "REQ-052: the rename dialog no longer hardcodes L\"OK\"/L\"Cancel\"");
        TEST_CHECK(count_occ("I18n::Get(StringId::DialogOk)") >= 2 &&
                       count_occ("I18n::Get(StringId::DialogCancel)") >= 2,
                   "REQ-052: rename OK/Cancel reuse the generic DialogOk/"
                   "DialogCancel pair (HF dialog + rename dialog)");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] REQ-052 HF add-dialog defect fixes passed."
                  << std::endl;
    } else {
        std::cout << "[FAIL] REQ-052 HF add-dialog defect fixes: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}
