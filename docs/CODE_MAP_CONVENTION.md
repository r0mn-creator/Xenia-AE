# `MAP(...)` — searchable code annotations

**Problem this solves:** finding "what code does what, and where" repeatedly cost
whole sessions. Knowledge lived in people's heads or in narrative docs that
nobody greps mid-investigation. The fix is to put it **in the code**, in a
format that a single search finds.

## The convention

Above any function that is an entry point, a decision point, or a source of
values other subsystems depend on:

```cpp
// MAP(<area>): one line - what this does.
// FED BY:  the guest registers / cvars / callers that determine its behaviour.
// FEEDS:   what consumes its output.
// KEY:     the one non-obvious fact a reader needs (optional).
// GOTCHA:  a trap that has already cost someone time (optional).
// DEBUG:   the toggle that instruments it (optional).
```

`<area>` is `subsystem/topic`, e.g. `gpu/resolve`, `gpu/scissor`,
`gpu/viewport`, `gpu/texture`, `gpu/shader`, `kernel/video`.

## How to use it

```sh
# what handles resolves?
grep -rn "MAP(gpu/resolve)" app/src/main/cpp/xenia-canary/src/

# everything in the GPU
grep -rn "MAP(gpu/" app/src/main/cpp/xenia-canary/src/

# what consumes the scissor?
grep -rn "MAP(" -A6 app/src/main/cpp/ | grep -i "scissor"

# list every mapped entry point
grep -rn "MAP(" app/src/main/cpp/xenia-canary/src/ | grep -oP 'MAP\([^)]+\)' | sort | uniq -c
```

## Rules

- **Write it when you learn it.** If you had to trace something to understand
  it, that trace is the annotation. Do it before moving on.
- **Record what FEEDS it**, not just what it does. Most real bugs are wrong
  inputs to correct code - the whole Halo 3 vista hunt ended up there.
- **Record gotchas with the cost.** "the SCISSOR is normally the limiter, not
  surface_pitch" saves the next person a build cycle.
- **Reference the doc, don't duplicate it.** Point at
  `docs/HALO3_VISTA_46_VS_64.md` rather than restating findings that may change.
- Keep it to facts about the code. Investigation narrative belongs in `docs/`.

## Annotated so far

| area | function | file |
|---|---|---|
| `gpu/resolve` | `GetResolveInfo` | `gpu/draw_util.cc` |
| `gpu/scissor` | `GetScissorTmpl` | `gpu/draw_util.cc` |
| `gpu/viewport` | `GetHostViewportInfo` | `gpu/draw_util.cc` |
| `gpu/texture` | `TextureCache::RequestTextures` | `gpu/texture_cache.cc` |
| `gpu/shader` | `GetCurrentVertexShaderModification` | `gpu/vulkan/vulkan_pipeline_cache.cc` |
| `kernel/video` | `VdQueryVideoMode` | `kernel/xboxkrnl/xboxkrnl_video.cc` |

Extend this table as areas are annotated. The goal is that a new investigation
starts with a grep, not an archaeology session.
