# Emebala Chat — REQ-046: 재설치 후 회귀 3종 + 버전 되돌림 세션 시작 프롬프트

> REQ-045(260919_0002) 마감 직후, 사용자가 0.10.2 셋업으로 **실제 재설치해 경험한 잔여 결함** 기준. 새 세션에 그대로 붙여 넣는다.

---

```
Emebala Chat 워크스페이스(d:\OneDrive\Projects\Emebalachat)에서 "REQ-045 후속: 설치본 회귀 3종根治 + 버전 0.10.1 되돌림" 세션을 시작한다.

## 목표 (사용자 원문 의도 — verbatim)
1. "새로 설치했는데, '로컬번역을 사용할 수 없습니다' 창이 뜨네. 그리고 실제로 번역이 안 됨. 모델은 다운로드 받아져있어." (= REQ-045 항목2根治가 실패했다. 0.10.2 셋업으로 재설치했는데도 로컬 서빙이 안 된다. 원인을 재진단하고 실제로 동작하게 만들어라.)
2. "버전은 v0.10.1을 유지했어야했는데, 네가 v0.10.2로 올렸으니, 다시 다운시켜서 v0.10.1로 모두 유지하게 하고..." (= 버전 식별자를 전부 0.10.1로 되돌리고, 0.10.1 파일명으로 최종 셋업을 재생성.)
3. "번역엔진선택 > OpenAI호환엔진설정 > 하면 창이 뜨긴 하는데, 어떤 입력도 할 수 없이 최소화된 창이 떠. 실제 입력 후에 번역이 되도록 이 기능도 완성시켜야해." (= 설정 다이얼로그 창 결함 수정 + 키/URL/모델 입력 → 실 번역 end-to-end 완성.)
4. "번역엔진선택 > 사용자선택(.gguf)를 클릭하고 모델을 선택하면, 모델 등록이 완료됐다고 나오고, 트레이메뉴에서 로컬LLM을 선택하라고 하는데, 실제로 작동이 안 됨. 그리고 로컬LLM을 선택하라고 하면 안 되고, 번역엔진선택 > 사용자선택(.gguf)로 하면, 사용자선택에 머무르는 상태로 해당 모델을 emebala engine의 llama cpp를 통해 번역에 이용할 수 있도록 해야지. 로컬LLM Hy-MT2-1.8B는 우리의 기본 내장 옵션이기 때문에, 이건 터치하면 안 됨." (= 사용자 모델 선택은 그 자체로 '엔진 선택' 상태가 되어야 함: 선택 후 로컬LLM으로 전환하지 말고, '사용자 선택(.gguf)'이 체크된 상태로 그 모델이 Emebala Engine(llama.cpp 워커)을 통해 번역을 서빙해야 한다. 로컬LLM=Hy-MT2 기본 내장 경로는 절대 불변·무영향.)

## 전제 사실 (260919 19:00 KST 기준 — 세션 시작 시 git log/status로 재확인)
- HEAD 2c78e29. REQ-045 커밋 10건(229e954 이후): 7b171a0(워커 loud-fail+설치기 registry.json+문구) / 7e0882c(언인스톨 가드) / f472625(OpenAI 엔진)+90d8d55(리다이렉트 차단) / 44c0fbe(워커 registry 해석+session_open 릴레이) / 102591e(파일찾기+SerializeRegistry+user_model_id) / ea42bb6(모달화) / 3b3a426(0.10.2 bump — 되돌릴 대상) / 2c78e29(CHANGELOG v0.10.2 섹션). push 미実施(20건 누적, 승인 전까지 금지).
- run_tests 3,570 checks / 0 failures = 현 기준선 (무회귀 기준). 스모크 11/11. 보안감사 PASS(155500). Ask Full Audit 18/18 ✅(180700)이었으나 **실사용 재현에서 위 3건 실패 판명** — P6/P7이 잡지 못한 원인 규명이 이번 세션의 핵심.
- 최종 셋업: installer\output\Emebalachat_Setup_0.10.2.exe (117,626,843B @ 18:39) — **사용자가 이걸로 실제 재설치했고 현재 이 머신에 설치·구동 상태. 절대 삭제/언인스톨하지 말 것 = 재현 환경.**
- 이전 세션 문서 전권: docs/260919_0002_session_postinstall-fixes-uninstall-engine/ (진단 123200/123315, 설계 124500+Rev2 041500, 게이트 041600/125400, 구현 보고서 8건, qa-e2e-checklist.md). 123200 §2-1의 디스크 표가 '0.10.1 설치본 기준' 사진이라면, **지금 0.10.2 재설치 후 디스크를 다시 실측**하는 것이 첫 작업.

## 최우선 가설 (P2/B2에서 우선 검증 — 기각/채택 증거 남길 것)
H1 (재발 의심 1위): setup.iss의 워커 [Files] 항목에 남아 있는 `Check: ShouldInstallEngineWorker` 런타임 게이트. 7b171a0는 `skipifsourcedoesntexist`(컴파일 타임)만 제거했고, Check 함수는 런타임에 WorkerExePath(`{source}`/SourceDir 상대 전개)를 판정해 False면 **여전히 무음 skip**한다. ISCC 컴파일 성공과 런타임 무음 skip은 양립하므로, 재설치 후 %LOCALAPPDATA%\Emebala\Common\engine\에 워커/worker.manifest가 실제로 들어왔는지 디스크 실측이 최우선. 미설치 확정 시: Check 로직(경로 전개 기준) 또는 제거+별도 방식으로根治.
H2: 설치됐다면 워커 기동 실패 계열 — worker.manifest 핸드셰이크, spawn 실패(백엔드/DLL 로드), models\registry.json 내용(idempotent 생성이 이전 잔존 파일 때문에 스킵되어 구식/부정확은 아닌지), .sha256ok 마커.
H3: 항목4가 가리키는 서빙 경로 결함 — user_model_id는 호스트가 **부팅 시 1회** 캐시(P4-4 설계). 사용자는 '선택 즉시' 동작을 기대. 게다가 엔진 선택이 local/'' 인 상태에서는 릴레이 자체가 무의미할 수 있음 — '사용자 선택(.gguf)'을 독립 엔진 상태로 만드는 재설계(R5: Chat→호스트 v1 wire은 동결이라 model_id를 못 싣고, 호스트 config 캐시 경로만 열려있음)와 연결해 진단.
H4: OpenAI 창 — openai_settings_window.cpp 의 CreateWindow/ShowWindow(WS_VISIBLE vs nCmdShow, SW_MINIMIZE 원인 추정: WinMain-style show flag 또는 부모/메시지 루프 부재) + 입력 불가(포커스/WS_DISABLED/자식 컨트롤 생성 실패) 실측.

## 절차 (P1→P8, 🟣 판정 — full mandatory)
P1: 이 프롬프트로 docs/YYMMDD_NNNN_session_req046-reinstall-regression-fixes/requirement-checklist.md 작성(위 4개 요구 verbatim + 하위 항목) + 복잡도 분류. 버전 되돌림은 사용자 지시 확정(추가 질의 불요).
P2/B2: debug — 재현 환경(현재 0.10.2 설치본) 실측 우선: 디스크(engine/models, registry.json 내용, worker 존재) + 프로세스/파이프 + 앱 diag 로그(%LOCALAPPDATA%\Emebalachat\logs, 필요 시 config로 diag_log_shape 활성화 후 재현 — 본문 로그 금지 원칙 유지) + setup.iss Check 런타임 거동 정적 분석 + 설치 로그(%TEMP%\Emebala Setup Log*) 수집. OpenAI 창과 .gguf 서빙도 같은 조사에서 증상별 원인 확정. 각 항목 검증 보고서.
P3: architect — 원인별 최소 수정 설계 + '사용자 선택(.gguf)=엔진 상태' 재설계(체크 표시 유지, Hy-MT2 로컬LLM 경로 무영향 불변식 명시, v1 동결 wire 제약 하 반영 타이밍 정책: 호스트 재기동 경계 또는 config 재검 타이밍). Ask Light Gate + Debug Tech Gate 필수.
P4: 구현 — 1건=1위임 직렬. 접촉 파일 충돌(setup.iss ↔ main.cpp ↔ openai_settings_window 등) 매트릭스. surgical만. 모든 위임: VsDevCmd 체이닝 빌드 경고 0 + run_tests 3,570 무회귀(신규만 증가) + (setup.iss 접촉 시) 인스톨러 게이트 2종 + ISCC + 스모크 11/11. **검증은 반드시 '실제 재설치 후 로컬 번역 성공'으로** — 사용자가 현재 설치본을 유지하므로, 재설치(덮어쓰기)는 사용자 승인 하에 이 머신에서 수행 가능.
P4-final: 버전 되돌림 — CMakeLists project VERSION, setup.iss AppVersion/OutputBaseFilename, README.md 배지/다운로드, installer/README.md 출력명, tests/run_tests.cpp REQ-006 버전 핀(L"0.10.2"→L"0.10.1") 전부 0.10.1로. CHANGELOG의 v0.10.2 섹션은 v0.10.1 산출 관점으로 수렴(사용자와 문구 확인; 공개된 0.10.1 릴리스 섹션과 혼동 없도록). 최종 셋업은 Emebalachat_Setup_0.10.1.exe로 재생성(.prev_*·구 산출물 덮어쓰기 방식 주의).
P5~P8: debug 기술리뷰(전 diff) + Ask Full Audit(원문 4건 정신 대비) + VP 독립 리뷰 + 마감. 완료 판정에 필수: **이 머신 0.10.1(되돌림) 셋업으로 재설치 후 (a) 모달 없이 로컬 Hy-MT2 번역 성공 (b) .gguf 파일찾기→'사용자 선택' 머문 상태로 해당 모델 번역 성공 (c) OpenAI 창 입력 가능+실 번역 성공 (d) 제어판 삭제 성공** — 4종 실기기 증명.

## 규칙 (REQ-044/045 계승)
- 동결 계약 무접촉: engine_host_protocol.hpp / engine_host_client.*(VERBATIM 사본) / worker_protocol.hpp / v1 파이프명 / EXTRA_INFO_MARKER / SetDllDirectoryW / delay-load / SHA-256 검증 / shape-only 로그 / 상수시간 비교 / privacy 동의 게이트 / Hy-MT2 기본 경로(로컬LLM) 불변.
- run_tests 3,570 기준선 무회귀·테스트 약화 금지. 통째 재작성 금지(apply_diff surgical). 변경 주석 REQ-046 태그.
- git commit은 항목 완료+검증 로그 확인 후 VP만. push는 사용자 승인 시에만.
- 현재 설치본/사용자 모델 파일/registry 등 실환경 파일을 진단 외 용도로 수정·삭제 금지. 재설치는 검증 목적 + 사용자 승인 하에만.
- API Key 보안(DPAPI+마스킹+SHA 무결성, https-only+consent, 리다이렉트 차단)은 완화 금지 — 창 수정은 보안 경계 밖.
- 3b OpenAI '완성' 판정엔 실제 프로바이더 또는 로컬 mock(/v1/models+/v1/chat/completions) 중 최소 하나와 왕복 증명 필요(mock이면 실 프로바이더 확인 항목을 사용자 QA 체크리스트로 이연 가능하나, 이번엔 창 자체가 열리지 않은 것이므로 UI 우선).
- /W4 0, C++20, RAII. 언어 프리퍼런스: 응답 한국어.

## 완료 판정
- 재진단 보고서(설치본 실측 전/후 + H1~H4 기각/채택) + 수정 실행 보고서(REQ-045 회귀 원인 3종 각根治, 동등성·무회귀 증명 표).
- 위 'P4-final 4종 실기기 증명' 전부 PASS + 게이트 전종(run_tests/스모크/인스톨러 2종/ISCC/SHA-sync) green.
- 버전 식별자 전 위치 0.10.1 일치 어서션 + CHANGELOG 정합 + 최종 셋업 `Emebalachat_Setup_0.10.1.exe`(신규 타임스탬프) 인도.
- '사용자 선택(.gguf)=독립 엔진 상태' 동작 명세: 체크 유지, 로컬LLM 전환 없음, Hy-MT2 무영향 회귀 테스트 신규 고정.
- 잔여 리스크 + M7/M8 전승 메모 갱신 (특히 user_model_id 반영 타이밍 계약).
```

