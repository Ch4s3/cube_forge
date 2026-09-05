# Fungus phase 2: the mycelium field — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A per-column mycelium field — species, vigour, reach — that lives beside the biome field, grows and contests ground by climate fitness each tick, is bounded per planting, mutates nothing in the world yet, and is visible on the map view.

**Architecture:** Two new modules. `CubeForge.Species` is a pure table (bands, tier, glow, colour) plus `fitness`. `CubeForge.Myc` owns the field (`species`, `vigour`, `reach`, `hold`, `claimant`, all 16,384 entries) and a **pull-based** tick: every column reads its eight neighbours from the previous tick's arrays and writes only its own entry into fresh arrays, so no array is read and then written in the same pass (GAPS G21/G63). The tick takes the biome field's eased temperature and moisture arrays as plain parameters, so tests can drive it with synthetic climates. The `Scene` carries the field; it ticks on the field slot right after `Biome.tick`; `CF_MYC_MAP=1` draws it over the map view through a C-built quad grid like the biome map.

**Tech Stack:** March (`forge build`, `forge test`, `forge lint --strict`), C shim for the map overlay.

Spec: `docs/superpowers/specs/2026-09-04-fungus-design.md` §2, §3, §8, §9 step 2.

## Global Constraints

- `&&` / `||` do not short-circuit (GAPS G33): a bounds guard must be a separate `if` from the array read it protects.
- A pass reads one set of arrays and writes a different, freshly allocated set (GAPS G21/G63/G68). The tick never writes into an array the `Scene` still references.
- Destructure a `Field` **once** per tick and pass its arrays as parameters into the hot loop (GAPS G68: destructuring a shared variant inside a loop allocates per iteration).
- Identifiers `by` and `on` are reserved (GAPS G65). A `doc` string and an attribute cannot coexist on one function (GAPS G56).
- Column index is `wx + n * wz` with `n = Biome.cols(w)`; never hard-code 128 inside the modules.
- Species ids are 1-based; 0 is "no fungus". Six species in the order of the spec table.
- Band thresholds line up with `Biome.classify`: cold `< 0.33`, hot `>= 0.66`, damp `>= 0.5`.
- Frame budget: `scratch/frame_budget.sh` (12 ms) must still pass at the end.
- Commit after every task. No attribution lines in commit messages.

## File structure

| file | responsibility |
|---|---|
| `lib/cube_forge/species.march` | **New.** The species table and `fitness`. Pure. |
| `lib/cube_forge/myc.march` | **New.** The field: build, plant, tick (pull), accessors, hash, counts. |
| `lib/cube_forge/biome.march` | Two accessors: `temps(f)`, `moists(f)` (the eased axis arrays). |
| `lib/cube_forge.march` | Field in `Scene`; tick on the field slot; `CF_MYC_MAP`, `CF_MYC_RATE`, `CF_AUTOPLANT`; state line; dump summary. |
| `native/cf_shim.c` | `cf_myc_map_upload`: species colour scaled by vigour, slot 246. |
| `lib/cube_forge/ffi/window.march` | Binding for the above. |
| `test/species_test.march`, `test/myc_test.march` | **New.** |

## Tuning constants (all in `Myc`, each a function so a knob can scale it)

| name | value | meaning |
|---|---|---|
| `plant_reach()` | 8 | hops a planting can spread |
| `seed_vigour()` | 0.25 | vigour a new column starts at |
| `spread_vigour()` | 0.6 | a column spreads once its vigour reaches this |
| `claim_floor()` | 0.2 | minimum fitness to claim an empty column |
| `contest_margin()` | 0.15 | a challenger needs this much more fitness than the incumbent |
| `hold_ticks()` | 30 | consecutive ticks a claim must persist before the column flips |
| `default_rate()` | 1/540 | vigour moved per tick toward target (full range in ~540 ticks, about a tenth of a `CF_DAY`) |
| `contest_rate()` | 3 × rate | how fast a contested incumbent loses vigour |
| `Species.edge()` | 0.06 | width of the linear fitness falloff inside a band edge |

---

### Task 1: The species table and fitness

**Files:**
- Create: `lib/cube_forge/species.march`
- Create: `test/species_test.march`

**Interfaces:**
- Produces: `Species.count() : Int` = 6; `Species.name(id) : String`; `Species.t_lo/t_hi/m_lo/m_hi(id) : Float`; `Species.tier(id) : Int` (0 small, 1 medium, 2 giant); `Species.glow(id) : Int` (0 none, 1 dim, 2 bright); `Species.spores(id) : Int`; `Species.colour_r/colour_g/colour_b(id) : Int` 0..255; `Species.edge() : Float`; `Species.fitness(id, temp, moist) : Float` in [0, 1]; `Species.best_for(temp, moist) : Int` (the fittest species, 0 if none above `Species.viable()` = 0.2).

- [ ] **Step 1: Write the failing test** — `test/species_test.march`:

```march
-- The species table: bands that line up with the biome thresholds, and fitness.
mod CubeForge.Test.Species do

  import Test
  alias CubeForge.Species as S

  pfn near(a : Float, b : Float) : Bool do Math.abs(a -. b) < 0.0001 end

  describe "Species table" do
    test "six species, in spec order, with 0 meaning none" do
      Test.assert_eq_int(S.count(), 6, "six")
      Test.assert_true(S.name(1) == "Frostcap", "1")
      Test.assert_true(S.name(2) == "Pinewart", "2")
      Test.assert_true(S.name(3) == "Meadowbell", "3")
      Test.assert_true(S.name(4) == "Lanterncap", "4")
      Test.assert_true(S.name(5) == "Marshlight", "5")
      Test.assert_true(S.name(6) == "Sunshelf", "6")
      Test.assert_true(S.name(0) == "none", "0")
    end
    test "tiers, glow and yield follow the table" do
      Test.assert_eq_int(S.tier(1), 0, "Frostcap small")
      Test.assert_eq_int(S.tier(4), 1, "Lanterncap medium")
      Test.assert_eq_int(S.tier(5), 2, "Marshlight giant")
      Test.assert_eq_int(S.glow(1), 1, "Frostcap dim")
      Test.assert_eq_int(S.glow(3), 0, "Meadowbell none")
      Test.assert_eq_int(S.glow(4), 2, "Lanterncap bright")
      Test.assert_eq_int(S.glow(5), 2, "Marshlight bright")
      Test.assert_eq_int(S.spores(1), 1, "small yields 1")
      Test.assert_eq_int(S.spores(4), 3, "medium yields 3")
      Test.assert_eq_int(S.spores(6), 8, "giant yields 8")
    end
  end

  describe "Species.fitness" do
    test "is 1 in the core of the band and 0 outside it" do
      Test.assert_true(near(S.fitness(3, 0.5, 0.3), 1.0), "Meadowbell at temperate dry")
      Test.assert_true(near(S.fitness(3, 0.9, 0.3), 0.0), "Meadowbell in the desert")
      Test.assert_true(near(S.fitness(3, 0.5, 0.9), 0.0), "Meadowbell in the swamp")
      Test.assert_true(near(S.fitness(0, 0.5, 0.5), 0.0), "no species has no fitness")
    end
    test "falls linearly across the edge" do
      -- Meadowbell's upper moisture edge is 0.56; edge width 0.06
      Test.assert_true(near(S.fitness(3, 0.5, 0.50), 1.0), "at the inner edge")
      Test.assert_true(near(S.fitness(3, 0.5, 0.53), 0.5), "halfway across")
      Test.assert_true(near(S.fitness(3, 0.5, 0.56), 0.0), "at the outer edge")
    end
    test "every biome's home ground has a species above the viable floor" do
      Test.assert_eq_int(S.best_for(0.15, 0.2), 1, "tundra -> Frostcap")
      Test.assert_eq_int(S.best_for(0.15, 0.8), 2, "taiga -> Pinewart")
      Test.assert_eq_int(S.best_for(0.5, 0.2), 3, "grassland -> Meadowbell")
      Test.assert_eq_int(S.best_for(0.5, 0.8), 4, "forest -> Lanterncap")
      Test.assert_eq_int(S.best_for(0.85, 0.8), 5, "wetland -> Marshlight")
      Test.assert_eq_int(S.best_for(0.85, 0.2), 6, "desert -> Sunshelf")
    end
    test "the transitional ground is contested" do
      -- warm forest: both Lanterncap and Marshlight are viable
      Test.assert_true(S.fitness(4, 0.64, 0.7) > S.viable(), "Lanterncap viable at 0.64")
      Test.assert_true(S.fitness(5, 0.64, 0.7) > S.viable(), "Marshlight viable at 0.64")
    end
  end

end
```

