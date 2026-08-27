#pragma once

#include <cstddef>
#include <cstdint>

namespace Game::EntityTracker
{
    enum class NpcCategory : std::uint8_t
    {
        Other,
        Civilian,
        Enemy,
        Police,
    };

    struct Stats
    {
        bool hookCreated = false;
        std::uint64_t registered = 0;
        std::uint64_t positioned = 0;
        std::uint64_t puppets = 0;
        std::uint64_t trackedPuppets = 0;
        std::uint64_t trackedCivilians = 0;
        std::uint64_t trackedEnemies = 0;
        std::uint64_t trackedPolice = 0;
        std::uint64_t lastEntityId = 0;
        float lastPosition[3]{};
        bool hasLastPuppet = false;
        std::uint64_t lastPuppetId = 0;
        float lastPuppetPosition[3]{};
    };

    struct PuppetSnapshot
    {
        std::uint64_t entityId = 0;
        float position[3]{};
        NpcCategory category = NpcCategory::Other;
    };

    // MinHook 초기화 후, MH_EnableHook(MH_ALL_HOOKS) 전에 호출한다.
    bool CreateHook();
    void Shutdown();
    Stats GetStats();

    // Refreshes registered NPC pointers defensively and copies only validated ID/position snapshots to the caller.
    std::size_t GetPuppetSnapshots(PuppetSnapshot* output, std::size_t capacity);
}
