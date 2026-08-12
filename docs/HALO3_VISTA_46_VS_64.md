# Halo 3 vista — the 46-vs-64 resolve lead

**Single-source reference for the upside-down Halo 3 vista.**
Read this BEFORE running any vista test. Most plausible ideas have already been
tested and eliminated with evidence; re-running them wastes a session.

Status 2026-08-12: **not fixed.** Narrowed from ~13,000 diverging lines to one
resolve, one address, one register.

---

## 1. THE LEAD, in one table

Halo 3 main menu. Every resolve destination matches between the two emulators
**except one**:

| | `w_div8` | `h_div8` | pixels | `len` | order |
|---|---|---|---|---|---|
| **Canary AE** | **46** | **46** | 368x368 | 770,048 | n=**25** (last) |
| **XenDroid** | **64** | **64** | 512x512 | 1,048,576 | n=**5** |

Address `0x04D20000`, `fmt=22`, `depth=1`.

- XenDroid's is self-consistent: 64*8 = 512, and 512*512*4 = 1,048,576 exactly.
- The **read side** binds that address as **512x512 tiled, fmt 22**
  (`debug.canary.texbind` - a direct texture-key read, no derived arithmetic).

**So the guest binds a 512x512 depth surface, XenDroid fills it, and we write
only 368x368 into it.** Xenos derives row addresses from surface dimensions, so
the composite samples a partly-unwritten, mis-addressed surface.

This is **internally inconsistent inside our own build** (write 368, read 512),
not merely different from theirs - which is why it outranks everything else.

## 2. Where the bug is NOT (proven identical in both trees)

Do **not** re-diff these. Confirmed byte-identical:

- `width_div_8` / `height_div_8` assignment:
  `(x1 - x0) >> kResolveAlignmentPixelsLog2`
- The **surface-pitch clamp**: `x0/x1 = std::min(..., surface_pitch_aligned)`
- The 8-pixel resolve-rectangle alignment
- `ResolveCoordinateInfo`'s `width_div_8`/`height_div_8` **bitfield declarations**

**Therefore 46 vs 64 comes from the INPUTS, not the maths.**

## 3. THE NEXT TEST (narrow, definite, not yet run)

In **both** builds, gated behind a toggle and filtered to
`copy_dest_base == 0x04D20000` only, log immediately before `width_div_8` is
computed:

```
x0, x1, y0, y1                      (final rect)
the PRE-clamp vertices
rb_surface_info.surface_pitch
surface_pitch_aligned
rb_copy_control
```

**Whichever input differs IS the bug**, because everything downstream is proven
identical.

**Prime suspect: `surface_pitch`.** 46*8 = 368 and 64*8 = 512, so a
`surface_pitch_aligned` of 368 on our side would produce the observed value
through the clamp alone.

---

## 4. ELIMINATED — do not retest

Each of these was tested on-device with evidence. Nineteen dead ends:

### Eliminated by direct measurement against XDtester
| hypothesis | how it died |
|---|---|
| **NDC scale/offset** | Both builds produce **identical** per-draw regimes: 3 unflipped shaders (`0A6D1DD7767FDF27`, `C049A8C9E556F129`, `C2543FD5CD52420B`), same values, same everything |
| **Vertex-stage Y** | The vista draws **DO** receive the flip (`ndc_scale_y = -1`); only 3 screen-space UI draws are unflipped, correctly |
| **Translated shader position math** | Instruction-for-instruction identical (`pos.xyz * ndc_scale + ndc_offset * w`); only member indices shift |
| **`host_vertex_shader_type` / the `0x12000000` decode** | Disproven - would appear in NDCYDRAW; does not. The bit decode was wrong |
| **Resolve rectangle degeneracy/inversion** | Guard ported; **0 occurrences** on Halo 3 |
| **Resolve tiling pitch (raw vs aligned)** | Ported and active; vista unchanged |

### Eliminated by on-device toggle
`fix_wclip` (our W==0 guard) · `fix_rsq` (our RSQ precision fix) ·
`vfetch_bounds_clamp` · `vulkan_user_clip_planes` (implemented fully; Halo 3
never sets `ucp_ena`, so it never engages) · `readback_resolve=full` ·
`clear_memory_page_state` · `ytest_fallback` / `ytest_invert`

### Eliminated by config bisect on XenDroid's own build
`vulkan_resolve_to_texture` (+`_promote`/`_serve`) - off, their models/vista
still correct · `vulkan_shared_memory_host_visible` - off, still correct ·
`VK_EXT_external_memory_host` two-buffer routing - **extension is not enabled on
ANY Adreno**, dead code here · memexport CPU-visibility - their
`memexport_enable = false` and it still renders correctly

### Eliminated earlier
driver/Turnip (XenDroid renders correctly on the SAME driver) · resolve row
addressing · rectangle-list geometry shader (byte-identical to upstream)

## 5. RETRACTED claims — do not resurrect

