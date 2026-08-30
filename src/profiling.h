#pragma once

#include <atomic>
#include <cstdint>

// QPC 기반 구간 계측. 최적화 전/후를 로그로 대조하기 위한 것이며, 슬롯 하나당 비용은 QPC 2회와
// 소수의 relaxed 원자 연산이다. 누적값은 메인 틱에서 5초마다 한 번 로그로 비우므로 오버플로 걱정이 없다.
namespace Diagnostics::Profile
{
    enum class Slot : unsigned
    {
        // Present 스레드 경로.
        SnapshotPass,      // GetPuppetSnapshots 전체 (락 대기 포함). 프레임당 1회 — esp/aimbot과 형제 슬롯이다.
        SnapshotLockWait,  // 그중 g_puppetListLock 배타 획득 대기
        SnapshotPuppets,   // 패스당 복사된 스냅샷 개수 (시간이 아니라 개수)
        PoseSlots,         // ReadCurrentPoseSlots 1회 (퍼펫당, 33 ms 주기)
        EspFrame,          // Esp::DrawOverlay 전체 (스냅샷 패스는 제외 — 호출자가 먼저 돌린다)
        AimbotFrame,       // Aimbot::RunFrame 전체

        // 게임 메인 틱 경로. TickTotal이 트레이너가 게임 틱에 얹는 총 지연이다.
        TickTotal,
        TickHealth,
        TickAttitude,
        TickHighlight,
        TickPlayerModifiers,
        TickVisibility,

        // 메인 틱 세부. 슬롯 순회 비용과 리플렉션 호출 비용을 나눠서 어느 쪽이 실제 병목인지 가른다.
        HealthCollect,
        HealthInvoke,
        AttitudeCollect,
        AttitudeInvoke,
        HighlightCollect,

        Count,
    };

    // 계측 마스터 스위치. 끄면 Scope가 QPC조차 부르지 않는다. 성능 최적화가 끝난 뒤 계측 비용을
    // 완전히 뺀 상태의 프레임을 재기 위한 것으로, config.ini의 [diagnostics] profiling으로 조절한다.
    inline std::atomic<bool> g_enabled{true};
    inline bool Enabled() { return g_enabled.load(std::memory_order_relaxed); }
    void SetEnabled(bool enabled);

    std::int64_t Now();
    void Record(Slot slot, std::int64_t ticks);
    // 시간이 아닌 값(개수 등)을 같은 누적기에 넣는다. 로그에서 마이크로초로 환산하지 않는다.
    void RecordValue(Slot slot, std::uint64_t value);

    // 게임 메인 틱에서 매 틱 호출한다. 5초가 지났을 때만 로그를 찍고 누적값을 리셋한다.
    void LogCadence();
    void Reset();

    // Present 스레드에서 한 프레임의 트레이너 구간을 묶는다. Scope 자체는 기존처럼 슬롯별 누적 로그에도
    // 기록되지만, 이 경계 사이의 Present 슬롯 합계는 그래프용으로 별도 보존된다.
    void BeginPresentFrame();
    void EndPresentFrame();
    // 마지막으로 완료된 Present 경로의 SnapshotPass + EspFrame + AimbotFrame 합계(us). profiling이 꺼져
    // 있으면 0을 반환한다. SnapshotPass는 호출자가 실제로 스냅샷을 요청한 프레임에만 포함된다.
    std::uint64_t LastPresentMicroseconds();
    // 마지막으로 완료된 게임 메인 틱의 TickTotal(us). profiling이 꺼져 있으면 0을 반환한다.
    std::uint64_t LastTickTotalMicroseconds();

    class Scope
    {
    public:
        // 꺼져 있으면 start_가 0으로 남고 소멸자도 아무것도 하지 않는다. QPC 2회가 통째로 빠진다.
        explicit Scope(Slot slot) : slot_(slot), start_(Enabled() ? Now() : 0) {}
        ~Scope()
        {
            if (start_ != 0)
                Record(slot_, Now() - start_);
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Slot slot_;
        std::int64_t start_;
    };
}
