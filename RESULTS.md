# RESULTS.md — timing numbers

Machine: Apple Silicon (14 cores), macOS, march 0.3.0 (`forge build --release`
= `--opt 2`), GLFW 3.5.1, 800x600 window, vsync on (display runs ~120 Hz).
All numbers from the runs recorded in the session on 2026-09-03; each is a
single run, not a median.

## Mesh time per chunk (naive per-face mesher, 16x16x256, `F32Buf` output)

| build | chunk | time | allocations during mesh |
|---|---|---|---|
| M2, debug, F32Buf reading `length_f32` before each set (G21) | test terrain, 10 608 verts | 2 341 ms | 74 258 |
| M2, release, same code | " | 1 773 ms | 74 258 |
| M2, release, capacity cached in the constructor | 31 680 verts | **9.4 ms** | 8 (all growth steps, all leaked — G31) |
| M4, release, neighbour-aware, noise terrain | 64 chunks, 455 028 verts total | 7.2 ms/chunk single-threaded | — |

Where the time goes (M2 chunk, 9.4 ms): 65 536 `block_faces` calls, each
doing 1 `C.get` plus up to 6 `get_or_air` neighbour reads; every visible face
is 6 × 7 = 42 `F32Buf.push` calls, each a C call (`native_f32_arr_set`) with
an rc==1 check. No allocation in the loop body. A `blit`-style multi-push
would roughly halve it.

## World generation (8x8 chunks, `List.pmap_n` with 14 workers, headless)

| `MARCH_NUM_SCHEDULERS` | terrain (scalar noise) | terrain (F32x4 noise) | mesh all 64 chunks |
|---|---|---|---|
| 1 | 3–6 ms | 4–6 ms | 459–468 ms |
| 2 | 3 ms | 3 ms | 326 ms |
| 4 (default) | 3 ms | 1–2 ms | 333–372 ms |
| 8 | 2 ms | 3 ms | 334 ms |
| 14 | 2 ms | 3 ms | 324 ms |

Terrain is too cheap to measure at ms resolution (`System.monotonic_time` is
milliseconds). Meshing scales ~1.4x and then stops (GAPS.md G37).

## Frame rate at the 8x8 world

| scenario | fps | notes |
|---|---|---|
| M3 single chunk | 115–119 | vsync-bound |
| M4/M5 64 chunks, 455k vertices, 64 draw calls + outline + HUD | **112–117** | vsync-bound; frame time ~8.6 ms |

The frame loop is not the bottleneck at this world size; the GPU work is 64
`glDrawArrays` of static VBOs. Per-frame March work: input, `Player.update`
(3 axis sweeps), quaternion → view matrix in place, 16-entry matrix product
into the upload buffer, DDA raycast, outline rebuild.

## Per-frame allocation (net live objects, frames 100–200, `march_live_allocs`)

| state | per frame |
|---|---|
| M3 first version, march 0.2.0 | 84 |
| same source, march 0.3.0 | 69 |
| tuples/records/newtype wrappers removed from the frame path | 37 |
| view-projection computed in place into persistent buffers | **1** |

The residual 1/frame is a `NativeFloatArr`-related leak (G31) that only
appears when `write_view` and `mul_into_f32` run back to back.

## Block edit

`CF_AUTOEDIT`: break + remesh of one chunk (plus neighbours when on a
boundary) 17.1 ms; place + remesh 20.4 ms. That is one full chunk remesh
(~7 ms) plus a 64 KB copy-on-write of the edited chunk (G38) plus VBO upload.

## Vectorization evidence

`objdump -d --macho .march/build/release/cube_forge`, symbol
`_CubeForge.Noise.value2_x4`: 24 vector-form instructions in the function
body, e.g.

```
10000c5b0:  fadd.2d  v2, v2, v3
10000c5bc:  fmul.2d  v1, v2, v4[0]
10000cb0c:  fsub.4s  v1, v1, v2
```

next to the scalar lattice/hash part (`fmul d8, d8, d0` …). So the `Simd`
blend does compile to NEON, but the lattice step is scalar by necessity
(GAPS.md G32), and it makes no measurable difference at this problem size.

## Static water (2026-09-03)

| measure | value |
|---|---|
| world with sea level 62, terrain base 50 | 460 698 vertices, 30 246 water |
| mesh all, 4 schedulers, headless | 326 ms wall (5.1 ms/chunk), unchanged by the second buffer |
| frame rate with 64 opaque + 64 translucent draws + tint quad | 114–117 fps (vsync) |
| per-frame live-object delta | 1 (unchanged) |
| break / place / place-water edit + remesh | 12.6 / 12.1 / 12.1 ms |
| swim test | falls in at y≈62.4, sinks at the −3 m/s clamp to 59.0, Space lifts at +2.5 m/s, eye underwater frames 240–400 |

## Spreading water (2026-09-03, 64 `WaterChunk` actors, tick every 10 frames, 1 scheduler thread)

| measure | value |
|---|---|
| spawn 64 actors + queue their `WLoad` | 25–35 ms (loads run asynchronously, ~5 chunk generations each) |
| `Actor.call` round of a tick, 2 dirty chunks, via `pmap_n` | 0.4–2.2 ms |
| apply replies + remesh, 1 chunk changed | 12–21 ms |
| apply replies + remesh, 2 chunks changed (+ edge neighbours) | 37–39 ms (was 58–60 ms before remeshing only the edge directions involved) |
| a source on a plateau, spread across 3 chunks | 40 cells, stable after ~4 ticks |
| drain after breaking the source | back to 0 within ~8 ticks (80 frames) |
| per-frame live-object delta with actors idle | 1 (unchanged) |
| frame rate during flow | 113–118 fps; the 40 ms tick lands in one frame every 10 |

