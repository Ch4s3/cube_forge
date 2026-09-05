# Fungus phase 3: mycelium blocks — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The mycelium field becomes visible in the world: a column the field holds gets a combined "mycelium over <base>" surface block textured in its species' colour, a column the field has left gets its base block back, both through a budgeted per-tick queue; glowing species' mycelium seeds the block-light channel.

**Architecture:** Block ids encode **base × glow** (7 bases × 3 glow levels = 21 ids, 23..43), so `Light.emission` stays a pure function of the id and `Chunk.myc_base` restores the right surface. The **species** is not in the id: `World` gains a per-column `shown` array (the species the surface currently displays), the mesher reads it to pick a per-(base, species) texture layer, and the greedy key carries the species so faces of different species never merge. `Myc.migrations` compares the field against `shown` and returns at most a budget of columns whose surface should change; the frame loop applies them on the retexture slots as real block edits, relighting block light only when the emission changed.

**Tech Stack:** March, the C shim untouched.

Spec: `docs/superpowers/specs/2026-09-04-fungus-design.md` §4, §6 (mycelium row), §9 step 3.

## Global Constraints

- Everything from the phase 1 and 2 plans: no short-circuit `&&`, no read-then-write on one array in a pass, destructure once per pass, `by`/`on` reserved, `doc` and attributes do not mix, test aliases unique across `test/` (GAPS G69).
- **Texture generators never read the texture array** (GAPS G21: a read before a write copies the whole 60 KB array per texel). A mycelium layer reproduces its base's pattern from the base's formula and overlays threads; it does not copy the base layer.
- The mesher indexes the `shown` array as `wx + 128 * wz`, the same fixed width the light field uses.
- Mycelium ids are opaque, collidable, breakable and never palette blocks, so nothing in the biome retexture, vegetation or lighting code needs a special case for them.
- Frame budget: `scratch/frame_budget.sh` (12 ms) must still pass at the end.
- Commit after every task. No attribution lines.

## File structure

| file | responsibility |
|---|---|
| `lib/cube_forge/chunk.march` | `myc_id`, `is_myc`, `myc_base`, `myc_glow`, `myc_base_index`, `myc_first/last`. |
| `lib/cube_forge/light.march` | `emission` for mycelium glow levels. |
| `lib/cube_forge/texture.march` | 42 mycelium layers (18..59), `myc_layer(base_index, species)`, `layers()` = 60. |
| `lib/cube_forge/world.march` | `shown` array: `shown(w)`, `shown_at`, `set_shown`, constructors. |
| `lib/cube_forge/mesher.march`, `chunk_mesh.march` | `sps` threaded; species in the key; layer choice. |
| `lib/cube_forge/myc.march` | `show_vigour`, `migrations`. |
| `lib/cube_forge/inventory.march` | mycelium yields its base. |
| `lib/cube_forge.march` | apply migrations on the retexture slots with two budgets; knobs; `CF_AUTOPLANT_SPECIES`. |
| tests | `light_test`, `greedy_test`, `myc_test`, a new `myc_block_test`. |

---

### Task 1: Ids, emission, textures

**Files:**
- Modify: `lib/cube_forge/chunk.march` (after `glow_cap`), `lib/cube_forge/light.march` (`emission`), `lib/cube_forge/texture.march`
- Create: `test/myc_block_test.march`

**Interfaces:**
- Produces: `Chunk.myc_first() : Int` = 23, `Chunk.myc_last() : Int` = 43, `Chunk.myc_id(base : Int, glow : Int) : Int` (base a block id among grass 1, dirt 2, sand 12, snow 13, gravel 20, clay 21, stone 3; glow 0..2; returns 0 for any other base), `Chunk.is_myc(id) : Bool`, `Chunk.myc_base(id) : Int` (the base block id), `Chunk.myc_glow(id) : Int`, `Chunk.myc_base_index(id) : Int` 0..6, `Chunk.myc_base_of_index(k) : Int`. `Light.emission`: glow 1 → 2, glow 2 → 4. `Texture.myc_layer(base_index : Int, species : Int) : Float` = `18 + 6 * base_index + (species - 1)`; `Texture.layers()` = 60; `Texture.layer_for(myc id)` = the base's layer.

- [ ] **Step 1: Write the failing test** — `test/myc_block_test.march`:

