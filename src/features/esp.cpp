#include "esp.h"
#include "features.h"
#include "../diagnostics.h"
#include "../game/entity_tracker.h"
#include "../game/projection.h"
#include "../game/visibility.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

namespace Esp
{
    namespace
    {
        struct CategoryStyle
        {
            const char* label;
            ImU32 color;
            bool enabled;
        };

        CategoryStyle GetCategoryStyle(Game::EntityTracker::NpcCategory category,
                                       Game::EntityTracker::Hostility hostility,
                                       const Features::EspSettings& settings)
        {
            using Game::EntityTracker::Hostility;
            using Game::EntityTracker::NpcCategory;
            // The archetype is fixed at spawn, so an NPC that turns on the player is only red because of this.
            // Police stay under their own toggle even while hostile.
            if (hostility == Hostility::Hostile && category != NpcCategory::Police)
                return {"HOSTILE", IM_COL32(255, 82, 96, 245), settings.showEnemies};
            switch (category)
            {
            case NpcCategory::Civilian:
                return {"CIVILIAN", IM_COL32(74, 222, 128, 245), settings.showCivilians};
            case NpcCategory::Enemy:
                return {"ENEMY", IM_COL32(255, 82, 96, 245), settings.showEnemies};
            case NpcCategory::Police:
                return {"POLICE", IM_COL32(72, 153, 255, 245), settings.showPolice};
            default:
                return {"UNCLASSIFIED", IM_COL32(180, 188, 201, 225), settings.showUnclassified};
            }
        }

        // 실제 사람 실루엣에 맞춘 값. 애니메이션 시스템 AABB는 무기/모션까지 감싸느라 눈에 띄게 크다.
        constexpr float kHorizontalMarginMeters = 0.16f;
        constexpr float kMinimumRadiusMeters = 0.26f;
        constexpr float kMaximumRadiusMeters = 1.10f;
        constexpr float kHeadClearanceMeters = 0.14f;
        constexpr float kFootClearanceMeters = 0.04f;
        constexpr float kFallbackHeightMeters = 1.80f;

        // 월드 공간에서의 대상 실린더. 화면 사각형은 여기서 만든다.
        struct WorldExtent
        {
            float centerX = 0.0f;
            float centerY = 0.0f;
            float bottomZ = 0.0f;
            float topZ = 0.0f;
            float radius = kMinimumRadiusMeters;
            bool fromPose = false;
        };

        WorldExtent BuildWorldExtent(const Game::EntityTracker::PuppetSnapshot& puppet)
        {
            const Game::AnimationData::VisualData& visual = puppet.visual;
            WorldExtent extent;
            extent.centerX = puppet.position[0];
            extent.centerY = puppet.position[1];
            extent.bottomZ = puppet.position[2];
            extent.topZ = puppet.position[2] + kFallbackHeightMeters;

            if (visual.posePointCount > 0)
            {
                extent.fromPose = true;
                float radius = 0.0f;
                float lowest = puppet.position[2];
                float highest = puppet.position[2];
                for (std::size_t i = 0; i < visual.posePointCount; ++i)
                {
                    const float* point = visual.posePoints[i];
                    const float offsetX = point[0] - extent.centerX;
                    const float offsetY = point[1] - extent.centerY;
                    radius = (std::max)(radius, std::sqrt(offsetX * offsetX + offsetY * offsetY));
                    lowest = (std::min)(lowest, point[2]);
                    highest = (std::max)(highest, point[2]);
                }
                extent.radius = std::clamp(radius + kHorizontalMarginMeters, kMinimumRadiusMeters,
                                           kMaximumRadiusMeters);
                extent.bottomZ = lowest - kFootClearanceMeters;
                extent.topZ = (visual.hasHeadPosition ? visual.headPosition[2] : highest) + kHeadClearanceMeters;
            }
            else if (visual.hasBounds)
            {
                // 포즈 슬롯이 없을 때만 애니메이션 AABB를 쓰되, 수평 반경은 사람 크기로 제한한다.
                const float halfX = (visual.boundsMaximum[0] - visual.boundsMinimum[0]) * 0.5f;
                const float halfY = (visual.boundsMaximum[1] - visual.boundsMinimum[1]) * 0.5f;
                extent.centerX = (visual.boundsMinimum[0] + visual.boundsMaximum[0]) * 0.5f;
                extent.centerY = (visual.boundsMinimum[1] + visual.boundsMaximum[1]) * 0.5f;
                extent.radius = std::clamp((std::min)(halfX, halfY), kMinimumRadiusMeters, kMaximumRadiusMeters);
                extent.bottomZ = visual.boundsMinimum[2];
                extent.topZ = visual.boundsMaximum[2];
            }

            if (!(extent.topZ > extent.bottomZ))
                extent.topZ = extent.bottomZ + kFallbackHeightMeters;
            return extent;
        }

