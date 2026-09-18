# Emebala Chat — 코드 품질 감사·압축 세션 시작 프롬프트 (REQ-044 예정)

> M6(공용 추론 호스트 v2) 완료 직후 시점 기준. 새 세션에 그대로 붙여 넣는다.

---

```
Emebala Chat 워크스페이스(d:\OneDrive\Projects\Emebalachat)에서 "코드 품질 감사 + 기능 보존 압축" 세션을 시작한다.

## 목표 (사용자 원문 의도)
"emebala chat의 코드들이 수준 높은 코드상태인지, 기능을 전부 그대로 유지하면서 압축할 부분이 있는지. 나는 매우 우수한, 아무 문제 없는 코드수준을 원한다."
= ①현재 코드 수준을 객관적으로 감사 ②기능 동등성(무회귀)을 수학적으로 보장하면서 중복·dead code·복잡도를 압축 ③남은 부채를 제로에 가깝게.

## 현재 상태 (2026-09-19 기준, 이 사실들을 출발점으로 사용)
- M6 완료·마감 검증 PASS: 오케스트레이터 v2(세션/스케줄러/워커 매니저/health) + ggml-translate 워커 분리 + registry/manifest/components 파서 + 부트스트래퍼(URL placeholder, 비활성 배포) + 임베디드 엔진 제거. 버전 0.10.1 고정.
- 테스트: run_tests **3,388 checks / 0 failures** (baseline — 전 과정의 무회귀 기준선). 스모크 tools/e2e/m6_orchestrator_smoke.py 11 probes PASS.
- 직전 수정: d5e0dae — WorkerManager::ShutdownAll 자기-데드락(0xc0000409) 해소. 로컬 커밋 8개 미push(사용자 승인 대기일 수 있음 — 세션 시작 시 git log/status로 상태 재확인하고, push는 사용자 지시가 있을 때만).
- 세션 보고서 일체: docs/260918_0001_session_engine-host-v2-orchestrator-m6/ (리서치→설계→T1~T8→감사→마감검증 25여 건). 그중 234330(코드 표면)과 235200(아키텍처)이 가장 신선한 지도.
- 계획/동결 문서: plans/emebala-engine-host-shared-inference.md (v1 §4 동결 + v2.0-draft §V2-14 결정).

## 이미 알려진 기술 부채 (감사에서 우선 처리 후보 — 발견을 재확인하고 확장하라)
1. VERBATIM 사본 동기화 부채: src/engine_host_protocol.hpp의 JSON 파서/프레임 코드가 src/engine_host_client.cpp L57-318에 복제(타 워크스페이스 베어복사용 자기완결 계약 — 설계 D-3). 사본 동기화 테스트의 실효성 감사 + 계약 유지 하에 부채 최소화(단일화 불가 전제).
2. kExpectedModelSha256 3중 관리: engine_core(이동됨) / engine_host_client.hpp 사본 / installer/setup.iss — 라운드트립 검증 테스트 유무 감사.
3. 공용 경로 상수 중복: host_main.cpp vs engine_host_client.cpp (registry.json/매니페스트 위치 등 신규 상수 포함).
4. tests/: run_tests.cpp(약 1.4만 행 단일 파일) + m6_engine_host_*.inc 6종 include 구조 — 빌드 순서/등록부 충돌 없이 통합 유지되는지 감사. 통째 재작성은 금지(surgical만).
5. i18n 37개 테이블 집계 초기화(필드 순서 계약) + 최근 추가된 Repair* 3필드 — 구조적 오탈자防线 감사.
6.注释 REQ 태그 역추적 가능성(REQ-001~043)과 유휴 주석/죽은 주석 정리.

## 감사 기준선 (무엇을 "우수한 코드"로 볼지)
- 정확성: 동결 계약(§4/난부 §V2-3) byte-level 준수, 실패 매트릭스(§5.4/§V2-11) 전 경로 수렴, 락·원자성·스레드 종료 시퀀스(직전 데드락 같은 유형 전수 재점거: 뮤텍스 중 콜백, join 전 세션 close 패턴).
- 안전: shape-only 로그(사용자 텍스트 0), 파이프 ACL/토큰(파일링 포함 상수시간 비교), SetDllDirectoryW/delay-load, https-only + 해시 검증, 동의 게이트 불변.
- 견고성: 예외 경계(no-throw 경계 명확), 자원 RAII(핸들 누수 0 — 종료 시나리오 포함), 오버플로/경계값( 길이/카운터/시간).
- 가독성/일관성: snake_case 파일·PAIRE, include 그룹 주석, req 태그 규율, 함수 복잡도(과대 스레드/상태머신 중복).
- 압축 여지: 중복 로직 함수화, dead code(제거된 임베디드 잔향 포함), 불필요한 전역, 템플릿 남용, 주석화 과거 코드, 재실행 산출물의 git 혼입 여부.

## 절차 (P1→P8 VP 파이프라인 준수, 🟣 판정 시 full)
P1 이 프롬프트로 체크리스트 작성(docs/YYMMDD_NNNN_session_code-quality-audit-compression/requirement-checklist.md) + 복잡도 분류.
P2 project-research: VibeZoo review_code/analyze_call_graph/extract_patterns + 코드베이스 grep으로 부채 목록 갱신(라인 포인터 포함).
P3 architect: 압축/리팩터 설계 — 각 항목을 (A) 기능 동등 자명 (B) 테스트 필요 (C) 구조 변경 으로 분류, cross-file 매핑과 직렬화 계획. Ask Light Gate + Debug Tech Gate 필수.
P4 구현: 항목 1건 = 1위임(같은 파일 병렬 금지). 모든 변경은 "기능 동등성 증명"(관련 단위 테스트 무수정 통과 or 재설계 근거 명기)과 함께. 통째 파일 재작성 금지, apply_diff surgical만, 착수 전 접촉 파일 백업.
P5 기술 리뷰(debug 또는 architect) + quality-gate(빌드/warning 0, run_tests 3,388 기준 무회귀+신규, 스모크, 설치기 건드릴 시 게이트 2종+ISCC).
P6 Final Ask Audit(전 항목 ✅), P7 VP 독립 리뷰, P8 보고서.

## 규칙
- 기능·프로토콜 동작 변경 금지: 사용자 눈에 보이는 번역 UX, 동결 §4, 난부 계약, v1 하위호환, 0.10.1 버전 고정.
- 타 워크스페이스(Emebala, Emebala_Listner)는 읽기 전용.
- git commit/push 금지(로컬 커밋·push는 완료 판정 후 사용자 승인 하에 VP만).
- 보안/프라이버시 원칙 절대 양보 금지(로그/파이프/동의/DLL 로드/URL).
- /W4 경고 0, C++20, RAII, 변경 주석에 REQ-044 태그.
- 테스트 삭제·단순화(약화)는 어떤 압축의 대가라도 금지. 죽은 테스트 제거(대상 코드 소멸 시)만 허용, 근거 명기.
- 압축 기준: 줄 수 감소 자체가 목표 아님 — '동일 기능·명확한 책임·낮은 결합·줄어든 부채'로 판정. 애매하면 보존.

## 완료 판정
- 감사 보고서(수준 평가: 항목별 등급 + 근거 라인) + 압축 실행 보고서(전/후, 동등성 증명 표).
- run_tests 전 과정 무회귀(3,388 → 신규로 증가만), 스모크·게이트·ISCC(설치기 접촉 시) PASS.
- 알려진 부채 6종 각: 해소/축소/보존(근거) 종결 상태.
- 미해결 잔여 리스크 목록 + M7(Listener)/M8(Reader)에 전승할 계약 메모.
```

---

## 세션 컨텍스트 메모 (참고용, 프롬프트 외부)
- 압축 후보가 VERBATIM 사본·동결 계약과 충돌할 수 있으므로 P3 설계 단계에서 "계약 영역 표"(무접촉 파일 목록)를 먼저 못 박고 착수.
- 직전 0xc0000409 사례가 보여주듯, 종료/생애주기 코드의 "락 중 콜백" 패턴은 grep(`lock_guard`/`mu.lock` 블록 내 함수 호출)으로 전수 재점거하면 같은 등급 결함을 더 찾을 가능성이 있음 — P2 우선 조사 항목.
- 셋업 파일: installer/output/Emebalachat_Setup_0.10.1.exe (07:32 생성본, d5e0dae 반영). 품질 세션에서 설치기 접촉 시 반드시 게이트 2종+ISCC 재실행.
