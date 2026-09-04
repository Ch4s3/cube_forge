# Biomes Phase 1 — The Field — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A per-column climate field — temperature, moisture, elevation → one of eight biomes — derived from live world state, eased over time, recomputed whole each tick, and visible on the map view. Mutates nothing.

**Architecture:** A new `CubeForge.Biome` module owns five `16384`-entry arrays (surface height, water flag, eased temperature, eased moisture, biome id) and three pure functions: classify, build, tick. The `Scene` carries the field; `edit_block` gives it an O(1) heightmap/water update; the frame loop ticks it on `tick_period()` and, in map mode with `CF_BIOME_MAP=1`, draws a C-built quad grid coloured by biome id.

**Tech Stack:** March (forge), OpenGL 3.3 via `native/cf_shim.c`.

## Global Constraints

- World is `128 x 128` columns; column index is `wx + 128 * wz`. `CubeForge.World.side(w) * 16` gives the width — do not hard-code 128 in the module, take it from the world.
- Biome ids: `0 tundra, 1 taiga, 2 grassland, 3 forest, 4 desert, 5 wetland, 6 beach, 7 alpine`.
- `MOISTURE_REACH = 24` columns; moisture = `clamp01(1 - dist / 24)` over Chebyshev distance.
- Temperature: `temp_base` is one `Noise.value2` octave at scale `0.018` (the `ruggedness` scale), minus `LAPSE = 0.012` per block above `sea_level` (62).
- Alpine gate: surface height `>= 95` (the existing `snow_line`).
- Beach: water-adjacent (Chebyshev distance `<= 1`) and surface height `<= sea_level + 2`.
- Easing: full range in **10,800 ticks** (one `CF_DAY` at `tick_period()` 10 and 60 fps) at `CF_BIOME_RATE=100`; the knob scales it linearly.
- Anti-flap: a column's biome flips only after the raw classification has disagreed with it for `HOLD_TICKS = 30` consecutive ticks. (The spec asks for a value dead band; a hold counter has the same effect on every axis at once and needs no per-threshold tuning. Note this in the spec when it lands.)
- **Every tick writes into freshly allocated arrays.** GAPS G68: a `NativeArray` write on an array the `Scene` still references copies the whole array. Fresh arrays are uniquely owned and write in place. Never `set_u8` into an array that was read from the field.
- Frame dumps need `MARCH_NUM_SCHEDULERS=1 CF_NOMOUSE=1`; compare with `scratch/cmpframe.py`.
- Build cycle every task: `forge check`, `forge build`, `forge lint --strict`, `forge test`.

---

## File Structure

| File | Responsibility | Task |
|------|----------------|------|
| `lib/cube_forge/biome.march` | **New.** Field type, classification, build, edit hook, tick. No GL, no frame loop. | 1-3 |
| `test/biome_test.march` | **New.** Classification, heightmap agreement, distance, canal, levelling, no-flap. | 1-3 |
| `lib/cube_forge.march` | Field in `Scene`; `edit_block` calls `note_edit`; tick on the tick boundary; knobs; map draw. | 4-5 |
| `native/cf_shim.c` | `cf_biome_map_upload` — builds the coloured quad grid in C (G68 again: 98k vertices cannot be pushed from March per tick). | 5 |
| `lib/cube_forge/ffi/window.march` | Binding for the above. | 5 |
| `RESULTS.md`, `todos.md` | Tick cost and the dead-band substitution. | 5 |

---

## Task 1: Classification and axis targets

Pure functions only. Everything later leans on these being right, and they are the cheapest thing in the plan to test exhaustively.

**Files:**
- Create: `lib/cube_forge/biome.march`
- Create: `test/biome_test.march`

**Interfaces:**
- Produces: `Biome.classify(temp : Float, moist : Float, h : Int, dist_water : Int) : Int`; `Biome.temp_target(wx : Int, wz : Int, seed : Int, h : Int) : Float`; `Biome.moisture_target(dist : Int) : Float`; the eight `b_*()` id constants; `Biome.reach() : Int` (= 24); `Biome.name(id : Int) : String`.

- [ ] **Step 1: Write the failing test**

```march
-- Biome classification: the two axes, the elevation gate, and beach.
mod CubeForge.Test.Biome do

  import Test
  alias CubeForge.Biome as Biome

  -- Inland (far from water) at sea level, so only the axes decide.
  pfn inland(t : Float, m : Float) : Int do Biome.classify(t, m, 62, 99) end

  describe "Biome.classify" do
    test "walks the Whittaker table" do
      Test.assert_eq_int(inland(0.1, 0.2), Biome.b_tundra(),    "cold dry")
      Test.assert_eq_int(inland(0.1, 0.8), Biome.b_taiga(),     "cold damp")
      Test.assert_eq_int(inland(0.5, 0.2), Biome.b_grassland(), "temperate dry")
      Test.assert_eq_int(inland(0.5, 0.8), Biome.b_forest(),    "temperate damp")
      Test.assert_eq_int(inland(0.9, 0.2), Biome.b_desert(),    "hot dry")
      Test.assert_eq_int(inland(0.9, 0.8), Biome.b_wetland(),   "hot damp")
    end

    test "elevation gate beats the axes" do
      Test.assert_eq_int(Biome.classify(0.9, 0.8, 95, 99), Biome.b_alpine(), "at the snow line")
      Test.assert_eq_int(Biome.classify(0.9, 0.8, 94, 99), Biome.b_wetland(), "one below it")
    end

    test "beach needs water adjacency AND low ground" do
      Test.assert_eq_int(Biome.classify(0.5, 0.9, 63, 1), Biome.b_beach(), "next to water, at sea level")
      Test.assert_eq_int(Biome.classify(0.5, 0.9, 70, 1), Biome.b_forest(), "next to water but high: not beach")
      Test.assert_eq_int(Biome.classify(0.5, 0.9, 63, 2), Biome.b_forest(), "low but two columns from water: not beach")
    end
  end

  describe "Biome.moisture_target" do
    test "falls linearly to zero at the reach" do
      Test.assert_true(Math.abs(Biome.moisture_target(0) -. 1.0) < 0.0001, "on the water")
      Test.assert_true(Math.abs(Biome.moisture_target(12) -. 0.5) < 0.0001, "halfway")
      Test.assert_true(Biome.moisture_target(24) < 0.0001, "at the reach")
      Test.assert_true(Biome.moisture_target(99) < 0.0001, "beyond it")
    end
  end

  describe "Biome.temp_target" do
    test "is colder higher up, and seeded" do
      let low = Biome.temp_target(40, 40, 7, 62)
      let high = Biome.temp_target(40, 40, 7, 100)
      Test.assert_true(high < low, "lapse rate")
      Test.assert_true(Math.abs(Biome.temp_target(40, 40, 7, 62) -. Biome.temp_target(40, 40, 8, 62)) > 0.0001, "seed matters")
      Test.assert_true(low >= 0.0 && low <= 1.0, "clamped")
    end
  end

end
```

