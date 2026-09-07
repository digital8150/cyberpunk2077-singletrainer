#include "profiling.h"
#include "framework.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace Diagnostics { void Log(const char*, ...) {} }
void Check(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main()
{
    using namespace Diagnostics::Profile;
    SetEnabled(true);
    Reset();
    WindowSnapshot window;
    Check(!ReadWindow(window), "no completed window must report unavailable");
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const auto unit = frequency.QuadPart / 1000; // 1 ms in QPC ticks
    LogCadence();
    BeginPresentFrame();
    Record(Slot::SnapshotPass, unit * 2);
    Record(Slot::SnapshotLockWait, unit); // Nested: must not inflate Present total.
    Record(Slot::PoseRequestPass, unit);
    Record(Slot::EspFrame, unit * 3);
    Record(Slot::AimbotFrame, unit * 4);
    Record(Slot::TickTotal, unit * 20); // Separate thread path: must not enter Present total.
    RecordValue(Slot::PoseDeferred, 7);
    EndPresentFrame();
    Sleep(5050);
    LogCadence();
    Check(ReadWindow(window), "completed window missing");
    const double expected = unit * 1000000.0 / frequency.QuadPart;
    Check(std::abs(window.Get(Slot::PresentTotal).total - expected * 10) < 0.01, "nested timing double-counted");
    Check(std::abs(window.Get(Slot::TickTotal).maximum - expected * 20) < 0.01, "tick units wrong");
    Check(window.Get(Slot::PoseDeferred).Average() == 7, "counter converted to duration");
    WindowSnapshot again;
    Check(ReadWindow(again) && again.Get(Slot::PresentTotal).count == 1, "UI read drained counters");
    Check(window.Get(Slot::PoseSlots).count == 0, "inactive scope must remain absent");
    SetEnabled(false);
    Check(!ReadWindow(again), "disabled profiling must hide stale values");
    SetEnabled(true);
    Check(!ReadWindow(again), "reenabling must not expose old window");
}
