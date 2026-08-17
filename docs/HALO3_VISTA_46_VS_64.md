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

---

## 37. ⭐⭐⭐ IT IS NOT A FLIP — the two builds DRAW DIFFERENT CONTENT

**2026-08-15, same session as section 36. This supersedes the framing the whole
document has used since section 14.**

### 37.1 The oracle, captured in its correct state

`scratchpad/xd_vista_correct.png` — XDtester, Halo 3 main menu, vista **right
side up**: ground at the bottom, sky at the top, wreck and menu overlay all
correct, 16.8 FPS. Same device, same game, same Turnip driver as AEX. Every
comparison below is against this.

### 37.2 NDC-Y is IDENTICAL between the two builds

`debug.canary.ndcy_draw` in both (the probe already existed in both trees, same
name and format). 43 vertex shaders appear in both logs.

* **Every shared shader has the same `flipped` flag and the same
  `ndc_scale_y`.** 40 are `ndc_scale_y=-1, flipped=1`; the same 3 are
  `ndc_scale_y=0.00024414062 (1/4096), flipped=0, extent_y=8192` in both.
* XD logged one extra shader (`0455646B0C370966`), simply having got further.
* One numeric difference in the entire dataset: shader `D020A6D8A05BD5CC` has
  `extent_y=343` in AEX and `extent_y=512` in XD. An extent, not a sign.

So the viewport maths is not the difference, and the claim in `draw_util.cc`
that "ndc_scale[1] ... is the ONLY thing that can flip Y" does not locate the
bug — that value is identical in the build that renders correctly.

### 37.3 XenDroid's chain preserves orientation TOO

The `dump_row_marker` probe of section 36 was ported into XDtester
(`897d2add9` in the xendroid tree) — identical placement, name and ramp.

| column x=960 | AEX R/G | XenDroid R/G |
|---|---|---|
| y=60 (top) | 8 / **25** | 9 / **26** |
| y=510 | 17 / 12 | 17 / 13 |
| y=960 (bottom) | **28** / 2 | **26** / 1 |

`scratchpad/xd_dump_marker_result.png`. Identical. **Both builds map host render
target row 0 to the top of the screen.** There is no compensating flip in
XenDroid to port.

### 37.4 What that leaves — and why "flip" is the wrong word

Both chains preserve orientation. Both NDC-Y regimes are identical. The guest
constants matched back in 23.2. Yet one build's vista is correct and the other's
is inverted.

**Therefore the host render target CONTENT differs. The two builds are not
drawing the same thing.** Nothing about the transform or the presentation is
wrong in AE; the pixels going in are different.

### 37.5 The hypothesis this points at

Halo 3's menu vista sits over water, and a **water-reflection pass is
legitimately rendered upside down**. If AE composites the reflection render
target because the real scene draws were never submitted, the result is exactly
what is observed: an otherwise-correct scene, inverted, deterministic, with the
2D UI unaffected.

This would unify the vista with the ball for the first time. Section 30 measured
that the guest issues **4.6-7x fewer draws** in AE (88-138 vs 611-641) and that
the draws are missing from the ring buffer rather than dropped by us. A vista
composed from the wrong render target because the later passes never ran is the
same defect seen from the other end — and it matches the long-standing intuition
that "if the vista is upside down, the character is a ball".

⚠️ It also means **no amount of Y-sign work can fix this**, which is consistent
with every Y-sign hypothesis in sections 14-23 having failed.

### 37.6 Next

Identify which render target the vista is composited from, in each build, and
compare: `debug.canary.vista_rt_base` (already built,
`vulkan_render_target_cache.cc:6491`) and `debug.canary.halo3_vista_probe`
(`vulkan_command_processor.cc:2643`). If AE's base differs from XD's, that names
the wrong-RT directly. If they match, count the draws targeting that RT in each.

---

## 38. The scheduler is NOT the vista fix, and neither is UMA readback

**2026-08-15, continuing sections 36-37.**

### 38.1 ⭐⭐ XenDroid renders the vista CORRECTLY with guest_scheduler=false

Set `guest_scheduler = false` in XDtester and gave it real time. It walks the
Bungie intro at 32.6 FPS, reaches the main menu, and renders the vista **right
side up at 16.9 FPS** — a frame rate comparable to Canary AE's ~15.

⚠️ **RETRACTION.** An earlier run the same day was scored as "XenDroid also gets
stuck at the title screen, so the scheduler is load-bearing". That was giving up
after ~90 s. It was slow, never stuck. **"Stuck" and "slow" look identical if
you stop watching too early** — and screenshots of equal file size over a short
window are not evidence of a hang.

**Consequence: finishing AEX's cooperative-scheduler port will NOT un-flip the
vista.** The vista is a defect in non-scheduler code, and XenDroid's correct
rendering without the scheduler means the difference is portable GPU/emulation
code, not the threading model. (This says nothing either way about the ball.)

### 38.2 Config diff: only FOUR real divergences

Applying the technique that solved NFS Carbon — diff the config against a fork
that works, before suspecting code. Shared cvars, values compared:

| cvar | AEX | XDtester |
|---|---|---|
| `collapse_ctr_spin_loops` | false | true |
| `collapse_memory_delay_spins` | false | true |
| `park_memory_poll_loops` | false | true |
| `readback_resolve` | **none** | **uma** |

The first three are the JIT spin passes deliberately defaulted OFF here (they
regress without preemption, section 34). That left one.

### 38.3 readback_resolve=uma tested — NOT the fix

`ReadbackResolveMode::kUma` is already ported and was recorded as the mechanism
behind XenDroid's ball fix, but AEX's config had it at `none`. Set it to `uma`
plus `debug.canary.shared_memory_host_visible=1` (it silently falls back to
kFast without the host-mapped buffer).

**Result: the image changed substantially but is still wrong** — the vista
becomes a close-up rocky texture rather than the landscape, and the frame rate
drops to ~5.5-7.9 FPS. Reverted to `none`.

So the vista is not explained by any remaining config divergence.

### 38.4 Where that leaves it

Everything GPU-side has now been measured identical to the build that renders
correctly: the dump/resolve/composite chain (36, 37.3), the NDC-Y regime per
shader (37.2), the render target path, and now the configuration (38.2). The
scene is the same scene, vertically mirrored, with the 2D UI upright.

The untested axis is the **CPU side**. `docs/` records that the desktop RADV
oracle - the same AE lineage built for x86 - renders Halo 3 cleanly. If that
includes an upright vista, then the same emulator code produces a correct camera
on an x86 JIT and a mirrored one on the a64 JIT, which would place the mirror in
guest-side float maths (a camera up-vector built from sin/cos, for instance)
rather than anywhere in the GPU backend.

**Verify that first** - it is a local, offline check and it either opens the CPU
axis or closes it.

---

## 39. ⭐⭐⭐⭐ MEASURED: THE GUEST COMPUTES A MIRRORED CAMERA. IT IS A CPU BUG.

**2026-08-15. This overturns section 23.2 and moves the vista out of the GPU
backend entirely.**

### 39.1 Closing 23.2's hole

Section 23.2 concluded from VSCONST that "the guest is NOT producing a mirrored
transform". That probe **dedupes on the vertex-shader hash alone**, so it logs
each shader exactly once - whichever constants happened to be live on that
shader's first ever draw. Every later draw with different constants was never
sampled. The conclusion was never actually tested.

New probe `debug.canary.vsconst_states` (in **both** trees, identical name and
format) dedupes on **(shader hash, hash of c0..c7)**, so it captures every
distinct transform a shader draws with. 256-entry table with an overflow guard -
without the guard an unmatched key finds no free slot either and every draw
logs, which produced 583k lines on the first attempt.

### 39.2 The result

Both builds at the Halo 3 main menu, same device, same driver. Taking c3.x - a
distinctive camera basis component with magnitude ~0.995:

| build | samples | c3.x POSITIVE | c3.x NEGATIVE | mean abs |
|---|---|---|---|---|
| **XDtester** | 254 | **254** | **0** | 0.994675 |
| **Canary AEX** | 255 | 34 | **221** | 0.995737 |

Per-shader, on shaders present in both, the pattern is a **sign flip at matching
magnitude** rather than a different value. For `488D9488AB7ED7D8`:

```
c0  AEX=( 0.0914172, -1.40483,  0.0777927, ...)
c0  XD =(-0.0910121, -1.40483,  0.0782452, ...)
c1  AEX=(0.227969, -0.123224, -2.49314, ...)
c1  XD =(0.227827, +0.1241,    +2.49311, ...)
c3  AEX=(-0.993814, -0.0684036, -0.0874919, ...)
c3  XD =(+0.993838, -0.0681431, -0.0874273, ...)
```

Magnitudes agree to 4-5 significant figures while several components carry
opposite signs. **A panning camera would change magnitudes; this keeps them and
flips signs.** That is a mirrored basis, not a sampling artifact.

### 39.3 What it means

**The guest itself computes a mirrored camera.** The mirror is present in the
constants the PowerPC code uploads, before a single GPU stage runs - which is
exactly why sections 14-38 found every GPU stage identical to the build that
renders correctly. The vista is a **CPU-side emulation defect**, not a render
bug.

This retires the entire Y-sign search space in the GPU backend, and explains why
~25 hypotheses there all failed.

### 39.4 Not yet found

A first diff of the a64 backend (`a64_sequences.cc`, `a64_seq_vector.cc`)
against XenDroid shows only cosmetic differences - brace style, a scratch
register choice, comment wording - and identical `fnmsub`/`fnmadd` handling. So
the divergence is not a one-line opcode sign error in the obvious place.

Candidates, in order:
1. **XenDroid's four JIT optimisation passes that AE lacks**
   ([[project-xendroid-comparison]]) - a miscompile in an AE pass would produce
   exactly this: correct magnitudes, wrong signs.
2. FPSCR / rounding-mode or denormal handling around the camera maths.
3. Kernel-side float helpers.

### 39.5 How to reproduce the measurement

```
setprop debug.canary.vsconst_states 1     # both builds
# AEX  library tile (488,525), two taps ~3s apart after ~12s
# XDtester tile (494,360), same
grep -a VSCONSTSTATE <log>                # 257 lines incl. the "table full" line
```
Then compare the sign of c3.x across states. Fast, and it discriminates in one
run per build.

### 39.6 CPU-side bisection - what is ELIMINATED (2026-08-15)

All measured against the c3.x sign histogram (baseline **34 positive / 221
negative**; XenDroid **254 / 0**), which is a far sharper instrument than
looking at the screen:

| tried | result |
|---|---|
| `disable_context_promotion = true` | vista unchanged. (AE already *excludes* VEC128 from promotion; XenDroid is the one that ADDED it.) |
| `debug.canary.saverest_fast = 0` (AE-only opt, "set 0 to bisect") | histogram **bit-identical** 34/221 |
| `dcbz` 32 -> 128 bytes (real AE regression, fixed in `3d461e5e5`) | histogram **bit-identical** 34/221 |
| `readback_resolve = uma` + host-visible shared memory | image changes, still wrong, 5.5-7.9 FPS. Reverted |
| AltiVec `vmsum*` / `vsum*` unimplemented in AE, implemented in XenDroid | real gap, but **not executed at the menu** - zero `Unimplemented instruction:` lines in the log |
| `ppc_emit_fpu.cc`, `ppc_emit_alu.cc` vs XenDroid | **byte-identical** |
| `simplification_pass.cc`, `a64_seq_vector.cc` sign-relevant paths | differences are **cosmetic only** (brace style) |
| AE vs upstream canary in `cpu/ppc` + `cpu/compiler` | small; only `dcbz` was substantive, and it is eliminated above |

