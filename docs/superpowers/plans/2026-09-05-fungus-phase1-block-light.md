# Fungus phase 1: block light channel — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A second per-voxel light channel, flooded from emissive blocks, that reaches the screen at night and is repaired incrementally on block edits — verified with one temporary emissive block before any fungus exists.

**Architecture:** The block-light field is a second 4 MB `NativeU8Arr` on `World`, beside skylight. It reuses the skylight module's level-synchronous sweep (GAPS G63 rules out a BFS queue) with a different seed: `Light.emission(id)` per voxel instead of the sky. The mesher reads both fields per corner, widens the greedy key corner from 6 to 10 bits, and packs sky-shade and block-shade into the one existing `shade` vertex float so the vertex layout stays at nine floats. The fragment shader unpacks them and takes `max(sky * daylight, block * GLOW)`.

**Tech Stack:** March (`forge build`, `forge test`, `forge lint --strict`), the C shim's GLSL 330 shaders in `native/cf_shim.c`.

Spec: `docs/superpowers/specs/2026-09-04-fungus-design.md` §6 and §9 step 1.

## Global Constraints

- `&&` / `||` do not short-circuit (GAPS G33): a bounds guard must be a separate `if` from the array read it protects.
- Reading a `NativeArray` and then writing the same array in one function makes every write a full copy (GAPS G21/G63): every propagation pass reads one array and writes a different one. Follow the shapes already in `light.march`.
- Never call `World.block_at` per voxel in a bulk pass (GAPS G64): fetch the chunk once and read through `Chunk.get_or_air`.
- Identifiers `by` and `on` are reserved (GAPS G65).
- A `doc` string and an attribute like `@[no_alloc]` cannot coexist on one function (GAPS G56): use a `--` comment above the attribute.
- Vertex layout stays **nine floats**. `Vertex.floats()` remains 9 and its test stays green.
- Frame budget: `scratch/frame_budget.sh` (12 ms) must still pass at the end.
- Commit after every task. No attribution lines in commit messages.

## File structure

| file | responsibility in this phase |
|---|---|
| `lib/cube_forge/light.march` | `emission`, block-light seeding (`seed_emitters`, `flood_block`), block-light relight (`relight_block_marked`), `or_marks`. Sweep helpers are shared unchanged. |
| `lib/cube_forge/world.march` | `World` gains the block-light field: accessors, `relight_all` floods both, `relight_marked` repairs both and unions the marks. |
| `lib/cube_forge/chunk.march` | `glow_cap()` block id 22, the temporary emitter. |
| `lib/cube_forge/texture.march` | layer 17 for the glow cap; `layers()` 17 → 18. |
| `lib/cube_forge/vertex.march` | `pack_shade`, `sky_of`, `blk_of`, `brightness_of`. |
| `lib/cube_forge/mesher.march` | 10-bit corners carrying block light, shade packing at emit, `lb` threaded to the section meshers. |
| `lib/cube_forge/chunk_mesh.march` | passes `World.block_light(world)` to the section meshers. |
| `native/cf_shim.c` | fragment shader unpacks the shade word and adds the glow term. |
| `lib/cube_forge.march` | edit paths use `World.relight_marked`; startup floods block light; `CF_AUTOGLOW` knob. |
| `test/light_test.march`, `test/vertex_test.march`, `test/greedy_test.march` | tests. |

---

### Task 1: `World` carries a block-light field

**Files:**
- Modify: `lib/cube_forge/world.march:11-51,170-183`
- Modify: `test/light_test.march:18,33`, `test/veg_test.march:17`, `test/biome_test.march:18`, `test/flow_test.march:345`

**Interfaces:**
- Produces: `type World = World(Array.PVec(Chunk), NativeU8Arr, NativeU8Arr, NativeU8Arr, Int)` — chunks, skylight, **block light**, occupancy, side. `World.block_light(w) : NativeU8Arr`, `World.set_block_light(w, lb) : World`, `World.block_light_at(w, x, y, z) : Int`.

- [ ] **Step 1: Write the failing test** — append to `test/light_test.march` inside the module, before the final `end`:

```march
  describe "World block light field" do
    test "a fresh world has a dark block-light field that reads back what is written" do
      let w0 = air_world()
      Test.assert_eq_int(CubeForge.World.block_light_at(w0, 40, 100, 40), 0, "dark at birth")
      let lb = NativeArray.set_u8(L.new(), L.index(40, 100, 40), 9)
      let w1 = CubeForge.World.set_block_light(w0, lb)
      Test.assert_eq_int(CubeForge.World.block_light_at(w1, 40, 100, 40), 9, "written value")
      Test.assert_eq_int(CubeForge.World.light_at(w1, 40, 100, 40), 0, "skylight untouched")
    end
  end
```

- [ ] **Step 2: Run it to verify it fails**

Run: `forge test 2>&1 | tail -20`
Expected: a compile error naming `block_light_at` (unknown function).

- [ ] **Step 3: Widen `World`** — in `lib/cube_forge/world.march` replace the type and every match. The complete set of changed functions:

```march
  type World = World(Array.PVec(CubeForge.Chunk.Chunk), NativeU8Arr, NativeU8Arr, NativeU8Arr, Int)   -- chunks, skylight field, block-light field, occupancy field, side length in chunks

  fn side(w : World) : Int do match w do World(_, _, _, _, n) -> n end end

  doc "The world's skylight field (see CubeForge.Light)."
  fn light(w : World) : NativeU8Arr do match w do World(_, la, _, _, _) -> la end end

  doc "The world's block-light field: light emitted by blocks, independent of the sky (see CubeForge.Light.emission)."
  fn block_light(w : World) : NativeU8Arr do match w do World(_, _, lb, _, _) -> lb end end

  doc "The world's occupancy field: non-zero where a voxel blocks light. Used for ambient occlusion and uploaded to the shadow ray-marcher."
  fn occupancy(w : World) : NativeU8Arr do match w do World(_, _, _, o, _) -> o end end

  doc "Replace the occupancy field."
  fn set_occupancy(w : World, o : NativeU8Arr) : World do match w do World(cs, la, lb, _, n) -> World(cs, la, lb, o, n) end end

  doc "Update one voxel of the occupancy field after a block edit."
  fn set_occupied(w : World, x : Int, y : Int, z : Int, solid : Bool) : World do
    if x < 0 || x >= 128 || y < 0 || y >= 256 || z < 0 || z >= 128 do w
    else match w do World(cs, la, lb, o, n) ->
      World(cs, la, lb, NativeArray.set_u8(o, CubeForge.Light.occ_index(x, y, z), if solid do CubeForge.Light.occ_solid() else 0 end), n) end
    end
  end

  doc "Replace the world's skylight field."
  fn set_light(w : World, la : NativeU8Arr) : World do match w do World(cs, _, lb, o, n) -> World(cs, la, lb, o, n) end end

  doc "Replace the world's block-light field."
  fn set_block_light(w : World, lb : NativeU8Arr) : World do match w do World(cs, la, _, o, n) -> World(cs, la, lb, o, n) end end

  @[no_alloc]
  -- Skylight at a world coordinate; 0 outside the world.
  fn light_at(w : World, x : Int, y : Int, z : Int) : Int do
    match w do World(_, la, _, _, _) -> CubeForge.Light.get(la, x, y, z) end
  end

  @[no_alloc]
  -- Block light at a world coordinate; 0 outside the world.
  fn block_light_at(w : World, x : Int, y : Int, z : Int) : Int do
    match w do World(_, _, lb, _, _) -> CubeForge.Light.get(lb, x, y, z) end
  end
```

