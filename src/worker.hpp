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

// BUG-004 F2 (session 260913_0002, debug analysis 020300 §7, user approval
// decisions.md 23:05): the SelectAll rescue leaves a LIVE whole-document
// selection that the paste consumes, but the editor/browser owns the landing
// state of a text-height-scale replacement (the documented WebKit paste
// scroll-to-top class) and REQ-R03 deliberately skips the release on paste
// success - so nothing re-normalized the caret/selection/scroll and the user
// saw a lingering "everything selected" state with the viewport at the top.
// This pure predicate gates the remedy as ONE definition shared by worker.cpp
// and the unit tests (same discipline as SelectionReleaseRequired): exactly
// one VK_RIGHT (the same field-proven ReleaseSelectionOnce primitive every
// failure path runs) strictly AFTER a SUCCESSFUL rescue paste-back. Both
// inputs must hold - a non-rescue paste success keeps the REQ-R03 no-release
// contract byte-identical (the selection was consumed and landed fine), and
// a failed paste keeps the REQ-R03 release (never a collapse on top of it).
constexpr bool PostPasteCollapseRequired(bool paste_succeeded, bool select_all_rescued) {
    return paste_succeeded && select_all_rescued;
}

// BUG-005 (session 260914_0001, user report 2026-09-14 03:07 KST, build r4
// 26a3fce): the SECOND-Enter data loss on the rescue path. The SelectAll
// rescue's whole-document selection stays LIVE in the editor through the
// capture (that highlight is what the user saw fire again on the second
// Enter); a send-through terminal then hands the intercepted Enter to the
// app with only the single VK_RIGHT release (10 ms settle) standing between
// the live selection and the Enter. On the structured-contenteditable class
// the documented failure mode (BUG-002, win32_input.hpp contract block) is
// that a synthetic caret-motion key can silently fail against the editor's
// reconciled selection - so the Enter lands on the still-live
// whole-document selection and REPLACES it: the entire composer except the
// trailing residue is destroyed (the user's report: only the last line
// survived). This pure predicate gates the remedy as ONE definition shared
// by worker.cpp and the unit tests (same discipline as
// SelectionReleaseRequired / PostPasteCollapseRequired): a send-through
// terminal whose capture came from the SelectAll rescue (provenance) with a
// non-empty payload (the live selection has span) MUST consume the
// selection with the identity-paste mechanism (the editor-owned replace
// the 1st Enter itself uses) before the Enter is injected. Both inputs must
// hold: a NON-rescued capture keeps the REQ-R03 VK_RIGHT release contract
// byte-identical (classic editors, the class the failure paths were
// field-proven on), and an EMPTY rescue capture left no whole-document
// span to consume (collapsed/empty payload).
//
// BUG-005 3-arg extension (represcription 061500 §3-E, Light Gate 060150
// REJECT paths B/C): `identity_arm` scopes the gate to the identity
// send-through terminal only. `identity_outcome` (worker.cpp L720:
// !translated.empty() && translated == line && EqualsSourceNeedsSendThrough)
// is FALSE on the two non-identity paths that reach the relocated pre-release
// gate site: Path B - translated.empty() (engine failure / consent block)
// fails the !translated.empty() requirement, and Path C - a successful paste
// requires translated != line (paste entry condition), which contradicts the
// identity equality. The default `= true` keeps the three legacy arms
// (empty-tail / exact-match / bypass, 2-arg call sites) byte-identical;
// only the identity arm passes the third argument. Path C is additionally
// double-blocked by placement: pasted==true can never enter the
// SelectionReleaseRequired block (worker.cpp static_assert).
constexpr bool RescueLiveSelectionNeedsConsume(bool select_all_rescued, bool capture_nonempty,
                                               bool identity_arm = true) {
    return select_all_rescued && capture_nonempty && identity_arm;
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
// -> Ctrl+C changes nothing -> stale-refuse -> EMPTY capture -> the R5
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
// block-slice-from-whole-capture lives in win32_input.hpp as
// `FindCurrentBlockStart` (session 260913_0001, debug report 022121 §7
// Step 2: RELOCATED so the capture seam's slice-before-guard and the
// worker's F3 slice share the ONE pure definition - see the full contract
// comment there). This header includes win32_input.hpp above, so the name
// stays visible to worker.cpp and run_tests.cpp exactly as before; the
// move changed no semantics.

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

// S2 (session 260914_0001, design spec 222500 §1): canonical form for the
// ledger comparison. ProseMirror re-renders pasted text through a fixed
// normalization (CRLF -> LF, trailing-whitespace trim per line, NBSP -> SP,
// Unicode NFC composition), so byte-equality against the raw paste breaks
// deterministically on re-render and makes the ExactMatch routing
// non-deterministic (BUG-005 §9-3). Canonicalizing BOTH sides of
// AnalyzeCaptureVsLastPaste through this function makes the verdict
// deterministic while preserving exact-match semantics: after the transform
// set the comparison remains strict UTF-16 byte equality. Fuzzy matching,
// similarity thresholds, and edit distance are permanently prohibited
// (docs/adr/ADR-001-ledger-canonical-form-exact-match.md).
//
// Transformation order is FIXED (spec §1.2):
//   1. CRLF -> LF               (must run first: trailing-space removal must
//                               see final line endings, spec §1.3 U7 pin)
//   2. trailing [ \t] removal   (per line; NBSP is Unicode White_Space and is
//                               removed here too - spec §1.3 order analysis)
//   3. NBSP (U+00A0) -> SP      (ProseMirror converts NBSP in text nodes)
//   4. Unicode NFC              (composes Korean jamo / accented sequences;
//                               runs last: steps 1-3 are ASCII-only)
//
// Properties (spec §1.4, pinned by TestLedgerCanonicalForm):
//   - pure: input is unmodified, returns a new string (RVO/NRVO)
//   - idempotent: canonical(canonical(x)) == canonical(x)
//   - symmetric: canonical(a) == canonical(b) is an equivalence relation
//   - never over-normalizes: ZWSP/BOM/fullwidth/ideographic space pass
//     through (negative grid N7-N10; content-self-invalidation preserved)
//
// NFC failure fallback (spec §6.3): NormalizeString failure returns the
// steps-1-3 result unnormalized. Never crashes, never blocks. One definition
// shared by worker.cpp and the unit tests (same discipline as
// AnalyzeCaptureVsLastPaste above).
std::wstring CanonicalFormForLedger(std::wstring_view input);

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

// REQ-041 (session 260917, Reddit 재번역 사용자 보고): dead-ledger NoMatch
// 캡처에서 K를 0으로 강제하는 순수 판정. ledger_present = 이 창에 앱이
// 붙여넣은 번역문이 아직 기억되어 있음(= 지난 Enter 어딘가에서 번역이
// 성공했다는 뜻). 이 상태에서 NoMatch = 윗쪽에 사용자 편집이나 에디터
// 재직렬화가 있었다는 증거이고, 이 순간 키스트로크 회계(K = Shift+Enter
// 횟수)는 "문서 개행 1개 = 키 1회" 전제가 이미 깨졌으므로 신뢰 근거가
// 소멸한다(경계 개행 삭제 / 에디터가 합성 Enter 삼킴 / Ctrl+V 포화 등).
// K를 0으로 강제하면 블록은 "마지막 논리줄"로 제한되고, verbatim prefix는
// 라이브 캡처 바이트에서 그대로 가져오므로(F3 raw arm 기존 동작) 이미
// 번역된 윗줄 - 사용자의 수동 수정분 포함 - 이 절대 다시 번역·덮어씌워지지
// 않는다. under-slice는 텍스트를 놓칠 뿐이고 over-claim은 이미 번역된
// 내용을 파괴한다는 BUG-003의 안전 방향 원칙(docs/260913_0002
// 004200_debug-surgical-bug003-report.md §5 scope note)을 그대로 적용.
// ledger가 없으면(초기 작성/창 전환) K를 그대로 신뢰 — 첫 작성 다중 줄
// 블록 통째 번역 계약(verify 220750 예시1) 유지, 회귀 없음.
// One definition shared by worker.cpp and the unit tests (same pure-
// predicate discipline as AnalyzeCaptureVsLastPaste above).
constexpr int EffectiveSliceKForVerdict(int k_block, bool ledger_present,
                                        PasteLedgerVerdict verdict) {
    return (ledger_present && verdict == PasteLedgerVerdict::NoMatch) ? 0 : k_block;
}

// REQ-042 (session 260917_0002, 사용자 Reddit 현장 검증 후속): dead-ledger
// NoMatch에서의 "tail-unchanged short-circuit" 순수 판정 계열. 배경: REQ-041이
// NoMatch를 마지막 논리줄 1줄로 캡(k_eff=0)한 뒤, 그 마지막 줄이 이미 우리가
// 붙여넣은 번역문 그 자리(=레저의 마지막 줄)와 바이트 동일하면 엔진에 넣을
// 신규 텍스트가 사실상 없다. 이때도 엔진을 돌리면 (a) identity여도 지연만
// 추가되고 (b) 모델이 재표현을 바꾸면 이미 받아들인 번역줄이 통째로 치환되는
// churn이 생긴다(사용자 보고 "왜 전체 텍스트를 다시 번역하나요?"의 원인).
// 해결: 엔진 호출과 붙여넣기를 생략하고 Enter만 통과시킨다.
//
// DeadLedgerTailUnchanged: 캡 경로에서 "블록(=캡처의 마지막 논리줄)이 레저의
// 마지막 논리줄과 정확히 같음"을 판정. 양쪽 입력은 호출자가 CanonicalFormFor-
// Ledger를 통과시킨 캐노니컬 표현이어야 한다(S2 계약: ProseMirror 재직렬화의
// CRLF/NBSP/NFC 변환을 흡수하면서도 여전히 정확한 바이트 비교 — ADR-001의
// no-fuzzy 원칙 유지). ledger_last_line_canonical은 LastLogicalLine()로 레저
// 캐노니컬에서 추출. 빈 꼬리/빈 레저 줄은 거짓(정상 흐름 유지).
//   - ledger_present/verdict/k_eff 조건은 EffectiveSliceKForVerdict와 동일한
//     전제를 명시적으로 재선언(호출부가 실수로 캡 없는 경로에서 발동하는 것을
//     방지).
//   - 윗줄 편집분은 verbatim prefix로 보존(REQ-041 그대로)되므로, 이 판정이
//     참이어도 사용자 편집은 절대 덮어씌워지지 않는다. under-slice 방향.
constexpr bool DeadLedgerTailUnchanged(bool ledger_present, PasteLedgerVerdict verdict,
                                       int k_eff, std::wstring_view tail_canonical,
                                       std::wstring_view ledger_last_line_canonical) {
    return ledger_present && verdict == PasteLedgerVerdict::NoMatch && k_eff == 0 &&
           !tail_canonical.empty() && !ledger_last_line_canonical.empty() &&
           tail_canonical == ledger_last_line_canonical;
}

// REQ-042: 캐노니컬 텍스트의 마지막 논리줄 추출(마지막 '\n' 이후 부분).
// '\n'이 없으면 전체가 마지막 줄. 끝이 '\n'으로 끝나면 빈 뷰(빈 줄) — 호출부
// 판정이 거짓으로 떨어지는 안전 방향. CanonicalFormForLedger 출력에 대해
// 사용한다(줄 끝 공백은 이미 트림됨).
constexpr std::wstring_view LastLogicalLine(std::wstring_view canonical_text) {
    const size_t pos = canonical_text.rfind(L'\n');
    return (pos == std::wstring_view::npos) ? canonical_text : canonical_text.substr(pos + 1);
}

// REQ-042 계획-2: under-slice 피드백 툴팁 발동 판정(순수 조합). 네 조건이
// 전부 참일 때만 발동:
//   - tail_unchanged_shortcircuit: 위 DeadLedgerTailUnchanged 판정이 참(=
//     "위에 편집이 있었고, 마지막 줄은 우리 출력 그대로"라는 정확한 상황 —
//     위양성을 최소화하는 핵심 게이트),
//   - residue_translatable: verbatim prefix에 아직 번역 대상이 되는 텍스트가
//     남아 있음(worker가 ShouldTranslate(prefix)로 계산 — 기존 스마트
//     바이패스 판정과 동일한 정의),
//   - category_b: 에디터 계열 창(CategoryA 채팅 전송 직후 팝업은 혼란만
//     주므로 제외),
//   - callback_registered: 시작 시 등록된 알림 콜백이 있음(없으면 계획-1의
//     순수 통과 동작으로 무음 저하 — 새로운 무음 삼킴 경로는 생기지 않음).
// 발동 시에도 paste/선택/레저 상태는 전혀 건드리지 않는다(순수 알림).
constexpr bool UntranslatedResidueNoticeWarranted(bool tail_unchanged_shortcircuit,
                                                  bool residue_translatable, bool category_b,
                                                  bool callback_registered) {
    return tail_unchanged_shortcircuit && residue_translatable && category_b &&
           callback_registered;
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

    // REQ-003 (Issue C, session 260910_0003): the target_hwnd of the task
    // currently in flight, for the hook thread's same-window bare-Enter
    // guard (BusyEnterSameWindowSuppressed, hook.hpp). nullptr while idle.
    // Published under the exact discipline of is_busy_: set by PostTask
    // BEFORE the queue push (the hook may read it as soon as IsBusy() flips
    // true), cleared in ExecuteTask's BusyGuard destructor right AFTER
    // is_busy_ goes false, so the pair (busy, hwnd) never advertises a
    // finished task. Relaxed ordering suffices - same as IsBusy(): the
    // consumer re-checks nothing finer-grained off this value, and HWND is
    // a pointer-sized scalar. Hook-thread read + worker-thread write only.
    HWND BusyTargetHwnd() const {
        return busy_target_hwnd_.load(std::memory_order_relaxed);
    }

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

    // REQ-042 (계획-2): called (on the worker thread) exactly when the Enter
    // path takes the tail-unchanged short-circuit AND the verbatim prefix
    // still holds translatable text (see UntranslatedResidueNoticeWarranted).
    // Same contract as SetEmptyCaptureCallback: registered once at startup
    // BEFORE Start(), read-only afterwards, so the worker thread reads it
    // without a lock. main.cpp registers a TooltipWindow::ShowMessageThreadSafe
    // wrapper (the REQ-R10 seam) that surfaces the localized
    // StringId::TooltipUntranslatedAbove notice - a pure notification; the
    // predicate guarantees paste/selection/ledger state is untouched here.
    void SetUntranslatedResidueCallback(std::function<void()> cb) {
        untranslated_residue_cb_ = std::move(cb);
    }

private:
    void WorkerLoop(std::stop_token stop_token);
    void ExecuteTask(const PipelineTask& task);

    AppConfig& config_;
    TranslationManager& engine_;
    FloatingBadge& badge_;

    std::atomic<bool> is_busy_{false};
    std::atomic<bool> running_{false};

    // REQ-003: in-flight task's target, published in PostTask before the
    // queue push and cleared on ExecuteTask exit (after is_busy_). Read by
    // the hook thread via BusyTargetHwnd(). See that accessor's contract.
    std::atomic<HWND> busy_target_hwnd_{nullptr};

    // R5: set once at startup via SetEmptyCaptureCallback (see contract there).
    std::function<void()> empty_capture_cb_;

    // REQ-042: set once at startup via SetUntranslatedResidueCallback (see
    // contract there). Single-worker-thread read inside ExecuteTask.
    std::function<void()> untranslated_residue_cb_;

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
    // S2 (session 260914_0001, design spec 222500 §2.3/§7.1.1): canonical
    // twin of last_paste_text_, written ATOMICALLY with the original in the
    // paste-success block (one adjacent statement pair, same expression
    // sequence). The ledger verdict and the PrefixWithTail tail slice both
    // operate in THIS representation (spec §2.3 v2): length-changing
    // transforms (CRLF -k, NFC jamo -4) destroy the pre-S2 byte-prefix
    // invariant "last_paste_text_.size() == matching prefix length of line",
    // so prefix length is only well-defined in canonical space.
    //
    // Lifecycle contract (spec §7.1.1; every quiescent point MUST satisfy):
    //   last_paste_canonical_.empty() == last_paste_text_.empty()
    //   - init:            both empty (members' default init)
    //   - successful paste: written adjacent to last_paste_text_ = translated
    //   - invalidation:    cleared ADJACENT to every last_paste_text_.clear()
    //                      (both existing sites: the send-through one-shot
    //                      clear and the ledger_maint wipe) - there must be
    //                      NO code path that clears one without the other
    //                      (a stale canonical twin would make the next Enter
    //                      compare against a DIFFERENT paste's canonical
    //                      form - the cross-task contamination class BUG-005
    //                      fixed for the original ledger)
    //   - tear-down:       same worker destruction as last_paste_text_
    // Single-worker-thread discipline, same as the ledger pair above.
    std::wstring last_paste_canonical_;
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