**Methodological note that saved time:** compare the *histogram*, not the
screenshot. The menu camera pans, so two screenshots of the same build at
different moments look quite different; the sign statistic does not move.

### 39.7 Still open

XenDroid's own CPU additions (`inline_leaf_calls`, VEC128 context promotion,
the range-keyed validity tracking) are **optimisations XenDroid ADDED**, so AE
lacking them cannot explain AE computing a *wrong* value - AE is the
conservative side. That inverts the search: the defect is more likely something
**AE does differently**, not something it is missing.

Unexamined AE-vs-XenDroid surface, by size: `a64_seq_memory.cc` (456),
`a64_backend.cc` (245), `a64_sequences.cc` (222), `a64_emitter.cc` (213),
`a64_seq_util.h` (84), `ppc_translator.cc` (119), `processor.cc` (77).
The a64 **memory** sequences are the most promising of these: guest vector
loads/stores are byte-order sensitive, and a lane or endianness error there
produces exactly this signature - right magnitudes, wrong signs.

### 39.8 Second bisection round - also eliminated

| tried | result |
|---|---|
| `validate_hir = true` (runs ValidationPass between every pass) | **no validation errors** - no pass emits malformed HIR |
| `MemorySequenceCombinationPass` vs XenDroid | **byte-identical** |
| `PPCContext` layout vs upstream | AE's scheduler fields are correctly **append-only**; no offset shift |
| `xboxkrnl_memory.cc` memory reported to the guest | identical (512 MB / `total_physical_pages = 0x20000`) |
| `xboxkrnl_video.cc` display mode reported | both report **1280x720 mode 8** (already verified 2026-08-12) |
| Pass **ORDER**: AE runs `PreemptCheckInjectionPass` 1st, XenDroid 3rd (after CFA) | real divergence, but the pass **returns early when `guest_scheduler` is off**, and the vista is broken with it off. Worth fixing for when the scheduler is on; not this bug. |

### 39.9 Sharper read of the constants

Comparing distinct c0-c3 states rather than sign counts:

* Both builds share **3 identical** "identity" states - `c3=(1,0,0,0)`,
  `c0.x=-0.00156`, `c1.z=+2.57067 / +2.50657`.
* The **camera** states diverge:

| | c3.x | c1.z | c3.w |
|---|---|---|---|
| XDtester | **+0.994** | **+2.4928** | +9.415 … +9.630 |
| Canary AEX | **-0.9976** | **-2.4813** | +4.386 … +7.346 |

`c3.x` and `c1.z` flip **together**, and the remaining components differ in
magnitude too - so it is a camera rotated ~180 degrees, not a clean negation of
one matrix. **`c1.z` ~ ±2.5 is a projection Y-scale, and a negative projection
Y-scale IS the vertical flip.**

Critically: **AEX never once produces XenDroid's camera values** (0 of AEX's
positive states appear in XenDroid's set). That rules out staleness/ordering -
if the draws were merely observing an old state, AEX would still produce the
correct values at some point. **AEX's guest computes a different projection.**

### 39.10 The shape of the remaining question

This now matches a divergence the document already recorded independently:
Halo 3 picks a **368x368 shadow cascade** under AE where XenDroid gets
**512x512** (section 39.8's video note, and the `extent_y` 343-vs-512
difference in section 37.2). Both are **guest decisions**, made from something
the emulator reports.

So the question is no longer "which instruction is miscompiled" - the HIR
validates, the FPU/ALU emitters are byte-identical, and every pass and context
layout checks out. It is **"what does the guest ask us that we answer
differently from XenDroid?"**

**▶️ NEXT: diff the kernel-call stream.** Enable `log_all_kernel_calls` in both
builds (it is gated behind `LogLevel::Debug`, so `log_level` must be raised
above the shipped 2 - that gating is why an earlier attempt produced nothing),
reach the menu in each, and diff the sequence of calls and return values. That
directly finds the input the guest is deciding on, and it should explain the
projection sign and the 368-vs-512 cascade together.

---

## 40. ⭐⭐⭐⭐⭐ MINIMAL REPRO: the FIRST camera state, same shader, negated

**2026-08-15. This is the tightest the bug has ever been pinned.**

### 40.1 The two builds run in LOCKSTEP, then diverge at one exact point

`debug.canary.vsconst_states` output ordered by `n` (the probe's global state
counter), AEX vs XDtester at the Halo 3 main menu:

* **n=1 through n=32: bit-identical in both builds** - same shaders, same order,
  same constants. Every one is an identity/2D transform
  (`c3=(1,0,0,0)`, `c1.z=+2.50657`).
* **n=33: XenDroid switches to the real 3D camera.** AEX instead emits three
  MORE identity-transform states (shaders `C2543FD5CD52420B`,
  `C5E0199746AB8E83`, `9EA48FC2B26C325D` - note these are the same three that
  `ndcy_draw` reports as `flipped=0, extent_y=8192`).
* **Both builds' first real camera state comes from the SAME vertex shader,
  `E05650CA89E232AF`:**

| | n | c1.z (projection Y-scale) | c3.x |
|---|---|---|---|
| XDtester | 33 | **+2.49331** | **+0.99365** |
| Canary AEX | 36 | **-2.49313** | **-0.99383** |

Magnitudes agree to four decimals - AEX's arrives three states later, so a small
animation-phase difference is expected. **Both components are negated.**

### 40.2 What negating both means

Negating the X basis and the Y (projection) scale together is a **180 degree
roll about the view axis**. On this landscape that reads as "upside down", which
is exactly the reported symptom, and it leaves the separately-drawn 2D UI
untouched.

### 40.3 Why this is the useful form of the bug

Everything before the divergence is bit-identical, so the guest executed
identically up to that point. The divergence is not gradual drift and not a
global sign error - it appears **at the first draw that carries a real camera**,
in one named shader, at a known state index.

That gives, for the first time, a **minimal reproduction with a fixed address in
the trace**: run either build to the menu with `debug.canary.vsconst_states=1`,
look at the first state whose `|c3.x|` is a camera rather than 1.0, and read two
numbers.

### 40.4 Also eliminated this round

| tried | result |
|---|---|
| Kernel-call stream diff (`log_all_kernel_calls`, `log_level=3`, both builds; 583k lines AEX / 5.07M XD) | **call sets match** - nothing called in one and not the other except thread-name noise |
| `VdSetDisplayMode`, `VdQueryVideoMode`, `XGetVideoMode`, `VdGetCurrentDisplayInformation`, `VdGetCurrentDisplayGamma`, `VdInitializeEngines`, `VdIsHSIOTrainingSucceeded`, `VdPersistDisplay` | **identical arguments in both builds** |
| `VdQueryVideoFlags` implementation | **byte-identical** source in both |
| Physical-memory allocation failures | **none** in the AEX log |

So the guest is told the same things and asks the same questions. The divergence
is in what it *computes* at that one transition.

### 40.5 Next

The three extra identity draws AEX emits at n=33-35, immediately before its
camera appears, are the strongest remaining thread: they are the same shaders
`ndcy_draw` flags as `extent_y=8192, flipped=0`, and XenDroid does not draw them
at that point. Determine what those three draws are (a pre-pass? a clear?) and
why AEX issues them first - the camera negation appears on the very next state.

### 40.6 Third bisection round - two more real bugs fixed, vista unchanged

| tried | histogram | vista |
|---|---|---|
| `dcbz` 32 -> 128 bytes (`3d461e5e5`) | **34/221, bit-identical** | unchanged |
| `saverest_fast = 0` | **34/221, bit-identical** | unchanged |
| EVENT_WRITE_ZPD addressing + A-only sentinel (`5e3d6cef4`) | **34/221, bit-identical** | unchanged |

⭐ **That three independent, genuine CPU/GPU-side fixes move the histogram by
exactly zero is itself a strong signal:** the camera constants are completely
insensitive to the dcbz semantics, the save/restore helper path, and the
occlusion-query writeback. Whatever produces them is not reached through any of
those.

### 40.7 Honest assessment of the approach

Bisecting emulator subsystems has now produced three real bug fixes and zero
movement on the vista across roughly a dozen device runs. The technique has
stopped paying: each round costs a build plus a 2-3 minute run and returns the
same 34/221.

**The next technique should be guest-level, not emulator-level.** The divergence
is at a known point (section 40.1: first camera state, shader
`E05650CA89E232AF`, state index ~33-36, everything before bit-identical). The
question is what PowerPC code writes those constants and what it read to compute
them. Options, cheapest first:

1. **Trace the guest writes to the constant memory.** The values reach the GPU
   as SET_CONSTANT/SET_SHADER_CONSTANTS PM4 payloads assembled by D3D from a
   guest matrix. Find the guest address that matrix lives at, set a write watch
   on it, and capture the guest LR of the writer in both builds. That names the
   guest function, and the two builds can then be compared instruction by
   instruction at a known call site.
2. **Use the desktop oracle as a third data point.** `xenia_canary` at
   `/home/roman/xeniatest/oracle` is upstream on x86. Running the same probe
   there says whether upstream-on-x86 produces the positive or negative camera,
   which separates "AE fork regression" from "a64 backend" - a distinction none
   of the tests so far can make.

Option 2 is cheap and should be done first; it is a local, offline run.

---

## 41. XDtester cvar bisection - harness and results

**2026-08-15.** Per the "XDtester is the instrument" approach: flip XenDroid's
cvars toward Canary AE's behaviour until XenDroid's vista BREAKS. Whatever group
breaks it names the code responsible.

### 41.1 The harness (`scripts/vista_bisect/`)

`xd_bisect.sh <tag> "cvar=value" ...` restores XDtester's config from a pristine
snapshot, applies the group, launches Halo 3, and captures both a screenshot and
the `vsconst_states` log. `verdict.py` reads the verdict off the **c3.x sign
histogram** rather than the screenshot, because the menu camera pans and two
screenshots of one build at different moments look different while the statistic
does not move:

* POSITIVE dominant -> vista still CORRECT (group is not the cause)
* NEGATIVE dominant -> vista BROKE (cause is in this group)
* NO CAMERA STATES  -> run failed; re-run or split

**Two harness traps, both of which produced false "run failed" verdicts before
being fixed:**
1. **String cvars are quoted in the config.** Writing `occlusion_query = fake`
   bare breaks the TOML parse, the config silently fails to load and the game
   never starts. The script now quotes anything that is not a bool or number.
2. **A missed tile tap leaves you on the library screen**, which also scores as
   "run failed". The script now retries the two-tap launch up to four times and
   confirms the game is actually rendering (VSCONSTSTATE appearing) before
   trusting the run.

### 41.2 Results so far - all NEGATIVE (vista stayed correct)

