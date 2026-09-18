# Emebala Engine v2 — 신규 세션용 시작 프롬프트 3종

> 계획 문서(공통): `D:\OneDrive\Projects\Emebalachat\plans\emebala-engine-host-shared-inference.md`
> (v1.0 동결 계약 + 하단 v2.0-draft. §V2-14의 6개 질문은 2026-09-18 전부 결정 완료.)
> 아래 3개 프롬프트는 각 워크스페이스의 새 세션에서 그대로 붙여 넣어 사용한다.

---

## 1) Emebala Chat 세션용 (마일스톤 M6: 오케스트레이터 v2 + ggml-translate 워커)

````
Emebala Chat 워크스페이스에서 "공용 추론 호스트 v2 (오케스트레이터)" 구현 세션을 시작한다.

## 계획 문서 (반드시 먼저 전부 읽기)
D:\OneDrive\Projects\Emebalachat\plans\emebala-engine-host-shared-inference.md
- v1.0 전체(특히 §4 동결 계약 — 임의 변경 금지)와 하단 "v2.0-draft" 전체.
- 문서 충돌/모순/누락 발견 시 임의 해석 말고 사용자에게 보고하고 멈춘다.

## 현재 상태
- v1 호스트(Emebala.Engine.exe) 동작·검증 완료: 파이프+토큰+FIFO 번역, 설치/스폰/
  폴드백, 설치자 통합 릴리스(0.10.1). v2 결정사항이 문서에 반영돼 있다(§V2-14).

## 이번 세션 범위 = 마일스톤 M6 (문서 §V2-13)
1. 오케스트레이터 v2: v1 호스트를 확장해 세션 모델(§V2-4.3)·멀티세션 스케줄러
   (priority/drop_eligible/deadline, §V2-4.6)·워커 생명주기 관리(§V2-3) 추가.
   v1 프로토콜(§4) 지원은 그대로 유지(§V2-4.7).
2. 파이프 이름: 버전 무관 `\\.\pipe\emebala-engine`을 정식 이름으로,
   `\\.\pipe\emebala-engine-v1`은 전환기 별명으로 동시 생성(§V2-4.1).
3. ggml-translate 워커 분리: 호스트 안의 번역 로직(translation_common/
   LocalInferenceEngine)을 `Emebala.Engine.ggml-translate.exe`(llama.cpp b6099
   고정) 자식 프로세스로 분리. 오케스트레이터↔워커 난부 계약(§V2-3) 구현.
4. 모델 레지스트리 + 서명 매니페스트 인프라(§V2-5, §V2-6): registry.json 읽기,
   manifest.json 서명 검증 인터페이스. 인증서 미확보 개발 기간은 설치기가 쓴
   매니페스트를 신뢰하는 개발 모드로 동작하되, 인터페이스는 Authenticode 검증으로
   교체 가능하게 설계.
5. 수리 부트스트래퍼(§V2-8.3): Chat 기동 시 필수 컴포넌트 부재 검사 → 버전
   매니페스트 URL로 무음 복구. 오프라인 시 로컬 번역 비활성 + 사용자 안내.
   단, 매니페스트 호스팅 URL은 §V2-14 잔여 사항으로 **아직 미확정(운영 결정)** —
   URL은 config/컴파일 상수 주입점(placeholder)으로 두고 임의의 실제 URL을
   만들지 않는다. URL 없이는 복구 다운로드가 조용히 실패하고 안내 UX로 수렴해야 한다.
6. components.json 도입(§V2-8.1) + 설치기의 컴포넌트 번들 분할(§V2-8.2).
7. Chat 앱 임베디드 엔진 제거(§V2-8.6): engine.cpp의 임베디드 경로와 그
   라우팅/마이그레이션 코드 제거(호스트 빌드는 유지). 실패 UX = 수리 → 클라우드
   (동의) → 비활성 안내. **버전은 0.10.1 고정 — 범프 금지** (2026-09-18 사용자
   결정, §V2-8.6 갱신 반영). CMakeLists/setup.iss/README의 버전 값은 그대로 두고,
   제거된 임베디드 폴드백을 전제해 문서만 수정한다(9항).
8. 신뢰성 지표 health 노출(§V2-8.6) + 단위 테스트 대폭 추가(세션/스케줄러/워커
   계약/레지스트리/매니페스트).
