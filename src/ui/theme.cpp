#include "theme.h"

#include "../features/features.h"

#include <algorithm>

namespace UiTheme
{
    namespace
    {
        constexpr ImU32 Rgb(unsigned int red, unsigned int green, unsigned int blue, unsigned int alpha = 255)
        {
            return IM_COL32(red, green, blue, alpha);
        }

        // 다크가 1차 설계 대상이다. 회색은 네 단계(바닥/사이드바/면/호버)에 외곽선 둘로 끊는다.
        // 파랑은 액센트 하나뿐이고, 켜진 컨트롤과 선택된 내비게이션에만 쓴다.
        const Palette kDark = {
            Rgb(11, 13, 16),      // background
            Rgb(14, 17, 22),      // sidebar
            Rgb(18, 22, 28),      // surface
            Rgb(23, 28, 35),      // surfaceHovered
            Rgb(28, 34, 43),      // surfaceActive
            Rgb(36, 42, 51),      // border
            Rgb(27, 32, 40),      // borderSubtle
            Rgb(231, 234, 240),   // text
            Rgb(137, 147, 161),   // textSecondary
            Rgb(81, 89, 102),     // textDisabled
            Rgb(91, 141, 239),    // accent
            Rgb(124, 165, 244),   // accentHovered
            Rgb(26, 34, 51),      // accentSoft
            Rgb(224, 163, 63),    // warning
            Rgb(42, 34, 22),      // warningSoft
            Rgb(226, 87, 76),     // error
            Rgb(94, 212, 154),    // success
            Rgb(56, 64, 76),      // controlOff
            Rgb(238, 242, 248),   // knob
            Rgb(0, 0, 0, 120),    // shadow
        };

        // 라이트는 다크를 뒤집은 값이 아니다. 바닥을 서늘한 회색으로 내려서 흰 면이 실제로 떠 보이게
        // 하고, 액센트/경고는 흰 배경에서 대비를 맞추기 위해 각각 한 단계 어둡게 다시 골랐다.
        const Palette kLight = {
            Rgb(242, 244, 247),   // background
            Rgb(233, 236, 241),   // sidebar
            Rgb(255, 255, 255),   // surface
            Rgb(241, 244, 248),   // surfaceHovered
            Rgb(231, 236, 243),   // surfaceActive
            Rgb(215, 221, 229),   // border
            Rgb(230, 234, 240),   // borderSubtle
            Rgb(23, 28, 36),      // text
            Rgb(92, 102, 117),    // textSecondary
            Rgb(154, 163, 176),   // textDisabled
            Rgb(47, 107, 224),    // accent
            Rgb(36, 91, 200),     // accentHovered
            Rgb(228, 236, 251),   // accentSoft
            Rgb(176, 115, 11),    // warning
            Rgb(253, 243, 222),   // warningSoft
            Rgb(195, 58, 47),     // error
            Rgb(23, 121, 74),     // success
            Rgb(198, 205, 214),   // controlOff
            Rgb(255, 255, 255),   // knob
            Rgb(30, 45, 70, 30),  // shadow
        };

        ImVec4 AsImVec4(ImU32 color)
        {
            return ImGui::ColorConvertU32ToFloat4(color);
        }
    }

    bool IsLight()
    {
        return Features::GetSettings().ui.theme == Features::Theme::Light;
    }

    const Palette& Current()
    {
        return IsLight() ? kLight : kDark;
    }

    ImU32 WithAlpha(ImU32 color, int alpha)
    {
        const int clamped = (std::max)(0, (std::min)(255, alpha));
        return (color & 0x00FFFFFFu) | (static_cast<ImU32>(clamped) << IM_COL32_A_SHIFT);
    }

    ImU32 Mix(ImU32 from, ImU32 to, float t)
    {
        const float amount = std::clamp(t, 0.0f, 1.0f);
        const auto channel = [&](int shift) {
            const float a = static_cast<float>((from >> shift) & 0xFFu);
            const float b = static_cast<float>((to >> shift) & 0xFFu);
            return static_cast<ImU32>(a + (b - a) * amount + 0.5f) & 0xFFu;
        };
        return (channel(IM_COL32_R_SHIFT) << IM_COL32_R_SHIFT) |
               (channel(IM_COL32_G_SHIFT) << IM_COL32_G_SHIFT) |
               (channel(IM_COL32_B_SHIFT) << IM_COL32_B_SHIFT) |
               (channel(IM_COL32_A_SHIFT) << IM_COL32_A_SHIFT);
    }

    // ImGui 기본 위젯은 팝업(드롭다운)과 스크롤바에만 쓴다. 나머지 컨트롤은 UiKit이 직접 그리므로
    // 여기서는 그 두 가지와 전역 메트릭만 맞춘다.
    void ApplyImGuiStyle()
    {
        const Palette& palette = Current();
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;

        colors[ImGuiCol_WindowBg] = AsImVec4(palette.background);
        colors[ImGuiCol_ChildBg] = AsImVec4(IM_COL32(0, 0, 0, 0));
        colors[ImGuiCol_PopupBg] = AsImVec4(palette.surface);
        colors[ImGuiCol_Border] = AsImVec4(palette.border);
        colors[ImGuiCol_BorderShadow] = AsImVec4(IM_COL32(0, 0, 0, 0));
        colors[ImGuiCol_FrameBg] = AsImVec4(palette.surface);
        colors[ImGuiCol_FrameBgHovered] = AsImVec4(palette.surfaceHovered);
        colors[ImGuiCol_FrameBgActive] = AsImVec4(palette.surfaceActive);
        colors[ImGuiCol_Header] = AsImVec4(palette.accentSoft);
        colors[ImGuiCol_HeaderHovered] = AsImVec4(palette.surfaceHovered);
        colors[ImGuiCol_HeaderActive] = AsImVec4(palette.surfaceActive);
        colors[ImGuiCol_Text] = AsImVec4(palette.text);
        colors[ImGuiCol_TextDisabled] = AsImVec4(palette.textDisabled);
        colors[ImGuiCol_ScrollbarBg] = AsImVec4(IM_COL32(0, 0, 0, 0));
        colors[ImGuiCol_ScrollbarGrab] = AsImVec4(WithAlpha(palette.textDisabled, 90));
        colors[ImGuiCol_ScrollbarGrabHovered] = AsImVec4(WithAlpha(palette.textDisabled, 140));
        colors[ImGuiCol_ScrollbarGrabActive] = AsImVec4(WithAlpha(palette.textDisabled, 190));
        colors[ImGuiCol_Separator] = AsImVec4(palette.borderSubtle);
        colors[ImGuiCol_NavCursor] = AsImVec4(WithAlpha(palette.accent, 160));

        style.WindowRounding = 8.0f;
        style.ChildRounding = 0.0f;
        style.FrameRounding = 5.0f;
        style.GrabRounding = 5.0f;
        style.PopupRounding = 6.0f;
        style.ScrollbarRounding = 6.0f;
        style.WindowBorderSize = 0.0f;
        style.ChildBorderSize = 0.0f;
        style.FrameBorderSize = 0.0f;
        style.PopupBorderSize = 1.0f;
        style.WindowPadding = ImVec2(0.0f, 0.0f);
        style.FramePadding = ImVec2(8.0f, 4.0f);
        style.ItemSpacing = ImVec2(8.0f, 4.0f);
        style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
        style.ScrollbarSize = 9.0f;
        style.WindowMinSize = ImVec2(64.0f, 32.0f);
    }
}