- [ ] **Step 2: Run it and watch it fail**

```bash
forge test --filter=Biome
```
Expected: FAIL — `Unknown module`.

- [ ] **Step 3: Write the module (classification half)**

```march
-- Biome — a derived, mutable climate field, one entry per world column.
--
-- Temperature and moisture (the Whittaker pair) plus an elevation gate. The
-- field is recomputed WHOLE every tick: 16,384 columns is small enough that the
-- light field's incremental machinery would be waste, and it means a player
-- edit needs no special handling anywhere.
mod CubeForge.Biome do

  alias CubeForge.Noise as N

  -- height, water flag, eased temperature, eased moisture, biome id, hold counter
  type Field = Field(NativeU8Arr, NativeU8Arr, NativeFloatArr, NativeFloatArr, NativeU8Arr, NativeU8Arr)

  doc "Biome ids. The order is the Whittaker table read row by row, then the two gated ones."
  fn b_tundra() : Int do 0 end
  fn b_taiga() : Int do 1 end
  fn b_grassland() : Int do 2 end
  fn b_forest() : Int do 3 end
  fn b_desert() : Int do 4 end
  fn b_wetland() : Int do 5 end
  fn b_beach() : Int do 6 end
  fn b_alpine() : Int do 7 end

  fn name(id : Int) : String do
    if id == 0 do "tundra" else if id == 1 do "taiga" else if id == 2 do "grassland"
    else if id == 3 do "forest" else if id == 4 do "desert" else if id == 5 do "wetland"
    else if id == 6 do "beach" else "alpine" end end end end end end end
  end

  doc "Columns from water at which moisture reaches zero."
  fn reach() : Int do 24 end
  doc "Temperature lost per block above sea level."
  fn lapse() : Float do 0.012 end
  doc "Surface height at and above which a column is alpine, whatever the axes say."
  fn alpine_line() : Int do 95 end
  doc "Columns beside water this low are beach."
  fn beach_max_height() : Int do 64 end

  pfn clamp01(v : Float) : Float do if v < 0.0 do 0.0 else if v > 1.0 do 1.0 else v end end end

  doc "Moisture 0..1 for a column [dist] away from the nearest water: linear, zero at the reach."
  fn moisture_target(dist : Int) : Float do
    clamp01(1.0 -. int_to_float(dist) /. int_to_float(reach()))
  end

  doc "Temperature 0..1: one low-frequency octave (regions, not noise -- see Noise.ruggedness for why one octave), minus the lapse for height above sea level."
  fn temp_target(wx : Int, wz : Int, seed : Int, h : Int) : Float do
    let base = N.value2(int_to_float(wx) *. 0.018, int_to_float(wz) *. 0.018, seed + 1700)
    clamp01(base -. lapse() *. int_to_float(h - N.sea_level()))
  end

  doc "The Whittaker lookup with two gates in front of it. Thresholds: cold below 0.33, hot from 0.66; damp from 0.5."
  fn classify(temp : Float, moist : Float, h : Int, dist_water : Int) : Int do
    if h >= alpine_line() do b_alpine()
    else if dist_water <= 1 && h <= beach_max_height() do b_beach()
    else
      let damp = moist >= 0.5
      if temp < 0.33 do (if damp do b_taiga() else b_tundra() end)
      else if temp < 0.66 do (if damp do b_forest() else b_grassland() end)
      else (if damp do b_wetland() else b_desert() end) end end
    end end
  end

end
```

- [ ] **Step 4: Run the test and watch it pass**

```bash
forge test --filter=Biome
```
Expected: PASS, 5 tests.

- [ ] **Step 5: Commit**

```bash
git add lib/cube_forge/biome.march test/biome_test.march
git commit -m "biome: classification and axis targets

The Whittaker pair with an elevation gate and a beach gate in front of it.
Pure functions, tested across every cell of the table and both sides of each
gate before any of the field machinery exists."
```

---

## Task 2: Building the field from a world

The surface heightmap, the water map, the multi-source distance transform, and the initial classification. A fresh world is fully classified immediately — easing only applies to *changes*.

**Files:**
- Modify: `lib/cube_forge/biome.march`
- Modify: `test/biome_test.march`

