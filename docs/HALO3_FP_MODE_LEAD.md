# Halo 3 geometry: the a64 vs x64 floating-point mode asymmetry

**2026-08-06.** Found using the local ARM manual (`reference/arm_arm.txt`).

## Why a CPU lead for a "GPU" bug

The Halo 3 geometry collapse has always been attributed to Adreno. But
**"Adreno" has never been separated from "a64 JIT"**: every failing observation
is Android (Adreno **and** ARM64), every passing one is desktop (RADV **and**
x64). The two variables have never been varied independently.

That matters because the bone matrices / `c78.x` feeding the collapsed geometry
are **computed by guest CPU code running through the JIT**. If a64 float
behaviour differs from x64's, the constants differ - and no GPU-side fix can
help.

Note this is now more attractive than before, because
`HALO3_MEMEXPORT_READBACK.md` shows the "Adreno underfills memexport" premise -
the basis of five failed fixes - is itself in serious doubt.

## The asymmetry

Both backends set a VMX (vector) float mode. They are **not** equivalent.

```cpp
// x64: x64_backend.h:115
constexpr unsigned int DEFAULT_VMX_MXCSR =
    0x8000 |                   // FTZ - flush to zero (OUTPUTS)
    0x0040 | (_MM_MASK_MASK);  // 0x0040 = DAZ - denormals-are-zero (INPUTS)

// a64: a64_backend.h:92
constexpr unsigned int DEFAULT_VMX_FPCR = (1 << 24);  // FZ
```

x86 splits denormal handling into **two independent bits**:
- `FTZ` (0x8000) - flush denormal **outputs** to zero
- `DAZ` (0x0040) - treat denormal **inputs** as zero

x64 sets **both**. ARM has a single `FPCR.FZ`, and per the manual (C-code
`FZ, bit [24]`) its meaning is **conditional on `FPCR.AH`**:

> If `FPCR.AH` is 0, [FZ=0] disables flushing to zero of **inputs and outputs**...
> If `FPCR.AH` is 1, [FZ=0] disables flushing to zero of **outputs**...

So `FZ` alone is not a clean equivalent of `FTZ|DAZ` - its input-side behaviour
depends on `AH`, and **`AH` is never set anywhere in the a64 backend**. Also
unset: **`FZ16`** (half-precision denormals), which matters because Xenia has a
memexport **float16** path (`project_xenia_ae_upstream_diff_findings`).

Rounding mode is also asymmetric: x64 explicitly sets `0x0040` plus a rounding
mode "for vmx"; a64 sets **only** `FZ` and leaves `FPCR.RMode` at its default.

## Why this fits the symptom

Character/geometry collapse is skinning math: vertices multiplied by bone
matrices. Small float divergence in matrix construction produces exactly this -
**geometry that is present but positioned catastrophically wrong**, while
everything not skinned renders fine. That is what Halo 3 shows.

It also explains why it is **driver-independent** (reproduces on stock Adreno,
Turnip a7xx, Turnip a6xx) yet **absent on desktop** - the split is CPU
backend, not GPU.

## Caveat - do not oversell this

The a64 vector backend was previously **audited and cleared** for VMX128
correctness (`project_xenia_ae_arm64_vector_jit_lead`). But that audit covered
**instruction selection**, not **FP mode/denormal/rounding configuration**.
This is a different question and was not in scope.

## Tests, cheapest first

1. **Dump the constants.** Log `c78.x` and the bone matrices at `c[144+aL]` for
   the producer shader on Android and on the desktop oracle, and diff the raw
   bits. **This is the decisive test and needs no theory to be right** - it has
   been the outstanding lead for weeks.
2. **Toggle the FP mode.** Try `DEFAULT_VMX_FPCR = 0` (no flush) and
   `FZ | AH`, behind `debug.canary.vmx_fpcr_mode`. If geometry changes at all,
   float mode is implicated.
3. **Check NJM mapping.** `kA64BackendNJMOn` exists (a64_backend.h:60); confirm
   the PPC VSCR **Non-Java Mode** bit is actually mapped onto `FPCR.FZ`, and
   that switching NJM at runtime updates FPCR.

## Manual references (local copy)

`reference/arm_arm.txt` - grep `"^FZ, bit \[24\]"`, `"FPCR.AH"`, `"FZ16"`,
`"Flushing denormalized numbers to zero"`. Chapter **C6** for instructions,
**D** for system registers.
