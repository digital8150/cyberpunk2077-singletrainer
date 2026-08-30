# PID 29968 로딩 프리즈 메모리 덤프 분석

> **작성 메타데이터**
> - **작성 모델 (Author Model)**: `GPT-5 Codex`
> - **작성 일시 (Timestamp)**: `2026-08-30 19:46:10 (KST)`
> - **분석 대상 (Target)**: `Cyberpunk2077.exe PID 29968 / 로딩 중 하드 프리즈`
> - **핵심 요약 (Executive Summary)**:
>   - 메인 틱은 REDengine scheduler에서 완료되지 않는 job 하나를 기다리며 코어 하나를 소모하고 있었다. 렌더/GPU 행이나 Present 스레드 데드락은 아니다.
>   - 과거 Native Highlight 폴트는 재발하지 않았다. 이번 사건의 1순위 용의자는 월드 전환 게이트 없이 매 틱 엔진 객체를 획득·해제하는 PlayerModifiers 경로지만, 덤프만으로 정확한 선행 호출까지 확정할 수는 없다.

---

## 1. 장애 현상 및 수집 조건

- 프로세스: `Cyberpunk2077.exe`, PID `29968`, 시작 시각 `17:17:09`
- 마지막 트레이너 로그: `19:22:06.576`
- 관측 상태: `Responding=False`, 메인 틱 TID `34748`만 코어 하나를 계속 사용
- 주입 DLL: `build/bin/Release/cp2077_trainer.dll`
  - 로드 베이스: `0x7FFF8C790000`
  - SHA-256: `F3D559E847FF0877208A7FF9DF3ECE038BB28A58ADF59E7D12DF09555163353A`
- 사용자는 프리즈 전후 UI에서 일반 로깅을 껐다. 다만 `19:22:03`에 열린 fatal 세션에는 현재 DLL 모듈 표가 남아 있고, 프리즈 전까지 fatal sink가 활성 상태였음에도 `[FATAL]` 레코드는 없다.

라이브 프로세스를 종료하지 않고 약 2분 30초 간격으로 미니덤프 두 개를 수집했다.

| 아티팩트 | 시각 | 크기 / 해시 |
|---|---:|---|
| `cp2077_freeze_pid29968.dmp` | 19:36:44 | 154,047,417 bytes / `2C2733BDB97485D9EFFF586D4A8DA5719E342D1B1A5523CAD562B05B5FF0E0F5` |
| `cp2077_freeze_pid29968_b.dmp` | 19:39:14 | 154,047,417 bytes |
| `cp2077_trainer.log` | 마지막 19:22:06 | 전체 세션 로그 |
| `cp2077_fatal.log` | 세션 시작 19:22:03 | 현재 세션에는 모듈 표만 있고 예외 레코드 없음 |
| `config.ini` | 프리즈 후 복사 | 당시 기능 설정 확인용 |

보관 위치: `reports/artifacts/2026-08-30_freeze_pid29968/`. 덤프는 저장소 정책상 git에서 제외한다.

## 2. 확정된 메인 스레드 상태

메인 틱 TID `34748`은 2초 표본에서 CPU `2031 ms`를 사용했다. 나머지 103개 스레드는 CPU 증가가 없고 대부분 `ntdll`의 condition/SRW wait에 있었다. 따라서 GPU command queue나 Present가 멎은 형태가 아니라, 메인 게임 스레드 하나가 busy wait에 빠져 워커 전체가 대기하는 CPU 프리즈다.

두 덤프의 메인 스레드 유사 콜스택 41개 프레임은 RVA 단위로 완전히 동일했다. RIP만 `ntdll!RtlQueryPerformanceCounter` 내부의 짧은 재시도 지점 사이를 오갔으며, 실제 상위 대기 루프는 다음 게임 프레임이다.

```text
ntdll.dll+0x1A90D / +0x1A925
Cyberpunk2077.exe+0x14AC7B
Cyberpunk2077.exe+0x14AF14
...
cp2077_trainer.dll+0x1C185
```

PDB로 `cp2077_trainer.dll+0x1C185`를 심볼화하면 `HookOnTick+0x125`, `visibility.cpp:414`다. 디스어셈블 결과 이 주소는 `g_originalOnTick` 호출의 **복귀 주소**다. 즉 트레이너의 main-tick 작업들은 이미 끝났고, 스레드는 원본 REDengine `OnTick` 안에서 멈춰 있다.

게임 `+0x14AC10` 부근은 `[r14+0x18]`의 완료 상태를 반복 검사한다. 현재 두 덤프 모두 해당 값이 `1`로 고정되어 있었다. 의미상 **완료되지 않는 job/dependency 하나**를 scheduler가 영구 대기하고 있다.

## 3. 이전 Native Highlight 프리즈와의 비교

PID 26484의 두 덤프와 비교하면 41개 게임/시스템 프레임이 이번 덤프와 동일하다. 차이는 당시 트레이너 빌드의 원본 OnTick 복귀 RVA가 `+0x17D15`, 현재는 `+0x1C185`라는 점뿐이다. 따라서 최종적으로 빠진 scheduler 대기 상태는 같다.

