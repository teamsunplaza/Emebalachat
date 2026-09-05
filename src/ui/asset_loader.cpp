#include "asset_loader.hpp"

#include <filesystem>
#include <vector>

namespace emebalachat {

namespace {

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

std::wstring FindAppIconPath() {
    return FindAssetPath({ "Emebala_Chat_Appicon_small.png", "Emebala_Chat_Appicon.png", "Emebala_Chat_Logo_small.png", "logo.png" });
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
        hr = pDecoder->GetFrame(0, &pSource);
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
