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
        // 카메라/컨트롤러를 움직이지 않고, 게임 자체 크로스헤어 코어의 direction만 타겟 쪽으로 바꾼다.
        bool silentAim = false;
        // 에임을 걸어둘 키의 가상 키 코드. 기본값 0x02는 VK_RBUTTON(마우스 오른쪽 버튼).
        unsigned int activationKey = 0x02;
        // 진단용: 오버레이 GPU 제출을 멈추고 타겟 선택만 CPU에서 돌린다. 메뉴가 닫혀 있고 오버레이가 이미
        // 한 번 초기화된 뒤에만 적용되므로, 켜둔 채로도 Insert로 메뉴를 다시 열 수 있다.
        bool headlessDiagnostics = false;
        bool drawFovCircle = true;
        bool targetEnemies = true;
        bool targetPolice = false;
        // ESP의 visibilityCheck와 같은 게임 메인 틱 시야 캐시를 쓴다. 켜면 벽 뒤 대상은 클래식/사일런트
        // 양쪽 모두에서 타겟 후보에서 빠진다. 기본값은 ESP 쪽과 같이 꺼둔다.
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
