#pragma once

#include "config.hpp"
#include "engine.hpp"
#include "ui/badge.hpp"
#include "win32_input.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <thread>

namespace emebalachat {

// F3 (session 260908_0003, verify 220750 §6 adopted design): K = the number
// of Shift+Enter passthroughs the hook counted for the CURRENT composition
// block (since the last bare-Enter capture / window switch). The non-EM
// fallback captures the whole [0..caret) accumulation; the worker slices the
// current block (the last K+1 logical lines) out of it and preserves
// everything before the slice point verbatim (see FindCurrentBlockStart).
// 0 for the drag path and any task posted without block context.
struct PipelineTask {
    bool is_shift_enter = false;
    HWND target_hwnd = nullptr;
    int shift_enter_count = 0;
};

// REQ-R03 (Batch D1) path-matrix predicate - single source of truth, shared by
// worker.cpp (pinned there with static_assert at each call decision) and the
// unit tests. ExecuteTask attempts a paste only when the translation is
// non-empty and differs from the source; therefore every no-paste outcome -
// translated empty (engine failure / consent block), translated == source, or
// paste cancelled by the H1 foreground guard - collapses to paste_succeeded
// == false and MUST release the block selection exactly once (VK_RIGHT),
// otherwise the user's next keystroke destroys the whole highlighted message
// (audit §2.2 text evaporation). The ONLY path that skips the release is a
// successful paste, where Ctrl+V consumed the selection itself.
constexpr bool SelectionReleaseRequired(bool paste_succeeded) {
    return !paste_succeeded;
}

// R5 (Debug-Surgical): the Enter-path empty-capture verdict, as one pure
// predicate so worker.cpp and the unit tests assert on ONE definition
// (same discipline as SelectionReleaseRequired). True = the exact case the
// user reported ("Enter sent my text untranslated"): the worker intercepted a
// bare Enter, but the capture came back EMPTY - not a smart-bypass (that
// means the text already matched the target and passing through is the
// product's contract), and not a re-routed keystroke. For empty capture the
// original text is still sitting in the target app; sending Enter would
// submit it untranslated and SILENTLY - the R1 "tooltip must always land"
// complaint. So: hold the send and surface a TooltipNoSelection-style notice
// instead (wired via SetEmptyCaptureCallback, marshaled to the GUI thread
// by the already thread-safe ShowMessageThreadSafe seam).
constexpr bool EmptyCaptureNeedsHold(bool captured_empty, bool smart_bypass) {
    return captured_empty && !smart_bypass;
}

// REQ-034 F3-B (design 173700_architect §2.2.1, user rule: "엔터 치면 자동으로
// 입력되는 것을 체크할 때와 안 체크할 때의 차이가 분명히 있어야 한다"): a retry
// Enter fired right after a SUCCESSFUL paste is a re-translation intent, not
// a "no selection" mistake. The F3 log signature (emebalachat_260907171452
// L432/563/601): stored offset == caret -> EM_SETSEL(last,last) empty range
// -> Ctrl+C changes nothing -> 180 ms stale-refuse -> EMPTY capture -> the R5
// hold above would surface a FALSE TooltipNoSelection notice repeatedly. This
// pure predicate is the time-window gate applied at the worker's
// EmptyCaptureNeedsHold entry: inside the window after the last successful
// paste, the notice is suppressed and Enter is silently delivered to the app
// (ReleaseSelectionOnce + SendEnterKey). Outside the window - no paste
// recorded (sentinel 0) or window elapsed - the general empty Enter keeps the
// existing hold_send + notice behavior EXACTLY (constraint C-5;
// EmptyCaptureNeedsHold itself is untouched). Same shared-definition
// discipline as SelectionReleaseRequired / EmptyCaptureNeedsHold: worker.cpp
// and the unit tests assert on ONE definition.
//
// last_paste_ms uses 0 as the never-pasted sentinel: GetTickCount64() is
// effectively never 0 after system uptime exceeds one millisecond, so 0 is
// unambiguous and keeps "no paste history" out of the window. Non-monotonic
// pairs (now < last, only possible with a clock anomaly) return false so the
// gate can never unsigned-underflow into a bogus "inside window".
constexpr uint64_t kPasteEmptySuppressMs = 2000;

constexpr bool PasteWindowSuppressesNotice(uint64_t now_ms, uint64_t last_paste_ms) {
    if (last_paste_ms == 0) return false;
    if (now_ms < last_paste_ms) return false;
    return (now_ms - last_paste_ms) <= kPasteEmptySuppressMs;
}

// REQ-039 FIX-2 (VS Code / editor whole-content re-check): a capture whose
// translation EQUALS the source (identity outcome - e.g. the engine returns
// the text unchanged, or auto-translation of an already-translated doc)
// currently ends the task WITHOUT injecting Enter and WITHOUT pasting. The
// hook already intercepted and swallowed the user's bare Enter, so the user
// experiences "Enter does nothing: the caret never advances, and the whole
// content is re-checked on every retry" (user log emebalachat_260907204046
// L3450-3760, VS Code window: identical whole-file capture each Enter).
// One pure predicate so worker.cpp and the unit tests assert on ONE
// definition (same discipline as EmptyCaptureNeedsHold /
// PasteWindowSuppressesNotice): identity translations must hand the
// intercepted Enter to the target app exactly like the established
// send-through paths - the user's intent (line-break / send) is never lost.
//
// should_translate is the worker's decision for the captured block; the
// R5 hold branch (capture_empty) is upstream and unaffected. A smart
// bypass (already-target-language) is a positive product decision whose
// send-through contract is already pinned - this predicate covers ONLY the
// newly-translated-but-unchanged outcome, keeping the two contracts
// disjoint.
constexpr bool EqualsSourceNeedsSendThrough(bool captured_empty, bool smart_bypassed) {
    return !captured_empty && !smart_bypassed;
}

// REQ-F2 (session 260908_0001, log emebalachat_260908062830 L1561/L1858/L1915):
// category=0 apps (CategoryB, non-EM focus control - EVA_Window_Dblclk) fall
// back to SelectMessageBlock's whole-input geometry [0..caret). With
// auto_send=0 the send gate skips Enter, so the pasted translation REMAINS in
// the input. The next bare Enter then re-captures that leftover verbatim
// (44 -> 112 -> 200-char accumulation in the log), and each round trip
// re-translates the previous output - compounding drift. The worker keeps a
// "last paste ledger": (target hwnd, pasted text) of the most recent
// SUCCESSFUL paste. This pure predicate decides what a capture that matches
// the remembered prefix means, as ONE definition shared by worker.cpp and the
// unit tests (same discipline as EmptyCaptureNeedsHold):
//  - capture_equals_last_paste: the input still holds EXACTLY what we pasted
//    last time. The user's bare Enter is a SEND of our own output, never a
//    re-translation request - re-running the engine would risk rephrasing
//    (or, worse, identity churn). True -> skip translation, hand Enter to
//    the app exactly like the smart-bypass send-through contract.
//  - smart_bypassed: disjoint positive decision, never overridden.
//  - captured_empty: upstream R5 hold owns the empty case; this predicate
//    never sees it in practice, but refuses it for safety.
constexpr bool PastedPrefixNeedsSkip(bool capture_equals_last_paste, bool smart_bypassed,
                                     bool captured_empty) {
    return !captured_empty && !smart_bypassed && capture_equals_last_paste;
}

// REQ-F5 (docs/260908_0001 session, verification log
// emebalachat_260908082659 L435-472/L504-520/L556-607): the bare-Enter path
// whose capture is EMPTY (len=0). The old R5 hold (EmptyCaptureNeedsHold) and
// the REQ-034 paste-window suppress decided this case by TIME alone: inside
// 2 s of the last paste -> silent send-through (036); outside -> hold_send +
// no-selection notice (035). The F5 residual is the OUTSIDE case in an EM
// tracked editor (Notepad): after a successful paste the send gate left the
// translation in the input and the stored offset is the paste END. A bare
// Enter then yields [offset..caret) = empty selection -> empty capture (the
// "refusing stale read / provably empty" signatures), which is the user's
// SEND-of-output intent - not a no-selection mistake. The geometry proof of
// "no edit since the paste" is: live caret == stored offset == paste end.
// This pure predicate is the promotion decision as ONE definition shared by
// worker.cpp and the unit tests (same discipline as EmptyCaptureNeedsHold):
//   - captured_empty:           only the empty-capture case is expanded.
//   - smart_bypassed:            disjoint positive decision, never promoted.
//   - last_paste_valid (hwnd):   the ledger has an entry for THIS window.
//   - caret_equals_paste_end:    live caret == remembered paste-end offset.
// Both offsets are UTF-16 code units from EM_GETSEL; kEditCaretUnknown means
// "sampling failed / untracked", which can never equal (a real caret >= 0) nor
// (the stored end, also >= 0) - so an Unknown on EITHER side refuses, never
// promotes on a coincidence. A moved caret (backspace deleting pasted text,
// arrow-move, or new typing past the end) breaks the equality -> refuse ->
// existing behavior. All three must hold to promote.
constexpr bool EmptyCapturePromotesToSend(bool captured_empty, bool smart_bypassed,
                                          bool last_paste_valid, bool caret_equals_paste_end) {
    return captured_empty && !smart_bypassed && last_paste_valid && caret_equals_paste_end;
}

// F3 (session 260908_0003, verify 220750 §6 "수정-Ananke" adopted design):
// block-slice-from-whole-capture. The non-EM CategoryB fallback selection is
// the WHOLE [0..caret) accumulation; the CURRENT block is the last K+1
// logical lines, K = the Shift+Enter passthroughs the hook counted since the
// last bare-Enter capture. Everything before the slice point is earlier
// (already-translated) content and MUST be preserved verbatim - the worker
// recomposes [prefix][translation of block] with the SAME machinery as the
// REQ-F2 PrefixWithTail branch, so 예시1/2/3 hold even when the ledger chain
// broke (the R2/R4 whole-document destruction path of verify 220750 §2).
//
// Pure helper (no Win32 calls, unit-testable headlessly). Input should be
// CRLF-normalized (ExecuteTask normalizes at the capture seam) but lone-LF /
// lone-CR separators are tolerated: a \r\n PAIR is one logical newline, and
// a lone \r or \n is one too. Two ADJACENT terminators ("AAA\r\n\r\nBBB")
// are TWO boundaries with an empty logical line between them - exactly the
// split() semantics the "last K+1 LOGICAL lines" contract needs (merging
// them would under-count Shift+Enters and reach back into translated
// content). Returns the START INDEX of the current block within `capture`:
//   - empty capture, negative K, or fewer than K+1 terminators in the text
//     -> 0 (whole capture is the block: first-block geometry, legacy safe);
//   - otherwise: the index immediately after the (K+1)-th terminator
//     counted from the END. A capture ending in a terminator (caret on a
//     fresh empty line) with K=0 therefore yields index == capture.size() -
//     the "empty tail" the worker turns into a plain send-through (041)
//     instead of re-translating the prefix;
//   - K over-count (fewer terminators than K+1) clamps to 0, never out of
//     bounds. Word-wrap is irrelevant BY DESIGN: the clipboard capture only
//     ever contains LOGICAL newlines (the app's own line breaks), never the
//     display-line folds - exactly why the rejected Shift+Up x K geometry
//     (verify 220750 §6-(i)) is not needed here.
// Surrogate safety: 0x0D/0x0A never participate in UTF-16 surrogate pairs,
// and the returned index always sits immediately after a terminator.
constexpr size_t FindCurrentBlockStart(std::wstring_view capture, int shift_enter_count) {
    if (capture.empty() || shift_enter_count < 0) {
        return 0;
    }
    const int want = shift_enter_count + 1; // terminators to cross from the end
    int crossed = 0;
    size_t i = capture.size();
    while (i > 0) {
        const wchar_t c = capture[i - 1];
        if (c != L'\r' && c != L'\n') {
            --i;
            continue;
        }
        // One logical terminator ends at `i`: a \r\n pair (two units) or a
        // lone \r / \n (one unit). A lone \r BEFORE another \r (or end-of-
        // scan) is its own line break; the \n of a pair swallows its \r.
        size_t term_start = i - 1;
        if (c == L'\n' && term_start > 0 && capture[term_start - 1] == L'\r') {
            --term_start;
        }
        ++crossed;
        if (crossed == want) {
            // The block starts right after this terminator (`i` is the index
            // one past its end). When the capture ENDS in a terminator,
            // i == capture.size(): the empty-tail shape the worker trims to
            // a send-through.
            return i;
        }
        i = term_start;
    }
    return 0; // fewer than K+1 terminators: the whole capture is the block
}

// REQ-F2: pure decomposition of a capture against the last-paste ledger,
// shared by worker.cpp and the unit tests (ONE definition discipline). The
// verdict decides the accumulation defense in ExecuteTask:
//  - ExactMatch:    input still holds EXACTLY the pasted translation ->
//                   the Enter is a send-of-output (skip re-translation).
//  - PrefixWithTail: input holds the pasted translation followed by newly
//                   typed text -> translate ONLY the tail (offset =
//                   last_paste.size(), a whole-unit boundary: last_paste is
//                   a complete stored string, so the split can never land
//                   inside a surrogate pair).
//  - NoMatch:       anything else - user edited our output, deleted from
//                   it, typed BEFORE it, or the capture belongs to a
//                   different context. F3 (verify 220750 §2 R2/R4): the
//                   worker now slices the current block from the whole
//                   capture (FindCurrentBlockStart) in this arm, so earlier
//                   blocks keep their language and text.
// Both inputs are already CRLF-normalized by the caller.
enum class PasteLedgerVerdict { NoMatch, ExactMatch, PrefixWithTail };

inline PasteLedgerVerdict AnalyzeCaptureVsLastPaste(std::wstring_view captured,
                                                    std::wstring_view last_paste) {
    if (last_paste.empty() || captured.empty() ||
        captured.size() < last_paste.size()) {
        return PasteLedgerVerdict::NoMatch;
    }
    bool prefix_equal = true;
    for (size_t i = 0; i < last_paste.size(); ++i) {
        if (captured[i] != last_paste[i]) {
            prefix_equal = false;
            break;
        }
    }
    if (!prefix_equal) {
        return PasteLedgerVerdict::NoMatch;
    }
    return captured.size() == last_paste.size() ? PasteLedgerVerdict::ExactMatch
                                                : PasteLedgerVerdict::PrefixWithTail;
}

// F6 (session 260908_0002, verify report 164500 §5/§7, V5 ledger-on-focus-clear
// defect): the C3 ledger-maintenance `!pasted` arm is subdivided. A paste is
// ATTEMPTED only when the translation is non-empty and differs from the source
// (the worker's paste branch); PasteAndRestore then returns false on exactly
// one path - the H1 foreground-guard abort (win32_input.cpp PasteAndRestore:
// the foreground changed while the network translation was in flight). In that
// abort the target window receives NO paste and NO synthetic Enter (both are
// H1-gated), so its text state is byte-identical to what the previous
// SUCCESSFUL paste into it left behind: when the ledger entry belongs to the
// SAME target hwnd this task operated on, it still describes live text and
// must be PRESERVED. Wiping it (the pre-F6 unconditional clear) emptied the
// ledger and let the next Enter's fallback whole-input selection [0..caret)
// re-translate and overwrite earlier translated blocks (verify 164500 §1.4
// example-3 chain). A different target hwnd keeps the legacy clear
// (cross-window contamination hygiene). One pure definition shared by
// worker.cpp and the unit tests (same discipline as the other predicates).
// The ledger's own self-invalidation still applies: any later edit makes the
// next AnalyzeCaptureVsLastPaste comparison fail and route to legacy behavior.
//
// F3 C3 RE-ARM (session 260908_0003, verify 220750 §2 R4): the legacy clear
// for the no-paste-not-attempted outcomes (translation empty / identity,
// same hwnd) is replaced by KEEP when the ledger actually HAS text. Neither
// the engine failure nor the identity result modified the target window - the
// window's content still ends with exactly what the last successful paste
// deposited, so the ledger still describes live text and re-arming the block
// start on it (instead of losing it) keeps the PrefixWithTail defense one
// link alive: the next Enter's capture stays a tail-only translation and the
// F3 slice stays a pure fallback. An EMPTY ledger has nothing to keep (the
// clear is a no-op there), so the third input gates the re-arm. Renamed from
// LedgerSurvivesH1Abort to reflect the widened (F6 + F3-C3) contract.
constexpr bool LedgerSurvivesNoPaste(bool paste_attempted, bool ledger_same_target_hwnd,
                                     bool ledger_has_text) {
    return ledger_same_target_hwnd && (paste_attempted || ledger_has_text);
}

// F7 (session 260908_0002, log 문제2 clipboard_restored=0): whether the
// worker's scope-exit RAII clipboard restorer (ExecuteTask's RestorerGuard)
// must STAY ARMED after the paste attempt. It may be disarmed ONLY when the
// paste succeeded AND PasteAndRestore confirmed the ORIGINAL clipboard (text +
// extra formats) was restored before returning (win32_input.cpp out-param).
// Every other combination keeps the guard armed so the original clipboard is
// restored at ExecuteTask exit at the latest:
//   - paste aborted (H1 foreground-guard): the target never received the
//     translation, but the earlier Ctrl+C capture displaced the clipboard; the
//     scope-exit restore puts the user's pre-task content back.
//   - paste succeeded but internal restore failed (transient OpenClipboard
//     contention even after PasteAndRestore's retry): leaving the guard armed
//     gives a SECOND restore attempt at scope exit - without it the translated
//     text would remain on the user's clipboard permanently (the exact defect
//     F7 reports: backup collected, then discarded).
// One pure definition shared by worker.cpp and the unit tests (same discipline
// as LedgerSurvivesH1Abort above).
constexpr bool ClipboardRestorerStaysArmed(bool pasted, bool restore_confirmed) {
    return !(pasted && restore_confirmed);
}

class PipelineWorker {
public:
    PipelineWorker(AppConfig& config, TranslationManager& engine, FloatingBadge& badge);
    ~PipelineWorker();

