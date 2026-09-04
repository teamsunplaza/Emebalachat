#pragma once

#include <atomic>
#include <functional>
#include <thread>
#include <windows.h>

namespace emebalachat {

class MouseHook {
public:
    // Callback invoked when mouse drag release exceeds threshold (> 15px)
    using DragReleaseCallback = std::function<void(int x, int y)>;

    // Callback invoked on mouse button down (used for outside-click dismissals)
    using MouseDownCallback = std::function<void(int x, int y)>;

    MouseHook();
    ~MouseHook();

    bool Start();
    void Stop();

    void SetDragReleaseCallback(DragReleaseCallback cb);
    void SetMouseDownCallback(MouseDownCallback cb);

    void SetEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
    bool IsEnabled() const { return enabled_.load(std::memory_order_relaxed); }
    int GetClickCount() const { return click_count_; }

private:
    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    void HookThreadProc();

    std::atomic<bool> enabled_{true};
    std::atomic<bool> running_{false};
    DWORD hook_thread_id_ = 0;
    HHOOK hHook_ = nullptr;
    HANDLE hReadyEvent_ = nullptr;
    std::thread thread_;
    // Delayed double/triple-click callback thread. Joined in Stop() before
    // member teardown so it can never touch freed memory (fixes UAF where a
    // detached thread captured raw s_instance and dereferenced it post-destroy).
    std::thread delayed_click_thread_;

    // Drag and multi-click detection state
    bool is_lbutton_down_ = false;
    POINT lbutton_down_pt_ = {};
    DWORD lbutton_down_time_ = 0;

    DWORD last_click_time_ = 0;
    POINT last_click_pt_ = {};
    int click_count_ = 0;
    std::atomic<uint64_t> click_seq_{0};

    DragReleaseCallback drag_cb_;
    MouseDownCallback mouse_down_cb_;

    static MouseHook* s_instance;
};

} // namespace emebalachat
