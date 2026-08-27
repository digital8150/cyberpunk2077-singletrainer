#pragma once

namespace Features
{
    struct EspSettings
    {
        bool enabled = false;
        bool boundingBoxes = true;
        bool skeleton = false;
        bool healthBars = true;
        bool nativeHighlight = false;
        bool hideDead = true;
        bool showCivilians = true;
        bool showEnemies = true;
        bool showPolice = true;
        bool showUnclassified = true;
        float maxDistanceMeters = 300.0f;
    };

    struct AimbotSettings
    {
        bool enabled = false;
        bool drawFovCircle = true;
        bool targetEnemies = true;
        bool targetPolice = false;
        float fovRadiusPixels = 180.0f;
        float smoothing = 8.0f;
        float maxDistanceMeters = 150.0f;
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
