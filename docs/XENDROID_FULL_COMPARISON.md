# XenDroid vs Canary AE — full code comparison

**Purpose:** XenDroid is the goal post. This is the complete, data-derived
comparison of their engine against ours, with a judgement on what is worth
taking. Test-only scaffolding is excluded on both sides.

**Generated 2026-08-10.** Re-derive any table by re-running the extraction in
`scratchpad/` against the two trees.

| | Canary AE | XenDroid |
|---|---|---|
| repo | `Xenia-AE` (`canary-ae`) | `/home/roman/xeniatest/xendroid-git` |
| cvars | 261 | **386** |
| engine source files | 1004 | 1023 |
| commits (last 6 months) | — | **1877** |
| upstream tracked | xenia-canary | **xenia-edge** (more active) |

Their engine lives at `emulator-core/src/main/cpp/xenia/src/xenia/`, ours at
`app/src/main/cpp/xenia-canary/src/xenia/` — identical layout below that, so
every file diffs 1:1.

## Where their effort went (commits by area, 6 months)

```
Vulkan 157 · GPU 115 · Build 101 · Kernel 97 · Android 87 · Testing 80
CPU 80 · XMA 71 · UI 55 · XAM 52 · A64 52 · APU 46 · D3D12 44 · x64 34
Memory 32 · a64 32 · CI 31 · App 30
```

Vulkan+GPU = 272 commits is the single biggest investment, then Kernel and CPU.
Note **Testing 80** and **Build 101/CI 31** — they invested heavily in being
able to measure and bisect, which is why their per-title notes are so specific.

---

# 1. ⭐ The highest-value finding: they solved our #1 hotspot

`guest_826DEFD0` is **14.08% of all process CPU** in our profile and we never
shipped anything for it. They have **four HIR compiler passes** we lack.

`SpinLoopBackoffPass`'s header describes **our exact function**:

```
//   li      r11, 4          ; small constant trip count
//   mtctr   r11
// loop:
//   or      r31,r31,r31     ; xN - SMT priority-hint "nops" (db16cyc)
```

Our own disassembly of `guest_826DEFD0` recorded "counter = 4, eight `yield`
per iteration, then test a byte at ptr+0x2a3d". That is the same idiom, and
they collapse it "into a single bounded host wait".

| pass | what it does |
|---|---|
| `spin_loop_backoff_pass` | Collapses the XDK CTR spin-backoff idiom into one bounded host wait. |
| `memory_poll_park_pass` | Adaptive backoff into indefinite memory-poll self-loops (load/test/branch-back) so a long wait **parks instead of burning a core**. Removes nothing; skips loops using `LOAD_CLOCK`, stores, calls or atomics. |
| `delay_countdown_collapse_pass` | Same idea for countdowns **spilled to memory** instead of CTR, which the CTR pass cannot see. |
| `preempt_check_injection_pass` | Preemption safepoint at entry and every back-edge when the guest scheduler is on; x64 and a64 emitters lower it. |

Supporting cvars we lack: `park_memory_poll_loops`, `a64_park_spin_backoff`,
`collapse_ctr_spin_loops`, `collapse_memory_delay_spins`, `precise_guest_delays`,
`guest_scheduler`, `guest_scheduler_quantum_us`, plus **`wfe_yield` /
`wfe_precise_sleep`** — they use **WFE**, the ARM primitive our own ARM-manual
notes flagged as "possibly a bigger win than the ISB swap" and which nobody had
tested.

**This is the single most valuable thing to port, and it is aimed precisely at
the biggest measured cost in our profile.**

## 1a. Our ISB measurement was invalid — corrected

Their `ee030d15 [A64] Emit isb for db16cyc instead of yield` is the same file
and emitter we changed. **Theirs coalesces, ours never did:**

```cpp
constexpr uint32_t kIsbSy = 0xD5033FDFu;
if (last_emitted_insn == kIsbSy) return;   // whole sled -> ONE barrier
e.isb(Xbyak_aarch64::SY);
```

Swapping `yield`->`isb` also requires changing the **comparison constant**
(`0xD503203F` -> `0xD5033FDF`). Our `-21.5%` run emitted **8 pipeline flushes
per loop iteration** instead of 1 — it tested an unbatched sled, not the idea.

---

# 2. Per-title knowledge we can use immediately

Their `GAME_COMPAT.md` is better engineering documentation than anything
upstream ships: exact cvars, *why*, what was tried and rejected, and open bugs.

- **`clear_memory_page_state`** — their documented fix for *missing/broken
  character models*: "the engine reads back GPU-written memory; the page-state
  refresh makes those writes visible." That is our Halo 3 collapse mechanism.
  **We already have this cvar.** ⚠️ Measured cost is real — it runs
  `SetSystemPageBlocksValidWithGpuDataWritten()` every frame on the GPU
  submission path. Per-game only, never global.
- **`depth_float24_convert_in_pixel_shader`** — prevents Adreno/kgsl GPU hang ->
  `VK_ERROR_DEVICE_LOST`. We hit exactly that on Adreno 610. **We have it.**
- `vulkan_depth_unorm24 = false` (we lack) — best result on Adreno 830 + Turnip.
- `vulkan_mid_frame_submission_draws` (we lack) — splitting long frames every N
  draws took **Forza 20 -> 30 fps** on Adreno 830.
- `mount_cache = true` recommended globally (we have the cvar).

