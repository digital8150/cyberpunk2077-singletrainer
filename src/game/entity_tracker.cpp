#include "entity_tracker.h"
#include "animation_data.h"
#include "rtti_invoker.h"
#include "signature_scanner.h"
#include "../diagnostics.h"
#include "../framework.h"
#include "../hooks/hook_lifecycle.h"
#include "../profiling.h"

#include <MinHook.h>

#include <atomic>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace
{
    // RED4ext/CET address hash for world::RuntimeEntityRegistry::RegisterEntity. The local 2.31 address map names
    // this entry but does not name an UnregisterEntity symbol. Its complete function table does contain one unique,
    // unnamed entry at section-1 offset 0x8B4310 (image RVA 0x8B5310), hash 3878623943. The disassembly/xrefs below
    // independently prove that entry as the entity-removal routine before it is used as an optional observer hook.
    constexpr std::uint32_t kRegisterEntityAddressHash = 2840271332u;
    constexpr std::uint32_t kRuntimeEntityRegistryRemovalAddressHash = 3878623943u;
    constexpr std::uint32_t kCClassGetPropertyAddressHash = 0x8F031512u;
    constexpr std::uint32_t kVisionModeSetBraindanceModeAddressHash = 1070077985u;

    constexpr std::uint32_t kGameEngineAddressHash = 0x97F209D6u;
    constexpr std::uint32_t kRttiSystemGetAddressHash = 0x4A610F64u;
    constexpr std::size_t kEntRenderHighlightEventSize = 0x58;
    constexpr std::size_t kMaxLeakedHighlightEvents = 4096;
    constexpr std::size_t kComponentHandleSize = 0x10;
    constexpr std::size_t kComponentHandleSampleLimit = 8;
    constexpr std::size_t kComponentHandleSampleEntityLimit = 64;
    constexpr std::size_t kComponentDetailLogLimit = 8;
    constexpr ULONGLONG kPhase1SummaryIntervalMilliseconds = 3000;

    // Phase 2 owns the Entity through a strong Handle acquired in the RegisterEntity caller scope. Attitude no
    // longer reads a component DynArray entry: the retained Entity is the source for the reflected GetAttitudeAgent
    // getter, which returns its own strong Handle. The runtime gate still requires the exact 2.31 Handle ABI and
    // all reflected signatures to validate on the running image before any attitude call is made.
    constexpr bool kPhase2AttitudeLifetimeCompileGate = true;
    constexpr bool kPhase2AttitudeSourceContractCompileGate = true;
    constexpr bool kPhase2AttitudeRttiCompileGate = kPhase2AttitudeLifetimeCompileGate &&
                                                     kPhase2AttitudeSourceContractCompileGate;
    static_assert(kPhase2AttitudeRttiCompileGate, "Phase 2 retained-Entity attitude path must be compiled");

    // Cyberpunk 2077 2.31 / internal 3.0.80.51928에서 검증. 상대 call 대상만 wildcard 처리했으며
    // tools/scripts/memtool.py aobscan으로 Cyberpunk2077.exe 내 정확히 1개 매치를 확인했다.
    // Cyberpunk 2077 2.31 / file version 3.0.5294808: the unique RegisterEntity signature below starts at image RVA
    // 0x8B57F4
    // (the address map stores section-1 offset 0x8B47F4 plus its 0x1000 code-constant offset). Its first body
    // instructions move RCX to RSI, RDX to RDI, and read [RDI+0x48], proving the x64 RegisterEntity ABI used here:
    // (registry=this, entity).
    constexpr std::uint8_t kRegisterEntityPattern[] = {
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18,
        0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x60,
        0x48, 0x8B, 0xF1, 0x48, 0x8B, 0xFA, 0x48, 0x83, 0xC1, 0x48,
        0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x47, 0x48,
    };
    constexpr char kRegisterEntityMask[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxx";
    static_assert(sizeof(kRegisterEntityPattern) == sizeof(kRegisterEntityMask) - 1);

    // The local address-map hash 3878623943 resolves to image RVA 0x8B5310 (section-1 offset 0x8B4310). Its .pdata
    // range is [0x8B5310, 0x8B5380): RCX/RDX are saved as registry/entity, [entity+0x48] is passed as the erase
    // key to registry+0x50, the erase-success byte is returned in AL, and the shared notifier at 0x8B58A8 receives
    // R8B=0. The same notifier receives R8B=1 from RegisterEntity at 0x8B57F4. Direct xrefs at RVAs 0x8B5123 and
    // 0x1EBFBA6 pass the same registry/entity
    // pair; no EntityID-only call target was found. This signature is a fallback when RED4ext is unavailable and is
    // unique in the local 2.31 .text section; it is not inferred from adjacency.
    constexpr std::uint8_t kRuntimeEntityRegistryRemovalPattern[] = {
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20,
        0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xE9, 0x48, 0x8B, 0xF2, 0x48, 0x83, 0xC1, 0x48,
        0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x46, 0x48, 0x48, 0x8D, 0x4D, 0x50, 0x4C, 0x8D,
        0x44, 0x24, 0x50,
    };
    constexpr char kRuntimeEntityRegistryRemovalMask[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxx";
    static_assert(sizeof(kRuntimeEntityRegistryRemovalPattern) == sizeof(kRuntimeEntityRegistryRemovalMask) - 1);

    constexpr std::uint64_t Fnv1a64(const char* text)
    {
        std::uint64_t hash = 0xCBF29CE484222325ull;
        while (*text)
        {
            hash ^= static_cast<std::uint8_t>(*text++);
            hash *= 0x100000001B3ull;
        }
        return hash;
    }

    struct ClassLayout
    {
        std::byte pad00[0x10];
        ClassLayout* parent;
        std::uint64_t nameHash;
    };
    static_assert(offsetof(ClassLayout, parent) == 0x10);
    static_assert(offsetof(ClassLayout, nameHash) == 0x18);

    struct DynArrayLayout
    {
        void** entries;
        std::uint32_t capacity;
        std::uint32_t size;
    };
    static_assert(sizeof(DynArrayLayout) == 0x10);

    // RED4ext.SDK entEntity.hpp (wopss/RED4ext.SDK) declares components as DynArray<Handle<IComponent>> at 0xA0.
    // Handle/SharedPtrBase is two pointers: qword0=instance and qword1=RefCnt*. RefCnt's strongRefs/weakRefs are
    // at offsets 0/4, but Phase 1 does not dereference qword1 because its independent liveness is not proven.
    struct ComponentHandleLayout
    {
        void* instance;
        void* refCount;
    };
    static_assert(sizeof(ComponentHandleLayout) == kComponentHandleSize);
    static_assert(offsetof(ComponentHandleLayout, instance) == 0x00);
    static_assert(offsetof(ComponentHandleLayout, refCount) == 0x08);

    struct PropertyLayout
    {
        void* type;
        std::uint64_t nameHash;
        std::uint64_t groupHash;
        ClassLayout* parent;
        std::uint32_t valueOffset;
        std::uint32_t pad24;
        std::uint64_t flags;
    };
    static_assert(offsetof(PropertyLayout, valueOffset) == 0x20);
    static_assert(offsetof(PropertyLayout, flags) == 0x28);

    struct BoolPropertyLocation
    {
        std::uint32_t offset = 0;
        bool inValueHolder = false;
        bool found = false;
    };

    struct ClassificationLayout
    {
        BoolPropertyLocation civilian;
        BoolPropertyLocation police;
        BoolPropertyLocation ganger;
        bool logged = false;
    };

    struct FunctionLayout
    {
        void* vtable;
        std::uint64_t fullNameHash;
        std::uint64_t shortNameHash;
        std::byte pad18[0x80 - 0x18];
        const std::uint8_t* bytecode;
        std::uint32_t bytecodeSize;
    };
    static_assert(offsetof(FunctionLayout, shortNameHash) == 0x10);
    static_assert(offsetof(FunctionLayout, bytecode) == 0x80);
    static_assert(offsetof(FunctionLayout, bytecodeSize) == 0x88);

    struct WorldTransformLayout
    {
        std::int32_t x;
        std::int32_t y;
        std::int32_t z;
        std::byte pad0C[4];
        float orientation[4];
    };
    static_assert(sizeof(WorldTransformLayout) == 0x20);

    struct PlacedComponentLayout
    {
        std::byte pad00[0xE0];
        WorldTransformLayout worldTransform;
    };
    static_assert(offsetof(PlacedComponentLayout, worldTransform) == 0xE0);

    struct EntityLayout
    {
        std::byte pad00[0x30];
        ClassLayout* nativeType;
        void* valueHolder;
        std::byte pad40[0x48 - 0x40];
        std::uint64_t entityId;
        std::byte pad50[0xA0 - 0x50];
        struct
        {
            std::byte* entries;
            std::uint32_t capacity;
            std::uint32_t size;
        } components;
        PlacedComponentLayout* transformComponent;
    };
    static_assert(offsetof(EntityLayout, nativeType) == 0x30);
    static_assert(offsetof(EntityLayout, valueHolder) == 0x38);
    static_assert(offsetof(EntityLayout, entityId) == 0x48);
    static_assert(offsetof(EntityLayout, components) == 0xA0);
    static_assert(offsetof(EntityLayout, transformComponent) == 0xB0);

    enum class PuppetKind
    {
        None,
        Npc,
        Player,
    };

    struct PoseMetrics
    {
        std::uint8_t validMask = 0;
        float verticalSpan = 0.0f;
        float headRelativeZ = 0.0f;
        float headToHips = 0.0f;
    };

    struct TrackedPuppet
    {
        EntityLayout* entity = nullptr;
        // This is the owner of the raw Entity pointer above. Handle is intentionally opaque and must only be
        // moved under g_puppetListLock or copied with Game::Rtti::CopyHandle while that lock stabilizes storage.
        Game::Rtti::Handle entityHandle;
        std::uint64_t entityId = 0;
        std::uint64_t sequence = 0;
        Game::AnimationData::VisualData visual;
        ULONGLONG visualUpdatedAt = 0;
        ULONGLONG poseRequestedAt = 0;
        PoseMetrics poseMetrics;
        float poseEntityPosition[3]{};
        std::uint64_t poseSampleSequence = 0;
        std::uint64_t poseAnomalyEvents = 0;
        ULONGLONG poseAnomalyStartedAt = 0;
        bool poseAnomalyActive = false;
        Game::EntityTracker::NpcCategory category = Game::EntityTracker::NpcCategory::Other;
        Game::EntityTracker::Hostility hostility = Game::EntityTracker::Hostility::Unknown;
        ULONGLONG hostilityUpdatedAt = 0;
        bool isDead = false;
        bool healthValid = false;
        float healthCurrent = 0.0f;
        float healthMax = 0.0f;
        float healthRatio = 0.0f;
        bool healthReachedMin = false;
        bool highlightKnown = false;
        bool highlightDesired = false;
    };

    constexpr std::size_t kMaxTrackedPuppets = 256;
    // Reserve one clear-event slot per tracked entity. Enable transitions stop before this headroom is consumed,
    // so cleanup can still clear every known-enabled entry even after a long sequence of setting changes.
    constexpr std::size_t kReservedHighlightClearEvents = kMaxTrackedPuppets;
    static_assert(kReservedHighlightClearEvents < kMaxLeakedHighlightEvents);

    using RegisterEntityFn = bool (*)(void* registry, EntityLayout* entity);
    using UnregisterEntityFn = bool (*)(void* registry, EntityLayout* entity);
    using ResolveAddressFn = std::uintptr_t (*)(std::uint32_t);
    RegisterEntityFn g_originalRegisterEntity = nullptr;
    UnregisterEntityFn g_originalUnregisterEntity = nullptr;

    std::atomic_bool g_hookCreated{false};
    std::atomic_bool g_unregisterHookCreated{false};
    std::atomic_bool g_attitudeRttiRuntimeGate{false};
    std::atomic_bool g_attitudeFailClosedLogged{false};
    std::atomic_bool g_attitudeFailClosedStateCleared{false};
    std::atomic_bool g_unregisterDiagnosticLogged{false};
    std::atomic<void*> g_registry{nullptr};
    std::atomic_uint64_t g_registerCallbacks{0};
    std::atomic_uint64_t g_registerThreadAffinityMatches{0};
    std::atomic_uint64_t g_registerThreadAffinityMismatches{0};
    std::atomic_uint64_t g_unregisterCallbacks{0};
    std::atomic_uint64_t g_unregisterThreadAffinityMatches{0};
    std::atomic_uint64_t g_unregisterThreadAffinityMismatches{0};
    std::atomic_uint64_t g_unregisterTracked{0};
    std::atomic_uint64_t g_unregisterUntracked{0};
    std::atomic_uint64_t g_unregisterTrackingUnknown{0};
    std::atomic_uint64_t g_unregisterWithoutIdentity{0};
    std::atomic_uint64_t g_lastRegisteredEntityAddress{0};
    std::atomic_uint64_t g_lastRegisteredEntityId{0};
    std::atomic_uint64_t g_lastUnregisteredEntityAddress{0};
    std::atomic_uint64_t g_lastUnregisteredEntityId{0};
    std::atomic_bool g_lastUnregisteredTrackingKnown{false};
    std::atomic_bool g_lastUnregisteredTracked{false};
    std::atomic_uint64_t g_registered{0};
    std::atomic_uint64_t g_positioned{0};
    std::atomic_uint64_t g_puppets{0};
    std::atomic_uint64_t g_trackedPuppets{0};
    std::atomic_uint64_t g_trackedCivilians{0};
    std::atomic_uint64_t g_trackedEnemies{0};
    std::atomic_uint64_t g_trackedPolice{0};
    std::atomic_uint64_t g_trackedHostile{0};
    std::atomic_uint64_t g_attitudeValid{0};
    std::atomic_uint64_t g_attitudeInvalid{0};
    std::atomic_uint64_t g_attitudeFailClosedTicks{0};
    std::atomic_uint64_t g_attitudeLookupAttempts{0};
    std::atomic_uint64_t g_attitudeSourceContractBlocked{0};
    std::atomic_uint64_t g_attitudeLifetimeAcquisitionSuccess{0};
    std::atomic_uint64_t g_attitudeExpiredOrInvalidReference{0};
    std::atomic_uint64_t g_attitudeAgentLookupSuccess{0};
    std::atomic_uint64_t g_attitudeAgentLookupUnknown{0};
    std::atomic_uint64_t g_pendingPosition{0};
    std::atomic_uint64_t g_staleRemoved{0};
    std::atomic_uint64_t g_healthValid{0};
    std::atomic_uint64_t g_healthInvalid{0};
    std::atomic_uint64_t g_nativeHighlightQueued{0};
    std::atomic_uint64_t g_nativeHighlightCleared{0};
    std::atomic_uint64_t g_nativeHighlightFailures{0};
    std::atomic_uint64_t g_componentHandleSampleEntities{0};
    std::atomic_uint64_t g_componentHandleSampleOrdinal{0};
    std::atomic_uint64_t g_componentHandleSamplingSkipped{0};
    std::atomic_uint64_t g_componentHandleSamples{0};
    std::atomic_uint64_t g_componentHandleLayoutRejects{0};
    std::atomic_uint64_t g_componentHandleNullInstances{0};
    std::atomic_uint64_t g_componentHandleNullRefCounts{0};
    std::atomic_uint64_t g_componentHandleTruncated{0};
    std::atomic_uint64_t g_componentHandleDetailLogs{0};
    std::atomic_uint64_t g_mainTickCalls{0};

    struct ThreadObservation
    {
        // firstThreadId is the stable affinity baseline; lastThreadId remains mutable so thread migrations are
        // visible in the diagnostics without changing the baseline used by per-callback comparisons.
        std::atomic_uint64_t firstThreadId{0};
        std::atomic_uint64_t lastThreadId{0};
        std::atomic_uint64_t changes{0};
        std::atomic_uint64_t changeLogs{0};
    };
    ThreadObservation g_registerThread;
    ThreadObservation g_unregisterThread;
    ThreadObservation g_mainTickThread;

    std::atomic_uint64_t g_phase1SummaryTick{0};
    SRWLOCK g_lastEntityLock = SRWLOCK_INIT;
    std::uint64_t g_lastEntityId = 0;
    float g_lastPosition[3]{};
    bool g_hasLastPuppet = false;
    std::uint64_t g_lastPuppetId = 0;
    float g_lastPuppetPosition[3]{};
    SRWLOCK g_puppetListLock = SRWLOCK_INIT;
    std::array<TrackedPuppet, kMaxTrackedPuppets> g_puppetList{};
    std::atomic_uint32_t g_poseAnomalyLogRecords{0};
    constexpr std::uint32_t kPoseAnomalyLogRecordBudget = 768;
    // Occupancy bits for the slots above. The main-tick round-robins scan this instead of striding the list
    // itself: a TrackedPuppet is over a kilobyte, so touching all 256 slots cost one cache miss per slot and
    // dominated the health pass even with two NPCs on screen (measured 26 us with 2 puppets, 22 us with 45).
    // TrackedPuppet::entity stays the source of truth; this is maintained beside it under g_puppetListLock and
    // every scan still validates the entry it lands on.
    constexpr std::size_t kPuppetOccupancyWords = kMaxTrackedPuppets / 64;
    std::array<std::uint64_t, kPuppetOccupancyWords> g_puppetOccupancy{};

    void SetPuppetOccupied(std::size_t slot, bool occupied)
    {
        const std::uint64_t bit = 1ull << (slot % 64);
        if (occupied)
            g_puppetOccupancy[slot / 64] |= bit;
        else
            g_puppetOccupancy[slot / 64] &= ~bit;
    }

    bool IsPuppetOccupied(std::size_t slot)
    {
        return (g_puppetOccupancy[slot / 64] & (1ull << (slot % 64))) != 0;
    }

    bool IsPhase2AttitudeEnabled() noexcept
    {
        return kPhase2AttitudeRttiCompileGate && g_attitudeRttiRuntimeGate.load(std::memory_order_acquire);
    }

    bool HasHandleValue(const Game::Rtti::Handle& handle) noexcept
    {
        return handle.instance != nullptr || handle.refCount != nullptr;
    }

    bool IsHandleShapeValid(const Game::Rtti::Handle& handle) noexcept
    {
        return Game::Rtti::IsValidUserPointer(handle.instance) &&
               Game::Rtti::IsValidUserPointer(handle.refCount);
    }

    // Exact Handle destruction is deliberately the only release operation used by Phase 2. If an engine result is
    // malformed or the verified resolver disappears, clear the local storage and fail closed; never route a live
    // attitude/reference-counting path through the conservative/leaking ReleaseHandle fallback.
    bool ReleaseOwnedHandle(Game::Rtti::Handle& handle, const char* reason) noexcept
    {
        if (!HasHandleValue(handle))
            return true;
        if (!IsHandleShapeValid(handle) || !Game::Rtti::ReleaseHandleExact(&handle))
        {
            g_attitudeRttiRuntimeGate.store(false, std::memory_order_release);
            Diagnostics::Log("phase2 exact Handle release rejected: reason=%s instance=%p refCount=%p", reason,
                             handle.instance, handle.refCount);
            handle = {};
            return false;
        }
        return true;
    }

    std::uint64_t ObserveThread(ThreadObservation& observation, const char* path)
    {
        const std::uint64_t current = static_cast<std::uint64_t>(GetCurrentThreadId());
        std::uint64_t first = observation.firstThreadId.load(std::memory_order_acquire);
        if (first == 0 &&
            observation.firstThreadId.compare_exchange_strong(first, current, std::memory_order_release,
                                                              std::memory_order_relaxed))
        {
            Diagnostics::Log("phase1 lifecycle thread first: path=%s tid=%llu", path,
                             static_cast<unsigned long long>(current));
        }

        std::uint64_t previous = observation.lastThreadId.load(std::memory_order_relaxed);
        if (previous == current)
            return current;

        if (previous == 0 && observation.lastThreadId.compare_exchange_strong(previous, current,
                                                                                std::memory_order_release,
                                                                                std::memory_order_relaxed))
        {
            return current;
        }

        if (previous == current)
            return current;
        if (!observation.lastThreadId.compare_exchange_strong(previous, current, std::memory_order_release,
                                                              std::memory_order_relaxed))
            return current;

        const std::uint64_t changes = observation.changes.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::uint64_t logIndex = observation.changeLogs.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < kComponentDetailLogLimit)
        {
            Diagnostics::Log("phase1 lifecycle thread changed: path=%s oldTid=%llu newTid=%llu changes=%llu", path,
                             static_cast<unsigned long long>(previous), static_cast<unsigned long long>(current),
                             static_cast<unsigned long long>(changes));
        }
        else if (logIndex == kComponentDetailLogLimit)
        {
            Diagnostics::Log("phase1 lifecycle thread changed: path=%s further changes summarized", path);
        }
        return current;
    }

    void ObserveThreadAffinity(std::uint64_t currentThread, std::atomic_uint64_t& matches,
                               std::atomic_uint64_t& mismatches)
    {
        // Compare this callback's captured TID against the first observed main-tick TID. A mutable last-register
        // value would let another concurrent callback overwrite the identity being classified.
        const std::uint64_t mainTickThread = g_mainTickThread.firstThreadId.load(std::memory_order_acquire);
        if (currentThread == 0 || mainTickThread == 0)
            return;
        if (currentThread == mainTickThread)
            matches.fetch_add(1, std::memory_order_relaxed);
        else
            mismatches.fetch_add(1, std::memory_order_relaxed);
    }

    void MaybeLogPhase1Summary()
    {
        const ULONGLONG now = GetTickCount64();
        std::uint64_t previous = g_phase1SummaryTick.load(std::memory_order_relaxed);
        if (previous != 0 && now - previous < kPhase1SummaryIntervalMilliseconds)
            return;
        if (!g_phase1SummaryTick.compare_exchange_strong(previous, now, std::memory_order_relaxed,
                                                          std::memory_order_relaxed))
            return;

        Diagnostics::Log(
            "phase1 lifetime summary: registerCallbacks=%llu registered=%llu registerTid=%llu registerChanges=%llu "
            "registerOnMain=%llu registerOffMain=%llu mainTickCalls=%llu mainTickTid=%llu mainTickChanges=%llu "
            "unregisterCalls=%llu unregisterTid=%llu unregisterChanges=%llu unregisterOnMain=%llu "
            "unregisterOffMain=%llu unregisterTracked=%llu unregisterUntracked=%llu unregisterTrackingUnknown=%llu "
            "unregisterNoId=%llu "
            "lastRegister=(%p,0x%llX) lastUnregister=(%p,0x%llX) unregisterHook=%d "
            "componentEntities=%llu componentSkipped=%llu componentSamples=%llu layoutRejects=%llu "
            "nullInstance=%llu nullRefCount=%llu truncated=%llu "
            "attitudeAttempts=%llu attitudeSourceBlocked=%llu attitudeAcquire=%llu attitudeExpiredInvalid=%llu "
            "attitudeAgent=%llu attitudeUnknown=%llu attitudeFailClosed=%d",
            static_cast<unsigned long long>(g_registerCallbacks.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_registered.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_registerThread.lastThreadId.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_registerThread.changes.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_registerThreadAffinityMatches.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_registerThreadAffinityMismatches.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_mainTickCalls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_mainTickThread.firstThreadId.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_mainTickThread.changes.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_unregisterCallbacks.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_unregisterThread.lastThreadId.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_unregisterThread.changes.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_unregisterThreadAffinityMatches.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_unregisterThreadAffinityMismatches.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_unregisterTracked.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_unregisterUntracked.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_unregisterTrackingUnknown.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_unregisterWithoutIdentity.load(std::memory_order_relaxed)),
            reinterpret_cast<void*>(g_lastRegisteredEntityAddress.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_lastRegisteredEntityId.load(std::memory_order_relaxed)),
            reinterpret_cast<void*>(g_lastUnregisteredEntityAddress.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_lastUnregisteredEntityId.load(std::memory_order_relaxed)),
            g_unregisterHookCreated.load(std::memory_order_relaxed) ? 1 : 0,
            static_cast<unsigned long long>(g_componentHandleSampleEntities.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_componentHandleSamplingSkipped.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_componentHandleSamples.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_componentHandleLayoutRejects.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_componentHandleNullInstances.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_componentHandleNullRefCounts.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_componentHandleTruncated.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_attitudeLookupAttempts.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_attitudeSourceContractBlocked.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_attitudeLifetimeAcquisitionSuccess.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_attitudeExpiredOrInvalidReference.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_attitudeAgentLookupSuccess.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_attitudeAgentLookupUnknown.load(std::memory_order_relaxed)),
            IsPhase2AttitudeEnabled() ? 0 : 1);
    }

    std::uint64_t g_puppetSequence = 0;
    ClassificationLayout g_classificationLayout;
    using GetPropertyFn = PropertyLayout* (*)(ClassLayout*, std::uint64_t);
    GetPropertyFn g_getProperty = nullptr;
    bool g_getPropertyAttempted = false;
    constexpr std::uint32_t kFeatureHealthRequirementBit = 1u << 0;
    constexpr std::uint32_t kFeatureAttitudeRequirementBit = 1u << 1;
    constexpr std::uint32_t kFeaturePoseRequirementBit = 1u << 2;
    std::atomic_uint32_t g_featureRequirements{0};
    bool g_healthRequirementActive = false;
    bool g_attitudeRequirementActive = false;
    bool g_poseRequirementActive = false;
    // 하이라이트 경로를 완전히 잠재우기 전에 남은 per-entity desired 상태를 반드시 비워야 한다.
    // 게이트가 request/mode만 보면, desired는 남았는데 mode가 아직 안 켜진 좁은 창에서 경로가 잠들어
    // 엔티티가 하이라이트된 채로 영영 남는다. enabled가 내려가는 순간 이 래치를 세우고, 실제로 비울 게
    // 없다는 것을 확인한 뒤에만 내린다.
    std::atomic_bool g_nativeHighlightDrainPending{false};
    std::atomic_uint32_t g_nativeHighlightRequest{0};
    std::atomic_uint64_t g_nativeHighlightGeneration{0};
    std::atomic_bool g_nativeHighlightModeActive{false};
    std::atomic_bool g_cleanupRequested{false};
    std::atomic_bool g_cleanupClearQueued{false};
    std::atomic_bool g_cleanupAcknowledged{false};
    std::atomic_uint64_t g_cleanupGeneration{0};
    // Shared, main-tick-owned world gate for world-owned engine consumers that opt into transition protection.
    // The published bit is read only after EntityTracker::OnGameMainTick updates it for the current tick.
    std::atomic_bool g_worldReadyForConsumers{false};
    bool g_worldWasEmpty = true;
    ULONGLONG g_worldSettleUntil = 0;
    ULONGLONG g_lastWorldGateLog = 0;
    std::uint64_t g_healthRoundRobin = 0;
    std::uint64_t g_attitudeRoundRobin = 0;
    std::uint64_t g_poseRoundRobin = 0;
    ULONGLONG g_attitudePathLogTick = 0;

    struct NativeHighlightRuntime
    {
        bool attempted = false;
        ULONGLONG lastResolveAttempt = 0;
        Game::Rtti::Class* eventClass = nullptr;
        std::size_t eventSize = 0;
        using SetBraindanceModeFn = void (*)(void*, std::uint32_t);
        SetBraindanceModeFn setBraindanceMode = nullptr;
        ULONGLONG lastSystemAcquireFailureLog = 0;
        std::uint64_t systemAcquireFailures = 0;
        std::size_t leakedEvents = 0;
    };
    NativeHighlightRuntime g_highlightRuntime;

    struct HealthRuntime
    {
        bool attempted = false;
        ULONGLONG lastResolveAttempt = 0;
        Game::Rtti::Function* getValue = nullptr;
        Game::Rtti::Function* getMaxValue = nullptr;
        Game::Rtti::Function* reachedMin = nullptr;
        ULONGLONG lastSystemAcquireFailureLog = 0;
        std::uint64_t systemAcquireFailures = 0;
    };
    HealthRuntime g_healthRuntime;

    GetPropertyFn ResolveGetProperty()
    {
        if (g_getPropertyAttempted)
            return g_getProperty;
        g_getPropertyAttempted = true;

        using ResolveAddressFn = std::uintptr_t (*)(std::uint32_t);
        HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
        const auto resolve = red4ext
                                 ? reinterpret_cast<ResolveAddressFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                                 : nullptr;
        if (resolve)
            g_getProperty = reinterpret_cast<GetPropertyFn>(resolve(kCClassGetPropertyAddressHash));
        Diagnostics::Log("CClass::GetProperty resolver: address=%p", reinterpret_cast<void*>(g_getProperty));
        return g_getProperty;
    }

    void* ResolveGameInstanceOnMainTick()
    {
        HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
        const auto resolve = red4ext
                                 ? reinterpret_cast<ResolveAddressFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                                 : nullptr;
        if (!resolve)
            return nullptr;

        const std::uintptr_t enginePointerAddress = resolve(kGameEngineAddressHash);
        void* engine = enginePointerAddress ? *reinterpret_cast<void**>(enginePointerAddress) : nullptr;
        void* framework = engine ? *reinterpret_cast<void**>(static_cast<std::byte*>(engine) + 0x308) : nullptr;
        return framework ? *reinterpret_cast<void**>(static_cast<std::byte*>(framework) + 0x10) : nullptr;
    }

    void* VirtualFunction(void* object, std::size_t index)
    {
        if (!object)
            return nullptr;
        void** table = *reinterpret_cast<void***>(object);
        return table ? table[index] : nullptr;
    }

    void* GetSystemOnMainTick(void* gameInstance, std::uint64_t typeHash)
    {
        if (!gameInstance)
            return nullptr;
        void* rttiSystem = nullptr;
        HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
        const auto resolve = red4ext
                                 ? reinterpret_cast<ResolveAddressFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                                 : nullptr;
        const std::uintptr_t rttiGetAddress = resolve ? resolve(kRttiSystemGetAddressHash) : 0;
        if (rttiGetAddress)
            rttiSystem = reinterpret_cast<void* (*)()>(rttiGetAddress)();
        const auto getClass = reinterpret_cast<void* (*)(void*, std::uint64_t)>(VirtualFunction(rttiSystem, 2));
        void* type = getClass ? getClass(rttiSystem, typeHash) : nullptr;
        const auto getSystem = reinterpret_cast<void* (*)(void*, void*)>(VirtualFunction(gameInstance, 1));
        return getSystem && type ? getSystem(gameInstance, type) : nullptr;
    }

    void* AcquireHealthStatPoolsSystemOnMainTick()
    {
        void* statPoolsSystem = nullptr;
        __try
        {
            void* gameInstance = ResolveGameInstanceOnMainTick();
            statPoolsSystem = GetSystemOnMainTick(gameInstance, Game::Rtti::Hash("gameStatPoolsSystem"));
            if (!statPoolsSystem)
                statPoolsSystem = GetSystemOnMainTick(gameInstance, Game::Rtti::Hash("gameIStatPoolsSystem"));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            statPoolsSystem = nullptr;
        }

        if (!Game::Rtti::IsValidUserPointer(statPoolsSystem))
            statPoolsSystem = nullptr;
        if (!statPoolsSystem)
        {
            ++g_healthRuntime.systemAcquireFailures;
            const ULONGLONG now = GetTickCount64();
            if (g_healthRuntime.lastSystemAcquireFailureLog == 0 ||
                now - g_healthRuntime.lastSystemAcquireFailureLog >= 1000)
            {
                g_healthRuntime.lastSystemAcquireFailureLog = now;
                Diagnostics::Log("health stat-pool system unavailable: failures=%llu",
                                 static_cast<unsigned long long>(g_healthRuntime.systemAcquireFailures));
            }
        }
        return statPoolsSystem;
    }

    bool FindBoolProperty(const ClassLayout* type, std::uint64_t propertyName, BoolPropertyLocation& result)
    {
        constexpr std::uint64_t kInValueHolderFlag = 1ull << 21;
        if (GetPropertyFn getProperty = ResolveGetProperty())
        {
            const PropertyLayout* property = getProperty(const_cast<ClassLayout*>(type), propertyName);
            if (property)
            {
                result.offset = property->valueOffset;
                result.inValueHolder = (property->flags & kInValueHolderFlag) != 0;
                result.found = true;
                return true;
            }
        }

        // Fallback for environments without RED4ext's address resolver. The engine helper above is preferred because
        // it also handles overridden/script properties whose storage is not necessarily in the immediate class list.
        for (unsigned depth = 0; type && depth < 24; ++depth, type = type->parent)
        {
            const auto* properties = reinterpret_cast<const DynArrayLayout*>(
                reinterpret_cast<const std::byte*>(type) + 0x28);
            if (!properties->entries || properties->size > properties->capacity || properties->size > 4096)
                continue;

            for (std::uint32_t i = 0; i < properties->size; ++i)
            {
                const auto* property = static_cast<const PropertyLayout*>(properties->entries[i]);
                if (!property || property->nameHash != propertyName)
                    continue;
                result.offset = property->valueOffset;
                result.inValueHolder = (property->flags & kInValueHolderFlag) != 0;
                result.found = true;
                return true;
            }
        }
        return false;
    }

    const FunctionLayout* FindFunction(const ClassLayout* type, std::uint64_t functionName)
    {
        for (unsigned depth = 0; type && depth < 24; ++depth, type = type->parent)
        {
            const auto* functions = reinterpret_cast<const DynArrayLayout*>(
                reinterpret_cast<const std::byte*>(type) + 0x48);
            if (!functions->entries || functions->size > functions->capacity || functions->size > 8192)
                continue;

            for (std::uint32_t i = 0; i < functions->size; ++i)
            {
                const auto* function = static_cast<const FunctionLayout*>(functions->entries[i]);
                if (function && function->shortNameHash == functionName)
                    return function;
            }
        }
        return nullptr;
    }

    bool FindBoolPropertyFromGetter(const ClassLayout* type, std::uint64_t propertyName,
                                   const char* getterName, BoolPropertyLocation& result)
    {
        constexpr std::uint64_t kInValueHolderFlag = 1ull << 21;
        const FunctionLayout* function = FindFunction(type, Fnv1a64(getterName));
        if (!function || !function->bytecode || function->bytecodeSize < 10 || function->bytecodeSize > 4096)
            return false;

        // REDengine links a trivial scripted getter as: Return(0x27), ObjectField(0x1A), CProperty*.
        // Using the linked property is safer than invoking the script VM from the render thread and also survives
        // storage offsets moving between patches.
        if (function->bytecode[0] != 0x27 || function->bytecode[1] != 0x1A)
            return false;

        const PropertyLayout* property = nullptr;
        memcpy(&property, function->bytecode + 2, sizeof(property));
        if (!property || property->nameHash != propertyName || property->valueOffset > 0x10000)
            return false;

        result.offset = property->valueOffset;
        result.inValueHolder = (property->flags & kInValueHolderFlag) != 0;
        result.found = true;
        return true;
    }

    void ResolveClassificationLayout(const ClassLayout* type)
    {
        if (!g_classificationLayout.civilian.found)
        {
            constexpr std::uint64_t name = Fnv1a64("isCivilian");
            if (!FindBoolProperty(type, name, g_classificationLayout.civilian))
                FindBoolPropertyFromGetter(type, name, "IsCharacterCivilian", g_classificationLayout.civilian);
        }
        if (!g_classificationLayout.police.found)
        {
            constexpr std::uint64_t name = Fnv1a64("isPolice");
            if (!FindBoolProperty(type, name, g_classificationLayout.police))
                FindBoolPropertyFromGetter(type, name, "IsCharacterPolice", g_classificationLayout.police);
        }
        if (!g_classificationLayout.ganger.found)
        {
            constexpr std::uint64_t name = Fnv1a64("isGanger");
            if (!FindBoolProperty(type, name, g_classificationLayout.ganger))
                FindBoolPropertyFromGetter(type, name, "IsCharacterGanger", g_classificationLayout.ganger);
        }

        if (!g_classificationLayout.logged && g_classificationLayout.civilian.found &&
            g_classificationLayout.police.found && g_classificationLayout.ganger.found)
        {
            Diagnostics::Log("NPC classification RTTI resolved: civilian=0x%X/%d police=0x%X/%d ganger=0x%X/%d",
                             g_classificationLayout.civilian.offset,
                             g_classificationLayout.civilian.inValueHolder ? 1 : 0,
                             g_classificationLayout.police.offset,
                             g_classificationLayout.police.inValueHolder ? 1 : 0,
                             g_classificationLayout.ganger.offset,
                             g_classificationLayout.ganger.inValueHolder ? 1 : 0);
            g_classificationLayout.logged = true;
        }
    }

    bool ReadBoolProperty(const EntityLayout* entity, const BoolPropertyLocation& property)
    {
        if (!property.found)
            return false;
        const std::byte* base = property.inValueHolder
                                    ? static_cast<const std::byte*>(entity->valueHolder)
                                    : reinterpret_cast<const std::byte*>(entity);
        return base && *reinterpret_cast<const bool*>(base + property.offset);
    }

    Game::EntityTracker::NpcCategory ClassifyNpc(EntityLayout* entity)
    {
        ResolveClassificationLayout(entity->nativeType);
        if (ReadBoolProperty(entity, g_classificationLayout.police))
            return Game::EntityTracker::NpcCategory::Police;
        if (ReadBoolProperty(entity, g_classificationLayout.civilian))
            return Game::EntityTracker::NpcCategory::Civilian;
        if (ReadBoolProperty(entity, g_classificationLayout.ganger))
            return Game::EntityTracker::NpcCategory::Enemy;
        return Game::EntityTracker::NpcCategory::Other;
    }

    PuppetKind ClassifyPuppet(const ClassLayout* type)
    {
        constexpr std::uint64_t playerTypes[] = {
            Fnv1a64("PlayerPuppet"),
            Fnv1a64("gamePlayerPuppet"),
        };
        constexpr std::uint64_t puppetTypes[] = {
            Fnv1a64("gamePuppet"),
            Fnv1a64("gamePuppetBase"),
            Fnv1a64("gameNPCPuppet"),
            Fnv1a64("NPCPuppet"),
            Fnv1a64("ScriptedPuppet"),
        };

        bool isPuppet = false;
        for (unsigned depth = 0; type && depth < 24; ++depth, type = type->parent)
        {
            for (const std::uint64_t hash : playerTypes)
            {
                if (type->nameHash == hash)
                    return PuppetKind::Player;
            }
            for (const std::uint64_t hash : puppetTypes)
            {
                if (type->nameHash == hash)
                    isPuppet = true;
            }
        }
        return isPuppet ? PuppetKind::Npc : PuppetKind::None;
    }

    constexpr std::uint32_t kMaxComponentsPerEntity = 512;

    bool IsBoundedUserRange(std::uintptr_t address, std::size_t byteCount) noexcept
    {
        constexpr std::uintptr_t kMinUserAddress = 0x10000ull;
        constexpr std::uintptr_t kMaxUserAddress = 0x00007FFFFFFEFFFFull;
        return address >= kMinUserAddress && address <= kMaxUserAddress && byteCount <= kMaxUserAddress - address;
    }

    // This is an observation made immediately after RegisterEntity returns, while the registration argument is
    // still owned by the engine. It intentionally does not copy Handle values, call a Handle constructor, mutate
    // refcounts, dereference qword1, or use SEH/VirtualQuery as a lifetime test. qword1 counts are deferred until a
    // separately proven-live RefCnt source exists.
    void SampleComponentHandles(const EntityLayout* entity, std::uint64_t entityId)
    {
        if (!entity)
            return;

        const std::uint64_t sampleOrdinal =
            g_componentHandleSampleOrdinal.fetch_add(1, std::memory_order_relaxed) + 1;
        if (sampleOrdinal > kComponentHandleSampleEntityLimit)
        {
            const std::uint64_t skipped =
                g_componentHandleSamplingSkipped.fetch_add(1, std::memory_order_relaxed) + 1;
            if (sampleOrdinal == kComponentHandleSampleEntityLimit + 1)
            {
                Diagnostics::Log("phase1 component handle sampling cap reached: entities=%zu; later registrations "
                                 "skip component-array reads",
                                 kComponentHandleSampleEntityLimit);
            }
            if ((skipped & 0xFFu) == 0)
                MaybeLogPhase1Summary();
            return;
        }

        const std::uint64_t sampleEntity =
            g_componentHandleSampleEntities.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::byte* entries = entity->components.entries;
        const std::uint32_t capacity = entity->components.capacity;
        const std::uint32_t size = entity->components.size;
        if (sampleEntity == 1)
        {
            Diagnostics::Log("phase1 component layout sample: entity=%p id=0x%llX entries=%p size=%u capacity=%u",
                             entity, static_cast<unsigned long long>(entityId), entries, size, capacity);
        }

        if (size == 0)
        {
            if (capacity > kMaxComponentsPerEntity)
                g_componentHandleLayoutRejects.fetch_add(1, std::memory_order_relaxed);
            if ((sampleEntity & 0xFFu) == 0)
                MaybeLogPhase1Summary();
            return;
        }

        const std::size_t sampleCount = (std::min)(static_cast<std::size_t>(size), kComponentHandleSampleLimit);
        const std::size_t byteCount = sampleCount * kComponentHandleSize;
        if (!entries || size > capacity || capacity > kMaxComponentsPerEntity ||
            !IsBoundedUserRange(reinterpret_cast<std::uintptr_t>(entries), byteCount))
        {
            g_componentHandleLayoutRejects.fetch_add(1, std::memory_order_relaxed);
            if ((sampleEntity & 0xFFu) == 0)
                MaybeLogPhase1Summary();
            return;
        }

        if (size > sampleCount)
            g_componentHandleTruncated.fetch_add(1, std::memory_order_relaxed);

        for (std::size_t index = 0; index < sampleCount; ++index)
        {
            const std::uintptr_t handleAddress = reinterpret_cast<std::uintptr_t>(entries) +
                                                 index * kComponentHandleSize;
            const auto* handle = reinterpret_cast<const ComponentHandleLayout*>(handleAddress);
            const void* instance = handle->instance;
            const void* refCount = handle->refCount;
            g_componentHandleSamples.fetch_add(1, std::memory_order_relaxed);
            if (!instance)
                g_componentHandleNullInstances.fetch_add(1, std::memory_order_relaxed);
            if (!refCount)
                g_componentHandleNullRefCounts.fetch_add(1, std::memory_order_relaxed);

            const std::uint64_t detailIndex = g_componentHandleDetailLogs.fetch_add(1, std::memory_order_relaxed);
            if (detailIndex < kComponentDetailLogLimit)
            {
                Diagnostics::Log("phase1 component handle: entity=%p id=0x%llX index=%zu instance=%p "
                                 "refCount=%p strongRefs=deferred weakRefs=deferred",
                                 entity, static_cast<unsigned long long>(entityId), index, instance, refCount);
            }
            else if (detailIndex == kComponentDetailLogLimit)
            {
                Diagnostics::Log("phase1 component handle: detail log cap reached; later entries summarized");
            }
        }

        if ((sampleEntity & 0xFFu) == 0)
            MaybeLogPhase1Summary();
    }

    // 컴포넌트 포인터를 한 번에 걷어 온다. 여기서 읽는 것은 전부 게임 소유 메모리라 스트리밍 중에
    // 사라질 수 있어 SEH로 감싼다. 중간에 폴트가 나면 그때까지 모은 것만 쓴다.
    std::uint32_t CollectComponents(const EntityLayout* entity, void** output, std::uint32_t capacity)
    {
        if (!Game::Rtti::IsValidUserPointer(entity))
            return 0;

        std::uint32_t collected = 0;
        __try
        {
            if (!Game::Rtti::IsValidUserPointer(entity->components.entries) ||
                entity->components.size > entity->components.capacity ||
                entity->components.size > kMaxComponentsPerEntity)
            {
                return 0;
            }

            const std::uint32_t count = entity->components.size < capacity ? entity->components.size : capacity;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const std::byte* handle = entity->components.entries + static_cast<std::size_t>(i) * 0x10;
                void* component = *reinterpret_cast<void* const*>(handle);
                if (Game::Rtti::IsValidUserPointer(component))
                    output[collected++] = component;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return collected;
    }

    // 콜백은 SEH 밖에서 부른다. 콜백까지 감싸면 콜백 안에서 난 폴트를 여기서 삼켜 버리는데, 그러면
    // TrySnapshot의 __except가 그것을 보지 못한다. 죽어 가는 엔티티가 Stale로 판정되어 목록에서
    // 정리되는 대신, 부분 데이터를 들고 Ready로 남아 추적 목록에 계속 쌓이게 된다.
    template<typename Callback>
    void ForEachComponent(const EntityLayout* entity, Callback&& callback)
    {
        void* components[kMaxComponentsPerEntity];
        const std::uint32_t count = CollectComponents(entity, components, kMaxComponentsPerEntity);
        for (std::uint32_t i = 0; i < count; ++i)
            callback(static_cast<std::byte*>(components[i]));
    }

    // 슬롯 하나를 읽는 데 필요한 것은 entSlotComponent와 그 타입의 GetSlotTransform이고, 둘 다 슬롯
    // 이름과 무관하다. 예전에는 슬롯 이름마다 이것을 다시 찾느라 컴포넌트 리스트를 6번 걷고 컴포넌트마다
    // RTTI 계층 검사를, 그리고 같은 함수 조회를 6번 반복했다. 게다가 그 엔티티에 없는 슬롯 이름
    // (NPC에 따라 LegLeft/LegRight가 없다)에서는 조기 종료 조건이 걸리지 않아 매번 전체 순회였다.
    // 한 번만 찾아 두고 이름만 바꿔 가며 호출한다.
    constexpr std::size_t kMaxSlotAccessors = 4;

    struct SlotAccessors
    {
        std::byte* components[kMaxSlotAccessors]{};
        Game::Rtti::Function* functions[kMaxSlotAccessors]{};
        std::size_t count = 0;
    };

    enum PoseSlot : std::size_t
    {
        PoseHead,
        PoseChest,
        PoseHips,
        PoseRightHand,
        PoseLeftLeg,
        PoseRightLeg,
        PoseSlotCount,
    };

    struct SlotReadTelemetry
    {
        bool valid = false;
        float position[3]{};
        std::uint8_t invokeMask = 0;
        std::uint8_t resultMask = 0;
        std::uint8_t finiteMask = 0;
        std::uint8_t selectedAccessor = 0xFF;
    };

    struct PoseReadTelemetry
    {
        SlotReadTelemetry slots[PoseSlotCount]{};
        void* components[kMaxSlotAccessors]{};
        std::size_t accessorCount = 0;
    };

    // 하나만 찾고 끝내지 않는 이유는 예전 동작을 그대로 두기 위해서다. 예전 코드는 컴포넌트를 순서대로
    // 훑다가 "그 슬롯 이름으로 성공한" 첫 컴포넌트를 채택했으므로, 슬롯 컴포넌트가 둘 이상이면 이름마다
    // 다른 컴포넌트가 뽑힐 수 있었다. 목록으로 들고 있으면 그 순서와 결과가 동일하다.
    SlotAccessors FindSlotAccessors(const EntityLayout* entity)
    {
        constexpr std::uint64_t slotComponentName = Fnv1a64("entSlotComponent");
        constexpr std::uint64_t getSlotTransformName = Fnv1a64("GetSlotTransform");

        SlotAccessors accessors;
        ForEachComponent(entity, [&](std::byte* component) {
            if (accessors.count >= kMaxSlotAccessors)
                return;
            auto* type = Game::Rtti::NativeType(component);
            if (!Game::Rtti::IsClassOrDerived(type, slotComponentName))
                return;
            Game::Rtti::Function* function = Game::Rtti::FindFunction(type, getSlotTransformName);
            if (!function)
                return;

            accessors.components[accessors.count] = component;
            accessors.functions[accessors.count] = function;
            ++accessors.count;
        });
        return accessors;
    }

    bool ReadSlotPosition(const SlotAccessors& accessors, std::uint64_t slotName, SlotReadTelemetry& telemetry)
    {
        for (std::size_t i = 0; i < accessors.count; ++i)
        {
            WorldTransformLayout transform{};
            bool result = false;
            Game::Rtti::Argument arguments[] = {{&slotName}, {&transform}};
            const std::uint8_t accessorBit = static_cast<std::uint8_t>(1u << i);
            if (!Game::Rtti::Invoke(accessors.functions[i], accessors.components[i], arguments, 2, &result))
                continue;
            telemetry.invokeMask |= accessorBit;
            if (!result)
                continue;
            telemetry.resultMask |= accessorBit;

            constexpr float fixedPointScale = 1.0f / static_cast<float>(2 << 16);
            const float position[3] = {
                static_cast<float>(transform.x) * fixedPointScale,
                static_cast<float>(transform.y) * fixedPointScale,
                static_cast<float>(transform.z) * fixedPointScale,
            };
            if (std::isfinite(position[0]) && std::isfinite(position[1]) && std::isfinite(position[2]) &&
                std::abs(position[0]) < 1000000.0f && std::abs(position[1]) < 1000000.0f &&
                std::abs(position[2]) < 1000000.0f)
            {
                telemetry.finiteMask |= accessorBit;
                telemetry.selectedAccessor = static_cast<std::uint8_t>(i);
                telemetry.valid = true;
                memcpy(telemetry.position, position, sizeof(telemetry.position));
                return true;
            }
        }
        return false;
    }

    void AddSkeletonSegment(Game::AnimationData::VisualData& visual, const float start[3], const float end[3])
    {
        if (visual.skeletonSegmentCount >= Game::AnimationData::kMaxSkeletonSegments)
            return;
        auto& segment = visual.skeletonSegments[visual.skeletonSegmentCount++];
        memcpy(segment.start, start, sizeof(segment.start));
        memcpy(segment.end, end, sizeof(segment.end));
    }

    void AddPosePoint(Game::AnimationData::VisualData& visual, const float position[3])
    {
        if (visual.posePointCount >= Game::AnimationData::kMaxPosePoints)
            return;
        memcpy(visual.posePoints[visual.posePointCount++], position, sizeof(visual.posePoints[0]));
    }

    PoseReadTelemetry ReadCurrentPoseSlots(const EntityLayout* entity, Game::AnimationData::VisualData& visual)
    {
        Diagnostics::Profile::Scope profileScope(Diagnostics::Profile::Slot::PoseSlots);

        // 컴포넌트 순회와 RTTI 조회는 여기서 한 번이면 된다. 아래 6줄은 Invoke만 남는다.
        const SlotAccessors accessors = FindSlotAccessors(entity);
        PoseReadTelemetry telemetry;
        telemetry.accessorCount = accessors.count;
        memcpy(telemetry.components, accessors.components, sizeof(telemetry.components));
        auto& head = telemetry.slots[PoseHead];
        auto& chest = telemetry.slots[PoseChest];
        auto& hips = telemetry.slots[PoseHips];
        auto& rightHand = telemetry.slots[PoseRightHand];
        auto& leftLeg = telemetry.slots[PoseLeftLeg];
        auto& rightLeg = telemetry.slots[PoseRightLeg];
        ReadSlotPosition(accessors, Fnv1a64("Head"), head);
        ReadSlotPosition(accessors, Fnv1a64("Chest"), chest);
        ReadSlotPosition(accessors, Fnv1a64("Hips"), hips);
        ReadSlotPosition(accessors, Fnv1a64("RightHand"), rightHand);
        ReadSlotPosition(accessors, Fnv1a64("LegLeft"), leftLeg);
        ReadSlotPosition(accessors, Fnv1a64("LegRight"), rightLeg);

        visual.hasHeadPosition = head.valid;
        if (head.valid)
            memcpy(visual.headPosition, head.position, sizeof(visual.headPosition));
        visual.posePointCount = 0;
        const SlotReadTelemetry* const ordered[] = {&head, &chest, &hips, &rightHand, &leftLeg, &rightLeg};
        for (const SlotReadTelemetry* point : ordered)
        {
            if (point->valid)
                AddPosePoint(visual, point->position);
        }
        visual.skeletonSegmentCount = 0;
        if (hips.valid && chest.valid)
            AddSkeletonSegment(visual, hips.position, chest.position);
        if (chest.valid && head.valid)
            AddSkeletonSegment(visual, chest.position, head.position);
        if (chest.valid && rightHand.valid)
            AddSkeletonSegment(visual, chest.position, rightHand.position);
        if (hips.valid && leftLeg.valid)
            AddSkeletonSegment(visual, hips.position, leftLeg.position);
        if (hips.valid && rightLeg.valid)
            AddSkeletonSegment(visual, hips.position, rightLeg.position);
        return telemetry;
    }

    PoseMetrics MeasurePose(const PoseReadTelemetry& telemetry, const float entityPosition[3])
    {
        PoseMetrics metrics;
        float lowest = (std::numeric_limits<float>::max)();
        float highest = -(std::numeric_limits<float>::max)();
        for (std::size_t i = 0; i < PoseSlotCount; ++i)
        {
            const SlotReadTelemetry& slot = telemetry.slots[i];
            if (!slot.valid)
                continue;
            metrics.validMask |= static_cast<std::uint8_t>(1u << i);
            lowest = (std::min)(lowest, slot.position[2]);
            highest = (std::max)(highest, slot.position[2]);
        }
        if (metrics.validMask != 0)
            metrics.verticalSpan = highest - lowest;
        if (telemetry.slots[PoseHead].valid)
            metrics.headRelativeZ = telemetry.slots[PoseHead].position[2] - entityPosition[2];
        if (telemetry.slots[PoseHead].valid && telemetry.slots[PoseHips].valid)
        {
            metrics.headToHips = telemetry.slots[PoseHead].position[2] -
                                 telemetry.slots[PoseHips].position[2];
        }
        return metrics;
    }

    float Distance3(const float lhs[3], const float rhs[3])
    {
        const float x = lhs[0] - rhs[0];
        const float y = lhs[1] - rhs[1];
        const float z = lhs[2] - rhs[2];
        return std::sqrt(x * x + y * y + z * z);
    }

    bool ReservePoseAnomalyLogRecords(std::uint32_t count)
    {
        return g_poseAnomalyLogRecords.fetch_add(count, std::memory_order_relaxed) + count <=
               kPoseAnomalyLogRecordBudget;
    }

    void LogPoseAnomalyDetails(std::uint64_t entityId, std::uint64_t event, std::uint64_t sample,
                               const PoseReadTelemetry& telemetry)
    {
        const auto& h = telemetry.slots[PoseHead];
        const auto& c = telemetry.slots[PoseChest];
        const auto& p = telemetry.slots[PoseHips];
        const auto& r = telemetry.slots[PoseRightHand];
        const auto& l = telemetry.slots[PoseLeftLeg];
        const auto& g = telemetry.slots[PoseRightLeg];
        Diagnostics::Log(
            "[POSE-ANOMALY] slots entity=%016llX event=%llu sample=%llu "
            "H[%u:%X/%X/%X]=(%.3f,%.3f,%.3f) C[%u:%X/%X/%X]=(%.3f,%.3f,%.3f) "
            "P[%u:%X/%X/%X]=(%.3f,%.3f,%.3f) R[%u:%X/%X/%X]=(%.3f,%.3f,%.3f) "
            "L[%u:%X/%X/%X]=(%.3f,%.3f,%.3f) G[%u:%X/%X/%X]=(%.3f,%.3f,%.3f)",
            static_cast<unsigned long long>(entityId), static_cast<unsigned long long>(event),
            static_cast<unsigned long long>(sample), h.selectedAccessor, h.invokeMask, h.resultMask, h.finiteMask,
            h.position[0], h.position[1], h.position[2], c.selectedAccessor, c.invokeMask, c.resultMask, c.finiteMask,
            c.position[0], c.position[1], c.position[2], p.selectedAccessor, p.invokeMask, p.resultMask, p.finiteMask,
            p.position[0], p.position[1], p.position[2], r.selectedAccessor, r.invokeMask, r.resultMask, r.finiteMask,
            r.position[0], r.position[1], r.position[2], l.selectedAccessor, l.invokeMask, l.resultMask, l.finiteMask,
            l.position[0], l.position[1], l.position[2], g.selectedAccessor, g.invokeMask, g.resultMask, g.finiteMask,
            g.position[0], g.position[1], g.position[2]);
        Diagnostics::Log(
            "[POSE-ANOMALY] accessors entity=%016llX event=%llu count=%zu components=[%p,%p,%p,%p]",
            static_cast<unsigned long long>(entityId), static_cast<unsigned long long>(event),
            telemetry.accessorCount, telemetry.components[0], telemetry.components[1], telemetry.components[2],
            telemetry.components[3]);
    }

    void DiagnosePoseSample(TrackedPuppet& tracked, Game::AnimationData::VisualData& visual,
                            const PoseReadTelemetry& telemetry, const float entityPosition[3], bool isDead,
                            ULONGLONG now)
    {
        const PoseMetrics current = MeasurePose(telemetry, entityPosition);
        const PoseMetrics previous = tracked.poseMetrics;
        const std::uint8_t coreMask = static_cast<std::uint8_t>((1u << PoseHead) | (1u << PoseChest) |
                                                                (1u << PoseHips));
        const ULONGLONG sampleInterval = tracked.visualUpdatedAt != 0 ? now - tracked.visualUpdatedAt : 0;
        const bool comparable = tracked.poseSampleSequence != 0 && sampleInterval <= 150 &&
                                (current.validMask & coreMask) == coreMask &&
                                (previous.validMask & coreMask) == coreMask;
        const float rootTravel = tracked.poseSampleSequence != 0
                                     ? Distance3(entityPosition, tracked.poseEntityPosition)
                                     : 0.0f;
        const bool spanCollapsed = comparable && previous.verticalSpan >= 0.85f &&
                                   current.verticalSpan <= 0.50f &&
                                   current.verticalSpan <= previous.verticalSpan * 0.55f;
        const bool coreCollapsed = comparable && previous.headToHips >= 0.65f &&
                                   current.headToHips <= 0.32f &&
                                   current.headToHips <= previous.headToHips * 0.50f;
        const bool headDropped = comparable && previous.headRelativeZ >= 0.90f &&
                                 current.headRelativeZ <= 0.65f &&
                                 previous.headRelativeZ - current.headRelativeZ >= 0.50f;
        const bool transitionAnomaly = !isDead && rootTravel <= 0.40f && spanCollapsed &&
                                       (coreCollapsed || headDropped);
        const bool remainsCollapsed = tracked.poseAnomalyActive && !isDead && rootTravel <= 0.40f &&
                                      current.verticalSpan <= 0.55f &&
                                      (current.headToHips <= 0.36f || current.headRelativeZ <= 0.70f);
        const bool anomaly = transitionAnomaly || remainsCollapsed;

        visual.poseSampleSequence = ++tracked.poseSampleSequence;
        visual.poseAnomalyDetected = anomaly;
        const float boundsHeight = visual.hasBounds
                                       ? visual.boundsMaximum[2] - visual.boundsMinimum[2]
                                       : 0.0f;
        if (anomaly && !tracked.poseAnomalyActive)
        {
            tracked.poseAnomalyActive = true;
            tracked.poseAnomalyStartedAt = now;
            const std::uint64_t event = ++tracked.poseAnomalyEvents;
            if (ReservePoseAnomalyLogRecords(3))
            {
                Diagnostics::Log(
                    "[POSE-ANOMALY] begin entity=%016llX event=%llu sample=%llu dtMs=%llu rootMove=%.3f "
                    "valid=%02X->%02X span=%.3f->%.3f headRelZ=%.3f->%.3f headHips=%.3f->%.3f "
                    "boundsHeight=%.3f root=(%.3f,%.3f,%.3f)",
                    static_cast<unsigned long long>(tracked.entityId), static_cast<unsigned long long>(event),
                    static_cast<unsigned long long>(visual.poseSampleSequence),
                    static_cast<unsigned long long>(sampleInterval), rootTravel, previous.validMask,
                    current.validMask, previous.verticalSpan, current.verticalSpan, previous.headRelativeZ,
                    current.headRelativeZ, previous.headToHips, current.headToHips, boundsHeight,
                    entityPosition[0], entityPosition[1], entityPosition[2]);
                LogPoseAnomalyDetails(tracked.entityId, event, visual.poseSampleSequence, telemetry);
            }
        }
        else if (!anomaly && tracked.poseAnomalyActive)
        {
            const std::uint64_t duration = now - tracked.poseAnomalyStartedAt;
            tracked.poseAnomalyActive = false;
            if (ReservePoseAnomalyLogRecords(1))
            {
                Diagnostics::Log(
                    "[POSE-ANOMALY] recovered entity=%016llX event=%llu sample=%llu durationMs=%llu "
                    "valid=%02X span=%.3f headRelZ=%.3f headHips=%.3f boundsHeight=%.3f",
                    static_cast<unsigned long long>(tracked.entityId),
                    static_cast<unsigned long long>(tracked.poseAnomalyEvents),
                    static_cast<unsigned long long>(visual.poseSampleSequence),
                    static_cast<unsigned long long>(duration), current.validMask, current.verticalSpan,
                    current.headRelativeZ, current.headToHips, boundsHeight);
            }
        }

        tracked.poseMetrics = current;
        memcpy(tracked.poseEntityPosition, entityPosition, sizeof(tracked.poseEntityPosition));
    }

    bool ReadTransform(const EntityLayout* entity, float position[3], float orientation[4]);

    struct PoseWork
    {
        Game::Rtti::Handle entityHandle;
        EntityLayout* entity = nullptr;
        std::uint64_t entityId = 0;
        std::uint64_t sequence = 0;
        std::size_t slot = 0;
        bool isDead = false;
        bool ready = false;
        float position[3]{};
        Game::AnimationData::VisualData visual;
        PoseReadTelemetry telemetry;
    };

    void ProcessPoseWorkOnMainTick(PoseWork& work)
    {
        __try
        {
            auto* entity = static_cast<EntityLayout*>(work.entityHandle.instance);
            if (!entity || entity != work.entity || entity->entityId != work.entityId ||
                ClassifyPuppet(entity->nativeType) != PuppetKind::Npc)
            {
                return;
            }

            float orientation[4]{};
            if (!ReadTransform(entity, work.position, orientation))
                return;
            Game::AnimationData::ReadVisualData(work.entityId, work.position, work.visual);
            work.telemetry = ReadCurrentPoseSlots(entity, work.visual);
            work.ready = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            work.ready = false;
        }
    }

    void ProcessPoseOnMainTick()
    {
        constexpr std::size_t kPosePerTick = 24;
        constexpr ULONGLONG kPoseIntervalMilliseconds = 33;
        constexpr ULONGLONG kPoseRequestLifetimeMilliseconds = 250;
        std::array<PoseWork, kPosePerTick> workItems{};
        std::size_t workCount = 0;
        const ULONGLONG now = GetTickCount64();
        const std::size_t start = static_cast<std::size_t>(g_poseRoundRobin % kMaxTrackedPuppets);
        std::size_t scanned = 0;

        // Stabilize storage only long enough to copy exact strong owners. No RTTI lookup, Script VM entry, or
        // other game call is allowed while this lock is held.
        AcquireSRWLockShared(&g_puppetListLock);
        for (; scanned < kMaxTrackedPuppets && workCount < workItems.size(); ++scanned)
        {
            const std::size_t slot = (start + scanned) % kMaxTrackedPuppets;
            if (!IsPuppetOccupied(slot))
                continue;
            const TrackedPuppet& tracked = g_puppetList[slot];
            if (!tracked.entity || tracked.entityId == 0 || tracked.poseRequestedAt == 0 ||
                now - tracked.poseRequestedAt > kPoseRequestLifetimeMilliseconds ||
                (tracked.visualUpdatedAt != 0 && now - tracked.visualUpdatedAt < kPoseIntervalMilliseconds))
            {
                continue;
            }

            PoseWork& work = workItems[workCount];
            if (!Game::Rtti::CopyHandle(&tracked.entityHandle, &work.entityHandle))
                continue;
            work.entity = tracked.entity;
            work.entityId = tracked.entityId;
            work.sequence = tracked.sequence;
            work.slot = slot;
            work.isDead = tracked.isDead;
            ++workCount;
        }
        ReleaseSRWLockShared(&g_puppetListLock);
        g_poseRoundRobin = (start + (scanned == 0 ? 1 : scanned)) % kMaxTrackedPuppets;

        for (std::size_t i = 0; i < workCount; ++i)
            ProcessPoseWorkOnMainTick(workItems[i]);

        // Publish complete pose samples as one cache transaction. Present can now only observe the previous or the
        // new sample, never the six-slot capture in progress.
        AcquireSRWLockExclusive(&g_puppetListLock);
        for (std::size_t i = 0; i < workCount; ++i)
        {
            PoseWork& work = workItems[i];
            if (!work.ready)
                continue;
            TrackedPuppet& tracked = g_puppetList[work.slot];
            if (tracked.entity != work.entity || tracked.entityHandle.instance != work.entity ||
                tracked.entityId != work.entityId || tracked.sequence != work.sequence)
            {
                continue;
            }
            DiagnosePoseSample(tracked, work.visual, work.telemetry, work.position, work.isDead, now);
            tracked.visual = work.visual;
            tracked.visualUpdatedAt = now;
        }
        ReleaseSRWLockExclusive(&g_puppetListLock);

        for (std::size_t i = 0; i < workCount; ++i)
            ReleaseOwnedHandle(workItems[i].entityHandle, "pose-work");
    }

    void ClearPoseStateOnMainTick()
    {
        AcquireSRWLockExclusive(&g_puppetListLock);
        for (TrackedPuppet& tracked : g_puppetList)
        {
            if (!tracked.entity)
                continue;
            tracked.visual = {};
            tracked.visualUpdatedAt = 0;
            tracked.poseRequestedAt = 0;
            tracked.poseMetrics = {};
            memset(tracked.poseEntityPosition, 0, sizeof(tracked.poseEntityPosition));
            tracked.poseAnomalyStartedAt = 0;
            tracked.poseAnomalyActive = false;
        }
        ReleaseSRWLockExclusive(&g_puppetListLock);
    }

    bool IsCorpseDead(const EntityLayout* entity)
    {
        constexpr std::uint64_t corpseComponentNames[] = {
            Fnv1a64("entCorpseComponent"),
            Fnv1a64("CorpseComponent"),
        };
        bool dead = false;
        ForEachComponent(entity, [&](std::byte* component) {
            const auto* type = Game::Rtti::NativeType(component);
            for (const std::uint64_t name : corpseComponentNames)
                dead = dead || Game::Rtti::IsClassOrDerived(type, name);
        });
        return dead;
    }

    bool ReadTransform(const EntityLayout* entity, float position[3], float orientation[4])
    {
        if (!entity)
            return false;

        const auto* comp = entity->transformComponent;
        if (!comp || reinterpret_cast<std::uintptr_t>(comp) < 0x10000 ||
            reinterpret_cast<std::uintptr_t>(comp) > 0x7FFFFFFFFFFF)
            return false;

        constexpr float kFixedPointScale = 1.0f / static_cast<float>(2 << 16);
        const WorldTransformLayout& transform = comp->worldTransform;
        position[0] = static_cast<float>(transform.x) * kFixedPointScale;
        position[1] = static_cast<float>(transform.y) * kFixedPointScale;
        position[2] = static_cast<float>(transform.z) * kFixedPointScale;
        memcpy(orientation, transform.orientation, sizeof(transform.orientation));
        const float orientationLength = orientation[0] * orientation[0] + orientation[1] * orientation[1] +
                                        orientation[2] * orientation[2] + orientation[3] * orientation[3];
        return std::isfinite(position[0]) && std::isfinite(position[1]) && std::isfinite(position[2]) &&
               std::abs(position[0]) < 1000000.0f && std::abs(position[1]) < 1000000.0f &&
               std::abs(position[2]) < 1000000.0f && std::isfinite(orientationLength) &&
               orientationLength > 0.01f && orientationLength < 4.0f;
    }

    bool TrackPuppet(EntityLayout* entity, std::uint64_t entityId, Game::EntityTracker::NpcCategory category,
                     Game::Rtti::Handle& retainedEntity, Game::Rtti::Handle& accessEntity,
                     Game::Rtti::Handle& displacedEntity)
    {
        accessEntity = {};
        displacedEntity = {};
        AcquireSRWLockExclusive(&g_puppetListLock);

        TrackedPuppet* target = nullptr;
        TrackedPuppet* oldest = &g_puppetList[0];
        for (TrackedPuppet& tracked : g_puppetList)
        {
            if (tracked.entityId == entityId)
            {
                target = &tracked;
                break;
            }
            if (!tracked.entity && !target)
                target = &tracked;
            if (tracked.sequence < oldest->sequence)
                oldest = &tracked;
        }

        if (!target)
            target = oldest;
        const bool hasIncomingOwner = HasHandleValue(retainedEntity);
        // A streamed-out object can be replaced at the same slot/ID. Do not carry health/highlight state from the
        // old pointer into the replacement; it must earn a fresh snapshot and highlight transition. Move the old
        // owner out before clearing the slot so its exact final release happens after this lock is dropped.
        const bool sameEntity = target->entity == entity && target->entityId == entityId;
        const bool hasStoredOwner = sameEntity && HasHandleValue(target->entityHandle);
        if (!hasIncomingOwner && !hasStoredOwner)
        {
            ReleaseSRWLockExclusive(&g_puppetListLock);
            return false;
        }
        // Make the caller's post-original access owner before changing the slot. If this atomic strong increment
        // fails, leave the slot and incoming owner untouched; the caller will release the incoming owner safely.
        if (hasIncomingOwner)
        {
            if (!Game::Rtti::CopyHandle(&retainedEntity, &accessEntity))
            {
                ReleaseSRWLockExclusive(&g_puppetListLock);
                return false;
            }
        }
        else if (!Game::Rtti::CopyHandle(&target->entityHandle, &accessEntity))
        {
            ReleaseSRWLockExclusive(&g_puppetListLock);
            return false;
        }

        if (!sameEntity)
        {
            displacedEntity = target->entityHandle;
            *target = {};
        }
        else if (hasIncomingOwner)
        {
            // Duplicate registration of the same object supplies a fresh strong owner. Replace the stored owner so
            // the incoming handle is never copied without an increment; release the previous owner after unlock.
            displacedEntity = target->entityHandle;
            target->entityHandle = {};
        }
        target->entity = entity;
        target->entityId = entityId;
        target->sequence = ++g_puppetSequence;
        target->category = category;
        target->isDead = false;
        if (hasIncomingOwner)
        {
            target->entityHandle = retainedEntity;
            retainedEntity = {};
        }
        SetPuppetOccupied(static_cast<std::size_t>(target - g_puppetList.data()), true);

        std::uint64_t count = 0;
        for (const TrackedPuppet& tracked : g_puppetList)
            count += tracked.entity != nullptr ? 1u : 0u;
        g_trackedPuppets.store(count, std::memory_order_release);
        ReleaseSRWLockExclusive(&g_puppetListLock);
        return HasHandleValue(accessEntity);
    }

    enum class SnapshotResult : std::uint8_t
    {
        Ready,
        PendingPosition,
        Stale,
    };

    SnapshotResult TrySnapshot(TrackedPuppet& tracked, Game::EntityTracker::PuppetSnapshot& snapshot)
    {
        // Game streaming can free/reuse an entity independently of our list. Validate all identity data at the
        // point of use and contain a stale-pointer access; no raw pointer leaves this function.
        __try
        {
            EntityLayout* entity = tracked.entity;
            if (!entity)
                return SnapshotResult::Stale;
            if (entity->entityId != tracked.entityId || ClassifyPuppet(entity->nativeType) != PuppetKind::Npc)
            {
                return SnapshotResult::Stale;
            }

            float position[3]{};
            float orientation[4]{};
            if (!ReadTransform(entity, position, orientation))
            {
                g_pendingPosition.fetch_add(1, std::memory_order_relaxed);
                return SnapshotResult::PendingPosition;
            }

            snapshot.entityId = tracked.entityId;
            snapshot.position[0] = position[0];
            snapshot.position[1] = position[1];
            snapshot.position[2] = position[2];
            memcpy(snapshot.orientation, orientation, sizeof(orientation));
            snapshot.category = ClassifyNpc(entity);
            tracked.category = snapshot.category;
            snapshot.hostility = tracked.hostility;
            if (tracked.healthValid)
                tracked.isDead = tracked.healthReachedMin || tracked.healthCurrent <= 0.001f;
            else
                tracked.isDead = IsCorpseDead(entity);
            snapshot.isDead = tracked.isDead;
            snapshot.healthValid = tracked.healthValid;
            snapshot.healthCurrent = tracked.healthCurrent;
            snapshot.healthMax = tracked.healthMax;
            snapshot.healthRatio = tracked.healthRatio;
            snapshot.visual = tracked.visual;
            return SnapshotResult::Ready;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return SnapshotResult::Stale;
        }
    }

    void CaptureEntity(EntityLayout* entity, Game::Rtti::Handle& retainedEntity)
    {
        if (!entity)
            return;

        Game::Rtti::Handle displacedEntity;
        Game::Rtti::Handle accessEntity;
        Game::EntityTracker::NpcCategory npcCategory = Game::EntityTracker::NpcCategory::Other;
        __try
        {
            const std::uint64_t total = g_registered.fetch_add(1, std::memory_order_relaxed) + 1;
            const ClassLayout* nativeType = entity->nativeType;
            const std::uint64_t typeHash = nativeType ? nativeType->nameHash : 0;
            const PuppetKind puppetKind = ClassifyPuppet(nativeType);
            const bool puppet = puppetKind == PuppetKind::Npc;
            if (puppet)
            {
                npcCategory = ClassifyNpc(entity);
                g_puppets.fetch_add(1, std::memory_order_relaxed);
            }

            // Publish the owner before any subsequent entity reads. The duplicate copy made under the tracker lock
            // is the reference used by this function while concurrent UnregisterEntity callbacks invalidate slots.
            const std::uint64_t entityId = entity->entityId;
            bool trackingAccess = true;
            if (puppet)
            {
                trackingAccess = TrackPuppet(entity, entityId, npcCategory, retainedEntity, accessEntity,
                                             displacedEntity);
            }
            // If no retained owner could be copied, do not continue reading this entity after publishing the slot:
            // the original caller scope is the only remaining lifetime proof and UnregisterEntity may run on a
            // different thread. Other non-attitude registration counters remain valid.
            if (!puppet || trackingAccess)
            {
                EntityLayout* stableEntity = HasHandleValue(accessEntity)
                                                 ? static_cast<EntityLayout*>(accessEntity.instance)
                                                 : entity;

                float position[3]{};
                float orientation[4]{};
                bool hasPosition = false;
                if (ReadTransform(stableEntity, position, orientation))
                {
                    hasPosition = true;
                    g_positioned.fetch_add(1, std::memory_order_relaxed);
                }
                else if (puppet)
                {
                    g_pendingPosition.fetch_add(1, std::memory_order_relaxed);
                }

                // The id was read before TrackPuppet. An access violation between acquire and release would leak
                // g_lastEntityLock permanently now that this body is inside __except, so the lock below touches no
                // game memory.
                g_lastRegisteredEntityAddress.store(reinterpret_cast<std::uint64_t>(stableEntity),
                                                     std::memory_order_relaxed);
                g_lastRegisteredEntityId.store(entityId, std::memory_order_relaxed);
                AcquireSRWLockExclusive(&g_lastEntityLock);
                g_lastEntityId = entityId;
                g_lastPosition[0] = position[0];
                g_lastPosition[1] = position[1];
                g_lastPosition[2] = position[2];
                if (puppet)
                {
                    g_hasLastPuppet = true;
                    g_lastPuppetId = entityId;
                    g_lastPuppetPosition[0] = position[0];
                    g_lastPuppetPosition[1] = position[1];
                    g_lastPuppetPosition[2] = position[2];
                }
                ReleaseSRWLockExclusive(&g_lastEntityLock);

                if (total <= 5 || (total & (total - 1)) == 0)
                {
                    Diagnostics::Log("entity registered: total=%llu ptr=%p id=0x%llX typeHash=0x%llX puppet=%d "
                                     "positioned=%d pos=(%.2f, %.2f, %.2f)",
                                     static_cast<unsigned long long>(total), stableEntity,
                                     static_cast<unsigned long long>(entityId),
                                     static_cast<unsigned long long>(typeHash), puppet ? 1 : 0,
                                     hasPosition ? 1 : 0, position[0], position[1], position[2]);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Ignore stale/unmapped entity registrations safely
        }
        // Exact release can enter the engine's final-owner destruction path. Keep it outside the SEH containment
        // above so an engine exception is never swallowed with a half-completed destructor/refcount transition.
        ReleaseOwnedHandle(displacedEntity, "tracked-slot-replacement");
        ReleaseOwnedHandle(accessEntity, "register-capture");
    }

    constexpr std::uint32_t kHighlightEnabledBit = 1u << 0;
    constexpr std::uint32_t kHighlightCivilianBit = 1u << 1;
    constexpr std::uint32_t kHighlightEnemyBit = 1u << 2;
    constexpr std::uint32_t kHighlightPoliceBit = 1u << 3;
    constexpr std::uint32_t kHighlightOtherBit = 1u << 4;
    constexpr std::uint32_t kHighlightHideDeadBit = 1u << 5;
    constexpr ULONGLONG kWorldSettleMilliseconds = 1000;

    void UpdateWorldReadinessOnMainTick()
    {
        std::size_t trackedCount = 0;
        AcquireSRWLockShared(&g_puppetListLock);
        for (std::size_t slot = 0; slot < kMaxTrackedPuppets; ++slot)
        {
            if (IsPuppetOccupied(slot) && g_puppetList[slot].entity)
                ++trackedCount;
        }
        ReleaseSRWLockShared(&g_puppetListLock);

        const ULONGLONG now = GetTickCount64();
        if (trackedCount == 0)
        {
            g_worldReadyForConsumers.store(false, std::memory_order_release);
            g_worldSettleUntil = 0;
            if (!g_worldWasEmpty)
            {
                g_worldWasEmpty = true;
                if (g_lastWorldGateLog == 0 || now - g_lastWorldGateLog >= kWorldSettleMilliseconds)
                {
                    g_lastWorldGateLog = now;
                    Diagnostics::Log("world consumer gate: empty tracked=0");
                }
            }
            return;
        }

        if (g_worldWasEmpty)
        {
            g_worldWasEmpty = false;
            g_worldSettleUntil = now + kWorldSettleMilliseconds;
            g_worldReadyForConsumers.store(false, std::memory_order_release);
            if (g_lastWorldGateLog == 0 || now - g_lastWorldGateLog >= kWorldSettleMilliseconds)
            {
                g_lastWorldGateLog = now;
                Diagnostics::Log("world consumer gate: repopulated tracked=%zu settleMs=%llu", trackedCount,
                                 static_cast<unsigned long long>(kWorldSettleMilliseconds));
            }
            return;
        }

        if (g_worldSettleUntil != 0 && now < g_worldSettleUntil)
        {
            g_worldReadyForConsumers.store(false, std::memory_order_release);
            return;
        }

        if (g_worldSettleUntil != 0)
        {
            g_worldSettleUntil = 0;
            if (g_lastWorldGateLog == 0 || now - g_lastWorldGateLog >= kWorldSettleMilliseconds)
            {
                g_lastWorldGateLog = now;
                Diagnostics::Log("world consumer gate: settled tracked=%zu", trackedCount);
            }
        }
        g_worldReadyForConsumers.store(true, std::memory_order_release);
    }

    struct HighlightWork
    {
        // This is an independent strong owner copied from the tracked slot under g_puppetListLock. Do not replace it
        // with a raw Entity pointer: the work item outlives that lock while QueueEvent executes on the main tick.
        Game::Rtti::Handle entityHandle;
        std::uint64_t entityId = 0;
        std::uint64_t sequence = 0;
        std::size_t slot = 0;
        bool desired = false;
    };

    bool IsCategoryEnabled(Game::EntityTracker::NpcCategory category,
                           Game::EntityTracker::Hostility hostility, std::uint32_t settings)
    {
        // A hostile NPC follows the enemy toggle whatever its spawn archetype was. Police keep their own toggle so
        // turning them off still works once a scan or a firefight makes them hostile.
        if (hostility == Game::EntityTracker::Hostility::Hostile &&
            category != Game::EntityTracker::NpcCategory::Police)
            return (settings & kHighlightEnemyBit) != 0;
        switch (category)
        {
        case Game::EntityTracker::NpcCategory::Civilian:
            return (settings & kHighlightCivilianBit) != 0;
        case Game::EntityTracker::NpcCategory::Enemy:
            return (settings & kHighlightEnemyBit) != 0;
        case Game::EntityTracker::NpcCategory::Police:
            return (settings & kHighlightPoliceBit) != 0;
        default:
            return (settings & kHighlightOtherBit) != 0;
        }
    }

    bool ResolveNativeHighlightOnMainTick()
    {
        const ULONGLONG now = GetTickCount64();
        if (g_highlightRuntime.attempted && g_highlightRuntime.eventClass != nullptr &&
            g_highlightRuntime.eventSize == kEntRenderHighlightEventSize &&
            g_highlightRuntime.setBraindanceMode != nullptr)
            return true;
        if (g_highlightRuntime.attempted && now - g_highlightRuntime.lastResolveAttempt < 1000)
        {
            return false;
        }

        g_highlightRuntime.attempted = true;
        g_highlightRuntime.lastResolveAttempt = now;
        __try
        {
            g_highlightRuntime.eventClass = Game::Rtti::GetClass(Game::Rtti::Hash("entRenderHighlightEvent"));
            g_highlightRuntime.eventSize = Game::Rtti::ClassSize(g_highlightRuntime.eventClass);

            HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
            const auto resolve = red4ext
                                     ? reinterpret_cast<ResolveAddressFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                                     : nullptr;
            const std::uintptr_t modeAddress = resolve ? resolve(kVisionModeSetBraindanceModeAddressHash) : 0;
            g_highlightRuntime.setBraindanceMode = modeAddress
                                                       ? reinterpret_cast<NativeHighlightRuntime::SetBraindanceModeFn>(
                                                             modeAddress)
                                                       : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_highlightRuntime.eventClass = nullptr;
            g_highlightRuntime.eventSize = 0;
            g_highlightRuntime.setBraindanceMode = nullptr;
        }

        const bool resolved = g_highlightRuntime.eventClass != nullptr &&
                              g_highlightRuntime.eventSize == kEntRenderHighlightEventSize &&
                              g_highlightRuntime.setBraindanceMode != nullptr;
        Diagnostics::Log("native highlight resolver: eventClass=%p eventSize=0x%zX setBraindanceMode=%p resolved=%d",
                         g_highlightRuntime.eventClass, g_highlightRuntime.eventSize,
                         reinterpret_cast<void*>(g_highlightRuntime.setBraindanceMode), resolved ? 1 : 0);
        if (!resolved)
            g_nativeHighlightFailures.fetch_add(1, std::memory_order_relaxed);
        return resolved;
    }

    bool QueueHighlightEvent(const HighlightWork& work)
    {
        EntityLayout* entity = static_cast<EntityLayout*>(work.entityHandle.instance);
        if (!entity || !work.entityHandle.refCount || work.entityId == 0 || !IsHandleShapeValid(work.entityHandle))
            return false;
        const std::size_t eventLimit = work.desired
                                           ? kMaxLeakedHighlightEvents - kReservedHighlightClearEvents
                                           : kMaxLeakedHighlightEvents;
        if (g_highlightRuntime.leakedEvents >= eventLimit)
            return false;

        bool queued = false;
        __try
        {
            if (entity->entityId != work.entityId || ClassifyPuppet(entity->nativeType) != PuppetKind::Npc)
                return false;

            void* event = Game::Rtti::CreateInstance(g_highlightRuntime.eventClass);
            if (!event)
                return false;
            ++g_highlightRuntime.leakedEvents;

            // entRenderHighlightEvent is 0x58 bytes on Cyberpunk 2.31. CClass::CreateInstance has already installed
            // the native object header; only write the documented event fields. QueueEvent is void, so a successful
            // reflected call does not prove that it copied the handle before returning. We therefore retain this
            // local strong reference rather than risking a premature final Handle destructor while the event loop
            // may still own the object. The bounded allocation cap favors a safe leak over a UAF.
            auto* bytes = static_cast<std::byte*>(event);
            *reinterpret_cast<std::uint8_t*>(bytes + 0x40) = work.desired ? 0u : 0u; // fillIndex
            *reinterpret_cast<std::uint8_t*>(bytes + 0x41) = work.desired ? 1u : 0u; // outlineIndex
            *reinterpret_cast<std::uint8_t*>(bytes + 0x42) = work.desired ? 1u : 0u; // seeThroughWalls
            *reinterpret_cast<std::uint64_t*>(bytes + 0x48) = 0u;                     // componentName (all components)
            *reinterpret_cast<float*>(bytes + 0x50) = work.desired ? 1.0f : 0.0f;     // opacity
            *reinterpret_cast<std::uint8_t*>(bytes + 0x54) = 1u;                      // forced
            *reinterpret_cast<std::uint8_t*>(bytes + 0x55) = 0u;                      // pattern

            Game::Rtti::Handle handle;
            if (!Game::Rtti::ConstructHandle(&handle, event))
                return false;

            Game::Rtti::Function* queueEvent = Game::Rtti::FindFunction(
                Game::Rtti::NativeType(entity), Game::Rtti::Hash("QueueEvent"));
            if (!queueEvent || Game::Rtti::ParameterCount(queueEvent) != 1)
                return false;
            Game::Rtti::Argument argument{&handle};
            // QueueEvent is a reflected Void method. Invoke() returning true means the call reached the VM; it does
            // not demonstrate ownership transfer, hence the conservative local-handle lifetime above.
            queued = Game::Rtti::Invoke(queueEvent, entity, &argument, 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            queued = false;
        }

        if (queued)
        {
            g_nativeHighlightQueued.fetch_add(1, std::memory_order_relaxed);
            if (!work.desired)
                g_nativeHighlightCleared.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            g_nativeHighlightFailures.fetch_add(1, std::memory_order_relaxed);
        }
        return queued;
    }

    bool SetBraindanceModeOnMainTick(bool enabled, bool forceReassert = false)
    {
        const bool active = g_nativeHighlightModeActive.load(std::memory_order_acquire);
        if (!forceReassert && active == enabled)
            return true;
        if (!g_highlightRuntime.setBraindanceMode)
            return false;

        // A vision-mode system belongs to the current world/session. Resolve it on every transition instead of
        // retaining a raw pointer across ticks; save loads and streaming transitions can replace the object.
        void* gameInstance = nullptr;
        void* visionModeSystem = nullptr;
        __try
        {
            gameInstance = ResolveGameInstanceOnMainTick();
            visionModeSystem = GetSystemOnMainTick(gameInstance, Game::Rtti::Hash("gameIVisionModeSystem"));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            visionModeSystem = nullptr;
        }
        if (!visionModeSystem)
        {
            ++g_highlightRuntime.systemAcquireFailures;
            g_nativeHighlightFailures.fetch_add(1, std::memory_order_relaxed);
            const ULONGLONG now = GetTickCount64();
            if (g_highlightRuntime.lastSystemAcquireFailureLog == 0 ||
                now - g_highlightRuntime.lastSystemAcquireFailureLog >= 1000)
            {
                g_highlightRuntime.lastSystemAcquireFailureLog = now;
                Diagnostics::Log("native highlight system unavailable: enabled=%d gameInstance=%p failures=%llu",
                                 enabled ? 1 : 0, gameInstance,
                                 static_cast<unsigned long long>(g_highlightRuntime.systemAcquireFailures));
            }
            return false;
        }

        __try
        {
            // Keep this call adjacent to the fresh acquisition above. The local system pointer is never cached in
            // NativeHighlightRuntime or used by a later tick.
            g_highlightRuntime.setBraindanceMode(visionModeSystem, enabled ? 1u : 0u);
            g_nativeHighlightModeActive.store(enabled, std::memory_order_release);
            Diagnostics::Log("native highlight braindance mode: %s", enabled ? "1" : "0");
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_nativeHighlightFailures.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    void PublishHighlightResult(const HighlightWork& work, bool queued)
    {
        if (!queued)
            return;
        EntityLayout* entity = static_cast<EntityLayout*>(work.entityHandle.instance);
        if (!entity || work.slot >= g_puppetList.size())
            return;
        AcquireSRWLockExclusive(&g_puppetListLock);
        TrackedPuppet& tracked = g_puppetList[work.slot];
        if (tracked.entity == entity && tracked.entityHandle.instance == entity && tracked.entityId == work.entityId &&
            tracked.sequence == work.sequence)
        {
            tracked.highlightKnown = true;
            tracked.highlightDesired = work.desired;
        }
        ReleaseSRWLockExclusive(&g_puppetListLock);
    }

    bool HasDesiredHighlightState()
    {
        bool active = false;
        AcquireSRWLockShared(&g_puppetListLock);
        for (const TrackedPuppet& tracked : g_puppetList)
        {
            if (tracked.entity && tracked.highlightKnown && tracked.highlightDesired)
            {
                active = true;
                break;
            }
        }
        ReleaseSRWLockShared(&g_puppetListLock);
        return active;
    }

    void ProcessNativeHighlightsOnMainTick()
    {
        const std::uint32_t published = g_nativeHighlightRequest.load(std::memory_order_acquire);
        const std::uint32_t settings = g_cleanupRequested.load(std::memory_order_acquire) ? 0u : published;
        const bool enabled = (settings & kHighlightEnabledBit) != 0;
        std::array<HighlightWork, kMaxTrackedPuppets> workItems{};
        std::size_t workCount = 0;
        bool anyDesired = false;

        const std::int64_t collectStart = Diagnostics::Profile::Now();
        AcquireSRWLockShared(&g_puppetListLock);
        for (std::size_t slot = 0; slot < kMaxTrackedPuppets; ++slot)
        {
            if (!IsPuppetOccupied(slot))
                continue;
            const TrackedPuppet& tracked = g_puppetList[slot];
            if (!tracked.entity)
                continue;
            const bool desired = enabled && IsCategoryEnabled(tracked.category, tracked.hostility, settings) &&
                                 !((settings & kHighlightHideDeadBit) != 0 && tracked.isDead);
            // Queue only a state transition: unknown+desired=true is the first enable, while known entries queue
            // only when the cached state differs. Unknown+desired=false has nothing to clear. The cache is updated
            // only after QueueEvent succeeds below. Count a pending enable only after its Entity owner has been
            // copied successfully; an unknown transition with no owner must not turn on braindance mode.
            const bool transitionNeeded = tracked.highlightKnown ? tracked.highlightDesired != desired : desired;
            if (transitionNeeded && workCount < workItems.size())
            {
                HighlightWork& work = workItems[workCount];
                if (!Game::Rtti::CopyHandle(&tracked.entityHandle, &work.entityHandle))
                {
                    // An unknown desired enable must not turn on braindance mode without an owned Entity. Known
                    // desired state remains represented by HasDesiredHighlightState below and can keep mode active.
                    g_nativeHighlightFailures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                work.entityId = tracked.entityId;
                work.sequence = tracked.sequence;
                work.slot = slot;
                work.desired = desired;
                ++workCount;
                if (desired)
                    anyDesired = true;
            }
            else if (tracked.highlightKnown && tracked.highlightDesired && desired)
            {
                anyDesired = true;
            }
        }
        ReleaseSRWLockShared(&g_puppetListLock);
        Diagnostics::Profile::Record(Diagnostics::Profile::Slot::HighlightCollect,
                                     Diagnostics::Profile::Now() - collectStart);

        // The shared gate is updated before any feature consumer runs this tick. A false gate means world drain
        // or the bounded post-load settle window: drop only the local latch and make no engine-system calls.
        const bool worldReady = g_worldReadyForConsumers.load(std::memory_order_acquire);
        if (!worldReady)
            g_nativeHighlightModeActive.store(false, std::memory_order_release);
        // Keep mode enabled while a clear transition is pending. This prevents a failed clear from being hidden by
        // an eager mode=0 call and lets the next main tick retry the same transition.
        const bool cachedDesired = HasDesiredHighlightState();
        const bool modeDesired = anyDesired || cachedDesired;
        const bool modeActive = g_nativeHighlightModeActive.load(std::memory_order_acquire);
        const bool clearRetryNeeded = !enabled && cachedDesired;
        if (worldReady && (workCount > 0 || modeActive != modeDesired || clearRetryNeeded))
        {
            if (ResolveNativeHighlightOnMainTick())
            {
                // RedHotTools enables braindance mode before placing the first render event. Clear events are sent
                // before returning to mode 0 so the engine sees a consistent transition.
                bool modeReady = true;
                const bool currentModeActive = g_nativeHighlightModeActive.load(std::memory_order_acquire);
                if (modeDesired && (workCount > 0 || !currentModeActive))
                    modeReady = SetBraindanceModeOnMainTick(true, workCount > 0);

                // If the mode transition failed, do not enqueue events into a half-initialized render path. The
                // transition remains pending and will be retried on a later main tick.
                if (modeReady)
                {
                    for (std::size_t i = 0; i < workCount; ++i)
                        PublishHighlightResult(workItems[i], QueueHighlightEvent(workItems[i]));

                    // A clear is complete only after every known-enabled entry has published a successful clear.
                    // If any QueueEvent failed (including a cap guard), retain mode=1 and retry on a later tick.
                    if (!anyDesired && !HasDesiredHighlightState())
                        SetBraindanceModeOnMainTick(false);
                }
            }
        }

        // Work handles are retained through every QueueEvent/identity publication above. Release only after all game
        // calls are complete and after PublishHighlightResult has dropped the tracker lock.
        for (std::size_t i = 0; i < workCount; ++i)
            ReleaseOwnedHandle(workItems[i].entityHandle, "native-highlight-work");

        if (g_nativeHighlightDrainPending.load(std::memory_order_acquire) && !enabled &&
            !HasDesiredHighlightState() && !g_nativeHighlightModeActive.load(std::memory_order_acquire))
        {
            g_nativeHighlightDrainPending.store(false, std::memory_order_release);
        }

        if (g_cleanupRequested.load(std::memory_order_acquire))
        {
            const bool clearComplete = !HasDesiredHighlightState() &&
                                       !g_nativeHighlightModeActive.load(std::memory_order_acquire);
            if (clearComplete)
            {
                if (g_cleanupClearQueued.exchange(true, std::memory_order_acq_rel))
                    g_cleanupAcknowledged.store(true, std::memory_order_release);
            }
            else
            {
                g_cleanupClearQueued.store(false, std::memory_order_release);
            }
        }
    }

    bool ResolveHealthOnMainTick()
    {
        const ULONGLONG now = GetTickCount64();
        if (g_healthRuntime.attempted && g_healthRuntime.getValue &&
            g_healthRuntime.getMaxValue && g_healthRuntime.reachedMin)
            return true;
        if (g_healthRuntime.attempted && now - g_healthRuntime.lastResolveAttempt < 1000)
            return false;
        g_healthRuntime.attempted = true;
        g_healthRuntime.lastResolveAttempt = now;

        Game::Rtti::Function* getValue = nullptr;
        Game::Rtti::Function* getMaxValue = nullptr;
        Game::Rtti::Function* reachedMin = nullptr;
        bool resolved = false;
        __try
        {
            void* gameInstance = ResolveGameInstanceOnMainTick();
            // This system is only a resolver-local object. The invocation path reacquires the current system for
            // every nonempty batch, so a save/world transition cannot leave a raw system pointer in the runtime.
            void* statPoolsSystem = GetSystemOnMainTick(gameInstance, Game::Rtti::Hash("gameStatPoolsSystem"));
            if (!statPoolsSystem)
                statPoolsSystem = GetSystemOnMainTick(gameInstance, Game::Rtti::Hash("gameIStatPoolsSystem"));
            const Game::Rtti::Class* type = Game::Rtti::NativeType(statPoolsSystem);
            getValue = Game::Rtti::FindFunction(type, Game::Rtti::Hash("GetStatPoolValue"));
            getMaxValue = Game::Rtti::FindFunction(type, Game::Rtti::Hash("GetStatPoolMaxPointValue"));
            reachedMin = Game::Rtti::FindFunction(type, Game::Rtti::Hash("HasStatPoolValueReachedMin"));
            resolved = getValue && getMaxValue && reachedMin && Game::Rtti::ParameterCount(getValue) == 3 &&
                       Game::Rtti::ParameterCount(getMaxValue) == 2 && Game::Rtti::ParameterCount(reachedMin) == 2;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            resolved = false;
        }

        if (resolved)
        {
            g_healthRuntime.getValue = getValue;
            g_healthRuntime.getMaxValue = getMaxValue;
            g_healthRuntime.reachedMin = reachedMin;
        }
        else
        {
            g_healthRuntime.getValue = nullptr;
            g_healthRuntime.getMaxValue = nullptr;
            g_healthRuntime.reachedMin = nullptr;
        }
        Diagnostics::Log("health stat-pool resolver: value=%p max=%p reachedMin=%p resolved=%d",
                         g_healthRuntime.getValue, g_healthRuntime.getMaxValue, g_healthRuntime.reachedMin,
                         resolved ? 1 : 0);
        return resolved;
    }

    struct HealthWork
    {
        EntityLayout* entity = nullptr;
        std::uint64_t entityId = 0;
        std::size_t slot = 0;
        bool valid = false;
        float current = 0.0f;
        float maximum = 0.0f;
        float ratio = 0.0f;
        bool reachedMin = false;
    };

    // One exclusive lock for the whole batch, and each entry is written at its known slot. A slot can be recycled
    // between collection and publication, so identity is still validated; it is now one comparison rather than a
    // scan of all 256 entries per published value.
    void PublishHealthBatch(const HealthWork* items, std::size_t count)
    {
        std::uint64_t validCount = 0;
        AcquireSRWLockExclusive(&g_puppetListLock);
        for (std::size_t i = 0; i < count; ++i)
        {
            const HealthWork& work = items[i];
            validCount += work.valid ? 1u : 0u;
            TrackedPuppet& tracked = g_puppetList[work.slot];
            if (tracked.entity != work.entity || tracked.entityId != work.entityId)
                continue;
            tracked.healthValid = work.valid;
            tracked.healthCurrent = work.valid ? work.current : 0.0f;
            tracked.healthMax = work.valid ? work.maximum : 0.0f;
            tracked.healthRatio = work.valid ? work.ratio : 0.0f;
            tracked.healthReachedMin = work.valid && work.reachedMin;
            if (work.valid)
                tracked.isDead = work.reachedMin || work.current <= 0.001f;
        }
        ReleaseSRWLockExclusive(&g_puppetListLock);
        g_healthValid.fetch_add(validCount, std::memory_order_relaxed);
        g_healthInvalid.fetch_add(count - validCount, std::memory_order_relaxed);
    }

    void ProcessHealthOnMainTick()
    {
        constexpr std::size_t kHealthPerTick = 8;
        std::array<HealthWork, kHealthPerTick> workItems{};
        std::size_t workCount = 0;
        const std::size_t start = static_cast<std::size_t>(g_healthRoundRobin % kMaxTrackedPuppets);
        std::size_t scanned = 0;
        const std::int64_t collectStart = Diagnostics::Profile::Now();
        AcquireSRWLockShared(&g_puppetListLock);
        for (; scanned < kMaxTrackedPuppets && workCount < workItems.size(); ++scanned)
        {
            const std::size_t slot = (start + scanned) % kMaxTrackedPuppets;
            if (!IsPuppetOccupied(slot))
                continue;
            const TrackedPuppet& tracked = g_puppetList[slot];
            if (tracked.entity && tracked.entityId != 0)
            {
                HealthWork& work = workItems[workCount++];
                work.entity = tracked.entity;
                work.entityId = tracked.entityId;
                work.slot = slot;
            }
        }
        ReleaseSRWLockShared(&g_puppetListLock);
        Diagnostics::Profile::Record(Diagnostics::Profile::Slot::HealthCollect,
                                     Diagnostics::Profile::Now() - collectStart);
        g_healthRoundRobin = (start + (scanned == 0 ? 1 : scanned)) % kMaxTrackedPuppets;
        if (workCount == 0)
            return;

        const bool resolved = ResolveHealthOnMainTick();
        const std::int64_t invokeStart = Diagnostics::Profile::Now();
        // ResolveHealthOnMainTick only validates static RTTI metadata. Acquire the current world/session system
        // after the batch is collected and immediately before any stat-pool invocation; this pointer never escapes
        // this main-tick batch.
        void* statPoolsSystem = resolved ? AcquireHealthStatPoolsSystemOnMainTick() : nullptr;
        for (std::size_t i = 0; i < workCount; ++i)
        {
            bool valid = false;
            float current = 0.0f;
            float maximum = 0.0f;
            float ratio = 0.0f;
            bool reachedMin = false;
            if (resolved && statPoolsSystem)
            {
                __try
                {
                    std::int32_t pool = 17; // gamedataStatPoolType::Health
                    bool asPercentage = false;
                    Game::Rtti::Argument valueArguments[] = {{&workItems[i].entityId}, {&pool}, {&asPercentage}};
                    Game::Rtti::Argument maxArguments[] = {{&workItems[i].entityId}, {&pool}};
                    valid = Game::Rtti::Invoke(g_healthRuntime.getValue, statPoolsSystem, valueArguments, 3,
                                               &current) &&
                            Game::Rtti::Invoke(g_healthRuntime.getMaxValue, statPoolsSystem, maxArguments, 2,
                                               &maximum);
                    bool reachedResult = false;
                    if (valid)
                    {
                        valid = Game::Rtti::Invoke(g_healthRuntime.reachedMin, statPoolsSystem, maxArguments, 2,
                                                   &reachedResult);
                        reachedMin = reachedResult;
                    }
                    valid = valid && std::isfinite(current) && std::isfinite(maximum) && maximum > 0.001f;
                    if (valid)
                    {
                        ratio = std::clamp(current / maximum, 0.0f, 1.0f);
                        valid = std::isfinite(ratio);
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    valid = false;
                }
            }
            workItems[i].valid = valid;
            workItems[i].current = current;
            workItems[i].maximum = maximum;
            workItems[i].ratio = ratio;
            workItems[i].reachedMin = reachedMin;
        }
        Diagnostics::Profile::Record(Diagnostics::Profile::Slot::HealthInvoke,
                                     Diagnostics::Profile::Now() - invokeStart);
        PublishHealthBatch(workItems.data(), workCount);
    }

    void ClearHealthStateOnMainTick()
    {
        AcquireSRWLockExclusive(&g_puppetListLock);
        for (TrackedPuppet& tracked : g_puppetList)
        {
            if (!tracked.entity)
                continue;
            tracked.healthValid = false;
            tracked.healthCurrent = 0.0f;
            tracked.healthMax = 0.0f;
            tracked.healthRatio = 0.0f;
            tracked.healthReachedMin = false;
            tracked.isDead = false;
        }
        ReleaseSRWLockExclusive(&g_puppetListLock);
    }

    void ApplyAttitudeFailClosed()
    {
        const std::uint64_t blocked = g_attitudeFailClosedTicks.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!g_attitudeFailClosedLogged.exchange(true, std::memory_order_acq_rel))
        {
            Diagnostics::Log("PHASE2 FAIL-CLOSED: attitude disabled; retained Entity/GetAttitudeAgent contract or "
                             "GetAttitudeTowards signature is unavailable; hostility remains Unknown");
        }

        if (!g_attitudeFailClosedStateCleared.exchange(true, std::memory_order_acq_rel))
        {
            AcquireSRWLockExclusive(&g_puppetListLock);
            for (TrackedPuppet& tracked : g_puppetList)
            {
                if (!tracked.entity)
                    continue;
                tracked.hostility = Game::EntityTracker::Hostility::Unknown;
                tracked.hostilityUpdatedAt = 0;
            }
            ReleaseSRWLockExclusive(&g_puppetListLock);
        }

        if ((blocked & 0xFFu) == 0)
            MaybeLogPhase1Summary();
    }

    void ClearAttitudeStateOnMainTick()
    {
        AcquireSRWLockExclusive(&g_puppetListLock);
        for (TrackedPuppet& tracked : g_puppetList)
        {
            if (!tracked.entity)
                continue;
            tracked.hostility = Game::EntityTracker::Hostility::Unknown;
            tracked.hostilityUpdatedAt = 0;
        }
        ReleaseSRWLockExclusive(&g_puppetListLock);
        g_attitudeFailClosedStateCleared.store(false, std::memory_order_release);
    }

    struct AttitudeWork
    {
        EntityLayout* entity = nullptr;
        Game::Rtti::Handle entityHandle;
        std::uint64_t entityId = 0;
        std::size_t slot = 0;
        Game::EntityTracker::Hostility hostility = Game::EntityTracker::Hostility::Unknown;
    };

    struct AttitudeRuntime
    {
        bool attempted = false;
        ULONGLONG lastResolveAttempt = 0;
        Game::Rtti::Function* getLocalPlayer = nullptr;
        Game::Rtti::Function* getAttitudeAgent = nullptr;
        Game::Rtti::Function* getAttitudeTowards = nullptr;
        const Game::Rtti::Class* attitudeAgentClass = nullptr;
        std::uint64_t attitudeGetterOwnerHash = 0;
        ULONGLONG lastSystemAcquireFailureLog = 0;
        std::uint64_t systemAcquireFailures = 0;
        bool agentLogged = false;
        bool logged = false;
    };
    AttitudeRuntime g_attitudeRuntime;

    constexpr std::uint64_t kGetAttitudeAgentName = Fnv1a64("GetAttitudeAgent");
    constexpr std::uint64_t kGetAttitudeTowardsName = Fnv1a64("GetAttitudeTowards");
    constexpr std::uint64_t kGameAttitudeAgentName = Fnv1a64("gameAttitudeAgent");

    void* AcquirePlayerSystemOnMainTick()
    {
        void* playerSystem = nullptr;
        __try
        {
            void* gameInstance = ResolveGameInstanceOnMainTick();
            playerSystem = GetSystemOnMainTick(gameInstance, Game::Rtti::Hash("gameIPlayerSystem"));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            playerSystem = nullptr;
        }

        if (!Game::Rtti::IsValidUserPointer(playerSystem))
            playerSystem = nullptr;
        if (!playerSystem)
        {
            ++g_attitudeRuntime.systemAcquireFailures;
            const ULONGLONG now = GetTickCount64();
            if (g_attitudeRuntime.lastSystemAcquireFailureLog == 0 ||
                now - g_attitudeRuntime.lastSystemAcquireFailureLog >= 1000)
            {
                g_attitudeRuntime.lastSystemAcquireFailureLog = now;
                Diagnostics::Log("attitude player system unavailable: failures=%llu",
                                 static_cast<unsigned long long>(g_attitudeRuntime.systemAcquireFailures));
            }
        }
        return playerSystem;
    }

    bool ValidateHandleGetter(Game::Rtti::Function* function, Game::Rtti::FunctionInfo& info)
    {
        if (!function || !Game::Rtti::InspectFunction(function, info))
            return false;
        return info.parameterCount == 0 && info.hasReturnValue && info.returnIsHandle;
    }

    bool ValidateAttitudeTowards(Game::Rtti::Function* function, Game::Rtti::FunctionInfo& info)
    {
        if (!function || !Game::Rtti::InspectFunction(function, info))
            return false;
        // The local 2.31 metadata identifies GetAttitudeTowards as a native one-parameter integer-returning
        // method. Requiring the native handler prevents an unrelated scripted overload from entering this path.
        return info.parameterCount == 1 && info.hasReturnValue && !info.returnIsHandle &&
               (info.flags & 1u) != 0 && info.nativeHandler != nullptr;
    }

    void ReleaseAttitudeWorkHandles(AttitudeWork* workItems, std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!ReleaseOwnedHandle(workItems[i].entityHandle, "attitude-entity-work"))
                g_attitudeExpiredOrInvalidReference.fetch_add(1, std::memory_order_relaxed);
        }
    }

    bool InvokeHandleGetter(Game::Rtti::Function* function, void* context, Game::Rtti::Handle& result,
                            const char* reason)
    {
        result = {};
        Game::Rtti::FunctionInfo info;
        if (!function || !context || !ValidateHandleGetter(function, info))
            return false;

        const bool called = Game::Rtti::Invoke(function, context, nullptr, 0, &result);
        if (!called || !IsHandleShapeValid(result))
        {
            ReleaseOwnedHandle(result, reason);
            return false;
        }
        return true;
    }

    bool TryAcquireLocalPlayerAgent(void* playerSystem, Game::Rtti::Handle& playerAgent)
    {
        playerAgent = {};
        if (!IsPhase2AttitudeEnabled() || !Game::Rtti::IsValidUserPointer(playerSystem) ||
            !g_attitudeRuntime.getLocalPlayer)
            return false;

        Game::Rtti::Handle playerEntity;
        if (!InvokeHandleGetter(g_attitudeRuntime.getLocalPlayer, playerSystem, playerEntity, "local-player-result"))
            return false;

        const Game::Rtti::Class* playerType = Game::Rtti::NativeType(playerEntity.instance);
        if (!playerType || !g_attitudeRuntime.getAttitudeAgent)
        {
            ReleaseOwnedHandle(playerEntity, "local-player-entity");
            return false;
        }

        const bool agentCalled = Game::Rtti::Invoke(g_attitudeRuntime.getAttitudeAgent, playerEntity.instance,
                                                    nullptr, 0, &playerAgent);
        const bool entityReleased = ReleaseOwnedHandle(playerEntity, "local-player-entity");
        if (!agentCalled || !IsHandleShapeValid(playerAgent) || !entityReleased)
        {
            ReleaseOwnedHandle(playerAgent, "local-player-agent-result");
            return false;
        }

        const Game::Rtti::Class* agentType = Game::Rtti::NativeType(playerAgent.instance);
        if (!Game::Rtti::IsClassOrDerived(agentType, kGameAttitudeAgentName))
        {
            ReleaseOwnedHandle(playerAgent, "local-player-agent-type");
            return false;
        }
        g_attitudeAgentLookupSuccess.fetch_add(1, std::memory_order_relaxed);
        if (!g_attitudeRuntime.agentLogged)
        {
            Diagnostics::Log("phase2 local attitude agent: entityType=0x%llX getterOwner=0x%llX agent=%p",
                             static_cast<unsigned long long>(Game::Rtti::ClassNameHash(playerType)),
                             static_cast<unsigned long long>(g_attitudeRuntime.attitudeGetterOwnerHash),
                             playerAgent.instance);
            g_attitudeRuntime.agentLogged = true;
        }
        return true;
    }

    // This is the only Phase 2 attitude boundary. A raw Entity pointer is used only through entityHandle, which
    // owns the object until this function returns. GetAttitudeAgent supplies a second strong Handle for the agent;
    // that handle remains live across NativeType/IsClassOrDerived, GetAttitudeTowards, and its exact release.
    Game::EntityTracker::Hostility TryResolveAttitudeAgentSafely(const Game::Rtti::Handle& entityHandle,
                                                                 const Game::Rtti::Handle* playerAgent)
    {
        using Game::EntityTracker::Hostility;
        g_attitudeLookupAttempts.fetch_add(1, std::memory_order_relaxed);

        const auto unknown = [&](bool expired) {
            if (expired)
                g_attitudeExpiredOrInvalidReference.fetch_add(1, std::memory_order_relaxed);
            g_attitudeAgentLookupUnknown.fetch_add(1, std::memory_order_relaxed);
            return Hostility::Unknown;
        };

        if (!IsPhase2AttitudeEnabled())
        {
            g_attitudeSourceContractBlocked.fetch_add(1, std::memory_order_relaxed);
            return unknown(false);
        }
        if (!IsHandleShapeValid(entityHandle) || !playerAgent || !IsHandleShapeValid(*playerAgent))
            return unknown(true);

        const Game::Rtti::Class* entityType = Game::Rtti::NativeType(entityHandle.instance);
        if (!entityType || !g_attitudeRuntime.getAttitudeAgent)
            return unknown(false);

        Game::Rtti::Handle agent;
        const bool agentCalled = Game::Rtti::Invoke(g_attitudeRuntime.getAttitudeAgent, entityHandle.instance,
                                                    nullptr, 0, &agent);
        if (!agentCalled || !IsHandleShapeValid(agent))
        {
            ReleaseOwnedHandle(agent, "npc-agent-result");
            return unknown(true);
        }

        const Game::Rtti::Class* agentType = Game::Rtti::NativeType(agent.instance);
        if (!Game::Rtti::IsClassOrDerived(agentType, kGameAttitudeAgentName))
        {
            ReleaseOwnedHandle(agent, "npc-agent-type");
            return unknown(false);
        }
        g_attitudeAgentLookupSuccess.fetch_add(1, std::memory_order_relaxed);

        if (!g_attitudeRuntime.getAttitudeTowards)
        {
            ReleaseOwnedHandle(agent, "npc-agent-signature");
            return unknown(false);
        }

        std::int32_t attitude = -1;
        Game::Rtti::Argument arguments[] = {{const_cast<Game::Rtti::Handle*>(playerAgent)}};
        const bool called = Game::Rtti::Invoke(g_attitudeRuntime.getAttitudeTowards, agent.instance, arguments, 1,
                                               &attitude);
        const bool released = ReleaseOwnedHandle(agent, "npc-attitude-agent");
        if (!called || !released)
            return unknown(!released);

        // EAIAttitude: AIA_Friendly = 0, AIA_Neutral = 1, AIA_Hostile = 2.
        switch (attitude)
        {
        case 0:
            return Hostility::Friendly;
        case 1:
            return Hostility::Neutral;
        case 2:
            return Hostility::Hostile;
        default:
            return unknown(false);
        }
    }

    bool ResolveAttitudeOnMainTick()
    {
        if (!kPhase2AttitudeRttiCompileGate)
            return false;
        if (!Game::Rtti::HasExactHandleOwnership())
        {
            g_attitudeRttiRuntimeGate.store(false, std::memory_order_release);
            return false;
        }
        if (g_attitudeRttiRuntimeGate.load(std::memory_order_acquire) && g_attitudeRuntime.getLocalPlayer &&
            g_attitudeRuntime.getAttitudeAgent && g_attitudeRuntime.getAttitudeTowards &&
            g_attitudeRuntime.attitudeAgentClass)
            return true;

        const ULONGLONG now = GetTickCount64();
        if (g_attitudeRuntime.attempted && now - g_attitudeRuntime.lastResolveAttempt < 2000)
            return false;
        g_attitudeRuntime.attempted = true;
        g_attitudeRuntime.lastResolveAttempt = now;

        g_attitudeRuntime.getLocalPlayer = nullptr;
        g_attitudeRuntime.getAttitudeAgent = nullptr;
        g_attitudeRuntime.getAttitudeTowards = nullptr;
        g_attitudeRuntime.attitudeAgentClass = nullptr;
        g_attitudeRuntime.attitudeGetterOwnerHash = 0;

        // The player system belongs to the current world/session. Keep it resolver-local only; the batch path
        // reacquires a fresh system immediately before GetLocalPlayer.
        void* playerSystem = AcquirePlayerSystemOnMainTick();
        const Game::Rtti::Class* playerSystemType = Game::Rtti::NativeType(playerSystem);
        g_attitudeRuntime.getLocalPlayer = Game::Rtti::FindFunction(
            playerSystemType, Game::Rtti::Hash("GetLocalPlayerControlledGameObject"));
        Game::Rtti::FunctionInfo localPlayerInfo;
        const bool localPlayerValid = ValidateHandleGetter(g_attitudeRuntime.getLocalPlayer, localPlayerInfo);

        // Resolve GetAttitudeAgent from a live, ownership-bearing player Entity before opening the runtime gate.
        // This proves the zero-parameter Handle-return signature on the actual running type without touching the
        // component DynArray. During loading screens the null player simply leaves the gate closed for a retry.
        Game::Rtti::FunctionInfo getterInfo;
        Game::Rtti::Handle resolverPlayer;
        bool getterValid = false;
        if (localPlayerValid &&
            InvokeHandleGetter(g_attitudeRuntime.getLocalPlayer, playerSystem, resolverPlayer,
                               "attitude-resolver-player"))
        {
            const Game::Rtti::Class* playerType = Game::Rtti::NativeType(resolverPlayer.instance);
            Game::Rtti::Function* getter = Game::Rtti::FindFunction(playerType, kGetAttitudeAgentName);
            getterValid = ValidateHandleGetter(getter, getterInfo);
            if (getterValid)
            {
                g_attitudeRuntime.getAttitudeAgent = getter;
                g_attitudeRuntime.attitudeGetterOwnerHash = Game::Rtti::ClassNameHash(getterInfo.parent);
            }
        }
        if (!ReleaseOwnedHandle(resolverPlayer, "attitude-resolver-player"))
            getterValid = false;

        g_attitudeRuntime.attitudeAgentClass = Game::Rtti::GetClass(kGameAttitudeAgentName);
        g_attitudeRuntime.getAttitudeTowards = Game::Rtti::FindFunction(
            g_attitudeRuntime.attitudeAgentClass, kGetAttitudeTowardsName);
        Game::Rtti::FunctionInfo attitudeInfo;
        const bool attitudeValid = ValidateAttitudeTowards(g_attitudeRuntime.getAttitudeTowards, attitudeInfo);

        const bool resolved = playerSystem && localPlayerValid && getterValid && g_attitudeRuntime.attitudeAgentClass &&
                              attitudeValid;
        g_attitudeRttiRuntimeGate.store(resolved, std::memory_order_release);
        if (!g_attitudeRuntime.logged || resolved)
        {
            Diagnostics::Log(
                "phase2 attitude resolver: ownership=1 GetLocalPlayer=%p "
                "local=(owner=0x%llX params=%zu return=%d type=%u handle=%d flags=0x%X) "
                "GetAttitudeAgent=%p getter=(owner=0x%llX params=%zu return=%d type=%u handle=%d flags=0x%X) "
                "GetAttitudeTowards=%p agent=(owner=0x%llX params=%zu return=%d type=%u handle=%d flags=0x%X native=%p) "
                "resolved=%d",
                g_attitudeRuntime.getLocalPlayer,
                static_cast<unsigned long long>(Game::Rtti::ClassNameHash(localPlayerInfo.parent)),
                localPlayerInfo.parameterCount, localPlayerInfo.hasReturnValue ? 1 : 0,
                static_cast<unsigned>(localPlayerInfo.returnTypeKind),
                localPlayerInfo.returnIsHandle ? 1 : 0, localPlayerInfo.flags,
                g_attitudeRuntime.getAttitudeAgent,
                static_cast<unsigned long long>(Game::Rtti::ClassNameHash(getterInfo.parent)),
                getterInfo.parameterCount, getterInfo.hasReturnValue ? 1 : 0,
                static_cast<unsigned>(getterInfo.returnTypeKind),
                getterInfo.returnIsHandle ? 1 : 0, getterInfo.flags,
                g_attitudeRuntime.getAttitudeTowards,
                static_cast<unsigned long long>(Game::Rtti::ClassNameHash(attitudeInfo.parent)),
                attitudeInfo.parameterCount, attitudeInfo.hasReturnValue ? 1 : 0,
                static_cast<unsigned>(attitudeInfo.returnTypeKind),
                attitudeInfo.returnIsHandle ? 1 : 0, attitudeInfo.flags, attitudeInfo.nativeHandler,
                resolved ? 1 : 0);
            g_attitudeRuntime.logged = true;
        }
        return resolved;
    }

    // Same shape as PublishHealthBatch: one exclusive lock, one identity comparison at a known slot.
    void PublishHostilityBatch(const AttitudeWork* items, std::size_t count)
    {
        const ULONGLONG now = GetTickCount64();
        std::uint64_t validCount = 0;
        AcquireSRWLockExclusive(&g_puppetListLock);
        for (std::size_t i = 0; i < count; ++i)
        {
            const AttitudeWork& work = items[i];
            validCount += work.hostility != Game::EntityTracker::Hostility::Unknown ? 1u : 0u;
            TrackedPuppet& tracked = g_puppetList[work.slot];
            if (tracked.entity != work.entity || tracked.entityId != work.entityId)
                continue;
            tracked.hostility = work.hostility;
            tracked.hostilityUpdatedAt = now;
        }
        ReleaseSRWLockExclusive(&g_puppetListLock);
        g_attitudeValid.fetch_add(validCount, std::memory_order_relaxed);
        g_attitudeInvalid.fetch_add(count - validCount, std::memory_order_relaxed);
    }

    void ProcessAttitudeOnMainTick()
    {
        const bool resolved = ResolveAttitudeOnMainTick();
        if (!resolved || !IsPhase2AttitudeEnabled())
            ApplyAttitudeFailClosed();
        else
            g_attitudeFailClosedStateCleared.store(false, std::memory_order_release);

        // Attitude only matters at human reaction speed, so each puppet refreshes about four times a second. Entity
        // work items copy the already-retained slot Handle while the tracker lock is held; all RTTI/VM work below
        // happens after that lock is released.
        constexpr std::size_t kAttitudePerTick = 4;
        constexpr ULONGLONG kAttitudeIntervalMs = 250;

        const ULONGLONG now = GetTickCount64();
        std::array<AttitudeWork, kAttitudePerTick> workItems{};
        std::size_t workCount = 0;
        const std::size_t start = static_cast<std::size_t>(g_attitudeRoundRobin % kMaxTrackedPuppets);
        std::size_t scanned = 0;
        const std::int64_t collectStart = Diagnostics::Profile::Now();
        AcquireSRWLockShared(&g_puppetListLock);
        for (; scanned < kMaxTrackedPuppets && workCount < workItems.size(); ++scanned)
        {
            const std::size_t slot = (start + scanned) % kMaxTrackedPuppets;
            if (!IsPuppetOccupied(slot))
                continue;
            const TrackedPuppet& tracked = g_puppetList[slot];
            if (!tracked.entity || tracked.entityId == 0 || tracked.isDead)
                continue;
            if (tracked.hostilityUpdatedAt != 0 && now - tracked.hostilityUpdatedAt < kAttitudeIntervalMs)
                continue;
            AttitudeWork& work = workItems[workCount++];
            work.entity = tracked.entity;
            work.entityId = tracked.entityId;
            work.slot = slot;
            if (resolved && IsPhase2AttitudeEnabled())
            {
                if (Game::Rtti::CopyHandle(&tracked.entityHandle, &work.entityHandle))
                    g_attitudeLifetimeAcquisitionSuccess.fetch_add(1, std::memory_order_relaxed);
            }
        }
        ReleaseSRWLockShared(&g_puppetListLock);
        Diagnostics::Profile::Record(Diagnostics::Profile::Slot::AttitudeCollect,
                                     Diagnostics::Profile::Now() - collectStart);
        g_attitudeRoundRobin = (start + (scanned == 0 ? 1 : scanned)) % kMaxTrackedPuppets;

        const bool shouldLog = now - g_attitudePathLogTick >= 3000;
        const std::int64_t invokeStart = Diagnostics::Profile::Now();
        void* playerSystem = nullptr;
        Game::Rtti::Handle playerAgent;
        if (resolved && IsPhase2AttitudeEnabled() && workCount > 0)
        {
            // A player system belongs to the current world/session. Acquire it only for this nonempty batch and
            // pass the transient pointer directly to GetLocalPlayer; static RTTI metadata remains cached separately.
            playerSystem = AcquirePlayerSystemOnMainTick();
            if (playerSystem)
                TryAcquireLocalPlayerAgent(playerSystem, playerAgent);
        }
        for (std::size_t i = 0; i < workCount; ++i)
            workItems[i].hostility = TryResolveAttitudeAgentSafely(workItems[i].entityHandle, &playerAgent);
        Diagnostics::Profile::Record(Diagnostics::Profile::Slot::AttitudeInvoke,
                                     Diagnostics::Profile::Now() - invokeStart);

        if (workCount == 0)
        {
            ReleaseOwnedHandle(playerAgent, "attitude-player-agent");
            if (shouldLog)
            {
                g_attitudePathLogTick = now;
                Diagnostics::Log("attitude path: work=%zu phase2Enabled=%d", workCount,
                                 IsPhase2AttitudeEnabled() ? 1 : 0);
            }
            return;
        }

        if (shouldLog)
        {
            g_attitudePathLogTick = now;
            Diagnostics::Log("attitude path: work=%zu phase2Enabled=%d", workCount,
                             IsPhase2AttitudeEnabled() ? 1 : 0);
        }
        PublishHostilityBatch(workItems.data(), workCount);
        ReleaseOwnedHandle(playerAgent, "attitude-player-agent");
        ReleaseAttitudeWorkHandles(workItems.data(), workCount);
    }

    bool TryReadEntityIdBeforeRemoval(const EntityLayout* entity, std::uint64_t& entityId) noexcept
    {
        if (!entity)
            return false;

        __try
        {
            entityId = entity->entityId;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            entityId = 0;
            return false;
        }
    }

    bool HookUnregisterEntity(void* registry, EntityLayout* entity)
    {
        HookLifecycle::CallbackGuard callback;
        const std::uint64_t callbackCount = g_unregisterCallbacks.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::uint64_t currentThread = ObserveThread(g_unregisterThread, "UnregisterEntity");
        ObserveThreadAffinity(currentThread, g_unregisterThreadAffinityMatches, g_unregisterThreadAffinityMismatches);
        const std::uint64_t entityAddress = reinterpret_cast<std::uint64_t>(entity);

        std::uint64_t entityId = 0;
        const bool identityKnown = TryReadEntityIdBeforeRemoval(entity, entityId);
        Game::Rtti::Handle retainedEntity;
        bool tracked = false;
        // Invalidate and move the stored owner out before the original removal call. A concurrent attitude work item
        // that already copied the Handle remains an independent strong owner. Keep this local owner alive until the
        // original returns so its call cannot observe an object destroyed by our final release.
        AcquireSRWLockExclusive(&g_puppetListLock);
        for (std::size_t slot = 0; slot < kMaxTrackedPuppets; ++slot)
        {
            if (!IsPuppetOccupied(slot))
                continue;
            TrackedPuppet& candidate = g_puppetList[slot];
            if (candidate.entity != entity || (identityKnown && candidate.entityId != entityId))
                continue;
            tracked = true;
            retainedEntity = candidate.entityHandle;
            candidate = {};
            SetPuppetOccupied(slot, false);
            break;
        }
        std::uint64_t trackedCount = 0;
        for (const TrackedPuppet& candidate : g_puppetList)
            trackedCount += candidate.entity != nullptr ? 1u : 0u;
        g_trackedPuppets.store(trackedCount, std::memory_order_release);
        ReleaseSRWLockExclusive(&g_puppetListLock);
        constexpr bool trackedKnown = true;
        g_lastUnregisteredEntityAddress.store(entityAddress, std::memory_order_relaxed);
        g_lastUnregisteredEntityId.store(identityKnown ? entityId : 0, std::memory_order_relaxed);
        g_lastUnregisteredTrackingKnown.store(trackedKnown, std::memory_order_relaxed);
        g_lastUnregisteredTracked.store(trackedKnown && tracked, std::memory_order_relaxed);
        if (!identityKnown)
            g_unregisterWithoutIdentity.fetch_add(1, std::memory_order_relaxed);
        if (!trackedKnown)
            g_unregisterTrackingUnknown.fetch_add(1, std::memory_order_relaxed);
        else if (tracked)
            g_unregisterTracked.fetch_add(1, std::memory_order_relaxed);
        else
            g_unregisterUntracked.fetch_add(1, std::memory_order_relaxed);

        // Do not hold g_puppetListLock (or any tracker lock) across this game call. The local strong owner remains
        // alive for the complete original call and is released only after it returns.
        const bool result = g_originalUnregisterEntity(registry, entity);
        ReleaseOwnedHandle(retainedEntity, "unregister-entity");
        if (callbackCount <= 5 || (callbackCount & (callbackCount - 1)) == 0)
        {
            Diagnostics::Log("entity unregister observed: total=%llu ptr=%p id=0x%llX identity=%d trackedKnown=%d "
                             "trackedBefore=%d result=%d",
                             static_cast<unsigned long long>(callbackCount), reinterpret_cast<void*>(entityAddress),
                             static_cast<unsigned long long>(identityKnown ? entityId : 0), identityKnown ? 1 : 0,
                             trackedKnown ? 1 : 0, tracked ? 1 : 0, result ? 1 : 0);
        }
        if ((callbackCount & 0x7Fu) == 0)
            MaybeLogPhase1Summary();
        return result;
    }

    bool HookRegisterEntity(void* registry, EntityLayout* entity)
    {
        HookLifecycle::CallbackGuard callback;
        const std::uint64_t callbackCount = g_registerCallbacks.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::uint64_t currentThread = ObserveThread(g_registerThread, "RegisterEntity");
        ObserveThreadAffinity(currentThread, g_registerThreadAffinityMatches, g_registerThreadAffinityMismatches);
        g_lastRegisteredEntityAddress.store(reinterpret_cast<std::uint64_t>(entity), std::memory_order_relaxed);

        // The direct 2.31 caller retains the Entity through this detour's return. Acquire our own strong owner in
        // that known-live scope before calling the original; the slot takes ownership only after all post-original
        // reads complete. If the exact Handle ABI is unavailable, leave the attitude source gated and keep no raw
        // owner that could not be released safely.
        Game::Rtti::Handle retainedEntity;
        if (entity && Game::Rtti::HasExactHandleOwnership())
            Game::Rtti::ConstructHandle(&retainedEntity, entity);

        // The original is called exactly once and before any tracker lock. Shutdown disables all hooks and drains
        // CallbackGuard instances before clearing this pointer.
        const bool result = g_originalRegisterEntity(registry, entity);
        if (result && !HookLifecycle::IsShuttingDown())
        {
            void* expected = nullptr;
            if (g_registry.compare_exchange_strong(expected, registry, std::memory_order_release,
                                                   std::memory_order_relaxed))
            {
                Diagnostics::Log("runtime entity registry captured: registry=%p", registry);
            }
            CaptureEntity(entity, retainedEntity);
        }
        ReleaseOwnedHandle(retainedEntity, "register-entity");
        if ((callbackCount & 0x7Fu) == 0)
            MaybeLogPhase1Summary();
        return result;
    }

    std::uint8_t* ResolveRegisterEntity()
    {
        using ResolveAddressFn = std::uintptr_t (*)(std::uint32_t);
        if (HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll"))
        {
            const auto resolve = reinterpret_cast<ResolveAddressFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress"));
            if (resolve)
            {
                if (const std::uintptr_t address = resolve(kRegisterEntityAddressHash))
                {
                    Diagnostics::Log("RegisterEntity resolved through RED4ext: address=%p", reinterpret_cast<void*>(address));
                    return reinterpret_cast<std::uint8_t*>(address);
                }
            }
        }

        const auto scan = Game::Signatures::FindInText(GetModuleHandleW(nullptr), kRegisterEntityPattern,
                                                       kRegisterEntityMask, sizeof(kRegisterEntityPattern));
        Diagnostics::Log("RegisterEntity signature scan: matches=%zu address=%p", scan.matches, scan.address);
        return scan.matches == 1 ? scan.address : nullptr;
    }

    std::uint8_t* ResolveUnregisterEntity()
    {
        const auto scan = Game::Signatures::FindInText(GetModuleHandleW(nullptr),
                                                       kRuntimeEntityRegistryRemovalPattern,
                                                       kRuntimeEntityRegistryRemovalMask,
                                                       sizeof(kRuntimeEntityRegistryRemovalPattern));
        Diagnostics::Log("RuntimeEntityRegistry removal signature scan: matches=%zu address=%p", scan.matches,
                         scan.address);
        if (scan.matches != 1)
        {
            if (HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll"))
            {
                const auto resolve = reinterpret_cast<ResolveAddressFn>(
                    GetProcAddress(red4ext, "RED4ext_ResolveAddress"));
                const std::uintptr_t address = resolve ? resolve(kRuntimeEntityRegistryRemovalAddressHash) : 0;
                Diagnostics::Log("RuntimeEntityRegistry removal hash lookup not accepted: hash=%u address=%p; "
                                 "unique proven signature required",
                                 kRuntimeEntityRegistryRemovalAddressHash, reinterpret_cast<void*>(address));
            }
            return nullptr;
        }

        if (HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll"))
        {
            const auto resolve = reinterpret_cast<ResolveAddressFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress"));
            if (resolve)
            {
                if (const std::uintptr_t address = resolve(kRuntimeEntityRegistryRemovalAddressHash))
                {
                    if (reinterpret_cast<std::uint8_t*>(address) != scan.address)
                    {
                        Diagnostics::Log("RuntimeEntityRegistry removal rejected: hash address=%p differs from "
                                         "unique signature address=%p; no inferred detour installed",
                                         reinterpret_cast<void*>(address), scan.address);
                        return nullptr;
                    }
                    Diagnostics::Log("RuntimeEntityRegistry removal resolved through RED4ext and signature: "
                                     "hash=%u address=%p",
                                     kRuntimeEntityRegistryRemovalAddressHash, reinterpret_cast<void*>(address));
                }
            }
        }
        return scan.matches == 1 ? scan.address : nullptr;
    }

}

namespace Game::EntityTracker
{
    bool CreateHook()
    {
        if (g_hookCreated.load(std::memory_order_acquire))
            return true;

        std::uint8_t* target = ResolveRegisterEntity();
        if (!target)
        {
            Diagnostics::Log("entity tracker disabled: RegisterEntity address unavailable");
            return false;
        }

        std::uint8_t* removalTarget = ResolveUnregisterEntity();

        const MH_STATUS registerStatus = MH_CreateHook(target, &HookRegisterEntity,
                                                        reinterpret_cast<void**>(&g_originalRegisterEntity));
        if (registerStatus != MH_OK || !g_originalRegisterEntity)
        {
            Diagnostics::Log("MH_CreateHook(RegisterEntity) failed: %s (%d)", MH_StatusToString(registerStatus),
                             registerStatus);
            if (registerStatus == MH_OK)
                MH_RemoveHook(target);
            g_originalRegisterEntity = nullptr;
            return false;
        }

        if (!removalTarget)
        {
            // The 2.31 candidate is independently proved, but an older/newer image may not expose its unnamed hash
            // or its exact fallback signature. Keep the proven RegisterEntity observer alive and fail open for this
            // optional removal observer rather than installing an inferred target.
            if (!g_unregisterDiagnosticLogged.exchange(true, std::memory_order_acq_rel))
            {
                Diagnostics::Log("PHASE1 UnregisterEntity observer unavailable on this image: proven removal hash "
                                 "and signature did not resolve; no inferred detour installed; RegisterEntity "
                                 "observation remains active");
            }
            g_unregisterHookCreated.store(false, std::memory_order_release);
            g_hookCreated.store(true, std::memory_order_release);
            Diagnostics::Log("entity tracker hook created: RegisterEntity target=%p unregisterHook=0 "
                             "attitudeRtti=runtime-gated",
                             target);
            return true;
        }

        const MH_STATUS unregisterStatus = MH_CreateHook(removalTarget, &HookUnregisterEntity,
                                                         reinterpret_cast<void**>(&g_originalUnregisterEntity));
        if (unregisterStatus != MH_OK || !g_originalUnregisterEntity)
        {
            Diagnostics::Log("MH_CreateHook(UnregisterEntity) failed: %s (%d)", MH_StatusToString(unregisterStatus),
                             unregisterStatus);
            if (unregisterStatus == MH_OK)
                MH_RemoveHook(removalTarget);
            g_originalUnregisterEntity = nullptr;
            g_unregisterHookCreated.store(false, std::memory_order_release);
            g_hookCreated.store(true, std::memory_order_release);
            Diagnostics::Log("entity tracker hook created: RegisterEntity target=%p unregisterHook=0 "
                             "attitudeRtti=runtime-gated",
                             target);
            return true;
        }

        if (!g_unregisterDiagnosticLogged.exchange(true, std::memory_order_acq_rel))
        {
            Diagnostics::Log("PHASE1 UnregisterEntity observer proved: target=%p hash=%u ABI=(registry=this, "
                             "entity*) return=bool; original called once, no post-original entity dereference, "
                             "stale cleanup remains authoritative",
                             removalTarget, kRuntimeEntityRegistryRemovalAddressHash);
        }

        g_unregisterHookCreated.store(true, std::memory_order_release);
        g_hookCreated.store(true, std::memory_order_release);
        Diagnostics::Log("entity tracker hooks created: RegisterEntity target=%p UnregisterEntity target=%p "
                         "attitudeRtti=runtime-gated",
                         target, removalTarget);
        return true;
    }

    void Shutdown()
    {
        // d3d12_hook::Shutdown calls this only after MH_DisableHook(MH_ALL_HOOKS) and the global callback drain.
        // Keep original pointers valid until that point; clearing one while a detour can still be entered races the
        // trampoline and is not a safe unload path.
        g_hookCreated.store(false, std::memory_order_release);
        g_unregisterHookCreated.store(false, std::memory_order_release);
        g_registry.store(nullptr, std::memory_order_release);
        g_attitudeRttiRuntimeGate.store(false, std::memory_order_release);
        g_originalRegisterEntity = nullptr;
        g_originalUnregisterEntity = nullptr;

        // Hooks and callback guards are already drained by the caller. Invalidate slots first, then release their
        // exact Entity owners without holding the tracker lock so no future snapshot/attitude collection can race
        // the release. This is the same Handle ABI used by the unregister boundary.
        std::array<Game::Rtti::Handle, kMaxTrackedPuppets> retainedEntities{};
        std::size_t retainedCount = 0;
        AcquireSRWLockExclusive(&g_puppetListLock);
        for (std::size_t slot = 0; slot < kMaxTrackedPuppets; ++slot)
        {
            TrackedPuppet& tracked = g_puppetList[slot];
            if (retainedCount < retainedEntities.size())
                retainedEntities[retainedCount++] = tracked.entityHandle;
            tracked = {};
            SetPuppetOccupied(slot, false);
        }
        g_trackedPuppets.store(0, std::memory_order_release);
        ReleaseSRWLockExclusive(&g_puppetListLock);
        for (std::size_t i = 0; i < retainedCount; ++i)
            ReleaseOwnedHandle(retainedEntities[i], "tracker-shutdown");

        g_nativeHighlightModeActive.store(false, std::memory_order_release);
        g_nativeHighlightDrainPending.store(false, std::memory_order_release);
        g_featureRequirements.store(0, std::memory_order_release);
        g_healthRequirementActive = false;
        g_attitudeRequirementActive = false;
        g_poseRequirementActive = false;
        g_cleanupRequested.store(false, std::memory_order_release);
        g_cleanupClearQueued.store(false, std::memory_order_release);
        g_cleanupAcknowledged.store(false, std::memory_order_release);
        g_cleanupGeneration.store(0, std::memory_order_release);
        g_worldReadyForConsumers.store(false, std::memory_order_release);
        g_worldWasEmpty = true;
        g_worldSettleUntil = 0;
        g_lastWorldGateLog = 0;
        g_highlightRuntime = {};
        g_healthRuntime.lastSystemAcquireFailureLog = 0;
        g_healthRuntime.systemAcquireFailures = 0;
        g_attitudeRuntime.lastSystemAcquireFailureLog = 0;
        g_attitudeRuntime.systemAcquireFailures = 0;
    }

    Stats GetStats()
    {
        Stats result;
        result.hookCreated = g_hookCreated.load(std::memory_order_acquire);
        result.unregisterHookCreated = g_unregisterHookCreated.load(std::memory_order_acquire);
        result.attitudeRttiFailClosed = !IsPhase2AttitudeEnabled();
        result.registered = g_registered.load(std::memory_order_relaxed);
        result.registerCallbacks = g_registerCallbacks.load(std::memory_order_relaxed);
        result.registerThreadId = g_registerThread.lastThreadId.load(std::memory_order_relaxed);
        result.registerThreadChanges = g_registerThread.changes.load(std::memory_order_relaxed);
        result.registerOnMainTickThread = g_registerThreadAffinityMatches.load(std::memory_order_relaxed);
        result.registerOffMainTickThread = g_registerThreadAffinityMismatches.load(std::memory_order_relaxed);
        result.mainTickCalls = g_mainTickCalls.load(std::memory_order_relaxed);
        result.mainTickThreadId = g_mainTickThread.firstThreadId.load(std::memory_order_relaxed);
        result.mainTickThreadChanges = g_mainTickThread.changes.load(std::memory_order_relaxed);
        result.positioned = g_positioned.load(std::memory_order_relaxed);
        result.puppets = g_puppets.load(std::memory_order_relaxed);
        result.trackedPuppets = g_trackedPuppets.load(std::memory_order_acquire);
        result.trackedCivilians = g_trackedCivilians.load(std::memory_order_acquire);
        result.trackedEnemies = g_trackedEnemies.load(std::memory_order_acquire);
        result.trackedPolice = g_trackedPolice.load(std::memory_order_acquire);
        result.trackedHostile = g_trackedHostile.load(std::memory_order_acquire);
        result.attitudeValid = g_attitudeValid.load(std::memory_order_relaxed);
        result.attitudeInvalid = g_attitudeInvalid.load(std::memory_order_relaxed);
        result.attitudeFailClosedTicks = g_attitudeFailClosedTicks.load(std::memory_order_relaxed);
        result.attitudeLookupAttempts = g_attitudeLookupAttempts.load(std::memory_order_relaxed);
        result.attitudeSourceContractBlocked =
            g_attitudeSourceContractBlocked.load(std::memory_order_relaxed);
        result.attitudeLifetimeAcquisitionSuccess =
            g_attitudeLifetimeAcquisitionSuccess.load(std::memory_order_relaxed);
        result.attitudeExpiredOrInvalidReference =
            g_attitudeExpiredOrInvalidReference.load(std::memory_order_relaxed);
        result.attitudeAgentLookupSuccess = g_attitudeAgentLookupSuccess.load(std::memory_order_relaxed);
        result.attitudeAgentLookupUnknown = g_attitudeAgentLookupUnknown.load(std::memory_order_relaxed);
        result.pendingPosition = g_pendingPosition.load(std::memory_order_relaxed);
        result.unregistered = g_unregisterCallbacks.load(std::memory_order_relaxed);
        result.unregisterThreadId = g_unregisterThread.lastThreadId.load(std::memory_order_relaxed);
        result.unregisterThreadChanges = g_unregisterThread.changes.load(std::memory_order_relaxed);
        result.unregisterOnMainTickThread = g_unregisterThreadAffinityMatches.load(std::memory_order_relaxed);
        result.unregisterOffMainTickThread = g_unregisterThreadAffinityMismatches.load(std::memory_order_relaxed);
        result.unregisterTracked = g_unregisterTracked.load(std::memory_order_relaxed);
        result.unregisterUntracked = g_unregisterUntracked.load(std::memory_order_relaxed);
        result.unregisterTrackingUnknown = g_unregisterTrackingUnknown.load(std::memory_order_relaxed);
        result.unregisterWithoutIdentity = g_unregisterWithoutIdentity.load(std::memory_order_relaxed);
        result.staleRemoved = g_staleRemoved.load(std::memory_order_relaxed);
        result.healthValid = g_healthValid.load(std::memory_order_relaxed);
        result.healthInvalid = g_healthInvalid.load(std::memory_order_relaxed);
        result.nativeHighlightQueued = g_nativeHighlightQueued.load(std::memory_order_relaxed);
        result.nativeHighlightCleared = g_nativeHighlightCleared.load(std::memory_order_relaxed);
        result.nativeHighlightFailures = g_nativeHighlightFailures.load(std::memory_order_relaxed);
        result.componentHandleSampleEntities =
            g_componentHandleSampleEntities.load(std::memory_order_relaxed);
        result.componentHandleSamplingSkipped = g_componentHandleSamplingSkipped.load(std::memory_order_relaxed);
        result.componentHandleSamples = g_componentHandleSamples.load(std::memory_order_relaxed);
        result.componentHandleLayoutRejects = g_componentHandleLayoutRejects.load(std::memory_order_relaxed);
        result.componentHandleNullInstances = g_componentHandleNullInstances.load(std::memory_order_relaxed);
        result.componentHandleNullRefCounts = g_componentHandleNullRefCounts.load(std::memory_order_relaxed);
        result.componentHandleTruncated = g_componentHandleTruncated.load(std::memory_order_relaxed);
        result.lastRegisteredEntityAddress = g_lastRegisteredEntityAddress.load(std::memory_order_relaxed);
        result.lastRegisteredEntityId = g_lastRegisteredEntityId.load(std::memory_order_relaxed);
        result.lastUnregisteredEntityAddress = g_lastUnregisteredEntityAddress.load(std::memory_order_relaxed);
        result.lastUnregisteredEntityId = g_lastUnregisteredEntityId.load(std::memory_order_relaxed);
        result.lastUnregisteredTrackingKnown =
            g_lastUnregisteredTrackingKnown.load(std::memory_order_relaxed);
        result.lastUnregisteredTracked = g_lastUnregisteredTracked.load(std::memory_order_relaxed);

        AcquireSRWLockShared(&g_lastEntityLock);
        result.lastEntityId = g_lastEntityId;
        result.lastPosition[0] = g_lastPosition[0];
        result.lastPosition[1] = g_lastPosition[1];
        result.lastPosition[2] = g_lastPosition[2];
        result.hasLastPuppet = g_hasLastPuppet;
        result.lastPuppetId = g_lastPuppetId;
        result.lastPuppetPosition[0] = g_lastPuppetPosition[0];
        result.lastPuppetPosition[1] = g_lastPuppetPosition[1];
        result.lastPuppetPosition[2] = g_lastPuppetPosition[2];
        ReleaseSRWLockShared(&g_lastEntityLock);
        return result;
    }

    bool IsWorldReadyForMainTickConsumers()
    {
        return g_worldReadyForConsumers.load(std::memory_order_acquire);
    }

    std::size_t GetPuppetSnapshots(PuppetSnapshot* output, std::size_t capacity)
    {
        if (!output || capacity == 0)
            return 0;

        Diagnostics::Profile::Scope profileScope(Diagnostics::Profile::Slot::SnapshotPass);

        std::size_t count = 0;
        std::uint64_t trackedCount = 0;
        std::uint64_t civilians = 0;
        std::uint64_t enemies = 0;
        std::uint64_t police = 0;
        std::uint64_t hostile = 0;
        std::array<Game::Rtti::Handle, kMaxTrackedPuppets> staleHandles{};
        std::size_t staleHandleCount = 0;
        const ULONGLONG poseRequestNow = GetTickCount64();
        const std::int64_t lockWaitStart = Diagnostics::Profile::Now();
        AcquireSRWLockExclusive(&g_puppetListLock);
        Diagnostics::Profile::Record(Diagnostics::Profile::Slot::SnapshotLockWait,
                                     Diagnostics::Profile::Now() - lockWaitStart);
        for (std::size_t slot = 0; slot < kMaxTrackedPuppets; ++slot)
        {
            TrackedPuppet& tracked = g_puppetList[slot];
            if (!tracked.entity)
                continue;

            PuppetSnapshot snapshot;
            const SnapshotResult result = TrySnapshot(tracked, snapshot);
            if (result == SnapshotResult::Stale)
            {
                if (staleHandleCount < staleHandles.size())
                {
                    staleHandles[staleHandleCount++] = tracked.entityHandle;
                    tracked.entityHandle = {};
                }
                tracked = {};
                SetPuppetOccupied(slot, false);
                g_staleRemoved.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            ++trackedCount;
            if (result == SnapshotResult::PendingPosition)
                continue;
            civilians += snapshot.category == NpcCategory::Civilian ? 1u : 0u;
            enemies += snapshot.category == NpcCategory::Enemy ? 1u : 0u;
            police += snapshot.category == NpcCategory::Police ? 1u : 0u;
            hostile += snapshot.hostility == Hostility::Hostile ? 1u : 0u;
            if (count < capacity)
            {
                tracked.poseRequestedAt = poseRequestNow;
                output[count++] = snapshot;
            }
        }
        g_trackedPuppets.store(trackedCount, std::memory_order_release);
        g_trackedCivilians.store(civilians, std::memory_order_release);
        g_trackedEnemies.store(enemies, std::memory_order_release);
        g_trackedPolice.store(police, std::memory_order_release);
        g_trackedHostile.store(hostile, std::memory_order_release);
        ReleaseSRWLockExclusive(&g_puppetListLock);
        for (std::size_t i = 0; i < staleHandleCount; ++i)
            ReleaseOwnedHandle(staleHandles[i], "stale-puppet");
        Diagnostics::Profile::RecordValue(Diagnostics::Profile::Slot::SnapshotPuppets, count);
        return count;
    }

    void OnGameMainTick()
    {
        const std::uint64_t tickCount = g_mainTickCalls.fetch_add(1, std::memory_order_relaxed) + 1;
        ObserveThread(g_mainTickThread, "OnGameMainTick");
        UpdateWorldReadinessOnMainTick();
        if ((tickCount & 0x7Fu) == 0)
            MaybeLogPhase1Summary();

        // Both operations below are intentionally called only from visibility's single game-main-tick detour.
        // Present publishes requirements but never enters these RTTI/engine paths. A requirement that turns off is
        // cleared once here before the path becomes dormant, so a later re-enable starts with no stale cache.
        const std::uint32_t requirements = g_featureRequirements.load(std::memory_order_acquire);
        const bool poseRequired = (requirements & kFeaturePoseRequirementBit) != 0;
        if (poseRequired)
        {
            if (!g_poseRequirementActive)
            {
                Diagnostics::Log("pose capture activated: path=main-tick maxPerTick=24 intervalMs=33 "
                                 "requestLifetimeMs=250");
            }
            g_poseRequirementActive = true;
            Diagnostics::Profile::Scope profileScope(Diagnostics::Profile::Slot::TickPose);
            ProcessPoseOnMainTick();
        }
        else if (g_poseRequirementActive)
        {
            ClearPoseStateOnMainTick();
            g_poseRequirementActive = false;
        }

        const bool healthRequired = (requirements & kFeatureHealthRequirementBit) != 0;
        if (healthRequired)
        {
            g_healthRequirementActive = true;
            Diagnostics::Profile::Scope profileScope(Diagnostics::Profile::Slot::TickHealth);
            ProcessHealthOnMainTick();
        }
        else if (g_healthRequirementActive)
        {
            ClearHealthStateOnMainTick();
            g_healthRequirementActive = false;
        }

        const bool attitudeRequired = (requirements & kFeatureAttitudeRequirementBit) != 0;
        if (attitudeRequired)
        {
            g_attitudeRequirementActive = true;
            Diagnostics::Profile::Scope profileScope(Diagnostics::Profile::Slot::TickAttitude);
            ProcessAttitudeOnMainTick();
        }
        else if (g_attitudeRequirementActive)
        {
            ClearAttitudeStateOnMainTick();
            g_attitudeRequirementActive = false;
        }

        // Native highlight has its own request atom. Keep processing while a request, active mode, or shutdown
        // cleanup remains; once the last clear is acknowledged, an entirely disabled highlight path does no list
        // scan and no RTTI work.
        const std::uint32_t highlightRequest = g_nativeHighlightRequest.load(std::memory_order_acquire);
        const bool highlightRequired = (highlightRequest & kHighlightEnabledBit) != 0 ||
                                       g_nativeHighlightModeActive.load(std::memory_order_acquire) ||
                                       g_nativeHighlightDrainPending.load(std::memory_order_acquire) ||
                                       g_cleanupRequested.load(std::memory_order_acquire);
        if (highlightRequired)
        {
            Diagnostics::Profile::Scope profileScope(Diagnostics::Profile::Slot::TickHighlight);
            ProcessNativeHighlightsOnMainTick();
        }
    }

    bool PrepareForShutdown(std::uint32_t timeoutMilliseconds)
    {
        g_featureRequirements.store(0, std::memory_order_release);
        g_nativeHighlightRequest.store(0, std::memory_order_release);
        // A Present-side request may have been published before the first main tick, but that is not engine state yet.
        // Only wait when a mode transition or an actual per-entity enable event has been acknowledged.
        const bool wasActive = g_nativeHighlightModeActive.load(std::memory_order_acquire) ||
                               HasDesiredHighlightState();
        if (!wasActive)
            return true;

        g_cleanupRequested.store(true, std::memory_order_release);
        g_cleanupClearQueued.store(false, std::memory_order_release);
        g_cleanupAcknowledged.store(false, std::memory_order_release);
        const std::uint64_t generation = g_nativeHighlightGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        g_cleanupGeneration.store(generation, std::memory_order_release);

        const ULONGLONG deadline = GetTickCount64() + timeoutMilliseconds;
        while (!g_cleanupAcknowledged.load(std::memory_order_acquire))
        {
            if (GetTickCount64() >= deadline)
            {
                Diagnostics::Log("native highlight cleanup timed out: generation=%llu mode=%d",
                                 static_cast<unsigned long long>(generation),
                                 g_nativeHighlightModeActive.load(std::memory_order_acquire) ? 1 : 0);
                return false;
            }
            Sleep(1);
        }
        Diagnostics::Log("native highlight cleanup acknowledged: generation=%llu queued=%llu cleared=%llu",
                         static_cast<unsigned long long>(generation),
                         static_cast<unsigned long long>(g_nativeHighlightQueued.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_nativeHighlightCleared.load(std::memory_order_relaxed)));
        return true;
    }

    void UpdateNativeHighlights(bool enabled, bool showCivilians, bool showEnemies, bool showPolice,
                                bool showUnclassified, bool hideDead)
    {
        std::uint32_t settings = 0;
        settings |= enabled ? kHighlightEnabledBit : 0u;
        settings |= showCivilians ? kHighlightCivilianBit : 0u;
        settings |= showEnemies ? kHighlightEnemyBit : 0u;
        settings |= showPolice ? kHighlightPoliceBit : 0u;
        settings |= showUnclassified ? kHighlightOtherBit : 0u;
        settings |= hideDead ? kHighlightHideDeadBit : 0u;

        std::uint32_t previous = g_nativeHighlightRequest.load(std::memory_order_acquire);
        while (previous != settings &&
               !g_nativeHighlightRequest.compare_exchange_weak(previous, settings, std::memory_order_acq_rel,
                                                               std::memory_order_acquire))
        {
        }
        if (previous != settings)
        {
            // 켜져 있다가 꺼진 전환에서만 세운다. 처음부터 꺼져 있던 경로는 비울 것이 없다.
            if ((previous & kHighlightEnabledBit) != 0 && (settings & kHighlightEnabledBit) == 0)
                g_nativeHighlightDrainPending.store(true, std::memory_order_release);
            g_nativeHighlightGeneration.fetch_add(1, std::memory_order_acq_rel);
            Diagnostics::Log("native highlight desired settings published: enabled=%d civilians=%d enemies=%d "
                             "police=%d other=%d hideDead=%d",
                             enabled ? 1 : 0, showCivilians ? 1 : 0, showEnemies ? 1 : 0,
                             showPolice ? 1 : 0, showUnclassified ? 1 : 0, hideDead ? 1 : 0);
        }
    }

    void UpdateFeatureRequirements(bool health, bool attitude, bool pose)
    {
        std::uint32_t requirements = 0;
        requirements |= health ? kFeatureHealthRequirementBit : 0u;
        requirements |= attitude ? kFeatureAttitudeRequirementBit : 0u;
        requirements |= pose ? kFeaturePoseRequirementBit : 0u;
        const std::uint32_t previous = g_featureRequirements.exchange(requirements, std::memory_order_acq_rel);
        if (previous != requirements)
        {
            Diagnostics::Log("tracker requirements published: health=%d attitude=%d pose=%d",
                             health ? 1 : 0, attitude ? 1 : 0, pose ? 1 : 0);
        }
    }
}
