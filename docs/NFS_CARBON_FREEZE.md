# NFS Carbon — main-menu freeze: characterisation (2026-07-31)

NFS Carbon is the project's canary for GPU-backend breakage, so it is run after
any GPU change. This documents what the freeze actually *is*, measured rather
than assumed.

## Config under test
Shipped defaults: all TESTRIG probes off, all AE precision fixes ON
(`fix_rsq`/`fix_sincos`/`fix_wclip`=1, `reginit`=0), **stock Qualcomm driver**
(no per-game override; global `vulkan_lib_path = 'default'`), shader cache
cleared. Odin 2 / Adreno 740.

## ★ It now reaches the MAIN MENU
Better than the previously recorded state ("freezes at first load screen" /
"freezes at main menu right before the network check"). Observed live:
attract sequence → garage → **main menu** (CAREER / MY CARS / CHALLENGE, Palmont
City Motors, "Ⓐ Accept" prompt). Then it stops.

## What the freeze IS (measured)
| signal | value |
|---|---|
| Game image | **completely static** - 4 frames over 15 s, **0 changed pixels**, empty diff bbox |
| Command processor | **still executing**, `advancing=true` |
| CP submission rate | **collapsed**: ~360/s while loading -> **~58/s** frozen |
| Errors / warnings | **0 / 0** |
| Guest kernel calls | **ZERO new** over 30 s (non-CP log line count frozen) |
| Guest main thread | spinning in `/dev/ashmem/xenia_code_cache_*` = **JIT guest code** |
| Worker threads | all benign: 3x `XObject::Wait`, 4x `XThread::Delay`, 1x `WaitMultiple` |
| Guest thread count | 19, all `running=true` |

⇒ **A guest-side LIVELOCK, not a hang and not a GPU fault.** The emulator is
healthy and still feeding the GPU a static frame; the game's own main thread is
polling a memory flag that nothing ever sets, and every worker is idle.

⚠️ **Measuring "frozen" correctly:** comparing raw screenshots is WRONG - the
debug overlay prints a live counter, so every frame differs and it looks like
motion. Crop the overlay strip (bottom ~12%) and compare only the game area.
Four frames 5 s apart is enough.

## ⚠️ Xbox Live polling — TESTED AND REFUTED (2026-07-31)
User's hypothesis - *"after loading the alias the game tries to connect to Xbox
Live"* - fits the evidence better than anything else.

**`xam` is only 68% implemented: 55 of 173 imports are stubs.** Among the
unimplemented ones:
```
NetDll_XNetGetConnectStatus      <- the classic "am I connected yet?" poll
NetDll_XNetConnect
NetDll_XnpLogonGetStatus
NetDll_XnpLogonGetQVals / SetQVals / SetQEvent / ClearQEvent   <- logon queue
NetDll_XNetQosLookup / XNetQosGetListenStats
NetDll_WSARecv / WSASend / WSAEventSelect
```
`XNetGetConnectStatus` and `XnpLogonGetStatus` are exactly what a title polls in a
loop while waiting for Live sign-in. If they return an unchanging "pending", the
guest spins forever.

**This matches the measured signature precisely:** guest main thread spinning in
JIT code, CPU burning, GPU re-submitting one static frame, ZERO errors - and
crucially **zero kernel-call log lines**, which is explained if the poll lands on
a stub that returns immediately without logging.

**TEST RUN — hypothesis REFUTED.** Added a `debug.canary.log_kernel_calls`
property-gated launch arg for `log_high_frequency_kernel_calls` and traced the
freeze. In a 15 s window at the stall, the log contains **exactly two things**:
`REENTER_DIAG_CP` (our own probe) and **273 `VdSwap` calls**. There is
**ZERO `NetDll`, `Xam`, `Xnp` or `XNet` activity** - the game polls no network
function at all. The unimplemented NetDll stubs are never reached.

### ★ What the trace DOES show: it is not a hang at all
```
i> F800000C VdSwap(A972E31C, 701EF310, FFCA3008, ...)
i> F800000C VdSwap(A979EFEC, 701EF310, FFCA3008, ...)   <- different buffer each call
```
`VdSwap` fires ~18x/second from `MainThread (F800000C)`, with a **different first
argument every call**, i.e. the render loop is turning normally and submitting
fresh buffers - yet the presented image is **pixel-identical** (0 changed pixels
over 15 s, overlay cropped).

⇒ **The game is ALIVE and rendering at ~18 fps. What is stalled is its
update/simulation logic**, not the emulator and not the presenter. It re-renders
the same scene forever because nothing in its state advances.

⇒ And because it makes **no kernel calls at all** while doing so, whatever it
waits on is **entirely guest-internal** - a flag set by another guest thread, not
a host service that could be stubbed or implemented. That rules out the whole
class of "implement missing kernel function" fixes.