```march
-- Mycelium blocks: ids that encode base and glow, their emission, and their layers.
mod CubeForge.Test.MycBlock do

  import Test
  alias CubeForge.Chunk as Ch
  alias CubeForge.Light as Lt
  alias CubeForge.Texture as Tx

  pfn near(a : Float, b : Float) : Bool do Math.abs(a -. b) < 0.0001 end

  describe "Chunk mycelium ids" do
    test "encode seven bases and three glow levels in 23..43" do
      Test.assert_eq_int(Ch.myc_id(1, 0), 23, "grass, no glow, is the first")
      Test.assert_eq_int(Ch.myc_id(3, 2), 43, "stone, bright, is the last")
      Test.assert_eq_int(Ch.myc_first(), 23, "first")
      Test.assert_eq_int(Ch.myc_last(), 43, "last")
      Test.assert_true(Ch.is_myc(30), "30 is mycelium")
      Test.assert_false(Ch.is_myc(22), "the glow cap is not")
      Test.assert_false(Ch.is_myc(44), "44 is not")
      Test.assert_eq_int(Ch.myc_id(16, 0), 0, "a log is not a base")
    end
    test "round-trip base and glow" do
      Test.assert_eq_int(Ch.myc_base(Ch.myc_id(Ch.snow(), 1)), Ch.snow(), "snow")
      Test.assert_eq_int(Ch.myc_glow(Ch.myc_id(Ch.snow(), 1)), 1, "dim")
      Test.assert_eq_int(Ch.myc_base(Ch.myc_id(Ch.clay(), 2)), Ch.clay(), "clay")
      Test.assert_eq_int(Ch.myc_glow(Ch.myc_id(Ch.clay(), 2)), 2, "bright")
      Test.assert_eq_int(Ch.myc_base_index(Ch.myc_id(Ch.gravel(), 0)), 4, "gravel is base 4")
      Test.assert_eq_int(Ch.myc_base_of_index(4), Ch.gravel(), "and back")
      Test.assert_eq_int(Ch.myc_base(1), 1, "a non-mycelium id is its own base")
    end
    test "mycelium is solid, opaque and not a palette block" do
      Test.assert_true(Ch.is_collidable(Ch.myc_id(1, 0)), "collidable")
      Test.assert_true(Lt.is_opaque(Ch.myc_id(1, 0)), "opaque")
      Test.assert_false(CubeForge.Biome.is_palette_block(Ch.myc_id(1, 0)), "not migrated by the biome")
    end
  end

  describe "Light.emission for mycelium" do
    test "dim glows at 2, bright at 4, none at 0" do
      Test.assert_eq_int(Lt.emission(Ch.myc_id(2, 0)), 0, "none")
      Test.assert_eq_int(Lt.emission(Ch.myc_id(2, 1)), 2, "dim")
      Test.assert_eq_int(Lt.emission(Ch.myc_id(2, 2)), 4, "bright")
    end
  end

  describe "Texture mycelium layers" do
    test "one layer per (base, species), after the existing seventeen" do
      Test.assert_eq_int(Tx.layers(), 60, "17 + 42 + the glow cap")
      Test.assert_true(near(Tx.myc_layer(0, 1), 18.0), "grass x Frostcap")
      Test.assert_true(near(Tx.myc_layer(0, 6), 23.0), "grass x Sunshelf")
      Test.assert_true(near(Tx.myc_layer(6, 6), 59.0), "stone x Sunshelf is the last")
      Test.assert_true(near(Tx.layer_for(Ch.myc_id(Ch.sand(), 0)), 4.0), "without a species, the base layer")
    end
    test "a mycelium layer differs from its base and carries the species colour" do
      let a = Tx.checkerboard()
      -- texel (3, 3) of grass x Lanterncap against plain grass
      let off = float_to_int(Tx.myc_layer(0, 4)) * 1024 + (3 + 16 * 3) * 4
      let base = (3 + 16 * 3) * 4
      let differs = NativeArray.get_u8(a, off) != NativeArray.get_u8(a, base) || NativeArray.get_u8(a, off + 1) != NativeArray.get_u8(a, base + 1)
      Test.assert_true(differs, "not the plain base")
      Test.assert_eq_int(NativeArray.get_u8(a, off + 3), 255, "opaque")
    end
  end

end
```

- [ ] **Step 2: Run it to verify it fails**

Run: `forge test 2>&1 | grep -c ERROR`
Expected: non-zero.

- [ ] **Step 3: Implement.** In `chunk.march` after `glow_cap`:

