# Halo 3 collapse — detective log (test → result)

Running log of experiments with their **results and what each rules in/out**.
Newest session at the top. Companion to `HALO3_MENU_INVESTIGATION.md` (narrative)
and `HALO3_FINDINGS_CHECKLIST.md` (the resolve/flat-navy track, solved).

**Ground rules learned the hard way — read before adding a result:**
1. **Never judge this bug from a screenshot.** Four wrong calls. Only the user's
   live view counts. (Second reason, found 2026-07-29: `adb exec-out screencap`
   returns an all-black frame on some devices because it does not capture the
   emulator's SurfaceView.)
2. **No metric without a reference.** Four times an "alarming" Adreno number
   turned out identical or worse on RADV, which renders correctly.
3. **Rate-limit every probe.** Both the Android harness and the desktop oracle
   have self-DoSed (1.8M lines / 318MB once; the GPU probe alone emits ~9,500
   log lines/sec and makes the game unplayable).
4. **Guest-written values are the same on every platform.** Shader constants and
   loop constants come from the guest CPU; prior work confirmed they are
   bit-identical across platforms with wildly different fill. So an odd-looking
   constant is only a lead if it *differs* from RADV or *varies* when it
   shouldn't.

---

## Session 2026-07-30

### ★ T1 — Is the character-collapse shader the same one as the menu vista's?
**Method:** GPU probe toggled on for a 20s window during Sierra 117 gameplay with
the user visually confirming *"still a ball"* on screen, vs a menu capture.
(`setprop` is live, so the probe can be enabled *after* the level loads — leaving
it on through menus/loading makes the game effectively unplayable.)

**Result — IDENTICAL producer/consumer pair:**

| shader | role | MENU | GAMEPLAY (ball) |
|---|---|---|---|
| **`9EA48FC2B26C325D`** | producer | 25402 | **31640** |
| **`488D9488AB7ED7D8`** | consumer (`eM=0x0`, vtx 4-64 step 4) | 20736 | **27223** |
| `C5E0199746AB8E83` | producer (amplification variant) | 727 | 185 |
| `D3AFBA6827D428C6` | producer | 431 | **0** |
| `74F10091EC03A958` / `E63384A95752C73D` | producer | 0 | 185 each |

Gameplay memexport-writing draws were **96% `9EA48FC2B26C325D`** (3955/4109).

**Rules IN:** the menu vista bug and the in-game collapse are very likely **one
defect** at different geometry granularity.
**⇒ THE MAIN MENU IS NOW THE STANDARD REPRO** — seconds to boot, no controller,
no level load, same shader pair. Run everything here first.
**Rules OUT:** a structurally similar sibling skinning shader drawing characters.
**Rules OUT:** `D3AFBA6827D428C6` as the character shader — it has the most
skinning-*looking* structure (zero `vfetch`, indexed 4-row matrix palette at
`c[16+a0]`..`c[19+a0]`) yet appears **only at the menu, never in gameplay**.
⚠️ Ucode shape is not evidence of what a shader draws. Measure.

### T2 — Do the consumer's per-part transform matrices vary per draw?
Origin: user hypothesis that a character is many parts, so a bad *per-part*
transform piles parts together as a ball, while the single-mesh vista merely looks
flipped. Supported by the user's own earlier observation that an **intact head** is
visible inside the ball — per-part mis-transform relocates parts while leaving each
internally rigid; true vertex collapse would destroy the head.

The consumer uses `c33/c34/c35` as a rotation basis and **`c36` as the translation
column** (ucode instr 66-75, a `mad` chain). The pre-existing `CONSUMER_CONST` dump
was **one-shot**, so its identity sample proved nothing — per-part matrices vary
per draw by definition.

**Method:** new `CONSUMER_MTX` probe (commit `9d5c4d6d`) counts *distinct* matrices
across draws, dumping only the first 8 distinct values. Rate-limited.

**Result:** `draws=10240 distinct_matrices=1 identity_draws=10240 (100%)` —
exactly one matrix, identity with zero translation, every single draw.

**Rules OUT — and identity is BY DESIGN, not a bug:**
- `register_file_->values` is Xenia's shadow of the **guest's own** constant
  writes. If the game ever wrote real per-part matrices there, the probe would see
  them. It never does ⇒ **not a Xenia constant-upload bug.**
- `c33/c34/c35` also appear in `cndeq` with `.zxyy` swizzles against basis vectors
  (instr 163/164/175) — the standard **axis-selection idiom**.
⇒ The per-part transform does **not** live in these constants. It lives in the
**memexport buffer data**. Constants ruled out; the data is the target.

### T3 — Is the consumer's slot divisor wrong?
Consumer computes `slot = floor(vtxIndex / c78.x)`.
**Result:** `c78.x = 4.0`, and the consumer's observed vertex counts are 4-64 in
multiples of 4 — perfectly consistent.
**Rules OUT:** "wrong/saturating slot divisor concentrating writes into low slots."

### ★ T4 — What is the producer's actual loop trip count? (`loop i15`)
Not previously on the eliminated list. `LoopConstant = {count:8, start:8, step:8}`,
`aL = iterator * step + start` (`xenos.h`).

**Result at the menu:** **`l15 = 0x0001000D`** ⇒ **count = 13, start = 0, step = 1.**

⚠️ **CORRECTED 2026-07-30 (T6).** I first hand-decoded this as `start=1, step=0`
and concluded "aL is constant across all 13 iterations". **That was a bad manual
bit-decode.** The real fields are `start=0, step=1`, so `aL = 0,1,2…12` and
`c[144+aL]` **does vary per iteration** (c144..c156). Everything below that was
premised on a constant `aL` is therefore void — the gate is *not* proven
all-or-nothing per draw. Decode bitfields with code, not by eye.

**Rules IN / implications:**
- **`step = 0` ⇒ `aL = 1` for all 13 iterations**, so `c[144+aL]` is *always*
  `c145`. The producer's bail/skip gate
  `floor(c[144+aL].y * c218.y) != 0` therefore evaluates **identical inputs every
  iteration** ⇒ the gate is **all-or-nothing per draw, not per-slot.**
- **Rules OUT:** per-slot gate scatter as the cause of the partial fill. Consistent
  with the observed *clean dense prefix* rather than scattered fill.
- ⚠️ Also corrects an earlier misread of mine: `L129` sits at ucode line 805,
  **before** `endloop` at 817, so `jmp L129` is a **continue**, not a loop exit.
  The gate skips an iteration; it does not terminate the loop.
- ⚠️ Loop constants are guest-written, so `count=13/step=0` is almost certainly
  identical on RADV and is therefore **not yet a fault** — it needs the reference.

### T5 — What do the ~93% unfilled slots actually do? (prior session, re-read)
Recorded in `HALO3_MENU_INVESTIGATION.md`: **`p0 = TRUE`** for them, so they are
**not** skipped — they run the full position/lighting body on **zeroed input** and
land in *"a tight degenerate cluster / off-screen / NaN"*, painting over the sparse
~7% of real vertices.

**This is the ball.** And it explains the long-standing puzzle that fill % (5-14%)
shows **zero visible correlation** with the result: it is not the amount of real
data that decides the look, it is the flood of zero-input vertices that render
regardless. ⚠️ I initially mis-stated this as "unfilled parts go missing" — they do
not, they *render*.

### ★★ T6 — RADV REFERENCE (the standing blocker, now cleared)
Built `~/xeniatest/oracle-probe/` (worktree at `6e9bac0`, Release/RADV) with the
Android probes ported as **cvar-gated modules, default off**
(`--probe_master --probe_consumer_mtx --probe_loop_consts`, tagged `PROBE(...)`).

**Build validated stock first, per the modules rule:** with all probes off the
run produced **0 probe lines, 0 errors**, launched the title, on RADV RENOIR —
i.e. a normal Xenia build, so its numbers are trustworthy.

**Result — Halo 3, RADV (renders CORRECTLY):**
```
PROBE_LOOPCONST l15=0x0001000D count=13 start=0 step=1 (l0=0x00050001 l16=0x00050001)
CONSUMER_MTX distinct#1 draw=1 identity=1 c33=(1,0,0,0) c34=(0,1,0,0) c35=(0,0,1,0) c36=(0,0,0,1)
```

| measurement | Android (Adreno, ball) | RADV (correct) | verdict |
|---|---|---|---|
| `l15` raw | `0x0001000D` | `0x0001000D` | **IDENTICAL** |
| `c33..c36` | identity, 10240/10240 draws | identity | **SAME** |

**⇒ T2 and T4 are now properly CLOSED, not just "unconfirmed":**
- The identity transform is **by design** — the platform that renders Halo 3
  *correctly* uses the exact same identity matrix. It is not the bug.
- Loop constants are **not** a platform difference.
Both behave exactly as ground rule #4 predicts for guest-written values.

⚠️ **Caveat, stated honestly:** the RADV run logged only **1** consumer draw
(`draw=1`), so no `CONSUMER_MTX_SUMMARY` (that needs 2048). The identity match is
on the first draw only, versus Android's 10240. Directionally conclusive and
consistent with rule #4, but a longer RADV run reaching the menu proper would
make it airtight.

### ★★★ T7 — Format-correct paired VALUE comparison (Adreno vs RADV). MAJOR NEGATIVE.
**Why:** the in-tree comment stated *"the FILL metric is a red herring (RADV renders
correctly while filling LESS of this buffer than Adreno). Same slots, same
consumers, same draw counts -> the only surviving explanation is that the exported
VALUES differ."* The earlier `VALSHAPE` attempt at that was RETRACTED because it
read every dword as `float32` - but this is an 80-byte INTERLEAVED record
(Stride=20 dwords) with SIX formats, so 16 of 20 dwords are packed half/short/
2_10_10_10/8_8_8_8 data whose bit patterns merely *look* like NaN/denormals as
float. That artifact produced the bogus "NaN smoking gun".

