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
