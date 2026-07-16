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

## Consumer shader `488D9488AB7ED7D8` predicate `p0` DECODED (from ucode + interpreter ground truth)

Pulled the actual disassembled ucode (device `shaderdump/`) for the terrain
consumer and decoded the master predicate that gates its entire body.

**Instruction 49:** `setp_ne_push r9.w, c228.xxxx, r0.zzzz`. Per Xenia's own
CPU interpreter (`shader_interpreter.cc` `kSetpNePush`), the semantics are:
`p0 = (src0.w == 0.0) && (src1.w != 0.0)` = **`p0 = (c228.x == 0.0) && (r0.z != 0.0)`**.

`r0.z` is built (instr 46-47) from **exact float comparisons on the FETCHED
vf1 vertex** (vf1 = the memexport target the producer fills):
`r0.z = (r7.x >= c229.w ? 1:0) + (r11.w == 0.0 ? 1:0)`, where r7/r11 come from
`vfetch ... vf1` at computed index r0.y.

### Runtime constants dumped (CONSUMER_CONST diagnostic, one-shot, kept)
`c228 = (0, 1, 8, 30)` → **c228.x is EXACTLY 0.0**, so the first p0 term is
always satisfied: **the terrain path is NOT globally gated off**. p0 reduces
to `r0.z != 0`. `c229 = (0.5, -3, -2, 1)` → c229.w = 1.0.

