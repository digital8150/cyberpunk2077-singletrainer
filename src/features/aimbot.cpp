#include "aimbot.h"
#include "features.h"

#include <imgui.h>

namespace Aimbot
{
    void DrawOverlay(const Features::AimbotSettings& settings)
    {
        if (!settings.enabled || !settings.drawFovCircle)
            return;

        const ImGuiIO& io = ImGui::GetIO();
        if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
            return;

        const ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();

        // 어두운 외곽선 뒤에 강조색을 겹쳐 어떤 배경에서도 FOV 경계가 읽히게 한다.
        drawList->AddCircle(center, settings.fovRadiusPixels, IM_COL32(0, 0, 0, 180), 128, 2.8f);
        drawList->AddCircle(center, settings.fovRadiusPixels, IM_COL32(62, 157, 255, 220), 128, 1.4f);
    }
}