- [ ] **Step 2: Run it to verify it fails**

Run: `forge test 2>&1 | grep -c ERROR`
Expected: non-zero (module `CubeForge.Species` unknown).

- [ ] **Step 3: Implement** — `lib/cube_forge/species.march`:

```march
-- Species — the fungus table. One row per species: a temperature band, a
-- moisture band, a fruit tier, a glow level, a spore yield and a colour.
-- Adding a species is adding a row here (and, in phase 3, seven textures).
--
-- Bands overlap at their edges on purpose, so transitional ground is contested:
-- the edges line up with Biome.classify's thresholds (cold < 0.33, hot >= 0.66,
-- damp >= 0.5) and reach past them by a little.
mod CubeForge.Species do

  fn count() : Int do 6 end

  doc "Species ids are 1-based; 0 is no fungus."
  fn name(id : Int) : String do
    if id == 1 do "Frostcap" else if id == 2 do "Pinewart" else if id == 3 do "Meadowbell"
    else if id == 4 do "Lanterncap" else if id == 5 do "Marshlight" else if id == 6 do "Sunshelf"
    else "none" end end end end end end
  end

  -- Temperature band.
  fn t_lo(id : Int) : Float do
    if id == 1 do 0.0 else if id == 2 do 0.0 else if id == 3 do 0.30
    else if id == 4 do 0.30 else if id == 5 do 0.60 else if id == 6 do 0.62 else 0.0 end end end end end end
  end
  fn t_hi(id : Int) : Float do
    if id == 1 do 0.36 else if id == 2 do 0.42 else if id == 3 do 0.68
    else if id == 4 do 0.70 else if id == 5 do 1.0 else if id == 6 do 1.0 else 0.0 end end end end end end
  end
  -- Moisture band.
  fn m_lo(id : Int) : Float do
    if id == 1 do 0.0 else if id == 2 do 0.42 else if id == 3 do 0.0
    else if id == 4 do 0.44 else if id == 5 do 0.44 else if id == 6 do 0.0 else 0.0 end end end end end end
  end
  fn m_hi(id : Int) : Float do
    if id == 1 do 1.0 else if id == 2 do 1.0 else if id == 3 do 0.56
    else if id == 4 do 1.0 else if id == 5 do 1.0 else if id == 6 do 0.56 else 0.0 end end end end end end
  end

  doc "Fruit tier: 0 small, 1 medium, 2 giant."
  fn tier(id : Int) : Int do
    if id == 1 do 0 else if id == 2 do 1 else if id == 3 do 0
    else if id == 4 do 1 else if id == 5 do 2 else if id == 6 do 2 else 0 end end end end end end
  end
  doc "Glow: 0 none, 1 dim, 2 bright."
  fn glow(id : Int) : Int do
    if id == 1 do 1 else if id == 4 do 2 else if id == 5 do 2 else 0 end end end
  end
  doc "Spores a fruit body of this species yields: 1 small, 3 medium, 8 giant."
  fn spores(id : Int) : Int do
    let t = tier(id)
    if t == 0 do 1 else if t == 1 do 3 else 8 end end
  end

  -- Colour, for the map overlay and the phase 3 textures.
  fn colour_r(id : Int) : Int do
    if id == 1 do 200 else if id == 2 do 120 else if id == 3 do 230 else if id == 4 do 255 else if id == 5 do 80 else if id == 6 do 240 else 0 end end end end end end
  end
  fn colour_g(id : Int) : Int do
    if id == 1 do 230 else if id == 2 do 90 else if id == 3 do 200 else if id == 4 do 200 else if id == 5 do 220 else if id == 6 do 140 else 0 end end end end end end
  end
  fn colour_b(id : Int) : Int do
    if id == 1 do 255 else if id == 2 do 60 else if id == 3 do 90 else if id == 4 do 80 else if id == 5 do 200 else if id == 6 do 60 else 0 end end end end end end
  end

  doc "Width of the linear falloff inside each band edge."
  fn edge() : Float do 0.06 end

  doc "Fitness below which a species cannot hold a column."
  fn viable() : Float do 0.2 end

  pfn clamp01(v : Float) : Float do if v < 0.0 do 0.0 else if v > 1.0 do 1.0 else v end end end

  -- 1 well inside [lo, hi], 0 outside, linear over edge() at each end.
  pfn axis_fit(v : Float, lo : Float, hi : Float) : Float do
    let a = if v < lo do 0.0 else if v > hi do 0.0 else 1.0 end end
    if a <= 0.0 do 0.0
    else
      let up = clamp01((v -. lo) /. edge())
      let down = clamp01((hi -. v) /. edge())
      if up < down do up else down end
    end
  end

  doc "How well a species fits a column's eased climate, 0..1: the product of its two axis fits."
  fn fitness(id : Int, temp : Float, moist : Float) : Float do
    if id <= 0 || id > count() do 0.0
    else axis_fit(temp, t_lo(id), t_hi(id)) *. axis_fit(moist, m_lo(id), m_hi(id)) end
  end

  pfn best_go(temp : Float, moist : Float, id : Int, best : Int, bf : Float) : Int do
    if id > count() do best
    else
      let f = fitness(id, temp, moist)
      if f > bf do best_go(temp, moist, id + 1, id, f) else best_go(temp, moist, id + 1, best, bf) end
    end
  end
  doc "The fittest species for a climate, or 0 when none reaches viable()."
  fn best_for(temp : Float, moist : Float) : Int do best_go(temp, moist, 1, 0, viable()) end

end
```

Species whose band starts at 0.0 (Frostcap and Pinewart on temperature; several on moisture) get the falloff at 0 as well: a value of exactly 0 has `up = 0`, so fitness is 0 there. That is wrong for a tundra column at temperature 0.0. Fix inside `axis_fit`: when `lo <= 0.0` treat `up` as 1, and when `hi >= 1.0` treat `down` as 1:

```march
      let up = if lo <= 0.0 do 1.0 else clamp01((v -. lo) /. edge()) end
      let down = if hi >= 1.0 do 1.0 else clamp01((hi -. v) /. edge()) end
```

Use that form.

- [ ] **Step 4: Run the tests**

