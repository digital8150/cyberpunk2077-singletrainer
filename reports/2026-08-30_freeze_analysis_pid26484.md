# PID 26484 하드 프리즈: Phase 2A fresh-acquire로도 막지 못한 네이티브 하이라이트 결함

> **작성 메타데이터**
> - **작성 모델 (Author Model)**: `claude-opus-5`
> - **작성 일시 (Timestamp)**: `2026-08-30 15:12:00 (KST)`
> - **분석 대상 (Target)**: `PID 26484 / Cyberpunk2077.exe / Native Highlight (braindance mode)`
> - **핵심 요약 (Executive Summary)**:
>   - 월드 드레인(`tracked=0`) 시점에 `SetBraindanceModeOnMainTick(false)`가 호출되어 엔진
>     `gameIVisionModeSystem` 내부에서 `0xC0000005` write 폴트가 발생했다. pid30148과 **폴트 주소·대상·트레이너
>     프레임이 모두 동일**하며, Phase 2A의 fresh-acquire 수정이 들어간 빌드에서 재발했다.
>   - `__except`가 예외를 잡고 언와인드까지 성공했는데도 프로세스는 하드 프리즈했다. 폴트가 엔진 동기화
>     상태를 깨뜨렸고, 메인 틱이 게임 자체 스핀 대기 프리미티브에서 코어 하나를 100% 태우며 영구 정지했다.
>     **SEH는 엔진 호출에 대한 안전망이 될 수 없다**는 것이 실측으로 확정됐다.

---

## 1. 개요 및 장애 환경

| 항목 | 값 |
|---|---|
| 게임 | Cyberpunk 2077 2.31 (MO2 usvfs, RED4ext/CET/ArchiveXL/TweakXL 등 로드) |
| PID | 26484 |
| 프로세스 시작 | 14:40:33 |
| 트레이너 주입 | 14:42:31.234 (`trainer=00007FFF397A0000`, modules=189) |
| 주입 빌드 | `build/bin/Release/cp2077_trainer.dll`, 빌드 시각 14:42:19, SHA-256 `F197EAFD…D90DC31F` |
| 폴트 | 14:57:35.328 (주입 후 15분) |
| 관측 시각 | 15:04~15:11 (`Responding=False`, 프로세스 생존, 스레드 103개) |

**주입 빌드가 Phase 2A 빌드임을 바이너리로 확인했다.** `native highlight system unavailable` 문자열은 있고
(`5a1825a "Fix native highlight lifetime across world transitions"`, 14:17 커밋 → 14:42 빌드),
후속 world-gate 완화책의 `native highlight world gate` 문자열은 사고 시점 기준 `build/`·`build-next/`
**양쪽 DLL 모두에 없었다**. 즉 이번 프리즈는 fresh-acquire 수정만 적용된 상태에서 발생했다.

> **사후 갱신 (15:17)**: world-gate는 이 사고 이후
> `215fb70 "Harden world transitions in main-tick features"`로 커밋되었고
> `build-next/bin/Release/cp2077_trainer.dll`(15:15 빌드)에는 포함되어 있다. 프리즈를 일으킨
> `build/`(14:42)에는 여전히 없다. **이 결함에 대한 인게임 검증은 아직 이루어지지 않았다.**

## 2. 수집된 아티팩트 및 로그 분석

- 미니덤프 2개: 15:05(A), 15:08(B) — `MiniDumpWriteDump`로 살아 있는 프리즈 프로세스에서 직접 확보.
- 전체 스레드 스택 스냅샷 2개: 15:04(A), 15:10(B) — `tools/scripts/threadstacks.py`, 트레이너 PDB 심볼 포함.
- `cp2077_fatal.log`(pid 26484 세션), 트레이너 로그 세션 구간(라인 44220~46494).

### 2.1 치명적 폴트 기록

