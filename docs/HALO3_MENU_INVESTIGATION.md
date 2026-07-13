# Halo 3 main menu 3D-vista bug — investigation notes

Status: **UNRESOLVED**. The menu's 2D UI (logo, list, Bungie mark) always renders
correctly. The animated 3D background renders as flat dark navy on Adreno
(Odin 2, Adreno 740) instead of the real scene. This document consolidates
everything learned so a future session (or another dev) doesn't have to
re-derive it. Branch: `canary-ae` (package `org.xeniaae.canary`). All testing
must happen on Canary AE, never `org.xeniaae`.

## What the menu is actually supposed to show

Researched rather than assumed: the Halo 3 main menu background is "an
animated view of a cliff at the edge of the Excession with several Covenant
starships circling it, and Banshees patrolling over it." So the scene has:
static cliff/terrain geometry, an animated stormy sky, and multiple moving
objects (ships circling, Banshees patrolling). This matters — it means the
broken buffer is very unlikely to be simple single-mesh vertex skinning.

## The broken buffer

- Address `0x0574xxxx` (allocator-assigned, shifts slightly boot to boot),
  size 573440 bytes / 143360 dwords, holding ~19040 vertices' worth of data.
- Written via an **in-place, self-referential memexport**: the vertex
  shader's own vertex-fetch source (`vf1`) is the SAME address it exports to.
  Confirmed via address correlation (MEMEXPORT_TARGET == MEMSRC == the
  terrain draw's own fetch address, all identical).
- Filled via ~800+ small point-list draws (vertex counts 4-16 each) that
  iteratively refine/compact this buffer across the frame, each one read-then
  -written back through the exact same address.
- On Adreno, only a **dense prefix** of the buffer ever gets filled (roughly
  5-14% across many runs — see "Non-determinism" below), never the full
  range. Desktop RADV fills it (close to) completely and renders the vista
  correctly. The fill is NOT random scatter — it's a clean prefix that
  terminates and never fills the rest.

## The shader: `9EA48FC2B26C325D`

Confirmed via shader-hash-tagged logging that this ONE shader is the sole
writer of the buffer (dumped ucode: `scratchpad/shaderdump/` from earlier
sessions, 1030 lines). Key characteristics:

- A `loop i15, L132` construct early on, using **address-register-indexed
  constants** (`c[36+a0]`, `c[136+a0]`, `c[137+a0]`, `c[144+aL]`) — originally
  read as bone-matrix skinning, but given the actual menu content (multiple
  circling/patrolling objects, animated stormy sky), this pattern is at least
  as consistent with **per-instance transform lookup** (GPU instancing via
  address-register-indexed constant fetch — the standard Xbox-360-era trick
  before hardware instancing) or a **permutation-table lookup** for
  procedural noise, as with literal character skinning.
- Extremely heavy use of `sin`/`cos`/`frc`/`sqrt`/`dp3` in repeating ~150-line
  blocks, each block following the same shape: compute something via
  transcendental math, `trunc()` it, then run a chain of `seq`/`cndeq`
  (exact-equality conditional select) against small constants
  (`c222`/`c226`/`c227`/`c228`/`c229`). Counted **150 `seq`/`sne`/`cndeq`/
  `cndgt` instructions against only 43 `trunc`/`floor` calls** in this one
  shader. This is a textbook **procedural noise implementation** (e.g.
  multi-octave fBm/Perlin-style noise via a hash/permutation table,
  reproduced through the vertex ALU because Xbox 360 had no compute
  shaders) — almost certainly what drives the animated stormy sky/cloud
  motion. The output export slot itself is computed via `floor`/`truncs` of
  this same kind of float math.
- The `trunc(x)` → exact-equality-against-constant pattern is the real
  fragility here: it uses floating point as a **discrete switch/case
  selector**. If the upstream computation (any of the many `sin`/`cos`/`sqrt`
  calls feeding it) lands even one ULP off from the intended integer
  boundary, the truncated value silently selects the wrong case. This is far
  more failure-prone than plain transcendental-precision divergence in
  isolation, because ANY of dozens of upstream ops compounding by even a
  sub-ULP amount can flip a branch.

## Xbox 360 / Xenos hardware research (grounding, not guesswork)

- Xenos ALUs are IEEE 754 float32 "with typical graphics simplifications":
  denormals flush to zero on reads, and there's a dedicated transcendental
  unit for `sin`/`cos`/etc. `RSQ`/`RECIP` use a reduced-precision (~14-15 bit)
  hardware LUT seed + a refinement step — i.e. even **real Xenos RSQ is only
  an approximation**, so no Xbox 360 game can rely on bit-exact RSQ results
  across real consoles either.
- Xenos is historically described as based on the Adreno A200 architecture —
  ATI's mobile graphics group became Qualcomm Adreno. Architecturally
  relevant, not just a coincidence, for why Adreno and Xenos diverge from
  each other in different ways than RADV does.
- Tried to obtain the authoritative AMD R600-family Instruction Set
  Architecture PDF (closest real spec to Xenos-era hardware) from
  x.org/xorg.freedesktop.org — **blocked by a bot-challenge page**, both via
  WebFetch and curl with varied User-Agent/referrer. Never obtained the
  actual document; worked from search-result summaries instead. If revisited,
  try an alternate mirror or archive.org.
- Confirmed (via live device log, NOT the stale
  `/storage/emulated/0/Download/xenia_startup.log` — use
  `/storage/emulated/0/Android/data/org.xeniaae.canary/files/xeniaae/xe.log`
  instead) that the Adreno 740 reports and Xenia enables ALL of
  `shaderSignedZeroInfNanPreserveFloat32`, `shaderDenormFlushToZeroFloat32`,
  `shaderRoundingModeRTEFloat32`. So base float arithmetic is already under
  matching IEEE float-control execution modes on both RADV and Adreno — the
  remaining vendor variance lives entirely in the **GLSL.std.450 extended
  instruction set** (`Sin`/`Cos`/`InverseSqrt`/`Sqrt`/etc.), which SPIR-V does
  NOT mandate any particular ULP accuracy for.

## Experiments tried this investigation

All changes live in `spirv_shader_translator_alu.cc`
(+ `.h` for the sin/cos helper declaration), all measured via a custom
`VTXDIST` diagnostic log line (fill count / histogram of the terrain buffer,
added to `vulkan_command_processor.cc`'s vertex-fetch path).

### 1. RSQ → `1.0/sqrt(x)` instead of driver `InverseSqrt` — **KEPT**, commit `c14047bc`
Real mechanism: real Xenos RSQ is itself only approximate, so replacing the
vendor-approximate `GLSLstd450InverseSqrt` intrinsic with an exact
`sqrt`+`div` (mirroring how `RCP` in this same file already avoids a hardware
reciprocal intrinsic) should make RADV and Adreno agree much more closely
regardless of what real Xenos hardware does bit-for-bit. Single-sample
before/after: 10558/143360 (7.36%) → 13955/143360 (9.73%). **Caveat added
later**: once we discovered the non-determinism (below), a single-sample
comparison like this one is no longer trustworthy on its own — the
improvement is plausible and the engineering rationale is sound
independently of this specific bug, so it was kept, but should not be cited
as a *proven* fix for the menu specifically.

### 2. Portable SIN/COS (range-reduction + minimax polynomial) — TRIED, REVERTED
Replaced `GLSLstd450Sin`/`Cos` with a manual range-reduction (`x - 2pi*round(x/2pi)`)
+ 11th-degree minimax polynomial (DirectXMath `XMScalarSinCos` coefficients),
fully portable/vendor-independent. Compiled and ran fine. **This is where the
non-determinism was discovered**: 3 cold boots of the identical binary gave
13955/13540/11735 out of 143360 — a ~1.5-point spread from noise alone, no
code change between samples. Against that noise floor, sin/cos showed no
clear improvement over the RSQ-only baseline. Given the real cost (an
11-degree polynomial per call site, applies globally to every shader in every
game, not just this one) and no signal of benefit, reverted via `git checkout --`
(never committed).

### 3. TRUNC/FLOOR epsilon-snap before quantizing — TRIED, REVERTED
Directly targets the "trunc-into-exact-equality-switch" fragility described
above: snap values within 1/1024 of an integer to that integer before
truncating/flooring, so vendor float noise can't flip which side of a branch
boundary a value lands on. Compiled and ran fine. Tested properly this time
(multiple cold-boot samples per the lesson from experiment #2): 13097, 7686,
9479, 9416 out of 143360 (9.14%, 5.36%, 6.61%, 6.57%) — no improvement over
baseline, if anything trending lower on average. Reverted (never committed).

## The non-determinism (arguably the most important finding)

Across many cold boots of otherwise-identical binaries, the terrain buffer's
final fill count varies by roughly 5.4%–9.7% of 143360 — a real, repeatable
spread with **zero code changes**. Confirmed this is NOT frame-to-frame
animation drift: within a single boot, once the menu settles, the fill number
is rock-stable across many consecutive frames (same value logged repeatedly).
It's decided once, early, and then holds for the rest of that run — but
differs from boot to boot.

This rules out simple deterministic float-precision divergence as the primary
driver of the RUN-TO-RUN variance (a pure precision bug would give the same
wrong answer every time, given the same inputs). It points instead at
something timing/scheduling-dependent that gets locked in during the first
few frames of boot — candidates not yet investigated:
- Intra-draw races: each of the ~800 small memexport draws only has 4-16
  vertex invocations, but if invocations within a SINGLE draw call read
  data that a sibling invocation in the same draw is concurrently writing
  (no ordering guarantee across parallel invocations without explicit
  barriers, which vertex shaders don't have), the result could depend on
  Adreno's specific warp/wavefront scheduling that frame — confirmed
  cross-DRAW ordering is NOT the issue: `AwaitAllQueueOperationsCompletion()`
  runs after literally every memexport draw (verified it's inside
  `IssueDraw`, called per-draw), so draws are already fully serialized
  relative to each other.
- Something boot-timing-dependent seeding an early pass differently
  (e.g. a frame-counter or delta-time value baked into a constant before the
  compaction loop first runs, if the real console's boot timing was more
  consistent than ours).

**Methodological lesson for any future work here**: never trust a
single-sample before/after comparison on this bug again. Always take 3+ cold
boots per configuration (clear `pipelines_*.bin`, `cache/shaders/`, and
`xe.log` each time — see commands below) before concluding a change helped or
hurt.

## Reproduction / measurement commands

```bash
# Odin 2 device serial: 3a478943. Canary AE package: org.xeniaae.canary.
adb -s 3a478943 shell am force-stop org.xeniaae.canary
adb -s 3a478943 shell rm -rf /storage/emulated/0/Android/data/org.xeniaae.canary/files/xeniaae/cache/pipelines_4D5307E6.bin \
    /storage/emulated/0/Android/data/org.xeniaae.canary/files/xeniaae/cache/shaders
adb -s 3a478943 shell rm -f /storage/emulated/0/Android/data/org.xeniaae.canary/files/xeniaae/xe.log
adb -s 3a478943 shell monkey -p org.xeniaae.canary -c android.intent.category.LAUNCHER 1
# tap Halo 3's box art twice (coordinates depend on library layout), then wait ~45-50s for the menu
adb -s 3a478943 pull /storage/emulated/0/Android/data/org.xeniaae.canary/files/xeniaae/xe.log /tmp/xe.log
grep "VTXDIST" /tmp/xe.log | grep "nonzero=[0-9]*/143360" | tail -3
```

The live session log is `/storage/emulated/0/Android/data/org.xeniaae.canary/files/xeniaae/xe.log`
— NOT `/storage/emulated/0/Download/xenia_startup.log`, which is stale/unused
by this build (belongs to a different Xenia variant sharing that shared
external path).

## Update: the non-determinism is NOT CPU-input-driven — traced to GPU/timing, one real lead ruled out

Added targeted diagnostic logging (`CONSTDUMP`, gated to shader `9EA48FC2B26C325D`
only) that dumps the exact float constants `c220`-`c229` (the ones feeding the
trunc+exact-equality branch chain) and all 32 loop constants, straight from
`register_file_->values`, at the same point `MEMEXPORT_TARGET` already logs.

**Result: bit-for-bit identical across boots**, including the loop trip count
(`l15=0x0001000D`, i.e. count=13, matching `loop i15` in the ucode). Two boots,
same shader, same constants, same loop count — yet fill went from 636/143360
(0.44%!) to 12626/143360 (8.8%). **This conclusively rules out "different
CPU/game state boot to boot" as the cause.** The divergence is purely in GPU
execution, not input data.

**Also found**: which shader writes the terrain-sized target isn't even fixed.
Some boots only `9EA48FC2B26C325D` (the self-referential in-place compaction
shader) fires; other boots `C5E0199746AB8E83` (a *different* shader — reads a
small 46KB/11520-dword seed buffer via `vf=94` and expands it into the full
573440-byte target, i.e. GPU amplification, not compaction) fires instead, or
sometimes both fire in the same boot at different points. Visually the result
was flat navy regardless of which shader(s) ran — so shader selection alone
isn't the deciding factor either, though it's a real structural finding worth
remembering (this buffer can apparently be filled by more than one code path).

**Timing hypothesis tested and NOT supported (2 experiments, both reverted)**:
Since `readback_memexport = true` was left enabled in the on-device config
from earlier diagnostic sessions, and Xenia's own doc comment on that flag
says "\[D3D12 Only\]... causes mid-frame synchronization, so it has a huge
performance impact" — it was forcing a full `AwaitAllQueueOperationsCompletion()`
GPU pipeline stall after **every one of the ~800** small in-place memexport
draws, every frame, on a Vulkan backend the flag was never intended for. The
hypothesis: if this pass is a "keep refining until a time/frame budget runs
out" system, crushing its throughput this hard could easily starve it,
explaining both the incomplete fill and the boot-to-boot variance (different
real-world stall overhead each boot).

1. **Disabled `readback_memexport` entirely** (pure on-device config edit, no
   rebuild) — menu still rendered flat navy. Inconclusive rather than
   negative: without readback, guest RAM is never updated, so the VTXDIST
   diagnostic itself goes blind (always reads stale zero) — we lost the
   ability to measure the GPU-side result at all, only the visual outcome.
2. **Batched/coalesced the readback** (real code change: defer the stall to
   only fire when the written range changes or a non-memexport "consumer"
   draw runs, instead of after every single draw — see git history on
   `canary-ae` if resurrected) — kept measurability. Result: 2 samples at
   2.45% and 3.24% fill, both *lower* than the established baseline range
   (~5-14%), and boot-to-menu timing was NOT meaningfully faster (~39s,
   same order as before). **REVERTED** (surgically, preserving the
   pre-existing uncommitted Option-1/rasterizer-discard work in the same
   files) — both the lack of speedup and the lower fill suggest Xenia's
   normal (non-diagnostic) storage-buffer barrier tracking may not fully
   cover this self-referential same-buffer read-after-write pattern on its
   own, and the full per-draw stall might have been accidentally
   compensating for a latent barrier gap by brute-force draining the whole
   pipeline. Removing it exposed that rather than removing overhead that
   mattered. NFS Carbon reverified regression-free after reverting.

**Where this leaves it**: the timing/starvation theory, while a good hypothesis
given everything else observed, did not pan out under direct test. The
non-determinism is real, GPU-execution-side, and not explained by CPU input
data — but neither "just make it faster" experiment fixed or even clearly
improved it. The most likely remaining explanations: (a) a genuine intra-draw
race within the ~4-16 vertex invocations of a single draw (still not directly
tested — would need per-invocation instrumentation or a way to force
single-invocation draws), or (b) a real missing/insufficient GPU-side barrier
for the self-referential same-buffer access pattern specifically (suggested
by the batching experiment's regression) that exists independently of timing
- worth auditing `shared_memory_`'s barrier insertion for storage-buffer
read-after-write hazards on the SAME range across consecutive draws within a
frame, which is a different and more promising angle than anything tried so
far.

## Breakthrough: isolate what Halo 3 gives vs what Adreno wants (standalone GPU probe)

All prior precision experiments (RSQ, SIN/COS, TRUNC/FLOOR-snap) were tested
by inference through the full, noisy, non-deterministic in-game rendering
pipeline - never confirmed against ground truth. Built a **standalone Vulkan
compute probe** (`tools/adreno_probe/`, permanently saved in this repo) that
dispatches the SAME `GLSLstd450` intrinsics Xenia's SPIR-V translator uses,
directly, with no game, no Xenia, no boot time, against known inputs -
diffed against a Python/numpy float64-computed float32 reference. This is a
completely different, decisive, reusable methodology - see
`tools/adreno_probe/README.md` for build/run instructions and how to extend
it to test other GPUs or other candidate fixes.

**Definitive findings (Adreno 610 & 740, consistent across both):**
- `trunc`/`floor`/`fract`: **bit-exact**, zero mismatches across the whole
  test sweep including every near-integer boundary case. The earlier
  TRUNC/FLOOR epsilon-snap fix theory is now DISPROVEN as unnecessary - these
  intrinsics were never the problem.
- `sqrt`/`inversesqrt`: within 1 ULP of ideal - essentially optimal already.
  The earlier RSQ->sqrt+div fix (kept, commit `c14047bc`) wasn't fixing a
  real precision bug (Adreno's native InverseSqrt is already excellent) - it
  remains a sound, harmless change (matches the existing RCP precedent) but
  should not be credited with meaningfully improving this specific bug.
- `sin`/`cos`: **genuinely, measurably broken at large magnitudes** - up to
  1.6e-2 absolute error around magnitude 3e5, and critically **~6.85e-4 at
  x=10000 specifically** - which is `c224.y` in the real, live-captured
  constants for shader `9EA48FC2B26C325D` (the menu terrain/noise shader).
  This isn't a hypothetical edge case; it's the actual value the actual
  shader uses.

**Why the earlier reverted SIN/COS attempt was inconclusive, not wrong**: the
first portable sin/cos fix (simple `x - k*2*pi` single-float32-subtraction
range reduction) suffers the SAME catastrophic-cancellation problem at large
magnitudes as the native intrinsic - the probe proved it's a genuine mixed
trade-off (better at some magnitudes, worse at others), not a clean win. That
explains the inconclusive in-game result honestly: there wasn't a clean win
to find with that specific algorithm. The theory (sin/cos precision matters)
was right; the implementation was insufficient.

**The real fix: Cody-Waite range reduction.** Splits 2*pi into 3 terms, each
exactly representable in float32, subtracted from the argument separately
(not combined first) so the large-magnitude part of the reduction never loses
precision to cancellation - a standard, well-established numerical technique
(used in fdlibm/cephes-family libm implementations), not a guess. Probe-measured
to be **45-85x more accurate than the native intrinsic, consistently, across
the entire 0-100000 magnitude sweep** (not a mixed trade-off - see
`tools/adreno_probe/adreno610_codywaite_results.csv` for the raw data).

**Implemented** in `SpirvShaderTranslator::PortableSinCos`
(spirv_shader_translator_alu.cc), replacing the driver `GLSLstd450Sin`/`Cos`
calls for `kSin`/`kCos`. Also built `scripts/halo3_menu_sample.sh` (permanently
saved) - an automated multi-cold-boot sampling harness that fixes a
methodology bug in earlier manual testing (the poll condition was matching
ANY `VTXDIST` log line, including small unrelated buffers logged early during
loading, instead of specifically the 143360-dword terrain buffer - causing
premature/wrong-point-in-time captures that likely explains some of the
earlier "0.44% to 14%" wild swings being partly measurement-timing noise, not
purely GPU non-determinism).

**Re-tested with this fixed methodology** (5 cold boots requested, 4 completed
per run - Canary AE process occasionally doesn't survive the boot, a known
flakiness):
- Baseline (RSQ fix only, no SIN/COS fix): samples 6.95%, 5.58%, 6.95%, 6.32% -
  mean 6.45%, range 5.58-6.95%.
- With Cody-Waite SIN/COS: samples 6.10%, 7.39%, 7.31%, 6.89% - mean 6.92%,
  range 6.10-7.39%.

A real, consistent (if modest) improvement - the two distributions barely
overlap, every Cody-Waite sample sits at or above most of the baseline range.
**Committed** (`a0b2f29e`) given it's grounded in verified fact (the isolated
probe), a principled standard technique, and a directionally consistent (not
mixed) in-game result - unlike the two previous reverted attempts. NFS Carbon
reverified regression-free.

**Still not a full fix** - the menu is still flat navy at rest.

## The precision audit is now COMPLETE - every operation the shader uses has been individually verified

Extended the probe to the remaining untested operations: `EXP2`/`LOG2`
(GLSLstd450 extended instructions, same "vendor-defined precision" risk
category as SIN/COS/SQRT was) and the `mova`/address-register computation
itself (`floor(x+0.5)` then `clamp(-256,255)` then convert-to-int - the
mechanism behind `c[N+a0]`-style dynamic constant indexing that the shader's
skinning/instancing loop depends on). New tool files in
`tools/adreno_probe/`: `probe_explog.comp` + `probe_host_explog.cc` +
`compare3.py`.

**Results**: `EXP2` within 1 ULP, `LOG2` within 1 ULP (both essentially
optimal). `mova`'s floor+clamp+convert chain: **zero mismatches** across every
tested input including all the exact `±0.5` rounding-boundary cases - fully
correct. This isn't surprising in retrospect: `floor` was already proven
bit-exact, `clamp` of an already-integer-valued float against integer bounds
is trivial, and `OpConvertFToS` of an already-integer float is exact by
construction (and SPIR-V's spec mandates round-toward-zero for it anyway).

Went one step further and traced `GetStorageAddressingIndex` /
`LoadOperandStorage` (spirv_shader_translator.cc) - the actual mechanism that
turns a computed address-register value into a constant-buffer read
(`c[N+a0]`). It's pure 32-bit integer addition (`OpIAdd`) followed by a
uniform-buffer access chain - no floating-point vendor variance is even
structurally possible here; integer arithmetic and buffer indexing are exact
by the Vulkan/SPIR-V spec, not "vendor-defined precision" like the
transcendental extended instructions are.

**So: every individual ALU operation this shader uses has now been audited.**
`TRUNC`/`FLOOR`/`FRACT`/`EXP2`/`LOG2`/`SQRT`/`INVERSESQRT`/`mova` are all
exact or within 1 ULP - genuinely clean, not just "probably fine." `SIN`/`COS`
was the ONE real, provable bug, and it's fixed (Cody-Waite, committed). There
is no remaining candidate left in the "is some GPU math operation imprecise"
hypothesis space for this shader.

**What's left is therefore NOT a precision problem at all** - it's the
GPU-execution-level non-determinism documented in the section above (stable
within a boot, varies boot-to-boot, not explained by different CPU-fed
constants - proven via CONSTDUMP). The leading remaining theory: the ~800
memexport draws are already correctly serialized *relative to each other*
(confirmed - `AwaitAllQueueOperationsCompletion`/the Vulkan barrier in
`shared_memory_->Use()` both run per-draw), but each individual draw still
has 4-16 vertex-shader invocations running with **no ordering guarantee
relative to each other** (vertex shaders have no barrier/workgroup concept
the way compute does) - if invocation N's self-referential read depends on
invocation N-1 (both in the SAME draw call) having already written, that's a
structural, architecture-level race that Xenia's translator cannot fix with
better math, only by restructuring how these specific draws get dispatched
(e.g., forcing one-vertex-per-draw for memexport draws whose ucode proves
self-referential, or routing through the existing-but-disabled
`memexport_use_compute_` path with explicit per-invocation ordering - though
plain compute dispatch alone doesn't inherently guarantee cross-invocation
ordering either without explicit workgroup-level synchronization, so this
would need real design work, not just flipping the existing flag back on).

**Real vertex-count-per-memexport-draw distribution** (from 5 saved sample
logs, `MEMEXPORT_DRAW vtxcount=` lines, ~400-550 draws per boot): the
dominant value is **exactly 16 vertices, 70-80% of all draws** (e.g. 335/434,
353/462, 424/546 across three samples), with a long tail of smaller counts
(1-15, mostly single digits) and a handful of larger outliers up to 384. This
makes the intra-draw-invocation-race hypothesis concretely plausible - most
of the risk window is a small, bounded 16-invocation batch, not thousands.

## Draw-splitting experiment: attempted, RULED OUT intra-draw racing definitively

Did the groundwork first: traced `SubmitBarriers()`/`SubmitBarriersAndEnterRenderTargetCacheRenderPass()`
and found Xenia already ends+re-begins the render pass around EVERY memexport
draw today (since `shared_memory_->Use()` with a non-empty previous write
always queues a real barrier, and `SubmitBarriers` ends the render pass
whenever there's a pending barrier) - so extending this to run once per
VERTEX instead of once per DRAW is a quantitative risk (more of an operation
Xenia already survives ~800 times/frame), not a qualitative one (a wholly new
untested code path). This de-risked the experiment enough to attempt live.

**Implementation**: added a `TESTRIG(halo3-nondeterminism)` toggle
(`debug.canary.testrig.memexportsplit`, live-settable without rebuild) that,
when enabled, splits each memexport draw's single `CmdVkDraw`/`CmdVkDrawIndexed`
call (covering `host_draw_vertex_count` vertices at once) into one draw call
PER VERTEX, re-calling `shared_memory_->Use(kGuestDrawReadWrite, extent)` +
`SubmitBarriersAndEnterRenderTargetCacheRenderPass()` between every single
one - forcing the same real barrier that already correctly serializes
consecutive DRAWS to also serialize every INVOCATION within what used to be
one draw.

**Caveat discovered during testing**: the toggle framework
(`xe::testrig::HotPathEnabledCached`/`PropertyEnabled`) defaults to ENABLED
when the property is unset - correct for opt-out debug instrumentation, but
backwards for a risky experimental code path that should default OFF. The
first "control" run was actually unknowingly running WITH the split active.
Caught and corrected by explicitly setting the property to `0` for a true
control, `1` for the test - but this is a real gotcha worth remembering if
this toggle pattern gets reused for something similarly risky.

**Result**: 3 cold-boot samples with the split OFF (mean 6.51%, range
5.62-7.25%) vs 3 samples with it ON (mean 6.80%, range 6.35-7.22%) - the two
distributions are statistically indistinguishable, and no hang or crash at
any point (completed in normal boot time both ways). **This definitively
rules out intra-draw vertex-invocation ordering as the cause of the
non-determinism** - forcing maximally strict per-vertex ordering (finer than
Xenia's existing per-draw ordering) made no measurable difference.

Reverted the experimental code after getting this answer (kept as a git-diff-able
lesson here rather than left live, since a risky code path that defaults to
enabled-when-unset is a footgun for a future session to accidentally ship).

## Where this leaves the investigation

Every hypothesis that's been directly testable with the tools available has
now been tested:
- Precision of every ALU op the shader uses: closed, all clean except
  SIN/COS, which is fixed.
- Cross-draw GPU ordering: proven correct (barrier fires every draw).
- CPU-fed shader constants differing boot-to-boot: proven NOT the case
  (CONSTDUMP showed bit-identical constants across boots with wildly
  different fill results).
- Intra-draw vertex-invocation ordering: proven NOT the cause (this section).

**What's left, not yet tested**: whether the GPU buffer memory backing this
self-referential export target starts each boot in a genuinely different
state - not because of different CPU-fed constants (ruled out above), but
because of Xenia's own shared-memory page-validity tracking
(`SharedMemory::RequestRange`/`MakeRangeValid` in `shared_memory.cc`): a page
already marked `valid_and_gpu_written` from ANY earlier GPU write this
session is NOT re-uploaded/re-zeroed from guest RAM on subsequent
`RequestRange` calls - so if boot-timing variance affects how much
"warm-up" GPU activity touches this exact buffer region before the point
we're sampling, the actual starting seed state for the ~800-draw compaction
sequence could differ boot-to-boot for a reason that's neither CPU data nor
GPU execution-ordering, but genuine memory-content history. A quick sanity
check (comparing the log line number where `MEMEXPORT_TARGET` first appears
across two sample logs - both at line ~6259, nearly identical) argues weakly
AGAINST large warm-up-amount variance, but isn't a real test of the
hypothesis (it only shows roughly-similar TIMING, not roughly-similar PRIOR
BUFFER CONTENT). A real test would need to dump/hash the buffer's content at
the very first `MEMEXPORT_TARGET` for this address, across multiple boots,
before any of the tracked ~800 draws touch it, and compare - not yet done.

This is the clear next step if resumed: it's a real, well-motivated,
not-yet-ruled-out hypothesis, and unlike the draw-splitting experiment, it's
purely diagnostic (a snapshot/hash comparison) rather than a live risky code
change, so it should be lower-risk to attempt than what was just tried.

## Buffer pre-fill snapshot: attempted immediately, RULED OUT too

Added a one-shot diagnostic (`PREFILL_SNAPSHOT`, logs once per session, pure
read - no behavior change, safe by construction) that dumps a nonzero-dword
count of the target buffer's content at the exact moment of the FIRST
tracked `MEMEXPORT_TARGET` for shader `9EA48FC2B26C325D`, i.e. before any of
the ~800 draws in the tracked sequence have touched it.

**Result across 3 fresh cold boots: `nonzero=0/143360` every single time**,
with `firstnz` reporting "not found" (`0xFFFFFFFF`) and all sampled dwords
reading `0x00000000`. The buffer is genuinely, completely zero at first
touch, every boot, no exceptions. **This rules out the page-validity/warm-up
carryover hypothesis too** - there's no stale content, from this session or
otherwise, seeding the compaction sequence differently.

Also spot-checked total memexport draw count vs. fill percentage across
available samples for a cruder "does the game just run more compaction steps
some boots" signal - no clean correlation (e.g. 348 draws gave LOWER fill
than 319 draws in one comparison) - weak evidence against "total draw count
varies enough to explain it," though this specific comparison mixed data
from different testing rounds and isn't a clean, dedicated test on its own.

## Summary: five hypotheses tested and eliminated this session, none explain the non-determinism

1. Precision of every ALU operation the shader uses - closed (SIN/COS was
   the one real bug; it's fixed).
2. Cross-draw GPU execution ordering - proven correct (real Vulkan barrier
   fires before every draw, verified in code).
3. CPU-fed shader constants differing boot-to-boot - proven false
   (CONSTDUMP: bit-identical constants, wildly different fill results).
4. Intra-draw vertex-invocation ordering - proven not the cause (forcing
   maximally strict per-vertex barriers made no measurable difference).
5. Pre-existing/stale buffer content from earlier GPU activity this session -
   proven false (buffer is exactly zero at first touch, every boot).

At this point, every mechanism that's directly testable with the tools
available (isolated GPU probes, in-game diagnostic logging, live-toggleable
experiments) has been tested and eliminated. The non-determinism is real,
reproducible, and still unexplained. Further progress most likely needs
either: (a) genuine GPU-vendor-level tooling this session didn't have access
to (a proper Adreno driver/hardware profiler, not RenderDoc - which prior
sessions already found to be a dead end for this specific investigation), or
(b) a fundamentally different angle not yet considered - e.g. auditing
WHETHER the total number/identity of draws the GAME issues for this pass is
itself non-deterministic (weak, inconclusive signal above - worth a proper
dedicated test correlating draw count against fill on freshly, consistently
captured data, not a retrospective mix of different test rounds like the
quick check above).

## Ideas not yet tried

1. **Frame-locked/stable-seed investigation**: instrument exactly which early
   pass "locks in" the eventual fill count, to find the actual
   timing-dependent decision point instead of continuing to guess at ALU
   precision.
2. **Intra-draw race check**: add a barrier or split each of the ~800 small
   draws further to see if reducing per-draw invocation parallelism changes
   the variance — if it does, that confirms an intra-draw hazard.
3. Get the real R600 ISA PDF (or an equivalent authoritative Xenos ALU spec)
   past the bot-challenge, to check exact `MOVA`/address-register rounding
   semantics — not yet tried as a targeted fix.
4. Given the "procedural noise" reframing, consider whether this is a solved
   problem in other Xenia forks/upstream Xenia-Canary for other Adreno
   devices — worth checking upstream issue trackers/commit history for
   "memexport" + "Adreno" + noise/compaction discussions before continuing
   to reinvent this locally.

## MAJOR FINDING (later session, same day): the real terrain-consuming shader identified for the first time

All prior work (including earlier in this same session) implicitly assumed there
was a single large (~19040-vertex) draw that consumes the memexport buffer
and rasterizes the terrain. That assumption was never actually verified -
and it's wrong. Logged every draw whose vertex fetch address falls in the
tracked buffer's region (`ANYFETCH_IN_RANGE`, checks the actual runtime
fetch-constant address, not vf-slot-number heuristics which don't carry
meaning across different shaders). Result: the consumer is
**`sh=488D9488AB7ED7D8`**, `eM=0x0` (confirmed pure consumer, no memexport
writes), with **small vertex counts (4-64), matching the producer's own
small-batch pattern** - not one giant draw. The terrain is built from many
small produce-then-consume pairs, not a bulk compaction followed by one
final draw.

This shader is dramatically more complex than anything examined before (600
ucode lines vs ~100-1000 for the others) - it fetches from `vf1` (matching
the memexport target), then does its OWN substantial procedural computation
(sin/cos/sqrt/rsq/exp/log, address-register-indexed constants `c[41+a0]`,
heavy `cndeq`/`cndgt` branching) with **nearly the entire body wrapped in a
single predicate `(p0)`** set early from properties of the fetched vertex
data (`sge`/`seq` comparisons against fetched attributes). If a fetched
vertex is "invalid" (unfilled compaction slot), `p0` likely evaluates false
and the ENTIRE position/lighting computation is skipped - meaning `oPos`
gets written from whatever the registers held before the skip, not a
deliberately safe fallback value.

### Degenerate-W theory: tested, did not change the visual result (but the underlying portability fix was kept)

Traced how Xenia turns a guest position into `gl_Position`
(`CompleteVertexOrTessEvalShaderInMain`, spirv_shader_translator.cc): if the
guest shader's convention is "position.w is actually 1/W" (very common),
Xenia computes `true_w = 1.0 / guest_w`. A degenerate/skipped vertex with
`guest_w == 0` (plausible if `r0`/whatever register feeds `oPos` never gets
written when `p0` is false) reciprocates to **+Infinity** - which is NOT
`<= 0`, so it escapes the standard "discard if `w<=0`" perspective-clip
check that's supposed to cull degenerate vertices. The primitive gets
rasterized instead of clipped, and what happens next depends on
vendor-specific handling of infinite clip coordinates. With 86-95% of
vertices plausibly invalid, a dense web of such degenerate primitives
overwriting the valid ~5-15% would explain the single biggest unexplained
observation of this whole investigation: **fill percentage ranged 5-14%
across many tested configurations with ZERO visible correlation** - exactly
what you'd expect if degenerate geometry dominates the visual result
regardless of how much valid data exists underneath it.

Implemented a general, low-risk fix: force a safe negative sentinel instead
of +Infinity when reciprocating a W of exactly 0, so such a vertex clips the
same way on every GPU instead of depending on vendor-specific infinity
handling. **Tested: no visible change to the menu (still flat navy), same
as every other fix this session.** Kept anyway (commit `6a4b9932`) since
it's sound and low-risk on its own merits, matching the RCP/RSQ precedent of
preferring deterministic behavior over relying on IEEE-754 edge-case
propagation - but it means either (a) the degenerate vertices' `w` isn't
actually landing on exactly `0.0` the way this theory assumed, (b) this
shader's guest convention is NOT the "w = 1/W" one so this code path never
even applies to it (`is_w_not_reciprocal` may be true here - not verified),
or (c) something else about how `p0` resolves/what the skipped registers
actually contain is different from what's assumed here.

### Where this leaves it

This is a genuinely new, previously-unexplored, structurally significant
finding (the real consumer, 6x more complex than what was analyzed before)
that reframes the whole investigation - but the specific "degenerate-W
Infinity" theory built on top of it didn't pan out on first test. The
natural next steps, not yet attempted:
1. Verify whether `is_w_not_reciprocal` is actually true or false for THIS
   shader specifically (would immediately tell us if the W-reciprocal path
   even applies here).
2. Dump/inspect this shader's actual constant values (`c228`, `c229` are
   used pervasively in the early predicate-setting comparisons - likely
   sentinel/threshold constants analogous to the `c220-c229` range already
   captured for the producer shader) to understand exactly what condition
   sets `p0` and what a "valid" vs "invalid" fetched vertex actually looks
   like from this shader's perspective.
3. Check what `oPos` (`r0` at the end of the shader) actually resolves to
   for a skipped-predicate vertex, directly, rather than reasoning about it
   from the ucode alone - e.g. via a targeted register dump at the exact
   point of the final `max oPos, r0, r0` write.

## Confirmed: 2D UI and 3D scene are fully separate (no shared memory/state)

User question: does the always-correct 2D menu (HALO 3 logo, list, Bungie
logo) share any memory or resources with the missing 3D background, such
that fixing one could be blocked by/entangled with the other?

Answer: no. The UI is drawn by shader `4B00BAD98B735E75` - a trivial static
mesh fetch (`FMT_32_32_32_FLOAT` position, `Stride=8`, straight 4x4 matrix
transform, no procedural math, no memexport at all). Confirmed via the
`ANYFETCH_IN_RANGE` diagnostic that it never touches the `0x0570xxxx`-
`0x0580xxxx` address range used by the memexport terrain buffer. The two
rendering paths are provably independent - a fix for the 3D scene cannot
break the UI and vice versa, and there's no shared-state race between them.

## Pipeline cache reuse: a real but partial contributor to run-to-run variance

Ran 5 cold boots *without* clearing `pipelines_4D5307E6.bin`/shader cache
(letting Adreno reuse already-compiled shaders instead of recompiling from
scratch each boot): fill percentages 3.76%, 3.98%, 3.87%, 4.11%, 3.78% -
spread of 0.35 points, notably tighter than the usual 1.2-1.6 point spread
seen with the cache cleared every boot. Conclusion: Adreno's shader
compiler has some real non-determinism that contributes to the boot-to-boot
fill variance, but it isn't the whole story (variance didn't disappear).

## Blend-mode investigation (user hypothesis, most promising lead to date)

User observation: the 3D object on the menu has a distinct blue tint and
theorized it's a blend-mode effect - the menu foreground composited over a
"background" that's being blended incorrectly.

Confirmed via a new `SCENE_BLEND` diagnostic (dumps `RB_BLENDCONTROL` for
the draws in question) that both known scene shaders
(`488D9488AB7ED7D8`, the terrain consumer, and `3D774C769771A211`, a
lighting/normal-mapped prop shader that shares the same render state) use
standard alpha blending: `src_color=SRC_ALPHA dst_color=ONE_MINUS_SRC_ALPHA
comb=ADD`, targeting an `R16G16B16A16_FLOAT` render target. This connects to
an older RenderDoc capture (prior session, not re-verified fresh this
session) that caught a composite-stage "sky" pixel with RGBA
`(0.2275, 0.3613, 0.9233, 32.0)` - a real blue color with an alpha of 32.0,
wildly outside normal `[0,1]` range, consistent with an HDR
exposure/luminance-encoding trick rather than a literal opacity value.
Per the Vulkan spec, blend factors derived from alpha are *not* clamped to
`[0,1]` for floating-point attachments (unlike UNORM) - deliberately, to
support this kind of extended-range/HDR blending. If Adreno doesn't honor
that the same way RADV (the desktop oracle's driver) does, that's a
plausible, driver-specific divergence.

Tested empirically, gated to the two scene shaders via
`vulkan_pipeline_cache.cc`'s `EnsurePipelineCreated`, with pixel-level
measurement on real screenshots (not just the buffer-fill % metric, which
only reflects vertex/memexport data and is blend-blind by construction):

| Blend config | Background pixel RGB (mean) |
|---|---|
| Default (`SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`, as shipped) | (10.7, 14.0, 29.9) |
| Blending fully disabled | (18.7, 25.4, 49.1) - ~1.6-1.7x brighter |
| Forced additive (`ONE`/`ONE`) | (39.1, 50.0, 89.0) - ~3.6x brighter than default |

So blend factor choice measurably, substantially changes the *shade* of the
flat-navy background - the user's hypothesis that blending is involved is
**confirmed**. But critically, the additive-blend screenshot was sampled at
pixel resolution across a 1000x500px region of the background and found to
be **perfectly flat**: only 4 unique RGB values in 21,000 samples, all
within ±1 of each other (pure rounding/dither noise, not a gradient). Same
uniform flatness as the default and disabled-blend configs. **No blend
configuration tested reveals any actual scene structure/detail** - only the
overall wash color changes.

Conclusion: blending is a real, confirmed *secondary* factor - it decides
what shade of flat color you get - but it is not the source of the
flatness itself. That still traces back to the much better-established
prior finding (see `project_xenia_ae_renderdoc_findings.md`, July 12): the
in-place memexport compaction buffer only fills to ~6-7% on Adreno vs a
full fill on desktop RADV, because the shader's `trunc()`-based output-slot
computation is hypersensitive to sub-ULP float differences between GPU
vendors, collapsing ~94% of vertices to the origin. A blend-side fix alone
cannot produce a correct image if the vertex data feeding it is already
94% collapsed - both problems are real and would need to be addressed
together (or the vertex-collapse problem solved first, since it's
upstream).

The diagnostic pipeline-cache blend overrides (`blendEnable = VK_FALSE`,
then `ONE`/`ONE`) were both temporary, hash-gated, non-general test code -
**reverted** after collecting the above measurements. Nothing from this
blend investigation was kept/committed.

## Compute-memexport (OPTION 2) tested head-to-head vs rasterizer-discard (OPTION 1) - decisive negative result, but rules out a whole hypothesis class

Discovered that a full compute-shader memexport implementation
(`kMemExportCompute` translator support + the `vulkan_command_processor.cc`
dispatch wiring: create a compute pipeline translation of the memexport
vertex shader, `vkCmdDispatch` one invocation per guest vertex, barrier,
then let the consuming draw read the result) already existed as uncommitted
WIP in the tree from an earlier session (`ca1fa549`'s "compute-memexport
WIP"), alongside a simpler alternative already active by default: OPTION 1,
`rasterizerDiscardEnable = VK_TRUE` for memexport-writing draws, so Adreno's
vertex shader runs once (skipping the position-only binning pass that
strips/duplicates memory stores on tiled GPUs) instead of going through
compute at all. OPTION 1 was what produced every fill-percentage
measurement in this document so far (the ~5-14%, later ~6-7%, baseline).

Made the two mechanisms cleanly mutually exclusive (OPTION 1's discard now
checks `!command_processor_.memexport_use_compute()`, via a new public
`memexport_use_compute()` accessor) to avoid the graphics draw's own
(unreliable) vertex-stage store attempt landing on top of an
already-correct compute-dispatch result, then flipped `memexport_use_compute_`
to `true` and ran the same 5-sample cold-boot harness:

| Path | Fill % (5 samples) | Mean |
|---|---|---|
| OPTION 1 (rasterizer-discard vertex stores) | 6.35-7.75% (this doc, various runs) | ~6.5-7.2% |
| OPTION 2 (compute dispatch) | 4.82, 6.35, 6.60, 6.68, 7.14 | 6.32% |

**Statistically indistinguishable - same noise band, no improvement.** This
is a genuinely decisive negative result: compute dispatches never go
through Adreno's binning pass at all, so if unreliable vertex-store landing
were the actual bottleneck, compute should have filled dramatically more of
the buffer than the partially-landing vertex-store path did. It didn't -
which rules out "store-landing reliability" as the primary cause of the
~6-7% ceiling and reinforces the separate, independently-derived
`trunc()`-precision-divergence theory (`project_xenia_ae_renderdoc_findings.md`,
July 12) as the real explanation: the shader's own float math diverges
between GPU vendors regardless of which pipeline stage executes it.

Regression-tested NFS Carbon with `memexport_use_compute_ = true` (global
flag, affects all memexport draws, not just Halo 3): boots and plays
correctly, no visual or stability regression - safe code, just not a fix
for this bug. Reverted the flag back to `false` (restores the documented,
tested OPTION 1 baseline) but **left the compute-memexport implementation
in place** (it's real, working, tested infrastructure - useful if a future,
different Adreno issue turns out to be about store reliability after all,
just not this one). The mutual-exclusion gating between the two options
was kept regardless of which is active, since it's a correctness fix on
its own merits (prevents future double-write corruption if the flag is
ever flipped again).

**This closes the "maybe it's just unreliable stores" branch of the
investigation.** Combined with the closed precision audit (every ALU op
except SIN/COS proven exact/1-ULP) and the closed intra-draw-ordering test,
the `trunc()`-based output-slot float divergence remains the single most
credible, most specific, least-eliminated theory for the root cause. It has
not yet been directly tested with a targeted fix (e.g. computing the output
slot via an alternate, vendor-consistent formulation, or forcing full
precision on that specific computation) - that's the natural next step.

## Re-tested the epsilon-snap TRUNC/FLOOR fix now that Cody-Waite SIN/COS is in place - still no improvement

The July 12 "epsilon-snap TRUNC/FLOOR" fix (round the input to the nearest
integer if within 1/1024 of one, before truncating/flooring - so vendor
float noise can't flip which side of an integer boundary a value lands on,
which then flips a downstream exact-equality branch test) was tested and
reverted BEFORE the Cody-Waite SIN/COS fix (`a0b2f29e`) existed, when
upstream transcendental error was far larger. Since shader `9EA48FC2B26C325D`
pervasively uses exactly the vulnerable pattern (sin/cos → arithmetic →
trunc/floor → `seq`/`cndeq` equality test against small integer sentinel
constants, repeated throughout its loop body for procedural terrain
generation), it seemed plausible that re-testing the SAME idea now that
SIN/COS error is reduced 45-85x could behave differently, since much less
pre-trunc divergence needs absorbing.

Re-implemented it (`TruncFloorSnapNearInteger` in
`spirv_shader_translator_alu.cc`/`.h`, wired into the `kTrunc`/`kFloor`
(vector) and `kTruncs`/`kFloors` (scalar) translation paths only - not
`kFrc`/`kFrcs`, since fractional-part semantics are unrelated to the
boundary-flip theory), same 1/1024 threshold for a clean comparison against
the original attempt. Tested with 9 cold-boot samples across two batches:
6.83/7.45/7.48/6.90/7.57/7.03/6.97/7.26/7.03%, mean **7.17%** (range
6.83-7.57%).

**Statistically indistinguishable from the current baseline** (7.24% mean,
n=4, measured earlier this same session with all currently-committed fixes
in place). The fix-ordering hypothesis doesn't hold up - re-testing with
Cody-Waite already applied didn't unlock a different result. **Reverted**
(`git checkout --` on both files, never committed).

This is a real, if disappointing, negative result: it further narrows the
credible hypothesis space by ruling out (a second time, under better
preconditions) the "boundary-flip at trunc/floor" mechanism as the
dominant effect, at least at the 1/1024 threshold. Combined with everything
else closed out this session (blend mode - secondary only; compute-memexport
- rules out store-unreliability; intra-draw ordering - closed; precision
audit - closed), the remaining open, least-explored angle is the
GPU-execution non-determinism documented in
`project_xenia_ae_renderdoc_findings.md` (July 13): fill % is stable within
a boot but varies boot-to-boot with proven-identical CPU-fed constants,
proven-correct cross-draw ordering, and now proven-irrelevant store
mechanism - something about actual per-invocation GPU execution timing/
scheduling on Adreno itself remains the standing, unexplained cause.