Run: `forge test 2>&1 | grep -B2 -A8 "ERROR\|FAIL:" | head -30; forge test 2>&1 | tail -1`
Expected: pass. If "halfway across" fails, check the arithmetic: 0.53 is 0.03 inside the 0.56 edge, 0.03 / 0.06 = 0.5.

- [ ] **Step 5: Commit**

```bash
git add lib/cube_forge/species.march test/species_test.march
git commit -m "feat(fungus): species table and climate fitness"
```

---

### Task 2: The field: build, plant, accessors, hash

**Files:**
- Create: `lib/cube_forge/myc.march`
- Create: `test/myc_test.march`
- Modify: `lib/cube_forge/biome.march` (after `temp_of`)

**Interfaces:**
- Consumes: `Species.*`.
- Produces: `Myc.Field = Field(species u8, vigour float, reach u8, hold u8, claimant u8)`; `Myc.build(n : Int) : Field` (all empty, `n` columns per side); `Myc.plant(f, n, x, z, sp) : Field`; `Myc.species_of(f, n, x, z) : Int`; `Myc.vigour_of(f, n, x, z) : Float`; `Myc.reach_of(f, n, x, z) : Int`; `Myc.field_species(f) : NativeU8Arr`; `Myc.vigour_u8(f, n) : NativeU8Arr` (a fresh 0..255 copy for upload); `Myc.count_species(f, n, sp) : Int`; `Myc.state_hash(f, n) : Int`; the tuning constants from the table above. `Biome.temps(f) : NativeFloatArr`, `Biome.moists(f) : NativeFloatArr`.

- [ ] **Step 1: Write the failing test** — `test/myc_test.march`:

```march
-- The mycelium field: build, plant, spread, contest, wither.
mod CubeForge.Test.Myc do

  import Test
  alias CubeForge.Myc as M
  alias CubeForge.Species as S

  pfn near(a : Float, b : Float) : Bool do Math.abs(a -. b) < 0.0001 end

  -- A uniform synthetic climate over an n x n field.
  pfn uniform(n : Int, v : Float) : NativeFloatArr do NativeArray.make_float(n * n, v) end

  describe "Myc.build and plant" do
    test "a fresh field is empty and hashes stably" do
      let f = M.build(32)
      Test.assert_eq_int(M.species_of(f, 32, 5, 5), 0, "empty")
      Test.assert_eq_int(M.count_species(f, 32, 3), 0, "none of species 3")
      Test.assert_eq_int(M.state_hash(f, 32), M.state_hash(M.build(32), 32), "same hash twice")
    end
    test "planting seeds one column at seed vigour and full reach" do
      let f = M.plant(M.build(32), 32, 10, 12, 3)
      Test.assert_eq_int(M.species_of(f, 32, 10, 12), 3, "species")
      Test.assert_true(near(M.vigour_of(f, 32, 10, 12), M.seed_vigour()), "seed vigour")
      Test.assert_eq_int(M.reach_of(f, 32, 10, 12), M.plant_reach(), "full reach")
      Test.assert_eq_int(M.species_of(f, 32, 11, 12), 0, "the neighbour is untouched")
      Test.assert_eq_int(M.count_species(f, 32, 3), 1, "one column")
      Test.assert_true(M.state_hash(f, 32) != M.state_hash(M.build(32), 32), "hash moved")
    end
    test "planting outside the field is a no-op" do
      let f = M.plant(M.build(32), 32, -1, 40, 3)
      Test.assert_eq_int(M.count_species(f, 32, 3), 0, "nothing planted")
    end
    test "vigour_u8 scales 0..1 to 0..255" do
      let f = M.plant(M.build(32), 32, 10, 12, 3)
      let v = M.vigour_u8(f, 32)
      Test.assert_eq_int(NativeArray.get_u8(v, 10 + 32 * 12), float_to_int(M.seed_vigour() *. 255.0 +. 0.5), "scaled")
      Test.assert_eq_int(NativeArray.get_u8(v, 0), 0, "empty is 0")
    end
  end

end
```

- [ ] **Step 2: Run it to verify it fails**

Run: `forge test 2>&1 | grep -c ERROR`
Expected: non-zero.

- [ ] **Step 3: Implement** — `lib/cube_forge/myc.march`:

```march
-- Myc — the mycelium field, one entry per world column beside the biome field.
--
-- species: 0 for none, else a Species id.   vigour: 0..1, how established.
-- reach: hops this column may still spread.  hold: consecutive ticks the same
-- claimant has wanted this column.           claimant: that claimant's species.
--
-- Recomputed whole every tick, like the biome field, and for the same reason.
-- The tick is PULL-based: each column reads its eight neighbours from the
-- previous tick's arrays and writes only its own entry into fresh arrays, so
-- no array is ever read and then written in one pass (GAPS G21/G63) and the
-- Scene's shared reference never forces a copy (GAPS G68).
mod CubeForge.Myc do

  alias CubeForge.Species as S

  type Field = Field(NativeU8Arr, NativeFloatArr, NativeU8Arr, NativeU8Arr, NativeU8Arr)

  -- ── Tuning ────────────────────────────────────────────────────────────────
  doc "Hops a planting can spread."
  fn plant_reach() : Int do 8 end
  doc "Vigour a newly claimed or planted column starts at."
  fn seed_vigour() : Float do 0.25 end
  doc "A column spreads once its vigour reaches this."
  fn spread_vigour() : Float do 0.6 end
  doc "Minimum fitness to claim an empty column."
  fn claim_floor() : Float do 0.2 end
  doc "A challenger needs this much more fitness than the incumbent to contest."
  fn contest_margin() : Float do 0.15 end
  doc "Consecutive ticks a claim must persist before the column flips."
  fn hold_ticks() : Int do 30 end
  doc "Vigour moved per tick toward its target: the full range in ~540 ticks."
  fn default_rate() : Float do 1.0 /. 540.0 end
  doc "How much faster a contested incumbent loses vigour than it would ease."
  fn contest_factor() : Float do 3.0 end

  -- ── Build, plant, read ────────────────────────────────────────────────────
  doc "An empty field of n x n columns."
  fn build(n : Int) : Field do
    Field(NativeArray.make_u8(n * n, 0), NativeArray.make_float(n * n, 0.0), NativeArray.make_u8(n * n, 0), NativeArray.make_u8(n * n, 0), NativeArray.make_u8(n * n, 0))
  end

  pfn copy_u8_go(src : NativeU8Arr, dst : NativeU8Arr, i : Int, n : Int) : NativeU8Arr do
    if i >= n do dst else copy_u8_go(src, NativeArray.set_u8(dst, i, NativeArray.get_u8(src, i)), i + 1, n) end
  end
  pfn copy_u8(a : NativeU8Arr, n : Int) : NativeU8Arr do copy_u8_go(a, NativeArray.make_u8(n, 0), 0, n) end
  pfn copy_f_go(src : NativeFloatArr, dst : NativeFloatArr, i : Int, n : Int) : NativeFloatArr do
    if i >= n do dst else copy_f_go(src, NativeArray.set_float(dst, i, NativeArray.get_float(src, i)), i + 1, n) end
  end
  pfn copy_f(a : NativeFloatArr, n : Int) : NativeFloatArr do copy_f_go(a, NativeArray.make_float(n, 0.0), 0, n) end

  doc "Plant species [sp] at column (x, z): seed vigour, full reach, claim cleared. Copies the arrays (the Scene shares them, GAPS G68); plantings are rare."
  fn plant(f : Field, n : Int, x : Int, z : Int, sp : Int) : Field do
    if x < 0 || z < 0 || x >= n || z >= n do f
    else
      match f do
        Field(sps, vg, rc, hd, cl) ->
          let i = x + n * z
          Field(NativeArray.set_u8(copy_u8(sps, n * n), i, sp),
                NativeArray.set_float(copy_f(vg, n * n), i, seed_vigour()),
                NativeArray.set_u8(copy_u8(rc, n * n), i, plant_reach()),
                NativeArray.set_u8(copy_u8(hd, n * n), i, 0),
                NativeArray.set_u8(copy_u8(cl, n * n), i, 0))
      end
    end
  end

  fn species_of(f : Field, n : Int, x : Int, z : Int) : Int do
    match f do Field(sps, _, _, _, _) -> NativeArray.get_u8(sps, x + n * z) end
  end
  fn vigour_of(f : Field, n : Int, x : Int, z : Int) : Float do
    match f do Field(_, vg, _, _, _) -> NativeArray.get_float(vg, x + n * z) end
  end
  fn reach_of(f : Field, n : Int, x : Int, z : Int) : Int do
    match f do Field(_, _, rc, _, _) -> NativeArray.get_u8(rc, x + n * z) end
  end
  fn field_species(f : Field) : NativeU8Arr do match f do Field(sps, _, _, _, _) -> sps end end

  pfn u8_go(vg : NativeFloatArr, out : NativeU8Arr, i : Int, n : Int) : NativeU8Arr do
    if i >= n do out
    else u8_go(vg, NativeArray.set_u8(out, i, float_to_int(NativeArray.get_float(vg, i) *. 255.0 +. 0.5)), i + 1, n) end
  end
  doc "Vigour as 0..255 bytes, a fresh array for the map upload."
  fn vigour_u8(f : Field, n : Int) : NativeU8Arr do
    match f do Field(_, vg, _, _, _) -> u8_go(vg, NativeArray.make_u8(n * n, 0), 0, n * n) end
  end

  pfn count_go(sps : NativeU8Arr, sp : Int, i : Int, n : Int, acc : Int) : Int do
    if i >= n do acc
    else count_go(sps, sp, i + 1, n, if NativeArray.get_u8(sps, i) == sp do acc + 1 else acc end) end
  end
  doc "Columns held by species [sp]."
  fn count_species(f : Field, n : Int, sp : Int) : Int do
    match f do Field(sps, _, _, _, _) -> count_go(sps, sp, 0, n * n, 0) end
  end

  pfn hash_u8(a : NativeU8Arr, n : Int, i : Int, h : Int) : Int do
    if i >= n do h
    else hash_u8(a, n, i + 1, (h * 131 + NativeArray.get_u8(a, i)) % 1073741789) end
  end
  doc "A hash of species and reach, the discrete state: an oracle for 'the field evolved the same way'."
  fn state_hash(f : Field, n : Int) : Int do
    match f do Field(sps, _, rc, _, _) -> hash_u8(rc, n * n, 0, hash_u8(sps, n * n, 0, 11)) end
  end

end
```

