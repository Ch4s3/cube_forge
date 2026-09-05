# Fungus phases 5 and 6: fruit — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Mature mycelium fruits mushrooms of three tiers as cube structures: a one-block cutout small mushroom, a medium body with a stem and a 3x3 cap, and a walkable giant with a tall stem and a wide disc or dome cap. Bodies grow on a budget, decay when the ground under them loses the species, and fell when their small block or any stem block is broken, yielding spores and their stem and cap blocks and resetting the column's reach.

**Architecture:** Fruit blocks encode the **species** in the id (18 ids, 45..62: small, stem, cap x six), so a placed cap glows and textures on its own, off the network. A new `CubeForge.Fruit` module owns the cell grid per tier, a pure body function `block_of_body`, stamping and felling (the `Veg.plant`/`fell` pattern: only into air, remove only what the shape owns), a candidate scan over a rolling window, and the growth roll. Growth runs on the vegetation slot on the periods vegetation skips, one body per slot, relit through `World.relight_marked` like a tree. Small mushrooms join the cutout mesh pass through a new `Chunk.is_cutout` predicate, leaving `is_foliage`'s leaf semantics alone.

**Tech Stack:** March; no C changes.

Spec: `docs/superpowers/specs/2026-09-04-fungus-design.md` §5, §6 (fruit rows), §4 (fruit blocks), §9 steps 5-6.

## Global Constraints

- All prior-phase constraints (GAPS G21/G33/G56/G63/G65/G68/G69).
- A body is a pure function of (species, its canonical column, the seed): its stem height, cap radius and dome are hashed from those, never from the world, so what grows is what fells.
- Stamping writes only into air and never into another body's blocks; felling removes only blocks whose id the shape predicts at that position.
- Frame budget: `scratch/frame_budget.sh` (12 ms) after phase 5 and again after phase 6. Phase 4 left ~2.7 ms of headroom on the worst run; fruit growth must land on a frame no other phase uses.
- Commit after every task. No attribution lines.

## File structure

| file | responsibility |
|---|---|
| `lib/cube_forge/chunk.march` | fruit ids, predicates, `is_cutout`, small fruit not collidable. |
| `lib/cube_forge/light.march` | `emission` for fruit; `opacity` of cutout blocks. |
| `lib/cube_forge/mesher.march` | cutout category by `is_cutout`. |
| `lib/cube_forge/texture.march` | 18 fruit layers (66..83); `layers()` = 84. |
| `lib/cube_forge/inventory.march` | small fruit yields nothing on its own (felling yields). |
| `lib/cube_forge/fruit.march` | **New.** Cells, body shapes, stamp, fell, scan, roll. |
| `lib/cube_forge/myc.march` | `reseed`, `fruit_vigour`. |
| `lib/cube_forge.march` | fruit slot, `fruit_go`, harvest on break, knobs, dump counts. |
| tests | `fruit_test.march` (new), `myc_block_test`, `light_test` layer count. |

## Constants (in `Fruit`)

| name | value |
|---|---|
| `cell_size(tier)` | 3 / 6 / 16 |
| `Myc.fruit_vigour()` | 0.8 |
| `rate(tier)` per visit of a cell (default) | 0.05 / 0.02 / 0.005; `CF_FRUIT_RATE` percent scales |
| medium stem | 2 + hash(0..1) → 2..3 |
| giant stem | 5 + hash(0..4) → 5..9 |
| giant radius | 3 + hash(0..1) → 3..4; dome when species is Marshlight (5): a second cap layer of radius - 2 |
| footprint | 9x9 columns around the canonical column, up to stem + 3 high |

---

### Task 1: Fruit blocks: ids, emission, cutout, textures

**Files:**
- Modify: `lib/cube_forge/chunk.march`, `lib/cube_forge/light.march`, `lib/cube_forge/mesher.march`, `lib/cube_forge/texture.march`, `lib/cube_forge/inventory.march`
- Modify: `test/myc_block_test.march`, `test/light_test.march`

**Interfaces:**
- Produces: `Chunk.fruit_small(sp)` = 44 + sp, `Chunk.fruit_stem(sp)` = 50 + sp, `Chunk.fruit_cap(sp)` = 56 + sp; `is_fruit_small/is_fruit_stem/is_fruit_cap/is_fruit(id)`; `fruit_species(id)` (0 when not fruit); `Chunk.is_cutout(id)` = foliage or small fruit; `is_collidable` false for small fruit. `Light.emission`: small = 3 × glow(sp), cap = 5 × glow(sp), stem 0 (bright: small 6, cap 10; dim: 3 and 5); `Light.opacity(cutout)` = 6. `Mesher.category_of/see_through/face_visible` use `is_cutout`. `Texture.fruit_layer(kind, sp)` with kind 0 small, 1 stem, 2 cap = `66 + 6 * kind + (sp - 1)`; `layer_for` maps fruit ids; `layers()` = 84. `Inventory.yield_of(small fruit)` = 0 (felling gives the spores).

- [ ] **Step 1: Failing tests.** `test/myc_block_test.march`:

```march
  describe "Fruit blocks" do
    test "ids encode kind and species in 45..62" do
      Test.assert_eq_int(Ch.fruit_small(1), 45, "small Frostcap")
      Test.assert_eq_int(Ch.fruit_stem(1), 51, "stem Frostcap")
      Test.assert_eq_int(Ch.fruit_cap(6), 62, "cap Sunshelf is the last")
      Test.assert_true(Ch.is_fruit_small(48), "48 is small")
      Test.assert_true(Ch.is_fruit_stem(54), "54 is a stem")
      Test.assert_true(Ch.is_fruit_cap(60), "60 is a cap")
      Test.assert_true(Ch.is_fruit(60), "and fruit")
      Test.assert_false(Ch.is_fruit(44), "44 is not")
      Test.assert_false(Ch.is_fruit(63), "63 is not")
      Test.assert_eq_int(Ch.fruit_species(Ch.fruit_cap(4)), 4, "species of a cap")
      Test.assert_eq_int(Ch.fruit_species(3), 0, "stone has none")
    end
    test "small fruit is a cutout you walk through; stems and caps are solid" do
      Test.assert_true(Ch.is_cutout(Ch.fruit_small(3)), "small is cutout")
      Test.assert_true(Ch.is_cutout(Ch.oak_leaves()), "leaves still are")
      Test.assert_false(Ch.is_cutout(Ch.fruit_cap(3)), "a cap is not")
      Test.assert_false(Ch.is_collidable(Ch.fruit_small(3)), "walk through a small mushroom")
      Test.assert_true(Ch.is_collidable(Ch.fruit_cap(3)), "stand on a cap")
      Test.assert_false(Ch.is_foliage(Ch.fruit_small(3)), "not a leaf: leaf decay leaves it alone")
      Test.assert_eq_int(Lt.opacity(Ch.fruit_small(3)), 6, "dappled like leaves")
      Test.assert_true(Lt.is_opaque(Ch.fruit_cap(3)), "a cap blocks light")
    end
    test "emission follows the species glow and the kind" do
      Test.assert_eq_int(Lt.emission(Ch.fruit_small(4)), 6, "bright small")
      Test.assert_eq_int(Lt.emission(Ch.fruit_cap(4)), 10, "bright cap")
      Test.assert_eq_int(Lt.emission(Ch.fruit_stem(4)), 0, "stems never glow")
      Test.assert_eq_int(Lt.emission(Ch.fruit_small(1)), 3, "dim small")
      Test.assert_eq_int(Lt.emission(Ch.fruit_cap(1)), 5, "dim cap")
      Test.assert_eq_int(Lt.emission(Ch.fruit_cap(3)), 0, "no glow")
    end
    test "one texture layer per kind and species, and small fruit yields nothing" do
      Test.assert_eq_int(Tx.layers(), 84, "66 + 18")
      Test.assert_true(near(Tx.fruit_layer(0, 1), 66.0), "small Frostcap")
      Test.assert_true(near(Tx.fruit_layer(2, 6), 83.0), "cap Sunshelf is the last")
      Test.assert_true(near(Tx.layer_for(Ch.fruit_stem(2)), 73.0), "stem Pinewart")
      Test.assert_eq_int(CubeForge.Inventory.yield_of(Ch.fruit_small(2)), 0, "the fell gives the spores")
      Test.assert_eq_int(CubeForge.Inventory.yield_of(Ch.fruit_cap(2)), Ch.fruit_cap(2), "a cap is a block")
      let a = Tx.checkerboard()
      Test.assert_eq_int(NativeArray.get_u8(a, float_to_int(Tx.fruit_layer(0, 3)) * 1024 + 3), 0, "a small mushroom's corner texel is transparent")
      Test.assert_eq_int(NativeArray.get_u8(a, float_to_int(Tx.fruit_layer(0, 3)) * 1024 + (8 + 16 * 12) * 4 + 3), 255, "its stem texel is opaque")
    end
  end
```

Update the two `layers()` expectations (`light_test` 66 → 84, `myc_block_test` 66 → 84).

- [ ] **Step 2: Run to verify failure.**

- [ ] **Step 3: Implement.** `chunk.march`, after the mycelium block:

```march
  -- ── Fruit: mushroom bodies ───────────────────────────────────────────────
  -- Ids 45..62 encode kind x species: small 44 + sp, stem 50 + sp, cap 56 + sp.
  -- The species IS in the id here, unlike mycelium: a cap is a block the
  -- player carries away and places, and off the network it still has to glow
  -- and look like what it is.
  fn fruit_small(sp : Int) : Int do 44 + sp end
  fn fruit_stem(sp : Int) : Int do 50 + sp end
  fn fruit_cap(sp : Int) : Int do 56 + sp end
  fn is_fruit_small(id : Int) : Bool do id >= 45 && id <= 50 end
  fn is_fruit_stem(id : Int) : Bool do id >= 51 && id <= 56 end
  fn is_fruit_cap(id : Int) : Bool do id >= 57 && id <= 62 end
  fn is_fruit(id : Int) : Bool do id >= 45 && id <= 62 end
  doc "The species of a fruit block, or 0."
  fn fruit_species(id : Int) : Int do if is_fruit(id) do (id - 45) % 6 + 1 else 0 end end
  doc "Alpha-cutout blocks: leaves and small mushrooms. Meshed in the cutout pass and see-through for face culling and light."
  fn is_cutout(id : Int) : Bool do is_foliage(id) || is_fruit_small(id) end
```

`is_collidable` becomes `id != 0 && !is_water(id) && !is_fruit_small(id)`. `light.march`: `opacity` uses `C.is_cutout(id)` where it used `is_foliage`; `emission` gains, before the `else 0`:

```march
    else if C.is_fruit_small(id) do 3 * CubeForge.Species.glow(C.fruit_species(id))
    else if C.is_fruit_cap(id) do 5 * CubeForge.Species.glow(C.fruit_species(id))
```

(`light.march` may need `alias CubeForge.Species as S`; check the module can be referenced — `Species` imports nothing.) `mesher.march`: `see_through`, `category_of` and `face_visible` use `C.is_cutout` in place of `C.is_foliage`. `inventory.march` `yield_of`: `else if C.is_fruit_small(id) do 0` beside the leaves line. `texture.march`: `layers()` 84, allocation 84, chain `fruit_all_go(spore_all_go(...), 0)`, `layer_for` gains `else if CubeForge.Chunk.is_fruit(id) do fruit_layer(fruit_kind(id), CubeForge.Chunk.fruit_species(id))` (with `pfn fruit_kind(id) = if is_fruit_small 0 else if is_fruit_stem 1 else 2`), and:

```march
  -- ── Fruit layers: 66 + 6 * kind + (species - 1); kind 0 small, 1 stem, 2 cap ──
  fn fruit_layer(kind : Int, sp : Int) : Float do int_to_float(66 + 6 * kind + (sp - 1)) end

  -- Small: a mushroom silhouette on a transparent ground -- a domed cap in the
  -- species colour over a pale stem. Stem: pale, vertically streaked, faintly
  -- tinted. Cap: the species colour, speckled, darker at the rim.
  pfn fruit_go(a : NativeU8Arr, kind : Int, sp : Int, i : Int) : NativeU8Arr do
    if i >= 256 do a
    else
      let x = i % 16
      let y = i / 16
      let sr = CubeForge.Species.colour_r(sp)
      let sg = CubeForge.Species.colour_g(sp)
      let sb = CubeForge.Species.colour_b(sp)
      let d = ((x * 7 + y * 13 + x * y) % 5) * 6 - 12
      let layer = 66 + 6 * kind + (sp - 1)
      if kind == 0 do
        -- cap: rows 2..7, a dome |x - 8| <= 6 - (7 - y); stem: rows 8..15, columns 6..9
        let cx = if x >= 8 do x - 8 else 8 - x end
        let in_cap = y >= 2 && y <= 7 && cx <= y - 1
        let in_stem = y >= 8 && x >= 6 && x <= 9
        if in_cap do fruit_go(put_rgba(a, layer * 1024 + i * 4, sr + d, sg + d, sb + d, 255), kind, sp, i + 1)
        else if in_stem do fruit_go(put_rgba(a, layer * 1024 + i * 4, 225 + d / 2, 215 + d / 2, 195 + d / 2, 255), kind, sp, i + 1)
        else fruit_go(put_rgba(a, layer * 1024 + i * 4, 0, 0, 0, 0), kind, sp, i + 1) end end
      else if kind == 1 do
        let s = (x * 7 + (y / 3) * 3) % 24 - 12
        fruit_go(put_rgba(a, layer * 1024 + i * 4, (5 * 220 + sr) / 6 + s, (5 * 210 + sg) / 6 + s, (5 * 190 + sb) / 6 + s, 255), kind, sp, i + 1)
      else
        let rim = if x == 0 || x == 15 || y == 0 || y == 15 do -30 else 0 end
        fruit_go(put_rgba(a, layer * 1024 + i * 4, sr + d + rim, sg + d + rim, sb + d + rim, 255), kind, sp, i + 1)
      end
    end
  end
  pfn fruit_all_go(a : NativeU8Arr, j : Int) : NativeU8Arr do
    if j >= 18 do a else fruit_all_go(fruit_go(a, j / 6, j % 6 + 1, 0), j + 1) end
  end
```

The small texture's corner texel (0, 0) must be transparent and its stem texel (8, 12) opaque, which the test checks.

- [ ] **Step 4: Test**, **Step 5: commit** `feat(fungus): fruit block ids, emission, cutout small mushrooms, 18 layers`.

---

### Task 2: `Fruit`: cells, bodies, stamp, fell

**Files:**
- Create: `lib/cube_forge/fruit.march`
- Create: `test/fruit_test.march`
- Modify: `lib/cube_forge/myc.march` (`fruit_vigour`, `reseed`)

