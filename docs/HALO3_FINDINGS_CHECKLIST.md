# Halo 3 menu 3D-vista bug — MASTER CHECKLIST (single source of truth)

Purpose: one place listing what WORKS, what's RULED OUT, what's DISCOVERED, and
every TEST already run — so we never repeat a test or re-chase a dead theory.
Detailed narrative lives in `HALO3_MENU_INVESTIGATION.md`; this file is the
quick-reference checklist. Update it after every test.

Device: Odin 2, Adreno 740. Package: `org.xeniaae.canary`. Branch: `canary-ae`.
Desktop RADV (Linux Xenia-Canary) renders this menu CORRECTLY — the bug is
Adreno-specific in AE's forked Vulkan GPU backend.

## THE BUG (one line)
Halo 3 main menu: 2D UI renders perfectly, but the animated 3D vista background
is a flat/uniform dark navy instead of the dark snowy landscape.

---

## ✅ CONFIRMED WORKING / TRUE (do not re-test)
- [x] 2D UI (HALO 3 logo, menu list, Bungie logo) renders perfectly.
- [x] The menu is a DEFERRED-SHADING scene (not one big draw).
- [x] The 3D scene geometry renders and RESOLVES into shared memory correctly —
      G-buffer shared memory is FULL of real varied per-frame content
      (GBUFGPU readback: 0x044B0000 65536/65536 nonzero, thousands distinct).
- [x] The EDRAM resolve (host-RT dump + resolve compute) WORKS.
- [x] The composite pass (vsh 831761DE / psh 373E65D9) REACHES the screen
      (magenta test: forced its output magenta → whole background flooded).
- [x] The composite's texture COORDINATE is correct (output r0.xy → clean 0..1
      screen gradient; interpolator/param-gen fine).
- [x] The texture LOAD compute works: the loaded G-buffer host IMAGE is VARIED
      (decoupled image capture of 0x44B0: 65536/65536 nonzero, distinct_runs
      1578, real low-contrast brown terrain). Front buffer (same load_shader)
      presents crisp UI → load handles fine detail.
