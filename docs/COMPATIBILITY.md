# Tested games

Games actually run on hardware, with what works and what does not.

Only titles someone has really played are listed — nothing is added from
assumption. If a game is not here, it has not been tested, which is not the same
as "does not work".

**Test device unless stated:** Odin 2 Portal (Snapdragon 8 Gen 2, **Adreno 740**),
Android 13.
**Build:** Canary AE `v0.3.0-canary`.

Legend — **Loads**: reaches the game's own menus · **Playable**: can be played
through normally · ⚠️ = partly, with caveats · ❌ = no · — = not tested.

| Game | Title ID | Loads | Playable | Issues |
|---|---|---|---|---|
| Need for Speed: Carbon | `454107EC` | ✅ | ✅ | None known. Completes races; audio and level loading fixed in v0.3.0. |
| Metal Gear Rising: Revengeance | `4B4E080A` | ✅ | ✅ | None reported — appears fully playable. |
| Geometry Wars: Retro Evolved | `584B87F0` | ✅ | ✅ | None reported — "works like a champ". Launched from the Xbox Live Arcade *Experience Disc* (`584107ED`), which itself loads fine. |
| Halo 3 | `4D5307E6` | ✅ | ⚠️ | Runs and is controllable, but **character models collapse** into a spiky ball, and the **main-menu vista renders upside-down**. Needs a **Turnip** driver (the stock Qualcomm driver gives a flat navy menu) and currently an **Adreno 700-series** GPU. |
| Halo 4 | `4D530919` | ⚠️ | ❌ | Boots, but **crashes before reaching the main menu**. Not yet diagnosed. |
| Need for Speed: The Run | `4541094A` | ⚠️ | ❌ | Starts, but **crashes before gameplay begins**. Not yet diagnosed. |
| Need for Speed: Most Wanted | `454107D9` | — | — | **Not tested yet** — present in the library but never launched. |

## Notes on the Halo 3 issues

The model collapse is **not** a fault in this emulator's engine:

- The **desktop build renders Halo 3 correctly** on an AMD/RADV GPU — same
  emulator lineage, different graphics driver.
- **XenDroid**, an unrelated third-party Android port, **fails the same way** on
  the same device.
- It reproduces on **every** Adreno driver tried — stock, Turnip a7xx, Turnip
  a6xx.

⇒ It is an **Adreno-side** problem, so it is unlikely to be fixed by changes here
alone.

**What it looks like, if you hit it in another game:** the environment renders
correctly, but *animated* models — characters, creatures — collapse to a single
point or a spiky ball. Static geometry is fine.

**Why it is probably uncommon:** it comes from **MEMEXPORT**, an Xbox 360 GPU
feature where a vertex shader writes results back to memory to be re-read as
geometry, used for GPU skinning. Halo 3 relies on it heavily for character
animation; most titles skin on the CPU or in an ordinary vertex shader and never
touch that path. That matches what has been seen so far — the other tested games
are unaffected.

## The two early crashes (Halo 4, NFS: The Run)

Recorded for the record only — **neither has been investigated yet**, and no
log was captured at the time of the crash. Both get further than "won't start":
each reaches the point of doing real work and then dies, Halo 4 before its main
menu and The Run before gameplay.

They are grouped here because the shape is the same and they may or may not
share a cause — nothing so far says they do. **Do not assume a single fix.**

When either is picked up, the first step is a log from the crash itself:

```
adb -s <serial> logcat -c
# launch, reproduce the crash, then:
adb -s <serial> logcat -d > crash.log
adb -s <serial> shell "run-as org.xeniaae.canary cat files/xeniaae/xe.log" > xe.log
```

Both titles are later and heavier than NFS Carbon, so a crash before first
render is as likely to be an unimplemented kernel/XAM path or a shader
translation failure as anything graphical. That is a guess, not a finding.

## Known issue affecting all titles

- Sending the app to the background ends the emulator process; the game does not
  survive being backgrounded. Pause/resume across app switches is not implemented
  yet.

## Hardware guidance (as of 2026-08-02, expected to improve)

- **Adreno 700-series** — the reference target; everything above was tested here.
- **Adreno 600-series** — runs lighter titles. NFS Carbon was proven first on an
  **Adreno 610**, the weakest 600-series part. Halo 3 hard-hangs on the 610
  (`VK_ERROR_DEVICE_LOST`) on every driver tried.
- Treat this as "where things stand today", **not** a minimum-spec statement.
  Part of the library runs on 600-series hardware.

## Contributing a result

Please include: game name, title ID if known, device and GPU, driver (stock or
which Turnip build), and whether it **loads**, is **playable**, and what breaks.
A precise description of the failure is worth far more than "doesn't work" —
"characters collapse into a ball while the level renders fine" is what let the
issue above be traced to a specific GPU feature.