The tick itself is cheap; the cost is meshing whole chunks for a handful of
changed cells. Per-cell mesh patching or a dirty-region mesher is the obvious
next step (todos.md, greedy meshing).

## Greedy sections + shim-side VBO assembly (2026-09-03)

| measure | before (naive whole-chunk) | after (greedy 16x16x16 sections) |
|---|---|---|
| world vertices (64 chunks, water unchanged) | 460 698 | **74 610** (44 364 opaque + 30 246 water) |
| mesh all 64 chunks, 4 schedulers, headless | 307–337 ms | **259 ms** (4.0 ms/chunk); an earlier greedy pass without empty-section/empty-slice early-outs took 1 282 ms |
| block edit + remesh | 12 ms | **2.6–6.6 ms** (1–3 sections + neighbour sections) |
| water tick apply + remesh, 1 chunk changed | 12–21 ms | **5.5–9 ms** (3 sections: the changed one ± 1) |
| water tick apply + remesh, 2 chunks changed | 37–39 ms | **16–19 ms** (6 sections) |
| fps, 64 opaque + 64 water draws | 113–118 | 116–117 (unchanged; vsync-bound) |
| per-frame live-object delta | 1 | 1 |

`blit` (`cf_f32_blit`, the proposed `NativeArray.blit`) is exercised by
`F32Buf.append` and its test, but the upload path no longer needs it (G54).

## `@[no_alloc]` contract sweep (march main 137737f3, 2026-09-03)

| | count |
|---|---|
| attributes inserted by `forge fix --contracts` | 16 |
| frame-path functions annotated `@[no_alloc(warn)]` by hand | 40 |
| verified (kept as strict `@[no_alloc]`) | 15 |
| rejected for a fresh small variant (`Vec3`, `Quat`, `Hit`, `Sweep`) | 11 |
| rejected for amortized buffer growth | 4 (`push` and its callers) |
| rejected for `Nil` being a heap cell | 3 |
| extern wrappers marked `@[no_alloc(assume)]` | 2 |

Build, tests (25), lint and the windowed run are unchanged with the attributes in place.

## Lighting (skylight flood-fill, smooth per-vertex light + AO)

Release build, headless, 8x8 chunks.

| measure | before lighting | after |
|---|---|---|
| world vertices | 74 610 (44 364 opaque + 30 246 water) | **112 038** (81 792 opaque + 30 246 water) |
| opaque vertices | 44 364 | **81 792** (+84%) |
| full-world skylight flood | — | **521 ms** (one-off, at startup) |
| incremental relight per block edit | — | **~50 ms** debug / ~22 ms release |
| day/night cycle | — | 1800 s (30 min) by default; `CF_DAY` overrides |
| sun/moon direction | fixed per-face constants | a moving `N·L`; `CF_SUN=<deg>` pins the angle |
| mesh all 64 chunks | 755 ms | 1 676 ms |

The opaque vertex rise is the accepted cost of smooth lighting: the greedy mask
key now carries the four per-corner light/AO values, so two faces merge only
when their block *and* all four corners agree. As argued in the design doc this
is self-limiting rather than arbitrary — cell A's right-hand corners are
computed from the same voxels as cell B's left-hand corners, so key equality
forces all four corners equal, and merging survives exactly across uniformly-lit
regions. Large stone walls at light 0 and open plains at light 15 still merge
fully; the cost is concentrated at lighting gradients.

Two findings dominated the flood's cost and are written up in GAPS.md:

- **G63** — G21 (a borrowed read before a consuming update turns FBIP into a
  full copy) makes a BFS queue over a `NativeU8Arr` impossible: a 1M-iteration
  read-then-write probe reached a 229 GB peak footprint. Propagation is a
  level-synchronous downward sweep instead, where every pass reads one array and
  writes a different one. `cf_u8_blit` and an early-out before the chunk lookup
  took the lighting tests from 49 s to 21 s.
- **G64** — `World.block_at` per voxel (chunk coords re-derived plus a PVec trie
  walk, 727 ns each) was 179 ms of the relight's 217 ms. Hoisting the chunk out
  of the column loop cut seeding 15x and the full flood from 1 724 ms to 521 ms.

## Dynamic shadows (voxel ray-marched)

The world's opacity is uploaded as a 128x256x128 `GL_R8` 3D texture (4 MB) and
the fragment shader marches an Amanatides-Woo DDA toward the light. One trace
for the sun or the moon (only one is ever above the horizon), one for the
flashlight. `CF_SHADOW_DIST` sets the reach in blocks; `0` disables.

| measure | value |
|---|---|
| occupancy build + upload | **52 ms**, once at startup |
| block edit | one `glTexSubImage3D` texel; no re-flood, no remesh |
| frame rate, 800x600 | 115 fps at reach 0, 24, 64 **and 300** — vsync-bound throughout |

**The cost is below the vsync headroom on this machine even at whole-world
reach**, so the 64-block default is conservative and could be raised.

### Verification

The DDA is GLSL and cannot be unit-tested. The March side is covered by
`forge test` (the occupancy buffer agrees with `Light.is_opaque` at every voxel
of a chunk, and water never occludes). The shader is verified by a scripted
sweep: fix the world and the camera, vary only the sun angle, and measure how
much of the frame the shadow pass darkens.

