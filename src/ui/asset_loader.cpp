#include "asset_loader.hpp"

#include <filesystem>
#include <vector>

#include "../diag_logger.hpp"

namespace emebalachat {

namespace {

// DP-1 fix (REQ-024 follow-up), W6/C3 extraction: the WIC ICO codec exposes
// frames in directory order, so GetFrame(0) of Emebala_Chat_Appicon.ico is
// the 16x16 LOWEST frame, not the highest resolution (B-1 probe: 7 frames
// 16/24/32/48/64/128/256). For multi-frame containers pick the frame with the
// largest pixel area; single-frame formats (PNG etc.) keep index 0. Frame-
// enumeration failures degrade to index 0. Shared by LoadWicBitmap (D2D path)
// and LoadWicIconPixels (GDI/tray path, W6/C3) so there is exactly ONE
// frame-selection implementation in the codebase.
UINT SelectLargestFrameIndex(IWICBitmapDecoder* pDecoder,
                             UINT* outFrameW,
                             UINT* outFrameH,
                             const char* logTag) {
    UINT selectedIndex = 0;
    UINT frameCount = 0;
    if (FAILED(pDecoder->GetFrameCount(&frameCount)) || frameCount <= 1) {
        if (outFrameW) *outFrameW = 0;
        if (outFrameH) *outFrameH = 0;
        return selectedIndex;
    }
    UINT64 bestArea = 0;
    UINT bestWidth = 0;
    UINT bestHeight = 0;
    for (UINT i = 0; i < frameCount; ++i) {
        IWICBitmapFrameDecode* pProbe = nullptr;
        if (FAILED(pDecoder->GetFrame(i, &pProbe))) {
            continue; // unreadable frame: skip, keep best-so-far
        }
        UINT w = 0;
        UINT h = 0;
        const HRESULT hrSize = pProbe->GetSize(&w, &h);
        pProbe->Release();
        if (FAILED(hrSize)) {
            continue;
        }
        const UINT64 area = static_cast<UINT64>(w) * h;
        if (area > bestArea) {
            bestArea = area;
            bestWidth = w;
            bestHeight = h;
            selectedIndex = i;
        }
    }
    DIAG_LOG("ASSET_LOADER",
             "%s multi-frame container: frames=%u selected_idx=%u selected=%ux%u",
             logTag, frameCount, selectedIndex, bestWidth, bestHeight);
    if (outFrameW) *outFrameW = bestWidth;
    if (outFrameH) *outFrameH = bestHeight;
    return selectedIndex;
}

std::wstring FindAssetPath(const std::vector<std::string>& filenames) {
    std::vector<std::filesystem::path> baseDirs;

    // 1. Check relative to current executable module
    wchar_t exePath[MAX_PATH] = {0};
    if (::GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        std::filesystem::path p(exePath);
        std::filesystem::path dir = p.parent_path();
        baseDirs.push_back(dir / "assets");
        baseDirs.push_back(dir / ".." / "assets");
        baseDirs.push_back(dir / ".." / ".." / "assets");
    }

    // 2. Check current working directory
    try {
        std::filesystem::path cwd = std::filesystem::current_path();
        baseDirs.push_back(cwd / "assets");
        baseDirs.push_back(cwd / ".." / "assets");
        baseDirs.push_back(cwd / ".." / ".." / "assets");
    } catch (...) {}

    for (const auto& fn : filenames) {
        for (const auto& base : baseDirs) {
            std::filesystem::path candidate = base / fn;
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec)) {
                return candidate.wstring();
            }
        }
    }

    return L"";
}

} // namespace

std::wstring FindLogoPath() {
    return FindAssetPath({ "Emebala_Chat_Logo_small.png", "logo.png" });
}

// REQ-024: Emebala_Chat_Appicon.ico is now the primary candidate so the
// floating bar medallion and tray icon render the branded .ico asset.
// WIC's built-in ICO codec decodes it through the existing
// CreateDecoderFromFilename pipeline (LoadWicBitmap), which selects the
// largest frame of multi-frame containers (DP-1 fix, see LoadWicBitmap).
// All previous candidates are kept as fallbacks (no removals).
std::wstring FindAppIconPath() {
    return FindAssetPath({ "Emebala_Chat_Appicon.ico", "Emebala_Chat_Appicon_small.png", "Emebala_Chat_Appicon.png", "Emebala_Chat_Logo_small.png", "logo.png" });
}

