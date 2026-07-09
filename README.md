# Xenia AE

An experimental **Xbox 360 emulator for Android**. It runs the Xenia Canary
emulator core with an ARM64 JIT and a simple, controller‑friendly Android UI.

> ⚠️ **Early and experimental.** Some games boot and play, others glitch or
> crash. This is a hobby project, not a finished product — expect rough edges.

---

## Minimum requirements

*(These are conservative starting numbers and will be refined as the emulator is
optimized.)*

| Requirement | Minimum |
|---|---|
| **Android version** | 9.0 (Pie) or newer |
| **CPU** | 64‑bit ARM (arm64‑v8a). 32‑bit devices are **not** supported. |
| **Graphics** | Vulkan 1.1 capable GPU (most Adreno 6xx / Mali G‑series and newer) |
| **RAM** | 6 GB (8 GB+ recommended) |

**For a playable experience**, a recent high‑end phone or handheld is strongly
recommended — a Snapdragon 8‑series (or equivalent) with an Adreno 700‑class GPU.
Emulating the Xbox 360 is heavy; lower‑end devices may boot games but run slowly.

Tested on: AYN Odin 2 (Snapdragon 8 Gen 2 / Adreno 740) and Motorola Moto G Play
(Adreno 610).

---

## Install

1. Go to the [**Releases**](../../releases) page.
2. Download the latest `.apk`.
3. On your device, open the file and allow installation from unknown sources if
   prompted.

You supply your own game files — none are included.

---

## How to use

1. Open **Xenia AE**.
2. On the **Games** tab, tap the **+** button to add a game file (`.iso`, `.xex`,
   or a game folder), or point it at a folder of games.
3. Tap a game's box art to launch it. Long‑press a game for more options
   (details, per‑game settings, box art, create a home‑screen shortcut).
4. While in a game, press **Back** for the pause menu (Resume / Settings / Exit).

Settings are split into a simple **Settings** tab for common options and an
**Advanced Settings** screen exposing the full emulator configuration.

---

## Build from source (for developers)

Requires Android Studio (or the Android SDK + NDK) and a machine set up for
Android native builds.

```bash
git clone https://github.com/r0mn-creator/Xenia-AE.git
cd Xenia-AE
./gradlew assembleDebug
```

The APK lands in `app/build/outputs/apk/debug/`. The first build is slow because
it compiles the entire native emulator core.

---

## Credits

Built on the work of others:

- [**Xenia**](https://xenia.jp/) and **Xenia Canary** — the Xbox 360 emulator core.
- The **aX360e** project — the original Android port and ARM64 JIT this project grew from.

Xenia AE keeps its own Android front‑end and applies fixes on top of the core.

## License

Xenia AE's own code is released under the [MIT License](LICENSE). The bundled
Xenia / Xenia Canary sources retain their original licenses — see the license
headers in the relevant source directories.
