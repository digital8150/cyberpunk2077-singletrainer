#pragma once

#include <cstdint>

namespace Game::SilentAim
{
    struct DiagnosticsSnapshot
    {
        bool hookCreated = false;
        bool queueHookCreated = false;
        // The only mutation path: the native crosshair core's caller-visible direction out-parameter.
        bool crosshairCoreHookCreated = false;
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

    // Resolves the native crosshair core (the single mutation path) plus observation-only effect/attack/crosshair
    // native handlers and projectile ShootEvent listeners. Projectile and effect mutation stay gated off.
    // Call after MH_Initialize and before MH_EnableHook(MH_ALL_HOOKS).
    bool CreateHook();

    // Present publishes only plain coordinates. Native callbacks use freshness as an early filter, so a target
    // that stops being published (out of FOV, dead, or occluded while visibleOnly is on) stops being redirected.
    void PublishTarget(const float worldTarget[3], bool active);
    void ClearTarget();
    DiagnosticsSnapshot GetDiagnostics();
    void Shutdown();
}
