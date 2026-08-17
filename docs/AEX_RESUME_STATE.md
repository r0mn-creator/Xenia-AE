# AEX — RESUME STATE (single source of truth)

**Read this first. It is written to be enough on its own.**
Last updated 2026-08-17. **Vista still broken, but the search has a named
target now.**

## ⭐⭐⭐⭐⭐ LATEST (doc §50): one hypothesis refuted, next watch needs re-aiming

Captured two more live fields on the existing watch (no new mechanism -
same struct, same site): `ctx328`/`ctx568` (the node-to-node data channel
§49.3 spotted) and `r24`/`r26`/`r28` (§48.3's identified pointer-arithmetic
inputs).

**`ctx328` is a constant `4.0` across 19,292 samples, zero variance** -
it's a fixed handler-selector value, not camera data, and since §49.4
already showed the call sequence itself is baked into the (identical,
same-XEX) guest binary, this can't be the source of a build-to-build
divergence. **The "node channel" hypothesis from §49.3 is retracted.**

**`r24`/`r26`/`r28` came back real and stable** (`0xA5AFE52C`,
`0xA5AFE4C4`, `0x82745EA4`) but with an important caveat found while
writing this up: the watch fires *inside* `82203D10`, not inside
`8212BCE0` - and GPRs are shared context-global state, so these values may
be `82203D10`'s OWN loop pointers (§47.2: it's its own generic
array-iteration routine), not the `8212BCE0` pre-call values §48.3
described. **Next watch needs to fire inside `8212BCE0` itself**, right
after its `fsub d4,d4,d5` (already located in 48.3), to get the actual
inputs unambiguously - see doc §50.3.

## OLDER (doc §49): named `8212BCE0`'s caller - it's a generic dispatch loop

Extended the caller-aware watch (§48.1) one frame further
(`grandcaller_guest_addr`, `stackpoints[current_stackpoint_depth-2]` - same
array, same mechanism, no new infra). All 28,736 hits on the known call
site resolved to **one constant caller**, `guest_821A8FF8`. Verified
directly in its disassembly (not inferred from address proximity - first
attempt had an off-by-20-pages arithmetic bug in the manual `/proc/pid/mem`
read that silently produced a bogus-but-plausible-looking disassembly;
caught it because the output didn't start with the standard function
prologue, redid it correctly): line 1129 builds the exact watched return
address, immediately followed by the resolver-call sequence targeting
`0x8212BCE0` - a direct, byte-confirmed call site.

**The surrounding code is a dispatch loop, not camera logic**: at least six
near-identical call sites in one window, each loading the same context
slots, building a *different* guest target address via the same
resolver-load pattern, and calling it - the shape of a loop walking a table
of function pointers/node handlers, not hand-written camera code. Combined
with `8212BCE0` initializing its own loop counters internally (§45/48.3),
this whole layer looks like a **generic node/element evaluator system**,
with the camera-specific behavior living entirely in the DATA it processes,
not in any function found so far. See doc §49.4 for next steps - tracing
one more caller level, or comparing the node/handler table contents
directly instead of continuing to chase individual pointers.

## ⭐⭐⭐⭐⭐ LATEST (doc §48): found the EXACT bug value live, in a named call chain - and it's a DATA bug, not a JIT bug

Watched a live process write `0xBF7E5FB4` - the *exact* raw float from
this investigation's original bug report (`0xBF7E5FB4` vs XenDroid's
`0x3F7E5FB4`) - and traced its full producing call chain:
`guest_8212BCE0` (at guest address `0x8212BDC4`) calls `guest_82203D10`,
which is where the value gets written. Found this using a NEW tool: read
Xenia's own existing stack-unwind array
(`A64BackendContext::stackpoints`, the same one
`A64Backend::PopulatePseudoStacktrace` uses for real backtraces) from
inside the inline JIT watch, giving a real caller instead of a possibly-
stale `guest_lr`.

**Traced every step of the computation feeding that call - all of it is
byte-for-byte identical between AEX and XenDroid**: the two pointers
providing the delta-vector components (same context offsets, same
addition order), the `fsub` that subtracts them (same registers, no
operand swap), and the call itself. Combined with §47's proof that
`guest_82205690`'s core computation also matches exactly, **five
independent pieces of arithmetic across this whole pipeline are now
proven identical between builds.**

