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
    constexpr std::size_t kEntityComponentsOffset = 0xA0;
    constexpr std::size_t kComponentWorldTransformOffset = 0xE0;
    constexpr std::uint64_t kFppCameraType = Game::Rtti::Hash("gameFPPCameraComponent");

    // gameFPPCameraComponent camera-input update, Cyberpunk 2077 2.31. Argument 4 is the raw pitch delta. Yaw is
    // owned by the surrounding camera controller and written through its live input owner immediately before this
    // call. Both paths are upstream of the derived projection transform and camera-system matrix caches.
    constexpr std::uint8_t kFppCameraInputUpdatePattern[] = {
        0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x78, 0x10, 0x55,
        0x48, 0x8D, 0x68, 0xD8, 0x48, 0x81, 0xEC, 0x20, 0x01, 0x00, 0x00, 0x80,
        0xB9, 0xD8, 0x04, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF9,
    };
    constexpr char kFppCameraInputUpdateMask[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    static_assert(sizeof(kFppCameraInputUpdatePattern) == sizeof(kFppCameraInputUpdateMask) - 1);

    constexpr std::uint8_t kCameraControllerUpdatePattern[] = {
        0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48,
        0x89, 0x78, 0x20, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x8D, 0x68, 0x98, 0x48, 0x81, 0xEC, 0x40, 0x01, 0x00, 0x00,
        0x0F, 0x29, 0x70, 0xC8, 0x45, 0x33, 0xED, 0x4C, 0x8B, 0xF2, 0x0F, 0x29,
        0x78, 0xB8, 0x48, 0x8B, 0xF1, 0x44, 0x0F, 0x29, 0x40, 0xA8, 0x44, 0x0F,
        0x29, 0x48, 0x98, 0x4C, 0x39, 0x69, 0x50,
    };
    constexpr char kCameraControllerUpdateMask[] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    static_assert(sizeof(kCameraControllerUpdatePattern) == sizeof(kCameraControllerUpdateMask) - 1);

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
    using FppCameraInputUpdateFn = void (*)(void*, float, float, float, float, float, bool);
    using CameraControllerUpdateFn = void (*)(void*, void*);
    using GetYawInputOwnerFn = void* (*)(void*);

    struct State
    {
        bool resolverAttempted = false;
        void* playerSystem = nullptr;
        void* cameraSystem = nullptr;
        Game::Rtti::Function* getLocalPlayer = nullptr;
        std::atomic<void*> fppCamera{nullptr};
        ULONGLONG lastPlayerResolveTick = 0;

        std::atomic_bool hookCreated{false};
        std::atomic_bool aimActive{false};
        std::atomic<float> targetX{0.0f};
        std::atomic<float> targetY{0.0f};
        std::atomic<float> targetZ{0.0f};
        std::atomic<float> smoothing{0.0f};
        std::atomic_uint64_t targetGeneration{0};
        std::atomic_uint64_t hookCallbacks{0};
        std::atomic_uint64_t appliedWrites{0};
        std::atomic_uint64_t calculationFailures{0};
        std::atomic<float> angularError{0.0f};
    };

    State g_state;
    FppCameraInputUpdateFn g_originalFppCameraInputUpdate = nullptr;
    CameraControllerUpdateFn g_originalCameraControllerUpdate = nullptr;
    thread_local void* g_currentCameraControllerOwner = nullptr;
    ULONGLONG g_lastAimUpdateTick = 0;
    bool g_wasAimActive = false;
    bool g_hasLoggedAimMode = false;
    bool g_lastLoggedHardLock = false;

    void* VirtualFunction(void* object, std::size_t index)
    {
        if (!object)
            return nullptr;
        void** table = *reinterpret_cast<void***>(object);
        return table ? table[index] : nullptr;
    }

    bool InitializeResolvers()
    {
        if (g_state.resolverAttempted)
            return g_state.playerSystem && g_state.cameraSystem && g_state.getLocalPlayer;
        g_state.resolverAttempted = true;

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
        void* playerType = getClass ? getClass(rttiSystem, Game::Rtti::Hash("gameIPlayerSystem")) : nullptr;
        void* cameraType = getClass ? getClass(rttiSystem, Game::Rtti::Hash("gameICameraSystem")) : nullptr;
        g_state.playerSystem = getSystem && playerType ? getSystem(gameInstance, playerType) : nullptr;
        g_state.cameraSystem = getSystem && cameraType ? getSystem(gameInstance, cameraType) : nullptr;
        g_state.getLocalPlayer = Game::Rtti::FindFunction(Game::Rtti::NativeType(g_state.playerSystem),
                                                          Game::Rtti::Hash("GetLocalPlayerControlledGameObject"));
        Diagnostics::Log("memory aim resolver: playerSystem=%p cameraSystem=%p getPlayer=%p",
                         g_state.playerSystem, g_state.cameraSystem, g_state.getLocalPlayer);
        return g_state.playerSystem && g_state.cameraSystem && g_state.getLocalPlayer;
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
        auto* components = reinterpret_cast<ComponentArray*>(
            static_cast<std::byte*>(playerInstance) + kEntityComponentsOffset);
        if (!components->entries || components->size > components->capacity || components->size > 512)
            return nullptr;
        for (std::uint32_t index = 0; index < components->size; ++index)
        {
            void* component = *reinterpret_cast<void**>(components->entries + static_cast<std::size_t>(index) * 0x10);
            if (component && Game::Rtti::IsClassOrDerived(Game::Rtti::NativeType(component), kFppCameraType))
                return component;
        }
        return nullptr;
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

    float QuaternionDot(const float left[4], const float right[4])
    {
        return left[0] * right[0] + left[1] * right[1] + left[2] * right[2] + left[3] * right[3];
    }

    float NormalizeAngle(float degrees)
    {
        degrees = std::fmod(degrees + 180.0f, 360.0f);
        if (degrees < 0.0f)
            degrees += 360.0f;
        return degrees - 180.0f;
    }

    float AimAlpha(float smoothing, ULONGLONG now, ULONGLONG& lastTick, bool wasActive)
    {
        if (smoothing <= 0.001f)
            return 1.0f;
        const float elapsed = wasActive && now >= lastTick
                                  ? std::clamp(static_cast<float>(now - lastTick) * 0.001f, 0.0f, 0.1f)
                                  : 1.0f / 60.0f;
        return 1.0f - std::exp(-(35.0f / std::max(smoothing, 0.05f)) * elapsed);
    }

    void* ResolveYawInputOwner()
    {
        void* provider = g_currentCameraControllerOwner
                             ? *reinterpret_cast<void**>(
                                   static_cast<std::byte*>(g_currentCameraControllerOwner) + 0xC8)
                             : nullptr;
        void** table = provider ? *reinterpret_cast<void***>(provider) : nullptr;
        void* wrapper = table && table[0]
                            ? reinterpret_cast<GetYawInputOwnerFn>(table[0])(provider)
                            : nullptr;
        return wrapper ? *reinterpret_cast<void**>(static_cast<std::byte*>(wrapper) + 0x8) : nullptr;
    }

    bool CalculateAimDeltas(void* camera, const float target[3], float& yawDelta, float& pitchDelta)
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
        const float norm = QuaternionDot(orientation, orientation);
        if (!std::isfinite(norm) || norm < 0.8f || norm > 1.2f)
            return false;

        const float forwardX = 2.0f * (orientation[0] * orientation[1] - orientation[3] * orientation[2]);
        const float forwardY = 1.0f - 2.0f * (orientation[0] * orientation[0] + orientation[2] * orientation[2]);
        const float forwardZ = 2.0f * (orientation[1] * orientation[2] + orientation[3] * orientation[0]);
        const float currentHorizontal = std::hypot(forwardX, forwardY);
        if (!std::isfinite(currentHorizontal) || currentHorizontal < 0.001f)
            return false;

        const float currentYaw = -std::atan2(forwardX, forwardY) * kRadiansToDegrees;
        const float currentPitch = std::atan2(forwardZ, currentHorizontal) * kRadiansToDegrees;
        const float desiredYaw = -std::atan2(deltaX, deltaY) * kRadiansToDegrees;
        const float desiredPitch = std::atan2(deltaZ, horizontal) * kRadiansToDegrees;
        yawDelta = NormalizeAngle(desiredYaw - currentYaw);
        pitchDelta = desiredPitch - currentPitch;
        return std::isfinite(yawDelta) && std::isfinite(pitchDelta);
    }

    void HookFppCameraInputUpdate(void* camera, float deltaTime, float yawInput, float pitchInput,
                                  float additiveYaw, float additivePitch, bool hasAdditiveInput)
    {
        HookLifecycle::CallbackGuard guard;
        g_state.hookCallbacks.fetch_add(1, std::memory_order_relaxed);

        const bool matchingCamera = camera && camera == g_state.fppCamera.load(std::memory_order_acquire);
        const bool active = matchingCamera && !HookLifecycle::IsShuttingDown() &&
                            g_state.aimActive.load(std::memory_order_acquire);
        if (active)
        {
            float target[3]{};
            float smoothing = 0.0f;
            float yawDelta = 0.0f;
            float pitchDelta = 0.0f;
            if (ReadPublishedTarget(target, smoothing) &&
                CalculateAimDeltas(camera, target, yawDelta, pitchDelta))
            {
                const auto* bytes = static_cast<const std::byte*>(camera);
                const float pitchScale = *reinterpret_cast<const float*>(bytes + 0x49C);
                void* yawInputOwner = ResolveYawInputOwner();
                if (yawInputOwner && std::isfinite(pitchScale) && std::abs(pitchScale) > 0.0001f)
                {
                    const ULONGLONG now = GetTickCount64();
                    const float alpha = AimAlpha(smoothing, now, g_lastAimUpdateTick, g_wasAimActive);
                    *reinterpret_cast<float*>(static_cast<std::byte*>(yawInputOwner) + 0x9C) =
                        yawDelta * alpha;
                    pitchInput = pitchDelta * alpha / pitchScale;
                    g_state.angularError.store(std::hypot(yawDelta, pitchDelta), std::memory_order_relaxed);
                    g_state.appliedWrites.fetch_add(1, std::memory_order_relaxed);
                    g_lastAimUpdateTick = now;
                    g_wasAimActive = true;
                }
                else
                {
                    g_state.calculationFailures.fetch_add(1, std::memory_order_relaxed);
                }
            }
            else
            {
                g_state.calculationFailures.fetch_add(1, std::memory_order_relaxed);
            }
        }
        else if (!g_state.aimActive.load(std::memory_order_acquire))
        {
            g_wasAimActive = false;
        }

        g_originalFppCameraInputUpdate(camera, deltaTime, yawInput, pitchInput,
                                       additiveYaw, additivePitch, hasAdditiveInput);
    }

    void HookCameraControllerUpdate(void* owner, void* tickContext)
    {
        HookLifecycle::CallbackGuard guard;
        void* previous = g_currentCameraControllerOwner;
        g_currentCameraControllerOwner = owner;
        g_originalCameraControllerUpdate(owner, tickContext);
        g_currentCameraControllerOwner = previous;
    }
}

