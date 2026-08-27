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
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace
{
    // RED4ext/CET verified address hash for world::RuntimeEntityRegistry::RegisterEntity.
    // The UnregisterEntity hash is intentionally not hooked: its native ABI is not verified.
    constexpr std::uint32_t kRegisterEntityAddressHash = 2840271332u;
    constexpr std::uint32_t kCClassGetPropertyAddressHash = 0x8F031512u;
    constexpr std::uint32_t kVisionModeSetBraindanceModeAddressHash = 1070077985u;

    constexpr std::uint32_t kGameEngineAddressHash = 0x97F209D6u;
    constexpr std::uint32_t kRttiSystemGetAddressHash = 0x4A610F64u;
    constexpr std::size_t kEntRenderHighlightEventSize = 0x58;
    constexpr std::size_t kMaxLeakedHighlightEvents = 4096;

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
        Game::EntityTracker::NpcCategory category = Game::EntityTracker::NpcCategory::Other;
        bool isDead = false;
        bool healthValid = false;
        float healthCurrent = 0.0f;
        float healthMax = 0.0f;
        float healthRatio = 0.0f;
        bool healthReachedMin = false;
        bool highlightKnown = false;
        bool highlightDesired = false;
    };

    constexpr std::size_t kMaxTrackedPuppets = 256;
    // Reserve one clear-event slot per tracked entity. Enable transitions stop before this headroom is consumed,
    // so cleanup can still clear every known-enabled entry even after a long sequence of setting changes.
    constexpr std::size_t kReservedHighlightClearEvents = kMaxTrackedPuppets;
    static_assert(kReservedHighlightClearEvents < kMaxLeakedHighlightEvents);

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
    std::atomic_uint64_t g_pendingPosition{0};
    std::atomic_uint64_t g_staleRemoved{0};
    std::atomic_uint64_t g_healthValid{0};
    std::atomic_uint64_t g_healthInvalid{0};
    std::atomic_uint64_t g_nativeHighlightQueued{0};
    std::atomic_uint64_t g_nativeHighlightCleared{0};
    std::atomic_uint64_t g_nativeHighlightFailures{0};
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
    std::atomic_uint32_t g_nativeHighlightRequest{0};
    std::atomic_uint64_t g_nativeHighlightGeneration{0};
    std::atomic_bool g_nativeHighlightModeActive{false};
    std::atomic_bool g_cleanupRequested{false};
    std::atomic_bool g_cleanupClearQueued{false};
    std::atomic_bool g_cleanupAcknowledged{false};
    std::atomic_uint64_t g_cleanupGeneration{0};
    std::uint64_t g_healthRoundRobin = 0;

    struct NativeHighlightRuntime
    {
        bool attempted = false;
        ULONGLONG lastResolveAttempt = 0;
        Game::Rtti::Class* eventClass = nullptr;
        std::size_t eventSize = 0;
        void* visionModeSystem = nullptr;
        using SetBraindanceModeFn = void (*)(void*, std::uint32_t);
        SetBraindanceModeFn setBraindanceMode = nullptr;
        std::size_t leakedEvents = 0;
    };
    NativeHighlightRuntime g_highlightRuntime;

    struct HealthRuntime
    {
        bool attempted = false;
        ULONGLONG lastResolveAttempt = 0;
        void* statPoolsSystem = nullptr;
        Game::Rtti::Function* getValue = nullptr;
        Game::Rtti::Function* getMaxValue = nullptr;
        Game::Rtti::Function* reachedMin = nullptr;
    };
    HealthRuntime g_healthRuntime;

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

    void* ResolveGameInstanceOnMainTick()
    {
        HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
        const auto resolve = red4ext
                                 ? reinterpret_cast<ResolveAddressFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                                 : nullptr;
        if (!resolve)
            return nullptr;

        const std::uintptr_t enginePointerAddress = resolve(kGameEngineAddressHash);
        void* engine = enginePointerAddress ? *reinterpret_cast<void**>(enginePointerAddress) : nullptr;
        void* framework = engine ? *reinterpret_cast<void**>(static_cast<std::byte*>(engine) + 0x308) : nullptr;
        return framework ? *reinterpret_cast<void**>(static_cast<std::byte*>(framework) + 0x10) : nullptr;
    }

    void* VirtualFunction(void* object, std::size_t index)
    {
        if (!object)
            return nullptr;
        void** table = *reinterpret_cast<void***>(object);
        return table ? table[index] : nullptr;
    }

    void* GetSystemOnMainTick(void* gameInstance, std::uint64_t typeHash)
    {
        if (!gameInstance)
            return nullptr;
        void* rttiSystem = nullptr;
        HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
        const auto resolve = red4ext
                                 ? reinterpret_cast<ResolveAddressFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                                 : nullptr;
        const std::uintptr_t rttiGetAddress = resolve ? resolve(kRttiSystemGetAddressHash) : 0;
        if (rttiGetAddress)
            rttiSystem = reinterpret_cast<void* (*)()>(rttiGetAddress)();
        const auto getClass = reinterpret_cast<void* (*)(void*, std::uint64_t)>(VirtualFunction(rttiSystem, 2));
        void* type = getClass ? getClass(rttiSystem, typeHash) : nullptr;
        const auto getSystem = reinterpret_cast<void* (*)(void*, void*)>(VirtualFunction(gameInstance, 1));
        return getSystem && type ? getSystem(gameInstance, type) : nullptr;
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

    void AddPosePoint(Game::AnimationData::VisualData& visual, const float position[3])
    {
        if (visual.posePointCount >= Game::AnimationData::kMaxPosePoints)
            return;
        memcpy(visual.posePoints[visual.posePointCount++], position, sizeof(visual.posePoints[0]));
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
        visual.posePointCount = 0;
        const PosePoint* const ordered[] = {&head, &chest, &hips, &rightHand, &leftLeg, &rightLeg};
        for (const PosePoint* point : ordered)
        {
            if (point->valid)
                AddPosePoint(visual, point->position);
        }
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

    bool IsCorpseDead(const EntityLayout* entity)
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
        // A streamed-out object can be replaced at the same slot/ID. Do not carry health/highlight state from the
        // old pointer into the replacement; it must earn a fresh snapshot and highlight transition.
        if (target->entity != entity || target->entityId != entity->entityId)
            *target = {};
        target->entity = entity;
        target->entityId = entity->entityId;
        target->sequence = ++g_puppetSequence;
        target->category = ClassifyNpc(entity);
        target->isDead = false;

        std::uint64_t count = 0;
        for (const TrackedPuppet& tracked : g_puppetList)
            count += tracked.entity != nullptr ? 1u : 0u;
        g_trackedPuppets.store(count, std::memory_order_release);
        ReleaseSRWLockExclusive(&g_puppetListLock);
    }

    enum class SnapshotResult : std::uint8_t
    {
        Ready,
        PendingPosition,
        Stale,
    };

    SnapshotResult TrySnapshot(TrackedPuppet& tracked, Game::EntityTracker::PuppetSnapshot& snapshot)
    {
        // Game streaming can free/reuse an entity independently of our list. Validate all identity data at the
        // point of use and contain a stale-pointer access; no raw pointer leaves this function.
        __try
        {
            EntityLayout* entity = tracked.entity;
            if (!entity)
                return SnapshotResult::Stale;
            if (entity->entityId != tracked.entityId || ClassifyPuppet(entity->nativeType) != PuppetKind::Npc)
            {
                return SnapshotResult::Stale;
            }

            float position[3]{};
            float orientation[4]{};
            if (!ReadTransform(entity, position, orientation))
            {
                g_pendingPosition.fetch_add(1, std::memory_order_relaxed);
                return SnapshotResult::PendingPosition;
            }

            snapshot.entityId = tracked.entityId;
            snapshot.position[0] = position[0];
            snapshot.position[1] = position[1];
            snapshot.position[2] = position[2];
            memcpy(snapshot.orientation, orientation, sizeof(orientation));
            snapshot.category = ClassifyNpc(entity);
            tracked.category = snapshot.category;
            if (tracked.healthValid)
                tracked.isDead = tracked.healthReachedMin || tracked.healthCurrent <= 0.001f;
            else
                tracked.isDead = IsCorpseDead(entity);
            snapshot.isDead = tracked.isDead;
            snapshot.healthValid = tracked.healthValid;
            snapshot.healthCurrent = tracked.healthCurrent;
            snapshot.healthMax = tracked.healthMax;
            snapshot.healthRatio = tracked.healthRatio;
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
            return SnapshotResult::Ready;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return SnapshotResult::Stale;
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
        else if (puppet)
        {
            g_pendingPosition.fetch_add(1, std::memory_order_relaxed);
        }

        if (puppet)
            TrackPuppet(entity);

        AcquireSRWLockExclusive(&g_lastEntityLock);
        g_lastEntityId = entity->entityId;
        g_lastPosition[0] = position[0];
        g_lastPosition[1] = position[1];
        g_lastPosition[2] = position[2];
        if (puppet)
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

    constexpr std::uint32_t kHighlightEnabledBit = 1u << 0;
    constexpr std::uint32_t kHighlightCivilianBit = 1u << 1;
    constexpr std::uint32_t kHighlightEnemyBit = 1u << 2;
    constexpr std::uint32_t kHighlightPoliceBit = 1u << 3;
    constexpr std::uint32_t kHighlightOtherBit = 1u << 4;
    constexpr std::uint32_t kHighlightHideDeadBit = 1u << 5;

    struct HighlightWork
    {
        EntityLayout* entity = nullptr;
        std::uint64_t entityId = 0;
        bool desired = false;
    };

    bool IsCategoryEnabled(Game::EntityTracker::NpcCategory category, std::uint32_t settings)
    {
        switch (category)
        {
        case Game::EntityTracker::NpcCategory::Civilian:
            return (settings & kHighlightCivilianBit) != 0;
        case Game::EntityTracker::NpcCategory::Enemy:
            return (settings & kHighlightEnemyBit) != 0;
        case Game::EntityTracker::NpcCategory::Police:
            return (settings & kHighlightPoliceBit) != 0;
        default:
            return (settings & kHighlightOtherBit) != 0;
        }
    }

    bool ResolveNativeHighlightOnMainTick()
    {
        const ULONGLONG now = GetTickCount64();
        if (g_highlightRuntime.attempted && g_highlightRuntime.eventClass != nullptr &&
            g_highlightRuntime.eventSize == kEntRenderHighlightEventSize &&
            g_highlightRuntime.visionModeSystem != nullptr && g_highlightRuntime.setBraindanceMode != nullptr)
            return true;
        if (g_highlightRuntime.attempted && now - g_highlightRuntime.lastResolveAttempt < 1000)
        {
            return false;
        }

        g_highlightRuntime.attempted = true;
        g_highlightRuntime.lastResolveAttempt = now;
        __try
        {
            g_highlightRuntime.eventClass = Game::Rtti::GetClass(Game::Rtti::Hash("entRenderHighlightEvent"));
            g_highlightRuntime.eventSize = Game::Rtti::ClassSize(g_highlightRuntime.eventClass);
            void* gameInstance = ResolveGameInstanceOnMainTick();
            g_highlightRuntime.visionModeSystem = GetSystemOnMainTick(
                gameInstance, Game::Rtti::Hash("gameIVisionModeSystem"));

            HMODULE red4ext = GetModuleHandleW(L"RED4ext.dll");
            const auto resolve = red4ext
                                     ? reinterpret_cast<ResolveAddressFn>(GetProcAddress(red4ext, "RED4ext_ResolveAddress"))
                                     : nullptr;
            const std::uintptr_t modeAddress = resolve ? resolve(kVisionModeSetBraindanceModeAddressHash) : 0;
            g_highlightRuntime.setBraindanceMode = modeAddress
                                                       ? reinterpret_cast<NativeHighlightRuntime::SetBraindanceModeFn>(
                                                             modeAddress)
                                                       : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_highlightRuntime.eventClass = nullptr;
            g_highlightRuntime.eventSize = 0;
            g_highlightRuntime.visionModeSystem = nullptr;
            g_highlightRuntime.setBraindanceMode = nullptr;
        }

        const bool resolved = g_highlightRuntime.eventClass != nullptr &&
                              g_highlightRuntime.eventSize == kEntRenderHighlightEventSize &&
                              g_highlightRuntime.visionModeSystem != nullptr &&
                              g_highlightRuntime.setBraindanceMode != nullptr;
        Diagnostics::Log("native highlight resolver: eventClass=%p eventSize=0x%zX visionModeSystem=%p "
                         "setBraindanceMode=%p resolved=%d",
                         g_highlightRuntime.eventClass, g_highlightRuntime.eventSize,
                         g_highlightRuntime.visionModeSystem,
                         reinterpret_cast<void*>(g_highlightRuntime.setBraindanceMode), resolved ? 1 : 0);
        if (!resolved)
            g_nativeHighlightFailures.fetch_add(1, std::memory_order_relaxed);
        return resolved;
    }

    bool QueueHighlightEvent(const HighlightWork& work)
    {
        if (!work.entity || work.entityId == 0)
            return false;
        const std::size_t eventLimit = work.desired
                                           ? kMaxLeakedHighlightEvents - kReservedHighlightClearEvents
                                           : kMaxLeakedHighlightEvents;
        if (g_highlightRuntime.leakedEvents >= eventLimit)
            return false;

        bool queued = false;
        __try
        {
            if (work.entity->entityId != work.entityId || ClassifyPuppet(work.entity->nativeType) != PuppetKind::Npc)
                return false;

            void* event = Game::Rtti::CreateInstance(g_highlightRuntime.eventClass);
            if (!event)
                return false;
            ++g_highlightRuntime.leakedEvents;

            // entRenderHighlightEvent is 0x58 bytes on Cyberpunk 2.31. CClass::CreateInstance has already installed
            // the native object header; only write the documented event fields. QueueEvent is void, so a successful
            // reflected call does not prove that it copied the handle before returning. We therefore retain this
            // local strong reference rather than risking a premature final Handle destructor while the event loop
            // may still own the object. The bounded allocation cap favors a safe leak over a UAF.
            auto* bytes = static_cast<std::byte*>(event);
            *reinterpret_cast<std::uint8_t*>(bytes + 0x40) = work.desired ? 0u : 0u; // fillIndex
            *reinterpret_cast<std::uint8_t*>(bytes + 0x41) = work.desired ? 1u : 0u; // outlineIndex
            *reinterpret_cast<std::uint8_t*>(bytes + 0x42) = work.desired ? 1u : 0u; // seeThroughWalls
            *reinterpret_cast<std::uint64_t*>(bytes + 0x48) = 0u;                     // componentName (all components)
            *reinterpret_cast<float*>(bytes + 0x50) = work.desired ? 1.0f : 0.0f;     // opacity
            *reinterpret_cast<std::uint8_t*>(bytes + 0x54) = 1u;                      // forced
            *reinterpret_cast<std::uint8_t*>(bytes + 0x55) = 0u;                      // pattern

            Game::Rtti::Handle handle;
            if (!Game::Rtti::ConstructHandle(&handle, event))
                return false;

            Game::Rtti::Function* queueEvent = Game::Rtti::FindFunction(
                Game::Rtti::NativeType(work.entity), Game::Rtti::Hash("QueueEvent"));
            if (!queueEvent || Game::Rtti::ParameterCount(queueEvent) != 1)
                return false;
            Game::Rtti::Argument argument{&handle};
            // QueueEvent is a reflected Void method. Invoke() returning true means the call reached the VM; it does
            // not demonstrate ownership transfer, hence the conservative local-handle lifetime above.
            queued = Game::Rtti::Invoke(queueEvent, work.entity, &argument, 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            queued = false;
        }

        if (queued)
        {
            g_nativeHighlightQueued.fetch_add(1, std::memory_order_relaxed);
            if (!work.desired)
                g_nativeHighlightCleared.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            g_nativeHighlightFailures.fetch_add(1, std::memory_order_relaxed);
        }
        return queued;
    }

    bool SetBraindanceModeOnMainTick(bool enabled)
    {
        const bool active = g_nativeHighlightModeActive.load(std::memory_order_acquire);
        if (active == enabled)
            return true;
        if (!g_highlightRuntime.setBraindanceMode || !g_highlightRuntime.visionModeSystem)
            return false;
        __try
        {
            g_highlightRuntime.setBraindanceMode(g_highlightRuntime.visionModeSystem, enabled ? 1u : 0u);
            g_nativeHighlightModeActive.store(enabled, std::memory_order_release);
            Diagnostics::Log("native highlight braindance mode: %s", enabled ? "1" : "0");
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_nativeHighlightFailures.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    void PublishHighlightResult(const HighlightWork& work, bool queued)
    {
        if (!queued)
            return;
        AcquireSRWLockExclusive(&g_puppetListLock);
        for (TrackedPuppet& tracked : g_puppetList)
        {
            if (tracked.entity == work.entity && tracked.entityId == work.entityId)
            {
                tracked.highlightKnown = true;
                tracked.highlightDesired = work.desired;
                break;
            }
        }
        ReleaseSRWLockExclusive(&g_puppetListLock);
    }

    bool HasDesiredHighlightState()
    {
        bool active = false;
        AcquireSRWLockShared(&g_puppetListLock);
        for (const TrackedPuppet& tracked : g_puppetList)
        {
            if (tracked.entity && tracked.highlightKnown && tracked.highlightDesired)
            {
                active = true;
                break;
            }
        }
        ReleaseSRWLockShared(&g_puppetListLock);
        return active;
    }

    void ProcessNativeHighlightsOnMainTick()
    {
        const std::uint32_t published = g_nativeHighlightRequest.load(std::memory_order_acquire);
        const std::uint32_t settings = g_cleanupRequested.load(std::memory_order_acquire) ? 0u : published;
        const bool enabled = (settings & kHighlightEnabledBit) != 0;
        std::array<HighlightWork, kMaxTrackedPuppets> workItems{};
        std::size_t workCount = 0;
        bool anyDesired = false;

        AcquireSRWLockShared(&g_puppetListLock);
        for (const TrackedPuppet& tracked : g_puppetList)
        {
            if (!tracked.entity)
                continue;
            const bool desired = enabled && IsCategoryEnabled(tracked.category, settings) &&
                                 !((settings & kHighlightHideDeadBit) != 0 && tracked.isDead);
            anyDesired = anyDesired || desired;
            // Queue only a state transition: unknown+desired=true is the first enable, while known entries queue
            // only when the cached state differs. Unknown+desired=false has nothing to clear. The cache is updated
            // only after QueueEvent succeeds below.
            const bool transitionNeeded = tracked.highlightKnown ? tracked.highlightDesired != desired : desired;
            if (transitionNeeded && workCount < workItems.size())
                workItems[workCount++] = {tracked.entity, tracked.entityId, desired};
        }
        ReleaseSRWLockShared(&g_puppetListLock);

        // Keep mode enabled while a clear transition is pending. This prevents a failed clear from being hidden by
        // an eager mode=0 call and lets the next main tick retry the same transition.
        const bool cachedDesired = HasDesiredHighlightState();
        const bool modeDesired = anyDesired || cachedDesired;
        if (workCount > 0 || g_nativeHighlightModeActive.load(std::memory_order_acquire) != modeDesired)
        {
            if (ResolveNativeHighlightOnMainTick())
            {
                // RedHotTools enables braindance mode before placing the first render event. Clear events are sent
                // before returning to mode 0 so the engine sees a consistent transition.
                bool modeReady = true;
                if (modeDesired && !g_nativeHighlightModeActive.load(std::memory_order_acquire))
                    modeReady = SetBraindanceModeOnMainTick(true);

                // If the mode transition failed, do not enqueue events into a half-initialized render path. The
                // transition remains pending and will be retried on a later main tick.
                if (modeReady)
                {
                    for (std::size_t i = 0; i < workCount; ++i)
                        PublishHighlightResult(workItems[i], QueueHighlightEvent(workItems[i]));

                    // A clear is complete only after every known-enabled entry has published a successful clear.
                    // If any QueueEvent failed (including a cap guard), retain mode=1 and retry on a later tick.
                    if (!anyDesired && !HasDesiredHighlightState())
                        SetBraindanceModeOnMainTick(false);
                }
            }
        }

        if (g_cleanupRequested.load(std::memory_order_acquire))
        {
            const bool clearComplete = !HasDesiredHighlightState() &&
                                       !g_nativeHighlightModeActive.load(std::memory_order_acquire);
            if (clearComplete)
            {
                if (g_cleanupClearQueued.exchange(true, std::memory_order_acq_rel))
                    g_cleanupAcknowledged.store(true, std::memory_order_release);
            }
            else
            {
                g_cleanupClearQueued.store(false, std::memory_order_release);
            }
        }
    }

    bool ResolveHealthOnMainTick()
    {
        const ULONGLONG now = GetTickCount64();
        if (g_healthRuntime.attempted && g_healthRuntime.statPoolsSystem && g_healthRuntime.getValue &&
            g_healthRuntime.getMaxValue && g_healthRuntime.reachedMin)
            return true;
        if (g_healthRuntime.attempted && now - g_healthRuntime.lastResolveAttempt < 1000)
            return false;
        g_healthRuntime.attempted = true;
        g_healthRuntime.lastResolveAttempt = now;

        __try
        {
            void* gameInstance = ResolveGameInstanceOnMainTick();
            g_healthRuntime.statPoolsSystem = GetSystemOnMainTick(
                gameInstance, Game::Rtti::Hash("gameStatPoolsSystem"));
            if (!g_healthRuntime.statPoolsSystem)
                g_healthRuntime.statPoolsSystem = GetSystemOnMainTick(
                    gameInstance, Game::Rtti::Hash("gameIStatPoolsSystem"));
            const Game::Rtti::Class* type = Game::Rtti::NativeType(g_healthRuntime.statPoolsSystem);
            g_healthRuntime.getValue = Game::Rtti::FindFunction(type, Game::Rtti::Hash("GetStatPoolValue"));
            g_healthRuntime.getMaxValue = Game::Rtti::FindFunction(
                type, Game::Rtti::Hash("GetStatPoolMaxPointValue"));
            g_healthRuntime.reachedMin = Game::Rtti::FindFunction(
                type, Game::Rtti::Hash("HasStatPoolValueReachedMin"));
            if (Game::Rtti::ParameterCount(g_healthRuntime.getValue) != 3 ||
                Game::Rtti::ParameterCount(g_healthRuntime.getMaxValue) != 2 ||
                Game::Rtti::ParameterCount(g_healthRuntime.reachedMin) != 2)
            {
                g_healthRuntime.statPoolsSystem = nullptr;
                g_healthRuntime.getValue = nullptr;
                g_healthRuntime.getMaxValue = nullptr;
                g_healthRuntime.reachedMin = nullptr;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_healthRuntime.statPoolsSystem = nullptr;
            g_healthRuntime.getValue = nullptr;
            g_healthRuntime.getMaxValue = nullptr;
            g_healthRuntime.reachedMin = nullptr;
        }

        const bool resolved = g_healthRuntime.statPoolsSystem && g_healthRuntime.getValue &&
                              g_healthRuntime.getMaxValue && g_healthRuntime.reachedMin;
        Diagnostics::Log("health stat-pool resolver: system=%p value=%p max=%p reachedMin=%p resolved=%d",
                         g_healthRuntime.statPoolsSystem, g_healthRuntime.getValue, g_healthRuntime.getMaxValue,
                         g_healthRuntime.reachedMin, resolved ? 1 : 0);
        return resolved;
    }

    struct HealthWork
    {
        EntityLayout* entity = nullptr;
        std::uint64_t entityId = 0;
    };

    void PublishHealth(const HealthWork& work, bool valid, float current, float maximum, float ratio,
                       bool reachedMin)
    {
        AcquireSRWLockExclusive(&g_puppetListLock);
        for (TrackedPuppet& tracked : g_puppetList)
        {
            if (tracked.entity != work.entity || tracked.entityId != work.entityId)
                continue;
            tracked.healthValid = valid;
            tracked.healthCurrent = valid ? current : 0.0f;
            tracked.healthMax = valid ? maximum : 0.0f;
            tracked.healthRatio = valid ? ratio : 0.0f;
            tracked.healthReachedMin = valid && reachedMin;
            if (valid)
                tracked.isDead = reachedMin || current <= 0.001f;
            break;
        }
        ReleaseSRWLockExclusive(&g_puppetListLock);
        if (valid)
            g_healthValid.fetch_add(1, std::memory_order_relaxed);
        else
            g_healthInvalid.fetch_add(1, std::memory_order_relaxed);
    }

    void ProcessHealthOnMainTick()
    {
        constexpr std::size_t kHealthPerTick = 8;
        std::array<HealthWork, kHealthPerTick> workItems{};
        std::size_t workCount = 0;
        const std::size_t start = static_cast<std::size_t>(g_healthRoundRobin % kMaxTrackedPuppets);
        std::size_t scanned = 0;
        AcquireSRWLockShared(&g_puppetListLock);
        for (; scanned < kMaxTrackedPuppets && workCount < workItems.size(); ++scanned)
        {
            const TrackedPuppet& tracked = g_puppetList[(start + scanned) % kMaxTrackedPuppets];
            if (tracked.entity && tracked.entityId != 0)
                workItems[workCount++] = {tracked.entity, tracked.entityId};
        }
        ReleaseSRWLockShared(&g_puppetListLock);
        g_healthRoundRobin = (start + (scanned == 0 ? 1 : scanned)) % kMaxTrackedPuppets;
        if (workCount == 0)
            return;

        const bool resolved = ResolveHealthOnMainTick();
        for (std::size_t i = 0; i < workCount; ++i)
        {
            bool valid = false;
            float current = 0.0f;
            float maximum = 0.0f;
            float ratio = 0.0f;
            bool reachedMin = false;
            if (resolved)
            {
                __try
                {
                    std::int32_t pool = 17; // gamedataStatPoolType::Health
                    bool asPercentage = false;
                    Game::Rtti::Argument valueArguments[] = {{&workItems[i].entityId}, {&pool}, {&asPercentage}};
                    Game::Rtti::Argument maxArguments[] = {{&workItems[i].entityId}, {&pool}};
                    valid = Game::Rtti::Invoke(g_healthRuntime.getValue, g_healthRuntime.statPoolsSystem,
                                               valueArguments, 3, &current) &&
                            Game::Rtti::Invoke(g_healthRuntime.getMaxValue, g_healthRuntime.statPoolsSystem,
                                               maxArguments, 2, &maximum);
                    bool reachedResult = false;
                    if (valid)
                    {
                        valid = Game::Rtti::Invoke(g_healthRuntime.reachedMin, g_healthRuntime.statPoolsSystem,
                                                   maxArguments, 2, &reachedResult);
                        reachedMin = reachedResult;
                    }
                    valid = valid && std::isfinite(current) && std::isfinite(maximum) && maximum > 0.001f;
                    if (valid)
                    {
                        ratio = std::clamp(current / maximum, 0.0f, 1.0f);
                        valid = std::isfinite(ratio);
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    valid = false;
                }
            }
            PublishHealth(workItems[i], valid, current, maximum, ratio, reachedMin);
        }
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

        // The UnregisterEntity address hash is known, but its native ABI is not verified. Do not install a hook
        // based on an inferred signature. Snapshot identity/transform validation remains the authoritative removal
        // path; a temporarily unavailable transform stays pending and a stale/mismatched object is removed.
        Diagnostics::Log("UnregisterEntity hook disabled: ABI is unverified; stale snapshot validation remains active");

        g_hookCreated.store(true, std::memory_order_release);
        Diagnostics::Log("entity tracker hook created: target=%p", target);
        return true;
    }

    void Shutdown()
    {
        g_hookCreated.store(false, std::memory_order_release);
        g_registry.store(nullptr, std::memory_order_release);
        g_originalRegisterEntity = nullptr;
        g_nativeHighlightModeActive.store(false, std::memory_order_release);
        g_cleanupRequested.store(false, std::memory_order_release);
        g_cleanupClearQueued.store(false, std::memory_order_release);
        g_cleanupAcknowledged.store(false, std::memory_order_release);
        g_cleanupGeneration.store(0, std::memory_order_release);
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
        result.pendingPosition = g_pendingPosition.load(std::memory_order_relaxed);
        result.staleRemoved = g_staleRemoved.load(std::memory_order_relaxed);
        result.healthValid = g_healthValid.load(std::memory_order_relaxed);
        result.healthInvalid = g_healthInvalid.load(std::memory_order_relaxed);
        result.nativeHighlightQueued = g_nativeHighlightQueued.load(std::memory_order_relaxed);
        result.nativeHighlightCleared = g_nativeHighlightCleared.load(std::memory_order_relaxed);
        result.nativeHighlightFailures = g_nativeHighlightFailures.load(std::memory_order_relaxed);

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
            const SnapshotResult result = TrySnapshot(tracked, snapshot);
            if (result == SnapshotResult::Stale)
            {
                tracked = {};
                g_staleRemoved.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            ++trackedCount;
            if (result == SnapshotResult::PendingPosition)
                continue;
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

    void OnGameMainTick()
    {
        // Both operations below are intentionally called only from visibility's single game-main-tick detour.
        // Present publishes requests but never enters these RTTI/engine paths.
        ProcessHealthOnMainTick();
        ProcessNativeHighlightsOnMainTick();
    }

    bool PrepareForShutdown(std::uint32_t timeoutMilliseconds)
    {
        g_nativeHighlightRequest.store(0, std::memory_order_release);
        // A Present-side request may have been published before the first main tick, but that is not engine state yet.
        // Only wait when a mode transition or an actual per-entity enable event has been acknowledged.
        const bool wasActive = g_nativeHighlightModeActive.load(std::memory_order_acquire) ||
                               HasDesiredHighlightState();
        if (!wasActive)
            return true;

        g_cleanupRequested.store(true, std::memory_order_release);
        g_cleanupClearQueued.store(false, std::memory_order_release);
        g_cleanupAcknowledged.store(false, std::memory_order_release);
        const std::uint64_t generation = g_nativeHighlightGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        g_cleanupGeneration.store(generation, std::memory_order_release);

        const ULONGLONG deadline = GetTickCount64() + timeoutMilliseconds;
        while (!g_cleanupAcknowledged.load(std::memory_order_acquire))
        {
            if (GetTickCount64() >= deadline)
            {
                Diagnostics::Log("native highlight cleanup timed out: generation=%llu mode=%d",
                                 static_cast<unsigned long long>(generation),
                                 g_nativeHighlightModeActive.load(std::memory_order_acquire) ? 1 : 0);
                return false;
            }
            Sleep(1);
        }
        Diagnostics::Log("native highlight cleanup acknowledged: generation=%llu queued=%llu cleared=%llu",
                         static_cast<unsigned long long>(generation),
                         static_cast<unsigned long long>(g_nativeHighlightQueued.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_nativeHighlightCleared.load(std::memory_order_relaxed)));
        return true;
    }

    void UpdateNativeHighlights(bool enabled, bool showCivilians, bool showEnemies, bool showPolice,
                                bool showUnclassified, bool hideDead)
    {
        std::uint32_t settings = 0;
        settings |= enabled ? kHighlightEnabledBit : 0u;
        settings |= showCivilians ? kHighlightCivilianBit : 0u;
        settings |= showEnemies ? kHighlightEnemyBit : 0u;
        settings |= showPolice ? kHighlightPoliceBit : 0u;
        settings |= showUnclassified ? kHighlightOtherBit : 0u;
        settings |= hideDead ? kHighlightHideDeadBit : 0u;

        std::uint32_t previous = g_nativeHighlightRequest.load(std::memory_order_acquire);
        while (previous != settings &&
               !g_nativeHighlightRequest.compare_exchange_weak(previous, settings, std::memory_order_acq_rel,
                                                               std::memory_order_acquire))
        {
        }
        if (previous != settings)
        {
            g_nativeHighlightGeneration.fetch_add(1, std::memory_order_acq_rel);
            Diagnostics::Log("native highlight desired settings published: enabled=%d civilians=%d enemies=%d "
                             "police=%d other=%d hideDead=%d",
                             enabled ? 1 : 0, showCivilians ? 1 : 0, showEnemies ? 1 : 0,
                             showPolice ? 1 : 0, showUnclassified ? 1 : 0, hideDead ? 1 : 0);
        }
    }
}
