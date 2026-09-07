#include "game/shot_trace.h"
#include "diagnostics.h"
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <set>
#include <sstream>
#include <cstdio>

namespace Diagnostics { RuntimeToggles GetRuntimeToggles() { return {}; } }
std::string output;
void Sink(const char* data, std::size_t size) { output.append(data, size); }
int main()
{
    using namespace Game::ShotTrace;
    Start();
    std::atomic<unsigned> finished{0};
    std::vector<std::thread> producers;
    for (unsigned producer = 0; producer < 8; ++producer)
        producers.emplace_back([&, producer] {
            for (unsigned i = 0; i < 4000; ++i)
            {
                auto r = Begin(Crosshair);
                r.identity = static_cast<std::uint64_t>(producer) * 4000 + i;
                r.object = r.identity ^ 0x1234ABCD;
                Submit(r);
            }
            finished.fetch_add(1);
        });
    while (finished.load() < 8) { Drain(Sink); Sleep(1); }
    for (auto& producer : producers) producer.join();
    Drain(Sink);
    Stop();
    Drain(Sink);
    std::set<unsigned long long> sequences;
    std::istringstream lines(output);
    std::string line;
    unsigned count = 0, stops = 0;
    unsigned long long lost = 0;
    while (std::getline(lines, line))
    {
        unsigned long long cap, seq, qpc, end, caller, object, context, event, id;
        unsigned kind, tid, flags;
        const int fields = sscanf_s(line.c_str(),
            "[SHOTTRACE] cap=%llu seq=%llu kind=%u tid=%u qpc=%llu end=%llu caller=%llX object=%llX context=%llX event=%llX id=%llX flags=%u",
            &cap, &seq, &kind, &tid, &qpc, &end, &caller, &object, &context, &event, &id, &flags);
        if (fields != 12 || cap != 1 || !sequences.insert(seq).second) return 1;
        if (kind == Crosshair)
        {
            if (object != (id ^ 0x1234ABCD)) return 2;
            ++count;
        }
        if (kind == CaptureStop) { ++stops; lost = id; }
    }
    if (stops != 1 || count + lost != 32000 || count > 24000 || IsCapturing()) return 3;
    auto stale = Begin(Crosshair);
    Start();
    Submit(stale); // Cross-capture callbacks must not be reassigned to the new capture.
    Stop();
    output.clear();
    Drain(Sink);
    if (output.find("kind=4") != std::string::npos) return 4;
    printf("Shot trace concurrency/budget/capture tests passed: recorded=%u dropped=%llu\n", count, lost);
    return 0;
}
