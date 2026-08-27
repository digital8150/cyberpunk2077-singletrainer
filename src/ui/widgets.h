#pragma once

// AGENTS.md 기술 스택 절에서 정한 "ImGui를 즉시모드 캔버스로만 쓰고 커스텀 드로잉으로 위젯을 그린다"
// 방식의 구현. 실제 기능 설정은 Features::Settings에 보관하고 이 파일은 표시/상호작용만 담당한다.
namespace Widgets
{
    // ImGuiStyle 커스터마이징 (라운딩, 색상 팔레트). ImGui_ImplWin32_Init보다 먼저 호출되어야 폰트 로드
    // 등 이후 단계에 영향을 준다.
    void ApplyStyle();

    void DrawMainMenu();
    void DrawStartupHint();

    // pill 모양 토글 스위치. 스톡 위젯이 없어서 InvisibleButton으로 입력만 가져오고 배경/thumb은
    // ImDrawList로 직접 그린다. 반환값은 "이번 프레임에 눌렸는지"(값 자체는 *value에 반영됨).
    bool ToggleSwitch(const char* label, bool* value);

    // 값만큼 트랙이 채워지는 커스텀 float 슬라이더. 스톡 SliderFloat의 프레임은 사용하지 않는다.
    bool FilledSliderFloat(const char* label, float* value, float minimum, float maximum,
                           const char* format = "%.1f");
}
