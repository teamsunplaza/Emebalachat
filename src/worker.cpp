#include "worker.hpp"
#include "diag_logger.hpp"
#include "smart_bypass.hpp"
#include "sound.hpp"
#include "unicode_utils.hpp"
#include "win32_input.hpp"

#include <cstdio>

namespace emebalachat {

namespace {

// REQ-R03: the single selection-release primitive (VK_RIGHT key-down/key-up pair,
// marked synthetic via EXTRA_INFO_MARKER so our own keyboard hook ignores it).
// Every non-paste pipeline outcome routes through this exactly once.
void ReleaseSelectionOnce() {
    INPUT unsel[2] = {};
    unsel[0].type = INPUT_KEYBOARD;
    unsel[0].ki.wVk = VK_RIGHT;
    unsel[0].ki.dwExtraInfo = EXTRA_INFO_MARKER;
    unsel[1] = unsel[0];
    unsel[1].ki.dwFlags = KEYEVENTF_KEYUP;
    ::SendInput(2, unsel, sizeof(INPUT));
    ::Sleep(10);
}

// REQ-R03 path matrix, compile-time proven against the shared header predicate
// (src/worker.hpp) so worker.cpp and the unit tests assert on ONE definition:
//   (a) translated empty (engine failure / consent block)      -> release,
//   (b) translated == line (unchanged text)                    -> release,
//   (c) paste cancelled by the H1 guard (pasted == false)      -> release,
//   (d) successful paste (restorer.active=false, Ctrl+V ate the selection) -> no release.
static_assert(SelectionReleaseRequired(true) == false, "REQ-R03: successful paste must NOT re-release (Ctrl+V already consumed selection)");
static_assert(SelectionReleaseRequired(false) == true, "REQ-R03: every non-paste path MUST release the block selection");

} // namespace

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
    // ---- 260905 diagnostics: full pipeline trace (task #c). Every stage is
    // recorded with content and durations so "what happened in the backend
    // when Enter was pressed, and why" is answerable from the log alone.
    // Keystroke/translation CONTENT logging is user-authorized for this
    // diagnostic build (VP directive), overriding the old shape-only rule. ----
    const ULONGLONG t_task_start = ::GetTickCount64();
    DIAG_LOG("PIPELINE", "task_received source=enter_pipeline target_hwnd=%p shift_enter=%d",
             reinterpret_cast<const void*>(task.target_hwnd), task.is_shift_enter ? 1 : 0);

    ClipboardBackup backup;
    const bool clip_backup_ok = BackupClipboard(backup);
    DIAG_LOG("PIPELINE", "clipboard_backup ok=%d has_text=%d extra_formats=%zu",
             clip_backup_ok ? 1 : 0, backup.text.has_value() ? 1 : 0, backup.formats.size());

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

    // REQ-R17 (audit §5 latent item 5): worker-side backstop for the race
    // window between the hook's mirror check and this point (the user can
    // open a FRESH composition while the task queues, and FlushIme's VK_RIGHT
    // only flushes what was open at flush time). Re-probing the foreground
    // window's IME context here is safe: this is the pipeline worker thread,
    // not the LL hook (ImmGetContext cross-thread SendMessage cannot cause a
    // LowLevelHooksTimeout unhook on this thread). Still composing -> take
    // the same fail-safe bypass the empty-copy path uses: release any block
    // selection and hand Enter to the app, whose IME commits + sends exactly
    // as it would have without us. Copying/pasting mid-composition is the
    // corruption the audit describes ("마지막 글자 중복 복사") and must never
    // happen against a live GCS_COMPSTR.
    if (ForegroundImeComposing()) {
        DIAG_F("WORKER/ExecuteTask/033: IME composition detected at task start; "
                        "bypassing translation (Enter handed to the app's IME)\n");
        DIAG_LOG("PIPELINE", "stage=ime_backstop decision=bypass reason=foreground_composing "
                             "duration_ms=%llu",
                 ::GetTickCount64() - t_task_start);
        ReleaseSelectionOnce();
        SendEnterKey(task.is_shift_enter);
        DIAG_LOG("PIPELINE", "stage=send_enter action=synthetic(reason=ime_backstop) shift=%d",
                 task.is_shift_enter ? 1 : 0);
        return;
    }

