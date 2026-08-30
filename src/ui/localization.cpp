#include "localization.h"

#include "../features/features.h"

#include <cstddef>

namespace Loc
{
    namespace
    {
        struct Entry
        {
            const char* korean;
            const char* english;
        };

#define LOC_KO_EN(koreanText, englishText) {koreanText, englishText}

        // 이 배열의 순서는 Str 열거형과 1:1로 맞아야 한다 (아래 static_assert가 개수를 지킨다).
        constexpr Entry kText[] = {
            LOC_KO_EN("CBPK Trainer", "CBPK Trainer"),

            LOC_KO_EN("일반", "General"),
            LOC_KO_EN("표시", "Visuals"),
            LOC_KO_EN("시스템", "System"),
            LOC_KO_EN("에임봇", "Aimbot"),
            LOC_KO_EN("ESP", "ESP"),
            LOC_KO_EN("기타", "Misc"),
            LOC_KO_EN("디버그", "Debug"),
            LOC_KO_EN("조준 보조 동작과 타겟 필터를 설정합니다.", "Configure aiming behavior and target filters."),
            LOC_KO_EN("엔티티 표시 방식과 가시성 필터를 설정합니다.", "Configure entity rendering and visibility filters."),
            LOC_KO_EN("플레이어 편의 기능을 설정합니다.", "Configure player convenience features."),
            LOC_KO_EN("진단, 개발자 도구, 런타임 정보입니다.", "Diagnostics, developer tools and runtime information."),

            LOC_KO_EN("빌드", "Built on"),
            LOC_KO_EN("Insert 메뉴 · End 언로드", "Insert menu · End unload"),
            LOC_KO_EN("한국어", "Korean"),
            LOC_KO_EN("English", "English"),
            LOC_KO_EN("다크", "Dark"),
            LOC_KO_EN("라이트", "Light"),

            LOC_KO_EN("일반", "General"),
            LOC_KO_EN("타겟 선정", "Targeting"),
            LOC_KO_EN("조준 동작", "Aiming"),
            LOC_KO_EN("표시 요소", "Visuals"),
            LOC_KO_EN("분류 필터", "Filters"),
            LOC_KO_EN("가시성과 거리", "Visibility and range"),
            LOC_KO_EN("무기", "Weapon"),
            LOC_KO_EN("오버레이", "Overlay"),
            LOC_KO_EN("개발자 진단", "Developer diagnostics"),
            LOC_KO_EN("실험적 기능", "Experimental"),
            LOC_KO_EN("불안정", "UNSTABLE"),
            LOC_KO_EN("개발과 진단을 위한 기능입니다. 평소의 런타임 동작이 달라질 수 있습니다.",
                      "Intended for development and diagnostics. May alter normal runtime behavior."),

            LOC_KO_EN("에임봇 활성화", "Enable aimbot"),
            LOC_KO_EN("조준 방식", "Aim mode"),
            LOC_KO_EN("클래식", "Classic"),
            LOC_KO_EN("사일런트", "Silent"),
            LOC_KO_EN("활성화 키", "Activation key"),
            LOC_KO_EN("FOV 원 그리기", "Draw FOV circle"),
            LOC_KO_EN("FOV 반경", "FOV radius"),
            LOC_KO_EN("스무딩", "Smoothing"),
            LOC_KO_EN("조준 거리", "Aim distance"),
            LOC_KO_EN("적 조준", "Target enemies"),
            LOC_KO_EN("경찰 조준", "Target police"),
            LOC_KO_EN("시야가 확보된 대상만", "Only visible targets"),
            LOC_KO_EN("체력 풀이 확인된 대상만", "Require health pool"),
            LOC_KO_EN("체력 풀 상한 사용", "Limit health pool"),
            LOC_KO_EN("최대 체력 풀", "Max health pool"),
            LOC_KO_EN("%s 키를 누르는 동안 카메라는 그대로 두고 발사 방향만 대상에게 보냅니다.",
                      "Hold %s: the shot follows the target while the camera stays where you point it."),
            LOC_KO_EN("%s 키를 누르는 동안 대상에게 카메라를 부드럽게 회전합니다.",
                      "Hold %s to rotate the camera onto the target."),
            LOC_KO_EN("ESP와 같은 시야 캐시를 사용합니다. 아직 결과가 없는 대상은 통과합니다.",
                      "Shares the ESP visibility cache. Targets without a cached result are allowed through."),
            LOC_KO_EN("스탯 풀이 확인되지 않은 NPC와 최대 체력이 지나치게 높은 차량·보스급 대상을 제외합니다.",
                      "Drops NPCs with unresolved stat pools and vehicle/boss-class actors with unusually high maximum health."),
            LOC_KO_EN("사일런트 조준은 카메라를 움직이지 않아 스무딩을 쓰지 않습니다.",
                      "Silent aim never moves the camera, so smoothing is unused."),
            LOC_KO_EN("에임봇을 켜면 설정할 수 있습니다.", "Enable the aimbot to configure these."),

            LOC_KO_EN("ESP 활성화", "Enable ESP"),
            LOC_KO_EN("바운딩 박스", "Bounding boxes"),
            LOC_KO_EN("스켈레톤", "Skeleton"),
            LOC_KO_EN("체력 바", "Health bars"),
            LOC_KO_EN("네이티브 하이라이트", "Native highlight"),
            LOC_KO_EN("시민", "Civilians"),
            LOC_KO_EN("적", "Enemies"),
            LOC_KO_EN("경찰", "Police"),
            LOC_KO_EN("미분류", "Unclassified"),
            LOC_KO_EN("죽은 NPC 숨기기", "Hide dead NPCs"),
            LOC_KO_EN("시야 확인", "Visibility check"),
            LOC_KO_EN("가려진 NPC 숨기기", "Hide occluded NPCs"),
            LOC_KO_EN("최대 거리", "Max distance"),
            LOC_KO_EN("게임 물리 시야 쿼리는 비용이 듭니다. 전투 중 프레임 시간이 늘면 끄세요.",
                      "Game-physics visibility queries are expensive. Turn this off if frame time rises during combat."),
            LOC_KO_EN("ESP를 켜면 설정할 수 있습니다.", "Enable ESP to configure these."),

            LOC_KO_EN("반동 없음", "No recoil"),
            LOC_KO_EN("탄퍼짐 없음", "No spread"),
            LOC_KO_EN("현재 장착한 무기의 스탯 수정자에 적용됩니다. 무기를 바꾸면 다시 적용됩니다.",
                      "Applies to the equipped weapon's stat modifiers and is re-applied when you switch weapons."),

            LOC_KO_EN("FPS 표시", "FPS counter"),
            LOC_KO_EN("성능 그래프", "Performance graph"),
            LOC_KO_EN("그래프 투명도", "Graph opacity"),
            LOC_KO_EN("내부 통계", "Internal stats"),
            LOC_KO_EN("아래 런타임 통계 패널을 표시합니다.", "Shows the runtime statistics panel below."),
            LOC_KO_EN("진단 로깅", "Diagnostic logging"),
            LOC_KO_EN("진단 로그를 디스크에 기록합니다. 디스크 작업이 늘어납니다.",
                      "Writes diagnostics to disk; adds disk activity."),
            LOC_KO_EN("크래시 리포팅", "Crash reporting"),
            LOC_KO_EN("치명적 폴트와 복구 가능한 예외를 함께 기록합니다.",
                      "Records fatal faults and first-chance exceptions."),
            LOC_KO_EN("성능 프로파일링", "Performance profiling"),
            LOC_KO_EN("모든 계측 지점에서 QPC 구간 시간을 측정합니다.",
                      "Times every instrumented scope with QPC."),
            LOC_KO_EN("디버거 출력", "Debugger output"),
            LOC_KO_EN("메시지마다 디버거 출력 예외 경로를 거칩니다.",
                      "Routes each message through the debugger output path."),
            LOC_KO_EN("헤드리스 에임봇", "Headless aimbot"),
            LOC_KO_EN("메뉴가 닫혀 있을 때만 적용됩니다.", "Applies only while the menu is closed."),
            LOC_KO_EN("진단 전용입니다. 메뉴가 닫혀 있는 동안 오버레이 GPU 제출을 멈추고 타겟 선택만 실행합니다.",
                      "Diagnostics only. While the menu is closed, overlay GPU submissions stop and only target selection runs."),
            LOC_KO_EN("런타임 통계", "Runtime statistics"),
            LOC_KO_EN("사일런트 에임 진단", "Silent aim diagnostics"),

            LOC_KO_EN("%.0f m", "%.0f m"),
            LOC_KO_EN("%.1f°", "%.1f°"),
            LOC_KO_EN("%.0f HP", "%.0f HP"),
            LOC_KO_EN("%.1f", "%.1f"),
            LOC_KO_EN("%.0f%%", "%.0f%%"),

            LOC_KO_EN("트레이너 준비 완료", "Trainer ready"),
            LOC_KO_EN("메뉴를 열려면", "Press"),
            LOC_KO_EN("키를 누르세요", "to open the menu"),
            LOC_KO_EN("INSERT", "INSERT"),

            LOC_KO_EN("키를 누르세요...", "Press a key..."),
            LOC_KO_EN("모든 키를 떼세요...", "Release keys..."),
            LOC_KO_EN("마우스 L", "Mouse L"),
            LOC_KO_EN("마우스 R", "Mouse R"),
            LOC_KO_EN("마우스 M", "Mouse M"),
            LOC_KO_EN("마우스 4", "Mouse 4"),
            LOC_KO_EN("마우스 5", "Mouse 5"),
            LOC_KO_EN("백스페이스", "Backspace"),
            LOC_KO_EN("Tab", "Tab"),
            LOC_KO_EN("Enter", "Enter"),
            LOC_KO_EN("Shift", "Shift"),
            LOC_KO_EN("오른쪽 Shift", "R shift"),
            LOC_KO_EN("Ctrl", "Ctrl"),
            LOC_KO_EN("오른쪽 Ctrl", "R ctrl"),
            LOC_KO_EN("Alt", "Alt"),
            LOC_KO_EN("오른쪽 Alt", "R alt"),
            LOC_KO_EN("Caps Lock", "Caps lock"),
            LOC_KO_EN("Space", "Space"),
            LOC_KO_EN("Page Up", "Page up"),
            LOC_KO_EN("Page Down", "Page down"),
            LOC_KO_EN("Home", "Home"),
            LOC_KO_EN("왼쪽", "Left"),
            LOC_KO_EN("위쪽", "Up"),
            LOC_KO_EN("오른쪽", "Right"),
            LOC_KO_EN("아래쪽", "Down"),
            LOC_KO_EN("Delete", "Delete"),
            LOC_KO_EN("숫자 키패드 %u", "Numpad %u"),
            LOC_KO_EN("F%u", "F%u"),
            LOC_KO_EN("0x%02X", "0x%02X"),
        };

#undef LOC_KO_EN

        constexpr std::size_t kTextCount = sizeof(kText) / sizeof(kText[0]);
        static_assert(kTextCount == static_cast<std::size_t>(Str::KeyHexFormat) + 1,
                      "localization table must contain one entry for every Str value");
    }

    const char* Text(Str id, Features::Language language)
    {
        const std::size_t index = static_cast<std::size_t>(id);
        if (index >= kTextCount)
            return "";
        return language == Features::Language::English ? kText[index].english : kText[index].korean;
    }

    const char* Text(Str id)
    {
        return Text(id, Features::GetSettings().ui.language);
    }
}
