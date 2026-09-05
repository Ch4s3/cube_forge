# New Terrain Blocks Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add basalt, mud, and ice as three new full-cube terrain blocks, per `docs/superpowers/specs/2026-09-04-new-terrain-blocks-design.md`.

**Architecture:** All three are plain opaque solid block ids (22, 23, 24) needing only a texture layer and a generation rule — no new mesh geometry, no new mesher/inventory special-casing (`is_solid`, `is_collidable`, `yield_of` already cover any nonzero non-water id generically). Basalt is a depth-only override in `Chunk.fill_column_mat`. Mud is a per-column noise pick alongside clay in `Biome.palette`, which changes that function's signature to take the column coordinates — four call sites to update. Ice is a generation-time altitude override of the single water block `plant_springs_go` places.

**Tech Stack:** March (forge).

## Global Constraints

- Block ids: `basalt() = 22`, `ice() = 23`, `mud() = 24` in `lib/cube_forge/chunk.march`, following the existing `sand()`/`snow()`/`granite()`/... pattern (a zero-arg `fn` returning the literal id).
- Texture layers: 17 = basalt, 18 = ice, 19 = mud; `Texture.layers()` goes from 17 to 20.
- `Biome.palette` signature changes from `palette(id : Int) : Int` to `palette(id : Int, x : Int, z : Int) : Int`. Every call site must be updated in the same task that changes the signature, or the build breaks: `lib/cube_forge/biome.march:501` (`migrations_go`), `lib/cube_forge.march:743` (`migrate_go`), `lib/cube_forge/veg.march:55` and `:121` (`can_grow`, `can_grow_bush`), `test/biome_test.march:158` and `:168` (`apply_palette`, `all_mismatched`).
- Cycle per task: `forge check && forge build && forge test`.
- No melting, slipperiness, tundra-biome freezing, obsidian, or new biome — out of scope per the design doc's §6.

---

## Task 1: Block ids and textures

**Files:**
- Modify: `lib/cube_forge/chunk.march` (new id functions `basalt()`, `ice()`, `mud()`)
- Modify: `lib/cube_forge/texture.march` (`checkerboard`, `layer_for`, `layers()`)
- Test: `test/terrain_test.march`

**Interfaces:**
- Produces: `Chunk.basalt() : Int` (22), `Chunk.ice() : Int` (23), `Chunk.mud() : Int` (24). `Texture.layer_for(id) : Float` returns 17.0/18.0/19.0 for these three ids.

- [ ] **Step 1: Write the failing test for the new ids and their texture layers**

Add to `test/terrain_test.march`, inside the existing `describe "surface materials"` block (or a new `describe`):

```march
  describe "new terrain block ids" do
    test "basalt, ice and mud have distinct ids above clay" do
      Test.assert_eq_int(C.basalt(), 22, "basalt id")
      Test.assert_eq_int(C.ice(), 23, "ice id")
      Test.assert_eq_int(C.mud(), 24, "mud id")
    end
    test "each has its own texture layer" do
      Test.assert_true(CubeForge.Texture.layer_for(C.basalt()) == 17.0, "basalt layer")
      Test.assert_true(CubeForge.Texture.layer_for(C.ice()) == 18.0, "ice layer")
      Test.assert_true(CubeForge.Texture.layer_for(C.mud()) == 19.0, "mud layer")
      Test.assert_true(CubeForge.Texture.layer_for(C.basalt()) != CubeForge.Texture.layer_for(C.mud()), "basalt and mud are visually distinct")
    end
  end
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `forge test`
Expected: FAIL — `C.basalt`, `C.ice`, `C.mud` are undefined.

- [ ] **Step 3: Add the block id functions**

In `lib/cube_forge/chunk.march`, right after `fn clay() : Int do 21 end` (and before `fn is_log(...)`):

```march
  doc "Deep volcanic rock: the material below the deep stone band (see fill_column_mat)."
  fn basalt() : Int do 22 end
  doc "Frozen water: generation-time only, placed instead of a spring above the snow line."
  fn ice() : Int do 23 end
  doc "Wetland surface variant alongside clay, picked per column."
  fn mud() : Int do 24 end
