#include "player_modifiers.h"

#include "entity_tracker.h"
#include "rtti_invoker.h"
#include "../diagnostics.h"
#include "../features/features.h"
#include "../framework.h"

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
    constexpr std::size_t kRecoilModifierCount = 11;
    constexpr std::size_t kSpreadModifierCount = 16;
    constexpr std::size_t kModifierCount = kRecoilModifierCount + kSpreadModifierCount;
    constexpr std::uint32_t kNoRecoilMask = 1u << 0;
    constexpr std::uint32_t kNoSpreadMask = 1u << 1;
    constexpr ULONGLONG kWorldGateFallbackMilliseconds = 1000;
    constexpr ULONGLONG kWeaponLookupGraceMilliseconds = 250;

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
    std::atomic_uint32_t g_desiredModifierMask{0};
    std::atomic_bool g_cleanupRequested{false};
    std::atomic_bool g_cleanupAcknowledged{false};
    std::atomic_uint64_t g_playerId{0};
    std::atomic_uint64_t g_targetId{0};
    std::atomic_uint64_t g_weaponId{0};
    std::atomic_bool g_active{false};
    std::atomic_uint32_t g_activeModifierMask{0};
    std::atomic_bool g_worldRetirementPending{false};
    // Dump-visible, logging-independent stage marker. A hang inside a reflected call leaves the last stage intact.
    enum class MainTickStage : std::uint32_t
    {
        Idle,
        WorldGate,
        ReleaseRetiredHandles,
        ResolveRuntime,
        AcquireSystems,
        GetPlayer,
        ProcessModifiers,
        ReleasePlayer,
    };
    std::atomic_uint32_t g_mainTickStage{static_cast<std::uint32_t>(MainTickStage::Idle)};
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
    ULONGLONG g_worldGateBlockedSince = 0;
    ULONGLONG g_weaponLookupMissSince = 0;
    bool g_worldGateFallbackActive = false;

    void SetMainTickStage(MainTickStage stage)
    {
        g_mainTickStage.store(static_cast<std::uint32_t>(stage), std::memory_order_release);
    }

    constexpr std::array<std::int32_t, kRecoilModifierCount> kRecoilStats = {
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

    // Spread values are gamedataStatType enum entries from the 2.31 enum dump. The current local RTTI bridge
    // exposes class/function metadata but no enum-member lookup, so these are deliberately kept with their names
    // beside the values instead of being treated as patch-independent offsets. The mapping is also the one used by
    // the REDmodding base-stat reference (wiki.redmodding.org): default/min/max/change-per-shot for both hip-fire and ADS. Runtime class and
    // function validation still gates all writes; in-game 2.31 validation remains required.
    constexpr std::array<std::int32_t, kSpreadModifierCount> kSpreadStats = {
        1432, // Spread
        1435, // SpreadAdsDefaultX
        1436, // SpreadAdsDefaultY
        1442, // SpreadAdsMaxX
        1443, // SpreadAdsMaxY
        1444, // SpreadAdsMinX
        1445, // SpreadAdsMinY
        1433, // SpreadAdsChangePerShot
        1451, // SpreadDefaultX
        1452, // SpreadDefaultY
        1461, // SpreadMaxX
        1462, // SpreadMaxY
        1463, // SpreadMinX
        1464, // SpreadMinY
        1447, // SpreadChangePerShot
        1465, // SpreadPenalty
    };

    constexpr const char* kSpreadStatNames[kSpreadModifierCount] = {
        "Spread", "SpreadAdsDefaultX", "SpreadAdsDefaultY", "SpreadAdsMaxX", "SpreadAdsMaxY",
        "SpreadAdsMinX", "SpreadAdsMinY", "SpreadAdsChangePerShot", "SpreadDefaultX", "SpreadDefaultY",
        "SpreadMaxX", "SpreadMaxY", "SpreadMinX", "SpreadMinY", "SpreadChangePerShot", "SpreadPenalty",
    };

    std::int32_t ModifierStatType(std::size_t index)
    {
        if (index < kRecoilModifierCount)
            return kRecoilStats[index];
        return kSpreadStats[index - kRecoilModifierCount];
    }

    std::uint32_t ModifierFeatureMask(std::size_t index)
    {
        return index < kRecoilModifierCount ? kNoRecoilMask : kNoSpreadMask;
    }

    const char* ModifierStatName(std::size_t index)
    {
        if (index < kRecoilModifierCount)
            return "recoil";
        return kSpreadStatNames[index - kRecoilModifierCount];
    }

    std::size_t DesiredModifierCount(std::uint32_t mask)
    {
        std::size_t count = 0;
        for (std::size_t i = 0; i < kModifierCount; ++i)
        {
            if ((ModifierFeatureMask(i) & mask) != 0)
                ++count;
        }
        return count;
    }

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
                Diagnostics::Log("player modifier systems unavailable: required=0x%X available=0x%X failures=%llu",
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
        const bool playerResolved = g_runtime.getLocalPlayer != nullptr;
        const bool modifierResolved = playerResolved && g_runtime.addModifier && g_runtime.removeModifier &&
                                      g_runtime.modifierDataClass && g_runtime.modifierDataSize == kModifierDataSize &&
                                      g_runtime.exactHandleOwnership;
        if (modifierResolved)
        {
            g_runtimeAvailable.store(modifierResolved, std::memory_order_release);
            return true;
        }
        if (g_runtime.attempted && now - g_runtime.lastResolveAttempt < 1000)
        {
            g_runtimeAvailable.store(modifierResolved, std::memory_order_release);
            return modifierResolved;
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

        const bool playerResolvedAfterAttempt = g_runtime.getLocalPlayer != nullptr;
        const bool modifierResolvedAfterAttempt = playerResolvedAfterAttempt && g_runtime.addModifier &&
                                                  g_runtime.removeModifier && g_runtime.modifierDataClass &&
                                                  g_runtime.modifierDataSize == kModifierDataSize &&
                                                  g_runtime.exactHandleOwnership;
        g_runtimeAvailable.store(modifierResolvedAfterAttempt, std::memory_order_release);
        Diagnostics::Log("player modifier resolver: getPlayer=%p add=%p remove=%p getItem=%p modifierClass=%p "
                         "size=0x%zX exactHandle=%d resolved=%d",
                         g_runtime.getLocalPlayer, g_runtime.addModifier, g_runtime.removeModifier,
                         g_runtime.getItemInSlot, g_runtime.modifierDataClass, g_runtime.modifierDataSize,
                         g_runtime.exactHandleOwnership ? 1 : 0, modifierResolvedAfterAttempt ? 1 : 0);
        if (!modifierResolvedAfterAttempt)
            g_failures.fetch_add(1, std::memory_order_relaxed);
        return modifierResolvedAfterAttempt;
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
        g_activeModifierMask.store(0, std::memory_order_release);
        g_playerId.store(0, std::memory_order_release);
        g_targetId.store(0, std::memory_order_release);
        g_weaponId.store(0, std::memory_order_release);
        g_usingWeaponTarget.store(false, std::memory_order_release);
        g_ownerGameInstance.store(0, std::memory_order_release);
        g_ownerStatsSystem.store(0, std::memory_order_release);
        g_ownerPlayerInstance.store(0, std::memory_order_release);
    }

    void RetireModifiersForWorldTransition()
    {
        const std::uint64_t targetId = g_targetId.load(std::memory_order_relaxed);
        std::size_t tracked = 0;
        for (ModifierEntry& modifier : g_modifiers)
        {
            if (!modifier.cleanupTracked)
                continue;
            // Do not call RemoveModifier or touch a refcount while the old world is draining. The local strong
            // handles are released only after the replacement world has passed the shared settle gate.
            modifier.active = false;
            ++tracked;
        }
        ClearPublishedModifierState();
        g_worldRetirementPending.store(tracked != 0, std::memory_order_release);
        if (tracked != 0)
        {
            g_retiredOwnerResets.fetch_add(1, std::memory_order_relaxed);
            Diagnostics::Log("player modifiers retired by world gate: targetId=0x%llX handles=%zu",
                             static_cast<unsigned long long>(targetId), tracked);
        }
    }

    void ReleaseWorldRetiredHandlesAfterSettle()
    {
        if (!g_worldRetirementPending.load(std::memory_order_acquire))
            return;

        std::size_t released = 0;
        for (ModifierEntry& modifier : g_modifiers)
        {
            if (!modifier.cleanupTracked)
                continue;
            ReleaseLocalHandle(modifier.handle);
            modifier = {};
            ++released;
        }
        g_worldRetirementPending.store(false, std::memory_order_release);
        Diagnostics::Log("player modifier retired handles released after world settle: count=%zu", released);
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
        Diagnostics::Log("player modifiers retired owner reset: targetId=0x%llX handles=%zu "
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
            if (trackedCount != 0)
            {
                Diagnostics::Log("player modifiers removed: targetId=0x%llX count=%zu",
                                 static_cast<unsigned long long>(removalTarget), activeCount);
            }
            ClearPublishedModifierState();
        }
        return allRemoved;
    }

    bool ApplyModifiers(const SystemContext& systems, std::uint64_t targetId, std::uintptr_t playerInstance,
                        std::uint32_t desiredMask)
    {
        const std::size_t desiredCount = DesiredModifierCount(desiredMask);
        if (!targetId || desiredCount == 0 || !systems.statsSystem || !g_runtime.addModifier ||
            !g_runtime.modifierDataClass || g_runtime.modifierDataSize != kModifierDataSize)
            return false;

        g_ownerGameInstance.store(reinterpret_cast<std::uintptr_t>(systems.gameInstance), std::memory_order_release);
        g_ownerStatsSystem.store(reinterpret_cast<std::uintptr_t>(systems.statsSystem), std::memory_order_release);
        g_ownerPlayerInstance.store(playerInstance, std::memory_order_release);

        std::size_t appliedCount = 0;
        for (std::size_t i = 0; i < g_modifiers.size(); ++i)
        {
            ModifierEntry& modifier = g_modifiers[i];
            modifier = {};
            if ((ModifierFeatureMask(i) & desiredMask) == 0)
                continue;
            modifier.statType = ModifierStatType(i);
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
                Diagnostics::Log("player modifier handle construction failed: stat=%s(%d) targetId=0x%llX; "
                                 "raw instance ownership unresolved",
                                 ModifierStatName(i), modifier.statType,
                                 static_cast<unsigned long long>(targetId));
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

        if (appliedCount != desiredCount)
        {
            // Remove every successfully constructed Handle, including a handle whose AddModifier call failed or
            // threw. If removal fails, keep it alive and let the next main tick retry; releasing it here could race
            // an internal StatsSystem reference.
            RemoveModifiers(systems);
            return false;
        }
        g_active.store(true, std::memory_order_release);
        g_activeModifierMask.store(desiredMask, std::memory_order_release);
        g_targetId.store(targetId, std::memory_order_release);
        Diagnostics::Log("player modifiers applied: targetId=0x%llX count=%zu mask=0x%X",
                         static_cast<unsigned long long>(targetId), desiredCount, desiredMask);
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
            path = "player-fallback-no-item";
        else if (weaponResult == EquippedWeaponResult::InvalidItem)
            path = "player-fallback-invalid-item";
        else if (weaponResult == EquippedWeaponResult::CallFailed)
            path = "player-fallback-weapon-call-failed";
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

    bool ProcessModifierState(const SystemContext& systems, const Game::Rtti::Handle& player,
                              std::uint64_t playerId, bool hasPlayer, std::uint32_t desiredMask)
    {
        const std::uintptr_t playerInstance = hasPlayer ? HandleInstanceIdentity(player) : 0;
        bool currentlyActive = g_active.load(std::memory_order_acquire);
        if (desiredMask == 0)
        {
            if (!currentlyActive)
                return true;
            if (ModifierOwnerChanged(systems, playerInstance))
            {
                AbandonRetiredOwner(systems, playerInstance);
                return true;
            }
            return RemoveModifiers(systems);
        }

        if (!hasPlayer)
        {
            if (currentlyActive)
            {
                if (ModifierOwnerChanged(systems, 0))
                {
                    AbandonRetiredOwner(systems, 0);
                    return true;
                }
                return RemoveModifiers(systems);
            }
            return false;
        }

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
            // GetItemInSlot can return an empty handle indefinitely after a late injection even while the player
            // has a weapon equipped. Waiting forever made both modifier features silently unavailable. Preserve a
            // short transition grace, then use the verified player entity as the stat target; the next successful
            // weapon lookup migrates the modifiers back to the weapon target through the normal remove/apply path.
            const ULONGLONG now = GetTickCount64();
            if (g_weaponLookupMissSince == 0)
                g_weaponLookupMissSince = now;
            if (now - g_weaponLookupMissSince < kWeaponLookupGraceMilliseconds)
                return currentlyActive;
        }
        else
        {
            g_weaponLookupMissSince = 0;
        }

        const std::uint64_t targetId = usingWeapon ? weaponId : playerId;
        LogTargetDecision(playerId, weaponId, usingWeapon, weaponResult);
        const std::uint64_t currentTarget = g_targetId.load(std::memory_order_acquire);
        const std::uint64_t currentPlayer = g_playerId.load(std::memory_order_acquire);
        const bool currentPathWeapon = g_usingWeaponTarget.load(std::memory_order_acquire);
        const std::uint32_t currentMask = g_activeModifierMask.load(std::memory_order_acquire);
        if (currentlyActive && (currentPlayer != playerId || currentTarget != targetId ||
                                currentPathWeapon != usingWeapon || currentMask != desiredMask))
        {
            if (!RemoveModifiers(systems))
                return false;
        }
        if (!g_active.load(std::memory_order_acquire) &&
            ApplyModifiers(systems, targetId, playerInstance, desiredMask))
        {
            g_playerId.store(playerId, std::memory_order_release);
            g_weaponId.store(weaponId, std::memory_order_release);
            g_usingWeaponTarget.store(usingWeapon, std::memory_order_release);
        }
        return g_active.load(std::memory_order_acquire);
    }
}

namespace Game::PlayerModifiers
{
    void PublishDesired(const Features::MiscSettings& misc)
    {
        const std::uint32_t modifierMask = (misc.noRecoil ? kNoRecoilMask : 0u) |
                                           (misc.noSpread ? kNoSpreadMask : 0u);
        // Once unload cleanup starts it owns the desired state. Present/headless publication must not continually
        // re-enable modifiers while the unload worker waits for its main-tick acknowledgement.
        if (modifierMask != 0 && g_cleanupRequested.load(std::memory_order_acquire))
            return;
        const std::uint32_t previousMask =
            g_desiredModifierMask.exchange(modifierMask, std::memory_order_acq_rel);
        if (previousMask != modifierMask)
            Diagnostics::Log("player modifier settings published: mask=0x%X", modifierMask);
    }

    void OnGameMainTick()
    {
        SetMainTickStage(MainTickStage::WorldGate);
        const bool cleanupRequested = g_cleanupRequested.load(std::memory_order_acquire);
        const std::uint32_t desiredModifierMask = cleanupRequested
                                                       ? 0u
                                                       : g_desiredModifierMask.load(std::memory_order_acquire);
        const bool modifierActive = g_active.load(std::memory_order_acquire);
        const bool retirementPending = g_worldRetirementPending.load(std::memory_order_acquire);
        const bool modifierPathNeeded = desiredModifierMask != 0 || modifierActive || retirementPending;
        if (!modifierPathNeeded)
        {
            if (cleanupRequested)
                g_cleanupAcknowledged.store(true, std::memory_order_release);
            SetMainTickStage(MainTickStage::Idle);
            return;
        }

        // EntityTracker runs first in the same detour and publishes this bit without entering an engine system.
        // A late injection cannot see NPCs that were registered before the hook existed, however, so tracked=0 is
        // ambiguous: it can mean either a draining world or a perfectly live already-loaded save. Keep the bounded
        // transition guard, then fall back to the player/system validation below instead of latching both features
        // off forever. Active old-world state is retired before the fallback timer starts.
        const bool sharedWorldReady = Game::EntityTracker::IsWorldReadyForMainTickConsumers();
        if (sharedWorldReady)
        {
            g_worldGateBlockedSince = 0;
            g_worldGateFallbackActive = false;
        }
        else if (!g_worldGateFallbackActive)
        {
            if (modifierActive)
                RetireModifiersForWorldTransition();
            const ULONGLONG now = GetTickCount64();
            if (g_worldGateBlockedSince == 0)
                g_worldGateBlockedSince = now;
            if (now - g_worldGateBlockedSince >= kWorldGateFallbackMilliseconds)
            {
                g_worldGateFallbackActive = true;
                Diagnostics::Log("player modifier world gate fallback: tracked world unavailable for %llums; "
                                 "using current player/system validation",
                                 static_cast<unsigned long long>(kWorldGateFallbackMilliseconds));
            }
        }
        if (!sharedWorldReady && !g_worldGateFallbackActive)
        {
            if (cleanupRequested)
                g_cleanupAcknowledged.store(false, std::memory_order_release);
            SetMainTickStage(MainTickStage::Idle);
            return;
        }

        SetMainTickStage(MainTickStage::ReleaseRetiredHandles);
        ReleaseWorldRetiredHandlesAfterSettle();
        if (cleanupRequested && !g_active.load(std::memory_order_acquire))
        {
            g_cleanupAcknowledged.store(true, std::memory_order_release);
            SetMainTickStage(MainTickStage::Idle);
            return;
        }

        SetMainTickStage(MainTickStage::ResolveRuntime);
        if (!ResolveRuntimeOnMainTick())
        {
            if (cleanupRequested)
                g_cleanupAcknowledged.store(false, std::memory_order_release);
            SetMainTickStage(MainTickStage::Idle);
            return;
        }

        std::uint32_t requiredSystems = kPlayerSystem | kStatsSystem;
        if (g_runtime.getItemInSlot)
            requiredSystems |= kTransactionSystem;

        SetMainTickStage(MainTickStage::AcquireSystems);
        SystemContext systems;
        AcquireSystemContextOnMainTick(requiredSystems, systems);

        SetMainTickStage(MainTickStage::GetPlayer);
        Game::Rtti::Handle player;
        std::uint64_t playerId = 0;
        const bool hasPlayer = GetLocalPlayer(systems, player, playerId);

        SetMainTickStage(MainTickStage::ProcessModifiers);
        ProcessModifierState(systems, player, playerId, hasPlayer, desiredModifierMask);

        SetMainTickStage(MainTickStage::ReleasePlayer);
        ReleaseLocalHandle(player);

        if (cleanupRequested)
        {
            const bool allClean = !g_active.load(std::memory_order_acquire) &&
                                  !g_worldRetirementPending.load(std::memory_order_acquire);
            g_cleanupAcknowledged.store(allClean, std::memory_order_release);
        }
        SetMainTickStage(MainTickStage::Idle);
    }

    bool PrepareForShutdown(std::uint32_t timeoutMilliseconds)
    {
        g_desiredModifierMask.store(0, std::memory_order_release);
        if (!g_active.load(std::memory_order_acquire) &&
            !g_worldRetirementPending.load(std::memory_order_acquire))
            return true;

        g_cleanupRequested.store(true, std::memory_order_release);
        g_cleanupAcknowledged.store(false, std::memory_order_release);
        const ULONGLONG deadline = GetTickCount64() + timeoutMilliseconds;
        while (!g_cleanupAcknowledged.load(std::memory_order_acquire))
        {
            if (GetTickCount64() >= deadline)
            {
                Diagnostics::Log("player modifier cleanup timed out: active=%d retired=%d targetId=0x%llX stage=%u",
                                 g_active.load(std::memory_order_relaxed) ? 1 : 0,
                                 g_worldRetirementPending.load(std::memory_order_relaxed) ? 1 : 0,
                                 static_cast<unsigned long long>(g_targetId.load(std::memory_order_relaxed)),
                                 g_mainTickStage.load(std::memory_order_relaxed));
                return false;
            }
            Sleep(1);
        }
        Diagnostics::Log("player modifier cleanup acknowledged");
        return true;
    }

    void Shutdown()
    {
        g_desiredModifierMask.store(0, std::memory_order_release);
        g_cleanupRequested.store(false, std::memory_order_release);
        g_cleanupAcknowledged.store(false, std::memory_order_release);
        g_runtime = {};
        g_modifiers = {};
        g_active.store(false, std::memory_order_release);
        g_activeModifierMask.store(0, std::memory_order_release);
        g_worldRetirementPending.store(false, std::memory_order_release);
        g_mainTickStage.store(static_cast<std::uint32_t>(MainTickStage::Idle), std::memory_order_release);
        g_playerId.store(0, std::memory_order_release);
        g_targetId.store(0, std::memory_order_release);
        g_weaponId.store(0, std::memory_order_release);
        g_usingWeaponTarget.store(false, std::memory_order_release);
        g_ownerGameInstance.store(0, std::memory_order_release);
        g_ownerStatsSystem.store(0, std::memory_order_release);
        g_ownerPlayerInstance.store(0, std::memory_order_release);
        g_worldGateBlockedSince = 0;
        g_weaponLookupMissSince = 0;
        g_worldGateFallbackActive = false;
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
