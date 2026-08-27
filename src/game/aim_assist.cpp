#include "aim_assist.h"
#include "rtti_invoker.h"
#include "signature_scanner.h"
#include "../diagnostics.h"
#include "../framework.h"
#include "../hooks/hook_lifecycle.h"

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace
{
    constexpr std::uint32_t kGameEngineAddressHash = 0x97F209D6u;
    constexpr std::uint32_t kRttiSystemGetAddressHash = 0x4A610F64u;
    constexpr float kRadiansToDegrees = 57.29577951308232f;
    constexpr float kFixedPointScale = 1.0f / static_cast<float>(2 << 16);

    // gameFPPCameraComponent::Update reads its yaw and pitch providers through this helper before applying camera
    // input. Hooking immediately after those reads lets the trainer replace the two live aim offsets without using
    // TargetingSystem::LookAt or any of the engine's AimRequest easing path. Verified on Cyberpunk 2077 2.31.
    constexpr std::uint8_t kReadAimOffsetsPattern[] = {
        0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68,
        0x18, 0x48, 0x89, 0x70, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20,
        0x48, 0x8B, 0x99, 0x80, 0x03, 0x00, 0x00, 0x48, 0x8B, 0xF1,
        0x48, 0x8D, 0x48, 0x08, 0x49, 0x8B, 0xE8, 0x48, 0x8B, 0xFA,
    };
    constexpr char kReadAimOffsetsMask[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    static_assert(sizeof(kReadAimOffsetsPattern) == sizeof(kReadAimOffsetsMask) - 1);

    constexpr std::size_t kEntityComponentsOffset = 0xA0;
    constexpr std::size_t kComponentWorldTransformOffset = 0xE0;

    struct HandleLayout
    {
        void* instance;
        struct RefCountLayout
        {
            volatile LONG strong;
            volatile LONG weak;
        }* refCount;
    };
    static_assert(sizeof(HandleLayout) == 0x10);

    struct ComponentArray
    {
        std::byte* entries;
        std::uint32_t capacity;
        std::uint32_t size;
    };

    struct WorldTransformLayout
    {
        std::int32_t x;
        std::int32_t y;
        std::int32_t z;
        std::byte pad0C[4];
        float orientation[4];
    };
    static_assert(sizeof(WorldTransformLayout) == 0x20);

    using ResolveAddressFn = std::uintptr_t (*)(std::uint32_t);
    using GetRttiSystemFn = void* (*)();
    using GetClassFn = void* (*)(void*, std::uint64_t);
    using GetSystemFn = void* (*)(void*, void*);
    using ReadAimOffsetsFn = void (*)(void*, float*, float*);

    struct State
    {
        bool playerResolverAttempted = false;
        void* playerSystem = nullptr;
        Game::Rtti::Function* getLocalPlayer = nullptr;
        std::atomic<void*> fppCamera{nullptr};
        ULONGLONG lastPlayerResolveTick = 0;

        std::atomic_bool hookCreated{false};
        std::atomic_uint64_t hookCallbacks{0};
        std::atomic_uint64_t appliedWrites{0};
        std::atomic_bool aimActive{false};
        std::atomic<float> targetX{0.0f};
        std::atomic<float> targetY{0.0f};
        std::atomic<float> targetZ{0.0f};
        std::atomic<float> smoothing{0.0f};
        std::atomic_uint64_t targetGeneration{0};
    };

    State g_state;
    ReadAimOffsetsFn g_originalReadAimOffsets = nullptr;
    ULONGLONG g_lastAimUpdateTick = 0;
    bool g_wasAimActive = false;
    bool g_hasLoggedAimMode = false;
    bool g_lastLoggedHardLock = false;
    void* g_validatedCamera = nullptr;
    bool g_hookObservedLogged = false;

    void* VirtualFunction(void* object, std::size_t index)
    {
        if (!object)
            return nullptr;
        void** table = *reinterpret_cast<void***>(object);
        return table ? table[index] : nullptr;
    }

    bool InitializePlayerResolver()
    {
        if (g_state.playerResolverAttempted)
            return g_state.playerSystem && g_state.getLocalPlayer;
        g_state.playerResolverAttempted = true;

        HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
        const auto resolve = red4ext
                                 ? reinterpret_cast<ResolveAddressFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                                 : nullptr;
        if (!resolve)
            return false;

        const std::uintptr_t enginePointerAddress = resolve(kGameEngineAddressHash);
        const std::uintptr_t rttiGetAddress = resolve(kRttiSystemGetAddressHash);
        void* engine = enginePointerAddress ? *reinterpret_cast<void**>(enginePointerAddress) : nullptr;
        void* framework = engine ? *reinterpret_cast<void**>(static_cast<std::byte*>(engine) + 0x308) : nullptr;
        void* gameInstance = framework ? *reinterpret_cast<void**>(static_cast<std::byte*>(framework) + 0x10) : nullptr;
        void* rttiSystem = rttiGetAddress ? reinterpret_cast<GetRttiSystemFn>(rttiGetAddress)() : nullptr;
        const auto getClass = reinterpret_cast<GetClassFn>(VirtualFunction(rttiSystem, 2));
        const auto getSystem = reinterpret_cast<GetSystemFn>(VirtualFunction(gameInstance, 1));
        void* playerSystemType = getClass ? getClass(rttiSystem, Game::Rtti::Hash("gameIPlayerSystem")) : nullptr;
        g_state.playerSystem = getSystem && playerSystemType ? getSystem(gameInstance, playerSystemType) : nullptr;
        g_state.getLocalPlayer = Game::Rtti::FindFunction(Game::Rtti::NativeType(g_state.playerSystem),
                                                          Game::Rtti::Hash("GetLocalPlayerControlledGameObject"));
        Diagnostics::Log("memory aim resolver: playerSystem=%p getPlayer=%p", g_state.playerSystem,
                         g_state.getLocalPlayer);
        return g_state.playerSystem && g_state.getLocalPlayer;
    }

    bool GetPlayerHandle(HandleLayout& player)
    {
        player = {};
        if (!Game::Rtti::Invoke(g_state.getLocalPlayer, g_state.playerSystem, nullptr, 0, &player))
            return false;
        return player.instance && player.refCount && player.refCount->strong > 0;
    }

    void ReleaseReturnedHandle(HandleLayout& handle)
    {
        if (handle.refCount)
        {
            LONG observed = InterlockedCompareExchange(&handle.refCount->strong, 0, 0);
            while (observed > 1)
            {
                const LONG previous = InterlockedCompareExchange(&handle.refCount->strong, observed - 1, observed);
                if (previous == observed)
                    break;
                observed = previous;
            }
        }
        handle = {};
    }

    void* FindFppCamera(void* playerInstance)
    {
        if (!playerInstance)
            return nullptr;

        constexpr std::uint64_t fppCameraType = Game::Rtti::Hash("gameFPPCameraComponent");
        auto* components = reinterpret_cast<ComponentArray*>(
            static_cast<std::byte*>(playerInstance) + kEntityComponentsOffset);
        if (!components->entries || components->size > components->capacity || components->size > 512)
            return nullptr;

        for (std::uint32_t index = 0; index < components->size; ++index)
        {
            void* component = *reinterpret_cast<void**>(components->entries + static_cast<std::size_t>(index) * 0x10);
            if (component && Game::Rtti::IsClassOrDerived(Game::Rtti::NativeType(component), fppCameraType))
                return component;
        }
        return nullptr;
    }

    float NormalizeAngle(float degrees)
    {
        degrees = std::fmod(degrees + 180.0f, 360.0f);
        if (degrees < 0.0f)
            degrees += 360.0f;
        return degrees - 180.0f;
    }

    bool ReadPublishedTarget(float target[3], float& smoothing)
    {
        for (unsigned attempt = 0; attempt < 3; ++attempt)
        {
            const std::uint64_t before = g_state.targetGeneration.load(std::memory_order_acquire);
            if ((before & 1u) != 0)
                continue;
            target[0] = g_state.targetX.load(std::memory_order_relaxed);
            target[1] = g_state.targetY.load(std::memory_order_relaxed);
            target[2] = g_state.targetZ.load(std::memory_order_relaxed);
            smoothing = g_state.smoothing.load(std::memory_order_relaxed);
            const std::uint64_t after = g_state.targetGeneration.load(std::memory_order_acquire);
            if (before == after)
                return std::isfinite(target[0]) && std::isfinite(target[1]) && std::isfinite(target[2]);
        }
        return false;
    }

    bool CalculateAimOffsets(void* camera, const float target[3], float& yaw, float& pitch)
    {
        const auto* transform = reinterpret_cast<const WorldTransformLayout*>(
            static_cast<const std::byte*>(camera) + kComponentWorldTransformOffset);
        const float cameraX = static_cast<float>(transform->x) * kFixedPointScale;
        const float cameraY = static_cast<float>(transform->y) * kFixedPointScale;
        const float cameraZ = static_cast<float>(transform->z) * kFixedPointScale;
        const float deltaX = target[0] - cameraX;
        const float deltaY = target[1] - cameraY;
        const float deltaZ = target[2] - cameraZ;
        const float horizontal = std::hypot(deltaX, deltaY);
        if (!std::isfinite(horizontal) || horizontal < 0.001f)
            return false;

        const float* orientation = transform->orientation;
        const float norm = orientation[0] * orientation[0] + orientation[1] * orientation[1] +
                           orientation[2] * orientation[2] + orientation[3] * orientation[3];
        if (!std::isfinite(norm) || norm < 0.8f || norm > 1.2f)
            return false;

        const float currentForwardX =
            2.0f * (orientation[0] * orientation[1] - orientation[3] * orientation[2]);
        const float currentForwardY =
            1.0f - 2.0f * (orientation[0] * orientation[0] + orientation[2] * orientation[2]);
        const float currentForwardZ =
            2.0f * (orientation[1] * orientation[2] + orientation[3] * orientation[0]);
        const float currentHorizontal = std::hypot(currentForwardX, currentForwardY);
        if (!std::isfinite(currentHorizontal) || currentHorizontal < 0.001f)
            return false;

        const float currentWorldYaw = -std::atan2(currentForwardX, currentForwardY) * kRadiansToDegrees;
        const float currentWorldPitch = std::atan2(currentForwardZ, currentHorizontal) * kRadiansToDegrees;
        const float desiredWorldYaw = -std::atan2(deltaX, deltaY) * kRadiansToDegrees;
        const float desiredWorldPitch = std::atan2(deltaZ, horizontal) * kRadiansToDegrees;
        const float currentYaw = *reinterpret_cast<const float*>(static_cast<const std::byte*>(camera) + 0x42C);
        const float currentPitch = *reinterpret_cast<const float*>(static_cast<const std::byte*>(camera) + 0x430);

        // FPPCameraComponent changes its yaw reference frame in restricted/heading-locked camera states. Derive the
        // local field delta from the live world quaternion instead of assuming an absolute or player-body basis.
        yaw = NormalizeAngle(currentYaw + NormalizeAngle(desiredWorldYaw - currentWorldYaw));
        pitch = currentPitch + (desiredWorldPitch - currentWorldPitch);

        return std::isfinite(yaw) && std::isfinite(pitch);
    }

    void ValidateCameraLayout(void* camera)
    {
        if (!camera || camera == g_validatedCamera)
            return;

        const auto* transform = reinterpret_cast<const WorldTransformLayout*>(
            static_cast<const std::byte*>(camera) + kComponentWorldTransformOffset);
        const float* orientation = transform->orientation;
        const float forward[3] = {
            2.0f * (orientation[0] * orientation[1] - orientation[3] * orientation[2]),
            1.0f - 2.0f * (orientation[0] * orientation[0] + orientation[2] * orientation[2]),
            2.0f * (orientation[1] * orientation[2] + orientation[3] * orientation[0]),
        };
        const float target[3] = {
            static_cast<float>(transform->x) * kFixedPointScale + forward[0] * 10.0f,
            static_cast<float>(transform->y) * kFixedPointScale + forward[1] * 10.0f,
            static_cast<float>(transform->z) * kFixedPointScale + forward[2] * 10.0f,
        };

        float resolvedYaw = 0.0f;
        float resolvedPitch = 0.0f;
        if (CalculateAimOffsets(camera, target, resolvedYaw, resolvedPitch))
        {
            const float currentYaw = *reinterpret_cast<const float*>(static_cast<const std::byte*>(camera) + 0x42C);
            const float currentPitch = *reinterpret_cast<const float*>(static_cast<const std::byte*>(camera) + 0x430);
            Diagnostics::Log("memory aim layout check: current=(%.3f,%.3f) resolved=(%.3f,%.3f) delta=(%.3f,%.3f)",
                             currentYaw, currentPitch, resolvedYaw, resolvedPitch,
                             NormalizeAngle(resolvedYaw - currentYaw), resolvedPitch - currentPitch);
            g_validatedCamera = camera;
        }
    }

    void HookReadAimOffsets(void* camera, float* yaw, float* pitch)
    {
        HookLifecycle::CallbackGuard guard;
        g_originalReadAimOffsets(camera, yaw, pitch);
        g_state.hookCallbacks.fetch_add(1, std::memory_order_relaxed);

        const bool active = !HookLifecycle::IsShuttingDown() &&
                            g_state.aimActive.load(std::memory_order_acquire) &&
                            camera == g_state.fppCamera.load(std::memory_order_acquire) && yaw && pitch;
        if (!active)
        {
            if (!g_state.aimActive.load(std::memory_order_acquire))
                g_wasAimActive = false;
            return;
        }

        float target[3]{};
        float smoothing = 0.0f;
        float desiredYaw = 0.0f;
        float desiredPitch = 0.0f;
        if (!ReadPublishedTarget(target, smoothing) ||
            !CalculateAimOffsets(camera, target, desiredYaw, desiredPitch))
        {
            return;
        }

        const ULONGLONG now = GetTickCount64();
        const bool hardLock = smoothing <= 0.001f;
        if (hardLock)
        {
            *yaw = desiredYaw;
            *pitch = desiredPitch;
        }
        else
        {
            const float elapsed = g_wasAimActive && now >= g_lastAimUpdateTick
                                      ? std::clamp(static_cast<float>(now - g_lastAimUpdateTick) * 0.001f,
                                                   0.0f, 0.1f)
                                      : 1.0f / 60.0f;
            const float response = 35.0f / std::max(smoothing, 0.05f);
            const float alpha = 1.0f - std::exp(-response * elapsed);
            *yaw = NormalizeAngle(*yaw + NormalizeAngle(desiredYaw - *yaw) * alpha);
            *pitch += (desiredPitch - *pitch) * alpha;
        }

        g_lastAimUpdateTick = now;
        g_wasAimActive = true;
        g_state.appliedWrites.fetch_add(1, std::memory_order_relaxed);
    }
}

namespace Game::AimAssist
{
    bool CreateHook()
    {
        const auto scan = Game::Signatures::FindInText(GetModuleHandleW(nullptr), kReadAimOffsetsPattern,
                                                       kReadAimOffsetsMask, sizeof(kReadAimOffsetsPattern));
        Diagnostics::Log("memory aim signature scan: matches=%zu address=%p", scan.matches, scan.address);
        if (scan.matches != 1 || !scan.address)
            return false;

        const MH_STATUS status = MH_CreateHook(scan.address, &HookReadAimOffsets,
                                               reinterpret_cast<void**>(&g_originalReadAimOffsets));
        if (status != MH_OK)
        {
            Diagnostics::Log("MH_CreateHook(memory aim) failed: %s (%d)", MH_StatusToString(status), status);
            return false;
        }
        g_state.hookCreated.store(true, std::memory_order_release);
        Diagnostics::Log("memory aim hook created: target=%p yawOffset=0x42C pitchOffset=0x430",
                         scan.address);
        return true;
    }

    void ProbePlayerCamera()
    {
        const ULONGLONG now = GetTickCount64();
        if (g_state.fppCamera.load(std::memory_order_acquire) &&
            now - g_state.lastPlayerResolveTick < 1000)
        {
            return;
        }

        __try
        {
            if (!InitializePlayerResolver())
                return;
            HandleLayout player;
            if (!GetPlayerHandle(player))
                return;
            void* camera = FindFppCamera(player.instance);
            void* previous = g_state.fppCamera.exchange(camera, std::memory_order_acq_rel);
            g_state.lastPlayerResolveTick = now;
            if (camera != previous)
                Diagnostics::Log("memory aim camera: player=%p fppCamera=%p", player.instance, camera);
            ValidateCameraLayout(camera);
            if (!g_hookObservedLogged)
            {
                const std::uint64_t callbacks = g_state.hookCallbacks.load(std::memory_order_relaxed);
                if (callbacks > 0)
                {
                    Diagnostics::Log("memory aim hook live: callbacks=%llu applied=%llu",
                                     static_cast<unsigned long long>(callbacks),
                                     static_cast<unsigned long long>(
                                         g_state.appliedWrites.load(std::memory_order_relaxed)));
                    g_hookObservedLogged = true;
                }
            }
            ReleaseReturnedHandle(player);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_state.fppCamera.store(nullptr, std::memory_order_release);
        }
    }

    bool ApplyMemoryAim(const float worldTarget[3], float smoothing)
    {
        if (!worldTarget || !g_state.hookCreated.load(std::memory_order_acquire))
            return false;

        ProbePlayerCamera();
        if (!g_state.fppCamera.load(std::memory_order_acquire))
            return false;

        const bool hardLock = smoothing <= 0.001f;
        if (!g_hasLoggedAimMode || g_lastLoggedHardLock != hardLock)
        {
            Diagnostics::Log("memory aim mode: %s", hardLock ? "hard" : "smooth");
            g_lastLoggedHardLock = hardLock;
            g_hasLoggedAimMode = true;
        }

        g_state.targetGeneration.fetch_add(1, std::memory_order_acq_rel);
        g_state.targetX.store(worldTarget[0], std::memory_order_relaxed);
        g_state.targetY.store(worldTarget[1], std::memory_order_relaxed);
        g_state.targetZ.store(worldTarget[2], std::memory_order_relaxed);
        g_state.smoothing.store(smoothing, std::memory_order_relaxed);
        g_state.targetGeneration.fetch_add(1, std::memory_order_release);
        g_state.aimActive.store(true, std::memory_order_release);
        return true;
    }

    void ClearMemoryAim()
    {
        g_state.aimActive.store(false, std::memory_order_release);
    }

    void Shutdown()
    {
        ClearMemoryAim();
        g_state.hookCreated.store(false, std::memory_order_release);
        g_state.fppCamera.store(nullptr, std::memory_order_release);
        g_state.playerResolverAttempted = false;
        g_state.playerSystem = nullptr;
        g_state.getLocalPlayer = nullptr;
        g_state.lastPlayerResolveTick = 0;
        g_originalReadAimOffsets = nullptr;
        g_lastAimUpdateTick = 0;
        g_wasAimActive = false;
        g_hasLoggedAimMode = false;
        g_lastLoggedHardLock = false;
        g_validatedCamera = nullptr;
        g_hookObservedLogged = false;
    }
}
