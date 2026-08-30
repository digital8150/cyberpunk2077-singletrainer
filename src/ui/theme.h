#pragma once

#include <imgui.h>

namespace UiTheme
{
    struct Palette
    {
        ImU32 windowBackground;
        ImU32 sidebarBackground;
        ImU32 panelBackground;
        ImU32 cardBackground;
        ImU32 cardRaised;
        ImU32 controlBackground;
        ImU32 controlHovered;
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
        ImU32 toggleOff;
        ImU32 toggleOffHovered;
        ImU32 toggleThumb;
        ImU32 shadow;
    };

    const Palette& Current();
    ImU32 WithAlpha(ImU32 color, int alpha);
    void ApplyImGuiStyle();
}
