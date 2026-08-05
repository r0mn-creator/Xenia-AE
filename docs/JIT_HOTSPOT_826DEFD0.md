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

---

## TESTED 2026-08-05: save/restore helper fix — SAFE but UNMEASURABLE

`debug.canary.saverest_fast` (default ON). Skips stackpoint bookkeeping for
kProlog/kEpilog/kEpilogReturn leaf helpers.

| run | median | n |
|---|---|---|
| `vblank_fixed` (baseline) | 9.67 | 167 |
| `saverest` | **9.69** | 171 |

**+0.2%, p=0.78 — NOT significant.** Game stable, 0 asserts, no hang.

**This was predicted before the run:** the helpers total 3.67% of CPU and this
removes only part of that, which is below the harness's ~10% detection floor.
Prediction recorded in advance and confirmed — the measurement is working
correctly, the change is simply too small to see.

**Kept** (safe, removes real work, toggleable) but it must **not** be counted as
progress toward 30 FPS.

### The strategic conclusion this forces

We are now in the regime where **individual micro-optimisations are
unmeasurable**. 9.67 -> 30 FPS is a **3.1x** gain. Everything identified so far:

| candidate | ceiling | verdict |
|---|---|---|
| PM4 translation (whole GPU cmd thread) | 1.33x if made *infinitely fast* | insufficient alone |
| stackpoints | — | **REFUTED** - load-bearing, hangs Carbon |
| save/restore helpers | 3.67% | **tested, unmeasurable** |
| `[vdso]` / `clock_gettime` | ~10% | **unexamined - best remaining** |
| descriptors / RT+texture cache | ~5-15% | untested |
| resolution | — | **refuted** - GPU is not the constraint |

Stacking every remaining candidate optimistically gives maybe 1.4x -> ~13 FPS.
**3x is not reachable by accumulating these.** It requires either a structural
change (command-buffer caching / replay, JIT code-quality work) or accepting
that this title on this hardware lands in the low-to-mid teens.

**Next, in order:**
1. **`[vdso]` ~10%** - largest single unexamined cost, and it is plumbing, not
   emulation. Cheapest remaining shot.
2. **Disassemble `826DEFD0` as PowerPC** - 14% of CPU and we still do not know
   what it *does*. Could be a game hot loop (nothing to win) or a pathological
   translation (large win). This is the highest-variance unknown.

---

## FULL BODY ANALYSIS 2026-08-05 — it contains a guest SPIN-WAIT

Earlier only the first ~40 of 357 instructions (the prologue) were examined,
which is where the stackpoint code was found. The **body** tells a different
story and **weakens the call-overhead theory**.

| metric | value |
|---|---|
| total instructions | 357 |
| **calls (`blr`/`bl`)** | **4** |
| loads | 53 (14%) |
| stores | 59 (16%) |
| **`mov`** | **105 (29%)** |
| `movk` | 23 |
| `rev` (endian swap) | 16 |
| **`yield`** | **8** |

### 1. Call overhead is NOT the story here

Only **4 calls in 357 instructions**. At ~15 instructions of stackpoint
bookkeeping per call that is ~60 instructions of a 357-instruction function -
real, but nowhere near enough to explain 14% of total CPU. **The stackpoint
theory was wrong twice**: once refuted by the hang test, and again here on the
numbers.

### 2. 29% of the function is `mov` (36% counting `movk`)

105 `mov` + 23 `movk` = 128 of 357 instructions doing nothing but shuffling
registers and materialising constants. That is a strong smell of **poor
register allocation / constant rematerialisation** in the a64 backend. Worth
comparing against what the x64 backend emits for the same guest function.

### 3. ⭐ Eight consecutive `yield` instructions — a guest spin-wait

```asm
mov  w17, #0x4000000
str  w17, [x21, x0]      ; write 0x04000000 into guest memory (x21 = membase)
yield ; x8               ; delay / spin hint
ldr  x22, [x20, #0x30]
```

Straight-line, not a loop body - this is a **delay primitive** (the PPC
`db16cyc`-style pause used in Xbox 360 spin-wait code), emitted right after
writing a flag to guest memory.