Then `chunk_at` (line 61) becomes `match w do World(cs, _, _, _, n) -> ...`, `set_chunk` (line 66) becomes `match w do World(cs, la, lb, o, n) -> World(Array.set(cs, cx + n * cz, c), la, lb, o, n) end`, and the two constructors at lines 177 and 182 become `World(Array.from_list(chunks), CubeForge.Light.new(), CubeForge.Light.new(), CubeForge.Light.new(), n)`.

- [ ] **Step 4: Fix the five test constructors** — each `CubeForge.World.World(<chunks>, L.new(), L.new(), 8)` (or `CubeForge.Light.new()` twice) gains a third `CubeForge.Light.new()` before the side length. Files and lines: `test/light_test.march:18,33`, `test/veg_test.march:17`, `test/biome_test.march:18`, `test/flow_test.march:345`.

- [ ] **Step 5: Run the tests**

Run: `forge test 2>&1 | tail -20`
Expected: all pass, including the new one.

- [ ] **Step 6: Commit**

```bash
git add lib/cube_forge/world.march test/light_test.march test/veg_test.march test/biome_test.march test/flow_test.march
git commit -m "feat(light): World carries a block-light field beside skylight"
```

---

### Task 2: A glow cap block, its texture layer, and `Light.emission`

**Files:**
- Modify: `lib/cube_forge/chunk.march:545` (after `clay`)
- Modify: `lib/cube_forge/texture.march:9-32,50,60-75`
- Modify: `lib/cube_forge/light.march:47` (after `is_opaque`)
- Test: `test/light_test.march`

**Interfaces:**
- Produces: `Chunk.glow_cap() : Int` = 22. `Light.emission(id : Int) : Int` — 10 for the glow cap, 0 otherwise. `Texture.layers()` = 18; layer 17 is the glow cap.

- [ ] **Step 1: Write the failing test** — append to `test/light_test.march`:

```march
  describe "Light.emission" do
    test "only the glow cap emits, at level 10" do
      Test.assert_eq_int(L.emission(C.glow_cap()), 10, "glow cap")
      Test.assert_eq_int(L.emission(0), 0, "air")
      Test.assert_eq_int(L.emission(3), 0, "stone")
      Test.assert_eq_int(L.emission(C.water()), 0, "water")
      Test.assert_eq_int(L.emission(C.oak_leaves()), 0, "leaves")
      Test.assert_true(L.is_opaque(C.glow_cap()), "a glow cap is a solid block")
    end
    test "the glow cap has its own texture layer" do
      Test.assert_eq_int(CubeForge.Texture.layers(), 18, "layer count")
      Test.assert_true(near(CubeForge.Texture.layer_for(C.glow_cap()), 17.0), "layer 17")
    end
  end
```

- [ ] **Step 2: Run it to verify it fails**

Run: `forge test 2>&1 | tail -20`
Expected: compile error, `glow_cap` / `emission` unknown.

- [ ] **Step 3: Implement.** In `chunk.march` after `fn clay()`:

```march
  doc "Glowing mushroom cap: the first emissive block. Placeholder until the fungus fruit blocks land; the block-light channel is verified with it."
  fn glow_cap() : Int do 22 end
```

In `light.march` after `is_opaque`:

```march
  @[no_alloc]
  -- Block light a voxel of block [id] emits: 0 for everything but emissive blocks.
  -- Levels are deliberately below max_level(): a cap at 10 lights a radius of
  -- about nine, and keeps every relight box small.
  fn emission(id : Int) : Int do
    if id == C.glow_cap() do 10 else 0 end
  end
```

In `texture.march`: the doc string gains `17 glow cap`; `checkerboard` allocates `18 * 16 * 16 * 4` and its last line becomes

```march
    let a16 = speckle_go(a15, 16, 0, 226, 236, 246)
    speckle_go(a16, 17, 0, 250, 214, 120)
```

`layers()` returns 18, and `layer_for` gains `else if id == 22 do 17.0` before the final `else 0.0` (and one more `end`).

- [ ] **Step 4: Run the tests**

Run: `forge test 2>&1 | tail -20`
Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add lib/cube_forge/chunk.march lib/cube_forge/texture.march lib/cube_forge/light.march test/light_test.march
git commit -m "feat(light): glow cap block 22 and Light.emission"
```

---

### Task 3: Full block-light flood

**Files:**
- Modify: `lib/cube_forge/light.march` (new section after `flood`, line 222)
- Modify: `lib/cube_forge/world.march:47` (`relight_all`)
- Test: `test/light_test.march`

**Interfaces:**
- Consumes: `Light.sweep_go`, `Light.sky_floor`, `Light.new`, `Chunk.get_idx`.
- Produces: `Light.seed_emitters(lb, w) : NativeU8Arr`, `Light.flood_block(w) : NativeU8Arr`. `World.relight_all(w)` now floods both fields.

- [ ] **Step 1: Write the failing tests** — append to `test/light_test.march`:

```march
  describe "Light.flood_block" do
    test "a glow cap in the open lights a sphere that falls off one per step" do
      let w = CubeForge.World.set_block(air_world(), 40, 100, 40, C.glow_cap())
      let lb = L.flood_block(w)
      Test.assert_eq_int(L.get(lb, 40, 100, 40), 10, "the emitter itself")
      Test.assert_eq_int(L.get(lb, 41, 100, 40), 9, "one step")
      Test.assert_eq_int(L.get(lb, 40, 105, 40), 5, "five steps up")
      Test.assert_eq_int(L.get(lb, 49, 100, 40), 1, "nine steps, the last light")
      Test.assert_eq_int(L.get(lb, 50, 100, 40), 0, "ten steps, dark")
      Test.assert_eq_int(L.get(lb, 43, 100, 43), 4, "manhattan six")
    end

    test "block light does not pass through stone but rounds a corner" do
      let w0 = slab_world(100)
      let w1 = CubeForge.World.set_block(w0, 40, 100, 40, 0)
      let w2 = CubeForge.World.set_block(w1, 40, 101, 40, C.glow_cap())
      let lb = L.flood_block(w2)
      Test.assert_eq_int(L.get(lb, 40, 99, 40), 8, "down the shaft, two steps")
      Test.assert_eq_int(L.get(lb, 41, 99, 40), 7, "along the gallery")
      Test.assert_eq_int(L.get(lb, 41, 100, 40), 0, "inside the slab")
    end

    test "a world with no emitters is dark everywhere" do
      let w = CubeForge.World.relight_all(CubeForge.World.generate(8, 1, 1))
      Test.assert_eq_int(first_difference(CubeForge.World.block_light(w), L.new()), -1, "all zero")
    end
  end
