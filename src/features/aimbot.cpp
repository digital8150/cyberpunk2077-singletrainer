#include "aimbot.h"
#include "features.h"
#include "../diagnostics.h"
#include "../framework.h"
#include "../game/aim_assist.h"
#include "../game/entity_tracker.h"
#include "../game/projection.h"
#include "../game/silent_aim.h"
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
        Stats g_stats;

        void StopAim()
        {
            if (g_aimActive)
                Game::AimAssist::ClearMemoryAim();
            Game::SilentAim::ClearTarget();
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
            using Game::EntityTracker::Hostility;
            using Game::EntityTracker::NpcCategory;
            if (puppet.isDead)
                return false;
            // Police keep their own toggle even after they turn on the player.
            if (puppet.category == NpcCategory::Police)
                return settings.targetPolice;
            // Runtime attitude first: an NPC that started neutral and turned hostile keeps its spawn archetype, so
            // the category alone would leave it untargetable for the whole fight.
            if (puppet.hostility == Hostility::Hostile)
                return settings.targetEnemies;
            if (puppet.category == NpcCategory::Enemy)
                return settings.targetEnemies;
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

    void RunFrame(const Features::AimbotSettings& settings, float displayWidth, float displayHeight,
                  ImDrawList* drawList)
    {
        if (!settings.enabled)
        {
            StopAim();
            return;
        }

        if (!settings.silentAim)
            Game::AimAssist::ProbePlayerCamera();

        if (displayWidth <= 0.0f || displayHeight <= 0.0f)
            return;

        const ImVec2 center(displayWidth * 0.5f, displayHeight * 0.5f);
        if (drawList && settings.drawFovCircle)
        {
            drawList->AddCircle(center, settings.fovRadiusPixels, IM_COL32(0, 0, 0, 180), 128, 2.8f);
            drawList->AddCircle(center, settings.fovRadiusPixels, IM_COL32(62, 157, 255, 220), 128, 1.4f);
        }

        g_stats = Stats{};
        static std::array<Game::EntityTracker::PuppetSnapshot, 128> puppets{};
        const std::size_t count = Game::EntityTracker::GetPuppetSnapshots(puppets.data(), puppets.size());
        float bestScreenDistance = (std::numeric_limits<float>::max)();
        Game::Projection::ScreenPoint bestPoint;
        float bestWorld[3]{};
        std::uint64_t bestEntityId = 0;
        Game::Projection::ScreenPoint lockedPoint;
        float lockedWorld[3]{};
        bool lockedTargetAvailable = false;
        const int activationKey = static_cast<int>(settings.activationKey);
        const bool activationHeld =
            activationKey > 0 && activationKey < 0xFF && (GetAsyncKeyState(activationKey) & 0x8000) != 0;
        float camera[3]{};
        const bool visibleOnly = settings.visibleOnly && Game::Projection::GetCameraPosition(camera);

        for (std::size_t i = 0; i < count; ++i)
        {
            const auto& puppet = puppets[i];
            if (!IsEligible(puppet, settings))
                continue;
            ++g_stats.candidates;

            // 스탯 풀이 아직 안 잡힌 대상은 살아 있는지도, 체력이 얼마인지도 확인되지 않는다. 실제로
            // healthValid=0인 대상이 사일런트 에임에 armed된 사례가 로그에 남아서 기본으로 걸러낸다.
            if (settings.requireHealthPool && (!puppet.healthValid || puppet.healthCurrent <= 0.0f))
            {
                ++g_stats.skippedNoHealthPool;
                continue;
            }
            // 차량/보스급 퍼펫이 일반 NPC로 분류되어 들어오는 경우를 최대 체력으로 걸러낸다. 관측된 사례는
            // 최대 체력 4,343짜리 대상이었다. 체력 풀이 아직 없으면 위 필터가 처리하므로 여기선 건너뛴다.
            if (settings.limitHealthPool && puppet.healthValid && puppet.healthMax > settings.maxHealthPool)
            {
                ++g_stats.skippedHealthCap;
                continue;
            }

            float aimWorld[3]{};
            GetAimPoint(puppet, aimWorld);
            Game::Projection::ScreenPoint point;
            if (!Game::Projection::WorldToScreen(aimWorld, displayWidth, displayHeight, point) ||
                point.behind || point.depth <= 0.0f || point.depth > settings.maxDistanceMeters)
            {
                continue;
            }

            const float deltaX = point.x - center.x;
            const float deltaY = point.y - center.y;
            const float screenDistance = std::sqrt(deltaX * deltaX + deltaY * deltaY);

            // 클래식 모드에선 시점이, 사일런트 모드에선 탄도가 벽 뒤 대상으로 끌려가지 않도록 ESP와 같은
            // 시야 캐시로 가려진 대상을 후보에서 뺀다. 캐시가 비어 있으면(Unknown) 통과시키는 fail-open이
            // 그대로 유지되고, FOV 안이거나 이미 잠긴 대상만 우선 요청으로 넣어 ESP의 대량 요청 뒤로
            // 밀리지 않게 한다.
            if (visibleOnly)
            {
                const float torso[3] = {puppet.position[0], puppet.position[1], puppet.position[2] + 1.1f};
                const bool priority =
                    screenDistance <= settings.fovRadiusPixels || puppet.entityId == g_lockedEntityId;
                if (Game::Visibility::Query(puppet.entityId, camera, aimWorld, torso, priority) ==
                    Game::Visibility::State::Occluded)
                {
                    ++g_stats.skippedOccluded;
                    continue;
                }
            }

            ++g_stats.eligible;
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

        const Game::EntityTracker::PuppetSnapshot* selected = nullptr;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (puppets[i].entityId == bestEntityId)
            {
                selected = &puppets[i];
                break;
            }
        }
        g_stats.targetEntityId = bestEntityId;
        if (selected)
        {
            g_stats.targetHealthValid = selected->healthValid;
            g_stats.targetHealth = selected->healthCurrent;
            g_stats.targetHealthMax = selected->healthMax;
        }

        if (drawList)
            drawList->AddCircle(ImVec2(bestPoint.x, bestPoint.y), 5.0f, IM_COL32(255, 92, 105, 245), 20, 1.6f);
        if (!activationHeld || Overlay::IsVisible() || !IsGameForeground())
        {
            StopAim();
            return;
        }

        if (g_lockedEntityId == 0)
            g_lockedEntityId = bestEntityId;
        const ULONGLONG now = GetTickCount64();
        if (settings.silentAim)
        {
            if (g_aimActive)
                Game::AimAssist::ClearMemoryAim();
            g_aimActive = false;
            Game::SilentAim::PublishTarget(bestWorld, true);
            static ULONGLONG lastSilentLogTick = 0;
            if (now - lastSilentLogTick >= 2000)
            {
                const Game::SilentAim::DiagnosticsSnapshot diagnostics = Game::SilentAim::GetDiagnostics();
                Diagnostics::Log("silent aim armed: target=%016llX world=(%.2f,%.2f,%.2f) "
                                 "healthValid=%u health=%.2f/%.2f dead=%u "
                                 "candidates=%u eligible=%u noPool=%u overCap=%u occluded=%u "
                                 "crosshairCoreHook=%u calls=%llu redirects=%llu rejected=%llu",
                                 static_cast<unsigned long long>(bestEntityId), bestWorld[0], bestWorld[1], bestWorld[2],
                                 selected && selected->healthValid ? 1u : 0u,
                                 selected ? selected->healthCurrent : 0.0f,
                                 selected ? selected->healthMax : 0.0f,
                                 selected && selected->isDead ? 1u : 0u,
                                 g_stats.candidates, g_stats.eligible, g_stats.skippedNoHealthPool,
                                 g_stats.skippedHealthCap, g_stats.skippedOccluded,
                                 diagnostics.crosshairCoreHookCreated ? 1u : 0u,
                                 static_cast<unsigned long long>(diagnostics.nativeCrosshairCoreCalls),
                                 static_cast<unsigned long long>(diagnostics.nativeCrosshairCoreRedirects),
                                 static_cast<unsigned long long>(diagnostics.rejectedShots));
                lastSilentLogTick = now;
            }
            return;
        }
        Game::SilentAim::ClearTarget();
        const bool hardLock = settings.smoothing <= 0.001f;
        if (hardLock || now - g_lastApplyTick >= 16)
        {
            g_aimActive = Game::AimAssist::ApplyMemoryAim(bestWorld, settings.smoothing);
            g_lastApplyTick = now;
        }
        static ULONGLONG lastLogTick = 0;
        if (now - lastLogTick >= 2000)
        {
            const Game::AimAssist::DiagnosticsSnapshot diagnostics = Game::AimAssist::GetDiagnostics();
            Diagnostics::Log("memory aim active: target=%016llX mode=%s world=(%.2f,%.2f,%.2f) "
                             "screenDelta=(%.1f,%.1f) depth=%.1f inputCallbacks=%llu writes=%llu "
                             "calcFail=%llu fppCamera=%p cameraSystem=%p angularError=%.2f",
                             static_cast<unsigned long long>(bestEntityId), hardLock ? "hard" : "smooth",
                             bestWorld[0], bestWorld[1], bestWorld[2],
                             bestPoint.x - center.x, bestPoint.y - center.y, bestPoint.depth,
                             static_cast<unsigned long long>(diagnostics.hookCallbacks),
                             static_cast<unsigned long long>(diagnostics.appliedWrites),
                             static_cast<unsigned long long>(diagnostics.calculationFailures),
                             diagnostics.fppCamera, diagnostics.cameraSystem, diagnostics.angularError);
            lastLogTick = now;
        }
    }

    Stats GetStats()
    {
        return g_stats;
    }

    void DrawOverlay(const Features::AimbotSettings& settings)
    {
        const ImGuiIO& io = ImGui::GetIO();
        RunFrame(settings, io.DisplaySize.x, io.DisplaySize.y, ImGui::GetBackgroundDrawList());
    }

    void UpdateHeadless(const Features::AimbotSettings& settings, float displayWidth, float displayHeight)
    {
        RunFrame(settings, displayWidth, displayHeight, nullptr);
    }

    void Shutdown()
    {
        StopAim();
        Game::AimAssist::Shutdown();
    }
}
