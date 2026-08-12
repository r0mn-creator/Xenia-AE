# Halo 3 menu vista renders UPSIDE DOWN (2D UI does not)

Status 2026-08-06: **not fixed**, but narrowed to a specific mechanism with a
cheap decisive test. Halo 3 boots to the menu on Turnip v26.0.0 R8.

## The key structural clue

**The 3D vista is inverted; the 2D UI overlay on top of it is not.** Both are
composited by the same final present path, so the flip is introduced *before*
presentation, in something the 3D path does and the 2D path does not.

## Ruled out: negative viewport height

Vulkan's `VK_KHR_maintenance1` negative-viewport-height trick is **not** used.
`vulkan_command_processor.cc:5520` always sets a **positive** height from
`viewport_info.xy_extent[1]`, and the present path
(`vulkan_command_processor.cc:1688`) likewise. So the Xenos(Y-up) ->
Vulkan(Y-down) correction must be happening in the **shader**, via `ndc_scale[1]`
computed in `draw_util.cc GetHostViewportInfo`.

## The mechanism that fits

`draw_util.cc:305` reads the guest viewport conditionally:

```cpp
float scale_xy[] = {
    pa_cl_vte_cntl.vport_x_scale_ena ? args->PA_CL_VPORT_XSCALE : 1.0f,
    pa_cl_vte_cntl.vport_y_scale_ena ? args->PA_CL_VPORT_YSCALE : 1.0f,
};
```

**2D UI draws normally disable the viewport transform** (`vport_*_ena = 0`,
pre-transformed screen-space coordinates), so `scale_xy[1]` falls back to the
literal `1.0f`. **3D draws enable it**, so `scale_xy[1]` takes the guest's
`PA_CL_VPORT_YSCALE` - which in D3D9 convention is typically **negative**.

That maps *exactly* onto the observed split: transform-disabled geometry is
correct, transform-enabled geometry is inverted. It means the Y-flip handling
is wrong specifically on the path where the guest supplies its own Y scale -
either its sign is being dropped, or the Vulkan correction is applied on top of
a value that already encodes the flip (a double negation).

## The decisive test (cheap, on-device, no rebuild if a probe is added)

Log, per draw, for the menu:

- `pa_cl_vte_cntl.vport_y_scale_ena`
- `args->PA_CL_VPORT_YSCALE` (**sign matters**)
- the resulting `ndc_scale[1]` and `ndc_offset[1]`

Then compare a **vista draw** against a **UI draw**:

| observation | conclusion |
|---|---|
| vista `YSCALE < 0` and `ndc_scale[1]` ends up with the **same** sign as the UI's | the guest's flip is being **lost** |
| vista `YSCALE < 0` and `ndc_scale[1]` is flipped **twice** vs expectation | **double negation** - the Vulkan correction is applied to an already-flipped value |
| `YSCALE > 0` for the vista | hypothesis wrong; look at the EDRAM resolve -> texture path instead |

## Why the third branch is plausible too

Prior work recorded the vista as a **deferred** scene that goes
RT -> EDRAM resolve -> shared memory -> texture -> sample. Any Y-orientation
mismatch in that round trip inverts the sampled result while leaving directly
drawn UI untouched. If `YSCALE` turns out positive, that path - not the
viewport math - is the place to look, and `RENDER_PIPELINE_AUDIT.md` already
describes it.

## Do NOT repeat

- The flat-navy menu bug is **separate and already solved** (stock Qualcomm
  driver mis-compiling `resolve_full_32bpp`; Turnip fixes it). The vista being
  upside-down was only *discovered* once Turnip made it visible.
- This is **not** the memexport/geometry-collapse bug. See
  `HALO3_MEMEXPORT_READBACK.md` - and note that investigation's central
  "underfill" premise is now in serious doubt.

---

## ⭐ MECHANISM (2026-08-06): the `1.0f` fallback does not apply the Vulkan Y flip

Traced the whole Y path end to end. There is **no Vulkan-specific Y negation
anywhere** - not a negative viewport height, and not in the SPIR-V translator.
`spirv_shader_translator.cc:1899` simply does:

```
position_xyz = position_xyz * ndc_scale + ndc_offset * w
```

So **`ndc_scale[1]` is the only thing that can flip Y**, and its sign comes
entirely from `scale_xy[1]` in `draw_util.cc:305`.

### Why that works for a normal 3D draw

D3D9's viewport transform is `screenY = (1 - ndcY) * h/2 + y`, so the guest's
`PA_CL_VPORT_YSCALE` is **negative** (`-h/2`). In the clipping-enabled branch:

```
ndc_scale_axis = scale_axis * 2.0f * inv_axis_extent_rounded
               = (-h/2) * 2 / h  =  -1.0
```

`ndc_scale[1] = -1.0` converts Xenos/D3D **Y-up** NDC to Vulkan **Y-down** NDC.
Correct - and the viewport rect itself is unaffected because the extent is
computed from `scale_axis_abs`.

### Why it works for the 2D UI

UI draws are pre-transformed and disable the viewport transform, so
`scale_xy[1]` takes the `1.0f` fallback. Their coordinates are already in
**screen space (Y down)**, and Vulkan's NDC is also Y-down, so a **positive**
scale is right. Correct by coincidence of conventions.

### ⭐ Where it breaks

The fallback is a bare `1.0f`:

```cpp
pa_cl_vte_cntl.vport_y_scale_ena ? args->PA_CL_VPORT_YSCALE : 1.0f
```

That value is correct **only** for pre-transformed screen-space geometry. For
geometry still in **clip space (Y-up)** that merely has the viewport Y scale
disabled, `+1.0` applies **no flip at all** - and the result renders **upside
down**, exactly as observed.

So the predicted condition for the vista draws is:

> `pa_cl_vte_cntl.vport_y_scale_ena == 0` **while** `pa_cl_clip_cntl.clip_disable == 0`

i.e. clip-space geometry with the viewport Y scale disabled. The fallback needs
to be **-1.0f** (or the flip applied separately) on that path, while staying
`+1.0f` for the pre-transformed 2D path.

### Test before changing anything

Log `vport_y_scale_ena`, `clip_disable`, `PA_CL_VPORT_YSCALE` and the final
`ndc_scale[1]` for a vista draw and a UI draw.

- vista shows `y_scale_ena=0, clip_disable=0` → **hypothesis confirmed**, and
  the fix is a conditional fallback sign.
- vista shows `y_scale_ena=1` with negative YSCALE → hypothesis **wrong**; the
  viewport math is fine and the inversion is in the EDRAM resolve -> texture
  round trip instead.

⚠️ This fallback is shared by **every title**, so any change must be toggled
(`debug.canary.*`) and re-tested on NFS Carbon and Geometry Wars, not just
Halo 3.

---

## ⭐⭐ TEST 1 RESULT (2026-08-06) — HYPOTHESIS CONFIRMED

`debug.canary.ytest_fallback=1` (negate only the `1.0f` Y-scale fallback).
Halo 3 menu, Turnip, three stable screenshots.

**The vista changed from UPSIDE-DOWN to BLANK WHITE. The 2D UI was completely
unaffected.**