```
for a in 0 30 55 75 85; do
  for d in 0 64; do
    MARCH_PIN_MAIN=1 CF_SEED=7 CF_SUN=$a CF_SHADOW_DIST=$d \
      CF_FRAMES=8 CF_DUMP_FRAME=5 CF_DUMP=/tmp/q_${a}_$d.bmp ./cube_forge
  done
done
```

| sun angle | pixels shadowed | mean brightness off → on |
|---|---|---|
| 0° (overhead) | **0.0%** | 134.9 → 134.9 |
| 30° | 1.8% | 133.4 → 132.9 |
| 55° | 10.7% | 128.3 → 124.7 |
| 75° | **24.6%** | 103.5 → 99.2 |
| 85° | 4.0% | 44.3 → 43.1 |

The shape of that curve is the correctness argument. **0.0% with the sun
overhead** is the load-bearing number: nothing on open terrain casts a shadow
straight down, so any self-shadowing or ray-origin bug would show up here as
spurious darkening. Coverage then rises as the sun lowers and lengthens
shadows, and falls again at 85° because the light itself is nearly out and
there is little brightness left to remove.

### Two bugs this shook out, both silent

- **Transposed axes.** The light field is indexed `x + 128*(z + 128*y)`, but
  `glTexImage3D` reads its data as `x + width*(y + height*z)` — x, then *y*,
  then z. Uploading the light field's layout samples the world sideways. The
  occupancy buffer has its own `occ_index` for exactly this reason.
- **`GL_R8` is normalized.** A stored byte of `1` samples back as `1/255`, so
  the shader's `> 0.5` occluder test was never true and nothing cast a shadow
  at all. Occluders are stored as `255`.

Both produced plausible-looking output — the first darkened almost everything,
the second nothing — which is why the sun-angle sweep above is the check that
matters rather than a single screenshot.

## Weather

Overcast, fog, precipitation and a weather actor
(`docs/superpowers/specs/2026-09-03-weather-design.md`). Release build,
`MARCH_NUM_SCHEDULERS=1`, seed 7, `CF_SUN=45`, best of four 150-frame runs — the
machine was under variable load, so single runs swing by 3x and only the best
run approximates an uncontended one.

| particles | fps |
|-----------|-----|
| 0         | 270 |
| 4000 (default) | 265 |
| 16000     | 181 |

The pool and its geometry live in the C shim rather than in March. In March the
step alone ran 4000 particles at **5.9 fps** and the geometry build was SIGKILLed
for memory: `NativeArray` writes copy the whole array when it is not uniquely
owned, so both loops are O(n^2). See GAPS.md G68.

Vertex layout went 8 -> 9 floats (packed effect + alpha), +12.5% vertex memory,
about 1 MB at a full 217k-vertex world.

Verification:

- `CF_WEATHER=0` renders **pixel-identical** to the pre-weather baseline. The
  0.002 haze floor is below 8-bit precision at this view distance.
- Storm underground: **0** rain pixels. Storm above ground: 39,466
  (`scratch/rainpixels.py`).
- Two identical runs are pixel-identical from ~frame 150 onward. Earlier frames
  are not reproducible, because the async water actors are still mutating the
  light field that the precipitation kill reads — pre-existing behaviour, not
  something the pool introduces.
- A bare `cmp` on two dumps never matches: the FPS counter differs between any
  two runs. `scratch/cmpframe.py` masks it.

### Fullscreen performance

Measured at 3200x2000 backing (`CF_WIDTH=1600 CF_HEIGHT=1000` on a 2x Retina
display, 6.4M fragments — a stand-in for fullscreen, which GLFW reports as a
1920x1200 scaled mode on this machine). Release, `CF_VSYNC=0`, seed 7,
`CF_SUN=45`.

|                      | before | after | frame time |
|----------------------|--------|-------|------------|
| clear weather        | 164 fps | **330 fps** | 6.09 → 3.03 ms |
| storm (rain + fog)   | 143 fps | **511 fps** | 7.01 → 1.96 ms |
| true fullscreen, clear | — | 602 fps | |
| true fullscreen, storm | — | 705 fps | |

At fullscreen the shadow ray-march was **66% of the frame** — rain was only
0.9 ms of it. Two changes:

1. **Two-level DDA.** A coarse occupancy texture, one texel per 8x8x8 voxels,
   lets the trace cross open air eight blocks per fetch instead of one; it drops
   into the fine grid only for cells that contain something. Per-cell solid
   counts (16 KB) are maintained on block edits so a break can clear a coarse
   texel exactly rather than conservatively.
2. **Two weather early-outs.** `shad` is mixed toward 1.0 by `u_overcast`, so
   above 0.98 the trace was ray-marching 64 blocks and discarding the answer;
   likewise a fragment the fog has washed out by 98%. Both skip work whose
   result is provably invisible. This is why the storm case is now *faster* than
   the clear one.

Neither trades quality. The two-level trace was verified **pixel-identical** to
the single-level one at `CF_SUN` 12, 30 and 60, and after both a block break and
a block place (`CF_AUTOEDIT`), which is what exercises the coarse-cell counts.

New knobs: `CF_WIDTH`, `CF_HEIGHT`, `CF_FULLSCREEN=1`, `CF_VSYNC=0`.
## M6 — vegetation (trees and bushes)

Oak and pine, planted as cubes with alpha-cutout leaves. Placement is
cell-based and depends only on the cell and the seed, never on which chunk is
asking, so two chunks that share a tree agree on it without communicating.

