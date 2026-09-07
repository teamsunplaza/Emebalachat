#include "win32_input.hpp"
#include "diag_logger.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <imm.h> // ImmGetContext/ImmGetCompositionStringW (imm32.lib already linked)
#include <mutex>
#include <thread>
#include <unordered_map>
#include <windows.h>

namespace emebalachat {

namespace {

inline INPUT CreateKeyInput(WORD vk, bool is_up, DWORD extra_info = EXTRA_INFO_MARKER) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = is_up ? KEYEVENTF_KEYUP : 0;
    switch (vk) {
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_UP:
        case VK_RIGHT:
        case VK_DOWN:
        case VK_INSERT:
        case VK_DELETE:
        case VK_RCONTROL:
        case VK_RMENU:
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            break;
    }
    input.ki.time = 0;
    input.ki.dwExtraInfo = extra_info;
    return input;
}

// REQ-R13 (audit §5 latent item 1): OpenClipboard contention with another
// process holding the clipboard. Bounded exponential backoff: up to
// kClipboardOpenMaxAttempts tries with 5/10/20/40 ms sleeps (75 ms total,
// inside the ~100 ms budget) - replaces the old fixed-5 ms retry spin. The
// caller-supplied timeout_ms stays a hard wall-clock cap so tight budgets win.
bool OpenClipboardWithRetry(HWND hwnd, DWORD timeout_ms = 100) {
    const auto start = std::chrono::steady_clock::now();
    for (int attempt = 1; attempt <= kClipboardOpenMaxAttempts; ++attempt) {
        if (::OpenClipboard(hwnd)) {
            return true;
        }
        const DWORD backoff = ClipboardOpenBackoffDelayMs(attempt);
        if (backoff == 0) {
            break; // final scheduled attempt failed
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start
        ).count();
        if (static_cast<DWORD>(elapsed) >= timeout_ms) {
            break;
        }
        ::Sleep(backoff);
    }
    return false;
}

// R6 Phase 3 (audit item 5): RAII clipboard scope. The audit confirmed every
// OpenClipboard/CloseClipboard pair below is already balanced on ALL normal
// and failure branches (no early return between open and close). The residual
// gap is exception safety: BackupClipboard allocates std::vector/std::wstring
// while the clipboard is OPEN, and a throw (std::bad_alloc on a huge format
// blob) would unwind past the explicit CloseClipboard - a clipboard left open
// for this process wedges it for EVERY other process on the desktop until we
// exit. Scoping the close in a destructor removes that entire class. Behavior
// on the non-throwing path is identical (CloseClipboard ran exactly once
// before; it still runs exactly once now).
class ScopedClipboard {
public:
    explicit ScopedClipboard(HWND owner, DWORD timeout_ms = 100)
        : opened_(OpenClipboardWithRetry(owner, timeout_ms)) {}
    ~ScopedClipboard() {
        if (opened_) {
            ::CloseClipboard();
        }
    }
    ScopedClipboard(const ScopedClipboard&) = delete;
    ScopedClipboard& operator=(const ScopedClipboard&) = delete;
    explicit operator bool() const { return opened_; }

private:
    bool opened_;
};

// REQ-R04 driver clock: milliseconds in the same epoch ClipboardCopyWatcher
// expects (steady_clock, monotonic, unaffected by wall-clock adjustments).
uint64_t SteadyNowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count());
}

// L2 fix: build the per-process synthetic-input sentinel.
//
// The old value was the compile-time constant 0x1337BEEF, which any local
// process could hardcode in SendInput dwExtraInfo to make our keyboard and
// mouse hooks silently ignore its events (bypass of text interception).
// The replacement is randomized per process from entropy that an external
// process cannot read: the high-resolution performance counter value and the
// ASLR-randomized address of a stack variable, mixed with the usual cheap
// identifiers through a splitmix64 finalizer.
//
// Deliberately dependency-free (no bcrypt.lib / advapi32 additions). This is
// an anti-spoofing hint, not a cryptographic secret; see the header note.
DWORD MakeSyntheticMarker() {
    auto splitmix64 = [](uint64_t x) {
        x += 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    };

    LARGE_INTEGER qpc = {};
    ::QueryPerformanceCounter(&qpc);
    volatile int stack_probe = 0;

    uint64_t seed = static_cast<uint64_t>(qpc.QuadPart) ^
                    (static_cast<uint64_t>(::GetCurrentProcessId()) << 16) ^
                    static_cast<uint64_t>(::GetCurrentThreadId()) ^
                    static_cast<uint64_t>(::GetTickCount64()) ^
                    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&stack_probe));

    seed = splitmix64(seed);
    DWORD marker = static_cast<DWORD>(seed >> 32);
    if (marker == 0) {
        // Never 0: 0 is the dwExtraInfo of ordinary real user input, so a zero
        // sentinel would make the hooks ignore genuine keystrokes/clicks.
        marker = static_cast<DWORD>(seed) | 1u;
    }
    return marker;
}

} // namespace

// Defined here (declared `extern const` in the header) so the producers in this
// file and the consumers in hook.cpp / mouse_hook.cpp / worker.cpp all compare
// against the exact same value chosen once at process start.
const DWORD EXTRA_INFO_MARKER = MakeSyntheticMarker();

bool IsGdiClipboardFormat(UINT format) {
    switch (format) {
        case CF_BITMAP:        // 2
        case CF_METAFILEPICT:  // 3
        case CF_PALETTE:       // 9
        case CF_ENHMETAFILE:   // 14
            return true;
        default:
            return false;
    }
}

void FlushIme() {
    INPUT inputs[2] = {
        CreateKeyInput(VK_RIGHT, false, EXTRA_INFO_MARKER),
        CreateKeyInput(VK_RIGHT, true, EXTRA_INFO_MARKER)
    };
    ::SendInput(2, inputs, sizeof(INPUT));
    ::Sleep(10);
}

bool SelectMessageBlock() {
    // Multi-line block fix: two-stage selection that captures ALL lines from the
    // start of text to the cursor, not just the current physical line.
    //   Stage 1 (Shift+Home)  : extends the selection to the start of the CURRENT
    //                           line — the old, line-bounded behavior.
    //   Stage 2 (Ctrl+Shift+Home): with the modifier already held, extends the
    //                           selection to the START OF TEXT, sweeping every
    //                           preceding line. Standard edit-control navigation
    //                           (works in RichEdit/EDIT, browsers, Electron,
    //                           Notepad, Word, etc.).
    // This is the same successful paste mechanics of Quick Translator et al.
    // Both stages keep the synthetic EXTRA_INFO_MARKER so our keyboard hook
    // ignores its own injection, and the resulting selection spans the full
    // multi-line block, allowing Ctrl+C to capture it verbatim.
    INPUT inputs[8] = {
        CreateKeyInput(VK_SHIFT, false, EXTRA_INFO_MARKER),    // Shift down
        CreateKeyInput(VK_HOME, false, EXTRA_INFO_MARKER),     // Home -> line start
        CreateKeyInput(VK_HOME, true, EXTRA_INFO_MARKER),      // Home up
        CreateKeyInput(VK_CONTROL, false, EXTRA_INFO_MARKER),  // Ctrl down (Shift held)
        CreateKeyInput(VK_HOME, false, EXTRA_INFO_MARKER),     // Home -> text start
        CreateKeyInput(VK_HOME, true, EXTRA_INFO_MARKER),      // Home up
        CreateKeyInput(VK_CONTROL, true, EXTRA_INFO_MARKER),   // Ctrl up
        CreateKeyInput(VK_SHIFT, true, EXTRA_INFO_MARKER)      // Shift up
    };
    return ::SendInput(8, inputs, sizeof(INPUT)) == 8;
}

bool SelectAll() {
    INPUT inputs[4] = {
        CreateKeyInput(VK_CONTROL, false, EXTRA_INFO_MARKER),
        CreateKeyInput('A', false, EXTRA_INFO_MARKER),
        CreateKeyInput('A', true, EXTRA_INFO_MARKER),
        CreateKeyInput(VK_CONTROL, true, EXTRA_INFO_MARKER)
    };
    return ::SendInput(4, inputs, sizeof(INPUT)) == 4;
}

AppCategory ClassifyAppWindow(HWND hwnd) {
    if (!hwnd || !::IsWindow(hwnd)) {
        return AppCategory::CategoryB; // fail-open to editor path
    }
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) {
        return AppCategory::CategoryB;
    }
    HANDLE hProc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) {
        return AppCategory::CategoryB;
    }
    wchar_t image_path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    BOOL ok = ::QueryFullProcessImageNameW(hProc, 0, image_path, &size);
    ::CloseHandle(hProc);
    if (!ok || size == 0) {
        return AppCategory::CategoryB;
    }
    std::wstring_view path_view(image_path, size);
    auto last_slash = path_view.find_last_of(L"\\/");
    std::wstring_view filename = (last_slash != std::wstring_view::npos)
        ? path_view.substr(last_slash + 1) : path_view;
    auto equals_ci = [](std::wstring_view a, std::wstring_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (::towlower(a[i]) != ::towlower(b[i])) return false;
        }
        return true;
    };
    // Phase 5 (REQ-011): Category A = chat/command apps (Enter = send/execute).
    // Static exe-name table, lowercase-insensitive (plan §2.2). Everything not
    // listed is Category B (editor-type). Terminals are deliberately NOT listed
    // (plan §2.2: synthetic Ctrl+C = SIGINT hazard, Phase 0 §5.2).
    static const std::wstring_view kCategoryAApps[] = {
        L"KakaoTalk.exe", L"Discord.exe", L"Slack.exe", L"Telegram.exe",
        L"Teams.exe", L"ms-teams.exe", L"Line.exe", L"WeChat.exe",
        L"WhatsApp.exe",
        L"Code.exe", L"Code - Insiders.exe", L"Cursor.exe", L"Windsurf.exe",
        L"VSCodium.exe", L"opencode.exe", L"claude.exe", L"codex.exe"
    };
    for (const auto& app : kCategoryAApps) {
        if (equals_ci(filename, app)) {
            return AppCategory::CategoryA;
        }
    }
    return AppCategory::CategoryB;
}

