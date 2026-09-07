#include "shot_trace.h"
#include "../framework.h"
#include "../diagnostics.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace Game::ShotTrace
{
    namespace
    {
        constexpr unsigned kCapacity = 4096, kBudget = 24000;
        struct Cell { std::atomic<unsigned> state{0}; Record record; };
        std::array<Cell, kCapacity> cells;
        std::atomic_uint64_t ticket{0}, session{0}, deadline{0}, dropped{0};
        std::atomic_uint remaining{0};
        bool keyHeld = false;
        unsigned lastInput = ~0u;
        ULONGLONG nextWeapon = 0;
        void Push(Record record)
        {
            record.sequence = ticket.fetch_add(1, std::memory_order_relaxed);
            auto& cell = cells[record.sequence % kCapacity];
            unsigned expected = 0;
            if (!cell.state.compare_exchange_strong(expected, 1, std::memory_order_acquire))
            {
                dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            cell.record = record;
            cell.state.store(2, std::memory_order_release);
        }
        void Stamp(Record& record)
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            if (!record.qpc) record.qpc = now.QuadPart;
            record.thread = GetCurrentThreadId();
            if (!record.capture) record.capture = session.load(std::memory_order_acquire);
        }
    }
    bool IsCapturing()
    {
        const auto until = deadline.load(std::memory_order_acquire);
        return until != 0 && GetTickCount64() < until;
    }
    void Submit(Record record)
    {
        if (!IsCapturing()) return;
        if (record.capture && record.capture != session.load(std::memory_order_acquire)) return;
        const unsigned count = remaining.fetch_add(1, std::memory_order_relaxed);
        if (count >= kBudget)
        {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        Stamp(record);
        Push(record);
    }
    Record Begin(Kind kind)
    {
        Record record;
        record.kind = kind;
        Stamp(record);
        return record;
    }
    void Stop()
    {
        if (!deadline.exchange(0, std::memory_order_acq_rel)) return;
        Record record;
        record.kind = CaptureStop;
        Stamp(record);
        record.identity = dropped.load(std::memory_order_relaxed);
        record.flags = remaining.load(std::memory_order_relaxed);
        Push(record);
    }
    bool Start()
    {
        if (!Diagnostics::GetRuntimeToggles().diagnosticLogging) return false;
        Stop();
        session.fetch_add(1, std::memory_order_acq_rel);
        remaining.store(0, std::memory_order_relaxed);
        lastInput = ~0u;
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        Record record;
        record.kind = CaptureStart;
        Stamp(record);
        record.identity = frequency.QuadPart;
        record.object = reinterpret_cast<std::uint64_t>(GetModuleHandleW(L"Cyberpunk2077.exe"));
        record.context = GetTickCount64();
        record.event = dropped.load(std::memory_order_relaxed);
        Push(record);
        deadline.store(GetTickCount64() + 20000, std::memory_order_release);
        return true;
    }
    void Tick(bool menuVisible, unsigned settingsFlags)
    {
        const bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        DWORD process = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &process);
        if (down && !keyHeld && !menuVisible && process == GetCurrentProcessId())
        {
            if (IsCapturing()) Stop();
            else Start();
        }
        keyHeld = down;
        if (deadline.load(std::memory_order_acquire) &&
            (!IsCapturing() || menuVisible || process != GetCurrentProcessId())) Stop();
        if (!IsCapturing()) return;
        const unsigned input = ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1u : 0u) |
                               ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 2u : 0u) | (settingsFlags << 8);
        if (input != lastInput)
        {
            lastInput = input;
            Record record;
            record.kind = Input;
            record.flags = input;
            Submit(record);
        }
    }
    bool WantWeaponSample()
    {
        if (!IsCapturing()) return false;
        const auto now = GetTickCount64();
        if (now < nextWeapon) return false;
        nextWeapon = now + 250;
        return true;
    }
    bool Drain(void (*sink)(const char*, std::size_t))
    {
        // A full bounded scan also drains after shutdown. Disk writes are batched, on the writer thread only.
        char batch[64 * 1024];
        std::size_t used = 0;
        bool wrote = false;
        for (auto& cell : cells)
        {
            unsigned expected = 2;
            if (!cell.state.compare_exchange_strong(expected, 3, std::memory_order_acquire)) continue;
            const Record r = cell.record;
            cell.state.store(0, std::memory_order_release);
            char line[900];
            const int size = snprintf(line, sizeof(line),
                "[SHOTTRACE] cap=%llu seq=%llu kind=%u tid=%u qpc=%llu end=%llu caller=%llX object=%llX "
                "context=%llX event=%llX id=%llX flags=%u origin=%.6g,%.6g,%.6g before=%.6g,%.6g,%.6g after=%.6g,%.6g,%.6g\r\n",
                r.capture, r.sequence, r.kind, r.thread, r.qpc, r.endQpc, r.caller, r.object,
                r.context, r.event, r.identity, r.flags, r.origin[0], r.origin[1], r.origin[2],
                r.before[0], r.before[1], r.before[2], r.after[0], r.after[1], r.after[2]);
            if (size <= 0 || static_cast<std::size_t>(size) >= sizeof(line)) continue;
            if (used + size > sizeof(batch)) { sink(batch, used); used = 0; }
            memcpy(batch + used, line, size);
            used += size;
            wrote = true;
        }
        if (used) sink(batch, used);
        return wrote;
    }
}
