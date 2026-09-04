# Lighting — flood-fill skylight, smooth per-vertex light + AO, day/night, flashlight — design

Today lighting is four constants (`Mesher.shade_top/bottom/x/z`, 1.0 / 0.55 /
0.75 / 0.85) baked per-vertex and multiplied into albedo in the fragment
shader. There is no sky light, no ambient occlusion and no time of day: a cave
interior is exactly as bright as an open field.

Scope: **baked skylight** (one channel, flood-filled), **smooth per-vertex
light with ambient occlusion**, a **day/night cycle**, and a **camera-mounted
flashlight**. No emissive blocks and no coloured light — the flashlight is
dynamic and lives entirely in the fragment shader, so it needs no voxel data.

Accepted cost: smooth per-vertex lighting restricts greedy merging to
uniformly-lit regions (see §3). This is deliberate.

## 1. Storage

- New `CubeForge.Light` mirrors `Chunk`: one byte per voxel, value 0..15, the
  same flat index, reusing `Chunk.index` so the refinements are not duplicated.
- Light is **bundled into the chunk**: `type Chunk = Chunk(NativeU8Arr,
  NativeU8Arr)` — blocks, then light. The alternative (a parallel
  `Array.PVec` on `World`) turns the mesher's `(c, n, s, e, w)` into ten
  arguments threaded through `mesh_section_opaque`, `face_key`, `nb_get` and
  `build_sections`. Bundling leaves every one of those signatures unchanged.
- **Risk, retired first (step 1 of §6):** this makes `Chunk` a two-field
  variant, and GAPS.md G28-G30 lists aggregate RC for tuples/records as
  unimplemented. `Meshes(o, wb)` and `ChunkMesh(os, ws)` are already two-field
  and work, so the evidence is good — but the widening lands as its own commit
  with the existing suite green before any lighting logic sits on top of it.
- `opacity(id)` replaces the binary solid/air test: air `0`, water `2`,
  everything else opaque. It drives both seeding and propagation, and gives
  underwater its gloom for free.

## 2. Propagation

**World-space BFS**, not per-chunk. Index `x + 128 * (z + 128 * y)` over the
whole 128x256x128 world; the queue is a ring buffer over `NativeIntArr`. This
departs from the per-chunk-actor pattern `water.march` established, and does so
deliberately: light crossing chunk boundaries through edge mirrors converges by
iteration and is fiddly to get right, where one flat index space is obviously
correct. The full-world flood is a one-off at generation time; incremental
relights touch a small region.

- **Seed.** Walk each `(x, z)` column from `y = 255` down carrying a value that
  starts at 15 and drops by `opacity(id)` per voxel, stopping at the first
  opaque block. Air holds a shaft at full 15 all the way down; water dims it.
- **Spread.** The standard 6-neighbour BFS: pop a voxel at light `L`, and for
  each neighbour `n`, when `L - 1 - opacity(n) > n.light`, write it and
  enqueue.

**Incremental relight** is the add/remove pair:

- **Placing** a block runs removal first. Pop `(index, oldLight)`; for each
  neighbour, when `n.light` is non-zero and *less* than `oldLight` it was lit
  by us — record it, zero it, enqueue. When `n.light >= oldLight` it is lit by
  something that survives, so push it into the *addition* queue as a re-seed.
  When removal drains, the addition pass runs from those re-seeds.
- **Breaking** a block is addition only: seed from the brightest neighbour,
  plus a re-run of the vertical column seed from that point down, in case a sky
  shaft just opened.
- The mirror case: placing a block that *caps* a sky shaft darkens the whole
  column beneath it, handled by running removal down the column.

BFS inner loops carry `@[no_alloc]`, consistent with the frame path.

**Remesh set:** a 1024-byte dirty-flag array, one bit per `(chunk, section)`,
set as propagation writes; when the queues drain, exactly the flagged sections
are remeshed.

## 3. Mesher — per-vertex light and AO

For a face in direction `d` everything is computed in the *outside*
half-space. Let `n` be the voxel across the face — non-opaque by construction,
or the face would not exist. Each of the four corners looks at three more
voxels beside `n`: two edge-adjacent (`s1`, `s2`) and the diagonal (`c`).

- **AO**, the standard formula: `if s1 && s2 then 0 else 3 - (s1 + s2 + c)`,
  counting opaque as 1.
- **Light**: the mean over `{n, s1, s2, c}` of the non-opaque ones — always at
  least `n`, so never empty.
- **Vertex shade** = `(light / 15) * (0.55 + 0.15 * ao)`. The face directional
  constant is *not* folded in here; it moves to the shader with `face`.

**Quad flip.** Required, not cosmetic: without it corner shading tears along
the fixed diagonal. When one diagonal's AO sum exceeds the other's, emit the
alternate triangulation `(v1,v2,v3 / v1,v3,v0)` in place of today's
`(v0,v1,v2 / v0,v2,v3)`.

