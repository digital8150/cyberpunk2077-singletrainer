# PID 15808 — Orientation Provider 오염으로 인한 엔티티 소멸자 크래시 정밀 분석

> **작성 메타데이터**
> - **작성 모델 (Author Model)**: `Gemini 3.8 Flash (High)`
> - **작성 일시 (Timestamp)**: `2026-09-07 15:45 (KST)`
> - **분석 대상 (Target)**: `pid=15808 / Cyberpunk2077.exe + cp2077_trainer.dll`
> - **핵심 요약 (Executive Summary)**:
>   - 이전 분석(`reports/2026-09-07_crash_analysis_pid3180.md`)은 `Cyberpunk2077.exe+0x25161B` 크래시의 원인을 `WeakHandle` 조작으로 추정했으나, **이 가설은 오진이었음이 실증됨**.
>   - 실제 원인은 `RedirectOrientationProviderSafely`가 `provider + 0x30`에 쿼터니언 float 4개(`qx, qy, qz, qw`)를 덮어쓰면서 발생한 **엔진 내부 포인터 오염**.
>   - `provider + 0x30`은 쿼터니언이 아니라 하위 엔진 객체를 가리키는 **64-bit 포인터**였음. `qy = 0.0f` (0x00000000)와 `qx` (IEEE-754 float 비트열)가 64-bit 포인터 자리를 덮어써 `0x000000003A9F3E18`(PID 15808) 및 `0x000000003CFA2FF2`(PID 3180)라는 가짜 주소를 형성함.
>   - 이벤트/프로바이더 소멸자(`0x14014A200` -> `0x14014934C`)가 `[rcx + 0x30]`을 포인터로 읽어 `0x140251600`을 호출했고, 해당 함수 첫 줄 `test byte ptr [rcx + 0x2C9], 1`에서 `0x3A9F3E18 + 0x2C9 = 0x3A9F40E1`을 역참조하며 `ACCESS_VIOLATION` 즉사 크래시 발생.

---

## 1. 장애 현상 및 환경

### 1.1 환경
- 게임: Cyberpunk 2077 (DX12)
- 트레이너 DLL: `cp2077_trainer.dll` (commit `f1ac368` — WeakHandle 제거 빌드)
- 크래시 시각: 15:39:26.563 KST

### 1.2 Fatal 레코드 (VEH + Unhandled)

```
[FATAL][veh] 15:39:26.563 pid=15808 tid=16176
  code=0xC0000005 EXCEPTION_ACCESS_VIOLATION
  at=0x00007FF67EB2161B (Cyberpunk2077.exe+0x25161B)
  access=READ target=0x000000003A9F40E1
  fault-in-trainer=no
  rip=0x00007FF67EB2161B rsp=0x0000001D9A6FF5F0 rbp=0x0000000000000000
  rax=0x0000001D9A6FF688 rbx=0x0000014044BBCD80 rcx=0x000000003A9F3E18 rdx=0x0000000000000000
  rsi=0x000000003A9F3E18 rdi=0x0000014044BBCD00 r8=0x0000000000003F30 r9=0x0000000000000000
  stack:
    +0x28 0x00007FF67ED1804A (Cyberpunk2077.exe+0x44804A)
    +0x58 0x00007FF67EA193A1 (Cyberpunk2077.exe+0x1493A1)
    +0x98 0x00007FF67EA1A218 (Cyberpunk2077.exe+0x14A218)
    +0xC8 0x00007FF67F872AD8 (Cyberpunk2077.exe+0xFA2AD8)
    +0xF8 0x00007FF67F450822 (Cyberpunk2077.exe+0xB80822)
    +0x100 0x00007FF6828D1438 (Cyberpunk2077.exe+0x4001438)
```

---

## 2. 디어셈블리 및 근본 원인 규명

### 2.1 크래시 지점 (`Cyberpunk2077.exe+0x25161B`)

```x86asm
0x140251600: mov      qword ptr [rsp + 0x10], rbx
0x140251605: mov      qword ptr [rsp + 0x20], rbp
0x14025160A: push     rsi
0x14025160B: push     rdi
0x14025160C: push     r12
0x14025160E: push     r14
0x140251610: push     r15
0x140251612: sub      rsp, 0x30
0x140251616: xor      ebp, ebp
0x140251618: mov      dil, dl
0x14025161B: test     byte ptr [rcx + 0x2c9], 1   <-- [FAULT: rcx=0x3A9F3E18, target=0x3A9F40E1]
0x140251622: mov      rsi, rcx
0x140251625: je       0x140251642
```

함수 시작 시점에 `rcx`의 오프셋 `+0x2C9` 플래그를 검사합니다.

### 2.2 호출자 (`Cyberpunk2077.exe+0x1493A1`) 및 소멸자 체인