```march
  -- ── Mycelium: a surface block wearing the network ─────────────────────────
  -- Ids 23..43 encode base x glow: id = 23 + 3 * base_index + glow. The base
  -- is in the id so the surface can be restored when the network leaves; the
  -- glow is in the id so Light.emission stays a function of the block alone.
  -- The SPECIES is not in the id: it lives in the field, and the mesher reads
  -- it from World.shown to pick the texture. Seven bases, in index order:
  -- grass, dirt, sand, snow, gravel, clay, stone.
  fn myc_first() : Int do 23 end
  fn myc_last() : Int do 43 end
  fn is_myc(id : Int) : Bool do id >= myc_first() && id <= myc_last() end
  doc "Base index 0..6 for a base block id, or -1 when the block cannot carry mycelium."
  fn myc_index_of(base : Int) : Int do
    if base == 1 do 0 else if base == 2 do 1 else if base == sand() do 2 else if base == snow() do 3
    else if base == gravel() do 4 else if base == clay() do 5 else if base == 3 do 6 else -1 end end end end end end end
  end
  doc "The base block id for base index 0..6."
  fn myc_base_of_index(k : Int) : Int do
    if k == 0 do 1 else if k == 1 do 2 else if k == 2 do sand() else if k == 3 do snow()
    else if k == 4 do gravel() else if k == 5 do clay() else 3 end end end end end end
  end
  doc "Mycelium over [base] at glow 0..2, or 0 when the base cannot carry it."
  fn myc_id(base : Int, glow : Int) : Int do
    let k = myc_index_of(base)
    if k < 0 do 0 else myc_first() + 3 * k + glow end
  end
  fn myc_base_index(id : Int) : Int do (id - myc_first()) / 3 end
  fn myc_glow(id : Int) : Int do if is_myc(id) do (id - myc_first()) % 3 else 0 end end
  doc "The base block under a mycelium id; any other id is its own base."
  fn myc_base(id : Int) : Int do if is_myc(id) do myc_base_of_index(myc_base_index(id)) else id end end
```

In `light.march`, `emission` becomes:

```march
  fn emission(id : Int) : Int do
    if id == C.glow_cap() do 10
    else if C.is_myc(id) do 2 * C.myc_glow(id)
    else 0 end end
  end
```

In `texture.march`: `layers()` returns 60; `checkerboard` allocates `60 * 16 * 16 * 4`; after the glow-cap line, chain the mycelium layers:

```march
    let a17 = speckle_go(a16, 17, 0, 250, 214, 120)
    myc_all_go(a17, 0)
```

and add the generators:

```march
  -- ── Mycelium layers: the base's pattern with threads over it ─────────────
  -- Layer 18 + 6 * base_index + (species - 1). Each layer REPRODUCES its base
  -- from the base's formula rather than copying the base layer: reading the
  -- array while writing it would copy all 60 KB per texel (GAPS G21).
  doc "Texture layer for mycelium over base index [k] (0..6) in species [sp] (1..6)."
  fn myc_layer(k : Int, sp : Int) : Float do int_to_float(18 + 6 * k + (sp - 1)) end

  -- The base colour at texel i for base index k, by the same formulas as the
  -- plain layers: grass is the checker, everything else a speckle.
  pfn myc_base_r(k : Int) : Int do if k == 1 do 134 else if k == 2 do 219 else if k == 3 do 242 else if k == 4 do 112 else if k == 5 do 158 else 128 end end end end end end
  pfn myc_base_g(k : Int) : Int do if k == 1 do 96 else if k == 2 do 203 else if k == 3 do 246 else if k == 4 do 110 else if k == 5 do 142 else 128 end end end end end end
  pfn myc_base_b(k : Int) : Int do if k == 1 do 67 else if k == 2 do 146 else if k == 3 do 250 else if k == 4 do 106 else if k == 5 do 124 else 132 end end end end end end

  pfn myc_go(a : NativeU8Arr, layer : Int, k : Int, sp : Int, i : Int) : NativeU8Arr do
    if i >= 256 do a
    else
      let x = i % 16
      let y = i / 16
      -- the base
      let br = if k == 0 do (if (x / 2 + y / 2) % 2 == 0 do 110 else 70 end) else myc_base_r(k) + ((x * 7 + y * 13 + x * y) % 5) * 6 - 12 end
      let bg = if k == 0 do (if (x / 2 + y / 2) % 2 == 0 do 190 else 140 end) else myc_base_g(k) + ((x * 7 + y * 13 + x * y) % 5) * 6 - 12 end
      let bb = if k == 0 do (if (x / 2 + y / 2) % 2 == 0 do 80 else 60 end) else myc_base_b(k) + ((x * 7 + y * 13 + x * y) % 5) * 6 - 12 end
      -- the threads: a sparse branching lattice, about a third of the texels
      let t = (x * 3 + y * 5 + (x * y) % 7 + sp) % 9
      let thread = t < 3
      let sr = CubeForge.Species.colour_r(sp)
      let sg = CubeForge.Species.colour_g(sp)
      let sb = CubeForge.Species.colour_b(sp)
      -- a thread texel is two-thirds species colour; the rest of the layer is
      -- the base pulled a sixth of the way toward it, so the tint reads at a distance
      let r = if thread do (br + 2 * sr) / 3 else (5 * br + sr) / 6 end
      let g = if thread do (bg + 2 * sg) / 3 else (5 * bg + sg) / 6 end
      let b = if thread do (bb + 2 * sb) / 3 else (5 * bb + sb) / 6 end
      myc_go(put_rgba(a, layer * 1024 + i * 4, r, g, b, 255), layer, k, sp, i + 1)
    end
  end

  -- All 42 layers: j runs over base index x species.
  pfn myc_all_go(a : NativeU8Arr, j : Int) : NativeU8Arr do
    if j >= 42 do a
    else
      let k = j / 6
      let sp = j % 6 + 1
      myc_all_go(myc_go(a, 18 + j, k, sp, 0), j + 1)
    end
  end
```

