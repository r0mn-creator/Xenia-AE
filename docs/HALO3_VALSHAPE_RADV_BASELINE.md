# Halo 3 memexport VALUE comparison — RADV baseline captured

**2026-08-06.** Fill counts were refuted as a red herring
(`HALO3_MEMEXPORT_READBACK.md`), so this compares the **statistical shape of the
exported float values** instead. A raw byte diff is meaningless because scene
state differs between platforms; a collapsing-to-a-point skinning result should
show as clustered magnitudes or non-finite values.

The `VALSHAPE` probe already exists **identically on both sides** (Xenia-AE
Android build and the desktop oracle).

## RADV baseline — the platform that renders CORRECTLY

Halo 3 menu, Media ID `699E0227`, `--readback_memexport=true`, 400 samples,
buffer `0x05748D80`, producer `9EA48FC2B26C325D`. Stable across samples:

```
nonfinite = 632          zero   = 134627
denorm    = 3238         finitenz = 8101
min = -3.347e+38   max = 3.336e+38   meanabs = 1.215e+37
mag[<1e-3, <1, <1e2, <1e4, <1e6, >=1e6] = 4844 180 101 105 127 2744
```

## ⭐ Why `denorm = 3238` is the number to watch

The correct-rendering platform exports **3238 denormal floats**. Denormals are
exactly the value class the a64/x64 float-mode asymmetry would destroy
(`HALO3_FP_MODE_LEAD.md`):

```cpp
// x64 - x64_backend.h:115
DEFAULT_VMX_MXCSR = 0x8000        // FTZ: flush denormal OUTPUTS
                  | 0x0040        // DAZ: denormal INPUTS treated as zero
                  | _MM_MASK_MASK;
// a64 - a64_backend.h:92
DEFAULT_VMX_FPCR  = (1 << 24);    // FZ only; AH unset, FZ16 unset, RMode default
```

**Prediction to test:** if Adreno's `denorm` count is **near zero** while
RADV's is ~3238, denormals are being flushed somewhere they should not be, and
the resulting geometry is wrong. That would be a **CPU-side** cause for what has
been treated as a GPU bug for weeks - and consistent with the collapse being
driver-independent (all three Adreno drivers) yet absent on desktop.

Note the ~2744 values at `>=1e6` and `meanabs ~1.2e37`: these are huge
magnitudes, so the buffer is not obviously garbage on either platform - the
comparison has to be done on the distribution, not by eye.

## Also worth comparing

- `nonfinite = 632` - if Adreno differs materially, NaN/Inf handling diverges
  (the RPCS3 transcript notes PS3 SPUs have no NaN/Inf and needed workarounds;
  Xenos VMX128 has its own quirks).
- The `mag[]` buckets - a collapse-to-a-point shows as magnitudes clustering.

## Next step (needs a device run)

1. On Android: `readback_memexport = true`, `debug.canary.testrig.master 1`,
   `debug.canary.testrig.gpu 1`, boot Halo 3 to the menu on Turnip.
2. `grep VALSHAPE xe.log` and compare against the block above.
3. If `denorm` collapses on Adreno, test `DEFAULT_VMX_FPCR = 0` (no flush) and
   `FZ | AH` behind `debug.canary.vmx_fpcr_mode`.

⚠️ The VTXDIST probe grew `xe.log` to **3.6 GB** last run - delete it afterwards.

## Reproducing the oracle side

```
cd /home/roman/xeniatest/oracle/build/bin/Linux/Release
./xenia_canary --storage_root=/home/roman/xeniatest/oracle_data/Xenia \
               --readback_memexport=true "/home/roman/xeniatest/games/Halo 3.iso"
grep VALSHAPE xenia.log
```

---

## ⭐ RESULT (2026-08-06): FP-mode hypothesis REFUTED; `meanabs` is the real signal

Both sides, 400 samples each, same Media ID `699E0227`, Halo 3 menu,
`readback_memexport=true`. Adreno on Turnip.

| metric | RADV (**correct**) | Adreno (**broken**) | |
|---|---|---|---|
| **denorm** | **3080** | **2970** | **SAME** |
| zero | 134994 | 133656 | same |
| nonfinite | 574 | 676 | +18% |
| finitenz | 7786 | 9028 | +16% |
| **meanabs** | **1.213e+37** | **1.923e+36** | **6.3x LOWER** |

mag buckets `[<1e-3, <1, <1e2, <1e4, <1e6, >=1e6]`
RADV `[4604, 164, 96, 99, 122, 2666]` · Adreno `[5082, 249, 156, 163, 176, 3187]`

### Denormals survive — the float-mode lead is DEAD

If `FPCR.FZ` were flushing denormals, Adreno's count would collapse toward zero.
**2970 vs 3080 is noise.** The a64/x64 asymmetry in `HALO3_FP_MODE_LEAD.md` is
real in the source but **does not manifest on this path**. Do not pursue it for
this bug. (Whether `AH`/`FZ16`/`RMode` matter elsewhere is untested and
separate.)

### What actually differs: mean magnitude, 6.3x lower on Adreno

RADV `meanabs = 1.213e+37`, Adreno `1.923e+36`. Adreno's exported values are
systematically **smaller**, and it has **more** nonzero values (9028 vs 7786).

⚠️ **Do not over-read this.** Scene state differs between platforms (camera,
frame, animation phase), so absolute counts are not directly comparable, and
Adreno having more values in *every* magnitude bucket is partly just "more
nonzero data". The **ratio** is the interesting part: same buffer, same shader,
mean magnitude off by 6x.

A collapse-to-a-point would show as magnitudes shrinking toward a common value -
which is directionally consistent with a 6x lower mean. But it is one metric
from one frame pair. **Confirm before building on it.**

### Next

1. **Control for scene state.** Capture both at a comparable menu moment, or
   sample many frames and compare distributions rather than single medians.
2. **If the 6x holds**, the producer is writing systematically smaller values on
   Adreno -> look at `c78.x` and the bone matrices `c[144+aL]` (still never
   compared) and at the producer's own math.
3. `nonfinite` differing (574 vs 676) is worth watching but is small next to a
   6x magnitude gap.
