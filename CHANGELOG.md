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