    // I4: this runs on the pipeline worker thread while the UI/hook threads may
    // mutate the shared string fields; take one consistent locked snapshot.
    const AppConfig::Snapshot snap = config_.GetSnapshot();
    DIAG_LOG("PIPELINE", "stage=lang_pair src=\"%s\" tgt=\"%s\" engine=%s auto_send=%d",
             snap.source_language.c_str(), snap.target_language.c_str(),
             snap.engine_type.c_str(), snap.auto_send ? 1 : 0);

    const ULONGLONG t_capture_start = ::GetTickCount64();
    DIAG_LOG("PIPELINE", "stage=capture begin target_hwnd=%p",
             reinterpret_cast<const void*>(task.target_hwnd));
    std::wstring line = NormalizeNewlinesToCRLF(CopySelectedText(task.target_hwnd));
    DIAG_LOG("PIPELINE", "stage=capture end len=%zu duration_ms=%llu content=\"%s\"",
             line.size(), ::GetTickCount64() - t_capture_start,
             ToUtf8(line).c_str());

    // R5 observability: log the capture/bypass decision so a silent
    // "nothing translated" can be attributed to the exact failing gate
    // (lengths/booleans only; never the captured content itself).
    const bool was_smart_bypassed =
        !line.empty() && !ShouldTranslate(line, snap.target_language, snap.source_language);
    const bool should_translate = !line.empty() && !was_smart_bypassed;
    DIAG_F("WORKER/ExecuteTask/034: captured %zu chars, should_translate=%d\n",
            line.size(), should_translate ? 1 : 0);
    DIAG_LOG("PIPELINE", "stage=bypass_decision smart_bypass=%d should_translate=%d reason=%s",
             was_smart_bypassed ? 1 : 0, should_translate ? 1 : 0,
             line.empty() ? "capture_empty" : (was_smart_bypassed ? "already_target_language" : "translate"));

    // R5 (Debug-Surgical): the bare-Enter path's empty capture is no longer
    // silent. Gate-11 evidence (R5 report section 2.2): CopySelectedText
    // returned empty, so the user's original text is still sitting in the
    // target app. Passing the Enter through here would SUBMIT it untranslated
    // with no feedback - exactly the reported failure ("Enter sent my text
    // untranslated, no tooltip"). Per the R1 mandate (a tooltip must always
    // land), hold the send in this exact case and surface the existing
    // localized TooltipNoSelection notice via the thread-safe marshal path
    // (SetEmptyCaptureCallback / ShowMessageThreadSafe). The Enter keypress
    // is NOT re-sent: the user's next bare Enter retries the full pipeline.
    // A SMART BYPASS (already-in-target) is a positive product decision, not
    // a failure: it keeps the historical ReleaseSelectionOnce + send-through
    // behavior and must NOT trigger the hold (pinned by EmptyCaptureNeedsHold).
    // The hold applies ONLY when a notice can actually land (empty_capture_cb_
    // registered at startup); without the seam this degrades to the legacy
    // send-through below rather than creating a new silent-swallow path.
    if (EmptyCaptureNeedsHold(line.empty(), was_smart_bypassed) && empty_capture_cb_) {
        DIAG_F(
                "WORKER/ExecuteTask/035: empty capture on bare-Enter path; holding send, "
                "showing no-selection notice (smart_bypass=%d)\n",
                was_smart_bypassed ? 1 : 0);
        DIAG_LOG("PIPELINE", "stage=empty_capture decision=hold_send action=no_selection_notice "
                             "duration_ms=%llu",
                 ::GetTickCount64() - t_task_start);
        ReleaseSelectionOnce();
        empty_capture_cb_();
        return;
    }

    // Check if line is empty or smart bypass says no translation needed
    if (line.empty() || !should_translate) {
        DIAG_LOG("PIPELINE", "stage=send_through decision=bypass_pipeline reason=%s "
                             "duration_ms=%llu",
                 line.empty() ? "empty_capture_no_notice" : "smart_bypass",
                 ::GetTickCount64() - t_task_start);
        // No paste will happen on this path: release the block selection before
        // Enter so the (about to be sent) text cannot be clobbered (REQ-R03).
        ReleaseSelectionOnce();
        SendEnterKey(task.is_shift_enter);
        return;
    }

