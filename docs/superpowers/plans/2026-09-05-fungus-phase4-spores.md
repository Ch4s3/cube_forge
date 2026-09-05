# Fungus phase 4: spores and planting — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The loop closes: wild mycelium exists at world start, digging it sometimes yields spores, spores are inventory items with icons, using one on the ground plants that species there, and a HUD readout says what the looked-at ground holds and what would grow on it.

**Architecture:** Spores are item ids `200 + species` (items already share the block-id space; nothing else lives above 199). `Species` owns the item mapping, `Texture` six icon layers, `Inventory` a `give`. `Myc.plant_patch` writes a mature octagon (what spreading would have produced) and `Myc.wild` seeds one per 16x16 cell at low density by the cell hash, choosing the fittest species for the cell's climate; at session start those patches are applied to the world as blocks **before** the light flood, so glowing wild patches cost no relight. `interact` routes a spore item to `plant_spore` instead of `edit_block`; breaking mycelium rolls a position hash for a spore of the shown species. The readout is one text line in its own HUD slot, rebuilt only when its string changes.

**Tech Stack:** March; `Hud.emit_text` for the readout; no C changes.

Spec: `docs/superpowers/specs/2026-09-04-fungus-design.md` §7, §4 (spore chance), §9 step 4, §10 (canal).

## Global Constraints

- All prior-phase constraints (GAPS G21/G33/G56/G63/G65/G68/G69).
- Item ids: blocks are 0..43 today; spores are 201..206. `Inventory` treats any positive int as an item; only `yield_of` and the HUD icon need to know what a spore is.
- A spore plants only on a column whose surface can carry mycelium (`Chunk.myc_index_of(Chunk.myc_base(surface)) >= 0`) and that is not wet; a miss consumes nothing.
- Wild density: 12% of 16x16 cells, hash-driven, seeded, so a world's wild fungus is stable across runs.
- Frame budget: `scratch/frame_budget.sh` (12 ms) must still pass; wild patches now exist in the pinned scenario, so this is the first budget run with fungus live.
- Commit after every task. No attribution lines.

## File structure

| file | responsibility |
|---|---|
| `lib/cube_forge/species.march` | `spore_item`, `is_spore`, `species_of_item`. |
| `lib/cube_forge/texture.march` | spore icon layers 60..65; `icon_layer` maps spores; `layers()` = 66. |
| `lib/cube_forge/inventory.march` | `give(i, item, n)`. |
| `lib/cube_forge/myc.march` | `plant_patch`, `wild`, `dig_spore`, `readout`. |
| `lib/cube_forge/hud.march` | `build_readout(b, text)`. |
| `lib/cube_forge.march` | startup wild patches + `apply_shown_all`; `plant_spore`; interact routing; spore drop; readout slot; `CF_AUTOSPORE`, `CF_WILD`. |
| tests | `species_test`, `myc_test`, `myc_block_test`, `inventory_test`. |

---

### Task 1: Spore items, icons, `Inventory.give`

**Files:**
- Modify: `lib/cube_forge/species.march`, `lib/cube_forge/texture.march`, `lib/cube_forge/inventory.march`
- Modify: `test/species_test.march`, `test/inventory_test.march`, `test/myc_block_test.march`

**Interfaces:**
- Produces: `Species.spore_item(sp) : Int` = 200 + sp; `Species.is_spore(id) : Bool` (201..206); `Species.species_of_item(id) : Int` (0 when not a spore). `Texture.spore_layer(sp) : Float` = 59 + sp; `Texture.icon_layer(spore id)` = that; `Texture.layers()` = 66. `Inventory.give(i, item, n) : Inv` (adds `n` of `item` via `slot_for`; a full inventory drops it, as `harvest` does).

- [ ] **Step 1: Failing tests.** `test/species_test.march`:

```march
  describe "Species spore items" do
    test "spore ids are 200 + species and round-trip" do
      Test.assert_eq_int(S.spore_item(4), 204, "Lanterncap spores")
      Test.assert_true(S.is_spore(204), "is a spore")
      Test.assert_false(S.is_spore(22), "the glow cap is not")
      Test.assert_false(S.is_spore(207), "207 is not")
      Test.assert_eq_int(S.species_of_item(204), 4, "back to the species")
      Test.assert_eq_int(S.species_of_item(3), 0, "a block is no species")
    end
  end
```

