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
        bool unregisterHookCreated = false;
        bool attitudeRttiFailClosed = true;
        std::uint64_t registered = 0;
        std::uint64_t registerCallbacks = 0;
        std::uint64_t registerThreadId = 0;
        std::uint64_t registerThreadChanges = 0;
        std::uint64_t registerOnMainTickThread = 0;
        std::uint64_t registerOffMainTickThread = 0;
        std::uint64_t mainTickCalls = 0;
        std::uint64_t mainTickThreadId = 0;
        std::uint64_t mainTickThreadChanges = 0;
        std::uint64_t positioned = 0;
        std::uint64_t puppets = 0;
        std::uint64_t trackedPuppets = 0;
        std::uint64_t trackedCivilians = 0;
        std::uint64_t trackedEnemies = 0;
        std::uint64_t trackedPolice = 0;
        std::uint64_t trackedHostile = 0;
        std::uint64_t attitudeValid = 0;
        std::uint64_t attitudeInvalid = 0;
        std::uint64_t attitudeFailClosedTicks = 0;
        // Phase 2 attitude ownership diagnostics. Acquisition success counts a strong retained-Entity copy made
        // under the tracker lock; unknown counts include candidates rejected by the runtime signature/lifetime gate.
        std::uint64_t attitudeLookupAttempts = 0;
        std::uint64_t attitudeSourceContractBlocked = 0;
        std::uint64_t attitudeLifetimeAcquisitionSuccess = 0;
        std::uint64_t attitudeExpiredOrInvalidReference = 0;
        std::uint64_t attitudeAgentLookupSuccess = 0;
        std::uint64_t attitudeAgentLookupUnknown = 0;
        std::uint64_t pendingPosition = 0;
        // Observed UnregisterEntity calls are zero when the proven 2.31 observer is unavailable; stale
        // identity/transform validation remains authoritative cleanup and is reported separately by staleRemoved.
        std::uint64_t unregistered = 0;
        std::uint64_t unregisterThreadId = 0;
        std::uint64_t unregisterThreadChanges = 0;
        std::uint64_t unregisterOnMainTickThread = 0;
        std::uint64_t unregisterOffMainTickThread = 0;
        std::uint64_t unregisterTracked = 0;
        std::uint64_t unregisterUntracked = 0;
        std::uint64_t unregisterTrackingUnknown = 0;
        std::uint64_t unregisterWithoutIdentity = 0;
        std::uint64_t staleRemoved = 0;
        std::uint64_t healthValid = 0;
        std::uint64_t healthInvalid = 0;
        std::uint64_t nativeHighlightQueued = 0;
        std::uint64_t nativeHighlightCleared = 0;
        std::uint64_t nativeHighlightFailures = 0;
        // Component handle qwords are inspected for only the first bounded registration sample set; skipped keeps
        // the total population visible without reading component arrays forever.
        std::uint64_t componentHandleSampleEntities = 0;
        std::uint64_t componentHandleSamplingSkipped = 0;
        std::uint64_t componentHandleSamples = 0;
        std::uint64_t componentHandleLayoutRejects = 0;
        std::uint64_t componentHandleNullInstances = 0;
        std::uint64_t componentHandleNullRefCounts = 0;
        std::uint64_t componentHandleTruncated = 0;
        std::uint64_t lastRegisteredEntityAddress = 0;
        std::uint64_t lastRegisteredEntityId = 0;
        // Removal identity/tracking are captured before the original returns; the observer never reads entity memory
        // after that call.
        std::uint64_t lastUnregisteredEntityAddress = 0;
        std::uint64_t lastUnregisteredEntityId = 0;
        bool lastUnregisteredTrackingKnown = false;
        bool lastUnregisteredTracked = false;
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

    // Called during hook setup before MH_EnableHook(MH_ALL_HOOKS); the optional unregister observer is installed
    // only when its target and ABI are proven for the running image.
    bool CreateHook();
    void Shutdown();
    // Executes tracker health refreshes and native highlight events on the game main tick. Never call from Present.
    void OnGameMainTick();

    // Publishes which snapshot consumers still need reflected health/attitude refreshes. The values are consumed
    // only by OnGameMainTick; disabling a requirement clears its cached state once on that same main-tick path.
    void UpdateFeatureRequirements(bool health, bool attitude);

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