// Phase 8 Batch 1 (REQ-005, plan 225900 §1.5/§4.1): console/terminal detection
// for the drag-capture SIGINT guard. See win32_input.hpp for the contract.
// Class-name based (not exe-name): the hwnd holding a terminal selection is
// owned by conhost/WindowsTerminal, never by the shell process running inside.
// Checked on the window itself AND its GA_ROOT ancestor because the capture
// target may be the top-level frame (ConsoleWindowClass / CASCADIA_HOSTING...)
// or a nested child (PseudoConsoleWindow under Windows Terminal). Fail-open:
// any query failure returns false so non-terminal apps keep the exact
// pre-Phase-8 clipboard path (regression blocker by construction).
bool IsConsoleCaptureUnsafe(HWND hwnd) {
    if (!hwnd || !::IsWindow(hwnd)) {
        return false; // fail-open: invalid handle keeps existing behavior
    }
    static const wchar_t* const kConsoleWindowClasses[] = {
        L"ConsoleWindowClass",            // conhost (classic console host)
        L"CASCADIA_HOSTING_WINDOW_CLASS", // Windows Terminal hosting frame
        L"PseudoConsoleWindow"            // OpenConsole / pseudo-console surface
    };
    // No capture needed: kConsoleWindowClasses has static storage duration.
    auto class_is_console = [](HWND w) -> bool {
        wchar_t cls[64] = {};
        // Local, no cross-thread send (same cost profile as the hook-thread
        // class-name cache in hook.cpp). len == 0 means query failed -> not
        // console (fail-open).
        if (::GetClassNameW(w, cls, static_cast<int>(sizeof(cls) / sizeof(cls[0]))) == 0) {
            return false;
        }
        for (const auto* name : kConsoleWindowClasses) {
            if (::lstrcmpiW(cls, name) == 0) {
                return true;
            }
        }
        return false;
    };

    if (class_is_console(hwnd)) {
        return true;
    }
    // Promote to the top-level frame: a child hwnd under a terminal root is
    // still a terminal capture target (GetAncestor failure returns nullptr ->
    // the extra check is simply skipped, fail-open preserved).
    const HWND root = ::GetAncestor(hwnd, GA_ROOT);
    if (root && root != hwnd && class_is_console(root)) {
        return true;
    }
    return false;
}

bool CopySelection() {
    INPUT inputs[4] = {
        CreateKeyInput(VK_CONTROL, false, EXTRA_INFO_MARKER),
        CreateKeyInput('C', false, EXTRA_INFO_MARKER),
        CreateKeyInput('C', true, EXTRA_INFO_MARKER),
        CreateKeyInput(VK_CONTROL, true, EXTRA_INFO_MARKER)
    };
    return ::SendInput(4, inputs, sizeof(INPUT)) == 4;
}

// REQ-R04: pure state machine (declared in win32_input.hpp). No Win32 calls -
// the caller feeds sequence numbers and timestamps so the full timeline matrix
// is unit-testable headlessly (TestClipboardSequencePolling).
//
// Invariants:
//  - Pending until either a post-baseline change settles (Confirmed) or the
//    bounded budget expires (Failed). Terminal states never re-open.
//  - Confirmed requires the sequence to have left pre_seq_ at least once AND
//    to have held stable for kClipboardStableWindowMs, so handlers that write
//    EmptyClipboard() (seq+1) then SetClipboardData() (seq+2) cannot be
//    observed in their empty gap.
//  - A change that is still flapping when the hard deadline hits is Confirmed:
//    the clipboard demonstrably holds a committed post-Ctrl+C payload.
//  - No change by kClipboardChangeTimeoutMs is Failed: reading now would
//    return PRE-CopySelectedText content (the audit §2.3 stale-read bug).
ClipboardCopyOutcome ClipboardCopyWatcher::Update(uint32_t current_seq, uint64_t now_ms) {
    if (outcome_ != ClipboardCopyOutcome::Pending) {
        return outcome_;
    }

    if (current_seq != last_seq_) {
        last_seq_ = current_seq;
        last_change_ms_ = now_ms;
        if (current_seq != pre_seq_) {
            advanced_ = true;
        }
    }

    const uint64_t elapsed = (now_ms > start_ms_) ? (now_ms - start_ms_) : 0;

    if (advanced_ && (now_ms - last_change_ms_) >= kClipboardStableWindowMs) {
        outcome_ = ClipboardCopyOutcome::Confirmed;
    } else if (advanced_ && elapsed >= kClipboardCopyDeadlineMs) {
        outcome_ = ClipboardCopyOutcome::Confirmed;
    } else if (!advanced_ && elapsed >= kClipboardChangeTimeoutMs) {
        outcome_ = ClipboardCopyOutcome::Failed;
    }
    return outcome_;
}

bool CopySelectionWithSequenceWait() {
    // Baseline captured IMMEDIATELY before the keystroke: any sequence jump
    // from the worker's earlier BackupClipboard()/RestoreClipboard() writes is
    // already absorbed here (REQ-R04 directive: re-read after backup/restore).
    const uint32_t pre_seq = ::GetClipboardSequenceNumber();

    if (!CopySelection()) {
        DIAG_F("WIN32_INPUT/CopySelectionWithSequenceWait/001: SendInput(Ctrl+C) failed\n");
        return false;
    }

    const uint64_t start = SteadyNowMs();
    ClipboardCopyWatcher watcher(pre_seq, start);

    while (true) {
        const ClipboardCopyOutcome outcome = watcher.Update(::GetClipboardSequenceNumber(), SteadyNowMs());
        if (outcome == ClipboardCopyOutcome::Confirmed) {
            return true;
        }
        if (outcome == ClipboardCopyOutcome::Failed) {
            DIAG_F(
                    "WIN32_INPUT/CopySelectionWithSequenceWait/002: clipboard sequence %lu unchanged %ums after Ctrl+C; "
                    "refusing stale read (copy treated as failure)\n",
                    static_cast<unsigned long>(pre_seq), kClipboardChangeTimeoutMs);
            return false;
        }
        ::Sleep(kClipboardPollIntervalMs);
    }
}

bool PasteSelection() {
    INPUT inputs[4] = {
        CreateKeyInput(VK_CONTROL, false, EXTRA_INFO_MARKER),
        CreateKeyInput('V', false, EXTRA_INFO_MARKER),
        CreateKeyInput('V', true, EXTRA_INFO_MARKER),
        CreateKeyInput(VK_CONTROL, true, EXTRA_INFO_MARKER)
    };
    return ::SendInput(4, inputs, sizeof(INPUT)) == 4;
}

std::wstring GetClipboardText(DWORD timeout_ms) {
    // R6 Phase 3 (audit item 5): RAII scope, see ScopedClipboard note.
    ScopedClipboard clip(nullptr, timeout_ms);
    if (!clip) {
        return {};
    }

    std::wstring result;
    if (::IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE hData = ::GetClipboardData(CF_UNICODETEXT);
        if (hData) {
            SIZE_T sz = ::GlobalSize(hData);
            if (sz > 0) {
                const auto* ptr = static_cast<const wchar_t*>(::GlobalLock(hData));
                if (ptr) {
                    size_t char_count = sz / sizeof(wchar_t);
                    size_t len = 0;
                    while (len < char_count && ptr[len] != L'\0') {
                        len++;
                    }
                    result.assign(ptr, len);
                    ::GlobalUnlock(hData);
                }
            }
        }
    }

    return result;
}

// Windows Clipboard Monitor exclusion format IDs (prevents temporary text leaking into Win+V History or Cloud)
static const UINT g_cfExcludeClipboardProcessing = ::RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
static const UINT g_cfCanIncludeInHistory = ::RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
static const UINT g_cfCanUploadToCloud = ::RegisterClipboardFormatW(L"CanUploadToCloudClipboard");

// M1 (security): Clipboard observation window.
// Between SetClipboardText() and RestoreClipboard() inside PasteAndRestore(), the
// translated text is observable by any process polling GetClipboardData (third-party
// clipboard managers are not bound by the advisory exclusion formats above). The only
// shrinkable part of that window is the artificial "settle" wait after Ctrl+V: the
// target app must finish reading the clipboard (Electron/Slate.js read it asynchronously
// via IPC) before we EmptyClipboard, so it cannot be zero.
// 120 ms is the field-proven value shipped in v0.10.0 (commit f32d724); the 180 ms
// value added 60 ms of pure exposure with no measured benefit (raised defensively in
// d8f6a46). Keep this as small as real-world pastes allow; raise it back ONLY with a
// manual paste test against Discord/Slack/Teams, not by default.
constexpr DWORD kPasteSettleDelayMs = 120;

