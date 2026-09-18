# Emebala 공용 추론 호스트 (Shared Inference Host) — 구현 계획서

> Cross-workspace handoff 문서. 이 문서 하나만으로 구현 가능해야 한다.
> 받는 AI는 반드시 §1(배경)·§4(프로토콜 계약)·§6(자기 프로젝트 범위)·§9(수용 기준)를
> 전부 읽고 시작하라. §4는 **동결 계약**이다 — 임의 변경 금지, 변경 필요 시
> 양쪽 워크스페이스에 명시적 통지가 선행되어야 한다.

- 상태: **설계 동결 (v1.0, 2026-09-17)** — M1 완료. §4 프로토콜 계약은 변경 없이 동결.
  §7은 아래 M1 결정 사항을 반영해 보완했음.
- v2: **v2.0-draft (2026-09-18, 미동결)** — 멀티 모델·멀티 백엔드 확장안(오케스트레이터+
  워커, 스트리밍 프로토콜, 모델 레지스트리+서명 매니페스트, 임베디드 즉시 제거)을
  같은 파일 하단에 둔다. v1.0 계약은 v2에서도 유효하며, v2 호스트는 v1 클라이언트를
  지원한다(§V2-4.7). **§V2-14의 6개 열린 질문은 전부 결정 완료(2026-09-18)** —
  남은 동결 전 작업: 세 워크스페이스 합의 리뷰(§V2-13) + 코드 서명 인증서·카탈로그
  호스팅 URL 확정(운영 결정).
- M1 결정 사항 (2026-09-17, Chat 워크스페이스):
  1. **범위**: Chat은 M2에 설치자 통합(§7.1 호스트 번들/버전 규칙, §7.2 모델 공용 경로,
     §7.3 제거 규칙)을 포함해 한 번에 진행한다.
  2. **모델 이관**: 기존 설치 경로의 모델은 이사시키지 않고, 설치 시 구경로 파일을
     삭제한 뒤 공용 경로 기준으로 핀 확인 → 필요 시 재다운로드한다.
  3. **SHA-256 검증**: 임베디드 폴드백으로 앱이 GGUF를 직접 로드하는 경로는 기존
     `.sha256ok` 캐시 검증을 그대로 유지한다 (§7.2 문구 보완).
  4. **출시 버전**: 0.10.1 — §4.4 `hello` 예시의 `client_version` 값과 동일.
- 작성: Emebala Chat 워크스페이스 (REQ-042 세션, `docs/260917_0002_session_req042-tail-unchanged-shortcircuit/`)
- 대상 프로젝트: `Emebalachat`, `Emebala_Listner`, `Emebala Reader`(예정), 향후 모든 Emebala 계열 앱

---

## 1. 배경과 목표

Emebala 계열 앱(Emebala Chat, Emebala_Listner, Emebala Reader, …)은 전부 동일한
로컬 번역 모델 **Tencent Hy-MT2-1.8B (Q8_0 GGUF, ~1.9GB)** 를 llama.cpp(tag b6099)로
로드해 쓴다. 지금은 **앱 프로세스마다 각자 모델을 VRAM에 올린다** → 앱 3개 실행이면
VRAM 약 3×(6~7GB). 이는 지양해야 한다.

**목표**: 모델을 GPU에 **1개만** 상주시키고, 모든 Emebala 앱이 **큐(FIFO 1슬롯)** 로
공유해 쓴다. 어떤 앱 조합(1개/2개/3개/향후 N개)이 설치돼도 동작해야 하고, 공용
컴포넌트가 없으면 각 앱은 **오늘처럼 독립 동작**해야 한다(하드 의존 금지).

비슷한 선례: Ollama, LM Studio 서버, llama.cpp `llama-server`. 본 계획은 그것의
최소 맞춤형(translation 전용, 프로토콜 단순화, 강한 로컬 보안).

## 2. 아키텍처 개요

```
[Emebala Chat]   ─┐
[Emebala_Listner] ─┼─ named pipe (로컬만, 유저 전용 ACL + 부팅별 토큰)
[Emebala Reader] ─┘        │            ── [Emebala.Engine.exe]
                           │                │  Hy-MT2 모델 상주 (VRAM 1×)
                    요청 FIFO 1슬롯 직렬화    │  추론 워커 1스레드
                           │                │  유휴 시 언로드/자동 종료
   클라우드(Google) 폴드백은 각 앱이 기존처럼 자체 처리 (호스트와 무관)
```

- **Emebala.Engine.exe** (이하 "호스트"): 번들 부품. 시작 메뉴/트레이에 나타나지
  않는 백그라운드 프로세스. 유저가 직접 실행/설치하는 단계는 없다(§7).
- **클이언트**: 각 앱이 링크하는 얇은 코드(§5.3). "서비스 우선, 폴드백은 기존
  동작(자체 임베디드 엔진 또는 CPU 폴드백)".
- 각 앱의 **번역 파이프라인(캡처/슬라이스/레저/붙여넣기)은 건드리지 않는다.**
  변경 지점은 엔진 호출 계층뿐이다.

## 3. 용어

| 용어 | 의미 |
|---|---|
| 호스트 | `Emebala.Engine.exe`. 모델을 소유하고 추론을 직렬 실행 |
| 클라이언트 | 앱 프로세스 내 호스트 연결 코드 |
| 토큰(보안) | 호스트 기동 시 생성되는 128bit 랜덤 값. 파일 경로의 토큰 파일 (§4.2) |
| 임베디드 폴드백 | 호스트를 못 쓸 때 앱이 자체 GGUF를 로드하는 오늘의 동작 |

## 4. 프로토콜 계약 (동결)

### 4.1 전송

- **Named pipe**: `\\.\pipe\emebala-engine-v1` (고정 이름).
- 서버(호스트) 생성 시 보안 기술자: **현재 유저 SID만 GA 권한** (`D:P(A;;GA;;;<owner-sid>)`),
  상속 금지 플래그 포함. 다른 유저/서비스는 연결 자체가 거부된다.
- 파이프 인스턴스: 최소 4개 (동시 접속 클라이언트 수). 메시지 모드
  (`PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE`),blocking은 아님(오버랩).
- 최대 프레임: **1 MiB** (번역 문장 규모 고려; 초과는 `bad_request`).

### 4.2 보안 토큰

- 호스트 기동 시 128bit 난수 hex(32문자) 생성 →
  `%LOCALAPPDATA%\Emebala\Common\engine\token` 에 **유저 전용 ACL**로 기록
  (기존 파일 있으면 교체). 프로세스 종료 시 파일 삭제.
- 클라이언트는 연결 직후 `hello`에 이 토큰을 실어 본다. 불일치/없음 →
  `error: unauthorized` + 즉시 연결 종료.