- [x] tf2 (0x043FC000, the composite's 2nd input, 64bpp HDR half-float) is also
      VARIED (distinct_runs 40960).
- [x] Composite pixel-shader constants are correct/sane (COMPOSITE_CONST:
      c3=(1,1.5,0,-0.5) c5=(1,1,0,0) c229/230/231 = small positive color-matrix
      rows with .w=0, c232=(0.9,..) c255=(1,0.333,0,0)).
- [x] Fragments run per-pixel across the whole screen (magenta covered it).
- [x] Texture invalidation after resolve is WIRED
      (MarkRangeAsResolved→RangeWrittenByGpu→FireWatches).
- [x] Committed GPU precision fixes are real/kept: RSQ (c14047bc), Cody-Waite
      SIN/COS (a0b2f29e), register-zero-init (54e6a4d6), degenerate-W-clip
      (6a4b9932).

## ❌ RULED OUT (with how — do not re-chase)
- [x] Geometry / memexport collapse — force-vis fullscreen the terrain consumer
      488D9488 + cull/depth/blend off → still navy (draws 15,795x, invisible).
- [x] MSAA resolve — forced all rendering msaa=0 (RB_SURFACE_INFO bits) → still navy.
- [x] Composite output path / front-buffer resolve / presentation — magenta floods.
- [x] EDRAM resolve to shared memory — proven WORKING (G-buffer varied).
- [x] Texture-invalidation wiring — present in code.
- [x] Synchronization / ordering — forced full GPU idle after every G-buffer
      resolve → no change. Resolve is before the composite draw in exec order.
- [x] Tiling math — AE's UModel XeTextureTiledOffset2D == upstream's bank/pipe
      XenosTextureTiledAddress2D, byte-identical (scratchpad/tilecmp.c, 0/4096 diff).
- [x] Texture LOAD compute — the loaded host image is VARIED (proven).
- [x] Render-target-as-texture serving — AE doesn't do it (checked); composite
      samples the normally-loaded texture.
- [!] Mip/LOD selection — PREVIOUSLY "ruled out" via a forced-LOD-0 amplified
      probe (unreliable readout). RE-OPENED July 17: with the reliable
      raw-sample/coordinate readout, mip/LOD is now the PRIME SUSPECT (see
      CURRENT LIVE LEAD). The old amplified LOD-0 test is not trustworthy.
- [x] Composite pixel-shader constants — dumped, all correct/sane.
- [x] c32=NaN constant (in the OTHER shader, terrain 488D9488) — flushing NaN→0
      changed nothing; that shader is offscreen anyway.
- [x] Texture-INSTANCE / aliasing mismatch — RULED OUT. Logged the composite's
      bound fc0 texture vs the loaded instance: SAME image pointer
      (0x77F8583D60), same key (1152x640 fmt6 tiled, signed=NULL). The composite
      samples the exact image the load fills with varied content. Load completes
      (no LOADFAIL). So no wrong-instance/wrong-size issue.

## 🔎 RETRACTED / WRONG CONCLUSIONS (don't repeat the mistake)
- [!] "Texture LOAD is broken" — was based on a raw-texel probe accidentally
      placed in the VERTEX-fetch path (pixel shaders never vfetch) → the probe
      NEVER FIRED; "uniform navy" was just the unchanged output. RETRACTED.
      Redone in the texture-fetch path; the load actually WORKS.
- [!] "The vista is 94% geometry-collapsed / low memexport fill %" — the whole
      memexport/precision/fill-% investigation chased a shader whose output
      never reaches the screen. Superseded.

## ★★★★★★★ July 23 — REFINED ROOT CAUSE + 7 FAILED FIXES: Adreno collapses the per-thread EDRAM-read address in resolve_full_32bpp
Refined the July-22 finding. The collapse is triggered specifically by the
PRESENCE of the per-thread SOURCE-ADDRESS computation feeding the EDRAM buffer
load in the FULL shader. Proof: a probe that outputs a CONSTANT (source addr +
load dead-code-eliminated) writes correctly to ALL pixels (dest addr + threading
fine in the SIMPLIFIED shader); ANY variant that keeps the per-thread source
address collapses pixel_index/GlobalInvocationID to uniform => every thread
reads the same EDRAM location => uniform resolved albedo. So earlier "dest works"
was an artifact of the simplified probe; in the real/full shader BOTH source and
dest collapse (all threads write one value -> uniform SHM). This is an Adreno
DRIVER mis-compilation, not spirv-opt (reduced-opt also collapses).
SEVEN shader-level fixes TRIED, ALL FAILED (each: recompiled via aecompile.py,
built, on-device screenshots still uniform+flickering; all reverted, shader tree
verified byte-identical to committed):
  1. shift-by-comparison-vector -> explicit multiply (edram.xesli).
  2. reduced spirv-opt (single/none instead of -O -O).
  3. inline the whole XeEdramOffsetInts computation into main (bypass func call).
  4. remove the wrap modulo (address %= ...).
  5. division-free address (rt_sample.y*80+rt_sample.x, no int divide).
  6. manual GlobalInvocationID (gl_WorkGroupID*gl_WorkGroupSize+gl_LocalInvocationID).
  7. remove the per-thread early bounds-return.
None changed the collapse. The shader (resolve_full_32bpp, shader_index=6, the
kFull32bpp EDRAM->SHM resolve-copy for the 1152x640 4xMSAA-source albedo) is
mis-compiled by the Adreno 740 driver such that the per-thread buffer-read index
degenerates to uniform.
★ NEXT-PHASE OPTIONS (all beyond shader-line tweaks): (A) reduce register
pressure - drop from 4 pixels/thread to 1 (needs matching C++ group-count change
in draw_util GetCopyShader + the dispatch); (B) buffer qualifiers volatile/
coherent on the EDRAM SSBO / a different SSBO access pattern; (C) try a
different/newer Adreno driver via the project's libadrenotools custom-driver
support (driver bug => a different driver version may not have it); (D) force the
FAST resolve shader (simpler, may dodge the complexity trigger) and verify output
correctness; (E) bypass the compute resolve-copy for this case entirely (hardware
resolve to a staging image -> copy to shared memory, or the code's TODO "direct
host RT -> shared memory resolve"). Recommend (C) first (cheapest, and it's a
driver bug) then (A)/(D).

## ★★★★★★ EXACT LOCALIZATION July 22 (shader probes) — XeEdramOffsetInts source addr returns UNIFORM; obvious fixes FAILED
Probed resolve_full_32bpp by recompiling it (scratchpad/aecompile.py:
xesl->glslang(GLSL)->spirv-opt -O -O->.h; VALIDATED - clean recompile reproduces
the committed 13893-dword bytecode byte-for-byte) with its output overridden,
and reading SHM 0x044B0000:
 - Output = CONSTANT 0xFFFFFFFF  -> SHM ALL 0xFFFFFFFF (65536/65536). ⇒ thread
   dispatch + DEST address (XeResolveDestPixelAddress) + write path ALL WORK.
 - Output = the computed SOURCE address (XeResolveColorCopySourcePixelAddress...
   -> XeEdramOffsetInts) -> SHM UNIFORM 0x00E00B00 = byteswap(0x000BE000) =
   778240 = 608*1280 = base_tiles(608) * tile_area(80*16), within-tile = 0. ⇒
   the SOURCE ADDRESS is the SAME for every thread (pixel-0's address) = every
   thread reads the same EDRAM location = uniform resolve output. THIS is the
   collapse, inside XeEdramOffsetInts, NOT the load/unpack, NOT the dest, NOT
   threading.
KEY ASYMMETRY: the SAME pixel_index gives a VARYING dest address (constant-white
proved all pixels written) but a UNIFORM source address. So XeEdramOffsetInts
(which has integer vector div/mod, unlike the dest addr) is where pixel_index
effectively collapses to 0 / the result is computed as uniform. Looks like an
Adreno-driver optimization treating the source-address computation as uniform
(hoisted, ~pixel 0) when the full shader runs (with the source path DCE'd via
the constant probe, dest stays correct).
FIXES TRIED THAT DID **NOT** WORK (reverted):
 (1) rewrote the first line `pixel_index << xesl_uint2(xesl_greaterThanEqual(
     msaa>=k4X,k2X))` as an explicit multiply `pixel_index * ((msaa>=k4X?2:1),
     (msaa>=k2X?2:1))` in edram.xesli. Address STILL 0x00E00B00 (unchanged). Not
     the shift.
 (2) recompiled resolve_full_32bpp with REDUCED spirv-opt (single/none, 12121 vs
     13893 dwords). Vista STILL uniform + flickering (mean per-frame 16->34->32->
     18->19). Not an spirv-opt-level artifact.
All shader files RESTORED to committed state (bytecode verified byte-identical
to original). Nothing shader-side committed.
★ NEXT FIX IDEAS (untried): (a) INLINE the XeEdramOffsetInts computation directly
in resolve_full_32bpp main using pixel_index (bypass the function call / arg
passing that may be where the uniformity mis-analysis happens); (b) rewrite the
vector integer DIVISION `rt_sample_index / tile_size_samples` and the MODULO
`address %= ...` as scalar ops or multiply-by-reciprocal-free forms (Adreno
compute int div/mod is a classic mis-optimization site); (c) add an explicit
thread-varying dependency / optimization barrier the driver can't hoist; (d)
compare this shader's SPIR-V to the desktop-RADV-correct one for a structural
diff. The bug is 100% inside XeEdramOffsetInts for the 4xMSAA-source, wrap=true
resolve params.

## ★★★★★ REAL ROOT CAUSE July 22 PM — the EDRAM->SHM RESOLVE-COPY collapses the albedo
Same-frame capture at the composite draw of the resolve-copy's INPUT vs OUTPUT
for tile 608 (the varied albedo), 1216-resolve skipped:
  frame  EDRAM tile608 (input)   SHM 0x044B0000 (output)
    1     59880 varied            7476 varied
    2     59488 VARIED            1 UNIFORM (0x43464900)
    3     59488 VARIED            1 UNIFORM (0x59636800)
    4     59488 VARIED            1 UNIFORM (0x5F616500)
⇒ THE tile-608 EDRAM->SHARED-MEMORY RESOLVE-COPY reads richly-varied EDRAM and
writes a SINGLE UNIFORM value, in the SAME steady-state frame. Definitively
localized (this supersedes the July 20 "resolve-copy is correct" - that was
never cleanly tested for 608 alone; the SHM uniformity was mis-attributed to the
1216 clobber, now disproven).
MECHANISM: the uniform output is a single constant that CHANGES per frame (not
stale varied data). A barrier/ordering race would yield STALE-but-VARIED data;
one value repeated across every output pixel = every output thread reading the
SAME EDRAM source location = an ADDRESS-COMPUTATION COLLAPSE in the resolve-copy
compute shader on Adreno (NOT a race). Analogous to the SIN/COS Adreno issue but
in the resolve_copy shader's EDRAM source addressing. It's specific to this
resolve's params (NFS Carbon resolves fine) - likely the 4xMSAA-source /
particular pitch/offset case. Frame 1 varied = SHM initial state before the
broken resolve-copy takes over (or first-frame timing), a red herring.
★ SHADER IDENTIFIED (July 22 PM): the albedo (0x044B0000) resolve-copy uses
shader_index=6 = **kFull32bpp** (resolve_full_32bpp.xesli/.cs.xesl), groups
36x20 for 1152x640 (also index 0 kFast32bpp1x2xMSAA groups 18x80 for some
sub-resolves). kFull32bpp: 1 thread = 4 host pixels, pixel_index =
GlobalInvocationID.xy<<(2,0); reads EDRAM via XeResolveLoad4RGBAColors at
XeResolveColorCopySourcePixelAddressIntsYHalfPixelOffsetFilling ->
XeEdramOffsetInts (edram.xesli - integer div/mul tile addressing incl.
msaa_samples=4X); packs via XePack32bpp4Pixels; writes shared memory at
XeResolveDestPixelAddress. THIS shader collapses varied EDRAM -> uniform SHM on
Adreno (NFS Carbon works, so likely param-specific: 4xMSAA source + full-path
format conversion). Collapse is in ONE of: (a) source addr (XeEdramOffsetInts),
(b) the MSAA sample load/unpack (XeResolveLoad4RGBAColors), or (c) dest addr
(XeResolveDestPixelAddress) / GlobalInvocationID threading.
★ NEXT (the real fix): PROBE the shader - recompile resolve_full_32bpp with its
output replaced by GlobalInvocationID.x / the computed source address (scratchpad
aecompile.py pipeline: xesl -> glslang -> spirv-opt -> .h bytecode array). If
SHM shows a gradient => threads/addressing fine, collapse is in the LOAD/unpack;
if constant => GlobalInvocationID or address collapses. Then apply a portability
fix to the specific op (SIN/COS-style), or route this resolve through a path that
works. Only kFull32bpp (and maybe kFast32bpp*4xMSAA) need the fix.

## ★★★ COMPOSITE-SIDE TRACE July 22 PM — verified inputs; 1216-clobber DISPROVEN; it's a per-frame COLLAPSE of the albedo resolve-copy
Re-traced from the composite draw (psh 0x373E65D9). VERIFIED from the fetch
constants (COMPOSITE_BIND, not assumed): the composite samples exactly TWO
textures - fc0 = 0x044B0000 (1152x640, fmt6 k_8_8_8_8, tiled) = ALBEDO, and
fc2 = 0x043FC000 (288x160, fmt32, tiled) = HDR.
RESOLVE map (RESOLVESRC): 0x044B0000 is written BOTH from tile 608 (color0 of
the 4xMSAA G-buffer, VARIED; 4 partial 160-row strips/frame, length 737280) AND
from tile 1216 (one FULL 1152x640 resolve/frame, length 2949120). RTMAP: the
4xMSAA terrain draws write cbase=[608,1216] (tile 608 = G-buffer color0, tile
1216 = color1); separate NON-MSAA passes (pitch 1200) also write tile 1216.
SKIP-1216 test: skipped the tile-1216 -> 0x044B0000 full resolve. Vista
UNCHANGED, and SHM 0x044B0000 STILL collapses. ⇒ the 1216 clobber is NOT the
cause (disproven).
DECISIVE SHM capture (1216 skipped, at the composite draw): SHM 0x044B0000 =
frame1 distinct_runs 7476 (VARIED) → steady state distinct_runs 1 (UNIFORM),
the flat value CHANGING per frame (0x44484C00, 0x68707700) = the animating
"flat navy that flickers". So the albedo is varied on frame 1 then collapses to
one (animating) color per frame - reading ONLY tile 608 (which reads back
VARIED in EDRAM every frame). ⇒ THE COLLAPSE IS IN THE tile-608 EDRAM->SHM
RESOLVE-COPY (or a per-frame feedback that regenerates 0x044B0000 from its
prior value and loses info on Adreno) - NOT the transfer, NOT the 1216 clobber.
Reconcile with July 20: EDRAM tile608 VARIED every frame + SHM 0x044B0000
UNIFORM steady state ⇒ the 608 resolve-copy collapses in steady state (varied
frame 1). Suspect: the DUMP(RT->EDRAM) -> RESOLVE-COPY(EDRAM->SHM) ordering/
barrier on Adreno - resolve-copy reads EDRAM before the dump publishes it in
steady state (frame-1 timing differs), OR the vista terrain shader samples the
previous albedo (feedback) and that read collapses.
★ NEXT: capture EDRAM tile 608 AND SHM 0x044B0000 in the SAME steady-state
frame to confirm EDRAM-608 varied + SHM uniform (⇒ 608 resolve-copy collapses);
then inspect the dump->resolve-copy barrier for tile 608 specifically, AND check
whether the terrain G-buffer draws (cbase=[608,1216]) SAMPLE 0x044B0000 (a
feedback). Diagnostics (COMPOSITE_BIND log, SHM capture, resolve skip) all
committed and gated/revertible.

## ⚠️ RETRACTION July 22 PM — the transfer collapse is a RED HERRING for the vista
Two cheap tests BOTH say the tile-1216 color->color transfer is NOT on the
visible vista's critical path, so the "★★★★ transfer IS the collapse" section
below (still true as a fact about that shader) does NOT explain the on-screen
bug, and the resolve-based "route 1" fix is NOT worth building:
 1. SKIP test: skipped the transfer entirely (kTestrigSkipVistaColorTransfer) →
    vista UNCHANGED (still uniform navy).
 2. GRADIENT-PROBE test: forced the transfer to output a gl_FragCoord gradient
    (kTestrigTransferGradientProbe, overriding source_color post-read) → vista
    UNCHANGED, AND the transfer's dest RT still RESOLVED to the identical
    uniform constant 0x00010000 as without the probe. (The dest staying a fixed
    constant regardless of the transfer's source is itself suspicious - it hints
    the captured dest RT's uniform value may not even come from the transfer, or
    the source-vs-dest "collapse" compared two RTs the transfer doesn't actually
    connect the way assumed. Do not over-trust the "transfer collapses
    22989->2" framing as the vista cause.)
Both probes reverted to false. NET: the whole tile-1216 render-target-transfer
line of investigation (July 20-22) is ruled out as the VISIBLE vista cause.
The varied source RT (22989) and the transfer collapse are real but a side-show.
★ NEXT (re-trace from the COMPOSITE side, dropping the tile-1216-transfer
assumption): the composite samples its albedo texture from guest address
0x044B0000. Trace, at the composite draw, EXACTLY which EDRAM tile / which RT /
which resolve populated 0x044B0000 THIS frame, and capture that resolve's true
source. Everything so far assumed 0x044B0000 <- tile 1216; verify that from the
resolve side rather than inferring it. Also reconcile: last session EDRAM tile
608 read back VARIED while 1216 was uniform - re-check whether the composite
should be reading 608 (varied) and a wrong tile/instance is being bound.

## ★★★★ (still factually true, but NOT the vista cause - see retraction above) July 22 — THE REPAINT (ownership-transfer shader) IS THE COLLAPSE
Built a read-only, non-destructive capture at the exact moment AFTER a draw's
ownership transfers run (the copy-forward "repaint") but BEFORE the guest
geometry draws (VulkanRenderTargetCache::TestrigCaptureVistaRtPostTransfer,
called from IssueDraw right after render_target_cache_->Update()). Confirmed the
repaint and the geometry draw are in the SAME submission but SEPARATE render
passes (natural boundary → safe to capture; no need to disable geometry).
Captured the SAME tile-1216 transfer's INPUT vs OUTPUT (both 4xMSAA, resolved
to 1x for capture; 256x256 sample, distinct_runs where 1-2 = uniform):
  - Repaint SOURCE (fmt0 = k_8_8_8_8): distinct_runs = **22989 (richly VARIED)**
    — this is real terrain/albedo content; proves the varied vista data DOES
    reach a tile-1216 RT.
  - Repaint DEST  (fmt2 = k_2_10_10_10): distinct_runs = **2 (UNIFORM)**.
⇒ THE FORMAT-CONVERTING COPY-FORWARD ("transfer shader") COLLAPSES VARIED→UNIFORM
on Adreno, for the tile-1216 fmt0(k_8_8_8_8)→fmt2(k_2_10_10_10) 4xMSAA transfer.
Both formats are 32bpp, so on real hardware this is ~a byte-reinterpret that
preserves everything; desktop RADV renders it correctly. So it's an
Adreno-specific execution problem with that specific transfer shader / pipeline
(TransferMode::kColorToColor, TransferShaderKey source_resource_format=fmt0,
dest fmt2, source_msaa_samples=k4X). Read-only capture confirmed non-destructive
(on-screen vista unchanged, UI still rendering).
The transfer/copy-forward draw is in PerformTransfersAndResolveClears
(vulkan_render_target_cache.cc); the shader is built by GetTransferShader /
the SpirvShaderTranslator transfer path; pipeline via GetTransferPipelines.
NEXT: read the kColorToColor transfer shader generation for the 32bpp→32bpp
MSAA case and find why it collapses on Adreno (candidate causes: per-sample
MSAA load mishandled like other Adreno MSAA issues; a redundant lossy value
re-encode where a bit-reinterpret/skip would do since both are 32bpp; or a
host-format packing bug). Fix candidates in the session notes.

DISAMBIGUATION (July 22): forced the transfer path to use the per-sample-mask
(non-gl_SampleID) mechanism instead of sample-rate-shading, via a
kTestrigTransferForceNoSRS flag gating all 5 sampleRateShading branches in the
transfer path (GetTransferShader x2, GetTransferPipelines x2,
PerformTransfersAndResolveClears x1). RESULT: dest STILL uniform (0x00010000,
distinct=2) and vista UNCHANGED. ⇒ the collapse is NOT in Adreno's gl_SampleID
handling. It is in the transfer shader's SOURCE-READ / FORMAT-CONVERSION logic
itself (the read→pack-as-guest-bits→re-encode-as-dest-format chain in
GetTransferShader ~lines 3183-3300+), independent of the MSAA sample mechanism.
Consistent with the constant (not "averaged") output and with the older
"forced msaa=0 still navy". Flag reverted to false. The source-color read uses
createTextureCall on the multisampled source RT (~line 3193); note the DUMP
compute shader's per-sample texelFetch on an MSAA source worked fine last
session (tile 608 varied), so if the read is the culprit it'd be a
fragment-shader-specific / this-shader-specific issue, not general MSAA fetch.
SKIP TEST (July 22): skipped the tile-1216 color->color 4xMSAA transfer entirely
(kTestrigSkipVistaColorTransfer, in the invocation-building loop). RESULT: vista
UNCHANGED (still uniform navy). ⇒ removing the corrupting repaint alone does NOT
help - the dest RT has no varied content of its own without a working copy. Two
readings, NOT yet distinguished: (A) the transfer IS load-bearing (desktop needs
it to carry the varied source into the dest) but skipping can't help since there's
nothing good to preserve without a WORKING copy → route 1 could still fix it; (B)
the transfer output is not on the composite's critical path → route 1 won't help.
The dest output being a flat CONSTANT (every pixel identical, 0x00010000) is
consistent with an Adreno multisampled-texture-sampling-in-fragment-shader bug
returning a constant (→ route 1 fixes) OR a source-coordinate collapse (→ route 1
may not). Reverted skip to false.
★ NEXT (cheap decisive gate BEFORE building the large route-1): inject a known
gl_FragCoord gradient as the transfer's DEST output for the vista case and check
if it reaches the on-screen vista. Reaches screen ⇒ transfer output IS on the
critical path ⇒ build route 1. Doesn't reach ⇒ transfer output is a dead end ⇒
route 1 won't help, look elsewhere (which RT/tile the composite albedo actually
resolves from).
LEADING FIX CANDIDATES: (a) route the transfer SOURCE through a resolved 1x
companion (we KNOW resolving this source gives varied 22989) instead of the
per-sample MSAA fetch - jaggy but should preserve content; (b) for same-bitwidth
(32bpp↔32bpp) color↔color transfers, replace the lossy decode/re-encode
round-trip with a raw bit-reinterpret copy (matches real EDRAM); (c) diff this
shader's generated SPIR-V vs a known-good reference and apply a targeted
portability fix (SIN/COS-style).

## ★★★ DECISIVE July 20 — COLLAPSE PINPOINTED to the EDRAM→SHM RESOLVE SOURCE
Built reliable DECOUPLED captures (deferred image/buffer copy recorded mid-frame
at the composite draw, read after EndSubmission at swap — the tooling that was
"blocked" before now WORKS). Captured the SAME stage across 3 frames at the
composite draw (fc0 = albedo 0x044B0000). distinct_runs (1 = uniform/flat):

  stage            frame1        frame2      frame3     verdict
  EDRAM tile 608   59880 varied  59488 var   59488 var  DUMP of 608 = ALWAYS OK
  EDRAM tile 1216  456          1 uniform   1 uniform   uniform in steady state
  SHM 0x044B0000   7476 varied  1 uniform   1 uniform   tracks tile 1216
  loaded fc0 image 8689 varied  1 uniform   1 uniform   == SHM byte-for-byte

DECISIVE CONCLUSIONS (all from reliable captures, supersede everything below):
1. TEXTURE LOAD is EXONERATED — the loaded image == shared memory byte-for-byte
   every frame (varied when SHM varied, uniform when SHM uniform). NOT the load.
2. The DUMP (host-RT → EDRAM) WORKS — EDRAM tile 608 reads back 59488
   distinct/varied EVERY frame. The MSAA per-sample compute texelFetch is FINE.
   ⇒ this session's "Adreno collapses raw 4xMSAA dump" theory is a RED HERRING
   (also independently ruled out: msaa=0 forced still navy). The read-resolved-1x
   dump workaround was implemented, changed NOTHING, and is now DISABLED (dormant
   infra kept in vulkan_render_target_cache: ResolveColorRenderTargetForDump,
   DumpPipelineKey.read_resolved_1x, TRANSFER_SRC on color RTs).
3. THE COLLAPSE IS THE EDRAM→SHARED-MEMORY RESOLVE SOURCE SELECTION. The
   composite reads fc0=0x044B0000, whose data is UNIFORM in steady state (a
   single per-frame-changing mid-tone ≈ 0x004B4844/0x0069655D → the flat
   navy/olive vista). Per-frame the resolves to 0x044B0000 are (in order):
     608→737KB ×2, 608→2.9MB(full), 608→737KB ×2, 608→2.9MB, 1216→2.9MB(full)
   The LAST writer each frame is color_base=1216 → 2.9MB full ⇒ the composite
   samples the tile-1216 resolve, and EDRAM tile 1216 is UNIFORM in steady state
   (varied only in frame1). The varied terrain lives in tile 608 but is
   OVERWRITTEN at 0x044B0000 by the uniform tile-1216 resolve.
4. INTERMITTENT: frame1 everything varied, steady-state uniform. Frame1's
   0x044B0000 is varied (7476) which tile 1216 (456) can't source ⇒ the resolve
   feeding 0x044B0000 effectively FLIPS from tile-608-content (varied) in frame1
   to tile-1216-content (uniform) in steady state.

★ ANSWERED (July 21): tile 1216 ownership-transfer instrumented (RTOWN log in
render_target_cache.cc ChangeOwnership, RTTRANSFER log in
vulkan_render_target_cache.cc PerformTransfersAndResolveClears, gated to tiles
608/1216, deduped). CONFIRMED: tile 1216 flips ONCE PER FRAME (not mid-frame
thrashing) between two real render targets: fmt0=k_8_8_8_8 4xMSAA (the vista's
G-buffer RT) and fmt3=k_2_10_10_10_FLOAT non-MSAA (matches the composite's HDR
output, per the frame graph below). EVERY flip, in BOTH directions, triggers a
FULL-target-covering copy-forward draw (rects=1, whole [1216..1816) range) via
a dedicated "transfer shader" fullscreen-rectangle draw
(PerformTransfersAndResolveClears, CmdVkDraw at vulkan_render_target_cache.cc
~5469) - i.e. each frame, right before the vista scene starts, Xenia paints the
PREVIOUS frame's composite output into the fresh MSAA target (expanding 1
sample to 4), and the vista's geometry is expected to fully overwrite it. This
"transfer shader" copy is a DISTINCT, still-UNVERIFIED code path - separate
from the dump shader, resolve-copy shader, and texture load (all proven correct
this session). It is the prime remaining suspect for where the corruption
enters, given RT 1216's raw host image is uniform+decaying (see below) despite
DUMP/RESOLVE/LOAD all being proven correct downstream of it.
★ NEXT: verify the transfer-shader draw's OUTPUT directly - capture the fresh
vista RT (base1216, msaa4x, fmt0) immediately after this transfer draw but
BEFORE the vista's own geometry draws for the frame, to see if it faithfully
carries over the previous frame's (correct) composite content into all 4
samples, or corrupts/blanks it on Adreno. If corrupt, the bug is in the
MSAA<->1x / format-converting transfer shader itself (kColorToColor mode,
TransferShaderKey with source_msaa_samples=k1X dest msaa=k4X or vice versa).

