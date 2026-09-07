# PID 3180 — Silent Aim WeakHandle 조작 후 엔티티 해제 크래시

> **작성 메타데이터**
> - **작성 모델 (Author Model)**: `Claude Opus 4.6 (Thinking)`
> - **작성 일시 (Timestamp)**: `2026-09-07 14:47 (KST)`
> - **분석 대상 (Target)**: `pid=3180 / Cyberpunk2077.exe + cp2077_trainer.dll`
> - **핵심 요약 (Executive Summary)**:
>   - Silent Aim의 `HandleSpawnerLaunchEvent`가 `gameprojectileSpawnerLaunchEvent` 이벤트 슬롯의 `WeakHandle<IPlacedComponent>` (+0x0F0) refCount를 `InterlockedIncrement`로 조작한 뒤, 해당 엔티티가 게임 엔진에 의해 해제될 때 `Cyberpunk2077.exe+0x25161B`에서 dangling weak ref 역참조로 ACCESS_VIOLATION이 발생하여 프로세스가 즉사(unhandled exception).
>   - **수정 시도(commit `8a76331`)는 크래시를 방지하지 못했음**. WeakHandle 조작 로직 자체가 근본 원인이며, 현재 접근 방식은 구조적으로 안전하지 않다.

---

## 1. 장애 현상 및 환경

### 1.1 타임라인

| 시각 (KST) | 이벤트 |
|---|---|
| 14:42:07 | PID 3180 세션 시작. 트레이너 DLL 주입 완료 (`cp2077_trainer.dll @ 0x7FFCBB2A0000`) |
| 14:42:07 | Silent aim 훅 생성: `crosshairCore=1`, `queueHook=1`, `projectileListeners=1` |
| 14:42:15 | Silent aim 첫 타겟 armed: `entity=0x896893` |
| 14:42:17 | 첫 번째 spawner launch redirect (`count=1`, `targetComp=000001C440AC98C0`, `guided=1`) |
| 14:42:24 | 두 번째 redirect (`count=2`, `targetComp=000001C443CA0400`) |
| 14:42:36 | 세 번째 redirect (`count=3`, `targetComp=000001C443CA0400`) |
| 14:42:54 | 네 번째 redirect (`count=4`, `targetComp=000001C440AC98C0`) |
| 14:42:59 | 다섯 번째 redirect (`count=5`, `targetComp=000001C43EB4DD00`) — **tid=30276** |
| 14:43:00 | 엔티티 해제 관측 (`ptr=000001C86C82CCD0`, `total=2048`) |
| **14:43:18** | **CRASH**: `Cyberpunk2077.exe+0x25161B` ACCESS_VIOLATION, **tid=30276** |

### 1.2 환경

- 게임: Cyberpunk 2077 (DX12)
- 트레이너 DLL: `cp2077_trainer.dll` (commit `8a76331` — "fix: crash fix attempt but failed")
- 이전 세션: `[SESSION] closed cleanly at 14:40:17.757` — 정상 종료 후 재주입

---

## 2. 수집된 아티팩트 및 로그 분석

### 2.1 Fatal 레코드 (VEH + Unhandled)

```
[FATAL][veh] 14:43:18.235 pid=3180 tid=30276
  code=0xC0000005 EXCEPTION_ACCESS_VIOLATION
  at=0x00007FF67EB2161B (Cyberpunk2077.exe+0x25161B)
  access=READ target=0x000000003CFA32BB
  fault-in-trainer=no

[FATAL][unhandled] 14:43:18.235 pid=3180 tid=30276
  code=0xC0000005 EXCEPTION_ACCESS_VIOLATION
  at=0x00007FF67EB2161B (Cyberpunk2077.exe+0x25161B)
  access=READ target=0x000000003CFA32BB
  fault-in-trainer=no
```

**핵심 관측:**

1. **`fault-in-trainer=no`**: 폴트 지점은 트레이너 코드가 아니라 **게임 엔진 내부** (`Cyberpunk2077.exe+0x25161B`).
2. **`[FATAL][unhandled]` 동반**: 이 예외는 게임 자체의 복구 경로로도 처리되지 못해 프로세스를 죽였음.
3. **`target=0x000000003CFA32BB`**: 유효 유저모드 범위에 있지만, 정상 힙 주소로는 매우 낮은 값. **해제 후 재사용(UAF) 또는 ref-count 관련 메타데이터가 깨진 포인터의 전형적 패턴**.
4. **`rcx=0x000000003CFA2FF2`**: 크래시 시 `rcx`가 `target`과 매우 가까움 (`0x3CFA2FF2` + 오프셋 `0x2C9` = `0x3CFA32BB`). 이는 이미 해제된 refcount 블록의 포인터를 역참조하여 필드에 접근하다 실패한 것.
5. **`tid=30276`**: 마지막 spawner launch redirect(count=5)를 실행한 것과 **같은 스레드**.

### 2.2 크래시 콜스택

