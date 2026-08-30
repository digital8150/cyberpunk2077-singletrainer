#pragma once

// 오버레이 화면 조립부. 컨트롤과 레이아웃 조각은 전부 `ui_kit.h`가 제공하고, 이 파일은 그것을
// 페이지로 엮는 일만 한다 — 페이지마다 컨트롤을 따로 그리면 같은 애플리케이션으로 읽히지 않는다.
// 설정 값 자체는 `Features::Settings`가 소유하며 여기서는 표시와 상호작용만 담당한다.
namespace Widgets
{
    // 폰트 아틀라스 구성 + ImGui 스타일. ImGui 백엔드 초기화보다 먼저 호출되어야 한다.
    void ApplyStyle();

    // 메뉴가 열려 있을 때의 창 전체.
    void DrawMainMenu();

    // 주입 직후 잠깐 떠 있는 안내 토스트. 메뉴 표시 여부와 무관하다.
    void DrawStartupHint();

    // 프리뷰 하네스(`tools/ui_preview`)가 페이지를 골라 스크린샷을 찍기 위한 진입점.
    // 인게임에서는 사이드바 내비게이션이 같은 값을 바꾼다. 0=Aimbot, 1=ESP, 2=Misc, 3=Debug.
    void SelectPage(int index);
}
