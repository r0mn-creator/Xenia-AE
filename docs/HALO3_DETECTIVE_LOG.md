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

**Result at the menu:** **`l15 = 0x0001000D`** ⇒ **count = 13, start = 1, step = 0.**

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