**Interfaces:**
- Consumes: `CubeForge.World.chunk_at`, `Chunk.get`, `Chunk.is_water`, `Light.sky_floor`.
- Produces: `Biome.build(w : World, seed : Int) : Field`; `Biome.height_of(f, wx, wz) : Int`; `Biome.biome_of(f, wx, wz) : Int`; `Biome.distances(f, w) : NativeIntArr` (Chebyshev distance to water per column, capped at `reach()`); `Biome.field_biomes(f) : NativeU8Arr`.

- [ ] **Step 1: Write the failing tests**

Append inside `mod CubeForge.Test.Biome`:

```march
  alias CubeForge.Chunk as C
  alias CubeForge.World as W

  -- A flat world: every column solid stone up to [h].
  pfn flat_cells(c : CubeForge.Chunk.Chunk, i : Int, h : Int) : CubeForge.Chunk.Chunk do
    if i >= 256 * h do c
    else flat_cells(C.set(c, i % 16, i / 256, (i / 16) % 16, 3), i + 1, h) end
  end
  pfn flat_chunks(i : Int, h : Int, acc : List(CubeForge.Chunk.Chunk)) : List(CubeForge.Chunk.Chunk) do
    if i < 0 do acc else flat_chunks(i - 1, h, Cons(flat_cells(C.new(), 0, h), acc)) end
  end
  pfn flat_world(h : Int) : CubeForge.World.World do
    W.World(Array.from_list(flat_chunks(63, h, Nil)), CubeForge.Light.new(), CubeForge.Light.new(), 8)
  end

  describe "Biome.build" do
    test "heightmap reads the surface" do
      let f = Biome.build(flat_world(70), 7)
      Test.assert_eq_int(Biome.height_of(f, 5, 5), 69, "top solid block of a 70-high column")
      let w2 = W.set_block(flat_world(70), 5, 70, 5, 3)
      Test.assert_eq_int(Biome.height_of(Biome.build(w2, 7), 5, 5), 70, "a block placed on top raises it")
    end

    test "distance to water is Chebyshev and capped at the reach" do
      let w = W.set_block(flat_world(70), 64, 70, 64, C.water())
      let d = Biome.distances(Biome.build(w, 7), w)
      Test.assert_eq_int(NativeArray.get_int(d, 64 + 128 * 64), 0, "on the water")
      Test.assert_eq_int(NativeArray.get_int(d, 67 + 128 * 66), 3, "Chebyshev: max(3, 2)")
      Test.assert_eq_int(NativeArray.get_int(d, 0), Biome.reach(), "far away is capped")
    end

    test "a dry temperate flat world is grassland; beside water it is beach" do
      let w = W.set_block(flat_world(63), 64, 63, 64, C.water())
      let f = Biome.build(w, 7)
      Test.assert_eq_int(Biome.biome_of(f, 65, 64), Biome.b_beach(), "next to the water")
      let far = Biome.biome_of(f, 5, 5)
      Test.assert_true(far == Biome.b_grassland() || far == Biome.b_tundra() || far == Biome.b_desert(), "far away is one of the dry biomes")
    end
  end
```

- [ ] **Step 2: Run and watch it fail**

```bash
forge test --filter=Biome
```
Expected: FAIL — `build` not defined.

- [ ] **Step 3: Implement build**

Append inside `mod CubeForge.Biome` (before the final `end`):