| group | cvars flipped | verdict |
|---|---|---|
| A - CPU/vector semantics | `a64_vmx_nan_fixup`, `a64_native_reserved_ops`, `context_promote_vec128`, `inline_leaf_calls`, `inline_gprlr_saverest`, `precise_guest_delays`, `precise_interpolation` | **CORRECT** 215/0 |
| B1 - guest-visible readback | `occlusion_query=fake`, `readback_resolve=none` | **CORRECT** 223/0 |
| memexport | `memexport_enable=true`, `memexport_await_fences=true` | **CORRECT** 215/0 |
| C - shared memory coherency | `shared_memory_zero_copy`, `tiled_shared_memory`, `vulkan_shared_memory_host_visible` | **CORRECT** 223/0 |

⭐ Group A matters most: **`a64_vmx_nan_fixup` is a XenDroid-only PPC NaN
propagation fixup on VMX float ops that AE has no equivalent of** - the single
most promising CPU-side candidate found in the whole investigation - and turning
it off does not break XenDroid's vista. Same for `context_promote_vec128`, the
VEC128 promotion AE excludes.

B1 also retroactively confirms the EVENT_WRITE_ZPD fix (`5e3d6cef4`) was never
going to fix the vista: XenDroid on the fake ZPD path still renders it correctly.

## 42. ⭐ TEST LEDGER - do NOT repeat these

Every vista test run so far, with its verdict. Verdicts on AEX are the c3.x sign
histogram (baseline **34 positive / 221 negative** = BROKE); verdicts on XDtester
are whether the flip could be INDUCED (baseline ~220/0 = CORRECT).

### 42.1 On Canary AEX - tried to FIX it. None worked.

| # | change | result |
|---|---|---|
| 1 | `dcbz` 32 -> 128 bytes (real bug, kept, `3d461e5e5`) | **bit-identical 34/221** |
| 2 | `debug.canary.saverest_fast = 0` | **bit-identical 34/221** |
| 3 | EVENT_WRITE_ZPD addressing + A-only sentinel (real bug, kept, `5e3d6cef4`) | **bit-identical 34/221** |
| 4 | `disable_context_promotion = true` | unchanged |
| 5 | `validate_hir = true` | **no validation errors** (no pass emits bad HIR) |
| 6 | `readback_resolve = uma` + host-visible shared memory | image changes, still wrong, 5.5 FPS. Reverted |
| 7 | `debug.canary.memexport_no_store = 1` (new toggle, kept, default OFF) | **still BROKE 0/221** - suppressing memexport stores does not help |

### 42.2 On XDtester - tried to BREAK it. None worked.

| # | cvars flipped toward AE behaviour | result |
|---|---|---|
| A | `a64_vmx_nan_fixup`, `a64_native_reserved_ops`, `context_promote_vec128`, `inline_leaf_calls`, `inline_gprlr_saverest`, `precise_guest_delays`, `precise_interpolation` | **CORRECT 215/0** |
| B1 | `occlusion_query=fake`, `readback_resolve=none` | **CORRECT 223/0** |
| - | `memexport_enable=true`, `memexport_await_fences=true` | **CORRECT 215/0** |
| C | `shared_memory_zero_copy`, `tiled_shared_memory`, `vulkan_shared_memory_host_visible` | **CORRECT 223/0** |
| ALL | **33 cvars at once** - all gamma/number-format, all resolve/transfer paths, all async-shader, all vulkan pipeline caching, plus the group A CPU set | **CORRECT 220/0** |

⭐⭐ **No XenDroid feature is responsible for their vista being correct**, and no
AE-only cvar is responsible for ours being wrong (the AE-only set -
`gamma_render_target_as_srgb`, `preempt_check_every_block`, `readback_memexport`,
`vfetch_bounds_clamp`, `vulkan_user_clip_planes`, `render_target_path_vulkan` -
is entirely at its default/inert value already). **The cause is un-gated code.**

### 42.3 Eliminated by inspection (no run needed)

Kernel-call stream (call sets match, 583k/5.07M lines) · every `Vd*` and
`XGetVideoMode` argument · `VdQueryVideoFlags` source · reported guest memory ·
reported display mode (1280x720 mode 8) · physical allocation failures (none) ·
`ppc_emit_fpu.cc` and `ppc_emit_alu.cc` (byte-identical to XenDroid) ·
`MemorySequenceCombinationPass` (byte-identical) · `PPCContext` layout
(append-only) · `simplification_pass` / `a64_seq_vector` sign paths (cosmetic) ·
compiler pass ORDER (AE runs PreemptCheckInjection 1st vs XD 3rd, but it no-ops
with `guest_scheduler` off, which is the failing configuration).

### 42.4 New fact from this round

The three shaders AEX draws immediately before its negated camera
(`C2543FD5CD52420B`, `C5E0199746AB8E83`, `9EA48FC2B26C325D`) exist in BOTH
builds in the same relative order, but at very different points:

| | first n |
|---|---|
| AEX | **33, 34, 35** - before the camera |
| XDtester | **66, 67, 68** - after the camera |

So AEX pulls that pass ~33 states earlier. This is a **draw-ordering
divergence**, and the camera that follows it in AEX is the negated one.

---

## 43. ⭐⭐⭐⭐⭐ SETTLED: it is a COMPUTATION bug, not an ordering bug

**2026-08-16.** New probe `debug.canary.camwrite` (default OFF) logs every value
the guest WRITES into the camera constant slot (`SHADER_CONSTANT_000_X + 12`,
i.e. c3.x), as opposed to what a draw later OBSERVES there.

### 43.1 Why this was needed

`vsconst_states` samples at DRAW time, so it could never separate:
* **(a) computation** - the guest genuinely computes a mirrored camera, versus
* **(b) ordering** - the guest computes it correctly but our draws observe the
  wrong constant state, which is exactly the mechanism that collapses the bone
  matrices (`debug.canary.bonedistinct`, section 29).

Every round of this investigation has been ambiguous between those two.

### 43.2 Result

At the Halo 3 main menu, one AEX run:

```
CAMWRITE camera-magnitude writes:  POSITIVE = 0   NEGATIVE = 235489
```

**The guest never writes the correct positive camera. Not once, in a quarter of
a million writes.**

### 43.3 What this settles

* **(b) ORDERING IS RULED OUT for the vista.** No amount of fixing draw/constant
  interleaving can help - the correct value is never produced to begin with.
  (The ordering mechanism remains valid for the BALL, which is a separate
  measurement - section 29.)
* **(a) COMPUTATION is confirmed.** The PowerPC guest, running under Canary AE,
  computes a mirrored camera.

Combined with the ledger in section 42, the guest does this while:
kernel answers are identical, reported memory and display mode are identical,
`LOAD_ALU_CONSTANT`/`SET_CONSTANT` are identical to upstream, `ppc_emit_fpu.cc`
and `ppc_emit_alu.cc` are byte-identical to XenDroid, the HIR validates clean,
and no cvar in either build changes the outcome.

### 43.4 Next - locate the guest code

The remaining task is to find the PowerPC function that computes it. The write
side is on the GPU thread (parsing the ring buffer), so it carries no guest LR;
the ring buffer was filled by the guest CPU earlier. Route:

1. **Find the matrix in guest RAM.** The value written is a distinctive float
   (approximately -0.99383). Scan guest memory for that bit pattern to get the
   address D3D copies from.
2. **Watch that address** for writes and capture the writing guest LR. That names
   the PPC function.
3. Compare that function's execution between the two builds - by then it is a
   single named function, not a subsystem.

### 43.5 The difference is EXACTLY the sign bit

Raw values the guest writes into c3.x, from `debug.canary.camwrite`:

| | raw | value |
|---|---|---|
| Canary AEX | `0xBF7E5FB4` | -0.993648 |
| XDtester | `0x3F7E5FB4` (from its +0.99365) | +0.993650 |

**Identical mantissa and exponent; only bit 31 differs.** The magnitude is
computed the same way in both builds and then one of them has the sign inverted.
That rules out a different formula, a different input, a precision difference and
a byte-swap error (a swap would scramble all four bytes, not one bit).

Checked and clean: no differences in any sign-capable AltiVec op (`vxor`,
`vsubfp`, `vaddfp`, `vmaddfp`, `vnmsubfp`, `vspltisw`, `vsel`, `vrefp`,
`vrsqrtefp`, `vnor`, `vandc`) between AE and either XenDroid or upstream, and
`ppc_emit_fpu.cc`/`ppc_emit_alu.cc` are byte-identical to XenDroid.

### 43.6 ⚠️ Failed approach - do not repeat as written

Tried locating the matrix by scanning guest RAM for the bit pattern, driven from
the `camwrite` hook. **It hangs the command-processor thread**: the scan walks
512 MB from physical 0, which crosses unmapped guest pages, and the run produced
one `CAMWRITE` line where the same build had produced 415k without it. Reverted.

If retried, it must (a) run off the command-processor thread, and (b) scan only
regions known to be mapped rather than a flat 0..512 MB sweep.

## 44. ⭐⭐⭐⭐⭐ FOUND: the writer is guest function `825AD9F0` (2026-08-16)

Avoided the scan in 43.6 entirely. `WriteALURangeFromMem`
(`command_processor.cc`) is the `PM4_LOAD_ALU_CONSTANT` path
(`pm4_command_processor_implement.h:1263`) - the game DMAs shader constants
from a guest RAM location it names in the packet, via
`memory_->TranslatePhysical(address)`. That host pointer is already the exact
guest RAM source; no scan needed, just log it at the point it is already
known.

### 44.1 CAMWRITE now reports the source address

Instrumented all three producer paths (`WriteRegisterRangeFromRing`,
`WriteOneRegisterFromRing`, `WriteRegistersFromMem`) to stash the host pointer
of the dword about to be consumed in a global
(`g_ae_last_reg_write_src_host`), which the existing `CAMWRITE` log
(`command_processor.cc`) reads and converts to a guest physical address
(`src_phys`). Confirms the c3.x constant comes through the from-mem path: the
address drifts through a linear allocator early (menu load), then settles
into a stable **2-buffer alternation** once steady state is reached at the
vista (e.g. `0x0536E7D0` / `0x05375790` in one run) - a double-buffered
per-frame constant upload arena, exactly as expected.

### 44.2 A write-watch that survives the 43.6 trap

`Memory::EnableCamwatchDiag(physical_address)` (`memory.cc`) arms Xenia's
existing physical-memory write-watch (`EnablePhysicalMemoryAccessCallbacks` +
`RegisterPhysicalMemoryInvalidationCallback` - the same mechanism
`SharedMemory`/texture cache use) on the page containing `src_phys`, called
once per `CAMWRITE` so it re-arms as the target rotates between buffers. This
sidesteps 43.6's hang completely: no scan, just watching an address already
known.

⚠️ **First cut flooded**: `RegisterPhysicalMemoryInvalidationCallback` is
global - every registered callback fires for every page ANY subsystem
invalidates, not just the one you armed. Produced 7700+ hits sweeping
unrelated 256 KB-strided ranges within two seconds. Fixed by tracking the (at
most two) pages actually armed and dropping anything else before it counts as
a hit (`g_ae_camwatch_pages` in `memory.cc`).

To read the guest link register at the fault: `HostThreadContext` (captured
in `mmio_handler.cc`'s `ExceptionCallback`, in the same branch that already
calls `access_violation_callback_` under `global_critical_region` locked
once) exposes `x[20]`, the PPCContext pointer per this session's earlier
finding ("Context register = x20"). `PPCContext::lr` is at struct offset
0x10; read raw via `memcpy` rather than including `ppc_context.h` into
`memory.cc`/`mmio_handler.cc`.