`layer_for` gains, before the final `else 0.0`: `else if CubeForge.Chunk.is_myc(id) do layer_for(CubeForge.Chunk.myc_base(id))` (and one more `end`). Check `texture.march` can reference `CubeForge.Chunk` and `CubeForge.Species` without a cycle: `chunk.march` does not import `Texture`, and `species.march` imports nothing. If the module system complains, move the base-colour table into `Species` instead.

- [ ] **Step 4: Run the tests**

Run: `forge test 2>&1 | grep -B2 -A8 "ERROR\|FAIL:" | head -30; forge test 2>&1 | tail -1`
Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add lib/cube_forge/chunk.march lib/cube_forge/light.march lib/cube_forge/texture.march test/myc_block_test.march
git commit -m "feat(fungus): mycelium block ids (base x glow), emission, 42 species layers"
```

---

### Task 2: `World.shown` — the species each column displays

**Files:**
- Modify: `lib/cube_forge/world.march`
- Modify: the five test constructors (`test/light_test.march:18,33`, `test/veg_test.march`, `test/biome_test.march`, `test/flow_test.march`)
- Modify: `test/myc_block_test.march`

**Interfaces:**
- Produces: `World = World(chunks, sky, block light, occupancy, shown, side)`; `World.shown(w) : NativeU8Arr` (16,384 entries, indexed `wx + 128 * wz`); `World.shown_at(w, x, z) : Int`; `World.set_shown(w, x, z, sp) : World`; `World.shown_new() : NativeU8Arr`.

- [ ] **Step 1: Write the failing test** — append to `test/myc_block_test.march`:

```march
  describe "World.shown" do
    test "starts empty, reads back a write, and is out of range safe" do
      let w0 = CubeForge.World.generate(1, 1, 1)
      Test.assert_eq_int(CubeForge.World.shown_at(w0, 5, 5), 0, "empty")
      let w1 = CubeForge.World.set_shown(w0, 5, 5, 4)
      Test.assert_eq_int(CubeForge.World.shown_at(w1, 5, 5), 4, "written")
      Test.assert_eq_int(CubeForge.World.shown_at(w1, 6, 5), 0, "neighbour untouched")
      Test.assert_eq_int(CubeForge.World.shown_at(w1, -1, 5), 0, "outside reads 0")
      Test.assert_eq_int(CubeForge.World.shown_at(CubeForge.World.set_shown(w1, 200, 5, 3), 5, 5), 4, "outside write is a no-op")
    end
  end
```

- [ ] **Step 2: Run to verify failure**, then **Step 3: implement.** In `world.march` the type gains a `NativeU8Arr` between occupancy and side; every `World(...)` match and construction (16 in `world.march`) gains the field in that position — name it `sh` in patterns. New functions:

```march
  doc "A fresh, empty shown-species array: one byte per column of a 128-wide world."
  fn shown_new() : NativeU8Arr do NativeArray.make_u8(16384, 0) end
  doc "The species each column's surface currently displays (0 = none). The mesher reads it to pick mycelium textures; the fungus migration writes it in step with the block."
  fn shown(w : World) : NativeU8Arr do match w do World(_, _, _, _, sh, _) -> sh end end
  fn shown_at(w : World, x : Int, z : Int) : Int do
    if x < 0 || z < 0 || x >= 128 || z >= 128 do 0
    else match w do World(_, _, _, _, sh, _) -> NativeArray.get_u8(sh, x + 128 * z) end end
  end
  fn set_shown(w : World, x : Int, z : Int, sp : Int) : World do
    if x < 0 || z < 0 || x >= 128 || z >= 128 do w
    else match w do World(cs, la, lb, o, sh, n) -> World(cs, la, lb, o, NativeArray.set_u8(sh, x + 128 * z, sp), n) end end
  end