```

- [ ] **Step 4: Add the three texture layers**

In `lib/cube_forge/texture.march`, change `layers()` and extend `checkerboard()`:

```march
  fn layers() : Int do 20 end
```

```march
  fn checkerboard() : NativeU8Arr do
    let a0 = checker_go(NativeArray.make_u8(20 * 16 * 16 * 4, 0), 0)
    let a1 = water_go(a0, 0)
    let a2 = speckle_go(a1, 2, 0, 134, 96, 67)
    let a3 = speckle_go(a2, 3, 0, 128, 128, 132)
    let a4 = speckle_go(a3, 4, 0, 219, 203, 146)
    let a5 = speckle_go(a4, 5, 0, 242, 246, 250)
    let a6 = speckle_go(a5, 6, 0, 150, 141, 138)
    let a7 = speckle_go(a6, 7, 0, 46, 44, 48)
    let a8 = bark_go(a7, 8, 0)
    let a9 = rings_go(a8, 9, 0)
    let a10 = leaves_go(a9, 10, 0, 62, 128, 54)
    let a11 = leaves_go(a10, 11, 0, 38, 92, 68)
    let a12 = bark_tinted_go(a11, 12, 0, 74, 56, 42)
    let a13 = speckle_go(a12, 13, 0, 112, 110, 106)
    let a14 = speckle_go(a13, 14, 0, 158, 142, 124)
    let a15 = spring_go(a14, 0)
    let a16 = speckle_go(a15, 16, 0, 226, 236, 246)
    let a17 = speckle_go(a16, 17, 0, 40, 38, 42)
    let a18 = speckle_go(a17, 18, 0, 205, 225, 240)
    speckle_go(a18, 19, 0, 70, 48, 38)
  end
```

(`a16` here is the existing `speckle_go(a15, 16, ...)` foam layer call — only its variable name changes from the tail-return to a threaded `let`, to make room for the three new layers after it. Verify against the current file before editing so the diff is just "add three layers", not an accidental foam-layer edit.)

- [ ] **Step 5: Add the three `layer_for` branches and update the doc comment**

In `lib/cube_forge/texture.march`, extend `layer_for`:

```march
  fn layer_for(id : Int) : Float do
    if id == 2 do 2.0
    else if id == 3 do 3.0
    else if id >= 4 && id <= 11 do 1.0
    else if id == 12 do 4.0
    else if id == 13 do 5.0
    else if id == 14 do 6.0
    else if id == 15 do 7.0
    else if id == 16 do 8.0
    else if id == 18 do 12.0
    else if id == 17 do 10.0
    else if id == 19 do 11.0
    else if id == 20 do 13.0
    else if id == 21 do 14.0
    else if id == 22 do 17.0
    else if id == 23 do 18.0
    else if id == 24 do 19.0
    else 0.0 end end end end end end end end end end end end end end end end
  end
