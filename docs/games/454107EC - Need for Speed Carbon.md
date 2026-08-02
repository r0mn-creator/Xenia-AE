# 454107EC — Need for Speed: Carbon

**Role in this project: the canary.** It was the first title to render gameplay
at all, it is light enough to iterate on quickly, and it exercises the shared GPU
path hard. Run it after every GPU change. When it breaks, a shared subsystem
broke — that is exactly what it is for.

- ISO: `/sdcard/Download/360/Need for Speed - Carbon.iso`
  (`content://media/external/downloads/1000069113` on the Odin 2)
- Pipeline cache: `<storage>/cache/pipelines_454107EC.bin`

## Status (2026-08-02) — ★ MAIN-MENU FREEZE FIXED

**Root cause: `headless` was `false`.** Fixed by making `--headless=true` a
default launch arg on Android (`EmulatorActivity`). NFS Carbon now leaves the
main menu and loads career mode.

## Timeline

### 2026-07-03 — first render, on the moto (Adreno 610)
The original breakthrough. Two JIT bugs fixed plus a critical-section deadlock
diagnosed. This is the device that proved the emulator worked at all.

### 2026-07-04 — ★ LAST KNOWN GOOD: a race completed on the Odin 2
Fully playable through a whole race. Fixed to get here: an XThread
use-after-free, an XMA `assert_always`, a GPU stall loop, and a `WaitMultiple`
optimisation. Known issue at the time: audio cut out mid-race (XMA streaming
starvation) — a defect, but not a blocker.

### 2026-07-05 — "stuck loading next level" stall investigated
Ruled out: the GPU command processor (heartbeat logging showed it executing
normally, and the loading spinner genuinely animated), swap thrashing, and
disc-I/O. Narrowed to a guest-side condition that never becomes true. Confirmed
unrelated to that night's tessellation work.
**Note the shape of that stall — it is the same shape as the current freeze.**

### 2026-07-10 … 07-13 — the Halo 3 GPU work lands
Eight commits touching shared GPU code. NFS was not re-tested against each one.
See "Suspect list" below.

### 2026-07-31 — freeze characterised properly
Full measurement in [`../NFS_CARBON_FREEZE.md`](../NFS_CARBON_FREEZE.md). It now
gets *further* than the older notes claimed: attract sequence → garage → **main
menu** (CAREER / MY CARS / CHALLENGE), then stops.

**It is not a hang and not a GPU fault — it is a guest-side livelock:**

| signal | value |
|---|---|
| Game image | static: 4 frames / 15 s, **0 changed pixels** |
| Command processor | still executing, `advancing=true` |
| `VdSwap` rate | ~18/s, **different buffer each call** |
| Errors / warnings | 0 / 0 |
| New guest kernel calls | **zero** over 30 s |
| Guest main thread | spinning in JIT'd guest code |
| Guest threads | 19, all running, all workers idle on benign waits |

The render loop is turning; the game's own update logic is not.

⚠️ **Measure it correctly:** raw screenshot diffs are useless because the debug
overlay prints a live counter. Crop the bottom ~12% and compare only the game
area, four frames about 5 s apart.

### Hypotheses tested and REFUTED
Each of these was believed, tested, and killed. Do not re-run them.

