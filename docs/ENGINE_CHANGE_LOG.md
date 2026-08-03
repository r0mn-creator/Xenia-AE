# Engine change log — cross-game risk register

Every change to **shared engine code** (anything under `cpp/xenia-canary/src/`,
plus the native input driver) gets an entry here, with the one question that
actually matters recorded up front: **which other titles could this affect?**

## Why this file exists

On 2026-07-04 NFS Carbon completed a race. GPU work for Halo 3 landed
2026-07-10…13, and NFS then froze at its main menu for four weeks. Answering
"did the Halo 3 work break NFS?" should have been a `git bisect`. It was not,
because the relevant work sat inside **one squashed commit** (`53b299db`, "Add
Xenia AE front-end, Halo 3 fixes, and repo setup"), so behaviour had to be
bisected instead of history — roughly a day of device testing to eliminate
changes that a granular history would have ruled out in minutes.

**The answer, for the record: the Halo 3 GPU work was NOT the cause.** All eight
of those changes were disabled simultaneously and NFS froze identically. The real
cause was `headless=false` (see below) — a setting that predated all of it.

Two rules follow, and they are the point of this document:

1. **One logical change per commit.** Never squash engine changes together.
   A commit that touches shared code must be revertable on its own.
2. **Anything that alters shared behaviour ships behind a runtime toggle**
   (`debug.canary.*`, see `base/ae_fix_toggle.h`) so a regression can be
   bisected on-device without a 15-minute rebuild per hypothesis.

## How to use this when a game regresses

1. Find the last build where the title worked; note the date.
2. Read the entries below that landed after it, worst-risk first.
3. Flip the listed toggle **off** and re-test — no rebuild needed.
4. Record the result in that title's file under `docs/games/`, whether or not it
   was the cause. A ruled-out hypothesis is worth as much as a confirmed one.

Risk key: **HIGH** = changes behaviour for every title · **MED** = changes
behaviour on a common path · **LOW** = diagnostics only, inert when off.

---

## 2026-08-03

### ★ FPS counter + benchmark harness — **LOW** (measurement only)
- **Files:** `base/ae_fps.h` (new, self-contained), one call in
  `vulkan_command_processor.cc` `IssueSwap`, JNI `current_fps` in
  `emulator.cpp` / `Emulator.java`, `fps_counter` view in
  `activity_emulator.xml` / `EmulatorActivity.java`,
  `scripts/fps_bench.sh` (new).
- **Toggle:** `debug.canary.fps=1` (its own property; off by default; drives
  both the on-screen counter and the log lines).
- **Why this had to exist before any optimisation work:** measured scene
  variation on NFS Carbon is **8.9–13.9 FPS, a ±25% spread**. A single reading
  cannot distinguish a real gain from noise, and doing exactly that already
  produced one false *"10% win"* in this project (one 12.9 FPS sample; the next
  three were 11.9 / 8.9 / 10.9).
- **What the harness does:** records a distribution and compares two runs with a
  **Mann-Whitney U** test (non-parametric — FPS is not normally distributed).
  Prints an explicit *NOT significant* verdict rather than a percentage, so a
  noise result cannot be read as a win.
- **Calibrated by simulation against the real spread:** a 60 s run detects a
  **≥10% change every time**; a 5% change **less than half the time**. So a 60 s
  run can prove a 10% win but cannot disprove a 5% one — record 180 s for
  small effects.
- **Why not count `VdSwap` in the log:** that needs `log_kernel_calls` +
  `log_level=3`, which floods the log and slows down the thing being measured.
- **Why the counter is not part of `status_overlay`:** that overlay tails
  `xe.log` and reads `/proc` once a second. An FPS readout must not do file I/O,
  or it perturbs the number it displays.
- **Position:** top **centre** — the Odin's own system counter occupies the
  upper left.

### Halo 3 vista probe now gated — **LOW**
- **File:** `vulkan_command_processor.cc`
- **Toggle:** `debug.canary.halo3_vista_probe` (off by default)
- **What:** `TestrigCaptureVistaRtPostTransfer()` was called **unconditionally
  on every draw**. It early-outs cheaply, so this is **not** an FPS fix — but it
  survived the diagnostics-off sweep and broke the rule that tests stay off
  unless in use. Gated rather than deleted because the vista investigation is
  paused, not finished.

### Guest-thread affinity: big.LITTLE hazard documented — **LOW** (comment only)
- **File:** `kernel/xthread.cc`
- **What:** `SetActiveCpu` maps a **guest** hardware-thread index straight onto a
  **host** CPU index. Harmless on desktop x86 (all cores equivalent, and the
  360's 6 threads are 3 identical PPE cores × 2-way SMT). **Not harmless on
  Android:** on a Snapdragon 8 Gen 2, cpu0–2 are Cortex-A510 @ 2016 MHz while
  cpu7 is a Cortex-X3 @ 3187 MHz, so `XSetThreadProcessor(0)` would pin a thread
  the game considered important to the *slowest* core on the chip.
- **Not a live bug:** `ignore_thread_affinities` defaults to `true`, and NFS
  Carbon never calls it (0 hits in the log). Comment added so nobody enables it
  on Android without first translating through the host's real core topology.

---

## 2026-08-02

### Performance experiments — measured, NO gain — **LOW**
- **Files:** `AndroidManifest.xml` (`appCategory="game"`),
  `EmulatorActivity` (sustained performance mode),
  `PerformanceHints.java` (ADPF hint session)
- **Result: none of these moved the frame rate.** Recorded so they are not
  re-tried:

| lever | result |
|---|---|
| `appCategory="game"` + `setSustainedPerformanceMode` | no gain |
| ADPF hint session (5 tids, 33ms target) | no gain |
| Turnip driver on NFS Carbon | **worse**: 11.8 -> 7.9 FPS |
| all diagnostics/probes off | no gain |
| core affinity pinning | **not needed** - already optimal |

- **Why they cannot help, measured:** during gameplay `Main XThread` ~107%,
  `MainThread` ~100%, `GPU Commands` ~95-100% of a core each, with **0% of
  samples on an efficiency core** and cpu7 already at its full 3187/3187 MHz.
  There is no idle clock to unlock and no misplacement to correct.
- **Turnip specifically:** `GPU Commands` CPU *dropped* 97% -> 56%, but FPS fell
  to 7.9 - the thread was **waiting**, not being efficient. Turnip costs less
  CPU per draw but executes slower on this GPU. Halo 3 still needs Turnip
  (stock miscompiles a shader), so per-game driver selection is the right model.
- **⚠️ Measure across several samples.** A single reading of 12.9 looked like a
  10% win; three more gave 11.9 / 8.9 / 10.9. Scene variation on this title
  spans 8.9-12.9, which is wider than any effect measured so far.
- **What is left:** the bottleneck is the emulator's own code - the guest threads
  stayed pinned at ~105% through six configurations. Next step is profiling
  (`simpleperf`) on `Main XThread`, not more hints.

### ★ Diagnostics now default OFF (they defaulted ON) — **HIGH**
- **Files:** `base/testrig_debug_server.h`, `gpu/vulkan/vulkan_shared_memory.cc`,
  `gpu/vulkan/vulkan_command_processor.cc`, `gpu/command_processor.cc`,
  `java/org/xeniaae/EmulatorActivity.java`
- **Toggles:** `testrig.master` + per-subsystem, `probe_upload`, `probe_gbuf`,
  `probe_cp`, `logging`, `overlay` — all default **off**, all switchable on
- **What was wrong:** the testrig harness **defaulted to ON**. Its own comment
  said so: *"the default (nothing set) is fully enabled"*. That left **18
  hot-path GPU probes and 5 TCP debug servers live in every normal play
  session** — and since `debug.*` properties do not survive a reboot, turning
  them off was undone by the next restart.
- **Also gated (the WORK, not just the log line — a disabled probe must cost
  nothing):**
  - `UPLOAD_UNIFORM` scanned up to 16 dwords on **every** shared-memory upload,
    on the thread measured at 98% of a core during gameplay
  - `GBUF` scanned 4 x 256KB of guest RAM at swap time
  - `REENTER_DIAG_CP` logged inside the ring-buffer wait loop
  - `flush_log` (every batch to disk) and `log_to_stdout` (every line to logcat)
  - the status overlay (`/proc/self/stat` + `/proc/stat` + tailing `xe.log`, 1Hz)
- **Bug fixed on the way:** `PropertyEnabled` could only ever *disable* a
  default-on feature - an explicit `1` did **not** turn anything on. With the
  default flipped it now honours `1`, so every probe stays usable for testing.
- **Cross-game risk:** none to correctness - diagnostics only. Affects
  **performance in every title**, which is the point.
- **Principle:** Canary AE is a test bed, so nothing is deleted; but the default
  must be silent. A frame-rate measurement has to measure emulation, not
  instrumentation. With all tests off, Canary AE should behave like Xenia AE.

### Exiting a game returns to the library instead of closing the app — **LOW**
- **File:** `java/org/xeniaae/EmulatorActivity.java`
- **What:** `Exit Game` now starts `MainActivity` before the emulator process
  dies.
- **Why:** `EmulatorActivity` runs in its own `:emu` process and `onDestroy()`
  calls `System.exit(0)`. That kill is deliberate - the native core cannot be
  re-initialised in place, so a second game launched into the same process would
  fail - but nothing brought the library back first, so exiting killed the only
  visible process and dropped the user on the home screen.
- **Cross-game risk:** none - front-end navigation only, no engine behaviour.
- **Also covers:** the main process being reclaimed by Android while the
  emulator held ~1.4 GB. `NEW_TASK | CLEAR_TOP | SINGLE_TOP` relaunches it.
- **Verified:** library -> game -> Exit Game lands back on `MainActivity`, with
  the `:emu` process gone and the main process alive.

### ★ `xma_decoder` default changed `"old"` -> `"new"` — **HIGH**
- **File:** `apu/xma_decoder.cc`
- **Toggle:** `--xma_decoder=old` via `debug.canary.extra_args` restores it
  (also `master`, `fake`)
- **What:** Xenia ships four XMA implementations. Ours defaulted to `"old"`.
- **Why:** `"old"` conflates two distinct ring-buffer states - *wrote nothing*
  and *completely full* - because it tests `write_offset() == read_offset()`,
  true for both. NFS Carbon deadlocks there: ~10 s into a race the guest's own
  audio thread (`RWAudioCore Dac`) pins a full core polling one context whose
  state never changes again (`in0_valid=0 in1_valid=0`, `out_read_off=12`,
  `out_write_off=16`, **zero errors**). Audio goes silent, and because the level
  load waits on that same audio thread, the post-race load screen never
  completes. **One cause, both symptoms.**
- **Measured:** XMA poll lines **24821 -> 325**; the audio thread drops out of
  the top-5 CPU consumers entirely; audio survives a full race; the load screen
  completes.
- **Cross-game risk:** affects **every title that uses XMA audio** - i.e. nearly
  all of them. `"new"` is the upstream-intended implementation and is what
  XenDroid ships, but any title that happened to work under `"old"` should be
  re-checked.
- **✅ Halo 3 re-tested 2026-08-02 — NO REGRESSION.** Boots, reaches gameplay,
  zero errors, `XMA Decoder` actively decoding (85 ticks/6s), `MAIN_THREAD` not
  pegged, rendering unchanged. Its pre-existing character collapse is also
  unchanged - the change moved nothing in either direction, which is exactly what
  a regression test wants. See `docs/games/4D5307E6 - Halo 3.md`.
- **Still to re-test:** Geometry Wars Evolved, Experience Disc.


### ★ `headless=true` is now a default launch arg — **HIGH**
- **File:** `java/org/xeniaae/EmulatorActivity.java`
- **Toggle:** `debug.canary.headless=0` restores the old behaviour
- **What:** every game launch now passes `--headless=true`.
- **Why:** Xenia's non-headless path tries to display its own ImGui dialogs
  (sign-in, storage-device prompts). This front-end has no ImGui overlay, so such
  a dialog can be neither shown nor dismissed — the guest requests one and waits
  forever. This is what froze **NFS Carbon** at its main menu: pressing A on
  CAREER triggered an Xbox Live attempt whose prompt never completed.
- **Cross-game risk:** affects **every title**. Any game that legitimately
  expects a user answer from a system dialog now silently receives the default
  instead. That is almost certainly what we want on Android (there is no way to
  answer), but a title that branches on the answer will take the default path.
  **Both known-working Android forks (aX360e, XenDroid) already ship
  `headless = true`.**
- **Verified:** NFS Carbon reaches career loading and completes a race.

### Analog triggers, end to end — **MED**
- **Files:** `cpp/xe_android_input_driver.cpp`,
  `java/org/xeniaae/EmulatorActivity.java`, `java/org/xeniaae/KeyMapConfig.java`
- **Toggle:** none (bug fix, no behavioural ambiguity)
- **What:** three separate defects:
  1. `onGenericMotion` never read `AXIS_LTRIGGER`/`AXIS_RTRIGGER` (or
     `BRAKE`/`GAS`), so on any pad reporting triggers as **axes** — most of them,
     Xbox included — the triggers did nothing regardless of mapping.
  2. The native driver clamped triggers to `0xFF`, i.e. digital on/off. It now
     passes the real 0–255 value; `-1` (`KEY_VALUE_UNUSED`) still means "fully
     pressed" so the on-screen pad and digital `BUTTON_L2/R2` keep working.
  3. Default key map bound thumbstick clicks to `104`/`105` — which are
     `BUTTON_L2`/`BUTTON_R2`, the **triggers** — and left the triggers at `0`.
     Binding a trigger by hand then collided with the thumb press, and the
     runtime map (keyed by keycode) silently kept only one.
- **Cross-game risk:** affects **every title that uses triggers**. Racing games
  gain analog throttle/brake. A title previously relying on the accidental
  digital behaviour would now see partial values — no such title is known.
- **Migration:** `ControllerAutoMap.migrateLegacyKeyMap()` repairs maps saved by
  older builds, touching only the four affected entries and only when they still
  hold the broken values.

### Controller auto-detect / auto-map — **LOW**
- **Files:** `java/org/xeniaae/ControllerAutoMap.java` (new),
  `EmulatorActivity.java`, `SettingsCategoryActivity.java`
- **What:** pads are identified (vendor ID first, name as fallback) and mapped on
  connect, with an in-game toast and a Settings → Input listing.
- **Cross-game risk:** none to emulation — it only writes the same key-map prefs
  the user could set by hand.
- **⚠️ Untested:** the Nintendo (A/B, X/Y swap) and PlayStation profiles have
  **not** been tried on real hardware. Only an Xbox-layout pad was available.

### Live diagnostic toggles — **LOW**
- **Files:** `base/ae_fix_toggle.h`, `base/logging.cc`,
  `kernel/util/shim_utils.h`, `kernel/kernel_flags.*`
- **What:** `XE_AE_DIAG_ENABLED` re-samples its property every 500 ms so probes
  can be switched on **while a game runs**; `debug.canary.log_level_live` does
  the same for log level. Behaviour toggles stay cached deliberately — a fix must
  not change mid-session or the run means nothing.
- **Why:** the state worth measuring is usually one you cannot easily get back
  to. Cached toggles meant probing the NFS load stall required driving an entire
  race again.
- **Cross-game risk:** none when off (one clock read per call on probe paths).

---

## 2026-08-01

### Runtime toggles for the Halo-3-era GPU changes — **LOW** (bisect only)
- **File:** `base/ae_fix_toggle.h` (new) and the GPU/kernel sites it guards
- **Toggles:** `fix_rsq`, `fix_sincos`, `fix_wclip`, `reginit`, `gpu_3d_to_2d`,
  `fix_sampler`, `fix_rt_1010102`, `fix_stencil_discard`, `fix_swap_renderpass`,
  `profile_local_only`, `reenter_longjmp`, `fifo_semaphore`, `ax360e_waits`,
  `xnaddr_online`
- **What:** each pre-existing AE-only change can now be disabled at runtime. All
  **default ON**, so a shipped build is byte-for-byte the tested behaviour.
- **Cross-game risk:** none by default. This exists purely so the next "did X
  break Y" is a `setprop`, not a rebuild.
- **Result:** all eight Halo-3-era GPU changes disabled together → NFS froze
  identically, which is what cleared them.

### `log_all_kernel_calls` — **LOW**
- **Files:** `kernel/util/shim_utils.h`, `kernel/kernel_flags.*`
- **Why:** the trampoline only logs exports tagged `kLog`, and **no `xam_net`
  export carries it**. That produced a false "the game never touches the
  network", which sent the NFS investigation down the wrong path for hours. The
  network hypothesis was in fact **correct**.
- **Rule this encodes:** before concluding a subsystem is unused, verify the
  instrument can see it.

### FIFO semaphore (ported from XenDroid) — **MED**, default OFF
- **File:** `base/threading_posix.cc`
- **Toggle:** `debug.canary.fifo_semaphore=1` (default OFF)
- **What:** strict-FIFO ticketed guest semaphores, so a thread arriving after a
  release cannot steal a token from one already parked (Windows never does).
- **Cross-game risk:** would affect every title using guest semaphores, hence
  default OFF. **Did not fix NFS**; kept because it is a real semantic
  difference from Windows and may matter for other titles.

---

## Open items with known cross-game reach

| Item | Risk | Note |
|---|---|---|
| XMA decode stall | **HIGH** | Guest spins on `XMAGetOutputBufferReadOffset` / `WriteOffset` / `XMAIsInputBuffer0Valid` at ~1300/sec while the decoder stays parked. Causes NFS Carbon's audio dropout **and** its post-race load stall — one bug, two symptoms. The same EA engine family has a known "audio loop stuck" defect (community patch exists for NFS Most Wanted). **Any title using XMA streaming can hit this.** Fix carefully and re-test NFS + Halo 3 + Geometry Wars. |
| Backgrounding kills `:emu` | MED | Activity destruction ends the process (`VulkanPresenter: ... surface has become outdated`); pause/resume is still stubbed. |
| Halo 3 model collapse | — | **Not our fork**: XenDroid fails it too, and the desktop RADV oracle renders it correctly ⇒ Adreno-side. |
