#include "asset_loader.hpp"
#include "theme.hpp"

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

    const float width  = rect.right - rect.left;
    const float height = rect.bottom - rect.top;
    if (width <= 0.0f || height <= 0.0f) return;

    const auto& theme = ui::GetActiveTheme();
    const float min_dim = (std::min)(width, height);
    const float radius  = (std::max)(2.5f, min_dim * 0.22f);
    const D2D1_ROUNDED_RECT badge_rr = D2D1::RoundedRect(rect, radius, radius);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1.0f), &brush)) || !brush) {
        return;
    }

    // 1. Minimal geometric emblem container using active theme surface & border tokens.
    brush->SetColor(ui::ColorFromHex(hovered ? theme.surface_hover : theme.surface_elevated, 0.98f));
    pRenderTarget->FillRoundedRectangle(badge_rr, brush.Get());

    if (width > 3.0f && height > 3.0f) {
        const D2D1_RECT_F inner_rect = D2D1::RectF(
            rect.left + 1.0f, rect.top + 1.0f, rect.right - 1.0f, rect.bottom - 1.0f
        );
        const float inner_r = (std::max)(1.5f, radius - 1.0f);
        brush->SetColor(ui::ColorFromHex(theme.border_inner, theme.border_inner_alpha));
        pRenderTarget->DrawRoundedRectangle(D2D1::RoundedRect(inner_rect, inner_r, inner_r), brush.Get(), 1.0f);
    }

    brush->SetColor(hovered
        ? ui::ColorFromHex(theme.border_hover, theme.border_hover_alpha)
        : ui::ColorFromHex(theme.border_subtle, (std::max)(0.25f, theme.border_subtle_alpha)));
    pRenderTarget->DrawRoundedRectangle(badge_rr, brush.Get(), hovered ? 1.25f : 1.0f);

    // 2. Clean minimal geometric translation emblem (opposing horizontal arrows / glyph).
    const D2D1_RECT_F glyph_bounds = D2D1::RectF(
        rect.left   + width  * 0.18f,
        rect.top    + height * 0.18f,
        rect.right  - width  * 0.18f,
        rect.bottom - height * 0.18f
    );
    const float stroke_w = (std::max)(1.2f, min_dim * 0.085f);
    brush->SetColor(ui::ColorFromHex(theme.accent_primary, hovered ? 1.0f : 0.92f));
    ui::DrawSwapIcon(pRenderTarget, glyph_bounds, brush.Get(), stroke_w);
}

} // namespace emebalachat