★ ANSWERED (July 20 followup): RT 1216 HOST IMAGE is itself uniform near-black.
Captured RT base_tiles=1216's hardware-resolved host image directly (companion
resolve + decoupled capture) across 12 frames: nonzero 40960/65536, values all
≈ 0xFF090708 (near-black, full alpha), distinct_runs DECAYING 125→117→...→44.
⇒ (a) confirmed: the RT's host image is uniform (render-side), NOT a dump/resolve
collapse. AND the DECAY (monotonic loss of distinct values each frame) is the
signature of a FEEDBACK LOOP: the composite reads albedo 0x044B0000, whose LAST
writer each frame is the resolve of tile 1216 = the composite's OWN prior output,
so each frame re-tonemaps its previous output and decays toward uniform. Frame1
varied because the loop hadn't collapsed yet.
⇒ ROOT CAUSE (localized): the composite's albedo input 0x044B0000 is fed by the
WRONG render target — the composite's own output (tile 1216) is resolved into
0x044B0000, CLOBBERING the vista's FRESH varied albedo (the color_base=608
resolves, which ARE varied). This is a RENDER-TARGET ALIASING / resolve-source
problem in AE's forked vulkan_render_target_cache: two guest RTs (fresh vista
albedo vs composite output) collide at host EDRAM tile 1216 / dest 0x044B0000 and
the wrong one wins on Adreno. Desktop RADV handles the aliasing correctly. This
matches the long-standing hypothesis "Adreno-specific bug in AE's forked RT cache"
and the earlier "3D-as-2D stacked / MRT" RT-cache fixes.
★ NEXT: in vulkan_render_target_cache, trace how guest RTs are keyed/aliased to
host EDRAM tiles and how ownership transfers when tile 1216 is used as BOTH the
vista MRT color1 AND the composite output; compare RT (re)allocation / ownership
transfer / GetOrCreateRenderTarget keying vs upstream. Likely fix: correct RT
ownership/eviction so the composite's output doesn't alias/overwrite the vista's
albedo before the composite samples it (or so the composite samples the correct
fresh-albedo RT). Verify with the decoupled captures (RT1216 host image should
stay varied; 0x044B0000 SHM should stay varied in steady state).

