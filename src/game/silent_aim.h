#pragma once

#include <cstdint>

namespace Game::SilentAim
{
    struct DiagnosticsSnapshot
    {
        bool hookCreated = false;
        bool queueHookCreated = false;
        std::uint32_t listenerHooks = 0;
        std::uint32_t producerHooks = 0;
        std::uint64_t callbacks = 0;
        std::uint64_t queueCallbacks = 0;
        std::uint64_t projectileEvents = 0;
        std::uint64_t weaponShootEvents = 0;
        std::uint64_t localPlayerEvents = 0;
        std::uint64_t validatedLocalEvents = 0;
        std::uint64_t redirectedShots = 0;
        std::uint64_t rejectedShots = 0;
        std::uint64_t effectRuns = 0;
        std::uint64_t attackStarts = 0;
        std::uint64_t attackPrepares = 0;
        std::uint64_t crosshairCalls = 0;
        std::uint64_t defaultCrosshairCalls = 0;
        std::uint64_t nativeCrosshairCoreCalls = 0;
        std::uint64_t nativeCrosshairCoreRedirects = 0;
    };

    // Resolves observation-only effect/attack/crosshair native handlers plus projectile ShootEvent listeners.
    // All mutation remains separately gated off until live call-path and payload validation succeeds.
    // Call after MH_Initialize and before MH_EnableHook(MH_ALL_HOOKS).
    bool CreateHook();

    // Present publishes only plain coordinates. Native observation callbacks use freshness as an early filter.
    void PublishTarget(const float worldTarget[3], bool active);
    void ClearTarget();
    DiagnosticsSnapshot GetDiagnostics();
    void Shutdown();
}