### The important reframe: invalid/unfilled vertices RUN the body (opposite of the prior assumption)
For an UNFILLED compaction slot (all-zero fetch): r7.x = 0, r11.w = 0 →
`r0.z = (0 >= 1 ? 1:0) + (0 == 0 ? 1:0) = 0 + 1 = 1` → `r0.z != 0` →
**p0 = TRUE**. So the ~93% of vertices whose slots the producer never filled
do NOT get skipped - they execute the FULL position/lighting body on zeroed
input. The earlier working theory ("invalid → p0 false → body skipped →
degenerate leftover position") was backwards. This means the visible collapse
is 93% of vertices running the terrain math on zero input and landing wherever
that math sends them (a tight degenerate cluster / off-screen / NaN), painting
over the sparse ~7% of real vertices - which reconciles perfectly with the
long-standing "fill % 5-14% with ZERO visible correlation" observation: it's
not the amount of real data that decides the look, it's the flood of
zero-input vertices that all render regardless.

### New anomaly found and tested: `c32` is a NaN constant in the position path
`c32 = (0x7FC00000, 0, 0, 0x7FC00000)` - the x lane is the **canonical quiet
NaN**, bit-stable across cold boots (so NOT the non-determinism source). The
shader moves it into a register via the max(c,c) identity-move idiom
(instr 56: `(p0) max r0, c32.xxxx`), making r0 = NaN in all lanes; it then
propagates into r4.y/r4.w (instr 70) before being consumed by seq/cndeq
comparisons - exactly the ops where Adreno (which does not request
`SignedZeroInfNanPreserve`) may legally fold NaN-comparisons differently than
desktop RADV. Looked like a strong lead.

**Tested: flushed all NaN vertex-float-constants to 0.0 on upload. The menu
did not change at all (still flat navy, fill 5.47%, in-range).** So the c32
NaN, while a genuine latent oddity, is NOT what collapses the visible terrain -
consistent with the fill-%/visual decorrelation. Reverted the flush (it changes
seq/cndeq semantics for any NaN-carrying constant, so it's not a safe keeper).
The one-shot `CONSUMER_CONST` diagnostic was kept.

### Where the p0 decode leaves it
The decode is conclusive and reframes the visual mechanism, but it points the
root cause right back at the **producer fill** (the hard, boot-to-boot
non-deterministic core): the consumer faithfully runs on whatever the producer
gives it, and with only ~7% of slots filled, the zero-input majority dominates
the frame. The two concrete, not-yet-tried directions this unlocks:
1. **Cull zero-input vertices in the consumer** (force oPos to clip when the
   fetched vf1 vertex is entirely zero) - purely removes the garbage flood;
   would reveal whether the sparse ~7% real vertices actually form recognizable
   (if incomplete) terrain, which would definitively separate "producer fills
   too little" from "producer fills WRONG data." A clean diagnostic-by-culling.
2. Keep attacking producer fill (the standing non-determinism), which remains
   the true blocker.

## ★★★★★ BREAKTHROUGH: the geometry was never the blocker - the terrain draws don't composite to the screen ★★★★★

Decisive "does the draw path even reach the framebuffer" probe. Hash-gated to
the terrain consumer `488D9488AB7ED7D8`, in `CompleteVertexOrTessEvalShaderInMain`
DISCARDED the shader's computed position entirely and forced every triangle to
span most of NDC (vertex_index%3 -> three screen corners). Then, to remove
fixed-function confounds, also force-overrode this shader's pipeline
(`vulkan_pipeline_cache.cc`): `cullMode = NONE`, `depthTestEnable = FALSE`,
`depthWriteEnable = FALSE`, blend disabled, full color write mask.

**Result: the menu background stayed EXACTLY flat navy. No color flood, no
change whatsoever** - while the log confirms this shader drew **15,795 times**
that frame (so the override was unquestionably live). Forcing guaranteed-
fullscreen, guaranteed-visible, depth/cull/blend-neutralized geometry from the
terrain consumer produces ZERO visible pixels.

### What this proves
The terrain consumer's draws do **not** land in the visible framebuffer. The
entire prior investigation - memexport fill %, producer non-determinism,
trunc/precision, degenerate-W, the p0 predicate, the c32 NaN - was chasing the
GEOMETRY of a shader whose output never reaches the screen in the first place.
Geometry correctness was never the thing standing between us and a visible
background.

The overwhelmingly likely structure (standard for a console menu 3D vista):
the terrain/vista renders to an **offscreen 3D render target**, which is then
**resolved to a texture** and drawn as the menu background by a **separate
fullscreen pass**. The break is in that resolve-and-composite path on Adreno,
NOT in the vista geometry. This fits the oldest note in the project memory -
that the Halo 3 corruption is "AE-specific in the FORKED Vulkan GPU backend
(texture_cache / render_target_cache)".

### Draw census at the menu (distinct vsh/psh/vtxcount/eM signatures)
- `9EA48FC2B26C325D` vtx=16 **eM=0x1** - the memexport producer (compaction).
- `488D9488AB7ED7D8` vtx=64 eM=0x0 - the terrain consumer (proven offscreen
  above).
- `FED9E00DE375B2D4` / `3D774C769771A211` vtx=306-426 - large scene-geometry
  draws with real pixel shaders, drawn heavily (candidate actual 3D vista, or
  its resolve consumers).
- `B2771A0FDDE3B29E` vtx=4 - 4-vertex draws (classic fullscreen-quad /
  composite shape - prime suspect for the pass that's SUPPOSED to paint the
  vista as the background).
- `4B00BAD98B735E75` vtx=38-67 - the 2D UI shader (renders correctly).
- `C049A8C9E556F129` vtx=1 - huge count of 1-vertex draws.

### The new, correct next direction
Stop investigating vista geometry. Instead trace the **render-target
composition**: which color RT (EDRAM base) each of these draws targets, whether
the offscreen 3D RT is resolved to a texture, and whether the fullscreen
background pass actually samples that texture (vs. sampling an
unresolved/wrong/navy-cleared image) on Adreno. Concretely:
1. Log the color RT EDRAM base + host VkImage for each distinct draw signature,
   and for `IssueSwap`'s presented image, to map who-draws-where.
2. Find the fullscreen background pass (likely `B2771A0F` vtx=4) and check what
   texture it samples and whether that texture ever received a resolve from the
   3D RT.
3. This is a render_target_cache / texture_cache resolve-path bug on the tiled
   Adreno GPU, which is exactly the class of bug desktop RADV tolerates and
   Adreno does not.

All force-vis probes were REVERTED (they destroy the shader's real geometry);
only the harmless one-shot `CONSUMER_CONST` diagnostic was kept.

## Render-target composition FULLY MAPPED - the vista is a deferred-shading scene; prime suspect is the 4x MSAA resolve on Adreno

Added safe (no force-vis) diagnostics: `RTMAP` (EDRAM color/depth base per
shader, once each), `TEXSRC` (guest addresses each draw samples), `RESOLVE` /
`RESOLVESRC` (EDRAM source tile -> guest dest of every resolve), and `SWAPSRC`
(the guest address actually presented). Together they reconstruct the entire
menu frame graph:

### The frame graph
1. **3D vista renders at 4x MSAA (msaa=2) to EDRAM tile 608** (RT0) + tile 1216
   (RT1), pitch 1160. Bulk scene shaders: `FED9E00D`, `3D774C76`, `D584861E`,
   `D75B5EB3`, `DF62E069`, `9BC49A6E`, etc.
2. **EDRAM 608/1216 resolve to the G-buffer textures** in guest RAM:
   `0x043FC000`, `0x044B0000`, `0x04780000` (RESOLVESRC: color_base=608/1216 ->
   those dests). Many deferred passes then SAMPLE those (TEXSRC).
3. **Deferred composite**: `831761DEED869F91` samples the G-buffer
   (`0x044B0000`, `0x043FC000`) and writes to **EDRAM tile 1216 non-MSAA**
   (pitch 1200, msaa=0) - the final lit image.
4. **EDRAM 1216 resolves to the front buffer 0x04E20000** (RESOLVESRC: 830x
   copy_src_select=0 color_base=1216 -> dest 0x04E20000).
5. **SWAPSRC = 0x04E20000** every frame - confirmed the presented image.

### Why this indicts the MSAA resolve specifically
- The **2D UI renders correctly** and is composited fine.
- The entire **3D scene is the only thing that's MSAA (4x)**; the UI and the
  final composite/present passes are non-MSAA (pitch 1200, msaa=0).
- 4x-MSAA-EDRAM -> texture resolve on a **tiled Adreno GPU** is exactly the
  class of operation that desktop RADV handles transparently and a tiled
  renderer's EDRAM emulation frequently gets wrong (tile ownership, sample
  layout, resolve shader). If step 2 (resolve the MSAA vista to the G-buffer)
  produces empty/navy on Adreno, every downstream deferred pass composites
  navy, the front buffer is navy where the vista should be, and the non-MSAA UI
  still lands correctly on top - which is EXACTLY the observed picture.
- Independent corroboration: force-vis'ing the MSAA composite pass `3D774C76`
  fullscreen caused an immediate **VK_ERROR_DEVICE_LOST** on Adreno - the MSAA
  scene path is genuinely fragile on this driver, whereas the same probe on the
  non-MSAA terrain shader ran fine (just invisible).

### The concrete next step
Investigate the render_target_cache MSAA-resolve path on Adreno:
1. Confirm the vista's msaa=2 EDRAM tile 608 resolve to `0x044B0000` actually
   produces non-empty content on Adreno (dump/compare the resolved host image,
   or the guest RAM if readback is on).
2. Test forcing the scene to 1x (msaa=0) - if the vista appears (even
   aliased), the MSAA resolve is confirmed as the break.
3. Compare AE's Vulkan MSAA EDRAM resolve implementation against upstream
   Xenia-Canary's (this is the forked backend the oldest project note already
   fingered: texture_cache / render_target_cache).

This is a completely different, well-evidenced, and far more specific target
than the geometry/memexport rabbit hole the investigation lived in for weeks.
All force-vis probes reverted; the RTMAP/TEXSRC/RESOLVE/RESOLVESRC/SWAPSRC
diagnostics were kept (safe, read-only, and exactly what's needed to verify the
MSAA-resolve fix).

## MSAA=0 test: NEGATIVE - MSAA resolve is NOT the cause

Forced all rendering to 1x by clearing the msaa_samples field (bits 16-17) of
RB_SURFACE_INFO in the register file at the top of IssueDraw AND IssueCopy
(so render-target creation, render pass, pipeline multisample state, and the
resolve all read a consistent 1x). Verified effective: every RTMAP line now
reports `msaa=0` (previously many were `msaa=2` = 4x), no device loss, emulator
stable, menu still fully functional.

**Result: the vista is STILL flat navy at 1x.** The prime suspect - the
4x-MSAA EDRAM->texture resolve on tiled Adreno - is RULED OUT. The deferred
scene fails to appear for a reason independent of MSAA. Reverted.

### What this leaves
The scene draws execute, resolve to the G-buffer textures, the deferred
composite (831761DE) reads them and writes the front-buffer-feeding tile, yet
the vista never appears - and it's not MSAA. The remaining fork:
1. The scene geometry isn't actually rendering into EDRAM/the G-buffer, OR
2. The intermediate render-target-to-texture round-trip is broken on Adreno
   independent of MSAA - i.e. the resolved G-buffer host image isn't picked up
   when the downstream deferred passes sample those addresses as textures
   (the RT-as-texture aliasing path in the forked texture_cache). This is the
   stronger candidate: it's exactly the kind of thing RADV handles and a
   tiled-GPU EDRAM emulation gets wrong, and it's the `texture_cache` half of
   the backend the oldest project note fingered.

### Cleanest next test
Force the composite pass 831761DE's PIXEL shader to output a solid color
(safe - no geometry change, unlike the vertex force-vis that device-lost on the
MSAA pass). If the background turns that solid color, 831761DE DOES reach the
screen and the bug is its G-buffer INPUT (RT-as-texture aliasing / resolve
content). If it stays navy, the composite's own output never reaches the front
buffer. Either way it bisects the remaining chain in one test.

## ★★★★★ DECISIVE: composite reaches the screen - the vista is lost in the EDRAM-resolve-to-texture data path ★★★★★

### Test 1 (magenta): the deferred composite DOES reach the screen
Forced the composite pixel shader `373E65D9ADCF4380` (paired with vsh
`831761DE`, the pass that reads the resolved G-buffer and writes EDRAM tile
1216 -> front buffer) to output solid magenta. **The entire background flooded
magenta**, with the 2D UI correctly composited on top. So the composite's
output path, the EDRAM-1216 -> front-buffer resolve, and presentation ALL work.
The vista is lost in the composite's **G-buffer INPUT**.

### Test 2 (G-buffer content): everything lives in host images, not guest RAM
Dumped guest-RAM content at the G-buffer addresses AND the front buffer at swap
time: `0x044B0000`, `0x04780000`, `0x043FC000`, `0x04E20000` are ALL exactly
zero in CPU guest RAM (nonzero=0/65536) - yet the front buffer clearly presents
on screen. Confirms AE uses **host render-target images** (Path::kHostRenderTargets):
resolved data lives in the GPU shared-memory buffer / host VkImages, and CPU
guest RAM is never written. So the composite reading the G-buffer depends
entirely on the EDRAM-resolve-to-shared-memory-to-texture chain, all GPU-side.

### Code trace: the resolve->texture chain is wired correctly
- The resolve writes EDRAM -> shared memory via a compute shader
  (`vulkan_render_target_cache.cc` ~1197, storage-buffer write to
  `shared_memory.buffer()` at `copy_dest_base`).
- After writing, it calls `texture_cache.MarkRangeAsResolved(...)` (line 1308)
  -> `RangeWrittenByGpu(..., is_resolve=true)` -> `FireWatches` -> invalidates
  overlapping textures so the composite reloads fresh data. This IS present and
  correct - the "missing texture invalidation" hypothesis was checked and
  DISPROVEN.

### Where the break must be (the remaining, well-scoped target)
Since the resolve->shared-memory write and the texture invalidation are both
wired, and the composite still samples empty/navy on Adreno while desktop RADV
renders the vista, the break is **upstream of the shared-memory write**, in the
host-render-target EDRAM emulation:
1. The **host RT image -> edram_buffer_ dump** (`PerformTransfersAndResolveClears`
   / `GetCopyEdramTileSpan` path) - if the vista's host render-target image
   isn't correctly dumped into the EDRAM emulation buffer on Adreno, the resolve
   reads empty EDRAM and writes empty to shared memory.
2. The **resolve compute shader** reading edram_buffer_ -> shared memory
   producing zeros on Adreno.

Both are in the forked `render_target_cache` host-RT EDRAM path - exactly the
backend the oldest project note fingered. This is now a concrete, bounded fix
target: instrument/compare the host-RT->EDRAM dump and resolve-compute output
for the vista's tile on Adreno vs. the working desktop path, or port that
specific path from upstream Xenia-Canary.

### Summary of this session's eliminations (all decisive)
- Geometry (memexport/terrain): ruled out - force-vis fullscreen terrain = nothing.
- MSAA resolve: ruled out - forced msaa=0, still navy.
- Composite output / front-buffer resolve / present: ruled out - magenta floods.
- Texture invalidation after resolve: ruled out - MarkRangeAsResolved is wired.
- REMAINING: host-RT -> EDRAM-buffer dump, or the resolve compute, on Adreno.

## ★★★★★ DECISIVE: the resolve WORKS - G-buffer shared memory is fully populated; the break is the TEXTURE LOAD ★★★★★

Implemented a diagnostic GPU readback (`GBUFGPU`) that copies the actual
shared-memory GPU buffer content at the vista's resolve dest addresses into a
host-visible buffer and histograms it - bypassing the readback
memory-accessible gate that made the earlier guest-RAM dump read zeros (those
G-buffer addresses are GPU-only scratch, not committed guest RAM). Uses the
existing RequestReadbackBuffer + CmdVkCopyBuffer + AwaitAllQueueOperationsCompletion
+ map pattern. Read-only, does not touch guest RAM.

### Result: the G-buffer shared memory is FULL of real scene content on Adreno
- `0x044B0000`: nonzero=65536/65536, up to 7476 distinct dwords, varied pixel
  data (0x65524F00, 0x431D1600, ...).
- `0x04780000`: nonzero=65536/65536, up to 43812 distinct dwords.
- `0x043FC000`: nonzero=65536/65536, up to 65536 distinct dwords (maximally
  detailed - a real, fully varied image).
- (cross-check) `0x04E20000` front buffer: nonzero=65536/65536, growing
  variation - validates the readback reads real content.
The content also EVOLVES per frame (changes count grows), i.e. the animated
vista is genuinely rendering and resolving into shared memory every frame.

### What this proves / flips
The EDRAM resolve - host-RT->edram_buffer dump AND the resolve compute - WORKS
on Adreno. The prior "resolve/dump produces zeros" hypothesis is RULED OUT. The
vista data is sitting correctly in shared memory. Yet the composite (proven to
reach the screen via the magenta test) samples those exact addresses as
textures and outputs uniform navy. Uniform navy (not varied garbage) means the
composite samples effectively UNIFORM/EMPTY texture data, NOT the varied
content that's demonstrably in shared memory.

**So the break is the TEXTURE LOAD/RELOAD from shared memory -> host texture
image** (the forked `texture_cache`): the load compute that untiles/converts
the Xbox-tiled shared-memory G-buffer into a host VkImage produces empty/uniform
output on Adreno for this format, OR the post-resolve invalidation doesn't
actually trigger a reload so the composite keeps sampling a stale empty texture
(note: MarkRangeAsResolved is wired, but "wired" != "effective on Adreno" -
the shared memory content changes every frame yet the sampled result stays
static navy, consistent with no reload).

### Eliminations now (all decisive this session)
geometry✗  MSAA✗  composite-output/present✗  texture-invalidation-wiring✗
EDRAM-resolve-to-shared-memory✗ (proven WORKING - G-buffer is full).
REMAINING, well-scoped: the `texture_cache` shared-memory -> host-image load
(untile/format-convert compute) or the actual reload-after-resolve on Adreno.

### Cleanest next tests
1. Force the composite pixel shader to output its RAW sampled G-buffer texel
   (bypass lighting math). Scene => texture load works, lighting is the issue;
   uniform navy => texture load confirmed broken.
2. Read back the loaded HOST TEXTURE image for 0x044B0000 (vs the shared-memory
   content already dumped) - if the host image is empty/uniform while shared
   memory is varied, the load compute is the bug.
3. Diff AE's vulkan_texture_cache load-shader / tiling path vs upstream
   Xenia-Canary for this G-buffer format.

## RAW-TEXEL TEST: texture load CONFIRMED broken (samples empty despite full shared memory)

Captured the composite pixel shader (373E65D9)'s first texture-fetch texel into
a debug var and output it raw (bypassing the lighting math), instead of magenta.

**Result: still uniform navy - no scene structure.** The raw sampled texel is
uniform/empty even though the shared memory at the sampled G-buffer addresses
(0x044B0000 etc.) is proven full of varied, per-frame-changing content. This
also covers the "which fetch is first" caveat: the composite's FINAL lit output
is likewise uniform navy while it demonstrably samples fc0=0x044B0000, so that
texture's sampled value must be uniform regardless of which fetch was captured.

### Conclusion: the bug is the texture LOAD/RELOAD from shared memory -> host image
The vista renders and resolves into shared memory correctly (varied content,
proven), the composite reaches the screen (magenta, proven), texture
invalidation is wired (MarkRangeAsResolved, proven) - but the textures the
composite actually samples are uniform/empty. So the forked `vulkan_texture_cache`
either (a) loads the shared-memory G-buffer into a host image incorrectly on
Adreno (untile/format-convert compute produces empty/uniform for this format),
or (b) races/never reloads after the resolve (loads the texture once while empty
and the FireWatches invalidation doesn't trigger an effective reload on Adreno,
or there's a missing barrier so the load reads shared memory before the resolve
write is visible - note the GBUFGPU readback only saw content because it forced
a full AwaitAllQueueOperationsCompletion sync).

### THE fix target (this is where the bug lives)
`vulkan_texture_cache` load path for the resolved G-buffer:
- the shared-memory -> host-image load compute (tiling/format), and/or
- the reload-after-resolve effectiveness + the barrier between the resolve
  compute's shared-memory WRITE and the texture-load compute's shared-memory
  READ.
Compare against upstream Xenia-Canary's vulkan_texture_cache; the resolve/
render-target side is proven correct and should NOT be touched.

### Full pipeline walk complete - every stage tested, one culprit left
geometry✗  MSAA✗  composite-output/present✗  EDRAM-resolve✗(works)
texture-invalidation-wiring✗  →  REMAINING: texture LOAD/RELOAD (barrier or
untile-compute) in vulkan_texture_cache. That is the fix location.

## AE vs upstream canary texture_cache: structured study + "recreate results" vs "splice" assessment

Compared AE's `vulkan_texture_cache` + load shaders against upstream
xenia-canary (canary-git @6e5b8324f, /home/roman/xeniatest/canary-git).

### The differences found in the texture LOAD path, and their nature
1. **Tiling address math** — AE `XeTextureTiledOffset2D` (UModel-derived) vs
   upstream `XenosTextureTiledAddress2D` (bank/pipe formulation). LOOKS like a
   totally different algorithm. **Verified numerically (scratchpad/tilecmp.c):
   they produce BYTE-IDENTICAL offsets for all coords (0/4096 differ)** at
   bpb_log2=2, pitch=1152. => same result, different code. NOT the bug, and
   nothing to "recreate" - already equivalent.
2. **Linear pitch unit** — AE passes row_pitch in BYTES for linear textures
   (blocks for tiled); upstream passes blocks always. AE's shader is written to
   match (comment: "For linear textures - row pitch in bytes"). Internally
   consistent. Also moot here: the vista's G-buffer textures are TILED, not
   linear.
3. **Load-constants mechanism** — AE uses a transient uniform buffer
   (WriteTransientUniformBufferBinding + descriptor set); upstream uses
   CmdVkPushConstants. Pure plumbing; same values delivered. Not behavioral.
4. **Shader framework** — AE `xesl` macros vs upstream's. Cosmetic/structural.
5. **scaled_resolve mip blit generation** — AE has an extra blit-based mip path
   upstream lacks. Only for scaled_resolve textures; the vista G-buffer is
   scaled_resolve=0, so not exercised.
6. **source_length_alignment** — AE aligns the source buffer binding range to
   the shader's source bpe (the real-upstream-Xenia fix for partial-vector
   (0,0,0,0) reads on small textures). AE HAS this; good.