### 44.3 Result: `guest_lr` is 0, but `host_pc` is fully deterministic

Every hit across every run: `guest_lr=0x00000000`. But `host_pc`/`host_lr`
(the ARM64 register values, not the guest ones) are **perfectly consistent
across 12 hits in a row within a run**, and the fault address
(`aa0000000-ab0000000`, the `/dev/ashmem/xenia_code_cache_*` mapping) is
**stable across separate app launches** - e.g. `0xAA10E9FA8` appeared
identically in two independent runs before a third run's fresh JIT
compilation order produced a different but *equally self-consistent*
`0xAA10B7808`.

`guest_lr=0` is not a dead end - it means this write happens from a guest
thread that has not yet executed a `bl` since PPCContext was created (fits: a
freshly-spawned worker thread whose first action is populating initial
constant buffers), not that the fault is somehow outside JIT'd code.

### 44.4 Resolved via the existing JIT perf-map probe

`debug.canary.perf_map=1` (see `ae_perf_map.h`, already built for the JIT
hotspot work) writes `guest_<addr>_<name>` symbols keyed by the exact
absolute host code-cache address to
`/sdcard/Android/data/<pkg>/files/xeniaae/perf-<pid>.map` at JIT compile
time - the same absolute addresses `host_pc`/`host_lr` are already in.
Looked the captured addresses up directly (no `simpleperf` needed, that's
only for sampling profiles):

```
0xaa10b7808 -> guest_825AD9F0   [0xaa10b7360 - 0xaa10b7984]
0xaa10b73ec -> guest_825AD9F0   [same function]
```

**All 12 consecutive hits, across every buffer instance in the early linear-
allocator progression, resolve to the SAME guest function: `825AD9F0`.** One
routine, called repeatedly, writes the camera constant into its shadow
buffer slot every frame.

Three other addresses on the same physical page (shared with other
constants - bone matrices etc., not camera-specific) resolved to
`guest_82177870`, `guest_8258E090`, and `guest_8216E020` - unsurprising,
useful only as confirmation the mechanism resolves correctly.

### 44.5 What this does and does NOT mean

`825AD9F0` is a location in Halo 3's OWN compiled PowerPC code - the same
bytes run under AEX and XenDroid (same XEX). So the divergence is **not**
"this function differs between builds" (already ruled out at the source
level: `ppc_emit_fpu.cc`/`ppc_emit_alu.cc` byte-identical, §39.6/39.8). It
must be either (a) an INPUT to this function that differs - a register or an
earlier guest-memory value this store depends on, produced by code we
haven't located yet, or (b) a JIT MISCOMPILATION specific to whichever
opcodes make up this one function, where our translator and XenDroid's
diverge for this exact instruction sequence despite the emitter source files
matching (a plausible mechanism the earlier byte-identical-source checks
could not rule out, since they compared the emitter's C++, not its output for
this specific function).

### 44.6 ▶️ NEXT