```

- [ ] **Step 2: Run to verify failure**

Run: `forge test 2>&1 | tail -20`
Expected: compile error, `flood_block` unknown.

- [ ] **Step 3: Implement** — add after `fn flood` in `light.march`:

```march
  -- ── Block light: the same sweep, seeded from emissive blocks ─────────────
  --
  -- The sweep does not care where light came from: a voxel at level L gives
  -- L - 1 - opacity to its neighbours whatever seeded it. So block light is
  -- the skylight machinery with a different seed pass -- every voxel whose
  -- block emits is written at its emission level, and spread() does the rest.
  -- Emission is capped below max_level(), so the sweep's top levels are no-ops.

  -- Seed one chunk's emitters. Write-only on [lb]; the chunk is fetched once (G64).
  pfn seed_emit_chunk_go(lb : NativeU8Arr, c : CubeForge.Chunk.Chunk, cx : Int, cz : Int, i : Int, stop : Int) : NativeU8Arr do
    if i >= stop do lb
    else
      let e = emission(C.get_idx(c, i))
      if e <= 0 do seed_emit_chunk_go(lb, c, cx, cz, i + 1, stop)
      else seed_emit_chunk_go(NativeArray.set_u8(lb, index(16 * cx + i % 16, i / 256, 16 * cz + (i / 16) % 16), e), c, cx, cz, i + 1, stop) end
    end
  end

  pfn seed_emit_go(lb : NativeU8Arr, w : CubeForge.World.World, ci : Int, n : Int, stop : Int) : NativeU8Arr do
    if ci >= n * n do lb
    else seed_emit_go(seed_emit_chunk_go(lb, CubeForge.World.chunk_at(w, ci % n, ci / n), ci % n, ci / n, 0, stop), w, ci + 1, n, stop) end
  end

  doc "Write every emissive block's emission level into [lb]. Scans only up to the highest non-air layer."
  fn seed_emitters(lb : NativeU8Arr, w : CubeForge.World.World) : NativeU8Arr do
    seed_emit_go(lb, w, 0, CubeForge.World.side(w), (sky_floor(w) + 1) * 256)
  end

  doc "Compute the world's block light from scratch: seed every emitter, then spread."
  fn flood_block(w : CubeForge.World.World) : NativeU8Arr do
    spread(seed_emitters(new(), w), w)
  end
```

In `world.march`:

```march
  doc "Recompute both light fields from scratch."
  fn relight_all(w : World) : World do
    set_block_light(set_light(w, CubeForge.Light.flood(w)), CubeForge.Light.flood_block(w))
  end
```

- [ ] **Step 4: Run the tests**

Run: `forge test 2>&1 | tail -20`
Expected: pass. If `manhattan six` fails with 4 vs another value, check the arithmetic: (43,100,43) is 3+3 = 6 steps from the emitter, so 10 - 6 = 4.

- [ ] **Step 5: Commit**

```bash
git add lib/cube_forge/light.march lib/cube_forge/world.march test/light_test.march
git commit -m "feat(light): full block-light flood from emissive blocks"
```

---

### Task 4: Incremental block-light relight and a combined `World.relight_marked`

**Files:**
- Modify: `lib/cube_forge/light.march` (after `relight_at`, line 416)
- Modify: `lib/cube_forge/world.march:49-52`
- Test: `test/light_test.march`

**Interfaces:**
- Consumes: `Light.zero_box_go`, `Light.box_sweep_go2`, `Light.copy_prefix`, `Light.mark_box`, `Light.marks_new`, `Light.clampi`, `Light.Relit`.
- Produces: `Light.relight_block_marked(lb, w, ex, ey, ez) : Relit`, `Light.or_marks(a, b) : NativeIntArr`, `World.Relit = Relit(World, NativeIntArr)`, `World.relight_marked(w, x, y, z) : World.Relit` (repairs both fields), `World.relit_world(r)`, `World.relit_marks(r)`. `World.relight_at` repairs both and drops the marks.

- [ ] **Step 1: Write the failing tests** — append to `test/light_test.march`:

```march
  describe "Light.relight_block_marked" do
    test "placing a glow cap matches a full re-flood and marks its sections" do
      let w0 = CubeForge.World.relight_all(CubeForge.World.generate(8, 1, 1))
      let w1 = CubeForge.World.set_block(w0, 40, 90, 40, C.glow_cap())
      let r = L.relight_block_marked(CubeForge.World.block_light(w1), w1, 40, 90, 40)
      let full = L.flood_block(w1)
      Test.assert_eq_int(first_difference(L.relit_field(r), full), -1, "incremental equals full re-flood")
      Test.assert_eq_int(unmarked_go(CubeForge.World.block_light(w0), full, L.relit_marks(r), 0), -1, "every changed section is marked")
      Test.assert_true(marked_count(L.relit_marks(r), 0, 0) > 0, "something was marked")
    end

    test "removing a glow cap darkens what it lit" do
      let w0 = CubeForge.World.relight_all(CubeForge.World.set_block(air_world(), 40, 100, 40, C.glow_cap()))
      Test.assert_eq_int(CubeForge.World.block_light_at(w0, 44, 100, 40), 6, "lit before removal")
      let w1 = CubeForge.World.set_block(w0, 40, 100, 40, 0)
      let inc = L.relit_field(L.relight_block_marked(CubeForge.World.block_light(w1), w1, 40, 100, 40))
      Test.assert_eq_int(first_difference(inc, L.flood_block(w1)), -1, "incremental equals full re-flood")
      Test.assert_eq_int(L.get(inc, 44, 100, 40), 0, "dark after removal")
    end

    test "walling off an emitter with stone matches a full re-flood" do
      let w0 = CubeForge.World.relight_all(CubeForge.World.set_block(air_world(), 40, 100, 40, C.glow_cap()))
      let w1 = CubeForge.World.set_block(w0, 42, 100, 40, 3)
      let inc = L.relit_field(L.relight_block_marked(CubeForge.World.block_light(w1), w1, 42, 100, 40))
      Test.assert_eq_int(first_difference(inc, L.flood_block(w1)), -1, "incremental equals full re-flood")
      Test.assert_eq_int(L.get(inc, 43, 100, 40), 5, "behind the wall, light arrives around it two steps longer")
    end

    test "an edit at the world edge matches a full re-flood" do
      let w0 = CubeForge.World.relight_all(air_world())
      let w1 = CubeForge.World.set_block(w0, 0, 80, 127, C.glow_cap())
      let inc = L.relit_field(L.relight_block_marked(CubeForge.World.block_light(w1), w1, 0, 80, 127))
      Test.assert_eq_int(first_difference(inc, L.flood_block(w1)), -1, "incremental equals full re-flood")
    end

    test "an emitter just outside the relight box still feeds it" do
      -- Two caps 20 apart: editing beside one must not lose the other's light.
      let wa = CubeForge.World.set_block(air_world(), 40, 100, 40, C.glow_cap())
      let w0 = CubeForge.World.relight_all(CubeForge.World.set_block(wa, 60, 100, 40, C.glow_cap()))
      let w1 = CubeForge.World.set_block(w0, 41, 100, 40, 3)
      let inc = L.relit_field(L.relight_block_marked(CubeForge.World.block_light(w1), w1, 41, 100, 40))
      Test.assert_eq_int(first_difference(inc, L.flood_block(w1)), -1, "incremental equals full re-flood")
      Test.assert_eq_int(L.get(inc, 55, 100, 40), 5, "the far cap still lights its own ground")
    end
  end

  describe "World.relight_marked" do
    test "repairs both fields and unions the marks" do
      let w0 = CubeForge.World.relight_all(CubeForge.World.generate(8, 1, 1))
      let w1 = CubeForge.World.set_block(w0, 40, 90, 40, C.glow_cap())
      let r = CubeForge.World.relight_marked(w1, 40, 90, 40)
      let w2 = CubeForge.World.relit_world(r)
      Test.assert_eq_int(first_difference(CubeForge.World.light(w2), L.flood(w1)), -1, "skylight equals full re-flood")
      Test.assert_eq_int(first_difference(CubeForge.World.block_light(w2), L.flood_block(w1)), -1, "block light equals full re-flood")
      Test.assert_eq_int(unmarked_go(CubeForge.World.block_light(w0), CubeForge.World.block_light(w2), CubeForge.World.relit_marks(r), 0), -1, "block-light changes are marked")
      Test.assert_eq_int(unmarked_go(CubeForge.World.light(w0), CubeForge.World.light(w2), CubeForge.World.relit_marks(r), 0), -1, "skylight changes are marked")
    end
  end