    // Indicate translating state on UI pill
    badge_.SetStatus(BadgeStatus::Translating);
    DIAG_LOG("PIPELINE", "stage=translate begin engine=%s src_len=%zu",
             engine_.GetActiveEngineName().c_str(), line.size());

    // REQ-R02: capture the explicit engine status. An empty result is no longer
    // silent - the status below distinguishes privacy-block from engine failure.
    TranslationStatus status = TranslationStatus::Ok;
    const ULONGLONG t_translate_start = ::GetTickCount64();
    std::wstring translated =
        NormalizeNewlinesToCRLF(engine_.Translate(line, snap.source_language, snap.target_language, &status));
    DIAG_LOG("PIPELINE", "stage=translate end status=%d engine=%s duration_ms=%llu "
                         "out_len=%zu out=\"%s\"",
             static_cast<int>(status), engine_.GetActiveEngineName().c_str(),
             ::GetTickCount64() - t_translate_start, translated.size(),
             ToUtf8(translated).c_str());

    // Restore active state
    badge_.SetStatus(BadgeStatus::Active);

    bool pasted = false;
    if (!translated.empty() && translated != line) {
        // H1 guard: pass the captured target HWND. PasteAndRestore re-verifies the
        // foreground window immediately before Ctrl+V and aborts on mismatch.
        const ULONGLONG t_paste_start = ::GetTickCount64();
        pasted = PasteAndRestore(translated, backup, task.target_hwnd);
        DIAG_LOG("PIPELINE", "stage=paste result=%d target_hwnd=%p duration_ms=%llu "
                             "clipboard_restored=%d",
                 pasted ? 1 : 0, reinterpret_cast<const void*>(task.target_hwnd),
                 ::GetTickCount64() - t_paste_start, pasted ? 0 : 1);
        // If !pasted (focus shifted): clipboard untouched, nothing leaked. The
        // block selection is still live and MUST be released - a VK_RIGHT into a
        // possibly different foreground window is a benign cursor move, while
        // leaving it selected destroys the user's whole message on their next
        // keystroke (audit §2.2 / §5-3, REQ-R03). Enter remains H1-gated below.
        if (pasted) {
            // Clipboard swap consumed the backup; RAII restorer must not overwrite.
            restorer.active = false;
        }
    } else {
        DIAG_LOG("PIPELINE", "stage=paste skipped reason=%s",
                 translated.empty() ? "translation_empty" : "translation_equals_source");
    }

    // REQ-R03: exactly-once selection release on every non-paste outcome. The
    // single call site below is the guarantee; the constexpr matrix above pins
    // it at compile time.
    if (SelectionReleaseRequired(pasted)) {
        ReleaseSelectionOnce();
    }

    // REQ-R02: failure is user-visible, not silent. When the engine produced no
    // translation (privacy-consent block or engine error), dispatch the low
    // failure tone via the existing sound facility before the (possibly) Enter
    // sends the untranslated original.
    if (!pasted && translated.empty() &&
        (status == TranslationStatus::EngineFailed || status == TranslationStatus::CloudConsentBlocked)) {
        DIAG_F("WORKER/ExecuteTask/031: translation produced no result (status=%d); audible failure feedback dispatched\n",
                static_cast<int>(status));
        PlaySoundAsync(SoundType::Disable);
    }

    // Auto-Send or Shift+Enter immediate send dispatch
    // (auto_send is std::atomic; implicit load is safe from this thread - I4)
    const bool want_send = task.is_shift_enter || config_.auto_send.load(std::memory_order_relaxed);
    const bool h1_ok = IsSameWindowForInjection(task.target_hwnd, ::GetForegroundWindow());
    if (want_send) {
        if (h1_ok) {
            SendEnterKey(task.is_shift_enter);
            DIAG_LOG("PIPELINE", "stage=send_enter action=synthetic shift=%d auto_send=%d",
                     task.is_shift_enter ? 1 : 0,
                     config_.auto_send.load(std::memory_order_relaxed) ? 1 : 0);
        } else {
            DIAG_LOG("PIPELINE", "stage=send_enter action=SKIPPED reason=h1_foreground_mismatch target=%p",
                     reinterpret_cast<const void*>(task.target_hwnd));
        }
    }
    DIAG_LOG("PIPELINE", "stage=task_end pasted=%d total_ms=%llu",
             pasted ? 1 : 0, ::GetTickCount64() - t_task_start);
}

} // namespace emebalachat
