#pragma once

#include "framework.h"

// 크래시 직전까지 살아남는 최소 진단 로그 및 예외/크래시 핸들러(VEH).
//
// Log()는 호출자 스레드에서 포맷만 하고 링 버퍼에 넣은 뒤 즉시 반환한다. 파일 쓰기와
// FlushFileBuffers는 전용 writer 스레드가 배치로 처리하므로 Present 스레드나 게임 메인 틱이
// 디스크를 기다리지 않는다. 환경 변수:
//   CBPK_LOG=0      진단 로그를 통째로 끈다 (Log()가 원자 로드 하나로 반환).
//   CBPK_LOG_DIR    로그/덤프 디렉터리 지정. 기본값은 %LOCALAPPDATA%\cp2077_trainer\.
//   CBPK_DBGOUT=1   OutputDebugStringA 출력을 켠다 (기본 off: 줄마다 SEH 예외가 든다).
//   CBPK_VEH=1      예외 관측용 VEH를 등록한다.
//   CBPK_FATAL=0    치명적 폴트 직기록을 끈다 (기본 on).
//
// 치명적 폴트는 위의 비동기 경로를 타지 않는다. 링은 다음 메인 틱에서야 비워지는데 프로세스를 죽이는
// 폴트에는 그 틱이 오지 않기 때문이다. 대신 미리 열어 둔 전용 핸들(`cp2077_fatal.log`)에 곧바로 쓴다.
// 그 경로는 CRT도 할당도 로더 락도 쓰지 않고, 프로세스 수명 전체에 쓰기 예산이 걸려 있으며, 잠금은
// 기다리지 않는 try-lock 하나뿐이다 -- 로거가 크래시나 프리징의 원인이 되지 않는 것이 유일한 설계 목표다.
// 남는 기록은 두 종류이고, 둘을 비교하면 예외의 성격을 가릴 수 있다:
//   [FATAL][veh]        first-chance. 게임이 스스로 복구한 예외도 여기 찍힌다.
//   [FATAL][unhandled]  아무도 처리하지 않았다. 이 예외가 실제로 프로세스를 죽였다는 확정 신호.
// `[SESSION] closed cleanly` 줄이 없는 세션은 정상 종료가 아니었다는 뜻이다.
namespace Diagnostics
{
    void Initialize(HMODULE module);
    void Shutdown();

    void Log(const char* format, ...);
    // 큐에 쌓인 줄을 디스크까지 밀어낸다. 평소에는 부를 필요가 없다 (writer 스레드가 1초
    // cadence로 알아서 한다). 덤프 직전, 종료 직전처럼 이 다음이 없을지도 모르는 지점에서만 쓴다.
    void Flush();
    void LogHr(const char* operation, HRESULT hr);
    void LogDeviceRemovedData(ID3D12Device* device, const char* trigger);

    bool WriteMiniDump(PEXCEPTION_POINTERS exceptionInfo = nullptr, const char* reason = "manual");

    // VEH가 기록해 둔 예외를 사람이 읽을 수 있는 로그로 옮긴다. 핸들러 자신은 아무것도 포맷하지 않고
    // 모듈 조회도 하지 않으므로(로더 락/디스크 I/O 금지), 실제 로깅은 이 함수를 부르는 쪽 스레드에서
    // 일어난다. 메인 틱의 5초 cadence에서 호출한다.
    void DrainExceptionLog();

    const wchar_t* LogPath();
}
