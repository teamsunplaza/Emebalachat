#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>

namespace emebalachat {

// Searches filesystem for logo in known candidate paths (prioritizes Emebala_Chat_Logo_small.png)
std::wstring FindLogoPath();

// Searches filesystem for app icon in known candidate paths (prioritizes Emebala_Chat_Appicon.ico — REQ-024)
std::wstring FindAppIconPath();

// Loads a bitmap from file using WIC and creates an ID2D1Bitmap matching the render target format
HRESULT LoadWicBitmap(
    ID2D1RenderTarget* pRenderTarget,
    const std::wstring& filePath,
    ID2D1Bitmap** ppBitmap
);

// W6/C3 (session 260910_0007): GDI-side sibling of LoadWicBitmap for callers
// that cannot bind an ID2D1Bitmap (the tray icon is built into a 32bpp DIB
// section). Decodes the BEST frame of a possibly multi-frame container through
// the SAME DP-1 largest-frame selection LoadWicBitmap uses, scales it to
// targetSize x targetSize (WICBitmapInterpolationModeHighQualityCubic) and
// copies a premultiplied 32bpp BGRA buffer (B in the low byte - GDI DIB
// layout). outPixels receives targetSize*targetSize uint32 pixels on S_OK.
// outSelectedFrameSize (optional, may be null) receives the width of the
// SOURCE frame selected before scaling (0 on failure). This is the single WIC
// pixel-decode owner: tray.cpp MUST NOT re-roll its own decoder.
HRESULT LoadWicIconPixels(
    const std::wstring& filePath,
    UINT targetSize,
    std::vector<uint32_t>& outPixels,
    UINT* outSelectedFrameSize = nullptr
);

// Fallback vector drawing of the Emebala tablet relief logo
void DrawTabletLogoVector(
    ID2D1RenderTarget* pRenderTarget,
    const D2D1_RECT_F& rect,
    bool hovered
);

// R6 Phase 3 (audit item 4 / plan §3.1 A3): the single device-lost decision
// seam shared by every D2D surface (tooltip, badge, drag icon, about). A DC
// render target survives BindDC churn, but a GPU driver reset / desktop
// transition can fail EndDraw with D2DERR_RECREATE_TARGET; without recovery
// the surface renders permanently blank (reads as a "stale/empty tooltip").
// Pure + constexpr so tests/run_tests.cpp pins the classification without a
// GUI (the recreation itself is runtime-only).
constexpr bool IsRecoverableDeviceLost(HRESULT hr) {
    return hr == D2DERR_RECREATE_TARGET;
}

} // namespace emebalachat
