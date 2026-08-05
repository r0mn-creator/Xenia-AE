# JIT hotspot: `guest_826DEFD0` — 14% of all process CPU

**Found 2026-08-05** with the new JIT perf map (`debug.canary.perf_map`).
NFS Carbon, in-race, 20 s capture, 71330 samples, 0 lost.

## The headline: the profile is NOT flat

Earlier profiles said "no single hotspot, 1012 symbols, top one 0.48%". That was
an artefact of **36% of cycles being an unresolvable `unknown` blob** — JIT'd
code has no ELF symbols, so the peak was averaged away. With symbols:

| function | % of ALL process cycles | host size |
|---|---|---|
| **`guest_826DEFD0`** | **14.08%** | 1428 B |
| **`guest_826DAAF8`** | **6.11%** | 1668 B |
| `__restgprlr_29` | 2.00% | — |
| `__savegprlr_29` | 1.67% | — |

**Two guest functions = 20.2% of the whole process.** Top 10 = **77% of all JIT
time** across 810 sampled functions. Each compiled exactly once, so this is not
recompilation churn.

Threads: Main XThread 29.7% (85.8% JIT) · MainThread 27.0% (20.5% JIT) ·
GPU Commands 25.0% (0% JIT) · RWAudioCore 6.2% (78.8% JIT).

## What the emitted ARM64 actually does

Dumped from `/proc/<pid>/mem` at the host address in the map and disassembled.
The prologue, **before any guest work**, maintains a shadow call stack:

```asm
sub  sp, sp, #0x50
str  x30, [sp, #0x40]
ldr  x8,  [x19, #0x98]     ; shadow-stack base   (x19 = thread context)
ldr  w9,  [x19, #0xac]     ; shadow-stack depth
mov  w10, #0x10
umull x10, w9, w10         ; depth * 16
add  x8, x8, x10           ; &slot[depth]
mov  x10, sp
str  x10, [x8]             ; slot.host_sp
ldr  w10, [x20, #0x30]
str  w10, [x8, #0x8]       ; slot.<guest field>
ldr  w10, [x20, #0x130]
str  w10, [x8, #0xc]       ; slot.guest_lr
add  w9, w9, #0x1
str  w9, [x19, #0xac]      ; depth++
cmp  w9, #0x10000          ; overflow check vs 65536
b.ge <overflow handler>
```

then per call site:

```asm
ldr  w16, [x19, #0xac]     ; re-read depth
str  w16, [sp, #0x48]      ; save it
mov  x0, #0x826defd8       ; guest return address (materialised in 2 insns)
str  x0, [x20, #0x130]
mov  x9, #0xa00008630      ; trampoline addr (materialised in 3 insns)
blr  x9                    ; indirect call
ldr  w17, [x19, #0xac]     ; re-read depth after return
ldr  w16, [sp, #0x48]
cmp  w17, w16
b.ne <mismatch handler>    ; verify depth unchanged
```

So **every guest call costs ~15 extra instructions** of shadow-stack
bookkeeping and depth verification, plus an **indirect call through a
trampoline** rather than a direct branch.

## Open questions (do these next)

1. **Is the shadow stack a debug feature or load-bearing?** If it exists for
   the debugger / stack traces / `XThread::Reenter`, it may be gateable. Note
   this project already fixed a JIT-unwind bug by switching `Reenter()` to
   `setjmp`/`longjmp` (`project_xenia_ae_halo3_reentry_fix`), so unwinding may
   no longer need it. **Find where the emitter writes offsets `0x98`/`0xac`.**
   Grep for the prologue emitter in `a64_emitter.cc` - the obvious greps
   (`call_stack`, `0xac`) found nothing, so it is probably a struct offset via
   `offsetof`, not a literal.
2. **`__savegprlr_29` / `__restgprlr_29` = 3.67% combined.** These are PowerPC
   compiler helpers for out-of-line prologue/epilogue (save/restore r29-r31).
   If the backend translates them as real calls instead of inlining 3
   load/stores, that is near-pure overhead with a well-understood fix.
   **Cheapest credible win on the board.**
3. **What is `826DEFD0` in guest terms?** Not yet disassembled as PowerPC. The
   ISO is at `/home/roman/xeniatest/games/Need for Speed - Carbon.iso`.

## Reproducing

```
adb shell setprop debug.canary.perf_map 1     # BEFORE launching
# launch, get into a race
./scripts/jit_profile.sh 20
```
Map lands beside `xe.log` at `.../files/xeniaae/perf-<pid>.map`.
**Turn perf_map off before any FPS benchmark** - it costs a lock + flushed
write per compiled function.

