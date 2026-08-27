#include "animation_data.h"
#include "../diagnostics.h"
#include "../framework.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace
{
    constexpr std::uint32_t kGameEngineAddressHash = 0x97F209D6u;
    constexpr std::size_t kAnimationSystemIndex = 7;
    constexpr std::size_t kRuntimeSystemHandleSize = 0x10;
    // world::IRuntimeSystem is 0x48 bytes and the first 16-byte-aligned bucket begins at 0x50.
    constexpr std::size_t kAnimationSystemBucketOffset = 0x50;
    constexpr std::size_t kBucketStride = 0x2E170;
    constexpr std::size_t kAnimatedObjectsOffset = 0x30;
    constexpr std::size_t kAnimationBoundsAnnotatedOffset = 0x4090;
    constexpr std::size_t kAnimationBoundsSdkLayoutOffset = 0x4040;
    constexpr std::size_t kMaximumEntries = 2048;

    struct Vector4
    {
        float x;
        float y;
        float z;
        float w;
    };

    struct Box
    {
        Vector4 minimum;
        Vector4 maximum;
    };

    struct DynArrayLayout
    {
        void* entries;
        std::uint32_t capacity;
        std::uint32_t size;
    };

    struct HashMapNodeList
    {
        std::byte* nodes;
        std::uint32_t capacity;
        std::uint32_t stride;
        std::uint32_t nextIndex;
        std::uint32_t size;
    };

    struct HashMapLayout
    {
        std::uint32_t* indexTable;
        std::uint32_t size;
        std::uint32_t capacity;
        HashMapNodeList nodeList;
    };

    struct AnimatedObjectLayout
    {
        std::byte padding00[0xC0];
        Box objectBounds;
    };

    static_assert(sizeof(HashMapLayout) == 0x28);
    static_assert(offsetof(AnimatedObjectLayout, objectBounds) == 0xC0);

    using ResolveAddressFn = std::uintptr_t (*)(std::uint32_t);

    struct State
    {
        bool attempted = false;
        std::byte* animationSystem = nullptr;
        bool loggedBounds = false;
    };

    State g_state;

    bool IsFinite(float value)
    {
        return std::isfinite(value) && std::abs(value) < 1000000.0f;
    }

    float LengthSquared(float x, float y, float z)
    {
        return x * x + y * y + z * z;
    }

    std::uint32_t HashEntityId(std::uint64_t entityId)
    {
        std::uint32_t hash = 0x811C9DC5u;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&entityId);
        for (std::size_t i = 0; i < sizeof(entityId); ++i)
        {
            hash ^= bytes[i];
            hash *= 0x01000193u;
        }
        return hash;
    }

    bool Initialize()
    {
        if (g_state.attempted)
            return g_state.animationSystem != nullptr;
        g_state.attempted = true;

        HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
        const auto resolve = red4ext
                                 ? reinterpret_cast<ResolveAddressFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                                 : nullptr;
        if (!resolve)
        {
            Diagnostics::Log("animation data unavailable: RED4ext address resolver is not loaded");
            return false;
        }

        const std::uintptr_t enginePointerAddress = resolve(kGameEngineAddressHash);
        void* engine = enginePointerAddress ? *reinterpret_cast<void**>(enginePointerAddress) : nullptr;
        std::byte* framework = engine
                                   ? *reinterpret_cast<std::byte**>(static_cast<std::byte*>(engine) + 0x308)
                                   : nullptr;
        const std::uintptr_t runtimeSceneAddress = framework
                                                       ? *reinterpret_cast<std::uintptr_t*>(framework + 0x18)
                                                       : 0;
        if (runtimeSceneAddress)
        {
            auto* handle = reinterpret_cast<void**>(runtimeSceneAddress +
                                                    kAnimationSystemIndex * kRuntimeSystemHandleSize);
            g_state.animationSystem = static_cast<std::byte*>(handle[0]);
        }

        Diagnostics::Log("animation data initialized: system=%p", g_state.animationSystem);
        return g_state.animationSystem != nullptr;
    }

    bool FindEntry(std::uint64_t entityId, std::byte*& bucket, std::uint32_t& entryIndex)
    {
        constexpr std::uint32_t invalidIndex = 0xFFFFFFFFu;
        const std::uint32_t hash = HashEntityId(entityId);

        for (std::size_t bucketIndex = 0; bucketIndex < 3; ++bucketIndex)
        {
            std::byte* candidate = g_state.animationSystem + kAnimationSystemBucketOffset +
                                   bucketIndex * kBucketStride;
            const auto* map = reinterpret_cast<const HashMapLayout*>(candidate);
            if (!map->indexTable || !map->nodeList.nodes || map->capacity == 0 || map->capacity > 8192 ||
                map->size > map->capacity || map->nodeList.capacity == 0 || map->nodeList.capacity > 8192 ||
                map->nodeList.stride < 0x18 || map->nodeList.stride > 0x80)
            {
                continue;
            }

            std::uint32_t nodeIndex = map->indexTable[hash % map->capacity];
            for (std::uint32_t step = 0; nodeIndex != invalidIndex && step < map->nodeList.capacity; ++step)
            {
                if (nodeIndex >= map->nodeList.capacity)
                    break;
                const std::byte* node = map->nodeList.nodes +
                                        static_cast<std::size_t>(nodeIndex) * map->nodeList.stride;
                std::uint32_t next = invalidIndex;
                std::uint32_t nodeHash = 0;
                std::uint64_t nodeEntityId = 0;
                std::uint32_t value = invalidIndex;
                std::memcpy(&next, node, sizeof(next));
                std::memcpy(&nodeHash, node + 0x4, sizeof(nodeHash));
                std::memcpy(&nodeEntityId, node + 0x8, sizeof(nodeEntityId));
                std::memcpy(&value, node + 0x10, sizeof(value));
                if (nodeHash == hash && nodeEntityId == entityId && value < kMaximumEntries)
                {
                    bucket = candidate;
                    entryIndex = value;
                    return true;
                }
                nodeIndex = next;
            }

            // ent::EntityID uses an engine-side hasher that is not exposed by the public SDK. The generic
            // FNV path above is fast when it agrees; walking the map's active chains is the authoritative fallback.
            for (std::uint32_t tableIndex = 0; tableIndex < map->capacity; ++tableIndex)
            {
                nodeIndex = map->indexTable[tableIndex];
                for (std::uint32_t step = 0; nodeIndex != invalidIndex && step < map->nodeList.capacity; ++step)
                {
                    if (nodeIndex >= map->nodeList.capacity)
                        break;
                    const std::byte* node = map->nodeList.nodes +
                                            static_cast<std::size_t>(nodeIndex) * map->nodeList.stride;
                    std::uint32_t next = invalidIndex;
                    std::uint64_t nodeEntityId = 0;
                    std::uint32_t value = invalidIndex;
                    std::memcpy(&next, node, sizeof(next));
                    std::memcpy(&nodeEntityId, node + 0x8, sizeof(nodeEntityId));
                    std::memcpy(&value, node + 0x10, sizeof(value));
                    if (nodeEntityId == entityId && value < kMaximumEntries)
                    {
                        bucket = candidate;
                        entryIndex = value;
                        return true;
                    }
                    nodeIndex = next;
                }
            }
        }
        return false;
    }

    bool IsPlausibleBounds(const Box& box, const float reference[3], float& score)
    {
        const float minimum[3] = {box.minimum.x, box.minimum.y, box.minimum.z};
        const float maximum[3] = {box.maximum.x, box.maximum.y, box.maximum.z};
        for (unsigned i = 0; i < 3; ++i)
        {
            if (!IsFinite(minimum[i]) || !IsFinite(maximum[i]) || minimum[i] > maximum[i])
                return false;
        }

        const float extentX = maximum[0] - minimum[0];
        const float extentY = maximum[1] - minimum[1];
        const float extentZ = maximum[2] - minimum[2];
        if (extentX < 0.02f || extentY < 0.02f || extentZ < 0.15f || extentX > 15.0f || extentY > 15.0f ||
            extentZ > 15.0f)
        {
            return false;
        }

        const float centerX = (minimum[0] + maximum[0]) * 0.5f;
        const float centerY = (minimum[1] + maximum[1]) * 0.5f;
        const float centerZ = (minimum[2] + maximum[2]) * 0.5f;
        score = LengthSquared(centerX - reference[0], centerY - reference[1], centerZ - reference[2]);
        return score < 400.0f;
    }

    bool ReadBounds(std::byte* bucket, std::uint32_t entryIndex, const float reference[3], Box& output)
    {
        std::array<const Box*, 3> candidates{};
        candidates[0] = reinterpret_cast<const Box*>(bucket + kAnimationBoundsAnnotatedOffset) + entryIndex;
        candidates[1] = reinterpret_cast<const Box*>(bucket + kAnimationBoundsSdkLayoutOffset) + entryIndex;

        auto* animatedObject = *reinterpret_cast<AnimatedObjectLayout**>(
            bucket + kAnimatedObjectsOffset + static_cast<std::size_t>(entryIndex) * sizeof(void*));
        candidates[2] = animatedObject ? &animatedObject->objectBounds : nullptr;

        float bestScore = (std::numeric_limits<float>::max)();
        bool found = false;
        for (const Box* candidate : candidates)
        {
            if (!candidate)
                continue;
            float score = 0.0f;
            if (IsPlausibleBounds(*candidate, reference, score) && score < bestScore)
            {
                output = *candidate;
                bestScore = score;
                found = true;
            }
        }
        return found;
    }

}

