// DLL 진입점. 인젝션되면 여기서 시작한다.
//
// DllMain 안에서 직접 무거운 초기화(D3D12 디바이스 생성 등)를 하면 로더 락 문제로 데드락이 나기 쉬우므로,
// 실제 초기화는 별도 스레드(MainThread)에서 수행한다.
#include "framework.h"
#include "diagnostics.h"
#include "hooks/d3d12_hook.h"

namespace
{
    HMODULE g_hModule = nullptr;

    DWORD WINAPI MainThread(LPVOID parameter)
    {
        Diagnostics::Initialize(static_cast<HMODULE>(parameter));
        Diagnostics::Log("initialization thread started");
        Hooks::Initialize();
        Diagnostics::Log("initialization thread finished");
        return 0;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        if (HANDLE thread = CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr))
            CloseHandle(thread);
        break;

    case DLL_PROCESS_DETACH:
        Hooks::Shutdown();
        Diagnostics::Shutdown();
        break;
    }
    return TRUE;
}
