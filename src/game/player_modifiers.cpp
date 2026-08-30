#include "player_modifiers.h"

#include "rtti_invoker.h"
#include "../diagnostics.h"
#include "../features/features.h"
#include "../framework.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace
{
    constexpr std::uint32_t kGameEngineAddressHash = 0x97F209D6u;
    constexpr std::uint32_t kRttiSystemGetAddressHash = 0x4A610F64u;
    constexpr std::size_t kModifierDataSize = 0x50;
    constexpr std::uint32_t kMultiplierModifierType = 2;
    constexpr std::size_t kModifierCount = 11;

    struct TweakDbId
    {
        std::uint32_t hash = 0;
        std::uint8_t length = 0;
        std::uint8_t padding[3]{};
    };
    static_assert(sizeof(TweakDbId) == 8);

    struct ModifierEntry
    {
        std::int32_t statType = 0;
        std::uint64_t targetId = 0;
        void* instance = nullptr;
        Game::Rtti::Handle handle;
        // ConstructHandle establishes a strong handle before AddModifier runs. Keep this separate from active:
        // a failed/exceptional AddModifier still leaves a handle the engine may have retained and must normally
        // be removed through the exact reflected call before its exact local Handle release.
        bool cleanupTracked = false;
        bool active = false;
    };

    enum class EquippedWeaponResult : std::uint8_t
    {
        ApiUnavailable,
        NoItem,
        InvalidItem,
        CallFailed,
        Found,
    };

    enum SystemMask : std::uint32_t
    {
        kPlayerSystem = 1u << 0,
        kStatsSystem = 1u << 1,
        kTransactionSystem = 1u << 2,
    };

    struct SystemContext
    {
        // These pointers are valid only for the current main-tick/bounded operation. Runtime must never retain them.
        void* gameInstance = nullptr;
        void* playerSystem = nullptr;
        void* statsSystem = nullptr;
        void* transactionSystem = nullptr;
    };

    struct Runtime
    {
        bool attempted = false;
        ULONGLONG lastResolveAttempt = 0;
        Game::Rtti::Function* getLocalPlayer = nullptr;
        Game::Rtti::Function* addModifier = nullptr;
        Game::Rtti::Function* removeModifier = nullptr;
        Game::Rtti::Function* getItemInSlot = nullptr;
        Game::Rtti::Class* modifierDataClass = nullptr;
        std::size_t modifierDataSize = 0;
        bool exactHandleOwnership = false;
        ULONGLONG lastSystemAcquireFailureLog = 0;
        std::uint64_t systemAcquireFailures = 0;
    };

    Runtime g_runtime;
    std::array<ModifierEntry, kModifierCount> g_modifiers{};
    std::atomic_bool g_desired{false};
    std::atomic_bool g_cleanupRequested{false};
    std::atomic_bool g_cleanupAcknowledged{false};
    std::atomic_uint64_t g_playerId{0};
    std::atomic_uint64_t g_targetId{0};
    std::atomic_uint64_t g_weaponId{0};
    std::atomic_bool g_active{false};
    std::atomic_uint64_t g_applied{0};
    std::atomic_uint64_t g_removed{0};
    std::atomic_uint64_t g_retiredOwnerResets{0};
    std::atomic_uint64_t g_failures{0};
    std::atomic_bool g_usingWeaponTarget{false};
    std::atomic_bool g_runtimeAvailable{false};
    // Address values are identity tokens only. They are never dereferenced after the tick that acquired them.
    // The modifier handles belong to this exact world/player owner and must not be removed through a replacement
    // StatsSystem after a save load.
    std::atomic_uintptr_t g_ownerGameInstance{0};
    std::atomic_uintptr_t g_ownerStatsSystem{0};
    std::atomic_uintptr_t g_ownerPlayerInstance{0};
    ULONGLONG g_lastPathLogTick = 0;

    constexpr std::array<std::int32_t, kModifierCount> kRecoilStats = {
        1212, // RecoilAngle
        1250, // RecoilKickMax
        1252, // RecoilKickMin
        1223, // RecoilDir
        1210, // RecoilAlternateDir
        1213, // RecoilAngleADS
        1251, // RecoilKickMaxADS
        1253, // RecoilKickMinADS
        1224, // RecoilDirADS
        1211, // RecoilAlternateDirADS
        1274, // RecoilUseDifferentStatsInADS
    };

    void* VirtualFunction(void* object, std::size_t index)
    {
        if (!object)
            return nullptr;
        void** table = *reinterpret_cast<void***>(object);
        return table ? table[index] : nullptr;
    }

    void* ResolveGameInstanceOnMainTick()
    {
        HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
        const auto resolve = red4ext
                                 ? reinterpret_cast<std::uintptr_t (*)(std::uint32_t)>(
                                       GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                                 : nullptr;
        const std::uintptr_t engineAddress = resolve ? resolve(kGameEngineAddressHash) : 0;
        if (!engineAddress)
            return nullptr;
        void* engine = *reinterpret_cast<void**>(engineAddress);
        void* framework = engine ? *reinterpret_cast<void**>(static_cast<std::byte*>(engine) + 0x308) : nullptr;
        return framework ? *reinterpret_cast<void**>(static_cast<std::byte*>(framework) + 0x10) : nullptr;
    }

    void* GetSystemOnMainTick(void* gameInstance, std::uint64_t nameHash)
    {
        if (!gameInstance)
            return nullptr;
        HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
        const auto resolve = red4ext
                                 ? reinterpret_cast<std::uintptr_t (*)(std::uint32_t)>(
                                       GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                                 : nullptr;
        const std::uintptr_t rttiAddress = resolve ? resolve(kRttiSystemGetAddressHash) : 0;
        void* rtti = rttiAddress ? reinterpret_cast<void* (*)()>(rttiAddress)() : nullptr;
        const auto getClass = reinterpret_cast<void* (*)(void*, std::uint64_t)>(VirtualFunction(rtti, 2));
        const auto getSystem = reinterpret_cast<void* (*)(void*, void*)>(VirtualFunction(gameInstance, 1));
        void* type = getClass ? getClass(rtti, nameHash) : nullptr;
        return getSystem && type ? getSystem(gameInstance, type) : nullptr;
    }

    std::uint32_t AvailableSystems(const SystemContext& context)
    {
        std::uint32_t available = 0;
        if (context.playerSystem)
            available |= kPlayerSystem;
        if (context.statsSystem)
            available |= kStatsSystem;
        if (context.transactionSystem)
            available |= kTransactionSystem;
        return available;
    }

    bool AcquireSystemContextOnMainTick(std::uint32_t required, SystemContext& context)
    {
        context = {};
        __try
        {
            context.gameInstance = ResolveGameInstanceOnMainTick();
            if (context.gameInstance)
            {
                if ((required & kPlayerSystem) != 0)
                    context.playerSystem = GetSystemOnMainTick(
                        context.gameInstance, Game::Rtti::Hash("gameIPlayerSystem"));
                if ((required & kStatsSystem) != 0)
                {
                    context.statsSystem = GetSystemOnMainTick(
                        context.gameInstance, Game::Rtti::Hash("gameStatsSystem"));
                    if (!context.statsSystem)
                        context.statsSystem = GetSystemOnMainTick(
                            context.gameInstance, Game::Rtti::Hash("gameIStatsSystem"));
                }
                if ((required & kTransactionSystem) != 0)
                {
                    context.transactionSystem = GetSystemOnMainTick(
                        context.gameInstance, Game::Rtti::Hash("gameTransactionSystem"));
                    if (!context.transactionSystem)
                        context.transactionSystem = GetSystemOnMainTick(
                            context.gameInstance, Game::Rtti::Hash("gameITransactionSystem"));
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            context = {};
        }

        const std::uint32_t available = AvailableSystems(context);
        const bool complete = (available & required) == required;
        if (!complete)
        {
            ++g_runtime.systemAcquireFailures;
            const ULONGLONG now = GetTickCount64();
            if (g_runtime.lastSystemAcquireFailureLog == 0 ||
                now - g_runtime.lastSystemAcquireFailureLog >= 1000)
            {
                g_runtime.lastSystemAcquireFailureLog = now;
                Diagnostics::Log("no-recoil systems unavailable: required=0x%X available=0x%X failures=%llu",
                                 required, available,
                                 static_cast<unsigned long long>(g_runtime.systemAcquireFailures));
            }
        }
        return complete;
    }

    std::uint32_t Crc32(const char* text)
    {
        std::uint32_t value = 0xFFFFFFFFu;
        for (const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text); *cursor; ++cursor)
        {
            value ^= *cursor;
            for (unsigned bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ (0xEDB88320u & (0u - (value & 1u)));
        }
        return ~value;
    }

    bool ResolveRuntimeOnMainTick()
    {
        const ULONGLONG now = GetTickCount64();
        const bool baseResolved = g_runtime.getLocalPlayer && g_runtime.addModifier && g_runtime.removeModifier &&
                                  g_runtime.modifierDataClass && g_runtime.modifierDataSize == kModifierDataSize &&
                                  g_runtime.exactHandleOwnership;
        const bool weaponApiResolved = g_runtime.getItemInSlot != nullptr;
        if (baseResolved && weaponApiResolved)
        {
            g_runtimeAvailable.store(true, std::memory_order_release);
            return true;
        }
        if (g_runtime.attempted && now - g_runtime.lastResolveAttempt < 1000)
        {
            g_runtimeAvailable.store(baseResolved, std::memory_order_release);
            return baseResolved;
        }

        g_runtime.attempted = true;
        g_runtime.lastResolveAttempt = now;
        g_runtimeAvailable.store(false, std::memory_order_release);

        SystemContext systems;
        // The systems below are temporary resolver inputs only. They are discarded when this function returns;
        // only verified reflected metadata is retained in Runtime.
        AcquireSystemContextOnMainTick(kPlayerSystem | kStatsSystem | kTransactionSystem, systems);

        __try
        {
            if (systems.playerSystem)
            {
                Game::Rtti::Function* getLocalPlayer = Game::Rtti::FindFunction(
                    Game::Rtti::NativeType(systems.playerSystem),
                    Game::Rtti::Hash("GetLocalPlayerControlledGameObject"));
                if (getLocalPlayer && Game::Rtti::ParameterCount(getLocalPlayer) == 0)
                    g_runtime.getLocalPlayer = getLocalPlayer;
            }

            if (systems.statsSystem)
            {
                const Game::Rtti::Class* statsType = Game::Rtti::NativeType(systems.statsSystem);
                Game::Rtti::Function* addModifier = Game::Rtti::FindFunction(
                    statsType, Game::Rtti::Hash("AddModifier"));
                Game::Rtti::Function* removeModifier = Game::Rtti::FindFunction(
                    statsType, Game::Rtti::Hash("RemoveModifier"));
                if (addModifier && Game::Rtti::ParameterCount(addModifier) == 2)
                    g_runtime.addModifier = addModifier;
                if (removeModifier && Game::Rtti::ParameterCount(removeModifier) == 2)
                    g_runtime.removeModifier = removeModifier;
            }

            if (systems.transactionSystem)
            {
                const Game::Rtti::Class* transactionType = Game::Rtti::NativeType(systems.transactionSystem);
                Game::Rtti::Function* getItemInSlot = Game::Rtti::FindFunction(
                    transactionType, Game::Rtti::Hash("GetItemInSlot"));
                if (getItemInSlot && Game::Rtti::ParameterCount(getItemInSlot) == 2)
                    g_runtime.getItemInSlot = getItemInSlot;
            }

            Game::Rtti::Class* modifierDataClass = Game::Rtti::GetClass(
                Game::Rtti::Hash("gameConstantStatModifierData"));
            const std::size_t modifierDataSize = Game::Rtti::ClassSize(modifierDataClass);
            if (modifierDataClass && modifierDataSize == kModifierDataSize)
            {
                g_runtime.modifierDataClass = modifierDataClass;
                g_runtime.modifierDataSize = modifierDataSize;
            }
            g_runtime.exactHandleOwnership = Game::Rtti::HasExactHandleOwnership();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Preserve previously verified metadata. A world transition can make this tick's temporary systems
            // unavailable; dropping exact function/class metadata would turn a transient miss into a latch.
        }

        const bool resolved = g_runtime.getLocalPlayer && g_runtime.addModifier && g_runtime.removeModifier &&
                              g_runtime.modifierDataClass && g_runtime.modifierDataSize == kModifierDataSize &&
                              g_runtime.exactHandleOwnership;
        g_runtimeAvailable.store(resolved, std::memory_order_release);
        Diagnostics::Log("no-recoil resolver: getPlayer=%p add=%p remove=%p getItem=%p modifierClass=%p "
                         "size=0x%zX exactHandle=%d resolved=%d",
                         g_runtime.getLocalPlayer, g_runtime.addModifier, g_runtime.removeModifier,
                         g_runtime.getItemInSlot, g_runtime.modifierDataClass, g_runtime.modifierDataSize,
                         g_runtime.exactHandleOwnership ? 1 : 0, resolved ? 1 : 0);
        if (!resolved)
            g_failures.fetch_add(1, std::memory_order_relaxed);
        return resolved;
    }

    bool ReadEntityId(void* object, std::uint64_t& entityId)
    {
        entityId = 0;
        if (!object)
            return false;
        __try
        {
            std::memcpy(&entityId, static_cast<std::byte*>(object) + 0x48, sizeof(entityId));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            entityId = 0;
            return false;
        }
        return entityId != 0;
    }

    void ReleaseLocalHandle(Game::Rtti::Handle& handle)
    {
        // Invoke may populate only part of an out Handle before a transition fault. Exact release validates both
        // words before touching the refcount; a partial result is cleared locally without guessing a destructor.
        if (handle.instance || handle.refCount)
        {
            if (!Game::Rtti::ReleaseHandleExact(&handle))
                g_failures.fetch_add(1, std::memory_order_relaxed);
        }
        handle = {};
    }

    bool GetLocalPlayer(const SystemContext& systems, Game::Rtti::Handle& player, std::uint64_t& playerId)
    {
        player = {};
        playerId = 0;
        if (!g_runtime.getLocalPlayer || !systems.playerSystem)
            return false;

        bool invoked = false;
        __try
        {
            invoked = Game::Rtti::Invoke(g_runtime.getLocalPlayer, systems.playerSystem, nullptr, 0, &player);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            invoked = false;
        }

        bool hasInstance = false;
        __try
        {
            hasInstance = player.instance != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            hasInstance = false;
        }
        if (!invoked || !hasInstance)
        {
            ReleaseLocalHandle(player);
            return false;
        }

        bool valid = false;
        __try
        {
            valid = ReadEntityId(player.instance, playerId);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            playerId = 0;
            valid = false;
        }
        if (!valid)
            ReleaseLocalHandle(player);
        return valid;
    }

    EquippedWeaponResult GetEquippedWeaponId(const SystemContext& systems, const Game::Rtti::Handle& player,
                                              std::uint64_t& weaponId)
    {
        weaponId = 0;
        bool hasPlayerInstance = false;
        __try
        {
            hasPlayerInstance = player.instance != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            hasPlayerInstance = false;
        }
        if (!g_runtime.getItemInSlot || !hasPlayerInstance)
            return EquippedWeaponResult::ApiUnavailable;
        if (!systems.transactionSystem)
            return EquippedWeaponResult::CallFailed;
        TweakDbId slot;
        slot.hash = Crc32("AttachmentSlots.WeaponRight");
        slot.length = static_cast<std::uint8_t>(sizeof("AttachmentSlots.WeaponRight") - 1);
        Game::Rtti::Handle item{};
        Game::Rtti::Argument arguments[] = {{const_cast<Game::Rtti::Handle*>(&player)}, {&slot}};
        bool invoked = false;
        __try
        {
            invoked = Game::Rtti::Invoke(g_runtime.getItemInSlot, systems.transactionSystem,
                                         arguments, 2, &item);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            invoked = false;
        }
        if (!invoked)
        {
            ReleaseLocalHandle(item);
            return EquippedWeaponResult::CallFailed;
        }

        bool hasItemInstance = false;
        __try
        {
            hasItemInstance = item.instance != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            hasItemInstance = false;
        }
        if (!hasItemInstance)
        {
            ReleaseLocalHandle(item);
            return EquippedWeaponResult::NoItem;
        }

        bool valid = false;
        __try
        {
            valid = ReadEntityId(item.instance, weaponId);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            weaponId = 0;
            valid = false;
        }
        if (!valid)
        {
            ReleaseLocalHandle(item);
            return EquippedWeaponResult::InvalidItem;
        }
        ReleaseLocalHandle(item);
        return EquippedWeaponResult::Found;
    }

    bool ModifierCall(const SystemContext& systems, Game::Rtti::Function* function, std::uint64_t targetId,
                      ModifierEntry& modifier)
    {
        if (!function || !systems.statsSystem)
            return false;

        bool hasHandleInstance = false;
        __try
        {
            hasHandleInstance = modifier.handle.instance != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            hasHandleInstance = false;
        }
        if (!hasHandleInstance)
            return false;

        Game::Rtti::Argument arguments[] = {{&targetId}, {&modifier.handle}};
        bool result = false;
        bool hasReturnValue = false;
        __try
        {
            hasReturnValue = Game::Rtti::HasReturnValue(function);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        bool invoked = false;
        __try
        {
            invoked = Game::Rtti::Invoke(function, systems.statsSystem, arguments, 2,
                                         hasReturnValue ? &result : nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            invoked = false;
        }
        return invoked && (!hasReturnValue || result);
    }

    void ClearPublishedModifierState()
    {
        g_active.store(false, std::memory_order_release);
        g_playerId.store(0, std::memory_order_release);
        g_targetId.store(0, std::memory_order_release);
        g_weaponId.store(0, std::memory_order_release);
        g_usingWeaponTarget.store(false, std::memory_order_release);
        g_ownerGameInstance.store(0, std::memory_order_release);
        g_ownerStatsSystem.store(0, std::memory_order_release);
        g_ownerPlayerInstance.store(0, std::memory_order_release);
    }

    std::uintptr_t HandleInstanceIdentity(const Game::Rtti::Handle& handle)
    {
        __try
        {
            return reinterpret_cast<std::uintptr_t>(handle.instance);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    bool ModifierOwnerChanged(const SystemContext& systems, std::uintptr_t playerInstance)
    {
        const std::uintptr_t ownerGame = g_ownerGameInstance.load(std::memory_order_acquire);
        const std::uintptr_t ownerStats = g_ownerStatsSystem.load(std::memory_order_acquire);
        const std::uintptr_t ownerPlayer = g_ownerPlayerInstance.load(std::memory_order_acquire);
        const std::uintptr_t currentGame = reinterpret_cast<std::uintptr_t>(systems.gameInstance);
        const std::uintptr_t currentStats = reinterpret_cast<std::uintptr_t>(systems.statsSystem);

        return (ownerGame && currentGame && ownerGame != currentGame) ||
               (ownerStats && currentStats && ownerStats != currentStats) ||
               (ownerPlayer && playerInstance && ownerPlayer != playerInstance);
    }

    void AbandonRetiredOwner(const SystemContext& systems, std::uintptr_t playerInstance)
    {
        const std::uint64_t targetId = g_targetId.load(std::memory_order_relaxed);
        const std::uintptr_t ownerGame = g_ownerGameInstance.load(std::memory_order_relaxed);
        const std::uintptr_t ownerStats = g_ownerStatsSystem.load(std::memory_order_relaxed);
        const std::uintptr_t ownerPlayer = g_ownerPlayerInstance.load(std::memory_order_relaxed);
        std::size_t released = 0;
        for (ModifierEntry& modifier : g_modifiers)
        {
            if (!modifier.cleanupTracked)
                continue;
            ReleaseLocalHandle(modifier.handle);
            modifier = {};
            ++released;
        }
        ClearPublishedModifierState();
        g_retiredOwnerResets.fetch_add(1, std::memory_order_relaxed);
        Diagnostics::Log("no-recoil retired owner reset: targetId=0x%llX handles=%zu "
                         "old[game=%p stats=%p player=%p] new[game=%p stats=%p player=%p]",
                         static_cast<unsigned long long>(targetId), released,
                         reinterpret_cast<void*>(ownerGame), reinterpret_cast<void*>(ownerStats),
                         reinterpret_cast<void*>(ownerPlayer), systems.gameInstance, systems.statsSystem,
                         reinterpret_cast<void*>(playerInstance));
    }

    bool RemoveModifiers(const SystemContext& systems)
    {
        if (!g_runtime.removeModifier || !systems.statsSystem)
            return false;

        bool allRemoved = true;
        std::size_t activeCount = 0;
        std::size_t trackedCount = 0;
        std::uint64_t removalTarget = 0;
        for (ModifierEntry& modifier : g_modifiers)
        {
            if (!modifier.cleanupTracked)
                continue;
            ++trackedCount;
            if (modifier.active)
                ++activeCount;
            if (removalTarget == 0)
                removalTarget = modifier.targetId;
            const bool removed = ModifierCall(systems, g_runtime.removeModifier, modifier.targetId, modifier);
            if (!removed)
            {
                allRemoved = false;
                g_failures.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const bool wasActive = modifier.active;
            ReleaseLocalHandle(modifier.handle);
            modifier = {};
            if (wasActive)
                g_removed.fetch_add(1, std::memory_order_relaxed);
        }
        if (allRemoved)
        {
            if (trackedCount == kModifierCount && activeCount == kModifierCount)
            {
                Diagnostics::Log("no-recoil modifiers removed: targetId=0x%llX count=%zu",
                                 static_cast<unsigned long long>(removalTarget), activeCount);
            }
            ClearPublishedModifierState();
        }
        return allRemoved;
    }

    bool ApplyModifiers(const SystemContext& systems, std::uint64_t targetId, std::uintptr_t playerInstance)
    {
        if (!targetId || !systems.statsSystem || !g_runtime.addModifier || !g_runtime.modifierDataClass ||
            g_runtime.modifierDataSize != kModifierDataSize)
            return false;

        g_ownerGameInstance.store(reinterpret_cast<std::uintptr_t>(systems.gameInstance), std::memory_order_release);
        g_ownerStatsSystem.store(reinterpret_cast<std::uintptr_t>(systems.statsSystem), std::memory_order_release);
        g_ownerPlayerInstance.store(playerInstance, std::memory_order_release);

        std::size_t appliedCount = 0;
        for (std::size_t i = 0; i < g_modifiers.size(); ++i)
        {
            ModifierEntry& modifier = g_modifiers[i];
            modifier = {};
            modifier.statType = kRecoilStats[i];
            modifier.targetId = targetId;
            modifier.instance = Game::Rtti::CreateInstance(g_runtime.modifierDataClass);
            if (!modifier.instance)
                break;

            __try
            {
                auto* bytes = static_cast<std::byte*>(modifier.instance);
                *reinterpret_cast<std::int32_t*>(bytes + 0x40) = modifier.statType;
                *reinterpret_cast<std::int32_t*>(bytes + 0x44) = static_cast<std::int32_t>(kMultiplierModifierType);
                *reinterpret_cast<float*>(bytes + 0x48) = 0.0f;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                break;
            }

            const bool constructed = Game::Rtti::ConstructHandle(&modifier.handle, modifier.instance);
            if (!constructed)
            {
                g_failures.fetch_add(1, std::memory_order_relaxed);
                // ConstructInstance has no verified destruction ABI in this project. Keep this raw allocation
                // uncertainty explicit; do not invent a destructor or release a possibly partial Handle.
                Diagnostics::Log("no-recoil handle construction failed: statType=%d targetId=0x%llX; "
                                 "raw instance ownership unresolved",
                                 modifier.statType, static_cast<unsigned long long>(targetId));
                break;
            }

            // From this point the exact Handle must remain cleanup-tracked even when AddModifier throws or returns
            // false: the engine may already have retained it. Safe unload is gated by g_active until removal is
            // acknowledged, and failed removal leaves this entry intact for the next main tick.
            modifier.cleanupTracked = true;
            g_active.store(true, std::memory_order_release);
            if (!ModifierCall(systems, g_runtime.addModifier, targetId, modifier))
            {
                g_failures.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            modifier.active = true;
            ++appliedCount;
            g_applied.fetch_add(1, std::memory_order_relaxed);
        }

        if (appliedCount != g_modifiers.size())
        {
            // Remove every successfully constructed Handle, including a handle whose AddModifier call failed or
            // threw. If removal fails, keep it alive and let the next main tick retry; releasing it here could race
            // an internal StatsSystem reference.
            RemoveModifiers(systems);
            return false;
        }
        g_active.store(true, std::memory_order_release);
        g_targetId.store(targetId, std::memory_order_release);
        Diagnostics::Log("no-recoil modifiers applied: targetId=0x%llX count=%zu",
                         static_cast<unsigned long long>(targetId), g_modifiers.size());
        return true;
    }

    bool LogTargetDecision(std::uint64_t playerId, std::uint64_t weaponId, bool usingWeapon,
                           EquippedWeaponResult weaponResult = EquippedWeaponResult::Found)
    {
        const ULONGLONG now = GetTickCount64();
        if (now - g_lastPathLogTick < 3000)
            return false;
        g_lastPathLogTick = now;
        const char* path = usingWeapon ? "equipped-weapon" : "player-fallback";
        if (weaponResult == EquippedWeaponResult::NoItem)
            path = "waiting-for-equipped-weapon";
        else if (weaponResult == EquippedWeaponResult::InvalidItem)
            path = "waiting-for-valid-equipped-weapon";
        else if (weaponResult == EquippedWeaponResult::CallFailed)
            path = "waiting-for-weapon-call";
        else if (weaponResult == EquippedWeaponResult::ApiUnavailable)
            path = "player-fallback-api-unavailable";
        Diagnostics::Log("no-recoil target: path=%s playerId=0x%llX weaponId=0x%llX targetId=0x%llX "
                         "runtimeValidationRisk=%d",
                         path,
                         static_cast<unsigned long long>(playerId),
                         static_cast<unsigned long long>(weaponId),
                         static_cast<unsigned long long>(usingWeapon ? weaponId : playerId),
                         usingWeapon || weaponResult != EquippedWeaponResult::ApiUnavailable ? 0 : 1);
        return true;
    }
}

namespace Game::PlayerModifiers
{
    void PublishDesired(const Features::MiscSettings& misc)
    {
        const bool enabled = misc.noRecoil;
        // Once unload cleanup starts it owns the desired state. Present/headless publication must not continually
        // flip the atomic back to true while the unload worker is waiting for its main-tick acknowledgement.
        if (enabled && g_cleanupRequested.load(std::memory_order_acquire))
            return;
        const bool previous = g_desired.exchange(enabled, std::memory_order_acq_rel);
        if (previous != enabled)
            Diagnostics::Log("no-recoil desired settings published: enabled=%d", enabled ? 1 : 0);
    }

    void OnGameMainTick()
    {
        const bool requested = g_cleanupRequested.load(std::memory_order_acquire)
                                   ? false
                                   : g_desired.load(std::memory_order_acquire);
        bool currentlyActive = g_active.load(std::memory_order_acquire);
        if (!requested && !currentlyActive)
        {
            if (g_cleanupRequested.load(std::memory_order_acquire))
                g_cleanupAcknowledged.store(true, std::memory_order_release);
            return;
        }

        if (!ResolveRuntimeOnMainTick())
        {
            if (!requested && currentlyActive)
                g_cleanupAcknowledged.store(false, std::memory_order_release);
            return;
        }

        if (!requested)
        {
            SystemContext systems;
            AcquireSystemContextOnMainTick(kPlayerSystem | kStatsSystem, systems);
            Game::Rtti::Handle player;
            std::uint64_t playerId = 0;
            const bool hasPlayer = GetLocalPlayer(systems, player, playerId);
            const std::uintptr_t playerInstance = hasPlayer ? HandleInstanceIdentity(player) : 0;
            if (ModifierOwnerChanged(systems, playerInstance))
                AbandonRetiredOwner(systems, playerInstance);
            ReleaseLocalHandle(player);
            if (!g_active.load(std::memory_order_acquire))
            {
                if (g_cleanupRequested.load(std::memory_order_acquire))
                    g_cleanupAcknowledged.store(true, std::memory_order_release);
                return;
            }
            if (RemoveModifiers(systems) && g_cleanupRequested.load(std::memory_order_acquire))
                g_cleanupAcknowledged.store(true, std::memory_order_release);
            return;
        }

        SystemContext systems;
        const std::uint32_t requiredSystems = kPlayerSystem | kStatsSystem |
                                               (g_runtime.getItemInSlot ? kTransactionSystem : 0u);
        AcquireSystemContextOnMainTick(requiredSystems, systems);

        Game::Rtti::Handle player;
        std::uint64_t playerId = 0;
        if (!GetLocalPlayer(systems, player, playerId))
        {
            if (currentlyActive)
                RemoveModifiers(systems);
            g_failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const std::uintptr_t playerInstance = HandleInstanceIdentity(player);
        if (currentlyActive && ModifierOwnerChanged(systems, playerInstance))
        {
            AbandonRetiredOwner(systems, playerInstance);
            currentlyActive = false;
        }

        std::uint64_t weaponId = 0;
        const EquippedWeaponResult weaponResult = GetEquippedWeaponId(systems, player, weaponId);
        const bool usingWeapon = weaponResult == EquippedWeaponResult::Found;
        if (weaponResult != EquippedWeaponResult::Found && weaponResult != EquippedWeaponResult::ApiUnavailable)
        {
            // A resolved TransactionSystem/GetItemInSlot path is authoritative. An empty or failed slot lookup is
            // not evidence that the player is a valid StatsSystem target: remove any old weapon modifiers and wait
            // for a valid right-hand weapon instead of silently applying recoil modifiers to the player entity.
            LogTargetDecision(playerId, weaponId, false, weaponResult);
            ReleaseLocalHandle(player);
            if (currentlyActive && !RemoveModifiers(systems))
                return;
            return;
        }

        // Player fallback is intentionally restricted to the case where the transaction API itself could not be
        // resolved. Once GetItemInSlot is available, the branch above waits for an equipped weapon.
        const std::uint64_t targetId = usingWeapon ? weaponId : playerId;
        LogTargetDecision(playerId, weaponId, usingWeapon, weaponResult);
        ReleaseLocalHandle(player);

        const std::uint64_t currentTarget = g_targetId.load(std::memory_order_acquire);
        const std::uint64_t currentPlayer = g_playerId.load(std::memory_order_acquire);
        const bool currentPathWeapon = g_usingWeaponTarget.load(std::memory_order_acquire);
        if (currentlyActive && (currentPlayer != playerId || currentTarget != targetId ||
                                currentPathWeapon != usingWeapon))
        {
            if (!RemoveModifiers(systems))
                return;
        }
        if (!g_active.load(std::memory_order_acquire))
        {
            if (ApplyModifiers(systems, targetId, playerInstance))
            {
                g_playerId.store(playerId, std::memory_order_release);
                g_weaponId.store(weaponId, std::memory_order_release);
                g_usingWeaponTarget.store(usingWeapon, std::memory_order_release);
            }
        }
    }

    bool PrepareForShutdown(std::uint32_t timeoutMilliseconds)
    {
        g_desired.store(false, std::memory_order_release);
        if (!g_active.load(std::memory_order_acquire))
            return true;

        g_cleanupRequested.store(true, std::memory_order_release);
        g_cleanupAcknowledged.store(false, std::memory_order_release);
        const ULONGLONG deadline = GetTickCount64() + timeoutMilliseconds;
        while (!g_cleanupAcknowledged.load(std::memory_order_acquire))
        {
            if (GetTickCount64() >= deadline)
            {
                Diagnostics::Log("no-recoil cleanup timed out: targetId=0x%llX",
                                 static_cast<unsigned long long>(g_targetId.load(std::memory_order_relaxed)));
                return false;
            }
            Sleep(1);
        }
        Diagnostics::Log("no-recoil cleanup acknowledged");
        return true;
    }

    void Shutdown()
    {
        g_desired.store(false, std::memory_order_release);
        g_cleanupRequested.store(false, std::memory_order_release);
        g_cleanupAcknowledged.store(false, std::memory_order_release);
        g_runtime = {};
        g_modifiers = {};
        g_active.store(false, std::memory_order_release);
        g_playerId.store(0, std::memory_order_release);
        g_targetId.store(0, std::memory_order_release);
        g_weaponId.store(0, std::memory_order_release);
        g_usingWeaponTarget.store(false, std::memory_order_release);
        g_ownerGameInstance.store(0, std::memory_order_release);
        g_ownerStatsSystem.store(0, std::memory_order_release);
        g_ownerPlayerInstance.store(0, std::memory_order_release);
        g_runtimeAvailable.store(false, std::memory_order_release);
    }

    Stats GetStats()
    {
        Stats result;
        result.available = g_runtimeAvailable.load(std::memory_order_acquire);
        result.active = g_active.load(std::memory_order_acquire);
        result.targetId = g_targetId.load(std::memory_order_relaxed);
        result.weaponId = g_weaponId.load(std::memory_order_relaxed);
        result.usingWeaponTarget = g_usingWeaponTarget.load(std::memory_order_relaxed);
        result.applied = g_applied.load(std::memory_order_relaxed);
        result.removed = g_removed.load(std::memory_order_relaxed);
        result.retiredOwnerResets = g_retiredOwnerResets.load(std::memory_order_relaxed);
        result.failures = g_failures.load(std::memory_order_relaxed);
        return result;
    }
}