namespace Game::AimAssist
{
    bool CreateHook()
    {
        const auto scan = Game::Signatures::FindInText(GetModuleHandleW(nullptr), kFppCameraInputUpdatePattern,
                                                       kFppCameraInputUpdateMask,
                                                       sizeof(kFppCameraInputUpdatePattern));
        Diagnostics::Log("memory aim input signature scan: matches=%zu address=%p", scan.matches, scan.address);
        if (scan.matches != 1 || !scan.address)
            return false;
        const auto controllerScan = Game::Signatures::FindInText(
            GetModuleHandleW(nullptr), kCameraControllerUpdatePattern, kCameraControllerUpdateMask,
            sizeof(kCameraControllerUpdatePattern));
        Diagnostics::Log("memory aim controller signature scan: matches=%zu address=%p",
                         controllerScan.matches, controllerScan.address);
        if (controllerScan.matches != 1 || !controllerScan.address)
            return false;
        const MH_STATUS status = MH_CreateHook(scan.address, &HookFppCameraInputUpdate,
                                               reinterpret_cast<void**>(&g_originalFppCameraInputUpdate));
        if (status != MH_OK)
        {
            Diagnostics::Log("MH_CreateHook(memory aim input) failed: %s (%d)", MH_StatusToString(status), status);
            return false;
        }
        const MH_STATUS controllerStatus = MH_CreateHook(
            controllerScan.address, &HookCameraControllerUpdate,
            reinterpret_cast<void**>(&g_originalCameraControllerUpdate));
        if (controllerStatus != MH_OK)
        {
            Diagnostics::Log("MH_CreateHook(memory aim controller) failed: %s (%d)",
                             MH_StatusToString(controllerStatus), controllerStatus);
            MH_RemoveHook(scan.address);
            g_originalFppCameraInputUpdate = nullptr;
            return false;
        }
        g_state.hookCreated.store(true, std::memory_order_release);
        Diagnostics::Log("memory aim hooks created: pitch=%p yawController=%p yawField=controllerInput+0x9C",
                         scan.address, controllerScan.address);
        return true;
    }

