// DLL 진입점. 인젝션되면 여기서 시작한다.
//
// DllMain 안에서 직접 무거운 초기화(D3D12 디바이스 생성 등)를 하면 로더 락 문제로 데드락이 나기 쉬우므로,
// 실제 초기화는 별도 스레드(MainThread)에서 수행한다.
#include "framework.h"
#include "diagnostics.h"
#include "hooks/d3d12_hook.h"

#include <cstdio>

namespace
{
    HMODULE g_hModule = nullptr;

    DWORD WINAPI MainThread(LPVOID parameter)
    {
        const HMODULE module = static_cast<HMODULE>(parameter);
        Diagnostics::Initialize(module);
        Diagnostics::Log("initialization thread started");
        Hooks::Initialize();

        wchar_t unloadEventName[96]{};
        swprintf_s(unloadEventName, L"Local\\cp2077_trainer_unload_%lu", GetCurrentProcessId());
        HANDLE unloadEvent = CreateEventW(nullptr, TRUE, FALSE, unloadEventName);
        if (unloadEvent)
            Diagnostics::Log("initialization finished; press End or signal the unload event to unload safely");
        else
            Diagnostics::Log("CreateEventW(unload) failed: error=%lu; End hotkey remains available", GetLastError());

        bool endWasDown = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
        for (;;)
        {
            const bool endIsDown = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
            const bool eventSignaled = unloadEvent && WaitForSingleObject(unloadEvent, 0) == WAIT_OBJECT_0;
            if ((endIsDown && !endWasDown) || eventSignaled)
                break;
            endWasDown = endIsDown;
            Sleep(50);
        }

        Diagnostics::Log("safe unload requested");
        while (!Hooks::Shutdown())
        {
            Diagnostics::Log("safe unload is waiting for hook cleanup; retrying in 1000 ms");
            Sleep(1000);
        }

        Diagnostics::Log("safe unload checks passed; releasing DLL");
        if (unloadEvent)
            CloseHandle(unloadEvent);
        Diagnostics::Shutdown();
        FreeLibraryAndExitThread(module, 0);
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
        // Explicit unload is cleaned up by MainThread before FreeLibraryAndExitThread. During process shutdown,
        // doing MinHook/ImGui/D3D work here would run under the loader lock and is intentionally avoided.
        break;
    }
    return TRUE;
}
