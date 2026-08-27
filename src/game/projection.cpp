#include "projection.h"
#include "rtti_invoker.h"
#include "../diagnostics.h"
#include "../framework.h"

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

    struct ProjectionState
    {
        bool attempted = false;
        void* cameraSystem = nullptr;
        ProjectPointFn projectPoint = nullptr;
        bool loggedFirstProjection = false;
        CameraPositionSource cameraPositionSource = CameraPositionSource::Unknown;
    };

    ProjectionState g_state;

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
            return g_state.cameraSystem && g_state.projectPoint;
        g_state.attempted = true;

        HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
        const auto resolve = red4ext
                                 ? reinterpret_cast<ResolveAddressFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                                 : nullptr;
        if (!resolve)
        {
            Diagnostics::Log("projection unavailable: RED4ext address resolver is not loaded");
            return false;
        }

        const std::uintptr_t enginePointerAddress = resolve(kGameEngineAddressHash);
        const std::uintptr_t rttiGetAddress = resolve(kRttiSystemGetAddressHash);
        const std::uintptr_t projectPointAddress = resolve(kCameraProjectPointAddressHash);
        if (!enginePointerAddress || !rttiGetAddress || !projectPointAddress)
        {
            Diagnostics::Log("projection unavailable: an address-library lookup failed");
            return false;
        }

        void* engine = *reinterpret_cast<void**>(enginePointerAddress);
        void* framework = engine ? *reinterpret_cast<void**>(static_cast<std::byte*>(engine) + 0x308) : nullptr;
        void* gameInstance = framework
                                 ? *reinterpret_cast<void**>(static_cast<std::byte*>(framework) + 0x10)
                                 : nullptr;
        void* rttiSystem = reinterpret_cast<GetRttiSystemFn>(rttiGetAddress)();

        const auto getClass = reinterpret_cast<GetClassFn>(VirtualFunction(rttiSystem, 2));
        void* cameraType = getClass ? getClass(rttiSystem, Fnv1a64("gameICameraSystem")) : nullptr;
        const auto getSystem = reinterpret_cast<GetSystemFn>(VirtualFunction(gameInstance, 1));
        g_state.cameraSystem = getSystem && cameraType ? getSystem(gameInstance, cameraType) : nullptr;
        g_state.projectPoint = reinterpret_cast<ProjectPointFn>(projectPointAddress);

        if (!g_state.cameraSystem)
        {
            Diagnostics::Log("projection unavailable: gameICameraSystem was not found");
            g_state.projectPoint = nullptr;
            return false;
        }

        Diagnostics::Log("projection initialized: cameraSystem=%p camera=%p ProjectPoint=%p", g_state.cameraSystem,
                         static_cast<std::byte*>(g_state.cameraSystem) + 0x60,
                         reinterpret_cast<void*>(g_state.projectPoint));
        return true;
    }

    bool ProjectRaw(const Vector3& point, Vector4& output)
    {
        output = {};
        void* camera = static_cast<std::byte*>(g_state.cameraSystem) + 0x60;
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
    bool IsCameraPositionPlausible(const float world[3])
    {
        if (!IsFinitePosition(world))
            return false;
        Vector4 projected{};
        const Vector3 point{world[0], world[1], world[2]};
        return ProjectRaw(point, projected) && std::abs(projected.w) < 1.0f;
    }

    bool ReadCameraPositionFromVirtualSlot(float world[3])
    {
        __try
        {
            void** table = *reinterpret_cast<void***>(g_state.cameraSystem);
            if (!table)
                return false;
            const auto read = reinterpret_cast<GetCameraPositionFn>(
                table[kGetCameraPositionVtableOffset / sizeof(void*)]);
            if (!read)
                return false;

            Vector3 position{};
            read(g_state.cameraSystem, position);
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

    bool ReadCameraPositionFromScript(float world[3])
    {
        __try
        {
            Game::Rtti::Function* function =
                Game::Rtti::FindFunction(Game::Rtti::NativeType(g_state.cameraSystem),
                                         Game::Rtti::Hash("GetActiveCameraWorldTransform"));
            if (!function)
                return false;

            Transform transform{};
            bool result = false;
            Game::Rtti::Argument arguments[] = {{&transform}};
            if (!Game::Rtti::Invoke(function, g_state.cameraSystem, arguments, 1, &result) || !result)
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
        if (!world || displayWidth <= 0.0f || displayHeight <= 0.0f || !Initialize())
            return false;

        const Vector3 point{world[0], world[1], world[2]};
        Vector4 projected{};
        if (!ProjectRaw(point, projected))
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

    bool GetCameraPosition(float world[3])
    {
        if (!world || !Initialize() || g_state.cameraPositionSource == CameraPositionSource::Unavailable)
            return false;

        switch (g_state.cameraPositionSource)
        {
        case CameraPositionSource::VirtualSlot:
            return ReadCameraPositionFromVirtualSlot(world);
        case CameraPositionSource::ScriptTransform:
            return ReadCameraPositionFromScript(world);
        default:
            break;
        }

        world[0] = 0.0f;
        world[1] = 0.0f;
        world[2] = 0.0f;
        if (ReadCameraPositionFromVirtualSlot(world) && IsCameraPositionPlausible(world))
            g_state.cameraPositionSource = CameraPositionSource::VirtualSlot;
        else if (ReadCameraPositionFromScript(world) && IsCameraPositionPlausible(world))
            g_state.cameraPositionSource = CameraPositionSource::ScriptTransform;
        else
            g_state.cameraPositionSource = CameraPositionSource::Unavailable;

        Diagnostics::Log("camera position source: %s world=(%.2f, %.2f, %.2f)",
                         g_state.cameraPositionSource == CameraPositionSource::VirtualSlot ? "virtual slot"
                         : g_state.cameraPositionSource == CameraPositionSource::ScriptTransform
                             ? "GetActiveCameraWorldTransform"
                             : "unavailable",
                         world[0], world[1], world[2]);
        return g_state.cameraPositionSource != CameraPositionSource::Unavailable;
    }
}