```

(three new `else if` branches means three more closing `end`s at the tail — the existing chain has 13 branches before the final `else 0.0` in the current file, hence 13 trailing `end`s after `0.0 end`; adding three branches for ids 22/23/24 makes it 16 branches and 16 trailing `end`s, as written above. Count them if you're unsure — a mismatch here fails to compile immediately, so `forge check` catches it right away.)

Also update the `checkerboard()` doc comment above it to mention layers 17-19 (basalt, ice, mud), matching the existing style (`"...15 spring (icon only), 16 foam, 17 basalt, 18 ice, 19 mud."`).

- [ ] **Step 6: Run the test to verify it passes**

Run: `forge test`
Expected: PASS

- [ ] **Step 7: Full cycle and commit**

Run: `forge check && forge build && forge test`
Expected: all pass.

```bash
git add lib/cube_forge/chunk.march lib/cube_forge/texture.march test/terrain_test.march
git commit -m "blocks: add basalt, ice and mud ids with texture layers"
```

## Task 2: Basalt — deep stone band

**Files:**
- Modify: `lib/cube_forge/chunk.march` (`fill_column_mat`)
- Test: `test/terrain_test.march`

**Interfaces:**
- Consumes: `Chunk.basalt()` from Task 1.
- Produces: `Chunk.deep_stone_depth() : Int` (24), used only inside `fill_column_mat`.

- [ ] **Step 1: Write the failing test**

`fill_column_mat` is a `pfn` (module-private — see March's visibility rule: `fn`/`type` are public, `pfn`/`ptype` are private, and a private definition cannot be called from another module, tests included). Test through the public `Chunk.test_terrain()` instead, which fills every column of a 16x16 chunk via `fill_column` -> `fill_column_mat` with `height_at(x, z)` as the top. At `(x, z) = (0, 0)`, `height_at(0, 0)` is `64 + (3*sin(0) + 2*cos(0) rounded) + 2 = 68` — comfortably above `deep_stone_depth()` (24), so y = 10 (basalt), y = 24 (the boundary, still plain stone since the check is `y < deep_stone_depth()`), y = 40 (plain stone), and y = 1 (bedrock) are all within the filled column and land in the branches this task changes.

Add to `test/terrain_test.march`, in `describe "surface materials"` (or its own `describe`):

```march
  describe "deep stone band" do
    test "test_terrain uses basalt below deep_stone_depth, stone above it" do
      let c = C.test_terrain()
      Test.assert_eq_int(C.get(c, 0, 10, 0), C.basalt(), "below the deep stone depth is basalt")
      Test.assert_eq_int(C.get(c, 0, C.deep_stone_depth(), 0), 3, "at the depth boundary is plain stone")
      Test.assert_eq_int(C.get(c, 0, 40, 0), 3, "well above the boundary is plain stone")
      Test.assert_eq_int(C.get(c, 0, 1, 0), C.bedrock(), "still bedrock at the floor")
    end
  end
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `forge test`
Expected: FAIL — `C.deep_stone_depth` undefined, and y=10 currently returns plain stone (3), not basalt.

- [ ] **Step 3: Implement the depth band**

In `lib/cube_forge/chunk.march`, add near `bedrock_depth()`:

```march
  doc "Below this depth, the deep stone band is basalt instead of plain stone."
  fn deep_stone_depth() : Int do 24 end
```

Change the "everything else is plain stone" branch of `fill_column_mat`:

```march
  pfn fill_column_mat(c : Chunk, x : Int, z : Int, y : Int, top : Int, surface : Int, sub : Int) : Chunk do
    if y > top do fill_water(c, x, z, top + 1)
    else
      let id = if y < bedrock_depth() do bedrock()
               else if y == top do surface
               else if y > top - 4 do sub
               else if y < deep_stone_depth() do basalt()
               else 3 end end end end
      fill_column_mat(set(c, x, y, z, id), x, z, y + 1, top, surface, sub)
    end
  end
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `forge test`
Expected: PASS

- [ ] **Step 5: Full cycle and commit**

Run: `forge check && forge build && forge test`

```bash
git add lib/cube_forge/chunk.march test/terrain_test.march
git commit -m "blocks: basalt replaces plain stone below the deep stone band"
```

## Task 3: Mud — wetland surface variant alongside clay

**Files:**
- Modify: `lib/cube_forge/biome.march` (`palette`, `is_palette_block`, `migrations_go`)
- Modify: `lib/cube_forge.march` (`migrate_go`)
- Modify: `lib/cube_forge/veg.march` (`can_grow`, `can_grow_bush`)
- Modify: `test/biome_test.march` (`apply_palette`, `all_mismatched`)
- Test: `test/biome_test.march`

**Interfaces:**
- Consumes: `Chunk.mud()` from Task 1, `Noise.hash2(ix, iz, seed) : Float` (existing, in `[0, 1)`, deterministic).
- Produces: `Biome.palette(id : Int, x : Int, z : Int) : Int` — **signature changed** from the old `palette(id : Int) : Int`. Every caller in the codebase is updated in this task.

- [ ] **Step 1: Write the failing test for the mud/clay mix**

Add to `test/biome_test.march`, near the existing `describe "Biome.migrations"` block:

```march
  describe "Biome.palette wetland mix" do
    test "wetland picks clay or mud per column, and stays a palette block" do
      let a = Biome.palette(Biome.b_wetland(), 0, 0)
      let b = Biome.palette(Biome.b_wetland(), 1, 0)
      Test.assert_true(a == C.clay() || a == C.mud(), "column (0,0) is clay or mud")
      Test.assert_true(b == C.clay() || b == C.mud(), "column (1,0) is clay or mud")
      Test.assert_true(Biome.is_palette_block(C.mud()), "mud is a palette block")
    end
    test "the same column is always the same pick" do
      Test.assert_eq_int(Biome.palette(Biome.b_wetland(), 42, 17), Biome.palette(Biome.b_wetland(), 42, 17), "deterministic")
    end
    test "other biomes are unaffected by the x, z parameters" do
      Test.assert_eq_int(Biome.palette(Biome.b_alpine(), 0, 0), C.gravel(), "alpine still gravel")
      Test.assert_eq_int(Biome.palette(Biome.b_alpine(), 99, 99), C.gravel(), "alpine ignores column")
    end
  end
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `forge test`
Expected: FAIL to compile — `Biome.palette` currently takes one argument.

- [ ] **Step 3: Change `palette`'s signature and add the wetland hash**

In `lib/cube_forge/biome.march`, add near `palette`:

```march
  doc "Fraction of wetland columns that pick mud over clay."
  fn mud_fraction() : Float do 0.4 end
  doc "Arbitrary fixed seed for the clay/mud split: decorative, not tied to the world seed."
  fn wetland_mix_seed() : Int do 91537 end

  doc "Clay or mud for one wetland column, mixed by a fixed per-column hash."
  fn wetland_surface(x : Int, z : Int) : Int do
    if N.hash2(x, z, wetland_mix_seed()) < mud_fraction() do C.mud() else C.clay() end
  end
```

Change `palette` itself:

```march
  doc "The surface block a biome wants. Granite is absent on purpose: cliff faces stay on the slope rule in every biome."
  fn palette(id : Int, x : Int, z : Int) : Int do
    if id == b_tundra() do C.snow()
    else if id == b_taiga() do 2                       -- dirt
    else if id == b_desert() || id == b_beach() do C.sand()
    else if id == b_wetland() do wetland_surface(x, z)
    else if id == b_alpine() do C.gravel()
    else 1 end end end end end                         -- grass: grassland, forest
  end
```

- [ ] **Step 4: Add `mud()` to `is_palette_block`**

```march
  doc "Only these ever migrate: the natural surface blocks. Granite, stone, water, logs, leaves and bedrock are never rewritten."
  fn is_palette_block(id : Int) : Bool do
    id == 1 || id == 2 || id == C.sand() || id == C.snow() || id == C.gravel() || id == C.clay() || id == C.mud()
  end
```

- [ ] **Step 5: Update `migrations_go`'s call site**

In `lib/cube_forge/biome.march`, `migrations_go` currently has `let want = palette(biome_of(f, x, z))`. Change to:

```march
      let want = palette(biome_of(f, x, z), x, z)
```

- [ ] **Step 6: Update `migrate_go` in `lib/cube_forge.march`**

Currently `let id = Biome.palette(Biome.biome_of(bio, x, z))`. Change to:

```march
        let id = Biome.palette(Biome.biome_of(bio, x, z), x, z)
```