**Conclusion: this is not a JIT miscompilation. It's a data problem.** If
the code is proven identical and still produces a different result, the
GUEST MEMORY it reads (a dynamic position/vector value, not the static
game constant §46.4 already checked and confirmed identical) must already
differ by the time this code runs. The search should now trace **where
that memory gets WRITTEN**, not scrutinize more arithmetic - see doc
§48.5 for the concrete next step (one more level of the same caller-aware
watch, applied to `8212BCE0` itself, to find who calls IT and sets up its
inputs).

## OLDER: every FP computation checked so far is CORRECT - the JIT isn't miscompiling the arithmetic

Doc §47. **Correction to §46**: the top `guest_lr` hits that named
`guest_82177870` (`0x82178360`/`0x8216A70C`) turned out to be a constant
`-0.5` (coincidentally matches the sign+exponent filter) spamming the
ranking - not camera data. **Check the VALUE distribution behind a
`guest_lr`, not just its hit count, before trusting it as signal** - a
hot constant will always outrank a real but rarer varying value.

Re-ranked by variance instead of frequency and found two cleaner
candidates: `guest_82205690` (vector length/normalize - `fmul`, `fmadd`,
`fnmsub`, `fdiv`, `fsqrt`) and `guest_82203D10` (much larger, has the
first `fneg`/`fabs` this investigation has seen). Went past opcode-count
comparison this time and checked the actual instructions and their
register operands directly against XenDroid:

* **`82205690`: PROVABLY IDENTICAL core computation.** The exact
  `fnmsub`/`fmadd`/`fsqrt` sequence - same registers, same order, same
  `PPCContext` offsets for both inputs and outputs - matches between AEX
  and XenDroid instruction-for-instruction. Not "probably the same
  algorithm" - the same registers were chosen by both compilers.
* **`82203D10`: one real, large, precisely-localized divergence found**
  (XenDroid does 15 more multiply/multiply-subtract instructions after a
  shared loop-dispatch point) but tracing it revealed the function is a
  generic array-iteration routine (968-byte element stride - bones/lights
  shaped, not camera-specific), and the actual watched address
  (`0x82203D20`) is nowhere near this divergence - it's the return point
  of a nested call 4 guest instructions into the function. Not confirmed
  as the bug.

**Every function examined this session — `825AD9F0`, `82177870`,
`82205690`, and most of `82203D10` — translates its floating-point
arithmetic correctly and identically to XenDroid.** This rules out "the
JIT miscompiles the arithmetic" as the explanation for any function found
via convergent evidence so far, and redirects the search toward an INPUT
that differs before this correctly-computed math runs, or toward a
genuinely new, not-yet-examined function. Doc §47.5 has four concrete
next steps.

## OLDER: `825AD9F0` is a DEAD END - ruled out, not confirmed

Doc `docs/HALO3_VISTA_46_VS_64.md` §45 (supersedes §44's tentative
conclusion - §44 named `guest_825AD9F0` as "the writer" from address-only
evidence; §45 verified it byte-for-byte and that verification **failed**).

