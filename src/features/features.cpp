#include "features.h"
#include "aimbot.h"
#include "esp.h"
#include "fps_counter.h"
#include "../config.h"
#include "../game/entity_tracker.h"
#include "../game/player_modifiers.h"
#include "../game/visibility.h"

#include <array>

namespace
{
    Features::Settings g_settings;

    // Present 스레드 전용. OnPresent가 렌더 뮤텍스를 잡은 채로만 들어오므로 프레임당 한 번 채워진다.
    std::array<Game::EntityTracker::PuppetSnapshot, 128> g_frameSnapshots{};

    Features::FrameSnapshots CaptureFrameSnapshots()
    {
        Features::FrameSnapshots frame;
        frame.puppets = g_frameSnapshots.data();
        frame.count = Game::EntityTracker::GetPuppetSnapshots(g_frameSnapshots.data(), g_frameSnapshots.size());
        return frame;
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
        // Present may publish the desired value, but all StatsSystem calls are drained by the game main tick.
        Game::PlayerModifiers::PublishDesired(g_settings.misc);
        // ESP와 에임봇이 같은 시야 캐시를 공유하므로 프레임 예산은 여기서 한 번만 초기화한다.
        Game::Visibility::BeginFrame();
        // 트래킹 리스트 순회도 프레임당 한 번이면 충분하다. ESP와 에임봇이 같은 배열을 읽는다.
        const FrameSnapshots frame = CaptureFrameSnapshots();
        Esp::DrawOverlay(g_settings.esp, frame);
        Aimbot::DrawOverlay(g_settings.aimbot, frame);
        FpsCounter::Draw(g_settings.debug.showFps);
    }

    void UpdateHeadless(float displayWidth, float displayHeight)
    {
        Config::Update();
        Game::PlayerModifiers::PublishDesired(g_settings.misc);
        Game::Visibility::BeginFrame();
        const FrameSnapshots frame = CaptureFrameSnapshots();
        Aimbot::UpdateHeadless(g_settings.aimbot, frame, displayWidth, displayHeight);
    }
}
