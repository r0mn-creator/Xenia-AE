# Custom GPU drivers on Android — exact paths, commands, and compatibility rules

Practical reference for getting a custom Vulkan (Mesa Turnip) driver loading in
Canary AE / Xenia AE across many devices. Written after debugging a silent
failure on a OnePlus 7 Pro (2026-07-29) where everything *looked* correct and the
emulator still ran on the stock driver.

---

## 1. How driver loading actually works

The driver is loaded by **adrenotools**, in
`src/xenia/ui/vulkan/vulkan_instance.cc`:

```cpp
std::string custom_lib_path = cvars::vulkan_lib_path;
if (!custom_lib_path.empty() && std::filesystem::exists(custom_lib_path)) {
    // split the full .so path into dir + filename
    adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr,
                               hook_dir, custom_lib_dir, custom_lib_name, ...);
} else {
    dlopen(loader_library_name);   // stock /vendor/lib64/hw/vulkan.adreno.so
}
```

`vulkan_lib_path` is the **full path to the `.so`**, not a directory.

### Timing constraint (do not fight this)
`vulkan_lib_path` is consumed during `Emulator::Setup()` when the Vulkan
instance is created — **long before** the per-game config is read in
`CompleteLaunch()`. A per-game TOML entry for the driver is therefore silently
ignored. The per-game driver is passed as a **launch argument** instead
(`--vulkan_lib_path=...`), which also wins over both config files because cvar
precedence is `commandline > game_config > config` (`base/cvar.h`).

---

## 2. On-device paths

| What | Path |
|---|---|
| Installed drivers | `/data/data/<pkg>/xeniaae/driver/<Name>/` |
| Driver files | `<Name>/meta.json` + `<Name>/vulkan.*.so` |
| Storage root | `/storage/emulated/0/Android/data/<pkg>/files/xeniaae/` |
| Emulator log | `<storage_root>/xe.log` |
| Global config | `<storage_root>/xenia-canary.config.toml` |
| Per-game driver pref | `/data/data/<pkg>/shared_prefs/xenia_prefs.xml`, key `game_driver_<TITLEID>` |
| Game ISOs | `/sdcard/Download/360/` |

`<pkg>` = `org.xeniaae.canary` (Canary AE) or `org.xeniaae` (Xenia AE).

⚠️ Both `/data/data/<pkg>/...` and `/data/user/0/<pkg>/...` work — they resolve to
the same place. Either form is fine in `vulkan_lib_path` (verified).

---

## 3. Verifying a driver actually loaded

**Do NOT trust the config dump in `xe.log`.** The log prints the *config file*
contents, so `vulkan_lib_path = ""` appears even when a launch-arg override is
active. That line proves nothing.

Use these three, in order of authority:

### a) `driverName` in `xe.log` — the clearest signal
```bash
adb -s <serial> shell "su -c 'grep -iE \"Vulkan device .|driverName\" \
  /storage/emulated/0/Android/data/<pkg>/files/xeniaae/xe.log'"
```
| Output | Meaning |
|---|---|
| `driverName: Qualcomm Technologies Inc. Adreno Vulkan Driver` | **stock** driver |
| `driverName: PurpleVK public driver` / a Mesa name | custom driver active |

Stock also reports a much older API (e.g. `API 1.1.128`) than Turnip
(`API 1.4.344`) — another quick tell.

### b) The mapped `.so` in the **`:emu`** process
```bash
PID=$(adb -s <serial> shell pidof <pkg>:emu | tr -d '\r')
adb -s <serial> shell "su -c 'grep -oE \"[^ ]*vulkan[^ ]*\\.so\" /proc/$PID/maps | sort -u'"
```
⚠️ **Must be the `:emu` process.** The emulator runs in a separate process; the
UI process always shows the stock driver and looks like a failure.

### c) adrenotools' own hook log (the only place failures appear)
```bash
adb -s <serial> logcat -c
# launch the game, then:
adb -s <serial> logcat -d | grep -iE "hook_impl|AdrenoVK-0: Driver Path"
```
Success looks like:
```
hook_impl: hook_android_dlopen_ext: loading custom driver: /data/data/.../vulkan.purple.so
```
Failure looks like:
```
hook_impl: hook failed: failed to load custom driver: dlopen failed: ...
hook_impl: falling back!
AdrenoVK-0: Driver Path : /vendor/lib64/hw/vulkan.adreno.so
```

---

