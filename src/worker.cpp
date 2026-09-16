#include "worker.hpp"
#include "diag_logger.hpp"
#include "smart_bypass.hpp"
#include "sound.hpp"
#include "unicode_utils.hpp"
#include "win32_input.hpp"

#include <cstdio>
#include <optional>

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

// B1 (session 260910_0007, catalog verify 050211 §B1): single-sourced send-through
// sequence. Every non-paste "hand the Enter to the app" outcome executes exactly:
//   1. EditCaretTracker_SampleCaret(target)      (REQ-036 FIX-1 pre-send baseline;
//                                                 VK_RIGHT in step 2 moves the caret,
//                                                 so the sample MUST precede the release)
//   2. ReleaseSelectionOnce()                    (REQ-R03)
//   3. SendEnterKey(is_shift_enter)              (all seven legacy sites passed the
//                                                 task flag explicitly; SendEnterKey is
//                                                 void, no site ever inspected a return)
//   4. EditCaretTracker_NotifySentNewline(target, pre_caret)  (settle + store)
// No site had a branch, log, or wait BETWEEN the four steps (re-verified at HEAD
// 6c2aa30); the per-step comments at the call sites describe the branch's WHY and
// were kept in place. The paste path's SampleCaret + SendEnterKey +
// SettleNewlineVisible + NotifyReplacement sequence is a DIFFERENT contract
// (B-6a, verify 050211 §B1) and is deliberately NOT folded in here.
//
// pre_sampled_caret design choice (site 5, REQ-F5 post-window promote): that site
// sampled the caret EARLIER (before the EmptyCapturePromotesToSend gate; the value
// also feeds the gate arithmetic), so re-sampling inside the helper would move the
// /039 caret baseline. The site passes its existing sample and the helper skips
// step 1 - execution order stays provably identical at all seven sites. std::optional
// instead of a kEditCaretUnknown sentinel: kEditCaretUnknown is a legal SampleCaret
// failure value a future caller could pass, and conflating "use this value" with
// "sample now (and maybe fail)" would be ambiguous; nullopt is the only true
// "no pre-sample" witness.
// BUG-005 (session 260914_0001): identity-paste selection consume. The exact
// mechanism the 1st Enter's successful rescue translation uses (the editor's
// paste transaction REPLACES the live whole-document selection and lands a
// collapsed post-paste caret - the ProseMirror replaceSelection contract
// cited in the BUG-004 analysis §6) is re-used here as the selection
// normalizer for the send-through terminals whose capture came from the
// SelectAll rescue: the single VK_RIGHT of ReleaseSelectionOnce is a
// heuristic collapse that the structured-editor class can silently drop
// (BUG-002 class), leaving the whole-document selection LIVE when the
// handed-through Enter arrives - and Enter over a live selection REPLACES
// it (the exact "맨 마지막 줄만 빼고 다 삭제됨" data loss). The paste of the
// CAPTURED text back over its own selection is byte-safe by construction:
// the selection spans exactly [document content captured]. Reuses the
// field-proven PasteAndRestore primitive (H1 foreground guard included) so
// no new chord, no new timing constant; clipboard is restored by the same
// F7 retry discipline. `pasted` must be true for the consume to have
// completed; on an H1-abort the selection stays live and the caller KEEPS
// the legacy VK_RIGHT release (the next Enter re-runs the whole gate).
bool ConsumeRescueSelectionIdentityPaste(std::wstring_view captured_text, HWND target_hwnd,
                                          const ClipboardBackup& backup,
                                          bool& restorer_active, bool* restore_ok_out) {
    if (restore_ok_out) {
        *restore_ok_out = false;
    }
    const bool consume_ok =
        PasteAndRestore(captured_text, backup, target_hwnd, restore_ok_out);
    // F7 discipline, same as the paste-success branch: disarms the scope-exit
    // guard only when the original clipboard was CONFIRMED restored.
    restorer_active = ClipboardRestorerStaysArmed(consume_ok, restore_ok_out ? *restore_ok_out : false);
    return consume_ok;
}

void SendThroughWithNewlineTracking(HWND target_hwnd, bool is_shift_enter,
                                    std::optional<DWORD> pre_sampled_caret = std::nullopt) {
    const DWORD pre_caret = pre_sampled_caret.has_value()
                                ? *pre_sampled_caret
                                : EditCaretTracker_SampleCaret(target_hwnd);
    ReleaseSelectionOnce();
    SendEnterKey(is_shift_enter);
    EditCaretTracker_NotifySentNewline(target_hwnd, pre_caret);
}

// REQ-R03 path matrix, compile-time proven against the shared header predicate
// (src/worker.hpp) so worker.cpp and the unit tests assert on ONE definition:
//   (a) translated empty (engine failure / consent block)      -> release,
//   (b) translated == line (unchanged text)                    -> release,
//   (c) paste cancelled by the H1 guard (pasted == false)      -> release,
//   (d) successful paste (restorer.active=false, Ctrl+V ate the selection) -> no release.
static_assert(SelectionReleaseRequired(true) == false, "REQ-R03: successful paste must NOT re-release (Ctrl+V already consumed selection)");
static_assert(SelectionReleaseRequired(false) == true, "REQ-R03: every non-paste path MUST release the block selection");
// BUG-004 F2 gate matrix (compile-time proven, same ONE-definition discipline):
//   rescue paste success  -> collapse (the whole-document landing must be
//   re-normalized: caret collapse + viewport reveal, debug analysis 020300 §7);
//   non-rescue success    -> NO collapse (REQ-R03 no-release contract byte-identical);
//   any paste failure     -> NO collapse (REQ-R03's release owns the caret, exactly once).
static_assert(PostPasteCollapseRequired(true, true) == true, "BUG-004: successful rescue paste MUST collapse the whole-document selection once");
static_assert(PostPasteCollapseRequired(true, false) == false, "BUG-004: non-rescue paste success keeps the REQ-R03 no-release contract");
static_assert(PostPasteCollapseRequired(false, true) == false, "BUG-004: failed paste never collapses - REQ-R03 release owns the caret");
static_assert(PostPasteCollapseRequired(false, false) == false, "BUG-004: non-rescue failure unchanged (no collapse, no double release)");
// BUG-005 (represcription 061500 §3-E): the identity arm's 3-arg predicate form
// adds the arm scope to the SAME ONE definition - identity_outcome is false on
// path B (translated.empty: L720 requires non-empty) and path C (successful
// paste: translated != line), so the relocated pre-release gate can only fire
// on the identity send-through terminal; the default argument keeps the three
// legacy arms byte-identical.
static_assert(RescueLiveSelectionNeedsConsume(true, true, true) == true, "BUG-005: identity arm fires on the live rescue selection (pre-release gate)");
static_assert(RescueLiveSelectionNeedsConsume(true, true, false) == false, "BUG-005: Path B/C (translated empty / successful paste) never fire the identity gate");
static_assert(RescueLiveSelectionNeedsConsume(true, false, false) == false, "BUG-005: empty rescue capture never consumes regardless of arm");
static_assert(RescueLiveSelectionNeedsConsume(false, true, false) == false, "BUG-005: non-rescued capture never consumes regardless of arm");

} // namespace