    void Start();
    void Stop();

    // Enqueues a translation pipeline task if not already busy.
    // Returns true if task was accepted, false if currently busy.
    // F3 (session 260908_0003): shift_enter_count is the hook's K for the
    // current composition block (see PipelineTask::shift_enter_count); 0 for
    // callers without block context (double-Ctrl+C, tests).
    bool PostTask(bool is_shift_enter, HWND target_hwnd = nullptr,
                  int shift_enter_count = 0);

    // Returns true if the worker is actively executing a task.
    bool IsBusy() const { return is_busy_.load(std::memory_order_relaxed); }

    // R5 (Debug-Surgical): called (on the worker thread, never the hook
    // thread) exactly when the Enter path takes the new hold-and-notice
    // empty-capture branch (see EmptyCaptureNeedsHold). The registered
    // callback must marshal to the GUI thread itself - main.cpp registers a
    // wrapper over TooltipWindow::ShowMessageThreadSafe, which is already the
    // REQ-R10 thread-safe seam. Follows the existing SetXxxCallback contract:
    // registered once at startup BEFORE Start(), read-only afterwards
    // (identical discipline to KeyboardHook::SetDoubleCtrlCCallback), so the
    // worker thread reads it without a lock.
    void SetEmptyCaptureCallback(std::function<void()> cb) {
        empty_capture_cb_ = std::move(cb);
    }

private:
    void WorkerLoop(std::stop_token stop_token);
    void ExecuteTask(const PipelineTask& task);

