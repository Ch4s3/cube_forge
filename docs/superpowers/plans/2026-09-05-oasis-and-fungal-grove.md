# Oasis and fungal grove — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two biomes the field earns rather than the seed: an oasis around a small body of water in a hot region, ringed by palms, and a fungal grove wherever the mycelium network is established. Design: `docs/superpowers/specs/2026-09-05-oasis-and-fungal-grove-design.md`.

**Architecture:** Water columns are labelled into connected bodies each tick and a small body moistens `small_reach()` columns instead of `reach()`; the flag rides in the distance byte's top bit, so the Field's shape is unchanged. `classify` gains two booleans (small water, established mycelium) and two ids appended after alpine. The grove is computed inside `Biome.tick` from the network's species and vigour arrays, woken by the network's dirty rows the way the pulls' rows wake it today. Palms are a third tree species: the `pine : Bool` on the tree generator becomes `species : Int` (0 oak, 1 pine, 2 palm) with two new blocks.

**Stability:** the grove reads mycelium state; the mycelium reads eased temperature and moisture and never the biome id; the grove's outputs (felled trees, more fruit) touch neither field's inputs. No loop.

**Tech Stack:** March; one C change (the map colour table in `native/cf_shim.c`).

## Constants

| knob | value |
|---|---|
| `Biome.small_body()` | 48 water columns |
| `Biome.small_reach()` | 4 columns |
| `Biome.small_bit()` | 128 (top bit of the distance byte) |
| `Biome.grove_vigour()` | `Myc.fruit_vigour()`, 0.8 |
| `Biome.grove_neighbours()` | 5 of 9 |
| `Biome.b_oasis()` / `b_grove()` | 8 / 9 |
| `Fruit.biome_rate(b)` | grove 3.0, else 1.0 |
| `Veg.density(oasis)` | 0.5 |
| `Chunk.palm_log()` / `palm_frond()` | 65 / 66 |
| `Texture.layers()` | 87 -> 89; palm bark 87, frond 88 |

## Tasks

### Task 1: water bodies and the small-body reach (`biome.march`, `biome_test.march`)

The label pass. A `NativeIntArr(n * n + 1)`: water column `i` starts at `i + 1`, every other column 0, and slot `n * n` counts the writes of the current round. A round is two passes over one threaded array, in the shape of `level_go`:

- `relabel_go`: for each water column, the minimum of its own label and its water 8-neighbours' labels; write if smaller and bump the counter.
- `jump_go`: `lab[i] = lab[lab[i] - 1]` where that differs (pointer jumping: the root's label is the body's least column index); bump the counter.

`rounds_go` clears the counter, runs both, and stops when the counter stays 0. A queue is out (GAPS G63); reading and writing one threaded array in one pass is the proven `mark8` shape.

Then `counts_go` over water columns bumps `counts[lab[i] - 1]` (a second `NativeIntArr(n * n)`), and `seed_go` becomes `seed_small_go(ws, lab, counts, n, i, d)`: a water column seeds `d[i]` with `small_bit()` when its body's count is at or under `small_body()`, else 0.

The sweep carries the bit: `mark8` reads the marker's byte once, `nd = lv + 1 + (v / 128) * 128`, and treats `v % 128 == reach()` as unvisited; `level_go` tests `v % 128 == lv`. `sweep_go` still runs to `reach() - 1`: a small body's columns beyond `small_reach()` keep a distance in the byte and the flag, and `moisture_target` gives them zero.

- `fn dist_of(v : Int) : Int do v % 128 end`, `fn is_small(v : Int) : Bool do v >= 128 end`.
- `fn moisture_target(dist : Int) : Float` unchanged (the large-body curve, tests keep passing); new `fn moisture_target_of(v : Int) : Float` decodes the byte and uses `small_reach()` when the bit is set. `classify_all` and `tick_go` call the new one and pass `dist_of(v)` to `classify`.
- `mark_dist_go` compares whole bytes: a body that grows past the threshold changes its columns' bytes and wakes them, which is right.

