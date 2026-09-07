# Per-pellet silent aim, Cyberpunk 2077 2.31

## Evidence and boundary

The operator identified the capture-3 weapon as DB-4 Igla (six projectiles). Static disassembly of the installed executable establishes an explicit loop in RVA `0x659C5C..0x65A088`:

- Receiver is carried in R14; the second argument is carried in RBX.
- `0x659DF7` tests the byte at second argument + `0x1C0`.
- `0x659E4C` calls wrapper `0x2048B7C` once per iteration.
- `0x659EC3` increments R15B; `0x659EC6` compares it with that same count and branches back.
- The wrapper's virtual call returns at `0x2048BED`, matching the six-call groups in capture 3.

The wrapper passes aim-parameter + `0x80` as an in/out rotation. The shared core at `0x4D8354` generates a spread quaternion when its eighth argument is true (`0x4D8865..0x4D889F`), writes it to its fifth argument, and composes it with camera orientation. The aim-parameter copy routine at `0x5150D0` copies all 16 rotation bytes at + `0x80`. The later wrapper at `0x6573A8` loads those 16 bytes, passes a local copy with generation disabled, and returns at `0x657410`.

These instructions support exact rotation matching across worker threads. The new live trace below verifies first-pass/replay pairing; the old trace did not record quaternion values. The seven later calls must not be blindly treated as seven pellets.

## Implementation

- Hook the enclosing native function, validating all bytes of that function and both wrappers with fixed 2.31 FNV-1a checksums before installing. A mismatch disables only the new pellet path.
- Only arm a shot scope when the receiver equals the current local equipped-weapon identity published by the existing main-tick lookup within 1000 ms. The numerical identity is never dereferenced after the weapon handle is released. Read the count from the live function argument; accept 2..64 for distribution. No timing-based shotgun classification, new VM invocation, or new engine query.
- A thread-local scope freezes the published target-region coordinates once per shot and counts only calls from the verified first wrapper. Nested scopes restore their parent; `__finally` also restores the pointer on SEH unwinding.
- Present supplies up to five eligible points, one closest-to-crosshair joint for each selected group, on the already selected entity. Points must meet projection, range and FOV filters. Missing groups are omitted. Two or more eligible groups cycle in Head/Neck/Chest/Arm/Leg order. Head+Chest with six pellets gives Head/Chest/Head/Chest/Head/Chest.
- A fixed 256-entry cache maps `(targeting-system identity, exact four quaternion words)` to shot/index/count/target. Later calls from the second wrapper recover that same target without consuming a global round-robin counter. Each cell has a one-attempt atomic claim only while copying POD; no lock is held during any game call. Contention, expiry, ambiguity and missing mappings fall back to ordinary silent aim. Cache expiry is 350 ms, solely a lifetime limit.
- Profile invalidation or activation release invalidates cached mappings with an epoch. Nearest, a single available region, and no-spread retain ordinary closest-bone targeting. No-spread disables distribution because identical rotations cannot identify individual pellets through the copied data. No game rotation is overwritten to smuggle an identifier.
- The existing projectile/throwing-knife path is unchanged. The new hook supplies the firearm loop only.

## Trace additions

Kinds 6/7 are shot begin/end: `event=shot serial`, `object=weapon`, `context=second argument`, `id=epoch`; flags low byte is expected count, next byte is region count (begin) or observed first-pass count (end).

Kinds 8/9 are first-pass/replay: `event=shot serial`, `id=bone mask bit`, flags low byte is index, next byte total count, bit 16 means distribution mapping used. `origin` is frozen target, `before` is quaternion XYZ, `after` is final direction. A replay miss has no shot identity and is not assigned by temporal proximity. These records remain F8-only and use the existing bounded asynchronous trace ring.

`shot_trace_report.py` now lists explicit shots, their expected/observed counts, assigned bones and recovered replay indices. A normal extra downstream query may remain unmatched; success means the six actual indices each recover their own mapping, not merely six arbitrary matches.

## Validation

- Release build succeeded; CTest config_profiles, shot_trace and pellet_targets passed.
- Cache tests cover 3/3 assignment, exact-key discrimination, different system identities, invalidation, expiry, duplicate-key rejection and eight concurrent producers/readers.
- Three Python report tests passed, including explicit shot IDs, reordered worker output and an unrelated extra replay query.
- Korean/light UI preview inspected: both helper lines fit inside the bone-selection card.
- Safe unload/reinjection in PID 20628 succeeded. At 20:20:39.704 the live log confirmed the new pellet loop hook; subsequent main ticks and ESP updates continued.
- Latest live capture received while documenting: capture 2, START QPC 386222360463, duration 15860.968 ms. All 562 sequence IDs 1002..1563 are contiguous and unique, START/STOP present, dropped=0. Analyze this latest capture only; previous attempts are not needed for the result.
- Shots 5..11: all seven report expected=observed=6 and two regions. Every shot assigned `[Head, Chest, Head, Chest, Head, Chest]`; all 42 replay records recovered indices 0..5 exactly once per shot, with identical bone IDs and frozen world targets to their first-pass records. Seven additional replay queries remained unmatched and did not consume a pellet index.
- Weapon metadata was consistently six; firing used silent/profile1, bone_mask=5, Nearest/no-spread/no-recoil off. At the end of capture the user switched to profile2/Nearest after the seven shots; it is not another distribution sample.
- This verifies per-pellet direction assignment through the downstream crosshair path. Damage events/actual collision bone IDs were not instrumented, so the trace does not independently prove each pellet's final hit location.