**Layout, decoded from BOTH shaders' own vfetch instructions (producer instr
90-100, consumer instr 38-45 - they agree):**
`dw0-3 FMT_32_32_32_32_FLOAT` (the ONLY genuine float32 field) · `dw4-5` half4 ·
`dw6-7`/`dw8-11` short4 · `dw12-13` half4 · `dw14` half2 · `dw15` short2 ·
`dw16` 2_10_10_10 · `dw17-18` 8_8_8_8.

**Method:** new `RECFIELD0` probe reads ONLY dw0-3 per record as float4 and reports
valid/zero/nonfinite counts plus per-axis mean, sd and range. Identical code in
both trees. ⚠️ **Both sides run with readback_memexport ON** (see the correction
below - the first attempt at this was invalid without it).

**Result at the Halo 3 main menu:**

| metric | Android (Adreno, BALL) | RADV (renders CORRECTLY) |
|---|---|---|
| valid records | **561**/7168 (7.85%) | **494**/7168 (6.89%) |
| zero records | 6603 | 6666 |
| nonfinite | 4-8 | 8 |
| sd (x,y,z) | 1.13e37, 1.63e37, 1.73e37 | 1.30e37, 1.46e37, 2.10e37 |
| x range | [-1.51e38, 1.08e38] | [-7.87e37, 1.65e38] |

