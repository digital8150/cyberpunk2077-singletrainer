#pragma once

#include <cstdint>

namespace Game::SilentAim
{
    struct DiagnosticsSnapshot
    {
        bool hookCreated = false;
        bool queueHookCreated = false;
        // Mutation paths:
        // 1. Hitscan firearms: native crosshair core direction out-parameter redirection.
        // 2. Projectiles (throwing knives/axes): spawner orientation provider (entFuncOrientationProvider) redirection.
        bool crosshairCoreHookCreated = false;
        bool projectileHookCreated = false;
        bool orientationHookCreated = false;
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
        std::uint64_t spawnerLaunchEvents = 0;
        std::uint64_t spawnerLaunchRedirects = 0;
        std::uint64_t orientationRedirects = 0;
    };

    // Resolves the native crosshair core (hitscan mutation path) and projectile ShootEvent listeners
    // (throwing knife/axe ballistic mutation path). Call after MH_Initialize and before MH_EnableHook.
    bool CreateHook();

    // Present publishes only plain coordinates. Native callbacks use freshness as an early filter, so a target
    // that stops being published (out of FOV, dead, or occluded while visibleOnly is on) stops being redirected.
    void PublishTarget(const float worldTarget[3], bool active);
    void ClearTarget();
    DiagnosticsSnapshot GetDiagnostics();
    void Shutdown();
}