```march
  alias CubeForge.World as W
  alias CubeForge.Chunk as C

  fn cols(w : CubeForge.World.World) : Int do W.side(w) * 16 end
  fn index(f_cols : Int, wx : Int, wz : Int) : Int do wx + f_cols * wz end

  -- Highest non-air block in a column, scanning down from [top]. 0 if none.
  pfn surface_go(w : CubeForge.World.World, wx : Int, wz : Int, y : Int) : Int do
    if y <= 0 do 0
    else if W.block_at(w, wx, y, wz) != 0 do y
    else surface_go(w, wx, wz, y - 1) end end
  end

  -- Water at the surface, or one above it.
  pfn wet_at(w : CubeForge.World.World, wx : Int, wz : Int, h : Int) : Bool do
    C.is_water(W.block_at(w, wx, h, wz)) || C.is_water(W.block_at(w, wx, h + 1, wz))
  end

  pfn scan_go(w : CubeForge.World.World, n : Int, top : Int, i : Int,
              hs : NativeU8Arr, ws : NativeU8Arr) : Field do
    if i >= n * n do Field(hs, ws, NativeArray.make_float(n * n, 0.0), NativeArray.make_float(n * n, 0.0), NativeArray.make_u8(n * n, 0), NativeArray.make_u8(n * n, 0))
    else
      let h = surface_go(w, i % n, i / n, top)
      scan_go(w, n, top, i + 1, NativeArray.set_u8(hs, i, h), NativeArray.set_u8(ws, i, if wet_at(w, i % n, i / n, h) do 1 else 0 end))
    end
  end

  -- ── Multi-source BFS: Chebyshev distance to the nearest water column ──────
  -- Queue is a ring in a fresh NativeIntArr; distance array starts at reach()
  -- everywhere so "unreached" and "at the cap" are the same value.
  pfn seed_go(ws : NativeU8Arr, n : Int, i : Int, d : NativeIntArr, q : NativeIntArr, qn : Int) : Int do
    if i >= n * n do qn
    else if NativeArray.get_u8(ws, i) == 1 do
      seed_go(ws, n, i + 1, NativeArray.set_int(d, i, 0), NativeArray.set_int(q, qn, i), qn + 1)
    else seed_go(ws, n, i + 1, d, q, qn) end end
  end

  -- Relax the eight neighbours of column [i]; push any whose distance improved.
  pfn relax(d : NativeIntArr, q : NativeIntArr, qn : Int, n : Int, x : Int, z : Int, nd : Int, k : Int) : Int do
    if k >= 8 do qn
    else
      let dx = if k < 3 do k - 1 else if k < 5 do (if k == 3 do 0 - 1 else 1 end) else k - 6 end end
      let dz = if k < 3 do 0 - 1 else if k < 5 do 0 else 1 end end
      let nx = x + dx
      let nz = z + dz
      if nx < 0 || nz < 0 || nx >= n || nz >= n do relax(d, q, qn, n, x, z, nd, k + 1)
      else
        let j = nx + n * nz
        if nd < NativeArray.get_int(d, j) do
          let _ = NativeArray.set_int(d, j, nd)
          let _ = NativeArray.set_int(q, qn, j)
          relax(d, q, qn + 1, n, x, z, nd, k + 1)
        else relax(d, q, qn, n, x, z, nd, k + 1) end
      end
    end
  end

  pfn bfs_go(d : NativeIntArr, q : NativeIntArr, qh : Int, qn : Int, n : Int) : NativeIntArr do
    if qh >= qn do d
    else
      let i = NativeArray.get_int(q, qh)
      let nd = NativeArray.get_int(d, i) + 1
      if nd >= reach() do bfs_go(d, q, qh + 1, qn, n)
      else bfs_go(d, q, qh + 1, relax(d, q, qn, n, i % n, i / n, nd, 0), n) end
    end
  end

  doc "Chebyshev distance to the nearest water column, capped at reach(). Fresh arrays every call (GAPS G68)."
  fn distances(f : Field, w : CubeForge.World.World) : NativeIntArr do
    let n = cols(w)
    match f do
      Field(_, ws, _, _, _, _) ->
        let d = NativeArray.make_int(n * n, reach())
        -- every column can be pushed at most (reach) times; size the ring generously
        let q = NativeArray.make_int(n * n * 4, 0)
        let qn = seed_go(ws, n, 0, d, q, 0)
        bfs_go(d, q, 0, qn, n)
    end
  end

  -- Classify every column from its targets, writing the eased axes AT target
  -- (a fresh world starts fully classified; easing is for changes).
  pfn classify_all(f : Field, w : CubeForge.World.World, seed : Int, d : NativeIntArr, n : Int, i : Int,
                   ta : NativeFloatArr, ma : NativeFloatArr, bs : NativeU8Arr) : Field do
    if i >= n * n do match f do Field(hs, ws, _, _, _, hold) -> Field(hs, ws, ta, ma, bs, hold) end
    else
      let h = height_of(f, i % n, i / n)
      let t = temp_target(i % n, i / n, seed, h)
      let m = moisture_target(NativeArray.get_int(d, i))
      let b = classify(t, m, h, NativeArray.get_int(d, i))
      classify_all(f, w, seed, d, n, i + 1, NativeArray.set_float(ta, i, t), NativeArray.set_float(ma, i, m), NativeArray.set_u8(bs, i, b))
    end
  end

  doc "Build the field from a world: scan every column's surface and water, then classify."
  fn build(w : CubeForge.World.World, seed : Int) : Field do
    let n = cols(w)
    let f0 = scan_go(w, n, CubeForge.Light.sky_floor(w), 0, NativeArray.make_u8(n * n, 0), NativeArray.make_u8(n * n, 0))
    let d = distances(f0, w)
    classify_all(f0, w, seed, d, n, 0, NativeArray.make_float(n * n, 0.0), NativeArray.make_float(n * n, 0.0), NativeArray.make_u8(n * n, 0))
  end

  fn height_of(f : Field, wx : Int, wz : Int) : Int do
    match f do Field(hs, _, _, _, _, _) -> NativeArray.get_u8(hs, wx + 128 * wz) end
  end
  fn biome_of(f : Field, wx : Int, wz : Int) : Int do
    match f do Field(_, _, _, _, bs, _) -> NativeArray.get_u8(bs, wx + 128 * wz) end
  end
  fn field_biomes(f : Field) : NativeU8Arr do match f do Field(_, _, _, _, bs, _) -> bs end end
```

`height_of` / `biome_of` use 128 because the accessors have no world to ask; if the world side ever changes, thread `cols` through `Field`. Check `W.block_at` exists (`world.march`) — it does, per the G64 note there.

- [ ] **Step 4: Run and watch it pass**

```bash
forge test --filter=Biome
```
Expected: PASS, 8 tests. If the Chebyshev test reports 5 rather than 3, `relax` is visiting only 4 neighbours — check the `dx/dz` decode for `k = 3..7`.

- [ ] **Step 5: Commit**

```bash
git add lib/cube_forge/biome.march test/biome_test.march
git commit -m "biome: build the field from a world

Surface heightmap and water map by scanning down from Light.sky_floor, a
multi-source Chebyshev BFS for distance to water capped at the reach, and an
initial classification with the eased axes parked at their targets so a fresh
world starts fully classified. Every array is freshly allocated (GAPS G68)."
```

---

## Task 3: Edits and ticks

The O(1) heightmap hook, and the per-tick recompute with easing and the hold counter. This is where the spec's two player-agency cases become tests.

**Files:**
- Modify: `lib/cube_forge/biome.march`
- Modify: `test/biome_test.march`