9. 문서/체인저로그 동기화(버전 0.10.1 고정 전제):
   - CHANGELOG.txt v0.10.1 항목을 새 동작으로 수정 — 현재 "임베디드 폴드백"을
     소개하는 문구(25–37행)가 제거 이후 사실을 반영하도록 재작성. 별도 버전
     섹션 추가 없음.
   - README.md: 임베디드 폴드백 서술(§Features L56, engine_host 섹션 L95/L246,
     config `model_path` 설명 L234 등)과 테스트 수치 문구를 실제 구현 결과와
     맞게 갱신. AGENTS.md의 테스트 수치도 함께 맞춘다.
   - 설치기: 신규 워커/오케스트레이터 바이너리 번들 + components.json 초기화.
     버전 상수는 0.10.1 유지.

## 규칙
- 다른 워크스페이스(Emebala, Emebala_Listner)는 읽기 전용, 수정 금지.
- v1 §4 계약과 v2 §V2-14 결정을 임의 변경 금지. 문서에 없는 선택지가 나오면
  사용자에게 보고.
- 보안/프라이버시 원칙 유지: shape-only 로그 기본 OFF, 사용자 텍스트 로깅 금지,
  SetDllDirectoryW(L""), delay-load, 파이프 ACL/토큰.
- /W4 경고 0, C++20, RAII, 변경 주석에 REQ-043 태그.
- git commit/push 금지(작업 트리만). installer 수정 시 게이트 2종
  (tools/check_installer_encoding.py, tools/check_installer_display_text.py)
  통과 필수.

## 완료 판정
- 빌드 0 경고, run_tests 전체 통과(세션 시작 시 측정 baseline 무회귀 + 신규 추가 —
  문서 기록상 최신 수치는 2,863 [docs/260917_0001 세션 보고서], M2 호스트 세션
  이후 실제 수치는 세션 시작 시 재측정해 기준선으로 고정), 스모크 갱신(세션/
  워커 재스폰 케이스; ASR 스트림은 M7 소관이라 오케스트레이터 세션 프레임
  목업 테스트로 대체), v1 클라이언트 호환 확인(hello.protocol=1로 translate),
  게이트 2종 PASS, ISCC 성공(`Emebalachat_Setup_0.10.1.exe` 산출).
- CHANGELOG.txt v0.10.1 섹션이 임베디드 제거 후 사실을 반영하는지 확인.
- 보고서: 변경 파일 목록, 테스트 수, 미완료/리스크, Listener(M7)/Reader(M8)
  세션에 전할 메모(워커 병합 방식 제안 포함).
````

---

## 2) Emebala Listener 세션용 (마일스톤 M7: 스트리밍 ASR + ggml-asr 워커 + 호스트 전용 재탄생)

````
Emebala Listener 워크스페이스에서 공용엔진 v2 이식 세션을 시작한다.
Listener는 미공개 앱이므로 "호스트 전용"으로 재탄생하는 것이 목표다.

## 계획 문서 (반드시 먼저 전부 읽기)
D:\OneDrive\Projects\Emebalachat\plans\emebala-engine-host-shared-inference.md
- v1.0 §4(동결 계약) + 하단 v2.0-draft 전체. 특히 §V2-4(스트리밍 프로토콜)·
  §V2-3(워커)·§V2-5(레지스트리·경로 흡수)·§V2-8(배포)·§V2-10(업그레이드 게이트)·
  §V2-14(결정 완료).
- Chat 레퍼런스 C++ 클라이언트 읽기 허용: D:\OneDrive\Projects\Emebalachat\src\
  engine_host_client.hpp / .cpp
- 모순/누락 시 임의 해석 금지, 사용자에게 보고.

## 현재 상태 (이 워크스페이스)
- C++20/Win32: WASAPI loopback 캡처 → ASR(Nemotron-3.5-ASR-Streaming-0.6B Q8_0,
  transcribe.cpp handy-computer 핀 + PR22836 패치) → 번역(Hy-MT2, llama.cpp
  74ce157, temp 0.7) → HUD 자막/TTS(SAPI). 추론은 전부 인-프로세스 정적 링크,
  CPU 전용 빌드. 모델: %LOCALAPPDATA%\EmebalaListener\models. 공용엔진 미연동.

