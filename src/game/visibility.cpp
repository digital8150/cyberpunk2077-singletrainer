#include "visibility.h"
#include "entity_tracker.h"
#include "player_modifiers.h"
#include "rtti_invoker.h"
#include "../diagnostics.h"
#include "../framework.h"
#include "../hooks/hook_lifecycle.h"

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>

namespace
{
    // RED4ext address hash for red::GameAppRunningState::OnTick. CET uses the same address and chains its
    // onUpdate work through the original function, so this is deliberately hooked at the game tick rather than at
    // Present or from a private thread.
    constexpr std::uint32_t kOnTickAddressHash = 3592689218u;
    constexpr std::uint32_t kGameEngineAddressHash = 0x97F209D6u;
    constexpr std::uint32_t kRttiSystemGetAddressHash = 0x4A610F64u;

    constexpr std::uint64_t kSightBlockerPreset = Game::Rtti::Hash("Sight Blocker");
    constexpr std::uint64_t kWorldStaticPreset = Game::Rtti::Hash("World Static");

    constexpr ULONGLONG kRefreshIntervalMilliseconds = 500;
    constexpr std::size_t kCacheSize = 256;
    constexpr std::size_t kQueueSize = 64;
    constexpr std::size_t kRequestsPerTick = 1;
    constexpr ULONGLONG kStatsLogIntervalMilliseconds = 3000;

    constexpr float kEndPullbackMeters = 0.02f;
    constexpr float kSelfHitToleranceMeters = 0.55f;

    struct alignas(16) Vector4
    {
        float x;
        float y;
        float z;
        float w;
    };

    // physicsTraceResult (0x60): position(0x00), normal(0x0C), material(0x18).
    struct alignas(16) TraceResult
    {
        float position[3];
        float normal[3];
        std::uint64_t material;
        std::byte reserved[0x60 - 0x20];
    };
    static_assert(sizeof(TraceResult) == 0x60);

    using ResolveAddressFn = std::uintptr_t (*)(std::uint32_t);
    using GetRttiSystemFn = void* (*)();
    using GetClassFn = void* (*)(void*, std::uint64_t);
    using GetSystemFn = void* (*)(void*, void*);
    using OnTickFn = bool (*)(void*, void*);

    struct CacheEntry
    {
        std::uint64_t entityId = 0;
        ULONGLONG tick = 0;
        Game::Visibility::State state = Game::Visibility::State::Unknown;
        bool pending = false;
    };

    struct Request
    {
        std::uint64_t entityId = 0;
        float camera[3]{};
        float primary[3]{};
        float secondary[3]{};
        bool hasSecondary = false;
    };

    struct State
    {
        std::atomic_bool hookCreated{false};
        std::atomic_bool spatialResolveAttempted{false};
        std::atomic<void*> spatialQueriesSystem{nullptr};
        std::atomic<Game::Rtti::Function*> raycast{nullptr};
        std::atomic<std::uint64_t> preset{kSightBlockerPreset};
        std::atomic_uint64_t totalCasts{0};
        std::atomic_uint64_t totalVisible{0};
        std::atomic_uint64_t totalOccluded{0};
        std::atomic_uint64_t droppedRequests{0};
    };

    State g_state;
    OnTickFn g_originalOnTick = nullptr;

    SRWLOCK g_lock = SRWLOCK_INIT;
    std::array<CacheEntry, kCacheSize> g_cache{};
    std::array<Request, kQueueSize> g_queue{};
    std::size_t g_queueHead = 0;
    std::size_t g_queueCount = 0;

    std::atomic_bool g_loggedFirstMainTick{false};
    DWORD g_mainTickThreadId = 0;
    ULONGLONG g_lastStatsLogTick = 0;

    void* VirtualFunction(void* object, std::size_t index)
    {
        if (!object)
            return nullptr;
        void** table = *reinterpret_cast<void***>(object);
        return table ? table[index] : nullptr;
    }

    std::uint8_t* ResolveOnTick()
    {
        HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
        const auto resolve = red4ext
                                 ? reinterpret_cast<ResolveAddressFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                                 : nullptr;
        if (!resolve)
        {
            Diagnostics::Log("visibility disabled: RED4ext address resolver is not loaded");
            return nullptr;
        }

        const std::uintptr_t address = resolve(kOnTickAddressHash);
        Diagnostics::Log("visibility OnTick resolver: hash=%u address=%p", kOnTickAddressHash,
                         reinterpret_cast<void*>(address));
        return reinterpret_cast<std::uint8_t*>(address);
    }

