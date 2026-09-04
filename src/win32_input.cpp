#include "win32_input.hpp"

#include <chrono>
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

} // namespace

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

std::wstring CopySelectedLine() {
    SelectCurrentLine();
    ::Sleep(10);

    CopySelection();
    ::Sleep(35); // 35ms copy settle delay

    return GetClipboardText();
}

bool PasteAndRestore(std::wstring_view text, const ClipboardBackup& backup) {
    bool ok = SetClipboardText(text);
    if (ok) {
        PasteSelection();
        ::Sleep(120); // 120ms paste settle delay
    }

    // Always restore original clipboard state without leak
    RestoreClipboard(backup);
    return ok;
}

void SendEnterKey(bool release_shift) {
    bool shift_was_down = false;
    if (release_shift) {
        shift_was_down = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (shift_was_down) {
            INPUT up = CreateKeyInput(VK_SHIFT, true, EXTRA_INFO_MARKER);
            ::SendInput(1, &up, sizeof(INPUT));
            ::Sleep(5);
        }
    }

    INPUT enter[2] = {
        CreateKeyInput(VK_RETURN, false, EXTRA_INFO_MARKER),
        CreateKeyInput(VK_RETURN, true, EXTRA_INFO_MARKER)
    };
    ::SendInput(2, enter, sizeof(INPUT));

    if (release_shift && shift_was_down) {
        if ((::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
            INPUT down = CreateKeyInput(VK_SHIFT, false, EXTRA_INFO_MARKER);
            ::SendInput(1, &down, sizeof(INPUT));
        }
    }
}

} // namespace emebalachat
