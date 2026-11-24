// ---------------------------------------------------------------------------
// V10F (v0.10.10 phased fixes, unit P3-C4): GGUF manager view/refresh, the HF
// progress-bar mode, four manager/HF defects, the truthful strict-local
// failure status, and the Arabic-script letter-only predicate.
//
// Covered:
//   (a) G1  RefreshManagerView — the add-file / add-HF / delete mutations run
//       the duplicate-warning pass too, while the policy / rename / activate
//       paths stay rebuild-only (no duplicate-scan disk stats).
//   (b) G2  IDC_HF_PROGRESS — the first determinate percent clears the
//       PBS_MARQUEE style once (Get/SetWindowLongPtrW), tracked by the dialog
//       state's marquee_active flag.
//   (c) G3-1 HfPreFlight / HfRegisterDownloaded — the reuse compare folds
//       ASCII case (ManagerFileNameMatches), so a differently-cased spelling of
//       an already-registered store file is a REUSE, never a re-download.
//   (d) G3-2 IDC_MGR_DELETE — the engine reset is gated on the live user_gguf
//       route (config.engine_type), so deleting a stale registration cannot
//       re-route a running google/openai engine to "auto".
//   (e) G3-3 IDC_MGR_POLICY — the toggled row is re-selected through the shared
//       SelectManagerRowById helper (also used by rename and double-click).
//   (f) G3-4 the HF download worker percent-encodes the request target
//       (HfPercentEncodeRequestPath) at the WinHttpOpenRequestW site only, so
//       NormalizeHfUrl / IsHfResolveUrl keep their decoded form.
//   (g) E2  StrictLocalFailureStatus — "model_missing" maps to
//       LocalModelMissing, every other local-seam code keeps
//       CloudConsentBlocked.
//   (h) FUP-09 IsArabicCodePoint — letters and marks only (the block's digits
//       and punctuation no longer claim Arabic script), mirroring bidi_utils'
//       kDigitsAN exclusion.
//   (i) Pin survival — the pre-existing pins that anchor the edited regions
//       still hold (REQ-053 sync-count / REQ-052 progress decode / REQ-051
//       strict-local CloudConsentBlocked convergence / p1_u4 E1 cancel gates /
//       WS1+V107 retired-shape bans).
//
// Source pins read through ResolveRepoFile + a CRLF-normalized find() (the
// p2_e / req052 precedent; every needle is ASCII-only).
// ---------------------------------------------------------------------------

#include <fstream>
#include <string>

#include "../src/bidi_utils.hpp"                // FUP-09 mirror: GuessBaseDirection
#include "../src/engine.hpp"                    // E2: TranslationStatus / StrictLocalFailureStatus
#include "../src/smart_bypass.hpp"              // FUP-09: ContainsArabic / DetectLanguage
#include "../src/ui/gguf_model_manager_window.hpp" // G1-G3: pure helpers + templates