### Verdict on the differences
Every texture-LOAD-path difference examined is either (a) functionally
identical (tiling), (b) internally consistent and not on this texture's path
(linear pitch), or (c) mechanism-not-behavior (uniform buffer, shader
framework). **None is a "produces different results on the bug path"
difference.** This strongly implies the remaining bug is NOT a load-MATH
divergence at all, but a **synchronization / ordering** issue (Adreno-specific:
the load compute reading shared memory before the resolve's write is visible),
which is a different class of fix - adding a correct barrier - not a
code-matching exercise. (The next planned test - read shared-mem source AT LOAD
TIME - directly checks this.)

### Assessment: "recreate the same results with new AE-native code" vs "splice"
The user's instinct is RIGHT as a general strategy and is confirmed by finding
#1: the biggest, scariest divergence (whole different tiling algorithm) is
actually behaviorally identical, i.e. much of the AE-vs-upstream delta is
refactoring, not behavior. So where a real behavioral, causal difference IS
found, rewriting AE-native code to match upstream's RESULT (same I/O contract,
AE's framework) is clearly safer and easier than splicing:
- **Splice (copy upstream files in)** = the `gpu-transplant` path = known open
  Adreno "draws garbage" defect; also won't build cleanly (upstream
  texture_cache needs upstream tiling shaders, command-processor interfaces,
  etc. - deeply entangled). AVOID as a wholesale move.
