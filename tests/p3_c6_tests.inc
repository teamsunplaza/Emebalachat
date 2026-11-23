// ---------------------------------------------------------------------------
// 261122_0002 P3-C6 (v0.10.10 phased fixes, wave A unit P3-C6: About window +
// updater).
//
//   Fix 1 / ABT-U1    AboutWindow::Dismiss() resets an UpToDate zone to Idle.
//                     Dismiss kills the 6 s auto-return timer, so the expired
//                     "up to date" panel used to survive the hide and come
//                     back on the next Show (only a click inside the old zone
//                     band could clear it).
//   Fix 2 / ABT-U2(1) The footer reset pill is gated on the zone state in the
//                     cursor / hover / click paths, so the Available consent
//                     scrim can no longer be clicked through into the
//                     destructive-reset MessageBox (theme_rects_ already had
//                     that gate).
//   Fix 3 / ABT-U2(2) Enter AND Space dispatch the consent dialog's FOCUSED
//                     button (dlg_focus_ 0 = Later, 1 = Download), matching
//                     the ring Render paints. Pre-fix Enter always downloaded
//                     and Space was swallowed.
//   Fix 4 / ABT-U2(3) The Downloading zone paints its progress label once. The
//                     duplicate below the bar overlapped the meta line
//                     ("336 / 700 MB") by 84 DIP in every locale.
//   Fix 5 / ABT-U3    DownloadUpdate() fails closed BEFORE the output file
//                     exists and before any socket opens when the release
//                     notes carry no parseable SHA256 line; pre-fix it
//                     streamed the whole ~700 MB installer and deleted it at
//                     the digest gate.
//
// Coverage:
//   (a) TestP3C6AboutPureDecisions
//         The three new pure folds (ShouldResetZoneOnDismiss /
//         FooterResetInteractive / ConsentConfirmIsDownload) driven over the
//         full 11-state truth table, plus the P2-E label-id decision they sit
//         next to as a regression guard (that pair's pins must not drift).
//   (b) TestP3C6AboutStructuralPins
//         Source pins on src/ui/about_window.cpp/.hpp: the reset call inside
//         Dismiss (and its ordering after the timer kill / visible_ = false),
//         the three gated reset-rect sites (count + gate-before-rect shape,
//         so neither dropping a gate nor adding an ungated site survives),
//         the VK_RETURN/VK_SPACE dispatch through dlg_focus_, the single
//         progress-label draw with its 67 SetColor budget, and the three
//         header declarations.
//   (c) TestP3C6UpdateCheckerEarlyHashGate
//         A REAL DownloadUpdate call with a nullopt expected hash (returns
//         HashMismatch with no file created, no network), two ordering
//         controls (cancel wins first; an engaged hash still reaches the URL
//         gate), plus the ordering pins inside the function body.
//
// Staged as an .inc next to this runner; registered from main() per the
// end-of-file pattern. The structural pins read the production sources with
// ResolveRepoFile + a CRLF-normalized raw read (the P2-E / P1-U1 precedent);
// every needle is ASCII-only. The functional leg needs no socket: the early
// gate it proves sits before every WinHTTP call.
// ---------------------------------------------------------------------------

