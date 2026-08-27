#include "projection.h"
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

    using ResolveAddressFn = std::uintptr_t (*)(std::uint32_t);
    using GetRttiSystemFn = void* (*)();
    using GetClassFn = void* (*)(void* rttiSystem, std::uint64_t name);
    using GetSystemFn = void* (*)(void* gameInstance, void* type);
    using ProjectPointFn = void* (*)(void* camera, Vector4& output, const Vector3& point);

    struct ProjectionState
    {
        bool attempted = false;
        void* cameraSystem = nullptr;
        ProjectPointFn projectPoint = nullptr;
        bool loggedFirstProjection = false;
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
        void* camera = static_cast<std::byte*>(g_state.cameraSystem) + 0x60;
        g_state.projectPoint(camera, projected, point);

        if (!std::isfinite(projected.x) || !std::isfinite(projected.y) || !std::isfinite(projected.w))
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
}