★ (historical) EARLIER open question: WHY is EDRAM tile 1216 uniform in steady state?
tile 1216 is dumped from RT base_tiles=1216 (the vista MRT's RT1 / or the
composite's own output tile — buffer is reused). Two sub-hypotheses to split
next with the SAME decoupled-capture tool:
  (a) RT 1216's HOST IMAGE is itself uniform in steady state (render-side: the
      albedo/RT1 draw isn't producing varied output, or RT1 is cleared and not
      redrawn) — capture RT 1216's host VkImage directly (like the 608 companion
      capture which was VARIED). If uniform ⇒ render/draw-side, not resolve.
  (b) RT 1216 host image is varied but its dump→EDRAM collapses (unlikely — tile
      608 dump works with the same shader). 
Also: confirm whether 0x044B0000 (albedo) is legitimately sourced from RT1216 vs
RT608 by checking the guest resolve's real source RT for the LAST 2.9MB resolve.
TOOLS (kept, all in vulkan_command_processor): TestrigCaptureImageDeferred/
ReadCapturedImage, TestrigCaptureSharedMemoryDeferred/ReadCapturedSharedMemory,
TestrigCaptureEdramDeferred/ReadCapturedEdram, texture_cache.TestrigCaptureBoundImage.
Hooked at composite draw (psh 0x373E65D9ADCF4380) + read at IssueSwap end.

## (superseded July 20) CURRENT LIVE LEAD — July 17 PM: MIP RULED OUT → LOAD-vs-USE
NEW decisive data (supersedes the mip/LOD lead below):
- MODE 3 (force explicit LOD 0 on the composite fetch) → sample STILL spatially
  flat (std<0.5, 1 distinct) AND still temporally alive (frame Δ 36.9/17.5/3.1),
  IDENTICAL to MODE 1. So LOD/mip is NOT the cause.
- Composite fetch attribute log: fc0 dim=k2D, unnorm_coords=FALSE, lod_bias=0,
  use_computed_lod(attr)=true. Coordinate is normalized [0,1] (matches MODE 2
  gradient). So NOT an unnormalized-coordinate mismatch.
- Composite MIP log: fc0 mip_min_level=0, mip_max_level=0, sampler.minLod=0,
  host_image_mipLevels=1, mag=min=mip=POINT(0). Single-mip 1152×640 image,
  point-sampled, no LOD clamp. Definitively NOT a mip/view/sampler-LOD issue.
