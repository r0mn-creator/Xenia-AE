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

## 2026-08-02

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
  re-checked. **Re-test Halo 3 and Geometry Wars before release.**


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
