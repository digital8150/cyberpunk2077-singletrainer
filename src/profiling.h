#pragma once

#include <cstdint>

// QPC 기반 구간 계측. 최적화 전/후를 로그로 대조하기 위한 것이며, 슬롯 하나당 비용은 QPC 2회 +
// relaxed 원자 연산 3회다. 누적값은 메인 틱에서 5초마다 한 번 로그로 비우므로 오버플로 걱정이 없다.
namespace Diagnostics::Profile
{
    enum class Slot : unsigned
    {
        // Present 스레드 경로.
        SnapshotPass,      // GetPuppetSnapshots 전체 (락 대기 포함)
        SnapshotLockWait,  // 그중 g_puppetListLock 배타 획득 대기
        SnapshotPuppets,   // 패스당 복사된 스냅샷 개수 (시간이 아니라 개수)
        PoseSlots,         // ReadCurrentPoseSlots 1회 (퍼펫당, 33 ms 주기)
        EspFrame,          // Esp::DrawOverlay 전체
        AimbotFrame,       // Aimbot::RunFrame 전체

        // 게임 메인 틱 경로. TickTotal이 트레이너가 게임 틱에 얹는 총 지연이다.
        TickTotal,
        TickHealth,
        TickAttitude,
        TickHighlight,
        TickPlayerModifiers,
        TickVisibility,

        Count,
    };

    std::int64_t Now();
    void Record(Slot slot, std::int64_t ticks);
    // 시간이 아닌 값(개수 등)을 같은 누적기에 넣는다. 로그에서 마이크로초로 환산하지 않는다.
    void RecordValue(Slot slot, std::uint64_t value);

    // 게임 메인 틱에서 매 틱 호출한다. 5초가 지났을 때만 로그를 찍고 누적값을 리셋한다.
    void LogCadence();
    void Reset();

    class Scope
    {
    public:
        explicit Scope(Slot slot) : slot_(slot), start_(Now()) {}
        ~Scope() { Record(slot_, Now() - start_); }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Slot slot_;
        std::int64_t start_;
    };
}
