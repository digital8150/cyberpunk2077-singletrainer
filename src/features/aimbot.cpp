#include "aimbot.h"
#include "features.h"
#include "../diagnostics.h"
#include "../framework.h"
#include "../game/entity_tracker.h"
#include "../game/projection.h"
#include "../ui/overlay.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Aimbot
{
    namespace
    {
        bool IsGameForeground()
        {
            HWND foreground = GetForegroundWindow();
            DWORD processId = 0;
            if (foreground)
                GetWindowThreadProcessId(foreground, &processId);
            return processId == GetCurrentProcessId();
        }

        bool IsEligible(const Game::EntityTracker::PuppetSnapshot& puppet,
                        const Features::AimbotSettings& settings)
        {
            using Game::EntityTracker::NpcCategory;
            if (puppet.isDead)
                return false;
            if (puppet.category == NpcCategory::Enemy)
                return settings.targetEnemies;
            if (puppet.category == NpcCategory::Police)
                return settings.targetPolice;
            return false;
        }

        void GetAimPoint(const Game::EntityTracker::PuppetSnapshot& puppet, float output[3])
        {
            if (puppet.visual.hasBounds)
            {
                output[0] = (puppet.visual.boundsMinimum[0] + puppet.visual.boundsMaximum[0]) * 0.5f;
                output[1] = (puppet.visual.boundsMinimum[1] + puppet.visual.boundsMaximum[1]) * 0.5f;
                const float height = puppet.visual.boundsMaximum[2] - puppet.visual.boundsMinimum[2];
                output[2] = puppet.visual.boundsMaximum[2] - height * 0.12f;
                return;
            }
            output[0] = puppet.position[0];
            output[1] = puppet.position[1];
            output[2] = puppet.position[2] + 1.62f;
        }

        void MoveMouseTowards(float deltaX, float deltaY, float smoothing, float deltaTime)
        {
            static float remainderX = 0.0f;
            static float remainderY = 0.0f;
            const float response = std::clamp(deltaTime * 60.0f / (std::max)(1.0f, smoothing), 0.01f, 1.0f);
            remainderX += std::clamp(deltaX * response, -80.0f, 80.0f);
            remainderY += std::clamp(deltaY * response, -80.0f, 80.0f);
            const LONG moveX = static_cast<LONG>(std::trunc(remainderX));
            const LONG moveY = static_cast<LONG>(std::trunc(remainderY));
            remainderX -= static_cast<float>(moveX);
            remainderY -= static_cast<float>(moveY);
            if (moveX == 0 && moveY == 0)
                return;

            INPUT input{};
            input.type = INPUT_MOUSE;
            input.mi.dx = moveX;
            input.mi.dy = moveY;
            input.mi.dwFlags = MOUSEEVENTF_MOVE;
            SendInput(1, &input, sizeof(input));
        }
    }

    void DrawOverlay(const Features::AimbotSettings& settings)
    {
        if (!settings.enabled)
            return;

        const ImGuiIO& io = ImGui::GetIO();
        if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
            return;

        const ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        if (settings.drawFovCircle)
        {
            drawList->AddCircle(center, settings.fovRadiusPixels, IM_COL32(0, 0, 0, 180), 128, 2.8f);
            drawList->AddCircle(center, settings.fovRadiusPixels, IM_COL32(62, 157, 255, 220), 128, 1.4f);
        }

        static std::array<Game::EntityTracker::PuppetSnapshot, 128> puppets{};
        const std::size_t count = Game::EntityTracker::GetPuppetSnapshots(puppets.data(), puppets.size());
        float bestScreenDistance = (std::numeric_limits<float>::max)();
        Game::Projection::ScreenPoint bestPoint;
        std::uint64_t bestEntityId = 0;

        for (std::size_t i = 0; i < count; ++i)
        {
            const auto& puppet = puppets[i];
            if (!IsEligible(puppet, settings))
                continue;

            float aimWorld[3]{};
            GetAimPoint(puppet, aimWorld);
            Game::Projection::ScreenPoint point;
            if (!Game::Projection::WorldToScreen(aimWorld, io.DisplaySize.x, io.DisplaySize.y, point) ||
                point.behind || point.depth <= 0.0f || point.depth > settings.maxDistanceMeters)
            {
                continue;
            }

            const float deltaX = point.x - center.x;
            const float deltaY = point.y - center.y;
            const float screenDistance = std::sqrt(deltaX * deltaX + deltaY * deltaY);
            if (screenDistance <= settings.fovRadiusPixels && screenDistance < bestScreenDistance)
            {
                bestScreenDistance = screenDistance;
                bestPoint = point;
                bestEntityId = puppet.entityId;
            }
        }

        if (bestEntityId == 0)
            return;

        drawList->AddCircle(ImVec2(bestPoint.x, bestPoint.y), 5.0f, IM_COL32(255, 92, 105, 245), 20, 1.6f);
        const bool activationHeld = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        if (!activationHeld || Overlay::IsVisible() || !IsGameForeground())
            return;

        MoveMouseTowards(bestPoint.x - center.x, bestPoint.y - center.y, settings.smoothing, io.DeltaTime);
        static ULONGLONG lastLogTick = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - lastLogTick >= 2000)
        {
            Diagnostics::Log("aimbot active: target=%016llX screenDelta=(%.1f,%.1f) depth=%.1f",
                             static_cast<unsigned long long>(bestEntityId), bestPoint.x - center.x,
                             bestPoint.y - center.y, bestPoint.depth);
            lastLogTick = now;
        }
    }
}
