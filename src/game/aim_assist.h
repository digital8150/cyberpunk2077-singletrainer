#pragma once

namespace Game::AimAssist
{
    // Uses TargetingSystem::LookAt and therefore updates the game's own camera/aim offset, not mouse input.
    bool ApplyLookAt(const float worldTarget[3], float smoothing);
    void BreakLookAt();
    void Shutdown();
}
