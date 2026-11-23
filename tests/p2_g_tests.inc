// ---------------------------------------------------------------------------
// 261122_0002 P2-G (v0.10.10 phased fixes, wave 2 unit P2-G: history +
// editor window + probe dialog).
//
//   Fix 1 / L2-B  translation_history: the in-memory 10-entry ring (newest
//                 first, mutex-guarded, NEVER disk/log).
//   Fix 2 / L2-B  translation_editor_window: pure layout planner pins + the
//                 structural pins on the modeless RichEdit50W dialog.
//   Fix 3 / RISK-04 user_language_manager_dialog PollInFlight: refresh the
//                 rows whenever ANY outcome drained (not only when all
//                 probes finished) + stop the poll timer when nothing is
//                 in flight.
//
// Coverage:
//   (a) TestP2GHistoryRing        - ring semantics: cap, eviction, newest
//                                   first, copy-out isolation. HWND-free
//                                   (the handle is carried but never
//                                   dereferenced).
//   (b) TestP2GEditorLayoutPlan   - the pure DIP planner: exact rects at the
//                                   default window size, containment/order
//                                   invariants at other sizes, constexpr
//                                   usability.
//   (c) TestP2GStructuralPins     - source pins on the three TUs + the
//                                   CMakeLists source-listing pin.
//
// Staged as an .inc next to this runner; registered from main() per the
// end-of-file pattern. The structural pins read the production sources with
// ResolveRepoFile + a CRLF-normalized raw read (the P2-A / P1-U1 precedent);
// every needle is ASCII-only.
// ---------------------------------------------------------------------------

#include "../src/translation_history.hpp"
#include "../src/ui/translation_editor_window.hpp"

namespace {

// Resolve + read a production source for the structural pins (P2-A pattern).
void P2GReadSrc(const wchar_t* rel, std::string& out) {
    out.clear();
    const std::filesystem::path p = ResolveRepoFile(rel);
    if (p.empty()) return;
    std::ifstream in(p, std::ios::binary);
    if (!in) return;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string norm;
    norm.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\r' && i + 1 < out.size() && out[i + 1] == '\n') continue;
        norm.push_back(out[i]);
    }
    out.swap(norm);
}

} // namespace

// ---- (a) the history ring -------------------------------------------------

