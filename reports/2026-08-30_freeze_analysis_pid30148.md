# Cyberpunk 2077 PID 30148 freeze analysis

## Incident summary

- Time: 2026-08-30 12:59:43 KST
- Game: Cyberpunk 2077 2.31, PID 30148
- Trainer build: Phase 2B (`dc0b34a`), injected at 11:31:58 KST
- Symptom: the window remained alive but `Responding=False` for three consecutive watchdog polls. CPU did not
  advance during a subsequent two-second sample and the trainer log heartbeat stopped at 12:59:42.
- The failure happened about 87 minutes 45 seconds into the Phase 6 validation window.

## Evidence

The trainer fatal sink recorded a first-chance access violation on the game main-tick thread:

```text
[FATAL][veh] 12:59:43.035 pid=30148 tid=29636
  code=0xC0000005 EXCEPTION_ACCESS_VIOLATION
  at=Cyberpunk2077.exe+0xA037C8
  access=WRITE target=0x0000000000000049
  fault-in-trainer=no
  stack +0x28: cp2077_trainer.dll+0x14748
  stack +0x58: cp2077_trainer.dll+0x12BC2
```

PDB symbolization resolves the trainer addresses as:

- `+0x14748`: `SetBraindanceModeOnMainTick+0x38`
- `+0x12BC2`: `ProcessNativeHighlightsOnMainTick+0x382`

The Release DLL disassembly makes the first address decisive:

```text
180014744  mov edx, ebx
180014746  call rax
180014748  mov byte ptr [g_nativeHighlightModeActive], bl
```

Thus `+0x14748` is the return address immediately after the indirect
`g_highlightRuntime.setBraindanceMode(g_highlightRuntime.visionModeSystem, ...)` call. The exception itself occurred
inside the resolved engine function.

The engine crash manifest agrees with the fatal record:

- `stopThreadID=29636`
- `EXCEPTION_ACCESS_VIOLATION`, write target `0x49`
- `uptimeSeconds=11271`
- `Game/SessionDesc/IsLoadingSavedSession=true`
- `Streaming/IsLastObserverPositionValid=false`
- `Streaming/DebugLastTeleportDistance=2098.310303`

Immediately before the fault, the trainer log changed from 26 tracked snapshots to:

```text
[12:59:42.986] ESP diagnostics: snapshots=0 tracked=0 ... unregistered=22290
```

This is consistent with a save/world transition invalidating runtime system objects while the main-tick hook remains
active.

## Root cause

`ResolveNativeHighlightOnMainTick` cached a raw `gameIVisionModeSystem*` in
`g_highlightRuntime.visionModeSystem`. Once the resolver had succeeded, its fast path returned without reacquiring the
system. During the saved-session transition the tracker became empty, but the trainer still believed braindance mode
was active. `ProcessNativeHighlightsOnMainTick` therefore attempted to turn the mode off through the cached, now stale
system pointer. The engine member function dereferenced invalid state and entered its crash handler.

The observed write target `0x49` is a near-null member write from the stale/invalid engine call; it is not an attitude
component RTTI fault.

The same audit also identified a separate lifetime gap in the highlight path: `HighlightWork` copied a raw Entity
pointer under the tracker lock and invoked `QueueEvent` after releasing the lock, while `UnregisterEntity` runs mostly
off-main. Phase 2B already stores an Entity strong Handle, so highlight work must copy and retain that Handle through
the event call instead of relying on the raw pointer.

## Attitude Phase 2B result before the unrelated highlight freeze

The final complete lifetime summary before the transition reported:

```text
attitudeAttempts=579786
attitudeAcquire=579786
attitudeExpiredInvalid=0
attitudeUnknown=0
attitudeFailClosed=0
```

No trainer-origin first-chance AV occurred in the Phase 2B attitude path. This session therefore supports the retained
Entity plus retained `GetAttitudeAgent` result design, but it cannot satisfy Phase 6 because another trainer feature
froze the process before 90 minutes.

## Corrective action

1. Reacquire `gameIVisionModeSystem` on the game main tick immediately before each braindance-mode transition; never
   keep the raw system object across ticks or world/save transitions.
2. Apply the same no-long-lived-raw-system rule to other cached game-system instances used by health and attitude
   batches where appropriate. Static RTTI metadata and verified native function addresses may remain cached.
3. Make `HighlightWork` own a copied Entity strong Handle from the tracked slot, release it outside the tracker lock,
   and keep it alive through `QueueEvent`.
4. Fail closed when fresh system acquisition or Handle acquisition is unavailable. Do not use SEH as normal lifetime
   control flow.
5. Rebuild, reinject into a restarted game, and restart the 90-minute Phase 6 window.

## Preserved artifacts

- `reports/artifacts/2026-08-30_freeze_pid30148/cp2077_trainer.log`
- `reports/artifacts/2026-08-30_freeze_pid30148/cp2077_fatal.log`
- `reports/artifacts/2026-08-30_freeze_pid30148/Cyberpunk2077.exe-20260830-095149-30148-29636.txt`