**Gotchas that also apply to us:**
- Per-game TOML section header must match the cvar's **category**
  (`[Vulkan]` vs `[GPU]`) — wrong section is a **silent no-op**.
- Patch filenames must match `^[A-Fa-f0-9]{8}.*\.patch\.toml$` and the module
  hash must match, or they are silently rejected.
- `mount_*` cvars cannot be per-game (registered before title launch) — the
  same class of limitation as our `vulkan_lib_path` / `vsync` / `apu`.

Their **"do NOT bother with"** list for a stubborn Vulkan geometry bug, which
saves us repeating it: `readback_resolve`, `readback_memexport`,
`occlusion_query`, `depth_float24_round`, `render_target_path_vulkan=fsi`
(Turnip lacks the feature -> **silent fallback**), `turnip_debug=nolrz`
(made it worse).

---

# 3. Independent evidence Turnip mis-compiles our shaders

- `62a6a0d4 [GPU] Test multiply operands for zero on their bits so Mesa cannot fold it into fmulz`
- `71e67832 [GPU] Gate the bitwise multiply zero test behind a cvar, on for PGR3 and Dark Souls`

A real **Mesa codegen workaround**, gated per-title. This corroborates the known
precedent that the stock Qualcomm driver mis-compiled `resolve_full_32bpp` (our
old flat-navy bug) and supports a driver-side cause for the upside-down vista.

They also ship `turnip_debug` and an instrumented Turnip counter sampler
(`turnip_perf_sampler{,_file,_period_ms}`) we could reuse.

**No fix for the upside-down vista exists in their tree** — searched
flip/upside/invert/orientation/vista/mirror. Nearest relevant:
`2ec7ee04 [Vulkan] Invert the tiled base delta into a 2D texel origin for
texture stores` and `842ede52 [GPU] Use XOR to flip X texel group in all
load/resolve shaders` (X axis, not Y). That bug remains ours to solve.

---

# 4. Engine source files they have that we do not

Excluding Metal/D3D12/wx/desktop-only.

**CPU / JIT**
`compiler/passes/{memory_poll_park,spin_loop_backoff,delay_countdown_collapse,preempt_check_injection}_pass.{cc,h}` ·
`testing/{convert_single_nan,reserved_atomic}_test.cc`

**GPU**
`gpu/guest_spirv_shader_cache.cc` · `ui/vulkan/vulkan_gpu_completion_timeline.{cc,h}` ·
`ui/vulkan/vulkan_descriptor_pool_chain.{cc,h}`

**App / infrastructure**
`game_quirks.{cc,h}` (per-title quirk system) ·
`app/{game_compat_db,game_library,game_title_db,directory_scanner}.{cc,h}` ·
`app/title_id_util.h` · `debug/gdb/gdbstub.{cc,h}` (**GDB stub**) ·
`base/{embedded_bundle,frame_stats,shader_compile_counter,threading_fiber}` ·
`apu/xmp_state.h`

**VFS**
`vfs/{iso,stfs,xex,zar}_metadata.{cc,h}` · `vfs/content_install_standalone.{cc,h}` ·
`vfs/gdfx_util.h`

# 5. What WE have that they do not

Our 15 unique cvars: `readback_memexport`, `render_target_path_vulkan`,
`render_target_path_d3d12`, `ac6_ground_fix`, `gamma_render_target_as_srgb`,
`keyboard_mode`, `keyboard_user_index`, `query_occlusion_sample_lower_threshold`,
`query_occlusion_sample_upper_threshold`, `mute`, `notification_sound_path`,
`log_all_kernel_calls`, `d3d12_submit_on_primary_buffer_end`,
`d3d12_tiled_shared_memory`, plus our own toggle layer.

Files: `base/ae_fix_toggle.h`, `ae_fps.h`, `ae_perf_map.h`,
`testrig_debug_server.h`, ALSA audio driver, `app/profile_dialogs`,
tessellation shaders (`adaptive_quad_hs`, `adaptive_triangle_hs`), gamma-PWL
shader bytecode, and the per-game **driver + patches UI** (480 bundled patches).

That UI layer and the tessellation work are genuinely ours and are the base for
"like XenDroid but better".

---

# 6. Every applicable cvar they have that we lack

119 of the 140, after excluding Metal/D3D12/Win32 as non-applicable.
Descriptions are theirs, extracted from source.

### CPU (19)