## 4. ★ The compatibility trap: bionic symbols, not GPU generation

**adrenotools silently falls back to the stock driver when the custom `.so`
fails to `dlopen`.** Xenia logs nothing, Vulkan comes up fine, and the only
evidence is the `hook_impl` logcat line above. This is the single most
confusing failure mode.

Real case: **MrPurple T29** on Android 12 (API 31):
```
dlopen failed: cannot locate symbol "pthread_getaffinity_np"
```
`pthread_getaffinity_np` does not exist in older Android bionic. The driver was
built against a newer NDK/bionic, so it cannot load — **regardless of GPU**.

### `meta.json` `minApi` is NOT trustworthy
T29 declares `"minApi": 30` but genuinely requires a much newer bionic. Do not
gate on that field alone.

### Reliable pre-flight check (host side)
```bash
unzip -o -q driver.zip -d x
readelf --dyn-syms -W x/vulkan.*.so | grep "UND.*pthread_getaffinity_np"
```
Any hit ⇒ will fail on older Android. Measured across builds:

| Driver build | `minApi` claims | needs `pthread_getaffinity_np` | Loads on Android 12 |
|---|---|---|---|
| MrPurple **T29** | 30 | **yes** | ❌ falls back to stock |
| MrPurple **T26** | 30 | no | ✅ loads |
| MrPurple **T24** | 30 | no | ✅ loads |
| K11MCH1 Turnip R8 | 27 | no | ✅ loads |
| Qualcomm v805 | 27 | no | ✅ loads |

---

## 4b. ★ Second trap: loading ≠ working (Turnip's KGSL ioctl requirements)

A driver can `dlopen` fine and still render **nothing**. Newer Mesa uses a
VM_BIND-style ioctl on the downstream Qualcomm **KGSL** kernel driver that older
device kernels do not implement.

Observed on the OnePlus 7 Pro / Android 12 with **MrPurple T26** (Mesa 26.1):
```
w> Vulkan Warning (../src/freedreno/vulkan/tu_knl_kgsl.cc:287):
     GPUMEM_ALLOC_ID failed (Out of memory) (VK_ERROR_OUT_OF_DEVICE_MEMORY)
w> Vulkan Warning (../src/freedreno/vulkan/tu_knl_kgsl.cc:1539):
     bind submit failed: Inappropriate ioctl for device
!> VulkanPresenter: Failed to submit command buffers
```
`Inappropriate ioctl for device` is `ENOTTY` — the kernel has no such ioctl. The
"Out of memory" line is a *consequence*, not real memory pressure (the device has
8 GB). Result: audio plays, the guest runs and submits draws, screen stays black.

**Dropping one Mesa minor version fixed it: MrPurple T24 (Mesa 26.0) renders.**

So the driver-selection rule is a *two*-part check:
1. Does it `dlopen`? → `hook_impl` in logcat (§3c)
2. Once loaded, does it submit? → grep `xe.log` for `tu_knl`, `VK_ERROR`,
   `Failed to submit command buffers`

Newest is not best. On older kernels, prefer an older Mesa.

### Working combination found (2026-07-29)
**OnePlus 7 Pro, Adreno 640, Android 12 → MrPurple T24.** Renders Halo 3 with no
`tu_knl`/`VK_ERROR`/submit errors. (T26 loads but cannot present; T29 cannot even
load.)

---

## 4c. ⚠️ Diagnostic probes default to ON on a fresh device

The TESTRIG probes are gated on `debug.canary.testrig.*` system properties, and
when those properties are **unset** the probes run. A brand-new device therefore
logs `ANYDRAW` / `TEXREQUEST` / `RESOLVE` on **every draw** — measured at ~4,500
lines/sec, 204,000 log lines in 45 s — which alone can make a game look broken.

Turn them off before judging any rendering result:
```bash
for k in master gpu audio jit mem kernel; do
  adb -s <serial> shell "setprop debug.canary.testrig.$k 0"
done
adb -s <serial> shell "getprop | grep canary"   # verify
```
This must be inverted before shipping to end users — see §6.

### GPU-generation support
- **MrPurple666/purple-turnip** builds are **unified A6XX / A7XX / A8XX** — use
  these for Adreno 6xx. Confirmed: T26 runs on **Adreno 640**.
- **K11MCH1/AdrenoToolsDrivers** Turnip builds are **a7xx/a8xx focused**; their
  binaries contain no a6xx device names (only `Adreno 7c+ Gen 3`, `Adreno 8c
  Gen 3`, `Adreno X1-*`). Fine for the Odin 2 (Adreno 740), wrong repo for 6xx.