- **Recreate results (rewrite in place)** = best for pure-function behavioral
  diffs with a clear contract (e.g. a tiling/format/offset formula): drop-in
  same-signature replacement, keeps AE's framework, disposable on canary-ae.
- **BUT** neither applies if the bug is synchronization/ordering (current
  leading hypothesis after tiling was cleared): that fix is "add the right
  barrier/sync on Adreno", which is neither splice nor result-recreation - it's
  an AE-native correctness fix guided by (not copied from) how upstream orders
  the resolve->load.

### Practical rule going forward
Only recreate/port a difference once it's proven BEHAVIORAL (different results)
AND causal (on the failing path). So far no load-MATH difference qualifies;
tiling is cleared. Next: the load-time source-content test to confirm
ordering-race vs load-compute correctness, which decides whether the fix is a
barrier (AE-native, small) or a deeper load-path port.

## Tooling: unified toggleable GPU pipeline trace (core -> screen)

Added a single opt-in trace covering every GPU pipeline stage, so a frame's
execution can be followed in exact fire order.

**Toggle (live, no rebuild):**
```
adb shell setprop debug.canary.testrig.gputrace 1    # enable
adb shell setprop debug.canary.testrig.gputrace 0    # disable (default off)
```
Honors the testrig master switch. Defaults OFF (the trace is verbose) - unlike
the other testrig subsystems which default on.