Measured against a build of the immediately preceding commit (`9cd1ffb`), same
seed, same machine:

| measure | before vegetation | after |
|---|---|---|
| world vertices (64 chunks) | 249 252 | **259 608** (+4.2%) |
| mesh all 64 chunks, headless | 1 892 ms | **1 909–1 922 ms** (+1%) |
| frame rate | 59.6 (vsync) | 59.1–59.6 (vsync, unchanged) |
| live objects per frame | 1 | **1** (unchanged) |

**Correction.** This section first reported vegetation as a 5.5x startup
regression, 350 ms to 1.9 s. That was wrong: the 350 ms figure was stale, from
several features earlier, and I compared against it instead of measuring. The
1.9 s was already there before a single tree existed. Vegetation costs about
4% more vertices and 1% more meshing time. The lesson is the ordinary one —
a remembered number is not a baseline.

### The culling rule

A face exists when the neighbour is see-through **and** the pair is not
leaf-against-leaf:

```
face_visible(id, nb) = see_through(nb) && !(is_foliage(id) && is_foliage(nb))
```

Without the second clause every interior face of a canopy is emitted and a
tree becomes a solid brick of quads. Leaves still count as see-through for
everything else, so stone behind a canopy and a trunk seen through the gaps
both still draw. Three tests pin each half of that rule; getting it wrong in
either direction is invisible in a screenshot from the outside.

### What the tests caught

- **Two hotbar slots rendered identically.** Oak and pine logs both pointed at
  texture layer 8, so slots 8 and 9 were the same pixels — found by scanning
  the dumped frame at each of the nine slot centres, not by looking at it.
  Pine now has its own darker bark layer (12).
- **A one-cell seed check proves nothing.** Asserting that cell (3, 4) differs
  between two seeds passes trivially when neither seed puts a tree there. The
  test now compares the layout across 256 cells.

## Block-edit cost: where the 26 ms actually went

Breaking a block took 25–29 ms, over a frame's budget at 60 fps. The obvious
suspect was the greedy remesh, and the obvious suspect was wrong.

`CF_AUTOBREAK=<frame>` mines straight down one block every 20 frames. It always
hits real terrain regardless of where the camera points, so the cost is
comparable between runs — an aim-independent edit benchmark.

| phase | time | share |
|---|---|---|
| skylight relight | **22.3 ms** | 85% |
| greedy section remesh | 3.8 ms | 14% |
| chunk VBO upload | 0.25 ms | 1% |
| occupancy texel update | 0.017 ms | ~0 |

Inside the relight, the propagation sweep was 20 of those 22 ms. Its box was
**126 y-layers tall** for an edit at y=109.

### The fix: bound the box by how far light can actually go

The box reached y=0 whenever full skylight touched the voxel above the edit,
on the reasoning that capping a sky column can darken everything beneath it.
True, but only as far as the column is actually open: skylight descends until
the first opaque block, and below that only lateral spread matters, which the
radius already bounds. `Light.open_bottom` walks down to that block and the box
starts a radius below it.

| | before | after |
|---|---|---|
| box y-range (edit at y=109) | 0..125 (126 layers) | 93..125 (33 layers) |
| sweep | 20.0 ms | **9.9 ms** |
| whole edit | 26.5 ms | **16.0 ms** (median over 15 breaks) |

A 40% cut, and it is a tightening of a bound rather than an approximation: the
four existing equals-a-full-re-flood tests still pass, and a new one caps a
40-block shaft — deeper than the relight radius, so it only passes if the bound
follows the open column all the way down — and still matches a full re-flood.

### The fix that March would not allow

Halving the box did not halve the sweep, because a fixed cost remains: each of
the fourteen propagation passes allocates and copies a fresh 2 MB prefix, about
60 MB of memory traffic per broken block. The passes only ever write one
y-slice, so the right structure is two preallocated buffers, ping-ponged, with
only the written slice re-synced between passes.

That is unimplementable in March today. Reading a `NativeArray` in a loop
inflates its refcount by roughly one per read and never decrements, so after a
single pass the source buffer's refcount is in the hundreds of thousands and it
can never be used as a write destination again. Measured and written up as
GAPS.md **G67**; the numbers there come from the shim's uniqueness guard
reporting the count at three different sweep sizes.

So the remaining ~10 ms stays until either that refcount bug is fixed or the
sweep moves into the C shim. The first is the point of this project; the second
would be routing around the problem silently, which is not.

### Frame cost, uncapped (`CF_VSYNC=0`)

Every earlier figure in this file was taken with vsync on and is therefore
pinned to the display, not a measure of headroom. `CF_VSYNC=0` uncaps it.
800x600, `CF_SEED=7`, sun at 45 degrees:

| configuration | fps | ms/frame |
|---|---|---|
| shadows off | 1315 | 0.76 |
| shadow reach 24 | 1321 | 0.76 |
| shadow reach 64 (default) | 965 | 1.04 |
| shadow reach 300 | 680 | 1.47 |
| map view, whole world drawn top-down | 1361 | 0.73 |

The frame budget at 60 Hz is 16.7 ms and the frame costs **1 ms**, so the
renderer uses about 6% of it. Shadows are the single largest item at 0.28 ms
for the default reach.