- ⚠️ Searching a driver binary for `a6xx` strings is **inconclusive** — a7xx-only
  builds still contain `a6xx` *instruction-encoding* tables (shared ISA), and
  Mesa generates most Adreno device names from the chip ID at runtime rather
  than storing literals. Test on-device instead.

---

## 5. Installing a driver by hand (for testing)

Normally use the in-app importer (Settings → Drivers, or long-press a game →
Game GPU Driver). For scripted testing:

```bash
D=<serial>; PKG=org.xeniaae.canary; NAME=MrPurple_T26
DD=/data/data/$PKG/xeniaae/driver/$NAME
U=$(adb -s $D shell "su -c 'stat -c %U /data/data/$PKG'" | tr -d '\r')   # e.g. u0_a2

unzip -o -q driver.zip -d drv
adb -s $D push drv/vulkan.purple.so drv/meta.json /sdcard/
adb -s $D shell "su -c '
  mkdir -p $DD &&
  cp /sdcard/vulkan.purple.so /sdcard/meta.json $DD/ &&
  chown -R $U:$U /data/data/$PKG/xeniaae/driver &&
  chmod 700 $DD && chmod 600 $DD/* &&
  restorecon -R /data/data/$PKG/xeniaae/driver'"
```

### ⚠️ Ownership + SELinux are mandatory
- `chown` to the **app's** uid. Files left owned by `shell` or `root` are
  unreadable/unwritable by the app.
- `restorecon` matters on **Enforcing** devices: the file needs
  `u:object_r:app_data_file:s0:c<N>,c256,c512,c768` — the bare
  `...app_data_file:s0` context produced by a root copy lacks the app's category
  labels. A correct install shows `avc: granted { execute }` in logcat.
- `run-as <pkg>` works for app-**internal** storage (`/data/data/...`) and gives
  correct ownership automatically, but it **cannot read `/sdcard`**, so it can't
  be used to copy a pushed file in. Root-copy + `chown` + `restorecon` is the
  working recipe.
- Never hand-write app **config** or **patch** files with `adb push` / bare `adb
  shell` — see `EmulatorSettings`' self-heal and the patch-folder notes; the app
  must be the writer.

---

## 6. "It just works" — required UX guardrails

The current behaviour is a bad experience for a non-technical user: they pick a
driver, adrenotools silently falls back, and the app still reports the driver as
selected while rendering with stock. Known gaps to close:

1. **Detect the fallback.** After device creation, if a custom driver was
   requested but `driverName` is the Qualcomm stock string, treat it as a failed
   load — log an error and expose a flag to the UI.
2. **Tell the user plainly**, e.g. *"Turnip T29 isn't compatible with this
   device's Android version — using the built-in driver."* Silence is the bug.
3. **Filter the download list** by a real compatibility check (undefined-symbol
   scan against the running bionic), not `meta.json`'s `minApi`.
4. **Remember bad pairings** per (driver, device) so a known-bad driver is
   marked in the picker instead of being silently re-tried.

---

## 7. Device notes

| Device | Serial | GPU | Android | SELinux | Working driver |
|---|---|---|---|---|---|
| Odin 2 Portal | `3a478943` | Adreno 740 (a7xx) | — | Permissive | K11MCH1 Turnip R8 |
| OnePlus 7 Pro (GM1917) | `baa95977` | Adreno 640 (a6xx) | 12 (API 31) | **Enforcing** | **MrPurple T26** |
| moto | `ZL83246JRD` | Adreno 610 (a6xx) | — | — | none — GPU hangs on Halo 3 |

**OnePlus / OxygenOS quirk:** `appops` and `pm grant` are blocked for the shell
user (`OplusAppOpsService`: `uid 2000 does not have MANAGE_APP_OPS_MODES`). Run
them via `su -c` instead:
```bash
adb -s baa95977 shell "su -c 'appops set org.xeniaae.canary MANAGE_EXTERNAL_STORAGE allow'"
adb -s baa95977 shell "su -c 'pm grant org.xeniaae.canary android.permission.READ_EXTERNAL_STORAGE'"
```

**After pushing an ISO**, trigger a media scan or the in-app picker won't see it:
```bash
adb -s <serial> shell "su -c 'am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE \
  -d file:///sdcard/Download/360/Halo3.iso'"
```
