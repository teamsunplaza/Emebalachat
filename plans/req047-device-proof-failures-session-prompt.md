# Emebala Chat — REQ-047: REQ-046 실기기 검증 실패 4종(D1~D4)根治 세션 시작 프롬프트

> REQ-046(260919_0003)의 P4-5 실기기 검증에서 잔존/신규 결함 4종 확인. 새 세션에 그대로 붙여 넣는다.

---

```
Emebala Chat 워크스페이스(d:\OneDrive\Projects\Emebalachat)에서 "REQ-047: REQ-046 후속 — 실기기 검증 실패 4종根治" 세션을 시작한다.

## 최우선 입력 문서 (필독 순서)
1. docs/260919_0003_session_req046-reinstall-regression-fixes/233200_handoff-req046-to-req047-device-proof-failures.md  ← 모든 사실·단서·절차가 여기 있음
2. docs/260919_0003_session_req046-reinstall-regression-fixes/p45-device-proof-checklist.md  ← 사용자 관찰 원문(체크 결과)
3. plans/req046-reinstall-regression-fixes-session-prompt.md §규칙 (동결 계약 계승)
4. 같은 폴더의 194000/194730 설계서와 구현 보고서 4건 (직접 재작성 금지 — 계승)

## 목표 (사용자 관찰 원문 기준 — 4종根治)
D1. "메모장 한글 입력 후 Enter → 인라인 번역이 안 된다. 그런데 '로컬번역을 사용할 수 없습니다' 모달도 뜨지 않는다." (로컬LLM 체크 상태, 워커는 디스크에 실설치 확인됨) — **무반응 실패 + 실패 알람까지 소실**. 상태 수렴(config engine_type↔tray↔런타임 enum) 또는 bootstrap/repair-signal 경로 붕괴 가능성. P4-2에서 추가된 'user_gguf→LocalLlama' 부팅 해석 분기(main.cpp ~L998)와 상호작용 의심.
D2. ".gguf 등록 완료 모달은 새 스펙대로 떴다(지연 상한 문구 OK). 그러나 ① 트레이 재오픈 시 체크가 '사용자 선택(.gguf)'에 머물지 않고 ② 60초+ 대기 후 Enter해도 새 모델로 번역되지 않는다." — 등록 파이프라인의 config 저장/refresh_tray 순서·snapshot 수렴 버그 후보. ②는 D1과 동일 근본일 수 있음(서빙 경로 전반).
D3. "OpenAI 호환엔진설정 창이 여전히 최소화/입력불가로 뜬다." — parent+WS_VISIBLE+DS_CENTER/DS_SETFOREGROUND 정적修复(bf421a2)로는 증상 미해결이 **실기기에서 확인됨**. 유력 가설(h2handoff §2/D3): 트레이 컨텍스트 메뉴 트래킹(모달 루프) 중인 GUI 스레드에서 DialogBoxIndirectParamW 동기 호출 → 메뉴 모달리티와 중첩. 대조 실증: ea42bb6 '로컬불가' 모달은 실기기 정상 표시됨(=언제 어디서 뜨나?) vs OpenAI 창(트레이 메뉴 항목 클릭 경로). 해법 계열: 메뉴 종료 후 PostMessage/wparam 지연으로 다이얼로그 open, 또는 WS_POPUP 최상위 소유 재설계. **동적 진단 필수 — 정적 추측만으로 재수정 금지.**
D4. "제어판 삭제 → 'unins000.exe를 찾을 수 없습니다' 오류." — 0.10.2 위에 0.10.1 덮어쓰기 재설치 후 발생. 7e0882c loud-fail 가드가 어떻게 통과됐는지(경고만?) + ARP UninstallString/설치 {app} 경로(per-user vs per-machine) + 이번 설치 로그(%TEMP%\Emebala Setup Log*, 2026-09-19 23시 대) 부재 여부까지가 1차 조사 범위.

## 전제 사실 (260919 23:30 KST — 시작 시 git/dir로 재확인)
- HEAD 9dbe767 (REQ-046 커밋 4건: 3a7b7cc/d4d5108/bf421a2/9dbe767). push 미실행, 누적 ~24건 — 마감 시 사용자 일괄 승인.
- run_tests 3,599/0 = 현 기준선 (REQ-044/045/046 계승. 무회귀 기준).
- 현 재현 환경: **이 머신에 0.10.1(셋업 117,627,060B @ 23:15, SHA 6B23B847…4327E4) 덮어쓰기 설치 완료·구동 가능 상태.** 삭제/언인스톨로 파괴 금지 (D4 때문에 언인스톨 경로 자체가 깨져 있음 — 복구가 더 어려움).
- %LOCALAPPDATA%\Emebala\Common\engine\ 에 워커 exe/worker.manifest 실재 (H1根治 실증됨 — 건드리지 말 것).
- config.json 실측(사용자 검증 중): engine_type/user_model_id 현재 값은 P2에서 재확인 (D1/D2의 출발 데이터).
- ISCC 실제 경로: C:\Users\k1yt\AppData\Local\Programs\Inno Setup 6\ISCC.exe (AGENTS.md 경로와 다름 — 매번 실측).
- 에이전트 터미널: WinHTTP 라이브 차단 + GUI 세션 없음 → 창/번역 실측은 사용자 협조 또는 tools/e2e(인터랙티브)로만.

## 절차 (P1→P8, 🟣 — full mandatory)
P1: docs/YYMMDD_NNNN_session_req047-device-proof-failures/requirement-checklist.md — D1~D4를 사용자 관찰 원문 인용으로 승격 + 하위 검증 항목. 복잡도 분류.
P2/B2: debug — 재현 환경 실측 1순위 (handoff §2의 항목별 단서·커맨드 그대로 사용): config/engine dir/프로세스·파이프/23시 설치로그/ARP 레지스트리/HKCU·HKLM {app} 갈림 확인/diag_log_shape 활성 재현(본문 로그 금지). D3는 ea42bb6 모달과의 컨텍스트 대조(스레드·시점·메뉴 트래킹 중 여부). 각 항목 검증보고서(H 기각/채택 증거).
P3: architect — 원인별 최소 수정. D3는 '트레이 메뉴 종료 후 지연 open' 아키텍처 유력 — 메시지 흐름 설계 명시. D4는 설치/언인스톨 등록 계약 전체 재검토. Ask Light Gate + Debug Tech Gate 필수.
P4: 1건=1위임 직렬 (수정 파일 충돌 매트릭스: main.cpp는 D1·D2·D3 공용 → 반드시 직렬). 매 위임: 빌드 0경고 + run_tests 3,599 무회귀 + (setup.iss 접촉 시) 인스톨러 게이트 2종. **각 항목修复 직후 해당 증상을 사용자 실측으로 1회 확인**(REQ-046 교훈: 자동 게이트만으로는 GUI 모달리티·ARP·런타임 파이프라인을 못 잡는다) — 체크리스트 회신 형식으로.
P4-final: 전 구현 완료 후 **최종 바이너리 상태에서 셋업 1회 재생성**(타임스탬프/SHA 기록, 구 산출물 .prev_* rename — 삭제 금지) → 사용자 승인 하 덮어쓰기 재설치 → 4종 재증명 순서: (a) Hy-MT2 번역 → (b) .gguf 상태 유지+서빙+로컬LLM 복귀 → (c) OpenAI 창+실 왕복 → (d) 제어판 삭제 **최후**. (d) PASS 후 선택 재설치로 환경 복구.
P5~P8: debug 기술리뷰(전 diff) + Ask Full Audit(D1~D4 원 관찰 대비) + VP 독립 리뷰 + push 일괄 승인.

## 규칙 (REQ-044/045/046 계승 — 무접촉 동결)
- protocol/client VERBATIM/worker_protocol/v1 파이프명/EXTRA_INFO_MARKER/SetDllDirectoryW/delay-load/SHA-256 검증/shape-only 로그/상수시간 비교/privacy 동의 게이트/Hy-MT2 'local' 불변/**REQ-046 C1 게이트(user_gguf일 때만 model_id) 후퇴 금지**/OpenAI 보안 경계(DPAPI·마스킹·https-only+consent·리다이렉트 차단) 완화 금지.
- 실환경 파일(설치본·모델·registry·config)은 진단 외 수정·삭제 금지. 재설치는 검증 목적 + 사용자 승인 하에만.
- run_tests 기준선 무회귀·테스트 약화 금지·통째 재작성 금지(apply_diff surgical)·REQ-047 태그 주석.
- git commit은 항목 완료+검증 확인 후 VP만. push는 사용자 승인 시에만.
- 언어: 응답 한국어. /W4 0, C++20, RAII.

## 완료 판정
- D1~D4 원인 재진단 보고서(실측 전/후 + 가설 기각/채택) +根治 실행 보고서(각症状 실기기 재증명 로그).
- 4종 재증명 전부 PASS: (a) 로컬LLM 모달 없이 Hy-MT2 인라인 번역 / (b) 등록→체크 '사용자 선택(.gguf)' 유지 + ≤1분 후 그 모델 번역 + 로컬LLM 복귀 시 Hy-MT2 / (c) OpenAI 창 정상 표시·입력 + 실 프로바이더(또는 mock 왕복 + 사용자 실확인 분담) 후 실 번역 / (d) 제어판 삭제 성공(unins000 정상).
- 신규 결함 없음 증명: run_tests(3,599+신규)/0, 게이트 전종 green, 무회귀 스모크.
- push 승인 + REQ-047 잔여 리스크/M8 전승 메모.
```