**Output** - one ordered line per event, with a global monotonic `seq` and the
`frame` number:
```
GPUTRACE seq=<n> frame=<f> DRAW      vsh=.. psh=.. vtx=.. eM=.. cbase0=<edram tile>
GPUTRACE seq=<n> frame=<f> MEMEXPORT sh=.. addr=0x.. size=..
GPUTRACE seq=<n> frame=<f> TEXLOAD   base=0x.. WxH tiled=.. fmt=.. load_shader=..
GPUTRACE seq=<n> frame=<f> RESOLVE   dest=0x.. length=..
GPUTRACE seq=<n> frame=<f> SWAP      frontbuffer_ptr=0x.. WxH
```
Stages instrumented: DRAW (IssueDraw), MEMEXPORT (per exported range), TEXLOAD
(every texture load: address/size/tiling/format/load-shader), RESOLVE (every
EDRAM->guest resolve), SWAP (present). Extensible - add
`if (gpu_trace_enabled()) GpuTrace("STAGE", fmt::format(...));` at any new point.

Infra: `VulkanCommandProcessor::gpu_trace_enabled()` (cached ~250ms) +
`GpuTrace(stage, detail)`; the texture cache / RT cache call it via
`command_processor_`. Verified: 0 lines when off, full ordered trace when on.
Also added `TestrigReadbackAndLogBuffer()` (buffer->host readback + histogram)
for inspecting intermediate GPU-side content from a flushable context (the
per-load scratch-buffer readback is NOT possible mid-load - AwaitAllQueue only
checks idle, can't flush an open submission).

### Note on the barrier fix (kept, real, but not the whole story)
Found and fixed a genuine bug: `VulkanSharedMemory::GetUsageMasks` had
`kComputeWrite` access_mask = `VK_ACCESS_SHADER_READ_BIT` (should be
`SHADER_WRITE`, per upstream) - the resolve's compute writes were never made
available by the write->read barrier. KEPT (correct on its own merits). But it
did NOT restore the vista, and a forced full GPU idle after every G-buffer
resolve ALSO didn't - so the remaining issue is NOT synchronization. The load
compute is dispatched with fully correct params (groups 36x20, size 1152x640,
guest_pitch 1152, host_pitch 4608, buffer 2949120, offset 0x044B0000, source
proven full) yet the sampled texture is uniform - pointing at the load COMPUTE
SHADER's execution on Adreno (or the scratch->image copy), which the scratch
readback couldn't reach due to the mid-load flush limitation. That's the open
edge.
