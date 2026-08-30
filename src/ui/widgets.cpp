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
        ImFont* g_fontLabel = nullptr;  // 섹션 제목용 소형 굵은 자체
        ImFont* g_fontSmall = nullptr;  // 부연 설명·값 표시
        ImFont* g_fontMono = nullptr;   // 런타임 통계의 숫자 전용

        bool g_runtimeStatsOpen = false;

        // 라벨은 왼쪽 끝, 토글은 오른쪽 끝에 붙는다. 본문 폭을 창 폭에 맡기면 넓은 창에서 그 사이가
        // 600px 넘게 벌어져서 어느 토글이 어느 라벨의 것인지 눈으로 잇기 어려워진다. 설정 본문만
        // 여기서 끊고, 남는 폭은 여백으로 둔다.
        constexpr float kMaxContentWidth = 660.0f;

        void PushFontScaled(ImFont* font)
        {
            ImGui::PushFont(font ? font : ImGui::GetFont(), 0.0f);
        }

        float RowWidth()
        {
            return (std::min)(kMaxContentWidth, (std::max)(1.0f, ImGui::GetContentRegionAvail().x));
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
                                    hovered ? palette.surfaceHovered : palette.surface, 7.0f);
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

        // 카드를 걷어냈다. 설정 여덟 개를 보여주는 화면에 둥근 박스가 세 겹씩 겹치면 UI가 실제
        // 정보량의 서너 배로 무거워 보인다. 구역은 이제 섹션 제목 + 얇은 규칙선 + 여백이 만든다.
        void SectionHeader(const char* title)
        {
            if (!title || !*title)
                return;

            const UiTheme::Palette& palette = UiTheme::Current();
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            PushFontScaled(g_fontLabel);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(palette.textMuted));
            ImGui::TextUnformatted(title);
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0.0f, 3.0f));
            const ImVec2 rule = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(rule, ImVec2(rule.x + RowWidth(), rule.y),
                                                UiTheme::WithAlpha(palette.border, 190));
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
        }

        // 섹션 제목 바로 아래 붙는 한 줄 설명. 실험적 기능처럼 "이건 평범한 표시 옵션이 아니다"를
        // 미리 말해 둬야 하는 구역에만 쓴다.
        void SectionNote(const char* text)
        {
            if (!text || !*text)
                return;

            const UiTheme::Palette& palette = UiTheme::Current();
            PushFontScaled(g_fontSmall);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(palette.textSubtle));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth());
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
        }

        // 설정 한 줄. 항목마다 배경도 테두리도 없고, 행 사이 간격만으로 목록이 된다.
        // note가 있으면 라벨 아래 작은 회색 한 줄이 붙는다 - 항목 하나에만 해당하는 주의사항은
        // 별도 박스가 아니라 그 행에 딸려 있어야 어느 스위치 얘기인지 바로 읽힌다.
        void ToggleRow(Loc::Str label, const char* id, bool* value, const char* note = nullptr)
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
            if (note && *note)
            {
                const UiTheme::Palette& palette = UiTheme::Current();
                ImGui::SetCursorPosX(startX);
                PushFontScaled(g_fontSmall);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(palette.textSubtle));
                ImGui::PushTextWrapPos(startX + width - kToggleWidth - 16.0f);
                ImGui::TextUnformatted(note);
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }
            ImGui::Dummy(ImVec2(0.0f, 3.0f));
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
                drawList->AddRectFilled(minimum, maximum, palette.surfaceHovered, 7.0f);
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
            const ImVec2 size(width, 34.0f);
            ImGui::PushID(id);
            const bool pressed = ImGui::InvisibleButton("##tab", size);
            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 maximum = ImGui::GetItemRectMax();
            const bool selected = g_activeTab == tab;
            const bool hovered = ImGui::IsItemHovered();
            const UiTheme::Palette& palette = UiTheme::Current();
            // 선택 표시는 아주 옅은 면 + 왼쪽 액센트 바까지다. 예전엔 선택 항목 전체가 진한 파란
            // 블록이라, 화면에서 가장 강한 색이 "지금 어느 탭인지"라는 가장 안 중요한 정보에
            // 붙어 있었다. 글자색도 파랑 대신 본문색으로 올려서 대비만 준다.
            if (selected)
            {
                window->DrawList->AddRectFilled(minimum, maximum, UiTheme::WithAlpha(palette.accent, 26), 6.0f);
                window->DrawList->AddRectFilled(ImVec2(minimum.x, minimum.y + 7.0f),
                                                ImVec2(minimum.x + 2.0f, maximum.y - 7.0f), palette.accent, 1.0f);
            }
            else if (hovered)
            {
                window->DrawList->AddRectFilled(minimum, maximum, palette.surfaceHovered, 6.0f);
            }
            window->DrawList->AddText(ImVec2(minimum.x + 14.0f,
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

        // 언어와 테마는 내비게이션이 아니다. 사이드바에 두면 Aimbot/ESP/Misc/Debug와 같은 계층으로
        // 읽혀서 섹션이 여섯 개인 것처럼 보인다. 창 하단 푸터로 내리고, 생김새도 채워진 박스가 아니라
        // 글자 + 캐럿만 있는 조용한 버튼으로 줄였다.
        float CompactPickerWidth(const char* value)
        {
            return ImGui::CalcTextSize(value).x + 42.0f;
        }

        bool CompactPicker(const char* id, const char* value)
        {
            const UiTheme::Palette& palette = UiTheme::Current();
            const ImVec2 size(CompactPickerWidth(value), 26.0f);
            ImGui::PushID(id);
            const bool pressed = ImGui::InvisibleButton("##picker", size);
            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 maximum = ImGui::GetItemRectMax();
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            if (hovered)
                drawList->AddRectFilled(minimum, maximum, palette.surfaceHovered, 6.0f);
            drawList->AddText(ImVec2(minimum.x + 10.0f, minimum.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f),
                              hovered ? palette.text : palette.textMuted, value);
            const float caretX = maximum.x - 20.0f;
            const float caretY = minimum.y + size.y * 0.5f - 1.0f;
            drawList->AddTriangleFilled(ImVec2(caretX, caretY), ImVec2(caretX + 9.0f, caretY),
                                        ImVec2(caretX + 4.5f, caretY + 5.0f), palette.textSubtle);
            ImGui::PopID();
            return pressed;
        }

        void DrawLanguagePicker(Features::Settings& settings)
        {
            const char* current = settings.ui.language == Features::Language::English
                                      ? Loc::Text(Loc::Str::English)
                                      : Loc::Text(Loc::Str::Korean);
            ImGui::PushID("language_picker");
            if (CompactPicker("button", current))
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
            const bool light = settings.ui.theme == Features::Theme::Light;
            const char* current = light ? Loc::Text(Loc::Str::Light) : Loc::Text(Loc::Str::Dark);
            ImGui::PushID("theme_picker");
            if (CompactPicker("button", current))
                settings.ui.theme = light ? Features::Theme::Dark : Features::Theme::Light;
            ImGui::PopID();
        }

        void DrawSidebar()
        {
            const UiTheme::Palette& palette = UiTheme::Current();
            ImGuiWindow* window = ImGui::GetCurrentWindow();

            // 브랜드 표시: 액센트 사각형 하나 + 굵은 제목.
            const ImVec2 brand = ImGui::GetCursorScreenPos();
            window->DrawList->AddRectFilled(ImVec2(brand.x, brand.y + 3.0f), ImVec2(brand.x + 3.0f, brand.y + 20.0f),
                                            palette.accent, 1.5f);
            PushFontScaled(g_fontTitle);
            window->DrawList->AddText(ImVec2(brand.x + 12.0f, brand.y), palette.text,
                                      Loc::Text(Loc::Str::BrandName));
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0.0f, 34.0f));

            SidebarTab("aimbot", Loc::Str::TabAimbot, Tab::Aimbot);
            SidebarTab("esp", Loc::Str::TabEsp, Tab::Esp);
            SidebarTab("misc", Loc::Str::TabMisc, Tab::Misc);

            // 디버그는 기능 탭이 아니라 도구다. 얇은 선 하나로 갈라 둔다.
            ImGui::Dummy(ImVec2(0.0f, 5.0f));
            const ImVec2 rule = ImGui::GetCursorScreenPos();
            window->DrawList->AddLine(rule, ImVec2(rule.x + ImGui::GetContentRegionAvail().x, rule.y),
                                      UiTheme::WithAlpha(palette.border, 170));
            ImGui::Dummy(ImVec2(0.0f, 5.0f));

            SidebarTab("debug", Loc::Str::TabDebug, Tab::Debug);
        }

        void DrawFooter(Features::Settings& settings)
        {
            const UiTheme::Palette& palette = UiTheme::Current();
            const float width = (std::max)(1.0f, ImGui::GetContentRegionAvail().x);
            const ImVec2 top = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(top, ImVec2(top.x + width, top.y),
                                                UiTheme::WithAlpha(palette.border, 170));
            ImGui::Dummy(ImVec2(0.0f, 5.0f));

            const float startX = ImGui::GetCursorPosX();
            DrawLanguagePicker(settings);
            ImGui::SameLine(0.0f, 4.0f);
            DrawThemePicker(settings);

            // 단축키 안내는 오른쪽 끝에 작게. 항상 같은 자리에 있으면 읽지 않아도 방해가 안 된다.
            ImGui::SameLine();
            PushFontScaled(g_fontSmall);
            const char* hotkeys = Loc::Text(Loc::Str::FooterHotkeys);
            ImGui::SetCursorPosX(startX + width - ImGui::CalcTextSize(hotkeys).x);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(palette.textSubtle));
            ImGui::TextUnformatted(hotkeys);
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }

        // 페이지 본체는 배경도 테두리도 없는 스크롤 영역일 뿐이다. 스크롤바가 제목까지 밀어내지
        // 않도록 제목/설명은 이 영역 바깥에 둔다.
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
            SectionHeader(Loc::Text(Loc::Str::CardGeneral));
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

            SectionHeader(Loc::Text(Loc::Str::CardTargeting));
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

            SectionHeader(Loc::Text(Loc::Str::CardAiming));
            ToggleRow(Loc::Str::DrawFovCircle, "fov_circle", &settings.aimbot.drawFovCircle);
            FilledSliderFloat(Loc::Text(Loc::Str::FovRadius), &settings.aimbot.fovRadiusDegrees, 1.0f, 60.0f,
                              Loc::Text(Loc::Str::DegreesFormat));
            if (!settings.aimbot.silentAim)
                FilledSliderFloat(Loc::Text(Loc::Str::Smoothing), &settings.aimbot.smoothing, 0.0f, 30.0f,
                                  Loc::Text(Loc::Str::DecimalFormat));
            FilledSliderFloat(Loc::Text(Loc::Str::AimDistance), &settings.aimbot.maxDistanceMeters, 10.0f,
                              300.0f, Loc::Text(Loc::Str::MetersFormat));
        }

        void DrawEsp(Features::Settings& settings)
        {
            SectionHeader(Loc::Text(Loc::Str::CardGeneral));
            ToggleRow(Loc::Str::EspEnabled, "esp_enabled", &settings.esp.enabled);

            SectionHeader(Loc::Text(Loc::Str::CardVisuals));
            ToggleRow(Loc::Str::BoundingBoxes, "boxes", &settings.esp.boundingBoxes);
            ToggleRow(Loc::Str::Skeleton, "skeleton", &settings.esp.skeleton);
            ToggleRow(Loc::Str::HealthBars, "health", &settings.esp.healthBars);
            ToggleRow(Loc::Str::NativeHighlight, "native", &settings.esp.nativeHighlight);

            SectionHeader(Loc::Text(Loc::Str::CardFilters));
            ToggleRow(Loc::Str::Civilians, "civilians", &settings.esp.showCivilians);
            ToggleRow(Loc::Str::Enemies, "enemies", &settings.esp.showEnemies);
            ToggleRow(Loc::Str::Police, "police", &settings.esp.showPolice);
            ToggleRow(Loc::Str::Unclassified, "unclassified", &settings.esp.showUnclassified);
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
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
        }

        void DrawMisc(Features::Settings& settings)
        {
            SectionHeader(Loc::Text(Loc::Str::CardWeapon));
            ToggleRow(Loc::Str::NoRecoil, "no_recoil", &settings.misc.noRecoil);
            ToggleRow(Loc::Str::NoSpread, "no_spread", &settings.misc.noSpread);
        }

        // 런타임 통계는 설정이 아니다. 예전에는 마지막 토글 아래에 원시 텍스트가 그대로 이어져서
        // 설정의 연장처럼 보였다. 이제 접히는 구역 안의 별도 패널이고, 숫자만 고정폭으로 찍는다.
        struct StatCell
        {
            char value[40];
            const char* label;
        };

        void StatValue(StatCell& cell, unsigned long long number, const char* label)
        {
            snprintf(cell.value, sizeof(cell.value), "%llu", number);
            cell.label = label;
        }

        // 숫자 + 이름 쌍을 폭에 맞춰 흘려 넣는다. "registered 68 | positioned 68 | ..." 처럼 한 줄에
        // 파이프로 이어 붙이면 어디가 값이고 어디가 이름인지 매번 다시 읽어야 한다.
        void StatFlow(const StatCell* cells, int count)
        {
            if (count <= 0)
                return;

            const UiTheme::Palette& palette = UiTheme::Current();
            const float width = RowWidth();
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            constexpr float kLineHeight = 21.0f;
            constexpr float kValueGap = 6.0f;
            constexpr float kCellGap = 24.0f;
            float x = 0.0f;
            float y = 0.0f;

            for (int index = 0; index < count; ++index)
            {
                PushFontScaled(g_fontMono);
                const float valueWidth = ImGui::CalcTextSize(cells[index].value).x;
                ImGui::PopFont();
                PushFontScaled(g_fontSmall);
                const float labelWidth = ImGui::CalcTextSize(cells[index].label).x;
                ImGui::PopFont();
                const float cellWidth = valueWidth + kValueGap + labelWidth;

                if (x > 0.0f && x + cellWidth > width)
                {
                    x = 0.0f;
                    y += kLineHeight;
                }

                PushFontScaled(g_fontMono);
                drawList->AddText(ImVec2(origin.x + x, origin.y + y), palette.text, cells[index].value);
                ImGui::PopFont();
                PushFontScaled(g_fontSmall);
                drawList->AddText(ImVec2(origin.x + x + valueWidth + kValueGap, origin.y + y + 1.0f),
                                  palette.textMuted, cells[index].label);
                ImGui::PopFont();

                x += cellWidth + kCellGap;
            }

            ImGui::Dummy(ImVec2(width, y + kLineHeight));
        }

        // 통계 그룹 제목 + 상태 배지. 배지는 "hooked"/"unavailable"처럼 숫자가 아닌 상태를 담는다.
        void StatGroup(const char* title, const char* status, bool statusGood)
        {
            const UiTheme::Palette& palette = UiTheme::Current();
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            const ImVec2 position = ImGui::GetCursorScreenPos();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            PushFontScaled(g_fontLabel);
            drawList->AddText(position, palette.textMuted, title);
            const float titleWidth = ImGui::CalcTextSize(title).x;
            ImGui::PopFont();
            if (status && *status)
            {
                PushFontScaled(g_fontSmall);
                const ImVec2 statusSize = ImGui::CalcTextSize(status);
                const ImVec2 badgeMinimum(position.x + titleWidth + 10.0f, position.y - 1.0f);
                const ImVec2 badgeMaximum(badgeMinimum.x + statusSize.x + 12.0f, badgeMinimum.y + statusSize.y + 3.0f);
                const ImU32 tint = statusGood ? palette.success : palette.warning;
                drawList->AddRectFilled(badgeMinimum, badgeMaximum, UiTheme::WithAlpha(tint, 34), 4.0f);
                drawList->AddText(ImVec2(badgeMinimum.x + 6.0f, badgeMinimum.y + 1.0f), tint, status);
                ImGui::PopFont();
            }
            ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() + 3.0f));
        }

        void DrawRuntimeStatistics()
        {
            const Game::EntityTracker::Stats entityStats = Game::EntityTracker::GetStats();
            const Game::PlayerModifiers::Stats modifierStats = Game::PlayerModifiers::GetStats();
            const Aimbot::Stats aimStats = Aimbot::GetStats();
            const Game::Visibility::Stats visibilityStats = Game::Visibility::GetStats();
            const Game::SilentAim::DiagnosticsSnapshot silentStats = Game::SilentAim::GetDiagnostics();

            StatCell cells[8]{};

            StatGroup("Entity feed", entityStats.hookCreated ? "hooked" : "unavailable", entityStats.hookCreated);
            StatValue(cells[0], entityStats.registered, "registered");
            StatValue(cells[1], entityStats.positioned, "positioned");
            StatValue(cells[2], entityStats.puppets, "NPCs");
            StatValue(cells[3], entityStats.trackedPuppets, "live");
            StatFlow(cells, 4);

            StatGroup("Classified live", nullptr, true);
            StatValue(cells[0], entityStats.trackedCivilians, "civilian");
            StatValue(cells[1], entityStats.trackedEnemies, "enemy");
            StatValue(cells[2], entityStats.trackedPolice, "police");
            StatValue(cells[3], entityStats.trackedHostile, "hostile");
            StatFlow(cells, 4);

            StatGroup("Attitude feed", nullptr, true);
            StatValue(cells[0], entityStats.attitudeValid, "resolved");
            StatValue(cells[1], entityStats.attitudeInvalid, "unavailable");
            StatFlow(cells, 2);

            StatGroup("Health feed", nullptr, true);
            StatValue(cells[0], entityStats.healthValid, "valid");
            StatValue(cells[1], entityStats.healthInvalid, "fallback");
            StatValue(cells[2], entityStats.pendingPosition, "pending position");
            StatFlow(cells, 3);

            StatGroup("Native highlight", nullptr, true);
            StatValue(cells[0], entityStats.nativeHighlightQueued, "queued");
            StatValue(cells[1], entityStats.nativeHighlightCleared, "cleared");
            StatValue(cells[2], entityStats.nativeHighlightFailures, "failures");
            StatFlow(cells, 3);

            StatGroup("No recoil",
                      modifierStats.active ? "active" : (modifierStats.available ? "ready" : "unavailable"),
                      modifierStats.active || modifierStats.available);
            snprintf(cells[0].value, sizeof(cells[0].value), "0x%llX",
                     static_cast<unsigned long long>(modifierStats.targetId));
            cells[0].label = modifierStats.usingWeaponTarget ? "weapon target" : "player fallback";
            StatValue(cells[1], modifierStats.applied, "applied");
            StatValue(cells[2], modifierStats.removed, "removed");
            StatValue(cells[3], modifierStats.retiredOwnerResets, "retired");
            StatValue(cells[4], modifierStats.failures, "failures");
            StatFlow(cells, 5);

            StatGroup("Targets", nullptr, true);
            StatValue(cells[0], aimStats.candidates, "candidates");
            StatValue(cells[1], aimStats.eligible, "eligible");
            StatValue(cells[2], aimStats.skippedNoHealthPool, "no pool");
            StatValue(cells[3], aimStats.skippedHealthCap, "over cap");
            StatValue(cells[4], aimStats.skippedOccluded, "occluded");
            StatFlow(cells, 5);
            if (aimStats.targetEntityId != 0)
            {
                snprintf(cells[0].value, sizeof(cells[0].value), "0x%llX",
                         static_cast<unsigned long long>(aimStats.targetEntityId));
                cells[0].label = "selected";
                snprintf(cells[1].value, sizeof(cells[1].value), "%.0f / %.0f", aimStats.targetHealth,
                         aimStats.targetHealthMax);
                cells[1].label = aimStats.targetHealthValid ? "health" : "health (pool unresolved)";
                StatFlow(cells, 2);
            }

            StatGroup("Visibility cache", visibilityStats.available ? "ready" : "unavailable",
                      visibilityStats.available);
            StatValue(cells[0], visibilityStats.visible, "visible");
            StatValue(cells[1], visibilityStats.occluded, "occluded");
            StatValue(cells[2], visibilityStats.dropped, "dropped");
            StatFlow(cells, 3);

            StatGroup("Silent aim",
                      silentStats.crosshairCoreHookCreated ? "crosshair core hooked" : "unavailable",
                      silentStats.crosshairCoreHookCreated);
            StatValue(cells[0], silentStats.nativeCrosshairCoreRedirects, "redirects");
            StatValue(cells[1], silentStats.rejectedShots, "rejected");
            StatFlow(cells, 2);

            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            if (DisclosureHeader("silent_diagnostics", Loc::Str::SilentAimDiagnostics, &g_silentDiagnosticsOpen))
            {
                StatGroup("Crosshair core", nullptr, true);
                StatValue(cells[0], silentStats.nativeCrosshairCoreCalls, "calls");
                StatValue(cells[1], silentStats.nativeCrosshairCoreRedirects, "redirects");
                StatFlow(cells, 2);

                if (silentStats.producerHooks == 0 && silentStats.listenerHooks == 0)
                {
                    StatGroup("Observation hooks", "disabled in this build", false);
                }
                else
                {
                    StatGroup("Observation hooks", nullptr, true);
                    StatValue(cells[0], silentStats.producerHooks, "producer");
                    StatValue(cells[1], silentStats.listenerHooks, "projectile");
                    StatValue(cells[2], silentStats.effectRuns, "effect runs");
                    StatValue(cells[3], silentStats.attackStarts, "attack starts");
                    StatValue(cells[4], silentStats.attackPrepares, "attack prepares");
                    StatFlow(cells, 5);
                    StatValue(cells[0], silentStats.crosshairCalls, "crosshair");
                    StatValue(cells[1], silentStats.defaultCrosshairCalls, "default crosshair");
                    StatValue(cells[2], silentStats.projectileEvents, "projectile events");
                    StatValue(cells[3], silentStats.localPlayerEvents, "local player events");
                    StatFlow(cells, 4);
                }
            }
        }

        void DrawDebug(Features::Settings& settings)
        {
            SectionHeader(Loc::Text(Loc::Str::CardOverlay));
            ToggleRow(Loc::Str::ShowFps, "show_fps", &settings.debug.showFps);
            ToggleRow(Loc::Str::ShowGraph, "show_graph", &settings.debug.showGraph);
            ToggleRow(Loc::Str::ShowInternalStats, "show_stats", &settings.debug.showInternalStats);

            // 이 섹션의 토글은 전부 켜면 비용을 내는 것들이다. 켜져 있을 때만 경고 박스를 붙인다 -
            // 꺼져 있는 항목까지 경고를 달면 화면이 경고로 뒤덮여서 진짜 경고가 안 읽힌다.
            SectionHeader(Loc::Text(Loc::Str::CardDiagnostics));
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

            SectionHeader(Loc::Text(Loc::Str::CardExperimental));
            SectionNote(Loc::Text(Loc::Str::ExperimentalNote));
            ToggleRow(Loc::Str::HeadlessAimbot, "headless_aimbot", &settings.debug.headlessAimbot,
                      Loc::Text(Loc::Str::HeadlessAimbotNote));
            if (settings.debug.headlessAimbot)
                WarningBox(Loc::Text(Loc::Str::HeadlessAimbotHint));

            if (settings.debug.showInternalStats)
            {
                ImGui::Dummy(ImVec2(0.0f, 12.0f));
                if (DisclosureHeader("runtime_statistics", Loc::Str::RuntimeStatistics, &g_runtimeStatsOpen))
                    DrawRuntimeStatistics();
            }
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
            ImGui::Dummy(ImVec2(0.0f, 2.0f));

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

    namespace
    {
        // 폰트 하나를 찾을 때까지 후보 경로를 훑는다. 문자열 리터럴 안의 역슬래시는 반드시 두 번
        // 써야 한다 - 직전 판에서 "C:\Windows\..."로 한 번만 쓴 탓에 MSVC가 \W/\F를 알 수 없는
        // 이스케이프로 접어서 "C:WindowsFonts..."가 됐고, 폰트 파일을 못 찾은 오버레이가 기본
        // ProggyClean 비트맵 폰트로 폴백해 UI 전체가 픽셀 폰트로 보이고 한글이 전부 "??"로 깨졌다.
        const wchar_t* FindFontFile(const wchar_t* const* candidates, int count, char* out, int outSize)
        {
            for (int index = 0; index < count; ++index)
            {
                if (GetFileAttributesW(candidates[index]) == INVALID_FILE_ATTRIBUTES)
                    continue;
                // ImGui의 파일 로더는 UTF-8 경로를 받는다.
                if (WideCharToMultiByte(CP_UTF8, 0, candidates[index], -1, out, outSize, nullptr, nullptr) > 0)
                    return candidates[index];
            }
            out[0] = '\0';
            return nullptr;
        }

        // %LOCALAPPDATA%\cbpk\fonts\<name> 을 만든다. config.ini가 있는 디렉터리와 같은 뿌리다.
        // tools/scripts/fetch_fonts.py가 Pretendard를 여기에 내려받는다.
        void LocalFontPath(const wchar_t* name, wchar_t* out, size_t outCount)
        {
            out[0] = L'\0';
            wchar_t local[MAX_PATH]{};
            const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
            if (length == 0 || length >= MAX_PATH)
                return;
            _snwprintf_s(out, outCount, _TRUNCATE, L"%s\\cbpk\\fonts\\%s", local, name);
        }
    }

    void ApplyStyle()
    {
        // 폰트가 이 UI에서 가장 큰 변수다 (AGENTS.md: "ImGui가 투박해 보이는 원인의 8할이 폰트").
        // 우선순위는 Pretendard → 맑은 고딕 → ImGui 기본 폰트다. Pretendard는 OFL 라이선스에 웨이트당
        // 2 MB가 넘어서 저장소에 넣지 않고 tools/scripts/fetch_fonts.py로 사용자 디렉터리에 받는다.
        // 못 찾으면 Windows에 항상 있는 맑은 고딕으로 조용히 내려간다 - 한글이 깨지지 않는 것이
        // 조판보다 우선이라, 한글 글리프가 없는 Segoe UI는 본문 후보로 쓰지 않는다.
        //
        // 크기는 네 단계다. 전부 한 크기로 그리면 제목과 항목 이름과 부연 설명이 같은 무게로 읽혀서
        // 눈이 어디를 먼저 볼지 못 정한다. 굵은 자체가 있으면 제목/섹션 라벨에만 쓴다.
        ImGuiIO& io = ImGui::GetIO();

        wchar_t pretendardRegular[MAX_PATH]{};
        wchar_t pretendardSemiBold[MAX_PATH]{};
        LocalFontPath(L"Pretendard-Regular.ttf", pretendardRegular, MAX_PATH);
        LocalFontPath(L"Pretendard-SemiBold.ttf", pretendardSemiBold, MAX_PATH);

        const wchar_t* regularCandidates[] = {
            pretendardRegular,
            L"C:\\Windows\\Fonts\\Pretendard-Regular.ttf",
            L"C:\\Windows\\Fonts\\malgun.ttf",
        };
        const wchar_t* boldCandidates[] = {
            pretendardSemiBold,
            L"C:\\Windows\\Fonts\\Pretendard-SemiBold.ttf",
            L"C:\\Windows\\Fonts\\malgunbd.ttf",
            L"C:\\Windows\\Fonts\\malgun.ttf",
        };
        // 숫자만 고정폭으로 찍으면 통계가 개발자 도구처럼 읽히고 자릿수가 흔들리지 않는다.
        // 본문까지 고정폭으로 그리면 안 된다 - 그게 화면 전체를 임시 툴처럼 보이게 만든 것이다.
        const wchar_t* monoCandidates[] = {
            L"C:\\Windows\\Fonts\\consola.ttf",
            L"C:\\Windows\\Fonts\\CascadiaMono.ttf",
            L"C:\\Windows\\Fonts\\cour.ttf",
        };

        char regularPath[MAX_PATH * 3]{};
        char boldPath[MAX_PATH * 3]{};
        char monoPath[MAX_PATH * 3]{};
        FindFontFile(regularCandidates, IM_ARRAYSIZE(regularCandidates), regularPath, sizeof(regularPath));
        FindFontFile(boldCandidates, IM_ARRAYSIZE(boldCandidates), boldPath, sizeof(boldPath));
        FindFontFile(monoCandidates, IM_ARRAYSIZE(monoCandidates), monoPath, sizeof(monoPath));
        if (boldPath[0] == '\0')
            memcpy(boldPath, regularPath, sizeof(regularPath));

        if (regularPath[0] != '\0')
        {
            ImFontConfig config;
            config.OversampleH = 2;
            config.OversampleV = 2;
            // 첫 번째로 얹은 폰트가 기본값이 된다. 본문이 기본이어야 한다.
            g_fontBody = io.Fonts->AddFontFromFileTTF(regularPath, 15.0f, &config);
            g_fontTitle = io.Fonts->AddFontFromFileTTF(boldPath, 19.0f, &config);
            g_fontLabel = io.Fonts->AddFontFromFileTTF(boldPath, 12.5f, &config);
            g_fontSmall = io.Fonts->AddFontFromFileTTF(regularPath, 13.0f, &config);
            if (monoPath[0] != '\0')
                g_fontMono = io.Fonts->AddFontFromFileTTF(monoPath, 13.0f, &config);
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
                                        : (hovered ? UiTheme::WithAlpha(palette.textSubtle, 130) : palette.border);
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
                                palette.surface, kTrackHeight * 0.5f);
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
        drawList->AddRectFilled(minimum, maximum, UiTheme::WithAlpha(palette.surface, alpha), 14.0f);
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
        drawList->AddRectFilled(keyMinimum, keyMaximum, UiTheme::WithAlpha(palette.surface, alpha), 6.0f);
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
        ImGui::SetNextWindowSize(ImVec2(880.0f, 660.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(720.0f, 520.0f), ImVec2(1250.0f, 1050.0f));
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

        // 사이드바는 자기 배경면으로 구분된다. 그래서 테두리도 카드도 필요 없다 - 좋은 다크 UI는
        // 선이 아니라 명도 차로 구역을 만든다.
        constexpr float kSidebarWidth = 164.0f;
        constexpr float kFooterHeight = 42.0f;
        drawList->AddRectFilled(ImVec2(windowMinimum.x + 3.0f, windowMinimum.y + 5.0f),
                                ImVec2(windowMaximum.x + 3.0f, windowMaximum.y + 5.0f), palette.shadow, 12.0f);
        const float bodyHeight = (std::max)(1.0f, windowSize.y - kFooterHeight);
        // 사이드바 면은 푸터 위에서 끝난다. 푸터는 창 폭을 가로지르는 한 줄이지 사이드바의 일부가
        // 아니고, 여기서 색을 끊어야 그렇게 읽힌다.
        drawList->AddRectFilled(windowMinimum, ImVec2(windowMinimum.x + kSidebarWidth, windowMinimum.y + bodyHeight),
                                palette.sidebarBackground, 12.0f, ImDrawFlags_RoundCornersTopLeft);

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 18.0f));
        ImGui::BeginChild("##sidebar", ImVec2(kSidebarWidth, bodyHeight), false, ImGuiWindowFlags_NoBackground);
        DrawSidebar();
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26.0f, 20.0f));
        ImGui::BeginChild("##content_panel", ImVec2(0.0f, bodyHeight), false, ImGuiWindowFlags_NoBackground);
        DrawPage(settings);
        ImGui::EndChild();
        ImGui::PopStyleVar();

        // 푸터는 창 전체 폭을 쓴다. 언어와 테마가 사이드바에 있었을 때는 기능 탭과 같은 계층으로
        // 읽혔는데, 여기 내려오면 "창 설정"이라는 제자리를 찾는다.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 0.0f));
        ImGui::BeginChild("##footer", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_NoBackground);
        DrawFooter(settings);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleVar();

        ImGui::End();
    }
}
