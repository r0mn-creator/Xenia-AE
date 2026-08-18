# Xenia-AE launch intent — front-end integration

How to boot a game in Xenia-AE from another app (Daijishō, ES-DE, Beacon, a
launcher, a script). Stable contract; changes here are additive.

## The short version

```
component  org.xeniaae.aex/org.xeniaae.EmulatorActivity
action     android.intent.action.VIEW
data       content://…            ← the ROM, as a content URI
type       application/octet-stream
flags      FLAG_GRANT_READ_URI_PERMISSION
```

No extras are required. This is the same shape Eden and most Android emulators
accept, so a front-end that already supports them needs no new code path —
only a new entry in its emulator list.

## Package IDs

Three builds install side by side. Target whichever the user has.

| Build | Package | Activity |
|---|---|---|
| Xenia-AE (stable) | `org.xeniaae` | `org.xeniaae.EmulatorActivity` |
| Canary | `org.xeniaae.canary` | `org.xeniaae.EmulatorActivity` |
| Canary AEX | `org.xeniaae.aex` | `org.xeniaae.EmulatorActivity` |

The activity class is `org.xeniaae.EmulatorActivity` in all three — the Java
namespace stays `org.xeniaae` even where the application ID differs. So the
component is `<package>/org.xeniaae.EmulatorActivity`.

**Availability:** this contract works from **0.4.0-aex** onward. Stable 0.2.0
and canary 0.3.0-canary declare the `VIEW` filter but do not implement it —
they will show "Can't open game". Detect by package version, or just let the
user pick.

## Launch forms

All four resolve to the activity. Only the first is verified end to end.

| Form | Status |
|---|---|
| `content://` + `application/octet-stream` | **Verified — boots** |
| `content://` + `*/*` | Resolves |
| `content://`, no type | Resolves |
| `file://` ending `.iso` / `.xex` / `.zar` | Resolves, but see below |

**Do not send `file://`.** Passing a file URI between apps has thrown
`FileUriExposedException` since Android 7, and Xenia-AE holds neither
`READ_EXTERNAL_STORAGE` nor `MANAGE_EXTERNAL_STORAGE`, so it cannot open a raw
path even if you get one to it. The filter exists for completeness. Use a
`content://` URI from SAF or your own `FileProvider`.

## Permission

Xenia-AE reads the ROM through `ContentResolver`, so the URI must be readable
by it. Set `FLAG_GRANT_READ_URI_PERMISSION` on the intent. A SAF URI the user
picked in your app, or one from your own `FileProvider`, both work.

The emulator core runs in a separate `:emu` process, but URI grants are made to
the receiving **UID**, so a single grant covers both processes. Nothing extra
is needed.

## Kotlin

```kotlin
val intent = Intent(Intent.ACTION_VIEW).apply {
    setClassName("org.xeniaae.aex", "org.xeniaae.EmulatorActivity")
    setDataAndType(romUri, "application/octet-stream")
    addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
}
startActivity(intent)
```

Omit `setClassName` to let the user choose among installed builds.

## adb

```sh
adb shell am start \
  -a android.intent.action.VIEW \
  -d 'content://media/external/file/1000069042' \
  -t application/octet-stream \
  --grant-read-uri-permission \
  -n org.xeniaae.aex/org.xeniaae.EmulatorActivity
```

## Optional extras

None are required. Supply them to get behaviour the URI alone cannot express.

| Extra | Type | Effect |
|---|---|---|
| `game_uri` | String | ROM URI. Takes precedence over the intent data. Used by Xenia-AE's own library; a front-end should prefer the data URI. |
| `game_title` | String | Display name. Defaults to the file name. Not cosmetic — see below. |
| `game_title_id` | String | XEX title ID, e.g. `4D5307E6`. Selects the per-game config and per-game GPU driver. Without it those are skipped. |
| `precache_mode` | boolean | Boots a 120 s shader pre-cache session instead of playing. Not for normal launching. |

`game_title` is worth sending if you have it. When a stored URI stops
resolving — MediaStore reassigns row IDs on re-index, reboot, or file move —
Xenia-AE searches for the file by this name and recovers. With no title it can
only report failure.

`game_title_id` is what unlocks per-game settings. A user who has configured a
specific GPU driver for a title gets it only when the launch carries the ID.

## The custom action

```
org.xeniaae.intent.action.EMULATE
```

Also accepted, with the same extras. It is what Xenia-AE's own library uses,
and it carries the ROM in `game_uri` rather than the data URI. There is no
reason for a front-end to prefer it — `ACTION_VIEW` is the portable form and
gets the same result.

## Behaviour notes

- The activity is `singleTask`. Launching a second game replaces the first.
- Emulation runs in the `:emu` process; the activity returns immediately.
- No result is delivered. `startActivityForResult` will return nothing useful.
- If the URI cannot be opened, the user gets a recoverable dialog rather than a
  crash or a silent black screen.
