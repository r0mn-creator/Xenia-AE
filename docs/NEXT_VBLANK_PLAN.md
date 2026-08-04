# Next session: fix the vblank flood

**Start here.** Everything below is written to be picked up cold.

## The bug, in one paragraph

`gpu/graphics_system.cc`, frame limiter worker (~line 200). With `vsync=false`
and `framerate_limit=0` the loop calls `MarkVblank()` then `Sleep(1ms)`, so the
guest receives vblank interrupts at **~1000 Hz instead of 60 Hz** — about 16x
real hardware. Vblank is how a 360 game paces itself, so this corrupts the clock
the game's own logic runs against.

**Confirmed on-device:** in NFS Carbon, a character says "press Y" and the button
icon appears noticeably late. The timing is demonstrably wrong.

```cpp
if (!cvars::vsync) {
  MarkVblank();
  if (normalized_framerate_limit > 0) {
    xe::threading::NanoSleep(1000000000 / normalized_framerate_limit);
  } else {
    xe::threading::Sleep(std::chrono::milliseconds(1));   // <-- ~1000 Hz vblank
  }
}
```

Note the asymmetry: the `vsync=true` path paces `MarkVblank()` properly against
`vsync_duration_d`. The `vsync=false` path does not pace it at all.

## Why this is worth doing even if FPS does not move

The user's reasoning, and it is sound: the guest is currently servicing ~16x more
vblank interrupts than it should. Every one of those is guest code running an
interrupt handler — real CPU spent on work that should not exist. Removing it
frees cycles for actual game commands. Even a small gain compounds with the other
candidates.

Two outcomes, both useful:
- **Timing fixed AND FPS holds near 12.18** -> the +24.8% was real, and we also
  fixed a correctness bug. Bank it.
- **Timing fixed AND FPS drops toward ~10** -> the +24.8% was largely an artifact
  of the broken clock. Painful but important to know before building on it.

## The fix

Decouple **vblank rate** from **frame limiting**. Vblank should fire at the
display rate the guest expects (60 Hz) regardless of whether the host is
throttling presentation. `vsync` should control presentation/throttling, not
whether the guest's display interrupt is emulated correctly.

Sketch:

**Do NOT hardcode 60.** The accessor already exists — use it:

```cpp
// kernel/xboxkrnl/xboxkrnl_video.cc:52
inline const static float GetVideoRefreshRate() {
  return cvars::use_50Hz_mode ? 50.0f : 60.0f;
}
```

It is driven by the existing **Settings -> Video -> use_50Hz_mode** toggle, so
PAL titles are handled by a setting the user can already reach, and it can be
overridden per-game via `config/<title_id>.config.toml` under `[Video]` exactly
like Halo 3's `internal_display_resolution` already is. That is the intended
design: automatic from the setting, per-game override when a title needs it.

```cpp
// Vblank is a GUEST-VISIBLE clock. It must tick at the rate the title expects,
// whether or not the host is limiting frames. Tying it to cvars::vsync made it
// fire at ~1000 Hz with vsync off, which broke in-game timing (Carbon's
// "press Y" prompt appeared late).
const double vblank_hz = GetVideoRefreshRate();   // 50 or 60, from the setting
```

Pace `MarkVblank()` against that in **both** branches; keep `vsync` controlling
only the sleep/throttle behaviour.

**Wiring note:** `GetVideoRefreshRate()` is an `inline static` in
`xboxkrnl_video.cc`, so it is not visible outside that translation unit as
written. `graphics_system.cc` already calls `GetInternalDisplayResolution()`
from the same area, so check how that one is exposed and follow the same
pattern — either promote the accessor to the header or read
`cvars::use_50Hz_mode` directly. Prefer the accessor so there is one source of
truth.

**Make it a toggle** (project standing rule): `debug.canary.vblank_fix`, default
ON once verified, so it can be bisected against without a rebuild.

## How to verify

1. **Timing first, FPS second.** The "press Y" prompt delay in Carbon is the
   cheapest regression test — it is visible without instrumentation.
2. **Then re-measure:**
   ```
   ./scripts/fps_bench.sh record vblank_fixed 180
   ./scripts/fps_bench.sh compare vsync_off_180 vblank_fixed
   ```
   **180 s, not 60 s** — see the sensitivity correction in ENGINE_CHANGE_LOG.md.
   At the real spread (sd=2.16) a 60 s run misses a genuine 10% change 3 times
   in 10.
3. **Re-test other titles.** This touches every game. Geometry Wars and Halo 3
   at minimum.

## Housekeeping still owed

- **Flip global `vsync` back to `true`** (Settings -> GPU). It is still `false`
  from a UI change, so Carbon's per-game override is not actually being
  exercised.
- Existing baselines in `.bench/`: `baseline` (vsync ON, 60 s),
  `vsync_off` (60 s, unrepresentative), `vsync_off_180` (the real reference).

## Still open, not part of this task

- **The ~12.6 FPS ceiling.** 95 of 166 samples piled on 12 FPS, max 12.62.
  Ruled out: display sync (swapchain is MAILBOX, does not block on refresh) and
  the frame limiter (free-runs with vsync off). Remaining: genuine CPU plateau,
  or something guest-side in Carbon.
- **The flat CPU profile.** 1012 distinct symbols, top 0.48%; GPU command thread
  is only 24.6% of process CPU while JIT'd guest code is 57.3%. No single fix
  wins. Largest actionable blocks: RT cache 7.4% + texture cache 6.5% (the
  EDRAM round-trip already flagged in RENDER_PIPELINE_AUDIT.md), descriptors
  4.86% plus an unknown share of the Adreno driver's 22%.