Dump the ARM64 machine code our JIT emitted for guest `825AD9F0` (host range
`0xaa10b7360`-`0xaa10b7984` in this run; re-resolve via a fresh perf map each
time, the code-cache layout is not address-stable across builds) and compare
instruction-by-instruction against XenDroid's JIT output for the same guest
function on the same input. If they match, the input differs and the search
moves one level up the call chain (still lr=0 dead-ends the naive "who
called this" question - would need a stack walk or a second watch on
whatever guest address feeds this function's input register). If they
differ, the JIT backend gap is now scoped to one function's worth of
opcodes instead of "all of ppc_emit_fpu.cc".

### 44.7 Tooling added this round

* `CAMWRITE` now includes `src_phys` (guest physical address of the value's
  source) - `command_processor.cc`.
* `Memory::EnableCamwatchDiag(physical_address)` - one-call arm/re-arm of a
  filtered physical-memory write-watch with guest-LR capture, capped at 12
  hits. `memory.h`/`memory.cc`.
* `cpu::g_ae_camwatch_fault_context` - `HostThreadContext*` captured at the
  fault site, `mmio_handler.h`/`.cc`.
* Probe: `debug.canary.camwatch` (default OFF, pairs with `camwrite` which
  must also be on - camwatch reads camwrite's `src_phys`).

## 45. `guest_825AD9F0` is a mover, not the computer - and where that leaves us

Continuation of section 44, same day. Executed the ▶️ NEXT from 44.6. Net
result: **the vista is still broken.** Three real things were found along the
way; none of them is the bug, but all three close off ground so a future
session doesn't re-walk it.

### 45.1 ⭐⭐⭐⭐⭐ SECOND REAL BUG FOUND AND FIXED: `guest_lr` was reading the wrong PPCContext field

The 44.2 camwatch tool's `guest_lr` read a **hardcoded offset 0x10**, copied
from a comment in `ppc_context.h` (`uint64_t lr; // 0x10`). That comment is
**stale** - the struct was reordered ("most frequently used registers
first") and the comment never re-annotated. Every `guest_lr` value 44
reported was **silently wrong** (always 0, from reading padding/cr-register
bytes, not lr).

Found by disassembling `guest_825AD9F0` (see 45.2) and noticing it wrote a
guest return address to `[x20, #304]` - not `#0x10`. Confirmed by compiling
a standalone probe against the **real** `ppc_context.h` with `offsetof`
instead of trusting a comment a second time:

```
offsetof(PPCContext, lr) = 304
```

Fixed in `memory.cc`'s camwatch callback: reads
`reinterpret_cast<const PPCContext*>(ppc_ctx_ptr)->lr` through the real
struct now, guarded by `static_assert(offsetof(PPCContext, lr) == 304, ...)`
so a future struct change fails the BUILD instead of silently reading
garbage again. ⚠️ **Any other AE-only code that reads PPCContext by a
hardcoded numeric offset should be treated as suspect** - this is exactly
the class of bug a layout change makes silently wrong, and it already
happened once.

With the fix, `guest_lr` reads real, stable values (e.g. `0x825AD9F8`,
`0x8214C5D0`) - see 45.3.

### 45.2 `guest_825AD9F0` disassembled: zero FP/vector instructions

Dumped the JIT'd ARM64 bytes for `guest_825AD9F0` from the live process
(`dd` from `/proc/<pid>/mem` at the perf-map-resolved host range) and
disassembled with `llvm-mc -triple=aarch64 -disassemble` (system
`objdump`/`llvm-objdump` have no raw-binary aarch64 mode on this host;
`llvm-mc` fed a hex byte list does).

**393 instructions, zero of them float or vector** (`fmov`/`fneg`/`fadd`/
`eor`/`scvtf`/etc. all absent - checked explicitly). Heavy on `mov`/`ldr`/
`str`/`cmp`/`strb`/`cset`. This function **moves already-computed 32-bit
words around; it does not compute anything.** Whatever flips the sign runs
somewhere else - `825AD9F0` is downstream of it, a packer/mover only.

The function opens with a fixed prologue that appends a `{tag, r1, lr}`
triple to a ring buffer at `[x19+152 base, x19+172 index]`, incrementing a
counter capped at 65536 - this is Xenia's own generic call-trace
instrumentation (present in every JIT'd function start), not
Halo-3-specific logic, and explains why `x19` and the ring-write pattern
look identical across every guest function sampled.

### 45.3 Exact-byte watching: technically works, practically dead-ended

Extended `EnableCamwatchDiag` to also capture and log the **exact** faulting
host/physical address (`ex->fault_address()` via a new
`cpu::g_ae_camwatch_fault_host_address`), not just the watched page -
because a 4 KB page turned out to hold many unrelated fields. Result across
every test this round: **`exact_match=false`, always** - the byte that
actually faults after arming is never the byte CAMWRITE told us about.

Chased this through three redesigns, each confirmed correct but insufficient:

1. **Self-re-arm from inside the invalidation callback.** Confirmed safe -
   `SharedMemory::MemoryInvalidationCallback` already re-acquires
   `global_critical_region_` from inside itself in production, so doing real
   work here isn't a new risk. Result: thrashed by the EXTERNAL trigger -
   `WriteALURangeFromMem` calls `WriteRegister` in a tight host loop, so the
   external `CAMWRITE` hook re-fires far faster (many times before the next
   guest frame) than a re-armed page could be hit again. 60/60 hits, every
   one a **different** page.
2. **Arm once, chase one page patiently, gated on page-recurrence.** First
   cut used a 16-slot recent-page ring to detect "this page has been seen
   before, it's part of the reused set" - **silently never fired.** Root
   cause: c3.x is a generic constant-register slot every shader's constants
   land on, not camera-exclusive, so unrelated pages interleave; measured
   repeat period was **~31 calls**, blowing through 16 slots before the
   repeat arrived (confirmed by `grep -n` line-position deltas on a raw
   `CAMWRITE` capture). Fixed by widening to 256 slots.
3. **256-slot recurrence gate, arm-once.** Now fires, and the internal
   re-arm loop DOES catch repeat writes to the same page - but always at
   **some other offset**, never the tracked one (e.g. one run: 24 back-to-
   back hits on one page, offsets clustering at just two OTHER byte
   positions, target never touched, then the page went permanently silent).
   A separate run armed on the very first-ever sample (a menu-load address)
   and waited **4 minutes** - one single hit, then nothing.

⇒ **The conclusion, not a tooling gap:** this class of buffer looks
**single-use** - written once by the CPU, read once by the GPU, then either
never revisited or revisited only for OTHER fields, on a timescale beyond
what's practical to wait out. Watching an address after the fact only works
if the same byte gets written again; here it doesn't, reliably, across every
technique tried. **Do not re-attempt address-recurrence watching on this
buffer class - it has now failed three different ways for the same
underlying reason.**

### 45.4 XenDroid comparison: `825AD9F0`'s JIT output for the SAME guest bytes is ~27% larger - explained, not a bug

XenDroid ships `a64_perf_map` **on by default** (`DEFINE_bool(a64_perf_map,
true, ...)`, `a64_code_cache.cc`) - no cvar needed, path
`/data/data/<pkg>/perf-<pid>.map` (fall back `/data/local/tmp`,
`/tmp`). Ran Halo 3 on XDtester (package
`xendroid.compose.xdtester.debug`), confirmed the vista renders **correctly**
(as always), pulled its perf map, found `825AD9F0` at host
`0xaa189aa90`/size `0x7cc`, dumped the bytes the same way as AEX, and
disassembled.

| | AEX | XenDroid |
|---|---|---|
| instructions | 393 | 499 |
| `rev` (byte-swap) | 10 | 28 |
| `str`/`ldr` | 51/57 | 68/76 |
| `sub` | 7 | 28 |
| unconditional `b` | 0 | 28 |
| `ldrb` | 0 | 4 |

The two are **byte-identical through the shared ring-log prologue**, then
diverge right after: XenDroid inserts a conditional block - `ldrb w8,
[x20, #2712]; cbz w8, ...` guarding a register-spill sequence (`rev`+`str`
of r24/r25/r26 to `[r1-72]`/`[r1-64]`/`[r1-56]`, i.e. saving GPRs to the
**guest's own stack**) - that AEX's output skips entirely, rejoining at the
same continuation point either way.

**Traced this to `PreemptCheckInjectionPass::Run` (line 59):
`if (!cvars::guest_scheduler || !builder->first_block()) return;` - the
entire pass is a no-op unless `guest_scheduler` is on.** Neither test run
this session set `guest_scheduler=true` (AEX defaults it off); XDtester
apparently ran with it effectively on. **This is the expected, correct
difference for that config gap - not a JIT miscompilation, and not the
vista bug** (independently: 45.2 already showed this function has no FP
ops, so it can't be flipping a float's sign regardless).

⚠️ Don't resurrect "825AD9F0's JIT output differs" as a lead without first
matching `guest_scheduler` between the two builds - the difference is fully
explained and will reappear every time as noise otherwise.

### 45.5 Genuine PPCContext struct-layout mismatch found (unrelated to the vista, but real)

Computed `offsetof` against **both trees'** real `ppc_context.h` (compiled
minimal standalone probes, not manual struct counting - see 45.1 for why
manual counting is untrustworthy here). AEX's struct has an extra field,
**`reserved_val`** (`uint64_t`, "value of last reserved load", used for
`lwarx`/`stwcx` reservations) between `physical_membase` and `thread_state`.
**XenDroid's struct does not have this field at all** (`grep reserved_val`
on their header: zero matches). Every field from `thread_state` onward is
offset **+8 in AEX relative to XenDroid**:

| field | AEX offset | XenDroid offset |
|---|---|---|
| `physical_membase` | 2688 | 2688 |
| `reserved_val` | 2696 | *(absent)* |
| `thread_state` | 2704 | 2696 |
| `virtual_membase` | 2712 | 2704 |
| `preempt_requested` | 2720 | 2712 |
| `last_safepoint_pc` | 2724 | 2716 |

This is why `[x20, #2712]` means **`virtual_membase`** (a pointer) in AEX's
layout but **`preempt_requested`** (the scheduler yield flag) in
XenDroid's - the same numeric offset in each disassembly is coincidental,
not evidence of anything by itself (each tree's JIT computes its own
offsets from its own header at compile time, so this mismatch is not
inherently a bug). Recorded because **any AE code that references
PPCContext by a hardcoded numeric offset copied from XenDroid source or
docs would be silently wrong** - the same failure mode as 45.1, just not
triggered here.

### 45.6 ▶️ NEXT (untested, for whoever resumes this)

`825AD9F0` is a dead end for finding the computer - it's a mover with no FP
ops (45.2), and the one real JIT difference found is explained and
irrelevant (45.4). The exact-byte-watch technique is now proven unreliable
for this buffer class across three redesigns (45.3) - **do not retry it
without a fundamentally different targeting strategy**, e.g.:

1. **Find a guest function that actually HAS FP/vector instructions** near
   this one in the call graph, rather than continuing to instrument
   `825AD9F0`'s neighborhood. The perf map lists every compiled guest
   function by address; scanning compiled ranges for `rev`-heavy vs
   `fmul`/`fneg`/`vmaddfp`-heavy bodies (same `llvm-mc` technique as 45.2)
   could locate the real computer without needing to catch it in the act at
   all.
2. **Instrument the JIT's store-emission path directly** (inside the a64
   backend itself) to check a target guest address inline and log the
   *live* guest LR at that exact moment, instead of using Xenia's
   page-granular, one-shot access-violation mechanism - this sidesteps
   45.3's entire single-use-buffer problem, at the cost of modifying the
   JIT backend rather than staying in diagnostic-only code. Higher
   engineering cost, but the only approach in this list guaranteed not to
   depend on a write recurring.

## 46. Built the JIT store-watch (idea 2 from 45.6) - found a MUCH stronger candidate, still not the bug

Same day, continuation. Implemented 45.6's idea 2 instead of idea 1: an
inline, address-independent, VALUE-based check baked directly into every
JIT-compiled 32-bit guest store. New file
`cpu/backend/a64/a64_jit_watch_diag.h`; instrumentation in `STORE_I32::Emit`
(`a64_seq_memory.cc`); gated by `debug.canary.jit_store_watch`, must be set
**before launch** to affect functions compiled during boot.

### 46.1 Why value-based, not address-based

45.3 exhausted three address-based redesigns because the target buffer is
effectively single-use - watching "the address CAMWRITE already told us
about" can't work when that exact byte is never written again. This
sidesteps the problem entirely: every 32-bit store checks its **value**
(sign bit set, exponent byte == 126, i.e. magnitude in [0.5, 1.0) - matches
every mirrored camera sample so far: `0xBF7E5FB4`, `0xBF268EE8`,
`0xBF2979F4`) and records `{guest_addr, value, guest_lr}` to a 64-entry ring
buffer with **no function call** - only x0-x18 touched, which the a64
register allocator never assigns to guest values (`a64_backend.cc`: "GPR
set: x22-x28"), so this cannot corrupt a live guest register even while
active. Verified safe in practice: baseline run with the flag off (new code
compiled in but not emitted) behaved identically to before; with it on, the
game still booted to the menu at comparable FPS, no crashes.

### 46.2 First cut drowned in noise - fixed with an address floor

Unfiltered, one 50s run produced **594,997 hits**, overwhelmingly
`guest_addr=0x400F41xx`, `guest_lr=0x89411CD4` - almost certainly a
math/audio library, far below any address range the game's own heaps use.
Added one more compare: skip unless `guest_addr >= 0xA0000000` (the
physical-alias range CAMWRITE's `src_phys` samples always translate
into). Cheap (one extra `cmp`+branch) and categorically can't drop a real
hit, since the camera constant's source has never been observed outside
that range.

### 46.3 ⭐⭐⭐⭐ Convergent evidence: `guest_82177870` is a real candidate

Filtered run: 285,433 hits (still noisy - the value bracket also matches
whatever *else* legitimately produces negative floats of that magnitude
throughout the whole game, not just the camera). Top two `guest_lr`
addresses: `0x82178360` (69,744 hits) and `0x8216A70C` (55,283 hits).

**Both had already appeared independently in section 45's exact-byte-watch
runs** (`guest_lr=0x82178360` at hit 13-14 of one page-chase run;
`0x8216A70C`/`0x8214EEA8` in others) - two structurally different
techniques (page-fault-triggered exact-byte watch vs. inline value-based
check) converging on the same addresses is meaningfully stronger evidence
than either alone.

`0x82178360` resolves (nearest preceding perf-map entry) to
**`guest_82177870`**, `+0xaf0` in. Disassembled it (same `dd` +
`llvm-mc -triple=aarch64 -disassemble` technique as 45.2): **44 `fmov`, 28
`fcvt`, 12 `fcmp`, 4 `scvtf`, 4 `fccmp`, 2 `fadd`, 2 `fsub`** - genuine
floating-point work, unlike `825AD9F0`'s zero. This is the first real
FP-computing candidate this whole investigation has found.

### 46.4 AEX vs XenDroid comparison - close in size, one concrete gap found (not the bug)

Pulled both trees' compiled bytes for `82177870` (XDtester:
`sub_82177870` at `0xaa1799e80`/`0x4628`; AEX clean, `jit_store_watch` off
during the dump to avoid contaminating the disassembly with the probe's own
instructions: `0xaa07206d0`/`0x46bc`). Sizes are close this time - **4527
(AEX) vs 4490 (XenDroid) instructions, ~0.8% apart** - nothing like
`825AD9F0`'s 27% gap. Core FP-op counts **match exactly**: `fcvt`=28,
`fcmp`=12, `scvtf`=4, `fccmp`=4, `fadd`=2, `fsub`=2, `eor`=1, `bic`=1.
Structural counts differ: `fmov` 44 vs 58, `csel` 2 vs 14, unconditional
`b` 25 vs 178, `b.ne` 26 vs 1.

**Found the specific divergence.** Immediately after the first `fcvt d4,
s4` (converting a value loaded from fixed guest constant-table address
`0x82000AF8` from single to double, stored to `PPCContext+568` = `f[31]`),
XenDroid inserts an explicit **NaN-payload-preservation fixup** AEX does
not have:

```
and  w25, w23, #0x7fffffff      ; abs(original single-precision bits)
cmp  w25, #0x7f800000            ; > +Infinity's bit pattern => was a NaN
cset w25, hi
lsr  w23, w23, #22
and  w23, w23, #0x1              ; extract one payload bit from the original
lsl  x23, x23, #51
and  x26, x24, #0xfff7ffffffffffff  ; clear bit 51 of the converted double
orr  x23, x26, x23                ; reinsert the preserved payload bit
csel x23, x23, x24, ne            ; only apply the patch if it WAS a NaN
```

This is real - ARM64's native `FCVT` does not reproduce PowerPC's NaN
payload-preservation rule across single/double precision changes, and
XenDroid patches it in software; AEX does a bare `fcvt` and trusts the
hardware. It very plausibly repeats at every `fcvt` site in a broadcast
loop later in the function (`add x23, x22, #192/196/200/204...`, writing
the same double into 4 consecutive constant slots - looks like a `splat`
filling one vec4 constant with a scalar), which would account for most of
the size/opcode-count gap.

⚠️ **Not our bug, as far as verified**: the camera constant is a normal
float (`0xBF7E5FB4` etc.), not a NaN - `cset w25, hi` would be 0 for it, so
XenDroid's fixup collapses to a no-op (`csel` picks the unpatched `x24`)
for this exact case. A **real, separate JIT correctness gap**, worth fixing
independently, but doesn't explain the sign flip by itself. Checked the
only other candidates (`eor`/`bic`, one each, identical in both trees at
matching positions) - both are plain boolean-flag manipulation (`eor
w22,w22,#0x1`), not sign-bit related. Ruled out.

### 46.5 Second cluster (~line 3596): NaN-propagating `fadd` - also equivalent

Checked the `fadd d4, d6, d7` cluster (AEX line 3596, `func_82177870_clean_mc.asm`;
XenDroid line 3564, `xd_82177870_mc.asm`). Both implement PowerPC's NaN-
propagation rule for addition in software (check each operand for NaN via
`fcmp x,x`+`fccmp`, do the add, canonicalize an unordered result to the
quiet-NaN bit pattern, else quiet whichever operand was NaN and
round-trip it through single precision) - the same shape of fixup as
section 46.4's `fcvt` case, just for `fadd`. **Structurally identical
control flow in both trees**, just compiled with inverted branch polarity
(AEX: `b.vs` direct to the NaN path; XenDroid: `b.vc` skip-then-`b`,
double-negated to the same effect) - a cosmetic codegen-style difference,
not a behavioral one. Ruled out.

### 46.6 Third cluster (~line 4341): a big one, but cross-checked against EARLIER testing and also ruled out

The final `fmov`/return-sequence cluster (AEX line 4337-4364; XenDroid line
4230-4359) looked like the biggest lead this round: both load the same two
stack values into `PPCContext.f[30]`/`f[31]` (offsets 560/568, identical
stack offsets, identical order - no swap), but **XenDroid then restores
r14 through r31 (all 18 PPC non-volatile GPRs) plus `lr` from the guest
stack before its tail-dispatch; AEX restores none of them** (goes straight
from the two `fmov`s to closing the trace-log entry and branching away).
Whole-function counts confirm this isn't just this one exit: `str x.., 
[x20, #150-249]`-shaped restores appear 19× in XenDroid's disassembly vs
6× in AEX's.

This read at first like a serious, independent correctness gap (PowerPC's
ABI treats r14-r31 as callee-saved; skipping their restore would corrupt
whatever the caller stored there). But `a64_backend.cc` only ever discusses
**host** ARM64 callee-saved registers (x19-x28) in this context - there is
no mechanism in AEX for caching **guest** PPC registers in host registers
across a call boundary, so there is nothing to spill back for AEX to skip.
XenDroid's restore sequence reads as the mirror of an optimization AEX
doesn't have (caching hot guest GPRs in host registers within a function,
requiring an explicit spill-to-context before any call/dispatch) rather
than an ABI requirement AEX forgot.

**Cross-checked against already-completed testing, not just this
session's reasoning:** doc §39.6/§42.1 already ran AEX with
`disable_context_promotion=true` - which would force exactly the
"everything always reads/writes context memory directly, nothing cached in
host registers" behavior this hypothesis describes - and found the vista
**stayed broken, bit-identical**. If a missing spill-before-call were the
cause, forcing the always-memory-backed behavior (eliminating any need to
spill in the first place) should have changed something. It didn't.
Ruled out.

### 46.7 ▶️ NEXT

All three FP-relevant clusters in `82177870` are now checked and each has
a benign explanation (NaN-fixup that's a no-op for non-NaN values, cosmetic
branch-polarity differences, a caching-strategy difference cross-validated
against prior `disable_context_promotion` testing). `82177870` remains the
strongest candidate this investigation has produced - real FP work,
convergent evidence from two independent techniques - but a full
instruction-for-instruction diff of the ~4500-instruction function is not
complete; only the three regions with the densest FP-instruction clusters
have been examined. What hasn't been tried:

1. **Diff the sections BETWEEN the FP clusters** - the bulk of the
   function's instructions are `mov`/`str`/`ldr`/`rev` (constant/register
   marshalling, not obviously FP-related), any of which could still carry
   the actual bug even without an `fmov`/`fcvt` nearby.
2. **Find what calls `82177870`** and what it's called WITH - the
   convergent evidence names this function as *involved*, not necessarily
   as the ONE place the sign is wrong; an input it receives could already
   be wrong.
3. Apply the JIT store-watch (46.1) with the value bracket narrowed to
   match a SPECIFIC observed camera sample's mantissa (not just sign+
   exponent) to see whether `82177870` is still implicated once the noise
   from unrelated same-magnitude floats is removed.

