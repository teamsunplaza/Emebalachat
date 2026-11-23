// ---------------------------------------------------------------------------
// R4 fix unit (261122_0002 v0.10.10 phased fixes, ROUND 1, tag V10F-R1):
// app-surface remediations (main.cpp, tooltip, translation editor, history,
// gguf-model-manager HF download, settings hub, OpenAI settings dialog).
// ROUND 2 EXTENSION (tag V10F-R2, fix unit R5): rows F-24..F-31 of the Round-2
// table (docs/261122_0002_session_v01010-phased-fixes/fix-list-round1.md):
//   * F-24/F-25: the editor's bounded activation pump re-posts an observed
//     WM_QUIT once and ABORTS (the pre-fix continue re-peeked it forever),
//     and the apply path is single-flight (in-progress flag, Apply/
//     Retranslate grayed for the wait, the pump skips the editor's own
//     messages but still services kMsgResultReady).
//   * F-26: the engine's F-17 http-consent gate derives the candidate origin
//     from the base_url in the legacy case (persisted origin empty + consent
//     flag), mirroring the dialog's WM_INITDIALOG seed; a persisted non-empty
//     origin stays fail-closed.
//   * F-27: the editor refuses the apply when target_pid is 0 for a non-null
//     target_hwnd, and re-verifies the owner immediately before the paste
//     (after the activation wait); the history contract comments carry the
//     refusal.
//   * F-28: the model-list GET is origin-gated exactly like the chat leg —
//     OpenAiConfig carries the request-time http_consent_origin and the
//     dialog hands the fetch the consented scope; a stale boolean alone fails
//     closed pre-transport.
//   * F-29: the preview marshal merge rule adopts the newest pending Append's
//     cumulative into an End payload (an End carries success only), so a run
//     whose last Append is queued when End arrives does not freeze blank.
//   * F-30: the fetch generation is a session-global atomic referenced by the
//     dialog state (the F-18 teardown bump is now visible to the next
//     session for good).
//   * F-31: the post-hoc abort scan has a purpose-scoped variant; the chat
//     post-hoc arm reads the chat slot only (a chat cancel cannot discard a
//     sibling purpose's cleanly-finished result).
//   * F-02 (call sites): the hub apply_extra + hub diag writes route through
//     the R3 locked config verbs (SetHotkey*/SetSamplingParams/SetDiagLog*),
//     never direct member writes (the BUG-08 class).
//   * F-04: the back-translate history Push records the TRUE pair — src_code
//     = the forward source language (job.forward_source, off the request's
//     resolved source), tgt_code = job.forward_target.
//   * F-06: the typing-error notice autohides on a DEDICATED 3 s constant;
//     the shared kMessageAutohideMs (1.8 s) is untouched.
//   * F-07: the live preview card's wheel/thumb clamp basis is its own body
//     band (h - 48 - 21 = h-69), not the translation card's h-92 formula.
//   * F-08: the preview marshal merge rule — an Append/End MERGES behind a
//     pending Begin (which carries total_chunks) instead of coalescing it
//     away; a Begin still replaces everything.
//   * F-09: the editor apply verifies the target PID against HWND recycling,
//     wraps PasteAndRestore in a BackupClipboard/restorer fallback, pumps a
//     bounded <=300 ms peek loop (no 30xSleep(10)), and the modeless editor
//     is registered in the shutdown sweep; the history ring zeroizes evicted
//     + cleared entry buffers.
//   * F-15: the HF download pins WINHTTP_OPTION_REDIRECT_POLICY_NEVER
//     (fail-closed on the SetOption result) and follows 3xx hops ONLY through
//     the https + HF-CDN allowlist follower (SplitHfRedirectLocation /
//     IsAllowedHfDownloadHost), before the structural GGUF gate.
//   * F-17 (dialog half): [저장]/IDOK hand the consent origin out through the
//     in/out channel and the caller persists it via the locked
//     SetHttpConsentOrigin verb; WM_INITDIALOG seeds from config;
//     [설정 삭제] clears it at both persist sites.
//   * F-18: WM_DESTROY bumps fetch_generation so a detached fetch result
//     cannot refill a later session.
// Source pins read through ResolveRepoFile + raw-byte find() (CRLF-normalized,
// the r3_fix_tests.inc precedent; every needle is ASCII-only).
// ---------------------------------------------------------------------------

#include "../src/translation_history.hpp"
#include "../src/ui/tooltip.hpp"
#include "../src/ui/gguf_model_manager_window.hpp"