**The map view row is the interesting one for culling questions.** It draws all
64 chunks at once and runs as fast as the first-person view, which says the
renderer is not draw-call or geometry bound — 128 draw calls and 244k vertices
are nowhere near a limit. Occlusion culling would remove work that is not
costing anything. What time there is goes to fragment work, so if this ever
does need optimising, drawing chunks front-to-back (so early-Z rejects hidden
fragments before the shadow trace runs) targets the real cost, and frustum
culling — which does not exist yet either — is the cheaper first step. Both
become worthwhile when chunk streaming raises the chunk count; neither is worth
doing at 8x8.

### Stale lighting after an edit (fixed)

Per-vertex light is baked into the mesh, but the edit path only remeshed the
edited cell's section (plus neighbours on a boundary), while `relight_at`
changes light across a far wider box. Everything else kept stale lighting until
something forced a rebuild.

Measured before the fix, counting sections whose light actually moved against
the one section being remeshed:

| edit | sections whose light changed | sections remeshed |
|---|---|---|
| break a surface block | 0 | 1 |
| place a block at y=90 | 2 | 1 |
| place a block high in open air | **5** | 1 |
| 3-block platform | **5** | 1 |

`relight_marked` now returns a dirty `(chunk, section)` mask alongside the
repaired field, and the edit remeshes exactly those. A block edit measures
17-30 ms end to end, still dominated by the relight rather than the extra
sections (~1.2 ms each). Blindly remeshing the whole relight box would have
been ~173 ms, which is why the mask is worth having.

The mask is one slot per (chunk, section) rather than a packed per-chunk
bitmask: a bitmask means reading the accumulator before OR-ing into it, and
reading an array before writing it turns every write into a full copy (G63).
The mark pass is write-only; the 16-bit masks are derived in a separate read.

The invariant is pinned by a test: for the edit that reaches furthest (a block
placed high in open air, darkening the column beneath it), no voxel changes
light in a section the mask failed to flag.

### Soft shadows and the reach fade

`trace` now returns the distance to the occluder rather than a yes/no, so a
shadow whose caster sits near the reach limit fades out instead of ending in a
hard line where the trace gives up.

Soft edges spread four rays over a small cone (`SOFT_SPREAD` 0.035 rad),
rotated per pixel so the samples read as softness rather than four bands.
Because the rays diverge, the penumbra widens with distance from the caster on
its own -- no separate penumbra estimate needed. `CF_SHADOW_SOFT=0` reverts to
one ray.

Measured at 800x600 with a low sun (`CF_SUN=75`), which is the expensive case
because rays travel further before escaping:

| | fps | ms/frame |
|---|---|---|
| hard, 1 ray | 501 | 2.0 |
| soft, 4 rays | 206 | 4.9 |

2.4x the frame cost, and only 2.6% of pixels change by more than 4/255 -- the
gain is real but small, chiefly removing hard-edged shadow bands. It is on by
default because 4.9 ms still fits a 16.7 ms budget comfortably.

**Caveat worth knowing:** these numbers are at an 800x600 framebuffer. On a
retina backing store (1600x1200, which this window sometimes gets) the shadow
cost is per-pixel and would be roughly 4x, putting soft shadows near the 60 Hz
budget. That case has not been measured. `CF_SHADOW_SOFT=0` halves the cost if
it bites.

### Ambient occlusion across chunk boundaries (fixed)

AO needs the diagonal neighbour of a face corner, and that voxel can leave the
chunk on BOTH axes at once, landing in a diagonal chunk the mesher is not given.
`nb_get_d` read it as air, softening occlusion on the four corner columns of
every chunk.

The fix was not to pass nine chunks. `World` now retains the occupancy field it
was already building for the shadow ray-marcher, and the mesher reads occluders
from it. Occupancy is world-space, so the seam is gone by construction and
`nb_get_d` is deleted rather than extended. It also unifies the two notions of
"solid": the mesher's occluder test and the shadow marcher's are now literally
the same bytes.

Cost: the 4 MB occupancy field is kept rather than built and dropped, and a
block edit updates one byte of it alongside the GPU texel.

## Lighting performance: bounding the sweep by the sky

Profiling the flood (debug build) put the cost in one place:

| phase | ms | share |
|---|---|---|
| `seed_all` | 127 | 7% |
| **`spread` (the level sweep)** | **1721** | **90%** |
| 14 x per-level alloc+copy | 56 | 3% |

The sweep ran fourteen passes over all 4,194,304 voxels. Half of them were empty
sky: `sky_floor` for this world is **127 of 256**, and every voxel above it is at
full light with every neighbour also at full light, so it matched level 15 and
called `give6` on six neighbours that could not be improved.

Bounding every sweep pass at one layer above the highest **non-air** voxel:

| | before | after |
|---|---|---|
| skylight flood (release) | 718 ms | **271 ms** (2.6x) |
| occupancy build (release) | 69 ms | **49 ms** |
| `spread` (debug) | 1721 ms | 611 ms |

Non-air rather than opaque is load-bearing. Water is transparent but
attenuating, so a lake sitting above the highest solid block would leave dimmed
voxels above an opaque-only bound and the sweep would skip them. A test covers
exactly that shape.

The bound is computed by scanning each chunk's flat index downward -- that index
runs `lx + 16*(lz + 16*ly)`, so higher indices are higher y and the scan stops at
the chunk's topmost non-air voxel. The first attempt used `World.block_at` per
voxel and made the flood *slower* (1721 -> 3998 ms), which is G64 a second time:
that call re-derives chunk coordinates and walks a PVec trie, and it has no place
in a bulk loop.

### What did not work

