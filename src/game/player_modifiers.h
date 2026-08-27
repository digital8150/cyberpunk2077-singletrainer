#pragma once

#include <cstdint>

namespace Game::PlayerModifiers
{
    // Present/UI code only publishes this atomic request. StatsSystem and TransactionSystem calls run from the
    // existing game-main-tick detour in visibility.cpp.
    void PublishDesired(bool enabled);
    void OnGameMainTick();

    // Requests removal on the main tick and waits for its acknowledgement. Returns false when the game stopped
    // ticking or a reflected remove call failed, in which case Hooks::Shutdown must abort the unload.
    bool PrepareForShutdown(std::uint32_t timeoutMilliseconds);
    void Shutdown();

    struct Stats
    {
        bool available = false;
        bool active = false;
        std::uint64_t targetId = 0;
        std::uint64_t weaponId = 0;
        bool usingWeaponTarget = false;
        std::uint64_t applied = 0;
        std::uint64_t removed = 0;
        std::uint64_t failures = 0;
    };

    Stats GetStats();
}
