#pragma once

#include "framework.h"

// 크래시 직전까지 살아남는 최소 진단 로그 및 예외/크래시 핸들러(VEH). 파일과 OutputDebugStringA에 동시에 기록한다.
namespace Diagnostics
{
    void Initialize(HMODULE module);
    void Shutdown();

    void Log(const char* format, ...);
    void LogHr(const char* operation, HRESULT hr);
    void LogDeviceRemovedData(ID3D12Device* device, const char* trigger);

    bool WriteMiniDump(PEXCEPTION_POINTERS exceptionInfo = nullptr, const char* reason = "manual");

    // VEH가 기록해 둔 예외를 사람이 읽을 수 있는 로그로 옮긴다. 핸들러 자신은 아무것도 포맷하지 않고
    // 모듈 조회도 하지 않으므로(로더 락/디스크 I/O 금지), 실제 로깅은 이 함수를 부르는 쪽 스레드에서
    // 일어난다. 메인 틱의 5초 cadence에서 호출한다.
    void DrainExceptionLog();

    const wchar_t* LogPath();
}
