# Halo 3 vista — the 46-vs-64 resolve lead

**Single-source reference for the upside-down Halo 3 vista.**
Read this BEFORE running any vista test. Most plausible ideas have already been
tested and eliminated with evidence; re-running them wastes a session.

Status 2026-08-12: **not fixed.** Narrowed from ~13,000 diverging lines to one
resolve, one address, one register.

---

## 1. THE LEAD, in one table

Halo 3 main menu. Every resolve destination matches between the two emulators
**except one**:

| | `w_div8` | `h_div8` | pixels | `len` | order |
|---|---|---|---|---|---|
| **Canary AE** | **46** | **46** | 368x368 | 770,048 | n=**25** (last) |
| **XenDroid** | **64** | **64** | 512x512 | 1,048,576 | n=**5** |

Address `0x04D20000`, `fmt=22`, `depth=1`.

- XenDroid's is self-consistent: 64*8 = 512, and 512*512*4 = 1,048,576 exactly.
- The **read side** binds that address as **512x512 tiled, fmt 22**
  (`debug.canary.texbind` - a direct texture-key read, no derived arithmetic).

**So the guest binds a 512x512 depth surface, XenDroid fills it, and we write
only 368x368 into it.** Xenos derives row addresses from surface dimensions, so
the composite samples a partly-unwritten, mis-addressed surface.

This is **internally inconsistent inside our own build** (write 368, read 512),
not merely different from theirs - which is why it outranks everything else.

## 2. Where the bug is NOT (proven identical in both trees)

Do **not** re-diff these. Confirmed byte-identical:

- `width_div_8` / `height_div_8` assignment:
  `(x1 - x0) >> kResolveAlignmentPixelsLog2`
- The **surface-pitch clamp**: `x0/x1 = std::min(..., surface_pitch_aligned)`
- The 8-pixel resolve-rectangle alignment
- `ResolveCoordinateInfo`'s `width_div_8`/`height_div_8` **bitfield declarations**

**Therefore 46 vs 64 comes from the INPUTS, not the maths.**

## 3. THE NEXT TEST (narrow, definite, not yet run)

In **both** builds, gated behind a toggle and filtered to
`copy_dest_base == 0x04D20000` only, log immediately before `width_div_8` is
computed:

```
x0, x1, y0, y1                      (final rect)
the PRE-clamp vertices
rb_surface_info.surface_pitch
surface_pitch_aligned
rb_copy_control
```

**Whichever input differs IS the bug**, because everything downstream is proven
identical.

**Prime suspect: `surface_pitch`.** 46*8 = 368 and 64*8 = 512, so a
`surface_pitch_aligned` of 368 on our side would produce the observed value
through the clamp alone.

---

## 4. ELIMINATED — do not retest

Each of these was tested on-device with evidence. Nineteen dead ends:

### Eliminated by direct measurement against XDtester
| hypothesis | how it died |
|---|---|
| **NDC scale/offset** | Both builds produce **identical** per-draw regimes: 3 unflipped shaders (`0A6D1DD7767FDF27`, `C049A8C9E556F129`, `C2543FD5CD52420B`), same values, same everything |
| **Vertex-stage Y** | The vista draws **DO** receive the flip (`ndc_scale_y = -1`); only 3 screen-space UI draws are unflipped, correctly |
| **Translated shader position math** | Instruction-for-instruction identical (`pos.xyz * ndc_scale + ndc_offset * w`); only member indices shift |
| **`host_vertex_shader_type` / the `0x12000000` decode** | Disproven - would appear in NDCYDRAW; does not. The bit decode was wrong |
| **Resolve rectangle degeneracy/inversion** | Guard ported; **0 occurrences** on Halo 3 |
| **Resolve tiling pitch (raw vs aligned)** | Ported and active; vista unchanged |

### Eliminated by on-device toggle
`fix_wclip` (our W==0 guard) · `fix_rsq` (our RSQ precision fix) ·
`vfetch_bounds_clamp` · `vulkan_user_clip_planes` (implemented fully; Halo 3
never sets `ucp_ena`, so it never engages) · `readback_resolve=full` ·
`clear_memory_page_state` · `ytest_fallback` / `ytest_invert`

### Eliminated by config bisect on XenDroid's own build
`vulkan_resolve_to_texture` (+`_promote`/`_serve`) - off, their models/vista
still correct · `vulkan_shared_memory_host_visible` - off, still correct ·
`VK_EXT_external_memory_host` two-buffer routing - **extension is not enabled on
ANY Adreno**, dead code here · memexport CPU-visibility - their
`memexport_enable = false` and it still renders correctly

### Eliminated earlier
driver/Turnip (XenDroid renders correctly on the SAME driver) · resolve row
addressing · rectangle-list geometry shader (byte-identical to upstream)

## 5. RETRACTED claims — do not resurrect

| claim | why it was wrong |
|---|---|
| "336x336 vs 512x512" | **Derived** size from a stale build; 336*336*4 = 451,584 did not match its own reported `len` of 770,048. Superseded by the raw-field 46 vs 64 |
| "The composite doesn't read guest RAM" | The marker probe bypassed the texture cache's memory watches; a `gpu_written` page is never re-uploaded, so the null result was structurally guaranteed |
| "The cause is driver-side (Adreno/Turnip)" | XenDroid renders correctly on the same device and driver |
| "ISB for db16cyc is a 21.5% regression" | Our version never coalesced; we benchmarked 8 pipeline flushes per loop against 8 free no-ops |
| "9.67 FPS baseline" | Contaminated by `readback_memexport` + `readback_resolve` left enabled. Turning both off: **~10 -> 15 FPS** |
| "We win on face stability" | Never observed - our characters are blobs, so there were no faces to judge. Face flicker is a **shared** bug |

---

## 6. TOOLING — how to reproduce any of this

### XDtester
Instrumented XenDroid built from source.
`/home/roman/xeniatest/xendroid-git` (branch `xdtester`),
package **`xendroid.compose.xdtester.debug`** - installs ALONGSIDE stock
`xendroid.compose`, which stays untouched as the known-good reference.

Build: `JAVA_HOME=/opt/android-studio/jbr ./gradlew :app:assembleDebug`

Debug module: `src/xenia/base/xdt_debug.h` - self-contained, toggleable,
**no platform guard** (see gotchas), `debug.canary.*` names matching Canary AE
so both logs diff line-for-line.

### Probes (identical names/formats in both builds)
| toggle | what it logs |
|---|---|
| `debug.canary.vista_dump` | `VISTA ENUM` - one line per distinct resolve destination, **raw** `w_div8`/`h_div8`/`len` |
| `debug.canary.ndcy_draw` | `NDCYDRAW` - per-draw NDC-Y regime + shader hash |
| `debug.canary.ndcy` | `NDCY` - viewport registers -> `ndc_scale[1]` |
| `debug.canary.texbind` | `TEXBIND` - **read side**: what each texture fetch constant actually binds |
| `debug.canary.resolve_rect_trace` | `RECTTRACE` - resolve rect vs scissor |

All default OFF. `setprop` + relaunch.

### Reproduce the headline result
```sh
# 1. delete the pipeline cache or NOTHING is re-translated / re-resolved
adb shell "su -c 'rm -f <files>/cache/pipelines_4D5307E6.bin'"
# 2. enable
adb shell setprop debug.canary.vista_dump 1
# 3. launch Halo 3, reach the MAIN MENU (no level load needed)
# 4. compare the two logs
adb shell "grep 'VISTA ENUM' <files>/xe.log"
```

---

## 7. GOTCHAS that cost real time

1. **`cache/pipelines_<TITLEID>.bin` short-circuits translation.** A 30 MB cache
   meant ZERO shader dumps and stale resolves. **Delete it before any GPU
   test.**
2. **"BUILD SUCCESSFUL" does not mean your code is in the binary.** Always
   `strings libe.so | grep <your probe>`. Gradle reported success while shipping
   an older `.so` more than once.
3. **A wrong platform guard silently deletes probes.** `xdt_debug.h` originally
   guarded on `XE_PLATFORM_ANDROID`, which XenDroid does not define -> the
   function compiled to `return false` -> the compiler stripped every probe body
   INCLUDING its log strings, while the build succeeded. Cost three cycles.
   XDtester's header now has no platform guard.
4. **Never append a duplicate key to a TOML config.** A second `vulkan_lib_path`
   made the whole file fail to parse and the emulator silently fell back to
   defaults. **Edit the existing key.**
5. **App configs are mode 660 owned by the app uid.** A plain `adb shell grep`
   returns *permission denied*, which reads like "key absent". Use `su`.
6. **Sanity-check derived numbers against a raw field.** The retracted 336x336
   was caught by a 4-bytes-per-pixel multiplication that took seconds.
7. **The vista renders at the MAIN MENU.** Never load a level to test it -
   ~90 s per iteration instead of ~4 min.
8. Halo 3 title ID: **`4D5307E6`**. Turnip R8 driver lives at
   `/data/data/<pkg>/driver/vulkan.ad07xx.so`, set via `vulkan_lib_path`.

---

## 8. Reference points

- XenDroid renders Halo 3 **models AND vista correctly** on the same Odin 2 +
  Turnip R8, at **17.9-20.6 FPS** vs our 15. Both bugs are ours and fixable.
- Their weaknesses to NOT inherit: **NFS Carbon geometry pop-in/transparency**,
  flatter colour. Face flicker is shared.
- Revert point: tag **`pre-xendroid-port`**. Work branch: `xendroid-port`.
- Related: `HALO3_VISTA_UPSIDE_DOWN.md` (full narrative),
  `HALO3_BALL_XENDROID_FIX.md` (the models bug),
  `XENDROID_FULL_COMPARISON.md` (engine-wide comparison).

---

## 9. MEASURED 2026-08-12: the limiter is the SCISSOR, and the code is identical

Logged the inputs at the point `width_div_8` is computed
(`debug.canary.resolve_inputs`, filtered to `copy_dest_base == 0x04D20000`):

```
base=0x04D20000 rect=(0,0)-(368,368) w_div8=46 surface_pitch=560 pitch_aligned=560 msaa=0 scissor=(0,0)+(368x368)
base=0x04D20000 rect=(0,0)-(312,312) w_div8=39 surface_pitch=560 pitch_aligned=560 msaa=0 scissor=(0,0)+(312x312)
```

**Findings:**

1. **The SCISSOR is the limiter**, not the surface pitch. `surface_pitch = 560`
   and `pitch_aligned = 560`, both larger than 368 - so the surface-pitch clamp
   never engages. The rect is exactly the scissor.
2. **The size varies per call** (368, then 312) - consistent with shadow-map
   cascades into a depth surface.
3. **`GetScissorTmpl` is byte-identical between the two trees** (the only diff
   lines are our own probes).

So: identical scissor code, identical resolve maths, identical clamps - yet
different inputs. **The guest register state must differ when this resolve is
reached.**

### That points at the ORDERING difference

Recorded earlier and not yet chased: theirs resolves `0x04D20000` at **n=5**,
ours at **n=25 (last)**. Same guest command stream, different position in it.

If our build reaches this resolve at a different point in the frame, the scissor
registers naturally hold different values - which explains identical code
producing different rects **without any arithmetic being wrong**.

**Hypothesis to test next:** we are **missing or deferring earlier resolves** to
this address - the ones XenDroid performs with the larger (512) scissor. Our
n=25 may be a LATER cascade, with the earlier full-size ones never issued or
dropped.

### NEXT TEST (replaces the old section 3)

Remove the enumerator's dedup for `0x04D20000` and log **every** resolve to it,
in **both** builds, with the scissor and a frame/draw counter:

- If XenDroid performs resolves ours never does -> find why ours are dropped
  (candidates: the `!width_div_8 || !height_div_8` early-out, or an upstream
  condition that skips the resolve entirely).
- If both perform the same count but with different scissors -> the divergence
  is upstream in the register state, and the ordering is the clue.

⚠️ Do NOT re-diff `GetScissorTmpl`, `GetResolveInfo`'s maths, or the
surface-pitch clamp. All three are confirmed identical.

---

## 10. ⭐⭐⭐ 2026-08-12 BREAKTHROUGH: the GUEST is setting a different scissor

Logged **every** resolve to `0x04D20000` in both builds, identical probe:

| | scissor across 40 resolves |
|---|---|
| **XenDroid** | `(0,0)+(512x512)` - **CONSTANT, all 40** |
| **Canary AE** | `368x368` (x4) -> `328x328` (x4) -> `248x248` (x4) -> `232x232` (x4) ... **shrinking** |

`surface_pitch = 560` in **both**. `msaa = 0` in both. The scissor code is
byte-identical. The resolve maths is byte-identical.

**So the scissor REGISTERS differ - the guest is writing different values.**

### What this means

This is **not a GPU-code bug.** Our rect is exactly the scissor the guest asked
for; we compute it correctly. The game is *choosing* a smaller shadow-map
resolution in our emulator, and it shrinks over time (368 -> 328 -> 248 -> 232)
in groups of four - the signature of a **dynamic/adaptive shadow LOD** stepping
down.

XenDroid holds a constant 512, i.e. the game keeps its shadow resolution pinned
at maximum there.

### New leading hypothesis

Halo 3 has an **adaptive quality mechanism** that lowers shadow resolution in
response to some measured value - most likely **frame timing** (we run at ~15
FPS vs their ~18-20), or a timer/counter we emulate differently. Under our
emulator it keeps stepping the cascade resolution down.

If the vista is composited from these under-sized depth surfaces, that would
explain a wrong-looking image while every piece of GPU code is provably correct.

### Why this reframes everything

Nineteen GPU-side hypotheses failed because **the GPU side is not wrong**. The
divergence enters on the **guest/CPU side** - the game behaves differently under
our emulator. That is consistent with every prior elimination.

### NEXT TESTS

1. **Confirm the adaptive-LOD theory.** Log `PA_SC_WINDOW_SCISSOR_TL/BR` writes
   with a frame counter in both builds - does ours start at 512 and step down,
   or start low? If it starts at 512 and decays, it is adaptive quality reacting
   to our performance.
2. **If it decays:** find the guest input driving it. Prime suspects are frame
   timing / `LOAD_CLOCK` / the vblank rate (this project has a history of clock
   bugs - see the vblank flood fix) rather than anything in the GPU backend.
3. **Cheap sanity check:** does the vista look correct in the very FIRST frames
   before the scissor steps down? If yes, that is near-proof.

⚠️ Do not spend more effort on GPU-side resolve/composite code until (1) is
answered. The evidence now says the inputs are wrong, not the processing.

### `internal_display_resolution` — TESTED, NOT the cause

Our global config held the **string** `'848x480'` in a cvar declared
`DEFINE_uint32(internal_display_resolution, 8, ...)` - a genuine malformed
value. XenDroid uses `8`. Looked like a strong candidate for the game choosing
smaller shadow maps.

**Set ours to `8` and re-tested: scissor still `368x368`, unchanged.**

The per-game Halo 3 config already specified `8`, and the failed global parse
evidently falls back to the default (also 8), so the malformed string never
changed the effective value. Worth fixing for hygiene; it is not the vista
cause.

### Still open: what makes the guest choose 368 instead of 512?

Confirmed by measurement:
- Our **first** resolve (`seq=0`) is already `368x368` - we never start at 512,
  so this is NOT gradual adaptation from a good state.
- Ours then steps DOWN in groups of four: 368 -> 328 -> 248 -> 232.
- XenDroid is a constant `512x512` across all 40.
- `surface_pitch=560`, `msaa=0`, identical in both.

So the game picks a smaller shadow-cascade size from the very first frame under
our emulator, and shrinks further. Remaining candidates for what the guest reads
to make that decision:

1. **Memory reported to the guest** (available physical pages / heap size).
2. **A capability or video-mode query** other than
   `internal_display_resolution` - e.g. widescreen flag, safe area, or the
   XAM video mode struct.
3. **Frame timing** feeding an adaptive quality path (we are ~15 FPS vs their
   ~18-20) - though the `seq=0` value being already low argues against timing
   alone.

**Next:** log the guest-visible video mode / XAM video query results in both
builds and diff. That is the class of value a game consults when sizing shadow
buffers, and it is CPU/kernel-side, consistent with every GPU-side hypothesis
having failed.

### Video mode query — TESTED, IDENTICAL

Mirrored XenDroid's `VdQueryVideoMode` log into Canary AE (they already had it -
worth knowing their tree carries useful instrumentation we can just copy):

```
Canary AE: VdQueryVideoMode #0: reporting 1280x720 (cvar mode 8)
XenDroid:  VdQueryVideoMode #0: reporting 1280x720 (cvar mode 8)
```

**Identical.** The guest is told the same display resolution by both emulators,
so the video mode is not what drives the different shadow-cascade size.

Remaining candidates for the guest-visible input:
1. **Memory reported to the guest** (available physical pages / heap sizing) -
   now the leading suspect.
2. Frame timing feeding an adaptive path (weakened: our `seq=0` is already low).
3. Some other capability/XAM query not yet enumerated.

**Method note:** XenDroid's tree already contains diagnostic logging we lack
(this `VdQueryVideoMode` line among them). Before writing a new probe, grep
their tree for an existing one - it is often already there and matching its
format keeps the two logs diffable.

### Config comparison (2026-08-12) — 35 differences, most eliminated

Diffed all 155 shared cvars between Canary AE and XDtester.

**Eliminated:**
- **Patches** - we load 480 community patches, XenDroid loads none. BUT every
  entry in `4D5307E6 - Halo 3.patch.toml` is `is_enabled = false`, so nothing is
  applied. Not the cause.
- **Video mode** - both report `1280x720 (mode 8)` to the guest (verified with
  matching `VdQueryVideoMode` logs on both sides).
- **`internal_display_resolution`** - our global held the malformed string
  `'848x480'` in a `DEFINE_uint32`; set to `8`, scissor unchanged.

**Still-differing settings, ranked as candidates:**

| cvar | ours | theirs | note |
|---|---|---|---|
| `logged_profile_slot_0_xuid` | `E0300000A360E000` | `E03000009C0D4593` | **⭐ TOP CANDIDATE** - different signed-in profile |
| `license_mask` | `1` | `0` | full licence vs none - changes unlocked content paths |
| `mount_cache` | `true` | `false` | cache partition availability |
| `vulkan_sparse_shared_memory` | `false` | `true` | host-side, but affects memory behaviour |

### ⭐ Leading hypothesis: the PROFILE

**Halo 3 stores its own graphics/quality settings in the player profile (GPD).**
We run a different profile from XenDroid. If that profile carries a lower
shadow-quality setting, the game would legitimately request a smaller shadow
cascade - which matches the evidence exactly:

- our very first resolve (`seq=0`) is already small, i.e. **not** adaptive decay
- the GPU code is provably correct and identical
- the guest is *choosing* the smaller size