namespace Game::AnimationData
{
    bool ReadVisualData(std::uint64_t entityId, const float entityPosition[3], VisualData& output)
    {
        output = {};
        if (!entityPosition)
            return false;

        __try
        {
            if (!Initialize())
                return false;

            std::byte* bucket = nullptr;
            std::uint32_t entryIndex = 0;
            if (!FindEntry(entityId, bucket, entryIndex))
                return false;

            Box bounds{};
            if (!ReadBounds(bucket, entryIndex, entityPosition, bounds))
                return false;

            output.hasBounds = true;
            output.boundsMinimum[0] = bounds.minimum.x;
            output.boundsMinimum[1] = bounds.minimum.y;
            output.boundsMinimum[2] = bounds.minimum.z;
            output.boundsMaximum[0] = bounds.maximum.x;
            output.boundsMaximum[1] = bounds.maximum.y;
            output.boundsMaximum[2] = bounds.maximum.z;
            // MetaRig::boneTransforms contains the reference/bind pose. Live pose points are populated from
            // SlotComponent::GetSlotTransform by the entity tracker instead.
            output.skeletonSegmentCount = 0;

            if (!g_state.loggedBounds)
            {
                Diagnostics::Log("animation bounds resolved: entity=%016llX index=%u min=(%.2f,%.2f,%.2f) "
                                 "max=(%.2f,%.2f,%.2f)",
                                 static_cast<unsigned long long>(entityId), entryIndex, bounds.minimum.x,
                                 bounds.minimum.y, bounds.minimum.z, bounds.maximum.x, bounds.maximum.y,
                                 bounds.maximum.z);
                g_state.loggedBounds = true;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}
