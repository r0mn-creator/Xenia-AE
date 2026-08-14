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

---

## Step 1 progress (2026-08-13)

### Landed — the fiber foundation builds

| piece | where | status |
|---|---|---|
| `boost_context` (Boost.Context fcontext asm, incl. arm64 ELF GAS) | `third_party/boost_context` | copied, `add_subdirectory` wired, **builds** |
| `xe::threading::Fiber` class | `base/threading.h` | declared |
| Fiber implementation | `base/threading_fiber.cc` | copied, compiles |
| `xenia-base` links `boost_context` | `base/CMakeLists.txt` | done |
| `PPCContext::preempt_requested`, `last_safepoint_pc` | `cpu/ppc/ppc_context.h` | added |
| `backend::preempt_yield_handler`, `spin_backoff_yield_handler` | `cpu/backend/backend.{h,cc}` | added (null until the scheduler registers them) |
| `xe_global_mutex::is_held_by_current_thread` | `base/mutex.{h,cc}` | added (POSIX; uses the existing `owner_`) |
| `global_critical_region::is_held_by_current_thread` | `base/mutex.{h,cc}` | added |
| `guest_scheduler` / `guest_scheduler_quantum_us` cvars | `kernel/kernel_flags.{cc,h}` | added, **default OFF** (XenDroid ships ON) |
| `guest_scheduler.{cc,h}` | `kernel/` | copied, not yet compiling |

### Landed since — host-side plumbing

| piece | where | status |
|---|---|---|
| `XThread` scheduler block: `CooperativeWaitKind`, `set/clear_cooperative_wait_shape`, `cooperative_wait_set_epoch/count`, `cooperative_wait_object`, `SchedulerLinks` + accessor | `kernel/xthread.h` | added (in `protected:` so `XHostThread` sees them) |
| `XThread` members: `fiber_`, `scheduler_links_`, `cooperative_wait_object_`, `fiber_exit_event_`, `self_reference_dropped_` | `kernel/xthread.h` | added |
| `XThread::GetCurrentFiberThread` / `fiber()` / `ReclaimExited` declarations | `kernel/xthread.h` | declared |
| `XObject` cooperative API: `cooperative_signal_epoch`, `cooperative_pulse_epoch`, `Enter/LeaveCooperativeWait`, `AbandonCooperativeWait`, epoch member | `kernel/xobject.h` | declared |
| `threading::Fence::TryWait` | `base/threading.h` | added |
| `threading::PreciseSleep` | `base/threading.{h,posix.cc}` | added — **portable nanosleep form**; XenDroid's ARM WFE/event-stream path needs `cvars::wfe_precise_sleep` + `AT_HWCAP`/`HWCAP_EVTSTRM`, none of which exist here |
| `logging::GetFrameNumber` + `global_frame_number_` | `base/logging.{h,cc}` | added — counter is never advanced yet (AE has no per-present hook); only labels the scheduler's no-progress report, so 0 is harmless |

⚠️ **Two deliberate simplifications** to record so they are not mistaken for
faithful ports: `PreciseSleep` (no WFE path) and `GetFrameNumber` (static 0).

### ✅ guest_scheduler.cc now COMPILES AND LINKS

Everything the scheduler needs from the host side is in, and AEX builds clean:

* `XThread`: `HasPendingUserApc`, `OnQuantumEnd` (written in this fork's idiom -
  it has no `PublishPriority`, so priority is published by assigning `priority_`
  and mirroring into the guest `X_KTHREAD`), `GetCurrentFiberThread`,
  `ReclaimExited`, and a **static** `SetCurrentThread(XThread*)` overload
  alongside the existing instance form.
* `XObject`: the signal ring (`SignalRecord`, `RecordCooperativeSignal`,
  `RecentCooperativeSignals`), `Enter`/`Leave`/`AbandonCooperativeWait`,
  `WakeCooperativeWaiters`, `CooperativeWakeTarget`/`CooperativeMayAcquire`.
* `KernelState::guest_scheduler()` + member.

**Verified on device: AEX boots and reaches the Halo 3 menu with the scheduler
linked in, no regression.** It is inert - nothing constructs a `GuestScheduler`
and `cvars::guest_scheduler` defaults false.

### ⚠️ Remaining step 1 work (the behaviour half)

Compiling is not running. Still to do:

1. **The XThread fiber path** - `Create()` must build a `threading::Fiber` and
   register with the scheduler instead of spawning a host thread when
   `cvars::guest_scheduler` is set. This is the actual behaviour change.
2. **KernelState lifecycle** - construct the scheduler when the cvar is set,
   `EnsureStarted()`, and `Shutdown()`.
3. **Wait-path call sites** - `xboxkrnl_threading.cc`, `xevent`, `xmutant`,
   `xfile`, `xsocket`, `xiocompletion`, `xobject` must route through
   `BlockCurrentThread`/`WakeCooperativeWaiters` when the scheduler is active.
4. Register `preempt_yield_handler` / `spin_backoff_yield_handler` (step 2-3).

### How to resume

Build and read the error list - it is a precise worklist:
```
JAVA_HOME=/opt/android-studio/jbr ./gradlew :app:assembleDebug 2>&1 \
  | grep -oE "error: .*" | sed 's/error: //' | sort -u
```
Distinct errors: 8 -> 5 -> 5 (now all leaf kernel integration). Current list:

1. `KernelState::guest_scheduler` accessor + construction/shutdown.
2. `XThread::HasPendingUserApc` - does the thread have a pending user APC
   (gates alertable re-polls).
3. `XThread::OnQuantumEnd` - hook the scheduler calls at slice end.
4. `XObject::RecentCooperativeSignals` + `SignalRecord` +
   `RecordCooperativeSignal` - the signal ring for the no-progress report.
5. one "call to non-static member function without an object argument".

Then the **implementations** still have to be written in `xthread.cc` /
`xobject.cc` (`GetCurrentFiberThread`, `ReclaimExited`, `Enter/Leave/Abandon
CooperativeWait`, the fiber path in `Create()`), plus the wait-path call sites.

**Keep `guest_scheduler` default OFF until steps 1-4 are all in.** AEX must boot
identically to Canary AE until the whole chain lands - s34 proved partial
enablement wedges the guest.