**Interfaces:**
- Produces: `Biome.note_edit(f : Field, w : World, x : Int, y : Int, z : Int, id : Int) : Field`; `Biome.tick(f : Field, w : World, seed : Int, rate : Float) : Field` where `rate` is the fraction of full axis range moved per tick; `Biome.default_rate() : Float` (= 1/10800); `Biome.hold_ticks() : Int` (= 30).

- [ ] **Step 1: Write the failing tests**

```march
  pfn tick_n(f : CubeForge.Biome.Field, w : CubeForge.World.World, n : Int, rate : Float) : CubeForge.Biome.Field do
    if n <= 0 do f else tick_n(Biome.tick(f, w, 7, rate), w, n - 1, rate) end
  end

  describe "Biome.note_edit" do
    test "keeps the heightmap equal to a full rescan across edits" do
      let w0 = flat_world(70)
      let f0 = Biome.build(w0, 7)
      let w1 = W.set_block(w0, 10, 70, 10, 3)           -- place on top
      let f1 = Biome.note_edit(f0, w1, 10, 70, 10, 3)
      let w2 = W.set_block(w1, 20, 69, 20, 0)           -- break the surface
      let f2 = Biome.note_edit(f1, w2, 20, 69, 20, 0)
      let w3 = W.set_block(w2, 30, 40, 30, 0)           -- dig well below the surface: no change
      let f3 = Biome.note_edit(f2, w3, 30, 40, 30, 0)
      Test.assert_eq_int(Biome.height_of(f3, 10, 10), Biome.height_of(Biome.build(w3, 7), 10, 10), "raised column agrees")
      Test.assert_eq_int(Biome.height_of(f3, 20, 20), Biome.height_of(Biome.build(w3, 7), 20, 20), "lowered column agrees")
      Test.assert_eq_int(Biome.height_of(f3, 30, 30), Biome.height_of(Biome.build(w3, 7), 30, 30), "untouched surface agrees")
    end
  end

  describe "Biome.tick" do
    test "a canal turns its banks from dry to wet, gradually" do
      -- a warm flat world: hot enough to be desert when dry, wetland when damp
      let w0 = flat_world(66)
      let f0 = Biome.build(w0, 7)
      let before = Biome.biome_of(f0, 64, 60)
      -- dig a trench of water along z at x = 64
      let w1 = W.set_block(W.set_block(W.set_block(w0, 64, 65, 58, C.water()), 64, 65, 60, C.water()), 64, 65, 62, C.water())
      let f1 = Biome.note_edit(Biome.note_edit(Biome.note_edit(f0, w1, 64, 65, 58, C.water()), w1, 64, 65, 60, C.water()), w1, 64, 65, 62, C.water())
      -- one tick: target jumps, actual has barely moved, biome has not flipped
      let f2 = Biome.tick(f1, w1, 7, Biome.default_rate())
      Test.assert_eq_int(Biome.biome_of(f2, 68, 60), before, "no snap after one tick")
      -- many ticks at a fast rate: it turns
      let f3 = tick_n(f2, w1, 200, 0.05)
      Test.assert_true(Biome.biome_of(f3, 68, 60) != before, "the bank changed biome")
    end

    test "levelling a peak leaves alpine" do
      let w0 = flat_world(100)
      let f0 = Biome.build(w0, 7)
      Test.assert_eq_int(Biome.biome_of(f0, 40, 40), Biome.b_alpine(), "starts alpine")
      -- take the column down to 80
      let w1 = W.set_block(w0, 40, 99, 40, 0)
      let f1 = Biome.note_edit(f0, w1, 40, 99, 40, 0)
      let f2 = tick_n(f1, w1, 40, 0.05)
      Test.assert_eq_int(Biome.height_of(f2, 40, 40), 98, "surface dropped")
      -- still alpine: 98 is above the line. Now below it.
      let w2 = W.set_block(W.set_block(W.set_block(W.set_block(w1, 40, 98, 40, 0), 40, 97, 40, 0), 40, 96, 40, 0), 40, 95, 40, 0)
      let f3 = Biome.note_edit(Biome.note_edit(Biome.note_edit(Biome.note_edit(f2, w2, 40, 98, 40, 0), w2, 40, 97, 40, 0), w2, 40, 96, 40, 0), w2, 40, 95, 40, 0)
      let f4 = tick_n(f3, w2, 40, 0.05)
      Test.assert_true(Biome.biome_of(f4, 40, 40) != Biome.b_alpine(), "no longer alpine")
    end

    test "does not flap: a flipped column stays flipped" do
      let w0 = flat_world(66)
      let w1 = W.set_block(w0, 64, 65, 60, C.water())
      let f0 = Biome.note_edit(Biome.build(w0, 7), w1, 64, 65, 60, C.water())
      let f1 = tick_n(f0, w1, 300, 0.05)
      let b1 = Biome.biome_of(f1, 66, 60)
      let f2 = tick_n(f1, w1, 300, 0.05)
      Test.assert_eq_int(Biome.biome_of(f2, 66, 60), b1, "stable once settled")
    end
  end
```

- [ ] **Step 2: Run and watch it fail**

```bash
forge test --filter=Biome
```
Expected: FAIL — `note_edit` / `tick` not defined.

- [ ] **Step 3: Implement note_edit and tick**