---

## 세션 컨텍스트 메모 (참고용, 프롬프트 외부)
- **H1이 맞는지부터 확인**: 7b171a0의 ISCC loud-fail 실증은 '파일명 rename → 컴파일 실패'만 증명했고, **정상 경로에서 Check 함수가 런타임에 True를 반환하는지**는 실증하지 않았다. 컴파일 성공+설치 후 파일 부재 조합이 가능하므로 디스크 실측이 1순위. (`ShouldInstallEngineWorker`의 경로 전개 기준을 `git show 7b171a0`과 setup.iss L1135 부근에서 확인할 것.)
- 18:39 셋업에 담긴 i18n 문구("...재설치하면 로컬 엔진을 복구할 수 있습니다...")가 사용자에게 실제로 보였다는 것 = 모달화(ea42bb6)와 2c 문구(7b171a0)는 실기기에서 동작 확인. 즉 UX 개선분은 유효하고, **근본 서빙 결함만 잔존**.
- 항목4 관련 44c0fbe/102591e의 의도된 한계("부팅 시 캐시 → 유휴 재기동 후 반영", "등록 후 로컬LLM 선택")가 사용자 기대와 정면 충돌. P3 설계에서 정책 자체를 뒤집을 것: 선택 = 즉시 적용 + 엔진 상태 독립 항목화. v1 동결 wire 제약은 유지되므로 호스트 측 반영 시점(설정 저장 통지/new session 개시 시 재검)을 프로토콜 무접촉 범위에서 설계.
- push 대기 20건. 이번 세션 커밋도 누적되므로 마감 시 사용자에게 push 일괄 승인 받을 것.