`test/inventory_test.march` (inside the module, using its `total_of`):

```march
  describe "Inventory.give" do
    test "adds n of an item, stacking with the same item" do
      let i0 = Inv.give(Inv.new(), 204, 3)
      Test.assert_eq_int(total_of(i0, 204, 0, 0), 3, "three spores")
      let i1 = Inv.give(i0, 204, 2)
      Test.assert_eq_int(total_of(i1, 204, 0, 0), 5, "five, one stack")
      Test.assert_eq_int(Inv.item_at(i1, 0), 204, "in the first hotbar slot")
      Test.assert_eq_int(Inv.count_at(i1, 1), 0, "not a second stack")
    end
  end
```

`test/myc_block_test.march`:

```march
  describe "Texture spore icons" do
    test "one icon layer per species after the mycelium layers" do
      Test.assert_eq_int(Tx.layers(), 66, "60 + 6")
      Test.assert_true(near(Tx.spore_layer(1), 60.0), "Frostcap")
      Test.assert_true(near(Tx.icon_layer(CubeForge.Species.spore_item(6)), 65.0), "Sunshelf spores in the HUD")
      Test.assert_true(near(Tx.icon_layer(1), 0.0), "grass icon unchanged")
    end
  end
```

- [ ] **Step 2: Run to verify failure.**

- [ ] **Step 3: Implement.** `species.march`:

```march
  -- ── Spores: the item that plants a species ──────────────────────────────
  doc "Item id of a species' spores: 200 + species. Items share the block-id space; nothing else lives above 199."
  fn spore_item(sp : Int) : Int do 200 + sp end
  fn is_spore(id : Int) : Bool do id >= 201 && id <= 200 + count() end
  doc "The species a spore item plants, or 0 when [id] is not a spore."
  fn species_of_item(id : Int) : Int do if is_spore(id) do id - 200 else 0 end end
```

`texture.march`: `layers()` 66; allocation `66 * 16 * 16 * 4`; `checkerboard` ends `spore_all_go(myc_all_go(a17, 0), 1)`; `icon_layer` becomes `if id == 4 do 15.0 else if CubeForge.Species.is_spore(id) do spore_layer(CubeForge.Species.species_of_item(id)) else layer_for(id) end end`; and:

```march
  -- ── Spore icons: a dark disc dusted with the species colour ─────────────
  doc "HUD icon layer for a species' spores."
  fn spore_layer(sp : Int) : Float do int_to_float(59 + sp) end
  pfn spore_go(a : NativeU8Arr, sp : Int, i : Int) : NativeU8Arr do
    if i >= 256 do a
    else
      let x = i % 16 - 8
      let y = i / 16 - 8
      let d2 = x * x + y * y
      let dust = (x * 5 + y * 3 + x * y) % 4 == 0
      let sr = CubeForge.Species.colour_r(sp)
      let sg = CubeForge.Species.colour_g(sp)
      let sb = CubeForge.Species.colour_b(sp)
      let inside = d2 <= 36
      let r = if !inside do 30 else if dust do sr else sr / 3 end end
      let g = if !inside do 30 else if dust do sg else sg / 3 end end
      let b = if !inside do 30 else if dust do sb else sb / 3 end end
      spore_go(put_rgba(a, (59 + sp) * 1024 + i * 4, r, g, b, 255), sp, i + 1)
    end
  end
  pfn spore_all_go(a : NativeU8Arr, sp : Int) : NativeU8Arr do
    if sp > 6 do a else spore_all_go(spore_go(a, sp, 0), sp + 1) end
  end
```

`inventory.march`, beside `harvest`:

```march
  doc "Add [n] of [item] directly (no yield mapping). A full inventory silently drops it."
  fn give(i : Inv, item : Int, n : Int) : Inv do
    if item == 0 || n <= 0 do i
    else
      let s = slot_for(i, item)
      if s < 0 do i else set_slot(i, s, item, count_at(i, s) + n) end
    end
  end
```

- [ ] **Step 4: Test**, **Step 5: commit** `feat(fungus): spore items, icons, Inventory.give`.

---