```

The two `generate*` constructors pass `shown_new()`. The five test constructors gain `CubeForge.World.shown_new()` (or `L.new()`-style equivalent) before the side length: read each and insert in the right position.

- [ ] **Step 4: Build and test**, then **Step 5: commit**

```bash
git add lib/cube_forge/world.march test/light_test.march test/veg_test.march test/biome_test.march test/flow_test.march test/myc_block_test.march
git commit -m "feat(fungus): World.shown — the species each column displays"
```

---

### Task 3: The mesher picks mycelium textures by species

**Files:**
- Modify: `lib/cube_forge/mesher.march`, `lib/cube_forge/chunk_mesh.march`
- Modify: `test/greedy_test.march`, `test/light_test.march` (the two direct `mesh_section_opaque` callers)

**Interfaces:**
- Produces: `Mesher.mesh_section_opaque(c, n, s, e, w, la, lb, sps, occ, sy, ox, oz)` and `mesh_section_foliage` likewise — **`sps` inserted after `lb`**; `Mesher.key_of(id, c0, c1, c2, c3, sp)` with the species in bits 48..55; `Mesher.key_sp(k)`; `Mesher.layer_for_key(key, d) : Float`.

- [ ] **Step 1: Write the failing tests.** In `test/light_test.march`'s key round-trip test, change the call to `key_of(3, 1023, 0, 17, 640, 5)` and add `Test.assert_eq_int(CubeForge.Mesher.key_sp(k), 5, "species")`. In `test/greedy_test.march` add:

```march
  describe "mycelium species split merges" do
    test "a slab of mycelium under two species is two top quads, not one" do
      let e = C.new()
      let c = myc_slab_go(C.new(), 0)
      let one = Mesher.mesh_section_opaque(c, e, e, e, e, CubeForge.Light.new(), CubeForge.Light.new(), CubeForge.World.shown_new(), CubeForge.Light.new(), 0, 0.0, 0.0)
      let sps = half_go(CubeForge.World.shown_new(), 0)
      let two = Mesher.mesh_section_opaque(c, e, e, e, e, CubeForge.Light.new(), CubeForge.Light.new(), sps, CubeForge.Light.new(), 0, 0.0, 0.0)
      Test.assert_eq_int(B.len(one) / (6 * CubeForge.Vertex.floats()), 6, "one species: six quads")
      Test.assert_eq_int(B.len(two) / (6 * CubeForge.Vertex.floats()), 6 + 1, "two species: the top splits in two")
      -- the layer of the first top vertex is a mycelium layer for species 1 or 2
      let layer = Test.assert_some(B.get(two, 5), "layer of vertex 0")
      Test.assert_true(layer >= 18.0 && layer < 60.0, "a species layer")
    end
  end
```

with the helpers beside `slab_go`:

```march
  -- flat mycelium-over-stone slab at y = 9
  pfn myc_slab_go(c : CubeForge.Chunk.Chunk, i : Int) : CubeForge.Chunk.Chunk do
    if i >= 256 do c else myc_slab_go(C.set(c, i % 16, 9, i / 16, C.myc_id(3, 0)), i + 1) end
  end
  -- species 1 on the west half of the chunk's columns, 2 on the east
  pfn half_go(s : NativeU8Arr, i : Int) : NativeU8Arr do
    if i >= 256 do s else half_go(NativeArray.set_u8(s, i % 16 + 128 * (i / 16), if i % 16 < 8 do 1 else 2 end), i + 1) end
  end
```

Which quad comes first in the buffer depends on direction order; if "layer of vertex 0" is not a top face, read the layer of the first vertex whose `face` float (index 7) is `Mesher.face_top()` instead. Update the three existing `mesh_section_opaque` calls in `greedy_test.march` and the two in `light_test.march` to pass `CubeForge.World.shown_new()` after the block-light array.

- [ ] **Step 2: Run to verify failure** (arity errors).

- [ ] **Step 3: Implement.** In `mesher.march`:

```march
  -- Mask key: block id in the low 8 bits, four 10-bit corners in mask (u, v)
  -- order, then the column's shown species in bits 48..55. Two faces merge only
  -- when all of it agrees: block, light at every corner, and species.
  fn key_of(id : Int, c0 : Int, c1 : Int, c2 : Int, c3 : Int, sp : Int) : Int do
    id + 256 * c0 + 262144 * c1 + 268435456 * c2 + 274877906944 * c3 + 281474976710656 * sp
  end
  fn key_sp(k : Int) : Int do (k / 281474976710656) % 256 end
```

(`key_c3` keeps its `% 1024`, which already masks the species off.) `face_key` gains `sps : NativeU8Arr` after `lb`, and computes the species only for mycelium:

```march
        let sp = if C.is_myc(id) do NativeArray.get_u8(sps, oxi + x + 128 * (ozi + z)) else 0 end
        key_of(id, k0, k1, k2, k3, sp)
```

(`oxi + x` and `ozi + z` are always inside 0..127 for a chunk of the world, so no guard is needed; `fill_mask` passes chunk-local `x`/`z` in the same frame `corner_pack` uses.) `emit_rect` picks the layer:

```march
    let l = layer_for_key(key, d)
```
```march
  doc "Texture layer for a face of the block a greedy key describes: mycelium takes its (base, species) layer when a species is shown, everything else the block's own."
  fn layer_for_key(key : Int, d : Int) : Float do
    let id = key_id(key)
    let sp = key_sp(key)
    if C.is_myc(id) && sp > 0 do CubeForge.Texture.myc_layer(C.myc_base_index(id), sp)
    else layer_of_face(id, d) end
  end
