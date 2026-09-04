#include "mouse_hook.hpp"
#include "win32_input.hpp"
#include <cmath>

namespace emebalachat {

MouseHook* MouseHook::s_instance = nullptr;

MouseHook::MouseHook() {
    s_instance = this;
    hReadyEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

MouseHook::~MouseHook() {
    Stop();
    if (s_instance == this) {
        s_instance = nullptr;
    }
    if (hReadyEvent_) {
        ::CloseHandle(hReadyEvent_);
        hReadyEvent_ = nullptr;
    }
}

bool MouseHook::Start() {
    if (running_.exchange(true)) {
        return true;
    }

    if (hReadyEvent_) {
        ::ResetEvent(hReadyEvent_);
    }

    thread_ = std::thread(&MouseHook::HookThreadProc, this);

    if (hReadyEvent_) {
        ::WaitForSingleObject(hReadyEvent_, 2000);
    }

    return hHook_ != nullptr;
}

void MouseHook::Stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (hook_thread_id_ != 0) {
        ::PostThreadMessageW(hook_thread_id_, WM_QUIT, 0, 0);
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

void MouseHook::SetDragReleaseCallback(DragReleaseCallback cb) {
    drag_cb_ = std::move(cb);
}

void MouseHook::SetMouseDownCallback(MouseDownCallback cb) {
    mouse_down_cb_ = std::move(cb);
}

void MouseHook::HookThreadProc() {
    hook_thread_id_ = ::GetCurrentThreadId();
    HINSTANCE hInst = ::GetModuleHandleW(nullptr);

    hHook_ = ::SetWindowsHookExW(
        WH_MOUSE_LL,
        MouseHook::LowLevelMouseProc,
        hInst,
        0
    );

    if (hReadyEvent_) {
        ::SetEvent(hReadyEvent_);
    }

    if (!hHook_) {
        running_.store(false);
        return;
    }

    MSG msg = {};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    if (hHook_) {
        ::UnhookWindowsHookEx(hHook_);
        hHook_ = nullptr;
    }
}

LRESULT CALLBACK MouseHook::LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION || !s_instance) {
        return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);

    // Bypass synthetic input events
    if (ms->dwExtraInfo == EXTRA_INFO_MARKER) {
        return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    if (!s_instance->enabled_.load(std::memory_order_relaxed)) {
        return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    if (wParam == WM_LBUTTONDOWN) {
        DWORD now = ms->time;
        int d_last_x = std::abs(ms->pt.x - s_instance->last_click_pt_.x);
        int d_last_y = std::abs(ms->pt.y - s_instance->last_click_pt_.y);
        UINT max_dbl_time = ::GetDoubleClickTime();
        int max_dbl_cx = ::GetSystemMetrics(SM_CXDOUBLECLK);
        int max_dbl_cy = ::GetSystemMetrics(SM_CYDOUBLECLK);

        if ((now - s_instance->last_click_time_ <= max_dbl_time) &&
            (d_last_x <= max_dbl_cx) && (d_last_y <= max_dbl_cy)) {
            s_instance->click_count_++;
        } else {
            s_instance->click_count_ = 1;
        }

        s_instance->last_click_time_ = now;
        s_instance->last_click_pt_ = ms->pt;
        s_instance->click_seq_++;

        s_instance->is_lbutton_down_ = true;
        s_instance->lbutton_down_pt_ = ms->pt;
        s_instance->lbutton_down_time_ = ms->time;

        if (s_instance->mouse_down_cb_) {
            s_instance->mouse_down_cb_(ms->pt.x, ms->pt.y);
        }
    } else if (wParam == WM_MOUSEMOVE) {
        // O(1) performance guarantee: do no heavy computation during mouse movement
        return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
    } else if (wParam == WM_LBUTTONUP) {
        if (s_instance->is_lbutton_down_) {
            s_instance->is_lbutton_down_ = false;

            int dx = ms->pt.x - s_instance->lbutton_down_pt_.x;
            int dy = ms->pt.y - s_instance->lbutton_down_pt_.y;
            int dist_sq = dx * dx + dy * dy;

            // Trigger drag release when Euclidean drag distance exceeds 15 pixels
            if (dist_sq >= (15 * 15)) {
                if (s_instance->drag_cb_) {
                    s_instance->drag_cb_(ms->pt.x, ms->pt.y);
                }
            } else if (s_instance->click_count_ >= 2) {
                // Double-click (word selection) or triple-click (paragraph selection)
                // 60ms delay lets target app complete text selection highlight
                uint64_t current_seq = ++s_instance->click_seq_;
                POINT pt = ms->pt;
                std::thread([this_inst = s_instance, cb = s_instance->drag_cb_, pt, current_seq]() {
                    ::Sleep(60);
                    if (this_inst && this_inst->running_.load(std::memory_order_relaxed) &&
                        this_inst->click_seq_.load() == current_seq) {
                        if (cb) {
                            cb(pt.x, pt.y);
                        }
                    }
                }).detach();
            }
        }
    }

    return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
}

} // namespace emebalachat
