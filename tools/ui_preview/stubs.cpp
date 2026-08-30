// 프리뷰 하네스용 게임 측 스텁.
//
// `src/ui`는 설정 구조체 하나와 진단 카운터 다섯 개만 게임 쪽에서 읽는다. 그 여섯 개를 여기서
// 채워주면 게임 프로세스 없이도 실제 UI 코드를 그대로 띄워 볼 수 있다 — UI 코드는 한 줄도
// 복제하지 않는다. 이 파일은 트레이너 DLL에 링크되지 않는다.
#include "../../src/features/aimbot.h"
#include "../../src/features/features.h"
#include "../../src/game/entity_tracker.h"
#include "../../src/game/player_modifiers.h"
#include "../../src/game/silent_aim.h"
#include "../../src/game/visibility.h"

namespace Features
{
    Settings& GetSettings()
    {
        static Settings settings;
        return settings;
    }
}

namespace Game::EntityTracker
{
    Stats GetStats()
    {
        Stats stats;
        stats.hookCreated = true;
        stats.registered = 8421;
        stats.positioned = 8390;
        stats.puppets = 236;
        stats.trackedPuppets = 68;
        stats.trackedCivilians = 41;
        stats.trackedEnemies = 19;
        stats.trackedPolice = 6;
        stats.trackedHostile = 22;
        stats.attitudeValid = 63;
        stats.attitudeInvalid = 5;
        stats.healthValid = 61;
        stats.healthInvalid = 7;
        stats.pendingPosition = 3;
        stats.nativeHighlightQueued = 148;
        stats.nativeHighlightCleared = 141;
        stats.nativeHighlightFailures = 2;
        return stats;
    }
}

namespace Game::PlayerModifiers
{
    Stats GetStats()
    {
        Stats stats;
        stats.available = true;
        stats.active = true;
        stats.targetId = 0x1A2B3C4D5Eull;
        stats.usingWeaponTarget = true;
        stats.applied = 12;
        stats.removed = 11;
        stats.retiredOwnerResets = 4;
        stats.failures = 0;
        return stats;
    }
}

namespace Aimbot
{
    Stats GetStats()
    {
        Stats stats;
        stats.candidates = 68;
        stats.eligible = 14;
        stats.skippedNoHealthPool = 7;
        stats.skippedHealthCap = 2;
        stats.skippedOccluded = 45;
        stats.targetEntityId = 0x00FF31A2ull;
        stats.targetHealthValid = true;
        stats.targetHealth = 318.0f;
        stats.targetHealthMax = 640.0f;
        return stats;
    }
}

namespace Game::Visibility
{
    Stats GetStats()
    {
        Stats stats;
        stats.available = true;
        stats.casts = 1904;
        stats.visible = 611;
        stats.occluded = 1240;
        stats.dropped = 53;
        return stats;
    }
}

namespace Game::SilentAim
{
    DiagnosticsSnapshot GetDiagnostics()
    {
        DiagnosticsSnapshot snapshot;
        snapshot.crosshairCoreHookCreated = true;
        snapshot.nativeCrosshairCoreCalls = 5183;
        snapshot.nativeCrosshairCoreRedirects = 742;
        snapshot.rejectedShots = 18;
        return snapshot;
    }
}
