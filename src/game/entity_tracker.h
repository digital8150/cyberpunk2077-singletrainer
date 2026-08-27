#pragma once

#include <cstdint>

namespace Game::EntityTracker
{
    struct Stats
    {
        bool hookCreated = false;
        std::uint64_t registered = 0;
        std::uint64_t positioned = 0;
        std::uint64_t puppets = 0;
        std::uint64_t lastEntityId = 0;
        float lastPosition[3]{};
    };

    // MinHook 초기화 후, MH_EnableHook(MH_ALL_HOOKS) 전에 호출한다.
    bool CreateHook();
    void Shutdown();
    Stats GetStats();
}