**Two real bugs found and fixed this round** (see §45.1, §45.5 for detail -
both are about `PPCContext` field offsets, found the same way: a stale
comment or a cross-tree assumption trusted instead of a compiled
`offsetof`):
1. The camwatch tool's `guest_lr` read a **hardcoded offset 0x10** for `lr`,
   copied from a stale comment in `ppc_context.h`. The real offset is
   **304** (verified via a compiled `offsetof` probe, now guarded by a
   `static_assert` so this can't silently drift again). Every `guest_lr`
   value §44 reported was wrong (always read as 0).
2. AEX's `PPCContext` has an extra field (`reserved_val`) that XenDroid's
   does not, shifting every field from `thread_state` onward by **+8
   bytes** between the two trees. Not itself a bug, but the exact failure
   mode that made bug 1 possible - flagged so it isn't hit a third time.

**With the LR bug fixed**, exact-byte watching (not just page-level) was
tried three different ways (§45.3) and **never once caught a write to the
tracked byte** - only to other fields sharing its 4 KB page. Root cause:
this class of guest RAM buffer looks single-use (written once, read once,
then abandoned or reused for something else), so "watch an address after
CAMWRITE already told you about it" cannot work reliably for it. **Do not
retry address-recurrence watching on this buffer - it has now failed for
the same underlying reason three times.**

Disassembling `guest_825AD9F0` (the address §44 named) turned up **zero
floating-point or vector instructions** across 393 instructions - it moves
already-computed 32-bit words, it does not compute anything, so it cannot
be flipping a float's sign regardless of what caused §44's watch to land on
it. Comparing its JIT output against XenDroid's found a real 27%-larger
translation on their side - fully explained by `guest_scheduler` being off
in AEX's test config (`PreemptCheckInjectionPass` no-ops without it), not a
miscompilation.

**Net: the writer of the camera constant is still unidentified**, but §46
(same day, later) found a much stronger candidate.

## ⭐⭐⭐⭐ NEWER: `guest_82177870` - real FP work, convergent evidence, still not proven

Doc §46. Built §45.6's idea 2: an inline, **value-based** (not address-
based) watch baked into every JIT-compiled 32-bit store
(`debug.canary.jit_store_watch`, must be set before launch) - checks the
store's VALUE against a sign+exponent bracket matching every mirrored
camera sample seen so far, with no function call (only scratch registers
the allocator never assigns to guest values are touched, so it can't
corrupt a live register). This sidesteps §45.3's single-use-buffer dead end
entirely - no more need to catch a specific address being rewritten.

First cut drowned in 594,997 hits from an unrelated math/audio library
(`guest_addr=0x400F41xx`, far below any game heap). Fixed with one more
compare: skip unless `guest_addr >= 0xA0000000` (the physical-alias range).
Filtered run: 285,433 hits, top two `guest_lr` values `0x82178360`
(69,744×) and `0x8216A70C` (55,283×) - **both of which had already turned
up independently in §45's exact-byte-watch runs**, a different technique
converging on the same addresses.

`0x82178360` resolves to **`guest_82177870`**. Unlike `825AD9F0`, it has
**genuine FP instructions** (44 `fmov`, 28 `fcvt`, 12 `fcmp`, 4 `scvtf`, 2
`fadd`, 2 `fsub`) - the first real float-computing candidate this
investigation has found. Compared against XenDroid: much closer in size
(4527 vs 4490 instructions, ~0.8% apart, vs. `825AD9F0`'s 27% gap) with FP
op counts matching exactly. Found one concrete, real divergence - XenDroid
adds NaN-payload-preservation logic around the first `fcvt` that AEX
lacks - but it provably collapses to a no-op for non-NaN values, so it's a
genuine separate JIT bug, **not** this one.

**All three FP-instruction clusters in `82177870` are now checked** (§46.5,
§46.6) and each has a benign explanation - a second NaN-propagation fixup
(for `fadd`, same shape as the first, structurally identical between trees
just with inverted branch polarity), and a large register-restore
difference at the function's exit that looked serious at first (XenDroid
restores all 18 PPC non-volatile GPRs + lr before its tail-dispatch, AEX
restores none) but cross-checks against **already-completed** testing -
§39.6/§42.1 ran AEX with `disable_context_promotion=true` (which would
force the always-memory-backed behavior this difference implies AEX
already has) and the vista stayed broken, bit-identical. Ruled out.

**▶️ NEXT (doc §46.7):** `82177870` is still the strongest candidate, but
the FP-cluster-only search is exhausted. Try diffing the NON-FP bulk of the
function (constant/register marshalling could still carry the bug), find
what CALLS `82177870` and with what arguments (convergent evidence names it
as involved, not necessarily as the place the value first goes wrong), or
narrow the JIT store-watch's value bracket to one specific sample's
mantissa to cut through the same-magnitude noise.

## ⭐ WHERE WE ARE RIGHT NOW (start here)

**Goal: a correct Halo 3 menu vista on Canary AEX.** It is the cheap proxy for
the character "ball" — the user's model is that a fixed vista very likely means
the ball is fixed too, and an upside-down vista means the ball is still there.
The data supports a shared root cause: the two builds run **bit-identical for
32 constant states and then diverge**, after which camera sign, draw counts and
bone-matrix states all differ.

**Status: the vista is STILL UPSIDE DOWN.** Not fixed.

