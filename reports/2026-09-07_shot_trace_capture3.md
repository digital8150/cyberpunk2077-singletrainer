# Last requested shotgun capture: capture 3

Only the final capture was analyzed; captures 1 and 2 are excluded per the operator's instruction.

- PID 20628, Cyberpunk 2077 2.31, trace implementation commit 798cc7c.
- START QPC 375510320520; duration 19993.604 ms; START and STOP present; reported drops 0.
- 1204 records, sequence 90 through 1293 inclusive, all unique and contiguous.
- All 80 weapon samples: entity 0x9F02DA, ProjectilesPerShot 6.
- Input/settings records: silent aim on, head only, profile 1; no-spread and no-recoil both off.
- 1090 crosshair callbacks, no queued weapon/projectile events observed. Absence of queue events cannot identify whether firing happened.

## Caller separation

| Immediate return-site RVA | Calls | Observed pattern |
|---|---:|---|
| 0x2A60F85 | 999 | Repeated calls during aiming, across worker threads |
| 0x2048BED | 42 | Seven groups of exactly six calls |
| 0x657410 | 49 | Seven corresponding groups of seven calls |

At 0x2048BED, each six-call group has six different original returned direction vectors, followed by one identical final direction vector after existing silent redirection. All 42 were redirected. This is a strong candidate for the per-projectile direction path; it is **not yet a disassembly-verified pellet loop or a native shot ID**. Counts alone must not be used as a classifier.

| Group start relative to capture (s) | Calls at 0x2048BED | Group duration (ms) |
|---:|---:|---:|
| 4.523976 | 6 | 0.1221 |
| 7.076990 | 6 | 0.1338 |
| 9.784725 | 6 | 0.1214 |
| 10.540321 | 6 | 0.1198 |
| 13.377486 | 6 | 0.1264 |
| 15.011650 | 6 | 0.1109 |
| 18.947632 | 6 | 0.1297 |

The 0x657410 groups follow these closely, span approximately 1.4-2.4 ms, and have seven distinct original directions / two distinct final directions each. Their role is unresolved; do not add them to the six-call groups as additional pellets. There are more frame-sampled LMB press edges than these groups, so a click is not itself a confirmed accepted shot.

Raw source: `[SHOTTRACE] cap=3` in `%LOCALAPPDATA%/cp2077_trainer/cp2077_trainer.log`, restricted to QPC >= 375510320520. The final-only extraction and JSON summary are in untracked `tools/scripts/temp/shot_trace_final_capture.log` and `shot_trace_latest_report.json`.

Next investigation: disassemble the callers around 0x2048BED and 0x657410, identify loop bounds and shot/weapon context, and compare a single-projectile control capture if needed. No distribution implementation or runtime change was made during this analysis.
