#include "theme.h"

#include "../features/features.h"

#include <algorithm>

namespace UiTheme
{
    namespace
    {
        constexpr ImU32 Rgba(unsigned int red, unsigned int green, unsigned int blue, unsigned int alpha = 255)
        {
            return IM_COL32(red, green, blue, alpha);
        }

        // 표면은 값 차이로 구분된다. 예전 팔레트는 패널과 카드가 3% 차이라 화면에서 사실상 같은 면이었고,
        // 그래서 "카드 위에 얹힌 컨트롤"이라는 구조가 보이지 않았다. 아래는 바닥(window/panel) →
        // 사이드바 → 카드 순으로 확실히 벌린 값이다.
        const Palette kDark = {
            Rgba(13, 16, 23, 250),   // windowBackground  바닥
            Rgba(17, 21, 29),        // sidebarBackground
            Rgba(13, 16, 23),        // panelBackground   콘텐츠 바닥 = window와 같은 면
            Rgba(22, 27, 36),        // cardBackground    바닥 위로 떠 있는 면
            Rgba(28, 35, 48),        // cardRaised
            Rgba(26, 32, 43),        // controlBackground
            Rgba(35, 43, 58),        // controlHovered
            Rgba(38, 46, 60),        // border
            Rgba(230, 236, 245),     // text
            Rgba(141, 154, 173),     // textMuted
            Rgba(95, 108, 126),      // textSubtle
            Rgba(90, 141, 240),      // accent
            Rgba(120, 164, 247),     // accentHovered
            Rgba(30, 44, 70),        // accentSoft
            Rgba(224, 163, 63),      // warning
            Rgba(43, 36, 21),        // warningSoft
            Rgba(94, 212, 154),      // success
            Rgba(51, 60, 76),        // toggleOff
            Rgba(66, 77, 96),        // toggleOffHovered
            Rgba(242, 246, 252),     // toggleThumb
            Rgba(0, 0, 0, 110),      // shadow
        };

        // 라이트 테마는 순백 위에 순백을 얹지 않는다. 바닥을 서늘한 회색으로 내려서 흰 카드가 실제로
        // 떠 보이게 하고, 액센트도 형광에 가까운 파랑에서 한 단계 눌렀다.
        const Palette kLight = {
            Rgba(233, 237, 243, 250), // windowBackground  바닥
            Rgba(223, 229, 238),      // sidebarBackground
            Rgba(233, 237, 243),      // panelBackground   콘텐츠 바닥 = window와 같은 면
            Rgba(255, 255, 255),      // cardBackground    바닥 위의 흰 카드
            Rgba(247, 249, 252),      // cardRaised
            Rgba(237, 241, 247),      // controlBackground
            Rgba(225, 232, 243),      // controlHovered
            Rgba(211, 219, 230),      // border
            Rgba(27, 36, 50),         // text
            Rgba(90, 103, 121),       // textMuted
            Rgba(138, 151, 168),      // textSubtle
            Rgba(59, 110, 224),       // accent
            Rgba(47, 95, 201),        // accentHovered
            Rgba(220, 231, 251),      // accentSoft
            Rgba(176, 115, 11),       // warning
            Rgba(253, 243, 222),      // warningSoft
            Rgba(24, 122, 74),        // success
            Rgba(195, 204, 217),      // toggleOff
            Rgba(178, 189, 204),      // toggleOffHovered
            Rgba(255, 255, 255),      // toggleThumb
            Rgba(30, 45, 70, 26),     // shadow
        };

        ImVec4 AsImVec4(ImU32 color)
        {
            return ImGui::ColorConvertU32ToFloat4(color);
        }
    }

    const Palette& Current()
    {
        return Features::GetSettings().ui.theme == Features::Theme::Light ? kLight : kDark;
    }

    ImU32 WithAlpha(ImU32 color, int alpha)
    {
        const int clamped = (std::max)(0, (std::min)(255, alpha));
        return (color & 0x00FFFFFFu) | (static_cast<ImU32>(clamped) << IM_COL32_A_SHIFT);
    }

    void ApplyImGuiStyle()
    {
        const Palette& palette = Current();
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;

        colors[ImGuiCol_WindowBg] = AsImVec4(palette.windowBackground);
        colors[ImGuiCol_ChildBg] = AsImVec4(palette.panelBackground);
        colors[ImGuiCol_PopupBg] = AsImVec4(palette.cardBackground);
        colors[ImGuiCol_Border] = AsImVec4(palette.border);
        colors[ImGuiCol_FrameBg] = AsImVec4(palette.controlBackground);
        colors[ImGuiCol_FrameBgHovered] = AsImVec4(palette.controlHovered);
        colors[ImGuiCol_FrameBgActive] = AsImVec4(palette.accentSoft);
        colors[ImGuiCol_Button] = AsImVec4(palette.controlBackground);
        colors[ImGuiCol_ButtonHovered] = AsImVec4(palette.controlHovered);
        colors[ImGuiCol_ButtonActive] = AsImVec4(palette.accentSoft);
        colors[ImGuiCol_Header] = AsImVec4(palette.controlBackground);
        colors[ImGuiCol_HeaderHovered] = AsImVec4(palette.controlHovered);
        colors[ImGuiCol_HeaderActive] = AsImVec4(palette.accentSoft);
        colors[ImGuiCol_SliderGrab] = AsImVec4(palette.accent);
        colors[ImGuiCol_SliderGrabActive] = AsImVec4(palette.accentHovered);
        colors[ImGuiCol_Text] = AsImVec4(palette.text);
        colors[ImGuiCol_TextDisabled] = AsImVec4(palette.textMuted);

        colors[ImGuiCol_ScrollbarBg] = AsImVec4(IM_COL32(0, 0, 0, 0));
        colors[ImGuiCol_ScrollbarGrab] = AsImVec4(WithAlpha(palette.textSubtle, 70));
        colors[ImGuiCol_ScrollbarGrabHovered] = AsImVec4(WithAlpha(palette.textSubtle, 120));
        colors[ImGuiCol_ScrollbarGrabActive] = AsImVec4(WithAlpha(palette.textSubtle, 160));
        colors[ImGuiCol_Separator] = AsImVec4(palette.border);

        style.WindowRounding = 14.0f;
        style.ChildRounding = 12.0f;
        style.FrameRounding = 8.0f;
        style.GrabRounding = 10.0f;
        style.PopupRounding = 10.0f;
        style.ScrollbarRounding = 8.0f;
        style.WindowPadding = ImVec2(18.0f, 18.0f);
        style.ChildBorderSize = 0.0f;
        style.FrameBorderSize = 0.0f;
        style.WindowBorderSize = 0.0f;
        // 행 리듬은 위젯이 직접 만든다. 여기 세로 간격이 크면 카드 안이 성기게 벌어진다.
        style.ItemSpacing = ImVec2(10.0f, 4.0f);
        style.ItemInnerSpacing = ImVec2(8.0f, 5.0f);
        style.ScrollbarSize = 10.0f;
    }
}
