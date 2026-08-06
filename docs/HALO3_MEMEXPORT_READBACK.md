# Halo 3 memexport: the "underfill" theory is WRONG

**2026-08-06.** First measurement ever taken with **`readback_memexport = true`**.
Halo 3 main menu, Turnip v26.0.0 R8, Odin 2. 10324 VTXDIST samples, 536 distinct
buffer states.

## The instrument was blind before this

Every prior fill-rate number in this project was measured with
`readback_memexport = false`. In that state **VTXDIST reads GUEST RAM, not the
GPU's buffer** - so it was reporting a stale CPU-side copy the GPU never wrote
to. The "~6% fill, therefore Adreno underfills the memexport buffer" conclusion
rests entirely on those readings.

## What the GPU buffer actually contains

| idx | addr | max nonzero | total | fill % | lastnz | density within written span |
|---|---|---|---|---|---|---|
| 91 | `0x0CA3CC60` | 1074 | 1074 | **100.0%** | 1073 | **100.0%** |
| 94 | `0x05746E80` | 11661 | 143360 | 8.1% | 15998 | **72.9%** |

### 1. ⭐ A memexport buffer fills to **100%**

Buffer `0x0CA3CC60`: **1074 / 1074**, fully dense, observed 1127 times.
**Memexport is not fundamentally broken on Adreno.** A production shader writes
a buffer completely and correctly. That alone refutes "Adreno drops
vertex-stage stores".

### 2. The big buffer is not *scattered* - it is *bounded*

`0x05746E80` is 143360 entries but nonzero data stops at index **15998** and is
**72.9% dense within that span**. That is not "random stores lost" - that is
"the producer wrote the first N vertices and then stopped".

Prior work assumed a scatter/compaction failure. The data says the written
region is contiguous and dense; only the *extent* is short.

### 3. It fills **progressively across draws**

Sampling the same address over time shows the buffer growing:

```
nonzero=780    lastnz=1278
nonzero=6074   lastnz=10878
nonzero=11661  lastnz=15998
```

So a single VTXDIST reading is a **snapshot mid-fill**, not a final state.
Earlier low numbers may simply have sampled early. **Any fill figure quoted
without saying when it was sampled is meaningless.**

## What this means for the five failed fix attempts

All five targeted "make Adreno land more stores" or "fix slot-selection math".
If the buffer is bounded rather than scattered, and another buffer fills 100%,
those were aimed at a mechanism that is not failing.

## Next questions (in priority order)

1. **Is 8.1% actually correct for that buffer?** 143360 entries may be a
   worst-case allocation the menu scene never needs to fill. **We still have no
   control measurement.** The desktop RADV oracle renders Halo 3 correctly -
   run it there and compare `nonzero/total` for the same address. If RADV also
   stops near 16000, fill rate is a **red herring** and the bug is entirely in
   the consumer.
2. **Why does it stop at ~15998?** 143360 / 15999 ≈ 8.96. Check whether the
   producer's draw vertex count, an index-buffer limit, or `c78.x` bounds the
   run.
3. **What differs between buffer 91 (100%) and buffer 94 (8%)?** Same GPU, same
   driver, same frame. Compare their producer shaders and vertex counts - the
   contrast is the cheapest available discriminator and it has never been used.

## Reproducing

```
# config: readback_memexport = true      <-- REQUIRED, else VTXDIST reads guest RAM
adb shell setprop debug.canary.testrig.master 1
adb shell setprop debug.canary.testrig.gpu 1
# launch Halo 3 (Turnip driver), sit at the main menu
adb shell "grep VTXDIST .../xe.log"
```
Remote launch note: `am start` on `EmulatorActivity` does **not** work - it needs
`MainActivity` first. Tap selects a tile, then `KEYCODE_DPAD_CENTER` activates.

---

## ⭐ Buffer 91 vs 94: the histogram answers it

Confirmed on a second independent 60 s sample (12370 lines): identical numbers,
and **`lastnz = 15998` byte-for-byte both times**. A deterministic cutoff, not
random store loss - random loss cannot land on the same index twice across
~22000 samples.

### The 10-bucket histogram bins the buffer by INDEX RANGE

| buffer | consumer shader | histogram | reading |
|---|---|---|---|
| **91** | `6A1B637595275378` | `108 107 108 107 107 108 107 108 107 107` | **uniform across all 10 buckets** |
| **94** | `9EA48FC2B26C325D` / `488D9488AB7ED7D8` | `7954 1053 0 0 0 0 0 0 0 0` | **buckets 2-9 completely empty** |

Buffer 94 is 143360 entries, so each bucket spans 14336. `lastnz = 15998` falls
in bucket 1 - and the histogram agrees exactly: bucket 0 full (7954), bucket 1
partial (1053), buckets 2-9 **zero**. Two independent measures of the same fact.

### What actually differs

1. **Buffer 91 is sized to its content**: 1074 entries, 1074 written, uniformly
   distributed. **Buffer 94 is 143360 entries with ~16000 written**, all at the
   bottom of the range.
2. **Consumer draw sizes differ by two orders of magnitude.** Buffer 91's
   consumers draw 39-4968 vertices. Buffer 94's consumers draw **1-64**.

### ⭐ Leading hypothesis: 8.1% is CORRECT, and there is no underfill

Everything is consistent with `0x05746E80` being a **fixed worst-case
allocation** that the main-menu scene simply does not need to fill:

- the cutoff is **deterministic** (scene-determined, not flaky)
- the written region is **dense** (72.9% - the producer is writing what it means to)
- a **right-sized** buffer in the same frame on the same GPU fills **100%**

If that holds, "Adreno underfills the memexport buffer" was never real - it was
an artifact of comparing bytes-written against a buffer far larger than the
scene, measured through an instrument (`readback_memexport=false`) that was
reading guest RAM rather than the GPU buffer.

**That would mean all five previous fix attempts were aimed at a non-existent
bug**, which also explains why every one of them failed without any of them
being obviously wrong.

### The one measurement that settles it

Run the **RADV desktop oracle** on the Halo 3 menu and read `nonzero/total` for
the same buffer. RADV renders Halo 3 **correctly**.

- RADV also stops near 16000 → **fill rate is a red herring**; the bug is
  entirely in the consumer (`488D9488AB7ED7D8`) or its constants, and the
  producer side can be dropped from the investigation.
- RADV fills far more → underfill is real after all, and the deterministic
  cutoff at 15998 is the thing to explain.

Until that control exists, **no conclusion about "underfill" is supportable in
either direction** - which has been true for this entire investigation and is
why it kept stalling.
