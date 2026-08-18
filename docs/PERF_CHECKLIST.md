# Xenia-AE performance checklist

**Baseline: 17.34 FPS** (Halo 3 main menu, Odin 2, ~95 °C, `fps_bench.sh`, n=69).
**Goal: 30 FPS.** Needs **+73%**.

Everything below is ranked by *measured* cost in the profile of **2026-08-18**,
the first profile taken after the `-O2` fix. Every ranking older than that date
is void — at `-O0` wrapper overhead dominated whatever it wrapped.

## The one number that reframes everything

| Where the CPU goes | Share |
|---|---|
| **JIT'd guest code** | **77.3%** |
| `libe.so` (our own C++ — GPU backend, kernel HLE, everything) | 8.8% |
| `[vdso]` | 5.4% |
| kernel | 5.1% |
| libc | 1.2% |
| Turnip (Vulkan driver) | 1.1% |

**Our emulator code is under a tenth of the total.** Even deleting the entire
Vulkan backend could not buy 73%. The work is in the **JIT** and, specifically,
in **how guest wait-loops are executed**.

This retires the "let Android and Adreno do the rest" plan as a *performance*
strategy — post-process AA and resolution scaling are worth doing for image
quality and for GPU-bound scenes, but at the menu we are CPU-bound and they
cannot deliver the 73%.

## Hot functions (share of whole process)

```
32.77%  guest_825A7EC8      <-- one guest spin loop, 63% of the RENDER thread
10.69%  guest_82145ED8
 5.36%  __kernel_clock_gettime
 4.96%  guest_826C61C8
 3.43%  guest_825A18C0
 2.45%  guest_826C5010
 2.44%  __restgprlr_29      \  PPC register save/restore helpers
 2.34%  guest_82103AD8       |
 1.87%  guest_826C2810       |
 1.64%  __savegprlr_29      /   = 4.08% combined
```

---

## The checklist

### 1. ~~Collapse guest spin/delay loops~~ — TRIED 2026-08-18, BUYS NOTHING

**Result: no change to the frame rate. All three passes stay off.**

The passes were already present and registered — this entry was wrong to call
them missing. They never fired because all three cvars default false. They are
now individually switchable at runtime (`debug.canary.exp_collapse_delay_spins`,
`exp_collapse_ctr_spins`, `exp_park_memory_polls`), so this is cheap to re-test.

| 180 s run, Halo 3 menu | median | p05 | stalls <10 FPS |
|---|---|---|---|
| control | 14.93 | 14.19 | 0% |
| `collapse_memory_delay_spins` | 14.87 | — | — |
| `park_memory_poll_loops` | 14.91 | 1.40 | **14%** |

`collapse_memory_delay_spins` **worked perfectly and changed nothing.** It
collapsed the loop at guest `825A7EE4` (sled=8), the RENDER thread fell from
**52% → 4.6%** of process CPU, and `guest_825A7EC8` vanished from the profile.
The frame rate did not move. The freed CPU went straight into `guest_826C61C8`,
which rose **5% → 36%** — a pure load-test-branch with no store, i.e. the guest
waiting for something external to zero a memory word. A fence wait.

`park_memory_poll_loops` has the same median and adds a 14% stall rate.

**The lesson, which invalidates how this list was ordered:** the 33% busy-wait
was never the bottleneck — it was a *throttle*. This scene is bound by **wait
latency, not CPU throughput**. Ranking work by CPU share is the wrong ranking
function for an emulator whose guest threads spin: a spin loop is a *symptom* of
something else being slow, and deleting it just moves the spin somewhere else.

**What to ask instead of "what uses the most CPU":** what is `guest_826C61C8`
waiting for, and what on our side is late to write that word? That is the
bottleneck. Start there.

The one untried configuration is parking **with `guest_scheduler=true`**, so
parked loops have safepoints to wake them promptly — the restored
`OPCODE_CHECK_PREEMPT` tolerance unblocks it, and it is what §34.2 step 4 was
always about. Note the scheduler is itself untested on device.

<details>
<summary>Original entry (kept — the loop analysis is still accurate)</summary>

**The single biggest item by a factor of three.**

`guest_825A7EC8` is 32.77% of the process and 63% of the RENDER thread. Its
host JIT code (`disas.sh <dev> <pid> aa073fc80 594`) is a **self-decrementing
countdown delay loop**:

```
+0100  yield  x8            <-- 7.7%  (spin hint)
+0120  ldr    x22,[x20,#48]
+0134  ldr    w23,[x21,x0]  <-- load guest counter
+0138  rev    w23, w23      <-- byte-swap (guest is big-endian)
+0140  sub    x23, x23, #1  <-- decrement
+015c  str    w17,[x21,x0]  <-- store back
+0170  ldr    w22,[x21,x0]  <-- reload
+0178  mov    w23, w22      <-- 17.8%
+01a0  cbz    w22, #-160    <-- loop while != 0
```

