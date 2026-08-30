#pragma once

#include <cstdint>

namespace Features
{
    struct MiscSettings;
}

namespace Game::PlayerModifiers
{
    // Present/UI code only publishes this request. StatsSystem and TransactionSystem calls run from the
    // existing game-main-tick detour in visibility.cpp. The whole Misc group is published in one call so the
    // Present-side call site does not have to grow a line per feature.
    void PublishDesired(const Features::MiscSettings& misc);
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
        std::uint64_t retiredOwnerResets = 0;
        std::uint64_t failures = 0;
    };

    Stats GetStats();
}
