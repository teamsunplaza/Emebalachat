#pragma once

#include <windows.h>

namespace emebalachat {
namespace ui {

// REQ-R15 (audit §5 latent item 3): multi-monitor / mixed-DPI correctness.
//
// Before this module the process declared NO DPI awareness at all (no manifest,
// no SetProcessDpiAwareness call anywhere in src/), so Windows ran it in
// DPI-UNWARE mode: the DWM bitmap-scales every window, per-monitor DPI is never
// queried, and all layered-window DIB buffers (badge / drag icon / tooltip)
// render their DIP-sized layout 1:1 into physical pixels - tiny and blurry on
// high-DPI panels and impossible to keep aligned with the per-monitor physical
// mouse coordinates the LL hooks deliver. README already advertises
// "PerMonitorV2" awareness; this module makes that claim true and scales the
// GDI/D2D surfaces to the monitor each window actually lands on.

// Reference DPI of the Windows DIP coordinate space.
inline constexpr UINT kDpiBase = 96;

// ---- pure, headless-testable coordinate/scale math (no Win32 calls) ----

// Converts a DIP size to physical pixels for a given monitor DPI, rounding to
// nearest (same convention as MulDiv - the rounding addend is half the
// DIVISOR): 32 DIP @144 dpi -> 48 px, 240 @192 -> 480, 1 @144 -> 2.
// dpi<96 is clamped to kDpiBase so a failed DPI query can neither divide by
// zero nor scale the UI below 100%. int64 intermediate prevents overflow for
// sane screen sizes (dips <= 32767, dpi <= 480 -> << 2^63).
constexpr int ScaleDipsToPixels(int dips, UINT dpi) {
    const UINT d = (dpi >= 96) ? dpi : kDpiBase;
    return static_cast<int>((static_cast<long long>(dips) * static_cast<long long>(d) +
                             static_cast<long long>(kDpiBase / 2)) /
                            static_cast<long long>(kDpiBase));
}

// Inverse of ScaleDipsToPixels: physical pixels -> DIP for a given DPI
// (round-to-nearest). Used for hit-testing constants authored in DIP space.
constexpr int ScalePixelsToDips(int px, UINT dpi) {
    const UINT d = (dpi >= 96) ? dpi : kDpiBase;
    return static_cast<int>((static_cast<long long>(px) * static_cast<long long>(kDpiBase) +
                             static_cast<long long>(d / 2)) /
                            static_cast<long long>(d));
}

// Clamps a window's top-left so the whole (w x h) physical-pixel rect fits
// inside a monitor work area, pulling the window inward by `margin` px from
// the edge it would overflow. Order matches the historical ShowAt/tooltip/badge
// logic (right/bottom first, then left/top) so migrating call sites keep their
// exact behavior on single-monitor 96 dpi setups. Works on virtual-screen
// coordinates (negative left/top secondary monitors included) because all
// comparisons are against the work-rect edges themselves.
constexpr POINT ClampWindowOrigin(int x, int y, int w, int h, int margin, const RECT& work) {
    if (x + w > work.right) {
        x = work.right - w - margin;
    }
    if (y + h > work.bottom) {
        y = work.bottom - h - margin;
    }
    if (x < work.left) {
        x = work.left + margin;
    }
    if (y < work.top) {
        y = work.top + margin;
    }
    return POINT{ x, y };
}

// ---- Win32 seams ----

// Declares process-wide Per-Monitor-V2 DPI awareness (Win10 1703+). MUST be
// called before the first window is created. Uses GetProcAddress on user32 to
// avoid a load-time dependency on newer export ordinals, and falls back to
// shcore SetProcessDpiAwareness(PM), then SetProcessDPIAware (system-aware).
// Returns true when the resulting process awareness is per-monitor (V1 or V2);
// false (traced by the caller) when the session stayed system/unaware.
bool EnsurePerMonitorV2ProcessDpiAwareness();

// Effective DPI of the monitor nearest to `pt` (GetDpiForMonitor /
// MDT_EFFECTIVE_DPI via shcore, resolved at runtime). Never 0: falls back to
// the primary-monitor device DPI, then 96. This is the correct source for
// positioning windows at LL-mouse-hook coordinates, which are PHYSICAL pixels
// of the monitor under the cursor even before the window exists there.
UINT MonitorDpiAtPoint(POINT pt);

// DPI of an existing window (GetDpiForWindow on Win10+; falls back to
// MonitorDpiAtPoint of the window rect center, then 96).
UINT WindowDpi(HWND hwnd);

} // namespace ui
} // namespace emebalachat
