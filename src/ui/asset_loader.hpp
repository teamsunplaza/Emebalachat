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

} // namespace emebalachat
