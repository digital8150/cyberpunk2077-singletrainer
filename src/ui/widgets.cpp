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

        // ApplyStyle에서 채워진다. 폰트 아틀라스를 못 만든 환경에서는 전부 nullptr로 남고, 아래 헬퍼가
        // nullptr을 걸러 내므로 그대로 기본 폰트 하나로 그려진다.
        ImFont* g_fontBody = nullptr;
        ImFont* g_fontTitle = nullptr;
        ImFont* g_fontLabel = nullptr;  // 카드 제목용 소형 굵은 자체
        ImFont* g_fontSmall = nullptr;  // 부연 설명·값 표시

        // 카드 안에서는 오른쪽 여백만큼 줄어든 폭을 써야 컬트롤이 카드 테두리에 붙지 않는다.
        float g_contentInset = 0.0f;

        void PushFontScaled(ImFont* font)
        {
            ImGui::PushFont(font ? font : ImGui::GetFont(), 0.0f);
        }

        float RowWidth()
        {
            return (std::max)(1.0f, ImGui::GetContentRegionAvail().x - g_contentInset);
        }

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

        // 예전에는 모든 설명이 노란 경고 블록으로 나와서, 에임봇 탭 하나에만 세 개가 쌓이면 패널의
        // 절반을 먹었다. 설명과 경고를 분리한다: 대부분은 왼쪽 세로 줄만 가진 작은 보조 텍스트고,
        // 사용자가 켜면 성능을 잃는 진짜 경고만 박스로 그린다.
        void HintText(const char* text)
        {
            if (!text)
                return;

            const UiTheme::Palette& palette = UiTheme::Current();
            PushFontScaled(g_fontSmall);
            constexpr float kRule = 2.0f;
            constexpr float kGap = 10.0f;
            const float width = RowWidth();
            const float textWidth = (std::max)(1.0f, width - kRule - kGap);
            const float textHeight = ImGui::CalcTextSize(text, nullptr, false, textWidth).y;
            const ImVec2 minimum = ImGui::GetCursorScreenPos();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(minimum, ImVec2(minimum.x + kRule, minimum.y + textHeight),
                                    UiTheme::WithAlpha(palette.border, 220), 1.0f);
            drawList->AddText(nullptr, 0.0f, ImVec2(minimum.x + kRule + kGap, minimum.y), palette.textMuted,
                              text, nullptr, textWidth);
            ImGui::Dummy(ImVec2(width, textHeight));
            ImGui::PopFont();
        }

        // 성능 경고 전용. 사용자가 요구한 "힌트박스 디자인"은 이쪽이다.
        void WarningBox(const char* text)
        {
            if (!text)
                return;

            const UiTheme::Palette& palette = UiTheme::Current();
            PushFontScaled(g_fontSmall);
            constexpr float kBar = 3.0f;
            constexpr float kPaddingX = 12.0f;
            constexpr float kPaddingY = 9.0f;
            const float width = RowWidth();
            const float textWidth = (std::max)(1.0f, width - kBar - kPaddingX * 2.0f);
            const float textHeight = ImGui::CalcTextSize(text, nullptr, false, textWidth).y;
            const float height = textHeight + kPaddingY * 2.0f;
            const ImVec2 minimum = ImGui::GetCursorScreenPos();
            const ImVec2 maximum(minimum.x + width, minimum.y + height);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(minimum, maximum, palette.warningSoft, 7.0f);
            drawList->AddRectFilled(minimum, ImVec2(minimum.x + kBar + 4.0f, maximum.y), palette.warning, 7.0f);
            drawList->AddRectFilled(ImVec2(minimum.x + kBar, minimum.y), ImVec2(minimum.x + kBar + 6.0f, maximum.y),
                                    palette.warningSoft, 0.0f);
            drawList->AddText(nullptr, 0.0f, ImVec2(minimum.x + kBar + kPaddingX, minimum.y + kPaddingY),
                              palette.warning, text, nullptr, textWidth);
            ImGui::Dummy(ImVec2(width, height));
            ImGui::PopFont();
        }

        // 카드 하나. 내용의 높이를 미리 알 수 없으므로 드로우 채널을 갈라서 배경을 나중에 그린다
        // (ImGui에서 컨테이너 배경을 그리는 표준 방법이다). 카드는 중첩하지 않으므로 채널 하나면 된다.
        struct CardScope
        {
            ImVec2 minimum{};
            float width = 0.0f;
            float previousInset = 0.0f;
        };

        CardScope g_card;
        constexpr float kCardPadX = 16.0f;
        constexpr float kCardPadY = 14.0f;

        void BeginCard(const char* title)
        {
            g_card.minimum = ImGui::GetCursorScreenPos();
            g_card.width = (std::max)(1.0f, ImGui::GetContentRegionAvail().x);
            g_card.previousInset = g_contentInset;

            ImGui::GetWindowDrawList()->ChannelsSplit(2);
            ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);

            ImGui::Indent(kCardPadX);
            // Indent는 왼쪽만 밀어 준다. 오른쪽 여백은 위젯이 폭을 계산할 때 빼야 한다.
            g_contentInset = kCardPadX * 2.0f;
            ImGui::Dummy(ImVec2(0.0f, kCardPadY - ImGui::GetStyle().ItemSpacing.y));

            if (title && *title)
            {
                const UiTheme::Palette& palette = UiTheme::Current();
                PushFontScaled(g_fontLabel);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(palette.textSubtle));
                ImGui::TextUnformatted(title);
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0.0f, 3.0f));
            }
        }

        void EndCard()
        {
            ImGui::Dummy(ImVec2(0.0f, kCardPadY - ImGui::GetStyle().ItemSpacing.y));
            ImGui::Unindent(kCardPadX);
            g_contentInset = g_card.previousInset;

            const ImVec2 maximum(g_card.minimum.x + g_card.width, ImGui::GetCursorScreenPos().y);
            const UiTheme::Palette& palette = UiTheme::Current();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->ChannelsSetCurrent(0);
            // 아주 옅은 그림자 한 겹이 카드를 바닥에서 띄운다. 라이트 테마에서 특히 이것 없이는
            // 흰 카드가 회색 바닥에 그냥 얹힌 종이처럼만 보인다.
            drawList->AddRectFilled(ImVec2(g_card.minimum.x, g_card.minimum.y + 2.0f),
                                    ImVec2(maximum.x, maximum.y + 3.0f), palette.shadow, 11.0f);
            drawList->AddRectFilled(g_card.minimum, maximum, palette.cardBackground, 11.0f);
            drawList->AddRect(g_card.minimum, maximum, palette.border, 11.0f);
            drawList->ChannelsMerge();

            ImGui::Dummy(ImVec2(0.0f, 8.0f));
        }

        void SectionSeparator()
        {
            const UiTheme::Palette& palette = UiTheme::Current();
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            const ImVec2 start = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(start, ImVec2(start.x + RowWidth(), start.y),
                                                UiTheme::WithAlpha(palette.border, 180));
            ImGui::Dummy(ImVec2(0.0f, 5.0f));
        }

        void ToggleRow(Loc::Str label, const char* id, bool* value)
        {
            ImGui::PushID(id);
            const float width = RowWidth();
            const float startX = ImGui::GetCursorPosX();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(Loc::Text(label));
            ImGui::SameLine();
            constexpr float kToggleWidth = 36.0f;
            ImGui::SetCursorPosX(startX + width - kToggleWidth);
            ToggleSwitch("##switch", value);
            ImGui::PopID();
        }

        void KeyRow(Loc::Str label, unsigned int* key)
        {
            ImGui::PushID("activation_key_row");
            const float width = RowWidth();
            const float startX = ImGui::GetCursorPosX();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(Loc::Text(label));
            ImGui::SameLine();
            ImGui::SetCursorPosX(startX + width - 132.0f);
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
            // 50px 높이에 항목이 넷뿐이라 사이드바 위쪽이 텅 비어 보였다. 38px면 목록으로 읽힌다.
            const ImVec2 size(width, 38.0f);
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
                window->DrawList->AddRectFilled(ImVec2(minimum.x, minimum.y + 9.0f),
                                                ImVec2(minimum.x + 3.0f, maximum.y - 9.0f), palette.accent, 2.0f);
            }
            else if (hovered)
            {
                window->DrawList->AddRectFilled(minimum, maximum, palette.controlHovered, 8.0f);
            }
            // 예전엔 항목마다 회색 점이 붙어 있었는데, 선택 상태는 배경과 좌측 바가 이미 말해 준다.
            PushFontScaled(selected ? g_fontBody : g_fontBody);
            window->DrawList->AddText(ImVec2(minimum.x + 16.0f,
                                             minimum.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f),
                                      selected ? palette.accent : palette.textMuted, Loc::Text(label));
            ImGui::PopFont();
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

            // 브랜드 표시: 액센트 사각형 하나 + 굵은 제목. 예전에는 본문과 같은 크기의 "CBPK / TRAINER"
            // 한 줄이라 헤더로 읽히지 않았다.
            const ImVec2 brand = ImGui::GetCursorScreenPos();
            window->DrawList->AddRectFilled(ImVec2(brand.x, brand.y + 3.0f), ImVec2(brand.x + 4.0f, brand.y + 21.0f),
                                            palette.accent, 2.0f);
            PushFontScaled(g_fontTitle);
            window->DrawList->AddText(ImVec2(brand.x + 14.0f, brand.y), palette.text,
                                      Loc::Text(Loc::Str::BrandName));
            ImGui::PopFont();
            PushFontScaled(g_fontSmall);
            window->DrawList->AddText(ImVec2(brand.x + 14.0f, brand.y + 26.0f), palette.textSubtle,
                                      Loc::Text(Loc::Str::BrandSubtitle));
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0.0f, 56.0f));

            SidebarTab("aimbot", Loc::Str::TabAimbot, Tab::Aimbot);
            SidebarTab("esp", Loc::Str::TabEsp, Tab::Esp);
            SidebarTab("misc", Loc::Str::TabMisc, Tab::Misc);
            SidebarTab("debug", Loc::Str::TabDebug, Tab::Debug);

            constexpr float footerHeight = 122.0f;
            const float footerTop = ImGui::GetWindowHeight() - footerHeight - ImGui::GetStyle().WindowPadding.y;
            if (ImGui::GetCursorPosY() < footerTop)
                ImGui::SetCursorPosY(footerTop);
            const ImVec2 lineStart = ImGui::GetCursorScreenPos();
            window->DrawList->AddLine(lineStart,
                                      ImVec2(lineStart.x + ImGui::GetContentRegionAvail().x, lineStart.y),
                                      UiTheme::WithAlpha(palette.border, 200));
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            DrawLanguagePicker(settings);
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            DrawThemePicker(settings);
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            PushFontScaled(g_fontSmall);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(palette.textSubtle));
            ImGui::TextUnformatted(Loc::Text(Loc::Str::FooterHotkeys));
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }

        // 페이지 본체는 스크롤 영역일 뿐 그 자체가 카드가 아니다. 예전에는 여기서도 카드 배경을 그려서,
        // 이제 각 그룹이 카드를 갖게 된 뒤로는 카드 안에 카드가 겹치는 모양이 된다.
        bool BeginPageScroll(const char* id)
        {
            return ImGui::BeginChild(id, ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_NoBackground);
        }

        void EndPageScroll()
        {
            ImGui::EndChild();
        }

        void DrawAimbot(Features::Settings& settings)
        {
            BeginCard(Loc::Text(Loc::Str::CardGeneral));
            ToggleRow(Loc::Str::AimbotEnabled, "aimbot_enabled", &settings.aimbot.enabled);
            ToggleRow(Loc::Str::SilentAim, "silent", &settings.aimbot.silentAim);
            KeyRow(Loc::Str::ActivationKey, &settings.aimbot.activationKey);
            char activationHint[768]{};
            snprintf(activationHint, sizeof(activationHint),
                     settings.aimbot.silentAim ? Loc::Text(Loc::Str::ActivationSilentHint)
                                               : Loc::Text(Loc::Str::ActivationClassicHint),
                     KeyName(settings.aimbot.activationKey));
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            HintText(activationHint);
            EndCard();

            BeginCard(Loc::Text(Loc::Str::CardTargeting));
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
            if (settings.aimbot.visibleOnly || settings.aimbot.requireHealthPool ||
                settings.aimbot.limitHealthPool)
            {
                ImGui::Dummy(ImVec2(0.0f, 2.0f));
            }
            if (settings.aimbot.visibleOnly)
                HintText(Loc::Text(Loc::Str::VisibleOnlyHint));
            if (settings.aimbot.requireHealthPool || settings.aimbot.limitHealthPool)
                HintText(Loc::Text(Loc::Str::HealthFilterHint));
            EndCard();

            BeginCard(Loc::Text(Loc::Str::CardAiming));
            ToggleRow(Loc::Str::DrawFovCircle, "fov_circle", &settings.aimbot.drawFovCircle);
            FilledSliderFloat(Loc::Text(Loc::Str::FovRadius), &settings.aimbot.fovRadiusDegrees, 1.0f, 60.0f,
                              Loc::Text(Loc::Str::DegreesFormat));
            if (!settings.aimbot.silentAim)
                FilledSliderFloat(Loc::Text(Loc::Str::Smoothing), &settings.aimbot.smoothing, 0.0f, 30.0f,
                                  Loc::Text(Loc::Str::DecimalFormat));
            FilledSliderFloat(Loc::Text(Loc::Str::AimDistance), &settings.aimbot.maxDistanceMeters, 10.0f,
                              300.0f, Loc::Text(Loc::Str::MetersFormat));
            EndCard();
        }

        void DrawEsp(Features::Settings& settings)
        {
            BeginCard(Loc::Text(Loc::Str::CardGeneral));
            ToggleRow(Loc::Str::EspEnabled, "esp_enabled", &settings.esp.enabled);
            EndCard();

            BeginCard(Loc::Text(Loc::Str::CardVisuals));
            ToggleRow(Loc::Str::BoundingBoxes, "boxes", &settings.esp.boundingBoxes);
            ToggleRow(Loc::Str::Skeleton, "skeleton", &settings.esp.skeleton);
            ToggleRow(Loc::Str::HealthBars, "health", &settings.esp.healthBars);
            ToggleRow(Loc::Str::NativeHighlight, "native", &settings.esp.nativeHighlight);
            EndCard();

            BeginCard(Loc::Text(Loc::Str::CardFilters));
            ToggleRow(Loc::Str::Civilians, "civilians", &settings.esp.showCivilians);
            ToggleRow(Loc::Str::Enemies, "enemies", &settings.esp.showEnemies);
            ToggleRow(Loc::Str::Police, "police", &settings.esp.showPolice);
            ToggleRow(Loc::Str::Unclassified, "unclassified", &settings.esp.showUnclassified);
            SectionSeparator();
            ToggleRow(Loc::Str::HideDeadNpcs, "hide_dead", &settings.esp.hideDead);
            ToggleRow(Loc::Str::VisibilityCheck, "visibility", &settings.esp.visibilityCheck);
            ToggleRow(Loc::Str::HideOccludedNpcs, "hide_occluded", &settings.esp.hideOccluded);
            FilledSliderFloat(Loc::Text(Loc::Str::MaxDistance), &settings.esp.maxDistanceMeters, 10.0f, 300.0f,
                              Loc::Text(Loc::Str::MetersFormat));
            if (settings.esp.visibilityCheck)
            {
                ImGui::Dummy(ImVec2(0.0f, 4.0f));
                WarningBox(Loc::Text(Loc::Str::VisibilityPerformanceHint));
            }
            EndCard();
        }

        void DrawMisc(Features::Settings& settings)
        {
            BeginCard(Loc::Text(Loc::Str::CardWeapon));
            ToggleRow(Loc::Str::NoRecoil, "no_recoil", &settings.misc.noRecoil);
            ToggleRow(Loc::Str::NoSpread, "no_spread", &settings.misc.noSpread);
            ToggleRow(Loc::Str::AutoPistol, "auto_pistol", &settings.misc.autoPistol);
            EndCard();

            BeginCard(Loc::Text(Loc::Str::CardPlayer));
            ToggleRow(Loc::Str::InfiniteHealth, "infinite_health", &settings.misc.infiniteHealth);
            ToggleRow(Loc::Str::InfiniteStamina, "infinite_stamina", &settings.misc.infiniteStamina);
            EndCard();
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
                HintText(Loc::Text(Loc::Str::HeadlessAimbotHint));
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
            BeginCard(Loc::Text(Loc::Str::CardOverlay));
            ToggleRow(Loc::Str::ShowFps, "show_fps", &settings.debug.showFps);
            ToggleRow(Loc::Str::ShowGraph, "show_graph", &settings.debug.showGraph);
            ToggleRow(Loc::Str::ShowInternalStats, "show_stats", &settings.debug.showInternalStats);
            EndCard();

            // 이 카드의 토글은 전부 켜면 비용을 내는 것들이다. 켜져 있을 때만 경고 박스를 붙인다 -
            // 꺼져 있는 항목까지 경고를 달면 패널이 경고로 뒤덮여서 진짜 경고가 안 읽힌다.
            BeginCard(Loc::Text(Loc::Str::CardDiagnostics));
            ToggleRow(Loc::Str::DiagnosticLogging, "logging", &settings.debug.diagnosticLogging);
            if (settings.debug.diagnosticLogging)
                WarningBox(Loc::Text(Loc::Str::DiagnosticLoggingHint));
            ToggleRow(Loc::Str::CrashReporting, "crash_reporting", &settings.debug.crashReporting);
            if (settings.debug.crashReporting)
                WarningBox(Loc::Text(Loc::Str::CrashReportingHint));
            ToggleRow(Loc::Str::PerformanceProfiling, "profiling", &settings.debug.performanceProfiling);
            if (settings.debug.performanceProfiling)
                WarningBox(Loc::Text(Loc::Str::PerformanceProfilingHint));
            ToggleRow(Loc::Str::DebuggerOutput, "debugger_output", &settings.debug.debuggerOutput);
            if (settings.debug.debuggerOutput)
                WarningBox(Loc::Text(Loc::Str::DebuggerOutputHint));
            EndCard();

            BeginCard(Loc::Text(Loc::Str::CardAdvanced));
            ToggleRow(Loc::Str::HeadlessAimbot, "headless_aimbot", &settings.debug.headlessAimbot);
            if (settings.debug.headlessAimbot)
                WarningBox(Loc::Text(Loc::Str::HeadlessAimbotHint));
            EndCard();

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

            // 페이지 제목은 본문보다 확실히 커야 한다. 예전에는 본문과 같은 크기에 색만 파래서 제목으로
            // 읽히지 않았다.
            PushFontScaled(g_fontTitle);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(palette.text));
            ImGui::TextUnformatted(Loc::Text(title));
            ImGui::PopStyleColor();
            ImGui::PopFont();
            PushFontScaled(g_fontSmall);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(palette.textSubtle));
            ImGui::TextUnformatted(Loc::Text(description));
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0.0f, 10.0f));

            const bool visible = BeginPageScroll("##settings_scroll");
            if (visible)
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
            EndPageScroll();
        }
    }

    void ApplyStyle()
    {
        // 기본 ImGui 폰트엔 한글 글리프가 없어서 한글이 깨진다. Windows에 기본 내장된 맑은 고딕을
        // 얹고, 파일이 없는 환경이면 조용히 기본 폰트로 폴백한다.
        //
        // 크기를 네 단계로 나눠 얹는 이유: 전부 한 크기(17px)로 그리면 제목과 항목 이름과 부연 설명이
        // 같은 무게로 읽혀서 눈이 어디를 먼저 볼지 못 정한다. AGENTS.md가 "ImGui가 투박해 보이는
        // 원인의 8할이 폰트"라고 적어 둔 것이 정확히 이 지점이다. 굵은 자체(malgunbd)가 있으면 제목과
        // 카드 라벨에만 쓰고, 없으면 같은 파일을 크기만 달리 얹는다.
        ImGuiIO& io = ImGui::GetIO();
        const char* regular = "C:\Windows\Fonts\malgun.ttf";
        const char* bold = "C:\Windows\Fonts\malgunbd.ttf";
        const bool hasBold = GetFileAttributesW(L"C:\Windows\Fonts\malgunbd.ttf") != INVALID_FILE_ATTRIBUTES;
        if (GetFileAttributesW(L"C:\Windows\Fonts\malgun.ttf") != INVALID_FILE_ATTRIBUTES)
        {
            ImFontConfig config;
            config.OversampleH = 2;
            config.OversampleV = 2;
            // 첫 번째로 얹은 폰트가 기본값이 된다. 본문이 기본이어야 한다.
            g_fontBody = io.Fonts->AddFontFromFileTTF(regular, 15.5f, &config);
            g_fontTitle = io.Fonts->AddFontFromFileTTF(hasBold ? bold : regular, 19.0f, &config);
            g_fontLabel = io.Fonts->AddFontFromFileTTF(hasBold ? bold : regular, 12.0f, &config);
            g_fontSmall = io.Fonts->AddFontFromFileTTF(regular, 13.0f, &config);
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
        // 고정 크기다. 예전엔 GetFrameHeight()에 비례해서 27x48쯤 되는 바람에, 토글 열 개가 세로로
        // 쌓히면 채도 높은 파랑 알약이 패널을 지배했다. 작게 잡으면 같은 정보량이 훨씬 조용해진다.
        constexpr float kHeight = 20.0f;
        constexpr float kWidth = 36.0f;
        const ImVec2 position(window->DC.CursorPos.x,
                              window->DC.CursorPos.y + (ImGui::GetFrameHeight() - kHeight) * 0.5f);
        const ImRect boundingBox(position, ImVec2(position.x + kWidth, position.y + kHeight));

        ImGui::ItemSize(ImVec2(kWidth, ImGui::GetFrameHeight()));
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
        animation += (target - animation) * ImMin(g.IO.DeltaTime * 14.0f, 1.0f);
        storage->SetFloat(id, animation);

        const UiTheme::Palette& palette = UiTheme::Current();
        const ImU32 background = *value ? (held ? palette.accentHovered : palette.accent)
                                        : (hovered ? palette.toggleOffHovered : palette.toggleOff);
        window->DrawList->AddRectFilled(boundingBox.Min, boundingBox.Max, background, kHeight * 0.5f);
        const float radius = kHeight * 0.5f - 2.5f;
        const ImVec2 thumb(boundingBox.Min.x + radius + 2.5f + animation * (kWidth - kHeight),
                           boundingBox.Min.y + kHeight * 0.5f);
        // thumb 밑에 아주 약한 그림자를 깔아야 동그라미가 트랙 위에 얹혀 보인다.
        window->DrawList->AddCircleFilled(ImVec2(thumb.x, thumb.y + 1.0f), radius,
                                          UiTheme::WithAlpha(IM_COL32(0, 0, 0, 255), 28));
        window->DrawList->AddCircleFilled(thumb, radius, palette.toggleThumb);
        return pressed;
    }

    bool FilledSliderFloat(const char* label, float* value, float minimum, float maximum, const char* format)
    {
        if (!value || maximum <= minimum || !format)
            return false;

        // 예전 슬라이더는 트랙이 16px 높이에 폭을 꽉 채우고 값만큼 진한 파랑으로 칠해져서, 컨트롤이 아니라
        // 로딩 바처럼 보였다. 트랙을 얇게(4px) 깔고 그랩을 원으로 띄우면 같은 기능이 훨씬 가볍게 읽힌다.
        ImGui::PushID(label);
        const UiTheme::Palette& palette = UiTheme::Current();
        char valueText[64]{};
        snprintf(valueText, sizeof(valueText), format, *value);

        const float width = RowWidth();
        const float startX = ImGui::GetCursorPosX();

        // 라벨과 값은 같은 줄에 두고 값은 오른쪽 정렬한다. 세로 공간이 한 줄 줄어든다.
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        PushFontScaled(g_fontSmall);
        const float valueWidth = ImGui::CalcTextSize(valueText).x;
        ImGui::SetCursorPosX(startX + width - valueWidth);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(palette.textMuted));
        ImGui::TextUnformatted(valueText);
        ImGui::PopStyleColor();
        ImGui::PopFont();

        constexpr float kHitHeight = 18.0f;
        constexpr float kTrackHeight = 4.0f;
        constexpr float kGrabRadius = 7.0f;
        ImGui::SetCursorPosX(startX);
        // 히트 영역은 트랙보다 두껍게 잡아야 4px 선을 정확히 집지 않아도 잡힌다.
        ImGui::InvisibleButton("##track", ImVec2(width, kHitHeight));

        const ImVec2 itemMinimum = ImGui::GetItemRectMin();
        const ImVec2 itemMaximum = ImGui::GetItemRectMax();
        const float usable = (std::max)(1.0f, (itemMaximum.x - kGrabRadius) - (itemMinimum.x + kGrabRadius));
        bool changed = false;
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            const float normalized =
                std::clamp((ImGui::GetIO().MousePos.x - (itemMinimum.x + kGrabRadius)) / usable, 0.0f, 1.0f);
            const float newValue = minimum + normalized * (maximum - minimum);
            changed = newValue != *value;
            *value = newValue;
        }

        const float normalized = std::clamp((*value - minimum) / (maximum - minimum), 0.0f, 1.0f);
        const float centerY = itemMinimum.y + kHitHeight * 0.5f;
        const float trackLeft = itemMinimum.x + kGrabRadius;
        const float grabX = trackLeft + normalized * usable;
        const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemActive();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        drawList->AddRectFilled(ImVec2(itemMinimum.x, centerY - kTrackHeight * 0.5f),
                                ImVec2(itemMaximum.x, centerY + kTrackHeight * 0.5f),
                                palette.controlBackground, kTrackHeight * 0.5f);
        if (grabX > itemMinimum.x)
        {
            drawList->AddRectFilled(ImVec2(itemMinimum.x, centerY - kTrackHeight * 0.5f),
                                    ImVec2(grabX, centerY + kTrackHeight * 0.5f), palette.accent,
                                    kTrackHeight * 0.5f);
        }
        drawList->AddCircleFilled(ImVec2(grabX, centerY + 1.0f), kGrabRadius,
                                  UiTheme::WithAlpha(IM_COL32(0, 0, 0, 255), 30));
        drawList->AddCircleFilled(ImVec2(grabX, centerY), kGrabRadius, palette.toggleThumb);
        drawList->AddCircle(ImVec2(grabX, centerY), kGrabRadius,
                            hovered ? palette.accent : UiTheme::WithAlpha(palette.accent, 150), 0, 1.6f);

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