namespace p3_c6_tests {

// Resolve + read a production source for the structural pins (P2-E pattern).
void P3C6ReadSrc(const wchar_t* rel, std::string& out) {
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
std::string P3C6Slice(const std::string& src, const char* from_marker, const char* to_marker) {
    const size_t a = src.find(from_marker);
    if (a == std::string::npos) return std::string();
    const size_t b = src.find(to_marker, a + 1);
    return src.substr(a, (b == std::string::npos ? src.size() : b) - a);
}

int P3C6CountOf(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    int n = 0;
    for (size_t pos = hay.find(needle); pos != std::string::npos;
         pos = hay.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

// Function body = [marker, first column-0 `}` after it) — the W5 pin shape.
std::string P3C6FuncBody(const std::string& src, const char* marker) {
    const size_t i = src.find(marker);
    if (i == std::string::npos) return std::string();
    const size_t j = src.find("\n}", i);
    return src.substr(i, (j == std::string::npos ? src.size() : j) - i);
}

// ---- (a) the pure decisions ----------------------------------------------

void TestP3C6AboutPureDecisions() {
    std::cout << "[RUN] Testing P3-C6 About updater pure decisions..." << std::endl;
    const int failures_before = g_failed_count;

    using emebalachat::AboutWindow;
    using Z = AboutWindow::UpdateZoneState;

    // ABT-U1: ONLY the UpToDate status is transient enough to lose its meaning
    // on a hide. Every other state is user intent (Snoozed / Cancelled / the
    // retry arms) or a live job, and must survive a Dismiss.
    TEST_CHECK(AboutWindow::ShouldResetZoneOnDismiss(Z::UpToDate),
               "P3-C6 ABT-U1: a dismissed UpToDate zone resets to Idle");
    const Z others[] = {Z::Idle,      Z::Checking,  Z::Available, Z::Downloading, Z::Ready,
                        Z::Snoozed,   Z::Cancelled, Z::ErrOffline, Z::ErrRate,     Z::ErrHash};
    bool others_ok = true;
    for (const Z state : others) {
        if (AboutWindow::ShouldResetZoneOnDismiss(state)) {
            others_ok = false;
            std::cout << "  (unexpected reset decision for zone state "
                      << static_cast<int>(state) << ")" << std::endl;
        }
    }
    TEST_CHECK(others_ok,
               "P3-C6 ABT-U1: the other ten zone states keep their meaning across a hide");
    TEST_CHECK(!AboutWindow::ShouldResetZoneOnDismiss(Z::Downloading),
               "P3-C6 ABT-U1: an in-flight download is never reset by Dismiss");

    // ABT-U2(1): the consent scrim owns the card, so the footer reset pill is
    // dead there and live everywhere else (including Idle).
    TEST_CHECK(!AboutWindow::FooterResetInteractive(Z::Available),
               "P3-C6 ABT-U2(1): the reset pill is NOT hit under the consent scrim");
    bool footer_ok = true;
    for (const Z state : others) {
        if (state == Z::Available) continue;
        if (!AboutWindow::FooterResetInteractive(state)) footer_ok = false;
    }
    TEST_CHECK(footer_ok,
               "P3-C6 ABT-U2(1): every non-consent zone keeps the reset pill live");
    TEST_CHECK(AboutWindow::FooterResetInteractive(Z::Idle),
               "P3-C6 ABT-U2(1): the Idle intro keeps the historical reset button");

    // ABT-U2(2): dlg_focus_ 0 = Later, 1 = Download (the ring Render paints).
    TEST_CHECK(!AboutWindow::ConsentConfirmIsDownload(0),
               "P3-C6 ABT-U2(2): focus on [Later] confirms Later, not the download");
    TEST_CHECK(AboutWindow::ConsentConfirmIsDownload(1),
               "P3-C6 ABT-U2(2): focus on [Download] keeps Enter == Download");
    TEST_CHECK(AboutWindow::ConsentConfirmIsDownload(0) !=
                   AboutWindow::ConsentConfirmIsDownload(1),
               "P3-C6 ABT-U2(2): the two focus positions dispatch differently");

    // Regression guard for the P2-E decision this suite sits next to: both
    // existing ids, no new localization surface, distinct labels.
    TEST_CHECK(AboutWindow::DownloadZoneLabelId(false) == emebalachat::StringId::UpdateDlProgress,
               "P3-C6: the progress label stays UpdateDlProgress before a cancel");
    TEST_CHECK(AboutWindow::DownloadZoneLabelId(true) == emebalachat::StringId::CancellingProgress,
               "P3-C6: the cancelling label stays CancellingProgress");
    TEST_CHECK(AboutWindow::DownloadZoneLabelId(false) != AboutWindow::DownloadZoneLabelId(true),
               "P3-C6: the two download labels stay distinct");
    TEST_CHECK(AboutWindow::ShouldRequestDownloadCancel(Z::Downloading, false) &&
                   !AboutWindow::ShouldRequestDownloadCancel(Z::Downloading, true),
               "P3-C6: the P2-E cancel gate is unchanged (one abort per transfer)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C6 About updater pure decisions passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C6 About updater pure decisions: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (b) about_window.cpp/.hpp structural pins ---------------------------

void TestP3C6AboutStructuralPins() {
    std::cout << "[RUN] Testing P3-C6 About structural pins (ABT-U1/U2)..." << std::endl;
    const int failures_before = g_failed_count;

    std::string abt;
    std::string abtH;
    P3C6ReadSrc(L"src\\ui\\about_window.cpp", abt);
    P3C6ReadSrc(L"src\\ui\\about_window.hpp", abtH);
    if (abt.empty() || abtH.empty()) {
        std::cout << "[SKIP] about_window.cpp/.hpp not resolvable; P3-C6 About pins skipped."
                  << std::endl;
        return;
    }

    // ---- ABT-U1: the reset lives in Dismiss, after the timer kill ----------
    const std::string dismiss =
        P3C6Slice(abt, "void AboutWindow::Dismiss() {", "\nvoid AboutWindow::");
    TEST_CHECK(!dismiss.empty(), "P3-C6: AboutWindow::Dismiss is locatable");
    if (!dismiss.empty()) {
        const size_t kill = dismiss.find("::KillTimer(hwnd_, kUpToDateReturnTimerId);");
        const size_t hide = dismiss.find("visible_ = false;");
        const size_t gate = dismiss.find("if (ShouldResetZoneOnDismiss(zone_state_)) {");
        const size_t reset = dismiss.find("SetZoneState(UpdateZoneState::Idle);");
        TEST_CHECK(gate != std::string::npos && reset != std::string::npos,
                   "P3-C6 ABT-U1: Dismiss resets the UpToDate zone through the pure decision");
        TEST_CHECK(kill != std::string::npos && gate != std::string::npos && kill < gate,
                   "P3-C6 ABT-U1: the reset follows the auto-return timer kill (no orphan timer)");
        TEST_CHECK(hide != std::string::npos && reset != std::string::npos && hide < reset,
                   "P3-C6 ABT-U1: the transition runs after visible_ = false (never repaints the "
                   "window that is about to hide)");
        TEST_CHECK(dismiss.find("V10F/ABT-U1") != std::string::npos,
                   "P3-C6 ABT-U1: the hunk carries its V10F/ABT-U1 tag");
    }
    TEST_CHECK(abt.find("bool AboutWindow::ShouldResetZoneOnDismiss(UpdateZoneState state) {") !=
                   std::string::npos,
               "P3-C6 ABT-U1: the decision is defined out of line on the state alone");

    // ---- ABT-U2(1): three gated reset-rect sites, no ungated one -----------
    // The count pins are the mutation killers: dropping a gate from one path
    // lowers the gate count, and adding a raw rect test raises the rect count.
    TEST_CHECK(P3C6CountOf(abt, "FooterResetInteractive(pThis->zone_state_)") == 3,
               "P3-C6 ABT-U2(1): exactly three reset-pill sites ask the pure gate "
               "(WM_SETCURSOR + WM_MOUSEMOVE + WM_LBUTTONUP)");
    TEST_CHECK(P3C6CountOf(abt, "IsPointInRect(pThis->reset_rect_") == 3,
               "P3-C6 ABT-U2(1): exactly three reset-rect hit tests remain (no ungated fourth)");
    TEST_CHECK(abt.find("V10F/ABT-U2") != std::string::npos,
               "P3-C6 ABT-U2: the hunks carry their V10F/ABT-U2 tag");

    const std::string cursor =
        P3C6Slice(abt, "case WM_SETCURSOR: {", "case WM_MOUSEMOVE: {");
    TEST_CHECK(!cursor.empty() && cursor.find("FooterResetInteractive(pThis->zone_state_) &&") !=
                                     std::string::npos &&
                   cursor.find("IsPointInRect(pThis->reset_rect_, x, y)") != std::string::npos,
               "P3-C6 ABT-U2(1): the hand cursor skips the pill under the scrim");
    if (!cursor.empty()) {
        const size_t gate = cursor.find("FooterResetInteractive(");
        const size_t rect = cursor.find("IsPointInRect(pThis->reset_rect_, x, y)");
        TEST_CHECK(gate != std::string::npos && rect != std::string::npos && gate < rect,
                   "P3-C6 ABT-U2(1): the cursor gate binds BEFORE the rect test (short-circuit)");
    }

    const std::string move =
        P3C6Slice(abt, "case WM_MOUSEMOVE: {", "case WM_MOUSELEAVE: {");
    TEST_CHECK(!move.empty() && move.find("FooterResetInteractive(pThis->zone_state_) &&") !=
                                     std::string::npos &&
                   move.find("IsPointInRect(pThis->reset_rect_, x, y)") != std::string::npos,
               "P3-C6 ABT-U2(1): no hover paint on the pill under the scrim");
    TEST_CHECK(P3C6CountOf(abt, "hover = kHoverReset;") == 1,
               "P3-C6 ABT-U2(1): exactly one kHoverReset assignment survives");

    const std::string lbuttonup =
        P3C6Slice(abt, "case WM_LBUTTONUP: {", "case WM_KEYDOWN: {");
    TEST_CHECK(!lbuttonup.empty() &&
                   lbuttonup.find("if (FooterResetInteractive(pThis->zone_state_) &&") !=
                       std::string::npos &&
                   lbuttonup.find("IsPointInRect(pThis->reset_rect_, x, y)) {") !=
                       std::string::npos,
               "P3-C6 ABT-U2(1): the reset click is refused under the consent dialog");
    TEST_CHECK(lbuttonup.find("StringId::AboutResetButton") != std::string::npos &&
                   lbuttonup.find("MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND") !=
                       std::string::npos,
               "P3-C6 ABT-U2(1): the REQ-052 confirm gate itself is untouched");

    // ---- ABT-U2(2): Enter + Space follow dlg_focus_ -----------------------
    const std::string keydown =
        P3C6Slice(abt, "case WM_KEYDOWN: {", "case WM_TIMER: {");
    TEST_CHECK(!keydown.empty(), "P3-C6: the WM_KEYDOWN arm is locatable");
    if (!keydown.empty()) {
        const size_t confirm = keydown.find("if (wParam == VK_RETURN || wParam == VK_SPACE) {");
        const size_t dispatch = keydown.find("if (ConsentConfirmIsDownload(pThis->dlg_focus_)) {");
        const size_t download = keydown.find("pThis->StartDownload();");
        // Search the Later arm INSIDE the confirm block: the Esc arm above it
        // also snoozes, so a file-wide find() would read that one.
        const size_t later =
            (confirm == std::string::npos)
                ? std::string::npos
                : keydown.find("pThis->SetZoneState(UpdateZoneState::Snoozed);", confirm);
        TEST_CHECK(confirm != std::string::npos && dispatch != std::string::npos,
                   "P3-C6 ABT-U2(2): Enter and Space share the dlg_focus_ dispatch");
        TEST_CHECK(confirm != std::string::npos && dispatch > confirm,
                   "P3-C6 ABT-U2(2): the dispatch sits inside the confirm-key arm");
        TEST_CHECK(download != std::string::npos && later != std::string::npos &&
                       download < later,
                   "P3-C6 ABT-U2(2): both arms survive (Download starts, Later snoozes)");
        TEST_CHECK(keydown.find("if (wParam == VK_RETURN) {") == std::string::npos,
                   "P3-C6 ABT-U2(2): the pre-fix unconditional Enter arm is gone");
        TEST_CHECK(keydown.find("if (wParam == VK_SPACE) {") == std::string::npos,
               "P3-C6 ABT-U2(2): Space is not a separate (swallowed) arm");
        TEST_CHECK(keydown.find("return 0; // swallow other keys while the dialog is up") !=
                       std::string::npos,
                   "P3-C6 ABT-U2(2): the modal arm still swallows every other key");
        const size_t tab = keydown.find("if (wParam == VK_TAB) {");
        TEST_CHECK(tab != std::string::npos &&
                       keydown.find("(pThis->dlg_focus_ == 0) ? 1 : 0", tab) != std::string::npos,
                   "P3-C6 ABT-U2(2): Tab still cycles the focus the confirm keys dispatch on");
    }

    // ---- ABT-U2(3): one progress-label draw, no duplicate meta-row label --
    const std::string downloading = P3C6Slice(abt, "case UpdateZoneState::Downloading: {",
                                              "case UpdateZoneState::Ready: {");
    TEST_CHECK(!downloading.empty(), "P3-C6: the Downloading zone body is locatable");
    if (!downloading.empty()) {
        TEST_CHECK(P3C6CountOf(downloading, "DrawText(label.c_str()") == 1,
                   "P3-C6 ABT-U2(3): the progress label is painted exactly once");
        TEST_CHECK(P3C6CountOf(downloading, "uz_label_format_") == 1,
                   "P3-C6 ABT-U2(3): the meta row owns the only uz_label_format_ draw");
        TEST_CHECK(downloading.find("updatelogic::FormatProgressMb(received, total)") !=
                       std::string::npos,
                   "P3-C6 ABT-U2(3): the meta line itself stays (\"336 / 700 MB\")");
        TEST_CHECK(downloading.find("meta_right_x") == std::string::npos,
                   "P3-C6 ABT-U2(3): the overlapping right-hand slot variable is gone");
        TEST_CHECK(downloading.find("I18n::Get(DownloadZoneLabelId(download_cancelling_))") !=
                       std::string::npos,
                   "P3-C6 ABT-U2(3): the surviving label still routes the P2-E decision");
    }
    // The draw group the duplicate consumed is gone too: one fewer SetColor in
    // Render. The W5/C1 budget pin (tests/run_tests.cpp) moves 68 -> 67 in the
    // same change; pinning the new exact value here keeps the two in step.
    const std::string render = P3C6FuncBody(abt, "void AboutWindow::Render() {");
    TEST_CHECK(!render.empty(), "P3-C6: AboutWindow::Render is locatable");
    TEST_CHECK(P3C6CountOf(render, "scratch_brush_->SetColor(") == 67,
               "P3-C6 ABT-U2(3): Render drives 67 SetColor-just-before-use draw groups");
    TEST_CHECK(P3C6CountOf(render, "CreateSolidColorBrush(") == 0 &&
                   P3C6CountOf(render, "CreateTextLayout(") == 2,
               "P3-C6: the Render brush/measure budgets are untouched");

    // ---- header contract --------------------------------------------------
    TEST_CHECK(abtH.find("static bool ShouldResetZoneOnDismiss(UpdateZoneState state);") !=
                   std::string::npos,
               "P3-C6: about_window.hpp declares the ABT-U1 decision");
    TEST_CHECK(abtH.find("static bool FooterResetInteractive(UpdateZoneState state);") !=
                   std::string::npos,
               "P3-C6: about_window.hpp declares the ABT-U2(1) gate");
    TEST_CHECK(abtH.find("static bool ConsentConfirmIsDownload(int dlg_focus);") !=
                   std::string::npos,
               "P3-C6: about_window.hpp declares the ABT-U2(2) dispatch");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C6 About structural pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C6 About structural pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (c) ABT-U3: the early hash gate ------------------------------------

void TestP3C6UpdateCheckerEarlyHashGate() {
    std::cout << "[RUN] Testing P3-C6 ABT-U3 updater early hash gate..." << std::endl;
    const int failures_before = g_failed_count;

    std::string uc;
    P3C6ReadSrc(L"src\\update_checker.hpp", uc);
    TEST_CHECK(!uc.empty(), "P3-C6: update_checker.hpp resolvable");
    if (uc.empty()) {
        return;
    }

    const std::string body = P3C6FuncBody(uc, "inline UpdateDownloadStatus DownloadUpdate(");
    TEST_CHECK(!body.empty(), "P3-C6: DownloadUpdate body is locatable");
    if (!body.empty()) {
        const size_t cancel_gate =
            body.find("if (cancel.load(std::memory_order_relaxed)) return "
                      "UpdateDownloadStatus::Cancelled;");
        const size_t hash_gate = body.find("if (!expected_sha256) {");
        const size_t file = body.find("out.h = ::CreateFileW(dest_path.c_str()");
        const size_t socket = body.find("::WinHttpOpen(");
        TEST_CHECK(hash_gate != std::string::npos,
                   "P3-C6 ABT-U3: the nullopt hash gate exists at the top of DownloadUpdate");
        TEST_CHECK(cancel_gate != std::string::npos && hash_gate != std::string::npos &&
                       cancel_gate < hash_gate,
                   "P3-C6 ABT-U3: the cancel check still wins (a cancelled call is Cancelled)");
        TEST_CHECK(hash_gate != std::string::npos && file != std::string::npos && hash_gate < file,
                   "P3-C6 ABT-U3: the gate precedes the output file (CREATE_ALWAYS) creation");
        TEST_CHECK(hash_gate != std::string::npos && socket != std::string::npos &&
                       hash_gate < socket,
                   "P3-C6 ABT-U3: the gate precedes every WinHttpOpen (no socket, no traffic)");
        TEST_CHECK(hash_gate != std::string::npos &&
                       body.find("return UpdateDownloadStatus::HashMismatch;", hash_gate) !=
                           std::string::npos &&
                       body.find("return UpdateDownloadStatus::HashMismatch;", hash_gate) <
                           hash_gate + 600,
                   "P3-C6 ABT-U3: the gate fails closed with HashMismatch");
        TEST_CHECK(body.find("UPDATE/Download/008") != std::string::npos,
                   "P3-C6 ABT-U3: the refusal logs a shape-only diagnostic");
        TEST_CHECK(uc.find("V10F/ABT-U3") != std::string::npos,
                   "P3-C6 ABT-U3: the hunk carries its V10F/ABT-U3 tag");
    }
    // Both the gate and the late digest check survive (the late arm still owns
    // the real mismatch case; dropping it would let a wrong file through).
    TEST_CHECK(P3C6CountOf(uc, "if (!expected_sha256) {") == 1,
               "P3-C6 ABT-U3: exactly one early gate (no duplicate copies)");
    TEST_CHECK(uc.find("if (!expected_sha256 || actual != *expected_sha256) {") !=
                   std::string::npos,
               "P3-C6 ABT-U3: the late digest-mismatch gate is untouched");
    // The P2-A abort protocol around the gate is untouched.
    TEST_CHECK(uc.find("options.abort->ResetAborted();") != std::string::npos,
               "P3-C6 ABT-U3: the P2-A sticky-flag reset survives");

    // ---- functional leg: no network, no file ------------------------------
    {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
        if (ec || dir.empty()) {
            std::cout << "[SKIP] P3-C6: temp dir unavailable; the DownloadUpdate call leg is "
                         "skipped." << std::endl;
        } else {
            const std::filesystem::path dest = dir / L"emebala_p3c6_abt_u3_never.bin";
            std::error_code rm_ec;
            std::filesystem::remove(dest, rm_ec);

            std::atomic<bool> cancel{false};
            const std::string url =
                "https://github.com/teamsunplaza/Emebalachat/releases/download/v0.11.0/"
                "Emebalachat_Setup_0.11.0.exe";
            const UpdateDownloadStatus no_hash = DownloadUpdate(
                url, dest.wstring(), 734003200ull, std::nullopt, {}, cancel);
            TEST_CHECK(no_hash == UpdateDownloadStatus::HashMismatch,
                       "P3-C6 ABT-U3: a nullopt expected hash answers HashMismatch immediately");
            std::error_code exists_ec;
            TEST_CHECK(!std::filesystem::exists(dest, exists_ec),
                       "P3-C6 ABT-U3: the refused call never creates the output file");

            // Ordering control 1: a cancelled call is Cancelled, not
            // HashMismatch — the pre-existing cancel check keeps priority.
            std::atomic<bool> cancelled{true};
            const UpdateDownloadStatus was_cancelled = DownloadUpdate(
                url, dest.wstring(), 734003200ull, std::string(64, 'a'), {}, cancelled);
            TEST_CHECK(was_cancelled == UpdateDownloadStatus::Cancelled,
                       "P3-C6 ABT-U3: the entry cancel check still answers Cancelled first");
            TEST_CHECK(!std::filesystem::exists(dest, exists_ec),
                       "P3-C6 ABT-U3: a cancelled call creates no file either");

            // Ordering control 2: an ENGAGED hash is not swallowed by the new
            // gate — the call reaches the https/host gate and fails there
            // (example.com is not allowlisted; no socket is opened).
            std::atomic<bool> live{false};
            const UpdateDownloadStatus bad_host = DownloadUpdate(
                "https://example.com/Emebalachat_Setup.exe", dest.wstring(), 0,
                std::string(64, 'a'), {}, live);
            TEST_CHECK(bad_host == UpdateDownloadStatus::Failed,
                       "P3-C6 ABT-U3: an engaged hash passes the gate and still hits the "
                       "allowlist refusal");
            TEST_CHECK(!std::filesystem::exists(dest, exists_ec),
                       "P3-C6 ABT-U3: the allowlist refusal creates no file");
        }
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P3-C6 ABT-U3 updater early hash gate passed." << std::endl;
    } else {
        std::cout << "[FAIL] P3-C6 ABT-U3 updater early hash gate: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- registration ---------------------------------------------------------

void P3C6RegisterTests() {
    TestP3C6AboutPureDecisions();
    TestP3C6AboutStructuralPins();
    TestP3C6UpdateCheckerEarlyHashGate();
}

} // namespace p3_c6_tests
