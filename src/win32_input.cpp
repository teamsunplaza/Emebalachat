#include "win32_input.hpp"

#include <chrono>
#include <cwctype>
#include <thread>
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

bool OpenClipboardWithRetry(HWND hwnd, DWORD timeout_ms = 100) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
        if (::OpenClipboard(hwnd)) {
            return true;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start
        ).count();
        if (elapsed >= timeout_ms) {
            break;
        }
        ::Sleep(5);
    }
    return false;
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

bool SelectCurrentLine() {
    INPUT inputs[4] = {
        CreateKeyInput(VK_SHIFT, false, EXTRA_INFO_MARKER),
        CreateKeyInput(VK_HOME, false, EXTRA_INFO_MARKER),
        CreateKeyInput(VK_HOME, true, EXTRA_INFO_MARKER),
        CreateKeyInput(VK_SHIFT, true, EXTRA_INFO_MARKER)
    };
    return ::SendInput(4, inputs, sizeof(INPUT)) == 4;
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

bool IsChatApplicationWindow(HWND hwnd) {
    if (!hwnd || !::IsWindow(hwnd)) {
        return false;
    }

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) {
        return false;
    }

    HANDLE hProc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) {
        return false;
    }

    wchar_t image_path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    BOOL ok = ::QueryFullProcessImageNameW(hProc, 0, image_path, &size);
    ::CloseHandle(hProc);

    if (!ok || size == 0) {
        return false;
    }

    std::wstring_view path_view(image_path, size);
    auto last_slash = path_view.find_last_of(L"\\/");
    std::wstring_view filename = (last_slash != std::wstring_view::npos)
        ? path_view.substr(last_slash + 1)
        : path_view;

    auto equals_case_insensitive = [](std::wstring_view a, std::wstring_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (::towlower(a[i]) != ::towlower(b[i])) return false;
        }
        return true;
    };

    const std::wstring_view chat_apps[] = {
        L"KakaoTalk.exe",
        L"Discord.exe",
        L"Slack.exe",
        L"Telegram.exe",
        L"Teams.exe",
        L"ms-teams.exe",
        L"Line.exe",
        L"WeChat.exe"
    };

    for (const auto& app : chat_apps) {
        if (equals_case_insensitive(filename, app)) {
            return true;
        }
    }

    return false;
}

bool SelectTextForTranslation(HWND hwnd) {
    if (IsChatApplicationWindow(hwnd)) {
        return SelectAll();
    } else {
        return SelectCurrentLine();
    }
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
    if (!OpenClipboardWithRetry(nullptr, timeout_ms)) {
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

    ::CloseClipboard();
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

    if (!OpenClipboardWithRetry(nullptr, timeout_ms)) {
        ::GlobalFree(hMem);
        return false;
    }

    ::EmptyClipboard();
    HANDLE hRes = ::SetClipboardData(CF_UNICODETEXT, hMem);
    if (!hRes) {
        ::GlobalFree(hMem);
        ::CloseClipboard();
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

    ::CloseClipboard();
    return true;
}

bool BackupClipboard(ClipboardBackup& out, DWORD timeout_ms) {
    out.text.reset();
    out.formats.clear();

    if (!OpenClipboardWithRetry(nullptr, timeout_ms)) {
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

    ::CloseClipboard();
    return true;
}

bool RestoreClipboard(const ClipboardBackup& in, DWORD timeout_ms) {
    if (!OpenClipboardWithRetry(nullptr, timeout_ms)) {
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

    ::CloseClipboard();
    return true;
}

std::wstring CopySelectedText(HWND hwnd) {
    SelectTextForTranslation(hwnd);
    ::Sleep(10);

    CopySelection();
    ::Sleep(35); // 35ms copy settle delay

    return GetClipboardText();
}

std::wstring CopySelectedLine() {
    return CopySelectedText(nullptr);
}

bool IsSameWindowForInjection(HWND expected_target, HWND current_foreground) {
    // No captured target (e.g. CopySelectedLine path): nothing to verify against.
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

} // namespace emebalachat