Rotating two preallocated buffers instead of allocating a fresh destination per
pass would halve the copy traffic, since `copy_prefix` zeroes a buffer with
`make_u8` and then overwrites every byte of it. A probe that reads a buffer once
per iteration rotates safely. The sweep reads its source **twice** per pass, once
for the copy and once for the scan, and `cf_u8_blit` then refuses the reuse with
`rc=12196853`. That is G67 exactly: reads inflate the refcount permanently, at
roughly one increment per read, so a buffer that has been swept can never again
be uniquely owned. Left as a fresh allocation per pass, with the reasoning
recorded at the call site.

Per-edit relight is unchanged at 13-21 ms, and remains dominated by that same
copy traffic (24 ms of a 43 ms debug relight is the fourteen prefix copies).
G67 is what stands between it and a 2x improvement.

### Shadow tracing: two-level DDA

An 8x8x8 coarse occupancy level lets the shadow trace cross open air eight
blocks per texture fetch instead of one, dropping into the fine grid only for
cells that hold something. Per-cell solid counts (16 KB, against the 4 MB fine
copy the shadow design does not retain) keep the coarse level exact across block
edits — without them a break could only be handled conservatively and the cell
would stay marked solid forever.

It is skipped when `abs(dir.y) < 0.35`. A ray near the horizon spends its whole
length inside terrain, so nearly every coarse cell it crosses is occupied and
the outer walk buys nothing; measured at **-14%** near sunset before the guard.
The light direction is a uniform, so the branch is coherent across the draw.

True fullscreen (1920x1200), soft shadows (the default), clear weather:

| sun (deg from noon) | single-level | two-level + guard |
|---------------------|--------------|-------------------|
| 25                  | 93 fps       | **223 fps**       |
| 45                  | 92 fps       | **181 fps**       |
| 80 (near sunset)    | 126 fps      | 123 fps           |

Pixel-identical to the single-level trace at sun 12/25/80 x hard/soft, and after
a block break and a block place.

**Frame dumps need `CF_NOMOUSE=1`.** Without it the camera yaw depends on where
the window manager placed the window relative to the pointer, so two runs render
different views and any comparison is meaningless. `scratch/cmpframe.py` also
reads the BMP's real dimensions now; it had 800x600 hard-coded while dumps are
1600x1200 on a 2x display, so it had been comparing a mis-sliced sub-region.

## The compiler fix, measured end to end

`GAPS.md` G67 was fixed in march (`lib/tir/borrow.ml`: `extern_borrow_table` had
no `NativeArray` entries, so every array read looked like an ownership
transfer). cube_forge was rebuilt against a toolchain identical to its pinned
one except for that patch — same base commit, same runtime, same stdlib — so the
comparison has one variable.

Block-edit cost, 45 breaks each (3 runs of 15), idle machine:

| | median | mean | min |
|---|---|---|---|
| pinned toolchain, allocating sweep | 12.48 ms | 12.90 | 11.81 |
| **patched toolchain, same March code** | **7.11 ms** | 7.24 | 6.74 |
| patched toolchain + ping-pong sweep | **6.42 ms** | 6.56 | 6.09 |

**The compiler fix alone is a 43% cut with no change to cube_forge at all.** The
per-iteration `inc_rc` on every `NativeArray.get_u8` was not just blocking the
optimization below, it was the larger cost by far — an atomic-capable refcount
bump on every one of the ~500 000 voxel reads a relight performs.

The ping-pong sweep — two preallocated buffers with only the written y-slice
re-synced, replacing fourteen 2 MB allocate-and-copy passes — is worth a further
**10%**. That is much less than I predicted when I wrote it up as the fix G67 was
blocking. The ~60 MB of memory traffic per edit was real, but it was never the
dominant term; the refcount bumps were. Predicting which of two costs dominates
is exactly the thing worth measuring rather than reasoning about, and I had it
backwards.

Combined with the relight-box bound from `b951435`, a block edit went from
**26.5 ms to 6.4 ms**, and is no longer close to a frame's budget at 60 fps.

### A measurement error worth recording

The first A/B I ran gave 12.8 ms for a configuration that actually costs 7.1 ms,
because the compiler's 80-minute differential-oracle run was still going in the
background. Two of the three numbers above were wrong the first time for that
reason alone. Check the machine is idle before believing a timing.

### And a stale-artifact error

This project's test count was reported as 78, then 79, for several sessions. It
is **96**. `forge test` was reusing stale build artifacts and silently running a
subset; a `rm -rf .march/build` shows all 96 on both toolchains. The G60/G62
cross-check of declared-vs-run counts was itself unreliable, because the grep
used to count declarations missed indentation variants. Both counts were wrong
in the same direction, which is why they agreed.

## The inventory's hidden per-frame allocation

The frame loop is meant to hold at one live object per frame, and the inventory
window quietly doubled it to two. What caused it is worth recording because
nothing about the code looks like an allocation.

`Inventory.hud_key` hashed 36 slots through `item_at`/`count_at`, and each of
those destructures the `Inv` variant to reach its array — 73 destructures per
frame. Benchmarked on its own, that is free: 5 live objects per 100 iterations.
In the frame loop it cost one object per frame, because there the `Inv` is
reached through `Scene -> Ui -> Inv` and is shared rather than uniquely owned,
and destructuring a shared variant copies (GAPS.md **G68**).

Matching once and walking the raw cell array restored 1 per frame:

| | live objects per frame |
|---|---|
| before the inventory window | 1 |
| with `hud_key` calling accessors per slot | **2** |
| with the destructure hoisted out | 1 |
| with the window open every frame | 1 |