### 46.6 Tooling added this round

* `cpu/backend/a64/a64_jit_watch_diag.h` + instrumentation in
  `STORE_I32::Emit` (`a64_seq_memory.cc`) - inline, no-function-call,
  value-based store watch. Probe: `debug.canary.jit_store_watch` (must be
  set before launch).
* `debug.canary.camwatch_sweep` (`memory.cc`) - flips `EnableCamwatchDiag`
  from "chase one address" to "re-arm on every call", for surveying many
  different writers in one run. Superseded in practice by the JIT watch
  (46.1) for this specific investigation, but kept as a general tool.
* `Memory::EnableCamwatchDiag` gained an internal skip for sweep mode so
  the two re-arm strategies (external per-call vs. internal chase-one-page)
  don't fight each other.

## 47. `-0.5` was a false positive; two more candidates checked, arithmetic still provably identical

Same day, later. Re-examined section 46's "top hit" ranking with fresh eyes.

### 47.1 ⭐⭐⭐⭐⭐ `guest_825AD9F0`/`guest_82177870` via `0x82178360`/`0x8216A70C` were noise, not signal

Checked the actual *values* behind the two top `guest_lr` addresses from
section 46.3 (`0x82178360`: 69,744 hits; `0x8216A70C`: 55,283 hits) - both
turned out to be **the constant `0xBF000000` (exactly -0.5) on nearly every
hit**, not varying camera data. `-0.5` has exponent byte 126 (0.5 = 1.0 ×
2⁻¹), so it coincidentally satisfies the section 46.1 filter bracket
(sign set, exponent 126) despite having nothing to do with the camera.
Both addresses are almost certainly a rounding/clamp helper (`x + 0.5`
pattern or similar) called extremely often - hot enough to dominate the
hit ranking by sheer call frequency, drowning out the actual signal.

⚠️ **This means section 46's identification of `guest_82177870` as
involved was built on a contaminated top-of-list read.** The function
itself remains a legitimate finding (46.3's convergence with section 45
data still holds, and IS confirmed connected - see 47.3), but the
*ranking* that made it look dominant was an artifact.

**Lesson for next time: check the VALUE distribution behind a `guest_lr`
before trusting hit count as a relevance signal.** A hot, constant,
sign+exponent-matching value will always outrank a real but less frequent
varying one.

### 47.2 Two cleaner candidates found by filtering on variance, not frequency

Re-ranked all `guest_lr` addresses by "how many DISTINCT values does this
address write" instead of raw hit count. Four addresses -
`0x82205790`/`0x822057A4`/`0x82205820`/`0x82205830` - all resolve into one
function, **`guest_82205690`**, and share overlapping value pools across
runs (e.g. `0xBF7E68D2` appears under multiple of the four) - consistent
with one function writing several nearby fields of the same structure once
per frame. A second, much larger function, **`guest_82203D10`**, was also
implicated via `0x82203D20` (only 4 guest instructions/0x10 bytes into the
function - resolves to the guest address right after its own first nested
call, at line ~43 of the disassembly).

### 47.3 `guest_82205690`: real vector-normalize math, provably identical to XenDroid

1476 instructions (AEX), with genuine arithmetic density: 12 `fmul`, 4
`fmadd`, 3 `fnmsub`, 3 `fdiv`, 1 `fsqrt`. XenDroid's version (2147
instructions) matches **every one of those counts exactly** (`fcvt`=100/100,
`fcmp`=87/87, `fccmp`=32/32, `fmul`=12/12, `fmadd`=4/4, `fsub`=3/3,
`fnmsub`=3/3, `fdiv`=3/3, `fsqrt`=1/1).

Went further than a count comparison this time: located the actual
`fnmsub`/`fmadd`/`fsqrt` instructions by line and checked their operands
directly. **Identical registers, identical order, identical relative
position** in both disassemblies (`fnmsub d6, d7, d11, d8` at AEX line 683
= XenDroid line 1006; `fmadd d5, d6, d6, d5` then `fmadd d4, d4, d4, d5`
then `fsqrt d4, d4` - a textbook squared-length-then-sqrt sequence - at
AEX 1280/1308/1331 = XenDroid 1911/1943/1969). Also confirmed the
immediate inputs match: both store to the same `PPCContext` offsets (560,
568, 408, 392, 328) from the same guest-stack source offsets. **This
function's core computation is provably, not just statistically,
identical between the two builds.**

`guest_82205690` reads `PPCContext.f[30]` (offset 560) as its first real
action; `guest_82177870` (section 46) *writes* `f[30]`/`f[31]` right before
its own tail-dispatch. Strongly suggestive of a real pipeline connection
between the two functions, though the exact call mechanism is an indirect
dispatch through a shared resolver stub (`br x9` to a low, fixed code-cache
address, not a direct jump) that wasn't confirmed by static address
computation alone.

### 47.4 `guest_82203D10`: large divergence found, but traces to a generic per-element loop, not confirmed camera-specific

3306 instructions (AEX) vs 6309 (XenDroid) - the largest size gap of any
function compared this session (~91% larger, not ~1-27% like the others).
Has the first `fneg` (7×) and `fabs` (3×) instructions seen in this whole
investigation - explicit sign manipulation, a strong prior for relevance.

Aligned both disassemblies on the 10 `fneg`/`fabs` instructions as anchors
(all in identical relative order, same register `d4`, in both trees) and
bucketed the `fmul`/`fnmsub` instruction count between each pair of
anchors: **every bucket matched exactly except the last one** (after the
final `fabs`) - AEX has 3 there, XenDroid has 18. A precisely localized
15-instruction gap.

Traced both sides of that gap by hand. Through a `cmp`-and-branch
(comparing two magnitudes, storing `lt`/`gt`/`eq`/`vs` flags, matching a
"which is the larger" style decision) and a divide-then-multiply-by-3
sequence (normalize-by-ratio, writing 3 consecutive guest words), AEX and
XenDroid stay **logically equivalent** - same registers, same context
offsets, only the by-now-familiar cosmetic branch-polarity and NaN-fixup
differences. Past that point, both reach a `cmp w22, #6` / three-flag-store
block, then a **backward branch** (`cbnz`/`cbz`+`b`, AEX `-3512` bytes,
XenDroid `-5188` bytes) whose polarity also matches - a loop, sized
proportionally larger in XenDroid by the same accumulated boilerplate
factor as everything else found this session.

Followed the loop-exit path (the "not looping back" case) in both: a call
to another guest function at a constant address, then - only in
XenDroid's stream at this point - the same 18-non-volatile-register-restore
pattern from section 46.6, already cross-checked there against
`disable_context_promotion=true` testing and ruled benign.

**Traced what `0x82203D20` (the address that originally implicated this
function) actually corresponds to**: guest offset 0x10 into the function -
i.e. the return point of a nested call made almost immediately after the
prologue, nowhere near the `fneg`/`fabs`/loop region examined above. The
loop body indexes an array with a 968-byte element stride (`mul x24, x24,
#968` at AEX line 77) - the shape of iterating bones, lights, or a similar
per-object list, not a signature specific to "the camera." **Not confirmed
as the source of the bug** - a real, large, well-localized divergence, but
its relevance is unproven, unlike 47.3's clean result for `82205690`.

### 47.5 Where this leaves the investigation

Every floating-point computation examined and traced to instruction-level
detail this session - across `guest_825AD9F0`, `guest_82177870`,
`guest_82205690`, and most of `guest_82203D10` - has come back **correctly
and identically translated** by AEX's JIT compared to XenDroid's. This is
a substantive negative result: it is no longer plausible that "the JIT
miscompiles the arithmetic" is the answer for any of the functions found
via convergent evidence so far.

**▶️ NEXT for whoever resumes this:**

1. **The input, not the computation.** Since every checked computation is
   correct, an input reaching it (a constant table value, a kernel-reported
   quantity, or state set by a not-yet-examined earlier function) is the
   remaining live hypothesis. Section 46.4 already noted a fixed
   guest constant-table read at `0x82000AF8` inside `82177870` - comparing
   its raw bytes between a live AEX and XenDroid process (should be pure
   game data, so expected to match, but unverified) is a cheap next check.
2. **Confirm or refute the `82177870` → `82205690` pipeline** (47.3) by
   watching `PPCContext.f[30]`/`f[31]` writes specifically, or by resolving
   the indirect dispatch stub's actual target at runtime, rather than by
   static address computation.
3. **`82203D10`'s loop body** (the part between the ring-log prologue and
   the `fneg`/`fabs` region examined in 47.4) was never actually
   disassembled/compared - only its trailing dispatch/exit logic was.
4. Sections 44-46's candidates are exhausted for the "does this specific
   function's arithmetic match" question. A genuinely new candidate would
   need a different targeting method - e.g. narrowing the JIT watch to a
   specific sample's exact mantissa (46.7's idea 3, still untried) to name
   a function this session hasn't already ruled out.

