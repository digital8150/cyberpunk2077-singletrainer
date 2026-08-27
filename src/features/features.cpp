#include "features.h"
#include "aimbot.h"

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
        Aimbot::DrawOverlay(g_settings.aimbot);
    }
}
