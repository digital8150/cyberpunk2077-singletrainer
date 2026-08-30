# Cyberpunk 2077 PID 29084 GPU crash analysis (DLSS preset change)

## Incident summary

- Time: 2026-08-30 14:29:06.774 KST (game telemetry `timeCrash=2026-08-30T05:29:06Z`)
- Game: Cyberpunk 2077 2.31, PID 29084, injected 14:02:07, session length 1981 s
- Trigger reported by the user: changing the DLSS quality preset (Quality -> Balanced) in the graphics menu
- Outcome: hard crash. `isOom=false`. No Windows TDR and no Application Error event.

## Timeline (trainer log, same PID)

```text
14:17:56.275  safe unload requested            <- user pressed End 11 min before the crash
14:17:58.763  safe unload is waiting for hook cleanup; retrying in 1000 ms   (repeats until the crash)
14:29:06.219  ResizeBuffers intercepted; releasing overlay resources         <- the DLSS preset change
14:29:06.461  overlay initialization started: swapChain=0000015A5E054400 queue=00000159A23C01C0
14:29:06.461  swap-chain desc: buffers=2 format=28 size=2560x1440 flags=0x840   (identical to 14:02)
14:29:06.467  overlay initialization completed
14:29:06.475  first overlay frame submitted: buffer=0 fence=1
14:29:06.774  overlay fence reports device removal / hr=0x887A0001
14:29:06.778  device watchdog observed removal / hr=0x887A0001
14:29:06.776  [FATAL][unhandled] 0x80000003 at Cyberpunk2077.exe+0x2A43F4B, rsi=0x887A0005
```

The swap-chain pointer and command queue are unchanged across the resize, and the reported size is unchanged
(2560x1440). This was an upscaler reconfiguration, not a window or output resolution change.

## Failure class

The engine's own Aftermath sink (`%TEMP%\gpucrash-2026-08-30_14.29.06.774.log`) reports:

```text
Device Removed Reason: 0x887a0001 :
GPU Crash Reason: Unknown
```

`0x887A0001` is `DXGI_ERROR_INVALID_CALL`: the D3D12 runtime removed the device because the application made an
invalid API call. This is a CPU-side removal, not a GPU fault. Corroboration:

- No page fault (`PageFaultVA: 0000000000000000`), both DRED allocation lists empty.
- No `nvlddmkm` TDR event and no `Application Error` entry in the Windows event log between 14:00 and 14:45.
- The trainer `[FATAL]` record is `0x80000003` (breakpoint) inside the engine's own fatal handler with
  `rsi=0x887A0005` (`DXGI_ERROR_DEVICE_REMOVED`) and `D3D12Core.dll` / `nvwgf2umx.dll` / `d3d12.dll` on the
  stack. That is the engine reacting to the removal, not the origin of it.

Aftermath breadcrumbs for the dying frame 398899 show every pass up to `DecoupledParticleLighting` finished and
`Transparents` onward `Not started`, which is consistent with the device dying mid-frame but does not localize
the offending call.

## This is a new failure, not a known path

- All prior GPU deaths in this project (2026-08-27/28, 8 occurrences) were `0x887A0006` `DEVICE_HUNG` (plus one
  `0x887A002B`), correlated with **menu open**, and were root-caused to the ImGui DX12 vertex-buffer ring being
  2 deep while up to 32 overlay frames were in flight under DLSS Frame Generation. Fixed by setting
  `NumFramesInFlight = kMaximumAllocatorCount`.
- Since that fix there were **no GPU crashes for two days** (last one 2026-08-28 08:27). This one is the first,
  with a **different reason code**, a **different trigger** (swap-chain resize, not menu open), and the overlay
  was **not visible** (last `overlay visibility toggled: visible=0` at 14:20:22).
- The 2026-08-28 10:12 crash documented in `progress.md` was a USB-SSD I/O stall and is unrelated (no device
  removal was recorded there).

The previously fixed defect is therefore not recurring.

## Attribution: not yet decidable, and the reason is a stale DRED registration

DRED is the one instrument that would name the offending call, and it was **silently off for this session**:

```text
GetAutoBreadcrumbsOutput1 failed: hr=0x887A0004   (DXGI_ERROR_UNSUPPORTED)
DRED: Failed to get D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1. HR=0x887a0004
```

`d3dconfig dred` still reports `force-on` for everything, but `d3dconfig apps` contains only:

```text
g:\steamlibrary\steamapps\common\cyberpunk 2077\bin\x64\cyberpunk2077.exe
```

The game is no longer installed there. `libraryfolders.vdf` places app 1091500 in the
`C:\Program Files (x86)\Steam` library, and `G:\SteamLibrary\steamapps\common` no longer contains a
Cyberpunk 2077 directory or an `appmanifest_1091500.acf`. The DRED opt-in list is scoped to a path the game has
not run from since the install was moved off the G: USB enclosure, so DRED has been inert ever since.

### What the evidence does and does not support

Points away from the trainer:

- The overlay ran for roughly 48 presents (~299 ms at the measured ~160 fps) after re-initialization before the
  device died. A malformed re-init (bad RTV, wrong format, unclosed command list) removes the device on the
  first `ExecuteCommandLists`, not 48 frames later.
