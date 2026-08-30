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

        // 다크 팔레트. 면을 넷(바닥/사이드바/surface/hover)으로 줄이고, 파랑은 액센트 하나만
        // 남긴다 - 선택된 내비게이션, 토글, 포커스가 전부 같은 채도로 파랗던 것이 화면에
        // 우선순위가 없어 보이던 큰 이유였다. 이제 선택 표시는 옅은 배경 + 좌측 바가 맡고,
        // 진하게 채워지는 파랑은 켜진 토글과 슬라이더 채움에만 쓴다.
        const Palette kDark = {
            Rgba(11, 14, 19, 250),   // windowBackground
            Rgba(14, 18, 24),        // sidebarBackground
            Rgba(18, 23, 31),        // surface
            Rgba(23, 30, 40),        // surfaceHovered
            Rgba(36, 44, 56),        // border
            Rgba(232, 237, 245),     // text
            Rgba(141, 152, 168),     // textMuted
            Rgba(89, 98, 112),       // textSubtle
            Rgba(91, 141, 239),      // accent
            Rgba(124, 165, 244),     // accentHovered
            Rgba(29, 39, 61),        // accentSoft  (accent 12% 상당)
            Rgba(224, 163, 63),      // warning
            Rgba(40, 33, 20),        // warningSoft
            Rgba(94, 212, 154),      // success
            Rgba(242, 246, 252),     // toggleThumb
            Rgba(0, 0, 0, 110),      // shadow
        };

        // 라이트는 같은 역할을 뒤집어 매핑한다. 바닥을 서늘한 회색으로 내려서 흰 면이 실제로
        // 떠 보이게 하고, 액센트는 형광에 가까운 파랑에서 한 단계 눌렀다.
        const Palette kLight = {
            Rgba(244, 246, 249, 250), // windowBackground
            Rgba(237, 240, 245),      // sidebarBackground
            Rgba(255, 255, 255),      // surface
            Rgba(233, 238, 245),      // surfaceHovered
            Rgba(217, 223, 231),      // border
            Rgba(26, 33, 45),         // text
            Rgba(96, 107, 124),       // textMuted
            Rgba(141, 152, 168),      // textSubtle
            Rgba(59, 110, 224),       // accent
            Rgba(47, 95, 201),        // accentHovered
            Rgba(226, 235, 252),      // accentSoft
            Rgba(176, 115, 11),       // warning
            Rgba(253, 243, 222),      // warningSoft
            Rgba(24, 122, 74),        // success
            Rgba(255, 255, 255),      // toggleThumb
            Rgba(30, 45, 70, 24),     // shadow
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
        colors[ImGuiCol_ChildBg] = AsImVec4(IM_COL32(0, 0, 0, 0));
        colors[ImGuiCol_PopupBg] = AsImVec4(palette.surface);
        colors[ImGuiCol_Border] = AsImVec4(palette.border);
        colors[ImGuiCol_FrameBg] = AsImVec4(palette.surface);
        colors[ImGuiCol_FrameBgHovered] = AsImVec4(palette.surfaceHovered);
        colors[ImGuiCol_FrameBgActive] = AsImVec4(palette.accentSoft);
        colors[ImGuiCol_Button] = AsImVec4(palette.surface);
        colors[ImGuiCol_ButtonHovered] = AsImVec4(palette.surfaceHovered);
        colors[ImGuiCol_ButtonActive] = AsImVec4(palette.accentSoft);
        colors[ImGuiCol_Header] = AsImVec4(palette.surface);
        colors[ImGuiCol_HeaderHovered] = AsImVec4(palette.surfaceHovered);
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

        style.WindowRounding = 12.0f;
        style.ChildRounding = 0.0f;
        style.FrameRounding = 6.0f;
        style.GrabRounding = 8.0f;
        style.PopupRounding = 8.0f;
        style.ScrollbarRounding = 8.0f;
        style.WindowPadding = ImVec2(0.0f, 0.0f);
        style.ChildBorderSize = 0.0f;
        style.FrameBorderSize = 0.0f;
        style.WindowBorderSize = 0.0f;
        // 행 리듬은 위젯이 직접 만든다. 여기 세로 간격이 크면 섹션 안이 성기게 벌어진다.
        style.ItemSpacing = ImVec2(10.0f, 4.0f);
        style.ItemInnerSpacing = ImVec2(8.0f, 5.0f);
        style.ScrollbarSize = 10.0f;
    }
}