```

Thread `sps` after `lb` through `fill_mask`, `slices_go`, `dirs_go`, `mesh_section_cat`, `mesh_section_opaque`, `mesh_section_foliage` (`grep -n "la, lb, occ" lib/cube_forge/mesher.march` → every site becomes `la, lb, sps, occ`; signatures get `sps : NativeU8Arr` after `lb : NativeU8Arr`). In `chunk_mesh.march` the four calls pass `World.shown(world)` after `World.block_light(world)`.

- [ ] **Step 4: Build and test**, then **Step 5: commit**

```bash
git add lib/cube_forge/mesher.march lib/cube_forge/chunk_mesh.march test/greedy_test.march test/light_test.march
git commit -m "feat(fungus): the mesher textures mycelium by the shown species"
```

---

### Task 4: `Myc.migrations` and mycelium yield

**Files:**
- Modify: `lib/cube_forge/myc.march`, `lib/cube_forge/inventory.march`
- Modify: `test/myc_test.march`

**Interfaces:**
- Produces: `Myc.show_vigour() : Float` = 0.15; `Myc.wanted(f, n, i) : Int` (the species column `i` should display: its species if vigour ≥ `show_vigour()`, else 0); `Myc.migrations(f, n, shown : NativeU8Arr, tick : Int, scan : Int, budget : Int) : List(Int)` — column indices (`wx + n * wz`, and `n` must be 128 for the `shown` index to agree) whose `wanted` differs from `shown`, scanning `scan` columns from a rolling cursor, at most `budget`. `Inventory.yield_of(myc id)` = the base block.

- [ ] **Step 1: Write the failing tests** — append to `test/myc_test.march`:

```march
  describe "Myc.migrations" do
    test "lists columns whose wanted species differs from what is shown, within the budget" do
      let n = 128
      let f0 = Myc.plant(Myc.plant(Myc.build(n), n, 10, 10, 3), n, 50, 50, 4)
      let shown = CubeForge.World.shown_new()
      let picks = Myc.migrations(f0, n, shown, 0, n * n, 10)
      Test.assert_eq_int(List.length(picks), 2, "two plantings, two migrations")
      Test.assert_eq_int(Myc.wanted(f0, n, 10 + n * 10), 3, "column 10,10 wants Meadowbell")
      let shown1 = NativeArray.set_u8(NativeArray.set_u8(shown, 10 + n * 10, 3), 50 + n * 50, 4)
      Test.assert_eq_int(List.length(Myc.migrations(f0, n, shown1, 0, n * n, 10)), 0, "shown, nothing to do")
      let picks2 = Myc.migrations(f0, n, shown, 0, n * n, 1)
      Test.assert_eq_int(List.length(picks2), 1, "the budget caps it")
    end
    test "a withered column wants nothing, so a shown one migrates back" do
      let n = 128
      let f0 = Myc.plant(Myc.build(n), n, 10, 10, 3)
      let shown = NativeArray.set_u8(CubeForge.World.shown_new(), 10 + n * 10, 3)
      let f1 = tick_n(f0, n, uniform(n, 0.9), uniform(n, 0.9), 0.1, 50)
      Test.assert_eq_int(Myc.wanted(f1, n, 10 + n * 10), 0, "nothing wanted")
      Test.assert_eq_int(List.length(Myc.migrations(f1, n, shown, 0, n * n, 10)), 1, "one migration back")
    end
  end
```

And in `test/myc_block_test.march`:

```march
  describe "Inventory yield for mycelium" do
    test "digging mycelium yields the base block" do
      Test.assert_eq_int(CubeForge.Inventory.yield_of(Ch.myc_id(Ch.sand(), 2)), Ch.sand(), "sand")
      Test.assert_eq_int(CubeForge.Inventory.yield_of(Ch.myc_id(1, 0)), 1, "grass")
    end
  end
```

- [ ] **Step 2: Run to verify failure.**

- [ ] **Step 3: Implement.** In `myc.march`:

```march
  -- ── Migration: what the surface should show ──────────────────────────────
  doc "Vigour at and above which a column's surface shows its species."
  fn show_vigour() : Float do 0.15 end

  doc "The species column [i] should display: its species once vigour reaches show_vigour(), else 0."
  fn wanted(f : Field, n : Int, i : Int) : Int do
    match f do
      Field(sps, vg, _, _, _) ->
        let sp = NativeArray.get_u8(sps, i)
        if sp == 0 do 0 else if NativeArray.get_float(vg, i) >= show_vigour() do sp else 0 end end
    end
  end

  pfn migrations_go(sps : NativeU8Arr, vg : NativeFloatArr, shown : NativeU8Arr, n : Int, i : Int, left : Int, budget : Int, acc : List(Int)) : List(Int) do
    if left <= 0 || budget <= 0 do acc
    else
      let ci = i % (n * n)
      let sp = NativeArray.get_u8(sps, ci)
      let want = if sp == 0 do 0 else if NativeArray.get_float(vg, ci) >= show_vigour() do sp else 0 end end
      if want != NativeArray.get_u8(shown, ci) do migrations_go(sps, vg, shown, n, i + 1, left - 1, budget - 1, Cons(ci, acc))
      else migrations_go(sps, vg, shown, n, i + 1, left - 1, budget, acc) end
    end
  end

  doc "Column indices whose surface should change: wanted species differs from [shown]. Scans [scan] columns from where tick [tick] leaves off (a stateless cursor), returns at most [budget]. Destructures the field once (GAPS G68)."
  fn migrations(f : Field, n : Int, shown : NativeU8Arr, tick : Int, scan : Int, budget : Int) : List(Int) do
    match f do
      Field(sps, vg, _, _, _) -> migrations_go(sps, vg, shown, n, (tick * scan) % (n * n), scan, budget, Nil)
    end
  end