Nothing external gates the exit — the loop only waits on its own counter. It is
a **pure guest busy-wait, not a GPU sync stall**, so collapsing it is safe and
does not just move the stall elsewhere.

It burns a full core because **ARM's `yield` is a no-op without SMT** — the
guest's pause hint, which throttled this loop on the 360's hardware threads,
does nothing on an Adreno-class SoC.

**Do:** port XenDroid's three passes — `collapse_memory_delay_spins`,
`collapse_ctr_spin_loops`, `park_memory_poll_loops`. Start with
`collapse_memory_delay_spins`; it targets exactly this shape.
**Verify first:** confirm the same loop dominates *in gameplay*, not only at the
menu, before sizing the win.

</details>

### 2. Inline `__savegprlr` / `__restgprlr` — 4.08%
The PPC register save/restore helper thunks show up as real call frames.
XenDroid has `inline_gprlr_saverest`. Known-good, self-contained, low risk.

### 3. a64 peephole pass — emitted code is 7-8% literal no-ops
In the two hottest functions, **32 of 357** and **27 of 372** emitted
instructions are `mov wN, wN` — a zero-extend idiom that assembles to nothing
useful. The backend also recomputes the same address constant on every use:

```
mov  w17, #80        ; repeated 4x in one loop body
add  w0, w0, w17     ; for the same offset
```

A peephole that drops identity `mov`s and hoists loop-invariant address
constants is cheap to write and pays out across *every* guest function, not
just the hot ones.

### 4. Find the remaining `__kernel_clock_gettime` — 5.36%
Down from 22% but still material. Two clock-read regressions have already been
found and fixed this way, so check before assuming it is legitimate. Prime
suspect: the **GPU Frame limiter** thread (4.7% of process).
**Rule for this tree:** rate-limit on the diag epoch, never on a timestamp.

### 5. `inline_leaf_calls` — XenDroid pass we lack
Unquantified here, but it compounds with #2 and #3.

### 6. Compile the diagnostic probes out of shipping builds
`VTXDIST` / `MEMSRC` / `RECFIELD` do full 143,360-dword scans per vertex fetch
when enabled. Now that `libe.so` is only 8.8% of CPU the ceiling is smaller than
it looked, but they should be `#if`-compiled out regardless — a runtime gate
still costs a branch in a per-vertex path.

### 7. Try `-DNDEBUG` — untested, single variable
Deliberately left out of the `-O2` change so it can be measured on its own. It
strips every `assert_true`/`assert_always`. Measure it alone.

### 8. Post-process AA + resolution scaling — image quality, not the 73%
`postprocess_antialiasing` and `postprocess_scaling_and_sharpening` are empty
strings in AEX; XenDroid wires them up. Worth doing, and worth doing for
GPU-bound *gameplay* scenes — but per the table above this is not where the
frame rate is.

---

## Known dead ends

- **Making the GPU backend faster.** 8.8% total for all of `libe.so`.
- **Driver-level work.** Turnip is 1.1%.
- **Frame skip.** Would raise the counter without making the game more
  responsive; the guest spin loop burns the same core either way. Fix #1 first
  and see what is actually left.

## Method notes

- Profile with `scripts/jit_profile.sh`. **`debug.canary.perf_map=1` must be set
  *before* launching** — the map is written at emulator start, and without it
  77% of samples land in `unknown[+addr]`.
- Symbolize host frames against the unstripped `libe.so` whose **build-id
  matches** the crash/profile. The `cxx/<Config>/<hash>/obj` directory gets a
  new hash when build flags change, and the stale one silently resolves
  everything to `??`.
- Benchmark with `fps_bench.sh record <name> 180` then `compare a b`. Never a
  single reading — scene variation alone is +/-25%.
- **Use 180 s runs and a contemporaneous control.** Two 70 s runs of *identical
  code* measured 17.34 and 14.93 here (-14%, p=0.26): between-run drift exceeds
  within-run noise, so a baseline recorded an hour ago is not a control. This
  cost a wrong "-14.3% significant regression" call on 2026-08-18 before the
  proper control was run. Record the control in the same sitting, every time.
- Read `p05` and the stall rate, not only the median. `park_memory_poll_loops`
  matched the control's median exactly while putting 14% of frames under
  10 FPS.
- The Odin 2 reaches 94-95 °C in ~2 minutes and clamps cpu4. Compare runs at the
  same thermal equilibrium, back to back.
