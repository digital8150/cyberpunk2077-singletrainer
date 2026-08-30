#include "ui_kit.h"

#include "localization.h"
#include "theme.h"
#include "../framework.h"  // GetFileAttributesW / GetAsyncKeyState

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace UiKit
{
    namespace
    {
        // 폰트는 세 벌만 만든다 (본문/굵게/고정폭). 1.92부터 ImGui 폰트 크기는 동적이라 역할별 크기는
        // PushFont(font, size)로 지정한다 — 예전처럼 크기마다 ImFont를 따로 얹으면 아틀라스만 커지고,
        // PushFont(font, 0.0f)는 "현재 크기 유지"라서 의도한 크기가 실제로 반영되지도 않았다.
        ImFont* g_regular = nullptr;
        ImFont* g_bold = nullptr;
        ImFont* g_mono = nullptr;

        constexpr int kDisabledStackDepth = 16;
        bool g_disabledStack[kDisabledStackDepth]{};
        int g_disabledStackTop = 0;
        int g_disabledDepth = 0;

        float g_contentWidth = 480.0f;

        struct ColumnState
        {
            int count = 0;
            int current = -1;
            float width = 0.0f;
            float total = 0.0f;
            float indent = 0.0f;
        };
        ColumnState g_columns;

        struct SectionState
        {
            bool active = false;
            ImVec2 origin{};
            float width = 0.0f;
            int rowIndex = 0;
            ImDrawList* drawList = nullptr;
        };
        SectionState g_section;

        // 키 캡처는 동시에 하나만 살아 있어야 한다. 캡처 중인 행의 id를 들고 있지 않으면 다른
        // 바인딩 행이 같은 입력을 함께 삼킨다.
        ImGuiID g_captureId = 0;
        bool g_captureArmed = false;

        // 섹션 패널은 내용보다 먼저 그릴 수 없다 (높이를 미리 모른다). 그래서 드로우리스트를 두
        // 채널로 나누고 배경/구분선/호버는 0번, 글자와 컨트롤은 1번에 넣은 뒤 섹션이 닫힐 때 합친다.
        // 섹션은 중첩하지 않으므로 채널 분할도 중첩되지 않는다.
        ImDrawList* Background()
        {
            if (g_section.active && g_section.drawList)
            {
                g_section.drawList->ChannelsSetCurrent(0);
                return g_section.drawList;
            }
            return ImGui::GetWindowDrawList();
        }

        void EndBackground()
        {
            if (g_section.active && g_section.drawList)
                g_section.drawList->ChannelsSetCurrent(1);
        }

        ImDrawList* Foreground()
        {
            return ImGui::GetWindowDrawList();
        }

        ImU32 TextPrimary()
        {
            const UiTheme::Palette& palette = UiTheme::Current();
            return g_disabledDepth > 0 ? palette.textDisabled : palette.text;
        }

        ImU32 TextSecondary()
        {
            const UiTheme::Palette& palette = UiTheme::Current();
            return g_disabledDepth > 0 ? UiTheme::Mix(palette.textDisabled, palette.background, 0.4f)
                                       : palette.textSecondary;
        }

        float RowWidth()
        {
            return g_section.active ? g_section.width : g_contentWidth;
        }

        float RowInset()
        {
            return g_section.active ? Metrics::kPanelPadding : 0.0f;
        }

        float RowInnerWidth()
        {
            return (std::max)(1.0f, RowWidth() - RowInset() * 2.0f);
        }

        struct RowFrame
        {
            ImVec2 min{};
            ImVec2 max{};
            float innerLeft = 0.0f;
            float innerRight = 0.0f;
            bool hovered = false;
            bool pressed = false;
        };

        void RowDivider()
        {
            if (g_section.rowIndex <= 0)
                return;
            const ImVec2 position = ImGui::GetCursorScreenPos();
            ImDrawList* drawList = Background();
            drawList->AddRectFilled(position, ImVec2(position.x + RowWidth(), position.y + 1.0f),
                                    UiTheme::Current().borderSubtle);
            EndBackground();
            ImGui::Dummy(ImVec2(RowWidth(), 1.0f));
        }

        // 행의 공통 뼈대. 구분선을 깔고 지정한 높이의 사각형을 확보한 뒤 화면 좌표를 돌려준다.
        // 라벨과 컨트롤 그리기는 호출자 몫이다.
        RowFrame BeginRow(const char* id, float height, bool interactive)
        {
            RowDivider();
            ++g_section.rowIndex;

            RowFrame frame;
            const float width = RowWidth();
            frame.min = ImGui::GetCursorScreenPos();
            frame.max = ImVec2(frame.min.x + width, frame.min.y + height);
            frame.innerLeft = frame.min.x + RowInset();
            frame.innerRight = frame.max.x - RowInset();

            if (interactive && g_disabledDepth == 0)
            {
                ImGui::PushID(id);
                frame.pressed = ImGui::InvisibleButton("##row", ImVec2(width, height));
                frame.hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                if (frame.hovered)
                {
                    ImDrawList* drawList = Background();
                    drawList->AddRectFilled(frame.min, frame.max, UiTheme::Current().surfaceHovered);
                    EndBackground();
                }
            }
            else
            {
                ImGui::Dummy(ImVec2(width, height));
            }
            return frame;
        }

        void RowLabel(const RowFrame& frame, const char* label, float reservedRight)
        {
            const float available = (std::max)(24.0f, frame.innerRight - frame.innerLeft - reservedRight);
            const ImVec2 size = MeasureText(Font::Body, label, available);
            const float centerY = (frame.min.y + frame.max.y) * 0.5f;
            PaintText(Foreground(), Font::Body, ImVec2(frame.innerLeft, centerY - size.y * 0.5f),
                      TextPrimary(), label, available);
        }

        // 항목 하나에만 해당하는 설명은 별도 박스가 아니라 그 행 바로 아래 붙는다. 어느 스위치
        // 얘기인지 눈으로 잇지 않아도 되게.
        void RowHelper(const char* text)
        {
            if (!text || !*text)
                return;
            const float width = (std::max)(40.0f, RowInnerWidth() - 46.0f);
            const ImVec2 size = MeasureText(Font::Small, text, width);
            const ImVec2 position = ImGui::GetCursorScreenPos();
            PaintText(Foreground(), Font::Small, ImVec2(position.x + RowInset(), position.y - 3.0f),
                      TextSecondary(), text, width);
            ImGui::Dummy(ImVec2(RowWidth(), size.y + 6.0f));
        }

        void PaintToggle(ImVec2 center, bool value, bool hovered, ImGuiID animationId)
        {
            constexpr float kWidth = 32.0f;
            constexpr float kHeight = 18.0f;
            const UiTheme::Palette& palette = UiTheme::Current();
            const ImVec2 minimum(center.x - kWidth * 0.5f, center.y - kHeight * 0.5f);
            const ImVec2 maximum(minimum.x + kWidth, minimum.y + kHeight);

            float animation = value ? 1.0f : 0.0f;
            if (animationId != 0)
            {
                ImGuiStorage* storage = ImGui::GetCurrentWindow()->DC.StateStorage;
                const float target = animation;
                animation = storage->GetFloat(animationId, target);
                animation += (target - animation) * ImMin(ImGui::GetIO().DeltaTime * 16.0f, 1.0f);
                storage->SetFloat(animationId, animation);
            }

            // 켬 / 끔 / 비활성이 서로 다른 명도로 갈린다. 비활성은 트랙을 바닥 쪽으로 눌러서
            // "꺼둔 것"이 아니라 "지금 만질 수 없는 것"으로 읽히게 한다.
            ImU32 track;
            ImU32 knob = palette.knob;
            if (g_disabledDepth > 0)
            {
                track = value ? UiTheme::Mix(palette.accent, palette.background, 0.62f)
                              : UiTheme::Mix(palette.controlOff, palette.background, 0.55f);
                knob = UiTheme::Mix(palette.knob, palette.background, 0.55f);
            }
            else if (value)
            {
                track = hovered ? palette.accentHovered : palette.accent;
            }
            else
            {
                track = hovered ? UiTheme::Mix(palette.controlOff, palette.text, 0.2f) : palette.controlOff;
            }

            ImDrawList* drawList = Foreground();
            drawList->AddRectFilled(minimum, maximum, track, kHeight * 0.5f);
            const float radius = kHeight * 0.5f - 2.5f;
            const ImVec2 thumb(minimum.x + radius + 2.5f + animation * (kWidth - kHeight), center.y);
            drawList->AddCircleFilled(ImVec2(thumb.x, thumb.y + 0.75f), radius,
                                      UiTheme::WithAlpha(IM_COL32(0, 0, 0, 255), g_disabledDepth > 0 ? 10 : 30));
            drawList->AddCircleFilled(thumb, radius, knob);
        }

        void PaintCheckbox(ImVec2 center, bool value, bool hovered)
        {
            constexpr float kSize = 16.0f;
            const UiTheme::Palette& palette = UiTheme::Current();
            const ImVec2 minimum(center.x - kSize * 0.5f, center.y - kSize * 0.5f);
            const ImVec2 maximum(minimum.x + kSize, minimum.y + kSize);
            ImDrawList* drawList = Foreground();

            if (value)
            {
                ImU32 fill = hovered ? palette.accentHovered : palette.accent;
                if (g_disabledDepth > 0)
                    fill = UiTheme::Mix(palette.accent, palette.background, 0.62f);
                drawList->AddRectFilled(minimum, maximum, fill, 4.0f);
                const ImU32 mark = g_disabledDepth > 0 ? UiTheme::Mix(palette.knob, fill, 0.45f) : palette.knob;
                drawList->AddLine(ImVec2(minimum.x + 3.6f, center.y + 0.4f),
                                  ImVec2(center.x - 1.0f, maximum.y - 4.2f), mark, 1.9f);
                drawList->AddLine(ImVec2(center.x - 1.4f, maximum.y - 4.2f),
                                  ImVec2(maximum.x - 3.4f, minimum.y + 4.6f), mark, 1.9f);
            }
            else
            {
                const ImU32 fill = g_disabledDepth > 0 ? palette.background
                                                       : (hovered ? palette.surfaceActive : palette.surface);
                const ImU32 outline = g_disabledDepth > 0
                                          ? UiTheme::Mix(palette.border, palette.background, 0.5f)
                                          : (hovered ? palette.textSecondary : palette.controlOff);
                drawList->AddRectFilled(minimum, maximum, fill, 4.0f);
                drawList->AddRect(minimum, maximum, outline, 4.0f, 1.0f);
            }
        }

        // 값 표시 필드 (드롭다운 / 키바인드). 채워진 박스 대신 조용한 면 + 얇은 테두리다.
        void PaintField(ImVec2 minimum, ImVec2 maximum, bool hovered, bool active)
        {
            const UiTheme::Palette& palette = UiTheme::Current();
            ImU32 fill = palette.surface;
            ImU32 outline = palette.border;
            if (g_disabledDepth > 0)
            {
                fill = palette.background;
                outline = UiTheme::Mix(palette.border, palette.background, 0.5f);
            }
            else if (active)
            {
                fill = palette.surfaceActive;
                outline = palette.accent;
            }
            else if (hovered)
            {
                fill = palette.surfaceHovered;
                outline = UiTheme::Mix(palette.border, palette.accent, 0.45f);
            }
            ImDrawList* drawList = Foreground();
            drawList->AddRectFilled(minimum, maximum, fill, 5.0f);
            drawList->AddRect(minimum, maximum, outline, 5.0f, 1.0f);
        }

        void PaintCaret(ImVec2 center, ImU32 color)
        {
            ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(center.x - 4.0f, center.y - 2.0f),
                                                          ImVec2(center.x + 4.0f, center.y - 2.0f),
                                                          ImVec2(center.x, center.y + 2.8f), color);
        }

        bool FindFontFile(const wchar_t* const* candidates, int count, char* out, int outSize)
        {
            for (int index = 0; index < count; ++index)
            {
                if (!candidates[index] || candidates[index][0] == L'\0')
                    continue;
                if (GetFileAttributesW(candidates[index]) == INVALID_FILE_ATTRIBUTES)
                    continue;
                if (WideCharToMultiByte(CP_UTF8, 0, candidates[index], -1, out, outSize, nullptr, nullptr) > 0)
                    return true;
            }
            out[0] = '\0';
            return false;
        }

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

    float FontSize(Font role)
    {
        switch (role)
        {
        case Font::Title: return 17.0f;
        case Font::Section: return 13.0f;
        case Font::Body: return 13.5f;
        case Font::Small: return 12.0f;
        case Font::Micro: return 10.5f;
        case Font::Mono: return 12.0f;
        }
        return 13.5f;
    }

    ImFont* FontFace(Font role)
    {
        switch (role)
        {
        case Font::Mono: return g_mono ? g_mono : g_regular;
        case Font::Title:
        case Font::Section:
        case Font::Micro: return g_bold ? g_bold : g_regular;
        default: break;
        }
        return g_regular;
    }

    void PushFont(Font role)
    {
        ImGui::PushFont(FontFace(role), FontSize(role));
    }

    ImVec2 MeasureText(Font role, const char* text, float wrapWidth)
    {
        PushFont(role);
        const ImVec2 size = ImGui::CalcTextSize(text, nullptr, false, wrapWidth);
        PopFont();
        return size;
    }

    void PaintText(ImDrawList* drawList, Font role, ImVec2 position, ImU32 color, const char* text,
                   float wrapWidth)
    {
        drawList->AddText(FontFace(role), FontSize(role), position, color, text, nullptr, wrapWidth);
    }

    void PopFont()
    {
        ImGui::PopFont();
    }

    void LoadFonts()
    {
        // 우선순위는 Pretendard → 맑은 고딕이다. 한글 글리프가 없는 Segoe UI는 본문 후보로 쓰지
        // 않는다 — 조판보다 한글이 깨지지 않는 것이 먼저다. 문자열 리터럴의 역슬래시는 반드시 두 번
        // 쓴다. 한 번만 쓰면 MSVC가 \W/\F를 알 수 없는 이스케이프로 접어 경로가 깨지고, 폰트를 못
        // 찾은 오버레이가 기본 비트맵 폰트로 떨어진다.
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
        // 고정폭은 런타임 수치와 주소에만 쓴다. 본문까지 고정폭으로 그리면 화면 전체가 임시 툴처럼
        // 보인다.
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
            g_regular = io.Fonts->AddFontFromFileTTF(regularPath, 0.0f, &config);
            g_bold = io.Fonts->AddFontFromFileTTF(boldPath, 0.0f, &config);
            if (monoPath[0] != '\0')
                g_mono = io.Fonts->AddFontFromFileTTF(monoPath, 0.0f, &config);
        }

        ImGui::GetStyle().FontSizeBase = FontSize(Font::Body);
    }

    void BeginFrame()
    {
        UiTheme::ApplyImGuiStyle();
        ImGui::GetStyle().FontSizeBase = FontSize(Font::Body);
        g_disabledDepth = 0;
        g_disabledStackTop = 0;
        g_section = SectionState{};
        g_columns = ColumnState{};
    }

    void BeginDisabled(bool disabled)
    {
        if (g_disabledStackTop >= kDisabledStackDepth)
            return;
        g_disabledStack[g_disabledStackTop++] = disabled;
        if (disabled)
            ++g_disabledDepth;
    }

    void EndDisabled()
    {
        if (g_disabledStackTop <= 0)
            return;
        if (g_disabledStack[--g_disabledStackTop] && g_disabledDepth > 0)
            --g_disabledDepth;
    }

    float ContentWidth()
    {
        return g_contentWidth;
    }

    void SetContentWidth(float width)
    {
        g_contentWidth = (std::max)(1.0f, width);
    }

    int ResolveColumnCount(int desired, float availableWidth)
    {
        int count = (std::max)(1, desired);
        while (count > 1)
        {
            const float width = (availableWidth - Metrics::kColumnGap * (count - 1)) / count;
            if (width >= Metrics::kMinColumnWidth && availableWidth >= Metrics::kTwoColumnMinWidth)
                break;
            --count;
        }
        return count;
    }

    void ColumnsBegin(int count, float totalWidth)
    {
        g_columns.count = (std::max)(1, count);
        g_columns.current = -1;
        g_columns.total = totalWidth;
        g_columns.width = (std::min)(Metrics::kMaxColumnWidth,
                                     (totalWidth - Metrics::kColumnGap * (g_columns.count - 1)) /
                                         static_cast<float>(g_columns.count));
        const float used = g_columns.width * g_columns.count +
                           Metrics::kColumnGap * (g_columns.count - 1);
        g_columns.indent = (std::max)(0.0f, (totalWidth - used) * 0.5f);
    }

    // 열은 자식 창이 아니라 그룹으로 만든다. 자식 창으로 나누면 열마다 스크롤바가 따로 생겨서
    // 페이지가 하나로 읽히지 않는다.
    //
    // 호출자는 페이지에 논리적으로 필요한 만큼 Column()을 부르고, 실제 열 개수는 ColumnsBegin이
    // 폭을 보고 정한다. 그래서 열이 한 개로 접힌 좁은 창에서는 여기서 줄을 바꿔야 한다 — 항상
    // SameLine을 걸면 두 번째 묶음이 창 밖으로 나가서 통째로 보이지 않는다.
    void Column()
    {
        bool sameLine = false;
        if (g_columns.current >= 0)
        {
            ImGui::EndGroup();
            sameLine = ((g_columns.current + 1) % g_columns.count) != 0;
            if (sameLine)
                ImGui::SameLine(0.0f, Metrics::kColumnGap);
        }
        if (!sameLine && g_columns.indent > 0.0f)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + g_columns.indent);
        ++g_columns.current;
        ImGui::BeginGroup();
        SetContentWidth(g_columns.width);
    }

    void ColumnsEnd()
    {
        if (g_columns.current >= 0)
            ImGui::EndGroup();
        g_columns.current = -1;
        SetContentWidth(g_columns.total);
    }

    void PageHeader(Icon icon, const char* title, const char* subtitle)
    {
        const UiTheme::Palette& palette = UiTheme::Current();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        const float centerY = origin.y + Metrics::kHeaderHeight * 0.5f;
        const float badgeSize = 28.0f;
        const ImVec2 badgeMin(origin.x + Metrics::kContentPadding, centerY - badgeSize * 0.5f);
        const ImVec2 badgeMax(badgeMin.x + badgeSize, badgeMin.y + badgeSize);
        drawList->AddRectFilled(badgeMin, badgeMax, palette.surface, 6.0f);
        drawList->AddRect(badgeMin, badgeMax, palette.border, 6.0f, 1.0f);
        DrawIcon(drawList, icon, ImVec2((badgeMin.x + badgeMax.x) * 0.5f, centerY), 15.0f, palette.accent);

        const float textX = badgeMax.x + 12.0f;
        const ImVec2 titleSize = MeasureText(Font::Title, title);
        const ImVec2 subtitleSize = MeasureText(Font::Small, subtitle);
        const float block = titleSize.y + 2.0f + subtitleSize.y;
        PaintText(drawList, Font::Title, ImVec2(textX, centerY - block * 0.5f), palette.text, title);
        PaintText(drawList, Font::Small, ImVec2(textX, centerY - block * 0.5f + titleSize.y + 2.0f),
                  palette.textSecondary, subtitle);

        drawList->AddRectFilled(ImVec2(origin.x, origin.y + Metrics::kHeaderHeight - 1.0f),
                                ImVec2(origin.x + width, origin.y + Metrics::kHeaderHeight), palette.border);
        ImGui::Dummy(ImVec2(width, Metrics::kHeaderHeight));
    }

    namespace
    {
        void SectionOpen(const char* title, const char* tag, ImU32 tagColor)
        {
            const UiTheme::Palette& palette = UiTheme::Current();
            const float width = g_contentWidth;

            if (title && *title)
            {
                const ImVec2 position = ImGui::GetCursorScreenPos();
                const ImVec2 size = MeasureText(Font::Section, title);
                PaintText(ImGui::GetWindowDrawList(), Font::Section, position, palette.text, title);
                if (tag && *tag)
                {
                    PushFont(Font::Micro);
                    const ImVec2 tagSize = ImGui::CalcTextSize(tag);
                    PopFont();
                    const ImVec2 badgeMin(position.x + size.x + 8.0f, position.y + 1.0f);
                    const ImVec2 badgeMax(badgeMin.x + tagSize.x + 10.0f, badgeMin.y + tagSize.y + 4.0f);
                    ImGui::GetWindowDrawList()->AddRectFilled(badgeMin, badgeMax,
                                                              UiTheme::WithAlpha(tagColor, 38), 3.0f);
                    PaintText(ImGui::GetWindowDrawList(), Font::Micro,
                              ImVec2(badgeMin.x + 5.0f, badgeMin.y + 2.0f), tagColor, tag);
                }
                ImGui::Dummy(ImVec2(width, size.y + 7.0f));
            }

            g_section.active = true;
            g_section.origin = ImGui::GetCursorScreenPos();
            g_section.width = width;
            g_section.rowIndex = 0;
            g_section.drawList = ImGui::GetWindowDrawList();
            g_section.drawList->ChannelsSplit(2);
            g_section.drawList->ChannelsSetCurrent(1);
            ImGui::Dummy(ImVec2(width, 6.0f));
        }
    }

    void SectionBegin(const char* title)
    {
        SectionOpen(title, nullptr, 0);
    }

    void SectionBeginTagged(const char* title, const char* tag, ImU32 tagColor)
    {
        SectionOpen(title, tag, tagColor);
    }

    void SectionEnd()
    {
        if (!g_section.active)
            return;

        const float width = g_section.width;
        ImGui::Dummy(ImVec2(width, 6.0f));
        const float bottom = ImGui::GetCursorScreenPos().y;
        const UiTheme::Palette& palette = UiTheme::Current();

        ImDrawList* drawList = g_section.drawList;
        drawList->ChannelsSetCurrent(0);
        const ImVec2 maximum(g_section.origin.x + width, bottom);
        drawList->AddRectFilled(g_section.origin, maximum, palette.surface, 6.0f);
        drawList->AddRect(g_section.origin, maximum, palette.border, 6.0f, 1.0f);
        drawList->ChannelsMerge();

        g_section = SectionState{};
        ImGui::Dummy(ImVec2(width, Metrics::kSectionGap));
    }

    bool ToggleRow(const char* id, const char* label, bool* value, const char* helper)
    {
        if (!value)
            return false;
        constexpr float kControlWidth = 32.0f;
        const RowFrame frame = BeginRow(id, Metrics::kRowHeight, true);
        const bool pressed = frame.pressed;
        if (pressed)
            *value = !*value;
        RowLabel(frame, label, kControlWidth + 12.0f);

        ImGui::PushID(id);
        const ImGuiID animationId = ImGui::GetCurrentWindow()->GetID("##toggle_anim");
        ImGui::PopID();
        PaintToggle(ImVec2(frame.innerRight - kControlWidth * 0.5f, (frame.min.y + frame.max.y) * 0.5f),
                    *value, frame.hovered, animationId);
        RowHelper(helper);
        return pressed;
    }

    bool CheckRow(const char* id, const char* label, bool* value, const char* helper)
    {
        if (!value)
            return false;
        const RowFrame frame = BeginRow(id, Metrics::kRowHeight, true);
        const bool pressed = frame.pressed;
        if (pressed)
            *value = !*value;
        RowLabel(frame, label, 28.0f);
        PaintCheckbox(ImVec2(frame.innerRight - 8.0f, (frame.min.y + frame.max.y) * 0.5f), *value,
                      frame.hovered);
        RowHelper(helper);
        return pressed;
    }

    bool SliderRow(const char* id, const char* label, float* value, float minimum, float maximum,
                   const char* format, const char* helper)
    {
        if (!value || maximum <= minimum || !format)
            return false;

        const UiTheme::Palette& palette = UiTheme::Current();
        char valueText[64]{};
        snprintf(valueText, sizeof(valueText), format, *value);

        // 라벨과 값이 한 줄, 트랙이 그 아래 한 줄. 레퍼런스와 같은 배치이고 세로 공간이 가장 적게 든다.
        constexpr float kLabelLine = 22.0f;
        constexpr float kTrackLine = 16.0f;
        const RowFrame frame = BeginRow(id, kLabelLine + kTrackLine + 6.0f, false);

        const ImVec2 valueSize = MeasureText(Font::Small, valueText);
        const float labelCenterY = frame.min.y + kLabelLine * 0.5f + 2.0f;
        const ImVec2 labelSize = MeasureText(Font::Body, label);
        PaintText(Foreground(), Font::Body, ImVec2(frame.innerLeft, labelCenterY - labelSize.y * 0.5f),
                  TextPrimary(), label);
        PaintText(Foreground(), Font::Small,
                  ImVec2(frame.innerRight - valueSize.x, labelCenterY - valueSize.y * 0.5f),
                  TextSecondary(), valueText);

        constexpr float kTrackHeight = 4.0f;
        constexpr float kGrabRadius = 6.5f;
        const float innerWidth = frame.innerRight - frame.innerLeft;
        const float trackTop = frame.min.y + kLabelLine + 2.0f;
        bool changed = false;
        bool hovered = false;
        bool held = false;

        if (g_disabledDepth == 0)
        {
            ImGui::PushID(id);
            ImGui::SetCursorScreenPos(ImVec2(frame.innerLeft, trackTop));
            ImGui::InvisibleButton("##track", ImVec2(innerWidth, kTrackLine));
            hovered = ImGui::IsItemHovered();
            held = ImGui::IsItemActive();
            if (held && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                const float usable = (std::max)(1.0f, innerWidth - kGrabRadius * 2.0f);
                const float normalized =
                    std::clamp((ImGui::GetIO().MousePos.x - (frame.innerLeft + kGrabRadius)) / usable, 0.0f, 1.0f);
                const float next = minimum + normalized * (maximum - minimum);
                changed = next != *value;
                *value = next;
            }
            ImGui::PopID();
            ImGui::SetCursorScreenPos(ImVec2(frame.min.x, frame.max.y));
        }

        const float usable = (std::max)(1.0f, innerWidth - kGrabRadius * 2.0f);
        const float normalized = std::clamp((*value - minimum) / (maximum - minimum), 0.0f, 1.0f);
        const float centerY = trackTop + kTrackLine * 0.5f;
        const float grabX = frame.innerLeft + kGrabRadius + normalized * usable;

        ImU32 trackColor = palette.controlOff;
        ImU32 fillColor = held ? palette.accentHovered : palette.accent;
        ImU32 knobColor = palette.knob;
        if (g_disabledDepth > 0)
        {
            trackColor = UiTheme::Mix(palette.controlOff, palette.background, 0.55f);
            fillColor = UiTheme::Mix(palette.accent, palette.background, 0.62f);
            knobColor = UiTheme::Mix(palette.knob, palette.background, 0.55f);
        }

        ImDrawList* drawList = Foreground();
        drawList->AddRectFilled(ImVec2(frame.innerLeft, centerY - kTrackHeight * 0.5f),
                                ImVec2(frame.innerRight, centerY + kTrackHeight * 0.5f), trackColor,
                                kTrackHeight * 0.5f);
        drawList->AddRectFilled(ImVec2(frame.innerLeft, centerY - kTrackHeight * 0.5f),
                                ImVec2(grabX, centerY + kTrackHeight * 0.5f), fillColor, kTrackHeight * 0.5f);
        drawList->AddCircleFilled(ImVec2(grabX, centerY + 0.75f), kGrabRadius,
                                  UiTheme::WithAlpha(IM_COL32(0, 0, 0, 255), g_disabledDepth > 0 ? 10 : 30));
        drawList->AddCircleFilled(ImVec2(grabX, centerY), kGrabRadius, knobColor);
        drawList->AddCircle(ImVec2(grabX, centerY), kGrabRadius,
                            (hovered || held) ? fillColor : UiTheme::WithAlpha(fillColor, 160), 0, 1.5f);

        RowHelper(helper);
        return changed;
    }

    bool ComboRow(const char* id, const char* label, int* index, const char* const* items, int count)
    {
        if (!index || !items || count <= 0)
            return false;

        constexpr float kFieldWidth = 132.0f;
        constexpr float kFieldHeight = 24.0f;
        const RowFrame frame = BeginRow(id, Metrics::kRowHeight, false);
        RowLabel(frame, label, kFieldWidth + 12.0f);

        const int current = std::clamp(*index, 0, count - 1);
        const ImVec2 fieldMin(frame.innerRight - kFieldWidth, (frame.min.y + frame.max.y) * 0.5f - kFieldHeight * 0.5f);
        const ImVec2 fieldMax(fieldMin.x + kFieldWidth, fieldMin.y + kFieldHeight);

        bool hovered = false;
        bool pressed = false;
        bool changed = false;
        ImGui::PushID(id);
        if (g_disabledDepth == 0)
        {
            ImGui::SetCursorScreenPos(fieldMin);
            pressed = ImGui::InvisibleButton("##combo", ImVec2(kFieldWidth, kFieldHeight));
            hovered = ImGui::IsItemHovered();
            ImGui::SetCursorScreenPos(ImVec2(frame.min.x, frame.max.y));
        }

        const bool open = ImGui::IsPopupOpen("##combo_popup");
        PaintField(fieldMin, fieldMax, hovered, open);
        PaintText(Foreground(), Font::Body, ImVec2(fieldMin.x + 9.0f, fieldMin.y + (kFieldHeight - FontSize(Font::Body)) * 0.5f - 1.0f),
                  TextPrimary(), items[current]);
        PaintCaret(ImVec2(fieldMax.x - 11.0f, (fieldMin.y + fieldMax.y) * 0.5f),
                   g_disabledDepth > 0 ? UiTheme::Current().textDisabled : UiTheme::Current().textSecondary);

        if (pressed)
            ImGui::OpenPopup("##combo_popup");

        ImGui::SetNextWindowPos(ImVec2(fieldMin.x, fieldMax.y + 4.0f));
        ImGui::SetNextWindowSize(ImVec2(kFieldWidth, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 2.0f));
        PushFont(Font::Body);
        if (ImGui::BeginPopup("##combo_popup"))
        {
            for (int option = 0; option < count; ++option)
            {
                ImGui::PushID(option);
                if (ImGui::Selectable(items[option], option == current, 0, ImVec2(0.0f, 22.0f)))
                {
                    *index = option;
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
        PopFont();
        ImGui::PopStyleVar(2);
        ImGui::PopID();
        return changed;
    }

    // 버튼을 누른 그 클릭이 그대로 바인딩되지 않도록, 모든 키가 한 번 떼어진 뒤부터 입력을 받는다.
    // Escape는 취소, Insert(메뉴)와 End(언로드)는 트레이너가 이미 쓰는 키라 제외한다.
    bool KeybindRow(const char* id, const char* label, unsigned int* key)
    {
        if (!key)
            return false;

        constexpr float kFieldWidth = 120.0f;
        constexpr float kFieldHeight = 24.0f;
        const RowFrame frame = BeginRow(id, Metrics::kRowHeight, false);
        RowLabel(frame, label, kFieldWidth + 12.0f);

        ImGui::PushID(id);
        const ImGuiID selfId = ImGui::GetCurrentWindow()->GetID("##keybind");
        const bool capturing = g_captureId == selfId;
        const char* text = capturing ? (g_captureArmed ? Loc::Text(Loc::Str::PressKey)
                                                       : Loc::Text(Loc::Str::ReleaseKeys))
                                     : KeyName(*key);

        const ImVec2 fieldMin(frame.innerRight - kFieldWidth,
                              (frame.min.y + frame.max.y) * 0.5f - kFieldHeight * 0.5f);
        const ImVec2 fieldMax(fieldMin.x + kFieldWidth, fieldMin.y + kFieldHeight);

        bool hovered = false;
        bool pressed = false;
        if (g_disabledDepth == 0)
        {
            ImGui::SetCursorScreenPos(fieldMin);
            pressed = ImGui::InvisibleButton("##keybind", ImVec2(kFieldWidth, kFieldHeight));
            hovered = ImGui::IsItemHovered();
            ImGui::SetCursorScreenPos(ImVec2(frame.min.x, frame.max.y));
        }
        ImGui::PopID();

        PaintField(fieldMin, fieldMax, hovered, capturing);
        const ImVec2 textSize = MeasureText(Font::Small, text);
        PaintText(Foreground(), Font::Small,
                  ImVec2((fieldMin.x + fieldMax.x - textSize.x) * 0.5f,
                         (fieldMin.y + fieldMax.y - textSize.y) * 0.5f),
                  capturing ? UiTheme::Current().accent : TextPrimary(), text);

        if (pressed && !capturing)
        {
            g_captureId = selfId;
            g_captureArmed = false;
            return false;
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
            if (!g_captureArmed)
                break;
            if (virtualKey != VK_ESCAPE)
            {
                *key = static_cast<unsigned int>(virtualKey);
                changed = true;
            }
            g_captureId = 0;
            break;
        }
        if (!anyDown)
            g_captureArmed = true;
        return changed;
    }

    void MetricRow(const char* label, const char* value)
    {
        const RowFrame frame = BeginRow(label, 22.0f, false);
        const ImVec2 valueSize = MeasureText(Font::Mono, value);
        const ImVec2 labelSize = MeasureText(Font::Small, label);
        const float centerY = (frame.min.y + frame.max.y) * 0.5f;
        PaintText(Foreground(), Font::Small, ImVec2(frame.innerLeft, centerY - labelSize.y * 0.5f),
                  TextSecondary(), label);
        PaintText(Foreground(), Font::Mono, ImVec2(frame.innerRight - valueSize.x, centerY - valueSize.y * 0.5f),
                  TextPrimary(), value);
    }

    void MetricGroup(const char* title, const char* status, ImU32 statusColor)
    {
        const RowFrame frame = BeginRow(title, 24.0f, false);
        const ImVec2 titleSize = MeasureText(Font::Micro, title);
        const float centerY = (frame.min.y + frame.max.y) * 0.5f;
        PaintText(Foreground(), Font::Micro, ImVec2(frame.innerLeft, centerY - titleSize.y * 0.5f),
                  UiTheme::Current().textSecondary, title);
        if (status && *status)
        {
            const ImVec2 statusSize = MeasureText(Font::Micro, status);
            const ImVec2 badgeMin(frame.innerRight - statusSize.x - 10.0f, centerY - statusSize.y * 0.5f - 2.0f);
            const ImVec2 badgeMax(frame.innerRight, badgeMin.y + statusSize.y + 4.0f);
            Foreground()->AddRectFilled(badgeMin, badgeMax, UiTheme::WithAlpha(statusColor, 38), 3.0f);
            PaintText(Foreground(), Font::Micro, ImVec2(badgeMin.x + 5.0f, badgeMin.y + 2.0f), statusColor,
                      status);
        }
    }

    void HelperText(const char* text)
    {
        if (!text || !*text)
            return;
        const float width = RowInnerWidth();
        const ImVec2 size = MeasureText(Font::Small, text, width);
        const ImVec2 position = ImGui::GetCursorScreenPos();
        PaintText(Foreground(), Font::Small, ImVec2(position.x + RowInset(), position.y + 4.0f),
                  TextSecondary(), text, width);
        ImGui::Dummy(ImVec2(RowWidth(), size.y + 10.0f));
    }

    // 성능을 잃거나 런타임 동작이 달라지는 항목에만 쓴다. 설명까지 전부 경고로 그리면 진짜 경고가
    // 안 읽힌다.
    void WarningText(const char* text)
    {
        if (!text || !*text)
            return;
        const UiTheme::Palette& palette = UiTheme::Current();
        constexpr float kBar = 3.0f;
        constexpr float kPadX = 9.0f;
        constexpr float kPadY = 7.0f;
        const float blockWidth = RowInnerWidth();
        const float textWidth = (std::max)(24.0f, blockWidth - kBar - kPadX * 2.0f);
        const ImVec2 size = MeasureText(Font::Small, text, textWidth);
        const float height = size.y + kPadY * 2.0f;
        const ImVec2 position = ImGui::GetCursorScreenPos();
        const ImVec2 minimum(position.x + RowInset(), position.y + 4.0f);
        const ImVec2 maximum(minimum.x + blockWidth, minimum.y + height);

        ImDrawList* drawList = Foreground();
        drawList->AddRectFilled(minimum, maximum, palette.warningSoft, 5.0f);
        drawList->AddRectFilled(minimum, ImVec2(minimum.x + kBar + 5.0f, maximum.y), palette.warning, 5.0f);
        drawList->AddRectFilled(ImVec2(minimum.x + kBar, minimum.y), ImVec2(minimum.x + kBar + 5.0f, maximum.y),
                                palette.warningSoft);
        PaintText(drawList, Font::Small, ImVec2(minimum.x + kBar + kPadX, minimum.y + kPadY), palette.warning,
                  text, textWidth);
        ImGui::Dummy(ImVec2(RowWidth(), height + 10.0f));
    }

    bool CollapsibleRow(const char* id, const char* label, bool* open)
    {
        if (!open)
            return false;
        const RowFrame frame = BeginRow(id, 30.0f, true);
        if (frame.pressed)
            *open = !*open;

        const UiTheme::Palette& palette = UiTheme::Current();
        const float centerY = (frame.min.y + frame.max.y) * 0.5f;
        const float caretX = frame.innerLeft + 5.0f;
        ImDrawList* drawList = Foreground();
        if (*open)
        {
            drawList->AddTriangleFilled(ImVec2(caretX - 4.0f, centerY - 2.0f), ImVec2(caretX + 4.0f, centerY - 2.0f),
                                        ImVec2(caretX, centerY + 3.0f), palette.accent);
        }
        else
        {
            drawList->AddTriangleFilled(ImVec2(caretX - 2.0f, centerY - 4.0f), ImVec2(caretX + 3.0f, centerY),
                                        ImVec2(caretX - 2.0f, centerY + 4.0f), palette.textSecondary);
        }
        const ImVec2 labelSize = MeasureText(Font::Section, label);
        PaintText(drawList, Font::Section, ImVec2(frame.innerLeft + 16.0f, centerY - labelSize.y * 0.5f),
                  *open ? palette.text : palette.textSecondary, label);
        return *open;
    }

    // 아이콘 폰트를 새로 들이는 대신 네 개를 직접 그린다. 사이드바와 페이지 헤더에서만 쓰이고,
    // 전부 같은 1.6px 획으로 그려서 한 벌처럼 보인다.
    void DrawIcon(ImDrawList* drawList, Icon icon, ImVec2 center, float size, ImU32 color)
    {
        const float half = size * 0.5f;
        constexpr float kStroke = 1.5f;
        switch (icon)
        {
        case Icon::Crosshair:
        {
            drawList->AddCircle(center, half * 0.72f, color, 0, kStroke);
            drawList->AddLine(ImVec2(center.x - half, center.y), ImVec2(center.x - half * 0.4f, center.y), color,
                              kStroke);
            drawList->AddLine(ImVec2(center.x + half * 0.4f, center.y), ImVec2(center.x + half, center.y), color,
                              kStroke);
            drawList->AddLine(ImVec2(center.x, center.y - half), ImVec2(center.x, center.y - half * 0.4f), color,
                              kStroke);
            drawList->AddLine(ImVec2(center.x, center.y + half * 0.4f), ImVec2(center.x, center.y + half), color,
                              kStroke);
            drawList->AddCircleFilled(center, 1.4f, color);
            break;
        }
        case Icon::Eye:
        {
            drawList->PathClear();
            drawList->PathLineTo(ImVec2(center.x - half, center.y));
            drawList->PathBezierQuadraticCurveTo(ImVec2(center.x, center.y - half * 0.95f),
                                                 ImVec2(center.x + half, center.y), 16);
            drawList->PathBezierQuadraticCurveTo(ImVec2(center.x, center.y + half * 0.95f),
                                                 ImVec2(center.x - half, center.y), 16);
            drawList->PathStroke(color, kStroke, ImDrawFlags_None);
            drawList->AddCircleFilled(center, half * 0.3f, color);
            break;
        }
        case Icon::Sliders:
        {
            const float rows[3] = {center.y - half * 0.62f, center.y, center.y + half * 0.62f};
            const float knobs[3] = {center.x + half * 0.35f, center.x - half * 0.3f, center.x + half * 0.1f};
            for (int index = 0; index < 3; ++index)
            {
                drawList->AddLine(ImVec2(center.x - half, rows[index]), ImVec2(center.x + half, rows[index]),
                                  color, kStroke);
                drawList->AddCircleFilled(ImVec2(knobs[index], rows[index]), 2.1f, color);
            }
            break;
        }
        case Icon::Terminal:
        {
            drawList->AddRect(ImVec2(center.x - half, center.y - half * 0.82f),
                              ImVec2(center.x + half, center.y + half * 0.82f), color, 2.5f, kStroke);
            drawList->AddLine(ImVec2(center.x - half * 0.45f, center.y - half * 0.3f),
                              ImVec2(center.x - half * 0.05f, center.y), color, kStroke);
            drawList->AddLine(ImVec2(center.x - half * 0.05f, center.y),
                              ImVec2(center.x - half * 0.45f, center.y + half * 0.3f), color, kStroke);
            drawList->AddLine(ImVec2(center.x + half * 0.1f, center.y + half * 0.32f),
                              ImVec2(center.x + half * 0.5f, center.y + half * 0.32f), color, kStroke);
            break;
        }
        }
    }

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
}
