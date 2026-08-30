#pragma once

#include <cstdint>

namespace Features
{
    struct AimbotSettings;
    struct FrameSnapshots;
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
    // Clears active memory/silent aim state once when the feature transitions to disabled. This is intentionally
    // separate from RunFrame so a disabled feature does not enter the per-frame candidate path.
    void Disable();
    // 스냅샷 패스는 호출자(Features::DrawOverlay/UpdateHeadless)가 프레임당 한 번만 돌리고 그 결과를 넘긴다.
    void DrawOverlay(const Features::AimbotSettings& settings, const Features::FrameSnapshots& frame);
    void UpdateHeadless(const Features::AimbotSettings& settings, const Features::FrameSnapshots& frame,
                        float displayWidth, float displayHeight);
    void Shutdown();
}