### The one-command test

```
adb -s 3a478943 shell setprop debug.canary.vsconst_states 1
# launch Halo 3, reach the main menu (~2 taps on the tile, see "How to test")
adb -s 3a478943 shell "su -c \"grep -a VSCONSTSTATE <log>\"" > out.txt
python3 scripts/vista_bisect/verdict.py out.txt
```
* AEX baseline: **BROKE, 221 negative** · XDtester: **CORRECT, ~220 positive**
* Read the verdict off the **c3.x sign histogram, never the screenshot** — the
  menu camera pans, so two shots of one build differ while the statistic does not.

### What is PROVEN about the bug

1. **It is a COMPUTATION bug, not ordering** (doc §43). `debug.canary.camwrite`
   logs what the guest WRITES: **POSITIVE=0, NEGATIVE=235489**. The correct
   camera is never produced, so no draw/constant interleaving fix can help.
   (Ordering is still a live mechanism for the BALL — separate measurement, §29.)
2. **The difference is EXACTLY the sign bit** (§43.5). AEX writes `0xBF7E5FB4`,
   XenDroid `0x3F7E5FB4`. Identical mantissa and exponent. That rules out a
   different formula, a different input, a precision difference, and a byte-swap
   error.
3. **Minimal repro** (§40): both builds are bit-identical for constant states
   1–32; both reach their first real camera on the SAME shader
   `E05650CA89E232AF`; XD at n=33 with **+2.49331 / +0.99365**, AEX at n=36 with
   **−2.49313 / −0.99383**.

### ⛔ DO NOT REPEAT — full ledger in doc §42

**Tried on AEX, none fixed it:** `dcbz` 32→128 · `saverest_fast=0` ·
EVENT_WRITE_ZPD fix · `disable_context_promotion` · `validate_hir` (no errors) ·
`readback_resolve=uma` · `memexport_no_store` · the **movi64** fix.
**Tried on XDtester to BREAK it, none did:** CPU/vector group (incl.
`a64_vmx_nan_fixup`, `context_promote_vec128`, `inline_leaf_calls`) ·
`occlusion_query=fake`+`readback_resolve=none` · `memexport_enable` ·
shared-memory trio · **33 cvars at once**.
⇒ **No cvar in either build explains it. The cause is un-gated code.**
**Eliminated by inspection:** kernel-call stream · all `Vd*`/`XGetVideoMode`
args · reported memory/display mode · `ppc_emit_fpu`/`ppc_emit_alu`
(byte-identical) · all sign-capable AltiVec ops · `LOAD_ALU_CONSTANT` ·
`MemorySequenceCombinationPass` · `PPCContext` layout.
**Tooling gotcha (§44.2):** `Memory::RegisterPhysicalMemoryInvalidationCallback`
is GLOBAL, not scoped to the page you armed — an unfiltered callback fires for
every page any subsystem invalidates and floods 7700+ hits in ~2s. Always
filter on the armed page(s) inside the callback.
**Tried and failed this round (§45):** watching a guest RAM address AFTER
`CAMWRITE` already told you about it, to catch a repeat write and read its
guest LR — tried three ways (self-re-arm, 16-slot recurrence gate, 256-slot
recurrence gate), all correctly implemented, all failed the same way: the
tracked byte is never written again within any practical test window (up to
4 minutes tried). The buffer class looks single-use. **`guest_825AD9F0`**,
named by this technique in §44, disassembles to zero FP/vector instructions
— it cannot be the sign-flip source; do not re-chase it. Comparing its JIT
output against XenDroid's (27% larger there) is fully explained by
`guest_scheduler` being off in AEX's test config — not evidence of anything;
don't resurrect without first matching that cvar between builds.
**Read stale offset comments in `ppc_context.h` with suspicion** — `lr`'s
comment says `// 0x10`, the real offset is 304 (verified via compiled
`offsetof`, guarded by `static_assert` in `memory.cc` now). AEX's
`PPCContext` also has an extra `reserved_val` field XenDroid's lacks,
shifting everything after it by +8 bytes between the two trees.

### ▶️ NEXT STEP — `825AD9F0` ruled out, restart the search

