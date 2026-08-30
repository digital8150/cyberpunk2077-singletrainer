#include "profiling.h"

#include "diagnostics.h"
#include "framework.h"

#include <array>
#include <atomic>
#include <cstdio>

namespace
{
    constexpr ULONGLONG kCadenceMilliseconds = 5000;
    constexpr std::size_t kSlotCount = static_cast<std::size_t>(Diagnostics::Profile::Slot::Count);

    struct Accumulator
    {
        std::atomic_uint64_t count{0};
        std::atomic_uint64_t total{0};
        std::atomic_uint64_t maximum{0};
    };

    std::array<Accumulator, kSlotCount> g_slots{};
    std::array<std::atomic_uint64_t, kSlotCount> g_lastSlotTicks{};
    std::atomic<std::int64_t> g_frequency{0};
    std::atomic_uint64_t g_lastPresentTicks{0};
    ULONGLONG g_lastCadenceTick = 0;
    std::uint64_t g_presentFrameTicks = 0;
    std::atomic_bool g_presentFrameActive{false};

    struct SlotInfo
    {
        const char* name;
        bool isDuration;
    };

    // Slot 열거 순서와 반드시 일치해야 한다.
    constexpr SlotInfo kSlotInfo[kSlotCount] = {
        {"snapshot", true},
        {"snapLockWait", true},
        {"snapCount", false},
        {"esp", true},
        {"aimbot", true},
        {"tickTotal", true},
        {"pose", true},
        {"health", true},
        {"attitude", true},
        {"highlight", true},
        {"playerMods", true},
        {"visibility", true},
        {"poseSlots", true},
        {"healthCollect", true},
        {"healthInvoke", true},
        {"attitudeCollect", true},
        {"attitudeInvoke", true},
        {"highlightCollect", true},
    };

    std::int64_t Frequency()
    {
        std::int64_t frequency = g_frequency.load(std::memory_order_relaxed);
        if (frequency == 0)
        {
            LARGE_INTEGER value{};
            if (QueryPerformanceFrequency(&value) && value.QuadPart > 0)
                frequency = value.QuadPart;
            else
                frequency = 1;
            g_frequency.store(frequency, std::memory_order_relaxed);
        }
        return frequency;
    }

    struct Sample
    {
        std::uint64_t count;
        std::uint64_t total;
        std::uint64_t maximum;
    };

    Sample DrainSlot(Accumulator& slot)
    {
        Sample sample{};
        // count를 마지막에 비워서, 값만 실린 채 개수가 0인 창을 만들지 않는다. 계측 자체가 락을 쓰지
        // 않으므로 경계에서 표본 하나가 다음 창으로 밀릴 수는 있으며 이는 허용한다.
        sample.total = slot.total.exchange(0, std::memory_order_relaxed);
        sample.maximum = slot.maximum.exchange(0, std::memory_order_relaxed);
        sample.count = slot.count.exchange(0, std::memory_order_relaxed);
        return sample;
    }

    void AppendSample(char* buffer, std::size_t capacity, std::size_t& offset, const SlotInfo& info,
                      const Sample& sample)
    {
        if (sample.count == 0 || offset >= capacity)
            return;

        int written = 0;
        if (info.isDuration)
        {
            const double frequency = static_cast<double>(Frequency());
            const double averageMicroseconds =
                static_cast<double>(sample.total) * 1000000.0 / (frequency * static_cast<double>(sample.count));
            const double maximumMicroseconds = static_cast<double>(sample.maximum) * 1000000.0 / frequency;
            written = _snprintf_s(buffer + offset, capacity - offset, _TRUNCATE, " %s[n=%llu avg=%.1fus max=%.1fus]",
                                  info.name, static_cast<unsigned long long>(sample.count), averageMicroseconds,
                                  maximumMicroseconds);
        }
        else
        {
            const double average = static_cast<double>(sample.total) / static_cast<double>(sample.count);
            written = _snprintf_s(buffer + offset, capacity - offset, _TRUNCATE, " %s[n=%llu avg=%.1f max=%llu]",
                                  info.name, static_cast<unsigned long long>(sample.count), average,
                                  static_cast<unsigned long long>(sample.maximum));
        }

        if (written > 0)
            offset += static_cast<std::size_t>(written);
        else
            offset = capacity;
    }

    void LogGroup(const char* label, Diagnostics::Profile::Slot first, Diagnostics::Profile::Slot last,
                  std::uint64_t elapsedMilliseconds)
    {
        char buffer[512]{};
        std::size_t offset = 0;
        bool any = false;
        for (unsigned index = static_cast<unsigned>(first); index <= static_cast<unsigned>(last); ++index)
        {
            const Sample sample = DrainSlot(g_slots[index]);
            any = any || sample.count != 0;
            AppendSample(buffer, sizeof(buffer), offset, kSlotInfo[index], sample);
        }
        if (!any)
            return;
        Diagnostics::Log("profile %s (%llums):%s", label, static_cast<unsigned long long>(elapsedMilliseconds),
                         buffer);
    }