- The identical steady-state overlay submit path had run for 27 minutes in this session, and for days before,
  without a removal.
- `ResizeBuffers` was intercepted and survived on earlier occasions in this same log with no removal.
- The overlay allocator pool is stable at 4 with `allocatorMisses=0`, and the fence wait in
  `ReleaseOverlayResources` never timed out (`shutdown fence wait skipped` appears zero times in the whole log).

Points that keep the trainer in scope:

- The removal is 313 ms after the overlay re-initialized on the just-resized swap chain, and the overlay
  re-initializes on the **very first Present after `ResizeBuffers`**, i.e. exactly while Streamline is rebuilding
  its DLSS-SR / DLSS-RR / DLSS-G contexts. The two "first frames after resize" overlap in that window.
- `hkResizeBuffers` (`src/hooks/d3d12_hook.cpp:67`) discards the `HRESULT` of the original `ResizeBuffers`, so a
  failed resize would be invisible to us.

Points that keep the game and DLSS stack in scope:

- The user configuration is the fragile one: `FrameGeneration=DLSS`, `DLSSFrameGen=true`,
  `DLSS_MultiFrameGeneration=x2`, `DLSS_D=true` (Ray Reconstruction), `DLSS_BackendPreset=Transformer`,
  `RayTracing=true`. Changing the upscaler preset tears down and rebuilds all three Streamline contexts plus the
  proxy swap chain in a single step.
- The process has NVIDIA Streamline, AMD FSR3/FidelityFX and Intel XeSS upscaler runtimes all loaded at once,
  and the mod stack (RED4ext, CET, ArchiveXL, TweakXL, RedHotTools, ...) is live.

`DXGI_ERROR_INVALID_CALL` names the failure class but not the caller. With DRED off, this crash contains no
evidence that distinguishes the three candidates.

## Separate defect found: safe unload livelocks and never completes

Unrelated to the GPU crash, but the trainer should not have been resident at all:

```text
14:17:56.275  safe unload requested
...           hook shutdown started -> no-recoil cleanup timed out: targetId=0x9AFDC9
              -> hook shutdown aborted: main-tick feature cleanup did not acknowledge safely
              -> retry, every ~3.5 s, for 11 minutes, until the crash
```

`Game::PlayerModifiers::PrepareForShutdown` (`src/game/player_modifiers.cpp:518`) sets `g_cleanupRequested` and
waits 2500 ms for `g_cleanupAcknowledged`. The acknowledgement only happens if `RemoveModifiers()` succeeds on
the game main tick (`OnGameMainTick`, `src/game/player_modifiers.cpp:440`); it never did for target `0x9AFDC9`.
`HookLifecycle::Shutdown` (`src/hooks/d3d12_hook.cpp:272`) aborts before `MH_DisableHook`, so the hooks stayed
live — the retry loop is not disabling and re-enabling detours, which is why the resize was still intercepted
normally.

Two consequences worth fixing:

1. The abort path never clears `g_cleanupRequested`, so from 14:17:56 onward `requested` is permanently forced
   to `false`, No-Recoil is silently dead, and the main tick calls `RemoveModifiers()` on every tick for
   11 minutes.
2. `Features` republishes `PublishDesired(g_settings.noRecoil)` each cycle (`src/features/features.cpp:39`),
   which is why `no-recoil desired settings published: enabled=1` reappears right after every
   `hook shutdown started`. The unload cannot converge while the setting stays enabled.

The retry loop in `src/dllmain.cpp:46` has no attempt cap, so a user who presses End gets a trainer that stays
injected indefinitely with no visible feedback.

## Recommended next steps

1. **Re-register DRED against the real executable path** (highest value, cheapest). Without this the next
   occurrence produces the same empty dump:
   ```powershell
   d3dconfig apps --clear
   d3dconfig apps --add "C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077\bin\x64\Cyberpunk2077.exe"
   ```
   Settings are read at device creation, so the game must be restarted.
2. **A/B the trigger.** With the trainer *not* injected, change the DLSS preset ~10 times back and forth. Then
   repeat with the trainer injected but the overlay never opened. This is a discrete, repeatable user action and
   is a far better experiment than the earlier headless-window idea.
3. **Harden the resize path** regardless of the outcome: log the `HRESULT` of the original `ResizeBuffers`, and
   defer overlay re-initialization by a few presents after a resize instead of re-initializing inside the first
   Present, so the overlay is not building resources while Streamline is rebuilding its own.
4. **Fix the unload livelock**: clear `g_cleanupRequested` on the abort path, have `PrepareForShutdown` treat a
   repeatedly failing `RemoveModifiers()` as fail-closed after a bounded number of attempts, and cap the
   `dllmain.cpp:46` retry loop with a visible failure message.

## Preserved artifacts

- `reports/artifacts/2026-08-30_gpucrash_pid29084/gpucrash-2026-08-30_14.29.06.774.log` (engine Aftermath sink)
- `reports/artifacts/2026-08-30_gpucrash_pid29084/cp2077_fatal.log`
- `reports/artifacts/2026-08-30_gpucrash_pid29084/cp2077_trainer_session_pid29084.log`
- `reports/artifacts/2026-08-30_gpucrash_pid29084/CrashInfo.json`