- [ ] **Step 7: Update `veg.march`'s two call sites**

In `lib/cube_forge/veg.march`, `can_grow` (around line 55) and `can_grow_bush` (around line 121) both have `else if W.block_at(w, x, h, z) != Biome.palette(b) do false`. Both `x` and `z` are already in scope in each function. Change both to:

```march
      else if W.block_at(w, x, h, z) != Biome.palette(b, x, z) do false
```

- [ ] **Step 8: Update the two test helpers in `test/biome_test.march`**

```march
  pfn apply_palette(f : CubeForge.Biome.Field, w : CubeForge.World.World, cols : List(Int)) : CubeForge.World.World do
    match cols do
      Nil -> w
      Cons(ci, rest) ->
        let x = ci % 128
        let z = ci / 128
        apply_palette(f, CubeForge.World.set_block(w, x, Biome.height_of(f, x, z), z, Biome.palette(Biome.biome_of(f, x, z), x, z)), rest)
    end
  end
  pfn all_mismatched(f : CubeForge.Biome.Field, w : CubeForge.World.World, cols : List(Int)) : Bool do
    match cols do
      Nil -> true
      Cons(ci, rest) ->
        let x = ci % 128
        let z = ci / 128
        let sfc = CubeForge.World.block_at(w, x, Biome.height_of(f, x, z), z)
        if sfc == Biome.palette(Biome.biome_of(f, x, z), x, z) do false else all_mismatched(f, w, rest) end
    end
  end
```

- [ ] **Step 9: Run the full test suite to verify everything passes**

Run: `forge test`
Expected: PASS — including the pre-existing `Biome.migrations` tests, which now exercise the two-argument `palette` transitively.

- [ ] **Step 10: Full cycle and commit**

Run: `forge check && forge build && forge test`

```bash
git add lib/cube_forge/biome.march lib/cube_forge.march lib/cube_forge/veg.march test/biome_test.march
git commit -m "blocks: mud alongside clay as a wetland surface variant"
```

## Task 4: Ice — altitude-frozen springs

**Files:**
- Modify: `lib/cube_forge/chunk.march` (`plant_springs_go`)
- Test: `test/terrain_test.march`

**Interfaces:**
- Consumes: `Chunk.ice()` from Task 1, `Noise.snow_line() : Int` (existing, 95).
- Produces: nothing new consumed elsewhere — `plant_springs_go` is internal to `plant`.

- [ ] **Step 1: Write the failing test**

