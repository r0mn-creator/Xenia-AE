# Lessons from RPCS3's ARM optimisation work

Source: RPCS3 dev video transcript (2026). **They used the same device we do -
an Ayn Odin 2, Snapdragon 8 Gen 2.** They got **+60% performance at -25% power**.
Two of their findings map directly onto measurements we already have.

## 1. ⭐ `yield` IS A NO-OP ON CONSUMER ARM — and we use it in two places

From the ARM manual, as they report it: `yield` only yields **if the hardware has
SMT**. 99% of consumer ARM is SMP without SMT, so on phones/tablets/handhelds
`yield` "does absolutely nothing". ARM's own blog on porting x86 `pause` says the
same. RPCS3 replaced it with **`ISB`** (instruction synchronisation barrier),
which really does stall by restarting instruction fetch.

### Where this bites us

**(a) Our host spin loop.** `base/threading_posix.cc:183` (`MaybeYield`, the
`debug.canary.fast_yield` path) spins on:

```asm
__asm__ __volatile__("yield" ::: "memory");
```

If that is a NOP, our "adaptive backoff" is a **full-speed busy spin** with no
backoff at all. That is consistent with what we measured: the yield change
reclaimed kernel CPU 22.5% -> 3.6% but produced **no FPS gain**.

**(b) ⭐⭐ The GUEST's spin loop - potentially much bigger.** The hottest JIT'd
guest function, `guest_826DEFD0`, is **14.08% of ALL process CPU**, and is a
polling loop whose pause primitive is **eight consecutive `yield` instructions**
(see `JIT_HOTSPOT_826DEFD0.md`). Those are the JIT's translation of the PPC
`db16cyc`-style delay. **If `yield` is a NOP, the guest's own throttle does
nothing** and that spin burns a core at full rate.

This is the first mechanism found that plausibly explains why a *polling* loop
costs 14% of the process.

**Action:** emit `ISB` instead of `yield` in (a) the host `MaybeYield` spin and
(b) the a64 backend's translation of the guest pause/`db16cyc` instruction.
Toggle both; re-measure with a 180 s run.

## 2. Their headline bug does NOT apply to us (checked)

RPCS3's biggest win (+25% perf, -10% power on its own) was a busy-wait that
added a hardcoded `3000` to the hardware timer - about 1 microsecond at x86's
2-4 GHz, but **150 microseconds** on ARM where the generic timer runs at 19 MHz.

**Xenia already does this correctly.** `base/clock_arm64.cc` reads the real
frequency from `CNTFRQ_EL0` rather than assuming an x86 rate, so waits scale
with the actual timer. Worth knowing so nobody goes hunting for it.

## 3. Their other themes, for later

- **LLVM emits materially worse ARM code than x86** for many vector ops; several
  needed hand-written intrinsics or inline asm, and they filed upstream LLVM
  issues. Rhymes with our finding that `guest_826DEFD0` is **36% `mov`/`movk`**
  (register shuffling and constant rematerialisation).
- **"ARM is every bit as complex as x86"** - a rewrite is not required; port the
  x86 optimisations. Directly counters the "build a new emulator for ARM"
  framing.
- **SD8Gen2 has five effective core types**, not three: X3, 2x A715, 2x A710,
  and A510s that come in two flavours - **two share a single 128-bit vector
  unit**, and the third has only a **64-bit** vector unit (128-bit ops run at
  half rate). For SIMD-heavy guest code an A510 can be **slower than the
  original console**. Relevant to any future affinity work: never place vector
  -heavy emulation threads on A510s.

## Priority

The `yield` -> `ISB` change in the **guest pause translation** is the highest
-value item, because it targets the single largest identified CPU consumer
(14%) and the mechanism is now understood rather than guessed.