The measurement that found it was the whole-frame live-object count the game
prints on every run. A microbenchmark of `hud_key` says it is free, the
definition looks like a field read, and the call site looks like a hash. This is
the third time in this project that a per-frame allocation was only visible from
the outside, and the argument for keeping that counter in the default output.

## Latest March main, measured

`origin/main` at `7419c689`, eight commits past the toolchain this project pins,
built with the G67 NativeArray borrow fix applied on top (main does not have
it). Three of those commits target this project's gaps directly: unboxed small
scalar aggregates, stack promotion through non-retaining callees, and
`@[no_alloc(transient)]`.

### What the borrow fix is actually worth

Measuring startup meshing across three toolchains on an idle machine finally
put a number on the G67 fix that I had missed:

| toolchain | mesh all, 64 chunks | per chunk |
|---|---|---|
| `137737f3` + pin-main (what this project pinned) | 1 378 ms | 21.5 ms |
| the same, plus the NativeArray borrow fix | **372 ms** | **5.8 ms** |
| `7419c689` + the same fix | 380 ms | 5.9 ms |

**3.7x on startup meshing**, identical vertex counts. I had only measured the
borrow fix against block-edit cost (43%) and never against meshing, so this
project's headline number for that fix was badly understated. Earlier figures in
this file of 1 900-2 100 ms for meshing were also inflated by compiler builds
running in the background; 1 378 ms is the idle-machine number for the old
toolchain.

### What latest main adds on top: nothing here, and one regression

Meshing, frame rate and the 115 tests are unchanged. Six of this engine's types
now unbox — `Vec3`, `Quat`, `Mat4.Vec4`, `Player.Sweep`, `Weather.Phase` and the
weather pair — but the frame loop's allocation went from **1 live object per
frame to 2**, traced to that two-float pair.

It is a **leak**, not a cost. The live-object delta scales exactly with the
iteration count — 10 000 iterations leak 10 001 objects — and it fires only when
the aggregate is built inside a branch:

| shape, 5 000 iterations | boxed | unboxed |
|---|---|---|
| built with no branch | 3 | 3 |
| built in an `if` | 1 | **5 001** |
| built in a callee | 1 | 1 |

The two arms of an `if` merge through a join slot typed `ptr`, so the unboxed
struct is materialised onto the heap to pass through it — and that box is never
decremented. The boxed build emits seven `march_decrc_local` in the same
function where the unboxed build emits one. GAPS.md **G69** has the IR;
`probes/unboxed_pair/` has both repros.

That is why it is a compiler bug rather than a reason to avoid the feature: the
same program without the branch allocates nothing on either toolchain, so the
heap traffic is not inherent to unboxing. This project stays on `137737f3` +
pin-main + the borrow fix until it is fixed upstream.
## Biomes — phase 1, the field

A per-column climate field (`docs/superpowers/specs/2026-09-04-biomes-design.md`):
temperature from a seeded octave minus an elevation lapse, moisture from
Chebyshev distance to water, eight biomes behind an alpine gate and a beach
gate. Recomputed whole on `tick_period()`; nothing incremental except the
heightmap, which `edit_block` updates in O(1) and `chop` rescans in a box.

- **Tick cost: ~18 ms**, every ten frames, release build (`CF_AUTOFLOW` prints
  it). 24 full passes for the distance sweep dominate; the temperature base is
  cached at build so it is not 16,384 noise evaluations per tick. A frontier
  sweep would be O(frontier) but the obvious queue is the G70 trap.
- **Canal, end to end.** `CF_AUTOCANAL=10 CF_BIOME_RATE=20000`: the player's
  column goes `grassland moist 0.25` -> frame 890 `forest moist 0.917`, and the
  biome map changes along the pond and its reach (83,883 px of an 800x600
  frame). With the original infinite-source water this test flooded half the
  map — see "Finite water" below. `docs/biome-map.png`,
  `docs/biome-map-canal.png`.
- `CF_AUTOFLOW` cannot drive this test: it places through `interact`, which
  needs a raycast hit, and with `CF_NOMOUSE` the crosshair sits on the horizon.
  `CF_AUTOCANAL` lays nine water blocks beside the player through `edit_block`.
- Two G68 corollaries found and recorded as **GAPS G70**: arrays wrapped in a
  variant cell copy on every write (10.8 GB for 20 BFS passes), and a discarded
  `set_*` result silently drops the write.

## Finite water — conserved volume above sea level

`docs/superpowers/specs/2026-09-04-finite-water-design.md`. A placed water block
used to be id 4, an infinite source `process` never re-evaluated, so placing
water was placing a spring: the biome canal test flooded the eastern half of the
map. Now ids 5-11 are units that a push rule moves (fall first, then equalise
across a difference of two); id 4 stays the infinite source — the sea, and
already the spring; `source_neighbour` keeps pits filling to lake level.

- Nine placed blocks are now a pond beside the player, `docs/finite-water-pond.png`.
- Conservation is the test: a closed basin holds exactly 7 units across 40
  ticks; seven units on a plain settle as seven cells of level 1 and the queue
  empties; two chunks exchanging spills sum to 7 throughout; a stale mirror
  that would overfill a cell keeps 7 and bounces the rest — kept plus in-flight
  is exact.
- The `kind 2` spill was already forwarded end to end by `apply_reply`; the
  change is two functions, `process` and `accept_spill`, plus a pending-reply
  slot on `Sim` for the bounce.