```

In `inventory.march`'s `yield_of`, before the final `else id`: `else if C.is_myc(id) do C.myc_base(id)`.

- [ ] **Step 4: Test**, then **Step 5: commit**

```bash
git add lib/cube_forge/myc.march lib/cube_forge/inventory.march test/myc_test.march test/myc_block_test.march
git commit -m "feat(fungus): Myc.migrations against World.shown; mycelium yields its base"
```

---

### Task 5: Applying migrations in the frame loop

**Files:**
- Modify: `lib/cube_forge.march`

**Interfaces:**
- Consumes: `Myc.migrations`, `Myc.wanted`, `World.shown`, `World.set_shown`, `Chunk.myc_id/myc_base/is_myc`, `Light.emission`, `Light.relight_block_marked`, `Light.or_marks`, `Biome.height_of`, `Biome.is_wet`, `mark_geometry`, `stage_go`, `pass_opaque`.
- Produces: `migrate_myc(sc, picks)`; knobs `CF_MYC_BUDGET` (default 32, columns per retexture slot) and `CF_MYC_GLOW_BUDGET` (default 2, of those, how many may change emission and so relight); `CF_AUTOPLANT_SPECIES` (default 0 = fittest).

- [ ] **Step 1: The migration step.** Beside `migrate` / `migrate_go`:

```march
  -- One mycelium migration: the column's surface takes or drops its mycelium
  -- block, `shown` follows, and block light is repaired only when the emission
  -- changed. Skips wet columns and columns whose surface cannot carry mycelium.
  -- [glow_left] bounds the relights, which are the expensive part.
  type MycMigrated = MycMigrated(CubeForge.World.World, NativeIntArr, Int)
  pfn migrate_myc_go(w : CubeForge.World.World, bio : CubeForge.Biome.Field, myc : CubeForge.Myc.Field, marks : NativeIntArr,
                     n : Int, cols : List(Int), glow_left : Int) : MycMigrated do
    match cols do
      Nil -> MycMigrated(w, marks, glow_left)
      Cons(ci, rest) ->
        let x = ci % (n * 16)
        let z = ci / (n * 16)
        let h = Biome.height_of(bio, x, z)
        let sfc = World.block_at(w, x, h, z)
        let want = Myc.wanted(myc, n * 16, ci)
        let base = Chunk.myc_base(sfc)
        let new_id = if want == 0 do base else Chunk.myc_id(base, Species.glow(want)) end
        -- cannot carry it (water, logs, granite, bedrock...): show nothing and move on
        if h <= 0 || Biome.is_wet(bio, x, z) || new_id == 0 do
          migrate_myc_go(World.set_shown(w, x, z, 0), bio, myc, marks, n, rest, glow_left)
        else
          let glow_changes = CubeForge.Light.emission(sfc) != CubeForge.Light.emission(new_id)
          if glow_changes && glow_left <= 0 do migrate_myc_go(w, bio, myc, marks, n, rest, glow_left)
          else
            let w1 = World.set_shown(if new_id == sfc do w else World.set_block(w, x, h, z, new_id) end, x, z, want)
            let marks1 = mark_geometry(marks, x, h, z, n)
            if glow_changes do
              let relit = CubeForge.Light.relight_block_marked(World.block_light(w1), w1, x, h, z)
              migrate_myc_go(World.set_block_light(w1, CubeForge.Light.relit_field(relit)), bio, myc,
                             CubeForge.Light.or_marks(marks1, CubeForge.Light.relit_marks(relit)), n, rest, glow_left - 1)
            else migrate_myc_go(w1, bio, myc, marks1, n, rest, glow_left) end
          end
        end
    end
  end

  pfn migrate_myc(sc, cols : List(Int), glow_budget : Int) do
    match cols do
      Nil -> sc
      _ ->
        match sc do
          Scene(world, counts, outline, hit, ui, pids, flags, meshes, bio, pend, myc) ->
            match migrate_myc_go(world, bio, myc, CubeForge.Light.marks_new(), World.side(world), cols, glow_budget) do
              MycMigrated(w1, marks, _) ->
                Scene(w1, counts, outline, hit, ui, pids, flags, meshes, bio, stage_go(pend, marks, 0, pass_opaque()), myc)
            end
        end
    end
  end
