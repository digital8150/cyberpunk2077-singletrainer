#include "silent_aim.h"

#include "rtti_invoker.h"
#include "signature_scanner.h"
#include "../diagnostics.h"
#include "../framework.h"
#include "../hooks/hook_lifecycle.h"

#include <MinHook.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace
{
    constexpr std::size_t kMaxListenerHooks = 4;
    constexpr std::size_t kMaxProducerHooks = 5;
    constexpr std::size_t kShootStartPointOffset = 0xF0;
    constexpr std::size_t kShootStartVelocityOffset = 0x100;
    constexpr std::size_t kSetUpOwnerOffset = 0x40;
    constexpr ULONGLONG kTargetTimeoutMilliseconds = 250;
    // The first validated local-player event is observation-only. This separates callback/field validation from
    // mutation and prevents a burst or shotgun volley from being rewritten during the initial live ABI probe.
    constexpr ULONGLONG kValidationObservationMilliseconds = 1000;
    // Live observation must establish the exact event path and payload before any projectile field is modified.
    constexpr bool kEnableProjectileMutation = false;
    // Native RTTI handlers have one documented VM ABI. These hooks only count calls while a target is armed;
    // they do not inspect stack-frame parameters or modify effect/crosshair data.
    constexpr bool kEnableProducerObservationHooks = true;
    // Hooking the two RTTI event-108 callback targets is unsafe: those code targets are reused outside the typed
    // listener dispatch, and the live game crashed before a valid weapon payload was observed.
    constexpr bool kEnableWeaponListenerObservationHooks = false;
    constexpr std::uint64_t kShootEventType = Game::Rtti::Hash("gameprojectileShootEvent");
    constexpr std::uint64_t kShootTargetEventType = Game::Rtti::Hash("gameprojectileShootTargetEvent");
    constexpr std::uint64_t kSetUpEventType = Game::Rtti::Hash("gameprojectileSetUpEvent");
    constexpr std::uint64_t kWeaponShootEventType = Game::Rtti::Hash("gameweaponeventsShootEvent");
    constexpr std::uint64_t kPlayerPuppetType = Game::Rtti::Hash("PlayerPuppet");
    constexpr std::uint64_t kGamePlayerPuppetType = Game::Rtti::Hash("gamePlayerPuppet");
    constexpr std::uint32_t kRttiSystemGetAddressHash = 0x4A610F64u;
    constexpr std::uint8_t kQueueEventInternalPattern[] = {
        0x48, 0x83, 0xEC, 0x28, 0x8A, 0x81, 0x56, 0x01, 0x00, 0x00, 0x2C, 0x06, 0x3C, 0x01, 0x76, 0x00,
        0x48, 0x81, 0xC1, 0xD8, 0x00, 0x00, 0x00, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x28, 0xC3,
    };
    constexpr char kQueueEventInternalMask[] = "xxxxxxxxxxxxxxx?xxxxxxxx????xxxxx";
    static_assert(sizeof(kQueueEventInternalPattern) == sizeof(kQueueEventInternalMask) - 1);

    struct DynArrayLayout
    {
        void* entries;
        std::uint32_t capacity;
        std::uint32_t size;
    };
    static_assert(sizeof(DynArrayLayout) == 0x10);

    struct CallbackHandlerLayout
    {
        void* invoke;
        void* copy;
        void* move;
        void* destruct;
    };

    struct ListenerLayout
    {
        std::byte callbackTarget[0x10];
        CallbackHandlerLayout* callbackHandler;
        std::uint64_t callbackName;
        std::int16_t eventTypeId;
        bool isScripted;
        std::byte pad23[5];
    };
    static_assert(sizeof(ListenerLayout) == 0x28);
    static_assert(offsetof(ListenerLayout, callbackHandler) == 0x10);
    static_assert(offsetof(ListenerLayout, eventTypeId) == 0x20);

    struct ClassLayout
    {
        std::byte pad00[0x10];
        ClassLayout* parent;
        std::uint64_t nameHash;
        std::byte pad20[0x1B0 - 0x20];
        DynArrayLayout listeners;
        std::byte pad1C0[0x2C0 - 0x1C0];
        std::int16_t eventTypeId;
    };
    static_assert(offsetof(ClassLayout, listeners) == 0x1B0);
    static_assert(offsetof(ClassLayout, eventTypeId) == 0x2C0);

    struct FunctionProbeLayout
    {
        void** vtable;
        std::byte pad08[0xA8 - 0x08];
        std::uint32_t flags;
        std::uint32_t padAC;
        ClassLayout* parent;
        std::uint32_t regIndex;
    };
    static_assert(offsetof(FunctionProbeLayout, flags) == 0xA8);
    static_assert(offsetof(FunctionProbeLayout, parent) == 0xB0);
    static_assert(offsetof(FunctionProbeLayout, regIndex) == 0xB8);

    struct Vector4Layout
    {
        float x;
        float y;
        float z;
        float w;
    };
    static_assert(sizeof(Vector4Layout) == 0x10);

    // Native REDengine event listeners use Callback<void(IScriptable&, Handle<IScriptable>&)> with an unbound
    // function target. Its shared invoke thunk unwraps Handle::instance before tail-calling this target, so the
    // verified native ABI is (listener instance, event instance), not (listener instance, Handle*).
    using ListenerFn = void (*)(void*, void*);
    using NativeHandlerFn = void (*)(void*, void*, void*, void*);
    using QueueEventInternalFn = void (*)(void*, Game::Rtti::Handle*);
    using ResolveAddressFn = std::uintptr_t (*)(std::uint32_t);

    enum class ListenerHookKind : std::uint8_t
    {
        Unknown,
        Projectile,
        WeaponShoot,
    };

    enum class ProducerHookKind : std::uint8_t
    {
        Unknown,
        EffectRun,
        AttackStart,
        AttackPrepare,
        Crosshair,
        DefaultCrosshair,
    };

    struct State
    {
        std::atomic_bool hookCreated{false};
        std::atomic_bool queueHookCreated{false};
        std::atomic_uint32_t listenerHooks{0};
        std::atomic_bool targetActive{false};
        std::atomic<float> targetX{0.0f};
        std::atomic<float> targetY{0.0f};
        std::atomic<float> targetZ{0.0f};
        std::atomic_uint64_t targetGeneration{0};
        std::atomic_uint64_t targetPublishedAt{0};
        std::atomic_uint64_t callbacks{0};
        std::atomic_uint64_t queueCallbacks{0};
        std::atomic_uint64_t projectileEvents{0};
        std::atomic_uint64_t weaponShootEvents{0};
        std::atomic_bool weaponPayloadLogged{false};
        std::atomic_uint32_t listenerPayloadLogs{0};
        std::atomic_uint64_t localPlayerEvents{0};
        std::atomic_uint64_t validatedLocalEvents{0};
        std::atomic_uint64_t validationEstablishedAt{0};
        std::atomic_uint64_t redirectedShots{0};
        std::atomic_uint64_t rejectedShots{0};
        std::atomic_uint32_t producerHooks{0};
        std::atomic_uint64_t effectRuns{0};
        std::atomic_uint64_t attackStarts{0};
        std::atomic_uint64_t attackPrepares{0};
        std::atomic_uint64_t crosshairCalls{0};
        std::atomic_uint64_t defaultCrosshairCalls{0};
    };

    State g_state;
    std::array<void*, kMaxListenerHooks> g_hookTargets{};
    std::array<ListenerFn, kMaxListenerHooks> g_originalListeners{};
    std::array<ListenerHookKind, kMaxListenerHooks> g_listenerKinds{};
    const Game::Rtti::Class* g_weaponShootClass = nullptr;
    void* g_queueHookTarget = nullptr;
    QueueEventInternalFn g_originalQueueEventInternal = nullptr;
    std::array<void*, kMaxProducerHooks> g_producerHookTargets{};
    std::array<NativeHandlerFn, kMaxProducerHooks> g_originalProducerHandlers{};
    std::array<ProducerHookKind, kMaxProducerHooks> g_producerHookKinds{};

    bool IsExecutableAddress(const void* address)
    {
        if (!address)
            return false;
        MEMORY_BASIC_INFORMATION information{};
        if (VirtualQuery(address, &information, sizeof(information)) != sizeof(information) ||
            information.State != MEM_COMMIT || (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        {
            return false;
        }
        const DWORD protection = information.Protect & 0xFFu;
        return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
               protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    }

    bool ReadTarget(float output[3])
    {
        if (!g_state.targetActive.load(std::memory_order_acquire))
            return false;
        const ULONGLONG publishedAt = g_state.targetPublishedAt.load(std::memory_order_acquire);
        const ULONGLONG now = GetTickCount64();
        if (publishedAt == 0 || now < publishedAt || now - publishedAt > kTargetTimeoutMilliseconds)
            return false;
        for (unsigned attempt = 0; attempt < 3; ++attempt)
        {
            const std::uint64_t before = g_state.targetGeneration.load(std::memory_order_acquire);
            if ((before & 1u) != 0)
                continue;
            output[0] = g_state.targetX.load(std::memory_order_relaxed);
            output[1] = g_state.targetY.load(std::memory_order_relaxed);
            output[2] = g_state.targetZ.load(std::memory_order_relaxed);
            const std::uint64_t after = g_state.targetGeneration.load(std::memory_order_acquire);
            if (before == after)
                return std::isfinite(output[0]) && std::isfinite(output[1]) && std::isfinite(output[2]);
        }
        return false;
    }

    bool IsPlayerOwner(void* owner)
    {
        if (!owner)
            return false;
        const Game::Rtti::Class* type = Game::Rtti::NativeType(owner);
        return Game::Rtti::IsClassOrDerived(type, kPlayerPuppetType) ||
               Game::Rtti::IsClassOrDerived(type, kGamePlayerPuppetType);
    }

    void ObserveWeaponShootEvent(void* event, const Game::Rtti::Class* type)
    {
        g_state.weaponShootEvents.fetch_add(1, std::memory_order_relaxed);
        if (Game::Rtti::ClassSize(type) != 0x1E0 ||
            g_state.weaponPayloadLogged.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }

        const auto* bytes = static_cast<const std::byte*>(event);
        Diagnostics::Log("silent aim weapon ShootEvent observed: event=%p size=0x%zX", event,
                         Game::Rtti::ClassSize(type));
        // The native weapon event exposes no reflected properties. Capture its vector-sized payload slots once;
        // the values are read-only and will be correlated with the live muzzle/camera coordinates before any field
        // is selected for redirection.
        for (std::size_t offset = 0x40; offset <= 0x130; offset += 0x10)
        {
            const auto* values = reinterpret_cast<const float*>(bytes + offset);
            Diagnostics::Log("weapon ShootEvent +0x%03zX: f=(%.6g,%.6g,%.6g,%.6g) q=(%016llX,%016llX)",
                             offset, values[0], values[1], values[2], values[3],
                             static_cast<unsigned long long>(
                                 *reinterpret_cast<const std::uint64_t*>(bytes + offset)),
                             static_cast<unsigned long long>(
                                 *reinterpret_cast<const std::uint64_t*>(bytes + offset + 8)));
        }
    }

    void RedirectProjectileEvent(void* event)
    {
        if (!event)
            return;
        const Game::Rtti::Class* type = Game::Rtti::NativeType(event);
        if (Game::Rtti::IsClassOrDerived(type, kWeaponShootEventType))
        {
            ObserveWeaponShootEvent(event, type);
            return;
        }
        if (!Game::Rtti::IsClassOrDerived(type, kShootEventType) &&
            !Game::Rtti::IsClassOrDerived(type, kShootTargetEventType))
        {
            if (g_state.targetActive.load(std::memory_order_acquire))
            {
                const std::uint32_t logIndex = g_state.listenerPayloadLogs.fetch_add(1, std::memory_order_relaxed);
                if (logIndex < 128)
                {
                    const auto* eventType = reinterpret_cast<const ClassLayout*>(type);
                    Diagnostics::Log("silent aim listener payload: index=%u event=%p type=%016llX eventId=%d "
                                     "size=0x%zX",
                                     logIndex + 1, event,
                                     static_cast<unsigned long long>(eventType ? eventType->nameHash : 0),
                                     eventType ? eventType->eventTypeId : -1, Game::Rtti::ClassSize(type));
                }
            }
            return;
        }
        g_state.projectileEvents.fetch_add(1, std::memory_order_relaxed);

        auto* bytes = static_cast<std::byte*>(event);
        const auto* owner = reinterpret_cast<const Game::Rtti::Handle*>(bytes + kSetUpOwnerOffset);
        if (!owner || !IsPlayerOwner(owner->instance))
            return;
        g_state.localPlayerEvents.fetch_add(1, std::memory_order_relaxed);

        // All Shoot-only accesses below are guarded by the reflected runtime size as well as the derived type.
        if (Game::Rtti::ClassSize(type) < 0x120)
        {
            g_state.rejectedShots.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        float target[3]{};
        if (!ReadTarget(target))
            return;

        auto* start = reinterpret_cast<Vector4Layout*>(bytes + kShootStartPointOffset);
        auto* velocity = reinterpret_cast<Vector4Layout*>(bytes + kShootStartVelocityOffset);
        const float speed = std::sqrt(velocity->x * velocity->x + velocity->y * velocity->y +
                                      velocity->z * velocity->z);
        const float deltaX = target[0] - start->x;
        const float deltaY = target[1] - start->y;
        const float deltaZ = target[2] - start->z;
        const float distance = std::sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
        const bool vectorsPlausible = std::isfinite(start->x) && std::isfinite(start->y) &&
                                      std::isfinite(start->z) && std::isfinite(velocity->x) &&
                                      std::isfinite(velocity->y) && std::isfinite(velocity->z) &&
                                      std::abs(start->x) < 10000000.0f && std::abs(start->y) < 10000000.0f &&
                                      std::abs(start->z) < 10000000.0f;
        if (!vectorsPlausible || !std::isfinite(speed) || !std::isfinite(distance) ||
            speed < 0.01f || speed > 1000000.0f || distance < 0.01f)
        {
            g_state.rejectedShots.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        g_state.validatedLocalEvents.fetch_add(1, std::memory_order_relaxed);
        ULONGLONG establishedAt = g_state.validationEstablishedAt.load(std::memory_order_acquire);
        const ULONGLONG now = GetTickCount64();
        if (establishedAt == 0)
        {
            ULONGLONG expected = 0;
            if (g_state.validationEstablishedAt.compare_exchange_strong(expected, now, std::memory_order_acq_rel))
            {
                Diagnostics::Log("silent aim live layout validated (observation only): event=%p owner=%p "
                                 "start=(%.2f,%.2f,%.2f) velocity=(%.2f,%.2f,%.2f) speed=%.2f",
                                 event, owner->instance, start->x, start->y, start->z,
                                 velocity->x, velocity->y, velocity->z, speed);
                return;
            }
            establishedAt = expected;
        }
        if (now < establishedAt || now - establishedAt < kValidationObservationMilliseconds)
            return;

        if (!kEnableProjectileMutation)
            return;

        const float scale = speed / distance;
        velocity->x = deltaX * scale;
        velocity->y = deltaY * scale;
        velocity->z = deltaZ * scale;
        const std::uint64_t redirected = g_state.redirectedShots.fetch_add(1, std::memory_order_relaxed) + 1;
        if (redirected <= 4 || (redirected % 32u) == 0)
        {
            Diagnostics::Log("silent aim redirected projectile: count=%llu owner=%p start=(%.2f,%.2f,%.2f) "
                             "target=(%.2f,%.2f,%.2f) speed=%.2f",
                             static_cast<unsigned long long>(redirected), owner->instance,
                             start->x, start->y, start->z, target[0], target[1], target[2], speed);
        }
    }

    void RedirectProjectileEventSafely(void* event)
    {
        __try
        {
            RedirectProjectileEvent(event);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_state.rejectedShots.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void ObserveWeaponShootEventSafely(void* event)
    {
        __try
        {
            ObserveWeaponShootEvent(event, g_weaponShootClass);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_state.rejectedShots.fetch_add(1, std::memory_order_relaxed);
        }
    }

    template<std::size_t Index>
    void HookListener(void* instance, void* event)
    {
        HookLifecycle::CallbackGuard guard;
        g_state.callbacks.fetch_add(1, std::memory_order_relaxed);
        if (!HookLifecycle::IsShuttingDown())
        {
            if (g_listenerKinds[Index] == ListenerHookKind::WeaponShoot)
            {
                ObserveWeaponShootEventSafely(event);
            }
            else
            {
                RedirectProjectileEventSafely(event);
            }
        }
        if (g_originalListeners[Index])
            g_originalListeners[Index](instance, event);
    }

    constexpr std::array<ListenerFn, kMaxListenerHooks> kDetours = {
        &HookListener<0>, &HookListener<1>, &HookListener<2>, &HookListener<3>};

    bool IsTargetFresh()
    {
        if (!g_state.targetActive.load(std::memory_order_acquire))
            return false;
        const ULONGLONG publishedAt = g_state.targetPublishedAt.load(std::memory_order_acquire);
        const ULONGLONG now = GetTickCount64();
        return publishedAt != 0 && now >= publishedAt && now - publishedAt <= kTargetTimeoutMilliseconds;
    }

    const char* ProducerName(ProducerHookKind kind)
    {
        switch (kind)
        {
        case ProducerHookKind::EffectRun:
            return "EffectInstance.Run";
        case ProducerHookKind::AttackStart:
            return "IAttack.StartAttack";
        case ProducerHookKind::AttackPrepare:
            return "Attack_GameEffect.PrepareAttack";
        case ProducerHookKind::Crosshair:
            return "TargetingSystem.GetCrosshairData";
        case ProducerHookKind::DefaultCrosshair:
            return "TargetingSystem.GetDefaultCrosshairData";
        default:
            return "unknown";
        }
    }

    std::atomic_uint64_t* ProducerCounter(ProducerHookKind kind)
    {
        switch (kind)
        {
        case ProducerHookKind::EffectRun:
            return &g_state.effectRuns;
        case ProducerHookKind::AttackStart:
            return &g_state.attackStarts;
        case ProducerHookKind::AttackPrepare:
            return &g_state.attackPrepares;
        case ProducerHookKind::Crosshair:
            return &g_state.crosshairCalls;
        case ProducerHookKind::DefaultCrosshair:
            return &g_state.defaultCrosshairCalls;
        default:
            return nullptr;
        }
    }

    std::uint64_t ClassNameHash(const Game::Rtti::Class* type)
    {
        return type ? reinterpret_cast<const ClassLayout*>(type)->nameHash : 0;
    }

    void ObserveProducerCall(ProducerHookKind kind, void* context)
    {
        if (!IsTargetFresh())
            return;
        std::atomic_uint64_t* counter = ProducerCounter(kind);
        if (!counter)
            return;
        const std::uint64_t count = counter->fetch_add(1, std::memory_order_relaxed) + 1;
        if (count > 4 && (count & (count - 1)) != 0)
            return;

        const Game::Rtti::Class* contextType = Game::Rtti::NativeType(context);
        const std::uint64_t typeHash = ClassNameHash(contextType);
        const char* typeName = Game::Rtti::ResolveName(typeHash);
        Diagnostics::Log("silent aim producer observed: path=%s count=%llu context=%p type=%s(%016llX) mutation=0",
                         ProducerName(kind), static_cast<unsigned long long>(count), context,
                         typeName && typeName[0] ? typeName : "?", static_cast<unsigned long long>(typeHash));
    }

    void ObserveProducerCallSafely(ProducerHookKind kind, void* context)
    {
        __try
        {
            ObserveProducerCall(kind, context);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_state.rejectedShots.fetch_add(1, std::memory_order_relaxed);
        }
    }

    template<std::size_t Index>
    void HookProducerHandler(void* context, void* frame, void* result, void* resultType)
    {
        HookLifecycle::CallbackGuard guard;
        if (!HookLifecycle::IsShuttingDown())
            ObserveProducerCallSafely(g_producerHookKinds[Index], context);
        if (g_originalProducerHandlers[Index])
            g_originalProducerHandlers[Index](context, frame, result, resultType);
    }

    constexpr std::array<NativeHandlerFn, kMaxProducerHooks> kProducerDetours = {
        &HookProducerHandler<0>, &HookProducerHandler<1>, &HookProducerHandler<2>,
        &HookProducerHandler<3>, &HookProducerHandler<4>};

    bool AlreadyHookedProducer(void* target)
    {
        for (void* existing : g_producerHookTargets)
        {
            if (existing == target)
                return true;
        }
        return false;
    }

    bool AddProducerObservationHook(const char* className, const char* functionName, ProducerHookKind kind)
    {
        Game::Rtti::Class* type = Game::Rtti::GetClass(Game::Rtti::Hash(className));
        Game::Rtti::Function* function = type
                                             ? Game::Rtti::FindFunction(type, Game::Rtti::Hash(functionName))
                                             : nullptr;
        Game::Rtti::FunctionInfo info{};
        const bool inspected = Game::Rtti::InspectFunction(function, info);
        const std::uint64_t classHash = ClassNameHash(type);
        const std::uint64_t ownerHash = inspected ? ClassNameHash(info.parent) : 0;
        const char* resolvedClassName = Game::Rtti::ResolveName(classHash);
        const char* resolvedOwnerName = Game::Rtti::ResolveName(ownerHash);
        const char* resolvedFullName = Game::Rtti::ResolveName(info.fullNameHash);
        HMODULE executable = GetModuleHandleW(L"Cyberpunk2077.exe");
        const auto imageBase = reinterpret_cast<std::uintptr_t>(executable);
        const auto handlerAddress = reinterpret_cast<std::uintptr_t>(info.nativeHandler);
        const std::uint64_t imageOffset = imageBase && handlerAddress >= imageBase
                                              ? static_cast<std::uint64_t>(handlerAddress - imageBase)
                                              : 0;
        Diagnostics::Log("silent aim producer RTTI: request=%s.%s class=%s(%016llX) function=%p "
                         "full=%s(%016llX) owner=%s(%016llX) flags=0x%08X regIndex=%u params=%zu "
                         "handler=%p imageOffset=0x%llX",
                         className, functionName,
                         resolvedClassName && resolvedClassName[0] ? resolvedClassName : "?",
                         static_cast<unsigned long long>(classHash), function,
                         resolvedFullName && resolvedFullName[0] ? resolvedFullName : "?",
                         static_cast<unsigned long long>(info.fullNameHash),
                         resolvedOwnerName && resolvedOwnerName[0] ? resolvedOwnerName : "?",
                         static_cast<unsigned long long>(ownerHash), info.flags, info.registrationIndex,
                         info.parameterCount, info.nativeHandler, static_cast<unsigned long long>(imageOffset));

        if (!kEnableProducerObservationHooks || !inspected || (info.flags & 1u) == 0 ||
            (info.flags & 2u) != 0 || !IsExecutableAddress(info.nativeHandler))
        {
            return false;
        }
        if (AlreadyHookedProducer(info.nativeHandler))
        {
            Diagnostics::Log("silent aim producer handler shared; observation hook skipped: path=%s handler=%p",
                             ProducerName(kind), info.nativeHandler);
            return false;
        }

        const std::uint32_t slot = g_state.producerHooks.load(std::memory_order_relaxed);
        if (slot >= kMaxProducerHooks)
            return false;
        const MH_STATUS status = MH_CreateHook(info.nativeHandler, kProducerDetours[slot],
                                               reinterpret_cast<void**>(&g_originalProducerHandlers[slot]));
        if (status != MH_OK)
        {
            Diagnostics::Log("MH_CreateHook(silent aim producer) failed: path=%s handler=%p status=%s (%d)",
                             ProducerName(kind), info.nativeHandler, MH_StatusToString(status), status);
            return false;
        }
        g_producerHookTargets[slot] = info.nativeHandler;
        g_producerHookKinds[slot] = kind;
        g_state.producerHooks.store(slot + 1, std::memory_order_relaxed);
        Diagnostics::Log("silent aim producer observation hook created: path=%s handler=%p original=%p mutation=0",
                         ProducerName(kind), info.nativeHandler,
                         reinterpret_cast<void*>(g_originalProducerHandlers[slot]));
        return true;
    }

    void ObserveQueuedEventSafely(void* entity, Game::Rtti::Handle* eventHandle)
    {
        __try
        {
            if (!eventHandle || !eventHandle->instance)
                return;
            void* event = eventHandle->instance;
            const Game::Rtti::Class* type = Game::Rtti::NativeType(event);
            if (!type)
                return;
            if (Game::Rtti::IsClassOrDerived(type, kWeaponShootEventType))
            {
                const auto* entityType = reinterpret_cast<const ClassLayout*>(Game::Rtti::NativeType(entity));
                Diagnostics::Log("silent aim QueueEvent weapon event: entity=%p entityType=%016llX event=%p",
                                 entity,
                                 static_cast<unsigned long long>(entityType ? entityType->nameHash : 0), event);
                ObserveWeaponShootEvent(event, type);
            }
            else if (Game::Rtti::IsClassOrDerived(type, kShootEventType) ||
                     Game::Rtti::IsClassOrDerived(type, kShootTargetEventType))
            {
                g_state.projectileEvents.fetch_add(1, std::memory_order_relaxed);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_state.rejectedShots.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void HookQueueEventInternal(void* entity, Game::Rtti::Handle* eventHandle)
    {
        HookLifecycle::CallbackGuard guard;
        g_state.queueCallbacks.fetch_add(1, std::memory_order_relaxed);
        if (!HookLifecycle::IsShuttingDown())
            ObserveQueuedEventSafely(entity, eventHandle);
        if (g_originalQueueEventInternal)
            g_originalQueueEventInternal(entity, eventHandle);
    }

    bool AddQueueEventObservationHook()
    {
        const Game::Signatures::ScanResult scan = Game::Signatures::FindInText(
            GetModuleHandleW(L"Cyberpunk2077.exe"), kQueueEventInternalPattern, kQueueEventInternalMask,
            sizeof(kQueueEventInternalPattern));
        Diagnostics::Log("silent aim QueueEvent internal scan: matches=%zu target=%p", scan.matches, scan.address);
        if (scan.matches != 1 || !scan.address)
            return false;
        const MH_STATUS status = MH_CreateHook(scan.address, &HookQueueEventInternal,
                                               reinterpret_cast<void**>(&g_originalQueueEventInternal));
        if (status != MH_OK)
        {
            Diagnostics::Log("MH_CreateHook(silent aim QueueEvent) failed: target=%p status=%s (%d)",
                             scan.address, MH_StatusToString(status), status);
            return false;
        }
        g_queueHookTarget = scan.address;
        g_state.queueHookCreated.store(true, std::memory_order_release);
        Diagnostics::Log("silent aim QueueEvent observation hook created: target=%p original=%p mutation=0",
                         scan.address, reinterpret_cast<void*>(g_originalQueueEventInternal));
        return true;
    }

    bool AlreadyHooked(void* target);

    bool EnumerateWeaponShootListeners(std::int16_t weaponShootEventId)
    {
        bool createdAny = false;
        __try
        {
            HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
            const auto resolve = red4ext
                                     ? reinterpret_cast<ResolveAddressFn>(
                                           GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                                     : nullptr;
            using GetRttiSystemFn = void* (*)();
            void* rttiSystem = resolve
                                   ? reinterpret_cast<GetRttiSystemFn>(resolve(kRttiSystemGetAddressHash))()
                                   : nullptr;
            auto* baseClass = reinterpret_cast<ClassLayout*>(
                Game::Rtti::GetClass(Game::Rtti::Hash("IScriptable")));
            void** vtable = rttiSystem ? *reinterpret_cast<void***>(rttiSystem) : nullptr;
            if (!rttiSystem || !baseClass || !vtable || !IsExecutableAddress(vtable[14]))
            {
                Diagnostics::Log("silent aim listener enumeration unavailable: rtti=%p base=%p getClasses=%p",
                                 rttiSystem, baseClass, vtable ? vtable[14] : nullptr);
                return false;
            }

            DynArrayLayout classes{};
            using GetClassesFn = void (*)(void*, ClassLayout*, DynArrayLayout*, void*, bool);
            reinterpret_cast<GetClassesFn>(vtable[14])(rttiSystem, baseClass, &classes, nullptr, true);
            if (!classes.entries || classes.size > classes.capacity || classes.size > 100000)
            {
                Diagnostics::Log("silent aim listener enumeration invalid: entries=%p size=%u capacity=%u",
                                 classes.entries, classes.size, classes.capacity);
                return false;
            }

            std::uint32_t listenerCount = 0;
            std::uint32_t nativeCount = 0;
            auto** entries = static_cast<ClassLayout**>(classes.entries);
            for (std::uint32_t classIndex = 0; classIndex < classes.size; ++classIndex)
            {
                ClassLayout* type = entries[classIndex];
                if (!type || !type->listeners.entries || type->listeners.size > type->listeners.capacity ||
                    type->listeners.size > 4096)
                {
                    continue;
                }
                auto* listeners = static_cast<ListenerLayout*>(type->listeners.entries);
                for (std::uint32_t listenerIndex = 0; listenerIndex < type->listeners.size; ++listenerIndex)
                {
                    ListenerLayout& listener = listeners[listenerIndex];
                    if (listener.eventTypeId != weaponShootEventId)
                        continue;
                    void* target = nullptr;
                    std::memcpy(&target, listener.callbackTarget, sizeof(target));
                    ++listenerCount;
                    if (!listener.isScripted)
                        ++nativeCount;
                    const char* className = Game::Rtti::ResolveName(type->nameHash);
                    const char* callbackName = Game::Rtti::ResolveName(listener.callbackName);
                    Diagnostics::Log("silent aim weapon listener subscriber: class=%s(%016llX) scripted=%u "
                                     "callback=%s(%016llX) target=%p invoke=%p",
                                     className && className[0] ? className : "?",
                                     static_cast<unsigned long long>(type->nameHash),
                                     listener.isScripted ? 1u : 0u,
                                     callbackName && callbackName[0] ? callbackName : "?",
                                     static_cast<unsigned long long>(listener.callbackName), target,
                                     listener.callbackHandler ? listener.callbackHandler->invoke : nullptr);
                    if (!kEnableWeaponListenerObservationHooks || listener.isScripted ||
                        !IsExecutableAddress(target) || AlreadyHooked(target))
                        continue;
                    const std::uint32_t slot = g_state.listenerHooks.load(std::memory_order_relaxed);
                    if (slot >= kMaxListenerHooks)
                        continue;
                    const MH_STATUS status = MH_CreateHook(target, kDetours[slot],
                                                           reinterpret_cast<void**>(&g_originalListeners[slot]));
                    if (status != MH_OK)
                    {
                        Diagnostics::Log("MH_CreateHook(silent aim weapon listener) failed: target=%p "
                                         "status=%s (%d)",
                                         target, MH_StatusToString(status), status);
                        continue;
                    }
                    g_hookTargets[slot] = target;
                    g_listenerKinds[slot] = ListenerHookKind::WeaponShoot;
                    g_state.listenerHooks.store(slot + 1, std::memory_order_relaxed);
                    createdAny = true;
                }
            }
            Diagnostics::Log("silent aim weapon listener enumeration: classes=%u eventId=%d listeners=%u native=%u",
                             classes.size, weaponShootEventId, listenerCount, nativeCount);
            return createdAny;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Diagnostics::Log("silent aim weapon listener enumeration raised an exception");
            return false;
        }
    }

    bool AlreadyHooked(void* target)
    {
        for (void* existing : g_hookTargets)
        {
            if (existing == target)
                return true;
        }
        return false;
    }

    bool AddListenerHooks(ClassLayout* type, std::int16_t setUpEventId, std::int16_t shootEventId,
                          std::int16_t shootTargetEventId, ListenerHookKind kind)
    {
        bool createdAny = false;
        for (unsigned depth = 0; type && depth < 24; ++depth, type = type->parent)
        {
            const DynArrayLayout& listeners = type->listeners;
            if (!listeners.entries || listeners.size > listeners.capacity || listeners.size > 4096)
                continue;
            auto* entries = static_cast<ListenerLayout*>(listeners.entries);
            for (std::uint32_t index = 0; index < listeners.size; ++index)
            {
                ListenerLayout& listener = entries[index];
                void* target = nullptr;
                static_assert(sizeof(target) <= sizeof(listener.callbackTarget));
                std::memcpy(&target, listener.callbackTarget, sizeof(target));
                // REDengine dispatches a derived ShootEvent through the projectile component's SetUpEvent listener.
                // The detour still validates the live event's native type before touching its Shoot-only fields.
                if (listener.eventTypeId != setUpEventId && listener.eventTypeId != shootEventId &&
                    listener.eventTypeId != shootTargetEventId)
                {
                    continue;
                }
                if (listener.isScripted || !IsExecutableAddress(target) || AlreadyHooked(target))
                    continue;
                Diagnostics::Log("silent aim native listener candidate: class=%016llX eventId=%d target=%p "
                                 "invoke=%p",
                                 static_cast<unsigned long long>(type->nameHash), listener.eventTypeId, target,
                                 listener.callbackHandler ? listener.callbackHandler->invoke : nullptr);
                const std::uint32_t slot = g_state.listenerHooks.load(std::memory_order_relaxed);
                if (slot >= kMaxListenerHooks)
                    continue;
                const MH_STATUS status = MH_CreateHook(target, kDetours[slot],
                                                       reinterpret_cast<void**>(&g_originalListeners[slot]));
                if (status != MH_OK)
                {
                    Diagnostics::Log("MH_CreateHook(silent aim listener) failed: target=%p status=%s (%d)",
                                     target, MH_StatusToString(status), status);
                    continue;
                }
                g_hookTargets[slot] = target;
                g_listenerKinds[slot] = kind;
                g_state.listenerHooks.store(slot + 1, std::memory_order_relaxed);
                createdAny = true;
            }
        }
        return createdAny;
    }

}

namespace Game::SilentAim
{
    bool CreateHook()
    {
        __try
        {
            bool created = false;
            created = AddProducerObservationHook("gameEffectInstance", "Run", ProducerHookKind::EffectRun) || created;
            created = AddProducerObservationHook("gameAttack_GameEffect", "StartAttack",
                                                 ProducerHookKind::AttackStart) || created;
            created = AddProducerObservationHook("gameAttack_GameEffect", "PrepareAttack",
                                                 ProducerHookKind::AttackPrepare) || created;
            created = AddProducerObservationHook("gametargetingTargetingSystem", "GetCrosshairData",
                                                 ProducerHookKind::Crosshair) || created;
            created = AddProducerObservationHook("gametargetingTargetingSystem", "GetDefaultCrosshairData",
                                                 ProducerHookKind::DefaultCrosshair) || created;

            auto* shootEvent = reinterpret_cast<ClassLayout*>(Game::Rtti::GetClass(kShootEventType));
            auto* shootTargetEvent = reinterpret_cast<ClassLayout*>(Game::Rtti::GetClass(kShootTargetEventType));
            auto* setUpEvent = reinterpret_cast<ClassLayout*>(Game::Rtti::GetClass(kSetUpEventType));
            auto* weaponShootEvent = reinterpret_cast<ClassLayout*>(Game::Rtti::GetClass(kWeaponShootEventType));
            auto* projectileComponent = reinterpret_cast<ClassLayout*>(
                Game::Rtti::GetClass(Game::Rtti::Hash("gameprojectileComponent")));
            if (!setUpEvent || !shootEvent || !shootTargetEvent || !weaponShootEvent || !projectileComponent)
            {
                Diagnostics::Log("silent aim RTTI unavailable: setup=%p shoot=%p shootTarget=%p weaponShoot=%p "
                                 "component=%p",
                                 setUpEvent, shootEvent, shootTargetEvent, weaponShootEvent,
                                 projectileComponent);
            }
            else
            {
                Diagnostics::Log("silent aim projectile RTTI: setupId=%d shootId=%d shootTargetId=%d "
                                 "weaponShootId=%d componentListeners=%u",
                                 setUpEvent->eventTypeId, shootEvent->eventTypeId, shootTargetEvent->eventTypeId,
                                 weaponShootEvent->eventTypeId, projectileComponent->listeners.size);
                g_weaponShootClass = reinterpret_cast<Game::Rtti::Class*>(weaponShootEvent);
                // Enumeration remains read-only. The two event-108 callback targets are deliberately never hooked.
                EnumerateWeaponShootListeners(weaponShootEvent->eventTypeId);
                created = AddListenerHooks(projectileComponent, setUpEvent->eventTypeId, shootEvent->eventTypeId,
                                           shootTargetEvent->eventTypeId,
                                           ListenerHookKind::Projectile) || created;
            }
            g_state.hookCreated.store(created, std::memory_order_release);
            Diagnostics::Log("silent aim observation hooks created: producers=%u projectileListeners=%u "
                             "weaponListenerHooks=0 queueHook=0 mutation=0",
                             g_state.producerHooks.load(std::memory_order_relaxed),
                             g_state.listenerHooks.load(std::memory_order_relaxed));
            return created;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Diagnostics::Log("silent aim RTTI producer discovery raised an exception");
            return false;
        }
    }

    void PublishTarget(const float worldTarget[3], bool active)
    {
        if (!active || !worldTarget || !g_state.hookCreated.load(std::memory_order_acquire) ||
            !std::isfinite(worldTarget[0]) || !std::isfinite(worldTarget[1]) || !std::isfinite(worldTarget[2]))
        {
            ClearTarget();
            return;
        }
        g_state.targetGeneration.fetch_add(1, std::memory_order_acq_rel);
        g_state.targetX.store(worldTarget[0], std::memory_order_relaxed);
        g_state.targetY.store(worldTarget[1], std::memory_order_relaxed);
        g_state.targetZ.store(worldTarget[2], std::memory_order_relaxed);
        g_state.targetPublishedAt.store(GetTickCount64(), std::memory_order_release);
        g_state.targetGeneration.fetch_add(1, std::memory_order_release);
        g_state.targetActive.store(true, std::memory_order_release);
    }

    void ClearTarget()
    {
        g_state.targetActive.store(false, std::memory_order_release);
        g_state.targetPublishedAt.store(0, std::memory_order_release);
    }

    DiagnosticsSnapshot GetDiagnostics()
    {
        DiagnosticsSnapshot result;
        result.hookCreated = g_state.hookCreated.load(std::memory_order_acquire);
        result.queueHookCreated = g_state.queueHookCreated.load(std::memory_order_acquire);
        result.listenerHooks = g_state.listenerHooks.load(std::memory_order_relaxed);
        result.producerHooks = g_state.producerHooks.load(std::memory_order_relaxed);
        result.callbacks = g_state.callbacks.load(std::memory_order_relaxed);
        result.queueCallbacks = g_state.queueCallbacks.load(std::memory_order_relaxed);
        result.projectileEvents = g_state.projectileEvents.load(std::memory_order_relaxed);
        result.weaponShootEvents = g_state.weaponShootEvents.load(std::memory_order_relaxed);
        result.localPlayerEvents = g_state.localPlayerEvents.load(std::memory_order_relaxed);
        result.validatedLocalEvents = g_state.validatedLocalEvents.load(std::memory_order_relaxed);
        result.redirectedShots = g_state.redirectedShots.load(std::memory_order_relaxed);
        result.rejectedShots = g_state.rejectedShots.load(std::memory_order_relaxed);
        result.effectRuns = g_state.effectRuns.load(std::memory_order_relaxed);
        result.attackStarts = g_state.attackStarts.load(std::memory_order_relaxed);
        result.attackPrepares = g_state.attackPrepares.load(std::memory_order_relaxed);
        result.crosshairCalls = g_state.crosshairCalls.load(std::memory_order_relaxed);
        result.defaultCrosshairCalls = g_state.defaultCrosshairCalls.load(std::memory_order_relaxed);
        return result;
    }

    void Shutdown()
    {
        ClearTarget();
        const std::uint32_t count = g_state.listenerHooks.exchange(0, std::memory_order_acq_rel);
        for (std::uint32_t index = 0; index < count && index < kMaxListenerHooks; ++index)
        {
            if (g_hookTargets[index])
                MH_RemoveHook(g_hookTargets[index]);
            g_hookTargets[index] = nullptr;
            g_originalListeners[index] = nullptr;
            g_listenerKinds[index] = ListenerHookKind::Unknown;
        }
        g_weaponShootClass = nullptr;
        if (g_queueHookTarget)
            MH_RemoveHook(g_queueHookTarget);
        g_queueHookTarget = nullptr;
        g_originalQueueEventInternal = nullptr;
        g_state.queueHookCreated.store(false, std::memory_order_release);
        const std::uint32_t producerCount = g_state.producerHooks.exchange(0, std::memory_order_acq_rel);
        for (std::uint32_t index = 0; index < producerCount && index < kMaxProducerHooks; ++index)
        {
            if (g_producerHookTargets[index])
                MH_RemoveHook(g_producerHookTargets[index]);
            g_producerHookTargets[index] = nullptr;
            g_originalProducerHandlers[index] = nullptr;
            g_producerHookKinds[index] = ProducerHookKind::Unknown;
        }
        g_state.hookCreated.store(false, std::memory_order_release);
        Diagnostics::Log("silent aim shutdown: callbacks=%llu queue=%llu projectile=%llu weapon=%llu local=%llu validated=%llu "
                         "redirected=%llu rejected=%llu effectRun=%llu attackStart=%llu attackPrepare=%llu "
                         "crosshair=%llu defaultCrosshair=%llu",
                         static_cast<unsigned long long>(g_state.callbacks.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.queueCallbacks.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.projectileEvents.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.weaponShootEvents.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.localPlayerEvents.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.validatedLocalEvents.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.redirectedShots.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.rejectedShots.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.effectRuns.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.attackStarts.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.attackPrepares.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.crosshairCalls.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.defaultCrosshairCalls.load(std::memory_order_relaxed)));
    }
}
