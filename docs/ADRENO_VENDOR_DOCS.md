# Adreno vendor documentation — what it does and does not settle

Source: **Qualcomm Adreno Game Developer Guide**,
https://docs.qualcomm.com/doc/80-78185-2/topic/gpu.html
(also in the user's Drive `References` folder as a 21 MB PDF).

⚠️ The **ARM Architecture Reference Manual contains NOTHING about GPUs** —
verified: zero hits for `shader`, `SPIR-V`, `GPU`, `Vulkan`, `texture`,
`rasteriz`, `Adreno`. The ARM ARM is CPU-only. Adreno is Qualcomm's separate
architecture; shaders are Khronos SPIR-V. Three vendors, three specs.

## 1. Concurrent binning — the mechanism exists, but does NOT explain our bug

> "For each render pass, if no dependencies prohibit it, concurrent binning
> allows the binning pass to run asynchronously before vertex shader execution."

> "The compiler generates a position-only vertex shader (a version of the vertex
> shader that the compiler **attempts to simplify** to only the instructions
> that affect vertex positions) that will be used to determine which bin (or
> bins, in the event a triangle overlaps multiple bins) to place each triangle."

`vulkan_command_processor.cc` claims Adreno "advertise[s]
vertexPipelineStoresAndAtomics but do[es] not reliably run vertex-stage stores
(the vertex shader executes in a position-only binning pass, so its stores are
stripped/duplicated)".

**Qualcomm confirms the position-only pass exists but says nothing about side
effects being stripped or duplicated.** And two measurements already exonerate
it:

- the **compute-memexport** path bypasses binning entirely and underfilled
  **identically** (6.32% vs 6.5-7.2%);
- the **RADV control** showed the correct platform fills the buffer **LESS**
  (6.1% vs 8.1%), so underfill was never the bug.

So the comment's *mechanism* is real, but its *conclusion* is not supported.
Do not re-derive it.

## 2. Resolve / GMEM — confirms the path the live lead sits in

> Adreno uses Graphics Memory (GMEM) for "on-chip high performance" rendering,
> and "once the rendering is complete for the tile, the GMEM color contents are
> sent back (**resolved**) to system memory."

This is exactly the path where AE's `resolve.xesli` diverges from upstream by
**857 lines** (989 -> 818), missing `decode_pwl_gamma`, `dest_number_is_unorm`,
`dest_num_format` and `dest_row_pitch_macro_tiles`, and substituting
`dest_row_pitch_aligned`. See `HALO3_VISTA_UPSIDE_DOWN.md`.

## 3. What the vendor docs still have NOT answered

The pages fetched were largely a table of contents. **Still unanswered:**

- Are **vertex-stage stores** duplicated or dropped by the binning pass?
  (Qualcomm is silent; our data says it does not matter for this bug.)
- **Tile/macro-tile addressing and pitch requirements** for resolve — the
  single most relevant question for the `resolve.xesli` divergence.
- **Render-target Y orientation / origin conventions** — nothing found, and
  this is what the upside-down vista needs.

Worth pulling the 21 MB PDF locally (`reference/`, gitignored) and extracting it
with `pdftotext` the way `arm_arm.txt` was, so it can be grepped rather than
fetched page by page.
