#include "widgets.h"

#include <imgui.h>
#include <imgui_internal.h>  // ImGuiWindow/ItemAdd/ButtonBehavior 등 저수준 위젯 제작용 API

namespace Widgets
{
    void ApplyStyle()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 10.0f;
        style.FrameRounding = 6.0f;
        style.GrabRounding = 12.0f;
        style.WindowPadding = ImVec2(16.0f, 16.0f);
        style.ItemSpacing = ImVec2(10.0f, 10.0f);
        style.WindowBorderSize = 0.0f;

        ImVec4* colors = style.Colors;
        colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.10f, 0.96f);
        colors[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.19f, 1.00f);
        colors[ImGuiCol_Button] = ImVec4(0.20f, 0.45f, 0.95f, 1.00f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.52f, 1.00f, 1.00f);
        colors[ImGuiCol_Text] = ImVec4(0.92f, 0.92f, 0.94f, 1.00f);
        colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.55f, 0.58f, 1.00f);

        // TODO: 기본 Proggy 폰트 대신 Inter/Pretendard 등 TTF를 io.Fonts->AddFontFromFileTTF(...)로 로드.
        // ImGui_ImplWin32_Init/ImGui_ImplDX12_Init 이전, 폰트 아틀라스가 아직 안 구워졌을 때 호출해야 함.
        // ImGui가 "투박해" 보이는 원인의 8할이 폰트다 (AGENTS.md 기술 스택 절 참고).
    }

    bool ToggleSwitch(const char* label, bool* value)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return false;

        ImGuiContext& g = *ImGui::GetCurrentContext();
        const ImGuiID id = window->GetID(label);

        const float height = ImGui::GetFrameHeight();
        const float width = height * 1.8f;
        const ImVec2 pos = window->DC.CursorPos;
        const ImRect bb(pos, ImVec2(pos.x + width, pos.y + height));

        ImGui::ItemSize(bb);
        if (!ImGui::ItemAdd(bb, id))
            return false;

        bool hovered = false;
        bool held = false;
        const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
        if (pressed)
            *value = !*value;

        // thumb 위치를 프레임마다 lerp해서 슬라이드 애니메이션을 만든다. ImGuiStorage에 마지막 애니메이션
        // 진행도를 들고 있는다 (AGENTS.md의 "토글 스위치(pill)" 구현 방식).
        const float target = *value ? 1.0f : 0.0f;
        ImGuiStorage* storage = window->DC.StateStorage;
        float animT = storage->GetFloat(id, target);
        animT += (target - animT) * ImMin(g.IO.DeltaTime * 12.0f, 1.0f);
        storage->SetFloat(id, animT);

        const ImU32 bgColor = ImGui::GetColorU32(*value ? ImVec4(0.20f, 0.55f, 0.95f, 1.0f)
                                                          : ImVec4(0.30f, 0.30f, 0.34f, 1.0f));

        ImDrawList* drawList = window->DrawList;
        drawList->AddRectFilled(bb.Min, bb.Max, bgColor, height * 0.5f);

        const float radius = height * 0.5f - 2.0f;
        const float thumbX = bb.Min.x + radius + 2.0f + animT * (width - height);
        drawList->AddCircleFilled(ImVec2(thumbX, bb.Min.y + height * 0.5f), radius, IM_COL32(255, 255, 255, 255));

        return pressed;
    }

    void DrawMainMenu()
    {
        ImGui::SetNextWindowSize(ImVec2(420.0f, 260.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Cyberpunk 2077 Trainer");

        ImGui::TextDisabled("SCAFFOLD - Insert로 토글");
        ImGui::Separator();

        static bool espEnabled = false;
        static bool aimbotEnabled = false;

        ImGui::TextUnformatted("ESP");
        ImGui::SameLine(200.0f);
        ToggleSwitch("##esp_toggle", &espEnabled);

        ImGui::TextUnformatted("Aimbot");
        ImGui::SameLine(200.0f);
        ToggleSwitch("##aimbot_toggle", &aimbotEnabled);

        ImGui::Separator();
        ImGui::TextWrapped(
            "ESP/Aimbot 실제 로직은 아직 없음 - 오프셋을 리버싱으로 확보한 뒤 채울 것 "
            "(AGENTS.md 기능 스펙 절 참고).");

        ImGui::End();
    }
}