### Task 2: `Myc.plant_patch`, `Myc.wild`, `Myc.dig_spore`, `Myc.readout`

**Files:**
- Modify: `lib/cube_forge/myc.march`
- Modify: `test/myc_test.march`

**Interfaces:**
- Produces: `Myc.plant_patch(f, n, x, z, sp) : Field` — a mature octagon of `plant_radius()`: every column within it gets `sp`, vigour 1.0, reach `plant_reach() - cost` where cost is the octagonal distance (`2 * max(dx, dz) + min(dx, dz)` in half-hops; reach never below 0), hold and claimant 0; skips columns off the field. `Myc.wild_density() : Float` = 0.12; `Myc.wild_cell() : Int` = 16; `Myc.wild(f, n, ta, ma, seed) : Field` — for each cell with `Noise.hash2(cx * 31, cz * 17, seed + 9001) < wild_density()`, the column `(cx * 16 + 8, cz * 16 + 8)` gets `plant_patch` of `Species.best_for(ta[i], ma[i])` when that is not 0. `Myc.dig_spore(x, y, z, seed) : Bool` — `Noise.hash2(x * 3 + y, z * 5 + y, seed + 77) < 0.25`. `Myc.readout(f, n, x, z, temp, moist) : String` — `"<Name> <vigour%>"` when the column holds a species, else `"bare: <best-for name> <fitness%> would grow"` or `"bare: nothing grows here"`.

- [ ] **Step 1: Failing tests** — append to `test/myc_test.march`:

```march
  describe "Myc.plant_patch and wild" do
    test "a mature patch is the octagon a planting would have grown into" do
      let n = 40
      let f = Myc.plant_patch(Myc.build(n), n, 20, 20, 3)
      Test.assert_eq_int(radius_go(f, n, 3, 20, 20, 0, 0), Myc.plant_radius(), "radius")
      Test.assert_true(near(Myc.vigour_of(f, n, 20, 20), 1.0), "full vigour at the centre")
      Test.assert_true(near(Myc.vigour_of(f, n, 27, 20), 1.0), "full vigour at the rim")
      Test.assert_eq_int(Myc.reach_of(f, n, 20, 20), Myc.plant_reach(), "full reach at the centre")
      Test.assert_eq_int(Myc.reach_of(f, n, 28, 20), 0, "no reach at the rim")
      Test.assert_eq_int(Myc.species_of(f, n, 28, 28), 0, "the diagonal corner is outside the octagon")
      Test.assert_true(Myc.count_species(f, n, 3) > 100, "a real patch")
      -- stable: a tick on ideal ground changes nothing discrete
      let f1 = Myc.tick(f, n, uniform(n, 0.5), uniform(n, 0.3), 0.1)
      Test.assert_eq_int(Myc.state_hash(f1, n), Myc.state_hash(f, n), "already settled")
    end
    test "wild seeds patches by the cell hash, fittest species, seeded" do
      let n = 128
      let f = Myc.wild(Myc.build(n), n, uniform(n, 0.5), uniform(n, 0.3), 7)
      let held = Myc.count_species(f, n, 3)
      Test.assert_true(held > 0, "some Meadowbell on grassland")
      Test.assert_eq_int(Myc.count_species(f, n, 4), 0, "no Lanterncap on dry ground")
      Test.assert_eq_int(Myc.state_hash(Myc.wild(Myc.build(n), n, uniform(n, 0.5), uniform(n, 0.3), 7), n), Myc.state_hash(f, n), "same seed, same world")
      Test.assert_true(Myc.state_hash(Myc.wild(Myc.build(n), n, uniform(n, 0.5), uniform(n, 0.3), 8), n) != Myc.state_hash(f, n), "a different seed differs")
      Test.assert_true(held < n * n / 2, "low density: well under half the world")
    end
    test "a dig yields a spore about a quarter of the time" do
      Test.assert_true(dig_count(0, 0) > 150 && dig_count(0, 0) < 350, "roughly 250 of 1000")
    end
    test "the readout names what is there or what would grow" do
      let n = 32
      let f = Myc.plant(Myc.build(n), n, 5, 5, 4)
      Test.assert_true(Myc.readout(f, n, 5, 5, 0.5, 0.7) == "Lanterncap 25%", "held column")
      Test.assert_true(Myc.readout(f, n, 6, 5, 0.5, 0.7) == "bare: Lanterncap 100% would grow", "bare forest ground")
      Test.assert_true(Myc.readout(f, n, 6, 5, 0.01, 0.99) == "bare: nothing grows here", "nothing fits")
    end
  end

  describe "the canal" do
    test "wetting the desert lets Marshlight in and pushes Sunshelf out" do
      let n = 32
      let dry_t = uniform(n, 0.85)
      let dry_m = uniform(n, 0.1)
      let wet_m = uniform(n, 0.7)
      let f0 = Myc.plant(Myc.build(n), n, 16, 16, 6)
      let f1 = tick_n(f0, n, dry_t, dry_m, 0.1, 300)
      Test.assert_true(Myc.count_species(f1, n, 6) > 50, "Sunshelf thrives in the desert")
      Test.assert_eq_int(Myc.count_species(tick_n(Myc.plant(Myc.build(n), n, 16, 16, 5), n, dry_t, dry_m, 0.1, 100), n, 5), 0, "Marshlight cannot live there dry")
      -- the canal: same temperature, damp
      let f2 = tick_n(Myc.plant(f1, n, 10, 16, 5), n, dry_t, wet_m, 0.1, 900)
      Test.assert_true(Myc.count_species(f2, n, 5) > 50, "Marshlight lives there wet")
      Test.assert_true(Myc.count_species(f2, n, 6) < Myc.count_species(f1, n, 6), "and Sunshelf has lost ground")
    end
  end
```