| # | Hypothesis | How it died |
|---|---|---|
| 1 | The existing **save file** is no longer compatible | User's own control tests: deleting the save and creating a fresh alias ("ELI") froze identically; **no alias at all** also froze at the exact same place |
| 2 | The game is polling **Xbox Live** and never gets an answer | Kernel-call trace at the freeze contains **only** `VdSwap` (273 calls / 15 s) — **zero** `NetDll`/`Xam`/`Xnp`/`XNet`. Fitted the evidence perfectly and was still wrong |
| 3 | AE's four **precision fixes** (RSQ / SIN·COS / W-clip / register init) | All four disabled at runtime together — still froze |
| 4 | **3D-as-2D texture views** (`gpu_3d_to_2d`) | Disabled — still froze (2026-08-01) |
| 5 | **The whole Halo-3-era GPU change set** | All eight disabled *simultaneously* — froze in the identical spot (2026-08-01). This kills the "Halo 3 work broke NFS" premise for everything after the squash commit |
| 6 | **Local-only profile** (`profile_local_only=0`, aX360e's `1 \| 2`) | Restored aX360e's online-capable profile — still froze (2026-08-01) |

### ⚠️ RETRACTION — hypothesis 2 was never actually tested (2026-08-01)
The "zero NetDll calls" evidence was **an artifact of the instrument.** The
kernel-call trampoline in `util/shim_utils.h` only prints a call when its export
carries the `kLog` tag:

```cpp
if (TAGS & xe::cpu::ExportTag::kLog && ...) PrintKernelCall(...);
```

**Not one of the 51 `xam_net` exports carries `kLog`.** `NetDll` calls could not
have appeared in that log under any setting. "The game polls no network function
at all" measured the logger, not the game.

Fixed by adding `log_all_kernel_calls`, which ignores the tag —
`debug.canary.log_all_kernel_calls=1`. **Use it before concluding any subsystem
is unused.** Absence of evidence was not evidence of absence here.

## ★ The strongest measurement so far (2026-08-01)

With tracing on at the menu, the *entire* guest kernel activity is:

| calls | function | thread |
|---|---|---|
| 645 | `VdSwap` | `F800000C` MainThread — render loop, healthy |
| **292** | **`RtlNtStatusToDosError(00000103)`** | `F8000164`, `F8000168` |
| 5 | `RtlRaiseException(...406D1388)` | thread-naming, benign |

`0x103` is **`STATUS_PENDING`**. Two guest threads do *nothing else* — they never
make another kernel call — they just read a guest-memory status, convert it, and
loop. That is the textbook **overlapped/async poll**: something was set to
`IO_PENDING` and never completed.

Supporting measurement: the **`Kernel Dispatch` host thread** (which drains
`dispatch_queue_` and is what actually completes deferred overlapped operations)
is alive but has burned **zero CPU time** with only **8 voluntary context
switches** all session. It is parked in `dispatch_cond_.wait()`. So whatever the
game is polling was **never queued for completion at all**.

⚠️ Also spotted while reading that loop: `dispatch_cond_.wait(global_lock)` has
no predicate, so a spurious wakeup falls through to `dispatch_queue_.front()` on
an empty list — undefined behaviour. Not the freeze (that would crash), but a
real latent bug worth fixing.

## What aX360e tells us (2026-08-01)

aX360e is the base this project was built from, and per the user it **ran NFS
Carbon past the menu, stalling later at a network stage** — strictly further than
we get. Its source is at `/home/roman/Android/ax360e`. Diffing the non-GPU
engine against ours, the entire divergence is ~7 files:

| file | differs | note |
|---|---|---|
| `xam/xam_net.cc` | **0 lines — byte-identical** | rules out the network layer itself |
| `xam/xam_user.cc`, `xam/xam_msg.cc` | 0 lines | identical |
| `xam/user_profile.h` | 1 line | `type()`: aX360e `1 \| 2`, ours `1` |
| `xam/xam_content.cc` | 1 line | `license_mask` default: aX360e `0`, ours `1` |
| `kernel_state.cc/.h` | probe + `InitXmpVolumePatch` | ours |
| `xboxkrnl/xboxkrnl_threading.cc` | STUCK_WAIT probe | ours |
| `xthread.cc/.h` | ours | |
| `base/threading_posix.cc` | the lost-wakeup fix `cd464ff6` | ours |
| `apu/xma_decoder.cc`, `base/bit_stream.cc` | XMA fixes | ours |

`user_profile.h::type()` looked decisive: `XamUserGetXUID` computes
`type() & type_mask`, so a title requesting the **online** XUID (`type_mask=2`)
gets `X_E_NO_SUCH_USER` + xuid 0 from us but success from aX360e. **Tested via
`debug.canary.profile_local_only=0` — still froze.** Kept as a toggle anyway; it
is a genuine behavioural divergence from the base that got further, and may
matter once the real blocker is cleared.

`license_mask` is a plain cvar (`--license_mask=0`), so it needs no rebuild to
test.

Hypothesis 2 is worth remembering as a method lesson: it explained every
measured signal, including the odd one (zero kernel logging, explained by a
silent stub), and it was still false. The trace is what settled it.

## Suspect list — the remaining Halo-3-era changes

The user's read is that *"NFS worked past the menu until we tried to fix Halo 3"*.
The engine commits between the last-good build and now:

| commit | date | change | toggle | tested |
|---|---|---|---|---|
| `d04910e2` | 07-10 | 3D-as-2D texture views | `debug.canary.gpu_3d_to_2d` | ✗ not the cause |
| `d04910e2` | 07-10 | sampler min/mip filter fix | `debug.canary.fix_sampler` | — |
| `d04910e2` | 07-10 | 10:10:10:2 render-target format fix | `debug.canary.fix_rt_1010102` | — |
| `d04910e2` | 07-10 | anisotropic override wiring | inert (default −1) | n/a |
| `6daf1479` | 07-10 | stencil-bit discard in transfer shaders | `debug.canary.fix_stencil_discard` | — |
| `74a4ebfe` | 07-10 | render-pass tracking through `IssueSwap` | `debug.canary.fix_swap_renderpass` | — |
| `c14047bc` | 07-12 | RSQ via sqrt+div | `debug.canary.fix_rsq` | ✗ not the cause |
| `a0b2f29e` | 07-13 | Cody-Waite SIN/COS | `debug.canary.fix_sincos` | ✗ not the cause |
| `54e6a4d6` | 07-13 | register zero-init | `debug.canary.reginit` | ✗ not the cause |
| `6a4b9932` | 07-13 | degenerate-W clip | `debug.canary.fix_wclip` | ✗ not the cause |

⚠️ **Caveat on the premise.** The last-good build (2026-07-04) predates the repo
history — `53b299db` (07-08) is a squashed "Add Xenia AE front-end, Halo 3 fixes,
and repo setup" commit. So *"Halo 3 work broke NFS"* is a plausible correlation,
**not** an established fact: something inside that squash could equally be
responsible, and it cannot be bisected commit-by-commit. That is precisely why
the toggles exist — they bisect *behaviour*, which is available, rather than
*history*, which is not.

⚠️ **Second caveat.** The freeze is a **guest-side** livelock with the GPU
healthy. It is not obvious how a render-format or sampler change causes the
guest's own update loop to stall, so if disabling all of them together changes
nothing, the cause is probably not in this table at all — look at CPU/kernel
changes, or at whatever guest-visible value the game is waiting on.

## Reproducing

```bash
adb -s <serial> shell "am start -n org.xeniaae.canary/org.xeniaae.EmulatorActivity \
  -e game_uri 'content://media/external/downloads/1000069113' \
  -e game_title 'Need for Speed - Carbon' -e game_title_id '454107EC'"
```
Quote the whole `am start` for the **device** shell — unquoted, the title's
spaces are split by the host shell and the intent is silently mangled
(`pkg=for`).

Reaches the menu in roughly 40 s warm, ~90 s with the pipeline cache cleared.

---

# 2026-08-01 session — automated repro + eliminations

## ★ The freeze is now reproducible with NO human at the device
`scratchpad/nfs_test.sh` boots the title, presses START/A to the main menu, then
presses A three times 3 s apart and reports whether **any pixel** of the game
area changed. Two things made this possible:

- **Input injection works** — `adb shell input gamepad keyevent --longpress <code>`
  reaches the guest. It needs SEVERAL presses; NFS does not self-advance from
  "Press START to begin" (START=108, A=96). A single press does nothing, which
  earlier looked like "input is broken".
- **Screencap works on the Odin 2** and is the only trustworthy progress signal.

Baseline result, measured: at the main menu, three A presses change **0 pixels**.

⚠️ **Do not use the log as a progress metric.** Three separate times this
session a "measurement" turned out to be an artifact of the logger:
1. `VdSwap` is `kHighFrequency` → invisible unless high-frequency logging is on;
   its count sitting at 1 looked like a hang while the intro cinematic played
   perfectly.
2. Kernel calls need the `kLog` tag → no `xam_net` export has it, which produced
   a **false** "the game never touches the network".
3. `PrintKernelCall` logs at DEBUG → invisible at the shipped `log_level=2`.
Pixels cannot lie; the log can.

## What the guest is actually doing at the freeze
| thread | behaviour |
|---|---|
| **`Main XThread (F8000008)`, `main=true`** | **47,858 × `KeDelayExecutionThread(interval=0)`** in ~8 s and *nothing else* |
| `MainThread (F800000C)` | menu render loop: `VdSwap`, `XNotifyGetNext`, `XNetGetTitleXnAddr` — healthy |
| all 17 other guest threads | alive, `running=true`, near-zero CPU |

`interval=0` is `Sleep(0)` — a **yield-spin**. So ~6000 calls/sec is the expected
rate for a spin-wait, *not* evidence of a broken timer (an earlier reading of
this as "the delay returns 60x too fast" was wrong: every one of the 47,858 calls
requests 0, and `XThread::Delay` correctly maps 0 to `MaybeYield()`).

⇒ **The game's MAIN thread is spin-waiting on a guest memory value that nothing
ever updates, while every worker sits idle.** Two host threads are pegged in R
state; the rest sleep.

## Eliminated this session (each measured, not argued)
| # | Hypothesis | Result |
|---|---|---|
| 7 | All 8 Halo-3-era GPU changes, disabled **simultaneously** | froze identically — kills "Halo 3 work broke NFS" for everything post-squash |
| 8 | `profile_local_only=0` (aX360e's `1 \| 2`) | froze |
| 9 | `xnaddr_online=1` (full ONLINE XnAddr instead of zeroed loopback) | froze |
| 10 | **Deferred-overlapped completion never running** | **DISPROVEN by direct probe**: exactly ONE deferred overlapped all session, and the Kernel Dispatch thread *ran* it (`ENQUEUE`→`RUN`). Its 0 CPU / 8 context switches is innocent idleness |
| 11 | `NtReadFile` async path unimplemented | **not reached** — the branch is `if (true \|\| file->is_synchronous())`, so the sync path always runs and the broken async block is dead code |
| 12 | Blocked guest kernel wait | no `STUCK_WAIT` hits. ⚠️ but note that probe only fires when a wait *returns* after >1 s, so a permanently blocked wait is invisible to it |
| 13 | XenDroid config deltas: `clear_memory_page_state=true`, `vulkan_sparse_shared_memory=true`, `license_mask=0`, `mount_cache=false` | all froze |
| 14 | `readback_resolve=uma` | **breaks rendering** on our backend (near-black screen) — actively harmful, not a fix |
| 15 | **FIFO semaphore** ported from XenDroid | froze (boots fine, no regression — kept behind `debug.canary.fifo_semaphore`) |

## ★ XenDroid — the best oracle we have
`xendroid.compose`, upstream `rfandango/XenDroid`, source cloned to
`/home/roman/Android/xendroid`. **Same device, same ISO, and it gets past the
menu.** Its log for the run is saved at `scratchpad/xendroid_nfs.log`.

The trade is informative and worth remembering:
- **XenDroid advances but glitches visually** — its log contains **7,738 critical
  lines**, mostly `PM4_DRAW_INDX(...): Failed in backend` (thousands of failed
  draws). That is the glitching the user observed.
- **We render cleanly but stall.** So our GPU backend is genuinely ahead; the
  blocker is CPU/kernel-side. Do not "fix" our renderer toward theirs.
- It also does **not** render Halo 3 models — so that collapse is not specific to
  our fork; it is Adreno-side, consistent with the desktop RADV oracle rendering
  Halo 3 correctly.

Its network stub is **byte-identical to ours** (same zeroed `abOnline`, same
`STATIC`-only status), which is what killed the network hypothesis.

Its `threading_posix.cc` differs from ours by ~1094 lines and contains real
synchronisation work — a ticket-based FIFO semaphore (ported, didn't fix it) and
an NT-style auto-reset **Event hand-off** which is **off by default** and enabled
per-title (for AC6), so it is not what carries NFS either.

## Where to resume
The main thread polls a value nothing writes. Next step is to find **what** it
polls: instrument the spin (the guest PC / the address being read around the
`KeDelayExecutionThread(0)` loop) rather than guessing at subsystems. Everything
cheap has now been eliminated, and the remaining difference from a build that
works is **code, not configuration**.

---

# ★ 2026-08-01 late — FIRST change that moves the freeze point

## `ignore_thread_priorities` — ours `true`, aX360e `false`
Found by diffing **aX360e's on-device config** against ours (aX360e's config is at
`/sdcard/Android/data/aenu.ax360e.free/files/ax360e/xenia-canary.config.toml`;
only 11 settings really differ).

With `--ignore_thread_priorities=false`, NFS Carbon **gets past the main menu**
for the first time since 2026-07-04 — A is accepted, the menu exits, and the game
reaches **"Loading ELRO. Please don't turn off your Xbox 360 console."**

It then freezes there (verified: 4 frames, 8 s apart, 0 pixels changed, and the
user confirmed live). So this is **NOT a fix** - but it is the first change of
~15 tested that alters the symptom at all, and the direction fits the measured
mechanism exactly:

> the guest MAIN thread yield-spins (`KeDelayExecutionThread(interval=0)`,
> ~6000/sec) while every worker sits idle. If guest thread priorities are
> ignored, a hot-spinning high-priority main thread can starve the lower-priority
> worker it is waiting on. Restoring priorities lets the worker run - so the game
> progresses - until it hits the next instance of the same starvation.

**Next step: keep `ignore_thread_priorities=false` as the new baseline** and
re-measure the "Loading ELRO" stall from scratch (which thread spins now, which
one is starved). Do not assume it is the same wait.

⚠️ Related but SEPARATE issue, per the user: NFS Carbon's audio cuts out during
gameplay. That matches the July 4 XMA streaming-starvation note and the community
patch "Skip audio loop stuck code" (Gliniak) which exists for NFS **Most Wanted**
(`454107D9`, bundled) - a different title id, so its addresses do not apply to
Carbon. Do not conflate the audio dropout with the menu freeze.

## ⚠️ Method failure to avoid repeating
A run was reported as FIXED this session on the strength of one screenshot. It
was actually **aX360e** running, not Canary AE - the user caught it. Two lessons:
1. **Always confirm which package produced a result** (`pidof <pkg>:emu`, and the
   log's mtime/size), not just the screen.
2. aX360e Free **gates game loading behind an ad**: `Emulator.load_library()` is
   called inside the ad callback chain in `MainActivity`, so launching
   `EmulatorActivity` via `am start` never loads the native library and the game
   never starts (guest thread count stays at 4). Automating aX360e requires
   building it from source with the ad path stubbed - `app/build.gradle` is
   currently renamed to `.bak` in that tree.
Also: one success is not a fix. Re-run before believing it.

---

# ★ The spin loop has been located in guest code (2026-08-01)

Probe `debug.canary.trace_spin` logs the guest LR whenever
`KeDelayExecutionThread` is called with `interval == 0`, sampled 1-in-1024.
Every sample, on every run:

```
thread='Main XThread (F8000008)'  lr=0x8294A8E0
r3=1  r4=0  r5=0x7018FC20  r6=0x7018FC40  r7=0
```

## The guest code at that site, disassembled
```asm
8294A8D0  mr     r5, r30                  ; interval ptr
8294A8D4  mr     r4, r29                  ; alertable
8294A8D8  li     r3, 1                    ; processor_mode
8294A8DC  bl     KeDelayExecutionThread   ; <- lr = 8294A8E0
8294A8E0  cmplwi cr6, r31, 0
8294A8E4  beq    cr6, 8294A8F0            ; r31 == 0 -> leave
8294A8E8  cmpwi  cr6, r3, 0x101           ; STATUS_ALERTED?
8294A8EC  beq    cr6, 8294A8D0            ; retry while alerted
8294A8F0  cmpwi  cr6, r3, 0xC0            ; STATUS_USER_APC
8294A8F4  li     r3, 0xC0
8294A8F8  beq    cr6, 8294A900
8294A8FC  li     r3, 0
```

⇒ This is the title's **`SleepEx` wrapper**: sleep, retry while `STATUS_ALERTED`,
return `STATUS_USER_APC` or `0`. Our `XThread::Delay` returns only `0` or `0xC0`,
never `0x101`, so **this inner loop is not the spin** - it exits correctly.

⇒ The ~6000 calls/sec are therefore **6000 separate invocations from an OUTER
polling loop**. That outer loop is the thing actually waiting, and it is what
still needs to be identified.

## Guest caller chain at the stall (stable across samples)
```
sp=0x7018FBD0
82C651C8 826D13F4 82BA09C0 821A35C0 82B5279C 82948610
82B8C764 82C651C8 824E98B4 826D24CC 82B5279C 82B52790 82796310
```
`82948610` was checked and is a function **prologue**
(`mflr r12; stw r12,-8(r1); stwu r1,-0xB0(r1)`), i.e. a stale frame, not the
loop. The others are unexamined.

## New reusable tool
`debug.canary.dump_guest_addr` dumps 0x80 bytes of guest code around any address
with **no rebuild**:
```bash
adb shell setprop debug.canary.dump_guest_addr 82B5279C
```
Combined with `debug.canary.trace_spin`, any candidate from the chain can be
disassembled on the spot.

## Resume here
Walk the caller candidates with `dump_guest_addr` looking for a loop that
**loads a memory location, compares, and branches back** around the call to the
sleep wrapper. That names the polled address; then find what should write it.

---

# ★★ ROOT CAUSE FOUND AND FIXED (2026-08-02): `headless=false`

## The bug
Xenia's non-headless path tries to display its **own ImGui dialogs** for sign-in
and storage-device prompts. This front-end has no ImGui overlay, so such a dialog
can never be shown *or* dismissed: the guest requests it, waits for a result, and
waits forever.

Pressing A on CAREER makes NFS Carbon attempt an Xbox Live connection. The
resulting prompt never completed, so the game's main thread sat in a `Sleep(0)`
poll loop at guest **`0x824E98AC`** waiting for the flag at **`0x82C651C4`** to
become non-zero - a flag nothing would ever set.

## The fix
`--headless=true`, now added unconditionally in `EmulatorActivity`
(`debug.canary.headless=0` restores the old behaviour for A/B testing).

With it, the request returns a default immediately, the title gets its answer -
it displays its own **"Cannot connect to Xbox Live!"** dialog - and after OK,
career loading proceeds. Verified twice, then a third time with **no extra args
at all** on the shipped default.

**Both known-working Android forks already shipped `headless = true`** (aX360e
AND XenDroid). AE had `false`. It was visible in both config diffs and was
overlooked twice before being tested.

## ⚠️ The user was right and I was wrong
The user proposed early on: *"After loading the alias I believe the game tries to
connect to Xbox live."* That was **correct**. It was recorded as REFUTED on the
strength of a kernel trace showing "zero NetDll calls" - but no `xam_net` export
carries the `kLog` tag, so those calls were **structurally unloggable**. The
refutation measured the logger, not the game.

**Lesson: before concluding a subsystem is unused, verify the instrument can see
it.** `debug.canary.log_all_kernel_calls` + `debug.canary.log_level`(3) now exist
for exactly this.

## How it was finally located
1. `debug.canary.trace_spin` - logged the guest LR at every
   `KeDelayExecutionThread(interval==0)`: always `lr=0x8294A8E0`.
2. Disassembled that site - it is the title's `SleepEx` wrapper, so the real
   waiter was its caller.
3. Guest stack walk -> caller candidates -> dumped each one's code in a single
   run -> found the outer loop at `0x824E98AC`:
   `lwz r11,0x51C4(r27); cmpwi r11,0; beq loop`.
4. Logged non-volatile registers -> `r27=0x82C60000`, flag at `0x82C651C4`,
   value permanently `0`.
5. That said the guest was waiting on something the HOST should complete, which
   pointed at the un-dismissable dialog.

Tools added: `trace_spin`, `dump_guest_addr` (dump guest code at any address
with no rebuild), `log_all_kernel_calls`, `log_level` / `log_level_live`.

## Still open for this title
- Audio cuts out during gameplay (XMA streaming starvation, known since
  2026-07-04) - **separate** from this freeze.
- Backgrounding still ends the emu process (`VulkanPresenter: ... surface has
  become outdated`); the pause/resume path is still stubbed.

---

# ★ 2026-08-02 — a RACE WAS COMPLETED, and the next two bugs are now characterised

With the headless fix the title reaches gameplay and **a full race was finished**
(user-confirmed). Two issues remain, and measurement shows they are **the same
bug**, not two:

* audio cuts out ~10 s into a race and never returns
* the post-race load screen animates forever and never completes

## Measured at the live stall
| signal | value |
|---|---|
| Screen | **ANIMATING** - GPU healthy, presenting (~1.2 fps) |
| `RWAudioCore Dac` (guest audio thread) | **~100% of a core** |
| `MainThread` + `Main XThread` | **~100% of a core each** |
| Host `XMA Decoder` thread | **idle**, parked on `work_event_` |
| AAudio | `STARTED`, **`xrun_count: 0`**, still consuming |

Guest kernel calls in a 5 s window at the stall:
```
6489  XMAGetOutputBufferWriteOffset     ~1300/sec
6489  XMAGetOutputBufferReadOffset      ~1300/sec
6488  XMAIsInputBuffer0Valid            ~1300/sec
   6  VdSwap
```

⇒ The guest is spinning on the XMA registers waiting for the decoder to show
progress. This is the **EA "audio loop stuck"** pattern - the same engine family
for which the community patch set ships *"Skip audio loop stuck code"* (Gliniak)
for NFS **Most Wanted** (`454107D9`, bundled; different title id, so its
addresses do not apply to Carbon).

## Why the decoder never runs (mechanism)
`XmaContextOld::Work()` is **one-shot**:
```cpp
if (!is_enabled() || !is_allocated()) return false;  // idle unless kicked
set_is_enabled(false);                                // disables itself
Decode(&data); ...
```
`Enable()` is called **only** from the context-kick register write
(`XmaDecoder::WriteRegister`, `Context0Kick..Context9Kick`), which does wake the
worker (`work_event_->SetBoostPriority()`, and `SetBoostPriority(){ Set(); }` -
verified, so the wakeup is NOT missing).

So the cycle is: guest kicks -> decoder decodes one chunk -> context disables
itself -> guest inspects the context data -> guest kicks again. At the stall the
guest **stops kicking** because the values it polls never change, and the decoder
stays parked because only a kick would enable it. Neither side moves.

⇒ The defect is in **decode progress**, not in the wakeup path: the last decode
did not advance `output_buffer_write_offset` / release input buffer 0 the way the
title expects.

## Next step
Instrument `XmaContextOld::Decode()` / the `XMA_CONTEXT_DATA` fields across the
last few kicks before the stall - specifically `input_buffer_0_valid`,
`current_buffer`, `input_buffer_read_offset`, `output_buffer_write_offset` - and
find which one stops advancing. That names the decode bug.

⚠️ Two false starts to avoid repeating:
1. "`work_event_` is never set" - **wrong**, the kick path sets it via
   `SetBoostPriority()`. Check the alias before concluding a wakeup is missing.
2. "`VdSwap` count is 0, the GPU has stopped presenting" - **wrong**, `VdSwap` is
   `kHighFrequency` and high-frequency logging was off. The screen was animating
   the whole time. Trust pixels over the log.