## 48. ⭐⭐⭐⭐⭐ BREAKTHROUGH: found the exact `0xBF7E5FB4` value in a named, traced call chain - and the WHOLE chain's arithmetic is proven correct

Same day, continuation of section 47. Executed 47.5's idea 2 (a real call
stack, not guessed adjacency) and got a direct hit.

### 48.1 New tool: read Xenia's own stack-unwind mechanism from inside the JIT watch

`guest_lr` (raw `PPCContext.lr`) is usually stale by the time a store deep
inside a function fires - it shows the return point of whatever call *that
function itself* last made, not who called it. Xenia already solves this
problem for its own exception backtraces: every JIT'd function's prologue
pushes an `A64BackendStackpoint` (`a64_backend.h`) - `{host_stack_,
guest_stack_, guest_return_address_}`, 16 bytes - onto an array pointed to
by `A64BackendContext::stackpoints`, indexed by `current_stackpoint_depth`.
`A64Backend::PopulatePseudoStacktrace` already walks this from host C++ for
real stack traces. Confirmed by computing the struct's field offsets and
matching them exactly against the prologue bytes already seen in every
function disassembled this session (`ldr x8,[x19,#152]` = `stackpoints`;
`ldr w9,[x19,#172]` = `current_stackpoint_depth`; the `umull ..., #16` =
`sizeof(A64BackendStackpoint)`).

Added `caller_guest_addr` to `AeJitWatchEntry` (46.1's ring buffer) and,
inline in `STORE_I32::Emit`, read `stackpoints[current_stackpoint_depth -
1].guest_return_address_` the same way `PopulatePseudoStacktrace` does -
still no function call, same x0-x18 scratch-register safety guarantee as
the rest of the watch. Verified stable at the same FPS as before, no
crashes, before trusting the data.

### 48.2 The exact historical bug value, found live

Re-ran the (now caller-aware) watch. `guest_82203D20` (already a candidate
from section 47.2) resolves its caller consistently to `0x8212BDC4`
(76,486 and 93,861 hits across two independent runs). Checked the VALUE
distribution behind that specific (guest_lr, caller) pair -
**`0xBF7E5FB4` is in it** - the *exact* raw value from this
investigation's original bug report (§43.5: "AEX writes `0xBF7E5FB4`,
XenDroid `0x3F7E5FB4`"). This is no longer a plausible candidate by
convergent evidence - it is a **direct, unambiguous capture of the exact
historical bug manifesting live**, with a named caller.

`0x8212BDC4` resolves to `guest_8212BCE0 + 0xE4`. Dumped and disassembled
it (803 AEX instructions, `func_8212BCE0_mc.asm`; XenDroid 1051,
`xd_8212BCE0_mc.asm`). Confirmed the call graph directly in the bytes:
`8212BCE0` builds the guest address `0x82203D10` as a call target
(`mov w16,#15632; movk w16,#33312,lsl16` = `0x8220_3D10`) and `blr`s to it,
with the return point set to exactly `0x8212BDC4` beforehand - **so
`8212BCE0` is confirmed to directly call `guest_82203D10`**, and the
watched value is produced somewhere inside that call.

### 48.3 Traced the whole computation feeding the call - all of it matches XenDroid instruction-for-instruction

Right before the call, `8212BCE0` computes what the code shape strongly
suggests is a delta/direction vector: two pointers (`PPCContext` offsets
288 and 272, i.e. `r31`/`r29`) are each dereferenced to load a guest float,
and `fsub d4, d4, d5` subtracts one from the other before it feeds the
call to `82203D10`. Checked every step against XenDroid's disassembly:

* **The pointers' own setup** (`r29 = word read from guest[r24+r28]`;
  `r31 = r26 + that same word`) - identical registers, identical context
  offsets, identical addition order, in both trees.
* **The `fsub`** - `fsub d4, d4, d5` in both, same registers, no operand
  swap (a swap would silently flip the sign without changing the
  instruction or its count - checked specifically because that's exactly
  the failure mode `fmul`'s commutativity can't rule out but subtraction
  order can hide).
* **The call itself** - same guest target address, same argument-passing
  shape.

**Every single piece of this chain - pointer setup, the subtraction, the
call - is byte-for-byte identical between AEX and XenDroid.** Combined
with section 47's proof that `guest_82205690`'s core computation also
matches exactly, this is now four independently-verified computations
(825AD9F0 ruled irrelevant by content, not comparison; 82177870;
82205690; and now the 8212BCE0→82203D10 chain that provably produces the
exact bug value) all compiling correctly.

### 48.4 What this means

If the code computing with the data is proven identical and the data were
also identical, the output would have to be identical too. Since AEX's
output is wrong and the code is proven right, **the guest memory this
chain reads (at `guest[r24+r28]` and whatever `r31` dereferences to
downstream) must already hold different values by the time this code
runs** - not a JIT miscompilation, a runtime **data** difference. This
does not contradict section 46.4's finding that a *fixed, static*
guest constant (`0x82000AF8`) matches between builds - that was
read-only game data; `r24`, `r26`, `r28` here are almost certainly
**dynamic, per-frame state** (positions, indices) computed by
something else entirely, upstream of this chain.

### 48.5 ▶️ NEXT

The search is now on the INPUT side, with a concrete, named entry point
instead of a vague "somewhere upstream":

1. **Trace `r24`/`r26`/`r28`'s own values** (the context slots feeding the
   pointer arithmetic in 48.3) back to where THEY get set - likely earlier
   in `8212BCE0` itself, or passed in as ITS OWN arguments from ITS caller.
   One more level of the same caller-aware JIT watch technique (48.1)
   applied to `8212BCE0` itself would name that caller directly.
2. **Read the live guest memory at `[r24+r28]`** from both a running AEX
   and XenDroid process at the equivalent moment (same technique as
   46.4's constant check, but this address is dynamic - needs the live
   `r24`/`r28` values captured via CAMWATCH/JITWATCH first, not a fixed
   guest address) to directly compare the INPUT DATA, not just the code
   reading it.
3. This is likely NOT the end of the chain - `8212BCE0` is itself probably
   called from somewhere, and whatever sets `r24`/`r26`/`r28` might itself
   receive them from further up. Each level traced this way is strictly
   progress, even if the ultimate root is several calls further up.

### 48.6 Tooling added this round

* `AeJitWatchEntry::caller_guest_addr` + inline stackpoint-walk in
  `STORE_I32::Emit` (`a64_seq_memory.cc`) - reads
  `A64BackendContext::stackpoints[current_stackpoint_depth-1]
  .guest_return_address_` the same way `A64Backend::PopulatePseudoStacktrace`
  does, giving the JIT watch (46.1) a real caller instead of a possibly-stale
  `guest_lr`. Same safety property as the rest of the watch - only
  x0-x18/scratch registers touched.

## 49. Named `8212BCE0`'s own caller (`821A8FF8`) - it's a generic dispatch loop, not camera-specific code

Executed 48.5 step 1. Extended `AeJitWatchEntry` with a second field,
`grandcaller_guest_addr`, read from `stackpoints[current_stackpoint_depth-2]`
- one more frame up the exact same array the caller-aware watch (48.1)
already reads, no new mechanism.

### 49.1 One caller, zero variance

Every one of 28,736 hits on `caller=0x8212BDC4` (the same call site 48.2
found) resolved to **the exact same** `grandcaller=0x821A91E0` - not a
distribution, a constant. That determinism itself is a data point: this
call site is reached from exactly one place in the guest code, not
dispatched through varying paths.

### 49.2 Verified directly in the disassembly - not inferred from address proximity

Dumped and disassembled the containing function. First attempt used a
manually-computed page-aligned `/proc/pid/mem` read and got the arithmetic
wrong (an off-by-20-pages typo), which silently produced a disassembly
that decoded fine but didn't start with a valid function prologue - a
reminder to sanity-check a raw dump against something structural (a known
prologue shape) before trusting it, the same lesson as 45's stale-comment
bug. Redone correctly (`aa0b87280`, 11792 bytes = `guest_821A8FF8`'s full
host code per the perf map), the file starts with the exact prologue
shape seen in every other function this investigation has disassembled
(`sub sp,sp,#80` / stackpoint push).

Searched the corrected disassembly for the literal construction of
`0x821A91E0` (the watched return address) and found it at line 1129,
**directly followed by** (1135-1141) the resolver-table call sequence
building guest target `0x8212BCE0` (`mov w16,#48352; movk
w16,#33298,lsl16` = `0xbce0`/`0x8212`) and `blr`-ing to it. This is not
address-range inference - it is the actual "store return address, build
target, call" triplet, confirmed byte-for-byte.

### 49.3 What the surrounding code shows: a dispatch loop, not a camera routine

The call to `8212BCE0` is one of a **long run of near-identical call
sites** in `821A8FF8` - at least six visible in a 300-line window alone,
each with the same shape: load `d4` from context offset 568 into offset
328, clear a bit in the context-offset-32 flag word, set both `sp+56` and
context-offset-304 (`lr`) to a return address exactly 8 guest bytes past
the last, build a target guest address via the same `mov
w16/movk w16,lsl16` resolver-load pattern, `blr`. Each call's target is a
**different** guest address (`0x8247C1C8`, `0x8247A3A8`, `0x8212BCE0`,
`0x8247C708`, ...) - the classic shape of a loop walking a **table of
function pointers or node handlers**, calling whichever one the current
iteration's data names, not a hand-written sequence of camera-specific
calls.

This matches `8212BCE0`'s own internal shape (found in 45/48.3: it
initializes `r24=1`/`r26=184`/`r28=0` itself, i.e. its own loop counters)
- `8212BCE0` looks like a **generic node/element handler**, not a
camera-only function, called from a **generic dispatch loop**, not a
camera-only caller. The camera-specific behavior is entirely in the DATA
this generic machinery is pointed at, not in any function name found so
far.

### 49.4 ▶️ NEXT

* One more level (`821A8FF8`'s own caller, via `grandcaller` on a watch
  planted *inside* `821A8FF8` instead of `82203D10`) would name whoever
  drives the dispatch loop - likely where the node list itself starts.
* Per 48.5 step 2 (still open): capture live `r24`/`r26`/`r28`/context-328
  values from a running AEX process at the watched moment and read the
  guest memory those pointers name, then repeat on XenDroid at the
  equivalent point, to compare the actual INPUT rather than more code.
* **Correction, checked immediately after writing 49.3**: the call targets
  in `821A8FF8` are `mov`/`movk` **compile-time immediates**, not a value
  loaded from a guest-memory table at runtime. This is a straight-line
  sequence of `bl <fixed address>` calls baked into the original Xbox 360
  binary itself - since AEX and XenDroid run the **same XEX**, this exact
  call sequence is necessarily identical between builds by construction.
  There is no runtime "table contents" to compare here - retracting that
  half of 49.3's framing. The generic-handler-shape observation about
  `8212BCE0` itself still stands; it's the caller that isn't a data table.
* The real next step is still 48.5 step 2, unblocked now that a stable,
  named watch point exists: capture the live `r24`/`r26`/`r28` context
  register VALUES (not just the code reading them) at the moment
  `8212BCE0` is entered from `821A8FF8`, then read the same slots from a
  running XenDroid process at the equivalent point, to compare the actual
  per-frame INPUT DATA.