**⇒ STATISTICALLY IDENTICAL. Adreno even has MORE valid records than the platform
that renders correctly.**

**Rules OUT (two big ones):**
- **"The exported VALUES differ"** - the stated last surviving explanation. For
  field0, the only genuine float32 field, they do not.
- **"Underfill is the bug"** - ~93% zero records is **NORMAL**; RADV does the same
  and renders fine. The long-running "dense prefix 5-14%" concern is a non-issue,
  now confirmed by an independent, format-correct measurement.

**What that leaves (untested):**
1. The other fields (dw4-19, the packed half/short data) - not compared yet.
2. **The consumer's READ, not the buffer's contents**: if Adreno computes a
   different slot / fetch address per invocation than RADV, both platforms can hold
   identical data yet draw different geometry. This is now the leading candidate.
3. Something outside this buffer entirely.

### ⚠️ Correction: my first T7 run was invalid
I first compared RADV **with** `--readback_memexport=true` against Android
**without** it, and got Android `valid=0/7168` (100% zero) - which looks like a
total fill failure and is pure artifact: CPU-side probes read GUEST RAM, and the
GPU's memexport writes never reach it unless readback copies them back. The memory
file warned about exactly this and I walked into it anyway. Fixed by exposing
readback as a runtime toggle (`setprop debug.canary.readback_memexport 1`, default
OFF since the copy-back is expensive) so it can be matched to the oracle without a
rebuild. **Always confirm both sides have readback ON before comparing buffer
contents.**