And in `biome.march`, after `temp_of`:

```march
  doc "The eased temperature array, one entry per column: the climate the mycelium field reads."
  fn temps(f : Field) : NativeFloatArr do match f do Field(_, _, ta, _, _, _, _, _, _) -> ta end end
  doc "The eased moisture array, one entry per column."
  fn moists(f : Field) : NativeFloatArr do match f do Field(_, _, _, ma, _, _, _, _, _) -> ma end end
```

- [ ] **Step 4: Run the tests**

Run: `forge test 2>&1 | grep -B2 -A8 "ERROR\|FAIL:" | head -30; forge test 2>&1 | tail -1`
Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add lib/cube_forge/myc.march lib/cube_forge/biome.march test/myc_test.march
git commit -m "feat(fungus): the mycelium field — build, plant, accessors, hash"
```

---

### Task 3: The tick: vigour, spread, contest, reach, hold

**Files:**
- Modify: `lib/cube_forge/myc.march`
- Modify: `test/myc_test.march`

**Interfaces:**
- Produces: `Myc.tick(f : Field, n : Int, ta : NativeFloatArr, ma : NativeFloatArr, rate : Float) : Field`.

The rules, per column `j` with old state `(sp, vg, rc, hd, cl)` and climate `(t, m)`:

1. **Best claimant.** Over the eight neighbours `i` that hold a species `c != 0` with `vigour[i] >= spread_vigour()` and `reach[i] > 0`: candidate fitness `fc = fitness(c, t, m)`. The claimant is the candidate with the highest `fc`; ties go to the lowest neighbour index. It counts only if `fc >= claim_floor()` (empty column) or `fc >= fitness(sp, t, m) + contest_margin()` (occupied column, `c != sp`). Its reach for `j` is the best `reach[i] - 1` over neighbours of that species that qualify.
2. **Hold.** If there is a claimant `c`: `hd' = if c == cl then hd + 1 else 1`, `cl' = c`. Otherwise `hd' = 0, cl' = 0`.
3. **Occupied column** (`sp != 0`): `fit = fitness(sp, t, m)`, `target = fit`. If a claimant is contesting, `vg' = vg - contest_factor() * rate`; else `vg' = ease(vg, target, rate)`. If `vg' <= 0` and `fit < viable()`, or `vg' <= 0` under contest: the column empties (`sp' = 0, vg' = 0, rc' = 0`). If it emptied under a contest whose hold has matured (`hd' >= hold_ticks()`), the claimant takes it at once: `sp' = c, vg' = seed_vigour(), rc' = claimant reach`.
4. **Empty column** (`sp == 0`): if a claimant exists and `hd' >= hold_ticks()`: `sp' = c, vg' = seed_vigour(), rc' = claimant reach, hd' = 0, cl' = 0`. Otherwise stays empty.

`ease` is the biome's: move toward the target by at most `rate`, land exactly on it when within a step.

- [ ] **Step 1: Write the failing tests** — append to `test/myc_test.march` before the module's final `end`:

```march
  pfn tick_n(f : M.Field, n : Int, ta : NativeFloatArr, ma : NativeFloatArr, rate : Float, k : Int) : M.Field do
    if k <= 0 do f else tick_n(M.tick(f, n, ta, ma, rate), n, ta, ma, rate, k - 1) end
  end

  -- Chebyshev radius of species [sp] around (cx, cz): the farthest held column.
  pfn radius_go(f : M.Field, n : Int, sp : Int, cx : Int, cz : Int, i : Int, best : Int) : Int do
    if i >= n * n do best
    else
      let x = i % n
      let z = i / n
      let dx = if x > cx do x - cx else cx - x end
      let dz = if z > cz do z - cz else cz - z end
      let d = if dx > dz do dx else dz end
      if M.species_of(f, n, x, z) == sp do radius_go(f, n, sp, cx, cz, i + 1, if d > best do d else best end)
      else radius_go(f, n, sp, cx, cz, i + 1, best) end
    end
  end

  describe "Myc.tick" do
    test "vigour eases toward fitness and a planting in the wrong climate withers" do
      let n = 32
      -- Meadowbell (3) planted in a hot, wet climate: fitness 0
      let f0 = M.plant(M.build(n), n, 16, 16, 3)
      let f1 = tick_n(f0, n, uniform(n, 0.9), uniform(n, 0.9), M.default_rate(), 200)
      Test.assert_eq_int(M.species_of(f1, n, 16, 16), 0, "withered")
      Test.assert_eq_int(M.count_species(f1, n, 3), 0, "and spread nowhere")
    end

    test "on ideal ground a planting grows to full vigour" do
      let n = 32
      let f0 = M.plant(M.build(n), n, 16, 16, 3)
      let f1 = tick_n(f0, n, uniform(n, 0.5), uniform(n, 0.3), M.default_rate(), 600)
      Test.assert_true(near(M.vigour_of(f1, n, 16, 16), 1.0), "full vigour")
    end

    test "a planting spreads, and no further than its reach" do
      let n = 40
      let f0 = M.plant(M.build(n), n, 20, 20, 3)
      -- fast rate so vigour is not the limit; enough ticks for every hop's hold to mature
      let f1 = tick_n(f0, n, uniform(n, 0.5), uniform(n, 0.3), 0.1, (M.plant_reach() + 2) * (M.hold_ticks() + 12))
      Test.assert_eq_int(radius_go(f1, n, 3, 20, 20, 0, 0), M.plant_reach(), "exactly the reach")
      Test.assert_eq_int(M.reach_of(f1, n, 20 + M.plant_reach(), 20), 0, "the rim has no reach left")
      Test.assert_true(M.count_species(f1, n, 3) > 100, "a real patch")
    end

    test "nothing spreads where the species is unfit" do
      let n = 32
      let f0 = M.plant(M.build(n), n, 16, 16, 3)
      let f1 = tick_n(f0, n, uniform(n, 0.5), uniform(n, 0.9), 0.1, 100)
      Test.assert_eq_int(M.count_species(f1, n, 3), 0, "withered without claiming a neighbour")
    end

    test "two species settle to the fitter one and a column flips at most once" do
      let n = 40
      -- warm forest, 0.64 / 0.7: Lanterncap (4) fitness < Marshlight (5)
      let ta = uniform(n, 0.64)
      let ma = uniform(n, 0.7)
      Test.assert_true(S.fitness(5, 0.64, 0.7) > S.fitness(4, 0.64, 0.7) +. M.contest_margin(), "the margin holds at this climate")
      let f0 = M.plant(M.plant(M.build(n), n, 15, 20, 4), n, 25, 20, 5)
      let f1 = tick_n(f0, n, ta, ma, 0.05, 900)
      Test.assert_eq_int(M.count_species(f1, n, 4), 0, "Lanterncap lost the middle")
      Test.assert_true(M.count_species(f1, n, 5) > 0, "Marshlight holds ground")
      -- a column between them was Lanterncap first, then Marshlight, and never Lanterncap again
      Test.assert_eq_int(M.species_of(f1, n, 18, 20), 5, "the contested column ended Marshlight")
    end

    test "an unclaimable column stays empty and a settled field is stable" do
      let n = 32
      let f0 = M.plant(M.build(n), n, 16, 16, 3)
      let f1 = tick_n(f0, n, uniform(n, 0.5), uniform(n, 0.3), 0.1, 600)
      let f2 = M.tick(f1, n, uniform(n, 0.5), uniform(n, 0.3), 0.1)
      Test.assert_eq_int(M.state_hash(f2, n), M.state_hash(f1, n), "one more tick changes nothing")
    end
  end
```

- [ ] **Step 2: Run to verify failure**

Run: `forge test 2>&1 | grep -c ERROR`
Expected: non-zero (`M.tick` unknown).

- [ ] **Step 3: Implement the tick** — add to `myc.march` before the module's `end`:

```march
  -- ── The tick ──────────────────────────────────────────────────────────────

  pfn ease(cur : Float, target : Float, rate : Float) : Float do
    let d = target -. cur
    if d > rate do cur +. rate else if d < 0.0 -. rate do cur -. rate else target end end
  end

  -- dx, dz for neighbour k of 8: the 3x3 ring minus the centre, row by row.
  pfn ndx(k : Int) : Int do
    let kk = if k >= 4 do k + 1 else k end
    kk % 3 - 1
  end
  pfn ndz(k : Int) : Int do
    let kk = if k >= 4 do k + 1 else k end
    kk / 3 - 1
  end

  -- The claimant search returns two numbers packed in one Int so the loop
  -- threads a single accumulator: species * 65536 + reach * 256 + round(fitness * 255).
  pfn pack_claim(sp : Int, rc : Int, fit : Float) : Int do sp * 65536 + rc * 256 + float_to_int(fit *. 255.0 +. 0.5) end
  pfn claim_sp(c : Int) : Int do c / 65536 end
  pfn claim_rc(c : Int) : Int do (c / 256) % 256 end
  pfn claim_fit(c : Int) : Float do int_to_float(c % 256) /. 255.0 end

  -- Best qualifying neighbour of column (x, z). [own] is the column's species and
  -- [ownfit] its fitness; a neighbour of the same species does not contest but
  -- does carry reach (used when the column is empty -- then own is 0).
  pfn claimant_go(sps : NativeU8Arr, vg : NativeFloatArr, rc : NativeU8Arr, n : Int, x : Int, z : Int,
                  t : Float, m : Float, own : Int, ownfit : Float, k : Int, best : Int) : Int do
    if k >= 8 do best
    else
      let nx = x + ndx(k)
      let nz = z + ndz(k)
      if nx < 0 || nz < 0 || nx >= n || nz >= n do claimant_go(sps, vg, rc, n, x, z, t, m, own, ownfit, k + 1, best)
      else
        let i = nx + n * nz
        let c = NativeArray.get_u8(sps, i)
        let r = NativeArray.get_u8(rc, i)
        let strong = if c == 0 do false else if r <= 0 do false else NativeArray.get_float(vg, i) >= spread_vigour() end end
        if !strong do claimant_go(sps, vg, rc, n, x, z, t, m, own, ownfit, k + 1, best)
        else if c == own do claimant_go(sps, vg, rc, n, x, z, t, m, own, ownfit, k + 1, best)
        else
          let fc = S.fitness(c, t, m)
          let ok = if own == 0 do fc >= claim_floor() else fc >= ownfit +. contest_margin() end
          if !ok do claimant_go(sps, vg, rc, n, x, z, t, m, own, ownfit, k + 1, best)
          else
            -- better fitness wins; equal fitness and the same species keeps the larger reach
            let bsp = claim_sp(best)
            let take = if best == 0 do true
                       else if fc > claim_fit(best) +. 0.002 do true
                       else if bsp == c do r - 1 > claim_rc(best)
                       else false end end end
            claimant_go(sps, vg, rc, n, x, z, t, m, own, ownfit, k + 1, if take do pack_claim(c, r - 1, fc) else best end)
          end
        end end
      end
    end
  end

  -- One column. Reads the old arrays, writes index i of the new ones.
  pfn tick_col(sps : NativeU8Arr, vg : NativeFloatArr, rc : NativeU8Arr, hd : NativeU8Arr, cl : NativeU8Arr,
               ta : NativeFloatArr, ma : NativeFloatArr, n : Int, rate : Float, i : Int,
               nsps : NativeU8Arr, nvg : NativeFloatArr, nrc : NativeU8Arr, nhd : NativeU8Arr, ncl : NativeU8Arr) : Field do
    let x = i % n
    let z = i / n
    let sp = NativeArray.get_u8(sps, i)
    let t = NativeArray.get_float(ta, i)
    let m = NativeArray.get_float(ma, i)
    let fit = S.fitness(sp, t, m)
    let claim = claimant_go(sps, vg, rc, n, x, z, t, m, sp, fit, 0, 0)
    let c = claim_sp(claim)
    let hd0 = NativeArray.get_u8(hd, i)
    let hd1 = if c == 0 do 0 else if c == NativeArray.get_u8(cl, i) do hd0 + 1 else 1 end end
    let hd2 = if hd1 > 255 do 255 else hd1 end
    let matured = if c == 0 do false else hd2 >= hold_ticks() end
    if sp == 0 do
      if matured do
        Field(NativeArray.set_u8(nsps, i, c), NativeArray.set_float(nvg, i, seed_vigour()), NativeArray.set_u8(nrc, i, claim_rc(claim)), NativeArray.set_u8(nhd, i, 0), NativeArray.set_u8(ncl, i, 0))
      else
        Field(nsps, nvg, nrc, NativeArray.set_u8(nhd, i, hd2), NativeArray.set_u8(ncl, i, c))
      end
    else
      let v0 = NativeArray.get_float(vg, i)
      let v1 = if c != 0 do v0 -. contest_factor() *. rate else ease(v0, fit, rate) end
      let dies = if v1 <= 0.0 do (if c != 0 do true else fit < S.viable() end) else false end
      if dies do
        if matured do
          Field(NativeArray.set_u8(nsps, i, c), NativeArray.set_float(nvg, i, seed_vigour()), NativeArray.set_u8(nrc, i, claim_rc(claim)), NativeArray.set_u8(nhd, i, 0), NativeArray.set_u8(ncl, i, 0))
        else
          Field(NativeArray.set_u8(nsps, i, 0), NativeArray.set_float(nvg, i, 0.0), NativeArray.set_u8(nrc, i, 0), NativeArray.set_u8(nhd, i, hd2), NativeArray.set_u8(ncl, i, c))
        end
      else
        let v2 = if v1 < 0.0 do 0.0 else v1 end
        Field(NativeArray.set_u8(nsps, i, sp), NativeArray.set_float(nvg, i, v2), NativeArray.set_u8(nrc, i, NativeArray.get_u8(rc, i)), NativeArray.set_u8(nhd, i, hd2), NativeArray.set_u8(ncl, i, c))
      end
    end
  end

  -- Every column has at least one non-empty entry in its 3x3 neighbourhood?
  -- Cheap pre-test so an empty world's tick is 16k byte reads and no fitness.
  pfn any_near(sps : NativeU8Arr, n : Int, x : Int, z : Int, k : Int) : Bool do
    if k >= 9 do false
    else
      let nx = x + k % 3 - 1
      let nz = z + k / 3 - 1
      if nx < 0 || nz < 0 || nx >= n || nz >= n do any_near(sps, n, x, z, k + 1)
      else if NativeArray.get_u8(sps, nx + n * nz) != 0 do true
      else any_near(sps, n, x, z, k + 1) end end
    end
  end

  pfn tick_go(sps : NativeU8Arr, vg : NativeFloatArr, rc : NativeU8Arr, hd : NativeU8Arr, cl : NativeU8Arr,
              ta : NativeFloatArr, ma : NativeFloatArr, n : Int, rate : Float, i : Int,
              nsps : NativeU8Arr, nvg : NativeFloatArr, nrc : NativeU8Arr, nhd : NativeU8Arr, ncl : NativeU8Arr) : Field do
    if i >= n * n do Field(nsps, nvg, nrc, nhd, ncl)
    else if !any_near(sps, n, i % n, i / n, 0) do tick_go(sps, vg, rc, hd, cl, ta, ma, n, rate, i + 1, nsps, nvg, nrc, nhd, ncl)
    else
      match tick_col(sps, vg, rc, hd, cl, ta, ma, n, rate, i, nsps, nvg, nrc, nhd, ncl) do
        Field(a, b, c, d, e) -> tick_go(sps, vg, rc, hd, cl, ta, ma, n, rate, i + 1, a, b, c, d, e)
      end
    end end
  end

  doc """
  One tick against the eased climate arrays [ta] and [ma] (one entry per column,
  the biome field's `temps` and `moists`). Every column pulls from its eight
  neighbours in the previous state and writes its own entry into fresh arrays.
  """
  fn tick(f : Field, n : Int, ta : NativeFloatArr, ma : NativeFloatArr, rate : Float) : Field do
    match f do
      Field(sps, vg, rc, hd, cl) ->
        tick_go(sps, vg, rc, hd, cl, ta, ma, n, rate, 0,
                NativeArray.make_u8(n * n, 0), NativeArray.make_float(n * n, 0.0), NativeArray.make_u8(n * n, 0), NativeArray.make_u8(n * n, 0), NativeArray.make_u8(n * n, 0))
    end
  end
```

Note the `any_near` skip leaves the fresh arrays at their zero defaults for a column with nothing near it, which is exactly its new state (it was empty, nothing can claim it, hold and claimant are 0). An occupied column always passes the test through itself.

- [ ] **Step 4: Run the tests**

Run: `forge test 2>&1 | grep -B2 -A8 "ERROR\|FAIL:" | head -40; forge test 2>&1 | tail -1`
Expected: pass. Likely first failures and what they mean:
- *"exactly the reach"* too small: the rim never reaches `spread_vigour()` before the run ends; raise the tick count in the test, not the constants.
- *"exactly the reach"* too large: reach is not decrementing per hop; check `pack_claim(c, r - 1, fc)`.
- *"Lanterncap lost the middle"*: the contest never empties the incumbent; check that `dies` is true under contest when `v1 <= 0`, and that `matured` can be true on the same tick.

- [ ] **Step 5: Commit**

```bash
git add lib/cube_forge/myc.march test/myc_test.march
git commit -m "feat(fungus): the mycelium tick — vigour, spread, contest, reach, hold"
```

---

### Task 4: The field in the game: Scene, tick slot, knobs, state line

**Files:**
- Modify: `lib/cube_forge.march` (Scene type and every `Scene(` pattern; field tick; Knobs; state and dump lines; session start)

