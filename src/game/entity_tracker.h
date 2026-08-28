#pragma once

#include <cstddef>
#include <cstdint>

#include "animation_data.h"

namespace Game::EntityTracker
{
    enum class NpcCategory : std::uint8_t
    {
        Other,
        Civilian,
        Enemy,
        Police,
    };

    // Runtime attitude toward the local player, read from the puppet's attitude agent. NpcCategory is the NPC's
    // spawn-time archetype and never changes, so a neutral NPC that turns on the player is only visible here.
    enum class Hostility : std::uint8_t
    {
        Unknown,
        Friendly,
        Neutral,
        Hostile,
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
        std::uint64_t trackedHostile = 0;
        std::uint64_t attitudeValid = 0;
        std::uint64_t attitudeInvalid = 0;
        std::uint64_t pendingPosition = 0;
        // Compatibility diagnostic only. UnregisterEntity is not hooked because its ABI is unverified, so this
        // counter is intentionally always zero; stale identity/transform validation is reported separately.
        std::uint64_t unregistered = 0;
        std::uint64_t staleRemoved = 0;
        std::uint64_t healthValid = 0;
        std::uint64_t healthInvalid = 0;
        std::uint64_t nativeHighlightQueued = 0;
        std::uint64_t nativeHighlightCleared = 0;
        std::uint64_t nativeHighlightFailures = 0;
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
        float orientation[4]{0.0f, 0.0f, 0.0f, 1.0f};
        NpcCategory category = NpcCategory::Other;
        Hostility hostility = Hostility::Unknown;
        bool isDead = false;
        bool healthValid = false;
        float healthCurrent = 0.0f;
        float healthMax = 0.0f;
        float healthRatio = 0.0f;
        AnimationData::VisualData visual;
    };

    // MinHook 초기화 후, MH_EnableHook(MH_ALL_HOOKS) 전에 호출한다.
    bool CreateHook();
    void Shutdown();
    // Executes tracker health refreshes and native highlight events on the game main tick. Never call from Present.
    void OnGameMainTick();

    // Requests a main-tick clear and waits for a clear event plus a following tick before unload. The request itself
    // is atomic, so this function never calls into the engine from the unload worker.
    bool PrepareForShutdown(std::uint32_t timeoutMilliseconds);
    Stats GetStats();

    // Refreshes registered NPC pointers defensively and copies only validated ID/position snapshots to the caller.
    std::size_t GetPuppetSnapshots(PuppetSnapshot* output, std::size_t capacity);

    // Publishes the desired native highlight settings. Event allocation and QueueEvent execution happen only from
    // OnGameMainTick.
    void UpdateNativeHighlights(bool enabled, bool showCivilians, bool showEnemies, bool showPolice,
                                bool showUnclassified, bool hideDead);
}
