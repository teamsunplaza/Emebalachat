#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

namespace emebalachat {

enum class BadgeStatus {
    Active,       // Emerald #10B981
    Translating,  // Amber #F59E0B
    Disabled      // Gray #6B7280
};

class FloatingBadge {
public:
    using ClickCallback = std::function<void()>;

    FloatingBadge();
    ~FloatingBadge();

    // Creates the layered Direct2D pill badge window with optional initial position
    bool Create(HINSTANCE hInstance, std::wstring_view src_code = L"Auto", std::wstring_view tgt_code = L"EN", int initial_x = -1, int initial_y = -1);

    // Closes and destroys the badge window
    void Destroy();

    // Updates status and triggers immediate redraw (thread-safe)
    void SetStatus(BadgeStatus status);

    // Updates source and target language codes (thread-safe)
    void SetLanguages(std::wstring_view src_code, std::wstring_view tgt_code);

    // Shows or hides the badge window
    void SetVisible(bool visible);
    bool IsVisible() const;

    // Repositions badge on desktop
    void SetPosition(int x, int y);

    using ActionCallback = std::function<void()>;
    using PositionCallback = std::function<void(int x, int y)>;

    // Sets callbacks for user interaction
    void SetClickCallback(ActionCallback cb);
    void SetDoubleClickCallback(ActionCallback cb);
    void SetRightClickCallback(ActionCallback cb);
    void SetPositionCallback(PositionCallback cb);

    HWND GetHwnd() const { return hwnd_; }
    int GetCurrentWidth() const { return current_width_; }

    static constexpr UINT WM_BADGE_SET_STATUS = WM_USER + 201;
    static constexpr UINT WM_BADGE_SET_LANGS  = WM_USER + 202;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void Render();
    void UpdateAlpha(BYTE alpha);
    void ResetIdleTimer();
    void ReallocateBuffer(int width, int height);

    mutable std::mutex data_mutex_;
    mutable std::mutex render_mutex_;

    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    BadgeStatus status_ = BadgeStatus::Active;
    std::wstring src_code_ = L"Auto";
    std::wstring tgt_code_ = L"EN";
    bool visible_ = true;
    BYTE current_alpha_ = 217; // 85% opacity default
    bool is_hovered_ = false;
    int current_width_ = 240;

    // GDI DIB & Memory DC
    HDC hMemDC_ = nullptr;
    HBITMAP hBitmap_ = nullptr;
    HBITMAP hOldBitmap_ = nullptr;
    void* pBits_ = nullptr;

    // Direct2D & DirectWrite COM pointers
    ID2D1Factory* d2d_factory_ = nullptr;
    ID2D1DCRenderTarget* dc_render_target_ = nullptr;
    IDWriteFactory* dwrite_factory_ = nullptr;
    IDWriteTextFormat* text_format_ = nullptr;
    IDWriteTextFormat* code_format_ = nullptr;

    ActionCallback click_callback_;
    ActionCallback double_click_callback_;
    ActionCallback right_click_callback_;
    PositionCallback position_callback_;

    static constexpr int kDefaultWidth = 240;
    static constexpr int kHeight = 38;
    static constexpr float kRadius = 19.0f;
    static constexpr UINT_PTR kTimerIdle = 1001;
    static constexpr UINT_PTR kTimerSingleClick = 1002;
    static constexpr DWORD kIdleTimeoutMs = 5000;
    static constexpr DWORD kSingleClickDelayMs = 220;

    bool is_mouse_down_ = false;
    bool is_dragging_ = false;
    POINT drag_start_cursor_ = {};
    POINT drag_start_window_ = {};
};

} // namespace emebalachat
