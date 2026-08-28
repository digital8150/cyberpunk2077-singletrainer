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
