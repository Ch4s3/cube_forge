# Fungus: climate feedback — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A mycelium network changes the climate of the ground it holds, a little per species and more when species combine, enough that a deliberate collection of fungus can tip a column across a biome threshold — limited terraforming.

**Architecture:** `Myc.bonuses` derives two per-column climate offsets (temperature, moisture) from the species standing in each column's 3x3: each **distinct** species present contributes its own fixed pull, the pulls sum, and the sum is clamped to `cap()` per axis. `Biome.tick` adds the offsets to its temperature and moisture targets before easing, wakes a column whose offset changed since it last applied one, and keeps the offsets it applied in its field. Everything downstream — classification, retexturing, vegetation, precipitation — follows the eased axes as before, so a tipped biome grows its trees and swaps its surface through the paths that already exist.

**Stability argument** (the reason the original spec set this aside): every species' pull points at its own band's core, so a species never lowers its own fitness by being there; the feedback is monotone reinforcement, bistable at worst and never a cycle. Two species with opposite pulls on one column sum, and whichever dominates strengthens itself and weakens the other through the contest that already resolves mixed ground — the loop still has no state it can return to. Rates are bounded on both sides by easing (`Biome.default_rate`, `Myc.default_rate`) and by the biome hold counter.

**Tech Stack:** March. No C changes.

## Constants

| species | temperature pull | moisture pull | core |
|---|---|---|---|
| Frostcap | -0.06 | 0 | cold |
| Pinewart | -0.03 | +0.05 | cold damp |
| Meadowbell | 0 | -0.06 | temperate dry |
| Lanterncap | 0 | +0.06 | temperate damp |
| Marshlight | +0.02 | +0.08 | hot damp |
| Sunshelf | +0.06 | -0.06 | hot dry |

`Myc.cap()` = 0.2 per axis. One species alone moves an axis at most 0.08, less than the 0.16 between the damp threshold (0.5) and the centre of the dry band, so a monoculture cannot cross a biome threshold from the middle of a band; Pinewart + Lanterncap + Marshlight together reach +0.19 moisture, enough to turn grassland at 0.32 into forest. `CF_MYC_FEEDBACK` (percent, default 100) scales the pulls; 0 disables.

## Tasks

### Task 1: `Myc.bonuses`

- `Species.pull_t(sp)`, `Species.pull_m(sp)` from the table.
- `Myc.cap() : Float` = 0.2. `Myc.bonuses(f, n, scale) : NativeF32Arr` of `2 * n * n` (t at `2i`, m at `2i + 1`): for each column, sum the pulls of the distinct species in its 3x3 (a species counted once however many columns it holds there), times `scale`, clamped to `[-cap, cap]`. Rows with nothing in them or beside them are left at zero by the tick's row pre-pass idiom.
- Tests: an empty field is all zero; a lone Lanterncap column gives +0.06 moisture to itself and its eight neighbours and nothing two away; a Lanterncap next to a Marshlight next to a Pinewart gives the three-way sum on the columns that touch all three and is clamped where the sum would exceed the cap (use `scale` 3.0 to force it); the same species twice in a 3x3 counts once.

### Task 2: `Biome.tick` applies the offsets

- `Biome.Field` gains one `NativeF32Arr` (the offsets last applied, same layout). `Biome.tick(f, w, seed, rate, bonus : NativeF32Arr)`: targets become `clamp01(temp_from_base + bonus_t)` and `clamp01(moisture_target + bonus_m)`; a column whose bonus differs from the stored one by more than `Myc.vigour_eps()` is woken (`act` = 1) and the stored value updated. `Biome.build` stores zeros. `Biome.no_bonus(n)` = zeros for callers without a network.
- Tests (`biome_test`): with zero bonus nothing changes (existing tests pass unchanged with `no_bonus`); a +0.19 moisture bonus on a dry inland column moves its eased moisture up by 0.19 over enough ticks at a fast rate; a bonus that appears wakes a settled column (its `active` flag set) and the same bonus again does not.

### Task 3: wire, knob, verify

- Field slot: `let bonus = Myc.bonuses(myc0, cols, feedback / 100)` from the field **before** this tick (the mycelium reads the climate the biome just eased; the biome reads the network as it stood), then `Biome.tick(..., bonus)`, then `Myc.tick`.
- `CF_MYC_FEEDBACK` knob. The `mycelium at` dump line gains the column's bonus.
- End to end, seed 7 (spawn is grassland by a lake, moisture 0.25 at the planting column): a mature Marshlight patch alone (`CF_AUTOPLANT_SPECIES=5 CF_AUTOPLANT_MATURE=1 CF_MYC_RATE=0 CF_BIOME_RATE=100000`) raises the column's moisture toward 0.33 and the biome stays grassland; then a run with three damp species planted (extend `CF_AUTOPLANT_SPECIES` to accept a comma list, planting each three columns further east) reaches the cap and the biome under the overlap turns forest: the `biome at player column` line and the map dump show it. Measure the field-slot cost with the bonus pass. Budget. `RESULTS.md`, spec §out-of-scope → an *As built* §12, `todos.md`.
