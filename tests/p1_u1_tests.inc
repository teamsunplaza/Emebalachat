// ---------------------------------------------------------------------------
// 261122_0002 P1-U1 (v0.10.10 phased fixes, unit P1-U1: hook/win32_input):
//   FUP-01 [P0] - consoles/terminals excluded from the ENTER translate
//     pipeline (IsConsoleCaptureUnsafe wired into IsEnterTranslateExcludedApp;
//     terminal-emulator exe-name fallback mintty/alacritty/wezterm-gui/pwsh;
//     CopySelectedText aborts before any selection primitive / Ctrl+C chord).
//   FUP-07 [P1] - epoch-guarded K apply: k_epoch_ monotonic counter, Ctrl+V
//     cross-window reset, compare_exchange_weak apply on the fx worker.
//   D3 (partial) - the third-party terminal window classes (mintty / PuTTY /
//     Alacritty / org.wezfurlong.wezterm) joined kConsoleWindowClasses.
// Staged as an .inc next to this runner; registered from main() per the
// end-of-file pattern.
//
// Coverage:
//   (a) TestP1U1V10FClassAndExeMatchers - the pure matchers
//       IsKnownConsoleWindowClass / IsTerminalExeNameForConsoleUnsafe
//       (win32_input.hpp/.cpp): the extended class table, case-insensitivity,
//       and the exe table's fail-open polarity (conhost/WT shells and editors
//       stay OUT of the exe fallback).
//   (b) TestP1U1V10FEpochGuardedK - the pure epoch machinery
//       (hook.hpp): ResetShiftEnterKEpoched ordering, ApplyPasteNewlineCount
//       apply/clamp/stale-drop, and the FUP-07 resurrection regression pins
//       (reset-then-apply, window-switch-then-apply) with real atomics, no
//       HWNDs.
//   (c) TestP1U1V10FStructuralPins - source pins on src/win32_input.cpp,
//       src/hook.cpp and src/hook.hpp (ResolveRepoFile + raw-byte find, the
//       TestReq048CloudFailureSignaling precedent; every needle is ASCII-only,
//       CRLF-normalized by the shared reader below).
// ---------------------------------------------------------------------------

