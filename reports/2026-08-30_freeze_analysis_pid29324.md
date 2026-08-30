# Cyberpunk 2077 라이브 프로세스(PID 29324) 프리징 및 크래시 원인 분석 보고서

> **작성 메타데이터**
> - **작성 모델 (Author Model)**: `Gemini 3.7 Flash (High)`
> - **작성 일시 (Timestamp)**: `2026-08-30 00:12:18 (KST)`
> - **분석 대상 (Target)**: `Cyberpunk 2077 (PID: 29324) / cp2077_trainer.dll`
> - **핵심 요약 (Executive Summary)**:
>   - 게임 메인 틱(`OnTick`, TID 14916) 중 `FindAttitudeAgent`에서 RTTI 타입 검사(`IsClassOrDerived`) 시 유효하지 않은 포인터(`0xFFFFFFFFFFFFFFFF`)를 역참조하여 `0xC0000005 (EXCEPTION_ACCESS_VIOLATION)` 발생.
>   - `ReadHostility`에 `__try/__except`가 있었으나, 1st-chance 단계에서 REDengine 자체의 VEH(Vectored Exception Handler)가 먼저 개입하여 크래시 덤프 매니페스트를 작성하고 메인 스레드를 정지시킴으로써 게임 전체가 하드 프리즈됨.

---

## 📌 본문

### 1. 개요 및 장애 환경

- **프로세스 ID**: `29324` (`Cyberpunk2077.exe`)
- **장애 발생 시점**: `2026-08-30 00:06:39.618 (KST)`
- **증상**: 게임 화면 및 사운드가 완전히 멈추고 입력을 받지 않는 하드 프리징 발생 (프로세스는 백그라운드에 9.09GB 점유 상태로 유지).
- **실행 환경**: MO2 (Mod Organizer 2) 가상화 파일 시스템(VFS) 기반 실행 환경.

---

### 2. 수집된 아티팩트 및 로그 분석