```
at: Cyberpunk2077.exe+0x25161B    ← 폴트 지점: entity/component 해제 경로
    Cyberpunk2077.exe+0x44804A    ← 상위 프레임
    Cyberpunk2077.exe+0x14A700    ← 메인 틱 dispatcher (기존 분석에서 식별)
    Cyberpunk2077.exe+0x1493A1
    Cyberpunk2077.exe+0x2436E0
    Cyberpunk2077.exe+0x14BAA2
    Cyberpunk2077.exe+0x14A218
    Cyberpunk2077.exe+0xFA2AD8
    Cyberpunk2077.exe+0xB80822
    Cyberpunk2077.exe+0x4001438
    Cyberpunk2077.exe+0xFF3B8
    Cyberpunk2077.exe+0x128739
    Cyberpunk2077.exe+0x14A700    ← 재귀/반복 프레임
```

`+0x14A700`이 스택 상단과 하단에 모두 나타나는 것은 이 주소가 게임의 **main tick dispatcher** 역할임을 강하게 시사한다 (기존 프리즈 분석들에서도 이 근처 주소가 메인 틱 루프로 식별됨).

### 2.3 Trainer Log 최종 상태

```
[14:43:16.915] ESP diagnostics: snapshots=128 ... nativeHighlight[queued=0 cleared=0 failures=0]
```

로그가 `14:43:16.915`에서 끊김 → 크래시 시각 `14:43:18.235`와 일치. `[SESSION] closed cleanly` 줄 없음 → 비정상 종료 확정.

---

## 3. 콜스택 및 메모리 상태 정밀 분석

### 3.1 폴트 대상 주소의 특성

```
target = 0x000000003CFA32BB
rcx    = 0x000000003CFA2FF2
offset = target - rcx = 0x2C9
```

- **정상 힙 주소**: 이 세션의 게임 객체 포인터는 `0x000001C4XXXXXXXX` 대역 (48-bit canonical, 높은 주소)
- **폴트 주소**: `0x000000003CFA32BB` (32-bit 범위, 비정상적으로 낮음)

이 주소는 refcount 블록이 해제된 후 해당 메모리가 OS에 의해 **decommit 또는 재사용**된 상태에서 원래 포인터를 역참조한 결과. `rcx`의 값이 `rsi`와 동일(`0x3CFA2FF2`)한 것으로 보아, 함수에 인자로 전달된 `this`(객체 포인터)가 이미 dangling이었다.

### 3.2 스레드 상관관계

| 항목 | tid |
|---|---|
| 마지막 spawner redirect (count=5, 14:42:59) | **30276** |
| 크래시 발생 (14:43:18) | **30276** |

동일 스레드에서 **WeakHandle 조작 → 엔티티/이벤트 해제 경로 → 크래시** 순서가 성립.

---

## 4. 근본 원인 (Root Cause)

### 4.1 문제의 코드: WeakHandle refcount 조작