    // Resolve the spatial query metadata only from the game-main-tick callback. This keeps all engine RTTI access and
    // the synchronous query in the same context CET uses for onUpdate.
    bool ResolveSpatialQueryOnMainTick()
    {
        if (g_state.spatialResolveAttempted.exchange(true, std::memory_order_acq_rel))
            return g_state.spatialQueriesSystem.load(std::memory_order_acquire) != nullptr &&
                   g_state.raycast.load(std::memory_order_acquire) != nullptr;

        void* spatialQueriesSystem = nullptr;
        Game::Rtti::Function* raycast = nullptr;
        std::uint64_t preset = kSightBlockerPreset;
        std::size_t parameterCount = 0;
        bool resolved = false;

        __try
        {
            HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
            const auto resolve = red4ext ? reinterpret_cast<ResolveAddressFn>(
                                               GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                                         : nullptr;
            const std::uintptr_t enginePointerAddress = resolve ? resolve(kGameEngineAddressHash) : 0;
            const std::uintptr_t rttiGetAddress = resolve ? resolve(kRttiSystemGetAddressHash) : 0;
            void* engine = enginePointerAddress ? *reinterpret_cast<void**>(enginePointerAddress) : nullptr;
            void* framework = engine ? *reinterpret_cast<void**>(static_cast<std::byte*>(engine) + 0x308) : nullptr;
            void* gameInstance = framework ? *reinterpret_cast<void**>(static_cast<std::byte*>(framework) + 0x10)
                                           : nullptr;
            void* rttiSystem = rttiGetAddress ? reinterpret_cast<GetRttiSystemFn>(rttiGetAddress)() : nullptr;
            const auto getClass = reinterpret_cast<GetClassFn>(VirtualFunction(rttiSystem, 2));
            const auto getSystem = reinterpret_cast<GetSystemFn>(VirtualFunction(gameInstance, 1));
            void* systemType = getClass ? getClass(rttiSystem, Game::Rtti::Hash("gameISpatialQueriesSystem")) : nullptr;
            spatialQueriesSystem = getSystem && systemType ? getSystem(gameInstance, systemType) : nullptr;

            const Game::Rtti::Class* type = Game::Rtti::NativeType(spatialQueriesSystem);
            raycast = Game::Rtti::FindFunction(type, Game::Rtti::Hash("SyncRaycastByQueryPreset"));
            if (raycast)
                preset = kSightBlockerPreset;
            else
            {
                raycast = Game::Rtti::FindFunction(type, Game::Rtti::Hash("SyncRaycastByCollisionPreset"));
                preset = kWorldStaticPreset;
            }
            parameterCount = Game::Rtti::ParameterCount(raycast);
            if (parameterCount != 6)
                raycast = nullptr;
            resolved = spatialQueriesSystem != nullptr && raycast != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spatialQueriesSystem = nullptr;
            raycast = nullptr;
            parameterCount = 0;
            resolved = false;
        }

        if (resolved)
        {
            g_state.preset.store(preset, std::memory_order_release);
            g_state.raycast.store(raycast, std::memory_order_release);
            g_state.spatialQueriesSystem.store(spatialQueriesSystem, std::memory_order_release);
        }

        Diagnostics::Log("visibility resolver on main tick: spatialQueriesSystem=%p raycast=%p params=%zu preset=%s",
                         spatialQueriesSystem, raycast, parameterCount,
                         preset == kSightBlockerPreset ? "Sight Blocker" : "World Static");
        if (!resolved)
            Diagnostics::Log("visibility disabled: spatial query resolver failed or signature was not 6 parameters");
        return resolved;
    }

    // Returns true when the camera-to-target path is clear. Failure is fail-open so the ESP remains usable while the
    // cache is cold or a particular build's query result is unavailable.
    bool CastClear(const float camera[3], const float target[3])
    {
        __try
        {
            float direction[3] = {target[0] - camera[0], target[1] - camera[1], target[2] - camera[2]};
            const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                                           direction[2] * direction[2]);
            if (!std::isfinite(length) || length < 0.05f || length > 1000.0f)
                return true;

            const float scale = (std::max)(0.0f, length - kEndPullbackMeters) / length;
            Vector4 start{camera[0], camera[1], camera[2], 1.0f};
            Vector4 end{camera[0] + direction[0] * scale, camera[1] + direction[1] * scale,
                        camera[2] + direction[2] * scale, 1.0f};
            void* spatialQueriesSystem = g_state.spatialQueriesSystem.load(std::memory_order_acquire);
            Game::Rtti::Function* raycast = g_state.raycast.load(std::memory_order_acquire);
            std::uint64_t preset = g_state.preset.load(std::memory_order_acquire);
            if (!spatialQueriesSystem || !raycast)
                return true;

            TraceResult trace{};
            bool staticOnly = false;
            bool dynamicOnly = false;
            bool hit = false;
            Game::Rtti::Argument arguments[] = {{&start}, {&end},        {&preset},
                                                {&trace}, {&staticOnly}, {&dynamicOnly}};
            g_state.totalCasts.fetch_add(1, std::memory_order_relaxed);
            if (!Game::Rtti::Invoke(raycast, spatialQueriesSystem, arguments,
                                    sizeof(arguments) / sizeof(arguments[0]), &hit))
            {
                return true;
            }
            if (!hit)
                return true;

            const float hitX = trace.position[0] - camera[0];
            const float hitY = trace.position[1] - camera[1];
            const float hitZ = trace.position[2] - camera[2];
            const float hitDistance = std::sqrt(hitX * hitX + hitY * hitY + hitZ * hitZ);
            if (!std::isfinite(hitDistance))
                return false;
            return hitDistance >= length - kSelfHitToleranceMeters;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return true;
        }
    }