---

## 세션 컨텍스트 메모 (참고용, 프롬프트 외부)
- **D1이 최우선**: 서빙 파이프라인이 죽으면 (b)(c) 검증도 성립 불가. D1/D2를 먼저 분리-재결합(로컬LLM+클린 config로 재현)한 뒤 D3/D4로.
- D1의 '모달 소실'은 상태 정보다: repair-signal이 미발동이든, 발동됐으나 표시 경로가 죽었든, 어느 쪽인지가 분기점. `engine.cpp` TrySharedHost/실패 분류와 `TranslationManager` 라우팅 결정 로그(shape-only)가 결정적 증거.
- D2①(체크 미유지) 재현 커맨드: 등록 직후 `type %LOCALAPPDATA%\Emebalachat\config.json | findstr engine_type` — 파일은 user_gguf인데 체크만 안 따라오면 refresh/snapshot 경로, 파일도 다르면 저장 경로 붕괴.
- ea42bb6 모달(정상 동작 실증)과 OpenAI 다이얼로그(실패)의 **표시 시점 차이**에 답이 있을 가능성 높음: 모달은 후크 스레드/작업자 이벤트에서 (메뉴 트래킹 밖), OpenAI 창은 메뉴 항목 클릭 = 메뉴 모달 루프 안에서 직접 호출.
- 검증 사이클 단축: dev 빌드 실행본(`build\Emebala_chat.exe`)과 설치본 동작이 다르면(후크/DLL경로) 그 자체가 단서 — 동일 증상인지 먼저 대조.
