#pragma once

#include "config.hpp"
#include "engine.hpp"
#include "ui/badge.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

namespace emebalachat {

struct PipelineTask {
    bool is_shift_enter = false;
};

class PipelineWorker {
public:
    PipelineWorker(AppConfig& config, TranslationManager& engine, FloatingBadge& badge);
    ~PipelineWorker();

    void Start();
    void Stop();

    // Enqueues a translation pipeline task if not already busy.
    // Returns true if task was accepted, false if currently busy.
    bool PostTask(bool is_shift_enter);

    // Returns true if the worker is actively executing a task.
    bool IsBusy() const { return is_busy_.load(std::memory_order_relaxed); }

private:
    void WorkerLoop(std::stop_token stop_token);
    void ExecuteTask(const PipelineTask& task);

    AppConfig& config_;
    TranslationManager& engine_;
    FloatingBadge& badge_;

    std::atomic<bool> is_busy_{false};
    std::atomic<bool> running_{false};

    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::queue<PipelineTask> queue_;
    std::jthread thread_;
};

} // namespace emebalachat
