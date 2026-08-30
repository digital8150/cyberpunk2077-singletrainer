#pragma once

#include <imgui.h>

namespace UiTheme
{
    // 회색 단계를 네 개로 줄였다: 바닥(window) / 사이드바 / 올라온 면(surface) / 호버.
    // 예전 팔레트는 면이 일곱 개였고 그중 절반은 서로 3% 안에 모여 있어서, 눈은 구역을 구분하지
    // 못하면서 화면만 탁해졌다. 구역은 선이 아니라 명도 차와 여백으로 만든다.
    struct Palette
    {
        ImU32 windowBackground;
        ImU32 sidebarBackground;
        ImU32 surface;           // 바닥 위로 올라온 면 (토글 트랙, 드롭다운, 통계 패널)
        ImU32 surfaceHovered;
        ImU32 border;
        ImU32 text;
        ImU32 textMuted;
        ImU32 textSubtle;
        ImU32 accent;
        ImU32 accentHovered;
        ImU32 accentSoft;
        ImU32 warning;
        ImU32 warningSoft;
        ImU32 success;
        ImU32 toggleThumb;
        ImU32 shadow;
    };

    const Palette& Current();
    ImU32 WithAlpha(ImU32 color, int alpha);
    void ApplyImGuiStyle();
}
