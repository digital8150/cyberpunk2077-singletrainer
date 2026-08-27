#include "aim_assist.h"
#include "rtti_invoker.h"
#include "../diagnostics.h"
#include "../framework.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace
{
    constexpr std::uint32_t kGameEngineAddressHash = 0x97F209D6u;
    constexpr std::uint32_t kRttiSystemGetAddressHash = 0x4A610F64u;

    struct alignas(16) Vector4
    {
        float x;
        float y;
        float z;
        float w;
    };

    struct alignas(16) AimRequest
    {
        float duration;
        bool adjustPitch;
        bool adjustYaw;
        bool endOnTargetReached;
        bool endOnCameraInputApplied;
        bool endOnTimeExceeded;
        bool endOnAimingStopped;
        std::byte pad0A[2];
        float cameraInputMagToBreak;
        float cameraMouseInputMagToBreak;
        float precision;
        float maxDuration;
        bool easeIn;
        bool easeOut;
        bool checkRange;
        std::byte pad1F[0x30 - 0x1F];
        Vector4 lookAtTarget;
        bool processAsInput;
        std::byte pad41;
        bool bodyPartsTracking;
        std::byte pad43;
        float bptMaxDot;
        float bptMaxSwitches;
        float bptMinInputMag;
        std::byte pad50[4];
        float bptMinResetInputMag;
        std::byte pad58[0x100 - 0x58];
    };
    static_assert(sizeof(AimRequest) == 0x100);
    static_assert(offsetof(AimRequest, lookAtTarget) == 0x30);

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

    using ResolveAddressFn = std::uintptr_t (*)(std::uint32_t);
    using GetRttiSystemFn = void* (*)();
    using GetClassFn = void* (*)(void*, std::uint64_t);
    using GetSystemFn = void* (*)(void*, void*);

    struct State
    {
        bool attempted = false;
        void* playerSystem = nullptr;
        void* targetingSystem = nullptr;
        Game::Rtti::Function* getLocalPlayer = nullptr;
        Game::Rtti::Function* lookAt = nullptr;
        Game::Rtti::Function* breakLookAt = nullptr;
        bool lookAtActive = false;
    };

    State g_state;

    void* VirtualFunction(void* object, std::size_t index)
    {
        if (!object)
            return nullptr;
        void** table = *reinterpret_cast<void***>(object);
        return table ? table[index] : nullptr;
    }

    bool Initialize()
    {
        if (g_state.attempted)
            return g_state.playerSystem && g_state.targetingSystem && g_state.getLocalPlayer && g_state.lookAt &&
                   g_state.breakLookAt;
        g_state.attempted = true;

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
        void* targetingSystemType = getClass ? getClass(rttiSystem, Game::Rtti::Hash("gameITargetingSystem")) : nullptr;
        g_state.playerSystem = getSystem && playerSystemType ? getSystem(gameInstance, playerSystemType) : nullptr;
        g_state.targetingSystem = getSystem && targetingSystemType ? getSystem(gameInstance, targetingSystemType) : nullptr;
        g_state.getLocalPlayer = Game::Rtti::FindFunction(Game::Rtti::NativeType(g_state.playerSystem),
                                                          Game::Rtti::Hash("GetLocalPlayerControlledGameObject"));
        g_state.lookAt = Game::Rtti::FindFunction(Game::Rtti::NativeType(g_state.targetingSystem),
                                                  Game::Rtti::Hash("LookAt"));
        g_state.breakLookAt = Game::Rtti::FindFunction(Game::Rtti::NativeType(g_state.targetingSystem),
                                                       Game::Rtti::Hash("BreakLookAt"));
        Diagnostics::Log("native aim resolver: playerSystem=%p targetingSystem=%p getPlayer=%p lookAt=%p break=%p",
                         g_state.playerSystem, g_state.targetingSystem, g_state.getLocalPlayer, g_state.lookAt,
                         g_state.breakLookAt);
        return g_state.playerSystem && g_state.targetingSystem && g_state.getLocalPlayer && g_state.lookAt &&
               g_state.breakLookAt;
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
        if (!handle.refCount)
            return;
        const LONG strong = handle.refCount->strong;
        if (strong > 1)
            InterlockedDecrement(&handle.refCount->strong);
        handle = {};
    }
}

namespace Game::AimAssist
{
    bool ApplyLookAt(const float worldTarget[3], float smoothing)
    {
        if (!worldTarget)
            return false;

        __try
        {
            if (!Initialize())
                return false;
            HandleLayout player;
            if (!GetPlayerHandle(player))
                return false;

            const bool hardLock = smoothing <= 0.001f;
            AimRequest request{};
            request.duration = hardLock ? 0.001f : std::clamp(smoothing * 0.015f, 0.02f, 0.45f);
            request.adjustPitch = true;
            request.adjustYaw = true;
            request.endOnTargetReached = false;
            request.endOnCameraInputApplied = false;
            request.endOnTimeExceeded = true;
            request.endOnAimingStopped = false;
            request.cameraInputMagToBreak = 1000.0f;
            request.cameraMouseInputMagToBreak = 1000.0f;
            request.precision = 0.001f;
            request.maxDuration = request.duration;
            request.easeIn = !hardLock;
            request.easeOut = false;
            request.checkRange = false;
            request.lookAtTarget = {worldTarget[0], worldTarget[1], worldTarget[2], 0.0f};
            request.processAsInput = true;

            Game::Rtti::Argument arguments[] = {{&player}, {&request}};
            const bool invoked = Game::Rtti::Invoke(g_state.lookAt, g_state.targetingSystem, arguments, 2);
            ReleaseReturnedHandle(player);
            g_state.lookAtActive = invoked;
            return invoked;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Diagnostics::Log("native aim LookAt raised an exception; aim update skipped");
            g_state.lookAtActive = false;
            return false;
        }
    }

    void BreakLookAt()
    {
        if (!g_state.lookAtActive)
            return;
        __try
        {
            if (Initialize())
            {
                HandleLayout player;
                if (GetPlayerHandle(player))
                {
                    Game::Rtti::Argument argument{&player};
                    Game::Rtti::Invoke(g_state.breakLookAt, g_state.targetingSystem, &argument, 1);
                    ReleaseReturnedHandle(player);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        g_state.lookAtActive = false;
    }

    void Shutdown()
    {
        BreakLookAt();
        g_state = {};
    }
}
