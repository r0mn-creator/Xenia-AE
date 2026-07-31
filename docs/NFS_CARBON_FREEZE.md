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

## Leading suspect
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
