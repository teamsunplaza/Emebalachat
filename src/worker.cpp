#include "worker.hpp"
#include "smart_bypass.hpp"
#include "sound.hpp"
#include "win32_input.hpp"

namespace emebalachat {

PipelineWorker::PipelineWorker(AppConfig& config, TranslationManager& engine, FloatingBadge& badge)
    : config_(config), engine_(engine), badge_(badge) {}

PipelineWorker::~PipelineWorker() {
    Stop();
}

void PipelineWorker::Start() {
    if (running_.exchange(true)) return;
    thread_ = std::jthread([this](std::stop_token st) {
        WorkerLoop(st);
    });
}

void PipelineWorker::Stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        while (!queue_.empty()) queue_.pop();
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.request_stop();
        cv_.notify_all();
        thread_.join();
    }
}

bool PipelineWorker::PostTask(bool is_shift_enter, HWND target_hwnd) {
    if (is_busy_.exchange(true, std::memory_order_acquire)) {
        return false; // Busy with existing translation task
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push(PipelineTask{is_shift_enter, target_hwnd});
    }
    cv_.notify_one();
    return true;
}

void PipelineWorker::WorkerLoop(std::stop_token stop_token) {
    while (!stop_token.stop_requested() && running_.load()) {
        PipelineTask task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait(lock, [&]() {
                return stop_token.stop_requested() || !running_.load() || !queue_.empty();
            });

            if (stop_token.stop_requested() || !running_.load()) {
                break;
            }

            if (queue_.empty()) {
                continue;
            }

            task = queue_.front();
            queue_.pop();
        }

        ExecuteTask(task);
    }
}

void PipelineWorker::ExecuteTask(const PipelineTask& task) {
    ClipboardBackup backup;
    BackupClipboard(backup);

    // RAII guard ensuring the user's original clipboard is always restored
    // upon any early return, error, or unexpected exception
    struct RestorerGuard {
        const ClipboardBackup& b;
        bool active = true;
        ~RestorerGuard() {
            if (active) {
                RestoreClipboard(b);
            }
        }
    } restorer{backup};

    // RAII guard ensuring is_busy_ is released upon function exit
    struct BusyGuard {
        std::atomic<bool>& busy;
        ~BusyGuard() {
            busy.store(false, std::memory_order_release);
        }
    } busy_guard{is_busy_};

    FlushIme();

    // I4: this runs on the pipeline worker thread while the UI/hook threads may
    // mutate the shared string fields; take one consistent locked snapshot.
    const AppConfig::Snapshot snap = config_.GetSnapshot();

    std::wstring line = CopySelectedText(task.target_hwnd);

    // Check if line is empty or smart bypass says no translation needed
    if (line.empty() || !ShouldTranslate(line, snap.target_language, snap.source_language)) {
        // Clear text selection with right arrow
        INPUT unsel[2] = {};
        unsel[0].type = INPUT_KEYBOARD;
        unsel[0].ki.wVk = VK_RIGHT;
        unsel[0].ki.dwExtraInfo = EXTRA_INFO_MARKER;
        unsel[1] = unsel[0];
        unsel[1].ki.dwFlags = KEYEVENTF_KEYUP;
        ::SendInput(2, unsel, sizeof(INPUT));

        SendEnterKey(task.is_shift_enter);
        return;
    }

    // Indicate translating state on UI pill
    badge_.SetStatus(BadgeStatus::Translating);

    // Perform translation via active engine
    std::wstring translated = engine_.Translate(line, snap.source_language, snap.target_language);

    // Restore active state
    badge_.SetStatus(BadgeStatus::Active);

    bool pasted = false;
    if (!translated.empty() && translated != line) {
        // H1 guard: pass the captured target HWND. PasteAndRestore re-verifies the
        // foreground window immediately before Ctrl+V and aborts on mismatch.
        pasted = PasteAndRestore(translated, backup, task.target_hwnd);
        if (pasted) {
            // Clipboard swap consumed the backup; RAII restorer must not overwrite.
            restorer.active = false;
        }
        // If !pasted (focus shifted): clipboard untouched, nothing leaked, the
        // user's original text selection is still active in the (new) foreground
        // window — do NOT send unselect or Enter into the wrong window.
    } else {
        // Translation failed or text was unchanged: clear text selection with right arrow
        // to prevent SendEnterKey from deleting or overwriting the user's original text!
        INPUT unsel[2] = {};
        unsel[0].type = INPUT_KEYBOARD;
        unsel[0].ki.wVk = VK_RIGHT;
        unsel[0].ki.dwExtraInfo = EXTRA_INFO_MARKER;
        unsel[1] = unsel[0];
        unsel[1].ki.dwFlags = KEYEVENTF_KEYUP;
        ::SendInput(2, unsel, sizeof(INPUT));
        ::Sleep(10);
    }

    // Auto-Send or Shift+Enter immediate send dispatch
    // (auto_send is std::atomic; implicit load is safe from this thread - I4)
    if (task.is_shift_enter || config_.auto_send.load(std::memory_order_relaxed)) {
        // H1 guard: Enter is a synthetic keystroke delivered to the CURRENT focus.
        // Re-verify before dispatch so a focus race cannot send Enter to the
        // wrong application (e.g. confirming a dialog, submitting a form).
        if (IsSameWindowForInjection(task.target_hwnd, ::GetForegroundWindow())) {
            SendEnterKey(task.is_shift_enter);
        }
    }
}

} // namespace emebalachat