- A single source no longer exceeds the 250-cell tick budget — its rings settle
  in two ticks — so that test now uses a full 256-cell layer.

## Biomes — phase 2, weather by biome

The weather actor is unchanged: one global storm. The biome under the camera
decides how much of it falls and whether it is snow. `Precip.snow_mix` keys on
the column's eased temperature (cold below 0.33, blended over 0.08) rather than
`Noise.snow_line`, and `Biome.precip_scale` multiplies the particle count:
desert 0.15, tundra 0.5, beach 0.8, grassland and alpine 0.9, forest and taiga
1.0, wetland 1.3, capped at `CF_PRECIP`. Under `CF_WEATHER=100` the player's
grassland column runs 3,600 particles; after the canal turns it to forest, 4,000.

## Biomes — phase 3, the surface migrates

Two new blocks, gravel (20) and clay (21); dirt already existed as id 2. Each
biome has a palette block (tundra snow, taiga dirt, grassland and forest grass,
desert and beach sand, wetland clay, alpine gravel; granite stays on the slope
rule), and each tick up to `CF_BIOME_BUDGET` (default 32) dry columns whose
surface is a palette block that disagrees with their biome are rewritten. A
stateless cursor (`tick * 2048 mod 16384`) sweeps the world in eight ticks.

The first version applied each pick through `edit_block`: **~7 ms per column**
(8 columns 60 ms, 64 columns 470 ms), nearly all of it a relight and a
per-column section remesh. A palette swap never changes opacity, so the relight
is wasted; the batch now writes every pick with `set_block`, collects the
touched sections into one marks array and remeshes once: **8 columns 7 ms, 64
columns 15 ms**, pixel-identical output. `docs/biome-retexture.png` is a fresh
world after 900 frames — snow on the cold lowland, clay ringing the lake,
gravel above the treeline; `docs/biome-retexture-canal.png` shows clay forming
around the pond.

## Biomes — phase 4, vegetation grows and decays

Trees stay a pure function of the seed; ongoing change goes through `Veg`. A
column grows a tree only at its 8-block cell's canonical trunk column and only
when the cell's density draw is under its biome's density (forest 0.45 as at
generation, taiga 0.25 pines, wetland 0.15), so growth is spaced and a column
is a candidate at most once. A tree is felled where its biome holds no trees.
`CF_VEG_BUDGET` (default 1) trees per tick.

- **Felling cost 0.9-2.6 s per tree at first.** `World.decay_leaves` searches a
  (2r+1)^3 box for a log around every leaf in a (2r+1)^3 box — up to five
  million reads per tree. A tree of known trunk and species is ~730 predicate
  checks against `Trees.block_of_tree`, so `Veg.fell` removes exactly its own
  blocks: **~40 ms per tree** including relight and remesh.
- **Stale sections.** The first batched version relit with `relight_at` and
  remeshed only the tree's box. A relight changes baked light in sections the
  box never touches, and a section whose light moved but whose mesh was not
  rebuilt renders stale — whole-frame differences between remesh strategies.
  `relight_marked` reports which sections it changed; those join the geometry
  marks, and the result is pixel-identical to a superset 3x3-chunk remesh.
- **Grassland is "bushes only", so its trees are felled.** Generation plants on
  every flat grass column at 0.45; the biome table keeps trees only in forest,
  taiga and wetland. On a fresh world the temperate dry plain loses its trees
  over the first minutes while forest near the water keeps them —
  `docs/biome-vegetation.png`. Bushes are not implemented yet.

## Springs — generated, balanced against evaporation

`docs/superpowers/specs/2026-09-04-springs-and-flow-design.md`. A spring is a
source above sea level, generated one per 8-block cell that draws under
`CF_SPRING_DENSITY` per mille and whose column has slope >= 2 at height >= 72.
It gives at most `CF_SPRING_RATE` units a tick; thin sky-exposed water
evaporates one draw in `CF_EVAP` per tick.

**The bug that hid the whole feature:** a spring's receivers were never
processed. `set_cell` marks a written cell's neighbours, which covers the
receiver only when the giver is itself written -- and a source never is. Every
spring filled its two air neighbours to 7 and stopped. `give` now marks the
receiver explicitly. Found with a probe test on the chunk holding the spring at
(116,90,74), which is kept as the regression: the brook must still be fed after
sixty ticks, reach beyond the spring's neighbours, and stay under forty units.

Balance, release build, seed 7, frame ~890, one tick every ten frames:

| rate | evap | density (per mille) | springs | active cells | water actors | apply + remesh |
|------|------|---------------------|---------|--------------|--------------|----------------|
| 1 | 16 | 150 | 8 | 112 | 2.0 ms | 27.6 ms (23 sections) |
| 1 | 32 | 150 | 9 | 218 | 3.5 ms | 44.9 ms (30) |
| 2 | 16 | 150 | 9 | 205 | 4.1 ms | 36.3 ms (26) |
| 1 | 8  | 150 | 7 | 56  | 1.3 ms | 23.1 ms (22) |
| 1 | 16 | **100** | **5** | **87** | **1.4 ms** | **23.7 ms (16)** |
| 1 | 16 | 60  | 2 | 42  | 0.7 ms | 10.8 ms (7) |

Defaults landed: **rate 1, evap 16, density 100** -- five springs, ~9-cell
brooks in the probe, actors well under the 2 ms target. The remesh is the honest
cost of water that keeps moving: about three sections per brook per tick,
~1.5 ms each. `CF_SPRING_DENSITY` is the lever for it. `docs/spring-brook.png`.