bool SetClipboardText(std::wstring_view text, DWORD timeout_ms) {
    size_t byte_len = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, byte_len);
    if (!hMem) {
        return false;
    }

    void* ptr = ::GlobalLock(hMem);
    if (!ptr) {
        ::GlobalFree(hMem);
        return false;
    }

    memcpy(ptr, text.data(), text.size() * sizeof(wchar_t));
    static_cast<wchar_t*>(ptr)[text.size()] = L'\0';
    ::GlobalUnlock(hMem);

    // R6 Phase 3 (audit item 5): RAII scope, see ScopedClipboard note. On
    // SetClipboardData SUCCESS the hMem ownership transfers to the clipboard
    // (Windows destroys it with the next EmptyClipboard); on failure it stays
    // ours and is freed locally - both unchanged, only the explicit close
    // moved into the guard.
    ScopedClipboard clip(nullptr, timeout_ms);
    if (!clip) {
        ::GlobalFree(hMem);
        return false;
    }

    ::EmptyClipboard();
    HANDLE hRes = ::SetClipboardData(CF_UNICODETEXT, hMem);
    if (!hRes) {
        ::GlobalFree(hMem);
        return false;
    }

    // Protect clipboard privacy: prevent Windows 10/11 Clipboard History (Win+V)
    // and cloud clipboard sync from logging or leaking temporary translated text
    if (g_cfExcludeClipboardProcessing) {
        HGLOBAL hFlag = ::GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (hFlag) {
            void* p = ::GlobalLock(hFlag);
            if (p) {
                *static_cast<DWORD*>(p) = 1;
                ::GlobalUnlock(hFlag);
                if (!::SetClipboardData(g_cfExcludeClipboardProcessing, hFlag)) {
                    ::GlobalFree(hFlag);
                }
            } else {
                ::GlobalFree(hFlag);
            }
        }
    }
    if (g_cfCanIncludeInHistory) {
        HGLOBAL hFlag = ::GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (hFlag) {
            void* p = ::GlobalLock(hFlag);
            if (p) {
                *static_cast<DWORD*>(p) = 0;
                ::GlobalUnlock(hFlag);
                if (!::SetClipboardData(g_cfCanIncludeInHistory, hFlag)) {
                    ::GlobalFree(hFlag);
                }
            } else {
                ::GlobalFree(hFlag);
            }
        }
    }
    if (g_cfCanUploadToCloud) {
        HGLOBAL hFlag = ::GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (hFlag) {
            void* p = ::GlobalLock(hFlag);
            if (p) {
                *static_cast<DWORD*>(p) = 0;
                ::GlobalUnlock(hFlag);
                if (!::SetClipboardData(g_cfCanUploadToCloud, hFlag)) {
                    ::GlobalFree(hFlag);
                }
            } else {
                ::GlobalFree(hFlag);
            }
        }
    }

    return true;
}

bool BackupClipboard(ClipboardBackup& out, DWORD timeout_ms) {
    out.text.reset();
    out.formats.clear();

    // R6 Phase 3 (audit item 5): RAII scope - this is the function the
    // exception-safety note above is about (std::vector allocations inside).
    ScopedClipboard clip(nullptr, timeout_ms);
    if (!clip) {
        return false;
    }

    // Capture text if available
    if (::IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE hData = ::GetClipboardData(CF_UNICODETEXT);
        if (hData) {
            SIZE_T sz = ::GlobalSize(hData);
            if (sz > 0) {
                const auto* ptr = static_cast<const wchar_t*>(::GlobalLock(hData));
                if (ptr) {
                    size_t char_count = sz / sizeof(wchar_t);
                    size_t len = 0;
                    while (len < char_count && ptr[len] != L'\0') {
                        len++;
                    }
                    out.text = std::wstring(ptr, len);
                    ::GlobalUnlock(hData);
                }
            }
        }
    }

    // Enumerate formats, safely filtering out GDI objects
    UINT fmt = 0;
    while ((fmt = ::EnumClipboardFormats(fmt)) != 0) {
        if (IsGdiClipboardFormat(fmt)) {
            continue;
        }

        HANDLE hData = ::GetClipboardData(fmt);
        if (!hData) {
            continue;
        }

        SIZE_T sz = ::GlobalSize(hData);
        if (sz > 0) {
            const void* ptr = ::GlobalLock(hData);
            if (ptr) {
                std::vector<uint8_t> buffer(sz);
                memcpy(buffer.data(), ptr, sz);
                out.formats.emplace_back(fmt, std::move(buffer));
                ::GlobalUnlock(hData);
            }
        }
    }

    return true;
}

bool RestoreClipboard(const ClipboardBackup& in, DWORD timeout_ms) {
    // R6 Phase 3 (audit item 5): RAII scope, see ScopedClipboard note.
    ScopedClipboard clip(nullptr, timeout_ms);
    if (!clip) {
        return false;
    }

    ::EmptyClipboard();

    if (!in.formats.empty()) {
        // Check if CF_UNICODETEXT is present in formats
        bool has_unicode_text = false;
        for (const auto& [fmt, _] : in.formats) {
            if (fmt == CF_UNICODETEXT) {
                has_unicode_text = true;
                break;
            }
        }

        for (const auto& [fmt, data] : in.formats) {
            // Skip GDI formats and empty data
            if (IsGdiClipboardFormat(fmt) || data.empty()) {
                continue;
            }

            // If CF_UNICODETEXT is present, let Windows synthesize CF_TEXT and CF_OEMTEXT automatically
            if (has_unicode_text && (fmt == CF_TEXT || fmt == CF_OEMTEXT)) {
                continue;
            }

            HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, data.size());
            if (!hMem) {
                continue;
            }

            void* ptr = ::GlobalLock(hMem);
            if (!ptr) {
                ::GlobalFree(hMem);
                continue;
            }

            memcpy(ptr, data.data(), data.size());
            ::GlobalUnlock(hMem);

            if (!::SetClipboardData(fmt, hMem)) {
                ::GlobalFree(hMem);
            }
        }
    } else if (in.text.has_value()) {
        // Fallback text restore
        const auto& str = *in.text;
        size_t byte_len = (str.size() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, byte_len);
        if (hMem) {
            void* ptr = ::GlobalLock(hMem);
            if (ptr) {
                memcpy(ptr, str.data(), str.size() * sizeof(wchar_t));
                static_cast<wchar_t*>(ptr)[str.size()] = L'\0';
                ::GlobalUnlock(hMem);
                if (!::SetClipboardData(CF_UNICODETEXT, hMem)) {
                    ::GlobalFree(hMem);
                }
            } else {
                ::GlobalFree(hMem);
            }
        }
    }

    return true;
}