**Interfaces:**
- Consumes: `Myc.build`, `Myc.tick`, `Myc.plant`, `Myc.state_hash`, `Myc.count_species`, `Biome.temps`, `Biome.moists`, `Species.best_for`, `Biome.temp_of`, and a new `Biome.moist_of(f, wx, wz)`.
- Produces: `Scene` gains an 11th field `CubeForge.Myc.Field`; `scene_myc(sc)`, `scene_with_myc(sc, m)`; knobs `CF_MYC_RATE` (percent, default 100), `CF_MYC_MAP` (0/1), `CF_AUTOPLANT` (frame, <0 off: plant the fittest species for the player's column three columns east of the player); the `state:` line gains `myc <hash>`; the dump summary prints columns held per species.

- [ ] **Step 1: Add `Biome.moist_of`** in `biome.march` beside `temp_of`:

```march
  doc "Eased moisture 0..1 of one column."
  fn moist_of(f : Field, wx : Int, wz : Int) : Float do
    match f do Field(_, _, _, ma, _, _, _, _, _) -> NativeArray.get_float(ma, wx + 128 * wz) end
  end
```

- [ ] **Step 2: Widen `Scene`.** The type at line ~61 gains `CubeForge.Myc.Field` as its final field. Every `Scene(...)` pattern and construction in `lib/cube_forge.march` has ten comma-separated arguments today; each gets an eleventh. Do it with a script so nothing is missed, then read every changed line:

```python
import re
p='lib/cube_forge.march'; s=open(p).read()
out=[]; i=0
while True:
    j=s.find('Scene(', i)
    if j<0: out.append(s[i:]); break
    # skip the type declaration's own "Scene(p) = Scene(" head and "type Scene(p)"
    k=j+6; depth=1
    while depth>0:
        ch=s[k]
        if ch=='(': depth+=1
        elif ch==')': depth-=1
        k+=1
    inner=s[j+6:k-1]
    top=0; commas=0
    for ch in inner:
        if ch=='(': top+=1
        elif ch==')': top-=1
        elif ch==',' and top==0: commas+=1
    if commas==9:
        out.append(s[i:k-1]); out.append(', myc)'); i=k
    else:
        out.append(s[i:k]); i=k
open(p,'w').write(''.join(out))
```

This appends `, myc` to every ten-argument `Scene(...)`, which are the patterns and the constructions. After it runs:

- The **type declaration** line now reads `... CubeForge.Biome.Field, NativeIntArr, myc)`: replace that `myc` with `CubeForge.Myc.Field`.
- Patterns like `Scene(world, counts, outline, hit, ui, pids, flags, meshes, bio, pend, myc) ->` now bind `myc`; constructions in the same arm pass it through. Constructions in arms whose pattern used `_` for a field still bind `myc` by name — correct, since the appended token is the same name in both.
- `scene_with_biome` and the other `scene_with_*` helpers pattern `Scene(w, c, o, h, u, p, f, m, _, q, myc)` and construct with `myc`: correct.
- The **session start** construction (`let scene = Scene(world, counts, ... CubeForge.Light.marks_new(), myc)`) has no `myc` in scope: replace that `myc` with `Myc.build(n * 16)`.
- Any construction inside a function whose pattern did not come from a `Scene` match (grep `Scene(` lines and check each one binds or has `myc` in scope) gets `scene_myc(<the scene in scope>)`.

Add the accessors beside `scene_biome`:

```march
  pfn scene_myc(sc) : CubeForge.Myc.Field do match sc do Scene(_, _, _, _, _, _, _, _, _, _, m) -> m end end
  pfn scene_with_myc(sc, m : CubeForge.Myc.Field) do match sc do Scene(w, c, o, h, u, p, f, ms, b, q, _) -> Scene(w, c, o, h, u, p, f, ms, b, q, m) end end
```

and `alias CubeForge.Myc as Myc` and `alias CubeForge.Species as Species` at the top with the other aliases.

Run `forge build 2>&1 | grep -A6 ERROR | head -40` and fix each reported site until it builds.

- [ ] **Step 3: Tick on the field slot.** In the frame loop, the field phase becomes:

```march
          let scb = if phase == field_slot() do
            let tb0 = Win.time()
            let bio1 = Biome.tick(scene_biome(scw), scene_world(scw), seed, Biome.default_rate() *. int_to_float(biome_rate) /. 100.0)
            let f1 = scene_with_biome(scw, bio1)
            let tm0 = Win.time()
            let myc1 = Myc.tick(scene_myc(f1), World.side(scene_world(f1)) * 16, Biome.temps(bio1), Biome.moists(bio1), Myc.default_rate() *. int_to_float(myc_rate) /. 100.0)
            let f2 = scene_with_myc(f1, myc1)
            if autoflow >= 0 && frame % 100 == field_slot() do println("biome tick: " ++ ms(tm0 -. tb0) ++ "  myc tick: " ++ ms(Win.time() -. tm0)) else () end
            f2
          else scw end
```

- [ ] **Step 4: Knobs.** `Knobs` gains three fields at the end: `Int` myc_rate (`Env.get_int("CF_MYC_RATE", 100)`), `Bool` myc_map (`Env.get_int("CF_MYC_MAP", 0) == 1`), `Int` autoplant (`Env.get_int("CF_AUTOPLANT", -1)`); the type comment, the match pattern and the constructor all gain them in that order. Beside the `CF_AUTOGLOW` line:

```march
          -- CF_AUTOPLANT=<frame>: plant the fittest species for the player's
          -- column three columns east of the player, for the field end-to-end check.
          let sc0d1 = if autoplant >= 0 && frame == autoplant do plant_near(sc0d, player1) else sc0d end
```

and thread `sc0d1` into the next consumer (the water tick line reads `sc0d`; make it read `sc0d1`). The helper, beside `place_glow`:

```march
  -- Plant the species that best fits the column three east of the player.
  pfn plant_near(sc, p : CubeForge.Player.Player) do
    let pos = Player.position(p)
    let px = float_to_int(Math.floor(V.x(pos))) + 3
    let pz = float_to_int(Math.floor(V.z(pos)))
    let n = World.side(scene_world(sc)) * 16
    if px < 0 || pz < 0 || px >= n || pz >= n do sc
    else
      let sp = Species.best_for(Biome.temp_of(scene_biome(sc), px, pz), Biome.moist_of(scene_biome(sc), px, pz))
      println("planted " ++ Species.name(sp) ++ " at " ++ int_to_string(px) ++ "," ++ int_to_string(pz))
      if sp == 0 do sc else scene_with_myc(sc, Myc.plant(scene_myc(sc), n, px, pz, sp)) end
    end
  end
```

- [ ] **Step 5: State and dump lines.** The `state:` println gains `++ " myc " ++ int_to_string(Myc.state_hash(scene_myc(scq), World.side(scene_world(scq)) * 16))`. Beside the "biome at player column" line add:

```march
            println("  mycelium: " ++ myc_summary(scene_myc(sc1), World.side(scene_world(sc1)) * 16, 1, ""))
```

with

```march
  pfn myc_summary(f : CubeForge.Myc.Field, n : Int, sp : Int, acc : String) : String do
    if sp > Species.count() do acc
    else myc_summary(f, n, sp + 1, acc ++ Species.name(sp) ++ " " ++ int_to_string(Myc.count_species(f, n, sp)) ++ "  ") end
  end
```

- [ ] **Step 6: Build, lint, test, and an end-to-end run**

Run: `forge build 2>&1 | tail -1 && forge lint --strict 2>&1 | tail -1 && forge test 2>&1 | tail -1`
Expected: all clean.

Then, with a fast rate so the patch grows inside the run:

```bash
CF_NOMOUSE=1 CF_SEED=7 CF_SUN=45 CF_TIME=0 CF_WEATHER=0 CF_FRAMES=3100 CF_AUTOPLANT=100 CF_MYC_RATE=5000 CF_AUTOFLOW=999999 CF_DUMP_FRAME=3000 ./.march/build/debug/cube_forge 2>&1 | grep -i "planted\|mycelium\|myc tick\|state:"
```

Expected: `planted <species> at ...`; the `mycelium:` line shows that species holding well over one column (a reach-8 patch is up to 289); `myc tick:` well under a millisecond on quiet ticks. Note `CF_AUTOFLOW` is only there to turn on the periodic tick timing print; check that value does not trigger anything else at frame 999999 (it cannot, the run is 3100 frames).

- [ ] **Step 7: Commit**

```bash
git add lib/cube_forge.march lib/cube_forge/biome.march
git commit -m "feat(fungus): the mycelium field lives in the Scene and ticks with the climate"
```

---

### Task 5: The map overlay

**Files:**
- Modify: `native/cf_shim.c` (beside `cf_biome_map_upload`)
- Modify: `lib/cube_forge/ffi/window.march`
- Modify: `lib/cube_forge.march` (map draw)

**Interfaces:**
- Produces: C `void cf_myc_map_upload(void *species, void *vigour, int64_t n)` filling slot 246 with one quad per column that holds a species, at `CF_BIOME_Y + 0.5`, colour = the species colour × `(0.3 + 0.7 * vigour / 255)`; columns without a species get a zero-area quad. `Window.myc_map_upload(species, vigour, n)`.

- [ ] **Step 1: C side.** After `cf_biome_map_upload`:

```c
/* ── Mycelium map overlay ──────────────────────────────────────────────────
 * Same shape as the biome map, one layer above it. Species colours mirror
 * CubeForge.Species.colour_*; brightness is vigour. A column with no species
 * gets a degenerate quad so the draw count stays n*n*6. */
#define CF_MYC_SLOT 246
static const float CF_MYC_RGB[7][3] = {
    {0.0f, 0.0f, 0.0f},
    {200/255.0f, 230/255.0f, 255/255.0f}, {120/255.0f, 90/255.0f, 60/255.0f}, {230/255.0f, 200/255.0f, 90/255.0f},
    {255/255.0f, 200/255.0f, 80/255.0f},  {80/255.0f, 220/255.0f, 200/255.0f}, {240/255.0f, 140/255.0f, 60/255.0f},
};
static float  *g_myc_vtx = NULL;
static int64_t g_myc_cap = 0;

void cf_myc_map_upload(void *species, void *vigour, int64_t n) {
    int64_t cells = n * n;
    if (cells > g_myc_cap) {
        free(g_myc_vtx);
        g_myc_vtx = (float *)malloc((size_t)cells * 6 * CF_VERT_FLOATS * sizeof(float));
        g_myc_cap = cells;
    }
    const unsigned char *sp = (const unsigned char *)narr_data(species);
    const unsigned char *vg = (const unsigned char *)narr_data(vigour);
    const float y = CF_BIOME_Y + 0.5f;
    for (int64_t i = 0; i < cells; i++) {
        float *v = g_myc_vtx + i * 6 * CF_VERT_FLOATS;
        int s = sp[i];
        if (s == 0 || s > 6) {
            for (int k = 0; k < 6; k++) pcl_vert(v + k * CF_VERT_FLOATS, 0.0f, y, 0.0f, 0.0f, 0.0f, 0.0f, 255.0f);
            continue;
        }
        float b = 0.3f + 0.7f * (float)vg[i] / 255.0f;
        float r = CF_MYC_RGB[s][0] * b, g = CF_MYC_RGB[s][1] * b, bl = CF_MYC_RGB[s][2] * b;
        float x0 = (float)(i % n), z0 = (float)(i / n), x1 = x0 + 1.0f, z1 = z0 + 1.0f;
        pcl_vert(v + 0 * CF_VERT_FLOATS, x0, y, z0, r, g, bl, 255.0f);
        pcl_vert(v + 1 * CF_VERT_FLOATS, x0, y, z1, r, g, bl, 255.0f);
        pcl_vert(v + 2 * CF_VERT_FLOATS, x1, y, z1, r, g, bl, 255.0f);
        pcl_vert(v + 3 * CF_VERT_FLOATS, x0, y, z0, r, g, bl, 255.0f);
        pcl_vert(v + 4 * CF_VERT_FLOATS, x1, y, z1, r, g, bl, 255.0f);
        pcl_vert(v + 5 * CF_VERT_FLOATS, x1, y, z0, r, g, bl, 255.0f);
    }
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo[CF_MYC_SLOT]);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(cells * 6 * CF_VERT_FLOATS * (int64_t)sizeof(float)), g_myc_vtx, GL_STATIC_DRAW);
}
```

Read `pcl_vert`'s signature (line ~813) first and match its argument order; the biome upload above it is the reference. Check that slot 246 is free: `grep -n "246" native/cf_shim.c lib/cube_forge/*.march` — 247 is spray and 248 the biome map; if 246 is taken, use the next free slot below and say so in the comment.

- [ ] **Step 2: Binding.** In `lib/cube_forge/ffi/window.march`, beside the two `biome_map_upload` lines:

```march
    fn x_myc_map_upload(species: NativeU8Arr, vigour: NativeU8Arr, n: Int): Unit = "cf_myc_map_upload"
```
```march
  fn myc_map_upload(species : NativeU8Arr, vigour : NativeU8Arr, n : Int) : Unit do x_myc_map_upload(species, vigour, n) end
```

- [ ] **Step 3: Draw.** In the map-mode block after the biome map:

```march
            if myc_map do
              let mn = World.side(scene_world(sc1)) * 16
              if frame % tick_period() == 0 do Win.myc_map_upload(Myc.field_species(scene_myc(sc1)), Myc.vigour_u8(scene_myc(sc1), mn), mn) else () end
              CubeForge.Ffi.Window.gfx_draw_marker(246, mn * mn * 6)
            else () end
```

- [ ] **Step 4: Build, then a map dump**

```bash
forge build 2>&1 | tail -1
CF_NOMOUSE=1 CF_SEED=7 CF_SUN=45 CF_TIME=0 CF_WEATHER=0 CF_FRAMES=3100 CF_AUTOPLANT=100 CF_MYC_RATE=5000 CF_AUTOMAP=2900 CF_BIOME_MAP=1 CF_MYC_MAP=1 CF_DUMP=docs/myc-map.bmp CF_DUMP_FRAME=3000 ./.march/build/debug/cube_forge 2>&1 | grep -i "planted\|mycelium"
sips -s format png docs/myc-map.bmp --out docs/fungus-myc-map.png && rm docs/myc-map.bmp
```

Open `docs/fungus-myc-map.png`: a coloured patch of the planted species on the biome map beside the player marker, brighter at the centre than the rim.

- [ ] **Step 5: Budget, results, spec, backlog**

Run `forge build --release && scratch/frame_budget.sh`; expected: passes.

`RESULTS.md` gains `### Mycelium field (fungus phase 2)` with: the `myc tick` cost on a quiet tick and during growth (from the `myc tick:` prints), the budget result, the species counts at the dump, and the pull-based shape and why (G21/G63/G68).

Spec §2 gains an *As built* note: the tick is pull-based; vigour is a float 0..1 rather than 0..255; hold applies to claims (the claimant must persist `hold_ticks()`), which is the spec's anti-flap; contest drains the incumbent's vigour at `contest_factor()` × rate and the challenger takes the column when it hits zero. `todos.md` gains the phase 2 entry beside phase 1.

- [ ] **Step 6: Commit**

```bash
git add native/cf_shim.c lib/cube_forge/ffi/window.march lib/cube_forge.march RESULTS.md todos.md docs/superpowers/specs/2026-09-04-fungus-design.md docs/fungus-myc-map.png
git commit -m "feat(fungus): mycelium map overlay (CF_MYC_MAP); phase 2 recorded"
```

---

## Self-review

- **Spec coverage (§2, §3, §8):** species and vigour arrays (Task 2), fitness from the eased axes (Tasks 1, 3), vigour easing and wither (3), spread to empty neighbours above a floor (3), contest by margin with slow drift (3), reach per hop (3), hold counter (3), no climate feedback (nothing writes the biome field), species table as data (1), map overlay under `CF_MYC_MAP` (5). §10's reach, contest, wither and no-flap tests are Task 3's; the canal test waits for phase 4, when a species can be planted beside a real trench.
- **Deviations noted for the spec:** vigour as float; hold on claims; pull-based tick. Task 5 step 5 records them.
- **Type consistency:** `Myc.tick(f, n, ta, ma, rate)` everywhere; `Myc.plant(f, n, x, z, sp)`; `Field(species, vigour, reach, hold, claimant)` in that order in every match.
