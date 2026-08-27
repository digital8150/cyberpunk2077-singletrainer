#pragma once

#include "../framework.h"

namespace HookLifecycle
{
    // Every detour keeps one guard alive until it has fully returned to the original caller. This lets the
    // dedicated unload thread prove that no instruction pointer remains inside this DLL before FreeLibrary.
    class CallbackGuard
    {
    public:
        CallbackGuard();
        ~CallbackGuard();

        CallbackGuard(const CallbackGuard&) = delete;
        CallbackGuard& operator=(const CallbackGuard&) = delete;
    };

    void BeginShutdown();
    bool IsShuttingDown();
    bool WaitForCallbacks(DWORD timeoutMilliseconds);
}
