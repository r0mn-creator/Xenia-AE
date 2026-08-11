# How XenDroid fixed the Halo 3 "ball" (character collapse)

> ## ⚠️ CORRECTION 2026-08-10 — §2 BELOW IS WRONG FOR OUR HARDWARE
>
> §2 says the fix is **two-buffer routing via `VK_EXT_external_memory_host`**.
> That mechanism is real and is in their code, **but it is INACTIVE on Adreno.**
>
> Verified on-device: XenDroid's own log on our Odin 2 lists its enabled device
> extensions as `dynamic_rendering_local_read`, `custom_border_color`,
> `extended_dynamic_state3`, `memory_budget`, `non_seamless_cube_map`,
> `shader_stencil_export`, `swapchain` — **no `external_memory_host`**. Their
> own source comment says it outright: *"wherever guest RAM cannot be imported
> (no VK_EXT_external_memory_host, i.e. **every Adreno**)"*.
>
> So `host_buffer_` is `VK_NULL_HANDLE` on our device and every host-routed path
> early-returns. **The two-buffer design cannot be what makes their models
> render correctly here.**
>
> ### The actually-active mechanism: UMA host-mapped shared memory
>
> `ReadbackResolveMode::kUma` — *"Read host-mapped shared memory directly, no
> device->host copy. Adreno cannot import guest RAM ... so upstream's zero-copy
> never engages there; **this is the equivalent for such devices**."*
>
> Supported by the cvar **`vulkan_shared_memory_host_visible`** (we lack it),
> which allocates the shared-memory buffer in host-visible memory — viable on a
> mobile UMA part where CPU and GPU share physical memory. The CPU then reads
> GPU-written data straight out of that mapping instead of a device→host copy,
> so exported vertices and guest RAM never diverge.
>
> **Correct port targets are therefore:** `vulkan_shared_memory_host_visible`,
> a `uma` mode for `readback_resolve`, and `readback_resolve_sync` — none of
> which need the extension.
>
> Keep §2 for reference (it is what they do on desktop/UMA-import hardware), but
> **do not port it as the ball fix**.
>
> **How this was caught:** the extension plumbing was ported and built FIRST,
> then checked on-device before writing any buffer code. The enabled-extension
> list did not contain it, and XenDroid's own log confirmed the same. Verifying
> the dependency before building on it is what stopped a large wasted port.


**Status 2026-08-10:** mechanism identified from their source and git history.
Not yet ported. This document is the reference for doing that port.

**Directive:** keep all Canary AE UI/UX (settings, per-game config, custom GPU
driver UI, patches UI — all things XenDroid does *not* have). Replace only the
rendering internals.

---

## 1. Proof the bug is ours and fixable

Same Odin 2, same Halo 3, same stock Qualcomm driver, same Sierra 117 scene:

| | Canary AE | XenDroid `5ef8fc6` |
|---|---|---|
| character models | collapsed to blobs | **correct** |
| menu vista | upside down (Turnip) | **correct** |
| Sierra 117 FPS | ~10 | **17.7** |
| menu FPS | ~10-15 | **20.6** |
| faces | **flicker too** (user-confirmed 2026-08-10) | **flicker** (measured: 79-109% luminance swing on faces vs 0.3-1.0% on static scene) |

Neither Halo 3 bug is hardware, Adreno or Turnip. See
`HALO3_VISTA_UPSIDE_DOWN.md` and `XENDROID_FULL_COMPARISON.md` §10/§10a.

---

## 2. THE FIX: two-buffer memexport routing

### What we do today (the broken design)

Memexport (render-to-vertex-buffer skinning) writes vertices from a *producer*
vertex shader into memory; a *consumer* vertex shader reads them back. Our path
writes into the **device-local shared-memory buffer**, and keeps guest RAM in
sync by **CPU readback** — the `readback_memexport` cvar.

That is inherently racy. `SharedMemory` marks pages `gpu_written` so they are
not re-uploaded; if those pages go invalid again between the producer's write
and the consumer's fetch, `RequestRange` uploads guest RAM back over them and
the exported vertices are replaced with whatever the CPU last had (typically
zeros). The consumer then reads zeros → vertices converge on a point → **ball**.

`readback_memexport` also forces mid-frame GPU sync, which is why it is a large
performance cost (see §5).

### What XenDroid does instead

They **deleted the readback path** and replaced it with **two buffers**:

```
normal draws          -> shared_memory_->buffer()        (device-local, fast)
memexport-touching    -> shared_memory_->host_buffer()   (imported guest RAM)
```

`host_buffer_` is created with **`VK_EXT_external_memory_host`**, importing the
guest's own RAM pages directly as a `VkBuffer`. Their comment states the point
exactly:

> *"A host-imported (guest RAM) copy of the buffer... Bound instead of buffer()
> for memexport-touching draws so their output is coherent with the CPU (no
> clobber) — it aliases guest RAM."*

Because the memexport buffer **is** guest RAM, there is no divergence to
reconcile: no readback, no sync, no clobber. The whole class of bug disappears
rather than being mitigated.

Implementation shape (`vulkan_command_processor.cc`):
- **Two descriptor sets** are allocated when the host buffer exists — one bound
  to `buffer()`, one to `host_buffer()` (`shared_memory_set_count = 2`).
- At draw time the memexport set is selected, and the **index buffer is routed
  too**: `index_buffer.first = route_to_host ? host_buffer() : buffer()`.
- Gated on device support: bails out if
  `!vulkan_device->extensions().ext_EXT_external_memory_host`
  (`vulkan_shared_memory.cc:326`). Adreno/Turnip supports it.
- They first tried **full zero-copy** (alias guest RAM as *the* only buffer) and
  kept it as a separate path (`zero_copy_`); the shipped design is the
  **hybrid two-buffer** one, because device-local memory is much faster for the
  99% of draws that are not memexport.

### The routing decision — and the bug they hit doing it

`3a0e2e1c4 [Vulkan] Route memexport draws by shader, not by parsed stream ranges`

```cpp
// Keyed on the shader, not on memexport_ranges_ - AddMemExportRanges drops
// streams the shader's own weaker guard still stores to, leaving those
// writes to hit sparse pages nothing ever committed.
const bool memexport_used =
    (vertex_shader->memexport_eM_written() != 0 &&
     device_properties.vertexPipelineStoresAndAtomics) ||
    (pixel_shader && pixel_shader->memexport_eM_written() != 0 &&
     device_properties.fragmentStoresAndAtomics);
```

**Route on what the shader actually writes, not on parsed stream ranges.** The
CPU-side range parser is *stricter* than the shader's own guard, so ranges get
dropped while the shader still stores to them — those writes then land on sparse
pages nothing committed. Worth knowing before porting: the naive "use the parsed
ranges" version is wrong and they fixed it five days later.

---

## 3. The full commit series to port (chronological)

| commit | date | what |
|---|---|---|
| `f7d6cc87e` | 2026-07-14 | **[Vulkan] Replace memexport readback with two-buffer routing** — the core change. 769 lines in `vulkan_command_processor.cc` (net *removal*), plus `vulkan_shared_memory.{cc,h}`, `vulkan_texture_cache.cc`. |
| `58d321b56` | | [D3D12] same for D3D12 + removes the readback path (skip — not our target) |
| `198021076` | | [GPU] Add `memexport_enable` and `memexport_await_fences` |
| `39a2e8510` | | [GPU] Skip device-buffer upload for host-routed memexport streams |
| `3a0e2e1c4` | 2026-07-19 | **[Vulkan] Route memexport draws by shader, not parsed stream ranges** (fixes the above) |
| `d310fb887` | 2026-07-19 | [GPU] Refresh memexport ranges in the shared memory buffer **on the GPU** |
| `68caed562` | 2026-07-19 | [GPU] Fix memexport host buffer **barriers** and stale GPU-authoritative marking |
| `a4a188b0c` | | [GPU] Match memexport stream validation between `draw_util` and the shader |
| `935935005` | | [Vulkan] barrier readback buffer writes to avoid torn data on slot reuse |
| `51102c7e6` | | [Vulkan] extended range f32→f16 in memexport — **we already have this** (kept from an earlier session) |

Port order: `f7d6cc87e` first (the architecture), then `3a0e2e1c4` +
`68caed562` + `d310fb887` (the three correctness fixes on top), then the cvars.

⚠️ Their commit messages have **no bodies** — read the diffs.

---

## 4. What this means for our tree

- **`readback_memexport` becomes obsolete.** It is one of our 15 "unique" cvars;
  it is unique because they *deleted* the concept, not because we added value.
- Requires `VK_EXT_external_memory_host`. Check our
  `vulkan_device->extensions()` plumbing exposes it.