---

## ANSWERED 2026-08-05: the shadow stack is `stackpoints`, and it already has a cvar

Offsets confirmed exactly against `A64BackendContext` (`a64_backend.h`):
`stackpoints` = **0x98**, `current_stackpoint_depth` = **0xac**, matching
`[x19,#0x98]` / `[x19,#0xac]` in the disassembly. `x19` = backend context,
`x20` = guest `PPCContext`.

```cpp
DEFINE_bool(a64_enable_host_guest_stack_synchronization, true,
    "Records entries for guest/host stack mappings at function starts "
    "and checks for reentry at return sites. Has slight performance "
    "impact, but fixes crashes in games that use setjmp/longjmp.", "a64");
```

Each entry is 16 bytes (`host_stack_`, `guest_stack_`, `guest_return_address_`),
capped by `a64_max_stackpoints` (65536 — the `cmp w9, #0x10000` in the dump).

**Both `PushStackpoint()` and `PopStackpoint()` early-out on the cvar**, so
setting it false removes the entire prologue block *and* the post-call depth
verification. **No rebuild needed to test.**

### Why this is a real candidate, not just overhead

It is load-bearing for titles using setjmp/longjmp. But note this project
already replaced JIT unwinding with setjmp/longjmp in `XThread::Reenter()`
(`project_xenia_ae_halo3_reentry_fix`) precisely because DWARF unwind was
unreliable on bionic — so the guest-side longjmp cases and our host-side
reentry are different problems, and NFS Carbon may not need this at all.

The author calls it a "slight performance impact". Measured reality: it is
~15 instructions on **every guest call**, in a workload where guest threads are
57% of process CPU.

### The experiment

```
# global config, [a64] section:
a64_enable_host_guest_stack_synchronization = false
```
Then a **180 s** `fps_bench.sh` run vs `vblank_fixed` (9.67 median).

⚠️ Read at **emit time**, so it must be set before the game compiles code -
set it, then launch fresh.
⚠️ **Halo 3 is the risk case** - re-test it before considering this permanent.
⚠️ If it crashes with a longjmp-using title, that is the cvar doing its job.

---

## TESTED 2026-08-05: disabling stackpoints HANGS NFS Carbon — **do not pursue**

`--a64_enable_host_guest_stack_synchronization=false` (launch arg, verified
applied: `extra launch args: --a64_enable_host_guest_stack_synchronization=false`).

**Result: hard hang at the first load screen.** Not a crash.

| observation | value |
|---|---|
| emulator FPS counter | **0.6 FPS** |
| Odin system counter | 3.0 |
| **Main XThread** | **108% of a core** - spinning |
| **GPU Commands** | **0%** - absent from the busy list entirely |
| errors in `xe.log` | only pre-existing missing XInput exports; **no** assert, no crash, no `Overflowed stackpoints` |

The guest spins at full core while producing **no GPU work at all**. That is the
project's known "hang with zero errors" signature - see
`project_xenia_ae_thread_start_lost_wakeup_fix`.

### What this proves

The stackpoint shadow stack is **load-bearing for NFS Carbon**, not dead debug
weight. Carbon evidently uses setjmp/longjmp (or an equivalent non-local jump),
and without the recorded host/guest stack mappings the recovery path cannot
restore the correct frame, so the guest never makes progress.

The cvar's description is accurate; my framing of it as "overhead" was wrong.
It is ~15 instructions per guest call **that Carbon actually needs**.

### Scoreboard for this hypothesis

- FPS gain: **none - the game does not run**
- Hypothesis "the shadow stack is gateable overhead": **REFUTED for Carbon**
- Cost to find out: one launch-arg toggle, no rebuild, ~5 minutes. This is
  exactly what the toggle discipline is for.

### What remains from the JIT hotspot finding

Still valid and unaffected by this result:

1. **`guest_826DEFD0` = 14.08%, `guest_826DAAF8` = 6.11% of all process CPU.**
   The concentration is real; only the *explanation* (call bookkeeping) is now
   in doubt. Those functions must be disassembled as **PowerPC** to learn what
   the game is actually doing in them.
2. **`__savegprlr_29`/`__restgprlr_29` = 3.67% combined.** Untouched by this
   test - these are PowerPC out-of-line prologue/epilogue helpers, and whether
   the backend inlines them is a separate question. **Now the top candidate.**
3. **`[vdso]` 9.4-11%** (`clock_gettime`) - still completely unexamined.

### Reverted

`debug.canary.extra_args` cleared. No config file was ever modified.
