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
