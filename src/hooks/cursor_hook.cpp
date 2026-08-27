#include "cursor_hook.h"
#include "hook_lifecycle.h"
#include "../diagnostics.h"
#include "../framework.h"

#include <MinHook.h>

#include <atomic>

namespace
{
    using SetCursorPosFn = BOOL(WINAPI*)(int, int);
    using ClipCursorFn = BOOL(WINAPI*)(const RECT*);

    SetCursorPosFn g_originalSetCursorPos = nullptr;
    SetCursorPosFn g_originalSetPhysicalCursorPos = nullptr;
    ClipCursorFn g_originalClipCursor = nullptr;
    std::atomic_bool g_menuCapture{false};

    BOOL WINAPI HookSetCursorPos(int x, int y)
    {
        HookLifecycle::CallbackGuard callback;
        if (!HookLifecycle::IsShuttingDown() && g_menuCapture.load(std::memory_order_acquire))
            return TRUE;
        return g_originalSetCursorPos(x, y);
    }

    BOOL WINAPI HookSetPhysicalCursorPos(int x, int y)
    {
        HookLifecycle::CallbackGuard callback;
        if (!HookLifecycle::IsShuttingDown() && g_menuCapture.load(std::memory_order_acquire))
            return TRUE;
        return g_originalSetPhysicalCursorPos(x, y);
    }

    BOOL WINAPI HookClipCursor(const RECT* rectangle)
    {
        HookLifecycle::CallbackGuard callback;
        if (!HookLifecycle::IsShuttingDown() && g_menuCapture.load(std::memory_order_acquire) && rectangle)
            return TRUE;
        return g_originalClipCursor(rectangle);
    }

    bool CreateOneHook(void* target, void* detour, void** original, const char* name)
    {
        if (!target)
        {
            Diagnostics::Log("cursor hook target unavailable: %s", name);
            return false;
        }

        const MH_STATUS status = MH_CreateHook(target, detour, original);
        if (status != MH_OK)
        {
            Diagnostics::Log("MH_CreateHook(%s) failed: %s (%d)", name, MH_StatusToString(status), status);
            return false;
        }
        return true;
    }
}

namespace CursorHook
{
    bool CreateHooks()
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (!user32)
        {
            Diagnostics::Log("cursor hooks unavailable: user32.dll is not loaded");
            return false;
        }

        void* setCursorPos = reinterpret_cast<void*>(GetProcAddress(user32, "SetCursorPos"));
        void* setPhysicalCursorPos = reinterpret_cast<void*>(GetProcAddress(user32, "SetPhysicalCursorPos"));

        bool ok = true;
        ok &= CreateOneHook(setCursorPos,
                            reinterpret_cast<void*>(&HookSetCursorPos),
                            reinterpret_cast<void**>(&g_originalSetCursorPos), "SetCursorPos");
        if (setPhysicalCursorPos == setCursorPos)
        {
            // Current user32 exports both names at the same entry point. The SetCursorPos detour covers both;
            // asking MinHook to create a second hook for the same target returns MH_ERROR_ALREADY_CREATED.
            Diagnostics::Log("cursor hook alias: SetPhysicalCursorPos is covered by SetCursorPos");
        }
        else
        {
            ok &= CreateOneHook(setPhysicalCursorPos, reinterpret_cast<void*>(&HookSetPhysicalCursorPos),
                                reinterpret_cast<void**>(&g_originalSetPhysicalCursorPos), "SetPhysicalCursorPos");
        }
        ok &= CreateOneHook(reinterpret_cast<void*>(GetProcAddress(user32, "ClipCursor")),
                            reinterpret_cast<void*>(&HookClipCursor),
                            reinterpret_cast<void**>(&g_originalClipCursor), "ClipCursor");
        Diagnostics::Log("cursor hooks created: complete=%d", ok ? 1 : 0);
        return ok;
    }

    void SetMenuCapture(bool capture)
    {
        const bool changed = g_menuCapture.exchange(capture, std::memory_order_acq_rel) != capture;
        if (changed)
            Diagnostics::Log("menu cursor capture changed: capture=%d", capture ? 1 : 0);
        if (capture && g_originalClipCursor)
            g_originalClipCursor(nullptr);
    }

    bool IsMenuCapturing()
    {
        return g_menuCapture.load(std::memory_order_acquire);
    }

    void Shutdown()
    {
        g_menuCapture.store(false, std::memory_order_release);
    }
}
