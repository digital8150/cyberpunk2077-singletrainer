#pragma once

#include "framework.h"

// 크래시 직전까지 살아남는 최소 진단 로그. 파일과 OutputDebugStringA에 동시에 기록한다.
namespace Diagnostics
{
    void Initialize(HMODULE module);
    void Shutdown();

    void Log(const char* format, ...);
    void LogHr(const char* operation, HRESULT hr);
    void LogDeviceRemovedData(ID3D12Device* device, const char* trigger);

    const wchar_t* LogPath();
}
