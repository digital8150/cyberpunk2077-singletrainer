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

        const Palette kDark = {
            Rgba(9, 12, 18, 248),    // windowBackground
            Rgba(13, 18, 28),        // sidebarBackground
            Rgba(16, 23, 34),        // panelBackground
            Rgba(22, 31, 45),        // cardBackground
            Rgba(29, 41, 57),         // cardRaised
            Rgba(26, 37, 51),         // controlBackground
            Rgba(36, 52, 74),         // controlHovered
            Rgba(44, 59, 78),         // border
            Rgba(235, 242, 250),      // text
            Rgba(141, 154, 172),      // textMuted
            Rgba(93, 107, 125),       // textSubtle
            Rgba(90, 162, 255),       // accent
            Rgba(120, 181, 255),      // accentHovered
            Rgba(34, 62, 96),         // accentSoft
            Rgba(240, 178, 77),       // warning
            Rgba(59, 45, 24),         // warningSoft
            Rgba(94, 212, 154),       // success
            Rgba(67, 80, 100),        // toggleOff
            Rgba(84, 101, 124),       // toggleOffHovered
            Rgba(247, 250, 255),      // toggleThumb
            Rgba(0, 0, 0, 92),        // shadow
        };

        const Palette kLight = {
            Rgba(242, 245, 249, 250), // windowBackground
            Rgba(232, 237, 244),       // sidebarBackground
            Rgba(248, 250, 252),       // panelBackground
            Rgba(255, 255, 255),       // cardBackground
            Rgba(242, 246, 251),       // cardRaised
            Rgba(232, 238, 245),       // controlBackground
            Rgba(220, 232, 245),       // controlHovered
            Rgba(202, 213, 226),       // border
            Rgba(24, 34, 49),          // text
            Rgba(83, 98, 116),         // textMuted
            Rgba(115, 130, 152),       // textSubtle
            Rgba(36, 104, 216),        // accent
            Rgba(29, 87, 186),         // accentHovered
            Rgba(215, 230, 255),       // accentSoft
            Rgba(154, 92, 0),          // warning
            Rgba(255, 241, 214),       // warningSoft
            Rgba(24, 122, 74),         // success
            Rgba(170, 182, 197),       // toggleOff
            Rgba(143, 158, 177),       // toggleOffHovered
            Rgba(255, 255, 255),       // toggleThumb
            Rgba(45, 61, 80, 40),      // shadow
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

        style.WindowRounding = 12.0f;
        style.ChildRounding = 10.0f;
        style.FrameRounding = 7.0f;
        style.GrabRounding = 12.0f;
        style.PopupRounding = 8.0f;
        style.ScrollbarRounding = 10.0f;
        style.WindowPadding = ImVec2(16.0f, 16.0f);
        style.ChildBorderSize = 0.0f;
        style.FrameBorderSize = 0.0f;
        style.WindowBorderSize = 0.0f;
        style.ItemSpacing = ImVec2(10.0f, 9.0f);
        style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
        style.ScrollbarSize = 12.0f;
    }
}