- Touches `vulkan_shared_memory.{cc,h}` (426 substantive lines diverged),
  `vulkan_command_processor.{cc,h}` (6304 — but most is unrelated Vulkan work),
  `shared_memory.{cc,h}` (237).
- **Do NOT wholesale-adopt their texture path.** Their faces flicker and ours do
  not; that is the one Halo 3 thing we currently do better. Port the memexport
  /shared-memory change narrowly.

---

## 5. ⚠️ Benchmarking note discovered alongside this

`readback_memexport = true` and `readback_resolve = 'full'` were left enabled in
our device config from earlier investigations. Both force mid-frame GPU sync.
Turning both off took the Halo 3 menu from **~10 FPS to 15 FPS**.

**The documented 9.67 FPS baseline was contaminated.** Assert both are `false`
before any benchmark. Once the two-buffer path lands, `readback_memexport`
should be gone entirely.

---

## 6. Tested and REFUTED — do not retry

- **`clear_memory_page_state = true`** (their documented Team-Ninja "missing
  character models" fix, which we already ship) does **not** fix our ball.
  Verified clean: per-game `[GPU]` section, `Loaded game config` confirmed
  in-log that run, JIT pass off, `readback_*` off, GPU Commands at 62.9%
  showing the per-frame pass was really running.
  So their fix is **not** that cvar — it is the two-buffer routing above.
- The "guest RAM re-uploaded over `gpu_written` pages" hypothesis is what
  `clear_memory_page_state` mitigates, and it did not help. The two-buffer
  design removes the divergence entirely rather than mitigating it, which is
  presumably why it succeeds where the mitigation fails.

---

## 7. Reference

- Their tree: `/home/roman/xeniatest/xendroid-git` (14,978 commits, full history)
- Key files: `gpu/vulkan/vulkan_shared_memory.{cc,h}`,
  `gpu/vulkan/vulkan_command_processor.{cc,h}`, `gpu/shared_memory.{cc,h}`
- Our mechanism analysis of the bug: `HALO3_MEMEXPORT_READBACK.md` and
  memory `project-xenia-ae-halo3-memexport-mechanism`
- Comparison + ranked port list: `XENDROID_FULL_COMPARISON.md`

---

# 8. BISECT ON THEIR WORKING BUILD (2026-08-10) — both my hypotheses eliminated

Rather than reading code and guessing, flipped cvars in **XenDroid's own config
on the Odin 2** and observed. This is the strongest available method: a working
reference we can perturb.

## Their shipped values (the working baseline)

```
readback_resolve                 = "uma"
readback_resolve_sync            = true
vulkan_resolve_to_texture        = true   (+_promote, +_serve)
vulkan_shared_memory_host_visible = true
memexport_enable                 = false      <-- NOTE
clear_memory_page_state          = false
```

## Results

| change (all else theirs) | result |
|---|---|
| all of readback_resolve=none, resolve_to_texture(+promote+serve)=false, host_visible=false | **BLACK SCREEN** (audio only) |
| `vulkan_shared_memory_host_visible = false` only | **renders fine, models still CORRECT**, 31.7 FPS |
| `readback_resolve = "none"` only | **BLACK SCREEN** |

## What this eliminates

1. **`VK_EXT_external_memory_host` two-buffer routing** — the extension is not
   even enabled on Adreno (their own log; their own comment says "every
   Adreno"). Dead code here.
2. **`vulkan_shared_memory_host_visible` / the UMA host-mapped buffer** —
   turning it OFF leaves their models **correct**. Not the ball fix.
3. **memexport CPU-visibility routing** — `memexport_enable = false` in their
   shipped config, and models render correctly. **The ball is not a memexport
   CPU-visibility problem**, which undermines the long-standing assumption in
   `HALO3_MEMEXPORT_READBACK.md` / the memexport-mechanism memory.

## What remains

`readback_resolve = "uma"` is **load-bearing** — setting it to `none` alone
black-screens their renderer. Their resolve path evidently *depends* on readback
being active in that mode, whereas ours runs with readback off entirely.

⚠️ **`full` is NOT equivalent to `uma`.** `full` = GPU stall + device->host copy.
`uma` = read the host mapping directly, no copy. Tested `readback_resolve="full"`
on our build: **vista still inverted**. That does not test their mechanism,
because we do not have `uma`.

**Next candidate: `vulkan_resolve_to_texture` (+`_promote`, +`_serve`)** — not
yet isolated. It was in the group that black-screened, and has not been tested
alone. Test that next before writing any code.

## Bisect CONCLUSION — the ball fix is in their CODE, not their config

Completed the sweep on their working build:

| change (all else theirs) | vista | models | note |
|---|---|---|---|
| `vulkan_shared_memory_host_visible = false` | good | **correct** | 31.7 FPS |
| `vulkan_resolve_to_texture(+promote+serve) = false` | good | **correct** | 20.6 FPS |
| `readback_resolve = "none"` | — | — | **BLACK SCREEN**, cannot evaluate |
| all of the above together | — | — | **BLACK SCREEN** |

**Every config knob that can be turned off without breaking rendering leaves
their models correct.** `readback_resolve` cannot be evaluated because disabling
it black-screens their renderer entirely (it is load-bearing for them; our
renderer runs fine with readback off).

**Therefore: the Halo 3 character-collapse fix is a CODE difference, not a
setting.** No amount of config porting will fix our ball.

### Hypotheses eliminated by direct experiment (do not revisit)

1. `VK_EXT_external_memory_host` two-buffer routing — extension not enabled on
   any Adreno; dead code on our hardware.
2. `vulkan_shared_memory_host_visible` / UMA host-mapped buffer — off, models
   still correct.
3. `vulkan_resolve_to_texture` family — off, models still correct.
4. memexport CPU-visibility routing — their `memexport_enable = false` in the
   shipped config, models still correct. **The ball is not a memexport
   CPU-visibility problem.**
5. `clear_memory_page_state` — tested on our build, no effect (§6).
6. `readback_resolve = "full"` on our build — ball unchanged, vista still
   inverted. (Not equivalent to their `uma`, so their mode remains untested,
   but `full` is not the fix.)

### Where to look next

The difference is in the Vulkan GPU code. Ranked by substantive divergence
(churn-normalised, see `XENDROID_FULL_COMPARISON.md`):
`vulkan_command_processor.cc` (6304 lines), `vulkan_render_target_cache.cc`
(3509), `vulkan_pipeline_cache.cc` (3241), `spirv_shader_translator.cc` (1772),
`vulkan_texture_cache.cc` (1241), `spirv_shader_translator_rb.cc` (1079).

Since the collapse is per-vertex (vertices converging to a point), the
**shader translator** and **primitive/vertex path** are better targets than the
render-target/resolve code, despite the latter being where the vista lives.

### Also observed: we are better on colour

User comparison, same scene: **"our colors look better"** than XenDroid's, whose
Halo 3 output is flatter/washed. Preserve that when porting — go narrowly into
the vertex/geometry path, do not adopt their texture/colour handling wholesale.

⚠️ **CORRECTION 2026-08-10:** an earlier version of this document claimed our
faces are stable while theirs flicker. **That was wrong.** User confirmed
**our faces flicker as well**. The claim was never observed — our characters
render as blobs, so there are no faces to judge; "stable" was inferred from the
absence of evidence, which is not evidence. **The face flicker is a SHARED bug,
not a Canary AE advantage.** Lower priority than the ball.

---

# 9. CODE DIFF, vertex path (2026-08-10) — first concrete find

## Vertex-fetch bounds clamping: they have it, we do not

`spirv_shader_translator_fetch.cc`. XenDroid tracks a fetch end bound
(`var_main_vfetch_bound_`, derived from fetch constant word 1) and **guards
every shared-memory word load**:

```cpp
// Words at or past the end of the fetch buffer read as 0, matching the
// hardware's bounds clamping. Games rely on this - e.g. an over-allocated
// quad-list particle draw whose inactive vertices fetch 0 and collapse to a
// degenerate (zero-area) primitive instead of exploding to garbage.
spv::Id loaded_word = LoadUint32FromSharedMemory(word_address);
spv::Id word_in_bounds = builder_->createBinOp(spv::OpULessThan, type_bool_,
                                               word_address, fetch_end);
word_composite_constituents[word_count++] = builder_->createTriOp(
    spv::OpSelect, type_uint_, word_in_bounds, loaded_word, const_uint_0_);
```

Ours loads unconditionally:

```cpp
word_composite_constituents[word_count++] =
    LoadUint32FromSharedMemory(word_address);
```

...with a comment acknowledging the hardware behaviour but not implementing it.

**This is a genuine hardware-accuracy gap on the vertex path and is worth
porting on its own merits.**

⚠️ **But it is probably NOT the ball fix, and the polarity is why.** Their clamp
makes out-of-range fetches return **zero**; without it we read **garbage**.
Our symptom is vertices collapsing to a point, which is the *zero* signature,
not the garbage signature. Adding the clamp would make zero-reads *more* common,
not fewer. Flagging this explicitly to avoid a third premature "found it" —
port it as a correctness fix, verify against other titles, and do not expect it
to fix Halo 3.

Still to diff on the vertex path: `spirv_shader_translator.cc` (1772 divergent
lines, includes the position/W handling), `primitive_processor.cc` (100),
`vulkan_primitive_processor.cc` (23).

## Vertex/shader-path feature gaps (measured 2026-08-10)

| feature | ours | theirs | |
|---|---|---|---|
| `var_main_vfetch_bound_` (vertex-fetch bounds clamp) | 0 | 5 | **MISSING** |
| `var_main_rect_list_guest_positions_` | 0 | 7 | **MISSING** |
| `user_clip_planes` | 0 | 2 | **MISSING** |
| `output_per_vertex_clip_distance_member_index_` (gl_ClipDistance) | 0 | 6 | **MISSING** |
| `var_main_point_size_edge_flag_kill_vertex_` | 8 | 12 | partial |
| `memexport_eM_written` | 5 | 5 | parity |

**We are missing user clip planes / `gl_ClipDistance` entirely**, plus rect-list
guest-position tracking and the fetch bounds clamp. These are structural
vertex-pipeline features, not tweaks — consistent with the user's read that
XenDroid is simply further along on rendering.

## ⚠️ XenDroid regressions we must NOT inherit (user-observed 2026-08-10)

XenDroid is better overall (most games playable incl. Halo 3), **but has more
graphical issues than Canary AE**:

- **Halo 3: flickering textures** (faces; measured 79-109% luminance swing).
  ⚠️ Correction: **Canary AE flickers too** — an earlier claim that ours were
  stable was wrong and never observed (our characters are blobs, so there were
  no faces to judge).
- **NFS Carbon: map geometry disappears / turns transparent, then pops back in.**
  We do **not** have this. A real regression on their side.
- Colour: user reports **our colours look better**; theirs is flatter/washed.

**Therefore this is selective modelling, not a wholesale copy.** Port the
vertex/geometry pipeline to match them (Halo 3 first), keep our colour handling,
and re-test NFS Carbon for geometry pop-in after every stage — it is our
canary for importing their regression.

## Vertex-fetch bounds clamp: PORTED, and does NOT fix the ball (2026-08-10)

Implemented in `spirv_shader_translator_fetch.cc` + `.cc/.h` (fetch-end from
fetch constant word 1, stored for `vfetch_mini` reuse, `OpSelect` to 0 at/past
the end). Cvar **`vfetch_bounds_clamp`, default OFF**.

- Verified in the binary (`vfetch_bounds_clamp` + `xe_var_vfetch_bound` present
  in `libe.so`) rather than trusting "BUILD SUCCESSFUL".
- Halo 3: boots, menu renders, **0 errors**, **15 FPS** (baseline unchanged).
- **Ball: UNCHANGED.** As predicted from polarity — the clamp creates *more*
  zero-reads, and collapse is the zero signature.

**Keep as a correctness feature** (hardware-accurate, free), pending an NFS
Carbon regression check. Not a Halo 3 fix.

Observation worth following up: in the clamped gameplay frame there are large
flat translucent planes across the scene, i.e. vertices at *extreme* positions
forming huge triangles — not purely "all collapsed to a point". If that holds
across frames, the fault is **wrong** positions rather than **zero** positions.

## Next target, re-reasoned

Clip planes / `gl_ClipDistance` are genuinely missing (0 refs vs 8) but would
cause geometry to render that should be clipped — **not** vertices converging.
Deprioritised.

Better fit, and it reconnects with this project's own long-standing untested
lead: **relative constant addressing (`c[144+aL]`) used for bone matrices.**
[[project-xenia-ae-halo3-session-state]] records "bone-matrix CONSTANT VALUES
(`c[144+aL]`) were never compared across platforms" as the one lead never
followed. If relative constant indexing is wrong, every vertex reads the same or
a wrong bone matrix -> skinned geometry collapses, which is exactly the symptom,
and it is upstream of the viewport as required.

**We now have a working reference to diff that against.** Target the constant
load / relative addressing path in `spirv_shader_translator.cc`.
