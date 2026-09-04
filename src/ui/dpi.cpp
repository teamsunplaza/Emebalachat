#include "dpi.hpp"

#include <cstdio>

// The newer DPI exports (SetProcessDpiAwarenessContext, GetDpiForWindow,
// shcore GetDpiForMonitor/SetProcessDpiAwareness) are resolved through
// GetProcAddress at runtime so the binary still loads on older Windows 10
// builds (boring-technology stability rule); local declarations below avoid
// SDK-header include-order dependencies.
#include <windows.h>

// DPI_AWARENESS_CONTEXT / shcore prototypes we need at compile time without
// forcing <shcore.h> include-order dependencies on every TU.
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
typedef HANDLE DPI_AWARENESS_CONTEXT;
#define DPI_AWARENESS_CONTEXT_UNAWARE ((DPI_AWARENESS_CONTEXT)-1)
#define DPI_AWARENESS_CONTEXT_SYSTEM_AWARE ((DPI_AWARENESS_CONTEXT)-2)
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE ((DPI_AWARENESS_CONTEXT)-3)
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

// MDT_EFFECTIVE_DPI == 0 from knownfolders.h/shcore.h; declaring the enum
// locally avoids the include chain (the values are ABI-fixed by Microsoft).
enum monitor_dpi_type_local { MDT_EFFECTIVE_DPI_LOCAL = 0 };

namespace emebalachat {
namespace ui {

namespace {

// GetDpiForMonitor(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*) via shcore.
typedef HRESULT(WINAPI* GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);

GetDpiForMonitorFn ResolveGetDpiForMonitor() {
    static GetDpiForMonitorFn fn = []() -> GetDpiForMonitorFn {
        HMODULE shcore = ::LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!shcore) {
            return nullptr;
        }
        return reinterpret_cast<GetDpiForMonitorFn>(
            ::GetProcAddress(shcore, "GetDpiForMonitor"));
    }();
    return fn;
}

} // namespace

bool EnsurePerMonitorV2ProcessDpiAwareness() {
    // 1. Win10 1703+: user32!SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)
    typedef BOOL(WINAPI* SetCtxFn)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    auto set_ctx = user32
        ? reinterpret_cast<SetCtxFn>(::GetProcAddress(user32, "SetProcessDpiAwarenessContext"))
        : nullptr;
    if (set_ctx && set_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        return true;
    }
    // A previous awareness call (or a manifest) may already have locked the
    // process context: ERROR_ACCESS_DENIED (5) means "awareness already set".
    // Trust GetProcessDpiAwareness below to judge the effective state.

    // 2. Win8.1+: shcore!SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE=2)
    HMODULE shcore = ::LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (shcore) {
        typedef HRESULT(WINAPI* SetAwareFn)(int);
        auto set_aware = reinterpret_cast<SetAwareFn>(::GetProcAddress(shcore, "SetProcessDpiAwareness"));
        if (set_aware) {
            // S_OK(0)=success, S_FALSE(1)=already set, E_ACCESSDENIED=locked.
            const HRESULT hr = set_aware(2 /*PROCESS_PER_MONITOR_DPI_AWARE*/);
            if (SUCCEEDED(hr)) {
                return true;
            }
        }
    }

    // 3. Legacy fallback: system-DPI-aware. Better than unaware, still not PM.
    typedef BOOL(WINAPI* GetProcAwareFn)(HANDLE, int*);
    auto get_aware = shcore
        ? reinterpret_cast<GetProcAwareFn>(::GetProcAddress(shcore, "GetProcessDpiAwareness"))
        : nullptr;
    if (get_aware) {
        int awareness = 0;
        // 0=unaware,1=system,2=per-monitor(V1),3=per-monitorV2(undocumented)
        if (SUCCEEDED(get_aware(nullptr, &awareness)) && awareness >= 1) {
            fprintf(stderr, "UI/DPI/001: awareness locked at pre-set value %d (manifest or earlier call)\n",
                    awareness);
            return awareness >= 2;
        }
    }
    ::SetProcessDPIAware();
    fprintf(stderr, "UI/DPI/002: per-monitor awareness unavailable; degraded to system-aware\n");
    return false;
}

UINT MonitorDpiAtPoint(POINT pt) {
    HMONITOR hMon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (hMon) {
        if (GetDpiForMonitorFn fn = ResolveGetDpiForMonitor()) {
            UINT dpi_x = 0, dpi_y = 0;
            if (SUCCEEDED(fn(hMon, MDT_EFFECTIVE_DPI_LOCAL, &dpi_x, &dpi_y)) && dpi_x >= 96) {
                return dpi_x;
            }
        }
        // Pre-shcore fallback: CreateDC on the monitor's device name gives that
        // monitor's own device DC (per-monitor LOGPIXELS). GetDC cannot take an
        // HMONITOR (that was the bug class this sweep is meant to catch).
        MONITORINFOEXW mie = {};
        mie.cbSize = sizeof(MONITORINFOEXW);
        if (::GetMonitorInfoW(hMon, &mie)) {
            if (HDC hdc = ::CreateDCW(L"DISPLAY", mie.szDevice, nullptr, nullptr)) {
                const INT logpx = ::GetDeviceCaps(hdc, LOGPIXELSX);
                ::DeleteDC(hdc);
                if (logpx >= 96) {
                    return static_cast<UINT>(logpx);
                }
            }
        }
    }
    if (HDC hdc = ::GetDC(nullptr)) {
        const INT logpx = ::GetDeviceCaps(hdc, LOGPIXELSX);
        ::ReleaseDC(nullptr, hdc);
        if (logpx >= 96) {
            return static_cast<UINT>(logpx);
        }
    }
    return kDpiBase;
}

UINT WindowDpi(HWND hwnd) {
    if (!hwnd) {
        return kDpiBase;
    }
    typedef UINT(WINAPI* GetDpiForWindowFn)(HWND);
    static GetDpiForWindowFn fn = [] {
        HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<GetDpiForWindowFn>(::GetProcAddress(user32, "GetDpiForWindow"))
                      : nullptr;
    }();
    if (fn) {
        const UINT dpi = fn(hwnd);
        if (dpi >= 96) {
            return dpi;
        }
    }
    RECT r = {};
    if (::GetWindowRect(hwnd, &r)) {
        POINT center = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
        return MonitorDpiAtPoint(center);
    }
    return kDpiBase;
}

} // namespace ui
} // namespace emebalachat
