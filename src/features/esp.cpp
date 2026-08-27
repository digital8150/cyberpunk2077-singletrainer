#include "esp.h"
#include "features.h"
#include "../game/entity_tracker.h"
#include "../game/projection.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace Esp
{
    void DrawOverlay(const Features::EspSettings& settings)
    {
        std::array<Game::EntityTracker::PuppetSnapshot, 128> puppets{};
        const std::size_t count = Game::EntityTracker::GetPuppetSnapshots(puppets.data(), puppets.size());
        if (count == 0)
            return;

        static bool projectionValidated = false;
        if (!settings.enabled && projectionValidated)
            return;

        const ImGuiIO& io = ImGui::GetIO();
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        constexpr ImU32 outline = IM_COL32(0, 0, 0, 220);
        constexpr ImU32 accent = IM_COL32(74, 222, 128, 245);

        for (std::size_t i = 0; i < count; ++i)
        {
            const Game::EntityTracker::PuppetSnapshot& puppet = puppets[i];
            float headWorld[3] = {puppet.position[0], puppet.position[1], puppet.position[2] + 1.8f};
            Game::Projection::ScreenPoint feet;
            Game::Projection::ScreenPoint head;
            if (!Game::Projection::WorldToScreen(puppet.position, io.DisplaySize.x, io.DisplaySize.y, feet) ||
                !Game::Projection::WorldToScreen(headWorld, io.DisplaySize.x, io.DisplaySize.y, head))
            {
                continue;
            }
            projectionValidated = true;

            if (!settings.enabled || feet.behind || head.behind)
                continue;

            const float height = std::abs(feet.y - head.y);
            if (height < 3.0f || height > io.DisplaySize.y * 2.0f)
                continue;
            const float centerX = (feet.x + head.x) * 0.5f;
            const float width = height * 0.42f;
            const ImVec2 minimum(centerX - width * 0.5f, (std::min)(feet.y, head.y));
            const ImVec2 maximum(centerX + width * 0.5f, (std::max)(feet.y, head.y));

            if (maximum.x < 0.0f || minimum.x > io.DisplaySize.x || maximum.y < 0.0f ||
                minimum.y > io.DisplaySize.y)
            {
                continue;
            }

            if (settings.boundingBoxes)
            {
                drawList->AddRect(ImVec2(minimum.x - 1.0f, minimum.y - 1.0f),
                                  ImVec2(maximum.x + 1.0f, maximum.y + 1.0f), outline, 2.0f, 3.0f,
                                  ImDrawFlags_None);
                drawList->AddRect(minimum, maximum, accent, 2.0f, 1.4f, ImDrawFlags_None);
            }

            char label[32]{};
            snprintf(label, sizeof(label), "NPC %llX", static_cast<unsigned long long>(puppet.entityId));
            drawList->AddText(ImVec2(minimum.x, minimum.y - ImGui::GetFontSize() - 2.0f), outline, label);
            drawList->AddText(ImVec2(minimum.x + 1.0f, minimum.y - ImGui::GetFontSize() - 3.0f), accent, label);
        }
    }
}
