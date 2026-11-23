// ---------------------------------------------------------------------------
// V10F (v0.10.10 phased fixes, unit P2-E): download dialogs + updater UI.
//
// Covered:
//   (a) BUG-10 model-download dialog — Esc (IDCANCEL) routing + the
//       "Cancelling..." state machine. Source pins on
//       src/ui/model_download_dialog.cpp:
//         * WM_COMMAND handles IDCANCEL (the pre-fix proc only knew
//           IDC_MD_LATER/CLOSE/RETRY/DOWNLOAD, so Esc fell through to
//           `return FALSE;`),
//         * the one cancel route (MdRequestCancel) flips the cancel flag,
//           fires the P2-A transport abort (AbortActiveModelDownload) and only
//           THEN paints CancellingProgress — the order is the fix,
//         * kMdMsgDone closes the dialog only after the cancelling state (the
//           worker is provably finished, so OfferModelDownload's join is a
//           no-op instead of a 30 s GUI-thread freeze),
//         * WM_CLOSE takes the same route (no bare EndDialog), repeated
//           Esc/[X] are swallowed, and a retry cannot start mid-unwind.
//   (b) RISK-02 About-window updater cancel — the pure decision pair plus the
//       wiring pins on src/ui/about_window.cpp/.hpp:
//         * ShouldRequestDownloadCancel() / DownloadZoneLabelId() truth table,
//         * RequestDownloadCancel flips cancel -> AbortCurrent -> repaint,
//         * the Downloading render swaps the label to CancellingProgress and
//           drops the [Cancel] affordance (empty rect = not a hit),
//         * StartDownload publishes the P2-A DownloadAbortSlot into
//           DownloadOptions::abort before the worker thread exists,
//         * Destroy() aborts the in-flight handle before dropping the job.
//
// Source pins read through ResolveRepoFile + raw-byte find() (CRLF-normalized,
// the req052/p1_u2 precedent; every needle is ASCII-only).
// ---------------------------------------------------------------------------

