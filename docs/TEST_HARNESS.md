# Canary AE: the test/debug harness build

## What this is

Canary AE (`org.xeniaae.canary`) is a build of Xenia AE with all diagnostic,
instrumentation, and test-only code included and turned on. It installs
alongside the stable Xenia AE app (`org.xeniaae`) on the same device, so both
can be run and compared side by side.

- **Xenia AE** (`main` / `gpu-backport` branches, package `org.xeniaae`) —
  clean, optimized, shippable. No test code, no diagnostic overlays, no debug
  ports. Only receives a change once it has been proven correct in Canary AE.
- **Canary AE** (`canary-ae` branch, package `org.xeniaae.canary`) — the wide
  -open test rig. Every subsystem (GPU command processor, shared memory,
  texture cache, audio/APU, CPU/JIT) can be observed live while a game runs.
  All test code stays here permanently — it is not throwaway scaffolding to
  delete after a bug is fixed, it is the point of this build.

When a real fix is found and verified here, it is ported over to Xenia AE by
itself, without the surrounding instrumentation.

## Labeling convention for test/debug code

Every piece of test-only code — a log line, an instrumentation block, a debug
socket handler, an overlay field — must be tagged so it is unambiguous at a
glance that it is test scaffolding and not production logic, and searchable so
it can be found, ported, or stripped as a unit.

Tag format: `TESTRIG(<area>): <one-line reason>` in a comment directly above
the code it labels, where `<area>` is a short lowercase slug for the subsystem
being probed (e.g. `gpu`, `memexport`, `audio`, `jit`, `mem`).

```cpp
// TESTRIG(memexport): magic-marker write to distinguish "address computed
// wrong" from "address valid but exported data is zero" — see docs entry
// for the Halo 3 compute-memexport investigation.
if (IsSpirvComputeShader()) {
  StoreUint32ToSharedMemory(...);
}
```

```java
// TESTRIG(audio): expose XMA decoder queue depth on the debug overlay.
overlay.set("xma_queue", decoder.getQueueDepth());
```

Rules:
- One `TESTRIG(...)` comment per logical block, not per line.
- `<area>` should match one of the subsystem names used by the monitor ports
  (see below) so a probe and its live-inspection counterpart are easy to
  associate.
- Do not remove existing `DEBUG(halo3-vtx)` tags from code already carried
  over from Xenia AE — they are being migrated to `TESTRIG(...)` incrementally,
  not all at once. New test code should use `TESTRIG(...)` from the start.
- Never let a `TESTRIG(...)` block change behavior on the `org.xeniaae`
  (Xenia AE) package — this is enforced by keeping this code out of the
  branches that ship as Xenia AE, not by runtime package checks.

## Monitor ports — looking inside while a game runs

Every subsystem exposes its live state on a localhost-only TCP port. From a
host machine:

```
adb forward tcp:9931 tcp:9931   # pick the port(s) you want, see table below
nc 127.0.0.1 9931                # or: bash -c 'exec 3<>/dev/tcp/127.0.0.1/9931; cat <&3'
```

The connection stays open and gets a fresh text snapshot every 500ms,
separated by a `---` line. Ports never bind beyond `127.0.0.1` — they are not
reachable off the device even on a shared network.

| Port | Subsystem | What it shows |
|------|-----------|----------------|
| 9931 | `gpu`     | Device info, whether compute-memexport is active, live draw/dispatch counters |
| 9932 | `audio`   | AAudio stream state, xrun count, actual (not just requested) performance/sharing mode, buffer depths, Xenia's own frame queue |
| 9933 | `jit`     | arm64 JIT: functions compiled, code cache used/total bytes |
| 9934 | `mem`     | Per-heap guest memory page usage (all 8 virtual/physical heaps) |
| 9935 | `kernel`  | Live guest thread listing: id, name, running/guest/main flags, priority |

## Toggling — zero overhead when just playing, full visibility on demand

Every port (and, where relevant, the hot-path counters that feed it) can be
flipped on or off from the host with `adb shell setprop` — no reinstall, no
restart, takes effect within a fraction of a second. This uses Android's
`debug.*` system property namespace, which any `adb shell` can set without
root.

```
adb shell setprop debug.canary.testrig.master 0   # disable EVERYTHING
adb shell setprop debug.canary.testrig.gpu 0      # disable just the GPU port
adb shell setprop debug.canary.testrig.audio 0    # disable just the Audio port
adb shell setprop debug.canary.testrig.master 1   # re-enable (this is also the default)
```

Nothing set = fully enabled (matches how this build has always behaved).
Setting a property to `0` or `false` disables it; any other value (or leaving
it unset) means enabled.

Disabling a subsystem does two things:
1. Its port stops returning live data — a connected client sees a short
   "DISABLED, re-enable with: ..." message instead.
2. Any hot-path instrumentation gated with `HotPathEnabledCached()` (currently
   just the GPU draw/dispatch counters, the only always-on per-frame
   overhead this harness adds) stops running. This is what actually lets you
   play with zero added cost when you don't need the visibility, per the
   priority below.

**Priority is accuracy over speed.** When a subsystem is enabled, its
instrumentation should favor being trustworthy and complete over being cheap —
don't shy away from adding synchronization or more detail to a port if it
makes the numbers more reliable. The toggle exists so that cost is opt-in, not
so the "on" path has to be lightweight.

See `xenia/base/testrig_debug_server.h` (in
`app/src/main/cpp/xenia-canary/src/xenia/base/`) for the implementation this
table and toggle behavior are backed by.

## Status

All 5 subsystems above are wired and verified working simultaneously
on-device. Not yet done: an expanded on-screen overlay (ports were prioritized
over this), and migrating existing `DEBUG(halo3-vtx)` tags to `TESTRIG(...)`
(deferred — old tags stay as they are). See project memory
`project_xenia_ae_canary_as_test_harness` for full history and next steps.