```

A column skipped for the glow budget stays a candidate next slot; a column that cannot carry mycelium has `shown` forced to 0 so it stops being a candidate — but its field species is unchanged, so `wanted` stays non-zero and it would be picked every scan. Guard that in `migrations_go`: it cannot see the world, so instead make the skip cheap: it is one `block_at` per such column per scan, bounded by the budget. Acceptable; note it in the results.

- [ ] **Step 2: Run it on the retexture slots.** After the biome `migrate` in the frame loop (the `scm` binding):

```march
          let scm2 = if is_migrate_slot(phase) do
            let tq0 = Win.time()
            let mpicks = Myc.migrations(scene_myc(scm), World.side(scene_world(scm)) * 16, World.shown(scene_world(scm)), frame / tick_period(), 2048, myc_budget / migrate_slots())
            let m2 = migrate_myc(scm, mpicks, myc_glow_budget)
            if autoflow >= 0 && frame % 100 == 4 do println("myc migrate: " ++ int_to_string(List.length(mpicks)) ++ " columns in " ++ ms(Win.time() -. tq0)) else () end
            m2
          else scm end
```

and the vegetation phase reads `scm2` where it read `scm`.

- [ ] **Step 3: Knobs.** `Knobs` gains `Int` myc_budget (`CF_MYC_BUDGET`, 32), `Int` myc_glow_budget (`CF_MYC_GLOW_BUDGET`, 2), `Int` autoplant_species (`CF_AUTOPLANT_SPECIES`, 0). `plant_near` takes the override: `let sp = if force > 0 do force else Species.best_for(...) end`.

- [ ] **Step 4: Build, lint, test.**

- [ ] **Step 5: Two end-to-end runs.** Daytime growth, first person, the player looking east (`CF_AUTOSPIN=8` as in phase 1):

```bash
CF_NOMOUSE=1 CF_SEED=7 CF_SUN=45 CF_TIME=0 CF_WEATHER=0 CF_AUTOSPIN=8 CF_FRAMES=2100 CF_AUTOPLANT=100 CF_MYC_RATE=5000 CF_AUTOFLOW=999999 CF_DUMP=<scratch>/myc-ground.bmp CF_DUMP_FRAME=2000 ./.march/build/debug/cube_forge 2>&1 | grep -i "planted\|mycelium\|myc migrate\|state:"
```

Expected: `myc migrate: N columns in ...` lines with N up to 16 per slot early on and 0 once the patch is shown; the dump shows threaded, tinted ground spreading east of the player. Then the night run with a glowing species forced:

```bash
CF_NOMOUSE=1 CF_SEED=7 CF_SUN=180 CF_TIME=0 CF_WEATHER=0 CF_AUTOSPIN=8 CF_FRAMES=1300 CF_AUTOPLANT=100 CF_AUTOPLANT_SPECIES=4 CF_MYC_RATE=100 CF_DUMP=<scratch>/myc-glow.bmp CF_DUMP_FRAME=1200 ./.march/build/debug/cube_forge 2>&1 | grep -i "planted\|mycelium\|state:"
```

Lanterncap on grassland is unfit and withers, but at `CF_MYC_RATE=100` it holds above `show_vigour()` for roughly the first 50 ticks, so at frame 1200 the ground should still show a faint warm floor glow (emission 4) around the planting. Keep the daytime dump as `docs/fungus-myc-ground.png`. Look at both.

- [ ] **Step 6: Budget, results, spec, backlog, commit.** `forge build --release && scratch/frame_budget.sh` passes. `RESULTS.md` gains `### Mycelium blocks (fungus phase 3)`: the `myc migrate` cost with and without a glow relight, the block relight cost per column, the budget result, and the id/species split with its reason. Spec §4 gets an *As built* note (base × glow in the id; species in `World.shown`; the two budgets). `todos.md` gains the phase 3 entry.

```bash
git add lib/cube_forge.march RESULTS.md todos.md docs/superpowers/specs/2026-09-04-fungus-design.md docs/fungus-myc-ground.png
git commit -m "feat(fungus): mycelium surfaces migrate with the field; glow relights; phase 3 recorded"
```

---

## Self-review

- **Spec coverage (§4):** seven combined ids per base, species from the field at mesh time picking a per-(base, species) layer (Tasks 1, 3), migration both ways through a budgeted queue (Tasks 4, 5), diggable and yielding the base (Task 4); spore drop chance waits for phase 4 with the spore item. §6's mycelium emission row (Task 1, with dim 2 and bright 4 rather than a flat 4, so two glow strengths exist). §9 step 3 complete.
- **Deviation:** glow in the id (21 ids rather than 7) so emission stays a pure function of the block. Recorded in Task 5 step 6.
- **Type consistency:** `mesh_section_*` take `la, lb, sps, occ`; `key_of` has six arguments everywhere; `World` has six fields with `shown` fifth; `Myc.migrations(f, n, shown, tick, scan, budget)`.