```march
  fn default_rate() : Float do 1.0 /. 10800.0 end
  fn hold_ticks() : Int do 30 end

  doc "A block changed at (x, y, z). Only the surface and water flag of that one column can change, and only if the edit is at or above the current surface -- O(1)."
  fn note_edit(f : Field, w : CubeForge.World.World, x : Int, y : Int, z : Int, id : Int) : Field do
    let n = cols(w)
    if x < 0 || z < 0 || x >= n || z >= n do f
    else
      match f do
        Field(hs, ws, ta, ma, bs, hold) ->
          let i = x + n * z
          let cur = NativeArray.get_u8(hs, i)
          let h1 = if y > cur && id != 0 do y                   -- placed above: new surface
                   else if y == cur && id == 0 do surface_go(w, x, z, y - 1)   -- broke the surface: rescan down
                   else cur end end
          -- Fresh copies: these arrays are still held by the Scene (GAPS G68).
          let hs1 = NativeArray.set_u8(copy_u8(hs, n * n), i, h1)
          let ws1 = NativeArray.set_u8(copy_u8(ws, n * n), i, if wet_at(w, x, z, h1) do 1 else 0 end)
          Field(hs1, ws1, ta, ma, bs, hold)
      end
    end
  end

  pfn copy_go(src : NativeU8Arr, dst : NativeU8Arr, i : Int, n : Int) : NativeU8Arr do
    if i >= n do dst else copy_go(src, NativeArray.set_u8(dst, i, NativeArray.get_u8(src, i)), i + 1, n) end
  end
  pfn copy_u8(a : NativeU8Arr, n : Int) : NativeU8Arr do copy_go(a, NativeArray.make_u8(n, 0), 0, n) end

  pfn ease(cur : Float, target : Float, rate : Float) : Float do
    let d = target -. cur
    if d > rate do cur +. rate else if d < 0.0 -. rate do cur -. rate else target end end
  end

  pfn tick_go(f : Field, w : CubeForge.World.World, seed : Int, rate : Float, d : NativeIntArr, n : Int, i : Int,
              ta : NativeFloatArr, ma : NativeFloatArr, bs : NativeU8Arr, hold : NativeU8Arr) : Field do
    if i >= n * n do match f do Field(hs, ws, _, _, _, _) -> Field(hs, ws, ta, ma, bs, hold) end
    else
      match f do
        Field(_, _, ta0, ma0, bs0, hold0) ->
          let h = height_of(f, i % n, i / n)
          let dist = NativeArray.get_int(d, i)
          let t = ease(NativeArray.get_float(ta0, i), temp_target(i % n, i / n, seed, h), rate)
          let m = ease(NativeArray.get_float(ma0, i), moisture_target(dist), rate)
          let raw = classify(t, m, h, dist)
          let cur = NativeArray.get_u8(bs0, i)
          -- The hold counter: flip only after HOLD_TICKS consecutive disagreements,
          -- so a column parked on a threshold cannot alternate every tick.
          let hc = if raw == cur do 0 else NativeArray.get_u8(hold0, i) + 1 end
          let b1 = if hc >= hold_ticks() do raw else cur end
          let hc1 = if b1 == raw do 0 else hc end
          tick_go(f, w, seed, rate, d, n, i + 1,
                  NativeArray.set_float(ta, i, t), NativeArray.set_float(ma, i, m),
                  NativeArray.set_u8(bs, i, b1), NativeArray.set_u8(hold, i, hc1))
      end
    end
  end

  doc "One tick: recompute distances, ease both axes toward their targets by [rate], reclassify with the hold counter. Writes fresh arrays."
  fn tick(f : Field, w : CubeForge.World.World, seed : Int, rate : Float) : Field do
    let n = cols(w)
    let d = distances(f, w)
    tick_go(f, w, seed, rate, d, n, 0,
            NativeArray.make_float(n * n, 0.0), NativeArray.make_float(n * n, 0.0),
            NativeArray.make_u8(n * n, 0), NativeArray.make_u8(n * n, 0))
  end
```

- [ ] **Step 4: Run and watch it pass**

```bash
forge test --filter=Biome
```
Expected: PASS, 12 tests. If the canal test's "no snap" assertion fails, easing is being skipped on the first tick — check that `tick` reads `ta0`, not the target.

- [ ] **Step 5: Commit**

```bash
git add lib/cube_forge/biome.march test/biome_test.march
git commit -m "biome: edits and ticks

note_edit is O(1): only the edited column's surface can change, and only if the
edit is at or above it. tick recomputes distances, eases both axes, and
reclassifies behind a hold counter -- a column flips only after 30 consecutive
ticks of disagreement, which is what stops a threshold from flapping. The
spec's two player-agency cases are the tests: a canal wets its banks, and
levelling a peak leaves alpine."
```

---

## Task 4: Wire the field into the scene, the edit path, the tick, and the knobs

**Files:**
- Modify: `lib/cube_forge.march`

**Interfaces:**
- Consumes: everything from Tasks 1-3.
- Produces: `Scene` gains a ninth field; `scene_biome(sc) : Biome.Field`; `CF_BIOME_RATE` (Int, percent, default 100) in `Knobs`.

- [ ] **Step 1: Add the field to Scene**

`Scene` (line ~50) gains a trailing `CubeForge.Biome.Field`. Update every `Scene(...)` pattern and constructor — there are about six (`scene_ui`, `scene_with_ui`, `scene_counts`, `scene_world`, `scene_has_hit`, `edit_block`, `water_tick`, the constructor near line 1170, and `scene_water_count`). Add `alias CubeForge.Biome as Biome` and:

```march
  pfn scene_biome(sc) : CubeForge.Biome.Field do match sc do Scene(_, _, _, _, _, _, _, _, b) -> b end end
  pfn scene_with_biome(sc, b : CubeForge.Biome.Field) do match sc do Scene(w, c, o, h, u, p, f, m, _) -> Scene(w, c, o, h, u, p, f, m, b) end end
```

Construct it at startup, after the world is generated and relit (near the `Scene(world, counts, ...)` construction):

