#pragma once

#include "config.hpp"
#include "ui/badge.hpp"
#include "ui/tray.hpp"
#include "worker.hpp"

#include <atomic>
#include <functional>
#include <thread>
#include <windows.h>

namespace emebalachat {

class KeyboardHook {
public:
    KeyboardHook(AppConfig& config, PipelineWorker& worker, FloatingBadge& badge, SystemTray& tray);
    ~KeyboardHook();

    bool Start();
    void Stop();

    void SetActive(bool active);
    bool IsActive() const { return is_active_.load(std::memory_order_relaxed); }
    void ToggleActive();
    void CycleTargetLanguage();
    void ToggleAutoSend();

    void SetDoubleCtrlCCallback(std::function<void()> cb) { double_ctrl_c_cb_ = std::move(cb); }
    void SetEscCallback(std::function<bool()> cb) { esc_cb_ = std::move(cb); }

private:
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    void HookThreadProc();

    AppConfig& config_;
    PipelineWorker& worker_;
    FloatingBadge& badge_;
    SystemTray& tray_;

    std::atomic<bool> is_active_{true};
    std::atomic<bool> running_{false};
    DWORD hook_thread_id_ = 0;
    HHOOK hHook_ = nullptr;
    HANDLE hReadyEvent_ = nullptr;
    std::thread thread_;

    DWORD last_ctrl_c_time_ = 0;
    std::function<void()> double_ctrl_c_cb_;
    std::function<bool()> esc_cb_;

    static KeyboardHook* s_instance;
};

} // namespace emebalachat
