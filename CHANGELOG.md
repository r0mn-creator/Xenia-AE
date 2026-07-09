# Xenia AE — Change Log

Format: `[YYYY-MM-DD HH:MM] — Title`
Each entry records: What changed, Why, and the test result.

---

## [2026-07-03 ~current] — Phase 1: Project creation from aX360e + advertisement removal

### What
- Created new project "Xenia AE" at `/home/roman/Android/Xenia-AE/`
- Based on aX360e (WTFPL licensed C++ + Java UI, BSD licensed xenia-canary engine, MIT licensed libadrenotools)
- Rebranded throughout: package `aenu.ax360e` → `org.xeniaae`, app name "aX360e Free" → "Xenia AE"
- Removed advertisement entirely (see below)
- Storage directory renamed: `ax360e` → `xeniaae`

### Files changed (rebrand)
| File | Change |
|------|--------|
| `settings.gradle` | Project name `ax360e` → `Xenia-AE` |
| `app/build.gradle` | Namespace + applicationId `aenu.ax360e` → `org.xeniaae`; versionCode=1 versionName=1.0 |
| `AndroidManifest.xml` | All class names repackaged; intent action rebranded; Google Ads meta-data removed; INTERNET+ACCESS_NETWORK_STATE permissions removed |
| All `app/src/main/java/aenu/**` | Moved to `app/src/main/java/org/xeniaae/**`; package declarations updated |
| `app/src/main/cpp/emulator.cpp` | JNI FindClass strings updated to `org/xeniaae/...` |
| `app/src/main/cpp/emulator_ax360e.cpp` | JNI FindClass strings + preference class tags updated |
| `app/src/main/cpp/hardware_ProcessorInfo.cpp` | JNI export function renamed `Java_aenu_hardware_...` → `Java_org_xeniaae_hardware_...` |
| `app/src/main/res/xml/emulator_settings.xml` | Custom preference class names repackaged |
| `app/src/main/res/values/strings.xml` (+ all locale variants) | `app_name` changed to "Xenia AE" |
| `app/src/main/java/org/xeniaae/Application.java` | Data dir `"ax360e"` → `"xeniaae"` |

### Advertisement removal
**Why:** `AppOpenAdManager.java` is Apache 2.0 Google code. Removing it also removes the Google
Mobile Ads SDK dependency and the mandatory Internet permission.

| Item | Action |
|------|--------|
| `AppOpenAdManager.java` | **Deleted** (Apache 2.0 Google code) |
| `app/build.gradle` deps | Removed `play-services-ads-api`, `play-services-ads`, `user-messaging-platform` |
| `AndroidManifest.xml` | Removed `com.google.android.gms.ads.APPLICATION_ID` meta-data; removed INTERNET + ACCESS_NETWORK_STATE permissions |
| `MainActivity.java` `on_create()` | Was: load ad → show ad → then call `_on_create()`. Now: calls `_on_create()` directly |
| `MainActivity.java` `onStart()` | Removed `AppOpenAdManager.getInstance(this).showAdIfAvailable(this)` call |
| Google import lines | Removed from `MainActivity.java` |

### How aX360e was built (reference notes for clean-room replication)

