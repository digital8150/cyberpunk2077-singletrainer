#pragma once
#include <cstddef>
#include <cstdint>

namespace Game::ShotTrace
{
    enum Kind : unsigned { CaptureStart, CaptureStop, Input, Weapon, Crosshair, QueuedEvent,
                           ShotBegin, ShotEnd, Pellet, PelletReplay };
    struct Record
    {
        std::uint64_t sequence = 0, capture = 0, qpc = 0, endQpc = 0;
        std::uint64_t caller = 0, object = 0, context = 0, event = 0, identity = 0;
        unsigned kind = 0, thread = 0, flags = 0;
        float origin[3]{}, before[3]{}, after[3]{};
    };
    bool IsCapturing();
    bool Start(); // Called by the F8 controller; also usable by the isolated regression harness.
    Record Begin(Kind kind);
    void Tick(bool menuVisible, unsigned settingsFlags); // Present only: F8/input edges, no disk/VM work.
    bool WantWeaponSample(); // Game main tick only, throttled to 250 ms.
    void Submit(Record record); // Bounded, nonblocking producer; no pointer dereferences.
    bool Drain(void (*sink)(const char*, std::size_t)); // Existing diagnostic writer only.
    void Stop();
}