크래시 시점 `rsp`는 5개 레지스터 push(0x28) + `sub rsp, 0x30`으로 인해 원래 caller의 반환 주소 위치가 `rsp + 0x58`입니다. 스택 덤프의 `+0x58`에 기록된 주소는 **`Cyberpunk2077.exe+0x1493A1`**입니다:

```x86asm
0x14014938A: mov      rsi, qword ptr [rcx + 0x30]  ; [rcx + 0x30] 포인터 로드!
0x14014938E: test     rsi, rsi
0x140149391: je       ...
0x140149397: xor      edx, edx
0x140149399: mov      rcx, rsi                     ; rsi를 첫 번째 인자로 전달
0x14014939C: call     0x140251600                  ; sub_251600 호출!
0x1401493A1: mov      eax, dword ptr [rsi + 0x124] <-- [RETURN ADDRESS]
```

그리고 이 함수는 `0x14014A200` (소멸자)에서 호출됩니다:
```x86asm
0x14014A200: push     rbx
0x14014A202: sub      rsp, 0x20
0x14014A206: lea      rax, [rip + 0x2971ceb]       ; vftable 0x142ABBEF8
0x14014A210: mov      qword ptr [rcx], rax
0x14014A213: call     0x14014934c
0x14014A218: mov      rcx, rbx
```

### 2.3 오염된 포인터의 정체: IEEE-754 부동소수점 비트열

트레이너의 `FromForwardVector`와 `RedirectOrientationProviderSafely`:
```cpp
QuaternionLayout FromForwardVector(float dx, float dy, float dz)
{
    ...
    float qx = nz;
    float qy = 0.0f;  // <-- 항상 0.0f
    float qz = -nx;
    float qw = 1.0f + ny;
    return {qx, qy, qz, qw};
}

void RedirectOrientationProviderSafely(void* provider, const QuaternionLayout& quat)
{
    auto* quatPtr = reinterpret_cast<QuaternionLayout*>(static_cast<std::byte*>(provider) + 0x30);
    *quatPtr = quat; // provider + 0x30에 16바이트 쓰기!
}
```

`provider + 0x30` 위치에 쿼터니언을 쓰면 메모리에 다음과 같이 배치됩니다:
- `+0x30 ~ +0x33` (4바이트): `qx` (float)
- `+0x34 ~ +0x37` (4바이트): `qy` (float, `0.0f` = `0x00000000`)
- `+0x38 ~ +0x3F` (8바이트): `qz`, `qw`

이를 64비트 정수(포인터)로 읽으면:
`pointer = (uint64_t(as_uint(qy)) << 32) | as_uint(qx)`
`qy == 0.0f`이므로 **상위 32비트는 항상 `0x00000000`**이 됩니다!

실제 크래시 레코드의 `rcx` 값과 비트 일치 검증:
1. **PID 15808**:
   - `rcx = 0x000000003A9F3E18`
   - 하위 32비트 `0x3A9F3E18` -> `float 0.00121492` (정규화된 `nz` = `qx`)
   - 상위 32비트 `0x00000000` -> `float 0.0f` (`qy`)
2. **PID 3180**:
   - `rcx = 0x000000003CFA2FF2`
   - 하위 32비트 `0x3CFA2FF2` -> `float 0.03054044` (`qx`)
   - 상위 32비트 `0x00000000` -> `float 0.0f` (`qy`)

**결론: `rcx`는 dangling refcount 블록이 아니라, 우리가 `FromForwardVector`로 계산해 `provider + 0x30`에 쓴 `qx`, `qy` 부동소수점 비트열 그 자체였습니다.**

---

## 3. 해결 조치

1. `HandleSpawnerLaunchEvent`에서 Orientation Provider 조작 전면 제거:
   - `logicalHandle` (+0x058), `visualHandle` (+0x078) 역참조 및 `RedirectOrientationProviderSafely` 호출 삭제.
   - `QuaternionLayout`, `FromForwardVector`, `RedirectOrientationProviderSafely` 함수 자체 삭제.
   - `kSpawnerGuidedOffset` (+0x104) 및 shootEvent의 `0x154` 임의 플래그 조작 삭제.
2. `HandleSpawnerLaunchEvent`를 commit `7396ba6`의 검증된 안전한 상태(`targetPos` +0xD0만 갱신)로 복구.
3. 카메라 불필요 필드(`cameraX`, `cameraY`, `cameraZ`, `cameraValid`) 및 `#include "projection.h"` 제거.

---

## 4. 교훈 및 재발 방지 (INSIGHTS.md 연계)

- 크래시 레코드의 비정상적 32비트 주소(`0x00000000XXXXXXXX`)를 단순히 UAF로 단정하지 말고, 부동소수점 비트열 해석을 반드시 교차 검증할 것.
- 엔진의 미확인 객체 내부 오프셋에 임의로 구조체나 쿼터니언을 덮어쓰는 행위는 소멸자/정리 경로에서 심각한 메모리 오염을 유발함.