#### Architecture overview
```
Java layer (org.xeniaae)
├── Application.java        — App singleton; GPU detection; dir setup; loads JNI lib if not Adreno 5/6
├── MainActivity.java       — Games list (ListView), game dir picker (SAF), launch intent
├── EmulatorActivity.java   — Game window (SurfaceView), runs in :emu process, receives intent
├── Emulator.java           — Java shim extending emulator.Emulator (JNI bridge)
└── aenu.emulator.Emulator  — Core JNI interface (boot, config, path, game info structs)

C++ JNI layer (libxeniaae.so)
├── ax360e.cpp              — JNI_OnLoad: registers all JNI classes
├── emulator.cpp            — Boots xenia-canary; implements aenu.emulator.Emulator JNI methods
├── emulator_ax360e.cpp     — Implements aenu.ax360e.Emulator JNI methods (settings, game meta)
├── xe_aaudio_audio_*.cpp   — AAudio driver (Android audio output)
├── xe_android_hid.cpp      — Android gamepad/touch input driver
├── xe_saf_*.cpp            — Storage Access Framework wrappers (ISO/XEX via Android URIs)
├── vkapi.cpp / vkutil.cpp  — Vulkan helper (GPU name detection, custom driver loading)
├── cpuinfo.cpp             — CPU/GPU info via Vulkan
└── xenia-canary/           — BSD Xenia engine (a64 JIT backend, Vulkan GPU, PPC CPU)

Key data flow (launching a game)
1. User taps game in MainActivity → Intent("org.xeniaae.intent.action.EMULATE") + EXTRA_GAME_URI
2. EmulatorActivity (in :emu process) receives it, creates SurfaceView
3. EmulatorActivity calls Emulator.get.setup_*(context, surface, path, config)
4. emulator.cpp boots xenia-canary with the game URI wrapped in xe_saf_disc_image_device
5. Xenia runs the PPC game code via a64 JIT on ARM64
6. GPU commands go through xenia-canary Vulkan command processor → Adreno 740 GPU
7. Audio goes through xe_aaudio_audio_system → AAudio → speaker

Key directories on device
- External: /sdcard/Android/data/org.xeniaae/files/xeniaae/   (game configs, save data)
- Internal: /data/data/org.xeniaae/xeniaae/driver/            (custom driver storage, exec-capable)
- Game ISOs: user-selected via SAF (DocumentFile tree URI, no path assumption)

Two-process design (:emu)
- EmulatorActivity runs in android:process=":emu" (separate process from MainActivity)
- This isolates crashes: emulator crash doesn't kill the game list UI
- Communication: Intent extras only (URI string, no shared memory)

Custom Vulkan driver support (libadrenotools)
- Application.get_custom_driver_dir() → /data/data/org.xeniaae/xeniaae/driver/ (exec-capable partition)
- User can drop a custom Adreno driver .zip there via the Settings UI
- vkapi.cpp uses libadrenotools to load it before creating the Vulkan instance

Delay-load for old Adreno (5xx/6xx)
- Application.should_delay_load() returns true for "Adreno (TM) 5*" and "Adreno (TM) 6*"
- These GPUs need the JNI library loaded before the Vulkan surface exists
- For Adreno 7xx (e.g. 740), load happens immediately in Application.onCreate()
```

### Test result
- **PENDING** — build not yet attempted (Phase 1 setup complete, next step: build + install)

---

## [2026-07-03] — Phase 2: NFS Carbon breakthrough — race completed on Odin 2