commit `8a76331`에서 추가된 핵심 코드 ([silent_aim.cpp](file:///E:/repos/cyberpunk2077-singletrainer/src/game/silent_aim.cpp)):

```cpp
// HandleSpawnerLaunchEvent 내부 — kSpawnerTrackedTargetCompOffset (0x0F0) 슬롯 조작
auto* targetCompSlot = reinterpret_cast<Game::Rtti::Handle*>(bytes + kSpawnerTrackedTargetCompOffset);
targetCompSlot->instance = targetCompHandle.instance;
targetCompSlot->refCount = targetCompHandle.refCount;
if (targetCompHandle.refCount && Game::Rtti::IsValidUserPointer(targetCompHandle.refCount))
{
    auto* newWeakRefs = reinterpret_cast<volatile LONG*>(
        static_cast<std::byte*>(targetCompHandle.refCount) + 4);
    InterlockedIncrement(newWeakRefs);
}
```

**이 코드가 위험한 세 가지 이유:**

#### (a) Refcount 불균형 — 이벤트 소유권 계약 위반

주석에 "the game's event destructor handles the old slot's weak decrement"라고 썼지만, 이 가정은 **미검증 추측**이다. 게임의 이벤트 소멸자가 **정확히 어떤 시점에, 어떤 스레드에서** old slot의 weak ref를 감소시키는지 알 수 없다. 시나리오:

1. `HandleSpawnerLaunchEvent`가 슬롯을 덮어쓰고 new target의 weak ref를 1 증가
2. 이벤트 소멸자가 실행되기 **전에** 타겟 엔티티/컴포넌트가 해제됨
3. 우리가 증가시킨 weak ref가 **마지막 참조**로 남아, 엔진의 weak ref 해제 경로에서 이미 해제된 메모리를 역참조

#### (b) 멀티스레드 Race — 이벤트 슬롯의 비원자적 교체

`Handle`(instance + refCount)은 16바이트 구조체. 두 번의 포인터 쓰기로 교체하는 사이에 다른 스레드가 이벤트를 소비하면 **torn read** 발생:
- 스레드 A: `instance = new_instance` (완료)
- 스레드 B: 이벤트를 읽음 → `instance=new, refCount=old` → **불일치 상태**
- 스레드 A: `refCount = new_refCount` (뒤늦음)

#### (c) 타겟 컴포넌트 생명주기 미보증

`FindTargetingComponent`에서 획득한 `targetCompHandle`은 스냅샷 시점의 raw 포인터. `PublishTarget` → `ReadTarget` → `HandleSpawnerLaunchEvent` 체인을 거치는 동안 **해당 컴포넌트가 해제될 수 있다**. `IsValidUserPointer` 범위 검사는 UAF를 막지 못함 (INSIGHTS.md §1.2 참조).

### 4.2 크래시 메커니즘 재구성

```
시간 흐름 →

14:42:59.527  tid=30276: HandleSpawnerLaunchEvent count=5
              ├─ targetCompSlot->instance = 000001C43EB4DD00
              └─ InterlockedIncrement(weakRefs of 000001C43EB4DD00)  ← weak ref +1

14:43:00 ~    게임 워커 스레드들: 엔티티 해제 진행 (total=2048 관측)
              ├─ 타겟 컴포넌트 000001C43EB4DD00의 strong ref → 0
              ├─ weak ref는 우리가 올린 1이 남아 있어 refcount 블록 자체는 일시적으로 생존
              └─ 하지만 instance가 가리키던 실제 객체 메모리는 해제됨

14:43:18.235  tid=30276: 게임 메인 틱 또는 이벤트 소멸자
              ├─ 이벤트의 Handle 슬롯에서 instance 또는 refCount 역참조
              ├─ 이미 해제/재사용된 메모리 → rcx=0x3CFA2FF2 (dangling)
              ├─ READ at target=0x3CFA32BB → ACCESS_VIOLATION
              └─ [FATAL][unhandled] → 프로세스 즉사
```

---

## 5. 해결 방안 및 권고사항

### 5.1 즉시 조치: WeakHandle 조작 전면 제거 (**우선순위 1**)

`HandleSpawnerLaunchEvent`와 `HandleShootEvent` 양쪽에서 **WeakHandle 슬롯 조작(`targetCompSlot` 쓰기 + `InterlockedIncrement`) 코드를 전부 제거**한다.

제거 대상:
- `silent_aim.cpp`: `targetCompSlot->instance` / `targetCompSlot->refCount` 쓰기 + `InterlockedIncrement` 블록 (spawner launch 경로)
- `silent_aim.cpp`: shoot event 경로의 동일 로직 (`0x140` 오프셋 Handle 쓰기)
- `silent_aim.cpp`: `g_state.targetComponentInstance` / `g_state.targetComponentRefCount` atomic 상태 전체
- `entity_tracker.cpp`: `FindTargetingComponent()` 함수 및 `TrackedPuppet::targetingComponent` 필드
- `entity_tracker.h`: `PuppetSnapshot::targetingComponent` 필드
- `silent_aim.h`: `PublishTarget` 시그니처에서 `targetComponent` 파라미터
- `aimbot.cpp`: `PublishTarget` 호출에서 `targetComponent` 인자

이유:
- 엔진의 ref-counting 계약을 외부에서 정확히 모사할 수 없으며, 불균형은 UAF 또는 double-free로 직결
- 게임의 이벤트 소멸자가 우리가 넣은 포인터를 처리할 때의 동작이 미검증
- 이미 이 접근법이 실패했음이 이 크래시로 실증됨

### 5.2 대안: 순수 좌표 기반 리다이렉션만 유지

`targetPos` 변경(Vector4 좌표 덮어쓰기)과 `smartGunIsProjectileGuided=true` 플래그, 그리고 `RedirectOrientationProviderSafely`(quaternion 방향 조작)만으로 발사체 유도가 충분한지 먼저 테스트한다. Handle 계열 조작 없이도 발사체가 타겟으로 향하면, 위험한 ref-count 조작의 필요성 자체가 사라진다.

### 5.3 `__except` SEH 의존 경고

`HandleSpawnerLaunchEvent`에 `__except (EXCEPTION_EXECUTE_HANDLER)` 가드가 있지만, INSIGHTS.md §1.1에서 실증된 바와 같이 **SEH는 엔진 호출의 안전망이 될 수 없다**. 이번 크래시는 트레이너 코드 밖(게임 엔진 내부)에서 발생했으므로 SEH 가드로는 잡을 수 없었다.

---

## 📎 인사이트는 이 문서에 쓰지 않습니다

이 보고서에는 이번 사건에 대한 분석만 기록합니다. 조사 중에 얻은 재사용 가능한 디버깅 힌트는 [INSIGHTS.md](file:///E:/repos/cyberpunk2077-singletrainer/reports/INSIGHTS.md)에 추가합니다.
