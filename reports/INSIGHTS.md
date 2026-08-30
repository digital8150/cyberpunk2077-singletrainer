# 디버깅 인사이트 모음 (Debugging Hints & Insights)

> ⚠️ **성격**: 절대적인 규칙이 아니라, 실제 라이브 인젝션·프리징·크래시 디버깅 과정에서 축적된 **경험적
> 힌트**입니다. 문제 해결 시 참고 자료로 활용하세요. 강제되는 규칙은 `AGENTS.md`에 있습니다.
>
> 📌 **운영 방식**: 개별 보고서(`reports/*.md`)에는 인사이트 절을 두지 않습니다. 새 보고서를 쓰면서 재사용
> 가치가 있는 힌트를 얻었다면 **이 파일에 추가**하세요. 같은 주제가 이미 있으면 새 항목을 만들지 말고 기존
> 항목을 보강합니다. 각 항목 끝에 근거가 된 보고서를 `— 출처: …`로 남깁니다.

---

## 1. 엔진 호출 안전성 (SEH / VEH / 생명주기)

### 1.1 SEH는 엔진 호출의 안전망이 아니다

`__try / __except (EXCEPTION_EXECUTE_HANDLER)`로 감싸도 메모리 위반(`0xC0000005`)이 발생하는 순간
**1st-chance 단계에서 게임 엔진의 VEH나 크래시 리포터가 먼저 반응**합니다. 엔진 VEH가 크래시 덤프
매니페스트를 쓰고 프로세스를 중단/대기 상태로 만들면 `__except`로의 복구는 무의미해집니다.

더 나아가, **`__except`가 정상적으로 잡고 언와인드까지 성공해도 프로세스는 하드 프리즈할 수 있습니다.**
PID 26484에서 실측된 사례입니다: 폴트가 엔진 내부 동기화/참조 상태를 중간에 끊어놓았고, SEH 언와인드는
엔진이 잡고 있던 것을 되돌려주지 못합니다. 이후 엔진의 스핀 대기 predicate가 영원히 성립하지 않아 메인
틱이 코어 하나를 태우며 멈췄습니다.

**결론: 엔진 호출은 예외 처리로 방어하는 것이 아니라 사전 조건 검사로 회피해야 합니다.** 남은 `__except`는
최후 방어선일 뿐 "여긴 안전하다"는 근거가 될 수 없습니다.

— 출처: `2026-08-30_freeze_analysis_pid26484.md`, `2026-08-30_freeze_analysis_pid29324.md`,
`2026-08-30_freeze_analysis_pid13248.md`

### 1.2 포인터 범위 검사는 UAF를 막지 못한다