- 토큰은 호스트 **기동별**로 회전. 오래된 클라이언트는 스스로 재연결·재스폰 절차를 탄다(§5.4).

### 4.3 프레임 포맷

모든 메시지: `[u32 LE 길이][UTF-8 JSON]`. JSON 필드는 §4.4의 것만 사용
(확장 필드는 수신 측이 무시하되, 미확인 `op`는 `bad_request`).

### 4.4 메시지 정의 (protocol 버전 1)

**핸드셰이크 (연결 직후, 반드시 1회)**

클 → 호:
```json
{"op":"hello","protocol":1,"token":"<32hex>",
 "client":"emebala-chat","client_version":"0.10.1"}
```
호 → 클(성공):
```json
{"op":"welcome","protocol":1,
 "model":"Hy-MT2-1.8B-Q8_0",
 "model_sha256":"<64hex 고정 핀>",
 "capabilities":["translate"]}
```
- `protocol` 불일치, 토큰 불일치 → `{"op":"error","code":"unauthorized"|"version_mismatch"}` 후 close.
- 클라이언트는 `model_sha256`이 자기가 기대하는 핀과 다를 경우 서비스 사용을 포기하고 임베디드 폴드백으로 간다.

**번역 요청/응답**

클 → 호:
```json
{"op":"translate","id":<u64>,"src":"ko","tgt":"en",
 "text":"<원문>","timeout_ms":30000}
```
호 → 클(정상/오류 모두 id 매칭, 연결 순서 무관):
```json
{"op":"result","id":<u64>,"status":"ok","text":"<번역문>"}
```
`status` 열거: `ok` | `engine_failed` | `model_missing` | `timeout` | `bad_request` | `busy`.

- `busy`: 전역 대기열 깊이 > 8 이면 즉시 반환(큐에 넣지 않음). 클라이언트는 폴드백.
- `timeout`: 요청 `timeout_ms` 내 완료 불가. 호스트는 해당 추론을 중단하고 다음 작업 진행.
- v1은 **스트리밍 없음**(one-shot). 스트리밍은 capabilities 협상으로 v2 이후.

**취소**

클 → 호: `{"op":"cancel","id":<u64>}`
- 현재 디코딩 중인 작업이면 llama.cpp abort 콜백(`llama_set_abort_callback`, b6099 제공)으로 중단, `result status=timeout` 응답.
- 아니면 무시.

**유휴 종료** (서버 내규칙, 메시지 없음)

- 연결 수 0 이고 마지막 활동으로부터 `idle_exit_ms`(기본 600_000=10분) 경과 →
  프로세스 자체 종료. 다음 클라이언트 요청이 오면 재스폰(§5.4).
- 옵션: `unload_model_first`(기본 true) — 종료 전 모델만 먼저 언로드해 VRAM 즉시 반납.

### 4.5 직렬화/큐 규칙 (호스트 내)

- 추론은 **전역 FIFO 1슬롯**(배치 1). 요청은 접수 순서대로, 완료 후 다음.
- per-connection 순서 보존은 `id` 매칭으로 보장(응답 순서는 완료 순).
- 동일 모델에 대해 컨텍스트 1개 유지(지연 재사용).

## 5. 구현 범위 (프로젝트별)

### 5.1 Emebala Chat 워크스페이스 (`D:\OneDrive\Projects\Emebalachat`) — 호스트 + 레퍼런스 클라이언트

1. **`Emebala.Engine.exe` 신규 타겟** (CMake): Win32 GUI 서브시스템 콘솔 없음.
   - 파이프 서버 스레드 + 단일 추론 워커 스레드(`single_slot_worker.hpp` 패턴 재사용).
   - 기존 `src/engine.cpp`의 번역 전용 로직 이주: 프롬프트 조립, 제어토큰 스크러빙
     (`CollectControlTokenTexts`/`ScrubControlTokenTexts`, SEC-B2),
     `TruncateHeadTailWindow`, `NormalizeNFC`, `TranslationStatus` 매핑.
   - SHA-256 핀 검증(`VerifyModelSha256`, kExpectedModelSha256)은 호스트가 담당.
   - 모델 경로: `%LOCALAPPDATA%\Emebala\Common\models\Hy-MT2-1.8B-Q8_0.gguf` (§7.2).
   - 로깅: shape-only 원칙(기존 diag_logger 재사용, 기본 OFF).
2. **`src/engine.cpp` 라우팅 변경**: 로컬 엔진 다리를 "호스트 우선"으로.
   - `Translate()` 진입 시 `EngineHostClient::TryTranslate(...)` → 성공 시 반환.
   - 실패/불가 시 **기존 임베디드 경로 그대로**(CUDA→CPU 폴드백 포함).
   - config.json 신규(기본값): `"engine_host": {"enabled": true, "spawn": true, "idle_exit_ms": 600000}`.
3. **레퍼런스 클라이언트 `src/engine_host_client.hpp/.cpp`** — 다른 워크스페이스가
   그대로 복사해 쓸 수 있도록 **의존 최소**(Win32 + 표준라이브러리만, JSON은
   이스케이프 유틸 + 최소 파서; 프로젝트에 JSON 라이브러리 없음).
4. **spawn 온 디맨드**: 연결 실패 + `spawn:true` + 호스트 바이너리 존재 시
   `CreateProcess`(숨김, 동일 유저) → 500ms 내 재연결 재시도(최대 3회).
   단일 인스턴스: 호스트 자체가 이름 있는 뮤텍스로 중복 기동 방지.
   기동/탐색 경로는 §7.1의 고정 경로 단일.
