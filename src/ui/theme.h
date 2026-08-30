#pragma once

#include <imgui.h>

// 시맨틱 테마 토큰. 값이 아니라 역할로 이름을 붙였고, 다크/라이트가 같은 토큰 집합에 서로 다른 값을
// 채운다 (라이트는 다크의 반전이 아니라 독립적으로 조율한 값이다).
//
// 오버레이 UI는 불투명하다. 게임 장면이 설정 화면을 통과해 비치면 대비가 매 프레임 달라져서 어떤
// 글자색도 안정적으로 읽히지 않는다. 반투명은 창 그림자에만 남긴다.
namespace UiTheme
{
    struct Palette
    {
        ImU32 background;      // 콘텐츠 영역 바닥
        ImU32 sidebar;         // 내비게이션 기둥
        ImU32 surface;         // 바닥 위로 올라온 면 (섹션 패널, 입력 필드)
        ImU32 surfaceHovered;
        ImU32 surfaceActive;
        ImU32 border;          // 패널 외곽선
        ImU32 borderSubtle;    // 행 사이 구분선
        ImU32 text;
        ImU32 textSecondary;
        ImU32 textDisabled;
        ImU32 accent;
        ImU32 accentHovered;
        ImU32 accentSoft;      // 선택된 내비게이션 배경 등, 액센트의 아주 옅은 면
        ImU32 warning;
        ImU32 warningSoft;
        ImU32 error;
        ImU32 success;
        ImU32 controlOff;      // 꺼진 토글 트랙 - border보다 한 단계 밝아야 "끔"과 "비활성"이 갈린다
        ImU32 knob;            // 토글 thumb / 슬라이더 grab
        ImU32 shadow;
    };

    const Palette& Current();
    bool IsLight();
    ImU32 WithAlpha(ImU32 color, int alpha);
    // 두 색을 t(0~1)로 섞는다. 비활성 상태를 별도 토큰 없이 만들 때 쓴다.
    ImU32 Mix(ImU32 from, ImU32 to, float t);
    void ApplyImGuiStyle();
}