```

- [ ] **Step 2: Run to verify failure**

Run: `forge test 2>&1 | tail -20`
Expected: compile error, `relight_block_marked` unknown.

- [ ] **Step 3: Implement the block relight** — add after `fn relight_at` in `light.march`:

```march
  -- ── Incremental block-light relight ──────────────────────────────────────
  --
  -- The skylight relight, minus the sky column: block light has no source
  -- above the box, so the box is simply the relight radius in every direction.
  -- Clear it, re-seed the emitters inside it, and sweep the box one voxel wider
  -- so the unchanged ring around it feeds inward. Emission is at most 12 < 15,
  -- so an emitter outside the box cannot reach past the ring into the box's
  -- interior except through the ring, which is exactly what the sweep reads.

  -- Re-seed emitters in one column of the box. Write-only on [lb].
  pfn seed_emit_col_go(lb : NativeU8Arr, c : CubeForge.Chunk.Chunk, lx : Int, lz : Int, x : Int, z : Int, y : Int, y1 : Int) : NativeU8Arr do
    if y > y1 do lb
    else
      let e = emission(C.get_or_air(c, lx, y, lz))
      if e <= 0 do seed_emit_col_go(lb, c, lx, lz, x, z, y + 1, y1)
      else seed_emit_col_go(NativeArray.set_u8(lb, index(x, y, z), e), c, lx, lz, x, z, y + 1, y1) end
    end
  end

  pfn seed_emit_box_go(lb : NativeU8Arr, w : CubeForge.World.World, x0 : Int, bw : Int, z0 : Int, y0 : Int, y1 : Int, i : Int, n : Int) : NativeU8Arr do
    if i >= n do lb
    else
      let x = x0 + i % bw
      let z = z0 + i / bw
      let c = CubeForge.World.chunk_at(w, CubeForge.World.floor_div16(x), CubeForge.World.floor_div16(z))
      seed_emit_box_go(seed_emit_col_go(lb, c, CubeForge.World.mod16(x), CubeForge.World.mod16(z), x, z, y0, y1), w, x0, bw, z0, y0, y1, i + 1, n)
    end
  end

  doc "Repair the block-light field after the block at (ex, ey, ez) changed, reporting which sections moved. [w] is the world after the edit."
  fn relight_block_marked(lb : NativeU8Arr, w : CubeForge.World.World, ex : Int, ey : Int, ez : Int) : Relit do
    let r = relight_radius()
    let cx0 = clampi(ex - r, 0, 127)
    let cx1 = clampi(ex + r, 0, 127)
    let cz0 = clampi(ez - r, 0, 127)
    let cz1 = clampi(ez + r, 0, 127)
    let cy0 = clampi(ey - r, 0, 255)
    let cy1 = clampi(ey + r, 0, 255)
    let sy1 = clampi(ey + r + 1, 0, 255)
    let above_len = if sy1 >= 254 do volume() else (sy1 + 2) * 16384 end
    let before = copy_prefix(lb, above_len)
    let lb1 = zero_box_go(lb, cx0, cx1, cy0, cy1, cz0, cz1)
    let lb2 = seed_emit_box_go(lb1, w, cx0, cx1 - cx0 + 1, cz0, cy0, cy1, 0, (cx1 - cx0 + 1) * (cz1 - cz0 + 1))
    let sx0 = clampi(ex - r - 1, 0, 127)
    let sx1 = clampi(ex + r + 1, 0, 127)
    let sz0 = clampi(ez - r - 1, 0, 127)
    let sz1 = clampi(ez + r + 1, 0, 127)
    let sy0 = if cy0 <= 0 do 0 else clampi(cy0 - 1, 0, 255) end
    let plen = (sy1 + 2) * 16384
    let slo = sy0 * 16384
    let swept = box_sweep_go2(copy_prefix(lb2, plen), copy_prefix(lb2, plen), w, max_level(),
                              sx0, sx1, sy0, sy1, sz0, sz1, slo, plen - slo)
    let out = CubeForge.Ffi.Window.u8_blit(lb2, slo, swept, slo, plen - slo)
    Relit(out, mark_box(marks_new(), before, out, sx0, sx1, sy0, sy1, sz0, sz1))
  end

  -- The union of two marks arrays, as a fresh array: reads [a] and [b], writes
  -- only the new one, so neither input is copied (GAPS G21).
  pfn or_go(a : NativeIntArr, b : NativeIntArr, m : NativeIntArr, i : Int) : NativeIntArr do
    if i >= 1024 do m
    else if NativeArray.get_int(a, i) != 0 do or_go(a, b, NativeArray.set_int(m, i, 1), i + 1)
    else if NativeArray.get_int(b, i) != 0 do or_go(a, b, NativeArray.set_int(m, i, 1), i + 1)
    else or_go(a, b, m, i + 1) end end end
  end
  doc "Sections marked in either marks array."
  fn or_marks(a : NativeIntArr, b : NativeIntArr) : NativeIntArr do or_go(a, b, marks_new(), 0) end