void TestP2GHistoryRing() {
    std::cout << "[RUN] Testing P2-G L2-B translation history ring..." << std::endl;
    const int failures_before = g_failed_count;

    using translation_history::Entry;
    using translation_history::kMaxHistoryEntries;
    using translation_history::Push;
    using translation_history::Recent;

    // The cap itself is a static pin (a runtime TEST_CHECK would trip C4127
    // on the constant condition); the behavioral cap is exercised below.
    static_assert(translation_history::kMaxHistoryEntries == 10,
                  "L2-B: the ring cap is exactly 10");

    // 11 unique pushes evict everything older (any pre-existing ring content
    // from other suites included), so Recent() must be EXACTLY our 10 newest,
    // newest first.
    for (int i = 0; i < 11; ++i) {
        Entry e;
        e.original = L"p2g-ring-" + std::to_wstring(i);
        e.forward = L"p2g-fwd-" + std::to_wstring(i);
        e.back = L"p2g-back-" + std::to_wstring(i);
        e.src_code = L"KO";
        e.tgt_code = L"EN";
        e.target_hwnd = reinterpret_cast<HWND>(0x1234); // carried, never dereferenced
        Push(std::move(e));
    }
    std::vector<Entry> recent = Recent();
    TEST_CHECK(recent.size() == kMaxHistoryEntries,
               "L2-B: after 11 pushes the ring holds exactly 10 entries");
    if (recent.size() == kMaxHistoryEntries) {
        TEST_CHECK(recent.front().original == L"p2g-ring-10",
                   "L2-B: index 0 is the NEWEST entry");
        TEST_CHECK(recent.back().original == L"p2g-ring-1",
                   "L2-B: the oldest entry (p2g-ring-0) was evicted");
        bool order_ok = true;
        for (size_t i = 0; i + 1 < recent.size(); ++i) {
            const std::wstring a = L"p2g-ring-" + std::to_wstring(10 - i);
            const std::wstring b = L"p2g-ring-" + std::to_wstring(10 - (i + 1));
            if (recent[i].original != a || recent[i + 1].original != b) order_ok = false;
        }
        TEST_CHECK(order_ok, "L2-B: Recent() is strictly newest-first across the ring");
        TEST_CHECK(recent.front().forward == L"p2g-fwd-10" &&
                       recent.front().back == L"p2g-back-10" &&
                       recent.front().src_code == L"KO" &&
                       recent.front().tgt_code == L"EN" &&
                       recent.front().target_hwnd == reinterpret_cast<HWND>(0x1234),
                   "L2-B: every Entry field (forward/back/src/tgt/hwnd) is carried verbatim");
    }

    // Copy-out isolation: mutating the returned vector must not touch the ring.
    if (!recent.empty()) {
        recent.front().original = L"p2g-MUTATED";
        recent.front().forward = L"p2g-MUTATED";
        const std::vector<Entry> again = Recent();
        TEST_CHECK(!again.empty() && again.front().original == L"p2g-ring-10",
                   "L2-B: Recent() returns a detached copy (mutation cannot reach the ring)");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-G history ring passed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-G history ring: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (b) the editor layout plan -------------------------------------------

void TestP2GEditorLayoutPlan() {
    std::cout << "[RUN] Testing P2-G L2-B editor layout plan..." << std::endl;
    const int failures_before = g_failed_count;

    using translation_editor_window::EditorLayoutPlan;
    using translation_editor_window::kEditorActionButtonWidthDip;
    using translation_editor_window::kEditorButtonHeightDip;
    using translation_editor_window::kEditorClientHeightDip;
    using translation_editor_window::kEditorClientWidthDip;
    using translation_editor_window::kEditorCloseButtonWidthDip;
    using translation_editor_window::kEditorGapDip;
    using translation_editor_window::kEditorMarginDip;
    using translation_editor_window::PlanEditorLayout;

    // The planner must be constexpr-usable (it feeds static layout audits).
    constexpr EditorLayoutPlan kPinned = PlanEditorLayout(560, 440);
    static_assert(kPinned.source_pane.top == kEditorMarginDip,
                  "P2-G plan: the source pane starts at the top margin");
    static_assert(kPinned.close_button.right == 560 - kEditorMarginDip,
                  "P2-G plan: the close button ends at the right margin");

    // Exact geometry at the default window size (560x440 DIP):
    //   panes:  margin..client-margin wide, three stacked bands 10..396 with
    //           two 8-DIP gaps (pane height 123, back absorbs the remainder);
    //   buttons: 26 tall at y=404, right-aligned close/apply/retranslate.
    const EditorLayoutPlan p = PlanEditorLayout(kEditorClientWidthDip, kEditorClientHeightDip);
    TEST_CHECK(p.source_pane.left == 10 && p.source_pane.top == 10 &&
                   p.source_pane.right == 550 && p.source_pane.bottom == 133,
               "L2-B plan: the source pane rect is the pinned default-geometry band");
    TEST_CHECK(p.target_pane.left == 10 && p.target_pane.top == 141 &&
                   p.target_pane.right == 550 && p.target_pane.bottom == 264,
               "L2-B plan: the target pane sits one gap below the source pane");
    TEST_CHECK(p.back_pane.left == 10 && p.back_pane.top == 272 &&
                   p.back_pane.right == 550 && p.back_pane.bottom == 396,
               "L2-B plan: the back pane absorbs the remainder down to the content bottom");
    TEST_CHECK(p.close_button.left == 478 && p.close_button.top == 404 &&
                   p.close_button.right == 550 && p.close_button.bottom == 430,
               "L2-B plan: the close button is bottom-right, 72 DIP wide");
    TEST_CHECK(p.apply_button.left == 320 && p.apply_button.right == 470 &&
                   p.apply_button.top == 404 && p.apply_button.bottom == 430,
               "L2-B plan: the apply button sits one gap left of the close button");
    TEST_CHECK(p.retranslate_button.left == 162 && p.retranslate_button.right == 312 &&
                   p.retranslate_button.top == 404 && p.retranslate_button.bottom == 430,
               "L2-B plan: the retranslate button sits one gap left of the apply button");

    // Containment + ordering invariants at a different size (800x600 DIP).
    const EditorLayoutPlan q = PlanEditorLayout(800, 600);
    auto inside = [](const RECT& r, int w, int h) {
        return r.left >= 0 && r.top >= 0 && r.right <= w && r.bottom <= h;
    };
    TEST_CHECK(inside(q.source_pane, 800, 600) && inside(q.target_pane, 800, 600) &&
                   inside(q.back_pane, 800, 600) && inside(q.retranslate_button, 800, 600) &&
                   inside(q.apply_button, 800, 600) && inside(q.close_button, 800, 600),
               "L2-B plan: every control stays inside the client rect");
    TEST_CHECK(q.source_pane.bottom + kEditorGapDip == q.target_pane.top &&
                   q.target_pane.bottom + kEditorGapDip == q.back_pane.top,
               "L2-B plan: the three panes stack vertically with exactly one gap each");
    TEST_CHECK(q.back_pane.bottom < q.close_button.top,
               "L2-B plan: the pane stack never overlaps the button row");
    TEST_CHECK(q.source_pane.left == kEditorMarginDip &&
                   q.source_pane.right == 800 - kEditorMarginDip,
               "L2-B plan: the panes span the full client width minus the margins");
    TEST_CHECK(q.retranslate_button.right + kEditorGapDip == q.apply_button.left &&
                   q.apply_button.right + kEditorGapDip == q.close_button.left,
               "L2-B plan: the button row is right-aligned with one gap between buttons");
    TEST_CHECK(q.close_button.right == 800 - kEditorMarginDip &&
                   q.close_button.bottom - q.close_button.top == kEditorButtonHeightDip &&
                   q.close_button.right - q.close_button.left == kEditorCloseButtonWidthDip &&
                   q.apply_button.right - q.apply_button.left == kEditorActionButtonWidthDip,
               "L2-B plan: button sizes match the pinned constants");

    // Degenerate input is tolerated (no UB, deterministic output) - the
    // window code never plans a zero-size client, but the pure function must
    // not invert silently on the sane range.
    const EditorLayoutPlan tiny = PlanEditorLayout(200, 120);
    TEST_CHECK(tiny.source_pane.bottom >= tiny.source_pane.top &&
                   tiny.target_pane.bottom >= tiny.target_pane.top &&
                   tiny.back_pane.bottom >= tiny.back_pane.top,
               "L2-B plan: a small-but-sane client keeps non-inverted pane bands");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-G editor layout plan passed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-G editor layout plan: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (c) structural pins --------------------------------------------------

void TestP2GStructuralPins() {
    std::cout << "[RUN] Testing P2-G structural pins..." << std::endl;
    const int failures_before = g_failed_count;

    std::string hist_cpp, editor_cpp, manager_cpp, cmake_txt;
    P2GReadSrc(L"src/translation_history.cpp", hist_cpp);
    P2GReadSrc(L"src/ui/translation_editor_window.cpp", editor_cpp);
    P2GReadSrc(L"src/ui/user_language_manager_dialog.cpp", manager_cpp);
    P2GReadSrc(L"CMakeLists.txt", cmake_txt);

    TEST_CHECK(!hist_cpp.empty(), "P2-G pin: translation_history.cpp resolves");
    TEST_CHECK(!editor_cpp.empty(), "P2-G pin: translation_editor_window.cpp resolves");
    TEST_CHECK(!manager_cpp.empty(), "P2-G pin: user_language_manager_dialog.cpp resolves");
    TEST_CHECK(!cmake_txt.empty(), "P2-G pin: CMakeLists.txt resolves");

    if (!hist_cpp.empty()) {
        // In-memory ONLY ring: front-insert with oldest-eviction, and none of
        // the persistence/log sinks may ever appear in this TU.
        TEST_CHECK(hist_cpp.find("push_front") != std::string::npos,
                   "L2-B pin: the ring inserts at the front (newest first)");
        TEST_CHECK(hist_cpp.find("pop_back") != std::string::npos,
                   "L2-B pin: the ring evicts from the back (oldest out)");
        TEST_CHECK(hist_cpp.find("std::mutex") != std::string::npos,
                   "L2-B pin: the ring is mutex-guarded");
        TEST_CHECK(hist_cpp.find("WriteFile") == std::string::npos &&
                       hist_cpp.find("CreateFile") == std::string::npos &&
                       hist_cpp.find("fopen") == std::string::npos &&
                       hist_cpp.find("ofstream") == std::string::npos,
                   "L2-B pin: no file I/O anywhere in the history TU (in-memory only)");
        TEST_CHECK(hist_cpp.find("DIAG_") == std::string::npos,
                   "L2-B pin: the history TU never logs (content class: user text)");
    }

    if (!editor_cpp.empty()) {
        TEST_CHECK(editor_cpp.find("LoadLibraryW(L\"msftedit.dll\")") != std::string::npos,
                   "L2-B pin: the RichEdit50W backing library loads via LoadLibraryW");
        TEST_CHECK(editor_cpp.find("L\"RichEdit50W\"") != std::string::npos &&
                       editor_cpp.find("L\"EDIT\"") != std::string::npos,
                   "L2-B pin: the multiline EDIT fallback class is present");
        TEST_CHECK(editor_cpp.find("CreateDialogIndirectParamW") != std::string::npos,
                   "L2-B pin: the editor is a MODELESS dialog (not DialogBox*)");
        TEST_CHECK(editor_cpp.find("ES_READONLY") != std::string::npos,
                   "L2-B pin: the back-translation pane is read-only");
        TEST_CHECK(editor_cpp.find("PasteAndRestore(text, backup, target") != std::string::npos,
                   "L2-B pin: apply reuses the win32_input PasteAndRestore primitive exactly");
        TEST_CHECK(editor_cpp.find("BackupClipboard(backup)") != std::string::npos,
                   "L2-B pin: apply snapshots the clipboard before the paste");
        TEST_CHECK(editor_cpp.find("IsSameWindowForInjection") != std::string::npos,
                   "L2-B pin: apply proves the target took the foreground before injecting");
        TEST_CHECK(editor_cpp.find("StringId::TranslationEditorTitle") != std::string::npos,
                   "L2-B pin: the title uses the TranslationEditorTitle id");
        TEST_CHECK(editor_cpp.find("StringId::EditRetranslate") != std::string::npos,
                   "L2-B pin: the retranslate button uses the EditRetranslate id");
        TEST_CHECK(editor_cpp.find("StringId::ApplyToOriginalWindow") != std::string::npos,
                   "L2-B pin: the apply button uses the ApplyToOriginalWindow id");
        TEST_CHECK(editor_cpp.find("PlanEditorLayout(") != std::string::npos,
                   "L2-B pin: the window lays out from the pure DIP planner");
        TEST_CHECK(editor_cpp.find("IsWindow(st.hwnd)") != std::string::npos,
                   "L2-B pin: reopening Open() refreshes the single live instance");
        TEST_CHECK(editor_cpp.find("ui::WindowDpi") != std::string::npos &&
                       editor_cpp.find("ui::ScaleDipsToPixels") != std::string::npos,
                   "L2-B pin: DPI comes from the ui::dpi helpers");
    }

    if (!manager_cpp.empty()) {
        // RISK-04: PollInFlight refreshes on ANY drain and stops when nothing
        // is in flight - the refresh must no longer be gated on
        // "!any_in_flight" (the pre-fix pairing).
        const size_t fn_pos = manager_cpp.find("void PollInFlight(");
        TEST_CHECK(fn_pos != std::string::npos, "RISK-04 pin: PollInFlight is locatable");
        if (fn_pos != std::string::npos) {
            const std::string body = manager_cpp.substr(fn_pos, 1400);
            // NB: the body also carries the early `service == nullptr`
            // StopInFlightPoll, so pair each gate with the occurrence AFTER
            // it (the first StopInFlightPoll is NOT the !any_in_flight one).
            const size_t gate_pos = body.find("if (!any_in_flight)");
            const size_t drained_pos = body.find("if (drained > 0)");
            TEST_CHECK(gate_pos != std::string::npos && drained_pos != std::string::npos,
                       "RISK-04 pin: the poll tick keeps both decision gates");
            if (gate_pos != std::string::npos) {
                const size_t stop_pos = body.find("StopInFlightPoll(dlg, st);", gate_pos);
                TEST_CHECK(stop_pos != std::string::npos,
                           "RISK-04 pin: the !any_in_flight gate still stops the timer");
            }
            if (drained_pos != std::string::npos) {
                const size_t refresh_pos = body.find("RefreshRows(dlg, st);", drained_pos);
                TEST_CHECK(refresh_pos != std::string::npos,
                           "RISK-04 pin: the drained>0 gate refreshes the rows");
            }
            if (gate_pos != std::string::npos && drained_pos != std::string::npos) {
                // The refresh must be decoupled from the all-finished gate:
                // between the two gates there is no StopInFlightPoll AND no
                // RefreshRows (they are sibling decisions, not nested).
                const size_t hi = gate_pos < drained_pos ? drained_pos : gate_pos;
                const size_t lo = gate_pos < drained_pos ? gate_pos : drained_pos;
                const std::string between = body.substr(lo, hi - lo);
                TEST_CHECK(between.find("RefreshRows") == std::string::npos &&
                               between.find("StopInFlightPoll") == std::string::npos,
                           "RISK-04 pin: the two gates are siblings (refresh no longer nested "
                           "in the all-finished stop)");
            }
        }
    }

    if (!cmake_txt.empty()) {
        TEST_CHECK(cmake_txt.find("src/translation_history.cpp") != std::string::npos,
                   "P2-G pin: translation_history.cpp is in the Emebalachat_core sources");
        TEST_CHECK(cmake_txt.find("src/ui/translation_editor_window.cpp") != std::string::npos,
                   "P2-G pin: translation_editor_window.cpp is in the Emebalachat_core sources");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-G structural pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-G structural pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- registration ---------------------------------------------------------

namespace p2_g_tests {

void P2GRegisterTests() {
    TestP2GHistoryRing();
    TestP2GEditorLayoutPlan();
    TestP2GStructuralPins();
}

} // namespace p2_g_tests