**NEXT TEST (cheap, decisive):** run Halo 3 in Canary AE with **no profile
signed in** (or with XenDroid's profile), and check whether the scissor becomes
`512x512`. If it does, the vista bug is profile/save state, not code.

Also worth trying: `license_mask = 0` to match theirs.

### Profile — TESTED, NOT the cause

Ran Halo 3 with `logged_profile_slot_0_xuid = ''` (no profile signed in).
**Scissor still `368x368`.** Halo 3's profile-stored graphics settings are not
driving this. Config restored.

### Scissor call site — IDENTICAL

`GetScissorTmpl` is templated on `clamp_to_surface_pitch`, so an identical
function could still be called differently. Checked: **both call
`GetScissor(regs, scissor, false)`** - same template path. Eliminated.

---

## 11. WHERE THIS STANDS — a guest-execution divergence

Everything on the emulator side between the guest's register writes and the
final resolve is **proven identical**:

- `GetScissorTmpl` and its call site
- `GetResolveInfo` maths, the surface-pitch clamp, rect alignment
- The NDC/viewport path (identical per-draw values)
- The translated shader position math
- What we report to the guest for video mode (`1280x720`, mode 8)

And these guest-visible inputs are eliminated as the cause: **profile**,
**patches**, **`internal_display_resolution`**, **video mode**.

**Conclusion: the guest itself writes different `PA_SC_WINDOW_SCISSOR` values
under our emulator.** This is no longer a rendering bug - it is a
**guest-execution divergence**: Halo 3's own code takes a different path here
than it does under XenDroid.

That is a deeper and more expensive class of bug than anything chased so far,
and it will not be found by more GPU-side diffing.

### Candidate directions (unordered, none tested)

1. **Memory reported to the guest** - heap/physical page counts. A 2007 engine
   sizing shadow buffers from available memory is entirely plausible, and this
   has NOT been checked.
2. **CPU/JIT behavioural difference** - XenDroid has four spin/poll JIT passes,
   a guest scheduler, `inline_gprlr_saverest`, `context_promote_vec128` and
   native LSE atomics that we lack. Any of these changes guest timing or
   execution order.
3. **Kernel/XAM differences** - `license_mask` (ours `1`, theirs `0`) and
   `mount_cache` (ours `true`, theirs `false`) still differ and are untested.

### Cheapest next test

Set `license_mask = 0` and `mount_cache = false` to match XenDroid, and re-check
the scissor. Two config lines, one run - and it closes out the last untested
config differences before committing to the much larger memory/JIT hunt.

---

## 12. ⚠️⚠️ CRITICAL CORRECTION: the scissor size is NON-DETERMINISTIC

Three consecutive runs, `seq=0` scissor:

| run | config change | `seq=0` scissor |
|---|---|---|
| 1 | baseline | **368x368** |
| 2 | `license_mask=0`, `mount_cache=false` | **384x384** |
| 3 | `license_mask=0` only | **336x336** |

The value **varies run to run**. Reading run 2 as "license_mask moved the
scissor 368 -> 384" was mistaking noise for signal - corrected immediately by
run 3 going the other way.

`license_mask` and `mount_cache` are therefore **NOT** shown to affect it.
Config restored.

### What non-determinism actually tells us

Ours **fluctuates** (336-384 at `seq=0`, then decays: 368->328->248->232 within
a run). XenDroid is a **constant 512** across all 40 resolves.

That is the signature of a **dynamic/adaptive shadow LOD reacting to
performance**. We run ~15 FPS; XenDroid ~18-20 and never drops below maximum.
The game is lowering its own shadow-cascade resolution because our emulator is
slower.

### ⚠️⚠️ THEREFORE THIS LEAD IS PROBABLY NOT THE INVERSION

**A smaller shadow cascade makes shadows blurrier. It does NOT make a scene
render upside down.** The 46-vs-64 difference is real, measured and reproducible
in kind - but it is most likely a **quality** symptom of our lower frame rate,
not the cause of the **orientation** bug.

This whole thread (sections 1, 9, 10, 11) may be a red herring for the vista.
It was pursued because it was the only difference in the resolve list - but
"only difference found" is not "cause", and an inverted image and a smaller
shadow map are different failure modes.

### What this means for the investigation

- **Do not** invest further in 46-vs-64 as the inversion cause without first
  showing a mechanism by which cascade size could invert an image. There is no
  obvious one.
- The **inversion** remains unexplained, with the geometry path exhausted and
  the read side barely explored.
- If the adaptive-LOD reading is right, this difference should **disappear on
  its own** once our frame rate approaches theirs - making it a performance
  problem, not a rendering one.

**Suggested reset:** go back to the read side with fresh eyes. The composite's
texture *sampling* (coordinates, orientation of the RT-as-texture handoff) was
identified as the untouched half of the pipeline and has still never been
instrumented - only the texture *binding* was.

## 13. ⭐ DEFINITIVE: the GUEST writes the scissor. Registers logged in XDtester.

Instrumented `GetScissorTmpl` in XDtester to log the raw registers, filtered to
the shadow-cascade range:

```
win_tl=(0,0)  win_br=(512,512)  scr_tl=(0,0)  scr_br=(8192,8192)
winoff=(0,0)  ->  out=(0,0)+(512x512)
```

**`PA_SC_WINDOW_SCISSOR_BR = (512,512)` - written by the GUEST.**

Ours produces ~336-384 from the same code path, so the guest is writing
different values under our emulator. Our scissor computation is correct; it
faithfully reflects what Halo 3 asked for.

**This closes the question "what gives XenDroid its 512": the game does.**

### Consequence

Making Canary AE produce 512 means making Halo 3 *choose* 512 - i.e. fixing the
guest-execution divergence - not changing any GPU code. The value being
non-deterministic here (336/368/384, decaying within a run) and constant there
points at an adaptive quality path reacting to our lower frame rate.

⚠️ And per section 12, a larger shadow cascade would make shadows sharper, not
un-invert the vista. Forcing the scissor would be a hack that changes shadow
quality without addressing the orientation bug.

### To reproduce
`debug.canary.scissorlog` in XDtester (filtered to extent 300-600, square).
Add the same probe to Canary AE's `GetScissorTmpl` to compare register values
side by side.

---

## 14. Session round N+1 — the 46-vs-64 lead is DEAD, plus 5 new eliminations

### 14.1 FORCE512 result — the scissor was never the limiter (RETRACTS the lead)

`debug.canary.force_shadow_512` (experiment, default OFF, in `GetScissorTmpl`)
raised any square scissor in [200,512) to 512x512. It took: **120 hits, scissor
logged as `(0,0)+(512x512)`**.

**The resolve rect stayed 384x384** (`w_div8=48`).

So the rect is NOT clamped by the scissor in this path. Reading
`GetResolveInfo` (`draw_util.cc:1120-1155`) confirms why: the rect comes from
**guest memory** - vertex fetch constant 0, "D3D9 HACK: Vertices to use are
always in vf0, and are written by the CPU" - and only afterwards is clamped to
the scissor. With the scissor widened the guest vertices (384) bound the rect
by themselves.

**Consequences:**
1. The whole "46 vs 64 / surface_pitch clamp" line of attack is spent. The
   difference is guest-authored vertex data, not our clamp maths.
2. It could never have explained the symptom anyway: a smaller resolve is a
   smaller/blurrier image, **not a vertically mirrored one**. Flagged before
   the test, confirmed by it.
3. Screenshot `scratchpad/force512.png`: vista still upside down, unchanged.

**Do not re-run scissor/surface_pitch/resolve-rect experiments for the vista.**

### 14.2 XenDroid's resolve architecture (real, but NOT the cause)

XenDroid has an entire resolve subsystem we lack, exposed as cvars in
`vulkan_render_target_cache.cc`: `vulkan_in_pass_resolve` (in-pass EDRAM
resolve via `VK_KHR_dynamic_rendering_local_read`), `vulkan_direct_host_resolve`
("resolve eligible host render targets directly to guest memory with compute
shaders instead of first dumping the host render target back through EDRAM"),
`vulkan_in_pass_transfers`, `vulkan_normalize_dontcare_keys`,
`vulkan_depth_unorm24`, plus four `vulkan_in_pass_resolve_debug_*` probes.

⚠️ **`vulkan_resolve_to_texture` means something DIFFERENT in each tree.** In
XenDroid it means "have the in-pass resolve also store into the promoted
destination texture". Toggling the same-named cvar in our build was therefore
never the equivalent experiment.

**Test run:** set `vulkan_direct_host_resolve = false` in XDtester, i.e. force
XenDroid onto "always use the EDRAM dump path" - the path our build always
takes. **XDtester's vista still rendered correctly** (screenshot
`scratchpad/xdt_nodirect.png`, 19.8 FPS, horizon and terrain the right way up).

**=> The in-pass / direct-host resolve family is NOT what fixes the vista.**
This also means the EDRAM dump round-trip is not inherently the flipper.

### 14.3 Four more source-level eliminations (all confirmed identical)

| Suspect | Result |
|---|---|
| `GetHostViewportInfo` | **Byte-identical** between trees apart from our own probe code. Re-confirmed by direct diff, not inference. |
| Tessellation vertex winding | Both trees emit `spv::ExecutionModeVertexOrderCw`. (XenDroid moved it into `spirv_shader_translator.cc`; ours is in `vulkan_pipeline_cache.cc:2350`. Same value.) |
| Render-target path | Both retain `Path::kPixelShaderInterlock` and `kHostRenderTargets`. XenDroid **deleted the `render_target_path_vulkan` cvar**; ours is `''` (auto). Same code paths exist in both. |
| Fullscreen-pass Y direction | **The most promising of the four, and it is negative.** `fullscreen_cw.vs` (the VS behind every transfer/resolve/composite pass) scales Y by `XESL_Y_SCREEN_DIRECTION`. XenDroid renamed it `NDC_DIRECTION_Y_XE` and *widened* its condition to `#if SHADING_LANGUAGE_GLSL_XE \|\| XE_SLANG_SPIRV` (they compile via Slang, which would otherwise have taken the HLSL `-1.0` branch on Vulkan). We have no Slang path: `tools/build/compile_shader_spirv.py:96` passes `-DXESL_LANGUAGE_GLSL=1`, so we take `1.0` - **the same value XenDroid uses on Vulkan.** |

### 14.4 Where this leaves the vista

Everything in the *vertex* stage is now proven identical (viewport maths,
NDC scale/offset, translated SPIR-V, winding, fullscreen-pass Y). The guest's
own compositing quad uses guest shaders and guest vertices, identical in both.
The UI drawn over the vista is never flipped, and the final gamma/present pass
is shared by both - so the flip cannot be there either.

By elimination the mirrored data must already be **in the resolved texture**,
written by one of our own `.xesl` passes between the host RT and the sampled
texture: the EDRAM dump, the resolve shader, or the texture-load shader.

**Next step (not yet run) - the row-marker binary search.** Instead of
comparing more source, write a *known* row-indexed gradient at one stage of
that chain and look at the screen:
* if the gradient appears inverted, the flip is **downstream** of that write;
* if it appears correct, the flip is **upstream**.
Two or three rebuilds localise it exactly. This is a positive identification,
which is what the previous rounds of source-diffing have failed to produce.

---

## 15. THE ROW-MARKER RESULT — first positive localisation of the flip

After eight consecutive "not it" answers from source diffing, this is the first
test that says where the flip **is**.

### 15.1 The instrument

`debug.canary.resolve_row_marker` (experiment, default OFF). Two marker
variants of the 32bpp colour resolve shaders
(`resolve_full_32bpp_marker.cs.xesl`, `resolve_fast_32bpp_1x2xmsaa_marker.cs.xesl`,
both gated by `#define XE_RESOLVE_ROW_MARKER` inside the shared `.xesli`)
discard the resolved colour and instead write a vertical ramp keyed on the
**destination row**. They are compiled to their own bytecode headers and
substituted at pipeline-creation time in
`vulkan_render_target_cache.cc` (search `ROWMARKER`).

Why the resolve is the right place: the resolve is orientation-preserving **by
construction** - `XeResolveColorCopySourcePixelAddress...` and
`XeResolveDestPixelAddress` are indexed by the *same* `pixel_index` - so no flip
can originate there, which makes it a clean place to inject a known orientation.

### 15.2 The measurement (measured, not eyeballed)

Sampling column x=900 of `scratchpad/marker2.png` and taking per-band extremes:

| band top (screen y) | peak R | peak G | min R |
|---|---|---|---|
| 40 | 151 | 219 | 2 |
| 199 | 167 | 226 | 5 |
| 411 | 193 | 236 | 8 |
| 623 | 230 | 247 | 11 |
| 729 | 246 | 251 | 13 |

**The envelope rises monotonically from top to bottom.** Destination row 0
appears at the TOP of the screen.

### 15.3 Conclusion

**Everything downstream of the resolve destination write preserves orientation:
the texture load, the tiled addressing, the guest's sampling and the composite
are all innocent. The mirroring is introduced UPSTREAM - the EDRAM contents are
already mirrored by the time the resolve reads them.**

That narrows the remaining search to:
* the host render target -> EDRAM dump (generated in C++ in
  `vulkan_render_target_cache.cc`, not a `.xesl` file - note XenDroid has no
  dump/transfer shader sources either, both generate them), or
* the guest draws writing into the host RT / EDRAM already mirrored (the ROV
  `kPixelShaderInterlock` path derives its EDRAM address from the fragment
  coordinate - a Y-convention error there mirrors exactly this way and would
  affect only resolved 3D content, never the directly-drawn UI).

The ROV path is the stronger of the two: it is the one place left where a
fragment Y coordinate is turned into an EDRAM address.

### 15.4 Second, separate observation - a 32-row sawtooth

Superimposed on that envelope the ramp resets every **~53 screen px ~= 32
destination rows** - exactly `kTextureTileWidthHeight`. If write and read agreed
on the layout, a linear ramp in the destination would appear as a smooth linear
ramp on screen; instead the fast component is periodic at tile height while the
slow component tracks correctly.

This is an addressing inconsistency between the resolve write and the read side,
and it is plausibly the *visible* form of the recorded 46-vs-64 mismatch (we
resolve 368/384 wide into a surface the read side binds as 512x512). It is a
**second defect, independent of the flip**, and it now has a direct instrument.
Note it does NOT resurrect the surface_pitch theory as a cause of the flip -
section 14.1 stands.

### 15.5 Gotchas added this round

* **A tap on the Canary AE library tile only SELECTS it; it takes a second tap
  to launch.** A whole marker run was scored as "probe never fired" when the
  game had simply never started. Confirm the game is actually running before
  reading anything into an empty log.
* **`xe.log` is appended across sessions and is ~38 MB.** Old probe output from
  previous runs is still in it. Check the log's mtime and the screenshot before
  concluding a probe did or did not fire.
* `ui::vulkan::util::CreateComputePipeline` takes `const uint32_t*`, not
  `const void*`.

---

## 16. Full trace of the localised stage (host RT -> EDRAM -> resolve)

Section 15 localised the flip to *upstream of the resolve*. This section traces
that stage end to end in both trees.

### 16.1 Correction: AE is NOT on the ROV path

Section 15.3 named `kPixelShaderInterlock` as prime suspect. **Wrong.**
`vulkan_render_target_cache.cc` selects the ROV path only when
`cvars::render_target_path_vulkan == "fsi"`; AE's config has `''`, so AE runs
**`kHostRenderTargets`**. The ROV path is not used and cannot be the cause.

### 16.2 AE-only code in this stage — found, then eliminated

`read_resolved_1x`, `ResolveColorRenderTargetForDump`,
`DestroyResolvedDumpCompanions` exist in AE and **not at all** in XenDroid (an
Adreno MSAA workaround). Promising - AE-only code in exactly the localised
stage - but **dormant**: `pipeline_key.read_resolved_1x = 0;` is hardcoded, with
a comment recording it as already disproven. Dead code. Eliminated. The
`vkCmdResolveImage` it would use has zero offsets and full extent, so it is
orientation-preserving anyway.

### 16.3 Everything else in the stage is IDENTICAL

Verified by direct diff, not inference:

| Component | Result |
|---|---|
| Dump shader (`GetDumpPipeline`, generated SPIR-V) | Y/tile maths identical; XD's only changes are `native_layout`/`source_scale_native` for its scale classes, which are no-ops at scale 1 |
| Transfer shader (`GetTransferShader`) address maths | Identical |
| Transfer rectangle vertex generation | Identical |
| `transfer_viewport` + `pixels_to_ndc_y` | Identical (both positive) |
| `VulkanCommandProcessor::SetViewport` | Identical |
| `GetViewportInfoArgs::Setup` — `origin_bottom_left` | **`false` in both** |

Two `Setup` arguments *do* differ, and both are behaviourally equivalent under
default config, but they are worth knowing:
* `allow_reverse_z`: AE hardcodes `true`; XD passes `cvars::vulkan_allow_reverse_z`,
  which **defaults to `true`**.
* `convert_z_to_float24`: AE hardcodes `false`; XD passes
  `host_render_targets_used && depth_float24_convert_in_pixel_shader()`, and
  AE's own `depth_float24_convert_in_pixel_shader` **defaults to `false`**.
  AE has the cvar but never implemented the accessor, so the plumbing is absent
  rather than merely off.

### 16.4 REAL divergence found — gamma render targets (Halo 3 by name)

The one substantive difference in this stage:

| | cvar | default |
|---|---|---|
| Canary AE | `gamma_render_target_as_srgb` | **false** |
| XenDroid | `gamma_render_target_as_unorm16` | **true** |

XenDroid replaced the sRGB approach with promotion to `R16G16B16A16_UNORM` and
**turned it on**, and carried the conversion through the dump shader
(`bool is_gamma = ...k_8_8_8_8_GAMMA`, "stored as linear in the unorm16 host
render target, so encode RGB linear -> gamma before packing"). AE does **no**
gamma render-target handling at all.

Both trees carry the upstream comment tying this directly to Halo 3:
*"conversion in pixel shader output ... results in incorrect blending,
especially visible on decals in **4D5307E6**"* - and 4D5307E6 is Halo 3.

**Test:** set `gamma_render_target_as_srgb = true` in Canary AE (config only, no
rebuild), cleared the pipeline cache, relaunched.

**Result (`scratchpad/gamma_srgb.png`): a large, real improvement in the vista's
image - terrain texture, the ship hull, cables and structures all resolve where
before the vista was flat dark navy. The ORIENTATION is unchanged: ground still
at the top, sky at the bottom.** Frame rate fell from ~17 to ~9.

So gamma handling is a genuine Canary-AE defect with a visible fix, but it is
**not** the flip. Reverted to `false` pending a decision on the perf cost;
the proper fix is to port XenDroid's `gamma_render_target_as_unorm16` path
rather than enable the lossy sRGB one.

### 16.5 State of the flip

Within the localised stage, every code path that could mirror has now been
diffed and matches XenDroid. The flip is therefore **not in the host-RT ->
EDRAM -> resolve code**; it must be in the *content of the host render target*,
i.e. established before the dump ever runs.

Next: capture the host render target image directly rather than reasoning about
it. AE already has `TestrigCaptureImageDeferred(image, layout, width, height)`
wired into `ResolveColorRenderTargetForDump` (currently behind `if (false)`).
Pointing that at the vista's colour RT and viewing the captured image answers
"is the host RT itself mirrored?" outright - the same measure-don't-diff move
that produced section 15.

### 16.6 Host-RT capture — instrument built, result inconclusive

Built on the existing (dormant) capture path, gated by
`debug.canary.halo3_vista_probe`:

* `TESTRIG_ROWPROFILE` — mean luminance per 8 row bands of the captured render
  target, added to `TestrigReadCapturedImage`. Reads orientation directly off a
  sky-over-ground scene without needing an image viewer.
* `VISTA_RTCAND` — enumerates every colour RT receiving a transfer.
* `debug.canary.vista_rt_base` — selects which RT is captured (default 1216,
  the historical hardcoded guess).
* Column capture — `TestrigCaptureImageDeferred` now grabs a 4-px-wide
  **full-height** strip instead of a 256x256 top-left crop when the RT is
  taller than 256, so a whole-image vertical profile is actually visible.

**Findings:**

1. **RT 1216 was the wrong target all along.** Its profile is
   `1 1 1 1 1 0 0 0` — essentially black, confirming the
   `HALO3_FINDINGS_CHECKLIST.md` note that it is a uniform near-black
   feedback-decay target. Every earlier capture aimed at 1216 was measuring
   nothing. The probe no longer hardcodes it.
2. The real candidates are:

   | base_tiles | fmt | msaa | pitch_tiles |
   |---|---|---|---|
   | 1216 | 3 | 1x | 15 |
   | **0** | **0** | **4x** | **29** |
   | 608 | 0 | 1x | 15 |

   `base_tiles=0, 4x MSAA, pitch 29` is the main scene target.
3. Its full-height profile (568 rows) is `258 271 67 0 0 0 0 0` — **the image
   occupies only the top ~213 rows of a 568-row render target; the rest is
   black.** Within the content the two bands are nearly equal (258 vs 271), so
   this does **not** yet resolve orientation.

**Do not read a conclusion into this yet.** What it does establish is that the
vista's scene fills only the top ~37% of its render target, which is itself
worth explaining and is consistent with the recorded 46-vs-64 size anomaly
(a resolve/read disagreement about how tall this surface is).

**Next:** the two content bands are too coarse. Re-run with more bands (say 32)
over just the populated rows, and compare the same profile from XDtester — the
symmetric probe harness already exists in that build. A profile that rises in
one build and falls in the other is the flip, stated as a number.

---

## 17. Batch elimination + a CONFIRMED AE regression

### 17.1 It is not a XenDroid GPU feature (batch test)

Set **seven** XenDroid-only GPU features to their AE-equivalent values at once in
XDtester: `vulkan_dynamic_rendering`, `vulkan_in_pass_transfers`,
`vulkan_in_pass_resolve`, `vulkan_normalize_dontcare_keys`,
`vulkan_depth_unorm24`, `gamma_render_target_as_unorm16`,
`rt_cache_ownership_claim_memo` — all `false`.

**XDtester still rendered the vista correctly, at 23.8 FPS**
(`scratchpad/xdt_batch.png`). None of XenDroid's GPU features is what fixes the
vista. Combined with sections 14 and 16 this closes off "XenDroid does something
special" as a line of attack: **XenDroid is close to upstream here, and it is
Canary AE that regressed.**

The right comparison is therefore **AE vs upstream canary**, not AE vs XenDroid.
Local trees, by diff size against AE's `vulkan_render_target_cache.cc`:
`xeniatest/canary-git` **1112 lines** (closest base), `canary-fork` 1112,
`xeniatest/oracle` 1700 (a newer canary — it has the `copy_native` /
`IsResolveSourceNativeOnly` work AE predates, which is also where XenDroid's
`source_scale_native`/`native_layout` come from; at scale 1 all of it is a
no-op, so it is not the flip).

### 17.2 CONFIRMED REGRESSION: AE deleted upstream's gamma render-target support

Diffing AE against `canary-git` shows AE **removed** code upstream has:

```
-    gamma_render_target_as_unorm16_ = false;              (x2, capability setup)
-      return gamma_render_target_as_unorm16_ ? VK_FORMAT_R16G16B16A16_UNORM
-                                             : VK_FORMAT_R8G8B8A8_UNORM;
-bool VulkanRenderTargetCache::IsGammaFormatHostStorageSeparate() const {
-  return gamma_render_target_as_unorm16_;
-}
```

`grep -c gamma_render_target_as_unorm16` → **canary-git: 5+, Canary AE: 0.**

So AE has neither upstream's UNORM16 path nor XenDroid's; its only gamma option
is the older `gamma_render_target_as_srgb`, defaulted off. This is a genuine
Canary-AE regression against its own upstream, and it is **independently
confirmed by measurement**: setting `gamma_render_target_as_srgb = true`
visibly restores the vista's detail (terrain, ship hull, cables, structures
instead of flat navy) - section 16.4, `scratchpad/gamma_srgb.png` - at a cost of
~17 -> ~9 FPS.

**Fix to apply:** restore `gamma_render_target_as_unorm16_` from `canary-git`
(`VulkanRenderTargetCache` capability setup, `GetColorVulkanFormat`,
`IsGammaFormatHostStorageSeparate`) rather than shipping the lossy sRGB path.
This fixes vista *fidelity*. It does **not** fix the flip - tested.

### 17.3 The flip: still open, and the search is now correctly framed

Not fixed. What is now established:
* preserved orientation everywhere downstream of the resolve (s15);
* every host-RT -> EDRAM -> resolve code path identical to XenDroid (s16);
* not caused by any XenDroid GPU feature (s17.1);
* therefore an **AE-vs-upstream-canary regression**, in a 1112-line diff.

**Next step, concrete:** walk the `canary-git` -> AE diff of
`vulkan_render_target_cache.cc` (1112 lines, much of it TESTRIG noise that
filters out easily) and of `vulkan_command_processor.cc`, looking for AE
deletions like the gamma one above. The gamma regression was found in a single
filtered grep of AE's *removed* lines - that same filter over the remaining
files is the cheapest next move, and it is a far smaller space than the XenDroid
comparison ever was.

Also open from s16.6: the vista's scene occupies only the **top ~213 of 568
rows** of its render target (`base_tiles=0`, 4x MSAA, pitch 29). Unexplained,
and consistent with the recorded write/read size disagreement.

---

## 18. Gamma path: a real latent bug FIXED, and a RETRACTION

### 18.1 ⚠️ RETRACTED: "gamma_render_target_as_srgb=true improves the vista" (s16.4)

**That result was a confound, not an effect.** On the `kHostRenderTargets` path
the cvar was wired to nothing (see 18.2), so it could not have changed anything.
The visible improvement in `scratchpad/gamma_srgb.png` came from the **pipeline
cache being deleted** on that run, which was changed at the same time.

Classic single-variable failure - two things changed in one run. Recorded so it
is not cited again.

### 18.2 THE BUG (fixed): the cvar was dead on the path AE actually uses

`VulkanRenderTargetCache` has a complete sRGB gamma implementation -
`GetColorVulkanFormat` returns `VK_FORMAT_R8G8B8A8_SRGB`, plus handling in the
transfer and framebuffer paths (lines ~1557, ~1852, ~2003). But
`gamma_render_target_as_srgb_` was:

* declared `= false` in the header,
* assigned `false` in the `kPixelShaderInterlock` branch,
* **never assigned at all in the `kHostRenderTargets` branch.**

`render_target_path_vulkan` defaults to host render targets, so the member was
permanently `false` and `cvars::gamma_render_target_as_srgb` did nothing on the
only path the backend runs. Dead cvar, live implementation.

**Fix applied** (`vulkan_render_target_cache.cc`, `kHostRenderTargets` branch):

```cpp
gamma_render_target_as_srgb_ = cvars::gamma_render_target_as_srgb;
```

Default remains `false`, so shipped behaviour is unchanged; the difference is
that the cvar is now functional.

### 18.3 Measured result — the cvar works, but sRGB is the wrong curve

Controlled A/B, both runs with the pipeline cache deleted, same region
(x 80-620, y 40-560), 8 bands:

| | profile | mean |
|---|---|---|
| gamma OFF (control, `prof_screen.png`) | 83 78 69 73 79 104 133 145 | **95** |
| gamma ON (`gamma_wired.png`) | 369 370 373 376 373 376 378 363 | **372** |

The cvar now has a large, unmistakable effect - which **proves the wiring fix** -
but the image is *worse*: flat, washed out, low contrast, the UI blown out with
it, and ~9 FPS. Expected: Xenos piecewise-linear gamma is **not** sRGB, so
encoding it as sRGB applies the wrong transfer curve.

**Conclusion: keep `gamma_render_target_as_srgb` default OFF.** The correct fix
for 8_8_8_8_GAMMA fidelity is `gamma_render_target_as_unorm16` (promote to
`R16G16B16A16_UNORM`, blend in linear space).

### 18.4 Why "restore upstream's gamma path" is not enough

Upstream `canary-git`'s Vulkan backend **hardcodes it off in both paths**:

```cpp
// TODO(Triang3l): When color space conversion is implemented in the ownership
// transfer and resolve dump shaders, allow `gamma_render_target_as_unorm16` ...
gamma_render_target_as_unorm16_ = false;   // kHostRenderTargets
gamma_render_target_as_unorm16_ = false;   // kPixelShaderInterlock
```

So restoring upstream verbatim would be a **no-op on Vulkan** - upstream never
enables it there; it is a D3D12-only feature upstream. **XenDroid completed that
TODO**, which is what their dump-shader gamma encode is
(`bool is_gamma = ... k_8_8_8_8_GAMMA`, "stored as linear in the unorm16 host
render target, so encode RGB linear -> gamma before packing").

Porting it therefore means porting XenDroid's colour-space conversion into the
ownership-transfer and resolve-dump shaders, plus
`IsGammaFormatHostStorageSeparate()` (which AE's base predates entirely - AE has
zero references to it, upstream has it as a pure virtual with a
`GetColorResourceFormat` call site). That is a real port, not a restore.

**Still unrelated to the flip**, which remains open per section 17.3.

---

## 19. THE REGRESSION: AE passes RAW pitch/height to the resolve tiled-address helpers

Continuing the "what did AE delete from upstream" filter that found the gamma
bug (s18), applied to `draw_util.cc` (102 removed lines):

### 19.1 What upstream does vs what AE does

Upstream `canary-git` aligns **both** the destination pitch and height, then
feeds the aligned values to every tiled-address helper:

```cpp
const uint32_t copy_dest_pitch_aligned =
    xe::align(rb_copy_dest_pitch.copy_dest_pitch,
              texture_address::kStoragePitchHeightAlignmentBlocks);
const uint32_t copy_dest_height_aligned =
    xe::align(rb_copy_dest_pitch.copy_dest_height,
              texture_address::kStoragePitchHeightAlignmentBlocks);
...
copy_dest_base_adjusted += texture_address::Tiled3D(
    dest_base_x, dest_base_y, 0,
    copy_dest_pitch_aligned, copy_dest_height_aligned, bpp_log2);
texture_util::GetTiledAddressLowerBound3D(..., copy_dest_pitch_aligned,
                                          copy_dest_height_aligned, ...);
texture_util::GetTiledAddressUpperBound3D(..., copy_dest_pitch_aligned,
                                          copy_dest_height_aligned, ...);
```

AE passes the **RAW register values** to all three:

```cpp
copy_dest_base_adjusted += texture_util::GetTiledOffset3D(
    ..., rb_copy_dest_pitch.copy_dest_pitch,
         rb_copy_dest_pitch.copy_dest_height, bpp_log2);
```

**The tiling maths derives every row's address from the pitch and height it is
given.** Feeding raw values where the read side assumes aligned ones places rows
at the wrong offsets in the destination surface. AE also dropped the
`texture_address` module upstream added (AE has zero references to it; both
canary-git and XenDroid have it).

This is consistent with every unexplained addressing symptom on record:
* the **32-row sawtooth** in the row-marker (s15.4) - 32 is exactly
  `kTextureTileWidthHeight`, the alignment granularity;
* **46 vs 64** - resolving 368/384 into a surface the read side binds as 512;
* the vista occupying only the **top 213 of 568 rows** of its RT (s16.6).

### 19.2 A partial toggle already existed, and was untested — now tested

A previous session spotted the pitch half of this and added
`debug.canary.resolve_aligned_pitch` (experiment, default OFF), but wired it
only into a local `copy_dest_pitch_for_tiling` used by the 2D branch.

**Tested this session: enabling it does NOT fix the orientation**
(`scratchpad/aligned_pitch.png`). That is not a refutation of the theory,
because the toggle is not upstream's behaviour:
* it covers **pitch only** - `copy_dest_height` is still never aligned anywhere
  in AE, and height is what the *vertical* addressing depends on;
* it does not reach the **3D branch**, which still passes both raw values;
* upstream aligns to `texture_address::kStoragePitchHeightAlignmentBlocks`,
  while the toggle aligns to `kTextureTileWidthHeight` - these are not
  necessarily the same constant.

### 19.3 The fix to implement (specified, not yet applied)

Restore upstream's version properly in `GetResolveInfo`:
1. Compute `copy_dest_pitch_aligned` **and `copy_dest_height_aligned`** with
   `xe::align(..., texture_address::kStoragePitchHeightAlignmentBlocks)`.
2. Pass both to **all** of `GetTiledOffset3D`/`Tiled3D`,
   `GetTiledAddressLowerBound3D`, `GetTiledAddressUpperBound3D`, and the
   corresponding 2D calls in the `else` branch.
3. That needs `texture_address.h` (the alignment constant and helpers) ported
   from `canary-git` - AE lacks the module entirely.
4. Keep it behind an experiment toggle covering the **whole** change (retire the
   partial `resolve_aligned_pitch`), so it can be A/B'd in one flip.

This is the strongest outstanding lead: it is a confirmed AE-vs-upstream
regression, it sits exactly where the row-marker localised the problem, and its
granularity (32) matches the measured sawtooth period exactly.

### 19.4 Implemented and tested — real regression fixed, but NOT the flip

Implemented upstream's behaviour in full, replacing the partial
`resolve_aligned_pitch` experiment with **`debug.canary.resolve_aligned_storage`**
(experiment, default OFF), in `draw_util.cc` `GetResolveInfo`:

* align **both** pitch and height to 32
  (`kStoragePitchHeightAlignmentBlocks` == `kTextureTileWidthHeight` == 32, so
  upstream's `texture_address` module is **not** needed to match its behaviour);
* pass both aligned values to **all six** helper calls - `GetTiledOffset3D`,
  `GetTiledAddressLowerBound3D`, `GetTiledAddressUpperBound3D` and the three 2D
  equivalents. The 3D branch had been passing raw values on every call.

**Result: the vista is still upside down** (`scratchpad/aligned_storage.png`).

The change is **not** a no-op - 28% of sampled pixels differ from the control
(`prof_screen.png`), though the vista is a live animated scene so some of that
is frame variance. It measurably alters resolve destination addressing without
altering orientation.

**Verdict:** a genuine AE-vs-upstream correctness regression, now restorable
with one toggle - but the flip is a *different* defect. Three symptoms
(32-row sawtooth, 46-vs-64, 213-of-568 rows) pointed here convincingly and this
was still not it, which is worth remembering: destination-addressing granularity
symptoms and the mirroring are separate problems.

**Kept, default OFF.** Before it becomes the default it needs A/B on NFS Carbon
and Geometry Wars, since `GetResolveInfo` is shared by every title.

### 19.5 Running tally of AE-vs-upstream regressions found by the deletion filter

The `diff canary-git AE | grep '^-'` filter has now produced two real defects in
two files:

| File | Regression | Status |
|---|---|---|
| `vulkan_render_target_cache.cc` | `gamma_render_target_as_srgb_` never assigned on `kHostRenderTargets` - the cvar was dead | **Fixed** (s18.2); sRGB itself is the wrong curve, keep cvar off |
| `draw_util.cc` | raw instead of aligned pitch/height into the tiled-address helpers | **Fixed behind toggle** (s19.4); not the flip |

Files not yet swept, by removed-line count:
`vulkan_command_processor.cc` **901**, `vulkan_texture_cache.cc` **522**,
`texture_cache.cc` **184**, `render_target_cache.cc` 10.

`vulkan_texture_cache.cc` is the priority: it is the **read** side of the
resolve round trip, the one part of the path the row-marker measurement (s15)
did not clear by construction, and it is 522 removed lines unexamined.

### 19.6 `vulkan_texture_cache.cc` sweep — one difference, probably not the flip

Applied the deletion filter to the read side (522 removed lines). The one
addressing-relevant difference:

```cpp
// upstream canary-git: always in blocks
load_constants.guest_pitch_aligned =
    level_guest_layout.row_pitch_bytes / bytes_per_block;

// Canary AE: only converted for TILED textures
uint32_t level_guest_pitch = level_guest_layout.row_pitch_bytes;
if (texture_key.tiled) {
  level_guest_pitch /= bytes_per_block;   // "Shaders expect pitch in blocks"
  assert_zero(level_guest_pitch & (xenos::kTextureTileWidthHeight - 1));
}
load_constants.guest_pitch_aligned = level_guest_pitch;
```

For **linear** textures AE passes the pitch in **bytes** where upstream passes
**blocks** - a unit mismatch on the read side. It may be a deliberate AE fix
rather than a regression (the assert and comment suggest it was reasoned about).

**Probably not the vista flip:** resolve destinations are tiled, so
`texture_key.tiled` is true for the vista's texture and AE's path matches
upstream exactly. Recorded rather than pursued; worth revisiting if a
linear-texture bug shows up elsewhere.

Remaining unswept: `vulkan_command_processor.cc` (901 removed lines) and
`texture_cache.cc` (184).

### 19.7 Session end state

All experiment toggles OFF; `gamma_render_target_as_srgb` false;
XDtester config restored. Screenshot at shipping defaults:
`scratchpad/current_defaults.png` - **still inverted**.

Two real AE-vs-upstream regressions were found and fixed this session (s18.2,
s19.4); neither is the flip.

### 19.8 NDC-Y now eliminated by MEASUREMENT, not by reading code

`vulkan_command_processor.cc`'s 901 removed lines contain **nothing**
orientation-related (only resolution-scale plumbing and a submission-retry
flag), so the deletion filter is exhausted on the big files.

Ran the `NDCYDRAW` probe — which exists in **both** builds with identical
placement and format — in Canary AE and XDtester on the same game and device,
and joined the two logs on vertex-shader hash:

* Canary AE: 42 distinct (shader, Y-regime) pairs
* XDtester: 40
* **Shaders present in both with a differing `ndc_scale_y` or `extent_y`: ZERO.**

Every shared shader gets `ndc_scale_y = -1, flipped = 1` in both builds
(the one exception, `0A6D1DD7767FDF27`, is the pre-transformed 2D UI at
`0.00024414062 / flipped=0` — identical in both).

The doc previously listed "NDC scale/offset (identical)" as eliminated, but that
came from reading code. **It is now a measured A/B**, which is a much stronger
claim: the guest draws receive the same Y transform in the build that renders
correctly and the build that renders inverted.

XDtester logs one shader AE never does — `56FE9FEA13FD93F3`, `extent_y=512`
(note: 512, the recurring number). Worth a look, but it may simply be a draw AE
skips for unrelated reasons.

**Implication:** with the vertex-stage Y transform now measured identical, and
the resolve/dump/transfer paths measured or diffed identical, the inversion is
looking less like a transform applied in the wrong direction and more like a
difference in *which surface a draw lands in*. That is a different class of bug
from everything tried so far.

## 20. The guest-constant comparison (VSCONST) — AE side captured, XDtester side PENDING

**Reasoning:** the GPU renders what it is given. NDC transform, translated
SPIR-V, viewport maths, resolve, dump and transfer are now all *measured or
diffed identical* to XDtester. What has never been compared is the DATA - the
view/projection matrix the guest computes on the PowerPC side and uploads as
float constants. A sign error there inverts the scene while leaving every
GPU-side check identical, and it would not touch the 2D UI (pre-transformed,
never passes through a guest matrix). It would also plausibly unify the vista
with the character "ball", whose open lead is bone-matrix constants.

**Probe:** `debug.canary.vsconst` (diagnostic, default OFF), added beside
NDCYDRAW in BOTH trees with identical name and format so the logs join on vertex
shader hash. Dumps c0-c7 as decimal and raw bits.

* **Canary AE: captured, 37 shaders** ->
  `scratchpad/vsconst_ae.txt`.
* **XDtester: NOT captured.** Two problems to fix first:
  1. `./gradlew :app:assembleDebug` reported success in 36s **without
     recompiling native code** - `emulator-core/build/.../libe.so` was days old.
     `:emulator-core:externalNativeBuildDebug` put VSCONST into
     `app/build/intermediates/stripped_native_libs/...`, but the **installed
     APK still has 0 VSCONST**, so packaging did not pick it up. Verify with
     `unzip -p <installed base.apk> lib/arm64-v8a/libe.so | strings | grep -c VSCONST`
     before trusting any run.
  2. That run also logged **0 NDCYDRAW** in 9661 lines - the game never reached
     the menu, so the library tap missed (same "first tap only selects" trap as
     Canary AE, s15.5).

**To finish (~15 min):** force a clean XDtester native build, confirm VSCONST in
the *installed* APK, launch and confirm the vista is on screen, then
`join` the two files on `vs=` and look for a sign difference in c0-c7 - most
likely in the Y row of the view-projection matrix.

If the constants are identical too, the guest maths is exonerated and the
divergence is the guest-written register state already on record (XenDroid's
guest writes PA_SC_WINDOW_SCISSOR_BR=512 and resolve vertices of 512; AE's
writes 336-384, non-deterministically) - which would point at CPU/JIT or kernel,
not the GPU backend at all.

## 21. THE ORDERING HYPOTHESIS — 336 vs 512 cannot come from identical code

User insight, and it reframes the strongest anomaly on record:

> "Something is not identical. We have 336 and they have 512. That doesn't
> happen with identical code... For example they generate it 4 in while we do
> 25 in. Canary AE gives us that value at a later point."

### 21.1 Why this is likely right

Two facts that have been on record separately and were never combined:

1. **XenDroid is STABLE at 512** - `win_br=(512,512)`, resolve vertices 512.
2. **Canary AE is NON-DETERMINISTIC** - 336, 368, 384 across runs of the same
   frame of the same game (s14.1, and the "values are non-deterministic" note
   that killed the `license_mask` theory).

A pure maths difference is deterministic. **Non-determinism against a stable
reference is the signature of a timing/ordering difference, not a calculation
difference.** Combined with the fact that the resolve vertices are read from
GUEST MEMORY written by the guest CPU (s14.1), the chain is:

  guest CPU computes a value -> writes registers/memory -> we sample it

and we may be sampling at a **different point in that sequence** than XenDroid -
reading state before the guest has finished producing it, or letting a draw
consume a stale register.

This also finally explains something never accounted for: why the numbers cluster
just BELOW 512 (336/368/384 = 42/46/48 x 8). That looks like a value caught
mid-convergence, not a wrong formula.

### 21.2 The test (designed, NOT yet run)

A sequence-numbered write trace, in **both** builds, identical format:

* hook the guest register write path (`CommandProcessor::WriteRegister` /
  `LogRegisterSet` - note the existing
  `log_guest_driven_gpu_register_written_values` cvar is compiled out behind
  `XE_ENABLE_GPU_REG_WRITE_LOGGING` and is far too noisy anyway);
* filter to `PA_SC_WINDOW_SCISSOR_BR` (and `RB_SURFACE_INFO.surface_pitch`);
* log `seq` (a global monotonic counter), the value, and the current
  swap/frame index.

Then answer, per build:
* **Does AE ever write 512 at all?** If yes but LATER than the resolve consumes
  it, the bug is ordering and the fix is where we sample, not what we compute.
* **At what sequence position does each build first see 512?** The user's
  "4 in vs 25 in" - if XenDroid reaches it early and we reach it late (or after
  the draw), that is the crumb.

### 21.3 Why this outranks everything still open

It is the only hypothesis that explains **non-determinism**, which no
calculation-based theory does - and every calculation-based theory tried so far
(46-vs-64, aligned pitch/height, ROV, in-pass resolve, gamma, NDC-Y) has been
eliminated by measurement.

⚠️ Do not spend more time diffing GPU backend source. Sections 14-20 have
established, by measurement rather than reading, that the GPU-side maths is
identical. The remaining difference is in WHEN state is produced and consumed.

## 22. MEASURED: the 512 path is non-deterministic IN THE GUEST

Built `debug.canary.regtrace` (diagnostic, default OFF), two probes sharing one
global monotonic guest-register-write counter (`g_ae_reg_write_seq`, defined in
`command_processor.cc`):

* **REGTRACE** in `CommandProcessor::WriteRegister` - every guest write to
  `PA_SC_WINDOW_SCISSOR_TL/BR` and `RB_SURFACE_INFO`, with `seq`. All bulk write
  paths (`WriteRegistersFromMem`, `WriteRegisterRangeFromRing`) funnel through
  `WriteRegister`, so nothing is missed.
* **RESOLVESEQ** in `GetResolveInfo` - the `seq` at the moment the resolve reads
  its rectangle out of guest memory (vf0), plus the raw vertices and dest.

### 22.1 Canary AE DOES produce 512 — the register path is not the difference

Run 1, at the menu: **682 writes of `SCISSOR_BR raw=0x02000200 x=512 y=512`**,
one per frame (seq gaps ~62,383). So the 512 exists in our guest too. The
scissor register was never the divergence.

### 22.2 ...but not every run — same game, same screen, ZERO 512s

Run 2, identical build, identical menu, vista confirmed on screen and still
inverted (`scratchpad/seqrun.png`), 160,317 REGTRACE lines captured:

| value | run 1 | run 2 |
|---|---|---|
| `x=512 y=512` | **682** | **0** |
| `x=1152 y=640` | 24,176 | 46,218 |
| `x=1152 y=160` | 6,872 | 16,243 |

**The guest either takes the 512 path or does not, run to run.** This is the
non-determinism finally caught in the act, and it is in **guest behaviour**, not
in our GPU code - our GPU code cannot make the guest stop issuing a register
write.

### 22.3 What this means

A GPU-backend bug cannot explain a guest that behaves differently on identical
input. The divergence is upstream of the GPU entirely: **CPU/JIT execution,
thread timing, or kernel state**. That is consistent with everything sections
14-21 eliminated by measurement on the GPU side, and it explains why two days of
GPU work produced only unrelated regressions.

It also fits the shape of the numbers: 336/368/384 clustering just below 512
looks like a quantity the guest is still converging on when we sample it.

### 22.4 Read points captured so far

Canary AE main-scene resolve reads vf0 at seq 3421, 6719, 9976, 12590, 15204,
17818, 20432, 23046 - a steady ~2,600-3,300 register writes apart, one per
frame, and `vfaddr` advances every frame (0x0510C23C, 0x0512841C, ...), so the
guest allocates a fresh vertex buffer each time. No staleness on that path.

**Still needed:** the same REGTRACE/RESOLVESEQ pair in XDtester, to answer
"when does XenDroid read them" against the same timeline. The probes are written
to be copy-pasted; note the XDtester build traps in s20 (native code silently
not rebuilt, and the library tap needing two presses).

**Next, and it is no longer a GPU question:** find what makes the guest take the
512 path in one run and not the next. Prime suspects are the vblank/timing path
and thread scheduling - the same class of defect as the thread-start lost-wakeup
race that was already found and fixed in this project.

## 23. Guest constants compared — NOT mirrored. And the "upside down" premise is in doubt.

### 23.1 VSCONST captured in BOTH builds

Finished the symmetric guest-constant comparison (s20). Two blockers fixed:

* **A real bug in XDtester's `xdt_debug.h`.** `DiagEnabled` used a single
  `static thread_local CachedProp cache;` with a comment claiming "one cache
  slot per call site". A function-local static is ONE object shared by every
  call site, so NDCYDRAW and VSCONST clobbered each other's cached property and
  whichever ran first in a 500 ms window decided the answer for both - which is
  why enabling `debug.canary.vsconst` produced zero output from either. Now
  keyed by the `prop_name` pointer (safe: every call site passes a distinct
  literal). **Fixed in XDtester.**
* The installed-APK check must use the real path
  (`find /data/app -name base.apk -path "*xdtester*"`); a glob inside
  `adb shell` silently fails and reports 0.

Captured: **Canary AE 37 shaders, XDtester 44**, 37 in common.

### 23.2 Result: the guest is NOT producing a mirrored transform

* **29 of 37 shared shaders have BYTE-IDENTICAL c0-c7.**
* The 8 that differ are all animated camera matrices - magnitudes match closely,
  signs differ as the camera moves.
* **Determinant of the view basis (c4,c5,c6) is +1.0000 in BOTH builds for all
  36 measurable shaders.** A reflection would be -1. There is none.
* The up-axis dominant component is POSITIVE in both, so the camera is not
  rolled 180 degrees either (which would be a proper rotation and could
  otherwise have hidden here).

**The guest's matrices are fine.** Combined with s14-s22, that eliminates the
transform as the cause at every stage: GPU-side maths, NDC, and now the guest's
own constants.

### 23.3 ⚠️ The premise itself now looks wrong

If nothing anywhere applies a mirror, the image should not be mirrored - so the
"upside down" description was tested directly:

* **Vertical luminance profile, same region, both builds:**
  AE `83 78 69 73 79 104 133 145` (rising),
  XDtester `105 100 93 85 95 112 124 109` (rising).
  **Both get brighter downward.** A vertical mirror would inverse that.
* **Correlation of the vista region:** AE vs XDtester as-is **+0.131**;
  AE flipped vertically vs XDtester **-0.194**. Flipping makes it *worse*, not
  better. (Both are weak because the menu camera animates, so this is
  suggestive rather than conclusive.)

**Working conclusion: this is probably not a vertical mirror at all.** More
likely the wrong REGION of the surface is being displayed - which fits the
otherwise-unexplained s16.6 finding that the vista's scene occupies only the
**top ~213 of 568 rows** of its render target. Sampling or compositing the wrong
crop of a larger surface can read as "upside down" at a glance while leaving
every transform in the pipeline provably correct.

**Next:** stop treating this as an orientation bug. Determine which region of
which surface the composite samples, in both builds - the same measure-don't-
diff approach that produced s15 and s22.

## 24. Memexport transplant #1: eA validation fix - does NOT fix the ball

Branch `xd-memexport-transplant`, commit 7d48dbf82.

**Ported:** XenDroid's memexport eA validation - Z lane uses all 12 bits of
const_0x4b0 (shift 20, compare 0x4B0) instead of the top 9 (shift 23, 0x96), so
the constants the shader accepts match what `draw_util::AddMemExportRanges`
derives ranges from. Also made the pre-existing compute-path validation bypass
opt-in (`debug.canary.memexport_bypass_validation`, default OFF) so the
corrected validation is actually exercised.

**Result: character models are STILL BALLS** (confirmed in real gameplay on
Sierra 117). The vista is still inverted.

The change is still correct - AE was masking this exact rejection with a hack -
but it is not the ball's cause. **Keep it, do not re-test it.**

### 24.1 Next transplant target, and it is already identified

`project_halo3_ball_xendroid_fix` in memory records how XenDroid actually fixed
the ball, and it is NOT the eA validation:

* **`ReadbackResolveMode::kUma` + host-visible shared memory** - guest RAM is
  aliased directly as the GPU shared-memory buffer, so guest RAM and the GPU
  never diverge. XenDroid's cvar: `shared_memory_zero_copy = true`
  ("Alias guest RAM directly as the GPU shared-memory buffer instead of
  uploading dirty pages each frame. Removes upload copies and **keeps memexport
  and resolve output coherent with the CPU for free**. Default on for ARM64
  (unified-memory) builds").
* ⚠️ **NOT** `VK_EXT_external_memory_host` - unsupported on every Adreno.

**Canary AE has neither `shared_memory_zero_copy` nor
`vulkan_shared_memory_host_visible` - the cvars do not exist in its source at
all.** AE uploads dirty pages each frame instead, so GPU-written memexport data
and the CPU's view of guest RAM can diverge. That is the mechanism that
collapses skinned geometry.

**Do this next:** port XenDroid's zero-copy / host-visible shared memory path
(`vulkan_shared_memory.cc` + `shared_memory.cc`), behind a default-OFF toggle,
and re-test the ball in gameplay.

### 24.2 Test procedure that works (do not rediscover this)

Synthetic taps and instantaneous keyevents do NOT drive the game.
`adb shell input keyevent --longpress 96` (A, HELD) with `sleep 3` between
presses, x5, gets from the main menu into gameplay. Halo 3's level intro is a
PRE-RENDERED VIDEO - frames from it contain no engine-rendered models and cannot
be used to judge the ball. See memory `feedback-gamepad-input-held-presses`.

## 25. Transplant #2: host-visible shared memory - landed, and it exposed a bigger gap

Commit 9b43bb570, branch `xd-memexport-transplant`.

**Ported** (`vulkan_shared_memory.cc/.h`): prefer a host-visible cached
(-coherent) memory type for the shared-memory buffer, test the candidates
against **this buffer's** `memoryTypeBits` (a whole-device test fails on Adreno,
whose LAZILY_ALLOCATED type is not host-visible), and map it persistently.
Unmaps on shutdown. Behind `debug.canary.shared_memory_host_visible`
(experiment, default OFF).

**Verified live on Odin 2 / Adreno 740 / Turnip:**
```
SHMHOSTVIS host-map decision: is_uma=false type_bits=0x7 device_local=0xf
  host_visible=0x7 host_cached=0x6 host_coherent=0x3
  -> host_visible=true coherent=true
SHMHOSTVIS buffer host-mapped, coherent=1
```
The buffer now lives in cached-coherent memory the CPU can read directly.

### 25.1 ⚠️ RETRACTED: "the Vulkan backend has no readback at all"

**That claim was WRONG, and it was my own tooling error.** The grep that
produced it ended in `head -8`, which truncated the Vulkan hits and left only
`command_processor.*` and the D3D12 backend visible.

Canary AE **does** have a Vulkan resolve readback path:
`vulkan_command_processor.cc:4729` in `IssueCopy()` calls
`GetReadbackResolveMode()`, checks destination accessibility, then uses a keyed
ring of staging buffers (`readback_buffers_`, `MakeReadbackResolveKey` - the
same helper name XenDroid uses) to copy device -> host and `memory::vastcpy`
into guest RAM.

Note also that `GetReadbackResolveMode()` maps **any unrecognised string to
`kFast`**, and AE's config contains `readback_resolve = false` (a bool written
into a string cvar), so readback is effectively **on** by default.

The one thing genuinely missing is the **`kUma` mode**: reading the host-mapped
shared memory directly instead of doing a device->host staging copy. That is
what the section 25 mapping enables, and it is a bounded addition rather than a
whole subsystem.

### 25.2 Next transplant (the consumer side)

Add `ReadbackResolveMode::kUma` to AE and take the direct host-mapped read when
the shared memory is host-mapped and resolution scaling is off, instead of the
staging copy. AE already has the surrounding readback machinery, so this is a
small addition on top of the section 25 mapping - not the hundreds of lines
first estimated.

## 26. ⭐ THE VISTA IS A CHEAP PROXY FOR THE BALL - use it

User, and it changes how every future transplant should be tested:

> "if the vista is upside down, there's a very good chance the character is a
> ball."

The two share a root cause. The vista renders at the **main menu**, ~45 seconds
from launch, with no input needed beyond two taps. The ball needs held-A
navigation through menus and a pre-rendered intro video, several minutes, and is
easy to get wrong (see s24.2).

**So: test the vista first. If it is still inverted, the transplant did not fix
the ball either, and the expensive gameplay run can be skipped.** Only go
in-game once a change actually straightens the vista.

## 27. Transplant #3: UMA readback - IMAGE IMPROVED, ball unchanged

`readback_resolve = "uma"` + `debug.canary.shared_memory_host_visible`
(commits 9b43bb570, d92712f5f).

**User verdict: "Still a ball but the image does look clearer."**

So the UMA direct readback is a genuine image-quality win - real, keep it - but
not the ball's cause. Reading resolve output straight from the host mapping with
no staging copy visibly improves what reaches the screen.

## 28. Transplant #4: memexport page tracking + fence/coherency awaits

Commit f89c1a251. Ported `command_processor_memexport.inc` verbatim into
`gpu/`, included into the Vulkan command processor's class body, with no-op
defaults on the base class, plus `MarkMemexportPagesWritten` after each
memexport range and the three pm4 hook points XenDroid uses:

* before `DispatchInterruptCallback` (fence),
* at the `COHER_STATUS_HOST` poll before `MakeCoherent` (coherency request),
* after `WriteEventInitiator` in the fence writeback path.

**Result: menu renders with no regression, vista STILL INVERTED.** By the
section 26 heuristic the ball is unchanged, so the gameplay run was skipped.

### 28.1 What remains untried from XenDroid

* `vfetch` bounds/format handling differences (`spirv_shader_translator_fetch.cc`).
* The **consumer-side routing** this page tracking was built to serve:
  XenDroid uses `VertexFetchInMemexportRange` to route geometry draws that read
  memexport output to the host-imported buffer. The tracking is now ported but
  **nothing consumes it yet** - `VertexFetchInMemexportRange` has no call site in
  AE. That is the natural next step and it is the half that actually changes
  behaviour.

## 29. ⭐ MEASURED: Canary AE's draws observe far fewer distinct bone poses

`debug.canary.bonedistinct` (diagnostic, default OFF) - added to **both** builds
with identical name and format. Hashes the bone-matrix constants (c144-c151) at
DRAW time and counts distinct states, so write/draw interleaving is measurable
rather than inferred.

Same scene (Halo 3 main menu), same device, same driver:

| | draws | distinct bone states |
|---|---|---|
| **Canary AE** | 163,840 | **142** |
| **XDtester** | 971,776 | **>=512** (hit the probe's 512-slot cap) |

**AE's count PLATEAUED at 142** - identical across three samples 1024 draws
apart, so it is saturated, not still climbing. XDtester exceeded 512.

This reproduces, against XenDroid rather than RADV, the pattern an earlier
session recorded in the CWRITE probe comment: bone-matrix constant WRITES arrive
in equal numbers (434k vs 438k) yet draws observe far fewer distinct states
(~11 vs ~165 there). Writes arriving but draws not seeing them means **writes
and draws are not interleaving** - updates batch up and draws only ever observe
a small set of states.

Skinned geometry drawn against a handful of poses instead of hundreds is exactly
a collapsed "ball", and per section 26 it plausibly shares a cause with the
inverted vista.

**This is the most concrete ball-specific measurement in the investigation so
far, and it points at the COMMAND PROCESSOR (write/draw ordering), not the
GPU backend** - consistent with s22, where the guest itself was shown to behave
non-deterministically run to run.

### 29.1 Caveats before acting on it

* XDtester also ran ~6x more draws in the same wall time (it is roughly twice
  the frame rate), so the totals are not directly comparable; the meaningful
  fact is that **AE saturates at 142 while XDtester does not saturate at all**.
* The 512-slot table caps XDtester's true figure - raise it to get the real
  number before quoting a ratio.

### 29.2 Next

Find why draws in AE observe so few distinct bone states. Candidates, in order:
1. Constant updates batched between draws (command processor submits draws
   without re-reading the register file, or coalesces them).
2. The bone constants arriving via a path AE processes differently
   (`WriteRegistersFromMem` / ring-buffer bulk writes).
3. Draw submission deferring past the constant writes.

The CWRITE probe in `command_processor.cc` already counts writes to the same
register range; pair it with BONEDISTINCT in one run to see writes and observed
states on the same timeline.

## 30. ⭐⭐⭐ THE GUEST ISSUES 4.6x FEWER DRAWS IN CANARY AE

The single most important measurement in this investigation.

`debug.canary.drawentry` counts draws **entering `IssueDraw`**, before any
emulator-side filtering. Same probe, same name and format, in both builds. Halo 3
main menu, same device, same driver:

| | draws entered | frames | **draws/frame** |
|---|---|---|---|
| **Canary AE** | 118,784 | 858 | **138** |
| **XDtester** | 854,016 | 1,332 | **641** |

**XenDroid's guest issues 4.6x more draw commands per frame than ours.**

### 30.1 It is not us dropping them

* `debug.canary.dropdraw`: **ZERO** draws discarded at the
  `host_vertex_shader_type` gate (`vulkan_command_processor.cc:2480`) - so the
  fact that AE accepts fewer types than XenDroid there
  (AE: kVertex / kPointListAsTriangleStrip / adaptive-triangle only; XenDroid
  additionally kRectangleListAsTriangleStrip and **all** domain types via
  `IsHostVertexShaderTypeDomain`) is real but **never fires at the menu**.
  Worth fixing on principle, not the cause here.
* Entry (118,784) vs mid-function (113,664) differ by only ~4%, so almost
  nothing is lost inside `IssueDraw` either.

**The draws never arrive. The guest does not issue them.**

### 30.2 And AE is slower while drawing far less

AE: 858 frames / 55 s = 15.6 FPS. XDtester: 1,332 / 55 s = 24 FPS. XenDroid is
1.5x the frame rate while issuing 4.6x the draws - roughly **7x more draw work
per second**. So AE being slow is not explained by draw load; it is slow *and*
drawing a fraction of the scene.

### 30.3 What this means

A GPU-backend bug cannot make the guest emit fewer draw commands. This is
**guest execution divergence**, and it corroborates section 22, where the guest
was caught emitting the 512x512 scissor path in one run and not at all in the
next.

The game is simply not drawing most of the scene in Canary AE. That is a
sufficient explanation for an incomplete/wrong vista AND for collapsed skinned
models, without any renderer defect at all - and it is consistent with every
GPU-side comparison in sections 14-23 coming back identical.

**The investigation should move to the CPU side: JIT correctness, kernel state,
and thread/timing behaviour.** Sections 14-29 have, between them, eliminated the
GPU backend fairly comprehensively.

### 30.4 Immediate next steps

1. Find where the guest's draw commands are lost: trace PM4 packet counts
   (`PM4_DRAW_INDX` / `PM4_DRAW_INDX_2`) per frame in both builds. If the ring
   buffer carries 4.6x fewer draw packets, the divergence is upstream of the GPU
   entirely (JIT/kernel); if the packets are there but do not reach `IssueDraw`,
   it is in the command processor's parsing.
2. That is a small probe in `pm4_command_processor_implement.h`, symmetric in
   both builds, and it cleanly splits "the guest never submitted them" from
   "we failed to parse them".

## 31. ⭐⭐⭐ CONFIRMED: the draws are missing from the RING BUFFER, not lost by us

`debug.canary.pm4draw` counts draw PACKETS parsed out of the ring buffer, at the
single point both `PM4_DRAW_INDX` and `PM4_DRAW_INDX_2` funnel through
(`ExecutePacketType3Draw`).

**Canary AE: `PM4DRAW packets=73728` and `DRAWENTRY entered=73728` - EXACTLY
EQUAL.** Every draw packet parsed becomes an `IssueDraw` call. The command
processor loses nothing.

| | draws/frame |
|---|---|
| Canary AE | 73,728 / 836 frames = **88** |
| XDtester | 744,448 / 1,212 frames = **614** |

Since AE's parsing is lossless, fewer `IssueDraw` calls **necessarily** means
fewer draw packets in the ring buffer. **~7x fewer draws are being submitted.**

### 31.1 This closes the question section 30 opened

The candidates were "the guest never submitted them" vs "we failed to parse
them". Parsing is proven lossless, so it is the former:

**the guest, running under Canary AE, does not submit the draw commands.**

Nothing in the GPU backend can cause that. Combined with s22 (the guest emitting
the 512x512 scissor path in one run and not the next) and the run-to-run
variation in this very measurement (138 draws/frame one run, 88 the next, versus
XenDroid's stable ~614-641), the picture is consistent: **guest execution in
Canary AE is both impoverished and non-deterministic.**

### 31.2 Where the investigation goes now

Out of the GPU entirely. The scene is not being drawn because the game is not
asking for it to be drawn. Targets, in order:

1. **JIT correctness** - wrong results in game logic would cause the title to
   skip rendering work (culling everything, failing visibility tests, taking
   error paths). The recorded JIT hotspot `guest_826DEFD0` (14% of process CPU,
   a polling loop) is a hint that guest control flow is not behaving.
2. **Kernel/threading** - a lost wakeup or mis-timed event would let a frame be
   built and submitted half-populated. This project has already found and fixed
   exactly that class of bug once (thread-start lost-wakeup race).
3. **Timing** - anything frame-rate or clock dependent the guest samples to
   decide how much to draw.

⚠️ **Do not spend more effort diffing the GPU backend.** Sections 14-31 have
eliminated it by measurement: identical maths at every stage, lossless packet
parsing, and now proof that the work never arrives.

### 31.3 Probe note

The XDtester side of PM4DRAW needs `#include "xenia/base/xdt_debug.h"` in
`pm4_command_processor_implement.h` (build failed on the undeclared macro). Not
needed for the conclusion above - AE's packet/entry equality carries it - but
add it if XenDroid's own packet count is ever wanted.

## 32. Occlusion queries: a REAL missing feature, but not the draw deficit

Chasing s31 ("the guest submits ~7x fewer draws"), the obvious mechanism is
visibility culling: a game that is told nothing is visible stops asking for
draws. Halo 3 uses ZPD (Z Pass Done) occlusion queries heavily.

### 32.1 Canary AE does not implement occlusion queries at all

| symbol (Vulkan backend) | Canary AE | XDtester |
|---|---|---|
| `vkCmdBeginQuery` | **0** | 4 |
| `vkCmdEndQuery` | **0** | 6 |
| `QueryPool` | **0** | 98 |
| `zpd` / `ZPD` (whole GPU dir) | 13 | 867 |

XenDroid has a dedicated **938-line `vulkan_zpd_query_pool.{cc,h}`** doing real
Vulkan occlusion queries: `VkQueryPool`, `vkCmdCopyQueryPoolResults` into a
persistent buffer, deferral when no render pass is open, segment splitting at
pass boundaries, `VK_EXT_host_query_reset`. **AE has no such file.**

Instead AE (and upstream Xenia) FAKES the result in
`ExecutePacketType3_EVENT_WRITE_ZPD`:

```cpp
samples = samples <= lower_threshold ? upper_threshold : samples - 1;
```

The reported "pixels visible" count simply **sawtooths between 80 and 100**
(`query_occlusion_sample_lower/upper_threshold`), decrementing once per ZPD
event. The cvar help even says "Setting this to 0 means everything is reported
as occluded".

### 32.2 Tested - and it is NOT the draw deficit

Set both thresholds to 1,000,000 so the guest is always told a huge number of
samples passed (i.e. "everything is visible"), config only, no rebuild:

| | draws/frame | vista |
|---|---|---|
| default (80/100 sawtooth) | 88 | inverted |
| forced "all visible" (1e6) | **94** | **inverted, unchanged** |

No meaningful change in draw volume and no visual change
(`scratchpad/occl_high.png`). **The fake occlusion counts are not what is
suppressing the draws.**

### 32.3 Still worth fixing, separately

The missing occlusion query implementation is a genuine feature gap against
XenDroid and will matter for titles that gate visible effects on query results
(lens flares, god rays, LOD). It is just not the cause of Halo 3's missing
draws. **Do not re-test it for the vista.**

The s31 conclusion stands: the draws are absent from the ring buffer, and the
cause is upstream of the GPU - JIT, kernel or timing.

## 33. ⭐ The whole command stream is 3x thinner - and AE is SLOWER doing less

`debug.canary.pm4total` counts every PM4 packet parsed, alongside PM4DRAW.
Halo 3 menu, same device, symmetric probes in both builds:

| | total pkts/frame | draws/frame | draws as % of stream |
|---|---|---|---|
| **Canary AE** | 5,251 | 88 | **1.69%** |
| **XDtester** | 15,692 | 611 | **3.89%** |

Raw: AE 4,374,528 total / 73,728 draws / 833 frames.
XDtester 18,956,288 total / 738,304 draws / 1,208 frames.

Two distinct effects, both real:
1. **The stream is 3x thinner overall** - the guest is issuing far fewer
   commands of every kind, not just draws.
2. **Draws are half the share of what remains** (1.69% vs 3.89%) - so on top of
   doing less overall, proportionally less of it is drawing.

### 33.1 The damning part

**Canary AE runs at 15 FPS while processing a THIRD of the command work that
XenDroid processes at 24 FPS.** Per unit of guest work, AE is roughly 5x slower.

That inverts the usual reading. AE is not slow because it is drawing more; it is
slow while doing dramatically less. The bottleneck is on the **CPU/guest side**,
not the renderer - which is exactly what sections 14-32 concluded by
elimination, now with a positive number attached.

### 33.2 The likely shape of it

Halo 3 scales detail dynamically. A guest that is starved of CPU time will be
told, by its own timing code, that it must cut work - fewer objects, lower LOD,
fewer passes. That produces precisely this signature: a uniformly thinner
command stream with proportionally fewer draws, varying run to run (88 vs 138
draws/frame across runs, s30/s31), against a stable reference.

It also ties back to the recorded JIT hotspot `guest_826DEFD0` - 14% of ALL
process CPU in a guest polling loop ([[project-xenia-ae-jit-hotspot]]). A guest
burning its frame budget in a spin loop is a guest that will cut detail.

⚠️ This does NOT yet explain the vista being *inverted* - a thinner scene is not
a mirrored one. Treat "fewer draws" and "inverted" as possibly separate defects
until one is shown to cause the other.

### 33.3 Next

CPU-side performance and correctness, in this order:
1. Attack `guest_826DEFD0` - XenDroid has four JIT passes aimed at spin/poll
   loops ([[project-xendroid-comparison]]) and AE has none of them ported.
   `a64_park_spin_backoff` exists in XenDroid's cvar list.
2. Re-measure PM4TOTAL after each - if the stream thickens as CPU time frees up,
   the dynamic-detail theory is confirmed and the vista may follow.

## 34. JIT spin/poll pass transplant - REGRESSED, defaulted OFF

Following s33 (AE is ~5x slower per unit of guest work), ported the JIT passes
XenDroid has and AE lacks. Diff of `cpu/compiler/passes/`:

| pass | present |
|---|---|
| `memory_poll_park_pass` | XD only |
| `delay_countdown_collapse_pass` | XD only |
| `preempt_check_injection_pass` | XD only |

(AE already had `SpinLoopBackoffPass`, ported earlier, gated off by
`collapse_ctr_spin_loops`, and `a64_park_spin_backoff` which is ON.)

**`PreemptCheckInjectionPass` was NOT ported.** It needs a whole preemption
subsystem AE lacks: HIR `OPCODE_CHECK_PREEMPT`, `HIRBuilder::CheckPreempt`, an
a64 `EmitPreemptCheck` depending on `PPCContext::preempt_requested`,
`PPCContext::last_safepoint_pc`, and a `preempt_yield_handler` scheduler hook.
The other two passes only *tolerate* that opcode during analysis, so those
references were removed.

**Result with the two passes ON (XenDroid's default): SEVERE REGRESSION.**

| | PM4 packets | FPS | state |
|---|---|---|---|
| passes ON | **~8,192** | **1.6** | stuck on the legal screen |
| passes OFF | ~1.6-4.4M | ~10-15 | normal, 84 draws/frame |

The guest essentially stopped executing. Both are now **default OFF** in AE with
the reason recorded in-file.

### 34.1 Why this probably happened

XenDroid's passes park/collapse guest loops on the assumption that a preemption
safepoint exists to get the thread going again. Without
`PreemptCheckInjectionPass` injecting those safepoints, a parked loop has
nothing to wake it - so the guest wedges. **Porting these two without the
preemption subsystem is not a valid transplant.**

### 34.2 If this is retried

Port the whole preemption subsystem first, in this order:
1. `OPCODE_CHECK_PREEMPT` (opcodes.h + opcodes.inl) and
   `HIRBuilder::CheckPreempt`.
2. `PPCContext::preempt_requested` / `last_safepoint_pc` fields and the
   `preempt_yield_handler` hook.
3. `A64Emitter::EmitPreemptCheck` (36 lines) and the a64 `CHECK_PREEMPT`
   sequence.
4. `PreemptCheckInjectionPass`.
5. Only then re-enable the two ported passes.

Do NOT enable `park_memory_poll_loops` or `collapse_memory_delay_spins` before
step 4 is done.

## 35. ⭐⭐⭐ ROOT ARCHITECTURAL DIVERGENCE: cooperative fiber scheduler

Tracing why s34's JIT passes wedged the guest led to the real difference.

The park/collapse passes need a **preemption safepoint** to restart a parked
loop. That safepoint (`OPCODE_CHECK_PREEMPT` -> `EmitPreemptCheck` -> tests
`PPCContext::preempt_requested`) exists to serve something bigger: the flag is
written by **`kernel/guest_scheduler.cc`**.

| | Canary AE | XDtester |
|---|---|---|
| `guest_scheduler.{cc,h}` | **absent** | 1,626 + 360 lines |
| `guest_scheduler` references | **0** | 61 |
| `fiber` references (kernel) | 7 | 173 |
| guest thread model | **1:1 host threads** (`xe::threading::Thread::Create`) | **cooperative fibers** |

And it is ON by default in XenDroid's shipped config:
```
guest_scheduler = true            # Run guest threads as cooperative fibers driven by...
guest_scheduler_quantum_us = 1000 # Cooperative-scheduler timeslice in microseconds
fiber_reentry_longjmp = true
```

**XenDroid runs guest threads as cooperative fibers on its own scheduler with a
1 ms quantum. Canary AE runs them as 1:1 host threads at the mercy of the Linux
scheduler.**

### 35.1 This is consistent with every measurement in this document

* **Non-determinism** (s22: the 512x512 scissor path present in one run, absent
  the next; s30/s31: 138 vs 88 draws/frame across runs, against XenDroid's
  stable ~611-641). Host-thread scheduling is nondeterministic by nature; a
  cooperative scheduler with a fixed quantum is not.
* **~5x slower per unit of guest work** (s33) - AE at 15 FPS processing a third
  of XenDroid's command stream at 24 FPS.
* **The guest issuing 4.6-7x fewer draws** (s30/s31) - guest threads that do not
  get scheduled coherently do not finish building frames.
* **Why every GPU-side comparison came back identical** (s14-s23) - the renderer
  was never the problem.

It also fits this project's own history: the one previously-found real bug of
this class was a **thread-start lost-wakeup race that hung every guest thread**
([[project-xenia-ae-thread-start-lost-wakeup-fix]]).

### 35.2 The decision this forces

Porting the cooperative scheduler is not a transplant like the previous four. It
is ~2,000 lines plus deep integration with `XThread`, waits, and the JIT
(safepoints, fiber re-entry via `fiber_reentry_longjmp`). It changes how every
guest thread runs.

**Chain that must land together, in order:**
1. `guest_scheduler.{cc,h}` + `XThread` integration (fibers instead of host
   threads), `guest_scheduler`/`guest_scheduler_quantum_us` cvars.
2. `PPCContext::preempt_requested` / `last_safepoint_pc`;
   `backend::preempt_yield_handler`.
3. HIR `OPCODE_CHECK_PREEMPT` + `HIRBuilder::CheckPreempt`;
   a64 `EmitPreemptCheck` + `CHECK_PREEMPT` sequence.
4. `PreemptCheckInjectionPass`.
5. Only then enable `park_memory_poll_loops` /
   `collapse_memory_delay_spins` (ported, default OFF, s34).

⚠️ Steps 1-4 are a unit. Partial ports wedge the guest - that is exactly what
s34 demonstrated.

### 35.3 Recommended verification if attempted

Re-run the symmetric probes after each step; they are all built and default OFF:
`debug.canary.pm4total`, `pm4draw`, `drawentry`, `bonedistinct`. Success looks
like AE's draws/frame and total packets/frame approaching XenDroid's, and the
run-to-run variance collapsing. **The vista is the cheap visual proxy (s26).**

---

## 36. ⭐⭐⭐ DECISIVE: the flip is at DRAW time — the dump is innocent

**Date 2026-08-15. Branch `canary-aex`. This resolves the section 16.5 vs 23.2
contradiction, in favour of 16.5.**

### 36.1 The instrument

New probe `debug.canary.dump_row_marker` (diagnostic, default OFF), the
companion to `resolve_row_marker` but **one stage earlier**. Built directly into
the SPIR-V that `GetDumpPipeline` generates: right after the dump shader loads
its source texel from the host render target, the colour is replaced with a
ramp keyed on `source_pixel_y`, the row **in the host render target**.

Same ramp convention as the resolve marker so the two readouts compare
directly: **green at row 0, red at row 512**.

Colour targets only, float components only (`!key.is_depth && !source_is_uint`).
Logs `DUMPMARKER injected into dump pipeline (...)` per pipeline so the probe
can report that it fired — it fired 4 times in the run below.

### 36.2 Result: green at the TOP

Measured, not eyeballed — column x=960 of `scratchpad/dump_marker_result.png`:

| screen y | R | G |
|---|---|---|
| 60 | 8 | **25** |
| 330 | 14 | 18 |
| 510 | 17 | 12 |
| 690 | 21 | 7 |
| 960 | **28** | 2 |

Green falls monotonically 25 -> 1 top to bottom; red rises monotonically 8 -> 28.
Green is high at row 0, so **host render target row 0 lands at the TOP of the
screen.**

### 36.3 What this establishes

The whole chain **host RT -> EDRAM dump -> resolve -> texture load -> tiling ->
composite** is orientation-preserving, end to end. Combined with the earlier
resolve-marker result (which cleared everything from the resolve destination
onward), there is now no stage left downstream of the draw that mirrors.

**Therefore the host render target's CONTENT is already mirrored, and the flip
is introduced at DRAW time.** Section 16.5 predicted exactly this; section 36
measures it.

### 36.4 What this means for section 23.2

Section 23.2 concluded from VSCONST that "the guest is NOT producing a mirrored
transform". That conclusion and this measurement cannot both be right. Either
the guest constants do encode the mirror and the VSCONST comparison missed it,
or **the mirror is introduced by our own draw-time Y handling** — the viewport
Y/height computation or the NDC-Y conversion in the SPIR-V shader translator.

Note the previously "cleared" checks in 16.3 covered the **transfer/dump**
shaders' `pixels_to_ndc_y` and `VulkanCommandProcessor::SetViewport`. The guest
**draw** path's NDC-Y conversion lives in `SpirvShaderTranslator`, which is a
different place and has not been given the same treatment.

### 36.5 Next

Compare, for the vista draws specifically and at runtime (values, not source):
`draw_util::GetHostViewportInfo` output (y, height, and its sign) and the
`SpirvShaderTranslator` NDC-Y conversion, AE vs XenDroid. The search space is
now one stage wide, which it has never been before.