    AppConfig& config_;
    TranslationManager& engine_;
    FloatingBadge& badge_;

    std::atomic<bool> is_busy_{false};
    std::atomic<bool> running_{false};

    // R5: set once at startup via SetEmptyCaptureCallback (see contract there).
    std::function<void()> empty_capture_cb_;

    // REQ-034 F3-B: GetTickCount64() stamp of the last SUCCESSFUL paste
    // (pasted == true branch in ExecuteTask). Read by the empty-capture
    // paste-window gate at the EmptyCaptureNeedsHold entry (see
    // PasteWindowSuppressNotice contract in this header). Written and read
    // only on the pipeline worker thread - the atomic is defensive
    // (design §2.2.1), so relaxed ordering is sufficient. 0 = never pasted.
    std::atomic<uint64_t> last_paste_ms_{0};

    // REQ-F2: last paste ledger - the (target, pasted text) memory that the
    // capture stage compares against (see PastedPrefixNeedsSkip above).
    // At most one entry: only the most recent successful paste matters. All
    // reads/writes happen on the pipeline worker thread inside ExecuteTask;
    // plain members are sufficient (same single-thread discipline as
    // last_paste_ms_'s design intent). hwnd==nullptr means "no memory".
    HWND last_paste_target_ = nullptr;
    std::wstring last_paste_text_;
    // REQ-F5: the pasted text's END offset in the tracked window, sampled
    // (EM_GETSEL) at the moment of the last successful paste - the geometry
    // proof that a later empty capture means "no edit since the paste".
    // kEditCaretUnknown (UINT32_MAX, from win32_input.hpp) = no valid memory;
    // refreshed in lockstep with last_paste_target_/last_paste_text_ and
    // cleared in the same maintenance block, so the three never diverge.
    // Single-worker-thread discipline, same as the ledger pair above.
    DWORD last_paste_end_offset_ = kEditCaretUnknown;

    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::queue<PipelineTask> queue_;
    std::jthread thread_;
};

} // namespace emebalachat