Add to `test/terrain_test.march`, reusing the same `Trees.spring_at_cell` walk that `describe "Trees.spring_at_cell"` further down the file already uses (so this doesn't reinvent column plumbing). Scanning several seeds rather than one guards against the test passing vacuously just because a single seed happens not to generate a high spring:

```march
  describe "spring freezing" do
    test "springs at or above the snow line freeze to ice; below it, they're water" do
      Test.assert_true(all_springs_frozen_correctly(0, 0), "every sampled spring, across seeds, matches the altitude rule")
      Test.assert_true(any_spring_at_or_above_snowline(0, 0), "at least one sampled spring is at/above the snow line, so the ice branch is actually exercised")
    end
  end
  pfn all_springs_frozen_correctly(seed : Int, i : Int) : Bool do
    if seed >= 20 do true
    else if i >= 256 do all_springs_frozen_correctly(seed + 1, 0)
    else
      let col = CubeForge.Trees.spring_at_cell(i % 16, i / 16, seed)
      if col < 0 do all_springs_frozen_correctly(seed, i + 1)
      else
        let wx = col % 4096
        let wz = col / 4096
        let h = CubeForge.Noise.height(wx, wz, seed)
        let ch = CubeForge.Chunk.generate(wx - wx % 16, wz - wz % 16, seed)
        let block = CubeForge.Chunk.get(ch, wx % 16, h, wz % 16)
        let want = if h >= CubeForge.Noise.snow_line() do CubeForge.Chunk.ice() else CubeForge.Chunk.water() end
        if block == want do all_springs_frozen_correctly(seed, i + 1) else false end
      end
    end end
  end
  pfn any_spring_at_or_above_snowline(seed : Int, i : Int) : Bool do
    if seed >= 20 do false
    else if i >= 256 do any_spring_at_or_above_snowline(seed + 1, 0)
    else
      let col = CubeForge.Trees.spring_at_cell(i % 16, i / 16, seed)
      if col < 0 do any_spring_at_or_above_snowline(seed, i + 1)
      else
        let wx = col % 4096
        let wz = col / 4096
        if CubeForge.Noise.height(wx, wz, seed) >= CubeForge.Noise.snow_line() do true
        else any_spring_at_or_above_snowline(seed, i + 1) end
      end
    end end
  end
```

**Erratum (found during Task 4 execution):** both helpers above were originally written with one `end` too few — each has an `if seed >= 20 do ... else if ... do ... else ... end` chain (two `if`/`else if` keywords, needing two closing `end`s) but only one trailing `end` was given before the function's own closing `end`. This passed `forge check` unnoticed by the plan's author and was only caught when `forge test` silently dropped the whole file per GAPS.md G60 (a parse error in one test file drops that file's tests with no error, not a build failure) — the count is 220 with the bug, ~232 expected without it. Fixed above to `end end` where the chain closes.

- [ ] **Step 2: Run the test to verify it fails**

Run: `forge test`
Expected: FAIL — every spring currently generates as `water()` regardless of height, so `all_springs_frozen_correctly` returns `false` as soon as it walks past the first high spring (which `any_spring_at_or_above_snowline` confirms exists within the 20-seed sample).

- [ ] **Step 3: Implement the altitude freeze**

In `lib/cube_forge/chunk.march`, `plant_springs_go` currently has:

```march
        let c1 = if lx < 0 || lx >= 16 || lz < 0 || lz >= 16 || h < 1 || h >= 255 do c else set(c, lx, h, lz, water()) end
```

Change the placed id to depend on altitude:

```march
        let sid = if h >= CubeForge.Noise.snow_line() do ice() else water() end
        let c1 = if lx < 0 || lx >= 16 || lz < 0 || lz >= 16 || h < 1 || h >= 255 do c else set(c, lx, h, lz, sid) end
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `forge test`
Expected: PASS — both the altitude-rule check and the "branch actually exercised" check.

- [ ] **Step 5: Full cycle and commit**

Run: `forge check && forge build && forge test`

```bash
git add lib/cube_forge/chunk.march test/terrain_test.march
git commit -m "blocks: springs at or above the snow line generate as ice"
```

---

## Self-Review

- Spec §1 (block ids) -> Task 1.
- Spec §2 (basalt depth band) -> Task 2.
- Spec §3 (mud/clay wetland mix) -> Task 3.
- Spec §4 (ice, altitude-only per the scope note) -> Task 4.
- Spec §5 (textures) -> Task 1.
- Spec §6 (out of scope) -> nothing built for obsidian, melting, tundra freezing, badlands biome, or subsurface mixing; no task touches them.
- Spec §7 (landing order) -> followed: ids+textures, then basalt, then mud (palette signature change), then ice.
- Names used consistently across tasks: `Chunk.basalt()`, `Chunk.ice()`, `Chunk.mud()`, `Chunk.deep_stone_depth()`, `Biome.palette(id, x, z)`, `Biome.wetland_surface(x, z)`, `Biome.mud_fraction()`, `Biome.wetland_mix_seed()` — each defined exactly once (Task 1 or the task that introduces it) and referenced identically everywhere else.
- Task 3 is the only one with cross-file fan-out (four call sites beyond `biome.march` itself); every call site found by grepping the whole repo for `Biome.palette` and `palette(` is listed and updated in that task's steps, so the build cannot break silently on a missed caller.
