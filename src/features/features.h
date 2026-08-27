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
        // hideOccluded가 켜져 있으면 아예 그리지 않는다. 물리 쿼리는 전용 워커 스레드에서 도는데
        // 게임 물리 스텝과 겹칠 수 있어 아직 장시간 검증 전이므로 기본값은 꺼짐이다.
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
        bool drawFovCircle = true;
        bool targetEnemies = true;
        bool targetPolice = false;
        // ESP의 visibilityCheck와 같은 물리 워커를 쓰므로 기본값도 같이 꺼둔다.
        bool visibleOnly = false;
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
