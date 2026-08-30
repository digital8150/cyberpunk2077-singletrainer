// DLL 진입점. 인젝션되면 여기서 시작한다.
//
// DllMain 안에서 직접 무거운 초기화(D3D12 디바이스 생성 등)를 하면 로더 락 문제로 데드락이 나기 쉬우므로,
// 실제 초기화는 별도 스레드(MainThread)에서 수행한다.
#include "framework.h"
#include "diagnostics.h"
#include "config.h"
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
        Config::Initialize();
        Hooks::Initialize();

        wchar_t unloadEventName[96]{};
        swprintf_s(unloadEventName, L"Local\\cp2077_trainer_unload_%lu", GetCurrentProcessId());
        HANDLE unloadEvent = CreateEventW(nullptr, TRUE, FALSE, unloadEventName);
        if (unloadEvent)
            Diagnostics::Log("initialization finished; press End or signal the unload event to unload safely");
        else
            Diagnostics::Log("CreateEventW(unload) failed: error=%lu; End hotkey remains available", GetLastError());

        constexpr unsigned kShutdownAttemptsPerRequest = 3;
        bool shutdownComplete = false;
        bool endWasDown = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
        while (!shutdownComplete)
        {
            bool requestReceived = false;
            while (!requestReceived)
            {
                const bool endIsDown = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
                const bool eventSignaled = unloadEvent && WaitForSingleObject(unloadEvent, 0) == WAIT_OBJECT_0;
                requestReceived = (endIsDown && !endWasDown) || eventSignaled;
                endWasDown = endIsDown;
                if (!requestReceived)
                    Sleep(50);
            }

            Diagnostics::Log("safe unload requested");
            for (unsigned attempt = 1; attempt <= kShutdownAttemptsPerRequest; ++attempt)
            {
                if (Hooks::Shutdown())
                {
                    shutdownComplete = true;
                    break;
                }
                if (attempt < kShutdownAttemptsPerRequest)
                {
                    Diagnostics::Log("safe unload cleanup attempt %u/%u failed; retrying in 1000 ms",
                                     attempt, kShutdownAttemptsPerRequest);
                    Sleep(1000);
                }
            }

            if (!shutdownComplete)
            {
                // Never force FreeLibrary after a failed cleanup. Stop the hot retry loop, leave the DLL resident,
                // and require a fresh End edge or automation event before another bounded attempt set.
                Diagnostics::Log("safe unload deferred after %u attempts; DLL remains loaded; signal again to retry",
                                 kShutdownAttemptsPerRequest);
                if (unloadEvent)
                    ResetEvent(unloadEvent);
                endWasDown = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
            }
        }

        Diagnostics::Log("safe unload checks passed; releasing DLL");
        Config::Shutdown();
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
