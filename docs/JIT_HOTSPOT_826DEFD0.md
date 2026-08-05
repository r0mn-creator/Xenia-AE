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
