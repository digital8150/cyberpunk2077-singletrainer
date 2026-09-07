#include "silent_aim.h"

#include "projection.h"
#include "rtti_invoker.h"
#include "signature_scanner.h"
#include "../diagnostics.h"
#include "../framework.h"
#include "../hooks/hook_lifecycle.h"

#include <MinHook.h>

#include <algorithm>
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
    constexpr std::size_t kShootLocalToWorldOffset = 0xB0;
    constexpr std::size_t kShootStartPointOffset = 0xF0;
    constexpr std::size_t kShootStartVelocityOffset = 0x100;
    constexpr std::size_t kShootWeaponVelocityOffset = 0x110;
    constexpr std::size_t kShootParamsOffset = 0x120;
    constexpr std::size_t kSetUpOwnerOffset = 0x40;
    constexpr ULONGLONG kTargetTimeoutMilliseconds = 350;
    constexpr ULONGLONG kProjectileGracePeriodMs = 1200;
    // Cyberpunk 2077 projectile simulation gravity for throwing knives and axes (from tweakDB knife_params/axe_params).
    constexpr float kProjectileGravity = 20.0f;
    // Live projectile redirection for throwing knives/axes via gameprojectileShootEvent listener.
    constexpr bool kEnableProjectileMutation = true;
    // Projectile ShootEvent listener hook on gameprojectileComponent (dispatched via setUpEventId listener).
    constexpr bool kEnableProjectileObservationHooks = true;
    // QueueEvent hook on entIEntity for intercepting spawner and shoot events directly.
    constexpr bool kEnableQueueHook = true;
    // Native RTTI handlers have one documented VM ABI. These hooks only count calls while a target is armed;
    // they do not inspect stack-frame parameters or modify effect/crosshair data.
    constexpr bool kEnableProducerObservationHooks = false;
    // Hooking the two RTTI event-108 callback targets is unsafe: those code targets are reused outside the typed
    // listener dispatch, and the live game crashed before a valid weapon payload was observed.
    constexpr bool kEnableWeaponListenerObservationHooks = false;
    constexpr std::uint64_t kShootEventType = Game::Rtti::Hash("gameprojectileShootEvent");
    constexpr std::uint64_t kShootTargetEventType = Game::Rtti::Hash("gameprojectileShootTargetEvent");
    constexpr std::uint64_t kSetUpEventType = Game::Rtti::Hash("gameprojectileSetUpEvent");
    constexpr std::uint64_t kSpawnerLaunchEventType = Game::Rtti::Hash("gameprojectileSpawnerLaunchEvent");
    constexpr std::size_t kSpawnerLogicalOrientOffset = 0x58;
    constexpr std::size_t kSpawnerVisualOrientOffset = 0x78;
    constexpr std::size_t kSpawnerOwnerOffset = 0xA8;
    constexpr std::size_t kSpawnerTargetPosOffset = 0xD0;
    constexpr std::uint64_t kWeaponShootEventType = Game::Rtti::Hash("gameweaponeventsShootEvent");
    constexpr std::uint64_t kPlayerPuppetType = Game::Rtti::Hash("PlayerPuppet");
    constexpr std::uint64_t kGamePlayerPuppetType = Game::Rtti::Hash("gamePlayerPuppet");
    constexpr std::uint64_t kWeaponObjectType = Game::Rtti::Hash("gameweaponObject");
    constexpr std::uint64_t kItemObjectType = Game::Rtti::Hash("gameItemObject");
    constexpr std::uint32_t kRttiSystemGetAddressHash = 0x4A610F64u;
    constexpr std::uint8_t kQueueEventInternalPattern[] = {
        0x48, 0x83, 0xEC, 0x28, 0x8A, 0x81, 0x56, 0x01, 0x00, 0x00, 0x2C, 0x06, 0x3C, 0x01, 0x76, 0x00,
        0x48, 0x81, 0xC1, 0xD8, 0x00, 0x00, 0x00, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x28, 0xC3,
    };
    constexpr char kQueueEventInternalMask[] = "xxxxxxxxxxxxxxx?xxxxxxxx????xxxxx";
    static_assert(sizeof(kQueueEventInternalPattern) == sizeof(kQueueEventInternalMask) - 1);
    // TargetingSystem::GetCrosshairData native wrapper on Cyberpunk 2077 2.31. Its call at +0x3C
    // resolves the shared native core used by ordinary firearm shots.
    constexpr std::uint8_t kNativeCrosshairPattern[] = {
        0x4C, 0x8B, 0xDC, 0x48, 0x83, 0xEC, 0x68, 0x0F, 0x28, 0x05, 0x00, 0x00, 0x00, 0x00,
        0x8A, 0x84, 0x24, 0x90, 0x00, 0x00, 0x00, 0x88, 0x44, 0x24, 0x38, 0x49, 0x8D, 0x43,
        0xD8, 0x4D, 0x89, 0x4B, 0xC8, 0x4D, 0x8D, 0x4B, 0xE8,
    };
    constexpr char kNativeCrosshairMask[] = "xxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxx";
    static_assert(sizeof(kNativeCrosshairPattern) == sizeof(kNativeCrosshairMask) - 1);
    // entFuncOrientationProvider::GetOrientation (slot 33) pattern on Cyberpunk 2077 2.31.
    // Evaluates launch quaternion for throwing knives/axes spawned from screen center.
    constexpr std::uint8_t kFuncOrientationGetPattern[] = {
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x49, 0x78,
        0x48, 0x8B, 0xDA, 0x48, 0x85, 0xC9, 0x74, 0x00, 0x48, 0x8B,
        0x01, 0x48, 0x8D, 0x54,
    };
    constexpr char kFuncOrientationGetMask[] = "xxxxxxxxxxxxxxxxx?xxxxxx";
    static_assert(sizeof(kFuncOrientationGetPattern) == sizeof(kFuncOrientationGetMask) - 1);

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

    struct MatrixLayout
    {
        Vector4Layout x; // 0x00 Right
        Vector4Layout y; // 0x10 Forward
        Vector4Layout z; // 0x20 Up
        Vector4Layout w; // 0x30 Translation
    };
    static_assert(sizeof(MatrixLayout) == 0x40);

    // Native REDengine event listeners use Callback<void(IScriptable&, Handle<IScriptable>&)> with an unbound
    // function target. Its shared invoke thunk unwraps Handle::instance before tail-calling this target, so the
    // verified native ABI is (listener instance, event instance), not (listener instance, Handle*).
    using ListenerFn = void (*)(void*, void*);
    using NativeHandlerFn = void (*)(void*, void*, void*, void*);
    using QueueEventInternalFn = void (*)(void*, Game::Rtti::Handle*);
    using NativeCrosshairCoreFn = void (*)(void*, Vector4Layout*, Vector4Layout*, Vector4Layout*,
                                           void*, float, void*, bool);
    using FuncOrientationGetFn = Vector4Layout* (*)(void*, Vector4Layout*);
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
        std::atomic_bool crosshairCoreHookCreated{false};
        std::atomic_bool orientationHookCreated{false};
        std::atomic_uint32_t listenerHooks{0};
        std::atomic_bool targetActive{false};
        std::atomic<float> targetX{0.0f};
        std::atomic<float> targetY{0.0f};
        std::atomic<float> targetZ{0.0f};
        std::atomic<float> targetVx{0.0f};
        std::atomic<float> targetVy{0.0f};
        std::atomic<float> targetVz{0.0f};
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
        std::atomic_uint64_t nativeCrosshairCoreCalls{0};
        std::atomic_uint64_t nativeCrosshairCoreRedirects{0};
        std::atomic_uint64_t spawnerLaunchEvents{0};
        std::atomic_uint64_t spawnerLaunchRedirects{0};
        std::atomic<void*> spawnerLogicalProvider{nullptr};
        std::atomic<void*> spawnerVisualProvider{nullptr};
        std::atomic<float> spawnerQuatX{0.0f};
        std::atomic<float> spawnerQuatY{0.0f};
        std::atomic<float> spawnerQuatZ{0.0f};
        std::atomic<float> spawnerQuatW{1.0f};
        std::atomic<ULONGLONG> spawnerArmedAt{0};
        std::atomic_uint64_t orientationRedirects{0};
        std::atomic<float> lastCameraX{0.0f};
        std::atomic<float> lastCameraY{0.0f};
        std::atomic<float> lastCameraZ{0.0f};
        std::atomic_bool lastCameraValid{false};
        std::atomic<float> projectileGravityMultiplier{1.0f};
    };

    State g_state;
    std::int16_t g_spawnerLaunchEventId = -1;
    std::int16_t g_shootEventId = -1;
    std::int16_t g_shootTargetEventId = -1;
    std::int16_t g_weaponShootEventId = -1;
    std::array<void*, kMaxListenerHooks> g_hookTargets{};
    std::array<ListenerFn, kMaxListenerHooks> g_originalListeners{};
    std::array<ListenerHookKind, kMaxListenerHooks> g_listenerKinds{};
    const Game::Rtti::Class* g_weaponShootClass = nullptr;
    void* g_queueHookTarget = nullptr;
    QueueEventInternalFn g_originalQueueEventInternal = nullptr;
    void* g_nativeCrosshairCoreTarget = nullptr;
    NativeCrosshairCoreFn g_originalNativeCrosshairCore = nullptr;
    void* g_funcOrientationGetTarget = nullptr;
    FuncOrientationGetFn g_originalFuncOrientationGet = nullptr;
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

    bool ReadTarget(float output[3], float outVelocity[3] = nullptr, bool allowGracePeriod = false)
    {
        const bool active = g_state.targetActive.load(std::memory_order_acquire);
        if (!active && !allowGracePeriod)
            return false;
        const ULONGLONG publishedAt = g_state.targetPublishedAt.load(std::memory_order_acquire);
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG maxAge = allowGracePeriod ? kProjectileGracePeriodMs : kTargetTimeoutMilliseconds;
        if (publishedAt == 0 || now < publishedAt || now - publishedAt > maxAge)
            return false;
        for (unsigned attempt = 0; attempt < 3; ++attempt)
        {
            const std::uint64_t before = g_state.targetGeneration.load(std::memory_order_acquire);
            if ((before & 1u) != 0)
                continue;
            output[0] = g_state.targetX.load(std::memory_order_relaxed);
            output[1] = g_state.targetY.load(std::memory_order_relaxed);
            output[2] = g_state.targetZ.load(std::memory_order_relaxed);
            if (outVelocity)
            {
                outVelocity[0] = g_state.targetVx.load(std::memory_order_relaxed);
                outVelocity[1] = g_state.targetVy.load(std::memory_order_relaxed);
                outVelocity[2] = g_state.targetVz.load(std::memory_order_relaxed);
            }
            const std::uint64_t after = g_state.targetGeneration.load(std::memory_order_acquire);
            if (before == after)
            {
                return std::isfinite(output[0]) && std::isfinite(output[1]) && std::isfinite(output[2]);
            }
        }
        return false;
    }

    bool IsPlayerOwner(void* owner)
    {
        if (!owner)
            return false;
        const Game::Rtti::Class* type = Game::Rtti::NativeType(owner);
        if (!type)
            return false;
        if (Game::Rtti::IsClassOrDerived(type, kPlayerPuppetType) ||
            Game::Rtti::IsClassOrDerived(type, kGamePlayerPuppetType))
            return true;
        if (Game::Rtti::IsClassOrDerived(type, kWeaponObjectType) ||
            Game::Rtti::IsClassOrDerived(type, kItemObjectType))
            return true;
        return false;
    }

    bool RedirectNativeCrosshair(Vector4Layout* origin, Vector4Layout* direction)
    {
        if (!origin || !direction)
            return false;
        float target[3]{};
        if (!ReadTarget(target))
            return false;

        const float dx = target[0] - origin->x;
        const float dy = target[1] - origin->y;
        const float dz = target[2] - origin->z;
        const float lengthSquared = dx * dx + dy * dy + dz * dz;
        if (!std::isfinite(lengthSquared) || lengthSquared < 0.01f)
            return false;

        const float inverseLength = 1.0f / std::sqrt(lengthSquared);
        direction->x = dx * inverseLength;
        direction->y = dy * inverseLength;
        direction->z = dz * inverseLength;
        direction->w = 0.0f;
        return true;
    }

    bool RedirectNativeCrosshairSafely(Vector4Layout* origin, Vector4Layout* direction)
    {
        __try
        {
            return RedirectNativeCrosshair(origin, direction);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_state.rejectedShots.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    void HookNativeCrosshairCore(void* targetingSystem, Vector4Layout* origin,
                                 Vector4Layout* direction, Vector4Layout* crosshairPosition,
                                 void* queryContext, float maxDistance, void* filter, bool useRaycast)
    {
        HookLifecycle::CallbackGuard guard;
        if (g_originalNativeCrosshairCore)
        {
            g_originalNativeCrosshairCore(targetingSystem, origin, direction, crosshairPosition,
                                          queryContext, maxDistance, filter, useRaycast);
        }
        if (HookLifecycle::IsShuttingDown())
            return;

        if (origin && std::isfinite(origin->x) && std::isfinite(origin->y) && std::isfinite(origin->z))
        {
            g_state.lastCameraX.store(origin->x, std::memory_order_relaxed);
            g_state.lastCameraY.store(origin->y, std::memory_order_relaxed);
            g_state.lastCameraZ.store(origin->z, std::memory_order_relaxed);
            g_state.lastCameraValid.store(true, std::memory_order_release);
        }

        const bool redirected = RedirectNativeCrosshairSafely(origin, direction);

        const std::uint64_t count =
            g_state.nativeCrosshairCoreCalls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (redirected)
            g_state.nativeCrosshairCoreRedirects.fetch_add(1, std::memory_order_relaxed);
        if (redirected && (count <= 12 || (count & (count - 1)) == 0))
        {
            Diagnostics::Log("silent aim crosshair redirected: count=%llu raycast=%u "
                             "origin=(%.3f,%.3f,%.3f) direction=(%.6f,%.6f,%.6f)",
                             static_cast<unsigned long long>(count), useRaycast ? 1u : 0u,
                             origin->x, origin->y, origin->z,
                             direction->x, direction->y, direction->z);
        }
    }

    bool AddNativeCrosshairCoreHook()
    {
        const Game::Signatures::ScanResult scan = Game::Signatures::FindInText(
            GetModuleHandleW(L"Cyberpunk2077.exe"), kNativeCrosshairPattern, kNativeCrosshairMask,
            sizeof(kNativeCrosshairPattern));
        Diagnostics::Log("silent aim native crosshair wrapper scan: matches=%zu target=%p",
                         scan.matches, scan.address);
        if (scan.matches != 1 || !scan.address)
            return false;

        void* coreTarget = nullptr;
        __try
        {
            const auto* call = static_cast<const std::uint8_t*>(scan.address) + 0x3C;
            if (*call == 0xE8)
            {
                std::int32_t displacement = 0;
                std::memcpy(&displacement, call + 1, sizeof(displacement));
                coreTarget = const_cast<std::uint8_t*>(call + 5 + displacement);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            coreTarget = nullptr;
        }
        if (!IsExecutableAddress(coreTarget))
        {
            Diagnostics::Log("silent aim native crosshair core resolution failed: wrapper=%p target=%p",
                             scan.address, coreTarget);
            return false;
        }

        const MH_STATUS status = MH_CreateHook(
            coreTarget, &HookNativeCrosshairCore,
            reinterpret_cast<void**>(&g_originalNativeCrosshairCore));
        if (status != MH_OK)
        {
            Diagnostics::Log("MH_CreateHook(silent aim crosshair core) failed: target=%p status=%s (%d)",
                             coreTarget, MH_StatusToString(status), status);
            return false;
        }
        g_nativeCrosshairCoreTarget = coreTarget;
        g_state.crosshairCoreHookCreated.store(true, std::memory_order_release);
        Diagnostics::Log("silent aim native crosshair core hook created: target=%p original=%p mutation=1",
                         coreTarget, reinterpret_cast<void*>(g_originalNativeCrosshairCore));
        return true;
    }

    Vector4Layout* HookFuncOrientationGet(void* thisPtr, Vector4Layout* outQuat)
    {
        HookLifecycle::CallbackGuard guard;
        Vector4Layout* result = nullptr;
        if (g_originalFuncOrientationGet)
            result = g_originalFuncOrientationGet(thisPtr, outQuat);
        if (HookLifecycle::IsShuttingDown())
            return result;

        if (!outQuat)
            return result;

        const ULONGLONG armedAt = g_state.spawnerArmedAt.load(std::memory_order_acquire);
        const ULONGLONG now = GetTickCount64();
        if (armedAt == 0 || now < armedAt || now - armedAt > kTargetTimeoutMilliseconds)
            return result;

        void* logical = g_state.spawnerLogicalProvider.load(std::memory_order_acquire);
        void* visual = g_state.spawnerVisualProvider.load(std::memory_order_acquire);
        if ((logical != nullptr && thisPtr == logical) || (visual != nullptr && thisPtr == visual))
        {
            outQuat->x = g_state.spawnerQuatX.load(std::memory_order_relaxed);
            outQuat->y = g_state.spawnerQuatY.load(std::memory_order_relaxed);
            outQuat->z = g_state.spawnerQuatZ.load(std::memory_order_relaxed);
            outQuat->w = g_state.spawnerQuatW.load(std::memory_order_relaxed);

            const std::uint64_t count =
                g_state.orientationRedirects.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count <= 12 || (count & (count - 1)) == 0)
            {
                Diagnostics::Log("silent aim orientation provider redirected: count=%llu provider=%p "
                                 "(logical=%p visual=%p) quat=(%.4f,%.4f,%.4f,%.4f)",
                                 static_cast<unsigned long long>(count), thisPtr, logical, visual,
                                 outQuat->x, outQuat->y, outQuat->z, outQuat->w);
            }
        }
        return result;
    }

    bool AddOrientationProviderHook()
    {
        const Game::Signatures::ScanResult scan = Game::Signatures::FindInText(
            GetModuleHandleW(L"Cyberpunk2077.exe"), kFuncOrientationGetPattern, kFuncOrientationGetMask,
            sizeof(kFuncOrientationGetPattern));
        Diagnostics::Log("silent aim func orientation get scan: matches=%zu target=%p",
                         scan.matches, scan.address);
        if (scan.matches != 1 || !scan.address)
            return false;

        const MH_STATUS status = MH_CreateHook(
            scan.address, reinterpret_cast<void*>(&HookFuncOrientationGet),
            reinterpret_cast<void**>(&g_originalFuncOrientationGet));
        if (status != MH_OK)
        {
            Diagnostics::Log("MH_CreateHook(silent aim func orientation get) failed: target=%p status=%s (%d)",
                             scan.address, MH_StatusToString(status), status);
            return false;
        }
        g_funcOrientationGetTarget = scan.address;
        g_state.orientationHookCreated.store(true, std::memory_order_release);
        Diagnostics::Log("silent aim func orientation get hook created: target=%p original=%p mutation=1",
                         scan.address, reinterpret_cast<void*>(g_originalFuncOrientationGet));
        return true;
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

    bool CalculateBallisticTrajectory(const Vector4Layout& start, const float target[3],
                                      float speed, float gravity, Vector4Layout& outVelocity, MatrixLayout& outOrientation)
    {
        const float dx = target[0] - start.x;
        const float dy = target[1] - start.y;
        const float dz = target[2] - start.z;
        const float distHorizontalSq = dx * dx + dy * dy;
        const float distHorizontal = std::sqrt(distHorizontalSq);
        const float distTotalSq = distHorizontalSq + dz * dz;
        const float distTotal = std::sqrt(distTotalSq);

        if (!std::isfinite(distTotal) || distTotal < 0.05f || !std::isfinite(speed) || speed < 0.01f)
            return false;

        float vx = 0.0f;
        float vy = 0.0f;
        float vz = 0.0f;

        const float g = (std::isfinite(gravity) && gravity >= 0.0f) ? gravity : kProjectileGravity;
        if (g < 0.001f)
        {
            // Zero-gravity weapon modification (e.g. "무중력" mod): straight line directly to target.
            const float scale = speed / distTotal;
            vx = dx * scale;
            vy = dy * scale;
            vz = dz * scale;
        }
        else
        {
            const float A = 0.25f * g * g;
            const float B = g * dz - speed * speed;
            const float C = distTotalSq;
            const float disc = B * B - 4.0f * A * C;

            if (disc >= 0.0f && distHorizontal > 0.05f)
            {
                const float u = (-B - std::sqrt(disc)) / (2.0f * A);
                if (u > 0.0001f)
                {
                    const float t = std::sqrt(u);
                    vx = dx / t;
                    vy = dy / t;
                    vz = dz / t + 0.5f * g * t;
                }
                else
                {
                    const float scale = speed / distTotal;
                    vx = dx * scale;
                    vy = dy * scale;
                    vz = dz * scale;
                }
            }
            else
            {
                const float scale = speed / distTotal;
                vx = dx * scale;
                vy = dy * scale;
                vz = dz * scale;
            }
        }

        outVelocity.x = vx;
        outVelocity.y = vy;
        outVelocity.z = vz;
        outVelocity.w = 0.0f;

        // REDengine basis: +X Right, +Y Forward, +Z Up, +W Translation.
        const float invSpeed = 1.0f / speed;
        const float fx = vx * invSpeed;
        const float fy = vy * invSpeed;
        const float fz = vz * invSpeed;

        const float horizLen = std::sqrt(fx * fx + fy * fy);
        float rx = 0.0f;
        float ry = 0.0f;
        float rz = 0.0f;
        float ux = 0.0f;
        float uy = 0.0f;
        float uz = 0.0f;

        if (horizLen > 0.001f)
        {
            const float invH = 1.0f / horizLen;
            rx = fy * invH;
            ry = -fx * invH;
            rz = 0.0f;

            ux = ry * fz;
            uy = -rx * fz;
            uz = horizLen;
        }
        else
        {
            rx = 1.0f;
            ry = 0.0f;
            rz = 0.0f;
            if (fz > 0.0f)
            {
                ux = 0.0f;
                uy = -1.0f;
                uz = 0.0f;
            }
            else
            {
                ux = 0.0f;
                uy = 1.0f;
                uz = 0.0f;
            }
        }

        outOrientation.x = {rx, ry, rz, 0.0f};
        outOrientation.y = {fx, fy, fz, 0.0f};
        outOrientation.z = {ux, uy, uz, 0.0f};
        outOrientation.w = {start.x, start.y, start.z, 1.0f};

        return true;
    }

    Vector4Layout MatrixToQuaternion(const MatrixLayout& m)
    {
        // Shepperd's algorithm for converting rotation matrix to unit quaternion.
        // Row 0: right (rx, ry, rz)
        // Row 1: forward (fx, fy, fz)
        // Row 2: up (ux, uy, uz)
        const float m00 = m.x.x;
        const float m01 = m.x.y;
        const float m02 = m.x.z;

        const float m10 = m.y.x;
        const float m11 = m.y.y;
        const float m12 = m.y.z;

        const float m20 = m.z.x;
        const float m21 = m.z.y;
        const float m22 = m.z.z;

        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 1.0f;

        const float trace = m00 + m11 + m22;
        if (trace > 0.0f)
        {
            const float s = std::sqrt(trace + 1.0f) * 2.0f;
            w = 0.25f * s;
            x = (m12 - m21) / s;
            y = (m20 - m02) / s;
            z = (m01 - m10) / s;
        }
        else if (m00 > m11 && m00 > m22)
        {
            const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
            w = (m12 - m21) / s;
            x = 0.25f * s;
            y = (m10 + m01) / s;
            z = (m20 + m02) / s;
        }
        else if (m11 > m22)
        {
            const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
            w = (m20 - m02) / s;
            x = (m10 + m01) / s;
            y = 0.25f * s;
            z = (m21 + m12) / s;
        }
        else
        {
            const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
            w = (m01 - m10) / s;
            x = (m20 + m02) / s;
            y = (m21 + m12) / s;
            z = 0.25f * s;
        }

        const float lenSq = x * x + y * y + z * z + w * w;
        if (lenSq > 0.000001f)
        {
            const float invLen = 1.0f / std::sqrt(lenSq);
            x *= invLen;
            y *= invLen;
            z *= invLen;
            w *= invLen;
        }
        else
        {
            x = 0.0f;
            y = 0.0f;
            z = 0.0f;
            w = 1.0f;
        }
        return {x, y, z, w};
    }

    void HandleSpawnerLaunchEvent(void* entity, void* event)
    {
        if (!event || !kEnableProjectileMutation)
            return;

        __try
        {
            auto* bytes = static_cast<std::byte*>(event);
            const auto* owner = reinterpret_cast<const Game::Rtti::Handle*>(bytes + kSpawnerOwnerOffset);
            void* ownerInstance = owner ? owner->instance : nullptr;
            const bool isPlayer = IsPlayerOwner(ownerInstance);
            if (!owner || !isPlayer)
            {
                static std::atomic_uint32_t s_rejectedOwner{0};
                const std::uint32_t count = s_rejectedOwner.fetch_add(1, std::memory_order_relaxed) + 1;
                if (count <= 16)
                {
                    const auto* type = ownerInstance ? Game::Rtti::NativeType(ownerInstance) : nullptr;
                    const auto* classLayout = reinterpret_cast<const ClassLayout*>(type);
                    Diagnostics::Log("silent aim spawner launch rejected owner: count=%u owner=%p inst=%p type=%016llX",
                                     count, owner, ownerInstance,
                                     static_cast<unsigned long long>(classLayout ? classLayout->nameHash : 0));
                }
                return;
            }

            float target[3]{};
            float targetVel[3]{};
            if (!ReadTarget(target, targetVel, true /* allowGracePeriod */))
            {
                static std::atomic_uint32_t s_rejectedTarget{0};
                const std::uint32_t count = s_rejectedTarget.fetch_add(1, std::memory_order_relaxed) + 1;
                if (count <= 16)
                {
                    const ULONGLONG publishedAt = g_state.targetPublishedAt.load(std::memory_order_acquire);
                    const ULONGLONG now = GetTickCount64();
                    Diagnostics::Log("silent aim spawner launch rejected target: count=%u active=%d publishedAt=%llu age=%llu",
                                     count, g_state.targetActive.load(std::memory_order_relaxed) ? 1 : 0,
                                     publishedAt, publishedAt == 0 ? 0 : (now >= publishedAt ? now - publishedAt : 0));
                }
                return;
            }

            void* logicalProvider = nullptr;
            void* visualProvider = nullptr;
            const auto* logicalHandle = reinterpret_cast<const Game::Rtti::Handle*>(bytes + kSpawnerLogicalOrientOffset);
            if (logicalHandle)
                logicalProvider = logicalHandle->instance;
            const auto* visualHandle = reinterpret_cast<const Game::Rtti::Handle*>(bytes + kSpawnerVisualOrientOffset);
            if (visualHandle)
                visualProvider = visualHandle->instance;

            Vector4Layout start{};
            bool startValid = false;
            if (g_state.lastCameraValid.load(std::memory_order_acquire))
            {
                start.x = g_state.lastCameraX.load(std::memory_order_relaxed);
                start.y = g_state.lastCameraY.load(std::memory_order_relaxed);
                start.z = g_state.lastCameraZ.load(std::memory_order_relaxed);
                start.w = 1.0f;
                startValid = true;
            }
            if (!startValid)
            {
                float camPos[3]{};
                if (Game::Projection::GetCameraPosition(camPos))
                {
                    start.x = camPos[0];
                    start.y = camPos[1];
                    start.z = camPos[2];
                    start.w = 1.0f;
                    startValid = true;
                }
            }

            constexpr float kDefaultKnifeSpeed = 110.0f;
            float leadTarget[3] = {target[0], target[1], target[2]};
            const float vx = targetVel[0];
            const float vy = targetVel[1];
            const float vz = targetVel[2];
            const float targetSpeedSq = vx * vx + vy * vy + vz * vz;
            if (startValid && targetSpeedSq > 0.01f && targetSpeedSq < 900.0f)
            {
                const float dx0 = target[0] - start.x;
                const float dy0 = target[1] - start.y;
                const float dz0 = target[2] - start.z;
                const float dist0 = std::sqrt(dx0 * dx0 + dy0 * dy0 + dz0 * dz0);
                if (dist0 > 0.1f)
                {
                    float tFlight = dist0 / kDefaultKnifeSpeed;
                    const float px1 = target[0] + vx * tFlight;
                    const float py1 = target[1] + vy * tFlight;
                    const float pz1 = target[2] + vz * tFlight;
                    const float dx1 = px1 - start.x;
                    const float dy1 = py1 - start.y;
                    const float dz1 = pz1 - start.z;
                    const float dist1 = std::sqrt(dx1 * dx1 + dy1 * dy1 + dz1 * dz1);
                    tFlight = std::clamp(dist1 / kDefaultKnifeSpeed, 0.0f, 1.5f);

                    leadTarget[0] = target[0] + vx * tFlight;
                    leadTarget[1] = target[1] + vy * tFlight;
                    leadTarget[2] = target[2] + vz * tFlight;
                }
            }

            auto* targetPos = reinterpret_cast<Vector4Layout*>(bytes + kSpawnerTargetPosOffset);
            targetPos->x = leadTarget[0];
            targetPos->y = leadTarget[1];
            targetPos->z = leadTarget[2];
            targetPos->w = 1.0f;

            if (startValid)
            {
                Vector4Layout launchVel{};
                MatrixLayout launchRot{};
                const float mult = g_state.projectileGravityMultiplier.load(std::memory_order_relaxed);
                const float effectiveGravity = kProjectileGravity * ((std::isfinite(mult) && mult >= 0.0f) ? mult : 1.0f);
                if (CalculateBallisticTrajectory(start, leadTarget, kDefaultKnifeSpeed, effectiveGravity, launchVel, launchRot))
                {
                    const Vector4Layout quat = MatrixToQuaternion(launchRot);
                    g_state.spawnerQuatX.store(quat.x, std::memory_order_relaxed);
                    g_state.spawnerQuatY.store(quat.y, std::memory_order_relaxed);
                    g_state.spawnerQuatZ.store(quat.z, std::memory_order_relaxed);
                    g_state.spawnerQuatW.store(quat.w, std::memory_order_relaxed);
                    g_state.spawnerLogicalProvider.store(logicalProvider, std::memory_order_release);
                    g_state.spawnerVisualProvider.store(visualProvider, std::memory_order_release);
                    g_state.spawnerArmedAt.store(GetTickCount64(), std::memory_order_release);
                }
            }

            g_state.spawnerLaunchRedirects.fetch_add(1, std::memory_order_relaxed);
            const std::uint64_t redirected = g_state.redirectedShots.fetch_add(1, std::memory_order_relaxed) + 1;
            if (redirected <= 8 || (redirected % 16u) == 0)
            {
                const float mult = g_state.projectileGravityMultiplier.load(std::memory_order_relaxed);
                Diagnostics::Log("silent aim spawner launch redirected: count=%llu entity=%p owner=%p "
                                 "target=(%.2f,%.2f,%.2f) lead=(%.2f,%.2f,%.2f) vel=(%.2f,%.2f,%.2f) "
                                 "logical=%p visual=%p gMult=%.2f",
                                 static_cast<unsigned long long>(redirected), entity, ownerInstance,
                                 target[0], target[1], target[2],
                                 leadTarget[0], leadTarget[1], leadTarget[2],
                                 vx, vy, vz,
                                 logicalProvider, visualProvider, mult);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_state.rejectedShots.fetch_add(1, std::memory_order_relaxed);
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
        float targetVel[3]{};
        if (!ReadTarget(target, targetVel, true /* allowGracePeriod */))
            return;

        const auto* start = reinterpret_cast<const Vector4Layout*>(bytes + kShootStartPointOffset);
        auto* velocity = reinterpret_cast<Vector4Layout*>(bytes + kShootStartVelocityOffset);
        float speed = std::sqrt(velocity->x * velocity->x + velocity->y * velocity->y +
                                velocity->z * velocity->z);
        if (!std::isfinite(speed) || speed < 0.01f)
        {
            speed = 110.0f;
        }
        const float deltaX = target[0] - start->x;
        const float deltaY = target[1] - start->y;
        const float deltaZ = target[2] - start->z;
        const float distance = std::sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
        const bool vectorsPlausible = std::isfinite(start->x) && std::isfinite(start->y) &&
                                      std::isfinite(start->z) &&
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
                Diagnostics::Log("silent aim live layout validated: event=%p owner=%p "
                                 "start=(%.2f,%.2f,%.2f) velocity=(%.2f,%.2f,%.2f) speed=%.2f",
                                 event, owner->instance, start->x, start->y, start->z,
                                 velocity->x, velocity->y, velocity->z, speed);
            }
        }

        if (!kEnableProjectileMutation)
            return;

        float leadTarget[3] = {target[0], target[1], target[2]};
        const float vx = targetVel[0];
        const float vy = targetVel[1];
        const float vz = targetVel[2];
        const float targetSpeedSq = vx * vx + vy * vy + vz * vz;
        if (targetSpeedSq > 0.01f && targetSpeedSq < 900.0f && distance > 0.1f)
        {
            float tFlight = distance / speed;
            const float px1 = target[0] + vx * tFlight;
            const float py1 = target[1] + vy * tFlight;
            const float pz1 = target[2] + vz * tFlight;
            const float dx1 = px1 - start->x;
            const float dy1 = py1 - start->y;
            const float dz1 = pz1 - start->z;
            const float dist1 = std::sqrt(dx1 * dx1 + dy1 * dy1 + dz1 * dz1);
            tFlight = std::clamp(dist1 / speed, 0.0f, 1.5f);

            leadTarget[0] = target[0] + vx * tFlight;
            leadTarget[1] = target[1] + vy * tFlight;
            leadTarget[2] = target[2] + vz * tFlight;
        }

        Vector4Layout newVelocity{};
        MatrixLayout newOrientation{};
        const float mult = g_state.projectileGravityMultiplier.load(std::memory_order_relaxed);
        const float effectiveGravity = kProjectileGravity * ((std::isfinite(mult) && mult >= 0.0f) ? mult : 1.0f);
        if (!CalculateBallisticTrajectory(*start, leadTarget, speed, effectiveGravity, newVelocity, newOrientation))
        {
            g_state.rejectedShots.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        *velocity = newVelocity;

        if (Game::Rtti::ClassSize(type) >= kShootLocalToWorldOffset + sizeof(MatrixLayout))
        {
            auto* localToWorld = reinterpret_cast<MatrixLayout*>(bytes + kShootLocalToWorldOffset);
            newOrientation.w = localToWorld->w;
            *localToWorld = newOrientation;
        }

        if (Game::Rtti::ClassSize(type) >= kShootWeaponVelocityOffset + sizeof(Vector4Layout))
        {
            auto* weaponVelocity = reinterpret_cast<Vector4Layout*>(bytes + kShootWeaponVelocityOffset);
            weaponVelocity->x = 0.0f;
            weaponVelocity->y = 0.0f;
            weaponVelocity->z = 0.0f;
            weaponVelocity->w = 0.0f;
        }

        if (Game::Rtti::ClassSize(type) >= kShootParamsOffset + sizeof(Vector4Layout))
        {
            auto* targetPos = reinterpret_cast<Vector4Layout*>(bytes + kShootParamsOffset);
            targetPos->x = leadTarget[0];
            targetPos->y = leadTarget[1];
            targetPos->z = leadTarget[2];
            targetPos->w = 1.0f;
        }

        const std::uint64_t redirected = g_state.redirectedShots.fetch_add(1, std::memory_order_relaxed) + 1;
        if (redirected <= 8 || (redirected % 16u) == 0)
        {
            Diagnostics::Log("silent aim redirected projectile: count=%llu owner=%p "
                             "start=(%.2f,%.2f,%.2f) target=(%.2f,%.2f,%.2f) lead=(%.2f,%.2f,%.2f) speed=%.2f "
                             "vel=(%.2f,%.2f,%.2f) fwd=(%.3f,%.3f,%.3f)",
                             static_cast<unsigned long long>(redirected), owner->instance,
                             start->x, start->y, start->z, target[0], target[1], target[2],
                             leadTarget[0], leadTarget[1], leadTarget[2], speed,
                             newVelocity.x, newVelocity.y, newVelocity.z,
                             newOrientation.y.x, newOrientation.y.y, newOrientation.y.z);
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
            if (!Game::Rtti::IsValidUserPointer(event))
                return;
            const auto* classLayout = *reinterpret_cast<const ClassLayout* const*>(
                static_cast<const std::byte*>(event) + 0x30);
            if (!Game::Rtti::IsValidUserPointer(classLayout))
                return;

            const std::int16_t eventId = classLayout->eventTypeId;
            if (eventId <= 0)
                return;

            if (g_spawnerLaunchEventId > 0 && eventId == g_spawnerLaunchEventId)
            {
                g_state.projectileEvents.fetch_add(1, std::memory_order_relaxed);
                g_state.spawnerLaunchEvents.fetch_add(1, std::memory_order_relaxed);
                HandleSpawnerLaunchEvent(entity, event);
            }
            else if ((g_shootEventId > 0 && eventId == g_shootEventId) ||
                     (g_shootTargetEventId > 0 && eventId == g_shootTargetEventId))
            {
                RedirectProjectileEvent(event);
            }
            else if (g_weaponShootEventId > 0 && eventId == g_weaponShootEventId)
            {
                ObserveWeaponShootEvent(event, g_weaponShootClass);
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
            created = AddNativeCrosshairCoreHook() || created;
            created = AddProducerObservationHook("gameEffectInstance", "Run", ProducerHookKind::EffectRun) || created;
            created = AddProducerObservationHook("gameAttack_GameEffect", "StartAttack",
                                                 ProducerHookKind::AttackStart) || created;
            created = AddProducerObservationHook("gameAttack_GameEffect", "PrepareAttack",
                                                 ProducerHookKind::AttackPrepare) || created;
            created = AddProducerObservationHook("gametargetingTargetingSystem", "GetCrosshairData",
                                                 ProducerHookKind::Crosshair) || created;
            created = AddProducerObservationHook("gametargetingTargetingSystem", "GetDefaultCrosshairData",
                                                 ProducerHookKind::DefaultCrosshair) || created;

            auto* spawnerLaunchEvent = reinterpret_cast<ClassLayout*>(Game::Rtti::GetClass(kSpawnerLaunchEventType));
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
                if (spawnerLaunchEvent)
                    g_spawnerLaunchEventId = spawnerLaunchEvent->eventTypeId;
                g_shootEventId = shootEvent->eventTypeId;
                g_shootTargetEventId = shootTargetEvent->eventTypeId;
                g_weaponShootEventId = weaponShootEvent->eventTypeId;

                Diagnostics::Log("silent aim projectile RTTI: spawnerLaunchId=%d setupId=%d shootId=%d shootTargetId=%d "
                                 "weaponShootId=%d componentListeners=%u",
                                 spawnerLaunchEvent ? spawnerLaunchEvent->eventTypeId : -1,
                                 setUpEvent->eventTypeId, shootEvent->eventTypeId, shootTargetEvent->eventTypeId,
                                 weaponShootEvent->eventTypeId, projectileComponent->listeners.size);
                g_weaponShootClass = reinterpret_cast<Game::Rtti::Class*>(weaponShootEvent);
                // Enumeration remains read-only. The two event-108 callback targets are deliberately never hooked.
                EnumerateWeaponShootListeners(weaponShootEvent->eventTypeId);
                if (kEnableProjectileObservationHooks)
                {
                    created = AddListenerHooks(projectileComponent, setUpEvent->eventTypeId,
                                               shootEvent->eventTypeId, shootTargetEvent->eventTypeId,
                                               ListenerHookKind::Projectile) || created;
                }
            }
            if (kEnableQueueHook)
            {
                created = AddQueueEventObservationHook() || created;
            }
            created = AddOrientationProviderHook() || created;
            g_state.hookCreated.store(created, std::memory_order_release);
            Diagnostics::Log("silent aim hooks created: producers=%u projectileListeners=%u "
                             "weaponListenerHooks=0 queueHook=%u crosshairCore=%u orientationHook=%u",
                             g_state.producerHooks.load(std::memory_order_relaxed),
                             g_state.listenerHooks.load(std::memory_order_relaxed),
                             g_state.queueHookCreated.load(std::memory_order_acquire) ? 1u : 0u,
                             g_state.crosshairCoreHookCreated.load(std::memory_order_acquire) ? 1u : 0u,
                             g_state.orientationHookCreated.load(std::memory_order_acquire) ? 1u : 0u);
            return created;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Diagnostics::Log("silent aim RTTI producer discovery raised an exception");
            return false;
        }
    }

    void SetProjectileGravityMultiplier(float multiplier)
    {
        g_state.projectileGravityMultiplier.store(multiplier, std::memory_order_release);
    }

    void PublishTarget(const float worldTarget[3], bool active, const float worldVelocity[3])
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
        if (worldVelocity && std::isfinite(worldVelocity[0]) &&
            std::isfinite(worldVelocity[1]) && std::isfinite(worldVelocity[2]))
        {
            g_state.targetVx.store(worldVelocity[0], std::memory_order_relaxed);
            g_state.targetVy.store(worldVelocity[1], std::memory_order_relaxed);
            g_state.targetVz.store(worldVelocity[2], std::memory_order_relaxed);
        }
        else
        {
            g_state.targetVx.store(0.0f, std::memory_order_relaxed);
            g_state.targetVy.store(0.0f, std::memory_order_relaxed);
            g_state.targetVz.store(0.0f, std::memory_order_relaxed);
        }
        g_state.targetPublishedAt.store(GetTickCount64(), std::memory_order_release);
        g_state.targetGeneration.fetch_add(1, std::memory_order_release);
        g_state.targetActive.store(true, std::memory_order_release);
    }

    void ClearTarget()
    {
        g_state.targetActive.store(false, std::memory_order_release);
        // Retain targetPublishedAt and target coordinates so in-flight projectile launch
        // animations (which fire 200-500ms after the player releases aim) can still acquire
        // the target during the 1200ms grace period.
    }

    DiagnosticsSnapshot GetDiagnostics()
    {
        DiagnosticsSnapshot result;
        result.hookCreated = g_state.hookCreated.load(std::memory_order_acquire);
        result.queueHookCreated = g_state.queueHookCreated.load(std::memory_order_acquire);
        result.crosshairCoreHookCreated = g_state.crosshairCoreHookCreated.load(std::memory_order_acquire);
        result.projectileHookCreated = g_state.queueHookCreated.load(std::memory_order_acquire) ||
                                       g_state.listenerHooks.load(std::memory_order_relaxed) > 0;
        result.orientationHookCreated = g_state.orientationHookCreated.load(std::memory_order_acquire);
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
        result.nativeCrosshairCoreCalls = g_state.nativeCrosshairCoreCalls.load(std::memory_order_relaxed);
        result.nativeCrosshairCoreRedirects =
            g_state.nativeCrosshairCoreRedirects.load(std::memory_order_relaxed);
        result.spawnerLaunchEvents = g_state.spawnerLaunchEvents.load(std::memory_order_relaxed);
        result.spawnerLaunchRedirects = g_state.spawnerLaunchRedirects.load(std::memory_order_relaxed);
        result.orientationRedirects = g_state.orientationRedirects.load(std::memory_order_relaxed);
        result.projectileGravityMultiplier =
            g_state.projectileGravityMultiplier.load(std::memory_order_relaxed);
        result.targetVx = g_state.targetVx.load(std::memory_order_relaxed);
        result.targetVy = g_state.targetVy.load(std::memory_order_relaxed);
        result.targetVz = g_state.targetVz.load(std::memory_order_relaxed);
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
        g_spawnerLaunchEventId = -1;
        g_shootEventId = -1;
        g_shootTargetEventId = -1;
        g_weaponShootEventId = -1;
        if (g_queueHookTarget)
            MH_RemoveHook(g_queueHookTarget);
        g_queueHookTarget = nullptr;
        g_originalQueueEventInternal = nullptr;
        g_state.queueHookCreated.store(false, std::memory_order_release);
        if (g_nativeCrosshairCoreTarget)
            MH_RemoveHook(g_nativeCrosshairCoreTarget);
        g_nativeCrosshairCoreTarget = nullptr;
        g_originalNativeCrosshairCore = nullptr;
        g_state.crosshairCoreHookCreated.store(false, std::memory_order_release);
        if (g_funcOrientationGetTarget)
            MH_RemoveHook(g_funcOrientationGetTarget);
        g_funcOrientationGetTarget = nullptr;
        g_originalFuncOrientationGet = nullptr;
        g_state.orientationHookCreated.store(false, std::memory_order_release);
        g_state.spawnerLogicalProvider.store(nullptr, std::memory_order_release);
        g_state.spawnerVisualProvider.store(nullptr, std::memory_order_release);
        g_state.spawnerArmedAt.store(0, std::memory_order_release);
        g_state.projectileGravityMultiplier.store(1.0f, std::memory_order_release);
        g_state.targetPublishedAt.store(0, std::memory_order_release);
        g_state.targetVx.store(0.0f, std::memory_order_release);
        g_state.targetVy.store(0.0f, std::memory_order_release);
        g_state.targetVz.store(0.0f, std::memory_order_release);
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
                         "crosshair=%llu defaultCrosshair=%llu nativeCrosshairCore=%llu "
                         "nativeCrosshairRedirects=%llu spawnerLaunchRedirects=%llu orientationRedirects=%llu",
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
                         static_cast<unsigned long long>(g_state.defaultCrosshairCalls.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.nativeCrosshairCoreCalls.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.nativeCrosshairCoreRedirects.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.spawnerLaunchRedirects.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.orientationRedirects.load(std::memory_order_relaxed)));
    }
}