**Interfaces:**
- Produces: `Fruit.cell_size(tier) : Int`; `Fruit.canonical(cx, cz, tier, seed) : Int` (column packed `x + 4096 * z`, the one column of the cell a body may stand on); `Fruit.is_canonical(x, z, tier, seed) : Bool`; `Fruit.stem_height(x, z, tier, seed) : Int` (0 small, 2..3 medium, 5..9 giant); `Fruit.radius(x, z, tier, seed) : Int` (0 / 1 / 3..4); `Fruit.dome(sp) : Bool` (Marshlight); `Fruit.block_of_body(sp, bx, base, bz, seed, x, y, z) : Int` — the block id the body rooted at (bx, base, bz) puts at (x, y, z), or 0; `Fruit.footprint() : Int` = 4 (columns either side) and `Fruit.height_of(sp, bx, bz, seed) : Int` (stem + 3); `Fruit.base_of(w, x, z, h) : Int` (ground under a fruit column: from the heightmap's `h` down past fruit blocks); `Fruit.has_body(w, x, z, base) : Bool` (block at base + 1 is a small or stem block); `Fruit.stamp(w, sp, bx, base, bz, seed) : World` (only into air); `Fruit.fell(w, sp, bx, base, bz, seed) : Felled` with `Felled(World, stems : Int, caps : Int)` counting the blocks removed. `Myc.fruit_vigour()` = 0.8; `Myc.reseed(f, n, x, z) : Field` (reach back to `plant_reach()` on that column if it holds a species).

- [ ] **Step 1: Failing tests** — `test/fruit_test.march`:

```march
-- Fruit bodies: cells, shapes, stamping and felling.
mod CubeForge.Test.Fruit do

  needs IO.Spawn
  import Test
  alias CubeForge.Fruit as Fr
  alias CubeForge.Chunk as Cf
  alias CubeForge.World as Wf

  -- A flat stone world at height 70 (8x8 chunks).
  pfn flat_cells(c : CubeForge.Chunk.Chunk, i : Int) : CubeForge.Chunk.Chunk do
    if i >= 256 * 71 do c else flat_cells(Cf.set(c, i % 16, i / 256, (i / 16) % 16, 3), i + 1) end
  end
  pfn flat_chunks(i : Int, acc : List(CubeForge.Chunk.Chunk)) : List(CubeForge.Chunk.Chunk) do
    if i < 0 do acc else flat_chunks(i - 1, Cons(flat_cells(Cf.new(), 0), acc)) end
  end
  pfn flat_world() : CubeForge.World.World do
    Wf.World(Array.from_list(flat_chunks(63, Nil)), CubeForge.Light.new(), CubeForge.Light.new(), CubeForge.Light.new(), Wf.shown_new(), 8)
  end

  -- Count blocks of the body's ids in the 9x9x12 box above (bx, base, bz).
  pfn count_go(w : CubeForge.World.World, bx : Int, base : Int, bz : Int, i : Int, acc : Int) : Int do
    if i >= 81 * 12 do acc
    else
      let x = bx - 4 + i % 9
      let z = bz - 4 + (i / 9) % 9
      let y = base + 1 + i / 81
      count_go(w, bx, base, bz, i + 1, if Cf.is_fruit(Wf.block_at(w, x, y, z)) do acc + 1 else acc end)
    end
  end

  describe "Fruit cells" do
    test "cell sizes by tier and one canonical column per cell" do
      Test.assert_eq_int(Fr.cell_size(0), 3, "small")
      Test.assert_eq_int(Fr.cell_size(1), 6, "medium")
      Test.assert_eq_int(Fr.cell_size(2), 16, "giant")
      let c = Fr.canonical(3, 5, 2, 7)
      let x = c % 4096
      let z = c / 4096
      Test.assert_true(x >= 48 && x < 64 && z >= 80 && z < 96, "inside its 16x16 cell")
      Test.assert_true(Fr.is_canonical(x, z, 2, 7), "that column is canonical")
      Test.assert_false(Fr.is_canonical(x + 1, z, 2, 7), "its neighbour is not")
      Test.assert_true(Fr.canonical(3, 5, 2, 8) != c, "seeded")
    end
  end

  describe "Fruit shapes" do
    test "a small body is one cutout block on the ground" do
      Test.assert_eq_int(Fr.block_of_body(3, 40, 70, 40, 7, 40, 71, 40), Cf.fruit_small(3), "the block")
      Test.assert_eq_int(Fr.block_of_body(3, 40, 70, 40, 7, 40, 72, 40), 0, "nothing above")
      Test.assert_eq_int(Fr.block_of_body(3, 40, 70, 40, 7, 41, 71, 40), 0, "nothing beside")
    end
    test "a medium body is a stem of two or three and a 3x3 cap" do
      let s = Fr.stem_height(40, 40, 1, 7)
      Test.assert_true(s >= 2 && s <= 3, "stem 2..3")
      Test.assert_eq_int(Fr.block_of_body(4, 40, 70, 40, 7, 40, 71, 40), Cf.fruit_stem(4), "stem bottom")
      Test.assert_eq_int(Fr.block_of_body(4, 40, 70, 40, 7, 40, 70 + s, 40), Cf.fruit_stem(4), "stem top")
      Test.assert_eq_int(Fr.block_of_body(4, 40, 70, 40, 7, 41, 71 + s, 41), Cf.fruit_cap(4), "cap corner")
      Test.assert_eq_int(Fr.block_of_body(4, 40, 70, 40, 7, 40, 71 + s, 40), Cf.fruit_cap(4), "cap centre")
      Test.assert_eq_int(Fr.block_of_body(4, 40, 70, 40, 7, 42, 71 + s, 40), 0, "not two out")
      Test.assert_eq_int(Fr.block_of_body(4, 40, 70, 40, 7, 41, 71, 40), 0, "nothing beside the stem")
    end
    test "a giant body has a tall stem and a wide disc, domed for Marshlight" do
      let s = Fr.stem_height(40, 40, 2, 7)
      let r = Fr.radius(40, 40, 2, 7)
      Test.assert_true(s >= 5 && s <= 9, "stem 5..9")
      Test.assert_true(r >= 3 && r <= 4, "radius 3..4")
      Test.assert_eq_int(Fr.block_of_body(6, 40, 70, 40, 7, 40 + r, 71 + s, 40), Cf.fruit_cap(6), "disc edge on the axis")
      Test.assert_eq_int(Fr.block_of_body(6, 40, 70, 40, 7, 40 + r, 71 + s, 40 + r), 0, "disc corner is cut")
      Test.assert_eq_int(Fr.block_of_body(6, 40, 70, 40, 7, 40, 72 + s, 40), 0, "Sunshelf is flat")
      Test.assert_eq_int(Fr.block_of_body(5, 40, 70, 40, 7, 40, 72 + s, 40), Cf.fruit_cap(5), "Marshlight has a dome layer")
      Test.assert_true(Fr.dome(5), "dome")
      Test.assert_false(Fr.dome(6), "flat")
    end
  end

  describe "Fruit.stamp and fell" do
    test "stamps only into air and fells exactly its own blocks, counting stems and caps" do
      let w0 = flat_world()
      let w1 = Fr.stamp(w0, 4, 40, 70, 40, 7)
      let s = Fr.stem_height(40, 40, 1, 7)
      Test.assert_eq_int(count_go(w1, 40, 70, 40, 0, 0), s + 9, "stem blocks plus a 3x3 cap")
      Test.assert_true(Fr.has_body(w1, 40, 40, 70), "a body stands there")
      Test.assert_eq_int(Fr.base_of(w1, 40, 40, 70 + s + 1), 70, "the ground under the heightmap's top")
      -- a stone block in the cap's way survives, the cap block there is skipped
      let w2 = Fr.stamp(Wf.set_block(w0, 41, 70 + s + 1, 41, 3), 4, 40, 70, 40, 7)
      Test.assert_eq_int(Wf.block_at(w2, 41, 70 + s + 1, 41), 3, "not overwritten")
      Test.assert_eq_int(count_go(w2, 40, 70, 40, 0, 0), s + 8, "one cap block fewer")
      match Fr.fell(w1, 4, 40, 70, 40, 7) do
        Fr.Felled(w3, stems, caps) ->
          Test.assert_eq_int(count_go(w3, 40, 70, 40, 0, 0), 0, "all gone")
          Test.assert_eq_int(stems, s, "stems counted")
          Test.assert_eq_int(caps, 9, "caps counted")
          Test.assert_false(Fr.has_body(w3, 40, 40, 70), "no body")
      end
      -- felling does not touch a neighbour's cap that overlaps the footprint
      let w4 = Fr.stamp(Fr.stamp(w0, 4, 40, 70, 40, 7), 4, 43, 70, 40, 7)
      match Fr.fell(w4, 4, 40, 70, 40, 7) do
        Fr.Felled(w5, _, _) -> Test.assert_true(Fr.has_body(w5, 43, 40, 70), "the neighbour still stands")
      end
    end
  end

  describe "Myc.reseed" do
    test "resets reach on a held column and ignores an empty one" do
      let n = 32
      let f = CubeForge.Myc.plant_patch(CubeForge.Myc.build(n), n, 16, 16, 3)
      Test.assert_eq_int(CubeForge.Myc.reach_of(f, n, 22, 16), CubeForge.Myc.plant_reach() - 12, "before")
      Test.assert_eq_int(CubeForge.Myc.reach_of(CubeForge.Myc.reseed(f, n, 22, 16), n, 22, 16), CubeForge.Myc.plant_reach(), "after")
      Test.assert_eq_int(CubeForge.Myc.reach_of(CubeForge.Myc.reseed(f, n, 0, 0), n, 0, 0), 0, "empty stays empty")
    end
  end

end
```

The match on `Fr.Felled(...)` may need the constructor unqualified or the type public; follow how `Veg.Felled` is matched elsewhere (it is private there; make `Fruit.Felled` a public type and use `Fruit.felled_world/stems/caps` accessors if a dotted constructor pattern does not parse, GAPS G24/G25). Write the test with the accessors from the start:

```march
      let r = Fr.fell(w1, 4, 40, 70, 40, 7)
      let w3 = Fr.felled_world(r)
      Test.assert_eq_int(Fr.felled_stems(r), s, "stems counted")
      Test.assert_eq_int(Fr.felled_caps(r), 9, "caps counted")
```

- [ ] **Step 2: Run to verify failure.**

- [ ] **Step 3: Implement** — `lib/cube_forge/fruit.march`:

```march
-- Fruit — mushroom bodies on mature mycelium. Three tiers of cube structure,
-- each a pure function of (species, its canonical column, the seed), placed on
-- a cell grid per tier so bodies are spaced and a column is a candidate at
-- most once. The same shape function stamps a body and fells it, so what
-- grows is exactly what goes.
mod CubeForge.Fruit do

  alias CubeForge.Chunk as C
  alias CubeForge.World as W
  alias CubeForge.Noise as N
  alias CubeForge.Species as S

  doc "Side of a fruit cell for tier 0 small, 1 medium, 2 giant."
  fn cell_size(tier : Int) : Int do if tier == 0 do 3 else if tier == 1 do 6 else 16 end end end
  doc "Columns either side of the canonical column a body can reach."
  fn footprint() : Int do 4 end

  pfn hash(a : Int, b : Int, seed : Int, salt : Int) : Float do N.hash2(a * 7907 + salt * 104729, b * 6277 + salt * 15485863, seed + 3251) end

  doc "The one column of cell (cx, cz) at tier [tier] a body may stand on, packed x + 4096 * z."
  fn canonical(cx : Int, cz : Int, tier : Int, seed : Int) : Int do
    let s = cell_size(tier)
    let ox = float_to_int(hash(cx, cz, seed, 20 + tier) *. int_to_float(s))
    let oz = float_to_int(hash(cx, cz, seed, 30 + tier) *. int_to_float(s))
    (cx * s + ox) + 4096 * (cz * s + oz)
  end
  pfn floor_div(a : Int, b : Int) : Int do if a >= 0 do a / b else (a - b + 1) / b end end
  doc "Is (x, z) its cell's canonical column at this tier?"
  fn is_canonical(x : Int, z : Int, tier : Int, seed : Int) : Bool do
    let s = cell_size(tier)
    canonical(floor_div(x, s), floor_div(z, s), tier, seed) == x + 4096 * z
  end

  doc "Stem height: 0 small, 2..3 medium, 5..9 giant."
  fn stem_height(x : Int, z : Int, tier : Int, seed : Int) : Int do
    if tier == 0 do 0
    else if tier == 1 do 2 + float_to_int(hash(x, z, seed, 40) *. 2.0)
    else 5 + float_to_int(hash(x, z, seed, 41) *. 5.0) end end
  end
  doc "Cap radius: 0 small, 1 medium, 3..4 giant."
  fn radius(x : Int, z : Int, tier : Int, seed : Int) : Int do
    if tier == 0 do 0 else if tier == 1 do 1 else 3 + float_to_int(hash(x, z, seed, 42) *. 2.0) end end
  end
  doc "Giant caps of this species carry a second, smaller layer."
  fn dome(sp : Int) : Bool do sp == 5 end
  doc "Blocks above the ground a body of this species at (bx, bz) can occupy."
  fn height_of(sp : Int, bx : Int, bz : Int, seed : Int) : Int do stem_height(bx, bz, S.tier(sp), seed) + 3 end

  pfn abs_i(v : Int) : Int do if v < 0 do 0 - v else v end end

  doc "The block the body of [sp] rooted on ground [base] at (bx, bz) puts at (x, y, z), or 0."
  fn block_of_body(sp : Int, bx : Int, base : Int, bz : Int, seed : Int, x : Int, y : Int, z : Int) : Int do
    let tier = S.tier(sp)
    let dx = x - bx
    let dz = z - bz
    let dy = y - base
    if tier == 0 do
      if dx == 0 && dz == 0 && dy == 1 do C.fruit_small(sp) else 0 end
    else
      let s = stem_height(bx, bz, tier, seed)
      let r = radius(bx, bz, tier, seed)
      if dx == 0 && dz == 0 && dy >= 1 && dy <= s do C.fruit_stem(sp)
      else if dy == s + 1 do
        -- the cap: a square for medium, a disc for giant
        if tier == 1 do (if abs_i(dx) <= 1 && abs_i(dz) <= 1 do C.fruit_cap(sp) else 0 end)
        else (if dx * dx + dz * dz <= r * r do C.fruit_cap(sp) else 0 end) end
      else if dy == s + 2 && tier == 2 && dome(sp) do
        let r2 = r - 2
        if dx * dx + dz * dz <= r2 * r2 do C.fruit_cap(sp) else 0 end
      else 0 end end end
    end
  end

  -- ── Reading bodies off the world ─────────────────────────────────────────
  doc "The ground under a fruit column: from the heightmap's top [h] down past any fruit blocks."
  fn base_of(w : CubeForge.World.World, x : Int, z : Int, h : Int) : Int do
    if h <= 0 do 0
    else if C.is_fruit(W.block_at(w, x, h, z)) do base_of(w, x, z, h - 1)
    else h end end
  end
  doc "Does a body stand on ground [base] at (x, z)? Its first block is a small or a stem."
  fn has_body(w : CubeForge.World.World, x : Int, z : Int, base : Int) : Bool do
    let id = W.block_at(w, x, base + 1, z)
    C.is_fruit_small(id) || C.is_fruit_stem(id)
  end

  -- ── World edits ──────────────────────────────────────────────────────────
  pfn stamp_go(w : CubeForge.World.World, sp : Int, bx : Int, base : Int, bz : Int, seed : Int, i : Int, rows : Int) : CubeForge.World.World do
    if i >= 81 * rows do w
    else
      let x = bx - 4 + i % 9
      let z = bz - 4 + (i / 9) % 9
      let y = base + 1 + i / 81
      let id = block_of_body(sp, bx, base, bz, seed, x, y, z)
      let w1 = if id != 0 && y < 256 && W.block_at(w, x, y, z) == 0 do W.set_occupied(W.set_block(w, x, y, z, id), x, y, z, CubeForge.Light.occludes(id)) else w end
      stamp_go(w1, sp, bx, base, bz, seed, i + 1, rows)
    end
  end
  doc "Stamp the body of [sp] on ground [base] at (bx, bz), only into air."
  fn stamp(w : CubeForge.World.World, sp : Int, bx : Int, base : Int, bz : Int, seed : Int) : CubeForge.World.World do
    stamp_go(w, sp, bx, base, bz, seed, 0, height_of(sp, bx, bz, seed))
  end

  type Felled = Felled(CubeForge.World.World, Int, Int)
  fn felled_world(f : Felled) : CubeForge.World.World do match f do Felled(w, _, _) -> w end end
  fn felled_stems(f : Felled) : Int do match f do Felled(_, s, _) -> s end end
  fn felled_caps(f : Felled) : Int do match f do Felled(_, _, c) -> c end end

  pfn fell_go(w : CubeForge.World.World, sp : Int, bx : Int, base : Int, bz : Int, seed : Int, i : Int, rows : Int, stems : Int, caps : Int) : Felled do
    if i >= 81 * rows do Felled(w, stems, caps)
    else
      let x = bx - 4 + i % 9
      let z = bz - 4 + (i / 9) % 9
      let y = base + 1 + i / 81
      let want = block_of_body(sp, bx, base, bz, seed, x, y, z)
      let there = if want != 0 && y < 256 do W.block_at(w, x, y, z) == want else false end
      if !there do fell_go(w, sp, bx, base, bz, seed, i + 1, rows, stems, caps)
      else
        let w1 = W.set_occupied(W.set_block(w, x, y, z, 0), x, y, z, false)
        fell_go(w1, sp, bx, base, bz, seed, i + 1, rows, if C.is_fruit_stem(want) do stems + 1 else stems end, if C.is_fruit_cap(want) do caps + 1 else caps end)
      end
    end
  end
  doc "Remove exactly the blocks the body's shape owns, counting the stems and caps that were actually there."
  fn fell(w : CubeForge.World.World, sp : Int, bx : Int, base : Int, bz : Int, seed : Int) : Felled do
    fell_go(w, sp, bx, base, bz, seed, 0, height_of(sp, bx, bz, seed), 0, 0)
  end

end
```

In `myc.march`:

```march
  doc "Vigour at and above which a column may fruit."
  fn fruit_vigour() : Float do 0.8 end

  doc "A felled body reseeds its column: reach back to plant_reach(), so a mature patch keeps growing without replanting. An empty column is left alone."
  fn reseed(f : Field, n : Int, x : Int, z : Int) : Field do
    if x < 0 || z < 0 || x >= n || z >= n do f
    else
      match f do
        Field(sps, vg, rc, hd, cl) ->
          let i = x + n * z
          if NativeArray.get_u8(sps, i) == 0 do f
          else Field(sps, vg, NativeArray.set_u8(copy_u8(rc, n * n), i, plant_reach()), hd, cl) end
      end
    end
  end
```

Note the neighbour-overlap test: the second body at (43, 40) is medium (species 4) with a 3x3 cap at 42..44, which does not overlap the first's cap at 39..41 — that is what makes "the neighbour still stands" hold trivially. Keep it: felling is by predicted id at position, and the two caps never share a column at that spacing.

- [ ] **Step 4: Test**, **Step 5: commit** `feat(fungus): Fruit cells, body shapes, stamp and fell; Myc.reseed`.

---

### Task 3: Growth, decay and harvest in the frame loop

**Files:**
- Modify: `lib/cube_forge/fruit.march` (`candidates`, `rate`)
- Modify: `lib/cube_forge.march`
- Modify: `test/fruit_test.march`

**Interfaces:**
- Produces: `Fruit.rate(tier) : Float` (0.05 / 0.02 / 0.005); `Fruit.rolls(x, z, tick, seed, tier, scale) : Bool` (`hash2(x * 13 + tick, z * 17 - tick, seed + 91) < rate(tier) * scale`); `Fruit.candidates(myc, shown, w, bio_heights_via : Biome.Field, seed, tick, scan, budget, scale) : List(Int)` — entries: a column index for growth (canonical column of a cell, held by a species at vigour ≥ `fruit_vigour`, no body, ground can carry it, roll passes) or `column + 2^30` for decay (a body stands on a canonical column whose shown species is not the body's species). `fruit_go` in `cube_forge.march` applies entries as tree-sized edits with `World.relight_marked`, `sync_occupancy`, marks; the fruit phase runs on `veg_slot()` on the periods vegetation skips; `CF_FRUIT_BUDGET` (1) and `CF_FRUIT_RATE` (100). Harvest: breaking a small or stem block fells the body (its species from the block id, its base from `Fruit.base_of`), gives `Species.spores(sp)` spores plus the felled stems and caps as blocks, and reseeds the column. The dump summary prints body counts per tier.

- [ ] **Step 1: Failing test** — append to `test/fruit_test.march`:

```march
  describe "Fruit.candidates" do
    test "grows on a mature held canonical column, decays a body the network left" do
      let n = 128
      let w0 = flat_world()
      let f = CubeForge.Myc.plant_patch(CubeForge.Myc.build(n), n, 40, 40, 3)
      let bio = CubeForge.Biome.build(w0, 7)
      -- shown follows the field over the whole patch
      let shown = shown_of(f, n, 0, Wf.shown_new())
      let picks = CubeForge.Fruit.candidates(f, shown, w0, bio, 7, 0, n * n, 100, 1000.0)
      Test.assert_true(List.length(picks) > 0, "at scale 1000 every eligible cell rolls")
      Test.assert_true(List.length(picks) <= 100, "budget")
      let first = CubeForge.Fruit.column_of(List.head_or(picks, 0))
      Test.assert_true(CubeForge.Fruit.is_canonical(first % n, first / n, 0, 7), "a canonical small cell column")
      Test.assert_false(CubeForge.Fruit.is_decay(List.head_or(picks, 0)), "growth")
      -- a body on a column the network no longer shows is a decay entry
      let w1 = CubeForge.Fruit.stamp(w0, 3, first % n, 70, first / n, 7)
      let none = CubeForge.Fruit.candidates(f, Wf.shown_new(), w1, bio, 7, 0, n * n, 100, 1000.0)
      Test.assert_true(List.length(none) > 0, "something to do")
      Test.assert_true(has_decay(none, first), "that column decays")
    end
  end
```

with helpers `shown_of` (writes each held column's species into a shown array) and `has_decay(list, col)` (an entry equal to `col + 1073741824` exists); use `List.head_or` if it exists in the stdlib, else write a `first_of` helper.

- [ ] **Step 2: Implement `candidates`** in `fruit.march`:

```march
  -- ── Growth and decay candidates ──────────────────────────────────────────
  doc "Chance per visit that an eligible cell fruits: small, medium, giant."
  fn rate(tier : Int) : Float do if tier == 0 do 0.05 else if tier == 1 do 0.02 else 0.005 end end end
  doc "The growth roll for a cell's column on tick [tick], scaled by CF_FRUIT_RATE / 100."
  fn rolls(x : Int, z : Int, tick : Int, seed : Int, tier : Int, scale : Float) : Bool do
    N.hash2(x * 13 + tick, z * 17 - tick, seed + 91) < rate(tier) *. scale
  end
  fn is_decay(entry : Int) : Bool do entry >= 1073741824 end
  fn column_of(entry : Int) : Int do entry % 1073741824 end

  pfn scan_go(sps : NativeU8Arr, vg : NativeFloatArr, shown : NativeU8Arr, w : CubeForge.World.World, bio : CubeForge.Biome.Field, seed : Int, tick : Int,
              n : Int, i : Int, left : Int, budget : Int, scale : Float, acc : List(Int)) : List(Int) do
    if left <= 0 || budget <= 0 do acc
    else
      let ci = i % (n * n)
      let x = ci % n
      let z = ci / n
      let h = CubeForge.Biome.height_of(bio, x, z)
      let base = base_of(w, x, z, h)
      let body = has_body(w, x, z, base)
      let sp = NativeArray.get_u8(sps, ci)
      if body do
        -- decay when the ground no longer shows the body's species
        let bsp = C.fruit_species(W.block_at(w, x, base + 1, z))
        if NativeArray.get_u8(shown, ci) != bsp do scan_go(sps, vg, shown, w, bio, seed, tick, n, i + 1, left - 1, budget - 1, scale, Cons(ci + 1073741824, acc))
        else scan_go(sps, vg, shown, w, bio, seed, tick, n, i + 1, left - 1, budget, scale, acc) end
      else if sp == 0 do scan_go(sps, vg, shown, w, bio, seed, tick, n, i + 1, left - 1, budget, scale, acc)
      else
        let tier = S.tier(sp)
        let ok = if NativeArray.get_float(vg, ci) < CubeForge.Myc.fruit_vigour() do false
                 else if NativeArray.get_u8(shown, ci) != sp do false
                 else if !is_canonical(x, z, tier, seed) do false
                 else if base <= 0 || base + height_of(sp, x, z, seed) >= 255 do false
                 else if !C.is_myc(W.block_at(w, x, base, z)) do false
                 else rolls(x, z, tick, seed, tier, scale) end end end end end
        if ok do scan_go(sps, vg, shown, w, bio, seed, tick, n, i + 1, left - 1, budget - 1, scale, Cons(ci, acc))
        else scan_go(sps, vg, shown, w, bio, seed, tick, n, i + 1, left - 1, budget, scale, acc) end
      end end
    end
  end

  doc "Columns to act on this tick: growth entries are column indices, decay entries have bit 30 set. At most [budget], scanning [scan] columns from where tick [tick] leaves off."
  fn candidates(f : CubeForge.Myc.Field, shown : NativeU8Arr, w : CubeForge.World.World, bio : CubeForge.Biome.Field, seed : Int, tick : Int, scan : Int, budget : Int, scale : Float) : List(Int) do
    let n = CubeForge.Biome.cols(w)
    match f do
      CubeForge.Myc.Field(sps, vg, _, _, _) -> scan_go(sps, vg, shown, w, bio, seed, tick, n, (tick * scan) % (n * n), scan, budget, scale, Nil)
    end
  end
```

If matching `CubeForge.Myc.Field(...)` across modules does not parse (GAPS G24/G25), add `Myc.field_vigour(f)` beside `field_species` and read the two arrays through accessors before the loop.

The scan calls `World.block_at` twice per column over 2048 columns per visit (4096 trie walks, ~3 ms in debug per GAPS G64). That is the same shape `Veg.candidates` already has and it runs on a slot no other phase uses; measure it and, if it shows in the budget, add the row pre-pass from `Myc.tick` so columns with nothing near them are skipped.

- [ ] **Step 3: The frame loop.** In `cube_forge.march`, after the vegetation phase:

```march
          -- Fruit: on the vegetation slot, on the periods vegetation skips, so
          -- no frame carries a tree and a mushroom at once.
          let sc1pre2 = if if phase == veg_slot() do (frame / tick_period()) % 2 == 1 else false end do
            let tf0 = Win.time()
            let fpicks = Fruit.candidates(scene_myc(sc1pre), World.shown(scene_world(sc1pre)), scene_world(sc1pre), scene_biome(sc1pre), seed, frame / tick_period(), 2048, fruit_budget, int_to_float(fruit_rate) /. 100.0)
            let scf = fruit_go(sc1pre, seed, fpicks)
            if autoflow >= 0 && List.length(fpicks) > 0 do println("fruit: " ++ int_to_string(List.length(fpicks)) ++ " bodies in " ++ ms(Win.time() -. tf0)) else () end
            scf
          else sc1pre end
```

and the drain line reads `sc1pre2`. `fruit_go`, modelled on `vegetate`/`veg_go`:

```march
  pfn fruit_go(sc, seed : Int, entries : List(Int)) do
    match entries do
      Nil -> sc
      _ ->
        match sc do
          Scene(world, counts, outline, hit, ui, pids, flags, meshes, bio, pend, myc) ->
            let n = World.side(world)
            match fruit_edits(world, bio, myc, CubeForge.Light.marks_new(), seed, n, entries) do
              FruitEdited(w1, bio1, myc1, marks) ->
                Scene(w1, counts, outline, hit, ui, pids, flags, meshes, bio1, stage_go(pend, marks, 0, pass_solid()), myc1)
            end
        end
    end
  end
  type FruitEdited = FruitEdited(CubeForge.World.World, CubeForge.Biome.Field, CubeForge.Myc.Field, NativeIntArr)
  pfn fruit_edits(w : CubeForge.World.World, bio : CubeForge.Biome.Field, myc : CubeForge.Myc.Field, marks : NativeIntArr, seed : Int, n : Int, entries : List(Int)) : FruitEdited do
    match entries do
      Nil -> FruitEdited(w, bio, myc, marks)
      Cons(e, rest) ->
        let ci = Fruit.column_of(e)
        let x = ci % (n * 16)
        let z = ci / (n * 16)
        let h = Biome.height_of(bio, x, z)
        let base = Fruit.base_of(w, x, z, h)
        let sp = if Fruit.is_decay(e) do Chunk.fruit_species(World.block_at(w, x, base + 1, z)) else World.shown_at(w, x, z) end
        let top = base + Fruit.height_of(sp, x, z, seed)
        let w1 = if Fruit.is_decay(e) do Fruit.felled_world(Fruit.fell(w, sp, x, base, z, seed)) else Fruit.stamp(w, sp, x, base, z, seed) end
        let relit = World.relight_marked(w1, x, base + 2, z)
        let w2 = World.relit_world(relit)
        sync_occupancy(w2, x, z, base, top)
        let marks1 = or_marks(mark_box(marks, x - 4, base, z - 4, x + 4, top, z + 4, n, 0), World.relit_marks(relit), 0)
        fruit_edits(w2, Biome.rescan_box(bio, w2, x, z, Fruit.footprint() + 1, top), myc, marks1, seed, n, rest)
    end
  end
```

- [ ] **Step 4: Harvest.** In `interact`'s break branch, after `let sc3 = if Chunk.is_log(id) ...`:

```march
          -- breaking a small mushroom or a stem fells the body: spores by
          -- tier, its stems and caps as blocks, and the column reseeds
          let sc4 = if Chunk.is_fruit_small(id) || Chunk.is_fruit_stem(id) do fell_body(sc3, Ray.bx(hit), Ray.by_(hit), Ray.bz(hit), Chunk.fruit_species(id), seed) else sc3 end
```

and the harvest lines below use `sc4` and its inventory (`ui_inv(scene_ui(sc4))` in place of `inv1` for the give chain, since `fell_body` adds to the inventory). `fell_body`:

```march
  -- The block at (x, y, z) was part of a body of [sp] that has just lost it;
  -- take the rest down, pay out, and reseed the column.
  pfn fell_body(sc, x : Int, y : Int, z : Int, sp : Int, seed : Int) do
    match sc do
      Scene(world, counts, outline, hit, ui, pids, flags, meshes, bio, pend, myc) ->
        let n = World.side(world)
        let base = Fruit.base_of(world, x, z, y)
        let r = Fruit.fell(world, sp, x, base, z, seed)
        let w1 = Fruit.felled_world(r)
        let top = base + Fruit.height_of(sp, x, z, seed)
        let relit = World.relight_marked(w1, x, base + 2, z)
        let w2 = World.relit_world(relit)
        sync_occupancy(w2, x, z, base, top)
        let marks = or_marks(mark_box(CubeForge.Light.marks_new(), x - 4, base, z - 4, x + 4, top, z + 4, n, 0), World.relit_marks(relit), 0)
        let inv1 = Inv.give(Inv.give(Inv.give(ui_inv(ui), Species.spore_item(sp), Species.spores(sp)), Chunk.fruit_stem(sp), Fruit.felled_stems(r)), Chunk.fruit_cap(sp), Fruit.felled_caps(r))
        Scene(w2, counts, outline, hit, ui_with_inv(ui, inv1), pids, flags, meshes, Biome.rescan_box(bio, w2, x, z, Fruit.footprint() + 1, top),
              stage_go(pend, marks, 0, pass_solid()), Myc.reseed(myc, n * 16, x, z))
    end
  end
```

`base_of(world, x, z, y)` walks down from the broken block's own height; the block itself is already air by then (edit_block ran first), so it walks from `y`: if `y` is now air, `base_of` returns `y` — wrong. Compute `base` **before** `edit_block` runs: in `interact`, compute `let fruit_base = Fruit.base_of(world, Ray.bx(hit), Ray.bz(hit), Ray.by_(hit))` from the pre-edit `world`, and pass it to `fell_body` instead of recomputing.

- [ ] **Step 5: Knobs** `CF_FRUIT_BUDGET` (1), `CF_FRUIT_RATE` (100), and the dump summary line `fruit: small N medium N giant N` from a scan of the world's canonical columns (`Fruit.count_bodies(w, bio, seed, n, tier)`; add it to `fruit.march`).

- [ ] **Step 6: Build, lint, test.** Then an end-to-end run at a high rate on the wild world:

```bash
CF_NOMOUSE=1 CF_SEED=7 CF_SUN=45 CF_TIME=0 CF_WEATHER=0 CF_FRAMES=2100 CF_FRUIT_RATE=2000 CF_AUTOFLOW=999999 CF_DUMP=<scratch>/fruit.bmp CF_DUMP_FRAME=2000 ./.march/build/debug/cube_forge 2>&1 | grep -i "fruit"
```

Expected: `fruit: 1 bodies in ...` lines (tree-sized cost, ~50 ms debug), the summary with small and medium bodies, mushrooms visible in the frame if a patch is in view (the spawn at seed 7 has a Meadowbell patch nearby; small mushrooms should be there). Keep the frame as `docs/fungus-fruit.png`. A night run near a Lanterncap patch for glowing medium caps if one is in view; otherwise note it.

- [ ] **Step 7: Budget**, then commit `feat(fungus): fruit grows, decays and is harvested; small and medium bodies`.

---

### Task 4: Giants

Giants already grow through Task 3's shape function (tier 2 for Marshlight and Sunshelf). This task verifies them at scale and tunes what the numbers show.

- [ ] **Step 1: Cost.** A giant is up to 9 stem + 49 disc + 13 dome blocks with a `relight_marked` at its base: measure `fruit: 1 bodies in` on a Marshlight patch (`CF_AUTOPLANT_SPECIES=5` on a wet column, or the wild wetland patches at seed 7 with `CF_FRUIT_RATE=5000`). If a giant's edit exceeds a tree's (~60 ms debug), split its stamping across two slots (stem first, cap next) by returning the body in two entries; otherwise leave it.
- [ ] **Step 2: Walkable.** `Player` collision uses `is_collidable`, which is true for caps: confirm by a run that `CF_AUTOWALK` onto a cap lands the player on top (the `player at` line's y equals cap height + 1). If the player clips through, the cap is not in the occupancy the physics reads and `sync_occupancy` bounds need the disc radius.
- [ ] **Step 3: Night frame** of a glowing giant (Marshlight, emission 10 on every cap block): the dome should light the ground around it in a wide pool. Keep as `docs/fungus-giant.png`.
- [ ] **Step 4: Budget**, results (`### Fruit (fungus phases 5-6)`: per-body edit cost by tier, scan cost, budget), spec §5 *As built* (species in the fruit id; dome for Marshlight only; rates per visit; harvest yields), `todos.md` entries for phases 5 and 6, commit `feat(fungus): giants verified; phases 5-6 recorded`.

---

## Self-review

- **Spec §5:** fruit cells per tier (Task 2), pure body function so seams agree (Task 2: stamping is by world coordinate, and the shape depends only on the canonical column), growth roll on mature columns through the queue (Task 3), shapes for all three tiers with a dome variant (Task 2), decay when the column loses the species (Task 3), fell on small/stem break with spores by tier plus stems and caps, reseed (Task 3), `CF_FRUIT_RATE` (Task 3). §6 fruit emission rows (Task 1: small 6, cap 10; medium and giant caps share 10 rather than 10/12 — the giant has 49+ cap blocks, which is the bigger light). §4 fruit blocks harvestable and placeable, placed glowing cap a light block (Task 1: the id carries species and so emission). §9 steps 5-6.
- **Type consistency:** `Fruit.block_of_body(sp, bx, base, bz, seed, x, y, z)`, `Fruit.stamp/fell(w, sp, bx, base, bz, seed)`, `Fruit.candidates(f, shown, w, bio, seed, tick, scan, budget, scale)`, `Felled` accessors, `Myc.reseed(f, n, x, z)`.
