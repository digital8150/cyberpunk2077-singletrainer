#include "game/pellet_targets.h"
#include <cstdio>
#include <thread>
#include <vector>
#include <cstdlib>
void Check(bool condition) { if (!condition) { std::fprintf(stderr, "pellet invariant failed\n"); std::exit(1); } }
int main()
{
    using namespace Game::PelletTargets;
    Plan plan;
    plan.count = 2;
    plan.points[0] = {{1, 2, 3}, 1};
    plan.points[1] = {{4, 5, 6}, 4};
    unsigned heads = 0, chests = 0;
    for (unsigned i = 0; i < 6; ++i) { heads += plan.At(i).bone == 1; chests += plan.At(i).bone == 4; }
    Check(heads == 3 && chests == 3);
    Cache cache;
    Mapping m, out;
    m.key.system = 42; m.key.rotation = {1, 2, 3, 4};
    m.point = plan.At(1); m.shot = 1; m.epoch = 8; m.expires = 350; m.index = 1; m.count = 6;
    Check(cache.Store(m));
    Check(cache.Find(m.key, 8, 300, out) && out.point.bone == 4 && out.index == 1 && out.shot == 1);
    Check(!cache.Find(m.key, 9, 300, out)); // profile/activation invalidation
    Check(!cache.Find(m.key, 8, 351, out)); // expiry is only a lifetime bound, not a shot classifier
    auto other = m.key; other.system++;
    Check(!cache.Find(other, 8, 300, out));
    other = m.key; other.rotation[2]++;
    Check(!cache.Find(other, 8, 300, out)); // exact quaternion, no angular/timing approximation
    m.index = 2; m.point = plan.At(2);
    Check(cache.Store(m));
    Check(!cache.Find(m.key, 8, 300, out)); // ambiguous equal rotations must fail open
    Cache parallel;
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 8; ++t) threads.emplace_back([&, t] {
        for (unsigned i = 1; i < 3000; ++i)
        {
            Mapping v; v.shot = t * 3000 + i; v.epoch = 1; v.expires = 10;
            v.key.system = v.shot; v.key.rotation[0] = static_cast<unsigned>(v.shot);
            v.point.bone = static_cast<unsigned>(v.shot); v.point.world[0] = static_cast<float>(v.shot);
            parallel.Store(v);
            Mapping read;
            if (parallel.Find(v.key, 1, 0, read))
                Check(read.shot == v.shot && read.point.bone == v.shot && read.point.world[0] == v.shot);
        }
    });
    for (auto& thread : threads) thread.join();
    std::puts("pellet plan/cache checks passed");
}
