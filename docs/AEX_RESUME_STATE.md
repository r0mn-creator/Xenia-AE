# AEX — RESUME STATE (single source of truth)

**Read this first. It is written to be enough on its own.**
Last updated 2026-08-14.

## The goal

Halo 3's menu vista renders **upside down** in Canary AE and Canary AEX.
Fixing it is the objective. The character models are also collapsed to a "ball";
the two are linked (user's call: *"if the vista is upside down, there's a very
good chance the character is a ball"*). **The vista is the cheap proxy** — it
renders at the main menu ~45 s from launch; the ball needs a full in-game run.

## The three builds

| build | applicationId | role |
|---|---|---|
| Xenia AE | `org.xeniaae` | the real emulator, what ships |
| Canary AE | `org.xeniaae.canary` | test line, **kept working**, the fallback |
| **Canary AEX** | `org.xeniaae.aex` | branch `canary-aex`, the scheduler overhaul |

All three install side by side on the Odin 2 (`adb -s 3a478943`).

## THE CENTRAL FINDING — the renderer is not the bug

Measured symmetrically against XDtester (instrumented XenDroid) on the same
device, same game, same driver:

| | Canary AE | XDtester |
|---|---|---|
| draws/frame entering `IssueDraw` | **88–138** | **611–641** |
| total PM4 packets/frame | 5,251 | 15,692 |
| FPS | ~15 | ~24 |
| distinct bone-matrix states seen by draws | saturates ~130 | >512 (probe cap) |

* PM4 parsing is **provably lossless** (`PM4DRAW == DRAWENTRY` exactly), so the
  draws are **never submitted** — not dropped by us.
* AE is **~5× slower per unit of guest work** (15 FPS doing a third of the
  command work XenDroid does at 24 FPS).
* Guest behaviour is **non-deterministic run to run**; XenDroid is stable.
* Every GPU stage — viewport maths, NDC, translated SPIR-V, resolve, dump,
  transfer, guest constants (29/37 shaders byte-identical, view-basis
  determinant +1 in both) — measured **identical**.

**Root-cause hypothesis (unproven):** XenDroid runs guest threads as
**cooperative fibers** (`guest_scheduler=true`, 1 ms quantum); AE runs 1:1 host
threads. That is the one architectural difference explaining the numbers.

## WHERE THE WORK STANDS — the blocker in one paragraph

Steps 1–4 are ported and build. With `guest_scheduler=true`, the scheduler
starts, fibers are created, `preempt_requested=1` is raised — **but no JIT
compilation happens at all** (not even `ControlFlowAnalysisPass` logs, which
fires normally with the scheduler off). Watchdog shows
`Running tid=6 'Main XThread' lr=00000000`. **The main fiber is stuck in HOST
code before executing a single guest instruction.** Safepoints exist only inside
JIT'd guest code, so they cannot preempt it.

## ▶️ NEXT ACTION

**Two fixes just landed** (`XThread::Delay` host-slept, blocking the whole
dispatch thread; `XThread::Execute` deref'd `thread_->system_id()` which is NULL
on the fiber path). Guest code now executes under the scheduler.

Current state with `guest_scheduler=true`:

```
CPU 0 running tid=7 'MAIN_THREAD' lr=82589EC4   <- real guest code
       ready  tid=6 'Main XThread' lr=8219832C
       blocked tid=8 'ASYNC_IO' on semaphore wait=single[1] gated=1
CPU 1-5 idle
```

**The cooperative WAIT path is proven working** — ASYNC_IO parks correctly on a
semaphore. JIT compilation runs. But **CPU 0 never switches**: MAIN_THREAD holds
it with `preempt_requested=1` while tid=6 sits ready *on the same CPU* and CPUs
1–5 idle. So the injected safepoints are not preempting.

### Verified since (narrowing the preemption failure)

* **Safepoints ARE emitted.** `PREEMPTINJECT functions=1 checks_emitted=4 ...
  functions=5 checks_emitted=9` - the pass runs and injects at loop heads.
* **The context register is correct.** `EmitPreemptCheck` reads the flag off
  `x20`; both this fork and XenDroid document "Context register = x20"
  (`a64_emitter.h:128`). Not a register-convention bug.
* **The flag is raised** - watchdog shows `preempt_requested=1`, `irql=0`.

So: checks exist, the register is right, the flag is set - and the fiber still
never yields.

### ⭐ HYPOTHESIS 1 ELIMINATED, HYPOTHESIS 2 CONFIRMED

Instrumented `PreemptCurrentFiber` (guest_scheduler.cc:78):

* It **IS entered** — `PREEMPTHANDLER entry #0..#5`.
* It **does NOT decline** — no "holds global critical region" message ever.
* But it is entered **only ~6 times total**, then never again, while the
  watchdog keeps showing `MAIN_THREAD ... lr=82589EC4 preempt_requested=1`.

So the mechanism works end to end — safepoint fires, handler runs, no defer —
but **the hot loop the guest is actually spinning in carries no check**. Early
functions got checks (`checks_emitted` 4→9 over 5 functions); the loop at
`lr=82589EC4` did not.

### ⭐⭐ DECISIVE: the stuck fiber is NOT in guest code

Added `preempt_check_every_block` (cvar, default false) which injects a
safepoint into **every HIR block**, not just detected loop heads — the blunt
test of "the hot loop has no check".

**Result: no change whatsoever.** Same watchdog, same address:
`MAIN_THREAD lr=82589EC4 preempt_requested=1`, tid=6 ready on the same CPU,
CPUs 1–5 idle.

If the fiber were executing guest code, a check in every block would fire
within microseconds. It does not. Therefore:

**tid=7 MAIN_THREAD is blocked inside a HOST function called from guest code.**
`lr=82589EC4` is stale — it is the last guest call site before entering the
host, not where the thread is. Safepoints exist only in JIT'd guest code and can
never preempt this.

### ▶️ NEXT ACTION — find the unrouted host wait

Some kernel export called from guest `0x82589EC4` blocks on a host primitive
instead of yielding. `XObject::Wait`/`WaitMultiple` and `XThread::Delay` are
routed; this is something else.

1. **Disassemble/identify guest `0x82589EC4`** and see which import it calls —
   that names the export directly. (`xe::cpu` has a disassembler; or grep the
   log's import table dump for the nearest address.)
2. Or **probe the host blocking primitives**: log on entry/exit of
   `xe::threading::Wait`, `WaitAny`, `WaitAll`, `Sleep`, `AlertableSleep` and
   any `Fence::Wait`, run with the scheduler on, and see which call the main
   fiber enters and never leaves.
3. Strong suspects: `XIoCompletion`/`XFile` I/O waits, `XamContent*` /
   `XamUserRead*` startup calls, and any `Fence` wait inside the audio or
   content setup.

Then route that path through `CooperativeWait` the same way
`XObject::Wait` was.

Also still open: **all threads land on CPU 0** while CPUs 1–5 idle
(`DispatchCpuOf`).

## How to test (exact, these cost hours to learn)

```bash
# build + install
cd /home/roman/Android/Xenia-AE
JAVA_HOME=/opt/android-studio/jbr ./gradlew :app:assembleDebug
adb -s 3a478943 install -r app/build/outputs/apk/debug/app-debug.apk

# launch Halo 3 (library tile = TWO taps; first only selects)
adb -s 3a478943 shell "monkey -p org.xeniaae.aex -c android.intent.category.LAUNCHER 1"
sleep 9; adb -s 3a478943 shell input tap 489 530; sleep 4
adb -s 3a478943 shell input tap 489 530; sleep 55
adb -s 3a478943 exec-out screencap -p > /tmp/shot.png

# logs / config (NEVER adb-push app files; use su + preserve ownership)
L=/storage/emulated/0/Android/data/org.xeniaae.aex/files/xeniaae/xe.log
P=/storage/emulated/0/Android/data/org.xeniaae.aex/files/xeniaae/xenia-canary.config.toml
```

* **In-game** (for the ball) needs a HELD gamepad press:
  `adb shell input keyevent --longpress 96`, `sleep 3`, ×5. Taps and instant
  keyevents do NOT work. **Halo 3's level intro is a pre-rendered VIDEO** — its
  frames contain no engine models and cannot judge the ball.
* Delete `cache/pipelines_<TITLEID>.bin` before any GPU test.
* **"BUILD SUCCESSFUL" ≠ code in the binary** — verify with
  `strings libe.so | grep <PROBE>`, and confirm the *installed* APK, not the
  build output. A stale APK caused a wrong conclusion today.

## Probes available (all default OFF, same names in XDtester → logs diff 1:1)

`debug.canary.` + `pm4total` `pm4draw` `drawentry` `bonedistinct` `vsconst`
`regtrace` `ndcy_draw` `resolve_row_marker` `halo3_vista_probe` `vista_rt_base`
`shared_memory_host_visible` `resolve_aligned_storage`
`memexport_bypass_validation`

**Success criterion:** AEX draws/frame and packets/frame approach XenDroid's
(~611–641), and run-to-run variance collapses.

## Landed on canary-aex

Transplants: memexport eA validation; host-visible shared memory +
`ReadbackResolveMode::kUma` (**user-confirmed image improvement**); memexport
page tracking + fence/coherency awaits; MemoryPollPark/DelayCountdownCollapse
passes (**default OFF — they regress without preemption**); the full fiber
scheduler (steps 1–4).

**Real bug fixes** (cherry-pick candidates for `canary-ae` / `main`):
1. `vulkan_sparse_shared_memory` forced off with a custom driver — **fixes a
   black screen hitting every NEW install with a custom Turnip driver**.
2. `gamma_render_target_as_srgb_` was never assigned on `kHostRenderTargets` —
   the cvar was wired to nothing.
3. `PPCContext` scheduler fields **appended, not inserted** — the JIT addresses
   that struct by offset off x20; inserting shifted every later field.
4. `WakeCooperativeWaiters` / `RecordCooperativeSignal` crashes:
   `XThread::GetCurrentThread()` **asserts** on non-guest threads (guard with
   `IsInThread()`), and `XObject::handle()` is `handles_[0]` with **no bounds
   check**.

## Hard-won rules — violating these has already cost hours

* Guard cooperative paths on **`GuestScheduler::enabled()`**, never on the
  pointer — `KernelState` constructs the scheduler unconditionally.
* **Sample the signal epoch BEFORE polling** in `CooperativeWait`, or a signal
  landing after a failed poll is lost and the fiber never wakes.
* Landing gated code incrementally is **safe**; *enabling* a partially-ported
  coupled unit **wedges silently**. Do not conflate these (I did, twice).
* Watchdog `last_safepoint` is **always 0** here — `log_safepoint_pc` was not
  ported. **Not evidence.**
* When two builds of the same code differ, **diff their configs before
  suspecting code** — a fresh config differs from Canary AE's 2020-era one on 12
  cvars, and one of them was the whole "AEX regression".

## Retracted — do not resurrect

"46-vs-64 / surface_pitch" as the vista cause · "the composite doesn't read
guest RAM" · "the cause is driver-side" · "gamma=true improves the vista" (that
was the pipeline-cache deletion) · "AE's Vulkan backend has no readback path" ·
"the injection pass never runs" (stale APK) · "we win on face stability".

## Reference trees

`/home/roman/xeniatest/xendroid-git` (XenDroid + XDtester probes, branch
`xdtester`, package `xendroid.compose.xdtester.debug`) ·
`/home/roman/xeniatest/canary-git` (closest upstream base, best for
`diff | grep '^-'` to find AE deletions) · `/home/roman/xeniatest/oracle`
(newer upstream canary).

Deeper history: `docs/AEX_OVERHAUL.md`, `docs/HALO3_VISTA_46_VS_64.md` (s14–s35).