## 50. Captured live ctx328/568 and r24/r26/r28 - one hypothesis refuted, one lead needs re-aiming

Extended the watch struct twice more (same mechanism, plain context reads
at the existing watch site - no new infra): first `ctx328`/`ctx568` (the
node-to-node data channel 49.3 spotted), then `r24`/`r26`/`r28`
(48.3's identified pointer-arithmetic inputs, PPCContext offsets
224/240/256).

### 50.1 `ctx328` is a constant `4.0` - the node-channel hypothesis is REFUTED

Across all 19,292 hits sampled, `ctx328` was `0x4010000000000000` (IEEE
double `4.0`) and `ctx568` was always `0.0`, with zero variance. This is
not per-frame camera data - it reads like a fixed type/case selector
identifying which handler `821A8FF8`'s dispatch loop is invoking, constant
because the call site itself is fixed (49.4 already established the call
sequence is baked into the guest binary, identical between builds by
construction). **Retracting the "node channel" as a source of the
divergence** - it's the same fixed value in both builds by necessity, not
a plausible carrier of a mirrored camera.

### 50.2 `r24`/`r26`/`r28` are real, stable, plausible addresses - but likely belong to `82203D10`, not `8212BCE0`

Live values (stable across a full 30s sample, same on every one of 7,219
hits): `r24=0xA5AFE52C`, `r26=0xA5AFE4C4` (104 bytes apart, both in the
physical-alias RAM range the rest of this investigation already uses),
`r28=0x82745EA4` (sign-extended in the 64-bit slot - real 32-bit value -
looks like a fixed table/object base pointer, not a per-frame address).

**Important caveat, not yet resolved**: this watch fires *inside*
`82203D10`, several instructions past where `8212BCE0` calls into it.
`r24`/`r26`/`r28` are PowerPC GPRs - shared, global context slots, free to
be reused by any function. §47.2 already characterized `82203D10` as its
own generic array-iteration routine (968-byte stride). **These captured
values may be `82203D10`'s own loop pointers, not the `r24`/`r26`/`r28`
that fed `8212BCE0`'s pre-call pointer arithmetic described in 48.3** -
the two are easy to conflate because they share register names but are
almost certainly different call frames' data, and the register file
doesn't distinguish them.

### 50.3 ▶️ NEXT

To get 48.3's ACTUAL inputs unambiguously, the watch needs to fire
*inside `8212BCE0` itself*, immediately before its call to `82203D10` -
not inside `82203D10`. Concretely: add a second, narrower watch keyed on
`guest_lr`/caller `== 0x8212BDC4`'s containing function rather than the
float-store filter, or simply add a one-off store-and-log right after the
`fsub d4,d4,d5` this investigation already located in `8212BCE0`'s own
disassembly (48.3) - that guarantees the captured `r24`/`r26`/`r28` are
the actual pointer-arithmetic inputs, not a downstream function's reuse
of the same register slots.

## 51. ⭐⭐⭐⭐⭐ MEASURED A/B: the camera is a QUATERNION and AEX's `w` is negated — first direct both-emulators capture of the bug value

**2026-08-17.** The first measurement in this investigation that captures the
wrong value in AEX **and** the right value in XenDroid, at equivalent state, in
the same field of the same object.

### 51.1 The value filter was structurally incapable of answering the question

Section 46's watch admits only negative floats with exponent byte 126, i.e.
magnitude in `[0.5, 1.0)`. **Every sample it can ever return is negative by
construction.** It can confirm that a wrong value exists; it can never show a
right one, and it cannot distinguish "wrongly negative" from "legitimately
negative". Several earlier readings were over-interpreted because of this - a
field looked suspicious purely because the only samples the instrument was
capable of emitting were negative.

Concretely: `guest_A5B06320` was recorded as holding values in `[-0.85, -0.53]`,
but reading the same address directly out of guest RAM showed `+3.044`. The
field was never negative-only; the filter was.

This is the same trap `feedback_measurement_discipline` names - an instrument
that cannot report failure is not one. Superseded by 51.4's exact-address mode.

### 51.2 Located the camera object by CONTENT signature, not address

Heap addresses differ between runs and between emulators, so no address-based
comparison can work across the two builds (reading AEX's address in XenDroid
returned all zeroes). Instead the object is found by what it contains:

* a frame delta `0x3C888889` (= 1/60), immediately followed by
* the ASCII tag `0x72616421` (`"rad!"`).

That pair is unique and stable: it matches exactly 2-3 objects per run, in both
emulators. Tooling: `scratchpad/findquat.sh` (+ `dumpguest.sh`, `readguest.sh`
- guest RAM is read through `/proc/<pid>/mem` at the membase taken from the
`xenia_memory` ashmem mapping; **the reads must be page-aligned**, unaligned
byte offsets return EIO).

### 51.3 ⭐ The field at +0x34 is a UNIT QUATERNION, and its `w` sign is the bug

At offset `+0x34` from the tag sits four floats whose norm is 1.000 in every
sample from both emulators - a unit quaternion, i.e. the camera orientation.

| build | `w` samples | vista |
|---|---|---|
| **AEX** | `-0.9495`, `-0.9418`, `-0.9983`, `-0.9931`, `-0.9397`, `-0.9979` | **upside down** |
| **XenDroid** | `+0.9942`, `+0.9942`, `+0.9937`, `+0.9937`, `+0.9937` | **correct** |

Same device, same game, same driver, same object, equivalent state. **AEX's
sign is negative in every sample ever taken; XenDroid's is positive in every
sample ever taken.**

This is exactly the shape of the original bug report (§43.5: AEX `0xBF7E5FB4`,
XenDroid `0x3F7E5FB4` - identical mantissa, sign only), and it finally explains
*why* that shape: **a quaternion with only `w` negated is the INVERSE
rotation.** `(-w, x, y, z) = -conj(q)`. Negating all four components would be
the same rotation and harmless; negating `w` alone turns the camera to look the
opposite way, which is precisely a mirrored vista with an upright 2D UI.

It also retires the "is this value legitimately negative?" ambiguity that
51.1's filter created: the same field is reliably positive in the build that
renders correctly.

### 51.4 New instrument: exact-address watch (can report CORRECT values)

Added `debug.canary.jit_watch_exact` + `debug.canary.jit_watch_addr` (a
**runtime** guest address, since a heap object's address is not known until the
scene is built and moves between runs; new `xe::AeDiagValue()` reads it, sampled
at 500 ms like `XE_AE_DIAG_ENABLED`). In this mode the watch matches one address
and records whatever is written or read there, sign included. Also added a
**load** watch (`debug.canary.jit_load_watch`, `kind` field in the ring entry) -
the store watch can only say where a value was put, the load watch says where it
was READ FROM, which is the address to trace to next.

Both share one emit body (`EmitAeJitWatchBody`) so the two can't drift apart.

### 51.5 ⭐ The quaternion is written by UNALIGNED VECTOR stores, not `STORE_I32`

Pointed the exact-address watch at the live `w` address: **90,237 loads, ZERO
stores** - across both the `0xA5…` physical alias and the `0x85…` alias (both
are read; 835 loads on the latter). The value animates continuously, so it is
certainly being written - just not through `STORE_I32`, which is all the watch
covered.

A 16-byte quaternion at a non-16-byte-aligned address is written with the
standard PowerPC `stvlx`/`stvrx` pair. **That is why every previous section's
store watch, and §44-§50's whole line of pointer-chasing, never saw the writer:
they were watching the wrong opcode class the entire time.**

### 51.6 Found and fixed a real `stvlx`/`stvrx` defect — but it is NOT the vista bug

AEX's `STVL_V128`/`STVR_V128` read the aligned 16-byte line, blend the in-range
bytes with a 2-register TBL, and **store all 16 bytes back** - including bytes
the instruction must not touch. Its own comment conceded the `offset == 0` case
"load and store back the same memory" while still emitting a full-line write for
an instruction that should write nothing. Single-threaded the bytes round-trip
unchanged, which is why it hides; but any concurrent write to the out-of-range
bytes between the load and the store is silently reverted. XenDroid rewrote both
to touch only the in-range bytes, with exactly that rationale in its comment.

Ported both. **Result: vista still upside down, `w` still negative
(`-0.9979`).** So this is a genuine correctness fix in the exact code path that
writes the camera quaternion, but it is **not** the cause of the flip. Kept
behind `debug.canary.fix_stvlr_partial` (defaults ON = correct behaviour; the
old whole-line blend is retained as `EmitBlendWholeLine` for a perf A/B),
because byte-at-a-time is more instructions than the vector blend and **the
perf cost has not been measured**.

### 51.7 Eliminations this round (all by direct check, not inference)

* **PPC frontend cleared.** `ppc_emit_fpu.cc` is byte-identical to XenDroid.
  `ppc_emit_altivec.cc` differs only by the `vmsum*`/`vsum*` integer family,
  which AEX leaves `XEINSTRNOTIMPLEMENTED` - and Halo 3 executes **zero** of
  them (`grep -c "Unimplemented instruction"` = 0 across a from-launch 200 MB
  log). A real gap to close, but not this bug.
* **`PackSingleKeepNaN` confirmed benign**, not assumed: for non-NaN inputs it
  returns `Select(is_nan, …, sbits)` = exactly AEX's `Cast(Convert(v, F32))`.
* **`PERMUTE_I32` byte-identical** apart from XenDroid's zip1/zip2 shortcut,
  which is computed off the *same* `tbl_ctrl` and is therefore equivalent by
  construction. The `inline_leaf_calls` / `inline_gprlr_saverest` blocks are
  XenDroid-only perf work, already shown non-causal by §41.
* **Static XEX data byte-identical** between the two running emulators
  (`0x82022510…0x8202252C` incl. the `4.0` that feeds `ctx328`). The image
  loads the same.
* **Config diff re-run independently** and reproduced §38.2's four divergences
  exactly, with no new ones. Method validated, no new information.

### 51.8 ▶️ NEXT — one clear step

The writer is an unaligned vector store, so **extend the exact-address watch to
`STVL_V128`/`STVR_V128` (and `STORE_V128`/`STORE_I64`)**, matching any store
whose 16-byte span covers the target address, and record all four lanes plus
caller/grandcaller. That names the guest function that writes the negated `w` -
which §44-§50 could never reach because they only ever watched `STORE_I32`.

Only after that does it make sense to ask *why* the sign is wrong. The most
likely shapes, given "only `w` differs":
* a quaternion SLERP shortest-path test (`if dot(q0,q1) < 0 negate`) taking the
  wrong branch - a single inverted compare produces exactly this;
* a `vsel`/`vcmp` or `fsel` whose condition is inverted;
* a sign-mask constant applied to the wrong lane.

### 51.9 Also attempted, blocked

Ran the desktop x86 oracle (`/home/roman/xeniatest/oracle`, Halo 3 boots and
translates shaders fine) to test §38.4's open question - whether the same
emulator lineage on an x64 JIT renders the vista upright, which would place the
bug squarely in the a64 backend. **Blocked on screen capture**: the desktop
session's display is blanked, `spectacle` returns an all-black frame, Xenia's
own F12 screenshot needs a keystroke, and no `xdotool`/`wmctrl`/`Xvfb` is
installed. Still worth doing - it is a cheap, decisive discriminator - but it
needs a working headed session or an input tool.
