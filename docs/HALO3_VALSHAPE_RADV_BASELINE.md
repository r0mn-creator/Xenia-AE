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
