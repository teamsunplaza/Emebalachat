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
    //
    // GATE 2 LIMIT (Phase 7 report §2.3): this probe is IMM32-based
    // (ForegroundImeComposing), i.e. a Korean/legacy-IME-only backstop.
    // TSF-native IMEs (modern Japanese/Chinese) keep composition state in
    // ITfContext that is NOT mirrored to IMM32 - such an IME CAN PASS THIS
    // GATE (probe returns false) even while a composition is open, falling
    // through to the translate pipeline. Detection for TSF IMEs relies on
    // the hook-side VK_PROCESSKEY mirror (GATE 1, ImeMirrorNext), the only
    // language-agnostic signal (report §3.4). Logged here so a diag trace
    // of a Japanese/Chinese composition incident attributes the pass to
    // this gate's documented IMM32 scope, not to a probe malfunction.
    if (ForegroundImeComposing()) {
        DIAG_F("WORKER/ExecuteTask/033: IME composition detected at task start; "
                        "bypassing translation (Enter handed to the app's IME). "
                        "NOTE: IMM32 backstop - a TSF IME (modern Japanese/Chinese) "
                        "can pass this gate undetected\n");
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
    // Phase 3 Batch 2 (plan §1.3/§3-Batch2, REQ-015/016): the Enter pipeline is
    // the TYPING context and uses the type pair. The legacy
    // source_language/target_language fields are migration-only and must never
    // be read here again (plan §1.2).
    DIAG_LOG("PIPELINE", "stage=lang_pair ctx=type src=\"%s\" tgt=\"%s\" engine=%s auto_send=%d",
             snap.type_source_language.c_str(), snap.type_target_language.c_str(),
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
        !line.empty() && !ShouldTranslate(line, snap.type_target_language, snap.type_source_language);
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
        NormalizeNewlinesToCRLF(engine_.Translate(line, snap.type_source_language, snap.type_target_language, &status));
    DIAG_LOG("PIPELINE", "stage=translate end status=%d engine=%s duration_ms=%llu "
                         "out_len=%zu out=\"%s\"",
             static_cast<int>(status), engine_.GetActiveEngineName().c_str(),
             ::GetTickCount64() - t_translate_start, translated.size(),
             ToUtf8(translated).c_str());

    // Restore active state
    badge_.SetStatus(BadgeStatus::Active);

    bool pasted = false;
    // B-6a: set when NotifyReplacement still owes its post-newline call (see
    // branch comment at the paste site and the injection block below).
    bool pending_notify = false;
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
        // REQ-027 B-6a (design 210000_architect §2.2 option (a)): the offset
        // saved here becomes the START of the NEXT Enter's EM_SETSEL range, so
        // WHEN a newline is injected (REQ-023 CategoryB / send gate below) the
        // save must happen AFTER SendEnterKey - the post-newline caret is the
        // next block's start. The old pre-newline save stored the caret at the
        // CRLF position, the next selection swallowed the preceding "\r\n"
        // (2 UTF-16 units) and the replacement deleted it: ISSUE-1 line-merge
        // ("엔터 치면 번역되고 줄바꿈이 안 됨"). Proven by the E2E harness
        // (054000_code-report: example1 newlines 1/6, consecutive merge;
        // e2e-run-consecutive-output.log L28/L38). No-op unless pasted==true
        // and the target is a tracked standard EDIT/RichEdit (design §2.6.A2
        // step 4). Branch flag below picks the call site; both sites pass
        // identical args, so exactly one call runs per task.
        pending_notify = pasted;
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
    // REQ-029-A (design 114800 §2.4-b): LocalModelMissing joins the audible set -
    // a missing local model must not be a silent no-response (B-7a precondition:
    // the enum value exists; the engine now returns it instead of the cloud
    // CloudConsentBlocked masquerade).
    if (!pasted && translated.empty() &&
        (status == TranslationStatus::EngineFailed ||
         status == TranslationStatus::CloudConsentBlocked ||
         status == TranslationStatus::LocalModelMissing)) {
        DIAG_F("WORKER/ExecuteTask/031: translation produced no result (status=%d); audible failure feedback dispatched\n",
                static_cast<int>(status));
        PlaySoundAsync(SoundType::Disable);
    }

    // REQ-028 (decisions.md 2026-09-07 11:46, design 114800 §2.4-a): Category B
    // is now auto_send-gated too - OFF = replacement only (no newline) / ON =
    // replacement + automatic newline. The REQ-023 "CategoryB always injects a
    // newline" rule was retired with explicit user approval. Category A keeps the
    // legacy send gate (shift_enter || auto_send). Both still pass the H1
    // foreground guard.
    // (auto_send is std::atomic; implicit load is safe from this thread - I4)
    const AppCategory category = ClassifyAppWindow(task.target_hwnd);
    const bool h1_ok = IsSameWindowForInjection(task.target_hwnd, ::GetForegroundWindow());
    bool inject_enter = false;
    if (category == AppCategory::CategoryB) {
        // REQ-028 (decisions.md 2026-09-07 11:46): CategoryB도 auto_send 게이트.
        // OFF = 치환만(개행 없음) / ON = 치환 후 자동 개행. REQ-023 "항상 개행"은
        // 유저 승인으로 폐기. CategoryA 송신 게이트는 기존 유지.
        inject_enter = config_.auto_send.load(std::memory_order_relaxed);
    } else {
        inject_enter = task.is_shift_enter ||      // Category A: 기존 전송 게이트
                       config_.auto_send.load(std::memory_order_relaxed);
    }
    if (inject_enter && h1_ok) {
        // B-6a settle baseline: caret right BEFORE the newline injection (the
        // post-paste position). kEditCaretUnknown on any EM-gate failure - the
        // settle then degrades to its fixed 50 ms wait (design §2.2).
        const DWORD pre_newline_caret =
            pending_notify ? EditCaretTracker_SampleCaret(task.target_hwnd)
                           : kEditCaretUnknown;
        SendEnterKey(task.is_shift_enter);
        if (pending_notify) {
            // Wait (bounded: 50 ms fixed + max 4×25 ms visibility poll) until
            // the app has visibly processed the injected Enter, THEN save the
            // offset = post-newline caret = next block's start. REQ-023
            // alignment: CategoryB always injects the newline, so this is the
            // normal editor path the E2E example1/consecutive gates cover.
            EditCaretTracker_SettleNewlineVisible(task.target_hwnd, pre_newline_caret);
            EditCaretTracker_NotifyReplacement(task.target_hwnd, pasted, translated.size());
            pending_notify = false;
        }
        // Phase 5: Category B newline vs Category A send are distinguished in
        // the DIAG log (plan §2.3) - same SendEnterKey primitive, different app
        // semantics (editor Enter = "\n", chat Enter = "send").
        DIAG_LOG("PIPELINE", "stage=send_enter action=%s shift=%d auto_send=%d",
                 category == AppCategory::CategoryB ? "synthetic_newline" : "synthetic_send",
                 task.is_shift_enter ? 1 : 0,
                 config_.auto_send.load(std::memory_order_relaxed) ? 1 : 0);
    } else {
        // No newline was injected (CategoryA send gate or H1 foreground
        // mismatch): the caret is still at the replacement end, so save the
        // offset here - pre-injection position is correct for this branch
        // (design §2.5 row 2: two call sites, one per outcome).
        if (pending_notify) {
            EditCaretTracker_NotifyReplacement(task.target_hwnd, pasted, translated.size());
            pending_notify = false;
        }
        DIAG_LOG("PIPELINE", "stage=send_enter action=SKIPPED reason=%s target=%p",
                 h1_ok ? "category_a_send_gate" : "h1_foreground_mismatch",
                 reinterpret_cast<const void*>(task.target_hwnd));
    }
    DIAG_LOG("PIPELINE", "stage=task_end pasted=%d total_ms=%llu",
             pasted ? 1 : 0, ::GetTickCount64() - t_task_start);
}

} // namespace emebalachat
