# Halo 3 BONEC: the constants are CORRECT but barely VARY on Adreno

**2026-08-06.** Identical `BONEC` probe added to both the Xenia-AE Android build
and the desktop RADV oracle, dumping the producer shader's constants as **raw
bits** — `c78` (slot divisor) and `c144-151` (bone matrices) — for shader
`9EA48FC2B26C325D`. Halo 3 menu, same Media ID `699E0227`,
`readback_memexport=true`.

## Result

| | RADV (**correct**) | Adreno (**broken**) |
|---|---|---|
| samples logged | 365 | 5878 |
| **distinct constant sets** | **165** | **11** |
| identical to the other platform | 9 shared | 9 shared |
| unique to that platform | 156 | 2 |

### 1. The values that arrive are CORRECT

The very first sample matches **byte-for-byte** across platforms:

```
c78=00000000 00000000 00000000 00000000
c144-151=00000000 43410000 00000000 48000200 00000000 43640000 48800000 ...
```

9 of Adreno's 11 distinct sets are **bit-identical** to sets RADV also produces.
**The a64 JIT is computing these constants correctly.** That kills the
CPU-side / float-mode theory for this bug as thoroughly as the `denorm`
comparison did.

### 2. But Adreno sees almost no VARIETY

RADV produces **165 distinct constant sets across 365 samples**. Adreno
produces **11 across 5878 samples**. The correct platform varies its bone
matrices constantly; the broken one reuses a tiny handful.

That is exactly the shape of a **collapse**: if nearly every skinned vertex is
transformed by the same small set of matrices instead of its own bone's matrix,
the mesh folds into a point or a spike — which is the reported symptom.

### ⚠️ Before trusting this

**The probes are not in identical code positions.** The AE probe sits after the
existing `CONSTDUMP` block; the oracle probe sits after `VALSHAPE`, and the
oracle probe is **rate-limited to 400 samples** while the Android one is not.
Different sample rates and call sites can produce different variety counts on
their own.

**This must be re-tested with the probe in the same place and the same rate
limit on both sides before it is treated as a finding.** Given this
investigation has already killed four hypotheses that looked solid, that check
is not optional.

## If it survives verification

The question becomes **why the constants stop varying**:

1. **Are the writes reaching the register file?** `WriteRegister` handles
   `SHADER_CONSTANT_*` specially (dirty-marking constant buffers). If updates
   are being coalesced or dropped, the shader sees stale values.
2. **Is the constant buffer being re-uploaded per draw?** Look at
   `current_constant_buffers_up_to_date_` and the float-constant dirty masks in
   `vulkan_command_processor.cc`.
3. **Is the guest issuing fewer distinct draws?** Then it is upstream of the GPU
   entirely and the difference is in game logic/state.

## What is now ruled out for this bug

- ❌ memexport **underfill** — the correct platform fills *less*
- ❌ **denormal flushing** / a64 vs x64 float mode — `denorm` counts match
- ❌ **constant values being miscomputed** — bits are identical where they overlap
- ⚠️ `meanabs` 5.07x gap — real and disjoint, but reads packed data as float32
  so it cannot be interpreted physically; likely a *symptom* of the low variety
  rather than an independent cause

---

## ⭐⭐⭐ CWRITE (2026-08-06): the writes ARRIVE. The problem is WHEN draws see them.

Identical `CWRITE` probe on both platforms, counting every write landing in the
bone-matrix constant range (`c144`-`c151`) inside `CommandProcessor::WriteRegister`.

| | bone-range writes | distinct sets seen at DRAW time (BONEC) |
|---|---|---|
| **RADV (correct)** | **434,176** | **165** (in 365 samples) |
| **Adreno (broken)** | **438,272** | **11** (in 5878 samples; 10 in the first 365) |

### The writes are not being dropped

Counts match within **1%**. The guest issues the same constant updates on both
platforms and they all reach the register file. Combined with BONEC showing the
values are **bit-identical** where they overlap:

- ❌ the guest is not issuing fewer updates
- ❌ we are not dropping or coalescing register writes
- ❌ the values are not miscomputed

### So the variety is lost between the write and the draw

Same writes in, same values, but at **draw time** Adreno's register file shows
~11 distinct bone-matrix states where RADV shows 165. The only thing left that
can explain both facts is **ordering**: on Adreno many draws are issued against
the *same* constant state, while on RADV each draw sees a freshly updated one.

That is a **write/draw interleaving problem** - draws being batched or deferred
relative to the constant updates that are supposed to precede them. It is
upstream of the entire GPU backend, which is exactly why it reproduces on all
three Adreno drivers and why every GPU-side fix failed.

And it is precisely the shape of the symptom: thousands of vertices skinned
against a handful of stale matrices collapses a mesh into a spike.

⚠️ **One control still missing.** The oracle's BONEC is rate-limited to 400
samples while Android's is not, so the two "samples" columns are not directly
comparable. What *is* comparable and does survive: Adreno's first **365**
samples yield **10** distinct sets versus RADV's **165** from the same count,
and Adreno never exceeds 11 no matter how long it runs.

### Next: find what defers the draws

1. **Count draws per constant update on both sides** - the direct measurement of
   the interleaving hypothesis.
2. Look at how the PM4 stream batches draws vs `SET_CONSTANT` packets, and
   whether AE defers or reorders draw submission (the frame-budget work showed
   the CP thread runs 99% executing, so it is not stalling).
3. Check `current_constant_buffers_up_to_date_` and the float-constant dirty
   masks in `vulkan_command_processor.cc` - if a constant upload is skipped
   because the mask says "clean", draws would legitimately see stale data.