| cvar | type | what it does |
|---|---|---|
| `a64_native_reserved_ops` | bool | Compile guest lwarx/stwcx. to inline native atomics (LSE CASAL) instead of the software-reservation thunk: lwarx captures the word and arms a per-thread flag; stwcx. |
| `a64_park_spin_backoff` | bool | For collapsed guest spin-backoff loops, spin cheaply for the first few iterations then park the thread with a short real sleep (adaptive) instead of the fixed isb sl |
| `a64_perf_map` | bool | Write a simpleperf/perf 'generic JIT symbols' map file (perf-<pid>.map) mapping generated code to guest function names, so profilers can attribute JIT samples. |
| `collapse_ctr_spin_loops` | bool | Collapse constant-trip-count bdnz spin-backoff loops (the XDK spin-wait primitive: mtctr small-constant + a sled of priority-hint nops + bdnz) |
| `collapse_memory_delay_spins` | bool | Collapse guest delay-countdown self-loops whose loop counter lives in memory (a stack slot) instead of CTR - `while (--*slot != 0) db16cyc...` - into one body pass,  |
| `context_promote_vec128` | bool | Promote VMX (VEC128) context loads/stores to SSA values and strip dead VEC128 context stores, letting the backend keep vectors in host registers. This restores upstr |
| `cpu_trace_mask` | uint32 | JIT execution trace modes to log (bitmask): 1=instructions, 2=data, 4=function calls (7=all). Each mode must be compiled in to be usable. |
| `inline_gprlr_saverest` | bool | Expand calls to the XDK __savegprlr_N/__restgprlr_N helpers inline instead of emitting real guest calls. The helpers are a handful of stack stores or loads, but as c |
| `inline_gprlr_saverest_parts` | uint32 | Bisect aid for inline_gprlr_saverest: bit 0 inlines the save helpers, bit 1 the restore helpers. 3 = both. Only consulted while inline_gprlr_saverest itself is on. |
| `inline_leaf_calls` | bool | Expand calls to small leaf guest functions inline instead of emitting a real call. A leaf here is a run of straight-line instructions ending in blr with no branches, |
| `inline_leaf_max_instructions` | uint32 | Largest leaf, in guest instructions, that inline_leaf_calls will expand. Raising it trades code cache size and translation time for fewer calls. |
| `log_delay_collapse_rejects` | bool | Log every candidate delay-countdown self-loop that collapse_memory_delay_spins rejects, with the guest address and the failing predicate. Diagnostic aid; noisy durin |
| `log_memory_poll_park` | bool | Log every candidate memory-poll self-loop this pass rejects, with the guest address and the failing condition. Accepted loops are always logged. |
| `log_safepoint_pc` | bool | Record the guest address of every JIT safepoint a fiber passes, so the cooperative scheduler's no-progress report can name where a wedged fiber last checked in rathe |
| `log_spin_loop_rejects` | bool | Log every candidate bdnz self-loop that collapse_ctr_spin_loops rejects, with the guest address and the first failing predicate condition. Diagnostic aid; noisy duri |
| `log_spin_wait_histogram` | bool | Bucket the wall-clock length of every collapsed guest spin-wait episode and dump the distribution once per second. Requires a64_park_spin_backoff. |
| `park_memory_poll_loops` | bool | Give indefinite guest memory-poll loops (load, test, branch back - the shape GPU fence and frame waits take) the same adaptive spin-then-park backoff collapsed spin  |
| `wfe_precise_sleep` | bool | ARM64: busy-wait PreciseSleep's sub-millisecond tail on the generic-timer event stream (WFE) for exact deadlines. Off by default: the spin holds threads on-CPU (~18% |
| `wfe_yield` | bool | ARM64: wait for the generic-timer event stream (WFE) instead of calling sched_yield() in guest yield/delay(0) spin loops. Avoids the syscall and lets the core idle i |

### a64 (1)

| cvar | type | what it does |
|---|---|---|
| `a64_vmx_nan_fixup` | bool | Emulate PPC NaN propagation on VMX float ops: the first NaN operand (by position) is returned, quieted; generated NaNs become the PPC default NaN (0xFFC00000). When  |

### GPU (44)

