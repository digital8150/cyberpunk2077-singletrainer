#include "entity_tracker.h"
#include "animation_data.h"
#include "rtti_invoker.h"
#include "signature_scanner.h"
#include "../diagnostics.h"
#include "../framework.h"
#include "../hooks/hook_lifecycle.h"

#include <MinHook.h>

#include <atomic>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace
{
    // RED4ext/CET 공식 주소 해시: world::RuntimeEntityRegistry::RegisterEntity.
    constexpr std::uint32_t kRegisterEntityAddressHash = 2840271332u;
    constexpr std::uint32_t kCClassGetPropertyAddressHash = 0x8F031512u;
    constexpr std::uint32_t kRenderProxySetHighlightParamsAddressHash = 1093803822u;
    constexpr std::uint32_t kRenderProxySetScanningStateAddressHash = 2838044016u;

    // Cyberpunk 2077 2.31 / internal 3.0.80.51928에서 검증. 상대 call 대상만 wildcard 처리했으며
    // tools/scripts/memtool.py aobscan으로 Cyberpunk2077.exe 내 정확히 1개 매치를 확인했다.
    constexpr std::uint8_t kRegisterEntityPattern[] = {
        0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18,
        0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x60,
        0x48, 0x8B, 0xF1, 0x48, 0x8B, 0xFA, 0x48, 0x83, 0xC1, 0x48,
        0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x47, 0x48,
    };
    constexpr char kRegisterEntityMask[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxx";
    static_assert(sizeof(kRegisterEntityPattern) == sizeof(kRegisterEntityMask) - 1);

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

    struct ClassLayout
    {
        std::byte pad00[0x10];
        ClassLayout* parent;
        std::uint64_t nameHash;
    };
    static_assert(offsetof(ClassLayout, parent) == 0x10);
    static_assert(offsetof(ClassLayout, nameHash) == 0x18);

    struct DynArrayLayout
    {
        void** entries;
        std::uint32_t capacity;
        std::uint32_t size;
    };
    static_assert(sizeof(DynArrayLayout) == 0x10);

    struct PropertyLayout
    {
        void* type;
        std::uint64_t nameHash;
        std::uint64_t groupHash;
        ClassLayout* parent;
        std::uint32_t valueOffset;
        std::uint32_t pad24;
        std::uint64_t flags;
    };
    static_assert(offsetof(PropertyLayout, valueOffset) == 0x20);
    static_assert(offsetof(PropertyLayout, flags) == 0x28);

    struct BoolPropertyLocation
    {
        std::uint32_t offset = 0;
        bool inValueHolder = false;
        bool found = false;
    };

    struct ClassificationLayout
    {
        BoolPropertyLocation civilian;
        BoolPropertyLocation police;
        BoolPropertyLocation ganger;
        bool logged = false;
    };

    struct FunctionLayout
    {
        void* vtable;
        std::uint64_t fullNameHash;
        std::uint64_t shortNameHash;
        std::byte pad18[0x80 - 0x18];
        const std::uint8_t* bytecode;
        std::uint32_t bytecodeSize;
    };
    static_assert(offsetof(FunctionLayout, shortNameHash) == 0x10);
    static_assert(offsetof(FunctionLayout, bytecode) == 0x80);
    static_assert(offsetof(FunctionLayout, bytecodeSize) == 0x88);

    struct WorldTransformLayout
    {
        std::int32_t x;
        std::int32_t y;
        std::int32_t z;
        std::byte pad0C[4];
        float orientation[4];
    };
    static_assert(sizeof(WorldTransformLayout) == 0x20);

    struct PlacedComponentLayout
    {
        std::byte pad00[0xE0];
        WorldTransformLayout worldTransform;
    };
    static_assert(offsetof(PlacedComponentLayout, worldTransform) == 0xE0);

    struct EntityLayout
    {
        std::byte pad00[0x30];
        ClassLayout* nativeType;
        void* valueHolder;
        std::byte pad40[0x48 - 0x40];
        std::uint64_t entityId;
        std::byte pad50[0xA0 - 0x50];
        struct
        {
            std::byte* entries;
            std::uint32_t capacity;
            std::uint32_t size;
        } components;
        PlacedComponentLayout* transformComponent;
    };
    static_assert(offsetof(EntityLayout, nativeType) == 0x30);
    static_assert(offsetof(EntityLayout, valueHolder) == 0x38);
    static_assert(offsetof(EntityLayout, entityId) == 0x48);
    static_assert(offsetof(EntityLayout, components) == 0xA0);
    static_assert(offsetof(EntityLayout, transformComponent) == 0xB0);

    struct HighlightParams
    {
        bool seeThroughWalls;
        std::uint8_t patternType;
        std::uint8_t fillIndex;
        std::uint8_t outlineIndex;
        float opacity;
        bool forced;
    };
    static_assert(offsetof(HighlightParams, opacity) == 0x4);

    using SetHighlightParamsFn = std::uint8_t (*)(void*, const HighlightParams&);
    using SetScanningStateFn = std::uint8_t (*)(void*, std::int8_t);

    enum class PuppetKind
    {
        None,
        Npc,
        Player,
    };

    struct TrackedPuppet
    {
        EntityLayout* entity = nullptr;
        std::uint64_t entityId = 0;
        std::uint64_t sequence = 0;
        Game::AnimationData::VisualData visual;
        ULONGLONG visualUpdatedAt = 0;
    };

    constexpr std::size_t kMaxTrackedPuppets = 256;

    using RegisterEntityFn = void (*)(void* registry, EntityLayout* entity);
    using ResolveAddressFn = std::uintptr_t (*)(std::uint32_t);
    RegisterEntityFn g_originalRegisterEntity = nullptr;

    std::atomic_bool g_hookCreated{false};
    std::atomic<void*> g_registry{nullptr};
    std::atomic_uint64_t g_registered{0};
    std::atomic_uint64_t g_positioned{0};
    std::atomic_uint64_t g_puppets{0};
    std::atomic_uint64_t g_trackedPuppets{0};
    std::atomic_uint64_t g_trackedCivilians{0};
    std::atomic_uint64_t g_trackedEnemies{0};
    std::atomic_uint64_t g_trackedPolice{0};
    SRWLOCK g_lastEntityLock = SRWLOCK_INIT;
    std::uint64_t g_lastEntityId = 0;
    float g_lastPosition[3]{};
    bool g_hasLastPuppet = false;
    std::uint64_t g_lastPuppetId = 0;
    float g_lastPuppetPosition[3]{};
    SRWLOCK g_puppetListLock = SRWLOCK_INIT;
    std::array<TrackedPuppet, kMaxTrackedPuppets> g_puppetList{};
    std::uint64_t g_puppetSequence = 0;
    ClassificationLayout g_classificationLayout;
    using GetPropertyFn = PropertyLayout* (*)(ClassLayout*, std::uint64_t);
    GetPropertyFn g_getProperty = nullptr;
    bool g_getPropertyAttempted = false;
    SetHighlightParamsFn g_setHighlightParams = nullptr;
    SetScanningStateFn g_setScanningState = nullptr;
    bool g_highlightResolveAttempted = false;
    bool g_nativeHighlightActive = false;
    ULONGLONG g_lastNativeHighlightTick = 0;

    GetPropertyFn ResolveGetProperty()
    {
        if (g_getPropertyAttempted)
            return g_getProperty;
        g_getPropertyAttempted = true;

        using ResolveAddressFn = std::uintptr_t (*)(std::uint32_t);
        HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
        const auto resolve = red4ext
                                 ? reinterpret_cast<ResolveAddressFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                                 : nullptr;
        if (resolve)
            g_getProperty = reinterpret_cast<GetPropertyFn>(resolve(kCClassGetPropertyAddressHash));
        Diagnostics::Log("CClass::GetProperty resolver: address=%p", reinterpret_cast<void*>(g_getProperty));
        return g_getProperty;
    }

    bool FindBoolProperty(const ClassLayout* type, std::uint64_t propertyName, BoolPropertyLocation& result)
    {
        constexpr std::uint64_t kInValueHolderFlag = 1ull << 21;
        if (GetPropertyFn getProperty = ResolveGetProperty())
        {
            const PropertyLayout* property = getProperty(const_cast<ClassLayout*>(type), propertyName);
            if (property)
            {
                result.offset = property->valueOffset;
                result.inValueHolder = (property->flags & kInValueHolderFlag) != 0;
                result.found = true;
                return true;
            }
        }

        // Fallback for environments without RED4ext's address resolver. The engine helper above is preferred because
        // it also handles overridden/script properties whose storage is not necessarily in the immediate class list.
        for (unsigned depth = 0; type && depth < 24; ++depth, type = type->parent)
        {
            const auto* properties = reinterpret_cast<const DynArrayLayout*>(
                reinterpret_cast<const std::byte*>(type) + 0x28);
            if (!properties->entries || properties->size > properties->capacity || properties->size > 4096)
                continue;

            for (std::uint32_t i = 0; i < properties->size; ++i)
            {
                const auto* property = static_cast<const PropertyLayout*>(properties->entries[i]);
                if (!property || property->nameHash != propertyName)
                    continue;
                result.offset = property->valueOffset;
                result.inValueHolder = (property->flags & kInValueHolderFlag) != 0;
                result.found = true;
                return true;
            }
        }
        return false;
    }

    const FunctionLayout* FindFunction(const ClassLayout* type, std::uint64_t functionName)
    {
        for (unsigned depth = 0; type && depth < 24; ++depth, type = type->parent)
        {
            const auto* functions = reinterpret_cast<const DynArrayLayout*>(
                reinterpret_cast<const std::byte*>(type) + 0x48);
            if (!functions->entries || functions->size > functions->capacity || functions->size > 8192)
                continue;

            for (std::uint32_t i = 0; i < functions->size; ++i)
            {
                const auto* function = static_cast<const FunctionLayout*>(functions->entries[i]);
                if (function && function->shortNameHash == functionName)
                    return function;
            }
        }
        return nullptr;
    }

    bool FindBoolPropertyFromGetter(const ClassLayout* type, std::uint64_t propertyName,
                                   const char* getterName, BoolPropertyLocation& result)
    {
        constexpr std::uint64_t kInValueHolderFlag = 1ull << 21;
        const FunctionLayout* function = FindFunction(type, Fnv1a64(getterName));
        if (!function || !function->bytecode || function->bytecodeSize < 10 || function->bytecodeSize > 4096)
            return false;

        // REDengine links a trivial scripted getter as: Return(0x27), ObjectField(0x1A), CProperty*.
        // Using the linked property is safer than invoking the script VM from the render thread and also survives
        // storage offsets moving between patches.
        if (function->bytecode[0] != 0x27 || function->bytecode[1] != 0x1A)
            return false;

        const PropertyLayout* property = nullptr;
        memcpy(&property, function->bytecode + 2, sizeof(property));
        if (!property || property->nameHash != propertyName || property->valueOffset > 0x10000)
            return false;

        result.offset = property->valueOffset;
        result.inValueHolder = (property->flags & kInValueHolderFlag) != 0;
        result.found = true;
        return true;
    }

    void ResolveClassificationLayout(const ClassLayout* type)
    {
        if (!g_classificationLayout.civilian.found)
        {
            constexpr std::uint64_t name = Fnv1a64("isCivilian");
            if (!FindBoolProperty(type, name, g_classificationLayout.civilian))
                FindBoolPropertyFromGetter(type, name, "IsCharacterCivilian", g_classificationLayout.civilian);
        }
        if (!g_classificationLayout.police.found)
        {
            constexpr std::uint64_t name = Fnv1a64("isPolice");
            if (!FindBoolProperty(type, name, g_classificationLayout.police))
                FindBoolPropertyFromGetter(type, name, "IsCharacterPolice", g_classificationLayout.police);
        }
        if (!g_classificationLayout.ganger.found)
        {
            constexpr std::uint64_t name = Fnv1a64("isGanger");
            if (!FindBoolProperty(type, name, g_classificationLayout.ganger))
                FindBoolPropertyFromGetter(type, name, "IsCharacterGanger", g_classificationLayout.ganger);
        }

        if (!g_classificationLayout.logged && g_classificationLayout.civilian.found &&
            g_classificationLayout.police.found && g_classificationLayout.ganger.found)
        {
            Diagnostics::Log("NPC classification RTTI resolved: civilian=0x%X/%d police=0x%X/%d ganger=0x%X/%d",
                             g_classificationLayout.civilian.offset,
                             g_classificationLayout.civilian.inValueHolder ? 1 : 0,
                             g_classificationLayout.police.offset,
                             g_classificationLayout.police.inValueHolder ? 1 : 0,
                             g_classificationLayout.ganger.offset,
                             g_classificationLayout.ganger.inValueHolder ? 1 : 0);
            g_classificationLayout.logged = true;
        }
    }

    bool ReadBoolProperty(const EntityLayout* entity, const BoolPropertyLocation& property)
    {
        if (!property.found)
            return false;
        const std::byte* base = property.inValueHolder
                                    ? static_cast<const std::byte*>(entity->valueHolder)
                                    : reinterpret_cast<const std::byte*>(entity);
        return base && *reinterpret_cast<const bool*>(base + property.offset);
    }

    Game::EntityTracker::NpcCategory ClassifyNpc(EntityLayout* entity)
    {
        ResolveClassificationLayout(entity->nativeType);
        if (ReadBoolProperty(entity, g_classificationLayout.police))
            return Game::EntityTracker::NpcCategory::Police;
        if (ReadBoolProperty(entity, g_classificationLayout.civilian))
            return Game::EntityTracker::NpcCategory::Civilian;
        if (ReadBoolProperty(entity, g_classificationLayout.ganger))
            return Game::EntityTracker::NpcCategory::Enemy;
        return Game::EntityTracker::NpcCategory::Other;
    }

    PuppetKind ClassifyPuppet(const ClassLayout* type)
    {
        constexpr std::uint64_t playerTypes[] = {
            Fnv1a64("PlayerPuppet"),
            Fnv1a64("gamePlayerPuppet"),
        };
        constexpr std::uint64_t puppetTypes[] = {
            Fnv1a64("gamePuppet"),
            Fnv1a64("gamePuppetBase"),
            Fnv1a64("gameNPCPuppet"),
            Fnv1a64("NPCPuppet"),
            Fnv1a64("ScriptedPuppet"),
        };

        bool isPuppet = false;
        for (unsigned depth = 0; type && depth < 24; ++depth, type = type->parent)
        {
            for (const std::uint64_t hash : playerTypes)
            {
                if (type->nameHash == hash)
                    return PuppetKind::Player;
            }
            for (const std::uint64_t hash : puppetTypes)
            {
                if (type->nameHash == hash)
                    isPuppet = true;
            }
        }
        return isPuppet ? PuppetKind::Npc : PuppetKind::None;
    }

    bool IsClassOrDerived(const ClassLayout* type, std::uint64_t nameHash)
    {
        for (unsigned depth = 0; type && depth < 24; ++depth, type = type->parent)
        {
            if (type->nameHash == nameHash)
                return true;
        }
        return false;
    }

    template<typename Callback>
    void ForEachComponent(const EntityLayout* entity, Callback&& callback)
    {
        if (!entity || !entity->components.entries || entity->components.size > entity->components.capacity ||
            entity->components.size > 512)
        {
            return;
        }
        for (std::uint32_t i = 0; i < entity->components.size; ++i)
        {
            std::byte* handle = entity->components.entries + static_cast<std::size_t>(i) * 0x10;
            void* component = *reinterpret_cast<void**>(handle);
            if (component)
                callback(static_cast<std::byte*>(component));
        }
    }

    bool ReadSlotPosition(const EntityLayout* entity, std::uint64_t slotName, float output[3])
    {
        constexpr std::uint64_t slotComponentName = Fnv1a64("entSlotComponent");
        constexpr std::uint64_t getSlotTransformName = Fnv1a64("GetSlotTransform");
        bool found = false;
        ForEachComponent(entity, [&](std::byte* component) {
            if (found)
                return;
            auto* type = Game::Rtti::NativeType(component);
            if (!Game::Rtti::IsClassOrDerived(type, slotComponentName))
                return;
            Game::Rtti::Function* function = Game::Rtti::FindFunction(type, getSlotTransformName);
            if (!function)
                return;

            WorldTransformLayout transform{};
            bool result = false;
            Game::Rtti::Argument arguments[] = {{&slotName}, {&transform}};
            if (!Game::Rtti::Invoke(function, component, arguments, 2, &result) || !result)
                return;

            constexpr float fixedPointScale = 1.0f / static_cast<float>(2 << 16);
            output[0] = static_cast<float>(transform.x) * fixedPointScale;
            output[1] = static_cast<float>(transform.y) * fixedPointScale;
            output[2] = static_cast<float>(transform.z) * fixedPointScale;
            found = std::isfinite(output[0]) && std::isfinite(output[1]) && std::isfinite(output[2]) &&
                    std::abs(output[0]) < 1000000.0f && std::abs(output[1]) < 1000000.0f &&
                    std::abs(output[2]) < 1000000.0f;
        });
        return found;
    }

    void AddSkeletonSegment(Game::AnimationData::VisualData& visual, const float start[3], const float end[3])
    {
        if (visual.skeletonSegmentCount >= Game::AnimationData::kMaxSkeletonSegments)
            return;
        auto& segment = visual.skeletonSegments[visual.skeletonSegmentCount++];
        memcpy(segment.start, start, sizeof(segment.start));
        memcpy(segment.end, end, sizeof(segment.end));
    }

    void ReadCurrentPoseSlots(const EntityLayout* entity, Game::AnimationData::VisualData& visual)
    {
        struct PosePoint
        {
            bool valid = false;
            float position[3]{};
        };

        PosePoint head, chest, hips, rightHand, leftLeg, rightLeg;
        head.valid = ReadSlotPosition(entity, Fnv1a64("Head"), head.position);
        chest.valid = ReadSlotPosition(entity, Fnv1a64("Chest"), chest.position);
        hips.valid = ReadSlotPosition(entity, Fnv1a64("Hips"), hips.position);
        rightHand.valid = ReadSlotPosition(entity, Fnv1a64("RightHand"), rightHand.position);
        leftLeg.valid = ReadSlotPosition(entity, Fnv1a64("LegLeft"), leftLeg.position);
        rightLeg.valid = ReadSlotPosition(entity, Fnv1a64("LegRight"), rightLeg.position);

        visual.hasHeadPosition = head.valid;
        if (head.valid)
            memcpy(visual.headPosition, head.position, sizeof(visual.headPosition));
        visual.skeletonSegmentCount = 0;
        if (hips.valid && chest.valid)
            AddSkeletonSegment(visual, hips.position, chest.position);
        if (chest.valid && head.valid)
            AddSkeletonSegment(visual, chest.position, head.position);
        if (chest.valid && rightHand.valid)
            AddSkeletonSegment(visual, chest.position, rightHand.position);
        if (hips.valid && leftLeg.valid)
            AddSkeletonSegment(visual, hips.position, leftLeg.position);
        if (hips.valid && rightLeg.valid)
            AddSkeletonSegment(visual, hips.position, rightLeg.position);
    }

    bool IsDead(const EntityLayout* entity)
    {
        constexpr std::uint64_t corpseComponentNames[] = {
            Fnv1a64("entCorpseComponent"),
            Fnv1a64("CorpseComponent"),
        };
        bool dead = false;
        ForEachComponent(entity, [&](std::byte* component) {
            const auto* type = *reinterpret_cast<ClassLayout**>(component + 0x30);
            for (const std::uint64_t name : corpseComponentNames)
                dead = dead || IsClassOrDerived(type, name);
        });
        return dead;
    }

    bool ReadTransform(const EntityLayout* entity, float position[3], float orientation[4])
    {
        if (!entity || !entity->transformComponent)
            return false;

        constexpr float kFixedPointScale = 1.0f / static_cast<float>(2 << 16);
        const WorldTransformLayout& transform = entity->transformComponent->worldTransform;
        position[0] = static_cast<float>(transform.x) * kFixedPointScale;
        position[1] = static_cast<float>(transform.y) * kFixedPointScale;
        position[2] = static_cast<float>(transform.z) * kFixedPointScale;
        memcpy(orientation, transform.orientation, sizeof(transform.orientation));
        const float orientationLength = orientation[0] * orientation[0] + orientation[1] * orientation[1] +
                                        orientation[2] * orientation[2] + orientation[3] * orientation[3];
        return std::isfinite(position[0]) && std::isfinite(position[1]) && std::isfinite(position[2]) &&
               std::abs(position[0]) < 1000000.0f && std::abs(position[1]) < 1000000.0f &&
               std::abs(position[2]) < 1000000.0f && std::isfinite(orientationLength) &&
               orientationLength > 0.01f && orientationLength < 4.0f;
    }

    void TrackPuppet(EntityLayout* entity)
    {
        AcquireSRWLockExclusive(&g_puppetListLock);

        TrackedPuppet* target = nullptr;
        TrackedPuppet* oldest = &g_puppetList[0];
        for (TrackedPuppet& tracked : g_puppetList)
        {
            if (tracked.entityId == entity->entityId)
            {
                target = &tracked;
                break;
            }
            if (!tracked.entity && !target)
                target = &tracked;
            if (tracked.sequence < oldest->sequence)
                oldest = &tracked;
        }

        if (!target)
            target = oldest;
        if (target->entityId != entity->entityId)
            *target = {};
        target->entity = entity;
        target->entityId = entity->entityId;
        target->sequence = ++g_puppetSequence;

        std::uint64_t count = 0;
        for (const TrackedPuppet& tracked : g_puppetList)
            count += tracked.entity != nullptr ? 1u : 0u;
        g_trackedPuppets.store(count, std::memory_order_release);
        ReleaseSRWLockExclusive(&g_puppetListLock);
    }

    bool TrySnapshot(TrackedPuppet& tracked, Game::EntityTracker::PuppetSnapshot& snapshot)
    {
        // Game streaming can free/reuse an entity independently of our list. Validate all identity data at the
        // point of use and contain a stale-pointer access; no raw pointer leaves this function.
        __try
        {
            EntityLayout* entity = tracked.entity;
            if (!entity || entity->entityId != tracked.entityId ||
                ClassifyPuppet(entity->nativeType) != PuppetKind::Npc)
            {
                return false;
            }

            float position[3]{};
            float orientation[4]{};
            if (!ReadTransform(entity, position, orientation))
                return false;

            snapshot.entityId = tracked.entityId;
            snapshot.position[0] = position[0];
            snapshot.position[1] = position[1];
            snapshot.position[2] = position[2];
            memcpy(snapshot.orientation, orientation, sizeof(orientation));
            snapshot.category = ClassifyNpc(entity);
            snapshot.isDead = IsDead(entity);
            const ULONGLONG now = GetTickCount64();
            if (tracked.visualUpdatedAt == 0 || now - tracked.visualUpdatedAt >= 33)
            {
                Game::AnimationData::VisualData refreshed;
                Game::AnimationData::ReadVisualData(snapshot.entityId, snapshot.position, refreshed);
                ReadCurrentPoseSlots(entity, refreshed);
                tracked.visual = refreshed;
                tracked.visualUpdatedAt = now;
            }
            snapshot.visual = tracked.visual;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void CaptureEntity(EntityLayout* entity)
    {
        if (!entity)
            return;

        const std::uint64_t total = g_registered.fetch_add(1, std::memory_order_relaxed) + 1;
        const ClassLayout* nativeType = entity->nativeType;
        const std::uint64_t typeHash = nativeType ? nativeType->nameHash : 0;
        const PuppetKind puppetKind = ClassifyPuppet(nativeType);
        const bool puppet = puppetKind == PuppetKind::Npc;
        if (puppet)
            g_puppets.fetch_add(1, std::memory_order_relaxed);

        float position[3]{};
        float orientation[4]{};
        bool hasPosition = false;
        if (ReadTransform(entity, position, orientation))
        {
            hasPosition = true;
            g_positioned.fetch_add(1, std::memory_order_relaxed);
        }

        if (puppet && hasPosition)
            TrackPuppet(entity);

        AcquireSRWLockExclusive(&g_lastEntityLock);
        g_lastEntityId = entity->entityId;
        g_lastPosition[0] = position[0];
        g_lastPosition[1] = position[1];
        g_lastPosition[2] = position[2];
        if (puppet && hasPosition)
        {
            g_hasLastPuppet = true;
            g_lastPuppetId = entity->entityId;
            g_lastPuppetPosition[0] = position[0];
            g_lastPuppetPosition[1] = position[1];
            g_lastPuppetPosition[2] = position[2];
        }
        ReleaseSRWLockExclusive(&g_lastEntityLock);

        if (total <= 5 || (total & (total - 1)) == 0)
        {
            Diagnostics::Log("entity registered: total=%llu ptr=%p id=0x%llX typeHash=0x%llX puppet=%d "
                             "positioned=%d pos=(%.2f, %.2f, %.2f)",
                             static_cast<unsigned long long>(total), entity,
                             static_cast<unsigned long long>(entity->entityId),
                             static_cast<unsigned long long>(typeHash), puppet ? 1 : 0,
                             hasPosition ? 1 : 0, position[0], position[1], position[2]);
        }
    }

    bool ResolveHighlightFunctions()
    {
        if (g_highlightResolveAttempted)
            return g_setHighlightParams && g_setScanningState;
        g_highlightResolveAttempted = true;

        HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
        const auto resolve = red4ext
                                 ? reinterpret_cast<ResolveAddressFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                                 : nullptr;
        if (resolve)
        {
            g_setHighlightParams = reinterpret_cast<SetHighlightParamsFn>(
                resolve(kRenderProxySetHighlightParamsAddressHash));
            g_setScanningState = reinterpret_cast<SetScanningStateFn>(
                resolve(kRenderProxySetScanningStateAddressHash));
        }
        Diagnostics::Log("native highlight resolver: params=%p scanning=%p",
                         reinterpret_cast<void*>(g_setHighlightParams),
                         reinterpret_cast<void*>(g_setScanningState));
        return g_setHighlightParams && g_setScanningState;
    }

    void SetEntityNativeHighlight(EntityLayout* entity, bool enabled)
    {
        constexpr std::uint64_t skinnedMeshName = Fnv1a64("entSkinnedMeshComponent");
        constexpr std::uint64_t morphMeshName = Fnv1a64("entMorphTargetSkinnedMeshComponent");
        const HighlightParams params = enabled ? HighlightParams{true, 0, 0, 1, 1.0f, true}
                                               : HighlightParams{false, 0, 0, 0, 0.0f, false};
        ForEachComponent(entity, [&](std::byte* component) {
            const auto* type = *reinterpret_cast<ClassLayout**>(component + 0x30);
            std::size_t proxyOffset = 0;
            if (IsClassOrDerived(type, morphMeshName))
                proxyOffset = 0x1E8;
            else if (IsClassOrDerived(type, skinnedMeshName))
                proxyOffset = 0x1E0;
            if (proxyOffset == 0)
                return;

            void* proxy = *reinterpret_cast<void**>(component + proxyOffset);
            if (!proxy)
                return;
            g_setHighlightParams(proxy, params);
            g_setScanningState(proxy, enabled ? 4 : 0); // rendPostFx_ScanningState::Complete / Off
        });
    }

    void HookRegisterEntity(void* registry, EntityLayout* entity)
    {
        HookLifecycle::CallbackGuard callback;
        g_originalRegisterEntity(registry, entity);
        if (!HookLifecycle::IsShuttingDown())
        {
            void* expected = nullptr;
            if (g_registry.compare_exchange_strong(expected, registry, std::memory_order_release,
                                                   std::memory_order_relaxed))
            {
                Diagnostics::Log("runtime entity registry captured: registry=%p", registry);
            }
            CaptureEntity(entity);
        }
    }

    std::uint8_t* ResolveRegisterEntity()
    {
        using ResolveAddressFn = std::uintptr_t (*)(std::uint32_t);
        if (HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll"))
        {
            const auto resolve = reinterpret_cast<ResolveAddressFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress"));
            if (resolve)
            {
                if (const std::uintptr_t address = resolve(kRegisterEntityAddressHash))
                {
                    Diagnostics::Log("RegisterEntity resolved through RED4ext: address=%p", reinterpret_cast<void*>(address));
                    return reinterpret_cast<std::uint8_t*>(address);
                }
            }
        }

        const auto scan = Game::Signatures::FindInText(GetModuleHandleW(nullptr), kRegisterEntityPattern,
                                                       kRegisterEntityMask, sizeof(kRegisterEntityPattern));
        Diagnostics::Log("RegisterEntity signature scan: matches=%zu address=%p", scan.matches, scan.address);
        return scan.matches == 1 ? scan.address : nullptr;
    }
}

namespace Game::EntityTracker
{
    bool CreateHook()
    {
        std::uint8_t* target = ResolveRegisterEntity();
        if (!target)
        {
            Diagnostics::Log("entity tracker disabled: RegisterEntity address unavailable");
            return false;
        }

        const MH_STATUS status = MH_CreateHook(target, &HookRegisterEntity,
                                               reinterpret_cast<void**>(&g_originalRegisterEntity));
        if (status != MH_OK)
        {
            Diagnostics::Log("MH_CreateHook(RegisterEntity) failed: %s (%d)", MH_StatusToString(status), status);
            return false;
        }

        g_hookCreated.store(true, std::memory_order_release);
        Diagnostics::Log("entity tracker hook created: target=%p", target);
        return true;
    }

    void Shutdown()
    {
        if (g_nativeHighlightActive)
            UpdateNativeHighlights(false, false, false, false, false, false);
        g_hookCreated.store(false, std::memory_order_release);
        g_registry.store(nullptr, std::memory_order_release);
        g_originalRegisterEntity = nullptr;
    }

    Stats GetStats()
    {
        Stats result;
        result.hookCreated = g_hookCreated.load(std::memory_order_acquire);
        result.registered = g_registered.load(std::memory_order_relaxed);
        result.positioned = g_positioned.load(std::memory_order_relaxed);
        result.puppets = g_puppets.load(std::memory_order_relaxed);
        result.trackedPuppets = g_trackedPuppets.load(std::memory_order_acquire);
        result.trackedCivilians = g_trackedCivilians.load(std::memory_order_acquire);
        result.trackedEnemies = g_trackedEnemies.load(std::memory_order_acquire);
        result.trackedPolice = g_trackedPolice.load(std::memory_order_acquire);

        AcquireSRWLockShared(&g_lastEntityLock);
        result.lastEntityId = g_lastEntityId;
        result.lastPosition[0] = g_lastPosition[0];
        result.lastPosition[1] = g_lastPosition[1];
        result.lastPosition[2] = g_lastPosition[2];
        result.hasLastPuppet = g_hasLastPuppet;
        result.lastPuppetId = g_lastPuppetId;
        result.lastPuppetPosition[0] = g_lastPuppetPosition[0];
        result.lastPuppetPosition[1] = g_lastPuppetPosition[1];
        result.lastPuppetPosition[2] = g_lastPuppetPosition[2];
        ReleaseSRWLockShared(&g_lastEntityLock);
        return result;
    }

    std::size_t GetPuppetSnapshots(PuppetSnapshot* output, std::size_t capacity)
    {
        if (!output || capacity == 0)
            return 0;

        std::size_t count = 0;
        std::uint64_t trackedCount = 0;
        std::uint64_t civilians = 0;
        std::uint64_t enemies = 0;
        std::uint64_t police = 0;
        AcquireSRWLockExclusive(&g_puppetListLock);
        for (TrackedPuppet& tracked : g_puppetList)
        {
            if (!tracked.entity)
                continue;

            PuppetSnapshot snapshot;
            if (!TrySnapshot(tracked, snapshot))
            {
                tracked = {};
                continue;
            }

            ++trackedCount;
            civilians += snapshot.category == NpcCategory::Civilian ? 1u : 0u;
            enemies += snapshot.category == NpcCategory::Enemy ? 1u : 0u;
            police += snapshot.category == NpcCategory::Police ? 1u : 0u;
            if (count < capacity)
                output[count++] = snapshot;
        }
        g_trackedPuppets.store(trackedCount, std::memory_order_release);
        g_trackedCivilians.store(civilians, std::memory_order_release);
        g_trackedEnemies.store(enemies, std::memory_order_release);
        g_trackedPolice.store(police, std::memory_order_release);
        ReleaseSRWLockExclusive(&g_puppetListLock);
        return count;
    }

    void UpdateNativeHighlights(bool enabled, bool showCivilians, bool showEnemies, bool showPolice,
                                bool showUnclassified, bool hideDead)
    {
        if (!enabled && !g_nativeHighlightActive)
            return;
        if (!ResolveHighlightFunctions())
            return;

        const ULONGLONG now = GetTickCount64();
        if (enabled == g_nativeHighlightActive && now - g_lastNativeHighlightTick < 250)
            return;
        g_lastNativeHighlightTick = now;

        std::size_t touched = 0;
        AcquireSRWLockShared(&g_puppetListLock);
        for (const TrackedPuppet& tracked : g_puppetList)
        {
            if (!tracked.entity)
                continue;
            __try
            {
                EntityLayout* entity = tracked.entity;
                if (!entity || entity->entityId != tracked.entityId ||
                    ClassifyPuppet(entity->nativeType) != PuppetKind::Npc)
                {
                    continue;
                }

                const NpcCategory category = ClassifyNpc(entity);
                bool categoryEnabled = false;
                switch (category)
                {
                case NpcCategory::Civilian:
                    categoryEnabled = showCivilians;
                    break;
                case NpcCategory::Enemy:
                    categoryEnabled = showEnemies;
                    break;
                case NpcCategory::Police:
                    categoryEnabled = showPolice;
                    break;
                default:
                    categoryEnabled = showUnclassified;
                    break;
                }
                const bool shouldHighlight = enabled && categoryEnabled && !(hideDead && IsDead(entity));
                SetEntityNativeHighlight(entity, shouldHighlight);
                ++touched;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        ReleaseSRWLockShared(&g_puppetListLock);

        if (enabled != g_nativeHighlightActive)
        {
            Diagnostics::Log("native highlight %s: entities=%zu", enabled ? "enabled" : "disabled", touched);
            g_nativeHighlightActive = enabled;
        }
    }
}