~~Locate the PPC function that computes it~~ ~~Found: `guest_825AD9F0`~~ —
**§44's identification did not survive verification.** See the LATEST
section above and doc §45. `825AD9F0` has zero FP/vector instructions (it
moves words, doesn't compute), and exact-byte watching (not just page-level)
never once caught a write to the tracked byte across three redesigns — the
address-only evidence that named it in §44 was catching some OTHER field on
the same shared page, not the camera constant.

**Two untested ideas for the actual next step (doc §45.6):**
1. Scan the `debug.canary.perf_map` symbol table for guest functions with
   FP/vector-heavy bodies (same `llvm-mc -triple=aarch64 -disassemble`
   technique used to check `825AD9F0`) instead of continuing to chase
   addresses pulled off this one buffer.
2. Instrument the JIT's store-emission path directly (inside the a64
   backend) to check a target guest address inline and log the *live*
   guest LR at that moment — sidesteps the single-use-buffer problem
   entirely, at the cost of touching the JIT backend instead of staying in
   diagnostic-only code.

⚠️ Two real bugs were found and fixed while chasing `825AD9F0` — both worth
carrying forward even though the function itself was a dead end:
`PPCContext::lr`'s real offset is **304**, not the `0x10` a stale header
comment claims (now guarded by a `static_assert`); and AEX's `PPCContext`
has an extra `reserved_val` field XenDroid's lacks, shifting every field
after it by +8 bytes between the two trees. Any code — including future
diagnostics — that reads `PPCContext` by a hardcoded numeric offset instead
of the real struct is suspect.

### Real bugs FIXED this session (none fix the vista; all need regression runs)

| commit | fix | ⚠️ |
|---|---|---|
| `3d461e5e5` | `dcbz` cleared 32 bytes, not 128 | JIT semantics, all titles |
| `5e3d6cef4` | EVENT_WRITE_ZPD raw address + A-and-B sentinel | all titles |
| `a17963228` | `movi` 2D got a pre-compressed imm8 (all-ones mask was wrong) | **visible: vista animation "fast-forwards"**; toggle `debug.canary.movi64_fix=0` |

**Re-test NFS Carbon before any of these reach `canary-ae` or `main`.**

### New tooling

* `scripts/vista_bisect/{xd_bisect.sh,verdict.py}` — flip XDtester cvars, run,
  read the verdict. ⚠️ String cvars must stay **quoted** (bare breaks TOML so the
  game never starts) and a missed tile tap leaves you on the library screen; the
  script retries and confirms via VSCONSTSTATE.
* Probes (all default OFF): `debug.canary.vsconst_states`, `camwrite`,
  `dump_row_marker` (also ported into XDtester), `memexport_no_store`.
* Toggle: `debug.canary.movi64_fix` (default ON).

---

## The goal