namespace {

// Resolve + read a production source for the P1-U1 structural pins (same
// candidate-path + CRLF-normalization precedent as Req052ReadSrc).
void P1U1ReadSrc(const wchar_t* rel, std::string& out) {
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

// ---- (a) the pure class / exe-name matchers --------------------------------

void TestP1U1V10FClassAndExeMatchers() {
    std::cout << "[RUN] Testing P1-U1 V10F console class + terminal exe matchers..." << std::endl;
    const int failures_before = g_failed_count;

    // The Phase 8 originals still hit (zero feature loss).
    TEST_CHECK(IsKnownConsoleWindowClass(L"ConsoleWindowClass"),
               "P1-U1: ConsoleWindowClass (conhost) is a known console class");
    TEST_CHECK(IsKnownConsoleWindowClass(L"CASCADIA_HOSTING_WINDOW_CLASS"),
               "P1-U1: CASCADIA_HOSTING_WINDOW_CLASS (Windows Terminal) is known");
    TEST_CHECK(IsKnownConsoleWindowClass(L"PseudoConsoleWindow"),
               "P1-U1: PseudoConsoleWindow (OpenConsole) is a known console class");

    // V10F D3: the third-party terminal classes.
    TEST_CHECK(IsKnownConsoleWindowClass(L"mintty"),
               "P1-U1: mintty is a known console class (D3)");
    TEST_CHECK(IsKnownConsoleWindowClass(L"PuTTY"),
               "P1-U1: PuTTY is a known console class (D3)");
    TEST_CHECK(IsKnownConsoleWindowClass(L"Alacritty"),
               "P1-U1: Alacritty is a known console class (D3)");
    TEST_CHECK(IsKnownConsoleWindowClass(L"org.wezfurlong.wezterm"),
               "P1-U1: org.wezfurlong.wezterm (WezTerm winit class) is known (D3)");

    // Case-insensitive fold on both the originals and the additions.
    TEST_CHECK(IsKnownConsoleWindowClass(L"consolewindowclass"),
               "P1-U1: class match folds case (conhost lowercase)");
    TEST_CHECK(IsKnownConsoleWindowClass(L"MINTTY"),
               "P1-U1: class match folds case (MINTTY)");
    TEST_CHECK(IsKnownConsoleWindowClass(L"putty"),
               "P1-U1: class match folds case (putty)");
    TEST_CHECK(IsKnownConsoleWindowClass(L"ALACRITTY"),
               "P1-U1: class match folds case (ALACRITTY)");

    // Negative: ordinary app surfaces must stay OUT of the class table.
    TEST_CHECK(!IsKnownConsoleWindowClass(L"Chrome_WidgetWin_1"),
               "P1-U1: Chromium surfaces are not console classes");
    TEST_CHECK(!IsKnownConsoleWindowClass(L"Notepad"),
               "P1-U1: Notepad is not a console class");
    TEST_CHECK(!IsKnownConsoleWindowClass(L""),
               "P1-U1: empty class name fails open (false)");
    TEST_CHECK(!IsKnownConsoleWindowClass(L"ConsoleWindowClassX"),
               "P1-U1: a mere class-name PREFIX must not match");

    // V10F FUP-01: the terminal-emulator exe fallback table.
    TEST_CHECK(IsTerminalExeNameForConsoleUnsafe(L"mintty.exe"),
               "P1-U1: mintty.exe is a terminal exe (FUP-01)");
    TEST_CHECK(IsTerminalExeNameForConsoleUnsafe(L"alacritty.exe"),
               "P1-U1: alacritty.exe is a terminal exe (FUP-01)");
    TEST_CHECK(IsTerminalExeNameForConsoleUnsafe(L"wezterm-gui.exe"),
               "P1-U1: wezterm-gui.exe is a terminal exe (FUP-01)");
    TEST_CHECK(IsTerminalExeNameForConsoleUnsafe(L"pwsh.exe"),
               "P1-U1: pwsh.exe is a terminal exe (FUP-01)");
    TEST_CHECK(IsTerminalExeNameForConsoleUnsafe(L"MINTTY.EXE"),
               "P1-U1: terminal exe match folds case");

    // Fail-open polarity: the class-based consoles and the shells hosted
    // inside them are NOT exe-table entries (they never needed one).
    TEST_CHECK(!IsTerminalExeNameForConsoleUnsafe(L"conhost.exe"),
               "P1-U1: conhost.exe is class-detected, not an exe entry");
    TEST_CHECK(!IsTerminalExeNameForConsoleUnsafe(L"WindowsTerminal.exe"),
               "P1-U1: WindowsTerminal.exe is class-detected, not an exe entry");
    TEST_CHECK(!IsTerminalExeNameForConsoleUnsafe(L"cmd.exe"),
               "P1-U1: cmd.exe (conhost-hosted) is not an exe entry");
    TEST_CHECK(!IsTerminalExeNameForConsoleUnsafe(L"powershell.exe"),
               "P1-U1: powershell.exe (conhost-hosted) is not an exe entry");
    // Editors/chat apps must not drift into the terminal table.
    TEST_CHECK(!IsTerminalExeNameForConsoleUnsafe(L"Code.exe"),
               "P1-U1: Code.exe stays out of the terminal table");
    TEST_CHECK(!IsTerminalExeNameForConsoleUnsafe(L"Discord.exe"),
               "P1-U1: Discord.exe stays out of the terminal table");
    TEST_CHECK(!IsTerminalExeNameForConsoleUnsafe(L""),
               "P1-U1: empty exe name fails open (false)");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P1-U1 V10F console class + terminal exe matchers passed."
                  << std::endl;
    } else {
        std::cout << "[FAIL] P1-U1 V10F console class + terminal exe matchers: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (b) the epoch-guarded K machinery -------------------------------------

void TestP1U1V10FEpochGuardedK() {
    std::cout << "[RUN] Testing P1-U1 V10F epoch-guarded K apply (FUP-07)..." << std::endl;
    const int failures_before = g_failed_count;
    constexpr int kCap = 16384;

    // Reset: epoch bumps BEFORE the zero-store, K and the tracker zero out.
    {
        std::atomic<int> k{7};
        std::atomic<uint64_t> epoch{41};
        LineBreakTracker tracker;
        tracker.k = 5;
        tracker.line_input = 9;
        ResetShiftEnterKEpoched(k, epoch, tracker);
        TEST_CHECK(epoch.load() == 42, "P1-U1: reset bumps the epoch exactly once");
        TEST_CHECK(k.load() == 0, "P1-U1: reset zeroes K");
        TEST_CHECK(tracker.k == 0 && tracker.line_input == 0,
                   "P1-U1: reset clears the LineBreakTracker");
    }

    // Live apply: +add lands and returns the post-apply K.
    {
        std::atomic<int> k{2};
        std::atomic<uint64_t> epoch{7};
        const int r = ApplyPasteNewlineCount(k, epoch, /*captured*/7, /*add*/3, kCap);
        TEST_CHECK(r == 5 && k.load() == 5,
                   "P1-U1: live apply adds the paste newline count to K");
    }

    // Clamp at the guard ceiling.
    {
        std::atomic<int> k{kCap - 2};
        std::atomic<uint64_t> epoch{7};
        const int r = ApplyPasteNewlineCount(k, epoch, /*captured*/7, /*add*/5, kCap);
        TEST_CHECK(r == kCap && k.load() == kCap,
                   "P1-U1: the apply clamps at the capture-guard ceiling");
    }

    // Stale epoch: a reset between dispatch and execution drops the apply.
    // (This is the FUP-07 resurrection regression: pre-fix the load+add+store
    // re-published the paste count AFTER the Enter reset.)
    {
        std::atomic<int> k{4};
        std::atomic<uint64_t> epoch{9};
        LineBreakTracker tracker;
        const uint64_t captured = epoch.load();       // dispatch-time capture
        ResetShiftEnterKEpoched(k, epoch, tracker);   // Enter reset
        const int r = ApplyPasteNewlineCount(k, epoch, captured, /*add*/3, kCap);
        TEST_CHECK(r == -1, "P1-U1: stale-epoch apply reports the drop (-1)");
        TEST_CHECK(k.load() == 0,
                   "P1-U1: K is NOT resurrected after the Enter reset (FUP-07)");
    }

    // Window-switch equivalent: the mirror gate's epoch bump (no K zero on a
    // pure switch) still invalidates a lambda queued for the old window, and
    // the subsequent Ctrl+V-path reset starts the new window from K=0.
    {
        std::atomic<int> k{6};
        std::atomic<uint64_t> epoch{3};
        const uint64_t captured = epoch.load();
        epoch.fetch_add(1, std::memory_order_acq_rel); // foreground change
        const int r = ApplyPasteNewlineCount(k, epoch, captured, /*add*/2, kCap);
        TEST_CHECK(r == -1 && k.load() == 6,
                   "P1-U1: a window-switch epoch bump drops the cross-window apply");
    }

    // Two live pastes in the same context accumulate (existing behavior kept).
    {
        std::atomic<int> k{0};
        std::atomic<uint64_t> epoch{5};
        const uint64_t captured = epoch.load();
        const int r1 = ApplyPasteNewlineCount(k, epoch, captured, /*add*/3, kCap);
        const int r2 = ApplyPasteNewlineCount(k, epoch, captured, /*add*/5, kCap);
        TEST_CHECK(r1 == 3 && r2 == 8 && k.load() == 8,
                   "P1-U1: sequential live pastes accumulate K");
    }

    // The apply must never touch the epoch (it is not a reset).
    {
        std::atomic<int> k{0};
        std::atomic<uint64_t> epoch{11};
        ApplyPasteNewlineCount(k, epoch, /*captured*/11, /*add*/1, kCap);
        TEST_CHECK(epoch.load() == 11,
                   "P1-U1: a live apply leaves the epoch unchanged");
    }

    // Zero-add equivalence: the caller filters pasted_nl<=0 before applying;
    // a direct 0-add still validates the epoch and is a no-op when live.
    {
        std::atomic<int> k{2};
        std::atomic<uint64_t> epoch{13};
        const int r = ApplyPasteNewlineCount(k, epoch, /*captured*/13, /*add*/0, kCap);
        TEST_CHECK(r == 2 && k.load() == 2,
                   "P1-U1: a zero add is a validated no-op");
    }

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P1-U1 V10F epoch-guarded K apply passed." << std::endl;
    } else {
        std::cout << "[FAIL] P1-U1 V10F epoch-guarded K apply: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// ---- (c) structural source pins --------------------------------------------

void TestP1U1V10FStructuralPins() {
    std::cout << "[RUN] Testing P1-U1 V10F structural pins..." << std::endl;
    const int failures_before = g_failed_count;

    std::string win32, hookcpp, hookhpp;
    P1U1ReadSrc(L"src\\win32_input.cpp", win32);
    P1U1ReadSrc(L"src\\hook.cpp", hookcpp);
    P1U1ReadSrc(L"src\\hook.hpp", hookhpp);
    TEST_CHECK(!win32.empty(), "P1-U1: src/win32_input.cpp resolves for the pins");
    TEST_CHECK(!hookcpp.empty(), "P1-U1: src/hook.cpp resolves for the pins");
    TEST_CHECK(!hookhpp.empty(), "P1-U1: src/hook.hpp resolves for the pins");

    // -- win32_input.cpp: FUP-01 wiring ---------------------------------------
    TEST_CHECK(win32.find("if (IsConsoleCaptureUnsafe(hwnd))") != std::string::npos,
               "P1-U1: IsEnterTranslateExcludedApp consults IsConsoleCaptureUnsafe first");
    TEST_CHECK(win32.find("IsTerminalExeNameForConsoleUnsafe(basename)") != std::string::npos,
               "P1-U1: IsConsoleCaptureUnsafe has the terminal exe-name fallback");
    TEST_CHECK(win32.find("ResolveImageBasename(hwnd, basename, gle) == ImageResolveStatus::Ok &&\n"
                          "        IsTerminalExeNameForConsoleUnsafe(basename)") != std::string::npos,
               "P1-U1: the exe fallback keeps the fail-open resolution polarity");

    // -- win32_input.cpp: D3 class table --------------------------------------
    TEST_CHECK(win32.find("L\"mintty\",                        // Mintty") != std::string::npos,
               "P1-U1: kConsoleWindowClasses gained mintty (D3)");
    TEST_CHECK(win32.find("L\"PuTTY\",                         // PuTTY") != std::string::npos,
               "P1-U1: kConsoleWindowClasses gained PuTTY (D3)");
    TEST_CHECK(win32.find("L\"Alacritty\",                     // Alacritty") != std::string::npos,
               "P1-U1: kConsoleWindowClasses gained Alacritty (D3)");
    TEST_CHECK(win32.find("L\"org.wezfurlong.wezterm\"") != std::string::npos,
               "P1-U1: kConsoleWindowClasses gained WezTerm (D3)");
    TEST_CHECK(win32.find("L\"pwsh.exe\"") != std::string::npos &&
               win32.find("L\"wezterm-gui.exe\"") != std::string::npos &&
               win32.find("L\"alacritty.exe\"") != std::string::npos &&
               win32.find("L\"mintty.exe\"") != std::string::npos,
               "P1-U1: kTerminalApps carries mintty/alacritty/wezterm-gui/pwsh");

    // -- win32_input.cpp: zero feature loss ------------------------------------
    TEST_CHECK(win32.find("const std::wstring_view kEditorApps[] = {") != std::string::npos &&
               win32.find("L\"Code.exe\", L\"Code - Insiders.exe\", L\"Cursor.exe\", L\"Windsurf.exe\",") != std::string::npos &&
               win32.find("L\"VSCodium.exe\", L\"opencode.exe\", L\"claude.exe\", L\"codex.exe\"") != std::string::npos,
               "P1-U1: kEditorApps is unchanged (the 8 editor exes)");
    TEST_CHECK(win32.find("CopySelectedText/005: editor/IDE/terminal app excluded") != std::string::npos,
               "P1-U1: the CopySelectedText abort message covers terminals");

    // -- win32_input.cpp: the abort fires BEFORE any selection/Ctrl+C ----------
    {
        const size_t abort_pos = win32.find(
            "if (category == AppCategory::CategoryB && IsEnterTranslateExcludedApp(hwnd))");
        const size_t loop_pos = win32.find(
            "for (int attempt = 0; attempt < kClipboardCopyChordAttempts; ++attempt)");
        TEST_CHECK(abort_pos != std::string::npos && loop_pos != std::string::npos &&
                   abort_pos < loop_pos,
                   "P1-U1: CopySelectedText aborts before the copy-chord retry loop");
    }

    // -- hook.hpp: the epoch machinery -----------------------------------------
    TEST_CHECK(hookhpp.find("std::atomic<uint64_t> k_epoch_{0};") != std::string::npos,
               "P1-U1: KeyboardHook owns the monotonic k_epoch_ atomic");
    TEST_CHECK(hookhpp.find("inline void ResetShiftEnterKEpoched(") != std::string::npos &&
               hookhpp.find("epoch.fetch_add(1, std::memory_order_acq_rel);") != std::string::npos,
               "P1-U1: the reset bumps the epoch before the K zero-store");
    TEST_CHECK(hookhpp.find("inline int ApplyPasteNewlineCount(") != std::string::npos &&
               hookhpp.find("if (epoch.load(std::memory_order_acquire) != captured_epoch)") != std::string::npos &&
               hookhpp.find("k.compare_exchange_weak(current, desired,") != std::string::npos,
               "P1-U1: the CAS apply re-validates the epoch per iteration");

    // -- hook.cpp: the Ctrl+V rework -------------------------------------------
    TEST_CHECK(hookcpp.find("if (v_fg != s_instance->k_anchor_hwnd_)") != std::string::npos &&
               hookcpp.find("ResetShiftEnterKEpoched(s_instance->shift_enter_k_, s_instance->k_epoch_,\n"
                            "                                        s_instance->line_break_);") != std::string::npos,
               "P1-U1: Ctrl+V resets K/line_break when the foreground moved");
    TEST_CHECK(hookcpp.find("const uint64_t dispatch_epoch =") != std::string::npos,
               "P1-U1: the Ctrl+V lambda captures the epoch at dispatch");
    TEST_CHECK(hookcpp.find("ApplyPasteNewlineCount(self->shift_enter_k_, self->k_epoch_,") != std::string::npos,
               "P1-U1: the fx worker applies via the epoch-guarded CAS");
    TEST_CHECK(hookcpp.find("int k = self->shift_enter_k_.load(std::memory_order_relaxed) + add;") == std::string::npos,
               "P1-U1: the old non-atomic load+add+store is gone");

    // -- hook.cpp: every Enter-family reset routes through the helper ----------
    {
        size_t pos = 0;
        int reset_sites = 0;
        while ((pos = hookcpp.find("ResetShiftEnterKEpoched(s_instance->shift_enter_k_", pos)) != std::string::npos) {
            ++reset_sites;
            pos += 10; // step past this occurrence
        }
        TEST_CHECK(reset_sites == 8,
                   "P1-U1: exactly 8 reset sites use ResetShiftEnterKEpoched "
                   "(Alt/Win+Enter, Ctrl+V, Ctrl+A/X/Z, Ctrl+Enter, Shift+Enter "
                   "window-switch, composing promoted, composing pass-through, "
                   "bare-Enter gate)");
    }
    TEST_CHECK(hookcpp.find("s_instance->k_epoch_.fetch_add(1, std::memory_order_acq_rel);") != std::string::npos,
               "P1-U1: the foreground-change mirror gate bumps the epoch");

    if (g_failed_count == failures_before) {
        std::cout << "[PASS] P1-U1 V10F structural pins passed." << std::endl;
    } else {
        std::cout << "[FAIL] P1-U1 V10F structural pins: "
                  << (g_failed_count - failures_before) << " check(s) failed." << std::endl;
    }
}

// Single registration entry (the run_tests.cpp end-of-file pattern): the
// integrator adds `#include "p1_u1_tests.inc"` to the include cluster and
// `TestP1U1V10FHotfixes();` inside main().
void TestP1U1V10FHotfixes() {
    TestP1U1V10FClassAndExeMatchers();
    TestP1U1V10FEpochGuardedK();
    TestP1U1V10FStructuralPins();
}