// ---- REQ-027 (Phase A §A-2): EditCaretTracker --------------------------------
//
// "Translate from the previous translation point up to the caret" offset
// tracking, gated by the B-6b EM_* capability probe (ProbeEmCapability) on
// the CategoryB path - window class names never feed the verdict.
// Public contract and design references: win32_input.hpp §REQ-027 block and
// docs/260907_0001 §A-2 (§2.2.A2 contract, §2.3.A2 state, §2.4.A2 HWND
// resolution, §2.5.A2 deadlock guard, §2.6.A2 selection sequence, §2.9.A2
// REQ-017/023 alignment). All state is file-local and mutex-guarded: the map
// is worker-thread-only today, but the mutex documents safety across
// jthread stop/restart boundaries (I4 pattern, design §2.3.A2).
namespace edit_caret {

struct Key {
    HWND focus_hwnd = nullptr;
    DWORD pid = 0;
    bool operator==(const Key& o) const {
        return focus_hwnd == o.focus_hwnd && pid == o.pid;
    }
};

struct KeyHash {
    size_t operator()(const Key& k) const {
        // HWND values differ mostly in the low bits; mix once before xoring
        // the pid so distinct (hwnd,pid) pairs do not collide trivially.
        auto h = static_cast<size_t>(reinterpret_cast<uintptr_t>(k.focus_hwnd));
        h = (h >> 4) ^ (h << 8);
        return h ^ (static_cast<size_t>(k.pid) * 0x9E3779B9u);
    }
};

// REQ-034 F2-B': sentinel for a baseline document-length sample that could
// not be taken (WM_GETTEXTLENGTH timed out - same §2.5.A2 budget as the
// rest of the EM family). DIAG-only; never compared against a real offset.
constexpr DWORD kBaselineUnknown = UINT32_MAX;

struct Entry {
    DWORD offset = 0;          // UTF-16 code-unit index of the last replacement end
    // REQ-034 F2-B' auxiliary signal: document length (WM_GETTEXTLENGTH) at
    // the moment the offset was stored. Logged by the leading-CRLF self-
    // correction DIAG (/010) for drift attribution only - the PRIMARY verdict
    // is the measured capture prefix (design 260907 173700 §4.1 F2-B'), so
    // this value never gates selection geometry (Boring Technology).
    DWORD baseline_textlen = kBaselineUnknown;
    uint64_t last_used_ms = 0; // LRU stamp (GetTickCount64)
};

// §2.3.A2 memory upper bound: 64 entries max, oldest used-time evicted.
constexpr size_t kMaxEntries = 64;
// §2.5.A2 deadlock guard: no EM_* call may block the worker past 100 ms.
constexpr DWORD kEmTimeoutMs = 100;

std::mutex g_mutex;
std::unordered_map<Key, Entry, KeyHash> g_map;

// DP-5: EM_GETSEL requery failures in NotifyReplacement that fell back to the
// last+pasted_cch estimate. The running counter is logged with every /005
// line so a diag trace can observe estimate drift across a session.
std::atomic<uint64_t> g_requery_failures{0};

// §2.4.A2: resolve the focus-control candidate for the captured top-level
// target hwnd. GetGUIThreadInfo is callable from this (worker) thread for a
// foreign thread and never blocks. A null hwndFocus falls back to the
// top-level hwnd itself (some apps ARE their control); a candidate that is
// not a live window or belongs to nothing resolvable returns false (DP-4(d)
// fail-open: caller falls back to SelectMessageBlock).
bool ResolveFocusCandidate(HWND target, Key& out) {
    if (!target || !::IsWindow(target)) {
        return false;
    }
    DWORD pid = 0;
    const DWORD tid = ::GetWindowThreadProcessId(target, &pid);
    if (tid == 0 || pid == 0) {
        return false;
    }
    HWND focus = nullptr;
    GUITHREADINFO gti = {};
    gti.cbSize = sizeof(gti);
    if (::GetGUIThreadInfo(tid, &gti)) {
        focus = gti.hwndFocus;
    }
    if (!focus) {
        focus = target;
    }
    if (!::IsWindow(focus)) {
        return false;
    }
    out.focus_hwnd = focus;
    out.pid = pid;
    return true;
}

// B-6b (design 192100 §2.3, VP ruling 260907 21:55): the §2.4.A2 class-name
// whitelist IsStandardEditClass is RETIRED (deleted, zero references kept).
// Entry into the EM path is decided by ProbeEmCapability below - how the
// control actually answers the EM_* family - so future EM-capable controls
// are adopted automatically and the whitelist's false negatives cannot
// recur. The class name survives ONLY as QA attribution in DIAG logs.

// Best-effort UTF-8 rendering of the window class name for DIAG attribution
// (never consulted for a verdict). out is always NUL-terminated; the string
// is empty when the class query itself fails.
void ClassNameUtf8(HWND hwnd, char (&out)[128]) {
    out[0] = '\0';
    wchar_t cls[64] = {};
    if (::GetClassNameW(hwnd, cls, static_cast<int>(sizeof(cls) / sizeof(cls[0]))) == 0) {
        return;
    }
    ::WideCharToMultiByte(CP_UTF8, 0, cls, -1, out, static_cast<int>(sizeof(out)),
                          nullptr, nullptr);
}

// §2.5.A2: EVERY cross-process EM_* call goes through this wrapper so a hung
// or message-pump-busy target can never stall the worker beyond
// kEmTimeoutMs. Returns false on any failure or timeout. result is the
// DWORD_PTR out-parameter (EM_GETSEL packs its reply into the low 32 bits).
bool SendEm(HWND hwnd, UINT msg, WPARAM w, LPARAM l, ULONG_PTR& result) {
    result = 0;
    return ::SendMessageTimeoutW(hwnd, msg, w, l,
                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, kEmTimeoutMs,
                                 &result) != 0;
}

// §2.3.B-6b: read-only EM_* capability probe. Every message goes through
// SendEm (SendMessageTimeoutW, 100 ms cap, SMTO_ABORTIFHUNG) so a hung or
// pump-busy target cannot stall the worker (§2.5.A2 deadlock bound), and
// EM_SETSEL is NEVER sent here (the probe mutates nothing). The verdict is
// the pure ClassifyEmProbe over the collected signals - no class names.
// VP-approved hardening over the §2.3 pseudocode: SendMessageTimeoutW also
// returns SUCCESS when DefWindowProc answers 0 to an UNHANDLED message, so
// the ambiguous (0,0) EM_GETSEL reply additionally requires positive EM line
// -model evidence (EM_GETLINECOUNT >= 1, plus EM_LINEFROMCHAR for a non-empty
// document) before Capable is granted. Fast paths (timeout / meaningful
// selection) cost a single message, same traffic as the pre-B-6b hot path.
EmCapability ProbeEmCapability(HWND focus) {
    EmProbeSignals s{};
    ULONG_PTR getsel = 0;
    s.getsel_handled = SendEm(focus, EM_GETSEL, 0, 0, getsel);
    if (s.getsel_handled) {
        const DWORD sel = static_cast<DWORD>(getsel);
        s.sel_start = LOWORD(sel);
        s.sel_end = HIWORD(sel);
    }
    if (!s.getsel_handled || s.sel_start != 0 || s.sel_end != 0) {
        return ClassifyEmProbe(s); // NotCapable (timeout) or Capable (meaningful reply)
    }
    // Ambiguous (0,0): cross-check with independent read-only queries.
    s.limit_ok = SendEm(focus, EM_GETLIMITTEXT, 0, 0, s.limittext);
    s.len_ok = SendEm(focus, WM_GETTEXTLENGTH, 0, 0, s.textlen);
    s.count_ok = SendEm(focus, EM_GETLINECOUNT, 0, 0, s.linecount);
    if (s.len_ok && s.textlen > 0) {
        ULONG_PTR line = 0;
        s.linefromchar_ok =
            SendEm(focus, EM_LINEFROMCHAR, static_cast<WPARAM>(-1), 0, line);
    }
    return ClassifyEmProbe(s);
}

// §2.3.A2 lifecycle (a): drop entries whose focus window was destroyed.
// Callers hold g_mutex and the map is bounded to kMaxEntries, so the sweep
// is O(64) worst case on an Enter path that already did SendMessage round
// trips.
void PurgeDeadEntriesLocked() {
    for (auto it = g_map.begin(); it != g_map.end();) {
        if (!::IsWindow(it->first.focus_hwnd)) {
            it = g_map.erase(it);
        } else {
            ++it;
        }
    }
}

// Inserts or refreshes key's entry and refreshes its LRU stamp. Caller must
// hold g_mutex. Enforces the kMaxEntries upper bound by evicting the entry
// with the oldest last_used_ms (§2.3.A2 lifecycle (c)).
// REQ-034 F2-B': baseline_textlen is stored alongside the offset at every
// save site (TrySelectNewText start store, NotifyReplacement requery/
// estimate, self-correct) so the /010 DIAG can attribute drift.
void StoreLocked(const Key& key, DWORD offset, DWORD baseline_textlen) {
    const uint64_t now = ::GetTickCount64();
    auto it = g_map.find(key);
    if (it != g_map.end()) {
        it->second.offset = offset;
        it->second.baseline_textlen = baseline_textlen;
        it->second.last_used_ms = now;
        return;
    }
    if (g_map.size() >= kMaxEntries) {
        auto oldest = g_map.begin();
        for (auto i = g_map.begin(); i != g_map.end(); ++i) {
            if (i->second.last_used_ms < oldest->second.last_used_ms) {
                oldest = i;
            }
        }
        g_map.erase(oldest);
    }
    g_map.emplace(key, Entry{offset, baseline_textlen, now});
}

} // namespace edit_caret

