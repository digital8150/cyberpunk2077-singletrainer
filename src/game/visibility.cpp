#include "visibility.h"
#include "entity_tracker.h"
#include "player_modifiers.h"
#include "rtti_invoker.h"
#include "../diagnostics.h"
#include "../framework.h"
#include "../hooks/hook_lifecycle.h"
#include "../profiling.h"

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
        std::uint64_t generation = 0;
        ULONGLONG tick = 0;
        Game::Visibility::State state = Game::Visibility::State::Unknown;
        bool pending = false;
    };

    struct Request
    {
        std::uint64_t entityId = 0;
        std::uint64_t generation = 0;
        float camera[3]{};
        float primary[3]{};
        float secondary[3]{};
        bool hasSecondary = false;
    };

    struct State
    {
        std::atomic_bool hookCreated{false};
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

    // Protected by g_lock: the gate and epoch change atomically with queue/cache invalidation.
    bool g_worldOpen = false;
    std::uint64_t g_worldGeneration = 0;
    enum class MainTickStage : std::uint32_t
    {
        Idle, WorldGate, ResolveMetadata, AcquireSystem, PrimaryCast, SecondaryCast, Publish
    };
    std::atomic<MainTickStage> g_mainTickStage{MainTickStage::Idle};
    std::atomic_uint64_t g_mainTickGeneration{0};
    std::atomic_uint64_t g_mainTickEntityId{0};

    struct TickStageReset
    {
        ~TickStageReset()
        {
            g_mainTickEntityId.store(0, std::memory_order_relaxed);
            g_mainTickStage.store(MainTickStage::Idle, std::memory_order_release);
        }
    };

    ULONGLONG g_lastResolveAttempt = 0;

    bool UpdateWorldGate()
    {
        const bool ready = Game::EntityTracker::IsWorldReadyForMainTickConsumers();
        AcquireSRWLockExclusive(&g_lock);
        const bool changed = ready != g_worldOpen;
        if (changed)
        {
            ++g_worldGeneration;
            g_worldOpen = ready;
            g_state.droppedRequests.fetch_add(g_queueCount, std::memory_order_relaxed);
            g_cache = {};
            g_queue = {};
            g_queueHead = 0;
            g_queueCount = 0;
            g_state.raycast.store(nullptr, std::memory_order_release);
            g_lastResolveAttempt = 0;
        }
        const auto generation = g_worldGeneration;
        ReleaseSRWLockExclusive(&g_lock);
        g_mainTickGeneration.store(generation, std::memory_order_release);
        if (changed)
            Diagnostics::Log("visibility world gate: ready=%d generation=%llu", ready ? 1 : 0,
                             static_cast<unsigned long long>(generation));
        return ready;
    }

    bool IsCurrentRequest(const Request& request)
    {
        AcquireSRWLockShared(&g_lock);
        const bool current = g_worldOpen && request.generation == g_worldGeneration &&
                             Game::EntityTracker::IsWorldReadyForMainTickConsumers();
        ReleaseSRWLockShared(&g_lock);
        return current;
    }

    std::atomic_bool g_loggedFirstMainTick{false};
    DWORD g_mainTickThreadId = 0;
    ULONGLONG g_lastStatsLogTick = 0;
    ULONGLONG g_lastSystemAcquireFailureLog = 0;
    std::uint64_t g_systemAcquireFailures = 0;

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

    void* ResolveGameInstanceOnMainTick()
    {
        HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
        const auto resolve = red4ext
                                 ? reinterpret_cast<ResolveAddressFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                                 : nullptr;
        const std::uintptr_t enginePointerAddress = resolve ? resolve(kGameEngineAddressHash) : 0;
        if (!enginePointerAddress)
            return nullptr;
        void* engine = *reinterpret_cast<void**>(enginePointerAddress);
        void* framework = engine ? *reinterpret_cast<void**>(static_cast<std::byte*>(engine) + 0x308) : nullptr;
        return framework ? *reinterpret_cast<void**>(static_cast<std::byte*>(framework) + 0x10) : nullptr;
    }

    void* GetSystemOnMainTick(void* gameInstance, std::uint64_t nameHash)
    {
        if (!gameInstance)
            return nullptr;
        HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
        const auto resolve = red4ext
                                 ? reinterpret_cast<ResolveAddressFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                                 : nullptr;
        const std::uintptr_t rttiAddress = resolve ? resolve(kRttiSystemGetAddressHash) : 0;
        void* rtti = rttiAddress ? reinterpret_cast<GetRttiSystemFn>(rttiAddress)() : nullptr;
        const auto getClass = reinterpret_cast<GetClassFn>(VirtualFunction(rtti, 2));
        const auto getSystem = reinterpret_cast<GetSystemFn>(VirtualFunction(gameInstance, 1));
        void* type = getClass ? getClass(rtti, nameHash) : nullptr;
        return getSystem && type ? getSystem(gameInstance, type) : nullptr;
    }

    bool AcquireSpatialQueriesSystemOnMainTick(void*& spatialQueriesSystem)
    {
        spatialQueriesSystem = nullptr;
        if (!Game::EntityTracker::IsWorldReadyForMainTickConsumers())
            return false;
        const auto previousStage = g_mainTickStage.exchange(MainTickStage::AcquireSystem);
        void* gameInstance = nullptr;
        __try
        {
            gameInstance = ResolveGameInstanceOnMainTick();
            spatialQueriesSystem = GetSystemOnMainTick(
                gameInstance, Game::Rtti::Hash("gameISpatialQueriesSystem"));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spatialQueriesSystem = nullptr;
        }

        g_mainTickStage.store(previousStage, std::memory_order_release);
        if (spatialQueriesSystem)
            return true;

        ++g_systemAcquireFailures;
        const ULONGLONG now = GetTickCount64();
        if (g_lastSystemAcquireFailureLog == 0 ||
            now - g_lastSystemAcquireFailureLog >= 1000)
        {
            g_lastSystemAcquireFailureLog = now;
            Diagnostics::Log("visibility spatial system unavailable: gameInstance=%p failures=%llu",
                             gameInstance, static_cast<unsigned long long>(g_systemAcquireFailures));
        }
        return false;
    }

    // Resolve the spatial query metadata only from the game-main-tick callback. This keeps all engine RTTI access and
    // the synchronous query in the same context CET uses for onUpdate.
    bool ResolveSpatialQueryOnMainTick()
    {
        if (g_state.raycast.load(std::memory_order_acquire))
            return true;

        const ULONGLONG now = GetTickCount64();
        if (g_lastResolveAttempt != 0 && now - g_lastResolveAttempt < 1000)
            return false;
        g_lastResolveAttempt = now;

        void* spatialQueriesSystem = nullptr;
        Game::Rtti::Function* raycast = nullptr;
        std::uint64_t preset = kSightBlockerPreset;
        std::size_t parameterCount = 0;
        bool resolved = false;

        __try
        {
            if (AcquireSpatialQueriesSystemOnMainTick(spatialQueriesSystem))
            {
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
                resolved = raycast != nullptr;
            }
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
        }

        Diagnostics::Log("visibility resolver on main tick: raycast=%p params=%zu preset=%s resolved=%d",
                         raycast, parameterCount,
                         preset == kSightBlockerPreset ? "Sight Blocker" : "World Static", resolved ? 1 : 0);
        if (!resolved)
            Diagnostics::Log("visibility disabled: spatial query resolver failed or signature was not 6 parameters");
        return resolved;
    }

    // Returns true when the camera-to-target path is clear. An invocation/result failure is fail-open so the ESP
    // remains usable; acquisition failure is handled by ProcessPendingOnMainTick and leaves work queued.
    bool CastClear(void* spatialQueriesSystem, const float camera[3], const float target[3])
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

    void PublishResult(const Request& request, bool clear)
    {
        AcquireSRWLockExclusive(&g_lock);
        CacheEntry& entry = g_cache[request.entityId % kCacheSize];
        if (!g_worldOpen || request.generation != g_worldGeneration ||
            !Game::EntityTracker::IsWorldReadyForMainTickConsumers())
        {
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
        if (entry.entityId == request.entityId && entry.generation == request.generation && entry.pending)
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
        TickStageReset resetStage;
        g_mainTickStage.store(MainTickStage::WorldGate, std::memory_order_release);
        if (!UpdateWorldGate() || QueueCount() == 0)
            return 0;
        g_mainTickThreadId = GetCurrentThreadId();
        if (!g_loggedFirstMainTick.exchange(true, std::memory_order_acq_rel))
        {
            Diagnostics::Log("visibility first main tick: threadId=%lu hookCreated=%d", g_mainTickThreadId,
                             g_state.hookCreated.load(std::memory_order_acquire) ? 1 : 0);
        }

        g_mainTickStage.store(MainTickStage::ResolveMetadata, std::memory_order_release);
        if (!ResolveSpatialQueryOnMainTick())
            return 0;

        std::size_t processed = 0;
        Request request;
        if (QueueCount() > 0)
        {
            // A spatial-query system is owned by the current world/session. Acquire it after the metadata check and
            // immediately before consuming work; only this local pointer is used for the entire bounded batch below.
            void* spatialQueriesSystem = nullptr;
            if (AcquireSpatialQueriesSystemOnMainTick(spatialQueriesSystem))
            {
                while (processed < kRequestsPerTick && PopRequest(request))
                {
                    if (!IsCurrentRequest(request))
                        continue;
                    g_mainTickEntityId.store(request.entityId, std::memory_order_relaxed);
                    g_mainTickStage.store(MainTickStage::PrimaryCast, std::memory_order_release);
                    bool clear = CastClear(spatialQueriesSystem, request.camera, request.primary);
                    if (!clear && request.hasSecondary && IsCurrentRequest(request))
                    {
                        g_mainTickStage.store(MainTickStage::SecondaryCast, std::memory_order_release);
                        clear = CastClear(spatialQueriesSystem, request.camera, request.secondary);
                    }
                    g_mainTickStage.store(MainTickStage::Publish, std::memory_order_release);
                    PublishResult(request, clear);
                    ++processed;
                }
            }
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
            {
                // TickTotal은 트레이너 detour가 게임 틱에 얹는 총 지연이다. 원본 OnTick 호출은 제외한다.
                Diagnostics::Profile::Scope profileScope(Diagnostics::Profile::Slot::TickTotal);
                Game::EntityTracker::OnGameMainTick();
                {
                    Diagnostics::Profile::Scope playerScope(Diagnostics::Profile::Slot::TickPlayerModifiers);
                    Game::PlayerModifiers::OnGameMainTick();
                }
                {
                    Diagnostics::Profile::Scope visibilityScope(Diagnostics::Profile::Slot::TickVisibility);
                    ProcessPendingOnMainTick();
                }
            }
            Diagnostics::Profile::LogCadence();
            // VEH가 링 버퍼에 적어 둔 예외를 여기서 로그로 옮긴다. 예외 문맥이 아닌 평범한 틱
            // 문맥이라 모듈 조회와 파일 쓰기를 해도 안전하다.
            Diagnostics::DrainExceptionLog();
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

    State Query(std::uint64_t entityId, const float camera[3], const float primary[3], const float secondary[3],
                bool priority)
    {
        if (!camera || !primary || entityId == 0 || !g_state.hookCreated.load(std::memory_order_acquire) ||
            HookLifecycle::IsShuttingDown())
        {
            return State::Unknown;
        }

        const ULONGLONG now = GetTickCount64();
        State state = State::Unknown;

        AcquireSRWLockExclusive(&g_lock);
        if (!g_worldOpen || !Game::EntityTracker::IsWorldReadyForMainTickConsumers())
        {
            ReleaseSRWLockExclusive(&g_lock);
            return State::Unknown;
        }
        CacheEntry& entry = g_cache[entityId % kCacheSize];
        if (entry.entityId != entityId || entry.generation != g_worldGeneration)
        {
            entry = {};
            entry.entityId = entityId;
            entry.generation = g_worldGeneration;
        }
        state = entry.state;
        if (!entry.pending && (entry.state == State::Unknown || now - entry.tick >= kRefreshIntervalMilliseconds))
        {
            if (g_queueCount < kQueueSize)
            {
                // 우선 요청은 큐 머리 앞쪽으로 넣는다. 큐가 가득 찼을 때는 기존과 동일하게 그냥 버려서
                // pending 상태로 남는 항목이 생기지 않게 한다.
                std::size_t slot = (g_queueHead + g_queueCount) % kQueueSize;
                if (priority)
                {
                    g_queueHead = (g_queueHead + kQueueSize - 1) % kQueueSize;
                    slot = g_queueHead;
                }
                Request& request = g_queue[slot];
                request = {};
                request.entityId = entityId;
                request.generation = g_worldGeneration;
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
        stats.available = Game::EntityTracker::IsWorldReadyForMainTickConsumers() &&
                          g_state.hookCreated.load(std::memory_order_acquire) &&
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
        g_state.raycast.store(nullptr, std::memory_order_release);
        g_originalOnTick = nullptr;

        AcquireSRWLockExclusive(&g_lock);
        g_worldOpen = false;
        ++g_worldGeneration;
        g_cache = {};
        g_queue = {};
        g_queueHead = 0;
        g_queueCount = 0;
        ReleaseSRWLockExclusive(&g_lock);

        g_loggedFirstMainTick.store(false, std::memory_order_release);
        g_mainTickThreadId = 0;
        g_lastStatsLogTick = 0;
        g_lastResolveAttempt = 0;
        g_lastSystemAcquireFailureLog = 0;
        g_systemAcquireFailures = 0;
        Diagnostics::Log("visibility state reset: casts=%llu visible=%llu occluded=%llu dropped=%llu",
                         static_cast<unsigned long long>(g_state.totalCasts.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.totalVisible.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.totalOccluded.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_state.droppedRequests.load(std::memory_order_relaxed)));
        return true;
    }
}