namespace p3_c4_tests {

// Resolve + read a production source, normalized to LF (needles are ASCII).
void P3C4ReadSrc(const wchar_t* rel, std::string& out) {
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

size_t P3C4Count(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    size_t n = 0;
    size_t pos = hay.find(needle);
    while (pos != std::string::npos) {
        ++n;
        pos = hay.find(needle, pos + needle.size());
    }
    return n;
}

// Slice [from_marker, next_marker) — empty when either end is missing.
std::string P3C4Slice(const std::string& src, const char* from_marker,
                      const char* to_marker) {
    const size_t a = src.find(from_marker);
    if (a == std::string::npos) return std::string();
    const size_t b = src.find(to_marker, a + 1);
    return src.substr(a, (b == std::string::npos ? src.size() : b) - a);
}

// ---- (a) G1: the one post-mutation view refresh ---------------------------

void TestP3C4RefreshManagerView() {
    std::cout << "[RUN] Testing P3-C4 G1 refresh-after-mutation wiring (V10F)..." << std::endl;
    const int failures_before = g_failed_count;

    std::string mgr;
    P3C4ReadSrc(L"src\\ui\\gguf_model_manager_window.cpp", mgr);
    if (mgr.empty()) {
        std::cout << "[SKIP] gguf_model_manager_window.cpp not resolvable; "
                     "P3-C4 G1 pins skipped." << std::endl;
        return;
    }

    // The helper exists and is exactly the rebuild + duplicate-warning pair.
    const std::string helper =
        P3C4Slice(mgr, "void RefreshManagerView(HWND dlg, ManagerDialogState* st) {",
                  "void RunDuplicateCleanup(HWND dlg, ManagerDialogState* st) {");
    TEST_CHECK(!helper.empty(),
               "P3-C4 G1: RefreshManagerView exists in the manager TU");
    if (!helper.empty()) {
        const size_t rebuild = helper.find("RebuildManagerList(dlg, st);");
        const size_t warn = helper.find("ApplyDuplicateWarning(dlg, st);");
        TEST_CHECK(rebuild != std::string::npos && warn != std::string::npos &&
                       rebuild < warn,
                   "P3-C4 G1: the helper rebuilds the list, then re-runs the "
                   "duplicate warning");
        TEST_CHECK(P3C4Count(helper, "RebuildManagerList(dlg, st);") == 1 &&
                       P3C4Count(helper, "ApplyDuplicateWarning(dlg, st);") == 1,
                   "P3-C4 G1: the helper calls each exactly once");
    }

    // Four call sites: the cleanup + the three registry MUTATIONS.
    TEST_CHECK(P3C4Count(mgr, "RefreshManagerView(dlg, st);") == 4,
               "P3-C4 G1: exactly four refresh call sites (cleanup + add-file + "
               "add-HF + delete)");

    // Each mutation handler refreshes AFTER its registry mutation commits. The
    // add flows reload the store from disk (an external registration may have
    // written it); delete mutates the in-memory registry directly (candidate +
    // atomic write), so its commit anchor is the assignment, not a reload.
    struct HandlerCase {
        const char* from;
        const char* to;
        const char* commit;
        const char* label;
    };
    const HandlerCase handlers[] = {
        {"case IDC_MGR_ADD_FILE: {", "case IDC_MGR_ADD_HF: {",
         "LoadManagerRegistry(dlg, st)", "add-file"},
        {"case IDC_MGR_ADD_HF: {", "case IDC_MGR_LIST:",
         "LoadManagerRegistry(dlg, st)", "add-HF"},
        {"case IDC_MGR_DELETE: {", "case IDC_MGR_POLICY: {",
         "st->registry = std::move(candidate);", "delete"},
    };
    for (const HandlerCase& h : handlers) {
        const std::string body = P3C4Slice(mgr, h.from, h.to);
        const size_t commit = body.find(h.commit);
        const size_t refresh = body.find("RefreshManagerView(dlg, st);");
        TEST_CHECK(!body.empty() && commit != std::string::npos &&
                       refresh != std::string::npos && commit < refresh,
                   std::string("P3-C4 G1: the ") + h.label +
                       " handler commits the registry mutation, then refreshes "
                       "the view");
    }

    // Policy-only paths stay disk-stat-free: rebuild only, no duplicate scan.
    const std::string policy = P3C4Slice(mgr, "case IDC_MGR_POLICY: {",
                                        "case IDC_MGR_DEDUPE: {");
    TEST_CHECK(!policy.empty() &&
                   policy.find("RebuildManagerList(dlg, st);") != std::string::npos &&
                   policy.find("ApplyDuplicateWarning(") == std::string::npos &&
                   policy.find("RefreshManagerView(") == std::string::npos,
               "P3-C4 G1: the policy toggle rebuilds without the duplicate scan "
               "(disk-stat-free)");
    const std::string rename_body = P3C4Slice(mgr, "case IDC_MGR_RENAME: {",
                                              "case IDC_MGR_DELETE: {");
    TEST_CHECK(!rename_body.empty() &&
                   rename_body.find("ApplyDuplicateWarning(") == std::string::npos &&
                   rename_body.find("RefreshManagerView(") == std::string::npos,
               "P3-C4 G1: the rename path stays rebuild-only (no stored file set "
               "change)");

    // RunDuplicateCleanup folds its two calls into the one helper.
    const std::string cleanup =
        P3C4Slice(mgr, "void RunDuplicateCleanup(HWND dlg, ManagerDialogState* st) {",
                  "INT_PTR CALLBACK RenameModelProc(");
    TEST_CHECK(!cleanup.empty() &&
                   cleanup.find("RefreshManagerView(dlg, st);") != std::string::npos &&
                   cleanup.find("RebuildManagerList(dlg, st);") == std::string::npos,
               "P3-C4 G1: the cleanup refreshes through the shared helper");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C4 G1 refresh-after-mutation passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C4 G1 refresh-after-mutation: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}

// ---- (b) G2: the marquee -> determinate style transition -----------------

void TestP3C4MarqueeStyleToggle() {
    std::cout << "[RUN] Testing P3-C4 G2 progress-bar marquee style toggle (V10F)..."
              << std::endl;
    const int failures_before = g_failed_count;

    std::string mgr;
    P3C4ReadSrc(L"src\\ui\\gguf_model_manager_window.cpp", mgr);
    if (mgr.empty()) {
        std::cout << "[SKIP] gguf_model_manager_window.cpp not resolvable; "
                     "P3-C4 G2 pins skipped." << std::endl;
        return;
    }

    // State flag + its init.
    TEST_CHECK(mgr.find("bool marquee_active = false;") != std::string::npos,
               "P3-C4 G2: the dialog state tracks the bar mode");
    const std::string init = P3C4Slice(mgr,
                                       "INT_PTR CALLBACK HfAddProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {",
                                       "case kHfMsgProgress: {");
    const size_t init_marquee = init.find("PBM_SETMARQUEE, TRUE, 50");
    const size_t init_flag = init.find("st->marquee_active = true;");
    TEST_CHECK(init_marquee != std::string::npos && init_flag != std::string::npos &&
                   init_marquee < init_flag,
               "P3-C4 G2: WM_INITDIALOG starts the marquee and marks the state");

    const std::string progress = P3C4Slice(mgr, "case kHfMsgProgress: {",
                                           "case kHfMsgDone: {");
    TEST_CHECK(!progress.empty(),
               "P3-C4 G2: the kHfMsgProgress handler is locatable");
    if (!progress.empty()) {
        // The determinate transition clears the STYLE exactly once.
        TEST_CHECK(P3C4Count(progress, "& ~static_cast<LONG_PTR>(PBS_MARQUEE)") == 1,
                   "P3-C4 G2: exactly one style clear (PBS_MARQUEE masked off once)");
        TEST_CHECK(P3C4Count(progress, "| static_cast<LONG_PTR>(PBS_MARQUEE)") == 1,
                   "P3-C4 G2: exactly one style restore (a retry re-arms the marquee)");
        const size_t get_style = progress.find("::GetWindowLongPtrW(prog, GWL_STYLE)");
        const size_t set_style = progress.find("::SetWindowLongPtrW(prog, GWL_STYLE");
        const size_t settle = progress.find("st->marquee_active = false;");
        const size_t range = progress.find("PBM_SETRANGE32, 0, 100");
        const size_t pos = progress.find("PBM_SETPOS");
        TEST_CHECK(get_style != std::string::npos && set_style != std::string::npos &&
                       get_style < set_style,
                   "P3-C4 G2: the style is read, then written (Get before Set)");
        TEST_CHECK(settle != std::string::npos && range != std::string::npos &&
                       pos != std::string::npos && settle < range && range < pos,
                   "P3-C4 G2: the style is cleared BEFORE range+pos (the "
                   "pre-fix PBM_SETMARQUEE FALSE alone left the bar sweeping)");
        // REQ-052 pin survival: the decode still goes through the helper.
        TEST_CHECK(progress.find("const int percent = HfDecodeProgressPercent(wp);") !=
                       std::string::npos ||
                       mgr.find("const int percent = HfDecodeProgressPercent(wp);") !=
                           std::string::npos,
                   "P3-C4 G2: the percent still decodes through HfDecodeProgressPercent");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C4 G2 marquee style toggle passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C4 G2 marquee style toggle: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}

// ---- (c) G3-1: the case-folded reuse compare -----------------------------

void TestP3C4FileNameReuseCompare() {
    std::cout << "[RUN] Testing P3-C4 G3-1 case-folded model-file reuse compare (V10F)..."
              << std::endl;
    const int failures_before = g_failed_count;

    // ---- pure: the compare itself (ASCII A-Z fold, exact otherwise) -------
    TEST_CHECK(ManagerFileNameMatches("Model.gguf", "model.gguf"),
               "P3-C4 G3-1: a differently-cased spelling is the same store file");
    TEST_CHECK(ManagerFileNameMatches("MODEL.GGUF", "model.gguf"),
               "P3-C4 G3-1: the fold covers the extension too");
    TEST_CHECK(ManagerFileNameMatches("model.gguf", "Model.GGUF"),
               "P3-C4 G3-1: the compare is symmetric");
    TEST_CHECK(ManagerFileNameMatches("model.gguf", "model.gguf"),
               "P3-C4 G3-1: an identical name matches");
    TEST_CHECK(!ManagerFileNameMatches("model.gguf", "model-gguf"),
               "P3-C4 G3-1: a punctuation difference is a different file");
    TEST_CHECK(!ManagerFileNameMatches("Model.gguf", "Model.gguf "),
               "P3-C4 G3-1: a trailing space is a different file (length differs)");
    TEST_CHECK(!ManagerFileNameMatches("model.gguf", "model_2.gguf"),
               "P3-C4 G3-1: a copy suffix is a different file");
    TEST_CHECK(ManagerFileNameMatches("", ""),
               "P3-C4 G3-1: two empty names compare equal (no read past the end)");
    // Non-ASCII bytes are compared EXACTLY (no UTF-8 -> UTF-16 collapsing).
    const std::string kModelKo = "\xEB\xAA\xA8\xEB\x8D\xB8.gguf"; // model stem + .gguf
    const std::string kModelKoUpper = "\xEB\xAA\xA8\xEB\x8D\xB8.GGUF";
    const std::string kModelKoOther = "\xEB\xAA\xA8\xEB\x8D\xB9.gguf";
    TEST_CHECK(ManagerFileNameMatches(kModelKo, kModelKoUpper),
               "P3-C4 G3-1: a non-ASCII stem still folds the ASCII extension");
    TEST_CHECK(!ManagerFileNameMatches(kModelKo, kModelKoOther),
               "P3-C4 G3-1: one differing non-ASCII byte is a different file");
    TEST_CHECK(!ManagerFileNameMatches(kModelKo, "model.gguf"),
               "P3-C4 G3-1: a non-ASCII name never matches an ASCII one");

    // ---- source: both HF reuse checks use the fold ------------------------
    std::string mgr;
    P3C4ReadSrc(L"src\\ui\\gguf_model_manager_window.cpp", mgr);
    if (mgr.empty()) {
        std::cout << "[SKIP] gguf_model_manager_window.cpp not resolvable; "
                     "P3-C4 G3-1 wiring pins skipped." << std::endl;
    } else {
        const std::string preflight =
            P3C4Slice(mgr, "HfPreFlightResult HfPreFlight(HWND dlg, HfAddDialogState* st,",
                      "// REQ-050: post-download registration");
        const std::string post =
            P3C4Slice(mgr, "bool HfRegisterDownloaded(HWND dlg, HfAddDialogState* st) {",
                      "void HfResetForRetry(HWND dlg, HfAddDialogState* st) {");
        TEST_CHECK(!preflight.empty() &&
                       preflight.find("ManagerFileNameMatches(m.files[0], filename)") !=
                           std::string::npos &&
                       preflight.find("m.files[0] != filename") == std::string::npos,
                   "P3-C4 G3-1: the pre-flight reuse check folds case");
        TEST_CHECK(!post.empty() &&
                       post.find("ManagerFileNameMatches(m.files[0], st->worker_filename)") !=
                           std::string::npos &&
                       post.find("m.files[0] != st->worker_filename") ==
                           std::string::npos,
                   "P3-C4 G3-1: the post-download reuse check folds case");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C4 G3-1 reuse compare passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C4 G3-1 reuse compare: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}

// ---- (d) G3-2: the delete-path engine-reset guard ------------------------

void TestP3C4DeleteEngineGuard() {
    std::cout << "[RUN] Testing P3-C4 G3-2 delete-path engine reset guard (V10F)..."
              << std::endl;
    const int failures_before = g_failed_count;

    std::string mgr;
    P3C4ReadSrc(L"src\\ui\\gguf_model_manager_window.cpp", mgr);
    if (mgr.empty()) {
        std::cout << "[SKIP] gguf_model_manager_window.cpp not resolvable; "
                     "P3-C4 G3-2 pins skipped." << std::endl;
        return;
    }

    const std::string del = P3C4Slice(mgr, "case IDC_MGR_DELETE: {",
                                      "case IDC_MGR_POLICY: {");
    TEST_CHECK(!del.empty(), "P3-C4 G3-2: the delete handler is locatable");
    if (!del.empty()) {
        const size_t id_gate = del.find("if (st->config->user_model_id == old_id &&");
        const size_t route_gate = del.find("st->config->engine_type == \"user_gguf\") {");
        TEST_CHECK(id_gate != std::string::npos && route_gate != std::string::npos &&
                       id_gate < route_gate,
                   "P3-C4 G3-2: the reset requires BOTH the id match and the live "
                   "user_gguf route");
        TEST_CHECK(del.find("if (st->config->user_model_id == old_id) {") ==
                       std::string::npos,
                   "P3-C4 G3-2: the pre-fix id-only gate is gone (it re-routed a "
                   "google/openai engine to auto)");
        TEST_CHECK(del.find("st->engine->SetEngineType(EngineType::Auto);") !=
                       std::string::npos,
                   "P3-C4 G3-2: the live-route delete still re-points the runtime "
                   "engine to Auto");
        // The delete body still refreshes through the shared helper (G1).
        TEST_CHECK(del.find("RefreshManagerView(dlg, st);") != std::string::npos,
                   "P3-C4 G3-2: the delete refreshes the view after the mutation");
    }

    // The active-row marker keeps the same two-part predicate (the mirror the
    // guard was aligned with).
    const std::string rebuild =
        P3C4Slice(mgr, "void RebuildManagerList(HWND dlg, ManagerDialogState* st) {",
                  "int SelectedUserIndex(");
    TEST_CHECK(!rebuild.empty() &&
                   rebuild.find("st->config->user_model_id == m->id &&") !=
                       std::string::npos &&
                   rebuild.find("st->config->engine_type == \"user_gguf\") {") !=
                       std::string::npos,
               "P3-C4 G3-2: the list's active marker keeps the same two-part "
               "predicate (guard and marker cannot drift)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C4 G3-2 delete engine guard passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C4 G3-2 delete engine guard: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}

// ---- (e) G3-3: the shared row re-select ----------------------------------

void TestP3C4PolicyReselect() {
    std::cout << "[RUN] Testing P3-C4 G3-3 toggled-row re-select (V10F)..." << std::endl;
    const int failures_before = g_failed_count;

    std::string mgr;
    P3C4ReadSrc(L"src\\ui\\gguf_model_manager_window.cpp", mgr);
    if (mgr.empty()) {
        std::cout << "[SKIP] gguf_model_manager_window.cpp not resolvable; "
                     "P3-C4 G3-3 pins skipped." << std::endl;
        return;
    }

    const std::string helper =
        P3C4Slice(mgr,
                  "void SelectManagerRowById(HWND dlg, ManagerDialogState* st, const std::string& id) {",
                  "void RefreshManagerView(");
    TEST_CHECK(!helper.empty(), "P3-C4 G3-3: the shared re-select helper exists");
    if (!helper.empty()) {
        TEST_CHECK(helper.find("::SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(row), 0);") !=
                       std::string::npos,
                   "P3-C4 G3-3: the helper selects the matching row");
        TEST_CHECK(helper.find("st->user_ids[static_cast<size_t>(data)] == id") !=
                       std::string::npos,
                   "P3-C4 G3-3: the row is located through the id -> row map, never "
                   "a stored index");
        TEST_CHECK(helper.find("SyncManagerActionButtons(") == std::string::npos,
                   "P3-C4 G3-3: the helper carries no sync call (the REQ-053 "
                   "four-site count stays intact)");
    }
    TEST_CHECK(P3C4Count(mgr, "SelectManagerRowById(dlg, st,") == 3,
               "P3-C4 G3-3: three call sites (policy toggle + rename + activate)");

    const std::string policy = P3C4Slice(mgr, "case IDC_MGR_POLICY: {",
                                        "case IDC_MGR_DEDUPE: {");
    const size_t rebuild = policy.find("RebuildManagerList(dlg, st);");
    const size_t reselect = policy.find("SelectManagerRowById(dlg, st, sel_id);");
    TEST_CHECK(rebuild != std::string::npos && reselect != std::string::npos &&
                   rebuild < reselect,
               "P3-C4 G3-3: the policy toggle re-selects the toggled row after the "
               "rebuild (the pre-fix selection jumped to row 0)");

    const std::string rename_body = P3C4Slice(mgr, "case IDC_MGR_RENAME: {",
                                              "case IDC_MGR_DELETE: {");
    TEST_CHECK(rename_body.find("SelectManagerRowById(dlg, st, applied_new_id);") !=
                   std::string::npos,
               "P3-C4 G3-3: the rename path re-selects the renamed row through the "
               "shared helper");

    const std::string activate = P3C4Slice(mgr, "if (HIWORD(wp) == LBN_DBLCLK) {",
                                           "case IDC_MGR_RENAME: {");
    const size_t act_reselect = activate.find("SelectManagerRowById(dlg, st, id);");
    const size_t act_sync = activate.find("SyncManagerActionButtons(dlg, st);");
    TEST_CHECK(act_reselect != std::string::npos && act_sync != std::string::npos &&
                   act_reselect < act_sync,
               "P3-C4 G3-3: the activate handler keeps its explicit sync AFTER the "
               "helper (the 4-sync-site pin)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C4 G3-3 toggled-row re-select passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C4 G3-3 toggled-row re-select: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}

// ---- (f) G3-4: the request-target percent encoder ------------------------

void TestP3C4HfRequestPathEncoding() {
    std::cout << "[RUN] Testing P3-C4 G3-4 HF request-target percent encoding (V10F)..."
              << std::endl;
    const int failures_before = g_failed_count;

    // ---- pure: the encoder ------------------------------------------------
    TEST_CHECK(HfPercentEncodeRequestPath("") == "",
               "P3-C4 G3-4: an empty path stays empty");
    TEST_CHECK(HfPercentEncodeRequestPath("a/b/model-Q8_0.gguf") ==
                   "a/b/model-Q8_0.gguf",
               "P3-C4 G3-4: a clean RFC 3986 path passes through byte-for-byte");
    TEST_CHECK(HfPercentEncodeRequestPath("a/Model Q4_K_M.gguf") ==
                   "a/Model%20Q4_K_M.gguf",
               "P3-C4 G3-4: a decoded space is encoded (%20)");
    TEST_CHECK(HfPercentEncodeRequestPath("a/%20") == "a/%20",
               "P3-C4 G3-4: an existing %HH triplet is preserved");
    TEST_CHECK(HfPercentEncodeRequestPath("a/%2f") == "a/%2f",
               "P3-C4 G3-4: a lowercase triplet is preserved verbatim");
    TEST_CHECK(HfPercentEncodeRequestPath("a/100%.gguf") == "a/100%25.gguf",
               "P3-C4 G3-4: a lone percent is encoded (no bogus triplet)");
    TEST_CHECK(HfPercentEncodeRequestPath("a/%2.gguf") == "a/%252.gguf",
               "P3-C4 G3-4: a truncated triplet is encoded");
    TEST_CHECK(HfPercentEncodeRequestPath("%2") == "%252",
               "P3-C4 G3-4: a trailing percent + digit is encoded");
    TEST_CHECK(HfPercentEncodeRequestPath("a/%zz.gguf") == "a/%25zz.gguf",
               "P3-C4 G3-4: a non-hex triplet is encoded");
    // Non-ASCII UTF-8: the bytes are encoded one at a time (uppercase hex).
    const std::string kModelKo = "\xEB\xAA\xA8\xEB\x8D\xB8.gguf";
    TEST_CHECK(HfPercentEncodeRequestPath(kModelKo) ==
                   "%EB%AA%A8%EB%8D%B8.gguf",
               "P3-C4 G3-4: a UTF-8 filename is percent-encoded byte by byte");
    TEST_CHECK(HfPercentEncodeRequestPath(std::string("a\tb")) == "a%09b",
               "P3-C4 G3-4: a control byte is encoded");
    TEST_CHECK(HfPercentEncodeRequestPath(std::string("a\x7F" "b")) == "a%7Fb",
               "P3-C4 G3-4: DEL is encoded");
    TEST_CHECK(HfPercentEncodeRequestPath(std::string("\x80")) == "%80",
               "P3-C4 G3-4: a bare high byte is encoded");
    TEST_CHECK(HfPercentEncodeRequestPath("a?b#c") == "a?b#c",
               "P3-C4 G3-4: printable ASCII sub-delims pass through (the URL "
               "validators already banned queries/fragments)");

    // ---- end-to-end: the normalizer keeps the decoded form ---------------
    // The model-page shape is the ONLY one that can produce a raw space (its
    // query value is percent-decoded by design), so the encoder is the missing
    // half of that path.
    std::string resolved;
    const bool normalized = NormalizeHfUrl(
        "https://huggingface.co/a/b?show_file_info=Model%20Q4_K_M.gguf", &resolved);
    TEST_CHECK(normalized &&
                   resolved == "https://huggingface.co/a/b/resolve/main/Model Q4_K_M.gguf",
               "P3-C4 G3-4: the normalizer keeps the DECODED show_file_info value "
               "(unchanged contract)");
    std::string decoded_name;
    TEST_CHECK(HfResolveFilename(resolved, &decoded_name) &&
                   decoded_name == "Model Q4_K_M.gguf",
               "P3-C4 G3-4: the derived local filename stays the human-readable form");
    // The gate still accepts the decoded URL, which is exactly why the worker
    // must encode at request-open time.
    TEST_CHECK(IsHfResolveUrl(resolved),
               "P3-C4 G3-4: IsHfResolveUrl still accepts the decoded form (the "
               "validator matrix is untouched)");
    constexpr std::size_t kPrefixLen = 23; // "https://huggingface.co/"
    TEST_CHECK(HfPercentEncodeRequestPath(resolved.substr(kPrefixLen)) ==
                   "a/b/resolve/main/Model%20Q4_K_M.gguf",
               "P3-C4 G3-4: the worker's request suffix encodes to a valid target");
    std::string resolved_ko;
    TEST_CHECK(NormalizeHfUrl("https://huggingface.co/a/b?show_file_info=%EB%AA%A8%EB%8D%B8.gguf",
                              &resolved_ko) &&
                   HfPercentEncodeRequestPath(resolved_ko.substr(kPrefixLen)) ==
                       "a/b/resolve/main/%EB%AA%A8%EB%8D%B8.gguf",
               "P3-C4 G3-4: a UTF-8 show_file_info value round-trips to its encoded "
               "request target");

    // ---- source: the encode happens at the request-open site only --------
    std::string mgr;
    P3C4ReadSrc(L"src\\ui\\gguf_model_manager_window.cpp", mgr);
    if (mgr.empty()) {
        std::cout << "[SKIP] gguf_model_manager_window.cpp not resolvable; "
                     "P3-C4 G3-4 wiring pins skipped." << std::endl;
    } else {
        const std::string worker =
            P3C4Slice(mgr, "void HfDownloadWorker(HWND dlg, HfAddDialogState* st) {",
                      "// REQ-052: the fresh-registration identity");
        TEST_CHECK(!worker.empty(),
                   "P3-C4 G3-4: the download worker is locatable");
        const size_t encode = worker.find("HfPercentEncodeRequestPath(ToUtf8(path))");
        const size_t open = worker.find("WinHttpOpenRequest(connect.get(), L\"GET\",");
        const size_t use = worker.find("request_path.c_str()");
        TEST_CHECK(encode != std::string::npos && open != std::string::npos &&
                       use != std::string::npos && encode < open && open < use,
                   "P3-C4 G3-4: the worker encodes the path, then opens the request "
                   "with the encoded target");
        TEST_CHECK(worker.find("const wchar_t* path = st->worker_url.c_str() + kPrefixLen;") !=
                       std::string::npos,
                   "P3-C4 G3-4: the pinned-prefix split is unchanged");
        // The validators themselves carry no encoding (contract preserved).
        const std::string gate =
            P3C4Slice(mgr, "bool IsHfResolveUrl(std::string_view url) {",
                      "namespace {");
        TEST_CHECK(!gate.empty() &&
                       gate.find("HfPercentEncodeRequestPath") == std::string::npos,
                   "P3-C4 G3-4: the resolve-URL gate is untouched by the encoder");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C4 G3-4 request-target encoding passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C4 G3-4 request-target encoding: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}

// ---- (g) E2: the truthful strict-local failure status --------------------

void TestP3C4StrictLocalFailureStatus() {
    std::cout << "[RUN] Testing P3-C4 E2 strict-local failure status mapping (V10F)..."
              << std::endl;
    const int failures_before = g_failed_count;

    // ---- pure: the mapping ------------------------------------------------
    TEST_CHECK(StrictLocalFailureStatus("model_missing") ==
                   TranslationStatus::LocalModelMissing,
               "P3-C4 E2: a host model_missing answer reports LocalModelMissing");
    TEST_CHECK(StrictLocalFailureStatus("") == TranslationStatus::CloudConsentBlocked,
               "P3-C4 E2: an empty code keeps the consent-gate status");
    for (const char* code : {"busy", "connect", "connect:2", "connect:peer", "timeout",
                             "io", "disabled", "no_host_binary", "unauthorized",
                             "version_mismatch", "pin_mismatch", "bad_request",
                             "engine_failed", "Model_Missing", "model_missing ",
                             "MODEL_MISSING"}) {
        TEST_CHECK(StrictLocalFailureStatus(code) == TranslationStatus::CloudConsentBlocked,
                   std::string("P3-C4 E2: '") + code +
                       "' keeps CloudConsentBlocked (only the exact model_missing code "
                       "names an absent model)");
    }
    // The enum values the rest of the app already switches on stay distinct.
    TEST_CHECK(StrictLocalFailureStatus("model_missing") !=
                   StrictLocalFailureStatus("busy"),
               "P3-C4 E2: the two strict-local statuses are distinguishable");

    // 261124_0001: the OPTIONAL defaulted error_code refinement — a
    // model_missing answer carrying "model_load_failed" names a model file
    // that EXISTS but the engine refused to load (its own status and wording);
    // every other detail, and the one-argument form (an old host's frame),
    // keeps the pre-261124 behavior exactly.
    TEST_CHECK(StrictLocalFailureStatus("model_missing", "model_load_failed") ==
                   TranslationStatus::LocalModelLoadFailed,
               "P3-C4 E2 261124: model_missing + model_load_failed reports "
               "LocalModelLoadFailed (present but unloadable)");
    TEST_CHECK(StrictLocalFailureStatus("model_missing", "") ==
                   TranslationStatus::LocalModelMissing,
               "P3-C4 E2 261124: an empty detail keeps LocalModelMissing "
               "(the old-host frame / the one-argument form)");
    TEST_CHECK(StrictLocalFailureStatus("model_missing", "engine_failed") ==
                   TranslationStatus::LocalModelMissing,
               "P3-C4 E2 261124: an unrelated detail never promotes");
    TEST_CHECK(StrictLocalFailureStatus("busy", "model_load_failed") ==
                   TranslationStatus::CloudConsentBlocked,
               "P3-C4 E2 261124: the detail refines a model_missing answer only");

    // ---- source: both local-seam convergence sites use the shared rule ----
    std::string eng, eng_hpp;
    P3C4ReadSrc(L"src\\engine.cpp", eng);
    P3C4ReadSrc(L"src\\engine.hpp", eng_hpp);
    // 261124_0001: the 2-argument defaulted form (the OPTIONAL error_code
    // refinement); the one-argument call sites keep compiling through the
    // defaulted tail argument.
    TEST_CHECK(eng_hpp.find("constexpr TranslationStatus StrictLocalFailureStatus(std::string_view host_err,\n") !=
                   std::string::npos ||
                   eng_hpp.find("constexpr TranslationStatus StrictLocalFailureStatus(std::string_view host_err,") !=
                   std::string::npos,
               "P3-C4 E2 261124: the mapping is the header-inline pure rule with the "
               "defaulted 2nd argument");
    TEST_CHECK(eng_hpp.find("std::string_view host_error_detail = {}") !=
                   std::string::npos,
               "P3-C4 E2 261124: the 2nd argument defaults to {} (old call sites and "
               "old-host frames keep the pre-261124 behavior)");
    if (eng.empty()) {
        std::cout << "[SKIP] src/engine.cpp not resolvable; P3-C4 E2 wiring pins "
                     "skipped." << std::endl;
    } else {
        // 261124_0001: the prefix count covers both the 1-arg spelling and the
        // refined (host_err, host_error_detail) call.
        TEST_CHECK(P3C4Count(eng, "StrictLocalFailureStatus(host_err") == 1,
                   "P3-C4 E2: the single-request convergence maps through the rule");
        // 261124_0001 fix-round-1 R1-F01: the coordinator convergence is the
        // REFINED form too (the frame's error_code rides along), so the
        // giant-text path cannot rank an unloadable model as a missing file.
        TEST_CHECK(P3C4Count(eng, "StrictLocalFailureStatus(frame_run.last_error, "
                                  "frame_run.last_error_detail)") == 1,
                   "P3-C4 E2: the multi-frame coordinator convergence maps through the "
                   "same rule WITH the frame's error_code refinement");
        TEST_CHECK(P3C4Count(eng, "StrictLocalFailureStatus(frame_run.last_error)") == 0,
                   "P3-C4 E2 261124 R1-F01: the one-argument frame call is gone (it "
                   "codified the excluded refinement)");
        TEST_CHECK(eng.find("set_status(TranslationStatus::CloudConsentBlocked);") !=
                       std::string::npos,
                   "P3-C4 E2: the pure-cloud strict status site is unchanged");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C4 E2 strict-local failure status passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C4 E2 strict-local failure status: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}

// ---- (h) FUP-09: the Arabic predicate is letters + marks only ------------

void TestP3C4ArabicScriptRanges() {
    std::cout << "[RUN] Testing P3-C4 FUP-09 Arabic letters-and-marks predicate (V10F)..."
              << std::endl;
    const int failures_before = g_failed_count;

    // ---- excluded: digits and punctuation ---------------------------------
    TEST_CHECK(!ContainsArabic(L"\u0661\u0662\u0663"),
               "P3-C4 FUP-09: Arabic-Indic digits alone are NOT Arabic script");
    TEST_CHECK(!ContainsArabic(L"\u06F1\u06F2\u06F3"),
               "P3-C4 FUP-09: Extended Arabic-Indic digits alone are NOT Arabic script");
    TEST_CHECK(!ContainsArabic(L"\u0600\u0601\u0605\u060F"),
               "P3-C4 FUP-09: the U+0600-U+060F number-sign/abbreviation cluster is "
               "excluded");
    TEST_CHECK(!ContainsArabic(L"\u061B"),
               "P3-C4 FUP-09: the Arabic semicolon (U+061B) is excluded");
    TEST_CHECK(!ContainsArabic(L"\u061F"),
               "P3-C4 FUP-09: the Arabic question mark (U+061F) is excluded");
    TEST_CHECK(!ContainsArabic(L"\u066A\u066B\u066C\u066D"),
               "P3-C4 FUP-09: the U+066A-U+066D punctuation cluster is excluded");

    // ---- kept: letters and combining marks -------------------------------
    TEST_CHECK(ContainsArabic(L"\u0628\u062A"),
               "P3-C4 FUP-09: Arabic letters still count (U+0628/U+062A)");
    TEST_CHECK(ContainsArabic(L"\u0641\u064A"),
               "P3-C4 FUP-09: the U+0641-U+064A letters still count");
    TEST_CHECK(ContainsArabic(L"\u064B\u0651"),
               "P3-C4 FUP-09: combining marks (harakat/shadda) still count");
    TEST_CHECK(ContainsArabic(L"\u0670"),
               "P3-C4 FUP-09: the superscript alef mark (U+0670) still counts");
    TEST_CHECK(ContainsArabic(L"\u06D2"),
               "P3-C4 FUP-09: the Urdu yeh (U+06D2, a letter) still counts");
    TEST_CHECK(ContainsArabic(L"\u0750\u077F"),
               "P3-C4 FUP-09: the Arabic Supplement range is untouched");
    TEST_CHECK(ContainsArabic(L"\u08A0\u08FF"),
               "P3-C4 FUP-09: the Arabic Extended-A range is untouched");
    TEST_CHECK(ContainsArabic(L"\u0628\u0661"),
               "P3-C4 FUP-09: a letter anywhere in the sample still counts");
    TEST_CHECK(ContainsArabic(L"\u0645\u0631\u062D\u0628\u0627 \u0628\u0627\u0644\u0639\u0627\u0644\u0645"),
               "P3-C4 FUP-09: the pinned Arabic greeting still detects");

    // ---- the language decision follows the predicate ---------------------
    TEST_CHECK(DetectLanguage(L"\u0661\u0662\u0663") != "Arabic",
               "P3-C4 FUP-09: a digits-only sample is no longer labeled Arabic");
    TEST_CHECK(DetectLanguage(L"\u0645\u0631\u062D\u0628\u0627") == "Arabic",
               "P3-C4 FUP-09: a pure Arabic word is still labeled Arabic");
    // The bidi mirror: the same AN-class exclusion keeps the direction neutral.
    TEST_CHECK(GuessBaseDirection(L"\u0661\u0662\u0663") == TextDirection::LTR,
               "P3-C4 FUP-09: the bidi kDigitsAN exclusion (LTR for digits-only) "
               "stays in force");
    TEST_CHECK(GuessBaseDirection(L"\u0661\u0662\u0663 \u0645\u0631\u062D\u0628\u0627") ==
                   TextDirection::RTL,
               "P3-C4 FUP-09: digits + letters is still RTL via the letters");

    // ---- source: the exact exclusion set is written down -----------------
    std::string sb;
    P3C4ReadSrc(L"src\\smart_bypass.cpp", sb);
    if (sb.empty()) {
        std::cout << "[SKIP] src/smart_bypass.cpp not resolvable; P3-C4 FUP-09 "
                     "source pins skipped." << std::endl;
    } else {
        const std::string predicate =
            P3C4Slice(sb, "inline bool IsArabicCodePoint(uint32_t cp) {",
                      "inline bool IsCyrillicCodePoint(");
        TEST_CHECK(!predicate.empty(),
                   "P3-C4 FUP-09: the predicate is locatable");
        for (const char* range : {"(cp >= 0x0600 && cp <= 0x060F)",
                                  "cp == 0x061B",
                                  "cp == 0x061F",
                                  "(cp >= 0x0660 && cp <= 0x066D)",
                                  "(cp >= 0x06F0 && cp <= 0x06F9)"}) {
            TEST_CHECK(predicate.find(range) != std::string::npos,
                       std::string("P3-C4 FUP-09: the predicate excludes ") + range);
        }
        TEST_CHECK(predicate.find("return false;") != std::string::npos &&
                       predicate.find("return (cp >= 0x0600 && cp <= 0x06FF)") !=
                           std::string::npos,
               "P3-C4 FUP-09: the exclusions short-circuit BEFORE the block range "
               "(letters/marks survive)");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C4 FUP-09 Arabic predicate passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C4 FUP-09 Arabic predicate: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}

// ---- (i) pin survival: the anchors of the edited regions still hold ------

void TestP3C4PinSurvival() {
    std::cout << "[RUN] Testing P3-C4 pin survival (pre-existing anchors)..." << std::endl;
    const int failures_before = g_failed_count;

    std::string mgr, eng, sb, bidi;
    P3C4ReadSrc(L"src\\ui\\gguf_model_manager_window.cpp", mgr);
    P3C4ReadSrc(L"src\\engine.cpp", eng);
    P3C4ReadSrc(L"src\\smart_bypass.cpp", sb);
    P3C4ReadSrc(L"src\\bidi_utils.cpp", bidi);

    if (!mgr.empty()) {
        // REQ-053 A7: exactly four sync sites; the pair uses the shared line.
        TEST_CHECK(P3C4Count(mgr, "SyncManagerActionButtons(dlg, st);") == 4,
                   "P3-C4 survival: REQ-053's four SyncManagerActionButtons sites hold");
        TEST_CHECK(P3C4Count(mgr, "::EnableWindow(b, enabled ? TRUE : FALSE);") == 2,
                   "P3-C4 survival: REQ-053's two-button EnableWindow line holds");
        TEST_CHECK(P3C4Count(mgr, "// 260922_0001 A7: defensive guard") == 2,
                   "P3-C4 survival: both action-handler defensive-guard annotations hold");
        // REQ-052: the progress decode helper + the 64-bit Content-Length parse.
        TEST_CHECK(mgr.find("const int percent = HfDecodeProgressPercent(wp);") !=
                       std::string::npos &&
                       mgr.find("static_cast<int>(static_cast<WORD>(wp))") ==
                           std::string::npos,
                   "P3-C4 survival: REQ-052's progress decode pin holds");
        TEST_CHECK(mgr.find("WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER") ==
                       std::string::npos &&
                       mgr.find("unsigned long long parsed") != std::string::npos,
                   "P3-C4 survival: REQ-052's 64-bit Content-Length pins hold");
        // REQ-050 2-1 + REQ-052 registration-path pins.
        TEST_CHECK(mgr.find("st->worker_url = ToUtf16(resolve_url)") != std::string::npos,
                   "P3-C4 survival: REQ-050's normalized-URL hand-off pin holds");
        TEST_CHECK(P3C4Count(mgr, "st->registered = true") >= 3 &&
                       P3C4Count(mgr, "I18n::Get(StringId::HfDone)") >= 2,
                   "P3-C4 survival: REQ-052's every-registration-path pins hold");
        TEST_CHECK(mgr.find("WriteRegistryAtomically(registry, models_dir)") !=
                           std::string::npos &&
                       mgr.find("policy toggled id_len=%zu") != std::string::npos,
                   "P3-C4 survival: the M7 A-2 atomic write + REQ-060 L-4 shape-only "
                   "log pins hold");
    }

    if (!eng.empty()) {
        // REQ-051: the strict-local CloudConsentBlocked convergence survives
        // AFTER the retry warrant (the security boundary pin).
        const size_t warrant = eng.find("EngineHostTransientRetryWarranted(");
        const size_t strict_site = eng.find("set_status(TranslationStatus::CloudConsentBlocked);");
        TEST_CHECK(warrant != std::string::npos && strict_site != std::string::npos &&
                       warrant < strict_site,
                   "P3-C4 survival: REQ-051's strict-local convergence pin holds");
        TEST_CHECK(P3C4Count(eng, "set_status(TranslationStatus::CloudConsentBlocked);") == 2,
                   "P3-C4 survival: exactly two literal CloudConsentBlocked verdicts "
                   "remain (the pure-cloud path + the V10F-R1 F-17 http consent-origin "
                   "gate at the OpenAI routing leg, engine.cpp:1299)");
        // p1_u4 E1: the helper guards three gates; two host-success notes.
        TEST_CHECK(P3C4Count(eng, "if (IsAnyCancelRequested(ctx)) {") == 3,
                   "P3-C4 survival: p1_u4 E1's three cancel-gate helper sites hold");
        TEST_CHECK(P3C4Count(eng, "NoteHostSuccess();") == 2,
                   "P3-C4 survival: the two host-success notes hold");
        // translation_progress: the cloud verdict line is untouched.
        TEST_CHECK(eng.find("set_status(res.empty() ? TranslationStatus::EngineFailed : TranslationStatus::Ok);") !=
                       std::string::npos,
                   "P3-C4 survival: the cloud seam verdict pin holds");
        // The REQ-029-B pre-block still exists with its LocalModelMissing verdict.
        TEST_CHECK(eng.find("if (preferred == EngineType::LocalLlama && !HostCouldServeFor(host_cfg)) {") !=
                           std::string::npos &&
                       eng.find("set_status(TranslationStatus::LocalModelMissing);") !=
                           std::string::npos,
                   "P3-C4 survival: the REQ-029-B pre-block pins hold");
    }

    if (!sb.empty()) {
        // WS1 / V107: the retired ASCII-shape helper stays retired.
        TEST_CHECK(sb.find("bool IsAsciiEnglishShapedToken(") == std::string::npos,
                   "P3-C4 survival: the WS1/V107 retired-shape ban holds");
        TEST_CHECK(sb.find("kEnglishSingleWordEvidence") != std::string::npos,
                   "P3-C4 survival: the WS1 English evidence lexicon pin holds");
    }

    if (!bidi.empty()) {
        // The FUP-09 mirror: bidi_utils still owns the AN-class exclusion.
        TEST_CHECK(bidi.find("constexpr CodeRange kDigitsAN[] = {") != std::string::npos &&
                       bidi.find("{0x0660, 0x0669},") != std::string::npos &&
                       bidi.find("{0x06F0, 0x06F9},") != std::string::npos,
                   "P3-C4 survival: bidi_utils' kDigitsAN exclusion (the FUP-09 "
                   "mirror) is unchanged");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C4 pin survival passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C4 pin survival: "
                  << (g_failed_count - failures_before) << " check(s) failed."
                  << std::endl;
    }
}

void P3C4RegisterTests() {
    TestP3C4RefreshManagerView();
    TestP3C4MarqueeStyleToggle();
    TestP3C4FileNameReuseCompare();
    TestP3C4DeleteEngineGuard();
    TestP3C4PolicyReselect();
    TestP3C4HfRequestPathEncoding();
    TestP3C4StrictLocalFailureStatus();
    TestP3C4ArabicScriptRanges();
    TestP3C4PinSurvival();
}

} // namespace p3_c4_tests
