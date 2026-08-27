#pragma once

// AGENTS.md 기술 스택 절에서 정한 "ImGui를 즉시모드 캔버스로만 쓰고 커스텀 드로잉으로 위젯을 그린다"
// 방식의 구현. 지금은 골격 단계라 pill 토글 하나와 최소 메뉴만 있다 — 슬라이더/카드 패널/사이드바는
// 실제 ESP/Aimbot 설정 항목이 생기면 이 파일에 계속 추가해 나갈 것.
namespace Widgets
{
    // ImGuiStyle 커스터마이징 (라운딩, 색상 팔레트). ImGui_ImplWin32_Init보다 먼저 호출되어야 폰트 로드
    // 등 이후 단계에 영향을 준다.
    void ApplyStyle();

    // 지금은 ESP/Aimbot 토글만 있는 자리표시 메뉴. 실제 기능이 생기면 탭/사이드바 구조로 확장할 것
    // (AGENTS.md 기능 스펙 절 참고).
    void DrawMainMenu();

    // pill 모양 토글 스위치. 스톡 위젯이 없어서 InvisibleButton으로 입력만 가져오고 배경/thumb은
    // ImDrawList로 직접 그린다. 반환값은 "이번 프레임에 눌렸는지"(값 자체는 *value에 반영됨).
    bool ToggleSwitch(const char* label, bool* value);
}