### What
- Fixed a use-after-free crash on Xbox Live connection attempts (`ExTerminateThread` released the thread handle, destroying its mutex, before `Thread::Exit()`'s own cleanup tried to lock that same mutex).
- Fixed a `SIGTRAP` crash when the player takes control of the car, caused by an `assert_always()` in the XMA audio decoder firing on a valid end-of-stream condition.
- Optimized the GPU command processor's idle-wait loop and the multi-handle `WaitMultiple` poll loop (both were burning CPU on syscall-heavy busy-waits instead of using cheap ARM `yield` hints before blocking).
- Suppressed Xbox Live connection attempts by reporting the local profile as offline-only.

### Why
The UAF crash blocked any attempt to reach the main menu once the game tried to sign in to Xbox Live. The XMA crash blocked entering a car. The wait-loop optimizations were a performance pass once the game was running well enough to profile.

### Files changed
| File | Change |
|------|--------|
| `xenia-canary/src/xenia/kernel/xthread.cc` | `XThread::Exit()`/`Terminate()`: replaced direct `ReleaseHandle()` + `Thread::Exit()` with `pthread_cleanup_push(ReleaseHandle, this)` so the handle is released *after* `pthread_exit()` completes, not before. |
| `xenia-canary/src/xenia/base/threading_posix.cc` | Added destruction-safety guards (`is_destroyed_`, `waiter_count_`) to `PosixConditionBase` so in-flight waits drain cleanly before the mutex is destroyed. `WaitMultiple`'s busy-poll changed from a 1ms `sleep_for` to a 32-iteration ARM `yield` spin + 200µs sleep. |
| `xenia-canary/src/xenia/kernel/xam/user_profile.h` | `UserProfile::type()` returns `1` (local only) instead of `1 \| 2`, reducing Xbox Live connection attempts. |
| `xenia-canary/src/xenia/apu/xma_context_old.cc` | Removed 4 `assert_always()` calls; `GetNextFrame()`'s negative-index case now treated as a normal end-of-stream instead of a fatal assertion. |
| `xenia-canary/src/xenia/gpu/command_processor.cc` | `WorkerThreadMain`'s idle wait loop: 17 ARM `yield` hints before falling back to a real 2ms blocking wait on `write_ptr_index_event_`, instead of hundreds of `sched_yield()` syscalls. |

### Test result
- Build: pass
- NFS Carbon: pass — reached main menu, completed a full race on the Odin 2 (Adreno 740)
- Known issue at the time: audio cuts out mid/end-of-race (XMA streaming buffer starvation, not yet fixed — see 2026-07-05 entry below for current status)

---

## [2026-07-04] — Phase 3: UI redesign (Profiles/Games/Settings)

### What
- Added PROFILES / GAMES / SETTINGS tab structure with an Xbox 360-style theme.
- Added box art scanning (embedded XEX thumbnail + Wikipedia fallback), custom box art, and game detail/properties dialogs.
- Added a "Pre-cache Shaders" launch mode.
- Changed the "Add Games" FAB to an icon-only button (no text), and made the accent green less lime/more true-green.
- Fixed the Dark Mode toggle getting permanently stuck once enabled (two independent root causes — see below).
- Fixed "Remove from Library" crashing (`IllegalStateException`, fragment detached before a deferred confirmation dialog callback ran).
- Fixed "Refresh Game List" being a no-op for games added via the legacy auto-migration path.
- Fixed a duplicate-library-entry bug caused by MediaStore reassigning row IDs across file renames (URI-only dedup treated the same physical file as new); added Title-ID-based dedup as a second check.

### Why
Product/UX pass to bring the app in line with the target look-and-feel and fix several real bugs found during that work.

### Files changed
| File | Change |
|------|--------|
| `app/src/main/res/values/colors.xml` | `xenia_green` hue shifted from lime (~80°) to true green (~98°). |
| `app/src/main/res/layout/activity_main.xml`, `MainActivity.java` | FAB changed from `ExtendedFloatingActionButton` to plain `FloatingActionButton`, icon-only. |
| `MainActivity.java` | `refreshGameList()` now also re-runs the legacy scan path, moved to a background thread; added `isDuplicateByTitleId()` dedup using `GameScanner.peekTitleId()`. |
| `GameScanner.java` | Added `peekTitleId()` — fast, synchronous XEX-header Title ID read for dedup, without the full box-art scan. |
| `GamePropertiesDialog.java` | `handleOption()` now captures the Activity reference once, before the outer list dialog can auto-dismiss, instead of using `requireContext()`/`getActivity()` inside deferred dialog callbacks. |
| `AndroidManifest.xml` | Removed `uiMode` from `configChanges` on `MainActivity`/`VirtualControlEdit` — its presence silently broke `AppCompatDelegate.setDefaultNightMode()`'s ability to apply a new theme. |
| `SettingsFragment.java` | Added `sw.setSaveEnabled(false)` on each settings switch row — all 4 rows share the same `row_switch` layout ID, so Android's view-state restoration was clobbering a manually-set switch value on relaunch, making Dark Mode look "stuck". Removed a now-redundant `recreate()` call. |

### Test result
- Build: pass
- All items above verified on-device (Odin 2)
- Not built this session: controller auto-detect (proposed, not approved)

---

## [2026-07-04] — Phase 4: Halo 3 boot crash fixed (JIT reentry unwind)

### What
Fixed a `SIGABRT` (uncaught `xe::kernel::FiberReentryException`) that crashed Halo 3 a few seconds after boot, every time.

### Why
Root-caused via targeted `__android_log_print` instrumentation (bypassing Xenia's own buffered logging, which was losing messages before the abrupt crash) and an independent `_Unwind_Backtrace` call at the throw site: the DWARF `.eh_frame` unwind info registered for JIT-compiled guest code (via `__register_frame`) was not reliably found by the unwinder on this platform (Android/bionic) once unwinding needed to cross from compiled engine code into the JIT code cache. This is used whenever a game does a fiber/stack-switch via `KeSetCurrentStackPointers` (Halo 3's engine does this; NFS Carbon does not, which is why this was never seen before).

### Files changed
| File | Change |
|------|--------|
| `xenia-canary/src/xenia/kernel/xthread.h`/`.cc` | `XThread::Reenter()`/`Execute()`: on this platform, switched from throwing a C++ exception (relies on the unreliable JIT unwind info) to `setjmp`/`longjmp` — the same mechanism the Windows backend already uses for this exact scenario. Safe because guest JIT frames never contain C++ objects with destructors, and the host frames between `Execute()` and the JIT call site hold no RAII guards across the boundary. |
| `xenia-canary/src/xenia/cpu/backend/a64/a64_code_cache_posix.cc` | Increased the unwind-info table budget from a fixed, unchecked 64MB to 256MB with an explicit bounds check (fails loudly instead of silently overflowing into unrelated heap memory) — a latent bug found during investigation, not the actual root cause. Also fixed the destructor deleting the wrong pointer (it deletes the table's advancing write cursor, not the original allocation). |

### Test result
- Build: pass
- Halo 3: pass — now boots past the crash point into its opening cinematic
- NFS Carbon: re-tested, not regressed

---

## [2026-07-04] — Phase 5: Vulkan tessellation support (adaptive triangle patches)

### What
Implemented hardware tessellation support in the Vulkan/SPIR-V rendering backend for the one domain/mode both Halo 3 and NFS Carbon actually use (`kTriangleDomainPatchIndexed` + adaptive tessellation mode — used for water/terrain). Previously, tessellated draws were entirely unsupported (an explicit `TODO(Triang3l): Tessellation` in the code), which surfaced as spammed "Failed in backend" errors and invisible water/terrain.

### Why
Both games' level-transition scenes render tessellated water/terrain, which was simply missing before this work.

### Files changed
| File | Change |
|------|--------|
| `xenia-canary/src/xenia/gpu/vulkan/vulkan_pipeline_cache.h`/`.cc` | Added patch-topology detection and `VkPipelineTessellationStateCreateInfo` wiring to pipeline creation; added `EnsureTessellationShadersAdaptiveTriangleCreated()` — two new, generic (non-per-game) SPIR-V shader modules (a passthrough vertex shader and a tessellation-control "hull" shader) built via a standalone `SpirvBuilder`, mirroring the existing `GetGeometryShader()` pattern used for other fixed shaders. |
| `xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.h`/`.cc` | Added a small `TessellationPushConstants` push-constant range to the main pipeline layout (inert for non-tessellated pipelines) to deliver the handful of register-derived values (vertex index endian/offset/min/max, tessellation factor min/max) the new shaders need. Narrowed `IssueDraw()`'s tessellation no-op skip so the adaptive-triangle case now flows through to real rendering. |
| `xenia-canary/src/xenia/gpu/spirv_shader_translator.cc`/`.h` | Finished the `// TODO(Triang3l): Barycentric coordinates and patch index.` gap — the guest domain shader now correctly receives barycentric coordinates (`gl_TessCoord`, with the empirically-required "ZYX" swizzle) and the patch control-point index in the right registers. |

### Test result
- Build: pass
- No regression confirmed on menu/cutscene content in both games
- **Not yet confirmed**: actual tessellated water/terrain rendering correctly — testing hasn't yet reached a qualifying scene. Other tessellation domain types/modes remain unimplemented and still no-op safely.

---

## [2026-07-05] — Ongoing investigations (no fix yet)

### What
Two separate, real bugs found during further testing, neither caused by the tessellation work above:
1. **NFS Carbon "stuck loading next level"**: confirmed via heartbeat logging that the GPU command processor is NOT hung — it continuously executes new command buffers and the on-screen loading animation genuinely progresses. The actual transition to the next level never fires, for far longer than the previously-normal 30-60s load. Narrowed to a likely game-logic-side condition/flag that never gets satisfied; not yet root-caused.
2. **Halo 3 deep-gameplay model/texture corruption**: reachable only once you get well past the opening cinematic (a jungle-canopy scene) — character models are visibly broken (e.g. a head displaced to the waist, a disconnected floating prop), alongside a burst of ~800 "texture fetch constant is completely invalid" warnings sharing an identical second word. Points to broken skeletal/bone transform data, not just texture binding. Confirmed pre-existing (unrelated to the tessellation work); likely never seen before because no earlier session reached this deep into gameplay.

### Files changed
- `xenia-canary/src/xenia/gpu/command_processor.cc`: added rate-limited (≤2/sec) heartbeat logging to `WorkerThreadMain` for the NFS Carbon investigation. Left in place — harmless, and useful if this needs revisiting.

### Test result
- Neither issue fixed yet. Both documented in detail for the next session (thread-dump/heartbeat-logging technique proved useful and is reusable for future "is this actually a hang" questions).

---

## Template for future entries

```
## [YYYY-MM-DD HH:MM] — Title

### What
<concise description of the change>

### Why
<motivation: license, bug, feature>

### Files changed
| File | Change |
|------|--------|
| ... | ... |

### Test result
- Build: pass/fail
- NFS Carbon launch: pass/fail
- Notes: ...
```
