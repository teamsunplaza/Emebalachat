#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include "layered_renderer.hpp"

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
    // 260922_0001 A4 (fix plan §4.1-A): after an EndDraw device-lost, Render()
    // recreates the target and re-renders exactly once. reentry_allowed caps
    // the recursion depth at 2 (top pass true -> recovery pass false); the
    // recovery pass is a plain call on the same (GUI) thread, so the only
    // re-lock it performs is render_mutex_ — hence the recursive mutex below.
    void Render(bool reentry_allowed = true);
    void UpdateAlpha(BYTE alpha);
    void ResetIdleTimer();
    void ReallocateBuffer(int width, int height); // physical px buffer
    // REQ-R15 (audit §5 latent item 3): the layout (current_width_, kHeight)
    // is authored in DIPs; the window, DIB buffer, and UpdateLayeredWindow
    // live in PHYSICAL pixels of the monitor the badge sits on. PhysW/PhysH
    // perform the conversion with the current dpi_.
    int PhysW() const;
    int PhysH() const;
    void RebindRenderTarget();
    // R6 Phase 3 (audit item 4, plan §3.1 A3): recreate the DC render target
    // (+ device-dependent logo bitmap) after EndDraw returns
    // D2DERR_RECREATE_TARGET, so a driver reset cannot leave the badge a
    // permanently blank pill.
    // 260922_0001 A4: returns true only when the target was recreated and
    // rebound — Render() uses it to decide between the one-shot recovery
    // re-render and skipping the UpdateAlpha commit of stale/blank pixels.
    bool RecreateAfterDeviceLost();
    void LoadLogoBitmap();

    mutable std::mutex data_mutex_;
    // 260922_0001 A4: std::mutex -> std::recursive_mutex. The one-shot
    // device-lost recovery pass runs as Render(false) from inside the locked
    // Render() body; a NON-recursive lock_guard would self-deadlock on that
    // re-entry (std::mutex is UB/deadlock when locked by its owning thread).
    // Cross-thread semantics are unchanged: only the GUI message thread ever
    // calls Render (worker-side SetStatus/SetLanguages marshal via
    // PostMessage — see the REQ-R10 seam comments), and the recursion is
    // depth-capped at 1 by reentry_allowed.
    mutable std::recursive_mutex render_mutex_;

    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    BadgeStatus status_ = BadgeStatus::Active;
    std::wstring src_code_ = L"Auto";
    std::wstring tgt_code_ = L"EN";
    bool visible_ = true;
    BYTE current_alpha_ = 217; // 85% opacity default
    bool is_hovered_ = false;
    int current_width_ = 240; // DIP layout width (recomputed by Render())
    UINT dpi_ = 96;           // REQ-R15: DPI of the monitor hosting the badge

    // REF-3.6: GDI DIB + memory DC + DC render-target lifetime moved to the
    // shared RAII owner. dc_render_target_ below is a NON-OWNING alias into
    // renderer_.target(), kept in sync at the three points where the target
    // changes (Create / RecreateAfterDeviceLost / Destroy) so the render code
    // referencing it stays untouched.
    LayeredD2DRenderer renderer_;

    // Direct2D & DirectWrite COM pointers
    ID2D1Factory* d2d_factory_ = nullptr;
    ID2D1DCRenderTarget* dc_render_target_ = nullptr; // alias of renderer_.target()
    ID2D1Bitmap* logo_bitmap_ = nullptr;
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

    bool is_mouse_down_ = false;
    // REQ-052 P3: single-click timer delay, captured once at Create() from
    // GetDoubleClickTime() (the OS double-click window, default 500ms). The
    // old fixed 220ms fired the pause-toggle action before a 250-350ms
    // double-click arrived and could cancel it via WM_LBUTTONDBLCLK.
    UINT single_click_delay_ms_ = 220; // overwritten in Create()
    bool is_dragging_ = false;
    POINT drag_start_cursor_ = {};
    POINT drag_start_window_ = {};
};

} // namespace emebalachat
