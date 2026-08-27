#include "entity_tracker.h"
#include "signature_scanner.h"
#include "../diagnostics.h"
#include "../framework.h"
#include "../hooks/hook_lifecycle.h"

#include <MinHook.h>

#include <atomic>
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

    using RegisterEntityFn = void (*)(void* registry, EntityLayout* entity);
    RegisterEntityFn g_originalRegisterEntity = nullptr;

    std::atomic_bool g_hookCreated{false};
    std::atomic_uint64_t g_registered{0};
    std::atomic_uint64_t g_positioned{0};
    std::atomic_uint64_t g_puppets{0};
    SRWLOCK g_lastEntityLock = SRWLOCK_INIT;
    std::uint64_t g_lastEntityId = 0;
    float g_lastPosition[3]{};

    bool IsPuppetClass(const ClassLayout* type)
    {
        constexpr std::uint64_t puppetTypes[] = {
            Fnv1a64("gamePuppet"),
            Fnv1a64("gamePuppetBase"),
            Fnv1a64("gameNPCPuppet"),
            Fnv1a64("NPCPuppet"),
            Fnv1a64("ScriptedPuppet"),
            Fnv1a64("PlayerPuppet"),
        };

        for (unsigned depth = 0; type && depth < 24; ++depth, type = type->parent)
        {
            for (const std::uint64_t hash : puppetTypes)
            {
                if (type->nameHash == hash)
                    return true;
            }
        }
        return false;
    }

    void CaptureEntity(EntityLayout* entity)
    {
        if (!entity)
            return;

        const std::uint64_t total = g_registered.fetch_add(1, std::memory_order_relaxed) + 1;
        const ClassLayout* nativeType = entity->nativeType;
        const std::uint64_t typeHash = nativeType ? nativeType->nameHash : 0;
        const bool puppet = IsPuppetClass(nativeType);
        if (puppet)
            g_puppets.fetch_add(1, std::memory_order_relaxed);

        float position[3]{};
        bool hasPosition = false;
        if (entity->transformComponent)
        {
            constexpr float kFixedPointScale = 1.0f / static_cast<float>(2 << 16);
            const WorldTransformLayout& transform = entity->transformComponent->worldTransform;
            position[0] = static_cast<float>(transform.x) * kFixedPointScale;
            position[1] = static_cast<float>(transform.y) * kFixedPointScale;
            position[2] = static_cast<float>(transform.z) * kFixedPointScale;
            hasPosition = true;
            g_positioned.fetch_add(1, std::memory_order_relaxed);
        }

        AcquireSRWLockExclusive(&g_lastEntityLock);
        g_lastEntityId = entity->entityId;
        g_lastPosition[0] = position[0];
        g_lastPosition[1] = position[1];
        g_lastPosition[2] = position[2];
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

        AcquireSRWLockShared(&g_lastEntityLock);
        result.lastEntityId = g_lastEntityId;
        result.lastPosition[0] = g_lastPosition[0];
        result.lastPosition[1] = g_lastPosition[1];
        result.lastPosition[2] = g_lastPosition[2];
        ReleaseSRWLockShared(&g_lastEntityLock);
        return result;
    }
}
