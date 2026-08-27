#include "widgets.h"
#include "../framework.h"  // GetFileAttributesW/INVALID_FILE_ATTRIBUTES

#include <imgui.h>
#include <imgui_internal.h>  // ImGuiWindow/ItemAdd/ButtonBehavior 등 저수준 위젯 제작용 API

namespace Widgets
{
    void ApplyStyle()
    {
        // 기본 ImGui 폰트엔 한글 글리프가 없어서 한글 텍스트가 "??"로 깨진다 (실사용 확인됨). Windows에
        // 기본 내장된 맑은 고딕을 얹는다 — 파일이 없는 환경이면 조용히 기본 폰트로만 폴백한다. 이
        // ImGui 버전(1.92+)은 DX12 백엔드가 ImGuiBackendFlags_RendererHasTextures를 지원해서 글리프를
        // 필요할 때마다 동적으로 래스터화하므로, 예전처럼 GetGlyphRangesKorean()으로 범위를 미리 지정할
        // 필요가 없다(그 함수 자체가 IMGUI_DISABLE_OBSOLETE_FUNCTIONS 하에서 더 이상 안 보이기도 함).
        // 진짜 커스텀 폰트(Inter/Pretendard 등)로 바꾸는 건 아래 TODO 그대로 남겨둠.
        ImGuiIO& io = ImGui::GetIO();
        if (GetFileAttributesW(L"C:\\Windows\\Fonts\\malgun.ttf") != INVALID_FILE_ATTRIBUTES)
        {
            ImFontConfig config;
            config.OversampleH = 2;
            config.OversampleV = 2;
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgun.ttf", 17.0f, &config);
        }

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

        // TODO: 지금은 한글 깨짐만 막으려고 맑은 고딕(위)을 얹은 상태 — 디자인용 커스텀 폰트
        // (Inter/Pretendard 등)로 바꾸는 건 아직. 바꿀 땐 위 AddFontFromFileTTF를 그 폰트로 교체.
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