```

Note `plen` when `sy1 >= 254`: `(sy1 + 2) * 16384` can exceed `volume()`. Guard it exactly as `above_len` does: `let plen = if sy1 >= 254 do volume() else (sy1 + 2) * 16384 end`. Use that form.

- [ ] **Step 4: Implement the combined world relight** — replace `relight_at` in `world.march`:

```march
  -- A repaired world together with the sections whose light (either field) changed.
  type Relit = Relit(World, NativeIntArr)
  fn relit_world(r : Relit) : World do match r do Relit(w, _) -> w end end
  fn relit_marks(r : Relit) : NativeIntArr do match r do Relit(_, m) -> m end end

  doc "Repair both light fields after an edit at a world coordinate, reporting the union of the sections either moved."
  fn relight_marked(w : World, x : Int, y : Int, z : Int) : Relit do
    let rs = CubeForge.Light.relight_marked(light(w), w, x, y, z)
    let rb = CubeForge.Light.relight_block_marked(block_light(w), w, x, y, z)
    Relit(set_block_light(set_light(w, CubeForge.Light.relit_field(rs)), CubeForge.Light.relit_field(rb)),
          CubeForge.Light.or_marks(CubeForge.Light.relit_marks(rs), CubeForge.Light.relit_marks(rb)))
  end

  doc "Repair both light fields after an edit, discarding the dirty-section marks."
  fn relight_at(w : World, x : Int, y : Int, z : Int) : World do relit_world(relight_marked(w, x, y, z)) end
```

- [ ] **Step 5: Run the tests**

Run: `forge test 2>&1 | tail -30`
Expected: pass. If "the far cap still lights its own ground" fails, the ring is not feeding: check `sx0..sx1` are one wider than `cx0..cx1` and that `box_sweep_go2` scans the wider box.

- [ ] **Step 6: Commit**

```bash
git add lib/cube_forge/light.march lib/cube_forge/world.march test/light_test.march
git commit -m "feat(light): incremental block-light relight; World.relight_marked repairs both fields"
```

---

### Task 5: Vertex shade word packs sky and block shade

**Files:**
- Modify: `lib/cube_forge/vertex.march`
- Test: `test/vertex_test.march`

**Interfaces:**
- Produces: `Vertex.pack_shade(sky : Float, blk : Float) : Float`, `Vertex.sky_of(p : Float) : Float`, `Vertex.blk_of(p : Float) : Float`, `Vertex.brightness_of(p : Float) : Float`.

Encoding: `packed = 2 * round(blk * 255) + clamp01(sky)`. Sky rides in the fractional-and-units part, block light in even integers above it. A plain shade `s` in [0, 1] (every HUD, outline, marker and precipitation vertex) decodes as sky `s`, block 0, so nothing that already pushes a shade float has to change. Linear interpolation across a triangle interpolates both parts correctly, because the encoding is linear in each and the sky part never leaves [0, 1].

- [ ] **Step 1: Write the failing tests** — append to `test/vertex_test.march` before the module's final `end`:

```march
  pfn shade_roundtrip(i : Int) : Int do
    if i > 255 do -1
    else
      let b = int_to_float(i) /. 255.0
      let p = Vx.pack_shade(0.37, b)
      if Math.abs(Vx.blk_of(p) -. b) > 0.0001 do i
      else if Math.abs(Vx.sky_of(p) -. 0.37) > 0.0001 do i
      else shade_roundtrip(i + 1) end end
    end
  end

  describe "Vertex.pack_shade" do
    test "round-trips every block-light step with the sky shade intact" do
      Test.assert_eq_int(shade_roundtrip(0), -1, "all 256 steps")
    end
    test "a plain shade float is sky only" do
      Test.assert_true(Math.abs(Vx.sky_of(1.0) -. 1.0) < 0.0001, "1.0 is full sky")
      Test.assert_true(Math.abs(Vx.blk_of(1.0)) < 0.0001, "and no block light")
      Test.assert_true(Math.abs(Vx.sky_of(0.5) -. 0.5) < 0.0001, "0.5 is half sky")
    end
    test "brightness is the sum, and sky clamps" do
      Test.assert_true(Math.abs(Vx.brightness_of(Vx.pack_shade(0.25, 0.5)) -. 0.75) < 0.01, "0.25 + 0.5")
      Test.assert_true(Math.abs(Vx.sky_of(Vx.pack_shade(1.7, 0.0)) -. 1.0) < 0.0001, "sky above 1 clamps")
    end
  end
```

- [ ] **Step 2: Run to verify failure**

Run: `forge test 2>&1 | tail -20`
Expected: compile error, `pack_shade` unknown.

- [ ] **Step 3: Implement** — add to `vertex.march` before the module's `end`:

```march
  -- ── The shade float: sky shade and block-light shade in one number ───────
  --
  --   packed = 2 * round(blk * 255) + clamp01(sky)
  --
  -- Sky lives in [0, 1]; block light sits above it in even integers, so any
  -- vertex that pushes a plain shade in [0, 1] (HUD, outline, marker, rain)
  -- still decodes as sky-only. Linear in both parts, so interpolation across
  -- a triangle is exact. The fragment shader unpacks with floor(v / 2).

  pfn clamp01(v : Float) : Float do if v < 0.0 do 0.0 else if v > 1.0 do 1.0 else v end end end

  doc "Pack a sky shade and a block-light shade, both 0..1, into the shade float."
  fn pack_shade(sky : Float, blk : Float) : Float do
    2.0 *. int_to_float(float_to_int(clamp01(blk) *. 255.0 +. 0.5)) +. clamp01(sky)
  end

  doc "The block-light shade 0..1 in a packed shade float."
  fn blk_of(p : Float) : Float do int_to_float(float_to_int(p /. 2.0)) /. 255.0 end

  doc "The sky shade 0..1 in a packed shade float."
  fn sky_of(p : Float) : Float do p -. 2.0 *. int_to_float(float_to_int(p /. 2.0)) end

  doc "Sky plus block shade: the brightness the quad-flip heuristic compares."
  fn brightness_of(p : Float) : Float do sky_of(p) +. blk_of(p) end
```

`float_to_int` truncates toward zero; `p / 2` is non-negative, so truncation is floor. If `float_to_int` rounds instead, `sky_of(1.0)` would come out as -1: the test "1.0 is full sky" catches that, and the fix is `Math.floor(p /. 2.0)` in both places.

- [ ] **Step 4: Run the tests**

Run: `forge test 2>&1 | tail -20`
Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add lib/cube_forge/vertex.march test/vertex_test.march
git commit -m "feat(vertex): shade float packs sky and block-light shade"
```

---

### Task 6: Mesher reads block light per corner

**Files:**
- Modify: `lib/cube_forge/mesher.march:31-50,181-197,337-360,437-460,498-536`
- Modify: `lib/cube_forge/chunk_mesh.march:44,46,84,86`
- Modify: `test/greedy_test.march:38,45,52`, `test/light_test.march` (corner packing tests)

**Interfaces:**
- Consumes: `Vertex.pack_shade`, `Vertex.brightness_of`, `World.block_light`.
- Produces: `Mesher.pack_corner(light, ao, blk) : Int` (10 bits: light + 16*ao + 64*blk), `Mesher.corner_sky(packed) : Float`, `Mesher.corner_blk(packed) : Float`, `Mesher.key_of(id, c0, c1, c2, c3)` with 10-bit corners, `Mesher.mesh_section_opaque(c, n, s, e, w, la, lb, occ, sy, ox, oz)`, `Mesher.mesh_section_foliage(...)` likewise — **`lb` inserted after `la`**. `corner_shade` is removed.

