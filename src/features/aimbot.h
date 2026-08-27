#pragma once

#include <cstdint>

namespace Features
{
    struct AimbotSettings;
}

namespace Aimbot
{
    // Last frame's target-selection breakdown. Written and read on the Present thread only.
    struct Stats
    {
        unsigned int candidates = 0;
        unsigned int eligible = 0;
        unsigned int skippedNoHealthPool = 0;
        unsigned int skippedHealthCap = 0;
        unsigned int skippedOccluded = 0;
        std::uint64_t targetEntityId = 0;
        bool targetHealthValid = false;
        float targetHealth = 0.0f;
        float targetHealthMax = 0.0f;
    };

    Stats GetStats();
    void DrawOverlay(const Features::AimbotSettings& settings);
    void UpdateHeadless(const Features::AimbotSettings& settings, float displayWidth, float displayHeight);
    void Shutdown();
}
