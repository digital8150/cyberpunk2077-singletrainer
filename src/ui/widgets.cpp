#include "widgets.h"
#include "../framework.h"  // GetFileAttributesW/INVALID_FILE_ATTRIBUTES
#include "../features/features.h"
#include "../game/entity_tracker.h"

#include <imgui.h>
#include <imgui_internal.h>  // ImGuiWindow/ItemAdd/ButtonBehavior 등 저수준 위젯 제작용 API

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Widgets
{
    namespace
    {
        float EaseInOutCubic(float value)
        {
            const float t = std::clamp(value, 0.0f, 1.0f);
            return t < 0.5f ? 4.0f * t * t * t
                            : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
        }
    }

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

    bool FilledSliderFloat(const char* label, float* value, float minimum, float maximum, const char* format)
    {
        if (!value || maximum <= minimum)
            return false;

        ImGui::PushID(label);

        char valueText[64]{};
        snprintf(valueText, sizeof(valueText), format, *value);
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        const float valueWidth = ImGui::CalcTextSize(valueText).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - valueWidth);
        ImGui::TextDisabled("%s", valueText);

        const float height = 16.0f;
        const float width = (std::max)(160.0f, ImGui::GetContentRegionAvail().x);
        ImGui::InvisibleButton("##track", ImVec2(width, height));

        bool changed = false;
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            const float normalized = std::clamp((ImGui::GetIO().MousePos.x - min.x) / (max.x - min.x), 0.0f, 1.0f);
            const float newValue = minimum + normalized * (maximum - minimum);
            changed = newValue != *value;
            *value = newValue;
        }

        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const float normalized = std::clamp((*value - minimum) / (maximum - minimum), 0.0f, 1.0f);
        const float radius = height * 0.5f;
        const float grabX = min.x + normalized * (max.x - min.x);

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(min, max, IM_COL32(48, 49, 57, 255), radius);
        if (grabX > min.x)
            drawList->AddRectFilled(min, ImVec2(grabX, max.y), IM_COL32(51, 140, 242, 255), radius);
        drawList->AddCircleFilled(ImVec2(grabX, min.y + radius), radius - 2.0f, IM_COL32(238, 243, 250, 255));

        ImGui::PopID();
        return changed;
    }

    void DrawStartupHint()
    {
        constexpr float slideDuration = 0.55f;
        constexpr float holdDuration = 3.0f;
        constexpr float panelWidth = 560.0f;
        constexpr float panelHeight = 78.0f;
        constexpr float restingY = 24.0f;

        static ULONGLONG startedAt = 0;
        if (startedAt == 0)
            startedAt = GetTickCount64();

        const float elapsed = static_cast<float>(GetTickCount64() - startedAt) / 1000.0f;
        const float totalDuration = slideDuration + holdDuration + slideDuration;
        if (elapsed >= totalDuration)
            return;

        float progress = 1.0f;
        if (elapsed < slideDuration)
            progress = EaseInOutCubic(elapsed / slideDuration);
        else if (elapsed > slideDuration + holdDuration)
            progress = 1.0f - EaseInOutCubic((elapsed - slideDuration - holdDuration) / slideDuration);

        const ImGuiIO& io = ImGui::GetIO();
        if (io.DisplaySize.x <= panelWidth)
            return;

        const float hiddenY = -panelHeight - 18.0f;
        const float y = hiddenY + (restingY - hiddenY) * progress;
        const float x = (io.DisplaySize.x - panelWidth) * 0.5f;
        const ImVec2 minimum(x, y);
        const ImVec2 maximum(x + panelWidth, y + panelHeight);
        ImDrawList* drawList = ImGui::GetForegroundDrawList();

        const int alpha = static_cast<int>(255.0f * progress);
        drawList->AddRectFilled(ImVec2(minimum.x + 3.0f, minimum.y + 8.0f),
                                ImVec2(maximum.x + 3.0f, maximum.y + 8.0f),
                                IM_COL32(0, 0, 0, static_cast<int>(90.0f * progress)), 14.0f);
        drawList->AddRectFilled(minimum, maximum, IM_COL32(20, 22, 29, alpha), 14.0f);
        drawList->AddRect(minimum, maximum, IM_COL32(76, 137, 244, static_cast<int>(150.0f * progress)),
                          14.0f, 1.0f, ImDrawFlags_None);
        drawList->AddRectFilled(ImVec2(minimum.x, minimum.y + 16.0f),
                                ImVec2(minimum.x + 4.0f, maximum.y - 16.0f),
                                IM_COL32(69, 146, 255, alpha), 2.0f);

        const ImVec2 titlePos(minimum.x + 24.0f, minimum.y + 14.0f);
        drawList->AddText(titlePos, IM_COL32(116, 168, 255, alpha), "CBPK  /  TRAINER READY");

        drawList->AddText(ImVec2(minimum.x + 24.0f, minimum.y + 45.0f),
                          IM_COL32(226, 231, 241, alpha), "메뉴를 열려면");
        const ImVec2 keyMinimum(minimum.x + 142.0f, minimum.y + 41.0f);
        const ImVec2 keyMaximum(keyMinimum.x + 74.0f, keyMinimum.y + 25.0f);
        drawList->AddRectFilled(keyMinimum, keyMaximum, IM_COL32(44, 49, 61, alpha), 6.0f);
        drawList->AddRect(keyMinimum, keyMaximum, IM_COL32(102, 115, 139, alpha), 6.0f);
        const ImVec2 keyTextSize = ImGui::CalcTextSize("INSERT");
        drawList->AddText(ImVec2(keyMinimum.x + (keyMaximum.x - keyMinimum.x - keyTextSize.x) * 0.5f,
                                 keyMinimum.y + (keyMaximum.y - keyMinimum.y - keyTextSize.y) * 0.5f),
                          IM_COL32(240, 244, 252, alpha), "INSERT");
        drawList->AddText(ImVec2(keyMaximum.x + 13.0f, keyMinimum.y + 3.0f),
                          IM_COL32(226, 231, 241, alpha), "버튼을 누르세요");
    }

    void DrawMainMenu()
    {
        ImGui::SetNextWindowSize(ImVec2(520.0f, 780.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(520.0f, 740.0f), ImVec2(900.0f, 1000.0f));
        ImGui::Begin("Cyberpunk 2077 Trainer");

        Features::Settings& settings = Features::GetSettings();
        const Game::EntityTracker::Stats entityStats = Game::EntityTracker::GetStats();

        ImGui::TextDisabled("Cyberpunk 2077 2.31  |  Insert: menu  |  End: unload");
        ImGui::Separator();

        ImGui::TextUnformatted("Show FPS");
        ImGui::SameLine(430.0f);
        ToggleSwitch("##show_fps", &settings.showFps);
        ImGui::Separator();

        ImGui::TextUnformatted("ESP");
        ImGui::SameLine(430.0f);
        ToggleSwitch("##esp_toggle", &settings.esp.enabled);

        ImGui::Indent(14.0f);
        ImGui::TextUnformatted("Bounding boxes");
        ImGui::SameLine(430.0f);
        ToggleSwitch("##esp_boxes", &settings.esp.boundingBoxes);
        ImGui::TextUnformatted("Skeleton");
        ImGui::SameLine(430.0f);
        ToggleSwitch("##esp_skeleton", &settings.esp.skeleton);
        ImGui::TextUnformatted("Health bars");
        ImGui::SameLine(430.0f);
        ToggleSwitch("##esp_health", &settings.esp.healthBars);
        ImGui::TextUnformatted("Native highlight");
        ImGui::SameLine(430.0f);
        ToggleSwitch("##esp_native", &settings.esp.nativeHighlight);
        ImGui::TextUnformatted("Hide dead NPCs");
        ImGui::SameLine(430.0f);
        ToggleSwitch("##esp_hide_dead", &settings.esp.hideDead);
        ImGui::Spacing();
        ImGui::TextUnformatted("Civilians");
        ImGui::SameLine(430.0f);
        ToggleSwitch("##esp_civilians", &settings.esp.showCivilians);
        ImGui::TextUnformatted("Enemies");
        ImGui::SameLine(430.0f);
        ToggleSwitch("##esp_enemies", &settings.esp.showEnemies);
        ImGui::TextUnformatted("Police");
        ImGui::SameLine(430.0f);
        ToggleSwitch("##esp_police", &settings.esp.showPolice);
        ImGui::TextUnformatted("Unclassified");
        ImGui::SameLine(430.0f);
        ToggleSwitch("##esp_unclassified", &settings.esp.showUnclassified);
        FilledSliderFloat("Max distance", &settings.esp.maxDistanceMeters, 10.0f, 300.0f, "%.0f m");
        ImGui::Unindent(14.0f);

        ImGui::TextDisabled("Entity feed: %s | registered %llu | positioned %llu | NPCs %llu | live %llu",
                            entityStats.hookCreated ? "hooked" : "unavailable",
                            static_cast<unsigned long long>(entityStats.registered),
                            static_cast<unsigned long long>(entityStats.positioned),
                            static_cast<unsigned long long>(entityStats.puppets),
                            static_cast<unsigned long long>(entityStats.trackedPuppets));
        ImGui::TextDisabled("Classified live: civilian %llu | enemy %llu | police %llu",
                            static_cast<unsigned long long>(entityStats.trackedCivilians),
                            static_cast<unsigned long long>(entityStats.trackedEnemies),
                            static_cast<unsigned long long>(entityStats.trackedPolice));

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Aimbot");
        ImGui::SameLine(430.0f);
        ToggleSwitch("##aimbot_toggle", &settings.aimbot.enabled);

        ImGui::Indent(14.0f);
        ImGui::TextUnformatted("Draw FOV circle");
        ImGui::SameLine(430.0f);
        ToggleSwitch("##aimbot_fov_circle", &settings.aimbot.drawFovCircle);
        ImGui::TextUnformatted("Target enemies");
        ImGui::SameLine(430.0f);
        ToggleSwitch("##aimbot_target_enemies", &settings.aimbot.targetEnemies);
        ImGui::TextUnformatted("Target police");
        ImGui::SameLine(430.0f);
        ToggleSwitch("##aimbot_target_police", &settings.aimbot.targetPolice);
        FilledSliderFloat("FOV radius", &settings.aimbot.fovRadiusPixels, 40.0f, 600.0f, "%.0f px");
        FilledSliderFloat("Smoothing", &settings.aimbot.smoothing, 0.0f, 30.0f, "%.1f");
        FilledSliderFloat("Aim distance", &settings.aimbot.maxDistanceMeters, 10.0f, 300.0f, "%.0f m");
        ImGui::TextDisabled("Hold right mouse to use native aim offset. Smoothing 0 is hard lock.");
        ImGui::Unindent(14.0f);

        ImGui::Separator();
        ImGui::TextWrapped(
            "ESP classifies ScriptedPuppet reaction presets as civilian, enemy (ganger), or police. "
            "Distance uses camera-forward depth. Animation-system AABB and live SlotComponent pose points are "
            "used when available; health values and dynamic hostility are still pending.");

        ImGui::End();
    }
}