// S2 (session 260914_0001, design spec 222500 §1/§6): the ledger canonical
// form. Definition of the ONE declaration in worker.hpp (shared with the
// unit tests) - defined in namespace emebalachat (NOT the anonymous one) so
// the header declaration and this definition are a single entity. The
// transformation order is the spec §1.2 contract:
//   1. CRLF -> LF  (before step 2 - the only order-sensitive pair, §1.3:
//      trailing-space removal must see final line endings or "a \r\nb"
//      would lose the "\n" entirely)
//   2. per-line trailing [ \t] removal (NBSP is Unicode White_Space and is
//      removed here too - order A of §1.3, same result as order B)
//   3. NBSP (U+00A0) -> SP (U+0020)
//   4. Unicode NFC (via unicode_utils' NormalizeNFC - same NormalizeString
//      seam the pipeline already uses; its fallback returns the input copy
//      on API failure, which IS the spec §6.3 "use the unnormalized string,
//      log a warning, never crash" path)
// Steps 1-3 are ASCII-unit transforms and cannot fail; only NFC can.
std::wstring CanonicalFormForLedger(std::wstring_view input) {
    // Step 1: CRLF -> LF. A lone '\r' is content (legacy Mac line ending
    // never produced by ProseMirror) and is deliberately left alone.
    std::wstring out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == L'\r' && i + 1 < input.size() && input[i + 1] == L'\n') {
            continue; // drop the CR; the LF is appended by the next iteration
        }
        out.push_back(input[i]);
    }

    // Step 2: remove trailing [ \t] (and trailing NBSP, see §1.3 order A)
    // before every LF and at end-of-string.
    size_t write = 0;
    for (size_t read = 0; read < out.size();) {
        if (out[read] != L'\n') {
            out[write++] = out[read++];
            continue;
        }
        // At a line break: strip the trailing whitespace run of the line
        // just written.
        while (write > 0 && (out[write - 1] == L' ' || out[write - 1] == L'\t' ||
                             out[write - 1] == L'\u00A0')) {
            --write;
        }
        out[write++] = out[read++];
    }
    // End-of-string trailing whitespace run of the last line.
    while (write > 0 && (out[write - 1] == L' ' || out[write - 1] == L'\t' ||
                         out[write - 1] == L'\u00A0')) {
        --write;
    }
    out.resize(write);

    // Step 3: NBSP -> SP. Step 2 already removed line-trailing NBSPs; this
    // converts the interior ones ProseMirror replaces with regular spaces.
    for (wchar_t& ch : out) {
        if (ch == L'\u00A0') {
            ch = L' ';
        }
    }

    // Step 4: Unicode NFC. NormalizeNFC falls back to its input copy on API
    // failure (unicode_utils.cpp) - the spec §6.3 confirmed fallback path.
    return NormalizeNFC(out);
}

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

