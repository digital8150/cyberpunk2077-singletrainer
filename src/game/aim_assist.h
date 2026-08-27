#pragma once

#include <cstdint>

namespace Game::AimAssist
{
    struct DiagnosticsSnapshot
    {
        std::uint64_t hookCallbacks;
        std::uint64_t appliedWrites;
        std::uint64_t calculationFailures;
        void* fppCamera;
        void* cameraSystem;
        float angularError;
    };

    // Hooks the first-person camera's upstream yaw/pitch input update.
    bool CreateHook();

    // Resolves the local player's first-person camera component without changing aim state.
    void ProbePlayerCamera();

    // Publishes a world target for the camera-input detour. Smoothing 0 supplies the full angular delta.
    bool ApplyMemoryAim(const float worldTarget[3], float smoothing);
    DiagnosticsSnapshot GetDiagnostics();
    void ClearMemoryAim();
    void Shutdown();
}
