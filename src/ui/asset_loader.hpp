#pragma once

#include <string>
#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>

namespace emebalachat {

// Searches filesystem for logo in known candidate paths (prioritizes Emebala_Chat_Logo_small.png)
std::wstring FindLogoPath();

// Searches filesystem for app icon in known candidate paths (prioritizes Emebala_Chat_Appicon_small.png)
std::wstring FindAppIconPath();

// Loads a bitmap from file using WIC and creates an ID2D1Bitmap matching the render target format
HRESULT LoadWicBitmap(
    ID2D1RenderTarget* pRenderTarget,
    const std::wstring& filePath,
    ID2D1Bitmap** ppBitmap
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
