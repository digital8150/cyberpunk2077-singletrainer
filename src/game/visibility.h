#pragma once

#include <cstdint>

namespace Game::Visibility
{
    enum class State : std::uint8_t
    {
        Unknown,
        Visible,
        Occluded,
    };

    struct Stats
    {
        bool available = false;
        std::uint64_t casts = 0;
        std::uint64_t visible = 0;
        std::uint64_t occluded = 0;
        std::uint64_t dropped = 0;
    };

    // Creates the optional game-main-tick hook. Call after MH_Initialize and before MH_EnableHook(MH_ALL_HOOKS).
    // Failure disables visibility only.
    bool CreateHook();

    // Kept as a frame boundary for existing ESP/aimbot callers. Query work is budgeted by the game-main-tick hook.
    void BeginFrame();

    // Returns the cached result immediately. A stale/unknown entry may enqueue a bounded request, but this call
    // never invokes the engine query itself.
    State Query(std::uint64_t entityId, const float camera[3], const float primary[3], const float secondary[3]);

    Stats GetStats();

    // Clears state after MinHook detours have been disabled and drained.
    bool Shutdown();
}