        // 카메라를 향한 사각형을 만든다. 월드 AABB의 8꼭짓점을 투영하면 대상이 월드 축과 어긋나 있을 때
        // 항상 실루엣보다 넓어지므로, 카메라-대상 방향에 수직인 축으로만 폭을 잡는다.
        bool ProjectFacingBox(const WorldExtent& extent, const float camera[3], bool hasCamera,
                              const ImGuiIO& io, ImVec2& minimum, ImVec2& maximum, float& depth, bool& behind)
        {
            float rightX = 1.0f;
            float rightY = 0.0f;
            if (hasCamera)
            {
                const float toTargetX = extent.centerX - camera[0];
                const float toTargetY = extent.centerY - camera[1];
                const float length = std::sqrt(toTargetX * toTargetX + toTargetY * toTargetY);
                if (length > 0.05f)
                {
                    rightX = -toTargetY / length;
                    rightY = toTargetX / length;
                }
            }

            const float middleZ = (extent.bottomZ + extent.topZ) * 0.5f;
            const float samples[4][3] = {
                {extent.centerX, extent.centerY, extent.bottomZ},
                {extent.centerX, extent.centerY, extent.topZ},
                {extent.centerX + rightX * extent.radius, extent.centerY + rightY * extent.radius, middleZ},
                {extent.centerX - rightX * extent.radius, extent.centerY - rightY * extent.radius, middleZ},
            };

            minimum = ImVec2((std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)());
            maximum = ImVec2(-(std::numeric_limits<float>::max)(), -(std::numeric_limits<float>::max)());
            depth = 0.0f;
            behind = false;
            for (const auto& sample : samples)
            {
                Game::Projection::ScreenPoint point;
                if (!Game::Projection::WorldToScreen(sample, io.DisplaySize.x, io.DisplaySize.y, point))
                    return false;
                if (point.behind)
                {
                    behind = true;
                    depth = point.depth;
                    return true;
                }
                minimum.x = (std::min)(minimum.x, point.x);
                minimum.y = (std::min)(minimum.y, point.y);
                maximum.x = (std::max)(maximum.x, point.x);
                maximum.y = (std::max)(maximum.y, point.y);
                depth += point.depth * 0.25f;
            }

            const float height = maximum.y - minimum.y;
            return height >= 2.0f && height <= io.DisplaySize.y * 4.0f && maximum.x > minimum.x;
        }

        ImU32 Fade(ImU32 color, float factor)
        {
            const float alpha = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * factor;
            return (color & ~IM_COL32_A_MASK) |
                   (static_cast<ImU32>(std::clamp(alpha, 0.0f, 255.0f)) << IM_COL32_A_SHIFT);
        }

