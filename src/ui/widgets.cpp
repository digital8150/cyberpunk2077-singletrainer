#include "widgets.h"

#include "localization.h"
#include "theme.h"
#include "../framework.h"  // GetFileAttributesW/INVALID_FILE_ATTRIBUTES
#include "../features/aimbot.h"
#include "../features/features.h"
#include "../game/entity_tracker.h"
#include "../game/player_modifiers.h"
#include "../game/silent_aim.h"
#include "../game/visibility.h"

#include <imgui.h>
#include <imgui_internal.h>  // ImGuiWindow/ItemAdd/ButtonBehavior 등 저수준 위젯 제작용 API

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Widgets
{
    namespace
    {
        enum class Tab
        {
            Aimbot,
            Esp,
            Misc,
            Debug,
        };

        Tab g_activeTab = Tab::Aimbot;
        bool g_silentDiagnosticsOpen = false;

        ImVec4 Color(ImU32 value)
        {
            return ImGui::ColorConvertU32ToFloat4(value);
        }

        float EaseInOutCubic(float value)
        {
            const float t = std::clamp(value, 0.0f, 1.0f);
            return t < 0.5f ? 4.0f * t * t * t
                            : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
        }

        // GetKeyNameTextA는 현재 키보드 레이아웃의 ANSI 코드페이지 문자열을 돌려주므로 사용하지 않는다.
        // 키 이름도 일반 UI 문자열 테이블을 거쳐 UTF-8로 반환한다.
        const char* KeyName(unsigned int key)
        {
            static char text[32]{};
            switch (key)
            {
            case VK_LBUTTON: return Loc::Text(Loc::Str::KeyMouseLeft);
            case VK_RBUTTON: return Loc::Text(Loc::Str::KeyMouseRight);
            case VK_MBUTTON: return Loc::Text(Loc::Str::KeyMouseMiddle);
            case VK_XBUTTON1: return Loc::Text(Loc::Str::KeyMouseFour);
            case VK_XBUTTON2: return Loc::Text(Loc::Str::KeyMouseFive);
            case VK_BACK: return Loc::Text(Loc::Str::KeyBackspace);
            case VK_TAB: return Loc::Text(Loc::Str::KeyTab);
            case VK_RETURN: return Loc::Text(Loc::Str::KeyEnter);
            case VK_SHIFT: case VK_LSHIFT: return Loc::Text(Loc::Str::KeyShift);
            case VK_RSHIFT: return Loc::Text(Loc::Str::KeyRightShift);
            case VK_CONTROL: case VK_LCONTROL: return Loc::Text(Loc::Str::KeyControl);
            case VK_RCONTROL: return Loc::Text(Loc::Str::KeyRightControl);
            case VK_MENU: case VK_LMENU: return Loc::Text(Loc::Str::KeyAlt);
            case VK_RMENU: return Loc::Text(Loc::Str::KeyRightAlt);
            case VK_CAPITAL: return Loc::Text(Loc::Str::KeyCapsLock);
            case VK_SPACE: return Loc::Text(Loc::Str::KeySpace);
            case VK_PRIOR: return Loc::Text(Loc::Str::KeyPageUp);
            case VK_NEXT: return Loc::Text(Loc::Str::KeyPageDown);
            case VK_HOME: return Loc::Text(Loc::Str::KeyHome);
            case VK_LEFT: return Loc::Text(Loc::Str::KeyLeft);
            case VK_UP: return Loc::Text(Loc::Str::KeyUp);
            case VK_RIGHT: return Loc::Text(Loc::Str::KeyRight);
            case VK_DOWN: return Loc::Text(Loc::Str::KeyDown);
            case VK_DELETE: return Loc::Text(Loc::Str::KeyDelete);
            default: break;
            }
            if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z'))
            {
                text[0] = static_cast<char>(key);
                text[1] = '\0';
                return text;
            }
            if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
            {
                snprintf(text, sizeof(text), Loc::Text(Loc::Str::KeyNumpadFormat), key - VK_NUMPAD0);
                return text;
            }
            if (key >= VK_F1 && key <= VK_F24)
            {
                snprintf(text, sizeof(text), Loc::Text(Loc::Str::KeyFunctionFormat), key - VK_F1 + 1);
                return text;
            }
            snprintf(text, sizeof(text), Loc::Text(Loc::Str::KeyHexFormat), key);
            return text;
        }

        // 키 바인딩 버튼. 버튼을 누른 그 클릭이 그대로 바인딩되지 않도록 모든 키가 한 번 떼어진 뒤부터
        // 입력을 받는다. Escape는 취소, Insert(메뉴)와 End(언로드)는 트레이너가 이미 쓰는 키라 제외한다.
        bool KeyBindButton(unsigned int* key)
        {
            static bool capturing = false;
            static bool armed = false;

            const char* label = capturing ? (armed ? Loc::Text(Loc::Str::PressKey)
                                                    : Loc::Text(Loc::Str::ReleaseKeys))
                                          : KeyName(*key);
            const ImVec2 size(156.0f, ImGui::GetFrameHeight() + 2.0f);
            ImGui::PushID("aimbot_activation_key");
            const bool pressed = ImGui::InvisibleButton("##bind_button", size);
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 maximum = ImGui::GetItemRectMax();
            const UiTheme::Palette& palette = UiTheme::Current();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(minimum, maximum,
                                    hovered ? palette.controlHovered : palette.controlBackground, 7.0f);
            drawList->AddRect(minimum, maximum, hovered ? palette.accent : palette.border, 7.0f);
            const ImVec2 textSize = ImGui::CalcTextSize(label);
            drawList->AddText(ImVec2(minimum.x + (maximum.x - minimum.x - textSize.x) * 0.5f,
                                     minimum.y + (maximum.y - minimum.y - textSize.y) * 0.5f),
                              palette.text, label);
            ImGui::PopID();

            if (pressed && !capturing)
            {
                capturing = true;
                armed = false;
            }
            if (!capturing)
                return false;

            bool changed = false;
            bool anyDown = false;
            for (int virtualKey = 0x01; virtualKey <= 0xFE; ++virtualKey)
            {
                if (virtualKey == VK_INSERT || virtualKey == VK_END)
                    continue;
                if ((GetAsyncKeyState(virtualKey) & 0x8000) == 0)
                    continue;
                anyDown = true;
                if (!armed)
                    break;
                if (virtualKey != VK_ESCAPE)
                {
                    *key = static_cast<unsigned int>(virtualKey);
                    changed = true;
                }
                capturing = false;
                break;
            }
            if (!anyDown)
                armed = true;
            return changed;
        }

        void HintBox(const char* text)
        {
            if (!text)
                return;

            const UiTheme::Palette& palette = UiTheme::Current();
            const float width = (std::max)(1.0f, ImGui::GetContentRegionAvail().x);
            constexpr float leftPadding = 44.0f;
            constexpr float rightPadding = 18.0f;
            constexpr float verticalPadding = 13.0f;
            const float textWidth = (std::max)(1.0f, width - leftPadding - rightPadding);
            const float textHeight = ImGui::CalcTextSize(text, nullptr, false, textWidth).y;
            const float height = (std::max)(46.0f, textHeight + verticalPadding * 2.0f);
            const ImVec2 minimum = ImGui::GetCursorScreenPos();
            const ImVec2 maximum(minimum.x + width, minimum.y + height);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(minimum, maximum, palette.warningSoft, 8.0f);
            drawList->AddRect(minimum, maximum, UiTheme::WithAlpha(palette.warning, 130), 8.0f);
            drawList->AddRectFilled(minimum, ImVec2(minimum.x + 4.0f, maximum.y), palette.warning, 8.0f);
            drawList->AddCircleFilled(ImVec2(minimum.x + 22.0f, minimum.y + height * 0.5f), 10.0f,
                                      UiTheme::WithAlpha(palette.warning, 42));
            const char* icon = "!";
            const ImVec2 iconSize = ImGui::CalcTextSize(icon);
            drawList->AddText(ImVec2(minimum.x + 22.0f - iconSize.x * 0.5f,
                                     minimum.y + height * 0.5f - iconSize.y * 0.5f),
                              palette.warning, icon);
            drawList->AddText(nullptr, 0.0f, ImVec2(minimum.x + leftPadding, minimum.y + verticalPadding),
                              palette.text, text, nullptr, textWidth);
            ImGui::Dummy(ImVec2(width, height));
        }

        void SectionSeparator()
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }

        void ToggleRow(Loc::Str label, const char* id, bool* value)
        {
            ImGui::PushID(id);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(Loc::Text(label));
            ImGui::SameLine();
            const float toggleWidth = ImGui::GetFrameHeight() * 1.8f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - toggleWidth);
            ToggleSwitch("##switch", value);
            ImGui::PopID();
        }

        void KeyRow(Loc::Str label, unsigned int* key)
        {
            ImGui::PushID("activation_key_row");
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(Loc::Text(label));
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 156.0f);
            KeyBindButton(key);
            ImGui::PopID();
        }

        bool DisclosureHeader(const char* id, Loc::Str label, bool* open)
        {
            const float width = (std::max)(1.0f, ImGui::GetContentRegionAvail().x);
            const ImVec2 size(width, 34.0f);
            ImGui::PushID(id);
            const bool pressed = ImGui::InvisibleButton("##disclosure", size);
            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 maximum = ImGui::GetItemRectMax();
            const UiTheme::Palette& palette = UiTheme::Current();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            if (ImGui::IsItemHovered())
                drawList->AddRectFilled(minimum, maximum, palette.controlHovered, 7.0f);
            const float centerY = minimum.y + size.y * 0.5f;
            if (*open)
            {
                drawList->AddTriangleFilled(ImVec2(minimum.x + 9.0f, centerY - 2.0f),
                                            ImVec2(minimum.x + 19.0f, centerY - 2.0f),
                                            ImVec2(minimum.x + 14.0f, centerY + 4.0f), palette.accent);
            }
            else
            {
                drawList->AddTriangleFilled(ImVec2(minimum.x + 11.0f, centerY - 5.0f),
                                            ImVec2(minimum.x + 17.0f, centerY),
                                            ImVec2(minimum.x + 11.0f, centerY + 5.0f), palette.textMuted);
            }
            drawList->AddText(ImVec2(minimum.x + 28.0f, minimum.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f),
                              *open ? palette.accent : palette.text, Loc::Text(label));
            ImGui::PopID();
            if (pressed)
                *open = !*open;
            return *open;
        }

        bool SidebarTab(const char* id, Loc::Str label, Tab tab)
        {
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            const float width = (std::max)(1.0f, ImGui::GetContentRegionAvail().x);
            const ImVec2 size(width, 50.0f);
            ImGui::PushID(id);
            const bool pressed = ImGui::InvisibleButton("##tab", size);
            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 maximum = ImGui::GetItemRectMax();
            const bool selected = g_activeTab == tab;
            const bool hovered = ImGui::IsItemHovered();
            const UiTheme::Palette& palette = UiTheme::Current();
            if (selected)
            {
                window->DrawList->AddRectFilled(minimum, maximum, palette.accentSoft, 8.0f);
                window->DrawList->AddRectFilled(ImVec2(minimum.x, minimum.y + 7.0f),
                                                ImVec2(minimum.x + 3.0f, maximum.y - 7.0f), palette.accent, 2.0f);
            }
            else if (hovered)
            {
                window->DrawList->AddRectFilled(minimum, maximum, palette.controlHovered, 8.0f);
            }
            const ImVec2 dotCenter(minimum.x + 19.0f, minimum.y + size.y * 0.5f);
            window->DrawList->AddCircleFilled(dotCenter, selected ? 5.0f : 4.0f,
                                              selected ? palette.accent : palette.textSubtle);
            window->DrawList->AddText(ImVec2(minimum.x + 36.0f,
                                             minimum.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f),
                                      selected ? palette.text : palette.textMuted, Loc::Text(label));
            ImGui::PopID();
            if (pressed)
            {
                g_activeTab = tab;
                return true;
            }
            return false;
        }

        void DrawPopupOption(const char* id, const char* label, bool selected, bool* changed)
        {
            ImGui::PushID(id);
            if (ImGui::Selectable(label, selected, 0, ImVec2(160.0f, 32.0f)))
                *changed = true;
            ImGui::PopID();
        }

        void DrawLanguagePicker(Features::Settings& settings)
        {
            const UiTheme::Palette& palette = UiTheme::Current();
            const float width = (std::max)(1.0f, ImGui::GetContentRegionAvail().x);
            const ImVec2 size(width, 38.0f);
            ImGui::PushID("language_picker");
            const bool pressed = ImGui::InvisibleButton("##language_button", size);
            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 maximum = ImGui::GetItemRectMax();
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(minimum, maximum, hovered ? palette.controlHovered : palette.controlBackground, 7.0f);
            drawList->AddRect(minimum, maximum, hovered ? palette.accent : palette.border, 7.0f);
            drawList->AddText(ImVec2(minimum.x + 12.0f, minimum.y + 10.0f), palette.textMuted,
                              Loc::Text(Loc::Str::Language));
            const char* current = settings.ui.language == Features::Language::English
                                      ? Loc::Text(Loc::Str::English)
                                      : Loc::Text(Loc::Str::Korean);
            const ImVec2 currentSize = ImGui::CalcTextSize(current);
            drawList->AddText(ImVec2(maximum.x - currentSize.x - 26.0f, minimum.y + 10.0f), palette.text, current);
            drawList->AddTriangleFilled(ImVec2(maximum.x - 16.0f, minimum.y + 16.0f),
                                         ImVec2(maximum.x - 8.0f, minimum.y + 16.0f),
                                         ImVec2(maximum.x - 12.0f, minimum.y + 22.0f), palette.textMuted);
            if (pressed)
                ImGui::OpenPopup("##language_popup");
            if (ImGui::BeginPopup("##language_popup"))
            {
                bool changed = false;
                DrawPopupOption("korean", Loc::Text(Loc::Str::Korean),
                                settings.ui.language == Features::Language::Korean, &changed);
                if (changed)
                {
                    settings.ui.language = Features::Language::Korean;
                    ImGui::CloseCurrentPopup();
                }
                changed = false;
                DrawPopupOption("english", Loc::Text(Loc::Str::English),
                                settings.ui.language == Features::Language::English, &changed);
                if (changed)
                {
                    settings.ui.language = Features::Language::English;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }

        void DrawThemePicker(Features::Settings& settings)
        {
            const UiTheme::Palette& palette = UiTheme::Current();
            const float width = (std::max)(1.0f, ImGui::GetContentRegionAvail().x);
            const ImVec2 size(width, 38.0f);
            ImGui::PushID("theme_picker");
            const bool pressed = ImGui::InvisibleButton("##theme_button", size);
            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 maximum = ImGui::GetItemRectMax();
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(minimum, maximum, hovered ? palette.controlHovered : palette.controlBackground, 7.0f);
            drawList->AddRect(minimum, maximum, hovered ? palette.accent : palette.border, 7.0f);
            drawList->AddText(ImVec2(minimum.x + 12.0f, minimum.y + 10.0f), palette.textMuted,
                              Loc::Text(Loc::Str::Theme));
            const bool light = settings.ui.theme == Features::Theme::Light;
            const char* current = light ? Loc::Text(Loc::Str::Light) : Loc::Text(Loc::Str::Dark);
            const ImVec2 currentSize = ImGui::CalcTextSize(current);
            drawList->AddText(ImVec2(maximum.x - currentSize.x - 26.0f, minimum.y + 10.0f), palette.text, current);
            drawList->AddTriangleFilled(ImVec2(maximum.x - 16.0f, minimum.y + 16.0f),
                                         ImVec2(maximum.x - 8.0f, minimum.y + 16.0f),
                                         ImVec2(maximum.x - 12.0f, minimum.y + 22.0f), palette.textMuted);
            if (pressed)
                settings.ui.theme = light ? Features::Theme::Dark : Features::Theme::Light;
            ImGui::PopID();
        }

        void DrawSidebar(Features::Settings& settings)
        {
            const UiTheme::Palette& palette = UiTheme::Current();
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            const ImVec2 windowMinimum = ImGui::GetWindowPos();
            const ImVec2 windowSize = ImGui::GetWindowSize();
            const ImVec2 windowMaximum(windowMinimum.x + windowSize.x, windowMinimum.y + windowSize.y);
            window->DrawList->AddRectFilled(windowMinimum, windowMaximum, palette.sidebarBackground, 12.0f);
            window->DrawList->AddRect(windowMinimum, windowMaximum, palette.border, 12.0f);

            const ImVec2 brandPosition = ImGui::GetCursorScreenPos();
            window->DrawList->AddText(brandPosition, palette.accent, Loc::Text(Loc::Str::BrandName));
            window->DrawList->AddText(ImVec2(brandPosition.x, brandPosition.y + 23.0f), palette.textSubtle,
                                      Loc::Text(Loc::Str::BrandSubtitle));
            ImGui::Dummy(ImVec2(0.0f, 61.0f));

            SidebarTab("aimbot", Loc::Str::TabAimbot, Tab::Aimbot);
            SidebarTab("esp", Loc::Str::TabEsp, Tab::Esp);
            SidebarTab("misc", Loc::Str::TabMisc, Tab::Misc);
            SidebarTab("debug", Loc::Str::TabDebug, Tab::Debug);

            constexpr float footerHeight = 144.0f;
            const float footerTop = ImGui::GetWindowHeight() - footerHeight - ImGui::GetStyle().WindowPadding.y;
            if (ImGui::GetCursorPosY() < footerTop)
                ImGui::SetCursorPosY(footerTop);
            ImGui::Spacing();
            const ImVec2 lineStart = ImGui::GetCursorScreenPos();
            window->DrawList->AddLine(lineStart,
                                      ImVec2(lineStart.x + ImGui::GetContentRegionAvail().x, lineStart.y),
                                      palette.border);
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            DrawLanguagePicker(settings);
            ImGui::Spacing();
            DrawThemePicker(settings);
            ImGui::TextDisabled("%s", Loc::Text(Loc::Str::FooterHotkeys));
        }

        bool BeginSectionCard(const char* id)
        {
            const UiTheme::Palette& palette = UiTheme::Current();
            const ImVec2 minimum = ImGui::GetCursorScreenPos();
            const ImVec2 size((std::max)(1.0f, ImGui::GetContentRegionAvail().x),
                              (std::max)(1.0f, ImGui::GetContentRegionAvail().y));
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 maximum(minimum.x + size.x, minimum.y + size.y);
            drawList->AddRectFilled(minimum, maximum, palette.cardBackground, 10.0f);
            drawList->AddRect(minimum, maximum, palette.border, 10.0f);
            return ImGui::BeginChild(id, ImVec2(0.0f, 0.0f), false,
                                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysVerticalScrollbar);
        }

        void EndSectionCard()
        {
            ImGui::EndChild();
        }

        void DrawAimbot(Features::Settings& settings)
        {
            ToggleRow(Loc::Str::AimbotEnabled, "aimbot_enabled", &settings.aimbot.enabled);
            SectionSeparator();
            ToggleRow(Loc::Str::SilentAim, "silent", &settings.aimbot.silentAim);
            KeyRow(Loc::Str::ActivationKey, &settings.aimbot.activationKey);
            ToggleRow(Loc::Str::DrawFovCircle, "fov_circle", &settings.aimbot.drawFovCircle);
            ToggleRow(Loc::Str::TargetEnemies, "target_enemies", &settings.aimbot.targetEnemies);
            ToggleRow(Loc::Str::TargetPolice, "target_police", &settings.aimbot.targetPolice);
            ToggleRow(Loc::Str::OnlyVisibleTargets, "visible_only", &settings.aimbot.visibleOnly);
            ToggleRow(Loc::Str::RequireHealthPool, "require_health", &settings.aimbot.requireHealthPool);
            ToggleRow(Loc::Str::LimitHealthPool, "limit_health", &settings.aimbot.limitHealthPool);
            if (settings.aimbot.limitHealthPool)
            {
                FilledSliderFloat(Loc::Text(Loc::Str::MaxHealthPool), &settings.aimbot.maxHealthPool, 500.0f,
                                  6000.0f, Loc::Text(Loc::Str::HealthFormat));
            }
            FilledSliderFloat(Loc::Text(Loc::Str::FovRadius), &settings.aimbot.fovRadiusDegrees, 1.0f, 60.0f,
                              Loc::Text(Loc::Str::DegreesFormat));
            if (!settings.aimbot.silentAim)
                FilledSliderFloat(Loc::Text(Loc::Str::Smoothing), &settings.aimbot.smoothing, 0.0f, 30.0f,
                                  Loc::Text(Loc::Str::DecimalFormat));
            FilledSliderFloat(Loc::Text(Loc::Str::AimDistance), &settings.aimbot.maxDistanceMeters, 10.0f, 300.0f,
                              Loc::Text(Loc::Str::MetersFormat));

            char activationHint[768]{};
            snprintf(activationHint, sizeof(activationHint),
                     settings.aimbot.silentAim ? Loc::Text(Loc::Str::ActivationSilentHint)
                                               : Loc::Text(Loc::Str::ActivationClassicHint),
                     KeyName(settings.aimbot.activationKey));
            HintBox(activationHint);
            if (settings.aimbot.visibleOnly)
                HintBox(Loc::Text(Loc::Str::VisibleOnlyHint));
            if (settings.aimbot.requireHealthPool || settings.aimbot.limitHealthPool)
                HintBox(Loc::Text(Loc::Str::HealthFilterHint));
        }

        void DrawEsp(Features::Settings& settings)
        {
            ToggleRow(Loc::Str::EspEnabled, "esp_enabled", &settings.esp.enabled);
            SectionSeparator();
            ToggleRow(Loc::Str::BoundingBoxes, "boxes", &settings.esp.boundingBoxes);
            ToggleRow(Loc::Str::Skeleton, "skeleton", &settings.esp.skeleton);
            ToggleRow(Loc::Str::HealthBars, "health", &settings.esp.healthBars);
            ToggleRow(Loc::Str::NativeHighlight, "native", &settings.esp.nativeHighlight);
            ToggleRow(Loc::Str::HideDeadNpcs, "hide_dead", &settings.esp.hideDead);
            ToggleRow(Loc::Str::VisibilityCheck, "visibility", &settings.esp.visibilityCheck);
            ToggleRow(Loc::Str::HideOccludedNpcs, "hide_occluded", &settings.esp.hideOccluded);
            SectionSeparator();
            ToggleRow(Loc::Str::Civilians, "civilians", &settings.esp.showCivilians);
            ToggleRow(Loc::Str::Enemies, "enemies", &settings.esp.showEnemies);
            ToggleRow(Loc::Str::Police, "police", &settings.esp.showPolice);
            ToggleRow(Loc::Str::Unclassified, "unclassified", &settings.esp.showUnclassified);
            FilledSliderFloat(Loc::Text(Loc::Str::MaxDistance), &settings.esp.maxDistanceMeters, 10.0f, 300.0f,
                              Loc::Text(Loc::Str::MetersFormat));
            if (settings.esp.visibilityCheck)
                HintBox(Loc::Text(Loc::Str::VisibilityPerformanceHint));
        }

        void DrawMisc(Features::Settings& settings)
        {
            ToggleRow(Loc::Str::NoRecoil, "no_recoil", &settings.misc.noRecoil);
            ToggleRow(Loc::Str::NoSpread, "no_spread", &settings.misc.noSpread);
            ToggleRow(Loc::Str::AutoPistol, "auto_pistol", &settings.misc.autoPistol);
            ToggleRow(Loc::Str::InfiniteHealth, "infinite_health", &settings.misc.infiniteHealth);
            ToggleRow(Loc::Str::InfiniteStamina, "infinite_stamina", &settings.misc.infiniteStamina);
        }

        void DrawInternalStats()
        {
            const Game::EntityTracker::Stats entityStats = Game::EntityTracker::GetStats();
            const Game::PlayerModifiers::Stats modifierStats = Game::PlayerModifiers::GetStats();
            const Aimbot::Stats aimStats = Aimbot::GetStats();
            const Game::Visibility::Stats visibilityStats = Game::Visibility::GetStats();
            const Game::SilentAim::DiagnosticsSnapshot silentStats = Game::SilentAim::GetDiagnostics();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            const UiTheme::Palette& palette = UiTheme::Current();
            ImGui::TextColored(Color(palette.accent), "%s", Loc::Text(Loc::Str::InternalStatsTitle));
            ImGui::TextDisabled("Entity feed: %s | registered %llu | positioned %llu | NPCs %llu | live %llu",
                                entityStats.hookCreated ? "hooked" : "unavailable",
                                static_cast<unsigned long long>(entityStats.registered),
                                static_cast<unsigned long long>(entityStats.positioned),
                                static_cast<unsigned long long>(entityStats.puppets),
                                static_cast<unsigned long long>(entityStats.trackedPuppets));
            ImGui::TextDisabled("Classified live: civilian %llu | enemy %llu | police %llu | hostile %llu",
                                static_cast<unsigned long long>(entityStats.trackedCivilians),
                                static_cast<unsigned long long>(entityStats.trackedEnemies),
                                static_cast<unsigned long long>(entityStats.trackedPolice),
                                static_cast<unsigned long long>(entityStats.trackedHostile));
            ImGui::TextDisabled("Attitude feed: resolved %llu | unavailable %llu",
                                static_cast<unsigned long long>(entityStats.attitudeValid),
                                static_cast<unsigned long long>(entityStats.attitudeInvalid));
            ImGui::TextDisabled("Health feed: valid %llu | fallback/invalid %llu | pending position %llu",
                                static_cast<unsigned long long>(entityStats.healthValid),
                                static_cast<unsigned long long>(entityStats.healthInvalid),
                                static_cast<unsigned long long>(entityStats.pendingPosition));
            ImGui::TextDisabled("Native highlight: queued %llu | cleared %llu | failures %llu",
                                static_cast<unsigned long long>(entityStats.nativeHighlightQueued),
                                static_cast<unsigned long long>(entityStats.nativeHighlightCleared),
                                static_cast<unsigned long long>(entityStats.nativeHighlightFailures));
            ImGui::TextDisabled("No recoil: %s (%s) | target 0x%llX | applied %llu | removed %llu | retired %llu | failures %llu",
                                modifierStats.active ? "active" : (modifierStats.available ? "ready" : "unavailable"),
                                modifierStats.usingWeaponTarget ? "weapon" : "player fallback",
                                static_cast<unsigned long long>(modifierStats.targetId),
                                static_cast<unsigned long long>(modifierStats.applied),
                                static_cast<unsigned long long>(modifierStats.removed),
                                static_cast<unsigned long long>(modifierStats.retiredOwnerResets),
                                static_cast<unsigned long long>(modifierStats.failures));
            ImGui::TextDisabled("Targets: candidates %u | eligible %u | no pool %u | over cap %u | occluded %u",
                                aimStats.candidates, aimStats.eligible, aimStats.skippedNoHealthPool,
                                aimStats.skippedHealthCap, aimStats.skippedOccluded);
            if (aimStats.targetEntityId != 0)
            {
                ImGui::TextDisabled("Selected 0x%llX | health %.0f / %.0f%s",
                                    static_cast<unsigned long long>(aimStats.targetEntityId),
                                    aimStats.targetHealth, aimStats.targetHealthMax,
                                    aimStats.targetHealthValid ? "" : " (stat pool unresolved)");
            }
            ImGui::TextDisabled("Visibility cache: %s | visible %llu | occluded %llu | dropped %llu",
                                visibilityStats.available ? "ready" : "unavailable",
                                static_cast<unsigned long long>(visibilityStats.visible),
                                static_cast<unsigned long long>(visibilityStats.occluded),
                                static_cast<unsigned long long>(visibilityStats.dropped));
            ImGui::TextDisabled("Silent aim: %s | redirects %llu | rejected %llu",
                                silentStats.crosshairCoreHookCreated ? "crosshair core hooked" : "unavailable",
                                static_cast<unsigned long long>(silentStats.nativeCrosshairCoreRedirects),
                                static_cast<unsigned long long>(silentStats.rejectedShots));

            ImGui::Spacing();
            if (DisclosureHeader("silent_diagnostics", Loc::Str::SilentAimDiagnostics, &g_silentDiagnosticsOpen))
            {
                HintBox(Loc::Text(Loc::Str::HeadlessAimbotHint));
                ImGui::TextDisabled("Crosshair core: calls %llu | redirects %llu",
                                    static_cast<unsigned long long>(silentStats.nativeCrosshairCoreCalls),
                                    static_cast<unsigned long long>(silentStats.nativeCrosshairCoreRedirects));
                if (silentStats.producerHooks == 0 && silentStats.listenerHooks == 0)
                {
                    ImGui::TextDisabled("Observation hooks: disabled in this build");
                }
                else
                {
                    ImGui::TextDisabled("Observation hooks: producer %u | projectile %u | effect %llu | start %llu | "
                                        "prepare %llu | crosshair %llu/%llu | projectile events %llu/%llu",
                                        silentStats.producerHooks, silentStats.listenerHooks,
                                        static_cast<unsigned long long>(silentStats.effectRuns),
                                        static_cast<unsigned long long>(silentStats.attackStarts),
                                        static_cast<unsigned long long>(silentStats.attackPrepares),
                                        static_cast<unsigned long long>(silentStats.crosshairCalls),
                                        static_cast<unsigned long long>(silentStats.defaultCrosshairCalls),
                                        static_cast<unsigned long long>(silentStats.projectileEvents),
                                        static_cast<unsigned long long>(silentStats.localPlayerEvents));
                }
            }
        }

        void DrawDebug(Features::Settings& settings)
        {
            ToggleRow(Loc::Str::ShowFps, "show_fps", &settings.debug.showFps);
            ToggleRow(Loc::Str::ShowGraph, "show_graph", &settings.debug.showGraph);
            ToggleRow(Loc::Str::ShowInternalStats, "show_stats", &settings.debug.showInternalStats);
            SectionSeparator();
            ToggleRow(Loc::Str::DiagnosticLogging, "logging", &settings.debug.diagnosticLogging);
            if (settings.debug.diagnosticLogging)
                HintBox(Loc::Text(Loc::Str::DiagnosticLoggingHint));
            ToggleRow(Loc::Str::CrashReporting, "crash_reporting", &settings.debug.crashReporting);
            if (settings.debug.crashReporting)
                HintBox(Loc::Text(Loc::Str::CrashReportingHint));
            ToggleRow(Loc::Str::PerformanceProfiling, "profiling", &settings.debug.performanceProfiling);
            if (settings.debug.performanceProfiling)
                HintBox(Loc::Text(Loc::Str::PerformanceProfilingHint));
            ToggleRow(Loc::Str::DebuggerOutput, "debugger_output", &settings.debug.debuggerOutput);
            if (settings.debug.debuggerOutput)
                HintBox(Loc::Text(Loc::Str::DebuggerOutputHint));
            ToggleRow(Loc::Str::HeadlessAimbot, "headless_aimbot", &settings.debug.headlessAimbot);
            if (settings.debug.headlessAimbot)
                HintBox(Loc::Text(Loc::Str::HeadlessAimbotHint));
            if (settings.debug.showInternalStats)
                DrawInternalStats();
        }

        void DrawPage(Features::Settings& settings)
        {
            const Loc::Str title = g_activeTab == Tab::Aimbot
                                       ? Loc::Str::TabAimbot
                                       : g_activeTab == Tab::Esp ? Loc::Str::TabEsp
                                       : g_activeTab == Tab::Misc ? Loc::Str::TabMisc
                                                                  : Loc::Str::TabDebug;
            const Loc::Str description = g_activeTab == Tab::Aimbot
                                             ? Loc::Str::AimbotDescription
                                             : g_activeTab == Tab::Esp ? Loc::Str::EspDescription
                                             : g_activeTab == Tab::Misc ? Loc::Str::MiscDescription
                                                                        : Loc::Str::DebugDescription;
            const UiTheme::Palette& palette = UiTheme::Current();
            ImGui::TextColored(Color(palette.accent), "%s", Loc::Text(title));
            ImGui::TextDisabled("%s", Loc::Text(description));
            ImGui::Spacing();
            const bool cardVisible = BeginSectionCard("##settings_card");
            if (cardVisible)
            {
                if (g_activeTab == Tab::Aimbot)
                    DrawAimbot(settings);
                else if (g_activeTab == Tab::Esp)
                    DrawEsp(settings);
                else if (g_activeTab == Tab::Misc)
                    DrawMisc(settings);
                else
                    DrawDebug(settings);
            }
            // BeginChild/EndChild 쌍은 클리핑되어 false가 반환된 경우에도 반드시 닫아야 한다.
            EndSectionCard();
        }
    }

    void ApplyStyle()
    {
        // 기본 ImGui 폰트엔 한글 글리프가 없어서 한글 텍스트가 깨질 수 있다. Windows에 기본 내장된
        // 맑은 고딕을 얹고, 파일이 없는 환경이면 조용히 기본 폰트로 폴백한다.
        ImGuiIO& io = ImGui::GetIO();
        if (GetFileAttributesW(L"C:\\Windows\\Fonts\\malgun.ttf") != INVALID_FILE_ATTRIBUTES)
        {
            ImFontConfig config;
            config.OversampleH = 2;
            config.OversampleV = 2;
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgun.ttf", 17.0f, &config);
        }

        UiTheme::ApplyImGuiStyle();
    }

    bool ToggleSwitch(const char* label, bool* value)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems || !value)
            return false;

        ImGuiContext& g = *ImGui::GetCurrentContext();
        const ImGuiID id = window->GetID(label);
        const float height = ImGui::GetFrameHeight();
        const float width = height * 1.8f;
        const ImVec2 position = window->DC.CursorPos;
        const ImRect boundingBox(position, ImVec2(position.x + width, position.y + height));

        ImGui::ItemSize(boundingBox);
        if (!ImGui::ItemAdd(boundingBox, id))
            return false;

        bool hovered = false;
        bool held = false;
        const bool pressed = ImGui::ButtonBehavior(boundingBox, id, &hovered, &held);
        if (pressed)
            *value = !*value;

        const float target = *value ? 1.0f : 0.0f;
        ImGuiStorage* storage = window->DC.StateStorage;
        float animation = storage->GetFloat(id, target);
        animation += (target - animation) * ImMin(g.IO.DeltaTime * 12.0f, 1.0f);
        storage->SetFloat(id, animation);

        const UiTheme::Palette& palette = UiTheme::Current();
        const ImU32 background = *value ? (held ? palette.accentHovered : palette.accent)
                                        : (hovered ? palette.toggleOffHovered : palette.toggleOff);
        window->DrawList->AddRectFilled(boundingBox.Min, boundingBox.Max, background, height * 0.5f);
        const float radius = height * 0.5f - 2.0f;
        const float thumbX = boundingBox.Min.x + radius + 2.0f + animation * (width - height);
        window->DrawList->AddCircleFilled(ImVec2(thumbX, boundingBox.Min.y + height * 0.5f), radius,
                                          palette.toggleThumb);
        return pressed;
    }

    bool FilledSliderFloat(const char* label, float* value, float minimum, float maximum, const char* format)
    {
        if (!value || maximum <= minimum || !format)
            return false;

        ImGui::PushID(label);
        char valueText[64]{};
        snprintf(valueText, sizeof(valueText), format, *value);
        const float trackPositionX = ImGui::GetCursorPosX();
        const float trackWidth = (std::max)(160.0f, ImGui::GetContentRegionAvail().x);
        const float trackRightX = trackPositionX + trackWidth;
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        const float valueWidth = ImGui::CalcTextSize(valueText).x;
        ImGui::SetCursorPosX(trackRightX - valueWidth);
        ImGui::TextDisabled("%s", valueText);

        const float height = 16.0f;
        ImGui::SetCursorPosX(trackPositionX);
        ImGui::InvisibleButton("##track", ImVec2(trackWidth, height));

        bool changed = false;
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            const ImVec2 trackMinimum = ImGui::GetItemRectMin();
            const ImVec2 trackMaximum = ImGui::GetItemRectMax();
            const float normalized = std::clamp((ImGui::GetIO().MousePos.x - trackMinimum.x) /
                                                    (trackMaximum.x - trackMinimum.x),
                                                0.0f, 1.0f);
            const float newValue = minimum + normalized * (maximum - minimum);
            changed = newValue != *value;
            *value = newValue;
        }

        const ImVec2 trackMinimum = ImGui::GetItemRectMin();
        const ImVec2 trackMaximum = ImGui::GetItemRectMax();
        const float normalized = std::clamp((*value - minimum) / (maximum - minimum), 0.0f, 1.0f);
        const float radius = height * 0.5f;
        const float grabX = trackMinimum.x + normalized * (trackMaximum.x - trackMinimum.x);
        const UiTheme::Palette& palette = UiTheme::Current();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(trackMinimum, trackMaximum, palette.controlBackground, radius);
        if (grabX > trackMinimum.x)
            drawList->AddRectFilled(trackMinimum, ImVec2(grabX, trackMaximum.y), palette.accent, radius);
        drawList->AddCircleFilled(ImVec2(grabX, trackMinimum.y + radius), radius - 2.0f, palette.toggleThumb);

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
        const UiTheme::Palette& palette = UiTheme::Current();
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        const int alpha = static_cast<int>(255.0f * progress);
        drawList->AddRectFilled(ImVec2(minimum.x + 3.0f, minimum.y + 8.0f),
                                ImVec2(maximum.x + 3.0f, maximum.y + 8.0f),
                                UiTheme::WithAlpha(palette.shadow, static_cast<int>(90.0f * progress)), 14.0f);
        drawList->AddRectFilled(minimum, maximum, UiTheme::WithAlpha(palette.cardBackground, alpha), 14.0f);
        drawList->AddRect(minimum, maximum, UiTheme::WithAlpha(palette.accent, static_cast<int>(150.0f * progress)),
                          14.0f, 1.0f, ImDrawFlags_None);
        drawList->AddRectFilled(ImVec2(minimum.x, minimum.y + 16.0f),
                                ImVec2(minimum.x + 4.0f, maximum.y - 16.0f),
                                UiTheme::WithAlpha(palette.accent, alpha), 2.0f);

        const ImVec2 titlePosition(minimum.x + 24.0f, minimum.y + 14.0f);
        drawList->AddText(titlePosition, UiTheme::WithAlpha(palette.accentHovered, alpha),
                          Loc::Text(Loc::Str::TrainerReady));

        const char* prefix = Loc::Text(Loc::Str::StartupOpenPrefix);
        const char* key = Loc::Text(Loc::Str::InsertKey);
        const char* suffix = Loc::Text(Loc::Str::StartupOpenSuffix);
        const float gap = 10.0f;
        const float keyPadding = 28.0f;
        const ImVec2 prefixSize = ImGui::CalcTextSize(prefix);
        const ImVec2 keySize = ImGui::CalcTextSize(key);
        const ImVec2 suffixSize = ImGui::CalcTextSize(suffix);
        const float contentWidth = prefixSize.x + gap + keySize.x + keyPadding + gap + suffixSize.x;
        float contentX = minimum.x + (panelWidth - contentWidth) * 0.5f;
        const float baselineY = minimum.y + 45.0f;
        drawList->AddText(ImVec2(contentX, baselineY), UiTheme::WithAlpha(palette.text, alpha), prefix);
        contentX += prefixSize.x + gap;
        const ImVec2 keyMinimum(contentX, baselineY - 4.0f);
        const ImVec2 keyMaximum(keyMinimum.x + keySize.x + keyPadding, keyMinimum.y + 25.0f);
        drawList->AddRectFilled(keyMinimum, keyMaximum, UiTheme::WithAlpha(palette.controlBackground, alpha), 6.0f);
        drawList->AddRect(keyMinimum, keyMaximum, UiTheme::WithAlpha(palette.border, alpha), 6.0f);
        drawList->AddText(ImVec2(keyMinimum.x + (keyMaximum.x - keyMinimum.x - keySize.x) * 0.5f,
                                 keyMinimum.y + (keyMaximum.y - keyMinimum.y - keySize.y) * 0.5f),
                          UiTheme::WithAlpha(palette.text, alpha), key);
        drawList->AddText(ImVec2(keyMaximum.x + gap, baselineY), UiTheme::WithAlpha(palette.text, alpha), suffix);
    }

    void DrawMainMenu()
    {
        // ApplyStyle는 초기화 시 한 번 호출되지만 테마 버튼은 런타임에 색을 바꾸므로 매 프레임 팔레트를
        // ImGui 기본 색에도 반영한다. 커스텀 드로잉은 각 위젯이 Current()에서 같은 팔레트를 읽는다.
        UiTheme::ApplyImGuiStyle();
        ImGui::SetNextWindowSize(ImVec2(920.0f, 720.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(820.0f, 580.0f), ImVec2(1250.0f, 1050.0f));
        if (!ImGui::Begin("##trainer_main_window", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }

        Features::Settings& settings = Features::GetSettings();
        const UiTheme::Palette& palette = UiTheme::Current();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 windowMinimum = ImGui::GetWindowPos();
        const ImVec2 windowSize = ImGui::GetWindowSize();
        const ImVec2 windowMaximum(windowMinimum.x + windowSize.x, windowMinimum.y + windowSize.y);
        drawList->AddRectFilled(ImVec2(windowMinimum.x + 3.0f, windowMinimum.y + 5.0f),
                                ImVec2(windowMaximum.x + 3.0f, windowMaximum.y + 5.0f), palette.shadow, 12.0f);
        drawList->AddRect(windowMinimum, windowMaximum, palette.border, 12.0f);

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f, 9.0f));
        ImGui::BeginChild("##sidebar", ImVec2(198.0f, 0.0f), false, ImGuiWindowFlags_NoBackground);
        DrawSidebar(settings);
        ImGui::EndChild();
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::BeginChild("##content_panel", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_NoBackground);
        const ImVec2 contentMinimum = ImGui::GetWindowPos();
        const ImVec2 contentSize = ImGui::GetWindowSize();
        const ImVec2 contentMaximum(contentMinimum.x + contentSize.x, contentMinimum.y + contentSize.y);
        drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(contentMinimum, contentMaximum, palette.panelBackground, 12.0f);
        drawList->AddRect(contentMinimum, contentMaximum, palette.border, 12.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 18.0f));
        DrawPage(settings);
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::End();
    }
}