// §2.6.A2 step 1: EM_GETSEL(last-known .. caret) selection. See header for
// the true/false contract. Any failure leaves the map untouched or returns
// false so the caller's SelectMessageBlock fallback reproduces the exact
// pre-REQ-027 behavior (regression-safe by construction).
bool EditCaretTracker_TrySelectNewText(HWND hwnd) {
    edit_caret::Key key{};
    if (!edit_caret::ResolveFocusCandidate(hwnd, key)) {
        DIAG_F("WIN32_INPUT/EditCaretTracker/002: focus candidate unresolved (hwnd=%p); fallback to SelectMessageBlock\n",
               reinterpret_cast<void*>(hwnd));
        return false;
    }
    // B-6b: capability probe replaces the class whitelist. The class name is
    // logged for QA attribution ONLY (app metadata, never user content) and
    // never feeds the verdict. Unknown/NotCapable both fall back to
    // SelectMessageBlock; only the DIAG code differs (/006 vs /007) so the
    // E2E matrix can attribute fallbacks to inconclusive vs refused EM_*.
    char cls_u8[128] = {};
    edit_caret::ClassNameUtf8(key.focus_hwnd, cls_u8);
    const EmCapability capability = edit_caret::ProbeEmCapability(key.focus_hwnd);
    if (capability == EmCapability::Unknown) {
        DIAG_F("WIN32_INPUT/EditCaretTracker/006: EM capability inconclusive (class '%s' attribution only, hwnd=%p); conservative fallback to SelectMessageBlock\n",
               cls_u8, reinterpret_cast<void*>(key.focus_hwnd));
        return false;
    }
    if (capability == EmCapability::NotCapable) {
        DIAG_F("WIN32_INPUT/EditCaretTracker/007: EM_* not handled (class '%s' attribution only, hwnd=%p); fallback to SelectMessageBlock\n",
               cls_u8, reinterpret_cast<void*>(key.focus_hwnd));
        return false;
    }
    DIAG_LOG("EditCaretTracker", "focus_class=%s hwnd=%p capability=capable",
             cls_u8, reinterpret_cast<const void*>(key.focus_hwnd));

    // EM_GETSEL with NULL pointer params: the selection comes back in the
    // return value (LOWORD start, HIWORD end), so no cross-process pointer
    // marshalling is needed. NOTE: the documented EM_GETSEL return is
    // WORD-scaled (>65535 saturates) - acceptable for message-sized editor
    // blocks, and both read and write paths below use the same encoding, so
    // a saturated offset stays self-consistent.
    ULONG_PTR em_result = 0;
    if (!edit_caret::SendEm(key.focus_hwnd, EM_GETSEL, 0, 0, em_result)) {
        DIAG_F("WIN32_INPUT/EditCaretTracker/001: EM_GETSEL failed/timed out (hwnd=%p gle=%lu); fallback\n",
               reinterpret_cast<void*>(key.focus_hwnd), ::GetLastError());
        return false;
    }
    const DWORD get_sel = static_cast<DWORD>(em_result);
    const DWORD sel_end = HIWORD(get_sel);

    // REQ-034 F2-B': baseline document length sampled with the start point
    // (auxiliary DIAG signal only - see Entry::baseline_textlen). The stored
    // start stays the pipeline-observed caret; geometry is unchanged.
    ULONG_PTR textlen_res = 0;
    const bool len_ok =
        edit_caret::SendEm(key.focus_hwnd, WM_GETTEXTLENGTH, 0, 0, textlen_res);
    const DWORD baseline = len_ok ? static_cast<DWORD>(textlen_res)
                                  : edit_caret::kBaselineUnknown;

    std::lock_guard<std::mutex> lock(edit_caret::g_mutex);
    edit_caret::PurgeDeadEntriesLocked();
    DWORD last = 0;
    const auto it = edit_caret::g_map.find(key);
    if (it != edit_caret::g_map.end()) {
        last = it->second.offset;
    }
    // §2.3.A2 lifecycle (b): the document shrank below the stored point
    // (user deleted text during the translation network delay - DP-4(b)).
    // Reset to 0 = select from text start (safe, pre-REQ-027 geometry).
    if (last > sel_end) {
        DIAG_F("WIN32_INPUT/EditCaretTracker/004: stored offset %lu > caret %lu (hwnd=%p); clamped to 0\n",
               last, sel_end, reinterpret_cast<void*>(key.focus_hwnd));
        last = 0;
    }

    ULONG_PTR set_sel_result = 0;
    if (!edit_caret::SendEm(key.focus_hwnd, EM_SETSEL, static_cast<WPARAM>(last),
                            static_cast<LPARAM>(sel_end), set_sel_result)) {
        DIAG_F("WIN32_INPUT/EditCaretTracker/003: EM_SETSEL(%lu,%lu) failed/timed out (hwnd=%p gle=%lu); fallback\n",
               last, sel_end, reinterpret_cast<void*>(key.focus_hwnd), ::GetLastError());
        return false;
    }
    // Remember the start point used for this Enter so a later NotifyReplacement
    // can compute the estimate (last + pasted_cch) even without a focus hwnd.
    edit_caret::StoreLocked(key, last, baseline);
    DIAG_LOG("EditCaretTracker", "em_setselect hwnd=%p last=%lu caret=%lu",
             reinterpret_cast<const void*>(key.focus_hwnd), last, sel_end);
    return true;
}

// REQ-034 F2-B' (design 260907 173700 §4.1 rule 4, debug §F2): self-correction
// re-selection, driven exclusively by CopySelectedText when the EM-path
// capture begins with a leading CRLF - measured proof the stored start points
// at a user-inserted (out-of-band) newline. Re-selects [0..caret) (the safe
// whole-block geometry shared with SelectMessageBlock, C-4) and stores start
// 0. ONCE per Enter by construction: a stored start of 0 refuses (a second
// capture would be byte-identical - the document itself begins with the
// newline), so the retry can never bounce. Same gates as TrySelectNewText
// (focus resolution + capability probe + §2.5.A2 100 ms budget).
bool EditCaretTracker_TrySelfCorrectReSelect(HWND hwnd) {
    edit_caret::Key key{};
    if (!edit_caret::ResolveFocusCandidate(hwnd, key)) {
        return false;
    }
    if (edit_caret::ProbeEmCapability(key.focus_hwnd) != EmCapability::Capable) {
        return false; // fallback geometry never drifts this way (design §2.1.1)
    }
    ULONG_PTR em_result = 0;
    if (!edit_caret::SendEm(key.focus_hwnd, EM_GETSEL, 0, 0, em_result)) {
        return false;
    }
    const DWORD sel_end = HIWORD(static_cast<DWORD>(em_result));
    ULONG_PTR textlen_res = 0;
    const bool len_ok =
        edit_caret::SendEm(key.focus_hwnd, WM_GETTEXTLENGTH, 0, 0, textlen_res);
    const DWORD baseline_now = len_ok ? static_cast<DWORD>(textlen_res)
                                      : edit_caret::kBaselineUnknown;

    std::lock_guard<std::mutex> lock(edit_caret::g_mutex);
    edit_caret::PurgeDeadEntriesLocked();
    const auto it = edit_caret::g_map.find(key);
    if (it == edit_caret::g_map.end() || it->second.offset == 0) {
        return false; // untracked or already whole-block: retry budget spent
    }
    const DWORD drifted_start = it->second.offset;
    const DWORD stored_baseline = it->second.baseline_textlen;

    ULONG_PTR set_sel_result = 0;
    if (!edit_caret::SendEm(key.focus_hwnd, EM_SETSEL, static_cast<WPARAM>(0),
                            static_cast<LPARAM>(sel_end), set_sel_result)) {
        DIAG_F("WIN32_INPUT/EditCaretTracker/010: self-correct EM_SETSEL(0,%lu) failed/timed out (hwnd=%p gle=%lu); keeping original capture\n",
               sel_end, reinterpret_cast<void*>(key.focus_hwnd), ::GetLastError());
        return false;
    }
    edit_caret::StoreLocked(key, 0, baseline_now);
    // /010 = design §4.1 rule 4. baseline/textlen deltas are attribution only.
    DIAG_F("WIN32_INPUT/EditCaretTracker/010: leading CRLF absorbed; start %lu -> 0 self-correct re-select [0..%lu) (hwnd=%p textlen=%lu baseline=%lu)\n",
           drifted_start, sel_end, reinterpret_cast<void*>(key.focus_hwnd),
           baseline_now, stored_baseline);
    return true;
}