    bool IsPresentDurationSlot(Diagnostics::Profile::Slot slot)
    {
        return slot == Diagnostics::Profile::Slot::SnapshotPass ||
               slot == Diagnostics::Profile::Slot::EspFrame ||
               slot == Diagnostics::Profile::Slot::AimbotFrame;
    }

    std::uint64_t TicksToMicroseconds(std::uint64_t ticks)
    {
        if (ticks == 0)
            return 0;
        const double microseconds = static_cast<double>(ticks) * 1000000.0 /
                                    static_cast<double>(Frequency());
        return microseconds > 0.0 ? static_cast<std::uint64_t>(microseconds) : 0;
    }
}

namespace Diagnostics::Profile
{
    std::int64_t Now()
    {
        LARGE_INTEGER value{};
        QueryPerformanceCounter(&value);
        return value.QuadPart;
    }

    void SetEnabled(bool enabled)
    {
        if (g_enabled.exchange(enabled, std::memory_order_relaxed) == enabled)
            return;
        if (!enabled)
            Reset();
    }

    void Record(Slot slot, std::int64_t ticks)
    {
        if (slot >= Slot::Count || ticks < 0 || !Enabled())
            return;

        Accumulator& accumulator = g_slots[static_cast<std::size_t>(slot)];
        const auto value = static_cast<std::uint64_t>(ticks);
        g_lastSlotTicks[static_cast<std::size_t>(slot)].store(value, std::memory_order_release);
        if (g_presentFrameActive.load(std::memory_order_acquire) && IsPresentDurationSlot(slot))
            g_presentFrameTicks += value;
        accumulator.count.fetch_add(1, std::memory_order_relaxed);
        accumulator.total.fetch_add(value, std::memory_order_relaxed);
        std::uint64_t previous = accumulator.maximum.load(std::memory_order_relaxed);
        while (value > previous &&
               !accumulator.maximum.compare_exchange_weak(previous, value, std::memory_order_relaxed,
                                                          std::memory_order_relaxed))
        {
        }
    }

    void RecordValue(Slot slot, std::uint64_t value)
    {
        Record(slot, static_cast<std::int64_t>(value));
    }

    void LogCadence()
    {
        if (!Enabled())
            return;

        const ULONGLONG now = GetTickCount64();
        if (g_lastCadenceTick == 0)
        {
            g_lastCadenceTick = now;
            return;
        }
        const ULONGLONG elapsed = now - g_lastCadenceTick;
        if (elapsed < kCadenceMilliseconds)
            return;
        g_lastCadenceTick = now;

        LogGroup("present", Slot::SnapshotPass, Slot::AimbotFrame, elapsed);
        LogGroup("tick", Slot::TickTotal, Slot::TickVisibility, elapsed);
        LogGroup("tickdetail", Slot::PoseSlots, Slot::HighlightCollect, elapsed);
    }

    void Reset()
    {
        for (Accumulator& slot : g_slots)
        {
            slot.count.store(0, std::memory_order_relaxed);
            slot.total.store(0, std::memory_order_relaxed);
            slot.maximum.store(0, std::memory_order_relaxed);
        }
        for (std::atomic_uint64_t& slot : g_lastSlotTicks)
            slot.store(0, std::memory_order_release);
        g_lastPresentTicks.store(0, std::memory_order_release);
        g_presentFrameTicks = 0;
        g_presentFrameActive.store(false, std::memory_order_release);
        g_lastCadenceTick = 0;
    }

    void BeginPresentFrame()
    {
        g_presentFrameTicks = 0;
        g_presentFrameActive.store(Enabled(), std::memory_order_release);
    }

    void EndPresentFrame()
    {
        if (!g_presentFrameActive.load(std::memory_order_acquire))
            return;
        g_lastPresentTicks.store(g_presentFrameTicks, std::memory_order_release);
        g_presentFrameActive.store(false, std::memory_order_release);
    }

    std::uint64_t LastPresentMicroseconds()
    {
        if (!Enabled())
            return 0;
        return TicksToMicroseconds(g_lastPresentTicks.load(std::memory_order_acquire));
    }

    std::uint64_t LastTickTotalMicroseconds()
    {
        if (!Enabled())
            return 0;
        return TicksToMicroseconds(
            g_lastSlotTicks[static_cast<std::size_t>(Slot::TickTotal)].load(std::memory_order_acquire));
    }
}
