#pragma once

#include <imgui.h>

// 오버레이 전체가 공유하는 단일 컴포넌트 시스템. 페이지 코드(widgets.cpp)는 여기 있는 조각만
// 조립하고, 색·간격·행 높이·상태 표현을 직접 그리지 않는다. 페이지마다 컨트롤을 다시 구현하면
// 같은 애플리케이션으로 읽히지 않는다는 것이 이전 UI가 실패한 이유 중 하나였다.
namespace UiKit
{
    // ── 메트릭 (전부 4px 배수) ────────────────────────────────────────────────
    namespace Metrics
    {
        constexpr float kSidebarWidth = 168.0f;
        constexpr float kFooterHeight = 28.0f;
        constexpr float kHeaderHeight = 52.0f;
        constexpr float kContentPadding = 16.0f;
        constexpr float kPanelPadding = 12.0f;
        constexpr float kRowHeight = 32.0f;
        constexpr float kColumnGap = 16.0f;
        constexpr float kSectionGap = 16.0f;
        // 2열이 실제로 이득이 되는 최소 폭. 이보다 좁으면 열 하나가 라벨을 두 줄로 접는다.
        constexpr float kTwoColumnMinWidth = 620.0f;
        constexpr float kMinColumnWidth = 290.0f;
        // 열 하나가 이보다 넓어지면 라벨은 왼쪽 끝, 컨트롤은 오른쪽 끝에 남아 그 사이가 눈으로
        // 이어지지 않는다. 넓은 창에서는 열을 늘리지 않고 남는 폭을 좌우 여백으로 돌린다.
        constexpr float kMaxColumnWidth = 470.0f;
    }

    enum class Font
    {
        Title,    // 페이지 제목
        Section,  // 섹션 제목
        Body,     // 설정 라벨
        Small,    // 보조 설명·값
        Micro,    // 사이드바 카테고리, 배지
        Mono,     // 런타임 수치·주소
    };

    enum class Icon
    {
        Crosshair,
        Eye,
        Sliders,
        Terminal,
    };

    // ── 생명주기 ─────────────────────────────────────────────────────────────
    // 폰트 아틀라스 구성. ImGui 백엔드 초기화 전에 한 번 호출한다.
    void LoadFonts();
    // 매 프레임 시작. 테마가 런타임에 바뀌므로 ImGui 스타일도 다시 밀어 넣는다.
    void BeginFrame();

    void PushFont(Font role);
    void PopFont();
    float FontSize(Font role);
    ImFont* FontFace(Font role);
    ImVec2 MeasureText(Font role, const char* text, float wrapWidth = 0.0f);
    void PaintText(ImDrawList* drawList, Font role, ImVec2 position, ImU32 color, const char* text,
                   float wrapWidth = 0.0f);

    // ── 비활성 스코프 ────────────────────────────────────────────────────────
    // 켤 수 없는 설정은 숨기지 않고 비활성으로 남긴다. 사라지면 왜 없는지 알 수 없다.
    void BeginDisabled(bool disabled);
    void EndDisabled();

    // ── 레이아웃 ─────────────────────────────────────────────────────────────
    void PageHeader(Icon icon, const char* title, const char* subtitle);

    // 지금 열의 폭. 모든 컴포넌트가 이 값을 기준으로 정렬한다.
    float ContentWidth();
    void SetContentWidth(float width);
    // 사용 가능한 폭에 맞춰 실제 열 개수를 정한다 (요청한 것보다 줄어들 수 있다).
    int ResolveColumnCount(int desired, float availableWidth);
    void ColumnsBegin(int count, float totalWidth);
    void Column();
    void ColumnsEnd();

    void SectionBegin(const char* title);
    void SectionEnd();
    // 섹션 제목 옆 배지 (예: "실험적"). SectionBegin 직전에 호출한다.
    void SectionBeginTagged(const char* title, const char* tag, ImU32 tagColor);

    // ── 행 ──────────────────────────────────────────────────────────────────
    bool ToggleRow(const char* id, const char* label, bool* value, const char* helper = nullptr);
    bool CheckRow(const char* id, const char* label, bool* value, const char* helper = nullptr);
    bool SliderRow(const char* id, const char* label, float* value, float minimum, float maximum,
                   const char* format, const char* helper = nullptr);
    bool ComboRow(const char* id, const char* label, int* index, const char* const* items, int count);
    bool KeybindRow(const char* id, const char* label, unsigned int* key);
    // 값을 표시만 하는 행 (런타임 통계).
    void MetricRow(const char* label, const char* value);
    void MetricGroup(const char* title, const char* status, ImU32 statusColor);

    void HelperText(const char* text);
    void WarningText(const char* text);
    // 섹션 안에서 접히는 하위 구역.
    bool CollapsibleRow(const char* id, const char* label, bool* open);

    // ── 조각 ────────────────────────────────────────────────────────────────
    void DrawIcon(ImDrawList* drawList, Icon icon, ImVec2 center, float size, ImU32 color);
    // 키 이름은 현재 언어 테이블을 거친 UTF-8이다 (GetKeyNameTextA는 ANSI 코드페이지라 쓰지 않는다).
    const char* KeyName(unsigned int key);
}