```text
[FATAL][veh] 14:57:35.328 pid=26484 tid=32932
  code=0xC0000005 EXCEPTION_ACCESS_VIOLATION
  at=0x00007FF73E6A37C8 (Cyberpunk2077.exe+0xA037C8)
  access=WRITE target=0x0000000000000049
  fault-in-trainer=no
  rcx=0x000001DB856FCCE0   rsp=0x00000081D97FC440
  stack:
    +0x28 cp2077_trainer.dll+0x14972
    +0x48 cp2077_trainer.dll+0x9218
    +0x68 cp2077_trainer.dll+0x6ECB5
    +0x78 cp2077_trainer.dll+0x12C2D
```

PDB 심볼화 결과 (`build/bin/Release/cp2077_trainer.pdb`, 주입 바이너리와 동일 빌드):

```text
+0x14972   `anonymous namespace'::SetBraindanceModeOnMainTick+0x132   entity_tracker.cpp:1514
+0x12C2D   `anonymous namespace'::ProcessNativeHighlightsOnMainTick+0x3ED  entity_tracker.cpp:1615
+0x6ECB5   문자열 리터럴 "gameIVisionModeSystem"
+0x9218    Diagnostics::Profile::Now+0x18   profiling.cpp:142
```

**pid30148(12:59) 보고서와 동일한 폴트다.** 당시 기록은
`at=Cyberpunk2077.exe+0xA037C8`, `access=WRITE target=0x49`, `SetBraindanceModeOnMainTick` +
`ProcessNativeHighlightsOnMainTick` 프레임이었고, 이번 것과 모듈 오프셋·대상 주소·프레임 구성이 전부 같다
(트레이너 RVA만 재빌드로 이동).

`[FATAL][unhandled]`은 **없다**. 프로젝트 규칙대로 이 폴트는 first-chance이며, 프로세스를 죽인 예외가 아니라
프리즈로 이어진 예외다.

### 2.2 폴트 직전 로그

```text
14:57:31.686  ESP diagnostics: snapshots=40 categories[civilian=20 enemy=3 police=1 other=16] ...
              nativeHighlight[queued=111 cleared=20 failures=0]
14:57:35.085  entity unregister observed: total=2048 ...
14:57:35.302  ESP diagnostics: snapshots=0 tracked=0 pendingPosition=0 unregistered=2223 staleRemoved=0
14:57:35.328  [FATAL][veh] ... (26 ms 뒤)
```

트래커가 40 → **0**으로 비워진 직후 26 ms 만에 폴트가 났다. 세이브 로드/월드 전환의 드레인 구간이다.
마지막 `native highlight braindance mode:` 로그는 14:57:21.443이며, 이번 호출은 로그를 남기지 못했다 —
`setBraindanceMode()` 호출이 반환하지 못했고 그 다음 줄이 `Diagnostics::Log`이기 때문이다.

## 3. 콜스택 및 덤프 정밀 분석

### 3.1 프리즈 상태 실측 (스냅샷 A vs B, 약 6분 간격)

- **스레드 103개 중 스택이 바뀐 것은 단 한 줄.** 전체 diff가 2줄(`<`/`>`)이며, 그 내용은 tid 32932의 RIP뿐이다.
  RSP(`0x00000081D97FF1E0`)와 24개 프레임 전부 6분간 완전히 동일하다.
- **CPU: 3초 동안 정확히 3초 소모.** 코어 하나가 100% 점유된 스피너가 정확히 하나 있다는 뜻이다.
- 나머지 스레드 RIP 분포: `ntdll.dll` 101개, `win32u.dll` 1개, `audioware.dll` 1개 — 전부 대기다.

### 3.2 스피너의 정체

```text
=== tid 32932 ===   (메인 틱 스레드, mainTickTid=32932)
  RIP=00007FF8BC79A925 → 00007FF8BC79A90D   ntdll.dll+0x1A925 → +0x1A90D
  RSP=00000081D97FF1E0 (불변)
    Cyberpunk2077.exe+0x14AC7B
    Cyberpunk2077.exe+0x14AF14
    ...
    cp2077_trainer.dll+0x17D15  `anonymous namespace'::HookOnTick+0x125   visibility.cpp:355
    RED4ext.dll+0x82020
