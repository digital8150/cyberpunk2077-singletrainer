#pragma once

namespace Game::AimAssist
{
    // Installs the post-provider hook that overrides FPPCameraComponent's live yaw/pitch fields.
    bool CreateHook();

    // Resolves the local player's first-person camera component without changing aim state.
    void ProbePlayerCamera();

    // Publishes a world target for the camera-update hook. Smoothing 0 writes exact offsets with no easing.
    bool ApplyMemoryAim(const float worldTarget[3], float smoothing);
    void ClearMemoryAim();
    void Shutdown();
}
