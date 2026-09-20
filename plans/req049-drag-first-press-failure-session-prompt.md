# REQ-049 세션 프롬프트 — 드래그 번역 '첫 실행 실패 / 이전 결과 표시'根治 + 실기기 실전 검증

## 최우선 입력 문서 (필독 순서)
1. `docs/260921_0001_session_req049-drag-first-press-failures/001000_handoff-req048-to-req049-drag-first-press.md` ← 모든 사실·가설·코드 지도·운영 메모
2. `docs/260920_0001_session_req048-device-round2-engine-lifecycle/092000_session-req048-r2-feedback-round.md` · `071000_session-summary-req048-device-round2.md` (선행 세션 맥락)
3. `plans/req048-device-round2-shared-engine-lifecycle-session-prompt.md` §규칙 (계약 계승)

## 목표 (사용자 관찰 원문 — 260921 00:05)
> "연달아 드래그 > 플로팅버튼 > 번역을 누륾면, 이전의 번역을 보여주거나, 모델이 없다고 실패하지만, **다시 번역을 누른면 번역이 되거든?**"

**REQ-049 목표**: 첫 실행이 반드시 성공하도록 콜드스타트/레이스 근본을根治하고, **실기기에서 실전 테스트를 수행하며** 검증한다. (이전 결과 표시·'파일 없음' 문구는 REQ-048 R4에서 이미 처리됨 — handoff §2 확인, 재작업 금지.)

## 전제 사실 (시작 시 재확인 — git/dir/실측)
- HEAD ff9e3c5 (REQ-048 14커밋). push 미실행, 대기 48건.
- run_tests 기준선 **3,861/0** (97 i18n 필드). 빌드 0경고.
- 서빙 스택 건강이 **이미 증명됨** (워커 직접 4/4, 오케스트레이터 종단 7/7) — 엔진/모델/해시 캐시 의심 금지. 남은 결함은 **첫 실행 경로의 타이밍/레이스**.
- 사용자 config `engine_type: "local"`, 공용 스토어 엔진 `0.10.1.r2`, `.sha256ok` 마커 존재.
- 최우선 가설 **H1: 오케스트레이터 콜드 부팅 대비 연결 예산(3×500ms) 부족** (handoff §3).

## 절차 (P1→P8, 사용자 무개입 자율 — 단 실기기 GUI 실측 시 사용자에게 설치/재현 협조 요청 가능)

- **P1**: `docs/260921_0001_session_req049-drag-first-press-failures/requirement-checklist.md` — 원문 인용 승격 + 하위 검증항목 + 신규 발견 규율("발견하면 전부 수정").
- **P2/B2(진단, 실전 테스트 병행)**: ① H1 실험 — 앱 종료 후 엔진 프로세스 전체 종료 → 자체 오케스트레이터 격리 스폰(REQ-048 R4 클린 프로브 패턴)으로 **첫 연결의 소요 시간·실패 여부** 정확 측정, `engine_host_client` 연결/스폰 예산 소스 확인. ② 실기기 — 사용자에게 R4 셋업(00:02, SHA 7371fd83…) 설치 + 재현 요청, DIAG 로그(shape-only)로 첫 클릭의 실패 지점 확인(`MAIN/DragIconClick/00N`, `ENGINEHOST/*`, `ENGINE/Translate/044`). ③ H3(캡처 레이스) 판별. 각 항목 검증보고서.
- **P3**: 최소 수정 설계 (연결 예산/재시도/프리스폰 중 최소 치 + 부수 효과 검토). Ask Light Gate + Debug Tech Gate.
- **P4**: 1건=1위임 직렬. 접촉 매트릭스: engine_host_client.cpp 단독 가능성 높음(main.cpp 공용 시 직렬). 매 위임: **실제 빌드 타임스탬프 확인 후** 테스트, run_tests 무회귀, 커밋 후 다음.
- **P4-final**: 게이트 전종(빌드 0경고·run_tests·인스톨러 2종+uninstall 계약·model-sha-sync·m6 스모크·동결 diff 0·충돌 마커 0) + **최종 셋업 재생성**(SHA/타임스탬프, `.prev_*` rename).
- **P5~P7**: debug 기술리뷰(전 diff) → ask Full Audit → VP 독립 리뷰 (zero-tolerance).
- **P8**: ① 본 세션 생성 tools_tmp_* 휴지통 정리(기존 선행 세션 잔존물은 무 touch) ② 문제패널 0 증명 ③ 커밋 완료(트리 클린, plans/handoff 문서 커밋 — docs는 gitignore) ④ 셋업 SHA 보고 ⑤ push 대기 목록 갱신(승인 시에만).

## 실전 테스트 규정 (이번 세션의 핵심)
- 대화형 데스크톱(사용자 기기)에서 `python tools/e2e/req027_e2e.py` 계열 + 수동 재현을 **수정 전(재현 확정) → 수정 후(재현 소멸)** 양쪽에 수행하고 결과를 검증보고서에 기록.
- DIAG 활성화(shape-only)는 허용; `diag_log_content`는 사용자 명시 동의 시에만.
- 라이브 프로브는 앱 실행 중이면 v1 id 충돌 오염 가능 — 반드시 격리(앱 종료 후 프로브, 또는 자체 오케스트레이터 스폰).
- 자동으로 못 잡는 항목은 🔶 이연 명문화 — **이연을 PASS로 포장 금지**.

## 규칙 (REQ-044~048 계승 — 무접촉 동결)
protocol/client VERBATIM·worker_protocol·v1 파이프명·EXTRA_INFO_MARKER·SetDllDirectoryW·delay-load·SHA-256 검증·shape-only 로깅·상수시간 비교·privacy 게이트·Hy-MT2 'local' 불변·C1 게이트·OpenAI 보안 경계·REQ-047 5종(unins000 고정명·drain-owning latch·bundled 등록 거부·WM_APP+0x500·라디오 ID 2010~2014)·REQ-048 계약(uninstall 계약 게이트 7 checks·WMI LIKE 'Emebala%' 핀·17+2개 최신 문자열 37개 로케일 영어-동일 하드 게이트·엔진 리비전 ENGINE_REVISION).
- 실환경 파일(설치본·모델·registry·config) 진단 외 수정 금지. 파괴적 언인스톨 금지.
- 테스트 약화 금지·apply_diff surgical·REQ-049 태그 주석·/W4 0·C++20·RAII.
- i18n 추가 시 37개 언어 실번역(영어 placeholder 금지 — 사용자 명시) + 필드 수 미러/게이트 갱신.
- **빌드 후 반드시 바이너리 타임스탬프로 실재 rebuild 확인**(REQ-048 교훈).
- git commit 항목별 VP만. push 사용자 승인 시만.
- 언어: 응답 한국어.

## 완료 판정
- 첫 실행 실패의 원인 진단서(가설 채택/기각 + 측정치) + 최소 수정 diff + 신규 테스트.
- 실기기 실전 검증: 수정 전 재현 확정 → 수정 후 재현 소멸(검증보고서).
- 자동 게이트 전종 green + 셋업 최종 재생성 SHA 기록 + run_tests 무회귀.
- 잔여 실측 이연 목록 정직 표기 + push 대기 갱신 보고 + 임시파일 정리·트리 클린.
