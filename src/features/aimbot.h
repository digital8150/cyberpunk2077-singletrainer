#pragma once

namespace Features
{
    struct AimbotSettings;
}

namespace Aimbot
{
    void DrawOverlay(const Features::AimbotSettings& settings);
    void UpdateHeadless(const Features::AimbotSettings& settings, float displayWidth, float displayHeight);
    void Shutdown();
}
