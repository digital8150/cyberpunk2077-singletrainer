# Cyberpunk 2077 라이브 프로세스(PID 13248) 프리징 원인 분석 보고서

> **작성 메타데이터**
> - **작성 모델 (Author Model)**: `Gemini 3.7 Flash (High)`
> - **작성 일시 (Timestamp)**: `2026-08-30 01:55:00 (KST)`
> - **분석 대상 (Target)**: `Cyberpunk 2077 (PID: 13248) / cp2077_trainer.dll`
> - **핵심 요약 (Executive Summary)**:
>   - 게임 메인 틱(`OnTick`, TID 30224) 중 `FindAttitudeAgent`에서 `NativeType` 호출 시 스트리밍 해제된(use-after-free) 컴포넌트 포인터(`0x41E5BB48`)의 클래스 오프셋(`+0x30`, `0x41E5BB78`)을 역참조하여 `0xC0000005 (EXCEPTION_ACCESS_VIOLATION)` 발생.
>   - 지난번 프리징(PID 29324)과 **동일한 구조적 근본 원인(Root Cause)**의 연속 발현임. `IsValidUserPointer` 범위 검사(`0x10000 ~ 0x7FFFFFFEFFFF`)를 통과했으나 실제로는 이미 해제/무효화된 힙 주소였으며, `__try/__except` 블록 도달 전 게임 엔진의 1st-chance VEH가 먼저 개입하여 프로세스를 정지 루프에 빠뜨려 하드 프리징을 유발함.

---

## 📌 본문

### 1. 개요 및 장애 환경

- **프로세스 ID**: `13248` (`Cyberpunk2077.exe`)
- **실행 파일 버전**: ProductVersion `2.31`, FileVersion `3.0.5294808`, P4CL `9778208`
- **장애 발생 시점**: `2026-08-30 01:48:03.404 (KST)`
- **증상**: 게임 화면 및 사운드가 완전히 멈추고 입력을 받지 않는 하드 프리징 발생 (프로세스는 메모리 10.3GB 점유 상태로 유지).
- **실행 환경**: MO2 (Mod Organizer 2) 가상화 파일 시스템(VFS) 기반 실행 환경.

---

### 2. 수집된 아티팩트 및 로그 분석

본 분석에 사용된 모든 원본 메모리 덤프와 로그는 향후 다른 모델 및 개발자가 독립적으로 재검토할 수 있도록 아래 전용 아티팩트 디렉터리에 보존 및 관리됩니다:

📂 **아티팩트 보존 디렉터리**: [`reports/artifacts/2026-08-30_freeze_pid13248/`](file:///E:/repos/cyberpunk2077-singletrainer/reports/artifacts/2026-08-30_freeze_pid13248/)

1. **라이브 프로세스 메모리 덤프 (Memory Dump)**
   - 파일 위치: [`reports/artifacts/2026-08-30_freeze_pid13248/cp2077_freeze_pid13248.dmp`](file:///E:/repos/cyberpunk2077-singletrainer/reports/artifacts/2026-08-30_freeze_pid13248/cp2077_freeze_pid13248.dmp) (160 MB)
   - 파이썬 `dbghelp.MiniDumpWriteDump`(`MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithProcessThreadData | MiniDumpWithFullMemoryInfo | MiniDumpWithThreadInfo`)를 통해 하드 프리징 상태의 전체 104개 스레드 상태, 스택 메모리 세그먼트, 모듈 목록을 보존 완료.

2. **MO2 Overwrite 크래시 매니페스트**
   - 파일 위치: [`reports/artifacts/2026-08-30_freeze_pid13248/Cyberpunk2077.exe-20260830-003624-13248-30224.txt`](file:///E:/repos/cyberpunk2077-singletrainer/reports/artifacts/2026-08-30_freeze_pid13248/Cyberpunk2077.exe-20260830-003624-13248-30224.txt)
   - 주요 기록:
     ```text
     Registered crash info file...
     InternalVersion: 3.0.5294808  P4CL: 9778208  Stream: //R6.Root/R6.Release 
     !!!CRASHED!!!
     Error Reason: Unhandled exception
     Expression: EXCEPTION_ACCESS_VIOLATION (0xC0000005)
     Message: The thread attempted to read inaccessible data at 0x41E5BB78.
     File: <Unknown>
     Line: 0
     "uptimeSeconds": 4298
     "stopThreadID": 30224
     "exceptionCode": 0xC0000005
     "processID": 13248
     ```

3. **트레이너 치명적 폴트 로그 (`cp2077_fatal.log`)**
   - 파일 위치: [`reports/artifacts/2026-08-30_freeze_pid13248/cp2077_fatal.log`](file:///E:/repos/cyberpunk2077-singletrainer/reports/artifacts/2026-08-30_freeze_pid13248/cp2077_fatal.log)
   - 주요 기록:
     ```text
     [FATAL][veh] 01:48:03.404 (utc_ft=0x01DD37D628900DC4) pid=13248 tid=30224
       code=0xC0000005 EXCEPTION_ACCESS_VIOLATION
       at=0x00007FFC16CAD816 (cp2077_trainer.dll+0xD816)
       access=READ target=0x0000000041E5BB78
       fault-in-trainer=YES
       rip=0x00007FFC16CAD816 rsp=0x0000008D38FFE868 rbp=0x4D402860BE84781A
       rax=0x0000000041E4BB48 rbx=0x0000000041E5BB48 rcx=0x0000000041E5BB48 rdx=0x00007FFFFFFDFFFF
       rsi=0x0000000000000072 rdi=0x0000000000000016 r8=0x4D402860BE84781A r9=0x00007FFFFFFDFFFF
       stack:
         +0x0 0x00007FFC16CAFFA8 (cp2077_trainer.dll+0xFFA8)
         +0x20 0x00007FF68226936E (Cyberpunk2077.exe+0x45936E)
         +0x30 0x00007FF68226936E (Cyberpunk2077.exe+0x45936E)
     ```

4. **트레이너 런타임 진단 로그 (`cp2077_trainer.log`)**
   - 파일 위치: [`reports/artifacts/2026-08-30_freeze_pid13248/cp2077_trainer.log`](file:///E:/repos/cyberpunk2077-singletrainer/reports/artifacts/2026-08-30_freeze_pid13248/cp2077_trainer.log)
   - 직전 마지막 정상 기록 (`01:48:02.788`): 메인 틱 및 Present 렌더가 정상적으로 작동 중이었으나 0.6초 뒤인 `01:48:03.404` 메인 틱에서 Attitude 경로 실행 중 즉시 중단됨.

---

### 3. 콜스택 및 심볼 역추적

빌드 PDB([`build/bin/Release/cp2077_trainer.pdb`](file:///E:/repos/cyberpunk2077-singletrainer/build/bin/Release/cp2077_trainer.pdb))와 `dbghelp`를 통해 덤프 및 폴트 RVA를 소스 코드 라인 단위로 역추적한 결과:

| 프레임 | RVA | 함수 심볼 | 소스 파일 및 라인 | 설명 |
|---|---|---|---|---|
| **#00** | `+0xD816` | [`Game::Rtti::NativeType`](file:///E:/repos/cyberpunk2077-singletrainer/src/game/rtti_invoker.cpp#L146-L161) | [`rtti_invoker.cpp:152`](file:///E:/repos/cyberpunk2077-singletrainer/src/game/rtti_invoker.cpp#L152) | `mov rax, [rcx + 0x30]` 실행 중 폴트 (`rcx=0x41E5BB48`) |
| **#01** | `+0xFFA8` | `FindAttitudeAgent` | [`entity_tracker.cpp:1367`](file:///E:/repos/cyberpunk2077-singletrainer/src/game/entity_tracker.cpp#L1367) | `ForEachComponent` 순회 중 `NativeType(component)` 호출 |
| **#02** | `+0x10D28` | `ProcessAttitudeOnMainTick` | [`entity_tracker.cpp:1554`](file:///E:/repos/cyberpunk2077-singletrainer/src/game/entity_tracker.cpp#L1554) | 메인 틱에서 AttitudeAgent 탐색 및 적대성 계산 |
| **#03** | `+0x15C05` | `HookOnTick` | [`visibility.cpp:355`](file:///E:/repos/cyberpunk2077-singletrainer/src/game/visibility.cpp#L355) | 엔진 메인 틱(`OnTick`) 디투어 진입점 |

#### 덤프 분석 기반 전체 스레드 상태 (104개 스레드)
- **TID 30224 (엔진 메인 틱 스레드)**: `ntdll.dll+0x1A90D` (엔진 VEH 크래시 핸들러 진입 후 `ZwWaitForAlertByThreadId` 무한 대기 루프)
- **TID 20624 / 34168 / 30656 등 100여 개 워커 및 렌더 스레드**: 메인 틱 신호 두절로 인해 `ntdll.dll+0x163FD4` (`SleepConditionVariableSRW`)에서 상시 대기 (Hard Freeze)

---

### 4. 지난번 프리징(PID 29324)과의 비교 및 원인 분석

#### 🔍 비교 대조표

| 분석 항목 | PID 29324 프리징 (이전 사건) | PID 13248 프리징 (현재 사건) |
|---|---|---|
| **발생 시점** | 2026-08-30 00:06:39 (KST) | 2026-08-30 01:48:03 (KST) |
| **정지 스레드** | TID 14916 (메인 틱) | TID 30224 (메인 틱) |
| **크래시 RVA** | `cp2077_trainer.dll+0xD65C` | `cp2077_trainer.dll+0xD816` |
| **발생 위치** | `Game::Rtti::IsClassOrDerived` (`rtti_invoker.cpp:159`) | `Game::Rtti::NativeType` (`rtti_invoker.cpp:152`) |
| **폴트 타겟** | `0xFFFFFFFFFFFFFFFF` (Non-canonical / #GP) | `0x0000000041E5BB78` (해제된 유저 포인터 `0x41E5BB48 + 0x30`) |
| **진입 경로** | `HookOnTick` → `FindAttitudeAgent` | `HookOnTick` → `FindAttitudeAgent` |
| **원인 판정** | **동일한 근본 구조적 취약점의 연속 발현** | **동일한 근본 구조적 취약점의 연속 발현** |

---

### 5. 근본 원인 (Root Cause) 심층 분석

#### 1) `IsValidUserPointer` 범위 검사의 근본적 한계
- PID 29324 수정 시 도입된 `IsValidUserPointer`는 주소 값이 유저모드 canonical 범위(`0x10000 <= addr <= 0x00007FFFFFFEFFFF`)에 속하는지만 검사하는 **단순 정적 수치 검사**입니다.
- PID 13248에서 전달된 컴포넌트 포인터 `rcx = 0x0000000041E5BB48`는 정상적인 유저모드 힙 주소 범위에 속하므로 `IsValidUserPointer`를 그대로 통과하였습니다.
- 그러나 해당 메모리 페이지는 엔티티 스트리밍 언로드 또는 메모리 재배치로 인해 이미 해제(`VirtualFree` / unmapped / deallocated)된 상태였습니다.

#### 2) `NativeType`의 `+0x30` 역참조 및 1st-chance VEH 개입
- `NativeType`은 전달된 `object`(`0x41E5BB48`)에 대해 `+0x30` 오프셋(`0x41E5BB78`)의 8바이트 역참조(`mov rax, [rcx + 0x30]`)를 수행했습니다.
- 해당 주소 접근 시 CPU 하드웨어 페이지 폴트 `0xC0000005 (EXCEPTION_ACCESS_VIOLATION)`가 발생했습니다.
- `NativeType` 함수 내부에 `__try / __except (EXCEPTION_EXECUTE_HANDLER)`가 둘러싸여 있었으나, Windows의 예외 디스패치 규격상 프레임 기반 SEH 핸들러보다 **Vectored Exception Handler (VEH)**가 먼저 1st-chance로 호출됩니다.
- Cyberpunk 2077(REDengine 4) 자체 크래시 리포터 VEH가 이 1st-chance 예외를 포착하여 크래시 매니페스트(`Cyberpunk2077.exe-20260830-003624-13248-30224.txt`)를 작성하고 메인 스레드를 무한 대기 루프로 진입시켰습니다.
- 이에 따라 `__except` 블록으로의 복구가 무력화되었고, 메인 틱 신호에 의존하는 게임 전체 스레드가 데드락에 빠져 프리징되었습니다.

---

### 6. 재발 방지 대책 및 권고사항

1. **`FindAttitudeAgent`의 매 틱 무차별 컴포넌트 목록 순회 전면 폐기**:
   - `entity->components` 배열은 스트리밍 언로드 과정에서 안전하지 않은 핸들을 포함할 수 있습니다.
   - 매 틱마다 모든 엔티티의 컴포넌트 목록을 전수 순회(`ForEachComponent`)하며 `NativeType`을 호출하는 방식 대신, 엔티티 스냅샷 패스에서 이미 유효성이 검증된(`SnapshotResult::Ready`) 살아있는 엔티티에 대해서만 캐시된 Attitude 정보를 갱신하도록 분리해야 합니다.

2. **엔티티 생명주기 및 메모리 접근 안전성 확보**:
   - 엔티티의 `isDisposed` 또는 활성 플래그를 사전에 확인하여 스트리밍 아웃 중인 엔티티의 내부 컴포넌트 접근을 원천 차단합니다.
   - Attitude 조회가 실패하더라도 `Unknown` 상태로 안전하게 유지하고, 위험한 원시 포인터 역참조를 최소화합니다.
