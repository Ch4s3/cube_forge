# Fungus: save the fields, and food effects — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A save carries the mycelium field and the biome's eased axes, so a load brings back planted and spread fungus and the climate it has shifted; and eating a mushroom cap gives a timed status effect by species.

**Architecture, fields:** one more file per slot, `fields.bin`, a byte blob of eight arrays of `n*n` entries: species, vigour (0..255), reach, shown, then temperature and moisture as 16-bit fixed point. `Biome.export_axes/import_axes` and `Myc.export/import` read and write their halves; `World.with_shown` restores the surface species; `Save.write_with_fields` writes chunks, then fields, then the header last, so the header still marks a complete save. A slot without `fields.bin` (an older save) loads with a printed note and the fields rebuilt from the seed, as before. Vigour at 1/255 and axes at 1/65535 are below anything the game can show.

**Architecture, food:** a new `CubeForge.Effects` module holds four until-times (speed, jump, swim, lantern) and answers multipliers for a given clock. It lives in the `Ui` beside the inventory. Using a cap item with no block in reach eats it: one is consumed and the species' effect starts for `duration()`. `Player.update_with` takes the three multipliers; `Player.update` is the same with ones. The lantern effect forces the flashlight on. The ground readout line shows the active effects with seconds left.

| species | effect |
|---|---|
| Meadowbell, Sunshelf | speed x1.5 |
| Frostcap, Pinewart | jump x1.35 |
| Marshlight | swim x1.6 |
| Lanterncap | lantern (the flashlight, hands free) |

Effects are not saved: they last thirty seconds.

## Tasks

1. **Export and import.** `Biome.export_axes(f, n, out, off)`, `Biome.import_axes(f, w, bytes, off)` (axes from bytes, reclassified from them, every column active). `Myc.export(f, n, out, off)`, `Myc.import(bytes, off, n)` (dirty and pull rows all set, seen -1, pulls recomputed). `World.with_shown(w, sh)`. Tests: round trips on a flat world; vigour within 1/255; pulls after import equal a fresh planting's.
2. **The file.** `Save.fields_path`, `Save.fields_bytes(side)`, `Save.write_with_fields`, `Save.read_fields`. `check` still validates chunks and header only: fields are optional. Tests in the save test's temp dir: round trip, wrong size rejected, missing file is `Err`.
3. **Wire.** `save_game` writes the fields; `run_session` on a load reads them and builds the two fields and `shown` from them, falling back with a note. End to end: plant, save, load, the `mycelium:` counts and `myc` hash match across the load.
4. **Effects.** The module and its tests; `Player.update_with` and a jump test.
5. **Eat.** `interact` gets `now`; the eat branch; effects into `Ui`; the frame loop reads multipliers and the lantern; the readout shows effects. `CF_AUTOEAT=<frame>` gives a Frostcap cap and eats it; the dump prints the effects line. Budget, `RESULTS.md`, spec as-built (§7 food), `todos.md`.
