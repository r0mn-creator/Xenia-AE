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

---

## ✅ RESOLVED: AEX black screen was a CONFIG DEFAULT, not code (2026-08-13)

With Turnip R8 installed via the driver UI, **AEX renders but presents black**.
Canary AE with the same driver on the same device shows the menu.

### What is ruled out

* **Not the driver.** `vulkan_lib_path` resolves to
  `.../Turnip_v26.0.0_R8/vulkan.ad07xx.so` and the device enumerates as
  "Turnip Adreno (TM) 740". R7 was tried first and also black - consistent with
  the standing rule that *newest is not best*, but R8 is what Canary AE uses.
* **Not the guest failing to run.** The log shows the guest actively producing
  GPU work: **36,277 LOADDEST** lines and **943** resolve/memexport lines, at
  the same 1152x640 render targets Canary AE uses.
* **Not shader compilation.** `cache/pipelines_4D5307E6.bin` exists and the
  screen stays black well past the point Canary AE reaches the menu.
* **Not the PPCContext layout** (though that WAS a real bug - see below).

So the emulation is running and the **presentation** is black.

### One real bug found and fixed on the way

`preempt_requested` / `last_safepoint_pc` were first added next to
`thread_state`, in the middle of `PPCContext`. **The JIT addresses that struct
by offset off x20**, so inserting mid-struct shifted every later field. Now
appended at the end. This was a genuine defect independent of the scheduler and
must never be reintroduced - if new context fields are ever needed, append them.

### How to isolate it next

AEX = `xd-memexport-transplant` + the scheduler foundation. Bisect the
foundation commits on a scratch branch, testing the Halo 3 menu after each:

* `06375414b` fiber foundation (boost_context, Fiber, PPCContext fields,
  backend handlers, mutex helper, cvars)
* `bbbdf4d86` XThread/XObject plumbing + base helpers
* `efc5f25a4` scheduler compiles/links + XThread/XObject implementations
* `<this>` PPCContext fields moved to end

Prime suspects, in order: the XThread member additions (`fiber_`,
`scheduler_links_`, `cooperative_wait_object_`, `fiber_exit_event_`), the two
new XObject virtuals, and `threading.h`'s new `Fiber` declaration.