5. **버전 범프 0.10.1**: `CMakeLists.txt` `project(... VERSION 0.10.1)` —
   `version.hpp`는 비자동(문자열 그대로). 설치자/README는 M2 설치자 통합 때 함께
   맞춘다 (M1 결정 #4).

금지: `worker.cpp`의 REQ-041/REQ-042 파이프라인, 훅, 클립보드 계약 변경 금지.

### 5.2 Emebala_Listner / Emebala Reader 워크스페이스 — 클라이언트 통합

1. 이 문서 §4(프로토콜)를 구현 근거로, **`engine_host_client`를 자기 코드베이스에
   이식**(5.1-3의 레퍼런스가 Chat 쪽에 먼저 들어가 있으면 그 소스를 복사;
   아직 없으면 본 문서 §4만으로 독립 구현).
2. 앱의 번역 엔진 호출 지점에 "호스트 우선, 폴드백은 기존 로컬/클우드" 라우팅 추가.
3. config에 동일 `engine_host` 블록 추가.
4. 호스트 바이너리를 **자기 설치자에도 번들**(§7.1 규칙대로 중복 설치 방지).

### 5.3 클라이언트 공통 동작 (모든 앱)

- 연결 → hello → welcome 핀 확인 → translate → result → (유휴 시 연결 유지 또는
  재연결은 구현 자유).
- 오류/시간초과/버전 불일치/토큰 불일치 모두 **예외 없이 폴드백**으로 수렴.
  폴드백은 "기존에 이 앱이 번역 실패 시 하던 것"(자체 임베디드 또는 동의된 클라우드).
- 클라우드 전송 동의(프라이버시 노티스)는 **클이언트가 호스트와 무관하게 기존 정책
  그대로** 관리한다.

### 5.4 폴드백 매트릭스 (모든 클라이언트가 만족해야 함)

| 상황 | 동작 |
|---|---|
| 호스트 바이너리 없음 | 임베디드 폴드백 (조용히) |
| 파이프 연결 거부/타임아웃 | spawn 시도(설정 시) → 재시도 3회 → 임베디드 |
| 토큰/버전/핀 불일치 | 폴드백 + shape-only 로그 1줄(사용자 텍스트 금지) |
| `busy` | 임베디드 폴드백 (큐 대기 금지 — UX 일관성) |
| 요청 중 호스트 종료/다운 | `timeout_ms` 내 감지 → 임베디드 폴드백, 다음 요청부터 재스폰 |
| 임베디드도 불가(모델 미설치 등) | 기존 앱의 기존 실패 UX 그대로 |

## 6. 보안 요구사항

1. 파이프 ACL = 생성 유저 단독(§4.1). 상속 끊기.
2. 토큰 파일 ACL = 유저 단독, 호스트 종료 시 삭제(§4.2).
3. 루프백 TCP/HTTP **금지** (로컬 pipe만).
4. 로그 shape-only(길이/상태/지연만, 원문·번역문 금지, 기본 OFF).
5. 클라우드 동의 게이트는 클라이언트에 그대로(호스트가 동의를 우회할 수 없음).

## 7. 설치/배포 규약

### 7.1 설치자 (Inno Setup) — 모든 앱 공통 규칙

- 각 앱 설치자는 `Emebala.Engine.exe`를 **같은 버전 규칙**으로 번들.
- 설치 시: `%LOCALAPPDATA%\Emebala\Common\engine\engine.version` 확인.
  - 없거나 **번들 버전이 더 높으면** 호스트 설치/교체. 같거나 낮으면 스킵.
- `engine.version` 내용: 번들된 호스트 버전 = 해당 앱의 AppVersion(예: `0.10.1`).
- **호스트 설치 경로 (고정, 2026-09-17 M1 보완)**:
  `%LOCALAPPDATA%\Emebala\Common\engine\Emebala.Engine.exe`. 클라이언트의
  spawn/존재 확인도 이 경로 단일.
- 유저에게 노출되는 설치 단계/선택지 추가 없음.

### 7.2 모델 파일

- 경로: `%LOCALAPPDATA%\Emebala\Common\models\Hy-MT2-1.8B-Q8_0.gguf` (중립 공통 경로).
- 설치 시 기존 파일 SHA-256 핀 일치 확인 → 일치 시 다운로드 스킵.
- 다운로드 중 쓰기 충돌 방지: `<파일>.download` 임시 + 완료 후 원자 이동.
- 호스트 경유 시 `welcome`의 `model_sha256` 핀이 유일한 검증(앱별 `.sha256ok`
  계약은 호스트 전용으로 이관). 단, **임베디드 폴드백으로 앱이 GGUF를 직접 로드하는
  경로는 기존 `.sha256ok` 캐시 검증을 그대로 유지**한다 (M1 결정 #3).
- **구경로 모델 이관 규칙 (M1 결정 #2)**: 기존 설치 경로(예: Chat의
  `<설치폴터>\models\`)의 모델은 공용 경로로 이사시키지 않는다. 설치 시 구경로
  파일은 삭제하고, 공용 경로 기준으로 핀 확인 → 필요 시 다운로드를 진행한다.

### 7.3 제거

- 각 앱 제거는 자기 것만. **마지막 앱 제거 시**에만 공용 엔진/모델 정리 (M1 결정).
  - 판정: 시스템의 제거 레지스트리(Uninstall 키)에서 다른 Emebala 계열 앱
    (Emebala_Listner, Emebala Reader, …)의 설치 유무를 확인. 없으면
    `%LOCALAPPDATA%\Emebala\Common\`의 엔진 디렉터리와 모델을 함께 삭제, 있으면 유지.
  - 계열 앱이 새로 추가될 때마다 판정 목록에 추가한다.

## 8. 실패 모드/리스크 목록

| 리스크 | 대책 |
|---|---|
| 호스트 hang(GPU 드라이버 이슈) | 클라이언트 `timeout_ms`(기본 30s) → 폴드백 |
| 동시 추론 경합 | 호스트 FIFO로 직렬화 — 경합 자체가 소멸 |
| VRAM 부족(다른 앱이 VRAM 점유) | 호스트 로드 실패 시 `model_missing`/CUDA→CPU 폴드백(기존 규칙) |
| 프로토콜 버전 불일치(앱-호스트 갱신 차이) | handshake `version`/`model_sha256`으로 사전 거부 + 폴드백 |
| 오래된 토큰 파일 | 기동별 회전 — 거부 시 클라이언트 재스폰 절차 |
| OneDrive Files-On-Demand(모델 placeholder) | 첫 로드 시 hydration 지연 허용; 설치 후 "항상 유지" 권장 안내 |
| 32bit 클라? | 지원 대상 아님(본 프로젝트 계열은 x64 전용) |

## 9. 수용 기준 (정의대로만 완료로 친다)

### 9.1 호스트+Chat (Chat 워크스페이스)

1. Chat + 호스트만 실행: VRAM 사용이 모델 1×분(태스크매니저/nvidia-smi로 확인,
   Q8_0 기준 약 2~2.5GB 이하).
2. Chat 기존 단위 테스트 전부 통과(현 2,891 checks), 신규 호스트/클로전트 테스트
   추가(프로토콜 골든 케이스 + 폴드백 매트릭스).
3. 호스트를 `taskkill`로 죽인 채 번역 요청 → `timeout_ms` 내 임베디드 폴드백.
4. 토큰 파일을 훼손 → 연결 거부 + 폴드백(충돌 없이).
5. diag 로그에 사용자 텍스트 없음(shape-only).

### 9.2 클라이언트 앱 (Listener/Reader 워크스페이스)

1. 호스트 있는 환경: Chat과 동시 실행핏 VRAM 증가 없음(1× 유지).
2. 호스트 없는 환경(바이너리 삭제): 앱이 기존 방식으로 정상 동작.
3. §4.4 메시지를 **바이트 호환**으로 주고받음(Chat 레퍼런스와 상호운용 테스트).

### 9.3 통합(마지막)

- 앱 2개(예: Chat+Listener) 동시 번역 요청 → 큐 직렬 처리, 둘 다 정답 반환.
- 설치 순서 조합(먼저 Listener만 → Chat 추가 등) 전부 §7 규약대로 동작.

## 10. 마일스톤 (권장 순서)

1. **M1 — 계획 동결**: 본 문서 리뷰/수정 완료 (각 워크스페이스가 같은 §4로 시작).
   ✅ 2026-09-17 v1.0 동결 (상단 M1 결정 사항 4건 반영, §4 프로토콜 자체는 변경 없음).
2. **M2 — Chat 쪽**: 호스트 + 레퍼런스 클라이언트 + Chat 라우팅 + **Chat 설치자 통합**
   (§7.1 호스트 번들/버전 규칙, §7.2 모델 공용 경로+구경로 정리, §7.3 제거 규칙) — 9.1 통과.
3. **M3 — Listener 쪽**: 클라이언트 이식/구현 + 라우팅 (9.2 통과).
4. **M4 — Reader 쪽**: 동일 (9.2).
5. **M5 — 나머지 앱 설치 통합**: Listener/Reader 설치자에 §7 규약 적용 + 조합 설치
   테스트 + 통합 QA(9.3). (Chat 쪽 설치자 통합은 M1 결정 #1에 따라 M2에 이미 포함)

병렬 주의: M3/M4는 §4만 확정되면 Chat 쪽 결과물(레퍼런스 클라이언트 소스) 없이도
착수 가능하나, **상호운용(9.2-3)은 M2 산출물과의 통합 테스트로만 확정**된다.

---

## 부록 A. Emebala Chat 측 파일 포인터(구현 시 출발점)

- 엔진/로컬 로딩: `src/engine.cpp` (`llama_model_load_from_file` ~L758/774,
  SHA 핀 ~L442-530, 프롬프트/스크러빙/트렁케이션 동일 파일)
- 설정 스냅샷/신규 `engine_host` 블록: `src/config.hpp`(AppConfig) + `src/config.cpp`
- 단일 슬롯 워커 패턴: `src/single_slot_worker.hpp`
- JSON 유니코드 이스케이프(클 출력용): `src/unicode_utils.cpp AppendJsonUnicodeEscape`
- SHA-256: `src/engine.cpp ComputeFileSha256` (호스트 이주 대상)
- 진단 로그: `src/diag_logger.hpp`(shape-only 규율)

## 부록 B. 문서를 받은 AI에게

- 이 문서만 보고 구현하라. 이전 대화 맥락은 없다고 가정된다.
- §4는 수정하지 말고 그대로 구현하라. 모순/누락 발견 시 구현을 멈추고 사용자에게
  보고하라(임의 해석 금지).
- 자기 워크스페이스의 §5 범위만 담당하라. 다른 프로젝트 파일을 수정하지 마라
  (단, Chat 레퍼런스 클라이언트 **읽기**는 허용).
- 완료 판정은 §9다. "테스트 통과"만으로 끝내지 말고 §9.3 통합 기준까지 남겨두면
  그 사실을 보고서에 명시하라.


---

# v2.0-draft — 멀티 모델·멀티 백엔드 공용 추론 호스트

> **상태: draft (미동결).** v1.0과 달리 이 섹션은 확정되지 않았다. 동결 대상 범위와
> 절차는 §V2-13을 본다. v1.0 계약(§4 포함)은 v2에서 **그대로 유효**하며, v2 호스트는
> v1 클라이언트를 반드시 지원한다(§V2-4.7).
>
> 근거: 2026-09-18 세 워크스페이스 조사(Chat/Listener/Reader) + v1.0 운용 경험
> (설치 후 라우팅 게이트 결함 발견·수정 포함 — v1 호스트가 "인식만 되면" 정상 작동
> 확인됨).

## V2-1. 배경과 목표 변화

v1.0의 목표(번역 모델 1개를 GPU에 1개만 상주, FIFO 공유)는 달성했다. v2가 필요해진
계기:

1. **Reader는 모델이 "고정 1개"가 아니다.** Flutter+Rust 앱으로, 이미 GGUF(llama-cpp-2
   0.1.150) / CTranslate2(ct2rs 0.9) / ONNX Runtime(ort 2.0 + DirectML) 세 백엔드를
   인-프로세스에서 운용 중이다. 카탈로그(`models_manifest.json`, 9종) 기반 다운로드와
   임의 로컬 .gguf 지정을 함께 지원한다("뭐든 받아서 쓴다"는 요구는 이미 구현됨).
2. **Listener는 스트리밍 ASR이 필수다.** Nemotron-3.5-ASR-Streaming-0.6B Q8_0을
   transcribe.cpp(ggml v0.20.2, llama.cpp와 계보가 다른 포크)로 돌리며, 부분 결과
  (onPartial) + ASR/MT 동시 상주 + "오래된 세그먼트 폐기" 백프레셔가 파이프라인
   계약이다. 지연 예산: 청크 <100ms, E2E <2s. (현재 CPU 전용 빌드, GPU는 백로그.)
3. **세 앱의 모델 저장 경로가 3곳으로 분산**되어 있다
   (`%LOCALAPPDATA%\Emebalachat\models`, `...\EmebalaListener\models`,
   `...\Emebala\models`). 다운로드 생태계도 각자 구현 중이다.
4. **제품 방향이 결정됐다**: 앱에서 추론 엔진(llama.cpp 등)을 제거하고, Emebala
   Engine을 필수 구성요소로 동행 설치·구동한다(v1 §1의 "하드 의존 금지" 개정).
5. Reader 워크스페이스의 기존 ADR(공유 추론 서버 **연기**)에 적혀 있던 재논의
   트리거 — "2개 앱 동시 실행 VRAM 문제, 3번째 앱 등장" — 이 **이미 충족**됐다.

**v2 목표**: 어떤 앱 조합으로 설치핵어도 동작 / 모델 교체·추가·백엔드 추가가
**데이터 변경**으로 끝남 / 엔진이 초보 사용자에게 보이지 않음.

## V2-2. 워크스페이스 현황 (2026-09-18 조사 기준)

| 앱 | 스택 | 추론 백엔드 | 주요 모델 | 모델 경로 | 특이사항 |
|---|---|---|---|---|---|
| Emebala Chat | C++20/Win32 | llama.cpp b6099 | Hy-MT2-1.8B-Q8_0 (greedy 0.0) | `Emebalachat\models` → Common 이주 중 | v1 호스트 운용 중, Inno 설치기 |
| Emebala Listener | C++20/Win32 | llama.cpp 74ce157(+PR22836), transcribe.cpp | Nemotron-3.5-ASR-0.6B-Q8_0 + Hy-MT2 (temp 0.7) | `EmebalaListener\models` | 스트리밍 ASR 필수, ASR+MT 동시상주, CPU-only 현재 |
| Emebala Reader | Flutter + Rust | llama-cpp-2, ct2rs, ort+DirectML | Hy-MT2 Q8_0, m2m100-418M-CT2-int8, Kokoro-82M-ONNX, PP-OCRv6 (+카탈로그) | `Emebala\models` | LLM↔CT2 상호배타 상태머신+가버너, 설치기 없음, OpenAI 호환 클라우드 별도 |

핵심 표시: 번역 sampling 계약이 앱마다 다르다(Chat greedy 0.0 / Listener 0.7) —
→ §V2-4.4의 per-request 프로파일로 해소.

## V2-3. 아키텍처 — 오케스트레이터 + 백엔드 패밀리별 워커

- **오케스트레이터** (`Emebala.Engine.exe`, v1 호스트를 겸함): named pipe 서버,
  유저 전용 ACL, 부팅별 토큰, **세션 관리, 스케줄링, 워커 생명주기**만 담당.
  백엔드(모델·엔진 라이브러리)에 대한 지식이 없다 — **바보 라우터 원칙**(§V2-12-1).
- **워커**: 오케스트레이터가 필요 시 띄우는 자식 프로세스. 시작 시 4종:
  | 워커 | 엔진(초기 핀) | 담당 |
  |---|---|---|
  | `Emebala.Engine.ggml-translate.exe` | llama.cpp (Chat 튠 기준 b6099) | GGUF 번역·생성 |
  | `Emebala.Engine.ggml-asr.exe` | transcribe.cpp (ggml v0.20.2 계열) | 스트리밍 ASR |
  | `Emebala.Engine.ct2.exe` | CTranslate2 (ct2rs 0.9 계열) | CT2 번역 |
  | `Emebala.Engine.onnx.exe` | ONNX Runtime 1.24 + DirectML | TTS·OCR 등 ONNX |
- **디스패치 규약**: 레지스트리 항목의 `family` 문자열 → 이름 규약
  `Emebala.Engine.<family>.exe` → 해당 워커의 매니페스트 확인 → 스폰/라우팅.
  **백엔드 추가 = 워커 exe 1개 + 매니페스트 + 레지스트리 데이터 추가. 오케스트레이터
  코드 무수정.**
- **워커 매니페페스트** (`worker.manifest`, exe 옆 또는 납품 시 고정):
  `{family, engine, engine_version, ops[], abi_version, protocol_min, protocol_max}`
- **오케스트레이터↔워커 내장 계약 (제2 동결 계약)**: 프라이빗 파이프(또는 stdin/out),
  프레임 형식은 §4.3 규약 재사용([u32 LE][JSON]). 오케스트레이터가 백엔드와 나누는
  말은 이 계약뿐이다. 워커 내의 엔진 라이브러리 교체는 이 계약을 깨지 않는 한
  오케스트레이터·앱 무관.
- **장애 격리**: 워커 크래시 → 해당 패밀리만 `unavailable` 보고 → 해당 세션 정리 →
  자동 재스폰. 다른 패밀리와 다른 세션은 무영향.
- **병합 규칙**: 계열 호환성이 생기면 워커 통합을 허용한다(예: ASR이 llama 계열로
  수렴하면 ggml-asr을 ggml-translate로 흡수). 통합도 데이터 변경으로 끝낸다.

## V2-4. 프로토콜 v2 (클리언트 ↔ 오케스트레이터)

### V2-4.1 프레임과 전송
파이프 이름은 **버전 무관 `\\.\pipe\emebala-engine`** (2026-09-18 결정: 이름에 버전을
박지 않는다). 버전은 `hello.protocol`으로만 협상(§V2-4.7). **전환기 호환**: 이미 배포된
v1 클라이언트(0.10.1)가 하드코딩한 `\\.\pipe\emebala-engine-v1`을 v2 오케스트레이터가
**별명으로 동시에** 생성해 구 앱도 서비스한다. 프레임 `[u32 LE 길이][UTF-8 JSON]`,
1 MiB 상한은 **제어 메시지용**으로 유지, ACL/토큰/파이프 인스턴스 규칙은 v1 §4.1과
동일.

**클리언트 연결 전략 (v2 구현 표준)**: v2 클라이언트는 먼저 `\\.\pipe\emebala-engine`
에 연결을 시도하고, 거절(오래된 v1 호스트만 있는 환경) 시
`\\.\pipe\emebala-engine-v1`로 한 번 더 시도한다. 둘 다 안 되면 §5.4 절차(스폰 →
재시도)로 수렴. 참고: v1 §4.1의 "고정 이름 `emebala-engine-v1`" 표기는 **v1
프로파일에 그대로 유효**한 동결 계약이며, v2의 버전 무관 이름은 프로토콜 2 클라이언트와
신규 오케스트레이터 간 규약이다 — 둘은 모순이 아니라 공존 별명 관계다.

### V2-4.2 대용량 페이로드 규칙 (신규)
오디오·이미지·문서 등은 바이트 실어보내지 않고 **로컬 경로 참조**로 전달한다
(같은 머신·같은 유저 전제, §6 정신 유지). `{... "audio_path": "C:\\..." }`.
대상 파일이 없거나 접근 불가 → `bad_request`. (확장 필드는 수신 측이 무시 — §4.3.)

### V2-4.3 세션 모델 (신규)
스트리밍 op는 세션 개념을 도입한다:
`session_open`(capability, 모델/프로파일, 옵션) → 양방향 프레임(클 → feed /
호 → event) → `session_close` / `cancel`. 이벤트 프레임:
`{"op":"event","session":N,"kind":"partial|final|token|error|eos","seq":M,...}`

### V2-4.4 op 목록 v2
| op | 방식 | 비고 |
|---|---|---|
| `translate` | 요청→응답 | v1 호환. `timeout_ms`(v1 §4.4 계약 그대로: 기본 30000, 클램프 1000~600000) 필수 유지. 선택 필드: `model`, `profile`(레지스트리 참조), `sampling`(temp/top_p/top_k/rep_pen) |
| `asr` | 세션 | open(모델, 언어힌트, partial on/off) → `feed_audio` → partial/final 이벤트 → close. **피드 규격 (확정 2026-09-18)**: 라이브 청크는 JSON 메타 프레임(`{session,seq,format:"pcm_s16le_16k_mono"}`) 직후 **바이너리 PCM 프레임**(16kHz mono s16le, 청크 ≤ 64 KiB)을 같은 파이프로 전송. 바이너리 프레임에도 v1의 프레임 상한(1 MiB)이 적용되며, PCM 청크는 반드시 64 KiB 이하로 분할한다. 녹음 파일 등 배치 입력은 로컬 경로 참조 |
| `tts` | 요청→응답 | 텍스트+음성+속도 → 합성 결과를 **WAV 파일(24kHz 16-bit PCM)으로 기록 → 경로 반환** (확정 2026-09-18, 세션 디렉터리에 기록). 재생·삭제는 앱 책임 |
| `generate` | 세션 | 커스텀 LLM. 토큰 이벤트 스트리밍, sampling 파라미터, 챗템플릿 적용 여부는 프로파일 |
| `register_model` | 요청→응답 | `origin:user` 모델 등록(파일 경로, 계열, 추정 capability). 해시 계산·경고는 앱 책임 |
| `list_models` | 요청→응답 | 레지스트리 열거(사용자 텍스트 무관) |
| `unload_model` | 요청→응답 | 상주 해제 힌트(스케줄러가 최종 결정) |
| `cancel` | — | 세션/요청 취소(v1 계약 유지) |

### V2-4.5 status 열거 v2
v1 6종(`ok|engine_failed|model_missing|timeout|bad_request|busy`) 유지 + 신규:
- `unavailable` — 해당 capability/모델 미배포, 또는 해당 패밀리 워커 장애.
미확인 status는 클라이언트가 **실패로 수렴**(§4.3 규칙의 보편화).

### V2-4.6 스케줄링 계약
요청/세션은 `priority`(0–9), `drop_eligible`(백프레셔 허용 여부), `deadline_ms`를
담을 수 있다. 오케스트레이터 규칙:
- ASR 등 장기 세션은 "예약"으로, 번역·생성 등 단발 요청과 공존.
- 자막류(drop_eligible=true)는 큐 포화 시 **가장 오래된 것부터 폐기**(Listener 계약).
- v1의 "busy 즉시 거부"는 drop_eligible=true에만 적용. 일반 요청은 유한 큐 대기
  허용(깊이 상한 초과 시 `busy`. 초기 상한은 v1 §4.4의 8을 계승, 레지스트리
  프로파일로 조정 가능 — §V2-12-4).
- 워커 패밀리 내에서 동시 컨텍스트 수(세션 수)는 레지스트리 리소스 프로파일이
  제한(§V2-5.2).

### V2-4.7 하위 호환과 v1↔v2 관계 (필독 — 구현자의 모순 판별 기준)

- `hello.protocol` 1 → **v1 프로파일**: translate only, v1 §4.4 전체를 **그대로**
  적용(busy 즉시거부, timeout, cancel 등). v1 프로파일의 동작은 v1.0 동결 계약을
  수정 없이 수행한다.
- `hello.protocol` 2 → **v2 프로파일**: §V2-4 전체(세션/스트리밍/스케줄링/신규 op).
- 두 프로파일은 **같은 파이프, 같은 hello**로 협상되며, v2의 새 규칙은 프로토콜 2를
  택한 클라이언트에만 적용된다. **v1 §4와 v2 §V2-4가 다르다고 모순이 아니다 —
  프로파일별로 다른 계약이다.**
- **v2 오케스트레이터는 v1 클라이언트를 반드시 서비스한다.** 프로토콜 불일치 처리는
  v1 규칙(`version_mismatch`) 유지.

**v1 핀 이음새 (한계의 기록 — 양쪽 계약 모두 올바르게 동작한다)**: v1 클라이언트는
welcome의 `model_sha256`을 **자기 exe에 컴파일된 핀**과 비교해 불일치면 서비스
포기(§4.4, 동결)다. 따라서 번들 번역 모델이 교첷된 환경에서 v1 프로파일 클라이언트는
폴드백으로 수렴한다. **"모델 교체가 앱 재배포 없이 전 앱에 적용"은 v2 프로파일
클리언트(서명 매니페스트 검증, §V2-6)에 적용되는 보장**이며, v1 프로파일의 컴파일
핀 규칙 자체를 바꾸지는 않는다. 실무적 귀결: 0.10.1 세대(v1 프로파일) 설치 기반은
매우 작으므로, 모델 교체는 "v2 클라이언트를 실은 앱 업데이트"와 함께 진행한다.

**v2 welcome 예시** (필드는 §V2-12-2에 따라 수신 측이 모르는 필드를 무시):
```json
{"op":"welcome","protocol":2,
 "models":[{"id":"hy-mt2-1.8b-q8","family":"ggml-translate",
            "capabilities":["translate"],"default":true}],
 "capabilities":["translate","asr","tts"],
 "health":{"session_success_ratio":0.997,"fallback_ratio":0.004}}
```

## V2-5. 모델 레지스트리 (registry)

### V2-5.1 위치와 소유
`%LOCALAPPDATA%\Emebala\Common\models\registry.json`. 작성: 설치기(번들)와
앱(사용자 모델). 읽기: 오케스트레이터(워커에 프로파일 전달). `schema_version`
필수(§V2-12-2).

### V2-5.2 항목 스키마
```json
{
  "schema_version": 1,
  "models": [{
    "id": "hy-mt2-1.8b-q8",
    "family": "ggml-translate",
    "files": ["...\\Hy-MT2-1.8B-Q8_0.gguf"],
    "capabilities": ["translate"],
    "origin": "bundled",
    "resource": {"vram_mb": 2400, "ctx": 4096, "max_sessions": 4,
                 "residency": "preload", "eviction": "sticky", "priority": 10},
    "profiles": {"default": {"temperature": 0.0, "top_p": 0.6, "top_k": 20,
                             "rep_pen": 1.05, "prompt_template_ref": "hymt2-official"}},
    "lang_pairs": ["*"]
  }]
}
```
- `residency`: `preload`(부팅 시 상주) / `ondemand`(요청 시 로드).
- 사용자 모델: `origin:"user"`, 해시는 최초 등록 시 계산해 앱이 기록. `resource`는
  추정치 허용(측정 후 교정).

**번역 프로파일의 소유 (2026-09-18 결정)**: `ggml-translate`의 `default` 프로파일은
**Chat 워크스페이스가 소유·관리**한다 — Chat의 번역(공식 SFT 템플릿 + greedy 0.0)이
패밀리 전체의 품질 기준. 변경 시 Chat의 번역 품질 게이트(§V2-10) 통과가 필수.
Listener(temp 0.7 등)와 Reader의 번역 설정은 전부 이 기준으로 수렴한다. 사용자의
앱 설정은 요청 파라미터로 오버라이드 가능(프로파일은 기본값일 뿐).

### V2-5.3 기존 경로 흡수 마이그레이션
세 앱의 기존 모델 파일(`Emebalachat\models`, `EmebalaListener\models`,
`Emebala\models`)은 **재다운로드 없이** 레지스트리 등록으로 흡수한다. 같은 모델이
여러 곳에 있으면 해시 비교 후 1개만 남기고 통합(이동/하드링크). 루트 소유권:
`%LOCALAPPDATA%\Emebala`는 Reader 앱 데이터, `Emebala\Common`은 **가족 공용**
(엔진 설치기가 관리) — 경계를 문서화한다.

### V2-5.4 사용자 모델의 제거 보호
`origin:"user"` 항목은 앱 제거·마지막 정리 대상에서 **제외**(사용자 데이터 보호).

## V2-6. 서명 매니페스트 (v1 핀의 일반화)

**문제**: v1의 `model_sha256` 핀은 클라이언트 exe에 컴파일된 리터럴이라, 새 모델로
바꾸면 옛날 앱이 거부한다 → "모델 교체가 전 앱에 자동 적용" 불가.

**해결**: `%LOCALAPPDATA%\Emebala\Common\models\manifest.json`:
`{"schema_version":1, "pinned": {"<파일명>": "<sha256>", ...}}` + **Authenticode
서명**(설치기가 서명). 검증 규칙:
1. 매니페스트 서명이 유효하고 Emebala 인증서인가(WinVerifyTrust).
2. 모델 파일 해시 == 매니페스트 기재 해시.
→ 둘 다 만족하면 어떤 버전의 앱이든 수용. 서명 매니페스트에 없는 파일은 번들
모델로서 **거부**(v1의 무결성 보장 수준 유지). `origin:user` 모델은 이 매니페스트
대상이 아니며 앱 책임 하에 동작.

**효과**: "Reader 업데이트로 번역모델이 최신으로 교첷돼도 Chat/Listener가 그대로
수혜" — 서명만 진짜면 앱 재배포가 필요 없다.

## V2-7. 카탈로그와 다운로드 (컴마友 UX 원칙)

- **카탈로그**(Emebala가 호스팅, 앱이 읽기 전용): 카드 = 표시명(사용자 언어) /
  capability / 언어쌍 / 용량 / 다운로드 URL+sha256 / `default` 여부.
- **초보자 경로**: 카드 클릭 1회 → 다운로드·해시검증·등록·적용 완료. 진행률·취소·
  부분파일 정리 UX는 Reader의 구현(`models_manifest.json` + download.rs)을 표준으로
  삼는다.
- **고급자 경로**: 로컬 파일 선택(.gguf/.onnx/CT2 디렉토리) → `register_model`로
  호스트가 포맷 판별·capability 추정·등록.
- **기본값 규칙**: capability별 `default` 모델이 카탈로그에 지정된다. 사용자가 아무것도
  고르지 않으면 default가 깔리고 쓰인다(결정 피로 제거).
- **엔진 무가시 원칙**: 설치·실행·업데이트·제거 전부 무음. 오류는 앱이 사용자 언어로
  "한 클릭 수리"로 번역한다. 사용자는 "엔진"이라는 단어를 볼 필요가 없다.

## V2-8. 설치/배포/제거 규약 v2

### V2-8.1 컴포넌트와 버전
컴포넌트 = 오케스트레이터 + 각 워커 + 번들 모델. 각각 버전을 가지며,
`%LOCALAPPDATA%\Emebala\Common\engine\components.json`에 기록(v1 §7.1
`engine.version` 규칙의 일반화). **앱 설치/업데이트가 유일한 업데이트 이벤트**다 —
엔진 스스로 인터넷에 업데이트 확인을 하지 않는다(프라이버시 브랜드 원칙, §6 정신).

**설치 판정 규칙 v2 (2026-09-18 사용자 결정 A)**: 설치기의 교체 판정은
components.json 우선으로 전환한다 —
1. `components.json` **부재** = 구세대(v1 §7.1 `engine.version` 만 존재) 설치
   상태 → **무조건 교체**(버전 문자열 비교 생략). 이는 v1 §7.1의 "같으면 스킵"이
   버전 고정(0.10.1) 환경에서 v1 호스트를 영구 잔존시키는 구멍을 막는다.
2. `components.json` **존재** → 컴포넌트별 `abi_version`/`version` 필드로 비교해
   번들이 더 높으면 교체, 같거나 낮으면 스킵.
v1 §7.1의 `engine.version` 기록/비교 규칙 자체는 동결 계약으로 그대로 유효하며
(v1 클라이언트/구 설치기 호환), v2 설치기는 위 규칙으로 components.json을 추가
기록한다. 판정 데이터는 파일(코드 무수정) — §V2-12-4 정신.

### V2-8.2 부분 설치
각 셋업은 자기 모델에 필요한 컴포넌트만 번들한다:
- Chat: 오케스트레이터 + ggml-translate + 번역 모델.
- Listener: + ggml-asr + ASR 모델.
- Reader: + ct2, onnx + 각 모델(카탈로그 default).
미설치 capability 요청 → `unavailable` → 앱은 기능 비활성/클우드(동의)/안내.

### V2-8.3 수리 부트스트래퍼
앱 기동 시 필수 컴포넌트(오케스트레이터/워커/모델) 부재를 검사 → 설치기와 동일한
버전 매니페스트 URL로 **무음 복구 다운로드**. 오프라인이면 해당 기능 비활성 + 사용자
안내. (DirectX 웹 설치기·WebView2 부트스트래퍼의 패턴.)

### V2-8.4 롤백
각 컴포넌트는 직전 버전 1개를 함께 유지(`.prev`). 문제 발견 시 수리 채널이 이전
버전으로 내린다. 게이트 결과(§V2-10)를 컴포넌트 버전과 함께 기록.

### V2-8.5 제거
v1 §7.3을 그대로 유지: 각 앱은 자기 것만 제거, **마지막 계열 앱만** 공용 엔진/
번들 모델을 정리(제거 레지스트리에서 계열 앱 검색). `origin:user` 모델은 정리 대상
제외.

### V2-8.6 임베디드 제거(엔진 필수화) — 즉시 제거 (2026-09-18 재결정)

배포 기반이 초기 단계(Chat v0.10.1 공개 직후, Reader·Listener 미공개)이므로
2단계 관찰 절차는 **폐기**한다.

- Reader·Listener: **첫 공개 빌드부터 임베디드 없이** 태어난다(호스트 전용).
- Chat: v2 오케스트레이터 + 수리 부트스트래퍼(§V2-8.3)가 갖춰지는 릴리스에서
  임베디드를 제거한다. 단, 호스트 자체의 빌드(엔진 바이너리)는 Chat
  워크스페이스에 그대로 남는다 — 제거 대상은 **앱 exe 안의 임베디드 경로**뿐.
  **(버전 결정 갱신 2026-09-18, 사용자): 목표 버전 v0.11.0 범프 폐기 — 배포 버전은
  0.10.1 고정. M6 제거 작업분은 0.10.1 재발 빌드에 포함하고 CHANGELOG v0.10.1
  섹션·README를 새 동작(임베디드 폴드백 없음)으로 수정한다.**
- 신뢰성 지표(`health{session_success_ratio, fallback_ratio}`)는 제거 문턱이 아니라
  **상시 건강 모니터링**으로 유지 — 각 기기 로컬 계산(telemetry 업로드 없음), 앱이
  welcome으로 읽고 개발자는 베타·낮부 측정으로 호스트 건강을 추적.
- 실패 UX (확정): 엔진 부재/장애 시 spawn → 수리(한 클릭) → 클라우드(동의 시) →
  기능 비활성 안내. 조용한 실패 금지, 크래시 없음(앱의 추론 접점이 파이프뿐이라
  자연 저하).

## V2-9. 참조 클라이언트 (다언어)

- **C++** (`engine_host_client.hpp/.cpp`): Chat에서 v1으로 검증된 것을 v2 확장.
  Chat·Listener 공용. 자기완결(Win32+STL) 원칙 유지.
- **Rust**: Reader용(fltk 런타임 아래 Rust 코어에서 사용). 동결 계약(§4, §V2-4)을
  동일하게 재구현. flutter_rust_bridge 래핑과 분리해 유닛 테스트 가능하게.
- 두 구현은 공유 **골든 케이스**(바이트 호환 요청/응답 쌍)로 상호운용을 검증한다.

## V2-10. 업그레이드 게이트 (엔진 라이브러리 갱신 절차)

llama.cpp / transcribe.cpp / onnxruntime / ct2 의 핀 변경은 **해당 워커 빌드**만
대상이다. 배포 조건 = 워커별 골든셋 통과:

| 워커 | 게이트 |
|---|---|
| ggml-translate | 번역 품질 프로브(기존 fidelity harness) 기준 통과 + 지연 상한 + 프롬프트 템플릿 불변 확인 |
| ggml-asr | WER 기준선 + 부분 결과 계약(onPartial 형식) + 지연 예산(청크 <100ms, E2E <2s) |
| onnx | EP 체인(DirectML→CUDA→CPU) 각 동작 + 출력 무결성(TTS 오디오/OCR 텍스트) |
| ct2 | 번역 정확도 기준선(M2M100 기준) |

미통과 시 **핀 유지** — 최신화는 선택이지 의무가 아니다. 게이트 결과는
components.json에 기록. 이 조항이 "업그레이드가 안전한 루틴 작업"이 되게 한다.

## V2-11. 실패 모드 매트릭스 v2 (v1 §5.4의 확장)

| 상황 | 동작 |
|---|---|
| 워커 크래시 | 해당 패밀리만 `unavailable` 보고 → 해당 세션 정리 → 자동 재스폰. 다른 패밀리 무영향 |
| capability 미배포 | `unavailable` → 앱: 기능 비활성 / 클라우드(동의) / 안내 |
| 스트리밍 세션 중단 | session 종료 이벤트(kind=error/eos) + 재시도는 앱 판단 |
| 오케스트레이터 사망 | 클라이언트가 timeout 내 감지(v1 계약) → 재스폰(수리 포함) |
| 모델 파일 훼손 | 서명 매니페스트 해시 불일치 → 해당 모델만 `unavailable` → 앱이 재다운로드 안내 |
| VRAM 부족 | 스케줄러: eviction(프로파일 정책) → 큐 대기 → 불가 시 busy/timeout |
| 미확인 필드/op/status | 무시 / `bad_request` / 실패로 수렴 (§4.3의 보편화) |

## V2-12. 확장성 4조항 (동결 대상)

1. **오케스트레이터 무지식** — 백엔드 추가·통합은 코드가 아니라 데이터(워커 exe +
   매니페스트 + 레지스트리)로 끝낸다.
2. **무시 규칙 보편화** — 프로토콜·레지스트리·매니페스트·워커 선언 전부: 미확인
   필드는 무시 + `schema_version` 필수.
3. **대용량 페이로드 경로 참조** — 프레임은 제어 메시지용, 데이터는 로컬 경로로.
4. **리소스 프로파일은 데이터** — 스케줄러 정책 입력은 레지스트리가 제공(코드
   변경 없이 정책 조정 가능).

## V2-13. 동결 절차와 마일스톤

**동결 대상**: §V2-3(난부 계약 포함), §V2-4, §V2-5(스키마), §V2-6, §V2-8, §V2-12.
세 워크스페이스 합의로 **v2.0**으로 동결하며, 이후 변경은 v1과 동일한 통지 규정을
따른다. §V2-7·§V2-9·§V2-10·§V2-11은 규범이되 구현 상세는 워크스페이스별 자율.

**마일스톤**:
- **M6 (Chat)**: 오케스트레이터(v1 호스트 확장) + ggml-translate 워커 분리 +
  레지스트리/서명 매니페스트 + 수리 부트스트래퍼. v1 클라이언트 호환 검증.
- **M7 (Listener)**: 스트리밍 ASR 세션 프로토콜 확정 + ggml-asr 워커 +
  멀티세션 스케줄러·백프레셔. Listener 이식 + 지연 예산 실측.
- **M8 (Reader)**: Rust 레퍼런스 클라이언트 + ct2/onnx 워커 + 카탈로그 규약 +
  경로 흡수 마이그레이션.
- **M9**: 임베디드 제거 1단계(지표 수집) → 기준 충족 시 2단계(앱별 제거).
- **M10**: 롤백/수리 조합 테스트 + 통합 QA(v1 §9.3의 확장판).

## V2-14. 결정 완료 기록 (2026-09-18)

동결 전에 열여 있던 6개 질문이 전부 결정됐다:

1. **ASR 피드 규격** — 라이브 청크: JSON 메타 프레임 + 바이너리 PCM(16kHz mono
   s16le, ≤ 64 KiB); 녹음 파일은 로컬 경로 참조 (§V2-4.4).
2. **TTS 반환** — WAV 파일 경로(24kHz 16-bit PCM), 세션 디렉터리 기록, 앱이 재생
   후 삭제 (§V2-4.4).
3. **WinRT OCR** — 엔진 범위 외. Reader 앱 잔존, PP-OCRv6만 onnx 워커로 (§V2-3).
4. **임베디드 제거** — 관찰 문턱 폐기, 즉시 제거 (§V2-8.6). 실패 UX: 수리 →
   클라우드(동의) → 비활성 안내.
5. **번역 프로파일 소유** — Chat 소유, 패밀리 공유 품질 기준 (§V2-5.2).
6. **파이프 이름** — `\\.\pipe\emebala-engine`(버전 무관), `hello.protocol` 협상,
   전환기 `emebala-engine-v1` 별명 병행 (§V2-4.1).

**잔여 동결 전 작업**: 세 워크스페이스 합의 리뷰(§V2-13 절차) + 코드 서명 인증서
확보(§V2-6, 운영 결정) + 카탈로그 호스팅 URL(§V2-7, 운영 결정).