HRESULT LoadWicBitmap(
    ID2D1RenderTarget* pRenderTarget,
    const std::wstring& filePath,
    ID2D1Bitmap** ppBitmap
) {
    if (!pRenderTarget || filePath.empty() || !ppBitmap) {
        return E_INVALIDARG;
    }
    *ppBitmap = nullptr;

    IWICImagingFactory* pWicFactory = nullptr;
    HRESULT hr = ::CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&pWicFactory)
    );
    if (FAILED(hr)) {
        return hr;
    }

    IWICBitmapDecoder* pDecoder = nullptr;
    hr = pWicFactory->CreateDecoderFromFilename(
        filePath.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnLoad,
        &pDecoder
    );

    IWICBitmapFrameDecode* pSource = nullptr;
    if (SUCCEEDED(hr)) {
        // Contract unchanged: any final failure still returns a null bitmap
        // to the existing callers' fallback logic.
        const UINT selectedIndex =
            SelectLargestFrameIndex(pDecoder, nullptr, nullptr, "LoadWicBitmap/001");
        hr = pDecoder->GetFrame(selectedIndex, &pSource);
    }

    IWICFormatConverter* pConverter = nullptr;
    if (SUCCEEDED(hr)) {
        hr = pWicFactory->CreateFormatConverter(&pConverter);
    }

    if (SUCCEEDED(hr)) {
        // Convert to Premultiplied 32-bit BGRA matching Direct2D render target format
        hr = pConverter->Initialize(
            pSource,
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0f,
            WICBitmapPaletteTypeMedianCut
        );
    }

    if (SUCCEEDED(hr)) {
        hr = pRenderTarget->CreateBitmapFromWicBitmap(
            pConverter,
            nullptr,
            ppBitmap
        );
    }

    if (pConverter) pConverter->Release();
    if (pSource) pSource->Release();
    if (pDecoder) pDecoder->Release();
    if (pWicFactory) pWicFactory->Release();

    return hr;
}

// W6/C3: see header contract. Same DP-1 frame selection as LoadWicBitmap via
// SelectLargestFrameIndex; scaler + PBGRA converter mirror the tray's old
// hand-rolled path minus the frame-0 defect and the D2D render-target bind.
HRESULT LoadWicIconPixels(
    const std::wstring& filePath,
    UINT targetSize,
    std::vector<uint32_t>& outPixels,
    UINT* outSelectedFrameSize
) {
    if (filePath.empty() || targetSize == 0) {
        return E_INVALIDARG;
    }
    outPixels.clear();
    if (outSelectedFrameSize) {
        *outSelectedFrameSize = 0;
    }

    IWICImagingFactory* pWicFactory = nullptr;
    HRESULT hr = ::CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&pWicFactory)
    );
    if (FAILED(hr)) {
        return hr;
    }

    IWICBitmapDecoder* pDecoder = nullptr;
    hr = pWicFactory->CreateDecoderFromFilename(
        filePath.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnLoad,
        &pDecoder
    );

    IWICBitmapFrameDecode* pSource = nullptr;
    if (SUCCEEDED(hr)) {
        UINT frameW = 0;
        UINT frameH = 0;
        const UINT selectedIndex =
            SelectLargestFrameIndex(pDecoder, &frameW, &frameH, "LoadWicIconPixels/001");
        hr = pDecoder->GetFrame(selectedIndex, &pSource);
        if (SUCCEEDED(hr) && outSelectedFrameSize) {
            *outSelectedFrameSize = frameW;
        }
    }

    IWICBitmapScaler* pScaler = nullptr;
    if (SUCCEEDED(hr)) {
        hr = pWicFactory->CreateBitmapScaler(&pScaler);
    }
    if (SUCCEEDED(hr)) {
        hr = pScaler->Initialize(
            pSource,
            targetSize,
            targetSize,
            WICBitmapInterpolationModeHighQualityCubic
        );
    }

    IWICFormatConverter* pConverter = nullptr;
    if (SUCCEEDED(hr)) {
        hr = pWicFactory->CreateFormatConverter(&pConverter);
    }
    if (SUCCEEDED(hr)) {
        // Premultiplied 32-bit BGRA: byte order B,G,R,A matches the little-
        // endian uint32 0xAARRGGBB layout of a 32bpp top-down GDI DIB section.
        hr = pConverter->Initialize(
            pScaler,
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0f,
            WICBitmapPaletteTypeMedianCut
        );
    }

    if (SUCCEEDED(hr)) {
        outPixels.resize(static_cast<size_t>(targetSize) * targetSize);
        const UINT stride = targetSize * sizeof(uint32_t);
        const UINT bufferSize = stride * targetSize;
        hr = pConverter->CopyPixels(
            nullptr,
            stride,
            bufferSize,
            reinterpret_cast<BYTE*>(outPixels.data())
        );
        if (FAILED(hr)) {
            outPixels.clear();
        }
    }

    if (pConverter) pConverter->Release();
    if (pScaler) pScaler->Release();
    if (pSource) pSource->Release();
    if (pDecoder) pDecoder->Release();
    if (pWicFactory) pWicFactory->Release();

    return hr;
}

