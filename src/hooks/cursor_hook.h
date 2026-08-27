#pragma once

namespace CursorHook
{
    // Create Win32 cursor hooks after MH_Initialize and before MH_EnableHook(MH_ALL_HOOKS).
    bool CreateHooks();
    void SetMenuCapture(bool capture);
    bool IsMenuCapturing();
    void Shutdown();
}
