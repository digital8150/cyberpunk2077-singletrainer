#include "projection.h"
#include "rtti_invoker.h"
#include "../diagnostics.h"
#include "../framework.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace
{
    constexpr std::uint32_t kGameEngineAddressHash = 0x97F209D6u;
    constexpr std::uint32_t kRttiSystemGetAddressHash = 0x4A610F64u;
    constexpr std::uint32_t kCameraProjectPointAddressHash = 1517361120u;

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

    struct alignas(16) Vector3
    {
        float x;
        float y;
        float z;
    };

    struct alignas(16) Vector4
    {
        float x;
        float y;
        float z;
        float w;
    };

    // gameICameraSystem::GetCameraPosition(Vector3&) — the same virtual slot RedHotTools uses to read the
    // active camera without going through the script VM.
    constexpr std::size_t kGetCameraPositionVtableOffset = 0x218;

    struct alignas(16) Transform
    {
        Vector4 position;
        float orientation[4];
    };
    static_assert(sizeof(Transform) == 0x20);

    using ResolveAddressFn = std::uintptr_t (*)(std::uint32_t);
    using GetRttiSystemFn = void* (*)();
    using GetClassFn = void* (*)(void* rttiSystem, std::uint64_t name);
    using GetSystemFn = void* (*)(void* gameInstance, void* type);
    using ProjectPointFn = void* (*)(void* camera, Vector4& output, const Vector3& point);
    using GetCameraPositionFn = void* (*)(void* cameraSystem, Vector3& output);

    enum class CameraPositionSource
    {
        Unknown,
        VirtualSlot,
        ScriptTransform,
        Unavailable,
    };

    // 트레이너가 게임 실행과 동시에 주입되면 첫 Present 시점에는 아직 gameICameraSystem이 없다. 예전
    // 구현은 초기화를 딱 한 번만 시도하고 실패를 영구 래치해서, 메인 메뉴에서 주입된 세션은 세이브를
    // 불러온 뒤에도 투영이 죽은 채로 남았다 — ESP 박스/스켈레톤/체력바와 에임봇 타겟 선정이 통째로
    // 사라지고, 투영이 필요 없는 네이티브 하이라이트만 살아 있는 증상이 된다. 이제는 주기적으로 다시
    // 해석하고, 세션 전환으로 카메라 시스템 포인터가 갈려도 따라간다.
    constexpr ULONGLONG kCameraResolveIntervalMs = 250;
    constexpr ULONGLONG kCameraSourceRetryIntervalMs = 2000;

    struct ProjectionState
    {
        // 주소 라이브러리 조회 결과는 모듈 베이스에 묶여 있어 프로세스 수명 동안 바뀌지 않는다.
        bool staticsResolved = false;
        std::uintptr_t enginePointerAddress = 0;
        std::uintptr_t rttiGetAddress = 0;
        ProjectPointFn projectPoint = nullptr;

        std::atomic<void*> cameraSystem{nullptr};
        std::atomic<ULONGLONG> lastResolveTick{0};
        bool loggedUnavailable = false;
        bool loggedFirstProjection = false;
        CameraPositionSource cameraPositionSource = CameraPositionSource::Unknown;
        CameraPositionSource loggedCameraSource = CameraPositionSource::Unknown;
        ULONGLONG cameraSourceTick = 0;
    };

    ProjectionState g_state;

    void* VirtualFunction(void* object, std::size_t index)
    {
        if (!object)
            return nullptr;
        void** table = *reinterpret_cast<void***>(object);
        return table ? table[index] : nullptr;
    }

    bool ResolveStatics()
    {
        if (g_state.staticsResolved)
            return true;

        HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
        const auto resolve = red4ext
                                 ? reinterpret_cast<ResolveAddressFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                                 : nullptr;
        if (!resolve)
        {
            if (!g_state.loggedUnavailable)
            {
                Diagnostics::Log("projection unavailable: RED4ext address resolver is not loaded");
                g_state.loggedUnavailable = true;
            }
            return false;
        }

        const std::uintptr_t enginePointerAddress = resolve(kGameEngineAddressHash);
        const std::uintptr_t rttiGetAddress = resolve(kRttiSystemGetAddressHash);
        const std::uintptr_t projectPointAddress = resolve(kCameraProjectPointAddressHash);
        if (!enginePointerAddress || !rttiGetAddress || !projectPointAddress)
        {
            if (!g_state.loggedUnavailable)
            {
                Diagnostics::Log("projection unavailable: an address-library lookup failed");
                g_state.loggedUnavailable = true;
            }
            return false;
        }

        g_state.enginePointerAddress = enginePointerAddress;
        g_state.rttiGetAddress = rttiGetAddress;
        g_state.projectPoint = reinterpret_cast<ProjectPointFn>(projectPointAddress);
        g_state.staticsResolved = true;
        return true;
    }

    // 게임 인스턴스에서 gameICameraSystem을 다시 찾는다. 주입 직후나 세션 전환 중에는 엔진 포인터 체인이
    // 아직 다 채워지지 않은 상태라 역참조가 폴트를 낼 수 있어 SEH로 감싼다.
    void* ResolveCameraSystem()
    {
        __try
        {
            void* engine = *reinterpret_cast<void**>(g_state.enginePointerAddress);
            void* framework = engine ? *reinterpret_cast<void**>(static_cast<std::byte*>(engine) + 0x308) : nullptr;
            void* gameInstance = framework
                                     ? *reinterpret_cast<void**>(static_cast<std::byte*>(framework) + 0x10)
                                     : nullptr;
            if (!gameInstance)
                return nullptr;

            void* rttiSystem = reinterpret_cast<GetRttiSystemFn>(g_state.rttiGetAddress)();
            const auto getClass = reinterpret_cast<GetClassFn>(VirtualFunction(rttiSystem, 2));
            void* cameraType = getClass ? getClass(rttiSystem, Fnv1a64("gameICameraSystem")) : nullptr;
            const auto getSystem = reinterpret_cast<GetSystemFn>(VirtualFunction(gameInstance, 1));
            return getSystem && cameraType ? getSystem(gameInstance, cameraType) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    // ESP 한 프레임만 그려도 투영이 수백 번 불리므로, 재해석은 kCameraResolveIntervalMs 간격으로만 하고
    // 그 사이에는 캐시된 포인터를 그대로 쓴다.
    bool EnsureCameraSystem(void*& cameraSystem)
    {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG lastResolveTick = g_state.lastResolveTick.load(std::memory_order_acquire);
        void* cached = g_state.cameraSystem.load(std::memory_order_acquire);
        if (lastResolveTick != 0 && now - lastResolveTick < kCameraResolveIntervalMs)
        {
            cameraSystem = cached;
            return cached != nullptr;
        }
        g_state.lastResolveTick.store(now, std::memory_order_release);

        cameraSystem = ResolveStatics() ? ResolveCameraSystem() : nullptr;
        if (!cameraSystem)
        {
            if (g_state.staticsResolved && !g_state.loggedUnavailable)
            {
                Diagnostics::Log("projection unavailable: gameICameraSystem was not found (previous=%p); "
                                 "retrying every %ums",
                                 cached, static_cast<unsigned>(kCameraResolveIntervalMs));
                g_state.loggedUnavailable = true;
            }
            g_state.cameraSystem.store(nullptr, std::memory_order_release);
            g_state.cameraPositionSource = CameraPositionSource::Unknown;
            return false;
        }

        if (cameraSystem != cached)
        {
            Diagnostics::Log("projection initialized: cameraSystem=%p camera=%p ProjectPoint=%p", cameraSystem,
                             static_cast<std::byte*>(cameraSystem) + 0x60,
                             reinterpret_cast<void*>(g_state.projectPoint));
            g_state.cameraSystem.store(cameraSystem, std::memory_order_release);
            g_state.cameraPositionSource = CameraPositionSource::Unknown;
            g_state.loggedCameraSource = CameraPositionSource::Unknown;
            g_state.cameraSourceTick = 0;
            g_state.loggedFirstProjection = false;
        }
        g_state.loggedUnavailable = false;
        return true;
    }

    bool ProjectRaw(void* cameraSystem, const Vector3& point, Vector4& output)
    {
        output = {};
        void* camera = static_cast<std::byte*>(cameraSystem) + 0x60;
        g_state.projectPoint(camera, output, point);
        return std::isfinite(output.x) && std::isfinite(output.y) && std::isfinite(output.w);
    }

    bool IsFinitePosition(const float world[3])
    {
        for (unsigned i = 0; i < 3; ++i)
        {
            if (!std::isfinite(world[i]) || std::abs(world[i]) > 1000000.0f)
                return false;
        }
        return true;
    }

    // A point sitting on the camera projects to ~zero forward depth. Anything else means the slot/function we
    // read is not the active camera position on this build, so the caller must try the other source.
    bool IsCameraPositionPlausible(void* cameraSystem, const float world[3])
    {
        if (!IsFinitePosition(world))
            return false;
        Vector4 projected{};
        const Vector3 point{world[0], world[1], world[2]};
        return ProjectRaw(cameraSystem, point, projected) && std::abs(projected.w) < 1.0f;
    }

    bool ReadCameraPositionFromVirtualSlot(void* cameraSystem, float world[3])
    {
        __try
        {
            void** table = *reinterpret_cast<void***>(cameraSystem);
            if (!table)
                return false;
            const auto read = reinterpret_cast<GetCameraPositionFn>(
                table[kGetCameraPositionVtableOffset / sizeof(void*)]);
            if (!read)
                return false;

            Vector3 position{};
            read(cameraSystem, position);
            world[0] = position.x;
            world[1] = position.y;
            world[2] = position.z;
            return IsFinitePosition(world);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadCameraPositionFromScript(void* cameraSystem, float world[3])
    {
        __try
        {
            Game::Rtti::Function* function =
                Game::Rtti::FindFunction(Game::Rtti::NativeType(cameraSystem),
                                         Game::Rtti::Hash("GetActiveCameraWorldTransform"));
            if (!function)
                return false;

            Transform transform{};
            bool result = false;
            Game::Rtti::Argument arguments[] = {{&transform}};
            if (!Game::Rtti::Invoke(function, cameraSystem, arguments, 1, &result) || !result)
                return false;

            world[0] = transform.position.x;
            world[1] = transform.position.y;
            world[2] = transform.position.z;
            return IsFinitePosition(world);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

namespace Game::Projection
{
    bool WorldToScreen(const float world[3], float displayWidth, float displayHeight, ScreenPoint& result)
    {
        result = {};
        void* cameraSystem = nullptr;
        if (!world || displayWidth <= 0.0f || displayHeight <= 0.0f || !EnsureCameraSystem(cameraSystem))
            return false;

        const Vector3 point{world[0], world[1], world[2]};
        Vector4 projected{};
        if (!ProjectRaw(cameraSystem, point, projected))
            return false;

        result.behind = projected.w <= 0.0f;
        result.depth = projected.w;
        if (!result.behind)
        {
            const float normalizedX = projected.x / projected.w;
            const float normalizedY = projected.y / projected.w;
            result.x = displayWidth * 0.5f + normalizedX * displayWidth * 0.5f;
            result.y = displayHeight * 0.5f - normalizedY * displayHeight * 0.5f;
            result.visible = result.x >= 0.0f && result.x <= displayWidth && result.y >= 0.0f && result.y <= displayHeight;
        }

        if (!g_state.loggedFirstProjection)
        {
            Diagnostics::Log("first world projection: world=(%.2f, %.2f, %.2f) clip=(%.3f, %.3f, %.3f, %.3f) "
                             "screen=(%.1f, %.1f) behind=%d visible=%d",
                             world[0], world[1], world[2], projected.x, projected.y, projected.z, projected.w,
                             result.x, result.y, result.behind ? 1 : 0, result.visible ? 1 : 0);
            g_state.loggedFirstProjection = true;
        }
        return true;
    }

    bool GetPixelsPerTangent(float displayWidth, float displayHeight, float& pixelsPerTangent)
    {
        pixelsPerTangent = 0.0f;
        void* cameraSystem = nullptr;
        if (displayWidth <= 0.0f || displayHeight <= 0.0f || !EnsureCameraSystem(cameraSystem))
            return false;

        // 기준점은 카메라 근처가 좋다. 클립 좌표의 절대값이 작을수록 아래 차분에서 남는 유효 자릿수가
        // 많다. 카메라 위치 소스가 아직 안 잡혔으면 월드 원점으로 떨어지는데, 변환이 아핀이라 결과
        // 자체는 같고 정밀도만 조금 나빠진다.
        float base[3]{0.0f, 0.0f, 0.0f};
        if (!GetCameraPosition(base))
        {
            base[0] = 0.0f;
            base[1] = 0.0f;
            base[2] = 0.0f;
        }

        // 한 걸음을 크게 잡을수록 차분의 상대 오차가 줄어든다. 아핀이므로 걸음 크기 자체는 결과에
        // 영향을 주지 않는다.
        constexpr float kStepMeters = 10.0f;

        Vector4 origin{};
        if (!ProjectRaw(cameraSystem, Vector3{base[0], base[1], base[2]}, origin))
            return false;

        float rowX[3]{};
        float rowW[3]{};
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            float probe[3]{base[0], base[1], base[2]};
            probe[axis] += kStepMeters;
            Vector4 projected{};
            if (!ProjectRaw(cameraSystem, Vector3{probe[0], probe[1], probe[2]}, projected))
                return false;
            rowX[axis] = (projected.x - origin.x) / kStepMeters;
            rowW[axis] = (projected.w - origin.w) / kStepMeters;
        }

        const float lengthX = std::sqrt(rowX[0] * rowX[0] + rowX[1] * rowX[1] + rowX[2] * rowX[2]);
        const float lengthW = std::sqrt(rowW[0] * rowW[0] + rowW[1] * rowW[1] + rowW[2] * rowW[2]);
        if (!std::isfinite(lengthX) || !std::isfinite(lengthW) || lengthW <= 1e-6f || lengthX <= 1e-6f)
            return false;

        const float scale = (lengthX / lengthW) * displayWidth * 0.5f;
        // 이 값은 "수평 화각 절반의 코탄젠트 × 화면 반폭"이다. 5°~150° 수평 화각을 넘어서는 값이 나오면
        // 우리가 읽은 행이 기대한 원근 행렬이 아니라는 뜻이므로 호출자가 폴백을 쓰게 한다.
        const float minimum = displayWidth * 0.5f / std::tan(75.0f * 3.14159265f / 180.0f);
        const float maximum = displayWidth * 0.5f / std::tan(2.5f * 3.14159265f / 180.0f);
        if (!std::isfinite(scale) || scale < minimum || scale > maximum)
            return false;

        pixelsPerTangent = scale;

        static bool loggedFirstScale = false;
        if (!loggedFirstScale)
        {
            const float horizontalFovDegrees =
                2.0f * std::atan(displayWidth * 0.5f / scale) * 180.0f / 3.14159265f;
            Diagnostics::Log("camera angular scale: pixelsPerTangent=%.1f horizontalFov=%.1fdeg display=%.0fx%.0f",
                             scale, horizontalFovDegrees, displayWidth, displayHeight);
            loggedFirstScale = true;
        }
        return true;
    }

    bool GetCameraPosition(float world[3])
    {
        void* cameraSystem = nullptr;
        if (!world || !EnsureCameraSystem(cameraSystem))
            return false;

        // Unavailable도 영구 래치하지 않는다. 카메라 시스템이 막 만들어진 직후에는 두 소스 모두 실패할 수
        // 있고, 그 한 번의 실패로 ESP 사각형의 폭 계산과 에임봇 시야 필터가 세션 내내 죽어버린다.
        const ULONGLONG now = GetTickCount64();
        if (g_state.cameraPositionSource == CameraPositionSource::Unavailable)
        {
            if (now - g_state.cameraSourceTick < kCameraSourceRetryIntervalMs)
                return false;
            g_state.cameraPositionSource = CameraPositionSource::Unknown;
        }

        switch (g_state.cameraPositionSource)
        {
        case CameraPositionSource::VirtualSlot:
            if (ReadCameraPositionFromVirtualSlot(cameraSystem, world))
                return true;
            // 한 번 고른 소스가 갑자기 실패하면 다음 호출에서 다시 고르도록 되돌린다.
            g_state.cameraPositionSource = CameraPositionSource::Unknown;
            return false;
        case CameraPositionSource::ScriptTransform:
            if (ReadCameraPositionFromScript(cameraSystem, world))
                return true;
            g_state.cameraPositionSource = CameraPositionSource::Unknown;
            return false;
        default:
            break;
        }

        world[0] = 0.0f;
        world[1] = 0.0f;
        world[2] = 0.0f;
        g_state.cameraSourceTick = now;
        if (ReadCameraPositionFromVirtualSlot(cameraSystem, world) &&
            IsCameraPositionPlausible(cameraSystem, world))
            g_state.cameraPositionSource = CameraPositionSource::VirtualSlot;
        else if (ReadCameraPositionFromScript(cameraSystem, world) &&
                 IsCameraPositionPlausible(cameraSystem, world))
            g_state.cameraPositionSource = CameraPositionSource::ScriptTransform;
        else
            g_state.cameraPositionSource = CameraPositionSource::Unavailable;

        if (g_state.cameraPositionSource != g_state.loggedCameraSource)
        {
            Diagnostics::Log("camera position source: %s world=(%.2f, %.2f, %.2f)",
                             g_state.cameraPositionSource == CameraPositionSource::VirtualSlot ? "virtual slot"
                             : g_state.cameraPositionSource == CameraPositionSource::ScriptTransform
                                 ? "GetActiveCameraWorldTransform"
                                 : "unavailable",
                             world[0], world[1], world[2]);
            g_state.loggedCameraSource = g_state.cameraPositionSource;
        }
        return g_state.cameraPositionSource != CameraPositionSource::Unavailable;
    }
}