// REQ-036 FIX-2 data-driven newline compensation, width-corrected by REQ-F1
// (docs/260908_0001 session). See the header contract for the full
// rationale. The capture text is passed IN: the leading pairs are recounted
// from it (caller and callee can never disagree), and the document's UTF-16
// newline WIDTH is back-computed from the capture-vs-selection arithmetic
// (EditCaretTracker_DocNewlineWidth): the clipboard CRLF-normalizes every
// newline, but the document may store each newline as a single LF unit -
// the old hardcoded 2-units-per-pair advance overshot Notepad's LF
// document (user log emebalachat_260908062830: selection 60 units vs
// capture 63 over 3 newlines), re-selected INSIDE the block, and left its
// first character(s) outside the replacement ("오"/"처"/"왜 " residue
// ahead of the pasted translation). Returns true when the stored offset
// was advanced by pair_count * width units. When the width is not
// recoverable from the arithmetic the function refuses and changes
// NOTHING - never guess a width; the hardcoded-width guess is exactly the
// F1 defect. The caller keeps its fallback budget either way.
bool EditCaretTracker_CompensateLeadingNewlines(HWND hwnd, std::wstring_view captured) {
    const size_t pair_count = EditCaretTracker_CountLeadingCrlfPairs(captured);
    if (pair_count == 0) {
        return false; // nothing measured: nothing to compensate
    }
    edit_caret::Key key{};
    if (!edit_caret::ResolveFocusCandidate(hwnd, key)) {
        return false;
    }
    if (edit_caret::ProbeEmCapability(key.focus_hwnd) != EmCapability::Capable) {
        return false; // no EM session: the map holds nothing meaningful
    }
    ULONG_PTR em_result = 0;
    if (!edit_caret::SendEm(key.focus_hwnd, EM_GETSEL, 0, 0, em_result)) {
        return false;
    }
    const DWORD sel = static_cast<DWORD>(em_result);
    const DWORD sel_start = LOWORD(sel);
    const DWORD sel_end = HIWORD(sel);

    std::lock_guard<std::mutex> lock(edit_caret::g_mutex);
    edit_caret::PurgeDeadEntriesLocked();
    const auto it = edit_caret::g_map.find(key);
    if (it == edit_caret::g_map.end() || it->second.offset == 0) {
        return false; // untracked or already block-head: no drift to repair
    }
    const DWORD drifted_start = it->second.offset;
    // REQ-F1 consistency gate: the width arithmetic assumes the live
    // selection is exactly the [drifted_start..caret) range the capture
    // was copied from. A different live start (or an inverted range)
    // means the geometry was disturbed between copy and compensation -
    // refuse rather than compute a width from unrelated numbers.
    if (sel_start != drifted_start || sel_end < sel_start) {
        return false;
    }
    // REQ-F1 width measurement: back-compute the document's UTF-16
    // newline width from the capture text vs the live selection span.
    // The clipboard CRLF-normalizes; the document may store 1-unit LFs.
    const size_t newline_sequences = EditCaretTracker_CountNewlineSequences(captured);
    const size_t width = EditCaretTracker_DocNewlineWidth(
        captured.size(), static_cast<size_t>(sel_end - sel_start), newline_sequences);
    if (width == 0) {
        // Ambiguous arithmetic (mixed-width document / non-normalized
        // capture / zero-span selection): refusing keeps the original
        // capture AND the stored offset - the safe pre-F1 geometry.
        DIAG_F("WIN32_INPUT/EditCaretTracker/012: document newline width not recoverable (captured %zu units vs span %lu over %zu newline(s)); refusing compensation, keeping capture\n",
               captured.size(), sel_end - sel_start, newline_sequences);
        return false;
    }
    // The clamp mirror (TrySelectNewText /004 rule): if the advanced start
    // would exceed the measured caret the entry is stale beyond repair by
    // arithmetic - refuse, the next Enter's clamp handles it as always.
    const uint64_t advanced = static_cast<uint64_t>(drifted_start) +
                              static_cast<uint64_t>(pair_count) * width;
    if (advanced > sel_end) {
        return false;
    }
    // Re-select EXCLUDING the pairs (same EM_SETSEL-under-lock discipline as
    // TrySelfCorrectReSelect). The pairs are the PREVIOUS blocks' terminators:
    // they must sit OUTSIDE the replacement range, or the paste would delete
    // the separator and merge the blocks (the F2 line-merge mechanism). The
    // capture text the caller holds is trimmed by the same 2*pair_count units,
    // so selection and text stay byte-consistent without a second Ctrl+C.
    ULONG_PTR set_sel_result = 0;
    if (!edit_caret::SendEm(key.focus_hwnd, EM_SETSEL, static_cast<WPARAM>(advanced),
                            static_cast<LPARAM>(sel_end), set_sel_result)) {
        return false; // offset NOT advanced: caller keeps its fallback budget
    }
    it->second.offset = static_cast<DWORD>(advanced);
    it->second.last_used_ms = ::GetTickCount64();
    DIAG_F("WIN32_INPUT/EditCaretTracker/011: leading %zu CRLF pair(s) compensated at doc width %zu unit(s); start %lu -> %lu re-select [%lu..%lu) (hwnd=%p)\n",
           pair_count, width, drifted_start, it->second.offset, it->second.offset, sel_end,
           reinterpret_cast<void*>(key.focus_hwnd));
    return true;
}

// §2.6.A2 step 4 + B-6a call-timing contract (design 210000 §2.2 option (a)):
// after a successful replacement, advance the stored "previous translation
// end" offset. The stored value is the START of the next Enter's EM_SETSEL
// range (TrySelectNewText above), so on paths where a newline is injected the
// caller MUST invoke this AFTER SendEnterKey + the settle poll - the REQ-023
// post-newline caret is the next block's start. Saving pre-newline stored the
// CRLF position and the following selection swallowed + merged the line break
// (ISSUE-1; E2E example1/consecutive gates). Internal logic is unchanged by
// B-6a: real-caret-first requery EM_GETSEL and use its end position; only on
// requery failure fall back to the last+pasted_cch estimate (DP-5). !pasted
// never updates (stale last stays, and the next Enter's clamp guards it -
// design §1.3.4 offset rule (b)).
void EditCaretTracker_NotifyReplacement(HWND hwnd, bool pasted, size_t pasted_cch) {
    if (!pasted) {
        return; // design §2.6.A2: failed/H1-aborted paste leaves the offset untouched
    }
    edit_caret::Key key{};
    if (!edit_caret::ResolveFocusCandidate(hwnd, key) ||
        edit_caret::ProbeEmCapability(key.focus_hwnd) != EmCapability::Capable) {
        return; // no EM session was possible for this window: nothing to advance
    }

    DWORD caret = 0;
    bool have_caret = false;
    ULONG_PTR em_result = 0;
    if (edit_caret::SendEm(key.focus_hwnd, EM_GETSEL, 0, 0, em_result)) {
        caret = HIWORD(static_cast<DWORD>(em_result));
        have_caret = true;
    }
    // REQ-034 F2-B': record the document length AT SAVE TIME on both branches
    // (requery success and estimate alike) so the next Enter's self-correct
    // DIAG can attribute drift. Purely auxiliary: the B-6a offset contract
    // (post-newline save) and the stored offset value are unchanged.
    ULONG_PTR textlen_res = 0;
    const bool len_ok =
        edit_caret::SendEm(key.focus_hwnd, WM_GETTEXTLENGTH, 0, 0, textlen_res);
    const DWORD baseline = len_ok ? static_cast<DWORD>(textlen_res)
                                  : edit_caret::kBaselineUnknown;

    std::lock_guard<std::mutex> lock(edit_caret::g_mutex);
    edit_caret::PurgeDeadEntriesLocked();
    if (have_caret) {
        // If the caret somehow precedes the stored start (deletion during
        // paste), the next Enter's clamp (§2.3.A2 lifecycle (b)) repairs it;
        // storing the real caret directly is the design's priority rule.
        edit_caret::StoreLocked(key, caret, baseline);
        return;
    }
    // Requery failed: estimate from the stored start plus the pasted length.
    // NOTE (B-6a, design §4.1-1): the estimate does NOT include the injected
    // newline (pasted_cch is the translated text only), so a call made
    // pre-newline would merge lines again. The post-newline call site makes
    // the estimate the conservative low bound (caret is >= last+pasted_cch);
    // the /005 counter below lets E2E/QA observe how often this path is taken
    // before any compensation is considered (measure first, 192100 §2.4).
    DWORD last = 0;
    const auto it = edit_caret::g_map.find(key);
    if (it != edit_caret::g_map.end()) {
        last = it->second.offset;
    }
    const uint64_t fails = edit_caret::g_requery_failures.fetch_add(1) + 1;
    const DWORD est = EditCaretTracker_EstimateNextOffset(last, pasted_cch);
    edit_caret::StoreLocked(key, est, baseline);
    DIAG_F("WIN32_INPUT/EditCaretTracker/005: EM_GETSEL requery failed (hwnd=%p gle=%lu); stored estimate %lu = last %lu + pasted %zu (requery_failures=%llu)\n",
           reinterpret_cast<void*>(key.focus_hwnd), ::GetLastError(), est, last, pasted_cch,
           static_cast<unsigned long long>(fails));
}

// REQ-027 B-6a: read-only caret (selection-end) probe. Same gates as
// TrySelectNewText/NotifyReplacement - focus candidate resolution, EM_*
// capability verdict (B-6b; class names unused), EM_GETSEL inside the
// 100 ms deadlock budget (§2.5.A2) - but it never touches the state map.
// kEditCaretUnknown on any gate failure.
DWORD EditCaretTracker_SampleCaret(HWND hwnd) {
    edit_caret::Key key{};
    if (!edit_caret::ResolveFocusCandidate(hwnd, key) ||
        edit_caret::ProbeEmCapability(key.focus_hwnd) != EmCapability::Capable) {
        return kEditCaretUnknown;
    }
    ULONG_PTR em_result = 0;
    if (!edit_caret::SendEm(key.focus_hwnd, EM_GETSEL, 0, 0, em_result)) {
        return kEditCaretUnknown;
    }
    return HIWORD(static_cast<DWORD>(em_result));
}

// REQ-039: read-only EM-selection emptiness probe for the copy-chord retry
// gate (see CopyChordRetryWarranted). Resolves the same focus candidate and
// capability verdict the capture path uses, then reads EM_GETSEL once. True
// only on Capable + successfully-probed empty range; every gate failure is
// false ("not provably empty"), so a failed copy on a non-EM control is
// always retried under the attempt budget - only the measured
// paste-window-type geometry can skip the retry.
bool EditCaretTracker_SelectionProvablyEmpty(HWND hwnd) {
    edit_caret::Key key{};
    if (!edit_caret::ResolveFocusCandidate(hwnd, key)) {
        return false;
    }
    if (edit_caret::ProbeEmCapability(key.focus_hwnd) != EmCapability::Capable) {
        return false;
    }
    ULONG_PTR em_result = 0;
    if (!edit_caret::SendEm(key.focus_hwnd, EM_GETSEL, 0, 0, em_result)) {
        return false;
    }
    const DWORD sel = static_cast<DWORD>(em_result);
    return LOWORD(sel) == HIWORD(sel);
}

