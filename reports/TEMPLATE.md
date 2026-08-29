# [보고서 제목 / Incident Title]

> **작성 메타데이터**
> - **작성 모델 (Author Model)**: `{{MODEL_NAME}}`
> - **작성 일시 (Timestamp)**: `{{YYYY-MM-DD HH:mm:ss (KST)}}`
> - **분석 대상 (Target)**: `{{PID / Process Name / Feature Name}}`
> - **핵심 요약 (Executive Summary)**:
>   - `{{보고서의 핵심 발견점, 장애 또는 연구 결과의 1~2줄 요약}}`
>   - `{{핵심 원인 및 취해진 조치 / 권고안}}`

---

## 📌 본문 (자율 구성 영역)

<!-- 
이 영역은 분석 유형(프리징/크래시 조사, 리버스 엔지니어링, 성능 병목 계측 등)에 맞추어 
작성 모델이 목차와 서식을 자율적으로 유연하게 구성합니다.
권장 섹션 예시:
1. 장애 현상 및 환경 (Incident Context)
2. 수집된 로그 및 추적 파일 (Collected Artifacts & Logs)
3. 스레드 / 콜스택 심층 분석 (Callstack & Thread State Analysis)
4. 근본 원인 분석 (Root Cause Analysis)
5. 재발 방지 대책 및 조치 사항 (Mitigation & Fixes)
-->

### 1. 개요 및 장애 환경

### 2. 수집된 아티팩트 및 로그 분석

### 3. 콜스택 및 덤프 정밀 분석

### 4. 근본 원인 (Root Cause)

### 5. 해결 방안 및 권고사항

---

## 💡 후속 세션 참고용 디버깅 힌트 (Debugging Hints & Insights)

> ⚠️ **안내**: 아래 항목들은 절대적인 규칙이 아니며, 과거 실제 라이브 인젝션 및 프리징/크래시 디버깅 과정에서 축적된 유용한 **경험적 힌트(Insight)**입니다. 문제 해결 시 참고 자료로 활용하세요.

1. **MO2 가상화 환경(VFS) 로그 및 크래시 파일 위치**:
   - 게임 엔진이 자체 생성하는 크래시 리포트 텍스트(`Cyberpunk2077.exe-YYYYMMDD-HHMMSS-<PID>-<TID>.txt`)는 게임 폴더 대신 MO2의 가상화 오버라이트 경로(`C:\CYBERPUNK_ARK_PACK_MO2\overwrite\bin\x64\` 등)에 직접 쓰일 수 있습니다.
   - 프로세스가 파일 핸들을 독점(`Exclusive Lock`)하고 있어 일반 읽기가 실패할 경우, Win32 `CreateFileW`의 `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE` 플래그를 이용해 파이썬/스크립트로 안전하게 읽을 수 있습니다.

2. **SEH(`__try/__except`)와 게임 엔진 VEH의 상호작용 주의점**:
   - 코드에 `__try / __except (EXCEPTION_EXECUTE_HANDLER)`를 둘렀더라도, 메모리 위반(`0xC0000005`)이 발생하는 순간 **1st-chance 예외 단계에서 게임 엔진 자체의 VEH(Vectored Exception Handler)나 크래시 리포터가 먼저 반응**합니다.
   - 엔진 VEH가 크래시 덤프 매니페스트를 작성하고 프로세스를 중단/대기 상태로 만들면 `__except`로의 정상 복구가 무의미해지므로, RTTI 역참조나 컴포넌트 접근 전 **유효 유저모드 포인터 검증(`uintptr_t >= 0x10000 && uintptr_t <= 0x00007FFFFFFEFFFF`)**을 선행하는 것이 안전합니다.

3. **라이브 프로세스 미니덤프 및 스택 심볼화 기법**:
   - 프리징 상태로 살아있는 프로세스는 `dbghelp.dll`의 `MiniDumpWriteDump` API를 파이썬 ctypes로 호출하여 즉시 미니덤프(`.dmp`)로 보존할 수 있습니다.
   - 빌드 디렉터리에 존재하는 PDB(`cp2077_trainer.pdb`)와 `dbghelp`의 `SymFromAddrW`, `SymGetLineFromAddrW64`를 조합하면 정적 덤프 및 RVA 주소를 소스 코드 라인 단위로 정확히 역추적할 수 있습니다.

4. **엔진 OnTick 스레드와 렌더 스레드 간 데드락 패턴**:
   - REDengine의 렌더 스레드(`Present`) 및 다수의 워커 스레드는 메인 엔진 틱(`OnTick`)의 상태 신호(`SRWLock`, `ConditionVariable`)를 지속적으로 동기화 대기합니다.
   - 메인 틱이 멈추거나 예외 처리 루프로 빠지면 전체 스레드가 `ZwWaitForAlertByThreadId` / `SleepConditionVariableSRW`에서 영구 정지하여 하드 프리즈로 이어집니다.
