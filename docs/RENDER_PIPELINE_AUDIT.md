# Render Pipeline Audit — "core → screen" path

Purpose: map the FULL path a game's rendering takes from the Xenia core to the
Android screen, mark each stage **WORKS / BROKEN / REDUNDANT**, and define the
**shortest-path target** so we can later strip whatever isn't needed. Keep this
updated as we prove/disprove things. Device: Adreno 740 (Odin 2). Reference
scene: Halo 3 main-menu 3D vista (deferred-shaded).

Legend: ✅ works · ❌ broken on Adreno · ⚠️ works but heavy/indirect (shortcut
candidate) · 🔵 needed for correctness (don't remove) · 🟡 removable/dead once a
shortcut lands.

---

## 1. The real model (correct this first)

The Xbox 360 game does its OWN deferred rendering. It draws multiple G-buffers
(albedo, normals, HDR, etc.), then runs its OWN lighting/composite pass that
reads those G-buffers as textures and outputs one final lit image. **The
"single clean image" already exists — the game makes it (the composite pass).**
We do NOT need to build a compositor/flattener; that would duplicate what the
game already does. Our job is only to faithfully carry the game's own buffers
through, and present the game's own final image.

So the bug is NOT "we need to flatten layers." It's that ONE step in our
emulation of the Xbox 360 EDRAM→memory copy is mis-compiled by the Adreno
driver, corrupting one of the G-buffers the game's composite reads.

---

## 2. Full current path (Halo 3 vista), stage by stage

| # | Stage | What it does | Status |
|---|---|---|---|
| 1 | GPU command processor (PM4) → Vulkan draws | Game's draws render the deferred G-buffer into host render targets that emulate EDRAM (4×MSAA, tiles 608=albedo, 1216=other). | ✅ 🔵 (geometry renders; EDRAM tile 608 reads back varied every frame) |
| 2 | Host render targets hold EDRAM contents | Emulate the 10 MB EDRAM as host Vulkan images. | ✅ 🔵 |
| 3 | **DUMP** `DumpRenderTargets` | Compute shader copies a host RT → `edram_buffer_` (the linear EDRAM emulation buffer). | ✅ (tile 608 EDRAM varied every frame — dump works) ⚠️ round-trip |
| 4 | **RESOLVE-COPY** `resolve_full_32bpp` | Compute shader copies `edram_buffer_` → shared memory (guest RAM), converting tiling/format. | ❌ **BROKEN** — Adreno driver collapses the per-thread read address → every thread reads the same EDRAM location → uniform output. THE BUG. |
| 5 | **Texture LOAD** | Compute shader copies shared memory → a Vulkan texture image (untile). | ✅ (loaded image == shared memory byte-for-byte) ⚠️ round-trip |
| 6 | **Composite draw** (psh 0x373E65D9) | Game's lighting/tonemap pass samples albedo (0x044B0000) + HDR (0x043FC000) textures, writes EDRAM tile 1216. | ✅ 🔵 (reaches screen — magenta test; only broken because its albedo input from stage 4 is uniform) |
| 7 | Resolve composite RT → front buffer 0x04E20000 | Same resolve machinery as #4, but the composite output is 1× and lands correctly. | ✅ (front buffer / 2D UI present crisp) |
| 8 | Present front buffer | Swapchain present to the Android surface. | ✅ 🔵 |

**Net: 1 broken stage (#4) out of 8.** The corruption is isolated. Stages
3→4→5 are a big round-trip (host RT ↔ EDRAM buffer ↔ shared memory ↔ texture
image) that a hardware resolve could shortcut — see §4.

---

## 3. What is PROVEN (do not re-test)

- Geometry/EDRAM: ✅ the G-buffer renders; EDRAM tile 608 = ~59,000 distinct
  (varied) every frame.
- Dump (RT→EDRAM buffer): ✅ produces varied EDRAM every frame.
- Resolve-copy (EDRAM buffer→shared memory): ❌ collapses varied→uniform in the
  SAME frame (definitive same-frame capture: EDRAM 608 varied, shared memory
  uniform). Shader = `resolve_full_32bpp` (kFull32bpp, index 6). Adreno DRIVER
  mis-compilation (reduced spirv-opt also collapses).
- Texture load (shared memory→image): ✅ image == shared memory byte-for-byte.
- Composite / present: ✅ reaches screen (magenta test), constants sane.

### Fixes tried on stage #4 that DID NOT WORK (7, all reverted, shader tree
byte-identical to committed) — do not repeat:
1. shift-by-comparison-vector → explicit multiply.
2. reduced spirv-opt level.
3. inline the address computation (bypass function call).
4. remove the wrap modulo.
5. division-free address (rules out integer divide).
6. manual GlobalInvocationID (workgroup×size+local).
7. remove the per-thread early bounds-return.
Conclusion: not a single arithmetic op — the Adreno driver collapses the
per-thread buffer-read index whenever the full source-address+load path is
present. A constant-output probe (which lets the compiler delete the address
math) writes all pixels correctly, proving threading/dest/write are fine.

---

## 4. Shortest-path target architecture

The current path has three copy/compute round-trips (dump, resolve-copy,
texture-load) between the render target and the composite's texture input. Two
ways to shorten it, both of which ALSO bypass the broken stage #4:

### Option A (incremental): hardware-resolve → shared memory
Replace the broken compute resolve-copy with a **fixed-function hardware
resolve** (`vkCmdResolveImage`, which we've PROVEN works on this GPU — it gave
~23,000 distinct values from the same MSAA RT) to a 1× image, then copy that
into shared memory. Removes stage #4's broken compute shader. Still keeps
shared memory + texture load (stage #5). Catch: shared memory is Xbox-tiled, so
the linear→tiled write still needs a (working) copy shader.

### Option B (shortest): render-target-as-texture ("serve the RT directly")
When the composite samples a texture whose guest address (0x044B0000) is a
render target we JUST resolved, serve the **hardware-resolved RT image directly
as that texture** — skip stages #3, #4, AND #5 entirely. This is the shortest
possible path: RT → (hardware resolve) → sampled by the composite. No EDRAM
buffer, no shared memory, no compute resolve, no texture load. Bypasses the
broken shader by construction and is the most efficient. Cost: more machinery
to detect "this texture == a live resolved RT" and to match formats/tiling
expectations (Xenia's texture cache has partial RT-awareness). Note: an earlier
capture found "AE doesn't do RT-as-texture serving" — building it is the work.

### Constraint (why we can't just delete the long path)
The Xbox 360 EDRAM + predicated-tiling model is used by games in weird ways
(EDRAM aliasing, reinterpretation, partial resolves). The long path is 🔵 needed
for correctness in the general case. The shortcut must be for the COMMON case (a
straightforward MSAA color RT resolved and immediately sampled), with automatic
FALLBACK to the full path when the game does something the shortcut can't model.

---

## 5. Removable-once-shortcut-lands (track for cleanup)
- 🟡 The compute resolve-copy for the common non-converting case (Option B makes
  it dead for that case; keep for the general/converting case).
- 🟡 The EDRAM-buffer round-trip for the common case (Option B).
- 🟡 Diagnostic capture/probe infra (Testrig*Deferred, resolve-skip flags,
  RTOWN/RTTRANSFER/RESOLVECOPY/COMPOSITE_BIND logs) — Canary-only; strip from
  the shipping Xenia-AE build, keep in canary-ae.
- 🟡 The dormant `read_resolved_1x` MSAA-dump companion path (proven not the
  cause; kept but unused).

---

## 6. Recommended order of attack
1. **Custom Adreno driver** via libadrenotools — cheapest test; it's a driver
   bug, a newer driver may lack it. If it fixes stage #4, we get correctness now
   with zero architecture change while we build the shortcut for efficiency.
2. **Option A** (hardware-resolve → shared memory) — moderate change, removes
   the broken shader, keeps the rest.
3. **Option B** (RT-as-texture) — the real shortest-path goal; do this for the
   efficiency win regardless of how stage #4 is fixed.

Reference: `docs/HALO3_FINDINGS_CHECKLIST.md` has the full decision log and the
same-frame captures that prove each status above.