bool PipelineWorker::PostTask(bool is_shift_enter, HWND target_hwnd,
                              int shift_enter_count) {
    if (is_busy_.exchange(true, std::memory_order_acquire)) {
        return false; // Busy with existing translation task
    }
    // REQ-003 (Issue C, session 260910_0003): publish the in-flight target
    // BEFORE the queue push - the hook thread's same-window bare-Enter
    // guard (BusyEnterSameWindowSuppressed, hook.hpp) must see a non-null
    // target as soon as IsBusy() can observe true. Published only from the
    // one caller that won the exchange, so no write race exists; nullptr
    // (untargeted callers/tests) simply never matches a live window.
    busy_target_hwnd_.store(target_hwnd, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push(PipelineTask{is_shift_enter, target_hwnd, shift_enter_count});
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

    // RAII guard ensuring is_busy_ is released upon function exit.
    // REQ-003 (Issue C): BusyTargetHwnd is cleared AFTER is_busy_ inside
    // this same destructor (sequenced stores on one thread) so the
    // (busy, hwnd) pair the hook reads never advertises a finished task:
    // the moment busy reads false the guard is already disarmed in the
    // hook's same-window check, and a torn read of the hwnd alone is
    // harmless there (suppression requires busy==true).
    struct BusyGuard {
        std::atomic<bool>& busy;
        std::atomic<HWND>& busy_hwnd;
        ~BusyGuard() {
            busy.store(false, std::memory_order_release);
            busy_hwnd.store(nullptr, std::memory_order_relaxed);
        }
    } busy_guard{is_busy_, busy_target_hwnd_};

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
        // REQ-036 FIX-1: this task ends by SENDING the Enter - in an editor it
        // inserts the current block's terminator. Sample the caret BEFORE the
        // selection release (VK_RIGHT moves it +1), then advance the stored
        // offset to the measured post-newline caret (no-op untracked/non-EM).
        SendThroughWithNewlineTracking(task.target_hwnd, task.is_shift_enter);
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
    // A-004 Option A: baseline the clipboard sequence BEFORE the copy chords (fast-path proof).
    const DWORD clip_seq_at_capture = ::GetClipboardSequenceNumber();
    DIAG_LOG("PIPELINE", "stage=capture begin target_hwnd=%p",
             reinterpret_cast<const void*>(task.target_hwnd));
    // Session 260913_0001 (Reddit long-post fix, debug report 022121 §7
    // Step 1): forward the hook-counted Shift+Enter depth K so the capture
    // seam's slice-before-guard can bound the non-EM whole-capture by the
    // CURRENT block instead of aborting the accumulation. capture_result
    // carries the shape-only seam verdict (Phase B, §8-1) so the end-log
    // below distinguishes empty-capture causes.
    EnterCaptureResult capture_result = EnterCaptureResult::Ok;
    RescueProvenance provenance; // S1: {rescued=false, Origin::none} by default
    std::wstring line = NormalizeNewlinesToCRLF(
        CopySelectedText(task.target_hwnd, task.shift_enter_count, &capture_result,
                         &provenance));
    // REQ-003 (session 260909): the captured body IS the user's text - recorded
    // only when diag_log_content is on (default off). Length and duration
    // metadata stay on both branches (shape-only rule, debugging preserved).
    // k= and result= are shape-only additions (integer + enum label such as
    // "guard_abort") - never user content, safe under diag_log_content=false.
    if (diag::ContentLoggingEnabled()) {
        // k=/result= stay AFTER the content= field: the req027 E2E log parser
        // matches `duration_ms=\d+ content="(.*)"` adjacently, and appending
        // new fields at the end keeps that regex (and the harness) unchanged.
        DIAG_LOG("PIPELINE", "stage=capture end len=%zu duration_ms=%llu content=\"%s\" k=%d result=%s",
                 line.size(), ::GetTickCount64() - t_capture_start,
                 ToUtf8(line).c_str(),
                 task.shift_enter_count, EnterCaptureResultName(capture_result));
    } else {
        DIAG_LOG("PIPELINE", "stage=capture end len=%zu duration_ms=%llu k=%d result=%s",
                 line.size(), ::GetTickCount64() - t_capture_start,
                 task.shift_enter_count, EnterCaptureResultName(capture_result));
    }
    // BUG-003 (session 260913_0002): the block-slice depth actually used for
    // this capture. A SelectAll-rescued whole-document capture re-anchors a
    // paste-saturated K (user Ctrl+V: "unknown paste geometry, translate the
    // whole capture once") to the document-end block so already-translated
    // upper content is never re-translated. ONE definition
    // (EffectiveBlockSliceK, win32_input.hpp) shared with the capture seam.
    // The capture-end log above keeps the RAW hook K; the block_slice log
    // below prints this effective value.
    const int k_block = EffectiveBlockSliceK(task.shift_enter_count, provenance.rescued);

    // R5 observability: log the capture/bypass decision so a silent
    // "nothing translated" can be attributed to the exact failing gate
    // (lengths/booleans only; never the captured content itself).
    const bool was_smart_bypassed =
        !line.empty() && !ShouldTranslate(line, snap.type_target_language, snap.type_source_language);

    // REQ-F2 (session 260908_0001): last paste ledger comparison. In
    // category=0 apps the fallback selection is the WHOLE input
    // [0..caret), and with auto_send=0 the send gate leaves our last pasted
    // translation sitting there. This Enter's capture may therefore be:
    //   (i)  EXACTLY the last pasted text  -> the user is pressing Enter to
    //        SEND our own output: skip re-translation entirely and hand
    //        Enter to the app (the L1297-1309 identity round trip in the
    //        user log was this case - Google churned the sentence and the
    //        "worked" appearance was a re-translation in disguise).
    //   (ii) the last pasted text PLUS a newly typed tail -> translate
    //        ONLY the tail and paste last_paste + tail_translation, so the
    //        already-translated prefix is never re-translated (each
    //        re-translation compounds the accumulation: 44 -> 112 -> 200
    //        chars in the log).
    //   (iii) neither (user edited our output, or a different window /
    //        fresh context) -> F3 block slice: the current block (the last
    //        K+1 logical lines, K = hook-counted Shift+Enters) is translated
    //        and the verbatim prefix before it is preserved by the same
    //        recomposition machinery as (ii) - the whole-capture
    //        re-translation that destroyed 예시1/2/3 (verify 220750 §2 R2)
    //        is gone. The same-hwnd ledger entry SURVIVES (C1/C3 keep);
    //        only a different-hwnd context still clears it.
    // The ledger is per (target hwnd) and both sides are already
    // CRLF-normalized (line passed through NormalizeNewlinesToCRLF above;
    // last_paste_text_ is stored from the equally normalized `translated`),
    // so the comparison is representation-stable. Edit-detection (deletion
    // warning in the delegation) falls out naturally: any change makes the
    // exact/prefix tests fail and route to (iii).
    bool pasted_prefix_skip = false;
    std::wstring pasted_prefix_text;   // (ii): the remembered verbatim prefix
    std::wstring untranslated_tail;    // (ii): the newly typed tail to translate
    // S2 (session 260914_0001, design spec 222500 §2.3/§2.7): the ledger
    // verdict runs in CANONICAL space. ProseMirror re-renders the composer
    // through CRLF->LF / trailing-space trim / NBSP->SP / NFC, which made
    // the pre-S2 byte comparison non-deterministic on the ExactMatch route
    // (BUG-005 §9-3). Canonicalizing BOTH sides keeps the comparison strict
    // byte equality (ADR-001: no fuzzy matching, ever) while making the
    // verdict editor-normalization-proof. The !last_paste_canonical_.empty()
    // guard is the spec §2.7 fast-path: when no ledger is active the block
    // is unreachable anyway, so the first-ever Enter pays no NFC cost. The
    // lifecycle contract (worker.hpp §7.1.1 comment) guarantees the canonical
    // twin is never stale w.r.t. last_paste_text_.
    // REQ-041: ledger presence/verdict as named state (was an anonymous
    // inline switch). k_eff below consumes both; the reader no longer
    // re-derives "was the ledger alive" from side effects.
    bool ledger_present = false;
    PasteLedgerVerdict ledger_verdict = PasteLedgerVerdict::NoMatch;
    if (!line.empty() && last_paste_target_ == task.target_hwnd &&
        !last_paste_canonical_.empty()) {
        ledger_present = true;
        const std::wstring line_canonical = CanonicalFormForLedger(line);
        switch (AnalyzeCaptureVsLastPaste(line_canonical, last_paste_canonical_)) {
            case PasteLedgerVerdict::ExactMatch:
                ledger_verdict = PasteLedgerVerdict::ExactMatch;
                pasted_prefix_skip = PastedPrefixNeedsSkip(true, was_smart_bypassed, false);
                break;
            case PasteLedgerVerdict::PrefixWithTail:
                ledger_verdict = PasteLedgerVerdict::PrefixWithTail;
                // S2 v2 re-specified slice (spec §2.3): BOTH the verbatim
                // prefix source and the tail slice live in CANONICAL space.
                // Tail offset = last_paste_canonical_.size(): a whole-unit
                // boundary (last_paste_canonical_ is a complete stored
                // string), so the split can never land inside a surrogate
                // pair - and, unlike the pre-S2 original-space offset, it
                // cannot amputate leading characters of the user's newly
                // typed tail when CRLF/NFC shifted the lengths (spec §2.4).
                pasted_prefix_text = last_paste_canonical_;
                untranslated_tail.assign(line_canonical, last_paste_canonical_.size(),
                                         line_canonical.size() - last_paste_canonical_.size());
                break;
            case PasteLedgerVerdict::NoMatch:
                break;
        }
    }
    // REQ-041 (session 260917, Reddit 재번역 사용자 보고): the K the F3 slice
    // may actually use. ledger_present && NoMatch means this window received
    // a successful translation paste whose bytes now FAIL to match - proof
    // that something above the current line changed (user edit, or an editor
    // re-serialization outside S2's canonical form). At that moment the
    // keystroke accounting behind K ("one document newline == one Shift+Enter
    // the hook saw") has lost its premise, so trusting K could reach BACK
    // into already-translated lines and re-translate (then paste-back
    // overwrite) them - the exact user report. Cap K to 0 = translate the
    // last logical line only; the verbatim prefix comes from the live capture
    // bytes, so edits above survive untouched. No-ledger captures (fresh
    // composition / window switch) keep the raw K: the whole fresh block
    // still translates (verify 220750 예시1 contract). The refine branch can
    // only run on PrefixWithTail (never capped), so passing k_eff there is
    // identical to k_block by construction. See worker.hpp
    // EffectiveSliceKForVerdict for the full rationale + rejected alternatives.
    const int k_eff = EffectiveSliceKForVerdict(k_block, ledger_present, ledger_verdict);
    if (k_eff != k_block) {
        DIAG_F("WORKER/ExecuteTask/045: dead-ledger NoMatch; K %d -> %d (last logical line only; prefix held verbatim from live capture)\n",
               k_block, k_eff);
    }
    // F3 (session 260908_0003, verify 220750 §6 adopted design): block-slice
    // from whole capture. The CURRENT block is the last K+1 logical lines of
    // the capture (K = hook-counted Shift+Enters of the current composition,
    // REQ-041: capped to 0 on dead-ledger NoMatch); everything before the
    // slice point is earlier (already-translated or foreign) content and
    // joins the REQ-F2 PrefixWithTail recomposition machinery, so the Ctrl+V
    // replacement is [prefix verbatim][block translated] - earlier blocks
    // keep their text AND their language (예시1/2/3), and the ledger
    // re-anchors to the recomposed post-replace state (the successful-paste
    // store IS the design §3 re-anchor). The slice replaces the legacy
    // whole-capture translation exactly in the arms that destroyed the
    // examples (R2 NoMatch with an empty/foreign ledger, R4 after C3 clears).
    // For the ledger-protected PrefixWithTail arm the ledger end is a LOWER
    // bound of the block start (the pasted text stays verbatim), and the
    // separator run the send-through Enter deposited between ledger and new
    // typing is moved into the verbatim prefix too - the engine never sees a
    // leading bare newline (it churns or drops it: the line-merge risk the
    // ledger-keep introduced). On EM-tracked captures the slice is a
    // provable no-op: an EM selection covers exactly the current block,
    // which contains exactly K boundaries, and FindCurrentBlockStart wants
    // K+1 -> clamps to 0 (whole block, as-is).
    if (!line.empty() && !pasted_prefix_skip) {
        // S2 F1 (adversarial review 080100 Q4b): the F3 refine stage is the
        // ONE definition shared with the tests (win32_input.hpp
        // F3BlockSliceRefine) - the regression grid must execute the real
        // production composition, not a re-derived simulation. line_canonical
        // is non-empty exactly when the S2 ledger verdict ran above (the
        // canonical PrefixWithTail split is the refine input); the empty
        // fast-path keeps the NoMatch/no-ledger arms byte-identical.
        const std::wstring line_canonical =
            (untranslated_tail.empty() && pasted_prefix_text.empty())
                ? std::wstring()
                : CanonicalFormForLedger(line);
        const F3BlockSliceSplit f3 = F3BlockSliceRefine(
            line, k_eff, line_canonical, pasted_prefix_text, untranslated_tail);
        if (f3.empty_block) {
            // Empty current block (capture ends on a separator with nothing
            // typed after it): the prefix is earlier content and MUST NOT be
            // re-translated. Hand the Enter to the app (same send-of-output
            // mechanics as the ExactMatch skip; the line-break the user just
            // asked for lands). The ledger is KEPT (F3 chain coherence):
            // nothing was pasted or edited here beyond the newline the app
            // inserts after the remembered text, so the entry still
            // describes live text and the next capture stays a
            // PrefixWithTail (byte-comparison self-invalidation remains the
            // stale-memory guard, same reasoning as the C1 keep).
            DIAG_F("WORKER/ExecuteTask/041: block slice is empty (capture ends at a line separator, K=%d, len=%zu); prefix held verbatim, Enter handed to the app (no re-translation)\n",
                   k_eff, line.size());
            DIAG_LOG("PIPELINE", "stage=send_through decision=f3_empty_block_slice duration_ms=%llu",
                     ::GetTickCount64() - t_task_start);
            if (RescueLiveSelectionNeedsConsume(provenance.rescued, !line.empty()) &&
                ConsumeRescueSelectionIdentityPaste(line, task.target_hwnd, backup, restorer.active, nullptr)) {
                DIAG_F("WORKER/ExecuteTask/044: rescue-captured whole-document selection consumed by identity paste before send-through (empty-tail arm); the Enter cannot replace a live selected span\n");
            }
            SendThroughWithNewlineTracking(task.target_hwnd, task.is_shift_enter);
            return;
        }
        if (f3.sliced) {
            // Slice owns this capture (NoMatch / foreign / no ledger) or
            // refines the ledger-protected PrefixWithTail split down to the
            // block boundary (block_start was floored at the ledger end
            // above, so the re-derivation never SHRINKS the verbatim prefix
            // below what the ledger proved). Either way: verbatim prefix +
            // translated block tail; the recomposition covers the whole
            // captured span exactly.
            DIAG_LOG("PIPELINE", "stage=block_slice%s K=%d K_eff=%d cap=%d capture_len=%zu prefix_len=%zu block_len=%zu",
                     f3.refines_ledger ? "_ledger_refine" : "",
                     k_block, k_eff, (k_eff != k_block) ? 1 : 0, line.size(),
                     pasted_prefix_text.size(), untranslated_tail.size());
        }
    }
    if (pasted_prefix_skip) {
        DIAG_F("WORKER/ExecuteTask/038: capture equals last pasted translation (len=%zu); Enter handed to the app (send-of-output, no re-translation)\n",
               line.size());
        DIAG_LOG("PIPELINE", "stage=send_through decision=pasted_prefix_skip duration_ms=%llu",
                 ::GetTickCount64() - t_task_start);
        // Same contract as the smart-bypass send-through below: release the
        // block selection (Ctrl+V of the next task must not clobber it) and
        // hand the intercepted Enter to the app.
        if (RescueLiveSelectionNeedsConsume(provenance.rescued, !line.empty()) &&
            ConsumeRescueSelectionIdentityPaste(line, task.target_hwnd, backup, restorer.active, nullptr)) {
            DIAG_F("WORKER/ExecuteTask/044: rescue-captured whole-document selection consumed by identity paste before send-through (exact-match arm); the Enter cannot replace a live selected span\n");
        }
        SendThroughWithNewlineTracking(task.target_hwnd, task.is_shift_enter);
        // F3 C1 hardening (session 260908_0003, verify 220750 §2 R1->R2 and
        // §7 예시1): the redundant-Enter (send-of-output) NO LONGER clears
        // the ledger. The pre-F3 clear was the chain-break that armed the R2
        // whole-document destruction: paragraph 2 typed after a redundant
        // Enter met an EMPTY ledger -> NoMatch -> [0..caret) whole
        // re-translate. The ledger is content-self-invalidating
        // (AnalyzeCaptureVsLastPaste byte-prefix): keeping it is SAFE in
        // every outcome -
        //  - editor (auto_send=0, text stays live): the kept entry is now
        //    the prefix of the new accumulation -> next Enter = PrefixWithTail
        //    (tail-only translation, explicit chain protection without
        //    relying on the hook's K count);
        //  - chat send (input cleared by the app): the next capture cannot
        //    byte-match the stale entry -> NoMatch -> the F3 block slice
        //    (K=0 -> whole current capture) owns the new accumulation from
        //    the document start, and the successful paste re-anchors the
        //    ledger anyway.
        // No clear here; the one-shot semantics are preserved by these two
        // self-invalidation mechanisms (verify 220750 §7 explicit guidance
        // against clearing on non-EM CategoryB ExactMatch).
        return;
    }
    const bool should_translate = !line.empty() && !was_smart_bypassed;
    DIAG_F("WORKER/ExecuteTask/034: captured %zu chars, should_translate=%d\n",
            line.size(), should_translate ? 1 : 0);
    // Session 260913_0001 (Phase B, debug report 022121 §8-1): an empty
    // capture is no longer one flat `capture_empty` label - the capture
    // seam's shape-only verdict (guard_abort / copy_chord_failed /
    // empty_selection / empty_tail / editor_excluded) names the cause,
    // which is exactly what made the Reddit long-post incident
    // undiagnosable from the user's description alone.
    DIAG_LOG("PIPELINE", "stage=bypass_decision smart_bypass=%d should_translate=%d reason=%s",
             was_smart_bypassed ? 1 : 0, should_translate ? 1 : 0,
             line.empty() ? EnterCaptureResultName(capture_result)
                          : (was_smart_bypassed ? "already_target_language" : "translate"));

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
    //
    // REQ-034 F3-B paste-window gate (design 173700_architect §2.2.1), checked
    // BEFORE the hold branch: an empty capture within kPasteEmptySuppressMs of
    // the last SUCCESSFUL paste is the last==caret geometry (empty EM_SETSEL
    // range -> Ctrl+C changes nothing -> stale-refuse -> empty), i.e.
    // the user's re-translation retry, NOT a "no selection" mistake. Suppress
    // the notice and hand Enter to the app exactly like the legacy send-through
    // (ReleaseSelectionOnce + SendEnterKey): the retry Enter line-breaks/sends
    // normally instead of being blocked by a false TooltipNoSelection (log
    // L432/563/601 repetition). Outside the window (no paste sentinel 0, or
    // elapsed) the R5 hold below runs UNCHANGED (C-5); the
    // EmptyCaptureNeedsHold predicate itself is untouched - the window is
    // shared-worker-only state, so the gate composes the two pure predicates
    // here and the tests fold the same composition (PasteWindowSuppressesNotice).
    const bool empty_capture_hold = EmptyCaptureNeedsHold(line.empty(), was_smart_bypassed);
    const ULONGLONG t_notice_gate_now = ::GetTickCount64();
    if (empty_capture_hold &&
        PasteWindowSuppressesNotice(t_notice_gate_now,
                                    last_paste_ms_.load(std::memory_order_relaxed))) {
        DIAG_F("WORKER/ExecuteTask/036: empty capture within paste window (elapsed %llums <= %ums); silent send-through (no no-selection notice)\n",
               t_notice_gate_now - last_paste_ms_.load(std::memory_order_relaxed),
               static_cast<unsigned int>(kPasteEmptySuppressMs));
        DIAG_LOG("PIPELINE", "stage=empty_capture decision=paste_window_suppress action=send_through_no_notice "
                             "duration_ms=%llu",
                 t_notice_gate_now - t_task_start);
        // Option E (A-005 observability): state the benign-skip provenance so a
        // WIN32_INPUT-line-agnostic grep classifies this empty capture as the
        // intended post-paste newline, not a swallowed Enter.
        DIAG_LOG("PIPELINE", "stage=empty_capture provenance=benign_skip basis=paste_window_sentinel(armed_by_pipeline_paste, win32_/004 EM provably-empty is the capture-side twin) action=send_through_no_notice");
        // Never log the captured body (R5 rule): the capture IS empty anyway.
        // REQ-036 FIX-1 (session 260907, debug report): the send-through Enter
        // below inserts the current block's terminator (editor) - sample the
        // caret BEFORE the release (VK_RIGHT moves it), settle + store after.
        SendThroughWithNewlineTracking(task.target_hwnd, task.is_shift_enter);
        return;
    }
    // REQ-F5 (session 260908_0001): POST-window empty-capture promotion. The
    // 036 paste-window suppress above covers only the first 2 s. After that,
    // an empty capture whose live caret equals the remembered paste-end
    // offset is the send-of-output geometry (auto_send=0 left the translation
    // sitting there; the user's bare Enter is now a SEND): promote to the
    // ExactMatch send-through contract instead of the 035 hold. Every other
    // empty capture (moved caret, deletion, typed-then-cleared, no ledger,
    // different hwnd, unknown offset) refuses and keeps the 035 hold.
    const bool last_paste_valid = (last_paste_target_ == task.target_hwnd) &&
                                  (last_paste_end_offset_ != kEditCaretUnknown);
    const DWORD now_caret =
        last_paste_valid ? EditCaretTracker_SampleCaret(task.target_hwnd)
                         : kEditCaretUnknown;
    if (EmptyCapturePromotesToSend(line.empty(), was_smart_bypassed, last_paste_valid,
                                   now_caret == last_paste_end_offset_)) {
        DIAG_F(
                "WORKER/ExecuteTask/039: empty capture on bare-Enter path promoted to "
                "send-of-output (caret %lu == paste-end %lu, no edit since paste); "
                "Enter handed to the app\n",
                static_cast<unsigned long>(now_caret),
                static_cast<unsigned long>(last_paste_end_offset_));
        DIAG_LOG("PIPELINE", "stage=empty_capture decision=promote_exact_match action=send_through_no_notice "
                             "duration_ms=%llu",
                 ::GetTickCount64() - t_task_start);
        // Option E (A-005 observability): post-window twin of the /036 provenance.
        DIAG_LOG("PIPELINE", "stage=empty_capture provenance=benign_skip basis=caret_equals_paste_end(no_edit_since_paste) action=send_through_no_notice");
        // Site 5: pass the pre-sampled caret (sampled above before the promote
        // gate); the helper skips its own SampleCaret - the /039 baseline stays.
        SendThroughWithNewlineTracking(task.target_hwnd, task.is_shift_enter, now_caret);
        // Same one-shot ledger contract as the ExactMatch skip: this output
        // has now been sent, so a fresh accumulation context starts.
        last_paste_target_ = nullptr;
        last_paste_text_.clear();
        // S2 (spec §7.1.1): the canonical twin clears ADJACENT to the
        // original - no code path may clear one without the other.
        last_paste_canonical_.clear();
        last_paste_end_offset_ = kEditCaretUnknown;
        return;
    }
    // A-004 Option A (user-approved decisions.md 09-09 08:32): has_text=0
    // fast-path send-through. An image-only backup clipboard (no CF_UNICODETEXT)
    // whose sequence never moved across ALL copy chords proves the target input
    // holds no selectable text (out-of-band image paste + bare send-Enter; live
    // repro 05:35:33-40: 3 Enters swallowed by the R5 hold). Stale-read safety
    // is preserved: any committed copy moves the sequence and refuses this path.
    // Disjoint from the REQ-034 F3-B EM exemption (category=1 carries has_text=1
    // in every A-005 event), and the normal-text empty-selection hold requires
    // has_text=1, so both keep their exact existing behavior.
    if (empty_capture_hold && clip_backup_ok && !backup.text.has_value() &&
        ::GetClipboardSequenceNumber() == clip_seq_at_capture) {
        DIAG_F("WORKER/ExecuteTask/042: empty capture with image-only clipboard (has_text=0) and clipboard sequence unchanged since capture start (no chord committed a copy); R5 hold bypassed, send-Enter handed to the app\n");
        DIAG_LOG("PIPELINE", "stage=empty_capture decision=has_text0_fastpath action=send_through_no_notice duration_ms=%llu",
                 ::GetTickCount64() - t_task_start);
        SendThroughWithNewlineTracking(task.target_hwnd, task.is_shift_enter);
        return;
    }
    if (empty_capture_hold && empty_capture_cb_) {
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
        // BUG-005: the smart-bypass arm is a send-through terminal that can be
        // reached with a live rescue-captured whole-document selection (all
        // capture scripts already in the target language). Same identity-
        // paste consume as /044 before the intercepted Enter is handed over.
        if (RescueLiveSelectionNeedsConsume(provenance.rescued, !line.empty()) &&
            ConsumeRescueSelectionIdentityPaste(line, task.target_hwnd, backup, restorer.active, nullptr)) {
            DIAG_F("WORKER/ExecuteTask/044: rescue-captured whole-document selection consumed by identity paste before send-through (bypass arm); the Enter cannot replace a live selected span\n");
        }
        // No paste will happen on this path: release the block selection before
        // Enter so the (about to be sent) text cannot be clobbered (REQ-R03).
        // REQ-036 FIX-1 (session 260907, debug report): same send-through
        // contract as the paste-window branch above - sample the caret BEFORE
        // the release, advance the stored offset past the terminator after.
        SendThroughWithNewlineTracking(task.target_hwnd, task.is_shift_enter);
        return;
    }

    // Indicate translating state on UI pill
    badge_.SetStatus(BadgeStatus::Translating);
    // REQ-F2 (ii): when the capture is last-paste + a newly typed tail, the
    // ENGINE sees only the tail (the remembered prefix is verbatim - the
    // compounding defect was the whole capture re-entering translation),
    // and the paste recomposes prefix + tail translation so the whole-input
    // selection geometry is still fully covered by the replacement.
    const std::wstring& engine_input = untranslated_tail.empty() ? line : untranslated_tail;
    DIAG_LOG("PIPELINE", "stage=translate begin engine=%s src_len=%zu%s",
             engine_.GetActiveEngineName().c_str(), engine_input.size(),
             untranslated_tail.empty() ? "" : " (req_f2 tail_only; prefix held verbatim)");

    // REQ-R02: capture the explicit engine status. An empty result is no longer
    // silent - the status below distinguishes privacy-block from engine failure.
    TranslationStatus status = TranslationStatus::Ok;
    const ULONGLONG t_translate_start = ::GetTickCount64();
    std::wstring translated =
        NormalizeNewlinesToCRLF(engine_.Translate(engine_input, snap.type_source_language, snap.type_target_language, &status));
    if (!untranslated_tail.empty() && !translated.empty()) {
        // Recompose: [remembered verbatim prefix][tail translation]. The
        // pasted result must equal the full input span so the selection is
        // wholly consumed by Ctrl+V (same guarantee as the normal path).
        auto is_ws = [](wchar_t c) { return c == L' ' || c == L'\n' || c == L'\r' || c == L'\t'; };
        if (!pasted_prefix_text.empty() && !is_ws(pasted_prefix_text.back()) &&
            !translated.empty() && !is_ws(translated.front()) &&
            !untranslated_tail.empty() && is_ws(untranslated_tail.front())) {
            translated.insert(translated.begin(), L' ');
            DIAG_F("WORKER: Added boundary whitespace between prefix and translation\n");
        }
        translated.insert(translated.begin(), pasted_prefix_text.begin(), pasted_prefix_text.end());
    }
    // REQ-003 (session 260909): the translation output is user content - logged
    // only when diag_log_content is on (default off). status/engine/duration/
    // out_len keep the gate-decision and failure triage observable.
    if (diag::ContentLoggingEnabled()) {
        DIAG_LOG("PIPELINE", "stage=translate end status=%d engine=%s duration_ms=%llu "
                             "out_len=%zu out=\"%s\"",
                 static_cast<int>(status), engine_.GetActiveEngineName().c_str(),
                 ::GetTickCount64() - t_translate_start, translated.size(),
                 ToUtf8(translated).c_str());
    } else {
        DIAG_LOG("PIPELINE", "stage=translate end status=%d engine=%s duration_ms=%llu "
                             "out_len=%zu",
                 static_cast<int>(status), engine_.GetActiveEngineName().c_str(),
                 ::GetTickCount64() - t_translate_start, translated.size());
    }

    // Restore active state
    badge_.SetStatus(BadgeStatus::Active);

    bool pasted = false;
    // F6 (session 260908_0002, verify 164500 §7): true when this task reached
    // the paste attempt (translation non-empty and != source). PasteAndRestore
    // then fails on exactly ONE path - the H1 foreground-guard abort (the
    // foreground changed while the translation network call was in flight).
    // Distinguishing that abort from the other no-paste outcomes (translation
    // empty / identity) lets the C3 ledger maintenance below preserve a
    // still-valid same-window ledger entry instead of wiping it (the V5
    // defect: the next Enter then re-translated earlier blocks).
    bool paste_attempted = false;
    // B-6a: set when NotifyReplacement still owes its post-newline call (see
    // branch comment at the paste site and the injection block below).
    bool pending_notify = false;
    // REQ-039 FIX-2 (user log L3450-3760, VS Code window): identity outcome -
    // the engine returned the source unchanged, so nothing is pasted and
    // nothing would inject Enter at the send gate below (auto_send=0,
    // bare Enter). The hook already swallowed the user's Enter; ending the
    // task here is the "엔터치면 넘어가지지도 않고" defect: the caret never
    // advances and every retry re-checks the whole capture. The shared
    // predicate (worker.hpp) keeps worker.cpp and the tests on ONE
    // definition of the send-through verdict.
    const bool identity_outcome = !translated.empty() && translated == line &&
                                  EqualsSourceNeedsSendThrough(line.empty(), was_smart_bypassed);
    if (!translated.empty() && translated != line) {
        // F6: this branch is the ONLY paste attempt in ExecuteTask; a false
        // `pasted` below therefore means the H1 foreground-guard abort.
        paste_attempted = true;
        // H1 guard: pass the captured target HWND. PasteAndRestore re-verifies the
        // foreground window immediately before Ctrl+V and aborts on mismatch.
        //
        // F7 (session 260908_0002, log 문제2 clipboard_restored=0): PasteAndRestore
        // reports whether it actually confirmed the ORIGINAL clipboard (text +
        // extra formats) is back before returning. The DIAG field below now logs
        // that REAL outcome (1 = restored, 0 = restore not confirmed yet) instead
        // of the old inverted `pasted ? 0 : 1` shorthand, which read as "restore
        // never happens" in every successful-paste log line and drove this task.
        bool restore_ok = false;
        const ULONGLONG t_paste_start = ::GetTickCount64();
        pasted = PasteAndRestore(translated, backup, task.target_hwnd, &restore_ok);
        DIAG_LOG("PIPELINE", "stage=paste result=%d target_hwnd=%p duration_ms=%llu "
                             "clipboard_restored=%d",
                 pasted ? 1 : 0, reinterpret_cast<const void*>(task.target_hwnd),
                 ::GetTickCount64() - t_paste_start, restore_ok ? 1 : 0);
        // If !pasted (focus shifted): clipboard untouched, nothing leaked. The
        // block selection is still live and MUST be released - a VK_RIGHT into a
        // possibly different foreground window is a benign cursor move, while
        // leaving it selected destroys the user's whole message on their next
        // keystroke (audit §2.2 / §5-3, REQ-R03). Enter remains H1-gated below.
        if (pasted) {
            // F7: one shared definition (worker.hpp ClipboardRestorerStaysArmed)
            // decides the guard. Disarmed ONLY when PasteAndRestore confirmed it
            // restored the original clipboard before returning; a paste whose
            // internal restore failed (transient OpenClipboard contention even
            // after retry) keeps the scope-exit guard armed - its RestoreClipboard
            // at function end is the second attempt that guarantees the user's
            // clipboard data is never left permanently replaced by the
            // translation (the F7 reported defect).
            restorer.active = ClipboardRestorerStaysArmed(pasted, restore_ok);
            if (!restore_ok) {
                DIAG_F("WORKER/ExecuteTask/040: paste succeeded but clipboard restore not confirmed; scope-exit RAII restorer retained as fallback\n");
            }
            // REQ-034 F3-B: stamp the paste time that the empty-capture
            // paste-window gate reads at the NEXT task's EmptyCaptureNeedsHold
            // entry (see PasteWindowSuppressesNotice contract in worker.hpp).
            // Success-only: a failed paste (H1 abort) must not open a
            // suppression window.
            last_paste_ms_.store(::GetTickCount64(), std::memory_order_relaxed);
            // REQ-F2: remember this paste for the next capture comparison
            // (both sides CRLF-normalized). Failure of the next comparison
            // clears it; see the maintenance block at task end.
            // S2 (spec §2.3): the canonical twin is written ATOMICALLY with
            // the original (adjacent statements, same expression sequence);
            // the reader verdict + tail slice consume the twin, the original
            // stays for DIAG logging and the F2 original-representation
            // audit trail (spec §2.6). The original store is the
            // G6-4-pinned single ledger store and remains untouched.
            last_paste_target_ = task.target_hwnd;
            last_paste_text_ = translated;
            last_paste_canonical_ = CanonicalFormForLedger(translated);
            // REQ-F5: remember the paste END offset (live caret right after
            // Ctrl+V consumed the selection) so a later EMPTY capture whose
            // live caret sits at exactly this offset proves "no edit since
            // the paste" and promotes to send-of-output instead of hold_send.
            last_paste_end_offset_ = EditCaretTracker_SampleCaret(task.target_hwnd);
            // BUG-004 F2 (debug analysis 020300 §7, user approval decisions.md
            // 23:05): after the rescue's whole-document paste-back the
            // editor/browser owns the landing - the user saw a lingering
            // whole-composer "Ctrl+A" selection with the viewport yanked to
            // the top. One guarded VK_RIGHT (the SAME ReleaseSelectionOnce
            // primitive every failure path runs - no new chord, no new timing
            // constant: its 10 ms settle is the field-proven cadence)
            // collapses any persisted selection toward its end; at the
            // document end it is a no-op (the caret cannot move past the
            // end). Strictly AFTER the paste success (an earlier collapse
            // would turn the replacement into an insertion - duplication);
            // AFTER the last_paste_end_offset_ sample above so the REQ-F5
            // paste-end baseline records the paste's own landing geometry,
            // not the post-collapse caret. The REQ-R03 release gate below is
            // skipped when pasted==true (SelectionReleaseRequired(true) ==
            // false, pinned above), so the collapse is the ONLY key event this
            // path injects. The H1 foreground guard was re-verified inside
            // PasteAndRestore immediately before its Ctrl+V, milliseconds ago;
            // a same-window identity collapse after that is the same benign
            // exposure class as the failure-path release (audit §5-3).
            if (PostPasteCollapseRequired(pasted, provenance.rescued)) {
                DIAG_F("WORKER/ExecuteTask/043: SelectAll-rescued paste-back landed; collapsing the whole-document selection (one VK_RIGHT, viewport re-reveals the caret)\n");
                ReleaseSelectionOnce();
            }
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
        if (identity_outcome) {
            DIAG_F("WORKER/ExecuteTask/037: identity translation (equals source); intercepted Enter will be handed to the app (no-paste send-through)\n");
        }
    }

    // REQ-R03: exactly-once selection release on every non-paste outcome. The
    // single call site below is the guarantee; the constexpr matrix above pins
    // it at compile time.
    if (SelectionReleaseRequired(pasted)) {
        // BUG-005 4th gate (represcription 061500, Light Gate 060150 REJECT ->
        // paths A/B/C): placed INSIDE this !pasted-only block, BEFORE the
        // VK_RIGHT release. Equivalence proof (report 061500 §1-B): reaching
        // this block with pasted==false means this task injected ZERO editor
        // selection-changing actions (no Ctrl+V - H1 abort pre-emits nothing;
        // no Enter - the send gate is below), so the selection here is
        // IDENTICAL to the capture-exit state S0: a select_all_rescued capture
        // means a LIVE whole-document selection, provably, regardless of
        // whether the release below will be honored or silently dropped
        // (BUG-002 class - unobservable by design). Consuming it here makes
        // the identity paste a replacement over its own selection (byte-safe
        // no-op content edit that collapses the selection editor-owned), so
        // the release below and the send-through Enter can never replace a
        // live selected span. Scope: the predicate's identity_arm input
        // (3-arg form, 061500 §3-E) is identity_outcome, which is false on
        // path B (translated empty: L720 requires non-empty) and path C
        // (successful paste: translated != line); pasted==true cannot enter
        // this block at all (L102 static_assert) - the 1st-Enter mainline
        // rescue paste-back is untouched.
        if (RescueLiveSelectionNeedsConsume(provenance.rescued, !line.empty(), identity_outcome) &&
            ConsumeRescueSelectionIdentityPaste(line, task.target_hwnd, backup, restorer.active, nullptr)) {
            DIAG_F("WORKER/ExecuteTask/044: rescue-captured whole-document selection consumed by identity paste BEFORE the REQ-R03 release (identity arm, pre-release gate); arm=identity_pre_release\n");
        }
        // consume failure (H1 abort) falls through to the legacy contract:
        // the release below runs exactly once (REQ-R03 unchanged), and the
        // Enter below stays H1-gated. Pre-fix behavior byte-identical.
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
    if (identity_outcome) {
        // REQ-039 FIX-2: nothing was pasted (translation == source), so the
        // intercepted Enter's whole purpose collapses to the user's intent -
        // a line-break/s movement the hook already swallowed. Hand it to the
        // app exactly like the established send-through paths (worker log
        // emebalachat_260907204046 L3450-3760: Enter dead, whole content
        // re-checked each retry). The H1 foreground guard below still vets
        // the target; the release above already restored the caret.
        inject_enter = true;
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
                 h1_ok ? "send_gate" : "h1_foreground_mismatch",
                 reinterpret_cast<const void*>(task.target_hwnd));
    }

    // REQ-F2 ledger maintenance. The ledger is the block-start memory of the
    // most recent paste into the CURRENT window context. It must never survive
    // a context where it could misfire:
    //   - successful paste: refreshed above - keep (this is the branch the
    //     next Enter's capture comparison reads).
    //   - H1-abort no-paste into the SAME window (F6, verify 164500 §5/§7):
    //     a paste WAS attempted (translation non-empty, != source) but
    //     PasteAndRestore's foreground guard aborted. Neither the paste nor
    //     the H1-gated Enter ran, so the target window's text is unchanged
    //     from the previous successful paste - the ledger entry for this same
    //     hwnd still describes live text. KEEP it; wiping it (the V5 defect)
    //     emptied the memory and the next Enter's fallback whole-input
    //     selection [0..caret) re-translated and overwrote earlier blocks
    //     (verify 164500 example-3 chain).
    //   - translation empty / identity, SAME window (F3 C3 re-arm, session
    //     260908_0003, verify 220750 §2 R4): the engine produced nothing new
    //     and NOTHING was injected into the window either, so the live text
    //     still ends with exactly what the last successful paste deposited -
    //     the same self-evidence the F6 H1-abort keep uses. KEEP (re-arm the
    //     block start) when the ledger has text: clearing it armed the R4
    //     whole-document destruction chain. An empty/foreign ledger has
    //     nothing to keep and is cleared as before.
    //   - different target hwnd: the previous paste went to another window.
    //     Clear (a stale cross-window memory is worse than none).
    // The (ii) prefix path lands here with pasted==true and its ledger
    // already refreshed to the recomposed full text - the correct new
    // memory: if the user keeps the recomposed output and presses Enter,
    // the skip applies to the WHOLE recomposed text.
    if (!pasted || last_paste_target_ != task.target_hwnd) {
        // F6 + F3-C3: preserve the same-window ledger when its text still
        // describes live input - proven by the paste attempt itself (H1 abort:
        // nothing injected) or by the no-paste outcome (empty/identity: also
        // nothing injected) provided the entry actually HAS text.
        const bool ledger_same_target = (last_paste_target_ == task.target_hwnd);
        const bool ledger_has_text = !last_paste_text_.empty();
        if (LedgerSurvivesNoPaste(paste_attempted, ledger_same_target, ledger_has_text)) {
            DIAG_LOG("PIPELINE",
                     "stage=ledger_maint decision=keep reason=%s "
                     "target=%p ledger_len=%zu",
                     paste_attempted ? "h1_abort_same_hwnd" : "f3_no_paste_rearm_same_hwnd",
                     reinterpret_cast<const void*>(task.target_hwnd),
                     last_paste_text_.size());
        } else {
            // QA-visible wipe evidence (F6 verify protocol: a same-hwnd H1
            // abort must log decision=keep; cross-window must log decision=
            // clear with the exact reason below; F3 adds the no-ledger-text
            // same-hwnd clear, the only remaining same-hwnd wipe).
            const char* clear_reason = "unknown";
            if (pasted) {
                // Unreachable today: a successful paste refreshes the ledger
                // to task.target_hwnd, so this block is never entered with
                // pasted==true. Kept for defensive completeness.
                clear_reason = "pasted_but_cross_hwnd";
            } else if (last_paste_target_ == nullptr) {
                clear_reason = "no_ledger";
            } else if (!paste_attempted && !ledger_has_text) {
                // Same window but the ledger holds no text (defensive: the
                // paste branch only stores non-empty translations): nothing
                // to re-arm on - legacy clear (a no-op wipe).
                clear_reason = "no_paste_no_ledger_text";
            } else if (!paste_attempted) {
                // Unreachable when the ledger has text (F3 keeps it); kept
                // for defensive completeness.
                clear_reason = "no_paste_not_attempted";
            } else {
                // paste_attempted (H1 abort) but the ledger belongs to a
                // DIFFERENT hwnd than this task's target - cross-window
                // contamination hygiene (kept legacy behavior).
                clear_reason = "h1_abort_different_hwnd";
            }
            DIAG_LOG("PIPELINE",
                     "stage=ledger_maint decision=clear reason=%s target=%p",
                     clear_reason, reinterpret_cast<const void*>(task.target_hwnd));
            last_paste_target_ = nullptr;
            last_paste_text_.clear();
            // S2 (spec §7.1.1): the canonical twin clears ADJACENT to the
            // original - no code path may clear one without the other.
            last_paste_canonical_.clear();
            last_paste_end_offset_ = kEditCaretUnknown;
        }
    }

    DIAG_LOG("PIPELINE", "stage=task_end pasted=%d total_ms=%llu",
             pasted ? 1 : 0, ::GetTickCount64() - t_task_start);
}

} // namespace emebalachat