## 이번 세션 범위 = 마일스톤 M7 (§V2-13) + 호스트 전용 재탄생
1. 임베디드 추론 제거(§V2-8.6): 프로세스 내 llama.cpp·transcribe.cpp 의존과
   추론 코드를 모두 걷어내고 공용엔진 클라이언트 호출로 대체. CMake의 추론
   ExternalProject/FetchContent와 관련 코드·테스트 정리.
2. C++ v2 클라이언트 이식: Chat의 engine_host_client.hpp/.cpp를 이 워크스페이스에
   맞게 이식·확장(스트리밍 세션 API 포함). 계약 근거는 문서 §V2-4.
3. ggml-asr 워커(§V2-3): transcribe.cpp 핀 + PR22836 패치를 유지해
   `Emebala.Engine.ggml-asr.exe`로 분리 빌드. Chat 워크스페이스 파일 수정 금지 —
   워커 소스·CMake는 이 워크스페이스에 두고, Chat 세션과의 병합 방식은 사용자와
   확인.
4. 스트리밍 ASR 세션: §V2-4.4 확정 규격(JSON 메타 + 바이너리 PCM 16kHz mono
   s16le)으로 파이프라인 재연결. onPartial/onFinal 계약 그대로 유지.
5. 백프레셔 이전: 기존 "MT 큐 오래된 것 폐기" 정책을 priority/drop_eligible/
   deadline 파라미터로 호스트 계약에 이전(§V2-4.6).
6. 번역 품질 수렴: 자체 sampling(temp 0.7 등) 폐기 → Chat 기준 프로파일
   (§V2-5.2) 사용. 자체 골든셋을 §V2-10 게이트에 추가.
7. 모델 경로 흡수(§V2-5.3): %LOCALAPPDATA%\EmebalaListener\models 의 두 모델을
   공용 레지스트리 등록. 재다운로드 금지.
8. 설치기(setup.iss): 공용엔진 컴포넌트 번들 규칙 적용(§V2-8.1/8.2). 인코딩/
   표시 게이트 통과.
9. GPU: 현재 CPU 전용 — ggml 워커의 CUDA 오프로드는 Chat 게이트(§V2-10) 결과를
   따름. 이번 세션은 CPU 동작 보장 + GPU 시 활성화 구조만.

## 규칙
- Emebalachat·Emebala 워크스페이스는 읽기 전용.
- 지연 계약 실측 통과가 완료 조건: 청크 <100ms, E2E <2s (기존 파이프라인
  스펙). 실패 시 병목 분석을 보고서에 포함.
- v1 §4 동결 계약·v2 결정사항 임의 변경 금지. 문서에 없는 선택지는 사용자 보고.
- shape-only 로그, 사용자 텍스트 로깅 금지, git commit/push 금지.

## 완료 판정
- 공용엔진(또는 문서 §V2-4 기준의 자체 목업 호스트 — Chat M6 산출물 미완 시
  허용)으로 ASR+번역 HUD까지 E2E 동작.
- ctest 전체 통과, 지연 실측 보고서, Chat 세션에 전달할 워커 병합 노트.
````

---

## 3) Emebala Reader 세션용 (마일스톤 M8: Rust 클라이언트 + ct2/onnx 워커 + 호스트 전용 재탄생)

````
Emebala (Reader) 워크스페이스에서 공용엔진 v2 이식 세션을 시작한다.
Reader는 미공개 앱이므로 "호스트 전용"으로 재탄생하는 것이 목표다.

## 계획 문서 (반드시 먼저 전부 읽기)
D:\OneDrive\Projects\Emebalachat\plans\emebala-engine-host-shared-inference.md
- v1.0 §4(동결 계약) + 하단 v2.0-draft 전체. 특히 §V2-4(세션/스트리밍)·
  §V2-5(레지스트리·경로 흡수)·§V2-7(카탈로그/컴퓨터 초보자 원칙)·§V2-9(러스트
  클라이언트)·§V2-10(업그레이드 게이트)·§V2-14(결정 완료).
- Chat 레퍼런스 C++ 클라이언트 읽기 허용(계약의 다언어 재구현 근거):
  D:\OneDrive\Projects\Emebalachat\src\engine_host_client.hpp / .cpp
