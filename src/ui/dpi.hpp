#pragma once

#include <vector>

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

// 261029_0001 U-I (R1-21): monitor-topology re-clamp.
//
// A layered window keeps its absolute virtual-screen coordinates when the
// monitor it sits on disappears (laptop undocked, external display unplugged,
// resolution shrunk), so the floating badge can end up parked where no monitor
// exists and stays invisible until the app restarts - its only clamps run at
// Create() and during a drag. Re-clamping after WM_DISPLAYCHANGE needs the
// monitor SET rather than one work rect, because the monitor the badge used to
// be on may no longer be attached. The selection policy is authored here as
// pure math (headless-testable with synthetic monitor rects); the clamp itself
// stays ClampWindowOrigin above, the single source of truth shared with the
// Create/drag call sites.

// Sentinel index for "no work area": an empty monitor set (headless / RDP
// console session), where callers must skip repositioning instead of inventing
// coordinates.
inline constexpr size_t kNoWorkArea = static_cast<size_t>(-1);

// Squared distance from (px,py) to the nearest point of `r`; 0 when the point
// lies on or inside the rect. int64: virtual-screen coordinates reach
// +/-32767, so a squared difference overflows a 32-bit int.
constexpr long long SquaredDistanceToRect(int px, int py, const RECT& r) {
    const long long dx = (px < r.left)   ? (static_cast<long long>(r.left) - static_cast<long long>(px))
                        : (px > r.right) ? (static_cast<long long>(px) - static_cast<long long>(r.right))
                                         : 0;
    const long long dy = (py < r.top)    ? (static_cast<long long>(r.top) - static_cast<long long>(py))
                       : (py > r.bottom) ? (static_cast<long long>(py) - static_cast<long long>(r.bottom))
                                         : 0;
    return dx * dx + dy * dy;
}

// Index of the work area a window at origin (x,y) belongs to: the nearest
// monitor in virtual-screen space. With the topology unchanged this mirrors
// MonitorFromPoint(MONITOR_DEFAULTTONEAREST) (0 distance inside a monitor);
// when the origin's own monitor is gone it lands on the nearest survivor, so a
// stranded badge is always pulled onto a display that exists. Ties prefer
// `primary_index`, then enumeration order - deterministic, and the primary
// monitor is the "best fit" fallback for degenerate sets. Returns kNoWorkArea
// for a null/empty set.
constexpr size_t NearestWorkAreaIndex(int x, int y, const RECT* areas, size_t count,
                                      size_t primary_index) {
    if (!areas || count == 0) {
        return kNoWorkArea;
    }
    size_t best = 0;
    long long best_dist = SquaredDistanceToRect(x, y, areas[0]);
    for (size_t i = 1; i < count; ++i) {
        const long long dist = SquaredDistanceToRect(x, y, areas[i]);
        // A tie goes to the primary monitor even when it is enumerated later.
        const bool primary_wins = (dist == best_dist) && (i == primary_index) &&
                                  (best != primary_index);
        if (dist < best_dist || primary_wins) {
            best = i;
            best_dist = dist;
        }
    }
    return best;
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

// 261029_0001 U-I (R1-21): work areas (physical px, virtual-screen coords) of
// every monitor currently attached to this desktop. `primary_index` receives
// the index of the primary monitor's rect, or kNoWorkArea when the enumeration
// reports no primary. An EMPTY result means there is no monitor at all
// (headless / RDP console session): the caller must skip repositioning rather
// than clamp into a rectangle it invented.
std::vector<RECT> MonitorWorkAreas(size_t* primary_index = nullptr);

} // namespace ui
} // namespace emebalachat
