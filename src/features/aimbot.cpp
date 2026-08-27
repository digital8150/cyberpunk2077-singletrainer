#include "aimbot.h"
#include "features.h"
#include "../diagnostics.h"
#include "../framework.h"
#include "../game/aim_assist.h"
#include "../game/entity_tracker.h"
#include "../game/projection.h"
#include "../game/visibility.h"
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
        bool g_aimActive = false;
        std::uint64_t g_lockedEntityId = 0;
        ULONGLONG g_lastApplyTick = 0;

        void StopAim()
        {
            if (g_aimActive)
                Game::AimAssist::ClearMemoryAim();
            g_aimActive = false;
            g_lockedEntityId = 0;
            g_lastApplyTick = 0;
        }

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
            if (puppet.visual.hasHeadPosition)
            {
                std::copy(std::begin(puppet.visual.headPosition), std::end(puppet.visual.headPosition), output);
                return;
            }
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
    }

    void DrawOverlay(const Features::AimbotSettings& settings)
    {
        if (!settings.enabled)
        {
            StopAim();
            return;
        }

        Game::AimAssist::ProbePlayerCamera();

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
        float bestWorld[3]{};
        std::uint64_t bestEntityId = 0;
        Game::Projection::ScreenPoint lockedPoint;
        float lockedWorld[3]{};
        bool lockedTargetAvailable = false;
        const bool activationHeld = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        float camera[3]{};
        const bool visibleOnly = settings.visibleOnly && Game::Projection::GetCameraPosition(camera);

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

            // 벽 뒤 대상으로 시점이 끌려가지 않도록, ESP와 같은 시야 캐시로 가려진 대상은 제외한다.
            if (visibleOnly)
            {
                const float torso[3] = {puppet.position[0], puppet.position[1], puppet.position[2] + 1.1f};
                if (Game::Visibility::Query(puppet.entityId, camera, aimWorld, torso) ==
                    Game::Visibility::State::Occluded)
                {
                    continue;
                }
            }

            const float deltaX = point.x - center.x;
            const float deltaY = point.y - center.y;
            const float screenDistance = std::sqrt(deltaX * deltaX + deltaY * deltaY);
            if (activationHeld && puppet.entityId == g_lockedEntityId)
            {
                lockedTargetAvailable = true;
                lockedPoint = point;
                std::copy(std::begin(aimWorld), std::end(aimWorld), lockedWorld);
            }
            if (screenDistance <= settings.fovRadiusPixels && screenDistance < bestScreenDistance)
            {
                bestScreenDistance = screenDistance;
                bestPoint = point;
                bestEntityId = puppet.entityId;
                std::copy(std::begin(aimWorld), std::end(aimWorld), bestWorld);
            }
        }

        if (lockedTargetAvailable)
        {
            bestEntityId = g_lockedEntityId;
            bestPoint = lockedPoint;
            std::copy(std::begin(lockedWorld), std::end(lockedWorld), bestWorld);
        }
        if (bestEntityId == 0)
        {
            StopAim();
            return;
        }

        drawList->AddCircle(ImVec2(bestPoint.x, bestPoint.y), 5.0f, IM_COL32(255, 92, 105, 245), 20, 1.6f);
        if (!activationHeld || Overlay::IsVisible() || !IsGameForeground())
        {
            StopAim();
            return;
        }

        if (g_lockedEntityId == 0)
            g_lockedEntityId = bestEntityId;
        const ULONGLONG now = GetTickCount64();
        const bool hardLock = settings.smoothing <= 0.001f;
        if (hardLock || now - g_lastApplyTick >= 16)
        {
            g_aimActive = Game::AimAssist::ApplyMemoryAim(bestWorld, settings.smoothing);
            g_lastApplyTick = now;
        }
        static ULONGLONG lastLogTick = 0;
        if (now - lastLogTick >= 2000)
        {
            Diagnostics::Log("memory aim active: target=%016llX mode=%s world=(%.2f,%.2f,%.2f) "
                             "screenDelta=(%.1f,%.1f) depth=%.1f",
                             static_cast<unsigned long long>(bestEntityId), hardLock ? "hard" : "smooth",
                             bestWorld[0], bestWorld[1], bestWorld[2],
                             bestPoint.x - center.x, bestPoint.y - center.y, bestPoint.depth);
            lastLogTick = now;
        }
    }

    void Shutdown()
    {
        StopAim();
        Game::AimAssist::Shutdown();
    }
}