with the helper near `tick_n`:

```march
  pfn dig_count(i : Int, acc : Int) : Int do
    if i >= 1000 do acc else dig_count(i + 1, if Myc.dig_spore(i % 37, 60 + i % 5, i / 37, 7) do acc + 1 else acc end) end
  end
```

- [ ] **Step 2: Run to verify failure.**

- [ ] **Step 3: Implement** in `myc.march` (needs `alias CubeForge.Noise as N` at the top):

```march
  -- ── Mature patches, wild fungus, spores ────────────────────────────────
  -- Octagonal distance in half-hops: straight steps cost 2, diagonal 3, the
  -- same metric the spread uses, so a patch written here is exactly the one
  -- a planting would have grown into.
  pfn oct_cost(dx : Int, dz : Int) : Int do
    let ax = if dx < 0 do 0 - dx else dx end
    let az = if dz < 0 do 0 - dz else dz end
    let hi = if ax > az do ax else az end
    let lo = if ax > az do az else ax end
    2 * hi + lo
  end

  pfn patch_go(sps : NativeU8Arr, vg : NativeFloatArr, rc : NativeU8Arr, n : Int, x : Int, z : Int, sp : Int, r : Int, i : Int) : Field do
    let span = 2 * r + 1
    if i >= span * span do Field(sps, vg, rc, NativeArray.make_u8(0, 0), NativeArray.make_u8(0, 0))
    else
      let dx = i % span - r
      let dz = i / span - r
      let cx = x + dx
      let cz = z + dz
      let cost = oct_cost(dx, dz)
      if cx < 0 || cz < 0 || cx >= n || cz >= n || cost > plant_reach() do patch_go(sps, vg, rc, n, x, z, sp, r, i + 1)
      else
        let j = cx + n * cz
        patch_go(NativeArray.set_u8(sps, j, sp), NativeArray.set_float(vg, j, 1.0), NativeArray.set_u8(rc, j, plant_reach() - cost), n, x, z, sp, r, i + 1)
      end
    end
  end

  doc "A mature patch of [sp] centred on (x, z): the octagon a planting grows into, at full vigour, reach falling with distance. Copies the arrays once."
  fn plant_patch(f : Field, n : Int, x : Int, z : Int, sp : Int) : Field do
    match f do
      Field(sps, vg, rc, hd, cl) ->
        match patch_go(copy_u8(sps, n * n), copy_f(vg, n * n), copy_u8(rc, n * n), n, x, z, sp, plant_radius(), 0) do
          Field(s1, v1, r1, _, _) -> Field(s1, v1, r1, hd, cl)
        end
    end
  end

  doc "Fraction of 16x16 cells that hold a wild patch."
  fn wild_density() : Float do 0.12 end
  fn wild_cell() : Int do 16 end

  pfn wild_go(f : Field, n : Int, ta : NativeFloatArr, ma : NativeFloatArr, seed : Int, c : Int, cells : Int) : Field do
    if c >= cells * cells do f
    else
      let cx = c % cells
      let cz = c / cells
      if N.hash2(cx * 31, cz * 17, seed + 9001) >= wild_density() do wild_go(f, n, ta, ma, seed, c + 1, cells)
      else
        let x = cx * wild_cell() + wild_cell() / 2
        let z = cz * wild_cell() + wild_cell() / 2
        let i = x + n * z
        let sp = S.best_for(NativeArray.get_float(ta, i), NativeArray.get_float(ma, i))
        if sp == 0 do wild_go(f, n, ta, ma, seed, c + 1, cells)
        else wild_go(plant_patch(f, n, x, z, sp), n, ta, ma, seed, c + 1, cells) end
      end
    end
  end

  doc "Seed the world's wild fungus: one mature patch of the fittest species per 16x16 cell the hash picks, against the climate arrays. Seeded, so a world's wild fungus is stable across runs."
  fn wild(f : Field, n : Int, ta : NativeFloatArr, ma : NativeFloatArr, seed : Int) : Field do
    wild_go(f, n, ta, ma, seed, 0, n / wild_cell())
  end

  doc "Whether digging the mycelium block at (x, y, z) yields a spore: about one in four, by a position hash so it cannot be farmed by re-placing."
  fn dig_spore(x : Int, y : Int, z : Int, seed : Int) : Bool do
    N.hash2(x * 3 + y, z * 5 + y, seed + 77) < 0.25
  end

  pfn pct(v : Float) : String do int_to_string(float_to_int(v *. 100.0 +. 0.5)) ++ "%" end
  doc "One line for the HUD: what the column holds, or what would grow there."
  fn readout(f : Field, n : Int, x : Int, z : Int, temp : Float, moist : Float) : String do
    let sp = species_of(f, n, x, z)
    if sp != 0 do S.name(sp) ++ " " ++ pct(vigour_of(f, n, x, z))
    else
      let best = S.best_for(temp, moist)
      if best == 0 do "bare: nothing grows here"
      else "bare: " ++ S.name(best) ++ " " ++ pct(S.fitness(best, temp, moist)) ++ " would grow" end
    end
  end
```

