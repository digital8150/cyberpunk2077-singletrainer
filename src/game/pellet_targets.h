#pragma once
#include <array>
#include <atomic>
#include <cstdint>

namespace Game::PelletTargets
{
    struct Point { float world[3]{}; unsigned bone = 0; };
    struct Plan
    {
        std::array<Point, 5> points{};
        unsigned count = 0;
        Point At(unsigned pellet) const { return count ? points[pellet % count] : Point{}; }
    };
    struct Key
    {
        std::uint64_t system = 0;
        std::array<std::uint32_t, 4> rotation{};
        bool operator==(const Key&) const = default;
    };
    struct Mapping
    {
        Key key{};
        Point point{};
        std::uint64_t shot = 0, epoch = 0, expires = 0;
        unsigned index = 0, count = 0;
    };
    // Only held while copying POD, never while calling the engine. No spinning or allocation.
    // Ambiguous identical spread rotations (e.g. no-spread) are deliberately not replayed.
    class Cache
    {
        struct Cell { std::atomic_flag busy = ATOMIC_FLAG_INIT; Mapping value{}; };
        std::array<Cell, 256> cells_{};
        std::atomic<unsigned> next_{0};
    public:
        bool Store(const Mapping& value)
        {
            auto& cell = cells_[next_.fetch_add(1, std::memory_order_relaxed) % cells_.size()];
            if (cell.busy.test_and_set(std::memory_order_acquire)) return false;
            cell.value = value;
            cell.busy.clear(std::memory_order_release);
            return true;
        }
        bool Find(const Key& key, std::uint64_t epoch, std::uint64_t now, Mapping& output)
        {
            bool found = false;
            for (auto& cell : cells_)
            {
                if (cell.busy.test_and_set(std::memory_order_acquire)) return false;
                const Mapping value = cell.value;
                cell.busy.clear(std::memory_order_release);
                if (!value.shot || value.epoch != epoch || now > value.expires || !(value.key == key)) continue;
                if (found) return false;
                output = value;
                found = true;
            }
            return found;
        }
    };
}