// REQ-027 B-6a settle (design §2.2 mandatory companion): SendEnterKey injects
// Enter asynchronously (down + 35 ms hold + up), so the target may not have
// written the CRLF by the time the worker re-samples the caret. Tier 1: fixed
// kNewlineSettleMs wait (same pattern as kPasteSettleDelayMs). Tier 2: poll
// the caret visibility kNewlineSettlePollMax times at kNewlineSettlePollMs
// intervals; stop as soon as it moved off the pre-newline baseline. Exhaustion
// only logs /008 and proceeds WITHOUT +2 compensation (rejected variant (b) -
// single-LF controls would be over-stored); the next Enter's clamp (EditCaretTracker/004)
// remains the last safety net ("settle failure allowed", design §2.2).
void EditCaretTracker_SettleNewlineVisible(HWND hwnd, DWORD pre_newline_caret) {
    ::Sleep(kNewlineSettleMs);
    if (pre_newline_caret == kEditCaretUnknown) {
        return; // no baseline (untracked/non-EM control): fixed wait only
    }
    for (int poll = 0; poll < kNewlineSettlePollMax; ++poll) {
        const DWORD now = EditCaretTracker_SampleCaret(hwnd);
        if (now == kEditCaretUnknown) {
            return; // EM path went away; NotifyReplacement decides on its own
        }
        if (now != pre_newline_caret) {
            return; // newline visible to EM_GETSEL - proceed to save
        }
        ::Sleep(kNewlineSettlePollMs);
    }
    DIAG_F("WIN32_INPUT/EditCaretTracker/008: injected newline not visible after settle (hwnd=%p caret=%lu unchanged); proceeding without compensation\n",
           reinterpret_cast<void*>(hwnd), pre_newline_caret);
}

// REQ-036 FIX-1: the worker sent an Enter that ends the task WITHOUT a paste
// (IME backstop, paste-window send-through, empty/bypass send-through; the
// full enumeration is in the debug-surgical report). That Enter inserted the
// CURRENT block's terminator into the document - the stored offset must
// advance past it or the NEXT Enter's capture begins with the CRLF pair.
// Same settle pattern as the B-6a paste path, then stores the measured
// caret (measurement, not a +2 assumption, so single-LF controls store
// correctly too). No-op when untracked or any EM gate fails.
void EditCaretTracker_NotifySentNewline(HWND hwnd, DWORD pre_newline_caret) {
    if (hwnd == nullptr) {
        return;
    }
    edit_caret::Key key{};
    if (!edit_caret::ResolveFocusCandidate(hwnd, key)) {
        return;
    }
    if (edit_caret::ProbeEmCapability(key.focus_hwnd) != EmCapability::Capable) {
        return; // settle would only burn the fixed 50 ms on a non-EM window
    }
    // Untracked? Nothing to advance (the map holds no entry) - skip the settle.
    {
        std::lock_guard<std::mutex> lock(edit_caret::g_mutex);
        if (edit_caret::g_map.find(key) == edit_caret::g_map.end()) {
            return;
        }
    }
    // Settle (same budget as the B-6a paste path): SendEnterKey's input is
    // async; waiting for the caret to move off the pre-sample proves the
    // newline is visible to EM_GETSEL before we store it.
    EditCaretTracker_SettleNewlineVisible(hwnd, pre_newline_caret);
    ULONG_PTR em_result = 0;
    if (!edit_caret::SendEm(key.focus_hwnd, EM_GETSEL, 0, 0, em_result)) {
        return;
    }
    const DWORD caret = HIWORD(static_cast<DWORD>(em_result));
    ULONG_PTR textlen_res = 0;
    const bool len_ok =
        edit_caret::SendEm(key.focus_hwnd, WM_GETTEXTLENGTH, 0, 0, textlen_res);
    const DWORD baseline = len_ok ? static_cast<DWORD>(textlen_res)
                                  : edit_caret::kBaselineUnknown;
    {
        std::lock_guard<std::mutex> lock(edit_caret::g_mutex);
        edit_caret::PurgeDeadEntriesLocked();
        const auto it = edit_caret::g_map.find(key);
        if (it == edit_caret::g_map.end() || caret < it->second.offset) {
            return; // untracked, or stale (deletion behind us): leave as-is
        }
        it->second.offset = caret;
        it->second.baseline_textlen = baseline;
        it->second.last_used_ms = ::GetTickCount64();
    }
    DIAG_F("WIN32_INPUT/EditCaretTracker/012: worker-sent newline; start advanced to measured caret %lu (hwnd=%p)\n",
           caret, reinterpret_cast<void*>(key.focus_hwnd));
}

std::wstring CopySelectedText(HWND hwnd) {
    // Phase 5 (REQ-011): the old SelectTextForTranslation() helper is inlined
    // here. ClassifyAppWindow is consulted once and its category picks the
    // selection primitive directly.
    //
    // Multi-line block fix (CategoryB path): non-chat apps previously captured
    // only the current physical line (Shift+Home), so a multi-line message
    // typed with Shift+Enter (or pasted with newlines) had ONLY its last line
    // translated when Enter fired. SelectMessageBlock() extends the selection
    // from the cursor to the start of the whole text flow, so the full block
    // reaches the translator. Identical for every language/script: it is pure
    // keyboard geometry.
    //
    // REQ-027 (Phase A §3 B-4): inside CategoryB, the EditCaretTracker EM path
    // gets first shot at a standard EDIT/RichEdit focus control - it performs
    // EM_SETSEL(previous translation end .. caret) IN-PROCESS on the target,
    // selecting ONLY the newly typed text instead of the whole flow. On false
    // (non-standard class, hung target, EM failure) the untouched
    // SelectMessageBlock fallback reproduces the exact pre-REQ-027 behavior;
    // the fallback is encapsulated here so worker.cpp needs no selection
    // wiring. CategoryA is untouched by construction.
    const AppCategory category = ClassifyAppWindow(hwnd);
    bool em_path = false; // REQ-034: self-correction is EM-path only
    bool sel_ok = false;
    // REQ-039 FIX-1 (chat-window Enter capture): Electron/Chromium targets
    // intermittently drop a synthetic Ctrl+C chord (the renderer-side
    // clipboard commit never lands - Discord log signatures L1782/L1860/
    // L1939; same dropped-chord class as PowerToys #46485). A single such
    // drop on the bare-Enter path is an empty capture, which the worker's
    // R5 hold turns into a swallowed Enter. Re-run the WHOLE cycle -
    // selection primitive + Ctrl+C + sequence wait - under the bounded
    // CopyChordRetryWarranted budget: every primitive (SelectAll /
    // SelectMessageBlock keyboard geometry / EM_SETSEL(last, caret)) is
    // idempotent, and each attempt re-baselines the sequence wait, so a
    // LATE commit from an earlier chord is read as a confirmed copy, never
    // stale text. The REQ-034 F3-B paste-window geometry (provably-empty EM
    // selection: Ctrl+C legitimately changes nothing) is exempt - the retry
    // could only add latency before the worker's silent send-through.
    bool copy_confirmed = false;
    // REQ-F3 (log 260908 F3): actual chord attempts executed so far (vs. the
    // kClipboardCopyChordAttempts budget constant) so the final diagnostic
    // reports what really ran - the provably-empty exemption can stop the
    // loop after one attempt.
    int attempts_executed = 0;
    for (int attempt = 0; attempt < kClipboardCopyChordAttempts; ++attempt) {
        if (attempt == 0) {
            if (category == AppCategory::CategoryA) {
                sel_ok = SelectAll();
            } else if (EditCaretTracker_TrySelectNewText(hwnd)) {
                sel_ok = true; // selection already set by EM_SETSEL - skip keyboard geometry
                em_path = true;
            } else {
                sel_ok = SelectMessageBlock();
            }
            ::Sleep(10);
        } else {
            ::Sleep(kClipboardCopyChordRetryGapMs);
            // Re-establish the selection: idempotent. CategoryA re-runs
            // Ctrl+A; the EM path re-runs the offset-based EM_SETSEL; the
            // fallback re-runs the Shift/Ctrl+Home geometry.
            if (category == AppCategory::CategoryA) {
                SelectAll();
            } else if (em_path) {
                EditCaretTracker_TrySelectNewText(hwnd);
            } else {
                SelectMessageBlock();
            }
            ::Sleep(10);
        }
        // REQ-R04: sequence-number polling replaces the old fixed 35 ms wait.
        // On timeout the clipboard provably still holds pre-copy content, so
        // we treat this attempt as failed (never read stale text).
        if (CopySelectionWithSequenceWait()) {
            copy_confirmed = true;
            break;
        }
        attempts_executed = attempt + 1;
        if (attempt + 1 >= kClipboardCopyChordAttempts) {
            break; // attempt budget exhausted - stop before the probe call
        }
        const bool provably_empty = EditCaretTracker_SelectionProvablyEmpty(hwnd);
        if (!CopyChordRetryWarranted(attempt, provably_empty)) {
            if (attempt == 0) {
                // REQ-F3 (log 260908 F3): the provably-empty EM selection is
                // the REQ-034 F3-B paste-window geometry - Ctrl+C on an empty
                // selection legitimately changes nothing, so the retry is
                // skipped by design. This used to log "retrying ..." and then
                // cancel silently, which misread in log analysis as a
                // broken retry loop; state the skip and its reason instead.
                DIAG_F("WIN32_INPUT/CopySelectedText/004: copy chord not confirmed on attempt 1/%d, but EM selection provably empty (hwnd=%p category=%d sel_send=%d); skipping retry by design (paste-window geometry, retry would only add latency)\n",
                        kClipboardCopyChordAttempts,
                        reinterpret_cast<void*>(hwnd), static_cast<int>(category), sel_ok ? 1 : 0);
            }
            break;
        }
        if (attempt == 0) {
            DIAG_F("WIN32_INPUT/CopySelectedText/003: copy chord not confirmed on attempt 1/%d (hwnd=%p category=%d sel_send=%d); retrying full selection+copy cycle\n",
                    kClipboardCopyChordAttempts,
                    reinterpret_cast<void*>(hwnd), static_cast<int>(category), sel_ok ? 1 : 0);
        }
    }
    if (!copy_confirmed) {
        // REQ-F3 (log 260908 F3): report the ACTUAL number of chord attempts
        // executed, not the budget constant. The old message printed the
        // constant unconditionally, so a first-attempt skip via the
        // provably-empty exemption read as "failed after 3 attempts in the
        // same millisecond" in log analysis.
        DIAG_F(
                "WIN32_INPUT/CopySelectedText/001: copy not confirmed after %d chord attempt(s) (hwnd=%p category=%d sel_send=%d); returning empty\n",
                attempts_executed,
                reinterpret_cast<void*>(hwnd), static_cast<int>(category), sel_ok ? 1 : 0);
        return {};
    }

    std::wstring text = GetClipboardText();
    // R5 observability: log capture SHAPE ONLY (length / newline count /
    // flags). The captured text is user message content - it must never be
    // printed (PII / chat-content leakage into diagnostics). Length and
    // newline count still discriminate empty vs non-empty vs multi-line,
    // which is exactly the gate-11 diagnosis.
    size_t nl = 0;
    for (wchar_t c : text) { if (c == L'\n' || c == L'\r') ++nl; }
    DIAG_F("WIN32_INPUT/CopySelectedText/002: captured %zu chars (%zu newline chars, category=%d)\n",
            text.size(), nl, static_cast<int>(category));

    // REQ-036 FIX-2 (design supersession of the F2-B' start=0 self-correct,
    // documented in the debug-surgical report 260907): a stored EM start that
    // lagged behind out-of-band Enter passes (hook pass-throughs, worker
    // send-throughs - the full enumeration is in the report) makes the
    // capture begin with N leading CRLF pairs. Those pairs are the PREVIOUS
    // blocks' terminators (or the drift itself): they must sit OUTSIDE the
    // replacement, and the blocks BEFORE them must NEVER be re-captured.
    // The pairs are counted from the MEASURED capture text (data-driven -
    // no path enumeration at the capture seam), the stored start and the
    // live selection advance past them, and the capture text is trimmed by
    // the same 2*N units - selection and text stay byte-consistent with no
    // second clipboard round-trip. The former start=0 re-select would have
    // swallowed every preceding already-translated block: exactly the
    // whole-document retranslation defect of REQ-036 (user log
    // emebalachat_260907200313 L322-326/L437-448), only correct when the
    // current block is the document's first.
    if (em_path && EditCaretTracker_HasLeadingCrlf(text)) {
        const size_t pairs = EditCaretTracker_CountLeadingCrlfPairs(text);
        if (EditCaretTracker_CompensateLeadingNewlines(hwnd, text)) {
            // The trim is in CLIPBOARD units: the pairs are literally
            // "\r\n" in the capture text, so each pair strips exactly 2
            // units regardless of the document's storage width (REQ-F1:
            // only the DOCUMENT-side advance is width-dependent).
            text.erase(0, 2 * pairs);
            // Shape only (same R5 rule as /002 - never log user content).
            size_t nl2 = 0;
            for (wchar_t c : text) { if (c == L'\n' || c == L'\r') ++nl2; }
            DIAG_F("WIN32_INPUT/CaptureCompensated/001: %zu leading CRLF pair(s) excluded; capture now %zu chars (%zu newline chars)\n",
                    pairs, text.size(), nl2);
        } else {
            // Refused: untracked / start already 0 (the document itself
            // begins with the newline - whole-block geometry is already the
            // safe bound) / EM gates failed. Keep the original capture; the
            // semantics are unchanged from the F2 /003 refusal.
            DIAG_F("WIN32_INPUT/CaptureCompensated/002: leading CRLF but compensation refused (hwnd=%p sel_send=%d); keeping capture (already-safe geometry)\n",
                    reinterpret_cast<void*>(hwnd), sel_ok ? 1 : 0);
        }
    }
    return text;
}