⚠️ Note AEX's config is **fresh** and may differ from Canary AE's long-lived one
(e.g. `readback_resolve` defaults to `"none"` here vs the `false` -> kFast that
Canary AE's config yields). Diff the two TOMLs before assuming a code cause.


### Resolution

**`vulkan_sparse_shared_memory` black-screens with a custom driver.**

| cvar | Canary AE | fresh AEX config |
|---|---|---|
| `vulkan_sparse_shared_memory` | **false** | **true** |
| `headless` | false | true |
| `defaults_date` | 2020123113 | 2026040912 |

Canary AE's config dates from 2020 and therefore **never applied newer cvar
default migrations**. AEX's config is fresh, so it took the current defaults -
including `vulkan_sparse_shared_memory = true`.

Setting it to `false` (matching Canary AE) made AEX render the Halo 3 menu with
Turnip R8 immediately. That also explains the exact symptom pattern the user
spotted:

* **stock driver -> navy menu renders** (sparse works on the Qualcomm driver)
* **custom driver -> black** (sparse binding differs on Turnip)

⚠️ **This is a real bug for END USERS, not just AEX.** Any *new* install gets a
fresh config with `vulkan_sparse_shared_memory = true`, so a new user who
installs a custom driver gets a black screen. Existing users are shielded only
by their stale `defaults_date`. **This should be fixed in Xenia AE and Canary AE
too** - either default it false on Android/Adreno, or force it off whenever a
custom `vulkan_lib_path` is set.

None of the AEX scheduler work was implicated. The one real code bug found while
chasing this was the PPCContext field-offset hazard, fixed separately.

---

## Step 1, final piece: the wait paths (DESIGNED, NOT YET PORTED)

Everything else in step 1 is done. `guest_scheduler=true` starts the scheduler
("GuestScheduler: preemption slice = 1000 us (19197 ticks)"), the process stays
alive with the frame limiter running, and the screen is **black** - guest
threads are fibers that park on waits, but the wait code still calls the
host-thread blocking path, so nothing wakes them.

### What has to be ported, as ONE unit

From XenDroid's `xobject.cc`:

1. **`WaitExit(kthread, result)`** - writes `thread_state = KTHREAD_STATE_RUNNING`
   and `wait_result` back into the guest `X_KTHREAD` on every exit path.
2. **`template <PollFn> CooperativeWait(scheduler, kthread, wait_object,
   alertable, deadline_ms, poll)`** - the poll-yield loop. Order matters:
   * check `HasPendingUserApc()` first when alertable -> `X_STATUS_USER_APC`;
   * **sample the epoch BEFORE polling** (`cooperative_signal_epoch()` for a
     single object, `cooperative_wait_set_epoch()` for a multi-wait) so a signal
     landing after a failed poll is not skipped - this is the subtle part;
   * run `poll()` (a zero-timeout acquire returning the terminal status, or
     nullopt); polling the host primitive preserves exact acquire semantics and
     only the *blocking* becomes cooperative;
   * deadline check against `Clock::QueryHostUptimeMillis()`;
   * else `scheduler->BlockCurrentThread(deadline_ms, wait_epoch, alertable)`.
3. **`SignalObjectCooperatively(object)`** - the KeSetEvent / KeReleaseSemaphore
   / KeReleaseMutant switch.
4. Route **`XObject::Wait`**, **`WaitMultiple`**, **`SignalAndWait`** through it
   when `kernel_state()->guest_scheduler()` is active, calling
   `EnterCooperativeWait`/`LeaveCooperativeWait` around the park for FIFO
   fairness (semaphores) and `set_cooperative_wait_shape()` for the diagnostics.
5. **Wake side**: `WakeCooperativeWaiters()` after the host primitive is
   signalled - never before - in `xevent`, `xmutant`, `xsemaphore`,
   `xiocompletion`, `xfile`, `xsocket`.

### Why it must land together

⚠️ Twice today a partial port of a coupled unit produced a **silent wedge**, not
an error: the JIT park passes without their preemption dependency (s34), and the
scheduler enabled without these wait paths. Routing *some* waits and not others
would do the same. Port all of it, then flip `guest_scheduler`.

### State to resume from

* Branch `canary-aex`, `guest_scheduler` **false** in config and cvar default.
* AEX builds, installs as `org.xeniaae.aex`, runs Halo 3 with Turnip R8.
* Fiber path + KernelState lifecycle are in and verified inert.
* Probes are built and default OFF: `pm4total`, `pm4draw`, `drawentry`,
  `bonedistinct` - XDtester has the same names/formats for line-for-line diffs.
* Success = AEX's draws/frame and packets/frame approaching XenDroid's
  (~611-641 vs our ~88-138) and run-to-run variance collapsing.

### Landmine removed (read before wiring the wake side)

`KernelState` constructs the `GuestScheduler` **unconditionally**, so
`kernel_state()->guest_scheduler()` is always non-null. Guard cooperative paths
on **`GuestScheduler::enabled()`**, never on the pointer alone - a pointer-only
check calls into a scheduler that was never started. `WakeCooperativeWaiters`
has been corrected; apply the same rule to every new call site.

---

## Wait paths WIRED (2026-08-14) - scheduler now asserts during kernel init

`XObject::Wait` and `XObject::WaitMultiple` take a cooperative poll-yield path
when `GuestScheduler::enabled()` **and** the caller is fiber-backed
(`XThread::GetCurrentFiberThread()`). Wake side wired into `XEvent::Set/Pulse`,
`XSemaphore::ReleaseSemaphore`, `XMutant::ReleaseMutant`, always **after** the
host primitive is signalled.

### Crash found and fixed by actually running it

`WakeCooperativeWaiters()` ran `RecordCooperativeSignal()` **unconditionally**.
That walks `thread_state()->context()->lr`, which is not valid for host threads,
so once the wake side was wired the emulator **died on launch with the scheduler
OFF**. The whole body is now scheduler-gated and the record is null-checked.

⚠️ Lesson: anything called from a hot kernel path must be gated on
`GuestScheduler::enabled()` **first thing**, before touching any thread state.

### Current status with `guest_scheduler = true`

The scheduler is now genuinely exercised - and asserts (SIGTRAP / SI_TKILL)
during **kernel init**, with the log ending at:

```
Setup: Initializing Kernel...
FindProfiles: Adding profile ... to profile list
ProfileManager: Found 1 Profiles
LoadAccount: Loading Acc<cut>
```

That is the point where the first guest threads are created, i.e. the **fiber
creation path in `XThread::Create`** is being hit for the first time. This is
progress: previously the scheduler started and everything silently stalled;
now it runs far enough to fail at a specific, findable place.

### Next debugging step

Get the assert's identity - run with logcat capturing the abort message, or add
a log line either side of the fiber `Create()`/`EnsureStarted()`/`MarkReady()`
sequence in `XThread::Create` to see which one trips. Prime suspects:
1. `EnsureStarted()` being called before the scheduler's CPUs/dispatch threads
   are ready.
2. `MarkReady()` on a thread whose `SchedulerLinks` were never initialised for
   the queue it is being pushed onto.
3. An assert inside `GuestScheduler` about the global critical region being held
   at a switch point (`is_held_by_current_thread`).

**`guest_scheduler` is back to false and AEX renders the Halo 3 menu normally.**

---

## Steps 2-4 landed; blocker is now precisely located (2026-08-14)

Ported ~150 targeted lines (NOT a wholesale copy): `OPCODE_CHECK_PREEMPT`,
`HIRBuilder::CheckPreempt`, `A64Emitter::EmitPreemptCheck`, the a64
`CHECK_PREEMPT` sequence, and `PreemptCheckInjectionPass` registered in the
`PPCTranslator` constructor.

### What the watchdog now proves

```
CPU 0 running tid=00000006 'Main XThread' preempt_requested=1
GuestScheduler: CPU 0 has not switched fibers in 2000 watchdog ticks
```

**`preempt_requested=1`** - the scheduler's quantum timer IS raising the flag.
The fiber simply never tests it, so it never yields and every other CPU starves.

### The remaining defect: the injection pass never runs

`PreemptCheckInjectionPass::Run()` is **never called**. Proven:
* a probe at the very first line of `Run()` (before any early return) produces
  **no output at all**;
* the probe strings ARE in the shipped binary (`strings libe.so | grep
  PREEMPTRUN` = 1), and the object file exists, so this is not a stale build;
* the pass IS registered - `ppc_translator.cc:62`, unconditionally, in the
  `PPCTranslator` constructor.

So the pass is compiled, linked and registered, yet its `Run` never executes.
**That is the next thing to solve.** Candidates:
1. `Compiler::Run` may skip passes it does not recognise, or iterate a list the
   constructor-added pass is not in (check how `AddPass` stores them and whether
   anything filters by pass name/type).
2. This fork may not route Halo 3's translation through `PPCTranslator` at all -
   confirm by putting the same probe in `ControlFlowAnalysisPass::Run`, which is
   registered on the very next line. **If that probe is also silent, the whole
   pass pipeline is bypassed and the problem is upstream of this pass.**

That second check is the cheapest next experiment and cleanly splits "my pass is
special" from "no passes run here".

### Diagnostic trap recorded

`last_safepoint` in the watchdog is **always 0** in this port - XenDroid only
records it under `cvars::log_safepoint_pc`, deliberately not ported. Do not read
that field as evidence about safepoints.

`guest_scheduler` is back to false; AEX renders the Halo 3 menu normally.