`wild_go` calls `plant_patch` per cell, which copies the arrays each time: 12% of 64 cells is ~8 copies of 200 KB at startup. Fine.

- [ ] **Step 4: Test**, **Step 5: commit** `feat(fungus): mature patches, wild fungus, spore drops, the readout`.

---

### Task 3: Startup wild patches applied before the light flood

**Files:**
- Modify: `lib/cube_forge.march` (`run_session`)

**Interfaces:**
- Produces: `apply_shown_all(w, bio, myc, n, i) : World` — every column whose `wanted` differs from `shown` gets its block and `shown` set (no relight, no marks); `CF_WILD` (default 1; 0 disables wild fungus, for the pinned scenarios that must not change).

- [ ] **Step 1:** In `run_session`, right after `let world_dark = World.generate(...)`:

```march
    -- stage 1a: the climate and the wild fungus. The biome field is pure over
    -- the terrain, and the wild patches are written as blocks NOW, before the
    -- light flood, so glowing wild mycelium is lit by the flood rather than
    -- by a relight per column.
    let biome0 = Biome.build(world_dark, seed)
    let cols = n * 16
    let myc0 = if wild do Myc.wild(Myc.build(cols), cols, Biome.temps(biome0), Biome.moists(biome0), seed) else Myc.build(cols) end
    let world_wild = apply_shown_all(world_dark, biome0, myc0, n, 0)
    println("  wild fungus: " ++ myc_summary(myc0, cols, 1, ""))
```

Then `World.relight_all(world_wild)` instead of `world_dark`, delete the later `let biome0 = Biome.build(world, seed)`, and construct the `Scene` with `biome0` and `myc0`. Note `Biome.build` does not need light or occupancy. Add `wild` to `Knobs` (`CF_WILD`, default 1, as a Bool) and read it in `run_session` from `k`.

