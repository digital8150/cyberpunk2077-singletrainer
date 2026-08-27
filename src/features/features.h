#pragma once

namespace Features
{
    struct EspSettings
    {
        bool enabled = false;
        bool boundingBoxes = true;
        bool skeleton = false;
        bool healthBars = true;
    };

    struct AimbotSettings
    {
        bool enabled = false;
        bool drawFovCircle = true;
        float fovRadiusPixels = 180.0f;
        float smoothing = 8.0f;
    };

    struct Settings
    {
        bool showFps = true;
        EspSettings esp;
        AimbotSettings aimbot;
    };

    Settings& GetSettings();

    // ImGui::NewFrame 이후 호출. 메뉴 표시 여부와 무관하게 활성 기능의 HUD 요소를 그린다.
    void DrawOverlay();
}
