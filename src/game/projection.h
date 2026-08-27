#pragma once

namespace Game::Projection
{
    struct ScreenPoint
    {
        float x = 0.0f;
        float y = 0.0f;
        float depth = 0.0f;
        bool visible = false;
        bool behind = true;
    };

    // Uses REDengine's active camera projection routine. Coordinates are ImGui display pixels.
    bool WorldToScreen(const float world[3], float displayWidth, float displayHeight, ScreenPoint& result);

    // Active camera world position. Callers should fetch it once per frame and pass it down; the result is
    // validated by projecting it back (a point at the camera has ~zero forward depth).
    bool GetCameraPosition(float world[3]);
}
