#pragma once

namespace Config
{
    // Loads %LOCALAPPDATA%\cbpk\config.ini. Missing or invalid values keep the compiled defaults.
    bool Initialize();

    // Called from the Present thread. Changes are saved after a short debounce so sliders do not write every frame.
    void Update();

    // Flushes a pending change during safe DLL unload.
    void Shutdown();

    const wchar_t* Path();
}