```

`ntdll+0x1A8E0` 영역의 바이트를 살아 있는 프로세스에서 직접 읽어 확인했다:

```text
+0x02  mov r11d, 0x7FFE03B8        ; KUSER_SHARED_DATA
+0x2A  rdtscp
+0x2D  shl rdx, 32                 ; <- 스냅샷 B의 RIP
+0x45  cmp eax, r10d               ; <- 스냅샷 A의 RIP
+0x48  jne -0x2D                   ; 시퀀스 불일치 시 재시도(역방향)
```

`RtlQueryPerformanceCounter`의 rdtscp + KUSER_SHARED_DATA 시퀀스 재시도 루프다. 즉 스레드가 ntdll에
갇힌 게 아니라, **한 단계 위의 `Cyberpunk2077.exe+0x14AC7B` / `+0x14AF14` 스핀 대기 프리미티브가 매
반복마다 QPC를 읽고 있는 것**이다. 이 두 주소는 `progress.md`에서 이미 "`lock cmpxchg` + 역방향 점프,
함수 포인터 predicate를 호출하는 재시도 루프 = 게임 자체의 스핀 대기 프리미티브"로 식별된 바로 그 주소다.

### 3.3 SEH는 동작했다. 그런데도 얼었다

폴트 시점 `rsp=0x00000081D97FC440`, 현재 `RSP=0x00000081D97FF1E0`. 현재 RSP가 **더 높다** — 스택이
언와인드되어 폴트 지점보다 얕은 곳으로 빠져나왔다는 뜻이다. `entity_tracker.cpp:1547~1560`의
`__try/__except (EXCEPTION_EXECUTE_HANDLER)`가 예외를 잡고 정상 복귀했다.

그런데도 프로세스는 얼었다. 언와인드된 스레드는 여전히 `HookOnTick` 아래에서 엔진 스핀 대기에 갇혀 있고,
그 predicate는 6분이 지나도 성립하지 않는다. **폴트가 엔진 내부 동기화/참조 상태를 중간에 끊어놓았고,
SEH 언와인드는 엔진이 잡고 있던 것을 되돌려주지 못한다.**

## 4. 근본 원인 (Root Cause)

1. **직접 원인** — 주입 빌드(Phase 2A)의 `ProcessNativeHighlightsOnMainTick`은 트래커가 비어도 mode-off를
   호출한다. 해당 소스(`git show 5a1825a`)의 판정은 다음과 같다:

   ```text
   1614:  const bool modeDesired = anyDesired || cachedDesired;
   1635:  if (!anyDesired && !HasDesiredHighlightState())
   1636:      SetBraindanceModeOnMainTick(false);
   ```

   `tracked=0`인 드레인 틱에서는 `anyDesired=false`, `HasDesiredHighlightState()=false`가 되어
   **퇴역 중인 월드의 vision system에 mode=0을 호출**한다.

2. **Phase 2A 수정이 왜 부족했나** — 2A는 raw 포인터를 틱 사이에 보존하지 않고 호출 직전에
   `GetSystemOnMainTick(gameInstance, Hash("gameIVisionModeSystem"))`로 다시 얻도록 했다. 이번에도 그렇게
   **막 획득한** 포인터(`rcx=0x000001DB856FCCE0`, 널 아님)로 호출했는데 폴트가 났다. 폴트 대상이
   `0x49`, 즉 **베이스 0에 오프셋 0x49를 쓰는 near-null write**다. 시스템 객체 자체는 살아 있으나 그
   내부 멤버(월드/블랙보드 계열 포인터)가 드레인 중 이미 널이었다는 뜻이다.

   > **포인터의 신선도(freshness)는 사용 가능성(usability)을 보장하지 않는다.** 월드 전환 중에는 시스템이
   > non-null이면서도 바인딩이 풀린 상태로 존재할 수 있다. 이것이 2A가 놓친 구멍이다.

3. **프리즈로 번진 경로** — `__except`가 예외를 삼켰지만 엔진 상태는 이미 깨졌고, 이후 엔진 스핀 대기가
   영구화되어 메인 틱이 멈췄다. 메인 틱이 멈추면 나머지 102개 스레드가 전부 대기에 잠긴다
   (`reports/INSIGHTS.md` 1.4의 패턴 그대로).

## 5. 해결 방안 및 권고사항

### 5.1 즉시

1. **드레인 틱에서는 엔진 호출을 하지 않는다.** `trackedCount == 0`이면 mode-off를 호출하지 말고 로컬
   latch(`g_nativeHighlightModeActive`)만 내린다. 퇴역하는 월드의 하이라이트를 굳이 꺼줄 필요가 없다 —
   그 월드는 사라진다.
2. **재populate 후 settle window.** 트래커가 다시 채워져도 곧바로 호출하지 말고 바운드된 대기 구간을 둔다.

   > `215fb70 "Harden world transitions in main-tick features"`(15:17)의 `worldWasEmpty` /
   > `worldSettleUntil` / `native highlight world gate`가 정확히 이 두 가지를 구현하고 있고
   > `build-next`(15:15 빌드)에 들어 있다. **다만 이 결함에 대한 인게임 검증은 아직 없다.**
   > 이 보고서는 그 방향이 옳다는 실측 근거를 제공한다.

3. **`__except`를 안전 근거로 삼지 않는다.** 이번 건은 SEH가 정상 동작했는데도 하드 프리즈가 난 실측
   사례다. pid30148 보고서의 권고 4("SEH를 정상 lifetime 제어 흐름으로 쓰지 말 것")를 규칙으로 승격하고,
   엔진 호출은 **사전 조건 검사로 회피**해야 한다. 남은 `__except`는 최후 방어선일 뿐 라이선스가 아니다.

### 5.2 구조적

4. **동일 원칙을 다른 시스템 캐시에도 적용.** `progress.md`가 이미 Phase 2B/2C로 분리해 둔
   `statPoolsSystem` / `playerSystem` / `spatialQueriesSystem` / no-recoil 계열도 fresh-acquire만으로는
   부족하다. 드레인·전환 구간을 감지해 호출 자체를 건너뛰는 게이트가 함께 필요하다.
5. **월드 전환 감지를 하이라이트 로컬이 아니라 트래커 공통 신호로 올린다.** `tracked=0`은 이미 ESP 진단에
   찍히고 있으므로, 전환 상태를 한 곳에서 판정해 모든 메인 틱 소비자가 공유하는 편이 중복 게이트보다 낫다.

### 5.3 검증 방법

6. world gate가 포함된 `build-next/bin/Release/cp2077_trainer.dll`(15:15 빌드, `215fb70`)을 게임을 재시작한
   뒤 주입하고, 세이브 로드/패스트 트래블을 **반복적으로** 수행하면서
   `native highlight world gate: empty tracked=0` → `repopulated tracked=N settleMs=…` 로그가 나오고
   `[FATAL][veh]`가 나오지 않는지 확인한다. 이번 프리즈는 주입 15분·전환 1회 만에 재현됐으므로 재현 비용이
   낮다.
7. 회귀 감시는 `[FATAL][veh]`의 `at=Cyberpunk2077.exe+0xA037C8` / `target=0x49` 조합을 시그니처로 삼으면
   된다. 두 번의 사고에서 동일하게 나타났다.

---

## 보존된 아티팩트

`reports/artifacts/2026-08-30_freeze_pid26484/` (`.dmp`는 `.gitignore` 대상):

- `cp2077_freeze_26484_a.dmp` (15:05), `cp2077_freeze_26484_b.dmp` (15:08)
- `threadstacks_a_15-04.txt`, `threadstacks_b_15-10.txt`
- `cp2077_fatal.log`, `cp2077_trainer_session_pid26484.log`