### What this establishes

1. **The vista is the consumer of the `1.0f` fallback.** Changing only that
   value changed only the vista - direct causal proof, not inference.
2. **The 2D UI supplies its own `PA_CL_VPORT_YSCALE`** and does *not* use the
   fallback. This is the **reverse** of the original reasoning, which assumed
   the UI took the fallback and the 3D path supplied its own scale. The
   conclusion survives; the reasoning behind it did not.
3. **A bare sign flip is not the fix.** Negating the scale alone mirrors about
   the **NDC origin**, not the viewport centre, so the geometry lands
   off-screen - which is exactly the white frame observed.

### Test 2 (built, installed, NOT yet run)

`debug.canary.ytest_offset=1` negates `ndc_offset[1]` alongside the scale so the
mirror happens in place. Read with `ytest_fallback=1`.

- vista appears **right-side-up** → that is the fix
- vista appears but **still inverted** → mirroring about the wrong axis; the
  offset needs a different correction than a plain negation
- still **blank** → the offset is not the missing piece; something else places
  this geometry

### State on pause

All experiment toggles reverted to 0 (they are `XE_AE_EXPERIMENT`, default OFF,
so the shipped build behaves exactly as before). `readback_memexport` back to
`false`. `xe.log` deleted - the VTXDIST probe had grown it to **3.6 GB**.

⚠️ This fallback is shared by **every title**. Even a confirmed fix must be
toggled and re-tested on **NFS Carbon** and **Geometry Wars** before becoming a
default.

Incidental: Halo 3's menu runs at **~15-18 FPS**, notably better than NFS
Carbon's ~9.7.

---

## ⭐ SOURCE-LEVEL DIVERGENCE FOUND (2026-08-07) — AE's resolve shaders are REDUCED

Unlike everything else in this investigation, this is a **static source diff**.
It cannot be a sampling error, a scene-state difference, or a misplaced probe.

`src/xenia/gpu/shaders/resolve.xesli` — the shared include every resolve shader
pulls in — differs from upstream by **857 lines**:

| | oracle (**renders correctly**) | AE (**broken**) |
|---|---|---|
| lines | **989** | **818** (171 fewer) |

**In the oracle but NOT in AE:**
`decode_pwl_gamma`, `dest_number_is_unorm`, `dest_num_format`,
`dest_row_pitch_macro_tiles`

**In AE instead:**
`dest_row_pitch_aligned`, `dest_slice_pitch_aligned`, `bpp_log`,
`pixel_stride_ints`

So AE is **missing operations upstream performs** (PWL gamma decode, destination
format classification) and uses a **different destination addressing model**
(plain alignment vs macro-tiles). That is not an equivalent-result rewrite - it
changes both the values written and where they land.

`resolve_full_32bpp.cs.xesl` itself is **byte-identical**; the divergence is
entirely in the shared include, which is why all 12 compiled resolve shaders
show 5-15k differing bytecode lines. `apply_gamma_pwl.xesli` and
`apply_gamma_table.xesli` also differ.

This is the EDRAM resolve path - where `RENDER_PIPELINE_AUDIT.md` places the
vista, and the shader family the flat-navy bug lived in.

### Swap test ATTEMPTED and REVERTED — the shader set is coupled

⚠️ **The Android build compiles shaders from source** (`compile_shader_spirv.py`
+ `glslangValidator`); the checked-in bytecode under `shaders/bytecode/` is
**regenerated**, so editing bytecode alone does nothing.

