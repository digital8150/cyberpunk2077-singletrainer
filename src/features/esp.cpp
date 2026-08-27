#include "esp.h"
#include "features.h"
#include "../game/entity_tracker.h"
#include "../game/projection.h"

#include <imgui.h>

namespace Esp
{
    void DrawOverlay(const Features::EspSettings& settings)
    {
        static bool projectionValidated = false;
        const Game::EntityTracker::Stats stats = Game::EntityTracker::GetStats();
        if (!stats.hasLastPuppet)
            return;

        if (!settings.enabled && projectionValidated)
            return;

        const ImGuiIO& io = ImGui::GetIO();
        Game::Projection::ScreenPoint point;
        if (!Game::Projection::WorldToScreen(stats.lastPuppetPosition, io.DisplaySize.x, io.DisplaySize.y, point))
            return;
        projectionValidated = true;

        if (!settings.enabled || !point.visible)
        {
            return;
        }

        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        const ImVec2 center(point.x, point.y);
        constexpr ImU32 outline = IM_COL32(0, 0, 0, 210);
        constexpr ImU32 accent = IM_COL32(74, 222, 128, 245);
        drawList->AddCircle(center, 7.0f, outline, 24, 4.0f);
        drawList->AddCircle(center, 7.0f, accent, 24, 2.0f);
        drawList->AddLine(ImVec2(center.x - 11.0f, center.y), ImVec2(center.x + 11.0f, center.y), outline, 4.0f);
        drawList->AddLine(ImVec2(center.x, center.y - 11.0f), ImVec2(center.x, center.y + 11.0f), outline, 4.0f);
        drawList->AddLine(ImVec2(center.x - 11.0f, center.y), ImVec2(center.x + 11.0f, center.y), accent, 1.5f);
        drawList->AddLine(ImVec2(center.x, center.y - 11.0f), ImVec2(center.x, center.y + 11.0f), accent, 1.5f);
        drawList->AddText(ImVec2(center.x + 12.0f, center.y - 9.0f), accent, "last NPC registration");
    }
}