CONCLUSION: single-mip full-res image + POINT filter + proven per-pixel gradient
coord ⇒ uniform output can ONLY mean the SAMPLED IMAGE CONTENT IS UNIFORM at
composite-DRAW time. Yet the load-dest capture (at swap/end-of-frame) looked
varied. ⇒ a LOAD-vs-USE problem: either (a) the composite binds a DIFFERENT
image instance than the one the load filled (cache thrashing — note "only 4-9
textures alive at swap, multiple 0x44B0/0x4780 instances, seen at 288x160 AND
1152x640"), or (b) the load's shared-mem→scratch→image copy is not
ordered/barrier-visible before the composite draw on Adreno (the earlier
GetUsageMasks SHADER_WRITE fix was in this area but insufficient).
NEXT: capture the EXACT image bound to the composite's fc0 descriptor AT the
composite draw (deferred image→buffer copy recorded at the COMPOSITE hook, read
at swap). If uniform ⇒ confirms (a)/(b); then compare the bound img ptr vs the
LOADINST dest ptr IN THE SAME FRAME, and check load→draw barrier/ordering.
NOTE: the earlier "load-dest image is VARIED" capture may have grabbed a
DIFFERENT 0x44B0 instance than the composite binds (cache has several).

LEADING ROOT-CAUSE HYPOTHESIS (July 17 PM): RESOLVE-happens-after-LOAD, or the
resolve's shared-memory compute-WRITE isn't barrier-visible to the texture
LOAD's read on Adreno. The composite's texture LOAD reads guest shared memory;
if the EDRAM→shared-memory RESOLVE for 0x044B0000 hasn't completed/been
published when the load runs, the load reads UNRESOLVED (uniform) data → a
uniform image → the observed flat, per-frame-changing vista. GBUFGPU readback
being "varied" was taken at SWAP (end of frame, after the resolve) — NOT at
composite-draw time, so it doesn't contradict this. The committed GetUsageMasks
SHADER_WRITE barrier fix was in exactly this area but evidently insufficient.
Tooling caveat: TestrigCaptureBoundImage records the copy but
TestrigReadCapturedImage yields nothing — AwaitAllQueueOperationsCompletion
can't flush the still-open submission at swap. Needs a forced submission/flush
before the read to confirm on-device.
NEXT (code, no device cycle): trace the ordering of (1) the vista's
EDRAM→shared-memory resolve compute write and (2) the composite texture LOAD
read of that shared memory; verify the write→read barrier
(VulkanSharedMemory usage masks / MakeRangeValid / upload ordering) and compare
to upstream. This is now the prime suspect for the FIX.

## (superseded) MIP/LOD COLLAPSE lead (July 17 AM)
DECISIVE on-device diagnostic (translator override, `kTestrigHalo3Mode` in
spirv_shader_translator.h, gated to composite PS hash 0x373E65D9ADCF4380):
- MODE 1 (output the composite's RAW albedo sample, bypassing the tonemap):
  the vista is SPATIALLY FLAT (std<0.5, 1 distinct color across the whole
  screen) but TEMPORALLY ALIVE — each frame a different single color that
  tracks the animating scene (frame means 69/66/62 → 42/40/38 → 50/47/45 →
  76/71/50; frame-to-frame Δ up to 25.8). Spatially-flat-but-scene-tracking =
  reading ONE texel/mip per frame (the whole-image AVERAGE), not per-pixel.
- MODE 2 (output the fetch's normalized COORDINATE as R=x,G=y): a clean smooth
  per-pixel GRADIENT (std 29.6/51.3/29.4, 1049 distinct; R rises L→R 132→248,
  G rises T→B 63→241). So the coordinate is CORRECT and per-pixel.
CONCLUSION: correct per-pixel coordinate + proven-varied mip-0 image + flat
"average" sample output ⇒ the sampler reads the MAXIMUM LOD (top ~1×1 mip =
the scene average) for every pixel. This is a **mip/LOD-selection bug**, NOT
the tonemap math and NOT a load/barrier ordering issue (ordering would give
stale/black, not a live scene-tracking average). The old "tonemap collapses"
and "load/barrier ordering" leads are RETRACTED.
WHY max LOD (to investigate, no device cycle needed — inspect code):
  (a) the composite G-buffer tfetch's LOD source — explicit LOD / lod_bias /
      register-gradients / use_computed_lod handling in this fetch;
  (b) the host texture's mip count & the VkImageView levelCount/baseMipLevel
      vs the resolve only filling mip 0 (are mips even generated? if the view
      exposes >1 level and the sampler maxLod is high, Adreno picks the top);
  (c) the VkSampler mip params (minLod/maxLod/mipLodBias/mipmapMode) for this
      fetch — compare AE vs upstream.
NEXT: read vulkan_texture_cache sampler/view creation + mip-count logic and the
tfetch LOD plumbing in spirv_shader_translator_fetch.cc; find the AE-vs-upstream
divergence that lets LOD go to max. Confirm fix with MODE 1 (sample must become
the per-pixel gradient/terrain). Optional MODE 3: force explicit LOD 0 on this
fetch — if the sample then varies, it pins the bug to the LOD source.

## KEY DATA (addresses / shaders / frame graph)
Frame graph (core→screen):
1. 3D vista renders 4x-MSAA → EDRAM tile 608 (+1216 as RT1).
2. Resolves to G-buffer textures in shared memory: 0x044B0000 (albedo, tiled,
   1152x640, k_8_8_8_8/fmt6, load_shader2/32bpb), 0x04780000, 0x043FC000
   (HDR, 288x160, fmt32, load_shader3/64bpb).
3. Deferred composite 831761DE(vs)/373E65D9(ps) samples fc0=0x044B0000 +
   fc2=0x043FC000, tonemaps, writes EDRAM tile 1216 (non-MSAA).
4. EDRAM 1216 resolves to front buffer 0x04E20000.
5. IssueSwap presents 0x04E20000 (=SWAPSRC).

Composite PS 373E65D9 ucode (tiny):
```
mad r0.__zw, r0.yyyx, c5.yyyx, c5.wwwz   // tf2 coord = same as tf0 coord
tfetch2D r1.xyz, r0.xy, tf0              // albedo (fc0 = 0x044B0000)
tfetch2D r0,     r0.wz, tf2, linear      // HDR   (fc2 = 0x043FC000)
r1.xyz = albedo*tf2.w + tf2.xyz
dp4 x3 (c229/230/231 color matrix) → r0.xyz ; dp3 luminance (c255)
log/exp tonemap (c232) ; min c3 ; mad c3 ; oC0 = r1.yzx*r0.yzx
```
Terrain shaders (OFFSCREEN, not the visible vista, superseded): producer
9EA48FC2 (memexport), consumer 488D9488 (predicate p0=(c228.x==0)&&(r0.z!=0)).

## 🔧 FIXES MADE & KEPT (real, but none alone fixes the vista yet)
- VulkanSharedMemory::GetUsageMasks kComputeWrite access_mask: was
  VK_ACCESS_SHADER_READ_BIT → corrected to SHADER_WRITE (matches upstream).
  Real barrier-correctness fix.

## 🛠️ DIAGNOSTIC TOOLS (kept in tree; how to use)
- GPU pipeline trace: `adb shell setprop debug.canary.testrig.gputrace 1`
  (default off) → ordered `GPUTRACE seq=N frame=F STAGE detail` for
  DRAW/MEMEXPORT/TEXLOAD/RESOLVE/SWAP.
- Buffer readback: `TestrigReadbackAndLogBuffer(tag,buf,off,size)`.
- Image readback (flushable pt): `TestrigReadbackAndLogImage(...)`.
- Decoupled image capture (mid-frame safe): `TestrigCaptureImageDeferred(...)`
  in a load, `TestrigReadCapturedImage(tag)` at swap.
- Added deferred `CmdVkCopyImageToBuffer`.
- Shader ucode dumps on device: `<files>/xeniaae/shaderdump/shader_<hash>.ucode.*`.
- Repro: launch Halo 3, wait ~50s for menu; clear cache/shaders + xe.log between
  cold boots; take 3+ samples (see HALO3_MENU_INVESTIGATION.md commands).

## 🧪 TEST LOG (chronological; input → result). Don't repeat these.
1. Precision audit of every ALU op → all exact/1-ULP except SIN/COS (fixed).
2. Cross-draw ordering (barrier every draw) → correct.
3. CPU constants boot-to-boot (producer) → bit-identical.
4. Intra-draw invocation ordering → not the cause.
5. Stale/prefill buffer content → zero at first touch.
6. Blend mode (disable/additive) → changes shade, not flatness.
7. Compute-memexport vs rasterizer-discard → identical fill %, ruled out stores.
8. Epsilon-snap TRUNC/FLOOR (re-test post-SIN/COS) → no change.
9. Force-vis fullscreen terrain 488D9488 (+cull/depth/blend off) → still navy = offscreen.
10. Full frame-graph map (RTMAP/TEXSRC/RESOLVE/RESOLVESRC/SWAPSRC) → deferred scene.
11. MSAA=0 forced → still navy.
12. Magenta composite output → whole bg magenta = composite reaches screen.
13. Guest-RAM dump of G-buffer/front-buffer → all zero (host-RT path, GPU-side).
14. GBUFGPU (shared-mem readback) → G-buffer FULL of varied content.
15. Barrier fix kComputeWrite READ→WRITE → real fix, but still navy.
16. Forced full GPU idle after every G-buffer resolve → still navy (not sync).
17. Tiling formula compare (tilecmp.c) → byte-identical to upstream.
18. Texture load dispatch params (TEXDISPATCH) → all correct (36x20, 1152x640…).
19. Decoupled load host-image capture of 0x44B0 → VARIED (load works!).
20. Capture tf2 (0x43FC) image → VARIED (HDR).
21. Capture front buffer 0x04E20000 top-left → UNIFORM (distinct_runs=1) = output collapses.
22. Dump composite PS constants → all correct/sane.
23. Output composite fc0 SAMPLE raw → uniform brown (low-contrast, misleading).
24. Output composite fc0 COORD → correct 0..1 gradient.
25. Force LOD 0 on composite fetch → still uniform (not mip).
26. Output fc0 sample AMPLIFIED → uniform BLACK (sample is uniform ~avg, NOT the
    varied image) → composite samples a different-content/instance texture.
27. Amplified + LOD 0 → still uniform black (not mip; instance/dimension mismatch).
28. Log composite bound fc0/fc2 texture vs loaded instance → SAME image ptr
    (0x77F8583D60), same key. Instance-mismatch RULED OUT.
29. Load-fail markers on every return-false path for 0x44B0 → load COMPLETES
    (LOADINST fires, no LOADFAIL). Not a load failure.
30. Composite output = front buffer 0x04E20000 vista region → EXACTLY uniform
    (distinct_runs=1) while both inputs varied → composite MATH collapses.
31. Measure-sample attempts (front-buffer capture / GBUFGPU on 0x04E20000) →
    tooling-blocked (front buffer not reloaded per-frame; readback gate). Redo
    via EDRAM-1216 resolve readback or a dedicated always-fires capture.
32. MODE 1 raw-albedo-sample output → vista SPATIALLY FLAT (std<0.5, 1 color)
    but TEMPORALLY varying/scene-tracking (frame Δ up to 25.8) = single
    texel/mip per frame = the scene AVERAGE.
33. MODE 2 coordinate output → clean per-pixel GRADIENT (std ~30/51/29, 1049
    distinct). Coordinate PROVEN correct with a reliable readout. ⇒ collapse is
    MIP/LOD selection (sampler reads max LOD / top mip = average), NOT tonemap,
    NOT coordinate, NOT load ordering. Retract those.
34. MODE 3 (forced explicit LOD 0) → still flat + temporally alive (Δ 36.9/17.5/
    3.1). LOD/mip NOT the cause.
35. Composite fetch attr log: fc0 k2D, unnorm_coords=FALSE, lod_bias=0. Not an
    unnormalized-coord mismatch.
36. Composite MIP log: fc0 mip_min=0 mip_max=0 minLod=0 host_mipLevels=1
    POINT filter. Single-mip full-res, point-sampled. Mip/view/sampler ruled out.
37. LOADDEST vs COMPOSITE_CAP: SAME VkImage ptr (0x...96B40) — the composite
    binds the EXACT image the load wrote. NO instance aliasing. Loaded 6x/frame
    (load_mips=false, level_last=0). ⇒ the LOAD produces a uniform image.
    NEXT: gputrace ordering of 0x44B0 RESOLVE vs its TEXLOAD (resolve-after-load?)
38. gputrace produced no RESOLVE lines (flag/gating); GPU-side content readback
    tooling (deferred capture + TestrigReadbackAndLogImage) all funnel through
    AwaitAllQueueOperationsCompletion which bails while a submission is open —
    can't reliably read GPU buffers/images mid-frame. Capture RECORDS (pending
    set) but the swap-time read never fires. Tooling wall for GPU-side content.

## ★★ CONSOLIDATED ROOT-CAUSE (July 17 PM) — THE TEXTURE LOAD PRODUCES UNIFORM
Chain of solid, reliable-readout facts:
- Composite point-samples a single-mip 1152x640 image at a proven per-pixel
  gradient coordinate, minLod 0 → UNIFORM output (spatial std<0.5). (modes 1/2/3
  + mip log)  ⇒ the sampled image is spatially UNIFORM at draw time.
- LOADDEST img ptr == COMPOSITE bound img ptr (same VkImage, e.g. 0x..EB70).
  NO instance aliasing. The composite samples the exact image the load wrote.
- The vista texture (0x44B0) is loaded ~6x PER FRAME (load_mips=false, single
  mip). ⇒ THE LOAD WRITES A UNIFORM IMAGE for this resolved, TILED, k_8_8_8_8
  (load_shader=2, 32bpp) G-buffer.
Two remaining sub-causes for the uniform load output:
  (a) the load compute (untile load_shader=2) mis-executes on Adreno for this
      tiled format/size (note: "load broken" was only ever RETRACTED due to an
      invalid no-op probe, never actually disproven);
  (b) the load reads a shared-memory source that is uniform at load time.
  NOTE: "full GPU idle after every resolve didn't help" argues AGAINST (b)
  (source would be complete) and FOR (a) — the load compute itself. Also the
  "6 loads/frame" = over-invalidation worth understanding.
TENSION to resolve: July-16 "loaded image VARIED (1578 distinct)" contradicts
today's "load-dest sampled uniform". Likely the July-16 capture caught a
different instance/moment, OR an early reload was varied and a later reload
(reading a cleared/!resolved source) overwrote it uniform. The readback tooling
can't currently settle this on-device.
NEXT (code, recreate-upstream): inspect the load_shader=2 untile compute + the
resolve→shared-memory→load data/barrier flow for this tiled k_8_8_8_8 G-buffer,
and WHY it reloads 6x/frame (over-invalidation). Compare to upstream canary
(reinstall the desktop oracle fresh per user). Front buffer uses the same load
path but presents crisp UI — so isolate what's different about THIS texture
(tiled + k_8_8_8_8 + resolved-source + 6x reload).
39. Re-upload CLOBBER hypothesis RULED OUT. SMUPLOAD (guest-RAM upload covering
    0x044B0000) fires only 2x, BOTH early (before the varied resolves), never
    after. GBUFGPU at 0x044B0000 after the real resolve = VARIED (changes 6735,
    then 7476). So shared memory (the load SOURCE) is varied and NOT clobbered.
    ⇒ the uniform load output is a resolve→load ORDERING/BARRIER issue or a
    frame-order issue (an early LOADDEST occurs between a uniform CLEAR
    (0xFFFFFFFF, changes=1) and the varied resolve). Untile compute reads
    shared_memory.buffer() directly (unscaled; res-scale=1 so NOT scaled_resolve).
    NEXT: inspect the barrier between the resolve's shared-memory compute WRITE
    and the texture load's compute READ; and the per-frame order of
    load(0x44B0) vs resolve(0x44B0) vs composite draw.
40. Frame-order instrumentation (HALO3FRAME markers): within the composite's
    frame, the last LOADDEST(0x44B0) occurs AFTER a varied GBUFGPU resolve and
    before the HALO3COMPOSITE draw. So the load that feeds the composite reads
    varied shared memory, yet the composite samples uniform. Frame-order
    hypothesis RULED OUT.
    ⇒ SOLID CONCLUSION: the texture LOAD from resolved shared memory produces a
    UNIFORM image for this tiled k_8_8_8_8 (load_shader=2, 32bpp) G-buffer,
    despite a varied source and no aliasing. The break is the untile LOAD path
    (untile compute / scratch->image copy / re-dispatch-vs-cache) on Adreno.
    CAVEAT: the GBUFGPU/readback diagnostics use AwaitAll/EndSubmission, which
    inserts submission boundaries that PERTURB the real execution order — so
    black-box on-device ordering analysis is now unreliable. Further progress
    needs either (i) a non-perturbing GPU readback, or (ii) upstream comparison.

## ★★★ FINAL LOCALIZATION (July 17) + RECOMMENDED PATH
The Halo 3 vista bug = the TEXTURE LOAD of the resolved, TILED, k_8_8_8_8
G-buffer (0x044B0000, load_shader=2) yields a spatially-UNIFORM image on Adreno,
even though: shared memory source is varied, no instance aliasing, coordinate is
a correct gradient, single-mip point-sampled, no scaled_resolve (res-scale=1),
no re-upload clobber, and the feeding load runs after the varied resolve.
RECOMMENDED NEXT (recreate-upstream, needs oracle): reinstall the desktop canary
fresh and diff AE's load path vs upstream for tiled k_8_8_8_8 resolved textures:
  - the untile LOAD compute shader (texture_load*.xesli / load_shader=2) and its
    dispatch params (guest_pitch/host_pitch/offset/size_blocks) — AE uses the
    `xesl` framework which DIFFERS from upstream;
  - the scratch-buffer -> image copy region math;
  - whether AE re-dispatches the load correctly after a resolve invalidation.
Do NOT re-chase: geometry, memexport, MSAA, presentation, EDRAM resolve,
tonemap, coordinate, mip/LOD, sampler, unnormalized coords, instance aliasing,
re-upload clobber, frame-order — ALL ruled out.

## ★★★★ BREAKTHROUGH LEAD (July 18) — AE REWROTE THE TEXTURE-LOAD SHADERS
Source diff of AE vs upstream canary (canary-git @6e5b8324f = AE's fork base):
the .cs.xesl entry points are IDENTICAL, but the .xesli INCLUDES that hold the
untile logic are REWRITTEN in AE:
  - texture_load_32bpb.xesli, texture_load.xesli, texture_address.xesli, +many.
KEY behavioral divergence in the untile compute:
  - UPSTREAM reads/writes the source/dest via BYTE-ADDRESSED buffers
    (byte_buffer_align16_load16_xe / store16), source bound at offset 0, shader
    uses ABSOLUTE byte addresses.
  - AE reads/writes via TYPED uint4 STORAGE BUFFERS
    (xesl_typedStorageBufferLoad/Store), source bound at a NON-ZERO offset
    (base_page<<12), shader uses a RELATIVE uint4 element index (byte_offset>>4).
Verified NOT bugs: guest_offset=0 relative (no double-count, vulkan_texture_cache
.cc:1619), tiling math equivalent (tilecmp.c), offsets/pitches correct.
⇒ PRIME ADRENO-SPECIFIC SUSPECT: AE's typed uint4 storage-buffer read, bound at
a non-zero offset, mis-reads on Adreno for this large tiled k_8_8_8_8 resolved
texture (returns uniform), while linear/UI loads (different path/params) work.
This fits: bug is Adreno-only, desktop RADV renders correctly (byte buffers OR
different driver behavior), load output uniform, no aliasing/clobber/ordering.
FIX EXPERIMENT (on Adreno, where the bug lives): make AE's texture_load_32bpb
(and the shared load path) use byte-addressed buffer access like upstream, OR
bind the typed source buffer at offset 0 and add base_page<<12 to guest_offset,
OR test typed-buffer robustness. The desktop oracle CANNOT reproduce this
(RADV != Adreno), so confirmation must be on-device.

## ORACLE STATUS (July 18)
- canary-git @6e5b8324f (AE's exact fork base) = the upstream SOURCE, present and
  used for the diff above. Build is current (Jul 12 binary) BUT HANGS on Halo 3
  (REENTER_DIAG_CP wait-loop, execute_calls=0 -> "Cheap-skate exit") = the "no
  good" the user reported. Not a runnable oracle right now.
- The prebuilt release canary/ (Mar 24) previously booted Halo 3 to a clean menu
  on RADV (see project_xenia_ae_halo3_wine_oracle memory) - use for VISUAL ref.
- IMPORTANT: RADV desktop cannot reproduce the Adreno-specific untile bug, so the
  oracle's runtime value is limited to (a) visual "should look like" reference
  and (b) upstream source (which gave the breakthrough above). Runtime byte
  comparison would only confirm upstream's byte-buffer load works (already known).

## FIX EXPERIMENT #1 (July 18): offset-0 source rebind — FAILED/INCONCLUSIVE
Tried: for the vista (0x44B0), bind the typed uint4 source storage buffer at
offset 0 (range = base_page<<12 + guest_base_size, ~75MB) and set
guest_offset = base_page<<12 (absolute addressing), to test whether the
NON-ZERO typed-buffer offset mis-reads on Adreno.
Result: did NOT render the vista (still uniform/black) AND the ~75MB storage-
buffer binding destabilized (maxStorageBufferRange=128MB, so under limit, but
the large binding likely stalled the load queue). REVERTED.
COMPLICATION: after this, the game repeatedly hit the intermittent
STREAMING-SEMAPHORE DEADLOCK (1-vertex draw loop vsh=C049A8C9, pre-menu black
screen) - baseline (reverted) ALSO stuck, and a full device reboot did NOT
clear it. So the experiment was UNEVALUABLE (menu never rendered in fix OR
baseline). Strong suspicion: the accumulated heavy diagnostics (GBUFGPU
AwaitAll mid-resolve, composite fence-wait capture, per-frame readbacks) force
GPU idles that trigger Halo 3's streaming deadlock more often.
NEXT: strip the perturbing diagnostics to a MINIMAL stable build first, confirm
the menu renders reliably, THEN retest any load fix. The offset-0/large-range
approach is a dead end; if pursuing the typed-buffer theory, change the buffer
ACCESS model (byte-addressed like upstream) rather than the binding offset.

## CLEAN BUILD RESTORED (July 18)
Stripped the perturbing GPU-stalling diagnostics (GBUFGPU AwaitAll mid-resolve
DISABLED; composite fence-wait image capture REMOVED; swap-time capture read
REMOVED; HALO3FRAME marker REMOVED). Result: Halo 3 menu renders STABLY again
(menu-panel std ~36 consistent; healthy MEMEXPORT/ANYDRAW, no C049A8C9 1-vertex
deadlock loop). Vista still uniform (std ~0.5) as expected. CONFIRMED: the heavy
readback/capture instrumentation was aggravating Halo 3's streaming-semaphore
deadlock (each AwaitAll/fence-wait forces a GPU idle that collides with the
game's streaming sync). Lightweight CPU-only logs (LOADDEST/SMUPLOAD/HALO3LOD/
mip) left in - non-perturbing. Clean stable baseline ready for a real load fix.

## BYTE-BUFFER / SCALAR-LOAD FIX — SETUP REQUIREMENTS (July 18-19)
Confirmed on the CLEAN stable build: vista uniform + FLICKERING (navy, blue
channel swings 89->76->84 per frame) = the composite reads ONE value per frame
that tracks the animating scene. Ordering/barrier RULED OUT (even the GBUFGPU
AwaitAll forced-idle runs still showed uniform). So the untile LOAD compute
(AE's forked typed-uint4 rewrite) is the culprit.
Candidate Adreno fix: AE's GLSL untile reads the source via a 128-bit uint4
std430 load (xesl_typedStorageBufferLoad); switch to 4x32-bit SCALAR loads
(xesl_uintVectorBufferLoad4) - a classic Adreno vec4-load workaround.
BLOCKER: the load shaders are PRECOMPILED SPIR-V embedded as C headers
(bytecode/vulkan_spirv/texture_load_32bpb_cs.h, "Generated with xb buildshaders",
LocalSize 4 32 1). The Android build does NOT compile shaders. To change the
untile I must regenerate the bytecode via `xb buildshaders` (xenia-build.py).
The DESKTOP canary-git HAS this toolchain (glslangValidator/glslc/spirv-as on
system; buildshaders found Vulkan SDK at /usr). AE lacks xb. So the desktop
build's REAL value = SHADER COMPILER (not runtime oracle - RADV can't repro).
PLAN: (1) modify AE texture_load_32bpb.xesli (uint4 read -> uintVector load4),
(2) compile via canary-git's buildshaders using AE's xesli framework,
(3) copy regenerated texture_load_32bpb_cs.h into AE, (4) build + test on Adreno,
(5) verify UI textures still load (same shader) so a correct port is
distinguishable from a broken one.

## ★★★★★ DEFINITIVE (July 19) — UNTILE IS FINE; SOURCE DATA IS UNIFORM AT LOAD
Built a working xesl->SPIR-V shader-recompile pipeline (scratchpad/aecompile.py,
uses desktop glslang/spirv-opt; -DXESL_LANGUAGE_GLSL=1; validated: unmodified
recompile has an IDENTICAL 357-op histogram to AE's committed bytecode). Used it
to instrument the untile LOAD compute (texture_load_32bpb) directly:
 - SCALAR-load variant (uint4 -> 4x uint loads): vista STILL uniform; UI still
   loads fine. => the vec4-vs-scalar source READ mechanism is NOT the bug.
 - POSITIONAL write (ignore source, write block_index): vista VARIED (std 52,
   84 distinct, static). => untile dispatch/addressing/WRITE all WORK.
 - INDEX write (write block_offset_guest, the tiled source address): vista VARIED
   (std 65, 126 distinct, static). => the tiled-address computation
   (XeTextureTiledOffset2D) VARIES per invocation on Adreno - it is CORRECT.
CONCLUSION (proven): block_index varies, the source ADDRESS varies, the write
works - yet reading source[address] yields UNIFORM. So THE SOURCE BUFFER
(shared memory at the load's read) IS UNIFORM at load-execution time. The untile
load is NOT broken; it faithfully reads uniform data. RETRACT "untile compute
broken" / "AE load-shader rewrite is the bug".
=> ROOT CAUSE is the RESOLVE->LOAD data flow: the vista's EDRAM->shared-memory
resolve result is NOT visible in the shared-memory buffer when the untile load
reads it (missing/insufficient barrier between the resolve's compute WRITE and
the load's compute READ, OR the load reads before the resolve on Adreno).
NOTE on GBUFGPU "shared memory varied": that readback used AwaitAll at IssueCopy
(perturbing - forces the resolve complete + flushes caches), so it saw varied;
the real in-frame load (no such flush) reads uniform. The forced-idle "didn't
help" because the idle was at the resolve, not enforced as a cache-flushing
buffer barrier before the load's read.
NEXT: inspect the resolve's shared-memory compute WRITE usage/barrier vs the
load's Use(kRead) - ensure a real buffer memory barrier (kComputeWrite->kRead,
cache flush+invalidate) is emitted between them on Adreno. Prime fix candidate.

## ★★★★★★ ROOT CAUSE (July 19) — RESOLVE READS STALE EDRAM (scene not in EDRAM)
Instrumented the RESOLVE copy shaders (via the recompile toolchain) to write a
POSITIONAL pattern to shared memory, ignoring EDRAM:
 - resolve_full_32bpp only (copy_shader=6): vista still uniform (ambiguous - the
   fast resolve copy_shader=0 also writes 0x044B0000 and overwrote it).
 - resolve_full_32bpp AND resolve_fast_32bpp_1x2xmsaa both positional: vista
   shows VARIED, STATIC pattern (std 53, 31 distinct). DECISIVE.
=> The resolve's shared-memory WRITE fully reaches the composite's texture load
(resolve -> shared memory -> load -> composite ALL WORK). So the uniform vista
is because the resolve reads STALE/UNIFORM EDRAM - the rendered 3D scene is NOT
in the EDRAM buffer when the resolve copy shader reads it. Flickers because the
stale EDRAM residual shifts slightly per frame.
Vista resolve uses TWO copy shaders on 0x044B0000: kFull32bpp (6, ~1112x/frame)
and kFast32bpp1x2xMSAA (0, ~738x/frame).
=> NEXT: the bug is the SCENE-RENDER -> EDRAM -> resolve-read path. The resolve
reads edram_storage_buffer (UseEdramBuffer(kComputeRead), render_target_cache.cc
:1255). The host render targets must be STORED to the EDRAM buffer (and that
store barriered) BEFORE the resolve reads it. Inspect the host-RT -> EDRAM store
("dump")/ownership-transfer and its barrier vs UseEdramBuffer(kComputeRead) -
that ordering/visibility is the prime fix candidate. GBUFGPU+AwaitAll saw varied
because the full idle flushed the RT store; normal operation does not.

## EDRAM-STORE PATH TRACE (July 19) — narrowed to dump-write -> resolve-read
The Resolve() calls DumpRenderTargets (host-RT -> EDRAM) before the resolve copy
(render_target_cache.cc:1083). Traced with logging:
 - Vista resolve: dest=0x044B0000 -> dump_base=608 (EDRAM tile). Composite
   resolve -> dump_base=1216.
 - DumpRenderTargets(608) is NON-empty: rectangles=1 (dump runs, not skipped).
 - The 608 dump reads RTs with rt_base_tiles=608, cur_stage=0x400
   (COLOR_ATTACHMENT_OUTPUT), cur_access=0x180 (COLOR read+write), cur_layout=2
   (COLOR_ATTACHMENT_OPTIMAL) - CORRECT usage, so the RT-visibility barrier is
   fine and the RT holds the scene render.
 - The EDRAM write->read barrier (UseEdramBuffer kComputeWrite->kComputeRead,
   whole-buffer) looks correct.
YET the resolve reads STALE/uniform EDRAM (proven by the positional-resolve
test). So the break is the DUMP-WRITE(EDRAM 608) -> RESOLVE-READ(EDRAM 608)
link, despite the dump running on a valid RT.
KEY CLUE: MULTIPLE RT instances at base_tiles=608 - TWO formats (fmt=0 and
fmt=7) across 4 distinct VkImages (0x..462E50,5E08D0 fmt0; 0x..56E450,5EE070
fmt7). A deferred MRT G-buffer whose targets all claim EDRAM tile 608 would
OVERWRITE each other in the EDRAM buffer when dumped; the 0x44B0 (albedo, fmt0)
resolve reads tile 608 but may get whichever RT was dumped last.
NEXT: (a) verify the resolve copy shader's EDRAM READ offset == the dump's WRITE
offset (dump_base 608) - a mismatch = stale read; (b) investigate the multiple
RTs at the same base_tiles=608 (EDRAM aliasing / MRT placement) - the dump may
write the wrong format's RT to 608, or dumps overwrite each other. Candidate: a
generated-dump-shader positional-write test to confirm dump->resolve visibility.

## ★★★★★★★ July 19 — DUMP READS A UNIFORM RT (scene-render or MSAA-fetch)
#1 (offset mismatch) RULED OUT: GetCopyEdramTileSpan (dump WRITE base) and the
resolve copy shader (EDRAM READ base) both derive from color_edram_info.base_tiles
-> both tile 608. No mismatch.
#2 DECISIVE dump-positional test: overrode the generated DUMP shader's store_value
with a per-invocation positional value (edram_sample_address), ignoring the RT
sample. Vista showed a VARIED, STATIC pattern (std 49, 449 distinct). So the
DUMP -> EDRAM -> resolve -> load -> composite -> screen path ALL WORKS.
=> The DUMP READS A UNIFORM RENDER TARGET. The G-buffer RT the dump samples
(rt_base_tiles=608, color-attachment usage) is UNIFORM. Two remaining
hypotheses:
  (A) the 3D vista SCENE renders uniform to its G-buffer RT on Adreno (a
      3D-scene rendering issue - vertex/pixel/depth/cull for the SCENE draws,
      NOT the composite), OR
  (B) the DUMP's multisampled RT fetch (source_is_multisampled path in
      GetDumpPipeline) is broken on Adreno, reading uniform from a varied MSAA
      RT (note: the working composite RT at tile 1216 is NON-MSAA; the vista
      G-buffer RTs are MSAA - copy shaders used were kFast32bpp1x2xMSAA(0) and
      kFull32bpp(6), so 1x/2x MSAA).
Everything from the RT onward (dump addressing, EDRAM, resolve, shared memory,
untile load, composite, present) is PROVEN WORKING. The break is RT-content ->
dump-sample.
NEXT: distinguish (A) vs (B): read the G-buffer RT image content (0x..462E50)
- if varied => (B) MSAA-fetch bug in the dump; if uniform => (A) the scene
renders uniform. Log the RT's msaa_samples; if 1x, (B) is out => (A). Then, for
(A), instrument/inspect the vista SCENE draws (the 3D geometry that writes the
G-buffer), which have NOT been directly tested (earlier "geometry ruled out" was
a different, deferred-terrain red herring).

## ★★★★★★★★ July 19 — THE G-BUFFER RT IS UNIFORM (bug is upstream of the resolve)
Vista G-buffer RT (base_tiles=608) is msaa_samples=k4X (4x MSAA), fmt=0.
Dump sample-0 test: forced the dump's multisampled fetch to read SAMPLE 0 only
(instead of the computed source_sample_id). Vista STILL uniform (flickering).
=> Even the base MSAA sample of the RT is uniform. Since sample-0 texelFetch is
standard on Adreno and the non-MSAA composite RT (1216) works, the RENDER TARGET
CONTENT ITSELF IS UNIFORM. The bug is UPSTREAM of the resolve/dump - in the
3D vista SCENE -> 4x-MSAA G-buffer RT.
PROVEN-WORKING (this session, do NOT re-chase): untile load, tiled addressing,
shared memory, resolve copy, EDRAM buffer, dump write+addressing, composite PS,
present. The ONLY remaining suspect is how the 4x-MSAA G-buffer RT gets its
(uniform) content.
Three sub-hypotheses for the uniform RT:
  (A1) the 3D scene draws render uniform to the MSAA G-buffer on Adreno
       (MSAA rasterization / the scene's vertex-pixel pipeline), OR
  (A2) RT OWNERSHIP/aliasing: the scene draws to one RT instance at tile 608 but
       the dump reads a DIFFERENT (cleared) instance - note MULTIPLE RT images
       at base_tiles=608 (fmt=0 x2, fmt=7 x2), suggesting instance churn, OR
  (A3) the MSAA render-pass store/ownership-transfer leaves the RT cleared.
NEXT: capture/inspect the 4x-MSAA G-buffer RT's content at scene-render time
(resolve it to a readable image), and check whether the scene draws that write
tile 608 actually execute and are the same RT instance the dump reads.
NOTE: 4x MSAA is the common thread (2D UI = non-MSAA works; composite = non-MSAA
works; only the 4x-MSAA G-buffer is uniform) - MSAA render-target handling on
Adreno is the leading area.

## ★★★★★★★★★ ROOT CAUSE CONFIRMED (July 19) — 4x MSAA RT READ COLLAPSES VARIATION
Research (Vulkan docs / Arm best practices): AVOID loadOp=LOAD and use
storeOp=DONT_CARE for multisampled attachments - tiled GPUs (Adreno) don't
handle storing/loading RAW multisample color well. AE MUST use LOAD+STORE on the
4x-MSAA G-buffer to emulate EDRAM per-sample storage - the anti-pattern.
Decisive scene-render tests (SPIR-V translator force of NON-composite pixel
shaders, kTestrigHalo3Mode=4):
 - Force scene PS output = MAGENTA (uniform): vista changed navy -> uniform
   YELLOW (magenta tonemapped). => the scene DOES rasterize and its output flows
   scene -> RT -> dump -> resolve -> composite -> vista. The whole path works
   with a value.
 - Force scene PS output = gl_FragCoord GRADIENT (per-pixel varied): vista =
   uniform BLACK (~= pixel-0 value (0,0,0.5) tonemapped), UI shows the gradient
   (so the force works). => per-pixel VARIATION collapses to a single value in
   the scene -> RT -> dump path.
Since dump-positional and resolve-positional tests proved dump->EDRAM->composite
PRESERVES variation, the collapse is the DUMP'S READ of the 4x-MSAA render
target: it effectively reads ONE texel/value for all pixels on Adreno (magenta
uniform->yellow; gradient's pixel-0->black). This is the raw-MSAA sampled-image
fetch / MSAA store-load failing on Adreno.
ROOT CAUSE: AE reads the 4x-MSAA G-buffer render target (via the dump's
multisampled texelFetch / the LOAD+STORE raw-MSAA path) and it collapses
per-pixel variation to a single value on Adreno, while RADV (desktop) preserves
it. Only 4x-MSAA targets fail (2D UI, composite = non-MSAA = fine).
FIX DIRECTIONS (to try):
  1. Avoid the raw-MSAA sampled read: make the dump read work per-sample
     correctly on Adreno, OR
  2. Render the G-buffer NON-multisampled (force msaa=1x for host RTs) as an
     Adreno workaround (loses AA but restores content), OR
  3. Use a proper vkCmdResolveImage / a resolve attachment instead of sampling
     the raw MSAA image.
Compare AE's MSAA RT image creation + dump source view vs upstream; check
usage flags (SAMPLED on a multisampled image), and whether the multisampled
image view the dump samples is created correctly on Adreno.

## ★★★★★★★★★★ FINAL ROOT CAUSE (July 20) — Adreno raw 4x-MSAA round-trip collapse
Ruled out that it's an AE config/limit/upstream bug:
 - Device limits: MSAALIMITS fbColor=0x7 sampledColor=0x7 sampledInteger=0x7 -
   the Adreno 740 DOES support 4x MSAA SAMPLED color images (0x7 & 0x4 = 4x). So
   creating a 4x-MSAA sampled G-buffer is valid; not a limit gap.
 - RT VkImage: samples=4, usage=SAMPLED|COLOR_ATTACHMENT, view type 2D, format
   correct - all valid and MATCHES upstream canary-git (diffed).
 - Dump shader: integer-coord texelFetch with a sample operand (fetch=true) -
   correct. Source read coordinate PROVEN to vary (dump-source-coord test: vista
   varied std 50).
 - Render pass: samples=4, loadOp=LOAD, storeOp=STORE - matches upstream; this is
   the raw-MSAA store/load path (the Vulkan/Arm best-practice ANTI-PATTERN for
   tiled GPUs: "avoid loadOp=LOAD and use storeOp=DONT_CARE for MSAA").
CONCLUSION: The 4x-MSAA G-buffer's RAW multisample round-trip (storeOp=STORE the
raw samples, then compute-shader texelFetch them per-sample) COLLAPSES per-pixel
variation to a single value on this Adreno driver, while RADV (desktop)
preserves it. The scene renders correctly (magenta force -> vista changed;
gl_FragCoord gradient force -> vista uniform => variation lost in scene->RT->
dump). This is a driver-level MSAA limitation Xenia's EDRAM emulation hits
because it must preserve raw per-sample data - upstream doesn't hit it because it
targets desktop. Root cause is NOT a code bug in AE; it's the raw-MSAA-on-Adreno
architecture.
FIX OPTIONS (engineering task):
  A. Use a RESOLVE ATTACHMENT in the host render pass (tiler-native resolve on
     store = the recommended Adreno path) and dump the resolved 1x - loses
     per-sample data but Adreno-friendly. Best for the menu.
  B. Explicit vkCmdResolveImage of the MSAA RT to a 1x image before the dump,
     dump reads 1x. (Add CmdVkResolveImage to the deferred cmd buffer.)
  C. Read the MSAA image in a FRAGMENT shader (sampler2DMS) instead of compute -
     some Adreno drivers handle MSAA reads in fragment but not compute.
  D. Disable host MSAA entirely (render G-buffer at 1x) - consistent 1x pipeline
     (must also adjust EDRAM tile math; naive RB_SURFACE_INFO force failed).
Recommended: try C first (cheapest - just change the dump from compute to a
fullscreen fragment pass reading sampler2DMS), then A if C fails.