void DrawTabletLogoVector(
    ID2D1RenderTarget* pRenderTarget,
    const D2D1_RECT_F& rect,
    bool hovered
) {
    if (!pRenderTarget) return;

    // Outer diptych tablet container
    D2D1_ROUNDED_RECT tablet = D2D1::RoundedRect(rect, 3.0f, 3.0f);

    ID2D1SolidColorBrush* pageBgBrush = nullptr;
    ID2D1SolidColorBrush* goldBrush = nullptr;
    ID2D1SolidColorBrush* darkGoldBrush = nullptr;

    pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0x182029, 0.95f), &pageBgBrush);
    pRenderTarget->CreateSolidColorBrush(
        hovered ? D2D1::ColorF(0xE5C158, 1.0f) : D2D1::ColorF(0xD4AF37, 0.92f),
        &goldBrush
    );
    pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0x9E7B30, 0.85f), &darkGoldBrush);

    if (pageBgBrush) {
        pRenderTarget->FillRoundedRectangle(tablet, pageBgBrush);
    }
    if (goldBrush) {
        pRenderTarget->DrawRoundedRectangle(tablet, goldBrush, hovered ? 1.5f : 1.0f);
    }

    float width = rect.right - rect.left;
    float height = rect.bottom - rect.top;
    float midX = rect.left + width * 0.5f;

    // Central tablet crease/spine
    if (darkGoldBrush) {
        pRenderTarget->DrawLine(
            D2D1::Point2F(midX, rect.top + 2.0f),
            D2D1::Point2F(midX, rect.bottom - 2.0f),
            darkGoldBrush,
            1.2f
        );
    }

    // Top Cuneiform Frieze Line (Left & Right)
    float friezeY = rect.top + height * 0.28f;
    if (goldBrush) {
        pRenderTarget->DrawLine(
            D2D1::Point2F(rect.left + 3.0f, friezeY),
            D2D1::Point2F(midX - 2.0f, friezeY),
            goldBrush,
            0.9f
        );
        pRenderTarget->DrawLine(
            D2D1::Point2F(midX + 2.0f, friezeY),
            D2D1::Point2F(rect.right - 3.0f, friezeY),
            goldBrush,
            0.9f
        );
    }

    // Left Page: Stylized Enki Living Water curves
    if (goldBrush) {
        float waterLeft = rect.left + width * 0.22f;
        float waterMidY = rect.top + height * 0.55f;
        pRenderTarget->DrawLine(
            D2D1::Point2F(waterLeft, waterMidY - 2.0f),
            D2D1::Point2F(waterLeft + width * 0.16f, waterMidY + 2.0f),
            goldBrush,
            1.2f
        );
        pRenderTarget->DrawLine(
            D2D1::Point2F(waterLeft, waterMidY + 3.0f),
            D2D1::Point2F(waterLeft + width * 0.16f, waterMidY + 7.0f),
            goldBrush,
            1.2f
        );
    }

    // Right Page: Stylized Winged Bull / Tree of life curves
    if (goldBrush) {
        float wingLeft = midX + width * 0.12f;
        float wingTop = rect.top + height * 0.40f;
        // Wing upward sweep
        pRenderTarget->DrawLine(
            D2D1::Point2F(wingLeft, wingTop + 7.0f),
            D2D1::Point2F(wingLeft + width * 0.22f, wingTop),
            goldBrush,
            1.2f
        );
        // Tree trunk
        pRenderTarget->DrawLine(
            D2D1::Point2F(wingLeft + width * 0.10f, wingTop + 2.0f),
            D2D1::Point2F(wingLeft + width * 0.10f, rect.bottom - 4.0f),
            darkGoldBrush ? darkGoldBrush : goldBrush,
            1.0f
        );
    }

    // Bottom Inscription Bar
    if (darkGoldBrush) {
        float barY = rect.bottom - height * 0.16f;
        pRenderTarget->DrawLine(
            D2D1::Point2F(rect.left + 5.0f, barY),
            D2D1::Point2F(rect.right - 5.0f, barY),
            darkGoldBrush,
            1.0f
        );
    }

    if (darkGoldBrush) darkGoldBrush->Release();
    if (goldBrush) goldBrush->Release();
    if (pageBgBrush) pageBgBrush->Release();
}

} // namespace emebalachat