Halo 3's menu vista renders **upside down** in Canary AE and Canary AEX.
Fixing it is the objective. The character models are also collapsed to a "ball";
the two are linked (user's call: *"if the vista is upside down, there's a very
good chance the character is a ball"*). **The vista is the cheap proxy** — it
renders at the main menu ~45 s from launch; the ball needs a full in-game run.

## The three builds

| build | applicationId | role |
|---|---|---|
| Xenia AE | `org.xeniaae` | the real emulator, what ships |
| Canary AE | `org.xeniaae.canary` | test line, **kept working**, the fallback |
| **Canary AEX** | `org.xeniaae.aex` | branch `canary-aex`, the scheduler overhaul |

All three install side by side on the Odin 2 (`adb -s 3a478943`).

## THE CENTRAL FINDING — the renderer is not the bug

Measured symmetrically against XDtester (instrumented XenDroid) on the same
device, same game, same driver:

| | Canary AE | XDtester |
|---|---|---|
| draws/frame entering `IssueDraw` | **88–138** | **611–641** |
| total PM4 packets/frame | 5,251 | 15,692 |
| FPS | ~15 | ~24 |
| distinct bone-matrix states seen by draws | saturates ~130 | >512 (probe cap) |

* PM4 parsing is **provably lossless** (`PM4DRAW == DRAWENTRY` exactly), so the
  draws are **never submitted** — not dropped by us.
* AE is **~5× slower per unit of guest work** (15 FPS doing a third of the
  command work XenDroid does at 24 FPS).
* Guest behaviour is **non-deterministic run to run**; XenDroid is stable.
* Every GPU stage — viewport maths, NDC, translated SPIR-V, resolve, dump,
  transfer, guest constants (29/37 shaders byte-identical, view-basis
  determinant +1 in both) — measured **identical**.

**Root-cause hypothesis (unproven):** XenDroid runs guest threads as
**cooperative fibers** (`guest_scheduler=true`, 1 ms quantum); AE runs 1:1 host
threads. That is the one architectural difference explaining the numbers.

## WHERE THE WORK STANDS — the blocker in one paragraph

Steps 1–4 are ported and build. With `guest_scheduler=true`, the scheduler
starts, fibers are created, `preempt_requested=1` is raised — **but no JIT
compilation happens at all** (not even `ControlFlowAnalysisPass` logs, which
fires normally with the scheduler off). Watchdog shows
`Running tid=6 'Main XThread' lr=00000000`. **The main fiber is stuck in HOST
code before executing a single guest instruction.** Safepoints exist only inside
JIT'd guest code, so they cannot preempt it.

## ▶️ NEXT ACTION

**Two fixes just landed** (`XThread::Delay` host-slept, blocking the whole
dispatch thread; `XThread::Execute` deref'd `thread_->system_id()` which is NULL
on the fiber path). Guest code now executes under the scheduler.

Current state with `guest_scheduler=true`:

```
CPU 0 running tid=7 'MAIN_THREAD' lr=82589EC4   <- real guest code
       ready  tid=6 'Main XThread' lr=8219832C
       blocked tid=8 'ASYNC_IO' on semaphore wait=single[1] gated=1
CPU 1-5 idle
```

**The cooperative WAIT path is proven working** — ASYNC_IO parks correctly on a
semaphore. JIT compilation runs. But **CPU 0 never switches**: MAIN_THREAD holds
it with `preempt_requested=1` while tid=6 sits ready *on the same CPU* and CPUs
1–5 idle. So the injected safepoints are not preempting.

### Verified since (narrowing the preemption failure)

* **Safepoints ARE emitted.** `PREEMPTINJECT functions=1 checks_emitted=4 ...
  functions=5 checks_emitted=9` - the pass runs and injects at loop heads.
* **The context register is correct.** `EmitPreemptCheck` reads the flag off
  `x20`; both this fork and XenDroid document "Context register = x20"
  (`a64_emitter.h:128`). Not a register-convention bug.
* **The flag is raised** - watchdog shows `preempt_requested=1`, `irql=0`.

So: checks exist, the register is right, the flag is set - and the fiber still
never yields.

### ⭐ HYPOTHESIS 1 ELIMINATED, HYPOTHESIS 2 CONFIRMED

Instrumented `PreemptCurrentFiber` (guest_scheduler.cc:78):

* It **IS entered** — `PREEMPTHANDLER entry #0..#5`.
* It **does NOT decline** — no "holds global critical region" message ever.
* But it is entered **only ~6 times total**, then never again, while the
  watchdog keeps showing `MAIN_THREAD ... lr=82589EC4 preempt_requested=1`.

So the mechanism works end to end — safepoint fires, handler runs, no defer —
but **the hot loop the guest is actually spinning in carries no check**. Early
functions got checks (`checks_emitted` 4→9 over 5 functions); the loop at
`lr=82589EC4` did not.

### ⭐⭐ DECISIVE: the stuck fiber is NOT in guest code

Added `preempt_check_every_block` (cvar, default false) which injects a
safepoint into **every HIR block**, not just detected loop heads — the blunt
test of "the hot loop has no check".

**Result: no change whatsoever.** Same watchdog, same address:
`MAIN_THREAD lr=82589EC4 preempt_requested=1`, tid=6 ready on the same CPU,
CPUs 1–5 idle.

If the fiber were executing guest code, a check in every block would fire
within microseconds. It does not. Therefore:

**tid=7 MAIN_THREAD is blocked inside a HOST function called from guest code.**
`lr=82589EC4` is stale — it is the last guest call site before entering the
host, not where the thread is. Safepoints exist only in JIT'd guest code and can
never preempt this.

### ⭐⭐⭐ FOUND (2026-08-14): the unrouted wait is the guest SPINLOCK

`xeKeKfAcquireSpinLock` (`xboxkrnl_threading.cc`) retried its CAS with
`xe::threading::Sleep(0)` / `MaybeYield()` — **host-thread** yields. Under the
fiber scheduler that sleeps the *dispatch thread the lock holder is queued on*,
so the holder can never run and the CAS never succeeds.

Every observation fits: the loop is host C++ (so a safepoint in every HIR block
correctly changed nothing), `lr=82589EC4` is the stale guest call site that
called `KfAcquireSpinLock`, and tid=6 ready **on the same CPU** is the starved
holder. Found by source-diffing against XenDroid with no device attached.

**Root cause class: the scheduler ENGINE was fully ported (`guest_scheduler.h`
is byte-identical to XenDroid's) but its CALL SITES were not.**

Fixed in `2faeb88b0` along with the rest of the missing call sites:
`xboxkrnl_threading` (spinlock/NtYieldExecution/APC wake), `xfile` (5×
`RunBlockingHostCall`), `xiocompletion`, `a64_seq_memory`, XMA, xam UI/NUI,
the `CooperativeWaiterFifo` + Begin/End overrides on XEvent/XSemaphore/XMutant,
the `cooperative_pulse_epoch` lost-wakeup fix and `alertable` pass-through in
`XObject::Wait`, and the **completely missing fiber paths in `XThread::Exit`
and `XThread::Terminate`** (they would have killed the shared dispatch thread).

⚠️ **Untested on device** — the Odin 2 was not connected. `guest_scheduler`
still defaults false, so the change is inert until the cvar is flipped.

**Known gaps, deliberate:** `xsocket.cc` (XenDroid rewrote it on asio, 956 vs
our 370 lines; no networking in Halo 3 offline) and their
`WaitEnter`/`AcquireStatus`/`GetWaitHandleForCurrentThread` wait-system
refactor (touches the host-thread path too).

### ▶️ NEXT ACTION — retest on the Odin with `guest_scheduler=true`

Install, launch Halo 3, and read the watchdog: does CPU 0 **switch**, and do
CPUs 1–5 pick up threads? Then the vista at the main menu (~45 s), then the
PM4 probes vs XDtester.

If it still wedges, the remaining unrouted-wait candidates below are unchanged:

⚠️ `log_all_kernel_calls = true` produces **no output** - it is gated behind
`logging::ShouldLog(LogLevel::Debug)`, and the shipped log level is Info. Either
raise the log level or use one of the probes below instead. (Tried; do not
repeat.)

1. **Disassemble/identify guest `0x82589EC4`**1. **Disassemble/identify guest `0x82589EC4`** and see which import it calls —
   that names the export directly. (`xe::cpu` has a disassembler; or grep the
   log's import table dump for the nearest address.)
2. Or **probe the host blocking primitives**: log on entry/exit of
   `xe::threading::Wait`, `WaitAny`, `WaitAll`, `Sleep`, `AlertableSleep` and
   any `Fence::Wait`, run with the scheduler on, and see which call the main
   fiber enters and never leaves.
3. Strong suspects: `XIoCompletion`/`XFile` I/O waits, `XamContent*` /
   `XamUserRead*` startup calls, and any `Fence` wait inside the audio or
   content setup.

Then route that path through `CooperativeWait` the same way
`XObject::Wait` was.

Also still open: **all threads land on CPU 0** while CPUs 1–5 idle
(`DispatchCpuOf`). This may well have been a *symptom* of the spinlock wedge —
recheck it after the retest before investigating separately.

## How to test (exact, these cost hours to learn)

```bash
# build + install
cd /home/roman/Android/Xenia-AE
JAVA_HOME=/opt/android-studio/jbr ./gradlew :app:assembleDebug
adb -s 3a478943 install -r app/build/outputs/apk/debug/app-debug.apk

# launch Halo 3 (library tile = TWO taps; first only selects)
adb -s 3a478943 shell "monkey -p org.xeniaae.aex -c android.intent.category.LAUNCHER 1"
sleep 9; adb -s 3a478943 shell input tap 489 530; sleep 4
adb -s 3a478943 shell input tap 489 530; sleep 55
adb -s 3a478943 exec-out screencap -p > /tmp/shot.png

# logs / config (NEVER adb-push app files; use su + preserve ownership)
L=/storage/emulated/0/Android/data/org.xeniaae.aex/files/xeniaae/xe.log
P=/storage/emulated/0/Android/data/org.xeniaae.aex/files/xeniaae/xenia-canary.config.toml
```

* **In-game** (for the ball) needs a HELD gamepad press:
  `adb shell input keyevent --longpress 96`, `sleep 3`, ×5. Taps and instant
  keyevents do NOT work. **Halo 3's level intro is a pre-rendered VIDEO** — its
  frames contain no engine models and cannot judge the ball.
* Delete `cache/pipelines_<TITLEID>.bin` before any GPU test.
* **"BUILD SUCCESSFUL" ≠ code in the binary** — verify with
  `strings libe.so | grep <PROBE>`, and confirm the *installed* APK, not the
  build output. A stale APK caused a wrong conclusion today.

## Probes available (all default OFF, same names in XDtester → logs diff 1:1)

`debug.canary.` + `pm4total` `pm4draw` `drawentry` `bonedistinct` `vsconst`
`regtrace` `ndcy_draw` `resolve_row_marker` `halo3_vista_probe` `vista_rt_base`
`shared_memory_host_visible` `resolve_aligned_storage`
`memexport_bypass_validation`

**Success criterion:** AEX draws/frame and packets/frame approach XenDroid's
(~611–641), and run-to-run variance collapses.

## Landed on canary-aex

Transplants: memexport eA validation; host-visible shared memory +
`ReadbackResolveMode::kUma` (**user-confirmed image improvement**); memexport
page tracking + fence/coherency awaits; MemoryPollPark/DelayCountdownCollapse
passes (**default OFF — they regress without preemption**); the full fiber
scheduler (steps 1–4).

**Real bug fixes** (cherry-pick candidates for `canary-ae` / `main`):
1. `vulkan_sparse_shared_memory` forced off with a custom driver — **fixes a
   black screen hitting every NEW install with a custom Turnip driver**.
2. `gamma_render_target_as_srgb_` was never assigned on `kHostRenderTargets` —
   the cvar was wired to nothing.
3. `PPCContext` scheduler fields **appended, not inserted** — the JIT addresses
   that struct by offset off x20; inserting shifted every later field.
4. `WakeCooperativeWaiters` / `RecordCooperativeSignal` crashes:
   `XThread::GetCurrentThread()` **asserts** on non-guest threads (guard with
   `IsInThread()`), and `XObject::handle()` is `handles_[0]` with **no bounds
   check**.

## Hard-won rules — violating these has already cost hours

* Guard cooperative paths on **`GuestScheduler::enabled()`**, never on the
  pointer — `KernelState` constructs the scheduler unconditionally.
* **Sample the signal epoch BEFORE polling** in `CooperativeWait`, or a signal
  landing after a failed poll is lost and the fiber never wakes.
* Landing gated code incrementally is **safe**; *enabling* a partially-ported
  coupled unit **wedges silently**. Do not conflate these (I did, twice).
* Watchdog `last_safepoint` is **always 0** here — `log_safepoint_pc` was not
  ported. **Not evidence.**
* When two builds of the same code differ, **diff their configs before
  suspecting code** — a fresh config differs from Canary AE's 2020-era one on 12
  cvars, and one of them was the whole "AEX regression".

## Retracted — do not resurrect

"46-vs-64 / surface_pitch" as the vista cause · "the composite doesn't read
guest RAM" · "the cause is driver-side" · "gamma=true improves the vista" (that
was the pipeline-cache deletion) · "AE's Vulkan backend has no readback path" ·
"the injection pass never runs" (stale APK) · "we win on face stability".

## Reference trees

`/home/roman/xeniatest/xendroid-git` (XenDroid + XDtester probes, branch
`xdtester`, package `xendroid.compose.xdtester.debug`) ·
`/home/roman/xeniatest/canary-git` (closest upstream base, best for
`diff | grep '^-'` to find AE deletions) · `/home/roman/xeniatest/oracle`
(newer upstream canary).

Deeper history: `docs/AEX_OVERHAUL.md`, `docs/HALO3_VISTA_46_VS_64.md` (s14–s35).