### ★★★ T8 — Magnitude/slot classification. T7's framing CORRECTED by the user.
**User's critique:** *"More valid records doesn't necessarily mean better valid
records."* Correct, and it invalidates how I read T7:
- T7's "valid" only meant *not-all-zero and finite*, so a record holding 1.5e38
  counted as valid. Both platforms sit at 1e37-1e38, which are NOT plausible
  world-space positions (real ones are ~1e0-1e4) - so T7 compared garbage to
  garbage.
- T7's mean/sd are dominated by 1e38 outliers, so wildly different datasets would
  still look "statistically identical". **"Statistically identical" was an
  unsound inference from those aggregates.**
- Adreno having MORE valid records is not reassurance; extra bogus records
  rendering IS the degenerate-cluster mechanism.

**Method:** `RECFIELD2` classifies each record's field0 by MAGNITUDE instead of
averaging - `plausible` (all |xyz|<1e5), `small` (<1e-3, collapsed to origin),
`huge` (>=1e20, garbage), `mid` - plus a 10-bucket histogram of WHERE the
plausible records sit. Identical code both trees, readback ON both sides.

| metric | RADV (CORRECT) | Adreno (BALL) |
|---|---|---|
| zero | 6702 | 6582 |
| **plausible** | **35** | **44** |
| small | 52 | 51 |
| mid | 105 | 140 |
| **huge (garbage)** | **268** | **344** |
| plausible x-range | [-3758, 1141] | [-2179, **21880**] |
| plausible location | **all in first 10%** | **all in first 10%** |

**Findings:**
1. **The user's point is confirmed:** Adreno's extra records are disproportionately
   JUNK - +76 huge, +35 mid, but only +9 plausible.
2. **⚠️ The field0 premise is shaky.** Only **35-44 of 7168 records (0.5%)** hold
   plausible positions on EITHER platform, and RADV renders a correct vista from
   just 35. Either field0 is not the geometry that matters, or the consumer reads
   only a tiny prefix. Every plausible record on both platforms is in the FIRST
   10% of the buffer.
3. **RADV carries 268 garbage records and renders correctly.** So garbage here is
   NORMAL and tolerated => the fault cannot be "Adreno has garbage". It must be
   **which records get fetched and drawn** - the consumer's addressing, not the
   buffer contents. Reinforces T7's leading candidate.

**Methodological rule earned:** never conclude "identical" from mean/sd on data with
extreme outliers, and never treat a *count* of loosely-defined "valid" items as a
quality measure. Classify by magnitude and location.

### ★★★★ T9 — field0 premise VERIFIED FALSE, and the ball mechanism found in ucode
User asked to verify the field0 premise before building further on it. **Good call -
it is false, and T7/T8 profiled a field the shader discards.**

**field0 is DEAD.** Consumer `488D9488AB7ED7D8`:
```
/* 38 */  vfetch_full r2.xywz, r0.y, vf1, FMT_32_32_32_32_FLOAT, Stride=20  <- field0 -> r2
/* 53 */  (p0) sgts r2._y__, -r_abs[0].x     <- r2.y OVERWRITTEN
/* 54 */  (p0) sgts r2.__z_, -r_abs[0].x     <- r2.z OVERWRITTEN
/* 55 */  (p0) sgts r2.___w, -r_abs[0].x     <- r2.w OVERWRITTEN
/* 56 */  (p0) sgts r2.x___, -r_abs[0].x     <- r2.x OVERWRITTEN
/* 66 */  (p0) mad r4  = r2.wwww*c35 + c36   <- uses the OVERWRITTEN r2
/* 74 */  (p0) mad r15 = r2.xxxx*c33 + r9
/* 262*/  (p0) mad r0  = r0.zxyy*r2.zzzz + r15.zxyy   (r2.z==0 => r0 = r15)
/* 418*/       max oPos, r0, r0
```
`sgts` computes `-|r0.x| > 0`, always FALSE => 0.0. So on the p0 path the transform
runs on ZEROS and yields just `c36`, the translation column of that identity
matrix => **the ORIGIN. Every p0-true vertex is sent to the origin. That is the
ball, written plainly in the ucode.**

