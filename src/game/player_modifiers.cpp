#include "player_modifiers.h"

#include "rtti_invoker.h"
#include "../diagnostics.h"
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

    struct Runtime
    {
        bool attempted = false;
        ULONGLONG lastResolveAttempt = 0;
        void* gameInstance = nullptr;
        void* playerSystem = nullptr;
        void* statsSystem = nullptr;
        void* transactionSystem = nullptr;
        Game::Rtti::Function* getLocalPlayer = nullptr;
        Game::Rtti::Function* addModifier = nullptr;
        Game::Rtti::Function* removeModifier = nullptr;
        Game::Rtti::Function* getItemInSlot = nullptr;
        Game::Rtti::Class* modifierDataClass = nullptr;
        std::size_t modifierDataSize = 0;
        bool logged = false;
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
    std::atomic_uint64_t g_failures{0};
    std::atomic_bool g_usingWeaponTarget{false};
    std::atomic_bool g_runtimeAvailable{false};
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
        const bool baseResolved = g_runtime.statsSystem && g_runtime.playerSystem && g_runtime.getLocalPlayer &&
                                  g_runtime.addModifier && g_runtime.removeModifier && g_runtime.modifierDataClass &&
                                  g_runtime.modifierDataSize == kModifierDataSize;
        const bool weaponApiResolved = g_runtime.transactionSystem && g_runtime.getItemInSlot;
        if (g_runtime.attempted && baseResolved && weaponApiResolved)
            return true;
        if (g_runtime.attempted && now - g_runtime.lastResolveAttempt < 1000)
            return baseResolved;

        g_runtime.attempted = true;
        g_runtime.lastResolveAttempt = now;
        g_runtime.gameInstance = nullptr;
        g_runtime.playerSystem = nullptr;
        g_runtime.statsSystem = nullptr;
        g_runtime.transactionSystem = nullptr;
        g_runtime.getLocalPlayer = nullptr;
        g_runtime.addModifier = nullptr;
        g_runtime.removeModifier = nullptr;
        g_runtime.getItemInSlot = nullptr;
        g_runtime.modifierDataClass = nullptr;
        g_runtime.modifierDataSize = 0;
        g_runtimeAvailable.store(false, std::memory_order_release);

        __try
        {
            g_runtime.gameInstance = ResolveGameInstanceOnMainTick();
            g_runtime.playerSystem = GetSystemOnMainTick(
                g_runtime.gameInstance, Game::Rtti::Hash("gameIPlayerSystem"));
            g_runtime.statsSystem = GetSystemOnMainTick(
                g_runtime.gameInstance, Game::Rtti::Hash("gameStatsSystem"));
            if (!g_runtime.statsSystem)
                g_runtime.statsSystem = GetSystemOnMainTick(
                    g_runtime.gameInstance, Game::Rtti::Hash("gameIStatsSystem"));
            g_runtime.transactionSystem = GetSystemOnMainTick(
                g_runtime.gameInstance, Game::Rtti::Hash("gameTransactionSystem"));
            if (!g_runtime.transactionSystem)
                g_runtime.transactionSystem = GetSystemOnMainTick(
                    g_runtime.gameInstance, Game::Rtti::Hash("gameITransactionSystem"));

            g_runtime.getLocalPlayer = Game::Rtti::FindFunction(
                Game::Rtti::NativeType(g_runtime.playerSystem),
                Game::Rtti::Hash("GetLocalPlayerControlledGameObject"));
            const Game::Rtti::Class* statsType = Game::Rtti::NativeType(g_runtime.statsSystem);
            g_runtime.addModifier = Game::Rtti::FindFunction(statsType, Game::Rtti::Hash("AddModifier"));
            g_runtime.removeModifier = Game::Rtti::FindFunction(statsType, Game::Rtti::Hash("RemoveModifier"));
            if (g_runtime.addModifier && Game::Rtti::ParameterCount(g_runtime.addModifier) != 2)
                g_runtime.addModifier = nullptr;
            if (g_runtime.removeModifier && Game::Rtti::ParameterCount(g_runtime.removeModifier) != 2)
                g_runtime.removeModifier = nullptr;

            const Game::Rtti::Class* transactionType = Game::Rtti::NativeType(g_runtime.transactionSystem);
            g_runtime.getItemInSlot = Game::Rtti::FindFunction(transactionType, Game::Rtti::Hash("GetItemInSlot"));
            if (g_runtime.getItemInSlot && Game::Rtti::ParameterCount(g_runtime.getItemInSlot) != 2)
                g_runtime.getItemInSlot = nullptr;
            g_runtime.modifierDataClass = Game::Rtti::GetClass(
                Game::Rtti::Hash("gameConstantStatModifierData"));
            g_runtime.modifierDataSize = Game::Rtti::ClassSize(g_runtime.modifierDataClass);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_runtime.gameInstance = nullptr;
            g_runtime.playerSystem = nullptr;
            g_runtime.statsSystem = nullptr;
            g_runtime.transactionSystem = nullptr;
            g_runtime.getLocalPlayer = nullptr;
            g_runtime.addModifier = nullptr;
            g_runtime.removeModifier = nullptr;
            g_runtime.getItemInSlot = nullptr;
            g_runtime.modifierDataClass = nullptr;
            g_runtime.modifierDataSize = 0;
        }

        const bool resolved = g_runtime.statsSystem && g_runtime.playerSystem && g_runtime.getLocalPlayer &&
                              g_runtime.addModifier && g_runtime.removeModifier &&
                              g_runtime.modifierDataClass && g_runtime.modifierDataSize == kModifierDataSize;
        g_runtimeAvailable.store(resolved, std::memory_order_release);
        Diagnostics::Log("no-recoil resolver: player=%p stats=%p transaction=%p getPlayer=%p add=%p remove=%p "
                         "getItem=%p modifierClass=%p size=0x%zX resolved=%d",
                         g_runtime.playerSystem, g_runtime.statsSystem, g_runtime.transactionSystem,
                         g_runtime.getLocalPlayer, g_runtime.addModifier, g_runtime.removeModifier,
                         g_runtime.getItemInSlot, g_runtime.modifierDataClass, g_runtime.modifierDataSize,
                         resolved ? 1 : 0);
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

    bool GetLocalPlayer(Game::Rtti::Handle& player, std::uint64_t& playerId)
    {
        player = {};
        playerId = 0;
        if (!Game::Rtti::Invoke(g_runtime.getLocalPlayer, g_runtime.playerSystem, nullptr, 0, &player) ||
            !player.instance)
            return false;
        const bool valid = ReadEntityId(player.instance, playerId);
        if (!valid)
            Game::Rtti::ReleaseHandle(&player);
        return valid;
    }

    EquippedWeaponResult GetEquippedWeaponId(const Game::Rtti::Handle& player, std::uint64_t& weaponId)
    {
        weaponId = 0;
        if (!g_runtime.transactionSystem || !g_runtime.getItemInSlot || !player.instance)
            return EquippedWeaponResult::ApiUnavailable;
        TweakDbId slot;
        slot.hash = Crc32("AttachmentSlots.WeaponRight");
        slot.length = static_cast<std::uint8_t>(sizeof("AttachmentSlots.WeaponRight") - 1);
        Game::Rtti::Handle item;
        Game::Rtti::Argument arguments[] = {{const_cast<Game::Rtti::Handle*>(&player)}, {&slot}};
        const bool invoked = Game::Rtti::Invoke(g_runtime.getItemInSlot, g_runtime.transactionSystem,
                                                arguments, 2, &item);
        if (!invoked)
        {
            Game::Rtti::ReleaseHandle(&item);
            return EquippedWeaponResult::CallFailed;
        }
        if (!item.instance)
        {
            Game::Rtti::ReleaseHandle(&item);
            return EquippedWeaponResult::NoItem;
        }
        if (!ReadEntityId(item.instance, weaponId))
        {
            Game::Rtti::ReleaseHandle(&item);
            return EquippedWeaponResult::InvalidItem;
        }
        Game::Rtti::ReleaseHandle(&item);
        return EquippedWeaponResult::Found;
    }

    bool ModifierCall(Game::Rtti::Function* function, std::uint64_t targetId, ModifierEntry& modifier)
    {
        if (!function || !g_runtime.statsSystem || !modifier.handle.instance)
            return false;
        Game::Rtti::Argument arguments[] = {{&targetId}, {&modifier.handle}};
        bool result = false;
        const bool invoked = Game::Rtti::Invoke(function, g_runtime.statsSystem, arguments, 2,
                                                Game::Rtti::HasReturnValue(function) ? &result : nullptr);
        return invoked && (!Game::Rtti::HasReturnValue(function) || result);
    }

    bool RemoveModifiers()
    {
        bool allRemoved = true;
        std::size_t activeCount = 0;
        std::uint64_t removalTarget = 0;
        for (ModifierEntry& modifier : g_modifiers)
        {
            if (!modifier.active)
                continue;
            ++activeCount;
            if (removalTarget == 0)
                removalTarget = modifier.targetId;
            const bool removed = ModifierCall(g_runtime.removeModifier, modifier.targetId, modifier);
            if (!removed)
            {
                allRemoved = false;
                g_failures.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            Game::Rtti::ReleaseHandle(&modifier.handle);
            modifier = {};
            g_removed.fetch_add(1, std::memory_order_relaxed);
        }
        if (allRemoved)
        {
            if (activeCount == kModifierCount)
            {
                Diagnostics::Log("no-recoil modifiers removed: targetId=0x%llX count=%zu",
                                 static_cast<unsigned long long>(removalTarget), activeCount);
            }
            g_active.store(false, std::memory_order_release);
            g_playerId.store(0, std::memory_order_release);
            g_targetId.store(0, std::memory_order_release);
            g_weaponId.store(0, std::memory_order_release);
            g_usingWeaponTarget.store(false, std::memory_order_release);
        }
        return allRemoved;
    }

    bool ApplyModifiers(std::uint64_t targetId)
    {
        if (!targetId || !g_runtime.modifierDataClass || g_runtime.modifierDataSize != kModifierDataSize)
            return false;

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

            if (!Game::Rtti::ConstructHandle(&modifier.handle, modifier.instance) ||
                !ModifierCall(g_runtime.addModifier, targetId, modifier))
            {
                g_failures.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            modifier.active = true;
            // Mark the feature active as soon as the first exact handle is accepted. If a later stat fails, the
            // shutdown handshake must still wait for RemoveModifier to retry rather than assuming a clean rollback.
            g_active.store(true, std::memory_order_release);
            ++appliedCount;
            g_applied.fetch_add(1, std::memory_order_relaxed);
        }

        if (appliedCount != g_modifiers.size())
        {
            // Remove only modifiers whose exact handles were accepted. If an engine call failed, keep its handle
            // alive and let the next main tick retry; releasing it here could race an internal StatsSystem reference.
            RemoveModifiers();
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
    void PublishDesired(bool enabled)
    {
        const bool previous = g_desired.exchange(enabled, std::memory_order_acq_rel);
        if (previous != enabled)
            Diagnostics::Log("no-recoil desired settings published: enabled=%d", enabled ? 1 : 0);
    }

    void OnGameMainTick()
    {
        const bool requested = g_cleanupRequested.load(std::memory_order_acquire)
                                   ? false
                                   : g_desired.load(std::memory_order_acquire);
        const bool currentlyActive = g_active.load(std::memory_order_acquire);
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
            if (RemoveModifiers() && g_cleanupRequested.load(std::memory_order_acquire))
                g_cleanupAcknowledged.store(true, std::memory_order_release);
            return;
        }

        Game::Rtti::Handle player;
        std::uint64_t playerId = 0;
        if (!GetLocalPlayer(player, playerId))
        {
            if (currentlyActive)
                RemoveModifiers();
            g_failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        std::uint64_t weaponId = 0;
        const EquippedWeaponResult weaponResult = GetEquippedWeaponId(player, weaponId);
        const bool usingWeapon = weaponResult == EquippedWeaponResult::Found;
        if (weaponResult != EquippedWeaponResult::Found && weaponResult != EquippedWeaponResult::ApiUnavailable)
        {
            // A resolved TransactionSystem/GetItemInSlot path is authoritative. An empty or failed slot lookup is
            // not evidence that the player is a valid StatsSystem target: remove any old weapon modifiers and wait
            // for a valid right-hand weapon instead of silently applying recoil modifiers to the player entity.
            LogTargetDecision(playerId, weaponId, false, weaponResult);
            Game::Rtti::ReleaseHandle(&player);
            if (currentlyActive && !RemoveModifiers())
                return;
            return;
        }

        // Player fallback is intentionally restricted to the case where the transaction API itself could not be
        // resolved. Once GetItemInSlot is available, the branch above waits for an equipped weapon.
        const std::uint64_t targetId = usingWeapon ? weaponId : playerId;
        LogTargetDecision(playerId, weaponId, usingWeapon, weaponResult);
        Game::Rtti::ReleaseHandle(&player);

        const std::uint64_t currentTarget = g_targetId.load(std::memory_order_acquire);
        const std::uint64_t currentPlayer = g_playerId.load(std::memory_order_acquire);
        const bool currentPathWeapon = g_usingWeaponTarget.load(std::memory_order_acquire);
        if (currentlyActive && (currentPlayer != playerId || currentTarget != targetId ||
                                currentPathWeapon != usingWeapon))
        {
            if (!RemoveModifiers())
                return;
        }
        if (!g_active.load(std::memory_order_acquire))
        {
            if (ApplyModifiers(targetId))
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
        result.failures = g_failures.load(std::memory_order_relaxed);
        return result;
    }
}