```march
  -- Every column at once, for session start: blocks and `shown` only. Light
  -- is flooded afterwards, so nothing here relights.
  pfn apply_shown_all(w : CubeForge.World.World, bio : CubeForge.Biome.Field, myc : CubeForge.Myc.Field, n : Int, i : Int) : CubeForge.World.World do
    let cols = n * 16
    if i >= cols * cols do w
    else
      let x = i % cols
      let z = i / cols
      let want = Myc.wanted(myc, cols, i)
      if want == 0 do apply_shown_all(w, bio, myc, n, i + 1)
      else
        let h = Biome.height_of(bio, x, z)
        let sfc = World.block_at(w, x, h, z)
        let new_id = Chunk.myc_id(Chunk.myc_base(sfc), Species.glow(want))
        if h <= 0 || Biome.is_wet(bio, x, z) || new_id == 0 do apply_shown_all(w, bio, myc, n, i + 1)
        else apply_shown_all(World.set_shown(World.set_block(w, x, h, z, new_id), x, z, want), bio, myc, n, i + 1) end
      end
    end
  end
```

`World.set_block` copies the 64 KB chunk on each call (copy-on-write against the shared generated world); ~1000 wild columns is ~64 MB of memcpy, tens of milliseconds at startup. If the `wild fungus` line's timing (add one) shows more than 200 ms, batch per chunk with `World.apply_cells`; otherwise leave it.

- [ ] **Step 2: Build; run with the map**

```bash
CF_NOMOUSE=1 CF_SEED=7 CF_SUN=45 CF_TIME=0 CF_WEATHER=0 CF_FRAMES=120 CF_AUTOMAP=50 CF_BIOME_MAP=1 CF_MYC_MAP=1 CF_DUMP=<scratch>/wild-map.bmp CF_DUMP_FRAME=100 ./.march/build/debug/cube_forge 2>&1 | grep -i "wild fungus\|mycelium:"
```

Expected: several species with three-figure column counts, and the map shows octagons of different colours across the biomes. Keep as `docs/fungus-wild-map.png`. Then the night shot: `CF_SUN=180`, first person, `CF_FRAMES=120 CF_DUMP_FRAME=100`, near a glowing patch: run the map first, pick a glowing patch's coordinates from... simpler: the dump summary prints the counts; look at the night frame for lit ground. If the spawn has no glowing patch in view, that is fine; record what the frame shows.

- [ ] **Step 3: Budget.** `scratch/frame_budget.sh` — the pinned scenario now has wild fungus (seed 7). If it fails, run it with `CF_WILD=0` to confirm the fungus is the cause, then look at which phase frame grew (`CF_AUTOFLOW` prints) before deciding between lowering `CF_MYC_BUDGET` and moving the mycelium migration to its own slot.

- [ ] **Step 4: Commit** `feat(fungus): wild patches at world start, applied before the light flood`.

---

### Task 4: Planting with spores, the spore drop, and the readout

**Files:**
- Modify: `lib/cube_forge.march`, `lib/cube_forge/hud.march`

**Interfaces:**
- Produces: `plant_spore(sc, x, z, sp) : Scene` (plants when the surface can carry mycelium and is dry; returns the scene unchanged otherwise); interact's place branch routes spore items to it and consumes one on success; interact's break branch adds a spore of `World.shown_at` when `Chunk.is_myc(id)` and `Myc.dig_spore`; `Hud.build_readout(b, text) : F32Buf`; HUD slot 250 for the readout; `Ui` gains the readout buffer and the last string; `CF_AUTOSPORE=<frame>` gives three spores of the fittest species for the column three east of the player at that frame and plants one there through `plant_spore` thirty frames later, printing the inventory before and after.

- [ ] **Step 1: `plant_spore`** beside `plant_near`:

```march
  -- Plant [sp] at column (x, z) if its surface can carry mycelium and is dry.
  -- Returns the scene unchanged (and so consumes nothing) otherwise.
  pfn can_plant(sc, x : Int, z : Int) : Bool do
    let n = World.side(scene_world(sc)) * 16
    if x < 0 || z < 0 || x >= n || z >= n do false
    else
      let h = Biome.height_of(scene_biome(sc), x, z)
      let sfc = World.block_at(scene_world(sc), x, h, z)
      if h <= 0 do false else if Biome.is_wet(scene_biome(sc), x, z) do false
      else Chunk.myc_index_of(Chunk.myc_base(sfc)) >= 0 end end
    end
  end
  pfn plant_spore(sc, x : Int, z : Int, sp : Int) do
    if !can_plant(sc, x, z) do sc
    else scene_with_myc(sc, Myc.plant(scene_myc(sc), World.side(scene_world(sc)) * 16, x, z, sp)) end
  end
```

