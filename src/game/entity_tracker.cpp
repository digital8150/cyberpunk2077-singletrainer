#include "entity_tracker.h"
#include "signature_scanner.h"
#include "../diagnostics.h"
#include "../framework.h"
#include "../hooks/hook_lifecycle.h"

#include <MinHook.h>

#include <atomic>
#include <array>
#include <cmath>
#include <cstddef>

namespace
{
    // RED4ext/CET 공식 주소 해시: world::RuntimeEntityRegistry::RegisterEntity.
    constexpr std::uint32_t kRegisterEntityAddressHash = 2840271332u;

    // Cyberpunk 2077 2.31 / internal 3.0.80.51928에서 검증. 상대 call 대상만 wildcard 처리했으며
    // tools/scripts/memtool.py aobscan으로 Cyberpunk2077.exe 내 정확히 1개 매치를 확인했다.
    constexpr std::uint8_t kRegisterEntityPattern[] = {
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18,
        0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x60,
        0x48, 0x8B, 0xF1, 0x48, 0x8B, 0xFA, 0x48, 0x83, 0xC1, 0x48,
        0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x47, 0x48,
    };
    constexpr char kRegisterEntityMask[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxx";
    static_assert(sizeof(kRegisterEntityPattern) == sizeof(kRegisterEntityMask) - 1);

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

    struct WorldTransformLayout
    {
        std::int32_t x;
        std::int32_t y;
        std::int32_t z;
        std::byte pad0C[4];
        std::byte orientation[16];
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
        std::byte pad38[0x48 - 0x38];
        std::uint64_t entityId;
        std::byte pad50[0xB0 - 0x50];
        PlacedComponentLayout* transformComponent;
    };
    static_assert(offsetof(EntityLayout, nativeType) == 0x30);
    static_assert(offsetof(EntityLayout, entityId) == 0x48);
    static_assert(offsetof(EntityLayout, transformComponent) == 0xB0);

    enum class PuppetKind
    {
        None,
        Npc,
        Player,
    };

    struct TrackedPuppet
    {
        EntityLayout* entity = nullptr;
        std::uint64_t entityId = 0;
        std::uint64_t sequence = 0;
    };

    constexpr std::size_t kMaxTrackedPuppets = 256;

    using RegisterEntityFn = void (*)(void* registry, EntityLayout* entity);
    RegisterEntityFn g_originalRegisterEntity = nullptr;

    std::atomic_bool g_hookCreated{false};
    std::atomic_uint64_t g_registered{0};
    std::atomic_uint64_t g_positioned{0};
    std::atomic_uint64_t g_puppets{0};
    std::atomic_uint64_t g_trackedPuppets{0};
    SRWLOCK g_lastEntityLock = SRWLOCK_INIT;
    std::uint64_t g_lastEntityId = 0;
    float g_lastPosition[3]{};
    bool g_hasLastPuppet = false;
    std::uint64_t g_lastPuppetId = 0;
    float g_lastPuppetPosition[3]{};
    SRWLOCK g_puppetListLock = SRWLOCK_INIT;
    std::array<TrackedPuppet, kMaxTrackedPuppets> g_puppetList{};
    std::uint64_t g_puppetSequence = 0;

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

    bool ReadPosition(const EntityLayout* entity, float position[3])
    {
        if (!entity || !entity->transformComponent)
            return false;

        constexpr float kFixedPointScale = 1.0f / static_cast<float>(2 << 16);
        const WorldTransformLayout& transform = entity->transformComponent->worldTransform;
        position[0] = static_cast<float>(transform.x) * kFixedPointScale;
        position[1] = static_cast<float>(transform.y) * kFixedPointScale;
        position[2] = static_cast<float>(transform.z) * kFixedPointScale;
        return std::isfinite(position[0]) && std::isfinite(position[1]) && std::isfinite(position[2]) &&
               std::abs(position[0]) < 1000000.0f && std::abs(position[1]) < 1000000.0f &&
               std::abs(position[2]) < 1000000.0f;
    }

    void TrackPuppet(EntityLayout* entity)
    {
        AcquireSRWLockExclusive(&g_puppetListLock);

        TrackedPuppet* target = nullptr;
        TrackedPuppet* oldest = &g_puppetList[0];
        for (TrackedPuppet& tracked : g_puppetList)
        {
            if (tracked.entityId == entity->entityId)
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
        target->entity = entity;
        target->entityId = entity->entityId;
        target->sequence = ++g_puppetSequence;

        std::uint64_t count = 0;
        for (const TrackedPuppet& tracked : g_puppetList)
            count += tracked.entity != nullptr ? 1u : 0u;
        g_trackedPuppets.store(count, std::memory_order_release);
        ReleaseSRWLockExclusive(&g_puppetListLock);
    }

    bool TrySnapshot(const TrackedPuppet& tracked, Game::EntityTracker::PuppetSnapshot& snapshot)
    {
        // Game streaming can free/reuse an entity independently of our list. Validate all identity data at the
        // point of use and contain a stale-pointer access; no raw pointer leaves this function.
        __try
        {
            EntityLayout* entity = tracked.entity;
            if (!entity || entity->entityId != tracked.entityId ||
                ClassifyPuppet(entity->nativeType) != PuppetKind::Npc)
            {
                return false;
            }

            float position[3]{};
            if (!ReadPosition(entity, position))
                return false;

            snapshot.entityId = tracked.entityId;
            snapshot.position[0] = position[0];
            snapshot.position[1] = position[1];
            snapshot.position[2] = position[2];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void CaptureEntity(EntityLayout* entity)
    {
        if (!entity)
            return;

        const std::uint64_t total = g_registered.fetch_add(1, std::memory_order_relaxed) + 1;
        const ClassLayout* nativeType = entity->nativeType;
        const std::uint64_t typeHash = nativeType ? nativeType->nameHash : 0;
        const PuppetKind puppetKind = ClassifyPuppet(nativeType);
        const bool puppet = puppetKind == PuppetKind::Npc;
        if (puppet)
            g_puppets.fetch_add(1, std::memory_order_relaxed);

        float position[3]{};
        bool hasPosition = false;
        if (ReadPosition(entity, position))
        {
            hasPosition = true;
            g_positioned.fetch_add(1, std::memory_order_relaxed);
        }

        if (puppet && hasPosition)
            TrackPuppet(entity);

        AcquireSRWLockExclusive(&g_lastEntityLock);
        g_lastEntityId = entity->entityId;
        g_lastPosition[0] = position[0];
        g_lastPosition[1] = position[1];
        g_lastPosition[2] = position[2];
        if (puppet && hasPosition)
        {
            g_hasLastPuppet = true;
            g_lastPuppetId = entity->entityId;
            g_lastPuppetPosition[0] = position[0];
            g_lastPuppetPosition[1] = position[1];
            g_lastPuppetPosition[2] = position[2];
        }
        ReleaseSRWLockExclusive(&g_lastEntityLock);

        if (total <= 5 || (total & (total - 1)) == 0)
        {
            Diagnostics::Log("entity registered: total=%llu ptr=%p id=0x%llX typeHash=0x%llX puppet=%d "
                             "positioned=%d pos=(%.2f, %.2f, %.2f)",
                             static_cast<unsigned long long>(total), entity,
                             static_cast<unsigned long long>(entity->entityId),
                             static_cast<unsigned long long>(typeHash), puppet ? 1 : 0,
                             hasPosition ? 1 : 0, position[0], position[1], position[2]);
        }
    }

    void HookRegisterEntity(void* registry, EntityLayout* entity)
    {
        HookLifecycle::CallbackGuard callback;
        g_originalRegisterEntity(registry, entity);
        if (!HookLifecycle::IsShuttingDown())
            CaptureEntity(entity);
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
}

namespace Game::EntityTracker
{
    bool CreateHook()
    {
        std::uint8_t* target = ResolveRegisterEntity();
        if (!target)
        {
            Diagnostics::Log("entity tracker disabled: RegisterEntity address unavailable");
            return false;
        }

        const MH_STATUS status = MH_CreateHook(target, &HookRegisterEntity,
                                               reinterpret_cast<void**>(&g_originalRegisterEntity));
        if (status != MH_OK)
        {
            Diagnostics::Log("MH_CreateHook(RegisterEntity) failed: %s (%d)", MH_StatusToString(status), status);
            return false;
        }

        g_hookCreated.store(true, std::memory_order_release);
        Diagnostics::Log("entity tracker hook created: target=%p", target);
        return true;
    }

    void Shutdown()
    {
        g_hookCreated.store(false, std::memory_order_release);
        g_originalRegisterEntity = nullptr;
    }

    Stats GetStats()
    {
        Stats result;
        result.hookCreated = g_hookCreated.load(std::memory_order_acquire);
        result.registered = g_registered.load(std::memory_order_relaxed);
        result.positioned = g_positioned.load(std::memory_order_relaxed);
        result.puppets = g_puppets.load(std::memory_order_relaxed);
        result.trackedPuppets = g_trackedPuppets.load(std::memory_order_acquire);

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

    std::size_t GetPuppetSnapshots(PuppetSnapshot* output, std::size_t capacity)
    {
        if (!output || capacity == 0)
            return 0;

        std::size_t count = 0;
        std::uint64_t trackedCount = 0;
        AcquireSRWLockExclusive(&g_puppetListLock);
        for (TrackedPuppet& tracked : g_puppetList)
        {
            if (!tracked.entity)
                continue;

            PuppetSnapshot snapshot;
            if (!TrySnapshot(tracked, snapshot))
            {
                tracked = {};
                continue;
            }

            ++trackedCount;
            if (count < capacity)
                output[count++] = snapshot;
        }
        g_trackedPuppets.store(trackedCount, std::memory_order_release);
        ReleaseSRWLockExclusive(&g_puppetListLock);
        return count;
    }
}
