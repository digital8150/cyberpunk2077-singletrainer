#include "esp.h"
#include "features.h"
#include "../diagnostics.h"
#include "../game/entity_tracker.h"
#include "../game/projection.h"

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
                                       const Features::EspSettings& settings)
        {
            using Game::EntityTracker::NpcCategory;
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

        bool ProjectBounds(const Game::AnimationData::VisualData& visual, const ImGuiIO& io, ImVec2& minimum,
                           ImVec2& maximum, float& depth)
        {
            if (!visual.hasBounds)
                return false;

            minimum = ImVec2((std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)());
            maximum = ImVec2(-(std::numeric_limits<float>::max)(), -(std::numeric_limits<float>::max)());
            depth = 0.0f;
            for (unsigned corner = 0; corner < 8; ++corner)
            {
                const float world[3] = {
                    (corner & 1) ? visual.boundsMaximum[0] : visual.boundsMinimum[0],
                    (corner & 2) ? visual.boundsMaximum[1] : visual.boundsMinimum[1],
                    (corner & 4) ? visual.boundsMaximum[2] : visual.boundsMinimum[2],
                };
                Game::Projection::ScreenPoint point;
                if (!Game::Projection::WorldToScreen(world, io.DisplaySize.x, io.DisplaySize.y, point) ||
                    point.behind)
                {
                    return false;
                }
                minimum.x = (std::min)(minimum.x, point.x);
                minimum.y = (std::min)(minimum.y, point.y);
                maximum.x = (std::max)(maximum.x, point.x);
                maximum.y = (std::max)(maximum.y, point.y);
                depth += point.depth;
            }
            depth /= 8.0f;
            return maximum.x > minimum.x && maximum.y > minimum.y;
        }

        bool ProjectFallbackBox(const Game::EntityTracker::PuppetSnapshot& puppet, const ImGuiIO& io,
                                ImVec2& minimum, ImVec2& maximum, float& depth, bool& behind)
        {
            const float headWorld[3] = {puppet.position[0], puppet.position[1], puppet.position[2] + 1.8f};
            Game::Projection::ScreenPoint feet;
            Game::Projection::ScreenPoint head;
            if (!Game::Projection::WorldToScreen(puppet.position, io.DisplaySize.x, io.DisplaySize.y, feet) ||
                !Game::Projection::WorldToScreen(headWorld, io.DisplaySize.x, io.DisplaySize.y, head))
            {
                return false;
            }
            behind = feet.behind || head.behind;
            depth = feet.depth;
            if (behind)
                return true;

            const float height = std::abs(feet.y - head.y);
            if (height < 3.0f || height > io.DisplaySize.y * 2.0f)
                return false;
            const float centerX = (feet.x + head.x) * 0.5f;
            const float width = height * 0.42f;
            minimum = ImVec2(centerX - width * 0.5f, (std::min)(feet.y, head.y));
            maximum = ImVec2(centerX + width * 0.5f, (std::max)(feet.y, head.y));
            return true;
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
        const std::size_t count = Game::EntityTracker::GetPuppetSnapshots(puppets.data(), puppets.size());
        static ULONGLONG lastDiagnosticsTick = 0;
        static unsigned diagnosticsWindows = 0;
        if (count == 0)
        {
            const ULONGLONG now = GetTickCount64();
            if ((settings.enabled || diagnosticsWindows < 3) && now - lastDiagnosticsTick >= 3000)
            {
                Diagnostics::Log("ESP diagnostics: snapshots=0 (no registered NPCs are currently tracked)");
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
        std::size_t categorizedCount = 0;
        std::size_t civilianCount = 0;
        std::size_t enemyCount = 0;
        std::size_t policeCount = 0;
        std::size_t unclassifiedCount = 0;
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
            const CategoryStyle style = GetCategoryStyle(puppet.category, settings);
            categorizedCount += puppet.category != Game::EntityTracker::NpcCategory::Other ? 1u : 0u;
            civilianCount += puppet.category == Game::EntityTracker::NpcCategory::Civilian ? 1u : 0u;
            enemyCount += puppet.category == Game::EntityTracker::NpcCategory::Enemy ? 1u : 0u;
            policeCount += puppet.category == Game::EntityTracker::NpcCategory::Police ? 1u : 0u;
            unclassifiedCount += puppet.category == Game::EntityTracker::NpcCategory::Other ? 1u : 0u;
            deadCount += puppet.isDead ? 1u : 0u;
            categoryEnabledCount += style.enabled ? 1u : 0u;
            if (settings.hideDead && puppet.isDead)
                continue;

            ImVec2 minimum;
            ImVec2 maximum;
            float distance = 0.0f;
            bool behind = false;
            const bool usedRealBounds = ProjectBounds(puppet.visual, io, minimum, maximum, distance);
            if (!usedRealBounds && !ProjectFallbackBox(puppet, io, minimum, maximum, distance, behind))
                continue;
            ++projectedCount;
            if (behind)
                continue;
            ++frontCount;
            realBoundsCount += usedRealBounds ? 1u : 0u;
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
            ++drawnCount;

            if (settings.boundingBoxes)
            {
                drawList->AddRect(ImVec2(minimum.x - 1.0f, minimum.y - 1.0f),
                                  ImVec2(maximum.x + 1.0f, maximum.y + 1.0f), outline, 2.0f, 3.0f,
                                  ImDrawFlags_None);
                drawList->AddRect(minimum, maximum, style.color, 2.0f, 1.4f, ImDrawFlags_None);
            }
            if (settings.skeleton && puppet.visual.skeletonSegmentCount > 0)
                skeletonLineCount += DrawSkeleton(puppet.visual, io, drawList, style.color, outline);

            char label[64]{};
            snprintf(label, sizeof(label), "%s  %.0fm%s", style.label, distance,
                     puppet.isDead ? "  DEAD" : "");
            drawList->AddText(ImVec2(minimum.x, minimum.y - ImGui::GetFontSize() - 2.0f), outline, label);
            drawList->AddText(ImVec2(minimum.x + 1.0f, minimum.y - ImGui::GetFontSize() - 3.0f), style.color,
                              label);
        }

        const ULONGLONG now = GetTickCount64();
        if ((settings.enabled || diagnosticsWindows < 3) && now - lastDiagnosticsTick >= 3000)
        {
            const float minimumDepth = frontCount > 0 ? minimumForwardDepth : 0.0f;
            Diagnostics::Log(
                "ESP diagnostics: snapshots=%zu categories[civilian=%zu enemy=%zu police=%zu other=%zu] "
                "dead=%zu categorized=%zu categoryEnabled=%zu projected=%zu front=%zu realBounds=%zu "
                "skeletonLines=%zu depthRange=[%.2f,%.2f] maxDistance=%.1f distanceRejected=%zu "
                "withinDistance=%zu drawn=%zu enabled=%d",
                count, civilianCount, enemyCount, policeCount, unclassifiedCount, deadCount, categorizedCount,
                categoryEnabledCount, projectedCount, frontCount, realBoundsCount, skeletonLineCount, minimumDepth,
                maximumForwardDepth, settings.maxDistanceMeters, distanceRejectedCount, withinDistanceCount,
                drawnCount, settings.enabled ? 1 : 0);
            lastDiagnosticsTick = now;
            ++diagnosticsWindows;
        }
    }
}
