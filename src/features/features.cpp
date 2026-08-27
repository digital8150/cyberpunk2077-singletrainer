#include "features.h"
#include "aimbot.h"
#include "esp.h"
#include "fps_counter.h"
#include "../config.h"

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
        Esp::DrawOverlay(g_settings.esp);
        Aimbot::DrawOverlay(g_settings.aimbot);
        FpsCounter::Draw(g_settings.showFps);
    }
}