Tests:
- one water block in a flat world: `dist_of` at the block is 0 and at (67,66) from (64,64) is 3, as today; `is_small` is true on both; far away is `reach()` with no flag.
- a 60-column trench at x = 64, z = 30..89: `is_small` false beside it; `moisture_target_of` at three columns is 0.875.
- a puddle of five columns: `moisture_target_of` at two columns is 0.5, at four 0.0, at five 0.0.
- a puddle touching the sea (a 70-column body plus a 2x2 pool adjacent to it) is one body: not small.
- the existing canal test passes unchanged (the trench is three water blocks: small, so the bank at four columns is dry — **move the asserted bank column to (66, 60)**, two from the water, which is what a small body wets; the test's point is the gradual change, not the width).
- the "does not flap" test reads (66, 60), two from one block: fine as is.

### Task 2: the two ids and their table rows

- `Biome.b_oasis() = 8`, `b_grove() = 9`, `name`, `precip_scale` (oasis 0.3, grove 1.1). `palette(b_grove(), _, _) = 0` and `migrations_go` skips `want == 0`. `is_palette_block` unchanged.
- `classify_full(temp, moist, h, dist, small : Bool, grove : Bool)`: grove, then alpine, then `temp >= 0.66 && moist >= 0.5 && small` -> oasis, then beach, then the table. `classify(temp, moist, h, dist)` = `classify_full(..., false, false)` so the existing threshold tests stand.
- `native/cf_shim.c`: `CF_BIOME_RGB[10]`, oasis `{0.30f, 0.85f, 0.35f}`, grove `{0.55f, 0.30f, 0.70f}`; index `b[i] < 10 ? b[i] : 0`.
- `audio.march`: oasis = desert's form (`form_r`) and transposition (2), forest's octave (3) and rate (10), timbre 3; grove = wetland's form (`form_ri`) and transposition (10), octave 2, timbre 4, rate 4.
- `Veg.wants_trees` adds oasis, `density(oasis) = 0.5`, `wants_bushes` adds oasis. Grove is in none of them.
- `Fruit.biome_rate(b)`: 3.0 for grove, 1.0 otherwise; `scan_go` multiplies `scale` by it for the column's biome (the field is already a parameter).

Tests: `classify_full(0.9, 0.9, 63, 1, true, false)` is oasis (beats beach); `(0.9, 0.9, 63, 1, false, false)` is beach; `(0.9, 0.9, 70, 5, false, false)` is wetland; `(0.5, 0.9, 70, 5, true, false)` is forest (oasis needs hot); `(0.9, 0.9, 100, 5, true, true)` is grove (beats alpine); `(0.9, 0.9, 100, 5, true, false)` is alpine. `precip_scale(oasis) < precip_scale(grassland)`. `palette(grove)` is 0 and a grove column never appears in `migrations` (a flat world with a grove id poked into the field: build the Field through `Biome.build` and check `migrations` on a field whose column is forced grove via a test-only `Biome.with_biome(f, x, z, id)` helper). `Veg.wants_trees(oasis)` and not grove. `Fruit.biome_rate(grove)` is 3.0 and `candidates` at scale `1.0 / 3.0` on a grove field returns what scale 1.0 returns on a forest field (same seed, same tick).

### Task 3: palms

- `chunk.march`: `palm_log() = 65`, `palm_frond() = 66`; `is_log` and `is_foliage` include them (`is_cutout`, `Light.opacity`, the mesher, `World.decay_leaves`, `Inventory.yield_of` all follow from those two).
- `texture.march`: `layers() = 89`, the array sized `89 * 16 * 16 * 4`; `bark_tinted_go(a, 87, 0, 188, 166, 118)` for the palm bark; `fronds_go(a, 88, 0)`: green `(84, 150, 62)` with the leaf hash's transparency and a lighter midrib on rows 7 and 8 (`+24`). `layer_for(65) = 87.0`, `layer_for(66) = 88.0`; `layer_for_face(65, d)` gives the rings (9.0) on the cut ends. Doc string lists the two.
- `trees.march`: `fn species_at(h : Int) : Int` (1 above the treeline else 0) replaces `is_pine`; `block_of_tree(tx, tz, base, trunk, species : Int, x, y, z)`: log id by species (`oak_log`, `pine_log`, `palm_log`); leaf predicate by species, palm: `dy == trunk && d >= 1 && d <= 2` or `dy == trunk + 1 && d <= 1` where `d = |dx| + |dz|`, frond id `palm_frond`. `trunk_height` unchanged (generation never plants palms). `fn species_of_log(id : Int) : Int` (0 oak, 1 pine, 2 palm).
- Callers: `Chunk.stamp_tree` / `plant_trees_go` (`species_at(base)`), `Veg.plant(w, tx, tz, base, trunk, species : Int)`, `Veg.fell` (`species_of_log` of the first log), `Veg.unleaf_go`, `Veg.species_for(b)` replaces `pine_for` (1 taiga, 2 oasis, else 0), `Veg.is_log` -> `C.is_log`. `cube_forge.march` `veg_go`: `let sp = Veg.species_for(b)`, trunk 7 for pine, 6 for palm, 5 for oak; `Veg.plant(w, x, z, base, trunk, sp)`. `trees_test` and `veg_test` call sites take the Int.

Tests: `block_of_tree` with species 2, trunk 6: logs at dy 1..6 on the trunk column, fronds at exactly the 8 + 5 predicate cells, nothing at dy 8 or beyond |dx|+|dz| 2, nothing outside the 9x9; a generated chunk at seed 5 has the same `count_ids` for logs and foliage as before the refactor (the trees test that counts already does this); `Veg.plant` then `Veg.fell` of a palm leaves the 9x9x8 box empty; `species_of_log(palm_log()) == 2`; `layers() == 89` in `myc_block_test` (the assertion text updates); `is_foliage(palm_frond())`, `is_log(palm_log())`, `Inventory.yield_of(palm_frond()) == 0`.

### Task 4: the grove in the tick

- `Myc.dirty_rows(f, n) : NativeU8Arr` (n bytes): 1 for row z when a column in rows z-1..z+1 is dirty. One pass over the dirty array.
- `Biome.tick_net(f, w, seed, rate, pulls, prow, scale, sps : NativeU8Arr, vg : NativeF32Arr, drow : NativeU8Arr)`; `Biome.tick(...)` keeps its signature and calls `tick_net` with `no_species(n)` (zeros, n*n), `no_vigour(n)` (f32 zeros) and `no_rows(n)`.
- `tick_go` skips a column only when `act == 0 && prow[z] == 0 && drow[z] == 0`. For an evaluated column: `established(sps, vg, n, x, z)` counts the 3x3 columns with species set and vigour at or above `grove_vigour()`, clipped at the field edge; grove when the count reaches `grove_neighbours()`. `classify_full(t, m, h, dist_of(v), is_small(v), grove)`. Settled stays as defined: a grove flip goes through the hold counter like any other.
- `cube_forge.march` field slot: `Biome.tick_net(..., Myc.field_species(scene_myc(scw)), Myc.field_vigour(scene_myc(scw)), Myc.dirty_rows(scene_myc(scw), cols))`.

Tests (`biome_test`, using `Myc.plant_patch` for a mature octagon and `Myc.build` for empty):
- an empty network: `tick_net` with zero arrays and `tick` agree on `state_hash` after 40 ticks.
- a mature patch of radius 8 at (40, 40) on a flat world, `drow` all ones on the first tick: after `hold_ticks() + 1` ticks at rate 0.05 the centre is grove, (40 - 8, 40) at the patch's rim is not, and (40 - 6, 40) is (one inside the rim; `plant_patch` sets full vigour everywhere so the 5-of-9 rule alone draws the edge).
- a single planted column (`Myc.plant`, seed vigour 0.25): never grove.
- wake: settle the field with an empty network, then hand it the patch's arrays with `drow` marking only row 40: column (40, 40) is active after that tick; with `no_rows` it stays settled.
- `Veg.can_decay` on a grove column standing under an oak (plant one with `Veg.plant`, force the biome with `with_biome`) is true.

### Task 5: run, measure, record

- `forge test`; `forge build --release`.
- Headless, seed 7: `CF_BIOME_MAP=1 CF_AUTOMAP=<frame> CF_NOMOUSE=1` before and after Task 1 on the same seed, and look at the two maps: small ponds' halos tighten, the coast does not move.
- Oasis end to end: a hot seed (or `CF_AUTOEDIT` placing a finite water block on desert), `CF_BIOME_RATE=100000 CF_VEG_BUDGET=8`: the `biome at player column` line says oasis, palms stand after a few periods.
- Grove end to end: `CF_AUTOPLANT_SPECIES=4 CF_AUTOPLANT_MATURE=1 CF_BIOME_RATE=100000` in forest: the column reads grove, an oak inside the patch is felled on the vegetation slot, fruit appears faster than a control run.
- Biome tick cost with the label pass, settled and while a body changes; `RESULTS.md`; spec gets an *As built* note where anything moved; `todos.md` entry ticked.