- [ ] **Step 1: Update the tests first.** In `test/light_test.march` replace the "Mesher corner packing" describe block (the `pack_corner`, `key_of` and `corner_shade` tests) with:

```march
  describe "Mesher corner packing" do
    test "light, AO and block light round-trip through the ten-bit corner" do
      Test.assert_eq_int(CubeForge.Mesher.pack_corner(15, 3, 0), 63, "brightest sky, unoccluded, no glow")
      Test.assert_eq_int(CubeForge.Mesher.pack_corner(0, 0, 0), 0, "darkest, fully occluded")
      Test.assert_eq_int(CubeForge.Mesher.pack_corner(11, 2, 0) % 16, 11, "light in the low nibble")
      Test.assert_eq_int(CubeForge.Mesher.pack_corner(11, 2, 0) / 16, 2, "AO above it")
      Test.assert_eq_int(CubeForge.Mesher.pack_corner(11, 2, 9) / 64, 9, "block light above that")
      Test.assert_eq_int(CubeForge.Mesher.pack_corner(15, 3, 15), 1023, "everything full is ten set bits")
    end

    test "the key round-trips a block id and four ten-bit corners" do
      let k = CubeForge.Mesher.key_of(3, 1023, 0, 17, 640)
      Test.assert_eq_int(CubeForge.Mesher.key_id(k), 3, "id")
      Test.assert_eq_int(CubeForge.Mesher.key_c0(k), 1023, "c0")
      Test.assert_eq_int(CubeForge.Mesher.key_c1(k), 0, "c1")
      Test.assert_eq_int(CubeForge.Mesher.key_c2(k), 17, "c2")
      Test.assert_eq_int(CubeForge.Mesher.key_c3(k), 640, "c3")
    end

    test "corner shades split sky from block light, both scaled by AO" do
      Test.assert_true(near(CubeForge.Mesher.corner_sky(CubeForge.Mesher.pack_corner(15, 3, 0)), 1.0), "full sky, open corner")
      Test.assert_true(near(CubeForge.Mesher.corner_sky(CubeForge.Mesher.pack_corner(0, 3, 10)), 0.0), "dark sky stays dark sky")
      Test.assert_true(near(CubeForge.Mesher.corner_blk(CubeForge.Mesher.pack_corner(0, 3, 15)), 1.0), "full glow, open corner")
      Test.assert_true(near(CubeForge.Mesher.corner_blk(CubeForge.Mesher.pack_corner(0, 0, 15)), 0.55), "full glow, closed corner")
      Test.assert_true(near(CubeForge.Mesher.corner_blk(CubeForge.Mesher.pack_corner(15, 3, 0)), 0.0), "no glow")
    end
  end
```

Keep any other tests that were in that block (the `should_flip` test, if present) as they are. In `test/greedy_test.march` lines 38, 45 and 52 insert a third `CubeForge.Light.new()` so the calls read `Mesher.mesh_section_opaque(<chunk>, e, e, e, e, CubeForge.Light.new(), CubeForge.Light.new(), CubeForge.Light.new(), <sy>, 0.0, 0.0)`.