### Where that leaves it
Three hypotheses now tested and dead: corrupt save, any save at all, Xbox Live
polling. The remaining shape is a guest-internal wait between guest threads -
consistent with the very first recorded diagnosis (*"a game-logic-side condition
or flag that never gets satisfied"*), now confirmed with much stronger evidence.
Next instrument would have to see INSIDE guest execution (JIT PC sampling of
MainThread to find the poll loop, then identifying the memory it reads and which
thread should write it) - the same class of problem as the Halo 3 hunt, and not
cheap.

## Superseded suspect
Last guest activity before going silent:
```
SAF_DiscImageDevice::ResolvePath(\content\0000000000000000\454107EC\00000001)
XamContentCreateEnumerator_entry: Adding: RO (Filename: ALIAS_RO)   x4
HostPathDevice::ResolvePath()                                        x3
```
It is enumerating **saved-game / profile content** and repeatedly resolving the
same content path. Consistent with "freezes right before the network check": the
menu wants a content-enumeration result that never arrives. This is CPU-side
kernel emulation (XAM content APIs), **not** the GPU backend - so it is
independent of the Halo 3 work.

## ⚠️ RETRACTED: the save file is NOT the trigger (corrected 2026-07-31)
User's hypothesis - *"I wonder if the save file isn't compatible anymore... can we
temporarily remove it and see if it pushes through"* - **CORRECT.**

The save lives under the PROFILE XUID, not the zero XUID the game resolves:
```
content/E0300000A360E000/454107EC/00000001/ALIAS_RO/ALIAS_RO   286764 bytes
content/E0300000A360E000/454107EC/Headers/00000001/ALIAS_RO.header  328 bytes
```
(dated 2026-07-26, written in an earlier session)

**Test:** backed the save up to `~/xeniatest/save_backups/` (tar, 7 entries),
then MOVED it aside on-device to `454107EC.disabled` (moved, not deleted - the
app recreates its own with correct ownership), cleared the shader cache and
relaunched.

| signal | WITH save | WITHOUT save |
|---|---|---|
| frame diff (overlay cropped) | `bbox=None`, **0 px changed** | **`bbox=(0,0,1920,950)`** every frame |
| 6-frame sequence over 48 s | identical md5s | **6 DISTINCT md5s, all transitions changed** |
| guest kernel calls | **0 new** in 30 s | **+40 in 10 s**, streaming |
| progress | stuck at main menu | **past the menu -> animated title -> LOADING** |
| errors | 0 | 0 |

⇒ ~~The freeze is caused by the existing save file.~~ **WRONG - RETRACTED.**

**Disproved by two follow-up tests the user ran:**
1. Created a BRAND-NEW alias ("ELI", 286764 bytes, freshly written by the game)
   -> **froze at the identical place.** So it is not a corrupt/stale save.
2. Ran with **NO alias at all** (chose "No" at the create-alias prompt)
   -> **still froze at the identical place.**

So the save is exonerated completely. The one run that got further (title screen ->
alias prompt) did so for some other reason - most likely it had simply not yet
reached the stall point, not because the save was absent. **A single passing run
is not evidence; the control test is.**

⚠️ NOT caused by this project's changes: a pristine `c3bccd37` build froze too.
The save was most likely written incompletely/corrupt in an earlier session (its
header is only 328 bytes and references content in `ALIAS_RO`), and the game's
load path never recovers from it - it polls forever instead of failing.

### What this means for users
A corrupt save silently bricks the title at the main menu with **zero errors
logged**. Worth handling: validate the content header on load and fall back to
"no save" rather than letting the guest spin. The per-game content folder is
`content/<XUID>/<TITLEID>/` under the storage root, so a "Clear Save Data" action
in the game's long-press menu would give users a way out.

## Next step
Map the spinning JIT PC back to a guest address and find what should write it.
⚠️ JIT frames have no usable unwind info on this platform, so `debuggerd` gives
only frame #00 - the guest call stack is not available that way. The
kernel-threads TESTRIG port is the better instrument.

## Tooling notes (both verified working this session)
- **Kernel-threads probe:** `setprop debug.canary.testrig.master 1` +
  `...testrig.kernel 1`, then `adb forward tcp:9935 tcp:9935` and read the
  socket. Lists every guest thread with id/name/priority/running. Toggles are
  LIVE (~250 ms), so it can be enabled *during* a freeze without losing state.
- `debuggerd -b <emu_pid>` + `llvm-addr2line` against the unstripped
  `app/build/intermediates/merged_native_libs/.../libe.so` (check the Build ID
  matches) symbolises host threads.