    void PublishResult(std::uint64_t entityId, bool clear)
    {
        AcquireSRWLockExclusive(&g_lock);
        CacheEntry& entry = g_cache[entityId % kCacheSize];
        if (entry.entityId == entityId && entry.pending)
        {
            entry.tick = GetTickCount64();
            entry.state = clear ? Game::Visibility::State::Visible : Game::Visibility::State::Occluded;
            entry.pending = false;
        }
        ReleaseSRWLockExclusive(&g_lock);

        if (clear)
            g_state.totalVisible.fetch_add(1, std::memory_order_relaxed);
        else
            g_state.totalOccluded.fetch_add(1, std::memory_order_relaxed);
    }

    bool PopRequest(Request& request)
    {
        AcquireSRWLockExclusive(&g_lock);
        const bool available = g_queueCount > 0;
        if (available)
        {
            request = g_queue[g_queueHead];
            g_queueHead = (g_queueHead + 1) % kQueueSize;
            --g_queueCount;
        }
        ReleaseSRWLockExclusive(&g_lock);
        return available;
    }

    std::size_t QueueCount()
    {
        AcquireSRWLockShared(&g_lock);
        const std::size_t count = g_queueCount;
        ReleaseSRWLockShared(&g_lock);
        return count;
    }

    std::size_t ProcessPendingOnMainTick()
    {
        g_mainTickThreadId = GetCurrentThreadId();
        if (!g_loggedFirstMainTick.exchange(true, std::memory_order_acq_rel))
        {
            Diagnostics::Log("visibility first main tick: threadId=%lu hookCreated=%d", g_mainTickThreadId,
                             g_state.hookCreated.load(std::memory_order_acquire) ? 1 : 0);
        }

        if (!ResolveSpatialQueryOnMainTick())
            return 0;

        std::size_t processed = 0;
        Request request;
        while (processed < kRequestsPerTick && PopRequest(request))
        {
            bool clear = CastClear(request.camera, request.primary);
            if (!clear && request.hasSecondary)
                clear = CastClear(request.camera, request.secondary);
            PublishResult(request.entityId, clear);
            ++processed;
        }

        const ULONGLONG now = GetTickCount64();
        if (now - g_lastStatsLogTick >= kStatsLogIntervalMilliseconds)
        {
            g_lastStatsLogTick = now;
            Diagnostics::Log("visibility tick: threadId=%lu processed=%zu queued=%zu casts=%llu visible=%llu "
                             "occluded=%llu dropped=%llu",
                             g_mainTickThreadId, processed, QueueCount(),
                             static_cast<unsigned long long>(g_state.totalCasts.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(g_state.totalVisible.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(g_state.totalOccluded.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(g_state.droppedRequests.load(std::memory_order_relaxed)));
        }
        return processed;
    }

    bool HookOnTick(void* gameState, void* gameApplication)
    {
        HookLifecycle::CallbackGuard callback;
        if (!HookLifecycle::IsShuttingDown())
        {
            Game::EntityTracker::OnGameMainTick();
            Game::PlayerModifiers::OnGameMainTick();
            ProcessPendingOnMainTick();
        }

        if (g_originalOnTick)
            return g_originalOnTick(gameState, gameApplication);
        return false;
    }
}

namespace Game::Visibility
{
    bool CreateHook()
    {
        if (g_state.hookCreated.load(std::memory_order_acquire))
            return true;

        std::uint8_t* target = ResolveOnTick();
        if (!target)
            return false;

        const MH_STATUS status = MH_CreateHook(target, &HookOnTick, reinterpret_cast<void**>(&g_originalOnTick));
        if (status != MH_OK || !g_originalOnTick)
        {
            Diagnostics::Log("visibility disabled: MH_CreateHook(OnTick) failed: %s (%d)",
                             MH_StatusToString(status), status);
            g_originalOnTick = nullptr;
            return false;
        }

        g_state.hookCreated.store(true, std::memory_order_release);
        Diagnostics::Log("visibility OnTick hook created: target=%p original=%p requestsPerTick=%zu refreshMs=%llu "
                         "queueSize=%zu",
                         target, reinterpret_cast<void*>(g_originalOnTick), kRequestsPerTick,
                         static_cast<unsigned long long>(kRefreshIntervalMilliseconds), kQueueSize);
        return true;
    }

    void BeginFrame()
    {
        // Query work is budgeted by the game-main-tick hook, so the render thread only reads/enqueues cache state.
    }

    State Query(std::uint64_t entityId, const float camera[3], const float primary[3], const float secondary[3])
    {
        if (!camera || !primary || entityId == 0 || !g_state.hookCreated.load(std::memory_order_acquire) ||
            !g_state.spatialQueriesSystem.load(std::memory_order_acquire) ||
            !g_state.raycast.load(std::memory_order_acquire) || HookLifecycle::IsShuttingDown())
        {
            return State::Unknown;
        }

        const ULONGLONG now = GetTickCount64();
        State state = State::Unknown;

        AcquireSRWLockExclusive(&g_lock);
        CacheEntry& entry = g_cache[entityId % kCacheSize];
        if (entry.entityId != entityId)
        {
            entry = {};
            entry.entityId = entityId;
        }
        state = entry.state;
        if (!entry.pending && (entry.state == State::Unknown || now - entry.tick >= kRefreshIntervalMilliseconds))
        {
            if (g_queueCount < kQueueSize)
            {
                Request& request = g_queue[(g_queueHead + g_queueCount) % kQueueSize];
                request = {};
                request.entityId = entityId;
                for (unsigned i = 0; i < 3; ++i)
                {
                    request.camera[i] = camera[i];
                    request.primary[i] = primary[i];
                    request.secondary[i] = secondary ? secondary[i] : 0.0f;
                }
                request.hasSecondary = secondary != nullptr;
                ++g_queueCount;
                entry.pending = true;
            }
            else
            {
                g_state.droppedRequests.fetch_add(1, std::memory_order_relaxed);
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
        return state;
    }

    Stats GetStats()
    {
        Stats stats;
        stats.available = g_state.hookCreated.load(std::memory_order_acquire) &&
                          g_state.spatialQueriesSystem.load(std::memory_order_acquire) != nullptr &&
                          g_state.raycast.load(std::memory_order_acquire) != nullptr;
        stats.casts = g_state.totalCasts.load(std::memory_order_relaxed);
        stats.visible = g_state.totalVisible.load(std::memory_order_relaxed);
        stats.occluded = g_state.totalOccluded.load(std::memory_order_relaxed);
        stats.dropped = g_state.droppedRequests.load(std::memory_order_relaxed);
        return stats;
    }

    bool Shutdown()
    {
        // Hooks::Shutdown disables every MinHook detour and waits for CallbackGuard instances before calling here.
        // There is intentionally no thread handle or join: all synchronous engine calls have already returned with
        // the game-main-tick callback.
        g_state.hookCreated.store(false, std::memory_order_release);
        g_state.spatialQueriesSystem.store(nullptr, std::memory_order_release);
        g_state.raycast.store(nullptr, std::memory_order_release);
        g_state.spatialResolveAttempted.store(false, std::memory_order_release);
        g_originalOnTick = nullptr;

        AcquireSRWLockExclusive(&g_lock);
        g_cache = {};
        g_queueHead = 0;
        g_queueCount = 0;
        ReleaseSRWLockExclusive(&g_lock);

        g_loggedFirstMainTick.store(false, std::memory_order_release);
        g_mainTickThreadId = 0;
        g_lastStatsLogTick = 0;
        Diagnostics::Log("visibility state reset: casts=%llu visible=%llu occluded=%llu dropped=%llu",
                         static_cast<unsigned long long>(g_state.totalCasts.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.totalVisible.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.totalOccluded.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.droppedRequests.load(std::memory_order_relaxed)));
        return true;
    }
}
