#include "features.h"
#include "aimbot.h"
#include "esp.h"
#include "fps_counter.h"
#include "../config.h"
#include "../game/entity_tracker.h"
#include "../game/player_modifiers.h"
#include "../game/visibility.h"
#include "../profiling.h"

#include <array>

namespace
{
    Features::Settings g_settings;

    // Present 스레드 전용. OnPresent가 렌더 뮤텍스를 잡은 채로만 들어오므로 프레임당 한 번 채워진다.
    std::array<Game::EntityTracker::PuppetSnapshot, 128> g_frameSnapshots{};
    bool g_aimbotEnabledLastFrame = false;

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
        Game::EntityTracker::UpdateFeatureRequirements(needsHealth, needsAttitude);
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

    void DrawOverlay()
    {
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
        FpsCounter::Draw(g_settings.debug.showFps, graphEnabled);
    }

    void UpdateHeadless(float displayWidth, float displayHeight)
    {
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
