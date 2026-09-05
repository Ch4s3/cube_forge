# Mycelium tick: an active set — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The mycelium tick costs nothing for columns that cannot change, so a world of settled patches ticks in well under a millisecond in release instead of 3.2 ms.

**Architecture:** The field gains a `dirty` byte per column, set after a tick on every column that changed or is unsettled (vigour off its target, or a claim pending). The next tick evaluates a column only if it or a 3x3 neighbour is dirty, or the biome field eased its climate this tick (the biome's own active flags, passed in). Everything else is carried forward by one memcpy per array: the tick blits the five arrays into fresh ones and the evaluated columns overwrite their own entry, still reading neighbours from the previous arrays, so the pull semantics and every result are unchanged. An always-evaluate `tick_all` stays as the oracle.

**Tech Stack:** March; `Window.u8_blit` / `f32_blit`; vigour moves from `NativeFloatArr` to `NativeF32Arr` so it can be blitted.

## Global Constraints

- Results must be bit-identical to the always-evaluate tick over any scenario: the oracle test asserts equal `state_hash` and equal vigour at every tick of a contest.
- No read-then-write on one array in a pass (GAPS G21/G63); the blit gives fresh destinations.
- Frame budget: `scratch/frame_budget.sh` (12 ms) still passes; record the `myc tick` release cost before and after on the pinned wild world (before: 3.2 ms, 12 samples).

## Tasks

### Task 1: `dirty` in the field, vigour as f32, blit copies

- `Field(species, vigour f32, reach, hold, claimant, dirty)`. `build` starts all-dirty. `plant`, `plant_patch`, `reseed` mark what they touch dirty (a patch: every column it wrote). `copy_u8` and `copy_f` become blits.
- Accessors unchanged except `vigour_of` reads f32; `field_vigour` returns `NativeF32Arr` (Fruit reads it with `get_f32`).
- Test: a fresh field is all dirty; after `plant` only the planted column is dirty; `plant_patch` dirties its octagon.

### Task 2: the active tick

- `tick(f, n, ta, ma, clim : NativeU8Arr, rate)`: blit the five arrays (the new dirty array starts at zero); rows pre-pass over `dirty | clim`; a column is evaluated iff `clim[j] != 0` or any of its 3x3 is dirty; `tick_col` writes new dirty = 1 when any of its five values changed or `vigour != fitness target` or the claimant is non-zero.
- `tick_all(f, n, ta, ma, rate)` = tick with an all-ones `clim`, the oracle.
- `Myc.no_climate(n) : NativeU8Arr` = zeros, for callers and tests.
- Tests: every existing tick test passes with `no_climate`; the contest scenario ticked 300 times by `tick` and by `tick_all` gives equal hashes and equal vigour at sampled columns every tick; a settled patch's `active_count` is 0; the wither-on-climate case: a settled Sunshelf patch under a dry climate, then a wet climate with `clim` all ones withers, and with `clim` zeros does not (documenting that climate change must be reported).

### Task 3: wire and measure

- `Biome.active(f) : NativeU8Arr` returns the biome's active flags. The field slot passes `Biome.active(bio1)` into `Myc.tick`.
- Measure `myc tick` on the pinned wild world in release (settled) and during growth (`CF_AUTOPLANT CF_MYC_RATE=5000`). Budget. `RESULTS.md` section, `todos.md` follow-up entry updated.
