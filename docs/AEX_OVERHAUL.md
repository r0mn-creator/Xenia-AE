# Canary AEX - the overhaul line

## What AEX is, and what it is not

Three lines now exist, with different jobs:

| build | applicationId | role |
|---|---|---|
| **Xenia AE** | `org.xeniaae` | **the real emulator.** What ships to users. |
| **Canary AE** | `org.xeniaae.canary` | testing/creating line. **Kept as-is, working, untouched** - the fallback if AEX does not pan out. |
| **Canary AEX** | `org.xeniaae.aex` | this branch. The big overhaul, drawing heavily on XenDroid. |

All three install side by side, so AEX can be A/B'd against Canary AE on one
device without reflashing.

If AEX works out it becomes the new test line feeding Xenia AE. If it does not,
Canary AE is untouched and nothing is lost.

## Why AEX exists

`docs/HALO3_VISTA_46_VS_64.md` s35. The investigation into Halo 3's inverted
vista ended somewhere unexpected: **the renderer was never the problem.**
Measured, symmetrically, against XenDroid on the same device:

* Canary AE's guest issues **4.6-7x fewer draw commands per frame** (88-138 vs
  611-641), and PM4 packet parsing is provably lossless - the draws are simply
  never submitted (s30, s31).
* The whole command stream is **3x thinner**, and AE is **~5x slower per unit of
  guest work** - 15 FPS processing a third of what XenDroid does at 24 FPS
  (s33).
* Guest behaviour is **non-deterministic run to run** (s22, s30/s31) where
  XenDroid is stable.
* Every GPU-side stage - viewport maths, NDC, translated SPIR-V, resolve, dump,
  transfer, guest constants - measured **identical** (s14-s23).

Root cause identified (s35): **XenDroid runs guest threads as cooperative fibers
on its own scheduler (`guest_scheduler = true`, 1 ms quantum). Canary AE runs
1:1 host threads and has no scheduler at all.**

## The work, in order (steps 1-4 are a UNIT)

1. `kernel/guest_scheduler.{cc,h}` + `XThread` integration (fibers, not host
   threads); `guest_scheduler`, `guest_scheduler_quantum_us`,
   `fiber_reentry_longjmp` cvars.
2. `PPCContext::preempt_requested` / `last_safepoint_pc`;
   `backend::preempt_yield_handler`.
3. HIR `OPCODE_CHECK_PREEMPT` + `HIRBuilder::CheckPreempt`; a64
   `EmitPreemptCheck` + `CHECK_PREEMPT` sequence.
4. `PreemptCheckInjectionPass`.
5. Only then enable `park_memory_poll_loops` / `collapse_memory_delay_spins`
   (already ported on this branch, default OFF).

⚠️ **Partial ports wedge the guest.** Proven: s34 ported steps 5's passes without
1-4 and Halo 3 stalled on the legal screen at 1.6 FPS with the PM4 stream
collapsing from ~4.37M packets to ~8K.

## Already on this branch (inherited from xd-memexport-transplant)

* memexport eA validation fix (correct; not the ball)
* host-visible shared memory + `ReadbackResolveMode::kUma` (**image clearer** -
  user-confirmed win)
* memexport page tracking + fence/coherency awaits
* MemoryPollPark / DelayCountdownCollapse passes (**default OFF**, regress
  without steps 1-4)
* Two real Canary AE bug fixes: the dead `gamma_render_target_as_srgb_`
  assignment, and aligned pitch/height into the tiled-address helpers

## Verification harness (built, default OFF)

`debug.canary.pm4total`, `pm4draw`, `drawentry`, `bonedistinct`, plus
`vsconst`, `regtrace`, `ndcy_draw`. XDtester carries the **same probe names and
formats**, so the two logs diff line-for-line.

**Success looks like:** AE's draws/frame and packets/frame approaching
XenDroid's, and run-to-run variance collapsing.
**Cheap visual proxy:** the Halo 3 menu vista (s26) - if it is still inverted,
do not bother with the expensive in-game ball test.
