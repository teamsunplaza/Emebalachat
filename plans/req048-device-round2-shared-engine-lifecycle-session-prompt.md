# REQ-048 세션 프롬프트 — 2차 실기기 실패 4종(P1~P4)根治 + 공유 엔진 라이프사이클(F1~F3) + 잔존 D3(P2) 완전根治 + 클린 마감

## 최우선 입력 문서 (필독 순서)
1. docs/260919_0004_session_req047-device-proof-failures/051600_handoff-req047-to-req048-device-round2-engine-lifecycle.md  ← 모든 사실·단서·VP 해석이 여기 있음
2. docs/260919_0004_session_req047-device-proof-failures/p47-device-proof-checklist.md  ← 미회신 A1~A4 (REQ-047 재증명 — 이번 관찰로 부분 대체됨)
3. 같은 세션의 033400(P7 VP 리뷰) · 032900(P6 감사, R1~R8 잔존) · 031819(게이트+셋업 명세)
4. plans/req047-device-proof-failures-session-prompt.md §규칙 (동결 계약 계승)

## 목표 (사용자 관찰 원문 기준 — 260920 05:12)
P1. "번역모델이 없다면서 기본 내장로컬엔진(Hy-MT2-1.8B)에 의한 번역이 안 됨. 설치하자마자 되어야하는데." — 설치 직후 부팅 상태에서 내장 엔진 번역 실패 + '모델 없음' 신호. 원인 후보(handoff §3): config 오염 계승(engine_type auto/user_gguf 갈림)·model_path 상대경로 해석·부팅 라우터 미초기화. **신규 설치(first-run) 상태 시뮬레이션이 결정적 재현.**
P2. "OpenAI호환 누르면 아무 것도 안 나옴… 제목만 있고 x만 있어. 어떤 입력칸도 없고." — D3(REQ-047 3f6419d)가 일부(창 생성은 됨)이고 **제2 근본 잔존**. 유력: 런타임 DLGTEMPLATE 바이트 정렬/DS_SETFONT 결함 → 컨트롤 0개 생성. GUI 없이 증명 가능한 템플릿 직렬화 파싱 단위 테스트 신설로 확정.
P3. "내장로컬엔진 실패 시 자동 Google 폴백되나본데 그 다음에도 번역이 안됨." — 폴백이 목격된 것 자체가 config 상태 증거(strict이면 폴백 불가). google_call이 실패하는 근본(초기화 순서? WinHTTP? smart_bypass 오작동?) 규명.
P4. "다른 모드로 바꿨다가 Google로 되돌리면 그 때는 작동함." — **수동 전환 경로 정상·부팅 경로 불량**의 비대칭이 핵심 단서. SetEngineType 수동 반영 vs wWinMain 부팅 해석 분기(main.cpp ~L998/L1001) 차이 규명. P1과 동일 근본일 가능성 높음.

F1. 셋업 설치 시 "Reader/Listener와 공유하는 Emebala Engine을 설치(또는 재사용)합니다" 안내 페이지 추가.
F3. 제어판 삭제 시 공유 엔진 삭제 확인 + 다른 Emebala 제품 사용 중이면 "사용하고 있으므로 삭제하지 않습니다" 안내 (감지 계약 설계 필수 — ARP 스캔/components.json/프로세스 3중, handoff §4).
(1-2는 설명으로 종료: 언인스톨 파일은 설치 시 Inno 표준 생성 — 코드 요구 없음.)

## 전제 사실 (시작 시 git/dir로 재확인)
- HEAD 8c09ee0 (REQ-047 6커밋). push 미실행, 누적 ~30건.
- run_tests **3,693/0** = 무회귀 기준선.
- 현 설치본 = REQ-047 최종 셋업 (Emebalachat_Setup_0.10.1.exe 117,617,142B SHA d4b09d11…cf39a) 실설치·구동 상태. 이 상태가 P1~P4 재현 환경.
- %LOCALAPPDATA%\Emebala\Common\engine\ 워커/오케스트레이터 실재(H1). **모델 파일(Hy-MT2 *.gguf 1.9GB) 존재 여부는 미실측 — P1 1순위.**
- config.json은 사용자가 REQ-047 A0 수동 복구를 했는지 미확인 — 실측으로 출발 상태 확정.
- 빌드: `call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && cmake --build build --config Release`. ISCC: `C:\Users\k1yt\AppData\Local\Programs\Inno Setup 6\ISCC.exe`.
- 에이전트 터미널: 파일/레지스트리 읽기·파이프 열거·**엔진 spawn+pipe 왕복 가능(m6_orchestrator_smoke 11/11 실증)** — 서빙 계열은 자동 재현 가능. WinHTTP 라이브·GUI 창 실측만 이연.