        std::size_t DrawSkeleton(const Game::AnimationData::VisualData& visual, const ImGuiIO& io,
                                 ImDrawList* drawList, ImU32 color, ImU32 outline)
        {
            std::size_t drawn = 0;
            for (std::size_t i = 0; i < visual.skeletonSegmentCount; ++i)
            {
                Game::Projection::ScreenPoint start;
                Game::Projection::ScreenPoint end;
                const auto& segment = visual.skeletonSegments[i];
                if (!Game::Projection::WorldToScreen(segment.start, io.DisplaySize.x, io.DisplaySize.y, start) ||
                    !Game::Projection::WorldToScreen(segment.end, io.DisplaySize.x, io.DisplaySize.y, end) ||
                    start.behind || end.behind)
                {
                    continue;
                }
                drawList->AddLine(ImVec2(start.x, start.y), ImVec2(end.x, end.y), outline, 3.0f);
                drawList->AddLine(ImVec2(start.x, start.y), ImVec2(end.x, end.y), color, 1.35f);
                ++drawn;
            }
            return drawn;
        }
    }

    void DrawOverlay(const Features::EspSettings& settings)
    {
        Game::EntityTracker::UpdateNativeHighlights(
            settings.enabled && settings.nativeHighlight, settings.showCivilians, settings.showEnemies,
            settings.showPolice, settings.showUnclassified, settings.hideDead);

        static std::array<Game::EntityTracker::PuppetSnapshot, 128> puppets{};
        const Game::EntityTracker::Stats entityStats = Game::EntityTracker::GetStats();
        const std::size_t count = Game::EntityTracker::GetPuppetSnapshots(puppets.data(), puppets.size());
        static ULONGLONG lastDiagnosticsTick = 0;
        static unsigned diagnosticsWindows = 0;
        if (count == 0)
        {
            const ULONGLONG now = GetTickCount64();
            if ((settings.enabled || diagnosticsWindows < 3) && now - lastDiagnosticsTick >= 3000)
            {
                Diagnostics::Log("ESP diagnostics: snapshots=0 tracked=%llu pendingPosition=%llu "
                                 "unregistered=%llu staleRemoved=%llu healthValid=%llu healthInvalid=%llu",
                                 static_cast<unsigned long long>(entityStats.trackedPuppets),
                                 static_cast<unsigned long long>(entityStats.pendingPosition),
                                 static_cast<unsigned long long>(entityStats.unregistered),
                                 static_cast<unsigned long long>(entityStats.staleRemoved),
                                 static_cast<unsigned long long>(entityStats.healthValid),
                                 static_cast<unsigned long long>(entityStats.healthInvalid));
                lastDiagnosticsTick = now;
                ++diagnosticsWindows;
            }
            return;
        }

        if (!settings.enabled && diagnosticsWindows >= 3)
            return;

        const ImGuiIO& io = ImGui::GetIO();
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        constexpr ImU32 outline = IM_COL32(0, 0, 0, 220);
        float camera[3]{};
        const bool hasCamera = Game::Projection::GetCameraPosition(camera);
        const bool visibilityCheck = settings.visibilityCheck && hasCamera;
        std::size_t occludedCount = 0;
        std::size_t unknownVisibilityCount = 0;
        std::size_t categorizedCount = 0;
        std::size_t civilianCount = 0;
        std::size_t enemyCount = 0;
        std::size_t policeCount = 0;
        std::size_t unclassifiedCount = 0;
        std::size_t hostileCount = 0;
        std::size_t attitudeUnknownCount = 0;
        std::size_t deadCount = 0;
        std::size_t categoryEnabledCount = 0;
        std::size_t projectedCount = 0;
        std::size_t frontCount = 0;
        std::size_t withinDistanceCount = 0;
        std::size_t distanceRejectedCount = 0;
        std::size_t realBoundsCount = 0;
        std::size_t skeletonLineCount = 0;
        std::size_t drawnCount = 0;
        float minimumForwardDepth = (std::numeric_limits<float>::max)();
        float maximumForwardDepth = 0.0f;

        for (std::size_t i = 0; i < count; ++i)
        {
            const Game::EntityTracker::PuppetSnapshot& puppet = puppets[i];
            const CategoryStyle style = GetCategoryStyle(puppet.category, puppet.hostility, settings);
            categorizedCount += puppet.category != Game::EntityTracker::NpcCategory::Other ? 1u : 0u;
            civilianCount += puppet.category == Game::EntityTracker::NpcCategory::Civilian ? 1u : 0u;
            enemyCount += puppet.category == Game::EntityTracker::NpcCategory::Enemy ? 1u : 0u;
            policeCount += puppet.category == Game::EntityTracker::NpcCategory::Police ? 1u : 0u;
            unclassifiedCount += puppet.category == Game::EntityTracker::NpcCategory::Other ? 1u : 0u;
            hostileCount += puppet.hostility == Game::EntityTracker::Hostility::Hostile ? 1u : 0u;
            attitudeUnknownCount += puppet.hostility == Game::EntityTracker::Hostility::Unknown ? 1u : 0u;
            deadCount += puppet.isDead ? 1u : 0u;
            categoryEnabledCount += style.enabled ? 1u : 0u;
            if (settings.hideDead && puppet.isDead)
                continue;

            ImVec2 minimum;
            ImVec2 maximum;
            float distance = 0.0f;
            bool behind = false;
            const WorldExtent extent = BuildWorldExtent(puppet);
            if (!ProjectFacingBox(extent, camera, hasCamera, io, minimum, maximum, distance, behind))
                continue;
            ++projectedCount;
            if (behind)
                continue;
            ++frontCount;
            realBoundsCount += extent.fromPose ? 1u : 0u;
            distance = (std::max)(0.0f, distance);
            minimumForwardDepth = (std::min)(minimumForwardDepth, distance);
            maximumForwardDepth = (std::max)(maximumForwardDepth, distance);
            if (distance > settings.maxDistanceMeters)
            {
                ++distanceRejectedCount;
                continue;
            }
            ++withinDistanceCount;
            if (!settings.enabled || !style.enabled)
                continue;

            if (maximum.x < 0.0f || minimum.x > io.DisplaySize.x || maximum.y < 0.0f ||
                minimum.y > io.DisplaySize.y)
            {
                continue;
            }

            Game::Visibility::State visibility = Game::Visibility::State::Unknown;
            if (visibilityCheck)
            {
                const float* head = puppet.visual.hasHeadPosition ? puppet.visual.headPosition : nullptr;
                const float body[3] = {extent.centerX, extent.centerY,
                                       (extent.bottomZ + extent.topZ) * 0.5f};
                visibility = Game::Visibility::Query(puppet.entityId, camera, body, head);
                occludedCount += visibility == Game::Visibility::State::Occluded ? 1u : 0u;
                unknownVisibilityCount += visibility == Game::Visibility::State::Unknown ? 1u : 0u;
            }
            const bool occluded = visibility == Game::Visibility::State::Occluded;
            if (occluded && settings.hideOccluded)
                continue;
            ++drawnCount;

            // 가려진 대상은 지우지 않고 흐리게 그려서 "벽 뒤"임을 바로 구분할 수 있게 한다.
            const float fade = occluded ? 0.42f : 1.0f;
            const ImU32 color = Fade(style.color, fade);
            const ImU32 shadow = Fade(outline, fade);

            if (settings.boundingBoxes)
            {
                drawList->AddRect(ImVec2(minimum.x - 1.0f, minimum.y - 1.0f),
                                  ImVec2(maximum.x + 1.0f, maximum.y + 1.0f), shadow, 2.0f, 3.0f,
                                  ImDrawFlags_None);
                drawList->AddRect(minimum, maximum, color, 2.0f, 1.4f, ImDrawFlags_None);
            }

            // Health is refreshed on the game main tick and copied into the snapshot. Keep the bar anchored to the
            // projected box so it remains useful with or without the optional bounding-box outline.
            if (settings.healthBars && puppet.healthValid && maximum.y > minimum.y)
            {
                const float barWidth = 5.0f;
                const float barGap = 6.0f;
                const float barLeft = minimum.x - barGap - barWidth;
                const float ratio = std::clamp(puppet.healthRatio, 0.0f, 1.0f);
                const ImU32 barBackground = Fade(IM_COL32(0, 0, 0, 220), fade);
                const ImU32 barColor = ratio > 0.60f
                                           ? Fade(IM_COL32(74, 222, 128, 245), fade)
                                           : ratio > 0.30f ? Fade(IM_COL32(255, 196, 72, 245), fade)
                                                          : Fade(IM_COL32(255, 82, 96, 245), fade);
                drawList->AddRectFilled(ImVec2(barLeft, minimum.y), ImVec2(barLeft + barWidth, maximum.y),
                                        barBackground, 1.5f);
                const float fillTop = maximum.y - (maximum.y - minimum.y) * ratio;
                drawList->AddRectFilled(ImVec2(barLeft + 1.0f, fillTop),
                                        ImVec2(barLeft + barWidth - 1.0f, maximum.y - 1.0f), barColor, 1.0f);
            }
            if (settings.skeleton && puppet.visual.skeletonSegmentCount > 0)
                skeletonLineCount += DrawSkeleton(puppet.visual, io, drawList, color, shadow);

            char label[64]{};
            snprintf(label, sizeof(label), "%s  %.0fm%s", style.label, distance,
                     puppet.isDead ? "  DEAD" : "");
            drawList->AddText(ImVec2(minimum.x, minimum.y - ImGui::GetFontSize() - 2.0f), shadow, label);
            drawList->AddText(ImVec2(minimum.x + 1.0f, minimum.y - ImGui::GetFontSize() - 3.0f), color, label);
        }

        const ULONGLONG now = GetTickCount64();
        if ((settings.enabled || diagnosticsWindows < 3) && now - lastDiagnosticsTick >= 3000)
        {
            const float minimumDepth = frontCount > 0 ? minimumForwardDepth : 0.0f;
            const Game::Visibility::Stats visibilityStats = Game::Visibility::GetStats();
            Diagnostics::Log(
                "ESP diagnostics: snapshots=%zu categories[civilian=%zu enemy=%zu police=%zu other=%zu] "
                "attitude[hostile=%zu unknown=%zu valid=%llu invalid=%llu] "
                "dead=%zu categorized=%zu categoryEnabled=%zu projected=%zu front=%zu poseBounds=%zu "
                "skeletonLines=%zu depthRange=[%.2f,%.2f] maxDistance=%.1f distanceRejected=%zu "
                "withinDistance=%zu drawn=%zu enabled=%d camera=%d "
                "health[valid=%llu invalid=%llu bars=%d] "
                "nativeHighlight[queued=%llu cleared=%llu failures=%llu] "
                "visibility[on=%d available=%d occluded=%zu unknown=%zu casts=%llu clear=%llu blocked=%llu "
                "dropped=%llu]",
                count, civilianCount, enemyCount, policeCount, unclassifiedCount, hostileCount,
                attitudeUnknownCount, static_cast<unsigned long long>(entityStats.attitudeValid),
                static_cast<unsigned long long>(entityStats.attitudeInvalid), deadCount, categorizedCount,
                categoryEnabledCount, projectedCount, frontCount, realBoundsCount, skeletonLineCount, minimumDepth,
                maximumForwardDepth, settings.maxDistanceMeters, distanceRejectedCount, withinDistanceCount,
                drawnCount, settings.enabled ? 1 : 0, hasCamera ? 1 : 0,
                static_cast<unsigned long long>(entityStats.healthValid),
                static_cast<unsigned long long>(entityStats.healthInvalid), settings.healthBars ? 1 : 0,
                static_cast<unsigned long long>(entityStats.nativeHighlightQueued),
                static_cast<unsigned long long>(entityStats.nativeHighlightCleared),
                static_cast<unsigned long long>(entityStats.nativeHighlightFailures),
                visibilityCheck ? 1 : 0,
                visibilityStats.available ? 1 : 0, occludedCount, unknownVisibilityCount,
                static_cast<unsigned long long>(visibilityStats.casts),
                static_cast<unsigned long long>(visibilityStats.visible),
                static_cast<unsigned long long>(visibilityStats.occluded),
                static_cast<unsigned long long>(visibilityStats.dropped));
            lastDiagnosticsTick = now;
            ++diagnosticsWindows;
        }
    }
}