    void ProbePlayerCamera()
    {
        const ULONGLONG now = GetTickCount64();
        if (g_state.fppCamera.load(std::memory_order_acquire) && now - g_state.lastPlayerResolveTick < 1000)
            return;
        __try
        {
            if (!InitializeResolvers())
                return;
            HandleLayout player;
            if (!GetPlayerHandle(player))
                return;
            void* camera = FindFppCamera(player.instance);
            void* previous = g_state.fppCamera.exchange(camera, std::memory_order_acq_rel);
            g_state.lastPlayerResolveTick = now;
            if (camera != previous)
            {
                Diagnostics::Log("memory aim camera: player=%p fppCamera=%p", player.instance, camera);
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
        if (!g_state.fppCamera.load(std::memory_order_acquire) || !g_state.cameraSystem)
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

    DiagnosticsSnapshot GetDiagnostics()
    {
        return {
            g_state.hookCallbacks.load(std::memory_order_relaxed),
            g_state.appliedWrites.load(std::memory_order_relaxed),
            g_state.calculationFailures.load(std::memory_order_relaxed),
            g_state.fppCamera.load(std::memory_order_relaxed),
            g_state.cameraSystem,
            g_state.angularError.load(std::memory_order_relaxed),
        };
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
        g_state.resolverAttempted = false;
        g_state.playerSystem = nullptr;
        g_state.cameraSystem = nullptr;
        g_state.getLocalPlayer = nullptr;
        g_state.lastPlayerResolveTick = 0;
        g_originalFppCameraInputUpdate = nullptr;
        g_originalCameraControllerUpdate = nullptr;
        g_currentCameraControllerOwner = nullptr;
        g_lastAimUpdateTick = 0;
        g_wasAimActive = false;
        g_hasLoggedAimMode = false;
        g_lastLoggedHardLock = false;
    }
}
