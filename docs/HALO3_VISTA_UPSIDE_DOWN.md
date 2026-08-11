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