**This reframes the whole finding.** If `826DEFD0` is a lock acquire or a
polling loop, then a large part of its 14% is the guest **waiting, not
computing** - and CPU is burned either way. That is not a JIT code-quality
problem and no amount of backend tuning fixes it.

It also rhymes with this project's history: `project_xenia_ae_thread_start_lost_wakeup_fix`,
`project_xenia_session14_cs_fix`, and the NFS level-load stall were all
"guest spins forever waiting for something we never delivered".

### Next (highest value first)

1. **Determine what it polls.** Identify the guest address written
   (`0x04000000` looks like a flag/handle value, not data) and what the loop
   condition reads. If it is waiting on another guest thread, on an emulated
   device register, or on a kernel object, the fix is on our side.
2. **Confirm loop vs straight-line** by checking whether `826DEFD0` is called
   repeatedly from a caller loop, or contains its own backward branch.
3. **Compare the `mov` density against the x64 backend** for the same function
   to judge whether the a64 register allocator is materially worse.

---

## ⭐ #1 ANSWERED: it is a guest POLLING LOOP on a status bit

Decoded the body. `[x20,#0x30]` is guest **r1** (stack pointer) - confirmed
against `PushStackpoint`, which uses `offsetof(PPCContext, r[1])`.

### Structure

```asm
; --- standard PPC frame setup: stwu r1, -0x80(r1) ---
ldr  x22, [x20,#0x30]      ; r1
sub  x23, x22, #0x80
rev/str                     ; link old SP at [new SP]
str  x23, [x20,#0x30]      ; r1 = new SP

ldr  x22, [x20,#0x40]      ; r3 = a pointer argument
ldr  w22, [x21, x0]        ; deref it
str  x22, [x20,#0x110]     ; keep the loaded pointer/handle

; --- delay counter on the stack ---
mov  w17, #0x4000000
str  w17, [x21, SP+0x50]   ; NOTE: stored WITHOUT rev

loop:                       ; <- 0x400178
  yield x8                  ; pause primitive
  ldr  w23, [x21, SP+0x50]
  rev  w23, w23             ; read back byte-swapped
  sub  x23, x23, #1         ; counter--
  rev/str back
  ldr  w22, [x21, SP+0x50]
  cmp  w22, #0
  cset ...                  ; CR bookkeeping into [x20,#0x18..0x1a]
  cbz  w22, loop            ; loop while counter != 0

; --- then poll a status byte ---
ldr  x22, [x20,#0x110]
add  w0, w22, #0x2a3d
ldrb w22, [x21, x0]        ; load BYTE at (ptr + 0x2a3d)
and  x22, x22, #4          ; test bit 2
```

### The counter is 4, not 67 million

`0x04000000` is stored **without** a byte swap but read back **with** one. The
guest is big-endian, so guest value `4` is bytes `00 00 00 04`, which as a host
little-endian word is `0x04000000`. The compiler folded the swap into the
constant. **The delay is 4 iterations x 8 `yield` = 32 yields**, i.e. a short
`db16cyc`-style pause - not a long stall.

### What this means

`826DEFD0` is a **spin-wait**: short pause, then poll **bit 2 (0x04) of the byte
at `+0x2a3d`** from a pointer passed in **r3**, repeat.

So a large share of its **14% of total CPU is the guest polling, not computing.**
Backend/JIT tuning cannot fix that - the CPU burns whether or not the emitted
code is good. What matters is **how long the guest has to wait**, i.e. whether
*we* are slow to set that bit.

### Next: find who sets `+0x2a3d` bit 2

1. The pointer comes from **r3** (caller-supplied) and is dereferenced first, so
   `+0x2a3d` is an offset into a **guest structure**, not a fixed MMIO address.
   Identify the structure by logging r3 at entry to `826DEFD0`.
2. Determine whether that byte is written by **another guest thread** (pure
   guest-side synchronisation, little we can do) or by **the emulator** on
   behalf of a device / kernel object (a wakeup we may be delivering late -
   which is exactly the class of bug this project has hit repeatedly:
   `project_xenia_ae_thread_start_lost_wakeup_fix`, `project_xenia_session14_cs_fix`).
3. A guest memory **write watch** on `ptr+0x2a3d` is the direct way to answer 2.

If it is (2), this is the first lead in the whole performance investigation with
genuinely unbounded upside.
