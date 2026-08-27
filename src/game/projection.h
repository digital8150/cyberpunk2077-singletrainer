#pragma once

namespace Game::Projection
{
    struct ScreenPoint
    {
        float x = 0.0f;
        float y = 0.0f;
        bool visible = false;
        bool behind = true;
    };

    // Uses REDengine's active camera projection routine. Coordinates are ImGui display pixels.
    bool WorldToScreen(const float world[3], float displayWidth, float displayHeight, ScreenPoint& result);
}
