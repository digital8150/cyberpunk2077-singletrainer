# Pose scheduling and consumer demand (Cyberpunk 2077 2.31)

## Confirmed cause

Before this change, `GetPuppetSnapshots` requested a pose for every returned snapshot (up to 128), before ESP/aim category filtering. Every main tick sampled eight requests round-robin. Removing the earlier 33 ms throttle did not remove this queue. Civilian and unclassified entries therefore consumed pose slots even when neither feature used them.

At 20:34:53.415, the live profiler recorded 591 ticks in five seconds, 4,728 pose samples (exactly eight per tick), mean pose pass 124.1 us, max 246.0 us. Per-entity slot lookup averaged 11.5 us. At 20:34:54.661, snapshots contained 91 civilians, two enemies, one police and 34 others; ESP category-enabled count was three. A full stable 128-request round at that tick cadence would take about 135 ms, but this is a scheduler estimate, not a measurement of each NPC's old interval.

## Change

- Snapshot capture no longer requests animation implicitly. Features publishes the union of ESP and aim demand as IDs after filtering.
- Shared category predicates preserve runtime hostility overrides and the separate police toggle. ESP death/visual-output settings and aim death/health conditions apply before requesting pose. Headless mode excludes ESP demand; native-highlight-only ESP requests no pose.
- A conservative root-depth test excludes targets more than three metres behind the camera or beyond both consumers' depth limits plus a three-metre limb margin. Projection failure allows a request to recover. FOV/occlusion do not gate pose: these consumers can need fresh joints to evaluate those conditions, and classic locked targets can lie outside the acquisition FOV.
- The frame snapshot array now matches the tracker capacity (256), so the old first-128 truncation does not hide later targets from consumers.
- Request publication replaces the set immediately. Paused/expired requests resume with an empty cache until a fresh main-tick sample; excluded frame entries expose no stale joints. Engine calls still happen outside tracker locks and slot VM entry remains on main tick only.
- Eight entities per tick remains in this change. The measured filtered sample had no deferred work; increasing this limit was not needed in that sample. If future requested demand exceeds eight, `deferred` explicitly exposes that limitation. This is not a claim that eight is universally appropriate.

## Metrics (existing profiler toggle; five-second log windows)

| Group / field | Meaning |
| --- | --- |
| present / poseRequest | Filter, projection and ID publication cost; included in Present graph total |
| tick / pose | Entire pose pass cost |
| tickdetail / poseSlots | Per-entity joint-slot read cost |
| pose-work / requested | Consumer ID count per snapshot frame |
| pose-work / eligible | Unexpired requests seen per main tick |
| pose-work / processed | Retained work items submitted for sampling per tick; not a success counter |
| pose-work / deferred | Eligible requests not submitted this tick, including retention failure |
| pose-age / intervalMs | Interval between complete cache publications for an entity while requested |
| pose-age / espAgeMs | Cache age of skeletons actually reaching draw |
| pose-age / aimAgeMs | Cache age of the selected aim candidate (also recorded before activation is held) |
| pose-request-category | Requested counts by original civilian/enemy/police/other archetype; hostile civilians/others may legitimately be enemy targets |

Age/interval fields use GetTickCount64 milliseconds and its coarse clock resolution. They are cache freshness measurements, not render FPS or proof of how often the engine itself changes animation transforms. `n`, average and maximum are reported; no percentile is claimed. Re-entry after a cancelled request starts a new interval series. Metrics use the existing asynchronous diagnostic writer, with no added disk wait on game/Present threads.

## Observed filtered sample

The first instrumented build at 20:40:51.067 recorded 470 ticks / five seconds, requested=eligible=processed=4.0 average/max, deferred=0 throughout. Publication interval averaged 10.6 ms (max 47 ms); drawn ESP skeleton cache age averaged 3.9 ms (max 16 ms). This establishes that the scheduler was no longer dividing these four targets across a large civilian queue. Scene composition changed after reinjection, so this is not a controlled before/after total-FPS benchmark.

Initial aim-age telemetry counted all examined candidates, including ones outside the request depth gate, and interval maxima included reactivation gaps. Those initial aim-age values must not be interpreted as active aim latency. The final build measures the selected candidate and resets cancelled caches before reactivation.

## Validation

Release build passed. Four CTests passed, including new target-filter cases for disabled categories, hostility overrides, police independence, ESP/aim union cases, corpse display, health requirements/caps, disabled consumers and native-only ESP. Safe unload/reinjection succeeded in PID 20628. No new VM hooks or engine memory mutations were added.

Final-build negative-demand validation: at 20:43:37.976 there were 20 snapshots (one civilian, 19 other, no hostile entries), with categoryEnabled=0. The corresponding 20:43:37.964 five-second window had requested=eligible=processed=deferred=0, and all requested-category maxima were zero. Thus these excluded NPCs actually consumed no pose calls. A new final-build active aim sample was not yet available at that time.