```march
        let biome0 = Biome.build(world, seed)
```

and pass `biome0` as the last Scene argument.

- [ ] **Step 2: Hook the edit path**

In `edit_block`, immediately after `let w0 = World.set_occupied(World.set_block(world, x, y, z, id), ...)`:

```march
        let biome1 = Biome.note_edit(biome, w0, x, y, z, id)
```

(bind `biome` from the widened `Scene` pattern) and carry `biome1` into the returned `Scene`.

- [ ] **Step 3: Tick on the tick boundary**

After the existing `let sc1 = if frame % tick_period() == 0 do water_tick(...) else sc0c end`:

```march
          let sc1b = if frame % tick_period() == 0 do
            scene_with_biome(sc1, Biome.tick(scene_biome(sc1), scene_world(sc1), seed, Biome.default_rate() *. int_to_float(biome_rate) /. 100.0))
          else sc1 end
```

and use `sc1b` where `sc1` was used from here on. `seed` must be in scope in the frame loop — thread it through `Knobs` if it is not (it is read at line ~1137 as `seed_env`; check the name the loop actually sees).

- [ ] **Step 4: The knob**

Extend `Knobs` with a trailing `Int` (`biome_rate`, documented as `biome ease rate in percent, 100 = one CF_DAY per full swing`), destructure it, and construct with `Env.get_int("CF_BIOME_RATE", 100)`.

- [ ] **Step 5: Build, test, and measure the tick**

```bash
forge check && forge build --release && forge lint --strict && forge test
```

Then the tick cost. Add a one-off timing print around the biome tick (same style as the existing `-- allocation gauge` prints) and run:

```bash
MARCH_NUM_SCHEDULERS=1 CF_NOMOUSE=1 CF_FRAMES=200 CF_SEED=7 ./.march/build/release/cube_forge 2>&1 | grep biome
```

Expected: a few milliseconds per tick at most. **If it is tens of milliseconds, an array is being copied per write** — find the `set_u8`/`set_int` on an array that is still referenced elsewhere (G68) and give it a fresh copy. Remove the timing print before committing, and record the number in `RESULTS.md`.

- [ ] **Step 6: Commit**

```bash
git add lib/cube_forge.march RESULTS.md
git commit -m "biome: field in the scene, ticked and edit-aware

Built at startup, ticked whole on tick_period() alongside the water actors,
told about every block edit. CF_BIOME_RATE scales the ease rate in percent."
```

---

## Task 5: The map view shows it

**Files:**
- Modify: `native/cf_shim.c`
- Modify: `lib/cube_forge/ffi/window.march`
- Modify: `lib/cube_forge.march`
- Modify: `RESULTS.md`, `todos.md`, `docs/superpowers/specs/2026-09-04-biomes-design.md`

**Interfaces:**
- Produces: `Win.biome_map_upload(biomes : NativeU8Arr, n : Int) : Unit` (slot 248); drawn via the existing `gfx_draw_marker` path.

- [ ] **Step 1: The C quad builder**

16,384 quads is 98k vertices — far too many to push from March per tick (G68), so the shim builds them, exactly as it does for precipitation. Add near `cf_precip_frame`:

```c
#define CF_BIOME_SLOT 248
#define CF_BIOME_Y    259.0f     /* under the marker at 260, above any terrain */
/* One flat colour per biome id, in the order CubeForge.Biome declares them:
 * tundra, taiga, grassland, forest, desert, wetland, beach, alpine. */
static const float CF_BIOME_RGB[8][3] = {
    {0.86f, 0.90f, 0.95f}, {0.25f, 0.45f, 0.35f}, {0.55f, 0.75f, 0.30f}, {0.15f, 0.50f, 0.15f},
    {0.90f, 0.80f, 0.45f}, {0.35f, 0.55f, 0.50f}, {0.95f, 0.90f, 0.70f}, {0.60f, 0.60f, 0.62f},
};
static float *g_biome_vtx = NULL;
static int64_t g_biome_cap = 0;

/* Build and upload a coloured quad per column from the biome id array. Colour
 * rides in the uv/layer slots (the marker idiom), shade 1, face 0, fx plain. */
void cf_biome_map_upload(void *biomes, int64_t n) {
    int64_t cells = n * n;
    if (cells > g_biome_cap) { free(g_biome_vtx); g_biome_vtx = (float *)malloc((size_t)cells * 6 * CF_VERT_FLOATS * sizeof(float)); g_biome_cap = cells; }
    const unsigned char *b = (const unsigned char *)narr_data(biomes);
    for (int64_t i = 0; i < cells; i++) {
        float x0 = (float)(i % n), z0 = (float)(i / n), x1 = x0 + 1.0f, z1 = z0 + 1.0f;
        const float *c = CF_BIOME_RGB[b[i] & 7];
        float *v = g_biome_vtx + i * 6 * CF_VERT_FLOATS;
        /* winding matches the mesher's top face: (x0,z0)->(x0,z1)->(x1,z1)->(x1,z0) */
        pcl_vert(v + 0 * CF_VERT_FLOATS, x0, CF_BIOME_Y, z0, c[0], c[1], c[2], 255.0f);
        pcl_vert(v + 1 * CF_VERT_FLOATS, x0, CF_BIOME_Y, z1, c[0], c[1], c[2], 255.0f);
        pcl_vert(v + 2 * CF_VERT_FLOATS, x1, CF_BIOME_Y, z1, c[0], c[1], c[2], 255.0f);
        pcl_vert(v + 3 * CF_VERT_FLOATS, x0, CF_BIOME_Y, z0, c[0], c[1], c[2], 255.0f);
        pcl_vert(v + 4 * CF_VERT_FLOATS, x1, CF_BIOME_Y, z1, c[0], c[1], c[2], 255.0f);
        pcl_vert(v + 5 * CF_VERT_FLOATS, x1, CF_BIOME_Y, z0, c[0], c[1], c[2], 255.0f);
    }
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo[CF_BIOME_SLOT]);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(cells * 6 * CF_VERT_FLOATS * (int64_t)sizeof(float)), g_biome_vtx, GL_STATIC_DRAW);
}
```