**Greedy key.** `face_key` packs `id | c0<<8 | c1<<14 | c2<<20 | c3<<26` — six
bits per corner (4 light, 2 AO), 32 bits total, comfortably inside an `Int`,
and the mask is already `NativeIntArr` so nothing widens. `emit_rect` decodes
the four corners straight back out of the key, so nothing new threads through
the mask machinery.

- **Trap:** `face_key` must compute corners in **mask `(u,v)` order**, not
  face-local order, or `emit_rect` maps them to the wrong quad corners. The
  `d`-dependent axis mapping in `emit_rect` is the reference.
- **Merging stays sound, and is self-limiting.** Cell A's right-hand corners
  are computed from the same voxels as cell B's left-hand corners, so
  `A.c1 == B.c0` always holds; combined with key equality that forces all four
  corners equal across any merged run. Merging therefore survives only across
  uniformly-lit regions — the accepted cost, with no correctness hazard. Large
  stone walls at light 0 and open plains at light 15 still merge fully.

Water keeps its per-cell path and takes per-vertex light with AO pinned at 3.

## 4. Shader and shim

Vertex layout 7 -> 8 floats: `pos.xyz, uv, layer, shade, face`. This touches
the stride in `cf_shim.c`, the `vert` push in `mesher.march`, and the `/ 7` in
`ChunkMesh.opaque_vertices` / `water_vertices`.

The vertex shader gains `const vec3 NORMALS[6]` and `const float
FACE_SHADE[6]` indexed by `a_face`, and outputs world position and normal.

```
float moon  = clamp((0.25 - u_sun) / 0.25, 0.0, 1.0);
vec3  sky   = vec3(u_sun) + MOON_TINT * (MOON_LEVEL * moon);
vec3  baked = v_shade * face_shade * sky;
vec3  L     = u_eye - v_world;
float spot  = smoothstep(u_cos_outer, u_cos_inner, dot(normalize(-L), u_dir));
float flash = u_flash * spot * max(dot(v_normal, normalize(L)), 0.0)
            / (1.0 + 0.05 * dot(L, L));
o_color     = vec4(t.rgb * (baked + flash), t.a);
```

One new shim entry point, `cf_gfx_set_light(sun, eye, dir, flash)`, called per
frame. The cone half-angles `u_cos_inner` / `u_cos_outer` are shader constants,
not uniforms — the flashlight's beam shape never varies at runtime, only its
position, aim and on/off state.

Night is lit by a cool moonlight term (`MOON_TINT` 0.60/0.72/1.00 at
`MOON_LEVEL` 0.13 of full daylight) that ramps in as the sun drops below 0.25,
replacing the flat 0.06 ambient floor of the original design. Because it is
multiplied by `v_shade` exactly as sunlight is, moonlight reaches only
sky-exposed surfaces: a sealed cave stays black at night and still needs the
flashlight. The clear colour carries the same ramp so the sky reads as dark navy
rather than black.

A full day is 1800 seconds (30 minutes) by default; `CF_DAY` overrides it, which
is what makes night reachable in a screenshot or a headless run.
`cf_win_time()` drives the clock; `F` toggles the flashlight.

**The sun has no direction.** `u_sun` is a scalar: it scales every face by the
same factor, and the directional component stays the fixed `FACE_SHADE[6]` table
inherited from the pre-lighting mesher. Sunlight therefore changes brightness
through the day but never angle, and because the skylight flood is seeded
straight down each column, overhang shadows never move or lengthen either.

## 5. Verification — `test/light_test.march`

The test that carries the most weight: **incremental relight equals a
from-scratch flood.** A `Check.all` property over random edits on a random
world, comparing the entire light field after incremental repair against a full
re-flood. Every removal-pass bug — and the removal pass is where they live —
fails this test.

Alongside it:

- An open column reads 15 top to bottom; a placed block reads 0 below it.
- Light decreases monotonically with BFS distance from a seed.
- Water attenuates 2 per voxel.
- The eight `(s1, s2, c)` combinations map to the expected AO 0..3, including
  the `s1 && s2 -> 0` override.
- A corner configuration with unequal diagonals selects the alternate
  triangulation.

## 6. Order of work

1. **Widen `Chunk` to two fields**, suite green — retires the G28-G30 risk
   before anything is built on it. No visual change.
2. `Light` module, full-world flood, tests.
3. Incremental relight, and the equals-full-flood property test.
4. **Layout 7 -> 8, `face` replaces the shade constant.** A pure refactor; the
   screen should look identical.
5. Per-vertex light, AO, quad flip, greedy key. Measure the vertex-count
   regression here and record it in `RESULTS.md` beside the existing 6x figure.
6. Day/night and the flashlight.

Steps 1 and 4 are both no-visual-change checkpoints, which makes the two
riskiest structural edits verifiable on their own.
