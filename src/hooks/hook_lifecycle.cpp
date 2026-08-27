#include "hook_lifecycle.h"

#include <atomic>

namespace
{
    std::atomic_bool g_shuttingDown{false};
    std::atomic_uint32_t g_activeCallbacks{0};
}

namespace HookLifecycle
{
    CallbackGuard::CallbackGuard()
    {
        g_activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
    }

    CallbackGuard::~CallbackGuard()
    {
        g_activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    }

    void BeginShutdown()
    {
        g_shuttingDown.store(true, std::memory_order_release);
    }

    bool IsShuttingDown()
    {
        return g_shuttingDown.load(std::memory_order_acquire);
    }

    bool WaitForCallbacks(DWORD timeoutMilliseconds)
    {
        const ULONGLONG deadline = GetTickCount64() + timeoutMilliseconds;
        while (g_activeCallbacks.load(std::memory_order_acquire) != 0)
        {
            if (GetTickCount64() >= deadline)
                return false;
            Sleep(1);
        }
        return true;
    }
}