| claim | why it was wrong |
|---|---|
| "336x336 vs 512x512" | **Derived** size from a stale build; 336*336*4 = 451,584 did not match its own reported `len` of 770,048. Superseded by the raw-field 46 vs 64 |
| "The composite doesn't read guest RAM" | The marker probe bypassed the texture cache's memory watches; a `gpu_written` page is never re-uploaded, so the null result was structurally guaranteed |
| "The cause is driver-side (Adreno/Turnip)" | XenDroid renders correctly on the same device and driver |
| "ISB for db16cyc is a 21.5% regression" | Our version never coalesced; we benchmarked 8 pipeline flushes per loop against 8 free no-ops |
| "9.67 FPS baseline" | Contaminated by `readback_memexport` + `readback_resolve` left enabled. Turning both off: **~10 -> 15 FPS** |
| "We win on face stability" | Never observed - our characters are blobs, so there were no faces to judge. Face flicker is a **shared** bug |

---

## 6. TOOLING — how to reproduce any of this

### XDtester
Instrumented XenDroid built from source.
`/home/roman/xeniatest/xendroid-git` (branch `xdtester`),
package **`xendroid.compose.xdtester.debug`** - installs ALONGSIDE stock
`xendroid.compose`, which stays untouched as the known-good reference.

Build: `JAVA_HOME=/opt/android-studio/jbr ./gradlew :app:assembleDebug`

Debug module: `src/xenia/base/xdt_debug.h` - self-contained, toggleable,
**no platform guard** (see gotchas), `debug.canary.*` names matching Canary AE
so both logs diff line-for-line.

### Probes (identical names/formats in both builds)
| toggle | what it logs |
|---|---|
| `debug.canary.vista_dump` | `VISTA ENUM` - one line per distinct resolve destination, **raw** `w_div8`/`h_div8`/`len` |
| `debug.canary.ndcy_draw` | `NDCYDRAW` - per-draw NDC-Y regime + shader hash |
| `debug.canary.ndcy` | `NDCY` - viewport registers -> `ndc_scale[1]` |
| `debug.canary.texbind` | `TEXBIND` - **read side**: what each texture fetch constant actually binds |
| `debug.canary.resolve_rect_trace` | `RECTTRACE` - resolve rect vs scissor |

All default OFF. `setprop` + relaunch.

### Reproduce the headline result
```sh
# 1. delete the pipeline cache or NOTHING is re-translated / re-resolved
adb shell "su -c 'rm -f <files>/cache/pipelines_4D5307E6.bin'"
# 2. enable
adb shell setprop debug.canary.vista_dump 1
# 3. launch Halo 3, reach the MAIN MENU (no level load needed)
# 4. compare the two logs
adb shell "grep 'VISTA ENUM' <files>/xe.log"
```

---

## 7. GOTCHAS that cost real time

1. **`cache/pipelines_<TITLEID>.bin` short-circuits translation.** A 30 MB cache
   meant ZERO shader dumps and stale resolves. **Delete it before any GPU
   test.**
2. **"BUILD SUCCESSFUL" does not mean your code is in the binary.** Always
   `strings libe.so | grep <your probe>`. Gradle reported success while shipping
   an older `.so` more than once.
3. **A wrong platform guard silently deletes probes.** `xdt_debug.h` originally
   guarded on `XE_PLATFORM_ANDROID`, which XenDroid does not define -> the
   function compiled to `return false` -> the compiler stripped every probe body
   INCLUDING its log strings, while the build succeeded. Cost three cycles.
   XDtester's header now has no platform guard.
4. **Never append a duplicate key to a TOML config.** A second `vulkan_lib_path`
   made the whole file fail to parse and the emulator silently fell back to
   defaults. **Edit the existing key.**
5. **App configs are mode 660 owned by the app uid.** A plain `adb shell grep`
   returns *permission denied*, which reads like "key absent". Use `su`.
6. **Sanity-check derived numbers against a raw field.** The retracted 336x336
   was caught by a 4-bytes-per-pixel multiplication that took seconds.
7. **The vista renders at the MAIN MENU.** Never load a level to test it -
   ~90 s per iteration instead of ~4 min.
8. Halo 3 title ID: **`4D5307E6`**. Turnip R8 driver lives at
   `/data/data/<pkg>/driver/vulkan.ad07xx.so`, set via `vulkan_lib_path`.

---

## 8. Reference points

- XenDroid renders Halo 3 **models AND vista correctly** on the same Odin 2 +
  Turnip R8, at **17.9-20.6 FPS** vs our 15. Both bugs are ours and fixable.
- Their weaknesses to NOT inherit: **NFS Carbon geometry pop-in/transparency**,
  flatter colour. Face flicker is shared.
- Revert point: tag **`pre-xendroid-port`**. Work branch: `xendroid-port`.
- Related: `HALO3_VISTA_UPSIDE_DOWN.md` (full narrative),
  `HALO3_BALL_XENDROID_FIX.md` (the models bug),
  `XENDROID_FULL_COMPARISON.md` (engine-wide comparison).