- [ ] **Step 2: Interact.** In the place branch, after `let id = ...`:

```march
          if !allowed do sc1
          else if Species.is_spore(id) do
            -- a spore plants the column of the block it was used on
            if can_plant(sc1, Ray.bx(hit), Ray.bz(hit)) do
              set_inv(plant_spore(sc1, Ray.bx(hit), Ray.bz(hit), Species.species_of_item(id)), Inv.consume(inv1))
            else sc1 end
          else if id != Chunk.water() && overlaps_player(p, px, py, pz) do sc1
```

(the remaining branches unchanged; one more `end`). In the break branch, replace `set_inv(sc3, Inv.release(Inv.harvest(inv1, id)))` with:

```march
          let inv2 = Inv.harvest(inv1, id)
          let inv3 = if Chunk.is_myc(id) && Myc.dig_spore(Ray.bx(hit), Ray.by_(hit), Ray.bz(hit), seed) do
                       let sp = World.shown_at(world, Ray.bx(hit), Ray.bz(hit))
                       if sp > 0 do Inv.give(inv2, Species.spore_item(sp), 1) else inv2 end
                     else inv2 end
          set_inv(sc3, Inv.release(inv3))
```

`interact` needs `seed`: add it as a parameter and pass it from the call site (the frame loop has `seed`).

- [ ] **Step 3: The readout.** `Ui` gains two fields: `Ui(Inv, hotbar buf, icons buf, key, readout buf : F32Buf, readout : String)`. Update `ui_inv`, `ui_with_inv` and the `Ui(...)` construction at session start (`F32Buf.new(1024), ""`). In `hud.march`:

```march
  doc "One line of text at the top left, in its own buffer."
  fn build_readout(b : CubeForge.F32Buf.F32Buf, t : String) : CubeForge.F32Buf.F32Buf do
    emit_text(B.clear(b), t, -0.97, 0.90, 0.012, 0.016)
  end
```

