#include "features.h"
#include "aimbot.h"
#include "esp.h"
#include "fps_counter.h"
#include "../config.h"
#include "../framework.h"
#include "../diagnostics.h"
#include "../ui/overlay.h"
#include "../game/silent_aim.h"
#include "../game/shot_trace.h"
#include "../game/entity_tracker.h"
#include "../game/player_modifiers.h"
#include "../game/visibility.h"
#include "../profiling.h"

#include <array>
#include <utility>
#include <cstdio>
#include <imgui.h>

namespace
{
    Features::Settings g_settings;

    // Present 스레드 전용. OnPresent가 렌더 뮤텍스를 잡은 채로만 들어오므로 프레임당 한 번 채워진다.
    std::array<Game::EntityTracker::PuppetSnapshot, 128> g_frameSnapshots{};
    bool g_aimbotEnabledLastFrame = false;
    ULONGLONG g_profileToastUntil = 0;

    void UpdateAimbotProfile()
    {
        const unsigned traceSettings = (g_settings.aimbot.enabled ? 1u : 0u) |
            (g_settings.aimbot.silentAim ? 2u : 0u) | (g_settings.misc.noSpread ? 4u : 0u) |
            (g_settings.misc.noRecoil ? 8u : 0u) | (g_settings.aimbot.boneMask << 4) |
            (g_settings.aimbot.nearestBone ? 512u : 0u) | (g_settings.activeAimbotProfile << 10);
        Game::ShotTrace::Tick(Overlay::IsVisible(), traceSettings);
        static bool switchHeld = false;
        static Features::AimbotSettings previous;
        static unsigned previousProfile = 0;
        DWORD foregroundProcess = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
        const unsigned key = g_settings.profileSwitchKey;
        const bool held = key > 0 && key < 0xFF && (GetAsyncKeyState(static_cast<int>(key)) & 0x8000);
        if (held && !switchHeld && !Overlay::IsVisible() && foregroundProcess == GetCurrentProcessId())
        {
            std::swap(g_settings.aimbot, g_settings.alternateAimbot);
            g_settings.activeAimbotProfile ^= 1u;
            g_profileToastUntil = GetTickCount64() + 1800;
            Diagnostics::Log("aimbot profile switched: profile=%u", g_settings.activeAimbotProfile + 1);
        }
        switchHeld = held;
        if (!(previous == g_settings.aimbot) || previousProfile != g_settings.activeAimbotProfile)
        {
            Aimbot::Disable();
            Game::SilentAim::InvalidateTarget();
            previous = g_settings.aimbot;
            previousProfile = g_settings.activeAimbotProfile;
        }
    }

    Features::FrameSnapshots CaptureFrameSnapshots()
    {
        Features::FrameSnapshots frame;
        frame.puppets = g_frameSnapshots.data();
        frame.count = Game::EntityTracker::GetPuppetSnapshots(g_frameSnapshots.data(), g_frameSnapshots.size());
        return frame;
    }

    void PublishFeatureRequirements(bool espConsumerActive)
    {
        const bool nativeHighlight = g_settings.esp.enabled && g_settings.esp.nativeHighlight;
        Game::EntityTracker::UpdateNativeHighlights(
            nativeHighlight, g_settings.esp.showCivilians, g_settings.esp.showEnemies,
            g_settings.esp.showPolice, g_settings.esp.showUnclassified, g_settings.esp.hideDead);

        const bool espActive = espConsumerActive && g_settings.esp.enabled;
        const bool needsHealth = (espActive && g_settings.esp.healthBars) ||
                                 (g_settings.aimbot.enabled &&
                                  (g_settings.aimbot.requireHealthPool || g_settings.aimbot.limitHealthPool)) ||
                                 (nativeHighlight && g_settings.esp.hideDead);
        const bool needsAttitude = espActive || g_settings.aimbot.enabled || nativeHighlight;
        const bool needsPose = espActive || g_settings.aimbot.enabled;
        Game::EntityTracker::UpdateFeatureRequirements(needsHealth, needsAttitude, needsPose);
    }

    bool NeedsVisibilityFrame(bool espConsumerActive)
    {
        return (espConsumerActive && g_settings.esp.enabled && g_settings.esp.visibilityCheck) ||
               (g_settings.aimbot.enabled && g_settings.aimbot.visibleOnly);
    }
}

namespace Features
{
    Settings& GetSettings()
    {
        return g_settings;
    }

    void DrawOverlay(bool menuVisible)
    {
        UpdateAimbotProfile();
        Config::Update();
        const bool graphEnabled = g_settings.debug.showGraph;
        if (graphEnabled)
            Diagnostics::Profile::BeginPresentFrame();
        // Present may publish the desired value, but all StatsSystem calls are drained by the game main tick.
        Game::PlayerModifiers::PublishDesired(g_settings.misc);
        PublishFeatureRequirements(true);
        if (NeedsVisibilityFrame(true))
            Game::Visibility::BeginFrame();

        FrameSnapshots frame;
        if (g_settings.esp.enabled || g_settings.aimbot.enabled)
            frame = CaptureFrameSnapshots();
        if (g_settings.esp.enabled)
            Esp::DrawOverlay(g_settings.esp, frame);
        if (g_settings.aimbot.enabled)
            Aimbot::DrawOverlay(g_settings.aimbot, frame);
        else if (g_aimbotEnabledLastFrame)
            Aimbot::Disable();
        g_aimbotEnabledLastFrame = g_settings.aimbot.enabled;

        if (graphEnabled)
            Diagnostics::Profile::EndPresentFrame();
        FpsCounter::Draw(g_settings.debug, menuVisible);
        if (!menuVisible && Game::ShotTrace::IsCapturing())
            ImGui::GetForegroundDrawList()->AddText(ImVec2(20.0f, 20.0f), IM_COL32(255, 110, 80, 255),
                                                   "SHOT TRACE REC - 20s / F8 stop");
        if (!menuVisible && GetTickCount64() < g_profileToastUntil)
        {
            char label[64]{};
            snprintf(label, sizeof(label), g_settings.ui.language == Language::Korean
                         ? "에임봇 프로필 %u" : "Aimbot profile %u", g_settings.activeAimbotProfile + 1);
            const ImVec2 size = ImGui::CalcTextSize(label);
            const ImVec2 origin((ImGui::GetIO().DisplaySize.x - size.x) * 0.5f, 32.0f);
            auto* draw = ImGui::GetForegroundDrawList();
            draw->AddRectFilled(ImVec2(origin.x - 14.0f, origin.y - 9.0f),
                                ImVec2(origin.x + size.x + 14.0f, origin.y + size.y + 9.0f),
                                IM_COL32(22, 27, 38, 230), 8.0f);
            draw->AddText(origin, IM_COL32(235, 241, 255, 255), label);
        }
    }

    void UpdateHeadless(float displayWidth, float displayHeight)
    {
        UpdateAimbotProfile();
        Config::Update();
        Game::PlayerModifiers::PublishDesired(g_settings.misc);
        PublishFeatureRequirements(false);
        if (NeedsVisibilityFrame(false))
            Game::Visibility::BeginFrame();

        if (g_settings.aimbot.enabled)
        {
            const FrameSnapshots frame = CaptureFrameSnapshots();
            Aimbot::UpdateHeadless(g_settings.aimbot, frame, displayWidth, displayHeight);
        }
        else if (g_aimbotEnabledLastFrame)
        {
            Aimbot::Disable();
        }
        g_aimbotEnabledLastFrame = g_settings.aimbot.enabled;
    }
}