namespace p2_e_tests {

// Resolve + read a production source for the P2-E structural pins.
void P2EReadSrc(const wchar_t* rel, std::string& out) {
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

// Slice [from_marker, next_marker) — empty when either end is missing.
std::string P2ESlice(const std::string& src, const char* from_marker, const char* to_marker) {
    const size_t a = src.find(from_marker);
    if (a == std::string::npos) return std::string();
    const size_t b = src.find(to_marker, a + 1);
    return src.substr(a, (b == std::string::npos ? src.size() : b) - a);
}

// ---- (a) BUG-10: model-download dialog cancel routing ---------------------

void TestP2EModelDownloadDialogCancel() {
    std::cout << "[RUN] Testing P2-E BUG-10 model-download dialog cancel (V10F)..." << std::endl;
    const int failures_before = g_failed_count;

    std::string dlg;
    P2EReadSrc(L"src\\ui\\model_download_dialog.cpp", dlg);
    if (dlg.empty()) {
        std::cout << "[SKIP] model_download_dialog.cpp not resolvable; P2-E pins skipped."
                  << std::endl;
        return;
    }

    // The phase enum grew the Cancelling state (the wait-for-done surface).
    TEST_CHECK(dlg.find("Cancelling,") != std::string::npos &&
                   dlg.find("enum class MdPhase {") != std::string::npos,
               "P2-E pin: MdPhase carries the Cancelling state");

    // One cancel route exists and is shared by Esc / [X] / Later.
    const std::string cancel_fn =
        P2ESlice(dlg, "void MdRequestCancel(HWND dlg, MdDialogState* st) {", "\nINT_PTR CALLBACK");
    TEST_CHECK(!cancel_fn.empty(), "P2-E pin: MdRequestCancel is locatable");
    if (!cancel_fn.empty()) {
        const size_t guard = cancel_fn.find("if (st && st->phase == MdPhase::Cancelling) {");
        const size_t flag = cancel_fn.find("st->cancel.store(true");
        const size_t abort = cancel_fn.find("modeldownloader::AbortActiveModelDownload();");
        const size_t body = cancel_fn.find("MdSetBody(dlg, I18n::Get(StringId::CancellingProgress));");
        const size_t repaint = cancel_fn.find("::InvalidateRect(dlg, nullptr, TRUE);");
        // Repeat-request guard first: a second Esc/[X] must not re-issue the
        // transport abort or re-enter EndDialog while the worker unwinds.
        TEST_CHECK(guard != std::string::npos && flag != std::string::npos &&
                       guard < flag,
                   "P2-E pin: repeated cancel is swallowed before any state change");
        // BUG-10 order: flag -> socket close -> the Cancelling surface.
        TEST_CHECK(flag != std::string::npos && abort != std::string::npos && body != std::string::npos &&
                       flag < abort && abort < body,
                   "P2-E pin: cancel flag then AbortActiveModelDownload then the "
                   "CancellingProgress surface (socket closed BEFORE the wait)");
        TEST_CHECK(body != std::string::npos && repaint != std::string::npos && body < repaint,
                   "P2-E pin: the cancelling state is painted (invalidate after the label swap)");
        // The consent / result path still closes quietly (no worker to join).
        TEST_CHECK(cancel_fn.find("st->phase != MdPhase::Progress") != std::string::npos &&
                       cancel_fn.find("::EndDialog(dlg, IDCANCEL);") != std::string::npos,
                   "P2-E pin: non-progress phases keep the historical quiet close");
    }

    // Esc now reaches the cancel route (the pre-fix bug: no IDCANCEL arm).
    const std::string cmd = P2ESlice(dlg, "case WM_COMMAND: {", "case WM_CTLCOLORSTATIC:");
    TEST_CHECK(!cmd.empty(), "P2-E pin: the WM_COMMAND arm is locatable");
    if (!cmd.empty()) {
        const size_t esc = cmd.find("if (id == IDCANCEL) {");
        const size_t esc_route = (esc == std::string::npos)
                                     ? std::string::npos
                                     : cmd.find("MdRequestCancel(dlg, st);", esc);
        const size_t later = cmd.find("if (id == IDC_MD_LATER) {");
        const size_t later_route = (later == std::string::npos)
                                       ? std::string::npos
                                       : cmd.find("MdRequestCancel(dlg, st);", later);
        TEST_CHECK(esc != std::string::npos && esc_route != std::string::npos,
                   "P2-E pin: IDCANCEL (Esc) routes to MdRequestCancel (BUG-10 root cause)");
        TEST_CHECK(later != std::string::npos && later_route != std::string::npos,
                   "P2-E pin: [Later] takes the same single route");
        // A retry must not overwrite a joinable/unwinding worker.
        const size_t download = cmd.find("if (id == IDC_MD_DOWNLOAD) {");
        const size_t retry_guard =
            (download == std::string::npos) ? std::string::npos
                                            : cmd.find("if (st->phase == MdPhase::Cancelling) {",
                                                       download);
        const size_t join = (download == std::string::npos) ? std::string::npos
                                                           : cmd.find("if (st->worker.joinable())", download);
        TEST_CHECK(retry_guard != std::string::npos && join != std::string::npos &&
                       retry_guard < join,
                   "P2-E pin: a retry is refused while the previous worker unwinds");
    }

    // [X] (WM_CLOSE) no longer EndDialogs on a live worker.
    const std::string close_arm = P2ESlice(dlg, "case WM_CLOSE: {", "\n    }\n    return FALSE;");
    TEST_CHECK(!close_arm.empty(), "P2-E pin: the WM_CLOSE arm is locatable");
    if (!close_arm.empty()) {
        TEST_CHECK(close_arm.find("MdRequestCancel(dlg, st);") != std::string::npos &&
                       close_arm.find("st->cancel.store(true)") == std::string::npos,
                   "P2-E pin: [X] routes through the cancelling state (no bare close+join)");
    }

    // kMdMsgDone is the only close of a cancelled transfer, and it comes after
    // the cancelling check (so the Result-phase rendering is skipped).
    const std::string done = P2ESlice(dlg, "case kMdMsgDone: {", "case WM_COMMAND: {");
    TEST_CHECK(!done.empty(), "P2-E pin: the kMdMsgDone arm is locatable");
    if (!done.empty()) {
        const size_t phase_check = done.find("if (st->phase == MdPhase::Cancelling) {");
        const size_t close = (phase_check == std::string::npos)
                                 ? std::string::npos
                                 : done.find("::EndDialog(dlg, IDCANCEL);", phase_check);
        const size_t result = done.find("st->phase = MdPhase::Result;");
        TEST_CHECK(phase_check != std::string::npos && close != std::string::npos,
                   "P2-E pin: kMdMsgDone closes a cancelled transfer (exit join is a no-op)");
        TEST_CHECK(close != std::string::npos && result != std::string::npos && close < result,
                   "P2-E pin: the cancelling exit precedes the Result-phase paint");
    }

    // The progress bar stays visible (frozen) in the cancelling state.
    TEST_CHECK(dlg.find("phase == MdPhase::Progress || phase == MdPhase::Cancelling ? SW : HD") !=
                   std::string::npos,
               "P2-E pin: the cancelling state keeps the progress bar and drops the buttons");

    // Worker progress / phase posts must not repaint over the cancelling
    // surface (a Verifying label would otherwise replace "Cancelling...").
    const std::string progress_arm = P2ESlice(dlg, "case kMdMsgProgress: {", "case kMdMsgPhase: {");
    const std::string phase_arm = P2ESlice(dlg, "case kMdMsgPhase: {", "case kMdMsgDone: {");
    TEST_CHECK(!progress_arm.empty() && !phase_arm.empty(),
               "P2-E pin: the progress/phase arms are locatable");
    TEST_CHECK(!progress_arm.empty() &&
                   progress_arm.find("if (st && st->phase == MdPhase::Cancelling) return TRUE;") !=
                       std::string::npos,
               "P2-E pin: progress posts keep the bar frozen while unwinding");
    TEST_CHECK(!phase_arm.empty() &&
                   phase_arm.find("if (st && st->phase == MdPhase::Cancelling) return TRUE;") !=
                       std::string::npos,
               "P2-E pin: phase labels never paint over the CancellingProgress body");

    // Existing contracts stay: one owner discipline + the exit join.
    TEST_CHECK(dlg.find("if (st->worker.joinable())") != std::string::npos &&
                   dlg.find("if (st->worker.joinable())") < dlg.find("st->worker = std::thread("),
               "P2-E pin: the Retry path still joins before re-creating the worker");
    TEST_CHECK(dlg.find("worker.detach()") == std::string::npos &&
                   dlg.find("st.worker.join()") != std::string::npos,
               "P2-E pin: no detach; the dialog-exit join remains");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-E BUG-10 model-download dialog cancel pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-E BUG-10 model-download dialog cancel pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (b) RISK-02: About-window download cancel, pure decisions ------------

void TestP2EAboutDownloadCancelPure() {
    std::cout << "[RUN] Testing P2-E RISK-02 About download-cancel pure decisions (V10F)..."
              << std::endl;
    const int failures_before = g_failed_count;

    using emebalachat::AboutWindow;
    using Z = AboutWindow::UpdateZoneState;

    // Cancel is requestable ONLY while a transfer is in flight and has not
    // begun unwinding — the second click must not re-issue the abort.
    TEST_CHECK(AboutWindow::ShouldRequestDownloadCancel(Z::Downloading, false),
               "P2-E: Downloading + not cancelling -> cancel is requestable");
    TEST_CHECK(!AboutWindow::ShouldRequestDownloadCancel(Z::Downloading, true),
               "P2-E: Downloading + already cancelling -> idempotent no-op");
    const Z others[] = {Z::Idle,      Z::Checking,  Z::UpToDate, Z::Available, Z::Ready,
                        Z::Snoozed,   Z::Cancelled, Z::ErrOffline, Z::ErrRate, Z::ErrHash};
    bool others_ok = true;
    for (const Z state : others) {
        if (AboutWindow::ShouldRequestDownloadCancel(state, false)) others_ok = false;
        if (AboutWindow::ShouldRequestDownloadCancel(state, true)) others_ok = false;
    }
    TEST_CHECK(others_ok,
               "P2-E: every non-Downloading zone refuses the cancel request");

    // The label swap uses two EXISTING ids (no new localization surface).
    TEST_CHECK(AboutWindow::DownloadZoneLabelId(false) == emebalachat::StringId::UpdateDlProgress,
               "P2-E: the progress label stays UpdateDlProgress before the cancel");
    TEST_CHECK(AboutWindow::DownloadZoneLabelId(true) == emebalachat::StringId::CancellingProgress,
               "P2-E: the cancelling label is CancellingProgress (existing StringId)");
    TEST_CHECK(AboutWindow::DownloadZoneLabelId(false) != AboutWindow::DownloadZoneLabelId(true),
               "P2-E: the two label states are distinct");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-E About download-cancel pure decisions passed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-E About download-cancel pure decisions: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (b) RISK-02: About-window download cancel, wiring pins ---------------

void TestP2EAboutDownloadCancelWiring() {
    std::cout << "[RUN] Testing P2-E RISK-02 About download-cancel wiring (V10F)..." << std::endl;
    const int failures_before = g_failed_count;

    std::string abt;
    std::string abtH;
    P2EReadSrc(L"src\\ui\\about_window.cpp", abt);
    P2EReadSrc(L"src\\ui\\about_window.hpp", abtH);
    if (abt.empty() || abtH.empty()) {
        std::cout << "[SKIP] about_window.cpp/.hpp not resolvable; P2-E About pins skipped."
                  << std::endl;
        return;
    }

    // The abort seam names the P2-A API: a heap-owned DownloadAbortSlot
    // published into DownloadOptions::abort, aborted through AbortCurrent().
    TEST_CHECK(abt.find("std::make_shared<updatelogic::DownloadAbortSlot>()") != std::string::npos &&
                   abt.find("opts.abort = seam.slot.get();") != std::string::npos &&
                   abt.find("slot->AbortCurrent();") != std::string::npos,
               "P2-E pin: the seam publishes DownloadAbortSlot and aborts via AbortCurrent");

    const std::string start =
        P2ESlice(abt, "void AboutWindow::StartDownload() {", "\nvoid AboutWindow::");
    TEST_CHECK(!start.empty(), "P2-E pin: StartDownload is locatable");
    if (!start.empty()) {
        const size_t opts = start.find("updatelogic::DownloadOptions opts;");
        const size_t seam = start.find("WireDownloadAbortSlot(opts);");
        const size_t park = start.find("job->abort = abort_seam.request;");
        const size_t spawn = start.find("std::thread([hwnd, job, url, dest, expected, advertised_size, opts]() {");
        TEST_CHECK(opts != std::string::npos && seam != std::string::npos && park != std::string::npos &&
                       spawn != std::string::npos,
                   "P2-E pin: the abort slot is wired before the worker starts");
        TEST_CHECK(seam != std::string::npos && park != std::string::npos && spawn != std::string::npos &&
                       seam < park && park < spawn,
                   "P2-E pin: publish -> park the request -> only then spawn (no race)");
        TEST_CHECK(start.find("job->cancel, opts);") != std::string::npos,
                   "P2-E pin: the options (with the slot) reach DownloadUpdate");
    }

    const std::string cancel =
        P2ESlice(abt, "void AboutWindow::RequestDownloadCancel() {", "\nvoid AboutWindow::");
    TEST_CHECK(!cancel.empty(), "P2-E pin: RequestDownloadCancel is locatable");
    if (!cancel.empty()) {
        const size_t gate = cancel.find("if (!ShouldRequestDownloadCancel(zone_state_, download_cancelling_)) return;");
        const size_t flag = cancel.find("job->cancel.store(true");
        const size_t abort = cancel.find("if (job->abort) job->abort();");
        const size_t paint = cancel.find("Render();");
        TEST_CHECK(gate != std::string::npos,
                   "P2-E pin: the request is gated by the pure decision");
        TEST_CHECK(flag != std::string::npos && abort != std::string::npos && paint != std::string::npos &&
                       flag < abort && abort < paint,
                   "P2-E pin: flag -> handle-close abort -> repaint (abort before any wait)");
    }

    // The Downloading render swaps the label through the pure selector and
    // drops the [Cancel] affordance (empty rect is not a hit).
    TEST_CHECK(abt.find("I18n::Get(DownloadZoneLabelId(download_cancelling_))") != std::string::npos,
               "P2-E pin: the zone label renders through DownloadZoneLabelId");
    TEST_CHECK(abt.find("dl_cancel_rect_ = download_cancelling_") != std::string::npos &&
                   abt.find("? D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f)") != std::string::npos,
               "P2-E pin: the cancelling zone drops the [Cancel] hit rect");
    TEST_CHECK(abt.find("if (!download_cancelling_ && uz_textbtn_format_) {") != std::string::npos,
               "P2-E pin: the cancel label is not painted while unwinding");

    // A new transfer always starts live.
    const std::string set_state =
        P2ESlice(abt, "void AboutWindow::SetZoneState(UpdateZoneState state) {", "\nvoid AboutWindow::");
    TEST_CHECK(!set_state.empty(), "P2-E pin: SetZoneState is locatable");
    if (!set_state.empty()) {
        const size_t reset = set_state.find("download_cancelling_ = false;");
        const size_t checking = set_state.find("if (state == UpdateZoneState::Checking) {");
        TEST_CHECK(reset != std::string::npos && checking != std::string::npos && reset < checking,
                   "P2-E pin: SetZoneState clears the cancelling sub-state on entry");
    }

    // Click routing uses the pure gate, not a raw rect test.
    TEST_CHECK(abt.find("AboutWindow::ShouldRequestDownloadCancel(pThis->zone_state_,") != std::string::npos &&
                   abt.find("pThis->RequestDownloadCancel();") != std::string::npos,
               "P2-E pin: WM_LBUTTONUP routes the [Cancel] click through the gate");

    // Destroy() closes the socket before dropping the job (app exit).
    const std::string destroy = P2ESlice(abt, "void AboutWindow::Destroy() {", "\nvoid AboutWindow::");
    TEST_CHECK(!destroy.empty(), "P2-E pin: Destroy is locatable");
    if (!destroy.empty()) {
        const size_t flag = destroy.find("job_->cancel.store(true");
        const size_t abort = destroy.find("if (job_->abort) job_->abort();");
        const size_t reset = destroy.find("job_.reset();");
        TEST_CHECK(flag != std::string::npos && abort != std::string::npos && reset != std::string::npos &&
                       flag < abort && abort < reset,
                   "P2-E pin: Destroy aborts the in-flight handle before dropping the job");
    }

    // Header contract: the job carries the abort request, the window carries
    // the sub-state, and the seam method exists.
    TEST_CHECK(abtH.find("std::function<void()> abort;") != std::string::npos,
               "P2-E pin: UpdateJob carries the abort request");
    TEST_CHECK(abtH.find("bool download_cancelling_ = false;") != std::string::npos,
               "P2-E pin: the window carries the cancelling sub-state");
    TEST_CHECK(abtH.find("void RequestDownloadCancel();") != std::string::npos &&
                   abtH.find("static bool ShouldRequestDownloadCancel(UpdateZoneState state, bool cancelling);") !=
                       std::string::npos &&
                   abtH.find("static StringId DownloadZoneLabelId(bool cancelling);") != std::string::npos,
               "P2-E pin: the seam + pure helpers are declared in the header");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P2-E About download-cancel wiring pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] P2-E About download-cancel wiring pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

void P2ERegisterTests() {
    TestP2EModelDownloadDialogCancel();
    TestP2EAboutDownloadCancelPure();
    TestP2EAboutDownloadCancelWiring();
}

} // namespace p2_e_tests