(check `emit_text`'s argument order and the `px`/`py` glyph size the menu uses, `menu_px`/`menu_py`, and match them). In the frame loop, after the hotbar HUD draw, compute the string from the current hit and rebuild only when it changed:

```march
          let hit2 = scene_hit(sc2)
          let text = if Ray.is_hit(hit2) do
              Myc.readout(scene_myc(sc2), World.side(scene_world(sc2)) * 16, Ray.bx(hit2), Ray.bz(hit2),
                          Biome.temp_of(scene_biome(sc2), Ray.bx(hit2), Ray.bz(hit2)), Biome.moist_of(scene_biome(sc2), Ray.bx(hit2), Ray.bz(hit2)))
            else "" end
          let sc3 = if text == ui_readout(scene_ui(sc2)) do sc2
                    else
                      let rb = Hud.build_readout(ui_readout_buf(scene_ui(sc2)), text)
                      Win.gfx_upload(250, F32Buf.storage(rb), F32Buf.len(rb))
                      scene_with_ui(sc2, ui_with_readout(scene_ui(sc2), rb, text))
                    end
          if ui_readout(scene_ui(sc3)) != "" do Win.gfx_draw_hud(250, F32Buf.len(ui_readout_buf(scene_ui(sc3))) / Vx.floats(), false) else () end
```

with `scene_hit`, `ui_readout`, `ui_readout_buf`, `ui_with_readout` accessors, and `sc3` threaded on to whatever used `sc2` after that point. Confirm slot 250 is unused (`grep -n "250" lib/cube_forge.march native/cf_shim.c`).

- [ ] **Step 4: `CF_AUTOSPORE`.** Knob (`Int`, frame, default -1). Beside `CF_AUTOPLANT`:

```march
          let sc0d3 = if autospore >= 0 && frame == autospore do give_spores(sc0d, player1) else sc0d end
          let sc0d = if autospore >= 0 && frame == autospore + 30 do use_spore(sc0d3, player1) else sc0d3 end
```

(rename the earlier `sc0d` binding to `sc0d_pre` and read it here). Helpers beside `plant_near`:

```march
  pfn east_of(p : CubeForge.Player.Player) : Int do float_to_int(Math.floor(V.x(Player.position(p)))) + 3 end
  pfn z_of(p : CubeForge.Player.Player) : Int do float_to_int(Math.floor(V.z(Player.position(p)))) end
  -- Three spores of the fittest species for the column three east, into the inventory.
  pfn give_spores(sc, p : CubeForge.Player.Player) do
    let sp = Species.best_for(Biome.temp_of(scene_biome(sc), east_of(p), z_of(p)), Biome.moist_of(scene_biome(sc), east_of(p), z_of(p)))
    let sc1 = if sp == 0 do sc else set_inv(sc, Inv.give(ui_inv(scene_ui(sc)), Species.spore_item(sp), 3)) end
    println("gave spores: " ++ inv_string(sc1))
    sc1
  end
  -- Use one spore from the first slot holding spores on the column three east.
  pfn use_spore(sc, p : CubeForge.Player.Player) do
    let inv = ui_inv(scene_ui(sc))
    let s = first_spore_slot(inv, 0)
    if s < 0 do sc
    else
      let sp = Species.species_of_item(Inv.item_at(inv, s))
      let sc1 = plant_spore(sc, east_of(p), z_of(p), sp)
      let sc2 = if can_plant(sc, east_of(p), z_of(p)) do set_inv(sc1, Inv.set_slot(inv, s, Inv.item_at(inv, s), Inv.count_at(inv, s) - 1)) else sc1 end
      println("used spore: " ++ inv_string(sc2))
      sc2
    end
  end
  pfn first_spore_slot(inv : CubeForge.Inventory.Inv, s : Int) : Int do
    if s >= Inv.slots() do -1
    else if Species.is_spore(Inv.item_at(inv, s)) && Inv.count_at(inv, s) > 0 do s
    else first_spore_slot(inv, s + 1) end end
  end
```

- [ ] **Step 5: Build, lint, test, run.**

```bash
CF_NOMOUSE=1 CF_SEED=7 CF_SUN=45 CF_TIME=0 CF_WEATHER=0 CF_WILD=0 CF_FRAMES=2100 CF_AUTOSPORE=100 CF_MYC_RATE=5000 CF_AUTOSPIN=8 CF_DUMP=<scratch>/spore.bmp CF_DUMP_FRAME=2000 ./.march/build/debug/cube_forge 2>&1 | grep -i "gave spores\|used spore\|mycelium\|state:"
```

Expected: `gave spores: inv 0:203x3`, `used spore: inv 0:203x2`, and at the dump a Meadowbell patch. Look at the frame: the hotbar shows the spore icon with a count, and the readout line at the top left names the ground under the crosshair. Keep the frame as `docs/fungus-spores.png`.

- [ ] **Step 6: Budget, results, spec, backlog, commit.** Budget with the default `CF_WILD=1`. `RESULTS.md` `### Spores and planting (fungus phase 4)`: the wild-patch counts and startup cost, the budget with fungus live, the readout's rebuild rule. Spec §7 *As built*. `todos.md` entry. Commit `feat(fungus): spores plant, digging drops them, the readout; phase 4 recorded`.

---

## Self-review

- **Spec §7:** spore item per species (Task 1), sources: fruit (phase 5) and digging (Task 4), wild patches at generation (Tasks 2, 3), planting via the hotbar on a surface block (Task 4), planting on another species' ground allowed (nothing forbids it; `Myc.plant` overwrites), fitness readout (Task 4). §10 canal (Task 2). §9 step 4 complete. `dig_spore_chance()` is `Myc.dig_spore` at one in four.
- **Type consistency:** `Myc.plant_patch(f, n, x, z, sp)`, `Myc.wild(f, n, ta, ma, seed)`, `Myc.readout(f, n, x, z, temp, moist)`, `Inventory.give(i, item, n)`, `Species.spore_item/is_spore/species_of_item`, `Ui` with six fields everywhere.
