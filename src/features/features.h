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
        // 게임 physics 쿼리로 카메라→대상 시야를 검사한다. 가려진 대상은 흐리게 그리고,
        // hideOccluded가 켜져 있으면 아예 그리지 않는다. 쿼리는 게임 메인 틱에서 처리한다.
        bool visibilityCheck = false;
        bool hideOccluded = false;
        bool showCivilians = true;
        bool showEnemies = true;
        bool showPolice = true;
        bool showUnclassified = true;
        float maxDistanceMeters = 300.0f;
    };

    struct AimbotSettings
    {
        bool enabled = false;
        // Selects a target for the observation-gated fire/effect path without moving the camera/controller.
        bool silentAim = false;
        bool drawFovCircle = true;
        bool targetEnemies = true;
        bool targetPolice = false;
        // ESP의 visibilityCheck와 같은 게임 메인 틱 쿼리를 사용하므로 기본값도 같이 꺼둔다.
        bool visibleOnly = false;
        float fovRadiusPixels = 180.0f;
        float smoothing = 8.0f;
        float maxDistanceMeters = 150.0f;
    };

    struct Settings
    {
        bool showFps = true;
        bool noRecoil = false;
        EspSettings esp;
        AimbotSettings aimbot;
    };

    Settings& GetSettings();

    // ImGui::NewFrame 이후 호출. 메뉴 표시 여부와 무관하게 활성 기능의 HUD 요소를 그린다.
    void DrawOverlay();
    void UpdateHeadless(float displayWidth, float displayHeight);
}