Then add to `test/greedy_test.march` (uses that file's existing `slab_go` and `e`):

```march
  describe "block light breaks merges" do
    test "a glow beside a slab splits the merged top face" do
      let c = slab_go(C.new(), 0)
      let e = C.new()
      let dark = Mesher.mesh_section_opaque(c, e, e, e, e, CubeForge.Light.new(), CubeForge.Light.new(), CubeForge.Light.new(), 0, 0.0, 0.0)
      let lb = NativeArray.set_u8(CubeForge.Light.new(), CubeForge.Light.index(5, 1, 5), 9)
      let lit = Mesher.mesh_section_opaque(c, e, e, e, e, CubeForge.Light.new(), lb, CubeForge.Light.new(), 0, 0.0, 0.0)
      Test.assert_true(Mesher.vertex_count(lit) > Mesher.vertex_count(dark), "a lit corner cannot merge with dark ones")
    end
  end
```

Check how `slab_go` builds its slab (which y) and place the glow one voxel above the slab's top face at (5, top+1, 5); adjust `L.index(5, 1, 5)` to that y.

- [ ] **Step 2: Run to verify failure**

Run: `forge test 2>&1 | tail -20`
Expected: compile errors (arity of `pack_corner`, unknown `corner_sky`).

- [ ] **Step 3: Implement the corner and key changes** in `mesher.march`:

```march
  doc "Pack one corner into ten bits: skylight 0..15 in the low nibble, AO 0..3 above it, block light 0..15 above that."
  fn pack_corner(light : Int, ao : Int, blk : Int) : Int do light + 16 * ao + 64 * blk end

  pfn ao_factor(packed : Int) : Float do 0.55 +. 0.15 *. int_to_float((packed / 16) % 4) end

  doc "Sky shade for a packed corner: skylight scaled by ambient occlusion."
  fn corner_sky(packed : Int) : Float do (int_to_float(packed % 16) /. 15.0) *. ao_factor(packed) end

  doc "Block-light shade for a packed corner: block light scaled by the same ambient occlusion."
  fn corner_blk(packed : Int) : Float do (int_to_float(packed / 64) /. 15.0) *. ao_factor(packed) end

  doc "Split the quad along c1-c3 instead of c0-c2 when the default diagonal is the darker pair; without this a lone dark corner smears across the whole quad. Compares packed shade words by total brightness."
  fn should_flip(s0 : Float, s1 : Float, s2 : Float, s3 : Float) : Bool do
    Vx.brightness_of(s0) +. Vx.brightness_of(s2) < Vx.brightness_of(s1) +. Vx.brightness_of(s3)
  end

  -- Mask key: block id in the low 8 bits, then four 10-bit corners in mask (u, v)
  -- order. Two faces merge only when block AND all four corners agree, so
  -- merging survives exactly across uniformly-lit regions. 48 bits in all.
  fn key_of(id : Int, c0 : Int, c1 : Int, c2 : Int, c3 : Int) : Int do
    id + 256 * c0 + 262144 * c1 + 268435456 * c2 + 274877906944 * c3
  end
  fn key_id(k : Int) : Int do k % 256 end
  fn key_c0(k : Int) : Int do (k / 256) % 1024 end
  fn key_c1(k : Int) : Int do (k / 262144) % 1024 end
  fn key_c2(k : Int) : Int do (k / 268435456) % 1024 end
  fn key_c3(k : Int) : Int do (k / 274877906944) % 1024 end
```

Delete `corner_shade`. Check whether anything else in `mesher.march` or `cube_forge.march` calls `corner_shade` or `should_flip` with raw floats (`grep -rn "corner_shade\|should_flip" lib`) and update each: `should_flip` now expects packed shade words, which `quad_sized` receives from `emit_rect` below. The non-greedy `quad` path pushes shade `1.0`, which is a valid packed word (sky 1, block 0).

- [ ] **Step 4: Thread `lb` and read it in `corner_pack`.** Change `corner_pack` to take `lb` after `la` and average the block light the same way:

```march
  pfn corner_pack(la : NativeU8Arr, lb : NativeU8Arr, occ : NativeU8Arr, oxi : Int, ozi : Int, nx : Int, ny : Int, nz : Int, aux : Int, auz : Int, avy : Int, avz : Int) : Int do
    let s1x = nx + aux
    let s1z = nz + auz
    let s2y = ny + avy
    let s2z = nz + avz
    let ccx = nx + aux
    let ccy = ny + avy
    let ccz = nz + auz + avz
    let o1 = occluded(occ, oxi, ozi, s1x, ny, s1z)
    let o2 = occluded(occ, oxi, ozi, nx, s2y, s2z)
    let oc = occluded(occ, oxi, ozi, ccx, ccy, ccz)
    let ln = CubeForge.Light.get(la, oxi + nx, ny, ozi + nz)
    let l1 = if o1 do 0 else CubeForge.Light.get(la, oxi + s1x, ny, ozi + s1z) end
    let l2 = if o2 do 0 else CubeForge.Light.get(la, oxi + nx, s2y, ozi + s2z) end
    let lc = if oc do 0 else CubeForge.Light.get(la, oxi + ccx, ccy, ozi + ccz) end
    let bn = CubeForge.Light.get(lb, oxi + nx, ny, ozi + nz)
    let b1 = if o1 do 0 else CubeForge.Light.get(lb, oxi + s1x, ny, ozi + s1z) end
    let b2 = if o2 do 0 else CubeForge.Light.get(lb, oxi + nx, s2y, ozi + s2z) end
    let bc = if oc do 0 else CubeForge.Light.get(lb, oxi + ccx, ccy, ozi + ccz) end
    let cnt = 1 + (if o1 do 0 else 1 end) + (if o2 do 0 else 1 end) + (if oc do 0 else 1 end)
    pack_corner((ln + l1 + l2 + lc) / cnt, corner_ao(o1, o2, oc), (bn + b1 + b2 + bc) / cnt)
  end
```

Then add `lb : NativeU8Arr` immediately after `la : NativeU8Arr` in the signatures of `face_key`, `fill_mask`, `slices_go`, `dirs_go`, `mesh_section_cat`, `mesh_section_opaque`, `mesh_section_foliage`, and pass it through at every call between them (the four `corner_pack` calls in `face_key`, the three `face_key` calls in `fill_mask`, and each recursive/forwarding call). Use `grep -n "la, occ" lib/cube_forge/mesher.march` to find every site; each becomes `la, lb, occ`.

- [ ] **Step 5: Pack the shade at emit time.** In `emit_rect` replace the four `corner_shade` lines with:

```march
    let s0 = Vx.pack_shade(corner_sky(key_c0(key)), corner_blk(key_c0(key)))
    let s1 = Vx.pack_shade(corner_sky(key_c1(key)), corner_blk(key_c1(key)))
    let s2 = Vx.pack_shade(corner_sky(key_c2(key)), corner_blk(key_c2(key)))
    let s3 = Vx.pack_shade(corner_sky(key_c3(key)), corner_blk(key_c3(key)))
```

`quad_sized` is unchanged: it already pushes the `s` values and calls `should_flip` on them.

- [ ] **Step 6: Update `chunk_mesh.march`** — all four `Mesher.mesh_section_opaque(...)` / `mesh_section_foliage(...)` calls at lines 44, 46, 84 and 86 gain `World.block_light(world)` immediately after `World.light(world)`.

- [ ] **Step 7: Build and run the tests**

Run: `forge build 2>&1 | tail -20 && forge test 2>&1 | tail -30`
Expected: build clean (`forge check` misses cross-module arity errors, GAPS G66, so build first), tests pass.

- [ ] **Step 8: Commit**

```bash
git add lib/cube_forge/mesher.march lib/cube_forge/chunk_mesh.march test/greedy_test.march test/light_test.march
git commit -m "feat(mesher): ten-bit corners carry block light; shade word packs sky and glow"
```

---

### Task 7: Fragment shader adds the glow term

**Files:**
- Modify: `native/cf_shim.c:196-200` (vertex shader comment), `native/cf_shim.c:404-440` (fragment `main`)

**Interfaces:**
- Consumes: the packed shade word from Task 5. Decode: `bl = floor(v_shade * 0.5) / 255.0`, `sk = v_shade - 2.0 * floor(v_shade * 0.5)`.

- [ ] **Step 1: Edit the fragment shader.** At the top of `main()` in `FS`, right after `int fxw = ...` / `int fe = ...`, add:

```c
    /* The shade float packs two channels (see Vertex.pack_shade): sky shade in
     * [0, 1], and block-light shade in even integers above it. Overlays push a
     * plain shade in [0, 1], which decodes as sky-only and leaves them alone. */
    "  float sk = v_shade - 2.0 * floor(v_shade * 0.5);\n"
    "  float bl = floor(v_shade * 0.5) / 255.0;\n"
```

Then replace every later use of `v_shade` in `FS`:

- `if (v_shade > 0.001 && lit_matters)` → `if (sk > 0.001 && lit_matters)`
- `vec3  baked = v_shade * sky;` → `vec3  baked = sk * sky;`
- `vec3  world = baked + vec3(flash);` → 
  ```c
    /* Block light is its own source: independent of the sun, so it is what
     * you see at midnight. max, not +, so a glowing patch at noon is not
     * brighter than the noon around it. GLOW is the warm tint of fungus. */
    "  vec3  world = max(baked, bl * GLOW) + vec3(flash);\n"
  ```
- `(u_unlit == 1) ? vec3(v_shade) : world` → `(u_unlit == 1) ? vec3(sk) : world`

Add the constant beside `MOON_TINT`:

```c
    "const vec3  GLOW = vec3(1.00, 0.90, 0.70);\n"
```

Update the vertex-shader comment at line 196 to say the shade is the packed sky/block word, passed through untouched.

- [ ] **Step 2: Build and run the game for a smoke check**

Run: `forge build 2>&1 | tail -5 && CF_NOMOUSE=1 CF_SUN=60 CF_MAX_FRAMES=90 ./.march/build/debug/cube_forge 2>&1 | tail -5`

(Check `lib/cube_forge.march` for the exact max-frames knob name in the `Knobs` constructor at line 1782 — `max_frames` is its first field; use whatever env var feeds it.)

Expected: runs to completion, no `cf: shader:` compile log on stderr. Day lighting must look unchanged — a shade of 1.0 decodes as sky 1.0, block 0.

- [ ] **Step 3: Commit**

```bash
git add native/cf_shim.c
git commit -m "feat(shader): unpack the shade word and add block light as its own source"
```

---

### Task 8: Edit paths and startup use both fields; `CF_AUTOGLOW`

**Files:**
- Modify: `lib/cube_forge.march:620-627` (edit_block), `:662-669` (veg relight), `:875-878` (World.relight_at site — already both after Task 4), `:1154-1160` (Knobs), `:1782` (Knobs constructor), `:1250-1300` (scripted edits), `:1816-1819` (startup flood)

**Interfaces:**
- Consumes: `World.relight_marked`, `World.relit_world`, `World.relit_marks`, `Chunk.glow_cap`.
- Produces: `CF_AUTOGLOW=<frame>`: at that frame, place a glow cap on the surface three columns east of the player through `edit_block`.

- [ ] **Step 1: `edit_block` repairs both fields.** Replace lines 624-626:

```march
        let relit = World.relight_marked(w0, x, y, z)
        let world1 = World.relit_world(relit)
        let marks = mark_geometry(World.relit_marks(relit), x, y, z, n)
```

- [ ] **Step 2: The vegetation relight repairs both fields.** Replace lines 666-669 to use the same API:

```march
        let relit = World.relight_marked(w1, x, base + (if bush do 1 else 4 end), z)
        let w2 = World.relit_world(relit)
        ...
        let marks1 = or_marks(mark_box(marks, x - 4, base, z - 4, x + 4, base + trunk + 2, z + 4, n, 0), World.relit_marks(relit), 0)
```

Keep the line between (`w2` is threaded exactly as before; only its source changes).

- [ ] **Step 3: Startup floods block light.** `World.relight_all` already floods both after Task 3, so the startup at line 1817 is correct. Change the printed label to `"  light flood (sky + block): "` so the log says what it timed.

- [ ] **Step 4: Add the knob.** In the `Knobs` type (line 1154) append a final `Int` field with the comment `autoglow frame (<0 = off): place a glow cap three columns east of the player, through edit_block, for the block-light end-to-end check`. In the match at line 1160 append `, autoglow` to the pattern. In the constructor at line 1782 append `, Env.get_int("CF_AUTOGLOW", -1)`. Then beside the `CF_AUTOCANAL` line (~1300):

```march
          -- CF_AUTOGLOW=<frame>: a glow cap on the surface three columns east
          -- of the player, through edit_block so both light fields are repaired
          -- and the affected sections remeshed, the way a real placement would.
          let sc0e = if autoglow >= 0 && frame == autoglow do place_glow(sc0d, player1) else sc0d end
```

and thread `sc0e` into whatever consumed `sc0d`. Add the helper next to `break_under`:

```march
  -- A glow cap on the surface three columns east of the player.
  pfn place_glow(sc, p : CubeForge.Player.Player) do
    let pos = Player.position(p)
    let px = float_to_int(Math.floor(V.x(pos))) + 3
    let pz = float_to_int(Math.floor(V.z(pos)))
    let y = World.surface_y(scene_world(sc), px, pz)
    if y < 0 || y >= 254 do sc else edit_block(sc, px, y + 1, pz, Chunk.glow_cap()) end
  end
```

Read `World.surface_y` to confirm it returns the top solid block's y (then `y + 1` is the air above it); if it returns the air above, drop the `+ 1`.

- [ ] **Step 5: Build, lint, test**

Run: `forge build 2>&1 | tail -5 && forge lint --strict 2>&1 | tail -5 && forge test 2>&1 | tail -10`
Expected: all clean.

- [ ] **Step 6: Night frame dump with and without the glow.** Both runs pinned: `CF_NOMOUSE=1 CF_SUN=180 CF_TIME=0 CF_WEATHER=0`. Sun at 180 degrees is straight down, so sunlight intensity is 0 and the moon is up. Use the max-frames and dump env vars the `Knobs` constructor reads (`CF_DUMP=<path>`, `CF_DUMP_FRAME=<n>`).

```bash
CF_NOMOUSE=1 CF_SUN=180 CF_TIME=0 CF_WEATHER=0 CF_DUMP=docs/glow-off.bmp CF_DUMP_FRAME=200 ./.march/build/debug/cube_forge 2>&1 | grep -i "shader\|state:"
```

```bash
CF_NOMOUSE=1 CF_SUN=180 CF_TIME=0 CF_WEATHER=0 CF_AUTOGLOW=100 CF_DUMP=docs/glow-on.bmp CF_DUMP_FRAME=200 ./.march/build/debug/cube_forge 2>&1 | grep -i "shader\|state:\|placed"
```

Then `python3 scratch/cmpframe.py docs/glow-off.bmp docs/glow-on.bmp` and expect a non-zero pixel difference concentrated near the placed cap. Open `docs/glow-on.bmp` and confirm a warm pool of light on the ground and on any nearby wall. Convert the kept dump to PNG as `docs/lighting-glow.png` (the way the other `docs/*.png` were made — `sips -s format png` on macOS) and delete the BMPs.

- [ ] **Step 7: Frame budget and startup cost**

Run: `scratch/frame_budget.sh`
Expected: passes at 12 ms. Note the `light flood (sky + block)` startup line from a normal run and record both in `RESULTS.md` under a new heading `### Block light (fungus phase 1)`: the flood cost before and after (the old skylight-only number is in the same file), the relight cost of a glow placement from the `placed block ... edit + section remesh` line, and the frame-budget result.

- [ ] **Step 8: Update the spec's as-built notes and the backlog.** In `docs/superpowers/specs/2026-09-04-fungus-design.md` §6 add an *As built* paragraph: the field is a world-flat array on `World`, not a third array in `Chunk`; propagation is the level-synchronous sweep (GAPS G63), not a BFS; the vertex packing is `2 * round(blk * 255) + sky` in the shade float rather than two nibbles, so overlay vertices needed no change. In `todos.md` add an entry under Engine: `- [x] **Block light channel** — done 2026-09-05 (fungus phase 1; plan docs/superpowers/plans/2026-09-05-fungus-phase1-block-light.md)`.

- [ ] **Step 9: Commit**

```bash
git add lib/cube_forge.march RESULTS.md todos.md docs/superpowers/specs/2026-09-04-fungus-design.md docs/lighting-glow.png
git commit -m "feat(light): edits repair both light fields; CF_AUTOGLOW; night dump and budget recorded"
```

---

## Self-review

- **Spec coverage (§6):** second byte per voxel (Task 1), flood seeded from emission with the add/remove pair as the bounded sweep (Tasks 3, 4), emission table for the glow cap at 10 (Task 2; the mycelium 4, small fruit 6 and giant 12 rows are phases 3 and 5, where those blocks exist), shared edit hook (Task 8), packed shade float with the layout at nine floats (Tasks 5, 6), shader `max(sky * daylight, block)` (Task 7), glow not in the shadow trace (nothing added to the DDA), verification with a temporary emissive block and a night frame dump (Tasks 2, 8), budget measured (Task 8).
- **Deviation from spec noted:** the packing is not "two nibbles"; Task 8 step 8 records why.
- **Type consistency:** `pack_corner(light, ao, blk)` everywhere; `mesh_section_*` take `la, lb, occ` in that order in Task 6 and its callers; `World.Relit` accessors are `relit_world` / `relit_marks`, distinct from `Light.relit_field` / `Light.relit_marks`.
- **Key width:** 8 + 4×10 = 48 bits; `274877906944 * 1023 + ...` is under 2^63.