1. **메모리 덤프 (Memory Dump)**
   - 생성 위치: [`build/cp2077_crash_pid29324.dmp`](file:///E:/repos/cyberpunk2077-singletrainer/build/cp2077_crash_pid29324.dmp) (158 MB)
   - 파이썬 `dbghelp.MiniDumpWriteDump`를 통해 프리징된 라이브 프로세스의 스레드 상태, 스택, 메모리 세그먼트를 덤프 파일로 보존 완료.

2. **MO2 Overwrite 크래시 매니페스트**
   - 파일 위치: `C:\CYBERPUNK_ARK_PACK_MO2\overwrite\bin\x64\Cyberpunk2077.exe-20260829-235900-29324-14916.txt`
   - 주요 내용:
     ```text
     Registered crash info file...
     InternalVersion: 3.0.5294808  P4CL: 9778208  Stream: //R6.Root/R6.Release 
     !!!CRASHED!!!
     Error Reason: Unhandled exception
     Expression: EXCEPTION_ACCESS_VIOLATION (0xC0000005)
     Message: The thread attempted to read inaccessible data at 0xFFFFFFFFFFFFFFFF.
     "stopThreadID": 14916
     "exceptionCode": 0xC0000005
     "processID": 29324
     "##CrashDump##/DumpCrashDataSeconds": "0"
     ```

3. **트레이너 치명적 폴트 로그 (`cp2077_fatal.log`)**
   - 파일 위치: `%LOCALAPPDATA%\cp2077_trainer\cp2077_fatal.log`
   - 주요 기록:
     ```text
     [FATAL][veh] 00:06:39.618 (utc_ft=0x01DD37C7FE580E0C) pid=29324 tid=14916
       code=0xC0000005 EXCEPTION_ACCESS_VIOLATION
       at=0x00007FFBFBEAD65C (cp2077_trainer.dll+0xD65C)
       access=READ target=0xFFFFFFFFFFFFFFFF
       fault-in-trainer=YES
       rip=0x00007FFBFBEAD65C rsp=0x0000005CF87FF508 rbp=0x00007FF6848D22A0
     ```

---

### 3. 콜스택 및 RVA 정밀 역추적

빌드 산출물 PDB([`build/bin/Release/cp2077_trainer.pdb`](file:///E:/repos/cyberpunk2077-singletrainer/build/bin/Release/cp2077_trainer.pdb))와 `dbghelp`를 통해 RVA를 소스 코드 라인 단위로 역추적한 결과:

| 프레임 | RVA | 함수 심볼 | 소스 파일 및 라인 |
|---|---|---|---|
| **#00** | `+0xD65C` | [`Game::Rtti::IsClassOrDerived`](file:///E:/repos/cyberpunk2077-singletrainer/src/game/rtti_invoker.cpp#L154-L163) | [`rtti_invoker.cpp:159`](file:///E:/repos/cyberpunk2077-singletrainer/src/game/rtti_invoker.cpp#L159) |
| **#01** | `+0xFD4D` | `FindAttitudeAgent` | [`entity_tracker.cpp:1348`](file:///E:/repos/cyberpunk2077-singletrainer/src/game/entity_tracker.cpp#L1348) |
| **#02** | `+0x1215C` | `ReadHostility` | [`entity_tracker.cpp:1458`](file:///E:/repos/cyberpunk2077-singletrainer/src/game/entity_tracker.cpp#L1458) |
| **#03** | `+0x10D28` | `ProcessAttitudeOnMainTick` | [`entity_tracker.cpp:1601`](file:///E:/repos/cyberpunk2077-singletrainer/src/game/entity_tracker.cpp#L1601) |
| **#04** | `+0x1071E` | [`Game::EntityTracker::OnGameMainTick`](file:///E:/repos/cyberpunk2077-singletrainer/src/game/entity_tracker.cpp#L1790) | [`entity_tracker.cpp:1790`](file:///E:/repos/cyberpunk2077-singletrainer/src/game/entity_tracker.cpp#L1790) |
| **#05** | `+0x15918` | `HookOnTick` | [`visibility.cpp:340`](file:///E:/repos/cyberpunk2077-singletrainer/src/game/visibility.cpp#L340) |

#### 당시 스레드 상태
- **TID 14916 (메인 틱)**: `Cyberpunk2077.exe+0x14AC5A` (엔진 VEH 크래시 핸들러 진입 후 대기 루프)
- **TID 23156 (렌더 스레드)**: `ntdll!ZwWaitForAlertByThreadId` / `SleepConditionVariableSRW`에서 대기
- **기타 100여 개 워커 스레드**: 메인 틱 신호 두절로 조건변수에서 대기 지속 (데드락)

---

### 4. 근본 원인 (Root Cause) 분석

#### 1) 비정상 컴포넌트 포인터 역참조
- [`FindAttitudeAgent`](file:///E:/repos/cyberpunk2077-singletrainer/src/game/entity_tracker.cpp#L1344-L1355)는 `ForEachComponent`를 통해 엔티티의 컴포넌트 목록을 순회합니다:
  ```cpp
  ForEachComponent(entity, [&](std::byte* component) {
      if (found)
          return;
      if (Game::Rtti::IsClassOrDerived(Game::Rtti::NativeType(component), agentName))
          found = component;
  });
  ```
- 엔티티가 스트리밍 언로드 또는 메모리 재배치 중인 경우, `component` 포인터 또는 `component + 0x30`에 위치한 클래스 레이아웃 포인터가 유효하지 않거나 센티넬 값(`0xFFFFFFFFFFFFFFFF`)일 수 있습니다.
- [`Game::Rtti::NativeType`](file:///E:/repos/cyberpunk2077-singletrainer/src/game/rtti_invoker.cpp#L146-L152)은 널 포인터(`!object`)만 검사하고 `*(object + 0x30)`을 그대로 반환하므로 `(Class*)0xFFFFFFFFFFFFFFFF`가 반환되었습니다.
- [`Game::Rtti::IsClassOrDerived`](file:///E:/repos/cyberpunk2077-singletrainer/src/game/rtti_invoker.cpp#L154-L163)는 방어 검사 없이 `current->nameHash`를 읽으려다 즉시 `0xC0000005` Access Violation을 일으켰습니다.

#### 2) `__try/__except`와 게임 엔진 VEH의 상호작용
- [`ReadHostility`](file:///E:/repos/cyberpunk2077-singletrainer/src/game/entity_tracker.cpp#L1450-L1484) 내부에는 `__try { ... } __except (EXCEPTION_EXECUTE_HANDLER)`가 선언되어 있었습니다.
- 그러나 Windows 예외 디스패치 구조상, SEH 프레임 핸들러(`__except`)가 실행되기 전에 **Vectored Exception Handlers (VEH)**가 먼저 1st-chance로 호출됩니다.
- Cyberpunk 2077 엔진의 크래시 리포터 VEH가 이 예외를 가로채 크래시 덤프 파일을 작성하고 프로세스를 정지 루프로 진입시켰기 때문에 `__except`를 통한 복구가 무력화되었습니다.

---

### 5. 해결 방안 및 권고사항

1. **RTTI 함수 포인터 검증 및 방어적 SEH 적용**:
   - [`src/game/rtti_invoker.cpp`](file:///E:/repos/cyberpunk2077-singletrainer/src/game/rtti_invoker.cpp)의 `NativeType`, `IsClassOrDerived`, `FindFunction`에 유효 유저모드 포인터 검증(`uintptr_t >= 0x10000 && uintptr_t <= 0x00007FFFFFFEFFFF`)을 추가합니다.
   - `ParentClass`, `ClassNameHash`처럼 `IsClassOrDerived` 및 `FindFunction` 내부에도 `__try / __except`를 적용합니다.

2. **컴포넌트 순회 시 안전성 강화**:
   - `ForEachComponent` 및 `FindAttitudeAgent`에서 컴포넌트 포인터의 유효성을 사전에 검증하여 1st-chance 예외 발생 자체를 원천 차단합니다.