그러나 선행 증거는 다르다.

| 항목 | PID 26484 | PID 29968 |
|---|---|---|
| 선행 예외 | `Cyberpunk2077.exe+0xA037C8`, `target=0x49` WRITE AV | 예외 레코드 없음 |
| Native Highlight 호출 | world drain 중 `SetBraindanceMode(false)` | 런타임 mode 플래그 `0`, desired transition 없음 |
| 트래커 상태 | 전환/드레인 경로 | 5개 객체 모두 civilian, civilian category 비활성 |
| 결론 | Highlight의 전환기 엔진 호출이 확정 원인 | 같은 종착 상태일 뿐 Highlight 재발 증거 없음 |

현재 DLL에는 `215fb70`에서 추가한 world-empty/1초 settle gate가 포함되어 있다. 덤프의 `g_nativeHighlightModeActive`도 `0`, cleanup request도 `0`이므로 이전 `SetBraindanceModeOnMainTick(false)` 경로가 다시 실행됐다고 볼 수 없다. **해결했던 Highlight 문제가 되돌아온 것은 아니다.**

## 4. PlayerModifiers 경로가 1순위인 이유

프리즈 당시 설정과 PDB global 상태는 다음과 같다.

```text
desiredModifierMask = 0x3       # no_recoil + no_spread
activeModifierMask  = 0x3
playerId            = 1
targetId / weaponId = 0x9B721C
active              = true
cleanupRequested    = false
usingWeaponTarget   = true
```

Infinite health/stamina는 꺼져 있었다. 마지막 로그도 같은 weapon target을 보고한 직후 끊겼다.

`ProcessModifierState`는 target과 mask가 같아 Add/Remove를 생략하는 정상 틱에도 다음 엔진 작업을 매번 수행한다.

1. 시스템 fresh-acquire
2. RTTI `GetLocalPlayer`
3. reflected `GetItemInSlot`
4. item/player strong handle release

stale-owner 수정(`896f332`)은 owner identity가 **바뀐 뒤** 이전 상태를 새 월드에 보내지 않도록 막는다. 하지만 로딩 중 같은 주소의 owner가 non-null 상태로 남아 있고 내부 job graph만 drain되는 limbo 구간은 막지 않는다. Native Highlight와 달리 PlayerModifiers에는 tracked-empty/settle gate가 없다. M2에서 no-spread가 추가되어 active mask도 `0x1`에서 `0x3`으로 넓어졌고, 이 조합의 clean save-load 반복 검증은 아직 완료되지 않았다.

따라서 현재 증거가 지지하는 설명은 다음과 같다.

> 로딩 전환기에 PlayerModifiers가 외형상 유효한 player/weapon owner를 통해 엔진 호출 또는 handle release를 수행했고, 그 전환기 작업이 REDengine job 하나를 완료 불능 상태로 남겼다. 트레이너 콜백은 복귀했지만 이어진 원본 OnTick이 그 job을 영구 대기했다.

이는 **확정 원인**이 아니라 가장 강한 가설이다. 덤프 시점에는 이미 트레이너 호출 스택이 사라졌으므로 `GetLocalPlayer`, `GetItemInSlot`, handle release 중 무엇이 선행했는지 구분할 수 없다. 순수 게임/다른 모드의 scheduler deadlock 가능성도 아직 배제되지 않았다.

## 5. 판정과 권고

### 확정

- CPU 메인 틱 scheduler hang이며 GPU/Present hang이 아니다.
- 대기 중인 job/dependency가 하나이고 두 덤프 사이에 진행이 없다.
- 이전 Native Highlight near-null AV는 발생하지 않았고 해당 경로도 비활성 상태였다.
- 프리즈 당시 PlayerModifiers의 recoil+spread mask `0x3`이 weapon `0x9B721C`에 활성 상태였다.

### 높은 가능성

- 원인은 PlayerModifiers의 로딩 전환기 무게이트 엔진 호출/handle 생명주기다.
- 즉 과거 문제와 **같은 구조적 계열**인 “월드 전환 중 non-null 객체를 usable로 오판”이 다른 기능 경로에서 남아 있었던 것으로 보인다.

### 다음 수정 시 필요한 조치

1. Native Highlight에만 있는 world-empty + repopulate settle gate를 PlayerModifiers를 포함한 모든 main-tick 엔진 소비자에 공유한다.
2. 전환/settle 구간에는 `GetLocalPlayer`, `GetItemInSlot`, Add/Remove 같은 엔진 호출을 만들지 않고, 퇴역 상태를 새 owner에 정리하려 하지 않는다.
3. 로깅과 무관하게 덤프에서 읽을 수 있는 atomic stage marker를 각 reflected call 전후와 handle release 전후에 둔다.
4. 새 프로세스에서 A/B save-load를 `트레이너 없음 → modifiers off → no-recoil만 → no-recoil+no-spread` 순서로 반복한다. Native Highlight는 이번 경로에서 배제되었으므로 별도 통제 변수로 둔다.

이번 분석만으로는 소스 수정이나 실행 중 프로세스 종료를 수행하지 않았다.