namespace r4_fix_tests {

void R4ReadSrc(const wchar_t* rel, std::string& out) {
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

size_t R4CountOf(const std::string& haystack, const std::string& needle) {
    size_t count = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

// ---- F-02: the call sites route through the locked verbs -------------------

void TestR4LockedVerbCallSites() {
    std::cout << "[RUN] Testing R4 F-02 locked-verb call sites..." << std::endl;
    const int failures_before = g_failed_count;

    std::string hub_cpp, main_cpp, hub_hpp;
    R4ReadSrc(L"src\\ui\\settings_hub_window.cpp", hub_cpp);
    R4ReadSrc(L"src\\main.cpp", main_cpp);
    R4ReadSrc(L"src\\ui\\settings_hub_window.hpp", hub_hpp);
    TEST_CHECK(!hub_cpp.empty() && !main_cpp.empty() && !hub_hpp.empty(),
               "R4 F-02: hub/main sources resolve for the pins");

    // The hub's diag writes go through the locked verbs, never a plain member
    // write (the BUG-08 class the header contract names).
    TEST_CHECK(hub_cpp.find("st->config->SetDiagLogEnabled(plan.diag_log_enabled);") !=
                   std::string::npos &&
                   hub_cpp.find("st->config->SetDiagLogContent(plan.diag_log_content);") !=
                       std::string::npos,
               "R4 F-02: the hub diag writes use the locked verbs");
    TEST_CHECK(hub_cpp.find("st->config->diag_log_enabled =") == std::string::npos &&
                   hub_cpp.find("st->config->diag_log_content =") == std::string::npos,
               "R4 F-02: no direct hub diag member write remains");
    TEST_CHECK(hub_cpp.find("V10F-R1 F-02") != std::string::npos,
               "R4 F-02: the hub hunks carry the V10F-R1 tag");

    // main.cpp's apply_extra: the four hotkeys + the sampling quad through the
    // locked verbs; drag/cloud keep their established discipline.
    TEST_CHECK(main_cpp.find("config.SetHotkeyToggle(extra.hotkey_toggle);") !=
                   std::string::npos &&
                   main_cpp.find("config.SetHotkeyLang(extra.hotkey_lang);") !=
                       std::string::npos &&
                   main_cpp.find("config.SetHotkeyMode(extra.hotkey_mode);") !=
                       std::string::npos &&
                   main_cpp.find("config.SetHotkeySelectionTranslate(extra.hotkey_selection_translate);") !=
                       std::string::npos,
               "R4 F-02: apply_extra writes the four hotkeys through the locked verbs");
    TEST_CHECK(main_cpp.find("config.SetSamplingParams(extra.temperature, extra.top_p, extra.top_k,") !=
                   std::string::npos,
               "R4 F-02: apply_extra writes the sampling quad through the locked verb");
    TEST_CHECK(main_cpp.find("config.hotkey_toggle =") == std::string::npos &&
                   main_cpp.find("config.temperature =") == std::string::npos &&
                   main_cpp.find("config.repetition_penalty =") == std::string::npos,
               "R4 F-02: no direct hotkey/sampling member write remains in main.cpp");
    TEST_CHECK(main_cpp.find("V10F-R1 F-02") != std::string::npos,
               "R4 F-02: the main.cpp apply_extra hunk carries the V10F-R1 tag");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R4 F-02 locked-verb call sites passed." << std::endl;
    } else {
        std::cout << "[FAIL] R4 F-02 locked-verb call sites: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- F-04: the back-translate history pair is the TRUE pair ----------------

void TestR4BackTranslateHistoryCodes() {
    std::cout << "[RUN] Testing R4 F-04 back-translate history codes..." << std::endl;
    const int failures_before = g_failed_count;

    std::string main_cpp;
    R4ReadSrc(L"src\\main.cpp", main_cpp);
    TEST_CHECK(!main_cpp.empty(), "R4 F-04: main.cpp resolves for the pins");

    // The job carries the forward source language off the request's resolved
    // source slot.
    TEST_CHECK(main_cpp.find("job.forward_source = req.detected_source;") != std::string::npos,
               "R4 F-04: the job records the forward source (the resolved source)");
    TEST_CHECK(main_cpp.find("std::string forward_source;") != std::string::npos,
               "R4 F-04: BackTranslateJob owns the forward_source field");

    // The Push at the back-translate site: src = forward source, tgt = forward
    // target, in THAT order (the pre-fix order was exactly backwards).
    const size_t push_at = main_cpp.find(
        "emebalachat::translation_history::Push({ job.original_text, job.forward_text, back_text,");
    TEST_CHECK(push_at != std::string::npos,
               "R4 F-04: the back-translate Push is locatable");
    if (push_at != std::string::npos) {
        // Bound at the call's real terminator (the call closes with
        // "job.target_hwnd });", not a "}));" — the latter never occurs, so the
        // old bound silently extended push_call to EOF and swept in unrelated
        // downstream job.back_target mentions).
        const size_t push_end = main_cpp.find("job.target_hwnd });", push_at);
        const std::string push_call = main_cpp.substr(
            push_at, push_end == std::string::npos ? std::string::npos
                                                   : push_end + 20 - push_at);
        const size_t src_at = push_call.find("ToUtf16(job.forward_source)");
        const size_t tgt_at = push_call.find("ToUtf16(job.forward_target)");
        TEST_CHECK(src_at != std::string::npos && tgt_at != std::string::npos &&
                       src_at < tgt_at,
                   "R4 F-04: src_code = forward_source PRECEDES tgt_code = forward_target");
        TEST_CHECK(push_call.find("job.back_target") == std::string::npos,
                   "R4 F-04: the back-translation language no longer leaks into the pair");
    }
    TEST_CHECK(main_cpp.find("V10F-R1 F-04") != std::string::npos,
               "R4 F-04: the hunks carry the V10F-R1 tag");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R4 F-04 back-translate history codes passed." << std::endl;
    } else {
        std::cout << "[FAIL] R4 F-04 back-translate history codes: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- F-06: the dedicated typing-error autohide -----------------------------

void TestR4TypingErrorAutohide() {
    std::cout << "[RUN] Testing R4 F-06 typing-error autohide..." << std::endl;
    const int failures_before = g_failed_count;

    // Constant pins (compile-time): the dedicated budget is exactly 3 s and
    // the shared message budget is untouched at 1.8 s.
    static_assert(emebalachat::TooltipWindow::kTypingErrorAutohideMs == 3000,
                  "R4 F-06: the typing-error notice autohides at exactly 3 s");
    static_assert(emebalachat::TooltipWindow::kMessageAutohideMs == 1800,
                  "R4 F-06: the shared message autohide stays at 1.8 s");

    std::string main_cpp, tooltip_hpp;
    R4ReadSrc(L"src\\main.cpp", main_cpp);
    R4ReadSrc(L"src\\ui\\tooltip.hpp", tooltip_hpp);
    TEST_CHECK(tooltip_hpp.find("kTypingErrorAutohideMs = 3000") != std::string::npos,
               "R4 F-06: the dedicated constant lives next to kMessageAutohideMs");
    // The typing-error seam passes the dedicated budget; the payload carries it.
    TEST_CHECK(main_cpp.find("emebalachat::TooltipWindow::kTypingErrorAutohideMs") !=
                   std::string::npos,
               "R4 F-06: SetOnTypingError passes the dedicated budget");
    TEST_CHECK(tooltip_hpp.find("DWORD autohide_ms = kMessageAutohideMs;") !=
                   std::string::npos,
               "R4 F-06: the MessagePayload carries the per-notice budget");
    TEST_CHECK(main_cpp.find("V10F-R1 F-06") != std::string::npos,
               "R4 F-06: the hunks carry the V10F-R1 tag");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R4 F-06 typing-error autohide passed." << std::endl;
    } else {
        std::cout << "[FAIL] R4 F-06 typing-error autohide: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- F-07: the preview wheel clamp basis is h-69 on the preview path -------

void TestR4PreviewViewportClamp() {
    std::cout << "[RUN] Testing R4 F-07 preview wheel clamp..." << std::endl;
    const int failures_before = g_failed_count;

    std::string tooltip_hpp;
    R4ReadSrc(L"src\\ui\\tooltip.hpp", tooltip_hpp);
    TEST_CHECK(!tooltip_hpp.empty(), "R4 F-07: tooltip.hpp resolves for the pins");

    const size_t fn_at = tooltip_hpp.find("float ActiveScrollViewportDip() const {");
    TEST_CHECK(fn_at != std::string::npos, "R4 F-07: ActiveScrollViewportDip is locatable");
    if (fn_at != std::string::npos) {
        const size_t fn_end = tooltip_hpp.find("\n    }", fn_at);
        const std::string fn = tooltip_hpp.substr(
            fn_at, fn_end == std::string::npos ? std::string::npos : fn_end - fn_at);
        TEST_CHECK(fn.find("preview_phase_ != ChunkPreviewPhase::Inactive") != std::string::npos,
                   "R4 F-07: the live-preview phase takes its own clamp branch");
        TEST_CHECK(fn.find("kBodyTopDip -") != std::string::npos ||
                       fn.find("- kBodyTopDip -") != std::string::npos ||
                       fn.find("kBodyTopDip") != std::string::npos,
                   "R4 F-07: the preview branch subtracts the body top");
        TEST_CHECK(fn.find("kPreviewFooterReserveDip") != std::string::npos,
                   "R4 F-07: the preview branch subtracts the 21 DIP preview footer reserve");
        TEST_CHECK(fn.find("V10F-R1 F-07") != std::string::npos,
                   "R4 F-07: the clamp hunk carries the V10F-R1 tag");
        // The translation-card path keeps the REQ-002 h-92 formula untouched.
        TEST_CHECK(fn.find("return BodyViewportHeightDip(current_height_);") != std::string::npos,
                   "R4 F-07: the non-preview path keeps the h-92 body formula");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R4 F-07 preview wheel clamp passed." << std::endl;
    } else {
        std::cout << "[FAIL] R4 F-07 preview wheel clamp: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- F-08: the preview marshal merge rule (behavioral) ---------------------

void TestR4PreviewCoalesce() {
    std::cout << "[RUN] Testing R4 F-08 preview marshal merge..." << std::endl;
    const int failures_before = g_failed_count;

    using emebalachat::TooltipWindow;
    using Payload = TooltipWindow::ChunkPreviewPayload;
    auto begin = [](int total) {
        Payload p;
        p.op = TooltipWindow::ChunkPreviewOp::Begin;
        p.total_chunks = total;
        return p;
    };
    auto append = [](const wchar_t* text) {
        Payload p;
        p.op = TooltipWindow::ChunkPreviewOp::Append;
        p.cumulative = text;
        return p;
    };
    auto end = [](bool success) {
        Payload p;
        p.op = TooltipWindow::ChunkPreviewOp::End;
        p.success = success;
        return p;
    };

    // 1. The regression itself: Begin queued, Append posted behind it before
    //    the drain — the Begin must SURVIVE (it carries total_chunks) and the
    //    Append rides behind it, so the drain applies Begin first.
    {
        std::deque<Payload> q;
        TooltipWindow::CoalesceChunkPreviewPayload(q, begin(5));
        TooltipWindow::CoalesceChunkPreviewPayload(q, append(L"chunk-1"));
        TEST_CHECK(q.size() == 2 && q[0].op == TooltipWindow::ChunkPreviewOp::Begin &&
                       q[0].total_chunks == 5 &&
                       q[1].op == TooltipWindow::ChunkPreviewOp::Append &&
                       q[1].cumulative == L"chunk-1",
                   "R4 F-08: an Append MERGES behind a pending Begin (no dropped Appends)");
    }
    // 2. Latest-wins within the run: a newer Append replaces the older pending
    //    one; the Begin stays.
    {
        std::deque<Payload> q;
        TooltipWindow::CoalesceChunkPreviewPayload(q, begin(3));
        TooltipWindow::CoalesceChunkPreviewPayload(q, append(L"old"));
        TooltipWindow::CoalesceChunkPreviewPayload(q, append(L"new"));
        TEST_CHECK(q.size() == 2 && q[0].total_chunks == 3 &&
                       q[1].cumulative == L"new",
                   "R4 F-08: the newest pending Append wins, the Begin survives");
    }
    // 3. An End rides behind the pending Begin too.
    {
        std::deque<Payload> q;
        TooltipWindow::CoalesceChunkPreviewPayload(q, begin(2));
        TooltipWindow::CoalesceChunkPreviewPayload(q, end(true));
        TEST_CHECK(q.size() == 2 && q[0].op == TooltipWindow::ChunkPreviewOp::Begin &&
                       q[1].op == TooltipWindow::ChunkPreviewOp::End && q[1].success,
                   "R4 F-08: an End merges behind a pending Begin");
    }
    // 4. A fresh Begin still supersedes EVERYTHING queued (new run).
    {
        std::deque<Payload> q;
        TooltipWindow::CoalesceChunkPreviewPayload(q, begin(9));
        TooltipWindow::CoalesceChunkPreviewPayload(q, append(L"stale"));
        TooltipWindow::CoalesceChunkPreviewPayload(q, begin(4));
        TEST_CHECK(q.size() == 1 && q[0].op == TooltipWindow::ChunkPreviewOp::Begin &&
                       q[0].total_chunks == 4,
                   "R4 F-08: a new Begin replaces the whole pending run");
    }
    // 5. An Append/End with no pending Begin still lands (the drain drops it on
    //    the inactive-phase guard, exactly the pre-F-08 contract for that case).
    {
        std::deque<Payload> q;
        TooltipWindow::CoalesceChunkPreviewPayload(q, append(L"orphan"));
        TEST_CHECK(q.size() == 1 && q[0].cumulative == L"orphan",
                   "R4 F-08: an Append with no pending Begin still queues");
    }
    // 6. The queue never grows past one Begin + one op.
    {
        std::deque<Payload> q;
        for (int i = 0; i < 20; ++i) {
            TooltipWindow::CoalesceChunkPreviewPayload(q, begin(i));
            TooltipWindow::CoalesceChunkPreviewPayload(q, append(L"x"));
        }
        TEST_CHECK(q.size() <= 2,
                   "R4 F-08: the merged queue never holds more than a Begin + one op");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R4 F-08 preview marshal merge passed." << std::endl;
    } else {
        std::cout << "[FAIL] R4 F-08 preview marshal merge: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- F-09: the editor hardening + the history zeroize ----------------------

void TestR4EditorAndHistoryHardening() {
    std::cout << "[RUN] Testing R4 F-09 editor + history hardening..." << std::endl;
    const int failures_before = g_failed_count;

    std::string editor_cpp, editor_hpp, hist_cpp, hist_hpp, main_cpp;
    R4ReadSrc(L"src\\ui\\translation_editor_window.cpp", editor_cpp);
    R4ReadSrc(L"src\\ui\\translation_editor_window.hpp", editor_hpp);
    R4ReadSrc(L"src\\translation_history.cpp", hist_cpp);
    R4ReadSrc(L"src\\translation_history.hpp", hist_hpp);
    R4ReadSrc(L"src\\main.cpp", main_cpp);
    TEST_CHECK(!editor_cpp.empty() && !hist_cpp.empty() && !hist_hpp.empty() &&
                   !main_cpp.empty(),
               "R4 F-09: the editor/history/main sources resolve for the pins");

    // Editor: PID verify, restorer fallback, bounded pump, shutdown sweep.
    TEST_CHECK(editor_cpp.find("GetWindowThreadProcessId(target, &live_pid)") !=
                   std::string::npos &&
                   editor_cpp.find("live_pid != st.entry.target_pid") != std::string::npos,
               "R4 F-09: the apply verifies the target PID against HWND recycling");
    TEST_CHECK(editor_cpp.find("RestorerFallback") != std::string::npos &&
                   editor_cpp.find("ClipboardRestoreArmable") != std::string::npos &&
                   editor_cpp.find("RestoreClipboard(backup);") != std::string::npos,
               "R4 F-09: the BackupClipboard/restorer fallback wraps PasteAndRestore");
    TEST_CHECK(editor_cpp.find("PeekMessageW") != std::string::npos &&
                   editor_cpp.find("+ 300") != std::string::npos &&
                   editor_cpp.find("Sleep(10)") == std::string::npos,
               "R4 F-09: the 30xSleep(10) busy-wait is a bounded <=300ms peek loop");
    TEST_CHECK(editor_cpp.find("void ShutdownDestroy()") != std::string::npos,
               "R4 F-09: the editor exposes ShutdownDestroy()");
    TEST_CHECK(main_cpp.find("emebalachat::translation_editor_window::ShutdownDestroy();") !=
                   std::string::npos,
               "R4 F-09: the shutdown sweep destroys the editor");
    TEST_CHECK(editor_cpp.find("V10F-R1 F-09") != std::string::npos,
               "R4 F-09: the editor hunks carry the V10F-R1 tag");

    // History: the PID field, the choke-point recording, the eviction zeroize,
    // the Clear() sweep.
    TEST_CHECK(hist_hpp.find("DWORD target_pid = 0;") != std::string::npos,
               "R4 F-09: Entry carries the recorded target PID");
    TEST_CHECK(hist_cpp.find("SecureZeroMemory") != std::string::npos &&
                   hist_cpp.find("ZeroizeEntry(h.back())") != std::string::npos,
               "R4 F-09: the eviction path zeroizes the evicted entry's buffers");
    TEST_CHECK(hist_cpp.find("GetWindowThreadProcessId(entry.target_hwnd, &entry.target_pid)") !=
                   std::string::npos,
               "R4 F-09: the ring records the owning PID centrally at Push");
    TEST_CHECK(hist_cpp.find("void Clear()") != std::string::npos,
               "R4 F-09: the ring exposes the zeroize-and-empty Clear()");
    TEST_CHECK(main_cpp.find("emebalachat::translation_history::Clear();") != std::string::npos,
               "R4 F-09: the shutdown sweep zeroizes the ring");

    // Behavioral: the ring cap + eviction still hold with the zeroize in
    // place, and Clear() empties it. (Runs after the P2-G suite; pushes our
    // own markers, then clears so no later suite sees R4 residue.)
    using emebalachat::translation_history::Clear;
    using emebalachat::translation_history::Entry;
    using emebalachat::translation_history::kMaxHistoryEntries;
    using emebalachat::translation_history::Push;
    using emebalachat::translation_history::Recent;
    static_assert(kMaxHistoryEntries == 10, "R4 F-09: the ring cap is still exactly 10");
    for (int i = 0; i < 12; ++i) {
        Entry e;
        e.original = L"r4-ring-" + std::to_wstring(i);
        e.forward = L"r4-fwd-" + std::to_wstring(i);
        e.back = L"r4-back-" + std::to_wstring(i);
        e.src_code = L"KO";
        e.tgt_code = L"EN";
        // nullptr target: no PID query runs (the hwnd is never dereferenced),
        // so the recorded PID stays the documented 0 = "unverifiable".
        e.target_hwnd = nullptr;
        Push(std::move(e));
    }
    std::vector<Entry> recent = Recent();
    TEST_CHECK(recent.size() == kMaxHistoryEntries &&
                   recent.front().original == L"r4-ring-11" &&
                   recent.back().original == L"r4-ring-2",
               "R4 F-09: the zeroizing ring still caps at 10, newest first");
    TEST_CHECK(recent.front().target_pid == 0,
               "R4 F-09: an unknown target records target_pid 0 (check skipped)");
    Clear();
    TEST_CHECK(Recent().empty(), "R4 F-09: Clear() zeroizes and empties the ring");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R4 F-09 editor + history hardening passed." << std::endl;
    } else {
        std::cout << "[FAIL] R4 F-09 editor + history hardening: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- F-15: the HF redirect follower (pure) ---------------------------------

void TestR4HfRedirectFollower() {
    std::cout << "[RUN] Testing R4 F-15 HF redirect follower..." << std::endl;
    const int failures_before = g_failed_count;

    using emebalachat::IsAllowedHfDownloadHost;
    using emebalachat::SplitHfRedirectLocation;

    // Host allowlist matrix.
    TEST_CHECK(IsAllowedHfDownloadHost("huggingface.co"),
               "R4 F-15: the exact origin host is allowed");
    TEST_CHECK(IsAllowedHfDownloadHost("cdn-lfs.huggingface.co") &&
                   IsAllowedHfDownloadHost("cdn-lfs-us-1.huggingface.co"),
               "R4 F-15: the .huggingface.co CDN subdomains are allowed");
    TEST_CHECK(IsAllowedHfDownloadHost("cas-bridge.xethub.hf.co") &&
                   IsAllowedHfDownloadHost("cdn-lfs-us-1.hf.co") &&
                   IsAllowedHfDownloadHost("transfer.xethub.hf.co"),
               "R4 F-15: the .hf.co CDN subdomains (xet bridge, LFS) are allowed");
    TEST_CHECK(!IsAllowedHfDownloadHost("evil.com") &&
                   !IsAllowedHfDownloadHost("huggingface.co.evil.com") &&
                   !IsAllowedHfDownloadHost("evilhf.co") &&
                   !IsAllowedHfDownloadHost("hf.co") &&
                   !IsAllowedHfDownloadHost(".hf.co") &&
                   !IsAllowedHfDownloadHost("huggingface.co.") &&
                   !IsAllowedHfDownloadHost(""),
               "R4 F-15: off-infrastructure / bare-suffix / empty hosts fail closed");
    TEST_CHECK(!IsAllowedHfDownloadHost("HuggingFace.co"),
               "R4 F-15: the host compare is case-sensitive (the lookalike gate)");

    // Location splitter matrix.
    {
        std::string host, path;
        TEST_CHECK(SplitHfRedirectLocation(
                       "https://cas-bridge.xethub.hf.co/a/b?X-Amz-Signature=abc%2F123",
                       &host, &path) &&
                       host == "cas-bridge.xethub.hf.co" &&
                       path == "/a/b?X-Amz-Signature=abc%2F123",
                   "R4 F-15: the Location path + signed query are kept VERBATIM "
                   "(no double percent-encoding)");
        TEST_CHECK(SplitHfRedirectLocation("https://huggingface.co/x/y", &host, &path) &&
                       host == "huggingface.co" && path == "/x/y",
                   "R4 F-15: a plain host + path splits");
        TEST_CHECK(SplitHfRedirectLocation("https://huggingface.co", &host, &path) &&
                       host == "huggingface.co" && path == "/",
                   "R4 F-15: a bare host yields the root path");
        TEST_CHECK(!SplitHfRedirectLocation("http://huggingface.co/x", &host, &path),
                   "R4 F-15: http redirect targets fail closed");
        TEST_CHECK(!SplitHfRedirectLocation("https://user@huggingface.co/x", nullptr, nullptr),
                   "R4 F-15: userinfo-bearing hosts fail closed");
        TEST_CHECK(!SplitHfRedirectLocation("https://huggingface.co:8443/x", nullptr, nullptr),
                   "R4 F-15: explicit ports fail closed (the default https port)");
        TEST_CHECK(!SplitHfRedirectLocation("", nullptr, nullptr),
                   "R4 F-15: an empty Location fails closed");
    }

    // Structural pins on the worker: the NEVER policy is pinned BEFORE the
    // request and the SetOption result is checked; the follower is the only
    // 3xx path; the hop loop polls cancel and runs before the GGUF gate.
    std::string mgr_cpp;
    R4ReadSrc(L"src\\ui\\gguf_model_manager_window.cpp", mgr_cpp);
    TEST_CHECK(!mgr_cpp.empty(), "R4 F-15: the manager source resolves for the pins");
    const size_t worker_at = mgr_cpp.find("void HfDownloadWorker(HWND dlg, HfAddDialogState* st) {");
    TEST_CHECK(worker_at != std::string::npos, "R4 F-15: the download worker is locatable");
    if (worker_at != std::string::npos) {
        const size_t pin_at = mgr_cpp.find("WINHTTP_OPTION_REDIRECT_POLICY_NEVER", worker_at);
        const size_t send_at = mgr_cpp.find("WinHttpSendRequest", worker_at);
        TEST_CHECK(pin_at != std::string::npos && send_at != std::string::npos &&
                       pin_at < send_at,
                   "R4 F-15: the NEVER policy is pinned BEFORE the first request");
        TEST_CHECK(mgr_cpp.find("UI/HfAdd/022") != std::string::npos,
                   "R4 F-15: a failed pin fails closed (the session dies)");
        TEST_CHECK(mgr_cpp.find("IsAllowedHfDownloadHost(redir_host)") != std::string::npos &&
                       mgr_cpp.find("SplitHfRedirectLocation(ToUtf8(location)") !=
                           std::string::npos,
                   "R4 F-15: every hop re-targets through the allowlist follower");
        TEST_CHECK(mgr_cpp.find("for (int hop = 1; hop < 6 && ok; ++hop)") != std::string::npos,
                   "R4 F-15: the explicit follower is hop-capped (initial request + <=5 hops)");
        TEST_CHECK(mgr_cpp.find("V10F-R1 F-15") != std::string::npos,
                   "R4 F-15: the worker hunks carry the V10F-R1 tag");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R4 F-15 HF redirect follower passed." << std::endl;
    } else {
        std::cout << "[FAIL] R4 F-15 HF redirect follower: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- F-17: the dialog half of the consent-origin persistence ---------------

void TestR4ConsentOriginDialogHalf() {
    std::cout << "[RUN] Testing R4 F-17 consent-origin dialog half..." << std::endl;
    const int failures_before = g_failed_count;

    std::string osw_cpp, osw_hpp, main_cpp;
    R4ReadSrc(L"src\\ui\\openai_settings_window.cpp", osw_cpp);
    R4ReadSrc(L"src\\ui\\openai_settings_window.hpp", osw_hpp);
    R4ReadSrc(L"src\\main.cpp", main_cpp);
    TEST_CHECK(!osw_cpp.empty() && !osw_hpp.empty() && !main_cpp.empty(),
               "R4 F-17: the dialog/main sources resolve for the pins");

    // WM_INITDIALOG seeds from config: the channel first, then the persisted
    // file read, then the legacy base_url derivation.
    TEST_CHECK(osw_cpp.find("st->http_consent_origin = *st->consent_origin_channel;") !=
                   std::string::npos &&
                   osw_cpp.find("persisted.GetHttpConsentOrigin()") != std::string::npos,
               "R4 F-17: WM_INITDIALOG seeds the scope from config (channel, file)");
    TEST_CHECK(osw_cpp.find("V10F-R1 F-17") != std::string::npos,
               "R4 F-17: the dialog hunks carry the V10F-R1 tag");

    // [저장]/IDOK hand the re-scoped origin out; [설정 삭제] clears both the
    // channel and the second-instance persist.
    TEST_CHECK(osw_cpp.find("*st->consent_origin_channel = st->http_consent_origin;") !=
                   std::string::npos,
               "R4 F-17: IDOK hands the consent origin out with the saved block");
    TEST_CHECK(osw_cpp.find("persisted.SetHttpConsentOrigin(\"\");") != std::string::npos,
               "R4 F-17: [설정 삭제] revokes the persisted origin through the verb");
    TEST_CHECK(osw_hpp.find("std::string* http_consent_origin = nullptr") !=
                   std::string::npos,
               "R4 F-17: the library-aware entry carries the in/out channel");

    // main.cpp: the sibling seam persists through the locked verb before the
    // single save; the kDeleted seam revokes; the saved branch fires the seam.
    TEST_CHECK(main_cpp.find("config.SetHttpConsentOrigin(origin);") != std::string::npos,
               "R4 F-17: the saved seam applies the origin through the locked verb");
    TEST_CHECK(main_cpp.find("config.SetHttpConsentOrigin(\"\");") != std::string::npos,
               "R4 F-17: the kDeleted seam revokes the origin through the verb");
    TEST_CHECK(main_cpp.find("g_apply_openai_consent_origin(consent_origin_scope);") !=
                   std::string::npos,
               "R4 F-17: the kSaved branch forwards the dialog's origin");
    TEST_CHECK(main_cpp.find("g_apply_openai_consent_origin = nullptr;") != std::string::npos,
               "R4 F-17: the seam retires at shutdown with the siblings");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R4 F-17 consent-origin dialog half passed." << std::endl;
    } else {
        std::cout << "[FAIL] R4 F-17 consent-origin dialog half: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- F-18: the generation bump at dialog teardown --------------------------

void TestR4FetchGenerationTeardown() {
    std::cout << "[RUN] Testing R4 F-18 fetch-generation teardown..." << std::endl;
    const int failures_before = g_failed_count;

    std::string osw_cpp;
    R4ReadSrc(L"src\\ui\\openai_settings_window.cpp", osw_cpp);
    TEST_CHECK(!osw_cpp.empty(), "R4 F-18: the dialog source resolves for the pins");

    // The bump must live in the WM_DESTROY branch (the BUG-04 profile-switch
    // bump inside ApplyOpenAiProfileSelection is a different site — the p1_u3
    // pin owns that one).
    const size_t destroy_at = osw_cpp.find("case WM_DESTROY:");
    TEST_CHECK(destroy_at != std::string::npos, "R4 F-18: the WM_DESTROY branch is locatable");
    if (destroy_at != std::string::npos) {
        const size_t destroy_end = osw_cpp.find("return TRUE;", destroy_at);
        const std::string branch = osw_cpp.substr(
            destroy_at, destroy_end == std::string::npos ? std::string::npos
                                                         : destroy_end - destroy_at);
        TEST_CHECK(branch.find("++st->fetch_generation;") != std::string::npos,
                   "R4 F-18: WM_DESTROY bumps the fetch generation");
        TEST_CHECK(branch.find("fetch_alive->store(false") != std::string::npos,
                   "R4 F-18: the liveness flip still runs in the same branch");
        TEST_CHECK(branch.find("DrainOpenAiFetchQueue();") != std::string::npos,
                   "R4 F-18: the queue drain still runs after the bump");
        TEST_CHECK(branch.find("V10F-R1 F-18") != std::string::npos,
                   "R4 F-18: the teardown hunk carries the V10F-R1 tag");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R4 F-18 fetch-generation teardown passed." << std::endl;
    } else {
        std::cout << "[FAIL] R4 F-18 fetch-generation teardown: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- ROUND 2 (V10F-R2): F-24/F-25/F-27 — the editor pump + PID gate ---------

void TestR4EditorPumpSingleFlightAndPidGate() {
    std::cout << "[RUN] Testing R4 R2 editor pump single-flight + quit + PID..." << std::endl;
    const int failures_before = g_failed_count;

    std::string editor_cpp, hist_hpp;
    R4ReadSrc(L"src\\ui\\translation_editor_window.cpp", editor_cpp);
    R4ReadSrc(L"src\\translation_history.hpp", hist_hpp);
    TEST_CHECK(!editor_cpp.empty() && !hist_hpp.empty(),
               "R4 R2: the editor/history sources resolve for the pins");

    // F-24: an observed WM_QUIT is re-posted exactly once and the pump BREAKS
    // (the apply aborts, the handler returns); the pre-fix branch re-posted
    // and `continue`d, so the re-posted quit was re-peeked forever and the
    // 300 ms deadline was unreachable.
    TEST_CHECK(editor_cpp.find("quit_seen") != std::string::npos &&
                   editor_cpp.find("::PostQuitMessage(static_cast<int>(msg.wParam));") !=
                       std::string::npos &&
                   editor_cpp.find("UI/TranslationEditor/009") != std::string::npos,
               "R4 R2 F-24: WM_QUIT re-posts once and aborts the pump");
    TEST_CHECK(editor_cpp.find("continue; // never consume the app's WM_QUIT") ==
                   std::string::npos,
               "R4 R2 F-24: the re-post-then-continue spin is gone");
    // The bounded-pump contract itself is untouched.
    TEST_CHECK(editor_cpp.find("PeekMessageW") != std::string::npos &&
                   editor_cpp.find("+ 300") != std::string::npos &&
                   editor_cpp.find("Sleep(10)") == std::string::npos,
               "R4 R2 F-24: the <=300 ms peek-pump deadline contract holds");

    // F-25: the apply path is single-flight and the wait grays BOTH buttons;
    // the pump dispatches nothing addressed to the editor's own dialog (or a
    // child) except the async kMsgResultReady delivery.
    TEST_CHECK(editor_cpp.find("apply_in_progress") != std::string::npos &&
                   editor_cpp.find("ApplySingleFlight") != std::string::npos &&
                   editor_cpp.find("UI/TranslationEditor/008") != std::string::npos,
               "R4 R2 F-25: the apply is single-flight (reentry early-returns)");
    TEST_CHECK(editor_cpp.find("EnableWindow(::GetDlgItem(st.hwnd, kIdApply), FALSE)") !=
                       std::string::npos &&
                   editor_cpp.find("EnableWindow(::GetDlgItem(st.hwnd, kIdRetranslate), FALSE)") !=
                       std::string::npos,
               "R4 R2 F-25: the wait grays Apply AND Retranslate");
    TEST_CHECK(editor_cpp.find("IsChild(st.hwnd, msg.hwnd)") != std::string::npos &&
                   editor_cpp.find("msg.message != kMsgResultReady") != std::string::npos &&
                   editor_cpp.find("::PostMessageW(msg.hwnd, msg.message, msg.wParam, msg.lParam)") !=
                       std::string::npos,
               "R4 R2 F-25: the pump skips (and re-queues) the editor's own "
               "messages but still services the async result");

    // F-27: the apply refuses a 0 PID with a live-looking hwnd, and the owner
    // is re-verified immediately before the paste (after the activation wait).
    TEST_CHECK(editor_cpp.find("UI/TranslationEditor/010") != std::string::npos &&
                   editor_cpp.find("target pid unrecordable") != std::string::npos,
               "R4 R2 F-27: an unrecordable PID refuses the apply (no skip-on-0)");
    TEST_CHECK(editor_cpp.find("UI/TranslationEditor/011") != std::string::npos,
               "R4 R2 F-27: the owner is re-verified after the activation wait");
    TEST_CHECK(R4CountOf(editor_cpp, "GetWindowThreadProcessId(target, &live_pid)") == 2,
               "R4 R2 F-27: the PID is verified at entry AND immediately before the paste");
    TEST_CHECK(editor_cpp.find("live_pid != st.entry.target_pid") != std::string::npos,
               "R4 R2 F-27: both verifications compare against the recorded PID");
    TEST_CHECK(hist_hpp.find("V10F-R2 F-27") != std::string::npos &&
                   editor_cpp.find("V10F-R2") != std::string::npos,
               "R4 R2: the F-24..F-27 hunks carry the V10F-R2 tag");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R4 R2 editor pump single-flight + quit + PID passed." << std::endl;
    } else {
        std::cout << "[FAIL] R4 R2 editor pump single-flight + quit + PID: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- ROUND 2 (V10F-R2): F-26 — the engine gate legacy consent carry ---------

void TestR4EngineLegacyConsentCarry() {
    std::cout << "[RUN] Testing R4 R2 engine legacy consent carry..." << std::endl;
    const int failures_before = g_failed_count;

    std::string engine_cpp;
    R4ReadSrc(L"src\\engine.cpp", engine_cpp);
    TEST_CHECK(!engine_cpp.empty(), "R4 R2 F-26: engine.cpp resolves for the pins");

    // The legacy arm lives INSIDE the F-17 gate lambda and derives the
    // candidate origin from the base_url only when the persisted origin is
    // empty AND the consent flag is set; the original arm stays verbatim
    // (p2_b pins the same two literals).
    const size_t lambda_at = engine_cpp.find("auto openai_http_consent_origin_denied");
    TEST_CHECK(lambda_at != std::string::npos,
               "R4 R2 F-26: the F-17 gate lambda is locatable");
    if (lambda_at != std::string::npos) {
        const size_t lambda_end = engine_cpp.find("};", lambda_at);
        const std::string lambda_body = engine_cpp.substr(
            lambda_at, lambda_end == std::string::npos ? std::string::npos
                                                       : lambda_end - lambda_at);
        TEST_CHECK(lambda_body.find("consent_cfg.GetHttpConsentOrigin().empty()") !=
                           std::string::npos &&
                       lambda_body.find("consent_cfg.openai.http_consent_given") !=
                           std::string::npos &&
                       lambda_body.find("OpenAiHttpConsentScope(openai_cfg.base_url)") !=
                           std::string::npos,
                   "R4 R2 F-26: the legacy arm derives the scope from the base_url "
                   "(empty persisted origin + consent flag only)");
        TEST_CHECK(lambda_body.find("CloudConsentBlocked") == std::string::npos,
                   "R4 R2 F-26: the derivation keeps the denial surfacing at the leg");
    }
    TEST_CHECK(engine_cpp.find("OpenAiHttpConsentOriginMatches(openai_cfg.base_url,") !=
                       std::string::npos &&
                   engine_cpp.find("consent_cfg.GetHttpConsentOrigin())") != std::string::npos,
               "R4 R2 F-26: the original fail-closed arm survives verbatim");
    TEST_CHECK(engine_cpp.find("ui/openai_settings_window.hpp") != std::string::npos,
               "R4 R2 F-26: the gate reuses the dialog's scope helper, not a copy");
    TEST_CHECK(engine_cpp.find("V10F-R2") != std::string::npos,
               "R4 R2 F-26: the hunk carries the V10F-R2 tag");

    // Behavioral: the derivation grammar round-trips through the gate helper.
    TEST_CHECK(emebalachat::OpenAiHttpConsentScope("http://Host:8080/v1") == "host:8080" &&
                   emebalachat::OpenAiHttpConsentScope("http://[::1]:11434") == "[::1]:11434" &&
                   emebalachat::OpenAiHttpConsentScope("http://user@host") == "host:80",
               "R4 R2 F-26: the scope derivation normalizes case/path/userinfo");
    TEST_CHECK(emebalachat::OpenAiHttpConsentScope("https://api.openai.com").empty() &&
                   emebalachat::OpenAiHttpConsentScope("http://host:abc").empty() &&
                   emebalachat::OpenAiHttpConsentScope("").empty(),
               "R4 R2 F-26: https / malformed bases derive no scope (fail closed)");
    TEST_CHECK(OpenAiHttpConsentOriginMatches(
                   "http://host:8080/x?y#z",
                   emebalachat::OpenAiHttpConsentScope("http://host:8080/v1")),
               "R4 R2 F-26: a derived scope passes the request-time gate");
    TEST_CHECK(!OpenAiHttpConsentOriginMatches(
                   "http://other:8080",
                   emebalachat::OpenAiHttpConsentScope("http://host:8080")),
               "R4 R2 F-26: a derived scope from a DIFFERENT base fails the gate");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R4 R2 engine legacy consent carry passed." << std::endl;
    } else {
        std::cout << "[FAIL] R4 R2 engine legacy consent carry: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- ROUND 2 (V10F-R2): F-28 — the model-list GET origin gate ---------------

void TestR4ModelFetchOriginGate() {
    std::cout << "[RUN] Testing R4 R2 model-fetch origin gate..." << std::endl;
    const int failures_before = g_failed_count;

    std::string client_cpp, client_hpp, osw_cpp;
    R4ReadSrc(L"src\\openai_compatible_client.cpp", client_cpp);
    R4ReadSrc(L"src\\openai_compatible_client.hpp", client_hpp);
    R4ReadSrc(L"src\\ui\\openai_settings_window.cpp", osw_cpp);
    TEST_CHECK(!client_cpp.empty() && !client_hpp.empty() && !osw_cpp.empty(),
               "R4 R2 F-28: the client/dialog sources resolve for the pins");

    TEST_CHECK(client_hpp.find("std::string http_consent_origin;") != std::string::npos,
               "R4 R2 F-28: OpenAiConfig carries the request-time origin evidence");
    TEST_CHECK(client_cpp.find(
                   "OpenAiHttpConsentOriginMatches(cfg.base_url, cfg.http_consent_origin)") !=
                       std::string::npos &&
                   client_cpp.find("OPENAI/ListModels/002") != std::string::npos,
               "R4 R2 F-28: the model-list GET is origin-gated like the chat leg");
    // The BUG-07 boolean gates stay EXACTLY two (p1_u4 owns that count); the
    // origin gate is an ADDITIVE arm, not a rewrite.
    TEST_CHECK(R4CountOf(client_cpp, "OpenAiUrlSecurity::Http && !cfg.http_consent_given") == 2,
               "R4 R2 F-28: both BUG-07 boolean gates are untouched");
    TEST_CHECK(osw_cpp.find("probe.http_consent_origin = st->http_consent_origin;") !=
                       std::string::npos &&
                   osw_cpp.find("probe.http_consent_given = st->cfg->http_consent_given;") !=
                       std::string::npos,
               "R4 R2 F-28: the dialog hands the fetch the consented origin with the probe");
    TEST_CHECK(client_cpp.find("V10F-R2") != std::string::npos &&
                   osw_cpp.find("V10F-R2") != std::string::npos,
               "R4 R2 F-28: the hunks carry the V10F-R2 tag");

    // Behavioral fail-closed: a stale boolean alone (empty origin) and a
    // mismatched origin both refuse BEFORE any transport (outputs cleared,
    // nothing listens on port 1, so a pass can only come from the gate).
    {
        emebalachat::OpenAiConfig gate_cfg;
        gate_cfg.base_url = "http://127.0.0.1:1/v1"; // port 1: nothing listens
        gate_cfg.model = "m";
        gate_cfg.http_consent_given = true;          // the stale YES
        std::vector<std::string> ids{"stale-id"};
        std::vector<emebalachat::OpenAiModelCapability> caps(1);
        TEST_CHECK(!emebalachat::OpenAiCompatibleClient::FetchModelsAndCapabilities(
                       gate_cfg, &ids, &caps) &&
                       ids.empty() && caps.empty(),
                   "R4 R2 F-28: http + consent + NO origin fails closed pre-transport");
        gate_cfg.http_consent_origin = "other-host:8080"; // non-consented origin
        ids.assign(1, "stale-id");
        caps.assign(1, emebalachat::OpenAiModelCapability{});
        TEST_CHECK(!emebalachat::OpenAiCompatibleClient::FetchModelsAndCapabilities(
                       gate_cfg, &ids, &caps) &&
                       ids.empty() && caps.empty(),
                   "R4 R2 F-28: http + consent + MISMATCHED origin fails closed pre-transport");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R4 R2 model-fetch origin gate passed." << std::endl;
    } else {
        std::cout << "[FAIL] R4 R2 model-fetch origin gate: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- ROUND 2 (V10F-R2): F-29 — the End payload carries the latest text ------

void TestR4EndCoalescing() {
    std::cout << "[RUN] Testing R4 R2 preview End coalescing..." << std::endl;
    const int failures_before = g_failed_count;

    using emebalachat::TooltipWindow;
    using Payload = TooltipWindow::ChunkPreviewPayload;
    auto begin = [](int total) {
        Payload p;
        p.op = TooltipWindow::ChunkPreviewOp::Begin;
        p.total_chunks = total;
        return p;
    };
    auto append = [](const wchar_t* text) {
        Payload p;
        p.op = TooltipWindow::ChunkPreviewOp::Append;
        p.cumulative = text;
        return p;
    };
    auto end = [](bool success) {
        Payload p;
        p.op = TooltipWindow::ChunkPreviewOp::End;
        p.success = success;
        return p;
    };

    // 1. The F-29 regression itself: the last Append is still queued when End
    //    arrives — the End must carry that final cumulative into the drain.
    {
        std::deque<Payload> q;
        TooltipWindow::CoalesceChunkPreviewPayload(q, begin(2));
        TooltipWindow::CoalesceChunkPreviewPayload(q, append(L"latest"));
        TooltipWindow::CoalesceChunkPreviewPayload(q, end(true));
        TEST_CHECK(q.size() == 2 && q[0].op == TooltipWindow::ChunkPreviewOp::Begin &&
                       q[1].op == TooltipWindow::ChunkPreviewOp::End &&
                       q[1].cumulative == L"latest" && q[1].success,
                   "R4 R2 F-29: an End adopts the newest pending Append's cumulative");
    }
    // 2. The newest of several pending Appends wins the adoption.
    {
        std::deque<Payload> q;
        TooltipWindow::CoalesceChunkPreviewPayload(q, begin(3));
        TooltipWindow::CoalesceChunkPreviewPayload(q, append(L"a"));
        TooltipWindow::CoalesceChunkPreviewPayload(q, append(L"b"));
        TooltipWindow::CoalesceChunkPreviewPayload(q, end(true));
        TEST_CHECK(q.size() == 2 && q[1].op == TooltipWindow::ChunkPreviewOp::End &&
                       q[1].cumulative == L"b",
                   "R4 R2 F-29: the adoption takes the NEWEST pending Append");
    }
    // 3. An End that already carries a cumulative keeps it (no overwrite).
    {
        std::deque<Payload> q;
        Payload explicit_end = end(true);
        explicit_end.cumulative = L"explicit";
        TooltipWindow::CoalesceChunkPreviewPayload(q, begin(1));
        TooltipWindow::CoalesceChunkPreviewPayload(q, append(L"queued"));
        TooltipWindow::CoalesceChunkPreviewPayload(q, explicit_end);
        TEST_CHECK(q.size() == 2 && q[1].op == TooltipWindow::ChunkPreviewOp::End &&
                       q[1].cumulative == L"explicit",
                   "R4 R2 F-29: a non-empty End cumulative is kept verbatim");
    }
    // 4. No pending Append: the End stays empty and the drain keeps the
    //    already-applied body (the pre-F-29 contract for that case).
    {
        std::deque<Payload> q;
        TooltipWindow::CoalesceChunkPreviewPayload(q, begin(2));
        TooltipWindow::CoalesceChunkPreviewPayload(q, end(true));
        TEST_CHECK(q.size() == 2 && q[1].op == TooltipWindow::ChunkPreviewOp::End &&
                       q[1].cumulative.empty(),
                   "R4 R2 F-29: an End with no pending Append stays empty");
    }
    // 5. The adoption does not resurrect a superseded run: a fresh Begin
    //    still replaces everything.
    {
        std::deque<Payload> q;
        TooltipWindow::CoalesceChunkPreviewPayload(q, begin(9));
        TooltipWindow::CoalesceChunkPreviewPayload(q, append(L"stale"));
        TooltipWindow::CoalesceChunkPreviewPayload(q, begin(4));
        TEST_CHECK(q.size() == 1 && q[0].op == TooltipWindow::ChunkPreviewOp::Begin &&
                       q[0].total_chunks == 4,
                   "R4 R2 F-29: a new Begin still replaces the whole pending run");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R4 R2 preview End coalescing passed." << std::endl;
    } else {
        std::cout << "[FAIL] R4 R2 preview End coalescing: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- ROUND 2 (V10F-R2): F-30 — the session-global fetch generation ----------

void TestR4FetchGenerationSessionGlobal() {
    std::cout << "[RUN] Testing R4 R2 fetch-generation session global..." << std::endl;
    const int failures_before = g_failed_count;

    std::string osw_cpp;
    R4ReadSrc(L"src\\ui\\openai_settings_window.cpp", osw_cpp);
    TEST_CHECK(!osw_cpp.empty(), "R4 R2 F-30: the dialog source resolves for the pins");

    TEST_CHECK(osw_cpp.find("std::atomic<unsigned long long>& OpenAiFetchGenerationGlobal()") !=
                       std::string::npos &&
                   osw_cpp.find("static std::atomic<unsigned long long> g{0};") !=
                       std::string::npos,
               "R4 R2 F-30: the generation lives in one session-global atomic");
    TEST_CHECK(osw_cpp.find(
                   "std::atomic<unsigned long long>& fetch_generation = OpenAiFetchGenerationGlobal();") !=
                       std::string::npos &&
                   osw_cpp.find("unsigned long long fetch_generation = 0;") == std::string::npos,
               "R4 R2 F-30: the dialog state REFERENCES the global, not a per-instance copy");
    // All three bump sites keep the pinned literal: the BUG-04 profile switch,
    // the REQ-052 spawn, and the F-18 WM_DESTROY teardown.
    TEST_CHECK(R4CountOf(osw_cpp, "++st->fetch_generation;") == 3,
               "R4 R2 F-30: the profile-switch, spawn and teardown bumps are intact");
    TEST_CHECK(osw_cpp.find("result.generation != st->fetch_generation") != std::string::npos,
               "R4 R2 F-30: the completion filter still compares against the counter");
    TEST_CHECK(osw_cpp.find("a fresh dialog starts at 0") == std::string::npos,
               "R4 R2 F-30: the stale per-instance comment is gone");
    TEST_CHECK(osw_cpp.find("V10F-R2") != std::string::npos,
               "R4 R2 F-30: the hunks carry the V10F-R2 tag");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R4 R2 fetch-generation session global passed." << std::endl;
    } else {
        std::cout << "[FAIL] R4 R2 fetch-generation session global: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- ROUND 2 (V10F-R2): F-31 — the purpose-scoped post-hoc abort scan -------

void TestR4AbortScanPurposeScope() {
    std::cout << "[RUN] Testing R4 R2 abort-scan purpose scope..." << std::endl;
    const int failures_before = g_failed_count;

    std::string client_cpp;
    R4ReadSrc(L"src\\openai_compatible_client.cpp", client_cpp);
    TEST_CHECK(!client_cpp.empty(), "R4 R2 F-31: the client source resolves for the pins");

    TEST_CHECK(client_cpp.find("bool RequestSlotAbortedFor(OpenAiRequestSlotPurpose purpose)") !=
                       std::string::npos &&
                   client_cpp.find("RequestSlotFor(purpose)") != std::string::npos,
               "R4 R2 F-31: the purpose-scoped post-hoc variant exists");
    TEST_CHECK(client_cpp.find("RequestSlotAbortedFor(OpenAiRequestSlotPurpose::Chat)") !=
                       std::string::npos,
               "R4 R2 F-31: the chat post-hoc arm reads the chat slot only");
    TEST_CHECK(client_cpp.find("stream_aborted || RequestSlotAbortedFor(") != std::string::npos,
               "R4 R2 F-31: the post-hoc arm is the scoped call (not the both-slot scan)");
    // The zero-argument consult stays the mid-read authority (p2_a owns these
    // literals); the scope change is post-hoc only.
    TEST_CHECK(R4CountOf(client_cpp,
                         "if (ChunkLoopAbortRequested(abort) || RequestSlotAborted()) {") == 2,
               "R4 R2 F-31: the mid-read zero-arg consults are untouched");
    TEST_CHECK(client_cpp.find("if (!aborted && (ChunkLoopAbortRequested(abort) || "
                               "RequestSlotAborted())) {") != std::string::npos,
               "R4 R2 F-31: the post-loop re-poll keeps its zero-arg shape");
    TEST_CHECK(client_cpp.find("V10F-R2") != std::string::npos,
               "R4 R2 F-31: the hunks carry the V10F-R2 tag");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] R4 R2 abort-scan purpose scope passed." << std::endl;
    } else {
        std::cout << "[FAIL] R4 R2 abort-scan purpose scope: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void R4RegisterTests() {
    TestR4LockedVerbCallSites();
    TestR4BackTranslateHistoryCodes();
    TestR4TypingErrorAutohide();
    TestR4PreviewViewportClamp();
    TestR4PreviewCoalesce();
    TestR4EditorAndHistoryHardening();
    TestR4HfRedirectFollower();
    TestR4ConsentOriginDialogHalf();
    TestR4FetchGenerationTeardown();
    TestR4EditorPumpSingleFlightAndPidGate();
    TestR4EngineLegacyConsentCarry();
    TestR4ModelFetchOriginGate();
    TestR4EndCoalescing();
    TestR4FetchGenerationSessionGlobal();
    TestR4AbortScanPurposeScope();
}

} // namespace r4_fix_tests
