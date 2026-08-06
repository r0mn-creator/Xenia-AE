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
