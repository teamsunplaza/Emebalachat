#pragma once

#include <functional>
#include <mutex>
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

namespace emebalachat {

class DragIconWindow {
public:
    using ClickCallback = std::function<void(int click_x, int click_y)>;

    DragIconWindow();
    ~DragIconWindow();

    bool Create(HINSTANCE hInstance);
    void Destroy();

    void ShowAt(int x, int y);
    void Hide();
    bool IsVisible() const { return visible_; }

    HWND GetHwnd() const { return hwnd_; }
    void SetClickCallback(ClickCallback cb) { click_cb_ = std::move(cb); }

    static constexpr int kSize = 32;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void Render();
    void UpdateLayered();
    void StartFadeoutTimer();
    void StopFadeoutTimer();
    void LoadLogoBitmap();

    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    bool visible_ = false;
    bool is_hovered_ = false;
    BYTE alpha_ = 235;

    HDC hMemDC_ = nullptr;
    HBITMAP hBitmap_ = nullptr;
    HBITMAP hOldBitmap_ = nullptr;
    void* pBits_ = nullptr;

    ID2D1Factory* d2d_factory_ = nullptr;
    ID2D1DCRenderTarget* dc_render_target_ = nullptr;
    ID2D1Bitmap* logo_bitmap_ = nullptr;

    ClickCallback click_cb_;

    static constexpr UINT_PTR kTimerFadeout = 3001;
    static constexpr DWORD kFadeoutTimeoutMs = 2500;
};

} // namespace emebalachat
