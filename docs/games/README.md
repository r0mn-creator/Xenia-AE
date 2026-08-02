# Per-game notes

One file per title, named **`<TITLE ID> - <Game Title>.md`**, e.g.
`454107EC - Need for Speed Carbon.md`. The title ID is what the emulator itself
keys everything on — the per-game config (`<TITLEID>.config.toml`), the per-game
driver preference, the patch file, and the pipeline cache
(`cache/pipelines_<TITLEID>.bin`) — so it is the only name that is guaranteed
to match across the app, the logs and this directory.

## Why these exist

A fix made for one title changes shared engine code and can break another. That
already happened: NFS Carbon reached gameplay on **2026-07-04**, the Halo 3 GPU
work landed **2026-07-10 … 07-13**, and NFS has frozen at the main menu since.
Working that out required reconstructing dates from `git log` after the fact.

These notes exist so that never has to be reconstructed again. Each file records
**when a title last worked, exactly which build and settings it worked under, and
what changed after** — so "did X break Y" is a lookup, not an investigation.

## What goes in a file

- **Status** — where the title gets to today, and on which device.
- **Working configuration** — the settings and toggles it was verified under.
  A result with no configuration attached is not reusable.
- **Code that makes it work** — the commits this title depends on, with what each
  one does. This is the list to check first when the title regresses.
- **Timeline** — dated entries: worked / broke / changed, each naming the commit
  or setting responsible. Append, never rewrite: a wrong entry gets a correction
  underneath it, because knowing that a theory was tried and failed is worth as
  much as knowing what worked.
- **Open problems** — what is still wrong, and what has already been ruled out.

## Rules

- **Date every entry** and use absolute dates, never "last week".
- **Record measurements, not impressions.** "0 changed pixels across 4 frames
  15 s apart" is a fact; "it looks frozen" is not.
- **Record retractions in place.** Several conclusions in these files were later
  disproved; they are kept with the disproof attached, so nobody re-runs them.
- Cross-link the deep-dive docs (`../HALO3_DETECTIVE_LOG.md`,
  `../NFS_CARBON_FREEZE.md`) rather than duplicating them here. These files are
  the index and the timeline; those are the investigations.