**Also: the slot formula is confirmed** - instr 35-37:
`r0.y = floor((vtxIndex + c229.x) / c78.x)` with `c78.x=4`, `c229.x=0.5`.

**What actually decides p0 (NOT dw0-3):**
```
/* 46 */ sge  r0.z = (r7.x  >= c229.w)   <- r7 from Offset=12 (half4)
       + seqs r0.w = (r11.w == 0)        <- r11 from Offset=17 (8_8_8_8)
/* 47 */ add  r0.z = r0.w + r0.z
/* 49 */ setp_ne_push -> p0 = (c228.x==0) && (r0.z != 0);  c228=(0,1,8,30) so
         c228.x==0 always holds => p0 = (r0.z != 0)
```

### ★★★★ THE LEAD: `r7.x` is read UNINITIALIZED, and the two trees init it differently
`vfetch_mini r7.w__z, Offset=12` writes **only `.w` and `.z`**. Instr 46 reads
**`r7.x`**, which nothing ever wrote.

Direct source comparison:
```cpp
// UPSTREAM 6e9bac0 (RADV, renders CORRECTLY) - spirv_shader_translator.cc:554
var_main_registers_ = builder_->createVariable(spv::NoPrecision,
    spv::StorageClassFunction, type_register_array, "xe_var_registers");
                                                        // NO initializer

// XENIA-AE (Adreno, BALL) - spirv_shader_translator.cc:550, commit 54e6a4d6
var_main_registers_ = builder_->createVariable(spv::NoPrecision,
    spv::StorageClassFunction, type_register_array, "xe_var_registers",
    register_array_zero);                               // ZERO-INITIALIZED
```
A SPIR-V Function-storage variable without an initializer is *indeterminate* per
spec. So:
- **AE:** `r7.x = 0` => `(0 >= 1)` false => `r0.z = 0`
- **Upstream/RADV:** indeterminate => may differ => `r0.z` may be 1

`r0.z` gates `p0`, and `p0` gates the origin-collapse path. **So the two platforms
can take different branches through this shader because of a register the GAME
never initialized - and AE's own zero-init fix (54e6a4d6) is what makes AE's value
deterministic and possibly different from what the game relies on.**

