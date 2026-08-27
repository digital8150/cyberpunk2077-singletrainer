#include "fps_counter.h"

#include <imgui.h>

#include <cstdio>

namespace FpsCounter
{
    void Draw(bool enabled)
    {
        if (!enabled)
            return;

        const ImGuiIO& io = ImGui::GetIO();
        if (io.DisplaySize.x <= 0.0f || io.Framerate <= 0.0f)
            return;

        char text[32]{};
        snprintf(text, sizeof(text), "%.0f FPS", io.Framerate);
        const ImVec2 textSize = ImGui::CalcTextSize(text);
        constexpr float paddingX = 10.0f;
        constexpr float paddingY = 6.0f;
        const ImVec2 maximum(io.DisplaySize.x - 14.0f, 14.0f + textSize.y + paddingY * 2.0f);
        const ImVec2 minimum(maximum.x - textSize.x - paddingX * 2.0f, 14.0f);

        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        drawList->AddRectFilled(minimum, maximum, IM_COL32(18, 20, 26, 210), 7.0f);
        drawList->AddRect(minimum, maximum, IM_COL32(64, 151, 245, 170), 7.0f, 1.0f, ImDrawFlags_None);
        drawList->AddText(ImVec2(minimum.x + paddingX, minimum.y + paddingY), IM_COL32(235, 241, 248, 255), text);
    }
}
