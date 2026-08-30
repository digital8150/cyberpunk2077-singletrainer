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

        constexpr Entry kText[] = {
            LOC_KO_EN("Cyberpunk 2077 트레이너", "Cyberpunk 2077 Trainer"),
            LOC_KO_EN("CBPK  /  TRAINER", "CBPK  /  TRAINER"),
            LOC_KO_EN("오프라인 싱글플레이어 오버레이", "Offline single-player overlay"),
            LOC_KO_EN("에임봇", "Aimbot"),
            LOC_KO_EN("ESP", "ESP"),
            LOC_KO_EN("기타", "Misc"),
            LOC_KO_EN("디버그", "Debug"),
            LOC_KO_EN("조준 보조와 타겟 필터", "Aim assistance and target filters"),
            LOC_KO_EN("엔티티 표시와 시야 필터", "Entity display and visibility filters"),
            LOC_KO_EN("플레이어 편의 기능", "Player convenience features"),
            LOC_KO_EN("표시·진단·성능 설정", "Display, diagnostics, and performance"),
            LOC_KO_EN("에임봇 활성화", "Enable aimbot"),
            LOC_KO_EN("ESP 활성화", "Enable ESP"),
            LOC_KO_EN("사일런트 에임", "Silent aim"),
            LOC_KO_EN("활성화 키", "Activation key"),
            LOC_KO_EN("FOV 원 그리기", "Draw FOV circle"),
            LOC_KO_EN("적 조준", "Target enemies"),
            LOC_KO_EN("경찰 조준", "Target police"),
            LOC_KO_EN("시야가 확보된 대상만", "Only visible targets"),
            LOC_KO_EN("체력 풀이 확인된 대상만", "Require health pool"),
            LOC_KO_EN("체력 풀 상한 사용", "Limit health pool"),
            LOC_KO_EN("최대 체력 풀", "Max health pool"),
            LOC_KO_EN("FOV 반경", "FOV radius"),
            LOC_KO_EN("스무딩", "Smoothing"),
            LOC_KO_EN("조준 거리", "Aim distance"),
            LOC_KO_EN("바운딩 박스", "Bounding boxes"),
            LOC_KO_EN("스켈레톤", "Skeleton"),
            LOC_KO_EN("체력 바", "Health bars"),
            LOC_KO_EN("네이티브 하이라이트", "Native highlight"),
            LOC_KO_EN("죽은 NPC 숨기기", "Hide dead NPCs"),
            LOC_KO_EN("시야 확인", "Visibility check"),
            LOC_KO_EN("가려진 NPC 숨기기", "Hide occluded NPCs"),
            LOC_KO_EN("시민", "Civilians"),
            LOC_KO_EN("적", "Enemies"),
            LOC_KO_EN("경찰", "Police"),
            LOC_KO_EN("미분류", "Unclassified"),
            LOC_KO_EN("최대 거리", "Max distance"),
            LOC_KO_EN("반동 없음", "No recoil"),
            LOC_KO_EN("탄퍼짐 없음", "No spread"),
            LOC_KO_EN("자동 권총", "Auto pistol"),
            LOC_KO_EN("무한 체력", "Infinite health"),
            LOC_KO_EN("무한 스태미나", "Infinite stamina"),
            LOC_KO_EN("FPS 표시", "Show FPS"),
            LOC_KO_EN("그래프 표시", "Show graph"),
            LOC_KO_EN("내부 통계 표시", "Show internal stats"),
            LOC_KO_EN("진단 로깅", "Diagnostic logging"),
            LOC_KO_EN("크래시 리포팅", "Crash reporting"),
            LOC_KO_EN("성능 프로파일링", "Performance profiling"),
            LOC_KO_EN("디버거 출력", "Debugger output"),
            LOC_KO_EN("헤드리스 에임봇 (진단)", "Headless aimbot (diagnostics)"),
            LOC_KO_EN("언어", "Language"),
            LOC_KO_EN("한국어", "Korean"),
            LOC_KO_EN("영어", "English"),
            LOC_KO_EN("테마", "Theme"),
            LOC_KO_EN("다크", "Dark"),
            LOC_KO_EN("라이트", "Light"),
            LOC_KO_EN("Insert: 메뉴 · End: 언로드", "Insert: menu · End: unload"),
            LOC_KO_EN("CBPK  /  TRAINER READY", "CBPK  /  TRAINER READY"),
            LOC_KO_EN("메뉴를 열려면", "Press"),
            LOC_KO_EN("버튼을 누르세요", "to open the menu"),
            LOC_KO_EN("INSERT", "INSERT"),
            LOC_KO_EN("키를 누르세요...", "Press a key..."),
            LOC_KO_EN("모든 키를 떼세요...", "Release keys..."),
            LOC_KO_EN("%s 키를 누르는 동안 카메라는 그대로 두고 발사 방향을 대상에게 보냅니다.",
                      "Hold %s: the shot follows the target while the camera stays where you point it."),
            LOC_KO_EN("%s 키를 누르는 동안 대상에게 카메라를 부드럽게 회전합니다. 스무딩 0은 보간을 사용하지 않습니다.",
                      "Hold %s to rotate the camera onto the target. Smoothing 0 has no easing."),
            LOC_KO_EN("ESP와 같은 시야 캐시를 사용합니다. 벽 뒤 대상은 두 에임 모드에서 제외되며, 아직 결과가 없는 대상은 통과합니다.",
                      "Shares the ESP visibility cache. Targets behind cover are skipped in both aim modes; targets without a cached result are allowed through."),
            LOC_KO_EN("스탯 풀이 아직 확인되지 않은 NPC와 일반 NPC보다 최대 체력이 높은 차량·보스급 대상을 후보에서 제외합니다. 실제 적이 빠지면 상한을 높이세요.",
                      "Drops NPCs with unresolved stat pools and vehicle/boss-class actors whose maximum health is far above a normal NPC. Raise the limit if a real enemy is skipped."),
            LOC_KO_EN("게임 물리 시야 쿼리는 비용이 들 수 있습니다. 전투 중 프레임 시간이 증가하면 이 옵션을 끄세요.",
                      "Game-physics visibility queries can be expensive. Turn this off if frame time increases during combat."),
            LOC_KO_EN("파일 진단 로그는 디스크 작업을 추가할 수 있어 성능이 낮아질 수 있습니다.",
                      "File diagnostics add disk activity and may reduce performance."),
            LOC_KO_EN("QPC 구간 계측은 모든 측정 지점에서 CPU 비용을 추가할 수 있습니다.",
                      "QPC instrumentation adds CPU work at every measured scope and may reduce performance."),
            LOC_KO_EN("OutputDebugStringA는 디버거 출력마다 예외 경로를 거칠 수 있어 성능이 낮아질 수 있습니다.",
                      "OutputDebugStringA can take an exception path for each message and may reduce performance."),
            LOC_KO_EN("치명적 폴트 기록과 예외 관측을 활성화합니다. 복구 가능한 예외도 관측될 수 있습니다.",
                      "Enables fatal fault recording and exception observation. Recoverable first-chance exceptions may also be observed."),
            LOC_KO_EN("진단 전용입니다. 메뉴가 닫혀 있을 때 오버레이 GPU 제출을 멈추고 타겟 선택만 실행합니다.",
                      "Diagnostics only. While the menu is closed, overlay GPU submissions stop and target selection continues."),
            LOC_KO_EN("%.0f m", "%.0f m"),
            LOC_KO_EN("%.1f°", "%.1f°"),
            LOC_KO_EN("%.0f HP", "%.0f HP"),
            LOC_KO_EN("%.1f", "%.1f"),
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
            LOC_KO_EN("사일런트 에임 진단", "Silent aim diagnostics"),
            LOC_KO_EN("내부 통계", "Internal stats"),
            LOC_KO_EN("언어 선택", "Select language"),
            LOC_KO_EN("테마 선택", "Select theme"),
        };

#undef LOC_KO_EN

        constexpr std::size_t kTextCount = sizeof(kText) / sizeof(kText[0]);
        static_assert(kTextCount == static_cast<std::size_t>(Str::ThemePopup) + 1,
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
