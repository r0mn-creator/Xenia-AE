# Adreno GLSL intrinsic probe

Standalone Vulkan compute tools for directly measuring what a target GPU's
`GLSLstd450` intrinsics (`Sin`/`Cos`/`Sqrt`/`InverseSqrt`/`Trunc`/`Floor`/
`Fract`) actually compute for known inputs, independent of any game or of
Xenia itself. Built during the Halo 3 menu investigation (see
`../../docs/HALO3_MENU_INVESTIGATION.md`) after repeatedly getting
inconclusive results trying to infer GPU precision behavior from a noisy,
non-deterministic full-game rendering proxy — this gives ground truth
instead, diffable against a Python/numpy float64-computed float32 reference.

## Why this exists

Xenia's SPIR-V shader translator calls the driver's own `GLSLstd450` builtins
for these ops by default. SPIR-V does not mandate any particular ULP
accuracy for the extended instruction set (unlike base arithmetic, which is
covered by the `SPV_KHR_shader_float_controls` execution modes Xenia already
enables). Different GPU vendors' implementations can diverge meaningfully,
and Xbox 360 games authored/tested only against real Xenos hardware (and, in
practice, against desktop emulation on AMD/RADV, which tends to be far more
accurate) can be sensitive to that divergence in ways that are very hard to
diagnose from inside a running game.

## Files

- `probe.comp` / `probe_host.cc` — dispatches `sin/cos/sqrt/inversesqrt/
  trunc/floor/fract` for a fixed set of test inputs (integers, near-integer
  boundary cases, pi-multiples, and Halo 3's own captured shader constants),
  outputs CSV. Use `compare.py` to diff against the reference.
- `probe_codywaite.comp` — companion shader for `probe_host2.cc`: computes
  `native_sin/native_cos` alongside a portable Cody-Waite-reduced sin/cos
  (matching `SpirvShaderTranslator::PortableSinCos`'s algorithm) for direct
  side-by-side comparison. `probe_host2.cc` sweeps large magnitudes
  (0-100000) specifically, since that's where native sin/cos was found to
  diverge. Use `compare2.py` to diff both against the reference.
- `compare.py [csv] [label]` — ULP/absolute-error analysis for probe.comp
  output.
- `compare2.py [csv]` — native-vs-portable absolute-error comparison for
  probe_host2.cc output (works for probe2.comp's "portable" or
  probe_codywaite.comp's "Cody-Waite" variant - the CSV header always says
  "portable_sin/portable_cos" regardless of which reduction algorithm the
  dispatched shader actually used).

## Building and running

Requires `glslangValidator` (shader compiler) and the Android NDK (already
configured for this project - see `local.properties`).

```bash
# Compile shader to SPIR-V
glslangValidator -V probe.comp -o probe.spv

# Cross-compile the host program for arm64 Android
NDK=/home/roman/Android/Sdk/ndk/27.0.12077973
CLANG=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang++
$CLANG -std=c++17 -O2 probe_host.cc -o probe -lvulkan -static-libstdc++

# Push and run on ANY connected Android device (does not need to be running
# Xenia/Canary AE at all - this is a completely standalone Vulkan compute
# dispatch, useful for testing any Adreno/Mali/etc. device you have access
# to for comparison)
adb -s <device serial> push probe /data/local/tmp/probe
adb -s <device serial> push probe.spv /data/local/tmp/probe.spv
adb -s <device serial> shell chmod +x /data/local/tmp/probe
adb -s <device serial> shell "cd /data/local/tmp && ./probe probe.spv" > results.csv

python3 compare.py results.csv "device label"
```

## Key findings so far (Adreno 610 & 740)

- `trunc`/`floor`/`fract`: bit-exact against the reference across every
  tested input, including all near-integer boundary cases. Not a source of
  divergence.
- `sqrt`/`inversesqrt`: within 1 ULP - essentially optimal.
- `sin`/`cos`: genuinely diverge at large magnitudes due to insufficient
  range-reduction precision - up to ~1.6e-2 absolute error around
  magnitude 3e5, and ~6.85e-4 at x=10000 specifically (a real constant used
  by Halo 3's menu vista shader). A naive single-float32-subtraction
  portable reduction has the *same* catastrophic-cancellation problem as the
  native intrinsic at large magnitudes (mixed win/loss, not a fix). Cody-Waite
  3-term reduction is 45-85x more accurate, consistently, across the full
  0-100000 sweep - see `PortableSinCos` in
  `../../app/src/main/cpp/xenia-canary/src/xenia/gpu/spirv_shader_translator_alu.cc`.
- `exp2`/`log2`: within 1 ULP - essentially optimal (`probe_explog.comp` +
  `probe_host_explog.cc` + `compare3.py`).
- `mova` (address-register computation: `floor(x+0.5)` -> `clamp(-256,255)`
  -> convert-to-int, the mechanism behind `c[N+a0]`-style dynamic constant
  indexing): zero mismatches across every tested input including all exact
  `.5` rounding-boundary cases. Fully correct - and the DOWNSTREAM constant
  read itself (`GetStorageAddressingIndex` in spirv_shader_translator.cc) is
  pure 32-bit integer arithmetic + buffer indexing, which has no
  floating-point vendor-variance risk at all by the Vulkan/SPIR-V spec.

**Conclusion for Halo 3's menu shader (9EA48FC2B26C325D)**: every individual
ALU operation it uses has now been audited this way. Only SIN/COS was
genuinely broken; everything else (TRUNC/FLOOR/FRACT/SQRT/INVERSESQRT/EXP2/
LOG2/mova) is exact or within 1 ULP. If a game's rendering is still wrong
after fixing SIN/COS, per-operation precision is very unlikely to be the
remaining cause - look at GPU-execution-level issues instead (draw
ordering/barriers, intra-draw invocation race conditions, etc.) - see
`../../docs/HALO3_MENU_INVESTIGATION.md` for how that reasoning played out.

## Extending this

To test a different device: just push and run the existing `probe`/`probe.spv`
binaries (no rebuild needed) and diff with `compare.py`.

To test different operations or a different candidate fix algorithm: edit
the `.comp` shader, recompile with `glslangValidator`, and reuse the existing
host binary (the host program's descriptor/buffer layout is generic - it
just needs the SPIR-V's input/output buffer bindings and stride to match
what the host program allocates).