bool IsSameWindowForInjection(HWND expected_target, HWND current_foreground) {
    // No captured target (e.g. drag path with a null hwnd): nothing to verify against.
    if (!expected_target) {
        return true;
    }
    // A target was captured but no foreground window exists now: refuse to inject.
    if (!current_foreground) {
        return false;
    }
    if (current_foreground == expected_target) {
        return true;
    }
    // Tolerate focus moving to a child/re-nested window within the same top-level
    // window (e.g. an embedded input control). GA_ROOTOWNER walks the owner chain.
    HWND expected_root = ::GetAncestor(expected_target, GA_ROOTOWNER);
    HWND current_root = ::GetAncestor(current_foreground, GA_ROOTOWNER);
    return expected_root && (expected_root == current_root);
}

bool PasteAndRestore(std::wstring_view text, const ClipboardBackup& backup, HWND expected_target) {
    // H1 guard: re-verify the foreground window IMMEDIATELY before the injection
    // sequence. The translation network call above can take seconds; if the user
    // Alt-Tabbed or focus shifted, pasting here would leak translated text into
    // (and send synthetic input to) the wrong application.
    if (!IsSameWindowForInjection(expected_target, ::GetForegroundWindow())) {
        // Abort: clipboard untouched, nothing pasted. Original clipboard is
        // preserved by the caller's RAII restorer (backup was never overwritten).
        return false;
    }

    bool ok = SetClipboardText(text);
    if (ok) {
        PasteSelection();
        ::Sleep(kPasteSettleDelayMs); // M1: minimal paste settle (Electron/Slate.js IPC stability)
    }

    // Always restore original clipboard state without leak
    RestoreClipboard(backup);
    return ok;
}

void SendEnterKey(bool release_shift) {
    (void)release_shift;
    // Explicitly release all modifier keys to prevent Shift+Enter / Ctrl+Enter misinterpretation
    const WORD mods[] = {
        VK_LSHIFT, VK_RSHIFT, VK_SHIFT,
        VK_LCONTROL, VK_RCONTROL, VK_CONTROL,
        VK_LMENU, VK_RMENU, VK_MENU
    };
    for (WORD m : mods) {
        if ((::GetAsyncKeyState(m) & 0x8000) != 0) {
            INPUT up = CreateKeyInput(m, true, EXTRA_INFO_MARKER);
            ::SendInput(1, &up, sizeof(INPUT));
        }
    }
    ::Sleep(10);

    // Hardware scancode 0x1C for Enter key with 35ms hold duration
    INPUT enter_down = {};
    enter_down.type = INPUT_KEYBOARD;
    enter_down.ki.wVk = VK_RETURN;
    enter_down.ki.wScan = 0x1C;
    enter_down.ki.dwFlags = 0;
    enter_down.ki.time = 0;
    enter_down.ki.dwExtraInfo = EXTRA_INFO_MARKER;

    ::SendInput(1, &enter_down, sizeof(INPUT));
    ::Sleep(35); // 35ms hold duration

    INPUT enter_up = enter_down;
    enter_up.ki.dwFlags = KEYEVENTF_KEYUP;
    ::SendInput(1, &enter_up, sizeof(INPUT));
}

// REQ-R17 (audit §5 latent item 5): non-empty GCS_COMPSTR on the foreground
// window's IME context == a composition is still open. Passing (LPVOID)0/0
// queries the required buffer size; > 0 means a live composition string.
// See the header contract: NEVER call from a low-level hook callback.
//
// SCOPE DOWNGRADE (Phase 7 report §2.3, REQ-014): this is an IMM32 backstop -
// Korean IME and other legacy IMM32-based IMEs ONLY. Modern Microsoft
// Japanese/Chinese IMEs are TSF-native: they keep composition state in
// ITfContext objects that are never mirrored into the IMM32 context, so
// GCS_COMPSTR reads empty/0 and this function returns FALSE EVEN WHILE a
// TSF composition is open. GATE 2 is therefore a Korean/legacy-IME-only
// backstop and must not be relied on for TSF IME protection; the hook-side
// VK_PROCESSKEY mirror (ImeMirrorNext) is the only language-agnostic signal
// (report §3.4). Failing to detect here lets the pipeline proceed, which is
// why GATE 1 (mirror) remains the primary defense.
bool ForegroundImeComposing() {
    HWND hwnd = ::GetForegroundWindow();
    if (!hwnd) {
        return false;
    }
    HIMC himc = ::ImmGetContext(hwnd);
    if (!himc) {
        return false; // window has no IME context -> nothing composing
    }
    const LONG comp_size = ::ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
    ::ImmReleaseContext(hwnd, himc);
    return comp_size > 0;
}

} // namespace emebalachat