## 절차 (P1→P8, 🟣 — full mandatory, 사용자 무개입 자율)
- P1: docs/YYMMDD_NNNN_session_req048-device-round2-engine-lifecycle/requirement-checklist.md — P1~P4·F1·F3 원문 인용 승격 + 하위 검증항목 + 신규 발견 시 추가 규율("발견하면 전부 수정" — 사용자 명시).
- P2/B2: debug — 실측 1순위: config.json·모델/워커 dir·파이프·프로세스·ARP·(허용 시) first-run 시뮬레이션(격리 %LOCALAPPDATA%로 dev 빌드 구동 — 실환경 파일 무접촉). P2는 템플릿 직렬화 파싱 테스트로 결정적 증명. P3/P4는 부팅 vs 수동 전환 대조 표. 각 항목 검증보고서.
- P3: architect — 최소 수정. P1/P4 단일 근본 여부 판정 후 설계. F3는 uninstall 수명주기 계약 신설 — reader/listner ARP 식별은 웹/레지스트리 실측 연구 후. Ask Light Gate + Debug Tech Gate 필수.
- P4: 1건=1위임 직렬. **접촉 매트릭스**: main.cpp는 P1·P3·P4 공용 → 직렬 필수. openai_settings_window.cpp P2 단독. setup.iss F1·F3 공용(같은 파일 — 직렬). 매 위임: 빌드 0경고 + run_tests 3,693 무회귀 + setup.iss 접촉 시 인스톨러 게이트 2종. 항목마다 커밋 후 다음.
- P4-final: 자동 가능 재증명 전부 실행 (m6 스모크로 서빙 왕복, 템플릿 파싱 테스트, 셋업 ISCC 컴파일+게이트) + **최종 바이너리에서 셋업 1회 재생성** (SHA/타임스탬프 기록, 구 산출물 .prev_* rename). GUI·파괴적 항목은 잔여 실측 목록으로 명문화 (무개입이므로 PASS 주장 금지 — "자동 증명 완료 + 사용자 1회 확인 이연"으로 정직 마감).
- P5~P7: debug 기술리뷰(전 diff) → ask Full Audit → VP 독립 리뷰 (zero-tolerance: 조건부 PASS = REJECT 재작업).
- P8 마감 의무 (사용자 명시): ① 임시파일 정리 — 세션 중 생성한 tools_tmp_*/프로브/임시 로그를 **휴지통 방식으로** 삭제 (기존 선행 tools_tmp_* 잔존물은 AGENTS.md 원칙상 손대지 않되, 본 세션이 만든 것만), .prev_* 셋업은 유지. ② **VS Code 문제 패널 0** — `cmake --build` 경고 0 + workspace 진단 잔존 오류 없음으로 증명 (열린 파일의 스테일 진단은 재검사로 해소). ③ 커밋 완료(작업 트리 클린, plans/handoff 문서 포함 커밋 대상 분류 — docs는 gitignore라 소스/셋업 관련만). ④ 셋업 파일 완성·SHA 보고. ⑤ push는 **승인 대기 목록 보고로 종료** (무개입이어도 push는 예외 — 규칙).

## 규칙 (REQ-044/045/046/047 계승 — 무접촉 동결)
- protocol/client VERBATIM·worker_protocol·v1 파이프명·EXTRA_INFO_MARKER·SetDllDirectoryW·delay-load·SHA-256 검증·shape-only 로그·상수시간 비교·privacy 동의 게이트·Hy-MT2 'local' 불변·C1 게이트 후퇴 금지·OpenAI 보안 경계(DPAPI·마스킹·https-only+consent·리다이렉트 차단)·**REQ-047 신계약: unins000.exe 고정명(loud 가드), drain-owning engine-modal latch, bundled 등록 거부, WM_APP+0x500 지연 오픈, 4-way 라디오 ID 2010~2014 불변**.
- 실환경 파일(설치본·모델·registry·config)은 진단 외 수정·삭제 금지. 언인스톨 파괴 금지 — F3 증명은 언인스톨러의 dry-run/정적 검증 + 사용자 이연.
- 자동 게이트로 못 잡는 항목은 🔶 이연 명문화하되, **이연을 PASS로 포장 금지**.
- run_tests 기준선 무회귀·테스트 약화 금지·apply_diff surgical·REQ-048 태그 주석·/W4 0·C++20·RAII.
- 발견한 신규 결함은 체크리스트에 추가 후根治 ("내가 발견하지 못 한 문제도 전부" — 단, 범위 폭주 시 VP가 잔여로 명문화).
- git commit 항목별 VP만. push 사용자 승인 시만.
- 언어: 응답 한국어.

## 완료 판정
- P1~P4 원인 진단서(실측 전/후, 가설 기각/채택, P1·P4 단일/이중 근본 판정) +根治 diff + 신규 테스트.
- F1·F3 설계·구현 + 인스톨러 게이트 green + uninstall 계약 자동 검증.
- 자동 재증명: run_tests(3,693+신규)/0, 빌드 0경고, m6 스모크 green, 셋업 최종 재생성 SHA 기록.
- 잔여 실측 이연 목록(정직 표기) + push 대기 ~30+n 건 보고 + 임시파일 정리·문제패널 0 증명 + 커밋 완료(트리 클린).
```
