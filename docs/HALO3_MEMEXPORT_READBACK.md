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
