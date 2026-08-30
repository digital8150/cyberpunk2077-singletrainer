# Issue #2 native highlight 3회 save-load 검증과 no-recoil stale-owner 분석

> **작성 메타데이터**
> - **작성 모델 (Author Model)**: `GPT-5 Codex`
> - **작성 일시 (Timestamp)**: `2026-08-30 15:38:52 KST`
> - **분석 대상 (Target)**: `Cyberpunk2077.exe PID 4392 / cp2077_trainer.dll`
> - **핵심 요약 (Executive Summary)**:
>   - Issue #2의 world-drain/settle 수정 빌드는 native highlight가 켜진 상태의 연속 save-load 3회를 통과했다.
>   - 별도로 no-recoil modifier가 이전 월드의 무기 ID에 남아 기능 재적용과 안전 언로드를 함께 막는 결함을 재현했다.

---

## 1. 검증 환경

- PID 4392는 주입 전 `cp2077_trainer.dll` 모듈이 없는 clean game process였다.
- 현재 HEAD를 `build/bin/Release/cp2077_trainer.dll`로 다시 빌드해 15:27:01에 주입했다.
- 주입 DLL SHA-256은 `3CCFBF10C1D6F0DE9467A850270536C90184132E1841504D8C88076C9F97F50D`였다.
- `%LOCALAPPDATA%\cbpk\config.ini`에서 `native_highlight=1`, `no_recoil=1`, logging/profiling/VEH/fatal log가 활성화돼 있었다.
- `tools/scripts/watchdog.ps1 -TargetPid 4392`를 주입 직후 붙였고, 검증 종료 시점까지 crash/freeze 신호를 내지 않았다.

## 2. Issue #2 검증 결과

초기 진입과 사용자가 수행한 세 번의 save-load 모두 정확히 `empty -> repopulated -> settled`를 통과했다.

| 구간 | empty | repopulated | settled |
|---|---:|---:|---:|
| 초기 진입 | 15:27:01.419 | 15:27:34.368 | 15:27:35.447 (`tracked=37`) |
| load 1 | 15:27:53.385 | 15:27:58.133 | 15:27:59.144 (`tracked=34`) |
| load 2 | 15:28:14.908 | 15:28:19.574 | 15:28:20.575 (`tracked=34`) |
| load 3 | 15:28:50.217 | 15:28:55.060 | 15:28:56.068 (`tracked=63`) |

- 모든 repopulation에 1000 ms settle gate가 적용됐다.
- 이전 재현의 시그니처였던 `Cyberpunk2077.exe+0xA037C8`, WRITE `0x49`가 없었고 현재 세션 fatal 파일에는
  `[SESSION]` 헤더만 있으며 `[FATAL]` 레코드가 없다.
- 사용자가 End unload를 요청한 뒤 native highlight는 15:30:38.720에
  `cleanup acknowledged: generation=9 queued=40 cleared=4`를 기록했다. 따라서 이후 unload 실패의 원인은
  native highlight가 아니다.
- 이 결과는 Issue #2의 즉시 재현 경로에 대한 수정 통과로 판정한다. 장시간 soak는 별도 안정성 검증이다.

## 3. no-recoil 및 unload 결함

현재 세션의 main-tick TID는 33064였다.

- 15:27:42.886: 무기 `0x9B0C63`에 modifier 11개 적용.
- 15:28:00.369: `0x9B0C63`의 modifier 11개 정상 제거 후 새 무기 `0x9B0C4B`에 11개 적용.
- 다음 두 번의 save-load 뒤에는 `0x9B0C4B` 제거도 새 무기 적용도 발생하지 않았다.
- 15:30:14.202 End unload 요청 뒤 매번 `no-recoil cleanup timed out: targetId=0x9B0C4B`가 발생했고,
  `hook shutdown aborted` 및 1초 후 재시도가 무한 반복됐다.

`gameInstance`, `gameStatsSystem`, local player는 월드 소유 객체다. fresh-acquire한 새 StatsSystem에 이전 월드의
target ID와 modifier Handle을 넘기는 것은 올바른 제거 경로가 아니다. 제거 실패를 보수적으로 유지하는 기존
상태 머신 때문에 `g_active`가 영구히 true로 남아 새 무기 적용과 unload acknowledgement를 동시에 막았다.

Present 쪽 `PublishDesired(true)` 재게시는 로그 노이즈와 상태 경쟁이지만 직접 원인은 아니다.
`OnGameMainTick`은 `cleanupRequested`를 우선해 실제 요청값을 false로 만든다. 직접 원인은 stale owner에 대한
`RemoveModifier` 실패다.

## 4. 적용한 수정

- modifier 적용 시 current `gameInstance`, `statsSystem`, local-player instance 주소를 **역참조하지 않는 identity
  token**으로 기록한다.
- 각 main tick에서 fresh-acquire한 owner와 비교한다. owner가 바뀌면 이전 월드의 StatsSystem에 제거 호출을 하지
  않고, 검증된 exact Handle destructor 경로로 로컬 strong owner를 해제한 뒤 상태를 초기화한다.
- 동일한 player/system owner 안에서 무기만 바뀐 경우에는 기존 `RemoveModifier` 경로를 유지한다.
- `HasExactHandleOwnership()`을 no-recoil runtime gate에 포함하고, 기존의 보수적 non-final release 대신 exact
  release를 사용한다.
- cleanup 중 Present가 `enabled=true`를 다시 publish하지 못하게 했다.
- unload는 요청 한 번당 최대 3회만 시도한다. 실패하면 `FreeLibrary`를 호출하지 않고 DLL을 resident 상태로
  남기며, 새 End edge 또는 automation event가 있어야 다시 시도한다.
- 진단 UI에 `retiredOwnerResets` 카운터를 추가했다.

## 5. 검증 및 다음 라이브 테스트

- `git diff --check` 통과.
- 실행 중 DLL 잠금 때문에 `build/` 링크는 `LNK1104`로 예상대로 실패했다. 동일 소스를 독립 출력 트리인
  `build-next`에서 Release 빌드했고 성공했다.
- 수정 DLL: `build-next/bin/Release/cp2077_trainer.dll`
- SHA-256: `A129F5BE7DCA2882C699B01C286EAC7C319CB88D67C832DC820E35A35D19DF43`
- 현재 PID 4392에는 구 빌드가 주입돼 있고 이미 unload 요청 루프에 들어갔으므로 강제 해제/재주입하지 않는다.
  다음 clean game process에서 no-recoil ON 상태로 2회 이상 save-load 후 `retired owner reset`, 새 target 재적용,
  End unload 완료를 확인한다.
