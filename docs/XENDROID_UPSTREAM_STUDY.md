# XenDroid upstream study (2026-08-10)

Source: `rfandango/XenDroid` (224 stars), our fork `r0mn-creator/xendroid-fork`
is behind - fork last pushed 2026-07-30, upstream pushed **2026-08-10**.

They are working on **the same open problems we are**, and in several cases
they have shipped something for a bug we only diagnosed. Notably they track
**`xenia-edge`**, a more active upstream than the canary we diff against.

Depth marker: ✅ = read the actual diff. 📄 = commit subject only.

---

## 1. ✅ Our ISB measurement is INVALIDATED - re-test required

`ee030d15 [A64] Emit isb for db16cyc instead of yield` touches the **same file
and same emitter** we changed: `a64_seq_memory.cc`, `DELAY_EXECUTION`.

We measured that change as a **21.5% regression** (NFS Carbon, 180 s,
9.67 -> 7.59 FPS, p=0.0000) and reverted it with a "do not fix this again"
comment. **That conclusion does not apply to their implementation.**

Their emitter **coalesces consecutive barriers** - it checks whether the last
emitted instruction is already an `isb` and returns without emitting a second:

```cpp
constexpr uint32_t kIsbSy = 0xD5033FDFu;
if (e.getSize() >= sizeof(uint32_t) &&
    *reinterpret_cast<const uint32_t*>(e.getCurr() - sizeof(uint32_t)) == kIsbSy) {
  return;                 // sled collapses to ONE isb
}
e.isb(Xbyak_aarch64::SY);
```

Critically, when they switched `yield` -> `isb` they **also updated the
comparison constant** (`0xD503203F` yield -> `0xD5033FDF` isb).

**Our `DELAY_EXECUTION` has no coalescing whatsoever** - verified, there is no
`getCurr()`/opcode check anywhere in the file. So during our test:

| | per db16cyc sled |
|---|---|
| XenDroid | **1** isb |
| Xenia-AE (as measured) | **8** isbs |

`guest_826DEFD0`'s pause is eight consecutive delay ops, so we benchmarked
**eight full pipeline flushes per loop iteration** against eight free no-ops.
That is not a test of "isb instead of yield" - it is a test of an unbatched
sled. The regression is real for *our* code and the revert was correct, but the
underlying idea was never actually evaluated.

**Action: re-test isb WITH coalescing.** Cheap, and it is one of the few
candidates aimed at the 14% hotspot.

---

## 2. 📄 They shipped fixes for our biggest open CPU finding

`guest_826DEFD0` (14.08% of all process CPU, a guest polling loop) is our top
performance lead. They have landed, in the last week:

- `edaf74cd [CPU] Park indefinite guest memory-poll loops with the adaptive spin backoff`
- `da6b36eb [CPU] Collapse memory-counter delay countdowns, without the bare-poll throttle`
- `ecd38cfa [CPU] Default the memory-delay countdown collapse on`
- `f6e0888e [CPU] Yield the fiber from collapsed spin-backoffs under the guest scheduler`
- `b97c7568 [CPU] Let the CTR spin collapse see past the injected safepoint`
- `4773f77f [Kernel] Skip the dispatcher round trip on a yield with nothing to yield to`

This is precisely the "park the poller instead of spinning" direction our own
analysis pointed at and never implemented.

`__savegprlr_29`/`__restgprlr_29` (3.67%, our "cheapest credible win") also has
a shipped answer:
- `98b691ea [CPU] Inline the XDK GPR save/rest helpers instead of calling them`
- `399b78a7 [CPU] Inline small leaf guest functions instead of calling them`
- `f87e7554` splits it save/rest for **on-device bisection** - they hit trouble
  and made it bisectable, worth copying as method.

Also `21291698 [GPU] Pace vblanks by absolute deadline` - our vblank fix
territory.

---

## 3. ✅ Independent evidence that Turnip/Mesa mis-compiles our shaders

- `62a6a0d4 [GPU] Test multiply operands for zero on their bits so Mesa cannot fold it into fmulz`
- `71e67832 [GPU] Gate the bitwise multiply zero test behind a cvar, on for PGR3 and Dark Souls`

A **Mesa codegen workaround**: they had to defeat Mesa folding a multiply into
`fmulz`. This is exactly the failure class hypothesised for the upside-down
vista, and it corroborates the known precedent that the stock Qualcomm driver
mis-compiled `resolve_full_32bpp` (the old flat-navy bug).

They also added `c2ab879a [Vulkan] Add cvars to drive the instrumented Turnip
GPU counter sampler` - Turnip instrumentation we could reuse.

**No fix for the upside-down vista exists in their tree.** Searched
flip/upside/invert/orientation/vista/mirror. The nearest hits are
`2ec7ee04 [Vulkan] Invert the tiled base delta into a 2D texel origin for
texture stores` (2026-08-03, addressing - worth reading) and
`842ede52 [GPU] Use XOR to flip X texel group in all load/resolve shaders`
(X axis, not Y). So this bug is still ours to solve.

---

## 4. 📄 Resolve features our fork diff already flagged as missing

`3addc186 [GPU] Gate resolve number format and gamma accuracy behind lean
shader variants`, `75447559 [GPU] Gate the full-resolve copy_dest_number
packing behind a cvar`, `99fc17a1 [GPU] Pack fixed-point components
branchlessly`.

These are the **same** `decode_pwl_gamma` / `dest_num_format` additions the
canary diff showed AE lacks - and they gate them behind cvars rather than
taking the cost unconditionally. Good model for how we should port them.

---

## Recommended order

1. **Re-test isb with coalescing** (invalidated measurement, cheap, hits the 14%).
2. **Study `edaf74cd` + `da6b36eb`** - shipped work on our #1 CPU hotspot.
3. **Port `98b691ea`** - our identified 3.67%, now with a reference implementation.
4. Keep the vista on the driver track; reuse `c2ab879a`'s Turnip instrumentation.
