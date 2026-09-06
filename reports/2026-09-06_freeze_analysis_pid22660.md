# PID 22660 freeze analysis - 2026-09-06

## Finding

The immediate hang is a REDengine main-tick scheduler spin waiting for an unfinished job. A null-pointer READ exception occurred on the same thread 36ms after the world tracker became empty. Visibility lacks the shared world-transition gate and is a priority investigation/fix candidate, but the captured exception does not establish it as the caller responsible.

## Evidence

- Process started 17:47:10, Responding=False. Game file version: 3.0.5294808; all game RVAs below refer to this executable.
- Main TID 14612 consumed 3046.875ms CPU over approximately 3 seconds; all other threads had zero CPU delta.
- Dumps captured 17:54:01 and 17:54:33, each 149,482,473 bytes, 108 threads. Main RSP remained 0x25A83FEF50; RIP moved from ntdll+0x1A90D to +0x1A911.
- Both dumps: r14=0x17DB4EEFF00, [r14+0x18]=1. Disassembly at game +0x14AC10 confirms a loop testing this field for zero.
- Two separately captured live main-stack candidate reports are identical. These are stack scans, not formal unwinds.
- Main stack includes game +0x14AC7B / +0x14AF14 and trainer +0x1CB95. PDB resolves the latter to HookOnTick+0x125. Disassembly confirms it follows the original OnTick indirect call at +0x1CB93: trainer tick work has returned, and the original game tick is spinning.
- Trainer SHA-256: 6FBE9A17B269338416115D6C5560EA39EBB4E86CB5583AF0359C1B87FD4963C2, matching the previously validated pose-main-tick build.

## Preceding exception

17:51:51.842: world consumer gate: empty tracked=0.

17:51:51.878: PID 22660 / TID 14612 first-chance 0xC0000005 at game +0x1118014, READ target=0x60, rcx=0, rsp=0x25A83FEA00.

The faulting instruction is mov rax,[rcx+0x60]. The preceding instruction loads rcx from [rax+0x78], demonstrating a null internal object. Exception stack candidates include +0x1117D97, +0x14628D, +0x1117B78 and +0x32BCA3; no trainer caller appears within the recorded range.

Current RSP is 0x550 higher than at the fault, consistent with leaving the deeper exception-time stack before entering the scheduler wait. There is no unhandled record for this session. The hypothesis that the exception stranded a job is plausible but not proven by job ownership or exception-handler tracing. Log filesystem LastWriteTime was stale; use timestamps inside the log.

## Feature state and remaining defect

Both dumps show worldReadyForConsumers=0, worldWasEmpty=1, nativeHighlightModeActive=0, and PlayerModifiers mainTickStage=0 (Idle). Config has misc no_recoil/no_spread=0 and native_highlight=0; preceding playerMods profiling averages 0.1us. The active recoil/spread hypothesis from PID 29968 must not be copied to this incident.

Visibility remains active with 63 queued requests. src/game/visibility.cpp:342, ProcessPendingOnMainTick, runs after EntityTracker updates the world gate, but does not check IsWorldReadyForMainTickConsumers before acquiring the spatial system and invoking synchronous raycasts. It also lacks queue invalidation/world-generation checks on this path. Running on the main thread alone does not make world-transition calls safe.

This missing gate is a confirmed code defect, not a confirmed attribution of the recorded AV. The game exception function has not been identified as raycast, and its captured stack does not show that trainer path. Game or other mod transition work remains possible; RED4ext/CET and other mods are present.

## Recommended follow-up

1. Gate Visibility engine acquisition and raycasts on shared world readiness; discard old queued requests/cache when closed and resume with fresh requests.
2. Add dump-readable stage markers around system acquisition/Invoke and record world generation.
3. Reproduce the same transition in fresh processes with trainer absent, Visibility off, and Visibility on, keeping other mods fixed.

No source changes, builds, injection, unload or process termination were performed. The observed primary stop is CPU scheduler spin, not a Present/fence wait. Ultimate initiating caller remains unconfirmed.

Artifacts: reports/artifacts/2026-09-06_freeze_pid22660/ contains logs, config, CPU sample, two live stack reports, two dumps and global-state comparison. Dumps are excluded from git. Dump stack_trace returned no candidates for these dumps; stack comparison claims refer exclusively to the separate live reports.

## Implemented follow-up

Visibility now gates metadata/system acquisition and casts on shared world readiness, invalidates queued/cache work on observed gate transitions, and checks a local generation before processing/publication. Dump-readable stage, generation and entity markers were added. Release build and initial closed-gate injection smoke check passed in PID 30040; populated-world clear/reopen reproduction remains unverified. See progress.md (2026-09-06 Visibility world-clear safety). The initiating caller in PID 22660 remains unconfirmed.