Copying the oracle's `resolve.xesli` alone failed to compile
(`resolve_clear_32bpp.cs.xesl` depends on AE's version). Copying **all 34**
resolve sources also failed - `glslangValidator` exits non-zero with **no
diagnostic at all**, which looks like a crash rather than a syntax error.

**Tree reverted with `git checkout`; build verified SUCCESSFUL afterwards.**

### Next

1. **Read the 857-line diff and classify it.** Is AE's version an older
   upstream snapshot, or a deliberate Adreno-targeted reduction? Git history on
   that file should say.
2. If it is drift, port upstream's version **with its C++ side** -
   `vulkan_render_target_cache.cc` sets the push constants these shaders read,
   and the two must match. That is the likely reason a shader-only swap crashed
   the compiler.
3. `decode_pwl_gamma` being absent is independently suspicious given
   `apply_gamma_pwl.xesli` also differs.

---

## TEST 2 (corrected) 2026-08-09 — mirror does NOT fix it; hypothesis in doubt

### First attempt was a no-op — my error

The offset mirror was added only to the `if (pa_cl_clip_cntl.clip_disable)`
branch. But the predicted condition for the vista is `vport_y_scale_ena == 0`
**with `clip_disable == 0`** - the *clipping-enabled* branch. So the code never
ran, and the result was reported as "outcome 3: offset is not the missing piece"
when in fact nothing had been tested. Corrected by adding the mirror to the
clipping-enabled path as well.

### With the mirror actually running

| test | vista |
|---|---|
| control (all toggles off) | renders, **upside down** |
| `ytest_fallback=1` | **blank white** |
| `ytest_fallback=1 + ytest_offset=1` (correct branch) | **still blank white** |

Negating scale *and* offset — a mirror about the viewport centre — does not
bring the geometry back. So the simple "the fallback needs to be -1.0" model is
**wrong**.

### What still holds, and what does not

**Holds:** the vista *does* consume the `1.0f` Y-scale fallback. Changing only
that value changes only the vista, and never the 2D UI. That is causal and was
reproduced three times.

**Does not hold:** that the inversion is *caused* by the missing flip there.

### Leading explanation now

The vista is a **deferred scene** - rendered offscreen, resolved through EDRAM,
then sampled as a texture (see `RENDER_PIPELINE_AUDIT.md`). That suggests:

- `ndc_scale[1]` affects the **offscreen geometry pass**. Negating it throws
  that geometry out of the offscreen target, so the resulting texture is empty -
  hence blank, at any offset.
- The **inversion** may happen later, in the **resolve or composite**, where the
  texture is sampled with the wrong Y orientation.

If so, the viewport math is a red herring for the *orientation* and the target
is the RT->EDRAM->texture path instead. That path is also where AE's
`resolve.xesli` is **171 lines shorter than upstream's**, missing
`decode_pwl_gamma`, `dest_number_is_unorm` and `dest_row_pitch_macro_tiles` -
see the resolve-divergence section above.

### Next

1. Confirm the vista is genuinely render-to-texture (log the RT bind + resolve
   for the vista draws) rather than drawn direct.
2. If so, check the **sampling** orientation in the composite, not the viewport.
3. The `resolve.xesli` divergence becomes the prime suspect again - and unlike
   the viewport theory it is a *known* difference from a working implementation,
   not an inferred one.

---

## Resolve addressing is EQUIVALENT — third hypothesis eliminated (2026-08-09)

The `resolve.xesli` divergence (989 vs 818 lines) looked like the cause, since
AE uses `dest_row_pitch_aligned` where upstream uses
`dest_row_pitch_macro_tiles`. **It is a refactor, not a functional difference.**

| | AE | oracle |
|---|---|---|
| stored pitch | `(info & 0x3FF) << 5` (pixels) | `info & 0x3FF` (macro-tiles) |
| used as | `int(pitch_aligned >> 5u)` | `int(pitch_macro_tiles)` |

AE shifts **left 5 on store, right 5 on use** — a round trip to the same value.

Address maths also match:
- AE 2D: `(p.x >> 5) + (p.y >> 5) * pitch`
- Oracle 2D: `(p.x >> WIDTH_LOG2) + (p.y >> HEIGHT_2D_LOG2) * pitch`, and both
  constants are **5**.
- AE 3D uses `p.y >> 4` / `height >> 4`, matching `HEIGHT_3D_LOG2 = 4`.

So row order is identical on both platforms and **cannot** be producing a
vertical flip. The functions AE genuinely lacks (`decode_pwl_gamma`,
`dest_number_is_unorm`, `dest_num_format`) are **gamma and format** handling —
they would change colour, not orientation.

## Eliminated so far

1. ❌ Viewport `1.0f` Y-scale fallback — mirroring scale+offset leaves it blank
2. ❌ Resolve destination row addressing — arithmetically equivalent
3. ❌ Missing resolve functions — gamma/format, not orientation

## Confirmed

- The vista **is** render-to-texture: RT tile **1216** resolves to
  `0x044B0000`, then is sampled.
- It **does** consume the `1.0f` Y fallback — changing it changes only the
  vista, never the UI (reproduced 3x).
- ⚠️ Screenshot size is a cheap oracle: **~1.4 MB = vista rendering,
  ~195 KB = blank**. No need to eyeball it.

## The next step should be EVIDENCE, not another hypothesis

Three inferred causes have now failed. Stop inferring and localise it:

**Dump the resolved surface at `0x044B0000` and check whether it is already
upside down in memory.**

- **Flipped in memory** → the fault is at or before the resolve write.
- **Correct in memory** → the resolve is fine and the flip is in how the
  texture is *sampled* during composite.

That single observation halves the search space, and unlike the last three
attempts it does not depend on a theory being right.

---

# RESULT (2026-08-09): the dump answered a *different* question than expected

The dump was built. It did not return "flipped in memory" or "correct in
memory" — it returned **neither**, because the premise of both branches was
wrong.

## What the resolves actually are

Enumerating every distinct resolve destination in a menu frame (rather than
dumping one chosen arbitrarily) showed the scene is resolved as **four
contiguous 1152x160 bands**, not one surface — classic EDRAM strip rendering,
since a 1152x640 target does not fit in 10 MB of EDRAM:

| region  | bases (each `0xB4000` apart)                             |
|---------|----------------------------------------------------------|
| colour fmt 6  | `0x044B0000` `0x04564000` `0x04618000` `0x046CC000` |
| colour fmt 7  | `0x04780000` `0x04834000` `0x048E8000` `0x0499C000` |
| depth  fmt 23 | `0x04A50000` `0x04B04000` `0x04BB8000` `0x04C6C000` |

4 x 737280 = 2949120 = exactly the length of the single 1152x640 resolve at
`0x04E20000`.

⚠️ Every earlier dump attempt read **1280x576** at `0x044B0000`. That geometry
is wrong on every axis, which is why the output was noise and then "all zeros".
The all-zeros reading was an artefact of the wrong address *and* wrong size —
not evidence that the resolve writes nothing.

## The decisive test: inject a signal instead of reading bytes

Reading the bytes kept failing because every conclusion depended on guessing
format, tiling and which of the three candidate regions is displayed. So
instead: paint the first 8 rows of each band a distinct colour (red / green /
blue / yellow) directly into guest memory right after the resolve, and look at
the screen. An 8-row stripe is an 8-row stripe regardless of channel order.

**Result: no stripes appear at all, and `TriggerPhysicalMemoryCallbacks`
reports `watched=0` for both colour regions.**

The `watched=0` check is what makes this conclusive rather than a false
negative. A first run without it also showed no stripes, but that proved
nothing: a raw `memcpy` through `TranslatePhysical` bypasses the physical
memory watches the texture cache uses to notice guest RAM changed, so the
texture would never be re-uploaded either way. After explicitly triggering the
watches, **nothing was watching those ranges** — nothing consumes them.

## Conclusion — the search space is halved, just not along the expected axis

**The vista composite never reads the resolved guest memory.** Those resolve
destinations are written and not consumed through guest RAM; the vista reaches
the screen via the host-side render-target → texture path (RT-as-texture),
without a guest-memory round trip.

Therefore the flip is in the **host RT → texture handoff**, and these are all
now off the table:

- resolve row addressing (already refuted arithmetically — now also moot)
- band ordering / band→address mapping (the idea this dump made look promising)
- Xenos tiling/swizzle of the resolve destination
- anything reached by dumping `0x044B0000`

Confirmed still true: the vista on screen is inverted (verified by flipping the
screenshot — the flipped version is the natural-reading image), while the 2D UI
overlay is not.

**Next:** instrument the RT-as-texture path in the render target cache, not the
resolve path. Find where the host render target is bound as a sampled image for
the composite and check the Y orientation of that view/blit.

---

# ⚠️ CORRECTION (2026-08-09, same day): the marker test above was INVALID

The conclusion "the vista composite never reads the resolved guest memory,
therefore the resolve path is off the table" is **wrong**. Retracted.

**Why it is wrong.** Xenia does not resolve to guest RAM and read it back.
Resolves write into the **GPU-side shared memory buffer**, and the texture
cache loads from *that* buffer (`vulkan_texture_cache.cc` reads
`vulkan_shared_memory.buffer()`), never through `TranslatePhysical`. After a
resolve, `SharedMemory::RangeWrittenByGpu` -> `MakeRangeValid(..., gpu_written)`
marks the pages valid precisely so they are **not** re-uploaded from guest RAM.

So both observations have an innocent explanation:

- **No stripes**: a CPU write to guest RAM is never uploaded over a
  `gpu_written` page, so the paint could not reach the texture no matter what.
- **`watched=0`**: watches are dropped for GPU-written ranges. It means "the
  GPU owns this range", not "nothing consumes this range".

The marker experiment cannot distinguish the two hypotheses at all. It was
answering a question nobody asked.

Galling detail: `shared_memory.cc` **already carried a comment stating this**,
written during the memexport investigation - *"The consumer does NOT read guest
RAM - it reads Xenia's GPU-side shared memory buffer."* The information needed
to predict the null result was in the same file being probed.

**Restored to the table:** the resolve path, resolve row addressing, band
ordering, and the tiled layout of the resolve destination. Nothing about the
resolve has been eliminated.

**Still valid from that round** (these were direct observations, not inferences):

- The scene resolves as **four contiguous 1152x160 bands**, not one surface,
  and the band bases are exactly `0xB4000` apart. Any future dump must use this
  geometry - the old 1280x576 assumption is wrong on every axis.
- The on-screen vista is genuinely inverted (verified by flipping the
  screenshot), while the 2D UI overlay is not.

## Prior-art search (2026-08-09) - no existing fix found

Searched the Xenia issue trackers, upstream git history, the Xenia GPU
write-ups, Mesa/Turnip trackers, and the Android forks. **Nobody has published
a fix for this.** What the search did turn up:

- **`959b8ef19` "[D3D12] Draw rectangles by mirroring one vertex across
  diagonal"** - Xenos `kRectangleList` builds a quad from 3 vertices with the
  4th mirrored across the longest edge; getting it wrong mirrors the quad.
  Plausible mechanism, and full-screen vista quads are rectangle lists.
  **CHECKED: AE's Vulkan rectangle-list geometry shader is byte-identical to
  upstream canary's. Eliminated.**
- `xenos.h:965-1000` documents that **Direct3D 9 performs resolves by drawing
  `kRectangleList`**, and does so using **Halo 3 (`4D5307E6`)** as its worked
  example. Directly relevant reading for this bug.
- eDRAM depth tiles flip even/odd 40-sample columns relative to colour tiles -
  a real orientation asymmetry in the format, worth remembering.
- An upstream "upside down render-to-texture" bug **was** fixed between
  2019-06 and 2020-11 (Go! Go! Break Steady, button prompts upside down ->
  later reported correct). The specific commit was not identified. AE's GPU
  backend is forked from older Xenia, so an unported fix in that window remains
  a live possibility.

## Best next step

The desktop canary oracle renders this menu correctly on RADV. So either AE's
fork diverges from upstream somewhere in the resolve/texture path, or the fault
is Adreno/Turnip-specific. Separating those two is worth more than any further
inference, and this project already has a proven method for it: **diff against
the Android forks that work**, which is how the two NFS Carbon bugs were found.

---

# FORK DIFF vs upstream canary (2026-08-10) - NO orientation divergence found

Diffed AE's GPU tree against a current upstream canary checkout
(`/home/roman/xeniatest/canary-git`, HEAD `6e5b8324f`). Raw line counts look
alarming, but nearly all of it is churn from two upstream refactors:

1. **XeSL naming**: `float4_xe` -> `xesl_float4`, `unpack_half_2x16_xe` ->
   `xesl_unpackHalf2x16`, `dont_flatten_xe` -> `xesl_dont_flatten`, ...
2. **Binding model**: constant buffers -> **push constants** across all
   texture-load and resolve shaders.

Normalising both away (`scratchpad/ren.py`) leaves very little.

| path | verdict |
|---|---|
| Vulkan rectangle-list geometry shader | **byte-identical** to upstream |
| `XeResolveDestPixelAddress` | structurally equivalent, **no Y flip in either** |
| `GetHostViewportInfo` Y-scale logic | **identical** to upstream once our default-OFF `ytest_*` toggles collapse to `1.0f` |
| `texture_load.xesli` | differences are 100% the push-constant refactor |
| `resolve.xesli` | real differences are **gamma/format/addressing-units only** (see below) |

The genuine upstream-only additions in `resolve.xesli` are
`decode_pwl_gamma`, `dest_num_format`, an EDRAM uint-vector buffer path, and a
`dest_row_pitch_macro_tiles` (upstream) vs `dest_row_pitch_aligned` (AE)
addressing model backed by a new `texture_address.h` module AE lacks.
**None of them touches vertical orientation.**

Also checked and **not** a bug: AE sets `dest_base = 0` in the
resolution-scaled resolve path where upstream adds it unconditionally. That is
deliberate - the scaled path supplies the base elsewhere. Nearly filed it as a
find.

## What this means

The fork-divergence hypothesis is **not supported** for the orientation bug.
Combined with the desktop oracle rendering this menu correctly on RADV, the
weight now sits on a **driver-side (Adreno/Turnip) cause**.

There is direct precedent in this very investigation: the earlier **flat-navy**
bug was the **stock Qualcomm driver mis-compiling `resolve_full_32bpp`**.
Turnip fixed that and exposed the inversion. The same shader family being
mis-compiled a second way by a different driver is entirely consistent.

**Next test (cheap, tooling already exists):** the `TU_DEBUG` probe
(`setprop debug.canary.tu_debug`, no rebuild) can disable Turnip optimisation
passes. If the orientation changes under any `TU_DEBUG` setting, it is a
driver codegen bug, not an emulator bug - and that is a one-command answer.

---

# `clear_memory_page_state` TESTED on the character collapse — NEGATIVE (2026-08-10)

XenDroid's `GAME_COMPAT.md` documents `clear_memory_page_state = true` as the
fix for *"missing/broken character models"* in Team Ninja titles, explaining it
as: *"the engine reads back GPU-written memory; the page-state refresh makes
those writes visible."* That is the same mechanism our memexport investigation
proposed for the Halo 3 collapse, and we already ship the cvar.

**Result: it does NOT fix the Halo 3 character collapse.** Sierra 117 opening
still renders Marines as smoothed blobs, helmets as balls, Johnson's cigar
detached. User confirmed independently: "still a ball".

**Test was clean and verified, not assumed:**
- Set per-game in `config/4D5307E6.config.toml` under `[GPU]` (category
  verified as `GPU` in `command_processor.cc:47`, so the section is right —
  a wrong section is a *silent* no-op).
- `Loaded game config: .../4D5307E6.config.toml` confirmed in the log **this
  run**. ⚠️ The `clear_memory_page_state = false` line at the top of `xe.log`
  is the **startup dump** and precedes game-config load — it proves nothing.
- The experimental `collapse_ctr_spin_loops` JIT pass was turned **off** for
  this run so it could not confound the result.
- `readback_memexport` / `readback_resolve` confirmed `false`.
- GPU Commands thread at **62.9%**, consistent with the cvar's per-frame
  `SetSystemPageBlocksValidWithGpuDataWritten()` pass actually running.

**What this eliminates.** The "guest RAM gets re-uploaded over GPU-written
pages, clobbering exported vertices" hypothesis is the mechanism this cvar
directly mitigates. It did not help, so either that is not the cause of the
Halo 3 collapse, or the mitigation does not reach the memexport path. Halo 3's
collapse is therefore **not** the same bug as the Team Ninja one, despite the
identical symptom description.

Cost of the test: one config line, no code. Worth it for a clean elimination.

---

# ⭐⭐⭐ RESOLVED AS *OUR* BUG (2026-08-10) — driver hypothesis REFUTED

Installed XenDroid `5ef8fc6` (`xendroid.compose`) on the **same Odin 2**, ran
the **same Halo 3**, and loaded the **same Turnip driver** (copied our
`Turnip_v26.0.0_R8` into their private storage and set `vulkan_lib_path`).

**Turnip load verified in their log, not assumed:**
```
Loading custom Vulkan driver: /data/data/xendroid.compose/driver/vulkan.ad07xx.so
* driverName: turnip Mesa driver
```

**Result: XenDroid's main-menu vista renders PERFECTLY right-side up**, at
**20.6 FPS** (ours ~10-15). Sky and cloud swirl at the top, terrain receding to
a horizon, tower and banshees correctly placed.

## The correction

The previous section of this document concluded:

> "The fork-divergence hypothesis is **not supported**... the weight now sits on
> a **driver-side (Adreno/Turnip) cause**."

**That is now refuted by direct experiment.** Same GPU, same driver, same game,
same device - one emulator gets it right and the other does not. The inversion
is **ours**, in our code.

Why the earlier reasoning failed: the fork diff covered the rectangle-list GS,
`XeResolveDestPixelAddress`, `GetHostViewportInfo` and the resolve/texture-load
shaders, found them equivalent, and I treated "no divergence found in the places
I looked" as "no divergence exists". The divergence is real and simply lives
somewhere I had not diffed.

## Both Halo 3 bugs now have a working reference on our own hardware

| | Canary AE | XenDroid (same device+driver) |
|---|---|---|
| menu vista | **upside down** | **correct** |
| character models | **collapsed to blobs** | **correct** |
| textures | correct | flat/washed |
| FPS | ~10-15 | **20.6** |

Neither bug is a hardware or driver limitation. Both are fixable, both are
ours, and the reference implementation is cloned at
`/home/roman/xeniatest/xendroid-git` for diffing.

⚠️ Config-editing lesson: `vulkan_lib_path` **already existed** in their config.
Blindly appending a second definition made the TOML parser reject the entire
file (`cannot redefine existing string 'vulkan_lib_path'`) and the emulator
silently fell back to defaults. Always edit the existing key. Also: their config
is mode 660 owned by their uid, so a plain `adb shell grep` returns *permission
denied*, which reads like "key not present" - use `su`.

---

# ⭐ 2026-08-11 — SHADER MATH IS IDENTICAL. The flip is in the INPUTS.

Compared our translated SPIR-V against XenDroid's for the same guest shaders
(dump both, join by ucode hash, `spirv-dis`, diff). See
`HALO3_BALL_XENDROID_FIX.md` §10 for the method.

## The final position computation is instruction-for-instruction IDENTICAL

**Ours:**
```
%1331 = OpCompositeConstruct %v3float %1325 %1330
%1333 = OpAccessChain ... %xe_uniform_system_constants %int_4   ; ndc_scale
%1335 = OpFMul %v3float %1331 %1334
%1336 = OpAccessChain ... %xe_uniform_system_constants %int_6   ; ndc_offset
%1338 = OpVectorTimesScalar %v3float %1337 %1319
%1339 = OpFAdd %v3float %1335 %1338
%1340 = OpCompositeConstruct %v4float %1339 %1319
        OpStore  (gl_Position)
```

**Theirs:** the same ops in the same order; only the member indices differ
(`int_5`/`int_7` vs our `int_4`/`int_6`), because their SystemConstants struct
has an extra `vertex_index_count` ahead of them.

So both compute `pos.xyz * ndc_scale + ndc_offset * w`. **The shader is not the
bug.**

## `GetHostViewportInfo` is also logically identical

Diffed ours vs theirs. The only differences in the ndc_scale/ndc_offset paths
are our own `ytest_*` diagnostic toggles, which are **default OFF** and collapse
to exactly their expression:
`pa_cl_vte_cntl.vport_y_scale_ena ? args->PA_CL_VPORT_YSCALE : 1.0f`.
(Ours is 439 lines vs their 378, but the excess is comments + the disabled
experiments.)

## Therefore the divergence is in the INPUTS, not the math

If both the shader and the viewport math are the same, and theirs renders the
vista correctly on the same device and driver, then what differs is **the values
fed in** - the guest register state for that draw (`PA_CL_VTE_CNTL`,
`PA_CL_VPORT_YSCALE`), or which draw/surface the vista is composited through.

A previous session's note in `draw_util.cc` already framed the hypothesis:

> "Normal 3D: D3D9's viewport transform makes the guest's PA_CL_VPORT_YSCALE
> negative (-h/2), giving ndc_scale[1] = -1.0 ... Hypothesis: the vista is
> CLIP-SPACE geometry with the viewport Y scale disabled, so it takes the bare
> 1.0f fallback and gets no flip at all."

That hypothesis is now much stronger, because the shader and the math are
eliminated.

## NEXT STEP (concrete)

Log, for the vista draw specifically: `pa_cl_vte_cntl.vport_y_scale_ena`,
`PA_CL_VPORT_YSCALE`, and the resulting **`ndc_scale[1]`**.

- If `ndc_scale[1] == +1.0` for the vista -> confirmed: it takes the fallback and
  never gets flipped. The fix is then about *why* that draw has the viewport
  transform disabled, or what XenDroid feeds differently upstream.
- If `ndc_scale[1] == -1.0` -> the flip is applied and the fault is downstream
  (composite/sampling), which contradicts the shader evidence and would need
  re-thinking.

⚠️ Eliminated for the vista so far (11): driver/Turnip · resolve row addressing ·
rect-list GS · viewport Y-scale toggles · resolve dest addressing ·
`vulkan_resolve_to_texture` · `vulkan_shared_memory_host_visible` ·
`readback_resolve=full` · `vfetch_bounds_clamp` · `fix_wclip` · `fix_rsq`

## ⭐⭐ MEASURED 2026-08-11 — the two NDC-Y regimes (hypothesis CONFIRMED)

Added a deduplicated diagnostic at the end of `GetHostViewportInfo`
(`debug.canary.ndcy`, default OFF) logging the guest registers and the resulting
`ndc_scale[1]`. Halo 3 main menu:

| `vport_y_scale_ena` | `PA_CL_VPORT_YSCALE` | `ndc_scale[1]` | `extent_y` |
|---|---|---|---|
| **1** | -320 | **-1** | 640 |
| 1 | -320 | -1.333 | 480 |
| 1 | -320 | -2 | 320 |
| 1 | -320 | -4 | 160 |
| 1 | -160 / -80 / -20 / -5 / -0.5 | **-1** | 320 / 160 / 40 / 10 / 1 |
| **0** | -320 / -180 / -156 / -128 | **+0.00024414062** | **8192** |

**Two regimes, and only one gets flipped:**

- `vport_y_scale_ena = 1` -> `ndc_scale[1]` is **negative**. This is the flip
  that converts Xenos Y-up NDC to Vulkan Y-down. Correct.
- `vport_y_scale_ena = 0` -> `ndc_scale[1]` is **positive** (2/8192 with the
  full 8192 extent). **No flip at all.**

So any 3D geometry submitted with the viewport transform disabled renders
Y-inverted relative to everything else - exactly the vista's symptom, with the
2D UI (also `ena=0`, but already screen-space Y-down) unaffected.

⚠️ **Note the fallback is NOT 1.0**, as the old comment assumed - it is
`2.0 / extent` = 0.000244 at extent 8192. Any fix must account for that; simply
negating a presumed 1.0 is wrong, which is likely why the earlier
`ytest_fallback` experiment misbehaved rather than fixing it.

### Why a blanket negation is the wrong fix

The 2D UI also takes `ena=0` and renders correctly today. Negating
`ndc_scale[1]` for all `ena=0` draws would flip the UI as well. The earlier
`ytest_fallback` experiment did roughly that and did not produce a correct
result.

### The remaining question

Both regimes exist in our build AND in XenDroid (same code, proven identical).
So either:

1. The **vista draw lands in a different regime** in their build (i.e. the guest
   is fed different register state upstream, or the draw is routed differently),
   or
2. Both emulators produce the same `ndc_scale[1]` for the vista, and their
   correct result comes from **where the vista is composited**, not from NDC.

**Next:** correlate. Log the NDCY line together with a draw identifier
(shader hash / RT base) so we know *which* regime the vista draw itself is in,
rather than knowing only that both regimes exist.

## ⚠️ REFUTED, one step later — the vista draws ARE flipped

Correlated the NDC-Y regime with the draw that produced it
(`debug.canary.ndcy_draw`, dedup on shader hash + sign). Halo 3 main menu:

- **39 of 42 draws: `flipped=1`, `ndc_scale_y = -1`** - including
  **`9EA48FC2B26C325D`** (the Halo 3 terrain/skinning shader) and
  **`488D9488AB7ED7D8`** (the memexport consumer), both at `extent_y=640`.
- **3 draws only: `flipped=0`**, `ndc_scale_y = +0.000244`, `extent_y=8192`:
  `0A6D1DD7767FDF27`, `C049A8C9E556F129`, `C2543FD5CD52420B` - the classic
  signature of pre-transformed screen-space 2D (the UI), which is correct as-is.

**So the vista's geometry IS receiving the Y flip at the vertex stage.** It is
NOT taking the unflipped fallback.

This **refutes** the "vista is clip-space geometry that never gets flipped"
hypothesis recorded one section above - a hypothesis that had been in
`draw_util.cc` for weeks and that the regime measurement appeared to confirm.
Knowing that two regimes *exist* was not the same as knowing which one the vista
is in; only the per-draw correlation could settle it, and it settled it the
other way.

### What this leaves

Vertex-stage Y is now **eliminated** as the vista's cause, on top of the shader
math and viewport math already proven identical to XenDroid. The inversion must
therefore be **downstream of the vertex stage**:

- how the vista's render target is **resolved**, or
- how the resolved texture is **sampled/composited** into the final image, or
- the orientation of the RT-as-texture handoff.

That is consistent with the very first observation in this document - the 3D
vista is inverted while the 2D UI drawn over it is not - and it points back at
the composite path rather than the geometry path.

**Next:** identify the draw that composites the vista (it will sample the
resolved surface) and compare its texture coordinate handling against
XenDroid's translated shader for the same hash. The SPIR-V dump tooling and the
106-shader common set are already in place for exactly this.

## ⭐⭐ 2026-08-11 — the composite shaders are translated with DIFFERENT modification bits

The 3 unflipped, screen-space (`extent_y=8192`) draws are the composite/UI
candidates. Comparing the dumped filenames, which carry the shader modification:

| shader | ours | theirs |
|---|---|---|
| `0A6D1DD7767FDF27` | `0000000000000000` | `0000000000000000` |
| **`C049A8C9E556F129`** | `0000000000000000` | **`0000000012000000`** |
| **`C2543FD5CD52420B`** | `0000000000000000` | **`0000000012000000`** |

Decoding their vertex `Modification` bitfield
(`spirv_shader_translator.h`): bits 0-15 `interpolator_mask`, 16
`output_point_parameters`, 17-24 `dynamic_addressable_register_count`,
**25-27 `host_vertex_shader_type`**, **28-30 `user_clip_plane_count`**,
31 `user_clip_plane_cull`.

`0x12000000` = bit 25 + bit 28:

- **`host_vertex_shader_type = 1`** (`kDomainStart` / `kLineDomainCPIndexed` -
  i.e. NOT plain `kVertex`)
- **`user_clip_plane_count = 1`**

**We emit `0` for both.** We have no `user_clip_plane_count` field in our
Modification struct at all (0 references vs 8 in theirs, measured earlier).

### Why this matters for the vista

These are exactly the draws that composite the scene, and they are the ones we
translate differently. Two same-hash guest shaders are being turned into
materially different host shaders - a different pipeline stage/input
configuration and one enabled user clip plane on their side, neither on ours.

This is a far better fit than anything eliminated so far: the geometry path is
proven identical and proven flipped, so the remaining difference has to be in
how the composite is set up - which is precisely what these bits control.

### Next

1. Find where XenDroid computes `user_clip_plane_count` and
   `host_vertex_shader_type` for these draws (`GetVertexShaderModification` or
   equivalent) and compare against ours.
2. Port `user_clip_plane_count` / `user_clip_plane_cull` into our Modification
   struct plus the `gl_ClipDistance` output support (both confirmed absent).
3. Re-test the vista at the menu - the fast loop.

⚠️ Keep in mind this is a **correlation** so far: these shaders differ AND the
vista is wrong. Confirm causally (toggle) before declaring it fixed - three
"confirmed" hypotheses have already been refuted in this document.

## ⭐⭐⭐ CONFIRMED GAP: we ignore guest user clip planes on Vulkan

Traced where the `0x12000000` modification comes from. XenDroid
(`guest_spirv_shader_cache.cc:68-74`):

```cpp
auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
uint32_t user_clip_planes =
    pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
modification.vertex.user_clip_plane_count = xe::bit_count(user_clip_planes);
modification.vertex.user_clip_plane_cull =
    uint32_t(user_clip_planes && pa_cl_clip_cntl.ucp_cull_only_ena);
```

**We never consult `ucp_ena` anywhere in the Vulkan pipeline cache.** The guest
enables a user clip plane for the composite draws and our Vulkan backend
silently ignores it - geometry that the guest expects to be clipped is not
clipped.

### We already have the feature - just not on Vulkan

`dxbc_shader_translator.h:151` has `uint32_t user_clip_plane_count : 3;` and the
clip/cull accounting at :190-193. So **upstream Xenia implements user clip
planes for D3D12**, XenDroid implemented them for **Vulkan**, and our Vulkan
path never received them. This is a port, not an invention, and there are two
reference implementations (our own DXBC one for the semantics, theirs for the
SPIR-V emission).

### Implementation checklist

1. `spirv_shader_translator.h` - add `user_clip_plane_count : 3` and
   `user_clip_plane_cull : 1` to the vertex `Modification` struct (mirror the
   DXBC layout).
2. `vulkan_pipeline_cache.cc` - compute both from `PA_CL_CLIP_CNTL` exactly as
   above when building the vertex modification.
3. `spirv_shader_translator.cc` - declare and write `gl_ClipDistance`
   (`output_per_vertex_clip_distance_member_index_`, 0 refs ours / 6 theirs) and
   emit the per-plane distance; handle the cull-distance variant.
4. `vulkan_device.cc` - ensure the `shaderClipDistance` feature is enabled.
5. Gate behind a cvar, default OFF, and A/B the vista at the **menu**.

⚠️ Still correlation until toggled: these draws differ AND the vista is wrong.
Confirm causally - three "confirmed" hypotheses have already been refuted here,
one of them within two turns tonight.

## ⭐⭐⭐ ROOT GAP IDENTIFIED: upstream's own unfinished TODO, which XenDroid completed

`vulkan_pipeline_cache.cc:970-980` in our tree:

```cpp
// TODO(Triang3l): Once all needed inputs and outputs are added, uncomment the
// real counts here.
key.interpolator_count =
    xe::bit_count(vertex_shader_modification.vertex.interpolator_mask);
key.user_clip_plane_count =
    /* vertex_shader_modification.vertex.user_clip_plane_count */ 0;
key.user_clip_plane_cull =
    /* vertex_shader_modification.vertex.user_clip_plane_cull */ 0;
key.has_vertex_kill_and =
    /* vertex_shader_modification.vertex.vertex_kill_and */ 0;
```

**Upstream Xenia's Vulkan backend hardcodes user clip planes (and vertex kill)
to zero with an explicit TODO.** It is unfinished, not broken by us - the D3D12
path has had the feature all along. **XenDroid completed it**, which is why the
same guest shader is translated with `user_clip_plane_count = 1` there and `0`
here, and why their Halo 3 composite is correct.

This is the cleanest explanation yet for the whole class of Halo 3 geometry
problems: the guest asks for clipping/culling that our Vulkan backend silently
drops.

### Port status

- [x] `spirv_shader_translator.h` - `user_clip_plane_count : 3` and
      `user_clip_plane_cull : 1` added to the vertex `Modification`. Builds
      clean; the bits read 0 everywhere so behaviour is unchanged so far.
- [ ] Compute both from `PA_CL_CLIP_CNTL` (`ucp_ena`, `clip_disable`,
      `ucp_cull_only_ena`) where the vertex modification is built
      (`vulkan_pipeline_cache.cc` ~line 276).
- [ ] Emit `gl_ClipDistance` in `spirv_shader_translator.cc`
      (`output_per_vertex_clip_distance_member_index_`) - the largest piece.
- [ ] Un-stub the geometry-shader key above.
- [ ] Enable the `shaderClipDistance` device feature.
- [ ] Cvar gate, default OFF, then A/B the vista at the menu.

⚠️ `vertex_kill_and` is stubbed on the same lines and is a *separate* feature -
port clip planes first and keep them independent so each can be A/B'd.

## Clip-plane port COMPLETE - and the test is INCONCLUSIVE, not negative

Full port landed (Modification fields, PA_CL_CLIP_CNTL computation,
`user_clip_planes` SystemConstants member with `kVersion` 6->7, gl_ClipDistance
/gl_CullDistance declaration + per-plane `dot(clip_space_position, plane)`
writes before the NDC transform, geometry-shader key un-stubbed;
`shaderClipDistance` was already enabled).

Enabled `vulkan_user_clip_planes = true` on Halo 3: **menu renders, 0 errors,
15 FPS, vista STILL INVERTED.**

**But the feature never engaged.** Dumping our shaders with the cvar on, **every
modification value has bits 28-30 clear** - `user_clip_plane_count == 0` for all
99 non-trivial shaders. Halo 3 does not set `ucp_ena` on these draws in our
build, so nothing was clipped and the test says nothing about whether clip
planes would fix the vista.

⚠️ Do not record this as "clip planes don't fix the vista". It is untested.

## ⭐ The much bigger signal in the same data: DOMAIN vs VERTEX shader

XenDroid's modification for the two composite shaders was **`0x12000000`**:

- bit 28 -> `user_clip_plane_count = 1`
- **bit 25 -> `host_vertex_shader_type = 1`**

`host_vertex_shader_type = 1` is **`kDomainStart` / `kLineDomainCPIndexed`** -
a **tessellation domain shader**. Ours translates the same guest shaders as
plain **`kVertex`** (type 0).

**So XenDroid runs these composite draws through the tessellation/domain-shader
path and we run them as ordinary vertex shaders.** That is a different pipeline
stage, not a flag - and it is a far larger divergence than clip planes. It also
explains why enabling clip planes changed nothing: the draws never reach the
configuration where they matter.

This connects to known unfinished work here: our tessellation phases 1-5 were
implemented but **Phase 6 (on-device visual verification) was never completed**
([[project-xenia-ae-tessellation-gap]]). If our host_vertex_shader_type
selection falls back to kVertex where it should pick a domain type, the vista
would be assembled by the wrong pipeline stage entirely.

**NEXT (highest value):** find where `host_vertex_shader_type` is chosen
(`GetHostVertexShaderTypeIfValid` / the primitive processor) and compare against
XenDroid for these two shaders. That is now the single best lead for the vista.

## ⚠️ The 0x12000000 decode needs verifying before it is acted on

Follow-up checks tightened the picture but also raised a contradiction:

- Both emulators dump the **same shaders with the same low bits**
  (`C049A8C9E556F129` at `...0000` and `...0001`, `C2543FD5CD52420B` at
  `...0000`). Theirs consistently carries `0x12000000` on top; ours carries
  nothing. So these are the **same draws**, not different variants.
- **But `primitive_processor.cc`'s `host_vertex_shader_type` selection is
  byte-identical between the two trees** (normalised diff: zero differences on
  every host_vertex_shader_type / tessellation / domain line).

Identical selection code cannot produce different `host_vertex_shader_type`
values from the same guest state. So one of these is true:

1. **The bit decode is wrong.** Bits 25-27 were read as
   `host_vertex_shader_type` using *their* struct layout. If their vertex
   `Modification` has any extra or differently-sized field before it (their
   struct does carry more members overall - `tessellation_mode`,
   `vertex_kill_and`, ...), every position shifts and `0x12000000` means
   something else entirely.
2. The inputs differ upstream (guest register state feeding the selection).

**Verify before acting:** decode `0x12000000` against *their* actual
`Modification` bitfield field-by-field (count the bits in their
`spirv_shader_translator.h` declaration order), rather than assuming our layout
maps onto theirs. Our own dumps now carry modification values too, so the same
decode can be sanity-checked against a known-type shader on our side.

This is exactly the failure mode that has produced several wrong "confirmed"
calls in this document - a plausible reading of a number, acted on before the
encoding was checked.

---

# ⭐⭐⭐ 2026-08-11 XDTESTER: first hard divergence found — the 0x04D20000 resolve

Built **XDtester** (XenDroid from source, `xendroid.compose.xdtester.debug`,
instrumented, installed alongside the stock build) and ran the SAME probes in
both emulators. Baseline verified faithful first: Halo 3 vista renders correctly
on Turnip R8, 17.9 FPS, 0 errors.

## Result 1 — the NDC path is ELIMINATED

`NDCYDRAW` in both builds, same scene:

| | XenDroid | Canary AE |
|---|---|---|
| flipped (`ndc_scale_y=-1`) | 41 | 39 |
| unflipped (`+0.000244`, extent 8192) | **3** | **3** |
| the unflipped shaders | `0A6D1DD7767FDF27`, `C049A8C9E556F129`, `C2543FD5CD52420B` | **identical three** |

Same shaders, same regimes, same values. Their vista is right and ours is wrong
while both feed **identical `ndc_scale[1]` into identical shader code**.

This also **kills the `0x12000000` decode** flagged as unverified above: if their
composite draws had a different `host_vertex_shader_type`, it would appear here.
It does not. That decode was wrong.

## Result 2 — the resolve lists differ in exactly ONE entry

`VISTA ENUM` in both, Halo 3 menu. Every resolve matches except:

| | Canary AE | XenDroid |
|---|---|---|
| base `0x04D20000`, fmt=22, **depth=1** | **336x336**, len=700416, order **n=25** | **512x512**, len=1048576, order **n=4** |

Same address, same format, **different geometry**, and a completely different
position in the frame - theirs resolves it 4th, ours 25th (i.e. last).

512x512 is a power of two (shadow map / cubemap face shape); 336x336 is not.
`336 = 42*8` and `512 = 64*8`, so this comes from
`coordinate_info.width_div_8` / `height_div_8` differing - the resolve
RECTANGLE is being computed differently, not just the destination.

**This is the first hard, measured divergence in the vista path**, and it is a
depth resolve - consistent with a deferred scene being composited wrongly.

## Next

1. Instrument `GetResolveInfo` in both builds to log the source registers
   (`RB_COPY_DEST_PITCH`, the resolve rectangle, `rb_copy_control`) for the
   `0x04D20000` resolve and find why the rectangle differs.
2. Check ordering: does theirs resolve it before the vista composite and ours
   after? A depth surface resolved too late would be sampled stale.

⚠️ Correlation until proven: same-address/different-size is a strong signal but
the causal link to the inversion is not yet shown.

## Analysis of the 0x04D20000 divergence — two real code differences, one caveat

Diffed `GetResolveInfo` (`draw_util.cc`) ours vs XenDroid:

**1. Degenerate-rectangle handling differs.**
```
ours:    assert_true(x0 <= x1 && y0 <= y1);   ... return false;   // DROPS the resolve
theirs:  info_out.coordinate_info.width_div_8 = 0;
         info_out.height_div_8 = 0;           ... return true;    // keeps it, zero-sized
```
We **drop** a resolve whose rectangle is degenerate; they **keep** it with a zero
size. A dropped resolve means a surface never gets written at all.

**2. Aligned vs raw destination pitch.**
```
ours:    texture_util::GetTiledOffset2D(..., rb_copy_dest_pitch.copy_dest_pitch, ...)   // RAW
theirs:  xe::align(copy_dest_pitch, kStoragePitchHeightAlignmentBlocks) -> aligned      // ALIGNED
```
They align the pitch/height before computing tiled offsets; we pass the raw
register value. That changes the destination ADDRESS arithmetic.

They also track `copy_dest_x0` / `copy_dest_y0` (the rect origin) as explicit
fields, which we do not have at all.

### ⚠️ Caveat on the 336x336 vs 512x512 reading

The `VISTA ENUM` probe **dedups on base address and logs only the FIRST resolve
to each**. If Halo 3 resolves `0x04D20000` several times with different
rectangles, then ours logging 336x336 at n=25 and theirs 512x512 at n=4 may be
the SAME set of resolves observed in a DIFFERENT ORDER - not different geometry.

**Do not act on "the rectangle is computed differently" until that is
distinguished.** Change the probe to log EVERY resolve to `0x04D20000` (not just
the first) in both builds and compare the full sequences. That is a one-line
change to the dedup condition and settles it.

The ordering difference (n=4 vs n=25) is real either way and is worth
understanding on its own: a depth surface resolved last rather than early could
be sampled stale by the composite.

## Both resolve-path differences TESTED — neither fixes the vista

Ported and tested the two concrete `GetResolveInfo` divergences found by diffing
against XenDroid:

| change | toggle | result |
|---|---|---|
| Bail out on empty/inverted resolve rect (we had only `assert_true`, a NO-OP in release) | `resolve_rect_guard` | **Never triggers** - 0 occurrences on Halo 3. Cannot be the cause. |
| Pass the ALIGNED `copy_dest_pitch` to the tiled-offset helpers instead of the RAW value | `resolve_aligned_pitch` | Active, 0 errors, 15 FPS - **vista still inverted**. |

Both are kept (default OFF): each matches upstream/XenDroid and is more correct
than what we had. Neither is the vista fix.

### Where the vista search now stands

Eliminated **by direct measurement against a working reference** (XDtester):
driver/Turnip · resolve row addressing · rect-list GS · viewport Y math ·
resolve destination addressing · `vulkan_resolve_to_texture` ·
`vulkan_shared_memory_host_visible` · `readback_resolve` · `fix_wclip` ·
`fix_rsq` · `vfetch_bounds_clamp` · user clip planes · **NDC scale/offset
(identical per-draw in both builds)** · **vertex-stage Y (the vista draws DO
receive the flip)** · **the translated shader position math (instruction-
identical)** · resolve-rect degeneracy · resolve tiling pitch.

That leaves, concretely:

1. **The `0x04D20000` resolve** - ours 336x336 at n=25, theirs 512x512 at n=4.
   ⚠️ STILL UNRESOLVED whether that is different geometry or the same resolves
   in a different ORDER, because the probe dedups on base address. **Remove the
   dedup for that address in both builds and compare the full sequences.** This
   is the single most concrete unexplained difference and it has not been
   properly measured yet.
2. **How the resolved surface is SAMPLED during composite** - never instrumented.
   The geometry path is now exhausted; the read side is not.

⚠️ Note the pattern: every geometry/vertex-path hypothesis has failed. The
evidence says the vertices are right and the flip happens when the resolved
image is consumed. Instrument the composite's texture fetch next, not more of
the vertex path.

## ⭐⭐⭐ 2026-08-11 STRONGEST LEAD: write/read size mismatch at 0x04D20000

Added a READ-SIDE probe (`debug.canary.texbind`, in `TextureCache::RequestTextures`,
logging what each texture fetch constant actually resolves to). First run on the
Halo 3 menu:

```
TEXBIND slot=1 base=0x04D20000 512x512 dim=1 tiled=1 fmt=22 signs=00
```

Compare against the RESOLVE to the same address:

| | resolve (WRITE) | texture binding (READ) |
|---|---|---|
| **Canary AE** | `0x04D20000` **336x336** | `0x04D20000` **512x512** |
| **XenDroid** | `0x04D20000` **512x512** | same address |

**We write a 336x336 region and then sample it as a 512x512 TILED surface.**
XenDroid writes 512x512 and reads 512x512 - self-consistent.

Xenos tiling derives each row's address from the surface dimensions, so reading
a tiled surface at 512 wide when only 336 was written mis-addresses every row.
That is a coherent-but-wrong image, which matches the symptom far better than
anything eliminated so far.

It also **resolves the earlier dedup caveat**: the binding is consistently
512x512 while our resolve was 336x336, so the sizes genuinely differ - this is
not an ordering artifact of the enumerator probe.

⚠️ Still correlation until proven causally. Confirm by making the resolve
produce 512x512 (i.e. find why `width_div_8`/`height_div_8` come out 42x42
instead of 64x64 for this resolve) and re-testing the vista.

**This is the first divergence found on the READ side, and the first that is
internally inconsistent within our own build rather than merely different from
theirs.** Next: instrument `GetResolveInfo`'s rectangle inputs for the
`0x04D20000` resolve specifically (the source registers and the scissor clamp)
and find where 512 becomes 336.

⚠️ Probe note: the TEXBIND dedup key collides (many repeated lines). Harmless
for this result but tighten the key before reusing it.
