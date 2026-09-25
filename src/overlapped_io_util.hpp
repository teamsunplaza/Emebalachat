// overlapped_io_util — REQ-L02 (session 260923_0001): CancelIoEx 후 커널
// 취소 승인 대기 패턴. "취소 요청 -> 커널 완료 승인 -> 그 뒤에만 이벤트/버퍼
// 해제" 순서를 한 함수로 강제한다. 곧바로 이벤트를 닫으면 커널이 이미 진행
// 중인 IRP 완료 처리에 닫힌 이벤트를 참조해 파괴 스택에 기록한다(UAF).
//
// 규칙 (설계 A-1.4, 사용자 처방):
//   * 이 헬퍼는 파이프 핸들을 절대 닫지 않는다.
//   * "취소 완료 전 파이프 핸들 close 금지" — CancelIoEx를 건 경로는 커널
//     취소 승인(GetOverlappedResult bWait=TRUE)이 끝난 뒤에만 핸들을 닫는다.
//   * 모든 취소 지점(공용 경로)은 이 헬퍼 하나로만 처리한다.
// REQ-L32 부록 ②-4 (session 260925): Listener a321916 + 260925 완료경주 수정본을
// 양쪽 클라이언트 공용으로 verbatim 이식 (Chat 카피의 WriteFrame/ReadFrame 동일
// 결함 해소 + host WaitForClientClose 2차 후보 동일 처리).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace emebalachat {
namespace engine_host {

// P5-fix D2 (session 260923_0001): RAII owner for the per-request overlapped
// completion event. Every frame path (ReadFrame/WriteFrame/RecvFrame/SendFrame)
// previously relied on a manual CloseHandle at each exit point — a321916's
// timeout early-return dropped the close on the false path from
// WaitOverlappedOrCancelApprove, leaking exactly one kernel event handle per
// timeout (measured: 50/50 linear). Owning the handle in a scope-bound object
// makes the leak structurally impossible: the destructor runs on EVERY exit
// path, including the early returns the review fixed.
//
// Ordering guarantee (REQ-L02): WaitOverlappedOrCancelApprove does not return
// on the timeout path until the kernel cancel-ack (GetOverlappedResult
// bWait=TRUE) completes. Because the event is destroyed only when the calling
// scope ends — which is strictly AFTER the helper returns — the event outlives
// the cancel approval on every code path. Safe.
class ScopedOverlappedEvent {
public:
    ScopedOverlappedEvent() : h_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ScopedOverlappedEvent() {
        if (h_) ::CloseHandle(h_);
    }
    ScopedOverlappedEvent(const ScopedOverlappedEvent&) = delete;
    ScopedOverlappedEvent& operator=(const ScopedOverlappedEvent&) = delete;

    bool IsValid() const { return h_ != nullptr; }
    HANDLE Get() const { return h_; }

private:
    HANDLE h_;
};

// I/O 완료 대기(타임아웃 포함) 후 실패 시: CancelIoEx -> GetOverlappedResult
// (bWait=TRUE)로 커널 취소 승인 대기 -> 그 뒤에만 이벤트 해제. 호출자는 이
// 함수 반환 후에만 버퍼/이벤트를 해제핮어도 된다.
//   반환 true  = I/O 완료(성공/실패 무관, 커널이 끝냄). transferred 유효.
//   반환 false = 타임아웃 후 취소 승인까지 완료. transferred == 0.
// 타임아웃 경과 판정은 호출자가 deadline을 보고 수행한다.
// P2-1 stabilization fix (session 260925_0001, live `io drop` root cause):
// the previous timeout path IGNORED the cancel-approve result. When the I/O
// completed inside the cancel window (a frame landing exactly at the
// deadline), GetOverlappedResult returned TRUE, the helper returned false
// anyway, and — GetLastError having been RESET TO 0 by the successful
// GetOverlappedResult — ggml_asr_worker's "ERROR_OPERATION_ABORTED means
// timeout" classification saw 0 and mapped a healthy read to IoError, killing
// the worker on a live pipe (observed every ~1-2 min under streaming). Honor
// the completed I/O and leave ERROR_OPERATION_ABORTED untouched on the real
// cancel path so the caller classification is reliable.
inline bool WaitOverlappedOrCancelApprove(HANDLE pipe, OVERLAPPED& ol,
                                          long long timeout_ms, DWORD& transferred) {
    const DWORD wait = timeout_ms <= 0 ? 0u : static_cast<DWORD>(timeout_ms);
    const DWORD wr = ::WaitForSingleObject(ol.hEvent, wait);
    if (wr == WAIT_OBJECT_0) {
        return ::GetOverlappedResult(pipe, &ol, &transferred, FALSE) != FALSE;
    }
    // 타임아웃: 취소 요청 후 커널이 실제로 취소를 완료할 때까지 대기한다.
    // CancelIoEx 자체는 비동기 — 승인 없이 이벤트를 닫으면 UAF(위 파일 헤더).
    // 취소 승인 대기 결과를 반드시 존중한다: 완료 경주(I/O가 취소 창 안에서
    // 이미 끝난 경우)면 성공으로, 진짜 취소면 ERROR_OPERATION_ABORTED를 그대로
    // 둔 채 false를 돌려준다.
    ::CancelIoEx(pipe, &ol);
    DWORD completed = 0;
    const BOOL done = ::GetOverlappedResult(pipe, &ol, &completed, TRUE);
    if (done) {
        transferred = completed;
        return true;
    }
    transferred = 0;
    return false;
}

} // namespace engine_host
} // namespace emebalachat