| cvar | type | what it does |
|---|---|---|
| `accurate_resolve_number_formats` | bool | Handle signed/integer resolve destination number formats and decode 8_8_8_8_GAMMA sources through the PWL curve during resolves. Both are accuracy features that only |
| `async_shader_compilation` | bool | Compile shaders and create pipelines asynchronously in background threads. Eliminates shader compilation stutter but may cause brief rendering artifacts while pipeli |
| `async_shader_skip_draws` | bool | Skip draws whose shaders can't render immediately via a placeholder (no interpreter stand-in, e.g. tessellation or textured/memexport/loop vertex shaders) until thei |
| `async_shader_vs_interpreter` | bool | Render new vertex shaders with the ucode interpreter while they translate and compile in the background, instead of stalling on translation. Requires async_shader_co |
| `async_shader_vs_interpreter_debug_color` | bool | Draw ucode interpreter VS placeholders with a flat grey pixel shader so the interim geometry is visible (host render target path only). Requires async_shader_vs_inte |
| `depth_bias_shader_offset` | bool | Route decal host render target draws with polygon offset through shader depth. This avoids Z-fighting in games that rely on tiny depth bias values that host fixed fu |
| `draw_resolution_scale_threshold` | uint32 | Surface pitch in pixels at or below render targets skip being upscaled by draw_resolution_scale_x/y. 0 disables it. Small offscreen surfaces like bloom or depth of f |
| `force_convert_triangle_strips_to_lists` | bool | Force CPU conversion of triangle strips to triangle lists. This may help diagnose rendering issues related to triangle strip handling. |
| `gamma_decode_pwl_resolve` | bool | During 8_8_8_8_GAMMA MSAA color resolves, average the samples in linear space instead of averaging the encoded PWL gamma values directly. This is separate from gamma |
| `gamma_render_target_as_unorm16` | bool | When the host can't write 8 bits per component pixels with piecewise linear gamma encoding directly with correct blending, use the 16-bit unsigned normalized format, |
| `gpu_debug_markers` | bool | Insert debug markers into GPU command streams for tools like RenderDoc. Annotates draw calls with Xbox 360 GPU context (primitive type, shader hashes, vertex count,  |
| `gpu_stall_spin_iterations` | uint32 | How many times the command processor polls the ring buffer with a cheap yield before parking on the write-pointer event when the guest has produced no commands. The  |
| `guest_display_refresh_cap` | bool | Control guest vblank timing. true: Fixed rate vblanks (50Hz PAL, 60Hz NTSC based on use_50Hz_mode). false: Unlimited vblanks, allows the guest to run as fast as poss |
| `log_gpu_frame_time_breakdown` | bool | Log a once-per-second breakdown of where the GPU command processor thread spends each guest frame (swap-to-swap): PM4 execution, draw processing, the swap itself, an |
| `log_resolve_details` | bool | Log a once-per-second histogram of resolve (EDRAM copy) operations: count and bytes per frame grouped by size, color/depth, MSAA, source and destination formats and  |
| `memexport_await_fences` | bool | Wait for the GPU to finish outstanding memory export before signalling a fence the guest reads, so exported data is in guest RAM by the time the guest looks at it. D |
| `memexport_enable` | bool | Make memory export output visible to the CPU by routing the draws that write it to a buffer aliasing guest RAM. Needed by games that read exported data on the CPU. D |
| `occlusion_query` | string | fast Controls hardware occlusion query behavior for EVENT_WRITE_ZPD. Used for effects like lens flares, object culling, and auto-exposure. Titles that use QueryBatch |
| `occlusion_query_fake_lower_threshold` | int32 | Lower end of the fake sample count value written on EVENT_WRITE_ZPD when real occlusion queries are disabled. -1 writes nothing, resulting in some games that sit and |
| `occlusion_query_fake_upper_threshold` | int32 | Upper end of the fake sample count value written on EVENT_WRITE_ZPD when real occlusion queries are disabled. Keep this higher than occlusion_query_fake_lower_thresh |
| `occlusion_query_log` | bool | Log occlusion query lifetime and summary stats. |
| `occlusion_query_querybatch_range` | int32 | Range of fake sample count values to walk for titles using the D3D QueryBatch standard before wrapping back to occlusion_query_fake_lower_threshold. This shouldn't b |
| `occlusion_query_saturation` | double | Compress higher occlusion query sample counts before guest writeback. This can be useful if effects such as lens flares appear too bright or too strong. 1.0 = defaul |
| `precise_interpolation` | bool | Manually interpolate pixel shader inputs with barycentric coordinates to exactly match the guest and avoid hardware interpolation precision differences. Fixes noise  |
| `readback_resolve_half_pixel_offset` | bool | When resolution scaling is active, sample from the center of each scaled pixel block during readback resolve instead of the top-left corner. May improve image qualit |
| `readback_resolve_sync` | bool | Stall the GPU after each readback_resolve copy so guest RAM is coherent in the same frame, instead of copying asynchronously. |
| `render_area_dirty_extent` | bool | Shrink each render pass's area to the region its draws actually touch. Host render targets span the whole EDRAM range for their pitch, so a narrow one is thousands o |
| `render_target_path` | string | performance Render target emulation path to use across all GPU backends. Use: [performance, accuracy] performance: Host render targets and fixed-function blending an |
| `resolve_check_number_format` | bool | Require the destination number format to match before using fast color resolves. Fast resolves copy the exact EDRAM bits. If a title resolves unsigned color data to  |
| `resolve_copy_dest_number_packing` | bool | Pack full-resolve fixed destinations according to copy_dest_number instead of assuming an unsigned fraction. Off restores the pre-upstream packing and keeps signed/i |
| `rt_cache_ownership_claim_memo` | bool | Skip EDRAM ownership-map walks for render target claims that provably change nothing (the same render target re-claiming an extent it already fully owns, with no own |
| `shader_profiling` | bool | Log shader translation and host pipeline (PSO) creation timings, tagged with 'shader_profiling:'. Off by default because it logs per shader and per pipeline. |
| `shared_memory_zero_copy` | bool | Alias guest RAM directly as the GPU shared-memory buffer instead of uploading dirty pages each frame. Removes upload copies and keeps memexport and resolve output co |
| `spirv_disable_rounding_mode_rte` | bool | Disable RoundingModeRTE capability in SPIR-V shaders. Enable this to allow shader debugging in RenderDoc, which doesn't support this capability. |
| `spirv_moltenvk_allow_contraction` | bool | When translating SPIR-V for MoltenVK, omit NoContraction decorations so SPIRV-Cross doesn't emit MSL NoContraction helper wrappers with [[clang::optnone]]. Other Vul |
| `spirv_multiply_zero_test_on_bits` | bool | Test multiply operands for zero on their raw bits instead of as min(/a/, /b/) == 0.0 when emulating Shader Model 3 zero semantics. Both forms are equivalent, but Mes |
| `spirv_switch_case_jump_fallthrough` | bool | Re-enter the main loop instead of falling through into the next control flow block, working around an NVIDIA shader compiler bug that turns an OpSwitch case fall-thr |
| `spirv_version_override` | string | 1.0 Override the SPIR-V version used in shader translation. Use: [1.0, 1.3, 1.4, 1.5, 1.6, auto] 1.0: SPIR-V 1.0 (Vulkan 1.0) (default) 1.3: SPIR-V 1.3 (Vulkan 1.1)  |
| `submit_on_primary_buffer_end` | bool | Submit the command buffer when a PM4 primary buffer ends if it's possible to submit immediately to try to reduce frame latency. |
| `texture_gradient_exp_bias` | bool | Apply the per-axis gradient exponent biases (LodBiasH/V, word 4 of the fetch constant) when sampling with computed gradients. The biases are zero in most games, and  |
| `texture_integer_num_format` | bool | Honour the texture fetch constant's integer num_format bit by scaling sampled components back to guest integer units (x255 for 8-bit, x65535 for 16-bit). Costs a per |
| `tiled_shared_memory` | bool | Enable tiled/sparse resources for efficient large address space support. Disable for graphics debugger compatibility. |
| `value_convert_7e3_8888_reuse` | bool | Decode (HDR float to LDR unorm) instead of bit-reinterpreting when a 7e3 (2_10_10_10_FLOAT) EDRAM tile is reused in place as 8_8_8_8 and the reusing draw blends over |
| `vulkan_mid_frame_submission_draws` | int32 | If greater than 0, end and submit the current command buffer after this many draws instead of only at the swap, so the GPU overlaps the frame's rendering with the bu |

### Vulkan (39)

| cvar | type | what it does |
|---|---|---|
| `ir3_debug` | string | Comma-separated IR3_DEBUG flags for the Turnip shader compiler, e.g. nopreamble or noearlypreamble to keep uniform math out of the shader preamble. Empty leaves IR3_ |
| `turnip_debug` | string | sysmem TU_DEBUG flags passed to the Turnip (Mesa freedreno) Vulkan driver, comma-separated. 'sysmem' (the default) forces sysmem (untiled) rendering, which masks a c |
| `turnip_perf_sampler` | string | Enable the instrumented Turnip build's whole-GPU KGSL performance counter sampler. Empty disables it. \"1\" samples the default triage set; \"tp\" swaps in the deepe |
| `turnip_perf_sampler_file` | string | Report path for turnip_perf_sampler. Empty writes to the app's files directory as tu_perf.log. |
| `turnip_perf_sampler_period_ms` | int32 | Poll period in milliseconds for turnip_perf_sampler. |
| `vulkan_allow_reverse_z` | bool | Pass viewports with minDepth > maxDepth (reverse depth ranges, used by many games for inverse-Z) directly to the driver. Vulkan allows this, but some drivers mishand |
| `vulkan_async_skip_draws` | bool | With asynchronous shader compilation: don't wait for pipeline creation at the submission boundary - draws whose pipeline hasn't finished compiling yet are dropped fo |
| `vulkan_avoid_geometry_shaders` | bool | Route guest primitives that would otherwise need a geometry shader through cheaper host paths on tile-based GPUs (Adreno/Turnip), where the geometry-shader stage ser |
| `vulkan_cache_sampler_parameters` | bool | Reuse sampler parameters and VkSampler handles across draws, re-deriving them only for fetch constants written since the previous draw (or on shader sampler-layout c |
| `vulkan_cache_texture_descriptors` | bool | Skip re-writing and re-binding the texture/sampler descriptor sets on draws whose resolved image views and samplers have not changed since the last write. Disable to |
| `vulkan_clamp_storage_buffer_range` | int32 | If > 0, clamp the reported maxStorageBufferRange to this many bytes. Drivers reporting very large limits get a single 512 MB storage buffer binding for shared memory |
| `vulkan_deferred_cmd_size_cursor` | bool | Track the deferred command buffer's recorded length with a size cursor over a geometrically grown buffer instead of resizing the vector per recorded command (vector: |
| `vulkan_depth_unorm24` | bool | Use the native D24_UNORM_S8_UINT format for guest 24-bit depth when the driver supports it. Disable to force the float32 depth emulation path (what drivers without u |
| `vulkan_direct_host_resolve` | bool | Resolve eligible (non-format-converting and packable format-converting) host render targets directly to guest memory with compute shaders instead of first dumping th |
| `vulkan_dynamic_constant_buffers` | bool | Bind the guest constant buffers as VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC so only the per-draw dynamic offsets vary; the constants descriptor set is re-allocated  |
| `vulkan_dynamic_pipeline_state` | bool | Use VK_EXT_extended_dynamic_state (and state 2/3 where available) to move cull/front-face/topology/depth/stencil/blend out of the baked pipeline key into dynamic sta |
| `vulkan_dynamic_rendering` | bool | Use VK_KHR_dynamic_rendering instead of traditional render passes. May improve or worsen performance depending on driver. Requires Vulkan 1.3 or VK_KHR_dynamic_rende |
| `vulkan_fast_register_ranges` | bool | Process PM4 register range writes with bulk byte-swapped copies and range-level constant dirty tracking (port of the D3D12 backend's register-write fast path) instea |
| `vulkan_fast_sampler_filterability` | bool | Derive host linear-filterability for samplers directly from the fetch constant instead of building the full texture binding info (texture key + host format pair) per |
| `vulkan_hoist_shmem_uploads` | bool | Record shared-memory uploads whose pages were not invalidated since the current submission opened at the start of the submission's command buffer instead of breaking |
| `vulkan_in_pass_resolve` | bool | Prepare color attachments for in-pass EDRAM resolves with VK_KHR_dynamic_rendering_local_read: RENDERING_LOCAL_READ image layout and input-attachment usage on color  |
| `vulkan_in_pass_resolve_debug_dump` | bool | DEBUG probe: still dump the owning render targets to the EDRAM buffer when a resolve was performed in-pass, to test whether skipping the dump is what a title misses. |
| `vulkan_in_pass_resolve_debug_exp_bias` | int32 | DEBUG probe: add this to the destination exponent bias of in-pass resolves only. Nonzero must visibly change the image if their output is consumed; identical output  |
| `vulkan_in_pass_resolve_debug_read_usage` | bool | DEBUG probe: declare shared memory as kRead rather than kGuestDrawReadWrite for guest passes even when local-read attachments are enabled. kRead covers COMPUTE/TRANS |
| `vulkan_in_pass_resolve_debug_reject` | int32 | DEBUG probe bitmask: 1 = reject multisampled sources, 2 = reject single-sampled sources. For bisecting which resolve population carries an artifact. |
| `vulkan_in_pass_transfers` | bool | Queue compatible render-target ownership transfers for execution inside the guest draw render pass instead of breaking the pass to perform them. On tile-based GPUs ( |
| `vulkan_install_missing_loader` | bool | When the Vulkan loader (vulkan-1.dll) is missing, offer to download it from LunarG. Disable to manage it manually. |
| `vulkan_normalize_dontcare_keys` | bool | Keep the loadOp discard bits out of framebuffer and pipeline cache keys under dynamic rendering, so toggling them cannot force a render pass break. Disable for A/B m |
| `vulkan_pipeline_creation_threads` | int32 | Number of threads used for graphics pipeline creation. -1 to calculate automatically (75% of logical CPU cores), a positive number to specify the number of threads e |
| `vulkan_placeholder_pipelines` | bool | Asynchronous shader compilation async model (A/B). Default false: the fork's deferred-bind model - no placeholder is created, the pipeline slot stays VK_NULL_HANDLE, |
| `vulkan_presenter_use_backing_scale` | bool | Use the macOS view backing scale factor for MoltenVK drawables. |
| `vulkan_rebar` | bool | Use ReBAR/SAM (Resizable BAR / Smart Access Memory) for upload buffers when available. This places staging buffers in GPU VRAM. May improve or hurt performance depen |
| `vulkan_renderdoc_capture` | bool | Request the VK_LAYER_RENDERDOC_Capture layer at instance creation for frame capturing. Requires the RenderDoc layer library to be bundled in the APK; attach from qre |
| `vulkan_resolve_to_texture` | bool | Have the in-pass resolve also store its result straight into the promoted destination texture, so vulkan_resolve_to_texture_serve can skip the upload that would re-r |
| `vulkan_resolve_to_texture_promote` | bool | Allocate textures that an in-pass resolve writes with STORAGE usage and a uint alias view, so the resolve can write them directly. Changes nothing on its own, but co |
| `vulkan_resolve_to_texture_serve` | bool | Skip the compute upload for textures an in-pass resolve already filled this frame. This is where the round-trip upload traffic disappears; requires vulkan_resolve_to |
| `vulkan_shared_memory_host_visible` | bool | On unified-memory GPUs (Adreno, integrated), allocate the shared-memory buffer from a host-visible cached memory type and map it so resolve readback (readback_resolv |
| `vulkan_skip_redundant_fetch_constant_writes` | bool | Don't invalidate texture bindings and the fetch/bool-loop constant buffers when a register write doesn't change the register's value. Games commonly re-emit identica |
| `vulkan_texture_descriptor_reuse_edge` | bool | When vulkan_cache_texture_descriptors is on, use upstream edge's bitmask+shader-pointer+sampler-vector descriptor-set reuse gate instead of XenDroid's content-hash g |

### Kernel (7)

| cvar | type | what it does |
|---|---|---|
| `auto_reset_event_handoff` | bool | Strict NT SetEvent semantics for auto-reset events (directed FIFO hand-off to a parked waiter). Can regress synchronization timing, so it is off by default; the hand |
| `fiber_reentry_longjmp` | bool | POSIX: reenter guest fiber stacks (KeSetCurrentStackPointers) with setjmp/longjmp instead of a C++ exception, avoiding a full DWARF unwind per fiber switch (~10% CPU |
| `guest_scheduler` | bool | Run guest threads as cooperative fibers driven by an in-kernel scheduler instead of mapping each to its own host OS thread. Requires a restart to take effect. |
| `guest_scheduler_quantum_us` | uint32 | Cooperative-scheduler timeslice in microseconds. A guest fiber running this long yields at its next JIT safepoint so co-resident fibers on the same dispatch thread m |
| `guest_scheduler_stats` | bool | Log guest scheduler counters once a second: blocked-waiter re-poll rate, fiber switches, forced preemptions, and how long offloaded blocking calls queue behind the s |
| `in_process_title_relaunch` | bool | Handle title-to-title launches in-process via full Shutdown/Setup cycle instead of spawning a new emulator process. |
| `precise_guest_delays` | bool | Serve KeDelayExecutionThread with a sub-millisecond-accurate host wait instead of a plain sleep. Guest frame-pacing sleeps sit on the frame critical path, and a plai |

### APU (1)

| cvar | type | what it does |
|---|---|---|
| `volume` | uint32 | Master volume for all audio output, from 0 (silent) to 100 (full volume). |

### Display (1)

| cvar | type | what it does |
|---|---|---|
| `show_debug_overlay` | bool | Show an on-screen debug overlay (FPS, instant/average frame time, and the number of shader/pipeline compiles currently in flight). |

### Logging (1)

| cvar | type | what it does |
|---|---|---|
| `log_sessions_keep` | uint32 | Number of past sessions whose logs (xenia log + logcat) are kept as zips in <storage>/logs, rotated at session start. |

### General (3)

| cvar | type | what it does |
|---|---|---|
| `dump_xex` | bool | Dump the main XEX to current directory on launch |
| `gdbport` | int32 | Port for GDBStub debugger to listen on, requires --debug (0 = disable) |
| `guest_crash_is_fatal` | bool | When a guest thread crashes (unhandled access violation or illegal instruction in JIT'd guest code), log a full crash report and terminate the emulator instead of si |

### UI (3)

| cvar | type | what it does |
|---|---|---|
| `achievement_notification_position_by_game` | bool | Use game-specified notification position for achievements. When disabled, achievements always appear at center-bottom. |
| `achievement_sound_path` | path | Path (including filename) to achievement unlock sound. Supports WAV, MP3, OGG, FLAC, and other common formats. |
| `ui_locale` | string | UI locale as an ISO code (e.g. \"en\", \"de\", \"ja\"). Empty selects the system default. |
---

# 7. What is actually USEFUL — ranked

Ranked by (measured value to us) x (confidence) / (effort). Everything here is
justified by a number from our own profiling or theirs, not by novelty.

## Tier 1 — do these

**1. The four spin/poll JIT passes.**
Targets `guest_826DEFD0` = **14.08%** of all process CPU, plus `guest_826DAAF8`
at 6.11% (20.2% in two functions). Their `SpinLoopBackoffPass` header documents
our exact guest idiom. Self-contained new files under `cpu/compiler/passes/`
plus emitter support and cvars, all individually gateable — so it can land
incrementally and be bisected. **Highest expected value of anything available.**

**2. Re-test isb-for-db16cyc WITH coalescing.**
Our revert was correct for our code but the idea was never tested. One small
emitter change, gated. Cheap, and it is the same 14% target.
⚠️ Do this *after* or *independently of* (1) — both touch `DELAY_EXECUTION`.

**3. `vulkan_mid_frame_submission_draws`.**
Their measurement: Forza **20 -> 30 fps** on Adreno 830, by splitting long
frames into multiple submissions to avoid kgsl GPU-idle bubbles. We are a
kgsl/Adreno target with a **GPU Commands thread at ~40-48%**, so the mechanism
applies. Small, self-contained, one cvar.

**4. Adopt their per-title config discipline.**
`clear_memory_page_state` and `depth_float24_convert_in_pixel_shader` are cvars
we already ship and have never used per-title. Zero code. The Halo 3 collapse
and the Adreno device-loss both have documented settings.

## Tier 1b — found in the cvar tables, directly tied to our profile

**`inline_gprlr_saverest`** (+ `inline_gprlr_saverest_parts` as a **bisect
aid**, bit 0 = save, bit 1 = restore). This is our `__savegprlr_29` /
`__restgprlr_29` finding — **3.67%**, which our own notes called "the cheapest
credible win" and we never implemented. They shipped it *and* shipped a
bisect switch because it went wrong for them first. Take both.

**`context_promote_vec128`** — promotes VMX/VEC128 context loads/stores to SSA
values and strips dead stores so the backend keeps vectors in host registers.
Our `guest_826DEFD0` disassembly found **105 `mov` + 23 `movk` = 36% of the
function** was register shuffling / constant rematerialisation, which we flagged
as "worth comparing against x64's output" and never chased. This is that fix.

**`a64_native_reserved_ops`** — compiles guest `lwarx`/`stwcx.` to inline native
LSE `CASAL` atomics instead of a software-reservation thunk. Atomics are on
every lock path; this is a broad, structural win on modern ARM.

## Tier 2 — likely wins, more work

**5. `vulkan_resolve_to_texture` / `_promote` / `_serve` + `vulkan_direct_host_resolve`.**
Resolve straight to a texture instead of round-tripping shared memory. Directly
relevant to our **upside-down vista**, which lives in the resolve->texture path,
and plausibly a perf win too. Also `vulkan_in_pass_resolve` (they have a whole
`perf/in-pass-resolve` branch).

**6. Modern Vulkan fast paths.** `vulkan_dynamic_rendering`,
`vulkan_dynamic_pipeline_state`, `vulkan_rebar`, `vulkan_hoist_shmem_uploads`,
`vulkan_shared_memory_host_visible`, `vulkan_cache_sampler_parameters`,
`vulkan_cache_texture_descriptors`, `vulkan_avoid_geometry_shaders`,
`submit_on_primary_buffer_end`, `tiled_shared_memory`.
Individually small, collectively the 157-commit Vulkan investment.

**7. `async_shader_compilation` + `async_shader_vs_interpreter`.**
Shader-compile stutter. We have a "Pre-cache Shaders" UI feature; async
compilation is the engine-side complement.

**8. `game_quirks.{cc,h}` + `game_compat_db`.**
Per-title quirks in-engine rather than hand-edited TOMLs. This is the
infrastructure behind "every game just works", which is the stated goal.

## Tier 3 — tooling worth having

`turnip_debug` + `turnip_perf_sampler*` (we have an ad-hoc TU_DEBUG prop probe;
theirs is a real cvar-driven sampler) · `debug/gdb/gdbstub` · `frame_stats` ·
`log_memory_poll_park`, `log_spin_wait_histogram`, `log_gpu_frame_time_breakdown`,
`log_resolve_details` · `spirv_multiply_zero_test_on_bits` (Mesa fmulz
workaround — keep in the back pocket for Turnip mis-compiles).

## Explicitly NOT useful to us

- All 14 `metal_*` cvars, `d3d12_*`, `dxil_debug`, `enable_rdrand_ntdll_patch`,
  wx/desktop UI, `windowed_app_*` — wrong platform.
- `readback_resolve` / `readback_memexport` — we already have these and they are
  **diagnostic only**. See the warning below.

---

# 8. ⚠️ Measurement warning discovered while doing this comparison

`readback_memexport = true` and `readback_resolve = 'full'` were left enabled in
our device config from earlier investigations. Both force **mid-frame GPU
synchronisation** ("huge performance impact", per their own cvar descriptions).

Turning both back to `false` took the Halo 3 menu from **~10 FPS to 15 FPS**.

**Our "9.67 FPS baseline" was measured with diagnostic readback enabled.** Any
performance conclusion drawn against that baseline needs re-checking, and every
future benchmark must assert these are off first. This is the same failure mode
as the vblank/vsync artifact: the instrument was changing the thing measured.

---

# 9. How to keep this current

```sh
git pull       # 1877 commits/6mo, moves fast
```

Re-derive the cvar delta with the multi-line-aware extractor (a single-line
`grep 'DEFINE_bool(name'` **misses wrapped definitions** and produced two false
"missing" entries the first time). Their commit messages have **no bodies** —
always read the diff.

---

# 10. ⭐⭐⭐ BREAKTHROUGH 2026-08-10: XenDroid renders the Halo 3 models CORRECTLY

Installed XenDroid `5ef8fc6` (`xendroid.compose`) on the **same Odin 2**, ran
the **same Halo 3**, on the **same stock Qualcomm driver**, into the **same
Sierra 117 opening scene**.

| | Canary AE | XenDroid |
|---|---|---|
| character models | **collapsed to blobs** (helmet = ball, cigar detached) | **fully correct** — distinct helmets, faces, articulated arms, rifle held properly |
| textures | correct | **flat/washed out** (faces render as black voids) |
| menu vista | renders, but **upside down** (Turnip) | **flat blue** on stock driver (our old flat-navy bug) |
| FPS | ~10 | **22.6** intro / 5.8 in the heavy scene |

## Why this matters more than any previous Halo 3 finding

**The character collapse is FIXABLE, and a working reference exists on our own
hardware.** Every prior theory treated it as possibly-unfixable (Adreno
underfilling memexport buffers, a driver limitation, a precision divergence).
It is none of those: the same GPU and the same driver render it correctly under
a different emulator build.

This also kills the last defence of the driver hypothesis for the models -
XenDroid is on the **stock Qualcomm driver here**, the one that cannot even
compile our `resolve_full_32bpp` correctly (their menu shows the identical
flat-navy bug we had). It still gets the geometry right.

⚠️ Note the split: **they win on geometry, we win on textures.** Their models
are correct but flat/untextured; ours are textured but collapsed. These are two
independent defects, and each project has one of them. That is a strong hint
they are *different* subsystems - geometry/memexport vs texture fetch - and
should be chased separately.

## What this makes the top priority

Diff the **memexport / vertex path** against theirs. Our own analysis says the
collapse is Xenos MEMEXPORT (render-to-vertex-buffer skinning): a producer VS
scatter-compacts vertices, a consumer VS expands them. We have `readback_memexport`;
they do not - they solve it some other way. Relevant candidates from their
cvar set: `vulkan_resolve_to_texture{,_promote,_serve}`,
`vulkan_direct_host_resolve`, `tiled_shared_memory`,
`vulkan_shared_memory_host_visible`, `vulkan_hoist_shmem_uploads`.

Also now **eliminated by direct experiment** (see HALO3_VISTA_UPSIDE_DOWN.md):
`clear_memory_page_state`, their documented Team-Ninja model fix, does **not**
fix ours - so whatever they do differently is not that cvar.

## 10a. Sierra 117 gameplay under XenDroid + Turnip (2026-08-10)

Loaded our `Turnip_v26.0.0_R8` into XenDroid (verified: `driverName: turnip
Mesa driver`) and played into the Sierra 117 jungle:

- **Character models fully correct** — Johnson's face with moustache and
  expression, a Marine's sunglasses/visor, full camo and gear detail.
- Jungle foliage, tree trunks, volumetric light shafts, HUD prompt all correct.
- **17.7 FPS** in that scene (ours renders the same scene as blobs at ~10).
- Menu vista **perfect, right-side up**, 20.6 FPS.

**Their remaining defect: faces flicker.** Comparing frames ~3 s apart, faces
alternate between correctly lit/detailed and pale/blown-out, while the rest of
the frame is stable. That is a face-specific shading/texture alternation - a
*different* bug from our collapse, and theirs to solve, not ours.

Net: on the same device, same game, same driver, XenDroid is correct on
**geometry, orientation and texturing**, and roughly **1.7-2x our frame rate**.
The only thing we currently do better in Halo 3 is stable face shading.


⚠️ **CORRECTION 2026-08-10:** earlier notes here claimed Canary AE's faces are
stable while XenDroid's flicker, and counted that as an advantage. **Wrong** —
user confirmed **our faces flicker too**. It was never observed: our characters
render as blobs, so there are no faces to judge. The face flicker is a **SHARED**
bug, not a differentiator. Our real remaining advantages are **colour**, the
per-game settings pipeline, custom driver support and the UI/UX.