⚠️ Not yet proven to be THE cause - the p0 polarity and what the non-p0 path draws
still need care, and `r11.w` (dw17's high byte) also feeds `r0.z`. But this is the
first *verified platform difference on the exact code path that produces the ball*.

**Next experiment (cheap, decisive):** change AE's register-array initializer from
0.0 to a value >= 1.0 (or drop the initializer) and have the user look at Halo 3.
If the characters change, the mechanism is confirmed. Requires a live visual check -
per the standing rule, only the user's view counts.

### T10 — Register initialization RULED OUT (both variants), and T9's p0 reading is WRONG
T9 found a genuine platform difference: upstream `6e9bac0` creates
`xe_var_registers` with **no initializer** (indeterminate per SPIR-V spec) while
Xenia-AE **zero-initializes** it (`54e6a4d6`), and Halo 3's consumer reads `r7.x`
**uninitialized** (instr 42 writes only `.w`/`.z`; instr 46 reads `.x`).

Made the init value switchable at runtime (`debug.canary.reginit`, TESTRIG module,
default `0` = current behaviour; also accepts a float, or `none` for no
initializer). ⚠️ Shader cache MUST be cleared between values or old pipelines are
reused - verified each run regenerated it (`Created new pipeline cache` = 1).

| reginit | meaning | result (user's live view) |
|---|---|---|
| `0` | current AE behaviour | **ball** (baseline) |
| `1` | forces `sge r0.z = (1.0 >= 1.0)` TRUE | **still a ball** |
| `none` | **matches upstream EXACTLY** | **still a ball** |

**⇒ Register initialization is NOT the cause.** Since `r7.x` and `r11.w` are the
only inputs to `r0.z`, and changing `r7.x`'s value across its whole meaningful
range changes nothing, either `r11.w` dominates or the predicate reading is wrong.

**⇒ And T9's `p0` interpretation is WRONG.** `reginit=1` should have forced `p0`
TRUE for *every* vertex; under T9's reading ("p0-true => r2 zeroed => position =
origin") the entire vista should have collapsed. It did not - it rendered normally
(still inverted, which is the separate orientation defect). So `(p0)` predication
here does not mean what I assumed; `setp_ne_push` has predicate-STACK/push
semantics and there are `(!p0)` paths I did not trace.

### ★ Methodological conclusion (earned the hard way, 3 failures in one session)
Static ucode reading by eye has now produced **three** wrong results today:
1. `l15` bit-decode (`start`/`step` swapped) - T4/T6
2. field0 assumed to be the position when it is overwritten before use - T7/T8
3. `p0` semantics - T9/T10

Every result that HELD UP came from **identical instrumentation on both platforms,
diffed**. Rule: **do not act on ucode inference; measure it on both sides.**

### Corrections to previously recorded conclusions
- **Y-flip cannot cause the ball** — but only in its *global* form. A single
  viewport flip is affine/invertible, so it cannot collapse distinct vertices.
  ⚠️ **Per-part** flips about per-part pivots are *piecewise*-affine (a different
  translation per part) and **can** pile parts into a blob. Do not over-apply the
  "affine can't do it" argument. (User found this hole.)
- **The inverted vista is NOT a viewport/NDC flip.** The 2D UI overlay is not
  flipped, and a viewport flip inverts everything drawn through it. Suspect the
  vista's own RT → EDRAM → resolve → shared-memory → texture round trip
  (orientation / V-flip) rather than `draw_util.cc GetHostViewportInfo`, whose
  sign is preserved in the normal path (`ndc_scale_axis = scale_axis * 2 * inv`;
  `std::abs` is used only for bounds, which is correct).

---

## The open question
**Why does the producer fill only a dense prefix (~5-14%)?** Everything above
narrows *where* to look (the memexport buffer data, and what terminates the fill)
without answering it.

**⚠️ Desktop paths changed 2026-07-30 (disk cleanup, 25 G → 20 G).** The
authoritative layout is `~/xeniatest/README.md`.
- **Known-good oracle:** `~/xeniatest/oracle/build/bin/Linux/Release/xenia_canary`
  (worktree at tag `6e9bac0`, Release/RADV).
- **Instrumented build goes in a SEPARATE folder:** `~/xeniatest/oracle-probe/`
  (worktree at the same commit) so the known-good build is never modified.
- ☠️ **Never delete `~/xeniatest/canary-git`** — `oracle/`, `oracle-probe/` and
  `canary-fork/` are git worktrees whose `gitdir` lives inside it. It is the
  shared object store and the largest directory; deleting it destroys all three.
  Only its `build/` was removed.
- Probes ported there must be **modules, off by default** (cvar-gated, e.g.
  `--probe_consumer_mtx=true`, tagged `PROBE(<area>)`), so with everything off it
  is a standard Xenia build that can be validated against real games first. Same
  rule as the Android TESTRIG harness.

**Next test, and the standing blocker:** every remaining lead needs the **RADV
oracle reference** at the *menu* (now cheap thanks to T1). The oracle binary exists
at `/home/roman/xeniatest/oracle/build/bin/Linux/Release/xenia_canary` (built
2026-07-25) but its source carries **none** of these probes, so `CONSUMER_MTX` /
`CONSTDUMP loop` must be ported there and rebuilt (clang + `-flto=thin`, Release,
fresh `XDG_DATA_HOME` per run — see `HALO3_MENU_INVESTIGATION.md`) before any of
T2/T3/T4 can be called a fault rather than a curiosity.
