#include "features.h"
#include "aimbot.h"
#include "esp.h"
#include "fps_counter.h"
#include "../config.h"
#include "../game/player_modifiers.h"
#include "../game/visibility.h"

namespace
{
    Features::Settings g_settings;
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
        Game::PlayerModifiers::PublishDesired(g_settings.noRecoil);
        // ESP와 에임봇이 같은 시야 캐시를 공유하므로 프레임 예산은 여기서 한 번만 초기화한다.
        Game::Visibility::BeginFrame();
        Esp::DrawOverlay(g_settings.esp);
        Aimbot::DrawOverlay(g_settings.aimbot);
        FpsCounter::Draw(g_settings.showFps);
    }

    void UpdateHeadless(float displayWidth, float displayHeight)
    {
        Config::Update();
        Game::PlayerModifiers::PublishDesired(g_settings.noRecoil);
        Game::Visibility::BeginFrame();
        Aimbot::UpdateHeadless(g_settings.aimbot, displayWidth, displayHeight);
    }
}