유효 유저모드 포인터 검사(`uintptr_t >= 0x10000 && uintptr_t <= 0x00007FFFFFFEFFFF`)는 커널 주소와
non-canonical 주소(#GP)만 걸러냅니다. **이미 해제된 힙(Use-After-Free)은 전혀 방어하지 못합니다.**
RTTI 역참조나 컴포넌트 접근 전에는 범위 검사가 아니라 **객체 생명주기 검증**이 선행되어야 합니다.
`ForEachComponent` / `FindAttitudeAgent` 같은 순회 경로에서는 1st-chance 예외 발생 자체를 원천 차단하는
방향으로 설계합니다.

— 출처: `2026-08-30_freeze_analysis_pid13248.md`, `2026-08-30_freeze_analysis_pid29324.md`

### 1.3 포인터의 신선도(freshness)는 사용 가능성(usability)이 아니다

"raw 포인터를 틱 사이에 캐시하지 않고 호출 직전에 다시 얻는다"는 것만으로는 부족합니다. 월드/세이브 전환
중에는 시스템 객체가 **non-null이면서도 내부 바인딩이 풀린 상태**로 존재할 수 있습니다. PID 26484에서는
막 획득한 non-null `gameIVisionModeSystem`으로 호출했는데도 엔진 내부에서 near-null write
(`target=0x49`)로 폴트가 났습니다.

즉 fresh-acquire와 **전환 구간 게이트**는 별개의 대책이며 둘 다 필요합니다. 트래커가 비는 드레인 틱
(`tracked=0`)에서는 엔진 호출 자체를 건너뛰고 로컬 상태만 정리하십시오 — 퇴역하는 월드의 상태를 굳이
되돌려줄 필요가 없습니다.

— 출처: `2026-08-30_freeze_analysis_pid26484.md`

### 1.4 엔진 OnTick 스레드와 렌더 스레드 간 데드락 패턴

REDengine의 렌더 스레드(`Present`)와 다수의 워커 스레드는 메인 엔진 틱(`OnTick`)의 상태 신호
(`SRWLock`, `ConditionVariable`)를 지속적으로 동기화 대기합니다. 메인 틱이 멈추거나 예외 처리 루프로 빠지면
전체 스레드가 `ZwWaitForAlertByThreadId` / `SleepConditionVariableSRW`에서 영구 정지하여 하드 프리즈로
이어집니다. **프리즈 분석은 항상 메인 틱 스레드부터 보십시오.**

— 출처: `TEMPLATE.md` 초기 축적분, `2026-08-30_freeze_analysis_pid26484.md`에서 재확인(103스레드 중 101개가
`ntdll` 대기)

---

## 2. 프리즈 실측 기법

### 2.1 "진행 없음"의 정량 증명: 스택 스냅샷 diff

`tools/scripts/threadstacks.py`를 몇 분 간격으로 두 번 떠서 diff하는 방법이 매우 효과적입니다. PID 26484에서는
103개 스레드 전체 diff가 **2줄**(한 스레드의 RIP)뿐이었고 RSP와 모든 프레임이 불변이었습니다.

여기에 `TotalProcessorTime` 델타를 붙이면 스피너 개수까지 바로 나옵니다 — 3초 동안 정확히 3초를 소모하면
코어 하나를 태우는 스레드가 정확히 하나라는 뜻입니다.

— 출처: `2026-08-30_freeze_analysis_pid26484.md`

### 2.2 RSP 비교로 SEH 언와인드 여부를 판정한다

`[FATAL]` 레코드의 `rsp`와 프리즈 시점의 `RSP`를 비교합니다. x64 스택은 아래로 자라므로, **현재 RSP가 더
높으면(더 얕으면) 언와인드가 일어난 것**입니다. "예외를 잡았는데도 얼었다"를 증명하는 가장 싼 방법입니다.

— 출처: `2026-08-30_freeze_analysis_pid26484.md`

### 2.3 ntdll 스핀은 대개 ntdll이 범인이 아니다

RIP가 ntdll 안에서만 움직이면 그 주소의 바이트를 살아 있는 프로세스에서 직접 읽어보십시오
(`tools/scripts/memtool.py read --type bytes`). `rdtscp` + `KUSER_SHARED_DATA(0x7FFE03B8)` + 역방향 `jne`
재시도 패턴이면 `RtlQueryPerformanceCounter`이고, **진짜 대기 루프는 한 프레임 위의 호출자**입니다.

Cyberpunk 2077에서 `Cyberpunk2077.exe+0x14AC7B` / `+0x14AF14`는 `lock cmpxchg` + 역방향 점프로 함수 포인터
predicate를 폴링하는 **게임 자체의 스핀 대기 프리미티브**로 이미 식별되어 있습니다.

— 출처: `2026-08-30_freeze_analysis_pid26484.md`, `progress.md` 2026-08-28 항목

### 2.4 주입된 빌드가 무엇인지 먼저 확정한다

수정이 반영된 빌드인지 소스로 추측하지 마십시오. `Get-Process -Id <pid>` 의 `.Modules`로 실제 파일 경로와
`LastWriteTime`, 해시를 얻고, **그 수정에만 있는 고유 로그 문자열을 DLL에서 `grep -a`로 찾으면** "이 수정이
이 빌드에 들어 있는가"를 확정할 수 있습니다. `build/`와 `build-next/`가 병존하고 작업 트리에 미커밋 변경이
섞여 있을 때 특히 중요합니다.

— 출처: `2026-08-30_freeze_analysis_pid26484.md`

---

## 3. 덤프 · 심볼화

### 3.1 라이브 프로세스 미니덤프

프리즈 상태로 살아 있는 프로세스는 `dbghelp.dll`의 `MiniDumpWriteDump`를 파이썬 ctypes로 호출해 즉시
미니덤프로 보존할 수 있습니다. 디버거 설치가 필요 없습니다. 시간 간격을 두고 두 번 뜨면 2.1의 "진행 없음"
비교에도 그대로 쓸 수 있습니다.

### 3.2 PDB 심볼화

빌드 디렉터리의 PDB(`cp2077_trainer.pdb`)와 dbghelp의 `SymFromAddr` / `SymGetLineFromAddr64`를 조합하면
원시 RVA를 소스 라인까지 역추적할 수 있습니다.

⚠️ **함정**: dbghelp는 **ANSI 계열(`SymInitialize` / `SymLoadModuleEx` / `SymFromAddr`) + 실제 프로세스 핸들
+ 명시적 모듈 크기** 조합이라야 동작합니다. 유니코드(`…W`) 변형에 가짜 핸들을 넘기는 조합은 **에러 없이
조용히 실패**해 모든 주소가 `?`로 나옵니다.

### 3.3 미니덤프 자체 파싱

심볼이 없어도 `tools/scripts/dumpwalk.py`가 미니덤프의 모듈/스레드/메모리 스트림을 파싱하고 스택을 스캔해
모듈 내부를 가리키는 qword를 유사 콜스택으로 뽑아 줍니다. 모듈 귀속만으로도 범인 후보를 좁힐 수 있습니다.

— 출처: `2026-08-30_freeze_analysis_pid29324.md`, `2026-08-30_freeze_analysis_pid26484.md`

---

## 4. 로그 · 아티팩트 위치

### 4.1 MO2 가상화 환경(VFS)의 로그와 크래시 파일

게임 엔진이 자체 생성하는 크래시 리포트 텍스트(`Cyberpunk2077.exe-YYYYMMDD-HHMMSS-<PID>-<TID>.txt`)와 각종
모드 로그는 게임 폴더가 아니라 **MO2의 오버라이트 경로**(`<MO2 인스턴스>\overwrite\bin\x64\`,
`…\overwrite\red4ext\logs\`, `…\overwrite\r6\logs\`)에 쓰일 수 있습니다. 인스턴스 경로와 드라이브 문자는
시간에 따라 바뀌었으므로 **`ModOrganizer.ini`의 `gamePath`와 인스턴스 디렉터리에서 직접 확인**하십시오.

프로세스가 파일 핸들을 독점(Exclusive Lock)해 일반 읽기가 실패하면, Win32 `CreateFileW`에
`FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`를 주어 안전하게 읽을 수 있습니다.

### 4.2 게임 GPU 크래시 로그

REDengine은 Aftermath 기반 GPU 크래시 로그를 `%TEMP%\gpucrash-YYYY-MM-DD_HH.MM.SS.mmm.log`에 남깁니다.
첫 두 줄의 `Device Removed Reason` / `GPU Crash Reason`이 실패 종류를 바로 알려 주며, 뒤쪽 Breadcrumbs 절은
어느 렌더 패스에서 멈췄는지 보여 줍니다. `%TEMP%`는 정리될 수 있으므로 조사 시작과 동시에 복사하십시오.

게임 텔레메트리는 `%LOCALAPPDATA%\CD Projekt Red\Cyberpunk 2077\CrashInfo.json`에 남습니다
(`timeCrash`, `crashPatch`, `isOom`, 위치/퀘스트).

### 4.3 DRED는 조용히 꺼진다

`d3dconfig dred`가 `force-on`이어도 `d3dconfig apps`에 등록된 **실행 파일 경로가 실제 경로와 다르면 DRED는
적용되지 않습니다.** 아무 경고도 없고, 크래시가 나서 `GetAutoBreadcrumbsOutput1`이 `0x887A0004`
(`DXGI_ERROR_UNSUPPORTED`)로 실패해야 드러납니다. **게임 설치 경로가 바뀌면 반드시 재등록**하십시오
(설정은 D3D12 디바이스 생성 시점에 읽히므로 게임 재시작 필요).

— 출처: `2026-08-30_gpucrash_analysis_pid29084.md`

### 4.4 아티팩트 보관 규칙

프리징·크래시 조사에서 생성된 미니덤프(`.dmp`), 엔진/트레이너 로그, 스택 스냅샷은
`reports/artifacts/<날짜>_<유형>_pid<PID>/` 하위에 PID별로 격리 보관해 후속 검증이 가능하도록 합니다.
`.dmp`는 `.gitignore` 대상이라 저장소를 오염시키지 않습니다.

— 출처: `2026-08-30_freeze_analysis_pid13248.md`