- 모순/누락 시 임의 해석 금지, 사용자에게 보고.
- 본 워크스페이스의 기존 ADR(공유 추론 서버 연기)은 트리거가 충족돼 해소 대상임을
  전제한다.

## 현재 상태 (이 워크스페이스)
- Flutter(Dart) UI + Rust 코어(native/emebala-ai, emebala-ffi.dll). 추론 인-
  프로세스: llama-cpp-2 0.1.150(Hy-MT2 등 GGUF 번역), ct2rs 0.9(M2M100 int8),
  ort 2.0-rc + DirectML(Kokoro-82M TTS, PP-OCRv6). WinRT OCR 병용.
- 모델: 카탈로그 assets/models_manifest.json(9종, 해시검증/진행률/취소) + 임의
  로컬 .gguf 지정. 저장: %LOCALAPPDATA%\Emebala\models. LLM↔CT2 상호배타
  상태머신 + 90s 가버너 + foreground 리스(번역/TTS/OCR 직렬). 설치기 없음(포터블).
  OpenAI 호환 클라우드 클라이언트는 엔진 범위 외.

## 이번 세션 범위 = 마일스톤 M8 (§V2-13) + 호스트 전용 재탄생
1. Rust 레퍼런스 클라이언트 신규 작성(§V2-9): 동결 계약 §4/§V2-4의 Rust 구현
   (보안 파이프 연결, 토큰, hello/welcome, 세션/이벤트, 폴드백 매트릭스). C++
   클라이언트를 근거로 바이트 호환 유지. flutter_rust_bridge 래핑과 분리해 단위
   테스트 가능하게.
2. 인-프로세스 추론 제거(§V2-8.6): llama-cpp-2/ct2rs/ort 의존과 번역·TTS·OCR
   추론 코드를 호스트 호출로 대체. FFI API(Flutter 쪽 호출면)는 최대한 유지해
   UI 무수정이 목표 — 불가능한 시그니처만 frb 갱신.
3. ct2/onnx 워커(§V2-3): CTranslate2·ONNX Runtime(+DirectML EP 체인) 워커를 이
   워크스페이스에서 분리 빌드. ggml와 별도. Chat 세션과의 병합 책임은 사용자와
   확인.
4. 생명주기 정책 이전: LLM↔CT2 상호배타·가버너·foreground 리스를 워커/스케줄러
   규칙으로 재구현하고 UI 동작 변화가 없음을 검증(§V2-3).
5. 카탈로그 규약 정합(§V2-7): 기존 models_manifest.json을 공용 카탈로그 규격으로
   정렬(표시명/기본값/해시). 다운로드 완료 시 공용 레지스트리 register_model로
   등록.
6. 경로 흡수(§V2-5.3): %LOCALAPPDATA%\Emebala\models 의 기존 모델을 공용
   레지스트리 등록(재다운로드 금지). Emebala\(앱 데이터) ↔ Emebala\Common\
   (가족 공용) 소유권 경계 정리.
7. 품질 게이트(§V2-10): TTS(Kokoro 음성 품질)·OCR 정확도·번역 기준선을 각
   워커 게이트에 추가.
8. WinRT OCR은 앱 잔존(§V2-14 #3 결정). PP-OCRv6만 onnx 워커로.
9. 설치기 신규(Inno, §V2-8): orchestrator+ggml-translate+ct2+onnx+기본 모델
   컴포넌트 번들. Chat의 installer 규약 참고(읽기 전용).

## 규칙
- Emebalachat·Emebala_Listner 워크스페이스는 읽기 전용.
- 사용자 텍스트 로깅 금지, 동의 없는 네트워크 전송 금지(백엔드는 기존 인증/
  동기화 범위만).
- 문서에 없는 결정이 필요하면 사용자에게 보고. git commit/push 금지.

## 완료 판정
- 공용엔진(또는 문서 §V2-4 기준의 자체 목업)으로 번역·CT2·TTS·OCR까지 E2E
  동작. UI 동작 변화 없음(상태머신/가버너 검증 보고).
- Rust 클라이언트 단위 테스트 + C++ 클라이언트와 골든 케이스 상호운용.
- 경로 흡수 마이그레이션 테스트, cargo test 통과.
- Chat 세션에 전달할 워커 병합 노트.
````