`pcl_vert` writes shade 1.0 and face 0.0 already; `255.0f` as fx is effect 0 alpha 1. It must be declared above this function (it is, in the precipitation block).

- [ ] **Step 2: Bind it**

`window.march`, extern block:
```march
    fn x_biome_map_upload(biomes: NativeU8Arr, n: Int): Unit = "cf_biome_map_upload"
```
wrapper:
```march
  doc "Upload the biome map as a flat coloured quad per column into slot 248, for the map view."
  fn biome_map_upload(biomes : NativeU8Arr, n : Int) : Unit do x_biome_map_upload(biomes, n) end
```

- [ ] **Step 3: Draw it in map mode**

Add `CF_BIOME_MAP` to `Knobs` as a `Bool` (`Env.get_int("CF_BIOME_MAP", 0) == 1`). Upload on the tick boundary when it changed, and draw right after the marker draw:

```march
          if map_mode1 && biome_map do
            if frame % tick_period() == 0 do Win.biome_map_upload(Biome.field_biomes(scene_biome(sc1b)), World.side(scene_world(sc1b)) * 16) else () end
            CubeForge.Ffi.Window.gfx_draw_marker(248, World.side(scene_world(sc1b)) * 16 * World.side(scene_world(sc1b)) * 16 * 6)
          else () end
```

Because it draws at y = 259 through the marker path (depth test on, unlit), it sits under the marker and over all terrain. The first frame is frame 0, which is a tick boundary, so the buffer is populated before the first draw.

- [ ] **Step 4: Verify with a dump**

```bash
forge build --release 2>&1 | grep -c "^-- ERROR"
MARCH_NUM_SCHEDULERS=1 CF_NOMOUSE=1 CF_BIOME_MAP=1 CF_AUTOMAP=0 CF_FRAMES=40 CF_DUMP_FRAME=30 CF_DUMP=/tmp/biomes.bmp CF_SEED=7 ./.march/build/release/cube_forge >/dev/null 2>&1
sips -s format png /tmp/biomes.bmp --out docs/biome-map.png
```

Look at it. Expected: distinct coloured regions; beach (pale sand) hugging every shoreline; alpine grey on the peaks; the lake surrounded by a wetland/forest ring fading to drier biomes outward. If it is a single flat colour, `temp_target`'s octave is not spanning the map — check the `0.018` scale reached `value2`.

Then the canal, end to end:

```bash
MARCH_NUM_SCHEDULERS=1 CF_NOMOUSE=1 CF_BIOME_MAP=1 CF_AUTOMAP=0 CF_BIOME_RATE=20000 CF_AUTOFLOW=10 CF_FRAMES=400 CF_DUMP_FRAME=390 CF_DUMP=/tmp/biomes-after.bmp CF_SEED=7 ./.march/build/release/cube_forge >/dev/null 2>&1
python3 scratch/cmpframe.py /tmp/biomes.bmp /tmp/biomes-after.bmp
```

`CF_AUTOFLOW` places water (see its existing use). Expected: a nonzero difference concentrated around where the water went. Save the after-image as `docs/biome-map-canal.png`.

- [ ] **Step 5: Docs**

`RESULTS.md`: a `## Biomes` section with the tick cost from Task 4 and the two images. `todos.md`: tick the biome entry's phase 1. The spec: add one line under §3 noting the hold counter replaces the value dead band, and why.

- [ ] **Step 6: Commit**

```bash
git add native/cf_shim.c lib/cube_forge/ffi/window.march lib/cube_forge.march RESULTS.md todos.md docs/
git commit -m "biome: map view colouring

A coloured quad per column, built in the shim (98k vertices is far past what
March can push per tick -- GAPS G68) and drawn through the marker path under
the marker at y = 259. CF_BIOME_MAP=1. This is phase 1's only verification
surface: without it the field is observable only through effects it does not
have yet."
```

---

## Self-Review

**Spec coverage (phase 1).** §1 axes and table → Task 1. §2 whole-field recompute, heightmap, water map, BFS → Task 2; O(1) edit → Task 3. §3 easing → Task 3 (`ease`, `default_rate`), anti-flap → hold counter (documented substitution). §8 map view → Task 5. §10 verification: classification (T1), heightmap agreement (T3), canal (T3 + T5 end to end), levelling (T3), no-flap (T3), tick cost (T4). §11 eased axes as world state — nothing to do until persistence exists; the arrays live in `Scene`.

**Not in this plan:** §4 weather, §5 retexturing, §6 vegetation, §7 new blocks — phases 2-4, planned after the map shows the field is right.

**Type consistency.** `Field` has six slots throughout (height, water, temp, moist, biome, hold). `distances` returns `NativeIntArr`; `classify` and `tick_go` read it with `get_int`. `note_edit` and `tick` both return `Field`. `Biome.field_biomes` returns the `NativeU8Arr` the C side reads.

**One thing to verify on arrival:** `W.block_at(w, x, y, z)` — the plan assumes this accessor exists on `World` (the G64 note in `world.march` refers to it). If it is named differently, every use is in `surface_go` and `wet_at`.
